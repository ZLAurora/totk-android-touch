"""Shared helpers of the benchmark tools: repository paths, the baseline write guard and store (per frequency signature),
frequency limits and signatures, and the comparison metrics."""
import datetime, hashlib, json, math, re, sys
from pathlib import Path

SCHEMA = 'totk-android-benchmark/1'
INDEX_SCHEMA = 'totk-android-baselines/1'
REPO = Path(__file__).resolve().parents[2]
BENCH_DOCS = REPO / 'docs/benchmarks'


# ---- baselines: immutable files, one per frequency signature -------------------------------------------------------
# docs/benchmarks/baseline-1.0.0.{md,json}: the 1.0.0 release baseline (no GPU cap recorded).
# docs/benchmarks/baselines/<device-slug>/<signature-short>.json: written only by `ledger.py add` for a signature that
# has no baseline yet. docs/benchmarks/baselines/index.json maps signature -> baseline file (append-only).
# docs/benchmarks/BASELINES.sha256 pins every baseline file (paths relative to docs/benchmarks).

def pins_path(docs=None):
    return (docs or BENCH_DOCS) / 'BASELINES.sha256'


def index_path(docs=None):
    return (docs or BENCH_DOCS) / 'baselines/index.json'


def is_baseline_path(path, docs=None):
    docs = (docs or BENCH_DOCS).resolve()
    p = Path(path).resolve()
    return (p.parent == docs and p.name.startswith('baseline-')) or (docs / 'baselines') in p.parents


def guard_output(path):
    """Baselines are immutable and only `ledger.py add` creates new ones: refuse to write any baseline path."""
    if is_baseline_path(path):
        sys.exit(f'refusing to write {path}: baseline files are immutable; new baselines are created only by '
                 '`ledger.py add` for a frequency signature that has none yet')


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_pins(docs=None):
    pins, p = {}, pins_path(docs)
    if p.exists():
        for line in p.read_text().splitlines():
            if line.strip():
                digest, name = line.split(None, 1)
                pins[name.lstrip('*')] = digest
    return pins


def pin(rel_name, docs=None):
    """Pin a NEW baseline file (path relative to docs/benchmarks); an already pinned file is never re-pinned."""
    docs = docs or BENCH_DOCS
    if rel_name in read_pins(docs):
        sys.exit(f'{rel_name} is already pinned; baselines are never re-pinned')
    with open(pins_path(docs), 'a') as f:
        f.write(f'{sha256_file(docs / rel_name)}  {rel_name}\n')


def load_index(docs=None):
    p = index_path(docs)
    if not p.exists():
        return {'schema': INDEX_SCHEMA, 'baselines': []}
    idx = json.loads(p.read_text(encoding='utf-8'))
    if idx.get('schema') != INDEX_SCHEMA:
        sys.exit(f'{p}: schema {idx.get("schema")!r}, expected {INDEX_SCHEMA!r}')
    return idx


def save_index(idx, docs=None):
    p = index_path(docs)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(idx, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def index_entry(sig, docs=None):
    return next((b for b in load_index(docs)['baselines'] if b['signature'] == sig), None)


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', (text or 'device').lower()).strip('-') or 'device'


def write_baseline(ddoc, label, run_id, commit, docs=None):
    """Record a one-device result as the NEW baseline of its frequency signature: baselines/<slug>/<short>.json, pinned
    and added to the index. Refuses when the signature already has a baseline or the file exists. Returns the entry."""
    docs = docs or BENCH_DOCS
    sig = device_signature(ddoc, label)
    if sig is None:
        sys.exit(f'{label}: no frequency signature (no frequency limits recorded): cannot become a baseline')
    if index_entry(sig['sha256'], docs):
        sys.exit(f'signature {sig["short"]} already has a baseline; baselines are never replaced')
    rel_name = f'baselines/{slug(sig["fields"]["model"])}/{sig["short"]}.json'
    path = docs / rel_name
    if path.exists() or rel_name in read_pins(docs):
        sys.exit(f'refusing to overwrite the existing baseline {rel_name}')
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = dict(ddoc, baseline={'signature': sig['sha256'], 'run_id': run_id, 'commit': commit, 'device': label})
    with open(path, 'x', encoding='utf-8') as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write('\n')
    pin(rel_name, docs)
    entry = {'signature': sig['sha256'], 'short': sig['short'], 'device': label, **sig['fields'],
             'cpu_hw_max_khz': sig['cpu_hw_max_khz'], 'gpu_hw_max_hz': sig['gpu_hw_max_hz'],
             'caps': caps_text(sig['fields']['cpu_caps_khz'], sig['cpu_hw_max_khz'], sig['fields']['gpu_cap_hz'],
                               sig['gpu_hw_max_hz']),
             'file': rel_name, 'run_id': run_id,
             'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}
    idx = load_index(docs)
    idx['baselines'].append(entry)
    save_index(idx, docs)
    return entry


def load_result(path):
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)
    if doc.get('schema') != SCHEMA:
        sys.exit(f'{path}: schema {doc.get("schema")!r}, expected {SCHEMA!r}')
    return doc


# ---- frequency limits and signatures -------------------------------------------------------------------------------
# A `bench_input.sh freqs` snapshot is one line of tokens: cpu<N>=<related cpus,>:<scaling_min>:<scaling_max>:<cpuinfo_max>
# (kHz, one per cpufreq policy), gpu_maxclk=<Hz> (kgsl max_gpuclk), gpu_freqs=<Hz,...> (available frequencies, kgsl
# order: power level 0 first), gpu_devfreq_max=<Hz>, gpu_min_pwrlevel / gpu_max_pwrlevel / gpu_thermal_pwrlevel.

def _int(v):
    return int(v) if v is not None and str(v).lstrip('-').isdigit() else None


def parse_freq_tokens(tokens):
    """{'cpu': [per policy], 'gpu': {...}} from a snapshot's key=value tokens (a dict or a list of 'k=v' strings)."""
    if not isinstance(tokens, dict):
        tokens = dict(t.split('=', 1) for t in tokens if '=' in t)
    cpu = []
    for k, v in tokens.items():
        if (m := re.fullmatch(r'cpu(\d+)', k)) and v.count(':') == 3:
            cpus, smin, smax, hw = v.split(':')
            cpu.append({'policy': int(m[1]), 'related_cpus': [int(c) for c in cpus.split(',') if c.isdigit()],
                        'scaling_min_khz': _int(smin), 'scaling_max_khz': _int(smax), 'cpuinfo_max_khz': _int(hw)})
    cpu.sort(key=lambda c: c['policy'])
    avail = [int(x) for x in tokens.get('gpu_freqs', '').split(',') if x.isdigit()]
    gpu = {'max_gpuclk_hz': _int(tokens.get('gpu_maxclk')), 'devfreq_max_hz': _int(tokens.get('gpu_devfreq_max')),
           'available_hz': avail, 'min_pwrlevel': _int(tokens.get('gpu_min_pwrlevel')),
           'max_pwrlevel': _int(tokens.get('gpu_max_pwrlevel')),
           'thermal_pwrlevel': _int(tokens.get('gpu_thermal_pwrlevel'))}
    gpu['hw_max_hz'] = max(avail) if avail else None
    caps = [gpu['max_gpuclk_hz'], gpu['devfreq_max_hz']]
    mpl = gpu['max_pwrlevel']
    if mpl is not None and 0 <= mpl < len(avail) and avail == sorted(avail, reverse=True):
        caps.append(avail[mpl])   # kgsl max_pwrlevel caps the clock at that level's frequency
    caps = [c for c in caps if c]
    gpu['cap_hz'] = min(caps) if caps else None
    return {'cpu': cpu, 'gpu': gpu}


def caps_of_limits(fl):
    """The effective caps of a freq_limits snapshot: ((scaling_max kHz per cluster, in policy order), GPU cap Hz)."""
    if not fl:
        return None
    return tuple(c['scaling_max_khz'] for c in fl['cpu']), (fl.get('gpu') or {}).get('cap_hz')


def settings_sha256(env):
    text = ''.join(f'{k}={env[k]}\n' for k in sorted(env) if k != 'TOTK_DATA_DIR')
    return hashlib.sha256(text.encode()).hexdigest()


SIGNATURE_FIELDS = ('model', 'soc', 'settings_env_sha256', 'cpu_caps_khz', 'gpu_cap_hz')


def signature_of_fields(fields):
    return hashlib.sha256(json.dumps({k: fields[k] for k in SIGNATURE_FIELDS}, sort_keys=True,
                                     separators=(',', ':')).encode()).hexdigest()


def signature_of(model, soc, env, cpu_caps, gpu_cap):
    """(sha256, fields) of a frequency signature: device model + SoC, the settings_env hash, the per-cluster effective
    CPU caps (kHz, policy order) and the effective GPU cap (Hz, None when unknown)."""
    fields = {'model': model, 'soc': soc, 'settings_env_sha256': settings_sha256(env or {}),
              'cpu_caps_khz': list(cpu_caps), 'gpu_cap_hz': gpu_cap}
    return signature_of_fields(fields), fields


def device_signature(doc, label, docs=None, path=None):
    """The frequency signature of one device of a result: computed from its freq_limits, or for a registered baseline
    file without them (baseline-1.0.0.json) taken from the baseline index. None when unknown.
    Returns {'sha256', 'short', 'fields', 'cpu_hw_max_khz', 'gpu_hw_max_hz'}."""
    dev = doc['devices'][label]
    fl = dev.get('freq_limits')
    if fl:
        cpu, gpu = caps_of_limits(fl)
        sha, fields = signature_of(dev.get('name'), dev.get('soc'), dev.get('settings_env'), cpu, gpu)
        return {'sha256': sha, 'short': sha[:12], 'fields': fields,
                'cpu_hw_max_khz': [c['cpuinfo_max_khz'] for c in fl['cpu']],
                'gpu_hw_max_hz': (fl.get('gpu') or {}).get('hw_max_hz')}
    if path is not None:
        docs = docs or BENCH_DOCS
        try:
            rel_name = Path(path).resolve().relative_to(docs.resolve()).as_posix()
        except ValueError:
            return None
        for b in load_index(docs)['baselines']:
            if b['file'] == rel_name and b['device'] == label:
                return index_sig(b)
    return None


def index_sig(b):
    """A baseline index entry as a device_signature() dict."""
    return {'sha256': b['signature'], 'short': b['short'], 'fields': {k: b[k] for k in SIGNATURE_FIELDS},
            'cpu_hw_max_khz': b['cpu_hw_max_khz'], 'gpu_hw_max_hz': b['gpu_hw_max_hz']}


def signature_relation(ref, new):
    """'match', 'reference-only' (the reference has no GPU cap; model, settings and CPU caps match), 'mismatch', or
    'unknown' (either side has no signature)."""
    if ref is None or new is None:
        return 'unknown'
    if ref['sha256'] == new['sha256']:
        return 'match'
    a, b = ref['fields'], new['fields']
    if a['gpu_cap_hz'] is None and all(a[k] == b[k] for k in ('model', 'settings_env_sha256', 'cpu_caps_khz')):
        return 'reference-only'
    return 'mismatch'


def signature_diff(ref, new):
    a, b = ref['fields'], new['fields']
    return ', '.join(f'{k}: {a[k]} -> {b[k]}' for k in a if a[k] != b[k])


def caps_text(cpu_caps, cpu_hw, gpu_cap, gpu_hw):
    """'CPU 2.02/2.80/3.19 GHz (max), GPU 680 MHz (max)' with the hardware maxima where a cap is below them."""
    ghz = lambda vs: '/'.join(f'{v / 1e6:.2f}' if v else '?' for v in vs)
    if cpu_caps:
        at_max = cpu_hw and len(cpu_hw) == len(cpu_caps) and all(c == h for c, h in zip(cpu_caps, cpu_hw))
        cpu = f'CPU {ghz(cpu_caps)} GHz ' + ('(max)' if at_max else f'(hw {ghz(cpu_hw)})' if cpu_hw else '(hw ?)')
    else:
        cpu = 'CPU caps unknown'
    if gpu_cap:
        gpu = f'GPU {gpu_cap / 1e6:.0f} MHz ' + ('(max)' if gpu_cap == gpu_hw else f'(hw {gpu_hw / 1e6:.0f})'
                                                 if gpu_hw else '(hw ?)')
    else:
        gpu = 'GPU cap unknown'
    return f'{cpu}, {gpu}'


def sig_caps_text(sig):
    return caps_text(sig['fields']['cpu_caps_khz'], sig['cpu_hw_max_khz'], sig['fields']['gpu_cap_hz'],
                     sig['gpu_hw_max_hz'])


# Comparison metrics: (summary key, label, direction (+1 higher is better, -1 lower is better), kind, tolerance).
# kind 'abs_rel': same while |delta| <= max(abs, rel * |baseline|); kind 'rate': the value is a total over the windows,
# compared per window, same while |delta per window| <= max(abs, rel * baseline per window); kind 'abs': |delta| <= abs.
# A difference beyond the tolerance is 'better' or 'worse' by the direction. Modern summaries compare steady FPS and
# dip time separately; mean FPS is informational. Only wholly legacy comparisons retain the historical primary set.
DIP_TIME_FRACTION_TOLERANCE = 0.05   # Five percentage points, absolute; the boundary is still 'same'.
DIP_CONFOUNDED = 'verdict confounded by dips'
PACING_KEYS = ('fps_steady', 'dip_windows', 'dip_time_fraction', 'frames_near33')
METRICS = [
    ('fps_mean',        'fps mean',        +1, 'abs_rel', (0.5, 0.02)),
    ('throughput_fps',  'throughput fps',  +1, 'abs_rel', (0.5, 0.02)),
    ('fps_stddev',      'fps stddev',      -1, 'abs_rel', (0.5, 0.10)),
    ('fps_steady',      'fps steady',      +1, 'abs_rel', (0.5, 0.02)),
    ('dip_time_fraction', 'dip time fraction', -1, 'abs', (DIP_TIME_FRACTION_TOLERANCE, 0)),
    ('fps_min',         'fps min',         +1, 'abs_rel', (1.0, 0.05)),
    ('p50_ms_median',   'p50 median ms',   -1, 'abs_rel', (0.3, 0.02)),
    ('p50_ms_max',      'p50 max ms',      -1, 'abs_rel', (0.3, 0.02)),
    ('p95_ms_max',      'p95 max ms',      -1, 'abs_rel', (1.0, 0.05)),
    ('p99_ms_max',      'p99 max ms',      -1, 'abs_rel', (1.0, 0.05)),
    ('p99_p50_ratio_max', 'p99/p50 max',   -1, 'abs_rel', (0.1, 0.05)),
    ('max_ms',          'worst frame ms',  -1, 'abs_rel', (5.0, 0.15)),
    ('gpu_ms_mean',     'GPU ms mean',     -1, 'abs_rel', (0.5, 0.03)),
    ('long_ge45_total', 'long >=45 ms',    -1, 'rate',    (0.5, 0.25)),
    ('over33_total',    '>33 ms frames',   -1, 'rate',    (0.5, 0.25)),
    ('over50_total',    '>50 ms frames',   -1, 'rate',    (0.5, 0.25)),
    ('cpu_max_c',       'CPU max C',       -1, 'abs',     (3.0, 0)),
    ('gpu_max_c',       'GPU max C',       -1, 'abs',     (3.0, 0)),
]
PRIMARY = ('fps_steady', 'dip_time_fraction', 'p50_ms_median', 'p95_ms_max', 'gpu_ms_mean', 'over50_total')
LEGACY_PRIMARY = ('fps_mean', 'p50_ms_median', 'p95_ms_max', 'gpu_ms_mean', 'over50_total')
UNCAPPED_PRIMARY = ('throughput_fps', 'p50_ms_max', 'p95_ms_max', 'p99_ms_max', 'max_ms', 'over33_total', 'over50_total')


def is_uncapped(summary):
    return summary.get('uncapped') is True or summary.get('speed_mode') == 'uncapped'


def metric_verdict(key, base, new):
    """(delta, verdict) for one summary metric of two runs' summaries."""
    _, _, direction, kind, (tol_abs, tol_rel) = next(m for m in METRICS if m[0] == key)
    b, n = base.get(key), new.get(key)
    if b is None or n is None:
        return None, 'n/a'
    delta = n - b
    if kind == 'rate':
        bw, nw = base.get('windows') or 0, new.get('windows') or 0
        if not bw or not nw:
            return delta, 'n/a'
        b, n = b / bw, n / nw
    d = n - b
    tol = tol_abs if kind == 'abs' else max(tol_abs, tol_rel * abs(b))
    if abs(d) <= tol or math.isclose(abs(d), tol, rel_tol=1e-12, abs_tol=1e-12):
        return delta, 'same'
    return delta, 'better' if d * direction > 0 else 'worse'


def primary_metrics(base, new):
    """Use the historical mean-FPS rule only when neither summary contains pacing information."""
    if is_uncapped(base) or is_uncapped(new):
        return UNCAPPED_PRIMARY
    return PRIMARY if any(k in s for s in (base, new) for k in PACING_KEYS) else LEGACY_PRIMARY


def pacing_verdict(base, new):
    """A pacing veto of the overall verdict, or None; a missing steady value never falls back to mean FPS."""
    if is_uncapped(base) or is_uncapped(new):
        if not (is_uncapped(base) and is_uncapped(new)):
            return 'inconclusive'
        return None if base.get('throughput_fps') is not None and new.get('throughput_fps') is not None else 'inconclusive'
    if primary_metrics(base, new) == LEGACY_PRIMARY:
        return None
    dip = metric_verdict('dip_time_fraction', base, new)[1]
    if dip in ('better', 'worse'):
        return DIP_CONFOUNDED
    if dip == 'n/a' or base.get('fps_steady') is None or new.get('fps_steady') is None:
        return 'inconclusive'
    return None


def pacing_note(base, new):
    """Explain the pacing policy, including legacy or all-dip summaries, without hiding per-metric deltas."""
    if is_uncapped(base) or is_uncapped(new):
        if not (is_uncapped(base) and is_uncapped(new)):
            return 'measurement modes differ (capped vs uncapped); no speed conclusion.'
        return ('uncapped: dip_metrics_valid=false; time-weighted throughput and frame-time tails are primary. '
                'Dip classification is disabled; mean-window fps and spread measures are informational.')
    if primary_metrics(base, new) == LEGACY_PRIMARY:
        return 'pacing info unavailable (legacy summaries); fps mean retains the historical verdict rule.'
    note = ('fps mean is informational; steady fps and dip time fraction are compared separately '
            f'(dip tolerance {DIP_TIME_FRACTION_TOLERANCE:g} absolute).')
    if base.get('dip_time_fraction') is None or new.get('dip_time_fraction') is None:
        note += ' pacing info unavailable for at least one summary; no speed conclusion.'
    if base.get('fps_steady') is None or new.get('fps_steady') is None:
        note += ' steady fps unavailable for at least one summary (no comparable steady windows); no speed conclusion.'
    return note


def scene_verdicts(base, new):
    """{metric: (delta, verdict)} and the scene's overall verdict for two runs' summaries."""
    per = {m[0]: metric_verdict(m[0], base, new) for m in METRICS}
    prim = [per[k][1] for k in primary_metrics(base, new)]
    overall = 'worse' if 'worse' in prim else 'better' if 'better' in prim else 'same'
    if all(v == 'n/a' for v in prim):
        overall = 'n/a'
    return per, pacing_verdict(base, new) or overall


def caps_of(run):
    """The set of cpufreq limits (scaling_max < cpuinfo_max) seen during a run's pans."""
    return sorted({(c['scaling_max'], c['cpuinfo_max']) for t in run.get('thermal', []) for c in t['cpufreq_caps_khz']})


def match_device(base_doc, label, dev):
    """The baseline device label matching a device: the same label, or the same model / name."""
    devices = base_doc.get('devices', {})
    if label in devices:
        return label
    names = {(dev.get('model') or dev.get('name') or '').lower()} - {''}
    for k, v in devices.items():
        if names & {(v.get('model') or '').lower(), (v.get('name') or '').lower()}:
            return k
    return None


def comparability(base_doc, base_label, new_doc, new_label):
    """Warnings that make two runs of one device not comparable: settings.env and cpufreq caps differences, and runs
    whose frequency limits changed during a pan (freq_stable false)."""
    warns = []
    be = base_doc['devices'].get(base_label, {}).get('settings_env')
    ne = new_doc['devices'].get(new_label, {}).get('settings_env')
    if be is not None and ne is not None and be != ne:
        diff = sorted(k for k in set(be) | set(ne) if be.get(k) != ne.get(k))
        warns.append(f'settings_env differs ({", ".join(f"{k}: {be.get(k)} -> {ne.get(k)}" for k in diff)})')
    bruns = {r['scene']: r for r in base_doc['runs'] if r['device'] == base_label}
    for r in new_doc['runs']:
        if r['device'] != new_label or r['scene'] not in bruns:
            continue
        bc, nc = caps_of(bruns[r['scene']]), caps_of(r)
        if bc != nc:
            fmt = lambda caps: ', '.join(f'{s}/{c}' for s, c in caps) or 'none'
            warns.append(f'{r["scene"]}: cpufreq caps differ (scaling_max/cpuinfo_max kHz {fmt(bc)} -> {fmt(nc)})')
        for side, run in (('reference', bruns[r['scene']]), ('new', r)):
            if run.get('freq_stable') is False:
                warns.append(f'{r["scene"]}: frequency limits changed during the {side} run (freq_stable false)')
    return warns


def fmt(v, nd=2):
    if v is None:
        return '-'
    if isinstance(v, float):
        return f'{v:.{nd}f}'
    return str(v)


RELATION_TEXT = {
    'match': 'frequency signatures match',
    'reference-only': 'REFERENCE ONLY: the reference has no recorded GPU cap (model, settings and CPU caps match); '
                      'verdicts are indicative, not a regression check',
    'mismatch': 'WARNING: NOT COMPARABLE: frequency signatures differ',
    'unknown': 'WARNING: NOT COMPARABLE: frequency signature unknown (no frequency limits recorded)',
}


def compare_docs(base_doc, new_doc, out=sys.stdout, base_path=None, new_path=None):
    """Print the per device x scene comparison of two result documents, with the frequency-signature relation of each
    device pair (see signature_relation); returns {(new_label, scene): (overall, relation)}."""
    verdicts = {}
    for new_label, dev in new_doc['devices'].items():
        base_label = match_device(base_doc, new_label, dev)
        if base_label is None:
            print(f'\n## {new_label}: not in the reference run, nothing to compare', file=out)
            continue
        warns = comparability(base_doc, base_label, new_doc, new_label)
        bsig = device_signature(base_doc, base_label, path=base_path)
        nsig = device_signature(new_doc, new_label, path=new_path)
        relation = signature_relation(bsig, nsig)
        line = RELATION_TEXT[relation]
        if relation == 'mismatch':
            line += f' ({signature_diff(bsig, nsig)})'
        print(f'\n# {new_label}: {line}', file=out)
        for side, sig in (('reference', bsig), ('new', nsig)):
            print(f'  {side}: ' + (f'signature {sig["short"]}, {sig_caps_text(sig)}' if sig else 'no signature'),
                  file=out)
        bruns = {r['scene']: r for r in base_doc['runs'] if r['device'] == base_label}
        for run in (r for r in new_doc['runs'] if r['device'] == new_label):
            title = f'{new_label}' + (f' (reference {base_label})' if base_label != new_label else '') + f' / {run["scene"]}'
            if run['scene'] not in bruns:
                print(f'\n## {title}: scene not in the reference run', file=out)
                continue
            b, n = bruns[run['scene']]['summary'], run['summary']
            per, overall = scene_verdicts(b, n)
            verdicts[(new_label, run['scene'])] = (overall, relation)
            head = (overall if overall == DIP_CONFOUNDED else overall.upper())
            head += {'match': '', 'reference-only': ' (reference only)'}.get(relation, ' (NOT COMPARABLE)')
            print(f'\n## {title}: {head}  (windows {b.get("windows")} -> {n.get("windows")})', file=out)
            print(f'  {pacing_note(b, n)}', file=out)
            print(f'| {"metric":<16} | {"baseline":>9} | {"new":>9} | {"delta":>8} | verdict |', file=out)
            print(f'|{"-" * 18}|{"-" * 11}|{"-" * 11}|{"-" * 10}|---------|', file=out)
            for key, label, *_ in METRICS:
                delta, v = per[key]
                d = '-' if delta is None else (f'{delta:+.2f}' if isinstance(delta, float) else f'{delta:+d}')
                print(f'| {label:<16} | {fmt(b.get(key)):>9} | {fmt(n.get(key)):>9} | {d:>8} | {v:<7} |', file=out)
        for w in warns:
            print(f'WARNING {new_label}: not comparable: {w}', file=out)
    return verdicts

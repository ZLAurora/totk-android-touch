#!/usr/bin/env python3
"""analyze.py <out-dir> [--json FILE] [--name NAME] [--date YYYY-MM-DD] [--commit SHA] [--meta FILE] [--arms [--guard-frames N]]

Per device x scene, the runtime's own 300-frame windows recorded while panning: from <out>/pans.txt, the windows whose
end frame is in [F0+600, F1] (started after the stick was pushed, finished before it was released), read from
<out>/logs/<device>.log, plus the max CPU / GPU zone temperature, cooling / GPU throttle state and cpufreq caps from
<out>/samples-<device>.txt over [T0, T1], and the frequency limits at the start and end of each pan from
<out>/freqs.txt. Prints the windows and a results table; --json writes the result in the totk-android-benchmark/1
schema (the shape of docs/benchmarks/baseline-1.0.0.json, plus the frequency fields below).

Frequency limits: each pan gets freq_limits {start, end} (per cpufreq policy related_cpus, scaling_min/scaling_max/
cpuinfo_max kHz; GPU max_gpuclk, available frequencies, devfreq max, power levels, effective cap_hz and hw_max_hz).
A run is freq_stable false when any effective cap (CPU scaling_max per cluster, GPU cap) differs between those
snapshots, between the sampler's records over the pans, or from the device's first scene: such a run is not usable
for comparison and never becomes a baseline. Each device gets freq_limits (its first pan's start) and freq_signature
(benchlib.device_signature: model + SoC, settings_env hash, CPU caps, GPU cap).

--arms (in-session A/B, tools/bench/ab.sh): the log's live-switch lines ([totk-ctl] gen=N frame=F applied ... / mark
...) split the session into generations; generation N covers the frames after F up to the next generation's frame.
Each window goes to the generation that covered its whole span, from more than --guard-frames (default 2) frames
after that generation started (the backend may still record a frame of the previous one); windows straddling a change
are dropped. A generation belongs to the arm named by its "mark ... arm=<X>" (ab.sh writes "ab arm=X block=n");
windows of generations without one are dropped too. Per arm: pooled summary metrics over its windows, and per block
(one generation) the same metrics and pan direction (right on odd blocks, left on even blocks for old runs).
The same-arm spread remains informational; abtest.py compares same-direction blocks within each ABBA quartet.

Inputs per device label (as written by collect.sh / sampler.py / pan.sh): logs/<d>.log, logs/<d>-dmesg.txt,
samples-<d>.txt, settings-<d>.env, facts-<d>.txt, totk-env-<d>.txt (only when a developer totk.env exists), freqs.txt.
pans.txt lines with the same device and scene (a trailing repeat number is ignored: hateno2 = hateno) form one run.
--meta: a JSON object deep-merged over the generated document (curated device names / profiles, build fields);
its "caveats" are appended to the generated ones. Never writes a baseline file (benchlib.guard_output)."""
import argparse, collections, datetime, json, os, re, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import SCHEMA, caps_of_limits, device_signature, guard_output, parse_freq_tokens, sig_caps_text  # noqa: E402
from sampler import GPU_SNAPSHOT_FILE, gpu_residency_delta, parse_gpu_snapshots, power_of_tokens  # noqa: E402

METHOD_SUMMARY = ('Default profile per device (no totk.env). Save loaded from the in-game Load list, ~55 s settle, then the '
                  'right stick held at {stick} (right, then left) via sendevent on the controller node. Only the runtime\'s own '
                  '300-frame windows that started after the stick was pushed and ended before it was released are counted.')
METHOD_SOURCES = {
    'fps': '[nvn-vk] frame N: X fps over the last 300',
    'frame_time': '计时 至帧 N： 帧时间 p50 / p95 / 最大 (runtime timing line)',
    'gpu_ms': '[nvn-vk] frame N, per frame: ... GPU X ms',
    'long_frames': '[长帧] lines (logged only when >20 ms and 40% above the recent 16-frame average: lower bound)',
    'over33_over50': '[nvn-vk] stutter line counts per window',
    'thermal': 'thermal zones, cooling devices, GPU thermal_pwrlevel and cpufreq limits sampled every 3 s',
    'freq_limits': 'cpufreq policies and the kgsl/devfreq GPU limits read at the start and end of each pan, plus the '
                   'sampler\'s scaling_max_freq / max_gpuclk records',
}
KNOWN_SCENES = {
    'hateno': {'name': 'Hateno Village', 'save_slot': 'slot_00', 'note': 'outdoors in the village'},
    'lookout': {'name': 'Lookout Landing', 'save_slot': 'slot_01',
                'note': 'arrival point is inside the shelter next to Monte (indoor)'},
    'rotsumamu': {'name': 'Rotsumamu Shrine', 'save_slot': 'slot_04', 'note': 'outdoors at the shrine'},
}
FAULT_RE = re.compile(r'page ?fault|smmu.*fault|context fault|gpu fault|adreno.*fault', re.I)
CTL_RE = re.compile(r'\[totk-ctl\] gen=(\d+) frame=(\d+) (applied|mark)(?: (.*))?$')
GUARD_FRAMES = 2
# Block metrics compared by abtest.py (rates per window).
AB_METRICS = ('fps_steady', 'dip_time_fraction', 'throughput_fps', 'fps_mean', 'fps_min', 'fps_stddev',
              'p50_ms_median', 'p50_ms_max', 'p95_ms_max', 'p99_ms_max', 'p99_p50_ratio_max', 'max_ms',
              'gpu_ms_mean', 'over33_total', 'over50_total', 'long_ge45_total')
PRESENT_BIN_LABELS = ('<12.5', '12.5-20.9', '20.9-29', '29-37.5', '37.5-45.8', '>=45.8')


def parse_log(path):
    timing, gpu, fps, stut, longf, info = {}, {}, {}, {}, [], {}
    frame_ms, near33, draw_samples = {}, {}, []
    last_window = None
    recording, present_stats, present_bins, traces = {}, {}, {}, {}
    frame_p99 = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if m := re.search(r'计时 至帧 (\d+)： 帧时间 p50 ([\d.]+) / p95 ([\d.]+) / 最大 ([\d.]+) ms', line):
            timing[int(m[1])] = tuple(float(x) for x in m.groups()[1:])
        if m := re.search(r'帧间隔 至帧 (\d+)：>16\.7ms \d+ / (\d+) 帧，p99 ([\d.]+) ms', line):
            frame_p99[int(m[1])] = (float(m[3]), int(m[2]))
        if m := re.search(r'\[nvn-vk\] frame (\d+), per frame: recording ([\d.]+) ms, waiting for the GPU ([\d.]+) ms, GPU ([\d.]+) ms', line):
            gpu[int(m[1])] = float(m[4])
            recording[int(m[1])] = float(m[2])
        if m := re.search(r'\[nvn-vk\] frame (\d+): ([\d.]+) fps over the last 300', line):
            fps[int(m[1])] = float(m[2])
            last_window = int(m[1])
            if d := re.search(r'over the last 300, (\d+) draws\b', line):
                draw_samples.append((last_window, int(d[1])))
        if m := re.search(r'\[nvn-vk\] frame (\d+), stutter: longest frame ([\d.]+) ms, (\d+) frames over (\d+) ms, (\d+) over (\d+) ms', line):
            stut[int(m[1])] = (int(m[3]), int(m[4]), int(m[5]), int(m[6]))
        if m := re.search(r'\[长帧\] 帧 (\d+)（([\d.]+) s）整帧 ([\d.]+) ms', line):
            longf.append((int(m[1]), float(m[3])))
            frame_ms[int(m[1])] = float(m[3])
        if m := re.search(r'\[frame-tl late\] frame=(\d+).*?\bgame_iv=([\d.]+)', line):
            frame_ms.setdefault(int(m[1]), float(m[2]))
        if last_window is not None and 'window present-fence cadence,' in line:
            if m := re.search(r'p50/p95/p99=([\d.]+)/([\d.]+)/([\d.]+)ms', line):
                present_stats[last_window] = {'present_p95_ms': float(m[2]), 'present_p99_ms': float(m[3])}
        if last_window is not None and (m := re.search(
                r'window present-fence interval bins ms \[<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8\]: all=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)', line)):
            bins = [int(x) for x in m.groups()]
            present_bins[last_window] = bins
            if sum(bins) == 300:
                near33[last_window] = bins[3]
        if 'game_version' not in info and (m := re.search(r'^\[totk\] game version (\S+)', line)):
            info['game_version'] = m[1]
        if 'turnip' not in info and (m := re.search(r'driver sha256=([0-9a-f]{64})', line)):
            info['turnip'] = m[1]
        if 'settings' not in info and (m := re.search(r'^\[totk-app\] settings\.env \([^)]*\): (.*)$', line)):
            info['settings'] = dict(kv.split('=', 1) for kv in m[1].split() if '=' in kv)
        if 'device lost' in line.lower():
            info['device_lost'] = info.get('device_lost', 0) + 1
        if '[present-trace]' in line:
            fields = dict(re.findall(r'(\w+)=(\S+)', line))
            keys = ('id', 'generation', 'source_frame', 'display_ns')
            if all(fields.get(k, '').isdigit() for k in keys):
                row = {k: int(fields[k]) for k in keys}
                key = row['generation'], row['id']
                # Identical duplicate delivery is harmless; conflicting identities cannot establish an interval.
                traces[key] = row if key not in traces or traces[key] == row else None
    info['frame_ms'], info['near33'] = frame_ms, near33
    info['draw_rates'] = cumulative_draw_rates(draw_samples)
    info.update(recording=recording, present_stats=present_stats, present_bins=present_bins, frame_p99=frame_p99,
                present_intervals=present_trace_intervals(traces))
    return timing, gpu, fps, stut, longf, info



def present_trace_intervals(traces):
    """Join immutable transaction IDs, never callback order or missing/reversed/generation-boundary timestamps."""
    intervals = []
    for (generation, identifier), row in sorted(traces.items()):
        previous = traces.get((generation, identifier - 1))
        if (row and previous and previous['display_ns'] > 0 and row['display_ns'] > previous['display_ns']
                and row['source_frame'] >= previous['source_frame']):
            intervals.append({'frames': [previous['source_frame'], row['source_frame']],
                              'interval_ns': row['display_ns'] - previous['display_ns']})
    return intervals


def presentation_of(start, end, info):
    return {**info.get('present_stats', {}).get(end, {}),
            'recording_ms': info.get('recording', {}).get(end),
            'present_interval_bins': info.get('present_bins', {}).get(end),
            'present_intervals_ns': [p['interval_ns'] for p in info.get('present_intervals', [])
                                     if start - 1 <= p['frames'][0] and start <= p['frames'][1] <= end]}


def presentation_summary(windows):
    bins = [w['present_interval_bins'] for w in windows if w.get('present_interval_bins') is not None]
    counts = [sum(b[i] for b in bins) for i in range(6)] if bins else None
    total = sum(counts) if counts is not None else 0
    intervals = [ns for w in windows for ns in w.get('present_intervals_ns', [])]
    hits = sum(16_700_000 < ns < 30_000_000 for ns in intervals)
    recording = [w['recording_ms'] for w in windows if w.get('recording_ms') is not None]
    return {'recording_ms_mean': round(sum(recording) / len(recording), 3) if recording else None,
            'recording_windows': len(recording),
            'present_p95_ms_max': mx(w.get('present_p95_ms') for w in windows),
            'present_p99_ms_max': mx(w.get('present_p99_ms') for w in windows),
            'present_quantile_windows': sum(w.get('present_p95_ms') is not None for w in windows),
            'present_interval_bin_labels_ms': list(PRESENT_BIN_LABELS),
            'present_interval_bin_counts': counts,
            'present_interval_bin_shares': [c / total for c in counts] if total else None,
            'present_interval_pairs': total, 'present_histogram_windows': len(bins),
            'present_judder_proxy_fraction': counts[2] / total if total else None,
            'present_judder_exact_fraction': hits / len(intervals) if intervals else None,
            'present_judder_exact_hits': hits, 'present_judder_exact_pairs': len(intervals)}


def print_presentation(summary, prefix='  '):
    fmt = lambda value: '-' if value is None else f'{value:.3f}'
    print(f'{prefix}presentation: recording mean {fmt(summary["recording_ms_mean"])} ms; '
          f'fence p95/p99 window maxima {fmt(summary["present_p95_ms_max"])}/'
          f'{fmt(summary["present_p99_ms_max"])} ms ({summary["present_quantile_windows"]} windows)')
    print(f'{prefix}frame tails p50/p95/p99 window maxima '
          f'{fmt(summary.get("p50_ms_max"))}/{fmt(summary.get("p95_ms_max"))}/{fmt(summary.get("p99_ms_max"))} ms; '
          f'p99/p50 max {fmt(summary.get("p99_p50_ratio_max"))}; fps stddev {fmt(summary.get("fps_stddev"))}; '
          f'>33/>50 ms {summary.get("over33_total")}/{summary.get("over50_total")}')
    if summary.get('uncapped'):
        print(f'{prefix}uncapped throughput {fmt(summary.get("throughput_fps"))} fps; dip_metrics_valid=false')
    shares = summary['present_interval_bin_shares']
    print(f'{prefix}present bin shares: ' + (', '.join(f'{label} ms {share * 100:.2f}%'
          for label, share in zip(PRESENT_BIN_LABELS, shares)) if shares is not None else 'unavailable') +
          f'; judder proxy (20.9-29 ms) {fmt(summary["present_judder_proxy_fraction"])} '
          f'({summary["present_interval_pairs"]} observed pairs)')
    print(f'{prefix}exact judder (>16.7 & <30 ms): {fmt(summary["present_judder_exact_fraction"])} '
          f'({summary["present_judder_exact_hits"]}/{summary["present_judder_exact_pairs"]} valid traced pairs; '
          'framework fences, not hardware scanout)')


def cumulative_draw_rates(samples):
    """Consecutive cumulative samples only: no invented zero origin, reset or missing-window interpolation."""
    rates, previous = {}, None
    for frame, draws in samples:
        if previous:
            pf, pd = previous
            if frame - pf == 300 and draws >= pd:
                rates[frame] = (draws - pd) / (frame - pf)
        previous = frame, draws
    return rates

def pacing_of(end, fps, target, info, uncapped=False):
    """Classify >=20% half-rate frames as a dip; otherwise use fps < 5/6 target when frame coverage is incomplete.

    Complete raw durations take precedence over the fence interval histogram. Sparse long-frame/late traces are
    lower bounds, never proof that unlogged frames were fast. A 30-target run's normal 33 ms frames are not dips.
    """
    times = [info.get('frame_ms', {})[f] for f in range(end - 299, end + 1) if f in info.get('frame_ms', {})]
    count = sum(29 <= ms < 37.5 for ms in times)
    full = len(times) == 300
    if full:
        source = 'frame-durations'
    elif end in info.get('near33', {}):
        count, source = info['near33'][end], 'present-fence-histogram'
        full = True
    else:
        source = 'sparse-frame-durations-lower-bound' if times else 'unavailable'
    dip = None
    classification = 'unknown'
    if target and not uncapped:
        if target == 60 and (full or count >= 60):
            dip, classification = count >= 60, source
        else:
            dip = fps < target * 5 / 6 if fps is not None else None
            classification = 'window-fps' if dip is not None else 'unknown'
    return {'pacing_state': 'uncapped' if uncapped else 'dip' if dip else 'steady' if dip is False else 'unknown',
            'pacing_source': 'uncapped' if uncapped else classification, 'target_fps': target,
            'uncapped': uncapped, 'dip_metrics_valid': False if uncapped else dip is not None,
            'duration_s': 300 / fps if fps and fps > 0 else None,
            'frames_near33': count if full or times else None, 'frames_near33_source': source}


def pan_direction(block):
    """ab.sh alternates the first pan direction, starting right at block 1."""
    return ('right' if block % 2 else 'left') if isinstance(block, int) and block > 0 else None


def parse_ctl(path):
    """The live-switch generations of a log, in order: [{'gen', 'frame', 'applied': [tokens], 'marks': [texts],
    'arm', 'block', 'direction'}]. Missing direction marks in old runs are inferred from the block index."""
    gens = {}
    with open(path, encoding='utf-8', errors='replace') as fh:
        lines = [line.rstrip('\n') for line in fh if '[totk-ctl]' in line]
    for line in lines:
        if not (m := CTL_RE.search(line)):
            continue
        g = gens.setdefault(int(m[1]), {'gen': int(m[1]), 'frame': int(m[2]), 'applied': [], 'marks': [], 'arm': None,
                                        'block': None, 'direction': None})
        if m[3] == 'applied':
            g['applied'].append(m[4] or '')
        else:
            g['marks'].append(m[4] or '')
            kv = dict(t.split('=', 1) for t in (m[4] or '').split() if '=' in t)
            if kv.get('arm'):
                g['arm'] = kv['arm']
                g['block'] = int(kv['block']) if kv.get('block', '').isdigit() else None
                g['direction'] = kv.get('direction', pan_direction(g['block']))
    return [gens[k] for k in sorted(gens)]


def window_generation(start, end, gens, guard=GUARD_FRAMES):
    """The generation (its dict) that covered frames [start, end] whole, with `guard` frames of margin after it began;
    None when a change falls inside (or within the guard before) the window. Frames up to the first generation's frame
    belong to generation 0 (no live change yet), returned as {'gen': 0, 'frame': 0, 'arm': None, ...}."""
    current = {'gen': 0, 'frame': 0, 'applied': [], 'marks': [], 'arm': None, 'block': None}
    for g in gens:
        if g['frame'] < start - guard:
            current = g
        elif g['frame'] < end:
            return None   # a change applied before the window's last frame, after its start (or inside the guard)
        else:
            break
    return current


def summarize(windows, thermal=()):
    vals = lambda k: [w[k] for w in windows if w.get(k) is not None]
    p50s = sorted(vals('p50_ms'))
    valid_fps = [fps for fps in vals('fps') if fps > 0]
    throughput = round(300 * len(valid_fps) / sum(300 / fps for fps in valid_fps), 2) if valid_fps else None
    uncapped = bool(windows) and all(w.get('uncapped') for w in windows)
    classified = [w for w in windows if w.get('pacing_state') in ('steady', 'dip')]
    pacing = {}
    if classified:
        steady = [w['fps'] for w in classified if w['pacing_state'] == 'steady' and w.get('fps')]
        durations = [w for w in classified if w.get('duration_s')]
        total_time = sum(w['duration_s'] for w in durations)
        pacing = {
            'fps_steady': round(sum(steady) / len(steady), 2) if steady else None,
            'dip_metrics_valid': True,
            'dip_windows': sum(w['pacing_state'] == 'dip' for w in classified),
            'pacing_windows': len(classified),
            'dip_time_fraction': round(sum(w['duration_s'] for w in durations if w['pacing_state'] == 'dip') /
                                       total_time, 6) if total_time else None,
            'measured_window_time_s': total_time,
            'fps_time_mean': round(300 * len(durations) / total_time, 2) if total_time else None,
            'frames_near33': sum(w['frames_near33'] for w in classified if w.get('frames_near33') is not None)
                             if any(w.get('frames_near33') is not None for w in classified) else None,
            'frames_near33_windows': sum(w.get('frames_near33') is not None for w in classified),
            'frames_near33_lower_bound': any(w.get('frames_near33_source') != 'frame-durations' and
                                           w.get('frames_near33_source') != 'present-fence-histogram' for w in classified),
        }
    if uncapped:
        pacing = {'uncapped': True, 'speed_mode': 'uncapped', 'dip_metrics_valid': False,
                  'fps_steady': None, 'dip_windows': None, 'dip_time_fraction': None}
    return {
        'windows': len(windows),
        **pacing,
        **presentation_summary(windows),
        'fps_min': min(vals('fps'), default=None), 'fps_max': max(vals('fps'), default=None),
        'fps_mean': round(sum(vals('fps')) / len(vals('fps')), 2) if vals('fps') else None,
        'throughput_fps': throughput, 'throughput_windows': len(valid_fps),
        'fps_stddev': round(statistics.pstdev(vals('fps')), 3) if vals('fps') else None,
        'draws_per_frame_mean': round(sum(vals('draws_per_frame')) / len(vals('draws_per_frame')), 2)
                                if vals('draws_per_frame') else None,
        'draws_windows': len(vals('draws_per_frame')),
        'p50_ms_median': p50s[len(p50s) // 2] if p50s else None,
        'p95_ms_max': max(vals('p95_ms'), default=None), 'max_ms': max(vals('max_ms'), default=None),
        'p50_ms_max': max(vals('p50_ms'), default=None), 'p99_ms_max': max(vals('p99_ms'), default=None),
        'p99_p50_ratio_max': max(vals('p99_p50_ratio'), default=None),
        'p99_windows': len(vals('p99_ms')),
        'gpu_ms_mean': round(sum(vals('gpu_ms')) / len(vals('gpu_ms')), 2) if vals('gpu_ms') else None,
        'long_ge45_total': sum(vals('long_ge45')), 'long_ge25_total': sum(vals('long_ge25')),
        'over33_total': sum(vals('over33')) if vals('over33') else None,
        'stutter_windows': len(vals('over33')),
        'over50_total': sum(vals('over50')),
        'cpu_max_c': mx(t['cpu_max_c'] for t in thermal), 'gpu_max_c': mx(t['gpu_max_c'] for t in thermal),
    }


def block_metric(summary, key):
    """A summary metric as compared: totals of rate metrics per window."""
    v = summary.get(key)
    if v is None:
        return None
    if key in ('over33_total', 'over50_total', 'long_ge45_total'):
        return round(v / summary['windows'], 3) if summary.get('windows') else None
    return v


def arm_analysis(windows, gens, guard=GUARD_FRAMES):
    """Assigns each window (dict with 'frames' [start, end]) to its generation and arm (adds 'gen' / 'arm' to it) and
    returns the run's "ab" record: per arm the pooled summary, per block (generation) the summary, and the spread of
    each AB_METRICS value over the arm's blocks (None with fewer than two blocks)."""
    counts = {'assigned': 0, 'straddling': 0, 'unlabelled': 0}
    blocks = collections.OrderedDict()
    for w in windows:
        g = window_generation(w['frames'][0], w['frames'][1], gens, guard)
        w['gen'] = g['gen'] if g else None
        w['arm'] = g['arm'] if g else None
        if g is None:
            counts['straddling'] += 1
        elif g['arm'] is None:
            counts['unlabelled'] += 1
        else:
            counts['assigned'] += 1
            blocks.setdefault(g['gen'], (g, []))[1].append(w)
    arms = collections.OrderedDict()
    for gen, (g, ws) in blocks.items():
        s = summarize(ws)
        arms.setdefault(g['arm'], {'blocks': [], 'windows': []})
        arms[g['arm']]['blocks'].append({'gen': gen, 'block': g['block'], 'frame': g['frame'],
                                         'direction': g.get('direction', pan_direction(g['block'])), 'summary': s,
                                         'metrics': {k: block_metric(s, k) for k in AB_METRICS}})
        arms[g['arm']]['windows'] += ws
    for a in arms.values():
        ws = a.pop('windows')
        a['summary'] = summarize(ws)
        a['memory'] = worst_memory([w.get('memory', {}) for w in ws])
        a['spread'] = {}
        for k in AB_METRICS:
            vs = [b['metrics'][k] for b in a['blocks'] if b['metrics'][k] is not None]
            a['spread'][k] = round(max(vs) - min(vs), 3) if len(vs) >= 2 else None
    return {'guard_frames': guard, 'windows': counts,
            'generations': [{**{k: g[k] for k in ('gen', 'frame', 'applied', 'marks', 'arm', 'block')},
                             'direction': g.get('direction', pan_direction(g['block']))} for g in gens],
            'arms': arms}


def print_arms(ab):
    c = ab['windows']
    print(f'  arms: {c["assigned"]} windows assigned, {c["straddling"]} dropped (straddle a live change, guard '
          f'{ab["guard_frames"]} frames), {c["unlabelled"]} dropped (no arm mark)')
    fmt = lambda v: '-' if v is None else f'{v:g}'
    for arm, a in ab['arms'].items():
        s = a['summary']
        print(f'  arm {arm}: {len(a["blocks"])} blocks, {s["windows"]} windows: fps {fmt(s["fps_mean"])}, p50 '
              f'{fmt(s["p50_ms_median"])} ms, p95 max {fmt(s["p95_ms_max"])} ms, GPU {fmt(s["gpu_ms_mean"])} ms, '
              f'>50 ms {s["over50_total"]} ({fmt(block_metric(s, "over50_total"))}/window)')
        if s.get('pacing_windows'):
            print(f'    steady fps {fmt(s.get("fps_steady"))}; dip windows {s["dip_windows"]}/{s["pacing_windows"]}; '
                  f'dip time fraction {fmt(s["dip_time_fraction"])}; ~33 ms frames {fmt(s["frames_near33"])}; '
                  f'time-weighted mean fps {fmt(s["fps_time_mean"])}')
        print_presentation(s, '    ')
        for b in a['blocks']:
            m = b['metrics']
            print(f'    block {b["block"] if b["block"] is not None else "?"} (gen {b["gen"]}, from frame {b["frame"]}, '
                  f'direction {b["direction"] or "?"}): '
                  f'{b["summary"]["windows"]} windows, fps {fmt(m["fps_mean"])}, p95 {fmt(m["p95_ms_max"])} ms, '
                  f'GPU {fmt(m["gpu_ms_mean"])} ms, >50/window {fmt(m["over50_total"])}')
        if a.get('memory'):
            print('    memory / pressure (worst): ' + ', '.join(f'{k} {v}' for k, v in a['memory'].items()))
        sp = a['spread']
        print(f'    same-arm spread: fps {fmt(sp["fps_mean"])}, p95 {fmt(sp["p95_ms_max"])} ms, GPU {fmt(sp["gpu_ms_mean"])} ms, '
              f'>50/window {fmt(sp["over50_total"])}' + ('' if len(a['blocks']) >= 2 else ' (one block: no noise estimate)'))


# Memory and pressure (sampler.py): per window and per arm the worst value, i.e. the minimum of MemAvailable and the
# maximum of the rest. Fields a device does not expose stay null.
MEMORY_FIELDS = (('mav', 'mem_available_min_mb', 1024, min), ('rss', 'rss_max_mb', 1024, max), ('swp', 'swap_max_mb', 1024, max),
                 ('shm', 'shmem_max_mb', 1024, max), ('kgk', 'kgsl_kernel_max_mb', 1 << 20, max),
                 ('kgm', 'kgsl_mapped_max_mb', 1 << 20, max), ('kgi', 'kgsl_imported_max_mb', 1 << 20, max),
                 ('kgsl', 'kgsl_max_mb', 1 << 20, max), ('pcs', 'psi_cpu_some_max', 1, max), ('pcf', 'psi_cpu_full_max', 1, max),
                 ('pms', 'psi_mem_some_max', 1, max), ('pmf', 'psi_mem_full_max', 1, max), ('ksw', 'kswapd_cpu_max_pct', 1, max))


def parse_sample_rows(path):
    """Every sampler record: time 't', frame 'f', the thermal fields, 'caps' and the memory fields (numbers or None)."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding='utf-8', errors='replace') as fh:
        lines = fh.readlines()
    for line in lines:
        p = line.split()
        if len(p) < 2 or 'caps=' not in line:
            continue
        try:
            t = float(p[0])
        except ValueError:
            continue
        d = dict(x.split('=', 1) for x in line.split('caps=', 1)[0].split()[1:] if '=' in x)
        num = lambda k: int(d[k]) if d.get(k, '').lstrip('-').isdigit() else None
        def real(k):
            try:
                return float(d[k]) if d.get(k) else None
            except ValueError:
                return None
        caps = []
        for c in line.split('caps=', 1)[1].split():
            s, _, m = c.partition('/')
            if s.isdigit() and m.isdigit():
                caps.append((int(s), int(m)))
        row = {'t': t, 'f': num('f'), 'cpu': num('cpu'), 'gpu': num('gpu'), 'cool': num('cool'), 'tpl': num('tpl'),
               'gcap': num('gcap'), 'caps': caps, 'ac': num('ac'), 'usb': num('usb'), 'level': num('level')}
        row.update(power_of_tokens(d))
        for key, *_ in MEMORY_FIELDS:
            row[key] = real(key) if key in d else None
        parts = [row[k] for k in ('kgk', 'kgm', 'kgi') if row[k] is not None]
        row['kgsl'] = sum(parts) if parts else None
        rows.append(row)
    return rows


def parse_samples(path, t0, t1, rows=None):
    return [r for r in (parse_sample_rows(path) if rows is None else rows) if t0 <= r['t'] <= t1]


def memory_of(rows):
    """The worst memory / pressure values of sampler rows ({} when none has any)."""
    out = {}
    for key, name, unit, worst in MEMORY_FIELDS:
        vs = [r[key] for r in rows if r.get(key) is not None]
        if vs:
            out[name] = round(worst(vs) / unit, 1)
    return out


def window_memory(end, rows):
    """Rows sampled while the window ending at `end` ran (their newest finished window ended in [end-300, end))."""
    return memory_of([r for r in rows if r['f'] is not None and end - 300 <= r['f'] < end])


def worst_memory(memories):
    out = {}
    for _, name, _, worst in MEMORY_FIELDS:
        vs = [m[name] for m in memories if m.get(name) is not None]
        if vs:
            out[name] = worst(vs)
    return out


def parse_freqs(path):
    """{(device, T0): {'start': freq_limits, 'end': freq_limits}} from pan.sh's freqs.txt (empty snapshots skipped)."""
    out = collections.defaultdict(dict)
    if not os.path.exists(path):
        return out
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            p = line.split()
            if len(p) < 4 or not p[2].startswith('T0=') or p[3] not in ('start', 'end'):
                continue
            fl = parse_freq_tokens(p[4:])
            if fl['cpu']:
                out[(p[0], int(p[2][3:]))][p[3]] = fl
    return out


def fmt_caps(caps):
    cpu, gpu = caps
    return f'CPU {"/".join(map(str, cpu))} kHz, GPU {gpu if gpu is not None else "?"} Hz'


def freq_stability(snapshots, rows, reference=None):
    """Whether the effective frequency caps stayed put over a run: snapshots = the freq_limits read at the start / end
    of its pans, rows = the sampler's records over the pans, reference = the device's first-scene caps (or None).
    Returns (True | False | None when nothing was recorded, [descriptions of the changes])."""
    changes = []
    caps = list(dict.fromkeys(caps_of_limits(s) for s in snapshots if s))
    if reference is not None and caps and caps[0] != reference:
        changes.append(f'caps differ from the device\'s first scene ({fmt_caps(reference)} -> {fmt_caps(caps[0])})')
    if len(caps) > 1:
        changes.append('caps changed between the pan start / end readings: ' + ' -> '.join(map(fmt_caps, caps)))
    cpu_seen = list(dict.fromkeys([c[0] for c in caps] + [tuple(s for s, _ in r['caps']) for r in rows if r['caps']]))
    if len(cpu_seen) > 1:
        changes.append('CPU scaling_max_freq changed during the pans: ' + ' -> '.join('/'.join(map(str, c)) for c in cpu_seen))
    gpu_seen = list(dict.fromkeys([s['gpu']['max_gpuclk_hz'] for s in snapshots if s and s['gpu']['max_gpuclk_hz']]
                                  + [r['gcap'] for r in rows if r.get('gcap')]))
    if len(gpu_seen) > 1:
        changes.append('GPU max_gpuclk changed during the pans: ' + ' -> '.join(map(str, gpu_seen)) + ' Hz')
    if not caps and not changes:
        return None, changes
    return not changes, changes


def read_kv(path, comments=True):
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8', errors='replace'):
            line = line.strip()
            if not line or (comments and line.startswith('#')) or '=' not in line:
                continue
            k, v = line.split('=', 1)
            out[k] = v
    return out


def mx(values):
    values = [v for v in values if v is not None]
    return max(values) if values else None


def deep_merge(base, over):
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            deep_merge(base[k], v)
        elif k == 'caveats' and isinstance(base.get(k), list):
            base[k] += [c for c in v if c not in base[k]]
        else:
            base[k] = v
    return base


def utc(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).isoformat()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out', help='the benchmark output directory (pans.txt, logs/, samples-*.txt, ...)')
    ap.add_argument('--json', help='write the result JSON here')
    ap.add_argument('--name', help='result name (default: the out directory name)')
    ap.add_argument('--date', help='measurement date (default: UTC date of the first pan)')
    ap.add_argument('--commit', help='source commit of the measured APK (its assets/build.txt)')
    ap.add_argument('--meta', help='JSON object deep-merged over the result (device names, profiles, extra caveats)')
    ap.add_argument('--arms', action='store_true', help='in-session A/B: assign windows to the arms of the live-switch marks')
    ap.add_argument('--uncapped', action='store_true',
                    help='uncapped measurement: disable dip classification and compare time-weighted throughput')
    ap.add_argument('--target-fps', type=int, choices=(30, 60),
                    help='override pacing target (otherwise pans.txt TARGET_FPS or preset TOTK_FPS60)')
    ap.add_argument('--guard-frames', type=int, default=GUARD_FRAMES,
                    help=f'frames after a live change before a window may start (default {GUARD_FRAMES})')
    a = ap.parse_args()
    if a.json:
        guard_output(a.json)
    out = a.out
    meta = json.load(open(a.meta, encoding='utf-8')) if a.meta else {}

    pans = collections.OrderedDict()
    pans_path = os.path.join(out, 'pans.txt')
    if not os.path.exists(pans_path):
        sys.exit(f'{pans_path} missing: run pan.sh for each device and scene first')
    for line in open(pans_path, encoding='utf-8'):
        p = line.split()
        if len(p) < 3:
            continue
        kv = dict(x.split('=', 1) for x in p[2:])
        pans.setdefault((p[0], re.sub(r'(?<=[A-Za-z_-])\d+$', '', p[1])), []).append({k: int(v) for k, v in kv.items()})
    if not pans:
        sys.exit(f'{out}/pans.txt has no pans')
    order = list(dict.fromkeys(list(meta.get('devices', {})) + [d for d, _ in pans]))
    order = [d for d in order if any(k[0] == d for k in pans)]
    scenes = list(dict.fromkeys(s for _, s in pans))

    logs, facts, devices, caveats = {}, {}, {}, []
    for dev in order:
        lp = os.path.join(out, 'logs', f'{dev}.log')
        if not os.path.exists(lp):
            sys.exit(f'missing {lp} (run collect.sh for {dev})')
        logs[dev] = parse_log(lp)
        info = logs[dev][5]
        facts[dev] = read_kv(os.path.join(out, f'facts-{dev}.txt'))
        senv = read_kv(os.path.join(out, f'settings-{dev}.env')) or dict(info.get('settings', {}))
        senv.pop('TOTK_DATA_DIR', None)
        effective = {**senv, **read_kv(os.path.join(out, f'totk-env-{dev}.txt'))}
        f = facts[dev]
        displays = f.get('displays')
        devices[dev] = {
            'name': f.get('model') or None, 'soc': f.get('soc') or None, 'gpu': f.get('gpu') or None,
            'android': f.get('android') or None, 'profile': None,
            'screens': {'1': 'single', '2': 'dual'}.get(displays, displays) if displays else None,
            'settings_env': senv,
            'uncapped': a.uncapped or effective.get('TOTK_UNCAPPED') == '1',
        }
        if f.get('serial_sha256'):
            devices[dev]['serial_sha256'] = f['serial_sha256']
        if os.path.exists(os.path.join(out, f'totk-env-{dev}.txt')):
            caveats.append(f'{dev}: a developer totk.env was present: not a default-profile run.')

    build_vals = collections.defaultdict(set)
    for dev in order:
        info, f = logs[dev][5], facts[dev]
        for key, val in (('app_version', f.get('app_version')), ('version_code', f.get('version_code')),
                         ('game_version', info.get('game_version')), ('apk_sha256', f.get('apk_sha256')),
                         ('turnip_driver_sha256', info.get('turnip'))):
            if val:
                build_vals[key].add(val)
    for key, vals in build_vals.items():
        if len(vals) > 1:
            caveats.append(f'devices disagree on {key}: {", ".join(sorted(vals))}.')
    one = lambda k: sorted(build_vals[k])[0] if build_vals.get(k) else None
    vc = one('version_code')
    build = {'app_version': one('app_version'), 'version_code': int(vc) if vc and vc.isdigit() else vc,
             'game_version': one('game_version'), 'commit': a.commit, 'apk_sha256': one('apk_sha256'),
             'turnip_driver_sha256': one('turnip_driver_sha256')}

    runs, table = [], []
    freqs = parse_freqs(os.path.join(out, 'freqs.txt'))
    try:
        gpu_snapshots = parse_gpu_snapshots(os.path.join(out, GPU_SNAPSHOT_FILE))
    except ValueError as exc:
        sys.exit(f'invalid GPU snapshots: {exc}')
    for dev in order:
        timing, gpu, fps, stut, longf, info = logs[dev]
        for scene in scenes:
            plist = pans.get((dev, scene))
            if not plist:
                continue
            print(f'\n## {dev} / {scene}')
            print('window          fps    p50    p95    p99    max   GPU ms  long>=25  long>=45  over33/over50(stutter line)')
            windows, thermal, all_rows, pan_residencies, batteries, targets, pan_summaries = [], [], [], [], [], [], []
            samples = parse_sample_rows(os.path.join(out, f'samples-{dev}.txt'))
            for kv in plist:
                f0, f1 = kv['F0'], kv['F1']
                preset = devices[dev]['settings_env'].get('TOTK_FPS60')
                target = a.target_fps or kv.get('TARGET_FPS') or (60 if preset == '1' else 30 if preset == '0' else None)
                targets.append(target)
                pan_window_start = len(windows)
                for f in sorted(x for x in timing if f0 + 600 <= x <= f1):
                    lf = [x for x in longf if f - 300 < x[0] <= f]
                    st = stut.get(f)
                    w = {'frames': [f - 299, f], 'fps': fps.get(f), 'p50_ms': timing[f][0], 'p95_ms': timing[f][1],
                         'p99_ms': info['frame_p99'].get(f, (None, None))[0],
                         'p99_samples': info['frame_p99'].get(f, (None, None))[1],
                         'max_ms': round(timing[f][2], 1), 'gpu_ms': gpu.get(f), 'long_ge25': sum(x[1] >= 25 for x in lf),
                         'long_ge45': sum(x[1] >= 45 for x in lf), 'over33': st[0] if st else None,
                         'over50': st[2] if st else None, 'draws_per_frame': info['draw_rates'].get(f)}
                    w.update(pacing_of(f, w['fps'], target, info, devices[dev]['uncapped']))
                    w['p99_p50_ratio'] = w['p99_ms'] / w['p50_ms'] if w['p99_ms'] is not None and w['p50_ms'] > 0 else None
                    w.update(presentation_of(f - 299, f, info))
                    windows.append(w)
                    print(f'{f-299:>6}-{f:<6} {w["fps"] or 0:5.1f} {w["p50_ms"]:6.2f} {w["p95_ms"]:6.2f} '
                          f'{w["p99_ms"] if w["p99_ms"] is not None else float("nan"):6.2f} {w["max_ms"]:6.1f} '
                          f'{w["gpu_ms"] if w["gpu_ms"] is not None else float("nan"):7.1f}  {w["long_ge25"]:>6}    '
                          f'{w["long_ge45"]:>6}    {st[0] if st else "-"}/{st[2] if st else "-"} '
                          f'(>{st[1] if st else "-"}/>{st[3] if st else "-"}) {w["pacing_state"]} '
                          f'near33={w["frames_near33"]} ({w["pacing_source"]}) '
                          f'draws/frame={w["draws_per_frame"] if w["draws_per_frame"] is not None else "-"}')
                rows = parse_samples(None, kv['T0'], kv['T1'], samples)
                pair = gpu_snapshots.get((dev, kv['T0']), {})
                try:
                    residency = {'status': 'ok', **gpu_residency_delta(pair.get('start'), pair.get('end'))}
                except ValueError as exc:
                    residency = {'status': 'unavailable', 'reason': str(exc)}
                pan_residencies.append(residency)
                if residency['status'] == 'ok':
                    label = 'active-clock frequency distribution' if residency['counter_kind'] == 'active' else 'residency'
                    print(f'  GPU {label}: ' + ', '.join(f'{hz / 1e6:g} MHz {100 * fraction:.2f}%'
                          for hz, fraction in residency['fractions'].items()) +
                          f' (counter unit {residency["counter_unit"]}; source {residency["source"]})')
                else:
                    print(f'  GPU residency unavailable: {residency["reason"]}')
                batteries.append([{'epoch': r['t'], **{k: r[k] for k in
                                  ('external_power', 'power_supplies', 'ac', 'usb', 'level')}} for r in rows])
                all_rows += rows
                if rows:
                    capped = sorted({c for r in rows for c in r['caps'] if c[0] != c[1]})
                    t = {'cpu_max_c': mx(r['cpu'] for r in rows), 'gpu_max_c': mx(r['gpu'] for r in rows),
                         'cooling_state_max': mx(r['cool'] for r in rows),
                         'gpu_thermal_pwrlevel_max': mx(r['tpl'] for r in rows),
                         'cpufreq_caps_khz': [{'scaling_max': s, 'cpuinfo_max': m} for s, m in capped],
                         'samples': len(rows)}
                    for k in ('cpu_max_c', 'gpu_max_c'):
                        t[k] = t[k] / 1000 if t[k] is not None else None
                    thermal.append(t)
                    print(f'  temps over pan: CPU max {t["cpu_max_c"]} C, GPU max {t["gpu_max_c"]} C; thermal cooling '
                          f'states max {t["cooling_state_max"]}, GPU thermal_pwrlevel max {t["gpu_thermal_pwrlevel_max"]}; '
                          f'cpufreq scaling_max<cpuinfo_max: {[f"{s}/{m}" for s, m in capped] or "none"} ({len(rows)} samples)')
                else:
                    print(f'  no samples over the pan {utc(kv["T0"])} .. {utc(kv["T1"])}')
                pan_summary = summarize(windows[pan_window_start:])
                pan_summaries.append(pan_summary)
                print(f'  pan T0={kv["T0"]}:')
                print_presentation(pan_summary, '    ')
            for w in windows:
                if m := window_memory(w['frames'][1], samples):
                    w['memory'] = m
            memory = worst_memory([w.get('memory', {}) for w in windows])
            if memory:
                print('  memory / pressure (worst over the windows): ' + ', '.join(f'{k} {v}' for k, v in memory.items()))
            summary = summarize(windows, thermal)
            print_presentation(summary)
            if summary.get('uncapped'):
                print(f'  uncapped: throughput {summary["throughput_fps"]} fps; dip_metrics_valid=false; '
                      f'fps stddev {summary["fps_stddev"]}; frame p50/p95/p99 window maxima '
                      f'{summary["p50_ms_max"]}/{summary["p95_ms_max"]}/{summary["p99_ms_max"]} ms; '
                      f'p99/p50 max {summary["p99_p50_ratio_max"]}; >33/>50 ms '
                      f'{summary["over33_total"]}/{summary["over50_total"]}')
            elif summary.get('pacing_windows'):
                steady = summary['fps_steady']
                print(f'  pacing: steady fps {steady if steady is not None else "-"}; mean fps {summary["fps_mean"]} '
                      f'(time-weighted {summary["fps_time_mean"]}); dip windows {summary["dip_windows"]}/{len(windows)}, '
                      f'dip time {100 * summary["dip_time_fraction"]:.1f}%; ~33 ms frames {summary["frames_near33"]}'
                      f'{" (lower bound / incomplete coverage)" if summary["frames_near33_lower_bound"] else ""}')
            else:
                caveats.append(f'{dev}/{scene}: pacing target unknown; use --target-fps or collect settings.env.')
            if 60 in targets and len(windows) < 12:
                caveats.append(f'{dev}/{scene}: only {len(windows)} complete windows at target 60; require >=12, '
                               'use pan.sh --target-fps 60 (144 s default) or repeat longer pans.')
            ab = None
            if a.arms:
                ab = arm_analysis(windows, parse_ctl(os.path.join(out, 'logs', f'{dev}.log')), a.guard_frames)
                print_arms(ab)
            snaps = [freqs.get((dev, kv['T0']), {}) for kv in plist]
            first = next((s.get('start') or s.get('end') for s in snaps if s), None)
            if first and not devices[dev].get('freq_limits'):
                devices[dev]['freq_limits'] = first
            ref = caps_of_limits(devices[dev].get('freq_limits'))
            stable, changes = freq_stability([s.get(k) for s in snaps for k in ('start', 'end')], all_rows, ref)
            print('  frequency caps: ' + (fmt_caps(caps_of_limits(first)) if first else 'not recorded (no freqs.txt)')
                  + {True: ', stable', False: ', CHANGED: ' + '; '.join(changes), None: ''}[stable])
            runs.append({'device': dev, 'scene': scene,
                         'pans': [{'start_frame': kv['F0'], 'end_frame': kv['F1'], 'start_utc': utc(kv['T0']),
                                   'end_utc': utc(kv['T1']), 'stick_value': kv['V'],
                                   'freq_limits': {'start': s.get('start'), 'end': s.get('end')},
                                   'target_fps': target, 'gpu_residency': residency, 'battery': battery,
                                   'summary': pan_summary}
                                  for kv, s, target, residency, battery, pan_summary in
                                  zip(plist, snaps, targets, pan_residencies, batteries, pan_summaries)],
                         'freq_stable': stable, 'freq_changes': changes,
                         'summary': summary, 'windows': windows, 'thermal': thermal,
                         **({'memory': memory} if memory else {}), **({'ab': ab} if ab else {})})
            table.append((dev, scene, summary, windows))

    for dev in order:
        capped = sorted({(c['scaling_max'], c['cpuinfo_max']) for r in runs if r['device'] == dev
                         for t in r['thermal'] for c in t['cpufreq_caps_khz']})
        if capped:
            caveats.append(f'{dev}: cpufreq limited below the hardware maximum during the pans (scaling_max/cpuinfo_max '
                           f'kHz {", ".join(f"{s}/{m}" for s, m in capped)}); compare only runs with the same limits.')
        hot = [r['scene'] for r in runs if r['device'] == dev for t in r['thermal']
               if (t['cooling_state_max'] or 0) > 0 or (t['gpu_thermal_pwrlevel_max'] or 0) > 0]
        if hot:
            caveats.append(f'{dev}: thermal throttling engaged during {", ".join(dict.fromkeys(hot))}.')
        unstable = [f'{r["scene"]} ({"; ".join(r["freq_changes"])})' for r in runs if r['device'] == dev
                    and r['freq_stable'] is False]
        if unstable:
            caveats.append(f'{dev}: frequency limits changed during {", ".join(unstable)}: freq_stable false, not usable '
                           'for comparison.')
        if not devices[dev].get('freq_limits'):
            caveats.append(f'{dev}: no frequency limits recorded (pan.sh freqs.txt): no frequency signature.')
    faults = {}
    for dev in order:
        dm = os.path.join(out, 'logs', f'{dev}-dmesg.txt')
        n = sum(1 for l in open(dm, encoding='utf-8', errors='replace') if FAULT_RE.search(l)) if os.path.exists(dm) else None
        faults[dev] = (n, logs[dev][5].get('device_lost', 0))
    bad = [f'{d} ({n} dmesg fault lines, {lost} device lost)' for d, (n, lost) in faults.items() if n or lost]
    missing = [d for d, (n, _) in faults.items() if n is None]
    caveats.append('long_ge45/long_ge25 counts are lower bounds; during sustained slowdowns the runtime does not log long frames.')
    if bad:
        caveats.append(f'GPU faults or device lost: {"; ".join(bad)}.')
    elif not missing:
        caveats.append('No GPU page faults, SMMU faults or device lost in any run.')
    if missing:
        caveats.append(f'no dmesg collected for {", ".join(missing)}: GPU faults not checked there.')

    sticks = {kv['V'] for plist in pans.values() for kv in plist}
    stick = 'half deflection' if sticks == {16000} else f'value {"/".join(map(str, sorted(sticks)))} of 32767'
    first_t0 = min(kv['T0'] for plist in pans.values() for kv in plist)
    doc = {
        'schema': SCHEMA,
        'name': a.name or os.path.basename(os.path.normpath(out)),
        'date': a.date or utc(first_t0)[:10],
        'build': build,
        'method': {'summary': METHOD_SUMMARY.format(stick=stick), 'sources': dict(METHOD_SOURCES)},
        'devices': devices,
        'scenes': {s: dict(KNOWN_SCENES.get(s, {'name': s, 'save_slot': None, 'note': None})) for s in scenes},
        'runs': runs,
        'caveats': caveats,
    }
    deep_merge(doc, meta)
    for dev in order:
        sig = device_signature(doc, dev)
        if sig:
            doc['devices'][dev]['freq_signature'] = {'sha256': sig['sha256'], 'short': sig['short'],
                                                     'caps': sig_caps_text(sig)}
            print(f'\n{dev}: frequency signature {sig["short"]}: {sig_caps_text(sig)}')

    print('\n| Device | Scene | Windows | fps | p50 ms | p95 ms | Worst ms | GPU ms | ≥45 ms | >50 ms | Max CPU / GPU °C |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    rng = lambda xs, nd: '-' if not xs else (f'{min(xs):.{nd}f}' if min(xs) == max(xs) else f'{min(xs):.{nd}f}–{max(xs):.{nd}f}')
    for dev, scene, s, ws in table:
        g = lambda k: [w[k] for w in ws if w[k] is not None]
        print(f'| {doc["devices"][dev].get("name") or dev} | {doc["scenes"][scene]["name"]} | {s["windows"]} | {rng(g("fps"), 1)} | '
              f'{rng(g("p50_ms"), 2)} | {rng(g("p95_ms"), 1)} | {s["max_ms"]} | {rng(g("gpu_ms"), 1)} | {s["long_ge45_total"]} | '
              f'{s["over50_total"]} | {s["cpu_max_c"]} / {s["gpu_max_c"]} |')
    print('\nCaveats:\n' + '\n'.join(f'- {c}' for c in doc['caveats']))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        with open(a.json, 'w', encoding='utf-8') as fh:
            json.dump(doc, fh, ensure_ascii=False, indent=2)
            fh.write('\n')
        print(f'\nwrote {a.json}')


if __name__ == '__main__':
    main()

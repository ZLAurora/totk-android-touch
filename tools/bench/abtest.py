#!/usr/bin/env python3
"""abtest.py <ab.json> [--ref A] [--out FILE]

Per device x scene of an in-session A/B (ab.sh, then analyze.py <out> --arms --json <ab.json>): every arm against the
reference arm (--ref, default A), per metric, with a verdict.

Values: each arm's pooled summary over its windows. Verdicts use equally weighted B-A block pairs of the same pan
direction within each complete ABBA (or BAAB) quartet: positions 1 vs 3 and 4 vs 2. Missing directions in old JSON
are inferred from block indices (odd right, even left). Incomplete/non-ABBA quartets are not paired.

Report: pooled delta, paired mean delta, per-direction mean delta, sign agreement with the paired mean, and a
two-sided exact sign test (zero differences are ties, excluded from the test but not from agreement). The old noise
(larger same-arm max-min block spread) is informational, not a verdict gate. Counts are compared per window.

Verdict rule for one metric:
  same            |paired delta| <= the compare.py tolerance, or pair signs disagree without p < 0.05 and >=6 pairs
  better / worse  pooled AND paired deltas beyond tolerance in the same direction, and all pair signs agree
                  (or exact sign p < 0.05 with >=6 pairs)
  lean better/worse only the paired delta crosses tolerance, or the pooled delta has the opposite sign; informational
  better? worse?  fewer than two valid pairs covering both directions (unconfirmed; pooled delta if no pairs)
  n/a             a value is missing
Modern capped summaries compare steady fps and dip time fraction separately; fps mean is informational. A pooled or
paired dip fraction difference above 0.05 absolute makes the overall result 'verdict confounded by dips', regardless
of pair confirmation. Missing steady fps or pacing data makes capped comparisons inconclusive.
Uncapped summaries disable dips and compare time-weighted throughput plus frame-time tails instead; capped versus
uncapped is inconclusive, never dip-confounded. Wholly legacy summaries retain the labelled fps-mean primary rule.
Otherwise an arm is worse if any primary metric is worse, else better if any is better, else inconclusive if any
is better? / worse?, else same. Exit 1 when any arm is worse. Memory and pressure are informational.
Prefer ABBAABBA or longer on 60 fps devices.

Restart sessions: abtest.py --restart-quartet O1.json B1.json B2.json O2.json --ref O.
This distinct mode pairs adjacent sessions (1 vs 2, 4 vs 3), with identical pan schedules, not alternating
in-session directions. Exactly two pairs must agree in sign; pooled AND paired deltas must exceed tolerance
(fps: max(0.5 fps, 2%)). Reference O1/O2 spread must not exceed tolerance; otherwise INCONCLUSIVE.
Other metrics retain compare.py tolerances and capped/uncapped rules. A quartet is not statistical significance.
GPU clock: each session's residency-weighted mean GPU clock comes from its pans' gpu_residency. When the arm means
differ by more than 3% the overall result is 'verdict confounded by GPU clock' (a governor that settles at another
clock changes GPU ms and fps by itself); --clock-lever keeps the verdict for arms that change the clock on purpose.
Missing residency is reported as not checked, never as equal."""
import argparse, io, json, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import (DIP_CONFOUNDED, DIP_TIME_FRACTION_TOLERANCE, LEGACY_PRIMARY, METRICS, PRIMARY,  # noqa: E402
                      guard_output, pacing_note, pacing_verdict, primary_metrics)
from analyze import AB_METRICS, MEMORY_FIELDS, block_metric, pan_direction, summarize, worst_memory  # noqa: E402

GPU_CLOCK_CONFOUNDED = 'verdict confounded by GPU clock'
GPU_CLOCK_TOLERANCE = 0.03


def session_gpu_clock(run):
    """Residency-weighted mean GPU clock in Hz over a session's pans; None unless every pan has one counter kind."""
    means, kinds = [], set()
    for pan in run.get('pans', []):
        residency = pan.get('gpu_residency') or {}
        fractions = residency.get('fractions')
        if residency.get('status') != 'ok' or not fractions:
            return None
        kinds.add(residency.get('counter_kind'))
        means.append(sum(int(hz) * fraction for hz, fraction in fractions.items()))
    return sum(means) / len(means) if means and len(kinds) == 1 else None


def gpu_clock_difference(ref, arm):
    """Relative difference of the arms' mean session clocks; None when any session lacks residency."""
    clocks = ref.get('gpu_clock_hz', []) + arm.get('gpu_clock_hz', [])
    if not ref.get('gpu_clock_hz') or not arm.get('gpu_clock_hz') or None in clocks or min(clocks) <= 0:
        return None
    r, a = (sum(x['gpu_clock_hz']) / len(x['gpu_clock_hz']) for x in (ref, arm))
    return abs(a - r) / max(a, r)


def restart_analysis(documents, reference):
    """Build arm records from four independent analyzer outputs, preserving whole session pair units."""
    indexed = []
    for document in documents:
        runs = {}
        for run in document.get('runs', []):
            key = run['device'], run['scene']
            if key in runs:
                raise ValueError(f'duplicate device/scene in session: {key}')
            runs[key] = run
        indexed.append(runs)
    if not indexed[0] or any(set(runs) != set(indexed[0]) for runs in indexed[1:]):
        raise ValueError('all four restart sessions must contain the same nonempty device/scene set')
    candidate = 'B' if reference != 'B' else 'A'
    result = []
    for key in indexed[0]:
        sessions = [runs[key] for runs in indexed]
        schedules = [[(p.get('direction'), p.get('stick_value')) for p in r.get('pans', [])] for r in sessions]
        if (not schedules[0] or any(direction not in ('right', 'left') for schedule in schedules
                                     for direction, _ in schedule) or any(s != schedules[0] for s in schedules[1:])):
            raise ValueError(f'{key}: restart quartet needs identical, explicit pan direction/stick schedules; '
                             're-analyze original pans.txt with current analyze.py')
        if len({bool(r['summary'].get('uncapped') or r['summary'].get('speed_mode') == 'uncapped')
                for r in sessions}) != 1:
            raise ValueError(f'{key}: mixed capped/uncapped restart sessions are inconclusive; do not pool them')
        arms = {}
        for name, indices in ((reference, (0, 3)), (candidate, (1, 2))):
            blocks = [{'block': i + 1, 'summary': sessions[i]['summary'], 'direction': 'restart',
                       'session': i + 1} for i in indices]
            windows = [w for i in indices for w in sessions[i].get('windows', [])]
            if any(not sessions[i].get('windows') for i in indices):
                raise ValueError(f'{key}: each restart session needs nonempty measured windows')
            spread = {}
            for metric in AB_METRICS:
                values = [block_metric(b['summary'], metric) for b in blocks]
                spread[metric] = abs(values[1] - values[0]) if None not in values else None
            arms[name] = {'blocks': blocks, 'summary': summarize(windows), 'spread': spread,
                          'memory': worst_memory([sessions[i].get('memory', {}) for i in indices]),
                          'gpu_clock_hz': [session_gpu_clock(sessions[i]) for i in indices]}
        result.append({'device': key[0], 'scene': key[1], 'ab': {
            'mode': 'restart-quartet', 'arms': arms,
            'windows': {'assigned': sum(len(r['windows']) for r in sessions), 'straddling': 0, 'unlabelled': 0}}})
    return {'runs': result}


def paired_metric(key, ref, arm, mode='in-session'):
    """Equal-weight complete quartet differences; restart mode pairs adjacent independent sessions."""
    blocks = {}
    for label, record in (('ref', ref), ('arm', arm)):
        for b in record['blocks']:
            index = b.get('block')
            if isinstance(index, int) and index > 0:
                blocks.setdefault(index, []).append((label, b))
    pairs = []
    for start in sorted({1 + 4 * ((i - 1) // 4) for i in blocks}):
        if any(len(blocks.get(i, [])) != 1 for i in range(start, start + 4)):
            continue
        quartet = [blocks[i][0] for i in range(start, start + 4)]
        labels = [label for label, _ in quartet]
        if labels not in (['ref', 'arm', 'arm', 'ref'], ['arm', 'ref', 'ref', 'arm']):
            continue
        for i, j in (((0, 1), (3, 2)) if mode == 'restart-quartet' else ((0, 2), (3, 1))):
            (_, x), (_, y) = quartet[i], quartet[j]
            r, a = (x, y) if quartet[i][0] == 'ref' else (y, x)
            direction = r.get('direction', pan_direction(r['block']))
            if mode != 'restart-quartet' and (direction not in ('right', 'left') or
                                             direction != a.get('direction', pan_direction(a['block']))):
                continue
            rv, av = block_metric(r['summary'], key), block_metric(a['summary'], key)
            if rv is not None and av is not None:
                pairs.append({'reference_block': r['block'], 'arm_block': a['block'], 'direction': direction,
                              'delta': av - rv})
    ds = [p['delta'] for p in pairs]
    mean = sum(ds) / len(ds) if ds else None
    positive, negative = sum(d > 0 for d in ds), sum(d < 0 for d in ds)
    nonzero = positive + negative
    # Under the null, either sign is equally likely for each independent nonzero block pair.
    p_value = min(1.0, 2 * sum(math.comb(nonzero, k) for k in range(min(positive, negative) + 1)) / 2 ** nonzero)
    return {'pairs': pairs, 'delta': mean,
            'mode': mode, 'reference_spread': ref['spread'].get(key),
            'directions': {direction: sum(p['delta'] for p in pairs if p['direction'] == direction) /
                          sum(p['direction'] == direction for p in pairs)
                          for direction in ('right', 'left') if any(p['direction'] == direction for p in pairs)},
            'agreement': sum(d * mean > 0 for d in ds) if mean is not None else 0,
            'ties': len(ds) - nonzero, 'p_value': p_value if pairs else None}


def metric_verdict(key, ref, arm, paired=None):
    """(paired delta, tolerance, informational spread, verdict); pooled delta if no valid pairs."""
    _, _, direction, kind, (tol_abs, tol_rel) = next(m for m in METRICS if m[0] == key)
    r, a = block_metric(ref['summary'], key), block_metric(arm['summary'], key)
    if r is None or a is None:
        return None, None, None, 'n/a'
    paired = paired_metric(key, ref, arm) if paired is None else paired
    d = paired['delta'] if paired['delta'] is not None else a - r
    tol = tol_abs if kind == 'abs' else max(tol_abs, tol_rel * abs(r))
    spreads = (ref['spread'].get(key), arm['spread'].get(key))
    noise = None if None in spreads else max(spreads)
    if paired.get('mode') == 'restart-quartet':
        spread = paired.get('reference_spread')
        n = len(paired['pairs'])
        if n != 2 or spread is None or (spread > tol and not math.isclose(spread, tol, rel_tol=1e-12, abs_tol=1e-12)):
            return d, tol, noise, 'inconclusive'
        if abs(d) <= tol or math.isclose(abs(d), tol, rel_tol=1e-12, abs_tol=1e-12):
            return d, tol, noise, 'same'
        if (abs(a - r) <= tol or math.isclose(abs(a - r), tol, rel_tol=1e-12, abs_tol=1e-12) or
                (a - r) * d <= 0 or paired['agreement'] != 2):
            return d, tol, noise, 'inconclusive'
        return d, tol, noise, 'better' if d * direction > 0 else 'worse'
    if abs(d) <= tol or math.isclose(abs(d), tol, rel_tol=1e-12, abs_tol=1e-12):
        return d, tol, noise, 'same'
    word = 'better' if d * direction > 0 else 'worse'
    n = len(paired['pairs'])
    if n < 2 or len(paired['directions']) < 2:
        return d, tol, noise, word + '?'
    if abs(a - r) <= tol or math.isclose(abs(a - r), tol, rel_tol=1e-12, abs_tol=1e-12) or (a - r) * d <= 0:
        return d, tol, noise, 'lean ' + word
    confirmed = paired['agreement'] == n or (n >= 6 and paired['p_value'] < 0.05)
    return d, tol, noise, word if confirmed else 'same'


def arm_verdict(per, ref=None, arm=None, paired=None):
    """Overall arm verdict; modern callers supply records to check pooled as well as paired dip differences.

    The per-only form retains historical callers. Recognizable modern metrics without the records are inconclusive:
    a paired tuple alone cannot establish that the pooled dip fractions match.
    """
    modern = any(k in per and per[k][3] != 'n/a' for k in ('fps_steady', 'dip_time_fraction'))
    if ref is None or arm is None:
        if modern:
            delta = per.get('dip_time_fraction', (None,))[0]
            if delta is not None and abs(delta) > DIP_TIME_FRACTION_TOLERANCE and not math.isclose(
                    abs(delta), DIP_TIME_FRACTION_TOLERANCE, rel_tol=1e-12, abs_tol=1e-12):
                return DIP_CONFOUNDED
            return 'inconclusive'
        primary = LEGACY_PRIMARY
    else:
        primary = primary_metrics(ref['summary'], arm['summary'])
        pacing = pacing_verdict(ref['summary'], arm['summary'])
        if pacing == DIP_CONFOUNDED:
            return pacing
        if primary == PRIMARY:
            dips = paired_metric('dip_time_fraction', ref, arm) if paired is None else paired['dip_time_fraction']
            delta = dips['delta']
            if delta is not None and abs(delta) > DIP_TIME_FRACTION_TOLERANCE and not math.isclose(
                    abs(delta), DIP_TIME_FRACTION_TOLERANCE, rel_tol=1e-12, abs_tol=1e-12):
                return DIP_CONFOUNDED
        if pacing:
            return pacing
    prim = [per[k][3] for k in primary if k in per]
    if 'worse' in prim:
        return 'worse'
    if 'inconclusive' in prim:
        return 'inconclusive'
    if 'better' in prim:
        return 'better'
    if any(v.endswith('?') for v in prim):
        return 'inconclusive'
    return 'n/a' if all(v == 'n/a' for v in prim) else 'same'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('result', nargs='+', help='one analyze.py --arms JSON, or four session JSONs in O/B/B/O order')
    ap.add_argument('--ref', default='A', help='reference arm (default A)')
    ap.add_argument('--out', help='also write the report to this file')
    ap.add_argument('--restart-quartet', action='store_true',
                    help='explicit independent O/B/B/O session mode; adjacent pairs and O-spread gate')
    ap.add_argument('--clock-lever', action='store_true',
                    help='restart-quartet: the arm changes the GPU clock on purpose; report the difference, keep the verdict')
    a = ap.parse_args()
    if a.out:
        guard_output(a.out)
    if len(a.result) != (4 if a.restart_quartet else 1):
        ap.error('restart-quartet requires four session JSONs in O/B/B/O order; in-session requires one --arms JSON')
    documents = [json.load(open(path, encoding='utf-8')) for path in a.result]
    try:
        doc = restart_analysis(documents, a.ref) if a.restart_quartet else documents[0]
    except ValueError as exc:
        ap.error(str(exc))
    buf, worse = io.StringIO(), 0
    p = lambda *x: print(*x, file=buf)
    fmt = lambda v, nd=2: '-' if v is None else f'{v:+.{nd}f}' if isinstance(v, float) else str(v)
    runs = [r for r in doc.get('runs', []) if r.get('ab')]
    if not runs:
        sys.exit(f'{a.result}: no run has "ab" (run analyze.py with --arms)')
    for run in runs:
        mode = run['ab'].get('mode', 'in-session') if a.restart_quartet else 'in-session'
        p(f'comparison mode: {mode}' + (' (independent sessions O/B/B/O; pairs 1-2 and 4-3)' if a.restart_quartet else ''))
        ab = run['ab']; arms = ab['arms']; c = ab['windows']
        p(f'## {run["device"]} / {run["scene"]}: {c["assigned"]} windows in arms, {c["straddling"]} straddling and '
          f'{c["unlabelled"]} unlabelled dropped')
        if a.ref not in arms:
            p(f'  no reference arm {a.ref} (arms: {", ".join(arms) or "none"})')
            continue
        ref = arms[a.ref]
        for name, arm in arms.items():
            if name == a.ref:
                continue
            paired = {k: paired_metric(k, ref, arm, mode) for k in AB_METRICS}
            per = {k: metric_verdict(k, ref, arm, paired[k]) for k in AB_METRICS}
            v = arm_verdict(per, ref, arm, paired)
            clock = gpu_clock_difference(ref, arm) if a.restart_quartet else None
            if clock is not None and clock > GPU_CLOCK_TOLERANCE and not a.clock_lever and v != DIP_CONFOUNDED:
                v = GPU_CLOCK_CONFOUNDED
            worse += v == 'worse'
            headline = v if v in (DIP_CONFOUNDED, GPU_CLOCK_CONFOUNDED) else v.upper()
            p(f'\n  arm {name} vs {a.ref}: {headline}  ({len(arm["blocks"])} vs {len(ref["blocks"])} blocks, '
              f'{arm["summary"]["windows"]} vs {ref["summary"]["windows"]} windows)')
            p(f'  {pacing_note(ref["summary"], arm["summary"])}')
            if a.restart_quartet:
                mhz = lambda record: ' / '.join('-' if hz is None else f'{hz / 1e6:.1f}'
                                                for hz in record.get('gpu_clock_hz', [])) or '-'
                p(f'  GPU clock, residency-weighted mean MHz per session: {a.ref} {mhz(ref)}; {name} {mhz(arm)}; ' +
                  ('residency unavailable, clock confound not checked' if clock is None else
                   f'arms differ by {100 * clock:.1f}% (limit {100 * GPU_CLOCK_TOLERANCE:.0f}%)' +
                   (', intended (--clock-lever)' if a.clock_lever and clock > GPU_CLOCK_TOLERANCE else '')))
            primary = primary_metrics(ref['summary'], arm['summary'])
            p(f'  {"metric":<16} {a.ref:>9} {name:>9} {"pooled Δ":>9} {"paired Δ":>9} {"tolerance":>9} {"spread":>9}  verdict')
            for key, label, *_ in METRICS:
                if key not in per:
                    continue
                _, tol, noise, verdict = per[key]
                rate = key in ('over33_total', 'over50_total', 'long_ge45_total')
                rv, av = block_metric(ref['summary'], key), block_metric(arm['summary'], key)
                pooled = av - rv if rv is not None and av is not None else None
                p(f'  {label + (" /win" if rate else ""):<16} {"-" if rv is None else f"{rv:g}":>9} '
                  f'{"-" if av is None else f"{av:g}":>9} {fmt(pooled):>9} {fmt(paired[key]["delta"]):>9} '
                  f'{"-" if tol is None else f"{tol:.2f}":>9} '
                  f'{"-" if noise is None else f"{noise:.2f}":>9}  {verdict}{" *" if key in primary else ""}')
                stats = paired[key]
                directions = ', '.join(f'{direction} {fmt(delta)}' for direction, delta in stats['directions'].items())
                probability = '-' if stats['p_value'] is None else f'{stats["p_value"]:.4f}'
                p(f'    pairs: {len(stats["pairs"])}; direction Δ: {directions or "-"}; '
                  f'sign agreement {stats["agreement"]}/{len(stats["pairs"])} (ties {stats["ties"]}); '
                  f'exact sign p={probability}')
                if a.restart_quartet:
                    p(f'    O-spread: {fmt(stats["reference_spread"])}; must be <= tolerance; '
                      'both adjacent pair signs must agree (no direction-confirmation claim)')
                    p('    session pair deltas: ' + ', '.join(
                        f'positions {pair["reference_block"]}/{pair["arm_block"]} (B-O) {fmt(pair["delta"])}'
                        for pair in stats['pairs']))
            mem = [(name, ref.get('memory', {}).get(name), arm.get('memory', {}).get(name)) for _, name, _, _ in MEMORY_FIELDS]
            if any(r is not None or x is not None for _, r, x in mem):
                p('  memory / pressure, worst over each arm\'s windows (' + a.ref + ' -> ' + name + '): ' + '; '.join(
                    f'{k} {"-" if r is None else r} -> {"-" if x is None else x}' for k, r, x in mem if r is not None or x is not None))
        if a.restart_quartet:
            p('\n  * primary metric. RESTART-QUARTET rule: pooled AND paired deltas beyond tolerance in the same '
              'direction (fps: max(0.5 fps, 2%)); both adjacent pair signs agree; O1/O2 spread <= tolerance. '
              'Missing pairs/spread or unstable O => INCONCLUSIVE. Other metrics use compare.py tolerances. '
              'This is not in-session direction confirmation or statistical significance. '
              'Capped dip-confounding and uncapped throughput/tail rules are unchanged.')
        else:
            p('\n  * primary metric. Rule: pooled AND paired deltas beyond the compare.py tolerance in the same direction, '
              'and (all pair signs agree or exact two-sided sign p < 0.05 with >=6 pairs); at least two pairs covering both '
              'directions. "lean" = only paired delta crosses tolerance (or pooled sign differs), informational only; '
              '"?" = insufficient pairs. Capped-only dip fractions differing by >0.05 override their arm result; '
              'uncapped arms compare throughput and tails with dips disabled. Same-arm spread is informational. '
              'Prefer ABBAABBA or longer at 60 fps.')
    sys.stdout.write(buf.getvalue())
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(buf.getvalue())
    sys.exit(1 if worse else 0)


if __name__ == '__main__':
    main()

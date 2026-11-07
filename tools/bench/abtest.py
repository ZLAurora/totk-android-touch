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
Prefer ABBAABBA or longer on 60 fps devices."""
import argparse, io, json, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import (DIP_CONFOUNDED, DIP_TIME_FRACTION_TOLERANCE, LEGACY_PRIMARY, METRICS, PRIMARY,  # noqa: E402
                      guard_output, pacing_note, pacing_verdict, primary_metrics)
from analyze import AB_METRICS, MEMORY_FIELDS, block_metric, pan_direction  # noqa: E402


def paired_metric(key, ref, arm):
    """Equal-weight same-direction block differences within complete ABBA/BAAB quartets."""
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
        for i, j in ((0, 2), (3, 1)):
            (_, x), (_, y) = quartet[i], quartet[j]
            r, a = (x, y) if quartet[i][0] == 'ref' else (y, x)
            direction = r.get('direction', pan_direction(r['block']))
            if direction not in ('right', 'left') or direction != a.get('direction', pan_direction(a['block'])):
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
    if 'better' in prim:
        return 'better'
    if any(v.endswith('?') for v in prim):
        return 'inconclusive'
    return 'n/a' if all(v == 'n/a' for v in prim) else 'same'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('result', help='analyze.py --arms JSON')
    ap.add_argument('--ref', default='A', help='reference arm (default A)')
    ap.add_argument('--out', help='also write the report to this file')
    a = ap.parse_args()
    if a.out:
        guard_output(a.out)
    doc = json.load(open(a.result, encoding='utf-8'))
    buf, worse = io.StringIO(), 0
    p = lambda *x: print(*x, file=buf)
    fmt = lambda v, nd=2: '-' if v is None else f'{v:+.{nd}f}' if isinstance(v, float) else str(v)
    runs = [r for r in doc.get('runs', []) if r.get('ab')]
    if not runs:
        sys.exit(f'{a.result}: no run has "ab" (run analyze.py with --arms)')
    for run in runs:
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
            paired = {k: paired_metric(k, ref, arm) for k in AB_METRICS}
            per = {k: metric_verdict(k, ref, arm, paired[k]) for k in AB_METRICS}
            v = arm_verdict(per, ref, arm, paired)
            worse += v == 'worse'
            headline = v if v == DIP_CONFOUNDED else v.upper()
            p(f'\n  arm {name} vs {a.ref}: {headline}  ({len(arm["blocks"])} vs {len(ref["blocks"])} blocks, '
              f'{arm["summary"]["windows"]} vs {ref["summary"]["windows"]} windows)')
            p(f'  {pacing_note(ref["summary"], arm["summary"])}')
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
            mem = [(name, ref.get('memory', {}).get(name), arm.get('memory', {}).get(name)) for _, name, _, _ in MEMORY_FIELDS]
            if any(r is not None or x is not None for _, r, x in mem):
                p('  memory / pressure, worst over each arm\'s windows (' + a.ref + ' -> ' + name + '): ' + '; '.join(
                    f'{k} {"-" if r is None else r} -> {"-" if x is None else x}' for k, r, x in mem if r is not None or x is not None))
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

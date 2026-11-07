#!/usr/bin/env python3
"""compare.py <reference.json> <new.json> [--out FILE]

Per device x scene, the reference's and the new run's summary metrics (totk-android-benchmark/1 results: a baseline
from docs/benchmarks/baselines/index.json, a ledger run file or an analyze.py --json output) with deltas and a verdict
per metric and per scene. Devices are paired by label, else by model / name. Exits 1 when any scene whose frequency
signatures match is worse.

Frequency signature (benchlib.signature_of): device model + SoC, the settings_env hash, the per-cluster effective CPU
caps (cpufreq scaling_max_freq, kHz) and the effective GPU cap (Hz). Each device pair is labelled:
  match           same signature: the verdicts are a regression check (they decide the exit status)
  reference only  the reference has no GPU cap recorded (the 1.0.0 baseline) but model, settings and CPU caps match:
                  shown for information, never decides the exit status
  NOT COMPARABLE  the signatures differ, or one side has none: printed with a loud WARNING, never decides the exit
                  status. Compare against the baseline of the new run's own signature instead.
The signature of baseline-1.0.0.json comes from the baseline index (CPU caps from its sampler records, GPU cap null).

Verdict thresholds (a difference within the tolerance is 'same'; beyond it 'better' or 'worse'):
  fps steady      max(0.5 fps, 2 %)        higher is better
  throughput fps  max(0.5 fps, 2 %)        higher is better; primary in uncapped comparisons only
  dip time fraction 0.05 absolute          lower is better (five percentage points)
  fps mean        max(0.5 fps, 2 %)        informational for modern summaries; primary for wholly legacy comparisons
  fps min         max(1 fps, 5 %)          higher is better
  p50 median ms   max(0.3 ms, 2 %)         lower is better
  p95 max ms      max(1 ms, 5 %)           lower is better
  p50 max ms      max(0.3 ms, 2 %)         lower is better; uncapped primary
  p99 max ms      max(1 ms, 5 %)           lower is better; uncapped primary
  fps stddev      max(0.5 fps, 10 %)       informational
  p99/p50 max     max(0.1, 5 %)           informational
  worst frame ms  max(5 ms, 15 %)          lower is better
  GPU ms mean     max(0.5 ms, 3 %)         lower is better
  long >=45 ms    per window: max(0.5, 25 %) lower is better (totals over runs with different window counts)
  >33 ms frames   per window: max(0.5, 25 %) lower is better; uncapped primary
  >50 ms frames   per window: max(0.5, 25 %) lower is better
  CPU / GPU max C 3 C                      lower is better
Modern summaries use steady fps, dip time fraction, p50 median, p95 max, GPU ms mean and >50 ms frames as primary
metrics; fps mean is informational. A dip fraction difference above 0.05 absolute yields 'verdict confounded by dips'.
Missing steady fps (including all-dip runs) or missing pacing data makes the result inconclusive, not a mean-FPS
conclusion. Wholly legacy comparisons retain the historical fps-mean primary rule and are labelled as lacking pacing
info. Otherwise a scene is 'worse' if any primary metric is worse, else 'better' if any is better, else 'same'.
Uncapped summaries (effective TOTK_UNCAPPED=1 or analyze.py --uncapped) disable dip classification. Their primary
speed is time-weighted throughput_fps; primary tails are frame p50/p95/p99 window maxima, worst frame, >33 and >50
ms counts per window. Dip differences never confound an uncapped verdict. Capped versus uncapped is inconclusive.
Missing p99 stays unavailable, never substituted with a present-fence percentile or the worst frame.
A WARNING also marks devices whose settings_env or cpufreq caps differ, or whose frequency limits changed during a
run (freq_stable false)."""
import argparse, io, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchlib import DIP_CONFOUNDED, compare_docs, guard_output, load_result  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], epilog=__doc__.split('\n\n', 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('baseline', help='the reference result JSON')
    ap.add_argument('new')
    ap.add_argument('--out', help='also write the comparison to this file')
    a = ap.parse_args()
    if a.out:
        guard_output(a.out)
    base, new = load_result(a.baseline), load_result(a.new)
    buf = io.StringIO()
    print(f'# {new.get("name")} vs {base.get("name")}', file=buf)
    verdicts = compare_docs(base, new, buf, base_path=a.baseline, new_path=a.new)
    count = lambda v, rel: sum(1 for o, r in verdicts.values() if o == v and r == rel)
    extra = ''.join(f', {count(v, "match")} {v}' for v in ('inconclusive', DIP_CONFOUNDED, 'n/a') if count(v, 'match'))
    print(f'\nscenes with matching signatures: {count("better", "match")} better, {count("worse", "match")} worse, '
          f'{count("same", "match")} same{extra}', file=buf)
    other = {r: sum(1 for _, x in verdicts.values() if x == r) for r in ('reference-only', 'mismatch', 'unknown')}
    if any(other.values()):
        print(f'scenes not used for the verdict: {other["reference-only"]} reference only, '
              f'{other["mismatch"] + other["unknown"]} NOT COMPARABLE', file=buf)
    sys.stdout.write(buf.getvalue())
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(buf.getvalue())
    sys.exit(1 if count('worse', 'match') else 0)


if __name__ == '__main__':
    main()

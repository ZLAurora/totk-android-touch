# Benchmark tools

Host-side scripts (POSIX sh and Python 3 standard library) for measuring TotK Native on rooted Android devices over adb
and comparing against the recorded baselines. The full procedure is the `totk-benchmark` skill
(`.omp/skills/totk-benchmark/SKILL.md`); this is the quick reference.

Device side: `su` root (for `sendevent`, the app's private `totk.log` and `settings.env`, `dmesg`, and the thermal /
cpufreq / kgsl sysfs files). Ordinary benchmark tools write only the dmesg marker and live-control file
(`<game>/totk.ctl`, atomically published with the game folder's owner and explicit SELinux context from `ls -Zd`).
The explicit CPU profiler below also stages its NDK binary in `/data/local/tmp` and temporarily changes
recorded perf access controls. Everything is read from `totk.log` and sysfs / procfs, never from
logcat (some devices' logd drops the game's lines). `bench_input.sh`, `pan.sh` and `live.sh` run every root command
through one persistent root shell per device (`rootsh.py`; `sampler.py` holds its own), pinned to the slowest cpufreq
cluster: a `su -c` per command costs the device a su session and, with Magisk, a su-notify `app_process` on any core.

Common options: `-s <serial>` (adb serial), `--device <label>` (a short name used in file names, e.g. `thor`),
`--package` (default `com.aure.totk`), `--game-dir` (default `/data/user/0/<package>/files/game`).

| Script | Use |
|---|---|
| `bench_input.sh -s <serial> [--event /dev/input/eventN] [--display <id>] <action>` | `press <A\|B\|X\|Y\|L\|R\|PLUS\|MINUS\|UP\|DOWN\|LEFT\|RIGHT> [ms]`, `rstick <x> [y]` (`rstick 0` releases), `shot <file>`, `log`, `detect`, `freqs` (the frequency limits as one line: `cpuN=<cpus>:<scaling_min>:<scaling_max>:<cpuinfo_max>` kHz per cpufreq policy, `gpu_maxclk`, `gpu_freqs`, `gpu_devfreq_max`, GPU power levels). Auto-detects the controller node (`getevent -p`: ABS_Z/ABS_RZ + BTN_SOUTH) and the first display (`dumpsys SurfaceFlinger --display-id`). |
| `pan.sh -s <serial> --device <label> --scene <label> [--target-fps 30\|40\|60] [--seconds N] [--repeat N] [--value 16000] [--first right\|left] [--no-shot] --out <dir>` | Return pan: stick right for half the time then left (`--first left`: reverse). Defaults to 48 s at 30 target, 144 s at 40/60 target. Records frame/time/frequency boundaries and exact GPU cumulative snapshots per pan, then optional screenshot outside motion. |
| `rootsh.py -s <serial> run [-c <script>] \| status \| stop` | The serial's persistent root shell: a host-side broker holding one `adb shell su -c sh`, started by the first `run`, ended by `stop`, after 30 idle minutes or when the device goes. `run` executes the script (default: stdin) in its own `sh -c` and passes its stdout, stderr and status through (124: no answer in time, 125: no root shell). The shell pins itself to the slowest cpufreq cluster (`status` prints the cores). |
| `live.sh -s <serial> set KEY=VALUE \| unset KEY \| mark <text>...` | Live switches (APP.md *Live switches*): atomically replaces `<game>/totk.ctl` with only the new batch (one generation at the next presented frame) and prints the game's `[totk-ctl]` answer (a mark's acknowledgement, refusals); exit 3 when a mark is not acknowledged, 4 when a line was refused. |
| `ab.sh -s <serial> --device <label> --scene <label> --arms 'A:<lines>;B:<lines>' [--order ABBAABBA] [--pan-seconds 30] [--settle 15] --out <dir>` | In-session A/B in one loaded scene: per block, the arm's lines plus `mark ab arm=<X> block=<n> scene=<scene> direction=<right\|left>` (live.sh), a settle, a pan (direction alternating). Writes `<dir>/arms.txt` with the direction recorded. |
| `sampler.py -s <serial> --device <label> --out <dir> [--duration S]` | Every 3 s: newest frame, max CPU/GPU zone temperature, cooling states, GPU `thermal_pwrlevel`/clock/`max_gpuclk`, cpufreq `scaling_max/cpuinfo_max`, MemAvailable, the game's VmRSS / VmSwap / RssShmem, its kgsl memory (`/sys/class/kgsl/kgsl/proc/<pid>/{kernel,gpumem_mapped,imported}`), PSI cpu / memory some / full avg10 and kswapd CPU % → `<dir>/samples-<label>.txt` (values a device lacks stay empty → null). Stops on Ctrl-C / SIGTERM / `--duration` and removes its device loop. |
| `collect.sh -s <serial> --device <label> --out <dir> [--mark]` | `--mark`: dmesg marker (after the scene settles). Default: pulls `logs/<label>.log`, `logs/<label>-dmesg.txt` (from the marker), `settings-<label>.env`, `facts-<label>.txt` (model, SoC, GPU, Android, displays, app version, APK sha256, serial hash), warns about a developer `totk.env`, prints the GPU-fault check. Exit 3 when there is no `totk.log`. |
| `analyze.py <dir> [--json <file>] [--name <name>] [--commit <sha>] [--meta <json>] [--arms]` | Per device × scene: the 300-frame windows that started after the stick was pushed and ended before it was released (end frame in `[F0+600, F1]`), thermal state over each pan, the frequency limits of each pan and whether they stayed put (`freq_stable`), each device's frequency signature, a results table and caveats; `--json` writes schema `totk-android-benchmark/1`. `--arms`: each window goes to the live-switch generation that covered it whole and that generation's arm (windows straddling a change are dropped); per arm pooled metrics, per block metrics and the same-arm spread (`ab` in the JSON). With the sampler's memory fields, each window, run and arm also gets `memory`: MemAvailable minimum and the maxima of the rest. |
| `compare.py <reference.json> <new.json>` | Per device × scene: fps mean/min, p50 median, p95 max, worst frame, GPU ms mean, long ≥45 ms, >50 ms, max temperatures, with deltas and verdicts (thresholds in `compare.py --help`); labels each device pair by frequency signature: match / reference only / NOT COMPARABLE; warns when `settings_env` or cpufreq caps differ or caps changed during a run. Exit 1 if a scene with a matching signature is worse. |
| `abtest.py <ab.json> [--ref A]` | Each arm of an `analyze.py --arms` result against the reference: pooled and same-direction ABBA paired deltas, per-direction deltas, sign agreement, exact sign p-value, tolerance, informational same-arm spread and verdict. Better/worse requires both deltas beyond tolerance plus unanimous pair signs (or p < 0.05 with ≥6 pairs); paired-only crossings are informational leans. Prefer ABBAABBA or longer at 60 fps; full rule in `--help` / the skill. Exit 1 if an arm is worse. |
| `ledger.py add <result.json> --commit <sha> --apk <apk> --profile <text> [--note ...]` | Append a run to `docs/benchmarks/ledger.jsonl` (+ `docs/benchmarks/runs/<run-id>/<device>.json`), tied to the clean commit the APK was built from (its `assets/build.txt`); compared with the baseline of its frequency signature, or, for a signature seen for the first time (with stable caps), recorded as that signature's NEW baseline (`docs/benchmarks/baselines/<device>/<signature>.json`, pinned, indexed); re-renders `docs/benchmarks/LEDGER.md`. Also `render`, `show <run-id>`, `compare <run-id> <run-id>`. |
| `check_baselines.py` | Fails if a baseline file (`docs/benchmarks/baseline-*`, `docs/benchmarks/baselines/**`) differs from `docs/benchmarks/BASELINES.sha256` or is unpinned, or `docs/benchmarks/baselines/index.json` is inconsistent (also run by `tests/test_bench_tools.py`). |

Baselines are immutable and per frequency signature (device model + SoC, settings.env hash, per-cluster CPU caps,
GPU cap), listed in `docs/benchmarks/baselines/index.json`. Only `ledger.py add` creates one (for a new signature);
`analyze.py`/`compare.py` refuse to write any baseline path. `baseline-1.0.0.json` is registered there without a GPU
cap, so it gives reference-only comparisons.

### Pacing states, not just a session average

`analyze.py` uses effective menu/developer settings (`TOTK_FPS60=1` with `TOTK_UNCAPPED=1` and
`TOTK_UNCAPPED_HZ=40`: 40 target; otherwise FPS60 `1`: 60, `0`: 30), recorded pan target, or explicit
`--target-fps 30|40|60`. Unknown targets remain unclassified, never silently treated as 60. At 60, a window with
at least 60/300 frames in 29–37.5 ms is a dip: complete frame durations take priority, then the complete
present-fence interval histogram. Sparse `[长帧]` / `[frame-tl late]` traces only establish lower bounds; otherwise
the fallback is window fps below 5/6 of target (below 50 for 60). A normal 30-target 33 ms window is steady.
Fence signals are a framework pacing proxy, **not guaranteed hardware scanout** and not proof of controller causality.

The JSON and console separate `fps_steady` (mean of non-dip window fps), `dip_windows`, `dip_time_fraction`
(sum of dip-window seconds / classified-window seconds, using 300/fps), `frames_near33`, and `fps_mean`
(legacy arithmetic window mean). `fps_time_mean` is the actual throughput over those complete windows, i.e.
total frames / total time; neither mean includes warm-up, dropped edge windows or screenshot time. Missing frame
counts remain null; `frames_near33_lower_bound` and coverage describe sparse traces. Do not infer counts from fps or
from the stutter line's >33 ms total, which misses frames just below 33 ms.

A/B comparisons report steady fps and dip time independently; a dip-time difference above 0.05 (five percentage
points) says **verdict confounded by dips** rather than declaring a speed win from mean fps. All-dip arms have no
steady-fps estimate. Historical JSON without pacing data is labelled unavailable and retains legacy comparisons;
re-analyze its original logs with the preset target to obtain a pacing-aware comparison.

**Uncapped mode:** effective `TOTK_UNCAPPED=1` is detected by reading collected `settings-<device>.env` followed by
`totk-env-<device>.txt` overrides; `analyze.py --uncapped` explicitly forces this interpretation for all devices in
an older capture. It does not change the device's settings. Windows are labelled `pacing_state=uncapped` and
summaries report `uncapped=true`, `speed_mode=uncapped`, `dip_metrics_valid=false`; dip counts/fractions and
`fps_steady` are null, not a statement that a 40fps uncapped run is entirely in a dip.
`throughput_fps` is total complete-window frames divided by their summed time (300/fps per window), and is the
primary speed metric in both `compare.py` and paired `abtest.py`. It is also emitted informationally for capped runs.
Uncapped comparisons use frame p50/p95/p99 window maxima, worst frame and >33/>50ms rates as primary tail metrics.
They never apply the dip-confounding veto; mixed capped/uncapped comparisons are inconclusive.

Frame-time tails remain independent of presentation-fence tails: each window retains `p50_ms`, `p95_ms`, and
`p99_ms` from the game's `帧间隔 至帧 ... p99` line (`p99_samples` preserves its sample count). No p99 is invented
when that line is absent, and a present-fence p99 is never substituted. Per-pan/arm/run summaries include
`p50_ms_max`, `p95_ms_max`, `p99_ms_max`, `p99_windows`, `over33_total`, `over50_total`, and `stutter_windows` coverage.
`p99_p50_ratio` per window / `p99_p50_ratio_max` summarize relative frame-time spread; `fps_stddev` is the population
standard deviation across included window fps values. Spread metrics are informational, with throughput and tails
reported separately so identical ~40fps means with different tail spikes do not collapse to one speed-only verdict.

Draws are a **cumulative** backend counter: `draws_per_frame` uses consecutive 300-frame sample differences,
`(D_now - D_previous)/(frame_now - frame_previous)`, never `D_now/300`. First samples, counter/frame resets and
missing intermediate samples have no rate (null); the next complete monotonic pair resumes measurement.
`draws_per_frame_mean` and `draws_windows` report the mean and coverage over usable windows.

### Presentation tails, bins and judder

Each selected window retains `[nvn-vk]` `recording_ms` and the framework present-fence cadence line's
`present_p95_ms` / `present_p99_ms`. Every `pans[].summary`, run summary and live-switch arm/block summary includes
`recording_ms_mean`, `present_p95_ms_max`, `present_p99_ms_max` and their window coverage. The latter are **maxima
of the reported window percentiles**, not reconstructed pooled p95/p99; percentiles cannot be pooled from quantiles.
These fields are informational and do not silently change A/B verdict tolerances.

Always-on AHB bins are `<12.5 / 12.5–20.9 / 20.9–29 / 29–37.5 / 37.5–45.8 / >=45.8 ms`.
`present_interval_bin_counts` sums selected windows' observed pairs; `present_interval_bin_shares` divides by the
total observed pair count, not by 300 or by window count. `present_judder_proxy_fraction` is the **20.9–29 ms bin
share**, only a proxy: the bins cannot recover an exact >16.7 & <30 ms share. Missing histogram coverage is explicit.
Cadence/histogram records are associated with the latest runtime FPS-window marker in the log, as for pacing-state
analysis; asynchronous fence-result blocks are not claimed to be exact physical pan-boundary measurements.

For exact timestamp arithmetic in a **separate diagnostic run**, set `TOTK_PRESENT_CADENCE_TRACE=1` in `totk.env`
for both arms and restart. `[present-trace]` records carry immutable `generation`, transaction `id`, `source_frame`
and `display_ns`. The analyzer sorts/join consecutive IDs within the same generation, requiring both framework fence
timestamps to be present and increasing; it never joins callback order, bridges missing IDs or crosses generation
boundaries. Intervals belong to included frame windows only when both source endpoints lie within that window
(allowing its preceding boundary frame). Duplicate conflicting identities are excluded.
`present_judder_exact_fraction` counts strictly **16,700,000 < delta_ns < 30,000,000**, with explicit
`present_judder_exact_hits` / `present_judder_exact_pairs`: exact over the **observed valid trace pairs**, not an
estimate for missing pairs or proof of physical scanout. It is null without valid traced intervals.
`TOTK_FRAME_TIMELINE=1` logs only selected late per-frame intervals and is not a substitute for this full trace.
Cadence tracing is high-volume and has measurably perturbed Thor timing: keep it off in ordinary performance arms,
do not mix traced/untraced arms, and use independent distinct-picture evidence for displayed smoothness.

### Long pans and exact GPU residency

The target defaults to the active preset (`settings.env`, then developer `totk.env` override); `--target-fps`
overrides detection. Unknown targets use 48 s and omit target metadata rather than incorrectly labelling a 60 run.
The 40 fps menu choice is detected as 40 rather than 60 despite `TOTK_FPS60=1`, and defaults to a 144 s pan.
For 60-target measurements use `pan.sh --target-fps 60 --out "$OUT" ...`: the default is a continuous **144 s**
pan (72 s each direction), long enough for >=12 complete 300-frame windows even during 30-interval stretches.
`--seconds N` explicitly chooses another length; `--repeat N` runs back-to-back return pans, each with independent
boundaries. `--target-fps 60 --seconds 48 --repeat 3` is the recommended shorter-camera-cycle alternative.
Keep at least 12 **included** windows per session/arm; the analyzer warns for shorter 60-target scene records.
If a prolonged slowdown leaves fewer, lengthen/repeat rather than accepting a short run. Use the same duration,
cycle length and direction schedule in both arms; explicit `ab.sh --pan-seconds 144` keeps its own block schedule.

Every pan invokes `sampler.py --gpu-snapshot` just before and after motion via the persistent root shell, storing raw
cumulative counters in `gpu-time-in-state.jsonl`. The first supported source is KGSL/devfreq `trans_stat`, then the
KGSL `gpu_clock_stats` equivalent (or devfreq `time_in_state`). `analyze.py` reports each pan's frequency counter
deltas and fractions, not a histogram of 3-second instantaneous clock samples. `trans_stat` uses the final residency
column, never its transition matrix; fixed-width concatenated headers are supported. Counter units are preserved
(`time(ms)` means milliseconds; an unspecified unit stays unknown), and frequencies are Hz.
KGSL `gpu_clock_stats` is a vector in `gpu_available_frequencies` order and counts **active clock time**: its fractions
are an active-frequency distribution, not wall-time residency. Boundary reads bracket the pan but include small
host/control latency; timestamps/uptimes and raw text remain available for that audit.
Missing/failed snapshots, source/frequency/unit/boot changes, actual per-frequency counter regressions and
non-advancing counters produce unavailable residency, never fake zeroes. KGSL `reset_count` is **not a devfreq
statistics-reset generation**: Qualcomm's [reset attribute](https://github.com/qualcomm-linux/kgsl/blob/d70a6765a318eda3e400c711587ce373c693c99e/kgsl_pwrctrl.c)
returns `device->reset_counter`, incremented by [Gen8 GPU boot](https://github.com/qualcomm-linux/kgsl/blob/d70a6765a318eda3e400c711587ce373c693c99e/adreno_gen8_gmu.c)
including normal wake after slumber (and recovery). A change alone does not invalidate monotonic residency.
The raw start/end values and `reset_count_delta` remain diagnostic; they do not establish a GPU fault count.
This upstream evidence is not a claim that the Odin's exact vendor kernel source was identified.

For declared `s`/`ms`/`us`/`ns` counters, `time_sanity` records the converted total and device-uptime read bounds.
Reject a total exceeding the elapsed upper bound plus `max(1 s, 5% of elapsed)` (accounting granularity/clock
tolerance). Do not impose a lower bound: devfreq suspension / active-clock counters may legitimately cover less
than wall time. Unknown units retain raw fractions with an explicitly unavailable time-sanity check, never a guessed
unit conversion. Older logs without boundary snapshots cannot recover exact residency.

Sampler `external_power` is **any readable online non-battery kernel supply** (JSON boolean, null when no online
observation exists). It is not Android BatteryService's AC/USB enum: the Odin can expose sysfs `usb=1` while
`dumpsys battery` calls the same external power AC. Each `supply.<name>.type` / `supply.<name>.online` token is
retained under per-pan `power_supplies`; legacy `ac` / `usb` remain raw kernel-type category observations, never
relabeled to match dumpsys. Battery/BMS supplies are excluded from external-power aggregation. `level` is battery
percent. Historical rows derive external_power from available raw ac/usb fields; no readable source stays null.
Pan JSON retains timestamped fields rather than inferring charger state from temperatures.

Android toybox `chcon` does not support GNU `--reference`. `live.sh` reads `CTX=$(ls -Zd "$GAME")`,
extracts its first field (including the full `s0:c...` MLS categories), then runs `chcon "$CTX"` on the fully
written temporary control file before atomic rename to `"$GAME/totk.ctl"`. Missing context or failed chcon
aborts publication without replacing the previous batch. There is no totk.env writer in these tools;
when writing one manually, apply the same game-directory owner/mode/context procedure to `"$GAME/totk.env"`,
not a generic `app_data_file:s0` label that discards its categories.

Example (one device, one scene; placeholders in `<>`):

```sh
OUT=.scratch/bench/<run-name>
tools/bench/bench_input.sh -s <serial> freqs                               # the caps you are about to measure
tools/bench/collect.sh -s <serial> --device <label> --out $OUT --mark     # after the scene settled
python3 tools/bench/sampler.py -s <serial> --device <label> --out $OUT &  # keep running for the whole session
tools/bench/pan.sh -s <serial> --device <label> --scene hateno --target-fps 60 --seconds 48 --repeat 3 --out $OUT
kill %1                                                                    # stop the sampler
tools/bench/collect.sh -s <serial> --device <label> --out $OUT
python3 tools/bench/analyze.py $OUT --commit <sha> --json $OUT/result.json
python3 tools/bench/ledger.py add $OUT/result.json --commit <sha> --apk <apk> --profile "<preset> (default)" --note "<why>"
```

## GPU pass profile (host-only analysis)

`python3 tools/bench/gpuprof.py 'capture/gpuprof-*.jsonl*' [--json analysis.json]` reads **schema-3 only**
GPU-profiler rows, including the three `.1`/`.2`/`.3` rotations. Normal analysis rejects schema 2 because its
attachment ratios used different reference semantics. `--json -` emits JSON only on stdout (`totk-gpuprof-analysis/3`).

Analysis defaults to **whole-input/measured-interval aggregation by exact key and structural family**,
not one estimate per 300-frame window. Optional `--per-window` adds `per_window` JSON entries and a text
breakdown while retaining those interval tables. Use a capture containing only the intended warmed measured
interval when interpreting a result; missing/evicted files are censored coverage, not zero observations.

### Sampled queries and interpretation

`TOTK_VK_GPU_PROFILE_SAMPLE` defaults to **`0`**, including unset: existing full-mode behavior is unchanged.
With `TOTK_VK_GPU_PROFILE=1`, `2` or `3`, a strict integer budget `1`–`16` enables deterministic rotating
pass-ordinal sampling. Recommended initial capture: `TOTK_VK_GPU_PROFILE=1`,
**`TOTK_VK_GPU_PROFILE_SAMPLE=2`**, `TOTK_VK_GPU_PROFILE_STATS=0`, with the normal recorder workers.
Every observed pass still contributes its key, draw count, absolute attachments and scene/reference metadata;
only selected passes have timestamps/statistics. Rotation is not randomized; inspect key coverage across the
whole measured interval and extend captures when keys or frame ordinals are thin.

Sampled pass rows add `sampling: {budget, observed_count, sample_count, observed_frames, sampled_frames,
histogram}`. Existing `count` is still observed count; `gpu_ms` is **sampled duration sum only**.
No synthetic total is extrapolated to unmeasured passes. Zero-sample keys remain present with zero measured
sum, null mean/percentiles and absent counters; that is not evidence that the pass costs zero.
`mean_ms = gpu_ms / sample_count`; `p50_ms`/`p90_ms`/`p99_ms` come from merged histogram counts,
**never averaged per-window percentiles**. The 272 bins use eighth-octave nanosecond upper bounds
`2^(i/8)` (zero and values ≤1 ns in bin 0). Above 1 ns the reported percentile is a bucket **upper bound**,
within about 9.1% quantization, not an exact measured percentile. The final bin saturates.

Group `sampling` reports count/frame coverage, histogram and budget(s). Exact distinct frame unions are
reported only where recoverable; families with overlapping keys expose `*_bounds` instead of summing
frame counts as if disjoint. Thin-coverage warnings use the **whole interval**: fewer than 10 sampled
frames, not just three samples in one 300-frame window. There is **no hard percentage gate**:
15–30 samples/ordinal in 48 seconds can be useful even near 1% frame coverage; the fraction and
denominator remain visible. Below 100 samples, p99 is descriptive-only, not a reliable stutter-tail
estimate; bucket precision does not establish statistical confidence. Uncertain unions are labeled uncertain.

Frame rows emit `sample_budget`, `observed_passes`, `sampled_passes`, `timing_scope: "main-frame"` and
**actual profiler-owned `vkCmdWriteTimestamp2` command `timestamp_count`**, including deferred assembly
ends but excluding the game's independent timestamp reports: at least the whole-main pair and at most `2k+2`.
`report.sampling.frames` retains each frame's actual count and bound; timing summaries include the sum,
min/max and bound totals. The whole-main-frame pair remains completion-accurate `ALL_COMMANDS`,
without pre/slot envelopes; full-mode spans can include pre work and are not interchangeable.
If Android has already sealed/submitted the final command buffer when CPU present runs, SAMPLE adds
**one small marker-only main submission** for the actual end timestamp; the start was recorded earlier.
The extra submission has explicit cost even though the timestamp bound is still `2k+2`. An open command
buffer retains the existing in-buffer marker. Full mode uses its last slot's real envelope end with no
additional GPU query or submission. Neither mode substitutes a CPU timestamp or a per-slot sample end.
Sampled frame/window `measured_ms` sums sampled durations; signed `gap_ms` includes intentionally untimed
work but may be negative when sampled pre work precedes the main span or scopes overlap. It is not total
untimed work. A negative sampled residual alone is not span failure; full-mode reconciliation is unchanged.
The analyzer suppresses complete-pass reconciliation comparisons for sampled windows, not invalid span,
query availability or timestamp-domain checks. Statistics coverage uses sampled count, not all observations;
time/draw ratios use only explicitly matched statistics draws/time, never all observed draws.

Sampling lowers query volume, not guaranteed perturbation: remaining `ALL_COMMANDS` timestamps can stall
the GPU, and statistics still perturb the passes selected. Sampled duration representativeness is conditional
on instrumentation **not serializing or otherwise changing GPU execution**. Device A/Bs must compare GPU
clock **time-in-state deltas** over equal intervals with an **unprofiled control**, plus fps, power and
temperatures; a point MHz reading or lavapipe timing does not establish mobile overhead.

The profiler's draw/pass CPU budget is O(1): count each draw, hash at most six bounded shader
identities on the first draw, and capture at most ten attachment records at pass begin. It no longer
scans texture/storage-image bindings, looks up/copies the texture registry, searches prior-pass reads,
allocates audit arrays or logs liveness candidates, including on unselected passes. Static attachment
LOAD/CLEAR/STORE/DONT_CARE operations and logical byte estimates remain pass-key-derived summaries,
not a speculative liveness audit or hardware traffic counter. The dedicated `totk-gpuprof` worker
owns query result reads, aggregation, formatting/output and resets; its sixteen prepared leases are
bounded. One-time opt-in startup preparation can wait, but exhaustion during play drops coverage
(`background_dropped_slots`, cumulative, plus missing frame coverage), never waits for profiler I/O.
CPU present has an independent ordered closure FIFO (256 entries, header `background_closures`).
Its `{frame, end_sequence}` events finalize only after the worker has processed all fenced leases through
the sealed submission sequence, including closures published after those leases were consumed.
This handles Android's sealed-then-present ordering and the last frame without needing a next-frame marker.
Cumulative `background_dropped_closures` records closure overflow separately from lease drops; both are
per-launch maxima in analysis, warn and censor coverage. Footer `pending_frame_closures` is retained;
zero pending CPU events does not prove all GPU queries retired.

**Attribution correction:** `.scratch/bench/rpmode/odin-overhead/report.md` measured old APK
`716c122ff09b` timestamps-only P1 at 21.55 fps versus 57.96 fps C0, recording
43.750 versus 10.789 ms/frame, and GPU busy 36.6% versus 80.2%. The source's old per-draw
2×160 binding scan, registry mutex/copy and all-prior-pass read audit are CPU overhead candidates;
the report does not isolate each mechanism's cost. Turnip `ALL_COMMANDS` timestamps still issue
WFI, but that source fact is **not** proof that GPU serialization dominated the observed FPS loss.
No post-cutover device improvement is established by host timing or this source analysis.

### Identity, retention and full-mode accounting

Windows now default to **one row per exact key + reference extent + scene role**, with `ordinal: 0`
meaning **all frame ordinals**, displayed as `all (0)`. `TOTK_VK_GPU_PROFILE_ORDINALS=1` restores
per-ordinal windows (default `0`, read once; startup logs `[gpu-profile] window ordinals=0/1`).
Mode-2 raw `[gpu-split]` logs and mode-3 selected `long_frame` rows always retain their actual positive
frame ordinals. Window `ordinals` records the selected policy; old schema-3 captures without it remain readable.
Counts, times, absolute dimensions and statistics counters/explicitly matched time/draws remain unchanged sums.

Default storage is **128 MiB per file × four files**, overridable with `TOTK_VK_GPU_PROFILE_MAX_BYTES`
(4096–1073741824). Compact mode targets a 6–8-minute pan without rotation, not a duration guarantee.
Each file begins with a `header` containing launch ID, rotation index, build commit from APK `assets/build.txt`,
package/versionCode when available, Vulkan device/driver identity, effective profiling switches and a whitelist
of requested launch environment values (null means unset, not necessarily an effective backend default).
Host fixtures/missing APK metadata explicitly retain unavailable identity; no environment build string is trusted.
Exact APK/driver artifact hashes must be joined from capture records, not inferred from version labels.
Orderly Android close/destruction stops new renderer-work admission, flushes/drains renderer GPU work,
then drains/joins the profiler reporter before emitting `session_end` and closing the writer.
`clean_end` records writer success independently of query retirement. New footers always carry boolean
`all_queries_retired` and a `query_retirement` object with nonnegative integer fields:

| Field | Meaning |
| --- | --- |
| `leases_acquired` | Prepared leases taken for recordings during this launch. |
| `leases_submitted` | Acquired leases attached to submitted work; excludes cancelled recordings. |
| `leases_retired` | Fenced leases whose results have been read/attempted. |
| `leases_cancelled` | Acquired leases abandoned without submission. |
| `leases_outstanding` | Acquired leases neither retired nor cancelled. |
| `queries_submitted` | Activated timestamp/statistics query instances in submitted work. |
| `queries_retired` | Submitted query instances with available read results (successful statistics reads). |
| `queries_cancelled` | Activated queries in abandoned, never-submitted recordings. |
| `queries_outstanding` | Submitted query instances not retired, including unavailable results. |

Queries count activated instances, not reserved whole-pool capacity or six statistics counters per query.
The analyzer requires `leases_acquired = leases_retired + leases_cancelled + leases_outstanding`,
`leases_retired <= leases_submitted <= leases_acquired - leases_cancelled` and
`queries_outstanding = queries_submitted - queries_retired`. `all_queries_retired` is true iff both
outstanding counts are zero. A retired lease does not guarantee all its query results were available;
clean writer closure can coexist with false retirement. `sessions` JSON and rendered text retain the
counts and retirement status independently from `clean_end`. Historical footers without `query_retirement`
and with nullable `all_queries_retired: null` remain readable as unknown, without fabricated counts.
Force-stop/crash/kill normally omits this marker.
Enabled GPU draining uses one two-second budget. The additive boolean `query_drain_timed_out` records
expiry; uncompleted leases and unread queries remain outstanding. Analysis retains/renders the flag
separately, or unknown when absent historically. Profiling-off shutdown remains a no-op without renderer
flush/GPU wait, not a new synchronization point.
The existing analyzer already flags absent session ends and preserves retained complete-window timing;
missing closure is expected after force-stop, not evidence of invalid timestamps. Never synthesize an
end marker or infer unobserved trailing frames from it. The old Odin P1/P2 captures both lack an end
after final force-stop, so only their retained completed-window timing is established.

Capacity model (not a new device-duration measurement): six original Thor S/D/B1 snapshots
(`.scratch/bench/rpmode/thor/{S,D,B1}`, initial and `after-hateno`) contain 77 complete windows,
463,121 per-ordinal pass rows and 11,736 window/key proxies after ordinal coalescing (39.5× fewer).
The largest observed proxy population is 265/window. Serializing those coalesced legacy templates
with an extra 256 bytes/row + 128 bytes/attachment for schema-3 metadata gives a peak 334,477 bytes/window.
At 60 fps for 480 seconds, 700 bytes/new frame × 28,800 frames + 96 peak windows + 64 KiB metadata
is 52,335,328 bytes (49.91 MiB). Schema 2 cannot recover actual schema-3 absolute/reference variants;
allowing **2×** the modeled pass bytes gives 84,445,120 bytes (80.53 MiB), hence the 128 MiB default
rather than a marginal 64 MiB. Arbitrary key/reference churn or raw/long-frame populations can exceed this model.

Pass and selected `long_frame` rows require absolute `render_width`/`render_height`,
`reference_width`/`reference_height`, and boolean `scene_relative`. Every attachment retains its slot, format,
load/store and reduced `width_num`/`width_den`, `height_num`/`height_den`, plus absolute `width`/`height`.
The analyzer checks that each rational exactly equals attachment dimension / reference dimension, not attachment
dimension / pass area. References are the completed frame's largest depth+MRT render area (at least two active
color slots), or the game-config fallback when no such scene is observed: `TOTK_RENDER_RES`, otherwise handheld
1280×720 or docked 1600×900. References and attachment dimensions are positive uint32; render dimensions are
positive uint32 for render scopes and may be paired 0/0 for nonrender scopes. Frame rows require
`scene_width`/`scene_height`: the observed largest depth+MRT render area, or 0/0 when absent.

Exact producer keys hash **absolute** render and attachment dimensions along with scope kind, formats,
load/store, samples and shaders; changing scene size changes its exact key, but not a fixed shadow's key.
Per-pass JSON retains the exact key/ordinal, absolute `render_width`/`render_height` and attachment `width`/`height`
for driver joins. Both pass and family tables expose every contributing `areas` variant with its exact `keys`,
absolute render/attachment dimensions, reference dimensions, attachment rationals and `scene_relative` flag.
Multiple reference extents in one window are distinct observations, not duplicate/conflicting rows, and are
preserved even when their absolute key is the same.

Families ignore shader/key/ordinal but retain kind, samples, slots, formats, load/store and `scene_relative`.
For `scene_relative=true` (depth+MRT geometry, or depth render areas equal to that frame's observed scene),
families compare exact render/reference and attachment/reference ratios, merging matching DRS geometry and
scene-sized depth prepasses across extents. A half-scene MRT remains distinct from a full-scene MRT, including
when both occur in one frame. Fixed depth shadows, color-only passes and all other `scene_relative=false`
scopes compare **absolute** render/attachment dimensions; two depth-only target sizes do not collapse together.
Times and counters are **summed before dividing**, never averages of producer ratios or per-frame ratios.
Unavailable statistics stay unavailable. Time/draw ratios use only complete-stat rows or explicitly matched
statistics draws/time so their denominators match; partial-stat rows still contribute their available counters
to counter-to-counter ratios. In sampled mode all-observed draws are never a duration denominator.

The report includes measured/span/gap sums, absent frame IDs within the captured range, missing window summaries,
complete missing window sequences, rotation eviction/absent files, absent end markers, pass/window sum mismatches,
dropped/failed slots, partial windows and malformed/incomplete/duplicate/conflicting rows. `sessions` retains every
header/end observation. Separate launch filenames remain separate observations; repeated rotation rows count once,
while conflicts are excluded rather than choosing an arbitrary value. Dedup retains exact key, ordinal, reference
dimensions and `scene_relative`, so compact rows do not merge distinct scene roles/references.
Headers establish sequence 1 as the launch start; older headerless captures only prove interior window gaps.
Missing trailing work after an unmarked exit cannot be inferred. A clean writer end is not a flush of pending queries.

Invalid frame/window spans have `span_valid: false` and null `span_ms`/`gap_ms`; reasons, unavailable/invalid
timestamp counters and raw forensic spans remain explicit. The analyzer excludes implausible older positive spans
above 10,000 ms/frame even if the old producer reported success, and propagates invalid frame authority to its window.
`timing` preserves measured sums and valid-span/gap subtotals; total span/gap stay null if that population contains
invalid rows. Invalid timing is not zero GPU time. `rejected_spans` preserves raw values and source locations.

For historical timing-only replay:
`python3 tools/bench/gpuprof.py --legacy-span-replay 'capture/gpuprof-*.jsonl*' --json replay.json`.
This explicit mode accepts schema-2/3 **frame/window timing only**, ignoring pass/long-frame rows and disabling
pass/window sum comparisons. It flags implausible spans without reconstructing query availability/endpoints or
claiming schema-3 attachment/reference semantics. Valid historical timing values remain unchanged.

### Counterfactual d003f2d CPU frame-close replay

```
python3 tools/bench/gpuprof.py --frame-close-replay 'capture/gpuprof-*.jsonl*' --json replay.json
```

This option adds `frame_close_replay` (`totk-gpuprof-frame-close-replay/1`); it **does not alter ordinary
analysis or rewrite inputs**. `coverage`, `timing`, `rejected_spans` and pass sums preserve original
invalidity. The new section retains input SHA-256/byte counts, source locations and the complete original
frame/window rows, exact `eligible_frame_ids`/`rejected_frame_ids` per stream and window, rejection
reasons, and conditional `counterfactual_span_ms`/`counterfactual_gap_ms`. Text output also exposes
the exact IDs and window decisions. It is incompatible with `--legacy-span-replay`, which omits the
pass authority required here.
Rows excluded by ordinary schema or launch-mode validation stay excluded: their source issues and
malformed-row counts remain authoritative. Replay does not resurrect those rows or fabricate frame/window
identities for them. Retained frames whose mandatory window summary was rejected stay unknown/rejected.

The affected APK's `assets/build.txt` header must consistently identify **`d003f2db137c`** with matching
build text/source, full mode and timestamp domain. Eligibility requires a complete canonical 300-frame
window with all contiguous frame IDs and explicit zero unavailable/invalid/dropped/failed/statistics/
output/background-lease counters. The then-nonexistent `background_dropped_closures` counter may be
absent in this old build; a nonzero value still rejects. Only original `incomplete-records` frames with
available uint-domain first/last GPU ticks, no wraps, matching header domain, matching forensic raw span,
recomputed interval ≤10,000 ms, nonnegative full-mode gap and consistent frame/pass/window measured sums
are eligible. Partial/missing windows, missing metadata, gaps, failures, real drops and corrupt timestamps
remain rejected; absent authority is **unknown**, never invented. A bad window-wide failure counter
rejects all its frames because its exact affected lease extent is not captured. A counterfactual window
sum is emitted only if every frame is eligible. Sampled missing endpoints cannot be reconstructed.

The source bug was that Android submitted the final slot before outer CPU present, leaving no command
buffer for the old in-buffer frame-end marker. The following frame transition then applied the
`incomplete-records` guard to normally CPU-closed work. The ordered CPU closure FIFO fixes this path;
it does **not** remove missing-query/drop/incomplete-record guards. The old host fixture kept a command
buffer open at present and therefore missed the Android ordering. Historical aggregates do not record
individual lease/CPU-close events: even eligible replay results are **conditional on a CPU-closure-only
hypothesis**, not proof of no unrecorded work. APK-header attribution and hashed capture bytes are not an
independently measured APK hash. Keep original writer-end/query-retirement uncertainty. Never call these
results device-corrected timings, a post-fix device verification, or a device performance improvement.


`long_frame` tables describe over-threshold frames and retained-history diagnostics; they are never added to window
totals or treated as an unbiased latency distribution. `TOTK_VK_GPU_PROFILE_LONG_FRAMES` bounds the history ring,
**not** the number of over-threshold frames dumped; a zero threshold selects every valid frame. Exit 1 means
malformed/conflicting rows; a partial final JSON line is a reported capture warning. Exit 2 means an I/O error.

`tests/test_gpuprof.py` pins schema-3 dimensions/references, scene-relative and fixed-depth families, ratios,
compact ordinal sentinel/dedup/statistics totals, capture-loss lifecycle metadata, strict retirement-count/
boolean consistency, clean-writer/retirement independence, historical unknown footers, legacy span replay,
optional statistics, rotation and long-frame accounting without Vulkan. Sampled unit fixtures pin strict histogram/
coverage validation, merged means/percentiles, zero-sample metadata, per-frame timestamp counts, interval
thin warnings and optional per-window output, without changing full-mode output.
The explicit CPU-close replay fixtures pin opt-in-only behavior, original invalidity/provenance, complete
frame/pass/window consistency and rejection of missing build/domain/endpoint authority, gaps, true query
failures, background losses, corrupt timestamps and negative full-mode gaps.
The optional `GPUPROF_TEST_BIN=<absolute vkgpuproftest path>` enables fresh-process real Vulkan runs for
modes 0/1/2/3, recorder workers 0/2, statistics on/off/unsupported, ordinal windows off/on, split-frame and
unsent multi-frame reconciliation, direct pre uploads while main rendering is open, default row/byte reduction
and bounded file rotation. Every rotated file must carry its header, and orderly fixture exit its writer-end marker.
The `--sampling` fixture renders both stable extents in every frame so deterministic ordinal rotation is not
aliased with alternating-frame keys. The acceptance matrix covers budgets **1/2 × workers 0/2 × stats 0/1**:
all stable keys eventually sampled, all observed metadata retained, counters only on sampled passes,
histogram/count consistency, and actual per-frame `2..2k+2` timestamp writes. It compares the same main-work
authority with full mode using endpoint/domain checks and a generous software timing tolerance, **not**
identical durations; full spans may also include pre work. Exact readback pixels and worker use remain required.
`--sealed-present` adds 302 frames with three main submissions, two pre uploads, two readbacks and a
present GPU scope per frame, all sealed before CPU present with no open command buffer. It alternates
worker-already-consumed and real GPU-event-gated pending submissions. Full mode and SAMPLE budgets
1/2 run with workers 0/2 and statistics 0/1; pass sums, actual timestamp counts and readback pixels remain
authoritative. `--last-closed` requires the isolated final frame to publish after CPU close, before any
next frame or shutdown flush. These are host regressions, not new Android-device measurements.
`--shutdown-inflight` exercises the same real one-frame sealed-present work with its final event-gated
submission verifiably pending when shutdown starts. A separate host thread releases the GPU event;
there is no profiler flush before shutdown. Modes **1/3 × workers 0/2 × stats 0/1 × SAMPLE 0/2** require
the pending-before/drained-after fixture marker, complete frame/pass output, zero pending CPU closures,
true `all_queries_retired` and exact acquired=submitted=retired lease accounting with no cancellations.
Every fresh fixture footer must carry retirement counts; the cancellation scenario must count abandoned
leases and their activated queries separately from submitted work.
Profiling-off runs require that pending work to remain undrained by the shutdown no-op and emit no
profiler file. `--shutdown-timeout` instead holds the event for three seconds, beyond the enabled
two-second GPU-drain budget. Workers 0/2 × SAMPLE 0/2 require the still-pending marker, a clean writer end,
`query_drain_timed_out=true`, false retirement and positive outstanding lease/query counts; incomplete
final frame output is not fabricated into a successful completion.
`--unavailable-span` deliberately leaves a real span-end query in an unsubmitted command buffer;
the existing submitted buffer's fence retires, but the unsubmitted end remains unavailable.
The matrix requires null invalid frame/window spans, explicit unavailable counters and the orderly writer end
with `all_queries_retired=false` and positive outstanding queries, not fabricated query values or a
zero-duration success.
The color-only 64/128 scenario
requires distinct absolute keys and families. Its `--areas` scenario covers changing depth+MRT scenes,
scene-sized prepasses, a smaller MRT later in the same frame, fixed 360×360/128×128 depth targets, and a frame
with no observed scene; all contributing absolute keys and reference variants remain available for joins.
Build `vkgpuproftest` via `games/totk/android/Makefile`; it also builds the existing `vkdraw-seed`.
Set `TOTK_SHADER_SEED_DIR` to that seed's absolute directory and select the host lavapipe loader as for `vkdrawtest`.
The fixture verifies actual pixels and secondary recorder use; it does not establish Android overhead or speedup.
Run host builds/matrices under `flock /home/agent/Projects/totk-android/.scratch/hosttest.lock`
and `taskset -c 0-3 nice -n 10`; no Android device is needed.

## Per-thread CPU profiling (rooted device, explicit opt-in)

`cpuprof.sh` records `cpu-clock` at 750 Hz per running thread (500–1000 configurable),
with frame-pointer call chains. It does not install an APK, change a game setting,
launch the game, or drive the camera. Reserve exclusive profiling access first;
no other tool may change perf sysctls/properties during its capture/recovery.

```sh
taskset -c 0-3 nice -n 10 tools/bench/cpuprof.sh -s <serial> \
  --symbols <release-work>/symbols --commit 3421073 --seconds 60 --hz 750 \
  --out <new-capture-directory>
# After capture.ready appears, wait 5 seconds, then run pan.sh --seconds 48 --no-shot
# in another controller. The profiler uses a separate persistent root broker so
# its bounded record command does not block camera input on the benchmark broker.
```

`build_release.py:138–155` keeps unstripped libraries in its `--work` directory's
`symbols/`; RC8 is commit `34210734b46ffc340ff61f1304f355bf32271bca`.
The capture authenticates installed `assets/build.txt` and every allocated ELF section
against the supplied DWARF/symbol-bearing `libtotk.so` before changing perf controls.
New runtime links also carry a SHA-1 GNU build ID. RC8 predates that link flag:
its no-build-ID fallback binds the verified allocated-section digest, full symbols
digest and installed APK digest to the resulting perf.data digest in `capture.json`.
A rebuild with the same source commit is not sufficient. The Android arm64 binary
comes from NDK `28.2.13676358/simpleperf/bin/android/arm64/simpleperf` by default.
Deployment pushes to a unique shell-accessible flat `/data/local/tmp` pathname,
then root-copies/chmods into its private capture directory; it never asks the
non-root adb shell user to push inside a mode-0700 root directory.

The device EXIT/signal trap restores recorded perf sysctls and the four
property-backed simpleperf controls (including initially empty properties),
then compares a full readback with the durable `controls.before`. The host
independently recovers/verifies before removing its owned directory and lock.
On connection/power loss, retain the host output directory and run
`cpuprof.sh -s <same-serial> --out <capture-directory> --recover`; do not start
another profiler first. A changed boot ID refuses stale writes. Recovery never
removes a different session's lock. `capture.json` records restoration/cleanup;
failed recordings are not successful profiles.

`cpuprof_report.py <perf.data> --symbols <release-work>/symbols --out <report.json>`
re-symbolizes offline using the NDK ReportLib API and rejects mismatched recorded
libtotk build IDs. For RC8, additionally pass `--capture-manifest <capture.json>`;
missing IDs are accepted only after both recording and exact-symbol hashes match
that capture's installed-APK section identity. Rankings retain each TID and group walker, recorders, submit,
game (`g-*`) and other threads. CPU milliseconds use event periods, not sample
counts. Inclusive stacks count a recursive symbol once per sample; they overlap
and are not additive wall-time/critical-path measurements. Frame-pointer gaps,
unknown/JIT guest symbols, sampling overhead and unsymbolized driver code limit
attribution. Do not divide whole-capture CPU by pan-only frame counts.
See [candidate ranking](cpuprof-candidates.md) for the host design gate.

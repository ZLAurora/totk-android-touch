---
name: totk-benchmark
description: Use when measuring TotK Native performance on Android devices, checking a change for performance regressions against the recorded baselines, or recording/reproducing a benchmark ledger entry.
---

# TotK Native benchmark

Measure the game on rooted Android handhelds with the same scenes and method as the recorded baselines, then compare.
All scripts are in `tools/bench/` (paths below are relative to the repository root; quick reference in
`tools/bench/README.md`).

## Scope: rooted Android devices over adb

- **Supported**: Android devices reachable with `adb` (USB or wireless) **with root through `su`** (Magisk, KernelSU),
  the app `com.aure.totk` installed and set up. Root is required by every measuring step:
  - **`sendevent`**: the scene is panned by writing stick events to the built-in controller's `/dev/input/event*` node
    (and menus are navigated with button events); those nodes are root-only. `getevent -p` (controller detection) too.
  - **The app's private files**: the frame timings come from the game's own log, `totk.log`, and the profile from
    `settings.env`, both in `/data/user/0/com.aure.totk/files` (readable only by the app or root). Never rely on logcat:
    some devices' logd drops the game's lines (the Pocket Duo showed none). A file root **writes** there (the live
    switches' `totk.ctl`) needs the game folder's owner and SELinux context, MLS categories included:
    `chown $(stat -c %u:%g <game dir>)`, mode0600 and an explicit `chcon "$CTX" <file>`, where
    `CTX=$(ls -Zd <game dir>); CTX=${CTX%% *}` preserves the whole MLS context. Android toybox lacks GNU
    `chcon --reference`. `live.sh` prepares a same-directory temporary batch before atomic rename to totk.ctl;
    it never republishes old commands. Use the same owner/mode/context procedure for any manual totk.env write.
    Abort on missing context or chcon failure rather than dropping the MLS categories.
  - **Kernel log and sysfs**: the dmesg marker and the GPU/SMMU fault check read the kernel log; the sampler and the
    frequency snapshots read thermal zones, cooling devices, cpufreq policies and `/sys/class/kgsl` (several of them
    root-only on production builds).
  - **The tools' own footprint**: every `su -c` opens a su session, and with Magisk each also starts a su-notify
    `app_process` (on the Duo it crashes: ~0.4 s of CPU, plus ~0.2 s of `crash_dump64`) on any core, the game's
    included; a screencap costs ~1.1 s of CPU. On the Duo those bursts landed in its >50 ms frames. So
    `bench_input.sh`, `pan.sh` and `live.sh` send every root command through ONE persistent root shell per device,
    `tools/bench/rootsh.py` (a host-side broker holding one `adb shell su -c sh`, started on first use, ended by
    `rootsh.py -s <serial> stop` or after 30 idle minutes); `sampler.py` keeps its own one. Both pin themselves, and
    so every command they start (`sendevent`, `grep`, `cat`, `screencap`), to the slowest cpufreq cluster with the
    device's `taskset` (`rootsh.py -s <serial> status` and the sampler's start line print the cores; on the Odin 3,
    whose slowest cluster is six of its eight cores, this only keeps them off the two fastest). `pan.sh` takes its
    screenshot after the stick is released, outside the measured windows (`--no-shot` skips it). A pan then starts no
    su at all (Thor: 6 su sessions and su-notify processes per 20 s pan before, 0 after).
- **Without root nothing is automatic**: `collect.sh` stops with "no root (su)", `sampler.py` with "the root shell never
  started", `pan.sh` / `bench_input.sh` fail with "rootsh: no root shell". There is no non-root source of the frame
  timings, so such a device cannot be measured with this method or added to the ledger.
- **Unknown devices** (anything besides the Thor, Odin 3 and Pocket Duo of the baseline) are handled like this:
  - Controller node: auto-detected as the input device whose `getevent -p` lists ABS_Z, ABS_RZ and BTN_SOUTH (a built-in
    gamepad; an attached USB/Bluetooth pad matches too). If none or the wrong one is found, give
    `--event /dev/input/eventN` to `bench_input.sh` and `pan.sh` (list the nodes with `adb shell su -c 'getevent -p'`).
  - Display: the first display of `dumpsys SurfaceFlinger --display-id` (the internal one on dual-screen devices); give
    `--display <id>` if `shot` shows the wrong screen.
  - Temperatures and throttling: CPU / GPU temperatures come from thermal zones whose type starts with `cpu` / `gpuss`
    (Qualcomm naming); cooling devices of type `cpufreq-*`, `cpu-cluster*`, `thermal-cluster*`, `kgsl`, `gpu`,
    `devfreq-*kgsl*`; GPU thermal power level / clock / cap from `/sys/class/kgsl` (Adreno). What a device does not have
    is recorded as empty sampler fields and `null` in the result (`cpu_max_c`, `gpu_max_c`, cooling state, ...; shown
    as `-`, compared as `n/a`); the frame metrics are unaffected. Look at `samples-<label>.txt` after the first scene.
  - GPU frequency limits: kgsl first, else the first `/sys/class/devfreq/*` whose name contains `kgsl`, `gpu` or `mali`;
    without either the GPU cap is unknown (null) and the signature says so.
  - `collect.sh` records the device facts (model, SoC, GPU, Android, displays, serial hash). The first run of a new
    device (or of new caps / settings) becomes the baseline of its frequency signature (below).

## Baselines: one per frequency signature, immutable

- A **frequency signature** identifies what a run is comparable with: device model + SoC, the hash of the profile's
  `settings_env`, the effective CPU cap of each cluster (cpufreq `scaling_max_freq` per policy, kHz) and the effective
  GPU cap (kgsl `max_gpuclk`, lowered by `max_pwrlevel` / devfreq `max_freq` when those are lower; Hz).
  Tools like ClusterTune, a vendor "performance mode", game boosters or thermal throttling change these caps; the same
  build then runs at another speed, so runs with different signatures are **not comparable**.
- `docs/benchmarks/baselines/index.json` maps each signature to its baseline file, with the caps in readable form.
  - The 1.0.0 release baseline (`docs/benchmarks/baseline-1.0.0.{md,json}`) is registered for its three devices with
    the CPU caps from its sampler records (Odin 3 2227200 / 2246400 of 3532800 / 4320000 kHz, Thor and Duo at the
    hardware maximum) and **GPU cap unknown (null)**. A null GPU cap never matches a known one, so it serves only as a
    **reference-only** comparison (same model, settings and CPU caps), labelled as such.
  - **Auto-baselining**: `ledger.py add` with a device whose signature has no baseline yet, and whose runs all have
    `freq_stable: true`, writes that result as the NEW baseline `docs/benchmarks/baselines/<device-slug>/<short>.json`,
    pins it in `docs/benchmarks/BASELINES.sha256` and adds it to the index. Later runs with that signature are compared
    against it. Nothing else creates a baseline.
- **Never edit, overwrite or regenerate a baseline file or an index entry.** `analyze.py` / `compare.py` refuse to
  write any baseline path; `python3 tools/bench/check_baselines.py` (also in `tests/test_bench_tools.py`) fails when a
  pinned file changed, a baseline file is unpinned, or an index entry does not match its file or fields.
- A run whose caps changed during a pan (`freq_stable: false`: throttling, a tool or the user changed a limit) is
  recorded in the ledger but never becomes a baseline and is not compared; repeat it.
- A measurement round may add a write-up: `docs/benchmarks/<name>.md` + `docs/benchmarks/<name>.json` (in the style of
  the 1.0.0 document plus the `analyze.py` JSON), and always a ledger entry (below).

## Prerequisites

- Host: `adb`, POSIX sh, Python 3. Device: root (see Scope).
- **Test with SELinux enforcing for release/compatibility acceptance.** Record `su -c getenforce` with the device
  facts; a permissive benchmark is performance evidence only, not proof that setup or gameplay works on stock phones.
  Root is for the measuring harness, not a game prerequisite. Never change SELinux mode without explicit approval
  and exclusive device ownership; restore and verify its original mode afterward. For the full setup/runtime/save
  regression and app-scoped AVC collection, follow [SELinux compatibility](../../../docs/selinux-compatibility.md).
- The app (`com.aure.totk`) installed and set up, on the device's **default profile**: in the app's Graphics screen the
  preset marked IN USE is the one marked "Recommended for this device" (not Custom), and there is no developer
  `totk.env` (`collect.sh` warns if there is). Start the game with the app's **Start game** (or PLUS): it rewrites
  `settings.env` from the profile.
- The merged save set (kept outside the repository: `merged-saves.zip` and its sha256 list `merged-saves.sha256`, 32
  files), imported on the device with the app's Saves → Import saves → A zip file → Replace my saves. Scenes:
  | Scene label | Save slot | App's Saves screen | Note |
  |---|---|---|---|
  | `hateno` | `slot_00` Hateno Village | Slot 1 | outdoors in the village |
  | `lookout` | `slot_01` Lookout Landing | Slot 2 | arrives **inside** the shelter next to Monte (indoor) |
  | `rotsumamu` | `slot_04` Rotsumamu Shrine | Slot 5 | outdoors at the shrine |

  The game's own Load list is ordered by save date, newest first, not by slot: pick the scene by its **name**.
- Nothing else measuring or loading the device (no other samplers, screen recorders, or test loops in `ps -A`).
- **Frequency caps as intended**: `tools/bench/bench_input.sh -s <serial> freqs` prints the current limits:
  `cpuN=<cpus>:<scaling_min>:<scaling_max>:<cpuinfo_max>` per cluster (kHz; `scaling_max` is the effective cap,
  `cpuinfo_max` the hardware maximum) and `gpu_maxclk` / `gpu_freqs` (Hz; the cap and the available frequencies, the
  first being the hardware maximum) plus the GPU power levels. A cap below the hardware maximum is usually ClusterTune,
  a performance-mode tile / setting being off, or a game booster: decide whether that is the state you want to measure
  (it becomes part of the signature) and keep it for the whole session.
- Never uninstall the app (it deletes the game data and saves); never change its settings for a benchmark unless the
  run is deliberately on another profile (then say so with `--profile` and in the notes).

## Procedure (per device; devices can run in parallel)

Use one output directory per round, e.g. `OUT=.scratch/bench/<YYYY-MM-DD>-<label>` (gitignored), and a short device
label (`thor`, `odin`, `duo`, or a new one for another device). `B="tools/bench/bench_input.sh -s <serial>"` below.

1. **Wake the device and keep it awake**: `adb -s <serial> shell input keyevent KEYCODE_WAKEUP` and
   `adb -s <serial> shell svc power stayon true` (note the previous `settings get global stay_on_while_plugged_in` to
   restore it). A sleeping device gives black screenshots and ignores input.
2. **Check the profile.** Open the app (`adb -s <serial> shell monkey -p com.aure.totk -c android.intent.category.LAUNCHER 1`),
   then Graphics (`$B press DOWN`, `$B press A`) and `$B shot $OUT/shots/<label>-graphics.png`; look at it: IN USE must
   be the Recommended preset. The main menu remembers its focus: check with `shot` before pressing A.
3. **Detect input/display** once: `$B detect` prints `event=... display=...` (pass them with `--event` / `--display` if
   detection picks the wrong node). **Check the caps**: `$B freqs` (see Prerequisites).
4. **Start the game** with `$B press PLUS` (Start game) and wait for the title screen (~45 s, `shot`).
5. **Load the scene from the game's Load list**: on the title screen `$B press A` opens "Select save data to load"; in
   the world it is PLUS menu → System → Load. Move to the scene by name with `$B press DOWN`, check with `shot`, then
   `$B press A` twice (select, then **Load**). Do not use the app's autoload ("Open the game directly").
6. **Settle 55 s** after the world appears (`shot` the arrival view).
7. **Mark dmesg** (first scene of the session): `tools/bench/collect.sh -s <serial> --device <label> --out $OUT --mark`.
8. **Start the sampler** (first scene; it runs for the whole session):
   `python3 tools/bench/sampler.py -s <serial> --device <label> --out $OUT &` (note its PID if you run the next steps
   from another shell).
9. **Pan**: `tools/bench/pan.sh -s <serial> --device <label> --scene <hateno|lookout|rotsumamu> --target-fps 60 --out $OUT`
   for a 60 preset (default **144 s**, half each direction). The recommended short-cycle alternative is
   `--target-fps 60 --seconds 48 --repeat 3`: three back-to-back return pans. Require >=12 included 300-frame windows
   per session/arm, even if it enters 30-interval stretches; lengthen/repeat if the analyzer warns of fewer. For a 30
   preset use `--target-fps 30` (default 48 s). Keep the duration/cycle/direction schedule identical in both arms.
   Each pan records frequency limits and raw cumulative GPU counter snapshots immediately before/after motion
   (`freqs.txt`, `gpu-time-in-state.jsonl`), then takes a screenshot outside the measured motion. Check `$OUT/shots/`.
   Motion executes as one device-side event/sleep/reverse/sleep/release batch with equal half-duration sleeps.
   Release is the **first operation after the timed second half**, before any final-frame/GPU/frequency reads;
   snapshot latency must never keep the stick deflected. Keep both halves and the first direction identical across
   corresponding sessions. This removes host/control asymmetry, not game camera dynamics or OS scheduling noise.
   INT/TERM waits are interruptible: cancellation stops the owned device command/sleep through an exceptional
   direct-root request and releases both axes, rather than queuing behind the busy broker. Temporary
   `<game>/.bench-pan-<host-pid>` control data is removed on completion/cancellation. A failed batch follows the
   same cleanup; untrappable SIGKILL can only rely on the device batch's finite duration.
   Repeat steps 5–6 and 9 for the other scenes; the sampler stays running across repeated pans.
10. **Stop the sampler** (`kill %1` in the same shell, else `kill <pid>`, or Ctrl-C; it removes its device loop) and
    **collect**: `tools/bench/collect.sh -s <serial> --device <label> --out $OUT`. It must report 0 GPU/SMMU fault
    lines and 0 "device lost"; investigate otherwise.
11. **Analyze** all devices of the round:
    `python3 tools/bench/analyze.py $OUT --name <name> --commit <full sha> --json $OUT/<name>.json`
    (optionally `--meta <json>` to add curated device names / profile descriptions / extra caveats). Check the
    `frequency caps: ..., stable` line of each scene and the printed frequency signature; a `CHANGED` run is not usable.
12. **Compare** with the baseline of the run's signature: `ledger.py add` (below) does it and creates the baseline when
    the signature is new. To look before recording: `python3 tools/bench/compare.py <baseline file> $OUT/<name>.json`
    with the file named for that signature in `docs/benchmarks/baselines/index.json` (or `baseline-1.0.0.json`, which
    gives a reference-only comparison at best).
13. **Record**: the ledger entry (below); for a full round also `docs/benchmarks/<name>.{md,json}` (build, devices and
    profiles, caps, results table printed by `analyze.py`, comparison, caveats).
14. **Restore state**: release the stick (`$B rstick 0`), stop the game (`adb -s <serial> shell am force-stop com.aure.totk`),
    `adb -s <serial> shell svc power stayon false` (or the previous value), delete anything you put in the device's
    Download folder, and confirm `ps -A` shows no leftover sampler. **Saves**: loading a scene can autosave over a slot,
    so restore and verify the merged set:
    1. `adb -s <serial> push merged-saves.zip /sdcard/Download/`; in the app: Saves → Import saves → A zip file; in the
       file picker tap the search icon, `adb shell input text merged-saves`, tap the file; then **Replace my saves**
       (the dialog's focus starts on "Keep my saves": `$B press UP`, `$B press A`).
    2. Saves → Export saves → SAVE (the picker proposes `TotK Native saves <date> <time>.zip` in Download).
    3. `adb pull "/sdcard/Download/TotK Native saves <date> <time>.zip"`, unzip it on the host and, inside its
       `TotK Native saves/` folder, `sha256sum -c merged-saves.sha256` must print 32 `OK` lines.
    4. Delete both zips from `/sdcard/Download` and stop the app.
    5. End the persistent root shell: `python3 tools/bench/rootsh.py -s <serial> stop` (`status` then prints
       "not running").
    6. **Final step: put the screen to sleep** (`adb -s <serial> shell input keyevent KEYCODE_SLEEP`), then verify
       `dumpsys power` reports `mWakefulness=Asleep` or `Dozing`. Include this in the restore receipt. Root force-stop
       when needed: `adb -s <serial> shell su -c 'am force-stop --user 0 com.aure.totk'`. Restore the original numeric
       stayon value exactly, never an assumed default; do not alter caps/governors/thermals for the benchmark.

## Reading the results

- Windows are the runtime's own 300-frame windows that started after the stick was pushed and ended before it was
  released. `fps`, `p50`/`p95`/`max` frame time, `GPU ms` per frame, `>50 ms` frames (stutter line) and long frames
  (`[长帧]`) come from `totk.log`; temperatures, cooling states, GPU `thermal_pwrlevel` and cpufreq limits from the
  sampler over each pan; the frequency limits (per cluster `related_cpus`, `scaling_min/max`, `cpuinfo_max`; GPU cap,
  hardware maximum and power levels) from `freqs.txt` (`freq_limits` per pan, `freq_stable` per run).
  Exact per-pan GPU residency comes from boundary differences of `trans_stat`'s final time column, never its
  transition matrix or an instantaneous-clock histogram. The KGSL `gpu_clock_stats` alternative counts active time
  in available-frequency order, so is labelled active-clock distribution, not full wall residency. Counter units
  and raw snapshots are retained; absent/failed/identity-mismatched or regressing counters are unavailable, not zero.
  KGSL `reset_count` tracks GPU boots/wakes as well as recovery, not devfreq statistics resets; its delta is reported
  separately and does not veto monotonic counters. Declared time units must pass the elapsed upper-bound check
  (device uptime plus max(1s,5%) tolerance); unknown units explicitly have no time-sanity check. See README sources.
  Sampler `external_power` is any online non-battery kernel supply, not Android's AC/USB charger enum; it is null
  without readable online state. Keep `power_supplies` raw type/online observations, raw legacy `ac`/`usb`, battery
  `level` and timestamps. Sysfs USB online can correspond to dumpsys AC=true on Odin; never rename one into the other.
- Read **steady fps, dip-window count, dip time fraction, ~33 ms frame count, and mean fps separately**. At a 60 target,
  >=60/300 frames in 29–37.5 ms makes a dip window (complete durations, else complete fence histogram); without full
  frame evidence the fallback is fps <50. At 30 target the fallback is <25; normal ~33 ms pacing is not a dip.
  Sparse long-frame logs give lower-bound counts, not a complete duration trace. `--target-fps 30|60` overrides the
  recorded target/preset; an unknown target must not be guessed. Dip time weights each window by 300/fps, not one vote.
  `fps_mean` is the legacy arithmetic mean of window fps; `fps_time_mean` is frames / elapsed window time.
- Each pan and arm summary retains backend `recording_ms_mean`, framework-fence `present_p95_ms_max` /
  `present_p99_ms_max` (maxima of window percentiles, **not pooled percentiles**), plus six-bin counts/shares and
  coverage. Bin shares use observed pairs, not D/300 or equal window votes. The **judder proxy** is the 20.9–29 ms
  bin share (`present_judder_proxy_fraction`); it cannot identify the full >16.7 & <30 ms range.
  For a separate diagnostic run, `TOTK_PRESENT_CADENCE_TRACE=1` emits immutable `[present-trace]` transaction
  ID/generation/source-frame/framework-fence timestamps. The analyzer joins only consecutive valid IDs in one
  generation, independent of callback order, and reports exact strict >16.7 & <30 ms share **among observed valid
  pairs**, with hit/pair counts (`present_judder_exact_*`); missing/reset/reversed endpoints are never bridged.
  This trace is high-volume and has perturbed Thor cadence: use both arms consistently and keep it off for ordinary
  performance acceptance. `TOTK_FRAME_TIMELINE=1` only logs late per-frame rows and is not a complete trace.
  All fence metrics remain framework proxies, not guaranteed panel scanout. Full field semantics: bench README.
- `compare.py` / `abtest.py` compare steady fps and dip time separately. A dip fraction difference >0.05 (five
  percentage points) reports **verdict confounded by dips**, not an optimization win/loss from a controller state.
  All-dip arms have no steady estimate; historic results without pacing data keep labelled legacy comparisons.
  Per-metric tolerances remain documented in `compare.py --help`. Frequency signatures still gate comparisons:
  **match** (regression check), **reference only** (indicative), **NOT COMPARABLE** (never decides exit status).
- Other **not comparable** warnings: a different `settings_env`, different cpufreq caps, or `freq_stable: false`.
- Temperatures depend on how long the device has been running the game: a one-scene session ends cooler than the
  1.0.0 round, which measured three scenes back to back. They are reported, not part of the scene verdict.

Known caveats:

- **Odin 3 CPU cap**: in the 1.0.0 baseline its CPU clocks were capped at 2.23 / 2.25 GHz (hardware 3.53 / 4.32 GHz) by a
  non-thermal limit (performance mode off, a ClusterTune "Performance" tile present); its Hateno and Lookout Landing
  numbers are CPU-limited. That cap is part of its 1.0.0 signature; an uncapped Odin 3 run is another signature.
- **Long-frame counts are lower bounds**: a long frame is logged only when over 20 ms and 40 % above the recent
  16-frame average, so 45–46.7 ms frames at 30 fps and sustained slowdowns are missed. For 30 fps devices the
  `>50 ms` count is the reliable one.
- **Lookout Landing is indoors** in this save set (the shelter next to Monte).
- Short runs (3–7 windows per scene): differences inside the tolerances are noise; repeat a run before calling a
  small change.

### Uncapped throughput comparisons

When capped 60-target A/B arms enter different stretches of the game's 60/30 controller, first report both pacing
states and the confounding; do not call the lower session mean a code regression. For a **separate throughput
experiment**, select the 60 fps preset and put `TOTK_UNCAPPED=1` in the developer `<game>/totk.env` for **both** arms,
then restart the game. `TOTK_FPS60=1` is required: FPS60 off explicitly disables this uncapped path. Keep the rest of
the environment identical, collect it, and label the result **uncapped profiling**, not Recommended/capped gameplay.
The current legacy FPS60 uncapped policy uses a 120-Hz clock, not a promise of unlimited output; controller/present
intervals can still change. Inspect `[fps-policy]` effective interval/uncapped lines to confirm the actual path;
conflicting physical-clock/display modes may override uncapped controls. Never assume the switch erased every dip.

The analyzer detects the effective collected `TOTK_UNCAPPED=1` (settings first, developer override second), or accepts
`--uncapped` for older captures. This flag changes analysis only. It **disables dip classification**:
`dip_metrics_valid=false`, null dip counts/fractions/steady fps, and `throughput_fps` (frames / summed window time)
as the primary speed. An uncapped ~40fps run is not labelled 100% dip just because the preset target is60.
`compare.py` / `abtest.py` compare uncapped throughput and independent frame-time tails, never emit a dip-confounded
verdict there, and make mixed capped/uncapped comparisons inconclusive. Keep game-frame p50/p95/p99 per window
and their maxima, >33/>50ms counts, p99/p50 spread and population window-fps standard deviation.
Game-frame p99 comes from `帧间隔 至帧 ... p99`, not the presentation-fence p99; absent tails stay unavailable.

Use this when isolating CPU/GPU throughput changes that a capped controller plateau would hide or decide, with the
same save, camera, caps, cache and thermal state, long pans and reversed arm order. It can change GPU load, residency,
power, thermals and latency: retain a capped 60-preset round for user-facing acceptance, and never pool capped and
uncapped results or compare either to the other's baseline. Preserve the previous `totk.env`, restore it after the
experiment and restart to restore normal behavior; benchmark tooling never enables uncapped mode by default.


## Compile and scene admission

Read each measured window's `compilation` and the console `COMPILES` flags. Frame-keyed `[nvn-vk]` stutter rows
provide graphics synchronous/worker build counts and milliseconds, plus new-pipeline creations (which can include
cache-backed creation; **do not add them to build counts**). Pan/run/arm/block summaries show `compile_windows`,
`compile_covered_windows` and observed count totals. Compiling windows are retained, never silently dropped:
report them as a possible confound and repeat under the same cache/warm-up policy if necessary.
Missing telemetry is null, not zero. Current shader-translation and compute messages lack always-on frame-keyed
counts, so those counts are unavailable. The overlay `COMPILE` row is a synchronous-only rolling second, not a
300-frame log count and not evidence that async work stopped.

Optional before each pan: `--warmup-calm-windows 2 --warmup-timeout 120`. Default is **off**. This waits for two
fresh consecutive 300-present reports with zero graphics creations/sync builds/worker builds, polling a bounded
log tail every 3 s; stale history, missing reports and gaps cannot satisfy it. `warmups.jsonl` retains newly
observed reports, elapsed time and `calm` / `timeout` / `unavailable`; the analyzer prints/retains the label even if
the bounded wait did not become calm. Measurement still proceeds, and compiling measured windows stay visible.
This does **not** prove shader/compute quiescence or an empty work queue. Apply the same warm-up policy to both arms;
maximum timeout is 3600 s, never an unbounded wait.

Time of day and weather are **not exposed by the current game log or performance overlay**; `scene_state` records
them as unavailable, never inferred from wall time or save names. Use the same named save/camera/settle procedure,
retain screenshots and describe visible scene differences manually. Do not claim scene equivalence from missing data.

## Restart-quartet A/B (launch-only switches)

Use **O1 → B1 → B2 → O2**, one restart per session, with separately collected/analyzed directories and matching
save, settle, pan durations/directions, cache, caps, power and thermal admission. Do **not** concatenate reset frame
counters or invent live-switch marks. Analyze each session with current `analyze.py`, then:

```sh
python3 tools/bench/abtest.py --restart-quartet \
  "$OUT/O1/result.json" "$OUT/B1/result.json" "$OUT/B2/result.json" "$OUT/O2/result.json" --ref O
```

This distinctly labelled **restart-quartet** mode needs the same device/scene set, measured windows, and explicit
matching pan direction/stick schedules (`pans[].direction` from `D` in pans.txt). Re-analyze old logs to obtain that
metadata. Mixed capped/uncapped sessions are rejected, not pooled. Inspect each source result's compile/cap/power/
GPU-residency caveats; the tool does not prove physical scene equivalence.

Pairs are **B1−O1 and B2−O2**, equal weight per session; pooled metrics are recomputed from original windows
(throughput remains frames/time). BETTER/WORSE requires pooled **and** paired-mean deltas strictly beyond tolerance
in the same direction and **both pair signs agreeing**, with no sign-test relaxation. FPS tolerance is
**max(0.5 fps, 2% of pooled O fps)**. **|O2−O1| must be ≤ tolerance** per metric; a confirmed effect therefore also
exceeds O-spread. Other metrics retain compare.py tolerances/rate normalization. Missing pairs/O-spread,
excess O-spread, sign disagreement or only one delta crossing => INCONCLUSIVE. With admitted O-spread, paired
delta inside tolerance => SAME. The report includes individual pair deltas and O-spread. Overall primary-tail and
capped dip-confounding rules remain in force; uncapped compares throughput/tails. A quartet is not statistical
significance and does not claim both-direction in-session confirmation. The existing mode below is unchanged.

## In-session A/B (live switches)

Compares experiment switches inside ONE running session and ONE loaded scene, alternating the arms (ABBA) instead of
restarting per arm. The APK must contain the live-switch channel (`runtime/live_ctl.h`; `[totk-ctl] following …` in
`totk.log`). Live switches, their ranges and where each takes effect: `games/totk/android/APP.md`, *Live switches*.
`TOTK_IDEAL_CORE_FIX` and any switch not listed there are refused (`[totk-ctl] refused …`): those still need a restart
with `totk.env`.

1. Steps 1–8 of the procedure above (profile, caps, start, load the scene from the Load list, settle 55 s once, dmesg
   mark, sampler).
2. Run the arms, e.g. descriptor stock off vs on on the Duo in Lookout Landing:
   ```sh
   tools/bench/ab.sh -s <serial> --device duo --scene lookout \
     --arms 'A:unset TOTK_VK_DESC_STOCK;B:set TOTK_VK_DESC_STOCK=1' \
     --order ABBAABBA --pan-seconds 30 --settle 15 --out $OUT
   ```
   Per block it writes the arm's lines and `mark ab arm=<X> block=<n> scene=<scene> direction=<right|left>` to
   `<game>/totk.ctl` with `live.sh` (one atomic batch replacement: one generation, applied at the next presented frame), waits for the game's
   `[totk-ctl] gen=N frame=F mark …` acknowledgement (it stops on a refused line or no acknowledgement), settles, then
   pans with `pan.sh` (right first on odd blocks, left first on even ones; the view returns each time). `arms.txt` lists
   the blocks with their generation, frame and direction. Arm lines are comma-separated `live.sh` lines; an arm may set several
   switches (`'A:unset TOTK_VK_DESC_STOCK,unset TOTK_PRIO_RENDER;B:set TOTK_VK_DESC_STOCK=1,set TOTK_PRIO_RENDER=-8'`).
   `unset` returns a switch to its launch value: with no developer `totk.env` that is the default (off).
   One-off changes: `tools/bench/live.sh -s <serial> set KEY=VALUE | unset KEY | mark <text>` prints the game's answer
   (exit 3: a mark not acknowledged, 4: a line refused).
3. Stop the sampler, `collect.sh` (step 10), then
   `python3 tools/bench/analyze.py $OUT --arms --json $OUT/ab.json` and `python3 tools/bench/abtest.py $OUT/ab.json`.
   `analyze.py --arms` gives each 300-frame window to the generation that covered all of it (from 2 frames after its
   change, `--guard-frames`) and so to the arm of that generation's mark; windows straddling a change are dropped and
   counted. Block records in `ab.json` carry the pan direction; old logs/JSON infer it from the block index (odd right,
   even left). **Keep alternating directions**: different view content alone can move fps by ~8 on the Odin in Hateno.
   `abtest.py` compares B against A of the **same direction within each complete ABBA quartet**: blocks 1↔3 and 4↔2,
   then 5↔7 and 8↔6, etc. BAAB quartets work too; incomplete/non-ABBA quartets are not paired. Pairs have equal weight,
   regardless of window count. The report includes pooled arm values/delta, paired mean delta, per-direction mean
   deltas, sign agreement with the paired mean (e.g. 4/4), and a two-sided **exact sign test** over the independent
   block pairs (zero differences are ties: excluded from the test, but prevent unanimous agreement).
   **Verdict**: *better*/*worse* needs BOTH the pooled and paired deltas beyond the compare.py tolerance in the same
   direction, AND (all pair signs agreeing OR exact sign `p < 0.05` with at least 6 pairs). At least two pairs covering
   both directions are required (`?` otherwise, unconfirmed). If only the paired delta crosses tolerance, or the pooled
   sign differs, the metric is *lean better/worse*, informational only. Otherwise it is *same*. The larger same-arm
   max−min block spread remains visible as information, never a verdict gate: it mixes the direction effect into noise.
   Overall: WORSE if any primary metric is worse, else BETTER if any is better, else INCONCLUSIVE if any has `?`,
   else SAME (leans do not count). Exit 1 when an arm is worse. Prefer **ABBAABBA or longer on 60 fps devices**; a
   single quartet has only two pairs and unanimous agreement is not itself statistical significance (sign p = 0.5).
   Beside the verdicts both print memory and pressure per arm (sampler: MemAvailable minimum, the game's RSS / swap,
   kgsl memory, PSI cpu / memory, kswapd CPU %): an arm that only fails under memory pressure (the Duo's Lookout
   collapse) shows it there.

Time per arm: a block is about `settle + pan + ~10 s` (write, acknowledgement, the pan's frequency reads), ~55 s with
the defaults, so ABBAABBA (4 blocks per arm) takes ~7.5 min in one scene load. A restart per arm costs ~45 s to the
title, the load, the 55 s settle and the pan: ~3 min per arm and scene, run once per arm without a repeat to estimate
the noise. A/B results are for deciding experiments; they are not ledger runs (no baseline, `ledger.py add` is for
default-profile runs).

## Capture a new benchmark (any device)

1. **Build the experiment APK from a clean checkout of the commit** (the APK's `assets/build.txt` must say
   `commit <sha12>` without `-dirty`, or the ledger refuses it). A separate worktree keeps your working tree out of it:
   ```sh
   git worktree add --detach .scratch/wt-<sha12> <commit>     # or a fresh clone; `git status --short` must be empty
   cd .scratch/wt-<sha12>
   python3 games/totk/android/build_release.py --ndk <Android NDK r28> --turnip <pinned Turnip driver.so> \
     --key <signing key .p12> --key-pass-env <VAR> [--translator <prebuilt android-arm64 totk_xlate>] \
     -o .scratch/bench/<sha12>.apk
   unzip -p .scratch/bench/<sha12>.apk assets/build.txt         # commit <sha12>
   ```
   The build needs the prerequisites of `games/totk/android/README.md`: Python 3.12+ **with `cryptography`**
   (`pip install cryptography==50.0.2`, e.g. in a venv whose `python` runs the command), git, make, cmake, javac 17+,
   the Android SDK and NDK r28, and the .NET 10 SDK unless `--translator` gives a prebuilt translator. The key password
   goes in an environment variable named by `--key-pass-env` (never on the command line).
2. **Install** with `adb -s <serial> install -r .scratch/bench/<sha12>.apk`. If it fails with a signature mismatch,
   stop and ask the user (never uninstall: that deletes the game data and saves).
3. **Profile**: the device's default (Recommended) profile, or a deliberate profile written in `--profile` and the note.
4. **Run the procedure above.** Unknown devices are fine (see Scope).
5. **Add to the ledger**, tied to that commit and APK (the commit must exist; the APK's `assets/build.txt` must match
   it; its sha256 must equal the installed APK's that `collect.sh` recorded):
   ```sh
   python3 tools/bench/ledger.py add $OUT/<name>.json --commit <full sha> --apk <path to the APK> \
     --profile "<IN USE preset> (default: Recommended for this device)" [--run-id <YYYY-MM-DD>-<label>] \
     --note "<what changed / why>"
   ```
   `--profile` names the preset as the Graphics screen shows it (or describes a deliberate other profile). This writes
   `docs/benchmarks/runs/<run-id>/<device>.json`, appends to `docs/benchmarks/ledger.jsonl`, prints the frequency
   signature and caps, and either the verdicts against that signature's baseline or **NEW BASELINE** (a signature seen
   for the first time: the baseline file, pin and index entry are written), plus a reference-only comparison with
   1.0.0 when model, settings and CPU caps match and the previous entry of the signature; then re-renders
   `docs/benchmarks/LEDGER.md` (`python3 tools/bench/ledger.py render` does it alone). The ledger is append-only.
6. **Commit** the ledger, LEDGER.md and the run JSON, plus any new `docs/benchmarks/baselines/` file, the index and
   `BASELINES.sha256` (and `docs/benchmarks/<name>.{md,json}` for a full round); run `python3 tools/bench/check_baselines.py`.

## Reproduce a ledger entry

1. `python3 tools/bench/ledger.py show <run-id>` prints everything needed: the commit, app version and APK sha256, the
   game version and Turnip driver sha256, the device facts, the profile and its `settings_env`, the frequency caps
   (per cluster and GPU) and signature, the method (`totk-android-benchmark/1` schema and the method summary: Load list,
   ~55 s settle, half-deflection stick pan) and per scene its name, save slot and pan length / stick value.
2. Check out that commit cleanly (step 1 of "Capture": a worktree), rebuild, and check the APK's `assets/build.txt`.
   (Release builds are not byte-reproducible across keys, so the APK sha256 differs with another key; the commit is what
   must match. The Turnip driver is pinned by `build_release.py`.)
3. Install with `adb install -r`, set the same profile (its `settings_env` must equal the entry's: `collect.sh` writes
   `settings-<label>.env`) and the same frequency caps (`bench_input.sh freqs` must show the entry's caps), import the
   merged save set (same sha256 list), and run the same scenes with the procedure above and the current `tools/bench`
   (check that `analyze.py` still writes the same schema and method summary).
4. Add it as a new ledger entry (new run id): with the same signature it is compared against that signature's baseline
   automatically; `python3 tools/bench/ledger.py compare <old-run-id> <new-run-id>` (or `compare.py` on the two result
   JSONs) compares the two runs directly.

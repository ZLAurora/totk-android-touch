# Android port: progress and open problems

A working log for whoever picks this up next, human or agent. Keep it current: when something is ruled out, fixed or
found, update the sections below in the same commit. Background docs: `README.md` (the port), `VULKAN.md` (the
Vulkan backend), `APP.md` (the APK). The Mac backend this one follows is `third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`;
its comments record the Mac's fixes (search for a date like `2026-10-15`), and most picture bugs here have a Mac
counterpart there.

**Integration note (2026-10-22):** this file preserves upstream's independent investigation and the owner's final
`test commit` status edit. It is not a device report for the combined dev/upstream build. See `UPSTREAM_MERGE.md`
for the 57-commit ledger, conflict decisions, defaults and pending device measurements, and `VULKAN.md` for dev's
measured optimizations. There is one pending-report ring (dev cpend) with upstream's cumulative synthetic samples,
not a second permanent per-address counter map. Precise occlusion remains opt-in, GPU timestamps remain enabled.

**Round-seven integration:** the complete merge/checkpoint ledger is in `UPSTREAM_MERGE.md`. Faithful original
early-fragment tests are now default-on; `CHUNK_DRAWS` stays 75. Plain `--runtime-only` keeps the existing shader seed:
request `--shaders` when the new seed-policy epoch must be evaluated. Host normal/TSan acceptance is not a combined
Android-device result.

**Round-eight default record (historical):** Android tried AHB with automatic copy fallback; explicit
`TOTK_VK_PRESENT=copy` remained the control. The then-current chunk rule was 600 only with
`TOTK_FRAME_LAG=1`, otherwise 75, and lag was recorded as off pending separate VMA safety work.
`TOTK_COMPANION=0` suppresses secondary presentation, polling and action injection after `totk.env` loads;
omit `--dex/--assets` in the low-level APK packer for the Java-free variant (`build_android.py --no-companion`
selects it, but that script is not part of integration verification).

**Round-ten status (historical, superseded for frame lag):** SGSR and automatic 60/30 selection are optional;
frame generation is separately optional and refuses automatic selection or real-60 mode. Development builds
default to the full top-right overlay; use `TOTK_OVERLAY=off` for matching instrumentation-off A/B runs, or
`--release` for the release default. Round ten recorded protected lag default-off after the open shadow report;
this was not evidence that lag caused it. Round 10.1 later enabled lag by default; the current policy below supersedes it.
The October 24 shader list has digest `f3788503…`; refresh with `--runtime-only --shaders` (and `--apk` if desired),
not plain runtime-only. `APP.md` records the full pinned-translator seed identity and release export limitation;
`UPSTREAM_MERGE.md` records authentic topic ancestry and the host test boundary.

**Current frame policy (2026-10-26; supersedes round 10.1):** protected `TOTK_FRAME_LAG` defaults
off on every platform and in every FPS mode. Only exact `TOTK_FRAME_LAG=1` enables it; every other
value leaves it off. Android shipping-30 (`TOTK_FPS60` unset/`0`) defaults GPU_TIME to hardware;
FPS60=1 and other platforms/BOTW retain latency. Explicit hardware/latency wins; unknown values
log and retain latency. Thor Lookout improves from lag-on/latency 27.71 fps (minimum 22.6, 13.2% long)
to lag-off/hardware 29.93 fps (minimum 29.6, 1.4% long). Latency reports overstated GPU time by
roughly 9–12 ms; hardware's roughly 23 ms matches KGSL's roughly 22.7 ms. The missing lag was not
the cause of the historical controller drops. Full supplied device results are in `docs/translation-research.md`.
`TOTK_VK_CHUNK_DRAWS` resolves to 75 by default; the rule remains 600 with resolved lag on and no native
VMA pressure, otherwise 75. An explicit positive value or 0 wins. Startup logs resolved `FPS60`,
`FRAME_LAG` and chunk threshold once. The unexplained round-nine lag-plus-slab-driver shadow report
still does not establish causation by lag.
Android also defaults to two recorder workers (`TOTK_VK_REC_THREADS=2`); explicit `0` preserves the synchronous
recorder A/B path. Ordered async submit follows the existing worker-enabled rule and its diagnostic exclusions.
`TOTK_DRS60=1` is separately opt-in and default off; it caps the native predicted GPU budget in FPS60 mode,
not the shipping 30-fps profile. See `APP.md` for controls and qualification limits.

**Native UI v2:** `TOTK_NATIVE_UI=1` is separately opt-in for v143. It retains the 720p guest canvas and
world/native-DRS policy, upscales once at the first UI draw into a native host sibling, and resolves game
reads to 1x. FSR/SGSR are bypassed; optional SMAA precedes the final overlay. Host copy/post/extent checks
cannot qualify Android AHB import, compositor behavior, text quality
or game performance. Use the paired native-off/SGSR and native-on comparisons in `APP.md`.

**Frame-generation modes:** `TOTK_FRAMEGEN=interp` selects real30→physical60 interpolation with one real-frame of
latency; UI comes from the older frame. `TOTK_FRAMEGEN=1` remains camera/depth extrapolation. Both require fixed
real30 and a physical60Hz display. `TOTK_FRAMEGEN_BUDGET_MS` is enforced per interpolated frame pair: invalid or
over-budget pairs fall back to the real image; the older frame's UI and 30Hz simulation/input cadence remain
limitations. This is not 60Hz gameplay, and disocclusions or independently moving objects can artifact. See `APP.md`.

`TOTK_FRAMEGEN=hybrid` is the separate opt-in1.4.3 native60 / real30+interp
wall-time policy (`FPS60=1 FPS_AUTO=0`, reserve disabled). Look for `[hybrid]`
startup/switch/per300-real lines, `[hybrid-present]` native-return retirement,
`[framegen-display]` child outcomes and `[hybrid-display]` AHB fence interval bins.
It never engages on a single guest interval2 frame. Exact options and Odin
qualification environment are in `docs/frame-interpolation.md`.

`TOTK_FRAMEGEN=adaptive` is separately opt-in, currently QCOM-only and admitted
before GL initialization only for the qualified Thor identity/Turnip A740 path.
Other devices (including Odin) must log real-only refusal without AFME startup;
adaptive LSFG is hard-disabled pending the independent doubling repair.
Use FPS60=1, REALTIME=1, native UI and fixed slots, capped synchronous replay;
the exact full prerequisite block and timing contract are in `APP.md`.
The guest's published timestep, accumulated at guest present, is simulation
authority; render-fence timestamps establish readiness only. `[adaptive]`
per300-real lines report phase bins, real rate, generation fraction and
missed/suppressed slots. A generated candidate waits until its target's2ms
transaction commit deadline so a ready real can win that slot; cancellation
never permits early GPU/source reuse. Below25fps, scene/loading/menu cuts,
invalid time or unsupported clocks clear history; two fresh eligible sources
are required to resume. At display rate it passes real frames through.
Use exact-layer SF cadence and visual review, not these counters, to qualify.

Adaptive QCOM misses use recoverable bounded cooldown/re-probe, while actual
capability/import/fence/corruption errors remain permanently refused. Inspect
the QCOM timing-policy counters rather than treating a temporary miss as a
device-capability failure. `TOTK_FRAMEGEN_JOB_TRACE=1` is default-off and
logging-only: every generated handoff/expiry logs submit-request, presenter
pickup, source/output ready-observation, target and decision times. Both tickets
ready before target with a later decision proves presenter/drain lateness.
Ready observations at/after target are fence/CPU-publication-late, not proof of
GPU-only lateness; no calibrated hardware timestamp is claimed. Use the same
trace setting in both comparison arms and label results instrumented. Host
formatter benchmarks do not establish Thor logging cost; obtain a separately
approved trace-off confirmation before production performance claims.
For `before observation without a live root`, inspect
`[framegen-authority-begin-failure]` under that same trace flag: the outer
message alone does not identify an invalid frame, current capture, attempted
mismatch, unsupported source, exhausted source ring or resource failure.
Its four `[framegen-authority-source]` records include atomic pin counts,
history membership and finite-ticket done observations. They do not retire
anything or wait; different observations can race completion. Negative retired
values distinguish unsealed(-1), missing(-2), and uninitialized(-3).
Per300/lifetime reason counters count failed calls, whereas extraction's
`backoff` counts frames that were never probed. Do not call~94% probe skips
“~94% depth failures”, or use eligible singleton frames as proof of a valid
adjacent prediction pair. No guard, depth comparison or source ownership rule
is relaxed by these diagnostics.

To test the presenter-blocking hypothesis, compare the SAME APK in adaptive
mode with `TOTK_FG_ADAPTIVE_BACKPRESSURE=serial` versus exact `request` (default
is serial). Request reuses existing API31+symbol admission and the two-flight
bound, with serial fallback when unavailable; all other pacing variables stay
unchanged. `[framegen-wait]` under JOB_TRACE records transaction callback wait
and previous-release poll intervals, including real transactions which can
block a later generated pickup. Correlate these CLOCK_MONOTONIC intervals with
job pickup/target timestamps, not with guesses about scheduler or GPU activity.
Within fixed recording bounds, retain every terminal generated job in the
handoff/deadline-share denominator and report completion/other expiries
separately. App handoff counts still do not identify displayed generated frames.

The asynchronous-retirement hypothesis is a separate same-build test:
`TOTK_FG_ASYNC_RETIRE=off|on` (default off), with request-mode backpressure
COMMON to all adaptive arms. A head-only worker retires the existing two
contexts; it may not free CURRENT, bypass a PREVIOUS release or reorder retirements.
Capacity waits remain on the presenter and are reported separately, so moving
pickup bookkeeping earlier cannot itself establish success. Preserve the same
recording-bound ALL-terminal denominator, SF actual-present evidence and visual
review. Report the fraction of jobs with capacity waits and per-arm read-only
CPU-cap/GPU-clock/thermal diagnostics. External F/state content changes between
paired arms are disclosed ORDER confounds; missing observations are UNOBSERVED,
not proof of equal workload or restoration.
An absent callback or never-signalling fence has a two-second steady-head
bound and one total two-second drain budget, not two seconds per queued context.
On timeout/error, quarantine compositor-owned storage and enable copy fallback;
do not fake completion or free a late callback's context. Join only after exit
is observed; otherwise a static quarantine owner keeps the ENTIRE reachable
graph alive: FIFO, mutexes/conditions, worker/callback contexts, buffers,
ASurfaceControl/window references and all presenter objects they touch.
Late callbacks after timed-out teardown still require valid storage; worker
exit after quarantine does not itself grant unsafe compositor-buffer reuse.

The GPU `vkframegentest --interp-job-reuse` regression exercises repeated NEW
generated jobs on one physical presenter slot, the actual generator completion
and four-source leases in both retirement modes. Cache/capacity state resets
at both NEW-job producer entries, never at SAME-job YIELD. A deliberately
omitted reset must expose retained leases before safe cleanup; passing only a
mock callback count does not establish that generator ownership was retired.

The next default-off experiment is `TOTK_FG_ADAPTIVE_SLOTS=0|1`, QCOM only:
keep request backpressure and async retirement ON in BOTH arms of the same APK.
Fresh API33 default-display clock metadata and an exact logical-display match
are mandatory; refused/stale clocks mean REAL-only. GAME timestamps determine
the phase; publication EWMA only predicts where an otherwise repeated slot may
exist. The first implementation dispatches at existing empty recorder boundaries
and skips missed opportunities rather than adding an unordered submission lane.
Report boundary opportunities, no-publication/no-hole/late-boundary/phase/clock/
GPU refusals and on-time app handoffs separately from actual SF presents.

`[adaptive-slot] event=apply-metadata` records BOTH the exact intended target
and the transaction's actual desired time. Only this flag encodes `desired =
target - 1ns` for SF's strict `desired < expected` eligibility rule; it remains
strictly later than the previous physical slot. Exact SF joins use actual
desired time, never a nearest-target match. Before apply a real can cancel a
pending generation; after apply ownership and SF latch order are irrevocable
to the app. Modelled zero real displacement does not prove that device residual.
Encoding failure is an explicit refusal, never an accidental desired=0
transaction. An unencodable generated target never reaches a setter/apply.
A real can cancel scheduler reservations and use a logged REAL-only positive
signed-clock fallback; that is not an invented scheduler slot or phase.
Final admission rechecks the encoded value under the window mutex.

READY newer reals are replanned on the same admitted API lattice before GEN
admission, including a stable re-sort of provisional producer queue keys.
Legacy callback-phase targets cannot arbitrate against API-clock child targets.

Use `framegenadaptiveslottest`, `adaptiveslotclocktest` and
`framegenqcompreparedtest` alongside the existing ownership/presenter suites.
Replay the retained QCOM5c 992/220 cohort through the actual policy implementation,
not only a model; it lacks per-job game anchors and models unchanged real
publications, so its occupied slots are conditional opportunities. Extra
generation GPU cost can slow real readiness and must be measured independently.
An active-game Perfetto FrameTimeline observer and both-panel visual captures
are common instrumentation for the exploratory pair. Empty, lost, unjoinable
or unavailable trace data stays UNKNOWN; app counters are not a displayed-FG,
smoothness, production-throughput or input-latency claim.

Keep `TOTK_PRESENT_PREROTATE=1` / `TOTK_PRESENT_UBWC=0` common to both Thor
scheduler arms and verify actual root-hint/native-buffer/inverse-geometry
logs. The earlier locked30 pre-rotation comparison is not adaptive evidence;
its historical non-pre-rotated cadence is not a rotation-independent ceiling.
The new baseline can already be near60, so report real-frame survival and
generated contribution as well as cadence. `request-refused` distinguishes
missing preparation/additional children. `irrevocable_gen_conflicts` counts
only the existing occupied-generated-slot traversal, not all late-real cases.
Additional potential late-real-after-GEN, ready-before-apply-unserviced and
unknown-ready-provenance records include source/transaction identities and
explicit `sf_outcome=UNKNOWN`. Backend readiness is a CPU fence-publication
timestamp, not calibrated GPU completion. Capacity records associate actual
live retirement tokens with applied GEN identities (including older epochs);
retired historical GEN time overlap does not turn an all-REAL queue into a
GEN-associated wait. Report these residuals separately from confirmed SF facts.

Delayed-child budget admission is predictive: maximum of the latest up-to-two
complete valid REAL costs plus ALL measured incremental costs in the charging
interval, within the unchanged 30ms ceiling. Missing authenticated evidence
refuses. Sealed ACTUAL aggregates are audited immutably, including refused and
cancelled child work; an overshoot or missing actual evidence triggers eight
subsequent charge intervals of back-off. Report predicted versus actual,
overshoots and cooldown per arm; prediction is not a hard actual-cost bound.

Scheduler defaults: with `TOTK_FG_ADAPTIVE_SLOTS=1`, CHUNK, LATTICE and (QCOM
engine) LEAN are on unless set to exact `0`; EARLY_APPLY, BUDGET, PIPE_SETTLE and
DIRECT_OUT stay opt-in. When reproducing the r7/r8 comparisons below, set
CHUNK/LATTICE explicitly; for comparisons before r14d also set `LEAN=0`.

The (formerly default-off) `TOTK_FG_ADAPTIVE_CHUNK=1` experiment adds an
opportunity after the existing `vk_submit_chunk` mandatory seal and recorder
service. Keep SLOTS=1 in BOTH arms and vary only CHUNK=0|1; do not compare it
with a non-scheduler control. It requires an empty primary and restores guest
submission attribution after backend-only child work. Guest final posts,
optional nonblocking allocation, the exact child ticket range and the 30ms
predictive aggregate rule are unchanged. No forced chunk, second lane or
capacity wait is introduced. Count opportunities/guard skips and SF-generated
contributions per covered second, not just successful hook calls. Under
JOB_TRACE, `[adaptive-dispatch-optional]` names the original inner allocator
refusal and queried facts; this is observational, not permission to weaken
replay ownership or increase ring capacity. A missing slot or missed deadline
still skips generation.

r7 (CHUNK=1) solved the dispatch lead, but 26% of handed-off generated frames
were absent from SurfaceFlinger, all in one episode. That episode correlated
with slot-lattice drift: the lattice period is the frozen first present-fence
calibration, about 4.6 µs longer per refresh than the measured vsync, so targets
drift later after each reset until generated frames miss their latch. Test
`TOTK_FG_ADAPTIVE_LATTICE=1` (phase re-anchor) and
`TOTK_FG_ADAPTIVE_EARLY_APPLY=1` (generated release at the FrameTimeline
deadline) as SEPARATE pairs with CHUNK=1 SLOTS=1 common. Use JOB_TRACE
`[adaptive-latch-phase]` rows joined to SF desired rows to show whether misses
follow phase error or apply-after-deadline. `[adaptive-scene]` names
SCENE/WARMUP causes; in r7 a 10 s pipeline-burst settle is the most likely
cause of the long SCENE hold (inference until traced). `TOTK_FG_ADAPTIVE_BUDGET=1`
(stale-estimate admission instead of the 1-per-30 probe) is a third, separate
experiment; see `.scratch/adaptive-fg/slot-scheduler/slots8-budget-evidence.txt`
for the measured tails and model counterfactuals.

`TOTK_FG_ADAPTIVE_LEAN` (default on since the r14d ABBA; exact `0` restores the
ordinary copies; optional `TOTK_FG_ADAPTIVE_DIRECT_OUT=1`) removes GPU cost from the
REAL frame. Measured r11/r12 per-real costs of the
skipped stages: authority init 0.094 ms, final copy and census 0.79 ms, scene,
depth and real copies 0.07/0.077/0.133 ms, plus three unprofiled spans with six
depth copies and pass splits each. The child keeps its AFME input copy;
DIRECT_OUT removes only the 0.16 ms mode-2 copy. Confirm the arm with the
startup `[framegen-adaptive-selectors]` row and, under JOB_TRACE,
`[framegen-adaptive-lean-summary]` (lean-sources and authority-roots near 300
per window). `selected but inactive` means scene post, WRITER_DIAG or DUMP kept
the copies. r14d (`.scratch/adaptive-fg/slot-scheduler/r14d-r13d/report.md`,
Tier E, one ABBA): real GPU interval −1.1 ms p50, delivered ~47.5 → ~56.5 Hz,
real 37.2 → 41.5 fps, no quality change found. Judge further changes against an
explicit `LEAN=0` pair with all other selectors equal.

The original unselected AHB shader/layout/16-byte push-constant command path
is preserved; nonidentity rotation lazily selects separate 32-byte pipelines.
Recorder entry checks the default-zero pending-template atomic before calling
the boundary/configuration path, but lifecycle cleanup keeps it set until all
prepared GPU leases drain.
Scheduler terminal descriptors and exact already-frozen readonly host payloads
now live in source-owned bounded storage (2MiB/source, two sets, 256 descriptors
per type). Capture happens at the original bind point, never from live guest
memory at a later dispatch. Unsupported/writable resources refuse only the
optional template. The lease retains the current recorder descriptor pool but
not mandatory snapshot arenas, including across the external immutable split.
Source references and actual completion tickets retain storage until safe reuse.
For consumed terminal geometry, the scheduler forces the existing immutable
replay snapshot selector at the original stream position instead of relying on
optional frame-lag capture. CPU update overlays are included; pending GPU-only
writers refuse optional generation. It never freezes a later live-guest reread.
The original real Vulkan bindings and flag-off path remain unchanged.
The borrowed-lease safety guard remains: mandatory snapshot allocation must
not wait for `UINT64_MAX` (an open recorder lease, not a GPU ticket). Its lifetime
bit survives queue drain, disable and quarantine; pressure cancels unsubmitted
borrowed jobs and appends while final ownership remains open, preserving frozen
prefixes without splitting a cached primary. Pending releases resolve at the
ordinary seal; quarantine remains retained. Under JOB_TRACE, per-job
`[adaptive-slot-job]` cancel guard/state/backend-reason and cumulative pressure
observations distinguish this from no-hole/deadline/clock/phase refusals.
`[adaptive-slot-owned]` records successful payload bounds or the first refusal
with role (descriptor/vertex/index), stage/set/binding/element or stream,
descriptor type, buffer/offset/range, freeze disposition and recorder serial.
These details are emitted only under JOB_TRACE and are not eligibility authority.
Real-recorder fixtures exercise retained immutable templates through ring wrap
as well as the legacy pressure/cancellation/quarantine path. The wait invariant
still rejects any UINT64_MAX GPU wait; omitted admission must abort exactly there.
Host lifetime evidence is not on-device safety or performance qualification.

For a prepared scheduler that reaches `dispatch-refused`, enable the existing
JOB_TRACE flag rather than guessing from the aggregate. Match
`[adaptive-dispatch-guard]` source/charge/output identity to its symbolic guard,
cached budget/probe/cooldown evidence and `[adaptive-dispatch-stage]` records.
Keep recording/seal, native AFME fence completion, CPU output-ready publication
and transaction apply-return separate. Native GL-fence time is not final
Vulkan/native-UI GPU-done; absent final hardware authority is explicitly UNKNOWN.
`[adaptive-slot-boundary]` records existing begin/commit enter/exit and
lock/submission/service brackets without relocating any hook. Publication during
the boundary's lock/loop interval can be later than its entry timestamp; use the
backend dispatch-stage time for actual eligibility, not entry as an oracle.
Host replay may conditionally shift observed durations to a hypothetical earlier
instant, but that does not prove a legal empty-recorder boundary existed there
or predict unchanged GPU contention. Do not loosen phase/budget/deadline to
hide late-ready work. Any second submission lane requires a separate ownership,
ordering/accounting/lifecycle design review before implementation.

Under ASan on Lavapipe (Mesa 26.0.8), the prepared fixture reports a
Lavapipe-only process-exit LeakSanitizer allocation whenever a replayed draw
binds a vertex buffer its pipeline does not consume. `TOTK_TEST_LEGACY_PREPARED=vertex`
reproduces it on the old borrowed path; `=1` (no owned capture) is clean. It is
not suppressed; functional checks still must report zero failures.

The optional exploratory Perfetto admission uses **source-profile protocol
authority, not vendor-binary equivalence**. Service-global discarded-chunk/
patch totals are diagnostic, not session loss. Only the exact pinned
v25.0/SDK33/schema/config/single-buffer profile with positive session activity
and real game events can derive absent supported buffer-loss scalars as
schema-default zero; raw presence and basis remain retained. Explicit session
errors/nonzero loss still refuse admission. A normal first-sequence
`previous_packet_dropped` marker resets incremental state, not observed loss;
later drops or unknown required state stay UNKNOWN. An ineligible five-second
active schema stops before the measured arm; do not reset a service, install
dependencies, fabricate missing events or silently proceed.




## Walker registry-kind lookup (2026-10-25)

The recorded Thor route put `vk_treg_kind` at 4.18% of walking CPU. Its per-address samples concentrated at the
generation/epoch loads and thread-cache tag check (`0x10aa15c` 33.67%, `0x10aa16c` 19.83% of that function);
another 10.65% reached the locked registry's validity check. Registry writers now publish a full-width atomic
format/target pair and validity bitmap indexed by the 20-bit texture ID. Readers check the atomic pool bound and
read that pair directly, without a TLS allocation, cache collisions, object-epoch checks, locks or whole-entry
copies. Registration/view overrides and the out-of-range locked fallback retain their original results. This adds
8.125 MiB of shared zero-initialized virtual storage and removes the old 256 KiB per-reading-thread kind cache;
only pages containing registered IDs need be touched. Registration publication cost is outside the read benchmark.

`vktest --registry-kind-bench` checks invalid IDs/output preservation, view overrides, full-width integer values,
pool shrink/growth, unregister, and concurrent format/target pair coherence (10 checks). It times 32 production
lookups per synthetic draw, both 32 hot IDs and a 16,384-ID sweep crossing the old 8,192-entry collision boundary.
Seven interleaved baseline/changed host runs (`-O1`, `taskset -c 0-3 nice -n 10`) gave median **70.67 -> 64.94 ns/draw
(-8.1%) hot**, **428.14 -> 64.31 ns/draw (-85.0%) wide**. This is a CPU lookup microbenchmark, not an Android frame
prediction. Reproduce after the ordinary host build with `taskset -c 0-3 nice -n 10 <OUT>/vktest --registry-kind-bench`.

## Walker texture descriptor cache indexing (2026-10-25)

The same route put `vk_tex_by_id` at 1.74%, `tex_get_inner` at 2.31%, and `view_get` at 1.39% of walker self time.
50.88% of `vk_tex_by_id` samples landed at its first cache-tag comparison (`0x10aa36c`). In `tex_get_inner`,
21.62% landed at the serial map's value test, and 65.10% at dependent object/image field tests. These are
memory-dependent lookup paths; the profile does not establish a device cache-miss rate.
The descriptor-cache index previously multiplied IDs by four before masking to 4,096 slots, leaving only 1,024
usable entries for any one shader target. IDs 1,024 apart therefore forced repeated registry locks, texture-map
lookups (including their `memcmp`), and view-key construction. The new index uses every low ID bit, folds higher
bits, and salts by target, without changing any exact key checks, generation/epoch/submission invalidation,
cache capacity, resource lifetime, uploads or Vulkan command construction.

`vktest --texture-cache-bench` realizes one real image with 2,048 descriptor aliases and validates every timed
view/object result, target-specific views, re-registration, unregister and pool shrink/growth (9 checks).
Seven interleaved old/new host runs, 32 lookups per synthetic draw, gave medians **1,637.14 -> 96.83 ns/draw
(-94.1%)** over the 2,048-ID sweep; the 32-ID hot control was **89.23 -> 93.81 ns/draw (+5.1%)**.
The deliberately conflicting sweep isolates the old index's capacity loss; neither that delta nor its frequency
is a claim about the game's actual ID distribution or Android frame time. Reproduce with the ordinary host seed
and lavapipe environment, `taskset -c 0-3 nice -n 10 <OUT>/vktest --texture-cache-bench`.

Host acceptance for the resolution cuts: core/draw/app 23/100/106 checks; recorder cross-mode identities; frozen
spans 7,097 checks; pipeline 65 checks; native UI off/on 16/24 checks. Protected-lag core/draw passed with recorder
workers 0/2/3. The changed registry's concurrent test and protected-lag two-worker core passed TSan
(`CC='cc -fsanitize=thread'`, `TSAN_OPTIONS=halt_on_error=1`, `LP_NUM_THREADS=0`), without suppressions.
The original base and changed 0/2/3-worker protected-lag identity streams were byte-identical (787,888 bytes,
SHA-256 `39c294dbd9ef59ae398ffca4c6a949138ae815576a3f4c383528ecc0a20dc8f6`).
Android `cc1.sh` syntax checks passed for `vk_res.c` and the test translation unit. No device build/run or new
device-frame measurement was performed.

## Walker exact binding keys (2026-10-25)

`bind_stage` (4.19% walker self time) contains the inlined recorder freeze path. Its used-binding comparison loop
loads and compares four words (`0x10ce1e0` through `0x10ce1f8`), although `record_binding_of` (0.74%) only ever
populates the first two. Keys are now exactly two words and the helper writes directly into the caller's destination,
including memo refreshes. All generated/uniform/storage/texture/sampler/image key cases retain the same meaningful
values; no revisions, descriptor dependencies or freeze-hit conditions are removed.

`vktest --record-binding-bench` checks those independent expected values and 6.4 million exact comparisons, grouped
as 32 bindings/draw. Seven interleaved old/new host runs gave median **172.94 -> 158.60 ns/draw (-8.3%)**.
A size-only, by-value-return candidate regressed its host control by 21.3% and was not retained; writing the compact
key directly avoids that return/copy tradeoff. The fixture is CPU-only and does not predict device frame time.
Core/draw/app, recorder/span/pipeline/native-UI identities were repeated after the resolution cuts; protected-lag
0/2/3-worker output remains byte-identical to the original base (the SHA-256 above). Protected-lag two-worker
core/draw also passed TSan without suppressions. Android syntax checks passed for all changed C/test units.

## Walker program memo stage enumeration (2026-10-25)

`vk_prog_resolve_record` (1.31% walker self time) spent 28.27% of its samples at the six-slot stage-presence branch
(`0x10b1f58`). It now retains the immutable program's stage bitmask and visits/compares only present signatures,
without clearing and comparing six words on every draw. The existing signature helper accepts the pre-scan registry
revision for this path, avoiding a second atomic registry read per stage; the ordinary resolver still acquires its
own revision. A registration during the scan invalidates the next draw exactly as before. Semantic signature reuse,
negative results, translation publication epochs and complete six-stage output pointers remain intact.

`vkdrawtest --program-resolve-bench` checks every resolved stage/result in stable and explicitly invalidated-fragment
cases. Seven interleaved host controls gave median **18.06 -> 13.05 ns/draw (-27.7%) stable** and
**24.29 -> 20.80 ns/draw (-14.4%) invalidated**. Use the ordinary test seed/lavapipe environment and CPU cap.
The repeated identity, TSan and Android syntax acceptance above includes this cut; these host lookup numbers are
not device-frame predictions.

## Walker span winner-interval coalescing (2026-10-25)

`resolve_local` (2.37% walker self time) and `record_resolve_inner` (1.67%) still spend substantial samples in
binary search: 31.51% of the former at `0x10919c0`, and 55.87% of the latter at `0x10915b0`/`0x10915b4`.
The span sweep previously retained every endpoint, even when a nested pool/view did not change the exact
furthest-reaching winning span. After computing those winners, it now removes adjacent identical-winner
boundaries. Mapping precedence, tie-breaking, offsets, returned available extents, gaps, generation invalidation
and immutable snapshots are unchanged. Searches and snapshot copies become smaller; existing hints cover the
entire exact winner interval instead of needlessly failing at a nested-view endpoint. No extra lookup cache or
larger span record is introduced.

`vktest --span-resolve-bench` uses one real 64-MiB owning pool and 1,024 nested views, verifying the owner buffer,
offset and available extent on every query. Seven interleaved host medians, 32 queries/draw:
**865.73 -> 180.56 ns live (-79.1%)**, **901.35 -> 154.71 ns frozen with hints (-82.8%)**,
**932.01 -> 163.71 ns frozen without hints (-82.4%)**. This specifically measures redundant-endpoint removal,
not the game's pool overlap distribution or Android frame time.

In addition to the repeated common identities above, protected-lag frozen spans passed 7,416 checks,
`--lag-vma` 147, `--lag-vma-async` 29, and `vkdrawtest --lag-walk-bench 1` 484 (including gated private-UBO
zero-wait checks). Existing lag geometry/same-address reuse/alias safety remains covered. TSan core/draw with
two workers and Android syntax checks also passed. The independent full-span inventory used for alias
projection is deliberately not coalesced.

## Walker alternating-pipeline lookup (2026-10-25)

`pipe_get` (1.28% walker self time) had 18.76% of samples in the full-key hashing loop (`0x10cb9c4`) and 29.42%
at the map-value test (`0x10cba3c`). Its one-entry last-pipeline memo now has a bounded 64-pointer exact-key front
cache for alternating shaders/topologies. A candidate must still match the layout and complete pipeline key;
collisions fall back to the original map. The memo retains entries, not published pipeline handles: every hit still
acquires and handles queued/building/ready/failed state, original compile/join policy, retries and capture behavior.
The existing one-entry stable fast path stays unchanged. Added storage is 512 bytes on 64-bit hosts, with no
per-draw allocations.

`vkdrawtest --pipeline-resolve-bench` verifies exact ready handles for all three topology classes. Seven
interleaved host medians were **104.47 -> 23.78 ns/draw (-77.2%) alternating**, with a stable control
**4.02 -> 3.58 ns/draw** (tiny absolute timings, not a meaningful device-speed claim). Use the ordinary seed,
lavapipe and CPU-capped environment. This isolates repeated pipeline-map work; actual game transition frequency
and Android frame benefit remain unmeasured. The repeated complete pipeline/recorder identities, protected-lag
baseline byte comparisons, two-worker TSan core/draw and Android syntax acceptance above includes this cut.

## Aligned scene extent versus capture compilation

The old `abs(width*9-height*16)<32` classifier can call a normal DRS scene a capture:
960x536 has delta64 and864x480 delta96, although both arise from ordinary540p-family allocations.
`vk_screen_classify.h` keeps the original near-exact16:9 path and adds a minimum640x360 landscape gate
for8/16/32-aligned floor rounding of16:9 or the configured `TOTK_RENDER_RES` aspect. Bounds come
from rounding intervals, not a percentage-aspect tolerance that would grow with scene size.
Native1280x720/1600x900/1920x1080 remain recognized.64x64 B10G11R11+depth probe-like targets and
square cubemap faces retain synchronous color-capture compilation; depth-only policy is unchanged.
Do not interpret the diagnostic `capture-sync` reason as recovered resource lifetime.

`vkpipetest --screen-classify` forces cold complete-key misses, checks aligned scene work goes to
workers, and checks a square color+depth capture renders synchronously on first demand and remains
complete after compilation recovery without guest redraw. `TOTK_PSO_TRACE=1` emits linked
`[pso-target]` and `[pso-target-color]` rows: actual extents/formats, new and old classification,
submission/pass cursor, serial/address/mip/layer identities. Every real synchronous recording-thread
build reports its demand-time context, including failures and retries; workers correlate by stable key
without inspecting mutable recording state. The cursor/open pair is not a guest-authored pass name.

Device comparison must restore the same cache for every baseline/fix arm, retain actual RT dimensions,
and separate traced attribution/video arms from clean timing. Check village/Sky movement and persistent
Gerudo lighting; reduced sync counts alone do not license missing or permanently changed probe content.

To distinguish a recurrent half-resolution target from temporal history, use
`TOTK_PSO_LIFE=480x268 TOTK_PSO_LIFE_START=3000 TOTK_PSO_LIFE_FRAMES=1800` in a separate
Sky attribution run. This preserves synchronous capture policy. The actual Vulkan-view cache supplies
image/incarnation and exact subresource ranges; reflected descriptors are enumerated before recorder
freeze-memo returns. Sampled reads deduplicate only within an unchanged physical pass, where no
intervening transfer/attachment writer can occur; storage/compute observations retain their order.
Pass writes record successful draw/clear events and full-clear base authority. External uploads/copies
remain conservative preserved-base writes, and non-descriptor reads/unsupported ordering emit unknowns.

`vkpipetest --target-life` uses real480x268 RGBA8+D32 producers,960x536 consumers and two recorder
workers. Frames2–4 overwrite before sampling; frame5 samples retained frame4, and frame6 samples the
same image through a changed serial before rewriting. It checks actual pixels/depth and descriptor-freeze
hits. Analyze its log with `analyze_target_lifetime.py` and run `tests/test_target_lifetime.py`.
Expected historical writer ages are1 and2, independent of serial changes. A complete zero-history
window is only bounded transient candidacy, not universal semantic proof or authority for arbitrary quarter
targets; a historical or incomplete result retains synchronous capture behavior.
## Optional Vulkan graphics pipeline library

`TOTK_VK_PIPELINE_LIBRARY=1` requests the GPL backend; it is disabled by default and activates only when
`VK_EXT_graphics_pipeline_library`, `VK_KHR_pipeline_library`, and the `graphicsPipelineLibrary` feature
are all supported. The startup log records requested/supported/enabled and the fast-linking and independent
interpolation properties. The default monolithic path is unchanged.

When enabled, missing GPL parts are compiled by workers and draws skip until every required part is ready.
A ready combination can then be fast-linked on the draw's frame while the existing worker queues the
monolithic optimized pipeline; a later draw swaps in the optimized handle. This experimental opt-in can
increase compilation and depends on driver correctness. Acceptance command: `./vkpipetest --gpl`
(requires a lavapipe build with GPL support; unsupported is a failure, not a fallback).
`vk_pipeline_test_stats` exposes cumulative `fast_links`, `fast_draws`, `optimized_replacements`,
`parts[4]`, `part_ns[4]`, `fast_link_ns`, `fast_link_max_ns`, `optimized_ns`, and `library_skips`.
The ordinary 300-frame log reports fast-link count/total/max milliseconds, actual fast-linked draws,
optimized replacement count/milliseconds, per-type part creation count/milliseconds and misses still skipped.
`TOTK_VK_COMPILE_THREADS=0` retains ordinary synchronous creation; captures, bounded takeover and retries
retain their existing correctness policy. Libraries retain optimization information, while the background
replacement is monolithic; cached libraries and superseded fast handles live as long as the pipeline cache
entries so recorded/in-flight commands cannot reference a destroyed pipeline.

## Setup

- Device: AYN Thor (Adreno 740, Android 13). Drivers tried: Turnip "Apex" and Purple T23 (`driver.so` in the game
  folder); no difference for any bug below.
- Game: 1.4.3 (base 1.0.0 dump + `--update`). 1.0.0 behaves the same.
- Build (Linux, inside the user's dev distrobox), from the repo root:

  ```
  python3 games/totk/android/build_android.py ~/totkdump/totk.nsp --update ~/totkdump/update.nsp -k ~/totkdump/prod.keys --firmware ~/totkdump/Firmware.22.5.0/ --ndk ~/Android/ndk/android-ndk-r27c --apk --shaders
  ```

  After a full build, code changes only need `--runtime-only --apk` (minutes), then `sh ~/totkdump/TOTK_android/install_app.sh`.
- Game folder on the device: `/sdcard/Android/data/com.aure.totk/files` (`totk.env`, `totk.log`, `shots/`).
  `install_app.sh log` prints the log. Options go in `totk.env` (`NAME=value` lines), read at start; force-stop the app
  (`adb shell am force-stop com.aure.totk`) after editing.
- Desktop tests (lavapipe): `make -C games/totk/android vktest vktest-app TARGET=linux CC=clang OUT=<dir>`, then run
  `vktest`, `vkdrawtest`, `vkdrawtest-app` with `TOTK_SHADER_SEED_DIR=<dir>/vkdraw-seed VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`.
  All must pass before pushing.

### Host draw-test seed prerequisite (2026-10-24)

The apparent 23/98 `vkdrawtest` and 24/104 `vkdrawtest-app` regression on both `e386478` and `5629f19`
was an **empty shader fixture**, not a lavapipe regression. Their logs reported missing VS `09c79c8c…`,
FS `d826ac5d…` and storage FS `ce0d6fa0…`; those draws were skipped. The top half consequently retained
the blue clear `(0,0,255,255)` instead of the textured `(0,0,128,255)` at pixel `(0,0)`. `vktest` does not
need these draw shaders and still passed. An existing seed directory alone does not mean generation succeeded:
require the successful make exit and its `vkdraw-seed/.done` stamp before running the binaries.

`gfx/test_vkdraw_seed.py` compiles 13 GLSL fixtures with **`glslangValidator`**, then runs `vk_reflect.py`;
it does not use the game's Ryujinx translator or `spirv-opt`. This host had no `glslangValidator` in PATH
and no installed `glslang-tools`. The original recipe already returned nonzero on Python's missing-tool
exception, but created the empty directory first; the recipe now diagnoses the missing compiler before
generation. Do not continue to the test executables after a failed parallel make.

Package history rules out an October 23/24 driver/compiler upgrade: `/var/log/dpkg.log.1` records Mesa
`26.0.8-1ubuntu0.3` installed September 22, LLVM `21.1.8-6ubuntu1` September 20 and clang of that version
September 27. October apt/dpkg history contains no later transitions for those packages or glslang;
`spirv-tools 2026.1-1` was installed October 7. An earlier missing-compiler failure also survives in
`.scratch/integ3/build-normal.log`. The successful historical fixture in `.scratch/framegen/host/vkdraw-seed`
and the NDK-glslc alternate fixture were already populated; using a fresh OUT without the generator
prerequisite exposed the setup gap. No ICD downgrade or backend barrier change is needed.

Workaround without a system install (from the project directory, with `spirv-tools` already installed):

```sh
mkdir -p .scratch/hosttest-regress/tools
(cd .scratch/hosttest-regress/tools &&
 apt-get download glslang-tools=16.2.0-2 &&
 dpkg-deb -x glslang-tools_16.2.0-2_amd64.deb root)
PATH="$PWD/.scratch/hosttest-regress/tools/root/usr/bin:$PATH" \
 make -C games/totk/android -j8 -s vktest vktest-app TARGET=linux CC=clang \
 OUT="$PWD/.scratch/hosttest-regress/out"
```

Run the three binaries with the seed/ICD settings above and the same absolute OUT. Alternatively install
the distribution's `glslang-tools` or put a real `glslangValidator` from a Vulkan SDK in PATH.
The regenerated glslang 16.2.0 seed had all 13 reflection/variant files byte-identical to the older
fixture; its SPIR-V bytes differed, and both compiler families rendered correctly. On unchanged Mesa/LLVM,
restoring the seed yielded **23/23**, **98/98**, **104/104** on both revisions. The full draw suite also
passed **98/98** with locally extracted Khronos validation `1.4.341.0-1`, with no validation errors.
A debugger stop at the first textured assertion confirmed that layer was loaded, zero bad pixels in
both halves, `(0,0)=(0,0,128,255)`, `(7,5)=(28,10,128,255)` and lower-half `(7,40)=(0,0,255,255)`.
Logs and the regenerated seed are under `.scratch/hosttest-regress/`. This is host-only evidence.

## Walker CPU sampling and TLS (2026-10-25)

Build the normal Android `APP=1` library with **`PROFILE_FP=1`** passed to
`games/totk/android/Makefile`. Normal builds already retain non-leaf frame pointers;
this option also passes `-mno-omit-leaf-frame-pointer`. The build-kind stamp includes
the option, so toggling it rebuilds existing runtime/graphics objects. Retain the
unstripped `libtotk.so` from that exact build beside the recording: symbols from a
different build cannot reliably identify sampled addresses. No device performance
result is implied by enabling frame pointers.

For an operator-run recording, keep the same route, CPU/GPU caps, thermal state,
driver and diagnostic settings as the control. Disable timeline/recording profilers
for the primary sample. With simpleperf installed on the device, record the app
process for 60 seconds using `cpu-clock` at 2 kHz and frame-pointer call chains:

```sh
simpleperf record -p <app-pid> -e cpu-clock -f 2000 --call-graph fp \
  --duration 60 -o <device-output>/walk-route.data
```

Copy the data and matching unstripped library to the host. Filter the report to
`totk-walk` (or include `totk-recw0`, `totk-recw1`, `g-main` to compare owners):

```sh
taskset -c 0-3 nice -n 10 <ndk>/simpleperf/bin/linux/x86_64/simpleperf report \
  -i walk-route.data --symdir symbols --comms totk-walk \
  --sort dso,symbol --percent-limit 0.3
```

Use `report -g` for call chains and `--sort symbol,srcline` for source attribution.
Check that callers actually reach the walking loop before trusting inclusive
percentages; driver/assembly frames can still interrupt an otherwise valid chain.

**Do not set `-ftls-model=initial-exec` for `APP=1`.** NativeActivity's library is
loaded with `dlopen` into an already-running Android process, not a startup
`DT_NEEDED` dependency of the process executable. Bionic rejects IE references to
TLS defined in a dynamically loaded object; there is no portable static-TLS-surplus
assumption to rely on. A headless executable has a different loading contract and
its TLS can already be linker-relaxed. See Bionic's
[ELF TLS design](https://android.googlesource.com/platform/bionic/+/HEAD/docs/elf-tls.md)
and [dlopen TLS tests](https://android.googlesource.com/platform/bionic/+/HEAD/tests/elftls_dl_test.cpp).
Use `readelf -Wr libtotk.so` to count `R_AARCH64_TLSDESC` relocations, and
`readelf -Ws libtotk.so` to identify TLS owners. The matching route-one profiling
library has 24 TLSDESC relocations (992 bytes of TLS); this is a baseline inventory,
not proof that every relocation executes per draw.

The timeline's inline walking counters now check the process-wide initialized
enable switch **before** accessing TLS. Disabled counters and frame lookups do not
initialize diagnostics and do not resolve a TLS descriptor. Enabled accounting
keeps its existing per-thread state and once-per-walk publication. This changes
executed work, not the library's TLS ABI or relocation inventory. A host PIC
dynamic-TLS fixture (four counter calls and two frame lookups per synthetic draw,
21 interleaved pairs) measured 13.95 → 5.01 ns/draw; this isolates the disabled
hooks and is not an Android frame-time prediction. Timeline on/off regression
fixtures cover pre-initialization, disabled counters and concurrent enabled owners.

`TOTK_VK_LAG_TIMING=1` now enables both sampled (1/64) lag-decision timing
and individual snapshot-copy timing, in addition to per-binding registration timing.
With it unset/zero, those hot paths do not read the clock; counts, captured bytes,
memo invalidations and protection decisions remain unchanged. `lag cost` labels the
timing state explicitly; zero copy/sample nanoseconds with `timing=off` means
unmeasured, not zero-cost. Chunk registration, recorder service, queue submission
and actual wait watchdogs retain their coarse timing.
Unselected classification samples also skip the timer-option check itself.

Generated 1.4–2.5-KiB driver/support blocks no longer keep a second heap copy
solely for equality checks: the memo reads the immutable bytes already placed in
the slot's transient arena. Its existing seal-serial check expires the pointer
before that slot can be reset or compacted. The required guest/snapshot copy and
descriptor-payload initialization are unchanged. The existing packet capacity/set
initialization is retained: its removal did not show a reliable fixture benefit.

An actual-`block_desc` host microfixture with 2.5-KiB blocks, alternating changed
blocks/exact-byte hits and 15 interleaved pairs measured 41.75 → 30.29 ns/draw.
This isolates duplicate-copy removal; it is not a whole-walk speedup. Filling
dynamic state directly into a packet was also tried and discarded after its
host microfixture regressed; the original local state/copy path remains.

Walker kernel-transition audit (source attribution, **not** a syscall trace):

- `vk_record.c` dispatches one condition-variable wake per chunk, not per draw;
  chunks below 16 operations already coalesce into batches of up to four jobs or
  32 operations. Completion is published once per worker batch. The slot handoff
  wakes the submit owner once per sealed slot. Mutex/condition operations may
  enter `futex` under contention; a source call is not proof of a kernel entry.
- Native lag registration already sorts/deduplicates immutable units and
  `lag_protect_run` holds all affected BUSY gates while combining up to 64 adjacent
  units into one protection run. `protect_span` merges contiguous page changes.
  Existing ARMED units need only a load/ticket CAS; uncontended `lag_unlock` avoids
  the futex wake. Completion's global futex wake runs on the GPU/completion owner.
- Walking snapshot captures can still issue `mprotect` through `lag_watch_copy`,
  as can CPU writes that unprotect watched ranges. These transitions establish
  byte-copy authority and must precede the capture/write. Delaying them until
  chunk end would permit a writer race; publishing ARMED before protection lands
  reproduces the documented stale-page-state deadlock. No additional deferred
  protection batching is applied.
- Native asynchronous mode moves assembly, guard registration, queue-submit driver
  work and GPU-fence waits off the walker. Synchronous/fallback modes can still
  enter the driver there. This code does not directly issue a draw-time `ioctl`;
  Vulkan implementation calls may do so internally. The sampled kernel
  `_raw_spin_unlock_irqrestore` name alone cannot distinguish futex, VMA or driver
  work. Use the frame-pointer profile above to establish its call chain rather
  than attributing all kernel samples to recorder wakes.


Final host evidence for the clock/generated-block changes (not an Android speed
claim): seven interleaved before/after pairs of `vkdrawtest-app --lag-walk-bench 6`,
protected lag/wait mode, two recorder workers, `TOTK_VK_LAG_TIMING=0`, under
`taskset -c 0-3 nice -n 10`. The fixture enables its full recording profiler.
Medians below are **walking-thread CPU ns/draw**, excluding the final GPU wait:

| Fixture case | Before | After | Change |
| --- | ---: | ---: | ---: |
| Stable bindings | 2761.06 | 2626.07 | -4.89% |
| Churn bindings | 3855.45 | 3842.75 | -0.33% |
| Device-style cold inputs | 1494.54 | 1460.02 | -2.31% |
| Device-style learned-hot inputs | 1290.41 | 1261.55 | -2.24% |
| Chunk stress | 2660.25 | 2678.48 | +0.69% |
| Device-style chunk stress | 1661.61 | 1664.29 | +0.16% |

All 36 frame pixel hashes matched before/after in each pair. The small deltas,
especially stress cases, are not evidence of a device-level win. An isolated
four-decision-counter microfixture with no classification work measured 8.16 →
10.91 thread-CPU ns/draw despite eliminating disabled sampled clocks: the timer
gate/code shape is not free. Its real-path tradeoff needs the next device profile,
not a claim that deleting clock calls necessarily makes every host microloop faster.
The pure TLS and generated-block microfixtures above isolate different work and
must not be added to this table as independent whole-frame savings.

Verification of the final source: normal Vulkan suites **23/23, 100/100, 106/106**;
protected-lag suites **175/175, 193/193, 199/199**, with both timing states passing
the draw fixtures; recorder pixel/command identity and job-reuse/storage/lease/
allocation-failure suites; frozen spans **7097/7097** normal and **7416/7416**
protected; pipeline **65/65**, prediction **82/82**; VMA **147/147** and async VMA
**29/29**; timeline enabled/disabled tests. Full Android `APP=1` links succeeded
for exact base `2c3a2c1` and the final source (`PROFILE_FP=1`), and Android
`-fsyntax-only` passed the affected production/test C files. Both Android libraries
have **24 TLSDESC relocations and 992 bytes of TLS**: dynamic TLS is intentionally
retained, while disabled timeline hooks bypass it. There is no recorder queue,
writer-gate or other synchronization-protocol change in this cut.
Reproduction sources, binaries and logs are in `.scratch/runtime-overheads/`.

## mmap-lock contention and syscall attribution (2026-10-25)

Owner-supplied Thor route evidence (not rerun here): 31% of walker D-sleep stacks end in
`rwsem_down_write_slowpath <- do_mprotect_pkey`; 17% end in the page-fault read-lock path.
Over 90 seconds, the walker slept 143 ms in mprotect (1,596 episodes, max 14.5 ms), and g-main
72 ms in mprotect (938 episodes, max 17.5 ms) plus 15 ms in faults. These are **blocked episodes**,
not total mprotect syscall counts. Linux 5.15 serializes these permission changes with `mmap_lock`.

Every actual tracker mprotect now increments bounded per-thread/reason calls, system pages and elapsed
time (including failures). The existing 300-frame reporter prints `[dirty-mprotect]` interval totals,
calls/pages per frame, TID and first-observed kernel thread name. Normal registration fast hits do not
touch these counters. One TLS slot lookup and three relaxed thread-local-shard atomic additions occur
per actual syscall; first use records TID/name with raw syscalls, without allocation or mutexes, including
inside the fault handler. 511 thread incarnations retain individual rows; excess threads share a labeled
overflow row rather than disappearing. Snapshot fields are independent cumulative atomics, so interval
edges are approximate. The existing aggregate call/page/time API is retained.

Reasons map directly to permission-changing source paths: `guard-rearm` = cold and re-armed explicit-ticket
unit guards; `snapshot-watch` = fence-zero native capture watches; `texture-watch` / `exact-watch` =
baseline protections; `fault-unprotect` / `host-unprotect` = guest faults / host pre-write preparation;
`gpu-unprotect` / `gpu-restore` = intentional GPU-completion stores; `pressure-drain` = one-shot cutover.
Baseline page scans and ARMED duplicate/ticket updates already skip syscalls for unchanged permissions.
Unsafe alternatives are not equivalent: an unwatched RW hot unit cannot certify native capture reuse,
and delaying watches until chunk end would expose a CPU writer race.

`vktest --mprotect-bench` is a pre-Vulkan protocol fixture with 2,400 64-KiB units and 240 rewritten units
per frame, two warmups plus six measured frames per guest-interior/host-interior/guest-edge case.
Baseline observed writer calls/frame are **720 / 720 / 480**, with **240** sparse guard re-arms/frame
and exactly **3840** writer system pages/frame. Initial contiguous registration uses **38** calls.
The fixture checks actual written bytes, dirty generations, untouched units, and exact agreement between
aggregate and reason call/page counts. Its protocol tickets are completed before every write.

Counter-only interleaved host lag-walk medians (CPU ns/draw, no speedup claimed): stable
2204.37 -> 1968.16; churn 2767.51 -> 2791.04; device-cold 1091.88 -> 1049.96; learned-hot
874.87 -> 924.65. Normal/GCC-TSan suites passed core/draw/app 23/100/106 (lag off), 175/193/199
(protected), recorder identity, spans 7416, pipelines 65, lag-bench 325, VMA/async/policy/merge
147/29/5/10, private UBO 189, lag-walk 559, query 45109, real cutover/job reuse 131, and new syscall
fixture 100. TSan retains backend workers with `LP_NUM_THREADS=0`, no suppressions. Android syntax
checks passed `nvn_dirty.c` and `vk_draw.c`. Evidence: `.scratch/mmap-lock/counters*`,
`walk-base-counters*`; no device/build-output mutation.

### Coalesced writer unprotection

An ordinary write to the middle of a fully guarded 64-KiB unit previously unprotected its neighbours
before/after the target page, then unprotected the target separately: **three** mmap write locks and
transient permission splits. The writer now includes its already-owned BUSY page in the same contiguous
RW run. Exact-watch neighbours remain protected barriers; foreign BUSY pages are never taken. Unit-gate
ownership, newest-ticket waiting, per-page dirty stamping, the second `dirty_now` advance, and PG_NONE
publication after a successful syscall are unchanged. The obsolete single-page unprotect helper is removed.

The 2,400-unit fixture observes **720 -> 240** writer calls/frame for both guest-interior and host-interior
cases (67% fewer), **480 -> 240** for guest-edge (50% fewer), with identical **3840** affected system
pages and **240** sparse guard calls/frame. Interleaved median CPU ns/write: guest-interior
**6500.68 -> 4394.02**, host-interior **5809.91 -> 2413.32**, guest-edge **5781.64 -> 5050.29**.
That models 480 fewer syscalls/frame for 240 interior writes, not the user's unknown total route write count.
For ordinary fully protected units the actual reduction is two calls per interior write and one per edge
write; exact-watch barriers reduce this benefit. Blocked-episode counts cannot supply the missing denominator.

Normal/TSan acceptance passed the complete matrix above and the expanded syscall fixture (103 checks),
including exact-hole generation preservation and a real later write to the still-protected exact page.
The single-system-page `TOTK_DIRTY_UNIT=1` lag-bench also passed 325 checks. Android syntax passed.
Evidence: `.scratch/mmap-lock/protect-counters-merged*`, `merged*`.

### Batched direct native watches

`nvn_dirty_lag_register` previously invoked one-unit protection separately even for a multi-unit span.
Direct snapshot watches/aliases now reuse the already-established `lag_protect_run` batching used by
frozen submission guards. One-unit bindings retain their original fast path. Adjacent newly writable units
share permission calls; already-ARMED units still skip mprotect and explicit-ticket BUSY encounters fail
closed. The 64-gate/4-MiB bound is deliberately retained rather than increasing individual lock holds.

The populated-PTE native-watch fixture compares scalar one-unit calls with an equivalent 512-unit span,
in six interleaved scalar/bulk runs: **512 -> 8 mprotects**, identical protected page coverage and real
post-watch dirty generation checks, **2503.39 -> 1303.16 CPU ns/unit**. Repeating either watch on RO memory
performs zero syscalls. This is 504 fewer calls for a 32-MiB direct watch; two contiguous newly writable
units similarly use one instead of two. It does not claim that route bindings are 32 MiB.
The 2,400-unit native submission batch remains 38 cold calls and 240 sparse re-arms/frame.
Lag-walk interleaved CPU ns/draw (merged-writer control -> batched direct watches): stable
2033.08 -> 2075.78, churn 2611.80 -> 2571.82, cold 1210.23 -> 1028.60, hot 888.75 -> 865.59.

All normal/TSan suites above passed, including the expanded syscall fixture (139 checks); Android syntax
passed `nvn_dirty.c`. Evidence: `.scratch/mmap-lock/watch*`, `walk-merged-watch*`.

### Runtime VM and diagnostic syscall attribution

The same report now includes repository-owned runtime protection calls, not just dirty tracking:
`services.c` emits `vm-commit` / `vm-decommit` for heap and physical-alias page operations, and
`stack-permission` for moved stacks; `loader.c` emits `module-permission` and host-service stack
guards; `gfx_bridge.c` emits `diagnostic-watch`. The opt-in `TOTK_WATCH_ADDR` / `DM_NVN_WATCH_SB`
watchpoint starts a 100-us rearm loop, which can itself be a substantial syscall source when armed.
It is not assumed enabled in the owner's trace.

The GPU_VK wrapper returns the original libc result and errno, counts failed attempts and rounded
system-page extents, works before the first dirty pool, and does **not** convert runtime ENOMEM into
lag-guard pressure fallback. Reporting initializes its own timebase outside the signal path, so startup
elapsed time is not lost. Null/Metal builds expand directly to libc without a Vulkan link dependency.
These are attribution changes only: runtime VM decisions, protection flags, lengths and call counts
are unchanged. Driver/internal-library syscalls remain outside this source-level instrumentation.

Normal/TSan full suites passed; the expanded syscall fixture passes 151 checks, including pre-init
time/page accounting, successful-call errno, invalid/aligned-unmapped failure errno, failure without
invented guard pressure, and restoration of actual write access. A standalone null-backend executable
also passed without linking Vulkan. Android syntax passed all three runtime callsite files and
`nvn_dirty.c`. Production game SVC execution was not attempted. Evidence: `.scratch/mmap-lock/runtime*`.

### Bounded maps-sampler read-lock fills

The background sampler retains its default 10-second, end-to-start cadence and conservative VMA
budget semantics. Both the raw counter and FILE-based classification reader now request 1 KiB rather
than 4 KiB per input fill. Upstream Linux 5.15
[`task_mmu.c`](https://raw.githubusercontent.com/torvalds/linux/v5.15/fs/proc/task_mmu.c) takes the
maps read lock in `m_start` and releases it in `m_stop`;
[`seq_file.c`](https://raw.githubusercontent.com/torvalds/linux/v5.15/fs/seq_file.c) stops filling at
the requested read length and calls `stop` before copying to userspace. Thus the lock does not span
userspace parsing or the entire scan. Smaller requests bound normal record-fill work per ownership
interval, not wall-clock lock duration: preemption and a single oversized record remain possible.
The exact Thor vendor implementation was not independently inspected, and the sampler is not proven
to have caused the reported 14.5-ms blocked episodes. Rarer sampling was rejected because it would
stale the protection-budget authority.

A host reader/writer fixture creates 40,000 alternating-permission VMAs, traverses maps five times
per phase while another thread repeatedly mprotects a separate populated page, and interleaves six
4-KiB/1-KiB phases. Median scan time was 14.564 -> 14.327 ms, at the explicit cost of 483 -> 1,917
read calls per scan. Writer p50 was 1.227 -> 1.217 us and p99 was 5.566 -> 5.866 us: there is **no
demonstrated host tail improvement**. Host kernel 7 is not the target kernel; this is a source-based
bounded-fill mitigation, not a measured device speedup or a reduction in mprotect count. Reproducer
and raw observations: `.scratch/mmap-lock/maps_lock_bench.c`, `maps-lock-bench.log`.

The final normal and unsuppressed TSan matrix passed: core/draw/app 23/100/106 checks with lag off,
175/193/199 protected; recorder identity/boundary/crossmode, recorder span 7,416, pipeline 65,
lag benchmark 325, VMA 147, async VMA 29, policy 5, merge 10, private-uniform 189, lag-walk 559,
lag-query 45,109, real recorder fallback/job reuse 131, and mprotect fixture 151. Merge exercises the
FILE classification path. Android syntax passed the final dirty tracker. Evidence:
`.scratch/mmap-lock/reader*`. All builds, tests and benchmarks were restricted to CPUs 0-3 at nice 10.


## Frame critical-path timeline (opt-in, 2026-10-23)

Operator observation, **not rerun here**: Thor, 60-Hz panel, shipping 30-fps controller, protected lag,
`CHUNK_DRAWS=600` and two recorder threads averages 29.6–29.8 fps, but each 300-frame window contains 22–59
40–60-ms frames followed by short frames. There are no in-game pipeline compiles; display pacing did not fix
the judder and a GPU-floor experiment was inconclusive. Average walking/recording/GPU times (~23/24/21 ms)
cannot identify those outliers. This diagnostic measures their frame-by-frame chain without changing pacing.

Add **`TOTK_FRAME_TIMELINE=1`** to the existing `totk.env`; unset or `0` is off. A complete reproduction
configuration for the described protected-lag path is:

```
TOTK_FRAME_TIMELINE=1
TOTK_FPS60=0
TOTK_FRAME_LAG=1
TOTK_VK_LAG_CAPTURE=1
TOTK_VK_LAG_MODE=wait
TOTK_VK_CHUNK_DRAWS=600
TOTK_VK_REC_THREADS=2
TOTK_ASYNC_REPLAY=0
```

Keep the selected presenter, `TOTK_PACE`, driver, GPU/CPU caps, save/camera, cache, brightness and thermal state
unchanged for the first run. `TOTK_ASYNC_REPLAY` is the older guest-to-translation-thread queue, **not** the
recorder threads or asynchronous slot submit owner; use its default `0` here (protected lag already requires
synchronous replay). This timeline's frame attribution targets that walking path. `TOTK_SVC_PROFILE` is not
required, and this diagnostic does not force recorder threads off. Avoid screenshots/pass dumps and other
profiling for the primary trace.

Instrumentation and ownership:

- `runtime/frame_timeline.c` owns a bounded 1,024-frame ring. Frame IDs are one-based guest presents, advanced
  only after `nvnQueuePresentTexture` returns through its host implementation. Every host anchor uses
  `CLOCK_MONOTONIC`; GPU query durations are converted with the existing timestamp period, not mixed into the
  CPU time domain. There is no hot-path allocation, file write or per-frame logging. Disabled hooks do not read
  clocks. Short atomic-flag critical sections protect record updates/snapshots, with no I/O under those flags.
- `runtime/threads.c` marks the guest thread actually calling `QueueSubmitCommands` as a walker. For its
  blocking SVC `0x34` address waits, the existing wake machinery records the signalling guest's handle and
  ready time. After the wait and virtual-core reacquisition, the first successful wake by TOTK main thread
  `0x1001` before that frame's first walk records the return timestamp, address and waker. Thus `dispatch`
  includes host/virtual-core rescheduling after the signal; `wake_gap` is the interval between successive
  frame wakes. A timed-out/non-main wake is not fabricated as a frame-start signal. The initial unclassified
  wake may be absent until the first submit identifies the walker.
- `hle_nvn.c` records every `QueueSubmitCommands` call's begin/end and its thread-CPU delta, summing all walks
  within the frame. `walk` includes deferred-bind resolution and synchronous replay through publication;
  `offcpu = max(walk wall − thread CPU, 0)` includes host scheduling, mutex/condition waits, and joins. It is
  **not** just explicitly enumerated waits. `join` separately sums actually-blocked slot reuse, submission
  publication joins and fence-value waits in disjoint walking scopes, including the final publication join.
  Inline recorder-worker joins in `vk_record.c` are covered by `offcpu`, not separately enumerated by `join`.
  These fields overlap: **do not add `offcpu` or `join` to `walk`**.
- `nvn_vk.c` retains each slot's walking-captured frame ID/ticket through the asynchronous owner. It records
  seal time and the actual `vkQueueSubmit2` entry/return; the existing two queue-call timing reads are reused.
  The protected-lag GPU guard retains its timestamp immediately after its existing fence wait. The completion
  thread publishes that GPU observation and query-derived duration **before CPU post operations**, so post
  stalls are not mislabeled GPU execution. GPU duration covers existing main-command-buffer timestamps, not
  pre-buffer work; zero means unavailable/unsuccessful, not proof of zero GPU cost. The observation timestamp
  is a CPU observation of a signaled fence, not a calibrated hardware signal timestamp.
- Acquire entry/exit is recorded around `nvnWindowAcquireTexture`, including pacing and texture-fence waits.
  `vk_draw.c` attaches the original frame ID when handing a buffer to the presenter; invalid-window/busy-ring
  drops do not manufacture a handoff. AHB `apply` is immediately before `ASurfaceTransaction_apply`; copy
  `apply` is the successful `ANativeWindow_unlockAndPost` return acknowledgement. They are submission
  anchors, **not** scanout timestamps.
- The AHB transaction completion callback owns **that transaction's present fence**. Pending fds are retained
  in a bounded 16-entry ring and re-queried nonblocking on later callbacks/presenter opportunities, not closed
  after a one-shot pending result. `SYNC_IOC_FILE_INFO` validates up to 16 children and records the latest
  signaled child `CLOCK_MONOTONIC` timestamp. Absent/error/oversized/invalid/evicted entries remain unknown.
  Previous-buffer release fences remain exclusively lifetime protection, never current-frame display time. The copy
  API exposes no such timestamp, so its `display` stays missing. To obtain this extra evidence, repeat with
  `TOTK_VK_PRESENT=ahb`, requiring its normal AHB log and no copy fallback; do not mix presenters within an A/B.

Reports appear in `totk.log` under **`[frame-tl]`** / **`[frame-tl dist]`** / **`[frame-tl late]`** /
**`[frame-tl summary]`**. Each 300-frame window is reported 30 frames later to allow asynchronous owners to
finish (first window 1..300 after frame 330). A dedicated `totk-frame-tl` reporter does the formatting/I/O;
the walking thread only publishes an atomic watermark and posts its semaphore once per 300 frames. Missing
completion evidence at the report deadline stays missing; the diagnostic does not wait for the GPU/compositor.

- Three distributions distinguish **game present**, **compositor submit**, and available **display-fence**
  intervals, with sample count, mean/min/p50/p95/p99/max and buckets relative to each frame's requested target:
  `<−4`, `−4..+4`, `+4..12`, `+12..20`, `+20..36`, `>+36` ms. Only consecutive known anchors contribute, never
  a multi-frame gap across a dropped/missing compositor frame. The first window lacks frame 0's interval.
- Every frame whose known interval in **any** of those domains exceeds target by **strictly more than 4 ms**
  gets one line, emitted in the batch. It includes all three intervals; wake gap/dispatch; total walking,
  off-CPU and explicit wait; maximum seal-to-queue-return latency; summed driver queue-call time; summed
  query GPU duration; last-slot GPU completion tail; acquire span; and handoff-to-compositor-submit delay.
  `gpu_backlog_or_observe` subtracts the last slot's query duration from its queue-return-to-GPU-observation
  tail where available: it can include queue backlog **and** CPU observation latency, not just GPU execution.
- `relative_prev_game_ms` lists signed anchors relative to the **previous frame's game-present entry**,
  in the printed order. `walks=[begin,end,cpu=…]` lists each walk; `slots=[ticket:seal,queue_begin,queue_end,
  gpu_observation,gpu=duration]` lists each slot. Those timestamps are also relative to that same origin;
  `nan` means unobserved. Negative offsets are valid CPU/GPU overlap, not corrupted ordering.
- `dominant` is an explicitly conservative **candidate**, not a causal claim: take the largest positive
  increase versus the previous frame in walking duration, seal-to-queue latency, GPU duration/tail, acquire
  span or presenter delay, or excess wake-to-wake interval above target. Summary counts those candidates
  across late frames. Compare raw anchors to follow propagated delay before blaming a stage. Missing GPU
  completions cannot win the GPU category; unchanged/unknown growth reports `unknown`.
- Capacity is 32 walks / 64 slots per frame. `lost_walks`, `lost_slots`, `overflow_frames`, `missing` and
  `expired_events` expose truncation or a reporter falling behind the ring. An expired async writer cannot
  corrupt a reused frame entry. Walking totals remain cumulative after walk-detail capacity is reached.

### Walk-variance counters

`TOTK_FRAME_TIMELINE=1` also enables cheap work accounting, with no separate profiler required. Late rows append
integer counters; each 300-frame report adds two `[frame-tl work]` rows, `class=late` and `class=on-time`, giving
the **mean per frame of every counter** and its denominator `n`. Group membership uses the same any-observed-clock
strict `target + 4 ms` rule as late rows. Only consecutive frames with a complete game-present interval enter these
means; the preceding anchor frame is not counted. Empty groups print `n=0 ... missing`, not measured zero work.

- `draws` counts actually recorded game draw commands (including worker secondaries), not skipped pipeline attempts;
  `passes` counts game rendering starts, including clear-only passes. `chunks` counts sealed Vulkan submission slots.
- `lag_vertex/index/uniform/storage` count valid enabled public lag-read decisions, excluding nested prepare/copy
  helpers. Their `_miss` counters follow the existing lag memos: vertex/index/storage miss only when both exact
  binding metadata and unit classification miss; uniform misses when its exact immutable-binding copy memo cannot
  satisfy the request (a base/cover lookup may still succeed). A miss is a subset of its kind's decisions.
- `snap_allocs/snap_bytes` count actual GPU-addressable immutable uniform logical allocations/requested bytes,
  including base shifts and overlays, not CPU staging, metadata or arena backing chunks. `hot_copies/hot_bytes`
  count performed classified-hot protected-lag captures, not cache hits or requested descriptor extents.
- `guard_regs` counts actual registration attempts: deduplicated dirty units on native batch paths, ranges on the
  software exclusion/fallback paths. It does **not** mean newly armed pages and is not cross-platform unit-normalized.
- `pipe_creates/desc_creates` count successful game pipeline/descriptor-set allocations, not cache hits or updates.
  `tex_uploads` counts buffer-to-image upload commands (one multi-region command counts once); `tex_realizations`
  counts new texture images, including format upgrades/dummy images, not views or shared-image reuse.
- `cpu_steps` counts actual replay CPU-step boundaries. Creation/worker counters retain their originating walk's
  frame ID even when the work finishes later; no worker attributes work to whichever frame happens to be current.
- `rec_s_ns/rec_p_ns/rec_o_ns` are the recorder's exclusive sequential/parallelizable/other split. Without
  `TOTK_VK_REC_PROFILE`, opcode scopes are sampled at **1/64**, weighted by 64; `rec_samples` reports actual selected
  scopes. Lifecycle outside opcode replay is measured directly. These are noisy per-frame time estimates, not exact
  CPU-time counters or an overhead subtraction. If the full recorder profiler is explicitly enabled, its configured
  sampling rate is retained instead; compare runs with the same settings.
  They exclude asynchronous worker CPU time and are not a complete partition of walk wall time; the existing timing
  scope ends before post-unlock resource sweeping.

Per-event walking counters use thread-local additions and are published once per walk. Secondary descriptor/draw
counts publish per job, rather than taking the timeline ring lock for every draw/allocation. Rare asynchronous
pipeline creation and slot-level guard registration publish against captured frame IDs. Disabled timeline accounting
does not enable recorder profiling, create another reporter, alter pacing, or change rendering defaults.

Acquire diagnostics now retain the original total span and add `acquire_pacing` and `acquire_texture` (milliseconds)
to late rows, plus their per-group means. The former measures the display-link/synthetic/deterministic pacing block;
the latter measures prior texture replay/present-ticket waits. They are disjoint wall-clock scopes, not GPU query
times, and neither includes acquire setup/sync publication.

Walk-counter host acceptance: deterministic timeline tests passed enabled/disabled in normal and TSan builds,
including every counter's multi-walk accumulation, concurrent worker ownership, expired-frame rejection, acquire
split, and a full 300-frame late/on-time mean comparison. Counter-enabled Vulkan core/draw fixtures passed normal
and TSan at `REC_THREADS=0/2/3` with protected lag/writeback. Twelve identity runs additionally cover recorder counts
and profiler off/on (rate 8), check 2,068 actual draws per walk against the independent stream count, and observe
sampled parallelizable timing. All identity outputs are byte-identical across those modes. Prescribed Android
`cc1.sh` checks passed for all nine changed C translation units without diagnostics. Evidence is under
`.scratch/integ3/walk-variance/`; no device run or performance improvement is claimed.

Use the existing frame-timeline environment, with `TOTK_FRAME_TIMELINE=1` and **`TOTK_VK_REC_PROFILE` unset** for
the cheap fixed-1/64 path. Keep recorder count, lag and pacing settings identical to the baseline. Retain the entire
`[frame-tl work]` pair and late rows; compare counter growth together with measured walk/off-CPU times, not sampled
S/P/O alone. This change diagnoses variance; it does not change main-thread handoff or frame buffering.

Host acceptance exercised for this change: `.scratch/cc1.sh` passed for `frame_timeline.c`, `threads.c`,
`hle_nvn.c`, `nvn_vk.c` and `vk_draw.c`; the deterministic bookkeeping test passed enabled and disabled.
It covers late-then-short intervals, target changes and the exact +4-ms threshold, missing display evidence,
out-of-order owner updates, frame IDs, concurrent slot writers/snapshots, capacity and stale ring-generation
writes. From the project root (no game data or GPU required):

```
cc -std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE \
  -DTOTK_FRAME_TIMELINE_TEST -pthread \
  wt-frametl/games/totk/android/gfx/test_frame_timeline.c \
  wt-frametl/games/totk/runtime/frame_timeline.c \
  wt-frametl/games/totk/runtime/frame_fence.c \
  -o .scratch/FramePacing/test_frame_timeline
.scratch/FramePacing/test_frame_timeline off
.scratch/FramePacing/test_frame_timeline
```

Device plan (not executed here): run the reported Thor scenario warmed for at least 180 seconds with the above
configuration; retain every late row and summary, not just average FPS. Verify the 40–60-ms game intervals are
represented, then sort candidate stages and inspect their per-slot/per-walk anchors. Compare game versus AHB
display-fence intervals to distinguish CPU/GPU scheduling from compositor misses. Repeat with only
`TOTK_PACE=display` changed, and then a diagnostic-off run to check observer cost. Keep the panel at 60 Hz.
No fix for the observed judder, device performance result, or complete attribution is claimed by these host checks.

## Thor judder: recorded chain and display-slot pacing

The next investigation uses the operator-supplied `.scratch/bench/t-ftl-a.log`, SHA-256
`83cd08c2960c9b2313e856e728b291eb763bf22481fc4dcfff46d65382be3213`. It is a **copy-presenter** run (line 75),
not AHB. The guest window was initialized with **two textures** (line 88); our independent presenter ring already
has **three** (`vk_draw.c:NPRES`). A full presenter ring drops a display copy; it does not block the guest acquire.
`mtl_present_wait` waits for the guest texture's GPU-use ticket, not for SurfaceFlinger to release our CPU copy.
Increasing presenter buffers alone therefore cannot remove the observed NVN acquire delay, and inventing a
third guest index would access a texture the game never allocated.

### What the timestamps actually establish

The original timeline logs detailed rows only for late frames; it does **not** preserve every nonlate frame's
raw chain in the log. The following medians are explicitly the **251 late-row sample, frames 7801..9300**, not
whole-run averages. The earlier 6901..7800 segment includes larger off-CPU waits and should not be conflated
with that warmed CPU-bound segment.

| Anchor/duration | Late-row median |
| --- | ---: |
| Acquire entry after previous game present | 0.191 ms |
| Acquire duration | 6.772 ms |
| Acquire exit after previous present | 7.003 ms |
| Main signal-ready after acquire exit | 2.127 ms |
| Ready-to-walker dispatch | 0.264 ms |
| Walker wake after previous present | 9.535 ms |
| Walk begin after wake | 0.014 ms |
| Walk duration / off-CPU | 28.872 / 0.742 ms |
| Walk end to current game-present entry | 0.007 ms |
| Current game present to presenter handoff | 0.170 ms |
| Final GPU completion after current game present | 10.847 ms |
| Copy post after final GPU completion | 1.171 ms |

Acquire finishes before the main-thread wake in **all 251 sampled rows**. Its wall-clock span is inside the
~10-ms pre-walk gap, but temporal containment does **not** establish a serial dependency: render-side acquire
can overlap main-side update/fence work. It is already finished a median ~2.1 ms before the main's signal.
Advancing acquire therefore cannot be assumed to advance the main signal or remove that gap. After walking,
the guest presents almost immediately, enqueues its readback, and the next frame's acquire begins ~0.2 ms
later. The presenter finishes that frame asynchronously while the next frame progresses; adding
`present_delay` to main + walking would double-count overlap.

Concrete frame **9008**, log line 5237, relative to frame 9007's game-present entry:

```
acquire          +0.202 .. +6.040 ms
main signal      +8.860 ms; walker returns from WaitForAddress +9.311 ms
walk             +9.323 .. +43.585 ms (33.455 ms thread CPU)
game present     +43.592 ms; presenter handoff +43.789 ms
last GPU done    +55.039 ms; copy post +56.505 ms
```

The observed game-present interval is the main-ready/wake offset plus CPU walking, not a 56.5-ms synchronous
present call. This row alone cannot split its pre-wake offset into main fence waiting versus update work. In the
full 6901..9300 logged late-row set, 137 of 523 rows were late only in the compositor-submission domain; the
game interval itself was <= target +4 ms. Actual display intervals are unknown in this copy run (`display n=0`).
The 9001..9300 all-frame game distribution is mean 33.309, p50 32.976, p95 39.594, p99 44.708 ms; its copy-post
p99 is 50.033 ms. These are logged observations, not a rerun or a diagnosis based only on best FPS.

`hle_nvn.c` currently schedules a synthetic display slot with `nvn_pace`, assigns the texture availability
`slot + game_period`, then blocks its later `nvnWindowAcquireTexture` until that time before checking the
GPU ticket. With two rotating guest textures this is a producer pacing gate. The source does not tie it to
the compositor's ownership of the separate three-buffer presenter. The original log's acquire span combines
that gate with texture-ticket waiting; it cannot prove their split for every frame. The additive acquire
pacing/texture timing fields from the walk-variance investigation distinguish them on the next device run.

### Acquire phase: no measured feedback loop

The follow-on operator log `.scratch/bench/t-wvar-a.log`, SHA-256
`816515178a10bfb59a8a54c6fa4ee720229f64db0401af956124fb347eb8d6fc`, splits acquisition into pacing
and texture reuse. For windows 8401..8700 / 8701..9000, late versus on-time pacing means were
8.448 versus 3.180 ms / 6.551 versus 3.073 ms; texture waiting was about 0.001 ms in both groups.
This establishes which acquire component was long, **not** which prior frame caused it.

With `TOTK_PACE` unset/0, `nvn_pace` already preserves a persistent synthetic phase:
`S[n] = S[n-1] + P` through ordinary lateness of less than one period; only a whole-period-or-longer
stall rebases it. With two guest textures, acquire N uses availability written at N-2:
`avail = S[N-2] + P`. The deadline is not simply previous present arrival + P. A frame that finished
early leaves more residual time to that phase, so its successor can show a long acquire wait before
doing its own long walk. In contrast, `TOTK_PACE=display` uses `display_pace_take`: consuming the
current callback discards missed callback credits (deliberately no catch-up), so its phase policy differs.

Detailed rows are still selected for lateness: the combined 8401..9000 sample has 177 rows but only
15 consecutive logged frame pairs, **none following a logged short frame**. Thirteen pairs following
a game-late frame had next acquire-pacing median 0.001 ms, mean 0.954 ms; the other two followed
on-time/compositor-late frames and both had 0.001 ms. Examples: 8481 game/acquire-pacing
37.737/6.347 ms -> 8482 37.537/2.056 ms; 8766 38.285/8.772 -> 8767 41.389/3.730.
Those rows do not support a growing acquire-deadline feedback loop, and cannot answer the missing
previous-short case. Reproducible sequence extraction is in
`.scratch/FramePacing/analyze_acquire_sequence.py` and `acquire_sequence_analysis.json`.

### All-frame main-fence evidence

`TOTK_FRAME_TIMELINE=1` also emits **every captured main frame-end wait**, not just late frames:
`[frame-tl fence]`, `[frame-tl fence-summary]`, and `[frame-tl fence-coverage]`. No extra switch is needed.
Static inspection of the local 1.4.3 rewritten `main.patched.bin` (SHA-256
`bad348845b8f786edfe27c3c44b97528727602e33fea978dd5e55987a981c5ee`) identified the NVN wait calls at
main+0x2db96c (group 1, frame-end sync) and +0x2db998 / alternate +0x2dbad4 (group 2, alternating pair).
The callback `[sys+0x158](1)` at +0x2db9c0 follows them; the subsequent vtbl+0x118 target in the inspected
static vtable is a no-op, **not** a known frame-kick function. This corrects the earlier comment.
Disabling the game's entire wait-mode branch also skips that callback and stops rendering; it is not a safe fix.

Each row has guest callsite/thread, sync pointer, raw fence bits and ticket, effective lag-redirected ticket,
setter frame/thread/time, NVN condition/flags, enter/exit and immediate/timeout status. Raw/effective ticket
records independently identify submission frame, seal/queue times, CPU observation of its GPU fence, and
completion publication **after CPU posts/UBO cleanup, immediately before the `g_done` store**. Absolute
timestamps and offsets from the previous game present allow sequence reconstruction even for on-time frames.
Setter sources distinguish queue fences, immediately CPU-ready acquisition syncs, and acquisition/queue
translation tokens; tokens are explicitly **not** looked up in the numerically overlapping GPU-ticket namespace.
Condition 0 declares `ALL_GPU_COMMANDS_COMPLETE`; 1 declares `GRAPHICS_WORLD_SPACE_COMPLETE`.

`completion=immediate-already-satisfied` means the zero-timeout fast probe succeeded.
`gpu-progress-observed` means the effective GPU fence was CPU-observed between enter/exit;
`gpu-already-observed-cpu-publication-pending` means that GPU observation preceded entry but publication
fell inside the wait. These are observed ordering classes, **not proof of the exact hardware signal or sole
cause of a wakeup**. Missing/evicted/contended/conflicting metadata and translation tokens remain explicit;
coverage counters must accompany any attribution. Instrumentation never signals a fence earlier.

The existing logs lack these per-wait anchors, so their 6–12-ms GPU tails cannot establish how much the
main actually waited for the previous frame. In the next device capture group by callsite and effective
submission frame; compare wait exit against GPU observation and final publication, and then against main
signal-ready. Only that can distinguish remaining previous-frame GPU serialization from update work or
CPU post-processing. Even if frame-private snapshots protect buffer reuse, they do **not** prove that the
game has no CPU-visible GPU-result dependency. No early CPU signaling or weakening of NVN completion is
implemented without the actual condition and consumer evidence.


### Android pacing defaults and controls

The Android app now defaults to `TOTK_ACQUIRE_SLACK=1`, `TOTK_PRESENT_SLOTS=1`, `TOTK_UNCAPPED=0`,
`TOTK_PACE=0` and `TOTK_ACQUIRE_CLOCK=0`, for both the shipping controller and FPS60. These are applied after
reading `totk.env`, only for unset variables; explicit values win. `TOTK_ACQUIRE_SLACK=0` disables producer
lead; `TOTK_PRESENT_SLOTS=0` disables fixed slots. Set **both 0** for the capped legacy A/B. Presenter buffers
remain three by default, FPS60 remains off by default, and real-VI/display-take remains a separate opt-in.

Operator-reported Thor Lookout sweep, c23d98e, lag on, chunk 600, timeline on, **copy presentation**:

| Measurement | Baseline (slack/slots 0/0) | Combined (1/1) |
| --- | ---: | ---: |
| Submit intervals within target ±4 ms, per 300 | 198 | 277 |
| Submit late / early, per 300 | 47 / 55 | 11 / 12 |
| Submit interval p95 | 40.6 ms | 35.9 ms |
| In-game mean FPS | 29.71 | 29.62 |
| Screenrecord ~33-ms intervals | 576 | 723 |
| Screenrecord long >41.7 ms / short <25 ms | 85 / 92 | 13 / 14 |

The game-present lateness distribution did not improve materially (late/300 55–76 versus 61–97); the result
supports steadier displayed/queued cadence, **not** improved producer throughput or measured input latency.
Logs are `.scratch/bench/t-pc-base.log` and `t-pc-slack.log`; these are operator observations, not a device
rerun by this worker. The user chose default-on in both FPS modes following that result. Odin 60-fps
regularity and end-to-end input latency still require the controlled device comparisons below.

**`TOTK_ACQUIRE_SLACK=1` with `TOTK_PRESENT_SLOTS=0`** advances a texture's *synthetic* availability by one requested game period:
`slot + period` becomes `slot`. This minor phase control retains the same `nvn_pace` clock and GPU reuse wait.
It can simply shift steady-state phase; it is not expected by itself to bank short-frame time.

**Combined with `TOTK_PRESENT_SLOTS=1`, slack uses a bounded lead gate instead of the synthetic availability
gate.** At most two handed-off pictures may be awaiting their slot/post: the mandatory next picture plus
**one extra**. A credit retires at successful copy-post acknowledgement or the CURRENT AHB transaction's
completion (not the PREVIOUS buffer's release fence). Failures/cancellations retire once; GPU/compositor
buffer lifetime remains protected independently. Acquisitions also have a virtual physical-lattice budget:
release k cannot precede its fixed acquisition phase minus one game period. This limits average simulation
rate even when a missing surface or pre-enqueue drop creates no presentation credit. Startup/stall recovery
permits only the bounded extra frame, never credits proportional to time spent suspended.

This lets short frames bank time for a subsequent long one while presentation keeps fixed slots. For example,
5-ms work followed by 25-ms work at a 20-ms period can start the long frame at +5 and finish at +30 for its +40
slot, rather than start at +20 and finish at +45. It does not create a third guest texture, remove GPU waits,
make sustained over-budget work fit, change the shipping controller, or rewrite simulation time. The intended
Thor control is `TOTK_PACE=0`, `TOTK_UNCAPPED=0`, `TOTK_ACQUIRE_CLOCK=0`. Slack is ignored with the fixed acquire
clock, display-take, deterministic, uncapped or interpolation pacing. Expect roughly **one additional game
period of queued input latency** when the extra pending picture is occupied; other existing pipeline stages
and missed slots can add latency. Measure this rather than treating buffering as free.

Slack is **not limited to the shipping 30-fps controller**. With `TOTK_FPS60=1`, interval 1 on a 60-Hz
panel gives the same one-extra-picture budget but a 16.67-ms lead instead of interval 2's 33.33 ms.
The Android app now supplies `TOTK_UNCAPPED=0` by default; keep it explicit for reproducible A/Bs.
An explicit uncapped override suppresses slack, retaining the profiling path.
`TOTK_PACE=display` uses its separate no-catch-up acquire policy and likewise does not use this slack gate.
Both copy and AHB presenters support combined slack. **`TOTK_ACQUIRE_SLACK=2`** opts into a second banked
real frame (two-period acquisition lead, pending limit **3**). Default remains **1**. It requires
`TOTK_PRESENT_SLOTS=1` and four independent presenter buffers: AHB CURRENT plus three pending pictures.
Unset `TOTK_PRESENT_BUFFERS` auto-selects **4** for slack 2; an explicit value resolving to fewer than four
refuses slack 2 with `[nvn] acquire slack 2: refused: requires TOTK_PRESENT_BUFFERS>=4...`, without raising
that explicit override. `TOTK_FRAMEGEN=interp` (also generated-frame mode `1`) refuses slack 2: the reserve
currently accounts for real pictures only. Other pacing exclusions above still apply.

The same virtual acquisition clock releases k no earlier than its phase minus **two** periods; the same
FIFO presenter retains each immutable slot, waits for the GPU, and retires credits at copy-post ACK /
CURRENT AHB completion. Short frames refill the bounded reserve; suspend/stall time never accumulates
unlimited credits. No guest texture, fence redirection, GPU/CPU timing report or controller instruction is
changed; `TOTK_FRAME_LAG=1` retains its independent protected-lag behavior. Sustained over-budget production
and overruns exceeding the reserve still hitch. The cost versus slack 1 is up to **one more game period
(~33.3 ms at 30 fps)** of queued input latency; fully occupied slack 2 can add two periods versus lead 0.

#### Two-frame reserve Thor A/B (`TOTK_ACQUIRE_SLACK=2`, opt-in)

Use these exact common `totk.env` lines for the 60-Hz Thor / shipping 30-fps comparison:

```ini
TOTK_FPS60=0
TOTK_FPS_AUTO=0
TOTK_REALTIME=1
TOTK_UNCAPPED=0
TOTK_PACE=0
TOTK_ACQUIRE_CLOCK=0
TOTK_PRESENT_SLOTS=1
TOTK_PRESENT_BUFFERS=4
TOTK_FRAMEGEN=0
TOTK_FRAME_LAG=1
TOTK_ASYNC_REPLAY=0
TOTK_VK_REC_THREADS=2
TOTK_VK_CHUNK_DRAWS=600
TOTK_VK_PRESENT=ahb
TOTK_FRAME_TIMELINE=1
```

Run **A** with `TOTK_ACQUIRE_SLACK=1`, **B** with `TOTK_ACQUIRE_SLACK=2`; use four buffers in BOTH to isolate
admission from storage. Separately omit `TOTK_PRESENT_BUFFERS` in B to check its automatic four-buffer choice,
and use `3` once to check refusal (not a valid B result). Remove other `DM_PRESENT_*` / deterministic
overrides. Fresh-launch each trial, warm at least 180 s, then follow the same route including heavy stretches
for at least 8,100 frames; reverse B/A order and match save, weather, camera, driver/cache, power and thermals.
Repeat with `TOTK_VK_PRESENT=copy` as a separate path experiment, never mix copy-post with AHB fence latency.

Require `[nvn] acquire slack: enabled (TOTK_ACQUIRE_SLACK=2); lead 2 periods, pending limit 3...`,
`[totk-pace] acquire slack admission: lead 2 period, pending limit 3...` and
`window present: 4 presenter buffers` in B; A must report lead 1 / limit 2. Compare all 300-frame windows:

- `[frame-tl reserve] ... banked-after-slot(0/1/2)=... hitches(absorbed/shown)=... late-ready=...
  skipped-slots=... missing=... incomplete-lookahead=...` retrospectively samples each immutable game slot
  using handoff plus independently published final-ticket readiness. Depth is FIFO-ready successors
  **after serving the due picture**, not credits for unfinished work. Skipped slots count depth 0.
  Hitches mean game-present intervals > target +4 ms; ready by slot = absorbed, otherwise shown. These are
  **readiness proxies**, not proof of actual scanout/counterfactual hitches; missing readiness stays missing.
  `display-interval-hitches/pairs` independently counts >target+4-ms intervals between observed consecutive
  AHB presentation fences (coverage denominator included); copy/missing display pairs cannot count as smooth.
  B defaults timeline on only when unset; explicit `TOTK_FRAME_TIMELINE=0` disables these reports.
- `window present-fence cadence, last 300 results`, `[frame-tl dist] ... clock=display` and screen-record
  intervals: compare 50/66/83+ ms tails, especially the clustered heavy stretches. Require signaled/missing
  coverage, ordering (no reversed frames), and no growing slot skips or presenter drops. Copy exposes
  submission/ACK, not a hardware display timestamp.
- `[frame-tl input-dist] ... association=global-window-observation metric=last-read-to-AHB-present-fence`
  p50/p95 versus A (and first-read metric / sample coverage). This matched difference measures added observed
  input-read-to-presentation latency; never subtract unrelated frames or substitute a predicted 33 ms.
  Main-wake snapshots remain separate and may be no-read after input-reader migration. Copy comparisons use
  `last-read-to-copy-buffer-post` instead and cannot establish scanout latency.

Host `acquirerunaheadtest` uses the production clock/admission/slot policies: repeated clusters of five
42–46-ms frames between 24-ms early stretches at 30 fps, ordered slot delivery, bounded pending/latency,
reserve refill, a >two-period overrun, missing-surface/suspend bounds and configuration refusals. It models
readiness and FIFO posts, not Android compositor behavior. Device acceptance remains the operator's A/B.


#### Deadline-predicted admission (`TOTK_ACQUIRE_SLACK=predict`, opt-in)

The default remains **1** until a matched device A/B accepts prediction. `predict` requires fixed slots,
`TOTK_PACE=0`, `TOTK_UNCAPPED=0`, `TOTK_ACQUIRE_CLOCK=0`, no deterministic/interpolation pacing, and synchronous
replay (`TOTK_ASYNC_REPLAY=0`). Unsupported combinations log the reason and retain their prior path.
No external latency-controller code is copied; this is a bounded original policy in `predict_pace.h`.

**Why the first predictor pinned full lead:** operator Thor AHB runs of `1ce8ae0` had strong cadence
(4/9 and 2/6 screenrecord long/short pairs versus slack1's 11/12 and off's 125/126), but warmed lead stayed
33.333 ms and median prediction error stayed about -13.7/-11.7 ms. That error is relative to
`release_target + predicted_cost`, **not** directly to readiness deadline D.

Eight-sample startup was not the continuing cause: `t-pr-predict.log` ids601..900 already reached
18.768/19.310-ms lead p50/p95. Two ratchets then prevented recovery: forecast/error increases restarted a
120-consecutive-quiet-frame decay gate, and physical feedback incorrectly treated `display > S + R/2`
as a missed slot. S comes from app-callback time, not a reconstructed SurfaceFlinger presentation phase.
That same report had **0 work misses but 131/300 alleged physical misses**, while the actual display
interval distribution had **297/300 within target ±4 ms and only 2 long intervals**. Normal compositor phase
was repeatedly inflating allowance and resetting decay. Raising a tolerated miss budget without correcting
that physical classification would leave a second source of pinning.

The supplied AHB fences now work: operator reports **8999/9000 signaled** (the most recent pending fence
can remain deferred), with real global-window last-read-to-AHB-present-fence p50/p95:
off **37.7/55.2**, slack1 **57.8/60.8**, predict **55.0/59.4** and **50.7/59.1 ms**.
Those are device observations, not worker reruns. Per-report extraction is retained in
`.scratch/FramePacing/miss-budget-analysis.json`; the current budgeted controller needs a new A/B.


At NVN acquire, reserve immutable frame identity and slot **S** on `vk_present_slots`' existing physical lattice.
Transfer that exact plan by guest texture identity to the presenter, not by a thread-local current-frame guess.
The backend copies S verbatim to AHB desired-present time or copy post floor; it does not independently
reserve/rephase the picture. Actual pending credits remain **<2**, never `pending==0` on every prediction.
Missing active plans explicitly drop that independent display copy with a diagnostic, never unpaced fallback.

Let **P** be the requested game period, **A** the measured presentation allowance and **C** the causal
readiness span: `actual gate release + wait-stripped work - observed post-target credit hold - original release target`.
C includes scheduler/resource delay after the target, but excludes deliberate admission-credit and presenter
FIFO/slot residence. Credit hold is measured only while blocked after the target; ordinary deadline wake jitter
remains. For
`b = TOTK_PREDICT_MISS_PCT / 100` (default **2%**), Q is the rolling nearest-rank `(1-b)` quantile:

```
D = S - protected_Q(A)
release = clamp(D - protected_Q(C) - margin, S - 2*P, S - P)
lead = (S - P) - release                     # 0 <= lead <= P
```

Use absolute `CLOCK_MONOTONIC` condition deadlines for fractional lead, not a spin or independent FPS limiter.
If release is already past, proceed immediately once credit/resource conditions permit; do not sleep another
period. A stale/suspended callback clock cannot grant an unlimited bank. More-than-one-period stalls and
refresh/interval changes reset prediction generation; already queued plans remain immutable and stale feedback
cannot train the new mode. Separate copy/AHB models are reset at interval/refresh changes, never mixed.

Each path keeps **300** samples, with quantile rank `max(1, ceil((1-b)*n))`. At n=300, budget2% uses
rank294 (six upper-tail samples are permitted), budget1% rank297, and budget0 uses the maximum. The first
**eight** valid readiness samples retain full-period lead. Fixed margin is `min(0.5 ms, P/16)`.
Protected forecasts rise immediately only when their realized rolling miss rate exceeds the budget;
otherwise they converge toward the current quantile by at most `min(0.125 ms, P/128)` per observation,
including decay. Isolated permitted misses do **not** reset a quiet-frame counter: there is no such counter
in predict. This is a statistical tail budget, not a guarantee every frame fits or a hardcoded 15–20-ms lead.

- **AHB ready:** independent final-ticket completion-publication observation, after CPU posts and `g_done`
  availability; it is not a hardware GPU timestamp or a delayed presenter-dequeue clock. C includes required
  texture/GPU resource waits after gate release. Allowance measures surface setup plus the apply call only,
  excluding one-time bridge/surface creation, OnComplete/previous-buffer-release waits and desired-slot residence.
- **Copy ready:** prepare lock/copy before its intentional slot floor (predict only), then post no earlier than S.
  C joins `(independent publication-ready - gate release) + measured copy preparation`; it excludes presenter
  FIFO residence and explicit slot holds. Actual wall post-ready time is retained separately. Copy allowance
  is measured post-call duration, not the deliberate wait. Legacy copy ordering is unchanged.

Work prediction error uses that credit-hold-stripped virtual ready endpoint, not a timestamp inflated by intentional
queue holds, relative to `original release target + protected predicted cost`. `[totk-predict]` logs work-ready
and actual wall-ready misses separately, plus excluded queue time.
Actual AHB fences calibrate `display - S` using a rolling lower-decile phase baseline. After initial calibration,
only an excess beyond **baseline + half a refresh** counts as a physical miss; normal fixed app-callback/HWC
phase does not. A non-additive budgeted phase-tail allowance is bounded to P/2 and decays independently of
isolated permitted misses. This adjusts readiness insurance, never S or measured work. A constant whole-refresh
phase shift is unidentifiable without an external display-slot identity; no claim of Android latch-deadline
recovery is made. Without valid fences, no physical samples are fabricated.

Every **300 terminal prediction frames** (completed or cancelled), `[totk-predict]` reports chosen lead p50/p95,
work-ready prediction-error p50/p95, work/wall misses, excluded queue residence, cancellations, path mismatches,
generation resets and AHB display-feedback/miss counts. A mutex-snapshotted model block adds path/interval/epoch,
period, current cost/allowance/margin, rolling readiness and phase-relative display miss counts/rates versus the
configured budget, and calibrated phase. Deferred display feedback may lag a terminal batch. These model rates
are rolling windows, not the terminal batch's differently delimited totals.
Prediction supplies `TOTK_FRAME_TIMELINE=1` only if unset, to emit the existing per-300
`[frame-tl input-dist] association=global-window-observation` first/last read-to-equivalent-endpoint latency.
Explicit timeline 0 suppresses those input diagnostics; the predictor's own readiness model does not depend on them.

**Phase 2 is intentionally not applied:** no wait is patched into main+0x2db8ac. Required waits/callbacks and
worker-side input/update consumption have not been established as a safe pre-simulation barrier; gameplay Npad
sampling occurs on separate ~60-Hz workers. Moving a main wait without that ownership proof can merely serialize
useful overlap or change semantics. Phase 1 claims renderer admission control, not full Reflex-style input ownership.

#### AHB present-fence recovery and the API33 bridge

The merged `f300b30` JNI SurfaceControl-parent bridge made AHB work on the Thor. Operator `t-sc33-ahb` observed
300/300 AHB submissions, **1 long / 5 short of 751 screenrecord frames**, 29.89 in-game fps, and live-read-to-AHB-apply
p50/p95 **17.4/28.5 ms**. It still had no present-fence endpoint, so these are not physical read-to-display numbers.

The old decoder closed a valid-but-pending present fd immediately. Current code retains it with copied
frame/apply/generation/prediction identity and harvests it later, without blocking the presenter or binder callback.
Resolved fds close once; bounded eviction and surface teardown close unresolved ownership explicitly.
`window present fences:` logs signaled/deferred/pending, absent, ioctl/fence errors, oversized/invalid timestamps,
eviction and teardown counts. The first few failures also log errno and queried framework latch time.
OnCommit cannot replace OnComplete: the public contract exposes no present/release fences there.
Latch time and previous-buffer release fences are **not** relabeled current-frame present-fence latency.

The historical log lacks fd/query-status diagnostics, so pending-fence loss is a proven source bug, **not**
a proven diagnosis of every Thor missing endpoint. If a device returns absent fd -1 or no usable kernel
timestamp, this remains explicit unknown; require growing `signaled`/AHB-display-feedback and nonzero
global-window `last-read-to-AHB-present-fence n` before treating physical deadline calibration as accepted.

**Predict acceptance plan (not device-run here):** use the working AHB bridge on **both** Thor 30 and Odin 60,
60-Hz panels, protected lag, two recorder workers and chunk 600, same save/camera/cache/driver/thermal state:

```
TOTK_ACQUIRE_SLACK=predict             # compare 1, predict, 0
TOTK_PREDICT_MISS_PCT=2                # predict only; compare 1/2% with all other settings fixed
TOTK_PRESENT_SLOTS=1
TOTK_PRESENT_BUFFERS=3
TOTK_VK_PRESENT=ahb
TOTK_UNCAPPED=0
TOTK_PACE=0
TOTK_ACQUIRE_CLOCK=0
TOTK_ASYNC_REPLAY=0
TOTK_FRAME_TIMELINE=1
TOTK_REALTIME=1
```

Set `TOTK_FPS60=0` on Thor and 1 on Odin. Keep slots on in the primary **1/predict/0** comparison; retain
0/0 as the separate legacy cadence control. Warm >=180 seconds, include quiet and camera-sweep phases, reverse
run order. Require the JNI parent/SurfaceControl/AHB success messages with no fallback. Check predictor lead
p50/p95 in [0,P], valid readiness coverage, zero unexpected missing-plan/path/generation failures, and real
present-fence counts/reasons. Compare screenrecord long/short counts, compositor/present-fence p95 and global-window
read-to-**the same endpoint** p50/p95 together, not just earlier AHB apply or mean FPS. Thor copy is supplemental,
not a substitute for the now-working AHB acceptance path. Recheck pause/resume, surface recreation, mode changes,
clock/audio speed and GPU/texture correctness. Reject a latency win that loses the cadence acceptance.

No fixed device latency saving is claimed. Target warmed lead is **15–20 ms on the supplied Thor walk**,
but measured tails choose the result; require single-digit long screenrecord frames per roughly25 seconds
alongside latency improvement, not just the requested budget in the log. Maximum one-picture opportunity is
bounded near **33.33 ms** at30 / **16.67 ms** at60, and necessary joined-tail lead can consume much of it.
Odin must use its own measured work, never Thor's walk timing. AHB-apply latency is not display latency.

Host checks passed for this budgeted-policy integration: `predictpacetest` (exact0/1/2/5% quantile ranks,
permitted-miss decay, above-budget fast rise, tail expiry, startup, phase/stall/mode/overflow bounds,
credit/FIFO exclusion, constant10–14-ms phase and true extra-refresh tails), `predictbudgettest`
(production decimal/scientific parser, exact rounding/range/invalid/default cases), `presentfencetest`,
timeline/fence enabled/disabled and acquire/display/slack/slot tests. Predictor/parser/fence tests also passed
ASan+UBSan. Android `cc1.sh` passed frontend_android.c, nvn_vk.c, vk_draw.c, vk_post.c and vk_overlay.c.
The preceding1ce8ae0 integration passed10 units and the full20-case lavapipe presenter matrix; that matrix
was not rerun for this policy-only change. These are not Android lifecycle, scanout or latency acceptance.
Current logs: `.scratch/FramePacing/miss-budget-host/`; historical matrix: `.scratch/FramePacing/present-slot-tests/`.
Reproduce the new pure tests from the project root:

```
make -C wt-frametl/games/totk/android TARGET=linux CC=cc \
  OUT="$PWD/.scratch/FramePacing/miss-budget-host" predictpacetest predictbudgettest presentfencetest
.scratch/FramePacing/miss-budget-host/predictpacetest
.scratch/FramePacing/miss-budget-host/predictbudgettest
.scratch/FramePacing/miss-budget-host/presentfencetest
```


**`TOTK_ACQUIRE_SLACK=auto`** is the opt-in latency-reduction experiment; the default remains **1** (always
allow the extra picture). Auto requires `TOTK_PRESENT_SLOTS=1` and the same eligible capped path. It starts
with zero extra producer lead; a game-present interval **strictly greater than requested target +2 ms**
enables one extra picture immediately. Each further late interval resets the quiet counter. Only **120
consecutive non-late game intervals** drain the extra picture: about four seconds at 30 fps or two at 60.
These are game-present, not compositor intervals, and do not depend on timeline diagnostics being enabled.

Zero/one lead uses the same fixed virtual acquisition phase and respectively one/two pending-picture limits.
Turning lead off delays the next admission one additional slot while already queued pictures display normally;
it neither skips a presentation slot nor resets the presenter's phase. Turning it on admits only the bounded
extra picture, not an unbounded catch-up burst. Hysteresis prevents a single short frame from draining/refilling
the queue. Auto cannot remove the GPU tail or sustained CPU overload, and a frequently late scene may keep it on.
It preserves the requested interval and existing measured simulation-time code.
The single interval immediately after a quiet-mode transition is labeled **intentional drain** and is
excluded from the late trigger, while its raw duration remains logged. Otherwise the intentional extra-period
admission wait would instantly refill the queue and defeat latency reduction. This guard covers exactly one
interval; subsequent late intervals reactivate normally. A genuine hitch coinciding with that drain interval
is therefore not classified by the detector, an explicit one-interval tradeoff rather than hidden clock filtering.


Expected latency **difference**, not a measured absolute result: when quiet auto has drained a previously full
extra-picture budget, read-to-presentation latency can fall by about one game period versus always-on:
~33.33 ms at 30 fps, ~16.67 ms at 60 fps. Existing slot lead, GPU work and copy/SurfaceFlinger latency remain;
actual p50/p95 must come from the input timeline below. Do not claim that every frame or every scene saves a period.

**`TOTK_ACQUIRE_CLOCK=vsync`** is a separate, minor no-lead control, default off. It replaces the legacy
availability/display-take acquire gate with a fixed Choreographer-aligned acquisition lattice: each reserve
advances by the NEW requested interval × physical refresh period, not by prior presents. Ordinary late/short
frames retain phase; a stall strictly greater than one game period or a real refresh-mode change reanchors.
`0` disables it. When eligible it replaces capped synthetic/explicit-Hz acquisition and takes precedence over
slack; uncapped, interpolation, deterministic or `DM_PRESENT_HZ=0` keep their prior policy instead.
Clock-only use does not replace the legacy VI source.
Since the unset-PACE synthetic clock already retains ordinary phase, do not assume this control fixes judder.
`[totk-pace] acquire clock frame ...` reports wait, lateness and reanchor counts every 300 acquisitions.

**`TOTK_PRESENT_SLOTS=1`** schedules the presenter's buffers on a real `AChoreographer` callback lattice, with
one requested game period of initial slack after the latest physical callback. Subsequent desired times advance
by the guest-requested number of refreshes (two at 30 fps on a 60-Hz panel), not by each jittered arrival. A late
then short frame can catch up without rephasing; more than one game period behind skips whole missed slots
instead of leaving all subsequent deadlines permanently in the past. Interval and physical-refresh changes
update the schedule without making future frame deadlines go backwards.

The producer only snapshots an already-initialized display clock; it never waits for display callbacks while
holding the graphics/recorder lock. Frontend startup initializes that clock before launching the guest. With
`TOTK_PACE=0`, the existing VI event thread is **unchanged**: this Choreographer supplies presentation and
the selected acquire control, not extra/fake VI signals. Only `TOTK_PACE=display` replaces the VI source.

- **AHB:** after the existing GPU wait, the default submits `ASurfaceTransaction_setDesiredPresentTime`
  equal to the **nominal slot exactly**. Experimental locked-only `TOTK_PRESENT_FLOOR_NS=1`
  instead passes `slot - 1 ns` to avoid an observed strict-equality eligibility rejection in Odin's
  SurfaceFlinger; reservations and API timeline metadata keep the exact slot. Separate
  `TOTK_PRESENT_GUARD_NS=125000` admits only a measured 125 μs predictor guard, strictly below
  half a physical refresh including the floor. `TOTK_PRESENT_DEDUPE=1` separately prevents
  near-identical cross-snapshot API targets from reserving one refresh twice. All are default-off.
  Existing completion/release waits still protect buffers; present fences remain diagnostic evidence.
- **Copy:** public lock/post cannot assign a native-window buffer timestamp. After the GPU wait, the presenter
  waits for a physical callback matching/reaching the nominal slot **and** host monotonic time >= that slot,
  then locks, copies and posts. There is no one-refresh-early submission window. Callback matching tolerates
  fractional-period rounding, but cannot deliberately post before the slot. This conservative public API
  path can therefore display **a refresh later**, and is not proof of scanout at the desired slot.

Expected benefit: absorb ordinary CPU/GPU arrival jitter while a ready frame is buffered, reducing physical
long/short pairs even when the game's raw present intervals remain variable. It cannot cure sustained work
above the frame budget: a 29.85-fps producer cannot supply fresh 30-fps pictures indefinitely, so eventually a
slot must repeat/miss. Initial presentation slack plus the combined extra producer picture increase latency.
This is a bounded queue, not a throughput improvement or a fixed end-to-end latency guarantee.

**`TOTK_PRESENT_BUFFERS=4`** optionally expands only the independent presenter pool from three to four pictures
(default remains three; allocation remains lazy). This may absorb a catch-up burst when current + pending
frames already occupy the three-picture pool. It is not guest triple buffering and cannot by itself shorten
NVN acquire. The extra picture costs memory when used and can permit another period of queued latency.
Use the unchanged three-buffer pool for the primary slot-queue comparison, then test four separately.

Keep the common reproduction settings from the timeline section, adding:

```
TOTK_UNCAPPED=0
TOTK_REALTIME=1
TOTK_PACE=0
TOTK_ACQUIRE_CLOCK=0
TOTK_VK_PRESENT=copy
```

| A/B | `TOTK_ACQUIRE_SLACK` | `TOTK_PRESENT_SLOTS` | `TOTK_PRESENT_BUFFERS` |
| --- | ---: | ---: | ---: |
| Legacy capped control | 0 | 0 | 3 |
| Producer lead only | 1 | 0 | 3 |
| Slot queue only | 0 | 1 | 3 |
| Default combined | 1 | 1 | 3 |
| Combined + extra presenter buffer | 1 | 1 | 4 |
| Adaptive latency control | auto | 1 | 3 |

As a separate minor control, repeat baseline with only `TOTK_ACQUIRE_CLOCK=vsync`; keep slack off.
The primary phase-lead comparison is baseline versus **Combined (three buffers)**.

Run each warmed Lookout camera sweep for >=180 s, then reverse the order; keep the panel at 60 Hz and change
only the row's settings. Repeat the useful pair with `TOTK_VK_PRESENT=ahb`, checking for no copy fallback, so
display-fence intervals can confirm whether copy-post improvements actually correspond to smoother display.
Retain all frame distributions, slot misses/skips, shown/dropped/cancelled counters and camera video; compare
actual display p95/p99/long-frame fraction, not just a 29.9-fps average. Check `acquire_pacing` versus
`acquire_texture` after integrating the additive timeline split. Also verify pause/resume, window recreation,
refresh-mode changes and normal game/HUD-clock speed; measure input response with the same video method.
These controls retain explicit opt-outs. Device regularity evidence above does not establish their input latency.

Keep `TOTK_REALTIME=1`: the existing producer replacement at main+0x3e5658 and
`runtime_frame_step_update` use five measured frame intervals (clamped 1/120..1/15 s), rather than assuming
one nominal simulation tick per render. See [APP.md](APP.md#display-paced-frame-acquisition-opt-in) for
the controller and secondary-animation evidence. These controls leave that code untouched. Their bounded
catch-up can still change instantaneous measured steps and the five-frame history; it is not a promise
that every undiscovered frame-counted consumer or input sampler is unaffected. A paced average 30 plus
HUD-clock/video comparison is the device requirement, not an inference from the FPS counter alone.

For the default combined path, expect `[totk-pace] acquire slack admission: lead 1 period, pending limit 2`
and `window present slots: ... never deliberately early`. Auto logs its initial quiet state, late activation,
120-quiet-interval drain and the intentional-drain interval; admission logs switch between lead 0/limit 1
and lead 1/limit 2. The startup message reports the acquisition **base policy** and **slack request**, not
final HLE eligibility; check the subsequent `[nvn] acquire slack` and admission lines for activation.
In the operator's older `t-pc-slack.log`, startup `acquisition legacy` at line 24 was misleading:
lines 96 and 97 confirm both slack and the gate were active. Startup now distinguishes the requested override.
Preserve all `[frame-tl fence]` rows and
`fence-summary`/`fence-coverage`, even for on-time frames. Compare group-1/group-2 wait distributions,
effective frame age, immediate fractions and observed GPU/publication anchors against main-ready and
acquire pacing. This run should answer whether previous-frame completion still serializes the main.


### Input freshness and latency A/B

Android controller input is already updated just in time; no additional latch or polling thread is introduced.
`frontend_android.c:queue_cb` drains the `AInputQueue` on the NativeActivity UI Looper. `handle_input` updates
buttons/axes under `g_pad_mu` immediately for each delivered event. `runtime/input.c:pad_states`, the
`nn::hid::GetNpadStates` full-key/handheld override, calls `runtime_gamepad_read` synchronously on the calling
guest thread and returns the latest mutex-protected state. A short press remains in `g_edges` until that read,
preserving the existing edge semantics. There is no periodic HID shared-memory publisher or fixed runtime
controller polling period to remove: OS delivery rate is device-dependent, and the game's actual read rate
is determined by its calls. The timeline counts those calls and records their guest thread identity.

Consequently, republishing on Android events or polling again after the game has consumed input cannot
legitimately late-latch the same simulation frame; it would either duplicate existing work or change game
semantics. The separate diagnostic `TOTK_INPUT_FILE` reader polls every 100 ms; that is not the real-controller
path and must not be mistaken for normal controller latency.
With `TOTK_FRAME_TIMELINE=1`, each connected Npad state formation records `CLOCK_MONOTONIC`, its actual
sampling number and guest thread. Per-frame `[frame-tl input]` rows expose first/last/count in two distinct
associations:

- **`main-wake-snapshot`**: copy the main sender's TLS read group at the observed main->renderer
  `WaitForAddress` wake; attach the immutable group to the resumed render frame. This avoids assigning a
  next-update read to the previous image merely because main/render overlap. Each wake consumes only new
  sender reads; an empty wake is `no_read`, never an assumed reuse of an old sample. Missing handoffs remain unknown.
- **`global-window-observation`**: all connected Npad reads within the global walking/present window, retaining
  first/last reader identity, mixed/unknown readers and invalid timestamp counts. Use these counts/sampling
  IDs to determine observed read frequency; do not double-count the separate main-wake rows.

Neither association proves which simulation update consumed a sample for a particular image. Main-wake
ownership is the observed thread-handoff boundary, **not** reconstructed image causality. Raw-window samples
never silently fill a missing main-wake measurement. Read-to-present is not hardware-event-to-photon latency.

The existing 300-frame reporter (30-frame completion delay) emits `[frame-tl input-dist]` first/last p50/p95
in milliseconds, with `association`, `read_group=all`, sample count and missing/invalid/source counts:

- `first-read-to-compositor-submit` / `last-read-to-compositor-submit`: **AHB apply only**, timestamp captured
  immediately before `ASurfaceTransaction_apply`.
- `first-read-to-copy-buffer-post` / `last-read-to-copy-buffer-post`: entry to an
  `ANativeWindow_unlockAndPost` call that subsequently succeeded, **not** scanout or its delayed acknowledgement.
- `first-read-to-AHB-present-fence` / `last-read-to-AHB-present-fence`: actual current-transaction present-fence
  timestamp when available; copy/unsupported/pending fences cannot fabricate this endpoint.

`[frame-tl input-sources]` counts AHB/copy/missing submission evidence separately. Invalid chronology or
main-sender ownership is rejected; inspect coverage before comparing percentiles. The historical
`[frame-tl dist] clock=compositor-submit` copy **post-acknowledgement** timestamp is retained unchanged for
previous jitter comparisons; new input latency uses its separate pre-call endpoint, never that acknowledgement.
All per-read bookkeeping is bounded/TLS or existing-ring storage, without per-read allocation or I/O.

#### Gameplay reader migration: corrected latency evidence

Operator-reported 9701613 Thor Lookout sweep (copy, protected lag, chunk 600):

| Mode | Screenrecord long / short frames (~750 frames) | In-game mean FPS | Live-read-to-copy-post p50 / p95 |
| --- | ---: | ---: | ---: |
| Always-on slack + slots | 25 / 26 | 29.81 | 48.8 / 52.1 ms |
| Auto slack + slots | 35 / 29 | 29.65 | 47.8 / 60.9 ms |
| Slack + slots off | 108 / 111 | 29.51 | 18.0 / 27.6 ms |

These are operator-reported warmed comparisons from `.scratch/bench/t-pc3-{on,auto,off}.log`, not reruns.
Auto engaged at game-present 2 and frequently late frames kept it active; it did not reclaim useful latency
in this scene. On versus both-off adds about **30.8 ms** to median read-to-post timing, near one 30-fps period.
It is not a physical input-to-photon measurement, nor does the both-off control isolate acquire lead from slot scheduling.

The huge main-wake values were an instrumentation bug: gameplay Npad reading moved from main guest thread
`0x1001` to gameplay workers `0x1039`/`0x103a`/`0x103b`, but an empty main wake reused main's last old read indefinitely. In `t-pc3-on.log`
frame 7200 (lines 32795–32796), live reads are samples 13281/13282 on `0x103a`; main instead repeats sample
3227 at 133071313548558 ns and reports 169563.973 ms. That is **age of a stale diagnostic snapshot**, not
current gameplay input latency. Repeated snapshots/new-vs-reused distributions are removed, not age-clipped:
each sender read group is consumed once, and subsequent empty wakes contribute `no_read` and no percentile.
A real explicitly captured read may still legitimately have a long duration; no arbitrary age cap hides it.
Frames 6901..7200 contain **601 connected Npad reads over 10.02997 seconds (~59.92 Hz)**, independent of
the ~30-fps renderer; 176/300 raw windows mix reader workers, with no unknown thread IDs. This is an
observed game-side polling cadence, not a frontend periodic publisher or a measured hardware polling rate.
The count/source derivation is retained in `.scratch/FramePacing/input-migration-analysis.json`.

For these captures, the authoritative **live-read-window comparison** is selected by all three fields:

```
association=global-window-observation
metric=last-read-to-copy-buffer-post
read_group=all
```

For example, frames 6901..7200 report 48.433 / 63.827 ms on that row. It is still a global-window observation,
not proven input-to-image causality. Main-wake rows remain useful only when that sender actually read input;
after migration they should show `n=0`, `p50=nan`, `p95=nan`, and `no_read=300`, not hundreds of seconds.
The regression fixture moves live reads to another host/guest thread and advances time by 100 seconds while
main keeps waking; raw latency stays 19 ms and the empty main wakes supply no fake samples.

#### Remaining latency options (not implemented by the instrumentation fix)

`vk_draw.c` already applies **AHB immediately after its GPU fence**, setting desired-present time to the
reserved slot; only the copy branch calls `runtime_android_present_wait` (predict prepares its pixels first).
AHB is already the requested early-submit/at-slot-display route, not another callback-wait limiter to remove.
The public API says SurfaceFlinger tries to present at or after the desired time; it is not a guaranteed deadline.

Concrete residual: frame 7200's last GPU observation is +52.024 ms from previous game present
133240808355993 ns; copy-post entry is 133240877521358 ns. That is about **17.14 ms GPU-observed-to-post**
and another **0.63 ms to the historical post acknowledgement**. This one late row includes slot waiting and
copy work; it is not an all-frame mean or proof of 17.14 ms physical-display savings.

| Option | Expected opportunity at 60 Hz | Constraints |
| --- | --- | --- |
| Existing `TOTK_VK_PRESENT=ahb` | Up to about one physical refresh (**16.67 ms**) versus copy missing the nominal latch; apply entry can move earlier by its residual copy/slot hold | Require successful AHB import/no fallback and compare actual AHB present fences, not AHB-apply against copy-post endpoints |
| Prepare the copy buffer before the slot, then post at the same floor | Implemented only for `predict`; possibly 0 or **16.67 ms** if preparation changes a missed latch | Still preserves the nominal no-early floor; holding a locked BufferQueue buffer longer needs device/lifecycle validation |
| CPU-copy into an AHardwareBuffer, then SurfaceControl desired-time submission | Similar 0..**16.67 ms** latch opportunity without requiring the Vulkan dma-buf import path | New opt-in presenter implementation needed; retain CPU-copy cost and all compositor release-fence/buffer ownership rules |
| Android API33 frame timelines (`postVsyncCallback`, expected presentation/deadline + `setFrameTimeline`) | Better target/deadline alignment; potentially prevent a one-refresh miss, **no guaranteed saving** | Existing period/callback grid does not use preferred expected-presentation timestamps or vsync IDs; must measure actual fences and preserve guest intervals |

`ANativeWindow_unlockAndPost` in the installed public NDK has no desired-time argument or public buffer-timestamp
setter. Merely posting copy **one refresh early** can save up to 16.67 ms but may display early and reintroduce
the short/long pattern; it violates the fixed-slot never-early policy. A timestamped EGL/Vulkan swapchain or
the CPU-AHB/SurfaceControl route above would be a real presentation-path change, not a flag on the existing call.
Do not count initial slot lead, producer lead and GPU overlap as independent serial periods; compare
slack/slots **0/1** against **1/1** as well as the existing **0/0** control before assigning the entire latency delta.


At a fixed 60-Hz panel, compare Thor shipping 30 and Odin `TOTK_FPS60=1` independently:

1. Fresh/default check: omit the five pacing variables from `totk.env`; require the resolved policy log to
   show slack/slots 1 and uncapped/pace/acquire-clock 0. Keep FPS60 explicit on Odin. Then set slack/slots
   both 0 and require the old capped path; no fresh-install FPS60 window should resume the old 77–78-fps policy.
2. Hold driver/cache/scene/camera/thermal state, lag and recorder settings constant. With timeline on and
   three presenter buffers, compare slack/slots **0/0**, **0/1**, **1/1** and **auto/1**, reverse order,
   >=180 s per run. Include a quiet/static interval long enough to drain auto (>=120 quiet frames) and a
   camera sweep that triggers lateness. Preserve transition logs to identify mixed-state 300-frame windows.
3. Prefer AHB with no copy fallback when comparing actual read-to-display latency; copy gives buffer-post
   submission timing, not scanout. Compare valid input sample coverage and p50/p95 alongside displayed
   cadence, drops/skips, game mean, frame-time tails and screenrecord motion. Pair the runtime measurements
   with physical input/video if true hardware-event-to-photon latency is required.
4. Require quiet auto to stay at zero extra lead after its single drain interval and refill only on a later
   genuine late interval; already queued slots must remain continuous. Recheck HUD-clock speed, pause/resume,
   surface recreation and refresh-mode changes. These new latency/adaptive tests have not been device-run here.

Host acceptance passed: `test_present_slots.c` (cadence, jitter, bounded catch-up/stalls, mode changes,
fractional callbacks, strict never-before-target submission and unavailable-clock handling),
`test_acquire_slack.c` (two-image synthetic cadence), `test_acquire_clock.c` (30/60 fixed phase,
late/short, stalls/refresh/suspend, bounded credits, exact parsing, 120-frame hysteresis, planned-drain guard
and 400-frame gap-free immutable-slot transitions), and `test_display_pace.c`. Timeline and fence tests
passed enabled/disabled and **both passed ThreadSanitizer**. Timeline covers first/last/sampling IDs,
overlapping main/render frames, immutable snapshots, no-read/migrated-reader regression, TLS isolation, 300-frame percentile math,
missing/invalid/source-separated endpoints, ring expiry and out-of-order publication. Fence checks retain
raw/effective owner and publication coverage. `.scratch/cc1.sh` passed for `frame_timeline.c`, `frame_fence.c`,
`input.c`, `threads.c`, frontend, NVN HLE, presenter and `nvn_vk.c`.
Lavapipe passed all **8 post-chain configurations** (AA off/SMAA × scale off/FSR × linear/sRGB) and **12
overlay/presenter configurations** (overlay off/fps/full × 3/4 buffers × post off/combined), explicitly
exercising auxiliary slot 3 without bypassing post/overlay. Host slot mode also exercised its explicit
unavailable-clock legacy path; a host fixture is not an Android display-clock/scanout test.
Full pacing/default checks are under `.scratch/FramePacing/default-latency-host/`; the input-reader correction's
enabled/disabled timeline/fence tests, both ThreadSanitizer runs and Android cc1 logs are under `.scratch/FramePacing/input-migration-host/`.
Presenter logs and the reproducible host runner are under `.scratch/FramePacing/present-slot-tests/` and
`.scratch/FramePacing/check_presenters.py`. No device test was run for this default/latency/adaptive change.
The deterministic tests need no GPU/game data; run from the project root:

```
make -C wt-frametl/games/totk/android TARGET=linux CC=cc \
  OUT="$PWD/.scratch/FramePacing/default-latency-host" \
  frametimelinetest framefencetest acquireclocktest displaypacetest acquireslacktest
.scratch/FramePacing/default-latency-host/frametimelinetest
.scratch/FramePacing/default-latency-host/frametimelinetest off
.scratch/FramePacing/default-latency-host/framefencetest
.scratch/FramePacing/default-latency-host/framefencetest off
.scratch/FramePacing/default-latency-host/acquireclocktest
.scratch/FramePacing/default-latency-host/displaypacetest
.scratch/FramePacing/default-latency-host/acquireslacktest
```


## Per-pass GPU timing and statistics (2026-11-05)

`TOTK_VK_GPU_PROFILE=1` enables the isolated `gfx/vk_gpuprof.c` diagnostic; default is off.
The threaded recorder remains enabled; `TOTK_VK_REC_THREADS=0` is also supported.
`TOTK_VK_GPU_PROFILE_SAMPLE` defaults to **`0`**, preserving full profiling: every pass part retains
its original primary's query coordinates, and assembly closes its bracket after the secondary chunks.
For lower query volume, start with **`TOTK_VK_GPU_PROFILE_SAMPLE=2`** and statistics off; strict integer
budgets `1`–`16` select at most that many pass parts per frame by deterministic ordinal rotation.
Do not combine it with `TOTK_VK_PASS_PROFILE`, pass dumps, screenshots or the pass probe for performance measurements.

Opt-in startup prepares sixteen reusable timestamp/statistics query-slot leases on the dedicated
`totk-gpuprof` worker; the caller can wait for that initialization once. During play, taking a prepared
lease is a short private-mutex O(1) operation, with no query reset, result read, formatting or I/O.
`vkCmdWriteTimestamp2(ALL_COMMANDS)` brackets primary rendering instances (including attachment
loads/stores), compute dispatches and named backend upload/transfer/post/upscale/present/framegen
work. Sampled mode brackets only selected passes, plus one completion-accurate **whole-main-frame**
pair, with no pre/slot pairs. Per-frame `timestamp_count` counts actual profiler-owned
`vkCmdWriteTimestamp2` commands (including deferred assembly ends), not reserved pool size or the game's
independent timestamp reports: at least two and at most `2k+2` for budget `k`. All observed pass keys,
draws, areas and completed-frame scene metadata remain recorded, even with zero samples.
Existing fence completion detaches/enqueues the fenced profiler lease and immediately releases the
game ring slot; it does not read queries or report inline. The profiler worker owns result reads
without WAIT or polling, aggregation, JSONL/log output, query resets and lease return. Its bounded
sixteen-lease FIFO does not share the recorder-worker mutex. No free lease drops profiler coverage
(`background_dropped_slots`, cumulative, and missing frame coverage), never blocks the game for I/O.
CPU present independently publishes `{frame, end_sequence}` to a bounded 256-entry closure FIFO under
the private background mutex. The worker finalizes only after all fenced leases through that sequence
have been processed, including when they were consumed before CPU present. This supports Android's
sealed-then-present path without depending on the next frame. Full mode uses the last slot's actual
envelope end with **no extra GPU query, submission or per-frame fence wait**. With an open command buffer,
the existing in-buffer GPU marker remains supported. If SAMPLE present finds no open command buffer,
it issues **one small marker-only main submission** for the real frame-end timestamp: this extra submit
has explicit cost even though the actual timestamp bound remains `2k+2`. No endpoint comes from CPU time.
Closure overflow is explicit cumulative `background_dropped_closures`, separate from dropped query leases;
analysis uses the maximum per launch and marks coverage censored. Timestamp/statistics commands can still
serialize GPU work. A frame boundary closes an outstanding rendering instance for timing attribution.
Results/reports can arrive several frames late; a 300-frame report waits for fenced work and its ordered
CPU closure. The footer exposes `pending_frame_closures`, not a guarantee of GPU-query retirement.

Read `[gpu-profile] frames A-B` for summed measured render/compute ms/frame, actual LOAD,
CLEAR, STORE and DONT_CARE-store attachment operations/frame and logical attachment load/store
MiB/frame (**not actual traffic**). Full-mode JSONL reports the first-to-last Vulkan GPU span and
signed `gap_ms = span_ms - measured_ms`, including barriers, queue bubbles and remaining unscoped work.
Sampled spans have `timing_scope: "main-frame"` and deliberately exclude pre-envelope authority;
sampled `measured_ms`/pass `gpu_ms` include only sampled durations, so their signed residual includes untimed
work but can be negative when pre samples precede the main span or sampled scopes overlap.
That alone does not invalidate sampled span authority. Never extrapolate the sampled sum or compare it
as a complete pass reconciliation; the residual is not a total of untimed work.
The compact top-N signature rows are sorted by summed measured GPU ms/frame; percentages are
shares of that measured total, not the full GPU span. Schema **3** exact pass keys include
render/compute kind, absolute renderArea width/height, ordered attachment formats and absolute
width/height, sample count, color slot count, load/store ops and first-draw shader-content hash.
Draw counts, frame ordinals, scene references and guest addresses are not part of the key.
Absolute JSONL `render_width/height` and attachment `width/height` allow exact-area driver joins.

The common per-frame reference is its largest depth+MRT renderArea (at least two active color
attachments), emitted as frame `scene_width/height`; `0/0` explicitly means none was observed.
Pass `reference_width/height` use that scene extent, or `TOTK_RENDER_RES` when no scene was
observed (otherwise the game's handheld 1280×720/docked 1600×900 mode default). Attachment
fractions are exact reduced absolute/reference ratios, **never** divided by their own pass area.
Completed-frame observations are retained until that reference is known, even across retired slots.

Window aggregation defaults to exact key + reference extent + `scene_relative`, not frame ordinal.
Schema-3 `pass` ordinal **0** means **all frame ordinals**; `TOTK_VK_GPU_PROFILE_ORDINALS=1`
restores per-ordinal windows. This launch-only switch defaults to `0` (all values except exact `1`
are off), logged once as `[gpu-profile] window ordinals=0/1`. Mode-2 `[gpu-split]` and mode-3
`long_frame` rows always preserve positive actual ordinals; compacting window rows does not change
absolute areas, sample counts, counters or the statistics-matched time/draw denominators.

The analyzer aggregates by key/family over the **whole input/measured interval** by default.
`python3 tools/bench/gpuprof.py 'capture/gpuprof-*.jsonl*' --per-window --json analysis.json`
also exposes individual windows for debugging, not as the default sampled population.
Sampled groups retain `observed_count`, `sample_count`, observed/sampled frame coverage and the
272-bin histogram. `mean_ms` divides sampled sum by sample count; p50/p90/p99 use **merged histogram
bucket upper bounds**, never averages of per-window percentiles. Eighth-octave bins have up to about
9.1% upper-bound quantization above 1 ns; zero/sub-nanosecond values share the first bin.
Thin coverage means fewer than 10 sampled frames over the **whole interval**, not three samples
in an individual 300-frame window. There is no hard percentage gate: a 48-second capture with
15–30 samples/ordinal can be useful near 1% frame coverage. Keep that denominator visible.
Below 100 samples, p99 is flagged descriptive-only rather than a reliable stutter-tail estimate;
fine histogram buckets do not manufacture statistical confidence. Family frame unions can be bounded
rather than exact; the analyzer exposes that uncertainty. Missing/censored keys are not zero-cost observations.
Raw long-frame rows retain every actual positive ordinal and mark each pass as sampled or untimed;
they remain a biased over-threshold population, separate from the normal interval tables.

Default retention is **128 MiB × four files**, including `.1`/`.2`/`.3`; override per-file size with
`TOTK_VK_GPU_PROFILE_MAX_BYTES`. Compact windows target a 6–8-minute pan without rotation, but
raw logs, pass-key churn and mode-3 dumps can still fill it sooner. Each physical file starts with
a `header`: launch ID, APK `assets/build.txt` commit/build text, package/versionCode where available,
effective profiling settings, whitelisted requested launch environment and actual Vulkan device/driver
IDs/name/version. Exact APK SHA-256 is unavailable in process; join the separately captured APK hash.
Header rotation indices and window `sequence` identify missing complete windows/evicted files, even
when no surviving frame reveals the gap. Keep all four files when collecting a launch.

Orderly Android close/activity destruction stops admission of new renderer work, flushes/drains renderer
GPU work, then drains/joins the profiler reporter before writing `session_end` under the output writer
mutex and rejecting late writes. `clean_end=true` records successful **writer** closure independently
of query retirement. New footers require boolean `all_queries_retired` plus `query_retirement` counts:
`leases_acquired`, `leases_submitted`, `leases_retired`, `leases_cancelled`, `leases_outstanding`,
`queries_submitted`, `queries_retired`, `queries_cancelled`, `queries_outstanding`.
Acquired leases equal retired + cancelled + outstanding; retired leases are submitted, and cancelled
recordings are never submitted. Outstanding queries equal submitted minus retired. All counts are
nonnegative integers; `all_queries_retired` is true iff both outstanding counts are zero.
A retired lease means its fenced results were read/attempted, not that every result was available:
only available timestamps and successful statistics reads retire queries. Queries count activated
timestamp/statistics instances, not reserved pool slots or individual statistics counters; cancelled
queries were activated in abandoned recordings. Thus a clean writer end can truthfully report false
retirement, including unavailable queries left outstanding after all lease reads were attempted.
The analyzer preserves these counts in `sessions` and renders them separately from writer closure.
Historical footers without `query_retirement` and with `all_queries_retired=null` remain readable as
unknown; no counts are fabricated. Force-stop normally omits the footer, as do crash/kill/incomplete
captures. The analyzer reports missing closure and retains completed-window timing without synthesizing
an end marker. Both Odin P1/P2 overhead captures lack an end after final force-stop; their retained
complete windows remain usable, but full-launch closure/query retirement is unknown.
The real Vulkan `--shutdown-inflight` host fixture enters shutdown with an unsignalled event-gated GPU
submission and no prior profiler flush. Its modes 1/3 × workers 0/2 × stats 0/1 × SAMPLE 0/2 coverage
requires the GPU drain, reporter drain, complete final frame and true retirement footer. It is not
Android-device validation or a performance measurement.
Enabled GPU draining has one two-second budget. If it expires, `query_drain_timed_out=true` and positive
outstanding counts expose unread/unretired work; a later GPU completion does not rewrite the footer.
The additive timeout flag is boolean when present, unknown for historical footers that omit it.
The `--shutdown-timeout` host scenario holds its real event beyond that budget and requires a clean writer
end with false retirement, not a fabricated complete final frame. Profiling-off `--shutdown-inflight`
requires pending work to remain undrained: disabled shutdown does not flush or wait for renderer GPU work.
Invalid/unavailable timestamps or implausible (>10,000 ms/frame) spans produce `span_valid=false`,
null span/gap and explicit reasons/counters; measured pass sums are not replaced. The analyzer keeps
valid-span subtotals and forensic raw spans separate, never sums absurd positive spans as real GPU time.
For historical schema-2 captures, `gpuprof.py --legacy-span-replay` reads only frame/window timing:
it flags implausible spans and propagates invalid frame authority to windows without claiming to
reconstruct query availability/endpoints or schema-3 attachment areas.

For the d003f2d **CPU frame-close bug only**:
`python3 tools/bench/gpuprof.py --frame-close-replay 'capture/gpuprof-*.jsonl*' --json replay.json`.
Android's `present_window` submitted the last slot before outer CPU present, leaving no open command
buffer for the old GPU marker. The next-frame transition then mislabeled otherwise complete full-mode
records `incomplete-records`. The old host fixture called present with a buffer open and missed this
ordering. The new `--sealed-present` fixture exercises multiple submissions, pre uploads, present scopes
and readbacks before CPU close, with both worker-already-consumed and real GPU-gated pending paths;
`--last-closed` requires the final frame to publish without a following frame or shutdown flush.

Replay is an explicit **counterfactual**, not a corrected device capture. It adds
`frame_close_replay` with original rows/provenance, exact eligible/rejected frame IDs and windows,
rejection reasons and conditional span/gap sums. Ordinary `rejected_spans`, invalid counts and null
totals remain unchanged. Eligibility requires consistent APK-build headers identifying d003f2d, a complete
contiguous 300-frame window, explicit zero unavailable/invalid/dropped/failed/background counters, valid
available unwrapped raw ticks/domain matching the header, a bounded recomputed raw interval, nonnegative
full-mode gap and consistent frame/pass/window sums. Missing authority and incomplete tails stay
unknown/rejected. Corrupt timestamps and real query/background losses are never relabeled; sampled
endpoints are not reconstructed. Aggregate records cannot prove there was no unrecorded lease loss,
so even eligible results are conditional on the CPU-closure-only hypothesis. No device rerun or post-fix
correctness/performance result is implied. This mode cannot be combined with timing-only legacy replay.

Analyzer families normalize depth+MRT layouts and scene-sized depth-only prepasses against this
reference (`scene_relative=true`). Thus a G-buffer and its depth prepass remain one family each
through dynamic-resolution changes. Other passes retain absolute family dimensions: fixed 360²
shadows stay one shadow family as the scene changes, and 360² versus 720² depth targets remain
distinct. Exact pass tables retain every absolute key/area/reference variant, independent of family
grouping. Normal analysis rejects schema 2 rather than reinterpreting its old self-normalized ratios;
the explicit legacy timing-only replay above does not admit its pass rows.

Traffic is an **uncompressed logical estimate**, render area × bytes/texel × samples per
LOAD/STORE, not an Adreno hardware counter. It excludes metadata, resolves, compression,
alignment and driver decisions such as sysmem rendering. Depth and stencil operations are
counted separately, splitting packed D24S8 into 3+1 bytes and D32S8 into 4+1. Unsupported or
block-compressed view byte sizes are excluded and counted in `unknown-byte attachments`.
Capacity is 2,048 timed records and statistics queries and 64 frame segments per submission,
and 16,384 retained observations per frame / exact-key+reference+scene-role aggregates per window
(also per ordinal when requested); nonzero drops/query failures mean incomplete coverage.
When off, there are no profiling query pools, allocations or timestamps; remaining hooks are a
single disabled branch. When on, the draw/pass paths are O(1) in draw/resource/prior-pass count:
at most six bounded shader identities are hashed on the first draw, later draws only increment
the counter, and pass begin captures at most ten attachment records. They do no binding scans,
texture-registry locking/copying, earlier-pass searches, allocation or logging. Static attachment
operation/byte summaries are computed from retained pass keys off the walker. Nested timing
scopes save/restore their timing parent, command buffer and secondary state without any audit state.

`TOTK_VK_GPU_PROFILE_STATS=1` requests input-assembly vertices/primitives, vertex-shader
invocations, clipping invocations/primitives and fragment invocations when the device
supports pipeline statistics (default off). In sampled mode, only selected passes get statistics.
Worker chunks own disjoint queries inside their secondaries; the completed pass sums those queries
without requiring inherited queries. Ratios use only statistics-matched draws and duration:
vertices/draw, primitives/draw, fragments/primitive and whole-pass ns/vertex or ns/primitive are not
isolated shader costs. Statistics still perturb sampled passes. Sampled duration representativeness
is conditional on the profiler **not serializing/changing GPU execution**, not guaranteed by a small budget.

The legacy speculative `LOAD-before-full-clear` / `STORE-no-later-observed-read` audit is removed
from the timing profiler, including its read/clear APIs, per-pass arrays and present-time candidate
checks/logging. It was not timing/statistics authority, and incomplete raw-memory/cross-frame consumer
tracking could not prove that a store was safely discardable. Static LOAD/CLEAR/STORE/DONT_CARE
operations and logical byte estimates above remain; no attachment operation is changed by this cutover.

### Odin overhead: CPU evidence supersedes GPU-dominant inference

`.scratch/bench/rpmode/odin-overhead/report.md` retains the fixed-order Hateno comparison for
APK `716c122ff09b`, recommended 60 fps with Keep the GPU busy On:

| Arm | FPS | Recording ms/frame | NVN GPU ms | GPU busy |
| --- | ---: | ---: | ---: | ---: |
| C0, custom driver/control | 57.96 | 10.789 | 13.278 | 80.2% |
| P1, profiler without stats | 21.55 | 43.750 | 15.950 | 36.6% |
| P2, profiler with stats | 21.40 | 44.300 | 19.100 | 43.9% |

P1 adds **32.961 ms/frame recording** and loses 62.82% FPS versus C0 while GPU busy falls sharply;
91.12% of its clock time-in-state is at 660 MHz. The observed CPU recording/feeding perturbation,
not a presumed GPU-only stall cost, is the primary evidence for this cutover. P2 adds 0.55 ms
recording and -0.15 fps versus P1, inside the 0.51-fps control bracket; its independent FPS cost
is unresolved. Profiler span and NVN GPU time have different scopes and are not interchangeable.
Fixed order, unequal complete-window populations, cache contexts and compile stalls limit causal splits.

Source evidence before removal: `vk_gpuprof_draw` visited up to 2×160 bindings per graphics draw,
including unselected sampled passes. Each live handle called `vk_treg_get` in `vk_res.c`, taking
`g_reg_mu` and copying a registry entry, then `vk_gpuprof_read` searched every earlier pass audit.
Pass end could realloc/append the audit array; present traversed the array and logged candidates.
This is avoidable draw-count × resource-count × prior-pass CPU work, not a measurement of the
individual contributors to the 32.961-ms increase.

Turnip's `tu_CmdWriteTimestamp2` still issues WFI before its always-on-counter read for
`ALL_COMMANDS` (`turnip/wt-rpmode/mesa/src/freedreno/vulkan/tu_query_pool.cc:2100–2144`).
That source fact remains a real GPU perturbation risk, but the earlier inference that it dominated
the Odin FPS collapse is withdrawn. Removing the CPU audit/backgrounding reporting does not make
timestamps or statistics free. Host tests/CPU benchmarks do not establish mobile recovery;
repeat a matched unprofiled / sampled timestamps-only / sampled-stats / full-mode device A/B.

Device plan (operator, no device execution performed here):

1. On Thor/Turnip and Odin 3/Turnip, retain identical save, camera, weather, resolution,
   warmed shaders/pipelines, target rate, AYN power profile and thermal start. Keep recorder
   threads fixed (normally **2**, or explicit **0** for a separate synchronous A/B); do not change
   worker policy between an unprofiled control and its profiled run.
2. Capture equal warmed intervals at Lookout Landing, Gerudo Town, an open-field view and a shrine.
   Start with GPU_PROFILE unset, then `=1` with SAMPLE `=2` and STATS `=0`, then the unprofiled control;
   repeat in reverse order. Separately test sampled statistics and full SAMPLE `=0`.
   Retain at least three complete 300-frame windows, and extend the interval when key coverage is thin.
   Thor target 30 fps, Odin 3 60 fps. Keep all rotations and report the actual measured interval.
3. Retain `[gpu-profile]` totals/top rows and audit, fps/stutter, submissions, memory, power and
   temperatures. Compare **GPU clock time-in-state deltas** over equal intervals against the
   **unprofiled control**, not merely point-in-time MHz; report thermal/clock drift.
   Require plausible valid whole-main-frame spans, stable observed keys, per-frame `timestamp_count`
   within `2k+2`, histogram/sample consistency and zero dropped/failed slots. Sampled pass sums do
   not reconcile to full GPU time: remaining work is intentionally untimed. Compare matched frame
   authority, not equal durations or profiling-on fps as though it were uninstrumented performance.
4. As a separate experiment, repeat profiling with `TOTK_VK_CLEAR_LOAD=1`, then
   `TOTK_VK_STORE_CLEAR=1` (implies CLEAR_LOAD). Compare matching shader/attachment families,
   traffic totals and visual captures after the timing runs. Never enable a new discard from
   the audit alone; confirm cross-frame lifetime and pixels first.

## Split reasons, fragment invocations and conservative pass merging

Operator device evidence (Thor Lookout, recorder threads 2): default **22.0 fps / 32.3 ms GPU**;
two `TU_DEBUG=sysmem` pairs **22.2–22.3 fps / 32.2–32.3 ms GPU**; forcing `TU_DEBUG=gmem`
**18.4 fps / 42.5 ms GPU**. Treat Turnip's default here as sysmem. The earlier logical GMEM
load/store estimates **are not real measured attachment traffic** and cannot be used to predict
bandwidth, power or milliseconds saved by merging. Any gain here must be evaluated as reduced
pass setup/teardown and synchronization overhead, against extra CPU hazard checks.

### Split diagnostics

`TOTK_VK_GPU_PROFILE=2` retains the normal 300-frame aggregates and adds `[gpu-split]` for
**every completed instance**, with recording-frame number, instance ordinal, stable signature,
draw count, GPU ms, end reason and detail. `=1` remains the compact mode. Both support
the normal recorder workers. Detailed logging can be voluminous; use short diagnostic
runs, not their fps as the final performance comparison.

Reasons distinguish NVN barriers, texture invalidation barriers, attachment changes, clears,
copies/blits, compute dispatches, CPU waits/readbacks, submission ends, uniform flushes,
presentation, diagnostic boundaries and a deferred barrier restored before a hazard. A
counter/report/condition boundary is marked `query-report` when it forces a deferred pass to
finish. Ordinary counter reports do not themselves split the original inline path; the logger
does not invent a split for them.

For NVN/texture barriers `detail` is the **original NVN barrier-bit mask**:
primitive order `0x1`, fragment order `0x2`, tiled fragment order `0x4`, indirect data `0x8`,
texture invalidation `0x10`, shader invalidation `0x20`, descriptor invalidation `0x40`,
ZCULL invalidation `0x80`. NVN's recorded command contains **no original Vulkan source or
destination stage/access masks**. Rendering pass-end barriers currently use
ALL_COMMANDS/MEMORY_WRITE → ALL_COMMANDS/MEMORY_READ|MEMORY_WRITE; named backend
scope ends do not invent that barrier. Split reason/detail and Vulkan synchronization
implementation remain separate facts.

### Draw/fragment cost observations

Every top signature now has a `[gpu-profile scale]` row: mean draws/instance and whole-pass
ns/draw. Add **`TOTK_VK_GPU_PROFILE_STATS=1`** (default off, requires GPU_PROFILE 1, 2 or 3)
to request six `VK_QUERY_TYPE_PIPELINE_STATISTICS` geometry and fragment counters. Startup
logs whether `pipelineStatisticsQuery` is supported/enabled; unsupported devices retain
timings and omit statistics fields. No device extension is inferred from an abbreviated probe.

Statistics have another per-slot query pool, reset on slot reuse and read only after the
same existing fence, with no new GPU wait. Rows show valid statistics samples/instances,
the six counters and derived geometry/fragment ratios; failed slots are explicit. Compute
and transfer/backend scopes do not have graphics counters. Fragment invocation counts are
driver-defined shader invocations, potentially including helper/sample-frequency work,
**not shaded pixels or guaranteed visible fragments**. Statistics can affect performance.
Ratios use matching statistics-valid time/draw denominators; JSONL preserves valid/total
instance counts. Occlusion and pipeline-statistics queries may coexist: Vulkan
requires active query types to differ, not to be globally exclusive.

These are workload-normalized observations, not a causal timing decomposition: vertex,
fixed-function, memory, barrier and pass overhead remain in the numerator. Compare warmed
matched camera/clock runs, and compare attachment/shader families across resolution changes
to distinguish draw-heavy from fragment-heavy scaling; a single sample cannot prove either.

### `TOTK_VK_PASS_MERGE=1` (default off)

`TOTK_VK_PASS_MERGE=1` forces `REC_THREADS` effectively to **0**; GPU_PROFILE itself no
longer does. Its host pixel-identity acceptance exercises the primary recorder.
The reason-aware pass-end implementation retains the threaded recorder's drain/parking/assembly path.
Merging disables itself, with a log, if `TOTK_VK_STORE_CLEAR=1`: that optimizer chooses
DONT_CARE at pass begin using assumptions about intervening pass boundaries.

The original `TOTK_VK_OFF=passbarrier` suppresses essentially **all** in-pass NVN barriers.
It is an unsafe diagnostic, not selective merging: material shaders write class flags in
storage buffers that later shaders read, so suppressing all synchronization can lose ordering.
The normal backend already coalesces consecutive SetRenderTargets of the exact same attachments
and views when there is no pending clear. Repeating that optimization cannot fix the measured
six G-buffer instances.

PASS_MERGE instead defers only a bounded safe subset of barriers while a pass contains no
accepted draw with any storage-buffer, storage-image or storage-texel descriptor, unknown
binding or potential sampled-attachment alias. Read-only uniform/sampled resources and
fixed-function attachment writes may continue in the same rendering instance, relying on
normal rasterization order. Before a later draw introduces any storage descriptor or possible
attachment feedback, the original full barrier is restored by ending the pass **before that
draw**. This also protects earlier reads against later writes (WAR). Whole guest storage ranges
retain aliases; unknown handles/bindings retain synchronization. No unreliable `rw=0` inference
is used. Counter/report/condition operations also finish a pending deferred pass.

ZCULL, indirect-data and unknown barrier bits are never deferred. Pass dumps, old PASS_PROFILE,
pass probes and serial-draw diagnostics retain their boundaries. OFF=passbarrier takes precedence
and disables the selective optimization. Attachment changes, transfers, compute, CPU access,
uniform flushes and submissions still finish passes normally.

No `vkCmdPipelineBarrier2` is issued inside dynamic rendering: the backend has not enabled
dynamicRenderingLocalRead or shaderTileImage features. Vulkan render-pass self-dependencies
are not a shortcut for this dynamic-rendering implementation. The probe does not establish
whether Turnip could support a separate future local-read implementation.

`[pass-merge]` every 300 frames reports deferred and retained barriers, late hazard splits and
**avoided pass boundaries/frame**. Deferral followed immediately by a hazardous draw is a moved
split, not a saved boundary. Known material/class-flag storage accesses remain unmerged; this is
deliberately conservative. Six observed G-buffer signatures give an upper bound of five removed
boundaries **only if** their attachment sets match and all five intervening reasons pass these
checks. That is at most about **2.4% of 205.7 total pass/dispatch instances**, not a 2.4% GPU-time
prediction; signatures alone do not prove even one removable boundary. Actual expected savings
are the avoided-boundary counter, not the earlier hypothetical 23 MB loads/stores per instance.

Host acceptance for this follow-up: lavapipe **48/48** checks with both switches off;
**50/50** with GPU_PROFILE=1 + PASS_MERGE=1; **50/50** with GPU_PROFILE=2 and merging off;
**50/50** with GPU_PROFILE=2 + PASS_MERGE=1 + GPU_PROFILE_STATS=1; **55/55** with precise
occlusion and fragment statistics together; **50/50** with STORE_CLEAR=1 proving the merge
guard disables itself. Read-only barriers preserve all **16,384** reference pixel bytes while
avoiding three boundaries; the mixed stream preserves those pixels and all covered fragment
storage writes, saves one boundary and restores/retains the required storage dependencies.
Statistics measured **8,192 invocations** for four half-screen 64×64 draws and **2,048**
for a single half-screen storage-writing draw, with zero failed statistics slots. Detailed
logs distinguish original bits `0x2`, `0x10`, `0x40`, `0x80`, late hazards and copies.
Android syntax-only checks passed for `nvn_vk.c`, `vk_draw.c`, `vk_res.c`, `vk_gpuprof.c`
and `test_vkdraw.c`. No device or validation-layer run is claimed for this follow-up.

Device follow-up: first run threads 0, GPU_PROFILE=2, PASS_MERGE unset for a short fixed Lookout
view and read the six G-buffer families' `end`/`detail` rows. Repeat with GPU_PROFILE=1 and
GPU_PROFILE_STATS=1 to obtain invocation density with much less logging. Compare PASS_MERGE unset
and `=1` in matched warmed runs (profilers unset for final fps/power), inspect `[pass-merge]`
and whole-slot GPU time, and separately compare deterministic visual captures. Leave Turnip's
sysmem/GMEM autotuning unchanged. If the storage guard retains the relevant splits or the avoided
count is tiny, merging is unlikely to justify more complex synchronization work.

## Status

Boots, plays, saves. As of 2026-10-22 the overworld renders correctly on the Thor (user-confirmed): the black flicker
(`condorder`), the dark water (`d16tie`), the overworld hangs (scheduler), the mapping-limit crash (64 KiB protection
units) and the memory leak (descriptor pools) are fixed. ~21-22 fps in the world (CPU recording ~24 ms, GPU ~18 ms a
frame); target 30. Open: performance, regular hitching.

### Overworld hangs and crash (2026-10-21/22) (fixed?)

Symptom: loading into the overworld, graphics stop, audio goes on. In order:

1. **No preemption.** A `threads` dump showed thread 0x1040 (priority 53) holding virtual core 1 for 5+ s in guest code
   without an SVC while 0x1004 (priority 30, core 1 only) waited: the TOTK scheduler only preempted at SVC entry.
   Fix: asynchronous preemption (`TOTK_ASYNC_PREEMPT`, on by default on Android; `=0` off): a waiter signals worse-priority
   owners every millisecond and the owner gives its core up in the handler (the Breath of the Wild mechanism,
   runtime/threads.c).
2. **Priority inversion.** 0x1040 (53) held a mutex that 0x1037 (44) waited for while 48-52 kept core 1 busy. Fix:
   priority inheritance (`tk_prio`: a mutex owner competes at its best blocked waiter's priority, as Horizon does).
3. **Preempted inside an SDK lock.** Host stacks (`su -c 'debuggerd -b <pid>'`, Magisk) showed two threads spinning at
   sdk+0x13f034 while a third, preempted at sdk+0x13f06c inside the same lock, waited for a core. Fix: priority
   preemption only in the game's own code (`main`).
4. **Starvation in SDK code.** The `threads` dump prints where the signal last interrupted each thread (`last interrupted
   ... at <module>+<offset>`; live registers cannot be read on Android). 0x1040 held core 1 for 53 s at sdk+0x5b2518
   (from main+0x2ef77c), refused 161k times. Fix: a starvation rule (`TOTK_STARVE=0`: off): a thread that waited 20 ms
   for a core takes one whose owner held it 20 ms, in any guest code, and keeps it 2 ms.
5. **Mapping limit.** Then it loaded into the overworld and froze or crashed (Android aborted creating a thread). The
   process had 92,673 mappings (`adb shell "su -c 'wc -l /proc/\$(pidof com.bernalr.totk)/maps'"`), past
   `vm.max_map_count` (65530): dirty-page tracking write-protected single 4 KiB pages, each a separate mapping. Raising
   the limit with root (`sysctl -w vm.max_map_count=1048576`) made it run. Fix: nvn_dirty.c protects in aligned
   64 KiB units on Android (`TOTK_DIRTY_UNIT`, pages; 1 = the old behaviour). A write to one page makes its unit
   writable and new, so textures sharing a unit re-upload once more. Desktop test: 1054 mappings per page vs 31 per unit.

The app reads only commands appended to TOTK_INPUT_FILE after it starts (old `threads` lines were replayed at every start).

### Memory leak (2026-10-22, fixed, to confirm)

The app's memory grew ~45 MB/s in the world (the Thor's monitor: up to ~200 MB/s) until Android ran out: textures dropped
to their lowest detail at ~15 GB, then surfaceflinger aborted ("no suitable EGLConfig found") - a system-wide
out-of-memory, not a crash in the game. `dumpsys meminfo` 10 s apart: Native Heap +107 MB, Gfx dev +366 MB. Ruled out:
write-back, pipelines (flat at ~830). The per-300-frame `memory:` line showed it: descriptor pools 8 -> 212 -> 512 ->
812, one more per frame. `vk_alloc_set` took sets only from the slot's last pool; a submission needing more than one
pool (the world: ~4,400 sets a frame, 4,096 per pool) made a new pool each time and never used the reset ones again.
Fixed: a slot's pools are used in turn (sets counted per pool as well; lavapipe never reports a full pool, so the
desktop test needs the count). Watch `descriptor pools` in the `memory:` line: it should stay small.

First world numbers (Apex, 1.4.3): 17 fps, ~3,400 draws and ~168 passes per frame, ~108 of those split by NVN
barriers inside a pass (each split stores and reloads every attachment on a tiled GPU). `per frame:` in the world: recording
32-36 ms, GPU 21 ms, one submission - CPU-bound on recording first (~4,400 descriptor sets written a frame, ~2,000
reused), then the GPU.

### Nine-frame glow flicker: direct uniforms by default (2026-10-22)

An inline uniform update belongs to the current game submission even after a Vulkan chunk split flushes it and assigns
its fence. Snapshot overlays used to discard every fenced update, reverting later draws and CPU uniform-pointer reads
to the submit-time constants. They now compare update sequence numbers with the snapshot's base sequence instead.
`vkdrawtest` exercises full and partial overlapping updates over twelve iterations of chunk splits, immediate CPU reads
and GPU draw/readback, recycling the eight-slot ring without ending the game submission. Renaming is now **off by
default** (`TOTK_VK_UB_RENAME=1` opts in; `TOTK_VK_OFF=rename` overrides it). With lag off, submit UBO snapshots are
coupled to renaming and are also off by default. Direct updates retain flushed CPU-only overlay entries until
completion, so `vk_ub_read` sees current storage-buffer pointers before GPU execution; `vk_ub_rename` explicitly
refuses to rename descriptors in this mode. The direct GPU path ends/drains the pass and orders `vkCmdUpdateBuffer`
with barriers. With protected frame lag, UBO-only updates instead use a private logical shadow and mandatory immutable
UBO descriptors: they never queue an update to the guarded guest pool. Real non-UBO aliases materialize the exact
overlap in command order, and genuine writable/GPU-produced ranges remain protected. This lag-only path does not
enable ordinary renaming or change its default.

Protected-lag hangs print `[dirty-lag-wait]` after 500 ms, once per ticket/gate wait episode. Retain its TID,
runtime role and OS name together with unit, ticket, GPUdone and submitted watermark. `unsubmitted` is a CPU-side
submission dependency, not evidence of a slow GPU; a `registrar` waiting on it can block the worker join needed to
queue that very ticket. Explicit-ticket registration must fail nonblockingly for every BUSY unit, even a sampled
completed ticket, because waking on that gate can otherwise follow a later future-ticket owner. Fallback waits belong
after successful queue submission. The diagnostic never releases protection or invents completion to recover a hang.

For protected-lag overhead, compare `recorder submit guards/frame` projected ranges **and unique units**. Preparation
overlaps recording; service sums worker CPU time, while merge/join/critical identify the late submission path. Retain
the separate `lag projection index` frozen-version/build cost: warm projection speed alone does not include index
construction. `vktest --lag-bench` separates cold all-unit mprotect, warm ticket updates and equal-ticket repeats;
`vkrecspantest --lag-bench` compares indexed/all-span projection. Host CPU results are not Android frame predictions.

`vktest --lag-query-bench` runs without Vulkan initialization. Its pending-original queries compare the production
sorted overlap index with the old full-log scalar scan over 256/4096 mixed private, completed and pending entries.
The index changes only candidate enumeration: an original producer is still pending until its exact fence completes;
private logical updates are never producers. Existing lag-walk alias/producer/pixel checks remain required.
The same fixture compares producer-journal validation with its scalar oracle at 64/512/4096 intervening writes.
A bounded 64-KiB-window revision summary skips unrelated history; bucket collisions fall back to the exact journal.
Wide/overflowing writes invalidate the global summary, and boundary-spanning writes stamp every intersected window.
These revisions prove only absence of a new original-memory producer. They never authorize a byte copy, skip a writer
gate, change alias projection, or replace ticket/completion checks. The summary is walking-owner-only like the journal.
Draw-touch also uses immutable shader reflection summaries: shaders without read-only storage skip that empty
preparation pass, and UBO preparation enumerates only consumed register bits (deduplicating repeated reflection entries).
Writable storage still publishes on every draw, all stage writers still precede reads, and broad UBOs still materialize
their true producer overlap before rendering opens. No binding/content/ticket decision is cached by these masks.
`vktest --lag-touch-bench` isolates the old descriptor reflection scan versus register-bit enumeration for two
128-binding stages with twelve UBOs each. Its equal register checksums and ns/draw exclude producer queries and Vulkan;
the existing lag-walk fixture supplies the independent real draw/alias/producer correctness and end-to-end timings.
The desktop presenter mock publishes its pixels/geometry through an atomic post counter; polling that counter must
establish the cross-thread publication before assertions inspect ordinary pixel memory (also required under TSan).
Pass-probe reports serialize their shared LCS/depth scratch between the presenter and explicit test callers;
that diagnostic lock is outside recording and queue publication.

`vkdrawtest --lag-walk-bench [frames=6]` measures a Lookout-shaped 5,760-draw stream with two warmup frames per
stable/churning case. It explicitly disables automatic draw-count chunking (`TOTK_VK_CHUNK_DRAWS=0`) for its
one-submission primary measurements; separate twelve-chunk stress frames test lifetime boundaries without contaminating
those means. Compare identical shader seeds and fixture sources, including `REC_PROFILE=1` / sampling rate 1, and retain
CPU-thread ns/draw separately from wall time, seal/join and GPU wait. Stable bindings are not a substitute for the
rebind/update/alias churn case. Per-frame pixels, actual submission counts, true storage producers and strict post-timing
uniform/indirect-pointer authority probes are mandatory gates, not optional benchmark overhead. Its event coordinator
opens only a genuinely queued ticket; a bounded watchdog reports failure rather than fabricating GPU completion.
With asynchronous submit, `vk_submit_serial()` advances when the walk seals a slot. It does not prove GPU queue
publication. The coordinator explicitly checks the queued watermark, then asserts the exact reserved/queued/fence
ticket; otherwise an early-opened event could turn this safety test into a false positive.

Additional `device-cold` / `device-learned-hot` cases change three vertex streams and an index range on every draw,
with 5,760 separately checked pixels. Two submitted training frames use real tracked CPU writes before the learned-hot
phase; no diagnostic switch invents hot history. The intended unique hot payload is 288 ranges / 243.4 KiB, but use
the observed copy counts/bytes, not that target. This isolated case has about 96 fixture input units rather than the
device's roughly 2,381 and no GPU-written hot neighbours; the original mixed producer and strict alias probes retain
that independent safety coverage. Compile the same corrected fixture/seed against saved `89c0735` for comparison;
the old baseline omits only the compile-gated new raw stats getter, not its pixels or safety assertions.

The existing 300-frame protected-lag log now separates vertex, index, storage, uniform, uniform-pointer, texel and copy
decisions. Distinguish exact binding-key hits from contained-unit classification hits. Range/mapping changes miss
metadata, but ticket/snapshot byte-key changes can retain metadata; use the separate cross-ticket/snapshot reuse and
copy-authority hit/miss lines rather than summing overlapping causes. Range-size buckets and maximum extent identify
oversized bindings. Every 64th whole decision is timed; sampled ns/count is the observed sample mean,
not an exact full-frame time or an overhead subtraction. Hot-copy time and snapshot requested allocation bytes/count
are separate; allocation bytes include alignment prefixes, not arena capacity growth. Lag off does not collect decisions.

The same benchmark also runs an owned-uniform-sharing probe over ten submissions and twelve actual chunks/frame,
crossing arena ring reuse. Each measured frame has three semantic versions × 64 draws × four UBO bindings, each
consuming 1 KiB. Five frames keep declared extent 1,024; five change it to 1,024/1,040/1,056 across versions, within
6,912/6,944-byte eager-owned unions. Two four-byte updates create distinct versions; real readonly non-UBO usage aliases
and repeated GPU copies test ordered materialization and producer invalidation.
First and last vec4 values affect pixels. Report allocation requests/bytes **per version**, not just per call:
one logical change cannot patch an allocation already referenced by an earlier draw. Version-2 test capabilities
enable sharing/cross-frame metadata cost assertions; the identical fixture still runs all pixel/lifetime probes
against saved `ba2bcc6` with those new cost assertions unavailable.

The Linux guest-wait wake-filter fixture bounds context switches of precisely tracked waiters, not whole-process
switches. Its six-handle fallback waiter intentionally wakes conservatively; TSan helpers also add process activity.
Both remain visible in the report but outside the unchanged tracked-waiter bound. Briefly paced signal batches ensure
a broken wake-all implementation cannot pass through coalescing. Normal/TSan negative controls with scratch-only
wake-all code must fail, while ready-at-wake cores off/on must preserve correct selected-handle indices.

Thor title bisection (12-second recordings, recorder threads zero, frame lag off) retained periodic dips with
`TOTK_VK_SYNC=1` and `TOTK_VK_FRAMES_AHEAD=1`, but `TOTK_VK_OFF=rename` alone removed them: this is not a CPU/GPU overlap
race. The game's every-nine-frame probe update adds 132 cube-filter passes, crossing the default 75-draw chunk threshold
before the usual lighting work. Sealing the inline update log at that boundary changes which constants the old code
overlays. No overflow or generation wrap is involved. The original opt-in host regression failed 24 checks before the
sequence fix. The expanded regression passes 91 checks with direct updates, 79 with opt-in renaming (lavapipe);
the old direct path fails 24 immediate/post-chunk CPU-read checks.

The supplied title dump f1026–f1037 has those extra passes at f1026 and f1035, but no matching isolated dropout in the
final or bloom outputs. The large differences in ser2613 compare different cube faces (f1034 layers 0/1, f1035 layers
2/3, f1036 layers 4/5), not corruption of the same output. Passdump adds copies/barriers and changes submission
boundaries, so it cannot identify the normal-run first bad pass. Metal avoids the same stale-snapshot case by dropping
overlapping snapshots when flushing inline updates (`nvn_mtl.m` `upd_flush` / `usnap_drop`).

Device verification of the sequence fix (commit 6be25f9): Odin title had zero dips in two 12-second recordings, but
rainy Lookout still flickered with renaming enabled. The torch/horizon band had 14.1 isolated tile spikes per 100
frames with renaming, versus 0.8 with `TOTK_VK_OFF=rename,snap`; total tile spikes were 33.5 versus 14.4, with sky
lightning accounting for much of the control's residual. Thus the title's sealed-update defect is fixed, but opt-in
renaming is not recommended: a second in-game emissive defect remains. Rename-off was also faster in two-worker
device controls (Odin Gerudo 53.0 → 57.1 fps; Thor Lookout 21.4 → 26.9 fps), motivating the direct default.

Verify the direct-default build on Thor and Odin: `TOTK_VK_OFF` and `TOTK_VK_UB_RENAME` unset, recorder threads
0/2/3, frame lag off, identical 12-second title recordings and 25-second stationary Lookout recordings including torches.
Count isolated brightness dips on distinct game frames (`.scratch/flicker.py`), separating sky lightning from the
torch/horizon band. Compare explicit `TOTK_VK_UB_RENAME=0` and the old `TOTK_VK_OFF=rename,snap` negative control.
Repeat with frame lag enabled and its read protection on; the independent lag guard/capture path must remain active.
Expect no periodic nine-frame title dips or localized emissive dropouts. `TOTK_VK_UB_RENAME=1` is a diagnostic
comparison only and retains the known Lookout issue; passdump cannot establish normal-run flicker absence.

### Walker range-lookup cut (2026-10-25)

The owner's 60-second Thor `walk-route1.data` CPU-clock profile attributes 6.63% of walker self time to
`nvn_dirty.c:find_range`, not a Vulkan range table. Its largest sampled address, `0x1080a58` (2.87%),
is the live-slot branch in the full `g_r` scan (80-byte stride), after missing the eight-entry recent ring.
`nvn_dirty_since` additionally used two identical pool lookups.
A 256-entry per-thread 64-KiB-window hint now checks one candidate before that existing fallback. Every
hit still checks the current slot's live flag and actual bounds; registration-only queries remain read-only.
TLS hints and the recent ring share one object. `since` retains one checked range instead of looking it up twice.
The isolated `--lag-bench` also checks tracked/untracked half-open bounds and slot reuse beyond ring capacity.

Host-only interleaved `base,cut,cut,base,base,cut` measurements used identical O1/clang fixture builds,
lavapipe, recorder workers 2, protected lag, two warmups and six measured frames/case. Medians of the three
`mtl_exec_thread` means (ns/draw), not wall/GPU time:

| case | base | range hint | change |
| --- | ---: | ---: | ---: |
| stable binding | 2036.58 | 1836.52 | -9.82% |
| churn binding | 2872.12 | 2883.14 | +0.38% |
| device cold | 1073.15 | 1068.24 | -0.46% |
| device learned hot | 977.99 | 950.63 | -2.80% |

All six measurements passed the fixture's 859 pixel, submission, producer and authority checks.
These noisy small-fixture host results are not an Android frame-time prediction; no device was touched.
Normal and GCC ThreadSanitizer fixtures passed core/draw/app (23/100/106 lag-off checks; 175/192/198
protected-lag checks), recorder identity at 0/2/3 workers, frozen spans (7416), pipelines (65),
lag-bench including lookup reuse (325), VMA/async/policy (147/29/5), private uniforms (189), and the
two-frame lag-walk (559). TSan used `LP_NUM_THREADS=0`, retained backend workers, and no suppressions.
Android `-fsyntax-only` passed for `nvn_dirty.c`. Logs: `.scratch/lag-hot/suite-dirty*` and
`.scratch/lag-hot/bench-base-dirty-*`.

### Physical-owner collision cut (2026-10-25)

`lag_physical_window` accounts for 5.14% of the supplied walker profile. Its `0x10a2f20` row (0.93%)
is the kind test inside the physical-pool owner scan, reached after a direct-mapped buffer-key collision
or ownership-generation change. The window cache itself remains clipped to elementary span breakpoints.
The owner memo is now four-way associative, retaining four alternating colliding handles; every way still
requires exact buffer identity and the current owner generation, including cached negative results.
No span layout, projection, CPU-watch, snapshot, producer, ticket or descriptor authority changes.

The pre-init `vktest --lag-query-bench` now compares the old direct memo against the production lookup
over 2,048 declared owners and four deliberately colliding handles. Interleaved medians: **979.61 -> 3.44
ns/query**, equal checksums `9830400000`, with generation-change and owner-removal checks. This is a collision
stress fixture, not a measured device collision rate. The lag-walk interleaved host means were noisy:
stable **2293.76 -> 2902.30**, churn **3429.36 -> 3821.58**, device-cold **1280.88 -> 1369.82**,
learned-hot **1163.10 -> 1210.15 ns/draw**. No end-to-end gain is claimed from those runs.
An earlier candidate that added owner bases to every span was discarded rather than increasing all frozen-span
copies by 25%; its mixed timings are retained only in scratch.

Normal and TSan passed the same complete matrix listed above plus the extended query fixture (45,108 checks);
Android syntax checking passed for `nvn_vk.c`. Evidence: `.scratch/lag-hot/suite-owner*`,
`.scratch/lag-hot/bench-dirty-owner-*`. The fixed memo adds 292 KiB over the old direct owner table,
allocated statically, not per draw; mapping changes invalidate entries lazily.

### Warm lag-decision cut (2026-10-25)

The `lag_binding_key` profile concentrates 3.20% of all walker samples at its first memo-key compare
(`0x109b240`), plus counter/key maintenance afterwards. Exact range/mapping/chunk/snapshot hits now
increment only the required binding-hit counter: zero-increment statistics and identical lifetime-field
stores are skipped. `lag_original_pending` no longer reads GPU completion for a proven negative memo;
positive answers still compare completion and misses still take a fresh completion watermark.
`vk_ub_pending` checks the empty update log before translating the address, avoiding physical-window
hashing and owner/span lookup when no mapping can possibly change the answer. These address the 5.05%,
4.85%, 0.87% rows directly and reduce work nested under uniform/read/storage decisions.

Same six-run interleaved protocol, owner-cache control vs warm cut, median CPU ns/draw:
stable **2133.71 -> 2128.63**, churn **3080.18 -> 3148.44**, device-cold **1241.73 -> 1070.13**,
learned-hot **1014.17 -> 977.67**. Preserve the mixed outcomes rather than treating the small host cases
as an additive device speedup. All normal/TSan identity, span, pipeline, lag/VMA and query suites passed;
Android syntax checking passed. Logs: `.scratch/lag-hot/bench-owner-warm-*`, `suite-warm*`.

### Inline protected-capture check (2026-10-25)

`vk_lag_capture_on` (1.01% of walker self time) is now one inline acquire load of a tri-state atomic.
The cold initializer resolves configuration once with compare/exchange; a delayed initializer cannot
re-enable capture after another thread has published pressure cutover. The same atomic replaces the
old separate disabled flag. Cutover still drains protected work, releases arenas and publishes logical
updates before storing off. `hle_nvn.c` calls the cold initializer directly for its one-time async-replay
policy; backend hot callers use the inline query. Wait-mode selection never writes mutable mode state
after cutover, including completion-thread event/report stores.

Warm-control vs inline-check interleaved medians (CPU ns/draw): stable **2141.69 -> 1986.69**,
churn **2852.86 -> 2773.00**, device-cold **1216.38 -> 1149.75**, learned-hot **1037.51 -> 941.26**.
Normal and TSan passed all previously listed suites, plus actual `--rec-vma-fallback` cutover and job/slot
reuse (131 checks). Android syntax checks passed for `nvn_vk.c` and `hle_nvn.c`; the host rebuild
compiled every header caller. Evidence: `.scratch/lag-hot/bench-warm-capture-*`, `suite-capture*`.

### Fewer UBO sort candidates (2026-10-25)

The `rng_cmp` qsort is not per draw: `ucap_build` sorts once per game submission, but its old 64-entry
deduplication hash ignored the address lanes separating 64-KiB-stride bindings. Repeated UBO binds therefore
re-entered the candidate list and sort. Capture now uses the existing full-lane lag hash and a shared
1,024-entry exact `(address,end)` seen table. Its 16-KiB stack storage is allocated once per capture,
not once per recursive command list or draw. Collisions still only add redundant candidates; the existing
sort, pool validation, overlap merge, producer exclusions and byte-copy rules are unchanged.

The pre-init query fixture compares the old collector/hash and sort against production collection/sort:
2,048 binds of 16 distinct 64-KiB-stride ranges produce **2048 -> 16** candidates with identical canonical
extents, at interleaved medians **57.13 -> 16.37 ns/bind**. Lag-walk's snapshot profile component falls
**173.96 -> 61.83 ns/draw** (stable) and **257.00 -> 83.45** (churn). Its primary thread-CPU timer excludes
the initial submission snapshot, so that separate timer does not establish the gain: stable
**2769.04 -> 2927.03**, churn **3879.49 -> 3880.87**, device-cold **1612.92 -> 1606.81**,
learned-hot **1333.72 -> 1424.85 ns/draw** in the same interleaved series.
All normal/TSan suites above passed, including the expanded 45,109-check query fixture and real VMA cutover;
Android syntax checking passed for `nvn_vk.c`. Logs: `.scratch/lag-hot/bench-capture-sort-*`, `suite-sort*`.

`storage_touch` still declares every writable alias on every draw: memoizing away that declaration merely
because its binding is unchanged would miss a new GPU producer and authorize stale captures. Its readonly
pending-update checks benefit from the empty-log fast return and inline capture flag. Likewise the existing
uniform/read memos retain their full mapping/lifetime/content/ticket authority; these cuts do not infer that
an unchanged address means unchanged bytes. The owner-cache stress and the mixed host timing deltas are not
substitutes for an on-device route/profile comparison of the integrated changes.

### Dark water (2026-10-22, fixed)

The dark "patches" are water (the user's observation): water surfaces render dark and unfogged. Passdump of a hilltop
frame with a lake (1280x720 internal; pass numbers vary per frame):
- the water material pass (fs 9175cf1e, ~31 draws, after the terrain) writes the G-buffer correctly over the lake:
  material flags c0 = (0.2, 0, 0), dark teal albedo, upward normals, linear depth;
- the deferred lighting pass (fs 233dfb93, 12 draws, HDR target 2955, its own DEPTH16 target cleared in the pass)
  first writes a per-pixel class depth (the "class marker", nvn_mtl.m: from per-tile flags the material shaders store
  the frame before), then lights each class with EQUAL depth tests. The lake's pixels get the water class depth (D16
  ~39154) but stay black where the far lake is: a contiguous region (60% of the class), while the near water (same
  class) is lit. So the water lighting draw does not cover those tiles: tile feedback, not depth precision.
Fix 1 (to test): storage-buffer stores were never marked rw (the translator's flag; nvn_mtl.m msl_stores_through), so
buffers the shaders write could be bound from uniform snapshots taken at submit (older CPU bytes). vk_shader.c now reads
each SPIR-V for OpStore / atomics / calls through storage bindings and marks them rw (log: "shader ... writes N storage
binding(s)"). Result: no change; neither did `TOTK_VK_OFF=snap` nor `TOTK_VK_SYNC=1` (so not a race with the GPU or a stale
snapshot). Next: passdump now lists every draw of passes with up to 32 draws (shaders, counts, depth / stencil state,
the class uniform vp c11[25], and draws skipped with the reason), and `TOTK_VK_PD_SPLIT=d16` dumps the lighting pass
after each draw, to see which draw should light the far lake and why it does not.

Result (per-draw dump): the lighting pass lights each material class with a full-screen quad at depth
`0.5 + 0.5 * nibble_swap(class) / 255`, EQUAL against the marker's class depth in a DEPTH16 target. The water quad
(class 35, vs 04c4bc6f, fs f7641da0) lit 0 of the lake's 7,065 pixels; classes 8 / 9 / 11 / 12 lit theirs. Every such
depth lands exactly on a DEPTH16 rounding tie (32767.5 + 128.5 * n, n even), so an ulp of difference between the
marker's vertex shader and the water quad's decides the step. `TOTK_VK_D16TIE=1` (bias every DEPTH16 draw a fifth of
a step off the tie) lit the water - confirmed - but turned Link's clothes black and stair-stepped the horizon: depth a
fragment shader writes gets no depth bias, so markers that export depth stay on the tie. Fix (to test, with
TOTK_VK_D16TIE=1): fragment shaders that write FragDepth are patched at load (`spirv_depth_nudge`: each store adds a
specialization constant, set to the same fifth of a step in pipelines with a DEPTH16 target). Confirmed on the Thor: water, clothes and
horizon all right. d16tie is now on by default (`TOTK_VK_D16TIE=0` or `TOTK_VK_OFF=d16tie`: off).

### Performance (2026-10-22)

simpleperf (10 s in the world, all threads): ub_overlaps 8.9%, bionic mutex unlock swap 7.9% + lock 3.1% + unlock 1.3%
+ kernel futex 5.1%, bind_stage 5.4%, memcpy 4%, vk_treg_get 1.8%, resolve_span 1.6%. Fixed: ub_overlaps scanned the
whole uniform log whenever one long entry was early in it (now bounded by the longest entry); texture ids resolve once
per submission (`texcache`: no registry / texture / view locks or registry copies for repeated ids; samplers cached
until a sampler registration). After: recording 32 -> 24 ms a frame, 17 -> 21-22 fps (GPU 18 ms). Next: descriptor sets (~3,000 written a frame),
the pool lock in resolve_span, pass splits (~165 passes a frame, ~105 of them split by NVN barriers). Second profile (with call stacks, after
d16tie): replay 40% of all CPU; ub_overlaps still 7.3% (the long-entry window did not bound it: the updates sit
together), vk_treg_get 7.4% from stage_sig (every bound texture id's format looked up through the registry lock on
every draw), mutex unlock 9.5%. Fixed: uniform renaming memoised per (range, snapshot, submission, log generation);
stage_sig reads a per-id format cache (registration generation); repeated GPU-write marks of one range in a
submission (each material draw's rw tile-flag buffer) skip the snapshot / log work; write-back supersede checks have
an unlocked range reject. `shader missing` counts draws skipped for variants not in the seed: fill them with
android/fill_shaders.py (--device-dir /sdcard/Android/data/com.bernalr.totk/files). After that round: 27-28 fps, recording 17-20 ms, GPU 12-15
ms a frame. The replay still runs inside the game's submit call; `TOTK_ASYNC_REPLAY=1` (hle_nvn.c translation thread,
the iPhone default) queues submits and presents to a separate thread so the game's next frame overlaps the replay.
Being tried on Android (the pool-invalidate write-back now drains that queue first, as the Mac's does).

### Region-only CPU texel transfers (2026-10-27)

The user-played Hateno whole-process recording (`t-user-hateno.data`, 360.060 s,
1 kHz CPU-clock with off-CPU stacks) attributes **1,437 ms to `nvn_swizzle` and
1,267 ms to `nvn_deswizzle` on g-119**, inclusive and excluding `__schedule`.
Together this is **2,704/3,022 ms (89.5%)** of that thread's sampled on-CPU time.
In the union of the existing long-frame attribution windows (1.2 s slack; overlapping
windows counted once), the split is **988/808 ms**, or 1,796/1,972 ms (91.1%).
The busiest window has 201/156 ms (357 ms combined); windows include work before
the marked frame, not just the long frame. Sizes/regions are not recorded in those
samples, and the old log cannot establish them.

Mirrored `WriteTexels` now copies only the requested bytes directly to block-linear
storage, splitting each row at contiguous 16-byte GOB runs. `ReadTexels` uses the
same region addressing in reverse. Per-level geometry, mip tile shrink, layer
offsets, packaged layouts and 3D tile depth come from the existing layout code.
Compressed formats are addressed in bytes/block rows, including partial final
blocks. Other texels, padding, levels and array layers are never rewritten.
Supported layouts need no per-call linear allocation. The old whole-level
implementation remains only as an unsupported-layout fallback, logged once per
direction with `texel region fallback`.
Pool-only access also avoids eager image realization/dirty re-upload:
`texel_image` checks the existing serial and storage-sharing metadata without
creating a Vulkan image. GPU-owned storage, including a different texture serial
over the same bytes, retains the normal resolver and GPU transfer path. The first
actual GPU use still performs the ordinary dirty-tracked upload.


**Recording ownership is unchanged.** `vk_int.h` requires texel entrypoints to
serialize with the walker; its `tex_get_inner`/`upload_inner` in `vk_res.c` may
realize an image, upload dirty pool bytes into a pre-command-buffer snapshot, and
change storage sharing. Dirty protection stamps a concurrent store for a later upload;
it does not make the current multi-GOB snapshot atomic. Write-back landing also
temporarily makes pages writable while owning the recording lock. The ordinary
game's unlocked stores are not proof that adding a concurrent backend writer is
safe. Retaining the lock replaces the expensive whole-level work inside it with
the exact region copy, without adding a new concurrency assumption. The GPU-written
branch retains staging, native-image patching and `vk_flush_wait`: this change
does not prove the write-back/native-image lifetime and visibility conditions
needed to replace that completion boundary with non-blocking ordering.

Set **`TOTK_TEXEL_STATS=1`** in `totk.env` and restart for a diagnostic replay of
the same Hateno route. Every 300 presents, `texel stats frame=... op=write/read`
logs interval calls, `region_bytes`, `old_convert_bytes` (the counterfactual old
whole-level conversion bytes, twice per written level), `converted_bytes` (actual
pool conversion bytes), total `call_ms`, `lock_ms` (recording-lock acquisition,
including owner-service work), upload calls, `gpu_written` upload calls, flush
wait count/time, and fallbacks. These byte counts cover the CPU pool conversion,
not independent dirty-image uploads performed by image realization. The first
eight mirrored calls per direction also log `texel shape` with format/block size,
mip/layer, level extent, exact region and linear strides.

For confirmation, expect zero fallbacks, `converted_bytes` close to mirrored
region bytes rather than `old_convert_bytes`, and lower texel call/lock time.
Compare long-frame incidence and g-119 swizzle/deswizzle samples on matched routes,
with normal timing runs separate from stats runs. Check GPU-written/flush counters
before attributing any remaining stalls to CPU conversion. No device result is
claimed by the host tests or synthetic benchmark.

The ordinary host `vktest` build also builds `texelregiontest`: it compares full
storage bytes and padded readback buffers against whole-level decode/patch/encode
over every supported format (including BC/ASTC), all supported bytes/block sizes
1/2/3/4/6/8/12/16, byte-unaligned randomized edges, all mips/layers, 3D slices,
linear/minimal/packaged layouts and tall/deep H0..5/D0..4 variants.
`texelregiontest --bench` times a 32x32 RGBA8 write at (1007,999) in a 2048x2048
texture against the allocating old path, verifying whole-storage equality.
`vktest --texel-regions` covers actual mirrored backend calls, strided multi-layer
and 3D mips, dirty re-upload and GPU-written staging/readback. Run both with the
same two default/protected-lag environments as the full host suites.

Host acceptance (lavapipe, clang, affinity 0-3/nice 10; logs in `.scratch/texel/`):

| Suite | Default lag/recorder unset | `TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2` |
| --- | ---: | ---: |
| Region oracle, 156 formats / 255,132 regions | 1,785,928 checks | 1,785,928 checks |
| Backend texel regions, including unrealized and GPU-owned aliases | 119 checks | 119 checks |
| Core / draw / app | 23 / 106 / 112 checks | 175 / 199 / 205 checks |
| Frozen spans | 7,097 checks | 7,416 checks |
| Pipeline / placement / native UI off / canvas | 64 / 58 / 16 / 19 checks | 64 / 58 / 16 / 19 checks |
| Recorder cross-mode, native/widened uint8 | 15 exact identities | 15 exact identities |
| Atlas UV reuse negative control | expected failure, reused UV pixel `(0,0,128,255)` | 5 checks, submit-time pixel `(192,192,128,255)` |

The draw/app subprocesses also pass their settled-writeback checks (57/57/14
default and 68/68/14 protected); seed-pack verification matches 51 loose files,
and both seed-I/O fixtures pass. TSan (`/usr/lib/llvm-21/bin/clang
-fsanitize=thread -g`, `TSAN_OPTIONS=halt_on_error=1 LP_NUM_THREADS=0`) passes the
region oracle, backend texel fixture, core/draw/app and frozen-span suites in
both arms. No recording-lock scope change is made.

Seven fresh host benchmark processes, 40 old/new writes each, give medians
**12,762.412 us old vs 1.115 us region-only** for that small-region/large-image
case. Whole-storage bytes agree in every run; conversion is **32 MiB -> 4 KiB
per call**. This is deliberately a sparse-write microbenchmark, not a Thor
frame-time prediction.

### Black terrain patches (2026-10-22, fix written, to confirm)

Screenshots: ground and distant terrain black in rectangular patches. The Mac had this (nvn_mtl.m 2026-10-15): the game
bakes terrain tile normals on the GPU into a CPU-visible pool and, after nvnMemoryPoolInvalidateMappedRange, copies them
with the CPU into new tiles. Our textures in CPU pools keep a GPU rendering only in their VkImage, so the CPU copied the
stale bytes. Fix (vk_res.c `wb_note` / `mtl_pool_invalidate`, opt-in with `TOTK_VK_WRITEBACK=1` since the merge into the
measured dev line: each invalidate makes the CPU wait for the GPU, 29.8 → 24.3 fps on the Thor's Lookout Landing sweep): every GPU write to
such a texture is noted (its pages protected page by page, `nvn_dirty_protect_exact`, so writes next to it in the same
64 KiB unit do not count); an invalidate over it reads every level back and swizzles it into memory, unless the CPU
wrote those pages since (then its bytes are newer) or a GPU copy into that memory came later. The log shows
`[writeback] ...` lines and, every 3000 submissions, a `GPU write-back:` summary. If patches remain, check those
counts first (no write-backs at all = the game reads them another way).

First device run: 423 write-backs (mostly 1x1 RGBA8 / R32F, every frame), no change to the patches, and
`TOTK_VK_OFF=writeback` looked the same. A closer look at the screenshots: the patches are pieces of terrain with their
grass and trees, dark and unfogged, floating at the wrong place (above the hill, in the sky next to a village) - tiles
with wrong heights and normals, not just wrong shading. The first version skipped block-compressed textures, but TOTK
renders into BC textures through a one-texel-per-block view (vk_draw.c blk_rt; nvn_mtl.m blk_aux) and the Mac writes
those back (it only skips BC on GPUs without BC). Now BC textures are written back too, and so are unshared images
(BC textures never share storage: a write-back record keeps the image after its texture object goes, `orphan`).
Log: `[writeback] block-compressed ...`. If that line never appears, the heights come from somewhere else.

### Terrain-normal population copy trace

For one bounded diagnostic run, set both:

```ini
TOTK_VK_TEXCOPY_TRACE=1
DM_NVN_LIFELOG=260x260:d
```

The explicit lifecycle filter is required; the trace never assumes a tile size or format.
`[texcopy] fp=<frame> sub=<generation>` records frontend texture/buffer copies, CPU texel
reads/writes (including strided APIs), pool flush/invalidate, packaged initialization, and
Vulkan realization/upload/sharing/retirement. Frontend `sub_kind=backend-at-record` is the
backend generation observed while recording, not a promise about the later replay generation.
Join records by serial, storage, command buffer and region; `phase=image` names the actual
resolved image, mirrored/shared/GPU ownership, pin presence, finalized status and current live
serial. Finalized/pin value `-1` means unknown, not live. `phase=replay result=success`
means a command was encoded; only `phase=readback result=complete` reports completed GPU bytes.

The first 32 supported GPU-region samples copy the actual resolved image to staging and wait;
`bytes`/`nonzero` exclude row padding. Unsupported regions and oversized samples are labelled
skips. CPU/guest summaries are labelled separately and do not establish GPU content. The
diagnostic can perturb timing when enabled. There are no draw hooks; when disabled only cached
checks remain. At 20,000 total lines a cap record is emitted and further lines are suppressed.
Keep the source image's last nonzero result, the destination's copy result and its subsequent
realization/upload events together: this distinguishes a zero source from a lost destination
or a new guest-zero upload. This is diagnostic only and does not change copy ownership rules.

`TOTK_VK_TEXCOPY_FROM=<frame>` drops records before that frame without spending the cap, so a
late event (the tile switch at frame ~3,500 below) stays inside the 20,000 lines.

`TOTK_VK_WRITE_RANGE=<lo hex>-<hi hex>` (independent of the trace above) logs, at any size or format,
the first GPU write to each image whose storage overlaps the range (`[gpuwrite]`: render target, copy
destination or storage image) and every buffer-to-buffer copy reading or writing it (`[b2b]`, with the
source's nonzero byte count at record time). Pair it with `TOTK_WATCH_ADDR=<lo> TOTK_WATCH_LEN=<size>`
for CPU writes: together they name every writer of a memory range.

### Vanishing grass (2026-10-26, fresh/concurrent CPU consumers corrected; eight clean combined device runs)

Grass in Hyrule Field vanished at once ~20-40 s after a load, leaving bare ground. The grass vertex shader
derives "up" from a 260x260 RG8 terrain-normal tile (`up*1000-600` fades the blade, the fragment shader discards
below a threshold), and at that moment every tile it sampled held zeros. Chain, from one Odin run each:

- the game renders 64 normal tiles once, right after the load (frames ~2,574-2,600: `[gpuwrite]`, 260x260 RG8,
  9 levels, CPU-visible pool); nothing in the tile range is ever GPU-written again;
- ~800-1,000 frames later it invalidates each baked tile (`[texcopy] op=nvnMemoryPoolInvalidateMappedRange`)
  and copies it with the CPU (`memcpy` from `main+0x2ef77c`, `TOTK_WATCH_ADDR` hits) into new slots, then
  creates new texture objects there, never rendered into;
- our rendering lives only in the VkImage, so guest memory under the baked tiles is zero; the write-back record
  that `TOTK_VK_WRITEBACK=1` would honour at that invalidate has aged out after 3,000 submissions (the bake is
  ~9,600 submissions old), so the CPU copies zeros and the new tiles upload zeros.

Ruled out on the way: occlusion queries, dead-serial descriptors (none of 4.99 M binds), unsupported texture
copies, buffer copies (`[b2b]`: none), the shared-image sweep (keeping GPU images changed nothing). Write-back with
records that never age keeps the grass (confirmed on video) but costs 48 -> 30 fps standing in Hyrule Field and
grows malloc 215 -> 583 MiB, so it is not the fix.

The default-on **settled-rendering write-back** in `gfx/vk_res.c` notes GPU writes into CPU-visible pools even when
`TOTK_VK_WRITEBACK` is unset. Once the producer fence has completed and a first rendering has gone **60 submissions**
without another GPU write, it copies all levels into staging in the next ordinary command stream. Later commits
land completed bytes in NVN block-linear layout, releasing the record/image pin/staging only when all chunks finish.
This asynchronous path adds no submit/fence wait; busy dirty-page owners/in-flight memory readers defer landing.
Targets rewritten every submission allocate no staging and do no readback/swizzling.

Learning is per live pool/storage/layout, across texture serials and CPU updates: each successful landing doubles
the next quiet interval, capped at **4,096 submissions** (or the configured base interval if larger). A different layout
or pool lifetime starts fresh. This suppresses periodic feedback targets without delaying the first 64 normal tiles.
History is bounded to 4,096 storages and pins no images. WB pins are separate from texture references; released shared
images use the existing 600-submission idle cache, and empty image blocks stay allocated for reuse. Aggressive
cold-image/empty-block release was removed entirely after the Odin A/B showed a ~7 fps cost without a memory benefit.

Each automatic sweep advances at most **1 MiB / ~2 ms**, checked between **16 KiB storage-contiguous spans** and
cleanup calls. Partially landed records retain their staging, source image and per-level/layer progress; later
ordinary sweeps resume them. The existing swizzle engine now accepts GOB-aligned spans (no shadow image/second
swizzler). Every span rechecks full live-pool bounds/identity and CPU freshness, owns only its destination pages,
and leaves padding untouched. New CPU data/GPU storage/layout writes or pool death cancel remaining work.
An explicit invalidate still completes all remaining chunks synchronously for correctness. Reads without invalidate
can observe partial progress until completion. The budget is cooperative: one syscall/OS deschedule cannot be
preempted; separate WB-sweep and staging-free maxima expose overshoot rather than claiming a hard wall deadline.

Learning is **metadata only**: the first CPU write to a noted rendering disarms its entire exact watch in the
fault/pre-write path, and the next commit drops its obsolete record (in-flight staging only waits for safe disposal).
It does not wait for the learned deadline. Only a new GPU write can re-arm WB protection; a CPU-owned storage stays
unwatched by WB. Retiring/landing a record also removes its exact-page exclusions, so ordinary dirty units cannot
keep inheriting them. Range-watch slots are a bounded static ~192 KiB table, not a new per-guest-page array.
WB watches never publish GPU-reader tickets or make a game fault wait for the producer/readback image fence.
Independent real GPU **buffer readers** on the same pages still retain their normal frame-lag safety waits.

**Explicit invalidate is the timing-independent consumer path for noted renderings:** by default, any overlapping
unlanded rendering completes before return, **including age<K** and before a learned deadline. An existing staged
copy is reused; otherwise a copy is recorded after its producer in stream/queue order, published and waited by its
exact ticket. There is no full GPU idle or wait for unrelated later work. Landing may additionally wait for actual
newer GPU readers of those guest pages to preserve dirty ownership. Newer CPU data still wins.
Only **automatic** staging waits for the base/learned quiet interval; reads without invalidate retain that timing
dependency and may see partial progress. The3,000-submission aging applies only when settled WB is disabled.

Before any frontend drain or recording-owner request, default invalidate checks **actual pending ranges** in a small
metadata-only index. A hole inside the broad record-address hull, an already-landed rendering, or a cancelled record
cannot trigger those expensive operations. The index lock never covers resource allocation, Vulkan, swizzling or
recording; the candidate is revalidated under the real locks after draining. This repairs a CPU serialization
regression introduced with the default fallback: `at invalidate 0` did **not** imply no drain/record-lock stalls.
An invalidate that has detached records into its synchronous list publishes a **borrowed metadata view** of those
exact ranges before removing their pending-index rows, and keeps it published until landing/cleanup completes.
A concurrent consumer must still match and serialize through owner service, rather than mistake in-progress data
for already-landed storage. The borrowed view reuses the existing list (no allocation/refcount), reads immutable
range fields only, and never holds the metadata lock across recording, waits, landing or log I/O. Hull holes remain cheap.

**Invalidate stall repair (written; new device check pending):** landing no longer blocks acquiring the recording
lock behind an unrelated whole submission. The caller retains frontend drain, then queues a stack-owned synchronous
request. It only **try-locks** recording ownership; an existing owner services the request at bounded64-opcode chunks
or a sealed-slot boundary. Empty polls use one relaxed atomic load/branch per chunk, with no lock, clock or syscall.
The callback revalidates the latest rendering and runs the unchanged copy/fence/ownership/swizzle/cleanup path
on that owner. Its short request mutex covers publication/completion only, never walker/recorder work or GPU waits.
An unfinished **same-image producer** deliberately defers to its slot seal, not a partial pass snapshot; mandatory
producer/reader/GPU waits remain. Worst scheduling granularity is64 completed opcodes (at most64 ordinary draws),
including draw-free/jump-only streams, not one whole submission. A single driver call/OS deschedule is not preemptible,
so there is no universal hard50ms guarantee. The400ms host hold fixture checks eligible fresh and already-staged
landings finish below50ms and unrelated passes resume correctly; a150ms unfinished producer must finish its final write.

New GPU image writes or overlapping buffer copies supersede staged results; CPU writes are checked both before
staging and after taking dirty page/unit ownership at landing. Landing validates a retained live **pool registration
identity and the full destination bounds**, with its lifetime lock held through the last store (the safety rule from
MIT MacNativeGames `873edff` plus the `4de7b4e` placement repair). It uses the existing write-back temporary-RW/BUSY
protocol, not a new memory-alias subsystem. **Residual race:** a fresh direct game store during the temporary-RW
landing window can bypass the fault handler, **even just before the freshness check**. Writers faulting while RO
are serialized; pre-ownership writes with a published dirty generation and post-restoration writes win.
CPU-visible physical pools have no backend-private always-writable alias; `totk_vpool_backing` is the game's own
physical view, not such an alias. This matches the existing synchronous landing guarantee, not arbitrary-store exclusion.

Controls: `TOTK_VK_OFF=settledwb` disables the default path; `TOTK_VK_SETTLED_WB_SUBS=N` tunes its positive base quiet
interval for automatic copies (default60). Leave `TOTK_VK_WRITEBACK` **unset** for device acceptance: `=1` selects
the separate full legacy flush/authority path; it is no longer necessary for fresh explicit consumers. Every300frames:

```
settled write-back: landed N (M.MMM MiB), pending P, skipped S (CPU wrote since), superseded U, staging B bytes, copies C, at invalidate I, WB faults F (X.XXX/frame), dirty faults D (Y.YYY/frame)
settled write-back memory: records R bytes, history H bytes/Q storages, pinned X.XXX MiB (WB-only Y.YYY MiB), staging B bytes
settled write-back invalidate fresh landings: N (B bytes), copy-wait X.XXX ms cumulative
memory image blocks: used/free X.XXX/Y.YYY MiB (empty blocks retained for reuse)
```

Counts/landed bytes are cumulative; pending/staging/ownership bytes are current. The 64 baked normal tiles should
contribute approximately **64 landings / 13 MiB** shortly after bake (about 214 KB each). Per-storage automatic copies
should decay/plateau for intermittent refreshes; explicit consumers may increase `at invalidate` and fresh-landings.
Staging should drain to zero. Small, continuously rewritten pending images are legitimate; steadily recurring
automatic copies of the same storage or growing staging/history/pinned memory indicate unwanted recurring cost.
`WB faults` counts storage-wide first-write disarms; `dirty faults` includes all dirty-tracker CPU faults. Counts
are cumulative, rates cover the elapsed present frames since the previous report. A CPU-rewritten once-rendered
storage must incur at most one WB fault per GPU re-arm, then none on later CPU-only frames.
Every 300 frames, including the off control, a separate `settled write-back stalls:` line reports interval maximum
wall time, phase/TID/thread/role, lifetime peak, per-phase maxima, WB-created submissions (interval/cumulative),
seal/drain/owner calls and preflight hits/lookups. The former `record-lock` phase is now `owner-delay` (enqueue to callback,
excluding callback work), and `rec-locks` is now `owner-calls`; no blocking recording-lock acquisition occurs. `WB sweep max X.XXX ms (peak P.PPP), bytes max B,
chunks max C, budget yields N` isolates budgeted landing from the whole resource sweep; `staging-free` measures
readback-buffer disposal. Scopes include CPU work/waits, overlap, and are not summed or GPU timestamps.
Ordinary-stream copies create **zero** extra WB submissions.
`TOTK_VK_WB_SERVICE_STATS=1` (default OFF) adds `write-back owner service:` every300presents: queued/inline/cooperative
calls, same-producer deferrals, enqueue-to-service delay p50/p99/max and recording-lock hold max/lifetime peak.
Percentiles are **upper bounds** from100µs bins; overflow uses the observed maximum. No telemetry clock runs per
opcode. Use this switch for the next one-run device comparison; the older artifact's eight clean videos do not
qualify this stall repair.

On Odin 3, the pre-backoff `f36ee4f` A/B retained grass on video, with 49.43 fps on versus 48.36 off and 0.3% versus
0% long frames. It still landed about 200 small targets / 11.7 MiB every 900 frames, and late malloc/image blocks were
about 442/768 MiB on versus 360/704 MiB off. Staging and live readback allocations were zero; these numbers do **not**
establish an 82 MiB write-back bookkeeping leak (pipeline/texture populations differed). The `3aa3fae` Odin rerun
confirmed memory parity (malloc 355 versus 358 MiB; image blocks 704/704 MiB), and landings plateaued at 249/55 MiB,
`at invalidate 0`. It nevertheless regressed to 30.08 versus 48.65 fps: game-thread CPU work rose to 15–20+ ms
versus ~10 ms, GPU time stayed ~14.5 ms, and dirty unprotects rose to ~5 calls/80 pages per frame versus 0.06–0.12
calls on the matching thread off. `b06b53e` reduced faults (WB faults0; fault-unprotect 0.64 versus1.79/frame) but
still ran29.85 versus48.04 fps, malloc357/355 MiB,245 landings/52.6 MiB. Thus faults were **not** the CPU-mode cause.
`8dd99b4`'s exact preflight fixed this hidden serialization (2,100 lookups, zero drains/locks/WB submissions).
Its default ran41.63 versus48.66 fps; disabling aggressive cold-image/empty-block release restored49.23 fps,
grass retained on video, malloc358/361 MiB and image blocks704/704 MiB. Those release features and their tokens
are now deleted, not default-off. Resource-sweep stalls were38/60/71/71/397 ms with release, and still up to317 ms
without it (other70–76 ms spikes, control peak3 ms). Unbounded landing work was the remaining candidate; the
bounded/resumable path was then written; the later eight-run qualification below includes it, but predates owner service.
Use `TOTK_VK_SETTLED_WB_DETAIL=1` for at most 64 `[settledwb]` lines per 300-frame window: dimensions, format, levels,
serial/storage/pool, guest/image bytes, storage landing count, old/next quiet interval and `cause=idle|invalidate`.
It identifies the recurring resources without per-frame logging. Compare the same save/route with the off token.

Round14 device qualification subsequently found **intermittent grass loss in one of four 200-second runs**
of the same save/route; the matching repeated APK also had a clean run. The bounded fix's Odin performance
A/B was48.76 versus48.57fps, but that does not establish visual correctness. The failure's cumulative aggregate
landings (259/56.655MiB, skipped0, superseded20, at-invalidate2) are not per-tile content/version evidence.
In particular, one clean repeat also finished its initial readbacks by frame2700; a large staging backlog is
not required for success. The fresh/concurrent-consumer gaps below are corrected; the frozen combined APK passed eight clean runs.

A same-series diagnostic baseline had **0/4** visible failures, but all four runs invalidated normal tiles at ages
24–50submissions, beforeK60, with no landed/staged bytes. A focused read watch (`o-gwatch1.log:1279–1317`) proved an
exact214528-byte invalidate at age48, zero guest bytes, then a guest **read** at `sdk+0x5b24d0`, LR`main+0x6d6ae8`,
before first staging. Later `main+0x2ef77c` was a **write** caller in this trace: CPU-disarm records show actual
in-range stores, not self-dirtying by WB. The default invalidate now bypasses the base delay for **all** textures,
without a normal-tile size/format special case. It also revalidates a new producer after frontend drain without
rejecting it merely for becoming fresh. The intrusive read-watch hook was removed before fixed-run qualification.
This establishes a real incorrect CPU-read ordering, not sole attribution of the earlier one-in-four visible failure.

Final qualification used one clean combined no-nx APK, HEAD`58e2663`, SHA16`dbf1a90cf2859d14`, **hybrid OFF**,
restored default play env and the same save/200-second route: **8/8 grass retained, no crash**, with third-clip
contact sheets/final screenshots inspected by both OdinOwner and SettledWriteback. Four interleaved actual-play
controls also retained grass. Logs prove nine fresh-normal completions at ages32–47 in runs1/3/4, and two more
age54/59 entries that crossedK during drain and landed before return in runs6/7. A decisive exact chain is
`o-final1.log:4054→4056→4057→4058`: GPU2088, age47,current0 → landed2088 →214528-byte fresh batch → returncurrent1.
No unexplained fresh pending return was found; no concurrent-detached-consumer event occurred on device, so that
specific interleaving remains deterministically host/TSan-tested rather than device-exercised. The earlier standalone
two clean runs are not counted in the final eight. The diagnostic baseline remains0/4 failures: this establishes
exercised-and-handled incorrect ordering plus eight clean videos, **not** a measured failure-rate reduction or sole
historical cause. Frozen APK/route/log/contact-sheet details and all exact chains are in `.scratch/grass/intermittent.md`.
Late intervals had staging0,pending7,zero WB-created submissions/drains/locks; fresh counts plateaued at336–340
(336 tiny loading consumers plus0–4 normals). Copy-fence wait was0.504–0.613s cumulative over each whole run,
not per frame. Loading recording-lock stalls up to~167ms and cooperative sweep overshoots up to5.797ms remain visible.

Always-on `[settledwb-tile]` evidence now targets260x260 RG8 nine-level textures. A bounded512-row metadata ledger
retains storage/pool/serial/image identity, a new version for **every** GPU-write note, the last completely landed
version/submission, staged version and disposition **after** a pending record is removed. `event=invalidate-enter`
therefore distinguishes an already-landed exact version (`gpu=N landed=N current=1`) from an unlanded rendering,
including one whose record was disarmed. `event=invalidate-return` records a changed state after synchronous work.
Repeated invalidates of unchanged state are coalesced; `settled write-back tiles:` retains cumulative counts.
Dispositions identify CPU-newer/untracked, buffer/image supersession, pool death/liveness, layout/allocation failure,
and legacy-only aging. `[settledwb-tile-view]` reports realization/drop/upload/share decisions for those same tiles,
including CPU-pool/shared/GPU-written flags, to separate source landing from later storage-cache selection.
The ledger reads no guest bytes, retains no image/staging refs, and never drains or acquires the recording lock.
The existing host settled fixture checks version re-arm, persistence after landing/CPU disarm, and zero additional
drain/owner-request work for an already-landed consumer. The eight-run qualification above covers this ledger;
the cooperative ownership change and its interaction with hybrid/native interpolation still need Odin qualification.
`invalidate_addr` / `invalidate_bytes` distinguish an actual tile-sized consumer from a broader pool invalidate.
`[settledwb-tile-cpu]` reports the first observed CPU fault/pre-write byte and whether it lies inside the texture,
plus watch/freshness state and GPU/current dirty stamps. Address0 means only an earlier dirty generation was
available; it is not proof of an in-range store. Same-page adjacent writes are explicitly distinguishable.
The GPU landing token preserves generations and never sets this CPU provenance; host checks cover both properties.


Host fixture: `vkdrawtest --settled-wb` (also run by ordinary draw/app tests) uses fresh processes for default K=60,
tuned K=4, and off. It renders a nine-level 260x260 RG8 CPU-pool target, checks every guest-layout byte without
invalidate, checks one-shot/hot-target behavior, CPU writes both before and after staging, and a gated copy whose
destination pool is finalized and made inaccessible before completion. It also covers supersession/re-arming,
full-pool bounds, same-address registration reuse, and nonblocking deferral across a future protected-memory reader ticket.
It also gates a real fresh producer, invalidates beforeK with full WB unset, then immediately compares every byte
of all nine guest-layout mip levels and checks fresh-landing bytes/wait counters. A fresh CPU overwrite still wins.
Host results do not establish the Android grass/performance outcome.

`vkdrawtest --settled-wb-race` adds a targeted concurrent-writer fixture (128 iterations at each of five exact
landing boundaries, each writer repeatedly storing 256 rounds into the texture and adjacent same-page ranges).
It checks three guaranteed interleavings: CPU generation published before ownership rejects landing; a writer
faulting while the token owns still-RO pages cancels before freshness or waits and stores after restoration; a fresh writer after restoration
wins. It also deliberately demonstrates the two **residual** cases: fresh stores in temporary RW before/after
the freshness check publish no dirty generation and can be overwritten. Adjacent bytes must remain intact in
all cases. The deterministic handoffs/fault observations compile only into host tests; no shipping hook overhead.

`vkdrawtest --settled-wb-learning` checks 64 prompt nine-level tiles, then 40 intermittent storage refreshes
at 100 submissions each (new serials) with no repeated automatic copy after the first landing. It also verifies
default invalidate before both base/learned deadlines, reuse of a gated staged copy, newer CPU data, different-layout
supersession with only the new rendering landed, dead pools and zero final WB pins/staging. Cached images/empty
allocation blocks are intentionally retained until their existing cache/lifetime rules allow reuse or retirement.

`vkdrawtest --settled-wb-cpu` learns an interval, then runs four GPU re-arms followed by 300 CPU-only rewrite frames
each over a nine-level 260x260 RG8 range spanning many pages/units. It asserts exactly one fault per GPU re-arm,
no later faults, no pending records/staging/copies, unchanged learning and every CPU byte preserved. An additional
host-gated producer proves CPU ownership returns without waiting its WB source fence. Run both recorder/lag modes
and TSan, as for the other fixtures.

`vkdrawtest --settled-wb-preflight` keeps two aged/backed-off renderings around a large address hole and issues
4,096 invalidates inside the hull: **zero frontend drains, owner calls, seals or submissions**.
An actual overlap must drain/request ownership exactly once and land every NVN level, independently of its age. It also tests an already
staged copy with an unrelated open stream; `settledseal` removes the optional seal while retaining required
copy/reader-safety publication. `--settled-wb-cutout <token>` checks the remaining backoff/CPU-watch controls.
It also replays a newer GPU producer **during** the frontend-drain callback: the candidate seen by preflight is old,
but the final selected producer is fresh. The fixture compares all mips/padding against that exact newer version and
requires one drain/owner call/fresh landing; the former post-drain age filter would incorrectly skip it.
Two real concurrent consumers also share a GPU-gated pair of pending renderings. While the first has detached its
records and waits, another4,096 hull-hole invalidates still take zero drains/locks/seals/submits; a true overlapping
second consumer must wait, then immediately read every mip/padding byte correctly. The gate has a bounded failure release.
`[settledwb-invalidate-inflight]` and `settled write-back inflight consumers: N cumulative` expose these serialized overlaps.
`vkdrawtest --settled-wb-service` holds recording ownership400ms around a real unrelated open render pass, services
at bounded chunks, and requires fresh/already-staged default consumers to finish below50ms with every mip/padding
byte correct. The unrelated pass must resume correctly. A separate unfinished150ms producer must defer, including
its final clear. Two requests must also escape a single unbounded draw-free jump stream, without a draw/return/submit.
Normal/protected-lag and TSan exercise request-stack lifetime, bounded polling and the owner/last-unlock race.
The existing `--lag-walk-bench` reports ns/draw with the idle check; test-only `TOTK_VK_TEST_OWNER_PENDING=1` adds
continuous genuine no-op owner requests and reports completed count, measuring service overhead separately from GPU copies.
The final host matrix passes normal/LLVM21TSan in both modes (2builds+64executions, no sanitizer reports).
The service fixture passes50checks: plain fresh/staged0.792/0.546ms; protected0.831/0.577ms during400ms holds.
An unfinished producer still takes about150ms. Ten alternating protected60-frame benchmark pairs give paired-median
idle cost+0.896% stable/+1.569% churn; paired means+2.402/+1.664% show remaining host noise, not a strict2% bound.
At the observed churn median2622ns/draw,64ordinary draws are about0.168ms; this is a scheduling scale, not a driver/OS bound.

`vkdrawtest --settled-wb-budget` gates a >10 MiB multi-level rendering, verifies a single sweep carries it within
the byte cap, and checks complete synchronous invalidate, partial CPU overwrite, different-layout supersession
and pool death. A separate point-address oracle checks full/chunked swizzling, padding/guards and round trips for
2D, array, 3D and linear layouts. The 64-tile fixture waits for ordinary budgeted completion, never invalidating.

## Pass-split LRZ safety (device validation required)

The P4 hazard is **depth written/cleared in one primary command-buffer epoch and later loaded for depth testing in
another epoch of the same game submission, without a full depth clear or a driver-recognized depth write invalidation**.
The first part consumes the pending clear; reopening uses LOAD. Attachment barriers make the pixels visible, but do
not themselves request LRZ invalidation. A depthless part can also cut between two users of the same depth image.
This is a driver-workaround condition, not a Vulkan rule forbidding cross-command-buffer LOAD. Current
[Turnip hardware notes](https://docs.mesa3d.org/drivers/freedreno/hw/lrz.html) describe GPU direction tracking on
A650+ and safe reuse when the depth view agrees; pre-A650 cannot reuse LRZ between passes. Do not claim that every
Turnip version necessarily has the stale-LRZ bug.

`TOTK_VK_SPLIT_LRZ_SAFE` defaults to 1 and is live. Keep the draw threshold fixed when comparing it with 0.
The safe rule keeps color-writing single-sample scene splits, defers depth-only/no-color-write candidates (it can cut
later in the same pass), and refuses cuts after an unrepairable/MSAA depth attachment has appeared in that submission.
Raster-discard/disabled draws return before the split check and never submit. Existing DONT_CARE/UI/framegen/
diagnostic exclusions remain. The bounded walking-owned cache keys actual VkImage plus game/command-stream epochs,
not a guest texture pointer or view. Eviction conservatively adds a touch after a cut; it never elides required work.
Before a cross-epoch depth LOAD, copy one texel's depth aspect to the current slot's transient buffer, barrier, then
copy its exact bits back and barrier before BeginRendering. Clear loads and known same-epoch loads need no touch.
Stencil is untouched, no shader/depth state is modified, and no CPU readback or additional queue wait is introduced.

[Turnip CopyBufferToImage](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_clear_blit.cc#L3024-L3039)
explicitly calls `tu_disable_lrz` for a destination with LRZ. The proprietary driver's internals are not available here;
the transfer-write workaround is empirical, not a guarantee for every blob revision. Feedback-loop extensions are not
a portable invalidate-LRZ command: they require supported/enabled features, matching image usage/layout and pipeline
state, and are not a reason to claim a false guest feedback loop or disable LRZ for all draws. Use the ordinary transfer
path rather than adding that second rendering convention.

Host: `vkrectest --split-only` runs color-only, same-depth near/far, depth-only and masked-depth-to-color streams at
0/2/3 recording workers, plus protected lag, with safe=0/1/unset and split=off/64/1. It checks byte identity against
unsplit color/depth and exact split/reopen/touch/refusal counts, including discard draws at the threshold. Lavapipe has
no LRZ: this pins decisions and pixels, not the Android artifact. The ordinary `vkrectest` includes this matrix.
The host scene fixture retains 100% of splits: 4/4 at 64 draws and 299/299 at 1 draw, for color-only and same-depth
near/far scenes. The intentionally half-masked scene retains 3/4 (75%) at 64, resuming cuts when color writes return;
the depth-only scene retains 0/4 by design. These are synthetic decision shares, not a measurement of the game workload.


Device gate (not exercised by host tests): same saved scene, camera path, resolution, driver and split=512; record
600 steady-state frames per arm in ABBA order with safe=0/1. Count isolated one-frame luminance dips relative to both
neighbors (exclude fades/cuts), inspect the flagged near-geometry blocks, and retain fps/GPU-ms plus `[split-pass]`
counts. Expect fewer/zero dips in the safe arm without losing most scene splits; a loss of the measured fps gain or
remaining spikes blocks promotion. Screenshots alone cannot catch a one-frame failure. This is separate from the
conditional-clear flicker below, whose earlier fix must remain enabled in both arms.

## Fixed: black flicker (lit geometry went dark for one frame)

**Cause:** the game clears its shadow / AO mask under conditional rendering (two counter reports compared). Our
conditionals were evaluated at record time from counter memory, which reports only write when their submission
completes, so the decision depended on timing; on some frames (camera cuts most of all) the clears ran and wiped the
mask. **Fix:** `condorder` in `vk_draw.c` - report values fixed at record time and read by later conditionals in GPU
order; SAMPLES_PASSED is a cumulative counter. Confirmed on the Thor: no flicker. The investigation log below is kept
for reference (several hypotheses in it were wrong; the final runs are what matter).

Symptom: for single frames, lit geometry goes dark while emissive things and the UI stay. 20-30 times a minute on the
title screen and in dialogue. **The title screen flickers very consistently on every camera cut to another angle of the
background, and not during fades** (user, 2026-10-21) - the cut is the reproducible trigger.

The dark frames are what the game rendered (the presenter's copy is fine): draws, passes and uploads on a dark frame
match a normal one.

Ruled out (the flicker survives each):
- every `TOTK_VK_OFF` switch, and `TOTK_VK_OFF=all`
- `TOTK_VK_SYNC=1` (wait for each submission: no CPU/GPU race)
- `TOTK_VK_SERIAL=1` (every draw its own pass: no in-pass overlap)
- another driver (Purple T23); `TOTK_UNCAPPED=0`; game version 1.0.0 vs 1.4.3
- compressible storage shared across formats (fixed anyway, `compalias`: the Mac's 2026-10-15 flash cause)
- NVN barriers dropped inside a pass (fixed anyway, `passbarrier`)
- non-compressible storage shared across formats: the log counts 0 such shares
- dynamic resolution: the scene stays 1280x720 (`depth target sizes` log lines only show shadow / cube passes
  alternating)

Found with the pass probe (2026-10-21, title screen, three flashes): every pass up to about pass 52 matches the good
frame. The first pass that goes black is a **640x360 RGBA8 (NVN 0x25) pass with 3 draws**: mean 0.0000 in the flash,
~0.72-0.75 in the good frame. The passes after it that read it (640x360 / 320x180 RGBA8, 1 draw each: a blur chain) are 0
too, then the 1280x720 R11G11B10F (0x3f) scene lighting (12 draws) comes out near 0 and everything after it is dim. A
half-resolution mask around 0.75 multiplied into the lighting is most likely the shadow / ambient-occlusion mask. An
RGBA8 target writes 0 for NaN, so the pass may compute NaN rather than draw nothing. Next: what that pass reads
(the probe report now lists each differing pass's fragment shader, the textures it samples and whether this frame drew
them, and missing textures / buffers), especially inputs not drawn this frame (history from the previous view).

Second probe run: that pass is fragment shader `679c5257f9a55f7f`. It reads the scene depth (1280x720 DEPTH32F 0x34),
G-buffer targets (1280x720 0x25 / 0x38), the linear depth (1280x720 R32F, drawn this frame) and a **DEPTH16 (0x32) map
that is 1024x1024 in the flash frame but 512x512 in the good frame** - almost certainly the shadow map; the probe said
"not drawn this frame" because depth passes were not tracked yet. So on a camera cut the shadow pass samples a different
(bigger) shadow map, and that one gives "fully shadowed". Next run tracks depth passes, copies, clears and image stores
too, so the report says what wrote that 1024x1024 map in the flash frame (or that nothing did). Suspects: a shadow map
the game rebuilds on a cut through a texture copy / blit between depth formats or sizes (`copy_t2t` falls back to a
blit, which Vulkan does not allow between different depth formats), or one never rendered in our replay.

Third probe run: both frames' shadow maps are written by a depth pass with **1 draw** right before the colour pass 30
(1024x1024 in the flash frame, 512x512 in the good one). One draw is not a shadow render of the scene: it is a
full-screen pass writing depth - most likely the game copying a cached shadow map into the one it samples. Next run
samples depth values (8x8 points) of every depth pass and lists what that 1-draw pass reads, to see whether its source
is empty or never written on a cut.

Fourth probe run (3 flashes): **the shadow map is not it.** In two flashes the good frame read the same 1024x1024
map, in the third both read 512x512, and the depth values match (0..1, mean ~0.27). The flash and good passes have the
same shader, the same textures, the same depth inputs - yet one outputs 0 and the other ~0.75. What is left is the
non-texture input: uniform / storage buffers (shadow matrices, cascade splits, light parameters). Next run reads the
first 256 bytes of every uniform / storage buffer each probed pass of up to 8 draws binds (on the CPU at record time; a
first version copied them on the GPU after each pass and the game stalled 30-40 s per frame) and the report prints the ones that differ, with the floats. NaN or zero matrices on the flash frame would explain it.

Fifth probe run (buffers): the uniforms of the shadow-mask pass (`679c5257f9a55f7f`) match between flash and good
frame up to camera jitter (shadow / view matrices fine, no NaN). The one real difference is **fragment storage buffer 4**
(0x3a4e0f000 in that session): at byte 72 the flash frame has 0 where the good frame has 2 (read as float bits: small
integers). That buffer is GPU-written, so this is likely the per-tile feedback the Mac describes (nvn_mtl.m
`msl_stores_through`: the lighting quads store a per-tile word the next frame reads; material shaders store a per-tile
class-group flag). Caveat: the probe reads it on the CPU at record time, so it shows what earlier frames' GPU work left
in memory, not exactly what the GPU sees. Next: read the shader (`679c5257f9a55f7f`, from the build's shader seed) to see
how storage 4 gates the output; find which passes write storage 4 and whether a camera cut clears or skips them.

The shader (`679c5257f9a55f7f`, decompiled with the build's spirv-cross): 17 textures, 5 uniform banks, one storage
buffer `fp_s0` (binding 18) that it both reads (word 100) and writes (`fp_s0[...] = 1` at a per-tile index when a
condition holds: the per-tile feedback), 6 colour outputs. **Output 0 is (1.0, y, 1.0, w): x and z are constant 1**, so a
mean of 0 on colour 0 means the draws wrote no fragments at all there - the pass is not computing a bad value, its
fragments are rejected (depth / stencil test against a class-depth or stencil the marker writes from last frame's
per-tile flags, scissor, or write mask). The probe now logs each differing pass's depth / stencil / colour-mask state
and the depth passes right before it (its own depth attachment too, with depth samples), and up to 16 inputs.

Sixth probe run (state): the flash pass has a **640x360 DEPTH16 attachment that holds 0.5 everywhere** (written by a
1-draw depth pass just before - the marker), and its draws use **depth test EQUAL (NVN func 3), no depth write, no
stencil, full colour mask, no blend - identical in the flash and the good frame**, with the same 0.5 depth. So the 3
draws' fragments arrive with a depth that is not exactly 0.5 on the flash frame, and with 0.5 on good frames. EQUAL on
DEPTH16 needs the bit-exact same depth from rasterization as from the marker. What sets the draws' depth: their vertex
positions (vertex shader, vertex data, vs uniform 1 - camera matrices that differ by jitter), the viewport depth range,
depth bias, depth clamp. Next run logs per draw: vertex shader key, primitive / count / instances, viewport, depth
range, bias, clamp, cull, scissor and the first vertex's data.

Seventh probe run (draws): the 3 draws are also identical in both frames (vertex shaders 345bb4abe6b6bbf4 /
97c2cb79ae8c75a5, 6-vertex quads, viewport 640x360, depth range 0..1, bias off, no clamp). Everything the game controls
matches. **Hypothesis (being tested): a DEPTH16 rounding tie.** 0.5 * 65535 = 32767.5 is exactly halfway between two
steps, so whether the marker's 0.5 and the shading draws' 0.5 land on the same step depends on an ulp of
interpolation error, which on Adreno can change sign with the jittered vertex positions; on a cut frame they land
on different steps and EQUAL rejects everything. The fix `d16tie` (on by default, `TOTK_VK_OFF=d16tie` to compare)
adds a fifth of a depth step of constant bias to every draw into a DEPTH16 target and nudges DEPTH16 clears that sit
on a tie. The probe now prints DEPTH16 samples as raw steps (32767 vs 32768 would confirm it).

Eighth run: the d16tie fix did not help, and the probe shows the marker stores step **32768** in both frames - no
tie. Ruled out. The shading draws' vertex shaders (345bb4abe6b6bbf4, 97c2cb79ae8c75a5) draw a full-screen quad from
gl_VertexID, and its depth is `k + (1 - k) * nibble_swap(class) / 255` with `class = max(trunc(vp_c11[25].x), 0)`
(uniform slot 8, byte 400) and `k = vp_c4[147].z` (slot 1, byte 2360) - both beyond the 256 bytes the buffer probe
reads. So on a flash frame these draws are given a different class (or k) than the marker: the uniform words for the
class are what to check. The probe now logs, per draw, slot 8 [25], slot 1 [147] and slot 1 [7] as the GPU sees them
(snapshot / memory plus this submission's inline updates, `vk_ub_peek`), for the shading draws and the marker.

Ninth run: the class words are identical in flash and good frames too (draws with classes 16, 24, 48, k = 0.5;
the marker's vs is 9a2cc1e8547cf9f1). And by the vertex shader's formula those classes give depths 0.502 / 0.753 /
0.506 (D16 steps ~32896 / ~49344 / ~33153), while the stored depth at all 64 probe points is 32768 (0.5) in both
frames - so EQUAL should fail at those points in the good frame as well, yet the good frame's colour there is ~0.75.
So the good frame's 0.75 may not come from this pass's fragments at all, but from what the colour target already held
(the pass loads it). Hypothesis: on a camera cut the target (or the one feeding it) is a fresh image with no content,
where on hardware the memory still holds the previous content. Next run logs, per differing pass, whether colour 0 was
cleared or loaded, its image handle and depth image handle, whether the same images appear in the other frame, and
where they were last drawn earlier in the frame.

Tenth run: **the first real difference.** In the flash frame the shadow-mask pass's colour target is *cleared at the
pass start*; in the good frame it is *loaded* (it then keeps the ~0.75 content that pass 51/54 drew into it earlier in
the same frame). The passes after it (the blur chain, 59 / 61 / 65) show the same: cleared in the flash, loaded in the
good frame. So the shading draws never contribute at the probe points on either frame (EQUAL against the 0.5 marker
fails there); the mask lives on carried content, and on a cut frame something clears it. Either the game issues the
clear only on cut frames (then on hardware the shading draws must fill the mask, and our marker / EQUAL path is what is
really broken: the marker writes 0.5 everywhere - per-tile class feedback not working?), or the game issues it every
frame and we skip it on normal frames (`clear_color` returns when `S.render_disabled` - conditional rendering, whose
counters we fake as "all visible"). Next run counts, per pass, the colour clears issued / skipped / done inside a pass
since the previous pass, the last clear colour, SetRenderTargets and conditional-render ops, and the render-enable state.

Eleventh run: **found the switch.** Between the passes, the game issues its colour clears of the mask under
conditional rendering (SetRenderEnableConditional, comparing two counter reports). Flash frame: 7 conditionals, 0
disabled, 7 clears done. Good frame: 6 conditionals, all 6 disabled, 6 clears skipped. Our conditionals were evaluated
at record time from memory, but a report's memory is only written when its submission completes - so they compared
whatever older, already completed reports had left there, and the outcome depended on timing (a camera cut changes
the sequence). Fix `condorder` (on; `TOTK_VK_OFF=condorder` for the old behaviour): each report's value is fixed when
it is recorded and kept per address; conditionals read those first (GPU order, as nvn_mtl.m `cond_eval` does with its
pending ring). SAMPLES_PASSED became a real cumulative counter (ResetCounter zeroes, each recorded draw adds its
vertices x instances; "everything visible" still, but two reports with no draw between them are now equal). Desktop
test: both cases pass with the fix, fail with it off. To verify on the device: the flicker should be gone; if the
picture is instead dark all the time, the conditional decision is now consistently "clear" and the EQUAL shading
path is the next thing to fix. Watch also for objects vanishing (occlusion queries now return per-draw counts).

The probe makes frames slow and the game froze a few seconds after each probe run (cause unknown; the completion
thread now logs "the GPU has not finished submission ..." every 5 s if the GPU hangs).

How the probe works: `TOTK_VK_PASSPROBE=1`. Every pass's colour target is sampled at 8x8; when the flash detector confirms a dark
frame it logs which passes' output differs from the good frame right after it (the same camera angle) (`pass probe, flash frame ...`). Passes are paired by size and format (the frames' pass lists differ: shadow / cube passes take turns). The first pass that
goes dark or NaN is where to look. Multisampled targets are not probed (blitting them is invalid; the first version did, and the game froze after a few reports - maybe that, unconfirmed). Ideas for what a camera cut changes: temporal history reset (TAA / exposure /
reprojection weights; NaN * 0 is NaN), occlusion results of the old view (we report every query as visible), the
per-tile material-class feedback the next frame reads (nvn_mtl.m `msl_stores_through`).

Integration: precise occlusion is available with `TOTK_VK_OCCLUSION=1` (default off); GPU timestamp reports are on
unless `TOTK_VK_GPU_TIMESTAMPS=0`. The observations above predate these dev changes.

## Adreno terrain squares: gather-coordinate bias

`TOTK_GATHER_BIAS` is a **host translation/build-time** switch, default on; `TOTK_GATHER_BIAS=0` disables it.
It is not a `totk.env` runtime toggle. At a texel centre Adreno can choose a different gather footprint than
NVIDIA; each normalized gather coordinate now receives `1 / (512 * textureSize(texture, 0))`
(`GatherBiasPrecision=8`, half a subtexel). This targets the 64 px black terrain squares, not every shadow glitch.

Regenerate the seed with the normal Android build command plus `--shaders` (also supported with
`--runtime-only --shaders`); use `TOTK_GATHER_BIAS=0` before that command for the control seed.
Deploy the resulting **shaders**, not only the APK/runtime. Rebuilding only the runtime cannot change a gather.
Seed identity includes the effective gather policy, so switching between on/off cannot reuse the other seed.
Source/project/patch/publish-script/vendor changes or a replaced binary also trigger a translator rebuild before
`--shaders`. That rebuild runs `publish_native.sh` from the source checkout, which has `vendor/ryujinx`.
An explicit `--translator /path/to/totk_xlate` is caller-owned and is never automatically rebuilt.
Use the same policy for subsequent shader fills; do not mix on/off variants under one seed.

**Do not diagnose an optimizer failure from the old “N gather-biased shaders loaded” marker.**
Upstream's marker scanned SPIR-V opcodes 117/118; `OpImageQuerySizeLod`/`OpImageQuerySize` are **103/104**.
A size query alone would not prove bias anyway. Fresh upstream translator builds (patches 0001–0007)
already preserve Gather through both AST optimization modes and emit the bias. The real delivery holes were
stale translator binaries and a seed reuse identity that ignored `TOTK_GATHER_BIAS`; no speculative
optimizer rewrite is needed.

Host evidence on the v143 seed: **686 of 25,282 SPIR-V variants** (593 shader keys) contain **1,314 gathers**,
including 322 variants from `agl_technique_shdw.sharcb`. Every gather's coordinate dataflow reaches both
dimension-specific query-size / multiply-by-512 / reciprocal / add chains, with `TOTK_SHADER_DEBUGMODE`
both 1 and 0. No `OpImageDrefGather` occurs in this seed; its gathers are plain `OpImageGather`.
All 686 control translations byte-match the old packaged SPIR-V; all 686 biased translations differ.
Changed SPIR-V totals 26,557,880 bytes (25.33 MiB), up 991,776 bytes; filenames/dependency signatures stay unchanged.
`spirv-val --target-env vulkan1.1` accepts 684/686 biased modules; the other two fail the same pre-existing
StorageImageWriteWithoutFormat capability check in their unbiased controls.

For a concrete shadow-package example, disassemble variant
`76656f2cff103018983f21337a69430e4c6a157629975057fe464621598d302c-b7530f698e76`.
The control feeds the original x/y into `OpImageGather`; the biased form queries width and height separately,
converts them to float, multiplies each by 512, divides 1 by each result, and adds the results to x/y before
the gather (and before the existing NaN-coordinate sanitization).
The previously logged shadow-mask shader `679c5257f9a55f7f` is covered too: all four packaged variants
(`3133577cb209`, `b7530f698e76`, `15d9a7463766`, `bf62de19337d`) have two gathers, each with both x/y bias chains.

Device A/B: use one save, daylight and camera at Lookout Landing and sunlit sloped terrain, no other switch
changes. Compare stationary views, slow pans, camera cuts and moving sun/shadows on Thor/Turnip and Odin 3.
Check the normal `shader seed:` startup path and `frame N:` lines; inspect the host seed manifest's
`gather_bias` field and `.res.json` notes (`[totk-tp] gather bias inserted`) rather than trusting a query opcode
counter. Confirm squares disappear without shifting shadow edges or introducing seams. Host SPIR-V proof
does not establish visual correctness on Adreno; that still needs this A/B.

## Diagnostics

| Setting (`totk.env`) | What it does |
| --- | --- |
| `TOTK_VK_OFF=a,b` | turn off backend features: `rename,ds,dyn,block,ts,span,snap,mirshare,present_wait,compalias,passbarrier,condorder,writeback,settledwb,settledbackoff,settledinval,settleddrain,settledseal,settledcpu,texcache,d16tie,storagewrites,prepare` or `all` (`prepare` keeps the opt-in `TOTK_VK_PREPARE_MASK` off) |
| `TOTK_VK_SETTLED_WB_SUBS=N` | positive automatic quiet interval (default60); successful storage landings double the next automatic interval up to4096 submissions; explicit invalidate bypasses both base and learned delays |
| `TOTK_VK_SETTLED_WB_DETAIL=1` | log at most 64 settled-storage details per 300-frame window (shape/format/levels, serial/storage/pool, bytes, landing count, quiet intervals, idle/invalidate cause) |
| `TOTK_VK_WB_SERVICE_STATS=1` | default OFF: per300present recording-lock hold max/peak and owner-service delay p50/p99 upper bounds/max; no idle opcode clocks/locks |
| `TOTK_VK_WRITEBACK=1` | select full legacy invalidate flush/authority behavior (default off); not required for fresh default explicit consumers; leave unset when measuring settled WB |
| `TOTK_VK_OFF=settledbackoff` | fixed base quiet interval for every rendering; disable learned history, not settled write-back |
| `TOTK_VK_OFF=settledinval` | disable default explicit-consumer landing (all ages); ordinary automatic settled copies remain |
| `TOTK_VK_OFF=settleddrain` | diagnostic: skip frontend drain for a real default invalidate candidate; can weaken queued-command/CPU-consumer ordering |
| `TOTK_VK_OFF=settledseal` | omit optional seals for an already-submitted copy; required copy/reader-safety publication remains |
| `TOTK_VK_OFF=settledcpu` | diagnostic: restore per-page exact watches and delayed CPU cancellation rather than first-write storage disarm |
| `TOTK_VK_XLATE_STATS=1` | option-0 translation measurements, exact `1` only, cached at first replay. Every 300 real presents: `[xlate-summary]`, `[xlate-freeze]` / `[xlate-miss]` (exclusive first cause per stage), `[xlate-uniform]` (slot/address version churn), `[xlate-textures]`, and `xlate recordings:` / `xlate recording coverage:` (previous-frame hashes, exact and address-abstracted). `[xlate-texture-types]` groups cumulative per-ID target/view sets every 3000 frames and at normal exit. Diagnostic-only bounded tables; no allocation or opcode/binding work when off, one cached flag branch per graphics draw. See `docs/translation-research.md` §4 for denominators, unknowns and limits; run separately from timing captures. |
| `TOTK_VK_UB_RENAME=1` | opt in to uniform renaming and submit UBO snapshots (default off; known remaining in-game emissive flicker). `=0` or `TOTK_VK_OFF=rename` uses ordered direct updates with CPU overlays; protected frame lag uses private UBO-only shadow updates with exact alias materialization |
| `TOTK_VK_D16TIE=0` | turn off the DEPTH16 nudge (rasterized: depth bias; fragment-shader written: a patched specialization constant; a fifth of a step off the rounding tie the class-depth EQUAL tests sit on). On by default: without it water renders black |
| `TOTK_VK_SYNC=1` | wait for every submission |
| `TOTK_VK_SERIAL=1` | end the pass after every draw |
| `TOTK_VK_FMT_SPLIT=1` | no storage sharing across format classes at all |
| `passdump <n>` (a line appended to `TOTK_INPUT_FILE`) | the n frames after the next: every pass's colour targets and depth / stencil as PNG (384x216 samples, float formats scaled to their own range, NaN magenta) in `passdump/f<frame>/`, with `index.txt` (targets: serial, size, NVN format, level / layer; draws; first fragment shader; value ranges). Pull with `adb pull /sdcard/Android/data/com.aure.totk/files/passdump` |
| `TOTK_VK_PD_SPLIT=<fs prefix>,...` or `d16` | with passdump: end the pass after every matching draw (d16: every draw of passes with colour targets and a DEPTH16 depth), so each draw's output is dumped |
| `TOTK_ASYNC_REPLAY=1` | replay submits and presents on a separate translation thread (the game's next frame overlaps the replay) |
| `TOTK_INLINE_BULK=0` | copy called command lists (`CallCommands`/`CopyCommands`) record by record instead of in one block. The block copy is the default since it showed byte-identical results in `dm_nvn_probe_inline_bulk` and was the largest `memcpy` source on the game's recording threads |
| `TOTK_GATHER_BIAS=0` (host build environment) | disable gather-coordinate bias; regenerate with `--shaders` and deploy the shaders |
| `TOTK_VK_PASSPROBE=1` | per-pass colour probe, reported on each detected flash (slow); also compares the uniform / storage buffers (read on the CPU) of passes with up to 8 draws |
| `TOTK_VK_FRAMES_AHEAD=1` | one frame in flight |
| `TOTK_VK_BLACK=0` | turn off the flash detector (on by default; logs `dark flash at frame`, saves `shots/flash_*.png`; misses some) |
| `TOTK_VK_SHOT=N` | save every N-th frame |
| `TOTK_INPUT_FILE=<path>` | diagnostic commands, one per line, read as they are appended; `threads` dumps every guest thread's wait state and return chain to the log (`[totk-dump]`): use it on a hang. On the app: `/sdcard/Android/data/com.aure.totk/files/input.txt` |

Log lines worth grepping: `dark flash`, `pass probe`, `depth target sizes`, `textures:` (sharing counters),
`frame N:` (fps, draws, passes).

## Build gotchas

- Run builds inside the dev distrobox; builds outside it used stale binaries more than once.
- `install_app.sh from-device` copies from `/data/local/tmp/totk` (the old adb-shell layout); use `data` to copy
  from the computer. The app needs exactly one version folder under `rewritten/`.
- adb cannot create folders under `Android/data` on Android 13; `install_app.sh data` makes them first.
- After installing, an old instance can hold the game-slot lock: clear the app from recents.

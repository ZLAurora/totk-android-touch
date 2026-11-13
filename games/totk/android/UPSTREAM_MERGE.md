# Android upstream integration (2026-10-22)

Integration base: `dev` at `8494a6b`; upstream: `origin/android-port` at `d2eb744`.
This ledger classifies all 57 upstream-only commits before merging. **Useful** means complementary;
**duplicate** names the existing dev implementation; **conflicting** requires one reconciled implementation.
Historical upstream device evidence is in `DEBUGGING.md`; dev device evidence is in `VULKAN.md`.
The per-round sections below distinguish later owner-reported combined-build results from host-only verification.

## November 11 shader-list refresh (V rows only)

Source: switch2mac fetched `origin/main` `b2ad88d8cc2e880f5d895d7a2580e1355f00652b`
pins MacNativeGames `a8ff9bc429bf451dd1040807c6dae6fa2a0cba55` in its TotK `game.json`.
The reference check mis-attributed MacNativeGames `a4bd59576479` to switch2mac: wrapper commit
`ef0b08c` quotes that engine commit's subject; 17,335 is the source list's V-row total, not our additions.
The previous import is `ff63a162b35d8b486b941191785d8e957c1f5df2` plus the four local Sky V rows
documented in `APP.md`. All 13,619 V rows of that upstream checkpoint survive in our existing union.
The imported identifiers remain covered by the unchanged root MIT licence, copyright 2026
MacNativeGames contributors. No shader code, translated output or Nintendo material is imported.

The reviewed V-only selection adds **301** rows for **197** shader keys in our authenticated
1.4.3 inventory. Of 3,358 V rows absent from our prior list, 3,057 name shaders this build
does not have and are excluded. Keep all existing rows, including Vulkan attr/fout and local-fill
coverage: upstream's 43,788 missing prior V rows are not deletions to apply here.
Final V/R/S/C counts: **58,066 / 61,613 / 1,557 / 63**; non-V rows are byte-for-byte unchanged.
The new list SHA-256 is `9b00d9a9083df6d2d8667969f9b105192851fd7e52d7fa93874ff0a7e5eb90b9`
(previous `74aa399c…`). The translation chain and early-fragment policy do not change.

Unlike the prior V/R union, this selection explicitly excludes all source R/A/B/T/S rows.
Upstream now packs its 79,365 R keys as two variant names plus 384 bytes of fixed state,
with zero runs encoded as `z<hex count>.`; our hex-only parser cannot import that representation.
Its 1,118 A rows preserve the answers behind warm-row variant suffixes after V pruning.
They are not V rows and are not silently reinterpreted. B/T remain unsupported Metal recipes.
A separate change would need bounded packed-key validation in Python and the C list validator,
A suffix/answer validation and seed-aware warm-name resolution, plus parity/malformed-input tests.
Even then Metal R rows do not directly supply Android's distinct Vulkan portable-key/prewarm formats.

The list digest automatically changes preparation/resume identity; no separate epoch constant is
bumped for data-only additions. APK updates do not automatically rebuild an already completed
on-device shader stage or invalidate the driver pipeline-cache envelope. Existing installs must
explicitly rebuild shaders to gain this coverage; desktop generation still requires `--shaders`
and the exact-name retention procedure below. See `APP.md` for update and device-qualification costs.
Host measurements, validation and test totals are recorded in
`.scratch/shader-list-refresh/report.md`; no APK build or device qualification is claimed.


## Native-UI follow-on to round 10.1

Historical v1 integration below is superseded by **native UI v2** (`APP.md`): the opt-in now keeps a
720p guest canvas, upscales once at the UI boundary and redirects UI rasterization into host-native siblings.
The original merge evidence remains historical; it is not v2 device qualification.

Signed merge `6ab94b8` integrates authentic `feat/android-native-ui` head `04810b6` on top of `9008f27`,
without textual conflicts or reconstructed topic commits. `TOTK_NATIVE_UI` remains **off by default**.
Its v143-only 1920x1080 canvas retains handheld mode and independent world resolution/native DRS.
It does not alter lag/chunk/recorder defaults (see the current policy below), only stale output-size and host-scaler choices.
Copy and AHB share the window-extent policy; optional SMAA precedes the overlay and exact 1:1 requires a
1920x1080 window/full canvas crop. Other window sizes aspect-fit with bars.

Integration adds the new presenter-policy header to the existing host-test dependency rule; the recipes continue
to compile C sources only. `APP.md` retains the topic's full native-UI configuration, stock-heap bounds, GPU memory
costs and paired device acceptance recipe. The shader seed, shader-list data and build pipeline are unchanged.

Fresh evidence is under **`.scratch/integ3/round10_2/`**. All seven changed C files pass Android `cc1.sh`;
the unsupported-version post backend also compiles. A fresh Android `APP=1` native build/link, current Java/dex,
both APK variants, signatures and exact payload/manifest/CRC/compression checks pass. The companion/legacy APKs
are **35,920,522 / 8,336,768 bytes**, using identical current native payloads, 65,364-byte dex for companion and
the existing 2,245 artwork files / 33,731,700 bytes. APK SHA-256 values are
`4f51a2dabb5af959e45171a653b3d42bab7f45ea2bf7663bb1aa4d21203beaae` /
`cf048c222cbd3eb6e27929fc5a0fd250b3e17ae51f3998948e2a31b3e507dafd`.
The signing key is the existing scratch key, not the owner's production certificate.

Production native-UI fixture checks pass normally and with its C fixture instrumented by TSan: authenticated NSO
and segment bytes, nine original-word guards, native canvas/crop publication and independent scene selection,
defaults, stale output-option priority, malformed switches, iOS/export controls, and authenticated stock HeapDef
with zero extra allocator/app-pool bytes for the supported profile. Existing game-render patch verification also
passes normal/TSan: 23 original-site corruption refusals, defaults/bounds/independent selection, and original
ARM64 far-clamp, shadow-resolution and FXAA semantics. These are bounded production-C/native-byte checks, not game
startup, GPU performance or device evidence.
DRS60 guard/native-predictor checks also pass normal/TSan on the combined source; its opt-in/default-off policy
and shipping 30-fps exclusion remain unchanged.

The focused graphics matrix passes **228/228 fresh-process cases**: 104 normal, 104 TSan, 18 ASan/UBSan,
and two unsupported-version controls (native off passes; native on refuses startup). It covers native on/off/unset,
overlay off/fps/full, off/SMAA, off/FSR/SGSR, postchain UNORM/sRGB, and core/draw/span suites with current Android
default workers plus explicit recorder 0/3. The fixtures independently check exact native pixel/glyph/bars/padding,
detach/reattach through wide/tall/missing-dimension windows, host-scaler bypass and postchain/overlay ordering.
Every positive process also checks the once-only resolved FPS60/lag/chunk startup line; core/draw/span checks verify
the actual recorder-worker log. `LP_NUM_THREADS=0`, sanitizer halt-on-error is enabled, with no suppressions.
Current C builds and completed sanitizer logs are clean; javac emits its existing source-8 bootstrap-classpath warning.
An additional **48/48 normal/TSan cases** explicitly exercise all four overlay corners across off/SMAA and
off/FSR/SGSR. The first matrix's corner sweep mistakenly set an unused harness variable and therefore repeated the
default top-right corner; the replay uses the actual `TOTK_OVERLAY_POS` and checks each corner independently.
The total is 276 passing process cases, not 276 distinct configurations. Authoritative records are
`focused-matrix-results.json`, `corner-matrix-results.json`, `cpu-check-results.json`, the verifier JSONs,
`android-check-results.json` and `apk-comparison.json` under the scratch directory above.

Real AHB import/SurfaceControl/compositor operation, physical output mode, native UI quality and scene/postpass
sizes, memory/performance/power, and the existing open shadow observation still need owner device qualification.
No device/real-ADB operation, `build_android.py`, shared `build/` write or push occurs in this follow-on.

## Current frame defaults (2026-10-26; supersedes round 10.1)

`integ/round10_1` started at `c8dc721` and enabled protected lag for Android shipping 30.
Thor's newer Lookout evidence supersedes that choice: `TOTK_FRAME_LAG` now defaults off on every
platform and in every FPS mode; only exact `TOTK_FRAME_LAG=1` enables protected lag.
Android shipping-30 (`TOTK_FPS60` unset/`0`) defaults `TOTK_GPU_TIME` to hardware; FPS60=1
and other platforms/BOTW retain latency. Explicit hardware/latency overrides either mode;
unknown values log and retain latency. Lag-off/hardware measured 29.93 fps (minimum 29.6,
1.4% long) versus lag-on/latency 27.71 fps (minimum 22.6, 13.2% long). Latency reports
overstated GPU time by roughly 9–12 ms; hardware's roughly 23 ms agrees with KGSL's roughly
22.7 ms, explaining the historical controller drops without requiring lag. Full supplied
device results are in `docs/translation-research.md`.
The chunk rule is unchanged: `TOTK_VK_CHUNK_DRAWS` defaults to 600 with resolved lag and no native
VMA pressure, otherwise 75, so it now resolves to **75 by default**. Explicit positive values
or `0` win. Startup reports resolved `FPS60`, `FRAME_LAG` and `CHUNK_DRAWS` once.
Android recorder workers additionally default to **2**; explicit `TOTK_VK_REC_THREADS=0` retains synchronous
recording, and ordered async submit keeps its existing worker-enabled rule and diagnostic exclusions.
The authentic `feat/android-drs60` topic `0dce4f5` is integrated separately: `TOTK_DRS60` remains default off,
does nothing to the shipping 30-fps profile, and opts into a capped native GPU prediction budget with FPS60 enabled.

The unexplained shadow report occurred in a round-nine run combining lag and the slab driver; the observation did not
establish lag as its cause. This decision changes the default, not the historical measured record or that uncertainty.

Source checkpoints are `21d4c04` (shared lag policy, effective chunk threshold and once-only startup log),
clean signed merge `4fe26d2` of authentic DRS60 head `0dce4f5`, and `2b70ed4` (two-worker Android default).
All lag consumers share the resolver: guest waits, protected backend capture and loader frame-wait identification.
No frontend-only environment rewrite stands in for resolution. Explicit chunk thresholds remain authoritative even
after pressure; an unset/empty threshold falls from 600 to 75 after the actual VMA-pressure cutover.

The following round 10.1 verification records are historical checkpoint results, not verification of today's defaults.

The lag-only checkpoint passes **458 focused GPU cases**, normal/TSan plus ASan/UBSan default coverage:
core/draw/spans at recorder 0/2/3, FPS60 unset/0/1, lag unset/0/1, chunk unset/empty/0/75/600 and pressure cutover.
Each fresh process checks exactly one resolved lag/chunk startup line alongside the existing independent slot/pixel
oracles. Header-level startup tests pass normal, TSan and ASan/UBSan across eight platform/macro variants; ordinary
host/BotW lag defaults remain unchanged, and explicit overrides win. These records remain in `.scratch/integ3/round10_1/`.

After the additional recorder-default/DRS60 request, fresh current-source outputs use
**`.scratch/integ3/round10_1/final/`**. All ten changed C units plus the BotW-loader context pass `cc1.sh`;
fresh Android `APP=1` native build/link, current Java/dex and both real APK variants pass. The companion/legacy APKs
are **35,919,149 / 8,335,395 bytes**, with freshly linked identical native payloads, 65,364-byte dex for companion,
and the existing 2,245 artwork files / 33,731,700 bytes. Signatures, exact entries/payloads, CRC, compression and
unchanged manifests are verified; the key is scratch-generated, not the owner's production certificate.

DRS60 verification passes normally and with its C fixture instrumented by TSan: authenticated v143 NSO/segment hashes,
nine original guard words/data, twenty occupied-padding refusals, disabled/30-fps controls, malformed values/bounds,
five budget operands and twenty synthetic native-predictor workloads. The original tighter budgets/cost reporting
and preserved operands/register/flag/stack contract remain checked. The existing DRS_MIN/template integration also
passes normally/TSan against the authenticated 292-member pack, including refusal and raw-zstd boundary tests.
These are source/byte-authentication and bounded native emulation tests, not game execution or device performance.

The final combined default-worker matrix passes **343/343 cases** on current source: normal/TSan core, draw and
span suites with recorder unset/0/2/3 across FPS60 and lag defaults/overrides; independent chunk slot/pixel checks;
default-worker pressure cutover; GPU_PROFILE 1/2 and PASS_MERGE inline guards; explicit async-submit-off; pipeline
checks; ASan/UBSan fresh-default cases; and unchanged ordinary-host lag/worker defaults. Each process independently
checks the once-only resolved lag/chunk line, actual worker-count startup log and async-submit **configuration**.
Existing software-protection exclusions can still execute synchronous submission on lavapipe; they are not mislabeled
as native asynchronous execution. The final source matrix takes 569.782 seconds; no final sanitizer or C compiler
diagnostics are present (`LP_NUM_THREADS=0`, halt-on-error enabled, no suppressions).

Authoritative records are `final/focused-matrix-results.json`, `final/android-check-results.json`,
`final/drs-check-results.json`, the DRS guard/emulation/template reports and `final/apk-comparison.json`.
The companion/legacy APK SHA-256 values are
`fc775fe4fc93694a0541f63df5190c29a526a9d82285c0c6b6c173afd8919217` /
`84e3deed1576dcb3d5979b27cd6e762adc854f459caba5646b219823b88adb02`.
No device/real-ADB operation, `build_android.py` invocation, shared `build/` write or push occurs in this round.
The open shadow observation and DRS60 device/FPS/quality qualification remain unresolved by these host gates.

## Round-ten integration

`integ/round10` starts at reviewed-original-upstream head `e756677` (round nine `dd366b3` plus the selective
`5281186` integration below). Original topic ancestry is retained:

| Order | Original topic head | Signed merge / resolution |
|---|---|---|
| 1 | `feat/android-sgsr` `89d6fa3` | `f60c831`; clean. Adds opt-in SGSR presentation scaling, independent golden-image/resize coverage and third-party notices. Existing SMAA/FSR/off modes and final overlay composition remain. |
| 2 | `feat/android-dev-overlay-default` `14e2b5b` | `b7e84e6`; clean. Development builds default to the full top-right overlay; explicit runtime settings still win. `--release` forwards `DEV_BUILD=0` and makes the manifest non-debuggable. |
| 3 | `feat/android-fps-auto` `6b50135` | `a8c6306`; cleanup/phony conflict retains the FPS-auto fixtures **and** the development-kind stamp/`FORCE` rebuild mechanism. Runtime source auto-merges with upstream scheduler/PSO diagnostics. |
| 4 | Final `feat/android-fps-auto` `273db73` | `1ecca4a`; retains the development stamp while adding `displayclocktest`. Uses measured display period across pacing modes, always-on AHB present-fence cadence bins and sustained raw-game-rate evidence for automatic downshift. |
| 5 | `feat/android-framegen` `967851c` | `7d06251`; readiness tracking is the union of predictor, FPS-auto and frame-generation users. Real/synthetic present packets retain both ownership groups; synthetic packets carry no game-frame feedback. Makefile retains all frame-generation/FPS/display fixtures and dev/release rebuilding. |
| 6 | Final `feat/android-framegen` `cbc21b3` | `003a414`; adds bounded dependency-traced G-buffer source selection, opt-in `TOTK_FRAMEGEN_DIAG` and the provenance fixture. Cleanup/phony resolution retains the new `framegendeptest` alongside all earlier fixtures and the dev/release stamp. |

The round-ten defaults below are the historical decision for that integration, not current policy. At that time
`TOTK_FRAME_LAG` was recorded as off after an unexplained shadow report in a lag-plus-slab-driver run; the run did
not establish that lag caused the report. Round 10.1 later changed that choice; today's policy above supersedes it while preserving the
observation and uncertainty. SGSR and automatic frame-rate control remain explicit opt-ins. Device performance and
visual qualification remain separate from the host/native compilation and packaging evidence recorded for this round.

Post-merge corrections retain the topics' intent:

- `8e8b77c`: same-texture reacquisition waits for the old asynchronous replay present to consume its immutable
  FPS-auto observation **before** publishing the new one. Release time is captured before that wait, so intentional
  pacing is excluded without losing real replay/resource delay. The production-boundary fixture extracts the actual
  acquisition/wait/frontend mapping and tests held replay, wraparound, old/new IDs/timestamps and delayed feedback.
- `0450174`: a release APK remains intentionally nondebuggable. Private save/cache exports now preflight `run-as`
  and publish an export only after successful transfer; a denied or partial transfer leaves no bogus backup.
  `APP.md` documents the matching-signed dev-update backup route without uninstalling.
- `4361ecf`: generated presentation packets explicitly clear game-frame feedback/skip ownership; startup-refusal
  fixtures verify incompatible FPS-auto/real-60/uncapped/disabled-slot requests record no generated output.

The new shader-list digest is `f3788503af4c300f3ee55f0cf42f0660c6037c511ff2b19863f1c5b48a502d49`.
The owner must request **`--shaders`**, normally `--runtime-only --shaders --apk` with the existing 1.4.3 work tree.
Plain runtime-only/full builds preserve the old seed. `APP.md` records the complete expected manifest identity
for the pinned reviewed translator (`0619788d…`), unchanged chain `totk-native-v5-unverified`, gather-bias on,
`early_fragment_tests_policy=nvn-control-0x718-v1`, SPIR-V retained, preparation digest `cbf46a4e…` and original
RomFS/SPIRV-Cross inputs. Runtime loading does not check that build-time manifest. No seed regeneration,
translator publication, device operation or shared `build/` write is performed by integration verification.

Pre-follow-up focused host/native acceptance at `4361ecf` uses `.scratch/integ3/round10/`:

| Gate | Result |
|---|---|
| New presentation/pacing fixtures | **175 passed**: dev/release unset/explicit overlay policies, SGSR both directions/valid and invalid controls with independent UNORM/sRGB goldens, display-clock/FPS-auto/scene/block/present-fence fixtures, frame-generation math/slot/Vulkan enabled/off/budget/late cases at recorder 0/2/3, and incompatible-option refusals. Pure/Vulkan fixtures run normal, TSan and ASan/UBSan as applicable; SGSR/overlay pixels run normal/TSan. |
| FPS observation ownership | Normal, TSan and ASan/UBSan production-boundary host regressions pass; no mirror observation map or policy is substituted. |
| Preserved ancillary matrix | **115 passed**: chunk/default-corner/four-buffer controls, inline-16 leases at 2/3/4 workers (including ASan), nine timeline/pacing fixtures, early-fragment tests, companion readers and native JNI gates. |
| Upstream diagnostics/lookahead/audio | Prediction with actual pixels/counters, scan-budget policy, production core-wait and guest-wait fixtures pass normal/TSan. Python translator 4, seeds 15 passed / 7 Mac-Foundation skips, list 11, APK 12, shader dependencies 1, audio 2, services 3, companion gate 3, build-kind 3 and reflection 21 pass; native Python-launched fixtures also run with TSan. Pure Python is not sanitizer instrumentation, and JVM-wide TSan is not claimed. |
| Android source/runtime | All **24 changed C units** and seven additional compile configurations pass `cc1.sh` in their intended translation-unit context; full fresh Android `APP=1` native build/link and normal/TSan/ASan host builds pass. |
| Real APK packaging | Fresh integrated native libraries and freshly compiled **65,364-byte dex**; signed `companion.apk` **35,873,228 bytes**, `legacy.apk` **8,289,478 bytes**. Both pass `apksigner verify`, exact native/dex/assets/entry/CRC/compression checks; existing 2,245 art files / 33,731,700 bytes are reused, not re-extracted. |

Both APKs are development builds under `.scratch/integ3/round10/apk/`; release manifest/build-kind behavior is
separately covered by policy/rebuild/packer fixtures. The unchanged legacy/companion manifest SHA-256 values are
`840945ef9ce27f052eeaca03373500fe2eb96d3283fe2966016d49b040cc86a3` /
`3045fb6d0e7f78f05b085521a84c274fa9c3913210e7eb2aca359c41d5901b19`.
The initial private-export fixture exposed unescaped shell-function braces in the formatted installer template;
escaping them fixes the actual generator, and the corrected/final fixture runs pass. Its temporary build-kind
outputs now honor `TMPDIR`. A standalone headless compile with the app-only macro emitted an undefined-static
warning; the actual headless context (`-UTOTK_ANDROID_APP`) is clean, and the app translation unit includes and
defines that bridge correctly. Original diagnostic logs are retained rather than relabeled as clean passes.

Host frame-generation goldens exercise the actual capture/generation shaders, not the game's full NVN camera/depth/UI
extraction or Android compositor. Source review preserves postprocessing-before-overlay and synthetic source lifetime,
but neither that review nor software Vulkan pixels establish Android AHB behavior, game HUD quality, generated-frame
deadline quality, measured performance or the unresolved protected-lag/slab-driver shadow report. Those remain device
qualification boundaries; the new optional SGSR/FPS-auto/frame-generation modes stay off by default.

The owner supplied `cbc21b3` after the initial full/resource batches had started. Those superseded processes were
stopped deliberately (not test failures); partial logs remain under the original round-ten scratch root. Fresh
current-source binaries, logs and APKs after signed merge `003a414` use **`.scratch/integ3/round10/final/`**:

| Final-head gate | Result |
|---|---|
| Complete Vulkan matrix | **1,250/1,250 passed**, normal + TSan on lavapipe (`LP_NUM_THREADS=0`, `TSAN_OPTIONS=halt_on_error=1`), recorder 0/2/3. Retains the entire 1,034-case protocol and expands AA/scaling composition to off/FSR/SGSR: 576 overlay and 72 post cases. Core/draw/writeback, identity/boundaries/ordered submit, spans/pipelines, uniform/lag, early-Z off/on, profiler/pass merge/clear/occlusion and asynchronous-submit-off legs all pass. |
| Focused presentation/pacing/dependency matrix | **178 passed**; repeats all 175 cases above against the final source and adds `framegendeptest` normal/TSan/ASan. |
| Recorder/VMA/lease/slab resources | **96 passed** normal/TSan/ASan-UBSan, including 0/2/3/4 workers, the complete 30-child reuse/fatal runner, native-pressure cutover, VMA policy/merge, BO compaction and cold/warm registration. Strict ASan/UBSan halt-on-error is enabled from the outset; completed resource sanitizer logs are clean. Inline-16 lease coverage is retained in the preserved matrix. |
| Ownership / preserved / upstream regressions | Ownership normal/TSan/ASan and the **115-case** preserved matrix pass again; all upstream/ancillary Python/native gates pass, now including the provenance Python wrapper normally and with a TSan compiler wrapper. |
| Android / compiler contexts | **25 changed C units + seven configurations** pass prescribed `cc1.sh` with empty diagnostics; fresh full current Android `APP=1` link and host builds have no compiler warnings/errors. |
| Current development APKs | Both freshly packaged/signed/verified: `final/apk/companion.apk` **35,915,762 bytes**, `final/apk/legacy.apk` **8,332,008 bytes**. Exact payload/CRC/compression/manifest checks pass; dex is 65,364 bytes and all existing artwork is reused. These supersede the earlier sizes above. |

Current companion/legacy APK SHA-256 values are
`d7eeb75f6c591fbb666696ffea88cadf9a430c70e392498f801219d16acfd1e1` /
`c85e12b848bb7475f137e10c7c205eddfb78e805b34e17a235c5818ba2c6eb61`.
The current source-provenance fixture tests bounded helper behavior, not the complete game's extraction-to-presenter
path; that distinction remains in the device-qualification boundary above.

Final normal protocol-only cold-guard observations (7,000 inputs / 2,000 units, no Vulkan/GPU work):
two/three shards issue **2,000 mprotect calls**, observing **5.054 / 7.035 ms** pipeline wall time
(critical CPU model **5.029 / 7.000 ms**). Warm repeats issue **zero mprotect calls**, observing
**1.076 / 1.116 ms** pipeline wall time. These overlap other host gates and are not quiet-device performance claims.

The complete final matrix takes 2,793.115 seconds; all final top-level records have zero failures.
`summary.json`, `matrix-results.json`, `recorder-matrix-results.json`, `round10-feature-results.json`,
`extra-matrix-results.json`, `new-feature-results.json`, `android-check-results.json`, `ownership-results.json`
and `apk-comparison.json` retain the exact evidence. Final log inspection finds **no TSan/ASan/UBSan diagnostics**;
no suppression is added. ASan keeps packet-slab poisoning and uses `detect_leaks=0`; both ASan and UBSan halt
on errors from the first final case. The real APK variants share a scratch-generated signing key, not a claim of
using the owner's production update certificate. No device/real-ADB operation, shared build-pipeline invocation,
`build/` write or push is performed.

## Original upstream selective merge (2026-10-24)

Branch `merge/orig-20261024` starts at round-nine `dd366b3` and records reviewed upstream
`5281186` as its merge parent. The reviewed delta is `84fe66a..5281186` (25 commits, including
integration merges); scheduler/cache/budget behavior uses final hardening `6b0c62f`, not the
intermediate contributor patches. Selection intentionally retains Android's recorder, protected-lag,
VMA/slab/lease, pacing, presenter and companion implementations. This is not desktop tree parity.

| Reviewed group | Decision | Android implementation / reason |
|---|---|---|
| `19730cd`, final `6b0c62f`: completed virtual-core waits and safe snapshots | Adapt | Complete counters at core grant, including ready-at-wake reservations; copied owner/held/CPU/wait snapshots are protected by the scheduler lock and logged after unlocking. Disabled collection adds no diagnostic clocks, counters, allocations or snapshot locks. |
| Owner register/PC and CPU sampling | Adapt / omit Mach-only portion | Linux/Android uses live POSIX thread CPU clocks and copied owner handles/TIDs/masks/priority/last-SVC metadata. Unavailable CPU is explicit. No Mach suspend/read/resume or guessed owner PC. Bounded 256-row tables are stack-local. |
| Three-core guest scheduler | Keep ours | Exclusive owners, three core ids, masks, priorities/FIFO, inheritance, starvation handling, SVC/signal preemption and the existing opt-in ready-at-wake policy remain intact. Diagnostics do not relax arbitration. |
| `eafa74e`, `9799eee`, final `6b0c62f`: border-key / ready-function memoization | Keep Vulkan equivalents; skip Metal caches | Vulkan already keys immutable samplers by complete parameters with registry-epoch invalidation, shaders by exact variant/early-bit identity, programs by serial/signature, and pipelines by complete fixed state with acquire-observed readiness. No per-draw MSL border string/function construction exists to cache. |
| `cd7018d`, final `6b0c62f`: adaptive bounded pipeline scanning | Adapt, default off | New same-pass Vulkan lookahead borrows only ready variants/private projected state and the existing exact pipeline map/workers. `TOTK_PSO_PREDICT=1` opts in. Only actual synchronous graphics driver build time earns credit; waits, scans, probes, workers and compute do not. Saturating accounting resets per frame, with independent command/key/time/rescan bounds. |
| Prediction-origin pending demand | Explicit opt-in difference | Demand joins a predicted pending key rather than introducing an extra skipped warm-cache draw. Ordinary demand-origin screen async policy and synchronous offscreen capture rules remain unchanged. No recorder or pacing policy is changed. |
| Renderer/core TSV attribution | Adapt | `@core_wait_self_ms`, `@core_wait_all_ms/n`, and Vulkan `@pso_wait_ms`, `@pso_create_ms`, `@pso_scan_ms` distinguish completed waits and real work. Overlapping core totals are not additive wall-frame time. Android has no runtime translator/Metal library work to invent. |
| `41c20b9`: shared shader contributions | Union | Add 209 V and 3,089 canonical R rows, zero S/C/L; final V/R/S/C = 56,921 / 32,539 / 1,557 / 63. All 56,712 prior V rows and the 29/46-row Android fill cohorts survive. Skip 99 new unsupported Metal B recipes. |
| `9d34bc9`, final `2c489b5`: TOTK end-of-stream policy | Integrate | TOTK resumes a voice on the next frame after EOS; BotW continues within the same frame. Decoder lookahead and returned sample metadata use one policy. Also propagate an empty EOS encountered after partial consumption out of the metadata loop, so it cannot disagree with decoded silence. |
| `f3e3405`: in-process SPIRV-Cross | Skip Apple MSL build optimization | No Vulkan SPIR-V correctness change; upstream adds Mac C++/native MSL-link requirements. Keep the existing Linux translator and external cross-compilation process, gather-bias policy, early-fragment identity and translation chain. |
| `fc1932e`, `2c01789`, `f1043e5`, rendering portions of `2c489b5` | Skip Metal/BotW ownership/writeback mechanisms | Metal texture snapshots/tiny-target completion readback and BotW grass-mask flush reconciliation are not a proven TOTK Android bug. Keep our per-binding protected-lag authority, exact aliases and existing Vulkan invalidation/writeback instead of importing broad CPU snapshots or changing flush semantics. |
| `3ee6720` / framebuffer-fetch hardening | Skip Metal-specific shader rewrite | Metal same-pixel tile attachment reads require a distinct Vulkan feedback/local-read/shader implementation; copying an MSL rewrite would not implement it. No claim that Android has this reported Mac 32x32-decal symptom. |
| `5203aea`: reactive MetalFX TAA mask | Skip | MetalFX camera-depth temporal work is not the Android SMAA/FSR presenter and adds no Android pacing fix. |
| `069a999`: Odyssey shader/combined-handle/cache/audio/preemption repairs | Skip separate-game tree | No TOTK translator correctness hunk exists in this commit; Odyssey Metal runtime indexes/argument reflection and its scheduler are not Android's offline SPIR-V indexes or required TOTK core policy. |
| `8b34743`: visionOS frontend/build/package; `5281186`: BotW Mac resizing | Skip | Apple-only sources/tools and BotW AppKit behavior; retain Android build, launcher/input/presenter and runtime API surface. |
| Earlier `57231de..84fe66a` ancestry | Already covered / excluded | Portable early-fragment correctness is already implemented by round eight's faithful default-on control-byte policy and seed-policy epoch; retain local chain/gather behavior rather than importing the Mac v8 label. Remaining changes are Apple/BotW builds, defaults and rebuilt Mac artifacts. |

Defaults: diagnostics remain opt-in; `TOTK_PSO_PREDICT` is **0**. Within that opt-in mode,
adaptive allowance defaults on, fixed frame allowance is 4,000 µs, each scan is limited to 8,192
records / 32 new keys / 2,000 µs, with 32 rescans after the first per submission. See `APP.md`
for bounds and the pending-demand waiting tradeoff. The historical round-nine decision recorded lag default-off;
round 10.1 superseded that choice before the current Thor-backed default-off policy above. Early-Z remains faithful/default on, and the other recorder/chunk/presenter/
companion controls remain as documented for that round.
The intentional existing-behavior correction is TOTK audio EOS stopping for the remainder of a frame.
No device performance claim is made.

**Regenerate the shader seed before device deployment.** The shared-list digest changes seed identity;
keep SPIR-V and derive the complete `.vk`/`.vv` metadata set. No translator republish or chain bump is
needed. Shared R rows remain irrelevant to Android's Vulkan prewarm, and the local fill retention
contract is unchanged. APK verification reuses existing artwork and does not itself regenerate that seed.
Implementation/verification artifacts and the complete taken/skipped report are in `.scratch/upstream8/`.

Verification on the merged sources passes: full **1,034-case** round-nine normal/TSan matrix,
**96 resource cases** (28 normal / 28 TSan / 40 strict ASan+UBSan), and **115 ancillary cases**
including nine inline-16 lease cases, chunk/default-overlay/pacing/faithful-early-Z fixtures,
companion readers and native-only JNI TSan gates. The new prediction/policy and production scheduler
diagnostic fixtures pass normally/TSan; Python translator/list/seed/APK/dependency/audio/service/
reflection and ordinary JVM companion gates pass, with only seven Mac-Foundation seed skips.
All **16 cc1 configurations** have empty logs. Native APP=1 runtime and all fresh host builds are
warning/error-free; sanitizer log inspection finds no TSan/ASan/UBSan reports.

Both fresh APKs pass signature, bytewise current-native/dex/assets, entry/CRC/compression and
legacy-manifest checks: `.scratch/upstream8/apk/legacy.apk` is **8,179,278 bytes**;
`companion.apk` is **35,763,034 bytes**, with a newly compiled 65,364-byte dex and the same
2,245 artwork files / 33,731,700 bytes. Legacy/companion manifest hashes remain
`840945ef9ce27f052eeaca03373500fe2eb96d3283fe2966016d49b040cc86a3` /
`3045fb6d0e7f78f05b085521a84c274fa9c3913210e7eb2aca359c41d5901b19`.
An initial clock-fixture-only IPC constructor link failure was repaired without a production fallback.
The orchestration deadline expired only after the full main matrix and 45 exit-zero resource cases;
remaining cases resumed with separate logs/checkpoints. Initial attempts are retained rather than
called passes. No device operation or shared build output write was used.

## Round-nine integration

`integ/round9` starts at signed round-eight head `2d61732`. Signed merge `0cf489c` integrates original
`fix/android-recorder-job-race` head `3359bfd` without textual conflicts. Its `1ce8ae0` base is already present
through round eight, so no reconstructed topic or duplicate pacing implementation is introduced.
The merge includes dirty-map/VMA classification `893434c`, recorder packet/descriptor slabs `78aa011`,
warm retained capacity/arena BO compaction/live VMA headroom/capture-off cutover `1e6daab`, and cross-slot
recording-owner command-buffer leases `3359bfd` (plus their original `63fa904` safety ancestry).

Signed follow-up merge `1558703` brings final topic `5ae720e` (`perf(android): preserve warm recorder leases
and packet slabs`), also without textual conflicts. Returned lease chains now splice once per retired slot and
drain on the recording owner's next batch, avoiding return-only worker wakeups. Cold maintenance preserves warm
demand, ages from last acquisition and requires long-idle excess; packet pressure pruning releases only genuinely
idle/unreferenced arrays/slabs rather than destroying an over-budget scene every ring wrap. Detailed timing/count
semantics and native-gated warm/long-idle regressions remain in `VULKAN.md`.

Recorder packet pools remain walking/slot-owned; workers consume frozen data. Secondary command pools and free
lists remain recording-owner-owned across slots; retirement publishes lease returns only after GPU/post completion
and submit-owner release. Active leases are not capped or reset while unsubmitted. Pressure cutover drains real
queued work at a game-submission boundary before releasing captured storage, and restores ordinary frame-end waits;
it does not fabricate completion. The round-eight record used chunk 600 with lag/75 otherwise and recorded
`TOTK_FRAME_LAG` default-off pending the then-separate VMA-safety measurement. Those are historical defaults,
superseded first by round 10.1 and then by the current policy above; host gates did not promote the earlier setting.

Owner-reported Thor observations distinguish the two memory effects: runtime heap/lease fixes reduce mappings
from the prior crash region (**65.7k**) to about **50k** with lag enabled. Separately, the Turnip command-stream
slab candidate reduces mappings by about **13k**, with **no FPS cost seen**. Do not infer an unmeasured additive
combined total or generalize the Thor observation to Odin or all scenes.

The driver candidate is outside the game checkout at `turnip/packages/cs-slabs-20261024`, built from
`totk/cs-slabs-20261024` commit `adc573c4c7a4a975b8127dceca62a263aef5a1c3`.
The deployable `release/driver.so` SHA-256 is
`6017dad5ea50b4afc33189604b5e20b0651e414a5c3a56ba2dc6fdd75fb93876`.
Close the app, preserve its prior driver, copy that ELF to
`/sdcard/Android/data/dev.kitcatkode.totk/files/driver.so`, then restart so the frontend refreshes its private
executable copy. No APK rebuild is required. `TU_DEBUG=noslabcs` in `totk.env`, followed by restart, is the
same-binary control; remove that token for slabs on, retaining any other comma-separated flags. See `APP.md`
for deployment/rollback and A/B controls. The package's original build-only validation record predates these
later owner device observations. This integration neither deploys the candidate nor contacts a device.

The final integrated Android `APP=1` native runtime was built and linked with `OUT` under
`.scratch/integ3/round9/native-current`; generated headers and codec archives are read-only inputs.
All **10 changed C translation units** pass `cc1.sh`; three test-hook variants and the recorder ASan-poison
compile configuration also pass (14 empty diagnostic logs). Normal/TSan/ASan host builds and the Android native
build emit no compiler warnings/errors.

Both APK variants use those freshly linked current native libraries and the newly compiled 62,332-byte dex,
not an earlier native payload. `apksigner verify`, exact entry/payload/CRC comparisons and asset compression checks pass:

| Artifact under `.scratch/integ3/round9/apk/` | Bytes | Activity / contents |
|---|---|---|
| `companion.apk` | 35,744,037 | `TotkActivity`, current dex, 2,245 generated asset files / 33,731,700 bytes |
| `legacy.apk` | 8,162,319 | framework `NativeActivity`, no dex/assets, identical native payload |

The legacy binary manifest remains byte-identical to round eight for identical packer options, SHA-256
`840945ef9ce27f052eeaca03373500fe2eb96d3283fe2966016d49b040cc86a3`; the companion manifest remains
`3045fb6d0e7f78f05b085521a84c274fa9c3913210e7eb2aca359c41d5901b19`. Existing generated artwork is reused,
not re-extracted. No `build_android.py`, device/ADB operation, `build/` write or push occurs in this integration.

The main final-topic matrix passes **1,034/1,034 cases** across normal and TSan builds on lavapipe
(`LP_NUM_THREADS=0`, `TSAN_OPTIONS=halt_on_error=1`, no suppressions), at recorder 0/2/3:
core/draw/writeback, lag wait/copy/sticky, rename off/on, early-Z off/on, profiler 1/2, pass merge/clear/occlusion,
pipelines/spans, native/widened identity and boundaries/ordered submit, asynchronous-submit-off, 384 overlay
combinations and 48 post combinations. The identity legs are lag-off/rename-off, protected-lag/rename-off and
rename-on/lag-off from the outset; real-backed lag+rename remains covered by core/draw.
The authoritative record is `.scratch/integ3/round9/matrix-results.json`.

Final-topic resource acceptance initially records **96 exit-zero top-level cases** across normal, TSan and
ASan/UBSan builds: the 0/2/3/4-worker reuse runner (24 graphics children plus six expected-fatal injections per build),
VMA guard/policy/merge cases, arena BO compaction, synchronous/profile-enabled pressure cutover and additional
four-worker/core ASan coverage. Nine more inline-16 lease cases exit zero across normal/TSan/ASan at 2/3/4 workers.
Final log inspection found inherited fixture-only undefined behavior: both core/draw operation builders passed
`NULL` to `memcpy` with zero length for the empty `OP_END` payload. Signed fixture correction `d682949` skips
that zero-byte copy in both helpers; the recorded stream is unchanged. Original logs are retained rather than
presented as clean UBSan acceptance. Rebuilt normal/TSan fixtures pass **52 additional cases**: core, draw,
spans and pipelines at recorder 0/2/3, lag off/on, plus both identity legs (including boundaries/ordered submit).
The two changed fixtures also pass `cc1.sh` again after the correction.
Rebuilt ASan/UBSan binaries pass **43 strict cases**: all 40 original ASan resource/core cases plus the three
inline-16 lease cases, with `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` as well as ASan halt-on-error.
This includes the complete child/fatal-injection reuse runner, not a narrowed substitute. Final strict and
post-correction sanity logs contain no UBSan, ASan or TSan diagnostics. Records are
`strict-sanitizer-results.json` and `final-fixture-sanity-results.json` under the round-nine scratch directory.
ASan retains packet-slab poisoning and uses `detect_leaks=0`; injected failures require the expected SIGABRT
and operation-specific diagnostic, not successful continuation.

The preserved round-eight controls also pass on the final topic: 48 chunk-policy cases, 24 default-corner/four-buffer
overlay cases, synthetic overlay power fixtures, all nine standalone timeline/pacing fixtures, faithful early-fragment
tests, guest-wait/ready-at-wake probes and thread-placement checks, normally and with TSan. Python suites pass:
gather-bias translator (4), shader seeds (15 passed / 7 Mac-only Foundation importer skips), shader list (11),
APK (10), shader dependencies (1), audio (1), services (3), and reflection (21). Native fixture compilation in the
TSan leg is instrumented; pure Python execution itself is not sanitizer coverage. Companion readers pass
ASan/UBSan and TSan/UBSan, the three ordinary JVM/native lifecycle gates pass, and native-only JNI gate tests
pass TSan. No JVM-wide TSan claim or suppression is introduced.

Observed normal host results (not Android performance predictions):

| Workers | Warm over-budget packet/info slabs | Referenced capacity / allowance | Warm evictions / cold resets | Long-idle retained / reclaimed secondaries |
|---|---|---|---|---|
| 2 | 40 / 8 MiB | 39.8 / 16 MiB | 0 / 0 | 1,024 / 3,080 |
| 3 | 40 / 8 MiB | 39.8 / 16 MiB | 0 / 0 | 1,536 / 2,560 |
| 4 | 40 / 8 MiB | 39.8 / 16 MiB | 0 / 0 | 3,048 / 1,056 |

The 1,200-pass scene deliberately exceeds the idle-pruning allowance while its data remains referenced.
The independent inline-16 case retains 768 handles at both warm/final points, with zero cold resets; measured walking
acquire/return is 1,423/268, 1,688/272 and 1,194/242 ns/frame at 2/3/4 workers. These diagnostic host runs overlap
other host verification and are not quiet-device A/B timings. Cold all-unit guard benchmarks retain 7,000 inputs,
2,000 unique units and **2,000 mprotect calls**: two/three shards observe 6.739/7.072 ms pipeline wall time
(modeled critical CPU 6.684/6.987 ms); warm repeats use **zero mprotect calls**, 1.102/1.094 ms pipeline wall.
They are protocol-only CPU measurements, not Vulkan/GPU frame time.

## Round-eight integration

`integ/round8` starts at signed round-seven head `eb7428c`. The owner device-accepted that head on Thor
(copy + overlay) and Odin 3 (AHB + overlay), and separately verified AHB/background-resume on both devices.
Those are owner observations; this integration performs no device/ADB operations.

| Order | Original topic head | Signed merge / resolution |
|---|---|---|
| 1 | `perf/android-frame-timeline` `65fd24a` | `8000b3b`; clean. Includes `63da9c1` walk-variance accounting, frame-fence provenance, slot pacing default-on for both presenters, opt-in prediction, API33 SurfaceControl JNI bridge and stale-input fix. |
| 2 | `feat/android-perf-overlay` `5ac0f27` | `db1d36b`; retains the existing per-frame golden-pixel/padding/AA/scale checks instead of reviving the old single-frame weak panel scan. Updates their independent corner expectation to the new top-right default; retains power-source fixtures and fourth staging slot coverage. |
| 3 | `feat/android-dual-screen` `2bdf28b` | `2bd5f7d`; cleanup/phony conflict keeps all timeline/pacing targets **and** companion target. Runtime list auto-merges correctly: frame timeline/fence plus `APP=1` companion snapshot/JNI sources all remain. |

The round-eight historical defaults requested Android AHB-by-default with existing automatic copy fallback, and
chunk threshold 600 with `TOTK_FRAME_LAG=1`, otherwise 75 (an explicit `TOTK_VK_CHUNK_DRAWS` won, including 0).
The historical lag decision was **off** pending native VMA safety work. Round 10.1 later enabled shipping-30 lag;
the current Thor-backed policy above supersedes it with lag off in every mode. Chunk still defaults to 600 with
resolved lag and no native VMA pressure, otherwise 75 (now the default); explicit settings win. AHB selection agrees in backend capability
selection, frontend SurfaceControl setup and predictor startup. Recorder, seed, optional AA/scaling and prediction
defaults remain as their topic branches define them.

The companion is an optional `NativeActivity` subclass, not a replacement native runtime. Its `JNI_OnLoad`
registers the app class in the `System.loadLibrary` class-loader context. The SurfaceControl bridge obtains the
actual activity class with `GetObjectClass`, resolving inherited framework methods; its `FindClass` calls name only
framework classes. It uses the activity's JavaVM/environment and does not add a second `JNI_OnLoad`.
The launcher is resolved through the LAUNCHER intent, so both activity names are supported by the owner's scripts.

`TOTK_COMPANION=0` in the already-loaded `totk.env` disables controller/display-listener construction,
Presentation, asset worker and poller startup, direct native snapshots **and** action injection. The query runs
after `NativeActivity.super.onCreate` and never caches an earlier option value in `JNI_OnLoad`. Default-on behavior
and kind-1 button pulses remain unchanged. Reads use `host_safe_read` for GameData/player state; only explicit
`− Map` / `+ Menu` touch releases call `SnapshotPoller.action(1,11/10)` → `DataSource.send` →
`CompanionNative.action`. They synthesize normal Switch button presses/releases and never write game memory.

Actual scratch APK packaging uses a freshly linked **current integrated Android runtime**, built with the Android
Makefile's `APP=1` target and `OUT=.scratch/integ3/round8/native-current` (generated headers and codec archives read
only; no `build/` writes). Current Java sources compile with JDK17 / Android API36 d8, min API33. Both signed APKs
pass `apksigner verify` and complete entry/byte/compression/CRC comparisons:

| Variant | Activity / content | Exercised artifact |
|---|---|---|
| Companion | `TotkActivity`, `hasCode=true`, current 62,332-byte dex plus 2,245 generated art files / 33,731,700 uncompressed bytes | `.scratch/integ3/round8/apk/companion.apk`, 35,697,529 bytes |
| Java-free (`--no-companion` equivalent: no `--dex/--assets` in `build_apk.py`) | framework `NativeActivity`, `hasCode=false`, no dex/assets; identical native libraries | `.scratch/integ3/round8/apk/legacy.apk`, 8,115,821 bytes |

The Java-free binary manifest is byte-identical to `eb7428c` for identical packer options (SHA-256
`840945ef9ce27f052eeaca03373500fe2eb96d3283fe2966016d49b040cc86a3`). Companion binary manifest SHA-256 is
`3045fb6d0e7f78f05b085521a84c274fa9c3913210e7eb2aca359c41d5901b19`. Art was reused from the existing generated
`.scratch/ds-assets` tree, not re-extracted from RomFS. Declared image/decoder/crypto versions were imported successfully
with scratch-local Pillow **12.3.0**, texture2ddecoder **1.0.6**, cryptography **50.0.2**; the APK suite passes with
the pinned Pillow. Java-free and companion packaging exercise the real current dex/native payloads, not placeholders.

Native companion reader tests pass ASan/UBSan and TSan/UBSan. The JVM lifecycle/class-loader suite passes normally;
the native JNI-entrypoint driver also passes TSan with six option cases and concurrent pulse coalescing/release.
An exploratory JVM-wide TSan preload reported races inside uninstrumented HotSpot GC/JIT code, not a green JVM
sanitizer run. No suppression was added. Native sanitization deliberately uses the standalone entrypoint driver;
ordinary JVM tests separately retain real class registration and activity/poller ordering coverage.

Runtime switch/defaults and their host regressions are signed checkpoint `e29811d`. Final host acceptance:

| Gate | Result |
|---|---|
| Full Vulkan matrix | **1,034 passed**, normal and TSan, lavapipe `LP_NUM_THREADS=0`, `halt_on_error=1`, recorder 0/2/3. Core/draw/writeback, lag wait/copy/sticky, rename off/on, early-Z off/on, profiler 1/2, pass merge/clear interaction, pipelines/spans, indexed/widened identity/boundaries/ordered submit, async-submit-off, overlay and post paths all exercised. |
| Conditional chunk policy | **48 passed**, normal/TSan × recorder 0/2/3 × lag off/on × default/75/600/0. A real 800-draw / 200-pass stream observes **11 slots at 75**, **2 at 600**, one when disabled, with independent draw count and textured pixels. |
| Presenter additions | **24** extra normal/TSan runs cover actual unset/empty top-right overlay default, fourth present buffer and AA/scale combinations; synthetic power-source tests pass both builds. The main matrix includes 384 overlay and 48 post combinations. |
| Timeline/pacing | All nine standalone timeline/fence/acquire/display/slack/predict/budget/present-fence/present-slot fixtures pass normal/TSan, including disabled timeline/fence modes. |
| Android sources | **30 changed C translation units** pass prescribed `cc1.sh` checks without diagnostics; the JNI native-driver compile mode also passes. Full current Android `APP=1` native build/link and both host Vulkan builds have no compiler warnings/errors. |
| Ancillary | Gather-bias translator (4), seed (15 passed / 7 Mac-only Foundation importer skips), shader-list (11), APK (10), shader dependencies (1), audio (1), services (3), reflection (21), guest-wait and standalone faithful early-fragment tests pass; native suites also run with TSan. Pure Python tests are not themselves sanitizer instrumentation. |
| Companion | Reader ASan/UBSan + TSan/UBSan, three normal host-JVM/native gate tests, and native-only TSan gate test pass. Every gate value is exercised in a fresh process; JVM-wide TSan is **not** claimed green, as explained above. |

The original identity protocol has three independent legs: lag off/rename off; protected lag/rename off; rename
on/**lag off**. The reconstructed first batch accidentally enabled lag in the rename identity leg, causing 16 wrapper
failures when that boundary fixture's deliberately `PROT_NONE`, metadata-only alias was CPU-captured. Debugger
evidence identifies that artificial unreadable alias, not a companion or pacing failure. Restoring the exact
round-seven protocol makes all 16 corrected runs pass; real-backed lag+rename remains in the core/draw matrix.
Both initial records and corrections are retained, with final authority in
`.scratch/integ3/round8/matrix-final-results.json`; no production workaround, invented bytes, skipped assertion or
sanitizer suppression was introduced.

All evidence, freshly linked native libraries, compiled dex and both APK variants remain under
`.scratch/integ3/round8/`. No `build_android.py`, device/ADB operation or push was performed, and no files under
`build/` were written. Android ART/dual-display lifecycle, actual live companion state/input and new combined-device
performance still require owner acceptance. AHB's prior owner validation is not evidence for the new companion UI.


## Round-seven integration

`integ/round7` starts at the cross-frame lag/sharing checkpoint `3890b18`. Original branch ancestry and defaults
are preserved; no reconstructed topics or device runs are part of this integration.

| Requested step | Original head | Signed merge checkpoint / resolution |
|---|---|---|
| Thread placement / ordered asynchronous submit | `693465a` | `cec43b2`; clean textual merge, plus the new producer-journal diagnostic uses the walking reserved ticket rather than concurrently published `g_fence`. |
| Opt-in guest-wait attribution / ready-at-wake cores | `c6485bf` | `45a84ba`; clean. |
| Faithful early-fragment default and seed-policy epoch | `b7d11fc` | `125cd52`; clean, retains gather-bias and original-early shader identity. |
| Native presenter performance overlay | `4d91e9c` | `2a4f25c`; Makefile cleanup/phony lists retain both thread-placement and overlay host targets. |
| Launcher icon / game label extraction | `50bc462` | `0c4a65b`; clean. |
| Explicit-LINEAR dma-buf AHB import | `390c358` | `e77263f`; keeps explicit modifier/stride/import validation **and** overlay `TRANSFER_DST` usage, with the release barrier and final overlay composition retained. |
| Opt-in SMAA / FSR1 presentation | `821822a` | Already an ancestor of `390c358` via `070c801`; included by `e77263f`, not a fake extra merge. Main explicitly approved authentic ancestry. |
| October 23 upstream seed union / audio splitter fixes | `52b0643` | `e6ae6d5`; seed epoch rejection and dependency-equivalent dedup tests are independent additions and both remain. |
| Reviewed upstream watch catalog / tool | `1ddb21b` | `f1af5bf`; clean, tools only. |

Both Vulkan source lists and host targets retain overlay **and** post-processing modules. In both copy and AHB
presenters, AA/scaling executes before the output overlay; nothing filters or overwrites the panel afterward.
The LINEAR AHB image-format query includes its actual combined color/transfer usage instead of retaining obsolete
AHB-format properties from the replaced import path. Host coverage cannot establish Android gralloc/compositor behavior.

`TOTK_VK_CHUNK_DRAWS` remains **75**. Faithful original early-fragment tests are now **default-on** as defined by their
branch; SMAA/FSR, overlay, guest SVC profiling, ready-at-wake cores and thread placement retain their branch-defined
opt-in controls. Async submit retains its branch's startup policy and software-lag synchronous fallback.
Private snapshot copies retain the game-submission arena lifetime and exact content authority through sealed slots.

The seed epoch is checked by `prepare_shader_seed.py`. Plain `build_android.py --runtime-only` deliberately **keeps
the current seed**: its shader preparation/copy step executes only with explicit `--shaders`. Thus runtime-only can
retain old shader semantics despite the new runtime default; `--runtime-only --shaders` or the next full shader build
evaluates the new epoch. No build script was executed to infer this policy.

The new UBO safety coordinator now waits for the genuine queued watermark from `vk_record_test_submit_status`,
not `vk_submit_serial`: asynchronous integration changes the latter into a walking seal generation. Guard/CPU-reuse
tests must never open a GPU gate merely because a slot was reserved or sealed.

Round-seven host acceptance (lavapipe, `LP_NUM_THREADS=0`, TSan `halt_on_error=1`):

| Gate | Exercised result |
|---|---|
| Vulkan / recorder / presenter matrix | **1,034 invocations passed**, normal and TSan, `REC_THREADS=0/2/3`; core, draw/writeback, identity and boundary identity, indexed/native/widened spans, pipelines, lag uniform/reuse and walking fixtures, profiler 1/2 and pass merge, explicit early-Z off/on and default-on probes. Async-submit-off A/Bs also passed. |
| Overlay composition | Included above: **384** normal/TSan combinations of recorder counts, actual modes `off/fps/full/1`, all four positions, SMAA off/on and FSR off/on. All seven ring-reuse frames check untouched green output/padding and independent golden glyph/panel pixels, not merely a changed pixel. |
| Post presentation | Included above: **48** normal/TSan combinations of recorder counts, sRGB off/on, SMAA off/on and FSR off/on. Thread-placement fixture also passed normal/TSan. |
| Android syntax | **27 changed C translation units**, prescribed `cc1.sh` checks, no diagnostics. Host Vulkan builds also emitted no compiler warnings/errors. |
| Python / native ancillary fixtures | Normal and TSan-injected native compilation passed: gather-bias translator (4), seed (15 passed, 7 Mac-only Foundation importer cases skipped), shader-list (11), APK/icon (6), shader dependencies (1), audio routing (1), services (3), reflection (21), standalone early-fragment, display and guest-wait suites. Pure Python cases are not themselves TSan instrumentation. |
| Seed translator identity | Committed prebuilt SHA-256 matches `0619788dcc93bc89bfd2ca8a15a23efdc2097b2b42361678dab6b433cfaf46f3`; no translator rebuild performed. |
| Watch tool | Local help and 16-project catalog parse passed; no network fetch or catalog cursor update. |

The guest event wake fixture now attributes its unchanged `<1000` context-switch bound to the 17 precisely tracked
waiters on Linux, not the whole process: TSan helpers and the deliberately conservative six-handle fallback waiter
are reported separately. The signal storm yields briefly every 16 signals so an incorrect wake-all cannot pass by
coalescing the whole storm. Production wake behavior is unchanged. Positive normal/TSan ready-core-off/on tracked
counts were **46 / 30 / 17 / 52**; scratch-only wake-all negative controls were **11,643 / 21,865 / 25,476 / 1,955**,
all correctly rejected. The original process-wide TSan attempt failed its attribution bound, not a race; this
regression fix supersedes it. Initial APK tests lacked `cryptography`; final runs use scratch-local dependencies
and pass without changing the system environment.

Evidence is under `.scratch/integ3/round7/` outside the checkout. No Android build, device/ADB operation or push was
performed. AHB native-handle import, compositor ownership/release, real battery display, and device performance remain
device acceptance work; host pixels and syntax checks do not establish those behaviors.

## Original upstream review (2026-10-23)

Reviewed the complete `a988b5e..orig/main` interval (`orig/main` = `57231de`) against Android base
`integ/round6` at `858924a`, in branch `merge/orig-20261023`. The former `kitcatkode/MacNativeGames`
remote is unavailable; its last local Android-port state remains `ba38858`. This is a selective integration,
not a merge of the desktop/iOS defaults. Full per-file classification, counts, check logs and the device A/B
plan are in `.scratch/upstream7/report.md` outside the checkout.

| Upstream change | Decision | Android reasoning / checkpoint |
| --- | --- | --- |
| Shared shader-list render-key normalization (`be8c0f8`) | Adapt | `2782bb1`: retain the Android v5/no-border-recipe schema; canonicalize unused R state and union warm flags. The Android seed imports this utility. |
| TOTK shader contributions | Union | `00cb243`: 897 new V identities and 23,579 new canonical R identities; S/C add zero. All 29 Android fill V rows from `a20ef05` survive. |
| Wildcard dependencies in six newly contributed V rows | Adapt | `6ca624e`: Vulkan accepts `*` only for non-depth textures and `*_depth` only for depth textures; concrete dimensions remain exact. This is required to use those rows, not a translator-policy migration. |
| Audio splitter ordering, #226 (`7b4c90b`) | Integrate | `10dc3e5`: the Android Makefile compiles this common mixer; order sources before every active splitter receiver. Import the PCM probe, enabling its existing portable backend on Linux. |
| Chinese locale script/region selection in `services.c` | Integrate | `fb6959c`: Android's `TOTK_LOCALE` path was always Simplified Chinese; explicit Hant and scriptless TW/HK/MO now select Traditional Chinese. Explicit `TOTK_LANG` still wins. |
| Successful allocation logging in `services.c` | Integrate | `cce4150`: unguarded platform-common observers now log failures only unless `TOTK_TRACE_ALLOC=1`; cache the switch atomically, following existing SVC tracing. |
| Default scene resolution in `game_patches_v143.c` | **Not adopted** | The upstream non-iOS branch also catches Android and would replace its game-selected handheld scene (1280x720 base) with 1920x1080/canvas-sized rendering. Keep Android's unset/0 behavior and all opt-in render reductions; the requested 30-fps target must not inherit desktop GPU/heap costs. |
| Picture-mode persistence in `game_patches_v143.c` | Mac-only usage; unchanged | The Mac launcher supplies `TOTK_PICTURE_MODE`; Android does not. Android's MetalFX/key-3 controls are stubs, so importing those helpers is not an Android picture-mode implementation. |
| iPhone aspect/FPS defaults and first-world warm wait in `hle_nvn.c` | iOS/Mac-only effect; unchanged | The 30-fps switch is `TARGET_OS_IPHONE`-gated. The non-iOS unlimited Metal warm wait would compile on Android, but Android `mtl_warm_wait`/`mtl_note_world` are no-ops. Preserve Android pacing. |
| `romfs.c`, `game_version.h`, BotW shader keys in `nvn_obj.h` | BotW-only; unchanged | `GAME_BOTW` / BotW version branches do not enter the TOTK 1.4.3 Android build; its `romfs.c` overlay behavior is otherwise identical. |
| `runtime.h`, Switch-left fallback and Apple gamepad/frontends | Mac/iOS-only; unchanged | The new declaration serves Apple GameController/IOHID code, not Android input; none of those `.m` files are selected by `android/Makefile`. |
| Metal renderer, pipeline archives, MetalFX, TAA, shader runtime/seed and overlay changes | Mac/iOS-only; unchanged | Android selects Vulkan `.c` sources, excludes `fps_overlay.c`, and never compiles the affected Objective-C implementations/headers. Border-emulation B rows remain outside its supported list schema. |
| `prepare_shader_seed.py` interval change | BotW-only; unchanged | Only BotW's `all_default` changes. Android restricts its build to TOTK and invokes the default TOTK seed path. |
| First-launch verifier, button icons and Apple build/packaging changes | Mac/iOS usage; unchanged | The verifier launches a Mac `.app`; icons serve Mac keyboard bindings and are not invoked by the Android build. Android imports `build_app.py`, but none of the modified app installation/localization/Metal-warm helpers are called. |
| `games/totk/tools/*`, Android build files, shared NVN C beyond `hle_nvn.c` | No interval change | No hidden translator/backend/build-file migration is included. The new root Makefile is an optional host source gate, not Android's build entry point, and is not adopted. Odyssey/BotW/other-game additions remain excluded. |

The delivered list has V/R/S/C = **56,712 / 29,450 / 1,557 / 63**. Normalization collapses 3,617
baseline R duplicates, so compare additions against 5,871 canonical baseline R rows, not the 9,488 raw
rows. Android seed preparation consumes V/locator rows, not shared R rows; its Vulkan prewarm is the
device/driver-bound `vk-prewarm.bin`, not Metal R descriptors. The extra R rows therefore do not imply
extra Android pipeline prewarming.

Explicit host checks pass: audio routing 1/1 (UBSan), shader list 11/11, services 3/3, Vulkan dependency
matcher 1/1; shader seed runs 20 tests with 13 passes and 7 Mac-Foundation-only skips. Android cc1
passes with empty logs for `audio_mixer.c`, `services.c`, `vk_shader.c` and their three C probes.
No full Android build, actual-game seed regeneration, device access or performance claim was made.
Device gates: regenerate the seed and compare shader requests/draw skips on identical routes; compare
splitter-heavy audio/listening and underruns; compare locale startup with Hant/Hans/script/region
overrides; compare allocations/streaming frame times with tracing unset/0/1 while retaining failures.
Keep each named checkpoint independently reversible; test wildcard support with the unioned seed.

### Seed regeneration accounting (2026-10-23)

A subsequent parent-run build of `398fc61` regenerated 24,748 variants and removed 541 former
device-fill filenames (1,082 `.spv`/`.vk` files), adding seven variants/two shader-key lists. This is not
541 lost shader implementations: 473 removed SPIR-V files have byte-identical current same-key
representatives; the other 68 match the **old** bytes of same-key names that survived and were
retranslated with the new policy. No cross-key substitute is used. All 84 failed translations are
unrelated startup-default jobs, not listed/fill jobs. The full 541-name lineage map and MD5 evidence
are retained in `.scratch/upstream7/identity-lineage.tsv` and the acceptance report.

Device fills hash all bound `attr/fout/img/tex` answers; shared V rows describe only the translator's
projected dependencies. Seed preparation intentionally deduplicates by those dependencies, so a
default/listed representative can replace many full-input filenames. Of the original full answer
sets, 45 are recoverable from surviving metadata and all 45 match a current `.vv` representative.
The remaining 496 original requests/old reflection texts were not retained locally; their conclusion
rests on same-key binary lineage, not an invented exact request replay. The fills were real, formerly
`.vv`-listed entries, not “stale orphans”; a manifest taken before the fills cannot establish orphaning.

No stale-binary preservation, unsafe default fallback or generator behavior change is needed.
A focused regression covers both dedup paths and genuine differing dependencies, including complete
SPIR-V/reflection/manifest membership. Shader-seed tests now run 21: 14 pass, seven Mac-only skips.
No additional seed/APK rebuild is required for this test/docs change; the parent's regenerated
artifacts already contain the same-key representatives. Old exact-name Vulkan prewarm records can
still miss those renamed fills, so relearn prewarm under the new seed for comparable warm runs.

## Round-six integration

`integ/round6` starts at the unit-batched protected-lag head `c3c9a61`. Merge order is recorder iteration six
`d772381`/`f29ed74`, opt-in game rendering reductions `8fd7002`, then opt-in early fragment tests/LRZ attribution
`a9d405a`/`c016c5d`. Each merge is retained as a separate A/B checkpoint; defaults remain unchanged.

Recorder-six conflicts combine descriptor templates/four-way cache buckets with per-ticket/frozen-span guard
collection identity, rather than losing guards on cache hits. Merged clear/timestamp events retain batched late guard
registration and ordered-primary assembly. Vertex/index bind deduplication operates on selected immutable overrides;
a private vertex override invalidates the raw live-resolution memo so an override-to-live transition cannot reuse
the wrong buffer. Individual command-buffer reset stays behind the existing slot-completion lifetime barrier.

The game-patch merge is clean and keeps every new switch opt-in. Render-patch production guards and ARM64 veneer
semantics pass the host verifier, including authentication against the local original 1.4.3 `main` NSO. DRS
transformation/serving and refusal tests pass normally and under TSan against the authenticated local RomFS, writing
fixtures only under scratch. The tests require Python 3.14's zstd support and scratch-installed Unicorn/lz4 for the
instruction verifier; initial missing-dependency attempts are not passes. Both runtime C files and both C fixtures
pass Android cc1. The supplied standalone 200 m far-clamp sweep's draw/fps improvement is documented separately from
the still-unmeasured combined protected-lag build.

Early-Z conflicts retain both gather-bias translation policy and original early-fragment metadata in service jobs.
Shader caching uses the expanded `(variant, original-early-bit)` key by address while retaining hitch load/byte
counters. This checkpoint used default-off; guest early-fragment semantics are now default-on (see VULKAN.md), with
explicit `TOTK_VK_EARLYZ=0` retained only for diagnosis. The standalone pixel fixture's local `clone` helper is renamed
to avoid Android bionic's public `clone` declaration.

Final early-Z checkpoint gates pass normally and with TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) for EARLYZ=0/1:
core suites at threads 0/2/3, rename 0/1, lag off/wait/copy/sticky-only; pipeline/indexed/linear spans; native/widened
identity and boundary streams at all four chunk/inline/lead settings; seven GPU profiler/pass-merge configurations;
and the private uniform fixture including explicit OFF=rename. Real Vulkan early/late side-effect, discard and
depth readback fixtures pass normal/TSan. Their unchanged Python launcher is mirrored beneath integration scratch
to respect its enforced local-scratch layout without writing into the checkout's default scratch directory.
The combined gather/early translator compiles with .NET 10 and redirected artifacts; all ten round-six C files pass
Android cc1 with empty final logs. Gather 4/4, reflection 21/21 and display normal/TSan pass. Evidence:
`.scratch/integ3/round6-recorder/` and `.scratch/integ3/round6-earlyz/`; initial SDK-shim and Android helper-name
failures are superseded by explicit-SDK/portable-helper passes. No combined device/GPU-speedup claim is made.

### Walking-side protected-lag follow-up

Supplied `c3c9a61` REC_PROFILE=1 device runs isolate the remaining cost before registration. Thor Lookout walking
29.1→39.1 ms includes draw-touch 6.2→12.2, vertex/index 1.7→8.5, resolution 3.9→6.5 and snapshots 0→2.1 ms;
Odin Gerudo walking 12.2→16.6 includes touch 2.05→4.70, vertex/index 0.51→2.51 and snapshots 0→0.80 ms.
Registration itself is about 1 ms. Logs are `.scratch/bench/{t,o}-r5e-prof{0,1}.log`; these observations supersede
an assumption that late arming alone accounted for the frame-time gap.

Cold/hot **classification** may remain conservative for an actual chunk: cold stays guarded and hot keeps copying.
Reuse of a native-backed private copy is a stronger claim and must also validate CPU dirty writes, inline/GPU changes,
mapping incarnation and actual ticket, including physical writers through virtual aliases. Explicit submit snapshots
instead own immutable old-frame bytes: later guest reuse does not revoke their base or CPU-known logical overlays.
An unchanged writable binding is not unchanged contents. Writable producer discovery still precedes every readonly
capture and the no-pending-update early return; mandatory broad-UBO alias materialization happens before rendering.

The follow-up keeps exact walking-side binding/negative-overlap memos, watched native-copy authority and owned
snapshot authority separate. Physical and every known alias are write-watched before a native capture, then checked
against the pre-watch dirty token. Unwatchable/unstable native copies and untracked indirect-pointer parents disable
outer stage freezing too; the child buffer's lifetime cannot certify its parent pointer. With lag off, native parent
pointers also remain nonmemoizable unless owned snapshot/log bytes certify them. Writable-stage reflection avoids
empty prepass scans, but every real writer still updates content history before readonly capture. Vertex/index live
resolution is retained per frozen mapping/version and actual submission, with explicit recycled packet resets.

The identical corrected `vkdrawtest --lag-walk-bench 6` fixture compares saved `858924a` sources, backend-only changes,
and the complete change in both A→B→C and C→B→A order. Each case has two warmup and six measured frames, 5,760 indexed
draws/frame, exactly one actual submission, 12 input pools plus safety storage, four VS/four FS UBOs, readonly SSBOs,
four vertex streams, physical/shared aliases, partial and unrelated updates, and true UBO/vertex/index GPU producers.
The second case deliberately rebinds relevant VS0/FS3 ranges and consumes inline updates; observed freeze input-hit
ratios are about 88% stable versus 28% churn, not an imposed cache-success target. Two twelve-chunk stress frames are
reported separately. Mean recording-thread `mtl_exec` CPU ns/draw across the two process orders:

| Source selection | Stable bindings | Relevant binding/update churn |
|---|---:|---:|
| Saved `858924a` baseline | 4,101 | 5,862 |
| Backend-only memo/authority work | 3,421 | 4,812 |
| Complete walking-side change | 2,459 | 3,720 |

These are observed host CPU reductions of 40.0%/36.5%, or 9.46/12.34 ms per artificial 5,760-draw replay; they are
**not Android frame predictions**. Full-change lag-off controls average 2,573/3,514 ns/draw. The host result does not
establish the requested device lag-on overhead target of 1–2 ms. Both partial comparisons correctly fail the two new
post-timing safety probes (physical/alias rewrite and untracked parent-pointer rewrite); their timing rows are not
acceptance passes. Complete runs preserve pixels, all producer values, actual submissions, immutable old-frame UBOs,
ordered alias overlays, distinct pointer-child pixels and zero gated UBO reuse waits.

Final gates pass normal/TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) at recorder 0/2/3 and EARLYZ=0/1: core suites
with rename 0/1 and lag off/wait/copy/sticky-only, pipeline/indexed/linear spans, seven profiler/pass-merge settings,
private UBO including OFF=rename, and the new walking fixture. Identity/boundary native/widened streams pass all four
chunk/inline/lead policies with lag off, protected lag and ordinary renaming. Seven changed C files pass Android cc1
with empty logs; gather 4/4, reflection 21/21 and display normal/TSan pass. Evidence is
`.scratch/integ3/round6-walk/`. Initial fixture failures are retained: the incorrect vertex stream-field initializer,
implicit automatic chunking, untracked readback destination and event/commit dependency were repaired, not relaxed.
The span test now checks reuse before deliberately killing its covering snapshot, then explicitly forbids untracked
native reuse. Boundary expectations distinguish private lag updates from live direct updates. Invoking the
private-UBO-only fixture without protected lag is a rejected prerequisite, not a suite pass.

### Device-shaped lag miss-path revision

Main's Thor Lookout device results for `89c0735` (APK `ccd03cd4`, recorder threads 2) retain visual correctness:
two 120 s MOVE sweeps measure protected lag 28.5/28.5 fps, worst windows 23.5/24.7, versus lag off 27.6/28.0 fps,
worst windows 20.7/20.8, with no flashes, crashes, waits or misses. Standing profile logs
`.scratch/bench/t-r6b-prof{0,1}.log` nevertheless show walking 29.8→35.3 ms: vertex/index 1.7→7.6, draw-touch
6.1→9.3, snapshots 0→2.1, guard registration 1.2 ms and about 2,381 registrations/frame. Hot copies are
279–325 ranges / 226–273 KiB/frame. Resolution, descriptors and program work compensate in other categories;
the remaining lag cost is not a claimed device regression in sweep FPS.

The earlier host fixture's near-zero lag overhead misses that workload. This revision targets distinct vertex/index
offsets within shared spans/dirty units and learned-hot small ranges, rather than treating repeated exact bindings
as representative. Sampled per-kind lag decision/memo counters belong to the existing 300-frame diagnostic log;
full device attribution remains necessary before claiming a sub-millisecond vertex/index result.

The change folds 64-KiB-separated address lanes into memo hashes, reuses physical mapping only inside a proven winning
elementary segment/window, and classifies contained readonly offsets using real **range-relative** dirty units.
Unit-wide producer negatives validate that whole unit against the ordered producer journal; broad/over-cap ranges
retain the conservative path. Unit caches hold metadata and alias projection **geometry**, never native bytes or a
reusable “watched” boolean. Every new native capture reacquires physical/all-alias writer gates and brackets copying
with the dirty token. Frozen vertex/index mapping misses use page-hashed hints; known readonly storage ranges with
no pending logical overlap avoid pool lookup, and repeated attributes touch each vertex stream once. Every writer
still invalidates contents before readonly capture.

`--lag-walk-bench` now includes cold, two real training frames, learned-hot and separate twelve-chunk device-shaped
cases: three vertex selections plus one changing U16 index selection per draw, 5,760 checked pixels, 96 fixture units,
and an observed 288 hot copies / 243.4 KiB per learned-hot frame. This intentionally does not reproduce the device's
2,381-unit inventory or GPU-written hot neighbours. Those remain covered by the original mixed-producer and strict
alias/pointer probes. Exact binding misses and contained-classification hits are reported separately; a cold case
observes 17,280 vertex and 5,760 index decisions/frame, not a synthetic draw-normalized hit estimate.

Identical final fixture/seed sources pass against both saved `89c0735` and the new backend. Six measured frames/case
after two warmups, baseline→new then new→baseline, give these **host recording-thread CPU ns/draw** means:

| CPU placement / case | `89c0735` | New |
|---|---:|---:|
| Normal allowed CPU set, stable bindings | 2,867 | 2,655 |
| Normal allowed CPU set, binding/update churn | 4,766 | 3,330 |
| Normal allowed CPU set, device-shaped cold | 1,155 | 1,251 |
| Normal allowed CPU set, device-shaped learned-hot | 1,401 | 1,194 |
| All threads pinned to CPU 19, device-shaped cold | 1,699 | 1,496 |
| All threads pinned to CPU 19, device-shaped learned-hot | 1,658 | 1,419 |

The unrestricted cold case regresses 8.3% in that pair while the single-CPU control improves 12.0%; learned-hot
improves 14.8%/14.4%. Pinning all Vulkan/recorder threads is a placement control, not a native scheduling model.
This variability and the remaining profiled vertex/index work do **not** establish a sub-millisecond device result;
the new 300-frame counters are required for the next device measurement. No host timing is a game FPS prediction.

Final normal/TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) gates pass at recorder 0/2/3 and EARLYZ=0/1: core suites,
rename 0/1 × lag off/wait/copy/sticky-only, pipeline/indexed/linear spans, seven profiler/pass-merge settings, private
UBO including OFF=rename, and all expanded walking cases. Identity/boundary native/widened streams pass all four
chunk/inline/lead policies with lag off, protected lag and ordinary renaming. Five changed C files pass Android cc1
with empty logs; gather 4/4, reflection 21/21 and display normal/TSan pass. Evidence:
`.scratch/integ3/round6-device/`. Initial new alias-producer fixture failures were corrected by modeling Android's donor
buffer aliases without a competing host-only K_OWN import, retaining actual shared pages and explicit buffer/offset
assertions. Copy-mode gated probes now distinguish optional wait-guard ticket zero from the independently checked
actual next fence; exact one-submission and pixel/zero-wait assertions remain mandatory.

A source audit also identified a pre-existing completion-post caveat: host event/counter posts use dirty-preserving
stores without entering the GPU producer journal. This change does not add broader writer-watch authority or alter
that completion protocol. Fresh gate acquisition remains mandatory on each actual capture; a separate producer-lifetime
audit is needed before expanding reusable native-byte authority across those posts.

### Cross-frame metadata and uniform sharing revision

Supplied Thor Lookout `ba2bcc6` two-worker sweeps (`t-r6c-lkm-{lag-a,off-a,lag-b}.log`) measure protected lag
29.1/28.9 fps versus off 29.0, with no flashes, crashes or waits and better worst windows. Walking nevertheless
remains about 24.1–24.2 versus 15.8 ms. Main's counter attribution is vertex 3,685 × 787 ns (~2.9 ms), index
2,539 × 308 ns (~0.8 ms), uniform 7,802 × 251 ns (~2.0 ms), 1,695 uniform allocation requests / 4.7 MiB per frame,
guard registration ~1.24 ms / 1,298 units, capture ~0.53 ms and storage ~0.47 ms. Submission/ticket invalidations
dominate the avoidable metadata misses: vertex 1,315, index 1,124 and uniform 2,053/frame.

Expected device savings are **inference, not measurements**: retaining unchanged metadata might remove roughly
0.5–1.5 ms (vertex/index miss-cost arithmetic ceilings are ~1.04/~0.35 ms); reducing duplicate uniform copies might
save ~0.3–1.2 ms inside the measured uniform work. The uniform portions overlap and must not be added twice.
Actual copied-byte reduction depends on overlapping ranges and distinct logical versions, not merely call counts.

Guard arming is already globally unit-batched for each actual ticket: `vk_record_lag_submit` merges immutable job
unit sets and partitions disjoint registrar shards; `nvn_dirty_lag_batch_register` invokes `lag_protect_unit` once per
deduplicated unit. Per-range alias projection occurs earlier on recording workers. Repeating that batching design
cannot be credited with another 1.24 ms saving, and guards must still be republished for every actual queued ticket.

Raw guest captures cannot simply move to a later chunk worker: concurrent CPU writers, inline updates and
materialization can change bytes after the walking position, while uniform-derived pointers influence walking-time
producer discovery and eligibility. Already-owned immutable source bytes plus frozen ordered overlays could be
copied on workers, but mutable guest pointers/log entries are not those inputs. This revision claims no speculative
worker-shift saving; avoiding redundant owned-source copies is the first safe structural reduction.

Implemented metadata reuse separates range/mapping identity from copied-byte lifetime. A 4,096-entry original-producer
journal advances for every real GPU producer before same-range deduplication, and for direct/materialized original
updates, not for logical shadows or snapshot reset/seal/prune bookkeeping. Negative answers survive unrelated tickets;
positive answers expire with completion, and missing/overflowed history rescans. Real present-frame and CPU-write history
refresh hot promotion/demotion. Frozen mapping-only lookup/hint keys use the immutable span generation.

Owned snapshot subranges without a relevant newer overlay bind directly, even if the broad merged snapshot was marked
updated. Overlay copies share bounded windows of at most twice the requested range and at most 4 KiB, entirely inside
owned bytes, with exact alignment/content/mapping/snapshot checks. Full-width origins are preferred to avoid two
unnecessary overlapping captures; only genuinely crossing ranges use the half-step origin. Immutable owned copies may
survive intermediate Vulkan chunk tickets because their arena is game-submission-owned. Native captures and complete
logical-log pointer authority retain their stricter ticket invalidation. No native byte reuse crosses that frontier.

The new ten-frame sharing probe retains five fixed-extent frames (1,024-byte bindings) and adds five changing-extent
frames (1,024/1,040/1,056 bytes for the three semantic versions). All shader-consumed first/last vec4 bytes, true non-UBO
usage aliases, repeated GPU producers, partial postproducer updates, twelve actual chunks/frame and ring reuse are
checked. The eager union is 6,912/6,944 bytes. Identical fixture/seed against saved `ba2bcc6` versus the new head:

| Observed copy/allocation work | `ba2bcc6` | New |
|---|---:|---:|
| Fixed-extent overlay version: allocations / bytes | 4 / 4,096 | 2 / 4,096 |
| Changing-extent first overlay: allocations / bytes | 24 / 24,960 | 2 / 4,096 |
| Changing-extent second overlay: allocations / bytes | 24 / 25,344 | 1 / 2,048 |
| Entire ten-frame probe, including GPU/native tails: allocations / bytes | 690 / 771,360 | 445 / 550,560 |

That is 35.5% fewer allocation requests and 28.6% fewer copied bytes in this probe, not a device bandwidth claim.
An initial 4x window implementation increased the fixed-extent byte total despite fewer allocations; the retained
fixed-extent case exposed it, and the final 2x bound removes that regression. The non-UBO consumer uses a genuine usage
alias of the same physical owner, rather than host-only independently imported VkBuffers masquerading as Android donor
aliases. Separate virtual-writer/remap tests remain intact.

Paired host CPU timings (six measured frames, baseline→new then reverse) do **not** show a broad speedup:
stable bindings 2,818→3,049 ns/draw, churn 3,496→3,634, device-shaped cold 1,190→1,164 and learned-hot 1,232→1,239.
Cross-ticket metadata reuse and byte reduction are exercised, but the initial inferred device saving ranges above
remain unverified; extra validation/counter cost and different workload locality may offset saved work.

Final 552 matrix invocations pass normally and under TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`): recorder 0/2/3,
EARLYZ=0/1, core rename 0/1 × lag off/wait/copy/sticky-only, pipeline, span policies, seven profiler/pass-merge settings,
private UBO including OFF=rename, expanded walking/sharing probes and native/widened identity/boundaries across four
chunk/inline/lead policies. The linear span diagnostic is explicitly `TOTK_VK_OFF=span`; earlier runners labelled
`SPAN_INDEX=0` did not select that mode and are not its coverage evidence. Five changed C files pass Android cc1 with
empty logs; gather 4/4, reflection 21/21 and display normal/TSan pass. Evidence: `.scratch/integ3/round6-frame/`.

## Round-five integration

`integ/round5` starts from the repaired round-four head `c1ca4c5`. Its device rerun removed the GPU-time
regression and in-game flashes (Odin Gerudo GPU 8.1 ms), but protected-lag registration and direct uniform
reuse still exceeded the FPS60 walking budget.

Merge order: Flicker9 `6be25f9`/`0cbb1e0` (rename default off, sequence-correct direct-update overlays);
LagSafe `1d2cb87` (concurrent explicit-ticket registration); recorder iterations four/five through
`a1b75ad` (lead chunks, profiling, semantic program/binding memos, range invalidation and touch fast paths).
Uniform-log conflict resolution retains the fourth `flushed` argument **and** `ub_changed(lo,n)`, along with
snapshot base sequence watermarks. Reason-aware profiler pass ends retain recorder parking/ordered assembly.
The writable prepass precedes the no-pending-update early return.

Recorder packets carry immutable live buffer ranges, reasons and the containing actual-submission ticket.
Recording workers project through every matching frozen owner/view/alias span into job-owned CPU-VA intents.
They do not arm future tickets during walking. After ordered-primary assembly and command-buffer finalization,
a separate registration worker phase arms these intents and joins before queue submission. Primary transfer/upload
guard batches obey the same boundary, including submissions without rendering. Registration failure is an atomic
ticket fallback: join first, submit, then wait. Hot selection and immutable copies remain walking-side.
The software-GPU path retains late write exclusions; the native-policy test override bypasses those exclusions.

Steps 1–3 checkpoint gates pass with rename default-off and `UB_RENAME=1`: normal and TSan
(`LP_NUM_THREADS=0`, `halt_on_error=1`), threads 0/2/3, lag off/wait/copy/sticky-only; pipeline and indexed/linear
span suites; seven profiler/pass-merge configurations; identity/boundary native/widened streams at
chunk/inline/lead 128/0/32, 1/0/0, 512/512/32 and 128/0/0. Direct-mode fixtures explicitly assert their extra
update pass boundaries and already-flushed logs, rather than requiring optional rename-only memo behavior.
Ten changed C files pass Android `cc1.sh` with empty logs. Display pacing normal/TSan and gather/reflection
Python tests pass; deliberately disabled lag controls still fail. Evidence is under `.scratch/integ3/round5/`.

The direct-mode chunk test exposed a real CPU-overlay race: the helper read native bytes while the GPU was
updating them, then overwrote those bytes with authoritative CPU-known log data. It now collects/prunes log
coverage first, reads only uncovered native gaps, then applies entries in sequence order. Snapshot bases remain
immutable. This fixes rename-off TSan runs without suppressions or waiting for known log bytes.

The subsequent private UBO path retains ordinary rename-OFF defaults but never sends UBO-only updates to the live
guest pool under protected lag. A CPU-known logical shadow feeds immutable per-submission descriptor storage.
Exact non-UBO aliases materialize ordered updates in guest storage; genuine writable/GPU-produced ranges retain
guards. Remaining private updates publish after final queue submission, with CPU post delivery gated separately
from GPU-only completion. This avoids both pure-UBO reuse waits and same-queue publication deadlocks. Descriptor
preparation before the recorder's no-pending early return prevents late alias materialization from ending a newly
opened rendering pass. `vkdrawtest --lag-uniform` covers repeated CPU reuse, pixel identity, chunk/ring reuse,
ordered aliases, genuine GPU production and writable aliases.

Device testing of checkpoint `0cf025c` hung at the title with protected lag and recorder threads two: after long
frame 213, image writeback messages continued but presents stopped. The native path could arm a future ticket while
the walking thread was still inside the same unsubmitted chunk. A later CPU store/writeback then waited for work
whose submission required that very thread to return. The two-phase registration boundary above removes this cycle;
returning failure only for BUSY registrars did not protect walking CPU writers. A native-policy lavapipe regression
retains readonly inputs, drains recording, then performs a walking store before submission under a bounded
process-failing watchdog. A separate real post-submit event verifies that reuse still waits for actual GPU reads.
Long lag ticket/gate waits now log once after 500 ms with unit, ticket, submitted/completion state and waiting TID,
using signal-safe formatting and raw writes on fault paths. These repairs still require a fresh device rerun.

Host gates at checkpoint `6eec899` (`.scratch/integ3/round5/repair-*`, `repair-results.json`):

| Coverage | Normal | TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) |
| --- | --- | --- |
| `vktest` / `vkdrawtest`: threads 0/2/3, rename 0/1, lag off/wait/copy/sticky-only | pass | pass, no reports |
| `vkpipetest`, indexed and linear `vkrecspantest`: threads 0/2/3, rename 0/1 | pass | pass, no reports |
| Identity + boundary, native/widened indices; chunk/inline/lead 128/0/32, 1/0/0, 512/512/32, 128/0/0; rename 0/1 | byte-identical | byte-identical, no reports |
| Seven GPU profiler/pass-merge configurations: threads 0/2/3, rename 0/1 | pass | pass, no reports |
| `vkdrawtest --lag-uniform`: threads 0/2/3, wait/copy/sticky-only and explicit OFF=rename | 189 checks each | 189 checks each, no reports |
| Native-policy prequeue store/postqueue reuse fixture: threads 2/3 | pass | pass, no reports |
| Scratch-only old-order control: threads 2/3 | bounded failure as intended | bounded failure as intended, no race report |

The old-order control restores arming at recording completion only in scratch sources. It exits 1 after the
10-second watchdog identifies `walking CPU store before QueueSubmit2`; its 500 ms record marks the captured
ticket **unsubmitted**. The fixed fixture passes, and its deliberate 600 ms GPU hold emits one **submitted/inflight**
fault-wait record before genuine completion. Protection-off negative controls still fail exactly two buffer and
four draw hazards; the TSan buffer control stops at the expected reuse race (exit 66), not a protected-mode failure.
Ten changed C files pass Android `cc1.sh` with empty logs (the large `nvn_vk.c` check needs more than 120 seconds).
Display pacing passes normal/TSan, gather-policy tests 4/4 and reflection tests 21/21. No device access or full Android
build was performed. The initial unseeded private-UBO invocations are recorded failures, superseded by the complete
seeded runs above; evidence must not count them as passes.

### Registrar ownership-handoff follow-up

Odin title testing of `6eec899` still hung near frame 395. Its watchdog captured a host-origin gate waiter and guest
fault on the same 8 KiB unit, ticket 1440, with GPUdone/submitted both 1439. The old log had no thread role/name,
so those TIDs alone do not establish which runtime thread was the host waiter.

The explicit-ticket registrar rejected BUSY only if its **sampled** old ticket was incomplete. If an already-complete
BUSY owner released the gate, another registrar could arm the future ticket and a guest could claim it before the
first registrar woke. `lag_gate_wait` followed that later BUSY owner and blocked the registrar join needed to submit
the ticket. Explicit-ticket registration now fails nonblockingly on every BUSY observation; the existing fallback
queues the actual work first and only then waits. Zero-ticket dirty watching retains its normal ownership protocol.
Runtime role scopes and raw OS thread names supplement the signal-safe watchdog so future records distinguish
registration, walking/submission, completion/post operations and private-uniform publication.

The deterministic native-policy regression passes at threads 2/3 normally and under TSan. It pauses a real registrar
after capturing completed BUSY, ends that owner, arms the actual next ticket, then starts a guest fault that takes
future BUSY before the registrar resumes. It requires registration failure, actual **postqueue** fallback observation,
protected reuse, correct GPU-read bytes and genuine completion releasing the writer. A scratch-only inverse restoring
the old completed-ticket predicate fails all four runs through the 10-second watchdog at
`future-BUSY registrar join before QueueSubmit2`. Its paired diagnostics reproduce the device's pattern and identify
`role=registrar` host-gate and `role=test-guest` fault waits on the same unsubmitted ticket; no TSan race report occurs.
This proves the code-level cycle, not a retrospective identification of the old device TID.

All checkpoint matrix coverage above was rerun after this fix and passes normally/TSan: threads 0/2/3, rename 0/1,
lag off/wait/copy/sticky-only; pipeline/indexed/linear spans; native/widened identity and boundaries at all four
chunk/inline/lead settings; seven profiler/pass-merge combinations; and the private UBO fixture's wait/copy/sticky-only
and explicit OFF=rename controls. The original 40-primary-guard fixture now opens its real GPU event on an independent
postqueue coordinator, because duplicate BUSY registrars may legitimately choose synchronous fallback; requiring
commit to return first was a fixture-only dependency cycle. All four changed C files pass Android `cc1.sh` with empty
logs. Gather 4/4, reflection 21/21 and display normal/TSan also pass. Evidence:
`.scratch/integ3/round5-handoff/{final-*,extra-*,uniform-*,native-coordinated-*,oldbusy-negative-*,cc1-*,results.json}`.
Initial fixture-only failures are retained separately; final positive runs pass. A fresh device title/world run is
still required; no device/full Android build was performed here.

### Unit-batched protection performance follow-up

Supplied Odin testing of `a4f648a` reports **no hang, zero reuse waits and no in-game flashes**, but protected lag is
recording-bound: Gerudo 30.0 fps / recording 20.9 ms / walking 14.7 ms versus lag-off 60.0 fps / 11.6 / 9.6 ms;
Lookout 25.5 fps / recording 36.0 ms / walking 23.8 ms versus lag-off recording 19.1 / walking 14.9 ms. Lookout
registered about 6,974 projected ranges/frame, with 2,362 armed-ticket updates, 230.7 registrar jobs and 4.481/3.367 ms
registrar service/join. These are device observations from `.scratch/bench/o-r5d-{gelag,lklag}.log`, not host timings.

Optimization keeps the proven post-walk arming boundary. A completed recorder job is not an independently submitted
Vulkan chunk: arming its future ticket while the same submission continues walking would restore the original CPU
store deadlock. Only immutable alias projection and dirty-unit collection may overlap the walk. Late arming deduplicates
all jobs by actual Vulkan ticket and dirty range-relative unit, with reason OR/max-extent diagnostics, before sharding
unique units across a bounded worker set. Retained unit identities must reject removed/reused range generations.
Frozen spans capture one published dirty-incarnation epoch. First collection rejects a range born later, closing
capture-to-first-add reuse without eagerly collecting every unit in every unrelated span. Later add/merge/register
checks exact slot identity. Batch registration rejects ticket zero, keeping legacy watch semantics out of registrars.
Physical-owner caches no longer invalidate on unrelated virtual remaps, and private/hot uniform descriptors reuse the
existing range-aware update history instead of treating every unrelated update as a full memo invalidation.

Representative host CPU microbenchmarks (not device frame predictions), 7,000 ranges / 2,000 pools and units:

| Phase | Legacy | Unit batch, 2 shards | Unit batch, 3 shards |
| --- | --- | --- | --- |
| Warm new-ticket input work | 140–141 ns/input range; 0.980–0.990 ms wall | collection 146.54 ns/input; merge 21.69 µs; arm release/join 41.56 µs | collection 146.57 ns/input; merge 23.25 µs; arm release/join 56.46 µs |
| Warm full serial-collection pipeline wall | 0.980–0.990 ms | 1.089 ms | 1.105 ms |
| Equal-ticket repeated arm | 146–165 ns/input range | 4.87 ns/unique unit | 5.96 ns/unique unit |
| Cold: all 2,000 units require mprotect | 4.783–4.787 ms, 2,000 calls | 6.464 ms full pipeline, 5.173 ms arm release/join | 7.518 ms full pipeline, 6.226 ms arm release/join |

Warm collection is not faster in this deliberately 2,000-pool lookup-heavy layout; its work moves into recording
overlap, while the late critical portion becomes merge plus bounded arm/join rather than per-range lookup and 230 jobs.
The all-cold case is slower due to concurrent permission changes and is not hidden. Device steady state previously
rearmed about 15 units/frame, not all 2,000. Indexed alias projection measured 23.7/21.7 ns/range versus full scans
4,050.2/3,950.7 ns/range (indexed/linear span modes), with cold copy/index/free about 9–10 µs/version. These separately
measured stages cannot simply be added to predict Android time; the requested device <1–2 ms target remains unverified.

Normal and TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) acceptance passes: all prior core/rename/lag matrices,
pipeline and span suites (now also protected indexed/linear spans), identity/boundary streams, seven profiler/merge
configurations and private UBO checks. Both old deadlock inverses still fail bounded at their exact expected phases
with unsubmitted-ticket diagnostics, while protected native fixtures pass. Five changed C files pass Android cc1
with empty logs; gather 4/4, reflection 21/21 and display normal/TSan pass.

The added native-policy lifetime fixture uses real shared physical/virtual aliases and a nested pool view. Collected
unarmed refs survive removal/re-add only as rejected stale refs, without protecting the new lifetime. For armed work,
a retirement thread follows the production finalizer's drain-before-dirty-remove/backend-delete ordering and is observed
blocked on the actual queued fence before the GPU event is opened. Physical/view/alias mappings remain intact, queued
bytes are correct, and reuse after genuine completion rejects old refs. CPU handshakes order metadata observations
for TSan; they cannot release the real GPU drain. This proves the synchronization contract exercised by finalization,
not permission for arbitrary concurrent destruction: the batch generation check does not pin non-atomic pool storage.
Evidence: `.scratch/integ3/round5-unitbatch/` (`results.json`, final/extra/uniform/lifetime-synchronized logs,
bench-core/projection logs and oldarm/oldbusy-negative controls). Earlier fixture-policy and observation failures are
retained separately, not counted as final passes.


## Round-four recorder override lifetime repair

Device tests of `bf84552` isolated a severe regression to recorder + protected lag: Odin Gerudo GPU time
27.5 ms with flashes, versus 8.3 ms with capture disabled or 7.0 ms with recorder threads disabled.
Thor Lookout showed 54.9–62.2 ms GPU work and extra flashes. These are supplied device observations,
not host performance measurements.

The integration added vertex buffer/offset overrides to reusable recorder draw packets but did not clear
them when the next lag decision retained a live range. After slot/job reuse, workers could bind an old
snapshot arena location while the walk registered protection for the new live address. The old offset could
also be paired with a new size. Used stream overrides now reset before each capture decision.

The added 24-submission regression fills every ring slot with captured streams, then switches to
GPU-produced live vertices. Before the fix, the actual-worker bind assertion fails; pixel-only checks could
miss it when another arena allocation happened to overwrite the stale location with matching data.
After the fix, worker bind checks, pixels and normal/TSan suites pass at threads 0/2/3.
The fixture's copied-byte/captured-range/GPU-owned-range totals match all three recorder modes:
wait 2,288/24/72; copy 7,248/96/16; sticky-only 0/0/72.

The reusable-field audit found index handles/addresses/offsets already reset at draw begin; descriptor and
texel-view inputs zeroed before filling, with memo reuse constrained to the exact chunk/submission; copy-source
handles are per-operation locals. Independent static reviews found no other definite override/arena-lifetime,
memory-type or extra-barrier defect. Snapshots use the same arena and cached-host preference as inline mode.
This identifies and fixes a concrete bad binding, not yet proof that every device timing symptom is resolved.

Evidence: `.scratch/integ3/regression/`. Protected wait/copy/sticky-only `vktest` and `vkdrawtest`, lag-off
core/span/pipeline suites, identity/boundary comparisons at all three chunk/inline settings, and seven
profiler/pass-merge configurations all pass normal and TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) at
REC_THREADS=0/2/3. Both changed C files pass Android `cc1.sh` without diagnostics. No device interaction,
full Android build, worker-side registration follow-up, default change or sanitizer suppression was added.

## Round-four lag safety and pacing integration

`integ/round4` continues from round three (`e97b522`) in the same isolated integration worktree.
It merges `fix/android-lag-safe` (eager capture `db3d93a`, default wait mode `266e694`) before
`feat/android-display-pacing` (`76cf59b`). In that historical checkpoint, `TOTK_FRAME_LAG` was opt-in;
round 10.1 later changed that default, now superseded by the current policy above. `TOTK_PACE=display` also remains opt-in, with prior pacing behavior unchanged when unset.

- Recorder storage descriptors pass reflected extents through the ordinary walking resolver under protected
  lag. Readonly inputs use returned immutable copies only when requested; writable bindings stay live and are
  registered before any readonly alias can be captured. Vertex packets now support immutable buffer/offset
  overrides; existing index overrides serve the same purpose. Workers never invoke lag protection.
- Freeze reuse keys already include the actual Vulkan submission serial and recorder chunk ID. Thus unchanged
  bindings register again after a real GPU chunk. The lag draw fixture exercises a completed first chunk then a
  gated second chunk inside the same logical submission, without rebinding, before CPU reuse.
- `lag_guard_submit` remains after recorder assembly/command-buffer finalization and before queue submission,
  including ordered-primary arrays. The separate GPU-only completion cursor is preserved; CPU post-op delivery
  retains its original completion semantics. Hitch timing records the observed completion handoff, not a driver
  signal timestamp.
- Shader fixture conflicts retain all profiler/pipeline fixtures and add distinct readonly lag fragment salts
  5/6, avoiding the profiler's fragment salt 4. Include and resolver conflicts combine the dirty-protection
  helpers with recorder span snapshots and CPU profiling wrappers.
- Display pacing merged cleanly. Odin 3's explicit `TOTK_UNCAPPED=0` control (77.4→60.0 fps) confirms
  the inherited 120-Hz uncapped-policy diagnosis, not physical acceptance of display-callback pacing.
- Sticky/hot follow-up `8687ace` is also merged. Wait-mode `vk_lag_read` may return an immutable copy for
  a learned-hot readonly range, and the same recorder override paths handle that result. The per-present
  dirty-frame hook coexists with hitch attribution. Guest tests use a real native first-byte store before
  instrumented `memset`; production CPU post stores hold unit ownership across the actual store, without
  sanitizer exclusions/suppressions. All registration deliberately remains walking-side in this round.

Final round-four acceptance is recorded under `.scratch/integ3/round4/` (`final-*` logs):

| Suite at REC_THREADS=0/2/3 | Normal | TSan, LP_NUM_THREADS=0, halt_on_error=1 |
| --- | --- | --- |
| Lag off: `vktest` / `vkdrawtest` / `vkpipetest` | 21/48/61 checks each | same, zero reports |
| Lag wait: `vktest` / `vkdrawtest` | 56/66 checks each | same, zero reports |
| Lag wait, HYBRID=0: `vktest` / `vkdrawtest` | 56/66 checks each | same, zero reports |
| Lag copy: `vktest` / `vkdrawtest` | 29/62 checks each | same, zero reports |
| `vkrecspantest`, indexed and OFF=span | 7,089 checks each | same, zero reports |
| `vkrectest`, identity + boundaries, native/widened; chunk/inline 128/0, 1/0, 512/512 | all byte comparisons | same, zero reports |
| All seven profiler/pass-merge configurations from round three | pass | pass, zero reports |

The deliberately unprotected `LAG_CAPTURE=0` controls fail at all three worker counts: normal `vktest`
reports exactly two 256-byte rewrite failures and `vkdrawtest` exactly four storage/vertex/index hazards.
TSan's unprotected buffer control stops on the expected real reuse race (exit 66); unprotected draw controls
report the same four pixel failures (exit 1). These are expected negative controls, not protected-mode passes.
All 17 C files changed since `c1fbffa` pass Android `cc1.sh` without diagnostics; standalone
`frontend_headless.c` uses `-UTOTK_ANDROID_APP` (the app checks it through `frontend_android.c`).
Display pacing passes standalone normal and TSan tests; Python gather-policy tests pass 4/4 and reflection 21/21.
Earlier wait-mode TSan failures were resolved by the native guest-write probe and production post-store
ownership fixes above; final protected runs need no suppressions. Lag-off ignores an invalid lag-mode
setting. No device access, full Android build, shared build writes or pushes were performed.


## Round-three feature integration

`integ/round3` starts at `test/mt-recorder` (`c1fbffa`, dev `0d94333` plus the original
shader-list data and recorder iterations 1–3). Merge order is shader-list fills (`a20ef05`,
29 rows), gather-bias policy, bounded async pipelines (`56b7c42`), then GPU pass profiling
(`baa3077` and `58e25f2`). Branch defaults are unchanged.

- Makefile conflicts combine recorder/profiler sources and all five host test executables.
  Seed conflicts keep the profiler's real storage-writing compute/fragment fixtures and the
  async pipeline fixture's shared graphics shaders.
- Every reason-aware pass end retains recorder parking, next-primary selection, and ordered
  assembly at slot submission. Submission reasons layer onto the recorder's profiling wrapper;
  hitch counters coexist with its CPU scopes, walk timing and completion wait accounting.
- GPU_PROFILE=1/2 and PASS_MERGE=1 force inline recording with a diagnostic log. This avoids
  timestamping walking-primary sections instead of parked worker work.
- Pipeline selection remains walking-only; recorder packets contain selected immutable handles.
  The program memo caches shaders, never pipelines. Async publication therefore changes no cached
  program result: each draw still acquires the pipeline entry state. No new epoch is needed for
  this publisher. A future asynchronous shader/variant publisher requires an epoch in that key.
  `vkpipetest` keeps stage signatures/bindings unchanged through cold miss, repeated skips,
  publication and a later real-pixel draw, asserting the recorder memo hits with workers enabled.
- Async skip bounds continue to count logical game submissions, not recorder jobs, draw count
  or ordered-primary/GPU submission chunks.

Round-three exercised acceptance (logs and `results.json` in `.scratch/integ3/`):

| Suite | Normal | TSan (`LP_NUM_THREADS=0`, `halt_on_error=1`) |
| --- | --- | --- |
| `vktest`, REC_THREADS=0/2/3 | 19/19 each | 19/19 each |
| `vkdrawtest`, REC_THREADS=0/2/3, WRITEBACK=1 | 48/48 each | 48/48 each |
| `vkpipetest`, REC_THREADS=0/2/3 | 61/61 each | 61/61 each |
| `vkrecspantest`, REC_THREADS=0/2/3, indexed and OFF=span | 7,089/7,089 each | 7,089/7,089 each |
| `vkrectest`, identity + boundary identity, native + widened uint8, REC_THREADS=0/2/3 | all ten byte comparisons per run | all ten byte comparisons per run |
| Identity chunk/inline 128/0, 1/0, 512/512 | all three settings | all three settings |
| `vkdrawtest`, GPU_PROFILE=1, =2, PASS_MERGE, profile+merge, stats, STORE_CLEAR guard, precise occlusion+stats; requested threads=0/2/3 | all seven configurations, 48/50/55 checks as applicable | same; zero reports |

Identity streams compare 787,888 bytes and boundary streams 409,856 bytes; boundary tests fix their own
128/0 worker-gate setting. GPU_PROFILE=1/2 and PASS_MERGE logs explicitly confirm worker disabling.
Nine changed C files (`nvn_vk`, `test_vkdraw`, `test_vkpipeline`, `test_vkrecord`, `vk_draw`,
`vk_gpuprof`, `vk_record`, `vk_res`, `vk_shader`) pass Android `cc1.sh` with empty diagnostic logs;
test fixtures use `TOTK_VK_TESTS`. Gather-bias translator/seed Python tests pass 4/4.
Normal and TSan host builds succeed. The initial seed command lacked `glslangValidator` on PATH;
using the already installed `.scratch/hosttest/bin` compiler completed seed generation without installation.
The TSan workaround disables only lavapipe raster threading, not backend workers; it does not fix or
claim coverage for the known default-threaded uninstrumented Mesa race. No device/Turnip performance,
full Android build, or Khronos validation-layer result is claimed.


## Commit decisions

| Upstream | Classification | Decision / dev counterpart |
| --- | --- | --- |
| c3d0100 | Useful | Keep black-frame logging/saving, default on. |
| 5107e99 | Useful | Keep pure-black detector refinement. |
| b61b855 | Useful | Keep sharing GPU-written CPU-pool textures by storage. |
| b07fd6d | Useful | Keep library build-date first log line. |
| b8d4adb | Useful | Keep flash-versus-fade detector refinement. |
| ef1aadf | Useful | Keep GPU-fenced present tickets and acquire waits; distinct from dev's explicitly opt-in frame-lag experiment. |
| c9825c5 | Useful | Keep synchronous-submission and frames-ahead diagnostics. |
| 6b1ee5e | Useful | Keep compressible format-class isolation alongside dev UBWC view lists (`d3293a1`). |
| a8d728e | Duplicate | Linux hactool cookie support already in `16582bd`; retain one Linux implementation and upstream portable reset/reapply handling. |
| 494c807 | Duplicate | ACID-line acceptance already in `a082ca6`; retain one check. |
| ca58b1c | Useful | Keep Android/data folder creation in installer generation. |
| 3389b27 | Useful | Keep pass-ending NVN barriers; measure attachment traffic/extra passes. |
| ac769c6 | Useful | Keep cross-format share logging / FMT_SPLIT diagnostic. |
| 6303829 | Useful | Keep depth-target-size diagnostics. |
| 91d6c81 | Useful | Keep opt-in pass probe and upstream debugging history. |
| cfc204f | Useful | Keep flash comparison against following good frame. |
| e539e59 | Useful | Keep pass pairing and multisample exclusion. |
| 05e7dd7 | Useful | Preserve uncertainty about probe freeze. |
| 2d02f0a | Useful | Keep probe shader/input/missing-binding details. |
| 37e50d5 | Useful | Keep probe depth/copy/clear/upload/store provenance. |
| 53b8262 | Useful | Keep probe depth inputs and 5-second GPU-hang log (adapt to dev fences). |
| 3851c65 | Conflicting | Keep final probe implementation, not intermediate GPU copies; clear stale snapshots at logical submission start without breaking dev chunk lifetime (`9123b49`). |
| 264308c | Useful | Keep probe mode/size limits as subsequently refined. |
| cfe4c64 | Useful | Keep CPU buffer inspection rather than stalled GPU-copy probe design. |
| 10ec880 | Useful | Keep historical observations, explicitly not proof of a storage-feedback cause. |
| 8af3940 | Useful | Keep depth/stencil/mask probe state. |
| 463a862 | Useful | Keep per-draw probe geometry state. |
| d4b8a86 | Conflicting | Keep final full DEPTH16 fix, including fragment-written depth (`6058722`, `f29e573`), not the earlier raster-only version that broke clothes. |
| afac445 | Duplicate + useful | Use dev `vk_ub_read` (`c107e22`) for probe reads; no second `vk_ub_peek` update-overlay implementation. |
| a9fdd4b | Useful | Keep probe target load/clear/provenance details. |
| ea38bc6 | Useful | Keep conditional/clear probe counters. |
| a1ae354 | Conflicting | Dev `f10def1` already has device-verified pending reports (cpend); keep that one pending-report implementation and dev opt-in precise queries (`101dd43`), add upstream cumulative synthetic samples. Do not replace precise completion/query handling. |
| e9abbda | Useful | Keep Thor confirmation of conditional-order fix; historical d16 default superseded by `f29e573`. |
| 18f6296 | Useful | Keep raster-only d16 regression history; not the final default. |
| a9e7a7f | Useful | Preserve historical downloaded-save uncertainty; latest status is upstream owner's final edit. |
| 5b01d16 | Duplicate | Dev `e7a862c` already starts file AND script readers after env; keep both and upstream start logging. |
| 86024f1 | Useful | Keep Android-default async preemption; no equivalent in dev. |
| 1f7dd2c | Useful | Keep guest-PC loader support needed by preemption. |
| 2177125 | Useful + duplicate | Keep priority inheritance and app's appended-only command reader; retain dev's script startup too (`e7a862c`). |
| e02710d | Useful | Keep main-only priority preemption to avoid SDK lock inversion. |
| 8727912 | Useful | Keep interrupted-PC diagnostics. |
| f82c5e6 | Useful | Keep Android starvation escape / 2 ms protection; measure power and latency. |
| 16ea7e5 | Duplicate + useful | Dev `504e6ac` already has Linux/Android 64 KiB dirty units; keep one implementation, add exact-page exception needed by write-back. |
| eadffb6 | Useful | Keep GPU rendering write-back on pool invalidate, exact tracking, newer-write protection. |
| 758065e | Useful | Keep BC and orphaned-image write-back. |
| cbce3b1 | Useful | Keep memory accounting / 300-frame diagnostics. |
| 3232e32 | Useful | Keep descriptor-pool cursor + set cap; dev draw memo (`bc5c360`) reduces sets but does not fix multi-pool reuse. |
| 3afd4c8 | Conflicting | Keep upstream interactive PNG passdump alongside dev env-driven binary range dump (`62f9462`, `7a84c5f`), with distinct helper names. |
| 461ad35 | Conflicting | Keep dev thread-local registry cache (`e5f7a10`) rather than a second texture cache; keep upstream resolved-view/sampler reuse and passdump inputs where unique. Keep dev uniform-log pruning (`4c090bd`, `d64ff7d`), add only compatible overlap bounds. |
| a03df3d | Useful | Keep SPIR-V storage-write detection; essential to avoid binding writable storage from snapshots. |
| 1c94f46 | Useful | Keep interactive per-draw dump and PD_SPLIT diagnostic. |
| 6058722 | Useful | Keep FragDepth patch / specialization paired with raster and clear nudge. |
| f29e573 | Useful | Default DEPTH16 nudge on: upstream Thor verified water, clothes and horizon. |
| 7aac01e | Conflicting | Keep dev dirty stage signature (`c8da11c`), program cache (`28c651e`), pipeline/draw memo (`bc5c360`), indexed spans (`5c66ccc`, `e7e3611`) and recording-cost hints (`d64ff7d`); add unique rename memo and GPU-write range de-duplication, not a second stage cache. |
| d122373 | Useful | Keep filled-request root fallback and app device path. |
| 1c19d9d | Useful | Keep async translation queue drain before pool write-back. |
| d2eb744 | Useful (docs only) | Owner's test commit changes status text to performance/regular hitching and marks hangs fixed?; preserve it, no executable change. |
| 9028e84 | Useful, adapted | Keep hitch attribution behind `TOTK_VK_HITCH=1` (default off): median sorting and cross-thread atomic increments are not free. Drain per present, fix upstream replay/wait unit conversion, retain dev's existing stutter/>50 ms/seed-I/O logs. |
| 885147e | Duplicate + useful | Dev already has async cache-control cold-miss workers, release/acquire entry publication, normalized keys, memo and prewarm. Keep the one existing implementation/default (`TOTK_VK_COMPILE_THREADS=2`), not upstream's second map/ring or `pipeasync` off-token. Share a cache mutex with compute creation and periodic snapshots; nonblocking cache probes queue on lock contention. |
| c74f0f1 | Conflicting | Keep `TOTK_PIPE_WORKERS` as a nonempty 1–4 override of the existing 0–8 compile-thread setting. Reject permanent-failure skip markers: log each key's first failure, retry with bounded submission backoff, log recovery and count failures/retries. Gather marker handled separately; query-size opcodes alone are not proof of a gather fix. |
| 33d58c4 | Useful + duplicate, corrected | Keep configurable skip bound (`TOTK_VK_PIPE_WAIT_MAX=90`, 0 disables); count distinct skipped game submissions, not upstream's draws. Take over queued work or join its one in-flight owner; no duplicate build/handle overwrite. Niced +10 builders already exist. Add opt-in game-wait counters. |
| 2d2d189 (Vulkan only) | Useful, corrected | Port opt-in fence-return→publication and publication→blocked-waiter latency. Sample under the completion mutex, exclude completed polls/timeouts, do not claim actual driver-signal timestamps or additive latency parts. Do not import translator/build_android.py changes in this slice. |

## Conflict resolutions

There were **40 conflict hunks in 12 files**. Locations below name the original merge hunk, not final line numbers.
“Both” means combined intent, not stacked competing implementations. Compile-only checks are not device evidence.

| File / original hunk | Resolution |
| --- | --- |
| `build_app.py`: bounded seek helper | Dev status/out-parameter helper; avoids narrowing the GNU offset and handles the full bounded range. |
| `build_app.py`: `fopencookie` callbacks | Dev native-size GNU callbacks; remove upstream's duplicate platform adapter block which auto-merged outside the markers. Keep upstream reset/reapply detection. |
| `android/README.md`: VMA limits | Both observations, one tracker: Linux/Android 64 KiB default, exact write-back pages, both env knobs; flag combined mapping count unmeasured. |
| `gfx/nvn_vk.c`: completion comment / `post_op` | Dev fence model and timestamp/query fields, upstream recording-time `cval` added. |
| `gfx/nvn_vk.c`: `slot_t` transient/pool fields | Dev independent arena and fence; upstream descriptor-pool cursor / set count. |
| `gfx/nvn_vk.c`: `apply_posts` counter value | Dev precise query result when enabled, otherwise upstream fixed-at-record synthetic count; keep GPU timestamps and cpend removal. |
| `gfx/nvn_vk.c`: completion wait | Dev `VkFence`, upstream 5-second timeout/hang diagnostics. No timeline semaphore reintroduced. |
| `gfx/nvn_vk.c`: slot resets | Reset both query pages and descriptor-pool cursor. |
| `gfx/nvn_vk.c`: `vk_transient` / `DPOOL_SETS` | Both: dev arena allocator; upstream 4096-set pool cap. |
| `gfx/nvn_vk.c`: `mtl_sub_begin` | Dev chunk/replay timing and lookup-thread state, upstream snapshot reset at logical begin only; not per chunk. |
| `gfx/nvn_vk.c`: uniform-log globals | Dev longest-entry bound and prune frontier plus upstream log generation for rename memo. |
| `gfx/nvn_vk.c`: `ub_prune` early exit | Keep dev effective completion/floor frontier check; upstream generation advances when pruning changes the log. |
| `gfx/nvn_vk.c`: `ub_log` | Dev covered-entry pruning and snapshot-update flags plus upstream memo invalidation. |
| `gfx/nvn_vk.c`: `ub_overlaps` scan | Keep dev equivalent/tighter longest-entry bound, including the zero-floor case; no second scan implementation. |
| `gfx/nvn_vk.c`: `mtl_sub_counter` | Dev timestamp queries and precise prefixes, upstream synthetic `cval`; push the same fixed value to the one pending ring. |
| `gfx/test_vk.c`: empty sample expectation | Zero in both precise and cumulative synthetic modes; additionally assert the pending synthetic report is already zero. |
| `gfx/test_vkdraw.c`: new test blocks | Keep all dev precise-query tests and upstream alias/write-back/conditional tests as separate blocks; synthetic-only conditional expectations are gated away from intentional unknown precise results. |
| `gfx/vk_draw.c`: `R` attachment metadata | Both diagnostics share dev level/layer fields; retain upstream serial/format/probe metadata, no duplicate subresource arrays. |
| `gfx/vk_draw.c`: pass probe insertion / `vk_in_pass` | Keep dev `vk_in_pass` and all upstream probe/d16/barrier helpers. |
| `gfx/vk_draw.c`: depth attachment setup | Upstream d16-aware clear and size/serial diagnostics plus dev raw-dump guest address / level / layer / clear bits. |
| `gfx/vk_draw.c`: draw pipeline/layout selection | Dev layout and dirty pipeline memo / async compile states; retain upstream skipped-draw diagnostic messages. |
| `gfx/vk_draw.c`: draw completion | Dev chunk count, raw draw dump, pass profile; add upstream cumulative samples and interactive split. |
| `gfx/vk_draw.c`: upstream persistent `g_cnt` map | Remove duplicate competing map; dev cpend is the sole pending-report authority. Preserve upstream cumulative sample counter and `condorder` A/B semantics. Proof below. |
| `gfx/vk_draw.c`: `OP_SET_RT` | Both stage-signature invalidation and probe set-target counter. |
| `gfx/vk_draw.c`: render condition / report / reset switch cases | Dev pending/unknown handling, raw-dump notes and pipeline dirtiness plus upstream conditional counters and synthetic reset. Reports get fixed values in `mtl_sub_counter`, not a second draw-side map. |
| `gfx/vk_draw.c`: 300-frame logging | Keep all dev fps/stutter/async pipeline/seed-file stats; add upstream barrier/missing-binding diagnostics on a separate line. |
| `gfx/vk_draw.c`: lookup vs memory report | Both, not one or the other. Memory reporting includes both ring and independent snapshot arenas. |
| `gfx/vk_res.c`: registry-cache definitions | Dev thread-local epoch-aware cache; retain upstream sampler epoch and `texcache` off switch. |
| `gfx/vk_res.c`: registration generations | Dev static per-ID generations plus stage revision (no second dynamically allocated generation table). |
| `gfx/vk_res.c`: effective format / resolved-view lookup | Dev `vk_treg_kind` and `tex_by_id_slow`; remove duplicate upstream format/texture caches. Dev stage dirty cache already avoids whole signature walks. |
| `gfx/vk_res.c`: slow lookup result | Dev result assignment; its caller always supplies the out-pointer. Preserve upstream sampler reuse independently. |
| `gfx/vk_shader.c`: `stage_sig` format lookup | Dev cached effective format **and target**, with dev whole-stage invalidation; no orphan `vk_treg_fmt` declaration. |
| `runtime/frontend_android.c`: diagnostic startup | Dev combined file/script startup after env; upstream append-only app setting before starting it. |
| `runtime/input.c`: file reader startup | Dev private starter under combined once-only dispatcher, upstream start log; remove redundant second `started` state. |
| `runtime/input.c`: constructor / dispatcher | Dev dispatcher starts both script and file once; no duplicate constructors or exported file-only entry point. |
| `scripts/extract_update_romfs.py`: ACID signature filter | Upstream ACID-section-scoped filter, dev explanation of prior NPDM verification; all IVFC/hash checks retained. |
| `nvn/nvn_dirty.c`: protection-unit design | Upstream per-system-page state and exact-page exceptions, dev Linux AND Android 64 KiB default. Keep clamped address/length helpers. |
| `nvn/nvn_dirty.c`: GPU-write begin | Upstream take-page state then unprotect contiguous runs (not a second per-unit mprotect). |
| `nvn/nvn_dirty.c`: GPU-write end | Upstream reprotect contiguous runs with generations unchanged. |
| `nvn/nvn_dirty.c`: first initialization | One system-page state granularity and one configurable protect-unit size; preserve `TOTK_DIRTY_UNIT_KB`, upstream pages override `TOTK_DIRTY_UNIT`, dev startup size log. |

Additional integration glue outside marked hunks:

- PNG `pd_sample_copy` / `pd_decode_texel` have distinct names from binary-dump helpers; both dump paths run.
  Upstream probe uniform reads use dev `vk_ub_read`, and the duplicate `vk_ub_peek` definition/declaration are gone.
- Rename memo invalidates on log edits, snapshot reset/drop and chunk seal. Independent snapshot arenas survive chunk
  wrap as before. Memory accounting follows those arenas, rather than upstream's obsolete slot `chunks` fields.
- SPIR-V storage-write detection remains on; `TOTK_VK_OFF=storagewrites` was added for an isolated device A/B.
- Host PNG-output checks consume `fread` sizes, removing GCC unused-result warnings in the imported test.
- Unconflicted runtime preemption, PC classification, image sharing/write-back, fragment-depth specialization,
  installer folder creation, shader fill root fallback and async write-back drain are retained.

## Why cpend covers the device-confirmed condorder case

`DEBUGGING.md`'s eleventh run found seven mask conditionals, none disabled, in a bad frame versus six conditionals,
all disabled, in a good one. Reports reached CPU memory only after GPU completion, so a record-time memory read
could compare different old generations. The same pattern was independently measured on dev for the terrain's
linear-depth clear (`VULKAN.md`): conditional clear when reports at `a` and `a + 16` differ, then immediately report `a`.

The combined code has these invariants:

1. `mtl_sub_counter` fixes the synthetic sample value at recording and stores it in BOTH `post_op.cval` and cpend.
   Non-sample comparison values are zero in both. Reset zeroes the synthetic count; emitted draws add
   `(uint64_t)count * instances`. Therefore two reports with no intervening draw agree; a draw makes them differ.
2. `vk_counter_value` searches pending reports newest-first before memory. A later report to the same address wins,
   even with earlier reports pending, across chunk and game-submission boundaries.
3. The completion thread applies posts in submission order, writes that **same** saved value to memory, then
   removes the oldest pending entry. If there is no pending report, memory holds the newest completed one. A
   completion cannot change the condition's logical value (pending or completed are the same report value).
4. Thus after the first guarded clear reports `a`, later conditions use its new value immediately, regardless of
   whether the GPU has finished it. This is the condorder fix, implemented once by cpend rather than by upstream's
   permanent fixed-size map. It also avoids retaining counter addresses after CPU storage reuse.

`test_vkdraw` retains upstream's no-draw/equality and intervening-draw/inequality tests and adds exactly this
repeated condition/report pattern, a next-submission evaluation and a completed-memory evaluation. They are
**compiled, not executed** here. The reasoning above is a source-level equivalence argument for synthetic reports,
not a claim that the combined device build was observed flicker-free.

Precise queries remain opt-in (`TOTK_VK_OCCLUSION=1`): pending GPU samples are unknown, so conditions stay enabled,
as dev/Metal did. That is intentionally not synthetic condorder equivalence; measure that mode separately.
`TOTK_VK_OFF=condorder` remains available for old memory-only evaluation and fixed `1 << 20` synthetic reports.

## Defaults and evidence

| Setting / feature | Combined default | Evidence / caution |
| --- | --- | --- |
| Pending-report order / cumulative synthetic samples | On; `TOTK_VK_OFF=condorder` disables | Upstream `e9abbda` confirms camera-cut flicker fixed; dev `f10def1` fixes terrain clear, measured in VULKAN. One mechanism as proven above. |
| `TOTK_VK_OCCLUSION` | 0 (unchanged) | Dev measured 76 queries/frame, -1.6% draws but +1.4 ms recording; not a win at Lookout. |
| `TOTK_VK_D16TIE` | 1; OFF token `d16tie` also disables | `f29e573` + DEBUGGING dark-water final result: Thor water, clothes, horizon correct. Keep raster, fragment-export and clear parts together. |
| `mirshare`, `compalias` OFF tokens | Features on | Upstream GPU-written CPU-pool alias fix and compressed-format isolation; not evidence that present waits cured old black flicker. |
| `TOTK_VK_WRITEBACK` | **0 (opt-in)**, was on | `eadffb6`/`758065e` provide read-back and newer-write host tests; upstream first device run saw 423 write-backs but no terrain improvement; BC device outcome unconfirmed. Measured on the Thor (Lookout camera sweep, `TOTK_FRAME_LAG=1`, every other upstream change off): 29.8 fps without, 24.3 with it (70 of 300 frames over 50 ms): each invalidate waits for the GPU. The other seven upstream changes cost nothing measurable there (29.8–29.9). |
| Descriptor-pool reuse | On, no switch (same rendering) | `3232e32`: memory log found 8→212→512→812 pools / ~45 MB/s growth; upstream final status says fixed, detailed section still asks confirmation. Re-measure plateau. |
| `passbarrier` OFF token | Feature on | Upstream fixes dropped NVN barriers; can add >100 pass splits/frame and increase attachment traffic. |
| `present_wait` OFF token | Feature on | GPU-fenced tickets / acquire waits retained. Early ring-overwrite diagnosis later ruled out for camera-cut flicker; no claim about lag-only 9-frame glitch. |
| `TOTK_VK_FRAMES_AHEAD` | 2; 1 waits latest present | Default waits the reacquired texture's ticket; 1 is stricter. Not the frame-lag switch. |
| `TOTK_ASYNC_PREEMPT` | 1 Android; 0 other hosts | Upstream `86024f1`→`f82c5e6`, thread dumps and final overworld status; main-only priority preemption plus PI and starvation escape. |
| `TOTK_STARVE` | 1 when async preemption is on | 20 ms starvation threshold, 2 ms granted-core protection; 0 disables starvation only. |
| `TOTK_DIRTY_UNIT_KB` | 64 Linux/Android, system page on Darwin | Preserve dev knob/default; rounded up to power-of-two pages as before. State is now per system page for exact write-back freshness. |
| `TOTK_DIRTY_UNIT` | Unset; overrides protect unit in system pages | Upstream knob retained; 1 restores per-page protection. Upstream default 16 pages becomes dev's 64 KiB default, which also stays 64 KiB on 16 KiB-page hosts. Exact pages bypass coarse neighbour dirtiness. |
| `texcache` OFF token | Caches on | Dev measured thread-local texture + effective-kind cache retained; upstream sampler cache also kept. |
| `storagewrites` OFF token | Detection on | Added during integration for A/B; a03df3d found reflection rw incorrect and tested SPIR-V stores/atomics. Upstream device dark-water A/B showed no change from this alone. |
| `TOTK_VK_BLACK` | 1 | Flash detector logs up to 40 and saves up to 5 recovered dark frames. |
| `TOTK_VK_SYNC`, `TOTK_VK_SERIAL`, `TOTK_VK_FMT_SPLIT` | 0 | Diagnostic submission wait / per-draw passes / full format isolation. Not performance defaults. |
| `TOTK_VK_PASSPROBE` | 0 | Opt-in 8×8 pass probe, CPU buffer reads; can distort timing and has upstream freeze history. |
| `TOTK_VK_PD_SPLIT` | Unset | Interactive PNG dump only: `d16` or fragment shader prefixes. Env binary dump's separate split controls unchanged. |
| `TOTK_INPUT_FILE_NEW` | App sets presence to 1 unless caller set it | Presence-based: even string `0` enables append-only; preserve upstream semantics. File + script startup after env retained. |
| `TOTK_ASYNC_REPLAY` | `0` (unchanged) | Synchronous replay remains the baseline. Protected lag is separately opt-in in every mode. |
| `TOTK_FPS60` | `0` (shipping controller) | `1` permits the 60-fps controller; lag remains default off, but explicit `TOTK_FRAME_LAG=1` wins. |
| `TOTK_GPU_TIME` | `hardware` for Android FPS60 unset/`0`; otherwise `latency` (including BOTW) | Explicit hardware/latency wins; unknown values log and retain latency. Startup mode, not AUTO's current interval. |
| `TOTK_FRAME_LAG` | `0` everywhere | Only exact `1` enables protected lag; unset and all other values leave it off. |
| `TOTK_VK_CHUNK_DRAWS` | `75` by default; `600` with resolved lag and no native VMA pressure, otherwise `75` | Explicit positive value or `0` wins; startup reports resolved threshold once. |
| `TOTK_VK_COMPILE_THREADS`, `TOTK_PIPE_WORKERS` | Existing **2**, 0–8 retained; upstream alias unset | Nonempty `TOTK_PIPE_WORKERS` overrides with 1–4. The assignment's initial synchronous-default premise was incorrect; parent explicitly required preserving the measured async default. No new PIPEASYNC switch. |
| `TOTK_VK_PIPE_WAIT_MAX` | **90**, range 0–10000; 0 disables | Safety bound, default on per revised parent direction. Count distinct skipped game submissions; 2 is an aggressive map/pop diagnostic, not a newly selected shipping default. |
| `TOTK_VK_HITCH` | **0**, opt-in with 1 | Existing cheap stutter/long-frame diagnostics stay on. New atomic attribution/median profiler disabled by default. |

`TOTK_VK_OFF` tokens added by upstream: `mirshare,present_wait,compalias,passbarrier,condorder,writeback,texcache,d16tie`.
Integration adds `storagewrites`; existing `rename,ds,dyn,block,ts,span,snap` remain. `all` disables all listed features,
not independently named switches such as preemption, black detection or precise queries. Feature switches are read
once by their feature; restart between A/B runs.

## Device measurement matrix for the parent

No Android device or adb was used for this integration. On **both Thor/Adreno 740 and Odin 3/Adreno 830**, use the same
Turnip build, save/camera/weather, shader/pipeline-cache warmth and CPU/GPU caps. Begin with all integration defaults
and `TOTK_FRAME_LAG=0`, `TOTK_ASYNC_REPLAY=0`, shipping 30-fps controller. Compare against dev `8494a6b`.
After load-in, use matching 180-second runs; compare mean/worst-300-frame fps, >33/>50 ms frame counts, recording /
GPU time, average system watts and GPU clock/busy%. Record video, not only still screenshots.

| Scene / task | A/B (one independent change per restart) | Logs and visual acceptance |
| --- | --- | --- |
| Title-camera cuts and dialogue, then Lookout Landing rain camera sweep | Default vs `TOTK_VK_OFF=condorder`; leave precise queries off | No frame-wide dark flashes in default. `dark flash`, `pass probe, flash frame` if separately diagnosing, raw `render cond` / `report counter` dump notes should show repeated guarded clears skipped. |
| Lookout Landing rain (~5500 draws) | Default vs `TOTK_VK_OFF=passbarrier`, then `...=present_wait`; default vs `TOTK_VK_FRAMES_AHEAD=1` | `frame N`, `per frame`, `stutter`, `diagnostics` (barrier splits), `window`; fps/power/GPU tail; no lighting/material feedback corruption. Extra pass splits may increase bandwidth. |
| Same Lookout sweep, Gerudo Town, Depths | Default vs `TOTK_VK_OFF=storagewrites`; then lag 0/1 with all correctness features on | Check storage-feedback lighting and dev's every-9-frame specular disappearance. `shader ... writes N storage binding(s)`, uniform snapshot summaries, `TOTK_SYNC_LOG=1`, optional `TOTK_TIMELINE`. Never promote lag from fps alone. |
| Gerudo Town / desert hill overlooking lake; inspect Link clothing and horizon | Default vs `TOTK_VK_D16TIE=0` (or OFF d16tie) | Default water lit, clothes not black, horizon smooth; compare PNGs and video. `passdump` plus `TOTK_VK_PD_SPLIT=d16` only for separate slow diagnostic runs. |
| Gerudo terrain / streaming into villages; Depths traversal | Default vs `TOTK_VK_OFF=writeback`; independently `mirshare`, `compalias` if needed | No dark/floating/unfogged terrain or wrong-height tiles. `[writeback]`, `[writeback] block-compressed`, `GPU write-back:`, `textures:`, `depth target sizes`; inspect BC write-back and newer-CPU/supersede counts. |
| 10-minute Lookout/Gerudo traversal with region changes | Default, plus writeback-off comparison if mapping growth occurs | `memory:` descriptor pools and allocations plateau after warm-up (no +1 pool/frame or ~45 MB/s trend); process map count stays below limit. `dirty tracking: unit` confirms settings; exact-page exceptions may raise VMAs. |
| Load overworld saves, cross regions, repeat sleep/resume | Default vs `TOTK_ASYNC_PREEMPT=0`, and independently `TOTK_STARVE=0` | No graphics freeze with audio continuing. `[threads] asynchronous preemption`, `[totk-dump]` kicks/preempts/refusals/starvation grants and interrupted PCs; compare hitching/power, not only successful boot. |
| Lookout/Gerudo CPU recording profile | Default vs `TOTK_VK_OFF=texcache`; optionally `TOTK_ASYNC_REPLAY=1` | `per frame`, descriptor written/reused, `memory`, `TOTK_VK_LOOKUP_STATS=1`; dev memos must retain their gains, async invalidate must drain queued work. |
| Separate diagnostic session | Append `passdump 1` after launch; use env binary dump independently | PNG `passdump/fN/index.txt`, raw dump/index output and pass probe still work. Old commands are not replayed at every app start. Do not include dump runs in power/fps comparisons. |

## Original upstream merge: host verification and retained files

That original slice required syntax/link checks only, not execution of Vulkan tests. It performed no full
Android build, shared build output writes, device interaction, commits on dev, pushes or runtime test execution.

Final checks:

- `cc1.sh`, Android app flags / generated v143 headers: **13 files, no diagnostics**:
  `android/gfx/{nvn_stubs,nvn_vk,test_vk,test_vkdraw,vk_draw,vk_res,vk_shader}.c`,
  `third_party/d2r_gfx/gfx/nvn/{hle_nvn,nvn_dirty}.c`,
  `runtime/{threads,loader,frontend_android,input}.c`.
  The first draw compilation exposed duplicate diagnostic helper names, resolved above; final compilation is clean.
- The actual `android/Makefile` link recipes with `TARGET=linux CC=cc`
  and `OUT=<scratch>/upstreammerge` built `vktest`, `vkdrawtest` and `vkdrawtest-app`.
  GCC initially identified unchecked imported test `fread` calls; consuming their return sizes made both final draw
  target builds warning-free. The binaries were **not run**, and shader-seed generation was not needed for compilation.
- Required `git grep -n '^<<<<<<<\|^>>>>>>>'`: no output (exit 1, no matches).
- Scratch retained intentionally: the three host test binaries, 13 per-file cc1 logs (empty means no diagnostics),
  and `verification.txt` under `.scratch/upstreammerge/`. No temporary scripts were created.

## Pipeline follow-on integration (9028e84 through 2d2d189, Vulkan parts)

The revised assignment preserves dev `0d94333`'s existing async pipeline behavior rather than making it synchronous:
the latter would restore the measured hitches. Prewarm remains opt-in and alias normalization, occlusion/cpend,
recording-cost memos and walking-owned shader/layout/pipeline maps remain intact. Full current switches, retry policy,
metric caveats and the Thor/Odin cold-area-load/loading-map/Lookout-sweep A/B matrix are in `VULKAN.md`,
“Pipeline skip safety and hitch attribution”. Default bound 90 was explicitly requested by the parent; a shorter
default has **not** been justified by device measurements.

Host acceptance now includes `vkpipetest`, built by `make ... vktest` with the ordinary backend source list.
Dispatch wrappers deterministically force cache misses, hold real builds and inject two transient driver errors;
successful compilation and pixel readback still use Vulkan. It checks skipped-first/later-real draws, per-submission
not per-draw bounds, in-flight join, queued takeover exactly once, nonblocking probes during busy compilation,
compute/cache exclusion, failure backoff/recovery, and synthetic hitch detail/histogram output. It does not emulate
Turnip or prove game-scene pixel equivalence. At that stage, seed generation added a resource-free compute shader;
round three retains the profiler's replacement storage-writing compute shader and additional fragment fixture.

Exercised follow-on verification (lavapipe Mesa 26.0.8, no device access):

- Android `cc1.sh`: `nvn_vk.c`, `vk_draw.c`, `vk_shader.c`, `vk_res.c`, `test_vkpipeline.c`, clean.
- Actual host `make ... TARGET=linux CC=cc ... vktest`: `vktest` **19/19**, `vkdrawtest` **42/42**,
  `vkpipetest` **58/58**. Writeback enabled for the existing writeback acceptance cases; caches disabled in scratch.
- Same three binaries built with `CC='cc -fsanitize=thread'`, `LP_NUM_THREADS=0`,
  `TSAN_OPTIONS=halt_on_error=1`: **19/19**, **42/42**, **58/58**, zero sanitizer reports, no suppressions.
  Disabling lavapipe raster threads does not disable the backend compiler/completion/helper threads.
- Pipeline test exercised two distinct bounded keys, two failed attempts/two retries and recovery, 205 compiling-draw
  skips, plus a synthetic 60 ms frame with a one-frame zero-counter hitch line and a histogram containing one >50 ms
  frame. These are correctness checks, not measured gameplay latency or power.
- Logs/build outputs: `.scratch/async-pipes/final-{host,tsan}-{vktest,vkdrawtest,vkpipetest}.log`,
  `build-{host,tsan}.log`; isolated output/scratch directories only. No `build_android.py`, shared build output,
  devices/adb, push, or edits to the integrator checkout were used.


## Argument-cache lifetime audit (orig e49aa55, 2026-10-25)

Reviewed original upstream
[`e49aa552832b4fa3e5c5f0e9b0ca98f783b6a12a`](https://github.com/ly05010419/MacNativeGames/commit/e49aa552832b4fa3e5c5f0e9b0ca98f783b6a12a)
against Android base `baeed33`; this is an adaptation, not a cherry-pick. Upstream
`nvn_mtl.m:8136,8150-8161` adds serial and snapshot-retirement generations to the
legacy argument dirty check. Its new `nvn_args_invalidation_probe.h:35-67` checks
replacement and captured-uniform retirement with unchanged bindings in five Metal
cache modes. Root MIT and `third_party/d2r_gfx/LICENSE` notices remain unchanged;
the Vulkan fix and host fixture are independently written, with no Metal or
game-derived shader payload copied.

| Upstream change / prerequisite | Android disposition and concrete path |
| --- | --- |
| Resource replacement changes argument identity even with unchanged guest bindings | **Applicable; ported the missing replacement notification.** `vk_res.c:img_upgrade_inner` swaps the existing owner's image/memory and retires all its views. Before this fix it did not change any cache generation. A preceding `vk_tex_by_id` hit therefore returned an old view in the same submission, while `vk_draw.c:record_stage` could reuse a frozen descriptor set because `key.resources = vk_treg_revision()` was unchanged. The upgrade now calls existing `tex_epoch_bump`, invalidating both the TLS ID cache and recorder resource identity. Existing descriptors already recorded for earlier draws remain alive through normal retirement; they are not mutated. |
| Serial generation production (upstream `serial_put`, `serial_del`, `ser_bump`) | **Existing equivalents retained.** `vk_res.c:mtl_reg_tex` publishes per-ID and registry revisions; `mtl_tex_release`, `vk_tex_pool_gone`, and `mtl_set_tex_pool_n` already bump texture/registry epochs. Mirrored reuploads ordinarily retain the same image; no separate serial-table implementation is needed. |
| Snapshot retirement changes uniform argument identity | **Already fixed, not ported again.** `nvn_vk.c:2741-2744` marks a retired snapshot dead and calls `ub_changed` for its whole captured range. `ub_changed` advances `g_ub_gen` and records an exact overlap journal (`1991-2008`). Recorder freezes check that revision and `record_uniforms_unchanged` checks each reflected UBO range (`vk_draw.c:2609-2620,2644,2662-2665`), so a retirement cannot be forgiven as unrelated. `snap_reset` also advances snapshot incarnation and invalidates all uniform ranges (`nvn_vk.c:2412-2413`). Protected-lag copy reuse checks the same range journal (`3438-3444`). |
| Legacy Metal dirty flags / argument encoders | **Not applicable as an implementation.** Non-recorder Vulkan `bind_stage` rebuilds resource infos per bind (`vk_draw.c:2760-2786,2815-2823`); descriptor-set reuse is keyed by resolved contents rather than guest binding identity. It still benefits from the corrected TLS image-view invalidation. No second dirty-generation system was introduced. |
| Five-mode Metal pixel probe | **Platform-specific; replaced by a Vulkan host regression.** `vktest --texture-replacement` realizes a real format-list image, caches its original view under two unchanged texture IDs, upgrades it through an out-of-list integer-format view, and asserts both recorder revision and exact new-view identity. It also checks no invalidation for an already-supported view. No claim that the five Metal modes exist on Android. |

The prerequisites are already provided by Android's epochs, snapshot retirement,
and ordered resource retirement; no additional upstream commit is required.
Default Android enables two recorder workers (`vk_record.c:809-816`), protected
frame lag (`runtime/frame_lag.h:7-14`), and UBWC format lists
(`vk_res.c:281-287,332-335`). An incompatible format view reaches
`view_get -> img_upgrade -> img_upgrade_inner` without an opt-in switch. This
establishes default-path eligibility, **not a claim that a device workload hit
it**. Each successful upgrade now logs the cumulative deterministic
`resource replacement invalidations N` count beside its serial. On Thor/Odin,
a nonzero count proves this exact replacement/invalidation path ran; no timings
or device performance claims are inferred.

Host validation (lavapipe Mesa 26.0.8, CPU affinity 0-3, nice 10):

- New test against unmodified production code: **11 checks, 3 failed** (resource
  freeze revision and both unchanged texture IDs), exit 1.
- With the fix: **11/11**, both inline and `TOTK_VK_REC_THREADS=2`; the latter
  enables the recorder and ordered asynchronous assembly/submission.
- Required ordinary suites: `vktest` **23/23**, `vkdrawtest` **100/100**,
  `vkdrawtest-app` **106/106**.
- Android NDK 28.2/API 33 `-fsyntax-only`: `vk_res.c` and `test_vk.c`, clean.

Outputs/logs are under `.scratch/upstream-argument-invalidation/`. No device,
adb, full Android build, or gameplay speedup test was performed. This change adds
no concurrent mutable authority or worker-side state; the existing atomically
published epoch remains the cross-thread cache identity.

## Translation-worker self-drain (orig c8587ed, 2026-10-26)

`hle_nvn.c` marks the translation worker with TLS before dispatching any job.
`x_drain` returns on that worker before giving up a guest core or taking the
queue mutex: the current job cannot wait for its own completion, and queued
later jobs cannot overtake its callback. Guest callers retain their normal
queue snapshot/wait, drain statistics, and core-release behavior. Android's
frame-lag synchronous-replay override and all resource retirement stay intact.
This covers submit, present and texture-retirement jobs, including the Vulkan
`mtl_pool_invalidate -> nvn_x_drain` callback; no renderer-specific bypass is
needed. Upstream shipped no regression test with this fix.

Run the deterministic production-worker host regression with:
`python3 android/gfx/test_translation_drain.py --scratch <scratch> --cc clang`.
It extracts the real worker/drain functions, exercises all three job kinds
with later work queued, and verifies a non-worker still waits for completion.

## 1.4.3 elapsed-time floor (orig ce7f0b3, 2026-10-26)

`runtime_frame_step_update` no longer raises each short present to 1/120 s.
The 1/15 s per-interval hitch cap, five-sample mean, invalid-gap reset and
pinned-step behavior are unchanged. The upstream fixture/verifier now checks
11 steady/jitter/hitch patterns (6000 presents each), as well as the existing
emulated production timing/cloth/bounce veneers. The verifier accepts
`--scratch`, `--source`, `--layout` and `--cc` for a CPU-capped Linux host run.
It uses the authenticated Darwin rewrite for instruction emulation, not an
Android runtime executable. Android resolution policy and expected patch sites
remain unchanged: with output/aspect off and no render-resolution override,
there are no resolution patch sites in this timing fixture. Upstream's later
four-site resolution expectation is deliberately not imported.

## Binary gate dispatch (orig a038e6b, 2026-10-26)

`loader.c:runtime_dispatch` uses a lower-bound search per loaded module rather
than scanning every gate. Both `scripts/prepare_runtime.py` and Android's
`fixture/make_fixture.py` sort gate return offsets and reject duplicates.
Timer/SVC behavior, module precedence and Android's monotonic guest clock are
unchanged. Existing generated runtime tables must be regenerated before using
the new loader; the Android build already regenerates them through
`generated_headers`. No scheduler, starvation or preemption policy changes.

Only the binary-search portion is ported. The spin-loop emitter/discovery,
structural/equivalence checks, spin gate ABI (`timer=2`), runtime handoff tracker,
BOTW 1.9.0 hash/registration and associated nxnative notice/test changes are
not imported: upstream registers only BOTW 1.9.0, not TOTK 1.4.3, so our
authenticated TOTK rewrite produces no spin gates. Adding an unused scheduler
handoff or enabling unmeasured TOTK loop rewriting is not this bounded port.
`tests/test_spin_waits.py` depends on that omitted emitter and is therefore
not installed/run. No upstream-only BOTW feature is silently claimed for TOTK.

Run `python3 android/gfx/test_gate_dispatch.py --scratch <scratch> --cc clang`
and optionally `--gates-dir <rewritten-dir>` to compare production dispatch
against the former linear lookup at every gate and unknown-address partition
boundary. The fixture also executes the two real generator parsing/sorting
blocks on shuffled and duplicate gate input, and exhausts a synthetic address
range with unloaded, empty and overlapping modules.

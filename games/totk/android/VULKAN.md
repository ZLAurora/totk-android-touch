# NVN on Vulkan: the plan

`android/gfx/nvn_vk.c` implements the replay interface `third_party/d2r_gfx/gfx/nvn/nvn_mtl.h`. The NVN layer
(`hle_nvn.c`) records the game's command buffers and hands them over. On the Mac, `nvn_mtl.m` (14k lines) plays them
on Metal. This file is its Vulkan counterpart, built in stages. Each stage runs the real game before the next one starts.

**Round-ten presentation controls:** development builds default to the full top-right overlay; `TOTK_OVERLAY=off`
disables it, while release builds (`--release` / `DEV_BUILD=0`) retain the off default. SGSR (`TOTK_POST_SCALE=sgsr`),
automatic 60/30 control (`TOTK_FPS_AUTO=1`, with `TOTK_FPS60=1`) and camera/depth frame generation
(`TOTK_FRAMEGEN=1`, fixed real 30 only) are opt-in; automatic control and frame generation are mutually exclusive.
Postprocessing precedes the final overlay on copy and AHB paths. Protected `TOTK_FRAME_LAG` remains off
when unset at runtime. The Android menu generates `TOTK_FRAME_LAG=1` only for Snapdragon 8 Elite-class
(`CQ8725S` / `SM8750`, Odin3) native60 fps without frame generation, including first-run and saved settings;
developer `totk.env` `=0` wins. All other chips/modes retain their prior policy. Resolved lag keeps the automatic
600-draw chunk threshold when native VMA pressure is absent (otherwise75); explicit chunk overrides still win.
Android shipping-30 mode (`TOTK_FPS60` unset/`0`) defaults `TOTK_GPU_TIME` to hardware; FPS60=1
and other platforms/BOTW retain latency. Explicit hardware/latency overrides remain effective;
unknown values log and retain latency. The latest Thor evidence supersedes the round 10.1 lag-on policy.
The open round-nine lag-plus-slab-driver shadow report still does not establish lag causation.
See `APP.md` for the complete policy and `UPSTREAM_MERGE.md` for the ledger.

**Native UI v2:** `TOTK_NATIVE_UI=1` is opt-in and 1.4.3-only. The guest canvas/crop stays 1280x720;
scene resolution/native DRS is unchanged. Exact registered window identity and its final texture-transfer draw
identify the UI canvas, not all same-sized post targets. Pre-UI composition stays 1x, then a single upscale seeds
a native host sibling at the first UI draw. Viewports/scissors/clears/copy rectangles scale there; game reads
resolve a 1x shadow, while the verified normalized final transfer samples native pixels. FragCoord.xy draws
log and fall back to 1x. Copy/AHB present native pixels directly, bypass post FSR/SGSR, and overlay last.
`TOTK_SCENE_AA=smaa` filters the learned pre-UI seed before optional scene SGSR/FSR; the legacy
`TOTK_POST_AA=smaa` still filters the composed output, including UI. Scene AA scratch (RG8 edges,
RGBA8 weights, scene-format neighborhood color) belongs to the native backing's deferred retirement.
The per-real-frame guard and retained source ring prevent midpoint AA/upscale; scene-AA-enabled QCOM/LSFG
capture filtered native real seeds before UI. The companion/presentation ownership protocol is unchanged. See `APP.md` for cost,
host fixture commands and required device qualification; no Odin throughput or menu/camera correctness is
inferred from the synthetic fixture. Native UI does not alter the frame defaults above.
Native shadow preparation is gated by promoted-storage descriptor bits and reflected/bound-slot masks;
unrelated draws do not realize textures or split ordinary 1x feedback passes. Shader family/coordinate/storage
classification is cached per module, and an open pass reuses its target/window identity. The owner's first v2
Thor test confirmed crisp HUD output but found a 1.7–3.0 ms recording regression; the fastpath repair requires
a new device comparison, not extrapolation from the `vknativeuitest --offcanvas-bench` host timing.
Native preservation work is elided only for a defining whole-image queued color load-clear (single color, no depth,
base subresource, unchanged format, single sample/level/layer and unpadded basis). Partial/alpha updates and game
reads still preserve prior pixels; partial resize paths retain resolve plus seed. Whole padded-window clears use
the original 1x allocation so guest-only padding remains defined. `native UI work per frame` reports actual and
full-clear-avoided operation counts every 300 frames; the pass profiler directly times seeds, shadow resolves,
patches, native UI/window passes, AHB/copy composition and optional host post-processing. Its unscoped residual
includes idle: do not attribute it to copies. No native/host timing scope introduces a barrier, wait or submission;
queries are emitted only on opt-in profiled frames, using the existing completed-fence reporting path.

## Opt-in frame generation v2

`TOTK_FRAMEGEN=1` remains fixed-real30/physical60Hz only and off by default.
Frame extraction and native UI consume one shared `vk_canvas` identity/boundary
module. Framegen keys current resources by image **incarnation** plus mip/layer/
aspect, and keys cross-frame scene history by accepted camera-source and stable
attachment format/extent/sample properties. Hash-indexed exact attachment groups
aggregate reopened/chunk-split passes; the four/five-MRT G-buffer variants share
one root only with the same live depth, compatible common attachments and camera.

The checked chain is G-buffer depth/camera → sampled lighting/post image versions
→ exact pre-UI canvas → committed terminal transfer → exact presented window.
Unrelated pre-scene resources no longer cause global ancestry failure. VS/FS and
binding-change contexts are observed without the old four-context/32-draw caps.
Missing/stale/ambiguous lineage, changed structure and bounded-table exhaustion
still refuse the frame, with a state-change reason and300-real-frame
`TOTK_FRAMEGEN_DIAG=1` eligible/generated/refusal/backoff counters, canvas identity/
size, transfer/first-UI/snapshot stages and last ancestry/taint-cause details.
The first v2 device run still generated0: dependency failures were mislabeled
boundary failures. UI-capable VS classification now exempts only post-boundary
exact-canvas UI, zero descriptors match real dummy inputs, and only overlapping
storage side effects poison proof. Load/transition and persistently refusing frames
run full extraction once per32 frames while shared canvas discovery stays live,
independently of native UI. An isolated capacity refusal instead skips one tick
and retries at the second tick; a consecutive failure restores the31-tick wait,
and a successful eligible probe resets that streak.
The next device run reached canvas/root/UI every frame but still refused: false
guest-range GPU alias invalidations tainted valid ancestry. GPU writes now follow
actual image incarnation/subresource, not overlapping guest addresses of independent
format images. Small mip/layer views now use exact subresource hash lookups when
cheaper than scanning every produced resource; large arrays still inspect only
produced versions. Both paths reserve their actual bounded traversal before work,
in addition to reflected descriptor traversal, against the unchanged32,768 budget.
No resource/proof capacity or retained history allocation is increased.
Failed probes checkpoint metadata without splitting the real pass, do not
force inline HUD recording and stop scanning immediately. State-change snapshot
check/taint details and per300 probe/backoff whole-walking-window CPU means/maxima
make the next device comparison attributable; they do not establish device success.

Under `TOTK_FRAMEGEN_DIAG=1` (off by default), `[framegen-binding]` separates
reflected descriptor entries from sampled-view traversal, reports the old
full-scan view-work estimate, raster-depth/raster-no-depth/compute classes, and
the busiest logical pass's frame/shape/extent. Its binding-walk thread CPU
total/max-context is only that ancestry walk, not whole tracking time; the
existing whole-walking-window probe/backoff CPU counters remain separate.
`[framegen-camera]` reports first-fired translation/rotation/projection/validation
rejections plus measured compared-pair rotation degrees and translation world
units (mean/max, valid-pose sample count, and all comparison count). Invalid
camera comparisons count as validation failures, never invented zero motion.
For one diagnostic raw-chain capture, also set `TOTK_FRAMEGEN_DUMP=3000`
and `TOTK_VK_SHOT_DIR=shots-fg-chain`. The first successfully recorded generated
real-frame ID at or after3000 writes `shots-fg-chain/framegen-NNNNNN/index.txt`
and native-format tightly packed raw images: both retained scenes/depths and UI
probes, composed canvas, authority fallback, replayed terminal, both adjacent
actual real terminals, and generated output. The index includes exact cameras,
transforms, extents/formats and write status; names distinguish generated versus
real parity. It requires DIAG, adds transfer-source usage only when enabled, and
explicitly flushes/waits once. Discard that timing window; this is not a benchmark.
There is no capture or added image usage by default.
Interpolation accepts smooth vertical-FOV changes up to3 degrees per real tick
and a1.10 maximum focal-scale ratio on both axes. For pure zoom this bounds
source-to-midpoint displacement to.025 UV at either edge, below the existing
shader's.04 UV motion limit. Each source uses its own exact inverse projection;
the common midpoint projection averages the two projection matrices, including
independent near/far. Projection interpolation is coefficient-wise, not an
arithmetic midpoint of FOV angles. Both source depths enter the same clip basis.
Aspect (declared and effective focal ratio), extent, projection-sign changes and
the existing off-center/jitter, translation and rotation cut guards still refuse;
extrapolation keeps its original strict projection gate. Existing depth/authority,
convergence and one-sided/fallback shader semantics are unchanged.
`[framegen-projection]` logs field and per-coefficient before/after/delta values
for at most the first two first-fired projection rejects per300-real DIAG window.
It identifies FOV/scale versus off-center/jitter versus depth/structural entries;
off-center frusta and temporal jitter cannot be distinguished from matrices alone.

Scene warp precedes UI in a private float canvas. The real UI pass is not split
per draw: its resolved pipelines, dynamic state, frozen descriptors, vertex/index
bindings and draw parameters are retained, then replayed once in one private UI
pass, including ordered attachment clears. A private depth/stencil copy prevents
duplicate writes to guest depth; full canvas overwrites and unrecorded copies refuse.
Readonly guest buffer bindings of a frozen UI/terminal packet now select
submission-owned immutable bytes at the original draw, even with frame lag off.
This includes ordered inline uniform updates and reflected readonly storage
uniforms; reflected extents bound the copy. Both the real draw and later private
replays bind that selected version. Retaining a descriptor set alone was not
enough: later updates could change its backing buffer before black/white replay.
The existing snapshot selector, reuse rules and arena retirement are shared;
there is no extra per-UI-draw render-pass split or GPU wait. A pending GPU-only
producer that cannot supply CPU-known bytes refuses generation rather than
reconstructing from stale guest memory.
Frame generation therefore also retains/seals/prunes the existing GPU-producer
journal with lag off, and GPU writes invalidate overlapping CPU update overlays.
This does not enable frame lag, private-update publication, or writer guards.
The game's terminal shader is replayed with only its canvas view replaced, so
encoding/transfer semantics are not guessed. No future guest-memory read and no
HUD difference/hold mask remains. Source and output lifetimes remain covered by
the real/generated retirement tickets; unsupported UI side effects/attachment
changes fail closed.
The full-pixel `vkframegentest --interp-static` oracle uses the observed
1280×720 R11G11B10 canvas and960×536 depth, identical real scenes and static
cameras. Its per-channel tolerance is derived from one native packed-float ULP
plus RGBA8 conversion rounding, not a scene-wide visual-error allowance.
`--interp-replay-buffers` mutates the original pool-backed UBO with an ordered
GPU update after the real draw. The live-handle negative must visibly diverge;
the production frozen-byte positive must preserve the real terminal at every
pixel. A separate pending GPU-only producer must refuse CPU snapshot selection.

The supplied Thor census has stable four/five-MRT shapes in all three full-world
frames despite changing group ordinals and4/6 physical splits. This does not
establish device generation. Host synthetic scene+128-UI-draw acceptance checks
opaque HUD identity after guest uniform/vertex mutation and correct translucent
recomposition; it keeps two real passes both off/on and adds one private warp,
one batched UI, one terminal and one presenter-output pass. The core tracker
microbenchmark measured about0.021–0.023ms for the128/64-pass model and0.198–0.234ms
at the2048/512 capacity model, not Adreno timings. The production recorder fixture
showed no measurable positive stable-binding overhead after guarding shared canvas
preparation by actual context changes; it has no valid game camera and cannot
prove <1ms on Thor. See `APP.md` for raw host measurements, reproduction and the
exact operator-run generated-count/fence-cadence/latency/video acceptance plan.

## Logical 32-lane subgroups (experimental)

`TOTK_VK_SUBGROUP_FIX=1` reconciles translated Maxwell subgroup operations with
the Vulkan device; default **off**, startup-only (device features and immutable
modules/pipelines cannot be switched live). Restart both arms and check the
`[nvn-vk] subgroup fix:` line plus each `subgroup shader` line for the actual size.

The backend requires Vulkan 1.3, where `VK_EXT_subgroup_size_control` is core.
It queries the native size, size-control range and required-size stages, enables
`subgroupSizeControl` only for this experiment, and pins affected stages to 32
when supported, otherwise 64 when supported. Geometry is never pinned.
No `ALLOW_VARYING_SUBGROUP_SIZE` or `REQUIRE_FULL_SUBGROUPS` stage flags are used:
unpinnable stages retain the reported fixed default. No pinning alone can fix
32-lane code on a 64/128-lane device.

The in-memory SPIR-V rewrite masks lane-id loads to 31, selects the current
32-lane word from ballots (zeroing the other three words), rebases absolute
shuffle targets to the physical lane's aligned 32-lane base, and lowers all/any
to predicate/active ballots within that group. This preserves partial/divergent
active masks rather than pretending that each invocation is a singleton.
Only the translator's observed ballot/all/any/shuffle and lane-id/size builtins
are accepted; other subgroup operations or a physical size outside 32/64/128
refuse the module with a diagnostic, never silently run an incorrect fallback.
Shaders without subgroup operations keep their original bytes and unpinned stages.
Seed files and the seed/reflection format do not change.

Run `android/inventory_subgroups.py <seed>/v143` on a loose seed to enumerate
actual operations, shader keys, variant names, stages and content hashes (unused
capabilities do not count; unused builtin declarations are separated from loads).
Add `--romfs <verified-romfs> --modules <rewritten/v143>`
for source-package names using the existing seed scanner. Output is local
game-derived data, not a shader bundle to commit. Package names identify likely
uses, not proof that a pass executes in any particular scene.

Host census of the prepared 1.4.3 seed: **38/24,751 modules actually use subgroup
operations**, across 30 raw keys (22 fragment, 11 vertex, 5 compute); one additional
vertex variant only declares an unused lane builtin and is not rewritten.
The operations are ballot (21 instructions), all (14), any (14), shuffle (8).
The 11 vertex shaders come from the tree package; remaining uses come from
system, application AGL, AGL common and gsys common packages. All 39 candidate
modules validate after rewriting. The local optimized/overlay seed has 42 actual
users/25,286 modules (the same 30 raw keys), plus the same unused declaration;
all 43 candidate modules validate after rewriting. This is shader inventory and
SPIR-V validation, not evidence of which shaders execute in a particular scene.

`tests/test_vk_subgroup.py` assembles a ballot/shuffle/vote compute fixture,
validates the rewritten module, executes its emitted instructions in a host
wave32/64/128 oracle, and checks Vulkan SSBO outputs with lavapipe. A lavapipe
device smaller than 32 only exercises partial active masks; it does not establish
Adreno wave64/128 execution. `make ... vksubgrouptest` builds the standalone
production-rewrite probe (`input.spv output.spv [gpu]`). Device acceptance is a
restart visual A/B focused on the inventory's passes, not an FPS claim.

## Offline SPIR-V optimization experiment

`android/optimize_shader_seed.py <seed>/v143 --output .scratch/seed-opt/v143`
creates a **new** seed; existing destinations and input/output overlaps are refused.
The script runs SPIRV-Tools `-O --preserve-bindings --preserve-interface
--preserve-spec-constants`, validates each input and output for Vulkan 1.2, and
checks descriptors (binding/count/type/target/extent), vertex/fragment interfaces,
execution modes (including EarlyFragmentTests) and specialization constants.
The original `.vk` guest-resource mappings/constant bank and `.vv` dependencies
are kept byte-for-byte. No layout rewriting, precision relaxation, spec-constant
freezing or debug-name stripping is performed. Gather-bias math stays in the
executable program; runtime Early-Z selection, descriptor-set patching and
DEPTH16 specialization nudge still run after loading the optimized module.

The current seed has an emitter omission in some formatless image stores.
Only the precise validator diagnostic requiring `StorageImageWriteWithoutFormat`
is legalized in the **copy**, by inserting that capability declaration; the
result must validate before optimization. All other failures stop publication.
`optimization.csv` records every module's byte/instruction counts, hashes,
validation and declaration legalization; `optimization.json` holds totals/tool
versions. `seed-id.txt` is published last, after the whole seed succeeds.

Host result (SPIRV-Tools v2026.1): **25,286/25,286 optimized modules valid**,
22 missing capability declarations legalized. SPIR-V bytes
1,467,886,936 → 578,371,760 (**60.60% smaller**); instruction count including
declarations/debug records 87,703,616 → 28,675,173 (**67.30% fewer**). This is
not a measured GPU speedup. Per-module evidence and the complete seed are in
`.scratch/seed-opt/v143`. One separately proven bogus inventory key,
`52e08b02c2d487a0d4658da225be44fbafe514525d9505eb04b9a90f97912b0d`,
was explicitly excluded via `--exclude-key` (and recorded in the report): its
SPIR-V was vertex but its reflection said fragment. The input seed was not
modified; all other modules were retained. No fp16 folder was generated.

Host acceptance: the production path/namespace/capability probe and dependency
matcher passed; real lavapipe `vkdrawtest` passed **98/98** with an alternate
13-module fixture and **98/98** with its optimized copy. The optimizer's
formatless-store/EarlyFragmentTests/specialization/interface fixture passed.
Android `cc1.sh` was clean for `vk_shader.c`, `vk_draw.c`, `nvn_vk.c` and the new
path probe. No device, screenshot or Adreno timing acceptance is claimed.

The existing switch is **`TOTK_SHADER_SEED_DIR`**, not `TOTK_SHADER_DIR`. Set it
to the actual directory containing `.spv/.vk/.vv`: the Android frontend resolves
relative paths against the **game folder** (the same base as the default seed),
before starting the guest, independently of later process working-directory
changes. Absolute paths are also accepted. Startup logs the resolved absolute
path; a missing/inaccessible directory or one containing no regular `.spv` files
emits one loud `shader seed error:` diagnostic before replay, rather than waiting
for variant misses. It is a complete replacement, **not an overlay**: missing
variants retain the normal missing-shader behavior. Selection is frozen for the
process. The default seed retains the **unchanged unsuffixed** pipeline-cache,
import and prewarm paths, preserving existing users' warm caches. This includes
the frontend's canonical `shaders/v143` (or `shaders/v0`) path, absolute paths
and symlink aliases resolving to that same directory, as well as the legacy
`shaders` fallback. Explicit cache/import path overrides remain unchanged too.
Only alternate seed folders have a SHA-256 suffix derived from their canonical
folder and optional `seed-id.txt`; their prewarm temporary files share that
namespace (`0` still disables cache/import). Keep each experiment immutable
while running; regenerate the content marker when rebuilding an optimized
folder. Unmarked alternate seeds are isolated by folder, not rehashed on every
startup. The default-path regression was corrected after Thor demonstrated a
cold first run with the initially unconditional namespace; host regression
coverage checks exact legacy names for default, versioned and canonical aliases.

Device A/B procedure (operator; no device access is performed by this change):

1. Copy the **whole** completed `.scratch/seed-opt/v143` next to the original on
   the device, e.g. `<game>/shaders-opt/v143`. Budget roughly the original
   680-MB transfer; this switch does not support partial/heaviest-N overlays.
   Keep the source `shaders` folder and all other environment options unchanged.
2. Baseline: leave `TOTK_SHADER_SEED_DIR` unset. Optimized: set
   `TOTK_SHADER_SEED_DIR=shaders-opt/v143`. Restart for every leg and check the
   logged `shader seed:` path. Use the same driver, clocks (Odin GPU cap about
   660 MHz), resolution, save, camera, weather/time and thermal starting state.
3. On Odin 3 / Adreno 830 use Gerudo with `TOTK_FPS60=1`; on Thor / Adreno 740
   use Lookout with its usual 30-fps settings. Set `TOTK_VK_GPU_PROFILE=1`.
   Warm the same route separately for each namespace until compilation/skips
   settle, then collect equal stationary and moving windows in A/B/B/A order.
   Compare total GPU ms and the expensive pass families' medians/tails; report
   draw counts, CPU/frame time and clock/thermal state alongside FPS. Do not
   compare one cold pipeline-cache leg with a warmed leg.
4. Capture matched screenshots and short motion sequences (alpha-tested foliage,
   shadows, depth ties, water, particles and UI), including Gerudo and Lookout.
   Reject missing draws, changed depth/alpha coverage, flicker or color artifacts.

Expected benefit is **unmeasured**: smaller SPIR-V can reduce seed I/O and driver
front-end work, but the Adreno compiler already optimizes, so steady-state GPU
time may not improve and register pressure/code scheduling can regress. Thor is
CPU-bound and may show no FPS change. No safe fp16 mode is provided: `.vv` records
only `fout=float/uint/sint`, not UNORM8 versus HDR target formats, so color-only
precision eligibility cannot be established. A future separately-namespaced
mode would require actual target-format proof and a def-use slice ending only
in UNORM8/sRGB color outputs, excluding depth/position, texture coordinates,
integer/bitcasts and any path to discard/alpha comparisons. RelaxedPrecision
permits reduced precision; it neither guarantees fp16 execution nor prevents
banding, threshold changes or accumulation artifacts.

### Indexed background seed IO and packed deployment

Indexed IO and packed deployment now default on. See
[APP.md](APP.md#packed-shader-io-and-learned-demand-warming) for production/install commands,
the exact `TOTK_SEED_STATS=1` fields and the repeat-route procedure.

`totk-seed-io` inventories the selected pack plus loose live-translation overlays at startup,
then asynchronously pre-reads the most recently used exact seed keys from the bounded,
namespaced `state/vk-seed-used.txt`. It attempts nice +10 and prefers a detected clearly smaller CPU tier
with at least two CPUs, otherwise retaining inherited affinity. Readiness joins metadata only, never the entire warm set.
Queued learned warming is canceled on demand and immediately read on the caller. Only an
already-running learned read is joined; simultaneous consumers share one read and copy only
when necessary for separate mutable ownership. Cold packed demand also uses immediate indexed
`pread`, independent of unrelated worker IO/history fsync, without pack mapping touches or
per-seed opens/stats/rescans. Worker creation failure retains the original inline loose path.

The default planned warm budget is learned indexed bytes (including terminators) +25%, rounded
up to MiB and clamped to 64–256 MiB. `TOTK_SEED_WARM_MB` explicitly overrides it (1–512 MiB).
No margin is preallocated. Consumption/publication/failure releases reservations.
Thor's measured 5,939-key set is 84.185 MiB and selects a 106-MiB cap, versus 64 MiB before.
Full learned payload residency can therefore rise about 20.185 MiB, plus allocator effects.
Graphics bind-time `.vv` and all-sibling payload speculation were removed after matched Thor
runs showed a seed-blocking regression: only 29 ready sibling hits against 107 joins and
9,246 queued; the learned set already exceeded 64 MiB. The compute worker is retained unchanged.

The runtime validates v1 header/magic/version, exact declared file size, full sorted unique
safe-name index, reserved fields, aligned/bounded non-overlapping payload ranges. Invalid,
truncated, incompatible and missing packs emit one fallback line. This is size/structure
validation, not cryptographic payload verification. Payload truncation after open fails a
`pread` rather than causing SIGBUS. Atomic replacement keeps the current descriptor's old inode
valid until the loader/recording readers drain at shutdown. On-device publication invalidates
cached loose/packed entries under the same mutex before releasing its translation epoch.
Shader/Vulkan object and program maps remain recording-owned.

Missing-shader requests still serialize into an owned memory stream on the walker; the IO worker
persists/starts translation, retaining the bounded 128-MiB queued-byte frontier.
MRU demand history snapshots are bounded to 16,384 records, coalesced for five seconds and atomically
published by the worker (plus shutdown), not fsynced on the recording thread.

`build_android.py` packs staged `.spv/.vk/.vv` by default, including runtime-only builds.
Generated `install_app.sh data` / `data-packed` and `push.sh [--packed]` transfer the pack and
unchanged seed marker via temporary device file + rename. Loose recovery remains explicitly
available (`--no-packed-shader-seed`, `data-loose`, `push.sh --loose`). An APK code update alone
does not install seeds. All seed/pack/cache bytes remain local game-derived outputs, never committed.

Recorder slab VMA names are now diagnostic-only: `TOTK_VK_REC_VMA_NAMES=1` restores `totk-rec-info/draw` names;
the default is **off** in both dev and release. The explicit old `prctl(PR_SET_VMA_ANON_NAME)` call was on walking
slab growth and takes the mmap write lock. This flag does not disable allocation, memory accounting, protection
or lifetime handling, and cannot suppress independent allocator/driver VMA naming. Storage/VMA diagnostic host
fixtures explicitly enable names so their owned-map checks remain meaningful.

Device A/B: preserve ClusterTune, driver/cache, seed bytes, route/caps and thermal state. Compare
metadata startup (`index=...ms`), asynchronous warm duration (`async=...ms`), memory, and recording
blocking separately. Initial installation has no learned history: play/learn once, then restart.
`warmed_hits` must have `warmed_storage_reads=0`; in-flight `warm_joins` are reported separately.
Never-seen/cap-skipped/reparsed keys can still read on demand. Do not interpret a warm miss as
missing geometry or promise a zero-IO first session. Validate scene pixels/live publication and
measure device p95/p99/long-frame tails: host suites do not establish a performance gain.

Historical opt-in implementation acceptance (before the current default/MRU design): inline, indexed loose and packed-only modes each passed Vulkan **23/23**, draw **100/100**, app
**106/106**, and pipeline **65/65** checks; indexed prediction passes **82/82**. Frozen spans pass 7,097 checks,
the recorder identity/boundary matrix passes, and explicitly named storage diagnostics pass 4,498/318 checks.
The IO unit test checks 1,000 distinct absent names without adding index entries, byte-exact 2-MiB demand beyond
a 1-MiB prefetch budget, detached mutable buffers, genuinely prefetched stale `.vv` invalidation, new publications
and request persistence in both loose/packed modes. Real missing-compute requests retain their full 32-byte code
and skip semantics with disabled/absent translators, with indexed IO off/on. Packed-reader comparisons match all
48 fixture files, malformed-pack loose fallback passes draw 100/100, and tooling/namespace Python tests pass
15/9/3/1 cases. All nine touched C files pass NDK r28.2 Android API 33 syntax-only compilation.

Host syscall traces show **zero seed pathname operations on the walker after its walking-thread identification**
in indexed loose and packed modes, and zero owned recorder VMA-name syscalls by default versus 16 with naming
enabled. This is not a blanket zero-filesystem assertion: the full harness still makes four Mesa driver disk-cache
opens during synchronous pipeline creation, plus four deliberate passdump write/read opens. Those are distinct
from seed lookups and are not hidden by disabling the driver cache or removing requested diagnostics.

### Formatless storage-image declaration repair

A complete read-only validation of the delivered seed found **22/25,287**
invalid modules, all requiring the missing `StorageImageWriteWithoutFormat`
capability. Example guest hashes: `183bab467cb19b35...`,
`1aaa5c83d776e776...`, `235b57c3b0928fb8...`; the complete diagnostics are in
`.scratch/seed-opt/source-validation.json`. The translator's SPIR-V image
load/store emitters did not declare formatless-image capabilities.
`tools/totk_xlate/patches/0010-formatless-storage-image-capabilities.patch`
tracks whether the actual image declaration uses `Unknown` format and declares
the read or write capability only when emitting that corresponding operation.

Vulkan device creation already enables both features when supported: it queries
`VkPhysicalDeviceFeatures2` and passes that same feature chain to `vkCreateDevice`
without clearing either `shaderStorageImageReadWithoutFormat` or
`shaderStorageImageWriteWithoutFormat`. The module loader now also refuses
declared capabilities 55/56 when the selected device lacks the corresponding
feature, rather than submitting such a module to Vulkan. This guard is specific
to these two capabilities, not a claim of complete device-capability validation.

The patched translator built with .NET 10 with zero warnings/errors; all **22**
affected guest shaders were retranslated from original read-only game inputs
into `.scratch/seed-opt/capability-retranslation`, then passed `spirv-val`
without legalization. The committed prebuilt translator and `build/` seed were
not replaced: publish the translator and regenerate the seed before deployment.

### AGL scanner false identities and stage consistency

`52e08b02...-b7530f698e76` originated in **PC seed preparation**, not a device
fill: its retained `_b.json` already says `stage=ps` but `msl_entry_kind=vertex`;
the delivered and prepared SPIR-V share the same inode/mtime and MD5
`4aeac7b5a1b1ab060de41a2cdde73845`. Both device push inventories contain those
same SPIR-V bytes; recent push logs changed its `.vk`, not its `.spv`.
Its source is `Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs/agl_technique_pfx.sharcb`.
The scanner interpreted bytes at **0x1a7d68** as a record with code offset 0,
control size 0x880 and relative control offset **0**: the supposed NVN control
is the record itself. Random fields at +0x704/+0x714 become length 1541 and
fragment stage, while code comes from the archive's first genuine vertex shader.
This is a false inventory identity, not a VS/FS hash collision or an identified
real program with a recoverable paired fragment stage.

Before publication checks, the runtime trusted `.vk` stage and did not compare
the SPIR-V execution model: this pair would ask Vulkan to use a vertex entry
point as a fragment stage, an invalid graphics pipeline. Driver failure is an
inference; this investigation did not execute that erroneous pair.

The scanner now rejects overlapping record/control intervals, while allowing
touching boundaries and shared controls. Complete old/new inventory comparison
finds **102** removed identities across seven archives: every one has control
start exactly equal to record start and selects code offset zero. **82** were
already failed translations in the original manifest; **20** produced one
default module each (19 vertex, one mislabeled fragment). All `.vv` references
are each key's own generated variant inventory, not cross-key program/use
evidence. The original population reconciles as **28,771 discovered unique
keys → 24,751 prepared variants + 536 added/fill variants = 25,287 delivered
modules**; the fixed scanner discovers **28,669** keys. Optimization omits only
the one explicit stage-mismatched key, not all 102.

Per-rejection archive digest, record/control/code ranges, declared stage,
Maxwell header stage, delivered models and references are recorded in
`.scratch/seed-opt/scanner-rejections.json`; population differences are in
`inventory-reconciliation.json`. The operator supplied read-only device request
snapshots: **55 Thor files (38 unique keys)** and **531 Odin files (452 unique
keys)**, including `done/`. **None** references any of the 102 rejected keys,
including the 20 delivered ones. Neither device has prewarm records. Request
absence is **not proof of never using an already-seeded shader**: installed
modules need no missing-shader request, and driver pipeline caches are opaque.
The structural overlap evidence, not request absence alone, justifies the rule.

Preparation now checks every newly produced or reused SPIR-V execution model
against its requested stage; Android reflection also rejects model/stage
disagreement. The translation service rejects a mismatched Metal entry kind
and reports it as a fatal stage mismatch: preparation fails rather than merely
counting a skipped shader. Regression coverage includes self/partial overlaps,
adjacent records, controls immediately after records, records immediately after
controls, wrong executable models and fatal service replies. Shader preparation
tests pass **19** with **7** Mac-only skips; reflection tests pass **22**.
The patched translator builds with zero diagnostics and rejects the original
bad request with `stage_mismatch=true`. No original build artifacts were changed.


## What the AYN Thor's drivers offer (android/vkprobe)

| | Qualcomm (stock, Dec 2023) | Turnip (Mesa 26.3, Balemuni build) |
| --- | --- | --- |
| Vulkan | 1.3.128 | 1.4.359 |
| Host-pointer import (`VK_EXT_external_memory_host`) | no | no |
| dma-buf export, mapped at a fixed address, GPU round trip | (rerun the probe) | **works** |
| dma-buf heap import | (rerun the probe) | **works** |
| BC textures | yes | yes |
| shaderInt64 / shaderFloat64 | no / no | yes / no |
| extended dynamic state 3, conditional rendering | no | yes |
| layer / viewport index output from the vertex stage | no | yes |

Turnip is the main target. The stock driver (Dec 2023) stops at startup on the Thor: it offers neither host-pointer
import, dma-buf export nor dma-buf heap import, so pools cannot be shared. `TOTK_VK_DRIVER` (or `driver.so` next to
`run.sh`, or in the app's game folder) picks the driver file. `android/gfx/vk_load.c` loads ICDs, plain libraries and
Android HAL modules (the format the emulator driver packs ship in).

Turnip builds compared in the world (Gerudo Desert, 180 s; ClusterTune "Large Underclock" caps, 2026-10-21):

| Driver | Standard mode (GPU ≥ 401 MHz) | Performance mode (GPU ≥ 550 MHz) |
| --- | --- | --- |
| Banners-Turnip v27.0.0 (Mesa main, 2026-10-21) | 27.1 fps | 32.7 fps, GPU 65 % busy |
| PurpleVK T26 (Mesa 26.1) | 25.7 fps | 31.7 fps, GPU 70 % busy (its timestamps under-report GPU time) |
| K11MCH1 v26.0.0 R8 / R8 Sysmem | 23.6 / 23.7 fps (before chunked submission) | — |
| K11MCH1 v26.0.0 R8 Gmem | 19.3 fps (before chunked submission) | — |

## Memory: zero-copy pools without host-pointer import

The game reads and writes NVN memory pools through its own pointers, so the GPU has to use the same pages. Metal and
`VK_EXT_external_memory_host` wrap the existing pages. The Adreno drivers can do neither, so the backend works the
other way round. When a pool is registered, the backend allocates device memory exported as a dma-buf, copies the
pool's bytes into it, and `mmap`s the dma-buf with `MAP_FIXED` over the pool's own address range. The game keeps its
pointers, and the GPU and CPU now share those pages. When the pool is finalized, the bytes are moved back into
ordinary shared memory with one `mremap`.

Virtual pools (`totk_vpool.c`) alias physical pool pages. For a dma-buf pool that alias is a second `mmap` of the same
dma-buf at an offset (`mtl_pool_alias`), because `mremap` cannot duplicate a dma-buf mapping. A GPU address inside a
virtual pool resolves through the alias table to the physical pool's buffer.

The indexed resolver and `TOTK_VK_OFF=span`'s alias-first scan agree on buffers and offsets for disjoint aliases and
nested own/view pools: an inner view is the same owner's buffer plus the same offset; CPU-only pools contribute no
span. Their available lengths may differ if the scan encounters a smaller view first, but both lengths stay inside the
same GPU backing. One real conflict was overlapping requests in a single `MapVirtual` batch (permitted by its
validation): `mmap` replaced the pages, while the alias table kept both old and new mappings. Registration now trims
or splits overwritten aliases, so both resolvers see disjoint ranges describing the last mapping. No hot lookup
algorithm or segment hint changed. Host differential/last-writer testing is in `.scratch/hitches/span_test.py`
(production registration, alias mutation, index and thread-local resolver extracted, Vulkan allocation/mmap stubbed):
2,432,816 queries, zero buffer/offset conflicts after the fix; 4,000 random alias overwrites also checked against a
last-writer page oracle. The baseline had 124,684 mapping conflicts before the fix. The 32,768 smaller-view available
length differences are safe and intentionally unchanged. Device regression measurement is pending.

`android/gfx/test_vk.c` checks all of this without the game: contents kept both ways, GPU copies and uniform updates
in order, CPU steps, events and counters after completion, many submissions in flight, aliases.

## Inline uniform updates: direct by default

Uniform renaming is **off by default**. `TOTK_VK_UB_RENAME=1` opts in; `TOTK_VK_UB_RENAME=0` or
`TOTK_VK_OFF=rename` keeps it off. Ordinary submit-time UBO snapshots require renaming, and
`TOTK_VK_OFF=snap` disables those optional snapshots independently. Protected frame lag has a separate mandatory
UBO lifetime path; it does not turn ordinary renaming on.

The direct path ends/drains rendering before `vkCmdUpdateBuffer`, with full memory barriers before and after the
transfer. Earlier draws read the old bytes and later draws read the update on the GPU timeline, even when recorder
workers are enabled. Already-recorded updates remain in the CPU overlay log until their fence completes: CPU-resolved
storage-buffer addresses (`RB_SBUF_UBO`) must see the update before GPU execution too. These flushed entries never
trigger descriptor renaming or another transfer; their generation changes also invalidate recorder uniform captures.
With protected `TOTK_FRAME_LAG=1` and renaming off, UBO-only updates instead remain in a CPU-known logical shadow.
Both inline drawing and recorder freezes call `vk_lag_uniform_read`: CPU-owned uniforms always use immutable
submission-arena descriptors, including cold uniforms that ordinary readonly lag selection would guard live.
The arena survives the **last game-submission fence**, not one ring slot: more than eight Vulkan chunk seals must
not recycle earlier descriptors. Fresh native capture supplies the base; `g_snap_seq` is its copied-update watermark,
distinct from an update's Vulkan materialization fence. Full and partial overlays apply in sequence order even after
chunk seals. No queued original-buffer update or writable lag guard is generated merely for a private UBO update.
`TOTK_VK_PREPARE_MASK=1` (default unset = off) skips the per-draw pre-render preparation (`vk_lag_uniform_prepare`) of a
consumed UBO register unless its bound range changed or an overlay/producer/mapping revision advanced since it was last
prepared; selection, snapshots and guards are unchanged. `TOTK_VK_OFF=prepare` forces it off; `TOTK_VK_PREPARE_CHECK=1`
runs every skipped preparation and dies on a missed materialization (correctness runs only). Startup logs the value;
`[prepare-mask]` counts preparations and skips per 300 presents. Host `--lag-walk-bench 6` (lag on, two recorders, 7
interleaved pairs): whole-walker ns/draw −3.2 % stable, −2.7 % churn, −5.5 % chunk stress (median per pair), 74 % fewer
preparations; compare walker ms/frame on the Odin. Live (`totk.ctl`): the next draw takes it, and a change dirties
every register.

Real non-UBO reads/writes/copies, CPU steps/readback and completion-side consumers materialize the relevant updates
in command order before original storage is consumed. Virtual aliases normalize to physical ownership. True GPU
copy/shader-writable producers revoke only the overlapping CPU authority (splitting partial entries), invalidate
snapshot/freeze memos, and retain live guarded descriptors; unknown GPU-produced bases are never captured as CPU data.
Remaining CPU-known UBO-only bytes publish with `nvn_dirty_lag_store` **after** the final `QueueSubmit2` and recorder
join, **before** synchronous `mtl_sub_commit` returns to the CPU producer and before registration-failure fallback waits.
A publication-ticket gate delays completion posts/`g_done` until CPU publication finishes; it does not delay the
independent GPU-only completion cursor needed by genuine alias waits. Protected lag already disables asynchronous
replay in `hle_nvn.c`. Earlier draws read only immutable private backing, so publication requires no pure-UBO reuse
wait and no queued GPU write can clobber a next-frame CPU rewrite. Real live/alias guards sharing the dirty unit are
still honored, and their submitted tickets can legitimately wait. Publication retires its logical entries immediately;
unit-level dirty changes never discard authoritative partial updates.

Opt-in renaming preserves inline updates across Vulkan chunk seals by comparing their sequence with the snapshot's
base, rather than discarding all fenced entries. This fixed the title's every-nine-frame glow dropout on Odin 3
(two 12-second recordings, zero dips), but **in-game emissive flicker remains with renaming enabled**. In a rainy
Lookout recording, the torch/horizon band had 14.1 isolated tile spikes per 100 frames with renaming, versus 0.8 with
renaming/snapshots off; sky lightning is not the same symptom. Direct updates are also faster in the measured
two-worker runs: Odin Gerudo 53.0 → 57.1 fps and Thor Lookout 21.4 → 26.9 fps. Retain the opt-in path for diagnosis,
not as the recommended setting. See `DEBUGGING.md` for the evidence and title/Lookout verification procedure.

## Frame pacing and CPU cost (measured on the Thor)

The game waits for its previous frame's GPU work before it starts the next one, so anything that delays the GPU's
start adds the whole GPU time to the frame. Measured standing in the Gerudo Desert (simpleperf, `stutter:` and
`per frame:` log lines; `.scratch`-style runs of 180 s after the load-in):

- **Fences, not a timeline semaphore.** Turnip on KGSL emulates timeline semaphores; signalling one in
  `vkQueueSubmit2` garbage-collects its points by waiting for the GPU, which blocked the render thread about 30 ms a
  frame. Each ring slot has a `VkFence`; the completion thread waits on it.
- **Chunked submission** (`TOTK_VK_CHUNK_DRAWS`): absent an explicit setting, the threshold is **75**
  with default-off lag, or **600** when resolved lag is on and native VMA pressure is absent, otherwise **75**.
  Explicit positive thresholds and `0` (disable automatic chunking) win. At a render-pass start, after that many draws, recorded work goes to the GPU
  while the rest of the frame is recorded. The once-only `[totk-gfx] frame policy: FPS60=… FRAME_LAG=… CHUNK_DRAWS=…`
  startup line records resolved values. One submission per frame made the frame recording + GPU time.
- **Early submit inside long passes** (`TOTK_VK_SPLIT_PASS_DRAWS=<n>`, default **0** = off): the chunk threshold is
  only checked when a pass opens, so a pass of hundreds of draws starts on the GPU only after it is fully recorded.
  With `n` set, the next draw after `n` draws in a still-open pass ends it, the reopen submits that part through the
  same chunk path, and the same targets reopen with `loadOp=LOAD`. Refused (counted) for passes with a `DONT_CARE`
  store, native/canvas UI, framegen extraction or pass diagnostics; nothing splits with chunking off (`CHUNK_DRAWS=0`).
  **Cost:** each split stores and reloads every attachment on a tiler (GMEM), so it stays off. Try `512`, then `256`;
  `[split-pass]` logs splits, extra passes, refused candidates, depth touches and LRZ refusals per 300 presents.
  Compare p95/GPU-busy overlap, not just fps. Live (`totk.ctl`, APP.md *Live switches*): each pass (part) takes the
  draw threshold when it opens. "Keep the GPU busy" enables `512`, defaulting on only for Snapdragon 8 Elite-class chips.
- **LRZ-safe splits** (`TOTK_VK_SPLIT_LRZ_SAFE=1`, default **on**, inert when pass splitting is off):
  live at the next split/depth-pass boundary; `0` restores the unsafe control. Color+depth scene passes keep their cuts,
  but depth LOAD after a command-buffer epoch change gets an exact one-texel GPU transfer write-back before rendering.
  Depth-only/masked regions defer cuts; unsupported/MSAA depth refuses further cuts in that game submission.
  A/B with splitting fixed at `512`: count isolated luminance dips in 600 recorded frames, plus fps/GPU ms and touches.
- **Attachment load elision** (`TOTK_VK_LOAD_ELIDE=1`, default **0**): at the next pass open, certify a straight-line
  SPIR-V `vec4(position.xy,0,1)` and an unconditional fragment output, then prove full-area triangle/quad coverage
  from ordered vertex bytes. Blending, masks, depth/stencil tests, feedback, unknown shaders or GPU-only vertices retain
  `LOAD`. `[load-elide]` counts proven/elided passes per 300 presents; compare GPU ms on identical scenes and pixels.
- **Narrow feedback splits** (`TOTK_VK_FEEDBACK_NARROW=1`, default **0**, live at the next pass open): defer only
  texture-only invalidations while sampled attachments remain unwritten. Earlier writes, current writes, later writes
  to previously sampled attachments, storage, aliases and unknown descriptors retain the split; the full end-of-pass
  barrier drains deferred invalidations before dependent reads. `[feedback-narrow]` counts net avoided ends per 300 presents.
- **NVN alpha rejection** (`TOTK_VK_ALPHA_TEST=1`, default **0**, live at the next draw): select a fragment variant
  for the NVN comparison and an immutable 16-byte reference uniform (reused by reference bits per submission).
  Alpha rejection runs after the shader and suppresses color/depth/stencil writes; alpha variants use late tests.
  `[alpha-test]` reports enabled and ignored draws every 300 presents **even while off**. Compare cutout/menu pixels,
  not fps; `vkrectest --alpha-clear` checks all comparisons with a checkerboard and inline/worker recording.
- **Pending clear before read** (`TOTK_VK_CLEAR_ON_READ=1`, default **0**, live at the next texture read): when a
  sampled/storage image overlaps a pending attachment clear, materialize the load-clear and end its pass before
  returning the read view, including descriptor-cache hits and storage aliases. Do not cancel the NVN write.
  `[clear-on-read] forced=N reason=first-read` counts resolutions per 300 presents. Compare menu backdrops;
  `vkrectest --alpha-clear` checks read-before-attach ordering with both live arms and aliases.
- **The uniform-update log** drops entries a newer update covers entirely (the game rewrites the same constant blocks
  many times a frame), and `ub_prune` returns at once unless a submission finished. `ub_overlaps` had been a quarter of
  the render thread.
- **ADPF**: the existing render-only session remains default-on (`TOTK_ADPF_TARGET_MS`, default 33.3).
  `TOTK_ADPF=0` disables it; `TOTK_ADPF=full` opts into a bounded critical-thread work group with
  30/60-profile targets and real-present work minus intentional acquire pacing. The rate hint is a
  separate opt-in (`TOTK_FRAME_RATE_HINT=1`). See APP.md's Android performance-hints section; no
  device-wide caps, governors or thermal settings are modified.
- **Lookups without locks.** Pool address lookups use a per-thread copy of the pool spans (rebuilt when their generation
  changes), and texture registry lookups a per-thread cache keyed by an epoch that every texture or pool change bumps;
  locks and atomics had been a third of the recording thread. Recording went 24.3 → 18.1 ms on Large + Standard.
- **Push descriptors: tried, not kept.** Pushing the fragment and compute sets (`VK_KHR_push_descriptor`) cut set writes
  from 3,260 to 2,000 a frame, but Turnip spends about as much per push: recording 24.2 → 24.1 ms and system power
  4.65 vs 4.58–4.67 W, inside the noise.
- **A heavy scene: Lookout Landing in the rain** (~5,500 draws a frame against Gerudo Town's 2,300). Profiled there,
  the recording thread spent 10.6 % in pool address lookups and 8.6 % in texture-kind lookups. The span lookup scanned
  back from its binary search while a prefix maximum of span ends passed the address; one large early pool kept that
  maximum high, so most lookups walked most of the ~1,700 spans. A segment index built when the pools change (the
  furthest-reaching span per elementary segment, a max-heap sweep) and a per-thread hint of the last segment make a
  lookup a check or a binary search; a host differential test against the old scan (60M lookups, random nested spans)
  returns the same span every time. The variant signature of each stage (`stage_sig`, which looked up every bound
  texture's format per draw) is kept until its inputs change. Smaller: the draw's pipeline entry and layout memoised
  behind a dirty flag, descriptor sets identified by two hashes instead of a memcmp of a retained copy, programs found
  through a cache by serial instead of a locked map. Lookout Landing: recording 44.1 → 32.3 ms, 17.3 → 20.2 fps (the
  game's own 20 fps mode there; GPU 32 ms at 401 MHz); Gerudo Town: recording 24.4 → 19.2 ms, system power
  4.66 → 4.51 W, steady 30.0 fps.
- **What a Lookout Landing frame waits for** (`TOTK_TIMELINE=<file>:<first frame>:<count>`, 120 frames, ClusterTune
  Large, one trace per row). In every traced frame the game's main thread waited for the submission the render thread
  had just made to finish on the GPU, and only then did the render thread submit the next frame: one submission in
  flight. A frame there is replay + the GPU work left after the replay ends + about 2.5 ms from the wait's end to the
  next submit; the game's own logic already runs alongside the replay. For 30 fps that chain has to fit in 33.3 ms
  (medians):

  | | replay | GPU after replay | to next submit | frame | fps |
  |---|---|---|---|---|---|
  | Standard mode (GPU 401 MHz) | 36.1 ms | 11.2 ms | 2.6 ms | 50.3 ms | 20.2, 4.32 W |
  | `TOTK_ASYNC_REPLAY=1` | 34.4 ms | 11.0 ms | 2.6 ms | 50.0 ms | 19.9, 4.38 W |
  | Performance mode (GPU 550 MHz) | 27.9 ms | 8.2 ms | 2.3 ms | 38.8 ms | 25.1, 5.04 W |

  The translation thread (`TOTK_ASYNC_REPLAY=1`, the iPhone default) found nothing to overlap with in this scene, so it
  stays off; it also turns off the recording-copy marks (see `copy_token_on`). Performance mode raises the GPU's minimum clock,
  and the replay got faster under it too (the A715 cap stayed at 1.79 GHz, so something else in the mode helps the CPU),
  but the chain stays about 5 ms over, for 16 % more power.
- **Current Thor default decision (2026-10-26):** lag off plus hardware GPU reports reaches 29.93 fps
  at Lookout (minimum 29.6, no windows below 29.5, 1.4% long frames), compared with lag on plus latency
  at 27.71 fps (minimum 22.6, 10 low windows, 13.2% long). Latency reports overstated GPU time by
  roughly 9–12 ms; hardware's roughly 23 ms agrees with KGSL's roughly 22.7 ms. The older lag
  measurements below used latency reports and do not justify default-on lag on Thor. The Android menu's
  Odin native60-only lag default uses newer capped cross-device evidence (APP.md); runtime unset stays off.
  Its protection and the600/75 chunk rule are unchanged. See `docs/translation-research.md`.
- **One frame late: `TOTK_FRAME_LAG=1`.** The frame end waits two syncs for the frame just submitted (the
  frame-end sync, then one of a pair at main+0x2db998); with the switch both wait for the previous frame's fence, so the
  next submission records while the GPU finishes the last one (at most two frames in flight). The game's own no-wait
  mode (request byte +0x1dc = 2) is no substitute: it also skips the call that starts the next frame and rendering stops.
  Pools are still finalised only with the GPU idle and images retire after their last submission, so nothing the GPU
  reads is freed early. Lookout Landing, 90 s each on Large + Standard: 20.5 → **30.0 fps** (worst 300-frame window
  30.0, none of 300 frames over 50 ms) at 4.29 → 5.41 W, the governors raising the GPU to 550 MHz and the replay clock
  on their own; Gerudo Town (already 30) 4.39 vs 4.47 W; the Depths 30.0 fps with no flicker. Later 120 s moving
  Lookout sweeps reported 2–12 dark/black frames in the lag run versus 0–1 in the no-lag run. This was an
  unexplained observation from a round-nine lag-plus-slab-driver run; it did not establish that lag caused the
  report, and is not a present recommendation to keep lag off. Those measurements predate the protection below.
  A later 120 s Thor Lookout test measured lag off **25.6 fps**, unprotected lag
  **29.9 fps**, and eager capture **28.5 fps**: capture copied 79.257 MiB in 7.140 ms/frame, with recording rising
  from 21.5 to 27.6 ms. The unprotected run did not reproduce an in-game dark flash that time, so this is a cost
  measurement, not proof of a device glitch fix.
  The following `266e694` wait-mode test measured **28.4 fps**, recording 26.5 ms, with 124.2 MiB and about
  730 merged guard ranges/frame taking 4.05–4.10 ms setup; actual reuse caused 0.907 fault waits plus 0.010–0.013
  host/contended waits/frame and 2.5–3.0 ms waiting. It had no in-game flash or crash in that 120 s sweep, but still
  cost about as much fps as eager copies. A later device run of sticky/hot commit `8687ace` measured
  **3.2 ms/frame** of walking-thread registration with **1,870 armed-ticket updates/frame**. Moving registration
  to recorder workers is a separate change, not part of round four; flash acceptance still requires the sweep below.
- **Lagged guest-read lifetime.** Resolved `TOTK_FRAME_LAG=1` enables protection; behavior with lag resolved off is unchanged.
  `TOTK_VK_LAG_CAPTURE=0` restores the completely unprotected control. `TOTK_VK_LAG_MODE` selects:
  - **`wait` (default):** keep cold GPU inputs live, with bounded learned-hot readonly inputs privately copied.
    Walking/recording captures storage/pointer, vertex/index, buffer-copy/upload, linear-pitch upload and buffer-texture
    read ranges, plus shader-writable/GPU copy/update destinations. At the actual chunk submission, its sealed owner
    merges the captured dirty-unit identities and registers them before QueueSubmit
    (64 KiB by default on Android, existing `TOTK_DIRTY_UNIT_KB` / `TOTK_DIRTY_UNIT` controls). Duplicate units already
    armed for that ticket return after an acquire load, without another CAS or global counter update. Original and registered
    virtual/physical aliases are protected. Only a CPU store or host `dm_guest_write_prepare` reusing a protected unit
    waits for its newest GPU fence; unchanged input has no per-frame byte copy. Native GPU reads/writes ignore CPU
    page protection and remain ordered on their original buffers. Untracked read-only input falls back to an eager
    copy; untracked writable input or a known untracked pool alias instead waits for its containing submission,
    with an explicit diagnostic.
  - **`copy`:** the previous eager mode, retained for A/B/fallback. Freeze CPU-owned read-only inputs in the
    submission arena; shader-writable/results stay live and overlapping in-flight GPU writes are not snapshotted.
    Copies survive the game submission's final GPU fence, including all chunk submissions.

  **Sticky protection and hybrid hot rings.** Once an entire dirty unit is read-only, subsequent submissions update
  only its combined atomic state/newest fence ticket: no per-page scan or `mprotect`. A CPU fault, host unprotection
  or temporary writeback takes the same unit ownership; only genuinely unprotected units need re-arming.
  Alias-range projections are cached until pool mappings change, rather than re-scanning every alias every frame.
  Cold/hot selection is memoized per chunk and pool-map generation; a cached cold decision conservatively stays guarded.
  Exact binding selections also retain actual submission/ticket, dirty-range lifetime, snapshot generation and
  alignment. Native-backed private-copy reuse additionally requires unchanged content revisions and successful
  write watching of the physical owner **and every known alias**. Watching starts before bytes are read, with a
  dirty-token check after capture; untracked or unstable copies remain usable once but never seed a freeze memo.
  Explicit submit snapshots instead retain their immutable old-frame base across later CPU reuse, with ordered
  logical overlays. A watched child buffer does not certify an untracked `RB_SBUF_UBO` parent pointer: its bytes
  must independently come from an owned snapshot, complete logical overlays or stable watched native memory.
  Shader reflection records whether any storage binding can write, avoiding empty writable scans. Actual writable
  bindings still invalidate content on every draw, before any readonly capture or no-update early return.
  `TOTK_VK_LAG_TIMING=1` (default 0) adds per-binding selection/list timing. Without it, `lag guard:` ms measures chunk
  registration only; copy time is always measured separately. `range registrations` / MiB now include overlapping
  input registrations, unlike the old merged-range count. Ticket-update counts count actual increasing unit tickets.

  **Worker registration contract.** `nvn_dirty_lag_register(addr,len,ticket,reason,original_size)` is thread-safe and
  atomically retains the maximum ticket, with load-only same-ticket deduplication. It touches no walking-thread
  vectors, alias caches, snapshot arenas or Vulkan state. Callers must retain immutable pool identities and mappings
  until workers finish and GPU reads complete. Recorder workers use an explicit packet ticket and frozen span
  inventory, projecting resolved buffer-offset guard intents through **every** matching span (including physical-owner
  VAs, not only the winning resolution segment). An immutable buffer-to-alias index avoids scanning unrelated spans.
  Native recording jobs collect reusable dirty-unit batches without arming; the actual-ticket merge deduplicates
  range-relative units across jobs and combines diagnostic reason bits/max extents. Retained unit identities include
  the range-slot generation, so remove/re-add cannot protect the wrong lifetime. They do **not** arm future tickets
  while submission walking can still perform a CPU fallback, texture writeback or source store. After walking,
  ordered-primary assembly and command-buffer finalization, at most the active worker count of disjoint registration
  shards arms the unique units and joins before `QueueSubmit2`. Primary transfers/uploads obey the same boundary.
  A failed registration forces completion of that containing submission **after** `QueueSubmit2`, never during
  worker join. An explicit-ticket registrar returns failure for **any** BUSY unit, including a sampled completed
  ticket: waiting for its gate can otherwise follow a later owner holding an unsubmitted ticket. It must not
  sleep behind that ownership handoff while queue submission is waiting for its worker to join.
  Hot selection and private copies remain walking-thread operations. The software-GPU path retains late writable
  exclusion and merging; software shader-written host VAs are not armed by recorder workers.
  Repeated GPU-writer deduplication keys both the selected canonical donor VA and alias-graph generation,
  not only raw VA/extent/submission. A same-serial remap must index the new donor, and an alias join must
  refresh frozen guard projection even with an unchanged donor. Keeping the donor separately also covers
  a remap completing between canonicalization and the generation sample; no guard is armed earlier.

  Lag ticket and unit-gate waits exceeding 500 ms emit one `[dirty-lag-wait]` record per wait episode, including
  waiting TID and scoped runtime role plus OS thread name, unit VA/extent, captured ticket, GPU completion and latest
  submitted ticket, plus gate and submission state. An `unsubmitted` ticket identifies a future-ticket dependency;
  `submitted` distinguishes a GPU/completion stall. Timed futex wakeups permit the diagnostic even when nothing else
  wakes the waiter; they never abandon protection or release ownership. Fault-path formatting is stack-only and uses
  raw writes/syscalls.

  `TOTK_VK_LAG_HYBRID=0` disables adaptive copies for a sticky-only A/B; the default is 1 in protected wait mode.
  Two distinct writing frames among the last eight promote a unit. Readonly bindings touching a hot unit use the
  existing submission snapshot arena, retaining static/explicit extent bounds and uniform overlays. Captured units
  stay readonly write-watched without extending the private-copy submission's GPU ticket, so continued CPU rewrites
  remain observable rather than falsely appearing quiet. Eight quiet frames demote the unit back to live guards.
  `TOTK_VK_LAG_HOT_MAX_KB` limits each adaptive copy (default 256 KiB); larger or GPU-produced/writable ranges remain
  live and guarded, never truncated. Another live/writable binding sharing a hot unit can still require a wait.
  `copy` mode and lag-off behavior do not use adaptive learning.

  The dirty-fault/pre-write path uses only lock-free atomics, the clock and Linux futex syscalls (no allocation,
  pthread locks or Vulkan). A GPU-only fence cursor advances independently of CPU post-op delivery: an earlier
  post-op may itself wait on a unit a later GPU submission reads. One completion cursor would deadlock. Atomic unit
  ownership also prevents a next submit publishing a future ticket to a writer it is waiting to re-arm.
  Completion counter/event stores retain unit ownership through the actual host C store, without token allocation;
  a prepare-before-memcpy alone leaves a re-arm window and puts sanitizer instrumentation before the real fault.
  Lag metadata is per protection unit and allocated only in wait mode; the existing dirty-state arrays are unchanged
  with lag off. Lavapipe's software GPU stores through the host-import CPU VA itself, so its GPU-written units are
  excluded from guards to avoid faulting its GPU thread on its own fence. That limitation is specific to software
  GPU host testing, not the native GPU guard policy.

  Both modes ignore `TOTK_ASYNC_REPLAY=1`: pointer-derived inputs must be captured/registered before submit returns.
  This makes CPU replay synchronous, **not** GPU execution; chunk submissions and the previous-frame wait still
  overlap. Recorder freeze memos include the actual Vulkan submission serial, so live read ranges are registered
  again for every chunk. Any captured packet (copy mode or a learned-hot wait-mode range) carries immutable
  descriptor/vertex/index overrides; workers neither
  call the lag helper nor re-resolve captured addresses. Writable storage is marked before any readonly capture
  and never frozen as an input copy. Uniform snapshots/inline overlays remain in place; block-linear uploads,
  generated constants and widened 8-bit indices already copy into fence-protected memory. Indirect draw/dispatch
  APIs are unimplemented stubs, not another live guest-read path.

  Recorder draw packets are reused after ring fences. Each used vertex stream must clear its old immutable
  buffer/offset override before the current lag decision: a cold or GPU-produced live range returns zero and
  must not inherit a prior hot/copy handle. The first round-four device build (`bf84552`) violated that rule,
  producing lighting flashes and a recorder+protected-lag GPU-time regression. The host regression now wraps
  all ring slots with captured streams, then verifies actual worker bindings and pixels for GPU-produced live
  streams. Snapshot/copy byte and range totals also match recorder threads 0/2/3 in wait/copy/sticky-only runs.
  The `c1ca4c5` device rerun restored Odin Gerudo GPU work to **8.1 ms** with no in-game flashes.
  Protected-lag walking work still missed the 16.7 ms FPS60 budget (Gerudo recording 17.3 ms; Lookout 30.6 ms);
  round five moves registration to workers and addresses direct-update uniform reuse independently.

  Unknown-size storage bindings now use a complete reflected static SPIR-V block extent when known (member offsets,
  fixed-array and matrix strides), otherwise the smallest containing registered pool/sub-pool, including alias mapping
  bounds, then the existing Vulkan descriptor limit. Explicit NVN binding sizes remain bounds too.
  Runtime arrays, unsupported layouts and partial
  prefixes remain **unknown**, not an invented 64 KiB bound. `.vk` binding lines accept an optional eleventh extent
  value; legacy seeds remain valid with extent zero. Regenerate reflection metadata from retained SPIR-V to obtain
  static extents for existing seeds; `res.json` itself provides pointer locations/flags, not complete storage sizes.

  Host regression holds GPU execution at an event, submits, and writes source/light/vertex/index bytes on a separate
  CPU thread before opening the gate. Unprotected copies read 256 overwritten bytes and four draw hazards go black
  or retain only the blue clear; both protection modes retain submit-time data. Wait mode additionally checks sticky
  ticket-only updates versus unchanged `mprotect` counts, re-arming after reuse, two-frame hot promotion, bounded
  hot copies without a reuse wait, seven/eight quiet-frame hysteresis, top-unit diagnostics, host pre-write, ordered
  GPU-produced sources and both completion/publication deadlock cases. The guest-write fixture starts with a native
  byte store before its fully instrumented memset: this materializes the real fault ownership before libc/TSan's
  pre-store annotation, without suppressions or instrumentation exclusions. `test_vk_reflect.py` covers layouts.
  In the four-frame learning fixture, hybrid made one 256-byte private copy (64 bytes/frame average): setup was
  0.010 ms/frame, with 0.75 reuse waits/frame and 0.035 ms/frame waiting in that run. Sticky-only made no copies:
  0.017 ms setup, one deliberately forced wait/frame and 0.112 ms/frame waiting. These tiny synthetic timings do
  not predict the game workload; the meaningful checks are unchanged mprotect/ticket-update counts on duplicate
  tickets, concurrent maximum-ticket publication, re-arming after a write, promotion to a correct wait-free private
  copy, and demotion back to safe live guards. The protocol fixture also prints registration ns/call.

  `vkdrawtest --lag-uniform` adds the rename-OFF UBO regression. Run it in fresh processes with
  `TOTK_FRAME_LAG=1`, `TOTK_VK_REC_THREADS=0/2/3`, and both default-off renaming and explicit `TOTK_VK_OFF=rename`.
  With the existing synthetic seed it holds execution behind an event, rewrites the same full/partial-updated UBO
  before opening the gate, checks exact pixels plus zero UBO fault/host waits, and checks that newer native CPU writes
  and untouched neighbours survive GPU completion. It also performs twelve actual seals in one game submission,
  copies an early immutable descriptor's complete extent after ring wrap, exercises ordered virtual non-UBO alias
  materialization, a real GPU-copy producer followed by a uniform alias, and a real writable-storage alias.
  All 189 checks pass normally and with TSan at threads 0/2/3, in wait/copy/sticky-only modes and with the explicit
  OFF=rename override. This is host correctness evidence, not device visual/performance acceptance.
  `vkdrawtest --atlas-uv-race` is a deliberately early UBO-reuse probe, not an ordinary lag-off frame.
  It submits a draw behind a GPU event, then overwrites the UV uniform before waiting for its fence;
  the game normally waits for that fence with lag off. Keep the exact submit-time pixel assertion
  (`192,192,128,255`): protected lag must pass it. With lag off and UBO renaming unset/off,
  `snapshot_ubos_inner` returns without a snapshot, so the next frame's UV bytes can reach the queued draw.
  That unprotected reuse is the negative control, regardless of recorder count or the Android default.
  Use explicit `TOTK_FRAME_LAG=1 TOTK_VK_LAG_CAPTURE=0` with renaming off for an unambiguous disabled-protection
  control; do not relabel its reused-UV pixel (`0,0,128,255`) as success or weaken the check when defaults change.
  `vktest` additionally forces native guard policy on isolated readonly lavapipe input (never on shader-written
  host VAs), covering primary guard batches and a real secondary recording worker. After assembly it performs an
  unconditional walking-thread source store before queue submission, then proves later guest/host reuse is protected
  by an actual GPU event. A 10-second process watchdog bounds the deadlock. Scratch-only restoration of early arming
  fails at the walking store on threads 2/3 normally and under TSan, with `ticket_state=unsubmitted`; the repaired
  ordering passes, and its deliberate 600 ms post-submit hold reports `ticket_state=submitted` once.
  Its ownership-handoff case pauses a real registrar after sampling a completed BUSY owner, releases that owner,
  arms the actual future ticket and lets a guest fault claim it before resuming registration. Assertions require
  nonblocking registration failure, the real **postqueue** fallback (not discarded protection), correct queued read
  bytes and release of the guest only on genuine GPU completion. This distinguishes the sampled-completed-ticket
  race from the earlier walking-store timing case.
  Fresh-process `vktest --lag-bench` measures the real dirty layer with 7,000 overlapping input ranges, 2,000
  unique dirty units/pools, 230 collection batches and `TOTK_VK_REC_THREADS=2/3` arm shards. Cold allocation/rearm,
  warm ticket-update and equal-ticket duplicate phases are reported separately. Compare legacy ns/input range
  with batch collection, merge, arm ns/unique unit and observed release/join wall time; modeled critical CPU is
  labeled separately, not a Vulkan/device frame-time claim.

  Device acceptance: identical warmed Thor Lookout save/weather, Turnip/cache/resolution/power and recorder/write-back
  settings; alternate at least three 120 s `MOVE=1 TOTK_VK_BLACK=1` sweeps: lag off; lag with `LAG_CAPTURE=0`; lag
  with `LAG_MODE=wait LAG_HYBRID=0`; lag with `LAG_MODE=wait LAG_HYBRID=1`; and `LAG_MODE=copy` (all shorthand
  names prefixed `TOTK_VK_`). Count `dark flash` lines and review flagged frames. Retain fps/stutter/power plus
  `lag capture:` copy MiB/ms; `lag guard:` setup and reuse waits/ms; `lag sticky:` re-arms, ticket-only updates,
  hot-copied ranges/KiB and promotions/demotions. Every 300 frames, `lag fault unit <address>+<size>` lists the top
  eight writing units with writes, waits, last writing frame and hot state, plus `guard reasons` and `max range`.
  Reasons are interval OR bits: readonly=1, cold/learning=2, over-cap=4, GPU-produced=8, writable=16, alias=32.
  A readonly hot copy can still share a unit with a guarded writable or oversized binding. The old Thor addresses
  alone do not identify which reason applies: inspect the new lines for the two dominant waited units before changing
  `LAG_HOT_MAX_KB`. For a readonly over-cap unit, compare one bounded cap increase against added copy MiB/ms; do not
  truncate an unknown shader descriptor to a dirty subrange, which would leave legal late reads unprotected.
  Watch untracked or failed-rearm fallback diagnostics. Require no excess lag-specific flashes and most of the
  ~29.9 fps gain. Repeat Gerudo, Depths/terrain streaming and Odin 3. The prior sticky/hybrid Thor sweep reported
  28.4 fps, zero flashes/crashes, 3.173 ms guard setup and 1.017 ms reuse waits/frame; this registration optimization
  still needs the same device sweep. Unknown extents, coarse false sharing, alias history and signal/driver interaction remain risks.
- **Measure the build you meant to.** A build started in the same batch as the merge it depended on copied the sources
  before the merge and measured the old code (an unchanged profile gave it away); check the new symbols in the APK.
- **Avoid repeated recording work.** Profiled again at Lookout Landing, 69 % of `resolve_local` was still its binary
  search: a draw's uniform/storage (62 % of lookups), vertex/index (28 %) and pre-touch (7 %) addresses evicted the one
  segment hint. 64 page-hashed hints now keep them warm (every hit still checks the segment bounds and the range; a
  pool change clears them; `TOTK_VK_LOOKUP_STATS=1` logs lookups per frame and the hit rate). Generated support/driver
  blocks are refilled and compared only when their exact inputs changed, and uniform snapshots no later inline update
  touched skip the update-log search. Results are identical (host differential tests: 120M lookups, 750k blocks, 1M
  snapshot reads). Recording: Gerudo Town 19.2 → 17.0 ms (4.51 → 4.39 W), Lookout Landing 33.1 → 32.1 ms.

On the Thor's own scheduling: core_ctl keeps the X3 paused, so the render thread runs on the A715 cluster; its cap
(ClusterTune profiles) and the GPU's minimum clock (AYN performance mode: 401 MHz Standard, 550 Performance) decide
where the frame rate ends up.

## Zero-copy presentation (Android default with copy fallback)

`TOTK_VK_PRESENT=ahb` renders the final crop/scale straight into a ring of three imported Android hardware buffers.
A SurfaceControl child presents them; the host-visible GPU readback, cache invalidation and CPU window copy disappear.
This stays on the directly loaded Turnip device, rather than mixing its handles with the platform loader's swapchain.
Android defaults to `ahb`; `TOTK_VK_PRESENT=copy` selects the existing `ANativeWindow_lock` presenter explicitly.
Missing extensions, explicit-layout import or SurfaceControl support automatically retain that copy fallback.
By default both keep `TOTK_APP_WIDTH` and the window's aspect ratio fixed across crop changes, with black bars.
Host spatial scaling and the explicit native-UI profile instead select window-sized buffers as documented in `APP.md`.
The AHB UNORM image has an sRGB attachment view where required; sampling and attachment conversion preserve the
old blit's sRGB decode/encode, with opaque alpha.
The AHB's native dma-buf is imported with an explicit LINEAR DRM modifier and gralloc's byte stride; the AHB memory
handle path is not used because this Turnip revision replaces caller tiling/modifiers with deferred gralloc metadata.
The VNDK native-handle accessor is resolved dynamically; unavailable access or unsupported explicit layout uses copy.
See `APP.md`, "The picture", for source citations and layout guards.
When the performance overlay is enabled, the imported image also retains `TRANSFER_DST` usage, including in the
explicit-LINEAR image-format capability query. Both presenters run optional SMAA/FSR before drawing the output-space
overlay; readback or the final AHB `GENERAL`/foreign-queue release follows the overlay. Round-seven host pixel tests
cover the copy presenter across every overlay position and AA/scale combination, not Android gralloc or SurfaceControl.
The same fixture also records the overlay into standalone GPU images at every quarter-turn, including repeated
staging-slot/angle reuse and clipped small panels, comparing pixels rotated back against the consumed panel's
identity raster. These exercise raster strides and GPU transfer regions, not Android AHB import or HWC admission.

`TOTK_PRESENT_PREROTATE=1` is an off-by-default AHB experiment for rotated panels.
The packaged `TotkActivity` queries the actual attached root's public
[`getBufferTransformHint()`](https://developer.android.com/reference/android/view/AttachedSurfaceControl#getBufferTransformHint())
and listens for changes (API32+). It starts only after the existing explicit parent attachment succeeds:
API34 and earlier can crash if this API is queried before a root surface exists. Missing Java activity/root/API,
unknown flags and mirror hints retain the ordinary identity path; no device-name or display-number allowlist is used.
An identity hint, including a landscape-native panel at its natural orientation, does not rotate or swap dimensions.
For clockwise hints 90/180/270, the existing final sampling pass rotates the game/post/native-UI composite into an
AHB of respectively `h x w` / `w x h` / `h x w`, and `setGeometry` applies the inverse 270/180/90 buffer transform.
The source crop uses actual AHB dimensions; destination and aspect-fit remain in logical-window coordinates.
The compositor can therefore combine display rotation with the inverse buffer rotation to obtain identity;
that is an optimization opportunity, **not** proof of HWC admission or a guaranteed latency/power saving.
Each queued real/generated picture retains its own orientation, and hint changes (including same-sized 180-degree
changes) invalidate older window generations. The small performance-overlay cache rasterizes changed cells with
rotated strides and rotates its existing transfer regions; there is no extra full-picture pass or staging copy.
The game's native UI is already part of `vk_tex_native_output`'s sampled image, not a separate Android layer.
The Java companion remains an independent `Presentation`/`CompanionView` on its own logical display and is unchanged.

The recorder-boundary adaptive scheduler snapshots these same real-frame rotation/UBWC choices before
recording the complete private generated AHB pass. The pass uses the rotated native allocation/render area,
rotated viewport/scissor and overlay, while its queued logical extent and inverse SurfaceControl geometry stay
paired. Preparation does not end a live guest render pass, flush/touch live uniforms, or open native GPU scopes.
AHB foreign ownership is published only after a successful covering submission, never on prepare/cancel.
Hint changes cancel scheduler metadata and wake adaptive presenter/retirement waiters; the recorder still owns
GPU-command/source leases and retires them at its existing boundary. No additional submission lane is introduced.
Both options remain unset/`0` by default; the scheduler comparison uses common
`TOTK_PRESENT_PREROTATE=1` and `TOTK_PRESENT_UBWC=0`, not the separate UBWC experiment.

This prerequisite is transplanted from Aurelio Bernal's signed
`f9dfec41102d1d465eecf3c04e701a5e9906b9df`, with the rotated-overlay fixture from
`64ced2d9f648afac5ea99b0b687d29b7c2c01d0c` and only the corrected importer allowlist from
`65b5e6c1644a4ab10eea9604aeda8fadc7d5bec0`. Diagnostic hooks, build changes and the descendant PSO
prewarm/starvation floor are not included.

The new scheduler selector's startup cache is atomic because UI lifecycle and renderer boundaries may
query it concurrently. Absent/`0` short-circuits before frame-generation configuration; exact `1` and invalid
request handling retain the existing selector semantics. Lock order remains `g_win_mu` then `display_mu` for
presenter clock copies: root-hint JNI/lifecycle calls never hold `display_mu` while entering `vk_window_*`.
Display callbacks remain bounded snapshot/notification producers, not presenter/recorder owners.

`TOTK_PRESENT_UBWC=1` is a separate, off-by-default compressed-AHB allocation experiment.
It also requires a known **nonidentity** root hint, but does not require pre-rotation: compression alone can be
compared against ordinary rotated presentation. The experimental import is admitted only for individually qualified
**complete** Vulkan `driverInfo` strings; unknown drivers retain the current CPU-readable explicit-LINEAR allocation.
The sole current experimental string is `Mesa 27.0.0-devel (git-u1p4c1b809)`, identifying the P4-only importer
on the 650 base. The earlier cumulative B4/B5 builds are not admitted; version/tag prefixes and later builds
do not match. This identifies the reviewed importer implementation; runtime/device qualification is still required.
It requests GPU/composer usage plus Qualcomm's private UBWC allocation bit, without CPU-read/write usage, and uses
Vulkan's actual AHB import rather than treating that fd as LINEAR. Descriptor, reported format/features, external
image/usage support, UNORM/sRGB views, and **post-bind** image requirements versus the AHB allocation/type are checked.
This experiment requires an exact size match, rejecting both undersized layouts and ambiguous allocation padding.
Any allocation/import/binding/layout/view refusal tears down that attempt and uses the unchanged explicit-LINEAR
allocator; only that existing allocator's own failure selects copy. Ordinary allocation policy is unchanged.
Public AHB/Vulkan APIs do not expose the compression of an OPTIMAL imported image; the logs distinguish a request
and qualified-driver import from device evidence of actual UBWC/HWC acceptance.
Qualification must compare the game layer's CLIENT/DEVICE status and combined transform, orientation, crop/bars,
UI/overlay, both displays, background/resume and present-fence latency. Other independent system/vendor overlay
layers can still require client composition even when the full-screen game layer becomes DEVICE.

The `totk-present` thread still waits for the GPU fence; no semaphore-export or game-pacing changes are bundled here.
It then waits for transaction completion and the **previous** buffer's release fence before recycling that buffer.
The displayed buffer itself remains busy until replaced or its surface is detached. The render thread never waits
for SurfaceFlinger: an occupied ring drops a present as before. Missing device extensions or import/SurfaceControl
failures log the reason and retain the copy fallback. `APP.md`, "The picture", records the API choice and lifecycle.

The owner accepted round seven on both devices (Thor copy + overlay; Odin 3 AHB + overlay) without flashes,
crashes or waits, and separately verified AHB including background/resume on both devices before requesting this
default change. These are owner-reported device results, not integration-agent runs. A quantified power/CPU/GPU-time
comparison remains separate: on Thor and Odin 3 compare warmed Gerudo Town at steady 30 fps, 180 s per mode in both
orders, all other settings fixed. Retain `totk-present` CPU, GPU/recording times, shown/dropped/cancelled counts,
and the AHB logs confirming no copy fallback. Continue checking orientation, sRGB, opacity, crop/black bars and lifecycle.

## Pipeline skip safety and hitch attribution (2026-10-22; device measurement pending)

Dev already compiled cache-control misses on two low-priority workers; this integration **keeps that default**.
It does not import upstream's second pipeline map/job ring or add a `TOTK_VK_PIPEASYNC` switch. Existing normalized
keys, walking-thread ownership, pipeline memo and prewarm share one entry/queue. `TOTK_VK_COMPILE_THREADS` still
defaults to **2**, range **0–8**; zero builds at the draw. `TOTK_PIPE_WORKERS`, when nonempty, overrides it with range
**1–4** (upstream spelling); unset that override when testing compile threads zero. Workers already run niced +10.

`TOTK_VK_PIPE_WAIT_MAX` defaults to **90**, range **0–10000**; **0 disables the bound** for a controlled A/B.
Each key counts distinct **game submissions in which it actually skipped**, not individual draws, elapsed submissions
where it was unused, or GPU chunks. After that many skipped submissions, its next submission builds synchronously
if still queued, or joins its one in-flight builder. It never races a second build against the worker or overwrites
an existing handle. The bound trades one possible hitch for prolonged missing geometry; it is not a wall-time bound.
Use **2** as the aggressive area-load/loading-map diagnostic, versus 90 and disabled, before selecting a lower default.
Offscreen color captures still build/wait immediately, even if their key was first queued by a screen pass.

`TOTK_VK_PIPE_BOOST=1` (default unset = off) lends priority to a joined build: when a recording thread waits in
such a join (or a dispatch joins the compute worker), the worker building that entry runs at the waiter's nice
until it publishes, then returns to +10. Every 300 presents `pipeline joins: N waits on worker builds ... in X ms;
boosts B, refused R` reports both arms; compare the join ms and the frames over 50 ms. `refused` counts kernel
refusals (no `RLIMIT_NICE` headroom), logged once as `pipe boost: ... refused`. Live (`totk.ctl`): the next join reads
the new value; a loan already made is still restored by the worker.

`TOTK_VK_UNWRITTEN_MASK=1` (default unset = off) builds `colorWriteMask` 0 for every colour attachment whose location
the fragment shader does not output (`fout` in the seed, array outputs spanning their locations). NVN/Maxwell leaves
such a bound target untouched; Vulkan leaves it undefined and Adreno writes black, and the write also costs bandwidth.
The mask follows the key's fragment shader, so pipeline keys and PSO lists are shared by both arms. Every 300 presents
`unwritten colour attachments: P new pipelines masked A attachments` (switch on); compare GPU ms and look for black or
stale targets. Live (`totk.ctl`): only pipelines and output libraries built after the change follow it; pipelines
already built keep their masks, because the key does not carry the switch (PSO lists stay shared). Every fragment
shader keeps its SPIR-V output span (`fout_span`) so an array output is never masked when the switch turns on live.

Build errors are no longer permanent silent skips. Each failed key logs its identity, shader variants and Vulkan
result once, then retries synchronously after a failure is observed: one subsequent game submission for the first
failure, then 30, 60, 120, 240 and at most 300 submissions between observations/retries. Repeated draws cannot cause
a retry storm. Successful retries log `pipeline recovered key ...`. A persistently invalid pipeline still cannot
draw; retries cannot make invalid shaders valid, and failure counts/visual holes remain device acceptance failures.

Graphics/compute creation and pipeline-cache extraction **do not use a shared backend mutex**.
The cache is created with `flags=0`, without `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`;
the implementation must internally synchronize concurrent modifying commands. `vkGetPipelineCacheData`
has no external host-synchronization requirement. See the normative
[cache creation](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html),
[graphics creation](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateGraphicsPipelines.html)
and [extraction](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html) rules.
Driver-internal locks can still cause interference: absence of a backend mutex and a clean TSan run do not
prove absence of a driver stall. Pipeline publication remains release/acquire, queue ownership is mutex-protected.

### Coalesced whole-cache persistence

`vk_pcache.c` replaces the detached 20-second size-change saver. Previously each changed-size check allocated
`size + size/8 + 1 MiB`, copied the **entire opaque cache**, wrote/fsynced/renamed it, then freed the buffer.
There were no quiet/lifecycle gates, explicit priority/affinity, shutdown join or directory fsync.
In the analyzed Hateno sessions this rewrote **355.16 MiB for 403 KiB net growth (six saves)** and
**240.66 MiB for 258 KiB (four)**, with **114/127/130 ms thread-CPU bursts** in the second profile.
These are historical observations, not new-policy device results.
Startup primary/import loading now shares the `vk_pcache_identity.h` context validator. A versioned envelope
records exact effective `TU_DEBUG` (empty/unset equal) and SHA256 of the copied custom driver, or a distinct
system-driver marker. The contained Vulkan vendor/device/cache UUID is still checked. Context is immutable
after env-file/A722 policy and driver selection, before saver dispatch; APK identity is deliberately excluded.
Hash failure disables persistence/import. A mismatch never reaches `vkCreatePipelineCache`; first mismatch
renames to `.stale`, preserving that first backup across later mismatches. When it exists, the rejected
current file remains until normal fresh-cache replacement. Envelope and raw payload are separate writes in
the **same atomic transaction**, without copying the opaque blob to prepend metadata. Universal1b rejects
the new leading header version safely before driver use.

Legacy raw files are accepted only with empty current `TU_DEBUG` and matching Vulkan identity. This preserves
unchanged Thor/Odin caches but cannot prove their historical flags/full driver bytes: Turnip's UUID includes
the driver build-id, not a hash of every byte. Nonempty flags (including automatic A722 sysmem) require a
known envelope. An accepted legacy primary marks one migration generation and uses a zero durable payload
baseline until the first successful coalesced save; all loaded entries remain in the live driver cache.
The growth contract below applies after that one-time metadata migration. See APP.md for import/rollback
limitations and the bounded Duo cache-reload evidence, not a claim to repair arbitrary opaque driver entries.


The new single saver checks every **5 s**. It tracks successful graphics/compute creations (including cache hits
and library links), plus startup imports. **Every snapshot requires positive growth in the cheap size-only
query versus the last saved size**, including deadline and lifecycle/shutdown snapshots. An unchanged (or smaller)
size acknowledges exactly the generation sampled **before** that size probe and clears dirty age, without payload
extraction, allocation or IO; creates completing between sampling/probing/acknowledgement remain dirty for another
probe. It logs `skipped reason=unchanged-size change_detection=size-growth-heuristic` (other fields intervene).
This does not advance the durable file-size or last-save time baseline.
Ordinary saves additionally require **60 s** since the prior snapshot AND **256 KiB cache growth OR 256 successful
creations**, only in a quiet candidate: Android background; no presents for >=2 s; or <=128 draws/frame sustained
>=2 s (loading/menu **heuristic**, not an authoritative guest pause decoder). Active play otherwise defers.
**120 s dirty age overrides the growth threshold and quietness**, but never the requirement for actual positive
size growth, so even a one-byte growth trickle during continuous play is bounded. onPause/onStop and orderly exit
bypass the coalescing gates, not the actual-growth check. Pause/stop enqueue work without IO on the UI thread;
onDestroy/explicit app close and `atexit` rendezvous with and join the saver.

This is a **size-change heuristic**, not exact content-change detection, deliberately sharing the old saver's
size-change assumption: a driver could change cache contents without
increasing serialized size. Such same-size changes will not be persisted. That residual risk is considered
negligible for the append-style driver caches observed here, not impossible by Vulkan's opaque-cache contract.
In the reported pcache1 warm-cache Hateno run, all three deadline snapshots had **zero growth** yet rewrote
**63,401,709 bytes each** (~190 MB total), consuming **305–373 ms thread CPU / 419–551 ms wall**.
pcache2 skips those cache-hit-only generations; successful pipeline object creation is not proof of new cache bytes.

The normal crash-loss bound is **120 s + <=5 s observation/poll delay + snapshot/durability completion time**,
assuming the process is scheduled, the driver responds and filesystem operations succeed. A create concurrent
with extraction is conservatively retained as dirty for the next pass. Suspension, forced kill before completion,
driver hangs and persistent IO/allocation failures cannot have a finite durability guarantee; errors retain the
old baseline/dirty age and retry, rather than claiming success. Background Android termination has no callback guarantee.

One saver owns scratch, policy and file replacement. Lifecycle callers only change mutex/condition-protected
requests; they never touch scratch or Vulkan. Creation generation and present hints are atomic. The generation
is sampled **before extraction**, not after it; concurrent creations cannot be acknowledged by an older snapshot.
No backend cache or recorder lock is held by the saver; compile workers and recorder remain concurrent.
The saver is joinable; scratch is released only after its final attempt, with concurrent shutdown callers serialized.
The retained buffer grows geometrically (`size + size/8 + 1 MiB`) only when capacity is insufficient.
Each file transaction handles short writes/EINTR, fsyncs the temp file, closes it, renames atomically, then fsyncs
the parent directory. Pre-rename failure preserves the old file. Directory-sync failure after rename leaves the
complete new file visible but does **not** advance the durable baseline. No opaque blobs are concatenated.

The saver requests nice **+10** and uses the same capacity/max-frequency detector as the seed worker:
select the lowest tier only when below 60% of the highest and containing at least two allowed CPUs.
Homogeneous/all-big or unavailable topology retains inherited affinity. It never broadens taskset/cgroup
restrictions or changes ClusterTune. Affinity/nice/source/decision/errors are read back in placement diagnostics.

`TOTK_PCACHE_STATS=1` (default off) logs `taken`, `skipped`, `deferred` or `failed`, reason/quiet-source,
snapshot count, bytes and cumulative bytes actually written, growth, successful creations, dirty age, wall/thread-CPU
duration, CPU and scratch capacity/allocation count. `overlap operation=graphics-create|compute-create|recording-scope`
reports whole-operation wall time and its intersection with extraction time, even for operations crossing extraction
boundaries. This measures **concurrent wall time, not causally attributed driver-lock wait**; sums across threads/scopes
overlap and must not be added into a frame budget.

Host coverage: `vkpcachetest` runs synthetic policy/time tests, unchanged-size quiet/deadline/lifecycle skips,
actual-size growth and failed-size-probe retention, all snapshot failure stages, concurrent atomic-file readers,
incomplete extraction, scratch reuse, two real Vulkan creators with concurrent extraction/lifecycle/shutdown,
and an exec'd fresh-process reload using the production loader. Controlled test-only padding around real Vulkan
data supplies deterministic size-growth stimuli; no such padding or alternate cache format exists in production.
TSan checks host races, not Vulkan spec compliance.


`TOTK_VK_HITCH=1` enables the upstream-style profiler; default **off**, with only disabled-flag branches at its hooks.
At each present it exchanges independent relaxed-atomic counters. A frame at least 40 ms and 2.5× the rolling
64-frame median logs `hitch at frame ...` (at most 24 detailed lines per 300 presents). Counters drain **every**
present, unlike upstream's since-last-hitch accumulation. Detail includes pipeline build/time/fail/retry/bound/skips,
shader loads/bytes/variant lists/requests, pool/chunk creation, CPU steps, idles, writebacks, replay and GPU waits.
`hitches, last ... frames` reports 40–60/60–100/100–200/200–400/≥400 ms bins and window pipeline failure/retry/bound/skip
totals. Existing always-on `stutter:` still supplies **>50 ms frames**, and existing `skipped: ... pipeline compiling`
is cumulative. Independent atomic exchanges can split concurrent worker updates across adjacent frames; aggregate
worker build durations can exceed frame duration and are not the walk's critical path.

Completion latency means **observed fence-return → completion publication**, not the unavailable driver's actual
signal time; wake latency is publication → blocked waiter resumes under the completion mutex. Polls, already-complete
calls and timeouts do not add wake samples. Completion totals include all finished jobs, not only jobs waited on by
the game, so completion/wake totals must not be interpreted as additive parts of the game-wait total. Zero counters
do not prove a stall came from the game: scheduler starvation and uninstrumented backend/driver work remain possible.

Device A/B on Thor and Odin 3: keep driver, CPU/GPU caps, resolution, save/weather/camera, frame-lag/recorder settings,
shader seed and cache warmth fixed. Start at 30 fps; test Odin's 60-fps controller separately after visual acceptance.
Use independent cold-cache area loads into Lookout/Gerudo/Depths plus the loading-screen map, then repeat warmed
Lookout camera sweeps for 180 s in both A/B orders. Keep `TOTK_VK_HITCH=1` for attribution runs; compare profiler off
separately for overhead/power. Test WAIT_MAX **0/90/2**, workers **1/2/4** one variable at a time, and prewarm off/on
with the same learned file. Read `hitch at frame`, `hitches, last`, `stutter`, `pipeline failure/recovered`, the
`reached ... skipped submissions` fallback lines and cumulative `pipeline compiling` skips. Count >50 ms frames,
worst intervals, pipeline skips/bounds/failures/retries and watts; video-check map completeness, object/material pops,
Gerudo persistent probe lighting, and Lookout camera-cut flashes. Any persistent blank area/map or failed key is a
failure, not a performance win. No Thor/Odin performance or pixel-equivalence result is claimed here.

## Compute first-use preparation

Unlike graphics, compute dispatches cannot safely be dropped during compilation. The dedicated
`totk-compute` worker therefore prepares an immutable module/layout pair and publishes one handle;
dispatch always joins unfinished creation. Shader/module/layout maps remain startup/replay-owned,
not worker-mutated. Binding a compute program queues every exact variant in its list without
selecting using incomplete bind state; shader resolution also queues a newly translated module.
A held graphics compile cannot exhaust the compute worker. Driver-internal contention can remain.

Compute dispatch learns exact variant keys in seed-namespaced `state/vk-compute-used.bin`
(bounded 4,096 records, 5-s worker save coalescing plus shutdown, atomic publication). Startup
resolves learned modules and queues creation before replay, independently of graphics
key lists and graphics compile-thread/cache-control settings.
The version/device/driver UUID, exact file size, key syntax and checksum are validated before
accepting any records. The checksum detects accidental corruption, not hostile authenticity.
Seed reads/module creation for learned keys still cost startup time; a fast load can join remaining
warmup. This compute learning is separate from portable graphics collection.

`TOTK_COMPUTE_STATS=1` (default off) prints per-create source/key/SPIR-V bytes/local size/driver
duration/result and per-300-present recording/background counts, ready hits, joins and blocking
total/max. The `long frame ... compute pipelines` field now counts only replay wait/synchronous
work, never asynchronous worker duration. See [APP.md](APP.md#first-use-compute-pipelines-and-learned-seed-warming)
for the exact lines, cand4's 149-ms attribution bounds and device confirmation procedure.
Neither host correctness nor the existence of overlap establishes measured Thor speedup.

## Portable first-visit graphics pipeline keys

`TOTK_PSO_COLLECT=1` learns **every successful complete graphics pipeline key**, including cache hits,
screen/capture builds and worker builds. It does not capture shader code, SPIR-V, opaque driver cache
bytes, handles or guest addresses. A collection APK can use `make PSO_COLLECT=1`; the environment
can explicitly turn collection off. The cumulative collection lives next to the selected driver cache
as `<cache-path>.pso-keys` (or seed-namespaced `state/vk-pso-collected.bin` if persistence is unavailable).
The existing single pcache saver snapshots it every five seconds and on lifecycle flush/shutdown.
No file writes occur on a draw/compile worker. Force-stop can lose recent unsaved keys.
Collection also uses that saver with opaque-cache persistence explicitly disabled; no extra saver is
started in inactive cache-off/context-failure configurations. Metadata path overflow disables collection
rather than truncating onto a cache file.

`games/totk/scripts/merge_pso_keys.py --output games/totk/assets/pso_keys.tsv COLLECTION...`
merges independent final snapshots, deduplicates by exact key, drops either-stage shader identities
whose 64-hex root is absent from `shader_list.tsv` V rows, sorts captures first then descending successful-build frequency, and
breaks ties by key bytes. Supply only the final cumulative snapshot from each device; overlapping
intermediate snapshots would overcount frequency. Identical input files count only once.
Input reads and union growth are bounded to the runtime limits; an over-limit union fails rather than
silently trimming keys. The APK builder independently validates the text list against the shared
shader inventory, then generates `assets/pso_keys.bin` directly during packaging. No binary
key-list source is committed or reused; absence of the TSV omits the asset, even if an old binary
remains in a local build directory. `--assets` cannot supply a parallel PSO source.

The canonical source is ASCII/LF **`TOTKPSO-TSV 1`**, with a fixed explanatory header and six
tab-separated columns: `vs`, `fs`, `early`, `state`, `capture`, `frequency`. Each line is one full
key. Stage names preserve the exact lowercase root64 and any existing `-suffix12`; `-` denotes
an absent fragment stage. `early` and `capture` are decimal `0`/`1`, and `frequency` is a nonzero
decimal uint32. `state` is exactly 155 comma-separated uint32 words, in the binary order below,
using minimal-width lowercase hexadecimal (`0`, not `00000000`, and no `0x` prefix). Every
word, including zeroes, is explicit; no state is inferred. Rows are capture-descending,
frequency-descending, then binary-key-byte ascending, with no duplicates. Noncanonical text,
out-of-range integers, missing words and inconsistent ordering fail packaging.
The final three-device TSV is **1,253,667 bytes**, below the 2,000,000-byte text-source target;
it generates **2,054,512 bytes / 2,594 keys / 499 capture keys**, SHA-256
`762cca1bfa5cc001ce0df44e62f146f53eb441cbb308aeb3dc1a241f9111ef1b`.
Minimal-width words avoid expanding predominantly-zero state to fixed eight-digit hex.

Format v1 is a strict 64-byte header (`TOTKPS01`, little-endian version/record-size/count,
reserved zeroes, SHA-256 of the complete payload). Each 792-byte record has two zero-padded
80-byte exact seed variant identifiers, an early-tests word, **155 explicit little-endian state
words**, a saturating frequency and capture flag. No bitfield layout, struct padding, native
endianness or pointer width is serialized. The fixed state is all inputs used by `pipe_key_fill` /
`pipe_create`: eight colour formats, depth/stencil formats, attachment count, samples, blend enables,
write mask, alpha-to-coverage, polygon/topology/depth-clamp/logic state, seven named values per
blend target, sixteen attribute format/offset/stream triples and sixteen instancing/divisor pairs.
Stride, cull/front-face and depth/stencil tests are Vulkan **dynamic state**, not omitted static
key inputs. Bump the format version if these pipeline-key semantics change.
Both binary collection and generated shipped files are bounded to **16,384 keys / 12,976,192 bytes**, within a
**16 MiB shipping budget**. Malformed, truncated, noncanonical identifier/state or bad-checksum
files are ignored as a whole. The checksum is integrity checking, not hostile authenticity.
The root is the already-shared shader identity; an optional 12-hex suffix is the existing seed's
normalized binding/render-state signature (`answers_json` SHA-256 prefix), not shader code or a
new shader root. Exact variant filenames need not occur literally in the shared list.

The APK's optional `pso_keys.bin` asset enables prewarm automatically; no asset means no
prewarm or graph change. Desktop/explicit deployments set `TOTK_PSO_KEYS=/path/to/list.bin`.
`TOTK_PSO_PREWARM=0` disables list replay for A/Bs. Compile workers remain required; zero workers
or missing cache-control support disables speculative compilation, never synchronous correctness.
Startup resolves the exact seed modules/reflection/layouts on their existing single owner, then
publishes immutable entries to a **separate low-priority queue**. Missing seed variants are skipped,
counted and retried next session, never substituted. Only worker zero runs speculative jobs, at
nice +10. Demand jobs always win the next dequeue, and a demanded speculative entry is promoted
to the demand queue. No new speculative build starts while any demand build is in flight.
A Vulkan driver call already in flight cannot be cancelled/preempted.
The worker probes the driver cache first and compiles only `COMPILE_REQUIRED` keys.

Capture-priority keys run first. Background work pauses during synchronous driver builds, after
frames exceeding 40 ms, after >25 ms recording or GPU submissions, or >4 ms recording GPU waits;
these are conservative pressure **heuristics**, not thermal/OS-pressure guarantees. Pressure pauses
last 500 ms; a 10 ms inter-key gap additionally limits sustained speculative pressure. Demands
build/join pending keys without skipping formerly cached draws. Persistent capture lighting keeps
the existing correctness-required synchronous rule even if startup is outrun.

Resume state `<cache-path>.pso-state` binds the exact list digest and a completed-key bitmap to
SHA-256 of the **successfully saved cache envelope + opaque bytes**. The cache loader must first
accept its vendor/device/UUID, driver SHA/source and `TU_DEBUG`; a list/driver/debug/cache change
therefore rebuilds instead of trusting stale progress. The saver samples completion **before**
Vulkan extraction and acknowledges it only after successful atomic cache persistence. Concurrent
later completions remain dirty. Dirty list progress is the one explicit exception to the usual
same-size-cache save suppression: lifecycle flush or a quiet >=60 s opportunity extracts even a
same-size cache, so a cache-hit-only list can acquire a durable marker. Interrupted publication
can lose progress, never falsely acknowledge an unsaved cache. Partial bitmaps resume next boot;
fully completed matching lists do no speculative work next boot.

`[pso-prewarm] list=... resumed=N/T missing=M startup_ms=...` reports resolver/startup cost;
`progress N/T ... build_ms=... wall_ms=...` reports compilation progress. `vk_pipeline_keys_stats`
exposes done/total/build-ns/missing/failed/collected counters for an eventual overlay consumer.
The existing `stutter:` sync-build / prewarm-wait counters remain separate from worker time.
Compare identical drivers, caps, seed packs, saves and cold-cache inputs; retain moving-route
sync counts, worst frame/FPS, startup duration and measured power. An uncollected route is a
generalization experiment, not proof that never-observed keys can be eliminated.


## Always-on walker thread CPU metric

Every 300 presents, next to the unchanged `frame N, per frame: recording ...` row, the backend logs:

```
[nvn-vk] walker cpu frame N: cpu X ms/frame, wall Y ms/frame, draws D/frame, cpu Z us/draw, offcpu W ms/frame
```

`cpu` sums `CLOCK_THREAD_CPUTIME_ID` deltas for committed NVN submissions, divided by 300.
`wall` is **raw elapsed CLOCK_MONOTONIC time**, not the legacy `recording` value. Both cover the existing
recording interval: after `mtl_sub_begin` has acquired the recording lock, initialized the backend and reset
snapshots, through `mtl_sub_commit`'s final submit/publication join and uniform/GPU-write pruning, before
profile shutdown, resource sweep and unlock. `hle_nvn.c:submit_exec` calls both endpoints synchronously
on the same thread; optional async replay moves that entire call to its translation thread, not one endpoint.
The thread is assigned the walking role in `mtl_sub_begin`. No recorder/submit/compiler worker CPU is included.
There are exactly two additional CPU-clock reads per NVN submission (one start/end pair), never per draw.
The existing wall-clock reads bracket that pair. No sampling or profile environment variable is required.

Raw `wall` includes explicit slot/GPU waits, recorder-worker/publication joins, scheduling delays, and diagnostic
`TOTK_VK_SYNC` waits occurring inside that interval. `cpu` includes the thread's CPU work executing those waits,
not time descheduled/asleep. The unchanged `recording` row subtracts the slot-recycle and `wait_value` elapsed
wait counters only; it still includes worker/publication joins and `TOTK_VK_SYNC`'s `mtl_wait`.
`offcpu = raw wall - cpu`, **not** adjusted recording minus CPU. CPU should be bounded by raw wall (allow log
rounding), not necessarily by adjusted recording. Work before the interval, present work and game sync waits
outside it are excluded. This is not whole-render-thread CPU time.

`draws` is the existing accepted graphics-draw counter's delta divided by 300, not attempted/skipped draws or
dispatches. `us/draw = total cpu ns / accepted draws / 1000`, zero for a no-draw window; it amortizes all submission
work, including copies/dispatches, over those draws and is not a pure draw-call cost. Frame N labels presents
N-299 through N; counters are drained at that present. Divergent scenes/work mixes still affect this ratio.

`python3 games/totk/android/walker_summary.py <log>...` summarizes CPU, raw wall, draws, draw-weighted CPU/draw,
FPS and long-frame fraction. Each stream independently drops its first eight logged windows, matching the route
helper. Long-frame selection exactly retains interval-bin windows with fourth bin >=150 and sums bins 5+6 over
all six bins. Missing FPS/bins print `n/a`, not zero. `--start-window 1` is useful for a short metric smoke check.
`--pair A.log B.log` reports median per-window CPU/draw differences for equal 300-present frame-index ranges;
this is exploratory, **not route-position/workload matching** and supplies no confidence or detection claim.
`--calibrate base1.log base2.log base3.log ...` reports empirical unchanged-baseline run spread and absolute
run-pair differences to calibrate a practical noise threshold; these dependent pairs are not confidence samples.
The standalone `vkdrawtest --walker-cpu` host mode checks the raw-clock relationship, sleep exclusion, counter
reset and one real 300-draw/300-present log window, with either recording/lag environment arm.

## Recording split profile (single-thread baseline)

`TOTK_VK_REC_PROFILE=1` enables recording-thread CPU accounting, read once at Vulkan initialization.
It does **not** enable parallel recording. `TOTK_VK_REC_PROFILE_RATE` now defaults to **8** (round up to a power of two,
clamp 1–64): approximately one in eight graphics attempts and other opcodes is timed, with weighted category estimates.
Use rate **1** for exhaustive probes and the original S/P/Amdahl comparison; lifecycle/O work is timed fully at every rate.

The path is `hle_nvn.c:2239 submit_exec` → `mtl_sub_begin` → `mtl_sub_snapshot_ubos_cap` →
`mtl_exec` (recursive calls and jumps included) → `vk_draw_exec` → `mtl_sub_commit`.
The default category for that entire locked replay is **S**; nested scopes move time, rather than double-count it.
Present's backend work is a separate **O** scope under the same recording lock. TLS activation excludes pipeline,
completion and presenter threads and concurrent game-thread resource access. No timer runs between submissions/presents.

### Classification

The file:line locations below identify baseline commit `0e89e59` in `android/gfx/` unless otherwise stated; function
names remain the cross-reference after the raw-input revision. P describes the original profiling category, not a
promise that its position-dependent uniform/resource values or pipeline skip decisions can move to workers.

| Class | Work and functions (file:line) |
|---|---|
| S | Stream validation, opcode walking, `OP_BEGIN/END/CALL/JUMP`, event post decisions: `nvn_vk.c:1559 mtl_exec`, `mtl_sub_event`. `OP_SET_TEX_POOL/SET_SAMP_POOL` are currently no-ops in `vk_draw_exec`; they still incur walking/dispatch cost. |
| S | Every bound-state update in `S`, dirty/revision hooks, target comparison, render enable/conditions, reset/report decisions: `vk_draw.c:1729 vk_draw_exec`; `nvn_vk.c:1538 mtl_sub_counter`, `:904 cpend_push`, `:920 vk_counter_value`. Pending report values must be observed in stream order. |
| S | Inline update validation/logging and fallback CPU writes: `update_ub`, `nvn_vk.c:1220 ub_log`; log pruning, overlays' version bookkeeping and flush decisions: `:1202 ub_prune_inner`, `:1256 ub_flush_inner`, `vk_ub_touch/pending`. Flush recording is conservatively retained in S with its ordered decisions. |
| S | Replay-time snapshot collection/materialization and earlier-submission overlays: `nvn_vk.c:1422 mtl_sub_snapshot_ubos_cap`. With async replay, the earlier game-thread capture is outside this profile. |
| S | GPU-write/snapshot invalidation: `nvn_vk.c:1326 gpu_write_inner` (`vk_gpu_write`, `snap_drop`, write-range tracking/sealing); `vk_res.c:628 vk_tex_mark_gpu_written`. These are reclassified as S even when reached from a descriptor build or a copy. |
| S | Draw eligibility/scissor checks, `vk_draw.c:1476 draw_touch` (including its resolves), query-segment decisions (`vk_occlusion_draw/end`), `nvn_vk.c:1137 cpu_step_inner`; the actual submission/wait within a CPU step is O. |
| S | Pass begin/end and pending-clear decisions, attachment realization, barriers and begin/end calls: `vk_draw.c:777 begin_rendering_inner`, `:648 end_rendering_inner`, `:880 clear_color`, `:898 clear_ds`. Chunk **decisions** are in `begin_rendering_inner`/`vk_submit_chunk`; actual submission is O. Pass/clear command calls stay conservatively bundled with the planner in S. |
| S | Slow texture realization/dirty-state and resource-version changes: `vk_res.c:397 tex_get_inner` (`vk_tex_get`), `:316 img_upgrade_inner`. Upload execution is O. The hot descriptor texture-ID cache path remains P; this conservative boundary does not claim resource mutation is worker-safe. |
| P: resolve | `nvn_vk.c:717 vk_resolve_for` and `vk_resolve/span` while called from a P scope: pool lookup, segment/page hints, index refresh. Resolves reached from S/O stay in that category, rather than falsely treating the sequential dependency walk as parallel work. |
| P: bind/desc | `vk_draw.c:1301 bind_stage`, `:1225 ubo_desc`, generated driver/support blocks, descriptor info construction, hashes/cache lookup, allocation/write, descriptor binding, texture/sampler/view lookup. Includes `nvn_vk.c:1451 vk_ub_rename` and `:1505 vk_ub_read` for a frozen snapshot/update version; their pruning, pending checks and GPU-write bookkeeping are S. Nested pool resolves are removed into P:resolve. |
| P: prog/variant | `vk_shader.c:383 vk_prog_resolve`, `:329 stage_sig`, program/variant caches, reflection/seed loading on misses. State hooks that invalidate signatures remain S. This category is measured only for graphics draws, not dispatches. |
| P: pipeline | `vk_draw.c:1156 pipe_get`, `:1024 pipe_create`, `vk_layout_of`, key generation/cache lookup, synchronous creation or enqueueing the existing compiler workers. Compiler-worker execution is excluded. |
| P: vertex/index | `vk_draw.c:1493 draw_buffers`: stream/offset/stride construction, zero stream, transient copies, uint8 index widening and quad-index generation. Nested resolves and command-call blocks are charged to their own P subcategories. |
| P: cmd | Draw pipeline binding, `vk_draw.c:1441 dynamic_state` and `vk_record.c:80 vk_record_dynamic_emit` (including CPU comparisons), vertex/index binds and `vkCmdDraw/DrawIndexed` in `draw_buffers`. Descriptor update/bind calls stay in bind/desc. This is not a pure driver-only timer. |
| O | `nvn_vk.c:982 slot_begin_inner`, `:1101 slot_submit_inner`, `:1130 wait_value_inner`: slot recycling, command/descriptor-pool reset, command-buffer start/end, queue submission and recording-thread GPU waits. Nested uniform flush/pass endings and sealing are still S. |
| O | Ordered non-graphics work not modeled as parallel draws: `vk_draw.c:1617 dispatch`, `:1643 copy_b2t`, `:1662 copy_t2b`, `:1681 copy_t2t`, `:1704 clear_tex`, explicit barrier ops, `mtl_sub_copy`; `vk_res.c:341 upload_inner`; `vk_draw.c:2465 present_window` and optional screenshot/pass diagnostics at present. Nested S dependency/write/pass work is removed. |
| D | **Not separately measured.** Driver CPU time is included in whichever S/P/O scope issues the call: descriptor updates in bind/desc, graphics pipeline creation in pipeline, draw calls in cmd, begin/end/barriers in S, queue calls/waits in O. No extra per-driver-call interception is installed. |

### Output and interpretation

Log shape (angle-bracket fields are placeholders, **not observed measurements**):

```text
rec profile frame 300, ms/frame: S <s> P <p> [resolve <r> bind/desc <b> prog/variant <v> pipeline <l> vertex/index <i> cmd <c>] O <o>; D=not-separated; draws <recorded>/<attempted> attempted, passes <passes>; ideal 2/3/4 <x2>x/<x3>x/<x4>x
rec profile granularity: passes/frame draws=0/1-15/16-63/64-255/256-1023/1024+ <six averages>; max draws/pass <max>; largest pass P <ms> ms/frame (<share>% of P, max frame share <share>%), max pass P <ms> ms; pass-attributed <share>% of P
rec profile scopes/frame S/resolve/bind/prog/pipeline/vertex/cmd/O <eight averages>; probes <n>/draw, estimated overhead <ns> ns/draw (<ms> ms/frame, <share>%); O-fixed ideal 2/3/4 <x2>x/<x3>x/<x4>x
rec profile work/frame: <ops> ops, <submissions> submissions, <steps> CPU steps
```

- The six P fields are exclusive and sum to P. With rate >1, S/P are **weighted estimates**, not measured elapsed time;
  rare/pass-local costs and scope counts have sampling variance. Lifecycle/O scopes always use measured weight 1.
  The sampling line separately reports coarse measured replay/present elapsed, sampled/actual draws, and actual versus
  weighted probe-bias estimates. Weighted categories need not equal coarse elapsed. Rate 1 bypasses sampling gates
  and retains exhaustive scope timing. All figures include preemption; none is thread CPU time or a frame interval.
- `ideal N` is `(S + P) / (S + P/N)`, for N = 2, 3, 4. The third line also reports
  `(S + P + O) / (S + P/N + O)` with O held fixed. Neither predicts game fps, GPU overlap, scheduling overhead,
  startup/cold shader behavior, or an attainable pipeline speedup.
- Completed/attempted draws, opcode counts and pass/draw histograms stay **actual**, including unsampled draws.
  Scope counts and per-pass P costs are weighted when sampled; one scope is not one driver call.
  Ops include BEGIN/END/CALL/JUMP; nested records preserve independent sampling context on return.
- Each pass accumulates its draws' **P time**, including program resolution before the pass opens and work on skipped
  draws attempted while it is open. The histogram includes empty passes; bucket values sum to passes/frame.
  `largest pass P` is the sum of each frame's largest pass divided by frames; its percentage is that sum / total P.
  `max frame share` is the worst single-frame fraction; `max pass P` and max draws/pass are window maxima.
  A pass spanning present is counted as frame-local slices without forcing extra rendering/submission boundaries.
  `pass-attributed` exposes P from early failures without an open pass, rather than pretending it belongs to a pass.
- A whole-pass job cannot be divided among workers: even with all jobs ready, its parallel part takes at least
  `max(P/N, largest_pass_P)`. Many tiny/empty passes or a dominant pass may make ideal Amdahl scaling misleading.

### Cost and limitations

The disabled path is one predictable branch **per probe**, with no clock, TLS access, allocation or counter update.
There is no timestamp per state opcode: the outer S interval accounts for walking/state updates; the optional opcode
counter only increments when enabled. The enabled path uses fixed-size aggregates/histograms and no profiling allocations.
`CLOCK_MONOTONIC` is deliberately used rather than blindly executing `CNTVCT_EL0` on a vendor kernel:
[Linux arm64's timer policy](https://github.com/torvalds/linux/blob/v6.1/drivers/clocksource/arm_arch_timer.c#L937-L960)
enables direct user access only where counter workarounds permit it; target-device readability has not been tested here.
[Android bionic's clock path](https://android.googlesource.com/platform/bionic/+/master/libc/bionic/vdso.cpp)
uses the vDSO when available, retaining the kernel's safe counter/erratum handling and syscall fallback.

At first activation a 4,096-pair push/pop loop calibrates the clock plus exclusive-accounting transition cost on the
recording CPU, outside measured intervals. Each window estimates overhead as `transition_count * calibrated_ns`,
reported per **attempted** draw (all probes, including per-frame/submission probes, are amortized over those draws).
For example, **if** calibration reports 30 ns and a window reports 30 probes/draw, the estimate is 900 ns/draw,
or 4.95 ms at 5,500 draws/frame; these are arithmetic examples, not device results. The estimate omits cache/code-size,
branch, opcode/pass-counter and draw-attribution costs, and can vary with frequency/preemption; do not subtract it as an
exact correction. Compare identical warmed scenes with the switch off/on before trusting small differences.

The profile excludes pre-lock contention, Vulkan initialization, frontend `gfx_enter`/`pin_step` and submission
diagnostics, the post-unlock `vk_res_sweep`, external game sync/pacing waits, async enqueue/capture work on the game
thread, and the background threads. Do not combine it with screenshots/pass dumps/GPU-pass profiling for clean CPU data.
Run warmed caches and check attempted/recorded counts: cold pipeline skips change the work being measured.

A real split recorder still needs thread-safe or worker-local program/variant/pipeline/texture/descriptor caches and
scratch arrays, per-worker descriptor **and command pools/transient arenas**, correct resource lifetime/upload/version
planning, worker-local bound-pipeline/dynamic-state memos, and secondary command buffers with dynamic-rendering
inheritance (or ordered primaries) preserving pass/barrier/query/transfer/chunk order. In particular, copying `S` is
insufficient: workers must share immutable uniform snapshots and the exact inline-update version visible to each draw;
the sequential walk must retain snapshot invalidation, writable-storage tracking, counter/condition order and CPU steps.
Snapshot copying, job creation, synchronization, lost memo locality, duplicate binds and load imbalance are additional
costs, absent from the ideal estimate.

## Multi-threaded graphics recorder design

`TOTK_VK_REC_THREADS` defaults to **2 on Android**, **0 elsewhere**. Explicit `=0` retains the original
primary-command-buffer path for A/B; positive values select recording workers, not pipeline compiler workers.
The walking thread remains the sole owner of stream position and resource realization. After the Thor regression
measurements, the ownership contract is:

- **Walking thread:** all S work above, including bound-state changes, inline-update logging and pruning,
  `vk_ub_rename`, snapshots and `vk_ub_read` (including storage pointers read from uniform blocks), `draw_touch`,
  counter/cpend/sample totals and render conditions, CPU steps, GPU-write tracking, attachment/texture realization,
  uploads and dirty-state mutation. It chooses pass begin/end, upstream in-pass barriers and queue-submission chunks.
  Workers never dereference guest memory.
- **Packets:** ordinary storage and vertex/index bindings carry raw addresses/ranges plus an immutable span-index
  version captured at the draw's walking position. Uniform renames, uniform-derived pointers, generated blocks and
  exceptional unaligned copies are frozen on the walk. Texture/view/sampler realization stays there too; packets carry
  the resulting immutable Vulkan handles, not registry pointers. Dynamic state and draw arguments are by value.
  There is no mutable uniform-log or shader-object access on a worker and no resolved-info scratch array copied twice.
- **Workers:** resolve raw ranges against the packet's immutable index using context-local hints, clamp descriptor
  ranges, provide zero buffers, construct descriptor infos/writes, dedup sets, bind state and record draws.
  They never begin/end rendering, issue barriers, mutate resource registries or submit queues. Ordered clears and
  timestamp writes use separate continuation secondaries; occlusion queries remain on the diagnostic inline path.
  A chunk begins with fresh bound-state memos. Descriptor pools, dedup caches, scratch and command pools belong to one
  worker and one ring slot. After the slot fence, a generation token permits each context to lazily reset its own
  pools on first use, so unused worker contexts incur no reset and the walking thread does not reset workers' pools.
- **Execution:** the primary begins dynamic rendering with `CONTENTS_SECONDARY_COMMAND_BUFFERS`; secondaries use
  `RENDER_PASS_CONTINUE` and dynamic-rendering inheritance containing the exact pass formats and sample count.
  Large passes split into bounded draw chunks; completed secondaries execute in original stream order, independently
  of worker completion order. Every chunk of a secondary pass remains a secondary, even a tiny synchronous chunk or
  a pass first opened by a partial clear. ClearAttachments and counter timestamp writes become ordered continuation
  secondaries, not new rendering instances; synthetic sample reports need no GPU command or split. Actual pass boundaries
  remain the original barriers/target changes/CPU steps/load-clear decisions. A pass executes secondary handles in batches.
  At logical pass end the walk parks the still-recording primary section and continues in another primary without a
  worker join. Slot submission finalizes the parked sections (ordered ExecuteCommands, EndRendering, EndCommandBuffer)
  and submits the ordered section array. Copies/barriers can record in later sections without waiting for CPU recording;
  CPU readbacks/steps still assemble and submit before their existing fence wait. Original GPU submission chunks stay intact.
  This uses core continuation rules, not mixed contents: the Vulkan command properties explicitly permit secondary
  [ClearAttachments](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdClearAttachments.html) inside rendering
  and secondary [WriteTimestamp2](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp2.html).
  The saved Thor probe and current device extension setup do not enumerate/enable maintenance7 or nested-command-buffer
  support; their availability is not inferred and the recorder does not depend on either extension.
  `TOTK_VK_REC_ONE_PRIMARY=1` (exact `1`, read once, default off) instead retains ordered rendering sections
  and walker sections, then assembles one main primary per original chunk without pass-end joins.
  `pre` and adopted frame-generation primaries keep their original order (at most three submitted primaries).
  Occlusion diagnostics and inline/FG profiler statistics fall back before recording any chunk; other inline passes work.
  Compare the recorder line's main/aux primaries, section kinds, walker secondaries and fallbacks with identical settings.
  Recorder workers use raw secondary commands; shared dynamic state resolves a walking token only on its first
  emitted command. Enabled recorder-overhead lines additionally report section-secondary begin/end and section-assembly ms/frame.
  Immediate PipelineBarrier2 (including deep-copied memory/buffer/image arrays without extension chains),
  CopyBuffer, UpdateBuffer (including data), FillBuffer, ResetQueryPool and WriteTimestamp2 commands are
  retained in a per-slot byte arena and replayed directly into the main primary. Other immediate commands
  materialize a secondary: its deferred prefix is replayed first and every later command stays in that
  secondary, preserving issue order. Rendering and worker sections are unchanged. Arena offsets survive
  growth; bytes are reused only when the existing slot-retirement rules permit reset.
  Enabled-only 300-frame diagnostics report `deferred records/frame`, `deferred-only sections/frame`,
  `secondary sections/frame`, and nonzero `recorder immediate commands/frame: <Vulkan command suffix>`
  counts, including commands recorded after materialization. `External` denotes opaque emitter routing.
- **Diagnostics:** occlusion queries, pass dumps, `TOTK_VK_PASS_PROFILE`, `TOTK_VK_PASS_MERGE=1`
  and pass probes force the original single-thread path with a diagnostic log. `TOTK_VK_GPU_PROFILE`
  (default off) retains recorder workers and asynchronous assembly. Full mode
  (`TOTK_VK_GPU_PROFILE_SAMPLE` absent/`0`) brackets every rendering pass part on its original primary,
  from before BeginRendering to after EndRendering. All secondary chunks contribute to that one exact
  bracket, not separately timed chunk approximations; an explicit pass split creates another pass part.
  Sampled mode uses the same bracket only on selected parts. Empty passes and `REC_THREADS=0` use
  the same timing authority when selected. Optional pipeline statistics use
  independent begin/end queries inside each secondary and sum into the pass; no primary statistics query remains
  active across ExecuteCommands, so recorder profiling does not require `inheritedQueries`. Inline passes keep
  their primary statistics query. The walker allocates chunk queries before queueing, and the assembler uses
  captured end handles/indices without consulting or waiting for the walking profiler.
  Keep profiler mode fixed for worker-count A/Bs; CPU split profiling still measures only the walking thread,
  with worker busy time and waits reported separately rather than added into its exclusive categories.
- **Profiler CPU budget:** draw/pass hooks are O(1) in the number of draws, bound resources and
  earlier passes: first draw hashes at most six bounded shader keys, later draws only increment
  the counter, and pass begin captures at most ten attachments. There are no binding/registry scans,
  prior-pass read searches, audit arrays/candidate checks, allocations or logs in these hooks.
  The old speculative `LOAD-before-full-clear` / `STORE-no-later-observed-read` audit is removed,
  not hidden behind sampling; static attachment operations and logical byte summaries remain,
  computed from pass keys off the walker. Nested timing scopes preserve only their timing parent,
  command buffer and secondary-query state.
  With profiling unset/off, secondary pass closes do not initialize/copy an end token and chunks
  never store/read profiler query metadata. `rec_job`/`rec_pass` retain their non-profiler sizes
  and member offsets: only enabled runs allocate enclosing cold metadata tails. The parked-pass
  close keeps its original one-argument ordinary entry; the enabled token copy has a separate
  profiled entry selected by the draw-side guard, not a token-preserving wrapper around ordinary work.
  Cached `vk_gpuprof_on` guards remain predictable branches, not a claim of zero instructions.
  One opt-in startup preparation creates 32 query-slot leases on the dedicated `totk-gpuprof`
  worker; startup may wait for readiness once. Thereafter the walker takes a prepared lease with
  a short private O(1) mutex operation, without query resets, result reads, formatting or I/O.
  Existing fence completion only detaches/enqueues the fenced lease and releases the game ring slot.
  The profiler worker reads results without WAIT or polling, aggregates, formats, resets queries
  and returns prepared leases. Compact file writes/flushes run on the separate `totk-prof-io` sink.
  The bounded 32-lease FIFO never blocks the game
  for output: no available lease means dropped profiler coverage, recorded by cumulative
  `background_dropped_slots` and missing frame coverage. Its mutex is not the recorder-worker mutex.
  CPU frame closure has its own bounded 256-entry FIFO under the same private background mutex:
  `{frame, end_sequence}` is published at CPU present, independently of whether a command buffer
  is still open. Every sealed submission, including one without a profiler lease, has an ordered
  sequence; canceled, never-submitted leases do not advance it. The worker finalizes a ready closure
  only after processing fenced leases through its end sequence, including closures arriving after
  those leases were already consumed. Overflow increments cumulative `background_dropped_closures`;
  the analyzer takes a per-launch maximum, warns and censors coverage, rather than hiding the loss.
  The cumulative drop count is a per-launch maximum in analysis, not a sum of successive windows;
  coverage stays explicitly censored rather than pretending skipped metadata was observed.
  Worker placement reuses the seed/cache-writer policy: nice 10, the lowest clearly separated
  inherited CPU-capacity tier when available, never widening the inherited affinity mask.
  Actual placement and errors are logged once. Orderly close first stops new renderer-work admission
  and flushes/drains renderer GPU work, then drains the profiler reporter before closing its writer.
- **Profiler coverage:** named `upload`, `transfer`, `post`, `upscale`, `present` and `framegen` scopes
  cover backend-owned Vulkan commands; game compute dispatches retain their own records. In full mode,
  slot envelopes start in pre (when present), include adopted immutable framegen primaries and every main
  primary, and reconcile each frame's pass sum against its first-to-last GPU timestamp span. Sampled mode
  instead has one whole-main-frame pair, excluding pre/slot envelopes, plus selected pass pairs.
  JSONL `gap_ms` is the signed residual, not profiler overhead. In sampled mode it includes intentionally
  untimed passes, but sampled pre work/overlap may make it negative; it is not total untimed work.
  A frame boundary can split a still-open slot without forcing a submission; its GPU marker remains
  supported. Android can instead seal/submit the final slot before CPU present. In full mode the
  ordered CPU closure uses that slot's real envelope end, with **no extra GPU timestamp or submit**;
  it does not need another frame to wake finalization. Frame rows expose `closure_source`
  (`cpu-sealed`, `gpu-marker` or `missing-closure`) and `closure_sequence`; missing closure remains
  invalid. Cross-frame pre uploads can execute before their recording frame's main work; such overlap
  is surfaced by `reconciliation_failed`, never clamped away. External GL/QCOM/LSFG execution,
  compositor work and scanout are outside Vulkan timestamp authority. Profiling requires
  graphics/compute timestamps and hostQueryReset; query pools are host-reset only after their
  existing slot fence, before either pre or main can use them.
- **Profiler size identity (schema 3):** exact keys retain absolute renderArea/attachment dimensions for
  driver-area joins. Each completed frame reports its largest depth+multi-color scene extent; pass attachment
  fractions use that common reference, with the game resolution as an explicit no-scene fallback, not the pass area.
  Analyzer families merge scene-relative MRTs and scene-sized depth prepasses through DRS; fixed depth targets
  keep absolute families. Window rows preserve separate reference variants of the same absolute key.
- **Compact profiler windows:** `TOTK_VK_GPU_PROFILE_ORDINALS=0` (default; only exact `1` enables it)
  aggregates across frame ordinals by exact key + reference dimensions + `scene_relative`; ordinal `0`
  is the all-ordinals sentinel, not a pass position. `=1` restores positive-ordinal window rows.
  `[gpu-profile] window ordinals=0/1` proves the launch policy. Mode-2 raw split logs and mode-3
  selected `long_frame` rows retain actual positive ordinals in either setting. Times, counts, absolute
  areas and explicitly statistics-matched draws/time are still sums, not averages or synthetic zeros.
- **Sampled profiler:** `TOTK_VK_GPU_PROFILE_SAMPLE=0` (including unset) leaves full profiling unchanged.
  A strict integer `1`–`16` enables a per-frame budget `k`; start with **`GPU_PROFILE=1 SAMPLE=2 STATS=0`**
  using the full `TOTK_VK_GPU_PROFILE_*` environment names. Selection rotates contiguous frame ordinals
  deterministically using the preceding frame's observed pass count. Every observed key, draw count,
  absolute area, scene reference and scene role remains available, including passes with zero samples.
  Only selected passes get timestamp/statistics queries. Rotation is not random sampling: periodic key
  churn or a short route can under-cover a key, so inspect coverage rather than assuming representativeness.
  Each frame emits pass-profiler-owned `vkCmdWriteTimestamp2` command `timestamp_count`, including
  deferred ends after assembly, not the game's independent timestamp reports or the optional pipeline pool.
  It is at least the two whole-main-frame endpoints and at most **`2k+2`**. There are no additional pre/slot pairs.
  The whole pair uses completion-accurate `ALL_COMMANDS`, spanning main submissions through present.
  When CPU present finds no open command buffer, SAMPLE must issue **one small marker-only main
  submission** for the real end timestamp (the start was written earlier). This is an explicit extra
  submission cost, not a per-slot end or CPU-time fallback; it keeps the **`2k+2`** write bound.
  With an open command buffer the existing in-buffer end marker is used. The full-mode closure path
  above adds neither a query nor a submission.
  Frame/window `measured_ms` and pass `gpu_ms` sum valid sampled durations only; `gap_ms` is the residual.
  `sampling` retains observed/sample counts, distinct observed/sampled frame counts and a 272-bin histogram;
  `stats_valid` cannot exceed sampled count. No total is extrapolated to untimed observations.
  Analyzer tables aggregate the whole input/measured interval by default; `--per-window` adds a diagnostic
  window breakdown. Means divide sampled duration by sampled count. p50/p90/p99 are merged-histogram
  **bucket upper bounds**, with eighth-octave spacing (at most about 9.1% quantization above 1 ns);
  zero and sub-nanosecond durations occupy the first bucket. Thin coverage warns below 10 sampled frames
  across the interval, not merely because a 300-frame window has three samples. There is no fixed
  percentage gate: 15–30 samples/ordinal in 48 seconds can be useful even near 1% frame coverage.
  Coverage fractions remain visible. Below 100 samples, p99 is flagged descriptive-only, not a reliable
  stutter-tail estimate; histogram precision does not establish statistical confidence.
  Overlapping family frame populations expose bounds rather than fabricated exact unions. A missing key
  observation in a censored capture is not a zero-cost pass.
- **Pipeline/shader group profiler:** `TOTK_VK_GPU_PROFILE_PIPELINES=0` (unset/invalid/out-of-range also off)
  preserves pass profiling. With `GPU_PROFILE=1/2/3`, budget `1`–`16` enables an independent bounded sampler;
  initial capture: `GPU_PROFILE=1 SAMPLE=2 PIPELINES=4 STATS=0` (full `TOTK_VK_` names). Startup logs
  `[gpu-profile] pipeline-budget=...`; requesting only PIPELINES while the parent profiler is off stays off.
  A group is consecutive draws with the same actual `VkPipeline` and exact selected VS/FS variants, within
  one pass part. Changes, inline clears and pass ends close it; returning to an earlier pipeline creates a
  new group. Compute dispatches are separate groups with CS identity and dispatch counts, never graphics
  draws or zero-primitives. The legacy pass `shader` is ONLY its first draw's signature: neither it nor
  the whole-pass draw count is a pipeline/shader work proxy.
  Graphics brackets start before pipeline bind/state setup; ends precede the next pipeline bind or clear/
  EndRendering. Direct rendering writes on its original command buffer. Deferred rendering copies exact
  pool/index/stage timestamp events into the ordered secondary job stream. A group may span several
  chunks, and an end-only chunk is retained before the original pass flush. Workers/assembly never consult
  the walker's current group/pass; pool leases, coordinates and copied variant names survive until the
  existing fence and profiler-worker read/reset. No waits, forced-inline workers or per-group submissions.
  Every frame uses one global ordinal sequence across all passes/groups (not a per-pass prefix).
  `getrandom` supplies uniform 32-bit launch bits once. Population `N` starts at **2048** and grows to
  the next power of two after observing a larger frame (never shrinks, max `2^31` groups/frame).
  Frame `f` selects `k` consecutive positions starting at `(launch_bits + (f-1)*k) mod N`.
  Keeping ALL launch bits preserves uniform phase after growth: no random syscall is needed per frame.
  A growing frame's new tail is explicitly uncovered for that frame, then becomes eligible; stable layouts
  including >2048 groups cover EVERY ordinal within `ceil(N/k)` frames. Failure to obtain uniform launch
  bits disables this sampler rather than inventing authority. Selection is correlated systematic sampling,
  not IID; periodic scenes may alias. Each in-domain group has marginal probability `k/N`, independent
  of draw count/pass length. The Horvitz-Thompson sum is **measured group ms × N/k**, never equal draw
  work or observed/sample scaling. This is conditional on domain coverage and workload independent of
  instrumentation, NOT proof of unperturbed cost or confidence. Headers retain the initial population and
  full launch bits (`pipeline_phase`); compact exposure strata retain actual population and selection weights.
  Actual GPU timing includes timestamp cost and intervening GPU state/setup; ALL_COMMANDS can serialize
  work/change tiled scheduling. A group is not an isolated shader invocation or marginal cost.
  Bounds: at most **2k extra timestamp writes/frame**, `1024 + 64k` metadata records/lease and
  `64k` query pairs/lease for configured pipeline budget k (maximum 16); 32 prepared leases bound
  backlog. Space for ALL selected groups is reserved; optional unselected detail uses at most
  1024 records/lease, with additional omitted detail explicitly counted as `unretained_groups`,
  not a selected timing drop. Thus a long pass cannot crowd tail samples out. Pipeline arrays/query
  pools exist only with PIPELINES enabled. Parent timestamp pools similarly pack the configured
  SAMPLE budget (386 query slots at SAMPLE=2, rather than the full-mode 4226); statistics scratch
  arrays exist only with STATS enabled. Headers report all actual capacities and CPU lease bytes.
  The walker compares/borrows three immutable shader-name pointers, never copies names, scans bindings
  or allocates group storage. Published shader objects never move or die (`vk_shader.c`); names are
  initialized before publication. Leases therefore keep valid references after GPU fence retirement.
  The worker copies canonical full names only when interning a new identity. Exact names still merge
  aliases from different source objects; stable source tuples avoid repeating string comparisons.
  Pass `timestamp_count` remains unchanged; sum compact coverage `timestamp_count` for the extra pool.
  With pass sample budget `s`, the combined normal-frame write bound is **2s + 2 + 2k**.
  Default pipeline output is compact: schema-3 header `pipeline_output:"compact"`,
  `pipeline_compact_version:1` and `pipeline_window_columns` publish the numeric column order.
  `pipeline_key` interns actual launch-local Vulkan handle + kind + full VS/FS/CS tuple, never pass key
  or ordinal. Only keys selected within a 300-frame window emit `pipeline_window` numeric arrays.
  Sums, sum-squares, actual-denominator weighted sums and eligible-domain counterparts remain separate;
  moments are descriptive, not IID standard errors. `pipeline_window_coverage` retains completed-frame
  counts, domain exclusions, selected/valid/missing samples, timestamps, optional omissions and true loss.
  `population_exposure` entries are `[N,k,N,frames,eligible_frames,observed_groups,selected_groups]`,
  so population growth within a window never borrows the latest denominator for earlier samples.
  Eligibility is committed only at completed-frame closure, after every split lease/nested scope.
  Missing/invalid selected samples, dropped slots/closures and explicit key/frame/aggregate overflows
  withhold unsupported estimates; unknown remains unknown. Suppressed unselected-only key observations
  also count as `unretained_groups`, making count subtotals honestly incomplete.
  `TOTK_VK_GPU_PROFILE_PIPELINES_RAW=1` restores the old per-group/per-lease-frame and global frame
  rows. Default compact suppresses all three, retaining parent pass/window rows (and explicit mode-3
  long-frame diagnostics). Parent profiling with PIPELINES off is unchanged. Startup logs
  `[gpu-profile] pipeline-output=compact/raw`. Raw `pipeline` rows name frame/global group, pass
  key/ordinal, handle, full variants, emitted draw/dispatch counts, primitive knownness, selected/status,
  nullable `gpu_ms`, timestamp count and probability numerator/denominator.
  Primitive counts include instances and generated quad triangles, not clipped/rasterized primitives.
  Indexed restart strips are unknown without index reads (`primitives:null`, known subtotal and unknown
  draw count); compute is `null`/`not-graphics`. This backend lowers guest draws to concrete direct draws:
  it does not synthesize zero primitive counts for unavailable GPU-indirect argument buffers.
  Raw mode emits per-lease/frame `pipeline_coverage`: observed/dropped/selected/unretained/out-of-range
  groups, timestamp count, lease/submission IDs, population/phase and discard state. Unselected or
  unavailable/invalid/incomplete/discarded timings have `gpu_ms:null`, never fake zero durations. Setup
  attempts rejected before drawing are explicit `no-work`, a missing selected sample rather than zero cost.
  A frame larger than its domain censors the WHOLE-capture `estimated_gpu_ms`. The analyzer separately
  ranks `eligible_estimated_gpu_ms` over its explicitly named domain-covered frames, with exclusions
  retained as `domain_excluded_frames`; it never pretends that subtotal includes the growing frame's tail.
  True metadata/query loss or missing authority still withholds estimates. Omitting only unselected
  detail does not change inclusion probabilities, but draw/primitive sums become retained subtotals
  (`counts_complete:false`), not whole-workload counts. Zero samples mean unknown cost. Pipeline handles
  remain launch-local; only full shader variants are merged across launches.
  Join seed manifest/module hashes externally before treating name parity as exact device seed parity.
  Calibration: first run all profiling off versus `GPU_PROFILE=1 SAMPLE=2 PIPELINES=4 STATS=0` on the
  same signed APK, driver/seed, warmed save/route, FPS cap, render size and thermal state in **A/B/B/A**
  order; collect equal stationary and moving intervals and report CPU frame p50/p95, GPU span, fps,
  query counts and drops. Then hold pass sampling fixed and ABBA PIPELINES=0/4 to isolate incremental
  cost. Repeat fresh launches (independent phases), extend beyond several rotations and require adequate
  per-pair samples before ranking. No device calibration was performed as part of the implementation.
  Supported pipeline-capture qualification is limited to **≤30 fps** (Duo or capped Thor).
  The 600-second 30-fps model has zero drops; the shared-host 45-fps models do not qualify.
  Uncapped Thor remains experimental pending its own O/P/P/O check. No automatic cap or larger pool
  hides this limit: retain the footer/drop counters and fail closed on any loss or missing authority.
  Compact active state is fixed: 8192 exact keys, a 16384-cell open-addressed index, 8192 frame-touched IDs and
  64 exposure strata: **4,427,008 bytes** on the supported 64-bit ABI. Compact mode also allocates exactly
  one enabled-only frozen report snapshot of that same size, plus a `gp_json` diagnostics snapshot
  (8192 data bytes and bookkeeping). Header/footer aggregation memory is
  `2 * sizeof(gp_compact) + sizeof(compact_report_diagnostics)`; this is separate from the leases and sink.
  Pipeline lease storage is separately bounded by 32 leases and the configured budget (including
  pipeline metadata, parent records and readback arrays; excluding Vulkan driver/query-pool storage).
  SAMPLE=2/PIPELINES=4/STATS=0 uses **31,075,072 bytes** on the host 64-bit ABI, down from compact2's
  **51,980,288 bytes** (−40.2%); header `lease_memory_bytes` publishes the actual build's size.
  At SAMPLE=2/PIPELINES=4/STATS=0, each lease has 1280 pipeline records, 386 parent and 512 pipeline
  timestamp query slots; across 32 leases that is **28,736 query slots**, versus compact2's
  **200,768** (32 × (4226 + 2048)). Actual driver bytes are not inferred from query counts.
  This is not RSS. A subsequent compact2 Duo pan exhausted all 32 leases (74 drops in frames
  4801–5100), despite aggregation averaging only about 160 ms per 300-frame gameplay window.
  Its worker peak was 32 **already-fenced queued/active leases**; the renderer has only eight slots,
  so GPU latency alone cannot explain that exhaustion. The old two-leases/frame host model missed
  the device's approximately 18 submissions/frame and unmeasured writer/scheduling stalls.
  The consumer requests nice +5; the independent blocking sink requests nice +10. Both retain
  inherited affinity, without extra pinning. Effective consumer nice/error/mask are in the header.
  Compact serialized bytes use a **1 MiB fixed FIFO**, with a **256 KiB sink-thread stack** plus
  fixed mutex/cursor state (`output_sink_memory_bytes` reports its actual size). A full FIFO never
  blocks the consumer: it increments `output_sink_overflows` and `output_failures`; the analyzer
  withholds estimates. Bounded rotation markers keep rotation IO on the sink as well; only terminal/
  test close waits for prior writes, and close joins the sink after writing the footer.
  Raw pipeline output and non-pipeline modes retain synchronous IO for their diagnostic fixtures.
  Worker service CPU/wall, readback/reset/report wall totals, maximum service and queue-wait wall
  time are cumulative window/footer counters. The footer retains at most 64 drop-frame summaries:
  `[frame, count, peak_queued, peak_consuming, peak_inflight, phase_bitmask]`; additional attempts
  are counted in `background_drop_frame_overflow`. These are per-frame maxima, not additive
  instantaneous occupancy. Phase names are in the header. No per-event logging or syscalls occur
  on draws; enqueue/service clocks are per submission.
  Bucket selection mixes the pipeline handle; full shader strings and compute kind still determine
  exact identity, including reused handles. Shader bytes are neither hashed per observation nor copied by the walker.
  Each pipeline lease record is 104 bytes rather than 320: hot counters and borrowed source pointers,
  not a 240-byte shader tuple copied per group. The dictionary owns the capture text; pointer equality
  is only a cache over already-proven immutable source objects, never a weaker exported shader identity.
  Borrowed names point into each published `vk_shader`'s inline `key[80]`, copied before cache publication.
  Published shader objects have process lifetime: variant-list refresh, program release, seed-IO shutdown
  and pipeline failure cleanup do not reclaim them. Native presentation/post/framegen helper modules
  are raw Vulkan objects, not temporary `vk_shader` name owners passed to the group profiler.
  Any future shader reclamation must retain names through reporter shutdown, including frozen reports;
  a completed GPU fence alone is insufficient. The GPU-free `test_vk_shader_lifetime.py` pins this contract.
  Optional count columns stay exact integers in the hot loop, with the same 2^53−1
  representability guard; only selected keys enter the per-frame floating-point moment-reduction list.
  A frame-ordered cursor walks each immutable lease once across frame segments. At each completed frame,
  pending numeric/text encoding visits at most **64 key indices**; all 8192 indices drain within **128 frames**
  of the next 300-frame window. A window edge freezes one bounded snapshot and resets active state instead
  of encoding the full report in a burst. Dictionary strings are encoded only when needed for the retained
  file; ordinary JSON string runs use bounded copies, not per-character printf calls. Numeric window columns
  use one fixed-schema encoding call. No new thread or report queue is introduced; the existing 1 MiB sink
  remains unchanged.
  Parent table occupancy is a 2 KiB bitmap; reports visit/clear only live groups instead of scanning/clearing
  a multi-MiB cold table. JSON initialization resets cursors, not whole 8192-byte scratch arrays.
  The compact index is rebuilt only after actual eviction leaves tombstones, not on insert-only
  windows; clean dictionary bucket positions remain valid across reset.
  Report-edge CPU therefore pays a bounded snapshot copy and reset, not full numeric/text encoding;
  the subsequent encoding is amortized rather than hidden with more leases.
  The compact aggregator makes no per-group/window allocations; existing parent pass buffering/histograms
  remain separately bounded. Inactive previous-window dictionary entries are
  evicted under lifetime churn; IDs are never reused. Current-window entries cannot be evicted: saturation
  is an explicit overflow, not a silently weakened estimate. A referenced key is announced before its
  summary in each file; reserve only that key/summary pair on rotation, never replay an entire dictionary.
  Losing earlier rotated files still loses coverage and cannot silently regain full-capture authority.
  Terminal close and sparse window jumps synchronously drain outstanding frozen reports. Close flushes a
  remaining partial window once; unfinished frames are explicitly excluded/overflowed, and in-flight GPU
  retirement remains unknown.
  Coverage reports `CLOCK_THREAD_CPUTIME_ID` aggregation and key-encoding/flush CPU milliseconds,
  serialized bytes before the coverage row (including parent rows and repeated rotation headers),
  peak queued+active leases and the fixed aggregation bound. The footer reports cumulative serialized
  bytes, backlog and close-flush thread CPU. File retention size is not cumulative emitted bytes.

  Host-only fixture, with outputs kept in a fresh scratch directory:
  build `make -C games/totk/android TARGET=linux CC=cc OUT=<absolute-scratch-build> vkgpuprofcompacttest`,
  then `<absolute-scratch-build>/vkgpuproftest --compact-contract` (no GPU), and
  `vkgpuproftest --compact-fixture --seconds 600 --fps 0 --pace 0 --loss 0`.
  `--fps 0` cycles 20/30/40/50/60 logical fps every 20 seconds; `--fps 60` is the size upper-rate arm.
  4096 groups/frame span two leases, each retaining 1024 optional unselected records plus selected groups.
  Recurrent keys appear/disappear between windows and a fresh-variant tail forces bounded lifetime churn.
  `--frames 302` exercises partial close; `--loss 1` injects unavailable/invalid/incomplete queries, true
  group loss and one dropped-slot counter. RAW=1 replays identical deterministic identities/timestamps/
  phase for analyzer comparisons. Tiny MAX_BYTES exercises dictionary reannouncement/retention eviction.
  The fixture prints cumulative bytes/average window, worker thread CPU/average/peak window, backlog,
  aggregation/lease and parent-aggregation memory bounds, zero compact allocation count, write-call count,
  producer wait and close CPU/wall time. `--pace 0` is accelerated throughput,
  not 600 wall seconds. `--pace 1` uses monotonic absolute wall pacing. Without `--nonblocking 1`,
  both wait for a free synthetic lease to keep datasets identical; producer wait and wall time MUST
  be reported. Production-rate runs use nonblocking admission. Backlog is queued+active host leases,
  not GPU latency. This excludes real query calls/GPU/driver/device scheduling and proves no Duo result.
  `--worker-scale 2700` charges 2.7x measured lease service to `CLOCK_THREAD_CPUTIME_ID`, using
  test-only CPU work rather than per-lease nanosleeps. Host descheduling is never multiplied;
  explicit coupled/sink stalls independently model IO. Reported model worker CPU includes this
  synthetic work and must not be presented as production aggregation overhead.
  Paced producers default to `--catch-up 0`: a missed whole-frame deadline rebases future pacing
  without skipping any logical frame or submission. `--catch-up 1` explicitly restores the old
  absolute-deadline burst stress. Output counts late producer frames/slots, worst lateness and
  rebased seconds; report those plus actual wall time, not just the nominal fps.
  For a 600-second Duo/Thor model use `--seconds 600 --fps 30` or `--fps 45`, plus
  `--pace 1 --parts 18 --gpu-held 8 --nonblocking 1 --sample-budget 2 --spread-percent 60
  --writer-stall-ms 200 --worker-scale 2700 --catch-up 0`: 540/810 nominal submissions/second.
  Eighteen submissions/frame is measured for Duo but assumed for Thor. Keep failed legacy/burst
  captures; a corrected host model is not equivalent to that stress or a real GPU/device gate.
  Check stable low backlog, producer waits, queued/processed equality and the writer-end footer;
  a larger pool with growing backlog is not success. `--record-fixture` measures production pipeline
  hooks and lease handoffs at 4096 groups/8192 draws per frame. Its counted timestamp sink excludes
  driver command encoding: report this CPU lower bound separately from real Vulkan/device recording.
  To exercise the observed mechanism, run `--compact-fixture --frames 330 --fps 30 --pace 1
  --parts 18 --gpu-held 8 --nonblocking 1 --sample-budget 2 --spread-percent 60 --writer-stall-ms 200`.
  Submissions span 60% of each frame (~20 ms at device 30 fps, ~10 ms at 60 fps stress), matching the
  measured recording interval rather than an instantaneous 18-lease burst. It uses the exact production
  no-free-lease path and retains up to eight synthetic GPU-held leases. `--coupled-stall 1` is a
  test-only control that stalls the lease consumer at the same output point; it must reproduce
  pool exhaustion. The default stalls only the independent sink and must drain without lease
  drops, output overflow or missing writer-end. `--sink-contract` checks FIFO overflow, nonblocking
  rejection, byte identity across ring wrap and final drain.
  Leases already cover an entire actual queue submission, including its pre/main primaries and
  every pass, not individual draws/secondary command buffers. Sharing across independent fences
  or frames is not a safe local rate reduction: cancellation, multiple frame markers per submission,
  immutable publication and query reset all rely on that ownership boundary.
  Host commands run with `taskset -c 0-3 nice -n 10`; matrices also hold the shared hosttest flock.

- **Profiler capture lifecycle:** the default is 128 MiB per JSONL file × four files; compact windows
  target a 6–8-minute pan without rotation, not a guaranteed duration for arbitrary pass counts/raw modes.
  Each rotation repeats a schema-3 header with launch ID, APK `assets/build.txt` commit, package/versionCode
  when available, whitelisted launch environment, effective profiling switches and Vulkan driver identity.
  Exact APK/driver hashes must be joined from the capture artifacts; they are not invented in process.
  Monotonic window sequences and rotation indices let the analyzer flag complete missing windows and
  retention eviction. Orderly Android close/destruction stops new renderer-work admission, flushes/drains
  renderer GPU work (including submissions still in flight at shutdown entry), then drains/joins the
  profiler reporter and writes `session_end` under the writer lock. `clean_end` reports successful writer
  closure independently of retirement. New footers always carry boolean `all_queries_retired` and
  `query_retirement`: nonnegative integer `leases_acquired`, `leases_submitted`, `leases_retired`,
  `leases_cancelled`, `leases_outstanding`, `queries_submitted`, `queries_retired`, `queries_cancelled`
  and `queries_outstanding`. Acquired leases equal retired + cancelled + outstanding; submitted leases
  include retired leases and exclude cancelled ones. Outstanding queries equal submitted minus retired;
  `all_queries_retired` is true iff both outstanding counts are zero. Retired leases have fenced results
  read/attempted; retired queries require available results (successful statistics reads), so unavailable
  queries can remain outstanding even after every lease was processed. Counts cover activated timestamp/
  statistics query instances, not pool capacity or the six statistics counter values. Cancelled counts
  describe activated queries/leases in abandoned, never-submitted recordings, not submitted work.
  Historical footers without `query_retirement` retain nullable `all_queries_retired: null` as unknown.
  Force-stop/crash/kill normally has no footer; the analyzer reports missing closure, not a fabricated
  end marker or GPU failure. Missing closure censors both raw and compact pipeline estimates
  (`streams_without_end`), even when retained complete windows are internally consistent. The footer also retains `pending_frame_closures`; zero pending CPU closures
  alone does not establish query retirement. The additive header `background_closures: 256` records the
  closure FIFO capacity, independently of the 32 query leases.
  Enabled GPU draining has one two-second budget. On expiry, `query_drain_timed_out: true` accompanies
  the retirement accounting; uncompleted leases and unread queries remain outstanding rather than
  being fabricated as retired. The additive timeout field is boolean when present; old footers without
  it remain unknown in analysis. Profiling-off shutdown is a no-op and does not flush or wait for GPU work.
  The explicit host option `gpuprof.py --frame-close-replay` adds conditional results only for
  header-attributed d003f2d full-mode captures with complete contiguous windows, zero loss/failure
  counters, available valid raw GPU endpoints/domain, bounded recomputed intervals, nonnegative gaps
  and consistent frame/pass/window sums. Original invalid rows and ordinary totals remain untouched.
  Missing authority stays unknown/rejected; this cannot establish device correction or performance.
  The old host fixture kept a command buffer open at present and exercised the GPU-marker path,
  missing Android's sealed-then-present ordering. The new host scenario covers both already-consumed
  and not-yet-fenced submissions and an isolated final CPU closure, not new Android evidence.
  Each GPU-blocked close also acquires/cancels a profiler lease on a real independent, never-submitted
  command buffer, and requires the exact cancellation count in the footer. It does not assume
  optional renderer admission succeeds: that API deliberately uses a nonblocking mutex trylock
  and may reject submit/completion contention even with free slots. The separate `--cancel` case
  retains optional-slot cancellation integration coverage.
  The host `--shutdown-inflight` scenario enters shutdown with a verifiably unsignalled real GPU fence
  and no prior profiler flush; its modes 1/3 × workers 0/2 × stats 0/1 × SAMPLE 0/2 matrix requires
  complete frame output and zero outstanding lease/query counts after shutdown, not merely writer closure.
  Profiling-off runs require that same pending work to remain undrained by the shutdown no-op.
  `--shutdown-timeout` holds the real event beyond the two-second drain budget; workers 0/2 and SAMPLE
  0/2 require a clean writer end with timeout=true, false retirement and positive outstanding counts.
- **Invalid span authority:** timestamp reads request 64-bit values **and availability**, without WAIT;
  an unavailable endpoint is never a zero-duration sample or silently replaced by a later endpoint.
  Each retired slot captures its immutable device, queue, queue family, `timestampValidBits` and period.
  Both endpoints and cross-slot adjacency must have the same domain. The renderer uses queue index zero
  of its one graphics+compute family (`V.queue`) for pre/main/framegen command buffers; worker secondaries
  inherit that submission, not a separate worker queue. Device/queue/domain changes invalidate a span.
  Counters are masked to the family's valid bits before modular subtraction; a genuine short counter wrap
  is accepted and counted. Modular arithmetic alone cannot distinguish a wrap from a backwards/stale
  endpoint: every individual interval and whole frame must also be at most 10,000 ms.
  This bound is a conservative corruption guard, **not** proof that available values have no driver fault.
  Thor's 48-bit/52.0833320618-ns counter wraps after roughly 169.7 days; the old huge positive spans
  were backwards endpoints misread as that wrap. Old files lack ticks/availability, so their original
  driver/query failure cannot be reconstructed.
  Frame/window `span_valid=false` makes `span_ms`/`gap_ms` null, with per-window unavailable/invalid
  timestamp and invalid-span counts plus raw forensic endpoint fields retained. A valid span requires
  every contributing endpoint/interval and complete records. Full mode also requires nonnegative
  reconciliation within timestamp quantization tolerance; sampled negative residuals alone do not
  invalidate main-frame authority. Windows retain `valid_span_ms`/`span_frames` separately if any frame is invalid.
  The analyzer also rejects implausibly huge older positive spans despite an old success flag.
  Explicit `--legacy-span-replay` accepts schema-2 frame/window timing only; it cannot recover missing
  query availability or reinterpret old pass-area semantics. No mobile overhead benefit is claimed.
- **Odin attribution correction (2026-11-05):** the fixed-order Hateno report
  `.scratch/bench/rpmode/odin-overhead/report.md` for APK `716c122ff09b` has timestamps-only P1
  at **21.55 fps / 43.750 ms recording / 36.6% GPU busy**, versus C0 at
  **57.96 fps / 10.789 ms recording / 80.2% GPU busy**. Recording adds **32.961 ms/frame**,
  and the GPU spends 91.12% of its clock residence at 660 MHz. These CPU observations and reduced
  GPU feeding supersede the earlier GPU-dominant explanation of this collapse. Stats P2 adds only
  0.55 ms recording and -0.15 fps versus P1 (inside the 0.51-fps control bracket); its independent
  FPS cost is unresolved. Different timestamp/NVN GPU scopes, cache/compile contexts and fixed order
  prevent a measured mechanism-by-mechanism split.
  Before cutover, `vk_gpuprof_draw` scanned up to 2×160 texture/storage-image bindings per graphics
  draw even on unselected passes; each live handle called `vk_treg_get` (`vk_res.c`), which took
  `g_reg_mu` and copied a registry entry. `vk_gpuprof_read` then searched all earlier pass audit
  records, while pass end appended/reallocated audit storage and present scanned/logged candidates.
  This source identifies nonconstant CPU work, not a measurement that the audit alone cost 32.961 ms.
  The removed liveness audit could not prove safe discards across frames/raw-memory consumers anyway.
  Host CPU benchmarks and a new matched device A/B are separate acceptance evidence; this change
  does not claim measured mobile recovery.
- **Profiler cost is not passive:** in the inspected Turnip source
  (`turnip/wt-rpmode/mesa/src/freedreno/vulkan/tu_query_pool.cc:2100–2144`), every
  `ALL_COMMANDS` timestamp issues a WFI before reading the always-on counter; its comment explicitly
  calls out CP stalling. Full profiling adds two such timestamps per timed pass/scope plus frame/slot
  boundaries. Sampled mode reduces this to at most `2k+2` writes/frame; it does not make the remaining
  writes free. Modes 2/3 do not reduce the selected query cost: mode 2 adds raw CPU logs, and mode 3 adds
  history memory/output for selected long frames, retaining untimed pass metadata as well.
  Statistics add query start/stop barriers per secondary chunk (or inline primary) of selected passes,
  not merely once per aggregate pass. Turnip's `emit_begin_stat_query`/`emit_end_stat_query`
  (`tu_query_pool.cc:1157–1222,1674–1733`) start/stop counters, WFI, snapshot all statistic registers,
  then subtract/accumulate results with memory-write waits. In a tiled pass that draw stream can execute
  repeatedly. Vertex-stage statistics explicitly disable concurrent binning for GMEM passes
  (`tu_cmd_buffer.cc:3479–3486`); secondary draw streams replay per tile (`:3817,6524`).
  These are real GPU instrumentation costs, not evidence that they dominate the observed Odin
  FPS loss. Host query resets are CPU memory writes; retired result reads use no WAIT flag
  (`tu_query_pool.cc:678–789,1075–1086`), so they do not intentionally add a new queue/fence wait.
  Removing the CPU audit and moving reporting off the critical path does not remove these GPU barriers.
  Always isolate profile-off, sampled timestamps-only, sampled statistics, then full profiling under
  identical driver/TU_DEBUG settings. Sampled duration representativeness is conditional on the remaining
  instrumentation **not serializing or otherwise changing GPU execution**; statistics still perturb the
  passes actually sampled. Device A/Bs must compare GPU clock **time-in-state deltas over equal intervals**
  with an unprofiled control, alongside fps, power, temperatures and thermal history. A single MHz reading
  is not equivalent. Do not interpret any instrumented span as uninstrumented game performance.

Pipeline handle selection deliberately remains on the walking thread in this cutover. `pipe_get`'s cold asynchronous
misses skip draws; emitted draws immediately update cumulative synthetic samples, used by later reports and conditions.
Deferring that decision changes stream-visible behavior unless the walk blocks at each such draw. Offscreen captures
also wait for queued prewarm entries rather than omit persistent lighting. The measured pipeline category is only
0.8–1.2 ms (versus 7.3–10.6 ms binding and 6.3–11.7 ms commands), so preserving that ordering is preferred to adding a
pipeline-map lock and per-draw rendezvous. Existing compiler workers still create cold pipelines asynchronously.

Recorder controls are read once when the first Vulkan slot opens: `TOTK_VK_REC_THREADS` defaults to **2 on Android**
and **0 elsewhere** (range 0–4); explicit values, including synchronous `0`, override the platform default.
`TOTK_VK_REC_CHUNK_DRAWS` defaults to **128** (clamped 1–512); `TOTK_VK_REC_INLINE_DRAWS` now defaults to **0**
(clamped 0–512). Zero moves tiny draws, clears and timestamp recording to workers; nonzero is an explicit inline
comparison override. Worker-bound tiny jobs coalesce across passes, up to four jobs or 32 draws per queue item.
`TOTK_VK_REC_LEAD_DRAWS` defaults to **32** (clamped 0–chunk size): the first worker-count chunks in a pass use
the smaller limit, starting every worker earlier; **0** restores uniform-sized chunks. Later chunks use the configured
chunk size (default 128).
This changes neither rendering instances nor GPU submission boundaries.
These are independent of `TOTK_VK_CHUNK_DRAWS` (GPU boundaries: 75 by default; 600 when resolved protected lag
is on and native VMA pressure is absent, otherwise 75; an explicit setting including 0 wins) and
`TOTK_VK_COMPILE_THREADS` (pipeline compiler workers, unchanged default 2). Worker creation failure reduces the count
to the workers actually started, with zero retaining the original path.

**Worker wakes** (`TOTK_VK_REC_WAKE_BATCH`, default **0** = off): every queued chunk batch signals a parked worker,
and the walker pays the futex wake (T7: 0.6 ms/frame Odin, 1.0 Duo; the default also signals workers already woken).
`1` signals from the walker only when no worker is already on its way to the queue; a worker that takes a batch and
leaves more queued wakes the next parked worker. `n ≥ 2` also lets an idle worker poll the queue for up to `n` µs
before it parks, so steady chunk streams need no wake (CPU spent polling; try 50/200 against 0). `recorder
wakes/frame` (every 300 presents) counts walker and worker signals, polled hits and parks. Live at the next enqueue;
`vkrectest` compares bytes with 1 and 200 at 2/3 workers, and `--lag-walk-bench` prints wakes per case.

Every 300 presents, `recorder frame ...` reports per-present walking elapsed time excluding GPU and worker waits,
each of four possible workers' busy elapsed time (unused entries zero), worker wait time, and parallel/inline chunk
and pass counts. Worker busy includes preemption and Vulkan calls, not thread CPU time. `TOTK_VK_REC_PROFILE`
remains exclusive walking-thread accounting: moved work is reported by the recorder busy fields, not its P buckets;
its original Amdahl estimate is meaningful only with recording threads disabled.

Every 300 presents, with or without recorder workers, the backend also logs
`descriptors frame <n>, per-frame: sets … allocate-calls … pool-resets … walker-ns … list-ns … lists … template-ns … templates …`.
`sets` are successful driver set allocations, `allocate-calls` every `vkAllocateDescriptorSets` attempt (pool-full
retries included) and `pool-resets` actual `vkResetDescriptorPool` calls, summed over recorder workers (published once
per job) and the primary slot owner (immediate graphics, compute, post, presentation). The three `-ns` columns are
elapsed time and stay 0 unless `TOTK_VK_REC_PROFILE=1` or frame tracing enables timing; `walker-ns` includes the list
and template times. `vkdrawtest --lag-walk-bench` prints the same counts per measured frame, and the driver-level
`vkAllocateDescriptorSets`/`vkResetDescriptorPool` counts when `TOTK_VK_TEST_CALL_COUNTS=<file>` is set.
The same line ends with what is retained, in both arms of `TOTK_VK_DESC_STOCK`:
`retained: pools N (recorder R, primary P) ~M MiB, stock sets S`. N counts the descriptor pools that exist (a pool is
never destroyed by the default policy, only reset, so N is each owner's peak); M is N times one pool's approximate
driver memory, with every declared descriptor at 64 bytes, a combined image sampler at 128 and a sampler at 16 (about
7.4 MiB a pool; a driver that reserves a pool's descriptors when it creates it keeps them until the pool is destroyed);
S is the sets held in the stocks. Compare M with the kgsl / `memory` sampler figure of the same window.

Every recorder context (1,024 sets) and primary slot (4,096 sets) makes its descriptor pools from one shape
(`vk_desc_pool_sizes`): combined image samplers and uniform buffers 32,768, sampled images and storage buffers 8,192,
samplers 8,192 (raised from 4,096, which was one per set in a primary pool, so a layout with two separate samplers
filled it at half its sets), storage images and uniform texel buffers 2,048, storage texel buffers 1,024. These are every
type a backend layout uses; input attachments and the `*_DYNAMIC` types are used by none and are not declared, since
each declared descriptor is driver memory reserved in every pool. A type no pool declares has zero capacity and would
fail even a fresh pool (DIE); shader loading now refuses such a layout, or one larger than a pool, with
`descriptor set layout of <shader>: ...` (`vkdrawtest --descriptor-stock` checks both and allocates every type).

`TOTK_VK_DESC_STOCK=1` (default **off**; logs `descriptor stock: on/off`) keeps each recorder context's and primary
slot's descriptor sets per exact layout (batches 1, 1, 2, 4 … 32) and reissues them, always rewritten, only after that
slot's own retirement, where every pool used to be reset; pools reset only after 32 generations under quarter use.
Measure with `descriptors frame`: warm `allocate-calls`/`pool-resets` drop to about 0. Host `--lag-walk-bench 32`, frames
16–31, allocate calls/resets per frame: threaded stable 263.88/2 → 0.06/0, churn 906.56/2 → 0.25/0; synchronous 70/1
and 140/1 → 0/0. Live (`totk.ctl`): each recorder context and primary slot switches at its next slot generation,
where it used to reset its pools; switched off, it frees its stock and destroys its pools, returning the retained sets'
memory (the off policy grows pools again as needed). `descriptor stock: on/off (live; ...)` logs the change once.

`TOTK_VK_DESC_STOCK_MAX_SETS=N` (default unset = no ceiling, today's stock; logs `descriptor stock ceiling: N sets
per owner ...` or `descriptor stock ceiling: none`) is a hard per-owner ceiling for the stock arm: an owner's stock
never holds more than N sets, a checkout past N is allocated outside the stock, and at the handoff an owner that
needed more (set limit) or holds more than ⌈N / pool sets⌉ + 1 pools (pool limit: 1,024-set recorder and 4,096-set
primary pools) resets its pools and destroys those past the limit, so the arm falls back to reset-and-allocate and
never keeps more than that. The descriptors line adds `stock sets S (ceiling N/owner)` and `stock resets
off/pressure/set-limit/pool-limit a/b/c/d pools-destroyed e` per frame: steady `set-limit` or `pools-destroyed`
above 0 means N is below the scene's demand (each such generation allocates as the off arm does, and pool churn costs a
pool create per destroy). Live (`totk.ctl`, 0…1048576, 0 = none) at the same generation handoff; a lower value applies
there at once. Host `--lag-walk-bench 32` (REC_THREADS=2, FRAME_LAG=1, stock on): stock sets 8,000 → 859 at N=128,
pools 16 in both. `vkdrawtest --descriptor-stock` checks the set limit, reuse under it, live lowering, the pool limit
and live off on one recorder context, and the primary slots' fallback below the ceiling.

Why a live switch-off (or a ceiling reset) may destroy pools there: `vkDestroyDescriptorPool` and the default policy's
`vkResetDescriptorPool` need the same thing, that no pending command buffer uses a set of the pool, and both happen at
the same point. A primary slot reaches `slot_open` only with `busy` (its fence observed by the completion thread),
`submit_busy` (the async submit owner) clear and no replay pin (`replay_until`: frame-generation owner leases and
external split continuations, released only once `g_done` reaches the covering ticket); a cancelled optional
pre-submit never reached the GPU. A recorder context recycles at its first job of the slot's next incarnation, which is
dispatched only after that open; its secondary leases have returned (`DIE` otherwise) and contexts belong to one slot.
Frame-generation UI replay re-binds sets of the frame being sealed, and extraction refuses split submissions.
`vkdrawtest --descriptor-stock-live` toggles `TOTK_VK_DESC_STOCK` and its ceiling through the live channel every three
frames over 240 frames of 64 draws (each its own UBO, tile and set) with six frames in flight, checks every frame's
pixels, and counts, at each reset or destroy by either owner, whether the slot's previous ticket had retired (the
primary one also checks the submit owner and replay pins); `vkreplaypintest` stages a toggle with every churn
submission while pinned slots are skipped. Both pass at 0/2/3 recorder workers, stock launched off and on, plain and
under ThreadSanitizer (`LP_NUM_THREADS=0`, no suppressions) with zero violations; no Khronos validation layer is
installed on the host.

`TOTK_VK_STAGING_RETAIN_MB=N` (default unset = no cap; logs `staging retention: N MiB per slot` or `staging retention:
no cap`) caps what each of the 8 slot arenas keeps after its reset. Texture-upload staging comes from the slot arena,
and a reset keeps what the submission needed (compacted into one high-water chunk), so an upload burst stays allocated
in that slot for the session; with N (8…4096) a slot keeps whole chunks up to N MiB and clips the high-water hint to N,
and a later submission that needs more allocates it again. Live (`totk.ctl`) at each slot open, after its fence. The
per-300 `memory staging retention: cap N MiB per slot, trims T, M MiB not retained` line counts it; compare
`memory recording: slot/... arenas retained`. Host `vktest --staging-retain` (a 45 MiB burst, then 16 quiet
submissions): the slot arenas keep 101 MiB uncapped, 72 MiB with N=16 (the burst slot 45 → 16 MiB).

`recorder overhead ...` additionally separates synchronous secondary recording, batched ExecuteCommands, walking
and worker pool-reset time, and job setup. Reset is a subset of inline/worker busy time, not another additive cost.
Worker pool resets are lazy on their owner; only generation/version bookkeeping remains in walking slot reset.
Worker waits now occur only while assembling a submission, not at each logical pass end; the execute field includes
finalizing parked primary sections. Primary arrays and pass records are retained per slot and reset after its fence.

`recorder memo per frame ...` reports whole-program and binding-freeze hit/miss counts. Recorder-only program
results are owned by each immutable-serial program (no direct-map collisions) and keyed by its **active semantic
signatures**, not dirty flags or signatures of unrelated stages. Dirty/registry-changed signatures are recomputed;
an unchanged result reuses the cached shaders, matching the ordinary resolver's existing current-variant rule.
Sampler-only texture-handle changes do not invalidate texture-format signatures in worker mode.
Binding freeze reuses immutable descriptor ranges **inside one job**. Chunk/submission/shader/pass size and resource,
sampler/span versions remain exact guards. A changed stage binding revision is revalidated against exact shader-used
binding values; unused bindings do not force a rebuild. A 256-entry uniform-range invalidation journal proves whether
changed log generations overlap that shader's uniform inputs. Overflow, missing history and snapshot invalidation
conservatively miss; storage/texel-buffer stages retain whole-generation invalidation for frozen-content dependencies.
Aborted draws invalidate descriptor-range identity. Writable stages never memoize their GPU-write tracking.
`recorder freeze/program miss causes/frame` gives the top three overlapping causes plus uniform/binding/span/program
counts; `recorder memo recovered/frame` counts revalidated components (not necessarily complete hits if another key
still misses). Threads=0 keeps the ordinary resolver/binding path. No worker reads these walking-only memos.

### Thor regression and revised split

The first device iteration was slower: Lookout recording **33.0 ms (0 workers), 34.9 (2), 35.5 (3)**; sweep FPS
**26.4 → 23.1**; Gerudo recording **18.6 → 20.7 ms**. The supplied split profile showed walking resolve/bind still
**5.1/9.9 ms**, O **9.1 ms** (worker wait **3.9 ms**), and **180 → 257 passes/frame**. Blanket clear/report
boundaries caused the extra passes; constructing all descriptor infos before copying packets left most binding work
on the walk. These are observed regressions, not an MT speedup claim.

The revision removes those boundaries, uses raw storage/vertex/index inputs with frozen span versions, and directly
captures only stream-position-sensitive uniform/resource values. A covering unchanged uniform snapshot now supplies
its existing buffer info without re-resolving its guest pool. Index eligibility checks and writable-storage range
invalidation intentionally stay walking-side because later counters/snapshots observe their result immediately.
Unaligned storage and uint8 widening still copy on the walk, avoiding deferred worker reads of mutable guest memory.

For the next device run, **inferred expectations, not measurements**, under the same profile overhead are:
S about **9–11 ms**; walking resolve **0.5–1.5**, bind/freeze **5–8**, program/variant **3–4**, pipeline **1–1.5**,
vertex/index capture/eligibility **0.3–0.8**, command capture **0.5–0.8 ms**. Non-wait O should return nearer
**1–3 ms**, with actual worker wait reported separately; tiny synchronous secondary costs and worker resets may
still dominate. No device claim follows from host timing. Pass counts must equal the 0-worker run, independently
of chunk/inline thresholds, before accepting performance results.

The initial implementation passed Android compile-only checks for the recorder, draw and submission translation
units, and lavapipe `vktest` (19/19) and `vkdrawtest` (42/42) with 0/2/3 workers, CPU split profiling enabled and
inline threshold 0 (so small test draws really ran on workers). The blocking-invalidate checks require
`TOTK_VK_WRITEBACK=1`; that path remains opt-in. Settled-rendering write-back is now independently default-on (below).

### Deferred assembly after the second device iteration

The supplied alternating device runs of `7c54f91` improved Thor Lookout standing **20.3 → 23.1–23.8 fps** and Odin 3
Gerudo **47.4–47.7 → 51.8–53.4 fps**, Lookout **33.0–33.2 → 35.6–35.7 fps**, with matching ~179 passes/frame.
Thor's one-pair camera sweep remained **26.6 → 25.3 fps**. These observations do not establish a universal win.
Walking/worker-wait time was **26.5/5.3 ms** on Thor and **18.5/4.3 ms** on Odin Lookout, with **3.0/2.3 ms** of
synchronous tiny recording. Forcing tiny chunks to workers on Odin lowered walking **19.1 → 13.5 ms** but recording
worsened **23.4 → 24.8 ms** and FPS stayed ~35.5: end-of-pass joins, not just walking work, became the critical path.

The new default therefore queues tiny recording off the walk, coalesces it across passes, and defers pass-primary
assembly to slot submission. Per-worker busy time remains logged: Thor's three-worker result (**22.2 fps**) was below
two workers (**23.1–23.8**), so worker count is not assumed to scale monotonically amid game-thread competition.
The pre-coverage-extension identity passed with 24,816 draws and 60 passes per mode. The expanded stream below adds
uint8/unaligned-index and timestamp checks; its actual pass counts are compared directly in the output payload.
The following device iteration measured these changes; the newer miss-key/profiling revision below has no device claim.

### Walking-key and residual-cost revision

The supplied iteration-3 APK (`c1fbffa`, integrated test branch) measured Odin Gerudo **47.6 → 54.7 fps** and
Lookout **33.0 → 35.8**, recording **25.7 → 21.6 ms** (walking 18.9, wait 2.8). Thor Lookout standing measured
**20.5 → 23.9 fps**, recording **33.1 → 28.4 ms** (walking 24.6, wait 4.0), without reported flashes/crashes.
Thor's profile had S **12.8**, bind/freeze **5.9**, program **3.4**, O **8.7 ms**, and only **54.6%/29.0%**
program/freeze hits. The keys above stop unrelated uniform/binding updates and dirty-but-equivalent signatures
from causing needless misses. In the deliberately update-heavy host identity, program hit/miss changed from
**0.1/82.7** to **82.8/0.0** per artificial present, and freeze from **0.0/165.7** to **81.9/83.8**.
Those counts demonstrate eliminated misses, not a device timing prediction.

The iteration-4 profiler introduced exclusive S and O detail lines. S separates stream decode/dispatch, state
setters, uniform-log maintenance, counters/reports, pass decisions, snapshot capture, GPU-write tracking, resource
realization, draw dependencies/eligibility and residual work. Existing rename/copy cost inside descriptor preparation
remains P:bind; this revision does not silently move that original profiling boundary. O separates slot bookkeeping,
primary/fence/descriptor resets, primary allocation/begin, submission bookkeeping, queue submission/queue-lock
contention, GPU waits, worker waits, secondary execution/finalization, assembly, dispatch, transfers, uploads,
present and residual work. Each detailed family sums to its current summary S/O total, modulo printed rounding.
Primary-section allocation/begin is now explicitly O:primary (previously nested in S:pass), so an apparent S reduction
can include that attribution transfer. Extra enabled probes add overhead; compare clean profile-off timings first,
then use the reported transition estimate. The supplied old aggregate cannot retroactively identify which calls made
its remaining ~4.6 ms of O.

Safe cost reductions accompany the attribution: repeated same-size covered uniform updates reuse the old log
payload rather than free/malloc it (worker mode only); counts are printed in `uniform update payloads/frame`.
Touch-only draw dependency work returns immediately when no updates are pending. The protected-lag writable-binding
prepass executes **before** that return. Empty secondary-content passes end directly in their current primary instead
of parking/beginning another primary; logical pass counts/loads are unchanged. Completed jobs publish with release
stores, letting assembly use acquire reads without taking the queue mutex for every already-finished pass. A true
worker wait includes mutex acquisition as well as the condition wait, preventing hidden wait attribution.

### Ordered-stage boundary and lighter recording profile

The supplied iteration-4 Thor APK (`6774595`) was neutral profile-off: recording **28.4 → 28.7 ms**, walking
**24.4 → 24.8 ms**; weather affected FPS. Program resolution was fixed (**5583 hits/24 misses**), but binding freezes
still mostly changed genuinely (**3252/7962**, binding causes 7810/frame). Full probing cost an estimated **15.6 ms**
and 32 transitions/draw. Sampling now leaves most opcodes clock-free and scales only selected category intervals;
O waits/queues/assembly and outside-stream snapshots remain fully measured. The actual transition count and coarse
elapsed are the performance authority, not the extrapolated probe cost. Weighted timings still amplify probe bias;
neither the weighted nor actual calibrated estimate is an exact overhead subtraction.

The requested A (ordered state) / B (resources/freeze/program/pipeline) boundary has these concrete backedges:

| Proposed B operation | State that prevents A running ahead safely |
|---|---|
| `draw_touch` / `vk_ub_touch` | Reads the ordered update log and can call `ub_flush` → `vk_end_rendering`, recording transfers into current `g_cur`. A cannot mutate/free later log versions or decide physical pass boundaries meanwhile. |
| `record_stage`, `vk_ub_read`, `vk_ub_rename` | Read live S/R, generated-bank revisions, mutable snapshot dead/updated flags and log overlays. Copying S alone does not freeze these indirect values; the current packet freezes them before enqueue. |
| Texture/view realization | Registry/dirty processing can upgrade storage, emit uploads and end the current rendering instance. Actual Vulkan pass planning and `g_cur` cannot simultaneously belong to A. |
| Program/variant/pipeline eligibility | Missing/compiling pipelines skip draws immediately. Their sample totals feed pending reports and subsequent render conditions; these would need ordered result futures/rendezvous, not guessed counters. |
| Shader writable-binding discovery | A must invalidate later snapshots before their rename/read. Delaying reflection/variant selection to B delays that write classification; conservatively discarding every potential read-only snapshot is not equivalent. |
| CPU steps, copies, reports and slot reuse | They may require all earlier CPU jobs, GPU writes, query positions or arena lifetimes to be resolved before stream interpretation continues. |

Consequently this revision does **not** advertise a decode-only queue as a safe A/B recorder. Under the present API,
the proposed B operations remain on the ordered owner, except the already-isolated immutable descriptor/command work
on workers. A real further split needs versioned update/resource planning and ordered eligibility/write-frontier
results, rather than two threads reading mutable S/R/log/arena state or synchronizing at every draw.

Touch work now caches **negative** pending-range tests across unrelated logged updates, using complete journal proofs.
Known-size storage ranges with no pending overlap skip their pool lookup. Updated-but-live snapshot pointer reads
up to 32 bytes cache their by-value overlay result with exact address/size/snapshot/submission and range-change checks;
raw guest reads never use that cache. The draw-touch line reports calls/no-pending returns, pool lookups avoided and
pointer hit/miss counts. Pending uniform updates normally survive until a relevant non-uniform access or submission,
so “no updates anywhere” was too broad a condition on most draws; these checks target the actual touched ranges.

Snapshot collection deduplicates exact ranges in a capture-local cache before sorting/merging. Async captures still
own eager bytes. Synchronous worker-mode replay can copy directly into its immutable snapshot arena when that arena
is already GPU-retired, avoiding staging bytes/offset allocations and a second memcpy; if acquiring the arena could
wait for the GPU, eager capture remains before the wait. Captures still finish before the caller can overwrite uniforms.
Snapshot logs expose input/unique candidates and direct/staged KiB.

Workers already started full chunks during a pass, not only at its end. Submission still must wait for remaining
commands before Vulkan submission; that is an unavoidable completion frontier, not a restored per-pass join.
Leading chunks reduce first-publication latency from 128 to 32 draws for each worker. Scheduling logs show ready
delay/service time, maxima, jobs and their contained draws encountered unfinished, and those jobs' reset totals
(overlapping, not additive). The waited-pass line reports average/max pass size to identify a large-pass tail versus
tiny-pass/queued/reset-heavy work; these are whole-job/pass counts, not remaining GPU instructions. No passes or submissions
are inserted, and a condition-variable test now proves a leading draw chunk starts before its pass ends.

### Protected-lag walking attribution

The supplied Thor Lookout `89c0735` two-worker MOVE sweeps pass without flashes, crashes, waits or misses:
protected lag 28.5/28.5 fps versus lag off 27.6/28.0, with improved worst windows. Profile-on standing runs still
show walking 29.8→35.3 ms, chiefly vertex/index 1.7→7.6, draw-touch 6.1→9.3 and snapshots 0→2.1 ms.
These device observations supersede the earlier exact-binding host fixture as a predictor of residual lag cost.
Hot inputs account for roughly 279–325 copied ranges / 226–273 KiB per frame, while about 2,381 units receive guards.
The device-shaped host case therefore exercises changing per-draw vertex/index offsets and actual learned-hot ranges;
its smaller dirty-unit inventory is a CPU-locality experiment, not a complete game-scene reproduction.

The next supplied `ba2bcc6` two-worker sweeps remain visually correct and near-neutral in FPS (protected
29.1/28.9 versus off 29.0), but walking is 24.1–24.2 versus 15.8 ms. Sampled decisions attribute about 2.9 ms to
vertices, 0.8 to indices and 2.0 to uniforms; uniforms request about 1,695 immutable allocations / 4.7 MiB per frame.
Submission/ticket changes account for many metadata misses even when mappings are unchanged.

Mapping identity and immutable byte authority have different lifetimes. Mapping-only answers key the frozen span
generation rather than its ring-slot allocation pointer or ticket; removal/re-add and alias/view mutation still miss.
Tracked-unit classification and negative original-producer history may likewise survive a ticket, while learned-hot
state must refresh with real frame history. A new ticket invalidates native captures and still arms the globally
deduplicated unit batch before its genuine queue submission. Owned snapshot/overlay allocations instead survive
intermediate Vulkan chunks inside their immutable game-submission arena; snapshot incarnation and range content
proof revoke them before final-fence arena reuse. A metadata hit never permits stale native bytes or recycled storage.

Private uniform sharing uses owned immutable snapshot bytes and exact logical-overlay versions, not a mutable guest
unit. Only bounded covering windows/subranges may share an allocation; query alignment, smaller true writable aliases
and partial updates remain exact. Updating one logical version cannot modify bytes referenced by an earlier draw.
Raw native captures remain freshly write-watched through every known writer alias; no unit-level watched boolean
certifies later copies. Worker-only raw guest capture would lose the walking-point source version and is not enabled.



### Turnip command-generation cost revision

The supplied symbolized Thor profile (Mesa `f7c93a1352`) attributes 14.6% of process CPU to the driver.
Within that driver share, pool reset is 12.0%, vertex binds 9.4%, descriptor updates 9.3%, refcount atomics 7.2%,
pipeline bind plus dynamic-state copy 7.8%, and command-stream reserve/allocation/reset about 8%.
These are sampled CPU shares, not wall-time speedup predictions.

The revision keeps ordered resolution, resource realization, uniform reads/renames, draw eligibility, counters and
pass planning on the walking owner. Workers still consume immutable packets. The changes are confined to
command generation: reuse individually resettable command buffers, batch changed contiguous vertex bindings and
memoize index bindings, compile immutable descriptor-update templates once per layout, retain slot-local payload
deduplication, and put legal in-pass clears/timestamps into the existing draw secondary instead of separate secondaries.
Load/clear operations that begin rendering remain in the primary; no barrier, pass or submission boundary is moved.
Pipeline and dynamic-value comparisons already exist per secondary and remain mandatory after every fresh begin.

Mesa's common `ResetCommandPool` walks every allocated buffer and dispatches reset; `BeginCommandBuffer` can instead
reset only the reused buffer if its pool has `RESET_COMMAND_BUFFER_BIT`. Turnip's actual reset still clears seven
command streams, descriptor state, trace/autotune and temporary allocations for every reused buffer; implicit reset
does not magically remove that work. Coalescing event-only secondaries reduces the number of real buffers to reset.
Both worker and worker-mode primary pools use `TRANSIENT | RESET_COMMAND_BUFFER`. The slot fence has retired every
old primary/secondary reference before either individual begin or descriptor-pool reset. Resetting an old secondary
may invalidate an old executable primary, but no old primary is pending; a reused primary is itself implicitly reset
before recording. Unused command buffers retain capacity without being reset or freed merely to recycle a slot.
Its vertex-bind implementation allocates/emits all bindings below `max_vbs_bound` on **every** call; our zero binding
16 means up to 17 records even when changing binding zero. Avoiding redundant calls matters more than argument count.

Core descriptor-update templates precompile binding offsets/types/strides (`tu_descriptor_set.cc`) and remove our
per-write structure construction and the driver's ordinary write decoding. Actual buffer/image encoding still runs.
Push descriptors are exposed by this Turnip, but each push allocates sub-command-stream storage, updates/copies the
whole set and binds it; they are not selected over already-deduplicated ordinary sets. Templates are created only by
the walking owner and their immutable handles travel in packets; workers never consult a shared template map.
Descriptor payload caching remains context/slot-local, now four-way set associative; every hit avoids allocation and
update. Distinct per-draw uniform snapshots in the stress stream genuinely need distinct descriptor contents; templates
do not turn those into shared writable sets. Pools retain their high-water capacity and reset only after the slot fence.
The default zero-worker path retains its existing pool reset, descriptor update and binding calls.

`TOTK_VK_TEST_CALL_COUNTS=<file>` enables atomic API-entry counters in the host draw fixture only, installed before
any submission and written as JSON at process exit. Compare preserved pre-change and post-change executables on
`--rec-identity`, not counterfactual counts or this synthetic fixture's FPS. The fixture also changes two contiguous
vertex streams (including independent binding-one changes); other tests exercise the missing-attribute zero buffer.

Preserved baseline `d772381` and revised executable, same twelve-submission fixture (24,816 emitted draws):

| API count | 2 workers before → after | 3 workers before → after |
|---|---:|---:|
| `ResetCommandPool` | 59 → 0 | 75 → 0 |
| `BeginCommandBuffer` / `EndCommandBuffer`, each | 419 → 371 | 443 → 395 |
| `CmdBindVertexBuffers2` | 49,632 → 3,420 (**−93.1%**) | 49,632 → 3,480 (**−93.0%**) |
| `CmdBindIndexBuffer` | 13,140 → 9,804 (**−25.4%**) | 13,140 → 9,804 (**−25.4%**) |
| Ordinary `UpdateDescriptorSets` | 24,873 → 0 | 24,893 → 0 |
| Template updates / template creations | 0 → 24,864 / 2 | 0 → 24,888 / 2 |
| Descriptor allocations | 24,873 → 24,864 | 24,893 → 24,888 |
| `CmdBindPipeline` | 264 → 264 | 288 → 288 |
| `CmdSetViewport` | 264 → 264 | 288 → 288 |
| `CmdSetScissor` (genuinely changes each draw) | 24,816 → 24,816 | 24,816 → 24,816 |

The 48 fewer command buffers are merged timestamp/event secondaries, not fewer passes/draws. Individual implicit
resets are **not counted** as pool resets; the real reused-buffer reset cost remains. Descriptor allocation savings
are deliberately small in this update-heavy fixture and vary with worker assignment/hash placement; most payloads
are unique. Existing pipeline/dynamic guards already removed redundant calls, so no extra reduction is claimed.
All 0/2/3 baseline-vs-revised output files compare identically. Normal and TSan identity matrices pass at default,
chunk 1, inline/chunk 512, leading chunks disabled, profile disabled, and linear-span/dedup/dynamic diagnostics off.
The test-only API wrappers also run clean under TSan with recorder workers enabled. Direct
`REC_THREADS=2 GPU_PROFILE=2` execution in that pre-v2 test logged forced inline recording and passed the draw fixture; the v2 profiler above now retains workers.

Inspected Mesa source anchors (relative to `src/`): `vulkan/runtime/vk_command_pool.c:141` (pool walk),
`vulkan/runtime/vk_command_buffer.c:92` (implicit reset), `freedreno/vulkan/tu_cmd_buffer.cc:4371`
(real reset), `:4674` (full vertex-state rebuild), `:5148`/`:5234` (push update/copy), `:5612` (pipeline state),
`freedreno/vulkan/tu_descriptor_set.cc:614` (allocation), `:717`/`:873` (layout ref/unref), `:1388` (ordinary
write decoding), `:1621`/`:1806` (template creation/update), and `freedreno/vulkan/tu_cs.cc:462`/`:538`
(space growth/BO retirement on reset). The 7.2% atomics sample has no caller attribution in the supplied flat report;
layout references are a concrete source, not proof that all sampled atomics belong to descriptors.

### Ordered asynchronous slot assembly

The supplied Thor off-CPU profile attributes about **1.7 ms/frame** to waiting for secondary recording at slot
submission, about 90% at mid-frame pass-begin chunk boundaries and 10% at final NVN commit. These GPU submission
chunks are distinct from recorder draw chunks. The GPU threshold (75 without frame lag; 600 with lag) is checked
only at pass begin: a 1,791-draw pass counts as one oversized GPU chunk, not a draw-count quotient. Draw/pass bins alone cannot determine
the actual per-frame slot count; retain explicit submission counters rather than claiming `5650 / 75` is measured.

The ownership boundary is a **sealed ring slot**: walking closes its last primary/pre buffer, freezes
post ops and lag metadata, reserves its ordered ticket and detaches its immutable pending-pass graph. One FIFO
submit owner joins that slot's secondary jobs, finishes its parked primaries, performs the existing lag registration
phase at its pre-QueueSubmit point, then queues exactly that slot and publishes completion visibility. The walk may
record a different ring slot meanwhile. No submit owner reads live S/R, current_slot, uniform logs or mutable alias
inventories. A slot's primary command pool has exactly one host owner at a time; its fence still controls reuse.

Final NVN commit and CPU-consumer boundaries must join actual queue publication, not merely successful enqueue
to the CPU work queue. CPU-known UBO publication and failed-guard fallback waits remain after genuine QueueSubmit;
the guest cannot regain control while its mandatory registrations/publications are incomplete. Present ordering
uses the same reserved ticket/fence stream. Lag registration may not run early during interpretation, nor depend
on walking locks that a faulting walk holds while awaiting a sealed ticket. Failure authority must remain per ticket
when multiple sealed slots overlap. Background assembly wait/queue time is reported separately from walking waits.
The host UBO reuse coordinator follows this same distinction: `vk_submit_serial()` is the walking seal generation,
not proof that QueueSubmit occurred. It must observe the actual queued ticket through `vk_record_test_submit_status`
before opening its GPU event or starting a guarded CPU-reuse write. A reservation alone cannot satisfy that gate.

`TOTK_VK_REC_ASYNC_SUBMIT` defaults **1 when recorder workers are enabled**; `=0` retains walking-thread assembly
and QueueSubmit for A/B. `REC_THREADS=0` still uses the original synchronous path. The new `totk-submit` thread
owns a bounded FIFO of at most eight slots, with no per-submit allocation beyond retained slot high-water storage.
It inherits the creator's affinity/nice like other helpers; leave placement controls unset for the initial A/B.
Occlusion/pass/GPU diagnostics that force workers off also force this path off.

The actual queued fence (`g_fence`), walking reservation (`g_reserved_fence`), CPU-owner completion and GPU/post
completion are separate cursors. Walking cache generations advance at seal, never asynchronously underneath a
draw. All ticket selection, CPU drains and resource retirement include sealed reservations. Snapshot arenas still
release at the final game-submission fence. The slot remains pinned until both GPU/post completion **and** submit
owner completion, preventing command-pool/fence reuse even if a tiny GPU submission finishes immediately.
There are no new Vulkan semaphore/timeline objects; FIFO QueueSubmit order and the original binary fences remain.

External-engine continuations use an additional **opt-in-only** retirement contract:
`vk_submit_signal(binary)` seals the input primary and waits only for CPU queue publication, so a pending
SYNC_FD can be exported without a GPU wait. It retains that physical slot's descriptor pools, transient
buffers and worker command leases through the **next submission's** covering ticket, not merely the input
signal fence. Live UBO snapshot arenas receive the same covering pin. `vk_submit_wait(binary)` attaches the
external producer's native-fence import to that continuation. The caller records composition and the exact
frozen terminal transfer before sealing it; a failed/canceled engine must still seal its safe continuation
before starting another guest snapshot. At most seven of the eight resource slots may be pinned, and ring
allocation skips pinned slots so it cannot wait for its own unsubmitted continuation. A completed input ticket
is not permission to reset a still-pinned pool or arena. Missing or timed-out external completion never
authorizes recycling those resources or presenting unfinished output. The engine must call
`vk_submit_external_enable()` **only for an admitted handoff**. `vk_submit_external_disable()` is nonblocking:
it refuses until every source lease and queued continuation semaphore has genuinely retired. Cooldown/refusal
uses the original camera submission/presentation policy while outstanding owners drain; never clear a pin to
make the ordinary ring appear idle. After drainage, ordinary FIFO selection and semaphore lists are restored
and external budget history is discarded only when no output/callback still owns its identities.

Adaptive multi-phase terminal replay additionally leases the **original real recording owner**, not the
empty current slot selected by a later external split. Under the recording lock, call
`vk_replay_lease_begin()` immediately before the real `vk_submit_now()`. A zero token refuses admission
without submitting or allocating; suppress generated replay for that frame. After **all** generated
submissions, call `vk_replay_lease_end(token, 0)` before frame-end, including no-output, cancellation,
external failure and dropped-real paths. This selects the latest reserved covering ticket; an older explicit
ticket is extended to it. An open primary or pending split defers release to the next ordinary seal, so
frame-end cleanup cannot unpin unsubmitted work. Future tickets/stale tokens are refused without changing
retention. The lease holds the owner's transient buffers, descriptor/command pools, worker command leases
and live UBO snapshot arenas until actual covering-ticket **and** submit-owner retirement. Ending a lease
does not wait for the GPU or allocate. Owner leases and external split pins share the seven-distinct-slot
admission limit, always leaving one runnable slot; owner retention does not require external-engine admission.

Adaptive generated presentation waits on its own `CLOCK_MONOTONIC` condition until the absolute
target-minus-2ms commit deadline, not on a display-callback/slot-reached wait. Queue, window-lifecycle and
suppression changes wake arbitration. An earlier queued real returns the still-owned candidate to the
ordered queue; a ready newer real cancels it. The active candidate stays cancellable through blocking
transaction/release drains and the final window/epoch/ready-real check immediately before
`ASurfaceTransaction_apply`. That decision and apply are serialized with producer enqueues by the window
mutex, which is released before any serial transaction-completion wait. Only actual apply advances the
published-slot/latest-real watermark. A canceled staged transaction is discarded, never reused by teardown.
Adaptive refuses the public copy fallback because it cannot cancel a locked buffer without posting;
legacy copy/AHB behavior is unchanged. Dropped-real cancellation resets policy/planner/runtime epoch and
invalidates source history while normal frame-end retirement/accounting still runs.

`make -C games/totk/android TARGET=linux CC=cc OUT=<scratch> framegenadaptivepresenttest` and
`<scratch>/framegenadaptivepresenttest` exercise the same production arbitration helper with deterministic
60/120Hz pre-slot deadlines, real arrival during wait/drain, epoch/window changes, failed-preparation
non-commit, and duplicate/reversed-slot refusal. These tests do not simulate GPU retirement or establish
device scheduling latency; `vkreplaypintest` supplies actual GPU owner-retirement coverage.

`vkreplaypintest <salt3-compute.spv>` uses the existing shader-seed compute fixture, real delayed GPU events,
binary semaphore handoff and retained descriptor/transient bindings. Its production-API cases cover
four-phase ring wrap, two real owners in flight, independent covering-ticket retirement, seven mixed
owner/split pins with an eighth runnable slot, invalid/stale release, timeout, cancellation before the
cleanup seal, and no generated output without external admission. Device AHB/GL interop still needs
separate qualification. Run fresh processes with `TOTK_VK_REC_THREADS=0/2` and asynchronous submit
enabled/disabled; these are test instructions, not a claim of execution for this change.
The replay-pin fixture derives `TOTK_SHADER_SEED_DIR` from the supplied compute-SPIR-V parent directory
and explicitly admits locked mode (`TOTK_REALTIME=1`, fixed real30, slots1, no budget override); unsupported
startup fails before ownership checks. Its two-owner pressure case holds A's final split/continuation and
B's real owner, filling exactly four remaining slots before a separate wrap worker attempts reuse. A's
covering gate retires first while B's open lease still retains its GPU-completed real; only then is B's own
gated covering replay queued and retired. This avoids assuming two unsignaled HOST→ALL event waits have
independent execution frontiers. The mixed-pressure case first observes six real fences retire while their
open owner leases remain pinned, then gates only the reserved eighth cleanup ticket with all seven mixed
input pins held. Each real phase has a liveness deadline and worker joins are bounded; a failed required
GPU completion stops immediately rather than contaminating the next scenario. No timeout opens a GPU
gate or drops a lease to rescue the fixture. Run from isolated cwd/data cache as for the other real Vulkan
host fixtures; an external timeout remains useful as a driver-level cap.

`tests/compare_framegen_graph.py <fresh-scratch-directory>` builds the real Vulkan scene/UI fixture against
the immutable98336d3 source snapshot and this tree. Both recorder arms compare exact queue-submit counts,
normalized physical-slot sequence, normalized ticket sequence, generated-output slots and retained-real/midpoint
schedule for engine unset, camera and refused QCOM. Additional cases execute real binary-semaphore input/
continuation submits, retain a lease, mark external timing unavailable, then compare the drained camera suffix
after refusal/cooldown. Normalization removes only the prefix ticket offset and cyclic physical-slot origin.
Only completed timestamp **values** are deterministic test inputs (not timing evidence); rendering,
descriptor/arena allocation, queueing and retirement are real. Host capability refusal remains genuine:
these cases do not emulate successful Android GL execution. Default layouts stay separate from external state.


Lag integration follows the agreed per-slot contract: walking freezes guard-only projections, remaining primary
units/aliases and their birth identities; the owner joins only that slot, merges its immutable batches and arms
its exact reserved ticket immediately before queueing it. Neither a later slot nor final commit can replace that
registration. Failure latches are per ticket in the pinned eight-slot ring, not a single maximum that could lose
an older failure. Failed intermediate slots wait for their genuine GPU fence on the owner, without waiting for
post ops that may themselves need a later slot queued. The final walking boundary retains the maximum **already
queued failed** post-op frontier and waits after UBO publication before returning to the game. Registration of BUSY
units remains fail-fast. The owner never takes the walking recording lock.

Software-GPU **wait-mode lag** remains explicitly synchronous: lavapipe producer exclusions consume live
GPU-write/uniform/deferred-range inventories and cannot be read concurrently. A diagnostic logs this choice.
Lag-off and copy-mode host recording exercise async submission, and isolated read-only native-policy tests exercise
async native guards without protecting the CPU driver's own output VAs. This exception does not disable native
Android async recording and does not pretend the software exclusion inventory was sealed.

Every 300 presents, `slot submission/frame` reports all/game/handed slots, game slots per NVN submit, walking
publication-join time and QueueSubmit elapsed time. `recorder background assembly` reports owner slots and its
secondary-worker wait separately; guard join time remains on the guard line. Owner waits are not subtracted from
walking elapsed or added into the walking-only split profiler. Worker busy and driver-call elapsed include
preemption, so overlapping fields must not be summed as exclusive CPU time.

Host identity now includes `--rec-submit`: the one-draw GPU threshold forces a real mid-frame seal while an earlier
worker remains gated. The walk must open/record a different slot before that old ticket is queued; frozen aliases,
pixels, actual pass/draw counts and counters still compare across 0/2/3. The tiny-threshold stream has one additional
clear-only pass per alias pair from the existing pending-load-clear split policy, identically in all modes.
The native guard fixture independently proves next-slot progress while native registration is blocked, then
protects real guest/host reuse behind a GPU event. Two failure-latch checks prove exact-ticket consumption.

Measured x86-64 lavapipe fixture, profile off, private uniforms (`UB_RENAME=1`), GPU threshold 75, `LP_NUM_THREADS=0`:
medians of three alternating pairs, each twelve NVN submits / 24,816 draws; **not a device speedup prediction**:

| ms per NVN submit | 2 workers sync → async | 3 workers sync → async |
|---|---:|---:|
| Walking body elapsed (before final commit) | 9.165 → 3.723 | 9.429 → 3.592 |
| Walking body secondary/publication wait | 5.166 → **0.000** | 5.097 → **0.000** |
| Final commit elapsed | 7.308 → 10.901 | 5.695 → 9.814 |
| Whole walking call elapsed | 16.473 → 14.624 | 16.269 → 13.303 |
| Summed chunk-worker busy | 27.800 → 26.744 | 35.886 → 36.900 |
| Total walking secondary/publication wait | 11.886 → 10.894 | 11.269 → 9.807 |

Component medians need not add. Final joins get longer because workers are no longer joined earlier; work is
overlapped, not deleted. All preserved outputs are byte-identical across sync/async and repeats. This fixture uses
**35 slots / 12 submits = 2.92**, unchanged by async or a 150-draw threshold; its coarse pass sizes cross both
thresholds at the same places. GPU threshold 0 reduces it to one slot and body wait is also zero synchronously;
that CPU-driver result does not justify removing GPU overlap on Android. The existing Thor measurements of
75/150/400/800 (27.1/25.5/21.2/21.6 fps) show why fewer slots are a simpler experiment, not an automatic fix.
Actual Lookout slot count is not recoverable from aggregate pass bins or off-CPU sample percentages. A naive
single-submit `5650/75` ceiling is 76, but the 1,791-draw pass alone collapses roughly 24 such quanta to one, and
other large passes reduce it further; extra NVN/CPU boundaries can add slots. Use the new actual slot counters.

Normal and TSan matrices pass at workers 0/2/3 with lag off, protected wait and eager-copy policies, including native
guard handoff/lifetime cases. The three-stream identity harness passes default, async disabled, rename on with
async on/off, recorder chunk 1, inline/chunk 512, lead 0, profile off, and linear-span/dynamic/dedup diagnostics.
TSan retains recorder/submit/compiler/completion threads with `LP_NUM_THREADS=0`, no suppressions or reports.
Android cc1 and the detailed matrix/benchmark logs are recorded in the handoff report.

Expected device effect is an **inference**, not measured: the assembly-only mid-frame budget is about
`1.7 × 0.9 ≈ 1.5 ms/frame`; a plausible net target is **0.5–1.5 ms less walking elapsed**, after final-join and
extra-owner scheduling costs. The separately measured lag-registration budget is not added as a promised gain.
Alternate `REC_THREADS=2 REC_ASYNC_SUBMIT=0/1`, GPU chunk 75 and unchanged lag/pacing/cache/power settings in
warmed Lookout/Gerudo on Thor/Odin. Retain slot counters, body/final/worker/owner waits, GPU time and frame-time
distribution, and validate identical normal-worker screenshots; a diagnostic pass dump intentionally forces inline.


### Bounded lag-guard VMAs and allocation failures

The Odin world-load failures were not specific to asynchronous submission: both async on and off produced
either an allocator abort at `vk_record_descriptors` or `vkEndCommandBuffer` returning
`VK_ERROR_MEMORY_MAP_FAILED` (-5). The supplied `/proc/<pid>/maps` samples rose from 3,673 to 31,424,
47,887 and 50,713 before the Odin abort; the Thor rose from 3,023 to 58,656 and was still climbing.
Both kernels allow 65,530 mappings. These observations identify VMA exhaustion as a release-blocking
resource risk; neither a stack inside `realloc` nor map totals alone establish which mappings grew.
**Device rejection of the first budget (63fa904):** on the Odin, fallback latched at 36,882 maps but
the process subsequently reached 63,456 and died. The lag-off control settled at 35,185 without a crash.
The first budget is therefore not an accepted device fix. Its retained-run credit can exceed its limit
after fallback because mandatory texture/exact watches and temporary writer reservations bypass admission.
It is not a cap on total VMAs or on retained driver/allocator mappings.

An ordinary lag guard previously protected each 64 KiB dirty unit separately. Adjacent read-only intervals
can already kernel-merge across separate syscalls, but alternating read-only/writable units still create
two extra VMAs per island. A CPU write restores its non-exact neighbours and its own page RW; exact
write-back pages remain independently protected. Sticky guards need no subsequent syscall until a write,
so untouched islands can remain indefinitely. Merely batching adjacent syscalls does not bound that pattern.

The enabled-lag policy budgets read-only runs against **current total-map headroom** and the observed
kernel limit, retaining the last observed limit or using the Linux default 65,530 when the sysctl is
unavailable in the app sandbox. Missing samples alone do not latch fallback. Reserve one eighth of the
limit (minimum 1,024 maps); new-run credit uses the worst-case two additional VMAs per run. Native
registration requests a sample when tracking-range lifetimes change; walking seals also request one
after pool/span/alias version changes. Requests only mark pending work; they never wake the worker,
advance its deadline or take the sampling mutex. A lazily started, detached process-lifetime
`totk-vmascan` thread runs at nice +10 where permitted. `TOTK_VMA_SCAN_MS` selects its period in
integer milliseconds (default **10,000**, clamped to **[1,000, 30,000]** with a one-time startup log
when clamped; malformed values retain the default). It sleeps that
period before its first scan and between completed scans; periodic, epoch-change and diagnostic
requests coalesce into at most one maps traversal per period. Classification reuses that traversal
for the budget count rather than reading maps a second time. The 10 s default reduces contention
on the Thor kernel's shared `mmap_lock`: a low-priority reader still contends with `mprotect` and faults,
and one-second sampling plus frequent epoch wakes removed boundary hitches but worsened overall pacing.
Startup samples
synchronously before gameplay to establish the first valid budget. The only later synchronous count
samples are the one-shot safe-boundary cutover's before/after-drain diagnostics, after GPU/registrar
owners have joined; no maps read runs on the recurring frame/present or recording path. The 300-frame
memory report only reads cached samples/telemetry. Fault paths retain their lock-free credit admission
and `mprotect` ENOMEM pressure latch, with no sampling, allocation, stdio or mutex added.

At the Android default 65,530-map limit the reserve is 8,191 maps. Observed worst baseline growth was
about 1–3k maps per 300 frames: at 30 fps that is 100–300 maps/s (at 60 fps, 200–600 maps/s).
Thus a ten-second interval plus a ~100–120 ms scan consumes about 3,036 maps at the Thor's observed
worst 30 fps rate, leaving 5,155 reserve maps. Even the 60 fps rate consumes about 6,072 maps, leaving
2,119 reserve maps. Longer period overrides reduce this margin and need corresponding growth evidence.
Atomic run
credit still accounts immediately for our own additional guards, with two worst-case VMAs per run;
stale counts do not grant new credit until another successful sample. Missing maps samples retain the
existing limit rather than replenishing it against old counts and newer reservations. This is
conservative for observed growth, not a guarantee against arbitrary external mapping bursts or
indefinite sampler starvation: a real `mprotect` ENOMEM still irreversibly latches pressure and the
same ordered safe-boundary cutover applies.
There is **no impossible fixed total-map target**. Current total
conservatively upper-bounds the same-point lag-off baseline, not a measured counterfactual; observed
device baselines now exceed 40k. A startup sample cannot predict the later baseline or guarantee that
draining protections recovers every VMA.

Credit counts runs independently inside each dirty unit, even where Linux merges neighbouring units.
Each unit gate serializes its page states, temporary worst-case writer/split credit and final run count;
global credit is lock-free atomic. Registration reserves before changing permissions and returns unused
credit after publication. Already-armed duplicates retain their existing load/CAS path. The native owner
sorts/rehashes its final immutable batch once, gives each registrar a contiguous shard, and protects up
to 64 adjacent newly guarded units with one `mprotect` while holding their gates. BUSY still fails without
waiting for an unsubmitted ticket; partial registration still joins and queues before fallback.

Pressure latches a transition **until process restart**:

- Immediately restore raw current-fence frame-end waits in the shared NVN layer.
- Stop additional lag GPU-read/private-copy watches. Already-captured immutable submission snapshots
  remain valid; a failed watch cannot authorize native-copy memo reuse.
- Until the current NVN stream ends, queue each affected slot in order and wait its real GPU fence,
  retaining the final UBO/post-op publication frontier. At the next NVN begin, drain all old owners,
  GPU/post work and lag-only watches once, invalidate snapshot-derived texel views, release snapshot
  arenas and disable capture/private UBOs. Future streams use ordinary lag-off submission without
  per-slot GPU waits or copy/guard overhead. The original GPU cursor keeps its slot-lifetime pin.
- Preserve baseline texture dirty watches and exact GPU write-back watches, with their actual generations.
  Two bits in the existing per-page watch byte distinguish them from additional lag watches; no new page
  array is allocated. A real CPU write retains the original coarse-unit invalidation, except for exact
  write-back neighbours. The drain must not invent a CPU write and discard a pending GPU-to-CPU write-back.
- New baseline texture/exact watches continue to work after fallback. Texture uploads remain **dirty-driven**,
  not unconditional: this avoids turning VMA pressure into enormous staging/copy allocations.
  Draw setup realizes changed inherited texture bindings before opening rendering, never after a secondary
  packet's descriptor array has been allocated. GPU-written/shared-image authority is unchanged.

The transition trades lag overlap for ordinary safe frame waits, not unconditional texture uploads or
permanent per-slot stalls. Its single trigger diagnostic records maps/limit and
run credit/budget at the transition; every 300 frames while lag guards are enabled, `memory VMAs`
reports last sampled maps/limit, run credit/budget and fallback state, plus completed worker scans
since the previous report, the epoch-requested subset, and total/max scan duration in milliseconds.
Timings come from the worker's monotonic clock and include any requested classification; startup and
cutover synchronous samples are excluded. Counters are copied/reset together under a telemetry-only
mutex that is never held during a scan or while holding the sampling/range mutexes.
Run credit includes temporary reservations and baseline texture/exact watches,
not an exact count of Linux VMAs; necessary baseline watches may exceed the extra-lag admission budget after
fallback. The new `memory recording` line
separates slot/snapshot arena capacity, retained draw packets/descriptor inputs and worker command-buffer/
descriptor-pool counts; driver-internal bytes are explicitly excluded.

With `TOTK_VMA_CLASSES=1` (off by default), the 300-frame report also requests background classification of `/proc/self/maps`: r--/rw-/r-x/--- and other protections,
anonymous versus file/device-backed, shared, recognizable KGSL/DRI/dma-buf, recognizable native allocator,
and mappings overlapping registered pools. The most fragmented contiguous **same-backing, linear-offset**
range is printed with its VA bounds, size, row count and pool overlaps. This distinguishes guest shmem
fragmentation from thousands of individually mapped command BOs or private allocator regions.
It snapshots pool bounds under the lifetime mutex, then releases it before sorting and scanning; scratch
storage is statically bounded by the existing range limit. Both count sampling and classification run on
the sampler thread, not the present, recording, per-draw/per-guard or signal-handler path.
Names are classification hints, not proof of ownership; unnamed allocator/driver
maps remain in the anonymous/file totals. Counts are observations of a concurrently changing process.
Classification remains diagnostic-only: when it formerly ran on the present path, ~30,000 maps on the
Odin 3 (60 fps mode, lag off) produced one ~300 ms present interval every 300 frames, about 3.5 fps of
the Gerudo average. Synchronous count-only samples with lag enabled still cost ~100–120 ms on the Thor;
neither scan now runs on that path.

`vktest --lag-vma-async` checks period parsing/clamping (including low/high/overflow values), uses
a 500 ms request clamped to a 1,000 ms test period, and checks periodic/requested stats and pressure
updates without caller-side refresh, including a pool-epoch registration and `vk_mem_report` with
classification enabled. Request storms exercise the period cap; worker telemetry checks count,
epoch-requested subset and total/max timing. A per-calling-thread proc-read counter proves that
neither recording nor reporting scans maps.
`--lag-vma-policy` retains synchronous synthetic-sample checks, and `--lag-vma` exercises actual
guard admission, pressure and the safe-boundary drain with baseline texture/exact watches retained.

`vktest --vma-merge` reproduces split/fault/restore geometry without Vulkan. On the Linux host, a 64 MiB
private anonymous range gives 1 → 2,048 → 1,024 VMAs after faults and RW restoration, even with one final
whole-range restore; shared anonymous memory gives 1 → 2,048 → 1. The guest reservations explicitly use
`MAP_SHARED|MAP_ANONYMOUS` (`runtime/services.c`), so the private-anon `anon_vma` explanation cannot simply
be assumed for the guest heap. The Odin classification then ruled out that hypothesis for this failure:
at the lag-on crash, tracked-pool maps were 14,320 versus 14,149 in the lag-off control, and recognizable
driver maps 14,331 versus 14,160. Instead, anonymous maps were 23,730 versus 4,973, native-heap maps
9,205 versus 1,239, and inaccessible maps 17,612 versus 6,886. These controls use CHUNK_DRAWS=600 with
lag and the default 75 without lag, so this is a configuration/resource comparison, not an isolated lag
performance comparison. The lag configuration retained 359 MiB of draw packets and 130 MiB of descriptor
inputs versus 83/32 MiB, and 4,608 secondary CBs versus 1,024. Full-128-draw allocations for tiny jobs
and individually reallocated descriptor vectors create Scudo secondary mappings/guard pages per job.
There is no demonstrated job lifetime race.

The structural cutover keeps draw packets and descriptor inputs in **walking-owned, per-slot slab pools**,
with power-of-two size-class free lists. Each slab is one anonymous mapping, not a Scudo secondary
allocation for each job. Unpublished packet arrays grow from four draws to their actual lead/chunk limit;
descriptor inputs grow in size classes as needed. Growth copies only that unpublished job's existing
values, returns its old block to the slot's free list, and never touches dispatched storage. Workers
only read their immutable blocks. The slot retirement handoff joins old jobs and GPU execution before
recycling or trimming its pools. No extra lock is required on the packet hot path.

`TOTK_VK_REC_MEMORY_MB` defaults to64MiB (clamped16–512), a global **idle retained-capacity
pruning allowance**, not a destructive hard cap on a larger active scene. Under budget, no slot
is scanned or evicted. Above budget, each slot scans at most every8 pressure reuses: jobs idle
for32 slot generations, or arrays over four times demand for32 consecutive job uses, return
their blocks to size classes. Only unreferenced slabs are unmapped. Active/recent capacity may exceed the allowance,
with mapped and referenced capacity, reclamations and walking prune time reported separately.
Driver bytes and other backend arenas are not included in this CPU-storage allowance.

Secondary command storage is recording-owner scoped across slots, not multiplied by eight independent
slot peaks. Retirement returns leases after both actual GPU/post completion and submit-owner release.
Only their recording owner performs Vulkan pool operations. Cold maintenance runs at most every256
global retired-slot epochs, requires4096 epochs since last use and capacity above a worker-wide
floor512/twice recent simultaneous demand. Gross unused capacity can be trimmed. Active leases
are never capped or waited while their slot is unsubmitted; ordinary warm handles remain available.

Non-splitting tracking alternatives have different correctness contracts:

- [Linux soft-dirty](https://docs.kernel.org/admin-guide/mm/soft-dirty.html) clears PTE dirty bits by
  writing `4` to `/proc/self/clear_refs` and samples bit 55 through `/proc/self/pagemap`. It does not split
  VMAs, but the kernel automatically permits a CPU store; it **cannot** replace a GPU-read guard that must
  block reuse until a ticket completes. Process-wide clearing and per-alias sampling also differ from the
  current independently stamped texture/exact watches, including host GPU-write-back stores. Access and
  kernel configuration must be checked in the actual app; OS version alone is insufficient.
- [Userfaultfd write protection](https://docs.kernel.org/admin-guide/mm/userfaultfd.html) can block writers
  without per-protection VMA splits. Android [app-domain policy](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/android15-release/public/te_macros)
  explicitly grants each app its own userfaultfd type (also present in Android 13); the generic anonymous-inode
  neverallow does not negate that separate type. Availability still depends on syscall/seccomp policy,
  kernel features and the **shared-memory WP** feature, not merely the existence of userfaultfd.
  `UFFD_USER_MODE_ONLY` does not handle kernel-origin copies; host write preparation must remain mandatory.
  Shared-memory WP requires the corresponding feature negotiation, plus a dedicated fault consumer,
  aliases/remap lifetime management and the existing ticket-gate ordering. A missing-page-only ART facility
  is not evidence that guest shmem WP works on either device. No unsupported facility is silently selected.
- A separate alias of the same physical pages is not a guard: CPU stores through another writable alias
  still change the GPU's bytes. A truly private snapshot avoids that hazard at the cost of copies and
  additional resident/upload memory; protecting an alias alone is not a correct fallback.


Mandatory recorder, span, snapshot and update-log heap allocations now abort at their own site if allocation
returns NULL, with the requested byte count, rather than publishing NULL into a job. Optional guard/vector/
native-batch failures retain the existing fail-closed queued-fence fallback. Vulkan allocation/map calls
already aborted on failure; arena failures additionally name their allocation size, memory type and arena.
Writable `mprotect` restoration failures are fatal through a raw signal-safe diagnostic: the tracker never
publishes a page as writable while the kernel still denies stores.

Host regressions:

- `vktest --lag-vma` counts actual process VMAs under alternating guarded units with a small test-only budget,
  verifies contiguous-run syscall coalescing, rejects new guards before exhausting mappings, drains additional
  lag watches back to baseline, and checks existing/new texture/exact watches and real CPU-write generations.
- `vkrectest --job-reuse` runs fresh-process native-policy graphics streams at recorder threads 0/2/3/4.
  Each stream wraps eight slots repeatedly within one game submission, combines tiny passes and a 641-draw
  pass, changes inline uniforms, and varies worker delay by up to 2 ms. The pressure variant changes sampled
  texture bytes each frame, checks every output pixel through real postqueue waits, and exercises actual
  GPU-to-CPU write-back plus preservation of a newer CPU write.
- `vkdrawtest --rec-storage` adds 4,096 real one-draw passes in one slot before 48 reuse frames, uses a
  32 MiB retention allowance, and checks every pixel/draw, whole-process steady-state VMAs after eight
  warmup frames (at most +1,024), warmed anonymous maps (at most +128), slab capacity/map count and
  secondary/primary reclamation. Warmed under-budget slabs must not be evicted again. The runner covers
  0/2/3/4 workers. Whole-process counts include lavapipe's independently retained shared driver mappings.
- The same runner injects recorder-info/calloc/slab-mmap, Vulkan allocation/map and secondary-end failures,
  requiring SIGABRT at the failing operation with its diagnostic, not continuation into half-initialized work.
  ASan builds poison unused/freed slab payloads and unpoison only acquired storage, so suballocation does
  not hide lifetime/address errors from the sanitizer.
- `vktest --lag-vma-policy` covers a missing app sysctl, missing maps samples, a 48k world baseline
  above the obsolete fixed target, genuine near-limit pressure and a non-oscillating fallback latch.
- `vktest --arena-bo` verifies copied bytes through five slot BOs compacted into one, and a fragmented
  snapshot arena compacted before reuse among four game-submission arenas. It checks actual labeled
  allocation counts and cached texel-view invalidation, including resource-lock re-entry.
- The pressure fixture also gates the real GPU with a closed event after cutover: both an intermediate
  chunk and final commit must return before the event is opened, with no additional fence fallbacks.
- `vkdrawtest --rec-leases` and `--rec-leases-unlimited` exercise GPU-slot CHUNK600 and0 at
  recorder0/2/3/4. Over84 frames, two384-pass submissions remain behind a real GPU event, then slots
  rotate through large/small workloads. Actual active leases must remain held behind the event; retained
  handles are bounded by summed recording-owner concurrent peaks plus at most seven spare handles
  per owner (eight-handle allocation batches), not by eight ring-slot peaks. Warm capacity is retained
  without allocation unless a new owner-demand peak requires growth. A separate owner-pin gate lets
  the GPU finish an intermediate slot before its submit owner retires and verifies leases stay held.
  Cold-tail resets and rendered pixels are checked; these cases are included in `vkrectest --job-reuse`.


Initial budget validation (63fa904, **not accepted on the Odin**): **42 top-level invocations passed in each build**
(126 total), including all four host Vulkan suites with lag off/on at threads 0/2/3/4, both exact-byte
identity matrices, private-uniform fixtures, native async/sync pressure cases, profile-enabled pressure,
and the reuse/failure-injection runner. TSan uses `LP_NUM_THREADS=0` and `halt_on_error=1`, with no
suppressions or reports; ASan uses `detect_leaks=0` with address-error reporting enabled. Android cc1 is clean for
all seven changed C files, including test configurations. The guard-heavy fixture passes **147 checks**:
actual VMAs **36 → 253 → 36**, 111 guards admitted and 401 refused at a test-only run budget of 128,
while baseline texture/exact watches remain authoritative. Its 128-unit contiguous batch uses at most
two protection syscalls. These are host correctness/resource-bound observations, not device acceptance.

Historical 78aa011 structural slab/CB validation: **43 top-level invocations pass in each of normal, ASan and TSan builds
(129 total)**. This includes the full lag off/on × 0/2/3/4 matrix for all four Vulkan suites, both
byte-identity matrices, private UBO tests, native sync/profile pressure cases, `--vma-merge` and the
expanded reuse/storage/six-failure runner. No TSan or ASan diagnostics occur; TSan has no suppressions,
`LP_NUM_THREADS=0`, and `halt_on_error=1`. ASan has slab poisoning and `detect_leaks=0`; no leak-clean
claim is made. Android cc1 is clean for the changed production and test C files, including the recorder
with test hooks enabled.

Historical 78aa011 normal-build storage observations (the smaller 1,536-pass transient and 24-frame reuse stream):

| Workers | Anonymous maps cold/warm/final | Total maps warm/final | Packet/info slabs | Named slab VMAs | Retired slots / CBs reclaimed |
|---|---|---|---|---|---|
| 0 | 67 / 67 / 67 | 4,782 / 4,798 | 0 / 0 MiB | 0 | 0 / 0 |
| 2 | 71 / 99 / 99 | 6,243 / 6,161 | 21 / 8 MiB | 25 | 8 / 2,520 |
| 3 | 69 / 98 / 98 | 6,425 / 6,322 | 17 / 8 MiB | 24 | 7 / 1,848 |
| 4 | 73 / 103 / 103 | 6,744 / 6,601 | 16 / 8 MiB | 23 | 8 / 2,336 |

Thus anonymous maps grow by only 28–30 in the worker cases, not one Scudo secondary mapping/guard
pair per tiny job. Total maps include lavapipe's separately retained shared driver BOs. The initial
cold-to-final whole-process +1,024 and then warmed +128 assertions were too strict for those BOs;
classification showed that growth was shared/file-backed, not packet storage. The final explicit
bounds above retain a tighter anonymous-map check and the independent physical slab-budget check.
These are host observations; only the matching device load/long-play run can accept the Android fix.


### App-safe pressure fallback and warm driver storage

Device acceptance of 78aa011 found the allocator failure fixed on both devices, but exposed a policy
regression. Android 13 on the Thor permits `/proc/self/maps` but hides `max_map_count`; interpreting
that missing sysctl as zero latched fallback at only 2,629 maps. The Odin's actual lag-off baseline is
about 41.6k and the Thor's about 42.6k, so the old 39k target was not a valid runtime baseline either.
The pressure policy uses the last observed kernel limit, or the Linux default 65,530 when unavailable.
Missing samples alone do not trigger fallback. Admission uses **current total-map headroom**, reserving
one eighth of the limit (minimum 1,024). The current total conservatively upper-bounds a same-point
lag-off baseline; it is not mislabeled as a measured counterfactual. Budgets may adapt as real total
headroom changes, rather than retaining a stale startup ceiling.

On real pressure, frame-end waits immediately target the current raw fence. At the next NVN submission
boundary, the walking owner finishes old queue owners/GPU/post work once, drains extra watches and
disables lag capture/private uniforms for future streams. Subsequent chunks use ordinary lag-off
submission: no persistent per-slot GPU wait, hot copies or guard projection. The GPU-only completion
cursor remains alive when originally started, preserving its slot-retirement pin. Snapshot-derived
texel views are invalidated before their arenas are released; a lag-off cache hit must not retain
an old private snapshot. Snapshots and uniform invalidation are reset at the cutover.

Warm capacity is not evicted while the **global** packet/descriptor slab total is below its allowance.
Secondary-CB maintenance is amortized over32 owner return batches and retains a generous worker-wide
warm allowance, rather than eight per-slot high-waters or freeing normal scene capacity every frame.
Primary segment buffers had a separate unbounded load-time high-water, independent of worker CBs and
packet slabs. Their retired-only reclamation likewise samples the peak across 32 reuses, keeps at
least 256 per slot and twice that recent peak, and trims only capacity at least twice the allowance.
The 300-frame `memory primary command buffers` line reports retained segment capacity and cumulative
reclamations (the one fixed pre-command buffer per initialized slot is not included). This is an
additional driver-BO source, not charged to the CPU packet-slab budget.

Transient slot and snapshot arenas compact only after their own GPU lifetime has completed: more than
four slot BOs or two snapshot BOs are replaced by one allocation with the same retained byte capacity.
Buffer-view cache entries are retired with their source buffers. This bounds retained arena BO counts
without multiplying large minimum allocations or discarding in-flight snapshots.
The resource mutex is recursive because texture upload and texel-buffer resolution can acquire an
arena while already holding it; safe arena retirement may re-enter solely to invalidate cached views.
The source-buffer retirement contract covers both currently cached and deferred-retirement views,
and runs only after recording owners and the buffer's actual GPU fence have completed.


Backend-owned `VkDeviceMemory` live-object counters are reported every 300 frames by purpose: guest pools,
slot arenas, snapshot arenas, pooled/dedicated images, present, readback, zero buffers, diagnostics and
overlay. They count actual successful backend allocation/free calls, not guessed driver VMAs; internal
command/descriptor BOs are driver-owned and remain visible through procfs/CB/pool diagnostics.

Historical1e6daab warm-capacity/fallback validation: **52 top-level test invocations per normal/ASan/TSan build,
156 total**, plus three clean host builds and Android cc1 for all nine changed C files (also the
recorder/backend test-hook configurations). This includes the four Vulkan suites with lag off/on at
0/2/3/4 workers, both pixel/stream identity matrices, private UBOs, native sync and profiled pressure,
the expanded reuse/six-failure runner, and explicit VMA-policy/arena-BO tests. No TSan reports or ASan
address errors occurred; TSan uses no suppressions and `halt_on_error=1`, and ASan retains slab
poisoning with `detect_leaks=0`. These are not a leak-clean or Android throughput claim.

The final 4,096-pass/48-frame normal-build stress observations are:

| Workers | Total maps warm/final | Anonymous maps warm/final | Packet/info MiB | Primary segments warm/final | Secondary CBs reclaimed |
|---|---|---|---|---|---|
| 2 | 12,000 / 11,976 | 103 / 105 | 24 / 8 | 8,304 / 368 | 5,120 |
| 3 | 11,902 / 11,910 | 103 / 105 | 23 / 8 | 8,304 / 368 | 4,608 |
| 4 | 11,881 / 11,884 | 106 / 108 | 23 / 8 | 8,304 / 368 | 4,096 |

Each reclaims one over-budget load-shaped slab slot, then performs **no further warmed slab eviction**.
All reclaim 7,936 primary segments; the remaining whole-process VMAs do not fall proportionally on
lavapipe, so a freed CB is not reported as proof that its driver's mappings were released. The arena
test independently verifies five slot BOs become one and the fragmented snapshot arena becomes one
(four total BOs after rotating all four snapshot arenas, rather than six). Device driver-private
retention and the required lag-on/off FPS/map plateau still require the authorized device A/B.


Larger architectural alternatives remain different trade-offs: protecting contiguous runs removes redundant
syscalls but not sparse-writer fragmentation; coarser protection domains increase false sharing and CPU waits;
private input copies remove original-VA guards but add copy bandwidth and retained snapshot memory. A read-only
alias alone does not intercept writes through the game's original writable VA, so it cannot supply watch authority.
Device acceptance uses the same lag-on/CHUNK_DRAWS=600/AHB title-to-world and long-play maps sampling on both
devices. Also run a lag-off **CHUNK_DRAWS=600** control to isolate lag from the earlier default-75 comparison.
Require lag-on mappings to settle within a few thousand of the **same-configuration** control, with
headroom below the real kernel limit (not an impossible fixed 40k target below the observed baseline),
one explained last-resort fallback when needed, no abort or pixel/write-back regression,
and report actual recording/frame-time/upload cost. Keep REC_THREADS=2 for the first resource A/B; then
measure 0/2/3 at Lookout Landing and Gerudo on both devices, without assuming three workers help the Thor.

### Cross-slot worker-owned secondary leases

The matched CHUNK600 device control identifies GPU-slot high-water multiplication, not lag alone:
Turnip retains roughly 13.7–14.1k CS BOs with either lag setting at CHUNK600, versus about 5.1k with
the default75 slot threshold. Secondary command storage therefore moves from eight independent
per-worker/slot high-waters to **one recording-owner pool per worker**, with cross-slot leases.
Descriptor pools, deduplication, span hints and immutable packet storage remain slot-generation scoped.

A lease is returned only after actual GPU/post completion **and** submit-owner retirement. The final
pin publisher enqueues the completed slot's lease list; it performs no Vulkan pool operation. Each
recording owner alone drains its returned lists and allocates/begins/resets/frees/trims that pool.
Owners drain returns before their next recording batch; retirement does not wake otherwise idle workers.
No wait for an unsubmitted slot is introduced: active demand may grow, while returned handles are reused
across every slot before another allocation is made.

Warm free handles are retained. Cold maintenance is gated by a512/owner floor, twice recent
simultaneous demand rounded up to a power of two, and a4096-retired-slot last-use grace.
It runs at most every256 global slot epochs. Resetting a cold handle releases Turnip's
geometric overflow BOs while preserving the final warm stream BOs; gross excess capacity
(at least twice the allowance) may instead be freed/trimmed down to that allowance.
Descriptor lifetimes and primary assembly ownership do not change. Tests gate two large-slot
GPU submissions, verify no early lease reuse, then exercise ring rotation with GPU-slot
CHUNK600 and0 and bound capacity by simultaneous demand rather than eight independent peaks.

Historical3359bfd cross-slot lease validation passed **53 top-level invocations per normal/ASan/TSan build (159 total)**,
including the two large-slot lease modes at0/2/3/4 workers, all four Vulkan suites with lag off/on at
0/2/3/4, both pixel-identity matrices, private UBOs, native sync/profile-pressure, the expanded
20-case reuse/storage/lease runner plus six expected-fatal injections, and the VMA/BO regressions.
Android cc1 is clean for all four changed C files and recorder/backend test-hook configurations.
TSan has zero reports without suppressions (`LP_NUM_THREADS=0`, `halt_on_error=1`); ASan has zero
address errors with slab poisoning and leak detection disabled.

In that earlier build, the normal84-frame lease fixture held768 real secondary leases behind its GPU
gate, checked every subsequent output pixel and the separate GPU-complete/owner-held case, and observed:

| Workers | CHUNK600 warm/final handles | CHUNK0 warm/final handles | Summed owner concurrent peak | Cold resets600/0 |
|---|---|---|---|---|
| 2 | 776 / 776 | 768 / 768 | 768 | 720 / 724 |
| 3 | 784 / 784 | 776 / 776 | 768 | 716 / 720 |
| 4 | 776 / 776 | 776 / 776 | 768 | 708 / 716 |

Warm handle capacity is unchanged through the smaller scene and the restored large scene; no
per-frame handle destruction/reallocation is used to obtain the bound. Cold resets release only
driver overflow storage, not live leases or the final warm stream BOs. This is host correctness and
allocation-stability evidence, not an Android FPS or exact driver-VMA reduction claim. Device A/B
must keep GPU-slot CHUNK600 matched when isolating lag, and separately test CHUNK0/75 and the driver
CS-slab change without conflating the two fixes.

#### Reducing lease-maintenance churn

The3359bfd Thor sweeps retain632–992 secondaries without fallback, but report40–57k
cold resets over240s. `returned` counts **handles**, not publications: retirement already
publishes once per completed slot, under one recorder lock. The remaining return work
walks each handle and wakes idle workers just to recycle it; the old reset policy also
touches handles below the512/owner warm floor after only32 retirement epochs.

Keep the same GPU/post/submit-owner lifetime pins, but link leases while acquiring them
so an owner splices an entire returned slot list in constant time. Publish once per slot;
drain pending returns on the owner's next recording batch, without return-only wakeups.
Cold maintenance must preserve the demand-based warm allowance, wait4096 global retired
slot epochs since last use, and run no more often than256 epochs, only above that allowance.
Actual reclaim still requires gross excess, so ordinary scene capacity is not evicted.
This age is measured from last acquisition; only fence-retired ready handles are eligible.

Report cumulative handles separately from slot publications and owner return batches.
Every300 frames report lease acquisition, return splicing and cold reset/reclaim time for
walking versus recording owners, plus background publication time and actual walking-only
packet-slab trim/slot-arena/snapshot-arena compaction time. The extra clocks are per job or
actual maintenance, never per draw. Host fixtures measure these paths in ns/frame, assert
zero warm-floor resets, retain the large-slot600/0 capacity and pixel checks, and exercise
long-idle excess reclamation separately. Arena compaction remains a retired-only one-time
high-water consolidation, not a per-frame shrink policy.

The same device logs identify a larger walking-path cost: whole-slot budget eviction
reclaims87,714/115,972MiB over the two sweeps, with walking slot reset reaching1.02/1.08ms/frame.
Keep packet arrays used by that slot's latest scene instead of discarding the scene on every
over-budget wrap. Pressure scans run once every8 retired reuses/slot:32 idle generations or
32 consecutive oversized job uses return an array to its size class, preserving normal variation.
A slab tracks referenced capacity; only wholly unreferenced slabs
can be unmapped, after filtering their size-class links. Oversized slabs cannot acquire small
tail allocations that would pin a large old mapping. The64MiB budget triggers pruning of
genuinely idle high-water, not repeated destruction of active-scene capacity; referenced
block capacity is logged separately from mapped capacity. Active demand can exceed the
allowance, but cold old-scene high-water cannot grow without bound. Existing fences and
walking-only ownership remain the authority; no worker sees a relocated or reclaimed packet.

Normal lavapipe path measurements (ns/frame, not Android FPS), with the final production source:

| Workers | Inline16 walking acquire / return | CHUNK600 worker acquire / return | Over-budget1200-pass walking prune |
|---|---|---|---|
|2|1,203 /139|2,039 /626|12,340|
|3|1,360 /162|1,718 /1,728|12,283|
|4|1,258 /162|1,610 /833|11,451|

Inline0 has no walking-owned lease work in this fixture; all warm cold-reset counts/costs are zero.
The84-frame over-budget fixture uses1200 draws (two exact600-draw threshold periods) so its slot-size
phase is stable across NVN commits; after32 warmup frames,52 measured frames retain the same
40/8MiB packet/info slabs and39.8MiB referenced capacity against a16MiB allowance, with zero evictions.
Recorder-owned named VMA counts must stay identical; whole-process VMAs include lazy driver/runtime
mappings and use the existing storage fixture's+1024 total/+128 anonymous bound.
Slot/snapshot consolidation measures29,735/743,156ns once, including replacement allocation,
then zero consolidation work across64 warm frames. Under-budget packet prune time is also zero.
Full body/commit timings include worker gates and joins and are not treated as pure allocator cost.

Validation covers61 named cases per normal/ASan/TSan build (183 cases): base suites with lag0/1
at0/2/3/4 workers, pixel/stream identities, private UBOs, synchronous submit/profile pressure,
both large-slot lease modes, inline16 and over-budget warm scenes, real long-idle reclamation,
VMA/arena cases and the24-child reuse runner with six expected-fatal injections.
TSan has zero reports without suppressions (`LP_NUM_THREADS=0`, `halt_on_error=1`); ASan has zero
address errors with slab poisoning and leak detection disabled. Seven Android cc1 checks are clean
for all five touched C files and both production/test-hook configurations.
After correcting a whole-process VMA assertion to distinguish exact recorder mapping ownership
from lazy driver/sanitizer mappings, every warm case and full runner was rerun in all three builds;
production source was unchanged after the complete core matrix.
The long-idle fixture uses4352 actual empty GPU-slot retirements, then checks resumed pixels and
reclamation: the final normal run retains/reclaims1024/3072,1536/2568,2528/1576 handles at2/3/4 workers.
Small-scene slabs may total32–33MiB at a32MiB allowance because referenced arrays are preserved;
idle capacity obeys the allowance and warmed scenes perform no slab evictions. Device throughput
recovery and long-session matched-configuration map deltas still require the authorized device A/B.


### Shared-structure safety contract

| Structure | Ownership / publication |
|---|---|
| `S`, `R`, counters, uniform log/snapshots, transient arena, generated-block/c1 memos | Walking thread under existing recursive recording lock; resolved values copied before enqueue. Workers never access them. |
| Program/variant/layout maps and shader reflection | Resolved only by walking thread/startup. Worker packets contain handles and copied binding metadata, not mutable shader fields. |
| Descriptor-update template map/entry scratch | Walking-only, once per immutable shader layout. Vulkan template objects and layouts have existing process lifetime; immutable handles and descriptor metadata publish with each job. Worker updates share only read-only templates and use distinct sets/pools. |
| Recorder program/freezing memos and uniform-change journal | Walking-only. Semantic program signatures match ordinary variant reuse. Exact used-binding comparison and complete per-range history can validate changed revisions; unknown history and storage-content dependencies miss conservatively. Freeze ranges remain within one uniquely identified job; writable stages bypass reuse. |
| Pipeline map/prewarm keys | Walking thread/startup only, as before. Compiler workers publish entry handles with release/acquire state; recorder workers consume the already-selected handle. |
| Texture/sampler/view registries and dirty/write-back state | Walking thread under existing resource/recording locks; workers see only Vulkan handles, retained through slot completion by existing retirement rules. |
| Pool spans and address hints | Walking capture copies each index generation under the pool lock, at most once per version per slot. Packets retain immutable versions until slot reuse; workers never read live span generations/maps and use context-local hints. Linear diagnostic mode preserves original alias/pool order. |
| Command/descriptor pools, descriptor dedup and bound-state memos | One command pool per recording owner across slots; per-slot lease chains retain handles through actual GPU/submit retirement. Descriptor/dedup/span contexts remain worker/slot-generation scoped and reset on that owner; with `TOTK_VK_DESC_STOCK=1` the owner rewinds its per-layout set stock at that same generation change instead of resetting its pools, and never hands sets to another owner; a live switch-off or a `TOTK_VK_DESC_STOCK_MAX_SETS` reset destroys pools only there (`--descriptor-stock-live`). Vertex/index/pipeline/dynamic memos start empty per secondary; zero buffers remain immutable context allocations. |
| Packet/descriptor slabs and size-class lists | Walking-only allocator per slot. Growth copies only the unpublished job; workers read immutable blocks after queue publication. Old construction pointers do not survive commit/abort. After GPU/submit-owner retirement, pressure pruning clears only cold job references and recycles blocks; slab live counts prevent unmapping referenced arrays, and free-list links are removed before an empty slab is released. ASan poison tracks returned payloads; physical-capacity counters are atomic. |
| Secondary lease returns and cold-tail maintenance | The final backend busy/submit_busy pin publishes stable completed-slot chain endpoints under rec_mu, once per slot and with no Vulkan calls. Only the recording owner splices these chains into its free list, mutates handle links, resets cold excess or frees/trims the pool. Pending flags protect chains against repeat retirement; a new generation consumes returns before reusing its context. No owner accesses command-pool metadata owned by another recording thread. Counters/timings publish atomically without inspecting owner metadata. |
| Retired primary/arena BOs and cached texel views | Walking-only metadata; reset requires the slot's busy and submit-owner pins to retire, or the snapshot arena's whole-submission release fence. View invalidation holds the recursive resource mutex, including deferred views with the same source buffer. No old packet, Vulkan descriptor or GPU command can reference a compacted buffer. |
| Pressure cutover | One walking owner under the recording lock drains every old queue/GPU/post owner before release-publishing the atomic capture-disabled flag. Original GPU-cursor selection stays immutable, so completion cannot skip its slot-lifetime pin. Future work reads the disabled flag with acquire semantics and never arms old capture state. |
| Backend memory-purpose counters | Successful allocation/free sites update relaxed atomic live counts; logging snapshots them without inspecting mutable allocation structures. They are diagnostics, not lifetime authority or an exact count of driver VMAs. |
| Sealed slots and queue owner | Walking closes/finalizes slot inputs and publishes via `g_mu`; the FIFO owner alone finishes parked primaries, arms native guards and calls QueueSubmit. It never reads live walking state. Completion visibility follows actual queue success; final guest-return/CPU boundaries join owner publication. GPU completion and the submit-owner pin both precede slot/fence/pool reuse. |
| Chunk queues and completion | Mutex/condition-variable packet publication; release/acquire completion permits lock-free reads of finished commands. The worker captures its next job before publishing and never accesses the finished job again. Assembly locks only for unfinished jobs. Ring reuse follows complete primary consumption and its fence. |
| Statistics | Walking totals stay walking-only. Worker/assembly/guard aggregates publish under the recorder mutex and logging reads/resets there. Cross-owner guard/QueueSubmit totals use coarse atomics, with no new per-draw clocks. No worker updates global draw/descriptor statistics. |
| VMA admission and baseline watch flags | Each dirty unit gate owns its page watch bits and reserved/final run credit; global budget/pressure/retained-capacity counters are atomic. Proc sampling/epoch bookkeeping uses a normal-thread mutex, never a fault-path lock. The ordered failed-slot owner drains under the range-lifetime mutex and unit gates only after its queued GPU fence. A sealed native batch is sorted/rehash-complete before worker publication and immutable until every shard joins. |

### Host identity and race checks

`make ... vktest` also builds `vkrectest`, a fresh-process comparison using the existing draw fixture and shader seed.
It runs 0/2/3 recording workers over twelve submissions (recycling the eight slots), each with four draw-heavy
passes of 1, 17, 257 and 1,793 draws. Every draw changes inline uniforms, scissor and texture/sampler handles;
array, uint16/uint8 indexed and quad draws share the stream. Unaligned index attempts must reject without advancing
samples. Submission snapshots precede deliberate guest-uniform rewrites. Pending sample reports control a skipped
clear; nonzero GPU timestamp values must remain ordered within and between passes. The comparison serializes the
**actual** pass/draw/rejected-index counts, pixels, ordered samples and normalized timestamp assertions, not raw clocks.
Each mode passes **243 assertions** and produces **787,888 identical bytes**, with 24,816 draws and 71 actual passes
on this host. Earlier stream versions' pass totals are not treated as the authority for the changed fixture.

A second `--rec-boundaries` stream passes **84 assertions** and compares **409,856 bytes** per mode. It executes a
partial clear that opens a pass, 33 draws with unrelated inline updates, unused UBO/texture rebinds, an executed mid-pass
clear between reports and another draw. It checks exact pixels/pass/samples and requires at least 60 freeze and 30
program hits despite those unrelated changes.
Then a condition-variable gate pauses a real worker **before** resolution while the walking thread replaces a live
virtual alias and records another pass. Per-packet expected Vulkan buffer/offset values are compared with actual
worker resolutions of both frozen versions; one textured result and one offscreen result are copied in stream order.
Twelve rounds wrap the ring, assert owner-context resets and reclaim old span generations. The gate has a timeout
and watchdog, not a scheduling sleep. Test-only hooks/expected fields compile out of production. The boundary fixture
fixes chunk/inline/lead to 128/0/32 for the worker gates and same-chunk memo reuse, even under matrix overrides.
Run the accepted environment legs separately: lag off/rename off, protected lag with rename off, and opt-in
rename with lag off. The boundary/submit tests deliberately reserve a metadata-only `PROT_NONE` virtual alias;
they do not provide CPU-readable alias bytes for the extra lag+rename capture combination. General real-backed
lag+rename behavior remains covered by `vktest`/`vkdrawtest`; do not mistake this artificial-address fixture for
native alias-memory coverage or hide its fault by inventing guest bytes.

The harness runs both streams with native uint8 indices and with test-only forced uint8→uint16 widening, comparing
0 vs 2, 0 vs 3, and native vs widened output (ten complete byte comparisons). Normal and TSan runs pass at
chunk/inline **128/0**, **1/0** and **512/512**; the latter two override only the draw-heavy identity stream.
`vkrecspantest` also covers negative-touch invalidation, frozen pointer overlays across unrelated/overlapping updates,
snapshot death and conservative journal overflow (**7,096** checks at 0 workers; **7,103** at 2/3). `vkdrawtest`
passes **43/43**, covering synchronous and eager asynchronous capture before guest overwrites; `vktest` passes **19/19**.
Normal/TSan 0/2/3 matrices, CALL/JUMP replay, and sampling-rate/chunk variants are recorded in the handoff report.
The 300-present profile/worker-stat paths remain exercised; this artificial present pattern is not a speed benchmark.
No Khronos validation-layer manifest is installed in the host's Vulkan layer directories (only Intel/Mesa utility
layers), so validation-layer execution is unavailable here, not silently claimed. Build/run in a scratch directory:

```sh
PATH=<scratch>/hosttest/bin:$PATH \
  make -C <worktree>/games/totk/android TARGET=linux CC=cc OUT=<scratch>/record vktest
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  TOTK_SHADER_SEED_DIR=<scratch>/record/vkdraw-seed <scratch>/record/vkrectest
```

ThreadSanitizer builds use `CC='cc -fsanitize=thread'` and a separate output directory. On this host, Mesa 26.0.8's
uninstrumented lavapipe with its default raster threads reported an internal `pthread_mutex_destroy` versus
`pthread_mutex_lock` race (both accesses in `libvulkan_lvp.so`). `LP_NUM_THREADS=0` avoids that driver-internal
threading path while retaining **all recorder workers and the backend completion/compiler threads**. With that
environment and `TSAN_OPTIONS=halt_on_error=1`, the 0/2/3 matrix of both existing tests and the cross-mode identity
test completed with zero reports, no suppressions and no sanitizer-disabled backend code. This is not a claim that
the default-threaded driver itself is race-free.

### Device measurement protocol

The original default-off measurement gate is superseded by the round-10.1 owner decision to use two workers on Android.
For further comparisons on Thor (30 fps target) and Odin 3 (60 fps target), use the same bundled
Turnip build, save/camera/weather, resolution, cache files, pipeline compiler/prewarm settings, AYN/ClusterTune mode,
frame pacing and thermal starting point. Warm each Lookout Landing and Gerudo scene, then record at least 180 seconds
per `TOTK_VK_REC_THREADS=0/2/3`, repeating in reverse order. Disable `REC_PROFILE` for primary frame-time/power comparisons;
repeat short profile-on samples separately to check where work moved. Retain all 300-frame lines, attempted/emitted
draw counts, worker utilization, waits, frame-time distribution and power, not only a best FPS window.

For visual controls, take identical-frame `TOTK_VK_PASS_DUMP` captures and compare every decoded attachment pixel and
the stream-order index; startup must say recording threads were disabled by the diagnostic. Such a dump intentionally
does **not** validate worker recording. Compare normal worker-mode `TOTK_VK_SHOT` output against the single-thread
reference in the same deterministic scene as well; shots end/drain the pass before copying and do not disable workers.
The host identity test supplies direct worker-vs-inline byte comparisons; Turnip worker-mode pixel identity and speed
remain device acceptance work, not results claimed here.

## GPU cost (measured on the Thor)

On Standard mode the GPU stays at 401 MHz and the game's CPU thread waits about 8–9 ms a frame on a GPU fence
(`TOTK_SYNC_LOG=1`), so GPU work sets the frame time there.

- **UBWC on render targets.** Turnip on the Adreno 740 (a7xx gen2) disables UBWC for `MUTABLE_FORMAT` images without
  a format list of one compatibility class (`TU_DEBUG=perf` listed every 1280×720 and 1920×1080 target). Images now
  list their own format and its sRGB/UNORM twin; a view of any other format upgrades the image in place
  (`img_upgrade`, the Mac's `tex_upgrade_pfv`). In the world the game reinterprets nothing else, so no upgrade
  happened. GPU time per frame 24.9 → 21.8 ms. `TOTK_VK_UBWC=0` restores the old full-format images.
- **The game's own dynamic resolution.** TOTK measures its GPU time with `TIMESTAMP_TOP` counter pairs (read through
  `nvnDeviceGetTimestampInNanoseconds`) and scales the scene between 80 % and 100 % of its mode size, as on the Switch
  (handheld 1024×576–1280×720). The reports used to carry the CPU time at which their submission completed, so the game
  read ~0 ms and never scaled. Timestamp reports now get a GPU timestamp at their place in the command stream
  (`mtl_sub_counter`, `TOTK_VK_GPU_TIMESTAMPS=0` for the old behaviour): the game reads 26–28 ms at 1280×720 and stays
  there; asked for 1600×900 it stepped down to 1280×720 by itself (27.5 → 31.1 fps).
- **Pass profile** (`TOTK_VK_PASS_PROFILE=<every n-th frame>`): timestamps around every render pass and dispatch of a
  frame, logged as the GPU span split into passes, dispatches and the rest, with passes grouped by target. With UBWC:
  32 ms span = 21.4 ms in 152 passes + 0.4 ms in 23 dispatches + 10 ms rest (mostly the GPU waiting for recording).
  The costliest groups: the 1280×720 G-buffer (6 colour targets + D32, ~750 draws, 4.9 ms), the 360×360 shadow maps
  (D16, ~900 draws, 3.4 ms) and a 1280×720 pass with 8 colour targets and few draws (3.3 ms, mostly attachment traffic).
  Native/host operation scopes separately report seed/resolve/patch/present/post GPU milliseconds, including
  their existing barriers. Native UI/window render-pass annotations reuse the existing pass query and remain
  included in render totals. The residual subtracts separately scoped operations but still includes unscoped
  copies, barriers **and GPU idle**. `dropped scopes` marks query-budget incompleteness; zero is required for
  full attribution. Sampled frames use primary recording, so these are diagnostic timing, not normal-mode fps.
- Not worth doing: `TU_DEBUG=gmem` is slower (28 ms GPU even at 680 MHz), `TU_DEBUG=sysmem` about equal to the driver's
  own choice; the game never calls `DiscardColor` / `DiscardDepthStencil`, so attachment loads cannot be dropped that way.
- Render resolution (`TOTK_RENDER_RES`, game patch): 1152×648 instead of the handheld 1280×720 takes the GPU from
  21.8 to 20.1 ms.
- LRZ and store look-ahead now have conservative opt-in experiments below; neither is enabled by default or measured
  on a device yet. The existing full-screen draw/copy paths are unchanged: coverage alone does not prove an overwrite
  when depth/stencil, discard, blending, channel masks or shader side-effects can leave pixels behind.
- **Real occlusion reports, opt-in** (`TOTK_VK_OCCLUSION=1`, default off: every report says "visible", 1 << 20). The
  game draws a 256×144 occluder depth pass, tests candidate objects with bounding-sphere draws and skips those that
  report 0. With the switch, precise Vulkan occlusion queries count samples: a query segment lives inside one
  dynamic-rendering instance and ends at each reset, report and pass end; the completion thread sums the segments of a
  chunk before recycling its slot, so each report keeps the exact total at its point. Needs `hostQueryReset` and
  `occlusionQueryPrecise` (startup logs it if missing); conditions on still-unknown sample reports stay enabled, as on
  Metal; `[occl]` logs queries per frame and the share reporting 0. Lookout Landing: 76 queries a frame, 25 % report
  0, draws −1.6 %, recording +1.4 ms (the queries), so no gain there; it stays opt-in.

**Vertex position precision:** `TOTK_VK_POS_INVARIANT=1` and `TOTK_VK_POS_NOCONTRACT=1` are independent, startup-only, default **0**.
The first adds `Invariant` to vertex Position outputs (including block members); existing seed decorations are preserved in every arm.
The second adds `NoContraction` only to core float arithmetic feeding Position through SSA, calls and conservative memory aliases; explicit fused Fma remains fused. Invariant/NoContraction guarantees require identical position computations and inputs: they do **not** make arbitrary shaders, or depth across unrelated shaders, identical.
Each opt-in arm isolates shader/pipeline/prewarm cache identity. Compare identical fresh-process camera sweeps for flicker, GPU ms and compile/warm-up cost; no speedup is claimed.
Host checks: `python3 -m unittest discover -s tests -p test_vk_position.py -v`; with `spirv-val`, set `TOTK_TEST_POSITION_SEED` to a loose seed directory or `seed.vkpack` for real vertex samples in all four arms.

### Original NVN early fragment tests (guest semantics)

Original shader **declared** early depth/stencil tests are honored **by default**. `TOTK_VK_EARLYZ=0` is an explicit,
non-faithful diagnostic override for the old late-test path; unset or `=1` preserves the guest contract. This is not a blanket force-early optimization. NVN's compiler control
section carries `early_fragment_tests` at **byte 0x718**, immediately after stage at 0x714; it is separate from the
Maxwell SPH's global-store/kill flags. The public
[`NVNshaderControl` layout](https://github.com/nvnprogram/uam-nvn/blob/f4470ae120392f76ecfdce74390dbb13a461a9df/source/nvn_control.h)
matches the real v1.4.3 controls, including all 73 seeded fragment-depth shader hashes' `writesDepth` byte at 0x71c.
Of 13,647 original fragment shader keys, 1,677 request early tests. The 2026-10-23 device-seed snapshot contains 24,748
variants; 1,631 hashes / 1,786 fragment variants already declare the mode. The previous default-off runtime stripped
it during module creation, so faithful source translation alone did not suffice.
Both eight-MRT title G-buffer shader hashes (`37bb03a9…`, `679c5257…`) request early tests.

The runtime reads the original control byte from full 0x880-byte compiler controls and normalizes only SPIR-V
`ExecutionMode EarlyFragmentTests` before module creation. Shorter BOTW controls do not interpret this TOTK field.
Existing seeds work without regeneration: missing modes are inserted, current faithful modes are retained.
Shader identity already hashes the control bytes; shader-module and pipeline/prewarm keys retain the original bit.
The driver pipeline cache sees the resulting exact SPIR-V, so an old compiled late module cannot masquerade as early.
Prewarm records remain version 2 because their existing original-mode field already carries this state.
Offline preparation and shader-miss requests pass the same flag through Ryujinx's `EarlyZForce` emitter.
Prepared manifests now explicitly require `early_fragment_tests_policy=nvn-control-0x718-v1` in addition to source and
translator SHA-256 identity; older manifests cannot be reused under that policy. This is an Android seed identity
epoch, not a global Metal chain rename. No new mutable texture/bind-state dependency is needed.
Depth exports, discard instructions, storage stores and atomics remain intact.
The packaged Linux translator is rebuilt from the reviewed source too: the older checked-in binary ignored the job
field despite the source support. Direct and service probes of the replacement emit mode counts 0/0/1 for absent,
false and true requests. The current deployed seed was already built with a newer, capable translator; it needs no
SPIR-V regeneration for this runtime correction. An explicit later seed preparation will rebuild its manifest under
the new policy identity rather than relabel an old one.

This matches the semantics of upstream
[`47d4b72` / `ae8f4f4`](https://github.com/ly05010419/MacNativeGames/commit/47d4b7276f1bbb75cff8f543b169138f1bae60ca).
Upstream reports that original-early G-buffer depth-EQUAL draws avoided late-test rounding differences from the
prepass and removed Great Sky Island lattice dots on Metal. That is a correctness rationale, **not** a demonstrated
Adreno visual fix or a new LRZ/FPS result. Check the lattice near the Great Sky Island start on Odin fixture
`slot_00-insa`, with the override unset/1 versus explicit 0, in fresh processes at identical camera/resolution.

Early tests intentionally change **when** side effects and depth/stencil updates occur: depth-rejected fragments must
not write storage for original-early shaders, and discard does not undo an already-performed early depth write.
Forcing the mode on original-late shaders is incorrect. Turnip's `Skipping LRZ due to FS` is the same side-effect/depth
predicate as `FS writes depth or has side-effects`, with a different current LRZ-direction branch; discard alone only
disables LRZ **writes** for the draw. This switch cannot remove genuine `ALWAYS/NOT_EQUAL` depth-write invalidations or
the sticky `Depth write + blending` restriction (also includes partial/missing color writes). Depth-write enable while
depth testing is disabled is already inert in Vulkan/Turnip; it is not an avoidable LRZ cause.

Host fixture: `python3 games/totk/android/gfx/test_vkearlyz.py` checks default/override/control-layout policy and real
offscreen Vulkan color/depth/SSBO readback, including depth-EQUAL with depth writes disabled and original-late controls.
Device acceptance remains required: fresh processes with `EARLYZ=0/1`, identical Lookout save/camera/weather, same
`REC_THREADS=2`, driver, resolution and thermal/clock policy; retain equal warmed windows of `TU_DEBUG=perf` reason
counts, GPU ms and pass-profile G-buffer ms. Then repeat the camera sweep and 9-frame lighting cycle, title/loading,
alpha-cut foliage, water/transparent passes, Gerudo and the Depths. Compare screenshots, sweep fps/pacing and power;
warning-count reduction alone is not a measured speedup. Existing `CLEAR_LOAD=1` showed no fps gain in the supplied
Lookout sample (184 vs 201 depth-clear invalidations), so clears remain an independent opt-in, unchanged here.

Subsequent Thor Lookout A/B of this change measured **no standing improvement** (within noise); the camera sweep was
28.5 fps off versus 27.2 fps on. Raw perf-message counts off/on: FS invalidation 6,290/3,529, temporary FS skip
5,998/7,702, depth-write+blending 7,703/8,378, ALWAYS/NOT_EQUAL 1,991/2,206. These are not frame-normalized. A temporary
skip can rise when an earlier invalidator disappears: `tu6_calculate_lrz_state` silently returns for already-invalid
passes, but now reaches the later original-late shader and reports it. Early+discard alone does not select the FS-skip
predicate; it only inhibits LRZ writes. The blend warning also inhibits **writes**, not the test itself. The retained
Lookout dump's blend-enabled depth-write candidates are material passes p0039 and p0054, FS `e4dc227f…` / `c9fd3040…`;
those also export unconstrained FragDepth, which independently disables the LRZ test. Do not remove depth writes or
Turnip's conservative blend guard to silence these messages.

For a fresh `TOTK_VK_PASS_DUMP` capture, depth-test+write draws now include `lrz:` detail: pipeline handle, raw NVN and
effective Vulkan depth state, raw/effective blend mask, logic op, alpha-to-coverage, original FS early/depth/output flags,
attachment formats, and each target's effective blend enable, channel mask and RGB/alpha factors/equations. It uses
the same blend-enable filtering as pipeline construction; integer/inactive targets cannot be misattributed. This is
dump-only diagnostic work, not additional normal-play logging.

### Opt-in attachment work reductions (device measurement pending)

Both options are read once at startup and default to **0**. They preserve command order, untouched attachments,
scissored clears and render conditions; they do not change shaders, resolution, depth functions or actual depth writes.

- **`TOTK_VK_CLEAR_LOAD=1`: full clears as load operations.** If a full-channel/full-stencil-mask clear covers an open
  pass's entire render area, finish that pass and queue the clear for the next pass's `loadOp=CLEAR`. Consecutive clears
  are combined by the existing pending-clear path. The full-area test uses the actual open pass's extent (including mip
  views and differently sized targets), not just texture 0's base size. Partial, masked and disabled clears retain their
  existing path. This can save a load/clear draw and, for depth clears, reinitialize LRZ instead of invalidating it.
  **Risk:** another pass can add more GMEM traffic than it saves, especially for a color-only clear with depth retained.
- **`TOTK_VK_STORE_CLEAR=1`: bounded clear-before-read store look-ahead.** This also enables `CLEAR_LOAD`, because a
  predicted overwrite must begin a new pass rather than share the pass whose store is discarded. At a draw that opens
  a pass, inspect at most 128 following records in that recording. Use `storeOp=DONT_CARE` only for an attachment fully
  cleared on the exact same target/view set before any later attachment load or possible sampled/image read. Unrelated
  draws can be crossed; any later draw using the attachment retains its store even if it might remain in the same Vulkan
  pass (buffer uploads and other implicit splits must remain safe). Copies, dispatches, updates, counter/event operations,
  conditions, calls, jumps, unknown operations and the recording end stop the search. All currently bound vertex/fragment
  texture and image slots count as reads, including same-address aliases; unknown bindings and aliased attachment slots
  retain stores. The opening draw's bindings are also checked. Clear-only passes flushed before a draw never discard:
  the opening draw may still load them. No cross-submit prediction or speculative full-screen-overwrite elimination.
  Pass dumps retain all stores, since the dump reads pixels before the future clear.
  **Expected effect/risk:** fewer GMEM resolves when an eligible single-draw pass produces a dead attachment; no resolve
  savings in sysmem. The cheap structural scan runs first; binding copies/registry checks run only for actual candidates.
  Those checks can nevertheless cost recording time, so compare CPU time as well as GPU time.

The pinned reference is Mesa mirror commit
[`0ac3efa9f4f99ece07971de98360f339a1dc8803`](https://github.com/chaotic-cx/mesa-mirror/tree/0ac3efa9f4f99ece07971de98360f339a1dc8803).
Exact rules (driver versions may differ):

- [`tu_clear_blit.cc:5267-5271`](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_clear_blit.cc#L5267-L5271)
  invalidates LRZ only when `vkCmdClearAttachments` clears **depth**, not for a color-only clear.
  [`tu_lrz.cc:265-301`](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_lrz.cc#L265-L301)
  initializes LRZ from the depth attachment's clear/load state.
- [`tu_pass.cc:953-958`](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_pass.cc#L953-L958)
  enables stores only for `STORE`; [`tu_clear_blit.cc:6330-6331`](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_clear_blit.cc#L6330-L6331)
  skips GMEM writeback if neither aspect stores.
- Avoiding a depth-write flag while depth testing is off is **not** an LRZ fix in this driver:
  [`tu_lrz.cc:1312-1316`](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_lrz.cc#L1312-L1316)
  already returns before changing LRZ. Actual `ALWAYS`/`NOT_EQUAL` depth writes invalidate it
  ([lines 1407-1417](https://github.com/chaotic-cx/mesa-mirror/blob/0ac3efa9f4f99ece07971de98360f339a1dc8803/src/freedreno/vulkan/tu_lrz.cc#L1407-L1417));
  suppressing those writes would change rendering. Shader reflection's `rw`/`writes_depth` fields are not passed as
  pipeline side-effect flags: Turnip sees the actual SPIR-V. No flags or real storage/depth writes have been stripped.

**Opportunity estimate, not a measurement of the new paths:** the retained Lookout Landing frame 2400 dump contains
5,704 draws, 147 passes, 52 enabled color-clear commands and 15 passes starting with a depth clear. All 52 color clears
already precede their pass's first draw; expect **zero color-clear splits in that captured frame**, not 52. Mid-pass depth
clear counts are absent from that dump, so their conversion rate is unknown (the new counter is the measurement).
Store look-ahead is narrower than Metal's per-pass-end index: passes with another draw on their own attachments cannot
qualify, and stale bindings/128-record boundaries may conservatively reject the rest. Expect zero to a few candidates per
frame, not a claimed frame-wide gain; a zero counter is a valid reason to leave it off.

With either option enabled, `frame …, GPU work per frame:` logs full-clear **pass splits**, remaining inline depth clears,
and color/depth-stencil stores skipped plus affected passes, all averaged over the same 300 frames as `per frame:`.
Use fresh processes for A/B: both 0; `CLEAR_LOAD=1, STORE_CLEAR=0`; then `STORE_CLEAR=1` (which includes `CLEAR_LOAD`).
On both Thor/Adreno 740 and Odin 3/Adreno 830, use the same warmed save/camera at Lookout Landing in the rain, first fixed
at the 401 MHz comparison point if supported, then normal governors for power. Keep frame lag off. Compare the existing
`per frame: … GPU … ms` **and recording time**, `TOTK_VK_PASS_PROFILE=300` pass-group times (especially G-buffer and
shadow maps), fps/stutter, GPU clock/busy and system power over equal sustained windows. Do not infer savings solely from
an application counter: sysmem passes skip no GMEM resolve. Compare screenshots (`TOTK_VK_SHOT`) and the same camera sweep,
including the 9-frame lighting cycle; also check Gerudo Town, the Depths, menu transitions and partial/scissored clears.
`test_vkdraw.c` adds color/depth readback cases for full, partial, disabled and copy-before-clear paths; run its existing
host harness separately with each option combination. Device timing, power, visual comparisons and host execution are
pending; only Android compile checks are part of this change's host acceptance.

### Region and channel-masked texture clears (correctness, unconditional)

NVN `ClearTexture` clears a region of one level and only the channels in its mask. It used to become
`vkCmdClearColorImage`/`vkCmdClearDepthStencilImage` of the whole level and every channel (logged `cleared whole`).
A region or colour channel mask now opens a temporary pass per layer/slice on that level (`loadOp=LOAD`): a region is
`vkCmdClearAttachments` with the region as its rect; a mask draws one triangle through a small pipeline whose
`colorWriteMask` is the mask (`vk_clear_mask.vert/.frag`, float/uint/int outputs), scissored to the region. Mask 0
writes nothing. Whole unmasked clears keep the image clears; views that cannot be render targets keep the old path and
its log line. `vkdrawtest` checks level-0/level-1 region, green-only and red+alpha clears texel by texel.

### Texture ID memo (`TOTK_VK_TEX_MEMO`, default off)

Descriptor builds resolve every sampled texture ID through `vk_tex_by_id`; on the Duo walker that path
(`vk_tex_by_id` + `tex_get_inner` + `vk_treg_kind`) took ~2.7 ms/frame in T7, mostly cache misses: the ID's generation,
its cache entry and two identity cells per lookup. `TOTK_VK_TEX_MEMO=1` puts a per-thread direct-mapped memo (2,048
entries) in front of that cache, keyed by ID and shader target and valid while the registry revision, a global
texture/image identity revision (every stamp bump or drop) and the submission are unchanged; any change retires the
whole memo, which then refills from the cache. With it on, a registration that writes the same texture (same bytes,
serial included) and view again keeps every revision, so it no longer retires the memo, the stage signatures (the
`vk_treg_kind` scans) or freeze keys. Startup logs the value; `[tex-memo]` reports hits, misses, hit rate, retirements,
registrations and unchanged ones per frame every 300 presents. Live: each lookup and registration reads it.
`TOTK_VK_TEXGEN_CHECK=1` re-resolves every memo hit through the cache and aborts on a mismatch.
`vktest --texture-cache-bench`/`--texture-churn-bench` and `vkrectest` (texture/sampler handles changing per draw)
cover it.

### Same-pass mip feedback (`TOTK_VK_FEEDBACK_CUT`, default off)

A pass that renders one level of an image while sampling that image (mip chains generated level by level) has a
feedback loop when the sampled view includes the attached level; Vulkan leaves it undefined and Adreno shows NaN
blotches. `TOTK_VK_FEEDBACK_CUT=1` gives such views only the levels below the attached one (`[base, attached)`, keeping
LOD numbering); a view based at the attached level is left whole. Startup logs `TOTK_VK_FEEDBACK_CUT=…`;
`[feedback-cut]` logs cut and left-whole views per 300 presents. Compare screenshots (`TOTK_VK_SHOT`) of scenes with
blotches. `vkdrawtest` renders a mip step with the switch off and on and checks identical texels and the counters.
Live (`totk.ctl`): each descriptor build takes the current value; the freeze memo's key carries the pass serial only
while the cut is on, so sets built under the other value are never reused.

## Upstream Android integration (2026-10-22, device measurement pending)

`origin/android-port` through `d2eb744` is merged with the measured dev work above; [UPSTREAM_MERGE.md](UPSTREAM_MERGE.md)
records all 57 commits, every conflict resolution and the device A/B matrix. The measurements above describe dev,
not this combined build. Upstream's separate Thor measurements and debugging history are in `DEBUGGING.md`.

- Keep dev's binary fences, chunk submissions, GPU timestamps, opt-in precise queries, pending-counter ring, span
  index and draw/stage/lookup memos. The pending ring now snapshots upstream's cumulative synthetic sample count
  (reset to zero, each emitted draw adds vertices × instances), rather than always reporting `1 << 20`.
  `TOTK_VK_OFF=condorder` restores memory-only conditions and the old fixed synthetic count. Default ordering handles
  both the terrain's repeated conditional clear and upstream's camera-cut shadow/AO-mask clear; see the ledger's proof.
- Upstream's full DEPTH16 nudge is **on** (`TOTK_VK_D16TIE=0` or `TOTK_VK_OFF=d16tie` off): rasterized depth,
  fragment-written depth and tie clears must agree. The earlier raster-only experiment broke clothes; upstream
  confirmed the complete fix correct for water, clothes and horizon on the Thor.
- GPU-written CPU-pool images share storage; compressible format classes stay isolated. First settled renderings write
  back by default after the producer completes and 60 quiet submissions: an image-to-staging copy rides the normal
  stream; later commits poll its fence and swizzle guest memory in contiguous16KiB spans, at most1MiB/~2ms per
  automatic sweep. Partial level/layer progress retains staging/image pins until completion. This path adds no submit
  or CPU wait; per-submission targets only update bookkeeping and never stage. Successful per-storage/layout landings
  double the next quiet interval (cap 4096 submissions), across serials/CPU updates; a new pool or layout starts fresh.
  Learning retains no protection: the first CPU write disarms the whole storage watch and the next commit cancels
  its record immediately, preserving only its learned interval. Only a new GPU write re-arms WB. Record retirement
  removes exact-page exclusions too. A bounded static ~192 KiB slot table groups these watches without per-page
  allocation, signal callbacks or locks. WB watches publish no GPU-reader tickets: CPU faults never wait for their
  producer/readback fence; independent actual buffer-reader guards on those pages remain intact.
  Default invalidate of any noted unlanded rendering bypasses **both** base and learned delays: stage if needed,
  publish after its producer in stream order, wait the exact copy fence, then land. No full GPU idle is needed.
  Dirty ownership may additionally wait for actual newer readers of those guest pages, not unrelated submissions.
  Only automatic staging depends on quiet time. `TOTK_VK_WRITEBACK=1` selects the separate full legacy flush/authority path.
  Explicit invalidate finishes all remaining chunks, bypassing the sweep budget; uninvalidated CPU reads can see
  partial progress until completion. Each chunk rechecks full pool lifetime/bounds and CPU freshness, acquiring only
  its destination pages. The shared swizzle engine supports bounded GOB spans without a shadow allocation or second
  layout implementation. The cooperative2ms check cannot preempt one syscall or OS deschedule.
  Default invalidate first checks exact pending spans in a short metadata-only index, before frontend drain or
  owner request. Unrelated hull holes/already-landed storage cannot serialize the game's async command queue;
  candidates are revalidated after draining, **without dropping a newly fresh producer**.
  Synchronous landing publishes a borrowed view of its existing detached list's immutable ranges before index
  removal, until cleanup completes. Concurrent explicit consumers still match and serialize; they cannot return
  early merely because another caller owns the record. No new allocation/refcount or long metadata-lock scope;
  hull holes stay cheap even while that owner waits. In-flight overlaps have separate event/cumulative counters.
  Explicit consumers now use a synchronous **cooperative recording-owner service**, never a blocking recording-lock
  acquisition: try-lock if idle, otherwise service at bounded64-opcode chunks or after slot seal, before unrelated work.
  Empty polling is one relaxed atomic load/branch per chunk. Only stack request/completion publication uses the
  short request mutex; no renderer/GPU work runs under it. The callback preserves current stream/native conversion,
  latest-producer revalidation, exact copy-ticket wait, dirty ownership and resource lifetime rules. An unfinished
  same-image producer defers until seal, never an early partial rendering. At most64 completed opcodes/ordinary draws
  separate polls, including draw-free/jump-only streams; one driver call/deschedule or mandatory wait is not a hard50ms promise.
  Both paths share copy/layout/landing code, reject newer CPU data, and cancel superseded GPU results. A retained
  pool-registration identity/full-bounds check and lifetime lock prevent writes into finalized/reused pools.
  Dirty BUSY ownership precedes the final freshness check; busy owners/in-flight memory readers defer automatic
  landing. The existing temporary-RW protocol remains: a fresh direct CPU store during that writable window (even
  before the freshness check) is a residual race, not strict arbitrary-store exclusion.
  `TOTK_VK_OFF=mirshare,compalias,writeback,settledwb` disables these independently. Dirty protection retains 64 KiB
  Linux/Android units (`TOTK_DIRTY_UNIT_KB`, or upstream `TOTK_DIRTY_UNIT` override in system pages), with per-page state
  for exact write-back tracking. Independent WB refs pin images until landing/cancellation; released shared images
  follow the normal600-submission idle cache. Empty record/index/history arrays are released; history holds at most
  4096 storages/no image refs. Empty64MiB image blocks stay allocated for reuse. Aggressive cold-image/empty-block
  release was deleted after the device A/B found a~7fps loss with no memory benefit.
  Always-on `[settledwb-tile]` lines retain per-storage GPU/landed/staged versions and disarm reasons for260x260
  RG8 nine-level tiles, including invalidates **after** records were dropped. Equal GPU/landed versions prove
  that the latest noted version landed, not that every subsequent CPU read used it or every GPU writer was noted.
  The512-row metadata-only ledger neither pins resources nor reads guest bytes/drains/acquires the recording lock.
  Unchanged invalidate states are coalesced; `settled write-back tiles:` counts all overlaps. Tile realization
  decisions use `[settledwb-tile-view]`. See `DEBUGGING.md` for the intermittent one-in-four Round14 failure and
  next-device evidence requirements; aggregate landing totals alone do not qualify grass correctness.
  Invalidate logs include the actual address/size; CPU-cancellation detail carries the first fault/pre-write byte,
  whether it lies inside the texture, and dirty stamps. A same-page adjacent store is not silently claimed as an
  in-range write; address0 denotes unavailable provenance. The GPU landing protocol sets no CPU provenance/stamp.
- NVN barriers inside a pass now end it (`TOTK_VK_OFF=passbarrier` off); present tickets wait for their GPU fence on
  acquire (`TOTK_VK_OFF=present_wait` off; `TOTK_VK_FRAMES_AHEAD=1` stricter, default 2). These can change throughput and
  power. They do not establish whether the separate 9-frame lighting glitch reported with protected frame lag is fixed.
- Descriptor pools are reused in order, not just the last pool; the `memory:` line should plateau rather than gain a
  pool each frame. Shader storage writes are inferred from SPIR-V (`TOTK_VK_OFF=storagewrites` off), preventing writable
  storage from using stale snapshots. Unique uniform-rename memo and GPU-write de-duplication improvements are retained.
- Android-default asynchronous virtual-core preemption, priority inheritance and starvation escape are enabled
  (`TOTK_ASYNC_PREEMPT=0` off; `TOTK_STARVE=0` disables just starvation). Loader PC classification and interrupted-PC
  thread dumps accompany them. Upstream observed overworld hangs before these fixes; combined scheduling/power is unmeasured.
- Both diagnostic dump interfaces remain: the env-driven binary range dump above and `passdump <n>` appended to
  `TOTK_INPUT_FILE` for sampled PNGs. `TOTK_VK_PASSPROBE` defaults off; the flash detector defaults on
  (`TOTK_VK_BLACK=0` off). `diagnostics:` adds barrier splits/missing bindings; `memory:` adds backend allocations.
  Pass-probe uniform bytes use the existing logical CPU update/snapshot reader, not raw guest memory that an
  earlier Vulkan chunk may still be updating. Storage-buffer diagnostic reads retain their existing raw-memory
  semantics. The chunked uniform fixture in the plain TSan app build exercises this diagnostic-only race boundary.

### Settled write-back controls (fresh/concurrent consumers corrected; eight clean device runs)

The frozen combined no-nx APK`58e2663`/SHA16`dbf1a90cf2859d14`, hybrid OFF/default play env, retained grass in
all eight200-second runs; four interleaved actual-play controls were also clean. Exact version/range chains prove
age32–47 fresh normal consumers completed before return, not merely that aggregate landings increased.
The diagnostic baseline was0/4 failures, so no failure-rate reduction or sole historical-cause claim follows.
Detached concurrent consumers are covered by deterministic host/TSan tests, not exercised device events.
See `DEBUGGING.md` and `.scratch/grass/intermittent.md` for evidence, visible startup stalls and residual CPU-store limits.
The subsequent cooperative owner-service stall repair is written but needs its own device comparison; earlier
eight-run qualification above predates that ownership change. Its host400ms unrelated-holder fixture checks
eligible consumers below50ms without weakening an unfinished producer's completion rule.

| Environment setting | Policy |
| --- | --- |
| `TOTK_VK_OFF=settledwb` | Disable default-on settled-rendering write-back; `all` also disables it. Does not enable/disable the separately gated blocking path. |
| `TOTK_VK_SETTLED_WB_SUBS=N` | Positive automatic quiet-submission interval, default60. Invalid/zero values use60. Storage landings double the next automatic interval up to max(base,4096); explicit invalidates bypass both delays. |
| `TOTK_VK_SETTLED_WB_DETAIL=1` | At most 64 `[settledwb]` lines per 300-frame window: shape/format/levels, serial/storage/pool, guest/image bytes, storage landing count, old/next quiet interval, idle/invalidate cause. |
| `TOTK_VK_WB_SERVICE_STATS=1` | Default OFF. Every300presents: recording-lock hold max/peak and owner-service delay p50/p99 upper bounds/max (100µs histogram bins, overflow uses max); no idle opcode clocks/locks. |
| `TOTK_VK_WRITEBACK=1` | Select full legacy invalidate flush/authority behavior, default off; not required for fresh default explicit consumers. `TOTK_VK_OFF=writeback` disables only this legacy path. |
| `TOTK_VK_OFF=settledbackoff` | Disable learned history; use the base quiet interval for every rendering. |
| `TOTK_VK_OFF=settledinval` | Disable default explicit-consumer landing at all ages; keep ordinary automatic settled copies. |
| `TOTK_VK_OFF=settleddrain` | Diagnostic only: omit frontend drain for a matched default consumer; may weaken queued-command ordering. |
| `TOTK_VK_OFF=settledseal` | Omit optional seals of unrelated open streams for already-submitted readbacks. Required copy submission/future-reader safety publication remains. |
| `TOTK_VK_OFF=settledcpu` | Diagnostic only: restore old per-page exact watches and quiet-deadline CPU cancellation. |

The policy change follows actual device evidence: a214528-byte normal tile was invalidated at age48, still zero,
then read by guest SDK code/LR`main+0x6d6ae8` before staging. Later CPU stores (LR`main+0x2ef77c`) genuinely disarmed
those records. Four same-series diagnostic baselines were visually clean but all exercised fresh invalidates;
this establishes an incorrect read-ordering class, not sole attribution of the historical intermittent visual loss.
The intrusive read-watch hook was removed. Fresh explicit consumers are handled generically, without tile special cases.
`[settledwb-invalidate-fresh]` logs each fresh batch's input range, actual landed count/bytes and copy-fence wait.
Every300frames, `settled write-back invalidate fresh landings: N (B bytes), copy-wait X.XXX ms cumulative`
exposes its cumulative cost. A count growing every frame means per-frame explicit-consumer work, not free async settling.

Every 300 frames with activity/pending records, the cumulative `settled write-back: landed N (M.MMM MiB), pending P,
skipped S (CPU wrote since), superseded U, staging B bytes, copies C, at invalidate I, WB faults F (X.XXX/frame),
dirty faults D (Y.YYY/frame)` summary distinguishes automatic work, explicit consumers and fault cost. Pending/staging
are current; other counts are cumulative, rates cover elapsed present frames since the prior report. About 64 normal
tiles (~13 MiB) should land promptly after load. Automatic copies of an intermittent storage should decay/plateau;
real explicit consumers may increase `at invalidate`. Staging should return to zero. The separate
`settled write-back memory:` line reports record/history capacities, storage count, unique pinned image allocations,
the WB-only subset and staging; `memory image blocks:` reports used/free bytes with empty blocks retained for reuse.
Every300frames (also with settledwb off), `settled write-back stalls:` reports maximum wall scope/caller and lifetime
peak, phase maxima, actual WB submissions/calls and preflight counts. `WB sweep max ... (peak ...), bytes max ...,
chunks max ..., budget yields ...` isolates bounded landing; `staging-free` exposes driver disposal cost. Whole
resource-sweep includes other retirement work. CPU work/waits overlap and must not be summed or read as GPU time.
The pre-backoff Odin 3 A/B retained grass without the legacy synchronous path but still copied intermittent small
targets and retained one extra 64 MiB image block. Pipeline/texture counts also differed, so its ~82 MiB malloc delta
is not evidence that write-back records leaked that amount. The `3aa3fae` device A/B confirmed malloc 355/358 MiB,
image blocks 704/704 MiB and plateauing landings, but 30.08/48.65 fps with excess game-thread dirty faults.
`b06b53e` reduced faults but still ran29.85/48.04 fps with equal memory, so faults were not the mode-lock cause.
`8dd99b4` fixed false invalidate drains (2100lookups,0drains/locks/WB submissions), but default41.63fps versus48.66off
rose to49.23fps with aggressive releases disabled, grass retained and malloc358/361MiB, blocks704/704MiB. Those
release paths/tokens are deleted. Remaining release-off resource-sweep spikes reached317ms (control peak3ms);
the later eight-run qualification includes the resumable1MiB/~2ms landing budget, but not cooperative owner service
or its hybrid/native-interpolation interaction. See `DEBUGGING.md`; host coverage alone does not qualify that combination.
Run `vkdrawtest --settled-wb-race` for the concurrent landing boundary test: 128 trials each before ownership,
faulted under still-RO BUSY ownership, temporary RW before/after freshness, and after RO restoration; every writer
stores 256 rounds into texture bytes and both adjacent same-page guards. The three protected/stamped cases must
preserve CPU bytes; the two temporary-RW cases intentionally demonstrate residual overwrite without dirty publication.
All adjacent guards must stay intact. Test-only handoffs do not exist in shipping code.

Run `vkdrawtest --settled-wb-learning` for 64 prompt RG8 tiles, 40 intermittent refreshes/new serials without recurring
automatic copies, default invalidate before learned deadlines/reuse of staged data, CPU overwrite, fresh rendering,
different-layout reuse, pool-death and final WB metadata/pin/staging release. Cached image allocation/block retention
now follows the ordinary cache. Use the normal and protected-lag/two-recorder modes, plus TSan, as for the race fixture.
Run `vkdrawtest --settled-wb-cpu` for four GPU re-arms ×300 CPU rewrite frames: ≤1 fault per re-arm, no later WB
protection/records/copies, persistent learned intervals and correct CPU bytes. A gated producer must not block
the CPU fault. Genuine independent buffer-reader gates still retain their usual safety waits.
Run `vkdrawtest --settled-wb-preflight [settledinval|settleddrain|settledseal]` for4,096 hull holes without drains/locks,
exactly one matched consumer, and already-staged optional-seal behavior. A deterministic real-GPU producer replayed
during drain must land its newer/fresh version, not be skipped by the former post-drain age predicate.
Run `--settled-wb-cutout settledbackoff|settledcpu` in separate processes for the remaining switches.
The fixture also gates an actual first consumer after detachment, issues4,096 holes without synchronization,
and requires an overlapping second consumer to block until complete guest data is ready. The borrowed list is
published before index removal and unpublished before free, with all lookups under the short metadata mutex.
Run `--settled-wb-service` for a400ms unrelated recording hold, fresh/already-staged consumers below50ms, correct
nine-level bytes/padding, resumed unrelated pass and a150ms unfinished-producer deferral. Two requests must escape
one unbounded draw-free jump stream, proving no draw/return/submit is needed. Run under normal/protected lag and TSan.
`--lag-walk-bench` compares idle ns/draw with the pre-change binary; test-only
`TOTK_VK_TEST_OWNER_PENDING=1` adds real no-op owner requests and reports their completed count.
Run `--settled-wb-budget` for partial >10MiB landings, byte/yield accounting, complete invalidate and CPU/GPU/pool
cancellation between chunks, plus a point-address swizzle oracle covering2D/array/3D/linear layouts and padding.

## Finding rendering errors: the frame dump

`TOTK_VK_PASS_DUMP=<frame>[-<last frame>][,<min width>]` copies the attachments of every pass of one frame (or a range) and logs, in recording
order, each pass, draw (state, vertex input, textures, storage buffers), dispatch, clear, render condition, counter report
and small buffer write (switches in `vk_draw.c`; `android/passdump_png.py` makes PNGs). It found the flat light-blue
wedge over the lower right and the washed-out distance: TOTK clears its linear-depth target (8-target G-buffer, colour 7)
under a render condition "counters at a and a + 16 differ" and reports a counter to `a` right after, so the clear runs
once per frame. Reports reach memory when their submission completes, and the condition read memory at recording, so
every guarded clear of the submission ran again: the terrain, which discards fragments behind the linear depth, drew
nothing, its tiles stayed unclassified, the half-resolution depth stayed empty and the sky layer's horizon mesh showed
through. Render conditions now read the newest pending report of an address (`vk_counter_value`, as the Mac's `cpend`):
terrain back, and 29.7 → 30.5 fps on Large + Standard (GPU 21.9 → 20.9 ms, the repeated clears gone).

The same kind of bug broke the Depths: stretched triangles across the screen, changing every frame on a 9-frame cycle
(the game spreads its work over 9 frames; 312 frame-wide brightness spikes in 25 s of video). Serialising CPU and GPU,
turning off uniform snapshots or GPU timestamps changed nothing; skipping G-buffer draws by vertex shader isolated four
vertex-pulling shaders (no vertex inputs) whose vertex buffer address sits in a uniform block (`RB_SBUF_UBO`). The game
sets that address per draw with an inline uniform update, which reaches memory only when the GPU runs it, so the binding
read the address from before the update: another mesh's vertices. `vk_ub_read` gives the bytes a uniform binding reads
at that point (snapshot or memory, with the logged updates laid over in order); 0 spikes since.

Two log lines help with stutter: a frame over 100 ms says where its time went (recording, pipelines made at the draw,
compute pipelines, shader seed file reads, GPU waits, the game's sync waits), and the 300-frame stutter line counts
pipelines compiled on worker threads, synchronous compiles and prewarm waits, plus a separate seed-files line.

## Compared with LASRecomp-Android (Link's Awakening, Turnip on Adreno 650)

Its Vulkan backend (github.com/Nycolazs/LASRecomp-Android-Plus) has the same pass, barrier, layout and descriptor
policy as ours and measured the same dead ends (DONT_CARE loads, forced GMEM, LRZ). Its gains are elsewhere; on the Thor:

- One-frame-late fence waits (the game's per-frame `nvnSyncWait` waits for the previous submission): removes the CPU's
  ~8 ms GPU wait, but needs every GPU-read guest buffer snapshotted at submit and costs a frame of latency and heat. Not
  done: TOTK reads GPU results that this would delay.
- Pre-rotated swapchain: its gain came from SurfaceFlinger composing a rotated layer with the GPU. On the Thor our layer
  is composed by the display hardware (`composition=DEVICE`, ROT_90 and scaling in the HWC), so there is nothing to win.
- ADPF with CPU busy time instead of the present interval: 29.2 vs 29.7 fps paced on Standard mode, more slow frames.
- Frame generation (30 → 60 by reprojection, ~1.4 ms GPU) and its own dynamic resolution + Snapdragon GSR: TOTK has its
  own dynamic resolution and FSR1 (now working, above); frame generation would cost power rather than save it.
- Its Android write tracking is the same mprotect scheme as `nvn_dirty.c`; the userfaultfd + PAGEMAP_SCAN path is
  desktop Linux only (6.7+).

## Stages

1. **Done: the CPU-visible half on the GPU.** Pools as above. Buffer copies and uniform updates are recorded into
   Vulkan command buffers in command order. Event values, counter reports and fences are applied by a completion
   thread only after the submission's fence signals, as on Metal. Work on addresses outside every pool
   becomes a "CPU step": the GPU finishes what came before, then the CPU does it. Draws are counted and skipped.
2. **Written, checked on lavapipe: shaders, textures, draws.** `vk_shader.c`, `vk_res.c` and `vk_draw.c` implement
   stages 2-4 below in a first, plain form (no queries yet, coarse barriers, every image in GENERAL layout).
   `test_vkdraw.c` draws through the whole backend (seed lookup by NVN key, block-linear upload, sampler, uniforms,
   vertex buffer, clear, Y orientation, re-upload after a guest write) and is clean under the Khronos validation
   layer. Missing variants are requested in `shader-requests/` and filled from the PC with `android/fill_shaders.py`.
   Frames can be saved as PNG with `TOTK_VK_SHOT=<every n-th frame>` (into `shots/`).

   The plan the code follows:

   **Shaders.** `tools/totk_xlate` already produces Vulkan SPIR-V (Ryujinx `TargetApi.Vulkan`); the Mac then
   converts it to MSL. Android stops at the SPIR-V and keeps the translator's reflection JSON for resource layouts. The
   translator is a .NET NativeAOT program. During development it runs on the PC and the device reaches it through
   `adb reverse`. For players, either publish it for `linux-bionic-arm64` or ship a pre-translated cache for the game's
   known shader list.
   **Textures and render targets.** `tex_t` → `VkImage` plus views. Block-linear deswizzle on upload (`gfx/tex`, then
   a compute version). BC is native on both drivers. Samplers come from the NVN sampler pool, with custom border
   colours. Texture copies (B2T, T2B, T2T), clears, and render targets as dynamic rendering. Textures in pools need the
   same "pool contents changed → re-upload" tracking the Metal backend has (`nvn_dirty.c`). On Linux it protects 64 KiB
   units (`TOTK_DIRTY_UNIT_KB`), because the per-process mapping limit (`vm.max_map_count`) is reached with single pages.
   `TOTK_VK_UPLOAD_THREADS=<1..3>` (default unset/0: serial on the calling thread) splits block-linear uploads of at
   least 512 KiB into runs of whole block rows (≥64 KiB) that 1–3 helper threads and the caller decode
   (`vk_upload_pool.c`); the copy is recorded after every run, byte-identical to the serial path (`uploadpooltest`,
   TSan). Helpers inherit the first uploading thread's nice and affinity. Every 300 presents `texture uploads: N (M MiB)
   in X ms, Y ms/frame; parallel deswizzles J` reports both arms; compare Y and the over-50 ms frame counts. Live
   (`totk.ctl`): each upload takes the current count before it starts (`uploadpooltest` decodes byte-identically while
   another thread reconfigures the helpers); at 0 the started helpers sleep without work.
   **Pipelines and draws.** Hash the NVN state into `VkPipeline`s, with dynamic state where the driver has it.
   Pipelines are created at their first draw; the `VkPipelineCache` is kept between runs in `TOTK_VK_PIPELINE_CACHE`
   (default `state/vk-pipeline-cache.bin`, `0` turns it off), checked against the driver's cache UUID when read and
   saved by a background thread. On the Thor the ~440 pipelines of the first world frames took 50–110 ms each without
   it and 33 ms in total with it. The log's `stutter:` line counts slow frames and pipeline time every 300 frames.
   Descriptor sets follow the Ryujinx layout, through push descriptors. Uniform snapshots at submit
   (`mtl_sub_capture_ubos`), because the game rewrites uniforms right after submitting. Vertex streams, index buffers,
   indirect draws, compute.
3. **Occlusion queries and conditional rendering** (opt-in samples). `TOTK_VK_OCCLUSION=1` enables precise samples-passed
   reports; default 0 remains everything visible. Render conditions count known pending reports (the clear-once pattern
   above); conditions on still-unknown sample reports stay enabled, without GPU conditional rendering.
4. **Picture: done (first form).** PNG frame dumps from `adb shell` (`TOTK_VK_SHOT`), and in the app ([APP.md](APP.md)) each
   presented frame is read back and written into the `ANativeWindow`. A zero-copy path (AHardwareBuffer + SurfaceControl) is later work.
5. **The APK's driver picker:** import driver zips like the emulators do (adrenotools-style namespace loading).

`nvn_mtl.m` stays the reference for NVN semantics. Most of its length is Metal-specific: tile memory, argument
buffers, MetalFX. Its comments record the game behaviours each piece exists for.

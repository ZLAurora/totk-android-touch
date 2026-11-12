# The Android app (TotK Native, `com.aure.totk`)

The app is one generic APK with nothing of the game inside (players: `docs/android-install.md`). It is built from the
repository alone by `android/build_release.py` (signed with the caller's key, audited by `android/apk/audit_apk.py`),
or as a debuggable developer build by `build_android.py --apk` (signed with `<work>/apk-key.pem`). Both pack the same
payload: `libtotk.so` (the runtime, `runtime/frontend_android.c` under the Java `TotkActivity`, a NativeActivity),
`libtotk_setup.so` + `libhactool.so` (the on-device setup, `android/setup`), `libtotk_xlate.so` (shader translator),
`libtotk_turnip.so` (the pinned Mesa Turnip, only in release builds or with `--turnip`), `libtotk_lsfg.so`,
`libdl_android.so`, `libc++_shared.so`, `classes.dex`, the app's own launcher icon (`android/app-assets/icon`), the
open fonts, the licence texts, `assets/shader_list.tsv` and `assets/pso_keys.bin`.

The launcher activity is `com.aure.totk.setup.SetupActivity` (process `:setup`, with the foreground service
`SetupService`, `foregroundServiceType="dataSync"`). It writes the game folder (`Android/data/com.aure.totk/files`:
`rewritten/v143`, `game-data/v143` with `runtime-tables.bin` / `romfs-config.txt` / `shared-fonts.txt`,
`extracted/v143/romfs`, `extracted/firmware/shared-fonts`, `shaders/v143/seed.vkpack`, `companion/`) and, last,
`install.json`. Once the base import (code, RomFS, index, fonts) is in place it shows the pre-game menu at every launch
(unless the player turned on "Open the game directly next time"; the launcher shortcut *Settings* always opens it) and
starts `TotkActivity` (not exported) from there, but only when `install.json` says `"playable": true` — which setup
writes once the shader seed is built too (without it nothing is drawn; a failed shader stage fails setup with Resume;
only the companion art is optional, and *Play without second-screen art* may skip it) — and `shaders/v143` holds a seed
with `.spv` modules (`InstallState.seedUsable`). Otherwise Home shows *Shaders not built* and offers *Build shaders*
(the shader stage alone) in place of *Start game*. Second-screen art from an older generator (its `companion/index.json`
`"generator"` below `CompanionArt.GENERATOR`, which equals `stage_companion.c` / `make_companion_assets.py`'s
`GENERATOR_VERSION`), or with an unreadable but present index, counts as not prepared: on devices with a built-in second
screen, Home opens the companion stage alone (once per launch) as *Updating second-screen art*; the old art stays in use
until the new tree replaces it, *Play now* stops the update and starts the game, and Game data shows *Out of date · Update*
(`tests/test_android_companion_art.py`). On other devices setup sends `skipCompanion: true`: native setup records
`stages.companion: {done:false, skipped:true}` (without a failure code) and runs shaders alone. Explicit Game data builds
still run on any device; automatic art updates and Home's art rebuild reminders are restricted to built-in second screens.
At every game start, including skip-menu launches, `ArtPolicy` uses the current display classification and actual
`companion/index.json` presence/currentness, not just the manifest's done flag. Missing art on a dual-screen device opens
the styled *Prepare second-screen art* / *Play without it* choice. Preparing runs the stage with progress and starts only
after success with usable art; cancellation/failure returns Home. `artDeclined` persists a skip across launches for this
install; a Game data build or deleting imported data clears it. `startAfterArt` preserves the requested build-then-play
through activity/process recreation. Pure-Java decisions are covered by `tests/test_android_art_policy.py`.

**App updates:** `TotkActivity` starts one background `Updates.check` per native game process, including skip-menu
launches. It requires active-network INTERNET + VALIDATED capabilities, a stable `X.Y.Z` versionName and the
default-On update switch. It performs exactly one HEAD to GitHub `releases/latest`, redirects disabled, no body read;
connect/read timeouts plus a five-second delivery deadline bound the check. Only an exact HTTPS github.com
`/AurelioB/totk-android/releases/tag/vX.Y.Z` Location newer by semantic version triggers a prompt. No telemetry, retry,
prerelease tags or automatic download. A private root-written source override enables test-package checks
([developer settings](../../../docs/developer-settings.md#private-update-source-for-device-tests)).
`UpdatePrompt` reuses menu fonts, panels and controller-focusable rows over the game. It explicitly holds
`nativeGameFocus(false, true)` while open, including on dual-screen devices; focus callbacks cannot release it.
The version is stored atomically in private `pending-update.txt`. Next time leaves it for a menu prompt before Home
on a later launch, without another request. An offer is suppressed if persistence fails. Update now enters a
non-exported `:setup` UpdateEntryActivity, which issues a short-lived one-use capability for the menu's immediate
update path. An external UPDATE intent to the exported menu can only show Update / Not now, never authorize closing
the game or downloading. `SetupActivity` reuses `needGameClosed` / `closeGame`, save-busy, GameControlService thaw and
close-request before `UpdateInstaller`.
The Updates setup step precedes Prepare; About has the same switch (`update-checks.txt`; absent means On).
The cancellable progress screen downloads APK + checksum into cache. Every redirect requires HTTPS github.com or
`*.githubusercontent.com`; the root-only test origin is the sole HTTP exception. One ten-minute deadline covers both
assets and every redirect. A separate progress watchdog disconnects blocked reads when an asset transfers less than
64 KiB in 30 seconds; redirects cannot restart either deadline. Failure removes partial downloads and keeps the
pending version. SHA-256, own package, strictly newer versionCode and the installed current signing certificate's
SHA-256 are checked. The exact bytes written to the PackageInstaller session are hashed again and must match before
commit. Unknown-sources permission is
explained and rechecked on return from Settings. Denial or installer failure keeps the pending version with a clear
note. A PackageInstaller session and explicit non-exported `:setup` UpdateReceiver handle Android's confirmation,
not a FileProvider. Temporary downloads are removed on completion/cancel/error or next menu startup after interruption.
Saves/game data are untouched. New permissions: INTERNET, ACCESS_NETWORK_STATE, REQUEST_INSTALL_PACKAGES.
Host coverage: `tests/test_android_updates.py`, `tests/test_android_update_security.py`.


All play paths converge on `SetupActivity.launchGame`: Start game/Play now, the launcher's direct-game preference,
the Settings `ACTION_VIEW` shortcut followed by Start game, and success after Prepare second-screen art.
`GameLaunch.start` uses the same `DisplayClass` built-in-panel classification as the companion, relative to
`Display.DEFAULT_DISPLAY`, not the menu's current panel. This also keeps art preparation and the Second screen
setting available when the menu is on the bottom screen. Only devices with a built-in second panel get
`FLAG_ACTIVITY_NEW_TASK` and `ActivityOptions.setLaunchDisplayId(DEFAULT_DISPLAY)`; single-screen devices and
external/cast/overlay-only configurations keep Android's ordinary launch behavior. The companion's enabled setting
does not change where the game runs.
SetupActivity declares `android:relinquishTaskIdentity="true"`: when the menu finishes, its existing same-affinity
task adopts the game's base Intent instead of permanently retaining the launcher/menu Intent. Without this,
Android 16 can fail to find the game across displays and create a second `TotkActivity`; neither
`moveTaskToFront` display options nor an explicit `AppTask.startActivity` fixes that stale identity. The native runtime
is process-global and must have exactly one NativeActivity. Game start does not replay/recreate the menu or change
its pending close/retry actions. `singleTask`, affinity, permissions, app label/icon and launcher/recents
declarations are otherwise unchanged; there is still one app card after the menu hands over to play.
On dual-screen devices a normal MAIN/LAUNCHER entry returns directly to a live game without another save sync or
startup marker; the explicit Settings VIEW shortcut always opens the menu. With no live game the normal menu/direct
game preference still applies. A pending save-safe Close game action is not cancelled by that launcher shortcut path.
An explicit Settings shortcut opened from the bottom panel binds its `SINGLE_TOP` launch to its own `AppTask`
before drawing on the main display: a normal cross-display task search cannot find a menu above the game-rooted
task. Existing menu content and gated retry actions stay on that activity; a pending Close game is not redirected
or replaced. A cold redirect initializes its recovery screen once, retaining Setup stopped / interrupted and Resume
instead of applying the already-open Settings-to-Home reset. Host coverage: `tests/test_android_settings_display.py`.
A newly created cross-display menu entry launches from the live menu's own activity token, explicitly moves its
cached `AppTask` to front, and finishes only the undrawn newcomer. A weak activity reference owns this routing, not
another persistent menu instance: Android 16 removes the old same-affinity task from `getAppTasks()` when it creates
the new launcher entry, so rediscovering it then is too late. The owner-bound `REORDER_TO_FRONT` launch has no
`NEW_TASK` flag, so it also reuses a menu below its file picker rather than matching the task's root Intent.
This includes launcher entry after native exit and preserves the existing menu's safe-close guard and retry.
If no menu exists and the game is live, Settings first returns through that singleTask game on display 0, then the
game opens the menu using its own caller token (no new-task flag). This keeps even a bottom-launcher Settings entry
above the game in the same task: after save-safe native exit there is no separate stale game task to reveal/restart.
Task-bound starts can report the new display configuration without delivering a new Intent; resume/configuration
callbacks therefore finish cold UI initialization, and the handoff Intent marks cold initialization to preserve
recovery regardless of callback order.
`TotkActivity` also checks resume, new-intent and configuration callbacks: a launcher can move an existing task
without opening the menu again. If it is on the second panel, the activity relaunches its own singleTask on the main
display, without finishing/restarting the native game; a pending-move guard prevents recursive launches. A display
move refreshes companion selection, which still excludes the game's current display. Host decision/callback
coverage: `tests/test_android_game_display.py`; Android task-manager behavior is checked separately on devices.
Configuration recovery runs only while the game is resumed: moving a shared task with the menu on top must not
bring the background game over that menu. The next Start game still targets the main display.

Device acceptance (2026-11-04, release-key TEST build `ad834eab3d20`): Retroid Pocket Duo / Android 16 (secondary
display 2) and AYN Thor / Android 13 (secondary display 4). Cold bottom-menu launches, top launches, Home + launcher
from either panel, a live game task moved to the bottom panel, and a shared menu/game task moved there all returned
the game to display 0 and the companion to the built-in second panel. The shared-task returns retained the same task,
ActivityRecord and native PID (Duo task 629 / PID 8549; Thor task 28543 / PID 22586); every scenario's activity dump had
at most one TotkActivity. The Settings VIEW shortcut from the bottom opened one visible menu on display 0; normal
launcher taps returned to the running game without a menu. Recents retained one TEST app card with the app icon.
Home logged game/audio pause at a guest frame boundary. A gated graphics-driver action survived Close the game,
logged the native pipeline-cache flush/exit, and continued into the file picker; cancelling it changed no driver.
After game exit, the next normal launcher tap showed the menu. Both devices' existing SELinux mode was Permissive
and was left unchanged; these are display/task/lifecycle checks, not new enforcing-mode compatibility evidence.

Follow-up Settings/task acceptance (2026-11-04, release-key TEST build `bcd059d`): the same Duo / Android 16 and
Thor / Android 13 completed 42 scenario captures with at most one game and one menu ActivityRecord each. A shared
game/menu task moved from display 2/4 back to 0 retained both records and the native PID (Duo task 690 / PID 27784;
Thor task 28587 / PID 20559), including a gated graphics-driver retry. Close flushed/exited the native runtime and
continued that retry into the file picker. Cold bottom Settings retained Setup stopped / interrupted with Resume.
Both launchers returned a live game without a menu; after native exit they reopened the menu without restarting
the runtime. Settings also foregrounded that same menu above its file picker. Dumps checked the menu's RESUMED state,
not just its existence. Recents retained one TEST task; affinity and the singleTask game remained unchanged.

The menu keeps its options in SharedPreferences and writes them, on every
change and again right before the game starts, to `<private>/settings.env` (`Settings.java` is the option → variable
mapping; `tests/test_android_settings.py`). The native side refuses to start without a playable install too and
finishes the activity. The runtime reads its module tables and RomFS / font hashes
from `TOTK_GAME_DATA` (default `<game>/game-data/v143`) and, being built for 1.4.3, refuses values other than the pinned
ones (`assets/expected-v1.4.3.json`). Vulkan driver: `<game>/driver.so` (copied into private storage) or
`TOTK_VK_DRIVER`, else the bundled `libtotk_turnip.so` used in place from the native library folder, else the system
driver. A `shaders/v143/seed.vkpack` selects the indexed seed IO (`TOTK_VK_SEED_IO=1`). Developer builds fill the same
folder over USB with `install_app.sh data` (install.json goes last). The rest of this file is the development history.

*Game data → Graphics driver* accepts a bare ELF64 AArch64 `.so` or an AdrenoTools Turnip `.zip`, detected by content.
`DriverImport.java` stages a bounded archive privately, reads `meta.json` (up to 64 KiB) and selects its exact
`libraryName` entry (including nested paths); without metadata exactly one `.so` is required. Other entries are
never extracted. Archives are limited to 160 MiB / 256 entries, libraries to 128 MiB; the library's ELF header,
uncompressed length and CRC are checked before `driver.so.tmp` replaces `driver.so` under `GameLock`.
The directory is bounded before `ZipFile` opens it; multiple EOCD signatures in the scanned footer are refused so a
platform parser cannot select a different, unchecked directory hidden in a zip comment.
Failed reads leave the previous driver and `driverName` preference intact and remove staging files.
The row uses metadata `name` plus `driverVersion`, or the picked filename; *Use the bundled driver* removes the override.
Host coverage: `tests/test_android_drivers.py`.

### Setup stopped: Share details

The controller-focusable **Share details** row leaves **Resume** primary. It builds a plain-text report on the
setup worker, then opens Android's `ACTION_SEND` chooser (`text/plain`, no FileProvider or attachments).
No game files or keys are included; nothing is sent until you choose an app.
Share details **before Resume**: `setup.c` clears `.import` when the next run starts and removes it on success.
Failure cleanup keeps the top-level logs, even for private Internal storage that a file picker cannot browse.

`SetupDetails.java` is the pure, host-tested builder (`tests/test_android_setup_details.py`). Its only file reads
are `.import/hactool-romfs.log`, `.import/hactool-exefs.log`, and, for a shader failure, `.import/shaders.log`.
Missing/unreadable files are marked; symlinks and non-regular files are excluded. The UTF-8 report is capped at
64 KiB, retaining each oversized log's beginning and end with a byte-omission marker. Partial records at byte cuts
are dropped; an oversized edge line is scrubbed whole before it is truncated, so filename/URI/path fragments cannot
escape redaction. Hex runs of 32 or more characters are redacted again across the whole report; input names,
URIs and absolute paths are scrubbed.
Metadata includes the result JSON, app version/code/commit, device/Android/SoC, route, destination kind and free
bytes, plus each input's direct-path/provider-authority access, extension and size only.

### Supplying your own launcher image

Add `--launcher-icon /absolute/path/outside/repo/logo.png` to `build_android.py ... --apk` (also supported with
`--runtime-only`), or set `TOTK_LAUNCHER_ICON=/absolute/path/outside/repo/logo.png` in the build environment.
The explicit argument takes precedence over the environment variable. The standalone `android/apk/build_apk.py`
accepts the same option and variable. JPEG and single-frame PNG are accepted; this override requires **Pillow**
in the APK builder's Python environment. Invalid or missing images fail packaging instead of silently selecting
the extracted icon. The build log records the resolved source path and its complete SHA-256.

Only the icon changes; the English game title still comes from the update/base Control NCA. Without the option
or environment variable, the existing control-icon extraction, JPEG fallback and missing-artwork behavior are
unchanged. The supplied image and every generated pixel file must stay **outside the repository**; never add
Nintendo artwork or generated icon fixtures to git.

The override fits the **whole** image, without cropping or stretching, on a square canvas using Lanczos
resampling and light small-icon sharpening. Transparent PNGs retain their alpha/glow, resized in premultiplied
alpha, against a dark teal (`#0b1416`) background. Opaque images use a solid sampled from their four corners
(white for white-background artwork). Generated outputs:

| Density | Legacy `ic_launcher.png` and round `ic_launcher_round.png` | Adaptive background/foreground PNGs (108 dp) |
|---|---:|---:|
| mdpi | 48 × 48 | 108 × 108 |
| hdpi | 72 × 72 | 162 × 162 |
| xhdpi | 96 × 96 | 216 × 216 |
| xxhdpi | 144 × 144 | 324 × 324 |
| xxxhdpi | 192 × 192 | 432 × 432 |

`android:icon` and `android:roundIcon` reference the mipmaps. On API 26+, the two `mipmap-anydpi-v26` adaptive-icon
XML resources select solid-background and transparent-foreground layers; the entire foreground image fits inside
the **66 dp safe circle** of the 108 dp layer, including its corners. Round legacy icons similarly fit the
whole image inside their circular mask. A separate **512 × 512** store PNG is written next to the output APK
as `<apk-stem>-launcher-512.png` (for `totk.apk`, `totk-launcher-512.png`); it is not added to the APK.

## Device profiles

One branch and one universal APK serve AYN Thor, AYN Odin 3, Retroid Pocket Duo and other compatible Android
devices. There is no device-name/SoC performance-profile switch; the driver correctness quirk below uses a raw
GPU chip id. `integ/universal5` keeps the conservative device profiles and the shipping render-only ADPF/FXAA
defaults. It enables the aligned scene-size classifier and packaged PSO keylist/prewarm path; the keylist is
generated from `games/totk/assets/pso_keys.tsv` into the APK binary asset. Full ADPF/display hints, presentation
timeline corrections, `TOTK_DRS_FRAMEGEN`, and scene SMAA remain opt-in. `totk.env` is optional; explicit
settings win over the same clean-install defaults everywhere: shipping **30-fps mode**, capped, `FPS60=0`,
`FPS_AUTO=0`, native UI and frame generation off, protected frame lag off, two recorder workers, graphics
keylist prewarm on when the packaged asset is present, and **hardware GPU time for Android shipping-30**.
FPS60 mode otherwise retains latency GPU reports unless explicitly overridden. Diagnostics default off
(the development build's ordinary overlay is separate). Existing Thor/Odin and Duo device-specific settings
below remain explicit operator profiles; no profile changes CPU/GPU caps, fan policy, ClusterTune or the user's
AYN/Retroid performance mode. On Adreno 722 only, an absent `TU_DEBUG` defaults to `sysmem` under the exact
guard below. The guest-extent cache defaults on but retains its cache/live-query A/B control.

**Optional LSFG profile (all devices, including Odin 3):** select
`TOTK_FRAMEGEN_ENGINE=lsfg` only with the locked timing block below and your private
user-owned DLL. `TOTK_LSFG_PERF=0` (normal model) and `TOTK_LSFG_FLOW=0.5` (half
width/height optical flow) are the universal defaults; `PERF=1` selects the lighter
model, and `FLOW=0.75` or `1.0` increases flow resolution, not output resolution.
There is no device branching or automatic performance-mode selection. The shared
device implementation removes the old AHB engine transport; it does not change
the presenter's normal zero-copy AHB surface. Device measurement, not model-only
GPU time, decides whether an optional profile sustains the on-screen cadence.

Odin3 locked village MOVE qualification of the shared-device implementation found
normal/flow0.5 at37.47Hz (40.21% tight), performance/flow0.5 at58.99Hz
(98.45% tight; model GPU1.91ms), and camera at59.77Hz (99.92% tight), using
SurfaceFlinger actual-present endpoints on the game's `TOTK zero-copy` layer.
Both LSFG models still showed whole-scene double exposure in consecutive visual
samples, also present in the inherited separate-device baseline. Therefore no
LSFG model is promoted to the play profile or chosen by device name; the normal
model default is compatibility policy, not an Odin performance recommendation.
`PERF=1 FLOW=0.5` is the best measured experimental LSFG combination, **not a
visual-quality pass or a59.8Hz claim**. Use the camera/play profile until that
integration failure is resolved. Model GPU time and submitted/generated counters
alone are not on-screen cadence authority.

`TOTK_LSFG_WIDE_BARRIERS=1` is an off-by-default diagnostic matching the mask
replacement reported in [Mesa issue 16406](https://gitlab.freedesktop.org/mesa/mesa/-/work_items/16406).
For existing backend `BarrierBuilder` image barriers, both source and destination
stages change from `COMPUTE_SHADER` to `ALL_COMMANDS`; access changes preserve the
direction: read-to-write uses `MEMORY_READ` → `MEMORY_WRITE`, and write-to-read uses
`MEMORY_WRITE` → `MEMORY_READ`, replacing the corresponding `SHADER_READ`/`SHADER_WRITE`
masks. Image handles, old/new layouts, queue families and subresource ranges are
unchanged; no extra barriers are inserted. Only the exact value `1` enables it
(unset, empty, `0` and other values keep the original masks). The choice is cached
on first use, so set it before starting the process and restart between arms.

The upstream author reports corruption in a bindless pipeline with memory reuse,
but not the older fixed-descriptor/no-reuse pipeline; both stage **and** access
mask widening were needed. `TU_DEBUG=flushall` reportedly helped, while `syncdraw`
and waits between command buffers did not. Our backend instead uses per-pass
descriptor sets and separately allocated image memory, so this is a targeted
comparison, not evidence that the upstream defect explains our visual failure.

`TOTK_LSFG_FULL_BARRIERS=1` remains a separate, off-by-default diagnostic: a global
`ALL_COMMANDS`/`MEMORY_READ | MEMORY_WRITE` barrier surrounds every backend dispatch,
including intervening image transitions. Unlike `WIDE_BARRIERS`, it adds barriers
and does not replace the existing image-barrier masks. Neither switch enables the
other; test wide-only with full unset or `0` to isolate the upstream workaround.
These over-synchronizing arms are not promoted to a play profile, and their timings
are not the ordinary profile's cost model.

`TOTK_LSFG_CLEAR_INTERNAL=1` additionally clears each newly allocated backend-owned
image to zero and makes the transfer visible before shader access. Borrowed game
images are never cleared. This startup-only diagnostic separates undefined
intermediate contents from synchronization/model quality; default off.

**Thor:** defaults; optional `TOTK_UNCAPPED=0` merely makes the capped default explicit (the existing play
environment contains that one line). The detected clearly smaller CPU tier is excluded from automatic guest
placement (normally guests `3-7`); seed/cache IO workers prefer that smaller tier (normally `0-2`) at nice +10.
These are topology decisions, not fixed CPU numbers.

**Retroid Pocket Duo (Adreno 722), plain play:** approved correctness-first near-30 profile, measured with
universal1b and the original cs-slabs Turnip, no frame generation or interpolation:

```ini
TOTK_UNCAPPED=0
TU_DEBUG=sysmem
TOTK_RENDER_RES=960x540
```

The default GMEM path reproduced transient black scene rectangles/vertical bands in village, sky and
rain captures on current and upstream Turnip. `sysmem` was the only qualified isolation setting with no
observed such corruption in three 25-second motion clips: zero whole-motion blackout candidates over
622/551/540 comparable decoded frames. This is bounded evidence, not proof for every game area.
Disabling UBWC, LRZ or command-stream slabs, and either tested MR45615 driver variant, did not fix it.
The Qualcomm system driver still fails the backend's shared-pool capability gate; the copy presenter
cannot bypass that requirement.

At user-selected **Medium** (SystemUI tile **Performance**), native scene size with sysmem measured
26.43 fps / 26.67% long fence intervals. At the same village pad, 1152x648 reached 27.65 fps / 16.45%;
a clean 960x540 repeat reached **29.63 fps / 33.75 ms / 2.47% long**, GPU 26.33 ms, walker CPU 18.07 ms,
3812 draws/frame, 82.0% GPU busy at 975 MHz. 648p had 3808 draws/frame and 28.97 ms GPU time.
540p is visibly softer/jaggier at roof edges, Link and fine foliage; HUD text remains readable.
The earlier interrupted 540p run is excluded. These are stationary village results, not a steady-30
guarantee during walking or in heavier scenes; native sysmem sky/rain measured 21.06/24.27 fps.

Standard mode exposed CPU caps 1113.6/1708.8/1766.4 MHz and KGSL GPU levels 724–796 MHz despite a
1150-MHz advertised maximum. The user's Medium selection changed these to 1497.6/2073.6/2150.4 MHz
and 900–975 MHz. Read-only KGSL thermal indicators remained zero. The app/worker changes none of
these policies, nor performance mode, fan, saves or governor. Higher-mode mappings are not assumed.

The user later selected mode 2: CPU caps 1843.2/2400/2803.2 MHz, KGSL levels 0/1 (1150/1075 MHz).
At sampled constant 1150 MHz, 540p+sysmem measured **29.82 fps / 0.38% long**, versus 648p 28.11 / 12.95%
and native 27.48 / 18.10%, at similar ~3815 draws/frame. GPU temperatures were about 70–73.5 C, CPU
74–77.5 C; the board proxy rose about 60→63 C. KGSL thermal indicator stayed zero in this finite sequence,
not proof against longer-run throttling. The gain over Medium 540p is small (29.63→29.82 fps), while USB
input rose about 5.00→6.14 W with a full battery; that is not a measured system-power efficiency ratio.
Retain the user's choice rather than selecting this higher mode automatically.

The High-mode village screen below uses capped 30, `sysmem`, the same travel pad, warmed shader cache
and ~3800 draws/frame. **Long** means present-fence intervals in the >=37.5 ms bins, not all frames
above exactly 33.3 ms. FPS is the arithmetic mean of retained 300-frame game windows, not a hardware
scanout guarantee. The baseline retained about 151 s; 648p/native about 150/142 s; the five option runs
retained about 145–155 s each (13–15 windows). Nominal 90 s sampling was extended by the power sampler.

| High-mode case | FPS | Long % | GPU ms/frame |
| --- | ---: | ---: | ---: |
| 960x540, plain | 29.82 | 0.38 | 25.44 |
| 1152x648, plain | 28.11 | 12.95 | 28.24 |
| Native scene, plain | 27.48 | 18.10 | 28.98 |
| 960x540 + shadow288 | 29.84 | 0.20 | 24.57 |
| 960x540 + whole-canvas SGSR | 29.37 | 4.67 | 27.23 |
| 960x540 + whole-canvas FSR | 29.07 | 7.78 | 30.00 |
| 960x540 + scene SGSR/native UI | 28.48 | 10.36 | 27.40 |
| 960x540 + scene FSR/native UI | 26.92 | 22.05 | 29.15 |

Shadow288 is **optional, not default**: `TOTK_SHADOW_RES=288` reduced GPU time about 0.87 ms but made
only a tiny cadence difference in this view, and reduces shadow detail. Whole-canvas SGSR uses
`TOTK_POST_SCALE=sgsr`, `TOTK_POST_SGSR_SHARPNESS=2`; FSR uses `TOTK_POST_SCALE=fsr`,
`TOTK_POST_SHARPNESS=0.2`. Both work with universal1b and filter the already-composed 720p canvas,
including HUD, into the 1080p window. They sharpen roof/Link edges but do not recover lost 540p detail;
sharpened foliage/contours can look harsh. SGSR is the least costly optional upscale, not the near-30 default.
The scene-upscale2 APK (`84c2168969f92342`) uses `TOTK_NATIVE_UI=1` plus `TOTK_SCENE_SCALE=sgsr`
and SGSR sharpness2, or `TOTK_SCENE_SCALE=fsr` plus `TOTK_SCENE_SHARPNESS=0.2`. Logs confirm the
pre-UI 1280x720 canvas -> 1920x1080 upscale then native HUD (the residual 540p scene -> canvas filter remains).
Its HUD is crisp, but neither scene scaler earns its FPS/cadence cost for this device's near-30 objective.
These are still-image quality comparisons, not temporal upscaler-stability qualification. Retain the plain
three-line profile for the near-30 objective; universal5 includes both scene scalers without requiring them.
FXAA off/far clamp12000 had no useful earlier-screen gain.

The integrated A722 policy defaults `TU_DEBUG=sysmem` immediately after `totk.env` is parsed on the main
thread, before input/thread diagnostics, seed IO, game workers or any driver call. It requires an existing
operator-selected custom-driver source, exact read-only KGSL chip id `0x43020100` and absent `TU_DEBUG`.
Explicit values, including empty or `0`, win; missing custom files, failed probes, other GPUs and explicit
system-driver selection do not enable it. The same path resolver selects the later private driver copy.
This is a GPU-id correctness quirk, not a Retroid performance-mode selector. The earlier standalone
automatic-default pan/walk clip had zero blackout candidates over642 comparable frames; early startup ordering
is a separate host-tested correctness fix, not a new device qualification. Explicit sysmem remains useful with
universal1b. Later Duo cache-reload DEVICE_LOST failures also occurred with explicit sysmem and universal1b
using the same accumulated cache. Three controlled universal3a automatic-default starts from a good cache
reached gameplay (~29.7fps; zero detector candidates over696/690/683 frames); the first added one VS/FS pair,
then the cache stayed byte-identical. This does not implicate universal3a as a cache poisoner. The separate
host-tested context envelope below conservatively separates driver binaries and debug configurations;
neither that segregation nor early ordering is a device-qualified cure for the earlier accumulated-cache failure.

Final High-mode sustained check, universal1b with the three-line profile: 91 retained 300-frame windows
(about **924.9 s**), **29.54 fps / 33.85 ms / 1.48% long**, 258 scripted key-downs and no real-controller
activity after the boundary. Thirty-second snapshots kept CPU clocks at 2400/2803.2 MHz and GPU at
1075–1150 MHz, with KGSL thermal indicator zero. GPU temperatures were 59–68.7 C, CPU 66.2–71.9 C;
the full-run sys-therm-0 board proxy rose 54.8→58.6 C. Separate live thermal-HAL **skin** samples,
covering elapsed 142–892 s at 30 s intervals, rose 55.997→57.743 C (status 3); do not call the board
proxy a physical skin sensor. No clock/thermal collapse was observed on this route.
**Limitation:** repeated pans/side steps drifted Link onto the nearby hillside and ended climbing, not a
constant dense-village view: draws ranged 1514–3875/frame (mean 2433), GPU busy 29–83%. The soak is
varied-load near-village evidence, not a maximum-load village thermal guarantee or globally steady 30.
Final readback retained universal1b, the exact three-line profile and original driver; slot/album/option
hashes matched the fresh user manifest (only CacheStorageKey.dat changed), no held slots remained,
and both display controllers reported OFF after normal sleep.

**Odin 3 ordinary play:** the menu's Snapdragon 8 Elite-class (`CQ8725S` / `SM8750`) 60 fps profile generates
`TOTK_FPS60_HYSTERESIS=1` and `TOTK_FRAME_LAG=1` only at native 60 fps with frame generation off, including
first run, existing stores and custom 60 fps mixes. Keep GPU busy retains its independent chip default.
Developer `totk.env` is read afterward: explicit `TOTK_FRAME_LAG=0` disables lag. Other chips, 30/40/Unlimited
and frame generation retain their prior settings. Do not add a chunk override: resolved lag automatically uses
600 draws when native VMA pressure is absent (otherwise 75), preserving explicit developer overrides.
Capped cross-device tests improved Odin static 57.1→59.8 / 59.2→59.9 fps and pan 58.4→59.5 / 58.2→59.5;
33 ms counts fell 20/1800→1/1800 static and 148/5100→31/5400 pan. The descriptive pan p99 tail rose
23.3→27.8 ms: throughput/cadence gains are not a tail or input-responsiveness guarantee. Duo tails worsened
and capped Thor was neutral, so neither gains the default. Additional ordinary-play opt-ins remain:

```ini
TOTK_VK_REC_THREADS=2
TOTK_FPS60=1
TOTK_UNCAPPED=0
TOTK_GPU_TIME=hardware
TOTK_NATIVE_UI=1
```

Odin's all-big topology keeps the inherited guest/background-worker affinity mask unchanged. Its intentional
CPU frequency caps and AYN performance mode are user policy, not inputs to the tier detector. Native UI requires
the v1.4.3 game layout; unsupported versions/invalid native-UI values fail explicitly rather than guessing.

**Odin 3 opt-in locked real30 presentation correction (not the ordinary native60 profile):**
the separately requested API-timeline mode can use the following complete timing block. Remove
the whole block to return to the ordinary policy. Timeline, timestamp-floor, API-pair dedupe
and timestamp-guard options are all independently default-off; this is a qualification recipe,
not a universal default. Do not combine it with hybrid, a fixed generation budget or the ordinary
profile's `FPS60=1`.

```ini
TOTK_UNCAPPED=0
TOTK_FPS60=0
TOTK_FRAME_LAG=0
TOTK_VK_REC_THREADS=2
TOTK_FPS_AUTO=0
TOTK_REALTIME=1
DM_DETCLOCK=0
DM_PRESENT_HZ=60
DM_PRESENT_INTERVAL=game
TOTK_PRESENT_SLOTS=1
TOTK_VK_PRESENT=ahb
TOTK_ACQUIRE_SLACK=0
TOTK_ACQUIRE_CLOCK=0
TOTK_PACE=0
TOTK_ASYNC_REPLAY=0
TOTK_MFX=0
TOTK_FRAMEGEN=locked
TOTK_FRAMEGEN_ENGINE=camera
TOTK_FRAMEGEN_LEAD_REFRESHES=1
TOTK_PRESENT_TIMELINE=1
TOTK_PRESENT_FLOOR_NS=1
TOTK_PRESENT_DEDUPE=1
TOTK_PRESENT_GUARD_NS=0
TOTK_PRESENT_CADENCE_TRACE=0
```

In the protected Odin docked1920×1080 final comparison, this combination delivered **59.9010 /
59.9006 displayed layer buffers/s**, with zero late/dropped app SurfaceFrames in both 15-second
root trace windows and 100.000 /99.979% tight intervals in independent route100–180s measurements.
The guard-off recommendation is deliberate: the separately tested 125μs guard added no demonstrated
benefit. Legacy controls spanned **35.8–59.8Hz**; Thor was neutral and did not exercise API dedupe.
Distinct-refresh admission increased intentional reservation/image age by roughly **10–15ms**
versus floor-only; this is **not measured input-to-photon latency**. Simulation/input remain30Hz.
No extra buffer, device-name switch, cap/governor/thermal/OEM-mode change or universal default
promotion is implied. This qualifies measured static-scene cadence, not moving-image generated
frame identity or optical quality. API admission remains limited to the internal logical display0.


**Docked (`TOTK_OPERATION_MODE=docked`):** unset/empty `TOTK_APP_WIDTH` becomes process-only effective `guest`
whether `TOTK_NATIVE_UI` is unset, `0`, or `1`. Both front-buffer dimensions follow the current guest canvas/crop,
aspect-fit without upsampling and capped by the live render-window extent; ordinary docked play no longer
downscales a shipping 1920×1080 canvas through the handheld 1280×720 default.
If `TOTK_NATIVE_UI=1` was requested, startup also logs `requested=1 effective=0`: the shipping docked canvas
already has native UI and must not require a handheld promoted sibling. Only in that requested-native-UI path,
unset/empty/`auto` `TOTK_OUTPUT_RES` becomes effective `0` (shipping canvas, no larger output heap);
explicit `0` or dimensions remain authoritative. A smaller/non-16:9 panel is fitted and
letterboxed; no fixed Odin/Thor width is assumed. AHB uses the fitted buffer and centered destination; copy
fallback paints its bars into a full window-sized buffer. A missing/zero window defers work; subsequent
resize/recreation uses an event-refreshed extent, not a fixed startup size or per-frame native-window polling.
Create/recreate, resize/redraw, content/configuration and focus/resume notifications refresh the atomic width/height
cache. Guest canvas/crop changes refresh the cached aspect-fit geometry; steady real/generated frames reuse it.
Attachment epochs reject stale callbacks, and a changed guest-window extent advances the queued-frame generation
so old copies are canceled before geometry publication. Each queued job retains its own coherent cached fit.
Every explicit nonempty `TOTK_APP_WIDTH` override retains its prior behavior. `totk.env` is never rewritten.
Handheld requested-native-UI behavior, companion enablement and launch-display routing are unchanged:
the policy uses whichever render window the existing routing selects, not a device name or display number.
The equivalent explicit profile for older APKs is `TOTK_NATIVE_UI=0 TOTK_APP_WIDTH=1920` on a1920x1080 panel;
that numeric profile is not a universal automatic sizing rule.

**Guest-extent cache experiment.** The cached path defaults on; `TOTK_GUEST_EXTENT_CACHE=0` explicitly selects
the prior per-frame live-query/fit path for a matched A/B (lifecycle epoch protection remains common to both arms).
It affects only `TOTK_APP_WIDTH=guest`; numeric width/native-UI presentation and companion/launch-display routing
are not changed. Treat performance as **unproven until the Odin A/B**, not an established fix.
With `TOTK_FRAMEGEN_DIAG=1`, `[guest-extent]` reports every 300 game presents, separately for producer/presenter:
`cache`, `query_pairs`, `native_calls` (two per width/height pair), `total_ms`, and `max_us` of the query wall time.
The counters are approximate concurrent reporting windows. One-time AHB surface creation can contribute a presenter
pair; cached steady-state windows should report zero. Clocks and counter updates are diagnostic-only.
Compare the same warmed save/camera/locked-mode settings in cache `1` and `0`, then explicit `TOTK_APP_WIDTH=1920`;
confirm identical AHB extent/allocation counts and report cadence, tight/held/no-buffer outcomes plus query timing.

Host acceptance: the 97-process draw/overlay subset, both startup arms and TSan cache/live-query cases passed;
Python passed 204 tests. The 600-frame host scenarios observed zero steady-frame native-window queries with
cache `1`, versus four calls per posted real frame with cache `0`; cached fit recomputation occurred only on
lifecycle or canvas/crop extent changes. These are mock-window call-count and lavapipe pixel checks, **not**
Android AHB or Odin performance measurements. Logs: `.scratch/overlay-res3/`.

Optional **Odin hybrid** adds the following lines to ordinary play; remove the whole block to return to plain
play. It is not a universal default or a promise of 60-fps scanout. Leave `TOTK_FRAMEGEN_BUDGET_MS` unset.

```ini
TOTK_FRAME_LAG=1
TOTK_FPS_AUTO=0
TOTK_REALTIME=1
DM_DETCLOCK=0
DM_PRESENT_HZ=60
DM_PRESENT_INTERVAL=game
TOTK_PRESENT_SLOTS=1
TOTK_VK_PRESENT=ahb
TOTK_ACQUIRE_SLACK=0
TOTK_ACQUIRE_CLOCK=0
TOTK_PACE=0
TOTK_ASYNC_REPLAY=0
TOTK_MFX=0
TOTK_FRAMEGEN=hybrid
```

### Experimental adaptive QCOM generation (default off)

`TOTK_FRAMEGEN=adaptive` is a distinct opt-in mode, not a change to locked,
hybrid, `interp`, or camera settings. It requires v1.4.3, `TOTK_FPS60=1`,
`TOTK_REALTIME=1`, `TOTK_NATIVE_UI=1`, `TOTK_PRESENT_SLOTS=1`, and
`TOTK_FRAMEGEN_ENGINE=qcom`. Keep `TOTK_UNCAPPED=0`, `TOTK_FPS_AUTO=0`,
`TOTK_ASYNC_REPLAY=0`, `TOTK_ACQUIRE_SLACK=0`, `TOTK_ACQUIRE_CLOCK=0`,
`TOTK_PACE=0`, `TOTK_MFX=0`, `DM_DETCLOCK=0`, `DM_PRESENT_INTERVAL=game`;
use `DM_PRESENT_HZ=60` for Thor's 60-Hz panel. This is a qualification option,
not an approved ordinary-play profile.

**Safety admission:** adaptive AFME is restricted to the already qualified
AYN Thor identity (`AYN Thor` / `kalama`, Qualcomm, Turnip Adreno740). Other
devices/drivers are refused before EGL/AFME initialization. In particular the
Odin AFME path has produced UCHE faults and must not be probed by enabling
adaptive. Refusal is real-only, never a silent switch to camera generation.
`TOTK_FRAMEGEN_ENGINE=lsfg` is hard-disabled in adaptive until the separate
whole-scene doubling defect has a qualified repair. Its fractional backend API
does not authorize adaptive LSFG playback.

FPS60 patches lower the guest controller minimum; adaptive overrides only host
acquisition pacing: one refresh at60Hz, two at120Hz, plus a strict minimum
16,666,667ns between actual acquire releases after texture/GPU waits. It keeps
that release clock across resets/stalls/refresh changes, so spare GPU headroom
cannot cause a catch-up burst. Real frames are capped at min(display,60) until
the guest fade/LOD high-rate sites are separately qualified; safety remains
provisional until device cadence logs confirm it. The simulation otherwise runs
at achieved real throughput, not fixed30. REALTIME uses measured dt (invalid
outside1–250ms, each accepted interval capped at1/15s, five-sample mean).
Adaptive captures the effective **post-selector** guest producer step (including
the intentional half-step), consumes it once at guest present, and accumulates
that simulation time before the next step is published. GPU completion is
readiness, not simulation time. Hybrid and other modes' gates are unchanged.

The newest real's simulated time is mapped to its fixed real presentation slot.
For a later uncovered slot, QCOM predicts
`scale=(target_sim-last_sim)/(last_sim-prev_sim)`, only in `(0,1]`.
Let `dS=last_sim-prev_sim`, `dH=last_guest_time-prev_guest_time` and `D` be the
target's distance from the newest real's immutable display slot. The local
game-clock map is `target_sim=last_sim+D*dS/dH`; thus scale algebraically equals
`D/dH`, while the actual simulation stamps/epoch remain required authority.
Half-step motion therefore halves predicted simulated advance, not real-image
arrival rate. The horizon is one source interval (`D<=dH`); later targets are
omitted, not assigned a mistimed clamped picture. No interpolation interval is
inserted. Retained real HUD/UI is composed over the generated scene. A ready
real wins before actual transaction handoff, including while prior transactions
drain; predictions wait until2ms before their target before final arbitration.
Canceling presentation never retires GPU/source/replay-pool ownership early.

Generation suspends below25 real fps (latest simulation or guest-present
interval >40ms), on invalid/pinned time, unsupported clocks, loading/low-draw
menus, pipeline settling, scene/camera cuts or backend refusal. Two new eligible
sources are needed after invalidation; at/above display rate it passes through.
Near display rate, short gaps may leave no useful prediction opportunity and
must not force real-frame delay. Existing eligibility/fallback and measured
total-GPU budgeting remain conservative refusal gates.
An independent counting planner prevents asking for more outputs than the
display budget:12 guest-interval samples, source EMA for the output ceiling,
0.5s continuous demand before raising that ceiling and three lower reads before
reducing it. Exact fractional nanosecond credit carries only the fractional
refresh remainder; capped/failed grants are forfeited, never burst later.
Counts do not quantize the simulation phase. This is an original implementation
of the display-cap/hysteresis and fractional-credit ideas surveyed in WinNative
WFG (MIT) and GameNative (GPL, idea only); no external source code was copied.

Adaptive timing/deadline misses reject the affected output and impose bounded
real-tick cooldowns before re-probing; they do not permanently disable QCOM.
Capability/import/fence/corruption refusals remain permanent, and the Thor-only
pre-initialization gate is unchanged. Each fractional output's own immutable
display target is its deadline: both source and output ticket publications
must precede it, regardless of the newest real's earlier slot or nominal FPS.
Locked/hybrid/camera timing-policy decisions are unchanged.

`[adaptive-sim]` separately reports actual real-game frames/s and effective
per-frame simulation-dt mean/min/max/histogram, including missing captures.
`[adaptive]` reports generation fraction, phase bins and refusal/miss/real-won
counters; generated, real-simulation and presented cadence are not conflated.
These are app-slot diagnostics, not proof of physical60Hz. Qualification must
use exact game-layer SurfaceFlinger timestamps and manual real/generated visual
review, including a55–59fps near-display scene. LSFG's eventual interpolation
contract is different: bracketed `t` in `[0,1]` with one variable real interval
of presentation delay; unbracketed targets must remain real-only.

`TOTK_FRAMEGEN_JOB_TRACE=1` enables default-off, logging-only per-generated-job
submit-request, presenter-pickup, source/output ticket-ready, target and decision
timestamps in both locked and adaptive modes. Ready timestamps are CPU
publication after GPU completion and CPU posts, not hardware GPU signal time.
When both precede the target, a later decision proves presenter/drain lateness;
a late publication is explicitly `GPU-only cause unknown`. Instrumented ABBA
measurements must be labelled instrumented; host formatter overhead is not
Thor/logcat impact. A separately approved trace-off confirmation is required
before uninstrumented production pacing/throughput claims.
The same trace flag logs each ordered-authority begin failure's precise guard,
current/authority/history/reset state and exact depth identity, followed by four
source pin/history/retirement observations. `[framegen-authority-begin-counts]`
reports per-reason last300 and lifetime FAILED-CALL counts, not probe-skipped
frames. `[framegen-authority-source] retired` is1/0 for published CPU-complete/
pending finite tickets, -1 unsealed, -2 missing ticket, -3 uninitialized.
These are nonblocking done-counter observations, not hardware timestamps or a
coherent multi-source snapshot. Tracing never acquires/recycles a source,
changes eligibility, waits for completion or makes an unavailable before-copy
valid. Both worker selectors stay OFF for the authority diagnostic pair.

`TOTK_FG_ADAPTIVE_BACKPRESSURE=request` is a separate default-off causal
experiment. Unset, `serial`, or invalid values keep adaptive's serial handoff.
Exact `request` admits adaptive to locked's existing API31+symbol-gated
backpressure request and at most two future transactions; absent capability
still falls back to serial. Locked selection and its policy are unchanged.
No wake lead, wait mechanism, priority/affinity, deadline or retirement rule
changes. With job tracing enabled, `[framegen-wait]` also logs callback-wait and
PREVIOUS-release-poll start/end times on the same monotonic clock. These test
whether blocking handoff retirement delays the next pickup; they do not by
themselves attribute lateness to CPU scheduling. Compare adaptive serial versus
request in the same APK with identical instrumentation before drawing conclusions.

`TOTK_FG_ASYNC_RETIRE=on` is a further default-off, adaptive-only experiment;
unset, `off` and invalid values retain synchronous retirement. It requires the
same admitted request-backpressure capability and keeps the hard two-context
bound. Start with an EXPLORATORY off/on pair in ONE APK with
`TOTK_FG_ADAPTIVE_BACKPRESSURE=request` common to both arms. Only a clear
effect merits a separately approved qualification comparison; the earlier
serial/request experiment was invalid and did not qualify either selector.
A dedicated worker waits for completion and PREVIOUS-release fences in FIFO
order, without blocking normal presenter pickup. At capacity the presenter
still waits; earlier pickup alone is not cadence improvement.
Steady-head retirement and total drain each have a two-second monotonic bound.
Missing callbacks, never-signalling fences or fence errors quarantine ownership
and select copy fallback; timeout never releases compositor-owned storage.
Shutdown joins only an observed-exited worker. A static quarantine owner retains
the ENTIRE reachable graph: queue/mutexes/conditions, callback contexts,
buffers, ASurfaceControl/window references and touched presenter objects,
until actual exit or process lifetime. A late callback must find valid storage.
Locked/off policy, two-ms wake, priorities/affinity, deadlines and GPU leases
are unchanged. Trace-on results require exact-layer SF cadence, all-terminal
handoff/deadline shares, capacity-wait fractions and manual visual review;
neither selector is promoted for production use.

Each NEW real or generated presenter job clears its completion cache and
capacity-wait observations, even when it reuses the same physical output slot.
A requeue of the SAME job retains that cache: generator completion runs once
per handoff, releases that job's source/output leases, and is never inherited
by its successor. The first async-retirement experiment build violated this
rule for generated-to-generated reuse in both worker modes; it is not evidence
that the worker improved generation. Earlier locked/request builds did not
have that completion cache.

`TOTK_FG_ADAPTIVE_SLOTS=1` is a separate **default-off, QCOM-only slot
scheduler experiment**, not a qualified 43→60 claim. Compare the SAME APK
with `0` versus `1`; keep the worker and request-backpressure settings common.
It requires a fresh copied API33 default-display FrameTimeline snapshot and
an exactly matching game-window logical display. Unknown/stale clocks,
unsupported capabilities and malformed nonzero requests keep real frames but
refuse generation; no SF phase or timeline ID is synthesized. Existing locked
FrameTimeline admission and the flag-off adaptive path retain their meanings.

The presenter assigns a real image the first uncommitted physical slot at or
after its publication estimate plus a two-ms latch margin. After the actual
platform apply, a causal EWMA1/8 of publication intervals (twelve past samples,
25-fps floor, reset after gaps over250ms) predicts the next real slot. Only
strictly intermediate slots are generation candidates. Publication cadence is
NOT game time: each child's phase uses the source pair's immutable simulated
stamps and GAME anchor interval, mapped to the assigned real slot, with the
existing one-interval prediction horizon. Refresh/lifecycle/scene resets cancel
reservations, never GPU or compositor ownership.

Preparation records the exact source/native-UI/output commands on the render
owner before real submission, pinning source references and descriptor/command
storage. Target selection and QCOM phase dispatch happen only after real
publication, at an existing empty recorder boundary. Preparation reserves no
submission ticket. Optional dispatch claims normal FIFO resources without
waiting; a missing boundary, late target, unavailable resource, phase refusal
or missing timing evidence skips generation rather than delaying a real frame.
The implementation prepares at most one child per source real; extra predicted
children are refused, not reused at an incorrect phase. The private output
requires the admitted AHB route, not the legacy shared copy scratch image.

`TOTK_FG_ADAPTIVE_CHUNK` adds a chunk-boundary dispatch opportunity to the
qualified QCOM slot scheduler. It is **on by default whenever
`TOTK_FG_ADAPTIVE_SLOTS=1`** (absent or exact `1`); exact `0` restores the
begin/commit-only path, and any other spelling is logged once and treated as
off. r7 (CHUNK 0 vs 1) measured the lead gain that justified the default. It tries dispatch
after an already-required `vk_submit_chunk` seal and recorder service, only
when no primary is open. It does not force a split, wait for optional capacity,
add a submission lane or change the phase, deadline or predictive budget.
The child is backend-only: guest timing attribution is suspended for that
callback and restored before guest recording resumes; guest post operations
remain attached to the original final submission. Missing capacity or an
unusable target still skips the child. An earlier boundary is an opportunity,
not a promise that the GPU or compositor can meet its slot.

Two further experiments act only when the slot scheduler
(`TOTK_FG_ADAPTIVE_SLOTS=1`) is active. `TOTK_FG_ADAPTIVE_LATTICE` is **on by
default** with the scheduler (absent or `1`; exact `0` restores the r7 lattice)
after r11 removed the drift misses with no real-cadence cost.
`TOTK_FG_ADAPTIVE_EARLY_APPLY` stays opt-in (exact `1`). Any other spelling is
logged once as invalid and leaves the r7 path. Measure changes against the same
APK with only that selector changed.

- `TOTK_FG_ADAPTIVE_LATTICE=1` re-phases the slot lattice. The lattice period is
  the frozen first present-fence calibration. In r7 it was 16,660,581 ns, about
  4.6 µs longer than the measured vsync, so targets drifted later relative to
  SurfaceFlinger's vsync by roughly 0.28 ms/s. A frame whose desired time lies
  just after vsync E is shown at the next vsync. r7 generated frames were
  reliably shown while the target stayed within about 2 ms after E, and went
  missing once it drifted further. At each REAL staging the target's lag after
  the matching fresh FrameTimeline vsync is checked. When the lag leaves
  0.1–1.5 ms, the lattice is moved to vsync + 0.5 ms. This happens only when no
  generated request is pending. The move bumps the slot generation (old
  identities become stale; no live target is edited), keeps every committed
  slot on its original display vsync and seals past it, so a REAL never lands
  on an applied generated frame's vsync. It keeps the cadence predictor, so it
  does not restart warmup. Window, clock, epoch, scene and gap resets are
  unchanged.
- `TOTK_FG_ADAPTIVE_EARLY_APPLY=1` releases a READY generated frame at its
  matched FrameTimeline deadline minus 1.5 ms when that is earlier than r7's
  target − 2 ms hold. Every REAL-priority, collision, epoch, window and
  suppression check still runs at the actual apply. It never waits longer than
  r7, never applies at or after the target and never changes readiness cutoffs.
  Without a fresh matching entry it behaves exactly like r7.

With `TOTK_FRAMEGEN_JOB_TRACE=1`, both arms log `[adaptive-latch-phase]` for
every scheduler REAL/GEN apply: frame, token, source/output CPU-ready times,
apply time, target/desired, lattice base/period/generation, the matched
FrameTimeline entry (expected, deadline, vsync id, phase error) and the
effective, present-fence and callback periods. `[adaptive-latch-clock]` repeats
the period observations every 300 reals. Join rows to SurfaceFlinger rows by
desired time; app rows are handoffs, not SF outcomes. Selector-on runs also log
`[adaptive-latch-reanchor]`, `[adaptive-latch-early]` and a once-per-second
`[adaptive-latch-summary]`. `[adaptive-scene]` (JOB_TRACE only) records each
scheduler epoch bump and eligibility change with its reason, for example a
pipeline-creation burst's 10 s settle, a scene-signature history cut or an
invalid simulation step. Grace sources are not distinguished.

`TOTK_FG_ADAPTIVE_PIPE_SETTLE=1` (default off; adaptive only, exact `1`) holds
generation for 1 s after a pipeline-creation burst (8 creations within 1 s)
instead of fps_auto's 10 s settle. Each further creation inside the burst window
re-extends the hold. In r7/r11 the in-game bursts were shader warm-up during the
camera route with valid simulation steps, and each 10 s hold removed ~10 s of
generated frames. First-world, loading, low-draw and grace holds keep their
existing durations, and invalid simulation steps still reset the epoch.
`[adaptive-pipe-settle]` logs the selector once; the `[adaptive-scene]`
pipeline-burst row reports the settle actually used.

`TOTK_FG_ADAPTIVE_BUDGET=1` is a separate default-off budget experiment. When
the newest measured generated-child cost is more than 4 real intervals old but
still inside the 32-interval p90 window, admission adds that measured child p90
to the fresh REAL p90 under the same 30 ms ceiling, instead of starting an
unestimated probe limited to one per 30 real frames. Totals over the ceiling
refuse as `stale-estimate`. Probes remain only when no measured child sample
exists in the window. The ceiling, completion check, actual audits, K=8
back-off, prepared cooling/capacity, presenter feedback and all deadlines are
unchanged. It logs `[framegen-adaptive-budget]`,
`[framegen-adaptive-budget-decision]` and `[framegen-adaptive-budget-actual]`.
Judge it on overshoot frequency and size (p95/p99) and on real-frame-rate loss
under 5%, in its own pair.

`TOTK_FG_ADAPTIVE_LEAN` (**default on** with the adaptive QCOM slot scheduler;
exact `0` opts out; other spellings are logged once and stay off; no effect in
any other framegen mode or engine) removes per-real work that the adaptive QCOM scheduler never
reads: the ordered depth-authority reservation, its clipped before/after span
copies and their scene pass splits, the final census/readback, and the scene,
depth and real-fallback copies. Unknown depth writers keep their existing CPU
admission and refusals. The AFME input copy, UI captures and terminal capture
are unchanged, and the uncopied images stay allocated and bound in a defined
state. QCOM refusal or suspension falls back to real-only presentation, which
never reads those copies. Scene AA/scale, `TOTK_FRAMEGEN_WRITER_DIAG` and
`TOTK_FRAMEGEN_DUMP` keep the copies and log `selected but inactive`. Made the
default after the Thor r14d ABBA (`.scratch/adaptive-fg/slot-scheduler/r14d-r13d/report.md`):
delivered cadence ~47.5 → ~56.5 Hz, real game speed 37.2 → 41.5 fps, real GPU
interval −1.1 ms p50, no image-quality change found in video or screenshots.
`TOTK_FG_ADAPTIVE_DIRECT_OUT=1` (default off) replays a prepared child's
terminal straight into its RGBA8 output when the terminal's format and extent
equal the output's, dropping the copy from the replay image; otherwise the copy
stays. At scheduler startup `[framegen-adaptive-selectors]` logs CHUNK,
LATTICE, EARLY_APPLY, BUDGET, PIPE_SETTLE, LEAN and DIRECT_OUT with their
effective values and source (default/explicit/invalid). Under JOB_TRACE,
`[framegen-adaptive-lean-summary]` counts the skipped work every 300 reals.

Immediately before platform apply under the window mutex, newer READY real
jobs are replanned on the SAME API-clock lattice as the generated candidate;
their provisional legacy callback keys are not compared with API targets.
The queue is stably re-keyed with REAL first on equality, and generated
admission receives the real's actual plan, yielding before apply on collision.
`ASurfaceTransaction_apply` is irrevocable to the app: already-applied children
may still be superseded by a later transaction before SF latches, but that is
not a real-priority guarantee. Two-flight capacity, late publication and SF
latch behavior remain residuals to measure. Submitted/cancelled outputs retain
all command/source leases until the covering ticket retires. Unretired or
quarantined graphs are retained, never prematurely reused.
Scheduler templates own an exact terminal replay payload instead of pinning
all four mandatory snapshot arenas. At the original descriptor-freeze point,
only readonly bytes already written into recorder/snapshot host mappings are
copied; no later guest-memory reread is allowed. Four source-owned buffers are
lazy and bounded to 2MiB each, with two private sets and 256 descriptors/type
per source. Terminal images must be that source's canvas (or supported NULL);
unknown/writable resources and capacity excess refuse optional generation.
Source references and covering GPU tickets protect buffers, descriptors and
images through cancellation/quarantine. The preparation lease still retains
its recorder descriptor/transient pool, but has no snapshot-arena mask, and
its immutable external split must not recreate that mask.
Consumed terminal vertex/index ranges are explicitly frozen at their original
draw position even when optional frame-lag capture declines. Logged CPU updates
are included; pending GPU-only writers or unavailable read authority refuse the
optional replay instead of sampling stale host bytes. Only replay bindings use
these snapshots: the original real draw bindings/commands are unchanged. The
same 2MiB bound applies before capture; zero/quad patterns retain their known
CPU-generated authority. Widened guest indices first snapshot their original
bytes through the same guarded selector before replay-only conversion.
The legacy open-sentinel pressure guard remains: an open `UINT64_MAX` lease
is never a GPU wait ticket. Pressure cancels unsubmitted borrowed templates,
then appends while pending/quarantined ownership remains; it never splits a
cached primary or recycles frozen prefixes. Ordinary sealing resolves pending
releases; quarantine retains the complete graph. Flag-off presentation and
finite-fence snapshot reuse keep their existing paths.

Delayed generation uses **predictive admission with post-hoc verification** of
the current open charge interval; sealed history is never rewritten. REAL cost
is predicted from the maximum of the latest up-to-two complete valid REAL
intervals within the four-tick freshness horizon, then added to ALL authenticated
incremental Vulkan and completed GL-group costs for that charge interval.
The unchanged 30ms ceiling applies to that aggregate, not the old source's
REAL cost. Missing source/charge/ticket/fence/timer evidence refuses admission.
At most one runtime template dispatch is admitted per charge interval.
When complete, its immutable ACTUAL aggregate includes even refused/cancelled
children. An overshoot or unavailable actual measurement backs off eight
subsequent charge intervals; duplicate audits cannot extend the pause.
This prediction cannot guarantee that a newly expensive REAL never overshoots.
Report predicted versus actual costs, overshoots and back-off separately.
`[adaptive-slot]` and `[adaptive-slot-dispatch]` distinguish real publication,
requested/committed slots, boundary opportunities and skips. With JOB_TRACE,
`[adaptive-slot-job]` records the exact cancel guard, entry generation state,
backend cancel reason and cumulative CPU-observed snapshot-pressure counters;
`[adaptive-slot-owned]` reports private-payload bounds/refusals. Refusal detail
identifies descriptor stage/set/binding/element/type versus vertex stream/index,
Vulkan buffer offset/range, freeze disposition and recorder serial. These DIAG
fields never admit a buffer or change the original real draw. Dispatch counts
are not SF presents. Validate exact-layer
SF actual-present rows, real simulation cadence, both panels and visual
artifacts; unchanged-publication host replay models slot opportunities, not
generation contention or scanout. Perfetto is deferred until adaptive has
authenticated platform timeline tokens.

With the same default-off `TOTK_FRAMEGEN_JOB_TRACE=1`, the scheduler also logs
`[adaptive-dispatch-guard]` for the precise backend refusal guard and
`[adaptive-dispatch-stage]` for record/seal, bridge, CPU-ready, and transaction
publication stages. Join source/charge/output/ticket identities and explicit
clock/authority fields; a generic `dispatch-refused` is not GPU-failure evidence.
Fact value `-1` means unqueried because an earlier guard short-circuited; it is
not false, missing-as-zero, or authority to call the skipped probe for logging.
Existing native-fence timestamps describe **AFME/GL completion only**. Final
Vulkan/native-UI hardware completion remains UNKNOWN when it is not exported;
CPU fence/post readiness and `ASurfaceTransaction_apply` return are distinct.
`[adaptive-slot-boundary]` brackets begin/commit hooks and, when CHUNK is
selected, the existing chunk seal/service followed by its optional hook.
Recorder-lock request and submission/service-return times remain distinct.
`[adaptive-dispatch-optional]` reports the original inner refusal (including
mutex contention, replay ownership, pin cap or insufficient free split/consumer
slots); unqueried facts are not false. This diagnostic never retries a lock,
waits for capacity, repeats a replay probe or admits a slot.
No dispatch occurs before publication, service completion or an open-primary
ownership barrier. There is no new submission lane. The diagnostic
changes no budget, phase or deadline. Logging has nonzero, unqualified device
cost; trace-on/off host decision parity uses identical clock observations, not
a claim that printing cannot perturb real scheduling.

For the Thor exploratory comparison, use `TOTK_PRESENT_PREROTATE=1` and
`TOTK_PRESENT_UBWC=0` in BOTH arms. The optional root-hint/native-buffer
pre-rotation path comes from signed prerequisite `f9dfec41102d1d465eecf3c04e701a5e9906b9df`,
with rotated-overlay fixture `64ced2d9f648afac5ea99b0b687d29b7c2c01d0c`
and corrected importer allowlist from `65b5e6c1644a4ab10eea9604aeda8fadc7d5bec0`;
no PSO-prewarm floor is required. See `VULKAN.md` for its separate opt-ins.
Display-rotator latency can dominate presentation, so a historical
non-pre-rotated result is not the new same-build baseline. Check admitted
root hint, immutable native buffer extent and inverse transaction geometry;
configured flags alone do not prove HWC admission or scanout.
Request-refusal counters distinguish missing preparation from additional
children. `irrevocable_gen_conflicts` counts occupied-slot traversal only;
separate potential late-real-after-GEN and GEN-associated capacity-wait
records retain actual transaction identities and label the SF outcome UNKNOWN.
Backend ready timestamps are CPU fence-publication observations, not calibrated
GPU end times; absent provenance is explicit, never invented.

The unselected pre-rotation path retains the original vertex shader and 16-byte
push-constant layout/commands. Nonidentity rotations lazily create separate
32-byte matrix pipelines. Recorder hooks first check a default-zero atomic
pending-template gate; no selector/configuration work runs with no prepared
templates. Lifecycle cancellation keeps the gate until recorder-owned cleanup
drains every template, preserving GPU ownership.




`TOTK_FRAMEGEN_DIAG=1` is an optional qualification diagnostic, not required for play. Hybrid checks its capped,
realtime, v143, fixed-slot/60-Hz and pacing preconditions; unsupported refresh/guest intervals retain real frames
and return to native policy. Native UI/hybrid are available from this same APK on other devices with explicit
settings and matching runtime capabilities, not enabled by device identity.

Optional **locked real30 + midpoint interpolation** uses that same explicit timing block, but replace
`TOTK_FPS60=1` with `TOTK_FPS60=0` and `TOTK_FRAMEGEN=hybrid` with `TOTK_FRAMEGEN=locked`.
It never switches simulation cadence. Leave `TOTK_FRAMEGEN_BUDGET_MS` unset: admission and every completed
output must fit the measured **30 ms total real+incremental GPU** ceiling. Correlated late real fences
only impose generation cooldown when that exact complete GPU interval exceeded the ceiling and is within
the four-real-tick freshness horizon. Missing/stale timing does not prove current overload and still cannot
authorize output. The separate feedback late-fence count remains a cadence-failure diagnostic; with
`TOTK_FRAMEGEN_DIAG=1`, correlated late fences log exact same-frame real/incremental/total GPU costs,
measurement availability/age and legacy-would-pause versus actual GPU pause. CPU lateness can still miss display
deadlines. Locked presentation retains the one-real-tick interpolation delay and adds **one physical refresh**
of fixed display-clock lead by default: about **50 ms total image latency at60Hz**. Set
`TOTK_FRAMEGEN_LEAD_REFRESHES=2` for two-refresh lead (about66.7ms total); exactly1 or2 is accepted.
For scene scaling off or SGSR, the maximum simultaneously owned presenter buffers is **seven at lead1 /
eight at lead2**, including backpressured buffers: six legacy disjoint ownerships (two real pair members,
compositor CURRENT/PREVIOUS, queued midpoint and new midpoint), plus at most one retained buffer per
refresh of lead. FSR scene scaling adds one independent phase buffer, composing to **eight / nine**.
These are nonblocking admission bounds, not permission to recycle an owned buffer; allocations remain lazy.
Two distinct future SurfaceControl transactions can be submitted without waiting for the first callback.
Contexts and PREVIOUS-buffer releases retire in submission order; late/nonfuture work drains serially, and
window changes/pause drain before surface removal. New/resumed surfaces request backpressure again.
On API31+ the optional runtime-resolved `ASurfaceTransaction_setEnableBackPressure` is requested before the
first buffer. If API/symbol availability fails, locked presentation uses the **serial fallback**; no load-time
dependency is introduced. The void setter has no readback; the log says `requested`, not verified enabled.
`TOTK_FRAMEGEN_BACKPRESSURE=0` is diagnostic-only: it retains asynchronous queueing without backpressure for
matched off/on tests and may drop/coalesce buffers. Leave unset for play. Missing present fences stay unknown.
Other modes retain their serial presenter and six interpolation slots (seven with FSR scene scaling).

### Experimental API timeline and timestamp corrections (default off)

`TOTK_PRESENT_TIMELINE=1` is a separate **experimental, default-off** locked-AHB interpolation
qualification switch. On Android API33+ it dynamically resolves `AChoreographer_postVsyncCallback`,
all frame-callback-data timeline getters and `ASurfaceTransaction_setFrameTimeline`. Missing API/symbols,
an unset/zero option, or non-locked mode retain the legacy callback/reservation behavior; no new loader
dependency or device-name profile is introduced. Direct QCOM extrapolation and copy presentation do not
use the interpolation-pair selector.
The current native Choreographer observes the default internal display (logical ID0). Every queued
real snapshots its window display ID; other/unknown displays retain legacy reservations and waits,
and their transactions never receive these IDs. Display routing itself is unchanged.

When admitted, each released older real and its generated child reserve adjacent **display-provided**
expected-present/deadline/vsync-ID tuples, with explicit immutable real/generated roles. IDs need not
be contiguous and are never synthesized. Lead1/2 chooses at least one/two physical refreshes of future
display time after reservation, in addition to the existing one-real-frame interpolation hold; quantizing
to the next eligible display timeline can add up to a refresh beyond that minimum. This is not a fixed
50/66.7ms input-to-photon guarantee. Existing seven/eight buffer and two-transaction ownership bounds
are unchanged. A missed child deadline omits that child; a late real is retained and reported.
The older real remains held until N+1 is **recorded**, but in admitted timeline mode it waits only
for its own immutable N buffer's GPU fence, not N+1's unrelated forward GPU completion. Generated
children still wait all own/source work. This lets the real meet its earlier platform deadline
without adding a buffer or another refresh; off/unavailable/other-display paths keep their old waits.

A bounded snapshot contains at most16 API entries. Stale/short/unusable snapshots in an **already
admitted** mode may re-anchor unpublished targets to a fresh legacy Choreographer callback, using invalid
IDs internally and never passing them to the transaction setter. With no fresh clock, only a real is
published beyond the already-reserved same-window floor. Clock recalibration never overtakes queued
targets; surface replacement and pause/resume invalidate callback snapshots. Low-volume
`[totk-present-timeline]` / `[present-timeline]` summaries report admission, API/re-anchor selection,
target lead, ready/apply deadline slack and missed deadlines; they are not physical display evidence.
Keep `TOTK_PRESENT_CADENCE_TRACE` off for timing. Qualify with exact game-layer SurfaceFlinger actual
presentation timestamps; generator counts and framework present fences cannot establish displayed60Hz.

`TOTK_PRESENT_FLOOR_NS=1` is a separate **experimental, default-off** locked-AHB
qualification switch (only the exact value `1` enables it; unset/`0` retain the existing path).
Odin's compositor trace demonstrates that an explicit desired timestamp **equal** to its
predicted present timestamp can be rejected as `not current`. The switch passes
`target - 1 ns` to `ASurfaceTransaction_setDesiredPresentTime` only. Reservation times,
API frame-timeline IDs/expected-present/deadline metadata, generation deadlines, diagnostics,
GPU waits and buffer ownership remain unchanged. It also applies to locked legacy reservations,
not copy presentation or other modes. This is an eligibility-boundary experiment, not permission
to present a refresh early or an established cross-device fix.

Two independently default-off qualification controls address the measured residuals:
`TOTK_PRESENT_DEDUPE=1` requires an API pair's real expected time to be at least half a
refresh beyond the previous child, even across clock recalibration. It selects actual
adjacent API entries and never invents IDs/times; a new drained window alone resets the floor.
This prevents distinct predictions only ~0.1 ms apart from reserving the same physical refresh.
`TOTK_PRESENT_GUARD_NS=125000` separately subtracts the measured 125 μs predictor allowance
from the setter timestamp, additive with the 1 ns floor. Only that exact guard value is
accepted; it is omitted if the total shift would not remain strictly below half the physical
refresh, or if subtraction cannot retain a valid positive signed timestamp. Reservations,
API metadata and generation deadlines remain exact. Other values/default0 leave each control off.

Simulation/input remain30Hz. The complete composition is:

| Mode | Effective lead | Scene off | Scene SGSR | Scene FSR |
| --- | ---: | ---: | ---: | ---: |
| Plain rendering / `FRAMEGEN=1` extrapolation | 0 (lead setting unused) | 3 / 4 | 3 / 4 | 3 / 4 |
| `FRAMEGEN=interp`, interpolating `hybrid` | 0 | 6 | 6 | 7 |
| `FRAMEGEN=locked`, camera / QCOM / LSFG | 1 | 7 | 7 | 8 |
| `FRAMEGEN=locked`, camera / QCOM / LSFG | 2 | 8 | 8 | 9 |

`3 / 4` means three ordinary buffers, or four with the existing explicit `TOTK_PRESENT_BUFFERS=4`
or `TOTK_ACQUIRE_SLACK=2` policy. Interpolation owns its derived pool instead of that override.
The array/output maximum is nine, not nine eagerly allocated images; the retained source ring remains four.
`TOTK_FRAME_LAG=1` is **not required** by locked mode: `0` and `1` are supported. **Thor's qualified locked
profile explicitly uses `TOTK_FRAME_LAG=0`** (replace the inherited hybrid block's lag line): its no-trace run
delivered60 distinct recorded pictures/s. Odin's measured paced runs used lag1; this is opt-in profile data,
not a hardware-name branch or universal default change. Choose lead using measured smoothness/latency.

**Experimental Snapdragon extrapolation:** with the same locked timing/profile settings above, add
`TOTK_FRAMEGEN_ENGINE=qcom` (unset, empty, or `camera` retains the existing generator). Only exact lowercase
`qcom` with `TOTK_FRAMEGEN=locked` selects this experiment; it is never selected by hardware name.
A private windowless GLES3 context must advertise `GL_QCOM_frame_extrapolation`. The two latest **real
pre-UI** scenes are copied into private RGBA16F AHardwareBuffers, preserving linear HDR values, and
`glExtrapolateTex2DQCOM(..., 0.5)` predicts half a real tick into the future. Current real UI is then composed
with the existing affine black/white captures and the frozen guest terminal transfer. The HUD itself is not
extrapolated. Vulkan/GL exchange native-fence FDs with GPU-side waits; a cross-ticket recorder pin retains
frozen descriptors and transient/UBO storage through terminal replay.

Each real is released without waiting for its successor, followed by its prediction **16.7ms later at60Hz**.
The existing lead/slot/backpressure machinery stays in use: expected intentional image latency is about
**16.7ms at lead1 / 33.3ms at lead2**, versus locked interpolation's50 /66.7ms — a **33.3ms reduction**.
These are designed delays, not measured input-to-photon latency or guaranteed scanout. Simulation/input
remain30Hz. Extrapolation predicts unseen motion and can produce disocclusion/animation artifacts that
interpolation avoids.

This path also requires Vulkan AHB import, foreign-queue ownership, SYNC_FD semaphore import/export,
EGL native-buffer/native-fence/wait-sync interop, valid `GL_EXT_disjoint_timer_query` results, and kernel
`SYNC_IOC_FILE_INFO` signal timestamps for the unchanged30ms real+incremental admission ceiling. The
charged external interval is **max(GL elapsed, input-to-output native-fence signal span)**. The fence span
conservatively includes cross-API handoff/queue gaps: it is not a claim of isolated AFME hardware execution
time. Very short GL queries alone cannot certify this implementation's offloaded work. If the exported input
is already signaled (`fd=-1`), a monotonic lower bound taken before the GL call is used instead.
Missing capabilities or failed imports log one `framegen qcom refused` reason and permanently fall back to
existing locked interpolation for that run. Recoverable display/timing incidents follow a different policy:
the first **300 successful real scene ticks** after the first scene (about10s at real30) cannot escalate to a
permanent refusal. Deadline misses, missing/disjoint timing, a100ms unfinished GL result, and completed
native-fence spans over100ms reject that output and pause QCOM for4/8/16/30 real ticks. Five distinct incidents
within120 **post-warmup** real ticks permanently refuse; a clean window clears escalation.

During warmup, cooldown, quarantine or permanent refusal, new frames use the **ordinary camera interpolation
and retained-real presenter schedule**, including its50ms lead1 delay. In-flight QCOM pictures keep immutable
engine provenance; a later mode change cannot relabel their pixels. External semaphore/retirement sidecars
disengage only after genuine terminal completion. Once external callbacks/outputs retire, the ordinary budget
policy is restored with its usual measured warmup boundary. A timed-out GL query/FD/AHB is kept quarantined
until actual completion, never recycled merely because100ms elapsed. A GPU-wide hang may still need restart.

The first valid scene starts a separate context-owner worker which prewarms AFME through three private
RGBA16F AHB/EGL textures at the captured dimensions. The render/presenter threads never join it or wait on its
fence; they keep the camera path meanwhile. This warms format/import/first-call work, not future Vulkan/GL queue
contention or every later resolution change. Scratch resources retire only after the native fence signals.

The high-priority experiment additionally requires `EGL_IMG_context_priority`: the worker requests
`EGL_CONTEXT_PRIORITY_HIGH_IMG` and verifies the **granted** level with `eglQueryContext`. Missing support,
failed creation/query, or a lower grant logs refusal and retains camera interpolation; it never silently
labels a MEDIUM context as HIGH. `framegen qcom EGL priority` records request/grant and EGL vendor;
`framegen qcom GL implementation` records the bound context's vendor/renderer/version. An EGL HIGH grant
is still a driver-level hint, **not proof of KGSL preemption or bounded scheduling latency**.

`framegen qcom Vulkan priority` is **report-only**: it inspects `VK_KHR_global_priority`,
`VK_EXT_global_priority` and the priority-query feature/selected-family levels without enabling those
features or changing queue creation. The renderer keeps its original device-relative priority1.0 and no
global-priority request (API default MEDIUM). Device-relative priority does not control cross-driver
scheduling. A different global priority requires queue/device creation; advertised LOW/HIGH support does
not establish application permission, and no privileged creation attempt or live-queue mutation is made.

With optional `TOTK_FRAMEGEN_DIAG=1`, grep `framegen qcom last 300 real` for submitted/refused reasons,
GL timer, native-fence-span and charged mean/max costs, AHB-copy GPU cost, startup-ignored incident counts
and cooldown-skipped/remaining/escalation counts. `framegen qcom phases`, `framegen qcom warmup` and
`framegen qcom slow-span` separate import/call host elapsed time and signed host/kernel-monotonic boundaries.
With diagnostics on, `framegen qcom slow-span fences` includes the input/output native fence's driver/object
labels and constituent count (for the latest-timestamp constituent). These are kernel-reported labels,
**not KGSL context IDs or scheduler state**; they require no additional ioctls. These diagnostics are not
isolated GPU stage costs. `framegen qcom-present last 300 real` reports accepted
predictions; `framegen headroom GPU` charges the Vulkan copy to real and the conservative external interval
once to incremental. Timing remains mandatory with diagnostics off.
The QCOM branch was device-verified running on Thor. That is not an Odin/Duo qualification or a promise of
visual quality, sustained cadence, preemption or input-to-photon latency. Host fallback/graph tests cover
the refused path, not AFME execution. All external-engine plus scene-scale combinations remain device-unqualified.

**Experimental user-owned Lossless Scaling:** keep the locked-mode timing/profile settings above and add:

```
TOTK_FRAMEGEN=locked
TOTK_FRAMEGEN_ENGINE=lsfg
# Optional reporting; leave absent for the normal timing arm:
TOTK_FRAMEGEN_DIAG=1
```

This uses LSFG 3.1 at multiplier2: one midpoint from two real **pre-UI** scenes,
with the older frame's exact affine UI. It retains locked mode's slots,
backpressure, roughly50ms intentional image latency at60Hz, and30ms total
GPU ceiling. Unset/empty/`camera` and the separate `qcom` experiment are unchanged.
The universal APK contains only the permissively licensed Vulkan backend,
PE parser and DXBC translator, not a Vulkan WSI layer.

Supply `Lossless.dll` from **your own purchased copy of Lossless Scaling**.
We do not distribute the DLL or its translated shaders; you are responsible
for complying with its license terms. The tested resource interface is Lossless
Scaling3.2.1.0's LSFG3.1 DXBC resources255–279 (normal) or255–256/280–302
(performance). Missing selected resources are refused; no model is silently substituted.
With the app stopped, place it at the app-private path
`/data/user/0/com.aure.totk/files/lsfg/Lossless.dll`. For a debuggable
local APK, these are operator-run steps (replace the host path):

```
adb shell run-as com.aure.totk mkdir -p files/lsfg
adb shell -T "run-as com.aure.totk sh -c 'cat > files/lsfg/Lossless.dll'" < /host/path/to/your/Lossless.dll
```

Then start the app normally. A non-debuggable APK requires an authorized way
to provision its private files; this prototype adds no file-picker/import UI.
The source DLL is read only. Translation is in memory, with **no derived
disk cache** and no DLL/shader asset in the APK or repository.

LSFG borrows the game's actual VkDevice and graphics/compute queue. Its compute
dispatches record into the existing renderer primary: the two immutable pre-UI
scene snapshots are sampled directly, and generation writes a retained RGBA16F
storage target directly. No extra AHB import, scene blit, format conversion,
output copy, cross-device semaphore or queue submission exists in this path.
Inputs enter and leave `SHADER_READ_ONLY_OPTIMAL`; output leaves `GENERAL` with a
compute-write to compositor-sample dependency. The same older affine UI, frozen
UI replay and terminal/present path remain in place. The renderer's source/output
ownership protects every borrowed image and descriptor until its covering ticket
retires. One model context per source is retained; skipped pairs rebind only the
input descriptors, without reallocating scratch images or descriptor sets.

`TOTK_LSFG_PERF=1` selects the DLL's reduced-channel shaders and tensor bindings.
`TOTK_LSFG_FLOW=1.0`, `0.75` or `0.5` controls flow width/height relative to the
pre-UI scene; default0.5 retains the earlier model's flow resolution. Output and
UI resolution are unaffected. Invalid settings, unsupported storage/timestamps,
a missing/mismatched DLL or initialization failure log `framegen lsfg refused`
and fall back to the camera engine for that run. No separate-device fallback is
needed: unavailable shared-device execution falls back to the existing camera.

GPU accounting charges LSFG exactly once **inside the real Vulkan ticket**, not
again as an external incremental cost. Its admission uses the maximum correlated
real+synthetic-presentation total from the last eight completed ticks (eight-sample
nearest-rank p90), plus the newest real-cost guard. Camera/QCOM retain their
independent32-sample p90 rule. Freshness, bounded bootstrap probes, reservations,
deadline-feedback cooldown and the strict30ms completion ceiling are unchanged.
This keeps the3.333ms margin; retrospective measurements cannot guarantee the
execution time of a future scene or promise no real deadline misses.

LSFG retains the shared300-scene warmup and4/8/16/30-tick camera cooldown for
late/timing incidents; five post-warm incidents in120 ticks permanently refuse.
Retirement of the actual renderer ticket, never a timestamp availability bit,
authorizes query reuse. Queued pictures retain immutable engine provenance.
Diagnostics report `framegen lsfg last 300 real`, `[lsfg-pass]` and
`framegen headroom GPU`. Pass0 is older mipmaps;1–7 older alpha coarse-to-fine;
8 current mipmaps;9–15 current alpha;16 beta;17–20 gamma0–3;21 gamma4;
22 delta0;23 gamma5;24 delta1;25 gamma6;26 delta2;27 generation.
These are chain-pass brackets (each alpha/beta/gamma/delta contains several
dispatches), not isolated individual shader timings. The total includes layout
barriers and per-pass timestamps. Check layer cadence, moving Link/NPCs,
grass/water disocclusions and translucent HUD edges on each target.

For an image-provenance diagnostic, `TOTK_FRAMEGEN_DUMP=<real frame>` also retains
the raw `lsfg-model-output` before affine UI/terminal composition, the earlier and
later real terminals and pre-UI scenes, and both black/white UI images. `index.txt`
records each source's actual real-frame ID, format, extent/full normalized scene
crop and observed accepted depth viewport/scissor. The model midpoint is0.5 and
the sole x2 intermediate is output0. This blocking dump is not a timing arm.
The accepted root requires a full depth-target viewport; a partial DRS viewport
is refused rather than silently treated as a full texture. A DRS rectangle
already normalized by the guest's composite into its full pre-UI canvas is
different: LSFG samples that complete canvas. Host DRS tests use a larger
allocation with changing smaller crops and NaN padding before this normalization.

`TOTK_LSFG_PROBE=1` is a separate **default-off, high-overhead local diagnostic**,
not a synchronization fix or a play/performance setting. Only the exact literal
`1` enables it; it does not select LSFG or enable the native dump. Combine it
with `TOTK_FRAMEGEN_DIAG=1`, `TOTK_FRAMEGEN_DUMP=<positive real frame>` and the
existing locked LSFG profile. Only generations at/after that request are probed,
until the native dump selects its first generated output. Preparation allocates
reusable backend readback outside dispatch recording. After **each** input's
mipmaps dispatch, before its seven R8 outputs can be overwritten, recording copies
all seven levels to retained host-visible storage. The existing native dump's
covering `vk_flush_wait` fence completes before any backend snapshot is exported;
there is no additional probe submit or wait.

The same private `framegen-<frame>` directory retains the original native raw
images, fourteen backend R8 mip snapshots (older/current, levels0–6), backend
format/extent/offset/byte metadata, and `lsfg-probe.txt`. That receipt checks the
recorded generation against the dumped frame and explicitly reports preparation,
unsupported/no-recording, frame/context mismatch and export/write failures;
`index.txt` also reports probe success/error. A failed probe does not erase the
native raw originals or silently count as a successful snapshot. No DLL,
translated shader, NIR, ISA or resource bytes are exported.

One bounded `[lsfg-provenance]` row describes the actually selected generation:
source slots, captured real-frame IDs and per-slot capture epochs, actual
context-bound image/view/format tuples, extent, history/pair-source state and
predicted pending ticket. The receipt also records captured input tuples,
capture CBs/predicted tickets and both sources' prior reader-ticket, retirement
and reference observations. Image-only rebinding policy is unchanged; a captured
view differing from the context-bound view is reported, not repaired.
At every generic submission seal, after ticket assignment and outside the
submission mutex, the probe compares the record CB with that slot's actual
primary-member list and the existing stream-seal incarnation. This observes
earlier splits and non-final primaries without mistaking a reused CB handle for
the original recording. A match assigns `actual-ticket` exactly once. The real
present seal also tags its final CB before submission and notifies the returned
fence afterwards. If no membership/incarnation match exists, `actual-ticket`
stays **0/UNKNOWN**; the observed seal is not claimed to contain that record.
`predicted-actual-mismatch=-1` means unknown, not a successful comparison.
These observations never replace the existing query/source retirement tickets,
frame-end fence or reference/history gates. With the flag absent/invalid there
are no probe allocations, copies, barriers, waits, ABI loads or generation logs.
Host-only attribution, one-shot matching and parsing/refusal coverage is available as fresh processes:
`vkframegentest --lsfg-probe unset`, `--lsfg-probe 0`, `--lsfg-probe 01`,
`--lsfg-probe true`, `--lsfg-probe '1 '` and `--lsfg-probe 1`.
This diagnostic has no new device qualification.

Each admitted pair bootstraps LSFG's three-set feature history as
`[older, older, current]`; it never reads an unwritten third history slot.
The earlier pair prototype left that slot undefined, making generated pixels
depend on old allocation contents. The regression test now poisons history,
pyramid and output images differently between identical pairs and compares
the complete result, including moving patterns touching the edges at
1280x720,1152x648 and960x536. No alignment padding, cropped border, black fill
or AHB stride workaround is used. Retest the lower corners during camera yaw
on device; host determinism alone does not establish game-image quality.



Interpolation accepts independently validated near/far depth ranges using each source's inverse projection
and a shared midpoint projection. Smooth vertical-FOV changes up to3 degrees per real tick and a1.10 focal
ratio are now accepted; aspect/extent/sign/off-center/jitter, pose-cut, authority, disocclusion and motion
guards remain fail-closed. Extrapolation keeps its strict projection gate. The binding-budget/cooldown,
FOV and frozen UI-buffer replay fixes were device-verified on Thor/Odin; they add no opt-in setting.
`framegen depth-range` counts pairs newly accepted by the depth-range rule and accepted generated outputs.
For qualification only, `TOTK_PRESENT_CADENCE_TRACE=1` logs each immutable transaction identity, real producer,
real/interp/extrap kind, desired/apply and **actual framework present-fence** timestamp. Missing fence results
remain zero/unknown, never substituted with present-call times. Sort by generation/transaction ID and join
only consecutive valid results; framework fences are not guaranteed physical-panel scanout. Diagnostics are off
by default. Real, generated and repeated pictures and missed deadlines must be reported separately.
The same trace opt-in adds transaction pipeline timestamps (record done, queue enqueue/dequeue, own/pair GPU-fence
completion observations, apply/callback), queue depth and pending transaction count, plus explicit dropped-child
and PREVIOUS-buffer release records. Ready observations are completion-thread host times, not GPU execution
timestamps; prior-buffer release is ownership evidence, never current-picture presentation. Off/unavailable
timestamps remain zero/unknown. Actual fence IDs and immutable metadata remain correct under out-of-order callbacks.
`TOTK_PRESENT_CADENCE_TRACE=1` is high-volume instrumentation and **perturbs cadence on Thor**: a traced
lag-off recording fell to about30Hz while the same APK/profile without that trace recorded60 distinct
pictures/s. Never substitute its present-fence rate for distinct-picture evidence, or treat a traced arm as
unperturbed performance. Use matched trace off/on diagnostic arms and independent native-rate video.

**Unknown-device fallback contract (no `totk.env`):**

| Detection/guard | Result |
| --- | --- |
| Complete allowed-CPU `cpu_capacity`, else complete `cpuinfo_max_freq` policy data | Guest auto excludes the lowest tier only below 60% of highest with >=2 CPUs left. IO auto selects it only below that threshold with >=2 CPUs in that tier. Single-tier/all-big, incomplete/unreadable sysfs or insufficient CPUs keeps the inherited mask; no user frequency cap is changed. |
| `seed.vkpack` opens and validates | Indexed pack plus loose overrides. Absent/invalid pack falls back to indexed loose files in the same seed directory; absent pack **and** absent usable loose modules reports missing seed, not fabricated drawing. |
| GPL opt-in plus both pipeline-library extensions and `graphicsPipelineLibrary` feature | Use libraries; otherwise ordinary monolithic pipelines. Missing pipeline-cache-control support disables asynchronous graphics compilation. |
| Required AHB external-memory extensions/entry points, valid allocation/import/format and SurfaceControl parent/fences | Prefer AHB; unsupported/failed import or release switches to copy presentation. Surface-parent absence is retried per window generation; import/fence failure is sticky until restart. Mandatory platform symbols still require the supported Android API level. |
| Timestamp support/valid bits, positive period, host-query reset and usable GPU/CPU clock bracket | Hardware GPU reports; unavailable timing/clock falls back to latency reports with monotonic floor and a diagnostic. Android Turnip bypasses its unreliable calibrated-device-clock result. |
| Host-import/export/dma-heap capability | Automatic pool mode chooses host, export, heap, then none. An explicitly requested unsupported pool mode fails rather than silently changing semantics. |
| Display/refresh callbacks and compatible cadence | Use observed display timing; FPS_AUTO/hybrid reject unsupported cadence. Missing mandatory Choreographer/Looper is a startup failure, not an invented display clock. |
| Optional custom driver/translator/secondary display | Missing custom driver falls back to system; on-device translation requires its executable; companion UI requires an eligible second display. |

Learned compute histories are keyed by Vulkan vendor/device/cache UUID; incompatible histories are ignored.
Compute preparation falls back to synchronous creation if its worker cannot start. Learned seed warming is
bounded (adaptive 64-256 MiB, or explicit bounded override); it does not infer a larger device-memory budget.
Configuration defaults are shared except the exact Adreno722 `TU_DEBUG` correctness guard. Different
FPS/native-UI/hybrid values above are explicit operator profiles; guest/IO affinity and Vulkan/presentation
fallback outcomes differ only by the listed runtime guards.

**Integration opt-in qualification index.** These statuses describe branch evidence, not a universal5 device run.
Host-only does not certify gralloc, SurfaceFlinger, game-image quality or performance.

| Settings (full names unless noted) | Documented status |
| --- | --- |
| `TOTK_UNCAPPED=0`, `TOTK_VK_REC_THREADS=2`, `TOTK_GPU_TIME=hardware` | Existing explicit capped/recorder/reporting controls, not new defaults; Thor/Odin profiles device-tested. |
| `TU_DEBUG=sysmem`, `TOTK_RENDER_RES=960x540` / `1152x648` | Duo device-verified as above; only absent `TU_DEBUG` on the exact A722 guard is automatic. Render resolution remains opt-in. |
| `TOTK_SHADOW_RES=288` | Duo device-tested image-detail trade; not default and little cadence gain in the measured view. |
| `TOTK_FPS60=1`, `TOTK_NATIVE_UI=1` | Odin ordinary-play profile; native UI also qualified in Thor locked play. Duo scene/native-UI still-quality screen only. |
| `TOTK_OPERATION_MODE=docked`, `TOTK_APP_WIDTH=guest`, `TOTK_OUTPUT_RES=0` | Explicit docked/guest-output controls; integrated docked policy is automatic only in docked mode. Host sizing/lifecycle coverage; no claim of a new universal5 device A/B. |
| `TOTK_GUEST_EXTENT_CACHE=0` / `1` | Default1, opt-out0 for prior live-query path; host pixel/call-count/TSan coverage only, Odin A/B pending. |
| `TOTK_FRAMEGEN=locked`, `TOTK_FRAMEGEN_LEAD_REFRESHES=1` / `2`, `TOTK_FRAME_LAG=0` / `1` | Locked camera device-qualified on Thor/Odin; Thor's qualified profile lag0, Odin measured lag1. Lead and optional FSR phase bounds compose as above; no Duo qualification. |
| `TOTK_PRESENT_TIMELINE`, `TOTK_PRESENT_FLOOR_NS`, `TOTK_PRESENT_DEDUPE`, `TOTK_PRESENT_GUARD_NS` | Independently default-off locked-AHB qualification controls; exact behavior and limits are documented in [Experimental API timeline and timestamp corrections](#experimental-api-timeline-and-timestamp-corrections-default-off). |
| `TOTK_DRS_FRAMEGEN=1` | Experimental, default off; applies only to locked real30 frame generation with explicit `TOTK_FPS60=0`. See [Native DRS headroom for locked frame generation](#native-drs-headroom-for-locked-frame-generation-experimental-default-off). |
| `TOTK_ADPF=full`, `TOTK_FRAME_RATE_HINT=1` | Opt-in full ADPF and display content-rate hint; the existing render-only ADPF session remains default. See [Android performance hints](#android-performance-hints-experimental-full-mode). |
| Aligned scene-size classifier | Always on for colour-target pipeline keys; no setting. See [Shader compiles and the pipeline cache](#shader-compiles-and-the-pipeline-cache). |
| Packaged PSO keylist / prewarm | APK binary is generated from `games/totk/assets/pso_keys.tsv`; startup prewarm is on when packaged keys exist. `TOTK_PSO_PREWARM=0` opts out. See [Shader compiles and the pipeline cache](#shader-compiles-and-the-pipeline-cache). |
| `TOTK_FRAMEGEN=hybrid` | Existing opt-in Odin policy. `TOTK_HYBRID_DROP_FPS`, `TOTK_HYBRID_DROP_SECONDS`, `TOTK_HYBRID_RECOVER_SECONDS`, `TOTK_HYBRID_RECOVER_SCORE`, `TOTK_HYBRID_DWELL_SECONDS` are host-tested policy tuners, not individually device-qualified values. |
| `TOTK_FRAMEGEN=interp` / `1`, `TOTK_FRAMEGEN_BUDGET_MS` | Legacy opt-in modes/fixed budget control; host regressions retained. Fixed budget must remain unset for locked mode; no new device recommendation. |
| `TOTK_FRAMEGEN_ENGINE=qcom` | Experimental, active on Thor; Odin/Duo unqualified. Unset/empty/`camera` and host-refused engines preserve the camera graph. |
| `TOTK_FRAMEGEN_ENGINE=lsfg` | Experimental, active-generation host-only with purchased user DLL read in place. Android transport and game quality remain unqualified. No DLL or translated shader distribution. |
| `TOTK_SCENE_SCALE=sgsr` / `fsr`, `TOTK_SCENE_SHARPNESS=0.2` | Opt-in; Odin device-verified, Duo bounded still-quality/cost comparison (not recommended for near30), Thor host-only. FSR extra phase has host ownership/negative-control coverage; no integrated device claim. |
| `TOTK_SCENE_AA=smaa` / `off` | Opt-in scene-only SMAA 1x HIGH before scene scaling/native UI. Companion: `TOTK_FXAA=0`, `TOTK_POST_AA=off`. Device quality/cost requires the matched four-arm comparison; not a new default. |
| `TOTK_POST_SCALE=sgsr` / `fsr`, `TOTK_POST_SHARPNESS=0.2`, `TOTK_POST_SGSR_SHARPNESS=2` | Existing whole-canvas alternatives, Duo device-tested; bypassed for effective native UI. Scene SGSR shares the SGSR controls without filtering UI. |
| `TOTK_POST_SGSR_EDGE_THRESHOLD`, `TOTK_POST_SGSR_EDGE_DIRECTION=1`, `TOTK_POST_AA=smaa` / `off` | Existing opt-in filter/control variants; host pixel coverage, no separate integrated device qualification. |
| `TOTK_OVERLAY=off` / `fps` / `full`, `TOTK_OVERLAY_POS=tl` / `tr` / `bl` / `br`, `TOTK_OVERLAY_POWER=1` | Compact fixed-layout FPS/resolution/mode and active-FG telemetry; power/temperature opt-in, default off. Release off, development full; host pixels/timings, no inferred device benefit. |
| `TOTK_FRAMEGEN_DIAG=1`, `TOTK_PRESENT_CADENCE_TRACE=1`, `TOTK_FRAME_TIMELINE=1` | Qualification-only diagnostics; Thor/Odin evidence, trace measurably perturbs Thor. Slot-exhaustion snapshots require DIAG. Never use a traced arm as ordinary-play timing. |
| `TOTK_FRAMEGEN_WRITER_DIAG=1`, `TOTK_FRAMEGEN_UNCERTIFIED_PLANE=1`, `TOTK_FRAMEGEN_UNKNOWN_PERCENT`, `TOTK_FRAMEGEN_DUMP=<frame>`, `TOTK_VK_PASS_PROFILE`, `TOTK_VK_PASS_DUMP`, `TOTK_INPUT_FILE` | Diagnostic or experimental proof controls, off by default except the unchanged35% unknown-depth ceiling; host coverage, no new device-safe/performance qualification. Uncertified plane is explicitly not a geometric proof; changing the coverage ceiling is not a quality guarantee. |
| `TOTK_FRAMEGEN_BACKPRESSURE=0` | Diagnostic-only unsafe async control; not a play recommendation. |
| `DM_DETCLOCK=0`, `DM_PRESENT_HZ=60`, `DM_PRESENT_INTERVAL=game`, `TOTK_PRESENT_SLOTS=1`, `TOTK_VK_PRESENT=ahb`, `TOTK_ACQUIRE_SLACK=0`, `TOTK_ACQUIRE_CLOCK=0`, `TOTK_PACE=0`, `TOTK_ASYNC_REPLAY=0`, `TOTK_MFX=0`, `TOTK_FPS_AUTO=0`, `TOTK_REALTIME=1` | Explicit timing block qualified with camera locked/hybrid profiles above, not independently device-qualified tuners. AHB/copy fallback remains runtime guarded. |
| `TOTK_PRESENT_BUFFERS=4`, `TOTK_ACQUIRE_SLACK=2`, `TOTK_VK_PRESENT=copy` | Existing bounded-buffer/presenter controls; host contract tests and retained baseline path. Not substitutes for locked-mode ownership bounds or its required timing block. |


## Optional on-device shader translation

Add `--ondevice-xlate` to the normal Android build command (including `--runtime-only --apk`) to ship the
NativeAOT translator. This does not replace `--shaders`: keep the prepared seed to avoid thousands of cold misses.
The build reuses `prebuilt/android-arm64/totk_xlate` when supplied, otherwise publishes with the existing NDK and
.NET 10 SDK. Publish explicitly without rebuilding the game:

```sh
export MISE_DOTNET_VERSION=10.0.402
sh games/totk/tools/totk_xlate/publish_native.sh --android --ndk "$ANDROID_NDK_HOME" --api 33 --out /path/to/output
```

The APK stores the PIE executable as `lib/arm64-v8a/libtotk_xlate.so`; `extractNativeLibs=true` lets Android install
it in the executable native-library directory. It is executed, never loaded as a shared library. An extracted asset
in writable app storage is deliberately not used: current Android app-data execution restrictions can reject it.
The headless output instead includes `totk_xlate`, selected by its generated `run.sh`.

Runtime switches in `totk.env`:

* `TOTK_XLATE_ONDEVICE=0`: disable on-device translation. Otherwise it defaults on **only if the executable exists**.
* `TOTK_SHADER_TRANSLATOR=/absolute/executable`: override native-library sibling discovery (also used for host tests).
* Existing `TOTK_SHADER_SEED_DIR` and `TOTK_SHADER_REQ_DIR` keep selecting the writable seed and PC-request folders.

The first shader miss lazily starts one worker and one long-lived niced (`+10`) child. The worker sends the same
saved request JSON, including `code_hex`, over stdin to `--service --vulkan --out <seed> --chain totk-native-v5-unverified`;
each stdout line is `{"ok":true}` or `{"ok":false,"error":"..."}`. Vulkan mode emits SPIR-V directly and ports
`vk_reflect.py`'s reflection/dependency policy in-process: **no SPIRV-Cross binary and no .NET runtime on the device**.
SPIRV-Cross remains external for the existing PC/Metal service, where it generates MSL, not Vulkan SPIR-V.

The service atomically installs `<variant>.spv` and `.vk` before replacing `<key>.vv`. Only a successful reply
publishes an epoch. The recording thread then reloads affected variant lists and drops negative program/recorder
memos; it creates Vulkan objects and uses the existing pipeline compilation path. Ready variants remain cached.
Draws/dispatches continue to skip until both their variant and pipeline are ready; the game never waits for
translation. A rejected shader keeps its PC request; a child/protocol failure disables this session's worker rather
than blocking the game. `shader-requests/` files remain unchanged for `fill_shaders.py`; avoid running a PC fill
concurrently with on-device publication to the same seed.

Logs distinguish `[shader-xlate] ready` (`translate` and `miss-to-ready`, including queue/startup in the latter)
from `first-draw` / `first-dispatch` (`miss-to-command`). The latter is the first successfully **recorded Vulkan
command**, after any asynchronous graphics-pipeline skips; it is not GPU-completion or screen-presentation latency.

### Initial feasibility and measured host proxy (2026-10-24)

SDK **10.0.402**, runtime packs **10.0.13**, and NDK **28.2.13676358** successfully publish a self-contained
`linux-bionic-arm64` NativeAOT executable from the patched translator. Plain `dotnet publish -p:PublishAot=true`
for either `linux-bionic-arm64` or `android-arm64` failed with `PrivateSdkAssemblies ItemGroup is required`;
`PublishAotUsingRuntimePack=true` fixes runtime-pack selection. Select the NDK compiler with
`CppCompilerAndLinker=<NDK>/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang`;
using host clang instead fails to find Android CRT objects/system libraries. These are build-configuration
requirements, not evidence that Android NativeAOT is unavailable.

The initial Android binary was **5,694,072 bytes (5.43 MiB)**; ELF AArch64 PIE, interpreter `/system/bin/linker64`,
16 KiB `LOAD` alignment, and only `libc.so`, `libdl.so`, `libm.so` in `DT_NEEDED`. Its x64 proxy was **5,574,304 bytes**.
Subsequent operator tests confirmed app-library execution, nice +10 and SELinux access on Thor/Android 13 and
Odin/Android 15, but exposed a lazily loaded OpenSSL dependency; see the correction below. Real game-scene drawing,
device CPU/RSS and first-presented-frame latency were not established by the initial host checks.
NativeAOT emits the existing IL2075 reflection warnings in the optional disassembler and Ryujinx logging formatter;
the translation path still successfully publishes and runs. C syntax checks are warning-free.

On an **Intel i5-13600K**, 35 retained local gather variants spanning VS/PS/CS (**3/16/16**) and **208–18,176**
Maxwell-code bytes each were translated serially with the real service. Direct Vulkan output matched the PC
Metal-service-plus-`vk_reflect.py` fill path for **every SPIR-V byte, `.vk` byte and `.vv` row** in this sample:

| Measurement | Observed host result |
|---|---:|
| Spawn to first protocol rejection (startup proxy, no shader work) | 12.48 ms |
| Direct Vulkan per variant, min / median / p95 / max | 0.56 / 2.03 / 23.93 / 38.67 ms |
| Direct Vulkan, all 35 variants | 268.50 ms wall; 0.21 CPU seconds |
| Existing Metal service + external SPIRV-Cross, median / total | 17.46 / 973.46 ms |
| Child peak/current resident memory after sample (`VmHWM` / `VmRSS`) | 39,516 / 39,516 KiB |
| Empty-seed host acceptance, miss → real Vulkan compute fence complete | 42.60 ms |

The last row is a **tiny synthetic compute** test, not a real game draw or an Android result. Its ready log was
17.89 ms after enqueue; its first recorded dispatch was 40.14 ms. The disabled and absent-binary runs retained
the request and emitted no dispatch. 29 focused existing/new packaging/build/dependency tests plus the real
NativeAOT protocol test passed; four affected C translation units passed the prescribed Android `cc1.sh` check.

**[INFERENCE]** If a mobile core takes 2–10× this host sample's CPU/wall time, budget roughly **4–20 ms median**,
**48–240 ms p95**, and **8–41 seconds** to serially fill 531 similarly distributed misses (mean-based estimate).
Those factors are deliberately not calibrated device measurements; thermals, nicing, shader complexity, storage
and queue position may dominate. No finite first-draw deadline is promised. Local reproducible measurements,
variant identities and parity results are in `.scratch/ondevice-xlate/measurements.json`; no game-derived inputs
or generated translator binaries are committed.

### Android BoringSSL correction

The first operator-driven device request aborted with `Cannot get required symbol a2d_ASN1_OBJECT from libssl`.
`DT_NEEDED` was insufficient evidence: the NativeAOT OpenSSL shim opens `libssl` dynamically, while Android ships
BoringSSL. Two reachable roots were identified, not merely the visible vertex hash:

* `VertexCounterLowering.Apply` called `System.Security.Cryptography.SHA256.HashData`. It now uses an allocation-free,
  managed FIPS 180-4 SHA-256 implementation with caller-supplied output storage. The byte-exact counter allowlist and
  all content-addressed output identities remain unchanged.
* A NativeAOT dependency graph traced `System.Text.Json`'s private `Marvin.GenerateSeed` through
  `RandomNumberGeneratorImplementation.GetBytes` to OpenSSL. This is `JsonDocument.PropertyNameSet`'s duplicate-name
  hashing, introduced in .NET 10. `JsonText` now builds the existing DOM from `Utf8JsonReader` directly, rejects
  duplicate names with `JsonObject.Add`, and preserves ordinary JSON syntax/depth handling without rooting that RNG.

The translator/Ryujinx Shader/Common/SPIR-V source audit found no other explicit cryptographic call sites.
The Android project removes the OpenSSL archive **and its captured linker argument**, retaining direct P/Invoke
bindings so any newly rooted native crypto call is an undefined-symbol link failure. There is no generic SDK
“disable crypto” switch used here; `StaticOpenSslLinking=true` would build/link OpenSSL and is not a fix.
Before copying output, `publish_native.sh --android` scans the complete executable bytes for `libssl`, `libcrypto`,
`System.Security.Cryptography.Native`, and `CryptoNative_`; any match fails publication. Expected output:

```text
Android translator crypto check: PASS (no libssl/libcrypto/crypto-native references)
```

The corrected Android executable is **5,571,224 bytes**. The managed digest passed **266** host-reference comparisons
(FIPS vectors, all lengths 0–256, padding boundaries and inputs through 16 MiB). The **35/35** Vulkan-vs-PC parity
sample still passes, and all **105** `.spv`/`.vk`/`.vv` files match the pre-correction outputs byte for byte.
The supplied real VS request `1a40e5f5…-eade92d6719e` also returns `{"ok":true}` on the host with exact old/new
output parity. Corrected host sample: 4.68 ms startup, 1.85 ms median / 18.93 ms p95 per variant, 35,592 KiB peak RSS;
these remain host measurements, not a claim that the corrected device run has been performed.
Evidence is under `.scratch/ondevice-xlate/measurements-crypto.json` and `crypto-parity.json`.

After packaging the corrected binary in a **debuggable** APK, an operator can exercise the supplied request without
starting the game (the following commands are a device procedure, not a recorded device result):

```sh
PKG=com.aure.totk
APK="$(adb shell pm path "$PKG" | tr -d '\r' | awk -F: '$2 ~ /\/base.apk$/ {print $2; exit}')"
test -n "$APK"
LIBDIR="${APK%/base.apk}/lib/arm64"
adb shell run-as "$PKG" mkdir -p files/xlate-test
adb exec-out run-as "$PKG" "$LIBDIR/libtotk_xlate.so" --service --vulkan \
  --out files/xlate-test --chain totk-native-v5-unverified < .scratch/xlate-req1.json
```

Expect one `{"ok":true}` reply, normal process exit after stdin closes, and complete `.spv`/`.vk`/`.vv` files in
the app's private `files/xlate-test`. This isolates the former abort; the game-scene drawing/latency acceptance
below is still separate.

### Host acceptance and device procedure

The host fixture uses a real NativeAOT linux-x64 translator and the production resolver/client, starting with an
empty seed and a tiny non-game Maxwell compute program. It checks cold skip, live publication, negative-cache
invalidation, unchanged-state reuse without seed I/O, and a real Vulkan dispatch/fence completion:

```sh
make -C games/totk/android vkxlatetest TARGET=linux CC=cc OUT=/absolute/scratch/vk
mkdir /absolute/scratch/xlate-run
/absolute/scratch/vk/vkxlatetest /absolute/linux-x64/totk_xlate /absolute/scratch/xlate-run
mkdir /absolute/scratch/xlate-disabled
/absolute/scratch/vk/vkxlatetest /absolute/linux-x64/totk_xlate /absolute/scratch/xlate-disabled --disabled
TOTK_TEST_XLATE=/absolute/linux-x64/totk_xlate PYTHONPATH=tests python3 -m unittest test_android_vulkan_service
```

Device acceptance must use a **disposable game-folder/seed copy**, not the only shared seed. Select a saved miss
that the target scene repeats, remove just its `.spv` / `.vk` and exact variant row from the copied `<key>.vv`,
and point the app's seed/request settings at those copies. Install an APK packaged with `--ondevice-xlate`.
Run the scene with translation disabled once to establish that the draw stays missing and its request is saved;
restart with the switch unset/`1`. Capture the matching miss, child PID, `ready`, and `first-draw` lines, then
verify the new files/index row and actual rendered result (screenshot plus Vulkan/GPU trace or a relevant pixel
comparison). Report `miss-to-command` separately from measured first rendered/presented frame time.

For the logged child PID, sample `/proc/<pid>/stat` before/after translation (utime+stime / CLK_TCK) and
`/proc/<pid>/status` (`VmRSS`/`VmHWM`), using app-owned instrumentation or a permitted shell on the test device.
Record the device model/OS, requested variant/stage, clock tick rate, sampling interval, CPU seconds and peak RSS.
Repeat with `TOTK_XLATE_ONDEVICE=0` and without the packaged binary: both must preserve the old skip/request path.
Device measurements are not implied by the host test, and no on-device draw, timing, CPU or RSS result is claimed
without that run.

## Dev and release builds

`build_android.py` makes a **dev build by default**, including `--runtime-only` and `--apk` rebuilds.
Add `--release` for a release build. Both use the existing optimized runtime flags; this distinction controls
the compiled `TOTK_DEV_BUILD=1` / `0` policy and APK debuggability, not game patches or CPU/GPU profiling.
The runtime logs its kind once at startup (`[totk-app] build dev …` / `build release …`, or `[totk] build …` headless).

Dev builds show the **performance overlay at top-right** without any `totk.env` entries: FPS, mean frame time,
resolution, mode and graph, plus active frame-generation telemetry. Release builds default to overlay **off**.
Explicit `TOTK_OVERLAY` and `TOTK_OVERLAY_POS` entries override either build's defaults; `TOTK_OVERLAY=0` / `off`
disables it even in dev builds, and `TOTK_OVERLAY=full` enables it in release builds.
Direct Makefile builds use `DEV_BUILD=1` by default; `DEV_BUILD=0` selects release, and changing it rebuilds affected
runtime objects even in the same output directory. The APK-only packer also accepts `--release` for a non-debuggable
manifest; it does not recompile a supplied native library, whose build kind must already match.

## Dual-screen companion

The Android build discovers the SDK via `--android-sdk`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, then `~/Android/Sdk`.
It uses the newest installed platform (at least API 33) and build-tools, with Java 17 source compatibility.
Install a JDK 17+ and the Python 3.14 dependencies in `games/totk/requirements.txt`. With `--apk`, companion assets
are decoded from your extracted RomFS into `<work>/companion-<version>/companion/`; the generator source hash
invalidates that cache. Assets, translations and fonts are never checked into the repository.

The companion offers Adventure (quarter hearts, gloom/extra hearts, capacity, currency, equipment, sages and
minimap), all eight TOTK inventory categories (tap for details, swipe sideways for categories, drag to scroll),
and a three-layer map (drag, pinch, manual layer selection, Recenter restores automatic layer/follow mode).
Capacity is not current stamina or battery charge: 1000 stamina units form one wheel and 1000 energy units
form one Zonai cell; partial final wheels/cells and temporary extra capacity are drawn separately.
Durability bars compare the current category's items when the snapshot has no verified maximum;
the detail panel always shows actual durability and Fuse durability values.
Selecting an inventory item shows its game actions (see [Item actions](#item-actions) below). The optional Minus/Plus
shortcuts only send the game's map/menu buttons and do not pause or move the game framebuffer.
The UI's panel unit is `max(0.55 × density, min(density, shortSidePx / 700))`, with device font scaling retained.
This keeps high-dpi handheld panels useful without forcing tablet layouts onto a fixed resolution.
Adventure uses compact equipment/armor rows to fit the Thor panel; smaller/portrait panels can scroll.
Tapping an equipped weapon, bow, shield or armor card on Adventure opens that item, selected, in its Inventory category.

The Canvas menu uses the game's own Purah Pad background, edge ornaments, icon tabs, item slots/cursor,
detail backing, map frame/north mark and gauge parts when present in the generated asset index. There is no
Purah Pad title/status bar or repeated section captions: only the selected inventory category and map layer
are named. A loading transition dims the last good page without adding a caption; explanatory text is kept
for waiting, title-screen, unsupported-build and unavailable-data states.
The menu keeps warm hairline rules, cream icons on dark tab plates, and translucent dark item slots:
equipped items get a small marker, not a solid blue fill. The fused material and its durability share one
detail row; partial stamina capacity is an arc over a faint complete ring.
`index.json` optionally supplies `decor`, `palette` and `pageIcons`. Decor entries declare a `path`, optional
source-pixel `nine: [left, top, right, bottom]` insets or `tile: true`, and `mask: true` with a CSS `#RRGGBBAA`
`color` for material tinting. Palette colors override the built-in text/panel/accent/background colors.
Any omitted or unreadable part retains a vector/text fallback. The game font is used for supported glyphs,
with system-font fallback where required; no game artwork, translations or fonts live in the source tree.

Snapshots are polled off the UI thread (8 Hz live, 1 Hz full; full refresh on page/category changes).
An unchanged snapshot does not redraw. Missing assets use vector/text fallbacks; absent pouch data is shown as
unavailable, not an empty inventory. Loading keeps the last ready data dimmed for at most 30 seconds.
Presentation windows do not take gamepad focus; display add/remove/change recreates the companion and detach
stops its poller and asset worker. The companion is shown on one secondary display only (never the game's display)
and only while the game activity is started: Home dismisses it and returning shows it again.
Native teardown is preceded by Presentation dismissal/listener removal.

`TOTK_COMPANION=0` in `totk.env` disables the companion at runtime without rebuilding the APK: no controller,
display listener, Presentation, asset worker or snapshot-poller thread is started. Direct JNI snapshot calls return
no snapshot and shortcuts return false without reading guest state or synthesizing input. Unset, empty and values
other than exactly `0` retain the existing default-on behavior; an eligible second display is still required.
This is a startup option, not a live file watcher: edit `totk.env` before restarting the app. `--no-companion`
instead omits the companion Java and assets at build time and keeps the original `android.app.NativeActivity`.

Startup ordering is deliberate: `TotkActivity.onCreate` first calls `NativeActivity.super.onCreate`, which invokes
`ANativeActivity_onCreate`; its `start_game` reads `totk.env` synchronously. Only after `super.onCreate` returns does
Java query the native runtime switch and, if enabled, construct the controller. There is no second environment-file
reader or companion startup thread. `JNI_OnLoad` only registers methods, never caches the switch: app-class lookup
occurs in the caller's app-loader context when `CompanionNative` calls `System.loadLibrary`. Missing companion classes leave
library loading successful for Java-free APKs. The native framework SurfaceControl bridge obtains the activity's
actual class with `GetObjectClass` (including the subclass); its `FindClass` calls target only framework classes.

To build the two pieces in isolation, without rebuilding the game:

```sh
python3 games/totk/android/apk/build_dex.py --android-sdk "$HOME/Android/Sdk" --out /tmp/classes.dex
python3.14 games/totk/scripts/make_companion_assets.py --romfs /path/to/extracted/v143/romfs --out /tmp/totk-assets
python3 games/totk/android/apk/build_apk.py --lib /path/to/libtotk.so --dex /tmp/classes.dex \
  --assets /tmp/totk-assets --key /path/to/apk-key.pem --out /tmp/totk.apk
```

### Item actions

The selected item's detail panel ends with game-style buttons for the actions the native side reports for it:
Equip/Unequip (weapons, bows, shields, armor), Eat/Drink/Use (materials, food), Take out and Fuse to arrows
(materials, Zonai devices). The pause menu's own Hold is not offered (no route through the menu handler exists).
Enabled state and reasons come from the snapshot (per-item `actions`, top-level `actionState` and `pouchGen`, in
live and full snapshots), computed natively from the game's own enable rules; the Java side never decides that an
action is allowed. Buttons are at least 44 dp tall whatever the panel unit.

- **Disabled actions stay visible** with their reason in plain words below the buttons, e.g. "Eat: Your hearts are
  already full", "Fuse to arrows: Aim your bow to fuse", "Take out: Not while aiming", "None left".
  Food that only cures hearts is refused at full health, as in the game; dishes and elixirs with an effect are not.
- **Blocked state.** When the game blocks every action of the item (cutscene, conversation or the title screen's
  event flow; loading; saving; game not ticking; unsupported build), a gold notice names the reason instead
  ("In a cutscene", "Wait for loading to finish", "Not supported on this game build"…).
- **Pause menu.** The native gate reads `PouchMgr+0x1fcc0` (byte; its only reader in code is main+0x2097648, guarded by
  the code words at main+0x2097688/0x209768c). Thor: it is 1 with the pause inventory open on every tab (weapons …
  key items, System) and on the − screen (map, Adventure Log, Album, Compendium tabs), 0 in the world;
  `actionState` is then `{"ok":false,"reason":"menu","pauseMenu":true}` and the companion shows "Close the game menu
  first". No action is routed through the pause menu's handler, so "Open the inventory (+) to …" is not sent by native.
- **One request at a time.** A tap sends one request naming the item by pouch category, slot, actor name, pouch
  partition and `pouchGen`. The tapped button shows a spinner; the other buttons are inert and say which action the
  game is still finishing. Repeated taps are dropped, never queued, and nothing is retried automatically.
- **Result.** "✓ Equipped" (Eaten, Drunk, …) or "✕ Refused: …" / "✕ Didn't work: …" with the reason, e.g.
  "The item moved; try again" (the slot no longer held that item, or the pouch changed since the snapshot),
  "The game didn't allow it", "No response from the game". It stays at least 1.5 s and clears when the inventory or
  action state next changes; a full snapshot is read right after every result.

Requests are submitted and their results polled on the companion's poller thread, never the UI thread
(`CompanionNative.request(action, category, slot, actor, partition, pouchGen)` → seq, or −1 busy, −2 refused now,
−3 unsupported or `TOTK_COMPANION=0`, −4 invalid; `result(seq)` → `{"seq","state","reason"}`). The game thread runs
them from its frame-end tick after re-checking the gate, the item identity and the called functions' code hashes; a
queued request with no game tick for 3 s is refused ("The game isn't running right now"), and one without a visible
outcome after about 2 s ends as "No response from the game" without a retry. The UI additionally gives up waiting after
15 s. The native side offers an action in the world only after it passed the device check below; every other one
stays disabled as "Not available from the companion yet".

**Device check (AYN Thor, 1.4.3, 2 Nov 2026, test save imported through the app).** Each action was tapped on the
companion with the game unpaused; the top screen was compared before/after, the native read-back logged, and the game
saved (System → Save) and reloaded (System → Load) afterwards:

| Action | In-world result | After save → reload | Offered |
|---|---|---|---|
| Equip / Unequip weapon, bow, shield | model on Link's back changes or disappears; pause menu shows the same equipment | kept | yes |
| Equip / Unequip armor (head, upper, lower) | Link's model and the armor set bonus (e.g. Cold Resistance) change; no glitch | kept | yes |
| Eat food (Steamed Fish, hearty) | hearts filled, extra yellow hearts added, dish gone (stock −1 once) | kept | yes |
| Eat raw material (Shock Fruit) | ¼ heart restored, stock 168 → 167 | kept | yes |
| Drink elixir (stamina) | drunk, stock −1 | kept | yes |
| Take out material (Bomb Flower) / Zonai device (Fan) | the game's quick-menu take-out places one in front of Link (pick it up with A), stock −1 | kept | yes |
| Fuse to arrows (Bomb Flower) while the bow is drawn | the arrow carries the flower; the material is used when the arrow is fired (355 → 354) | kept | yes |
| Use (fairy) (3 Nov 2026, Hateno save, hearts 8.75/14 after a bomb) | hearts refilled at once, stock 5 → 4 once, "✓ Used" | System → Save wrote 4 into slot_00's progress.sav | yes |
| Use (energy material, Zonai Charge) (3 Nov 2026, battery drained by a fused Flame Emitter) | offered once the battery was below its maximum ("Your battery is already full" otherwise, so the battery lookup works from the frame-end tick); stock 338 → 337 once (also in the saved progress.sav), but the battery's energy read 44850 before and right after the recharge call, and the gauge refills by itself within seconds, so the recharge is not proven | — | no |
| Fuse to arrows for Zonai devices | not tested on the device | — | no |

Equip/Unequip and the companion's ready state were checked again on the release-key build of 3 Nov 2026 (hash guards
on): Master Sword unequip → equip, each one game call and read back; the opening's Master Sword and Champion's Leathers
show their names and icons after the second-screen art update.

Refusals seen on the device: "Your hearts are already full" for a raw material at full health, "Aim your bow to fuse"
while not aiming, "Not while aiming" for Take out with the bow drawn, `menu` (pause menu and map), `saving` (during
System → Save), `loading` (save loading), `event` (the load-select screen and the event flow right after a load) and
`notEdible`. Five rapid taps on Equip produced exactly one request. The bow-drawn state comes from the game's own
`isArrowAimMode` (main+0x711000), which the quick menu's take-out uses to choose fuse instead; player-state bit 51 turned
out to mean "a material is fused to the arrow", not aiming. `weaponLocked` (player-state bit 33 clear) was only seen for
a moment while a shot is released and while knocked down; falling, gliding, Ultrahand and aiming keep weapon changes
allowed, as the game's own query says, so that refusal is covered by the host test only. The title screen now reports
`title` (its Player actor sits at the origin).

**Settling after any blocked state.** On the Thor a save load passes through ticks that look ok (no event, no menu)
between the load confirmation and the loading screen and again just after it, and the player actor pointer stayed the
same across an in-game System → Load. So the gate also requires the world to have been continuously ok for 1.0 s and
20 frames with the same player actor and pouch partition (and without a gap in the frame-end ticks); until then the
reason is `loading`, every queued request is refused, and the companion's buttons stay disabled. Device check (save
set `save2.zip`, Lookout Landing): Equip was tapped every 0.25 s on the companion from the in-game load confirmation
until well after loading; the log shows loading from frame 8347, the post-load event 8743–8794, settling 8797–8826,
and the first game call at frame 8858, none before. After settling every tap ran exactly one equip/unequip (each read
back done in ~8 frames). **Dialogue:** talking to Purah sets the event context (`event`), the companion shows "In a
cutscene" with Equip disabled for the whole conversation and the taps called nothing; the host test covers a request
queued just before an event or a reload (refused `event` / `loading`).

### Fixture preview (no game/native runtime)

`android/apk/build_preview.py` builds a separate `com.aure.totk.companionpreview` APK with the real companion
Canvas views and the same Presentation controller. It needs no native libraries; the checked-in JSON fixtures
cover starting/title/unsupported, gloom and extra hearts, fused equipment, every pouch category, Sky and Depths,
missing pouch data, a ready→loading transition and item actions with simulated outcomes.
`--assets` is optional and points to a generated root containing `companion/`. Nothing installs or starts
an emulator or device automatically; select the serial of an emulator you started, or of a dual-screen handheld you
may use (only the separate preview package is installed; never touch `com.aure.totk` on it).

```sh
python3 games/totk/android/apk/build_preview.py --out /tmp/companion-preview.apk --assets /tmp/totk-assets
ANDROID_AVD_HOME=/path/to/isolated-avds "$HOME/Android/Sdk/emulator/emulator" -avd dscomp -port 5570 \
  -no-window -no-audio -no-snapshot-load -no-snapshot-save
adb -s emulator-5570 install -r /tmp/companion-preview.apk
adb -s emulator-5570 shell wm size 1240x1080
adb -s emulator-5570 shell wm density 480
adb -s emulator-5570 shell am start -S -n com.aure.totk.companionpreview/.PreviewActivity \
  --ei page 1 --es fixture ready
adb -s emulator-5570 exec-out screencap -p > /tmp/companion.png
# Hot-plug: the companion moves to the overlay; this display then shows only a game stand-in.
adb -s emulator-5570 shell settings put global overlay_display_devices 1240x1080/480
adb -s emulator-5570 exec-out screencap -d "$(adb -s emulator-5570 shell dumpsys SurfaceFlinger --display-id \
  | sed -n 2p | cut -d' ' -f2)" -p > /tmp/companion-overlay.png
# Remove the overlay and stop only this owned emulator when done.
adb -s emulator-5570 shell settings delete global overlay_display_devices
adb -s emulator-5570 emu kill
```

Pages: 0 Adventure, 1 Inventory, 2 Map. Fixtures: `ready`, `starting`, `title`, `unsupported`, `sky`, `depths`,
`unavailable` (ready without a pouch), `loading` (ready for 1.5 seconds, then starting; dims for 30 seconds)
and `nativebuff` (native integer meal-effect ID, 40 hearts, 16 battery cells and temporary extra capacity).
Action fixtures: `actions` (full hearts; Equip/Unequip/Eat/Drink/Take out/Fuse to arrows with mixed enabled and
disabled actions), `event` (every action blocked by a cutscene), `menuonly` (Eat/Drink only from the game's inventory,
exercising the UI path no native action uses yet) and `aiming` (Fuse to arrows enabled, Take out disabled). The
preview answers requests itself: "done" after
`--ei delayMs N` (default 900; use 8000 to capture the pending state) and then changes the fixture (equip marker
moves, stock decrements); the `actions` fixture's Traveler's Spear equip is refused ("item moved"), Weapon_Sword_003
fails with a timeout, the Bokoblin Fang's Take out is refused by the game and the Zonai cart's Take out answers busy.
The companion appears exactly once and never on the activity's own display while a secondary display shows it:
that display then shows a "Game screen stand-in" text, and the companion is drawn on the activity's display only when
no secondary display exists (or with `--ez here true`, which creates no Presentation at all). On a real dual-screen
handheld (Retroid Pocket Duo: 1280×960 at 320 dpi second panel) drive and capture the second display with
`adb shell input -d <displayId> tap X Y` and `adb exec-out screencap -d <physical display id> -p` (ids from
`dumpsys SurfaceFlinger --display-id`). Check the Thor size on the main panel with `--ez here true` after
`wm size`/`wm density`, and reset both afterwards.
`-S` restarts the preview to apply new extras. The restyle screenshot matrix is 1240×1080 at 480 dpi,
1080×1240 at 440 dpi, 1920×1080 at 420 dpi and 1280×720 at 320 dpi: capture Adventure, Inventory and Map,
plus waiting and loading at each size. Expected chrome is icon-only page/category tabs, a selected-category
heading, square framed slots with a game cursor, and no title bar, ready/loading words or map gesture hint.
Build once without `--assets` and capture a page too, to check the readable vector/text fallback.
The preview activity is freely resizable; the actual game's landscape activity is unchanged. Overlay displays
exercise real Presentation hot-plug/non-focusable-window logic, but not NativeActivity/JNI or physical
second-panel input routing; those require a real game build.

### What the companion reads (native side)

`runtime/companion_snapshot.c` builds the JSON the Java side polls; `runtime/companion_jni.c` registers
`CompanionNative.nativeEnabled()` / `snapshot(int)` / `action(int, int)` from `JNI_OnLoad` (exported next to
`ANativeActivity_onCreate` in `libtotk.map`). Snapshot values are guarded reads (`host_safe_read`) of 1.4.3 state,
so the reader works only with the `main` it was written for; only [item actions](#item-actions) call game code.
Before using its offsets it checks six instruction words of that `main`; another build reports `"status":"unsupported"` and reads
nothing. Other game versions compile the reader out (also `unsupported`).

Only explicit taps on the companion's `− Map` / `+ Menu` shortcuts request synthetic Switch Minus/Plus button
press/release pulses (`action` kind 1); snapshot polling never injects input. These use the normal input boundary,
not game-memory writes or guest-function calls, and require both an enabled companion and a ready save snapshot.
`TOTK_COMPANION=0` rejects these requests and item-action requests before even reading the save state.

| Snapshot | Source in 1.4.3 |
| --- | --- |
| hearts, extra/gloom hearts, rupees, poes, stamina and battery capacity, current ability, meal effect | GameData flags (`PlayerStatus.*`) of `gmd::GameDataMgr` (`*(main+0x3ad66b0)`), committed values |
| position, heading | the scene's resident `Player` actor (the `world_survey.c` chain: position `+0x2b4`, rotation `+0x2c0`); `World_PlayerPos` if no actor |
| map layer | `MapData.CurrentLayer`, corrected by height (below -400 is always the Depths) |
| inventory, equipped items, unlocked slots, abilities | `Pouch.<Category>.*` arrays, current partition from `*(main+0x3a81678)+0x188` |
| sages | `PlayerStatus.Companion.<Wind/Fire/Water/Electric/Soul>.*` |
| map pins and stamps | `MapData.IconData.MapPinData.*` / `StampData.*` |

GameData tables are hash-indexed per flag type (MurmurHash3 of the full flag name; enum values are hashes of their
names). Status is `starting` until GameData exists, `title` while there is no `Player` actor (title screen and most
loading), `ready` with a save in play. The reads are not synchronized with the game: each snapshot re-checks the
manager, Int table, pouch partition and player actor afterwards, samples once more if they moved, and reports `title`
rather than mixed values if they move again; a field whose read fails or does not validate (actor names must be plain
identifiers, counts and indexes are bounded) is left out. Without a readable pouch partition the inventory is left out
instead of guessing one. Values the game queued but has not committed yet appear one update later. Current stamina
and current battery charge are not in GameData and are not shown.

`TOTK_COMPANION_LOG=1` in `totk.env` logs each status change and the first full ready snapshot to `totk.log` (to check
a device or save without looking at the second screen). The reader's host test lays out a fake 1.4.3 `main`,
GameData tables, pouch partitions and player actor, including transitions in the middle of a snapshot:
`make -C games/totk/android companiontest TARGET=linux CC=cc OUT=<scratch> && <scratch>/companiontest`.
`python3 -m unittest discover -s tests -p test_companion_gate.py` additionally exercises the production JNI bridge,
activity startup gate and poller-start gate on a Linux host with a C compiler and JDK 17+. It checks default-on and
runtime-off options applied after library loading, blocked direct reads/actions, preview defaults, teardown ordering
and `JNI_OnLoad` with no companion classes. Framework lifecycle/queue observers and synthetic guest-state boundaries
replace Android and the game; this is not coverage of ART's NativeActivity loader, actual displays or device input.
The same suite includes a standalone native JNI-entrypoint driver (`COMPANION_GATE_NATIVE_TEST` in `jni_probe.c`):
it checks registration before the option is initialized, all switch values, blocked reads/actions and concurrent
button-pulse coalescing/release. It replaces only JNI plumbing and guest/input boundaries, so the production bridge
can run under TSan without putting an uninstrumented JVM's GC/JIT in its address space. To select that test alone,
use `PYTHONPATH=tests python3 -m unittest test_companion_gate.CompanionGate.test_native_gate`; a sanitizer compiler
wrapper named `cc` on `PATH` supplies `-fsanitize=thread`. Real host-JVM lifecycle/class-loader tests run normally,
not with JVM-wide sanitizer preloading; this distinction does not establish ART/device behavior.

Compared with the Breath of the Wild companions this follows (Eden-DS, Eden Duo), TOTK replaces runes with abilities
(Ultrahand, Fuse, Ascend, Recall, Autobuild), champions with the sages, weapon modifiers with fused materials, adds
Zonai devices and the battery, and has three map layers (Sky, Ground, Depths). Equip and eat in those companions
call the game's own functions through guest hooks, as this one's item actions do; fast travel is not offered here.

## Install

```
python3 games/totk/android/build_android.py <game.nsp> -k <prod.keys> --firmware <firmware dir> --ndk <NDK> --runtime-only --apk
sh <TOTK_android>/install_app.sh
sh <TOTK_android>/install_app.sh from-device
```

1. The build writes `totk.apk` and `install_app.sh` into the output folder (`TOTK_android`).
2. `install_app.sh` installs or updates the app.
3. `from-device` fills the game folder once. It copies `rewritten/`, `extracted/`, `shaders/`, `user-data/` (the saves),
   `driver.so` and `totk.env` from the adb-shell folder `/data/local/tmp/totk` into the app's folder on the device.
   That's about 17 GB, so it takes a few minutes. `install_app.sh data` copies from the computer instead.

The first build makes a signing key, `apk-key.pem`, in the work folder (`TOTK_build`). Later builds must be signed with
the same key, or Android refuses the update. Uninstalling the app deletes its folder, saves included.

`install_app.sh saves` and `install_app.sh cache-export` use Android `run-as` to read app-private data, so they require
the installed APK to be a debuggable dev build. A release APK is intentionally nondebuggable and denies this access;
the script checks `run-as` before creating an export and reports the limitation without leaving an empty backup.
To export data while preserving it, install a dev APK signed with the same key over the release APK **without
uninstalling**, run the export, then install the release APK again if wanted. Do not uninstall between builds: that
deletes app data, including saves. Both builds must use the same signing key for an in-place update.

### November 11 list-only shader update

The current list adds 301 applicable V rows from MacNativeGames `a8ff9bc429bf451dd1040807c6dae6fa2a0cba55`,
selected through switch2mac's engine pin. `UPSTREAM_MERGE.md` records the exact provenance, exclusions
and previous import. All existing R/S/C and V rows remain; no upstream packed R or A/B/T/S rows are imported.
The list digest is now `9b00d9a9083df6d2d8667969f9b105192851fd7e52d7fa93874ff0a7e5eb90b9`.
The October 27 identity/count tables below are historical, not the identity of this refreshed list.

Installing an updated APK alone keeps an already completed seed usable: there is no automatic
shader-list epoch gate in `InstallState`. Use the setup UI's explicit shader rebuild to gain the
new coverage; it rebuilds the full default-plus-listed seed from the player's existing game files,
not just the added rows. The changed digest prevents reuse of an interrupted old-list preparation.
The translation chain remains `totk-native-v5-unverified`; no translator update or forced cache wipe
is part of this data refresh. Driver pipeline caches are keyed by driver/TU_DEBUG/Vulkan identity,
not the APK/list digest. Seed-bound warm-run comparisons must nevertheless use the same rebuilt seed;
new variants/pipelines can still need first-use work. Preserve saves and never uninstall to refresh.
For desktop-generated seeds, retain existing exact loose variant names and use explicit `--shaders`
as described below; these host measurements do not replace that retention/deployment gate.

Fresh host runs of the production Android C seed stage (four services, current-source isolated
Linux Vulkan translator, same authenticated 1.4.3 inputs) measure:

| Quantity | Before | Refreshed |
|---|---:|---:|
| Default translation requests / completed default variants | 12,456 | 12,456 |
| Listed translation requests | 24,864 | 25,165 |
| Completed listed variants after dependency deduplication | 12,327 | 12,576 |
| SPIR-V modules / total completed variants | 24,783 | 25,032 |
| Packed bytes | 1,479,997,164 | 1,509,968,940 |
| Full generation wall time (seconds, one run each) | 37.43 | 40.56 |

That is **249 net modules and 29,971,776 bytes** more, not 301 modules. Both runs have zero
failed translations; these are not the historical Metal/SPIRV-Cross seed or a retained deployed seed.
Fresh dependency deduplication selects two different listed representative names; their old SPIR-V
and reflection bytes have identical same-key replacements. Desktop deployment must still retain
persisted exact names. Host timings are observations, not phone update-time estimates or a controlled
performance benchmark. Source/prebuilt translators, installed seeds and caches were not replaced.

Device qualification remains required: time the update's cold full shader rebuild and scratch/storage
cost, compare first-use hitches on the same cold-area route, and inspect the picture (depth/alpha,
textures, effects and UI). No steady-state FPS gain or hitch elimination is promised.

### Refresh the shader seed after the October 27 upstream additions

The installation command above preserves an existing seed. Add **`--shaders`** when refreshing it; an ordinary full
build or `--runtime-only` without that flag does not regenerate the seed or deployed `.spv`/`.vk`/`.vv` files.
For an existing full 1.4.3 work/output tree, use the same base-game, keys, update and folder arguments as before:

```sh
env -u TOTK_GATHER_BIAS python3 games/totk/android/build_android.py <game.nsp> \
  -k <prod.keys> --update <1.4.3-update.nsp> --ndk <NDK> \
  --work <TOTK_build> -o <TOTK_android> --runtime-only --shaders --apk \
  --translator "$PWD/games/totk/prebuilt/linux-x64/totk_xlate"
```

Run from the integrated checkout. `--runtime-only` selects the most recently generated supported version, so ensure
its generated `runtime_tables.h` is the intended **v143** tree. Omit `--apk` only if the runtime/seed rather than the APK
is being refreshed; `--release` remains independent. The explicit translator pins the reviewed Linux artifact.
Without `--translator`, a stale binary/source stamp can trigger a translator republish and therefore a different
translator hash. No translation-chain bump or translator-source change is required merely for the list additions.

Generate into a separate work/output copy before replacing a deployed seed. Seed the isolated output with the existing
loose `.spv`/`.vk`/`.vv` files, including collected/fill variants; **a pack alone is not a retention input**.
The Android step passes that directory as `--retain-vulkan-seed`: exact prior names/deps are retranslated from this
game's authenticated inventory with the pinned translator, rather than trusting unknown old translator provenance.
Dependency deduplication must not delete persisted exact names needed by learned compute histories or loose indexes.
`place_shaders` writes a fresh staging directory, checks retained names for content collisions and closed indexes,
and only then replaces the isolated version directory. An incomplete variant or differing same-name payload aborts
without deleting the previous output. `seed-stage-manifest.json` records translator/chain identity and file hashes.
Persisted names also retain their original SPIR-V early-fragment-mode bit during retranslation; that bit is included
in the retention-input digest. Historical fill variants can predate the control-byte preparation policy. The Vulkan
loader already sets/clears `EarlyFragmentTests` authoritatively from the guest control at load time, so this preserves
byte-stable exact names without changing draw-time early-test semantics. New names use the current control-byte policy.

Keep the extracted game inputs read-only, copy the original generated v143 headers/modules into the isolated work
tree, and supply an existing `push.sh` in the isolated output (`--runtime-only` requires it). Check `runtime_tables.h`
against the original module backup after the run; this refresh does not rewrite modules. Pack and validate before
deployment. No device operation is part of generation.

The list sync takes the **union of the existing list and upstream `orig/main` (`ff63a16`) V/R additions**:
**840 V + 29,074 R added, zero existing rows removed**, leaving 57,761 V and 61,613 R rows (S/C unchanged).
All 56,921 original V rows remain, including attr/fout variants that upstream pruned for Metal's interface retyping
(`f541e14`, `10144b0`). Vulkan's matcher still requires those interfaces to agree; do not import those removals or
upstream's default-variant pruning. The 2,354 B (border-color) and 1,394 T (tile-read) rows are intentionally **not
imported**: they describe Metal-only behavior unsupported by this Vulkan path. The translation chain stays v5.
The Sky Islands qualification additionally contributed four V rows (no upstream issue): PS keys `8e3b1150…`,
`41ec32be…`, `a6117903…`, `d7be78b9…`, requiring `2darray_depth`/cube/3D rather than the default 2D bind state.
These requests were absent from both old and first-refreshed packs; they are a coverage addition, not an AGL-pruning
regression. The resulting list has **57,765 V rows** (844 total V additions including these four, still zero removals).

Added rows by first upstream source commit (counts are surviving additions to our list, not upstream's batch totals):

| Source commit / contribution batch | Added V | Added R |
|---|---:|---:|
| `46290da`, 184 issues | 621 | 14,220 |
| `c1acdd5`, #762 #763 #765 #768 #770–#777 #779–#783 | 2 | 370 |
| `802007f`, 8 issues | 0 | 515 |
| `ea1ce3b`, #801 | 0 | 11 |
| `82ba7e2`, #804–#810 #812 #813 #815 #816 #818 #821 #823–#827 | 9 | 52 |
| `1340bee`, 66 issues | 11 | 2,241 |
| `aaf6ba1`, upstream play collection (no issue) | 0 | 15 |
| `39f3eee`, 109 issues in #925–#1077 | 188 | 9,355 |
| `ff63a16`, 32 issues in #1078–#1121 | 9 | 2,295 |

Upstream commits do not retain individual issue-to-row ownership; issue ranges above identify batches only.

For this checkout, the existing 1.4.3 RomFS/SPIRV-Cross inputs, the pinned translator and the prior deployed seed
(`e83409ff…`) as retention input, the validated **v2** seed identity is:

| Manifest field | Expected value |
|---|---|
| `schema`, `chain`, `version`, `game` | `1`, `totk-native-v5-unverified`, `v143`, `totk` |
| `gather_bias`, `spirv` | `true`, `true` (`--keep-spirv` is supplied by the Android step) |
| `early_fragment_tests_policy` | `nvn-control-0x718-v1` |
| `shader_list_sha256` | `74aa399cdb1780e99affbe6151726656ccf746ded6f8ce8f2a07ace2c02fa01f` |
| `preparation_sha256` | `cee818ccade6ed272c9d3cad3df74b1914330d2d2663aad03e06d3e112089895` |
| `translator_sha256` | `0619788dcc93bc89bfd2ca8a15a23efdc2097b2b42361678dab6b433cfaf46f3` |
| `source_romfs_sha256` | `797e66f2efa5b99bdafe4af2d6e778a5ef2f2ca5dc3b45136dd601daea419e9b` |
| `spirv_cross_sha256` | `8df1077abf1d5115b05ab10d7dd097000708f59f34a0baa17ca45d35b291c46b` |
| `retained_vulkan_sha256` | `b0b67d362b5213c5b860747874faebeb93f6c97be8c2d0b7b05c3a7c97b835f0` |
| `retained_variants`, `retained_outside_inventory` count | `25267`, `20` |
| `shaders`, `listed_variants` | `25326`, `24864` |
| `discovered`, `startup_shaders`, `located` | `28669`, `12454`, `0` |
| `skipped` count | `2` (both pre-existing SPIRV-Cross failures; no new failures) |

The manifest replaces list digest `f3788503…` and preparation digest `cbf46a4e…`; the translator, RomFS, SPIRV-Cross,
gather-bias and draw-time early-fragment-test policy are unchanged. Preparation now includes exact-name retention;
it also keeps integrated `3f11923` (reject overlapping AGL records and stage mismatches), which the original seed
predates. The original prepared manifest did **not** describe all deployed files: the old deployed pack had 25,287
variants, including 536 collected/fill variants beyond its manifest's 24,751.

| Manifest count | Original prepared seed | Regenerated v2 |
|---|---:|---:|
| Completed variants (`shaders`) | 24,751 | 25,326 |
| In-scope listed requests (`listed_variants`) | 24,819 | 24,864 |
| Discovered shader keys | 28,771 | 28,669 |
| Prepared startup shaders | 12,474 | 12,454 |
| Skipped translations | 84 | 2 |

The **actual deployed-pack** variant-name diff is **59 added / 20 removed**, from 25,287 to 25,326.
All 25,267 valid prior exact names remain, including the three `-de54a198dbf6` compute names needed by learned
histories and all 536 collected/fill variants. Compared all 50,534 retained `.spv`/`.vk` payloads: **zero content
collisions**, zero retained dependency changes. All 25,326 final variants were regenerated with the pinned
translator/chain; all 72,487 packed payloads were hash-checked and all variant indexes are closed (zero dangling names).

Each of the 20 removed outputs was audited individually against its AGL record and old SPIR-V: all are fake
`rel=0` records inside real control blocks (19 at +0x7e0, one at +0x780), with no non-overlapping record for the
same key. The +0x780 case additionally declares a fragment shader but contains a vertex SPIR-V execution model.
Do not relax `3f11923` to restore them. The two remaining skips are the pre-existing `b940aa86…` / `dfe7662f…`
compute SPIRV-Cross failures.

The validated v2 `seed.vkpack` is **1,498,011,876 bytes** (72,487 records), SHA-256
`5baefe41ea4498f58aa6e1defddb2f02d045e9fdc079fdade5e14424962255ba`.
Loose `.spv`/`.vk`/`.vv` payloads total 1,490,265,009 bytes; the complete staged directory, including pack and staging
manifest, totals 2,999,328,599 bytes. The deployed seed and original shared `runtime_tables.h` were left unchanged.

An overlay-free host probe compiled the production `vk_shader.c` dependency matcher and `vk_seed_pack.h` loader.
It opened only v2's pack, supplied all four captured Sky PS bind states, and required matching packed `.vv` rows plus
packed fragment reflection/SPIR-V: **4/4 resolved**, without loose/device `.vv`/`.vk` overrides. This verifies pack
lookup and dependency coverage, not GPU execution; device qualification remains separate. Host regression suites:
`test_shader_seed.py` 28 tests OK (7 platform skips), plus the historical-mode regression test OK;
`test_pack_shader_seed.py` 17 tests OK.

The first-refresh Sky logs' three compute-prewarm misses were separate from the 579 skipped draws: the latter began
at four PS requests around frame 6405. The >33 ms rate was 49.83% versus 49.29%, close to the 33.3 ms pacing boundary;
the >50 ms tail was 71 versus 23 frames. Logs show translation/cold-read/compile work at the PS miss episode, but
also an earlier unattributed 46–60 ms band with no new pipelines/uploads. Overall performance causality is unproven;
do not present the small long-frame-rate difference as a demonstrated seed regression or guaranteed improvement.

`place_shaders` rebuilds reflection and variant indexes from the regenerated SPIR-V. Runtime loading does **not**
validate this build-time manifest: deploy the refreshed shader directory explicitly, rather than assuming an APK
update checks or replaces it.

## The game folder

The release app's game folder is `<filesDir>/game` (`/data/user/0/com.aure.totk/files/game`, private storage) by
default, the card's `Android/data/com.aure.totk/files`, or a folder picked with All files access (see *Game data
location* below). Developer installs (`install_app.sh`, adb shell) use `/sdcard/Android/data/com.aure.totk/files`
(shown as `Android/data/com.aure.totk/files` in file managers), which a launcher without `TOTK_DATA_DIR` in
settings.env still falls back to; adb cannot reach private storage without root (`fill_shaders.py --device-dir` then
needs `su`). The folder holds what the adb-shell folder holds:

| | |
| --- | --- |
| `rewritten/<version>/` | the game's code (one version folder) |
| `extracted/` | RomFS, shared fonts |
| `shaders/<version>/` | the Vulkan shader seed |
| `driver.so` | the custom Vulkan driver (a Turnip build), picked as an AdrenoTools `.zip` or a 64-bit ARM `.so`. It's copied into the app's private folder at each start, because files on shared storage cannot be loaded as code. Without it, the bundled Turnip driver is used. |
| `user-data/` | saves to import: on the first start (no private saves yet) they're copied into the app's private storage, where the game keeps its saves from then on. `install_app.sh saves` copies those to the computer. |
| `totk.env` | options, one `NAME=value` per line (`#` comments), e.g. `TOTK_VK_SHOT=300` or `TOTK_FACE_BUTTONS=position`. Read after the launcher menu's `settings.env` (app-private `files/settings.env`, written by the menu; only the menu's own option keys are accepted there, others are logged and ignored), so a line here overrides the menu. |
| `totk.log` | the log of the last start (`install_app.sh log`) |
| `totk.ctl` | live switches for in-session A/B runs (below), atomically replaced with each new batch by `tools/bench/live.sh`; startup contents are skipped |

### Diagnostics: logcat copy, crash reports and the stall watchdog

- **`TOTK_LOGCAT`** (default off; only `1` opts in). `totk.log` stays the primary log (written directly, so a sudden end
  loses nothing). Normally there is no mirror thread, watch, polling or logcat write. When opted in, a thread at nice 10
  (on the slower CPUs when sysfs shows them) copies its complete lines to logcat with
  tag `totk`: FATAL, CRASH or "device lost" as errors, red messages (`【红】`, `[RED]`) as warnings, the rest as info;
  lines over 1000 bytes are cut and marked ` [...]`. It sleeps on an inotify watch of the file (20 ms polling only
  without inotify), so an idle log costs no wakeup (`tests/test_log_mirror.py` measures it). The log's
  `[totk-logcat] on (TOTK_LOGCAT=1): ... wake=inotify nice=10` or `[totk-logcat] off` line shows the arm;
  `adb logcat -s totk` reads it. Active mirroring is diagnostic overhead even though an idle log costs no wakeups.
  Compare long-frame tails with `TOTK_LOGCAT=1` versus unset; the default avoids duplicating frame logs through logd.
- **Crash reports** (always on). The runtime's SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGABRT report (`[totk] signal=` …, `[totk]
  host abort`) is written by the first crashing thread only; another crashing thread waits for it at most 5 s, then
  ends the process with `_exit(128+signal)`. After the report the handler that was there before is called once,
  directly, with the original siginfo (`runtime/crash_chain.h`): Android's debuggerd writes a tombstone and a logcat
  backtrace (`adb logcat -b crash`, `/data/tombstones`). Then the signal's default action ends the process (tgkill
  to the crashing thread, `_exit(128+signal)` as the last fallback), whatever that handler did.
- **`TOTK_WATCHDOG`** (default on; `0` turns it off) and **`TOTK_WATCH_STALL=<seconds>`** (default unset). From the first
  presented frame a thread polls every 250 ms; after >= 1 s without a new frame it logs `[totk-watch] no presented frame
  for … s at stage "…" (frame …): thread dump n/3` and the `TOTK_DUMP_AFTER` thread dump (one per stall, at most 3,
  >= 10 s apart). Boot, the background (paused or no window) and held stages (host movies, the first-world pipeline
  warm-up, loading screens where the backend knows them) do not count, unless a held stage lasts 60 s. With
  `TOTK_WATCH_STALL` a stall that long ends the process with status 142 (128+SIGALRM), for unattended runs. The
  `[totk-watch] on: …` / `off` line shows the arm (`tests/test_progress_watch.py`).
  Each mark reuses the present path's existing monotonic timestamp and actual graphics frame number; it adds no
  clock read, atomic read-modify-write, or stage-string copy. Disabled marks publish nothing.
- **`TOTK_THREAD_SAMPLER=1`** (default off; live through `totk.ctl`). Names the threads behind long frames without root
  or logcat (the Duo's logd drops our lines): a thread at nice 10 (slower CPUs) reads `/proc/self/task/*/schedstat`
  4 times a second; each frame over 50 ms and every 300 presents log `[totk-threads] long frame F 87.3 ms, window W ms:
  <name>(tid) run R wait Q, …` to `totk.log`: the 3 threads that ran most in the window (the frame plus at most 250 ms
  before it) and their run-queue wait. Off, its thread never starts and the frame hook is one compare
  (`runtime/thread_sampler.h`, `tests/test_thread_sampler.py`). Confirm a named thread with simpleperf (DEBUGGING.md).

### Live switches (`totk.ctl`)

A/B measurements change experiment switches inside one running session, so both arms share one loaded scene
(`tools/bench/ab.sh`, the `totk-benchmark` skill). A thread at nice 10 (on the slower CPUs, like the logcat copy)
follows `<game folder>/totk.ctl` with inotify (on the file only once readable; 250 ms polling without inotify).
While the file is absent it retains one create/move-only directory watch: unrelated log/cache writes, closes and
attribute changes cause no reader wakeups. Unrelated creates/moves are ignored without reopening or rearming.
If a target open fails for a reason other than a missing file, a 250 ms retry handles later chown/chmod/context
repair without subscribing to all files' close/attribute events. Failed reads use the same retry; it stops when
the file opens successfully or disappears. Explicit watch removals' `IN_IGNORED` cannot feed a retry loop.
It stages each complete line appended after the start; the lines of one write are staged together. `tools/bench/live.sh`
publishes only its new batch via a same-directory temp file, with owner/mode/context set before atomic rename;
replacement is read from the beginning, so it must not copy old commands (which would replay them).

- `set KEY=VALUE` sets a live switch to a decimal inside its range; `unset KEY` returns it to its launch value
  (`totk.env`, else the switch's default); `mark <text>` only writes a marker (the A/B arm).
- Empty lines and `#` comments are ignored. Anything else logs `[totk-ctl] refused "<line>": <reason>` and changes
  nothing: a malformed line, a value out of range, a key that is not live, or a startup-only key
  (`TOTK_IDEAL_CORE_FIX`: set it in `totk.env` and restart).
- The present path applies everything staged at the next presented frame, in file order, and advances one generation:
  `[totk-ctl] gen=N frame=F applied KEY=VALUE unset KEY ...` and `[totk-ctl] gen=N frame=F mark <text>`. F is the
  timing frame of `计时 至帧 F` lines: frames after F run with generation N. Each switch takes effect at its own safe
  point after that; a busy staging lock is retried at the next frame, never waited for.
- Cost without a control file: one relaxed load per presented frame (and per call site of a live switch); reads
  re-look up a switch only when the generation changed (`runtime/live_ctl.h`, `totk_live_cached`).
  `tests/test_live_ctl.py` covers the parser, the frame-boundary application, generation order, the cache, the control
  thread on a real file and a writer / frame thread / readers race under ThreadSanitizer.

| Live switch | Range | Takes effect | Notes |
| --- | --- | --- | --- |
| `TOTK_VK_PIPE_BOOST` | 0–1 | the next join on a worker build | a loan already made is still restored |
| `TOTK_VK_UPLOAD_THREADS` | 0–3 | the next texture upload | helpers start with the first parallel upload; at 0 they sleep without work |
| `TOTK_PRIO_RENDER` | −20…19 | at the application, by tid on every registered render thread | unset returns each to the nice it inherited; the walker keeps `TOTK_PRIO_WALK` when that is set; render threads started later take the live value |
| `TOTK_TIMER_SLACK_NS` | 1…50000 | each game / frame thread before its next timed wait (the presenting thread at once) | unset returns to the launch value, else the thread's default (50 µs) |
| `TOTK_VK_DESC_STOCK` | 0–1 | each recorder context and primary slot at its next slot generation | switched off, an owner frees its stock and destroys its descriptor pools, so the retained memory goes back |
| `TOTK_VK_DESC_STOCK_MAX_SETS` | 0…1048576 | each recorder context and primary slot at its next slot generation | 0 = no ceiling; lowered below an owner's stock, that owner resets its pools and destroys those past the limit at once |
| `TOTK_VK_UNWRITTEN_MASK` | 0–1 | pipelines and output libraries built afterwards | pipelines already built keep their masks (the key does not carry the switch): compare it with a restart, or read the per-300 count of masked new pipelines |
| `TOTK_VK_SPLIT_PASS_DRAWS` | 0…1000000 | the next render pass opened | |
| `TOTK_VK_LOAD_ELIDE` | 0–1 | the next render pass opened | default off; only certified full-coverage first draws discard color loads; `[load-elide]` counts proofs and elided passes per 300 presents |
| `TOTK_VK_FEEDBACK_NARROW` | 0–1 | the next render pass opened | default off; retain read-only attachment feedback across texture-only invalidates; `[feedback-narrow]` counts avoided ends per 300 presents |
| `TOTK_VK_ALPHA_TEST` | 0–1 | the next draw | default off; alpha comparison fragment variant plus immutable reference UBO; `[alpha-test]` counts enabled/ignored draws even while off |
| `TOTK_VK_CLEAR_ON_READ` | 0–1 | the next sampled/storage image read | default off; materialize overlapping pending attachment clears before returning a cached/new view; `[clear-on-read]` counts first-read resolutions |
| `TOTK_VK_STAGING_RETAIN_MB` | 8…4096 | each slot arena at its next open, after its fence | unset returns to the launch value (no cap when unset at launch) |
| `TOTK_VK_FEEDBACK_CUT` | 0–1 | the next descriptor build | the freeze memo key carries the pass serial while it is on |
| `TOTK_VK_PREPARE_MASK` | 0–1 | the next draw's uniform preparation | a change marks every register dirty, so that draw prepares in full; `TOTK_VK_OFF=prepare` keeps it off |
| `TOTK_THREAD_SAMPLER` | 0–1 | the next presented frame | the first switch-on starts the sampler thread; switched off it sleeps until switched on again |
| `TOTK_VK_REC_WAKE_BATCH` | 0…2000 | the next chunk enqueue, and the next time a recorder worker finds no work | a worker already polling finishes its current poll; mixed old/new reads during the change only cost a wake or a moment of parallelism, never a queued batch |
| `TOTK_VK_TEX_MEMO` | 0–1 | the next texture ID lookup and texture registration | memo entries stay keyed by the registry/identity/submission state, so switching back on never reuses a stale one |
| `TOTK_NVN_CALL_NOCOPY` | 0–1 | the next CallCommands / CopyCommands | a recording already holding call markers still expands them at its EndRecording |
| `TOTK_ADPF_GPU` | 0–1 | the next presented frame | that frame is not reported; the session's target switches with it; no effect with `TOTK_ADPF=0` |
| `TOTK_ADPF_LOAD_HINT` | 0–1 | the next span report | clears consecutive-frame history, retains the 500 ms rate limit; needs `TOTK_ADPF_GPU=1` |
| `TOTK_ADPF_CPU_SPLIT` | 0–1 | the next presented frame | queue waits are measured for subsequent frames; total span/GPU unchanged; needs `TOTK_ADPF_GPU=1` and API 35 |
| `TOTK_ADPF_TARGET_PCT` | 50…100 | the next presented frame | default 90 (unchanged); updates the session target before reporting; needs `TOTK_ADPF_GPU=1` |
| `TOTK_RATE_HINT_HOLD` | 0…60000 | the next guest present | ms; a change clears the held rates; no effect without `TOTK_FRAME_RATE_HINT=1` |

`TOTK_IDEAL_CORE_FIX` and `TOTK_THERMAL_STEP` are startup-only and refused. Another launch-time switch becomes live by adding it, with its
range, to `totk_live_switches` in `runtime/live_ctl.h`, reading it through `totk_live_cached` at a point where a change
is safe, and adding its row here. Each switch's per-300 report shows the value in use.

### Game data location, game lock and save signals (release app)

- **Location.** Three kinds (`StoragePaths.Kind`, pure Java, host-tested in `tests/test_android_direct_access.py`):
  PRIVATE `<filesDir>/game` (default; pref `storage/data_dir` absent), APP `<volume>/Android/data/<package>/files` (the
  card's, or the internal one of installs made before private storage) and FOLDER (a picked folder below
  `/storage/emulated/<user>/` or `/storage/<uuid>/`, not the volume root, not inside `Android/`, usable only with All
  files access). `StorageLocation.gameDir()` serves every Java user. settings.env always carries
  `TOTK_DATA_DIR=<dir>`; `start_game` reads it before opening the log and `android_data_dir_check`
  (`runtime/android_storage.h`, host tests in `tests/test_android_storage.py`) accepts exactly: `<internalDataPath>/game`
  (real path compared with internalDataPath's real path), an existing link-free app folder on a mounted (non-tmpfs)
  volume, or a link-free FOLDER on a mounted volume while `Environment.isExternalStorageManager()` (asked over JNI at
  every start) is true. The package comes from internalDataPath, not a constant. Without the key (settings.env of an
  older launcher) it uses `<internalDataPath>/game` when that holds install.json, else `externalDataPath`. A refusal logs
  the reason to `<private>/state/totk.log`, finishes the activity and ends the process 1.5 s later
  (`android_refused_exit`): a reused process would skip `start_game` and show a black window, and the launcher would see
  a "running" game. The log's first line names the kind: `game folder … (private storage|app folder|folder with All
  files access; Android SDK …)`. The menu shows the missing-volume state and dims *Start game*; while it is started it
  listens through `StorageManager.registerStorageVolumeCallback` and rebuilds Home, Game data and the storage choices
  when a card is mounted, unmounted or removed (checked with `sm unmount`/`sm mount public:179,1`).
- **Defaults, measured** (AYN Thor, QCS8550, Android 13; `fsbench` from `.scratch/release/DirectAccess/bench`, run as the
  app's uid with the appop granted, page cache dropped before each phase; 4 GiB sequential write+fsync / read, 20,000
  files of 4–64 KiB created+written+renamed then deleted; MediaProvider CPU from `/proc/<pid>/stat`):
  private 2060 / 2355 MB/s, 1.37 s / 0.33 s, 0 s CPU; internal `Android/data` (an f2fs bind mount, no FUSE) 582 /
  2361 MB/s, 2.84 s / 0.39 s, 0 s; internal shared folder (FUSE) 502 / 2350 MB/s, 323 s / 101 s, 423 s; SD
  `Android/data` (FUSE on exFAT) 46 / 90 MB/s, 1007 s / 325 s, 707 s; SD shared folder 46 / 90 MB/s, 841 s / 242 s,
  561 s. Hence internal default = private storage, card default = the card's app folder (same speed as a shared folder,
  no permission, removed with the app); a FOLDER is offered, never chosen by default.
- **One-time migration.** `StorageLocation.init` (first call of this version, pref `layout`): if the old internal app
  folder holds game data and private storage does not (`StoragePaths.initialChoice`), the pref is set to the old folder
  (label "Internal storage (old location)"), so it stays playable. Home then asks once per menu visit
  (`StoragePaths.migrationPending`; "Not now" is remembered in `migrate_declined`; the skip-menu start shows the menu
  while it is pending) and *Move now* runs the ordinary verified move to PRIVATE, titled "Moving game data to faster
  storage". Thor: 18.7 GB moved in 68 s (copy + read-back ≈ 550 MB/s), first launch "RomFS unchanged; reused".
- **All files access** (Azahar's vanilla flavour, `local://azahar-reference.md`): `MANAGE_EXTERNAL_STORAGE` is declared
  (we ship outside Play; a Play build would remove it with `tools:node="remove"` as Azahar's googlePlay flavour does,
  and `requestLegacyExternalStorage` is unnecessary at minSdk 33). `FileAccess.granted()` =
  `Environment.isExternalStorageManager()`, re-checked on every use and when the menu comes back (`onStart` re-renders
  Home/Game data/Storage). Unlike Azahar it is optional: setup's Storage step and Game data → Storage show an *All files
  access* row that opens `ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION` for the package; declining keeps the SAF path
  and nothing asks again. `FileAccess.documentPath`/`treePath` port Azahar's `getNativePath` (+`RemovableStorageHelper`):
  `primary:` → `Environment.getExternalStorageDirectory()`, `home:` → its Documents, `<UUID>:` → the StorageVolume with
  that uuid, Downloads `raw:` → the path; Downloads `msf:<id>`/`<id>` → the `MediaStore.Files` row with that very `_ID`
  (its `_data`, else volume plus `RELATIVE_PATH` plus `DISPLAY_NAME`), accepted only when `Os.stat` of that path gives
  the same `st_dev`/`st_ino` as `Os.fstat` of the picker's own descriptor (`StoragePaths.mediaStorePath`; never
  `Download/<display name>`, which misses subfolders and could name a different same-named file); folders by id and
  anything else have no path (SAF).
  Users: setup inputs (`Inputs.Opened` emits `{"path":…}` instead of `{"fd":…}` per item when `useDirectPath` holds;
  setup.c opens and closes it, SetupIO's parser), the driver pick, save export (the created document written by path),
  import (zip read in place, folder walked with `SaveManager.candidates`) and the save folder mirror
  (`SaveManager.FileStore`: temp name + fsync + rename per file, partial files never listed). The mirror keeps its SAF
  tree grant and URI pref, so revoking the permission falls back to SAF without losing the folder. Picked folders for
  game data must resolve to a real path (else refused, like Azahar's invalid selection); a non-empty pick that holds no
  game data gets a `TotK Native` subfolder. Revoked with the data in a FOLDER: Home shows "All files access is off",
  *Start game* opens the settings page, Game data has *Allow All files access*; the runtime refuses the folder too.
- **Backup.** `allowBackup=false` (no cloud backup, unchanged) plus `dataExtractionRules`/`fullBackupContent`
  (`build_apk.backup_rules`, res/xml) excluding `file:game`, `file:state` and `external:.` from cloud backup and
  device-to-device transfer; the rest of the private files (saves, settings) keep Android's D2D behaviour.
- **Setup estimates.** Welcome, route cards and the Prepare rail use the route's measured approximate duration:
  decrypted dumps about 2–3 minutes, containers about 15 minutes, plus SD-card copy time (device/storage dependent),
  not the former blanket half-hour estimate.
- **Game lock.** The game holds an OFD write lock on `<private>/state/game.lock` for its lifetime (up to four seconds'
  wait for a setup operation). `GameLock` owns the setup JVM's single POSIX-lock descriptor behind a mutex/refcount:
  concurrent menu probes, save swaps, sync and service work share that owner instead of conflicting with themselves.
  Results are `FREE`, `GAME` (external contention) or `ERROR` with the exception/cause; open/lock errors never mean free
  or game-running. SetupService holds a lease across setup/move; `gameLockHeld=true` makes native move skip opening
  **or closing** that inode (any same-process descriptor close would release a POSIX lock). Standalone moves still
  acquire their own OFD lock. The file itself is never unlinked.
  Before `startActivity`, the menu writes `state/game-starting` with wall-clock time and setup pid under the same
  mutex. The game removes it immediately after acquiring the lock, and on refused start. A live default process plus
  marker counts as `GAME`; a cached process alone does not. A local launch bridges the pre-process gap for up to four
  seconds; abandoned markers without a default process are removed. `TotkSetup` logs the decision branch, process
  pid/name/importance, lock outcome/exception/cause and marker state. Busy/error decisions and native close outcomes
  use WARN so plain `logcat -s TotkSetup` includes them even when INFO is filtered; Java lines include effective
  INFO/WARN loggability flags.
- **Close the game.** Installing, moving/deleting game data, changing the driver and replacing saves are gated
  while `GAME` (errors display their reason instead). Dimmed controls remain focusable for handheld navigation;
  activating one records that exact operation and explains the gate, without mutating data. The menu re-checks every
  second and on resume, so controls clear without a restart. The refresh registers once even when the UI appears
  asynchronously after `onStart`, and unregisters on stop. *Close the game* binds to the non-exported default-process
  `GameControlService` with `BIND_AUTO_CREATE` only while the lock says `GAME`. The foreground menu's ordinary binding
  lifts the game out of Android's cached-app freezer **before** waiting for a save, allowing both the save and the
  native poller to progress. The service returns an inert Binder and never initializes the runtime, starts an activity,
  or posts a notification. A disconnect/death merely triggers another lock check: the lock, not the existence of a
  bound default process, decides whether the game is running. The binding is released on success, failure/timeout,
  and activity stop/destruction; a recreated menu never retains the old connection or its pending action.
  It waits up to 30 seconds for `state/save-busy` to disappear, then atomically writes
  `state/close-request` with a unique pid/time id. A non-render native thread checks it every 500 ms (one stat when
  absent), consumes it and calls the existing native close path. A saving race is refused with a correlated atomic
  `close-response`; I/O refusal includes errno/reason. The menu waits/retries within its bounded close deadline.
  This deliberately does **not** start non-exported, `singleTask` `TotkActivity`: that would pop/destroy the menu and
  lose the pending operation. Neither task affinity nor launcher/recents behavior changes.
  `runtime_fs_begin_close` refuses an active/uncommitted save and atomically gates any later save before its first
  change until process exit. The accepted close flushes the Vulkan pipeline cache and exits, never kills the process.
  Diagnostic close input receives an explicit accepted/refused result; refusing during a save keeps reading commands.
  The menu waits for the game lock/start marker to clear (bounded), then retries the selected operation. Once the
  game is gone, a stale `save-busy` left by a crash does not block completion. A timeout explains why the game was left
  running.
- **Background pause (default).** `TotkActivity`'s actual `onPause`/`onStop` pauses gameplay at the next
  `nvnWindowAcquireTexture`, after releasing the guest virtual core and before taking graphics locks. `onResume`
  wakes it. On a single display, losing window focus also pauses (including live recent-apps tiles which Android can
  leave `RESUMED`), and regaining focus wakes it without requiring another `onResume`. On multi-display devices,
  focus changes alone never suspend the game: using the companion Presentation or another display while Android
  keeps the game resumed continues playing. Callbacks only publish state; they never wait for a frame or an audio
  stream. Input is cleared on each transition and ignored while paused,
  including synthetic button presses and new companion requests. Rumble and motion retain GameFeedback's lifecycle.
  A pending save is allowed the guest frames it needs through `Commit`, not merely the three-second busy heuristic;
  save I/O, the save-generation watcher and Java's `SaveMirror` stop flush are never gated. The independent close-request
  poller still admits `runtime_fs_begin_close`, flushes the pipeline cache and exits while the frame thread is asleep.
  The AAudio callback mutes immediately and discards pre-pause samples. Its device worker requests pause then flush,
  and restarts only after flush completes, with fresh prefill; no queued background audio plays on return. The renderer
  keeps signalling its frame event at the normal 5-ms cadence, without advancing voices or holding its lock across a
  pause wait. The pause gate accumulates monotonic suspension time (including an ongoing pause); VP9 movies
  shift their first-frame deadline by each new suspended interval under the decoder lock and release no video
  during save-completion frames while audio is frozen. Resume therefore preserves the next movie PTS instead
  of dropping queued video as late. `[movie] resume` reports the applied shift, next PTS and cumulative dropped
  frames. Guest/host monotonic clocks themselves continue; the existing realtime frame-step producer rejects
  intervals over 250 ms and resets its five-frame history rather than feeding the background interval into physics.
- **Save signals.** `fs_host.c` creates `<private>/state/save-busy` on the first change to save data and, on the
  `Commit` that completes it with every fsync successful, removes it and rewrites `state/save-generation` (counter,
  tmp+rename). The game-process `SaveMirror.Watcher` polls it (1 s), waits 3 s and for `save-busy` to be gone, then
  uploads a stable snapshot; `onStop` flushes. Measured on the Thor with an SD-card SAF folder: in-game Save pressed →
  `save-generation` +1.1 s → upload start +3.4 s → new complete `sync-NNNNNN` +0.7 s (≈5.1 s in all; 10 files,
  3.6 MB, 0.33–0.97 s per upload of 4–12 files); Home pressed 0.5 s after a save started the upload at once (flush).
  A game killed during an upload leaves a generation without manifest, which the next sync ignores.
- **Move** (`NativeSetup.move`, `setup/move.c`): free-space check, copy into `<to>/.move`, RomFS re-cut to the
  destination layout and checked against the pin, every other file read back and compared, fsync, rename into place,
  `install.json` last, fresh RomFS proof. Java then switches the pref, rewrites settings.env and calls
  `removeGameData(from)` (install.json first). A killed move is never resumed: on the next menu start
  (`repairInterruptedMove`) settings.env is rewritten from the pref and, if the chosen folder is playable, setup's
  leftovers in the other places setup may have used (`StorageLocation.knownFolders`: private storage, the app folders,
  the chosen folder and both ends of the last move, recorded in `move_from`/`move_to` before it starts) are removed under
  the game lock. Measured on the Thor: 18.7 GB internal → exFAT card in 10 min 50 s – 11 min 12 s (copy ≈ 40–50 MB/s,
  read-back ≈ 50–57 MB/s); card → internal in 4–5 min; old internal app folder → private storage 68 s; private storage
  → a picked card folder (`/storage/AAAA-AAAA/TotK Native`, All files access) 11 min 28 s, launched from there with
  the RomFS record reused. Device-checked kill points (temporary hold, since removed): after the
  destination install.json but before the pref, after the pref but before settings.env, and during the old copy's
  removal; after each restart pref, settings.env and the one remaining complete copy agree and that copy launches.
  Known cost: the destination's RomFS proof replaces the source's at commit, so a move killed before the pref switch
  leaves the (still chosen) source to one full RomFS hash at its next start.
- **RomFS parts and proof.** `romfsPartBytes` > 0 (FAT volumes; native also enforces it) writes
  `romfs.bin.000…` of 4294901760 bytes; every reader accepts both layouts and install.json records `romfs.parts`. The
  proof records (`.romfs-verified-v1` / `-parts-v1`) key on dev/inode/size/mtime. Card files behind Android's FUSE view
  get new inode numbers after a remount **and after the page cache is dropped**, so `.romfs-samples-v1` (salted hashes
  of the first/last MiB and 64 sampled 64 KiB blocks, salt private) lets the earlier full hash stand when sizes, mtimes
  and samples match (log: `new device/inode numbers (storage remounted) … reused`). It is a start-up shortcut, not an
  integrity check; any mismatch forces the full hash. Device check (runtime probe from `runtime/romfs.c`, 256 MiB
  test image on the exFAT card): remount → reused via the samples; truncated by 1 byte → full hash, refused; one byte
  flipped inside a salted sample block (mtime restored) → full hash, refused; flipped outside every sample (mtime
  restored) → **accepted** by the shortcut, refused only with `TOTK_ROMFS_VERIFY=full`. The same log line ("new
  device/inode numbers") is printed when only the ctime changed.

### Turnip command-stream slab candidate

The local candidate is `turnip/packages/cs-slabs-20261024`, built from Mesa branch
`totk/cs-slabs-20261024`, commit `adc573c4c7a4a975b8127dceca62a263aef5a1c3`.
It suballocates KGSL command streams from reusable mapped slabs; this is separate from the runtime's
CPU recorder-packet slabs and cross-slot command-buffer leases. The owner measured **about 13,000 fewer
Thor mappings with no FPS cost seen**. That is a device A/B observation, not an integration-agent run,
an Odin result, or proof that all lag mapping churn is solved.

To deploy, close the app, retain the previous game-folder driver for rollback, then copy the package's
**`release/driver.so`** into `/sdcard/Android/data/com.aure.totk/files/driver.so`.
Copy the ELF itself, not the ZIP, `meta.json` or `.debug` file. The release binary SHA-256 is
`6017dad5ea50b4afc33189604b5e20b0651e414a5c3a56ba2dc6fdd75fb93876`; package metadata specifies
Android API33+, Mesa 27.0.0-devel, KGSL and A6xx/A7xx/A8xx. The app copies this game-folder file into
private executable storage at the next start; check `totk.log` for the selected driver/version.
No APK rebuild is needed. Restore the saved previous `driver.so` and restart to roll back.

Slab allocation is enabled by default in this candidate. For the **same-binary control**, put
`TU_DEBUG=noslabcs` in `totk.env` and restart; remove that token and restart for slabs on.
If other `TU_DEBUG` flags are needed, retain them as comma-separated tokens (for example
`TU_DEBUG=bos,noslabcs` enables BO diagnostics while disabling CS slabs). Keep the runtime head,
lag/recorder settings, scene, cache warmth, power policy and presenter identical during the A/B.
The round-nine default-off decision below is historical. In that round, an open shadow report occurred during a run
combining frame lag and this slab driver; the observation did not establish that lag caused the report. Round 10.1
later enabled protected lag for Android shipping 30, but the newer Thor GPU-time evidence supersedes that choice:
lag now defaults off everywhere and Android shipping 30 defaults to hardware reports. Neither the driver result
nor the historical shadow report is reinterpreted as causal.

### Adreno 722 GMEM workaround

`runtime/frontend_android.c` probes raw KGSL `DEVICE_INFO` immediately after `env_file()` when the shared
custom-driver path resolver finds the selected source file. Exact chip id `0x43020100` plus absent `TU_DEBUG`
selects the process-only default `TU_DEBUG=sysmem` and emits one early-startup line; `totk.env` is never rewritten.
Driver path/default setup finishes before app-owned workers start. Android input/thread diagnostic constructors
defer to this startup boundary; desktop/CLI constructor behavior remains unchanged. The pure decision fixture
is `adrenogmemtest`; `tests/test_android_driver_startup.py` executes the production startup with environment,
filesystem and ordering hooks (no Android/GPU emulation), including inherited diagnostics and explicit overrides.
See Device profiles for the bounded earlier device evidence and the separate cache-reload limitation.

Turnip reads debug flags once during initialization, so changing the environment after Vulkan device
enumeration is too late. Restart after every change. Explicit flags replace the automatic default: on an
affected GPU use `TU_DEBUG=sysmem,bos,noslabcs` to retain the workaround while doing that slab diagnostic.
An empty `TU_DEBUG=` or `TU_DEBUG=0` explicitly disables the automatic choice for an A/B. No GPU-name
guess is used if the raw query fails. This does not add a memory-pool fallback for the Qualcomm system
driver; the copy presenter still cannot bypass its missing shared-pool capabilities.


## Controls

Face buttons are mapped by name: the Thor's A is the Switch's A, B is B, and so on (the Thor reports its buttons by
their Nintendo-style labels). For an Xbox-layout gamepad, `TOTK_FACE_BUTTONS=position` maps them by position instead
(the bottom button is the Switch's B, the right one A, the left one Y, the top one X).

| Thor / Android | Switch |
| --- | --- |
| L1 / R1 | L / R |
| L2 / R2 (button or trigger axis) | ZL / ZR |
| stick clicks | L / R stick |
| Start / Select | + / − |
| d-pad (keys or hat) | d-pad |

The Back key does nothing, so a stray press doesn't leave the game.

**− then R** (within 150 ms) cycles the performance overlay off → FPS → full → off for the running session,
at the configured corner, even when it started off; neither button reaches the game. − is held back for up to
150 ms to tell the two apart: a lone − reaches the game that much later (a tap released sooner is replayed with its
own length; another button pressed first delivers it at once). R pressed before − is not the shortcut. Android's
cancelled release (focus loss, controller removed) never turns a held-back − into a press and releases a delivered
one. `TOTK_INPUT_LOG=1` in `totk.env` logs each −/R decision as `[chord] …`.

## Native performance overlay

Set `TOTK_OVERLAY=fps` (or `1`) for the frame rate alone, or `full` for the native performance panel.
The compact first row merges real/presented FPS, real-frame milliseconds and mode (`HH` handheld,
`DK` docked, from `runtime_handheld_mode()`). With FG off, the real and presented handoffs are the same,
so the single FPS value covers both; it is not a separate count of submitted/rendered-but-unpresented work.
The second row uses `960x540->1920x1080 BILIN+BILIN`, for example; equal sizes print one size.
`BILIN` means bilinear/linear stretch, `FSR1` the game's docked EASU or host FSR1, and `SGSR` host SGSR.
Only non-1:1 stages appear, in game scene → guest canvas, then host canvas → front-buffer order.
The integer font scale is preserved (3 at 1080p; 21-pixel glyphs). Four-digit resolution fields reserve
their maximum width; unavailable/overflowing values show `?`, never a plausible clamped measurement.

Active FG adds the engine (`CAMERA`, `QCOM`, `LSFG3.1`, or the `ME2` publication hook), method
(`INTERP`/`EXTRAP`) and warmup/cooldown/refusal state. QCOM fallback names `QCOM->CAMERA` rather
than implying the QCOM engine is still generating. LSFG's `PERF` marker is published by the model loader.
`GPU real+incremental/budget MS` uses only the renderer's already-completed, paired headroom samples;
the displayed budget is the real 33.3-ms tick, including the policy's reserved margin. Unknown timings
are omitted rather than zero-filled. No new timestamp queries, GPU readbacks or waits are installed.
`GEN shown/expected` counts synthetic handoffs divided by real handoffs in the actual trailing second
(one synthetic opportunity per real at 2×), using the same timestamp history as FPS. It is independent of
refresh stalls, engine warmup/cooldown changes and the diagnostic counters reset every300 frames.
It is not the number merely submitted or accepted by the generator; incomplete retained windows show unknown.
`COMPILE count totalMS` reports synchronous graphics/compute builds in the last second, not asynchronous
workers or pipeline-cache joins. Its fixed row stays reserved when empty.

Power/temperature are **off by default**, including `full`. Add `TOTK_OVERLAY_POWER=1` to opt in:
readable battery discharge current × voltage shows `PWR`, meaningful USB/input counters show `IN`,
and standard `battery/temp` shows `BAT …C`. Unavailable sensors are silently omitted; temperature is the
battery sensor, not an inferred GPU/SoC temperature. A dedicated background sampler reads sysfs at most 1 Hz.
The option reserves its row even when sensors are unavailable, preventing layout jitter.

System telemetry is separately **off by default**: `TOTK_OVERLAY_SYS=1` reserves two extra rows in `full`
(never in `fps`). `THM 0 H0.42 CPU6 0.75 CAP`, for example, shows Android's thermal status (0–6),
current thermal headroom, and the current/hardware-max frequency ratio of the policy containing `g-main`'s
sampled last host CPU. `CAP` means readable `scaling_max_freq` is below `cpuinfo_max_freq`; no marker is
not proof of no cap when the cap counter is unreadable. CPU migration follows the next sample, not a guessed cluster.
`RAM available/totalM RSS residentM` uses `/proc/meminfo` MemAvailable/MemTotal and `/proc/self/status` VmRSS,
all in MiB. Missing/invalid values are omitted; both rows keep their space. The existing background sampler
does all proc/sysfs/thermal reads at most 1 Hz while full is shown, with no per-frame system work.
Headroom (API 30+) refreshes at most every 10 seconds as Android requires; status needs API 29+.
Unsupported APIs, denied sysfs and NaN headroom hide only their fields. Compare a screenshot with
`dumpsys thermalservice`, `/proc/meminfo`, the game's `VmRSS` and that CPU's `cpufreq` files; use matching
full-overlay runs with/without SYS to measure overhead. `[totk-overlay] sys=0|1` records the startup switch.
`TOTK_OVERLAY=0` / `off` leaves the picture untouched (release default; dev default is `full`).
`TOTK_OVERLAY_POS=tl|tr|bl|br` selects the corner (default `tr`). `TOTK_OVERLAY` is the start mode; in app builds
the − then R shortcut (see Controls) changes it live, so presenter images always carry `TRANSFER_DST` and the
telemetry publications below run while the panel is hidden (a few relaxed stores per present); staging, raster,
copy and the power sampler's reads happen only while it is shown. The `fps` mode is one `FPS 30.0` row
(`FPS 30.0->60.0` with frame generation), no graph.

Intervals use `CLOCK_MONOTONIC` at successful copy-window posts or immediately before AHB transaction
handoff, not game submission, GPU completion or compositor-release waits. Rounded field changes are
formatted at most once per second. The 12-pixel-high graph spans the full inner width; its ring appends
one column per handoff interval without resampling or shifting history each frame. It clamps at 100 ms:
green at/below target, yellow through 1.5×, red above; a thin grey line marks the target.
Without a supplied target, `TOTK_FPS60=1` selects 16.7 ms, otherwise 33.3 ms.
This measures compositor handoff cadence, not physical scanout timing.

Scene size is best effort, with no GPU readbacks: the backend keeps the largest landscape depth-backed MRT
viewport used by an actual draw in the current frame, bounded by its existing attachment extents. The aspect
test permits independent per-axis rounding within a 16-texel alignment quantum, including 960×536, 1440×808
and 1600×896. A clear-only pass never nominates its inherited viewport; viewport changes dirty the sample so
the next real draw re-evaluates it. This follows DRS even with padded storage, rejects square shadow maps
and single-target UI passes, and does not depend on frame generation. Without such a draw (menus/video),
the game's valid presentation crop is the fallback. Publications return before any atomic stores when the overlay is unavailable (non-app builds started off).
This is not proof of scene provenance for every unusual pass. Output is the actual front-buffer extent recorded
by the presenter, not a requested scene override. Below the guest canvas the game method is inferred from its
reported mode: docked FSR1/EASU, handheld bilinear; a 1:1 stage is omitted.
Resolution, target cadence, scaling methods, frame-generation telemetry and display handoffs use relaxed
atomic publications, with no new locks, syscalls, clocks or allocation on their publish paths. Fixed layout
is computed only at initialization or actual output/mode/overlay-configuration changes, never from text
length, DRS, numeric digit boundaries, cooldown or COMPILE appearance. Each idle staging slot rewrites
only changed glyph cells and new graph columns. The GPU performs one small transfer command (disjoint
regions rotate the graph ring) with the existing barriers and no additional render pass.
`TOTK_FRAMEGEN_DIAG=1` reports `[overlay]` layout rebuilds, text-field updates and graph appends per
300 real frames. Diagnostics are qualification-only, not normal-play performance evidence.

Scene-upscale integration: call `vk_overlay_set_scale_stage(VK_OVERLAY_STAGE_HOST, VK_OVERLAY_SCALE_SGSR)`
(or `VK_OVERLAY_SCALE_FSR1`) only for the active host scaler; restore `VK_OVERLAY_SCALE_NONE` on bypass.
The default host method at differing canvas/output sizes is `BILIN`; dimensions suppress a 1:1 stage automatically.
`vk_overlay_set_resolution(render_w, render_h, canvas_w, canvas_h)` accepts known active scene/canvas extents.
`vk_overlay_set_framegen(engine, method, state, cooldown, fallback)` publishes the active engine/policy
without querying it from the overlay; `VK_OVERLAY_FG_ME2` is reserved for a merged motion-estimation engine.
`vk_overlay_set_lsfg_model(major, minor, performance)` identifies an actually admitted model.
`vk_overlay_set_gpu(real_ms, incremental_ms, budget_ms, paired)` accepts already-harvested cost only;
`vk_overlay_note_compile(existing_end_ns, existing_duration_ns)` reuses existing compile timestamps.
These hooks all return immediately when the overlay is unavailable.

The opaque panel is GPU-copied after final scaling/postprocessing on both presenters; integer text scale follows
output height. Each active present slot (three by default, optional fourth) owns persistent mapped staging, reused only after GPU/compositor
release. Battery power is the absolute microamp × microvolt product; `IN` is the reported input/source counter,
not calibrated whole-device or SoC consumption (battery `power_now` semantics differ between devices).
Values that would round to `0.0 W` are hidden.
For device A/B use `TOTK_OVERLAY=full`, `TOTK_OVERLAY_POS=tr`, `TOTK_VK_PRESENT=copy`, then repeat with
`TOTK_VK_PRESENT=ahb` and verify the AHB log has no copy fallback.
The host fixture is `make -C games/totk/android vktest-overlay TARGET=linux CC=cc OUT=<scratch>/overlay`;
run `vkoverlaytest` in fresh processes with the desired env, or `vkoverlaytest --bench` for uncached CPU raster cost.
`vkoverlaytest --power <absent scratch directory>` runs synthetic power, CPU-frequency and proc-memory fixtures without device access.
`vkoverlaytest --render-layout <output.ppm>` renders a controlled FG example without a game/device;
`TOTK_OVERLAY_POWER=1` adds its optional sensor row; `TOTK_OVERLAY_SYS=1` adds deterministic system examples.
`--bench-record` measures the warmed production
recorder; `TOTK_OVERLAY=off` is the empty control and `TOTK_OVERLAY_BENCH_FG=qcom` exercises the FG template.
`vkdrawtest --overlay-scene` (also `vkdrawtest-app`) submits real MRT/depth draws to check aligned DRS sizes,
clear-open → viewport-change → first-draw ordering, same-pass viewport updates, scene exclusions, and disabled
overlay sampling. `vkoverlaytest` also steps the live mode through all three states mid-run (pixel-checked).
`vkoverlaytest --guest-output` also checks zero steady-frame window queries, initial publication, stale attachment
epochs, destroy/recreate/display-move/resize events, and guest crop changes. `--extent-diag` extends that scenario
to 600 frames to exercise both 300-frame reports; repeat with `TOTK_GUEST_EXTENT_CACHE=0` as the live-query control.

Compact-layout host acceptance: **167 matrix processes**, both startup arms, TSan, the intentional default
atlas-UV negative control and **216 Python tests** completed with expected results. Exact glyph tests cover
HH/DK, equal/mismatched resolutions, BILIN/FSR1/SGSR, engine/fallback/method labels, GPU, GEN and COMPILE.
Digit boundaries (9.9→10.0 ms, 99→100 FPS), four-digit dimensions, COMPILE expiry, engine/state switches
and 16 DRS flips preserve layout; a real output viewport change causes exactly one rebuild.
The 600-frame diagnostic fixture's steady second window records **0 layout rebuilds, 8 text-field updates,
300 graph appends** in either arm. Initialization/resize/recreation occur in the first window.

At 1080p the plain fixed template is **588×147 pixels**, FG **678×213**, versus the prior **720×219**;
the opt-in sensor row adds 33 pixels. Text row pitch is exactly the old overlay's **11 base pixels / 33 pixels
at scale 3** (`c2af5eeb` `vk_overlay.c` text baselines 4,15,26,37), leaving four blank rows below each
7-pixel glyph. Tight outer padding, fixed configuration layout and the full-width graph are unchanged.
The following measurements describe the original compact layout (8-pixel row pitch), before the readability gap.
Five alternating fresh processes per variant, 1,000 warmed records each, shared host lock, cores 0–3,
nice 10, lavapipe Mesa 26.0.8/LLVM 21.1.8, `LP_NUM_THREADS=0`, power disabled:

| Variant | CPU record median / p95 (µs) | Vulkan timestamp median / p95 (µs) |
| --- | ---: | ---: |
| Overlay off / empty timestamp control | 0.014 / 0.018 | 1.200 / 1.422 |
| Prior resolution panel | 8.176 / 11.051 | 22.464 / 37.497 |
| Compact plain panel | 0.830 / 1.102 | 9.976 / 12.304 |
| Compact FG panel | 0.858 / 1.003 | 14.597 / 17.856 |

Entries are medians of run medians/p95s, not a zero-cost claim. GPU columns include the timestamp/control
interval; submit/wait and result reads are outside CPU recording. Both new templates reduce measured steady
CPU and software-Vulkan cost versus the prior panel despite the additional telemetry. This is a host proxy,
not Adreno/Thor timing; no device access or scanout measurement was performed.
The superseded readability-gap revision (9-pixel pitch) passed **58 overlay build/test/render/benchmark processes**
under the shared host lock across both arms, including exact blank-row pixels and unchanged full-width graph.
Single warmed 1,000-record batches measured plain **0.868 µs CPU / 10.036 µs Vulkan**, FG
**0.854 µs CPU / 15.337 µs Vulkan** (medians, same host controls; not the five-run aggregate above).
Those historical revision logs/results are `fg2-*.log` and `fg2-results.json`; current PNGs use the restored old pitch.
The final old-pitch revision (11 base pixels) passed **65 overlay-affected build/test/render/benchmark
processes**, including `vktest-app` overlay off/fps/full in both arms, under the shared lock. No unrelated full
matrix was rerun. Single warmed batches measured plain **0.954 µs CPU / 13.507 µs Vulkan**, FG
**1.046 µs CPU / 21.516 µs Vulkan**; logs/results are `fg3-*.log` and `fg3-results.json`.
Evidence, before/after PNGs, all logs and reproducible harnesses: `.scratch/overlay-fg/`
(`results.json`, `bench-results.json`, `before.png`, `after-plain.png`, `after-fg.png`, `layout-comparison.png`).

Historical pre-compact resolution-panel cost comparison (lavapipe, 1920×1080 RGBA, FPS mode/no power row, original 3× font scale, five alternating
baseline/current processes of 1,000 warmed records): median-of-run-medians CPU recording **18.044 → 11.007 µs**,
Vulkan timestamp interval **6.901 → 22.918 µs**. Run medians ranged 17.761–18.200 / 10.872–11.339 µs CPU
and 6.794–7.084 / 21.308–26.484 µs Vulkan. Static CPU row caching reduces recorder work; the larger opaque
transfer has a measurable **~16 µs** software-Vulkan cost (about 0.05% of a 33.3 ms frame), accepted to preserve
the existing handheld-readable font size. This is **not** a claim of zero GPU-cost increase or Adreno timing.
The game's final image overwrites the overlay destination every frame, so GPU transfers of unchanged labels
cannot be skipped without a separate retained layer/pass; only CPU staging pixels are cached.
The matched harness and logs are under `.scratch/overlay-res/` (`bench-results.json`); `vkoverlaytest --bench-record`
exercises the same warmed recording/timestamp path. Per-sample submit/wait is outside the measured CPU interval;
these host timings do not include Android compositor/scanout or prove device cost.
The host acceptance matrix reused `.scratch/thorcand6/run.py` in both default and
`TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2` arms, including TSan and the expected default-arm atlas-UV
negative control; Python passed 201 tests. Overlay coverage includes all corners/modes, both operation modes,
SMAA with off/FSR1/SGSR, guest/native output transitions, synthetic power sources, and a TSan presenter run in
each arm. The overlay fixture is explicitly compiled for game 1.4.3 so its native-UI cases exercise the version gate.

## The picture

`TOTK_VK_PRESENT=copy` explicitly selects GPU readback into one of three host-visible buffers; a
presenter thread writes it into the window with `ANativeWindow_lock`. This works with any driver, including a Turnip
loaded directly as a HAL module. Both presenters keep fixed-size picture buffers: `TOTK_APP_WIDTH` pixels wide
(default 1280), with the window's aspect ratio. The game's changing crop is scaled into that picture with black bars
where needed, without reallocating for dynamic resolution; the compositor scales it to the screen.
`TOTK_NATIVE_UI=1` below keeps the 720p guest canvas but selects native host UI/window buffers; host spatial upscalers
also select window-sized front buffers but upscale the already-composed game picture instead.
Android now tries the AHB path by default following the owner's validation on both devices, including background/resume.
Missing capability, explicit-layout import or SurfaceControl setup keeps the automatic copy fallback. Explicit
`TOTK_VK_PRESENT=copy` remains the baseline/control; the predictor startup and native activity select the same policy.

**Android default, with automatic copy fallback:** unset/empty `TOTK_VK_PRESENT` or `TOTK_VK_PRESENT=ahb` allocates three `AHardwareBuffer`s
(`GPU_FRAMEBUFFER | GPU_SAMPLED_IMAGE | COMPOSER_OVERLAY | CPU_READ_RARELY`) and imports their native dma-buf
into the same Vulkan device using `VK_EXT_image_drm_format_modifier`, explicit `DRM_FORMAT_MOD_LINEAR` (zero),
and `AHardwareBuffer_describe().stride * 4` byte pitch. It renders the final scale into that single-plane RGBA8 image.
A SurfaceControl child of the activity's native window gives those buffers to SurfaceFlinger. There is no image-to-
host-buffer GPU copy, mapped-memory invalidation, `ANativeWindow_lock`, or CPU pixel copy on this path. This can let
the hardware compositor scan the picture out, but neither overlay assignment nor a power saving is guaranteed.

Why this path rather than a swapchain: Android's Vulkan loader implements its WSI, including swapchains; the bundled
Turnip is opened directly by `vk_load_driver`, outside that loader. A loader entry point cannot safely take a Vulkan
device created by a different dispatch chain. Using it would require routing the custom HAL through a loader or
implementing Android's private native-window buffer-queue ABI. Explicit dma-buf import plus the public NDK
[SurfaceControl API](https://developer.android.com/ndk/reference/group/native-activity) (API 29+) avoids either
change. Obtaining the dma-buf requires the VNDK `AHardwareBuffer_getNativeHandle` accessor: it is dynamically
resolved from `libnativewindow.so`, never linked. If the app's linker namespace hides it, the experiment uses copy.

**API 33 / NativeActivity parent compatibility:** A successful AHB import does not prove that the window can parent a
SurfaceControl. NativeActivity uses `Window.takeSurface`, not a SurfaceView. On some Android releases its root
buffer producer lacks the SurfaceControl handle required by `ASurfaceControl_createFromWindow`
([upstream API 33/34 report](https://github.com/android/ndk/issues/1983)); repeated attempts on that same producer
do not repair the missing handle. SurfaceControl functions here are directly linked NDK APIs, not dlsym-loaded.
An absent mandatory symbol would prevent loading the app library, not return a null surface after a successful import.

For opt-in AHB mode the UI thread also creates an attached parent using public Java APIs: the decor view's
`getRootSurfaceControl()` (API 31), a named `SurfaceControl.Builder`, `buildReparentTransaction`, and a `Surface`
wrapping that control. `ANativeWindow_fromSurface` exposes a parentable native window. If creating a child from the
original NativeActivity window fails, the presenter creates its child from this attached parent instead. This works
without the API-34-only `ASurfaceControl_fromJava` or hidden `mNativeObject` fields, and follows the actual activity
display rather than selecting display 0 (important for Thor's two displays).

JNI preparation and native creation log their exact stages, API level, actual decor display ID (or -1 if unavailable),
window dimensions/attachment state, native pointers and return values. Native creation additionally logs the linked
symbol provider and errno, explicitly labelled as diagnostic only: the API does not specify errno as its status.
The transaction's first configuration/completion is logged separately, including desired-present-time; setters and
apply return void. If the root is not attached yet, normal window/focus callbacks retry preparing the optional parent;
there is no sleep/poll loop. Native surface-creation failure selects copy only for that window generation, allowing a
later attached parent to recover. Window teardown detaches/releases the Java parent, while the presenter retains its
own native references and releases the displayed AHB using the existing transaction/release-fence protocol.

The image uses RGBA8 UNORM storage with compatible UNORM and sRGB attachment views. A small fullscreen shader
samples the source and writes to the matching view: sRGB is decoded and
encoded exactly once, as with the old GPU blit, and alpha is forced opaque like the RGBX window. The SurfaceControl
uses the normal sRGB dataspace and no orientation transform. Shader sources and their embedded SPIR-V are
`gfx/vk_present.vert`, `gfx/vk_present.frag`, and `gfx/vk_present_spv.h`.

The CPU-read usage is an **allocation constraint**, not a CPU readback: no AHB lock/map/read is performed. It asks
Qualcomm gralloc for an uncompressed, CPU-readable layout and avoids sharing generation-specific UBWC storage with
the system compositor. The earlier GPU-only allocation worked on Thor/Adreno 740 but produced vertical noise bands
on Odin 3/Adreno 830 with Android 15 and Turnip `27.0.0-devel f7c93a1352`, including compositor screenshots/video with
postprocessing disabled. CPU-readable allocation plus `VK_IMAGE_TILING_LINEAR` still produced noise on Odin 3.
Mesa `f7c93a1352`'s deferred AHB-handle import overwrites that tiling and discards caller modifier structs
(`src/vulkan/runtime/vk_android.c:367–372`), then initializes from gralloc's modifier and stride
(`src/freedreno/vulkan/tu_device.cc:3833–3857`). Fallback gralloc can return `DRM_FORMAT_MOD_INVALID`
(`src/util/u_gralloc/u_gralloc_fallback.c:144–155`), which leaves Turnip tiled/possibly UBWC despite matching row pitch.
The [Qualcomm allocator policy](https://android.googlesource.com/platform/hardware/qcom/sm7250/display/+/66adc40e72c34e8db9ed2e6a9ac85a0f2fed272a/gralloc/gr_utils.cpp?format=HTML)
permits optional UBWC only without CPU-access usage for ordinary RGBA formats. Explicit LINEAR dma-buf import
bypasses Mesa's deferred AHB-handle path; Turnip selects `TILE6_LINEAR` and disables UBWC
(`src/freedreno/vulkan/tu_image.cc:708–715`). This may cost framebuffer bandwidth versus compression but remains
GPU-to-compositor zero-copy. The exact gralloc backend used by the prior run was not logged.

Startup logs each AHB's actual width/height/layers/format/usage/pixel stride, followed by `path=dma-buf-explicit`,
tiling, returned modifier, plane/fd counts, byte row pitch, offset, size, allocation size and selected memory type.
The one RGBA plane uses fd 0 of the native handle, as Turnip's own AHB importer does; extra fds are gralloc metadata.
No vendor-specific handle integers or UBWC flags are parsed. Vulkan takes ownership of a duplicate fd, and the AHB
stays referenced until its imported image/memory is destroyed. Modifier must be zero, offset zero, row pitch exactly
`stride*4`, and both the Vulkan memory requirements and image layout must fit the fd allocation. The fd's memory
type bits are intersected with the image requirements before import.
AHB memory-handle import is deliberately not a fallback: Mesa replaces an explicit modifier there too. Missing
modifier/dma-buf/foreign-queue support, unavailable native handle access or incompatible layout selects copy.
Recheck both Thor and Odin with AA/scale disabled first: retain all three allocation/layout logs and require clean
display, screencap and screenrecord without a copy-fallback line. Then repeat with combined postprocessing,
sRGB gradients, letterboxing, background/resume and window recreation. Explicit import/presentation works on Odin 3;
the API-33 attached-parent workaround still requires a Thor device run.

The game's present-interval pacing and 60 Hz VI event are unchanged. The recording thread only enqueues; the presenter
waits for GPU completion, submits a transaction, and waits for its completion before submitting another (so ready
frames are not coalesced). A buffer remains busy while displayed. Only the transaction **replacing** it supplies its
previous-buffer release fence; that fence must signal before the buffer can be rendered again. Queue-family ownership
is released to and acquired from `VK_QUEUE_FAMILY_FOREIGN_EXT`. Detaching/recreating the activity window removes the
old child surface and retires its displayed buffer; queued frames for an older window are discarded.

Missing modifier/dma-buf/foreign-queue extensions, native handle access, or allocation/import/view/pipeline failures
log `window present: …; using copy` and select the existing copy path for the rest of the run. Native SurfaceControl
setup failure instead selects copy for the current window generation; new attachment can recover as described above.
Unknown modes also use copy. Keep the default until both target devices have been measured. The existing `window: N frames shown, M not shown
(all buffers busy)` log stays cumulative; a second `window: N discarded (window changed or present failed)` line accounts
for lifecycle and presentation failures. In AHB mode "shown" means a completed SurfaceControl presentation transaction;
copy mode still counts successful window posts.

Measure warmed Gerudo Town at steady 30 fps, same save/camera/driver/brightness/governor caps, 180 s each with `copy`
and `ahb` (repeat in reverse order): system power against the previous ~4.39 W copy baseline, presenter-thread CPU,
`frame … per frame` recording/GPU times, and deltas of both `window:` counters (allow for frames still in flight).
Check orientation, red/blue channels, dark gradients/colours and sRGB, opaque menus, black bars and dynamic-resolution
crop changes; then background/resume and recreate the window. Repeat on Thor/Adreno 740 and Odin 3/Adreno 830.
The startup `window present: ahb (explicit LINEAR dma-buf import, modifier=0; device measurement pending)` and
`window AHB picture … (explicit LINEAR dma-buf, CPU-readable allocation, UNORM/sRGB views, no readback)` lines confirm the requested
path was actually exercised. Also require `window SurfaceControl: first transaction completed` for that generation:
import alone is insufficient. A fallback line disqualifies that run as an AHB measurement.

Present tickets now carry the GPU fence of the frame's work and window copy; reacquiring the same game swapchain
texture waits for it (default on, `TOTK_VK_OFF=present_wait` off; `TOTK_VK_FRAMES_AHEAD=1` waits for the latest ticket
instead of that texture's, default 2). This is independent of the opt-in protected frame-lag policy below. The upstream merge's
combined rendering, fps and power still need device measurement; [UPSTREAM_MERGE.md](UPSTREAM_MERGE.md) lists the A/Bs.

### Native UI v2 without enlarging the post chain (1.4.3, opt-in)

Set **`TOTK_NATIVE_UI=1`** in `totk.env` and restart. Unset/`0` keeps the existing renderer. In **handheld mode**,
unlike v1, v2 retains the game's **1280x720 canvas/crop** and existing world resolution/native DRS. Keep
`TOTK_RENDER_RES=960x540` if desired. Only the final UI canvas and terminal display transfer acquire native-sized
host images (1920x1080 on the Odin 3 and Thor main window). Guest textures, dimensions and heaps stay normal.
In docked mode Android automatically steps this promotion aside and follows the shipping guest canvas as
described in Device profiles; this avoids refusing interpolation for a nonexistent promoted sibling.

The backend follows registered NVN window texture identities and the 1.4.3 final normalized texture-transfer
shader back to its actual canvas backing. The first frame discovers that backing; subsequent frames keep all
pre-UI composition/post draws at 720p, upscale the completed scene **once at the first recognized UI draw**,
then rasterize the remaining UI into the native image. Same-sized bloom/post targets are not promoted.
The original terminal shader still performs the canvas-to-display conversion. Game texture sampling, texel fetch,
storage access, copies and readbacks use an ordered downscaled 1x shadow; only the inspected normalized terminal
transfer samples native pixels. Actual FragCoord.xy users log and execute at 1x, then native rendering resumes.
Such fallback can soften earlier UI and has a real copy cost; it is not silently treated as safe.

`gfx/vk_canvas.c/.h` shares exact 1x image/storage/incarnation/subresource identities, UI shader families,
and discovery-versus-emitted-transfer/present facts with frame generation. It works with native UI off;
when neither feature is requested, detection is a disabled branch with no registry/hash/allocation work.
Depth/camera provenance remains the frame generator's dependency-graph responsibility, not an inferred
association from equal canvas dimensions.

| Interaction | With effective `TOTK_NATIVE_UI=1` (handheld) |
| --- | --- |
| `TOTK_OUTPUT_RES` | Ignored, including stale 1080p/auto values. No game output patch: keep shipping 1920x1080 swapchain allocation, 1280x720 canvas/crop and native DRS. |
| `TOTK_RENDER_RES` / native DRS / `TOTK_DRS_MIN` | Independent, unchanged. No force-docked profile or simulation/fps change. |
| `TOTK_APP_WIDTH` | Ignored. Native siblings aspect-fit the actual window; front buffers follow its extent. |
| `TOTK_POST_SCALE=sgsr` / `fsr` | Bypassed, including sharpening. Do not filter the already-native UI. |
| `TOTK_POST_AA=smaa` | Optional AA on the native output before the final overlay; off for matched text comparisons. |
| `TOTK_SCENE_AA=smaa` | SMAA on the **pre-UI scene**, before optional scene SGSR/FSR; native HUD/text remains untouched. Independent of legacy `POST_AA`. |
| AHB / copy / overlay / companion | Same ownership, pacing and publication protocol. Present samples/copies the native display image directly; overlay remains last. |
| `TOTK_SCENE_SCALE=sgsr` / `fsr` | Opt-in spatial filtering **before** UI. Unlike `POST_SCALE`, this remains active with native UI and locked interpolation; see below. |

### Scene-only SMAA (opt-in, 1.4.3)

Set `TOTK_SCENE_AA=smaa`, `TOTK_POST_AA=off`, and `TOTK_FXAA=0` to replace the
shipping scene FXAA with vendored MIT SMAA 1x HIGH without filtering the HUD.
Unset, `off`, or `0` disables scene AA; unknown values log a refusal. This is a
**new option**: `TOTK_POST_AA=smaa` deliberately keeps its old whole-output
semantics, including UI, so existing users do not silently change behavior.
Enabling both explicitly stacks two SMAA filters and the legacy post filter
still affects UI/generated output; leave `POST_AA=off` for scene-only AA.
Shipping FXAA is otherwise unchanged: `TOTK_FXAA=1` plus scene SMAA is a useful
comparison arm, not a recommended double-AA default.

The exact learned canvas/first-UI boundary and effective native-composition
policy are shared with `TOTK_SCENE_SCALE` below, including handheld/docked
window sizing and the initial canvas-discovery frame. The order is:
game scene-to-canvas composition → crop-sized SMAA edge/weights/neighborhood
passes → optional scene SGSR or FSR EASU+RCAS (otherwise linear native seed)
→ native HUD/text → terminal/present → overlay. This does not intercept the
earlier game render-resolution-to-canvas filter, add temporal AA, or eliminate
moving foliage shimmer. The neighborhood image retains the scene's format,
including floating-point highlights; RG8 edges and RGBA8 weights do not store
scene color. SMAA uses the existing canvas color domain, not a new tone map.

Scratch images belong to the native canvas backing and retire with its normal
GPU ticket. The once-per-real-frame seed guard excludes later UI shadows and
partial patches. Locked/interpolation camera mode retains the filtered native
real scenes and reprojects them, never re-running SMAA or the spatial upscaler
on generated frames. With scene AA enabled, QCOM and LSFG also capture the
filtered real native scene **before UI**, rather than unfiltered low-resolution
input; their generated pictures are composed with native UI without a second
AA/upscale. External-engine capability/ownership refusal remains authoritative.
Plain rendering and locked camera/QCOM/LSFG are the comparison targets;
legacy camera extrapolation has a separate warp path and is not qualified by
this scene-AA comparison.
Camera authority masks conservatively include SMAA HIGH's bounded edge/weight
search footprint as well as the scaler kernel (44 canvas texels versus the
scale-only four); this can increase held-real coverage or refuse more pairs at
the unchanged unknown-depth threshold. No extra midpoint filter is introduced.

Host acceptance: `TOTK_SCENE_AA=smaa TOTK_POST_AA=off` with
`vkposttest --scene-aa` checks smoothed scene diagonals and 384 bit-identical
native HUD pixels; repeat with `TOTK_SCENE_SCALE=sgsr` and `fsr`.
`vkposttest --scene` exercises nonzero crop/DRS origins. The framegen
`--scene-scale` / `--scene-scale-drs` fixtures compare real/generated identity pixels
and assert one SMAA execution per real frame. `--scene-locked` / `--scene-locked-drs`
repeat those checks in actual locked adaptive mode with completed-query intervals
replaced by deterministic test values; this is pixel/ownership evidence, not a
headroom measurement. Request camera, QCOM and LSFG separately. External engines
genuinely refuse unsupported host interop, so those arms establish camera fallback,
not active Android QCOM/LSFG behavior.
`vkposttest --scene-benchmark 1280x720 1920x1080 120`
measures fenced lavapipe GPU timestamps with the same AA/scale controls; software
Vulkan cost is not Adreno performance evidence.

### Scene-only spatial scaling (opt-in)

`TOTK_SCENE_SCALE=sgsr` replaces the linear **pre-UI canvas seed**, not the final
composed picture, with the vendored BSD-3-Clause SGSR1 filter. `fsr` selects MIT
FSR1 EASU followed by RCAS; `TOTK_SCENE_SHARPNESS=0.2` controls RCAS (0..1,
same attenuation mapping as `POST_SHARPNESS`). Unset, `off` or `0` leaves every
existing mode unchanged. Unknown values log a refusal and retain the existing path.
SGSR uses the existing `TOTK_POST_SGSR_SHARPNESS`, `...EDGE_THRESHOLD` and
`...EDGE_DIRECTION` controls; they never sharpen UI in scene mode. Floating-point
scene targets retain highlights rather than clipping to an RGBA8 intermediate.

This opt-in requires the 1.4.3 canvas profile and enables **effective native host
composition**, even with `TOTK_NATIVE_UI=0`; it does not change the file on disk.
Android retains the shipping guest canvas/DRS and uses window-sized host output
(`OUTPUT_RES=0`, `APP_WIDTH=1920` process policy). In handheld that is normally a
1280x720 pre-UI canvas; docked retains its shipping 1920x1080 canvas. Equal-size
docked host siblings are supported, but provide no extra scene detail and cost
an additional filter. No device-name policy is used. Unsupported format/filter
capabilities or pipeline creation log a refusal: the real scene keeps its linear
seed, and generated work refuses rather than silently using a different filter.
The first discovered frame can still use the original path until canvas identity
has been learned through the terminal transfer.

**Important limitation: this is not a render-resolution-to-window intercept.**
The game still performs its own scene-to-canvas composition/upscale before the
host filter. The often-cited `a3a570eb` is the older display-referred twelve-tap
EASU-style shader; 1.4.3 uses HDR `ac58fc18`. Earlier handheld composition also
fuses scene sampling, bloom and tone mapping. Replacing those stages with an
UNORM post shader would change effects/color processing. Thus reduced handheld
input has two size-changing resamplings: game render resolution→720 canvas, then
SGSR/FSR→native content extent. Native UI and terminal/present follow at equal
size; aspect-fit bars remain outside the filtered image. Native/1x fallback
resolves and partial patches retain their existing semantics; **only the first
pre-UI seed per frame is spatially filtered**, never an already-composed shadow.

Locked/interpolation mode **upscales only once per real frame**. Immediately
after the successful pre-UI seed, it retains those native scene pixels in the
existing fence-protected source ring. The midpoint shader samples the two
already-upscaled real scenes at native resolution and composes native affine UI
in the same draw; **no midpoint SGSR/FSR or separate affine pass** is recorded.
Depth stays at its original lower resolution, with independent x/y pixel-center
mapping through normalized UV. Camera interpolation omits the redundant low-resolution color
preservation copy; its logical canvas extent remains for mapping. Active LSFG retains that copy as its
low-resolution input; QCOM captures the original pre-UI subresource independently. Their copy costs remain
inside real-ticket GPU accounting. Missing/duplicate native retention or mismatched resized UI refuses the
pair rather than binding stale/unfiltered color. Camera extrapolation retains its separate
low-resolution warp→spatial-scale→frozen-UI path.

Source images retire with their existing backing/source-ring tickets; no
per-frame CPU wait or extra queue submit is introduced. A separate
`real.retained-native-scene-copy` timestamp charges preservation; whole
real/synthetic submission timing includes it in adaptive locked headroom.
Spatial/RCAS support is baked into a conservative per-pair depth-mask halo,
instead of an 8x8 final-output mask search. The actual encoded-real fallback
remains last; the halo can increase held-real coverage near unsafe depth.

FSR scene interpolation reserves **one extra bounded presenter phase buffer**:
7/8/9 slots at lead0/1/2, versus unchanged 6/7/8 for scene-scale off or SGSR.
Plain rendering/extrapolation stays at 3/4; source histories stay at four.
This is not an FSR scratch-image ownership leak. Supplied scene-upscale2 Odin
captures showed seven legitimate owners at lead1: one pair-held real, three
queued GPU-ready pictures and three CURRENT/future compositor owners. The
two-stage real filter exposed a production/readiness/display phase where none
could be safely reclaimed for the new child. Callback completion alone is not
PREVIOUS release. Scene-upscale3 adds only that opt-in phase capacity; it does
not retime targets, relax GPU admission or reuse compositor-owned storage.

An extra 1920x1080 RGBA8 presenter picture is **8,294,400 logical bytes
(7.91 MiB)**. If that slot also creates a generated output image, the two
RGBA8 payloads total **16,588,800 bytes (15.82 MiB)**. AHB stride/alignment,
Vulkan allocation overhead and optional overlay/post storage are additional;
actual image allocations remain lazy. No extra FSR intermediate/source history
is allocated because of this slot. Startup `[framegen-slots]` reports
`FSR-scene-phase=1`; existing DIAG exhaustion snapshots remain available.
`presentslottest` permanently models GPU readiness, two future transactions,
ordered PREVIOUS release and four-source references: the FSR phase starves at
N slots and clears at N+1 for each lead, while the ordinary/SGSR phase and
true-overload refusal remain unchanged. This host model is not a device cadence
qualification or a bound on arbitrary compositor delays.

scene-upscale3 host acceptance: **239 processes, no unexpected results**, both
startup arms, full previous scene/DRS/UI/framegen matrix plus timed presenter
slot/fence regressions, normal and TSan. The expected default atlas-UV negative
control remains. Python: **202 tests, 23 skipped, no failures**. The timed pool
regression generates 580/580 at the extra FSR capacity for each lead and retains
the N-slot starvation and true-overload negative controls. This is not a claim
that the new APK has already been qualified on Odin.

Compare this with **docked + `TOTK_RENDER_RES` and scene scaling off**: the game's
HDR EASU can already write directly to the 1920x1080 canvas with native guest UI
(one size-changing resampling). This is a genuine alternative, not an inferior
baseline to hide. The handheld path pays an extra spatial filter and native
composition transfers, while its earlier effects run on fewer pixels; net mobile
GPU cost and quality require matched device measurements. Lavapipe numbers are
relative host timings only, not Thor/Odin admission or performance promises.

Example Thor locked handheld (choose **one** render-size line):

```ini
TOTK_OPERATION_MODE=handheld
TOTK_OUTPUT_RES=0
TOTK_RENDER_RES=1152x648
# Alternative: TOTK_RENDER_RES=960x540
TOTK_NATIVE_UI=1
TOTK_SCENE_SCALE=sgsr
TOTK_POST_SCALE=off
TOTK_POST_AA=off
TOTK_FPS60=0
TOTK_FPS_AUTO=0
TOTK_REALTIME=1
TOTK_UNCAPPED=0
TOTK_FRAMEGEN=locked
TOTK_VK_PRESENT=ahb
```

Odin can use the same lines, initially at `TOTK_RENDER_RES=1280x720`; compare
`TOTK_SCENE_SCALE=fsr` plus `TOTK_SCENE_SHARPNESS=0.2` against SGSR. For plain
rendering change only `TOTK_FRAMEGEN=0`; use the desired existing FPS policy.
For the single-resample comparison choose `TOTK_OPERATION_MODE=docked`,
`TOTK_NATIVE_UI=1`, `TOTK_SCENE_SCALE=off`, `TOTK_OUTPUT_RES=0` and
`TOTK_APP_WIDTH=guest`; Android's existing docked policy uses native guest UI.
Do not mix the two chains when comparing image quality or GPU time.

Startup evidence: grep `scene-scale policy`, `[scene-scale] active` or
`[scene-scale] refusing`; the active line records render setting, canvas/crop,
native extent, filter and RCAS strength once.
`[scene-scale-framegen] retaining real upscale once` records native scene and
low-resolution depth extents once; it explicitly confirms no midpoint filter.
`TOTK_FRAMEGEN_DIAG=1` enables existing `framegen headroom`/`framegen budget`/
`[framegen-profile]` diagnostics; normal runs add no
periodic scene-scaling diagnostics. Host coverage uses `vkposttest --scene`,
`vkposttest --scene-benchmark 1280x720 1920x1080 120` (also run scene-scale off
for the linear baseline), `vkframegentest --scene-scale`, `--scene-scale-drs`,
`--scene-benchmark` (fenced midpoint/replay/output subspans), and the existing
native UI/authority fixtures. Synthetic sharpness metrics are not perceptual game proof.

scene-upscale1 host acceptance (2026-10-27): the reused two-arm matrix plus scene/native UI/
framegen and TSan extensions completed **215 processes, no unexpected results**;
the default-arm atlas-UV race remains the expected negative control. Python:
202 tests, 23 skipped, no failures. Synthetic edge/text-like Laplacian-energy
ratios against bilinear were **1.80–2.41x SGSR** and **1.90–2.46x FSR**, with
crop-origin/DRS invariance, non-16:9 aspect fit, exact opaque native UI and
real/generated static-scene parity. The retained-native design also preserves
aligned scene texel fetches for exact identity reprojection in scene mode only;
default generation sampling remains unchanged.

Lavapipe/Mesa 26.0.8, LLVM 21.1.8, `LP_NUM_THREADS=0`, RGBA8 synthetic color
scene, 1280x720→1920x1080, 60 warmup + 120 measured GPU timestamp samples:

| Pre-UI seed | Mean ms | Delta vs linear | Relative |
| --- | ---: | ---: | ---: |
| Linear | 7.070 | — | 1.00x |
| SGSR1 | 29.422 | +22.352 | 4.16x |
| FSR1 EASU+RCAS | 95.818 | +88.748 | 13.55x |

These measure **only the host seed**, not game HDR EASU, native UI or complete
framegen, and exclude uploads/readbacks/CPU waits. They do not establish a mobile
GPU delta versus the single-resample docked chain. That net delta is the extra
host filter/transfers minus any savings in lower-resolution game effects; its
sign and magnitude remain unmeasured against the docked chain.

Main's Thor scene-upscale1 run (locked lead1, lag0, native UI, still village
spawn) found 960x540/off at real 25.5 ms + incremental 1.37 ms with 300/300
generated; SGSR at 960x540 had real 26.9–27.7 ms + incremental 7.0–7.1 ms and
300/300 budget skips. 1152x648+SGSR had real 29.8–30.5 ms + incremental 6.8 ms
and no generated frames. Those supplied device observations motivated retaining
the real upscale for interpolation instead of filtering every midpoint. They
are not scene-upscale2 results and do not justify relaxing locked admission.

scene-upscale2 retained-native acceptance: **231 processes, no unexpected
results**, including both startup arms, all previous cases, unequal
depth/canvas dimensions (480x268 depth, 640x360 canvas, 960x540 native), requested
native UI 0/1 and SGSR/FSR, normal and TSan. The expected default-arm atlas-UV
negative control is unchanged. Python: **202 tests, 23 skipped, no failures**.
Static real/generated pixels, opaque UI and clipped native clears still match;
missing native scene retention is explicitly rejected.

Matched scene-upscale1/2 lavapipe comparison uses the same synthetic RGBA32F
1280x720 scene and depth, 1920x1080 native output, an unknown-depth authority
span, 16 generated warmups and 60 measured pairs per filter. The baseline
backend/shaders are commit `347b53f`, with only the shared test driver and a
read-only timestamp getter added. GPU timestamp means:

| Midpoint work | scene-upscale1 ms | scene-upscale2 ms | Relative |
| --- | ---: | ---: | ---: |
| SGSR replay + final output | 2414.303 | 1203.621 | 0.499x |
| FSR replay + final output | 1707.786 | 767.614 | 0.449x |

The new retained-native copy costs 6.859 ms (SGSR run) / 4.448 ms (FSR run) per
real frame on this host. Charging one copy per generated pair gives 1210.480 /
772.062 ms, respectively: **49.9% / 54.8% less** than the previous measured
subspan sum. These are deliberately heavy software-rendered fixtures, **not
Android frame times** or complete game/queue costs. Native midpoint composition
itself is more expensive (SGSR 382.662→883.945 ms; FSR 350.168→559.233 ms), while
removing the midpoint filter/separate affine pass and the old final-output
8x8-mask search reduces the total. Different SGSR/FSR runs are not a valid
cross-filter ranking. Real upscaling remains once per real frame, outside this
incremental comparison; the mobile result and net delta versus docked remain
unmeasured. Locked admission still charges actual whole-ticket GPU time.

**Cost model.** v1 multiplied every canvas-sized pass by 2.25 pixels at 1080p. V2 leaves those passes at 720p
and pays one scene upscale, native UI coverage and the terminal transfer. A full-screen UI effect after the
boundary still costs native pixels; reads/fallbacks add down/upscales. The existing Lookout capture has one
full-screen scene/bloom composite (p0135, one draw), then UI additions p0137/p0143/p0145 (3/36/142 draws).
Exact changed-pixel counts in its packed B10G11R11 canvas are 1,139 / 6,448 / 43,026 respectively, versus
921,600 for p0135. Changed pixels are not rasterized coverage or a GPU-time multiplier. Interleaved p0136,
p0138–42 and p0144 are small 96/48/24/36/192-pixel icon/minimap producers, not full-screen canvas post passes.
Those stay 1x. Menus/camera may have different work and must be measured.

Native pixels must survive partial redraws and alpha blending, including across real frames and host resizes.
The window sibling remains valid across frames: it is **not** unconditionally seeded every present. A seed is
needed on first use, size/basis changes, or after a 1x write. Likewise, a previous-frame shadow resolve is required
before a partial 1x update or game read. Only a proven whole-image queued color load-clear (single color, no depth,
base single-level/layer/sample, unchanged format and no guest-only padding) may replace that preservation work.
Such clears now avoid the old resolve/seed; partial draws and clears retain both. A full padded-window clear runs
at 1x so it also defines guest-only padding; a later native draw seeds from that complete result.
At 720p/1080p with four-byte texels, each full seed/resolve reads+writes **11.98 MB of logical texels**, plus its
existing barriers. This is not measured physical memory traffic or a claimed GPU-ms saving.

Every 300 presents `native UI work per frame: <operation> <count>, full-clear avoided <count>` reports actual
canvas/window seeds, shadow resolves, region patches, native UI/window passes and host presentation/post work.
In a **separate diagnostic run**, `TOTK_VK_PASS_PROFILE=300` directly timestamps `canvas-seed`, `window-seed`,
`shadow-resolve`, `region-patch`, `ui-pass`, `window-pass`, `present-ahb`/`present-copy` and `host-post`.
Transfer scopes include their existing barriers; presentation includes overlay and AHB acquire/release or copy
readback. UI/window timings also appear in render-pass totals and must not be added twice. `dropped scopes`
must be zero for complete attribution. Unscoped time still includes GPU idle and is **not** a copy/barrier timer.
Profiling disables secondary recording on sampled frames; compare normal-mode fps separately.

No new HeapDef overlay or app-pool growth is required. Each 1080p packed B10G11R11 or RGBA8 sibling adds
7.91 MiB per live backing while retaining the 1x shadow; formats with eight-byte texels cost 15.82 MiB.
Existing native front-buffer reservation is unchanged relative to 720p+SGSR: three AHB allocations are
23.73 MiB, or three copy readbacks plus a shared image 31.64 MiB, before alignment/window buffers.
Optional SMAA has its existing additional intermediates. This is a bounded-cost design, not a promised
Odin fps gain, zero-copy game rendering, or a cached UI layer.

The owner's first v2 Thor 30 fps run (`f1e01f7`, APK `4be846aa`) confirmed crisp, correctly placed HUD/minimap/hearts
and correct gamma, but recording rose **1.7–3.0 ms/frame** (26.1/25.5 off, 27.8/28.5 on); long-interval share
rose from 4.7/5.4% to 7.6/9.3%. That is not a throughput acceptance. The recorder repair now avoids eagerly
resolving every ordinary descriptor or breaking ordinary 1x feedback passes: a registration-maintained promoted-storage
filter and cached shader/bound-slot masks gate native work. Window identity is cached and open-pass attachments are reused.
This repair still needs the same device A/B; host ns/draw is not a Thor/Odin performance result.
**Host checks (no game files):**

```
PATH=<host-tools>/usr/bin:$PATH make -C games/totk/android -j8 -s \
  vktest vktest-app vktest-overlay TARGET=linux CC=clang OUT=<scratch>
TOTK_SHADER_SEED_DIR=<scratch>/vkdraw-seed \
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json TOTK_NATIVE_UI=1 <scratch>/vknativeuitest
# Repeat with TOTK_NATIVE_UI=0 and with TOTK_VK_REC_THREADS=2.
# Recording-only off-canvas loop, after a promoted canvas exists; fresh on/off processes:
TOTK_SHADER_SEED_DIR=<scratch>/vkdraw-seed \
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json TOTK_NATIVE_UI=1 \
  <scratch>/vknativeuitest --offcanvas-bench
```

The native fixture exercises real scene+UI rendering, 1.5x host extents, distinct adjacent UI texels, scaled
viewport/scissor/clear/upload regions, aliased 1x readback, FragCoord.xy fallback and sibling resize.
It also checks late descriptor aliases/re-registration and that ordinary 1x aliases add no native pass boundaries.
Full-clear counters and pixels also cover avoiding previous-frame resolves, first-use/resized seeds, retaining
partial redraw/alpha backgrounds on first use and resize, guest-only window padding and aspect-fit extents.
Set `TOTK_VK_PASS_PROFILE=1` on the native fixture to exercise/report completed operation timestamp queries.
The opt-in benchmark reports median/min/max walking-thread CPU ns/draw and actual draw/extra-pass counts;
repeat with native UI off and recorder threads 0/2. It excludes compilation, initial attachment setup and GPU waits.
Existing draw/presenter tests remain controls for the off path. `scripts/verify_v143_native_ui.py` checks
that native mode leaves game output words untouched, with the independent 960x540 scene patch still valid.
None of these is a real gralloc/SurfaceControl or game menu/camera test.

**Owner device qualification (not executed by this change):**

1. Same signed build, warmed driver/seed/save/camera/weather/thermals, handheld, physical 1920x1080 window,
   framegen off; Odin 3 GPU capped near 660 MHz. Use `TOTK_FPS60=1` on Odin; `0` on Thor. Fix recorder/lag
   and pacing settings across each pair. Warm once, measure >=180 seconds each arm in Gerudo including the
   same camera sweep, repeat in reverse order.
2. Common `totk.env`: `TOTK_OUTPUT_RES=0`, `TOTK_RENDER_RES=960x540`, `TOTK_POST_AA=off`,
   `TOTK_POST_SCALE=off`, `TOTK_FRAMEGEN=0`, `TOTK_FPS_AUTO=0`, `TOTK_OVERLAY=full`,
   `TOTK_VK_PRESENT=ahb`, `TOTK_FRAME_TIMELINE=0`. Compare only `TOTK_NATIVE_UI=0` versus `1`.
   Then repeat with stale `TOTK_POST_SCALE=sgsr` to verify bypass. Keep the complete startup log:
   require 720p canvas/crop, identified canvas backing/native sibling, native presenter extent and no AHB fallback.
3. Keep every warmed 300-frame fps/stutter/recording/GPU-ms window, shown/dropped count, present-fence bins
   and p50/p95/p99/coverage (16.7/33.3/50/>50 ms), clocks, temperatures, power and resident/image memory.
   Fix the game's GPU-time reporting policy across both arms (use `TOTK_GPU_TIME=hardware` when the signed
   build includes that separate change); do not conflate controller interval changes with native-copy timing.
   Retain the new per-operation counts/avoided counts, then obtain their direct GPU milliseconds in step 5.
   Present-fence observations are not automatically distinct-picture cadence; retain screenrecord observations
   separately. Repeat `TOTK_VK_PRESENT=copy`, then background/resume, window recreation and companion on/off.
4. For matched image crops disable overlay and retain full screenshots plus 1:1 hearts, minimap, text,
   inventory/menu crops. Check sharpness, size/position, alpha, gamma and clipping, not just fps. Exercise
   pause blur, inventory, map/zoom, camera rune preview/photo/save and screenshot/readback transitions.
5. Diagnostics are a **separate run**, not performance evidence: add `TOTK_VK_PASS_PROFILE=300`,
   `TOTK_FRAME_TIMELINE=1` and `TOTK_INPUT_FILE=<writable-command-file>`; after reaching each screen append
   `passdump 1` to that file. Keep `passdump/f<frame>/index.txt` and PNGs plus the log/timeline. For complete
   binary captures choose `TOTK_VK_PASS_DUMP=<frame>,0` before restart, then on the host run
   `python3 games/totk/android/passdump_png.py <dump-directory>`. Capture world, pause blur, inventory,
   map and camera/photo/save. Existing Lookout data does not cover these read paths; new captures must show
   which native target is read and any coordinate fallback while confirming scene/bloom remain 1x.



### Optional camera-generated 30 → 60 presentation

| Switch | Effect / default |
| --- | --- |
| `TOTK_FRAMEGEN=interp` | Selects true frame interpolation instead of extrapolation: real frames remain 30 Hz, output targets physical 60 Hz, and each interpolated picture is one real frame delayed. UI is composited from the older frame in each pair. |
| `TOTK_FRAMEGEN=locked` | Explicit fixed-real30 interpolation with exact total-GPU admission/completion and GPU-evidenced feedback cooldown; requires `FPS60=0 REALTIME=1`, capped fixed-slot 60-Hz timing and no budget override. Use the Device profiles timing block, not together with hybrid; frame lag may be0 or1. Seven/eight bounded slots at lead1/2 (eight/nine only for FSR scene scaling) and two backpressured future transactions (API31 runtime symbol; otherwise serial fallback). `TOTK_FRAMEGEN_LEAD_REFRESHES=1` (default) /2 adds about50 /66.7ms total image latency at60Hz. `TOTK_FRAMEGEN_BACKPRESSURE=0` is diagnostic-only, unsafe for lossless async presentation. No steady60 promise when real/GPU/display deadlines cannot be met. |
| `TOTK_PRESENT_TIMELINE`, `TOTK_PRESENT_FLOOR_NS`, `TOTK_PRESENT_DEDUPE`, `TOTK_PRESENT_GUARD_NS` | Independently default-off qualification controls for locked AHB interpolation; see [Experimental API timeline and timestamp corrections](#experimental-api-timeline-and-timestamp-corrections-default-off) for exact behavior and limits. |
| `TOTK_FRAMEGEN_ENGINE=qcom` | Experimental locked-only Snapdragon GLES frame extrapolation; half-tick prediction from two real pre-UI scenes, current affine UI, no next-real wait. Opt-in; default/empty/`camera` retain the existing engine. Background AHB/AFME prewarm; runtime AHB/EGL/SYNC_FD/extension/timer checks; admission charges max(GL elapsed, native-fence span). Recoverable timing/deadline incidents use camera cooldown, with300-scene-tick startup exemption from permanent escalation. Capability/import failures or sustained warm failures log permanent camera fallback. Expected active-QCOM lead1 image delay16.7ms instead of50ms at60Hz; camera cooldown/fallback restores50ms. |
| `TOTK_FRAMEGEN_ENGINE=lsfg` | Experimental locked-only LSFG3.1 midpoint, older affine UI and existing locked timing. User's private purchased DLL required; no proprietary bytes distributed. Borrows game VkDevice/queue and pre-UI scene snapshots, writes HDR storage output directly; no engine AHB transport/cross-device handoff. Retained contexts/descriptors, per-pass GPU timestamps, correlated eight-tick admission and unchanged30ms completion/deadline recovery. |
| `TOTK_LSFG_PERF=0/1` | Normal (default0) or reduced-channel performance LSFG3.1 model from the same private DLL. Universal option; no device-name switching. |
| `TOTK_LSFG_FLOW=0.5/0.75/1.0` | Flow width/height relative to pre-UI scene, default0.5. Does not change output or UI extent. Invalid values refuse LSFG rather than silently substituting a model. |
| `TOTK_LSFG_PROBE=1` | Exact-literal, default-off LSFG provenance and after-mipmaps R8 snapshots for the native dump-selected generation; requires the separate DIAG/dump and LSFG opt-ins. Private pixel/binding metadata only. Blocking/perturbed diagnostic, not a play arm or synchronization fix; unmatched CB submission attribution remains unknown. |
| `TOTK_LSFG_UBO_REFRESH` | **Default on**; exact `0` opts out (other values refuse). Right after the first LSFG timestamp of each recorded pair, re-writes every pooled LSFG 48-byte constant buffer and each pass-owned fractional phase buffer from its creation bytes with `vkCmdUpdateBuffer` (plus buffer barriers); fractional dispatches still update only their timestamp afterwards. Logs `[lsfg] ubo-refresh:`. Workaround for Odin (Adreno 830) constant buffers observed overwritten after upload by an unidentified writer; not a root-cause fix. |
| `TOTK_FRAMEGEN=hybrid` | Opt-in Odin native60 / sustained real30+interpolation policy; requires1.4.3, `FPS60=1 FPS_AUTO=0`, measured capped timing, fixed slots, `ACQUIRE_SLACK=0 ACQUIRE_CLOCK=0 PACE=0 ASYNC_REPLAY=0`. Two5s windows below55fps enter interpolation;8s controller headroom and10s dwell release it. `TOTK_HYBRID_DROP_FPS`, `DROP_SECONDS`, `RECOVER_SECONDS`, `RECOVER_SCORE`, `DWELL_SECONDS` tune the wall-time policy (all prefixed `TOTK_HYBRID_`). Exact Odin environment, transitions, native UI qualification and diagnostics: `docs/frame-interpolation.md`. |
| `TOTK_FRAMEGEN=1` | Retains the existing camera/depth extrapolation mode; **off by default**. Fixed real30 only: `TOTK_FPS60=0`, `TOTK_UNCAPPED=0`, `TOTK_FPS_AUTO=0`, fixed slots and a physical60Hz panel. |
| `TOTK_FRAMEGEN_BUDGET_MS=<ms>` | Optional positive fixed incremental GPU ceiling. **Unset in interpolation:** adaptive exact real+synthetic headroom, target30ms of the33.333ms real tick. Explicit values retain strict source+generation incremental rejection; extrapolation still defaults to4ms and suspends on over-budget/late output. Remove old explicit4/10 entries to select adaptive interpolation. |
| `TOTK_FRAMEGEN_UNCERTIFIED_PLANE=1` | **Experimental, default off; exactly `1` enables it in interpolation only.** Assumes depth VALUE authority for the two explicitly audited active-shrink VS identities with the exact ecbe FS pair and matching live camera/projection state. This is **not** a geometric proof or error bound; artifacts remain possible. Always emits a loud `UNCERTIFIED` startup warning and per300-real admitted-pixel/copy/cost summaries, even with framegen diagnostics off. Other unknown final depth values retain masking and the unchanged35% limit. |
| `TOTK_FRAMEGEN_DIAG=1` | Each300 real frames: eligible/generated/refusal/backoff counts plus canvas image/incarnation/size, committed transfer, first-UI nomination, snapshot attempts/successes, roots and last dependency masks/taint/cause counts. State-change summaries plus per-refused-probe resource/root diagnostics described below. On presenter no-buffer refusal, emits one `[framegen-slot-exhaustion]` owner snapshot per300-real group: immutable source/kind/desired/enqueue, atomic apply-entry/callback/PREVIOUS-release-wait times, GPU readiness and whether a slot released during observation. No continuous cadence trace, waits or readback; off in ordinary play. |
| `TOTK_FRAMEGEN_WRITER_DIAG=1` | Separate high-overhead attribution arm, interpolation plus `TOTK_FRAMEGEN_DIAG=1` required; **off by default**. GPU depth-value changes and final residual-unknown ownership are attributed to exact VS/FS/check spans. Startup states on/off. Before/after depth copies perturb performance: leave absent in the timing arm. Overflow/incomplete samples are explicit, not silently claimed complete. |

Interpolation does not increase simulation, input or animation cadence: gameplay remains 30 Hz, and the older-frame UI can visibly retain 30 Hz motion. Invalid resources or an exceeded pair budget present the real frame instead. As with other image-space interpolation, disocclusions and independently moving objects can produce artifacts; this mode is not a substitute for native 60 Hz gameplay.

`interp` retains the adjacent pre-UI scene canvases, D32 depth and checked cameras.
Both source cameras are reprojected into the rigid midpoint camera; per-pixel validity
and midpoint depth select a visible side or blend agreeing sides. A source absent on
one side can be filled by the other; pixels invalid in both keep the older scene.
Real N is held until N+1 is submitted, then N / midpoint(N,N+1) / N+1 target
successive 16.667 ms physical slots. The real timeline remains 33.333 ms; missed
midpoints expire rather than repeating or delaying the next real picture.

Interpolation alone activates **six** lazy presenter/output entries: two real pair
members, two AHB owners (CURRENT and a transaction still waiting for PREVIOUS's
release), one prior queued midpoint and one new midpoint. Four entries can starve
generation even with zero GPU budget refusals; five cannot cover the queued child.
Source history remains four entries. Non-interpolation modes retain three buffers
by default and four with `TOTK_PRESENT_BUFFERS=4`. `[framegen-slots]` reports the
ownership budget and RGBA8 payload; actual AHB stride/alignment and other images
remain separately accounted by the image-memory diagnostics.

A real reservation marked late is **not** inherited by its child. A pair can still
generate until its midpoint's own `real-slot + refresh` deadline, and a completed
child is eligible when its newer source is ready before that child deadline.
FIFO preserves real / midpoint / next-real ordering; AHB checks the child's deadline
again immediately before transaction apply. There is no extra catch-up queue or
additional deliberate real tick of latency.

Adaptive interpolation pre-admits from independent nearest-rank p90 real and
incremental costs over the last32 completed real ticks. Fresh real timing is
required within four ticks. With no fresh measured incremental cost, a bounded
probe can start at most once per30 real ticks when both real p90 and the newest
real cost are below30ms; unknown probe cost is printed as `nan`, not a fictional
zero/10ms estimate. Repeated capability queries do not consume probes: actual
composition work reserves one start per real frame. Missing, nonfinite,
overwritten or incomplete exact-ticket provenance refuses rather than waits.
The existing GPU slot timestamps include uploads and source/replay/authority/pair
work. Only explicitly tagged post-real synthetic ticket ranges are removed from
the real sum, then added once as incremental GPU cost (including synthetic post
and output copy). After completion, this frame's actual nonoverlapping total
must also fit30ms, retaining3.333ms margin to the30Hz tick. Averages, FPS and CPU
completion latency are not GPU-budget evidence. Stale over-budget estimates
eventually permit another rate-limited measurement, not a permanent deadlock.
Signaled AHB present fences at least one refresh after their immutable interval2
real target feed bounded4–120-real-tick cooldown/backoff only when nearby exact
generation reservations exist. Synthetic, missing/teardown and callback/dequeue
times never become real misses; framework fences are not hardware scanout.
`framegen headroom` reports independent p90 provenance/counts, actual probes,
retry/cooldown and real-fence feedback. DIAG additionally reports fenced GPU phase
mean/max/counts for preservation, authority, dilation, UI replay, combined
bidirectional reprojection+UI, terminal, output and presentation paths.


Every 300 real frames `[framegen-display]` reports `ready`, `generated` (recorded
outputs), `completion-accepted`, `handed-off`, `expired`, `no-buffer` and `pending`.
`[framegen-expiry]` attributes every discard to source-missing/source-late,
child-deadline, completion-refused, output, window or post. Completion refusal can
mean budget or depth-authority rejection; the GPU counters distinguish those.
Asynchronous window boundaries can
shift a few pending outcomes; handoff is transaction apply/post, **not scanout**.
For warmed, eligible 30->60 AHB windows require ready/generated/handed-off near
300/300/300, `no-buffer=0`, and near-zero child-deadline expiries; generated minus
handed-off must be explained by expiries plus the change in pending children.
`presentslottest` reproduces Thor's four-slot starvation, Odin's punctual-parent
expiry cascade and genuinely missed child deadlines. `vkframegentest --interp-present`
also allocates/retires actual Vulkan outputs through all six entries with explicit
late-parent/on-time-child and expired-child timestamp fixtures. Host fixtures do
not establish Android scanout cadence: qualify 16.667 ms present-fence intervals
on the operator's Thor/Odin runs with `TOTK_VK_PRESENT=ahb`.

The midpoint uses **N's UI**, unwarped, so it agrees with the preceding real picture
and never leaks future menu/input state. At N the frozen UI packets are replayed
over black and white into private same-format images; their RGB difference retains
per-channel background transmittance. At N+1 composite
`midpoint * (white - black) + black` before the real terminal transfer. This preserves
opaque/text edges and ordinary affine translucent blending without a HUD difference
mask. Each replay has its own copied depth/stencil. Nonlinear destination-dependent
blend operations, transparent world effects and moving objects without motion vectors
are not reconstructed physically; device visual qualification remains required.

Interpolation logs every 300 real frames: synthesized, budget/camera-cut/history/late
skips, extraction refusals, measured incremental GPU mean/max and nominal added
latency (33.333 ms). GPU costs are checked after completion and before midpoint
presentation; a failed pair leaves the delayed real-frame timeline intact.

Canvas discovery is enabled for both `1` and `interp`, independently of native UI
and host AA/scaling. The first Thor interpolation run produced zero canvas/transfer/UI
events because startup admitted only the literal `1`; the corrected startup gate
also admits `interp`. `TOTK_NATIVE_UI=0 TOTK_POST_AA=off TOTK_POST_SCALE=off` is valid;
do not enable those features to mask an extraction failure. A real terminal transfer
must first teach the exact canvas identity, then a later frame's recognized first UI
draw can capture it. Unknown/recycled images and an unemitted/wrong-window transfer
remain ineligible. Nonzero host results are not proof of device synthesis.

With `TOTK_FRAMEGEN_DIAG=1`, each refused canvas-ancestry probe additionally emits
`[framegen-ancestry]` (canvas version/writer and diagnostic cause mask),
`[framegen-unknown]` for taint-origin image versions actually propagated into it,
and `[framegen-root]` mapping every scene-candidate bit to its depth image/subresource,
extent, format, signature, validity and revocation. The `unresolved` mask denotes
scene candidates, **not unidentified textures**. Unknown-resource lines include the
exact image/incarnation, guest storage, mip/layer/aspect, invalidating operation,
its physical-pass position, and the previous tracked writer/version. Writer bit31
denotes an operation ordinal; an untagged writer is a physical raster-pass index.
Diagnostic cause bits are version-specific, reset per real frame and never affect
selection. The fixed 64-entry cause list explicitly reports overflow; an
`untagged-taint` line or nonzero overflow means attribution is incomplete, not proof
that an omitted resource is safe. No diagnostic image is realized on refusal, and
these lines do not run during extraction backoff or when diagnostics are off.
`[framegen-root-camera]` adds exact camera identity/equality against a live candidate
on the same depth subresource, pose/projection/extents, and each root's first
revocation cause, pass, writer and last shader context. Same depth allocation alone
does not authorize merging revoked and live candidates: a clear/replacement may
have destroyed the earlier geometry even when the camera is unchanged.
`[framegen-root-viewport]` adds the camera-admission rejection, viewport, scissor
and depth range at first revocation. Proven continuations reuse a live root by
exact depth subresource and camera, not by color-MRT layout: a depth-only pass or
different G-buffer format does not create a different camera. Three audited VS
variants also expose separate view/projection matrices in UBO5; they can continue
an existing root only when both matrices exactly match it, viewport covers the
native depth image, depth range is0..1 and any fragment-depth export is audited.
The raster/sentinel explicit-depth FS is separately classified. Other exporters
need a per-pixel depth-value certificate rather than a shader-family whitelist.
A second exact audited VS/FS pair can continue a live root when its
export is `max(raster_depth, projected_world_Y_4_plane_depth)`: both stage camera
banks must match the live root (including projection-mode/coefficient fields),
the projection coefficients must prove the same depth convention, and VS UBO11's
clip-shrink gate coefficient must be zero. This is not an unconditional shader
whitelist. The audited material-depth-bias layout additionally requires zero bias.
Unknown layouts, changed matrices, active/unproved clip shrink, cropped viewports
and unclassified depth draws do not gain camera authority. In interpolation, the
first such depth-writing span on one unambiguous live scene root reserves a
source-private ordered VALUE certificate. Each unproven run gets before/after
D32 copies clipped to its conservative native scissor rectangle, outside
rendering. Only VALUES changed inside that run become unknown; ordinary proven
geometry before/between/after those runs does not become unknown merely because
it rendered after the first span. A changed value observed between runs or at
the final capture is certified as a proven update. Restoring the last observed
proven value is also certified; same-value unproven writes preserve existing
authority, including existing unknown state. Colour at equal depth remains the
same non-depth-writing-pass concern, not motion-vector proof.
Copies do not prove actual raster coverage: a proven identical-value write onto
an unknown value, or an unobserved proven change-then-restore, can conservatively
remain unknown. Scissor containment never re-certifies pixels by itself.
Compatible unproven contexts with contained scissors share a bracket; proven
writers, qualification changes, expanding scissors or physical pass ends close
it before subsequent commands. Failed/unsupported copies, stale identities,
unclosed/capacity-exhausted spans, clears, CPU/storage mutations and ambiguous
roots still refuse. No mask creates or revives a camera root.

Both sources retain immutable exact unknown-value prefixes and complete GPU
min/max reductions over **every** shader-valid final depth (signed zero included).
For each actual camera pair, a positive-projective-W interval proof bounds x/y
motion over that complete depth domain. It bounds the polynomial displacement
numerator, including quadratic interior extrema—not just sampled/corner motion.
Nonfinite, singular, horizon-crossing or empty domains retain the independent
0.04-UV shader cap. Each radius is the outward pixel bound plus a native texel
footprint; stationary pairs need only the numerical/texel guard.
Pair dilation uses conservative4×4 native-depth tiles: max-reduce unknown values,
expand the tile's complete native-X extent by the unchanged native radius, then
coarse-Y max and expand back into the unchanged full-resolution known-mask ABI.
Every pixel unknown under the full-resolution reference remains unknown; tile
rounding can only add unknowns/refusals. Partial edge tiles contribute their
actual native-pixel area to the census, not16 unconditionally. Raw unknown values,
motion proofs and the35% threshold are unchanged. This removes full-resolution
pair prefix scans; source exact VALUE/range comparison still retains its prefix.
The retained old source is redilated for the **new** pair, not its prior delta.
Pair masks/census belong to the current source and stay pinned by their output,
so later pairs cannot overwrite an older output's GPU masks or CPU readback.
Every inverse-reprojection, continuity and bilinear color tap checks authority in both sources; an unsafe
tap forces the actual encoded nearer-real-tick pixel, never the other source's
warp. At the midpoint temporal tie the earlier real wins, including its real UI.
`TOTK_FRAMEGEN_UNKNOWN_PERCENT=35` (default;0..100) rejects a pair if either
source's **dilated** unknown fraction exceeds the limit. This is a post-fence,
pre-handoff gate, with no recording-thread readback wait; recorded synthetic work
is not proof of accepted/presented frames. Certificate buffers, mask, count
readback and real terminal capture share the source fence/reference lifetime.
Every300 frames, `framegen authority` reports copies/comparisons, raw/dilated
unknown fraction mean/max, actual pair UV/radius mean/max, coverage skips and
total authority GPU mean/max. State initialization, final raw copy, ordered VALUE
comparison and range reduction/readback are timed separately from summed clipped
span copy/compute brackets; pair dilation/compose are included once
inside replay and the real GPU bracket. Encoded real capture also counts against
the budget; no overlapping dedicated pair-timing charge is added.
`[framegen-root-unproven]` records that first writer separately from the pass's last
context. `[framegen-plane]` reports the conditional proof and `[framegen-plane-coverage]`
counts depth draws and summed/max clipped scissor bounds in each tracked probe
prefix; these overlapping upper bounds are not measured raster coverage.
With the separate writer diagnostic enabled, GPU before/after comparisons replace
those area estimates with changed-depth pixel counts per exact shader pair/check.
Gross changes can overlap across spans and are not exclusive screen coverage.
Final residual-unknown value ownership is reported separately after ordinary and
experimental authority decisions; identical-value writes/restorations follow the
same VALUE semantics as the interpolator. These diagnostics add GPU copies and
physical pass boundaries, so their timings are not the clean performance arm.
Every300 frames, `[framegen-admission]` separates otherwise eligible pairs rejected
by presenter ordering/clock/deadline gates or unavailable buffers from submitted
generation. `[framegen-generate-admission]` separates generator state/prediction/
capture/output-busy/extent/allocation gates. These are diagnostic counts, not policy
changes or proof of scanout.

Regional attachment clears are deterministic versioned writes: the cleared region
adds no sampled origin and the unchanged remainder conservatively retains the old
subresource's provenance. Absence of a current-frame writer means rootless history,
matching ordinary texture reads; it is not an explicit unknown mutation. A tracked
unknown prior writer still taints the remainder and refuses extraction. Prior draws
are logically published before a regional clear without splitting the Vulkan pass.
Scene-depth revocation, exact mip/layer/aspect identities and captured-UI mutation
checks remain in force. This fixes Thor's first-touch D16 array-mip and small RGBA8
clear ancestors without a size/format special case or ignoring genuine unknown writes.

The game's960x536 scene/depth is stretched to the1280x720 composition canvas with
independent factors1280/960 and720/536. Capture no longer imposes equal aspect:
canvas pixel center`(x+.5,y+.5)` maps to native depth texel coordinates
`((x+.5)*960/1280-.5,(y+.5)*536/720-.5)`. Both shaders operate in normalized UV,
so these independent per-axis factors cancel exactly; there is no isotropic
factor or extra half-pixel correction. Camera validation/reprojection and depth
continuity use native depth dimensions; UI and preserved color use canvas dimensions.
Only full-subresource axis-aligned mapping is represented. A contributing cropped/
offset viewport or partial/cropped scene-copy rectangle is refused with mapping
taint32 instead of guessed coordinates; arbitrary shader-authored UV transforms
are not recovered. `[framegen-map]` under diagnostics reports accepted extents
and independent scales. Conservative invalid-depth border fallback is unchanged.

In extrapolation mode this remains **30 simulation/input/animation frames plus up to 30 predicted camera pictures**,
not native 60 gameplay. Default-off draws read cached flags only: no resource registry walk,
uniform read, matrix math, callback, allocation or diagnostic clock is performed for framegen.

#### Identity and checked extraction chain

The cross-frame scene signature uses attachment presence, Vulkan formats, extents and sample
counts, depth presence and accepted full-viewport UBO1/2368 camera data, never physical pass
number, guest serial or allocation address. Within a frame, exact attachment/subresource sets
use a bounded hash table, so chunk600/reopened passes accumulate rather than replace each
other. Four/five-MRT variants can share **one live depth plus identical accepted camera**
when their common attachment properties agree. Actual camera changes are not ignored.

The supplied Thor census (`t-fgcensus.log`, aac33df) contains three full world frames
2177–2179. All three have the same four-MRT signature and the same five-MRT extension
(C7 added), D32,1280x720, single-sample, accepted camera/full viewport. The four-MRT groups
move183→184 and the five-MRT groups185→186, but their structural identities are stable
**3/3 frames**; each reopens4/6 physical times. Across the seven camera-accepted census
frames there are12 rows and exactly these two shapes. This is retrospective identity
evidence, **not evidence that this implementation generated a device frame**.

Generation checks this current-real-frame chain:

1. Accepted G-buffer depth/camera → actual sampled image versions through lighting/post.
2. Those versions → the exact pre-UI canvas identified by shared `vk_canvas`, also used
   by native UI. Resource keys include allocation incarnation and subresource, not recycled
   Vulkan handles; overlapping guest writes revoke proof.
3. First UI boundary → committed terminal `dd3d1e0e…` transfer sampling that same canvas.
4. That transfer → the exact registered, actually presented window image.

The old global requirement to prove every unrelated resource up to the presented image is
gone. Unrelated pre-scene compute/material work cannot poison the whole frame. Missing,
stale, ambiguous or overwritten scene provenance, a different canvas/window, unsupported
formats, loading/menu/cutscene structures without this chain, and incompatible replay
attachments remain real-only. Discovery needs an observed real transfer before learning
a canvas, then two accepted real cameras before prediction; startup/reset skips are normal.

The first device run of `3e92c79` still generated **zero** (logs
`t-fgv2-on-a/b.log`):257+ reached first-UI nomination but failed canvas ancestry,
while menus/loading also reported no current-frame root. The old `boundary` bucket
misclassified these dependency refusals; they now count as `chain`.
Fixed extraction errors: a UI-capable fullscreen VS no longer skips scene/post
inputs before the nominated exact-canvas boundary; an unbound reflected slot is
the same deterministic dummy used by the real descriptor emitter, not an unknown
edge; unrelated storage writes no longer taint every draw. Actual same-image GPU
writes, overlapping CPU/buffer writes, missing identities and invalid ranges still refuse.
These code defects explain ways to lose/poison ancestry; the supplied log lacks
per-edge evidence to attribute every refused device frame to a particular one.

After a refusal the full tracker probes only once per32 real frames; cached
transfer/UI shader classification and shared canvas discovery remain live on
every frame, even with native UI off. A successful probe immediately resumes
continuous tracking, then adjacent real cameras warm prediction normally. Backoff
resets camera continuity and cannot reuse a seconds-old camera. Recovery takes at
most31 skipped real frames plus camera warmup; `backoff` is separate from failures.
The first repair reduced refusing-state device recording25.5/25.6→26.8/27.8ms
overhead to25.9→25.8ms on `c3ae109`, but that build still generated zero.

The `c3ae109` device log (`t-fgv2b-on.log`) isolates snapshot refusal: valid root
masks (`origins/eligible/unresolved=0x1`) reach the canvas, but `tainted=1` with
about117 false GPU-alias invalidations per probe and no missing/storage/CPU-write
events. GPU writes were invalidating **different Vulkan image incarnations** just
because their guest ranges overlapped. This contradicted `vk_res`: incompatible
format/extent images deliberately keep independent GPU content. GPU proof now
follows actual shared-image incarnation/subresource; CPU/buffer writes still
invalidate overlapping guest ranges. Real compatible aliases still overwrite proof.

Valid large sampled arrays/volumes now inspect only current-frame produced versions
inside the view, rather than declaring more than64 possible layers incomplete.
The64-source limit still applies to **actually produced sampled versions**. This
removes another artificial taint source without accepting missing identities/ranges.

A refusing boundary now checkpoints logical image versions without ending real
rendering, performs no snapshot/warp, never forces HUD recording inline, and stops
the rest of that frame's tracker work immediately. A GPU regression verifies no
extra real pass and that UI stays parallel-capable. With diagnostics enabled,
each300 frames compares probe/backoff mean/max recording-thread CPU from first
render pass to frame end (whole walking window, **not tracking alone**; two clock
reads per rendered frame, none default-off). State-change `snapshot check=...`
messages name ancestry, live depth, resource/copy failure or success, with propagated
taint bits: unknown1, identity2, view4, storage8, in-pass-clear16. Repaired device
generation, probe-tail cost and scanout remain to be measured.

The previous285–385 `contexts-missed` were not evidence of a too-small draw-context table:
the old code deliberately sampled only four contexts per physical pass, stopped after32
draws, keyed its fast guard only by VS, marked root materials incomplete, and latched
unrelated UBO updates as camera instability. The new guard observes VS **and FS** plus
changed reflected bindings; accepted scene material inputs are outside the extraction
root, not counted as missed proof. There is no per-pass four-context/32-draw truncation.
Global limits (512 groups,2048 physical passes,8192 events,32768 binding inspections,
64 roots and1024 frozen UI/terminal draws) still fail closed, never silently truncate.

#### UI and ownership

In **extrapolation** mode, at the first UI boundary copy the real scene/depth and create a private, same-format
warped canvas. The real UI pass then stays intact: collect its already-resolved Vulkan
pipelines, dynamic state, descriptor sets, vertex/index buffers and draw parameters.
After real rendering ends, issue the collected draws **once in order in one private UI
pass**, followed by the actual terminal transfer with only its canvas image view replaced.
There is no per-UI-draw real/private render-pass switching and no scene/final difference
mask or held-background halo. Opaque UI pixels are unchanged; translucent UI blends over
the generated background with the game's original blend state.
The single scene/UI snapshot split can add tile-store/reload work outside the injected
timestamp regions; whole-frame device GPU A/B is still required in addition to the4ms guard.

Replay never reads future/live guest uniform or vertex memory. It uses the real draw's
frozen/lag-protected bindings and shares its covering retirement ticket; source/output
rings are fixed and fence protected. UI depth/stencil is copied before the real UI and
replayed privately, not written a second time into the game's depth. Ordered depth/stencil
and partial-color attachment clears are retained in that same private pass, including
deferred LOAD clears. Full canvas overwrites, unrecorded copies/storage side effects or
changed UI attachment/extent structures refuse generation. With native UI also enabled,
the private target follows the real raster extent; a mid-UI native/1x fallback that changes
target structure is refused rather than replayed with the wrong coordinates.

The camera warp still cannot create newly revealed scenery or true object motion. Invalid
depth, depth edges and failed reprojection keep real scene pixels. Link/NPCs, water, thin
foliage and particles can still show30Hz motion/warping. Camera cuts discard prediction.
No Link-object pointer or screen-centre actor heuristic is used.

#### Host cost and reproduction

`framegentrackbench 1000 12345 all` (x86 host,128 warmups) measured enabled core tracking:
128-physical/64-logical scene cases **0.0198/0.0217ms per4200/5600 draws**, including its
baseline draw work; the2048/512 capacity model was **0.184/0.186ms** and explicitly
failed closed on diagnostic-link capacity. Off made zero callbacks/scans/group operations;
its measured branch delta was0.11–0.39ns/draw (at most about0.0022ms/frame). These are
synthetic CPU numbers, excluding real reflection, camera reads, Vulkan and GPU work.

The real recorder's `--lag-walk-bench 12`, lag1, no recorder workers, one lavapipe worker,
off/on/off measured stable bindings3728.76/3273.68/3402.43ns/draw and churn
4512.20/4214.91/4792.77ns/draw. Cold inputs were1702.05/1635.07/1618.24; learned-hot
1688.67/1677.58/1844.88. No speedup is claimed from negative deltas/noisy host scheduling.
The old measurable multi-ms overhead disappeared after guarding shared canvas preparation
by actual pass/program/input changes; this synthetic recorder fixture has no valid game
camera, so **it does not establish <1ms tracking on Thor or a complete world capture**.
Raw logs are in `.scratch/framegen-v2/host/` (`framegentrackbench.log`, `guarded-walk-*.log`).
The refusing-state repair's fresh off/on/off host walker (`--lag-walk-bench12`,
native UI off, lag1, workers0) measured stable4512.83/3311.97/3806.53ns/draw,
churn5180.36/4651.23/4479.29, cold1684.81/1592.42/1661.16 and learned-hot
1756.23/1708.35/1789.09. These noisy synthetic runs have no accepted camera/scene
and are not a Thor performance or successful-capture claim. Logs:
`repair-refusing-walk-*.log`. The probe policy test observes10 full probes over320
refused frames and immediate continuous tracking after eligibility.

After the image-ownership repair, `framegentrackbench 1000 12345 all` measured
128/64-pass core CPU0.0214/0.0232ms per4200/5600 draws and the capacity model
0.2339/0.1977ms (expected fail-closed link limit). Off made zero callbacks/scans;
small positive/negative baseline deltas remain host noise, not a speedup claim.
`vkframegentest` also executes320 **actual rendered** refusing frames:10 probes,
320 real passes and no probe-only split or forced-inline UI. Its diagnostic first
300-frame window measured probe CPU mean/max0.013/0.083ms versus backoff
0.004/0.027ms, for this tiny no-camera fixture only. These are whole walking-window
CPU measurements, not successful-world tracking cost or mobile timings. Logs:
`snapshot-framegentrackbench.log` and `snapshot-lifecycle-final-vkframegentest.log`.


Build host fixtures with `make -C games/totk/android TARGET=linux CC=clang OUT=<scratch>
vktest vktest-app framegenslottest framegenmathtest framegendeptest framegenpasstest
framegentrackbench vkframegentest`. Use `TOTK_SHADER_SEED_DIR=<scratch>/vkdraw-seed`
and a host Vulkan ICD. Run all pure fixtures and `vkframegentest --off`, default,
`--budget`, `--late`, `--interp`, `--interp-budget`, `--interp-device`,
`--interp-partial-clear`, `--interp-gameplay` in separate processes. The synthetic scene+128-UI-draw fixture
checks exact opaque HUD pixels after guest uniform/vertex arrays are overwritten,
translucent recomposition, camera/depth goldens and bounded ring reuse. Its pass census
is **two real passes both off and on**, plus exactly four private passes per generated
frame (warp, one batched UI, terminal transfer, output); one snapshot boundary may split
a previously combined scene/UI pass on device, independent of UI draw count.
`--interp-device` additionally exercises the production native-UI-off/post-off startup
gate and registered Vulkan image identities, then the learned canvas -> first UI ->
committed terminal transfer -> exact present callbacks and fail-closed counterexamples.
Its synthetic 960x540 scene, 1280x720 canvas and padded 1920x1080 window model distinct
roles, not a captured game pass trace; the original log does not expose all bindings.
It then runs the full interpolation shader goldens. Unlike these callbacks, the shader
goldens inject capture/replay inputs directly and alone could not catch the startup bug.
`--interp-partial-clear` drives actual scissored clear commands through the renderer
for mip1/layers0–1 of a D16 array and a120x67 RGBA8 target. It checks cleared pixels,
untouched history and mip0, then actual ancestry selection/capture and the complete
interpolation goldens (five accepted outputs), plus unknown guest-write and cropped
scene-copy refusals. Its native960x536 color image is GPU-blitted to1280x720 with
unequal x/y scale. An independent CPU bilinear texel-center golden checks the last
eight rows and bottom-right pixel, including the intentional older-frame fallback
at invalid-depth borders, so a half-pixel/last-row shift cannot hide in a smooth ramp.
The camera/scene seed and sampled-composition edges are explicit synthetic inputs;
it does not claim to execute the game's complete G-buffer/post shader chain.
Interpolation fixtures also drive the real context-classification callback through
depth-off→on→off state changes with a synthetic reflected camera binding. The on
context must immediately recognize its existing compatible root; the off context
must not inherit camera admission from its predecessor. This guards the per-context
depth-state ordering, without authorizing merges of previously revoked roots.
`--interp-gameplay` extends the partial-clear mode with synthetic draw spans through
the production context/pass/version tracker: depth-only standard-camera continuation,
single-color raster/sentinel explicit-depth export, MRT continuation,
alternate-UBO view/projection continuation, standard-camera MRT again, then the
conditionally proved world-plane depth export. It verifies one surviving root and
the same five generated midpoint goldens, including nonuniform960x536→1280x720
mapping at the bottom edge. Different camera matrices, nonzero unmodelled bias,
unknown shaders, cropped viewports, unclassified exports, nonstandard depth range,
active clip shrink, incompatible projection coefficients, changed VS/FS camera
banks and an unknown depth writer preceding an accepted context in the same
physical pass must refuse. Both audited world-plane FS variants are exercised.
These metadata spans are not submitted as guest shaders or counted as real GPU draws;
the uploads, partial clears, captured inputs and generated pixels remain real Vulkan.
`--interp-authority` executes a real unknown explicit-depth-export pass over a
partial scene through production context/ordered-span/pass publication. It requires
bitwise actual-real fallback inside the changed/dilated region, interpolation
outside it, same-value/color-change and changed-then-restored depths treated as
proven values, high-coverage post-fence rejection, span-copy failure/recovery,
unsupported source refusal, and no source/certificate reuse while outputs pin it.
`--interp-authority-ordering normal` and `--interp-authority-ordering lag`
exercise actual production draw/span hooks with changed proven geometry after
unchanged unproven spans (roughly46% of the scene): raw unknown must be0 and the
output must match the true midpoint, not the older real fallback. Other cases
cover ordered overlap/disjoint overwrites, last-proven restoration, conservative
same-value/hidden proven writes, qualified experimental isolation, odd/contained/
expanding/edge scissors and copied-texel counts, pinned masks across frames,
before/after failures, unsupported/reset/unclosed/257th-span refusal and recovery.
It also checks stationary radii, tighter translated bounds from certified z=2
depth, the old source's radius changing for cameras0,0,+.16, immutable earlier
output census, and a signed-zero/invalid/far-depth range that must keep the hard
cap rather than drop a shader-valid near value. `framegenmathtest` includes an
admissible clipped-triangle counterexample to a camera-only0.25px active-shrink
certificate: the VS leaves the FS's original position varying unshrunk, so a
per-vertex500-world-XZ ramp does not bound its fragment plane-fill error.
Active shrink remains unknown by default; no heuristic tolerance or depth-agreement
certificate is introduced. The explicitly experimental
`TOTK_FRAMEGEN_UNCERTIFIED_PLANE=1` bypass is an assumption, not a certificate:
it nominates only e87dbf67/6ec21411 VS identities with the exact ecbe FS variants,
finite nonzero shrink control, and the same checked live VS/FS/root camera and
projection state. Per-span before/after copies attribute only changed depth
VALUES to those draws. An unrelated different unproven value stays unknown;
ordinary proven updates re-certify through the same ordered certificate.
An unrelated same-value write or restore of an already assumed value retains
that VALUE assumption, not evidence of untouched colour. Failed copies, stale
source state and span-capacity overflow refuse.
`--interp-uncertified`, `--interp-uncertified-off` and
`--interp-uncertified-invalid` exercise the real extraction hooks and GPU
copy/compare machinery using synthetic draws (not execution of guest shaders):
exact pair variants, earlier/later unrelated writers, same-value/restoration,
camera/control/identity/depth-range negatives, copy failure and clear invalidation.
Qualified contiguous contexts share a bracket; proven or differently qualified
depth writers close it before their actual draw. Span diagnostics sum clipped
before/after copy+compute GPU brackets, not intervening real draws; adaptive
whole-real timing already contains the work, while a fixed override additionally
charges that measured span work.
`framegenbudgettest` checks exact policy provenance, ordering, stale/missing
samples, overload recovery and strict explicit overrides.
`--interp-headroom` exercises actual completed slot timestamps with no fixed
override, preserving real cadence while adaptive history warms up.
`--interp-dilation` dispatches the production compute shader and compares EVERY
mask pixel against an independent full-resolution rectangular-dilation reference.
Random/adversarial masks, all tile-corner phases, tiny/odd/native extents, partial
edge rows/tiles, minimum/maximum motion radii and exactly/below/above35% census
thresholds must never turn a reference-unknown pixel known or accept a pair which
the reference refuses. It also checks native-area census, source immutability
and retained pair masks across later dispatches.
`--interp-writer`, `--interp-writer-off`, `--interp-writer-invalid` and
`--interp-writer-no-diag` exercise production physical draw hooks and the separate
GPU attribution pipeline. Cases include disjoint/overlapping spans, identical
writes and restoration, exact keys/checks, contiguous batching, known-overwrite
revocation, experimental exemptions, failed/unsupported/capacity diagnostics and
pinned-source lifetime. These are synthetic depth writes, not guest shader execution.
Regenerate embedded production/test shaders with
`python3 games/totk/android/gfx/framegen/generate.py --compiler <glslc> --scratch <scratch>`.

#### Thor device acceptance plan (operator-run; not executed here)

1. Same signed build, warmed save/cache/Turnip, camera/weather, thermal state, brightness,
   governor/GPU cap and controller route. Physical60Hz, handheld720p canvas. Fix:
   `TOTK_FPS60=0 TOTK_UNCAPPED=0 TOTK_FPS_AUTO=0 TOTK_PRESENT_SLOTS=1
   TOTK_PRESENT_BUFFERS=3 TOTK_VK_PRESENT=ahb TOTK_FRAME_LAG=1
   TOTK_VK_CHUNK_DRAWS=600 TOTK_VK_REC_THREADS=2 TOTK_ASYNC_REPLAY=0
   TOTK_NATIVE_UI=0 TOTK_POST_AA=off TOTK_POST_SCALE=off`.
   Keep the census run's acquire-slack/pacing settings unchanged.
2. Warm180s; restart for each off/on/off arm, then reverse order. Measure180s per arm:
   30s still scene,60s reproducible constant camera pan with HUD,30s direction reversals,
   20s menus,20s loading/teleport/cut and20s return to world. On arm:
   `TOTK_FRAMEGEN=1 TOTK_FRAMEGEN_DIAG=1 TOTK_FRAMEGEN_BUDGET_MS=4
   TOTK_FRAME_TIMELINE=1`; off arm disables only framegen. Keep complete logs.
3. After transfer discovery/history warmup, require nonzero generated counts and sustained
   eligible≈generated in a normal pan (ideally299/300 or300/300 after startup, not0/300).
   Each refusal must have a state-change reason and matching300-frame bucket. Check GPU
   cost against4ms, real30 deadline stability, source retirement, no capacity failure and
   no permanent suspension. Generated counts mean encoded pictures; reconcile with actual
   presenter/fence results, since late/surface-dropped children are not displayed frames.
4. Confirm AHB/SurfaceControl with no copy fallback. `window present-fence cadence`
   must move from mostly33.33ms off to mostly16.67ms on, with near-equal real interval2
   and synthetic interval1 results. Inspect missing/deferred fences, repeated/reversed IDs,
   p50/p95/p99, <12.5ms bunching and >=29ms gaps; join adjacent transaction IDs, never callback
   order. Desired timestamps/framework fences alone are not guaranteed physical scanout.
5. Capture a separate60fps camera-pan video with HUD/minimap/text and still screenshots
   before/mid-pan plus HUD crops; recording overhead must not contaminate the timing arm.
   Inspect scene motion, unchanged HUD position/sharpness, semitransparent backgrounds,
   grass/water/Link edges and reversals. Add an external high-speed camera run for stick/button
   event→first visible response; compare identical endpoints and real-frame timeline
   global-window-read→present-fence p50/p95 against off.
6. Latency estimate: **no intentional future-real-frame/look-ahead wait**; the child is
   scheduled16.67ms after its real parent using old input/camera history. This does not
   provide60Hz input response. Extra GPU work (budgeted at4ms) and compositor queuing can
   still increase latency; report the measured delta, not an assumed reduction.
   Repeat pause/resume, window recreation and DRS, then separately test `TOTK_NATIVE_UI=1`.
   Odin's true60 mode remains the preferred60-Hz option; do not enable generation there
   or substitute generated FPS for simulation/input responsiveness.

### Optional presentation AA and upscaling

The presentation switches below default **off** and are read once at startup from `totk.env`. With presentation
and scene filters unset/`0`, presentation records exactly the previous bilinear path; no post pipelines, lookup
textures, intermediate images or timestamp queries are created. These legacy options affect the final picture
(including the UI), not world draws or the game's scene resolution. Prefer `TOTK_SCENE_AA=smaa` above when the
HUD must remain untouched.

| Switch | Effect |
| --- | --- |
| `TOTK_POST_AA=smaa` | SMAA 1x HIGH: luma-edge detection, area/search lookup blend weights, then neighborhood blending at the game's **cropped output resolution**. Smooths high-contrast stair steps without a history buffer; it cannot recover temporal detail or eliminate moving foliage shimmer. |
| `TOTK_POST_SCALE=fsr` | AMD FSR1 EASU to the actual window-content pixel size, followed by RCAS; replaces bilinear enlargement. Unlike the default 1280-wide presenter, this overrides the `TOTK_APP_WIDTH` cap and presents window-sized buffers (e.g. 1920x1080). Aspect ratio and black bars remain unchanged. |
| `TOTK_POST_SCALE=sgsr` | Qualcomm Snapdragon Game Super Resolution **1**, a single spatial upscale/edge-sharpening pass; an alternative to FSR, not SGSR 2 temporal reconstruction. Targets the actual window-content extent and overrides `TOTK_APP_WIDTH` just like FSR. Bilinear in smooth regions, twelve-tap Lanczos-like correction on green-channel edges; no extra RCAS pass. |
| `TOTK_POST_SHARPNESS=0.2` | RCAS strength, clamped to 0..1; default 0.2. Maps to `2*(1-strength)` attenuation stops: 0 is the weakest quarter-strength sharpening, **not** a bypass; 1 is strongest. Has no effect without FSR. Non-numeric/non-finite input retains the default. |
| `TOTK_POST_SGSR_SHARPNESS=2` | Upstream `EdgeSharpness` multiplier, default **2.0**. Finite nonnegative values, in upstream units (not FSR's 0..1 scale). Zero removes the reconstructed high-frequency term but is not an SGSR bypass. Applies only to SGSR. |
| `TOTK_POST_SGSR_EDGE_THRESHOLD=0.031372549` | Upstream `EdgeThreshold`, default **8/255**, in normalized channel units. Lower values process more edges; higher values leave more pixels bilinear. Values above 3 disable edge correction, useful as a bilinear comparison. Finite nonnegative values only; malformed/non-finite/negative controls retain their defaults. |
| `TOTK_POST_SGSR_EDGE_DIRECTION=1` | Selects the upstream optional `UseEdgeDirection` kernel at pipeline creation; default **0**, the cheaper original SGSR1 mobile kernel. May improve diagonals at extra cost. Only exact `1` enables it. |

Legacy presentation AA and upscaling can be combined with `TOTK_FXAA=0 TOTK_POST_AA=smaa` plus
`TOTK_POST_SCALE=fsr` or `sgsr`, but that filters HUD/text. For scene-only AA use
`TOTK_FXAA=0 TOTK_SCENE_AA=smaa TOTK_POST_AA=off` plus optional `TOTK_SCENE_SCALE=fsr` or `sgsr`;
AA runs before upscale. Leaving shipping FXAA enabled stacks spatial AA filters and can soften fine detail.
Both upscalers are spatial filters, not temporal AA or a way to recreate native-resolution detail.
All algorithm sampling and intermediate rendering use display-encoded RGB (UNORM views), preserving the final
sRGB/UNORM presentation transfer function and channel order. Crop boundaries are clamped, so DRS padding cannot leak
into the picture. SMAA and FSR1 are MIT components; SGSR1 is BSD-3-Clause with its full Qualcomm notice retained.
See `THIRD_PARTY_NOTICES.md`; the pinned SGSR1 source is independent of the LASRecomp integration that motivated this port.
The embedded SPIR-V and lookup headers are generated offline from pinned upstream sources under `gfx/post/`.
Regenerate from the repository root with
`python3 games/totk/android/gfx/post/generate.py --compiler "$ANDROID_NDK_HOME/shader-tools/linux-x86_64/glslc" --scratch ../.scratch/aa`.
No game shaders, runtime shader compilation, or external lookup assets are required.
This Android post-scale is independent of the game's existing FSR1 path. On 1.4.3, `Handheld_720` disables native FSR
in both its baseline and its .90/.80 DRS records. The investigated main+0x586958 store only changes the baseline;
active DRS records override it, and additional dispatch gates remain. Therefore **no `TOTK_GAME_FSR` switch is
exposed**: forcing that store alone would be a misleading, incomplete handheld toggle.

The same chain precedes both the window-copy and AHB presenters, recording into the existing present command buffer.
Pipelines are created at device startup; intermediates use the backend image allocator and existing present
slots. A slot is resized only after its normal GPU/compositor release, without introducing GPU waits. GPU timestamp
readback piggybacks on the presenter's existing fence and logs `post GPU last 300 frames: mean … ms, max … ms,
latest … ms`. This is **chain GPU time**, excluding lookup upload, final picture copy/letterbox and CPU presentation.
Unsupported timestamp queues log timing unavailable rather than estimating it.
The native FPS/power overlay remains the **last pixel-writing operation**, after AA/upscale and final letterboxing,
for both copy and AHB. Scaling does not filter its glyphs; ownership release/readback follows it.

**Cost is not yet device-measured.** SMAA adds three native-output passes; FSR adds two window-content-sized passes,
whereas SGSR adds one and needs one upscale intermediate instead of two.
On the Adreno 740 budget these are additional bandwidth/fragment costs, not a promised performance improvement;
either upscaler also raises the copy path's CPU/readback traffic from 1280-wide to window-wide (1080p is 2.25x 720p pixels).
AHB avoids that CPU copy. The Adreno 830 may have more headroom, but do not infer milliseconds from host/lavapipe.
Keep defaults off until the following acceptance run establishes the visual/performance trade-off.

Initial SMAA/FSR host acceptance (lavapipe, not device performance): `make -C games/totk/android vkposttest TARGET=linux CC=cc
OUT=<scratch>` builds the real upload/render/readback test. Run fresh processes with AA/scale off/off, smaa/off,
off/fsr and smaa/fsr; use `TOTK_POST_TEST_SRGB=1` for the combined case. These passed **6/6, 14/14, 15/15 and
314/314** checks, including exact disabled reference bytes, 96x64/192x128 RGBA8 outputs, nonzero crop isolation,
same-slot resize, and 300 completed frames through all three slots. SMAA-only produced **224** gray transition
pixels versus **0** in the hard-edged source. The combined run exercised the 300-frame timestamp log. The app
window-copy fixture also passed **104/104**, both disabled and combined; Android `cc1.sh` was clean for
`vk_post.c`, `vk_draw.c`, `nvn_vk.c` and `test_vkpost.c`. AHB runtime correctness and device costs remain unmeasured.

Device plan (not executed here): on **Thor/Adreno 740 and Odin 3/Adreno 830**, use the same warmed Lookout Landing
save, camera/weather, 60-Hz display, 30-fps cap, Turnip build, lag/recorder settings, power cap and thermal state.
Run at least 180 s with movement per case, repeat in reverse order: unchanged baseline; FXAA-off alone; SMAA with
FXAA-off; FSR alone; SMAA+FSR with FXAA-off. Repeat each with `copy` and `ahb`, verifying no AHB fallback.
Record every 300-frame chain mean/max/latest GPU-ms line, whole-frame GPU ms, fps/longest-frame/p95/p99 and presenter
shown/dropped counters. Thor's existing roughly 21–27 ms GPU load leaves limited margin inside 33.3 ms; report fps
loss rather than claiming the chain is free. Record power/thermals and window/readback dimensions as well.
Capture identical stationary frames for thin fences, roof/weapon diagonals, foliage, UI text and dark gradients;
compare matching native-pixel crops (no viewer resampling) at 720p and 1080p, plus motion clips for shimmer/halos.
Also cover DRS crop transitions (~1024x576 to 1280x720), letterboxing, orientation, background/resume and window recreation.

#### SGSR host golden images and indicative cost

The extended `vkposttest` computes independent scalar SGSR1 golden images from the input bytes, not a hash or
readback-derived baseline. Five fixtures (asymmetric color, aliased diagonal, gradient, flat color and high-contrast
crop border), 1x/1.5x/2x scaling, both UNORM/sRGB outputs and repeated same-slot resize are checked. Replacing every
texel outside a nonzero crop must leave its result **byte-exact**. RGB tolerance stays at **two code values**;
alpha and flat colors are exact. The oracle models both fractional and Q8/UNORM8 hardware bilinear sampling and
requires one coherent sampler model to match the **entire** suite. This matters at `EdgeThreshold`: legal sampler
rounding can change whether correction is applied, so inflating the RGB tolerance would hide genuine filter errors.
Lavapipe selected the independently modelled UNORM8/separable-rounding path; SGSR's algorithm was not changed to
fit that sampler, and the threshold oracle uses the shader's FP32 branch comparison.

Fresh-process runs passed: default SGSR **201/201**; edge direction **201/201**; sharpness 0/4 **200/200, 201/201**;
threshold 0/4 **201/201, 199/199**; two invalid/non-finite/negative-control cases **201/201** each; and common RCAS
sharpness 0/1 (which must not affect SGSR) **201/201** each. SMAA+SGSR passed **314/314** with UNORM and **314/314**
with sRGB. The native copy-presenter overlay fixture passed **92/92** in all four corners with SGSR and **92/92**
with SMAA+directional SGSR, including exact glyphs/colors after the scaler. Android `cc1.sh` was clean for
`vk_post.c`, `vk_draw.c`, `test_vkpost.c` and `test_vkoverlay.c`; shader embedding was generated with NDK glslc.
Disabled/SMAA/FSR regressions also passed. These are host checks, **not** Android AHB/compositor acceptance.

Host-only timestamps, llvmpipe Mesa 26.0.8 / LLVM 21.1.8, `LP_NUM_THREADS=4`, pipeline cache off, deterministic color
fixture, 60 excluded warmup frames then 300 samples per process:

| Synthetic input → output | Scaler | Mean / median / p95 GPU-span ms |
| --- | --- | --- |
| 1280x720 → 1920x1080, forward order | SGSR default, one pass | 12.812 / 12.158 / 17.528 |
| Same, forward order | FSR EASU+RCAS, two passes | 50.280 / 42.920 / 98.148 |
| Same, reverse order | FSR EASU+RCAS | 43.097 / 38.143 / 70.008 |
| Same, reverse order | SGSR default | 13.894 / 12.499 / 20.634 |
| Same | SGSR edge-direction | 19.284 / 16.282 / 38.007 |
| 960x540 → 1920x1080 | SGSR default | 15.410 / 14.158 / 28.151 |
| Same | FSR EASU+RCAS | 37.869 / 36.440 / 49.101 |

SGSR's median was roughly 28–33% of FSR's in the reversed 720p host pair. This is a **software-rasterizer comparison
with substantial scheduler variation**, not an Adreno GPU-ms or FPS prediction. Upload, readback, pipeline startup
and CPU fence-wait duration are excluded. Synthetic 540p direct input does not imply the game's final crop is 540p.
Reproduce with `TOTK_POST_AA=off TOTK_POST_SCALE=sgsr` (then `fsr`) and
`vkposttest --benchmark 1280x720 1920x1080 300`, keeping `LP_NUM_THREADS=4`, ICD and other settings identical.


#### Lower scene resolution with 1080p presentation

The existing 1.4.3 scene-size switch is **`TOTK_RENDER_RES`**, not `TOTK_SCENE_RES` (no such alias is implemented).
It changes the scene render size, independently of the game's output/UI canvas. Leave `TOTK_OUTPUT_RES=0` so the
game does **not** allocate a new 1080p/1440p output canvas and associated heap overlays just to apply a post upscaler.
For a 1920x1080 device window, this exact `totk.env` combination lowers scene cost and presents a 1080p SGSR picture:

```ini
TOTK_OUTPUT_RES=0
TOTK_RENDER_RES=1152x648
TOTK_VK_PRESENT=ahb
TOTK_APP_WIDTH=1920
TOTK_POST_SCALE=sgsr
TOTK_POST_AA=off
TOTK_POST_SGSR_SHARPNESS=2
TOTK_POST_SGSR_EDGE_THRESHOLD=0.031372549
TOTK_POST_SGSR_EDGE_DIRECTION=0
TOTK_OVERLAY=full
```

Use `TOTK_RENDER_RES=960x540` for the more aggressive scene reduction. The game's DRS may reduce these scene sizes
further; with `TOTK_RENDER_RES=0`, the game's normal handheld DRS policy remains in charge instead. Do not combine
this with unrelated Bootup base-resolution edits. `TOTK_APP_WIDTH=1920` is redundant while SGSR/FSR is enabled,
but keeps the **off/bilinear baseline** at the same 1080p presentation extent. It is not a request to supersample
the game. On a different-sized window, SGSR/FSR target that actual window size rather than forcing 1080p.

Important boundary: the Android post shader consumes the **final game crop**, normally up to 1280x720. A 1152x648 or
960x540 scene may already have been composed/upscaled by the game into that output, including UI. SGSR does not
intercept a raw pre-UI scene attachment or bypass the game's existing resolve/FXAA. Check game render-target sizes
and final crop separately rather than describing `TOTK_RENDER_RES=960x540` as proof SGSR directly sampled a 540p image.
Crop-only DRS output changes are supported without changing the fixed window-size result. Both SGSR variants clamp
all sampling footprints to the valid crop, including gathers at a nonzero crop origin.

The existing zero-copy presenter consumes the same post result/view as FSR and draws it into its explicit-LINEAR
AHB before the overlay and foreign-queue release. Require `post: … scale=SGSR1`, `window AHB picture 1920x1080`
and `window SurfaceControl: first transaction completed`, with **no fallback**, to count a device run as zero-copy.
Host pixel tests exercise the shared chain/copy presenter; Android AHB import and compositor operation still require
the device acceptance below and must not be inferred from a host test.

#### SGSR/FSR device A/B

On **Odin 3, warmed Gerudo, FPS60 enabled**, compare five cases at the same 1920x1080 window:
native scene/bilinear (`TOTK_RENDER_RES=0`, `TOTK_POST_SCALE=0`); 1152x648+SGSR; 960x540+SGSR;
1152x648+FSR; 960x540+FSR. Start with AA off and keep the game's FXAA, frame lag, pacing/cap, recorder workers,
driver, save/camera/weather, thermal state and power cap identical. Set `TOTK_FPS60=1`; if measuring a 60-Hz-capped
play mode keep that cap, otherwise label an uncapped run explicitly rather than comparing mixed pacing policies.
Repeat at least 180 s with movement, then reverse the order. Repeat on **Thor** with a 30-fps baseline (and a
separately labelled FPS60 run only if wanted), because its Adreno 740 has much less GPU margin.

Record chain GPU mean/max/latest ms every 300 completed frames, whole-frame GPU ms, mean FPS, frame-time
p50/p95/p99/worst, shown/dropped counts and power/thermals. SGSR's one pass is not a promise of a device speedup.
For screenshots, use identical stationary views and native-pixel crops of Gerudo roof edges, railings, fine foliage,
UI text and dark gradients, plus motion for shimmering and sharpening halos. Compare SGSR default/edge-direction
separately; hold its threshold/sharpness fixed in the main matrix. Verify overlay glyphs remain crisp.
Run AHB first with its successful import/SurfaceControl logs, then copy as a compatibility A/B; test crop/DRS
transitions, letterboxing and resume/recreation. No device SGSR result is claimed here.



## Opt-in game rendering reductions (1.4.3)

These independent `totk.env` switches default **off**. They are original implementations from verified 1.4.3
instruction/parameter facts, not nx-optimizer/UltraCam code or redistributed game data. Native code patches check
every original instruction in their group before any write; a wrong version, changed word or occupied veneer
padding stops startup rather than applying a partial patch. Each enabled reduction logs its application once.
Keep `TOTK_FPS60=0` and the existing real-time/secondary-animation fixes. No new performance gain has been
measured on a device; these are A/B options, not a promise of steady 30 fps while moving.

| Switch | Intended effect | CPU draws / visual cost |
| --- | --- | --- |
| Existing `TOTK_RENDER_RES=1152x648`, then `1024x576` or `960x540` | Overrides the base **scene** size at four checked instructions beginning main+0x585e48; output/UI size is separate. Unset or `0` keeps the game selection. Native DRS still operates below this base; record actual pass dimensions, not just the requested size. | Predominantly GPU fill/bandwidth; no world-draw reduction expected. More blur/shimmer, especially in motion. Do not also change Bootup base-resolution keys. Smaller scenes need no extra heap overlay. |
| `TOTK_SHADOW_RES=288` or `256` | Replaces main+0x6835a4 with `mov w11,#N`, and +0x6835a8 with a branch past distance-dependent width modulation. 288 maps to `0x5280240b`, 256 to `0x5280200b`; both are multiples of 8. Unset/`0` leaves shipping shadows. | Predominantly GPU shadow pixels, **same shadow draws expected**. Compared with an observed 360 map, 288 has 36% fewer pixels and 256 has 49.4% fewer; these are area ratios, not measured time savings. Softer/jaggier shadows and possible changes to effective cascade coverage. |
| `TOTK_DRS_MIN=0.75` (or `0.70` diagnostic) | Lowers only the native `Handheld_720` ladder minimum from .80; retains its .90 intermediate step, full-resolution recovery, priorities, thresholds and hysteresis. Finite `0 < scale < 0.8` required; **unset** disables (empty/zero is rejected). At base 1280x720, .75 nominally permits 960x540; engine alignment can differ. Other handheld/docked palettes are untouched. | Predominantly GPU pixels, no expected world-draw savings. More transient blur during load/motion, but recovery quality remains. Not “DRS off” and not a fixed ceiling. Combining a smaller base scene and lower floor compounds quality loss; measure together only after isolated runs. |
| `TOTK_FAR_CLAMP=12000`, then `8000`/`5000` diagnostic | At main+0x628c30 a checked veneer clamps the **graphics render-camera** far plane to `min(original, metres)` and keeps its source/published camera fields consistent. Preserves near plane, FOV, q1, all GPRs/LR, stack alignment and flags. Unset/`0` disables; otherwise finite positive metres required. | The only option here with supplied device evidence of fewer CPU world draws: the feature branch's 200 m sweep reduced draws about 16% and improved its worst window from 20.9 to 26.2 fps. This is not a measurement of the combined protected-lag build. Horizon clipping/pop-in and invisible-but-collidable distant objects; simulation, actor activation, streaming and collision distances are not changed. |
| `TOTK_FXAA=0` | Changes the common effective FXAA flag store at main+0x586944 from `strb w9` to `strb wzr`, including inherited settings. Unset/`1` keeps shipping FXAA. FSR is unchanged. | Small GPU/postprocess saving at most, not thousands of world draws. More moving-edge/grass/fence shimmer, especially with low scene sizes. |

Shadow guards also pin the original round-up-to-8 and downstream aligned/packed-size instructions: the packed
width/height at +0xf4/+0xf8 and dependent width/height stores receive the same selected dimension. The existing
cascade-count store is preserved. No explicit split/distance instruction is patched. This preserves consistent
dependent sizes, **not a proof of unchanged cascade coverage**:
the shipping algorithm can couple resolution and coverage. Inspect both actual shadow dimensions and all cascade
transitions before keeping a profile. The far veneer uses the zero-checked text-padding slot after the bounce
veneer (+288 from aligned text end), separate from the aspect and timing veneers.

### Preparing the local DRS template

There is no modified Bootup in the source tree. Prepare a decompressed, **unmodified** template once from the
player's own verified local 1.4.3 RomFS using Python 3.14+ (standard `compression.zstd`):

```
python3.14 games/totk/scripts/prepare_drs_template.py \
  --romfs-dir <local-extracted/v143/romfs> \
  --output <local-extracted/v143/romfs>/bootup-drs-v143.sarc
```

Place that generated template alongside the app's `extracted/v143/romfs/romfs.bin` (or split image). For a different
location set `TOTK_DRS_TEMPLATE=<absolute template path>` in `totk.env`; the default is
`<TOTK_ROMFS>/bootup-drs-v143.sarc`. Set `TOTK_DRS_MIN=0.75` and restart. Regeneration is needed only when the local
template is missing/damaged, not when selecting a new minimum. Never distribute that Nintendo game-data template.

With the switch enabled the runtime authenticates the original `Pack/Bootup.Nin_NX_NVN.pack.zs` SHA-256
`e551acaff6bf6cbb68fab1ad931adfb9307c21c7671d7474cb6a093252e74441`, the complete decoded SARC and the graphics
member hash, BYML type and original float words before changing the minimum at member+0x14a4. It preserves every
other byte and member and wraps the same-size archive in a standard dictionary-ID-3 raw-block zstd frame in memory.
RomFS open/size/partial-read/EOF therefore expose one consistent whole packed file, not a loose member the pack
loader would ignore. Existing non-Bootup overlays still apply. A Bootup replacement from any manifest is explicitly
refused while DRS is enabled; do not silently stack two different packed replacements. To use a separate Bootup mod,
leave this switch off. No template is opened or extra pack memory allocated when the switch is off.
The raw-block representation is 1,795,544 bytes versus the original compressed 286,256 bytes: enabling this option
retains about 1.71 MiB of host pack memory and increases the game's compressed-input allocation/read size, while the
decompressed archive stays 1,795,492 bytes. That dependency-free trade-off is startup-only; its device acceptance
still needs the boot/sweep checks below.

### Thor Lookout Landing acceptance sweep

Device acceptance remains pending. Use the same save, starting camera, weather/time, Turnip build, shader seed,
panel refresh, power/governor cap, brightness, presenter, frame-lag and recorder-thread settings. Warm caches and
thermals first. Keep `TOTK_FPS60=0`; retain fog/FOV, timing fixes and ordinary gameplay settings. Use the established
Lookout Landing camera sweep with **`MOVE=1`**, at least 180 seconds per run, repeating baseline and each isolated
switch in reverse order. Stationary screenshots alone are not a performance test.

1. Baseline all reductions off; then test scene 1152x648/1024x576/960x540, shadow 288/256, DRS .75/.70, far
   12000/8000/5000 and FXAA off **one at a time**. Restart for each profile, retain its single application log and
   reject any run with a guard/template failure or a different setting silently substituted.
2. Retain every warmed 300-frame window: fps, the **minimum window fps** (not only mean/best), recorder CPU
   ms/frame, draws/frame and GPU ms/frame. Keep GPU profiling mode identical. Also record actual scene/DRS and
   shadow pass dimensions, passes/frame and thermal/power state. Only after isolated results test the best visually
   acceptable combination. Do not add CPU and GPU savings naively; their work overlaps.
3. Take matched baseline/profile screenshots at the same points, including during movement: text/UI and fine
   fences/grass for scene/DRS/FXAA, sun shadows at each cascade boundary and on Link/nearby buildings, and the horizon,
   sky islands and town entrances for far clamp. Check for flicker, seams, shadow disappearance, size mismatch,
   pop-in and visibility of gameplay hazards. Walk/rotate through the whole town, enter/exit menus and interiors,
   and verify no gameplay/collision change or camera near/FOV regression.
4. Keep far clamp as a CPU optimization **only if measured draws/frame and recorder time fall**. Lower pixel
   dimensions without fewer draws is a GPU reduction, not a fix for the roughly 5,600-draw recorder bottleneck.
   Target sustained 30 fps including the worst moving-camera window, and compare power after a longer warmed run.

Host checks (no device/GPU claims): `scripts/verify_v143_render.py --output-dir <scratch-dir>` compiles a host
fixture against the production C and uses Unicorn to exercise default-off, independent selection, malformed
values, all original-word mismatches, padding/bounds refusal, shadow size packing, FXAA byte-store and far-clamp
register/publication semantics. It requires Python with `unicorn`; add `--main-nso <local exefs/main>` and the
`lz4` Python package to authenticate/check the real original sites. `scripts/verify_drs_transform.py` covers the
DRS transformation and zstd decode; use its `--help` for local paths.
Run the project's `.scratch/cc1.sh <checkout>/games/totk runtime/game_patches_v143.c` and `runtime/romfs.c`
syntax checks before integration. Test output belongs under scratch, not the game build directory.
The changed production C files passed those Android syntax checks. The renderer host check passed all 23
original-word corruption cases and authenticated the real NSO sites, then emulated both selected shadow dimensions,
matching dependent width/height stores and preserved cascade count, FXAA byte-store, and far values below/equal/above
the cap with live registers preserved. The DRS host check passed original/template/member refusals, default-off,
overlay conflict, split/mmap reads, .75/.70/.60/near-native floors, all 292-member accounting and independent libzstd
dictionary-3 decoding. These are host checks only; no game startup, screenshots, GPU timings or Thor sweep was run.


### Native DRS with a 60 FPS budget (experimental, default off)

For the Odin 3 Gerudo **60 FPS** profile, independently opt in with:

```
TOTK_FPS60=1
TOTK_DRS60=1
```

Unset `TOTK_DRS60` or use `0` for the existing controller. With `TOTK_FPS60=0`, `TOTK_DRS60=1` logs that it is
ignored and changes no DRS code/data, so the shipping 30 FPS profile is unaffected. This targets a **16.667 ms
maximum predicted GPU queue-span budget**; it does not guarantee 60 fps under a CPU/draw-recording bottleneck,
at the resolution floor, or after thermal throttling.

**What the native controller actually measures.** Authenticated 1.4.3 instructions at main+0xa1b8c..0xa1bb8
subtract the game's two GPU counter timestamps, convert their nanosecond difference to microseconds (`× .001`),
then compute `microseconds × 100 / 16666.666`. Thus native “stress” is **percent of one 60-Hz tick**, not
milliseconds: 100 means 16.667 ms, 200 means 33.333 ms. The handheld Bootup palette's `StressThresh=185/205`
corresponds to approximately 30.83/34.17 ms in those units, with `RecoverStress=20/40`, scales .90/.80,
`ReduceCounterMax=3` and `IncreaseCounterMax=60`. Those fields are not a direct “target FPS” setting.

The update at main+0x584300 feeds measured stress into a history/workload predictor, including resolution,
fixed rendering costs and other inputs. Both ordinary updates (+0x584790) and reset decisions (+0x584b68)
call the same level-selection helper at +0x584d70. That helper compares projected cost with a native budget
loaded from the manager's +0xc; the exact constructor-selected numeric budget was not established by this
static trace. **The patch does not assume that field is always 200 or blindly halve 185/205.**

Instead, a guarded veneer at main+0x584e84 uses `min(native_budget, 100.0)` for that helper's comparison and
all its candidate-level comparisons. A pre-existing tighter budget stays tighter. It leaves the actual GPU
measurement, prediction and last-cost mirror untouched, and retains the native palette and hysteresis.
The original load/comparison/store/branch, both decision call sites, stress-normalization instructions and
16,666.666-us constant are checked before any write. The zero-checked +320 text-padding slot follows the
far-clamp veneer; no GPR, LR, stack or flags is changed before the original comparison. Only s17 (original
budget destination) and predictor-local caller-saved s22 are used. No timing/physics/FOV or backend timestamp
is falsified. Unset performs no patch or extra allocation.

**Picture and floor.** This option should prefer lower native scene-resolution levels earlier; UI/output size
stays separate. It reduces GPU pixels, not world draw count. Expect more blur, edge/grass/fence shimmer and
possible resolution pumping during camera movement. The normal handheld floor is still .80 (nominal
1024x576 at a 1280x720 base). If that is insufficient, separately test `TOTK_DRS_MIN=0.75` or `.70`, using
the authenticated local template described above. DRS60 alone needs **no template or regeneration**; an existing
unmodified DRS_MIN template remains valid. DRS_MIN is an independent option and still affects a 30 FPS launch
if explicitly set there; unset it for the unchanged-30-FPS control. Smaller base scenes and lower floors compound
the visual cost; first compare DRS60 alone.

**Timestamp caveat.** Retain the startup line `GPU timestamps for the game's counter reports: on`.
`TOTK_VK_GPU_TIMESTAMPS=0` or `TOTK_VK_OFF=ts`, missing timestamp/host-query-reset support, a busy query-ring slot
or query-read failure can restore the old completion-time reports; their near-zero pairwise duration cannot
drive useful DRS. The on line confirms feature selection, not success of every report. The native pair measures
elapsed GPU queue time, including idle gaps while the CPU records, **not pure GPU busy time**. The existing
backend uses a one-shot clock offset and does not mask queue-family timestamp-valid bits on this path. These
pre-existing limitations are unchanged: resolution can fall without fixing recording-bound fps. Do not use
a diagnostic profiler or missing/failed timing data as proof that the option holds 60.

**Odin 3 Gerudo A/B, not yet exercised:**

1. Use the same warmed save/route, `MOVE=1` camera sweep, weather/time, Turnip/seed, approximately 660-MHz GPU
   profile, panel refresh, frame-lag/recording/presenter settings, brightness and thermal state. Keep
   `TOTK_FPS60=1`; baseline DRS60 unset, then `TOTK_DRS60=1`. Run at least 180 seconds each and repeat in reverse
   order. Retain the single `TOTK_DRS60=1 applied` line and timestamp-on line.
2. Record **every warmed fps window and minimum-window fps**, CPU recording ms/frame, GPU ms/frame,
   draws/passes per frame, actual DRS pass dimensions/level transitions and thermal/power state. Distinguish
   lower GPU cost from unchanged CPU draws. A sustained floor hit or unchanged recorder-limited result is a
   limitation, not a reason to advertise “holds 60.” Then separately repeat with DRS_MIN .75, and .70 only if needed.
3. Take matched baseline/DRS60 screenshots at identical stationary and moving-camera points, with fine fences,
   grass, distant edges, particles, menus and UI/text; inspect blur, shimmer, pumping, crop/output-size mistakes
   and persistent low resolution after an idle recovery. Test Gerudo interiors/menus and return to the sweep.
4. Negative control: `TOTK_FPS60=0`, DRS_MIN unset, compare DRS60 unset versus `1`. Require the ignored log and
   unchanged resolution/pacing behaviour; this is not a new way to uncap the 30 FPS profile.

Host verification: from the project root, run Python with `unicorn` and `lz4`:

```
<python> -B <checkout>/games/totk/scripts/verify_v143_drs60.py \
  --main-nso <local-extracted/v143/exefs/main> --scratch .scratch/drs60/host-checks
python3.14 -B <checkout>/games/totk/scripts/verify_drs_transform.py \
  --romfs-dir <local-extracted/v143/romfs> --scratch .scratch/drs60/template-checks
sh .scratch/cc1.sh <checkout>/games/totk runtime/game_patches_v143.c
```

The native check authenticates the original NSO and every decompressed segment, verifies all patch sites and the
normalization constant, tests default-off/FPS60-off, all word/data/padding mismatches and malformed values, and
emulates the production veneer plus the original native predictor with synthetic workloads. It checks an earlier
resolution reduction, preserved tighter budgets, unchanged cost reporting and register/flag/stack preservation.
The existing DRS template check independently verifies the packed-file integrity/292-member transform. These
host checks do not substitute for the Odin screenshots, actual dimensions and fps windows.
The changed production C passed the Android cc1 syntax check. The DRS60 host fixture passed all nine original
word/data corruption cases, all 20 occupied-padding bytes, disabled/30-FPS controls and five budget-operand cases.
Twenty synthetic native-predictor workloads preserved cost reporting and tighter budgets, with earlier reductions
for the 100-percent cap. The existing DRS_MIN/template integration check also passed unchanged, including
independent dictionary-3 decoding and all 292 members. No device/game-startup or 60-fps performance result is claimed.


### Native DRS headroom for locked frame generation (experimental, default off)

`TOTK_DRS_FRAMEGEN=1` independently retargets the native DRS predictor while
`TOTK_FRAMEGEN=locked` and explicit `TOTK_FPS60=0` are selected. Unset or `0`
leaves the original budget instructions/data unchanged (unless the independently
requested diagnostic snapshot below is enabled). Other modes log that the option
is ignored. `TOTK_DRS60` remains the separate real-60 option: its FPS60-off guard
and this option's FPS60-on guard make their budget patches mutually exclusive.

This is **opt-in**, not a proven quality/performance improvement. Framegen's exact
real/incremental submission brackets and the game's predicted GPU **queue span**
are different measurements. Native prediction includes fixed-cost terms and CPU
queue gaps; lowering scene pixels cannot fix every bottleneck. No real GPU time,
timestamp, FPS-controller input, physics or framegen admission test is falsified.

The guarded main+0x584e84 veneer compares against `min(native_budget,published_cap)`,
using native stress units (six percent per millisecond). Only the appended data
page's aligned float at +48 changes at runtime; executable code is installed once.
It starts with no cap. Once there are valid real samples and a completed measured
incremental sample, the backend targets
`clamp(30 ms - incremental_p90_EWMA - 0.5 ms, 24 ms, 30 ms)`.
Incremental cost comes from the existing nonoverlapping headroom accounting,
including QCOM/LSFG external costs, not an extra profiler or a guessed engine cost.

Active publications are at most once per **wall-clock second**, with a .25 EWMA,
.25-ms tightening deadband, .5-ms recovery deadband, at most 1-ms tightening and
.25-ms recovery per update. The native level/counter hysteresis remains intact.
Sparse budget-overload probes may refresh incremental cost once per 30 real ticks;
their historical measured p90 is retained for at most three seconds rather than
mistaking an unmeasured probe for zero. Framegen off/suspended, missing real data,
no usable scene, external-engine refusal/cooldown, or headroom-feedback cooldown
restores the original native budget immediately at the walking frame boundary.
This means the original budget (normally the shipping ~33.3-ms policy), **not**
an assumed constant written over a possibly tighter native budget. The safety
restoration is exempt from the one-Hz limit; reactivation is not.

The 24-ms clamp bounds quality pressure, not admission: expensive incremental
work or a real-frame floor hit can still drop generated pictures. Camera, QCOM
and LSFG retain the same total 30-ms admission/completion ceiling. When a selected
external engine is unavailable, the camera fallback does not inherit that engine's
DRS cap. This patch adds no Vulkan work or changes to the retained-picture graph.

`TOTK_DRS_MIN` still independently lowers the authenticated handheld ladder floor;
this option needs no Bootup template. `TOTK_RENDER_RES` remains the scene base,
so .90/.80 (or the selected lower floor) compound with a smaller base. Scene
scaling still upscales each real scene once before native UI composition; it does
not alter native DRS levels, the UI/output size, or this budget's units.

With `TOTK_FRAMEGEN_DIAG=1`, both baseline and enabled runs snapshot the native
selected priority and returned scale at the DRS update's common return path
(main+0x5848a8), without reading a guest manager pointer from a host thread.
The snapshot is at data-page **+64**, disjoint from the existing camera aspect
override at **+56** and budget float at +48. Compile-time ownership assertions
and a combined DRS/aspect-veneer regression preserve both the native 16:9 path
and an explicitly selected ultrawide ratio across every DRS level.
`[drs-framegen]` logs level transitions and one-Hz samples: monotonic time, real
frame, active/native budget state, real/incremental p90, smoothed incremental
cost, native priority (`-1` full; `0/1` palette priorities), returned scale, actual
scene-depth and canvas dimensions. The diagnostic veneer is separately guarded
and preserves all live registers/flags/stack except the original `w9` result.
Transitions are observed at real-frame boundaries, not a trace of every guest write.

Host regression: `python -m unittest discover -s tests` includes portable policy
and production guard fixtures; Unicorn/lz4 additionally exercise both veneers.
For original-site authentication and original-predictor synthetic workloads:

```
<python-with-unicorn-and-lz4> games/totk/scripts/verify_v143_drs_framegen.py \
  --main-nso <local-extracted/v143/exefs/main> --scratch <outside-source-scratch>
```

Device acceptance must compare baseline versus `TOTK_DRS_FRAMEGEN=1` with the
user's identical QCOM profile, heavy Lookout-rain/sky-island and village saves,
180-second `MOVE=1` runs. Retain every generated-shown/300 window, DRS/scene-size
time series, real/incremental GPU and real FPS, plus full-resolution recovery and
moving-picture quality. Host emulation/graph identity cannot establish those results.

## Frame rate

The app runs the game's shipping frame controller: a steady 30 fps (`TOTK_FPS60=0` by default). The 60 FPS patch the
Mac build uses lets the controller drop to one display tick whenever a frame's work fits it; a handheld that can't hold
60 then only renders a few surplus frames. On the AYN Thor that was 31.8 fps at 4.94 W against 30.0 fps at 4.81 W.
`TOTK_FPS60=1` in `totk.env` turns the patch on.

The menu's **Frame rate** choices are **30 / 40 / 60 fps / Unlimited**; no preset or device recommendation changes.
**40 fps** is a real-game-update cap, not frame generation, and keeps measured `TOTK_REALTIME=1` game speed.
Smoothest on 120 Hz displays; on 60 Hz it is uneven. The app requests an advertised 120 Hz mode through its
window/SurfaceControl hints, but cannot override the user's peak-refresh setting or other Android policy.
Only an actually observed 120 Hz display clock enables the three-refresh presentation slots; a 60 Hz-only or
60 Hz-limited display keeps a 25 ms producer cap without slots, displaying alternating one/two-refresh holds.
A successful hint alone does not establish 120 Hz or even presentation. Busy scenes may still miss the 40 fps cap;
scripted-camera/cutscene safety enforces a separate 33.333 ms producer floor, not a rounded 25 ms tick.
While that floor is admitted, auto slots use two observed 60 Hz or four observed 120 Hz refreshes; completion
returns immediately to the 40 fps policy. If 120 Hz is unavailable, auto40 requests at least 60 Hz content cadence
to avoid a 30 Hz panel-mode fallback. This changes neither ADPF's work target nor the user's Android refresh policy.
**Qualification:** 120 Hz three-refresh pacing is fixture-tested only, not yet device-qualified.
The Thor and Odin smokes remained at the user's 60 Hz limit; they qualify the uneven fallback, not physical 120 Hz output.

The 40 choice writes `TOTK_FPS60=1`, `TOTK_UNCAPPED=1`, `TOTK_UNCAPPED_HZ=40`, `TOTK_UNCAPPED_INTERVAL=1`,
`TOTK_REALTIME=1`, `TOTK_FRAME_LAG=0`, `TOTK_FPS_AUTO=0`, `TOTK_PACE=0`, `TOTK_PRESENT_SLOTS=auto`,
`TOTK_ACQUIRE_SLACK=0`, `TOTK_ACQUIRE_CLOCK=0`, `TOTK_FRAME_RATE_HINT=1`, and `TOTK_FRAME_RATE_HINT_HZ=120`.
Frame generation switches back to real 30; selecting 40 turns frame generation off. Latency is fixed for 40,
and frame lag stays off. Returning to 30/60 removes all 40-specific lines and the 120 Hz request.
Developer `totk.env` is still read last: explicit overrides, including `TOTK_UNCAPPED=0`, win and can invalidate
the menu's pacing contract. Leave `DM_PRESENT_HZ` and `DM_PRESENT_INTERVAL` unset for the 40 producer path.

`TOTK_PRESENT_SLOTS=auto` is startup-only and opt-in (the menu writes it only at 40); explicit `0`/`1`
retain their existing behavior. It selects slots from the observed display clock, not the advertised mode list.
Measure delivered-fence/SurfaceFlinger interval shares near 25 ms versus 16.7/33.3 ms and record effective
display mode; producer fps alone cannot show even pacing.

**Unlimited** lets real-game frame rate vary with available CPU/GPU headroom, up to the existing **120 fps safety
ceiling**. It may run hotter and use more battery, and is best with a high-refresh screen. Frame generation and
frame lag are off; latency is fixed. The 1.4.3 realtime clock is unchanged (five-sample measured step, 1/15 s hitch
cap, no artificial minimum step that could accelerate jittery frames). Scripted cameras still impose a 33.333 ms
producer floor. This is a variable producer, not a promise of 120 delivered fps or uniform panel intervals.
The menu writes `TOTK_FPS60=1 TOTK_UNCAPPED=1 TOTK_UNCAPPED_HZ=120 TOTK_UNCAPPED_INTERVAL=1`,
`TOTK_REALTIME=1 TOTK_FRAME_LAG=0 TOTK_FPS_AUTO=0 TOTK_PACE=0 TOTK_PRESENT_SLOTS=0`,
`TOTK_ACQUIRE_SLACK=0 TOTK_ACQUIRE_CLOCK=0 TOTK_FRAME_RATE_HINT=1 TOTK_FRAME_RATE_HINT_HZ=max`.
Fixed slots and acquire slack stay off so neither rounds the variable producer to a display lattice. The highest
advertised refresh rate (fractional rates included) is requested through the existing window/AHB FIXED_SOURCE
hints; missing display/query support falls back to the existing content-rate policy. Android/user refresh limits
still win. Returning to 30/40/60 or any preset removes all Unlimited-only lines; frame generation returns to real30.
For qualification, select Unlimited from the menu, warm its pipeline cache, and measure Hateno delivered fps,
frame/fence intervals, actual panel mode, temperatures and HUD-clock progress against wall time. Check a
scripted/title-screen camera separately for the 30 fps floor; an accepted refresh hint is not panel evidence.
Host qualification: 800 Python tests passed (44 optional/configuration skips with the signed release APK),
160 setup tests passed (20 skips), plus native FPS60 patch/floor, present-slot, acquire-clock, display-pace and
acquire-slack fixtures. Settings probes pin the existing rate/preset bytes and every Unlimited transition; rate
fixtures cover integer/max/fractional requests, display migration, JNI refusal and both hint targets.
**Device qualification is pending (Odin locked):** no delivered-fps, interval, panel-mode, HUD-clock/game-speed,
thermal-load or camera-floor smoke result is claimed. The attempted smoke did not reach the menu or game; RC8,
the unchanged 60-fps/GPU-busy profile and stayon 0 were restored, with the device left asleep.

**Benchmark clock (not a frame-rate policy):** launch with `TOTK_BENCH_TIME=HH:MM` (`00:00`–`23:59`),
using the same value in both arms; absent/`off` leaves all clock bytes unchanged and allocates no veneer.
Malformed values log one rejection and stay off. This overrides cutscene/sleep time changes, schedules,
day rollover and blood-moon progression: use restored saves only, never normal play. Weather, wind, clouds
and randomness remain uncontrolled. Retain the single `[totk-bench-time]` effective-time line and qualify
HUD/sky through load, static, pan and reload; unchanged save behavior is not yet device-qualified.

| Frame-rate option | Android default | Effect |
| --- | --- | --- |
| `TOTK_FPS60` | `0` | `1` permits the game's 60-fps controller; `0` retains the shipping 30-fps minimum interval. |
| `TOTK_FPS60_HYSTERESIS` | `0` (native) when unset; menu writes `1` only for Snapdragon 8 Elite-class native 60 fps | Startup-only, FPS60/v143: raise after seven consecutive over-budget decisions instead of three, then reload cooldown 2 instead of 6. `<game>/totk.env` `=0` overrides the menu. Compare interval-2 share/episode length and missed-refresh tails; details below. |
| `TOTK_FPS_AUTO` | `0` (off) | `1`, only with `TOTK_FPS60=1`, selects effective 60/30 presentation intervals from measured sustainability; guest code is not repatched. |
| `TOTK_GPU_TIME` | `hardware` when `TOTK_FPS60` is unset/`0`; `latency` when `TOTK_FPS60=1` | Android shipping-30 mode defaults to replay-free hardware-equivalent GPU reports. Explicit `hardware` or `latency` overrides either mode; unknown values log and retain latency. DRS also consumes these reports; they are not physical busy-time measurements. |
| `TOTK_FRAME_LAG` | Runtime unset: `0`; Android menu writes `1` only for Snapdragon 8 Elite-class native 60 fps | Only exact `1` enables protected lag. First-run and saved 60 fps settings without frame generation gain the line; every other chip/mode retains its prior policy. Developer `totk.env` `=0` wins. |
| `TOTK_VK_CHUNK_DRAWS` | `75` by default; `600` when resolved lag is on and native VMA pressure is absent | Explicit positive value or `0` wins (`0` disables automatic chunking). The resolved startup policy is logged once. |
| `TOTK_VK_SPLIT_PASS_DRAWS` | `0` (off) | `n > 0` ends a still-open pass after `n` draws, submits that part through the chunk path and reopens the same targets with `loadOp=LOAD`. Each split adds a store and load of every attachment on tiled GPUs; try 512 or 256 and read `[split-pass]` every 300 presents (VULKAN.md). |
| `TOTK_VK_REC_THREADS` | `2` on Android; `0` on other platforms | Explicit `0` keeps the synchronous recorder for A/B; positive overrides select up to four workers. `TOTK_VK_GPU_PROFILE` retains workers, including when pass sampling is enabled; other diagnostic guards still force inline recording. Keep profiler and sample/statistics settings fixed when comparing worker counts (VULKAN.md). |
| `TOTK_VK_GPU_PROFILE` | `0` (off) | `1` collects pass-part timings with recorder workers; `2` also logs every split; `3` also retains a per-frame ring and dumps every over-threshold frame's observed passes. Every 300-frame window writes schema-3 aggregates by exact key + scene reference + scene role; ordinal `0` means all frame ordinals. Without `GPU_PROFILE_SAMPLE`, every pass is timed as before. Absolute areas remain available for driver joins and DRS-aware families (VULKAN.md). Compare profiling-off/on separately before interpreting performance. |
| `TOTK_VK_GPU_PROFILE_SAMPLE` | `0` (full profiling unchanged) | With `GPU_PROFILE=1/2/3`, a strict positive integer `1`–`16` limits timestamped/statistics-enabled pass parts per frame; recommended **`2`**, initially with stats off. Deterministic ordinal rotation retains all observed keys, draws and scene metadata. Each frame also has one completion-accurate whole-main-frame timestamp pair; actual profiler-owned `vkCmdWriteTimestamp2` `timestamp_count` (including deferred assembly ends, excluding independent game reports) is at most `2k+2`, without pre/slot envelopes. If Android has already submitted the final command buffer before CPU present, SAMPLE adds **one small marker-only main submission** for the real end timestamp; the query-count bound still holds, but that extra submission has explicit cost. Pass `gpu_ms` is sampled-only, never an extrapolated total. Use the analyzer's whole-interval coverage, sampled mean and histogram percentiles; absent/`0` keeps the old full mode. |
| `TOTK_VK_GPU_PROFILE_PIPELINES` | `0` (off) | With `GPU_PROFILE=1/2/3`, `1`–`16` adds at most `2k` GPU timestamp writes/frame for contiguous actual-pipeline + full VS/FS-variant groups (compute separate), independently of pass sampling. Start with `GPU_PROFILE=1 SAMPLE=2 PIPELINES=4 STATS=0`. Default output is sampled-key 300-frame compact windows with interned exact variants, actual inclusion weights and explicit coverage/loss; global per-frame JSON rows are suppressed, pass/window aggregates remain. Estimates are withheld on true loss/missing authority. Calibrate off/on with the same APK in ABBA order (VULKAN.md). |
| `TOTK_VK_GPU_PROFILE_PIPELINES_RAW` | `0` (compact) | Exact `1` restores per-group `pipeline`, per-lease/frame `pipeline_coverage` and global `frame` JSON rows when pipeline mode is enabled; other values stay compact. Read once and logged as `[gpu-profile] pipeline-output=compact/raw`. RAW is intentionally large and useful for same-run estimator/debug comparisons, not long captures. Pipeline profiling off leaves parent output unchanged. |
| `TOTK_VK_GPU_PROFILE_STATS` | `0` (off) | `1` adds six pipeline counters when supported: IA vertices/primitives, VS invocations, clipping invocations/primitives and fragment invocations. Secondary-chunk queries sum into their pass; missing counters are absent, not fabricated zeros. In sampled mode only selected passes get queries; those queries still perturb sampled passes, so sampled durations are representative only if instrumentation does not serialize/change the GPU workload. Device A/Bs must compare GPU clock time-in-state against an unprofiled control, not only instantaneous MHz. |
| `TOTK_VK_GPU_PROFILE_ORDINALS` | `0` (off) | `1` restores per-ordinal window aggregates; every other value uses compact ordinal-`0` aggregates. Read once at launch and logged as `[gpu-profile] window ordinals=0/1`. Raw mode-2 split logs and mode-3 `long_frame` rows always keep their actual positive frame ordinals. Compare row counts/JSONL bytes with the same route and profile/stats settings; not a measured device-cost reduction. |
| `TOTK_VK_GPU_PROFILE_MAX_BYTES` | `134217728` (128 MiB) | Maximum bytes per JSONL file (4096–1073741824); rotate complete rows through `.1`, `.2`, `.3`, retaining at most four bounded files per launch. Each file repeats launch/build/package/config/driver metadata. Window sequences and rotation indices expose missing/evicted coverage; normal app close writes writer success and independent query-retirement accounting in `session_end`. |
| `TOTK_VK_GPU_PROFILE_LONG_MS` | `50` | Mode `3` dumps every frame with a valid Vulkan GPU span strictly above this threshold (0–60000 ms; decimal values allowed). Each `long_frame` pass row names its frame and positive ordinal. Invalid frame spans/gaps are null with reasons/counters, not usable timing; measured pass sums remain separate. |
| `TOTK_VK_GPU_PROFILE_LONG_FRAMES` | `8` | Mode `3` retains 1–64 frames of raw pass timings, bounded at 16,384 records per retained frame. This controls history memory, not a dump quota: every threshold crossing is emitted, subject only to explicit drops and JSONL rotation. |
| `TOTK_VK_REC_WAKE_BATCH` | `0` (off) | `1`: the walker wakes a parked recorder worker only when none is already on its way; a worker that leaves work queued wakes the next. `n ≥ 2`: also an idle worker polls the queue for up to `n` µs before parking (try 50 and 200). Startup logs the value; `recorder wakes/frame: walker … worker-chained … polled hits … parks …` every 300 presents counts the futex wakes. Live (see Live switches). |
| `TOTK_VK_TEX_MEMO` | `0` (off) | `1` memoizes texture ID → view lookups per thread until the registry, any texture/image identity or the submission changes, and keeps the registry revision on an unchanged re-registration (VULKAN.md, "Texture ID memo"). `[tex-memo]` every 300 presents gives hits, misses, hit rate, retirements and (unchanged) registrations per frame. Live (see Live switches). |
| `TOTK_NVN_CALL_NOCOPY` | `0` (off) | `1`: CallCommands/CopyCommands record a marker holding a reference to the called copy, and the caller's EndRecording copies each body once, straight into its own copy (the default copies it at the call and again at EndRecording, on the game's recording threads). Same copy bytes and reported command memory use; the reference keeps a released called copy alive until then. Not referencing at replay: a shared body would be rewritten by every submit's deferred-bind read and needs lifetimes past the replay. Startup logs `[nvn] TOTK_NVN_CALL_NOCOPY=…`; the `[cmd] recording copies` line counts markers and bytes not copied at the call. `vkdrawtest --nvn-call` runs the probes, a race and a bench. Live. |
| `TOTK_VK_REC_ASYNC_SUBMIT` | On when recorder workers are enabled | Explicit `0` keeps walking-thread assembly/submit. Existing diagnostic and software-protection exclusions are unchanged. |
| `TOTK_ADPF` | Existing render-only session (unset/`legacy`) | `0` disables all CPU hints; `full` opts into the bounded critical-thread group, profile-derived targets and wait-stripped work reporting. |
| `TOTK_ADPF_TARGET_MS` | `33.3` | Existing render-only target override; nonpositive disables the legacy session. Retained for existing profiles; ignored by `TOTK_ADPF=full`. |
| `TOTK_FRAME_RATE_HINT` | `0` | `1` requests FIXED_SOURCE content cadence through the window and zero-copy SurfaceControl. It is only a request, never evidence of a panel-mode change. |
| `TOTK_FRAME_RATE_HINT_HZ` | unset (`0`, off) | Startup-only integer decimal Hz or `max` for the highest advertised window-display rate; needs `TOTK_FRAME_RATE_HINT=1`. Invalid/unavailable rates retain the existing policy. See below. |
| `TOTK_RATE_HINT_HOLD` | unset (`0`) | With `TOTK_FRAME_RATE_HINT=1`: `N` ms requests the window's output rate (game interval, frame generation) held at the highest of the last N ms; 0 keeps the configured rate. Live through `totk.ctl`. See below. |
| `TOTK_ADPF_GPU` | `0` (off) | `1` reports each frame's span (the game's frame origin, else its first submit, to its present, less acquire pacing) against 90 % of the configured period (15 ms native60, else 30 ms), plus on API 35+ the GPU time of the newest completed frame once. Live through `totk.ctl`. See below. |

**Pipeline-profiler qualification limit: ≤30 fps captures** (Duo, or Thor with a 30-fps cap).
The 600-second 30-fps model passes; 45-fps/uncapped Thor is **not qualified**. This is a
capture-support boundary, not an automatic game-rate clamp. Uncapped captures must retain/check
the footer and drop counters; any lost coverage fails closed in analysis. Profiling stays default-off.

The timing profiler has no speculative LOAD-before-clear/STORE-without-observed-read audit.
It retains static attachment load/store operations and logical byte estimates from pass keys.
Its walking-thread draw hook does fixed work: count each draw and hash the bounded shader identities
only on the first draw; pass begin copies at most ten attachments. Neither path scans bindings or
earlier passes, locks/copies the texture registry, allocates or logs. A dedicated `totk-gpuprof`
worker owns result reads, aggregation, JSONL/log output and query resets. Sixteen prepared leases
bound its backlog; exhaustion drops profiling coverage, never waits for output on the game path.
Preparation can wait once at opt-in startup, not per frame.

CPU present publishes an ordered `{frame, end_sequence}` closure independently of an open command buffer.
The private bounded 256-entry closure FIFO lets `totk-gpuprof` finish a sealed-then-present frame only after
all fenced profiler leases through that submission sequence have been processed, including when the worker
consumed them before CPU present. Full mode uses the final slot's real envelope end with no extra GPU query
or submission. SAMPLE needs the marker-only submission described above when no command buffer remains;
an open command buffer retains its in-buffer marker. Closure overflow is explicit cumulative
`background_dropped_closures`, separate from dropped leases; neither is silently repaired.

Orderly close stops admission of new renderer work, flushes/drains renderer GPU work, then drains the
profiler reporter before closing its writer. New `session_end` rows carry boolean `all_queries_retired`
and a `query_retirement` object with acquired/submitted/retired/cancelled/outstanding lease counts and
submitted/retired/cancelled/outstanding query counts. Queries count activated timestamp/statistics query
instances, not reserved pools or the six statistics counters. A retired lease has had its fenced results
read/attempted; only available query results (successful statistics reads) retire queries. Cancelled
recordings were never submitted. `all_queries_retired` is true exactly when both outstanding counts are
zero; `clean_end` records writer success independently. Historical footers without accounting retain
`all_queries_retired: null` as unknown, not assumed retirement.
The enabled GPU drain has one two-second budget; `query_drain_timed_out: true` records expiry and
outstanding counts retain unretired work rather than claiming completion. Profiling-off shutdown is
a no-op: it does not flush renderer work or wait for the GPU.

`gpuprof.py --frame-close-replay` is an opt-in **counterfactual-only** analysis of header-attributed d003f2d
full-mode captures, not device correction: it preserves original invalidity and rejects missing authority,
corrupt/unavailable timestamps, actual drops/failures, gaps and inconsistent sums (tools/bench/README.md).

**Odin overhead evidence:** `.scratch/bench/rpmode/odin-overhead/report.md` records the old
`716c122ff09b` timestamps-only arm at 21.55 fps versus 57.96 fps control: recording rose
10.789 → 43.750 ms/frame while GPU busy fell 80.2 → 36.6%. This is a large CPU-side
instrumentation/feeding perturbation, not evidence that GPU timestamp WFI dominated the FPS loss.
The Turnip timestamp WFI still exists; the benchmark does not isolate its cost from the old CPU
audit/reporting costs. This source cutover is not a new device speedup measurement.
Force-stop normally omits `session_end`; the existing analyzer reports that incomplete closure
without inventing a footer or discarding retained complete-window timing (DEBUGGING.md).

**Keep the GPU busy** (Graphics → Performance) turns on `TOTK_VK_SPLIT_PASS_DRAWS=512` and `TOTK_ADPF_GPU=1`
together. It feeds smaller batches earlier and requests clocks that keep up; a little more GPU and CPU clock costs
power and heat. On by default only on **Snapdragon 8 Elite-class** chips (Odin 3), independently of the preset.
Other chips, including Thor and Duo, default off even when selecting **60 fps**. Off writes neither line, so Thor
and Duo retain 1.0.1's settings.env bytes for every preset. With frame generation both lines are omitted because
extraction cannot split passes. Presets neither change this option nor use it to decide preset identity; resetting
the Performance tab restores its chip default.

A 1.0.0 or 1.0.1 store without this option adopts the chip default, including custom mixes. An explicit stored choice
is never replaced. Unrelated preference metadata is ignored; 1.0.1's update-check setting and pending offer live
in separate private files and are unchanged by this migration.
Restart-based Odin 3 tests raised Hateno 46.5 → 57.3 fps and Lookout 52.2 → 57.1 fps (4/4 pairs);
Rotsumamu was neutral at the 60 fps cap. At the 60 fps preset, the Duo (~27 fps) worsened p95 by 4–5 ms
(4/4 pairs), while Thor (~30 fps) was neutral. These gates limit the default to the measured beneficial chip family.
Compare off/on on the same warmed route after restarting: fps, p95, GPU-clock residency, power and temperature;
retain `[split-pass]` and `[totk-perf] ADPF span` logs.

The **60 fps** preset on Snapdragon 8 Elite-class chips (`CQ8725S`/`SM8750`, the same exact chip-class check as
GPU busy) also writes `TOTK_FPS60_HYSTERESIS=1`. This is internal profile policy, not another visible option:
it follows any 60-fps custom mix, independently of GPU busy, and is omitted at 30 fps or with frame generation.
Existing stores adopt it when loaded without adding or replacing a preference; the menu already rewrites
`settings.env` at game start. Light, Balanced and Smooth retain their previous bytes on every chip, as do all
Thor/Duo/unknown-chip presets. The runtime remains native when the switch is unset, and developer
`<game>/totk.env` `TOTK_FPS60_HYSTERESIS=0` still wins. This branch's default awaits device validation before merging.

### Android performance hints (experimental full mode)

The shipped render-only ADPF session remains the default, including its historical
33.3ms target and raw present-interval reports. `TOTK_ADPF=full` replaces that session
(it does not stack a second hint on the same render thread). API33+ symbols are resolved
from `libandroid.so`; absent/refused support logs once and leaves scheduling to Android.
No CPU/GPU caps, governors, thermal controls or OEM modes are written.

Full mode uses one work-group session for the game main/render threads, NVN translation
thread if enabled, recorder workers, submit-assembly worker and presenter. Framegen runs on the render/presenter
path, so those TIDs cover camera, QCOM and LSFG work without double-reporting synthetic
midpoints. Every 300 real frames, up to six additional busy guest workers (>=15% of one
core over the sampling window) join the group. The registry and TID list are fixed-size;
thread CPU clocks, not `/proc` scans, supply activity. Exiting threads unregister through
pthread TLS cleanup. Membership is capped at 16 TIDs. API34 `setThreads` updates membership;
API33 closes/recreates only on a membership change. Targets update on effective profile
changes: 33,333,333ns for shipping/locked30, 16,666,667ns for native60; effective native
60/30 transitions follow the HLE interval. Actual work is consecutive real game-present
time minus the existing acquire pacing duration; GPU/dependency waits are retained.
Zero/invalid work and >=250ms discontinuities are not reported. Stable frames allocate
nothing in our hint code; session creation happens only at configuration changes.

`TOTK_FRAME_RATE_HINT=1` requests 60fps for locked/native60. Plain30 requests 30fps only
without a physical-clock lattice; fixed present slots, display pacing or acquire clocks
retain a 60fps request so a 30Hz panel mode cannot turn two physical slots into 15fps.
The SurfaceControl request is also applied to the actual zero-copy child, not only its
parent window, and permits seamless switches only. No preferred display mode or Android setting is
written. Panel offerings, request acceptance and actual physical panel cadence must be measured separately
on each handheld. Game frame intervals, framework/SurfaceFlinger fences and physical scanout are distinct:
a successful window return, void transaction or fence diagnostic does not prove a panel-rate change.

`TOTK_RATE_HINT_HOLD=<ms>` (default unset/0 = the configured rate above; 5000 is the design value; live through
`totk.ctl` at the next guest present; needs `TOTK_FRAME_RATE_HINT=1`) requests the window's output rate instead: the
game's own interval in 60-Hz units (60/30/20), doubled while frame generation queues generated frames (real frames at
most 30, so 20 real + generated asks for 40, not a forced 60), at least 60 on the present lattice, and the highest such
rate of the last `<ms>`: up at once, down only after the lower rate held that long. Never the measured present period,
which follows a panel that took a lower request. Logs `[totk-perf] TOTK_RATE_HINT_HOLD=N ms (launch|live)`; each
change still logs `window frame-rate hint=`. Measure the number of those lines and fps dips after 60→30→60 per arm.

`TOTK_FRAME_RATE_HINT_HZ=120` plus `TOTK_FRAME_RATE_HINT=1` is an experimental app-side FIXED_SOURCE
request on both the native window and actual AHB child. Unset/empty/`0` keeps the exact existing hint
policy. Integer decimal Hz is accepted (no signs, fractions or suffixes); the current window's
`decorView.getDisplay().getSupportedModes()` must advertise that rate within 0.01 Hz, allowing float
rounding such as 120.00001 but not substituting a distinct fractional mode. `max` instead selects the highest finite
positive rate advertised by that same display, retaining its fractional Hz through both native hint targets.
There is no default-display fallback. Window creation/recreation and existing readiness/configuration/focus/resume callbacks
revalidate against the current window display; a missing display, unadvertised rate or JNI failure logs
a refusal and restores the old hint policy. A validated override takes precedence over per-frame
content-rate recomputation and `TOTK_RATE_HINT_HOLD`, but changes no game/presentation pacing.
`TOTK_FRAME_RATE_HINT=0` leaves the override ineffective and logs that it requires `=1`.
This is not a preferred-mode/system-setting write and cannot override an OS/user refresh-rate vote.
Treat an accepted hint only as a request; measure game intervals, framework fences and real physical
panel cadence separately before claiming even 40 fps at 120 Hz.

Device comparison arms are unchanged default / `TOTK_ADPF=0` /
`TOTK_ADPF=full` / `TOTK_ADPF=full TOTK_FRAME_RATE_HINT=1`, interleaved on the same
profile, save, shader seed and cache state. Do not flip the full-mode default without
per-layer SurfaceFlinger cadence plus performance, power and temperature evidence.

`TOTK_ADPF_GPU=1` (default off; live through `totk.ctl`, applied at the next presented frame, which is not reported)
changes what the existing session (legacy or full) reports. The span runs from the game's own CPU frame origin, read at
the frame's first submit (the first submit itself when the origin is unusable), to its present, less the acquire pacing
inside it, so a frame reads short or long whatever the present interval was. The target defaults to 90 % of the configured
period and never the controller's current one, so a drop to 30 or 20fps cannot ask for the clocks that keep it there.
On API 35+ `APerformanceHint_reportActualWorkDuration2` and `AWorkDuration_*` are looked up at run time (the build's
API level has none of them) and carry start, total, CPU (= span unless CPU splitting is enabled) and the GPU time of the newest frame whose slots all
completed, each frame once (0 otherwise); below 35 the span alone goes through `reportActualWorkDuration`. Logs:
`[totk-perf] TOTK_ADPF_GPU=N (launch|live): ...; target T` and every 300 frames `[totk-perf] ADPF span: ...`
(reported spans, GPU durations, target). Measure on the Odin 3 with in-session ABBA: fps and the sampler's GPU clock
residency per arm (the GPU sits at its 660 MHz floor 95 % of the time with it off).

`TOTK_ADPF_LOAD_HINT=1` (default 0, live at the next span report) sends an API 36 workload-increase hint before
the duration report after three consecutive frames exceed 105 % of target, at most every 500 ms. CPU is flagged;
GPU is flagged only when the newest measured GPU duration exceeds 70 % of target. Missing API is a logged no-op.
Startup/live mode logs and each 300-frame span summary include hints sent/errors. A/B against gpu_feed alone:
compare fps/p95 and GPU clock residency, with temperature/power held in view.

`TOTK_ADPF_CPU_SPLIT=1` (default 0, live at the next presented frame) reports CPU as the existing span minus
native present/texture queue wait clipped to that span; acquire/vsync pacing already excluded from the span is
never subtracted again. Total and GPU are unchanged. API 35 report2 is required; older APIs log a no-op.
Mode logs identify the arm. Compare CPU clocks/power and p95 against gpu_feed alone; this is wall work minus
known native waits, not the sum of all hint threads' CPU time.

`TOTK_ADPF_TARGET_PCT=50..100` (default 90, unchanged) sets the span-mode target to that percentage of the
configured period; `80` gives 13.333 ms at native60 or 26.667 ms at real30. It never follows a controller slowdown.
Live changes apply at the next presented frame, before reporting, and clear the load-hint streak. Invalid launch
values log and retain 90. Compare fps/p95, GPU clocks and sustained temperature/power against the 90-percent control.

### FPS60 controller, fallback and diagnostics (1.4.3)

`TOTK_FPS60=1` **permits** 60; it is not a forced-60 swap-interval override. There is no constant
“60 FPS variable” patched in memory. Seven checked ARM64 instructions in the controller at
`main+0x2db174` are changed; `TOTK_FPS60=0` skips all seven:

| Main offset | FPS60 instruction / purpose |
| --- | --- |
| `0x2db204` | `cmp w10,#1`: minimum desired interval becomes one tick. |
| `0x2db208` | `csinc w10,w9,wzr,ge`: floor desired at one, retaining the upper clamp. |
| `0x2db22c` | `nop`: try the measured lower candidate instead of first loading the stored shipping candidate. |
| `0x2db360` | `cmp w13,#1`: lower-candidate floor on the raise path. |
| `0x2db36c` | `csinc w12,w13,wzr,gt`: next candidate never below one. |
| `0x2db340` | `mov w8,#1`: lower-path minimum after callbacks. |
| `0x2db34c` | `csinc w21,w10,wzr,ge`: final interval never below one. |

The original calculation is `trunc(max(workA*60+biasA,workB*60+biasB))+1`, with work seconds from
`sys+0x120` and `sys+0x128` and biases at controller `+8/+0xc`. The controller
retains its upper clamp, over-budget counter, cooldown, listener callbacks and listener veto.
Controller-relative `+0x40/+0x44` hold current/candidate ticks, `+0x48/+0x4c` are over/cooldown counters,
`+0x50` is `60/current`, `+0x54` is its selected FPS target, and the byte at `+0x58` freezes adaptation.
A nonpositive `sys+0x118` also skips adaptation. The resulting interval is sent through the indirect NVN
call at `main+0x2db2e8` (LR `+0x2db2ec`), cached at `*(sys+0x150)+0xa8`, and stored at `sys+0x88`.
Over-budget work, a freeze/explicit policy change, or a listener veto can therefore legitimately retain 30.
Stored `min/max=2/3` with FPS60 enabled is **expected**: the patch changes comparison/select
instructions, not those configuration words. Native emulation explicitly checks that interval 1 is
selected while the stored words remain 2/3; seeing 2/3 is not evidence that the patch is absent.

**Odin dip policies (both startup-only; runtime defaults off, with hysteresis enabled by the Odin native-60 menu profile):**
`TOTK_FPS60_HYSTERESIS=1` requires FPS60/v143 and changes three guarded controller loads:
over-limit 6 (raise on the seventh consecutive over-budget decision, versus native limit 2/third),
cooldown reload 2 versus native 6. Work/bias math, min/max, freeze, listener veto and interval setter
remain native; fixed experiment limits replace configuration overrides, while logged `limits` remain stored words.
Compare interval-2 frame/time share and episode length, not only mean fps; shorter cooldown can increase chatter,
and tolerating late work can produce missed-display judder. No 60 interval is forced under sustained overload.

`TOTK_GPU_TIME_FPS_MEDIAN=1` requires `TOTK_GPU_TIME=hardware` and filters only the two known FPS END
consumers with a three-unique-report median (no synthetic warmup zeros; duplicate reads do not advance it).
Invalid reports hold the last filtered work; DRS/absolute timestamps/raw GPU profiling are unchanged.
Startup logs the effective mode and `[fps-policy] GPU FPS median frame` reports raw/filtered means and changed reads.
An isolated spike is rejected, but a sustained rise passes on its second sample; recovery also needs two samples.
This is a controller-noise experiment, **not** evidence that hardware timings are wrong, and cannot fix CPU-triggered dips.

The existing `[fps-policy]` decision logs distinguish these causes: on Odin shadow A2,
frame 7302 drops with CPU/GPU 14.063/18.584 ms and recovers at 7470 (168 interval-2 frames);
frame 8555 drops with 19.587/12.875 ms and recovers at 8694 (139 frames).
At frame 7500 the hardware producer means are 14.403/11.561 ms before/after removal,
with zero fallback/clamps/invalid/withheld samples; the mean cannot disprove a transient over-budget input.
The ~33-ms cadence is a game policy result, not a shader-compile count or raw GPU busy duration.
Setters and 300-frame summaries do not retain all preceding inputs, so the native three-late rule comes from
instruction emulation, not an invented per-frame trace. Boot/scripted-camera floors must be excluded from pans.

The pacing-aware analyzer classifies a 60-target window as a dip when at least 60/300 measured intervals
are 29..37.5 ms (otherwise it uses a clearly labelled incomplete-coverage fps fallback). Its Odin shadow
re-analysis retains 31 windows per arm: A/B each have seven dip windows, taking 29.31/29.45% of measured
window time, with 1,348/1,331 near-33-ms frames. Steady fps is 57.35/57.52 versus time-weighted
52.29/52.36 overall: avoiding those windows could recover about 5 fps, not evidence of faster steady rendering.
That **dip-window time fraction** is distinct from interval-2 frame share/time and extra-refresh tax.
Near-33 cadence can also include missed refreshes at interval 1; do not identify every such frame as a controller drop.

Device qualification: use the same release APK, Recommended60 preset, 60-Hz panel, GPU busy On,
Hateno save/view, warmed seed, fixed resolution/DRS and recording threads. Fresh launch each arm:
control `TOTK_FPS60_HYSTERESIS=0 TOTK_GPU_TIME_FPS_MEDIAN=0`; then hysteresis-only `1/0`;
then median-only `0/1`, each interleaved ABBA. For every fresh launch run the existing
`tools/bench/pan.sh -s <odin-serial> --device odin --scene hateno --out <arm-dir> --target-fps 60 --repeat 3`;
retain all three pans, with the script's target-aware duration and settle, then analyze with the pacing-aware
`tools/bench/analyze.py`. Compare `dip_time_fraction` and `fps_steady` separately; mean fps is informational.
Keep `TOTK_GPU_TIME=hardware`, `TOTK_FPS_AUTO=0`, `TOTK_FRAMEGEN=0` and thermal/ADPF settings identical.
Require the switch startup logs, controller=sys+0x200, zero hardware fallback/invalid/withheld, and no cutscene floor.
Join every drop/recovery decision to immutable interval-1/2 cadence, CPU/GPU scores and GPU FPS median summaries;
report the analyzer's dip-window fraction/steady fps alongside interval-2 frame/time share and extra-refresh tax, plus p95/p99/>50ms,
physical GPU ms/DRS, CPU/GPU clocks, power and temperature. Reject apparent fps gains with worse missed-refresh
tails, altered time/physics, freeze/veto violations, sustained report withholding or rising thermal cost.
Only test `1/1` after each independent arm qualifies; no device result is claimed by these host experiments.

**Thermal frame step (Android, opt-in):** `TOTK_THERMAL_STEP=1` (default `0`, startup-only)
raises the v143 FPS60 controller's minimum result to two 60-Hz ticks when the running interval is one
and thermal status reaches SEVERE (3), or the 10-second headroom forecast reaches 1.0.
A nice-10 sampler calls the optional NDK status/headroom APIs via `dlsym` at most once per second,
only when enabled. Off/unset installs no thermal veneers and starts no sampler; the original FPS60
controller bytes stay unchanged. API30 status-only devices can cap; unavailable/NaN headroom cannot
prove recovery. No platform thermal settings are written.
Recovery releases our floor only after 30 continuous seconds of samples below MODERATE (2) and
headroom below 0.8; missing/stale samples restart that evidence. Change the switch only between launches.
The existing game controller still owns cooldown, vetoes, simulation step, and window interval:
no present delay, forced-60 recovery, or shortening of game-requested slower intervals is introduced.
The three FPS60 select veneers are installed only at enabled launches, with an aligned floor word;
shipping 30 (`TOTK_FPS60=0`) and other game versions have no thermal guest-floor hook.

Host-side device check (when devices are free): keep the 60fps preset and one loaded scene, collect
separate sustained five-minute off/on launches with the existing bench sampler (its thermal-zone
temperatures), and join the enabled arm's `totk.log` `[totk-thermal] last300 status=... headroom=...
floor=... guest_interval=... transitions=...` lines to `[fps-policy]` game/window decisions.
The off arm has no thermal sampler/last300 lines. Status uses NDK values (-1 unavailable,
0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency, 6 shutdown); headroom is the forecast,
not a temperature. Expect a 60→30 game interval at the hot threshold, no extra presenter hold, and
no release before 30 seconds of qualifying cool readings. Count transition chatter and compare
temperature slope, p95, >50ms frames and sustained cadence; lower peak fps is intentional, not a win
in mean fps. Do not infer thermal status from thermal-zone temperatures alone.

**Fade increments and cutscene pacing:** whenever FPS60 is enabled, four guarded fade-increment
hooks keep a positive fractional increment from truncating to zero. They are default-on in FPS60 mode,
including hybrid and other frame-generation modes, and are independent of `TOTK_FPS60_HARDENING`.
`TOTK_FPS60_HARDENING=0` disables only the diagnostic markers, camera-exit hook and guest/presenter
30-fps floor. That floor retains its mode gate: FPS60 enabled, frame generation unset/empty/`0`,
and hardening not `0`. `TOTK_FPS60=0` leaves all ten fade/floor sites untouched, including Android's
default shipping-30 profile. The raw runtime defaults FPS60 on when unset; Android supplies `0`.
All ten original patch words, six adjacent predicate/continuation words and the complete reserved
veneer/data padding are checked before installing any fade/cutscene hook in FPS60 mode.
`TOTK_FPS60_FADE_CLAMP=0` (default on) leaves only the four fade sites original for a matched
same-APK control; the floor group keeps its own gate and every guard is still checked.

The four generic entries are **not** proof of an active cinematic. Their individual timestamps are
diagnostic only; ordinary reset/type-query/update calls cannot renew either floor. Admission is now
limited to the known **`ExecuteCameraMoveCameraAnimationInstructions`** AI action at `main+0xa5a188`,
with a nonnull instruction pointer at object `+0x20` and completion bit31 clear at `+0x18`.
The original update immediately returns for null instructions; the guarded completion store at
`main+0xa5a670` sets that bit. The new completion veneer clears the shared guest/presenter stamp
on that same call; null/already-completed entries also clear it. If updates stop without that exit,
the admitted stamp still expires at **200 ms** (future timestamps do not admit a floor).
Both the guest interval join and physical presenter consume that same admitted stamp, not the
diagnostic entry timestamps. This is a conservative **scripted-camera-action subset**, not universal
cutscene detection; opening and other cinematic coverage still need device qualification.

`[fps60-cutscene]` logs changes in admission/release/expiry, marker freshness, guest interval choice
or physical floor decision. Each transition includes all four source offsets/meanings, their latest
timestamps/ages in counter ticks, the admitted stamp/window, and `guest=requested->applied` /
`present=requested->applied`. Stable refreshed timestamps are coalesced, not logged every present.
No extra Odin or Thor run is implied: separately qualify ordinary Odin native60 openfield (no floor
from generic markers) and Thor opening-cutscene entry/exit before claiming device coverage.

**Returning callbacks are not, by themselves, a stuck-30 bug.** The NOP at `+0x2db22c` keeps the measured
candidate in `w10`; after a returning listener, `+0x2db33c` reloads controller `+0x44`. This can retain
two ticks for one frame. Crucially, the continuation at `+0x2db3f0` lowers the stored candidate with the
patched floor and the next eligible frame recovers. Persistent instruction emulation with a returning
listener gives `2,1,1`, not a permanent `2,2,2`. A speculative candidate-publication change was rejected
after this negative result. No game patch or presentation policy is changed by this diagnostic work.
Static evidence is from the local Android rewritten main SHA-256
`bad348845b8f786edfe27c3c44b97528727602e33fea978dd5e55987a981c5ee`, controller bytes
`+0x2db174..+0x2db42f`; Ghidra evidence `ev_854a58daed0729e5bbfc7144486949328ba22baad9ac94e460a2506cc617d569`.
The isolated slice does not identify the runtime listener implementations or observe scene writes.

The independent, default-on `TOTK_REALTIME` patch replaces the nominal interval producer at `+0x3e5658`
and the global ratio producer at `+0x2daa54..+0x2daa78`. A private appended data page holds measured
`{seconds*30,seconds,seconds*60}`; the five-present moving mean is bounded to 1/120..1/15 seconds,
discarding gaps outside 1..250 ms. The original half-step selector (`sys+0x1da` bit 0), time scale and
FrameRateMgr arithmetic remain. Thus a real 30-fps fallback advances time at real speed, not half speed.
The diagnostic `framestep` input can pin this measured step; it does not change the NVN interval.

**Host pacing chain:** window-builder defaults use interval 1; builder setter/initialization copy the
game request; `nvnWindowSetPresentInterval` updates it without an FPS60 clamp. Acquisition uses
`present_ticks`/`present_period`; ordinary Android game mode retains the requested interval. AUTO may
lengthen it to 2 at an acquire boundary, never shorten a guest request. Fixed-clock/slack/predict paths
share measured display refresh; legacy acquisition uses its availability/pacer clock. With slots on,
`vk_draw.c` converts the requested seconds to display ticks and attaches an immutable interval/desired
slot to each AHB or copy picture; predict uses its acquire-owned plan instead. The presenter does not
reinterpret an old slot when the game changes policy. There is no separate EGL/Vulkan swap-interval
setter in this backend: presentation is AHB SurfaceControl or ANativeWindow copy, not a Vulkan swapchain.
The old one-time `window present slots: requested 33.333 ms, interval 2` line is only startup state.

`TOTK_PACE=display` connects acquisition and VI events to Choreographer; the normal `TOTK_PACE=0`
profile keeps VI on the separate synthetic 60-Hz absolute-time thread, even with real display slots.
`TOTK_ACQUIRE_CLOCK=vsync` alone does not change VI. `DM_PRESENT_HZ`, `DM_PRESENT_INTERVAL`,
`TOTK_UNCAPPED`, deterministic timing and interpolation are alternate pacing inputs; Android physical
clock modes retain their existing conflict rules. `TOTK_FPS_AUTO=1` is a separate opt-in 30 selector;
it was **0** in the reported fallback run. Frame generation is another, explicitly real-30-only policy.
DRS/render size affects the measured work, not this controller's minimum.

**Always-on, bounded-cost evidence:** `[fps-policy]` logs builder/window interval changes with object,
frame, old/new and first available guest main call PC/LR; initialization logs its copied interval.
`HLE effective ticks` and `presenter effective interval` log only transitions, including the
guest/AUTO/interpolation policy and the AHB/copy/plan context respectively. Capture saved guest `x19`
at the known NVN call and derive **controller = sys+0x200**, authenticated by original instructions
`main+0x2daa48..50`. Saved caller `x20` is also `sys`, **not** the live controller: diagnostics in
commit `75d3129` incorrectly used it, so disregard their target/freeze/counter fields (interval transitions
and work inputs remain valid). The corrected regression executes the original caller and checks this
negative control. Sample six target words and the freeze byte at acquire; changes log old/new.
Every 300 frames, and at every changed window setter decision, report targets, counters, limits,
listener presence, completed work ticks/ms, both biases, scores and one-tick work thresholds.
At present, CPU work is still partial; acquire/decision sampling sees the completed inputs actually
used by the next controller call. Samples say **writer PC/LR unavailable**: they cannot identify a
transient write between samples, and the sampling PC is not a writer. No watchpoint, page protection,
per-frame allocation or frame-time float-change logging is introduced. Setter frame numbers
name the next window present; presenter frame numbers name backend pictures, so retain both streams.

**Exact work provenance and threshold.** `main+0x2daba8` writes CPU work (`sys+0x120`) as
GetSystemTick minus the frame origin (`sys+0xe0`); frame-end `+0x2db930/+0x2db9e4` adds the measured
pre-/post-wait callbacks. The explicit sync waits between them are excluded, but work/waits inside
the callbacks are not. Present occurs inside the second callback, before its duration is added.
Vtable `+0x118` implementations `main+0x27bcdcc` / `+0x2b3cff8` convert the second GPU counter report
to ticks and write **GPU end timestamp minus CPU frame origin** to `sys+0x128` (`+0x27bce64` /
`+0x2b3d090`); it is not GPU end-minus-start busy time. Recording/submission delay, queue backlog
and GPU execution can therefore all contribute. `sys+0x118` receives max(CPU work,GPU work).
The native constructor `+0xbdc060` initializes both biases to float `0x3d4ccccd` (~0.05), over-limit
threshold 2 and cooldown 6; `+0xbdbeec` sets min/max 2/3 and permits configuration overrides of
biases/cooldown. At bias 0.05, one tick requires **work <15.83333332 ms** (not 16.667 ms):
`trunc(work*60+0.05)+1` becomes 2 at the boundary. Corrected logs print actual runtime overrides.

**GPU clock fix, not a forced-60 patch.** Previously `apply_posts` calibrated the first GPU query
to CPU fence-completion time once. That permanently added the first query's remaining submission,
GPU wait and completion-thread wakeup delay to every absolute GPU report, inflating `sys+0x128`.
The first EXT-clock implementation (`183d029`, operator APK `320bb5c8`) failed on Odin 3: captures
`o-fps60c-insa-a`, `o-fps60c-ge-a`, `o-fps60c-insa-b` select interval 3 with GPU work near device uptime.
In insa-a, frame 3 reports CPU/GPU 4.858/224104645.829 ms, increasing about 15 s per 300 frames.
This is not real work. **There is no process-start subtraction in this runtime's CPU clock**:
`runtime_dispatch` timer gates use `runtime_now_ns` (`host_now_ns`, CLOCK_MONOTONIC) at 19.2 MHz;
the SDK's GetSystemTick is a rewritten MRS followed by RET.

The selected Turnip source advertises calibrated timestamps, but its KGSL
`kgsl_device_get_gpu_timestamp` is `UNREACHABLE(""); return 0;` without writing the DEVICE output.
Mesa's common calibration calls it and sees success with an undefined device sample. Therefore
Android Turnip must not call that unsupported path, even though the extension is advertised.
Before real submissions or the completion worker exist, bracket three **empty timestamp-query
submissions** with the guest timer's own clock provider. Select the narrowest bracket and use its
midpoint; log the honest half-width uncertainty, GPU sample and guest/host nanoseconds. This bounds
initial epoch error rather than silently adding an arbitrary real submission's completion delay.
It adds three startup submissions, no per-frame wait or periodic queue drain. Drivers with functional
DEVICE calibration may then refresh the EXT pair once per second, explicitly rebasing its host sample
through a contemporaneous host/guest pair. On KGSL the startup pair is retained; long-term drift is
not bounded by the initial half-width and remains part of device acceptance.

`guest_clock.h` now centralizes the 19.2-MHz conversion shared by timer gates, calibrated/fallback
GPU counter reports, their nanosecond conversion, and DeviceGetCurrentTimestampInNanoseconds.
No process epoch is invented or uptime subtracted. Host tests deliberately put the guest clock eight
hours ahead of CLOCK_MONOTONIC; a real GPU report minus guest CPU origin must still be a small
positive duration. Pure tests also cover a guest clock starting at three seconds with a host uptime
of 62 hours, signed deltas, valid-bit wrap and periods. Deliberately disabled reports/ring overflow
still use guest-clock completion time. The fix does not force an interval or bypass hysteresis;
the corrected device result remains to be measured. Actual device biases are CPU 0.05/GPU 0.02,
so the printed one-tick limits are 15.833/16.333 ms.

**Recorded evidence / hypothesis ranking:**

1. **Inflated absolute GPU end timestamps plus genuine scene workload:** the backend clock-epoch
   defect above is proven in code, but its magnitude in the old captures is unknown. Correct calibration
   must precede concluding that all reported work is real. `o-r11f-opt-b` has real guest `1 -> 3 -> 2`
   transitions, then no return to 1 after world load around backend frame 2398 (351 ms, recording
   329.5 ms, seed reads 24.6 ms). At 2700/3000 GPU is 12.2/12.4 ms and recording 12.3/11.7 ms;
   these do not prove GPU-end-minus-CPU-origin or CPU work fits the biased one-tick threshold.
   Inspected insa logs first run the logo/menu/light scene at 58–59 fps, then load the world; their early
   60-fps windows are not proof that Great Sky Island itself was sustainable at 60. Insa at frame 3000
   has recording/GPU 16.1/20.1 ms (native), 16.4/19.8 ms (SGSR540), 16.3/21.5 ms (native-UI540).
   Even the named “540” native-UI run logs a 1440x808 depth target before later DRS reductions.
2. **Guest freeze/scene target rewrite or intentional listener veto:** possible, not observed in old
   logs; the new freeze/target/candidate samples distinguish these from a backend-only change.
3. **Backend-only interval lock / refresh / AUTO:** least supported: guest interval transitions already
   exist, AUTO is off, refresh stays near 60, and immutable interval-2 fence samples become all 300.
   Fence timestamps are framework signals, not guaranteed physical scanout.
4. **A missing callback-recovery patch:** specifically disproved for a normally returning listener by the
   persistent ARM64 regression. Intentional veto/target modifications remain possible, not established.

| Log | FPS at frames 600 / 900 / 1800 / 2100 / 2400 / 2700 / 3000 |
| --- | --- |
| `o-r11f-opt-b` | 59.2 / 59.9 / 59.9 / 45.4 / 50.8 / 29.5 / 30.0 |
| `o-r10-insa-native` | 57.8 / 58.4 / 58.0 / 43.7 / 53.3 / 36.4 / 29.9 |
| `o-r10-insa-sgsr540` | 58.9 / 58.8 / 58.6 / 44.0 / 51.3 / 44.9 / 29.9 |
| `o-r10-insa-nui540` | 59.5 / 58.5 / 58.1 / 44.5 / 46.7 / 35.8 / 29.9 |
| `o-r11f-orig-b` control | 59.6 / 59.9 / 59.9 / 48.1 / 46.5 / 46.6 / 59.9 |
| `o-r11f-opt-a` control | 59.3 / 60.0 / 59.9 / 45.1 / 55.1 / 40.3 / 51.2 |

In opt-b, interval-1/2/>2 picture counts at 2100, 2400, 2700, 3000 are respectively
260/18/22, 270/27/3, 0/291/9, 0/300/0. At 3000 native-fence p50/p95/p99 are
33.371/33.378/33.381 ms and apply-late is zero: the pictures were *requested* every two refreshes,
not simply every other one missing its deadline. Draws jump from about 121/frame at 2400 to
2372/frame at 2700; resident memory rises 2204 -> 2643 -> 2759 MiB over 2100..2700. The only
nearby depth-size sample at 2506 retains 1280x720; no exact DRS level or game scene name was logged.
CPU-boost request changes are present (the host treats these as informational, not governor commands);
there is no recorded game message proving an area-specific frame-rate write. Insa's long 1.9–2.1 s
startup frames and later world-load shader bursts are likewise real, not an unexplained silent backend cap.
The opt-a control avoids a permanent 30 lock but is ~51–54 fps in later world windows, not steady 60;
orig-b recovers to 59.9–60.0. Do not pool menu and world measurements.

Four operator captures of the first diagnostic build (`o-fps60d-insa-a/b`, `o-fps60d-ge-a/b`,
Odin 3 ~660-MHz GPU profile) contain **236 guest interval changes**, each matching the HLE and
presenter transition; all use call `main+0x2db2e8`, LR `+0x2db2ec`. Insa-a's final `1 -> 2` is
frame 2489; insa-b's final `3 -> 2` is 2499, both staying 2 through frame 10200. Gerudo-a/b
have 106/85 changes and repeated recovery; completed two-tick holds peak at 37 frames. Insa workB
rises from 2.18 ms at frame 2400 to 26.57 ms at 2700 and stays roughly 23–36 ms; reported workA
is ~12–20 ms but is partial in those old present-phase samples. Recorder GPU world cost is
~16.4/19.7 ms versus Gerudo ~9/12 ms, supporting real workload differences without quantifying the
epoch error. Invalid controller fields cannot establish a freeze, veto, cooldown or actual bias.
No backend-only interval divergence is observed. Retain full `[fps-policy]` sequences, not just
the final transition.

**Exact Odin 3 repro / acceptance (operator-run; not performed here):**

1. Install the runtime containing the `[fps-policy]` diagnostics; retain the existing driver, warmed seed,
   GPU/governor profile, refresh **60 Hz**, resolution/DRS settings and recorder worker count. Select the
   `slot_00-insa` fixture using the existing fixture-save procedure, preserving the user's real saves.
   Use `TOTK_FPS60=1 TOTK_FPS_AUTO=0 TOTK_UNCAPPED=0 TOTK_REALTIME=1 TOTK_FRAMEGEN=0`,
   `TOTK_PRESENT_SLOTS=1 TOTK_ACQUIRE_SLACK=1 TOTK_ACQUIRE_CLOCK=0 TOTK_PACE=0`,
   `TOTK_FRAME_LAG=0 TOTK_VK_REC_THREADS=2`. Leave `DM_PRESENT_*`, deterministic timing and interpolation
   unset; keep `TOTK_DRS60` identical in both runs (off for the original failing profile).
2. Fresh-launch for each trial. Require `seven verified controller instructions`, the resolved policy,
   calibrated ~60-Hz clock and AHB success lines. Require `[fps-policy] ... controller sys=... controller=...`
   with nonzero pointers and initial `WindowSetPresentInterval` call/LR; the adaptive call should be
   main+0x2db2e8 / LR+0x2db2ec. If capture is absent, retain the complete log rather than assuming zeros.
   On Odin/Turnip require `bypassing unsupported Turnip/KGSL DEVICE calibration`, then
   `GPU timestamp clock: bounded guest-clock query`; record its half-width uncertainty and raw samples.
   EXT calibration is expected only on supported drivers. Require small completed GPU work, never
   uptime-sized or increasing with wall time. Check `controller-sys` is exactly `0x200`, stored min/max
   2/3, plausible current/candidate/FPS fields and actual bias/limits. Compare early/late work to watch
   startup-pair drift during the full run.
3. Record from launch through the actual loaded island for **180 seconds after control is available**,
   at least frames 2100..3600. First hold camera still, then repeat the same sweep, menu-open/close and
   return to the quiet view. Repeat a fresh run, then the Gerudo fixture/route that produced opt-b.
4. Around each `WindowSetPresentInterval 1 -> 2/3`, read the same-frame `decision` work/bias/score
   line, target/freeze changes, `HLE effective ticks` and `presenter effective interval`. After loading
   clears, work below each printed one-tick threshold with cooldown zero and frozen=0 should allow
   `2 -> 1`; do not force 1 for genuine overload or override a veto. Record per-300 recording/GPU/
   depth sizes and fence cadence. Compare fresh-launch corrected and old builds at identical settings:
   old epoch inflation can vary with first-submission completion latency, so repeat both run orders.
   Guest=1/HLE=1/presenter=2 implicates backend quantization/plan policy; guest=2 with frozen!=0 or
   high work implicates guest policy/workload. Candidate=1/current=2 with persistently low work and no
   cooldown calls for listener/veto investigation, not a blind forced swap interval.
5. Negative control with `TOTK_FPS60=0`, otherwise identical: require the shipping-controller line and
   unchanged 30 mode. Check game clock, physics/animation speed, pause/resume, scene/menu recovery
   and no increased drop/fence-error rate in both modes. Archive full logs, not just overlay FPS.

Host regression (Python with `unicorn`, caller-owned Android rewritten main; output outside `build/`):

```sh
python games/totk/scripts/verify_v143_fps60.py \
  --image /path/to/rewritten/v143/main.patched.bin --text-end 0x2bb9c00 \
  --out /path/to/scratch/fps60-test
taskset -c 0-3 nice -n 10 python games/totk/scripts/verify_v143_fps60_hardening.py \
  --image /path/to/rewritten/v143/main.patched.bin --text-end 0x2bb9c00 \
  --out /path/to/scratch/fps60-hardening-test
make -C games/totk/android fps60patchtest TARGET=linux CC=clang OUT=/path/to/scratch/fps60patch -j4 && \
  /path/to/scratch/fps60patch/fps60patchtest
clang -std=gnu11 -Wall -Wextra -Werror games/totk/android/gfx/test_vk_timestamp.c \
  -o /path/to/scratch/test-vk-timestamp && /path/to/scratch/test-vk-timestamp
```

This compiles the production patch fixture, executes the real controller with empty/returning/veto/
candidate-changing listeners and persistent light/medium/heavy/recovery workloads. It checks
frozen/cooldown/overload/shipping controls, the missing-lower-trigger negative control and all seven
instruction guards; a returning callback must recover rather than be labelled a permanent lock.
The hardening regression separately covers the FPS60/framegen/hardening gate matrix, fade-only
installation with unchanged camera-floor sites/data, one reserved veneer region, all patch and
predicate-word/padding refusals with the floor enabled or disabled, and original fade semantics.
It checks that generic openfield entries retain interval1, active scripted-camera updates floor
both pacing paths, completion/null entries release immediately, and stale admitted stamps expire
without renewal from generic entries. It also checks register/flags preservation, guest-decision
publication, 60/120-Hz HLE behavior and coalescing across 200 stable timestamp refreshes.
These are the host checks to run for the camera-admission change; no new host/device result is claimed here.
`fps60patchtest` needs no game data: it runs the production fade/floor patcher on a synthetic main
with only the expected 1.4.3 words and checks the 72-case gate matrix, veneer entry/replay/return for
all ten sites, fail-closed refusal of each of the 16 single-word mismatches, padding/neighbour layout
and the admitted-stamp presenter floor.
The controller regression passed all 12 persistent candidate/cooldown/listener combinations, four
listener-boundary cases, bias/independent-work controls and seven guard refusals. Lavapipe host
regressions passed 23/23 (`vktest`), 103/103 (`vkdrawtest`) and 109/109 (`vkdrawtest-app`),
including real report-minus-origin checks with an eight-hour guest clock offset. Startup query
half-widths were 30–38 microseconds on lavapipe; supported EXT refresh reported 1 ns deviation.
Android NDK 28.2 API-33 `-fsyntax-only` passed the changed production C files; the shared HLE retains
existing format warnings (the Makefile normally suppresses these). No APK build or device run was made.


**Hardware-equivalent GPU reports (Android shipping-30 default; explicit override):**

`TOTK_GPU_TIME=hardware` changes guest-visible GPU report timestamps, not physical GPU speed.
Android shipping-30 mode (`TOTK_FPS60` unset/`0`) defaults to hardware; Android FPS60=1 and
other platforms/BOTW retain latency unless explicitly overridden. Unset or empty GPU_TIME selects
that startup-mode default; `TOTK_GPU_TIME=latency` opts out, explicit hardware opts in in either mode,
and unknown nonempty values log and retain latency. AUTO falling to 30 does not change the startup clock.
The lagged FPS-consumer ownership fix was accepted on Thor with zero negative controller inputs;
two isolated samples were held among 8,100 consumer reads. This validates the numerical fix,
not universal DRS/load equivalence or a performance improvement. Preserve physical GPU/query,
KGSL utilization, resolution, power and cadence comparisons; sustained holds, compressed/wrapped
DRS pairs, missing backlog or elevated physical load invalidate an unqualified timing comparison.

The latest Thor Lookout comparison supports lag off plus hardware reports (29.93 fps, minimum
29.6, 1.4% long frames), rather than lag on plus latency (27.71 fps, minimum 22.6, 13.2% long).
Latency-mode reports overstated GPU time by about 9–12 ms; hardware reports around 23 ms agree
with KGSL's roughly 22.7 ms. See `docs/translation-research.md` for the device table and route data.
Earlier Odin 3 native-UI-v2 Gerudo captures motivated separating host replay delay from GPU work:
native UI on requested interval 2 for 21–41% of frames versus 3–5% off despite lower GPU busy share.
That history does not prove every dip artificial or establish sustainable 60 fps.

Capture the original NVN QueueSubmit entry time before deferred-handle resolution, capture, lock wait
or asynchronous replay. Carry it and the then-current CPU frame origin through the replay job and
each Vulkan chunk. At ordered completion, put measured GPU execution on a virtual queue:
remove host recording gaps, retain real guest-submit idle gaps and accumulated GPU backlog. Overlapping
TOP/BOTTOM intervals are preserved rather than summed twice; backend-only upload/present slots add
GPU execution but no late host-CPU time floor. A query keeps the **chunk that wrote it**, calibration
pair and CPU origin until its carried post is consumed, even after that chunk's ring slot is reused.
Frame lag changes delivery/visibility, not the timestamp owner. Mapping stays in absolute guest-clock
nanoseconds, then converts back to 19.2-MHz report ticks. Reports are clamped monotonic on the one
actual Vulkan queue and never earlier than the owning guest submit.

The v143 FPS END-getter consumers (`main+0x27bce44` / `+0x2b3d070`) subtract the
**live** `sys+0xe0`, even when their selected report belongs to an older frame. With frame
lag, a valid virtual END can precede that newer origin (Thor: controller frame 3289,
`-309648` ticks / `-16.128 ms`, while the frame-3300 producer-origin summary remained positive).
Global timestamp monotonicity cannot fix this ownership mismatch. For **only these two callers**,
the getter now matches the report address **and timestamp** to its completed producer origin,
computes END minus that origin, and transports that measured duration to the live subtraction
origin. It rounds the transported ns upward so ConvertToTick preserves the exact tick duration,
including zero. It does not shift the stored report, DRS pairs, other getters, latency mode,
controller thresholds/hysteresis, or presentation.

Provenance is published under the existing completion mutex before `g_done`, independently
of query/ring-slot reuse. The bounded 1024-entry, four-way sidecar has no per-report allocation
or new per-report lock. A missing/evicted/mismatched record, unknown origin, unavailable hardware
query, or negative producer duration **withholds** the new FPS input: the previous valid measured
GPU duration is held (initially the game's zero), never replaced by a zero clamp or latency
estimate. Both fast and overloaded valid work pass through unchanged. Frequent holds can mask
a workload change; reject a performance comparison with sustained withholding and retain the
per-300 `controller samples/rebased/invalid/withheld` counts rather than claiming a faster GPU.

This is not a forced-FPS patch, altered CPU clock, fake fence completion or replacement GPU-performance
measurement. Raw backend GPU durations, frame timelines, presenter deadlines and sample counter values
stay physical/unchanged. Hardware mode adds point timestamps to other counter types as well so their
timestamp field does not jump to final-chunk delivery; this increases query pressure. Missing/disabled/
exhausted point queries or invalid GPU brackets log `hardware unavailable ... latency reports until
restart` and disable hardware mode for the run. Unknown GPU work is never silently removed from later
backlog. Require zero fallback reports for a valid hardware-mode comparison.

Known consumers and limits:

| Consumer | Meaning / consequence |
| --- | --- |
| FPS work listeners `main+0x27bcdcc` / `+0x2b3cff8`, then controller `+0x2db174` | GPU end report minus CPU origin `sys+0xe0` becomes `sys+0x128`; smaller replay-free work can change the selected interval. CPU work and hysteresis remain intact. |
| Native DRS `main+0xa1b0c/+0xa1b70` getter calls, `+0xa1b8c..+0xa1bb8` arithmetic | `(A_ns-B_ns)*.001*100/16666.666` is stress in percent of a 60-Hz tick. The reports are in different objects and may cross chunks/submits; subtraction is unsigned with **no underflow guard**. **This option also changes DRS input**, potentially raising world resolution, GPU cost and power. |
| Conditional rendering / samples-passed | Reads the counter word, not its timestamp; sample counts and enable decisions are not rewritten. |
| DeviceGetTimestampInNanoseconds | Converts stored report ticks to guest nanoseconds; both known FPS/DRS paths pass here. Other indirect/direct-memory readers are not exhaustively identified. |
| Engine wrapper `main+0x281dd28`, getter call `+0x281dd98` | Returns absolute report ns. Callers `+0x8f3b8` / `+0x27e1c3c` store it at object `+0x1e0` / `+0x1b8`; downstream business meaning and call frequency remain unidentified. It is affected, not assumed to be FPS/DRS. |
| DeviceGetCurrentTimestampInNanoseconds | Still returns the current guest clock, in the same epoch. No calls were observed in archived init/first-three-frame profiler windows; this is not proof of absence in other scenes. |
| Backend/per-pass GPU profiles, framegen/post timing | Separate raw query durations; not virtualized. Compare these, cadence, DRS and power rather than treating a reduced guest stress number as a measured speedup. |

The getter is bound by name at `main+0xbe1f90` into cell `+0x3a25f48` (GOT `+0x396e420`).
A full-text direct ADRP/ADD/LDR/STR and relocation-addend scan finds FPS A/B, DRS and the wrapper
above, with no additional direct users of that binding. This does not enumerate arbitrary copied
function pointers or direct-memory report readers. Archived 2/4/6 getter-call counts are not a
per-PC trace and cannot prove which paths execute every frame.

All current logical guest queues serialize onto one Vulkan queue; hardware mode preserves that order,
including cross-queue dependencies. It does not implement true multi-Vulkan-queue overlap. A GPU pause
inside a valid query bracket remains work; a replay-only gap between chunks of one already-submitted
guest stream does not. Monotonic clamps can compress a timestamp pair if publication/query order is
inverted: retain clamp counts and inspect DRS rather than asserting arbitrary unknown consumers safe.
The earlier KGSL startup-pair drift limitation still applies.
In particular, reject a run with wrapped/huge DRS stress or mismatched report pairs; global write-order
monotonicity alone does not prove the two DRS objects are paired correctly. FPS END getters separately
preserve the report producer's measured duration when the live CPU origin has advanced under lag.

Startup logs `[fps-policy] GPU report time: latency/hardware`. Every 300 presents,
`GPU report time frame ...` reports average actually removed time, guest-submit-to-Vulkan-submit delay,
GPU-end-minus-captured-CPU-origin before/after means, sample counts, fallbacks and monotonic clamps,
plus FPS consumer `samples`, `rebased`, `invalid` and `withheld` counts for that 300-present window.
Rebased samples are valid owner-duration transports, not dropped work; invalid/withheld must stay
zero during steady play for an unqualified hardware-mode comparison. The deterministic host draw
fixture deliberately injects one mismatched timestamp, so its counter is expected to be one.
Correlate `controller pre-acquire` / `controller decision` lines for live policy inputs. Completion
statistics and provenance publish under the existing lock; only the two FPS END reads acquire it.

Host regression: `make -C games/totk/android TARGET=linux CC=clang OUT=/path/to/scratch gpuitimingtest`,
then run `OUT/gpuitimingtest`. It covers delay invariance, idle, accumulated GPU-bound backlog,
overlapping intervals, backend work, logical cross-queue dependencies, chunks, delayed owners/frame lag,
the exact lagged `-309648`-tick subtraction, unchanged light/heavy work, stale/invalid/evicted sample
holds, monotonic floors and latency passthrough. Run the ordinary Vulkan suites in **both** latency and hardware
modes; the real eight-hour-offset guest-clock test now carries 12 delayed chunks across ring-slot reuse.
The standalone helper passed; lavapipe passed 103/103 and 109/109 in both modes, plus 201/201 app
checks in each mode with FPS60 off/lag on. Recorder-enabled asynchronous submission passed 103/103
in each mode; real occlusion counters with hardware timestamps passed 108/108. All synthetic
12-chunk runs had zero fallbacks/clamps; latency preserved the ~13.5-ms injected mean work while
hardware removed replay gaps. These are host fixtures, not device performance results. Android
API-33 syntax checks passed the changed production C files.

Device A/B (operator-run):

1. Odin 3 Gerudo, 60-Hz panel, ~660-MHz GPU cap, same driver/seed/save/camera/weather/thermals and
   recording/lag/pacing settings: measure all four combinations of native UI off/on and GPU time
   latency/hardware, at least 180 seconds after world control, then reverse order. Keep FPS60 on,
   AUTO off, DRS60 and resolution policy fixed. Do not combine a timing-mode comparison with a
   simultaneous DRS_MIN/resolution/governor change.
2. Require startup mode and calibrated guest clock, zero hardware fallbacks, plausible before/after
   work, preserved GPU-bound backlog, matching guest/HLE/presenter interval changes, and no uptime-sized
   or wall-time-growing work. Compare all FPS windows and 1/2/3 requested-interval shares, raw GPU
   busy/per-pass/recording costs, fence cadence/drops and actual DRS/world dimensions/screenshots.
3. Repeat the insa fixture from launch through 180 seconds of actual island control, including camera
   movement, menu recovery and quiet view. Reduced guest work is not acceptance if physical cadence,
   input/animation or load recovery worsens.
4. Thor shipping 30 mode (`TOTK_FPS60=0`, protected lag on), same DRS/resolution/driver/power profile:
   compare latency/hardware with native UI held fixed, then repeat the controlled UI comparison if
   wanted. Track DRS level transitions and image quality, sustainable 30 cadence, GPU backlog and
   thermals/average power/energy over the same route. Higher DRS resolution or power can outweigh
   unchanged 30 fps. Check cross-scene/menu transitions and preserve raw logs for both FPS modes.

**Automatic60/30 (opt-in):** on a60-Hz panel, `TOTK_FPS60=1` + `TOTK_FPS_AUTO=1` starts at60.
It requests30 only after **two independent, nonoverlapping5-second eligible raw game-present windows**,
each with at least4.75 seconds coverage,120 presents and actual rate **strictly below55fps**.
The rate is `(presents−1)/(last−first)` from every HLE game present, independent of READY/deadline
miss share. Slow raw intervals must span≥5 seconds across the pair: one2-second spike cannot trigger
a drop even across a window boundary. A sustainable57–60fps run with slightly late deadlines no longer
qualifies. At30 it returns to60 after an8-second readiness window (at least7.5 seconds and180 samples)
has at least**95% fitting refresh+2ms**. Isolated READY gaps over3 refreshes no longer erase recovery;
invalid evidence or a>1-second stall still clears it. The counterfactual readiness calculation takes
the maximum of `READY − gate release − post-gate kernel blocked time` and
`CPU-production busy time + GPU-ready tail after present`. CPU-production busy time is
`game-present interval − measured intentional acquisition pacing − guest-kernel blocked time`.
Blocked counters are per presenting guest thread and nested-wait-safe: SVC event/address/mutex/CV
waits, sleeps/yields and virtual-core waits count once; owner changes, counter reversals or impossible
overlaps reject the sample instead of fabricating headroom. Measured copy preparation and steady
presentation-call allowance are then added once. Native GPU/texture/resource waits and the independent
GPU tail remain; intentional presenter FIFO/slot residence does not. Readiness is a CPU publication
observation, not a hardware GPU timestamp.
The feature collects these anchors independently of `TOTK_FRAME_TIMELINE`; diagnostic logging is not a prerequisite.

**Loading/transition exclusion:** decision evidence is disabled before the existing first-world
heuristic (>2000 draws), during a game loading-screen callback when one exists, when draws fall below
`max(512, established-scene draws/3)`, and for10 seconds after world entry, low-draw/loading observations
or a burst of≥8 successful graphics/compute pipeline creations within1 second. Inhibition preserves
the current target but clears pending decisions/streaks and invalidates queued prior-epoch feedback.
Only eligible frames train fresh windows after the settle period. The Android port currently lacks
an authoritative game loading-screen callback; draw/pipeline gates are conservative observations,
not a claimed complete decoder of every loading state. Very low-draw scenes can delay decisions.
Additional **1-second grace** covers >100-ms game-present hitches, changed crop/present-texture
dimensions, active host VP9 playback and its stop transition. It also covers actual host save-data
write activity (`runtime_fs_save_busy`) and save-mount Commit entry/completion, outside filesystem
locks; this is host I/O evidence, not a guessed game/UI save-state bit. Existing decoder lifecycle
supplies movie activity; guest-library decoders are not claimed observable. Precise pause/menu/door
states are not exported here: low-draw/loading observations cover only observable cases, not every state.

**Failed-return backoff:** an actually applied60→30 fallback within20 seconds of a30→60 return
doubles the next minimum30 hold, capped8× (**8/16/32/64 seconds**), in addition to the healthy8-second
window. Grace/inhibition cannot fabricate a failed return or pay off its debt. One minute of valid,
uninterrupted60-fps evidence restores1×; mode/refresh reset starts a new policy. Transition/readiness
logs expose `recovery-backoff` and `remaining-min-hold`.

Both directions have a minimum5-second dwell. Invalid, missing or pause-sized evidence cannot prove
recovery; clock/refresh changes and guest modes slower than30 discard stale observations. Decisions are applied
only at the next NVN acquire, before reserving its fixed display slot; queued frames keep immutable
slots and their resource fences. The next reservation advances by the **new** interval on the same
lattice, with no queue drain, guest-code rewrite or extra transition sleep. Bounded slack1 and opt-in
predict continue to operate at both intervals. The guest's requested interval is never shortened:
guest requests2 remain authoritative while auto retains its hysteresis; request3 (20fps) suspends auto.
The existing1.4.3 FPS60 patch permits controller intervals1..3, not a forced interval1
(`scripts/verify_v143_fps60.py` documents its light/medium/heavy cases). Auto is an HLE **minimum interval**:
`effective = max(guest request, auto target)`. A guest1↔2 change therefore does not erase the new
controller's dwell/recovery evidence or bypass a selected30-fps floor.

Requires `TOTK_PRESENT_SLOTS=1`, capped60-Hz pacing and measured `TOTK_REALTIME` timing (default on).
Unsupported combinations (uncapped, deterministic/interpolation timing, `DM_PRESENT_INTERVAL=display`,
non60-Hz refresh, or `TOTK_REALTIME=0`) retain the prior policy and log why. The existing
main+0x3e5658 measured-time producer and five-frame time-step history are unchanged, including its
half-step selector and clamps; changing effective presentation interval must not make the world
clock run at half speed. The existing smoothing produces a brief transition in the reported step,
not a newly pinned nominal timestep.

**Design attribution:** busy-vs-blocked accounting, scene grace and failed-return backoff are original
TOTK implementations inspired by
[LASRecomp-Android-Plus `runtime/nvn/pace.c`](https://github.com/Nycolazs/LASRecomp-Android-Plus/blob/cca791a3e7218e1dac95e7d4a11b8f97dba3ea51/runtime/nvn/pace.c)
and its frame/interval hooks (MIT, Copyright(c)2026 Peiikai; see that project's LICENSE).
No LAS game offsets, mode hooks, world-step assumptions, second-step subtraction or GPU-specific
thresholds were transplanted. TOTK retains its own measured-time producer and existing resource fences.

**Why the policy changed:** the operator's Odin≈660-MHz Gerudo runs with6b50135 sustained mostly57–60
with auto off, but slightly-late readiness shares26.8% and18.1% falsely qualified auto downshifts.
Only the actual raw game-present rate now authorizes a downshift; readiness remains recovery evidence.
The supplied `o-auto3-on` recording also showed intermediate/long intervals during30. Native fence
evidence narrows, but does not fully identify, that observation:18 wholly interval2 windows contain
**5382 normal two-refresh intervals and18 one-refresh intervals out of5400**, with no native25/50-ms
bins, missing endpoints, skipped slots or irregular desired deltas. Each window has299 normal and1 short.
Native fence intervals indicate≈16.693ms physical refresh versus the API's nominal16.666666ms.
Advancing nominal interval2 targets loses≈15.8ms every300 frames, explaining a periodic one-refresh
catch-up candidate. This is not evidence that distinct recorded pictures or hardware scanout lack
the operator's reported25/50-ms holds; recording PTS, image identity and framework fences differ.

**Measured clock, all modes:** passive Choreographer timestamps are observed even without AUTO or
timeline logging. Separate callback and actual current-AHB-fence observers fold integer1..8-refresh
gaps, require256 ticks across eight blocks with robust consensus, and initially preserve nanosecond
precision. Calibrated present-fence period takes precedence over callback period; callback timestamps
still supply the physical phase. The period freezes after calibration, revalidates once per minute,
and changes only after two agreeing significant-change windows; a declared display mode change resets
calibration immediately. Fixed slots, display pacing, fixed acquire/slack, predict and the HLE capped
duration all consume this same measured period, not rounded60Hz. Future reservations reanchor on a fresh
callback after a promotion; already queued targets never change. `[totk-pace] measured display clock`
reports source, nominal/measured periods and phase once per minute and at source/period changes.
Explicitly disabling all physical clock gates retains legacy nominal pacing; observation alone does
not enable a wait or change guest VI signaling.

AUTO reports `window AUTO queued cadence` every300 queued reservations: exact interval1/2 counts,
below-acquired-floor, irregular desired deltas, skipped slots and enqueue lateness. **Every AHB run**,
without AUTO/timeline/predict, collects current-transaction present fences and reports
`window present-fence cadence, last 300 results` plus bins
`[<12.5,12.5–20.9,20.9–29,29–37.5,37.5–45.8,>=45.8]` ms, nearest-rank p50/p95/p99 and sample/pair
coverage, including adjacent immutable interval2 pairs. A bounded300-sample report-only sort has no
per-frame allocation. Missing endpoints, generation/refresh boundaries, overflow/history loss and
callback reordering are explicit; intervals never bridge absent transaction IDs. The first four
non-two-refresh tails in each interval2 window include IDs and exact desired/native deltas.
Framework present-fence signals are not guaranteed hardware scanout. `[totk-fps-auto] readiness`
every300 eligible observations separates joined readiness, CPU-production busy, kernel blocked time,
GPU tail, readiness misses, latest raw5s game rate and failed-return backoff.

**Device A/B, not run here:** Odin3 Gerudo at the user's≈660-MHz GPU profile,60-Hz panel,
same save/camera/weather/cache/thermal state, `TOTK_FPS60=1`, protected lag and recorder settings fixed.
Compare `TOTK_FPS_AUTO=0` versus1, `TOTK_VK_PRESENT=ahb`, `TOTK_UNCAPPED=0`,
`TOTK_PRESENT_SLOTS=1`, `TOTK_ACQUIRE_SLACK=1`; predict is a separate controlled repeat with
`TOTK_ACQUIRE_SLACK=predict`. Warm≥180s and reverse run order. Require measured-clock calibration
to≈16.693ms if the same physical mode persists, and compare multiple300-result native-fence windows:
the periodic one-refresh interval2 tail should disappear without new long tails or missing coverage.
With auto on,57–60fps and isolated spikes must stay60; only sustained raw<55fps should downshift.
Require transition raw rates, inhibited/resumed reasons, healthy return and no flapping.
Repeat **Thor default30** (`TOTK_FPS60=0`, `TOTK_FPS_AUTO=0`) with AHB and the same fixed slots/slack:
expect two actual refreshes per present,≈29.95fps at59.905Hz, not a forced nominal30.000. Require
six-bin/p50/p95/p99/coverage lines with `TOTK_FRAME_TIMELINE=0` and AUTO off.
Record distinct-picture/screenrecord distributions separately from fence bins, input latency, and wall-time
versus in-game clock/traversal/cloth/audio. Check pause/resume, surface recreation and guest-requested
slower intervals. No device smoothness, thermal saving or30/60 stability is claimed by host tests.

Host verification for this revision: `fpsautotest` passed independent raw5s×2 windows, all-late57/59.94/60fps,
strict55fps boundary versus54/45fps drops, isolated/boundary spikes, missing/raw/READY clocks,
recovery5% boundaries, failed-return1/2/4/8×/minute-reset rules, grace/inhibit and1→2→1 slot continuity.
`displayclocktest` passed robust initial calibration,1ns precision, skipped callback/30fps fence folding,
pause/mode resets, frozen/minute validation and immutable queued targets. Its600-frame physical-panel
fixture reproduces a nominal-period short tick and eliminates it with measuredR across slots/acquire/slack/predict.
`fpsautoscenetest`, `fpsautoblocktest` and extended `presentfencetest` passed scene/blocking and
always-on six-bin/percentile/coverage/missing/reordered-endpoint fixtures. These five passed ASan+UBSan;
all12 host pacing targets passed (timeline/fence also disabled). Android `cc1.sh` passed11 runtime/HLE/backend/probe
units, including frontend, threads, fs_host, movie_host, movie_probe, HLE, nvn_vk, vk_draw, vk_shader, vk_post and vk_overlay.
The movie probe's real VP9 playback run was **not** executed here (host libvpx development files and
an AArch64 runner are unavailable); syntax checking is not a playback claim. The20-case lavapipe
post/overlay/presenter matrix passed with auto off, not Android auto/scanout acceptance.
Logs: `.scratch/FramePacing/fps-auto-v4-tests/`; native/recording evidence: `.scratch/FramePacing/cadence-evidence.json`.

```
make -C wt-fps-auto/games/totk/android TARGET=linux CC=cc \
  OUT="$PWD/.scratch/FramePacing/fps-auto-v4-tests" fpsautotest fpsautoscenetest fpsautoblocktest presentfencetest displayclocktest
.scratch/FramePacing/fps-auto-v4-tests/fpsautotest
.scratch/FramePacing/fps-auto-v4-tests/fpsautoscenetest
.scratch/FramePacing/fps-auto-v4-tests/fpsautoblocktest
.scratch/FramePacing/fps-auto-v4-tests/presentfencetest
.scratch/FramePacing/fps-auto-v4-tests/displayclocktest
```

Historical latency-mode runs dropped Lookout Landing in the rain to the controller's 20 fps mode;
one-frame-late waits (`TOTK_FRAME_LAG=1`) then restored 30 fps at about 1.1 W more. Thor's newer
comparison identifies overstated latency-mode GPU reports (about 9–12 ms), not missing frame lag,
as the cause of those controller drops: lag off plus hardware reports reaches 29.93 fps, minimum
29.6, versus 27.71 fps, minimum22.6, with lag on plus latency. Protected lag remains runtime-unset **off
on every platform**; the Android menu now generates `TOTK_FRAME_LAG=1` only for Snapdragon 8 Elite-class
native60 without frame generation, using the capped Odin evidence above. Explicit developer `=0` wins;
every other value besides exact `1` leaves lag off. Android shipping-30 defaults to hardware reports;
FPS60=1 and other platforms/BOTW retain latency. Explicit GPU-time overrides and the hybrid opt-in are unchanged.
The open shadow report occurred in a round-nine lag-plus-slab-driver run; lag causation remains unestablished.
`TOTK_VK_CHUNK_DRAWS` defaults to **75** with default-off lag, or **600** when resolved lag is on
and native VMA pressure is absent, otherwise **75**; an explicit value wins, including 0 to disable chunking.
Startup logs the resolved `FPS60`, `FRAME_LAG`
and chunk threshold once. With lag enabled the Vulkan backend protects guest buffer ranges before returning from
submit. `TOTK_VK_LAG_MODE=wait` (default) keeps cold buffers live with sticky per-unit protection; only actual CPU
reuse waits for its newest fence.
Adaptive hot-ring copies are on (`TOTK_VK_LAG_HYBRID=0` gives a sticky-only A/B): two writing frames in eight promote
a bounded readonly range, eight quiet frames demote it. `TOTK_VK_LAG_HOT_MAX_KB` defaults to 256; oversized or writable
ranges stay guarded. Armed units update only increasing tickets; repeated registrations for the same chunk only load
the existing ticket, without a CAS, global counter update, page scan or `mprotect`. Native registration no longer sorts/merges.
`TOTK_VK_LAG_MODE=copy` retains eager read-only snapshots as an A/B fallback; `TOTK_VK_LAG_CAPTURE=0` disables
both. Both modes force synchronous CPU replay if `TOTK_ASYNC_REPLAY=1` was requested, but preserve the lagged GPU
wait. Eager capture measured 28.5 vs 29.9 unprotected fps; the first wait version measured 28.4 fps with about 4 ms
guard setup and 2.5–3 ms actual reuse waits/frame. The prior sticky/hybrid Thor sweep reported 28.4 fps, zero flashes/crashes,
3.173 ms setup and 1.017 ms reuse waits/frame; the cheaper registration follow-up still needs a matching sweep.
`TOTK_VK_LAG_TIMING=1` (default 0) adds per-binding selection/list timing; chunk registration/copy timing remains on.
See [VULKAN.md](VULKAN.md), "Lagged guest-read lifetime", for copy/guard/sticky counters, worker registration requirements
and top faulting-unit addresses/reasons/maximum descriptor sizes logged every 300 frames. Reasons distinguish writable
or GPU-owned shared units from readonly ranges exceeding the copy cap; do not infer safety from the address alone.

### Display-paced frame acquisition (opt-in)

`TOTK_PACE=display` is the optional policy that uses Android's real display callbacks for both emulated VI
events and frame acquisition. The Android app instead defaults to `TOTK_PACE=0`, `TOTK_UNCAPPED=0`,
`TOTK_ACQUIRE_CLOCK=0`, **`TOTK_ACQUIRE_SLACK=1` and `TOTK_PRESENT_SLOTS=1`**, for both controllers.
This enables bounded one-frame producer lead plus fixed real-vsync presentation slots, keeping legacy VI.
Explicit `TOTK_ACQUIRE_SLACK=0` / `TOTK_PRESENT_SLOTS=0` opt out independently; both 0 restore the capped legacy
acquire/presenter A/B. FPS60 defaults off; enabling it on a fresh install no longer inherits uncapped 120-Hz pacing.
App defaults are applied after `totk.env` without overwriting explicit values; the resolved
`[totk-pace] Android policy: ...` startup line records them. These defaults do not change command-line/headless
timing. Keep the panel at **60 Hz** for the comparisons below.

**Opt-in two-frame reserve:** `TOTK_ACQUIRE_SLACK=2` allows two periods of producer lead and at most
three pending real pictures on the same fixed slots. Default stays **1**. It requires `TOTK_PRESENT_SLOTS=1`;
unset `TOTK_PRESENT_BUFFERS` auto-raises to **4**, while an explicit lower override refuses slack 2 with a
reason in `[nvn] acquire slack 2: refused: ...`. `TOTK_FRAMEGEN=interp` / generated-frame mode `1` are
incompatible for now. Existing fixed-clock/display/uncapped/deterministic exclusions remain.
The FIFO presenter, virtual clock, retire-at-slot credits and all GPU/compositor fences are unchanged.
`TOTK_FRAME_LAG=1` and controller CPU/GPU timing remain independent. Early work can refill the deeper reserve,
but sustained overload still hitches. Expect up to **another ~33 ms of input latency versus slack 1 at
30 fps** (another tick, not free throughput). No third guest texture is added.

For the exact Thor A/B environment and log comparison, see
[two-frame reserve Thor A/B](DEBUGGING.md#two-frame-reserve-thor-ab-totk_acquire_slack2-opt-in):
compare 1 versus 2 with `TOTK_FPS60=0`, slots 1, lag 1 and **four buffers in both**, same warmed route, then
reverse run order. `[frame-tl reserve]` reports per-300-slot banked-depth 0/1/2 and absorbed/shown readiness
proxies; compare actual AHB fence cadence/tails and `[frame-tl input-dist]` last-read-to-AHB-present-fence
p50/p95 / coverage to quantify the added latency. Set `TOTK_FRAME_TIMELINE=1` for those reports: slack 2 no longer
turns the timeline on (its admission never reads it; the menu offers slack 2 as a player option). Missing timestamps
are not zero latency; copy-post latency is not scanout.

Diagnosis of the reported Odin 3 Gerudo result (77.4–77.9 fps with FPS60 + lag + recorder threads, versus 56.5
without lag):

The explicit-cap control confirmed this diagnosis on Odin 3: FPS60 + lag measured **77.4 fps**
with the inherited uncapped policy, versus **60.0 fps** with `TOTK_UNCAPPED=0`.
This confirms the old policy diagnosis, not device acceptance of the new display-callback mode.

- `runtime/frontend_android.c` defaults `TOTK_FPS60=0`. `runtime/game_patches_v143.c:fps60` changes seven checked
  controller instructions to permit interval 1; the shipping controller requests at least 2 (and can select 3).
  `nvnWindowBuilderSetPresentInterval` / `nvnWindowSetPresentInterval` in
  `third_party/d2r_gfx/gfx/nvn/hle_nvn.c` retain that request in `win_t.interval`.
- **The old 60-Hz VI event was not a global frame limiter.** `frontend_headless.c:vsync_thread`, also used by the
  Android app, signals an auto-clear event every 16,666,667 ns from an absolute-sleep thread. Signalling an event
  does not make every frame wait on it. Separately, `hle_nvn.c:present_cfg` retains the Mac uncapped policy:
  realtime + FPS60 + no explicit `DM_PRESENT_HZ` or `TOTK_UNCAPPED=0` selects a **120-Hz** clock.
  Before the Android default `TOTK_UNCAPPED=0`, interval 1 therefore permitted up to 120 game frames/s;
  77.9 was not an accidental 60-Hz FIFO limit violation. Explicit uncapped controls remain available for profiling.
- **Why the Thor still held 30:** `present_cfg` explicitly disables uncapped mode when `TOTK_FPS60=0`.
  `present_period` then maps interval 2 onto 2/60 seconds. `nvnQueuePresentTexture` assigns synthetic availability
  times using `nvn_pace`; `nvnWindowAcquireTexture` sleeps until a reused texture's availability time. That is the
  existing 30-fps cap, distinct from the frame-end GPU fence waits. Lag removes GPU serialization, not this cap.
- **There is no Vulkan WSI swapchain in either Android presenter.** `android/gfx/vk_draw.c:present_window` enqueues
  into a default-three-buffer ring (four is optional; interpolation alone needs six), dropping copies if all buffers are busy.
  `presenter` waits for GPU completion, then either CPU-copies through `ANativeWindow_lock` / `unlockAndPost`, or
  submits an AHB using `ASurfaceTransaction_setBuffer` / `apply`. Legacy AHB desired-present time was enqueue time.
  Neither path selects FIFO, MAILBOX or IMMEDIATE or calls `vkQueuePresentKHR`; the new default fixed-slot policy
  supplies Choreographer timing independently. BufferQueue/SurfaceFlinger still owns physical scanout.
  Present tickets (`mtl_present_wait`) protect GPU texture reuse, not physical scanout cadence. Consequently
  `VK_KHR_present_wait` / display-timing swapchain extensions cannot fix this particular presentation path.

With `TOTK_PACE=display`, `frontend_android.c:display_thread` owns an `ALooper` and `AChoreographer`:
`postFrameCallback64` supplies vsync timestamps, and `registerRefreshRateCallback` supplies the actual period
(public APIs available at this app's API 33 minimum, independent of Turnip's Vulkan extension support).
The old sleep thread is not started. Each fresh callback signals the VI events and wakes the acquire pacer;
duplicate/stale timestamps grant no credit. The existing `mtl_link_*` acquire hook waits for the requested number
of callbacks: interval 1 is at most one game frame per refresh, interval 2 one per two refreshes, interval 3 one per
three. A late game consumes only the latest callback, never banks catch-up frames; missed callbacks slow down
rather than manufacture elapsed ticks. Frame acquisition is the blocking point, not present, because the game
can hold locks while presenting. GPU fences and texture-reuse waits remain unchanged.

Display mode overrides `TOTK_UNCAPPED`, `TOTK_UNCAPPED_HZ`, `TOTK_UNCAPPED_INTERVAL`, `DM_PRESENT_HZ` and
`DM_PRESENT_INTERVAL=display`: the guest's requested interval wins. `DM_DETCLOCK` keeps its existing deterministic
clock path and is not part of this device mode. No sleep fallback silently uncaps the game if callbacks stop;
rendering waits until callbacks resume. A refresh change automatically changes callback cadence; 120 Hz with
interval 2 means 60 fps, not 30. This switch does not force the panel into a mode.

**Game speed:** do not equate 77.9 rendered fps with a proven 1.30x world clock. The stock frame-step producer is
frame dependent, but this port defaults `TOTK_REALTIME` on. `game_patches_v143.c:runtime_frame_step_update`, called
by `nvnQueuePresentTexture` before each present, averages five measured frame intervals with a 1/15 s hitch cap
(no 1/120 s minimum, so short/jittery frames do not accelerate game time), and stores ratio = seconds × 30,
seconds, and interval = seconds × 60. `patch_realtime` replaces the nominal
producer at main+0x3e5658, retaining its half-step selector and FrameRateMgr arithmetic; it also patches the global
ratio producer at +0x2daa54..+0x2daa78. Thus 77.9 fps is inside the real-time patch's range: [INFERENCE] consumers of
these steps should advance at wall-clock speed, not 77.9/60 speed. `patch_secondary_animation` separately corrects
four cloth/hair callers that originally constructed 1/30 seconds per rendered frame. The existing
`scripts/verify_v143_timing.py` tests those actual emitted veneers and includes unpatched negative controls; that
artifact-dependent script was not rerun for this change. Frame-counted UI/other undiscovered consumers can still
depend on fps. No live time-of-day-clock measurement is claimed; the device check below is needed for that.

Host checks for this change: the Android frontend, NVN adapters and `hle_nvn.c` compiled with `.scratch/cc1.sh`;
the standalone `android/gfx/test_display_pace.c` passed with `cc -std=c11 -O2 -Wall -Wextra -Werror`. It feeds
timestamps into the production policy in `display_pace.h`, checking 60/90/120-Hz sequences at intervals 1/2/3,
duplicate/stale callbacks, zero interval, stalls without catch-up, and interval/refresh changes. It has no Android,
GPU or sleeping dependency. From the project root, a reproducible host command (outputs only under scratch) is:

```
mkdir -p .scratch/FramePacing
cc -std=c11 -O2 -Wall -Wextra -Werror \
  wt-framepacing/games/totk/android/gfx/test_display_pace.c \
  -o .scratch/FramePacing/test_display_pace
.scratch/FramePacing/test_display_pace
```

Device acceptance (not yet exercised):

1. Odin 3, warmed Gerudo, 60-Hz panel: compare unset `TOTK_PACE` with `TOTK_PACE=display`, keeping
   `TOTK_FPS60=1`, `TOTK_FRAME_LAG=1`, recorder-thread count, Turnip, cache, camera/weather and thermal state fixed.
   Run at least 180 s each, then repeat in reverse order. Expect `[totk-pace] display pacing enabled`,
   `display refresh 60… Hz` and `display frame … interval 1`; warmed 300-frame FPS windows must be **<=60.0**,
   not 77–78. Retain callback/wait logs and all windows, not only the best one.
2. Thor, same controlled scene: `TOTK_FPS60=0`, lag unchanged, `TOTK_PACE=display`; expect interval 2 and **30.0 fps**.
   Repeat with lag off, and test a scene that requests interval 3 (20 fps). Repeat both devices with `copy` and
   `TOTK_VK_PRESENT=ahb`; require the AHB log with no copy fallback for the latter.
3. For frame-time distributions set `DM_NVN_TSV=<writable absolute path>` and `DM_NVN_TSV_CALLS=0`; inspect
   `@frame_ms` and `@present_guest_ticks` / `@present_effective_ticks`. Expect a narrow ~16.67-ms distribution for
   Odin interval 1 and ~33.33 ms for Thor interval 2, apart from genuine GPU/CPU misses; compare p50/p95/p99,
   longest frames, window shown/dropped/discarded deltas and visible motion. `@display_ms` / `@wait_ms` are legacy
   synthetic-schedule metrics, not proof of actual scanout or Choreographer wait duration; use `[totk-pace]` for
   acquire wait. SurfaceFlinger timestamps/trace, if collected by the integrator, distinguish physical display
   cadence from these game-present timestamps.
4. Suspend/resume, destroy/recreate the window, and switch refresh mode: no burst of accumulated frames, no
   deadlock, correct new refresh log. Keep the main acceptance runs at 60 Hz. Capture a fixed 120-s gameplay
   video with the HUD clock visible, in the same save away from menus/sleep/time-changing scripts, and compare
   clock progression against wall time with pacing on/off; also compare animation, traversal and UI timing.
   This tests remaining frame-dependent logic rather than assuming the shared real-time step covers everything.

Risks: callback/Looper scheduling and compositor latency still require device validation; missed callbacks
conservatively reduce throughput, and asynchronous GPU completion can still miss a display slot. Acquire pacing
does not claim per-frame SurfaceFlinger presentation confirmation, nor fix the separate frame-lag lighting hazard.
`TOTK_PACE=display` remains opt-in; the Android default is now bounded acquisition slack + fixed presentation
slots with legacy VI. See [DEBUGGING.md](DEBUGGING.md#thor-judder-recorded-chain-and-display-slot-pacing) for
the Thor device regularity result, independent opt-outs, `TOTK_ACQUIRE_SLACK=auto` and opt-in **`predict`**
deadline-based latency controls and input-read-to-presentation measurements. The default remains slack **1**.
Prediction shares one immutable slot from acquire to presenter, measures separate copy/AHB readiness, and
retains all GPU/texture/compositor lifetime waits. It defaults timeline diagnostics on only when unset;
use the global-window association and compare equivalent presentation endpoints. Deferred AHB fence sampling
retains pending current-frame fds; unavailable hardware timestamps remain unknown, never substituted with latch time.
For predict only, `TOTK_PREDICT_MISS_PCT=2` selects the tolerated readiness/phase-relative display-tail
percentage (default 2%, exact rolling 300-frame quantile; valid 0..100). It is a tail budget, not an FPS
or game-time change. Per-300 diagnostics expose realized rates, calibrated display phase and forecast
cost/allowance/margin. Constant app-callback-to-compositor phase is not itself a missed slot. See the matched
AHB device plan before promoting prediction over default slack 1; lower lead is not worth new cadence tails.


The upstream merge also enables Android virtual-core asynchronous preemption / priority inheritance
(`TOTK_ASYNC_PREEMPT=0` off) and starvation escape (`TOTK_STARVE=0` off), the complete DEPTH16 dark-water fix
(`TOTK_VK_D16TIE=0` off), CPU-pool rendering write-back and descriptor-pool reuse. These were separate upstream
fixes, not a new measurement of the frame-lag experiment. Compare the combined build in Lookout Landing, Gerudo
and the Depths before changing defaults. `DEBUGGING.md` keeps upstream's device observations.

Diagnostic commands from `TOTK_INPUT_FILE` and `TOTK_INPUT_SCRIPT` start after `totk.env` is read. On the app,
the app sets `TOTK_INPUT_FILE_NEW=1`: only newly appended commands are read (this is a presence flag, so even `0`
means append-only). `passdump <n>` now writes sampled PNG attachments and an index into `passdump/f<frame>/`;
the env-driven binary range dump is unchanged.

Attachment-work experiments are also off by default: `TOTK_VK_CLEAR_LOAD=1` turns eligible full mid-pass clears into
pass-start clears, and `TOTK_VK_STORE_CLEAR=1` also skips stores proven dead before a later full clear (and enables the
first option). Neither has device measurements yet; extra pass boundaries can cost more than they save. See
[VULKAN.md](VULKAN.md#opt-in-attachment-work-reductions-device-measurement-pending) for the counters and A/B procedure.

### Packed shader IO and learned demand warming

Android now defaults to indexed seed IO (`TOTK_VK_SEED_IO=0` retains legacy loose IO for diagnosis).
The build machine, not the APK or device, translates the user's own verified RomFS with
`scripts/prepare_shader_seed.py --keep-spirv`; `build_android.py --shaders` then stages the `.spv`,
Vulkan reflection `.vk`, and variant-list `.vv` files. `optimize_shader_seed.py` is an optional,
separate validated SPIR-V optimization step; packing does not optimize or change shader bytes.
`build_android.py` now packs the staged selected-version seed by default, including `--runtime-only`.
The existing `--packed-shader-seed` spelling is accepted; `--no-packed-shader-seed` skips packing.
To pack an already staged or optimized seed without translating again:

```
python3 games/totk/scripts/pack_shader_seed.py /path/to/your/seed
```

Only `seed.vkpack` plus the seed's **unchanged existing** `seed-id.txt` need deployment. These are
game-derived local build outputs: never commit or distribute them in the repository/APK.
The APK alone updates code, **not seeds**. Generated `install_app.sh data` (`data-packed` explicitly)
and `push.sh [--packed] [shaders|DEVICE_FOLDER]` now transfer only the pack/marker for each seed,
using a temporary device file and atomic rename. `data-loose` / `push.sh --loose` are recovery options.
Keep the app stopped during deployment; restart selects the new pack inode. The open descriptor pins
the previous inode safely if a pack is atomically replaced while running.

For Thor's existing installation, use a fresh external directory rather than scanning/overriding
the old 72k-file loose seed. Substitute the actual package/folder if different:

```
adb shell am force-stop com.aure.totk
adb shell mkdir -p /sdcard/Android/data/com.aure.totk/files/shaders/v143-packed
adb push /path/to/your/seed/seed.vkpack /sdcard/Android/data/com.aure.totk/files/shaders/v143-packed/seed.vkpack.tmp
adb shell mv /sdcard/Android/data/com.aure.totk/files/shaders/v143-packed/seed.vkpack.tmp /sdcard/Android/data/com.aure.totk/files/shaders/v143-packed/seed.vkpack
# If your seed has seed-id.txt, copy it unchanged into the same directory.
```

In the existing external `totk.env`, preserve every other setting and select
`TOTK_SHADER_SEED_DIR=shaders/v143-packed`, remove old `TOTK_VK_SEED_IO=0` / `TOTK_VK_SEED_PACK=0`, and add
`TOTK_SEED_STATS=1` for diagnosis. Alternate-directory cache namespacing remains unchanged:
this first switch also selects a separate pipeline cache and learned index. Do not confuse a cold
driver cache's pipeline compiles with seed IO; compare repeat runs after that cache has learned.

`totk-seed-io` attempts nice +10 and prefers a detected clearly smaller CPU tier with at least two CPUs,
otherwise retaining its inherited allowed mask, with actual mask/error readback. ClusterTune/frequencies never change.
It opens/validates one pack and builds the in-memory name index before readiness. Header/version,
declared file size, sorted unique names, index bounds, alignment, entry bounds and overlap are checked.
A missing/unreadable or invalid/truncated/incompatible pack emits **one** clear fallback line and
indexes loose files. Loose files remain necessary for existing installations and live-translator
overlays, especially expanded `.vv` lists; they override packed entries. A pack-only folder with
an unusable pack and no valid loose modules reports the existing missing-seed error instead of drawing
invalid data. Size checks are structural integrity checks, not a cryptographic payload checksum.

Successful lookups learn exact `.vv/.spv/.vk` demand keys, including compute, independently of
opt-in pipeline compilation prewarm (the opaque Vulkan cache cannot enumerate shader names).
`state/vk-seed-used.txt` uses the existing seed namespace and holds at most **16,384 keys**, newest
use first. The loader coalesces writes for five seconds, snapshots at shutdown, and publishes via
temporary file + fsync + atomic rename **on the worker**, never per-seed fsync on the walker.
A force-kill can lose the last five seconds of new history; existing history remains complete.

On the first run no history exists: play the target route, allow at least five seconds for learning,
then restart in the same seed namespace. Later starts asynchronously pre-read the MRU demand set.
`vk_shaders_ready` waits for the metadata index **only**, not payload warming; boot/load frames can
continue. A queued learned key requested by replay is **removed from the warm queue and read
immediately on the caller**. An already-running learned read is joined, never duplicated.
Cold, skipped and already-consumed packed entries also use immediate indexed `pread` on the
pinned descriptor; they never wait for unrelated worker reads or history/request fsync.
Only the short metadata publication lock is shared. Mutable warmed ownership transfers directly
to parsers; simultaneous consumers copy only when separate mutable ownership is necessary.

Without an override, the warm budget is **the indexed learned-set bytes plus 25% headroom**,
rounded up to MiB, with a **64-MiB floor and 256-MiB ceiling**. This counts terminators and is
calculated from metadata, not a payload scan. `TOTK_SEED_WARM_MB` remains an explicit fixed cap
(range 1–512 MiB). Reservations release on consumption/failure/publication. Over-cap keys stay
demand-only. The current measured Thor history has **5,939 keys / 88,274,661 bytes (84.185 MiB)**,
including terminators, using the selected pack index plus device overlay sizes. Its default
cap is **106 MiB**. Fully warming that set can retain about **20.185 MiB more payload RAM**
than the old 64-MiB budget, plus allocator overhead; the unused 21.815-MiB margin is **not**
preallocated. Actual process RSS also includes modules, driver objects and allocator retention.

All-variant sibling and graphics bind-time `.vv` speculation have been **removed**. Thor's
matched firstuse1 village runs regressed seed blocking to 129.5/216.9 ms versus cand4's
66.3/39.5 ms; run 2 produced only 29 ready sibling hits against 107 joins and 9,246 queued.
The full 64-MiB learned budget was already skipping known keys. Keep exact learned warming
with sufficient capacity and the independent compute worker, not low-value speculative IO.

Expected log lines:

```
[seed-io] indexed N files (P packed), learned K keys learned_bytes=B cap=C mode=adaptive, index=Xms directory ...
[seed-io] warm complete keys=K ready=R bytes=B cap=C skipped=S bypassed=D failed=F async=Yms
[seed-stats] 300-present lookups=L pack_hits=P warmed_hits=H demand_storage_reads=D demand_bytes=B warmed_storage_reads=0 block_total=Xms block_max=Yms worker_reads=W worker_bytes=B cached=C warm_bytes=B cap=C skipped=S warm_joins=J learned_bytes=B warm_bypassed=D
```

`TOTK_SEED_STATS` defaults **off**. Lookup/hit/read/byte/blocking fields are deltas per the existing
300-present stutter window; worker reads/bytes, warm bytes, skipped and cached payload bytes are
cumulative/current gauges. `demand_storage_reads` counts payload read attempts (individual `pread`
calls for packs, a loose-file read attempt for fallback) initiated for demand. Learned warm reads
are in `worker_reads`, even if subsequently joined. `warmed_hits` means already-ready RAM;
`warm_joins` only counts an already-running learned read, not a queued key. `warm_bypassed` is
cumulative queued learned work canceled and serviced immediately by demand, distinct from
cap `skipped`. After startup, expect `skipped=0` when the learned set fits the adaptive ceiling.
`warmed_storage_reads` must be **0**, demonstrated by synthetic tests that truncate the source after
warming. For a repeat route fully covered before demand: expect `H>0`, `J=0`, `F=0`, `S=0` and no
on-demand reads for those warmed first uses. Never-seen keys, cap skips, live publication and repeated
parses can legitimately make `D>0`. Compare `index` startup latency separately from asynchronous
warm duration and per-window recording blocking; host correctness is not a measured device speedup.

Anonymous recorder slab naming now defaults off to avoid `PR_SET_VMA_ANON_NAME` mmap-lock contention while the
walker grows slabs. `TOTK_VK_REC_VMA_NAMES=1` restores names for `/proc/maps` diagnostics; this is separate from
ordinary thread names and does not change memory protection/accounting. Compare IO and naming independently with
the same route, caps, driver/cache and thermal state; retain pixel/translation checks and measure startup/RSS/power
as well as walking tails. See [VULKAN.md](VULKAN.md#indexed-background-seed-io-and-packed-deployment).

### Host thread names and optional placement

Linux/Android host threads now identify themselves with kernel names (at most 15 bytes): `totk-walk` for any
thread that executes Vulkan submissions, `totk-recw0`… for recorder workers, `totk-pipe0`… for pipeline workers,
`g-main` / `g-<guest id>` for guest threads, `totk-audio`, `totk-auddev`, `totk-audcap`, `totk-post`,
`totk-gpufence`, `totk-pcache`, `totk-prewarm`, ordered slot owner `totk-submit`, and input/autoload helpers. Existing `totk-present` and `totk-vsync`
names remain. Guest ids are used because the SVC thread object has no guest-name field. A guest thread first
executing a submission becomes `totk-walk` permanently; this also identifies the async-replay thread when used.
Driver/platform-owned threads are not renamed. Apple scheduling/naming behavior is unchanged.

On **Android**, guest placement defaults to `TOTK_AFFINITY_GAME=auto`; all other placement/priority controls
remain **unset**. Linux host default placement and existing guest/audio priority boosts are unchanged.
Set launch overrides in `totk.env` and restart:

| Variable | Meaning |
|---|---|
| `TOTK_AFFINITY_WALK` | Host CPU affinity when a thread first executes `mtl_sub_begin`; applied once per host thread, not on every draw/frame. |
| `TOTK_AFFINITY_RECW` | Affinity of every recorder chunk worker at startup; the same mask applies to each. |
| `TOTK_AFFINITY_GAME` | Android main/created guest affinity: unset or `auto` selects the capacity policy below; `all` makes no affinity request; a CPU list/mask overrides auto. WALK still overrides it on an actual submission thread. |
| `TOTK_PRIO_WALK` | Linux per-tid nice value, decimal **−20…19**, on first submission. Unset leaves the inherited/current nice untouched. |
| `TOTK_PRIO_RENDER` | Linux per-tid nice value, decimal **−20…19**, at the start of each frame-critical host thread: the walker (`totk-walk`, applied before `TOTK_PRIO_WALK`, which wins), `totk-recw*`, `totk-submit`, `totk-gpufence`, `totk-post`, `totk-present`, `totk-vsync`. Never compile/seed/cache/translator workers (they keep +10) or guests (−8). Unset leaves the inherited nice untouched. Live through `totk.ctl` (*Live switches*): set by tid on every registered render thread at once, `[totk-ctl] gen=N TOTK_PRIO_RENDER=…: n render threads set …`. |
| `TOTK_UCLAMP_WALK` | Linux utilization-clamp minimum, decimal **0…1024**, on first submission. Zero clears that minimum; this is not a frequency in MHz or a percentage. |
| `TOTK_TIMER_SLACK_NS` | Linux per-thread timer slack, decimal **1…50000** ns, at the start of every guest thread (`g-*`) and frame-critical host thread (`totk-walk`, `totk-recw*`, `totk-submit`, `totk-gpufence`, `totk-post`, `totk-present`, `totk-vsync`). Unset keeps the kernel's inherited 50 µs; `0`/out-of-range log `EINVAL`. Logs the first application once (`TOTK_TIMER_SLACK_NS=1 result=1`). Live through `totk.ctl`: each of those threads applies it itself before its next timed wait (guest sleeps / timed waits / yield back-off, the frame threads' loops), the presenting thread at the application. |

CPU values are lists/ranges such as `7`, `3-6` or `3-6,7`, or explicitly `0x`-prefixed hexadecimal masks:
`0x80` means CPU 7 and `0x78` means CPUs 3–6. A bare `7` is **CPU 7**, not mask 7; `0` is CPU 0, not disabled.
Empty/zero masks, malformed values and out-of-range ids/nice/clamps log `EINVAL` without changing that setting.
Each name/setting logs `[totk-thread] tid=… name=… <setting>=… result=… mask=… errno=…`; `mask` is the
**resulting kernel affinity**, not merely the requested value. Nice/clamp `result` is the queried current value.
Permission errors, including `EPERM`, are visible and do not silently enable a substitute policy. Settings are
attempted once; correct the env and restart after an error.

Timer slack only moves **timed** sleeps/timeouts of the thread that sleeps: guest `SleepThread`, timed guest waits
and core rechecks, the opt-in yield back-off, the predicted-acquire, adaptive-commit and present-retire deadline
waits. The display/acquire/slot pacing waits are untimed condition waits released by the `totk-vsync` callback and
do not change. `sh tests/timer_slack/run.sh <absolute .scratch>` checks the switch and prints host `nanosleep`
lateness (x86 host: 10/100/1000 µs sleeps woke ~52/52/54 µs late at p50 with 50 µs slack, ~1.6/1.9/3.8 µs with 1).
On device, compare `[svc-wait]` `SleepThread` time and frame-time p95 / >50 ms counts with it unset and `=1`.

Without `TOTK_PRIO_RENDER`, host threads keep the nice of the thread that created them (Linux inherits it).
Guests start at −8 (`host_thread_boost_self`), but `g-main` is never boosted and keeps the boot thread's nice.
So the walker (a guest), and the recorder workers and `totk-submit` it creates, run at −8, as does the
presenter when a guest presents, while `totk-post` and `totk-gpufence`, created by `g-main` in
`nv::InitializeGraphics`, stay at the boot thread's nice (normally 0) behind the −8 guests. Measure with
`TOTK_PRIO_RENDER=-8` / `-10` against unset: frame-time p95, >50 ms counts and `[svc-wait]`/present-stage times.

Android snapshots the startup thread's current allowed set once, after reading `totk.env` and before launching
the game. It reads `cpuN/cpu_capacity` for **every allowed CPU**. If any capacity is absent/unreadable/invalid,
it retries the entire set in consistent units using each cpufreq policy's `cpuinfo_max_freq` (per-CPU policy
links or `cpufreq/policy*/related_cpus`); it never reads a user's `scaling_max_freq` cap or changes caps.
Only when the lowest tier is **strictly below 60%** of the highest does it remove that lowest tier, and only
if at least two CPUs remain. Single-tier, all-big/two-similar-tier designs, incomplete sysfs and already-small
cpusets retain the allowed mask. No CPU numbers or device models are hard-coded. The immutable mask is
applied at the start of **every** `g-main` / `g-N`, even if its creator had a narrower WALK mask.
This does not change virtual-core count/masks, nice, uclamp, WALK, recorder or other host-thread policies.
Non-guest children still inherit Linux affinity normally; they receive no new GAME placement calls.

Auto logs one startup decision (zero means unreadable in an incomplete source):

```text
[totk-thread] game affinity auto: source=capacity cpus=0:256,1:256,2:256,3:768,4:768,5:768,6:768,7:1024 allowed=0-7 mask=3-7 decision=exclude-lowest
```

Those **synthetic example capacities are not measured device values**. Thor's expected suffix is
`allowed=0-7 mask=3-7 decision=exclude-lowest`; Odin 3's two all-big tiers should produce
`allowed=0-7 mask=0-7 decision=unchanged`. Actual CPU values and source are printed from sysfs; an Android
cpuset may narrow `allowed`. `source=max_freq` is the complete-policy fallback and `source=unavailable`
leaves the mask unchanged. Per-thread `TOTK_AFFINITY_GAME=auto` logs confirm kernel readback.
Any explicit value other than `auto` bypasses detection, **including empty/invalid values** (which log the
existing error, not auto). `all` restores the old no-request/inheritance behavior; it cannot undo cpuset
restrictions. Explicit lists retain their existing semantics, including deliberate single-CPU experiments;
automatic placement never selects a single CPU.

Uclamp uses `sched_setattr` with `SCHED_FLAG_UTIL_CLAMP_MIN` and KEEP_POLICY/KEEP_PARAMS: it does not change the
scheduling class, nice value or maximum clamp. Older kernels may reject it; Android permissions, cpusets/cgroups
and thermal/frequency caps can restrict all these requests. `sched_setaffinity` cannot escape a disallowed cpuset.
Linux children inherit their creator's affinity/nice, including non-guest service/compiler/driver threads:
when pinning WALK, set RECW explicitly so new recorder workers do not inherit its single CPU. GAME does not
reserve those CPUs or alter Horizon's separate virtual-core scheduling.

The default follows Thor scheduler evidence: most investigated guest-core stalls left a guest runnable on
a LITTLE CPU while CPUs 3–7 were idle. At the same village save, two 240-s runs of explicit GAME `3-7`
reduced controller downshifts from 11/11 to 4/5 and long frames from 2.17%/0.99% to 0.41%/0.38%, without
raising measured power (4.90/4.99 W versus 4.83/4.40 W). These observations motivate placement, not a universal
FPS claim. For a baseline use `TOTK_AFFINITY_GAME=all`; compare to unset/`auto` with identical save, thermal,
power, controller and cache settings. WALK/RECW affinity, priority and clamp remain separate experiments,
not shipping defaults. Do not change intentional ClusterTune caps or infer a LITTLE tier on all-big Odin.

Host coverage: `make -C games/totk/android TARGET=linux CC=cc OUT=<scratch> threadplacetest` builds a game/GPU-free
test of synthetic capacity/frequency-policy trees (Thor, Odin, threshold, missing/malformed data and restricted
cpusets), keywords/explicit overrides, shared auto masks for main/created guests, parsing, name truncation,
actual per-thread affinity/nice/uclamp, once-only walk identification, failure handling and unchanged parent
state. The binary is also built by `vktest`; execute `<scratch>/threadplacetest` from a scratch working directory.

### Guest SVC waits and virtual-core handover

`TOTK_SVC_PROFILE=1` enables bounded per-guest-thread wait accounting. Every 300 actual NVN presents,
`[svc-wait]` prints the submission/present thread (`walk=1`, guest handle/id and host tid), plus five other
guest threads ranked by completed object/dispatch/core wait time. SVC rows separate `WaitSynchronization`,
`ArbitrateLock`, `WaitProcessWideKeyAtomic`, `WaitForAddress`, `SleepThread` and yields. Counts are blocking
episodes (core counts are host condition-wait episodes), not all calls including successful fast paths.
`object_ms` ends when the guest is made ready; `dispatch_ms` is ready-to-host-resume delay;
`core_ms` is the subsequent wall time inside `core_take`'s condition waits. `CoreOther` includes core
reacquisition after blocking host services, rather than incorrectly blaming the last guest SVC.
Sleep/yield rows exclude measured core-wait time. Divide 300-frame totals by 300 for ms/frame.

`[svc-peer]` identifies the object address/key (single sync handle for a one-object synchronization wait),
mutex owner or single joined-thread handle when known, and the actual signaling guest handle (`waker`).
An address arbiter/condition/event has no intrinsic owner: its signaler is evidence, not a guessed lock owner.
`waker=0` means host-origin signal or timeout, not guest handle zero. Multiple sync handles use `addr=0`;
the existing thread dump retains their handle list. Each thread retains 16 distinct object/owner/waker
tuples and 12 core-holder tuples; overflow is explicitly reported as `other`. `[svc-core]` records compatible
core owners sampled at the beginning of each host core wait, with their guest priorities and core ids.
Holder snapshot times may overlap across eligible cores and may include handovers before wakeup:
they are **not** an exact additive ownership timeline. Windows charge completed waits in full; long waits
can span windows, and closed/exited thread records can disappear before the next report.
Profiling is off by default; allocations, clocks and per-wait tables are avoided while disabled.
Reports take the scheduler lock, so compare instrumented cases to each other, then repeat without profiling.

`[svc-tiny]` (same switch, measurement only) counts short guest timeouts per window: zero-timeout polls,
timed waits asking under 20 µs in bins `<1/<2/<5/<10/<20 µs` with how many timed out, and `SleepThread` under
20 µs. Shown threads get a row; one `all threads` row sums every guest and names the busiest handle. Large
counts mean the game polls with tiny timeouts, which timer slack (`TOTK_TIMER_SLACK_NS`) and wake latency inflate.

`TOTK_READY_CORES=1` opts into ready-at-wake virtual-core competition. A signaled guest queues immediately
under the scheduler lock and can reserve a free core before Android schedules its host thread. Otherwise
it requests a strictly higher-effective-priority handover at an existing safe SVC/signal checkpoint.
The checkpoint revalidates masks, priority inheritance and starvation protection instead of blindly
honoring an old preempt flag. Equal-priority FIFO order and the existing three core ids/masks remain intact.
No extra virtual cores, priority boost, running-core theft, recorder change or lag-policy change is involved.
The old policy can hand a released core to a lower-priority queued thread while a better just-signaled
thread is still waiting for host scheduling. The CPU-only regression includes that negative control.

Recommended shipping default remains **unset/off pending device A/B and correctness runs**; `=0` is an
explicit baseline. Earlier guest readiness can expose game scheduling assumptions; a reserved core can
also sit idle until its host thread is scheduled. In this mode some virtual-core queue delay can occur
*before* `core_take`, inside `dispatch_ms`; compare `dispatch_ms + core_ms`, not only `core_ms`.
Do not expand core count: the game indexes unprotected per-core allocators/job queues by the guest core id.
`TOTK_FREE_CORES=1` is a **failed correctness control, not a proposed performance experiment**:
on Thor/Lookout with protected lag and a 600-draw chunk setting, the game hung shortly after frame 300
(28.2 fps until then; silent running audio, no signal/crash). Keep it absent or `=0` (both leave the virtual cores
on). It permits simultaneous execution on the same bookkeeping core.
The ready-at-wake option preserves exclusive ownership and guest masks instead of bypassing them.

Host coverage: `sh games/totk/tests/guest_waits/run.sh /absolute/path/.scratch/guestwaits/tests` runs the
deterministic production scheduler probe and existing thread/SystemEvent/address-arbiter probes with both
handover policies. Outputs stay outside `build/`. Linux C test entries run on their pthread stack:
ARM64 guest stack switching, asynchronous signal preemption, rendering and device performance are not tested.

`TOTK_CORE_DIAG=1` additionally enables completed virtual-core wait accounting and bounded owner/CPU
snapshots. A nonempty `DM_NVN_TSV` enables the completed counters without owner/table logging.
Both are launch-time settings; restart after changing them. With both disabled there are no added
diagnostic clocks, counters, allocations or snapshot scheduler locks. The required three-core
arbitration, masks, ready-at-wake setting and preemption policies are unchanged.

- `@core_wait_self_ms`, `@core_wait_all_ms`, `@core_wait_all_n` report completed waits at the actual
  core grant, including ready-at-wake reservations. Self time is `-1` when no same-presenter baseline
  exists. These metrics overlap: do not add self to all-thread time, or interpret all-thread time as
  wall-frame latency.
- `[core-wait]` reports long-wait masks and copied owner handle/TID/priority/held/last-SVC metadata.
  Linux/Android does not suspend guest threads or read their register state; no owner PC is guessed.
- `[core-table]` samples held/CPU/wait deltas at most every ten seconds, with at most 256 rows and
  explicit truncation. Snapshot values are protected by the scheduler lock; slow output happens
  after releasing it. CPU `-1` means unavailable, not measured zero.

The CPU-only production fixture `tests/core_wait_stats/run.sh <absolute .scratch output>` exercises
disabled, TSV-only and diagnostic modes for TOTK/BotW. It does not establish device scheduling gains.

The sleeper-aware notification experiment was removed after the Hateno Village
device run skipped only two wake calls across 12,300 frames. Object/core/startup
condition notifications remain unconditional, with no per-thread sleeper or
outstanding-notification bookkeeping.

`TOTK_SYNC_STATS=1` prints `[sync-stats] frame=... window=300` every 300 actual
presents, independently of `TOTK_SVC_PROFILE`. `wake_issued` counts unconditional
condition-signal calls, not measured Linux syscalls on other libcs.
`block_parks` includes startup and object condition waits; `core_parks`
counts core condition-wait entries, including spurious/timeout retries. Counters
are opt-in, bounded, allocation-free and reset per reporting window. Startup
logs the resolved launch settings; restart to change them.

`TOTK_SYNC_KICK` defaults to `1`; `=0` restores per-contender signal sends and
the non-READY checkpoint's old unconditional handover. With it enabled, all
waiters share each owner's generation and 1-ms retry timestamp. A pending kick,
an acknowledged refusal within that interval, or a handler/checkpoint already
committed to queue revalidation coalesces duplicate sends. Unacknowledged and
unsafe-PC requests still retry after 1 ms; a coalesced request keeps the short
core-wait retry rather than accidentally falling back to 20 ms. Releasing a
core invalidates its generation/pending state and retry timestamp.

The handler retains top-level host-slot and original guest-PC admission, the
game-vs-SDK restriction, inherited effective priorities, 20-ms starvation escape
and 2-ms protection. The checkpoint rechecks those scheduling predicates under
`G` before handing over, so a changed mask/priority/queue cannot make a stale
flag cause a pointless switch. Three virtual cores and existing FIFO ordering
are unchanged. `kick_sent`/`kick_coalesced` establish the mechanism;
`kick_ack`, `kick_refused`, `async_handoffs`, `checkpoint_handoffs` and `stale`
separate delivery/refusal from actual handovers and stale checkpoint requests.
Signals coalesce in the host kernel too: acknowledgments need not equal sends.

`kick_refused` has two causes: a cleared/missing top-level host-stack slot or
non-original-guest PC (unsafe host/reentry/gate context), and an admitted SDK PC
with a better-priority waiter but no starvation escape. A request that is stale,
no longer beats the owner, or falls inside the protection window does not
increment that counter. The aggregate cannot distinguish host from SDK refusals.

In the reported sync1 Hateno walk, 14.5 refusals versus 23.4 sends per frame
(about 62%) did not establish a cheaper safe retry policy. At 29.82 FPS those
rates are about 432 refusals and 698 sends per second; CPU cost per send/retry
was not measured by these counters. Retry work includes the timed park/wakeup,
relocking `G`, scanning masks/effective priorities/starvation, signal send and
delivery, and the handler's admission check.

By default a host-context refusal still has no near voluntary checkpoint:
`runtime_svc` checks at entry and `gate.S` returns directly. Calls may reenter
guest code or return into no-SVC spins, so pending/acknowledged coalescing
alone must not suppress progress retries.

`TOTK_CORE_WAIT_EVENT=1` (startup-only, default `0`) instead parks core waiters
on the existing owner-release grant/signal, with a 20-ms safety retry rather
than the kicked owner's 1-ms retry. Top-level HLE return checks pending handoffs
after host locks are dropped, before restoring the guest stack; nested HLE calls
do not park, return registers/errno survive, and opaque HLE resumes on the same core.
The startup `[totk-core-wait] event=...` log proves the arm; compare core parks,
kick counts and core-wait tails with `TOTK_SYNC_STATS=1` / `TOTK_CORE_DIAG=1`.
Masks, inherited priorities, protection and starvation remain authoritative;
refused signals and no-SVC guest/SDK spins retain the bounded 20-ms fallback.
T7 bounded the old park's Odin critical-path contribution at 0.64–0.87 ms/cycle:
this is an opt-in latency/CPU experiment, not a claim of a large FPS gain.

The sync-fastpath suite covers both event arms and includes a production-CV
probe measuring release-to-waiter-running latency (1,024 samples, p50/p95/max
reported, p95 <200 µs—not a worst-case bound) while asserting that a host-blocked
owner does not produce millisecond retries. It races owner release against the
waiter's ownership-check/atomic-park boundary and requires zero safety-timeout
expiries. It also covers same-core HLE return and stale mask,
priority-inheritance and protection revalidation. Host scheduling is not realtime.

The same single-run device comparison was 29.82 FPS and 41 controller decisions
(20 over 32.5 ms) versus texel1's 29.74 FPS and 55 decisions (29 over 32.5 ms),
with no observed crash/abort/hang. Treat that difference as noise-level evidence,
not an established speedup or long-session hang guarantee.

For device A/B, compare startup `TOTK_SYNC_KICK=0/1` with `TOTK_SYNC_STATS=1`,
then repeat without counters. `=0` restores the old kick/checkpoint policy,
not bit-identical instruction cost. Reduced `kick_sent` with nonzero
`kick_coalesced` confirms avoided signal sends, not a proven frame-time gain.
Require no longer core-wait tails or starvation growth.

`sh tests/sync_fastpath/run.sh <absolute .scratch output>` exercises both kick
settings. Its real SIGUSR2 fixture admits only a bounded synthetic guest
spin region, with synthetic host slots, and runs the production handler and
three-core arbitration. It cannot establish ARM64 translated-PC/stack-slot
integration or exclude a device-only hang; save loading, busy-area entry and
long play at unchanged clocks remain required before promotion.

For TSan, use
`CC="/usr/lib/llvm-21/bin/clang -fsanitize=thread -g" TSAN_OPTIONS=halt_on_error=1 SYNC_SIGNAL_INJECT=1 SYNC_ROUNDS=30000 SYNC_PREEMPT_ROUNDS=3000 sh tests/sync_fastpath/run.sh <scratch>`.
LLVM TSan defers real asynchronous callbacks outside interceptor boundaries:
the callout-free guest spin can receive hundreds of thousands of sends without
entering the handler. The explicit injected mode keeps real kernel sends
blocked on the synthetic owners and calls the fully instrumented production
handler at lock-free safe points with a synthetic guest PC. All scheduler and
handler code remains instrumented, with no suppressions. This validates queue,
park, acknowledgment and handover interleavings, **not** true asynchronous
interruption under TSan; the normal suite supplies real SIGUSR2 coverage.

### Releasing a virtual core during contended host locks

`TOTK_VCORE_RELEASE` defaults to `1`; startup `=0` restores the previous core
occupancy for the new host-lock scopes. It does **not** disable the preexisting
SVC or blocking NVN handoffs. `TOTK_VCORE_STATS=1` enables an independent
300-actual-present `[vcore-stats]` report; counters default off.

CPU texture reads/writes first try the recursive recording mutex. Uncontended
and recursive acquisitions do no scheduler work, clock reads or accounting.
On contention, a guest caller releases its core using the same `core_put` /
`core_take` queues as SVC waits, completes the host operation, unlocks recording
ownership, and only then reacquires a core. Recording ownership, dirty
snapshots, write-back ordering and synchronous texel semantics are unchanged.

Unlike a blocking SVC, an opaque NVN call does not prove that the guest may
migrate. The helper therefore constrains resume to the original virtual core,
without changing its guest mask or reserving that slot while blocked. A
concurrent explicit mask change that excludes that core takes precedence.
The released slot can run another eligible guest; resume still competes by
effective/inherited priority, equal-priority FIFO, preemption and starvation
protection. No fourth core is introduced.

`runtime/host_wait.h` is restricted to audited host-only scopes: enter with
no host mutex held, invoke no guest callbacks while released, and drop every
host lock before `end()` can park. At the texel backend call, descriptor/stride
validation and `x_drain()` have completed; neither `G` nor recording, texture,
pool or memory mutexes are held. The backend neither executes guest code nor
uses guest per-core allocators. Its user-owned source/destination storage stays
subject to the same synchronous NVN lifetime contract. Any guest locks the
application holds remain held: a competing guest that SVC-blocks on them gives
up its borrowed core, and inherited priority plus the existing asynchronous
preemption/starvation path governs the returning owner. This does not promise
progress for an arbitrary nonpreemptible guest spin; do not disable the existing
anti-starvation machinery or generalize this to arbitrary host callbacks.

The command-copy table (`AddControlMemory`, `EndRecording`) remains unconverted:
absence of host callbacks/locks inside its critical section does not establish
that guest-side command-memory/per-core critical state may be suspended there.
Vulkan HLE pool finalization uses the separately audited snapshot boundary below;
MapVirtual retains its registry-locked deletion path. Do not wrap
`vk_flush_wait`, registry locks or arbitrary callbacks generically. GPU/display/translation waits
that already use `NVN_BLOCKING()` retain their existing policy.

Android close/activity-destroy uses `_exit`, not concurrent renderer/scheduler
destruction or `pthread_cancel`. A guest `CloseHandle` only marks a live thread
closed; its `thr_t`, condition variable and host stack remain alive until that
thread returns from the host call, reacquires its core and executes ExitThread.
The helper publishes no descriptor/token pointers to another thread. Process
exit has no drain dependency; normal thread teardown does not free a released
caller's state early.

Read the device lines as follows:

```text
[threads] host vcore release=1 stats=1 (TOTK_VCORE_RELEASE, TOTK_VCORE_STATS)
[vcore-stats] frame=... window=300 contended=... releases=... blocked_ms=... blocked_max_ms=... host_ms=... reacquire_ms=... reacquire_max_ms=...
```

`contended` counts failed try-locks on guest callers that owned a core,
including when release is disabled. `releases` counts actual handoffs.
`blocked_ms` / `blocked_max_ms` measure released acquisition wall time;
`host_ms` additionally includes the protected host operation through unlock.
`reacquire_ms` / `reacquire_max_ms` measure unlock-to-core-resume wall time.
Times are recorded only for released operations, are summed across callers
(not main-thread latency), and are attributed on completion; counts are
attributed on entry, so a call spanning a report boundary can split windows.
No new clocks, counters, allocation or reporting locks run with stats off.

`sh tests/vcore_release/run.sh <absolute .scratch output>` links the real
recording lock and production scheduler without a Vulkan device or guest image.
It covers a blocked texel caller freeing a core for another guest, same-core
resume despite another free slot, recursive/uncontended paths, startup-off and
stats-off behavior, inherited priority/FIFO, explicit masks, cancellation and
subsequent timeout, a borrower blocking on a guest mutex held by the releaser,
live-handle closure/ExitThread, process shutdown during a released lock wait,
and 24-thread/three-core stress. Set
`CC="/usr/lib/llvm-21/bin/clang -fsanitize=thread -g" TSAN_OPTIONS=halt_on_error=1 VCORE_ROUNDS=10000`
for the instrumented matrix. The existing guest/core and real-signal/injected
TSan sync matrices remain required; synthetic host coverage cannot establish
an on-device FPS improvement or guest pointer-lifetime assumptions.

`sh tests/sync_fastpath/bench.sh <scratch>` alternates five runs of pinned
`70d3e68`, the kick-off control and the default with identical code/compiler
workloads and counters disabled. It reports wall/process-CPU time for
wake/block ping-pong and 24-worker fan-out under three-core contention.

### Horizon ideal-core semantics (`TOTK_IDEAL_CORE_FIX`)

Default unset/`0` keeps the old TotK bookkeeping: CreateThread stores ideal core −2/−3 verbatim with mask 7,
SetThreadCoreMask may leave the ideal core outside the new mask, and GetCurrentProcessorNumber of a thread holding
no virtual core can report a core outside its mask (or −2). `=1` uses the Horizon rules Breath of the Wild already
follows (1.4.3 `main.npdm`: ideal core 0, cores 0–2): −2 becomes core 0 with a one-core mask, −1/−3 are invalid at
creation, mask changes are validated, the main thread starts with mask 1, and a core of the mask is reported. This
moves −2 threads onto virtual core 0 beside the main thread. Startup logs `[threads] ideal core fix=N
(TOTK_IDEAL_CORE_FIX)`; `sh tests/ideal_core/run.sh <absolute .scratch>` pins both arms. A/B frame-time p95 and
`[core-table]` held/wait times on Odin 3 (CPU-bound) before considering it.

### Synchronous memory-pool finalization without a retained virtual core

The Vulkan `MemoryPoolFinalize` boundary now waits **before** registry teardown,
not with `g_pool_mu` held. The original ordered path was:

1. HLE `x_drain()` finishes queued CPU replay (not GPU execution).
2. `nvn_dirty_remove_range` restores writable pages, retires dirty/lag-guard
   metadata and waits for any active guard owner as before.
3. Backend `del_range` takes `g_pool_mu`, removes the newest matching registration
   and bumps span generation. Virtual registrations lose their aliases.
4. For a physical owner, bump owner generation, demote nested views to CPU-only
   and remove aliases targeting the owner; wait through the latest reserved GPU
   ticket, including asynchronous recorder/submit-worker work not yet queued.
5. Still under the registry lock, export/heap pools copy bytes to fresh ordinary
   shared pages and `mremap(MREMAP_FIXED)` them over the same guest address, close
   the dma-buf, destroy the VkBuffer and release memory. Host-import pools only
   destroy/release their Vulkan objects; guest pages stay mapped.
6. Unlock registry, cancel overlapping write-back/history records, invalidate
   shared-texture identities, free the registration. HLE finally unmaps any
   vpool-owned host storage, restores `game_mem`, and clears pool magic.

GPU-idle is necessary: vertex/index/uniform/storage/indirect/transfer accesses,
texture uploads and other recorded commands can still refer to a pool VkBuffer
or its shared pages. A view or virtual alias can reference the same owner.
`x_drain` alone does not retire GPU reads; reserved tickets include frozen span
snapshots still being assembled/submitted by host workers.

The new Vulkan-only `mtl_pool_finalize` keeps those synchronous lifetime effects
but precedes teardown with a snapshot/wait/recheck loop. First snapshot the
latest reserved ticket with no registry/recording lock. If incomplete, release
the virtual core and wait before dirty retirement; this also prevents an armed
dirty-reader guard from retaining the core inside `nvn_dirty_remove_range`.
Dirty retirement remains before any unbacking. Then, under recording ownership,
seal any open stream and snapshot its ticket. If work is incomplete, drop
recording ownership, release the core if not already away, and wait on the
completion condition with no registry lock. Reacquire recording, seal intervening
work, and recheck the latest frontier. Only when every reserved ticket is
completed may `del_range` lock the registry and run steps 3–6 without a GPU wait.
A second check under the registry lock asserts this invariant. Recording
ownership excludes new reservations and span captures until destruction ends;
workers can finish only already-reserved tickets. Late references between
snapshots cause another real wait, not early destruction. No pool pointer
survives an unlocked wait. A guest submitting after finalization has invalid
lifetime usage, but cannot resolve the retired registration.

The release token remains live across retries and all bookkeeping, and ends only
after recording, registry and resource locks are dropped. Same-core return,
explicit mask overrides and thread lifetime use the existing production helper.
Other guest threads can initialize/resolve pools and take the freed core while
GPU completion progresses independently on host workers. The special boundary
is not used by MapVirtual: that caller holds `g_map_mu` and retains its existing
deletion semantics. No generic release under registry/alias locks was added.

Deferred return is deliberately rejected: host-imported guest pages may be
reused immediately, and dma-buf unbacking/MAP_FIXED and dirty-slot retirement
must finish before return. Keeping only a VkBuffer alive cannot protect reused
guest bytes. Releasing around the old wait with `g_pool_mu` held is also rejected:
pool init, aliasing and lookup can need that lock on the borrowed core.

`TOTK_POOL_STATS=1` enables cumulative **completed Vulkan HLE finalizations**:

```text
[pool-stats] finalize=... wait_ms=... wait_max_ms=... core_released_ms=... (cumulative completed calls)
```

`wait_ms` sums completion-condition waits across snapshot retries and dirty
retirement wall time when the initial snapshot was incomplete; `wait_max_ms` is
the largest per-call sum. `core_released_ms` sums the released
host-work scope through final unlock, excluding core reacquisition. It is not
GPU time, main-thread latency or predicted FPS savings. Stats default off:
no stats clocks, counter updates or report mutexes in that arm.
`TOTK_VCORE_RELEASE=0` disables the new helper handoff for comparison, not the
safer snapshot/recording exclusion or preexisting `x_drain` handoff.

`sh tests/pool_finalize/run.sh <absolute .scratch output>` runs real Vulkan
gated reads, a late second submission requiring a fresh wait, borrowed-core
pool initialization/lookups, view/alias invalidation, writable byte preservation,
immediate MAP_FIXED reuse and new dirty tracking, eight-guest concurrent
init/finalize stress, stats/release controls and process shutdown while finalizing.
Run on lavapipe in both default and `TOTK_FRAME_LAG=1 TOTK_VK_REC_THREADS=2`
arms. `CC="/usr/lib/llvm-21/bin/clang -fsanitize=thread -g"
TSAN_OPTIONS=halt_on_error=1 POOL_ROUNDS=300` runs the same host/scheduler paths
instrumented; the driver itself is not instrumented. Existing full Vulkan and
guest/core/vcore/sync matrices remain required. Android close/destroy uses `_exit`;
the shutdown test covers that process-exit model, not Java lifecycle/device
integration. This change establishes no measured on-device FPS improvement.

## Shader compiles and the pipeline cache

Turnip needs 70–150 ms to compile each pipeline with a shader it hasn't seen, which used to freeze the game for up to
3 s on the first visit to an area. Pipelines the driver has to compile are now built on two worker threads
(`TOTK_VK_COMPILE_THREADS`, `0` compiles at the draw as before), and their draws are left out until they're ready:
on a first visit, new objects and effects can appear a few frames late. Pipelines already in the cache are made at once.
Non-screen colour captures (such as small square light-probe faces) still compile
at the draw: a capture made with draws left out kept its result, and Gerudo Town's plaza stayed visibly different. With
that, the longest first-visit freeze in the Gerudo Town start went from 3.8 s to 0.8 s.

The aligned scene-size classifier is always on for colour-target pipeline keys; it has no
environment-variable switch. It accounts for scene-allocation alignment instead of requiring
nearly exact16:9: requested960x540 can produce960x536, and864x486 can produce864x480.
The exact historical16:9 path is retained; additional matches require at least640x360,
landscape extents, and an8/16/32-aligned floor of either16:9 or the configured
`TOTK_RENDER_RES` aspect. Small square/cubemap/probe captures remain synchronous; depth-only
maps keep their existing asynchronous policy. This changes classification, not the
capture-correctness rule. Dimension-based policy is conservative inference, not a semantic
lifetime oracle: verify persistent Gerudo lighting and actual target traces before attributing
a capture-sync label to a real cached probe.

The cache lives in the app's private storage (`state/vk-pipeline-cache.bin`) and grows as you play.
`sh install_app.sh cache-export` copies it to the computer; `sh install_app.sh cache-import FILE` gives it to the app
(on this or another device), which merges it at its next start and renames the file to `vk-pipeline-cache.bin.imported`.
Both primary and import paths validate the same **app context envelope**: exact effective `TU_DEBUG` string
(unset and empty normalize to empty), SHA256 of the actual private copied `driver.so` when selected, and
the opaque Vulkan payload's vendor/device/cache UUID. Context is captured after `totk.env`, the A722 default
and driver loading. System-driver selection uses a distinct marker and the Vulkan identity, not a claimed
system-driver file hash. No APK/libtotk build-id is included: app updates must retain valid shader-content entries.
The copied driver is hashed once per start with bounded64-KiB scratch; its log includes `hash_ms`. Hashing
failure disables persistence/import for that run rather than treating an unknown binary as a match.
Size+mtime hash memoization is deliberately not used: the private file is recopied each launch, and metadata
alone cannot prove binary identity if a different driver preserves those values.

On mismatch or malformed/truncated input, the cache is never submitted to Vulkan. A single startup line
identifies the reason and the original is renamed to `<path>.stale`. **The first `.stale` is preserved**:
later mismatches do not replace it; the rejected current file remains until a successful fresh atomic save.
Keep/export that backup before intentionally starting another comparison. The envelope and opaque payload
are written together to `.tmp`, fsynced and atomically renamed; a partial temp file is never loaded.

**Legacy exception:** an old raw cache is accepted only with empty current `TU_DEBUG` and matching Vulkan
vendor/device/UUID, preserving unchanged Thor/Odin caches. Turnip derives that UUID partly from its driver
build-id; this detects a changed build-id, but cannot prove the legacy cache's whole driver SHA256 or its
historical flags (including a different binary retaining the same build-id). With any nonempty current flags,
including the A722 `sysmem` default, unknown legacy provenance is quarantined instead. Accepted legacy
entries are retained and wrapped by one normal coalesced save even without new learning. New envelopes
are not raw Vulkan files for other applications. Rolling back to universal1b safely rejects their unknown
header version before driver use; it then learns a fresh raw cache, so keep an exported compatible backup.

After the one-time accepted-legacy migration, pipeline-cache persistence requires **actual positive cache-size growth** before extracting or writing a blob.
Unchanged-size cache-hit generations are acknowledged and logged `skipped reason=unchanged-size`, with dirty age
cleared and zero payload IO, including on pause/stop/shutdown. Ordinary writes then coalesce at >=60 s and
>=256 KiB growth or 256 successful pipeline creations, preferring background, no presents >=2 s, or sustained
low-draw loading/menu candidates. At **120 s dirty age** the saver overrides those coalescing/quiet gates, never
the actual-growth requirement. The normal crash-loss bound for size-growing changes is **<=125 s plus snapshot/fsync
completion**, conditional on scheduling and successful driver/filesystem operations. This is a **size-change
heuristic**, not exact content-change detection (`change_detection=size-growth-heuristic` in diagnostics).
The acknowledgement records only the generation sampled before the probe; concurrent creates remain dirty.
Same-size content changes are not persisted: the old policy shared this assumption, considered negligible for
append-style driver caches but not guaranteed by Vulkan. The saver retains scratch and requests nice +10;
it prefers a detected clearly smaller CPU tier with at least two CPUs, otherwise inheriting the allowed mask.
See [the policy and concurrency contract](VULKAN.md#coalesced-whole-cache-persistence).

With packed-seed warming and virtual-core release enabled together, the seed worker
and cache saver remain independent host threads with no guest core. Both request
nice +10 and use the same guarded smaller-tier policy; their shared CPU, storage
and memory-bandwidth use can still delay demand warming. No CPU frequencies change.
An in-flight seed warm join is not a new releasable texel-lock scope:
`TOTK_VCORE_RELEASE` changes only audited CPU-texel and pool-finalization scopes.
The saver never acquires the recording lock or seed-IO mutex. Recording overlap
diagnostics cover both ordinary and texel acquisition through unlock, ending
before a released texel caller reacquires its guest core; they do not measure
that reacquisition or prove driver-lock blocking.

For a matched device capture, set `TOTK_PCACHE_STATS=1` in the game folder's `totk.env`, keep ClusterTune,
frame-lag/recorder/driver/seed and route fixed, and compare equal-length warm-cache runs:

- Find `[pcache-stats] placement`: verify actual nice >=10, detector source/decision and actual affinity, and errno 0.
  Homogeneous/unreadable topology retains inheritance; do not mistake a requested mask for an applied mask.
- During area entry expect `deferred reason=min-interval|small-growth|gameplay`; actual size growth must yield
  `taken reason=max-delay` at the bound even if tiny and without quiet moments. Cache-hit-only creation must instead
  yield `skipped reason=unchanged-size bytes=0`, clear dirty age and keep `saves`/`total_bytes` unchanged.
- Open pause/menu or wait at a loading screen: `quiet=low-draw-screen-heuristic` is only a candidate, not decoded
  game state. Press Home: pause/stop should yield `taken reason=lifecycle-flush quiet=background` only if size grew;
  otherwise expect `skipped reason=unchanged-size`, with no extraction or full-cache rewrite.
- Close orderly, then relaunch: startup `pipeline cache: ... from ...` must load the prior complete cache.
  Force-kill tests can lose only the pending window under the conditions above; restart must never read a partial file.
- Compare `total_bytes` / `saves` over matched captures, not only net growth. With no quiet moments there is at most
  one successful snapshot per roughly 120 s of dirty gameplay, versus a changed-size opportunity every 20 s before
  (up to **6x fewer whole-blob writes**, not a guaranteed sixfold measured gain). At comparable ~60 MiB blobs,
  reducing six/four writes to one/two would save ~300/~120 MiB; actual number depends on timing and lifecycle events.
  Quiet saves are >=60 s apart; lifecycle saves deliberately override the interval.
- Inspect `duration_ms`, `thread_cpu_ms` and `overlap operation=... extraction_overlap_ms=...`; persistent long
  recording/create wall times overlapping extraction are an interference candidate even with correct nice/affinity.
  The overlap is not a proven driver-lock blocked duration. Match it to frame timestamps/profiles before attribution.

`TOTK_PSO_TRACE=1` adds opt-in graphics-pipeline diagnostics. Each `[pso-trace]` record identifies the full normalized
pipeline key with a stable 64-bit hash (shader IDs including fragment early-tests, render-target formats, blend,
vertex input and raster state; no process pointers or structure padding). `phase=probe` records cache-only probes;
`phase=build` records actual synchronous, worker, prewarm, retry/takeover or pipeline-library creation, with its
result, duration, origin frame and library part. A thread joining another builder is not itself counted as a build.
For persistence investigations, compare successful build-key sets on the same route across two sessions, press
Home and allow the background cache flush to finish between them, and retain `TOTK_PCACHE_STATS=1` save/load logs.
Repeated shader pairs alone do not establish repeated PSOs: render state can require distinct keys. Tracing defaults
off, constructs/hashes no keys when disabled, and should be removed for clean performance measurements.
The same option emits `[pso-target]` rows at each cold complete-key demand and every actual recording-thread
graphics build, including retries/takeovers/no-cache-control builds. Rows report stable key, actual width/height,
`screen-exact|screen-aligned8|screen-aligned16|screen-aligned32|capture-guard|depth-only`, old-rule decision,
aspect delta, formats/sample count, submission/pass cursor, depth serial/address/mip/layer. Corresponding
`[pso-target-color]` rows identify every color attachment. The pass cursor plus `open` flag describes recording
position, not a recovered guest pass name. Worker records correlate with the earlier demand key; workers never
read mutable live render-target state. Startup/prewarm-only jobs may have no demand row until first use.

For target-lifetime attribution, `TOTK_PSO_LIFE=480x268` enables a separate **diagnostic-only** stream.
`TOTK_PSO_LIFE_START` is the first recording-frame epoch (default1); `TOTK_PSO_LIFE_FRAMES` is the
bounded window length (default12000, maximum30000). `[pso-life]` records actual image handle plus
incarnation/mip/layer/aspect, render/transfer writes, full-clear versus preserved base, and reflected
graphics/compute descriptor MAY-reads, including descriptor-freeze reuse. Identical sampled reads within
one physical pass are deduplicated; compute/storage observations are not. Serial/storage addresses are
metadata, never lifetime identity. A dedicated present-boundary epoch works even when timeline frame IDs
are disabled. The opt-in closes a pending pass before reporting that boundary; it changes no compile policy.

`python3 games/totk/android/analyze_target_lifetime.py <saved-log> --output <report.json>` reports
recurrence and prior-frame-content reads with writer/reader line anchors. Unknown view/range authority,
non-descriptor consumers, unsupported external write ordering, missing boundaries or truncated windows
prevent transient certification. Upload/copy writes do not invent a fully overwritten base. A read in N+1
after an N+1 overwrite is distinct from reading N's retained content before that overwrite. Descriptor
MAY-read is conservative, not a shader texel-fetch measurement; double-buffered addresses alone establish
neither transience nor history. No half/quarter classification relaxation follows merely from dimensions.
No extra game-derived seed/cache payload is required by this change. The APK carries the native saver code; existing
shader provisioning remains `sh install_app.sh data` (or `data-packed` with its existing packed-seed prerequisites).
The cache is learned in private app storage; optional same-driver `cache-import FILE` remains supported. Never commit
or redistribute the generated cache/seed/game data with this code.

`TOTK_PSO_COLLECT=1` records all successful graphics pipeline keys (screen, capture, async and
cache hits) off the render thread through the existing pcache saver. Collections are portable
identifier/render-state-only files next to the driver cache (`<cache-path>.pso-keys`), bounded to
16,384 keys. `games/totk/scripts/merge_pso_keys.py` merges final device snapshots, filters against
`shader_list.tsv`, and writes `games/totk/assets/pso_keys.tsv` in capture/frequency priority order.
This deterministic text source has one complete key per line: exact shader root64 plus any
12-hex seed suffix, early-tests, all 155 render-state words as minimal-width lowercase hex,
capture flag and successful-build frequency. The final three-device source is 1,253,667 bytes
(2,594 keys, 499 capture). No shader code or driver cache bytes are committed.
The APK asset step (`build_android.py` → `apk/build_apk.py`) validates that TSV and the shader
roots and generates the existing `assets/pso_keys.bin` format directly in the APK. There is no
committed/generated binary source fallback; an absent TSV means no packaged list. The TSV
itself is not packaged, and a stale binary cannot silently become the source.

When the APK includes the validated binary generated from `games/totk/assets/pso_keys.tsv`, startup resolves
exact seed variants and prewarms on one of the existing low-priority workers by default. `TOTK_PSO_PREWARM=0`
is the A/B opt-out; `TOTK_PSO_KEYS=/path/list.bin` selects an explicit list. Missing shaders are skipped.
Demand misses preempt queued background jobs; in-flight driver calls cannot be cancelled.
Pressure/synchronous-build pauses protect play; a rapid load can still join unfinished work,
never skip a persistent offscreen capture. Completed bits are saved against the exact list and
successfully persisted driver-cache identity, resuming incomplete work across sessions and
avoiding repeat warmup on an unchanged complete list/cache. Driver or `TU_DEBUG` changes rebuild.
See [VULKAN.md](VULKAN.md#portable-first-visit-graphics-pipeline-keys) for the 792-byte format,
16 MiB budget, pressure heuristics, persistence ordering, progress counters and measurement protocol.

### First-use compute pipelines and learned seed warming

Compute preparation is **always enabled**, independently of portable graphics
key collection/prewarm and graphics compile-worker count.
The dedicated nice +10 `totk-compute` worker creates immutable shader/layout pipelines without
holding the shader-map or graphics-worker queue locks. Replay still owns shader/module/layout
construction. Program bind loads the exact compute variants listed by `.vv` and queues them
without guessing from incomplete resource bindings; shader-load/live-publication resolution
also queues newly known compute modules immediately. Dispatch selects its ordinary exact variant,
joins any unfinished creation, and emits the dispatch only after a valid handle exists.
Compute is **never skipped to hide compilation latency**. Worker/build failure retains the
synchronous creation/error path; it is not treated as successful output.

First actual dispatch learns the exact variant in namespaced `state/vk-compute-used.bin`
(at most 4,096 keys). Five-second coalesced worker snapshots and orderly shutdown use temporary
file + fsync + rename, not recorder IO. Startup resolves learned seeds and queues pipeline creation
while boot/title continues. The bounded versioned file checks complete length, variant names,
GPU/device, `pipelineCacheUUID` and an accidental-corruption checksum; invalid files are ignored,
not partially interpreted. Missing seed keys are reported. A force-kill may lose recent learning.
An early load can still outrun warmup; driver/compiler changes or seed-namespace changes require
fresh learning. No game-derived payload is stored in the key file or committed.

For an attribution run, set `TOTK_COMPUTE_STATS=1` and `TOTK_SEED_STATS=1` (both default **off**):

```
[compute-prewarm] loaded K keys, missing seed M; worker=1 (independent of graphics key lists)
[compute-stats] create source=background key=... spv_bytes=N local=X,Y,Z build_ms=T result=0
[compute-stats] 300-present created_recording=0 created_background=N ready_hits=H joins=J block_total=Xms block_max=Yms failures=0 learned=K
```

After warmup, learned keys should have `ready_hits>0`, `joins=0`, `created_recording=0` and
`block_total` approximately **0 ms**. Compare the seed `block_total`/`block_max` and
`demand_storage_reads` on matched first-visit windows; no sibling-prefetch counters remain.
The adaptive budget should eliminate learned cap skips for the measured village history.
Keep the user's ClusterTune underclock, driver, caches, route, weather, and thermals unchanged.

The supplied cand4 frame 16079's **149.0 ms** timer covers `vk_pcache_compute` /
`vkCreateComputePipelines`, **after** `.vv/.spv/.vk` resolution, module and layout construction.
A driver-cache-miss compile is the likely cause, **not separately measured proof**; internal
driver synchronization/scheduling is included. That log does not identify the shader/size or
prove a single creation, so those details cannot be reconstructed retrospectively. The new
per-create line provides key/size/local dimensions for the next capture. Shader translation
is not inside that timer; on-device translation runs separately after missing-seed requests.
Ready learned warmup could remove **100% of this 149-ms component** (82.6% of 180.4-ms recording,
78.0% of the 191-ms present interval), not all frame work. For an unseen key, early preparation
removes at most `min(bind-to-dispatch overlap,149 ms) / 149 ms` of that component: **0–100%**,
not a measured fixed percentage. Adaptive exact-key seed warming targets the separately
reported seed waits (e.g. 22–23 ms in frames 15565/15567), not this driver timer.


### Optional Vulkan graphics pipeline library

`TOTK_VK_PIPELINE_LIBRARY=1` opts into the Vulkan graphics pipeline library backend; it defaults off.
It is enabled only when both `VK_EXT_graphics_pipeline_library` and `VK_KHR_pipeline_library` are present
and `graphicsPipelineLibrary` is supported. Startup logs the request, support, enablement, fast-linking and
independent-interpolation properties. Without the opt-in, the existing monolithic pipeline path is unchanged.

With the option enabled, compile workers create missing library parts. A draw skips until all four parts it
needs are ready. When a later draw finds a complete ready combination, it fast-links the graphics pipeline
for that draw in the same frame and queues the existing monolithic optimized pipeline build on a worker.
Once that optimized handle is ready, a subsequent draw replaces the fast-linked handle. This trades extra
worker compilation and temporarily less optimized pipelines for earlier rendering; it is experimental and
may increase compile work or expose driver-specific defects. Keep it off unless checking a driver known to
support the required extensions and feature.

The `TOTK_VK_TESTS` `vk_pipeline_test_stats` snapshot exposes cumulative `fast_links`, `fast_draws`,
`optimized_replacements`, per-part `parts[4]` and `part_ns[4]`, `fast_link_ns` / `fast_link_max_ns`,
`optimized_ns`, and `library_skips`. The GPL acceptance path is `./vkpipetest --gpl`; it requests the
option and requires actual support rather than silently falling back.
The 300-frame `[pipeline-library]` report gives fast-link count/total/max milliseconds, actual fast-linked
draws, optimized replacement count/milliseconds, per-type part creation count/milliseconds and misses
still skipped. Zero compile workers retains synchronous monolithic creation; capture builds, bounded
takeover and failure backoff remain in force. Libraries and superseded fast handles remain with their
cache entries for recorded/in-flight command safety. Full pipeline layouts are retained in shader-part
keys for descriptor compatibility, so reuse across shader pairs is intentionally conservative.

### Optional bounded pipeline lookahead

`TOTK_PSO_PREDICT=1` enables Android's opt-in same-pass pipeline prediction; the default remains **0**.
It borrows only already-resolved shader variants, projects private binding state and queues exact keys
on the existing compile workers. It does not load missing shaders, mutate active bindings, speculate
across attachment/control-flow/resource-update boundaries or modify recorder/lag/pacing policy.
Zero compile workers disables this lookahead.

| Setting | Default / bound |
|---|---|
| `TOTK_PSO_SCAN_ADAPT` | `1` when prediction is enabled; `0` uses only the fixed allowance |
| `TOTK_PSO_PREDICT_FRAME_US` | 4,000 µs fixed allowance per frame; 0..1,000,000 |
| `TOTK_PSO_PREDICT_SCAN` | At most 8,192 command records per scan; 1..1,048,576 |
| `TOTK_PSO_PREDICT_KEYS` | At most 32 new keys per scan; 1..160 |
| `TOTK_PSO_PREDICT_US` | At most 2,000 µs per scan, also limited by remaining frame allowance; 1..1,000,000 |
| `TOTK_PSO_RESCANS` | At most 32 rescans after the first per game submission; 0..1,024 |

Adaptive allowance is `max(fixed allowance, actual synchronous graphics pipeline-build time)`, not
their sum. Only time inside a synchronous `vkCreateGraphicsPipelines` call earns credit: cache-mutex
waits, worker joins, background builds, cache probes, compute creation and scans cannot feed it.
Counters saturate and spending/credit reset at the locked frame boundary; per-scan/rescan bounds
remain independent.

A **prediction-origin** pending key waits at first actual demand, rather than newly skipping a draw
whose original cache-only probe might have succeeded. This is an opt-in waiting difference, not a
claim of lower frame time. Ordinary screen-created entries retain their existing async policy and
offscreen captures remain synchronous. Compare default-off, enabled with fixed allowance, then
enabled/adaptive on matched cold and warm cache routes before promoting this option.

With `DM_NVN_TSV` or `TOTK_CORE_DIAG` enabled, `@pso_create_ms`, `@pso_wait_ms`, `@pso_scan_ms`
attribute completed synchronous graphics full-build/compute driver calls, worker joins, and prediction scans.
Creation excludes cache-only probes and cache-mutex wait; compute time is diagnostic only, not graphics lookahead credit.
Optional on-device translation runs in a separate low-priority process; its `[shader-xlate]` timings are not
in-process translator/Metal-library work and do not enter these pipeline timing totals.
Existing Vulkan sampler objects and program/shader/pipeline memos remain authoritative; the upstream
MSL border-key/function caches are not a second cache in the Vulkan backend.

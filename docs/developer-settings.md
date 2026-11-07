# Developer settings (`settings.env` and `totk.env`)

Players never need this page: every player option is in the app's menu (see the [README](../README.md#settings)).
It explains how the menu's choices reach the game, and how developers override them.

## Two files

| File | Where | Written by | Read |
|---|---|---|---|
| `settings.env` | the app's private `files/settings.env` (`/data/user/0/com.aure.totk/files/settings.env`) | the menu, on every change and again at game start (*Start game* or **+**) | first |
| `totk.env` | the game folder (see below) | you; the app never writes or rewrites it | after `settings.env` |

Both use the same format: one `NAME=value` per line, `#` starts a comment. Because `totk.env` is read second, a line
there **overrides the menu**. `settings.env` accepts only the menu's own option keys; anything else in it is logged and
ignored. Both files are read once when the game starts (`games/totk/runtime/frontend_android.c`): edit them, then
restart the game. Do not edit `settings.env` by hand: the menu overwrites it.

The game folder is `<filesDir>/game` (`/data/user/0/com.aure.totk/files/game`) for internal storage, the SD card's
`Android/data/com.aure.totk/files`, or the folder you chose with All files access. The private folder is reachable
only with root (release builds are not debuggable); developer installs made with `install_app.sh` use
`/sdcard/Android/data/com.aure.totk/files`. The folder's full layout (`rewritten/`, `extracted/`, `shaders/`,
`driver.so`, `user-data/`, `totk.env`, `totk.log`) is in
[games/totk/android/APP.md, *The game folder*](../games/totk/android/APP.md#the-game-folder).

A benchmark or bug report made with a `totk.env` present is not on the device's default profile: say so, or remove the
file first (the benchmark tools warn about it, see [benchmarks](benchmarks/)).

## Private update source for device tests

`<filesDir>/update-source.txt` is an optional, root-written file containing only a base origin, for example
`http://192.168.8.2:8042`. It is not an intent extra, preference, exported provider or menu option: other apps cannot
write the app's private files. Remove it after testing. Never ship it.
The `com.aure.totk.test` package never checks for updates unless this file exists; development versionNames containing
`-` still never check. The ordinary update switch and validated-connectivity requirement still apply.

The fake server must answer one `HEAD /AurelioB/totk-android/releases/latest` with `302` and an absolute `Location`
on that same origin ending `/AurelioB/totk-android/releases/tag/vX.Y.Z`. Serve
`/AurelioB/totk-android/releases/download/vX.Y.Z/TotK-Native-X.Y.Z.apk` and the same path plus `.sha256`.
The asset prefix is the single `UpdateRules.ASSET_PREFIX` constant; test APKs can be served under that production
filename. The checksum is `<64 hex>  TotK-Native-X.Y.Z.apk`. The APK must be the test package, newer than the installed
test APK, and signed with its same key. HTTP and redirects within the override's exact origin are allowed only for
this private override; production downloads require HTTPS and GitHub/GitHubusercontent hosts on every hop.
Checks never follow redirects or read a body; the five-second whole-check deadline includes connect/read/DNS.
Pending versions and the default-On switch live in private `pending-update.txt` and `update-checks.txt`, read across
both processes without a SharedPreferences cache. They contain no URL; changing the source requires this private file.


## What the menu writes

`Settings.env()` (`games/totk/android/java/com/aure/totk/setup/Settings.java`, host-tested in
`tests/test_android_settings.py`) turns the menu into these lines:

| Menu option | Value | `settings.env` lines |
|---|---|---|
| Game data location | SD card or chosen folder | `TOTK_DATA_DIR=<path>` (first line; absent for internal storage) |
| Frame rate | 30 fps / 40 fps / 60 fps | 30: `TOTK_FPS60=0`; 60: `TOTK_FPS60=1` plus `TOTK_GPU_TIME=hardware`. 40: `TOTK_FPS60=1`, `TOTK_UNCAPPED=1`, `TOTK_UNCAPPED_HZ=40`, `TOTK_UNCAPPED_INTERVAL=1`, `TOTK_REALTIME=1`, `TOTK_FRAME_LAG=0`, `TOTK_FPS_AUTO=0`, `TOTK_PACE=0`, `TOTK_PRESENT_SLOTS=auto`, `TOTK_ACQUIRE_SLACK=0`, `TOTK_ACQUIRE_CLOCK=0`, `TOTK_FRAME_RATE_HINT=1`, `TOTK_FRAME_RATE_HINT_HZ=120` ([pacing and qualification](../games/totk/android/APP.md#frame-rate)) |
| Frame generation | Interpolation | the locked timing block instead of the frame-rate and latency lines: `TOTK_FPS60=0`, `TOTK_FRAMEGEN=locked`, `TOTK_REALTIME=1`, `TOTK_FRAME_LAG=0`, `TOTK_FPS_AUTO=0`, `DM_PRESENT_HZ=60`, `DM_PRESENT_INTERVAL=game`, `TOTK_PRESENT_SLOTS=1`, `TOTK_VK_PRESENT=ahb`, `TOTK_ACQUIRE_SLACK=0`, `TOTK_ACQUIRE_CLOCK=0`, `TOTK_PACE=0`, `TOTK_ASYNC_REPLAY=0` |
| Render resolution | 1280 × 720, 1152 × 648, 960 × 540 | `TOTK_RENDER_RES=1280x720` (etc.; absent for *Game default*) |
| Latency | Responsive / Balanced / Smoothest | `TOTK_ACQUIRE_SLACK=0` / `1` / `2` (fixed to `0` at 40 fps; part of the locked block with frame generation) |
| Upscaler | Off / FSR 1 / Snapdragon GSR | `TOTK_POST_SCALE=off` / `fsr` / `sgsr` |
| Sharpness (FSR 1) | 0–100 | `TOTK_POST_SHARPNESS=<value / 100>` (20 → `0.2`) |
| Sharpness (Snapdragon GSR) | 0–100 | `TOTK_POST_SGSR_SHARPNESS=<value × 4 / 100>` (50 → `2.0`) |
| Anti-aliasing | Game (FXAA) / SMAA / Off | nothing / `TOTK_FXAA=0` + `TOTK_POST_AA=smaa` / `TOTK_FXAA=0` |
| Shadows | High / Medium / Low | nothing / `TOTK_SHADOW_RES=288` / `TOTK_SHADOW_RES=256` |
| Console mode | Handheld / Docked | `TOTK_OPERATION_MODE=handheld` / `docked` |
| Native HUD | Off / On | `TOTK_NATIVE_UI=0` / `1` |
| Performance overlay | Off / FPS / Full | `TOTK_OVERLAY=off` / `fps` / `full` |
| Overlay position | corners | `TOTK_OVERLAY_POS=tl` / `tr` / `bl` / `br` |
| Second screen | Off / On | `TOTK_COMPANION=0` / `1` |
| Button layout | Nintendo / Xbox | `TOTK_FACE_BUTTONS=label` / `position` |
| Audio latency | Low / Balanced / Safe | `TOTK_AUDIO_PREFILL_MS=10` / `20` / `40` |
| Rumble | Off / 25–100 % | `TOTK_RUMBLE=0`, or `TOTK_RUMBLE=1` + `TOTK_RUMBLE_GAIN=0.25`…`1.0` |
| Motion controls | On / Off | `TOTK_GYRO=1` / `0` |
| Game language | Device language / a language | `TOTK_LOCALE=<device's first language tag>` / `TOTK_LANG=<code>` (`en-US`, `ja`, `de`, `fr`, `es`, `it`, `nl`, `ru`, `ko`, `zh-Hans`, `zh-Hant`, `pt-BR`) |

The exact `settings.env` of each benchmarked device profile is recorded in
[benchmarks/baseline-1.0.0.json](benchmarks/baseline-1.0.0.json).

## Useful `totk.env` switches

Everything not in the menu is a developer switch. A selection:

| Switch | What it does |
|---|---|
| `TOTK_VK_DRIVER=system` | Use the device's own Vulkan driver instead of Turnip. No current Qualcomm system driver passes the game's startup check; for testing only. Pick a custom Turnip AdrenoTools `.zip` or 64-bit ARM `.so` in *Game data → Graphics driver* (or copy the library as `driver.so` in the game folder). |
| `TOTK_ASPECT=21:9` (or `4:3`, …) | Shows the game's view at that ratio with black bars instead of stretching it; the HUD and menus keep their proportions. |
| `TOTK_AUTOLOAD=1` | Loads the newest save again at start. |
| `TOTK_COMPANION=0` | Disables the second-screen companion at runtime (no controller, display listener or poller is started). |
| `TOTK_COMPANION_LOG=1` | Logs each companion status change and the first full snapshot to `totk.log`. |
| `TOTK_INPUT_LOG=1` | Logs each **−** / **R** overlay-chord decision as `[chord] …`. |
| `TOTK_VK_SHOT=300` | Saves every 300th frame as a screenshot. |
| `TOTK_VK_XLATE_STATS=1` | Shader-translation statistics in `totk.log` ([translation-research.md](translation-research.md)). |
| `TOTK_FRAMEGEN=locked` / `hybrid` / `adaptive` / `interp` | Frame-generation modes; *locked* is the menu's *Interpolation*. The others are opt-in qualification modes with their own required timing settings ([frame-interpolation.md](frame-interpolation.md), APP.md *Device profiles*). |
| `TOTK_FRAMEGEN_ENGINE=lsfg` (+ `TOTK_LSFG_PERF`, `TOTK_LSFG_FLOW`) | Experimental LSFG frame generation with your own, privately owned DLL; only with the locked timing block. |
| `TOTK_PSO_PREWARM=0` | Turns off the startup pipeline prewarm from the packaged key list. |
| `TU_DEBUG=…` | Turnip debug flags (e.g. `noslabcs`); on Adreno 722 an absent `TU_DEBUG` defaults to `sysmem`. |

The complete, current list of switches with their status and evidence is in
[games/totk/android/APP.md](../games/totk/android/APP.md) (the *Integration opt-in qualification index* under
[Device profiles](../games/totk/android/APP.md#device-profiles), and the sections on the picture, frame rate and
presentation) and the diagnostic switches in [games/totk/android/DEBUGGING.md](../games/totk/android/DEBUGGING.md).

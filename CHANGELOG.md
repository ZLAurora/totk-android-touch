# Changelog

All notable changes to TotK Native for Android (`com.aure.totk`). The app follows [semantic versioning](https://semver.org);
the game version it runs is shown separately. versionCode = major × 10000 + minor × 100 + patch.

## Unreleased

### Developer tools
- Add opt-in rooted-device CPU profiling with NDK simpleperf frame-pointer stacks,
  exact-build symbol checks, shell-accessible staging, and verified perf-control restoration.
  Per-thread period-weighted self/inclusive reports separate walker, recorder, submit and game CPU.
- Give newly linked `libtotk.so` a SHA-1 GNU build ID retained in the packaged library,
  so native profiles can identify matching unstripped symbols without relying on source revision alone.

### Runtime
- Keep `totk.log` as the direct primary log, but make its duplicate logcat mirror opt-in (`TOTK_LOGCAT=1`).
  Normal launches no longer start its tail thread or wake logd for every frame log. Crash reports remain enabled.
- Let the live-control reader sleep when `totk.ctl` is absent, including after unrelated files are created or deleted.
  Keep its directory watch until the control file appears instead of repeatedly waking on its own watch removals.
- Keep the stall watchdog enabled, but reuse the present timestamp/frame instead of reading a second clock and
  incrementing an atomic counter every frame; publish immutable stage names only when they change.

### Menu
- **Keep the GPU busy** in Graphics → Performance feeds smaller batches earlier and requests clocks that keep up.
  On by default only on Snapdragon 8 Elite-class chips (Odin 3), independently of the chosen preset; it uses a
  little more GPU and CPU clock, power and heat. Restart-based Odin 3 tests raised Hateno from 46.5 to 57.3 fps
  and Lookout from 52.2 to 57.1 fps (4/4 pairs); Rotsumamu stayed at the 60 fps cap. At the 60 fps preset the
  Duo's p95 worsened by 4–5 ms (4/4 pairs), while Thor was neutral, so both keep it off.
  Upgrades without a stored choice adopt the chip default; explicit choices survive. Presets no longer change
  or match this option. Thor and Duo defaults retain 1.0.1's settings.env bytes for every preset, and frame
  generation leaves the option inactive.
- Make controller hysteresis part of the Odin / Snapdragon 8 Elite-class native 60-fps menu profile:
  `TOTK_FPS60_HYSTERESIS=1` is generated only for `CQ8725S`/`SM8750` at 60 fps without frame generation,
  including existing stores and custom mixes, independently of GPU busy. No extra visible option or stored
  preference is added. Runtime unset remains native/off; developer `totk.env` `=0` overrides the menu.
  Other chips and every non-60 preset keep their previous settings.env bytes. Device validation is required
  before this branch is merged.

## 1.0.1 (versionCode 10001), game 1.4.3

Fixes for the first reports, plus a few improvements.

- Updates: when the game starts, the app checks GitHub for a newer version (one tiny request, only when online, never delays the game) and offers to download and install it; a setup step and a menu switch turn it off.
- Update safety: other apps cannot authorize an update through the exported menu; downloads stop if they stall or exceed ten minutes, and the exact bytes handed to Android are checked again before installation.

### Game
- The game pauses, with its sound, while it's in the background or the recent-apps screen is open (on dual-screen
  devices, using the other screen doesn't pause it).

### Setup
- The container route (XCI/NSP + update + `prod.keys`) no longer fails with "Could not create romfs.fifo" when the
  game data goes to the SD card or to a folder you picked. The merge pipe now always lives in the app's private
  storage; an interrupted merge's leftover pipe is removed on the next run.
- Setup errors name the call that failed and its real reason (1.0.0 could report "No such file or directory" for a
  different error).
- Time estimates match measured runs: about 2–3 minutes from decrypted files, about 15 minutes from containers, plus the
  copy time to an SD card.
- Setup stopped: a **Share details** button sends the error and the setup logs (no game files or keys) through any
  app, for bug reports.
- Setup skips the second-screen art on devices with one screen; on dual-screen devices that don't have it yet, Start
  game offers to prepare it.

### Menu
- The menu no longer reports "The game is running" when it isn't: its own background checks are no longer mistaken
  for the game.
- When the game really is still running in the background, the affected actions say so, and a **Close the game**
  button closes it safely (waiting for an in-progress save) and then carries on with what you chose. This also works
  after Android freezes the background game: a temporary menu binding wakes it without bringing its task forward.
- Graphics driver: Turnip drivers can be picked as the .zip they're distributed in (AdrenoTools format), not only as
  a .so.

### Second screen
- On dual-screen devices the game always opens on the main screen, with the companion on the second one, even when started from the second screen.
- The controller keeps working after you touch the companion or an overlay on the second screen (such as AYN's
  performance overlay): Android sends the buttons to the last-touched screen, so the game now takes them back.
- Fused materials use the game's own names (for example "Armor Shard" instead of "Obj Armor Spike B"), including world
  objects that are not pouch items and the Russian names. Existing second-screen art updates automatically on the next
  start; unknown objects fall back to readable text.

## 1.0.0 (versionCode 10000), game 1.4.3

The first public release: one generic, signed APK for arm64 Android 13+ devices with an Adreno GPU. It contains no
game files; everything game-specific is built on the device from the player's own dump.

### Setup
- On-device setup from either route: decrypted files (the 1.4.3 RomFS image or its split parts, plus the ExeFS), or
  containers (base game XCI/NSP, the 1.4.3 update NSP and `prod.keys`, merged on the device). Every input is checked
  against the pinned 1.4.3 hashes before anything is written; wrong picks are refused with the reason.
- The game code is rewritten for Android on the device, the RomFS is copied and verified, the shader seed is generated
  and the second-screen art is prepared, each with its own progress; setup can be cancelled and resumed.
- Storage choice: the app's private storage (recommended), the SD card's app folder, or a folder of your choice with
  Android's optional *All files access*; the game data can be moved later, with a verified copy before the old one is
  removed. FAT32 cards get the RomFS image in parts automatically.
- Per-device graphics profiles from the exact chip model, each Dragonwing part with its Snapdragon counterpart:
  Snapdragon 8 Elite class (CQ8725S, SM8750) 60 fps; Snapdragon 8 Gen 2 class (QCS8550, QCM8550, SM8550) Balanced;
  Snapdragon 7 Gen 4 class (CQ7790S, CQ7790M, SM7750) Light. Other devices start on Light, marked as untested.
  Frame generation and the other experimental options are off in every profile.

### Menu
- A pre-game menu with Graphics (Performance, Picture and Screen tabs, presets), Controls & audio, Game data, Saves and
  About; controller navigation throughout. **+** starts the game from any screen.
- About shows the app version, the game version, the build commit, the detected chip class and screens, the
  licences of every bundled component, the project's home (github.com/AurelioB/totk-android) and the projects it is
  built on (MacNativeGames by ly05010419 and kitcatkode's Android port of it).

### Saves
- Export to a zip, import from a zip or folder (this app's exports and JKSV, Checkpoint, Ryujinx or yuzu dumps),
  automatic backups before every change, and an optional save folder that is kept in sync after every in-game save.
- Each slot shows its picture and the date and time the game saved it (as on the game's own load screen).

### Second screen
- On devices with a built-in second display (AYN Thor, Retroid Pocket Duo) a companion shows Link's status, the map
  and the inventory, with item actions: equip and unequip, eat and drink, take out materials and Zonai devices, fuse
  materials to arrows. The built-in panel is detected from the displays present, never from the chip; external
  monitors, cast and overlay displays are ignored.
- Every item shows its game name and description, also the ones the game names after another item (caught fish and
  insects, bundles, the opening's Master Sword and Champion's Leathers) instead of the internal actor name. Art made by
  an earlier version of the app is updated on its own when the menu opens; the game stays playable meanwhile.

### Game
- The game runs natively at 30 or 60 fps through the bundled, pinned Mesa Turnip Vulkan driver, with optional render
  resolution, FSR 1 / Snapdragon GSR upscaling, FXAA/SMAA, shadow quality and a performance overlay (**−** then **R**
  while playing).
- The bundled Turnip describes the Adreno 722 (Retroid Pocket Duo) with its two CCUs and two SPs (Mesa had one of
  each), which ends the burst of GPU page faults the Duo logged at every start of the game.
- Experimental and off by default: frame generation (locked, adaptive, LSFG with your own DLL) and the native HUD.
- The game's text prompts (naming a horse) open a dialog with the system keyboard: it starts with the game's text, applies
  the game's length limit and refused characters while you type, and OK / Cancel (or a gamepad's A / B, in the button
  layout chosen in Controls & audio) answer the game.
- Rumble and motion controls: the game's rumble plays on the controller's own motors (or the device's vibration motor),
  and motion aiming reads the controller's gyroscope (or the device's, following the screen's rotation). Controls & audio
  sets the rumble strength (off, 25–100 %) and turns motion controls on or off; a device without a vibrator or gyroscope
  does not offer that feature to the game.
- When the graphics driver loses the GPU (Vulkan device lost), the game still has to close, but the menu then opens
  with "The game stopped" and the reason instead of leaving you on the home screen without a word.
- `TOTK_ASPECT` (developer option, totk.env, e.g. `21:9` or `4:3`) shows the game's view at that ratio with black bars
  instead of stretching it over the screen, and the HUD and menus keep their proportions: in the middle 16:9 for a wider
  ratio, scaled down to the shown width for a narrower one.
- `TOTK_AUTOLOAD=1` in totk.env (or a test session's settings.env) loads the newest save again; the app used to read it
  before those files.

### Tested devices
- AYN Thor (QCS8550, Adreno 740, Android 13), AYN Odin 3 (CQ8725S, Adreno 830, Android 15), Retroid Pocket Duo
  (CQ7790S, Adreno 722, Android 16).

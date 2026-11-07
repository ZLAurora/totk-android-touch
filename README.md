<p align="center"><img src="docs/images/icon.png" width="128" alt="TotK Native icon"></p>

# TotK Native for Android

[![Support me on Ko-fi](https://img.shields.io/badge/Support_me_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/J3J518XVKR)

**TotK Native** (`com.aure.totk`) runs **The Legend of Zelda: Tears of the Kingdom 1.4.3** natively on Android
handhelds with a Qualcomm Adreno GPU. It is not an emulator: the game's own code is prepared once, on your device, to
run directly on its processor, and its graphics are drawn by a bundled Vulkan driver. The app contains **no game
files**: you point it at your own dump of the game, and it builds everything it needs on the device. Then a
controller-friendly menu lets you pick graphics presets, controls and saves before you play.

<p align="center">
  <img src="docs/images/home.webp" width="49%" alt="Home menu">
  <img src="docs/images/graphics-performance.webp" width="49%" alt="Graphics: Performance tab with presets">
  <img src="docs/images/setup-install.webp" width="49%" alt="Setup preparing the game">
  <img src="docs/images/set-render-res.webp" width="49%" alt="A setting with its illustration">
</p>

- [Features](#features)
- [Requirements](#requirements)
- [Install and first setup](#install-and-first-setup)
- [Performance](#performance)
- [Settings](#settings)
- [Saves and storage](#saves-and-storage)
- [Dual-screen companion](#dual-screen-companion)
- [Known limitations](#known-limitations)
- [FAQ and troubleshooting](#faq-and-troubleshooting)
- [For developers](#for-developers)
- [Credits](#credits) · [License](#license) · [Legal](#legal)

What changed in each version: [CHANGELOG.md](CHANGELOG.md). The full player guide, with every message and option:
[docs/android-install.md](docs/android-install.md).

## Features

- **Runs the game natively**, at 30 fps or 60 fps, on one APK for every supported device.
- **Setup on the device from your own files**, by either of two routes (a decrypted dump, or the game files and your
  keys). Every file is checked before anything is written; a wrong pick is refused on the spot with the reason. Setup
  shows each step's progress, keeps going with the screen off, and can be cancelled and resumed.
- **A pre-game menu** made for controllers: *Start game*, *Graphics*, *Controls & audio*, *Saves*, *Game data* and
  *About*. **+** starts the game from any screen.
- **Per-device profiles**: the first start picks the tested preset for your chip (AYN Odin 3, AYN Thor, Retroid Pocket
  Duo); every option can be changed, and **Y** brings back your device's defaults.
- **Graphics options**: presets, 30 / 60 fps, render resolution, FSR 1 and Snapdragon GSR upscaling, FXAA / SMAA, shadow
  quality, handheld or docked mode, a performance overlay. Experimental: frame generation and a native-resolution HUD.
- **Every option explains itself**: the menu shows what each one does, with an illustration, its GPU cost and what it
  works with.
- **Controls & audio**: Nintendo or Xbox button layout, rumble strength on the controller (or the device), motion aiming
  from the controller's or device's gyroscope, audio latency, the game's language. The game's text prompts (naming a
  horse) open Android's keyboard.
- **Saves**: export and import (also from JKSV, Checkpoint, Ryujinx or yuzu dumps), automatic backups, an optional
  save folder kept in sync (for example on the SD card or a cloud-synced folder).
- **Storage of your choice**: internal storage (fastest), the SD card, or a folder you choose; the game data can be
  moved later.
- **Dual-screen companion** on the AYN Thor and Retroid Pocket Duo: status, map and inventory on the second screen,
  with item actions.

## Requirements

- **Android 13 or newer**, 64-bit (arm64).
- **A Qualcomm Snapdragon / Dragonwing chip with an Adreno GPU.** The game draws through the Mesa **Turnip** Vulkan
  driver bundled in the app, and Turnip supports Adreno GPUs only. The devices' own Qualcomm drivers lack the
  memory-sharing features the game needs, so they cannot be used instead.
- **Storage**: about **40 GB free during setup** (the 17 GB game image plus your copied files), about **20 GB
  afterwards** (setup says it needs about 19 GB; your copied files can be deleted once setup is done).
- **Your own, legally dumped copy of the game, version 1.4.3**, made from a cartridge or eShop purchase you own, on
  your own Switch. Either route works:

  | Route | What you need |
  |---|---|
  | **RomFS + ExeFS dump** (no keys needed) | the 1.4.3 RomFS with the update applied, as one image file (`romfs.bin`) or its split parts, for example from nxdumptool or hactool; and the ExeFS folder with `main`, `sdk` and `subsdk0` |
  | **XCI / NSP + update + keys** | the base game 1.0.0 as XCI or NSP, the 1.4.3 update NSP, and the `prod.keys` of your own console (Lockpick_RCM). They are decrypted and merged on the device |

Tested devices:

| Device | Chip | GPU | Android | Second screen |
|---|---|---|---|---|
| AYN Thor | QCS8550 | Adreno 740 | 13 | built in, companion on |
| AYN Odin 3 | CQ8725S | Adreno 830 | 15 | none |
| Retroid Pocket Duo | CQ7790S | Adreno 722 | 16 | built in, companion on |

## Install and first setup

1. Download the latest `TotK-Native-<version>.apk` from the Releases page and install it (allow installs from your
   browser or file manager).
2. Copy your dump to the device or an SD card.
3. Open **TotK Native** and press **Begin setup**. Then:
   1. **Storage**: where the game data goes. Internal storage is recommended (fastest); the SD card or a folder you
      choose work too.
   2. **What you have**: pick your route.
   3. **Choose your files** with the file picker. Each file is checked as soon as you pick it; when everything is
      there, press **+** or *Continue*.
   4. **Updates**: choose whether to check GitHub for a newer app version when the game starts (On by default).
   5. **Prepare**: the game is built on the device. With a decrypted dump on internal storage this took about
      1 minute 45 seconds on an AYN Thor, 1 minute 40 seconds on an Odin 3 and 3 minutes on a Retroid Pocket Duo. The
      container route also merges the base game and update: about 12 minutes on a Thor with the files and the install
      on an exFAT SD card. The optional second-screen art is skipped on single-screen devices; on dual-screen devices
      you can start playing before it is done.
   6. **Choose graphics** opens with your device's preset selected. Press **+** to play.
4. Nothing is uploaded: your files stay on the device.

<p align="center">
  <img src="docs/images/setup-welcome.webp" width="32%" alt="Welcome">
  <img src="docs/images/setup-storage.webp" width="32%" alt="Storage choice">
  <img src="docs/images/setup-route.webp" width="32%" alt="Route choice">
  <img src="docs/images/setup-files.webp" width="32%" alt="Files chosen">
  <img src="docs/images/setup-install.webp" width="32%" alt="Preparing the game">
  <img src="docs/images/setup-graphics.webp" width="32%" alt="Choose graphics">
</p>

Step by step, with every message, the measured times of each route and moving from older test builds:
[docs/android-install.md](docs/android-install.md). Export your saves before uninstalling: uninstalling removes them.

## Performance

Measured with release 1.0.0 on each device's **default profile**, panning the camera in three places of the same save
set (Lookout Landing is the **inside** of the shelter next to Monte). Each number covers 3–7 windows of 300 frames.

| Device · profile | Place | Average fps | Lowest–highest window average | 95 % of frames took at most | Frames over 50 ms |
|---|---|---|---|---|---|
| AYN Thor · Balanced (30 fps) | Hateno Village | 30.0 | 30.0–30.1 | 38.9 ms | 1 of 1,200 |
| | Lookout Landing (indoors) | 30.0 | 30.0 | 39.9 ms | 0 of 1,200 |
| | Rotsumamu Shrine | 30.0 | 30.0 | 38.9 ms | 0 of 1,200 |
| AYN Odin 3 · 60 fps | Hateno Village | 42.1 | 33.9–55.5 | 37.6 ms | 1 of 2,100 |
| | Lookout Landing (indoors) | 50.9 | 45.2–55.8 | 35.1 ms | 1 of 1,800 |
| | Rotsumamu Shrine | 59.9 | 59.9 | 19.2 ms | 0 of 2,100 |
| Retroid Pocket Duo · Light (30 fps, 960 × 540) | Hateno Village | 28.2 | 27.6–29.0 | 46.9 ms | 22 of 900 |
| | Lookout Landing (indoors) | 29.0 | 28.4–29.9 | 49.5 ms | 34 of 1,200 |
| | Rotsumamu Shrine | 29.9 | 29.9 | 37.8 ms | 1 of 1,200 |

*Average fps* is the mean of the 300-frame windows; the next column gives the slowest and fastest window. At 30 fps a
steady frame takes 33.3 ms, at 60 fps 16.7 ms. These sessions had no GPU faults and no thermal throttling.

- **The Odin 3 was CPU-limited**: during this session its CPU clocks were capped at about 2.2 GHz (hardware maximum
  3.5 / 4.3 GHz) by a non-thermal limit (performance mode off, a ClusterTune tile present). Its GPU needed only 13–17 ms
  per frame in Hateno and Lookout Landing, so an uncapped Odin 3 should do better there.
- Loading on the AYN Thor: the title screen appears about 12.5 s after launch from internal storage (19–22 s from an SD
  card), a save loads in 14–16 s (17–23 s from the card); frame rates are the same from both.

Method, temperatures, GPU time and caveats: [docs/benchmarks/baseline-1.0.0.md](docs/benchmarks/baseline-1.0.0.md);
later runs: [docs/benchmarks/LEDGER.md](docs/benchmarks/LEDGER.md).

## Settings

Every option is in the menu, changes apply when the game starts, and each one shows its explanation and an
illustration when selected. On the menu: **A** changes, **◀ ▶** adjust, **L** / **R** switch tabs, **Y** restores the
current tab's defaults for your device, **B** goes back, **+** starts the game.

The first start picks your device's profile from its exact chip model (each Dragonwing part shares the profile of its
Snapdragon counterpart); the *Recommended for this device* badge marks it:

| Chips | Class | Tested on | Default preset |
|---|---|---|---|
| CQ8725S, SM8750 | Snapdragon 8 Elite (Adreno 830) | AYN Odin 3 | **60 fps** |
| QCS8550, QCM8550, SM8550 | Snapdragon 8 Gen 2 (Adreno 740) | AYN Thor | **Balanced** |
| CQ7790S, CQ7790M, SM7750 | Snapdragon 7 Gen 4 (Adreno 722) | Retroid Pocket Duo | **Light** |
| anything else | untested | — | **Light**, without the badge |

In the tables below, *Odin 3*, *Thor* and *Duo* stand for these chip classes and *Other* for every other device.
Experimental options (frame generation, the native HUD) are off in every profile.

<details>
<summary><b>Graphics → Performance</b>: presets, frame rate, frame generation, render resolution, latency</summary>

<img src="docs/images/graphics-performance.webp" width="640" alt="Performance tab with the presets">

**Presets** set the Performance and Picture options together (Screen and Controls are never touched; changing any
option shows *Custom*):

| Preset | What it is |
|---|---|
| Light | For lighter GPUs: the game runs at 30 fps and draws its 3D scene at 960 × 540 with full shadows. The coolest and longest-lasting choice. |
| Balanced | The game as it was made: 30 fps at the game's own resolution with full shadows. |
| Smooth | The game runs at 30 fps and frame generation adds an in-between picture, so the screen shows 60. The 3D scene is drawn at 1152 × 648 to leave room for it. (Experimental.) |
| 60 fps | The game itself runs at 60 fps at its own resolution. Needs a fast GPU; busy areas can dip below 60. |

#### Frame rate

How often the game itself updates. 30 fps is what the game was made for; 60 fps doubles the work for the processor and
the GPU, and busy areas may drop below it. 60 fps turns frame generation off.

Options: 30 fps, 60 fps · Default: **60 fps** on Odin 3, **30 fps** on Thor, Duo and Other.

<img src="docs/images/set-frame-rate.webp" width="640" alt="Frame rate">

#### Frame generation (experimental)

Draws an in-between picture from two real frames, so the screen updates 60 times a second while the game itself runs
at 30. Motion looks smoother; input feels about one frame later (about 33 ms). Needs 30 fps, and sets the latency.

Options: Off, Interpolation · Default: **Off** everywhere.

<img src="docs/images/set-frame-gen.webp" width="640" alt="Frame generation">

#### Render resolution

The size the game draws its 3D scene at before it is enlarged to fill the screen. Smaller is faster and softer. *Game
default* lets the game choose, as on the console.

Options: Game default, 1280 × 720, 1152 × 648, 960 × 540 · Default: **Game default** on Odin 3 and Thor, **960 × 540**
on Duo and Other.

<img src="docs/images/set-render-res.webp" width="640" alt="Render resolution">

#### Latency

How many finished pictures may wait for the screen. *Responsive* shows each one as soon as possible; *Smoothest* keeps
one more in reserve to hide uneven frames, at up to one more frame of input delay.

Options: Responsive, Balanced, Smoothest · Default: **Balanced** everywhere.

<img src="docs/images/set-latency.webp" width="640" alt="Latency">

</details>

<details>
<summary><b>Graphics → Picture</b>: upscaler, sharpness, anti-aliasing, shadows</summary>

#### Upscaler

How the picture is enlarged to the screen. *FSR 1* is AMD's edge-aware upscaler; *Snapdragon GSR* is Qualcomm's, made
for Adreno GPUs. *Off* uses plain filtering.

Options: Off, FSR 1, Snapdragon GSR · Default: **Off** everywhere.

<img src="docs/images/graphics-picture.webp" width="640" alt="Picture tab: Upscaler">

#### Sharpness

How much detail the upscaler restores after enlarging the picture. Too high makes edges shimmer. Needs an upscaler;
each upscaler keeps its own value.

Options: 0–100 in steps of 5 · Default: **20** for FSR 1, **50** for Snapdragon GSR, everywhere.

<img src="docs/images/set-sharpness.webp" width="640" alt="Sharpness">

#### Anti-aliasing

Smooths stair-stepped edges. *Game (FXAA)* is the game's own filter; *SMAA* finds more edges and keeps textures
sharper for a little more GPU time; *Off* is crispest but shows the steps.

Options: Game (FXAA), SMAA, Off · Default: **Game (FXAA)** everywhere.

<img src="docs/images/set-aa.webp" width="640" alt="Anti-aliasing">

#### Shadows

The resolution of the shadow maps. Lower saves GPU time where many shadows are on screen; their edges get softer and
blockier.

Options: High, Medium, Low · Default: **High** everywhere.

<img src="docs/images/set-shadows.webp" width="640" alt="Shadows">

</details>

<details>
<summary><b>Graphics → Screen</b>: console mode, native HUD, performance overlay, overlay position, second screen</summary>

#### Console mode

*Handheld* runs the game as on a Switch held in your hands (up to 1280 × 720). *Docked* makes it believe it is
connected to a TV: it draws at its larger docked resolution (up to 1920 × 1080) and needs more GPU time.

Options: Handheld, Docked · Default: **Handheld** everywhere.

<img src="docs/images/graphics-screen.webp" width="640" alt="Screen tab: Console mode">

#### Native HUD (experimental)

Draws the game's menus, map and text at the screen's own resolution instead of enlarging them from the game's size, so
text is sharper. Handheld only: docked mode always uses the game's HUD.

Options: Off, On · Default: **Off** everywhere.

<img src="docs/images/set-native-hud.webp" width="640" alt="Native HUD">

#### Performance overlay

A small readout in the corner of the screen. *FPS* shows the frame rate only; *Full* adds frame time, the render and
output resolution, the upscaler and a frame-time graph. While playing, press **−** and then **R** quickly to cycle Off →
FPS → Full until the game closes.

Options: Off, FPS, Full · Default: **Off** everywhere.

<img src="docs/images/set-overlay.webp" width="640" alt="Performance overlay">

#### Overlay position

Which corner of the screen the performance overlay sits in.

Options: Top left, Top right, Bottom left, Bottom right · Default: **Top right** everywhere.

<img src="docs/images/set-overlay-pos.webp" width="640" alt="Overlay position">

#### Second screen

On handhelds with a second display, shows the map, inventory and shortcuts on it while you play. Needs the
second-screen art prepared during setup. Only shown on devices with a built-in second display.

Options: Off, On · Default: **On** (where offered).

<img src="docs/images/set-second-screen.webp" width="640" alt="Second screen">

</details>

<details>
<summary><b>Controls & audio</b>: button layout, rumble, motion controls, audio latency, game language</summary>

#### Button layout

*Nintendo*: every button does what its label says, so A is A. *Xbox*: buttons act by position like on a Switch, so the
right-hand button is A even though it is labelled B. Choose Xbox for gamepads with Xbox labels.

Options: Nintendo, Xbox · Default: **Nintendo** everywhere.

<img src="docs/images/controls.webp" width="640" alt="Controls & audio: Button layout">

#### Rumble

The game's rumble on the controller's motors when it has its own, otherwise on this device's vibration motor. *Off*
gives the game no rumble at all.

Options: Off, 25 %, 50 %, 75 %, 100 % · Default: **100 %** everywhere.

<img src="docs/images/set-rumble.webp" width="640" alt="Rumble">

#### Motion controls

The game's motion controls (aiming the bow, some puzzles) from the controller's gyroscope when it has one, otherwise
from this device's. The game's own options can still turn motion aiming off.

Options: On, Off · Default: **On** everywhere.

<img src="docs/images/set-motion.webp" width="640" alt="Motion controls">

#### Audio latency

How much sound is queued before it plays. *Low* reacts fastest; *Safe* avoids crackling on busy systems and wireless
headphones.

Options: Low (10 ms), Balanced (20 ms), Safe (40 ms) · Default: **Balanced** everywhere.

<img src="docs/images/set-audio-latency.webp" width="640" alt="Audio latency">

#### Game language

The language of the game's text and voices. *Device language* follows this device's first language when the game has
it, and falls back to English otherwise.

Options: Device language, English, 日本語, Deutsch, Français, Español, Italiano, Nederlands, Русский, 한국어, 简体中文,
繁體中文, Português (Brasil) · Default: **Device language** everywhere.

<img src="docs/images/set-game-language.webp" width="640" alt="Game language">

</details>

<details>
<summary><b>Game data</b> and <b>About</b></summary>

*Game data* shows the installed game (verified against the 1.4.3 digests), where it is stored, and lets you rebuild the
shaders or the second-screen art, choose another graphics driver (a Turnip AdrenoTools `.zip` or 64-bit ARM `.so`, or
go back to the bundled one), run setup again (finished steps are kept) or delete the imported data (saves, settings and the driver are kept).

<img src="docs/images/game-data.webp" width="640" alt="Game data">

*About* shows the app and game versions, the build, the detected chip and screens, the licences of every bundled
component and the project's home.

<img src="docs/images/about.webp" width="640" alt="About">

</details>

**X** on the main menu turns on *Open the game directly next time*; to reach the menu again, long-press the app icon
and choose *Settings*.

## Privacy and app updates

When the game starts, TotK Native checks for a newer stable app release with **one tiny HEAD request to GitHub**,
only when Android reports a validated internet connection. It runs in the background, never delays the game, and
does not follow redirects or download a response body. GitHub sees the ordinary connection information (including
your IP address), but the app sends no identifier, settings, saves, keys or game data. There is no telemetry.
Offline, errors and timeouts are silent, with no retry during that game session.

The **Updates** setup step and **About → Check for updates when the game starts** switch turn checks off.
The default is On, including when updating an existing install. Development versions do not check.
If a new version is found, a controller-friendly dialog pauses the game: **Update now** closes it safely before
downloading; **Next time** remembers the version and asks in the menu next launch, even offline. Downloads need a
connection and can be cancelled. The app verifies the APK checksum, package, newer version and matching installed
signing certificate before Android asks you to allow this app as an install source and confirm installation.
Updating keeps your saves and game data; **do not uninstall** to update.


## Saves and storage

- **Saves** always stay in the app's private storage, wherever the game data is. *Saves* exports them to a zip and
  imports from a zip or a folder (this app's exports, JKSV, Checkpoint, Ryujinx or yuzu dumps), making a backup before
  every change (the newest 10 are kept). An optional **save folder** (for example on the SD card or a cloud-synced
  folder) gets a copy a few seconds after every in-game save; newer saves from the folder are brought in before the
  game starts, and if both sides changed you choose which to keep.
- **Uninstalling the app removes your saves**: export them first.
- **Game data** (about 19 GB) lives in internal storage (recommended, fastest), on the SD card, or in a folder you
  choose (that needs Android's optional *All files access*). *Game data → Storage* moves it later; the copy is checked
  before the old one is removed. FAT32 cards get the game image in parts automatically.

Details: [docs/android-install.md](docs/android-install.md#saves-export-import-backups-and-a-save-folder).

## Dual-screen companion

On the AYN Thor and the Retroid Pocket Duo, the second screen shows Link's status (hearts, stamina, battery, rupees,
equipment, sages and a minimap), the map with its three layers, and the inventory with every item under its in-game
name and description.

While you play, items can be used from the second screen: equip and unequip weapons, bows, shields and armor; eat food
and edible materials; drink elixirs; use fairies; take out a material or Zonai device (placed in front of Link); and,
with the bow drawn, fuse a material to your arrows. Each tap does what the game itself would do. Buttons are greyed out
with the reason when the game would not allow it: with the pause menu or map open, in cutscenes, while loading or
saving, on the title screen, with full hearts, and for the actions not available from the second screen yet
(energy-recovering materials and fusing Zonai devices to arrows).

Only a display built into the device is used; external monitors and cast displays are ignored. Turn it off in
*Graphics → Screen → Second screen*. More: [docs/companion.md](docs/companion.md).

Setup prepares the art only when a built-in second screen is present. If it is missing on a dual-screen device,
**Start game** offers **Prepare second-screen art** (build it, then play) or **Play without it** (remembered for this
install). You can prepare it later in **Game data → Second-screen art**, even on a single-screen device.

## Known limitations

- Game version **1.4.3** only, from the two setup routes above. No DLC, online features or amiibo.
- Adreno GPUs only.
- Devices other than the three tested ones start on the Light preset and are untested.
- Shaders the shared list does not name yet are translated while playing, which can cause a short stutter the first
  time a new place is seen.
- Frame generation and the native HUD are experimental; frame generation is limited by the GPU headroom in heavy
  scenes on the Thor.
- From the second screen, energy-recovering materials and fusing Zonai devices to arrows are not available yet.
- Saves are removed with the app: export them first. Test builds signed with another key cannot be updated in place by
  a release build.

## FAQ and troubleshooting

**Setup says "A file does not match Tears of the Kingdom 1.4.3".** The dump is incomplete or another version: use the
1.4.3 RomFS with the update applied (all split parts) or the 1.4.3 update NSP, then choose it again. Every check and
message: [docs/android-install.md](docs/android-install.md).

**Setup was interrupted or stopped.** Finished steps are kept; *Resume* continues setup. On **Setup stopped**, use
**Share details** **before Resume** to include that failed run's logs in a bug report (Resume clears them).
It opens Android's share sheet with the error, device/build information and bounded, redacted setup logs.
No game files or keys are included; nothing is sent until you choose an app. Missing logs are marked explicitly.

**Home shows "Shaders not built".** The game would draw nothing without its shaders: press *Build shaders* (a few
minutes), then start the game.

**"Game data is on SD card, which isn't available".** Insert the card; the menu updates as soon as it is mounted. Or set
the game up again on another storage.

**The game stutters the first time I enter a new place.** A shader is being translated; it happens once per new
shader. See [docs/shader-list.md](docs/shader-list.md).

**Sound crackles.** Set *Controls & audio → Audio latency* to *Safe*. Missing dialogue or music: see
[docs/audio-troubleshooting.md](docs/audio-troubleshooting.md).

**The menu says "The game stopped".** The graphics driver lost the GPU or the game hit a fatal error. Your saves are
kept; progress since the last save is lost. The details are in `totk.log`; please include it in a bug report.

**The game opens directly, without the menu.** Long-press the app icon and choose *Settings*, then turn off *Open the
game directly next time* with **X**.

**Can I use my phone's own GPU driver or another Turnip build?** Another Turnip build: *Game data → Graphics driver*.
Pick its AdrenoTools `.zip` as downloaded, or the 64-bit ARM `.so` inside it; *Use the bundled driver* resets it.
The system driver does not work (see [Requirements](#requirements)).

## For developers

- [docs/architecture.md](docs/architecture.md): how the Android port works.
- [docs/building.md](docs/building.md): building the APK from source and running the tests.
- [docs/developer-settings.md](docs/developer-settings.md): what the menu writes, and `totk.env` developer switches.
- [docs/companion.md](docs/companion.md): the dual-screen companion in detail.
- [docs/benchmarks/](docs/benchmarks/baseline-1.0.0.md): measuring performance against the recorded baselines.
- [games/totk/android/README.md](games/totk/android/README.md), [APP.md](games/totk/android/APP.md),
  [VULKAN.md](games/totk/android/VULKAN.md), [DEBUGGING.md](games/totk/android/DEBUGGING.md): the full developer notes.

## Credits

This project builds on the work of others:

- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** by ly05010419 and its contributors (MIT): the
  native Tears of the Kingdom port for Apple silicon this project is derived from (runtime, Horizon host, rewriter
  integration, shared shader list, tests).
- **[kitcatkode/MacNativeGames](https://github.com/kitcatkode/MacNativeGames)**: the Android port of MacNativeGames
  (Linux / Android runtime, Vulkan backend, first APK) that this Android project started from.
- **[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT): the way of running Switch games natively, the NSO
  rewriter (`games/totk/third_party/d2r`), the Horizon host and the NVN graphics layer (`games/totk/third_party/d2r_gfx`)
  started as its generic parts; the translator front end too.
- **Ryujinx** (MIT): the shader translator (`vendor/ryujinx`: Ryujinx.Graphics.Shader,
  Ryujinx.Common, Spv.Generator) and the audio effect reference.
- **[Mesa](https://mesa3d.org) Turnip** (MIT): the bundled Vulkan driver for Adreno GPUs.
- **hactool** (ISC, SciresM) with mbedTLS, cJSON and LZ4; **Capstone** (BSD-3-Clause); **zstd** (BSD-3-Clause);
  **libwebp** (BSD-3-Clause); **libopus** and **libvpx** (BSD-3-Clause); **Noto Sans** (SIL OFL 1.1).
- **lsfg-vk-android** (MIT), **SMAA** (MIT), **AMD FidelityFX FSR 1** (MIT), **Snapdragon GSR** (BSD-3-Clause),
  **deko3d** (zlib), **bcdec**, the NVN API header by h1k421, **nxnative**, **nx2android**, **LASRecomp-Android-Plus**,
  **TOTKRecompAndroid**, texture decoders from texture2ddecoder, AssetStudio, mikunyan, bimg and Pillow, the .NET
  runtime (NativeAOT), SPIRV-Cross and LLVM libc++.

Every bundled component, its version and its licence are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
and shown in the app under *About → Open-source licences*. libvpx and libwebp also carry Google's patent grants
(PATENTS files, included there).

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

**You must own the game.** Use this project only with a copy of The Legend of Zelda: Tears of the Kingdom that you
bought, and with keys and system files dumped from your own Nintendo Switch. This is an independent, non-commercial
project. It is not affiliated with, endorsed by, or sponsored by Nintendo. The Legend of Zelda and Nintendo Switch are
trademarks of Nintendo.

The repository and the APK contain no game content and distribute none: no game program, art, text, audio, shaders or
keys, and they do not help to obtain them. Everything the game needs is made on your own device from your own files.
What the repository does hold is the information needed to make the port work: the patch code, the addresses and hashes
used to check your files, and the [shared shader list](docs/shader-list.md), which names shaders and bind states but
contains no shader code. The game data the app builds is for your own use only; do not share it.

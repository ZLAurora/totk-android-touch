# Installing TotK Native on Android

TotK Native runs **The Legend of Zelda: Tears of the Kingdom 1.4.3** natively on arm64 Android devices with a
Qualcomm Adreno GPU. Tested on the AYN Thor (QCS8550, Adreno 740), the AYN Odin 3 (CQ8725S, Adreno 830) and the Retroid
Pocket Duo (CQ7790S, Adreno 722). It is one APK for everyone. The APK contains **no Nintendo files**: no game code, art,
text, fonts or shaders. On first start it asks for files from **your own copy** of the game and builds everything it
needs on the device.

## What you need

- Android 13 (API 33) or newer, arm64, Adreno GPU. Adreno only, because the game draws through the bundled Mesa
  **Turnip** Vulkan driver, which supports Adreno GPUs only; the devices' own Qualcomm drivers lack the memory-sharing
  extensions the game needs (`VK_EXT_external_memory_host` / dma-buf), so they cannot run it.
- Free storage on the device: **about 40 GB during setup** (the 17 GB RomFS image plus the inputs you copy over), about
  **20 GB afterwards** (you may delete your copied inputs once setup says *Ready*). Inputs can also be on an SD card.
- Your own dump of the game, made from a cartridge or eShop purchase you own, on your own Switch. Pick **one** of the two
  routes below.

### Route A — decrypted files (no keys needed)

1. The **RomFS of 1.4.3 with the update applied**, as one raw image file (or its ordered split parts), for example
   nxdumptool's *Patch RomFS* "raw" dump, or `hactool --section1` of the update merged with the base.
2. The **ExeFS of 1.4.3**: the files `main`, `sdk` and `subsdk0` (or the raw ExeFS / PFS0 image).

### Route B — containers

1. The base game **1.0.0** as XCI or NSP.
2. The **1.4.3 update** NSP.
3. Your console's `prod.keys` (dumped with Lockpick_RCM). Setup reads only the keys it needs. While decrypting, it keeps
   them in a private temporary file (app-only, excluded from backups) that is deleted when the step ends, or, if the app
   was killed in the middle, when setup next starts. Keys are never shown or logged.

What to expect on the container route (measured on an AYN Thor, with the three files and the install both on an exFAT SD
card, base and update as NSP; XCI was not tested on a device):

- The files are checked as soon as they are picked. A good set reads *Tears of the Kingdom base game (NSP)*, *Tears of
  the Kingdom update (NSP; version checked during setup)* and *prod.keys (header key, N key generations)*. Wrong picks
  are refused on the spot, for example *update: expected a BKTR update, not a base game* (base picked as the update),
  *base game: this is the update, not the base game 1.0.0*, *update: it belongs to another game*, *base game: it
  belongs to another game, not Tears of the Kingdom*, *prod.keys lacks titlekek_0e (dump the keys again on a newer
  firmware)* and *prod.keys has no header_key (dump the keys again with Lockpick_RCM)*.
- Game code (ExeFS extracted from the update, then rewritten): about 20 s.
- Game data, the base + update merge: about 12 minutes. For the first ~3 minutes the bar stays at 0 B while the update
  is checked; after that the merged image is written at about 30 MB/s. Index and fonts take about 20 s more, the
  second-screen art about 30 s.
- Extra space during setup: a temporary copy of the update's program (about 0.5 GB) next to the game data.
- If the app is killed during the merge, the next start shows *Setup was interrupted*. *Resume* keeps the finished
  steps (game code), and the merge starts again from the beginning; any partly written image is discarded.
- Once setup says the game is ready, the base game, update and `prod.keys` are no longer needed and can be deleted.

Every input is checked against the pinned 1.4.3 hashes (`games/totk/assets/expected-v1.4.3.json`); anything else is
refused with an explanation. Other versions are not supported.

## Install

1. Download `TotK-Native-<version>.apk` from the project's Releases page and install it (allow installs from your browser or file
   manager). It installs side by side with older test builds.
2. Open **TotK Native**. The setup screen asks for the files of your route. Pick them with the file picker; nothing is
   uploaded anywhere.
   Before Prepare, the **Updates** step lets you turn off **Check for updates when the game starts** (On by default).
3. Setup runs in the foreground (a notification shows progress; keep the device charged). It rewrites the game code for
   Android, copies and verifies the RomFS (reading, hashing and writing overlap, so the copy runs at the storage's
   speed), indexes it, writes the shared fonts from the bundled open Noto Sans font, then builds the Vulkan shader seed
   and, on devices with a built-in second screen, the companion's drawings at the same time (each shows its own progress).
   Without a built-in second screen, the drawings are marked *skipped · Not needed on this device*. The game needs the shader
   seed (without it nothing is drawn), so setup is finished only once it is built; if it fails, setup stops with the
   reason and *Resume*. Only the companion's drawings are optional: once the seed is done, *Play without second-screen
   art* stops that step and starts the game (build it later from *Game data*). Setup can be cancelled and resumed.
4. When setup finishes it opens **Choose graphics** with your device's profile selected (see below); press **+** (or
   tap *Start game*) to play. Setup has already verified the RomFS, so the first start does not check it again.

### Graphics profiles

The first start, and **Y** (defaults) on the Performance and Picture tabs, use the profile of your chip, read from
Android's `SOC_MODEL` (the whole part number, never a guess from similar digits). Each Dragonwing part shares the
profile of its Snapdragon counterpart:

| Chips | Class | Tested on | Profile |
|---|---|---|---|
| CQ8725S, SM8750 | Snapdragon 8 Elite (Adreno 830) | AYN Odin 3 | 60 fps at the game's resolution, hardware GPU timing |
| QCS8550, QCM8550, SM8550 | Snapdragon 8 Gen 2 (Adreno 740) | AYN Thor | Balanced: 30 fps at the game's resolution |
| CQ7790S, CQ7790M, SM7750 | Snapdragon 7 Gen 4 (Adreno 722) | Retroid Pocket Duo | Light: 30 fps, 3D scene at 960 × 540, full shadows |
| anything else | untested | — | Light, without the *Recommended* badge |

Frame generation, adaptive frame generation, LSFG and the native HUD are experimental and off in every profile; you
can still turn them on. The Performance tab and *About* show what was detected, for example *Snapdragon 8 Gen 2 class
(QCS8550) · dual screen*.

## The menu

Every launch opens the menu: *Start game*, *Graphics* (Performance, Picture and Screen tabs with presets), *Controls &
audio*, *Game data* (rebuild the shaders or second-screen art, graphics driver, delete the imported data) and *About*
(version, licences). **+** starts the game from any menu screen, **B** goes back, **Y** restores the current tab's
defaults, **L**/**R** switch tabs. Changes apply when the game starts. **X** on the main menu toggles *Open the game
directly next time*; long-press the app icon and choose *Settings* to reach the menu again. While playing, press
**−** and then **R** quickly to cycle the performance overlay (off → FPS → full) for that session. An install without a
usable shader seed (an earlier version's *Play now* could skip it, or its files were removed) shows *Shaders not built*
and offers *Build shaders* in place of *Start game*; the game is not started without the seed.

## Versions and updates

App versions and the supported game version are separate: app 1.0.1 still runs your game dump 1.4.3.
Install a newer APK **over the existing app**, never uninstall first: saves, imported game data and settings survive.
Only an APK for the same package signed with the same key can update it.

By default the game checks GitHub's latest stable release once at startup, in the background, only on a validated
internet connection. It sends no personal or game information, reads no response body, and never delays launch.
Disable it in **About → Check for updates when the game starts**, or in setup's **Updates** step.
**Update now** pauses the game and uses the menu's save-safe close before downloading into the app's cache.
**Next time** keeps the version and offers **Update / Not now** before Home on the next launch, including offline.
The download needs a connection; Cancel or an error leaves the installed app unchanged and keeps the pending version.
If the download takes more than ten minutes, or transfers less than 64 KiB in 30 seconds, it stops with a clear
message and removes partial files. The pending update is kept so you can try again on a faster connection.

The download is checked against the release's `.sha256`, then checked for the app's package, a newer version code and
the installed app's signing certificate. Android may ask you to enable **Allow from this source** for TotK Native;
return to the app afterwards to continue. Denying it returns to Home and keeps the update for next time. Android's
installer still asks for confirmation. Downloaded files are removed afterwards. The system may close the app when
the update finishes; open it again normally.


## The second screen (dual-screen devices)

On a device with a built-in second display (AYN Thor, Retroid Pocket Duo), the second screen shows Link's status, a map
and the inventory. The app looks for that panel among the displays present, never from the chip: a display counts as
built in when Android reports it as built in, or as an unknown connection with the same panel maker's id as the main
screen. External monitors (HDMI, DisplayPort, USB-C), cast and screen-recording displays and the developer *Simulate
secondary displays* overlay are ignored; the Screen tab then says *External display · companion off* and has no
*Second screen* option. From the inventory you can, while playing (no game menu open), equip or unequip weapons, bows,
shields and armor; eat food and edible materials; drink elixirs; use fairies; take out a material or Zonai device (it
is placed in front of Link, as the game's own shortcut menu does); and, with your bow drawn, fuse a material to your
arrows. Each tap does exactly what the game itself would do. Buttons are greyed out with the reason when the game would
not allow it: with the pause menu or map open ("Close the game menu first"), in cutscenes and conversations, while
loading or saving, on the title screen, with full hearts (for food that only heals), or for actions not available from
the second screen yet (energy-recovering materials, fusing Zonai devices to arrows). More: [companion.md](companion.md).

If the art is missing on a dual-screen device, **Start game** offers **Prepare second-screen art** (with progress, then
the game starts) or **Play without it** (remembered for this install, including after app restarts). You can build it
later from **Game data → Second-screen art**, even on a single-screen device. Outdated art is updated automatically
only on dual-screen devices; single-screen devices get no art rebuild reminder.

## Where the game data lives (internal storage, SD card or a folder you choose)

The game data (about 19 GB) can be kept in three kinds of places; setup's first step asks, and *Game data → Storage*
moves it later:

- **Internal storage** (recommended): the app's own private storage. It is the fastest place, other apps cannot see
  or change it, and nothing (no MTP, no media scanner) sits between the game and the files.
- **SD card**: the app's folder on the card (`Android/data/com.aure.totk/files`). It frees room on the device and
  needs no extra permission.
- **A folder you choose**, on internal storage or the card (for example `TotK Native` at the top of the card). This
  needs *All files access* (see below). If you pick a folder that already holds other files, the data goes into a
  `TotK Native` folder inside it.

Moving copies everything to the new place, checks the copy again there, and only then removes the old copy. Measured
on an AYN Thor: about 1 minute 10 seconds from the old internal location into internal storage, about 11 minutes onto
the exFAT card, about 5 minutes back. If the app is closed or killed during a move, the next start says so and leaves
exactly one copy: the one the game uses; you can start the move again.

**Earlier test builds (`com.bernalr.totk`).** The app id is `com.aure.totk`. Earlier test builds were installed as
`com.bernalr.totk`; Android treats that as a separate app, so this APK installs next to it (both show *TotK Native*)
and cannot read its game data, settings or saves. Move your saves yourself: in the old app *Saves → Export saves*
(to Download), then in the new app *Saves → Import saves* → *A zip file* and pick that zip. Run setup in the new app
from your dump as on a first install. Uninstall the old app once you no longer need it; that deletes its game data
and saves.

How the places compare (AYN Thor, measured as the app's user with the page cache dropped; small files are 20,000
files of 4–64 KB):

| Place | Write 4 GB | Read 4 GB | Create 20,000 small files | Delete them | Media storage service CPU |
| --- | --- | --- | --- | --- | --- |
| Internal storage (app's own) | 2,060 MB/s | 2,355 MB/s | 1.4 s | 0.3 s | none |
| Internal `Android/data` (older versions) | 582 MB/s | 2,361 MB/s | 2.8 s | 0.4 s | none |
| Folder on internal storage (All files access) | 502 MB/s | 2,350 MB/s | 323 s | 101 s | 423 s |
| SD card `Android/data` | 46 MB/s | 90 MB/s | 1,007 s | 325 s | 707 s |
| Folder on the SD card (All files access) | 46 MB/s | 90 MB/s | 841 s | 242 s | 561 s |

So internal storage is the default; on a card, the card's app folder and a folder you choose are equally fast, and the
app folder needs no permission. Earlier measurements with the same save: the title screen appears after about 12.5 s
from internal storage versus 19–22 s from the card, and loading the save takes 14–16 s versus 17–23 s. Reloading in
the game (about 11 s) and frame times (p50 ≈ 33.4 ms at 30 fps) are the same on both.

If the game data is on a card that is not inserted (or not mounted), the menu says *Game data is on <card>, which
isn't available* and *Start game* does nothing. Insert the card again (the menu updates as soon as it is mounted), or
set the game up again on another storage.
After the card comes back, the game does **not** check all 17 GB again: it compares the files' sizes, dates and
sampled pieces of their content with the check made when they were written. This is a quick check at start-up, not
a full integrity check; any difference means a full check.

Cards formatted as **FAT32** (most cards of 32 GB or less) cannot hold a single file larger than 4 GB. On such a card
the 17 GB game image is stored as four parts (`romfs.bin.000` …) automatically; exFAT cards (64 GB and larger) keep
one file. Uninstalling the app removes the game data in internal storage and in the card's app folder (Android asks
first); a folder you chose is left as it is. The game data in internal storage is never part of a backup or a
transfer to a new phone (it is rebuilt from your own files); saves are transferred as before.

### All files access (optional)

TotK Native can use Android's *All files access* ("Allow access to manage all files"), like Azahar and other
emulators distributed outside Google Play. It is optional: setup's Storage step and *Game data → Storage* show
*All files access* with a button that opens Android's settings page for it. With it:

- the files you pick for setup (the dump, keys, containers) are read directly by their path instead of through the
  file picker, so large files can be read in parallel and nothing is copied;
- the game data can live in a folder you choose;
- saves are exported, imported and copied to your save folder directly, without the picker's provider.

Without it everything works as before through the file picker; the app does not ask again. If you turn it off while
the game data is in a folder you chose, the menu says *All files access is off* and *Start game* becomes *Allow All
files access to continue*, which opens the settings page; the game itself also refuses to start from that folder. Or
move the game data back to internal storage or the card's app folder after allowing it once more.

## Saves: export, import, backups and a save folder

Saves always stay in the app's private storage, wherever the game data is. *Saves* on the main menu shows your slots
(with their pictures) and offers:

- **Export saves**: a zip (`TotK Native saves <date>.zip`) with all slots, written where you choose.
- **Import saves**: from a zip or a folder, for example an export of this app or a dump made with JKSV, Checkpoint,
  Ryujinx or yuzu (folders with `slot_00` … `slot_05` and `option.sav`, also inside a wrapper folder). The app shows
  what it found before anything changes. Importing replaces the whole save set; your current saves are backed up
  first. Import and restore are refused while the game is running.
- **Backups**: made automatically before every import, restore and save-folder update (the newest 10 are kept). You
  can restore or delete each one; restoring also backs up the current saves first.
- **Save folder**: pick a folder (for example on the SD card, or a synced cloud folder). The game keeps saving
  privately, and the app copies your saves into `TotK Native saves/` in that folder a few seconds after every in-game
  save, when you leave the game, and when the menu opens. Each copy is complete before it counts: an interrupted copy
  is ignored and the previous one stays. When the app starts and the folder holds saves that changed since the last
  sync while your saves on the device did not, it brings the folder's saves in (backing up yours first) and shows
  *Saves updated from your save folder*. If **both** changed since the last sync, it asks which side to keep; the
  other side is kept as a backup. If the folder is missing or access was taken away, the menu shows *Save folder
  unavailable* and nothing is deleted on either side.

Saves live in the app's private storage. Uninstalling the app removes them: export them first.

## Drivers

The APK bundles a pinned Mesa Turnip build (MIT; how to rebuild it: `games/totk/android/turnip/README.md`). To try your
own Turnip build, open the app's *Game data → Graphics driver* and pick its AdrenoTools `.zip` as downloaded, or its
64-bit ARM `.so` (you can also copy the library as `driver.so` into the game folder). The zip's `meta.json` selects
`libraryName` and supplies the displayed name; without metadata, a zip must contain exactly one `.so`.
Only that library is installed, and a refused import leaves your current driver unchanged. Libraries are limited to
128 MiB, metadata to 64 KiB and archives to 256 entries / 160 MiB. *Use the bundled driver* goes back to the bundled one.
`TOTK_VK_DRIVER=system` in `totk.env` there selects the system driver instead; no current Qualcomm system driver
passes the game's startup check, so that is for testing only.

## Versions

The app has its own version (e.g. 1.0.1, semantic versioning); the game version it runs (1.4.3) is shown separately. The
Android version code grows with it: major × 10000 + minor × 100 + patch (1.0.0 is 10000, 1.2.3 would be 10203), so an
update always installs over the previous release. Release APKs are signed with one release key (certificate SHA-256
`a9e7a2a9fad6fdaede18c45a458e4555c0904658c9d7f4b74a73d9c83b67e2bf`); test builds signed with other keys cannot be
updated in place, so export your saves, uninstall, install the release and import them.

## Legal

This project does not include or distribute any part of the game or of the Switch firmware, and does not help to obtain
them. Use only files dumped from hardware and software you own. "Nintendo", "Nintendo Switch" and "The Legend of Zelda"
are trademarks of Nintendo; this project is not affiliated with or endorsed by Nintendo. The licences of every component
in the APK are shown in the app and listed in `THIRD_PARTY_NOTICES.md`.

## Building the APK yourself

Players do not need to. Developers: [building.md](building.md) (no game files; details in
[games/totk/android/README.md](../games/totk/android/README.md)).

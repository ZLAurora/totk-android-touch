# The dual-screen companion

On handhelds with a built-in second display (AYN Thor, Retroid Pocket Duo), TotK Native shows a companion on that
display while you play: Link's status, the map and the inventory. This page is the player-facing reference; the
implementation (snapshot format, polling, the native action checks and the device evidence for each action) is in
[games/totk/android/APP.md, *Dual-screen companion*](../games/totk/android/APP.md#dual-screen-companion).

## What it shows

- **Adventure**: hearts (in quarters, with gloom and extra hearts), stamina and battery capacity, rupees, the equipped
  weapon, bow, shield and armor, the sages and a minimap. Tapping an equipped item opens it in its inventory category.
- **Inventory**: all eight categories (weapons, bows & arrows, shields, armor, materials, food, Zonai devices, key
  items). Every item is shown under its in-game name and description, also the ones the game names after another item
  (caught fish and insects, bundles, the opening's Master Sword and Champion's Leathers). Tap an item for its details
  and actions; swipe sideways for categories, drag to scroll.
- **Map**: three layers (sky, surface, depths). Drag and pinch to move and zoom, pick a layer by hand; *Recenter* goes
  back to following Link.

The pictures (map, icons, item art) are made on your device from your own game data during setup (*Second-screen art*)
and can be rebuilt from *Game data*. They are optional: the game runs without them. Art made by an earlier version of
the app is updated on its own when the menu opens; the game stays playable meanwhile.

## Item actions

While playing (no game menu open), the selected item offers the actions the game itself would allow:

| Items | Actions |
|---|---|
| Weapons, bows, shields, armor | Equip, unequip |
| Food and edible materials | Eat (when it would have an effect, e.g. not with full hearts for food that only heals) |
| Elixirs | Drink |
| Fairies | Use |
| Materials and Zonai devices | Take out (placed in front of Link, as the game's own shortcut does) |
| Materials, with the bow drawn | Fuse to arrows |

Each tap does exactly what the game would do, and the result is read back from the game. A button is greyed out with
the reason when the game would not allow the action, for example:

- *Close the game menu first* (pause menu or map open), *In a cutscene*, *Wait for loading to finish*, *The game is
  saving*, *Load a save first* (title screen), *Link isn't in the world right now*;
- *Your hearts are already full*, *This can't be eaten*, *None left*, *No arrows left*, *Aim your bow to fuse*,
  *Weapons can't be changed right now*;
- *Not available from the companion yet*: for actions not yet proven on a device. In 1.0.1 these are **energy-recovering
  materials** (recharging the battery) and **fusing Zonai devices to arrows**.

Only one action runs at a time; a second tap while one is pending is ignored. The pause menu's *Hold* is not offered.
The optional minus / plus shortcuts only press the game's map / menu buttons.

## Which display is used

On dual-screen devices the game always opens on the main (top) screen, even when you start it from the second
screen's launcher or menu. With **Second screen** on and its art prepared, the companion appears on the second
(bottom) screen. Turning the companion off does not move the game to that screen. Single-screen devices are unchanged.

The companion uses a display that is built into the device, found among the displays present, never guessed from the
chip: a display counts as built in when Android reports it as built in, or as an unknown connection with the same panel
maker's id as the main screen. External monitors (HDMI, DisplayPort, USB-C), cast and screen-recording displays and
the developer *Simulate secondary displays* overlay are ignored; the Screen tab then says *External display ·
companion off* and has no *Second screen* option. The companion is shown on one secondary display only (never the
game's), and only while the game is in front: Home dismisses it and returning shows it again.

*Graphics → Screen → Second screen* turns it off for play. Developers can also disable it with `TOTK_COMPANION=0` in
`totk.env` ([developer settings](developer-settings.md)).

Android sends controller buttons to the screen that was touched last. When you touch the companion, or an overlay
shown on the second screen (such as AYN's performance overlay), the game takes the controller back: right after a
companion touch, and about 0.3 s after anything else took it. If a window on the second screen keeps taking it (for
example the keyboard of another app shown there), the game stops after three quick tries until you touch the
companion again. Apps cannot see other apps' windows, so such a window can lose the controller up to three times first.

## Preparing the pictures

Setup builds the optional second-screen art alongside shaders only on devices with a built-in second screen.
On single-screen devices, including phones and the Odin 3, the stage is recorded as **skipped**, with **Not needed
on this device**; no art is generated. External and cast displays do not make the art necessary.

If a built-in second screen is present when you press **Start game**, but the art is missing (skipped, never built,
failed, or deleted), you can **Prepare second-screen art** before playing or **Play without it** immediately.
Preparing shows the normal stage progress and starts the game only after the art is ready; a failure or cancellation
returns to Home. **Play without it** is remembered across launches for this install, so you are not asked again.
**Game data → Second-screen art** still builds it on demand on any device, and clears that choice. Deleting imported
data also clears it.

On a Retroid Pocket Duo with the imported game on internal storage, preparing the art alone took **12.8 seconds**
(about 15 seconds; warm filesystem cache, measured from the choice to the committed art on 2026-11-04), producing
35.3 MB. Other devices and slower storage may take longer.

On dual-screen devices, old-but-present art is updated automatically when Home opens, as before; **Play now** can stop
the update and use the old art. On single-screen devices there is no automatic art update or rebuild reminder.


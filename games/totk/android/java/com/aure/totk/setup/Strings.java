package com.aure.totk.setup;

import java.util.Locale;

/** Every user-visible menu and setup text, in one place so a translation only has to replace this class.
 * The APK has no resource table (it is built without aapt2), so strings cannot live in res/values. */
final class Strings {
    private Strings() { }

    static final String APP = "TotK Native";
    static final String SUBTITLE = "UNOFFICIAL ANDROID PORT";
    static final String LEGAL = "You must own the game. Your files stay on this device; nothing is uploaded.";
    /** The project's public home; one constant so the release can point it elsewhere. */
    static final String PROJECT_URL = "https://github.com/AurelioB/totk-android";
    /** The projects this one is built on: MacNativeGames by ly05010419, and kitcatkode's Android port of it. */
    static final String UPSTREAM_URL = "https://github.com/ly05010419/MacNativeGames";
    static final String ANDROID_PORT_URL = "https://github.com/kitcatkode/MacNativeGames";

    static final String UPDATES = "Updates";
    static final String UPDATE_CHECK = "Check for updates when the game starts";
    static final String UPDATE_PRIVACY = "Only asks GitHub for the latest version number. Nothing about you or your game is sent. You choose when to install.";
    static final String UPDATE_NOW = "Update now", UPDATE_NEXT_TIME = "Next time";
    static final String UPDATE_CLOSE_HINT = "Close the game safely, then download and install";
    static final String UPDATE_NEXT_HINT = "Ask in the menu next launch, even offline";
    static final String UPDATE_GAME_BODY = "The game is paused. Your saves and game data stay on this device.";
    static final String UPDATE_MENU_BODY = "Download and install the new version? Your saves and game data will be kept. A network connection is needed to download.";
    static final String UPDATE_ALLOW = "Allow app updates";
    static final String UPDATE_ALLOW_BODY = "Android needs permission for TotK Native to install its own update. Enable “Allow from this source”, then return here to continue. You will still confirm the installation.";
    static String updateAvailable(String version) { return "TotK Native " + version + " is available"; }
    // Button prompts
    static final String P_SELECT = "Select", P_QUIT = "Quit", P_START = "Start game", P_DEFAULTS = "Defaults", P_CHANGE = "Change";
    static final String P_ADJUST = "Adjust", P_BACK = "Back", P_CHOOSE = "Choose", P_INSTALL = "Install", P_CANCEL = "Cancel";
    static final String P_CANCEL_AGAIN = "Press again to cancel", P_PLAY_NOW = "Play now", P_OTHER_PICK = "Other way", P_OPEN = "Open";
    static final String P_PLAY_WITHOUT_ART = "Play without second-screen art";

    // Home
    static final String MAIN_MENU = "MAIN MENU";
    static final String START_GAME = "Start game";
    static final String START_HINT = "Continue from your saves";
    static final String GRAPHICS = "Graphics";
    static final String CONTROLS = "Controls & audio";
    static final String GAME_DATA = "Game data";
    static final String ABOUT = "About";
    static final String ABOUT_HINT = "Version, open-source licences";
    static final String SKIP_MENU = "Open the game directly next time";
    static final String INSTALLED_CHIP = "Game 1.4.3 installed";
    static final String SHADERS_NONE = "Shaders not built";
    // An install without a usable shader seed: the game would draw nothing, so Home offers Build shaders instead.
    static final String BUILD_SHADERS = "Build shaders";
    static final String BUILD_SHADERS_HINT = "Needed to start the game · takes a few minutes";
    static final String SEED_NEEDED = "The game draws nothing without its shader seed, which was never built or did not finish. "
            + "Build shaders to finish setup.";
    static final String SEED_UNUSABLE = "The game was not started: its shader seed is missing or empty, so it would show a black "
            + "screen. Build shaders, then start the game.";
    static final String SECOND_READY = "Second screen ready";
    static final String DRIVER_BUNDLED_SHORT = "Bundled Turnip driver";
    static final String DRIVER_CUSTOM_SHORT = "Custom driver · ";
    static String shadersReady(int count) { return String.format(Locale.US, "%,d shaders ready", count); }
    static String dataSummary(String size, boolean secondScreen, boolean companion, boolean artOutdated, boolean shaders) {
        return size + " · verified · " + (!shaders ? "shaders not built" : !secondScreen ? "rebuild shaders"
                : companion ? "rebuild shaders or companion art" : artOutdated ? "second-screen art out of date" : "second-screen art not prepared");
    }

    // Graphics
    static final String TAB_PERFORMANCE = "Performance", TAB_PICTURE = "Picture", TAB_SCREEN = "Screen";
    static final String IN_USE = "IN USE";
    static final String RECOMMENDED = "Recommended for this device";
    static final String UNTESTED_DEFAULT = "Default for untested devices: the lightest preset";
    static final String SECOND_DETECTED = "Second screen detected";
    static final String ONE_SCREEN = "One screen";
    static final String EXTERNAL_ONLY = "External display · companion off";
    /** The detected profile family and second screen, e.g. "Snapdragon 8 Gen 2 class (QCS8550) · dual screen". */
    static String detected(String socClass, String soc, int secondScreen) {
        String chip = soc == null || soc.isEmpty() || soc.equalsIgnoreCase("unknown") ? "unknown chip" : soc;
        String screens = secondScreen == com.aure.totk.companion.DisplayClass.BUILT_IN ? "dual screen"
                : secondScreen == com.aure.totk.companion.DisplayClass.EXTERNAL ? "external display, companion off" : "one screen";
        return (socClass == null ? "Untested device" : socClass) + " (" + chip + ") · " + screens;
    }
    static final String DETECTED = "Detected";
    static final String CHOOSE_GRAPHICS = "Choose graphics";
    static final String SETUP_COMPLETE = "Setup complete · step 6 of 6";
    static final String TAKES_EFFECT = "Takes effect when you start the game. Change it any time from this menu.";
    static final String GPU_COST = "GPU cost", ADDED_LATENCY = "Added latency", WORKS_WITH = "Works with", APPLIES = "Applies";
    static final String WHILE_PLAYING = "Change while playing", SOUND_BUFFER = "Sound buffer", GAME_RESOLUTION = "Game targets";
    static final String STRENGTH = "Strength";
    static final String WHEN_STARTS = "when the game starts";
    static final String LOCKED_FRAMEGEN = "Set by frame generation";
    static final String NEEDS_UPSCALER = "Needs an upscaler (FSR 1 or Snapdragon GSR)";
    static final String HOLD = "Yes · press ";
    static final String HOLD_THEN = " then ";
    static final String CHORD_HINT = "Cycles Off → FPS → Full until the game closes.";
    static String presetChip(String preset) { return "Preset: " + presetName(preset); }
    static String presetName(String preset) {
        switch (preset) {
            case "light": return "Light";
            case "balanced": return "Balanced";
            case "smooth": return "Smooth";
            case "60fps": return "60 fps";
            default: return "Custom";
        }
    }
    static String presetCaption(String preset) {
        switch (preset) {
            case "light": return "30 fps · 960 × 540";
            case "balanced": return "30 fps · game size";
            case "smooth": return "30 → 60 generated";
            case "60fps": return "60 fps · game size";
            default: return "your own mix";
        }
    }
    static String presetText(String preset) {
        switch (preset) {
            case "light": return "For lighter GPUs: the game runs at 30 fps and draws its 3D scene at 960 × 540 with full shadows. "
                    + "The coolest and longest-lasting choice.";
            case "balanced": return "The game as it was made: 30 fps at the game's own resolution with full shadows.";
            case "smooth": return "The game runs at 30 fps and frame generation adds an in-between picture, so the screen shows 60. "
                    + "The 3D scene is drawn at 1152 × 648 to leave room for it.";
            case "60fps": return "The game itself runs at 60 fps at its own resolution. "
                    + "Needs a fast GPU; busy areas can dip below 60.";
            default: return "Your own combination of the options below. Pick a preset to start from one of the tested mixes.";
        }
    }

    static String optionLabel(String key) {
        switch (key) {
            case "frame_rate": return "Frame rate";
            case "frame_gen": return "Frame generation";
            case "render_res": return "Render resolution";
            case "latency": return "Latency";
            case "gpu_feed": return "Keep the GPU busy";
            case "upscaler": return "Upscaler";
            case "sharpness_fsr": case "sharpness_sgsr": return "Sharpness";
            case "aa": return "Anti-aliasing";
            case "shadows": return "Shadows";
            case "console_mode": return "Console mode";
            case "native_hud": return "Native HUD";
            case "overlay": return "Performance overlay";
            case "overlay_pos": return "Overlay position";
            case "second_screen": return "Second screen";
            case "buttons": return "Button layout";
            case "audio_latency": return "Audio latency";
            case "game_language": return "Game language";
            case "rumble": return "Rumble";
            case "motion": return "Motion controls";
            default: return key;
        }
    }
    static String optionHint(String key) {
        switch (key) {
            case "frame_rate": return "60 fps turns frame generation off";
            case "frame_gen": return "Shows an extra picture between two real ones";
            case "render_res": return "Size of the 3D scene before upscaling";
            case "latency": return "Responsive · Balanced · Smoothest";
            case "gpu_feed": return "Feeds work earlier · more power and heat";
            case "upscaler": return "Off · FSR 1 · Snapdragon GSR";
            case "sharpness_fsr": case "sharpness_sgsr": return "Applies to the upscaler";
            case "aa": return "Smooths jagged edges";
            case "shadows": return "Lower saves GPU time in busy areas";
            case "console_mode": return "Docked uses the game's larger TV resolution";
            case "native_hud": return "Draws menus and text at the panel's own resolution";
            case "overlay": return "Frame rate and frame time while you play";
            case "overlay_pos": return "Corner of the screen";
            case "second_screen": return "Map, inventory and shortcuts on the lower display";
            case "buttons": return "Which face button is the Switch's A";
            case "audio_latency": return "Low · Balanced · Safe";
            case "game_language": return "Text and voices in the game";
            case "rumble": return "Vibration strength · Off";
            case "motion": return "Aiming by tilting the device or controller";
            default: return "";
        }
    }
    static String optionText(String key) {
        switch (key) {
            case "frame_rate": return "How often the game itself updates. 30 fps is what the game was made for; 60 fps doubles the work "
                    + "for the processor and the GPU, and busy areas may drop below it.";
            case "frame_gen": return "Draws an in-between picture from two real frames, so the screen updates 60 times a second while "
                    + "the game itself runs at 30. Motion looks smoother; input feels about one frame later.";
            case "render_res": return "The size the game draws its 3D scene at before it is enlarged to fill the screen. Smaller is faster "
                    + "and softer. Game default lets the game choose, as on the console.";
            case "latency": return "How many finished pictures may wait for the screen. Responsive shows each one as soon as possible; "
                    + "Smoothest keeps one more in reserve to hide uneven frames, at up to one more frame of input delay.";
            case "gpu_feed": return "Sends smaller batches of work to the GPU earlier and asks Android for clocks that keep up. "
                    + "A little more GPU and CPU clock means more power and heat. On by default on devices where it was "
                    + "measured to help (Snapdragon 8 Elite class); has no effect with frame generation.";
            case "upscaler": return "How the picture is enlarged to the screen. FSR 1 is AMD's edge-aware upscaler; Snapdragon GSR is "
                    + "Qualcomm's, made for Adreno GPUs. Off uses plain filtering.";
            case "sharpness_fsr": case "sharpness_sgsr": return "How much detail the upscaler restores after enlarging the picture. Too high "
                    + "makes edges shimmer.";
            case "aa": return "Smooths stair-stepped edges. Game (FXAA) is the game's own filter; SMAA finds more edges and keeps "
                    + "textures sharper for a little more GPU time; Off is crispest but shows the steps.";
            case "shadows": return "The resolution of the shadow maps. Lower saves GPU time where many shadows are on screen; their "
                    + "edges get softer and blockier.";
            case "console_mode": return "Handheld runs the game as on a Switch held in your hands. Docked makes it believe it is "
                    + "connected to a TV: it draws at its larger docked resolution and needs more GPU time.";
            case "native_hud": return "Draws the game's menus, map and text at the screen's own resolution instead of enlarging them "
                    + "from the game's size, so text is sharper. Handheld only: docked mode always uses the game's HUD.";
            case "overlay": return "A small readout in the corner of the screen. “FPS” shows the frame rate only; “Full” adds frame time, "
                    + "the render and output resolution, the upscaler and a frame-time graph.";
            case "overlay_pos": return "Which corner of the screen the performance overlay sits in.";
            case "second_screen": return "On handhelds with a second display, shows the map, inventory and shortcuts on it while you "
                    + "play. Needs the second-screen art prepared during setup.";
            case "buttons": return "Nintendo: every button does what its label says, so A is A. Xbox: buttons act by position like on a "
                    + "Switch, so the right-hand button is A even though it is labelled B. Choose Xbox for gamepads with Xbox labels.";
            case "audio_latency": return "How much sound is queued before it plays. Low reacts fastest; Safe avoids crackling on busy "
                    + "systems and wireless headphones.";
            case "game_language": return "The language of the game's text and voices. Device language follows this device's first "
                    + "language when the game has it, and falls back to English otherwise.";
            case "rumble": return "The game's rumble on the controller's motors when it has its own, otherwise on this device's "
                    + "vibration motor. Off gives the game no rumble at all.";
            case "motion": return "The game's motion controls (aiming the bow, some puzzles) from the controller's gyroscope when it "
                    + "has one, otherwise from this device's. The game's own options can still turn motion aiming off.";
            default: return "";
        }
    }
    static String valueName(String key, String value) {
        switch (key) {
            case "frame_rate": return value + " fps";
            case "frame_gen": return value.equals("on") ? "Interpolation" : "Off";
            case "gpu_feed": return value.equals("on") ? "On" : "Off";
            case "render_res": return value.equals("game") ? "Game default" : value.replace("x", " × ");
            case "upscaler": return upscalerName(value);
            case "aa": return value.equals("fxaa") ? "Game (FXAA)" : value.equals("smaa") ? "SMAA" : "Off";
            case "overlay": return value.equals("fps") ? "FPS" : value.equals("full") ? "Full" : "Off";
            case "overlay_pos":
                switch (value) { case "tl": return "Top left"; case "tr": return "Top right"; case "bl": return "Bottom left"; default: return "Bottom right"; }
            case "buttons": return value.equals("xbox") ? "Xbox" : "Nintendo";
            case "game_language": return languageName(value);
            case "rumble": return value.equals("0") ? "Off" : value + " %";
            default: return value.isEmpty() ? value : Character.toUpperCase(value.charAt(0)) + value.substring(1);
        }
    }
    static String upscalerName(String value) { return value.equals("fsr") ? "FSR 1" : value.equals("sgsr") ? "Snapdragon GSR" : "Off"; }
    static String languageName(String code) {
        switch (code) {
            case "auto": return "Device language";
            case "en-US": return "English";
            case "ja": return "日本語";
            case "de": return "Deutsch";
            case "fr": return "Français";
            case "es": return "Español";
            case "it": return "Italiano";
            case "nl": return "Nederlands";
            case "ru": return "Русский";
            case "ko": return "한국어";
            case "zh-Hans": return "简体中文";
            case "zh-Hant": return "繁體中文";
            case "pt-BR": return "Português (Brasil)";
            default: return code;
        }
    }
    /** Short value for the home screen summaries. */
    static String graphicsSummary(Settings s) {
        String rate = s.get("frame_gen").equals("on") ? "30 → 60 generated" : s.get("frame_rate") + " fps";
        String res = s.get("render_res").equals("game") ? "game resolution" : s.get("render_res").replace("x", " × ");
        String up = s.get("upscaler").equals("off") ? "" : " · " + upscalerName(s.get("upscaler"));
        return presetName(s.preset()) + " · " + rate + " · " + res + up;
    }
    static String controlsSummary(Settings s) {
        return valueName("buttons", s.get("buttons")) + " layout · " + languageName(s.get("game_language")) + " · audio " + s.get("audio_latency");
    }
    static String addedLatency(String key, String value) {
        if (key.equals("frame_gen")) return value.equals("on") ? "≈ 33 ms" : "none";
        switch (value) { case "responsive": return "lowest"; case "balanced": return "low"; default: return "up to one more frame"; }
    }

    // Diagram captions
    static final String DIAG_GENERATED = "generated", DIAG_REAL = "real", DIAG_DYNAMIC = "game chooses", DIAG_SCREEN = "screen";
    static final String DIAG_GAME = "Game", DIAG_SCREEN_SHORT = "Screen", DIAG_PLAIN = "plain";
    static final String DIAG_HANDHELD_RES = "Handheld · up to 1280 × 720", DIAG_DOCKED_RES = "Docked · up to 1920 × 1080";
    static final String DIAG_HUD_TEXT = "Sample caption 123", DIAG_NATIVE = "panel resolution", DIAG_ENLARGED = "enlarged";
    static final String DIAG_OFF = "off", DIAG_NO_SECOND = "No second display on this device right now";
    static final String DIAG_ACTS_AS_A_LABEL = "A is A", DIAG_ACTS_AS_A_POS = "acts as A";
    static String waiting(int n) { return n == 1 ? "1 picture may wait" : n + " pictures may wait"; }
    static String buffer(int ms) { return ms + " ms queued"; }
    static String shadowRes(String quality) { return quality.equals("high") ? "full shadow map" : quality.equals("medium") ? "shadow map 288" : "shadow map 256"; }

    // Game data
    static final String INSTALLED = "Installed game";
    static final String NOT_INSTALLED = "Not installed";
    static final String VERIFIED = "verified";
    static final String COMPANION_ROW = "Second-screen art";
    static final String COMPANION_HINT = "Map, inventory and shortcut pictures for the lower display";
    static final String ART_NOT_NEEDED = "Not needed on this device";
    static final String ART_NOT_NEEDED_HINT = "No built-in second screen · you can still prepare the art";
    static final String PREPARE_ART = "Prepare second-screen art";
    static final String ART_OFFER_TITLE = "Prepare your second screen?";
    static final String ART_OFFER_BODY = "This device has a built-in second screen, but its map, inventory and shortcut pictures "
            + "aren't prepared. Building took about 15 seconds on a Pocket Duo; other devices may take longer. The game starts when it's ready.";
    static final String ART_PREPARE_HINT = "Build from your imported game data, then play";
    static final String PLAY_WITHOUT_ART = "Play without it";
    static final String ART_DECLINE_HINT = "Start now · don't ask again for this install";
    static final String ART_OFFER_FAILED = "The second-screen art wasn't prepared. Start game offers to try again or play without it.";
    static final String SHADERS_ROW = "Shaders";
    static final String SHADERS_HINT = "Required: the game's GPU shaders, prepared from your game data";
    static final String DRIVER = "Graphics driver";
    static final String DRIVER_HINT = "The bundled Turnip build, or a Turnip .zip / .so you choose";
    static final String DRIVER_RESET_ROW = "Use the bundled driver";
    static final String DRIVER_RESET_HINT = "Removes the custom driver file";
    static final String DRIVER_BUNDLED = "Bundled";
    static final String DRIVER_BAD = "That file is not a Vulkan driver: pick a Turnip .zip (AdrenoTools) or its .so (64-bit ARM).";
    static final String DRIVER_ZIP_BAD = "That zip has no usable driver. Its meta.json must name an existing libraryName, or it must "
            + "contain exactly one .so (64-bit ARM, up to 128 MiB; at most 256 entries).";
    static final String RERUN = "Run setup again";
    static final String RERUN_HINT = "Choose your files again; finished steps are kept";
    static final String DELETE_ROW = "Delete imported data";
    static final String DELETE_HINT = "About 18 GB; saves, settings and the driver are kept";
    static final String READY = "Ready";
    static final String NOT_PREPARED = "Not prepared";
    static final String NOT_BUILT = "Not built";
    static final String OUT_OF_DATE = "Out of date";
    static final String BUILD = "Build", REBUILD = "Rebuild", UPDATE = "Update";
    static final String NEEDS_SETUP = "Finish setup first";
    static final String MEASURING = "measuring…", SIZE_UNKNOWN = "size unknown";
    static final String DATA_TEXT = "The game data imported from your own files: rewritten game code, the game's data image and its "
            + "index, fonts and the shader seed, plus optional second-screen art. The import was checked against the pinned digests of version 1.4.3.";
    static final String COMPANION_TEXT = "Pictures for the second-screen companion (map, inventory, shortcuts), generated on this device "
            + "from your game data. Optional: the game runs without them. Setup skips them without a built-in second screen; you can build them here.";
    static final String SHADERS_TEXT = "The seed of translated GPU shaders generated from your game data. The game needs it: without "
            + "it nothing is drawn. Building takes a few minutes.";
    static final String DRIVER_TEXT = "The Vulkan driver the game uses. The bundled Mesa Turnip build is tested with this app. Pick a "
            + "Turnip .zip (AdrenoTools format) or a 64-bit ARM .so; only the driver library is copied into the game folder and used instead.";
    static final String RERUN_TEXT = "Opens setup again to pick different files. Steps that are already finished and still match "
            + "are kept.";
    static final String DELETE_TEXT = "Removes the imported game data from this device. Saves, menu settings and a custom driver stay. "
            + "To play again you will need your files and to run setup again.";
    /** The game data size (DataSize): measuring while a walk runs, unknown when it failed or ran out of time. */
    static String size(long bytes) { return bytes == DataSize.MEASURING ? MEASURING : bytes < 0 ? SIZE_UNKNOWN : bytes(bytes); }

    // Game data: storage and moving
    static final String INTERNAL_STORAGE = "Internal storage", SD_CARD = "SD card", INTERNAL_OLD = "Internal storage (old location)";
    static final String STORAGE_ROW = "Storage", STORAGE_HINT = "Where the game data is kept";
    static final String STORAGE_TEXT = "Where the imported game data lives. Internal storage is the app's own storage: the "
            + "fastest place, and nothing else on the device can change it. An SD card frees room on the device. A folder you "
            + "choose (on internal storage or a card) needs All files access. Moving copies everything, checks it again in the "
            + "new place, and only then removes the old copy. Saves always stay on the device.";
    static final String STORAGE_TITLE = "Where to keep the game data";
    static final String STORAGE_LEAD = "Internal storage is fastest: the game starts and loads new areas quicker from it. "
            + "Moving takes a while; the data is checked again in the new place before the old copy is removed.";
    static final String SETUP_STORAGE_TITLE = "Where should the game go?";
    static final String SETUP_STORAGE_LEAD = "The game data needs about 19 GB. Internal storage is fastest; an SD card keeps "
            + "the device free. You can move it later from Game data.";
    static final String STEP_STORAGE = "Storage", STEP_STORAGE_TEXT = "Internal storage, an SD card or a folder";
    static final String IN_USE_STATE = "In use", NOT_ENOUGH = "Not enough space", FASTER = "fastest", CHOOSE_STATE = "Choose";
    static final String NOT_AVAILABLE = "not available";
    static String volumeLine(long free, long need) { return bytes(free) + " free · needs " + bytes(need); }
    static String storageValue(String name, long free) { return name + " · " + bytes(free) + " free"; }
    static final String MOVING_TITLE = "Moving game data";
    static final String MOVE_TIP = "If the app closes, the game data stays where it was; start the move again from Game data.";
    static final String STAGE_MOVE = "Moving game data";
    static final String NOTIFY_MOVING = "Moving TotK Native game data", NOTIFY_MOVED = "Game data moved", NOTIFY_MOVE_FAILED = "Moving the game data stopped";
    static String moved(String name) { return "Game data moved to " + name + "."; }
    static String moveFailed(String code, String name) {
        String why = "cancelled".equals(code) ? "Moving was cancelled." : "no_space".equals(code) ? "Not enough free space on the new storage."
                : "busy".equals(code) ? error("game_running") : "access".equals(code) ? folderProblem("access")
                : "not_available".equals(code) ? "The new storage is not available." : error(code);
        return why + " The game data is still on " + name + ".";
    }
    static String moveInterrupted(String name) { return "Moving was interrupted. The game data is on " + name + "."; }
    static final String MOVE_PARTIAL = "Finish setup before moving the game data.";
    static final String RECOMMENDED_SHORT = "Recommended";
    static final String FOLDER_INCOMPLETE = "Save folder incomplete, kept your saves. The folder's newest copy does not match its "
            + "list of files (it may be half copied or edited).";
    static final String FOLDER_INCOMPLETE_SHORT = "Incomplete · kept your saves";
    static final String FOLDER_REPLACE = "Replace the folder's copy", FOLDER_REPLACE_HINT = "Upload this device's saves; the folder's copy is backed up here first";
    static final String FOLDER_TURNED_OFF = "Save folder turned off · its files are left as they are";
    static String files(int n) { return n == 1 ? "1 file" : n + " files"; }
    static String missingVolume(String name) {
        return "Game data is on " + name + ", which isn't available. Insert the card, or set up again on another storage.";
    }
    static final String MISSING_CHIP = "Game data not available";
    static final String MISSING_START_HINT = "Insert the card with the game data";
    static final String SETUP_AGAIN_ROW = "Set up again on another storage", SETUP_AGAIN_HINT = "Starts a new setup; the card's copy is left alone";

    // Direct file access (All files access, MANAGE_EXTERNAL_STORAGE): optional
    static final String ACCESS_ROW = "All files access";
    static final String ACCESS_HINT = "Optional · open your game files directly, keep game data in any folder";
    static final String ACCESS_ON = "Allowed", ACCESS_OFF = "Off · Allow";
    static final String ACCESS_TITLE = "Allow All files access?";
    static final String ACCESS_TEXT = "With All files access, TotK Native opens the files you pick directly instead of through "
            + "the file picker (faster, and large files can be read in parallel), and it can keep the game data in a folder "
            + "you choose. It is optional: without it everything works through the file picker. Android opens its settings "
            + "page; switch on \"Allow access to manage all files\", then come back.";
    static final String ACCESS_ALLOW = "Open the settings", ACCESS_ALLOW_HINT = "Turn on \"Allow access to manage all files\" for TotK Native";
    static final String ACCESS_SKIP = "Not now", ACCESS_SKIP_HINT = "Keep using the file picker";
    static final String FOLDER_CARD = "A folder you choose";
    static final String FOLDER_CARD_LINE = "On internal storage or the SD card";
    static final String NEEDS_ACCESS = "Needs All files access";
    static String accessLost(String name) {
        return "Game data is in " + name + ", and TotK Native no longer has All files access. Allow it again to play, or set up again elsewhere.";
    }
    static final String ACCESS_LOST_CHIP = "All files access is off";
    static final String ACCESS_START_HINT = "Allow All files access to continue";
    static final String ACCESS_FIX_ROW = "Allow All files access", ACCESS_FIX_HINT = "The game data is in a folder that needs it";
    static String folderProblem(String code) {
        switch (code == null ? "" : code) {
            case "access": return "Choosing a folder needs All files access.";
            case "android_folder": return "Folders inside Android can't hold the game data. Choose another folder.";
            case "volume_root": return "Choose a folder on the storage, not the whole storage.";
            case "private": return "That folder is the app's own storage; choose Internal storage instead.";
            default: return "That folder can't be used directly. Choose one on internal storage or the SD card, starting from the storage's top level.";
        }
    }
    static final String MIGRATE_TITLE = "Moving game data to faster storage";
    static final String MIGRATE_BODY = "TotK Native now keeps the game data in its own internal storage, which loads faster. "
            + "Your installed game is still in the old location and stays playable there until the move has finished and "
            + "been checked. Saves are not affected.";
    static final String MIGRATE_NOW = "Move now", MIGRATE_NOW_HINT = "Takes a few minutes; the copy is checked before the old one is removed";
    static final String MIGRATE_LATER = "Not now", MIGRATE_LATER_HINT = "Keep it where it is; you can move it later from Game data";

    // Saves
    static final String SAVES = "Saves";
    static final String SAVES_EXPORT = "Export saves", SAVES_EXPORT_HINT = "A zip with all your save slots";
    static final String SAVES_IMPORT = "Import saves", SAVES_IMPORT_HINT = "From a zip or a folder · your saves are backed up first";
    static final String SAVES_FOLDER = "Save folder", SAVES_FOLDER_HINT = "Keeps a copy in a folder you choose, e.g. on an SD card";
    static final String SAVES_SYNC = "Sync now", SAVES_SYNC_HINT = "Copy the newest saves between this device and the folder";
    static final String SAVES_BACKUPS = "Backups", SAVES_BACKUPS_HINT = "Made before every import, restore or folder update";
    static final String SAVES_NONE = "No saves yet";
    static final String YOUR_SAVES = "Your saves";
    static final String SAVES_TEXT = "The game saves on this device. Export makes a zip you can keep anywhere; Import replaces "
            + "these slots with a save set from a zip or folder (this works with exports of this app and common save tools).";
    static final String FOLDER_TEXT = "The game keeps saving on this device; after every save, and when you return here, the "
            + "saves are also copied to the folder you pick. When the folder has newer saves (from another device), they are "
            + "brought back before the game starts, after a backup. Nothing is ever deleted when the folder is missing.";
    static final String BACKUPS_TEXT = "The ten newest copies of your saves, made automatically before an import, a restore or "
            + "an update from the save folder. Restoring one backs up the current saves first.";
    static final String FOLDER_OFF = "Off", FOLDER_UNAVAILABLE = "Save folder unavailable", FOLDER_CONFLICT = "Needs a choice";
    static final String FOLDER_ROW_FACT = "Folder", LAST_SYNCED = "Last synced", STATUS_FACT = "Status", SLOTS_FACT = "Slots", NEWEST_FACT = "Newest";
    static final String NEVER = "never";
    static final String SAVES_UPDATED = "Saves updated from your save folder";
    static final String SYNCING = "Syncing…", EXPORTING = "Exporting…", CHECKING_SAVES = "Checking the saves…", IMPORTING = "Importing…";
    static final String RESTORING = "Restoring…";
    static final String SYNCED = "Saves are in sync", PUSHED = "Saves copied to the folder", PULLED = "Saves updated from the folder";
    static final String PULL_LATER = "The folder has newer saves; close the game to bring them in";
    static String exported(String name) { return "Saves exported to " + name; }
    /** backedUp: the replaced set was backed up (false when the device had no saves to back up). */
    static String imported(int slots, boolean backedUp) { return (slots == 1 ? "1 slot imported" : slots + " slots imported") + (backedUp ? " · previous saves backed up" : ""); }
    static String restored(boolean backedUp) { return backedUp ? "Backup restored · previous saves backed up" : "Backup restored"; }
    static final String BACKUP_DELETED = "Backup deleted";
    static String slotCount(int n) { return n == 1 ? "1 slot" : n + " slots"; }
    static String slotTitle(int index) { return "Slot " + (index + 1); }
    /** Home/Welcome summary line; folder is the lowercase phrase from folderSummary. */
    static String savesSummary(int slots, long newest, String folder) {
        return (slots == 0 ? SAVES_NONE : slotCount(slots) + " · newest " + date(newest)) + " · " + folder;
    }
    static String folderSummary(String state, String name) {
        switch (state == null ? "" : state) {
            case "off": return "save folder off";
            case "unavailable": return "save folder unavailable";
            case "incomplete": return "save folder incomplete";
            case "conflict": return "save folder needs a choice";
            default: return "save folder " + name;
        }
    }
    static String folderValue(String name, long synced) { return name + " · " + (synced > 0 ? "synced " + date(synced) : "not synced yet"); }
    static String backupCount(int n) { return n == 0 ? "none yet" : n == 1 ? "1 backup" : n + " backups"; }
    static String reason(String reason) {
        switch (reason) {
            case "import": return "Before an import";
            case "restore": return "Before a restore";
            case "mirror": return "Before a save folder update";
            default: return "Backup";
        }
    }
    static String date(long millis) {
        if (millis <= 0) return NEVER;
        return new java.text.SimpleDateFormat("d MMM yyyy, HH:mm", Locale.UK).format(new java.util.Date(millis));
    }
    static final String IMPORT_FROM = "Import saves from", IMPORT_ZIP = "A zip file", IMPORT_ZIP_HINT = "An export of this app or a save tool's zip";
    static final String IMPORT_TREE = "A folder", IMPORT_TREE_HINT = "A folder with slot_00 … slot_05 (e.g. from JKSV or Checkpoint)";
    static String importFound(int slots) { return "Found " + slotCount(slots); }
    static String importLead(String source, long newest, int current) {
        return "From " + source + " · newest save " + date(newest) + ". "
                + (current == 0 ? "This device has no saves yet." : "Your current " + slotCount(current) + (current == 1 ? " is" : " are")
                + " backed up first; you can restore " + (current == 1 ? "it" : "them") + " from Backups.") + " The whole save set is replaced.";
    }
    static final String IMPORT_CHIP = "Checked · nothing changed yet";
    static final String REPLACE_SAVES = "Replace my saves", REPLACE_SAVES_HINT = "Import this save set";
    static final String KEEP_SAVES = "Keep my saves", KEEP_SAVES_HINT = "Nothing is changed";
    static final String FOLDER_TITLE = "Save folder";
    static final String FOLDER_CHANGE = "Choose another folder", FOLDER_CHANGE_HINT = "The current folder keeps its copy";
    static final String FOLDER_TURN_OFF = "Turn off the save folder", FOLDER_TURN_OFF_HINT = "The folder and its files are left as they are";
    static final String CONFLICT_TITLE = "Your saves changed in two places";
    static final String CONFLICT_LEAD = "This device and your save folder both have saves the other one doesn't. Choose which to "
            + "keep; the other copy is backed up first and stays under Backups.";
    static final String CONFLICT_CHIP = "Both copies changed";
    static final String THIS_DEVICE = "This device", THE_FOLDER = "Save folder", DECIDE_LATER = "Decide later", DECIDE_LATER_HINT = "Nothing is copied until you choose";
    static String side(int slots, long newest, String device) {
        return slotCount(slots) + " · newest " + date(newest) + (device == null || device.isEmpty() ? "" : " · " + device);
    }
    static final String KEEP_THIS = "Keep these";
    static final String RESTORE_TITLE = "Restore this backup?";
    static String restoreBody(long created, int slots) {
        return "The saves from " + date(created) + " (" + slotCount(Math.max(0, slots)) + ") replace the current ones, which are "
                + "backed up first.";
    }
    static final String RESTORE = "Restore", RESTORE_HINT = "Replace the current saves with this backup";
    static final String KEEP_CURRENT = "Keep the current saves";
    static final String DELETE_BACKUP_TITLE = "Delete this backup?";
    static String deleteBackupBody(long created) { return "The backup from " + date(created) + " is removed from this device."; }
    static final String DELETE_BACKUP = "Delete the backup", KEEP_BACKUP = "Keep it";
    static final String P_RESTORE = "Restore", P_DELETE = "Delete";
    static String saveError(String code, String detail) {
        switch (code == null ? "" : code) {
            case "absolute": case "traversal": return "This dump has file names that point outside its folder, so it was not imported.";
            case "symlink": return "This dump contains links, which a save set never has, so it was not imported.";
            case "too_many": return "This has more than 500 files, so it is not a TotK save set.";
            case "too_big": return "This is larger than 128 MB, so it is not a TotK save set.";
            case "huge_file": return "It contains a file far larger than any save file, so it was not imported.";
            case "no_saves": return "No TotK saves found. A save set has slot_00 … slot_05 folders with progress.sav, or option.sav.";
            case "corrupt": return "The saves are damaged or do not match their list of files" + (detail == null ? "." : ": " + detail);
            case "not_zip": return "That file is not a zip archive.";
            case "game_running": return error("game_running");
            case "unavailable": return "The folder or file can no longer be opened (access was removed, or its card is out).";
            default: return "Reading or writing the saves failed" + (detail == null || detail.isEmpty() ? "." : ": " + detail);
        }
    }

    // About
    static final String ABOUT_TEXT = "An unofficial port that runs Tears of the Kingdom 1.4.3 natively on Android handhelds from "
            + "your own game files. It is not affiliated with or endorsed by Nintendo.";
    static final String VERSION_ROW = "Version";
    static final String LICENCES_ROW = "Open-source licences";
    static final String LICENCES_HINT = "Turnip, Noto Sans, hactool and the other components";
    static final String LICENCES_TEXT = "The licence texts of every open-source component shipped in this app.";
    static final String PROJECT_ROW = "Project";
    static final String PROJECT_TEXT = "Source code, issues and releases: " + PROJECT_URL + ".\nBuilt on MacNativeGames by ly05010419 "
            + "and on kitcatkode's Android port of it.";
    static final String UPSTREAM_ROW = "MacNativeGames", UPSTREAM_BY = "ly05010419 · " + UPSTREAM_URL;
    static final String ANDROID_PORT_ROW = "Android port", ANDROID_PORT_BY = "kitcatkode · " + ANDROID_PORT_URL;
    static final String LICENSES = "Open-source licences";

    // Setup: steps
    static final String SETUP_TITLE = "Set up your game";
    static final String NOTHING_UPLOADED = "Nothing is uploaded";
    static final String STEP_ROUTE = "What you have", STEP_FILES = "Choose your files", STEP_PREPARE = "Prepare", STEP_GRAPHICS = "Choose graphics";
    static final String STEP_ROUTE_NONE = "A decrypted dump, or game files and keys";
    static final String STEP_FILES_TEXT = "From this device or an SD card";
    static String prepareEstimate(String route) {
        return Inputs.DECRYPTED.equals(route) ? "About 2–3 min · longer when copying from an SD card"
                : Inputs.CONTAINERS.equals(route) ? "About 15 min, plus SD-card copy time"
                : "About 2–3 min decrypted, or 15 min XCI/NSP, plus SD-card copy time";
    }
    static final String STEP_GRAPHICS_TEXT = "Pick a preset, start playing";
    static String routeName(String route) { return Inputs.CONTAINERS.equals(route) ? "Game files and keys" : Inputs.DECRYPTED.equals(route) ? "Decrypted dump" : STEP_ROUTE_NONE; }

    // Welcome
    static final String WELCOME_TITLE = "Prepare your game";
    static final String WELCOME_BODY = "This app contains no game data. Point it at your own dump of the game and it builds "
            + "everything it needs here on the device. Plan for about 19 GB of free space. Preparing a decrypted dump takes "
            + "roughly 2–3 minutes; XCI/NSP around 15 minutes, plus time copying from an SD card. Times vary by device and storage.";
    static final String BEGIN = "Begin setup";
    static final String BEGIN_HINT = "Pick what you have, then choose your files";
    static final String CONTINUE_SETUP = "Continue setup";
    static final String WELCOME_DATA_HINT = "Graphics driver, unfinished imports";

    // Route choice
    static final String ROUTE_TITLE = "What do you have?";
    static final String ROUTE_BODY = "Both routes need version 1.4.3. Pick the one that matches your files.";
    static final String ROUTE_DECRYPTED = "RomFS + ExeFS dump";
    static final String ROUTE_DECRYPTED_BODY = "Decrypted files, e.g. from nxdumptool or hactool: the 1.4.3 RomFS "
            + "(one file or its split parts) and the ExeFS files main, sdk, subsdk0. No keys needed. "
            + "About 2–3 minutes to prepare; copying from an SD card can take longer.";
    static final String ROUTE_CONTAINERS = "XCI / NSP + update + keys";
    static final String ROUTE_CONTAINERS_BODY = "The base game 1.0.0 as XCI or NSP, the 1.4.3 update NSP, and the "
            + "prod.keys from your own console. Decryption happens on the device. About 15 minutes to prepare, plus SD-card copy time.";
    static final String BACK = "Back";

    // Files
    static final String FILES_TITLE = "Choose your files";
    static final String FILES_LEAD_DECRYPTED = "Both must be version 1.4.3. Every file is checked before anything is written.";
    static final String FILES_LEAD_CONTAINERS = "The base game 1.0.0, the 1.4.3 update and your keys. Every file is checked before anything is written.";
    static final String ALT_DECRYPTED = "Have an XCI/NSP and your keys instead? ";
    static final String ALT_DECRYPTED_LINK = "Use game files and keys";
    static final String ALT_CONTAINERS = "Have a decrypted RomFS and ExeFS instead? ";
    static final String ALT_CONTAINERS_LINK = "Use a decrypted dump";
    static final String ROLE_ROMFS = "RomFS image";
    static final String ROLE_ROMFS_HINT = "romfs.bin, or all of its split parts";
    static final String JOIN_ORDER = ", joined in this order: ";
    static final String ROLE_EXEFS = "ExeFS";
    static final String ROLE_EXEFS_HINT = "The folder with main, sdk and subsdk0";
    static final String ROLE_BASE = "Base game 1.0.0";
    static final String ROLE_BASE_HINT = "XCI or NSP";
    static final String ROLE_UPDATE = "Update 1.4.3";
    static final String ROLE_UPDATE_HINT = "NSP";
    static final String ROLE_KEYS = "Keys";
    static final String ROLE_KEYS_HINT = "prod.keys from your console";
    static final String PICK_FILES = "Choose files", PICK_FILE = "Choose a file", PICK_FOLDER = "Choose a folder";
    static final String PICK_OR_FOLDER = "or a whole folder (X)", PICK_OR_FILES = "or pick the three files (X)";
    static final String NOT_PICKED = "Not chosen";
    static final String CHECKING = "Checking…";
    static final String PICKED = "Chosen";
    static final String LOOKS_GOOD = "Looks good";
    static final String INSTALL = "Install";
    static final String STORAGE = "Storage";
    static final String NO_LIBRARY = "The setup component is missing from this build, so files cannot be checked.";
    static final String ALL_SET = "Everything needed is here. Press + or Install.";
    static final String FIX_MARKED = "Choose a different file where a red mark is shown.";
    static final String CANT_OPEN = "Can't open — choose again";
    static final String NEED = "Still needed: ";
    static final String PICKER_MISSING = "No document picker is available on this device.";
    static String storageLine(long used, long total) { return bytes(used) + " of " + bytes(total); }

    // Install
    static final String PREPARING_TITLE = "Preparing the game";
    static final String KEEPS_GOING = "Keeps going with the screen off";
    static final String STEPS = "Steps";
    static final String RESUME_TIP = "If the app closes, setup continues from the last finished step.";
    static final String OPTIONAL = "optional";
    static final String CANCEL = "Cancel";
    static final String CANCELLING = "Cancelling…";
    static final String STOPPING = "Stopping the second-screen art…";
    static final String EXTRAS_RUNNING = "The game is ready. The second-screen art is still being prepared — play without it now "
            + "and build it later from Game data, or wait here.";
    static final String STAGE_CHECK = "Checking files";
    static final String STAGE_CODE = "Game code";
    static final String STAGE_ROMFS = "Game data";
    static final String STAGE_INDEX = "Data index";
    static final String STAGE_FONTS = "Fonts";
    static final String STAGE_COMPANION = "Second-screen art";
    static final String STAGE_SHADERS = "Shaders";
    static final String WAITING = "waiting";
    static final String DONE = "done";
    static final String SKIPPED = "not prepared";
    static final String ART_SKIPPED = "skipped";
    static final String FAILED = "Failed";
    static final String STARTING = "Starting…";
    // A single stage of the last two, started from Game data (or Home's Build shaders); again: its output exists already
    // and stays in place until the new one is complete. Only the second-screen art may be stopped for the game.
    static String rebuildTitle(String stage, boolean again, boolean artUpdate) {
        return stage.equals("shaders") ? (again ? "Rebuilding shaders" : "Building shaders")
                : artUpdate ? "Updating second-screen art" : again ? "Rebuilding the second-screen art" : "Preparing the second-screen art";
    }
    static String rebuildNote(String stage, boolean again, boolean artUpdate) {
        if (stage.equals("shaders"))
            return again ? "Rebuilding shaders. The current shaders stay in use until the new seed is complete; cancelling keeps them."
                    : "Building shaders. The game needs them to draw anything; Start game is offered once they are done.";
        if (artUpdate)
            return "The second-screen art was made by an earlier version of the app; this version makes it again (item names, icons). "
                    + "Play now stops the update and starts the game with the current art; it is offered again next time.";
        return again ? "Rebuilding the second-screen art. Play now stops the rebuild and starts the game with the current art; nothing is lost."
                : "Preparing the second-screen art. Play without it stops this and starts the game; build it later from Game data.";
    }
    static String rebuilt(String stage, int shaders) {
        return stage.equals("shaders") ? "Shaders ready · " + variants(shaders) : "Second-screen art ready";
    }

    // settings.env could not be written before starting the game
    static final String SAVE_FAILED_CHIP = "Settings not saved";
    static final String SAVE_FAILED_TITLE = "Could not save your settings";
    static String saveFailed(String why) {
        return "Could not save your settings: " + why + ". Starting now would use the settings saved last time (or the "
                + "defaults), not the ones shown in the menu.";
    }
    static final String RETRY = "Retry", RETRY_HINT = "Save the settings again, then start the game";
    static final String START_ANYWAY = "Start anyway", START_ANYWAY_HINT = "Play with the settings saved last time";

    // The game process stopped on a fatal error (TotkActivity.showGameError): Home's chip and the reason under it
    static final String GAME_STOPPED_CHIP = "The game stopped";
    static String gameStopped(String reason) {
        return reason + " Your saves are kept; progress since the last save is lost. Details are in totk.log.";
    }
    static String stageDone(String stage) {
        switch (stage) {
            case "code": return "Rewritten for Android and verified";
            case "romfs": return "Copied and checked block by block";
            case "index": return "Built and verified";
            case "fonts": return "Noto Sans in all six font slots";
            default: return null;
        }
    }

    // Done (notification text only: the menu continues with Choose graphics)
    static final String DONE_TITLE = "Ready to play";

    // Errors
    static final String ERROR_TITLE = "Setup stopped";
    static final String INTERRUPTED_TITLE = "Setup was interrupted";
    static final String INTERRUPTED_BODY = "The app was closed while setting up. Finished steps are kept; resume to continue.";
    static final String RESUME = "Resume";
    static final String RESUME_HINT = "Continue from the last finished step";
    static final String CHANGE_FILES = "Change files";
    static final String CHANGE_FILES_HINT = "Pick other files or the other route";
    static final String DETAILS = "Details: ";
    static final String SHARE_DETAILS = "Share details";
    static final String SHARE_DETAILS_HINT = "Share details before Resume to include the logs";
    static final String SHARE_DETAILS_BUSY = "Preparing details…";
    static final String SHARE_DETAILS_MISSING = "No app is available to share text.";

    static final String CLOSE_GAME = "Close the game";
    static final String WAITING_SAVE = "Waiting for the game to finish saving…";
    static final String CLOSING_GAME = "Closing the game…";
    static final String SAVE_WAIT_TIMEOUT = "The game is still saving after 30 seconds. It was not closed. Let it finish saving, then try again.";
    static final String CLOSE_GAME_TIMEOUT = "The game did not finish closing. Return to the game and close it, then try again.";
    static String gameRunning(String action) { return "TotK Native's game is still running in the background. " + action + " needs it closed."; }
    static String gameLockError(String reason) { return "Could not check or close the game: " + reason; }

    static String error(String code) {
        switch (code == null ? "" : code) {
            case "hash_mismatch": return "A file does not match Tears of the Kingdom 1.4.3. Make sure the dump is complete "
                    + "(all split parts, update applied), then choose it again.";
            case "missing_input": return "Something is missing. Choose every requested file, then resume.";
            case "bad_keys": return "These keys cannot decrypt your game. Use the prod.keys dumped from your own console "
                    + "with firmware at least as new as the 1.4.3 update.";
            case "unsupported_version": return "This is not version 1.4.3. Only 1.4.3 is supported: choose the 1.4.3 update, "
                    + "or a RomFS/ExeFS dump made with the 1.4.3 update applied.";
            case "no_space": return "Not enough free space. Free up storage, then resume.";
            case "io": return "Reading or writing a file failed. If your files are on an SD card or USB drive, keep it "
                    + "connected, then resume.";
            case "cancelled": return "Setup was cancelled. Finished steps are kept; resume when you are ready.";
            case "permission": return "This app can no longer open some of your chosen files (access was removed, or "
                    + "they were moved, renamed or are on a removed card). Choose them again below.";
            case "library": return NO_LIBRARY;
            case "game_running": return gameRunning("Installing");
            case "game_lock": return "Could not check the game's lock. The error details below explain why; nothing was changed.";
            case "not_available": return "Not available on this device.";
            default: return "Something unexpected went wrong. Resume to retry; if it keeps failing, report it with the details below.";
        }
    }

    // Delete
    static final String DELETE_CONFIRM_TITLE = "Delete imported data?";
    static final String DELETE_CONFIRM_BODY = "This removes the imported game data (about 18 GB). Saves, settings and the "
            + "custom driver are kept. To play again you will need your files and to run setup again.";
    static final String KEEP = "Keep the data";
    static final String DELETE = "Delete";
    static final String DELETING = "Deleting…";
    static final String DELETED = "Imported data deleted.";

    // Notification and launcher
    static final String CHANNEL = "Setup progress";
    static final String NOTIFY_RUNNING = "Setting up TotK Native";
    static final String NOTIFY_DONE = "TotK Native is ready";
    static final String NOTIFY_FAILED = "TotK Native setup stopped";
    static final String SHORTCUT_SETTINGS = "Settings";

    static String stage(String name) {
        switch (name == null ? "" : name) {
            case "inspect": return STAGE_CHECK;
            case "exefs": case "code": return STAGE_CODE;
            case "romfs": return STAGE_ROMFS;
            case "index": return STAGE_INDEX;
            case "fonts": return STAGE_FONTS;
            case "companion": return STAGE_COMPANION;
            case "shaders": return STAGE_SHADERS;
            case "move": return STAGE_MOVE;
            default: return name == null ? "" : name;
        }
    }
    static String bytes(long value) {
        if (value < 0) return "?";
        if (value >= 1_000_000_000L) return String.format(Locale.US, "%.1f GB", value / 1e9);
        if (value >= 1_000_000L) return String.format(Locale.US, "%.0f MB", value / 1e6);
        if (value >= 1_000L) return String.format(Locale.US, "%.0f kB", value / 1e3);
        return value + " B";
    }
    static String storageShort(long missing) { return "Free up " + bytes(missing) + " more."; }
    static String parts(int count, long size) { return count + " files · " + bytes(size); }
    static String eta(long seconds) {
        if (seconds < 90) return "about 1 minute left";
        if (seconds < 3600) return "about " + (seconds + 30) / 60 + " minutes left";
        return String.format(Locale.US, "about %d h %02d min left", seconds / 3600, (seconds % 3600) / 60);
    }
    static String rate(double bytesPerSecond) { return bytes((long) bytesPerSecond) + "/s"; }
    static String variants(int count) { return String.format(Locale.US, "%,d shaders", count); }
    static String version(String app, long code) { return "App " + app + " (" + code + ")"; }
    static final String GAME_VERSION = "1.4.3";
    static String build(String commit) { return commit == null || commit.isEmpty() ? "unknown" : commit; }
    static String setupFormat(int setupVersion, String route) {
        return "Install format " + setupVersion + (route.isEmpty() ? "" : " · " + route + " route") + " · game 1.4.3";
    }
    static String library(boolean present) { return "Setup library " + (present ? "present" : "missing"); }
    static String doneIn(long seconds) { return seconds <= 0 ? DONE : DONE + " · " + (seconds < 120 ? seconds + " s" : (seconds + 30) / 60 + " min"); }
}

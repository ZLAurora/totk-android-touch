package com.aure.totk.setup;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.text.SpannableString;
import android.text.TextUtils;
import android.text.style.UnderlineSpan;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import com.aure.totk.GameLaunch;
import com.aure.totk.GameControlService;
import com.aure.totk.UpdateRules;
import com.aure.totk.Updates;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Launcher entry (process ":setup"): the pre-game menu once the game is installed (shown at every launch unless
 * "Open the game directly next time" is on), otherwise the setup flow. The game starts only when install.json is playable
 * (the shader seed included) and the seed on disk is usable; an install without one gets Build shaders on Home instead.
 * It writes <private>/settings.env for the game (TotkActivity, default process) on every settings change and right
 * before starting it.
 * Built entirely in code: the APK has no resource table, so there are no XML layouts or R class. All screens are laid
 * out on the mockups' 1920 x 1080 design grid (Widgets.Board). */
public final class SetupActivity extends Activity implements SetupService.Listener {
    static final String EXTRA_SETTINGS = "com.aure.totk.setup.SETTINGS";
    /** Why the game process stopped (TotkActivity.showGameError, just before it aborted): Home says so until the next start. */
    public static final String EXTRA_GAME_ERROR = "com.aure.totk.setup.GAME_ERROR";
    public static final String EXTRA_UPDATE = "com.aure.totk.setup.UPDATE";
    static final String EXTRA_UPDATE_AUTH = "com.aure.totk.setup.UPDATE_AUTH";
    private static final String[] ALL_ROLES = {"romfs", "exefs", "base", "update", "keys"};
    private static final int REQ_PICK = 100, REQ_DRIVER = 900, REQ_NOTIFY = 901, REQ_EXPORT = 902, REQ_IMPORT_ZIP = 903, REQ_IMPORT_TREE = 904, REQ_MIRROR = 905,
            REQ_DATA_FOLDER = 906, REQ_ACCESS = 907;
    private static final String[] TABS = {Settings.PERFORMANCE, Settings.PICTURE, Settings.SCREEN};

    private enum Screen { WELCOME, SETUP_STORAGE, ROUTE, FILES, UPDATES, UPDATE_DOWNLOAD, INSTALL, ERROR, INTERRUPTED, HOME, GRAPHICS, CONTROLS, DATA, STORAGE, ABOUT, LICENSES,
        LICENSE_TEXT, CONFIRM_DELETE, SAVE_ERROR, SAVES, BACKUPS, SAVE_IMPORT, SAVE_CONFLICT, CHOICE }

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    /** Save operations (export, import, backups, the save folder) run in order on their own thread, so a slow folder
     * never holds up the file checks of setup. */
    private final ExecutorService saveWorker = Executors.newSingleThreadExecutor();
    private final ArrayDeque<Screen> history = new ArrayDeque<>();
    private Screen screen;
    private Inputs inputs;
    private Settings settings;
    private String tab = Settings.PERFORMANCE;
    /** Graphics opened as setup's last step ("Choose graphics"). */
    private boolean setupFinished;
    private boolean ui, playAfterStop, busy;
    private boolean sharingDetails;
    private String note = "", licence = "";
    private String gameError;
    /** D-pad focus to restore when navigating back to a screen (by view tag). */
    private final Map<Screen, Object> focusMemory = new HashMap<>();
    private Object restoreFocus;
    /** Sentinel for restoreFocus: a newly opened screen starts on its primary control, not a same-tagged one. */
    private static final Object PRIMARY = new Object();
    private Runnable afterPermission;
    /** Draws Home / Game data again when a size walk ends or its time runs out (see dataBytes()). */
    private final Runnable sizeChanged = () -> { if (ui && !isDestroyed() && (screen == Screen.HOME || screen == Screen.DATA)) render(); };
    private final Runnable sizeKnown = () -> main.post(sizeChanged);

    // Saves (SaveManager / SaveMirror)
    /** Slots of the live save set and their pictures, loaded in the background; null while unknown. */
    private List<SaveManager.Slot> liveSlots;
    /** Decoded caption pictures by file path + modification time (Boolean.FALSE: the file has none); filled on saveWorker. */
    private final Map<String, Object> pictures = new java.util.concurrent.ConcurrentHashMap<>();
    private List<SaveManager.Backup> backupList = new ArrayList<>();
    private SaveManager.Backup chosenBackup;
    /** A checked import waiting for confirmation: its staged tree, slots and where it came from. */
    private List<SaveManager.Slot> importSlots = new ArrayList<>();
    private File importStaged;
    private String importSource = "";
    private int importFiles;
    private SaveMirror.Result conflict;
    private boolean saveBusy, syncing, startAfterSync, pulledChip;
    /** migrationAsked: Home asked about the move to private storage on this visit. folderForSetup / folderMoving: what a
     * game-data folder pick (REQ_DATA_FOLDER) is for (setup's Storage step, or a move of installed data). */
    private boolean migrationAsked, folderForSetup, folderMoving;
    private long folderNeed;
    /** The CHOICE screen: a small confirmation or pick-one dialog. */
    private String choiceIcon = "save", choiceHeader = "", choiceTitle = "", choiceBody = "";
    private final List<Object[]> choiceOptions = new ArrayList<>();   // {tag, label, hint, warn?, Runnable}
    private Widgets.SlotGrid slotGrid;
    private TextView slotLine;

    // Live views of the current screen, updated in place rather than rebuilt.
    private Widgets.Board board;
    private View primary;
    private final Map<String, Widgets.Row> settingRows = new LinkedHashMap<>();
    private final Map<String, Widgets.PresetCard> presetCards = new LinkedHashMap<>();
    private final Map<String, Widgets.Card> slotCards = new HashMap<>();
    private final Map<String, Widgets.StageRow> stageRows = new HashMap<>();
    private final Map<String, Widgets.Row> dataRows = new HashMap<>();
    private Widgets.Chip statusChip;
    private TextView statusLine, storageText, extrasNote;
    /** note is a confirmation (green dot), not a warning. */
    private boolean noteOk;
    /** Why the last settings.env write before starting the game failed. */
    private String envError = "";
    /** seedRefused: a start was refused because the shader seed on disk is missing or empty (Home then offers Build
     * shaders). homeAfterBuild: the running shader build came from Home's Build shaders, so it returns there.
     * rebuildAgain: the single stage on the install screen replaces output that is already there. artUpdate: that stage
     * replaces second-screen art from an older generator. artUpdateStarted: this menu visit already started that update
     * (Play now or a cancel brings Home back without starting it again until the next launch). */
    private boolean seedRefused, homeAfterBuild, rebuildAgain, artUpdate, artUpdateStarted;
    private Widgets.StorageBar storageBar;
    private Widgets.Ring ring;
    private Widgets.Action installAction;
    private Widgets.Prompt installPrompt, cancelPrompt, playPrompt, otherPrompt;
    private Widgets.Toggle skipToggle;
    private LinearLayout detailFacts;
    private TextView detailTitle, detailText, detailConstraint;
    private Diagram detailDiagram;
    private View detailNote;
    private String detailId = "";
    private long cancelArmedAt;
    private JSONObject inspection;
    private boolean inspecting;
    private int inspectGeneration;
    private InstallState installSnapshot;
    private String snapshotStage = "";
    private long snapshotAt;
    private GameLock.Result gameState = new GameLock.Result(GameLock.State.FREE, "");
    private final List<View> gameControls = new ArrayList<>();
    private Runnable blockedAction;
    private String blockedNeed = "Installing";
    private boolean closingGame, closeSent;
    private long closeDeadline, closeStartedAt, gameCheckedAt;
    private String closeId = "";
    private ServiceConnection closeConnection;
    private TextView gameNotice;
    private boolean started, uiStarted;
    private Widgets.Action closeGameAction;
    private UpdateInstaller updater;
    private TextView updateProgress;
    private boolean pendingAsked;

    private String pendingUpdate() { return UpdateRules.pending(getFilesDir(), Updates.version(this)); }
    private void offerPendingUpdate() {
        if (pendingAsked || screen != Screen.HOME || getIntent().getBooleanExtra(EXTRA_UPDATE, false)) return;
        String version = pendingUpdate();
        if (version == null) return;
        pendingAsked = true;
        openChoice("about", Strings.UPDATES, Strings.updateAvailable(version), Strings.UPDATE_MENU_BODY,
                new Object[]{"update:now", "Update", Strings.UPDATE_CLOSE_HINT, false, (Runnable) this::startAppUpdate},
                new Object[]{"update:later", "Not now", "Keep this update for next launch", false, (Runnable) () -> replace(Screen.HOME)});
    }
    private void requestedUpdate() {
        if (!started || !getIntent().getBooleanExtra(EXTRA_UPDATE, false)) return;
        String authorization = getIntent().getStringExtra(EXTRA_UPDATE_AUTH);
        getIntent().removeExtra(EXTRA_UPDATE);
        getIntent().removeExtra(EXTRA_UPDATE_AUTH);
        if (UpdateAuthorization.consume(authorization)) {
            pendingAsked = true;
            startAppUpdate();
        } else if (updater == null && !SetupService.status.running && pendingUpdate() != null) {
            // This activity is exported. An external UPDATE intent is only a request to show the user's choice.
            pendingAsked = false;
            if (screen != Screen.HOME) replace(Screen.HOME);
            offerPendingUpdate();
        }
    }
    private void startAppUpdate() {
        String version = pendingUpdate();
        if (version == null || updater != null) return;
        if (SetupService.status.running) { showNote("Finish preparing the game before updating the app."); return; }
        if (!needGameClosed("Updating the app", this::startAppUpdate)) { closeGame(); return; }
        updater = new UpdateInstaller(this, new UpdateInstaller.Listener() {
            @Override public void changed() { if (!isDestroyed()) replace(Screen.UPDATE_DOWNLOAD); }
            @Override public void permission() {
                openChoice("about", Strings.UPDATES, Strings.UPDATE_ALLOW, Strings.UPDATE_ALLOW_BODY,
                        new Object[]{"update:allow", "Open Android settings", "Allow from this source, then return", false, (Runnable) () -> updater.openPermission()},
                        new Object[]{"update:cancel", "Not now", "Keep the update for next launch", false, (Runnable) () -> updater.cancel()});
            }
            @Override public void finished(String message) { finishAppUpdate(message); }
        });
        updater.begin(version);
    }
    private void finishAppUpdate(String message) {
        if (updater != null) { updater.dispose(); updater = null; }
        replace(InstallState.read(this).installed() ? Screen.HOME : Screen.WELCOME);
        note = message; noteOk = false; render();
    }
    private void updateInstallResult() {
        String message = UpdateReceiver.takeResult(this);
        if (!message.isEmpty()) finishAppUpdate(message);
    }
    private Widgets.Row updateSwitch(LinearLayout parent) {
        Widgets.Row row = new Widgets.Row(this, Strings.UPDATE_CHECK, "On by default · no uploads or automatic installation", 26, 18);
        row.setTag("update:checks");
        Widgets.Toggle toggle = new Widgets.Toggle(this);
        toggle.set(UpdateRules.enabled(getFilesDir()));
        LinearLayout value = Widgets.row(this);
        TextView state = Theme.text(this, UpdateRules.enabled(getFilesDir()) ? "On" : "Off", false, 22, Theme.TEXT2);
        value.addView(state, Widgets.margins(Widgets.size(-2, -2), 0, 0, 18, 0));
        value.addView(toggle, Widgets.size(58, 30));
        row.value(value);
        Runnable change = () -> {
            try {
                UpdateRules.enabled(getFilesDir(), !UpdateRules.enabled(getFilesDir()));
                boolean on = UpdateRules.enabled(getFilesDir());
                toggle.set(on); state.setText(on ? "On" : "Off");
            }
            catch (IOException failed) { showNote("Could not save the update setting: " + failed.getMessage()); }
        };
        row.setOnClickListener(v -> change.run());
        row.adjust(direction -> change.run());
        parent.addView(row, Widgets.rowParams(108, 18));
        return row;
    }
    private void updates() {
        setupHeader(Strings.SETUP_TITLE); rail(3, false);
        Widgets.Panel box = setupMain();
        heading(box, Strings.UPDATES, 36);
        lead(box, Strings.UPDATE_PRIVACY, 20);
        primary = updateSwitch(box);
        Widgets.Action next = new Widgets.Action(this, "Prepare", true, v -> requestNotificationsThen(() -> startSetup(null)));
        board.put(next, 1790, 880, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.END, Widgets.Board.END);
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt(), prompt("+", "Prepare", v -> next.performClick()));
    }
    private void appUpdateDownload() {
        header("about", Strings.UPDATES, 0, null);
        Widgets.Panel box = panel(340, 240, 1240, 560, true, 50, 44);
        heading(box, updater == null ? Strings.UPDATES : "TotK Native " + updater.version, 36);
        updateProgress = Theme.paragraph(this, "", 26, Theme.TEXT2);
        box.addView(updateProgress, Widgets.margins(Widgets.size(-1, -2), 0, 32, 0, 32));
        updateDownloadProgress();
        Widgets.Row cancel = new Widgets.Row(this, "Cancel", "Your installed app, saves and game data are unchanged", 27, 18);
        cancel.setOnClickListener(v -> { if (updater != null) updater.cancel(); });
        box.addView(cancel, Widgets.rowParams(100, 0));
        primary = cancel;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), prompt("B", Strings.P_CANCEL, v -> { if (updater != null) updater.cancel(); }));
    }
    private void updateDownloadProgress() {
        if (updater != null && updateProgress != null) updateProgress.setText(updater.status + "\n\n"
                + Strings.bytes(updater.bytes) + (updater.total > 0 ? " / " + Strings.bytes(updater.total) + " · " + (updater.bytes * 100 / updater.total) + "%" : ""));
    }

    /** Gated controls remain focusable on handhelds; activation records the intended retry, never the mutation. */
    private void gameControl(View view, String need, Runnable retry) {
        if (gameState.free()) {
            if (gameControls.contains(view)) {
                if (view instanceof Widgets.Row) ((Widgets.Row) view).dimmed(false);
                else view.setAlpha(1);
            }
            return;
        }
        if (!gameControls.contains(view)) {
            if (view instanceof Widgets.Row && ((Widgets.Row) view).dimmed() || !view.isEnabled()) return;
            gameControls.add(view);
            View.OnFocusChangeListener previous = view.getOnFocusChangeListener();
            view.setOnFocusChangeListener((v, has) -> {
                if (previous != null) previous.onFocusChange(v, has);
                if (has && !gameState.free()) { blockedAction = retry; blockedNeed = need; updateGameNotice(); }
            });
            view.setOnClickListener(v -> {
                if (!closingGame && needGameClosed(need, retry)) retry.run();
            });
        }
        if (view instanceof Widgets.Row) ((Widgets.Row) view).dimmed(true);
        else view.setAlpha(.42f);
    }
    private boolean needGameClosed(String need, Runnable retry) {
        gameState = InstallState.gameState(this);
        if (gameState.free()) return true;
        blockedNeed = need; blockedAction = retry;
        render();
        return false;
    }
    private void gameBanner() {
        if ((gameControls.isEmpty() && blockedAction == null) || gameState.free()) return;
        if (blockedAction == null) blockedNeed = screen == Screen.FILES || screen == Screen.ERROR || screen == Screen.INTERRUPTED ? "Installing"
                : screen == Screen.STORAGE ? "Moving game data" : screen == Screen.DATA ? "Changing game data" : "Changing saves";
        gameNotice = Theme.paragraph(this, "", 18, Theme.AMBER);
        board.put(gameNotice, 80, 930, 1360, 70);
        closeGameAction = new Widgets.Action(this, Strings.CLOSE_GAME, false, v -> closeGame());
        closeGameAction.setTag("close-game");
        board.put(closeGameAction, 1840, 930, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.END, Widgets.Board.START);
        if (blockedAction == null && screen == Screen.FILES) { blockedAction = () -> go(Screen.UPDATES); blockedNeed = "Installing"; }
        updateGameNotice();
    }
    private void updateGameNotice() {
        if (gameNotice != null) gameNotice.setText(closingGame ? (closeSent ? Strings.CLOSING_GAME : Strings.WAITING_SAVE)
                : gameState.state == GameLock.State.ERROR ? Strings.gameLockError(gameState.reason) : Strings.gameRunning(blockedNeed));
        if (closeGameAction != null) {
            closeGameAction.setVisibility(gameState.state == GameLock.State.GAME ? View.VISIBLE : View.GONE);
            closeGameAction.setEnabled(!closingGame);
        }
    }
    private void refreshGameState() {
        GameLock.Result next = InstallState.gameState(this);
        boolean changed = next.state != gameState.state || next.state == GameLock.State.ERROR && !next.reason.equals(gameState.reason);
        gameState = next;
        if (changed && ui && screen != Screen.INSTALL) {
            if (next.free() && !closingGame) blockedAction = null;
            render();
        }
    }
    /** A foreground binding thaws a cached game before waiting for its save. Only the lock decides whether it closed;
     * binding can create an empty default process if the game dies between the check and bindService. */
    private void closeGame() {
        if (closingGame || !started) return;
        closingGame = true; closeSent = false;
        closeStartedAt = SystemClock.uptimeMillis();
        closeDeadline = closeStartedAt + 30_000;
        closeId = android.os.Process.myPid() + ":" + System.nanoTime();
        gameState = InstallState.gameState(this);
        if (gameState.state == GameLock.State.GAME) {
            closeConnection = new ServiceConnection() {
                private void changed() {
                    if (closeConnection != this || !closingGame) return;
                    main.removeCallbacks(closePoll);
                    main.post(closePoll);
                }
                @Override public void onServiceConnected(ComponentName name, IBinder service) { changed(); }
                // Android reports remote binder death here. A disconnect alone is not proof that the lock is free.
                @Override public void onServiceDisconnected(ComponentName name) { changed(); }
                @Override public void onBindingDied(ComponentName name) { changed(); }
                @Override public void onNullBinding(ComponentName name) { changed(); }
            };
            try {
                // The ordinary binding inherits the visible client's importance; no ABOVE_CLIENT/IMPORTANT, activity
                // start, foreground service or notification is needed, and the service never starts the runtime.
                if (!bindService(new Intent(this, GameControlService.class), closeConnection, BIND_AUTO_CREATE)) {
                    closeFailed(Strings.gameLockError("Could not bind the game control service"));
                    return;
                }
            } catch (RuntimeException failed) { closeFailed(Strings.gameLockError(failed.toString())); return; }
        }
        updateGameNotice();
        main.post(closePoll);
    }
    private void releaseCloseBinding() {
        ServiceConnection connection = closeConnection;
        closeConnection = null;
        if (connection != null) {
            try { unbindService(connection); }
            catch (IllegalArgumentException absent) { /* A refused/failed bind may not have registered the connection. */ }
        }
    }
    /** Stopping/recreating the menu abandons the pending action, not the game's save-safe native close protocol. */
    private void cancelClose() {
        main.removeCallbacks(closePoll);
        if (closingGame) {
            try { GameLock.clearClose(getFilesDir(), closeId); }
            catch (IOException failed) { android.util.Log.w("TotkSetup", "close game: request cleanup failed", failed); }
            closingGame = false; blockedAction = null;
        }
        releaseCloseBinding();
        updateGameNotice();
    }
    private final Runnable closePoll = new Runnable() {
        @Override public void run() {
            if (!closingGame || !started || isDestroyed()) return;
            GameLock.Result state = InstallState.gameState(SetupActivity.this);
            if (state.free()) {
                try { GameLock.clearClose(getFilesDir(), closeId); }
                catch (IOException failed) { closeFailed(Strings.gameLockError(failed.toString())); return; }
                Runnable retry = blockedAction;
                closingGame = false; blockedAction = null; gameState = state;
                main.removeCallbacks(closePoll);
                releaseCloseBinding();
                render();
                if (retry != null) retry.run();
                return;
            }
            if (state.state == GameLock.State.ERROR) { closeFailed(Strings.gameLockError(state.reason)); return; }
            boolean saving;
            try {
                java.nio.file.Files.readAttributes(new File(getFilesDir(), "state/save-busy").toPath(), java.nio.file.attribute.BasicFileAttributes.class);
                saving = true;
            } catch (java.nio.file.NoSuchFileException idle) { saving = false; }
            catch (IOException | SecurityException failed) { closeFailed(Strings.gameLockError(failed.toString())); return; }
            try {
                String response = GameLock.closeResponse(getFilesDir(), closeId);
                if (response.equals("saving")) { GameLock.clearClose(getFilesDir(), closeId); closeSent = false; saving = true; }
                else if (response.startsWith("error:")) { closeFailed(Strings.gameLockError(response)); return; }
            } catch (IOException | SecurityException failed) { closeFailed(Strings.gameLockError(failed.toString())); return; }
            if (SystemClock.uptimeMillis() >= closeDeadline) { closeFailed(saving ? Strings.SAVE_WAIT_TIMEOUT : Strings.CLOSE_GAME_TIMEOUT); return; }
            if (!saving && !closeSent) {
                try {
                    GameLock.requestClose(getFilesDir(), closeId);
                    closeSent = true;
                    closeDeadline = Math.min(SystemClock.uptimeMillis() + 35_000, closeStartedAt + 65_000);
                } catch (IOException | RuntimeException failed) { closeFailed(Strings.gameLockError(failed.toString())); return; }
            }
            updateGameNotice();
            main.postDelayed(this, 200);
        }
    };
    private void closeFailed(String message) {
        try { GameLock.clearClose(getFilesDir(), closeId); }
        catch (IOException failed) { android.util.Log.w("TotkSetup", "close game: request cleanup failed", failed); }
        closingGame = false; blockedAction = null;
        main.removeCallbacks(closePoll);
        releaseCloseBinding();
        note = message; noteOk = false;
        render();
        if (gameNotice != null) gameNotice.setText(message);
    }

    // ---------- lifecycle ----------
    private boolean settingsMoving;
    private static final String EXTRA_COLD_SETTINGS = "com.aure.totk.setup.COLD_SETTINGS_HANDOFF";
    private static final String EXTRA_GAME_TASK = "com.aure.totk.setup.IN_GAME_TASK";
    private static java.lang.ref.WeakReference<SetupActivity> menuActivity = new java.lang.ref.WeakReference<>(null);
    private ActivityManager.AppTask menuTask;
    /** A game-rooted task may still contain the menu after native exit. Cross-display launcher matching
     * cannot find that menu by base Intent, so forward to its exact task before drawing another one. */
    private boolean reuseMenuTask(Intent intent) {
        if (!SettingsStore.secondaryDisplay(this)) return false;
        try {
            ActivityManager manager = getSystemService(ActivityManager.class);
            if (manager == null) return false;
            // Cache our task handle while it is visible in recents. Android 16 hides the old same-affinity
            // task from getAppTasks as soon as a cross-display launcher creates the undrawn new entry.
            for (ActivityManager.AppTask task : manager.getAppTasks())
                if (task.getTaskInfo().taskId == getTaskId()) { menuTask = task; break; }
            SetupActivity previous = menuActivity.get();
            if (previous != null && previous != this && !previous.isFinishing() && !previous.isDestroyed()
                    && previous.menuTask != null) {
                previous.startActivity(new Intent(intent)
                                .setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT),
                        ActivityOptions.makeBasic().setLaunchDisplayId(android.view.Display.DEFAULT_DISPLAY).toBundle());
                previous.menuTask.moveToFront();
                finish();
                overridePendingTransition(0, 0);
                return true;
            }
            menuActivity = new java.lang.ref.WeakReference<>(this);
        } catch (RuntimeException failed) {
            android.util.Log.w("TotkSetup", "Could not return to the existing menu task", failed);
        }
        return false;
    }
    /** Keep the Settings shortcut visible, rather than underneath the game's companion on the bottom panel.
     * Bind the start to this task: a normal cross-display search cannot find a menu above the singleTask game. */
    private boolean moveSettingsToMain(Intent intent) {
        if (!intent.getBooleanExtra(EXTRA_SETTINGS, false) || closingGame || !SettingsStore.secondaryDisplay(this)) return false;
        if (getDisplay() == null || getDisplay().getDisplayId() == android.view.Display.DEFAULT_DISPLAY) {
            settingsMoving = false;
            return false;
        }
        if (settingsMoving) return false;
        try {
            settingsMoving = true;
            ActivityManager manager = getSystemService(ActivityManager.class);
            if (manager == null) throw new IllegalStateException("Activity manager unavailable");
            for (ActivityManager.AppTask task : manager.getAppTasks()) {
                if (task.getTaskInfo().taskId == getTaskId()) {
                    task.startActivity(this,
                            new Intent(intent).putExtra(EXTRA_COLD_SETTINGS, !ui)
                                    .setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                            ActivityOptions.makeBasic().setLaunchDisplayId(android.view.Display.DEFAULT_DISPLAY).toBundle());
                    return true;
                }
            }
            throw new IllegalStateException("Settings task unavailable");
        } catch (RuntimeException failed) {
            settingsMoving = false;
            android.util.Log.w("TotkSetup", "Could not move Settings to the main display", failed);
            return false;
        }
    }
    /** AppTask can move the task before the Activity receives its new display configuration or Intent. */
    private void completeSettingsMove() {
        if (!settingsMoving || getDisplay() == null
                || getDisplay().getDisplayId() != android.view.Display.DEFAULT_DISPLAY) return;
        settingsMoving = false;
        if (!ui) showUi();
    }
    /** A launcher tap returns to a live game; the explicit Settings VIEW shortcut always remains a menu entry.
     * This is a direct task return, not another startup/save sync, and must not cancel a pending safe close. */
    private boolean returnToRunningGame(Intent intent) {
        if (closingGame || !Intent.ACTION_MAIN.equals(intent.getAction()) || !intent.hasCategory(Intent.CATEGORY_LAUNCHER)
                || intent.getBooleanExtra(EXTRA_SETTINGS, false) || intent.hasExtra(EXTRA_GAME_ERROR) || intent.hasExtra(EXTRA_UPDATE)
                || pendingUpdate() != null || !SettingsStore.secondaryDisplay(this)
                || InstallState.gameState(this).state != GameLock.State.GAME) return false;
        try {
            GameLaunch.start(this);
            finish();
            overridePendingTransition(0, 0);
            return true;
        } catch (RuntimeException failed) {
            android.util.Log.w("TotkSetup", "Could not return to the running game", failed);
            return false;
        }
    }
    /** Called only by the live, main-display game: its caller token keeps Settings in that same task. */
    public static void openOnGame(android.app.Activity game) {
        game.startActivity(new Intent(game, SetupActivity.class).setAction(Intent.ACTION_VIEW)
                .putExtra(EXTRA_SETTINGS, true).putExtra(EXTRA_GAME_TASK, true));
    }
    private boolean settingsOverGame(Intent intent) {
        if (!intent.getBooleanExtra(EXTRA_SETTINGS, false) || intent.getBooleanExtra(EXTRA_GAME_TASK, false)
                || !SettingsStore.secondaryDisplay(this)
                || InstallState.gameState(this).state != GameLock.State.GAME) return false;
        GameLaunch.startSettings(this);
        finish();
        overridePendingTransition(0, 0);
        return true;
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        boolean settingsShortcut = getIntent().getBooleanExtra(EXTRA_SETTINGS, false);
        gameError = getIntent().getStringExtra(EXTRA_GAME_ERROR);
        if (reuseMenuTask(getIntent())) return;
        if (settingsOverGame(getIntent())) return;
        if (moveSettingsToMain(getIntent())) return;
        if (returnToRunningGame(getIntent())) return;
        // After a game error the menu is shown (with the reason) even when it is set to start the game directly.
        if (gameError == null && !settingsShortcut && !getIntent().getBooleanExtra(EXTRA_UPDATE, false) && pendingUpdate() == null
                && !SetupService.status.running && !SetupService.jobActive(this) && InstallState.read(this).playable()
                && SettingsStore.skipMenu(this) && StorageLocation.available(this) && !StorageLocation.migrationPending(this, true)) {
            // translucent theme + no content: nothing flashes before the game. A save folder is synced first (newer
            // saves from it are brought in); a conflict needs the player, so it opens the menu instead.
            if (!SaveMirror.enabled(this)) { launchGame(); return; }
            saveWorker.execute(() -> {
                SaveMirror.Result r = SaveMirror.sync(this, true);
                main.post(() -> {
                    if (!SaveMirror.CONFLICT.equals(r.action)) { launchGame(); return; }
                    showUi();
                    openConflict(r);
                });
            });
            return;
        }
        showUi();
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        boolean hadUi = ui && !intent.getBooleanExtra(EXTRA_COLD_SETTINGS, false);
        setIntent(intent);
        if (moveSettingsToMain(intent)) return;
        if (!ui && intent.getBooleanExtra(EXTRA_SETTINGS, false)) showUi();
        if (returnToRunningGame(intent)) return;
        if (hadUi && intent.getBooleanExtra(EXTRA_SETTINGS, false) && screen != Screen.INSTALL
                && !closingGame && blockedAction == null)
            replace(InstallState.read(this).installed() ? Screen.HOME : Screen.WELCOME);
        String error = intent.getStringExtra(EXTRA_GAME_ERROR);
        if (error != null) {
            gameError = error;
            if (!ui) showUi(); else if (screen == Screen.HOME) render();
        }
        if (intent.getBooleanExtra(EXTRA_UPDATE, false)) { if (!ui) showUi(); requestedUpdate(); }
        else if (Intent.ACTION_MAIN.equals(intent.getAction()) && ui) { pendingAsked = false; offerPendingUpdate(); }
    }
    @Override protected void onStart() {
        super.onStart();
        started = true;
        startUi();
        requestedUpdate();
    }
    /** showUi can finish after onStart (a save-folder conflict while skipping the menu). Register exactly once. */
    private void startUi() {
        if (!started || !ui || uiStarted) return;
        uiStarted = true;
        SetupService.listen(this);
        onStatus();
        main.postDelayed(refresh, REFRESH_MS);
        // Back on the menu (app launch or return from elsewhere, the game closed): bring the save folder up to date.
        if (screen == Screen.HOME || screen == Screen.SAVES || screen == Screen.WELCOME) syncMirror(false);
        // All files access may have been switched on or off in Settings meanwhile (Azahar re-checks it on resume too).
        if (screen == Screen.HOME || screen == Screen.DATA || screen == Screen.STORAGE || screen == Screen.SETUP_STORAGE) render();
        StorageManager storage = getSystemService(StorageManager.class);
        if (storage != null) storage.registerStorageVolumeCallback(getMainExecutor(), volumeChanged);
    }
    @Override protected void onResume() {
        super.onResume();
        completeSettingsMove();
        if (ui) refreshGameState();
        if (ui) updateInstallResult();
    }
    @Override protected void onStop() {
        started = false;
        cancelClose();
        if (uiStarted) {
            uiStarted = false;
            StorageManager storage = getSystemService(StorageManager.class);
            if (storage != null) storage.unregisterStorageVolumeCallback(volumeChanged);
            SetupService.unlisten(this);
            main.removeCallbacks(refresh);
        }
        super.onStop();
    }
    /** A card mounted, unmounted or removed while the menu is visible: the screens that show where the game data is
     * (Home's missing-card state and Start game, Game data, the storage choices) are rebuilt from the new state. */
    private final StorageManager.StorageVolumeCallback volumeChanged = new StorageManager.StorageVolumeCallback() {
        @Override public void onStateChanged(StorageVolume volume) {
            if (!ui) return;
            DataSize.invalidate();
            if (screen == Screen.HOME || screen == Screen.DATA || screen == Screen.STORAGE || screen == Screen.SETUP_STORAGE
                    || screen == Screen.WELCOME) render();
        }
    };
    /** The install screen also redraws from the service state on a timer, so a missed or late status callback can never
     * leave a stale stage on screen for the rest of the run. */
    private static final long REFRESH_MS = 500;
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            if (!uiStarted) return;
            if (screen == Screen.INSTALL) onStatus();
            if (screen == Screen.UPDATE_DOWNLOAD) updateDownloadProgress();
            if (updater != null) updateInstallResult();
            if (cancelArmedAt != 0 && SystemClock.uptimeMillis() - cancelArmedAt > 3000) disarmCancel();
            if (SystemClock.uptimeMillis() - gameCheckedAt >= 1000) {
                gameCheckedAt = SystemClock.uptimeMillis();
                refreshGameState();
            }
            if (uiStarted) main.postDelayed(this, REFRESH_MS);
        }
    };
    @Override protected void onDestroy() {
        cancelClose();
        if (updater != null) updater.dispose();
        worker.shutdownNow();
        saveWorker.shutdown();   // never interrupted: an upload or swap in progress finishes (FileChannels close on interrupt)
        super.onDestroy();
    }
    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        completeSettingsMove();
        if (ui) render();
    }

    private void showUi() {
        ui = true;
        setTranslucent(false);
        Theme.init(this);
        Window window = getWindow();
        window.setBackgroundDrawable(new ColorDrawable(Theme.INK));
        window.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setDecorFitsSystemWindows(false);
        window.getDecorView();  // installs the decor; getInsetsController() is null-unsafe before that
        WindowInsetsController bars = window.getInsetsController();
        if (bars != null) {
            bars.hide(WindowInsets.Type.systemBars());
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        inputs = Inputs.load(this);
        UpdateReceiver.clean(this);
        settings = SettingsStore.load(this);
        publishShortcut();
        InstallState install = InstallState.read(this);
        JSONObject result = SetupService.lastResult(this);
        String moveNote = null;
        boolean moveOk = false;
        if (!SetupService.status.running && SetupService.jobActive(this) && SetupService.moveJob(this)) {
            // A move never resumes: it was copying beside the untouched source, which is still in use.
            SetupService.clearJob(this);
            moveNote = Strings.moveInterrupted(StorageLocation.label(this));
            repairInterruptedMove();
        } else if (!SetupService.status.running && result.has("ok") && "move".equals(result.optString("stage"))) {
            // A move that finished while the menu was closed: say how it went once.
            SetupService.clearResult(this);
            moveOk = result.optBoolean("ok");
            moveNote = moveOk ? Strings.moved(result.optString("name")) : Strings.moveFailed(result.optString("code"), StorageLocation.label(this));
            result = new JSONObject();
        }
        if (SetupService.status.running) screen = Screen.INSTALL;
        else if (result.has("ok") && Inputs.prefs(this).getBoolean(ArtPolicy.START_AFTER_BUILD, false)) screen = Screen.INSTALL;
        else if (moveNote != null) { history.push(install.installed() ? Screen.HOME : Screen.WELCOME); screen = Screen.DATA; }
        else if (SetupService.jobActive(this)) screen = Screen.INTERRUPTED;
        // A stopped second-screen art stage on an installed game is not a reason to keep the player away from the menu on
        // every launch: Game data shows its state and its Build/Rebuild row. Every other failure, the shader stage's
        // included (the game draws nothing without the seed), stays on the error screen with Resume.
        else if (result.has("ok") && !result.optBoolean("ok") && install.installed()
                && InstallState.OPTIONAL.equals(result.optString("stage"))) { SetupService.clearResult(this); screen = Screen.HOME; }
        else if (result.has("ok") && !result.optBoolean("ok")) screen = Screen.ERROR;
        // An install whose shaders were never built (cancelled by an earlier version's Play now) opens Home, which
        // offers Build shaders in place of Start game.
        else if (install.installed() || !StorageLocation.available(this)) screen = Screen.HOME;
        else if (!inputs.route.isEmpty() && !inputs.items.isEmpty()) { history.push(Screen.WELCOME); history.push(Screen.ROUTE); screen = Screen.FILES; }
        else screen = Screen.WELCOME;
        loadSaves();
        if (screen == Screen.HOME && pendingUpdate() == null) updateOutdatedArt(install);
        pulledChip = SaveMirror.takePulled(this);
        if (moveNote != null) { note = moveNote; noteOk = moveOk; }
        if (screen == Screen.INSTALL && !SetupService.status.running && result.has("ok")) main.post(this::finished);
        render();
        startUi();
        offerPendingUpdate();
        updateInstallResult();
    }
    /** A move killed after its copy was committed (the folder choice switched or not, settings.env written or not, the
     * old copy partly removed) is made consistent again: settings.env is rewritten from the folder choice, and when the
     * chosen folder is playable, setup's leftovers in the other places it may have used (an unused complete copy, a
     * half-removed old one, a .move staging folder) are removed. Saves are never touched (they are private). Skipped
     * while the game or another move holds the game lock. */
    private void repairInterruptedMove() {
        String envFailed = SettingsStore.writeEnv(this, settings);
        if (envFailed != null) android.util.Log.w("TotkSetup", "interrupted move: settings.env not rewritten: " + envFailed);
        worker.execute(() -> {
            if (!StorageLocation.available(this) || !InstallState.read(this).installed() || !InstallState.libraryAvailable()) return;
            File game = StorageLocation.gameDir(this);
            for (File dir : StorageLocation.knownFolders(this)) {
                if (dir.getPath().equals(game.getPath()) || !StorageLocation.holdsGameData(dir)) continue;
                try (GameLock.Hold held = GameLock.hold(getFilesDir())) {
                    if (!held.result.free()) return;
                    android.util.Log.i("TotkSetup", "interrupted move: removing the unused game data in " + dir + ": "
                            + NativeSetup.removeGameData(dir.getPath()));
                } catch (IOException | RuntimeException | LinkageError failed) {
                    android.util.Log.w("TotkSetup", "interrupted move: " + dir + " not cleaned: " + failed);
                }
            }
        });
    }
    /** Second-screen art from an older generator (an app update changed item names or icons; InstallState.artOutdated) is
     * made again on its own when Home opens, once per launch: the companion stage is optional, so Play now on the install
     * screen stops it and starts the game with the current art, and a failed update stays on Game data's Update row. */
    private void updateOutdatedArt(InstallState install) {
        if (artUpdateStarted || !install.playable() || !ArtPolicy.updateAutomatically(SettingsStore.secondaryDisplay(this), install.artState())
                || !install.errorCode(InstallState.OPTIONAL).isEmpty()
                || !InstallState.libraryAvailable() || SetupService.status.running || SetupService.jobActive(this)
                || !StorageLocation.available(this) || StorageLocation.migrationPending(this, true) || !InstallState.gameState(this).free()) return;
        artUpdateStarted = true;
        main.post(() -> { if (screen == Screen.HOME && !SetupService.status.running) buildStage(InstallState.OPTIONAL, true); });
    }

    /** settings.env is rewritten right before the game starts, so it always matches the menu (and the device language).
     * If that write fails the game is not started silently with stale settings: the player chooses Retry or Start
     * anyway (which keeps whatever settings.env the last successful write left, or none = shipping defaults). A missing
     * or empty shader seed is refused first: the game would only show a black screen (the runtime logs "shader seed
     * error"), so Home says so and offers Build shaders instead. */
    private void launchGame() { launchGame(false); }
    private void launchGame(boolean anyway) {
        if (!InstallState.read(this).seedUsable()) {
            seedRefused = true;
            if (!ui) showUi();
            replace(Screen.HOME);
            return;
        }
        if (ArtPolicy.offerBeforePlay(SettingsStore.secondaryDisplay(this), InstallState.read(this).artState(),
                Inputs.prefs(this).getBoolean(ArtPolicy.DECLINED, false))) {
            if (!ui) showUi();
            openChoice("data", Strings.COMPANION_ROW, Strings.ART_OFFER_TITLE, Strings.ART_OFFER_BODY,
                    new Object[]{"art:prepare", Strings.PREPARE_ART, Strings.ART_PREPARE_HINT, false, (Runnable) () -> {
                        Inputs.prefs(this).edit().putBoolean(ArtPolicy.START_AFTER_BUILD, true).apply();
                        buildStage(InstallState.OPTIONAL, true);
                    }},
                    new Object[]{"art:without", Strings.PLAY_WITHOUT_ART, Strings.ART_DECLINE_HINT, false, (Runnable) () -> {
                        Inputs.prefs(this).edit().putBoolean(ArtPolicy.DECLINED, true).apply();
                        launchGame(anyway);
                    }});
            return;
        }
        if (settings == null) settings = SettingsStore.load(this);
        String failed = SettingsStore.writeEnv(this, settings);
        if (failed != null && !anyway) {
            envError = failed;
            if (!ui) showUi();
            if (screen == Screen.SAVE_ERROR) render(); else go(Screen.SAVE_ERROR);
            return;
        }
        gameError = null;
        try { GameLock.start(getFilesDir(), System.currentTimeMillis(), android.os.Process.myPid(),
                () -> GameLaunch.start(this)); }
        catch (IOException | RuntimeException failedStart) {
            if (!ui) showUi();
            showNote(Strings.gameLockError(failedStart.toString()));
            return;
        }
        finish();
        overridePendingTransition(0, 0);
    }
    private void startGame() {
        if (closingGame) return;
        if (SetupService.status.running) { if (screen == Screen.INSTALL) playNow(); return; }
        if (!StorageLocation.available(this) || !InstallState.read(this).playable()) return;
        // A sync with the save folder in progress may be bringing in newer saves: start once it is done.
        if (syncing) { startAfterSync = true; showNote(Strings.SYNCING); return; }
        launchGame();
    }
    /** One of the last two stages alone, from Game data or (shaders, fromHome) Home's Build shaders; a finished build
     * returns to where it was started. */
    private void buildStage(String stage, boolean fromHome) {
        if (InstallState.OPTIONAL.equals(stage)) Inputs.prefs(this).edit().remove(ArtPolicy.DECLINED).apply();
        homeAfterBuild = fromHome;
        requestNotificationsThen(() -> startSetup(new String[]{stage}));
    }

    /** Long-press on the launcher icon offers Settings: the menu even when "open the game directly" is on. */
    private void publishShortcut() {
        ShortcutManager shortcuts = getSystemService(ShortcutManager.class);
        if (shortcuts == null) return;
        int flags = Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP;
        for (ShortcutInfo existing : shortcuts.getDynamicShortcuts())
            if (existing.getId().equals("menu") && existing.getIntent() != null
                    && (existing.getIntent().getFlags() & flags) == flags) return;
        Intent intent = new Intent(this, SetupActivity.class).setAction(Intent.ACTION_VIEW)
                .putExtra(EXTRA_SETTINGS, true).addFlags(flags);
        ShortcutInfo info = new ShortcutInfo.Builder(this, "menu").setShortLabel(Strings.SHORTCUT_SETTINGS)
                .setIcon(Icon.createWithBitmap(shortcutGlyph())).setIntent(intent).build();
        try { shortcuts.setDynamicShortcuts(Arrays.asList(info)); } catch (RuntimeException rateLimited) { }
    }
    private static Bitmap shortcutGlyph() {
        Bitmap bitmap = Bitmap.createBitmap(192, 192, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bitmap);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(Theme.INK); c.drawCircle(96, 96, 96, paint);
        c.save(); c.translate(16, 16); c.scale(160 / 360f, 160 / 360f);
        Widgets.Emblem.draw(c, paint, 360);
        c.restore();
        return bitmap;
    }

    // ---------- navigation ----------
    private void go(Screen next) {
        if (!closingGame) blockedAction = null;
        // Dialogs (confirmations, the import preview, the conflict choice) are never returned to with Back.
        boolean dialog = screen == Screen.CONFIRM_DELETE || screen == Screen.CHOICE || screen == Screen.SAVE_IMPORT || screen == Screen.SAVE_CONFLICT;
        if (screen != null && screen != next && !dialog) {
            history.push(screen);
            View focused = getCurrentFocus();
            if (focused != null && focused.getTag() != null) focusMemory.put(screen, focused.getTag());
        }
        if (next != Screen.GRAPHICS) setupFinished = false;
        screen = next; note = ""; noteOk = false; restoreFocus = PRIMARY;
        render();
    }
    private void replace(Screen next) { history.clear(); if (next != Screen.GRAPHICS) setupFinished = false; screen = next; note = ""; noteOk = false; restoreFocus = PRIMARY; render(); }
    private void back() {
        if (!closingGame) blockedAction = null;
        if (updater != null && (screen == Screen.UPDATE_DOWNLOAD || screen == Screen.CHOICE)) { updater.cancel(); return; }
        if (screen == Screen.UPDATES) { if (!history.isEmpty()) history.pop(); screen = Screen.FILES; render(); return; }
        if (screen == Screen.INSTALL && SetupService.status.running) { moveTaskToBack(true); return; }
        if (history.isEmpty()) { finish(); return; }
        setupFinished = false;
        screen = history.pop(); note = ""; noteOk = false;
        restoreFocus = focusMemory.remove(screen);
        render();
    }
    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() { back(); }

    /** Gamepad buttons: A confirms like D-pad centre, B goes back, + starts the game, Y restores defaults, X is the
     * screen's extra action, L/R switch tabs (some handhelds do not map them for apps). */
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (!ui) return super.dispatchKeyEvent(event);
        if (code == KeyEvent.KEYCODE_BUTTON_A)
            return super.dispatchKeyEvent(new KeyEvent(event.getDownTime(), event.getEventTime(), event.getAction(), KeyEvent.KEYCODE_DPAD_CENTER,
                    event.getRepeatCount(), event.getMetaState(), event.getDeviceId(), event.getScanCode(), event.getFlags(), event.getSource()));
        boolean up = event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled();
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_B:
                if (up) { if (screen == Screen.INSTALL) cancelPressed(); else back(); }
                return true;
            case KeyEvent.KEYCODE_BUTTON_START:
                if (up) plus();
                return true;
            case KeyEvent.KEYCODE_BUTTON_Y:
                if (up) defaults();
                return true;
            case KeyEvent.KEYCODE_BUTTON_X:
                if (up) extra();
                return true;
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_BUTTON_R1:
                if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0 && screen == Screen.GRAPHICS)
                    switchTab(code == KeyEvent.KEYCODE_BUTTON_L1 ? -1 : 1);
                return true;
            default:
                return super.dispatchKeyEvent(event);
        }
    }
    private boolean menuScreen() {
        return screen == Screen.HOME || screen == Screen.GRAPHICS || screen == Screen.CONTROLS || screen == Screen.DATA || screen == Screen.STORAGE
                || screen == Screen.ABOUT || screen == Screen.LICENSES || screen == Screen.LICENSE_TEXT || screen == Screen.SAVE_ERROR
                || screen == Screen.SAVES || screen == Screen.BACKUPS;
    }
    private void plus() {
        if (menuScreen()) startGame();
        else if (screen == Screen.FILES && installAction != null && installAction.isEnabled()) installAction.performClick();
        else if (screen == Screen.UPDATES) requestNotificationsThen(() -> startSetup(null));
        else if (screen == Screen.INSTALL && playPrompt != null && playPrompt.getVisibility() == View.VISIBLE) playNow();
    }
    private void defaults() {
        if (screen == Screen.GRAPHICS) settings.defaults(tab, SettingsStore.soc());
        else if (screen == Screen.CONTROLS) settings.defaults(Settings.CONTROLS, SettingsStore.soc());
        else return;
        changed();
    }
    private void extra() {
        if (screen == Screen.HOME) toggleSkip();
        else if (screen == Screen.BACKUPS) { if (chosenBackup != null && !saveBusy) confirmDeleteBackup(chosenBackup); }
        else if (screen == Screen.FILES) {
            View focused = getCurrentFocus();
            Object tag = focused == null ? null : focused.getTag();
            if (tag instanceof String && ((String) tag).startsWith("pick:")) {
                String role = ((String) tag).substring(5);
                if (multi(role)) pick(role, !primaryFolder(role));
            }
        }
    }
    private void selectFocused() { View focused = getCurrentFocus(); if (focused != null) focused.performClick(); }
    private void switchTab(int direction) {
        int i = Arrays.asList(TABS).indexOf(tab);
        tab = TABS[((i + direction) % TABS.length + TABS.length) % TABS.length];
        restoreFocus = PRIMARY;
        render();
    }

    // ---------- layout scaffolding ----------
    private void clearLive() {
        gameControls.clear(); gameNotice = null; closeGameAction = null;
        settingRows.clear(); presetCards.clear(); slotCards.clear(); stageRows.clear(); dataRows.clear();
        statusChip = null; statusLine = storageText = extrasNote = null;
        storageBar = null; ring = null; installAction = null; installPrompt = cancelPrompt = playPrompt = otherPrompt = null; skipToggle = null;
        detailFacts = null; detailTitle = detailText = detailConstraint = null; detailDiagram = null; detailNote = null; detailId = "";
        slotGrid = null; slotLine = null;
        updateProgress = null;
        primary = null; cancelArmedAt = 0;
    }
    private int seed() {
        switch (screen) {
            case HOME: return 7;
            case GRAPHICS: return tab.equals(Settings.PERFORMANCE) ? 11 : tab.equals(Settings.PICTURE) ? 23 : 31;
            case CONTROLS: return 19;
            case DATA: case CONFIRM_DELETE: case STORAGE: return 29;
            case SAVES: case BACKUPS: case SAVE_IMPORT: case SAVE_CONFLICT: case CHOICE: return 13;
            case ABOUT: case LICENSES: case LICENSE_TEXT: return 37;
            case INSTALL: return 53;
            case ERROR: case INTERRUPTED: return 47;
            default: return 41;
        }
    }

    /** Rebuild the whole screen; keeps D-pad focus on the same control (by tag). */
    private void render() {
        View focused = getCurrentFocus();
        Object focusTag = restoreFocus != null ? restoreFocus : focused == null ? null : focused.getTag();
        restoreFocus = null;
        clearLive();
        gameState = InstallState.gameState(this);
        Rect bounds = getWindowManager().getCurrentWindowMetrics().getBounds();
        Theme.fit(bounds.width(), bounds.height());
        FrameLayout root = new FrameLayout(this);
        root.addView(new Widgets.Backdrop(this, seed()), new FrameLayout.LayoutParams(-1, -1));
        board = new Widgets.Board(this);
        root.addView(board, new FrameLayout.LayoutParams(-1, -1));
        switch (screen) {
            case WELCOME: welcome(); break;
            case ROUTE: route(); break;
            case FILES: files(); break;
            case UPDATES: updates(); break;
            case UPDATE_DOWNLOAD: appUpdateDownload(); break;
            case INSTALL: install(); break;
            case ERROR: error(); break;
            case INTERRUPTED: interrupted(); break;
            case HOME: home(); break;
            case GRAPHICS: graphics(); break;
            case CONTROLS: controls(); break;
            case DATA: data(); break;
            case ABOUT: about(); break;
            case LICENSES: licences(); break;
            case LICENSE_TEXT: licenceText(); break;
            case CONFIRM_DELETE: confirmDelete(); break;
            case SAVE_ERROR: saveError(); break;
            case SETUP_STORAGE: storage(true); break;
            case STORAGE: storage(false); break;
            case SAVES: saves(); break;
            case BACKUPS: backups(); break;
            case SAVE_IMPORT: importPreview(); break;
            case SAVE_CONFLICT: conflictScreen(); break;
            case CHOICE: choice(); break;
        }
        gameBanner();
        getWindow().getDecorView().setKeepScreenOn(screen == Screen.INSTALL);
        setContentView(root);
        View target = focusTag == null || focusTag == PRIMARY ? null : root.findViewWithTag(focusTag);
        if (target == null || !target.isFocusable()) target = primary;
        if (target != null) {
            target.setFocusedByDefault(true);
            View finalTarget = target;
            // A window starts in touch mode, where the first D-pad/A press only reveals focus. On handhelds with a
            // gamepad, leave touch mode right away so the first press already acts on the highlighted control.
            root.post(() -> { if (!root.isInTouchMode()) finalTarget.requestFocus(); else if (hasGamepad()) finalTarget.requestFocusFromTouch(); });
        }
    }
    private static boolean hasGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && !device.isVirtual() && (device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                    || device.supportsSource(InputDevice.SOURCE_DPAD))) return true;
        }
        return false;
    }

    /** Title (icon + text) at the top left, an optional status chip at the top right, and the rule under both. */
    private Widgets.Chip header(String icon, String title, int chipKind, String chipText) {
        LinearLayout row = Widgets.row(this);
        row.addView(new Widgets.Icon(this, icon, Theme.TEXT), Widgets.size(30, 30));
        TextView text = Theme.text(this, title, true, 30, Theme.TEXT);
        row.addView(text, Widgets.margins(Widgets.size(-2, -2), 18, 0, 0, 0));
        board.put(row, 60, 94, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.START, Widgets.Board.CENTER);
        board.put(new Widgets.Rule(this, false, Theme.RULE), 60, 150, 1800, 9);
        if (chipText == null) return null;
        Widgets.Chip chip = new Widgets.Chip(this).set(chipKind, chipText);
        board.put(chip, 1860, 94, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.END, Widgets.Board.CENTER);
        return chip;
    }
    private void prompts(Widgets.Prompt... list) {
        LinearLayout row = Widgets.row(this);
        for (Widgets.Prompt prompt : list) {
            if (prompt == null) continue;
            row.addView(prompt, Widgets.margins(Widgets.size(-2, -2), row.getChildCount() == 0 ? 0 : 40, 0, 0, 0));
        }
        board.put(row, 1850, 1026, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.END, Widgets.Board.END);
    }
    private Widgets.Prompt prompt(String key, String label, View.OnClickListener click) { return new Widgets.Prompt(this, key, label, click); }
    private Widgets.Prompt backPrompt() { return prompt("B", Strings.P_BACK, v -> back()); }
    private Widgets.Prompt startPrompt() {
        Widgets.Prompt start = prompt("+", Strings.P_START, v -> startGame());
        start.setEnabled(InstallState.read(this).playable() && !seedRefused);
        return start;
    }
    private Widgets.Panel panel(float x, float y, float w, float h, boolean notch, float padH, float padV) {
        return board.put(new Widgets.Panel(this, notch, padH, padV), x, y, w, h);
    }
    private <T extends View> T add(LinearLayout parent, T view, float top) {
        parent.addView(view, Widgets.margins(Widgets.size(-1, -2), 0, top, 0, 0));
        return view;
    }
    private TextView heading(LinearLayout parent, String text, float size) { return add(parent, Theme.text(this, text, true, size, Theme.TEXT), 0); }
    private TextView lead(LinearLayout parent, String text, float top) { return add(parent, Theme.paragraph(this, text, 21, Theme.TEXT2), top); }
    /** A list row with label and hint at the given height, tagged for focus memory. */
    private Widgets.Row listRow(LinearLayout parent, String tag, String label, String hint, float height, float gap) {
        Widgets.Row row = new Widgets.Row(this, label, hint, 27, 18);
        row.setTag(tag);
        parent.addView(row, Widgets.rowParams(height, gap));
        return row;
    }
    private TextView rowText(String text, int color) {
        TextView view = Theme.text(this, text, false, 19, color);
        view.setGravity(Gravity.END);
        return view;
    }

    // ---------- home ----------
    private void hero(float top) {
        board.put(new Widgets.Emblem(this), 270, top, 340, 340);
        TextView word = Theme.text(this, Strings.APP, true, 64, 0xfff3ecd4);
        word.setLetterSpacing(.14f); word.setShadowLayer(Theme.f(24), 0, 0, 0x2ef0e6be);
        board.put(word, 440, top + 374, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.CENTER, Widgets.Board.START);
        TextView sub = Theme.text(this, Strings.SUBTITLE, false, 21, Theme.TEXT3);
        sub.setLetterSpacing(.32f);
        board.put(sub, 440, top + 470, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.CENTER, Widgets.Board.START);
    }
    private void home() {
        InstallState install = InstallState.read(this);
        boolean missing = !StorageLocation.available(this), accessLost = missing && StorageLocation.accessLost(this);
        // No usable shader seed (never built, its stage failed or was cancelled, or a start found it missing or empty):
        // the game would draw nothing, so Start game becomes Build shaders.
        boolean noSeed = !missing && install.installed() && (!install.done("shaders") || seedRefused);
        hero(170);
        // ready chips: two per line, centred under the word mark
        List<Widgets.Chip> chips = new ArrayList<>();
        if (missing) chips.add(new Widgets.Chip(this).set(Widgets.Chip.WARN, accessLost ? Strings.ACCESS_LOST_CHIP : Strings.MISSING_CHIP));
        else {
            chips.add(new Widgets.Chip(this).set(Widgets.Chip.OK, Strings.INSTALLED_CHIP));
            chips.add(!noSeed ? new Widgets.Chip(this).set(Widgets.Chip.OK, Strings.shadersReady(install.shaderVariants()))
                    : new Widgets.Chip(this).set(Widgets.Chip.WARN, Strings.SHADERS_NONE));
            if (install.done("companion") && SettingsStore.secondaryDisplay(this))
                chips.add(new Widgets.Chip(this).set(Widgets.Chip.OK, Strings.SECOND_READY));
        }
        if (pulledChip) chips.add(new Widgets.Chip(this).set(Widgets.Chip.OK, Strings.SAVES_UPDATED));
        if (gameError != null) chips.add(new Widgets.Chip(this).set(Widgets.Chip.WARN, Strings.GAME_STOPPED_CHIP));
        LinearLayout ready = Widgets.column(this);
        ready.setGravity(Gravity.CENTER_HORIZONTAL);
        for (int i = 0; i < chips.size(); i += 2) {
            LinearLayout line = Widgets.row(this);
            for (int j = i; j < Math.min(chips.size(), i + 2); j++)
                line.addView(chips.get(j), Widgets.margins(Widgets.size(-2, -2), j == i ? 0 : 12, 0, 0, 0));
            ready.addView(line, Widgets.margins(new LinearLayout.LayoutParams(-2, -2), 0, i == 0 ? 0 : 12, 0, 0));
        }
        if (gameError != null) {
            TextView why = Theme.paragraph(this, Strings.gameStopped(gameError), 19, Theme.AMBER);
            why.setGravity(Gravity.CENTER_HORIZONTAL);
            ready.addView(why, Widgets.margins(Widgets.size(-1, -2), 0, 18, 0, 0));
        }
        if (missing) {
            TextView why = Theme.paragraph(this, accessLost ? Strings.accessLost(StorageLocation.label(this)) : Strings.missingVolume(StorageLocation.label(this)), 19, Theme.AMBER);
            why.setGravity(Gravity.CENTER_HORIZONTAL);
            ready.addView(why, Widgets.margins(Widgets.size(-1, -2), 0, 18, 0, 0));
        }
        if (noSeed) {
            TextView why = Theme.paragraph(this, seedRefused && install.done("shaders") ? Strings.SEED_UNUSABLE : Strings.SEED_NEEDED, 19, Theme.AMBER);
            why.setGravity(Gravity.CENTER_HORIZONTAL);
            ready.addView(why, Widgets.margins(Widgets.size(-1, -2), 0, 18, 0, 0));
        }
        if (!note.isEmpty()) {
            TextView why = Theme.paragraph(this, note, 19, Theme.AMBER);
            why.setGravity(Gravity.CENTER_HORIZONTAL);
            ready.addView(why, Widgets.margins(Widgets.size(-1, -2), 0, 18, 0, 0));
        }
        board.put(ready, 440, 700, 700, Widgets.Board.WRAP, Widgets.Board.CENTER, Widgets.Board.START);

        Widgets.Panel menu = panel(880, 150, 920, 740, true, 56, 50);
        TextView h2 = Theme.text(this, Strings.MAIN_MENU, true, 22, Theme.TEXT3);
        h2.setLetterSpacing(.28f);
        menu.addView(h2, Widgets.margins(Widgets.size(-1, -2), 6, 6, 0, 22));
        File driver = gameFile("driver.so");
        String driverText = driver.isFile() ? Strings.DRIVER_CUSTOM_SHORT + Inputs.prefs(this).getString("driverName", "driver.so") : Strings.DRIVER_BUNDLED_SHORT;
        // six items: a little shorter than setup's welcome rows so they fit the same panel
        // With the data in a picked folder whose All files access was revoked, Start game is the fix: it opens the setting.
        Widgets.Row start;
        if (noSeed) {
            start = homeItem(menu, "home:start", "refresh", Strings.BUILD_SHADERS, Strings.BUILD_SHADERS_HINT, v -> buildStage("shaders", true), 88, 8);
        } else {
            start = homeItem(menu, "home:start", "play", Strings.START_GAME, accessLost ? Strings.ACCESS_START_HINT : missing ? Strings.MISSING_START_HINT : Strings.START_HINT,
                    v -> { if (accessLost) openAccessSettings(); else startGame(); }, 88, 8);
            start.value(Theme.text(this, driverText, false, 18, Theme.TEXT3));
            start.dimmed(missing && !accessLost);
        }
        homeItem(menu, "home:graphics", "gfx", Strings.GRAPHICS, Strings.graphicsSummary(settings), v -> { tab = Settings.PERFORMANCE; go(Screen.GRAPHICS); }, 88, 8);
        homeItem(menu, "home:controls", "controls", Strings.CONTROLS, Strings.controlsSummary(settings), v -> go(Screen.CONTROLS), 88, 8);
        homeItem(menu, "home:saves", "save", Strings.SAVES, Strings.savesSummary(liveSlots == null ? 0 : liveSlots.size(),
                liveSlots == null ? 0 : SaveManager.newest(liveSlots), folderSummary()), v -> go(Screen.SAVES), 88, 8);
        homeItem(menu, "home:data", "data", Strings.GAME_DATA, missing ? StorageLocation.label(this) + " · " + Strings.NOT_AVAILABLE
                : Strings.dataSummary(Strings.size(dataBytes()), SettingsStore.secondaryDisplay(this),
                        install.done("companion"), install.artOutdated(), install.done("shaders")), v -> go(Screen.DATA), 88, 8);
        homeItem(menu, "home:about", "about", Strings.ABOUT, Strings.ABOUT_HINT, v -> go(Screen.ABOUT), 88, 0);
        primary = start;
        // The one-time move of an install made before private storage existed: asked once per visit to the menu.
        if (!missing && !migrationAsked && !SetupService.status.running && StorageLocation.migrationPending(this, install.installed())) {
            migrationAsked = true;
            main.post(this::askMigration);
        }

        LinearLayout skip = Widgets.row(this);
        skip.addView(new Widgets.Key(this, "X", 38));
        skip.addView(Theme.text(this, Strings.SKIP_MENU, false, 20, Theme.TEXT2), Widgets.margins(Widgets.size(-2, -2), 14, 0, 14, 0));
        skipToggle = new Widgets.Toggle(this);
        skipToggle.set(SettingsStore.skipMenu(this));
        skip.addView(skipToggle, Widgets.size(58, 30));
        skip.setClickable(true); skip.setOnClickListener(v -> toggleSkip());
        board.put(skip, 120, 1026, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.START, Widgets.Board.END);
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), prompt("B", Strings.P_QUIT, v -> finish()), startPrompt());
    }
    private Widgets.Row homeItem(LinearLayout parent, String tag, String icon, String label, String hint, View.OnClickListener click) {
        return homeItem(parent, tag, icon, label, hint, click, 104, 10);
    }
    private Widgets.Row homeItem(LinearLayout parent, String tag, String icon, String label, String hint, View.OnClickListener click, float height, float gap) {
        Widgets.Row row = new Widgets.Row(this, label, hint, 30, 19).icon(icon, 62, 30);
        row.setTag(tag); row.setOnClickListener(v -> { if (!row.dimmed()) click.onClick(v); });
        parent.addView(row, Widgets.rowParams(height, gap));
        return row;
    }
    private void toggleSkip() {
        boolean next = !SettingsStore.skipMenu(this);
        SettingsStore.skipMenu(this, next);
        if (skipToggle != null) skipToggle.set(next);
    }

    // ---------- graphics / controls ----------
    /** "Snapdragon 8 Gen 2 class (QCS8550) · dual screen": the profile family and the second screen, both detected. */
    private String detected() {
        String soc = SettingsStore.soc();
        return Strings.detected(Settings.socClass(soc), soc, SettingsStore.secondScreen(this));
    }
    private String screenChip() {
        int second = SettingsStore.secondScreen(this);
        return second == com.aure.totk.companion.DisplayClass.BUILT_IN ? Strings.SECOND_DETECTED
                : second == com.aure.totk.companion.DisplayClass.EXTERNAL ? Strings.EXTERNAL_ONLY : Strings.ONE_SCREEN;
    }
    private void graphics() {
        boolean wideChip = setupFinished;
        String chip = wideChip ? Strings.SETUP_COMPLETE
                : tab.equals(Settings.PERFORMANCE) ? detected()
                : tab.equals(Settings.PICTURE) ? Strings.presetChip(settings.preset())
                : screenChip();
        statusChip = header("gfx", setupFinished ? Strings.CHOOSE_GRAPHICS : Strings.GRAPHICS, Widgets.Chip.OK, chip);
        LinearLayout tabs = Widgets.row(this);
        tabs.setGravity(Gravity.TOP);
        Widgets.Key left = new Widgets.Key(this, "L", 38);
        tabs.addView(left, Widgets.margins(Widgets.size(-2, -2), 0, 19, 10, 0));
        String[] names = {Strings.TAB_PERFORMANCE, Strings.TAB_PICTURE, Strings.TAB_SCREEN}, icons = {"perf", "picture", "screen"};
        for (int i = 0; i < TABS.length; i++) {
            String id = TABS[i];
            Widgets.Tab t = new Widgets.Tab(this, icons[i], names[i], v -> { if (!tab.equals(id)) { tab = id; restoreFocus = PRIMARY; render(); } });
            t.on(id.equals(tab));
            tabs.addView(t, Widgets.margins(Widgets.size(118, 86), 5, 0, 5, 0));
        }
        Widgets.Key right = new Widgets.Key(this, "R", 38);
        tabs.addView(right, Widgets.margins(Widgets.size(-2, -2), 10, 19, 0, 0));
        left.setOnClickListener(v -> switchTab(-1)); right.setOnClickListener(v -> switchTab(1));
        board.put(tabs, 960, 59, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.CENTER, Widgets.Board.START);

        float listTop = 186;
        if (tab.equals(Settings.PERFORMANCE)) {
            String soc = SettingsStore.soc(), recommended = Settings.profileKnown(soc) ? Settings.profile(soc) : null;
            String[] all = {"light", "balanced", "smooth", "60fps", Settings.CUSTOM};
            float w = (1040 - 4 * 14) / 5f;
            for (int i = 0; i < all.length; i++) {
                String id = all[i];
                Widgets.PresetCard card = new Widgets.PresetCard(this, Strings.presetName(id), Strings.presetCaption(id),
                        id.equals(recommended) ? Strings.RECOMMENDED : null, Strings.IN_USE, v -> choosePreset(id));
                card.setTag("preset:" + id);
                card.setOnFocusChangeListener((v, has) -> { if (has) showDetail("preset:" + id); });
                presetCards.put(id, card);
                board.put(card, 80 + i * (w + 14) - Widgets.CAP, 186, w + 2 * Widgets.CAP, 150);
            }
            listTop = 356;
        }
        Widgets.Panel list = panel(80, listTop, 1040, 906 - listTop, false, 28, 28);
        List<String> keys = new ArrayList<>();
        switch (tab) {
            case Settings.PERFORMANCE: keys.addAll(Arrays.asList("frame_rate", "frame_gen", "render_res", "latency", "gpu_feed")); break;
            case Settings.PICTURE: keys.addAll(Arrays.asList("upscaler", "sharpness", "aa", "shadows")); break;
            default:
                keys.addAll(Arrays.asList("console_mode", "native_hud", "overlay", "overlay_pos"));
                if (SettingsStore.secondaryDisplay(this)) keys.add("second_screen");
        }
        for (String key : keys) settingRow(list, key);
        detailPanel();
        if (setupFinished) {
            // Setup's last step: the recommended preset is selected; one press on Start game plays.
            String preset = settings.preset();
            primary = presetCards.get(presetCards.containsKey(preset) ? preset : "balanced");
            Widgets.Action start = new Widgets.Action(this, Strings.START_GAME, true, v -> startGame());
            start.setTag("start");
            board.put(start, 1100, 886, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.END, Widgets.Board.END);
        } else primary = settingRows.get(keys.get(0));
        refreshSettings();
        prompts(prompt("Y", Strings.P_DEFAULTS, v -> defaults()), prompt(tab.equals(Settings.PICTURE) ? Widgets.Key.DPAD : "A",
                tab.equals(Settings.PICTURE) ? Strings.P_ADJUST : Strings.P_CHANGE, v -> selectFocused()), backPrompt(), startPrompt());
    }

    private void controls() {
        header("controls", Strings.CONTROLS, 0, null);
        Widgets.Panel list = panel(80, 186, 1040, 720, false, 28, 28);
        for (String key : new String[]{"buttons", "rumble", "motion", "audio_latency", "game_language"}) settingRow(list, key);
        detailPanel();
        primary = settingRows.get("buttons");
        refreshSettings();
        prompts(prompt("Y", Strings.P_DEFAULTS, v -> defaults()), prompt("A", Strings.P_CHANGE, v -> selectFocused()), backPrompt(), startPrompt());
    }
    /** One option row: a ‹ value › selector, or the sharpness slider bound to the current upscaler's value. */
    private void settingRow(LinearLayout parent, String key) {
        String labelKey = key.equals("sharpness") ? "sharpness_fsr" : key;
        // Five Performance rows share the space below the preset cards; keep the last selector inside the panel.
        float gap = Settings.option(labelKey).tab.equals(Settings.PERFORMANCE) ? 6 : 18;
        Widgets.Row row = listRow(parent, "opt:" + key, Strings.optionLabel(labelKey), Strings.optionHint(labelKey), 92, gap);
        row.setFocusableInTouchMode(true);   // a tap selects the row (and shows its detail); the arrows change it
        if (key.equals("sharpness")) {
            Widgets.Slider slider = new Widgets.Slider(this, row, value -> {
                String k = settings.sharpnessKey();
                if (k != null && !row.dimmed()) { settings.set(k, String.valueOf(value)); changed(); }
            });
            row.value(slider);
            row.adjust(direction -> { String k = settings.sharpnessKey(); if (k != null) { settings.step(k, direction); changed(); } });
        } else {
            row.value(new Widgets.Selector(this, row));
            row.adjust(direction -> { settings.step(key, direction); changed(); });
            row.setOnClickListener(v -> row.step(1));
        }
        row.setOnFocusChangeListener((v, has) -> { if (has) showDetail("opt:" + key); });
        settingRows.put(key, row);
    }
    private void choosePreset(String id) {
        if (!id.equals(Settings.CUSTOM)) { settings.applyPreset(id); changed(); }
        showDetail("preset:" + id);
    }
    /** A setting changed: store it, rewrite settings.env, and update the visible rows, cards and detail in place. */
    private void changed() {
        SettingsStore.save(this, settings);
        refreshSettings();
    }
    private void refreshSettings() {
        for (Map.Entry<String, Widgets.Row> e : settingRows.entrySet()) {
            String key = e.getKey();
            Widgets.Row row = e.getValue();
            View value = row.getChildAt(row.getChildCount() - 1);
            if (key.equals("sharpness")) {
                String k = settings.sharpnessKey();
                ((Widgets.Slider) value).set(settings.number(k == null ? "sharpness_fsr" : k));
                row.dimmed(k == null);
                row.hint.setText(k == null ? Strings.NEEDS_UPSCALER : Strings.optionHint("sharpness_fsr"));
                continue;
            }
            Settings.Option option = Settings.option(key);
            // Long lists (13 languages) get no pips: they would be wider than the value.
            int count = option.values.length <= 6 ? option.values.length : 0;
            ((Widgets.Selector) value).set(Strings.valueName(key, settings.get(key)), count, option.index(settings.get(key)));
            if (key.equals("latency")) {
                row.dimmed(settings.latencyLocked());
                row.hint.setText(settings.latencyLocked() ? Strings.LOCKED_FRAMEGEN : Strings.optionHint(key));
            }
        }
        String preset = settings.preset();
        for (Map.Entry<String, Widgets.PresetCard> e : presetCards.entrySet()) e.getValue().active(e.getKey().equals(preset));
        // D-pad: up from the first row lands on the preset in use, down from any card on the first row (geometry alone
        // picks whichever card happens to sit above the row's centre).
        Widgets.Row first = settingRows.get("frame_rate");
        if (first != null && !presetCards.isEmpty()) {
            if (first.getId() == View.NO_ID) first.setId(View.generateViewId());
            Widgets.PresetCard active = presetCards.get(presetCards.containsKey(preset) ? preset : Settings.CUSTOM);
            if (active.getId() == View.NO_ID) active.setId(View.generateViewId());
            first.setNextFocusUpId(active.getId());
            for (Widgets.PresetCard card : presetCards.values()) card.setNextFocusDownId(first.getId());
        }
        if (statusChip != null && screen == Screen.GRAPHICS && tab.equals(Settings.PICTURE) && !setupFinished) statusChip.set(Widgets.Chip.OK, Strings.presetChip(preset));
        if (!detailId.isEmpty()) showDetail(detailId);
    }

    /** The right-hand panel: title, explanation, picture, facts, a constraint line, and the "takes effect" note. */
    private void detailPanel() {
        Widgets.Panel detail = panel(1160, 186, 680, 720, true, 44, 40);
        detailTitle = Theme.text(this, "", true, 34, Theme.TEXT);
        detail.addView(detailTitle);
        detailText = add(detail, Theme.paragraph(this, "", 21, Theme.TEXT2), 16);
        detailDiagram = new Diagram(this);
        detail.addView(detailDiagram, Widgets.margins(Widgets.size(-1, 150), 0, 28, 0, 0));
        detailFacts = Widgets.column(this);
        detail.addView(detailFacts, Widgets.margins(Widgets.size(-1, -2), 0, 18, 0, 0));
        detailConstraint = add(detail, Theme.paragraph(this, "", 18, Theme.AMBER), 8);
        detail.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout noteRow = Widgets.column(this);
        noteRow.addView(new Widgets.Rule(this, false, Theme.RULE_SOFT), Widgets.size(-1, 2));
        LinearLayout line = Widgets.row(this);
        TextView dot = Theme.text(this, "●", true, 14, Theme.ZONAI);
        line.addView(dot, Widgets.margins(Widgets.size(-2, -2), 0, 0, 14, 0));
        line.addView(Theme.paragraph(this, Strings.TAKES_EFFECT, 18, Theme.TEXT3), new LinearLayout.LayoutParams(0, -2, 1));
        noteRow.addView(line, Widgets.margins(Widgets.size(-1, -2), 0, 20, 0, 0));
        detail.addView(noteRow);
        detailNote = noteRow;
    }
    private void fact(String key, View value) { detailFacts.addView(Widgets.fact(this, key, value)); }
    private void fact(String key, String value) { fact(key, Theme.text(this, value, false, 20, Theme.TEXT)); }
    private static int cost(String key, String value) {
        switch (key) {
            case "frame_rate": return value.equals("60") ? 4 : 2;
            case "frame_gen": return value.equals("on") ? 3 : 0;
            case "gpu_feed": return value.equals("on") ? 1 : 0;
            case "render_res": return value.equals("960x540") ? 1 : value.equals("1152x648") ? 2 : 3;
            case "upscaler": return value.equals("off") ? 0 : 1;
            case "sharpness_fsr": case "sharpness_sgsr": return 1;
            case "aa": return value.equals("smaa") ? 2 : value.equals("fxaa") ? 1 : 0;
            case "shadows": return value.equals("high") ? 3 : value.equals("medium") ? 2 : 1;
            case "console_mode": return value.equals("docked") ? 4 : 2;
            case "native_hud": return value.equals("on") ? 1 : 0;
            case "overlay": return value.equals("full") ? 1 : 0;
            case "second_screen": return value.equals("on") ? 1 : 0;
            default: return 0;
        }
    }
    private void showDetail(String id) {
        if (detailTitle == null) return;
        detailId = id;
        detailFacts.removeAllViews();
        String constraint = null;
        if (id.startsWith("preset:")) {
            String preset = id.substring(7);
            detailTitle.setText(Strings.presetName(preset));
            detailText.setText(Strings.presetText(preset));
            detailDiagram.setVisibility(View.GONE);
            if (!preset.equals(Settings.CUSTOM))
                for (String key : new String[]{"frame_rate", "frame_gen", "render_res", "latency", "upscaler", "aa", "shadows"}) {
                    String v = Settings.presetValue(preset, key);
                    fact(Strings.optionLabel(key), v == null ? Strings.LOCKED_FRAMEGEN : Strings.valueName(key, v));
                }
            String soc = SettingsStore.soc();
            if (preset.equals(Settings.profile(soc)))
                constraint = Settings.profileKnown(soc) ? Strings.RECOMMENDED + " (" + soc + ")" : Strings.UNTESTED_DEFAULT + " (" + soc + ")";
        } else {
            String key = id.substring(4);
            if (key.equals("sharpness")) key = settings.sharpnessKey() == null ? "sharpness_fsr" : settings.sharpnessKey();
            String value = settings.get(key);
            detailTitle.setText(Strings.optionLabel(key));
            detailText.setText(Strings.optionText(key));
            detailDiagram.setVisibility(View.VISIBLE);
            detailDiagram.show(key, settings, SettingsStore.secondaryDisplay(this));
            if (!Settings.option(key).tab.equals(Settings.CONTROLS)) fact(Strings.GPU_COST, new Widgets.Meter(this).set(cost(key, value)));
            switch (key) {
                case "frame_rate":
                    fact(Strings.WORKS_WITH, value.equals("60") ? Strings.valueName("frame_gen", "off") + " frame generation" : "every option");
                    break;
                case "frame_gen":
                    fact(Strings.ADDED_LATENCY, Strings.addedLatency(key, value));
                    fact(Strings.WORKS_WITH, "30 fps only");
                    break;
                case "gpu_feed":
                    fact(Strings.WORKS_WITH, "Frame generation off");
                    if (settings.latencyLocked()) constraint = Strings.LOCKED_FRAMEGEN;
                    break;
                case "latency":
                    fact(Strings.ADDED_LATENCY, settings.latencyLocked() ? Strings.LOCKED_FRAMEGEN : Strings.addedLatency(key, value));
                    // the fact row already says "Set by frame generation"; no second line
                    break;
                case "sharpness_fsr": case "sharpness_sgsr":
                    fact(Strings.APPLIES, Strings.WHEN_STARTS);
                    if (settings.sharpnessKey() == null) constraint = Strings.NEEDS_UPSCALER;
                    break;
                case "console_mode":
                    fact(Strings.GAME_RESOLUTION, value.equals("docked") ? "up to 1920 × 1080" : "up to 1280 × 720");
                    break;
                case "native_hud":
                    if (settings.get("console_mode").equals("docked") && value.equals("on")) constraint = "Docked mode uses the game's HUD";
                    break;
                case "overlay": case "overlay_pos": {
                    LinearLayout chord = Widgets.row(this);
                    chord.addView(Theme.text(this, Strings.HOLD, false, 20, Theme.SEL));
                    chord.addView(new Widgets.Key(this, "−", 30));
                    chord.addView(Theme.text(this, Strings.HOLD_THEN, false, 20, Theme.SEL));
                    chord.addView(new Widgets.Key(this, "R", 30));
                    fact(Strings.WHILE_PLAYING, chord);
                    if (key.equals("overlay")) constraint = Strings.CHORD_HINT;
                    break;
                }
                case "second_screen": {
                    InstallState art = InstallState.read(this);
                    fact(Strings.COMPANION_ROW, art.done("companion") ? Strings.READY : art.artOutdated() ? Strings.OUT_OF_DATE : Strings.NOT_PREPARED);
                    break;
                }
                case "rumble":
                    fact(Strings.STRENGTH, value.equals("0") ? Strings.valueName(key, value) : value + " % of the game's rumble");
                    break;
                case "audio_latency":
                    fact(Strings.SOUND_BUFFER, value.equals("low") ? "10 ms" : value.equals("safe") ? "40 ms" : "20 ms");
                    break;
                default: break;
            }
        }
        detailConstraint.setText(constraint == null ? "" : constraint);
        detailConstraint.setVisibility(constraint == null ? View.GONE : View.VISIBLE);
        if (detailNote != null) detailNote.setVisibility(View.VISIBLE);
        if (detailDiagram.getVisibility() == View.VISIBLE) fitDiagram(id.substring(4).equals("sharpness") ? "sharpness_fsr" : id.substring(4));
    }
    /** The picture takes its preferred height, or what is left of the detail panel once the text, facts, constraint
     * and note are placed (long explanations must never push the note out of the panel). */
    private void fitDiagram(String key) {
        int width = Theme.u(680 - 2 * 44), any = View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED);
        int exact = View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), used = 0;
        for (View view : new View[]{detailTitle, detailText, detailFacts, detailConstraint, detailNote}) {
            if (view.getVisibility() == View.GONE) continue;
            view.measure(exact, any);
            LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) view.getLayoutParams();
            used += view.getMeasuredHeight() + lp.topMargin + lp.bottomMargin;
        }
        int room = Theme.u(720 - 2 * 40 - 28 - 10) - used;
        LinearLayout.LayoutParams p = (LinearLayout.LayoutParams) detailDiagram.getLayoutParams();
        p.height = Math.max(Theme.u(90), Math.min(Theme.u(Diagram.height(key)), room));
        detailDiagram.setLayoutParams(p);
    }

    // ---------- game data ----------
    private File gameFile(String name) { return new File(InstallState.gameDir(this), name); }
    /** Bytes of the imported data, or DataSize.MEASURING / UNKNOWN. Walked on DataSize's own thread whenever the install
     * changed since the last walk (another folder, a finished stage or rebuild, a move or delete); Home and Game data are
     * drawn again when the walk ends, and once more when its time is up, so "measuring…" never stays. */
    private long dataBytes() {
        long bytes = DataSize.get(InstallState.gameDir(this), sizeKnown);
        if (bytes == DataSize.MEASURING) { main.removeCallbacks(sizeChanged); main.postDelayed(sizeChanged, DataSize.timeoutMs + 100); }
        return bytes;
    }

    private void data() {
        statusChip = header("data", Strings.GAME_DATA, noteOk ? Widgets.Chip.OK : Widgets.Chip.WARN, note.isEmpty() ? null : note);
        InstallState install = InstallState.read(this);
        boolean installed = install.installed();
        Widgets.Panel list = panel(80, 186, 1040, 720, false, 28, 24);
        Widgets.Row installedRow = dataRow(list, "installed", Strings.INSTALLED, installed ? Strings.setupFormat(install.setupVersion(), install.route()) : Strings.NOT_INSTALLED,
                installed ? Strings.size(dataBytes()) + " · " + Strings.VERIFIED : Strings.NOT_INSTALLED, installed ? Theme.ZONAI : Theme.AMBER, null);
        boolean missing = !StorageLocation.available(this);
        StorageLocation.Volume current = missing ? null : StorageLocation.current(this);
        dataRow(list, "storage", Strings.STORAGE_ROW, Strings.STORAGE_HINT,
                missing ? StorageLocation.label(this) + " · " + Strings.NOT_AVAILABLE : current == null ? StorageLocation.label(this)
                        : Strings.storageValue(current.name, current.free), missing ? Theme.WARN : Theme.ZONAI, v -> {
                    if (SetupService.status.running || busy) return;
                    if (!missing && install.anyDone() && !installed) { showNote(Strings.MOVE_PARTIAL); return; }
                    go(Screen.STORAGE);
                });
        boolean secondScreen = SettingsStore.secondaryDisplay(this);
        boolean companion = install.done("companion"), shaders = install.done("shaders") && !seedRefused;
        Widgets.Row companionRow = dataRow(list, "companion", Strings.COMPANION_ROW, secondScreen ? Strings.COMPANION_HINT : Strings.ART_NOT_NEEDED_HINT,
                !secondScreen ? Strings.ART_NOT_NEEDED + " · " + (companion || install.artOutdated() ? Strings.REBUILD : Strings.BUILD)
                        : companion ? Strings.READY + " · " + Strings.REBUILD : !installed ? Strings.NEEDS_SETUP
                        : install.artOutdated() ? Strings.OUT_OF_DATE + " · " + Strings.UPDATE : Strings.NOT_PREPARED + " · " + Strings.BUILD,
                !secondScreen ? Theme.TEXT3 : companion ? Theme.ZONAI : Theme.AMBER, v -> buildStage("companion", false));
        // The shader seed is required: without it Start game is replaced by Build shaders on Home.
        Widgets.Row shaderRow = dataRow(list, "shaders", Strings.SHADERS_ROW, Strings.SHADERS_HINT,
                shaders ? Strings.variants(install.shaderVariants()) + " · " + Strings.REBUILD : !installed ? Strings.NEEDS_SETUP : Strings.NOT_BUILT + " · " + Strings.BUILD,
                shaders ? Theme.ZONAI : Theme.WARN, v -> buildStage("shaders", false));
        companionRow.dimmed(!installed || busy); shaderRow.dimmed(!installed || busy);
        gameControl(companionRow, "Preparing game data", () -> buildStage("companion", false));
        gameControl(shaderRow, "Preparing shaders", () -> buildStage("shaders", false));
        File driver = gameFile("driver.so");
        String driverName = Inputs.prefs(this).getString("driverName", "driver.so");
        Widgets.Row driverRow = dataRow(list, "driver", Strings.DRIVER, Strings.DRIVER_HINT,
                driver.isFile() ? Strings.DRIVER_CUSTOM_SHORT + driverName + " · " + Strings.bytes(driver.length()) : Strings.DRIVER_BUNDLED,
                driver.isFile() ? Theme.AMBER : Theme.ZONAI, v -> pickDriver());
        Widgets.Row reset = dataRow(list, "driver-reset", Strings.DRIVER_RESET_ROW, Strings.DRIVER_RESET_HINT, "", Theme.TEXT3, v -> resetDriver());
        driverRow.dimmed(missing);
        reset.dimmed(missing || !driver.isFile());
        gameControl(driverRow, "Changing the driver", this::pickDriver);
        gameControl(reset, "Changing the driver", this::resetDriver);
        if (missing && StorageLocation.accessLost(this))
            dataRow(list, "access", Strings.ACCESS_FIX_ROW, Strings.ACCESS_FIX_HINT, Strings.ACCESS_OFF, Theme.WARN, v -> openAccessSettings());
        else if (missing) dataRow(list, "again", Strings.SETUP_AGAIN_ROW, Strings.SETUP_AGAIN_HINT, "", Theme.TEXT3, v -> go(Screen.STORAGE));
        else dataRow(list, "rerun", Strings.RERUN, Strings.RERUN_HINT, "", Theme.TEXT3, v -> go(Screen.ROUTE));
        Widgets.Row delete = dataRow(list, "delete", busy ? Strings.DELETING : Strings.DELETE_ROW, Strings.DELETE_HINT, "", Theme.WARN, v -> go(Screen.CONFIRM_DELETE));
        delete.labelColor(Theme.WARN);
        delete.dimmed(busy || !(install.anyDone() || gameFile("extracted").exists() || gameFile(".import").exists()));
        gameControl(delete, "Deleting game data", () -> go(Screen.CONFIRM_DELETE));
        detailPanel();
        detailNote.setVisibility(View.GONE);
        primary = installedRow;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt(), startPrompt());
    }
    private Widgets.Row dataRow(LinearLayout parent, String id, String label, String hint, String state, int stateColor, View.OnClickListener click) {
        Widgets.Row row = listRow(parent, "data:" + id, label, hint, 76, 8);
        if (!state.isEmpty()) row.value(rowText(state, stateColor));
        row.setOnClickListener(v -> { if (!row.dimmed() && click != null) click.onClick(v); });
        row.setOnFocusChangeListener((v, has) -> { if (has) dataDetail(id); });
        dataRows.put(id, row);
        return row;
    }
    private void dataDetail(String id) {
        if (detailTitle == null) return;
        detailFacts.removeAllViews();
        detailDiagram.setVisibility(View.GONE);
        detailConstraint.setVisibility(View.GONE);
        InstallState install = InstallState.read(this);
        Widgets.Row row = dataRows.get(id);
        detailTitle.setText(row == null ? "" : row.label.getText());
        switch (id) {
            case "installed":
                detailText.setText(Strings.DATA_TEXT);
                for (String stage : InstallState.CORE) fact(Strings.stage(stage), install.done(stage) ? Strings.DONE : Strings.NOT_PREPARED);
                fact(Strings.STORAGE, Strings.size(dataBytes()));
                break;
            case "storage": case "again": {
                detailText.setText(Strings.STORAGE_TEXT);
                for (StorageLocation.Volume v : StorageLocation.volumes(this))
                    fact(v.name, Strings.bytes(v.free) + " free" + (v.kind == StoragePaths.Kind.PRIVATE ? " · " + Strings.FASTER : ""));
                if (!StorageLocation.available(this)) fact(StorageLocation.label(this), Strings.NOT_AVAILABLE);
                fact(Strings.ACCESS_ROW, FileAccess.granted() ? Strings.ACCESS_ON : Strings.ACCESS_OFF);
                break;
            }
            case "access": detailText.setText(Strings.ACCESS_TEXT); break;
            case "companion": detailText.setText((SettingsStore.secondaryDisplay(this) ? "" : Strings.ART_NOT_NEEDED + ". ") + Strings.COMPANION_TEXT); break;
            case "shaders": detailText.setText(Strings.SHADERS_TEXT); break;
            case "driver": case "driver-reset": detailText.setText(Strings.DRIVER_TEXT); break;
            case "rerun": detailText.setText(Strings.RERUN_TEXT); break;
            default: detailText.setText(Strings.DELETE_TEXT);
        }
        if ((id.equals("companion") || id.equals("shaders")) && !install.errorCode(id).isEmpty()) {
            detailConstraint.setText(install.errorText(id).isEmpty() ? Strings.error(install.errorCode(id)) : install.errorText(id));
            detailConstraint.setVisibility(View.VISIBLE);
        }
    }

    private void pickDriver() {
        if (!needGameClosed("Changing the driver", this::pickDriver)) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        try { startActivityForResult(intent, REQ_DRIVER); } catch (ActivityNotFoundException noPicker) { showNote(Strings.PICKER_MISSING); }
    }

    private void resetDriver() {
        if (!needGameClosed("Changing the driver", this::resetDriver)) return;
        try (GameLock.Hold held = GameLock.hold(getFilesDir())) {
            held.requireFree();
            gameFile("driver.so").delete();
        } catch (IOException failed) { showNote(Strings.gameLockError(failed.toString())); return; }
        render();
    }

    /** Copy a user-chosen Turnip build to <game>/driver.so (the runtime prefers it over the bundled libtotk_turnip.so). */
    private void installDriver(Uri uri) {
        if (!needGameClosed("Changing the driver", () -> installDriver(uri))) return;
        ContentResolver resolver = getContentResolver();
        worker.execute(() -> {
            String name = Inputs.describe(resolver, uri).name, outcome = null;
            File target = gameFile("driver.so");
            File direct = FileAccess.directFile(this, uri, -1);   // by its real path with All files access, else the picker's stream
            try (GameLock.Hold held = GameLock.hold(getFilesDir())) {
                held.requireFree();
                InputStream in = direct != null ? new java.io.FileInputStream(direct) : resolver.openInputStream(uri);
                if (in == null) throw new IOException("unreadable");
                String label = DriverImport.install(in, target, new File(getCacheDir(), "driver-import.zip.tmp"), name);
                Inputs.prefs(this).edit().putString("driverName", label).apply();
            } catch (DriverImport.Invalid failed) {
                outcome = failed.zip ? Strings.DRIVER_ZIP_BAD : Strings.DRIVER_BAD;
            } catch (IOException | SecurityException failed) {
                outcome = Strings.gameLockError(failed.toString());
            }
            String message = outcome;
            main.post(() -> { note = message == null ? "" : message; if (screen == Screen.DATA) render(); });
        });
    }

    private void confirmDelete() {
        header("data", Strings.GAME_DATA, 0, null);
        Widgets.Panel box = panel(460, 290, 1000, 480, true, 56, 48);
        heading(box, Strings.DELETE_CONFIRM_TITLE, 34);
        lead(box, Strings.DELETE_CONFIRM_BODY, 16);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        Widgets.Row keep = listRow(box, "keep", Strings.KEEP, null, 76, 10);
        keep.setOnClickListener(v -> back());
        Widgets.Row delete = listRow(box, "delete", Strings.DELETE, null, 76, 0);
        delete.labelColor(Theme.WARN);
        delete.setOnClickListener(v -> deleteData());
        gameControl(delete, "Deleting game data", this::deleteData);
        primary = keep;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt());
    }
    /** settings.env could not be written right before starting: Retry, or Start anyway with the last saved file. */
    private void saveError() {
        header("gfx", Strings.START_GAME, Widgets.Chip.WARN, Strings.SAVE_FAILED_CHIP);
        Widgets.Panel box = panel(460, 290, 1000, 480, true, 56, 48);
        heading(box, Strings.SAVE_FAILED_TITLE, 34);
        lead(box, Strings.saveFailed(envError), 16);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        Widgets.Row retry = listRow(box, "save:retry", Strings.RETRY, Strings.RETRY_HINT, 76, 10);
        retry.setOnClickListener(v -> launchGame(false));
        Widgets.Row anyway = listRow(box, "save:anyway", Strings.START_ANYWAY, Strings.START_ANYWAY_HINT, 76, 0);
        anyway.setOnClickListener(v -> launchGame(true));
        primary = retry;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt());
    }
    private void deleteData() {
        if (SetupService.status.running || !needGameClosed("Deleting game data", this::deleteData)) return;
        busy = true; screen = Screen.DATA; note = Strings.DELETING; render();
        File game = InstallState.gameDir(this);
        worker.execute(() -> {
            try (GameLock.Hold held = GameLock.hold(getFilesDir())) {
                held.requireFree();
                // install.json first: an interrupted delete must never leave something that still looks playable.
                new File(game, "install.json").delete();
                for (String name : new String[]{".import", "rewritten", "game-data", "extracted", "shaders", "companion"}) delete(new File(game, name));
            } catch (IOException failed) {
                main.post(() -> { busy = false; showNote(Strings.gameLockError(failed.toString())); });
                return;
            }
            main.post(() -> {
                busy = false; DataSize.invalidate();
                inputs.clear(getContentResolver());
                SetupService.clearResult(this);
                Inputs.prefs(this).edit().remove("jobStages").remove(ArtPolicy.DECLINED).remove(ArtPolicy.START_AFTER_BUILD).apply();
                inspection = null;
                // Nothing playable is left: the menu has nothing to start, so continue on the welcome screen.
                history.clear(); history.push(Screen.WELCOME);
                screen = Screen.DATA; note = Strings.DELETED; noteOk = true; restoreFocus = PRIMARY;
                render();
            });
        });
    }
    private static void delete(File file) {
        File[] children = file.isDirectory() && !isLink(file) ? file.listFiles() : null;
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }
    private static boolean isLink(File file) { return java.nio.file.Files.isSymbolicLink(file.toPath()); }

    // ---------- storage: setup's first step, and moving the game data ----------
    /** One card per place (StorageLocation.volumes: private internal storage, SD card app folders, the old internal
     * folder or a picked folder while the data is there) plus "A folder you choose". setup: where a new import goes
     * (setup's Storage step). Otherwise Game data > Storage: another place moves the installed data there
     * (install-screen visuals); with the chosen place missing, or nothing installed yet, it only sets where the next
     * setup imports to. Both offer the optional All files access step (Azahar's "Filesystem" permission page). */
    private void storage(boolean setup) {
        List<StorageLocation.Volume> volumes = StorageLocation.volumes(this);
        InstallState install = InstallState.read(this);
        boolean missing = !StorageLocation.available(this), moving = !setup && !missing && install.installed();
        boolean granted = FileAccess.granted();
        File current = StorageLocation.gameDir(this);
        // A move needs the data's size (plus the RomFS proof and staging slack); a new setup about 19 GB.
        long size = moving ? dataBytes() : -1;
        long need = size > 0 ? size + 500_000_000L : InstallState.ROMFS_SIZE + 2_000_000_000L;
        StorageLocation.Volume recommended = null;
        for (StorageLocation.Volume v : volumes) if (recommended == null && v.free >= need) recommended = v;   // private storage first
        Widgets.Panel box;
        Widgets.Row access;
        if (setup) {
            setupHeader(Strings.SETUP_TITLE);
            rail(0, false);
            box = setupMain();
            heading(box, Strings.SETUP_STORAGE_TITLE, 36);
            lead(box, Strings.SETUP_STORAGE_LEAD, 10);
        } else {
            statusChip = header("data", Strings.GAME_DATA, noteOk ? Widgets.Chip.OK : Widgets.Chip.WARN, note.isEmpty() ? null : note);
            box = panel(80, 186, 1040, 720, true, 50, 44);
            heading(box, missing ? Strings.SETUP_AGAIN_ROW : Strings.STORAGE_TITLE, 34);
            lead(box, missing ? (StorageLocation.accessLost(this) ? Strings.accessLost(StorageLocation.label(this)) : Strings.missingVolume(StorageLocation.label(this)))
                    : Strings.STORAGE_LEAD, 10);
            detailPanel();
            detailNote.setVisibility(View.GONE);
            detailDiagram.setVisibility(View.GONE);
            detailConstraint.setVisibility(View.GONE);
            detailTitle.setText(Strings.STORAGE_ROW);
            detailText.setText(Strings.STORAGE_TEXT);
            if (moving) fact(Strings.INSTALLED, Strings.size(size));
            for (StorageLocation.Volume v : volumes) fact(v.name, Strings.bytes(v.free) + " free");
        }
        int cards = volumes.size() + 1;
        float height = cards >= 4 ? 114 : 150, gap = cards >= 4 ? 12 : 18;
        Widgets.Card first = null;
        for (int i = 0; i < volumes.size(); i++) {
            StorageLocation.Volume v = volumes.get(i);
            boolean inUse = !missing && v.same(current), room = v.free >= need;
            Widgets.Card card = new Widgets.Card(this, v.internal ? "data" : "sd", v.name,
                    Strings.volumeLine(v.free, need) + (v.kind == StoragePaths.Kind.PRIVATE ? " · " + Strings.FASTER : "") + (v.fat ? " · FAT" : ""),
                    view -> chooseVolume(v, setup, moving, need));
            card.setTag("volume:" + v.dir.getPath());
            card.stateWidth(300);
            if (inUse && !setup) card.state("✓ " + Strings.IN_USE_STATE, Theme.ZONAI, null);
            else if (!room) card.state(Strings.NOT_ENOUGH, Theme.WARN, null);
            else if (v == recommended) card.state(Strings.RECOMMENDED_SHORT, Theme.ZONAI, null);
            else card.state(Strings.CHOOSE_STATE, Theme.TEXT2, null);
            box.addView(card, Widgets.margins(Widgets.size(-1, height), 0, i == 0 ? 30 : gap, 0, 0));
            if (moving && !inUse && room) gameControl(card, "Moving game data", () -> chooseVolume(v, setup, true, need));
            if (first == null) first = card;
            if (primary == null && (setup ? v == recommended : inUse)) primary = card;
        }
        Widgets.Card folder = new Widgets.Card(this, "data", Strings.FOLDER_CARD, Strings.FOLDER_CARD_LINE, view -> chooseFolder(setup, moving, need));
        folder.setTag("volume:folder");
        folder.stateWidth(300);
        folder.state(granted ? Strings.CHOOSE_STATE : Strings.NEEDS_ACCESS, granted ? Theme.TEXT2 : Theme.AMBER, null);
        box.addView(folder, Widgets.margins(Widgets.size(-1, height), 0, volumes.isEmpty() ? 30 : gap, 0, 0));
        if (moving) gameControl(folder, "Moving game data", () -> chooseFolder(setup, true, need));
        if (first == null) first = folder;
        // The optional permission step: in setup below the cards, on Game data under the facts.
        access = listRow(setup ? box : detailFacts, "access", Strings.ACCESS_ROW, Strings.ACCESS_HINT, 76, 0);
        access.value(rowText(granted ? Strings.ACCESS_ON : Strings.ACCESS_OFF, granted ? Theme.ZONAI : Theme.AMBER));
        access.setOnClickListener(v -> openAccessSettings());
        if (setup) ((LinearLayout.LayoutParams) access.getLayoutParams()).topMargin = Theme.u(gap);
        if (primary == null) primary = first;
        prompts(prompt("A", Strings.P_CHOOSE, v -> selectFocused()), backPrompt());
    }
    private void chooseVolume(StorageLocation.Volume v, boolean setup, boolean moving, long need) {
        if (SetupService.status.running) return;
        boolean missing = !StorageLocation.available(this);
        if (!missing && v.same(StorageLocation.gameDir(this)) && !setup) { back(); return; }
        if (v.free < need) { showNote(Strings.NOT_ENOUGH + " · " + Strings.volumeLine(v.free, need)); return; }
        if (moving) {
            if (!needGameClosed("Moving game data", () -> chooseVolume(v, setup, true, need))) return;
            requestNotificationsThen(() -> {
                SetupService.clearResult(this);
                installSnapshot = null;
                SetupService.startMove(this, v);
                SetupService.status.stages = SetupService.MOVE;
                replace(Screen.INSTALL);
            });
            return;
        }
        // Nothing to move: this only decides where the (next) import goes.
        if (v.kind == StoragePaths.Kind.FOLDER && !v.dir.mkdirs() && !v.dir.isDirectory()) { showNote(Strings.folderProblem("")); return; }
        StorageLocation.choose(this, v);
        SettingsStore.writeEnv(this, settings);
        DataSize.invalidate();
        if (setup) { go(Screen.ROUTE); return; }
        history.clear(); history.push(Screen.WELCOME);
        screen = Screen.ROUTE; note = ""; restoreFocus = PRIMARY;
        render();
    }
    /** "A folder you choose": needs All files access (asked first, optional), then the folder picker; the pick is used
     * by its real path (FileAccess.treePath), so a folder without one is refused like Azahar's invalid selection. */
    private void chooseFolder(boolean setup, boolean moving, long need) {
        if (SetupService.status.running) return;
        if (!FileAccess.granted()) { askAccess(); return; }
        folderForSetup = setup; folderMoving = moving; folderNeed = need;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        try { startActivityForResult(intent, REQ_DATA_FOLDER); } catch (ActivityNotFoundException missing) { showNote(Strings.PICKER_MISSING); }
    }
    private void usePickedFolder(Uri tree) {
        boolean setup = folderForSetup, moving = folderMoving;
        long need = folderNeed;
        worker.execute(() -> {
            String path = FileAccess.treePath(this, tree), problem;
            StorageLocation.Volume v = null;
            if (path == null) problem = "";
            else {
                File target = StorageLocation.folderTarget(new File(path));
                problem = StorageLocation.folderProblem(this, target);
                if (problem == null && (v = StorageLocation.folder(this, target)) == null) problem = "";
            }
            StorageLocation.Volume picked = v;
            String why = problem;
            android.util.Log.i("TotkSetup", "game-data folder pick: " + tree + " -> " + path + (why == null ? " ok " + picked.dir : " refused (" + why + ")"));
            main.post(() -> {
                if (why != null) { showNote(Strings.folderProblem(why)); return; }
                chooseVolume(picked, setup, moving, need);
            });
        });
    }
    /** Explain All files access and open its settings page; declining keeps the file picker (no further asking). */
    private void askAccess() {
        openChoice("data", Strings.GAME_DATA, Strings.ACCESS_TITLE, Strings.ACCESS_TEXT,
                new Object[]{"access:allow", Strings.ACCESS_ALLOW, Strings.ACCESS_ALLOW_HINT, false, (Runnable) () -> { back(); openAccessSettings(); }},
                new Object[]{"access:skip", Strings.ACCESS_SKIP, Strings.ACCESS_SKIP_HINT, false, (Runnable) this::back});
    }
    /** Settings' All files access page for this app (re-checked when the menu comes back, onStart). */
    private void openAccessSettings() {
        try { startActivityForResult(FileAccess.settingsIntent(this), REQ_ACCESS); }
        catch (ActivityNotFoundException noPage) {
            try { startActivityForResult(FileAccess.settingsFallback(), REQ_ACCESS); }
            catch (ActivityNotFoundException none) { showNote(Strings.PICKER_MISSING); }
        }
    }
    /** Home's one-time offer: move an install still in the old internal app folder to private storage (the verified
     * move; the old copy stays playable until it has succeeded). "Not now" is remembered. */
    private void askMigration() {
        if (screen != Screen.HOME) return;
        openChoice("data", Strings.GAME_DATA, Strings.MIGRATE_TITLE, Strings.MIGRATE_BODY,
                new Object[]{"migrate:now", Strings.MIGRATE_NOW, Strings.MIGRATE_NOW_HINT, false, (Runnable) () -> { back(); startMigration(); }},
                new Object[]{"migrate:later", Strings.MIGRATE_LATER, Strings.MIGRATE_LATER_HINT, false, (Runnable) () -> {
                    StorageLocation.declineMigration(this);
                    back();
                }});
    }
    private void startMigration() {
        worker.execute(() -> {
            StorageLocation.Volume own = null;
            for (StorageLocation.Volume v : StorageLocation.volumes(this)) if (v.kind == StoragePaths.Kind.PRIVATE) own = v;
            long size = DataSize.peek();
            long need = size > 0 ? size + 500_000_000L : InstallState.ROMFS_SIZE + 2_000_000_000L;
            StorageLocation.Volume target = own;
            main.post(() -> {
                if (target == null) return;
                if (target.free < need) { showNote(Strings.NOT_ENOUGH + " · " + Strings.volumeLine(target.free, need)); return; }
                chooseVolume(target, false, true, need);
            });
        });
    }

    // ---------- saves ----------
    /** Reload the live slots, their pictures and the backup list in the background, then redraw the save screens. */
    private void loadSaves() {
        File files = getFilesDir();
        saveWorker.execute(() -> {
            File root = SaveManager.existingRoot(files);
            List<SaveManager.Slot> slots = SaveManager.slots(root);
            for (SaveManager.Slot s : slots) decodeCaption(s);
            List<SaveManager.Backup> backups = SaveManager.backups(SaveManager.backupsDir(files));
            main.post(() -> {
                liveSlots = slots; backupList = backups;
                if (chosenBackup != null) {
                    SaveManager.Backup still = null;
                    for (SaveManager.Backup b : backups) if (b.dir.equals(chosenBackup.dir)) still = b;
                    chosenBackup = still;
                }
                if (screen == Screen.HOME || screen == Screen.WELCOME || screen == Screen.SAVES || screen == Screen.BACKUPS) render();
            });
        });
    }
    private static String pictureKey(File caption) { return caption.getPath() + ":" + caption.lastModified(); }
    /** Decode a slot's caption.sav picture into the cache (saveWorker only). */
    private void decodeCaption(SaveManager.Slot slot) {
        if (slot.caption == null) return;
        String key = pictureKey(slot.caption);
        if (pictures.containsKey(key)) return;
        Object picture = Boolean.FALSE;
        try {
            if (slot.caption.length() <= 1 << 20) {
                byte[] data = java.nio.file.Files.readAllBytes(slot.caption.toPath());
                int[] jpeg = SaveManager.captionJpeg(data);
                android.graphics.Bitmap bitmap = jpeg == null ? null : android.graphics.BitmapFactory.decodeByteArray(data, jpeg[0], jpeg[1]);
                if (bitmap != null) picture = bitmap;
            }
        } catch (IOException | RuntimeException unreadable) { }
        pictures.put(key, picture);
    }
    private List<Widgets.SlotGrid.Cell> cells(List<SaveManager.Slot> slots) {
        List<Widgets.SlotGrid.Cell> out = new ArrayList<>();
        for (SaveManager.Slot s : slots) {
            Object picture = s.caption == null ? null : pictures.get(pictureKey(s.caption));
            out.add(new Widgets.SlotGrid.Cell(picture instanceof android.graphics.Bitmap ? (android.graphics.Bitmap) picture : null,
                    Strings.slotTitle(s.index), Strings.date(s.time)));
        }
        return out;
    }
    private String folderText() {
        if (!SaveMirror.enabled(this)) return Strings.FOLDER_OFF;
        String state = SaveMirror.status(this).state;
        if (SaveMirror.UNAVAILABLE.equals(state)) return Strings.FOLDER_UNAVAILABLE;
        if (SaveMirror.INCOMPLETE.equals(state)) return Strings.FOLDER_INCOMPLETE_SHORT;
        if (SaveMirror.CONFLICT.equals(state)) return Strings.FOLDER_CONFLICT;
        return Strings.folderValue(SaveMirror.folderName(this), SaveMirror.syncedAt(this));
    }
    /** The lowercase save-folder phrase of the Home/Welcome summary line. */
    private String folderSummary() {
        if (!SaveMirror.enabled(this)) return Strings.folderSummary("off", null);
        return Strings.folderSummary(SaveMirror.status(this).state, SaveMirror.folderName(this));
    }
    private int folderColor() {
        if (!SaveMirror.enabled(this)) return Theme.TEXT2;
        String state = SaveMirror.status(this).state;
        return SaveMirror.UNAVAILABLE.equals(state) || SaveMirror.ERROR.equals(state) || SaveMirror.INCOMPLETE.equals(state) ? Theme.WARN
                : SaveMirror.CONFLICT.equals(state) ? Theme.AMBER : Theme.ZONAI;
    }
    /** The right-hand panel of the save screens: title, one line, the slot pictures, facts. */
    private void savesDetail() {
        Widgets.Panel detail = panel(1160, 186, 680, 720, true, 44, 40);
        detailTitle = Theme.text(this, "", true, 34, Theme.TEXT);
        detail.addView(detailTitle);
        detailText = add(detail, Theme.paragraph(this, "", 19, Theme.TEXT2), 12);
        slotGrid = new Widgets.SlotGrid(this);
        detail.addView(slotGrid, Widgets.margins(new LinearLayout.LayoutParams(-2, -2), 0, 22, 0, 0));
        detailFacts = Widgets.column(this);
        detail.addView(detailFacts, Widgets.margins(Widgets.size(-1, -2), 0, 12, 0, 0));
    }
    private void showSlots(String title, String line, List<SaveManager.Slot> slots) {
        if (detailTitle == null) return;
        detailFacts.removeAllViews();
        detailTitle.setText(title);
        detailText.setText(line);
        slotGrid.setVisibility(View.VISIBLE);
        slotGrid.set(cells(slots));
    }
    private static String slotsLine(List<SaveManager.Slot> slots) {
        return slots == null || slots.isEmpty() ? Strings.SAVES_NONE : Strings.slotCount(slots.size()) + " · newest " + Strings.date(SaveManager.newest(slots));
    }
    private Widgets.Row saveRow(LinearLayout parent, String id, String icon, String label, String hint, String state, int color, View.OnClickListener click) {
        Widgets.Row row = new Widgets.Row(this, label, hint, 27, 18).icon(icon, 56, 28);
        row.setTag("saves:" + id);
        if (!state.isEmpty()) row.value(rowText(state, color));
        row.setOnClickListener(v -> { if (!row.dimmed() && !saveBusy) click.onClick(v); });
        row.setOnFocusChangeListener((v, has) -> { if (has) savesFocus(id); });
        parent.addView(row, Widgets.rowParams(104, 12));
        return row;
    }
    private void saves() {
        statusChip = header("save", Strings.SAVES, noteOk ? Widgets.Chip.OK : Widgets.Chip.WARN, note.isEmpty() ? null : note);
        int slots = liveSlots == null ? 0 : liveSlots.size();
        boolean mirror = SaveMirror.enabled(this);
        Widgets.Panel list = panel(80, 186, 1040, 720, false, 28, 24);
        Widgets.Row export = saveRow(list, "export", "export", Strings.SAVES_EXPORT, Strings.SAVES_EXPORT_HINT,
                slots == 0 ? Strings.SAVES_NONE : Strings.slotCount(slots), Theme.TEXT2, v -> exportSaves());
        export.dimmed(slots == 0);
        Widgets.Row importRow = saveRow(list, "import", "import", Strings.SAVES_IMPORT, Strings.SAVES_IMPORT_HINT, "", Theme.TEXT2, v -> chooseImport());
        saveRow(list, "folder", "folder", Strings.SAVES_FOLDER, Strings.SAVES_FOLDER_HINT, folderText(), folderColor(), v -> folderAction());
        Widgets.Row sync = saveRow(list, "sync", "refresh", Strings.SAVES_SYNC, Strings.SAVES_SYNC_HINT, syncing ? Strings.SYNCING : "", Theme.AMBER, v -> syncMirror(true));
        sync.dimmed(!mirror);
        gameControl(importRow, "Importing saves", this::chooseImport);
        gameControl(sync, "Synchronizing saves", () -> syncMirror(true));
        saveRow(list, "backups", "clock", Strings.SAVES_BACKUPS, Strings.SAVES_BACKUPS_HINT, Strings.backupCount(backupList.size()), Theme.TEXT2, v -> go(Screen.BACKUPS));
        savesDetail();
        primary = slots == 0 ? importRow : export;
        showSlots(Strings.YOUR_SAVES, slotsLine(liveSlots), liveSlots == null ? new ArrayList<>() : liveSlots);
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt(), startPrompt());
    }
    private void savesFocus(String id) {
        if (detailTitle == null) return;
        switch (id) {
            case "export": case "import":
                showSlots(Strings.YOUR_SAVES, id.equals("import") ? Strings.SAVES_TEXT : slotsLine(liveSlots), liveSlots == null ? new ArrayList<>() : liveSlots);
                break;
            case "folder": case "sync": {
                slotGrid.setVisibility(View.GONE);
                detailFacts.removeAllViews();
                detailTitle.setText(Strings.FOLDER_TITLE);
                detailText.setText(Strings.FOLDER_TEXT);
                boolean on = SaveMirror.enabled(this);
                fact(Strings.FOLDER_ROW_FACT, on ? SaveMirror.folderName(this) : Strings.FOLDER_OFF);
                if (on) {
                    fact(Strings.LAST_SYNCED, Strings.date(SaveMirror.syncedAt(this)));
                    SaveMirror.Status status = SaveMirror.status(this);
                    String text = SaveMirror.UNAVAILABLE.equals(status.state) ? Strings.FOLDER_UNAVAILABLE
                            : SaveMirror.INCOMPLETE.equals(status.state) ? Strings.FOLDER_INCOMPLETE
                            : SaveMirror.CONFLICT.equals(status.state) ? Strings.CONFLICT_CHIP
                            : SaveMirror.ERROR.equals(status.state) ? Strings.saveError("", status.message) : Strings.READY;
                    fact(Strings.STATUS_FACT, text);
                }
                break;
            }
            default: {
                slotGrid.setVisibility(View.GONE);
                detailFacts.removeAllViews();
                detailTitle.setText(Strings.SAVES_BACKUPS);
                detailText.setText(Strings.BACKUPS_TEXT);
                fact(Strings.SAVES_BACKUPS, Strings.backupCount(backupList.size()));
                if (!backupList.isEmpty()) fact(Strings.NEWEST_FACT, Strings.date(backupList.get(0).created) + " · " + Strings.reason(backupList.get(0).reason));
            }
        }
    }

    /** A note that belongs to the save screens (they may have been left while the work ran). */
    private void saveNote(String text, boolean ok) {
        note = text; noteOk = ok;
        render();
    }
    private void exportSaves() {
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                .putExtra(Intent.EXTRA_TITLE, SaveManager.exportName(System.currentTimeMillis()));
        try { startActivityForResult(intent, REQ_EXPORT); } catch (ActivityNotFoundException missing) { saveNote(Strings.PICKER_MISSING, false); }
    }
    private void writeExport(Uri uri) {
        ContentResolver resolver = getContentResolver();
        String name = Inputs.describe(resolver, uri).name;
        saveBusy = true; saveNote(Strings.EXPORTING, true);
        saveWorker.execute(() -> {
            String failed = null;
            try { SaveMirror.export(this, uri); }
            catch (SaveManager.Invalid invalid) { failed = Strings.saveError(invalid.code, null); }
            catch (IOException | RuntimeException error) { failed = Strings.saveError("", error.getMessage()); }
            if (failed != null) try { android.provider.DocumentsContract.deleteDocument(resolver, uri); } catch (Exception leftover) { }
            String result = failed;
            main.post(() -> { saveBusy = false; saveNote(result == null ? Strings.exported(name) : result, result == null); });
        });
    }
    private void chooseImport() {
        openChoice("import", Strings.SAVES, Strings.IMPORT_FROM, null,
                new Object[]{"import:zip", Strings.IMPORT_ZIP, Strings.IMPORT_ZIP_HINT, false, (Runnable) () -> pickImport(false)},
                new Object[]{"import:tree", Strings.IMPORT_TREE, Strings.IMPORT_TREE_HINT, false, (Runnable) () -> pickImport(true)});
    }
    private void pickImport(boolean tree) {
        if (!needGameClosed("Importing saves", () -> pickImport(tree))) return;
        if (screen == Screen.CHOICE) back();
        Intent intent = tree ? new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                : new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try { startActivityForResult(intent, tree ? REQ_IMPORT_TREE : REQ_IMPORT_ZIP); } catch (ActivityNotFoundException missing) { saveNote(Strings.PICKER_MISSING, false); }
    }
    /** Check a picked zip or folder and stage its save set, then show what was found before anything changes. */
    private void stageImport(Uri uri, boolean tree) {
        if (!needGameClosed("Importing saves", () -> stageImport(uri, tree))) return;
        String source = tree ? treeName(uri) : Inputs.describe(getContentResolver(), uri).name;
        saveBusy = true; saveNote(Strings.CHECKING_SAVES, true);
        File staged = SaveManager.staging(getFilesDir(), "import");
        saveWorker.execute(() -> {
            String failed = null;
            SaveManager.Selection selection = null;
            List<SaveManager.Slot> slots = new ArrayList<>();
            try {
                selection = tree ? SaveMirror.stageFolder(this, uri, staged) : SaveMirror.stageZip(this, uri, staged);
                slots = SaveManager.slots(staged);
                for (SaveManager.Slot s : slots) decodeCaption(s);
            } catch (SaveManager.Invalid invalid) { failed = Strings.saveError(invalid.code, invalid.getMessage()); }
            catch (SaveMirror.Unavailable gone) { failed = Strings.saveError("unavailable", null); }
            catch (IOException | RuntimeException error) { failed = Strings.saveError("", error.getMessage()); }
            if (failed != null) SaveManager.delete(staged);
            String result = failed;
            int files = selection == null ? 0 : selection.files.size();
            List<SaveManager.Slot> found = slots;
            main.post(() -> {
                saveBusy = false;
                if (result != null) { saveNote(result, false); return; }
                importStaged = staged; importSource = source; importSlots = found; importFiles = files;
                go(Screen.SAVE_IMPORT);
            });
        });
    }
    private static String treeName(Uri tree) {
        String id;
        try { id = android.provider.DocumentsContract.getTreeDocumentId(tree); } catch (IllegalArgumentException bad) { return "?"; }
        String name = id.substring(id.lastIndexOf(':') + 1);
        name = name.substring(name.lastIndexOf('/') + 1);
        return name.isEmpty() ? id : name;
    }
    private void importPreview() {
        header("save", Strings.SAVES_IMPORT, Widgets.Chip.OK, Strings.IMPORT_CHIP);
        Widgets.Panel box = panel(80, 186, 1040, 720, true, 50, 44);
        heading(box, Strings.importFound(importSlots.size()), 36);
        lead(box, Strings.importLead(importSource, SaveManager.newest(importSlots), liveSlots == null ? 0 : liveSlots.size()), 12);
        add(box, Theme.text(this, Strings.files(importFiles), false, 19, Theme.TEXT3), 12);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        Widgets.Row replace = listRow(box, "import:replace", Strings.REPLACE_SAVES, Strings.REPLACE_SAVES_HINT, 86, 10);
        replace.labelColor(Theme.AMBER);
        replace.setOnClickListener(v -> confirmImport());
        gameControl(replace, "Replacing saves", this::confirmImport);
        Widgets.Row keep = listRow(box, "import:keep", Strings.KEEP_SAVES, Strings.KEEP_SAVES_HINT, 86, 0);
        keep.setOnClickListener(v -> discardImport());
        savesDetail();
        showSlots(Strings.importFound(importSlots.size()), slotsLine(importSlots), importSlots);
        primary = keep;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), prompt("B", Strings.P_BACK, v -> discardImport()));
    }
    private void discardImport() {
        File staged = importStaged;
        importStaged = null;
        if (staged != null) saveWorker.execute(() -> SaveManager.delete(staged));
        back();
    }
    private void confirmImport() {
        File staged = importStaged;
        if (staged == null || saveBusy) return;
        if (!needGameClosed("Replacing saves", this::confirmImport)) return;
        int slots = importSlots.size();
        importStaged = null;
        saveBusy = true;
        back();
        saveNote(Strings.IMPORTING, true);
        saveWorker.execute(() -> {
            String failed = null; boolean backedUp = false;
            try { backedUp = SaveManager.replace(getFilesDir(), staged, "import", System.currentTimeMillis()) != null; }
            catch (SaveManager.Invalid invalid) { failed = Strings.saveError(invalid.code, null); }
            catch (IOException | RuntimeException error) { failed = Strings.saveError("", error.getMessage()); }
            SaveManager.delete(staged);
            String result = failed; boolean madeBackup = backedUp;
            main.post(() -> {
                saveBusy = false;
                saveNote(result == null ? Strings.imported(slots, madeBackup) : result, result == null);
                loadSaves();
                if (result == null) syncMirror(false);   // the imported set is the new truth for the save folder
            });
        });
    }

    // ---------- save folder (mirror) ----------
    private void folderAction() {
        if (!SaveMirror.enabled(this)) { pickMirror(); return; }
        String state = SaveMirror.status(this).state;
        if (SaveMirror.CONFLICT.equals(state)) { inspectConflict(); return; }
        List<Object[]> options = new ArrayList<>();
        if (SaveMirror.INCOMPLETE.equals(state))
            options.add(new Object[]{"folder:replace", Strings.FOLDER_REPLACE, Strings.FOLDER_REPLACE_HINT, false, (Runnable) () -> { back(); resolveConflict(true); }});
        options.add(new Object[]{"folder:change", Strings.FOLDER_CHANGE, Strings.FOLDER_CHANGE_HINT, false, (Runnable) () -> { back(); pickMirror(); }});
        options.add(new Object[]{"folder:off", Strings.FOLDER_TURN_OFF, Strings.FOLDER_TURN_OFF_HINT, true, (Runnable) () -> {
            SaveMirror.turnOff(this);
            back();
            saveNote(Strings.FOLDER_TURNED_OFF, true);
        }});
        openChoice("folder", Strings.SAVES, Strings.FOLDER_TITLE + " · " + SaveMirror.folderName(this),
                SaveMirror.INCOMPLETE.equals(state) ? Strings.FOLDER_INCOMPLETE : Strings.FOLDER_TEXT, options.toArray(new Object[0][]));
    }
    private void pickMirror() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try { startActivityForResult(intent, REQ_MIRROR); } catch (ActivityNotFoundException missing) { saveNote(Strings.PICKER_MISSING, false); }
    }
    private void useMirror(Uri tree) {
        syncing = true; saveNote(Strings.SYNCING, true);
        saveWorker.execute(() -> {
            SaveMirror.Result r;
            try { SaveMirror.setFolder(this, tree); r = SaveMirror.sync(this, true); }
            catch (IOException failed) { r = new SaveMirror.Result(SaveMirror.UNAVAILABLE); r.error = String.valueOf(failed.getMessage()); }
            SaveMirror.Result result = r;
            main.post(() -> synced(result, true));
        });
    }
    /** Sync with the save folder in the background; manual = Sync now (reports every outcome). */
    private void syncMirror(boolean manual) {
        if (!SaveMirror.enabled(this) || syncing) return;
        if (manual && !needGameClosed("Synchronizing saves", () -> syncMirror(true))) return;
        syncing = true;
        if (manual) saveNote(Strings.SYNCING, true);
        saveWorker.execute(() -> {
            SaveMirror.Result r = SaveMirror.sync(this, true);
            main.post(() -> synced(r, manual));
        });
    }
    private void synced(SaveMirror.Result r, boolean manual) {
        syncing = false;
        boolean menu = screen == Screen.HOME || screen == Screen.SAVES || screen == Screen.WELCOME;
        switch (r.action) {
            case "pulled": SaveMirror.takePulled(this); pulledChip = true; loadSaves(); if (manual) saveNote(Strings.PULLED, true); break;
            case "pushed": if (manual) saveNote(Strings.PUSHED, true); break;
            case "none": if (manual) saveNote(Strings.SYNCED, true); break;
            case "pull_pending": if (manual) saveNote(Strings.PULL_LATER, false); break;
            case SaveMirror.CONFLICT:
                startAfterSync = false;
                if (menu) { openConflict(r); return; }
                break;
            case SaveMirror.INCOMPLETE: if (manual || screen == Screen.SAVES) saveNote(Strings.FOLDER_INCOMPLETE, false); break;
            case SaveMirror.UNAVAILABLE: if (manual || screen == Screen.SAVES) saveNote(Strings.FOLDER_UNAVAILABLE, false); break;
            default: if (manual) saveNote(Strings.saveError("", r.error), false);
        }
        if (menu && !manual) render();
        if (startAfterSync) { startAfterSync = false; startGame(); }
    }
    private void inspectConflict() {
        saveWorker.execute(() -> {
            SaveMirror.Result r = SaveMirror.inspectConflict(this);
            main.post(() -> { if (SaveMirror.CONFLICT.equals(r.action)) openConflict(r); else syncMirror(true); });
        });
    }
    private void openConflict(SaveMirror.Result r) { conflict = r; go(Screen.SAVE_CONFLICT); }
    private void conflictScreen() {
        header("save", Strings.SAVES, Widgets.Chip.WARN, Strings.CONFLICT_CHIP);
        SaveMirror.Result c = conflict == null ? new SaveMirror.Result(SaveMirror.CONFLICT) : conflict;
        Widgets.Panel box = panel(360, 186, 1200, 720, true, 56, 48);
        heading(box, Strings.CONFLICT_TITLE, 34);
        lead(box, Strings.CONFLICT_LEAD, 12);
        Widgets.Card device = new Widgets.Card(this, "save", Strings.THIS_DEVICE, Strings.side(c.liveSlots, c.liveTime, Build.MODEL), v -> resolveConflict(true));
        device.setTag("conflict:device"); device.stateWidth(260); device.state(Strings.KEEP_THIS, Theme.ZONAI, null);
        box.addView(device, Widgets.margins(Widgets.size(-1, 140), 0, 28, 0, 0));
        Widgets.Card folder = new Widgets.Card(this, "folder", Strings.THE_FOLDER + " · " + SaveMirror.folderName(this),
                Strings.side(c.folderSlots, c.folderTime, c.folderDevice), v -> resolveConflict(false));
        folder.setTag("conflict:folder"); folder.stateWidth(260); folder.state(Strings.KEEP_THIS, Theme.ZONAI, null);
        box.addView(folder, Widgets.margins(Widgets.size(-1, 140), 0, 18, 0, 0));
        gameControl(folder, "Replacing saves", () -> resolveConflict(false));
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        Widgets.Row later = listRow(box, "conflict:later", Strings.DECIDE_LATER, Strings.DECIDE_LATER_HINT, 80, 0);
        later.setOnClickListener(v -> back());
        primary = c.folderTime > c.liveTime ? folder : device;
        prompts(prompt("A", Strings.P_CHOOSE, v -> selectFocused()), backPrompt());
    }
    /** keepDevice: this device's saves go to the folder (the folder's copy is backed up here first); else the folder's
     * saves replace this device's (backed up first). */
    private void resolveConflict(boolean keepDevice) {
        if (saveBusy) return;
        if (!keepDevice && !needGameClosed("Replacing saves", () -> resolveConflict(false))) return;
        saveBusy = true; syncing = true;
        if (screen == Screen.SAVE_CONFLICT) back();
        saveNote(Strings.SYNCING, true);
        saveWorker.execute(() -> {
            SaveMirror.Result r = SaveMirror.resolve(this, keepDevice);
            main.post(() -> { saveBusy = false; conflict = null; loadSaves(); synced(r, true); });
        });
    }

    // ---------- backups ----------
    private void backups() {
        statusChip = header("clock", Strings.SAVES_BACKUPS, noteOk ? Widgets.Chip.OK : Widgets.Chip.WARN, note.isEmpty() ? null : note);
        Widgets.Panel list = panel(80, 186, 1040, 720, false, 28, 28);
        list.setClipChildren(true);   // the scrolled list must stay inside the panel
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false); scroll.setClipToPadding(false);
        LinearLayout column = Widgets.column(this);
        column.setPadding(Theme.u(8), Theme.u(4), Theme.u(8), Theme.u(4));
        scroll.addView(column);
        list.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        savesDetail();
        if (backupList.isEmpty()) {
            column.addView(Theme.paragraph(this, Strings.BACKUPS_TEXT, 21, Theme.TEXT2));
            detailTitle.setText(Strings.SAVES_BACKUPS); detailText.setText(Strings.backupCount(0)); slotGrid.setVisibility(View.GONE);
        }
        for (SaveManager.Backup b : backupList) {
            Widgets.Row row = listRow(column, "backup:" + b.dir.getName(), Strings.date(b.created),
                    Strings.reason(b.reason) + " · " + Strings.slotCount(Math.max(0, b.slots)), 86, 8);
            row.setOnClickListener(v -> { if (!saveBusy) confirmRestore(b); });
            row.setOnFocusChangeListener((v, has) -> { if (has) { chosenBackup = b; backupDetail(b); } });
            gameControl(row, "Restoring saves", () -> confirmRestore(b));
            if (primary == null) primary = row;
        }
        prompts(prompt("A", Strings.P_RESTORE, v -> selectFocused()), prompt("X", Strings.P_DELETE, v -> extra()), backPrompt(), startPrompt());
    }
    private void backupDetail(SaveManager.Backup b) {
        List<SaveManager.Slot> slots = SaveManager.slots(b.saves());
        showSlots(Strings.date(b.created), Strings.reason(b.reason), slots);
        boolean pending = false;
        for (SaveManager.Slot s : slots) if (s.caption != null && !pictures.containsKey(pictureKey(s.caption))) pending = true;
        if (!pending) return;
        saveWorker.execute(() -> {
            for (SaveManager.Slot s : slots) decodeCaption(s);
            main.post(() -> { if (screen == Screen.BACKUPS && chosenBackup == b) showSlots(Strings.date(b.created), Strings.reason(b.reason), slots); });
        });
    }
    private void confirmRestore(SaveManager.Backup b) {
        openChoice("clock", Strings.SAVES_BACKUPS, Strings.RESTORE_TITLE, Strings.restoreBody(b.created, b.slots),
                new Object[]{"restore:yes", Strings.RESTORE, Strings.RESTORE_HINT, false, (Runnable) () -> restoreBackup(b)},
                new Object[]{"restore:no", Strings.KEEP_CURRENT, Strings.KEEP_SAVES_HINT, false, (Runnable) this::back});
    }
    private void confirmDeleteBackup(SaveManager.Backup b) {
        openChoice("clock", Strings.SAVES_BACKUPS, Strings.DELETE_BACKUP_TITLE, Strings.deleteBackupBody(b.created),
                new Object[]{"delete:no", Strings.KEEP_BACKUP, Strings.KEEP_SAVES_HINT, false, (Runnable) this::back},
                new Object[]{"delete:yes", Strings.DELETE_BACKUP, null, true, (Runnable) () -> {
                    back();
                    saveBusy = true;
                    saveWorker.execute(() -> {
                        SaveManager.delete(b.dir);
                        main.post(() -> { saveBusy = false; chosenBackup = null; saveNote(Strings.BACKUP_DELETED, true); loadSaves(); });
                    });
                }});
    }
    private void restoreBackup(SaveManager.Backup b) {
        if (!needGameClosed("Restoring saves", () -> restoreBackup(b))) return;
        if (screen == Screen.CHOICE) back();
        saveBusy = true; saveNote(Strings.RESTORING, true);
        saveWorker.execute(() -> {
            String failed = null; boolean backedUp = false;
            try { backedUp = SaveManager.restore(getFilesDir(), b, System.currentTimeMillis()) != null; }
            catch (SaveManager.Invalid invalid) { failed = Strings.saveError(invalid.code, null); }
            catch (IOException | RuntimeException error) { failed = Strings.saveError("", error.getMessage()); }
            String result = failed; boolean madeBackup = backedUp;
            main.post(() -> {
                saveBusy = false;
                saveNote(result == null ? Strings.restored(madeBackup) : result, result == null);
                loadSaves();
                if (result == null) syncMirror(false);
            });
        });
    }

    // ---------- small choice dialog ----------
    /** options: {tag, label, hint (or null), warn label?, Runnable}. */
    private void openChoice(String icon, String header, String title, String body, Object[]... options) {
        choiceIcon = icon; choiceHeader = header; choiceTitle = title; choiceBody = body == null ? "" : body;
        choiceOptions.clear(); choiceOptions.addAll(Arrays.asList(options));
        go(Screen.CHOICE);
    }
    private void choice() {
        header(choiceIcon, choiceHeader, 0, null);
        float h = (choiceBody.isEmpty() ? 170 : 400) + 96 * choiceOptions.size();
        Widgets.Panel box = panel(460, Math.max(186, 560 - h / 2), 1000, h, true, 56, 48);
        heading(box, choiceTitle, 34);
        if (!choiceBody.isEmpty()) lead(box, choiceBody, 16);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        for (int i = 0; i < choiceOptions.size(); i++) {
            Object[] o = choiceOptions.get(i);
            Widgets.Row row = listRow(box, (String) o[0], (String) o[1], (String) o[2], 86, i == choiceOptions.size() - 1 ? 0 : 10);
            if ((Boolean) o[3]) row.labelColor(Theme.WARN);
            Runnable action = (Runnable) o[4];
            row.setOnClickListener(v -> action.run());
            if (primary == null) primary = row;
        }
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt());
    }

    // ---------- about and licences ----------
    private void about() {
        header("about", Strings.ABOUT, 0, null);
        String appVersion = "?"; long appCode = 0;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            appVersion = info.versionName; appCode = info.getLongVersionCode();
        } catch (PackageManager.NameNotFoundException impossible) { }
        InstallState install = InstallState.read(this);
        String version = Strings.version(appVersion, appCode);
        Widgets.Panel list = panel(80, 186, 1040, 720, false, 28, 28);
        Widgets.Row versionRow = listRow(list, "about:version", Strings.VERSION_ROW, version, 92, 18);
        Widgets.Row licencesRow = listRow(list, "about:licences", Strings.LICENCES_ROW, Strings.LICENCES_HINT, 92, 18);
        licencesRow.setOnClickListener(v -> go(Screen.LICENSES));
        Widgets.Row project = listRow(list, "about:project", Strings.PROJECT_ROW, Strings.PROJECT_URL, 92, 18);
        Widgets.Row updateRow = updateSwitch(list);
        detailPanel();
        detailNote.setVisibility(View.GONE);
        View.OnFocusChangeListener detail = (v, has) -> {
            if (!has) return;
            detailFacts.removeAllViews(); detailDiagram.setVisibility(View.GONE); detailConstraint.setVisibility(View.GONE);
            if (v == versionRow) {
                detailTitle.setText(Strings.APP); detailText.setText(Strings.ABOUT_TEXT);
                fact(Strings.VERSION_ROW, version);
                fact("Game", Strings.GAME_VERSION);
                fact("Build", Strings.build(buildCommit()));
                fact(Strings.DETECTED, detected());
                if (install.setupVersion() > 0) fact("Install", Strings.setupFormat(install.setupVersion(), install.route()));
                fact("Setup", Strings.library(InstallState.libraryAvailable()));
            } else if (v == licencesRow) {
                detailTitle.setText(Strings.LICENCES_ROW); detailText.setText(Strings.LICENCES_TEXT);
                fact("Files", String.valueOf(licenceFiles().size()));
            } else if (v == updateRow) {
                detailTitle.setText(Strings.UPDATES); detailText.setText(Strings.UPDATE_PRIVACY);
            } else {
                detailTitle.setText(Strings.PROJECT_ROW); detailText.setText(Strings.PROJECT_TEXT);
                fact(Strings.UPSTREAM_ROW, Strings.UPSTREAM_BY);
                fact(Strings.ANDROID_PORT_ROW, Strings.ANDROID_PORT_BY);
            }
        };
        versionRow.setOnFocusChangeListener(detail); licencesRow.setOnFocusChangeListener(detail); project.setOnFocusChangeListener(detail);
        updateRow.setOnFocusChangeListener(detail);
        primary = versionRow;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), backPrompt(), startPrompt());
    }

    /** The source commit this APK was built from: assets/build.txt ("commit <hash>", written by build_release.py). */
    private String buildCommit() {
        try (InputStream in = getAssets().open("build.txt")) {
            for (String line : new String(in.readAllBytes(), StandardCharsets.UTF_8).split("\n"))
                if (line.startsWith("commit ")) return line.substring(7).trim();
        } catch (IOException missing) { }
        return null;
    }
    private List<String> licenceFiles() {
        List<String> list = new ArrayList<>();
        for (String dir : new String[]{"licenses", "fonts"}) {
            try {
                String[] names = getAssets().list(dir);
                if (names != null) for (String name : names) if (name.endsWith(".txt")) list.add(dir + "/" + name);
            } catch (IOException ignored) { }
        }
        return list;
    }
    private static String licenceTitle(String path) {
        String name = path.substring(path.lastIndexOf('/') + 1).replaceFirst("\\.txt$", "");
        return name.replace('_', ' ').replace('-', ' ');
    }
    private String licenceBody(String path) {
        String text;
        try (InputStream in = getAssets().open(path)) { text = new String(in.readAllBytes(), StandardCharsets.UTF_8); }
        catch (IOException missing) { text = path; }
        // Licence files are hard-wrapped at ~80 columns; rejoin lines into paragraphs so they wrap to the panel.
        return text.replace("\r", "").replaceAll("(?<!\n)\n(?!\n)", " ");
    }
    private void licences() {
        header("book", Strings.LICENSES, 0, null);
        Widgets.Panel list = panel(80, 186, 1040, 720, false, 28, 28);
        list.setClipChildren(true);   // the scrolled list must stay inside the panel
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false); scroll.setClipToPadding(false);
        LinearLayout column = Widgets.column(this);
        column.setPadding(Theme.u(8), Theme.u(4), Theme.u(8), Theme.u(4));
        scroll.addView(column);
        list.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        detailPanel();
        detailNote.setVisibility(View.GONE);
        for (String path : licenceFiles()) {
            Widgets.Row row = listRow(column, "licence:" + path, licenceTitle(path), path, 76, 8);
            row.setOnClickListener(v -> { licence = path; go(Screen.LICENSE_TEXT); });
            row.setOnFocusChangeListener((v, has) -> {
                if (!has) return;
                detailFacts.removeAllViews(); detailDiagram.setVisibility(View.GONE); detailConstraint.setVisibility(View.GONE);
                detailTitle.setText(licenceTitle(path));
                String body = licenceBody(path);
                detailText.setText(body.length() > 900 ? body.substring(0, 900) + "…" : body);
                detailText.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Theme.f(17));
            });
            if (primary == null) primary = row;
        }
        prompts(prompt("A", Strings.P_OPEN, v -> selectFocused()), backPrompt(), startPrompt());
    }
    private void licenceText() {
        header("book", licenceTitle(licence), 0, null);
        Widgets.Panel box = panel(80, 186, 1760, 720, true, 56, 40);
        box.setClipChildren(true);
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(true);
        TextView body = Theme.paragraph(this, licenceBody(licence), 19, Theme.TEXT2);
        scroll.addView(body);
        // Focusable so a D-pad can scroll through long texts.
        scroll.setFocusable(true); scroll.setTag("licence:text"); scroll.setDefaultFocusHighlightEnabled(false);
        box.addView(scroll, new LinearLayout.LayoutParams(-1, -1));
        primary = scroll;
        prompts(backPrompt());
    }

    // ---------- setup: shared ----------
    /** Setup rail: Storage, What you have, Files, Updates, Prepare, Graphics. */
    private void rail(int current, boolean failed) {
        Widgets.Panel steps = panel(80, 186, 470, 720, false, 40, 44);
        List<String[]> items = new ArrayList<>();
        items.add(new String[]{Strings.STEP_STORAGE, current > 0 ? StorageLocation.label(this) : Strings.STEP_STORAGE_TEXT});
        items.add(new String[]{Strings.STEP_ROUTE, Strings.routeName(current > 1 ? inputs.route : "")});
        items.add(new String[]{Strings.STEP_FILES, Strings.STEP_FILES_TEXT});
        items.add(new String[]{Strings.UPDATES, "Choose automatic version checks"});
        items.add(new String[]{Strings.STEP_PREPARE, Strings.prepareEstimate(inputs.route)});
        items.add(new String[]{Strings.STEP_GRAPHICS, Strings.STEP_GRAPHICS_TEXT});
        int at = current + 1;
        for (int i = 0; i < items.size(); i++) {
            int n = i + 1;
            int state = n < at ? Widgets.Step.DONE : n == at ? (failed ? Widgets.Step.FAILED : Widgets.Step.CURRENT) : Widgets.Step.TODO;
            steps.addView(new Widgets.Step(this, n, items.get(i)[0], items.get(i)[1], state, i == items.size() - 1), Widgets.size(-1, 104));
        }
    }
    /** Begin setup: the Storage step first when nothing has been imported anywhere yet. */
    private void beginSetup() {
        boolean fresh = !InstallState.read(this).anyDone() && !gameFile(".import").exists();
        go(fresh ? Screen.SETUP_STORAGE : Screen.ROUTE);
    }
    private Widgets.Panel setupMain() { return panel(590, 186, 1250, 720, true, 50, 44); }
    private Widgets.Chip setupHeader(String title) { return header("data", title, Widgets.Chip.OK, Strings.NOTHING_UPLOADED); }
    private void storageLine(LinearLayout parent, float top) {
        LinearLayout line = Widgets.row(this);
        line.addView(Theme.text(this, Strings.STORAGE, false, 19, Theme.TEXT2));
        storageBar = new Widgets.StorageBar(this);
        line.addView(storageBar, Widgets.margins(new LinearLayout.LayoutParams(0, Theme.u(12), 1), 22, 0, 22, 0));
        storageText = Theme.text(this, "", false, 19, Theme.TEXT2);
        line.addView(storageText);
        parent.addView(line, Widgets.margins(Widgets.size(-1, -2), 0, top, 0, 0));
    }
    private void updateStorage(InstallState install) {
        if (storageBar == null) return;
        long free = install.freeBytes(), need = install.neededBytes(), total = install.totalBytes();
        storageBar.set(total, free, need);
        storageText.setText("needs " + Strings.bytes(need) + " · " + Strings.bytes(free) + " free" + (free >= need ? "" : " — " + Strings.storageShort(need - free)));
        storageText.setTextColor(free >= need ? Theme.TEXT2 : Theme.WARN);
    }

    // ---------- setup screens ----------
    private void welcome() {
        InstallState install = InstallState.read(this);
        hero(170);
        TextView legal = Theme.paragraph(this, Strings.LEGAL, 18, Theme.AMBER);
        legal.setGravity(Gravity.CENTER_HORIZONTAL);
        board.put(legal, 440, 700, 620, Widgets.Board.WRAP, Widgets.Board.CENTER, Widgets.Board.START);
        Widgets.Panel box = panel(880, 150, 920, 740, true, 56, 50);
        heading(box, Strings.WELCOME_TITLE, 36);
        lead(box, Strings.WELCOME_BODY, 14);
        storageLine(box, 30);
        updateStorage(install);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        Widgets.Row begin = new Widgets.Row(this, install.anyDone() ? Strings.CONTINUE_SETUP : Strings.BEGIN, Strings.BEGIN_HINT, 30, 19).icon("play", 62, 30);
        begin.setTag("begin"); begin.setOnClickListener(v -> beginSetup());
        box.addView(begin, Widgets.rowParams(92, 8));
        // Saves before setup too: a save set can be imported (or the save folder chosen) before the game is installed.
        homeItem(box, "welcome:saves", "save", Strings.SAVES, Strings.savesSummary(liveSlots == null ? 0 : liveSlots.size(),
                liveSlots == null ? 0 : SaveManager.newest(liveSlots), folderSummary()), v -> go(Screen.SAVES), 92, 8);
        homeItem(box, "welcome:data", "data", Strings.GAME_DATA, Strings.WELCOME_DATA_HINT, v -> go(Screen.DATA), 92, 8);
        homeItem(box, "welcome:about", "about", Strings.ABOUT, Strings.ABOUT_HINT, v -> go(Screen.ABOUT), 92, 0);
        primary = begin;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), prompt("B", Strings.P_QUIT, v -> finish()));
    }

    private void route() {
        setupHeader(Strings.SETUP_TITLE);
        rail(1, false);
        Widgets.Panel box = setupMain();
        heading(box, Strings.ROUTE_TITLE, 36);
        lead(box, Strings.ROUTE_BODY, 10);
        Widgets.Card decrypted = routeCard(box, Inputs.DECRYPTED, "data", Strings.ROUTE_DECRYPTED, Strings.ROUTE_DECRYPTED_BODY, 34);
        Widgets.Card containers = routeCard(box, Inputs.CONTAINERS, "key", Strings.ROUTE_CONTAINERS, Strings.ROUTE_CONTAINERS_BODY, 22);
        primary = Inputs.CONTAINERS.equals(inputs.route) ? containers : decrypted;
        prompts(prompt("A", Strings.P_CHOOSE, v -> selectFocused()), backPrompt());
    }
    private Widgets.Card routeCard(LinearLayout parent, String route, String icon, String title, String body, float top) {
        Widgets.Card card = new Widgets.Card(this, icon, title, body, v -> chooseRoute(route));
        card.setTag("route:" + route);
        if (route.equals(inputs.route)) card.state("✓ " + Strings.PICKED, Theme.ZONAI, null);
        card.stateWidth(160);
        parent.addView(card, Widgets.margins(Widgets.size(-1, 190), 0, top, 0, 0));
        return card;
    }
    private void chooseRoute(String route) {
        if (!route.equals(inputs.route)) { inputs.clear(getContentResolver()); inputs.route = route; inputs.save(); inspection = null; }
        go(Screen.FILES);
    }

    private static boolean multi(String role) { return role.equals("romfs") || role.equals("exefs"); }
    /** ExeFS is usually a folder of three files; RomFS usually one file or its split parts. */
    private static boolean primaryFolder(String role) { return role.equals("exefs"); }
    private void files() {
        setupHeader(Strings.SETUP_TITLE);
        rail(2, false);
        Widgets.Panel box = setupMain();
        heading(box, Strings.FILES_TITLE, 36);
        boolean containers = Inputs.CONTAINERS.equals(inputs.route);
        lead(box, containers ? Strings.FILES_LEAD_CONTAINERS : Strings.FILES_LEAD_DECRYPTED, 10);
        String[] roles = inputs.roles();
        float cardH = roles.length > 2 ? 112 : 150, gap = roles.length > 2 ? 14 : 22;
        for (int i = 0; i < roles.length; i++) {
            String role = roles[i];
            Widgets.Card card = new Widgets.Card(this, roleIcon(role), roleName(role), roleHint(role), v -> pick(role, primaryFolder(role)));
            card.setTag("pick:" + role);
            card.stateWidth(420);
            card.setOnFocusChangeListener((v, has) -> { if (otherPrompt != null && has) otherPrompt.setVisibility(multi(role) ? View.VISIBLE : View.INVISIBLE); });
            slotCards.put(role, card);
            box.addView(card, Widgets.margins(Widgets.size(-1, cardH), 0, i == 0 ? 30 : gap, 0, 0));
            if (primary == null && inputs.get(role).isEmpty()) primary = card;
        }
        storageLine(box, 28);
        statusLine = add(box, Theme.text(this, "", false, 18, Theme.TEXT2), 14);
        if (!note.isEmpty()) { statusLine.setText(note); statusLine.setTextColor(Theme.WARN); }
        // bottom line of the panel: the other route on the left, Install on the right
        SpannableString link = new SpannableString((containers ? Strings.ALT_CONTAINERS : Strings.ALT_DECRYPTED) + (containers ? Strings.ALT_CONTAINERS_LINK : Strings.ALT_DECRYPTED_LINK));
        int start = (containers ? Strings.ALT_CONTAINERS : Strings.ALT_DECRYPTED).length();
        link.setSpan(new UnderlineSpan(), start, link.length(), 0);
        TextView alt = Theme.text(this, "", false, 18, Theme.TEXT3);
        alt.setText(link);
        alt.setClickable(true);
        alt.setOnClickListener(v -> { history.pop(); screen = Screen.ROUTE; chooseRoute(containers ? Inputs.DECRYPTED : Inputs.CONTAINERS); });
        board.put(alt, 640, 870, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.START, Widgets.Board.END);
        installAction = new Widgets.Action(this, "Continue", true, v -> go(Screen.UPDATES));
        installAction.setTag("install");
        board.put(installAction, 1790, 880, Widgets.Board.WRAP, Widgets.Board.WRAP, Widgets.Board.END, Widgets.Board.END);
        if (primary == null) primary = installAction;
        otherPrompt = prompt("X", Strings.P_OTHER_PICK, v -> extra());
        installPrompt = prompt("+", Strings.P_INSTALL, v -> plus());
        prompts(prompt("A", Strings.P_CHOOSE, v -> selectFocused()), otherPrompt, backPrompt(), installPrompt);
        updateFiles();
        if (inspection == null && !inspecting) inspect();
    }
    private static String roleIcon(String role) {
        switch (role) {
            case "romfs": return "data";
            case "exefs": return "code";
            case "keys": return "key";
            default: return "cart";
        }
    }
    private static String roleName(String role) {
        switch (role) {
            case "romfs": return Strings.ROLE_ROMFS;
            case "exefs": return Strings.ROLE_EXEFS;
            case "base": return Strings.ROLE_BASE;
            case "update": return Strings.ROLE_UPDATE;
            default: return Strings.ROLE_KEYS;
        }
    }
    private static String roleHint(String role) {
        switch (role) {
            case "romfs": return Strings.ROLE_ROMFS_HINT;
            case "exefs": return Strings.ROLE_EXEFS_HINT;
            case "base": return Strings.ROLE_BASE_HINT;
            case "update": return Strings.ROLE_UPDATE_HINT;
            default: return Strings.ROLE_KEYS_HINT;
        }
    }
    private String summary(String role) {
        List<Inputs.Item> list = inputs.get(role);
        if (list.isEmpty()) return Strings.NOT_PICKED;
        if (list.size() == 1) return list.get(0).name + (list.get(0).size >= 0 ? " · " + Strings.bytes(list.get(0).size) : "");
        // Split parts are joined in exactly this order, so show it.
        List<String> names = new ArrayList<>();
        for (int i = 0; i < list.size(); i++) names.add((i + 1) + ".\u00a0" + list.get(i).name);  // no break after the number
        return Strings.parts(list.size(), Inputs.total(list)) + (role.equals("romfs") ? Strings.JOIN_ORDER : " — ") + String.join("  ", names);
    }

    /** Cards and the Install action from the latest NativeSetup.inspect answer. */
    private void updateFiles() {
        if (screen != Screen.FILES || installAction == null) return;
        InstallState install = InstallState.read(this);
        updateStorage(install);
        boolean library = InstallState.libraryAvailable();
        JSONArray found = inspection == null ? null : inspection.optJSONArray("items");
        JSONArray need = inspection == null ? null : inspection.optJSONArray("need");
        boolean problems = false;
        for (String role : inputs.roles()) {
            Widgets.Card card = slotCards.get(role);
            if (card == null) continue;
            if (inputs.get(role).isEmpty()) {
                boolean folder = primaryFolder(role);
                card.state(multi(role) ? (folder ? Strings.PICK_FOLDER : Strings.PICK_FILES) : Strings.PICK_FILE, Theme.TEXT2,
                        multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);
                continue;
            }
            String picked = summary(role);
            if (inspecting) { card.state(Strings.CHECKING, Theme.AMBER, picked); continue; }
            if (!library || found == null) { card.state(Strings.PICKED, Theme.TEXT2, picked); continue; }
            String what = null, problem = null;
            for (int i = 0; i < found.length(); i++) {
                JSONObject item = found.optJSONObject(i);
                if (item == null || !role.equals(item.optString("role"))) continue;
                if (!item.optBoolean("ok")) problem = item.isNull("problem") ? Strings.FAILED : item.optString("problem");
                else if (what == null) what = item.isNull("what") ? Strings.LOOKS_GOOD : item.optString("what");
            }
            if (lostRoles(role)) { card.state(Strings.CANT_OPEN, Theme.WARN, picked); problems = true; }
            else if (problem != null) { card.state(problem, Theme.WARN, picked); problems = true; }
            else if (what != null) card.state("✓ " + what, Theme.ZONAI, picked);
            else card.state(Strings.PICKED, Theme.AMBER, picked);
        }
        // Needs of the CHOSEN route only: native may not know the route yet (nothing picked) and then lists every role.
        List<String> routeRoles = Arrays.asList(inputs.roles()), missing = new ArrayList<>();
        for (String role : routeRoles) {
            boolean needed = inputs.get(role).isEmpty();
            if (need != null) for (int i = 0; i < need.length(); i++) needed |= role.equals(need.optString(i));
            if (needed) missing.add(roleName(role));
        }
        boolean space = install.freeBytes() >= install.neededBytes();
        boolean ready = library && !inspecting && inspection != null && inspection.optBoolean("ok") && missing.isEmpty() && !problems && space;
        if (!note.isEmpty()) { /* the "access lost" message stays until a new pick */ }
        else if (!library) { statusLine.setText(Strings.NO_LIBRARY); statusLine.setTextColor(Theme.WARN); }
        else if (inspecting) { statusLine.setText(Strings.CHECKING); statusLine.setTextColor(Theme.TEXT2); }
        else if (!missing.isEmpty()) { statusLine.setText(Strings.NEED + String.join(", ", missing)); statusLine.setTextColor(Theme.AMBER); }
        else if (ready) { statusLine.setText(Strings.ALL_SET); statusLine.setTextColor(Theme.ZONAI); }
        else if (problems) { statusLine.setText(Strings.FIX_MARKED); statusLine.setTextColor(Theme.WARN); }
        else statusLine.setText("");
        gameState = InstallState.gameState(this);
        if (!gameState.free()) {
            statusLine.setText(gameState.state == GameLock.State.GAME ? Strings.gameRunning("Installing") : Strings.gameLockError(gameState.reason));
            statusLine.setTextColor(Theme.AMBER);
        }
        installAction.setEnabled(ready);
        if (installPrompt != null) installPrompt.setEnabled(ready);
        gameControl(installAction, "Installing", () -> go(Screen.UPDATES));
        if (installPrompt != null) gameControl(installPrompt, "Installing", () -> go(Screen.UPDATES));
    }
    private boolean lostRoles(String role) {
        JSONArray lost = inspection == null ? null : inspection.optJSONArray("lost");
        if (lost != null) for (int i = 0; i < lost.length(); i++) if (role.equals(lost.optString(i))) return true;
        return false;
    }
    private void inspect() {
        if (!InstallState.libraryAvailable()) { inspecting = false; updateFiles(); return; }
        int generation = ++inspectGeneration;
        inspecting = true; updateFiles();
        Inputs snapshot = inputs;
        worker.execute(() -> {
            JSONObject answer;
            try (Inputs.Opened opened = snapshot.open(this, false, null)) {
                answer = new JSONObject(NativeSetup.inspect(opened.request.toString()));
                if (!opened.lostRoles.isEmpty()) answer.put("ok", false).put("lost", new JSONArray(opened.lostRoles));
            } catch (JSONException | RuntimeException | LinkageError failed) {
                answer = new JSONObject();
            }
            JSONObject result = answer;
            main.post(() -> {
                if (generation != inspectGeneration) return;
                inspecting = false; inspection = result; updateFiles();
            });
        });
    }

    private void pick(String role, boolean folder) {
        int index = Arrays.asList(ALL_ROLES).indexOf(role);
        Intent intent;
        if (folder) intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        else {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
            if (multi(role)) intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try { startActivityForResult(intent, REQ_PICK + index * 2 + (folder ? 1 : 0)); }
        catch (ActivityNotFoundException missing) { if (statusLine != null) statusLine.setText(Strings.PICKER_MISSING); }
    }
    @Override protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request == UpdateInstaller.REQUEST_SOURCES) { if (updater != null) updater.permissionReturned(); return; }
        if (code != RESULT_OK || data == null) return;
        if (request == REQ_DRIVER) { if (data.getData() != null) installDriver(data.getData()); return; }
        if (request == REQ_EXPORT) { if (data.getData() != null) writeExport(data.getData()); return; }
        if (request == REQ_IMPORT_ZIP || request == REQ_IMPORT_TREE) { if (data.getData() != null) stageImport(data.getData(), request == REQ_IMPORT_TREE); return; }
        if (request == REQ_MIRROR) { if (data.getData() != null) useMirror(data.getData()); return; }
        if (request == REQ_DATA_FOLDER) { if (data.getData() != null) usePickedFolder(data.getData()); return; }
        if (request == REQ_ACCESS) return;   // re-checked when the menu comes back (onStart)
        int index = (request - REQ_PICK) / 2;
        if (index < 0 || index >= ALL_ROLES.length) return;
        String role = ALL_ROLES[index];
        boolean folder = (request - REQ_PICK) % 2 == 1;
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
        else if (data.getData() != null) uris.add(data.getData());
        if (uris.isEmpty()) return;
        ContentResolver resolver = getContentResolver();
        for (Uri uri : uris) Inputs.keep(resolver, uri);
        inspecting = true; updateFiles();
        worker.execute(() -> {
            List<Inputs.Item> items = new ArrayList<>();
            if (folder) items.addAll(Inputs.children(resolver, uris.get(0), role));
            else {
                for (Uri uri : uris) items.add(Inputs.describe(resolver, uri));
                Inputs.sort(items);
                if (!multi(role) && items.size() > 1) items.subList(1, items.size()).clear();
            }
            main.post(() -> {
                inputs.set(resolver, role, items, uris);
                inspection = null; inspecting = false; note = "";  // a fresh pick supersedes any "access lost" message
                if (screen == Screen.FILES) render();
            });
        });
    }

    // ---------- install ----------
    private void requestNotificationsThen(Runnable action) {
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED
                || Inputs.prefs(this).getBoolean("askedNotify", false)) { action.run(); return; }
        Inputs.prefs(this).edit().putBoolean("askedNotify", true).apply();
        afterPermission = action;
        requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIFY);
    }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        Runnable action = afterPermission;
        afterPermission = null;
        if (request == REQ_NOTIFY && action != null) action.run();
    }
    private void startSetup(String[] stages) {
        if (!needGameClosed("Installing", () -> startSetup(stages))) return;
        if (!needsInputs(stages)) { launchService(stages); return; }
        // Re-open every persisted pick before starting: a revoked or moved document goes back to Files with a clear
        // message instead of failing deep inside the import.
        ContentResolver resolver = getContentResolver();
        Inputs snapshot = inputs;
        worker.execute(() -> {
            List<String> lost = snapshot.unreadableRoles(this, InstallState.read(this));
            main.post(() -> {
                if (lost.isEmpty()) launchService(stages);
                else lostAccess(lost);
            });
        });
    }
    /** Only the code stage (ExeFS / containers) and the RomFS copy read the user's files; later stages work from the
     * imported copy, so an install past those needs no access to the picks. */
    private boolean needsInputs(String[] stages) {
        InstallState install = InstallState.read(this);
        boolean pending = !install.done("code") || !install.done("romfs");
        return pending && (stages == null || !Arrays.asList(InstallState.PAIR).containsAll(Arrays.asList(stages)));
    }
    private void launchService(String[] stages) {
        if (stages == null && !InstallState.read(this).anyDone())
            Inputs.prefs(this).edit().remove(ArtPolicy.DECLINED).apply();
        if (stages == null || stages.length != 1 || !InstallState.OPTIONAL.equals(stages[0]))
            Inputs.prefs(this).edit().remove(ArtPolicy.START_AFTER_BUILD).apply();
        SetupService.clearResult(this);
        installSnapshot = null;
        SetupService.start(this, stages);
        // The service flips status.running in onStartCommand; show its rows right away.
        SetupService.status.stages = stages;
        replace(Screen.INSTALL);
    }
    /** Forget picks whose documents can no longer be opened and send the user back to choose them again. */
    private void lostAccess(List<String> roles) {
        ContentResolver resolver = getContentResolver();
        for (String role : roles) inputs.set(resolver, role, new ArrayList<>(), new ArrayList<>());
        SetupService.clearResult(this);
        inspection = null;
        history.clear(); history.push(Screen.WELCOME); history.push(Screen.ROUTE);
        screen = Screen.FILES; restoreFocus = PRIMARY;
        note = Strings.error("permission");
        render();
    }
    private void showNote(String text) {
        note = text;
        if (statusChip != null) statusChip.set(Widgets.Chip.WARN, text);
        else if (statusLine != null) { statusLine.setText(text); statusLine.setTextColor(Theme.AMBER); }
        else render();
    }

    private String[] stageList() {
        String[] requested = SetupService.status.stages;
        if (requested != null) return requested;
        String[] all = new String[InstallState.CORE.length + InstallState.PAIR.length];
        System.arraycopy(InstallState.CORE, 0, all, 0, InstallState.CORE.length);
        System.arraycopy(InstallState.PAIR, 0, all, InstallState.CORE.length, InstallState.PAIR.length);
        return all;
    }
    private void install() {
        String rebuild = rebuilding();
        // A build of output that is already there (and usable) replaces it only once complete; otherwise it is a first build.
        // Art from an older generator is still there and in use until the update replaces it.
        InstallState now = InstallState.read(this);
        artUpdate = InstallState.OPTIONAL.equals(rebuild) && now.artOutdated();
        rebuildAgain = rebuild != null && (now.done(rebuild) || artUpdate) && !(rebuild.equals("shaders") && seedRefused);
        header("data", SetupService.moving() ? (StorageLocation.migrating(this) ? Strings.MIGRATE_TITLE : Strings.MOVING_TITLE)
                : rebuild == null ? Strings.PREPARING_TITLE : Strings.rebuildTitle(rebuild, rebuildAgain, artUpdate), Widgets.Chip.OK, Strings.KEEPS_GOING);
        ring = board.put(new Widgets.Ring(this), 150, 230, 560, 560);
        Widgets.Panel list = panel(880, 186, 960, 720, true, 44, 40);
        heading(list, Strings.STEPS, 32);
        for (String stage : stageList()) {
            Widgets.StageRow row = new Widgets.StageRow(this, Strings.stage(stage), InstallState.OPTIONAL.equals(stage) ? Strings.OPTIONAL : null);
            stageRows.put(stage, row);
            list.addView(row, Widgets.margins(Widgets.size(-1, 86), 0, stageRows.size() == 1 ? 18 : 0, 0, 0));
        }
        list.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout tip = Widgets.row(this);
        tip.addView(new Widgets.Key(this, "i", 30));
        extrasNote = Theme.paragraph(this, SetupService.moving() ? Strings.MOVE_TIP : Strings.RESUME_TIP, 19, Theme.TEXT2);
        tip.addView(extrasNote, Widgets.margins(new LinearLayout.LayoutParams(0, -2, 1), 14, 0, 0, 0));
        list.addView(tip);
        // A rebuild of art that is already there keeps it, so the game starts with it; otherwise it starts without.
        playPrompt = prompt("+", InstallState.OPTIONAL.equals(rebuild) && rebuildAgain ? Strings.P_PLAY_NOW : Strings.P_PLAY_WITHOUT_ART, v -> playNow());
        cancelPrompt = prompt("B", Strings.P_CANCEL, v -> cancelPressed());
        prompts(playPrompt, cancelPrompt);
        updateInstall();
    }
    /** B on the install screen asks twice: an accidental press must not throw away a half-hour copy. */
    private void cancelPressed() {
        if (!SetupService.status.running) { back(); return; }
        if (SetupService.status.cancelling) return;
        long now = SystemClock.uptimeMillis();
        if (cancelArmedAt != 0 && now - cancelArmedAt <= 3000) {
            SetupService.cancel(this);
            cancelArmedAt = 0;
            if (cancelPrompt != null) { cancelPrompt.text.setText(Strings.CANCELLING); cancelPrompt.setEnabled(false); }
            return;
        }
        cancelArmedAt = now;
        if (cancelPrompt != null) { cancelPrompt.text.setText(Strings.P_CANCEL_AGAIN); cancelPrompt.text.setTextColor(Theme.AMBER); }
    }
    private void disarmCancel() {
        cancelArmedAt = 0;
        if (cancelPrompt != null && !SetupService.status.cancelling) { cancelPrompt.text.setText(Strings.P_CANCEL); cancelPrompt.text.setTextColor(Theme.TEXT2); }
    }
    /** Only the second-screen art may be skipped for the game: the prompt shows once the shader seed is committed and the
     * companion stage is all that still runs; it stops that stage (nothing runs beside the game), then starts it. */
    private void playNow() {
        if (!SetupService.status.running) { if (InstallState.read(this).playable()) launchGame(); return; }
        if (playPrompt == null || playPrompt.getVisibility() != View.VISIBLE || !playPrompt.isEnabled()) return;
        if (!InstallState.read(this).done(InstallState.OPTIONAL) && !InstallState.read(this).artOutdated())
            Inputs.prefs(this).edit().putBoolean(ArtPolicy.DECLINED, true).remove(ArtPolicy.START_AFTER_BUILD).apply();
        playAfterStop = true;
        playPrompt.text.setText(Strings.STOPPING); playPrompt.setEnabled(false);
        SetupService.cancel(this);
    }
    private static String rowKey(String stage) { return stage.equals("inspect") || stage.equals("exefs") ? "code" : stage; }

    private void updateInstall() {
        if (screen != Screen.INSTALL || ring == null) return;
        SetupService.Status status = SetupService.status;
        String current = rowKey(status.stage);
        long now = SystemClock.uptimeMillis();
        // Companion and shaders report in turns: re-read install.json when a core stage changes or a stage ends.
        int ended = 0;
        for (SetupService.Lane lane : status.lanes.values()) if (lane.finished) ended++;
        String key = (isPair(current) ? "pair" : current) + "/" + ended;
        if (installSnapshot == null || !key.equals(snapshotStage) || now - snapshotAt >= 2000) {
            installSnapshot = InstallState.read(this); snapshotStage = key; snapshotAt = now;
        }
        InstallState install = installSnapshot;
        String[] order = stageList();
        int currentIndex = Arrays.asList(order).indexOf(current);
        for (int i = 0; i < order.length; i++) {
            String stage = order[i];
            Widgets.StageRow row = stageRows.get(stage);
            if (row == null) continue;
            // Companion and shaders show their own progress (both may be active at once); a core stage the current one.
            SetupService.Lane lane = status.running && isPair(stage) ? status.lanes.get(stage) : null;
            boolean active = isPair(stage) ? lane != null && !lane.finished : stage.equals(current) && status.running;
            if (active) {
                long done = lane != null ? lane.done : status.done, total = lane != null ? lane.total : status.total;
                boolean known = total > 0;
                float fraction = known ? (float) Math.min(1.0, (double) done / total) : -1;
                String detail = lane != null ? activeDetail(done, total, lane.startedAt, lane.startDone, lane.detail)
                        : activeDetail(done, total, status.stageStartedAt, status.stageStartDone, status.detail);
                row.set(Widgets.StageRow.ACTIVE, fraction, known ? Math.round(fraction * 100) + "%" : "…", detail);
            } else if (install.done(stage) || (status.running && currentIndex >= 0 && i < currentIndex && !isPair(stage))) {
                row.set(Widgets.StageRow.DONE, 1, Strings.DONE, stage.equals("shaders") && install.shaderVariants() > 0 ? Strings.variants(install.shaderVariants()) : Strings.stageDone(stage));
            } else if (install.skipped(stage)) {
                row.set(Widgets.StageRow.SKIPPED, 0, Strings.ART_SKIPPED, Strings.ART_NOT_NEEDED);
            } else if (!install.errorCode(stage).isEmpty() && (currentIndex < 0 || i < currentIndex || !status.running || lane != null)) {
                // A failed shader stage fails setup; the companion stage is only skipped.
                String why = install.errorText(stage).isEmpty() ? Strings.error(install.errorCode(stage)) : install.errorText(stage);
                if (InstallState.OPTIONAL.equals(stage)) row.set(Widgets.StageRow.SKIPPED, 0, Strings.SKIPPED, why);
                else row.set(Widgets.StageRow.FAILED, 0, Strings.FAILED, why);
            } else row.set(Widgets.StageRow.PENDING, 0, Strings.WAITING, null);
        }
        // The ring follows the current stage: its percentage whenever the stage reports done/total, dots only without a
        // total (the arc then sweeps).
        boolean known = status.running && status.total > 0;
        float fraction = known ? (float) Math.min(1.0, (double) status.done / status.total) : -1;
        ring.set(status.running ? fraction : 0, known ? Math.round(fraction * 100) + "%" : "…",
                status.running ? Strings.stage(status.stage) : Strings.STARTING, eta(status));
        // Once the shader seed is committed and only the second-screen art still runs, the game may start without it.
        SetupService.Lane shaders = status.lanes.get("shaders");
        boolean playWithout = status.running && InstallState.OPTIONAL.equals(current) && install.playable()
                && (shaders == null || shaders.finished);
        playPrompt.setVisibility(playWithout ? View.VISIBLE : View.GONE);
        String rebuild = rebuilding();
        extrasNote.setText(SetupService.moving() ? Strings.MOVE_TIP : rebuild != null ? Strings.rebuildNote(rebuild, rebuildAgain, artUpdate)
                : playWithout ? Strings.EXTRAS_RUNNING : Strings.RESUME_TIP);
        if (status.cancelling && cancelPrompt.isEnabled()) { cancelPrompt.text.setText(Strings.CANCELLING); cancelPrompt.setEnabled(false); }
    }
    private static boolean isPair(String stage) { return Arrays.asList(InstallState.PAIR).contains(stage); }
    /** The one stage of the last two a Game data rebuild (or Home's Build shaders) runs, or null for a setup run. */
    private static String rebuilding() {
        String[] stages = SetupService.status.stages;
        return stages != null && stages.length == 1 && isPair(stages[0]) ? stages[0] : null;
    }
    private static String eta(SetupService.Status status) {
        if (status.total < 100_000_000L) return "";
        double seconds = (SystemClock.elapsedRealtime() - status.stageStartedAt) / 1000.0;
        long moved = status.done - status.stageStartDone;
        if (seconds <= 3 || moved <= 0) return "";
        return Strings.eta((long) ((status.total - status.done) / (moved / seconds)));
    }
    private static String activeDetail(long done, long total, long startedAt, long startDone, String detail) {
        StringBuilder text = new StringBuilder();
        if (total >= 100_000_000L) {  // large totals are byte counts: show size and speed
            text.append(Strings.bytes(done)).append(" of ").append(Strings.bytes(total));
            double seconds = (SystemClock.elapsedRealtime() - startedAt) / 1000.0;
            long moved = done - startDone;
            if (seconds > 3 && moved > 0) text.append(" · ").append(Strings.rate(moved / seconds));
        }
        if (!detail.isEmpty()) text.append(text.length() > 0 ? " · " : "").append(detail);
        return text.toString();
    }

    @Override public void onStatus() {
        if (!ui) return;
        if (screen == Screen.INSTALL) {
            if (SetupService.status.running) updateInstall();
            else finished();
        }
    }
    private void finished() {
        JSONObject result = SetupService.lastResult(this);
        InstallState install = InstallState.read(this);
        if (playAfterStop) {
            playAfterStop = false;
            if (install.playable()) { SetupService.clearResult(this); launchGame(); return; }
        }
        if (!result.has("ok")) return;  // service not finished yet (status listener fired before the result was stored)
        if (Inputs.prefs(this).getBoolean(ArtPolicy.START_AFTER_BUILD, false)) {
            Inputs.prefs(this).edit().remove(ArtPolicy.START_AFTER_BUILD).apply();
            SetupService.clearResult(this);
            if (result.optBoolean("ok") && install.done(InstallState.OPTIONAL) && install.playable()) { launchGame(); return; }
            replace(Screen.HOME);
            showNote(Strings.ART_OFFER_FAILED);
            return;
        }
        if (SetupService.moving() || "move".equals(result.optString("stage"))) {
            // Back to Game data with the outcome; a failed or cancelled move left the data where it was.
            SetupService.clearResult(this);
            boolean ok = result.optBoolean("ok");
            DataSize.invalidate();
            history.clear(); history.push(Screen.HOME); screen = Screen.DATA; restoreFocus = PRIMARY;
            note = ok ? Strings.moved(result.optString("name")) : Strings.moveFailed(result.optString("code"), StorageLocation.label(this));
            noteOk = ok;
            render();
            return;
        }
        if (result.optBoolean("ok")) {
            SetupService.clearResult(this);
            DataSize.invalidate();
            String rebuilt = rebuilding();
            boolean home = homeAfterBuild;
            homeAfterBuild = false;
            if (rebuilt == null || rebuilt.equals("shaders")) seedRefused = false;
            if (!install.playable()) { replace(Screen.DATA); return; }
            if (SetupService.status.stages == null) {
                // A fresh install: step 4, Choose graphics, with this device's profile selected.
                settings.applyPreset(Settings.profile(SettingsStore.soc()));
                SettingsStore.save(this, settings);
                history.clear(); history.push(Screen.HOME);
                setupFinished = true; tab = Settings.PERFORMANCE; screen = Screen.GRAPHICS; note = ""; restoreFocus = PRIMARY;
                render();
            } else if (home) {
                // Home's Build shaders: back there, where Start game is now offered.
                replace(Screen.HOME);
            } else {
                // A rebuild from Game data: back there, with a short confirmation.
                history.clear(); history.push(Screen.HOME); screen = Screen.DATA; restoreFocus = PRIMARY;
                note = rebuilt == null ? "" : Strings.rebuilt(rebuilt, install.shaderVariants()); noteOk = !note.isEmpty();
                render();
            }
        }
        else if ("permission".equals(result.optString("code"))) {
            ContentResolver resolver = getContentResolver();
            Inputs snapshot = inputs;
            worker.execute(() -> { List<String> lost = snapshot.unreadableRoles(this, InstallState.read(this)); main.post(() -> lostAccess(lost)); });
        }
        // A cancelled second-screen art stage on an installed game: back to Home, like showUi on the next launch (its
        // Game data row offers the rebuild again); the error screen's Resume is for stages the game needs.
        else if (install.installed() && InstallState.OPTIONAL.equals(result.optString("stage"))) {
            SetupService.clearResult(this);
            homeAfterBuild = false;
            replace(Screen.HOME);
        }
        else replace(Screen.ERROR);
    }

    private void error() {
        JSONObject result = SetupService.lastResult(this);
        String code = result.optString("code", "internal");
        setupHeader(Strings.SETUP_TITLE);
        rail(4, true);
        Widgets.Panel box = panel(590, 186, 1250, 720, true, 50, 32);
        heading(box, Strings.ERROR_TITLE, 36);
        String stage = result.optString("stage", "");
        if (!stage.isEmpty()) {
            LinearLayout wrap = Widgets.row(this);
            wrap.addView(new Widgets.Chip(this).set(Widgets.Chip.ERROR, Strings.stage(stage) + " · " + Strings.FAILED));
            add(box, wrap, 14);
        }
        lead(box, Strings.error(code), 14);
        String message = result.optString("message", "");
        if (!message.isEmpty()) {
            TextView details = add(box, Theme.paragraph(this, Strings.DETAILS + message, 17, Theme.TEXT3), 8);
            details.setMaxLines(2); details.setEllipsize(TextUtils.TruncateAt.END);
        }
        add(box, Theme.paragraph(this, SetupDetails.PRIVACY, 16, Theme.TEXT3), 8);
        if (!note.isEmpty()) add(box, Theme.paragraph(this, note, 18, Theme.AMBER), 8);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        Widgets.Row resume = resumeRows(box);
        Widgets.Row share = new Widgets.Row(this, Strings.SHARE_DETAILS,
                sharingDetails ? Strings.SHARE_DETAILS_BUSY : Strings.SHARE_DETAILS_HINT, 27, 18).icon("export", 56, 28);
        share.setTag("share-details");
        share.dimmed(sharingDetails);
        share.setOnClickListener(v -> { if (!sharingDetails) shareDetails(); });
        box.addView(share, Widgets.rowParams(76, 8));
        if (code.equals("library")) resume.dimmed(true);
    }
    /** Resume remains the primary action; sharing details never retries or changes setup. */
    private Widgets.Row resumeRows(LinearLayout box) {
        Widgets.Row resume = new Widgets.Row(this, Strings.RESUME, Strings.RESUME_HINT, 27, 18).icon("refresh", 56, 28);
        resume.setTag("resume");
        resume.setOnClickListener(v -> { if (!sharingDetails && !resume.dimmed()) requestNotificationsThen(() -> startSetup(SetupService.jobStages(this))); });
        gameControl(resume, "Installing", () -> { if (!sharingDetails) requestNotificationsThen(() -> startSetup(SetupService.jobStages(this))); });
        Widgets.Row change = new Widgets.Row(this, Strings.CHANGE_FILES, Strings.CHANGE_FILES_HINT, 27, 18).icon("folder", 56, 28);
        change.setTag("change"); change.setOnClickListener(v -> { if (!sharingDetails) changeFiles(); });
        Widgets.Row data = new Widgets.Row(this, Strings.GAME_DATA, Strings.WELCOME_DATA_HINT, 27, 18).icon("data", 56, 28);
        data.setTag("data"); data.setOnClickListener(v -> { if (!sharingDetails) go(Screen.DATA); });
        for (Widgets.Row row : new Widgets.Row[]{resume, change, data}) {
            row.dimmed(sharingDetails);
            box.addView(row, Widgets.rowParams(screen == Screen.ERROR ? 76 : 92, screen == Screen.ERROR ? 8 : 10));
        }
        primary = resume;
        prompts(prompt("A", Strings.P_SELECT, v -> selectFocused()), prompt("B", Strings.P_QUIT, v -> finish()));
        return resume;
    }
    /** Snapshot on the worker before another setup run can clear .import; only the chooser runs on the UI thread. */
    private void shareDetails() {
        if (sharingDetails || SetupService.status.running) return;
        sharingDetails = true;
        render();
        worker.execute(() -> {
            JSONObject result = SetupService.lastResult(this);
            Inputs picked = Inputs.load(this);
            File game = StorageLocation.gameDir(this);
            List<String> excluded = new ArrayList<>();
            StringBuilder metadata = new StringBuilder();
            String version = "?"; long versionCode = 0;
            try {
                PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
                version = info.versionName; versionCode = info.getLongVersionCode();
            } catch (PackageManager.NameNotFoundException impossible) { }
            metadata.append("App: ").append(version).append(" (versionCode ").append(versionCode).append(")\n")
                    .append("Build commit: ").append(buildCommit()).append('\n')
                    .append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n')
                    .append("Android: ").append(Build.VERSION.RELEASE).append(" (SDK ").append(Build.VERSION.SDK_INT).append(")\n")
                    .append("SoC: ").append(SettingsStore.soc()).append('\n')
                    .append("Failed stage: ").append(result.optString("stage", "setup")).append('\n')
                    .append("Route: ").append(picked.route).append('\n');
            StoragePaths.Kind kind = StorageLocation.kind(this);
            metadata.append("Destination: ").append(kind == StoragePaths.Kind.PRIVATE ? "private"
                    : kind == StoragePaths.Kind.APP ? "SD app folder" : "picked folder").append('\n');
            metadata.append("Destination free bytes: ").append(game.getUsableSpace()).append('\n')
                    .append("Private storage free bytes: ").append(getFilesDir().getUsableSpace()).append('\n');
            for (String role : picked.roles()) {
                int index = 0;
                for (Inputs.Item item : picked.get(role)) {
                    File direct = FileAccess.directFile(this, item.uri, item.size);
                    excluded.add(item.uri.toString());
                    excluded.add(item.name);
                    if (direct != null) excluded.add(direct.getPath());
                    metadata.append("Source ").append(role).append('[').append(++index).append("]: ")
                            .append(direct != null ? "direct path" : "provider authority " + item.uri.getAuthority())
                            .append("; extension ").append(SetupDetails.extension(item.name))
                            .append("; bytes ").append(direct != null ? direct.length() : item.size).append('\n');
                }
                if (index == 0) metadata.append("Source ").append(role).append(": not selected\n");
            }
            // Longest first so a filename cannot leave the rest of a containing path or URI behind.
            excluded.sort((a, b) -> Integer.compare(b.length(), a.length()));
            // Scrub the message before JSON encoding, not the structural stage/code or source summaries.
            metadata.append("Result JSON: ").append(SetupService.failure(result.optString("stage"), result.optString("code"),
                    SetupDetails.scrub(result.optString("message"), excluded))).append('\n');
            String report = SetupDetails.build(metadata.toString(), game, result.optString("stage"), excluded);
            main.post(() -> {
                sharingDetails = false;
                if (isDestroyed() || screen != Screen.ERROR) return;
                render();
                Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_SUBJECT, "TotK Native setup details")
                        .putExtra(Intent.EXTRA_TEXT, report);
                try { startActivity(Intent.createChooser(send, Strings.SHARE_DETAILS)); }
                catch (ActivityNotFoundException unavailable) { showNote(Strings.SHARE_DETAILS_MISSING); }
            });
        });
    }
    private void changeFiles() {
        SetupService.clearResult(this);
        history.clear(); history.push(Screen.WELCOME);
        screen = inputs.route.isEmpty() ? Screen.ROUTE : Screen.FILES;
        if (screen == Screen.FILES) history.push(Screen.ROUTE);
        inspection = null; note = ""; restoreFocus = PRIMARY;
        render();
    }
    private void interrupted() {
        setupHeader(Strings.SETUP_TITLE);
        rail(4, false);
        Widgets.Panel box = setupMain();
        heading(box, Strings.INTERRUPTED_TITLE, 36);
        lead(box, Strings.INTERRUPTED_BODY, 14);
        if (!note.isEmpty()) add(box, Theme.paragraph(this, note, 18, Theme.AMBER), 8);
        box.addView(Widgets.space(this), new LinearLayout.LayoutParams(-1, 0, 1));
        resumeRows(box);
    }
}

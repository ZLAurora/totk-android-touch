"""Host regressions for production setup control/lifecycle methods and the native diagnostic close reader.
Android widgets/services are contract doubles; the exercised method bodies are extracted unchanged from production.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
JAVA = GAME / 'android/java/com/aure/totk/setup'


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


JAVA_PRELUDE = r'''package com.aure.totk.setup;
import java.io.*;
import java.nio.file.*;
import java.util.*;
class View {
    interface OnFocusChangeListener { void onFocusChange(View view, boolean has); }
    interface OnClickListener { void onClick(View view); }
    private OnFocusChangeListener focus;
    private OnClickListener click;
    boolean enabled = true, focused;
    float alpha = 1;
    boolean isEnabled() { return enabled; }
    void setEnabled(boolean value) { enabled = value; }
    void setAlpha(float value) { alpha = value; }
    OnFocusChangeListener getOnFocusChangeListener() { return focus; }
    void setOnFocusChangeListener(OnFocusChangeListener value) { focus = value; }
    void setOnClickListener(OnClickListener value) { click = value; }
    boolean requestFocus() { if (!enabled) return false; focused = true; if (focus != null) focus.onFocusChange(this, true); return true; }
    void performClick() { if (enabled && click != null) click.onClick(this); }
}
class Widgets {
    static final class Row extends View {
        boolean dim;
        boolean dimmed() { return dim; }
        void dimmed(boolean value) { dim = value; setAlpha(value ? .42f : 1); }
    }
}
class SystemClock { static long now; static long uptimeMillis() { return now; } }
class ComponentName {}
interface IBinder {}
interface ServiceConnection {
    void onServiceConnected(ComponentName name, IBinder service);
    void onServiceDisconnected(ComponentName name);
    void onBindingDied(ComponentName name);
    void onNullBinding(ComponentName name);
}
class GameControlService {}
class Intent { Intent(Object context, Class<?> service) {} }
class TextView { void setText(String text) {} }
class StorageManager {
    int registered, removed;
    void registerStorageVolumeCallback(Object executor, Runnable callback) { registered++; }
    void unregisterStorageVolumeCallback(Runnable callback) { removed++; }
}
class SetupService {
    static final class Status { boolean running; }
    static final Status status = new Status();
    static int listens, unlistens;
    static void listen(SetupActivity activity) { listens++; }
    static void unlisten(SetupActivity activity) { unlistens++; }
}
class Strings {
    static final String SAVE_WAIT_TIMEOUT = "save timeout", CLOSE_GAME_TIMEOUT = "close timeout";
    static String gameLockError(String reason) { return reason; }
}
class BaseActivity { protected void onStart() {} protected void onStop() {} }
class Handler {
    final Set<Runnable> pending = new HashSet<>();
    int posted;
    void postDelayed(Runnable value, long delay) { pending.add(value); posted++; }
    void post(Runnable value) { pending.add(value); posted++; }
    void removeCallbacks(Runnable value) { pending.remove(value); }
}
class InstallState {
    static GameLock.Result next = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
    static GameLock.Result gameState(SetupActivity activity) { return next; }
'''

JAVA_ACTIVITY = r'''
}
public final class SetupActivity extends BaseActivity {
    enum Screen { HOME, SAVES, WELCOME, DATA, STORAGE, SETUP_STORAGE, INSTALL, UPDATE_DOWNLOAD }
    boolean ui, started = true, uiStarted, closingGame, closeSent, noteOk;
    GameLock.Result gameState = InstallState.next;
    final List<View> gameControls = new ArrayList<>();
    Runnable blockedAction;
    String blockedNeed = "", closeId = "probe";
    String note = "";
    TextView gameNotice;
    ServiceConnection closeConnection;
    static final int BIND_AUTO_CREATE = 1;
    int binds, unbinds, bindFlags;
    boolean bindAccepted = true;
    Runnable duringBind;
    long closeDeadline = 10000, closeStartedAt, gameCheckedAt, cancelArmedAt;
    final Handler main = new Handler();
    final StorageManager storage = new StorageManager();
    final Runnable volumeChanged = () -> {};
    static final long REFRESH_MS = 500;
    Screen screen = Screen.HOME;
    final File files;
    Object updater;
    void requestedUpdate() {}
    void offerPendingUpdate() {}
    void updateInstallResult() {}
    void updateDownloadProgress() {}
    int renders, statuses, syncs, checks, notices;
    SetupActivity(File files) throws IOException { this.files = files; Files.createDirectories(new File(files, "state").toPath()); }
    File getFilesDir() { return files; }
    boolean isDestroyed() { return false; }
    void render() { renders++; }
    void updateGameNotice() { notices++; }
    boolean bindService(Intent intent, ServiceConnection connection, int flags) {
        binds++; bindFlags = flags;
        if (duringBind != null) duringBind.run();
        return bindAccepted;
    }
    void unbindService(ServiceConnection connection) { unbinds++; }
    void onStatus() { statuses++; }
    void syncMirror(boolean manual) { syncs++; }
    void refreshGameState() { checks++; }
    void disarmCancel() { cancelArmedAt = 0; }
    Object getMainExecutor() { return this; }
    <T> T getSystemService(Class<T> type) { return type.cast(storage); }
    static void require(boolean condition, String reason) { if (!condition) throw new AssertionError(reason); }
'''

JAVA_TESTS = r'''
    public static void main(String[] args) throws Exception {
        SetupActivity activity = new SetupActivity(new File(args[1]));
        int[] mutations = {0}, details = {0};
        switch (args[0]) {
            case "focus": {
                InstallState.next = activity.gameState = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                Widgets.Row row = new Widgets.Row();
                row.setOnFocusChangeListener((v, has) -> { if (has) details[0]++; });
                row.setOnClickListener(v -> { throw new AssertionError("original mutation bypassed game gate"); });
                Runnable retry = () -> mutations[0]++;
                activity.gameControl(row, "Changing the driver", retry);
                require(row.isEnabled() && row.requestFocus(), "gated row no longer accepts D-pad focus");
                require(row.dimmed() && details[0] == 1, "gating lost the dimmed look or original detail listener");
                require(activity.blockedAction == retry, "focus did not remember the exact intended operation");
                row.performClick();
                require(mutations[0] == 0 && activity.blockedAction == retry && activity.renders == 1, "activation mutated instead of showing the running-game reason");
                View card = new View();
                activity.gameControl(card, "Moving game data", retry);
                require(card.requestFocus() && card.alpha == .42f, "gated card lost focus or dimming");
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
                row.performClick();
                require(mutations[0] == 1, "cleared game gate did not allow the recorded operation");
                View unavailable = new View(); unavailable.setEnabled(false);
                activity.gameControl(unavailable, "Installing", retry);
                require(!unavailable.isEnabled(), "gating enabled a control disabled for another reason");
                break;
            }
            case "free-control": {
                InstallState.next = activity.gameState = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
                View view = new View();
                boolean[] busy = {true};
                view.setOnClickListener(v -> { details[0]++; if (!busy[0]) mutations[0]++; });
                activity.gameControl(view, "Changing saves", () -> mutations[0]++);
                view.performClick();
                require(details[0] == 1 && mutations[0] == 0, "free control lost its original busy guard");
                busy[0] = false;
                view.performClick();
                require(details[0] == 2 && mutations[0] == 1, "free control lost its original activation listener");
                break;
            }
            case "stale-save": {
                Files.createDirectories(new File(activity.files, "state").toPath());
                Files.writeString(new File(activity.files, "state/save-busy").toPath(), "left by dead game");
                activity.closingGame = true; activity.blockedAction = () -> mutations[0]++;
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "lock=free marker=absent");
                activity.closePoll.run();
                require(mutations[0] == 1 && !activity.closingGame && activity.note.isEmpty(), "stale save-busy blocked completion after game death");
                require(activity.main.pending.isEmpty(), "completed close kept waiting for the dead game's save");
                break;
            }
            case "live-save": {
                Files.createDirectories(new File(activity.files, "state").toPath());
                Files.writeString(new File(activity.files, "state/save-busy").toPath(), "saving");
                activity.closingGame = true; activity.blockedAction = () -> mutations[0]++;
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended marker=absent");
                activity.closePoll.run();
                require(mutations[0] == 0 && activity.closingGame && !activity.closeSent, "close did not wait for a live game's save");
                require(activity.main.pending.contains(activity.closePoll), "live save wait was not scheduled");
                require(!new File(activity.files, "state/close-request").exists(), "close command sent during a live save");
                break;
            }
            case "late-ui": {
                activity.onStart();
                require(activity.started && !activity.uiStarted && activity.main.pending.isEmpty(), "invisible skip-menu path registered UI work");
                activity.showUi();
                require(activity.uiStarted && activity.storage.registered == 1 && SetupService.listens == 1, "late UI did not start its listeners/timer");
                require(activity.main.pending.contains(activity.refresh), "late UI has no refresh callback");
                activity.startUi();
                require(activity.storage.registered == 1 && activity.main.posted == 1, "late UI registered duplicate callbacks");
                SystemClock.now = 1000;
                activity.main.pending.remove(activity.refresh); activity.refresh.run();
                require(activity.checks == 1 && activity.main.pending.size() == 1, "one-second game-state refresh is missing");
                activity.onStop();
                require(!activity.started && !activity.uiStarted && activity.storage.removed == 1 && activity.main.pending.isEmpty(), "stop leaked refresh/listeners");
                int posted = activity.main.posted; activity.refresh.run();
                require(activity.main.posted == posted, "late callback restarted polling after stop");
                activity.onStart();
                require(activity.storage.registered == 2 && activity.main.pending.size() == 1, "resume failed to register exactly one timer");
                break;
            }
            case "bind-save": {
                Files.createDirectories(new File(activity.files, "state").toPath());
                Path saving = new File(activity.files, "state/save-busy").toPath();
                Files.writeString(saving, "saving");
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.closeGame();
                require(activity.binds == 1 && activity.bindFlags == BIND_AUTO_CREATE, "foreground binding missing or over-prioritized");
                activity.closePoll.run();
                require(!activity.closeSent && activity.closeConnection != null, "save was not allowed to progress while bound");
                Files.delete(saving);
                activity.closePoll.run();
                require(activity.closeSent && Files.exists(new File(activity.files, "state/close-request").toPath()), "finished save did not proceed to native close");
                require(activity.unbinds == 0, "binding released before native close finished");
                break;
            }
            case "disconnect": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                ServiceConnection connection = activity.closeConnection;
                connection.onServiceDisconnected(null);
                activity.closePoll.run();
                require(activity.closingGame && mutations[0] == 0, "disconnect bypassed the lock");
                InstallState.next = new GameLock.Result(GameLock.State.FREE, "lock=free");
                connection.onServiceDisconnected(null);
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(!activity.closingGame && activity.unbinds == 1 && mutations[0] == 1, "game death did not release binding and retry");
                connection.onBindingDied(null);
                require(activity.main.pending.isEmpty(), "late death callback restarted a completed close");
                break;
            }
            case "bind-race": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.duringBind = () -> InstallState.next = new GameLock.Result(GameLock.State.FREE, "empty default process, no lock");
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(activity.binds == 1 && activity.unbinds == 1 && mutations[0] == 1, "empty service process was mistaken for GAME");
                require(!new File(activity.files, "state/close-request").exists(), "empty process received a close request");
                break;
            }
            case "bind-failure": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.bindAccepted = false;
                activity.closeGame();
                require(!activity.closingGame && activity.closeConnection == null && activity.unbinds == 1, "refused bind leaked close state");
                require(!activity.note.isEmpty() && activity.main.pending.isEmpty(), "refused bind silently kept polling");
                break;
            }
            case "close-timeout": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.closeGame(); activity.closePoll.run();
                SystemClock.now = activity.closeDeadline;
                activity.closePoll.run();
                require(!activity.closingGame && activity.unbinds == 1 && activity.closeConnection == null, "timeout leaked foreground binding");
                require(!new File(activity.files, "state/close-request").exists() && activity.main.pending.isEmpty(), "timeout left its request/poller behind");
                break;
            }
            case "stop-close": {
                InstallState.next = new GameLock.Result(GameLock.State.GAME, "lock=contended");
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame(); activity.closePoll.run();
                ServiceConnection old = activity.closeConnection;
                activity.onStop();
                require(!activity.closingGame && activity.closeConnection == null && activity.unbinds == 1, "stop/recreation leaked binding");
                require(!new File(activity.files, "state/close-request").exists() && activity.main.pending.isEmpty(), "stop leaked request/poller");
                old.onServiceConnected(null, null);
                activity.closePoll.run();
                require(activity.main.pending.isEmpty() && mutations[0] == 0, "old activity retried an abandoned action");
                activity.onStart();
                activity.closeGame();
                require(activity.binds == 2 && activity.closeConnection != old, "resumed activity reused old connection");
                old.onServiceDisconnected(null);
                require(activity.closingGame, "old disconnect changed a new close");
                break;
            }
            case "already-free": {
                activity.blockedAction = () -> mutations[0]++;
                activity.closeGame();
                activity.main.pending.remove(activity.closePoll); activity.closePoll.run();
                require(activity.binds == 0 && mutations[0] == 1 && !activity.closingGame, "FREE lock spawned a service process");
                break;
            }
            case "logging": {
                InstallState.logGameDecision("game decision=GAME branch=lock pid=42 marker=absent");
                require(android.util.Log.lines.size() == 1, "branch log was filtered out with INFO disabled");
                String line = android.util.Log.lines.get(0);
                require(line.contains("TotkSetup") && line.contains("branch=lock") && line.contains("pid=42") && line.contains("marker=absent"), "branch context lost");
                require(line.contains("logInfoEnabled=false") && line.contains("logWarnEnabled=true"), "effective logging levels were not diagnosed");
                break;
            }
        }
        System.out.println("passed " + args[0]);
    }
}
'''

LOG = r'''package android.util;
import java.util.*;
public final class Log {
    public static final int INFO = 4, WARN = 5;
    public static final List<String> lines = new ArrayList<>();
    public static boolean isLoggable(String tag, int level) { return level >= WARN; }
    public static int w(String tag, String text) { if (isLoggable(tag, WARN)) lines.add(tag + ": " + text); return 0; }
    public static int w(String tag, String text, Throwable failed) { return w(tag, text); }
}
'''

INPUT_PRELUDE = r'''#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int close_attempts, keys;
static _Noreturn void runtime_fail(const char *reason) { fprintf(stderr,"%s\n",reason); exit(7); }
static void totk_thread_start(const char *name, void *unused) { (void)name; (void)unused; }
static int runtime_survey_command(const char *line) { (void)line; return 0; }
static void runtime_input_key_synthetic(unsigned code, int down) { if (code == 38) keys += down ? 1 : 10; }
static int runtime_front_request_close(void) { return ++close_attempts == 2; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_anomdump(void) {}
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
static int runtime_hid_feedback_flag(const char *name, int value) { (void)name; (void)value; return 0; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
static void dm_gfx_shot_at(long frame, const char *path) { (void)frame; (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
static void runtime_swkbd_selftest(void) {}
static void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
static void runtime_hle_trace_set(int mode) { (void)mode; }
static void runtime_frame_step_pin(double ms) { (void)ms; }
static void runtime_dump_threads(void) {}
'''


class SetupControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.java, javac, cc = shutil.which('java'), shutil.which('javac'), shutil.which('cc')
        if not cls.java or not javac or not cc:
            raise unittest.SkipTest('host JDK and C compiler required')
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', ROOT / '.scratch/setup-tests')).resolve() / 'controls'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='controls-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        source = (JAVA / 'SetupActivity.java').read_text()
        state = (JAVA / 'InstallState.java').read_text()
        methods = section(source, '    private void gameControl(', '    private void gameBanner(')
        close = section(source, '    private void closeGame()', '    // ---------- lifecycle')
        start = section(source, '    @Override protected void onStart()', '    @Override protected void onResume()')
        stop = section(source, '    @Override protected void onStop()', '    /** A card mounted,')
        refresh = section(source, '    private final Runnable refresh', '    @Override protected void onDestroy()')
        show = section(source, '    private void showUi()', '    /** A move killed')
        show_tail = show.rsplit('        render();', 1)[1]
        logger = section(state, '    static void logGameDecision(', '    /** Only an acquired game lock')
        probe = cls.work / 'SetupActivity.java'
        probe.write_text(JAVA_PRELUDE + logger + JAVA_ACTIVITY + methods + close + start + stop + refresh
                         + '    private void showUi() { ui = true; render();' + show_tail + JAVA_TESTS)
        log = cls.work / 'android/util/Log.java'; log.parent.mkdir(parents=True); log.write_text(LOG)
        process = cls.work / 'android/os/Process.java'; process.parent.mkdir(parents=True)
        process.write_text('package android.os; public class Process { public static int myPid() { return 42; } }')
        cls.classes = cls.work / 'classes'
        cls.command([javac, '--release', '17', '-d', cls.classes, probe, log, process,
                     *[JAVA / name for name in ('GameLock.java', 'SaveManager.java', 'Settings.java')]])
        native = cls.work / 'input.c'
        input_source = (GAME / 'runtime/input.c').read_text()
        body = section(input_source, 'static void *input_file(', 'static void input_file_start(')
        native.write_text(INPUT_PRELUDE + body + r'''
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    input_file(argv[1]);
    if (close_attempts != 2 || keys != 11) return 3;
    puts("refused close retained command reader; later close accepted"); return 0;
}
''')
        cls.input = cls.work / 'input'
        cls.command([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', native, '-o', cls.input])

    @staticmethod
    def command(args):
        result = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=120)
        if result.returncode:
            raise AssertionError(f'{args}\n{result.stdout}\n{result.stderr}')
        return result

    def probe(self, mode):
        case = tempfile.mkdtemp(prefix='case-', dir=self.work)
        result = self.command([self.java, '-cp', self.classes, 'com.aure.totk.setup.SetupActivity', mode, case])
        self.assertIn('passed ' + mode, result.stdout)

    def test_gated_rows_and_cards_keep_focus_and_intercept_activation(self):
        self.probe('focus')

    def test_free_controls_retain_original_activation_guards(self):
        self.probe('free-control')

    def test_dead_game_stale_save_marker_does_not_block_pending_action(self):
        self.probe('stale-save')

    def test_live_game_save_still_blocks_close_request(self):
        self.probe('live-save')

    def test_close_binding_lifecycle_and_save_progress(self):
        for mode in ('bind-save', 'disconnect', 'bind-race', 'bind-failure', 'close-timeout', 'stop-close', 'already-free'):
            with self.subTest(mode=mode):
                self.probe(mode)

    def test_ui_created_after_onstart_registers_one_refresh_and_removes_it(self):
        self.probe('late-ui')

    def test_branch_warning_survives_info_filter_and_reports_effective_levels(self):
        self.probe('logging')

    def test_refused_diagnostic_close_does_not_end_command_reader(self):
        commands = self.work / 'commands.txt'
        commands.write_text('close\ndown 38\nup 38\nclose\n')
        result = self.command([self.input, commands])
        self.assertIn('refused close retained command reader', result.stdout)

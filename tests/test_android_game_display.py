"""Production display classification/launch decisions on a host JVM.

Framework doubles record Intent/ActivityOptions and deliver display callbacks; actual
Android task reuse/reparenting is an on-device acceptance test, not simulated here.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk'


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def local_class(source, name):
    source = re.sub(r'^(?:package|import) .*;\n', '', source, flags=re.MULTILINE)
    return source.replace('public final class ' + name, 'final class ' + name)


FRAMEWORK = r'''
class Bundle { int display; Bundle(int id) { display = id; } }
class Configuration {}
class Log { static int warnings; static void w(String tag, String message, Throwable error) { warnings++; } }
class DeviceProductInfo {
    final int sink; final String pnp;
    DeviceProductInfo(int sink, String pnp) { this.sink = sink; this.pnp = pnp; }
    int getConnectionToSinkType() { return sink; }
    String getManufacturerPnpId() { return pnp; }
}
class Display {
    static final int DEFAULT_DISPLAY = 0, FLAG_PRIVATE = 4;
    final int id; final DeviceProductInfo info;
    boolean valid = true; int flags;
    Display(int id, int sink, String pnp) { this.id = id; info = sink == -1 ? null : new DeviceProductInfo(sink, pnp); }
    int getDisplayId() { return id; }
    DeviceProductInfo getDeviceProductInfo() { return info; }
    boolean isValid() { return valid; }
    int getFlags() { return flags; }
}
class DisplayManager {
    static final String DISPLAY_CATEGORY_PRESENTATION = "presentation";
    final Display main = new Display(0, 0, "QCM");
    Display[] secondary = {};
    Display getDisplay(int id) {
        if (id == 0) return main;
        for (Display d : secondary) if (d.id == id) return d;
        return null;
    }
    Display[] getDisplays(String category) { return secondary; }
}
class Context {
    final DisplayManager manager = new DisplayManager();
    <T> T getSystemService(Class<T> type) { return type.cast(manager); }
}
class Intent {
    static final int FLAG_ACTIVITY_NEW_TASK = 0x10000000;
    final Class<?> target; int flags;
    Intent(Context owner, Class<?> target) { this.target = target; }
    Intent addFlags(int value) { flags |= value; return this; }
    boolean settings;
    Intent putExtra(String key, boolean value) { settings = value; return this; }
    boolean getBooleanExtra(String key, boolean fallback) { return settings; }
}
class ActivityOptions {
    int display;
    static ActivityOptions makeBasic() { return new ActivityOptions(); }
    ActivityOptions setLaunchDisplayId(int id) { display = id; return this; }
    Bundle toBundle() { return new Bundle(display); }
}
class Activity extends Context {
    Display current = manager.main;
    int launches, directLaunches; Intent launched; Bundle options; boolean fail;
    Display getDisplay() { return current; }
    void startActivity(Intent intent, Bundle bundle) {
        directLaunches++; record(intent, bundle);
    }
    void record(Intent intent, Bundle bundle) {
        launches++; launched = intent; options = bundle;
        if (fail) throw new SecurityException("unavailable");
    }
    protected void onNewIntent(Intent intent) {}
    void setIntent(Intent intent) {}
    public void onConfigurationChanged(Configuration config) {}
    void moved(int id) { current = manager.getDisplay(id); onConfigurationChanged(new Configuration()); }
}
class SetupActivity { static int opens; static void openOnGame(Activity owner) { opens++; } }
'''


PROBE = r'''
public final class GameDisplayProbe {
    static void require(boolean yes, String why) { if (!yes) throw new AssertionError(why); }
    static TotkActivity dual(int id, int sink) {
        TotkActivity owner = new TotkActivity();
        owner.manager.secondary = new Display[] {new Display(id, sink, "QCM")};
        return owner;
    }
    public static void main(String[] args) {
        switch (args[0]) {
            case "classification": {
                for (int[] panel : new int[][] {{2, 1}, {4, 0}}) {
                    TotkActivity owner = dual(panel[0], panel[1]);
                    Display second = owner.manager.secondary[0];
                    owner.current = second;
                    require(PresentationController.secondScreen(owner, second) == DisplayClass.NONE, "owner panel excluded");
                    require(PresentationController.deviceSecondScreen(owner) == DisplayClass.BUILT_IN, "bottom menu lost device capability");
                    require(PresentationController.builtInDisplay(owner.manager, owner.manager.main) == second, "companion not on bottom");
                    require(PresentationController.builtInDisplay(owner.manager, second) == null, "companion on game's panel");
                    second.flags = Display.FLAG_PRIVATE;
                    require(PresentationController.deviceSecondScreen(owner) == DisplayClass.NONE, "private panel used");
                    second.flags = 0; second.valid = false;
                    require(PresentationController.deviceSecondScreen(owner) == DisplayClass.NONE, "invalid panel used");
                }
                break;
            }
            case "launch": {
                Activity owner = new Activity();
                owner.manager.secondary = new Display[] {new Display(4, 0, "QCM")};
                for (Display from : new Display[] {owner.manager.main, owner.manager.secondary[0]}) {
                    owner.current = from;
                    GameLaunch.start(owner);
                    require(owner.launched.target == TotkActivity.class, "wrong activity");
                    require(owner.launched.flags == Intent.FLAG_ACTIVITY_NEW_TASK, "task launch flag missing");
                    require(owner.options != null && owner.options.display == 0, "not routed to main");
                    require(GameLaunch.onMainScreen(owner) == (from.id == 0), "wrong move decision");
                    GameLaunch.startSettings(owner);
                    require(owner.launched.settings && owner.options.display == 0, "Settings did not route through the live main game");
                }
                int direct = owner.directLaunches;
                GameLaunch.start(owner);
                require(owner.directLaunches == direct + 1 && owner.options.display == 0
                        && owner.launched.flags == Intent.FLAG_ACTIVITY_NEW_TASK, "existing game cleared or not routed to main");
                owner.manager.secondary = new Display[] {new Display(7, -1, null)};
                GameLaunch.start(owner);
                require(owner.options == null && owner.launched.flags == 0 && GameLaunch.onMainScreen(owner), "cast/overlay behavior changed");
                owner.manager.secondary = new Display[] {new Display(5, 2, "DEL")};
                GameLaunch.start(owner);
                require(owner.options == null && owner.launched.flags == 0, "external-monitor behavior changed");
                owner.manager.secondary = new Display[0]; owner.current = owner.manager.main;
                GameLaunch.start(owner);
                require(owner.options == null && owner.launched.flags == 0 && GameLaunch.onMainScreen(owner), "single-screen behavior changed");
                break;
            }
            case "reuse": {
                TotkActivity owner = dual(4, 0);
                owner.current = owner.manager.secondary[0];
                owner.onNewIntent(new Intent(owner, TotkActivity.class));
                require(owner.launches == 1 && owner.options.display == 0, "old task not redirected");
                owner.onNewIntent(new Intent(owner, TotkActivity.class));
                require(owner.launches == 1, "pending task move recursively relaunched");
                owner.moved(0);
                require(owner.launches == 1 && owner.companion.changed == 1, "main move relaunched or companion stale");
                owner.moved(4);
                require(owner.launches == 2 && owner.companion.changed == 2, "launcher task move not corrected");
                owner.moved(0);
                owner.fail = true; owner.moved(4);
                require(Log.warnings == 1, "move failure hidden");
                owner.fail = false; owner.onNewIntent(new Intent(owner, TotkActivity.class));
                require(owner.launches == 4, "failed move prevented later recovery");
                owner.moved(0);
                owner.resumed = false;
                owner.moved(4);
                require(owner.launches == 4, "background configuration change dismissed the menu");
                owner.resumed = true; owner.moved(0);
                owner.onNewIntent(new Intent(owner, TotkActivity.class).putExtra(GameLaunch.OPEN_SETTINGS, true));
                require(SetupActivity.opens == 1 && !owner.settingsRequested, "Settings not opened in game's task");
                owner.current = owner.manager.secondary[0];
                owner.onNewIntent(new Intent(owner, TotkActivity.class).putExtra(GameLaunch.OPEN_SETTINGS, true));
                require(SetupActivity.opens == 1 && owner.settingsRequested, "Settings opened before main-display handoff");
                owner.moved(0);
                require(SetupActivity.opens == 2 && !owner.settingsRequested, "deferred Settings intent lost");
                break;
            }
            default: throw new AssertionError(args[0]);
        }
        System.out.println("pass: " + args[0]);
    }
}
'''


class GameDisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-game-display-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        controller = (JAVA / 'companion/PresentationController.java').read_text()
        methods = section(controller, '    private static DisplayClass.Screen[] screens(', '    private void refresh()')
        methods = methods.replace('private static Display builtInDisplay', 'static Display builtInDisplay')
        activity = (JAVA / 'TotkActivity.java').read_text()
        callbacks = section(activity, '    private void ensureMainDisplay()', '    @Override protected void onResume()').replace('com.aure.totk.setup.SetupActivity', 'SetupActivity')
        source = (FRAMEWORK + local_class((JAVA / 'companion/DisplayClass.java').read_text(), 'DisplayClass')
                  + local_class((JAVA / 'GameLaunch.java').read_text(), 'GameLaunch')
                  + 'class PresentationController { int changed; void onDisplayChanged(int id) { changed++; }\n'
                  + methods + '}\nclass TotkActivity extends Activity { boolean displayMoveRequested, settingsRequested, resumed = true; '
                  + 'PresentationController companion = new PresentationController();\n' + callbacks + '}\n' + PROBE)
        path = cls.work / 'GameDisplayProbe.java'
        path.write_text(source)
        cls.run_command([javac, '--release', '17', '-d', cls.work, path])

    @classmethod
    def run_command(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

    def test_device_capability_is_independent_of_menu_display_and_companion_excludes_game(self):
        self.assertIn('pass: classification', self.run_command([self.java, '-cp', self.work, 'GameDisplayProbe', 'classification']))

    def test_only_built_in_dual_panels_route_game_task_to_main(self):
        self.assertIn('pass: launch', self.run_command([self.java, '-cp', self.work, 'GameDisplayProbe', 'launch']))

    def test_existing_bottom_game_task_moves_without_restarting_or_recursive_launches(self):
        self.assertIn('pass: reuse', self.run_command([self.java, '-cp', self.work, 'GameDisplayProbe', 'reuse']))


if __name__ == '__main__':
    unittest.main()

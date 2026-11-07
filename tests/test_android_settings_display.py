"""Execute production Settings handoff callbacks with recording framework doubles.

Task movement/reuse remains an Android device acceptance test; this checks that
we target our own task and preserve cold recovery / live gated actions.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/android/java/com/aure/totk/setup/SetupActivity.java'

FRAMEWORK = r'''
import java.util.*;
class Bundle { int display; Bundle(int id) { display=id; } }
class Display { static final int DEFAULT_DISPLAY=0; int id; Display(int id) { this.id=id; } int getDisplayId(){return id;} }
class Intent {
    static final int FLAG_ACTIVITY_NEW_TASK=0x10000000, FLAG_ACTIVITY_SINGLE_TOP=0x20000000, FLAG_ACTIVITY_REORDER_TO_FRONT=0x20000;
    static final String ACTION_VIEW="view", ACTION_MAIN="main";
    final Map<String,Object> extras=new HashMap<>(); int flags;
    Intent putExtra(String key,boolean value){extras.put(key,value);return this;}
    Intent() {} Intent(Intent other){extras.putAll(other.extras); flags=other.flags;}
    Intent addFlags(int value){flags|=value;return this;}
    Intent setFlags(int value){flags=value;return this;}
    Intent(Activity owner,Class<?> type){} Intent setAction(String action){return this;} int getFlags(){return flags;}
    String getAction(){return ACTION_VIEW;}
    boolean getBooleanExtra(String key,boolean fallback){return (boolean)extras.getOrDefault(key,fallback);}
    String getStringExtra(String key){return (String)extras.get(key);}
}
class ActivityOptions {
    int display; static ActivityOptions makeBasic(){return new ActivityOptions();}
    ActivityOptions setLaunchDisplayId(int id){display=id;return this;} Bundle toBundle(){return new Bundle(display);}
}
class ActivityManager {
    final List<AppTask> tasks=new ArrayList<>();
    List<AppTask> getAppTasks(){return tasks;}
    static class RecentTaskInfo {int taskId; RecentTaskInfo(int id){taskId=id;}}
    static class AppTask {
        final int id; int starts,fronts; Intent launched; Bundle options; boolean fail;
        AppTask(int id){this.id=id;} RecentTaskInfo getTaskInfo(){return new RecentTaskInfo(id);}
        void moveToFront(){fronts++;}
        void startActivity(Activity owner,Intent intent,Bundle bundle){
            starts++; launched=intent; options=bundle;
            if(fail)throw new SecurityException("denied");
        }
    }
}
class Icon {static Object createWithBitmap(Object bitmap){return bitmap;}}
class Strings {static final String SHORTCUT_SETTINGS="Settings";}
class ShortcutInfo {
    final String id; final Intent intent; ShortcutInfo(String id,Intent intent){this.id=id;this.intent=intent;}
    String getId(){return id;} Intent getIntent(){return intent;}
    static class Builder {
        final String id; Intent intent; Builder(Activity owner,String id){this.id=id;}
        Builder setShortLabel(String value){return this;} Builder setIcon(Object value){return this;}
        Builder setIntent(Intent value){intent=value;return this;} ShortcutInfo build(){return new ShortcutInfo(id,intent);}
    }
}
class ShortcutManager {
    List<ShortcutInfo> shortcuts=new ArrayList<>(); int updates;
    List<ShortcutInfo> getDynamicShortcuts(){return shortcuts;}
    void setDynamicShortcuts(List<ShortcutInfo> value){shortcuts=value;updates++;}
}
class Activity {
    final ActivityManager manager=new ActivityManager(); Display display=new Display(4); int taskId=17;
    Display getDisplay(){return display;} int getTaskId(){return taskId;}
    final ShortcutManager shortcuts=new ShortcutManager();
    <T> T getSystemService(Class<T> type){return type.cast(type==ShortcutManager.class?shortcuts:manager);}
    void setIntent(Intent intent){} protected void onNewIntent(Intent intent){}
    boolean finished; void finish(){finished=true;} void overridePendingTransition(int a,int b){}
    boolean isFinishing(){return finished;} boolean isDestroyed(){return false;}
    Intent launched; void startActivity(Intent intent){launched=intent;}
    Bundle options; void startActivity(Intent intent,Bundle bundle){launched=intent;options=bundle;}
}
class SettingsStore { static boolean dual=true; static boolean secondaryDisplay(Activity owner){return dual;} }
class GameLock {enum State {FREE,GAME} State state=State.FREE;}
class GameLaunch {static int settings; static void startSettings(Activity owner){settings++;}}
class InstallState {
    static final GameLock game=new GameLock();
    static GameLock gameState(Activity owner){return game;}
    static InstallState read(Activity owner){return new InstallState();} boolean installed(){return true;}
}
class Log { static void w(String tag,String message,Throwable error){} }
'''

PROBE = r'''
public final class SettingsDisplayProbe {
    static void check(boolean yes,String why){if(!yes)throw new AssertionError(why);}
    static Intent settings(){Intent i=new Intent();i.extras.put(SetupActivity.EXTRA_SETTINGS,true);return i;}
    static SetupActivity owner(){
        SettingsStore.dual=true;
        SetupActivity a=new SetupActivity();
        a.manager.tasks.add(new ActivityManager.AppTask(99));
        a.manager.tasks.add(new ActivityManager.AppTask(17));return a;
    }
    static void delivered(SetupActivity a,Intent intent){a.display=new Display(0);a.onNewIntent(intent);}
    public static void main(String[] args){
        if(args[0].equals("target")){
            SetupActivity a=owner(); Intent i=settings();
            check(a.moveSettingsToMain(i),"bottom Settings not handed off");
            ActivityManager.AppTask own=a.manager.tasks.get(1);
            check(a.manager.tasks.get(0).starts==0 && own.starts==1,"not bound to menu task");
            check(own.options.display==0 && own.launched.flags==Intent.FLAG_ACTIVITY_SINGLE_TOP,"wrong display/reuse flags");
            check(!a.moveSettingsToMain(i) && own.starts==1,"recursive handoff");
            delivered(a,i);check(!a.settingsMoving,"move guard not reset");
            a.display=new Display(4);a.settingsMoving=false;own.fail=true;
            check(!a.moveSettingsToMain(i) && !a.settingsMoving,"failed handoff latched");
            own.fail=false;check(a.moveSettingsToMain(i),"failed handoff cannot retry");
            a=owner();a.manager.tasks.clear();check(!a.moveSettingsToMain(i),"missing task used unbound fallback");
        }else if(args[0].equals("cold")){
            for(SetupActivity.Screen recovery:new SetupActivity.Screen[]{SetupActivity.Screen.ERROR,SetupActivity.Screen.INTERRUPTED}){
                for(boolean configFirst:new boolean[]{false,true}){
                    SetupActivity a=owner();a.recovery=recovery;Intent i=settings();
                    check(a.moveSettingsToMain(i) && !a.ui,"cold handoff initialized bottom UI");
                    Intent handoff=a.manager.tasks.get(1).launched;
                    if(configFirst){a.display=new Display(0);a.completeSettingsMove();}
                    delivered(a,handoff);
                    check(a.ui && a.shown==1 && a.screen==recovery && a.replacements==0,"cold recovery overwritten with Home");
                }
            }
        }else if(args[0].equals("pending")){
            SetupActivity a=owner();a.ui=true;a.screen=SetupActivity.Screen.DATA;
            Runnable retry=()->{};a.blockedAction=retry;Intent i=settings();
            check(a.moveSettingsToMain(i),"warm handoff missing");delivered(a,i);
            check(a.screen==SetupActivity.Screen.DATA && a.blockedAction==retry && a.replacements==0,"pending action lost");
            a.closingGame=true;a.display=new Display(4);a.onNewIntent(i);
            check(a.screen==SetupActivity.Screen.DATA && a.blockedAction==retry && a.manager.tasks.get(1).starts==1,"safe close interrupted");
            a=owner();a.ui=true;a.screen=SetupActivity.Screen.DATA;check(a.moveSettingsToMain(i),"warm settings handoff missing");delivered(a,i);
            check(a.screen==SetupActivity.Screen.HOME && a.replacements==1,"ordinary warm Settings no longer returns Home");
        }else if(args[0].equals("existing")){
            for(Intent i:new Intent[]{new Intent(),settings()}){
                SetupActivity previous=owner();
                SetupActivity.menuActivity=new java.lang.ref.WeakReference<>(null);
                check(!previous.reuseMenuTask(settings()),"first menu was forwarded");
                ActivityManager.AppTask old=previous.manager.tasks.get(1);
                SetupActivity a=owner();a.taskId=99; // getAppTasks no longer exposes the old task.
                check(a.reuseMenuTask(i) && a.finished && !a.ui,"new entry drew a duplicate menu");
                check(old.starts==0 && old.fronts==1 && previous.options.display==0 && a.launched==null,"menu owner token not used");
                check((previous.launched.flags&Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)!=0,"menu below a file picker would be duplicated");
                check((previous.launched.flags&Intent.FLAG_ACTIVITY_NEW_TASK)==0,"bound handoff incorrectly searched by root Intent");
                previous.finished=true;SetupActivity next=owner();
                check(!next.reuseMenuTask(i) && !next.finished,"finished menu revived");
            }
        }else if(args[0].equals("shortcut")){
            SetupActivity a=owner();a.shortcuts.shortcuts.add(new ShortcutInfo("menu",new Intent()));
            a.publishShortcut();check(a.shortcuts.updates==1,"old shortcut flags not migrated");
            Intent i=a.shortcuts.shortcuts.get(0).getIntent();
            check(i.getBooleanExtra(SetupActivity.EXTRA_SETTINGS,false)
                    && (i.flags&Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)!=0,"shortcut cannot open menu above picker");
            a.publishShortcut();check(a.shortcuts.updates==1,"unchanged shortcut republished / rate limited");
        }else if(args[0].equals("gameTask")){
            SetupActivity a=owner();Intent i=settings();
            check(!a.settingsOverGame(i),"cold Settings started a native game");
            InstallState.game.state=GameLock.State.GAME;
            check(a.settingsOverGame(i) && a.finished && GameLaunch.settings==1,"live Settings did not return through the game");
            a=owner();i.putExtra("com.aure.totk.setup.IN_GAME_TASK",true);
            check(!a.settingsOverGame(i),"game-task menu recursively routed");
            SetupActivity.openOnGame(a);
            check(a.launched.flags==0 && a.launched.getBooleanExtra("com.aure.totk.setup.IN_GAME_TASK",false),"menu detached from game caller task");
        }else if(args[0].equals("unchanged")){
            SetupActivity a=owner();Intent i=settings();a.display=new Display(0);
            check(!a.moveSettingsToMain(i),"main Settings moved");a.display=new Display(4);SettingsStore.dual=false;
            check(!a.moveSettingsToMain(i),"single/external screen changed");SettingsStore.dual=true;
            check(!a.moveSettingsToMain(new Intent()),"ordinary menu moved");
        }else throw new AssertionError(args[0]);
        System.out.println("pass: "+args[0]);
    }
}
'''


class SettingsDisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-settings-display-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        source = SOURCE.read_text()
        move = source[source.index('    private boolean settingsMoving;'):source.index('    /** A launcher tap')]
        new_intent = source[source.index('    @Override protected void onNewIntent(Intent intent)'):source.index('    @Override protected void onStart()')]
        shortcut = source[source.index('    private void publishShortcut()'):source.index('    private static Bitmap shortcutGlyph()')]
        route = source[source.index('    public static void openOnGame('):source.index('    @Override protected void onCreate(Bundle state)')]
        body = (move + new_intent + shortcut + route).replace('private boolean', 'boolean').replace('private void', 'void').replace('private static java.lang.ref.', 'static java.lang.ref.').replace('android.app.Activity', 'Activity').replace('android.view.Display', 'Display').replace('android.util.Log', 'Log')
        wrapper = r'''
class SetupActivity extends Activity {
    static final String EXTRA_SETTINGS="settings",EXTRA_GAME_ERROR="error",EXTRA_UPDATE="update";
    enum Screen {HOME,WELCOME,DATA,INSTALL,ERROR,INTERRUPTED}
    boolean ui,closingGame; Runnable blockedAction; Screen screen,recovery=Screen.HOME;
    String gameError; int shown,replacements;
    boolean pendingAsked;
    void requestedUpdate() {}
    void offerPendingUpdate() {}
    void showUi(){ui=true;shown++;screen=recovery;}
    boolean returnToRunningGame(Intent intent){return false;}
    void replace(Screen value){screen=value;replacements++;} void render(){}
    Object shortcutGlyph(){return new Object();}
'''
        path = cls.work / 'SettingsDisplayProbe.java'
        path.write_text(FRAMEWORK + wrapper + body + '}\n' + PROBE)
        cls.run_command([javac, '--release', '17', '-d', cls.work, path])

    @classmethod
    def run_command(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

    def probe(self, case):
        self.assertIn('pass: ' + case, self.run_command([self.java, '-cp', self.work, 'SettingsDisplayProbe', case]))

    def test_settings_handoff_targets_own_task_and_recovers_from_failure(self):
        self.probe('target')

    def test_cold_redirect_keeps_error_and_interrupted_recovery(self):
        self.probe('cold')

    def test_warm_handoff_keeps_gated_action_and_safe_close(self):
        self.probe('pending')

    def test_main_single_external_and_normal_menu_unchanged(self):
        self.probe('unchanged')

    def test_new_entry_forwards_to_existing_menu_without_drawing_or_clearing_game(self):
        self.probe('existing')

    def test_settings_shortcut_migrates_reorder_flags_without_repeated_updates(self):
        self.probe('shortcut')

    def test_only_live_game_routes_settings_into_the_game_task(self):
        self.probe('gameTask')


if __name__ == '__main__':
    unittest.main()

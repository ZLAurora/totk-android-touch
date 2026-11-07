"""Exercise production companion touch/focus policy with framework contract doubles.

The task manager's real multi-display routing is covered on devices; the host JVM
runs the unchanged lifecycle, gesture, and reclaim method bodies.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/android/java/com/aure/totk/companion/PresentationController.java'


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


PRELUDE = r'''
import java.util.*;
class Bundle {}
class SystemClock { static long now; static long uptimeMillis() { return now; } }
class Log { static int warnings; static void w(String tag, String text, Throwable error) { warnings++; } }
class ComponentName {
    final String name;
    ComponentName(String name) { this.name = name; }
    @Override public boolean equals(Object other) { return other instanceof ComponentName && name.equals(((ComponentName) other).name); }
    @Override public int hashCode() { return name.hashCode(); }
}
class ActivityManager {
    static class RecentTaskInfo { int taskId; ComponentName topActivity; }
    static class AppTask {
        final RecentTaskInfo info = new RecentTaskInfo();
        int moves;
        boolean fail;
        AppTask(int id, String top) { info.taskId = id; info.topActivity = new ComponentName(top); }
        RecentTaskInfo getTaskInfo() { return info; }
        void moveToFront() { moves++; if (fail) throw new SecurityException("task unavailable"); }
    }
    final List<AppTask> tasks = new ArrayList<>();
    int queries;
    List<AppTask> getAppTasks() { queries++; return tasks; }
}
class Activity {
    boolean focused = true, finishing, destroyed;
    final ActivityManager manager = new ActivityManager();
    final ComponentName component = new ComponentName("TotkActivity");
    boolean hasWindowFocus() { return focused; }
    boolean isFinishing() { return finishing; }
    boolean isDestroyed() { return destroyed; }
    int getTaskId() { return 17; }
    ComponentName getComponentName() { return component; }
    <T> T getSystemService(Class<T> type) { return type.cast(manager); }
}
class Application {
    interface ActivityLifecycleCallbacks {
        void onActivityStarted(Activity activity);
        void onActivityStopped(Activity activity);
        void onActivityCreated(Activity activity, Bundle state);
        void onActivityResumed(Activity activity);
        void onActivityPaused(Activity activity);
        void onActivitySaveInstanceState(Activity activity, Bundle state);
        void onActivityDestroyed(Activity activity);
    }
}
class Handler {
    final List<Runnable> pending = new ArrayList<>();
    final List<Runnable> delayed = new ArrayList<>();
    int posts;
    void post(Runnable value) { pending.add(value); posts++; }
    void postDelayed(Runnable value, long ms) { if (ms != 300) throw new AssertionError("unexpected debounce"); delayed.add(value); }
    void removeCallbacks(Runnable value) { pending.removeIf(x -> x == value); delayed.removeIf(x -> x == value); }
    void drainDelayed() { SystemClock.now += 300; List<Runnable> copy = new ArrayList<>(delayed); delayed.clear(); for (Runnable value : copy) value.run(); }
    void drain() { while (!pending.isEmpty()) pending.remove(0).run(); }
}
class MotionEvent {
    static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6;
    final int action;
    MotionEvent(int action) { this.action = action; }
    int getActionMasked() { return action; }
}
class Presentation {
    boolean showing = true, handled = true;
    int dispatched;
    boolean isShowing() { return showing; }
    public boolean dispatchTouchEvent(MotionEvent event) { dispatched++; return handled; }
}
public final class CompanionFocusProbe {
    final Activity owner = new Activity();
    final Handler main = new Handler();
    Presentation shown;
    final ActivityManager.AppTask task = new ActivityManager.AppTask(17, "TotkActivity");
    int refreshes, dismissals;
    void refresh() { refreshes++; }
    void dismiss() { dismissals++; shown = null; }
'''

TESTS = r'''
    CompanionFocusProbe() {
        owner.manager.tasks.add(new ActivityManager.AppTask(3, "SetupActivity"));
        owner.manager.tasks.add(task);
        shown = new TouchPresentation();
        lifecycle.onActivityResumed(owner);
        ownerTopResumed(true);
        SystemClock.now = 1000;
    }
    static void require(boolean condition, String reason) { if (!condition) throw new AssertionError(reason); }
    void event(int action) { shown.dispatchTouchEvent(new MotionEvent(action)); }
    void tap() { event(MotionEvent.ACTION_DOWN); event(MotionEvent.ACTION_UP); }
    public static void main(String[] args) {
        CompanionFocusProbe p = new CompanionFocusProbe();
        switch (args[0]) {
            case "gesture": {
                p.event(MotionEvent.ACTION_DOWN);
                p.ownerTopResumed(false);
                for (int i = 0; i < 100; i++) p.event(MotionEvent.ACTION_MOVE);
                p.event(MotionEvent.ACTION_POINTER_DOWN);
                p.event(MotionEvent.ACTION_POINTER_UP);
                require(p.task.moves == 0 && p.main.posts == 0, "drag/pinch reclaimed before final UP");
                p.event(MotionEvent.ACTION_UP);
                require(p.task.moves == 0 && p.main.posts == 1, "reclaim must follow dispatch, not interrupt it");
                p.main.drain();
                require(p.task.moves == 1 && p.owner.manager.tasks.get(0).moves == 0, "moved setup or wrong task");
                p.ownerTopResumed(true);
                for (int i = 0; i < 10; i++) { p.tap(); p.main.drain(); }
                require(p.task.moves == 1, "already-top owner repeatedly moved to front");
                p.event(MotionEvent.ACTION_DOWN); p.event(MotionEvent.ACTION_UP);
                p.ownerTopResumed(false); // DOWN's framework callback may arrive after UP but before its posted check.
                p.main.drain();
                require(p.task.moves == 2 && p.owner.manager.queries == 1, "late callback or cached own task failed");
                break;
            }
            case "cancel": {
                p.ownerTopResumed(false);
                p.event(MotionEvent.ACTION_UP);
                p.event(MotionEvent.ACTION_DOWN); p.event(MotionEvent.ACTION_CANCEL); p.event(MotionEvent.ACTION_UP);
                p.main.drain();
                require(p.task.moves == 0 && p.main.posts == 0, "interrupted or unowned gesture stole focus");
                p.event(MotionEvent.ACTION_DOWN); p.event(MotionEvent.ACTION_UP);
                p.lifecycle.onActivityPaused(p.owner); // Home/recents can arrive before the queued return runs.
                p.main.drain();
                require(p.task.moves == 0, "queued touch undid lifecycle pause");
                p.lifecycle.onActivityResumed(p.owner);
                p.owner.focused = false;
                p.tap(); p.main.drain();
                require(p.task.moves == 0, "a system overlay on the game display lost focus");
                break;
            }
            case "guards": {
                p.ownerTopResumed(false);
                p.owner.finishing = true; p.tap(); p.main.drain(); p.owner.finishing = false;
                p.owner.destroyed = true; p.tap(); p.main.drain(); p.owner.destroyed = false;
                p.closed = true; p.tap(); p.main.drain(); p.closed = false;
                p.stopped = true; p.tap(); p.main.drain(); p.stopped = false;
                p.task.info.topActivity = new ComponentName("SetupActivity"); p.tap(); p.main.drain();
                require(p.task.moves == 0, "inactive owner or setup top was moved");
                p.task.info.topActivity = p.owner.component;
                p.lifecycle.onActivityPaused(p.owner);
                p.lifecycle.onActivityResumed(new Activity());
                p.tap(); p.main.drain();
                require(p.task.moves == 0, "another activity resumed the owner");
                p.lifecycle.onActivityResumed(p.owner);
                p.lifecycle.onActivityPaused(new Activity());
                p.tap(); p.main.drain();
                require(p.task.moves == 1, "another activity paused the owner");
                break;
            }
            case "retry": {
                p.ownerTopResumed(false);
                p.tap(); p.tap();
                require(p.main.posts == 1, "same-loop gestures were not coalesced");
                p.main.drain();
                require(p.task.moves == 1, "first claim missing");
                SystemClock.now += 249; p.tap(); p.main.drain();
                require(p.task.moves == 1, "pending/no-op claim spammed moveToFront");
                SystemClock.now++; p.tap(); p.main.drain();
                require(p.task.moves == 2, "no-op claim permanently blocked the next touch");
                SystemClock.now += 250; p.task.fail = true; p.tap(); p.main.drain();
                require(Log.warnings == 1, "failed claim not reported");
                p.task.fail = false; p.tap(); p.main.drain();
                require(p.task.moves == 4, "failed claim permanently blocked the next touch");
                break;
            }
            case "automatic": {
                p.ownerTopResumed(false);
                require(p.task.moves == 0 && p.main.delayed.size() == 1, "overlay loss was not debounced");
                p.main.drainDelayed();
                require(p.task.moves == 1, "overlay loss did not return controller focus");
                for (int i = 0; i < 3; i++) {
                    p.ownerTopResumed(true);
                    SystemClock.now += 100;
                    p.ownerTopResumed(false);
                    p.main.drainDelayed();
                }
                require(p.task.moves == 3 && p.autoReturnBlocked, "competing window was fought indefinitely");
                SystemClock.now += 2000;
                p.ownerTopResumed(false); p.main.drainDelayed();
                require(p.task.moves == 3, "circuit breaker reset without player intent");
                p.tap(); p.main.drain();
                require(p.task.moves == 4 && !p.autoReturnBlocked, "companion touch did not reset recovery");
                CompanionFocusProbe paused = new CompanionFocusProbe();
                paused.ownerTopResumed(false);
                paused.lifecycle.onActivityPaused(paused.owner);
                paused.main.drainDelayed();
                require(paused.task.moves == 0, "automatic recovery undid a lifecycle pause");
                paused.lifecycle.onActivityResumed(paused.owner);
                paused.ownerTopResumed(false);
                paused.event(MotionEvent.ACTION_DOWN);
                paused.main.drainDelayed();
                require(paused.task.moves == 0, "automatic recovery interrupted a companion drag");
                paused.event(MotionEvent.ACTION_CANCEL);
                paused.main.drain();
                require(paused.task.moves == 0, "cancelled drag requested recovery");
                break;
            }
            default: throw new AssertionError("unknown scenario");
        }
        System.out.println("passed " + args[0]);
    }
}
'''


class CompanionFocusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.java, javac = shutil.which('java'), shutil.which('javac')
        if not cls.java or not javac:
            raise unittest.SkipTest('host JDK required')
        cls.temp = tempfile.TemporaryDirectory(prefix='companion-focus-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        source = SOURCE.read_text()
        fields = section(source, '    private boolean closed,', '    public PresentationController(')
        top = section(source, '    public void ownerTopResumed(', '    /** Coalesced')
        reclaim = section(source, '    private boolean returnInputToOwner()', '    private void dismiss()')
        touch = section(source, '            private boolean touching;', '            @Override protected void onCreate(')
        probe = cls.work / 'CompanionFocusProbe.java'
        probe.write_text(PRELUDE + fields + top + reclaim + '\n    final class TouchPresentation extends Presentation {\n'
                         + touch + '\n    }\n' + TESTS)
        cls.command([javac, '--release', '17', '-d', cls.work, probe])

    @staticmethod
    def command(args):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def probe(self, scenario):
        result = self.command([self.java, '-cp', self.work, 'CompanionFocusProbe', scenario])
        self.assertIn('passed ' + scenario, result.stdout)

    def test_final_touch_returns_only_the_exact_owner_task(self):
        self.probe('gesture')

    def test_interrupted_gestures_and_queued_background_touches_never_refocus(self):
        self.probe('cancel')

    def test_owner_lifecycle_window_and_task_top_guards(self):
        self.probe('guards')

    def test_claims_coalesce_and_failed_or_noop_claims_can_retry_on_touch(self):
        self.probe('retry')

    def test_overlay_focus_loss_is_debounced_and_competing_windows_stop_recovery(self):
        self.probe('automatic')


class NativeInputFocusTests(unittest.TestCase):
    def test_routing_changes_release_held_input_without_resuming_a_paused_game(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('host C compiler required')
        source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        reset = section(source, 'void runtime_android_game_input_focus(', 'static void android_game_lifecycle_pause(')
        gate = section(source, 'static int handle_input(', '    int pad = ')
        prelude = r'''
#include <stdint.h>
#include <assert.h>
typedef void JNIEnv;
typedef void *jclass;
typedef unsigned char jboolean;
typedef struct { int type; int64_t time, down; } AInputEvent;
enum { AINPUT_EVENT_TYPE_KEY = 1, AINPUT_EVENT_TYPE_MOTION = 2 };
static int paused, pad, synthetic, clears, pad_clears, quiets;
static int64_t game_input_after_ns, now = 100;
static uint64_t mono_ns(void) { return (uint64_t)now; }
static int runtime_android_game_paused(void) { return paused; }
static void runtime_gamepad_clear(void) { pad = 0; pad_clears++; }
static void runtime_input_clear(void) { synthetic = 0; clears++; runtime_gamepad_clear(); }
static void runtime_feedback_quiet(void) { quiets++; }
static int AInputEvent_getType(AInputEvent *e) { return e->type; }
static int AInputEvent_getSource(AInputEvent *e) { (void)e; return 0; }
static int64_t AKeyEvent_getEventTime(AInputEvent *e) { return e->time; }
static int64_t AKeyEvent_getDownTime(AInputEvent *e) { return e->down; }
static int64_t AMotionEvent_getEventTime(AInputEvent *e) { return e->time; }
'''
        assertions = r'''
int main(void) {
    paused = 1; pad = 1024; synthetic = 1;
    runtime_android_game_input_focus(0, 0, 0);   /* routing lost: everything held is released */
    assert(paused == 1 && pad == 0 && synthetic == 0 && clears == 1 && quiets == 1 && game_input_after_ns == 100);
    now = 200; pad = 1024; synthetic = 1;         /* a companion shortcut issued before the reclaim lands */
    runtime_android_game_input_focus(0, 0, 1);   /* routing back: the controller resets, the shortcut survives */
    assert(paused == 1 && pad == 0 && synthetic == 1 && clears == 1 && pad_clears == 2 && quiets == 1
           && game_input_after_ns == 200);
    AInputEvent fresh = {AINPUT_EVENT_TYPE_KEY, 201, 201};
    assert(handle_input(&fresh) == 1); /* top-resumed cannot undo Home/recents pause */
    paused = 0;
    assert(handle_input(&fresh) == 0);
    AInputEvent held = {AINPUT_EVENT_TYPE_KEY, 202, 199};
    assert(handle_input(&held) == 1); /* held key/release from the old display is not replayed */
    AInputEvent queued = {AINPUT_EVENT_TYPE_MOTION, 199, 0};
    assert(handle_input(&queued) == 1);
    queued.time = 201;
    assert(handle_input(&queued) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='companion-input-focus-') as directory:
            work = Path(directory)
            probe = work / 'probe.c'
            probe.write_text(prelude + reset + gate + '    (void)src; return 0;\n}\n' + assertions)
            CompanionFocusTests.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', probe, '-o', work / 'probe'])
            CompanionFocusTests.command([work / 'probe'])

"""Compile and drive the real CoverActivity callbacks on a host JVM.

Framework doubles expose lifecycle methods for deterministic ordering only; they do not
model Android task selection, launch delivery, display policy, or OEM behavior.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
JAVA = TOTK / 'android/java/com/aure/totk/companion'
ACTIVITY = JAVA / 'CoverActivity.java'
PROBE = TOTK / 'tests/displays/CoverActivityProbe.java'

STUBS = {
    'android/os/Bundle.java': 'package android.os; public class Bundle {}',
    'android/content/Intent.java': '''package android.content;
import java.util.HashMap;
public class Intent {
 private final HashMap<String, Long> longs = new HashMap<>();
 public Intent putExtra(String key, long value) { longs.put(key, value); return this; }
 public long getLongExtra(String key, long fallback) { return longs.getOrDefault(key, fallback); }
}''',
    'android/app/Activity.java': '''package android.app;
import android.content.Intent; import android.os.Bundle; import android.view.Display; import android.view.Window; import android.view.View;
public class Activity {
 private Intent intent; private Display display; private boolean finished, destroyed; private final Window window = new Window();
 protected void onCreate(Bundle state) {} protected void onNewIntent(Intent value) {} protected void onDestroy() { destroyed = true; }
 public void onTopResumedActivityChanged(boolean top) {}
 public boolean isFinishing() { return finished; } public boolean isDestroyed() { return destroyed; }
 public void topResumedForTest(boolean top) { onTopResumedActivityChanged(top); }
 public Window getWindow() { return window; } public void setContentView(View view) {}
 public void overridePendingTransition(int enter, int exit) {} public void finishAndRemoveTask() { finished = true; }
 public Intent getIntent() { return intent; } public void setIntent(Intent value) { intent = value; }
 public Display getDisplay() { return display; }
 public void createForTest(Bundle state) { onCreate(state); }
 public void newIntentForTest(Intent value) { onNewIntent(value); }
 public void destroyForTest() { onDestroy(); }
 public void setDisplayForTest(Display value) { display = value; }
 public boolean isFinishedForTest() { return finished; }
}''',
    'android/graphics/Color.java': 'package android.graphics; public final class Color { public static final int BLACK = 0xff000000; }',
    'android/util/Log.java': 'package android.util; public final class Log { public static int i(String tag, String msg) { return 0; } public static int w(String tag, String msg) { return 0; } }',
    'android/view/Display.java': 'package android.view; public final class Display { private final int id; public Display(int id) { this.id = id; } public int getDisplayId() { return id; } }',
    'android/view/View.java': 'package android.view; public class View { public View(Object context) {} public void setBackgroundColor(int color) {} }',
    'android/view/Window.java': 'package android.view; public class Window { public void addFlags(int flags) {} }',
    'android/view/WindowManager.java': 'package android.view; public interface WindowManager { final class LayoutParams { public static final int FLAG_NOT_FOCUSABLE=8, FLAG_NOT_TOUCHABLE=16; } }',
    'com/aure/totk/companion/PresentationController.java': 'package com.aure.totk.companion; import android.view.Window; public final class PresentationController { static void immersive(Window window, boolean value) {} }',
    'com/aure/totk/companion/CompanionNative.java': '''package com.aure.totk.companion;
public final class CompanionNative {
 public static boolean logCover(String message) { System.err.println("[TotkCompanion] " + message); return true; }
}''',
}


class CoverActivityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-cover-activity-')
        root = Path(cls.temp.name)
        sources = []
        for name, content in STUBS.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
            sources.append(str(path))
        sources += [str(ACTIVITY), str(PROBE)]
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(root), *sources],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, scenario):
        result = subprocess.run([self.java, '-cp', self.temp.name, 'com.aure.totk.companion.CoverActivityProbe', scenario],
                                check=True, capture_output=True, text=True, timeout=60)
        self.last_stderr = result.stderr
        return result.stdout.strip().splitlines()

    def test_canceled_pending_token_finishes_when_activity_is_created(self):
        lines = self.probe('canceled-pending')
        self.assertIn('canceled  finished=true', lines)
        self.assertIn('[TotkCompanion] cover finish token=101 reason=pending_cancel\n', self.last_stderr)

    def test_two_accepted_intents_ignore_canceled_stale_create_and_bind_next(self):
        lines = self.probe('two-intents')
        self.assertIn('stale  finished=true', lines)
        self.assertIn('next attached finished=false', lines)

    def test_new_intent_rebind_releases_old_registry_and_stale_destroy_preserves_new(self):
        lines = self.probe('rebind')
        self.assertIn('rebound attached,top:301:true,finished:301,attached,top:302:true finished=false', lines)
        self.assertIn('top301=false top302=true', lines)
        self.assertEqual(self.last_stderr.count('[TotkCompanion] cover finish token=301 reason=new_intent\n'), 1)
        self.assertIn('stale311=false current312=true', self.probe('stale-destroy')[0])

    def test_restored_activity_without_live_launch_self_finishes(self):
        lines = self.probe('restored')
        self.assertIn('restored  finished=true', lines)
        self.assertIn('events=[]', lines)
        self.assertIn('[TotkCompanion] cover finish token=401 reason=restored\n', self.last_stderr)

    def test_expected_display_attaches_and_wrong_display_is_refused(self):
        lines = self.probe('displays')
        self.assertIn('right attached finished=false', lines)
        self.assertIn('wrong attached,refused:502 finished=true', lines)
        self.assertIn('[TotkCompanion] cover refusal token=502 reason=wrong_display\n', self.last_stderr)

    def test_top_resumed_authority_is_current_token_bound_and_cleared(self):
        lines = self.probe('top-resumed')
        self.assertIn('top601=true events=[attached, top:601:true, top:601:false, top:601:true]', lines)
        self.assertIn('afterCancel=false', lines)
        self.assertIn('stale602=false current603=false', lines)
        self.assertTrue(any(line.startswith('afterStaleCallback=false events=') for line in lines))
        self.assertIn('[TotkCompanion] cover finish token=601 reason=session_end\n', self.last_stderr)


if __name__ == '__main__':
    unittest.main()

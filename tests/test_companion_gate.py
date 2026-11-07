"""Exercise production companion JNI, startup ordering and poller gating without Android or game data.

A real host JVM loads the bridge; small Android lifecycle/queue observers replace only framework/UI types.
This does not exercise ART's NativeActivity loader, real display listeners or Android Handler scheduling.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
JAVA = TOTK / 'android/java'
FIXTURES = TOTK / 'tests/companion'

STUBS = {
    'android/os/Bundle.java': 'package android.os; public final class Bundle {}',
    'android/os/Looper.java': '''package android.os;
public final class Looper {
    public static Looper getMainLooper() { return new Looper(); }
}''',
    'android/os/Handler.java': '''package android.os;
public final class Handler {
    public Handler(Looper looper) {}
    public boolean post(Runnable callback) { return true; }
    public boolean postDelayed(Runnable callback, long delay) { return true; }
    public void removeCallbacksAndMessages(Object token) {}
}''',
    'android/os/HandlerThread.java': '''package android.os;
public final class HandlerThread {
    public static int started, stopped;
    public HandlerThread(String name) {}
    public void start() { started++; }
    public Looper getLooper() { return new Looper(); }
    public boolean quitSafely() { stopped++; return true; }
}''',
    'android/os/SystemClock.java': '''package android.os;
public final class SystemClock {
    public static long elapsedRealtime() { return 0; }
}''',
    'android/content/Context.java': '''package android.content;
public class Context {
    public void startActivity(Intent intent) { throw new AssertionError("the probe reports no game error"); }
}''',
    'android/content/Intent.java': '''package android.content;
public class Intent {
    public static final String ACTION_VIEW = "view";
    public Intent() {}
    public Intent(Context context, Class<?> target) {}
    public Intent setAction(String value) { return this; }
    public Intent putExtra(String name, boolean value) { return this; }
    public Intent setClassName(Context context, String name) { return this; }
    public Intent putExtra(String name, String value) { return this; }
    public boolean getBooleanExtra(String name, boolean fallback) { return fallback; }
}''',
    'android/content/res/Configuration.java': 'package android.content.res; public final class Configuration {}',
    'com/aure/totk/GameLaunch.java': '''package com.aure.totk;
public final class GameLaunch {
    static final String OPEN_SETTINGS = "com.aure.totk.OPEN_SETTINGS";
    static boolean onMainScreen(Object activity) { return true; }
    static void start(Object activity) {}
}''',
    'com/aure/totk/setup/SetupActivity.java': '''package com.aure.totk.setup;
public final class SetupActivity {
    public static final String EXTRA_GAME_ERROR = "com.aure.totk.setup.GAME_ERROR";
    public static final String EXTRA_UPDATE = "com.aure.totk.setup.UPDATE";
    public static void openOnGame(android.app.Activity game) {}
}''',
    'com/aure/totk/Updates.java': '''package com.aure.totk;
public final class Updates {
    public static void check(android.content.Context context, java.util.function.Consumer<String> result) { result.accept(null); }
}''',
    'com/aure/totk/setup/UpdatePrompt.java': '''package com.aure.totk.setup;
public final class UpdatePrompt {
    public static void show(android.app.Activity activity, String version, java.util.function.Consumer<Boolean> answer) {
        throw new AssertionError("update networking is outside this companion fixture");
    }
}''',
    'android/os/Build.java': '''package android.os;
public final class Build {
    public static final class VERSION { public static final int SDK_INT = 33; }
}''',
    'android/util/Log.java': '''package android.util;
public final class Log {
    public static int w(String tag, String message) { return 0; }
    public static int w(String tag, String message, Throwable error) { return 0; }
}''',
    'android/view/AttachedSurfaceControl.java': '''package android.view;
public interface AttachedSurfaceControl {
    interface OnBufferTransformHintChangedListener { void onBufferTransformHintChanged(int hint); }
    int getBufferTransformHint();
    void addOnBufferTransformHintChangedListener(OnBufferTransformHintChangedListener listener);
    void removeOnBufferTransformHintChangedListener(OnBufferTransformHintChangedListener listener);
}''',
    'android/view/Display.java': '''package android.view;
public final class Display {
    public int getDisplayId() { return 0; }
    public Mode[] getSupportedModes() { return new Mode[0]; }
    public static final class Mode { public float getRefreshRate() { return 60; } }
}''',
    'android/view/View.java': '''package android.view;
public class View {
    public boolean isAttachedToWindow() { return false; }
    public Display getDisplay() { return null; }
    public AttachedSurfaceControl getRootSurfaceControl() { return null; }
}''',
    'android/view/Window.java': '''package android.view;
public class Window {
    public View getDecorView() { return new View(); }
}''',
    'android/app/Activity.java': 'package android.app; public class Activity extends android.content.Context {}',
    'android/app/NativeActivity.java': '''package android.app;
import android.content.Context;
import android.os.Bundle;
import android.view.Window;
public class NativeActivity extends Activity {
    public static Runnable initialize, destroy;
    public static boolean initialized;
    protected void onCreate(Bundle state) { initialize.run(); initialized = true; }
    protected void onNewIntent(android.content.Intent intent) {}
    public void setIntent(android.content.Intent intent) {}
    public void onConfigurationChanged(android.content.res.Configuration config) {}
    protected void onResume() {}
    protected void onPause() {}
    protected void onStop() {}
    protected void onDestroy() { destroy.run(); }
    public boolean isFinishing() { return false; }
    public boolean hasWindowFocus() { return true; }
    public Window getWindow() { return new Window(); }
    public android.view.Display getDisplay() { return new android.view.Display(); }
    public void onWindowFocusChanged(boolean focused) {}
    public void onTopResumedActivityChanged(boolean top) {}
}''',
    'com/aure/totk/companion/AssetStore.java': '''package com.aure.totk.companion;
import android.content.Context;
public final class AssetStore {
    public static boolean available(Context context) { return true; }
}''',
    'com/aure/totk/setup/SaveMirror.java': '''package com.aure.totk.setup;
import android.content.Context;
public final class SaveMirror {
    public static final class Watcher {
        public static Watcher start(Context context) { return null; }
        public void flush() {}
        public void close(long waitMs) {}
    }
}''',
    'com/aure/totk/GameFeedback.java': '''package com.aure.totk;
final class GameFeedback {
    GameFeedback(Object activity, int enabled) { throw new AssertionError("the probe gives the game no rumble or motion"); }
    static int hardware(Object context) { return 0; }
    void resume() {}
    void pause() {}
    void close() {}
    void rumble(int left, int right) {}
}''',
    'com/aure/totk/TextEntry.java': '''package com.aure.totk;
final class TextEntry {
    interface Answer { void done(String text); }
    static void show(Object activity, String prompt, String initial, TextRules rules, boolean secure, Answer answer) {}
}''',
    'com/aure/totk/companion/PresentationController.java': '''package com.aure.totk.companion;
import android.app.NativeActivity;
import android.content.Context;
import android.view.Display;
public final class PresentationController {
    public static int created, closed;
    public static int secondScreenKind;
    public static int secondScreen(Context context, Display own) { return secondScreenKind; }
    public PresentationController(NativeActivity owner, DataSource source) {
        if (!NativeActivity.initialized) throw new AssertionError("controller constructed before native initialization");
        created++;
    }
    public void ownerTopResumed(boolean top) {}
    public void onDisplayChanged(int id) {}
    public void close() { closed++; }
}''',
    'com/aure/totk/companion/Snapshot.java': '''package com.aure.totk.companion;
public final class Snapshot {
    public String status = "starting", fingerprint = "", actionKey = "";
    public static Snapshot parse(String text, Snapshot previous, boolean full) { return new Snapshot(); }
    public static Snapshot waiting() { return new Snapshot(); }
}''',
    'org/json/JSONException.java': '''package org.json;
public class JSONException extends Exception {
    public JSONException(String message) { super(message); }
}''',
    'org/json/JSONObject.java': '''package org.json;
public final class JSONObject {
    public JSONObject(String text) throws JSONException { throw new JSONException("not parsed by the gate stub"); }
    public int optInt(String key) { return 0; }
    public String optString(String key, String fallback) { return fallback; }
}''',
}


@unittest.skipUnless(sys.platform.startswith('linux'), 'host JNI bridge fixture needs Linux')
class CompanionGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        javac, java = shutil.which('javac'), shutil.which('java')
        if not compiler or not javac or not java:
            raise unittest.SkipTest('A C compiler and JDK 17+ are required for host JNI regressions')
        include = Path(javac).resolve().parents[1] / 'include'
        if not (include / 'jni.h').is_file():
            raise unittest.SkipTest('The selected JDK must include JNI headers')
        cls.java = java
        cls.compiler, cls.include = compiler, include
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.classes = cls.directory / 'classes'
        cls.classes.mkdir()
        sources = []
        for name, content in STUBS.items():
            source = cls.directory / 'stubs' / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(content, encoding='utf-8')
            sources.append(str(source))
        sources.extend(str(JAVA / name) for name in (
            'com/aure/totk/TotkActivity.java',
            'com/aure/totk/TextRules.java',
            'com/aure/totk/companion/CompanionNative.java',
            'com/aure/totk/companion/DataSource.java',
            'com/aure/totk/companion/DisplayClass.java',
            'com/aure/totk/companion/ItemActions.java',
            'com/aure/totk/companion/SnapshotPoller.java',
        ))
        sources.append(str(FIXTURES / 'CompanionGateProbe.java'))
        cls.command([javac, '--release', '17', '-d', str(cls.classes), *sources])
        cls.command([
            compiler, '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
            '-fPIC', '-shared', '-pthread', '-I' + str(include), '-I' + str(include / 'linux'),
            str(TOTK / 'runtime/companion_jni.c'), str(FIXTURES / 'jni_probe.c'),
            '-o', str(cls.directory / 'libtotk.so'),
        ])
        cls.native_only = cls.directory / 'native-only'
        cls.native_only.mkdir()
        no_companion = cls.directory / 'NativeOnlyProbe.java'
        no_companion.write_text('''public final class NativeOnlyProbe {
    public static void main(String[] args) {
        System.loadLibrary("totk");
        System.out.println("pass: JNI_OnLoad without companion classes");
    }
}''', encoding='utf-8')
        cls.command([javac, '--release', '17', '-d', str(cls.native_only), str(no_companion)])

    @classmethod
    def command(cls, command, env=None):
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def environment(self):
        env = os.environ.copy()
        env.pop('TOTK_COMPANION', None)
        env.pop('TOTK_COMPANION_LOG', None)
        return env

    def test_runtime_switch_and_lifecycle(self):
        for setting in ('unset', '', '0', '1', 'false', '00'):
            with self.subTest(setting=setting):
                result = self.command([
                    self.java, '-Djava.library.path=' + str(self.directory), '-cp', str(self.classes),
                    'com.aure.totk.CompanionGateProbe', setting,
                ], env=self.environment())
                self.assertIn('pass: companion gate ' + setting, result.stdout)
                self.assertNotIn('RegisterNatives failed', result.stderr)

    def test_native_gate(self):
        # This driver is also suitable for TSan: no uninstrumented JVM GC/JIT
        # accesses share its address space with the production native bridge.
        executable = self.directory / 'native-gate'
        self.command([
            self.compiler, '-std=gnu11', '-D_GNU_SOURCE', '-DCOMPANION_GATE_NATIVE_TEST',
            '-Wall', '-Wextra', '-Werror', '-pthread',
            '-I' + str(self.include), '-I' + str(self.include / 'linux'),
            str(TOTK / 'runtime/companion_jni.c'), str(FIXTURES / 'jni_probe.c'),
            '-o', str(executable),
        ])
        for setting in ('unset', '', '0', '1', 'false', '00'):
            with self.subTest(setting=setting):
                result = self.command([str(executable), setting], env=self.environment())
                self.assertIn('pass: native companion gate ' + setting, result.stdout)

    def test_jni_load_without_companion_classes(self):
        result = self.command([
            self.java, '-Djava.library.path=' + str(self.directory), '-cp', str(self.native_only),
            'NativeOnlyProbe',
        ], env=self.environment())
        self.assertIn('pass: JNI_OnLoad without companion classes', result.stdout)
        self.assertIn('CompanionNative class not found', result.stderr)


if __name__ == '__main__':
    unittest.main()

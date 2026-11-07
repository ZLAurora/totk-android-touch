"""Bounded host coverage of production hint parsing/selection and the window-display query.
Framework display/JNI objects are doubles; this does not observe real panel cadence.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'
ACTIVITY = ROOT / 'games/totk/android/java/com/aure/totk/TotkActivity.java'


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


class AndroidFrameRateHint(unittest.TestCase):
    def command(self, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_native_policy_and_both_hint_targets(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('A native C compiler is required')
        perf = (RUNTIME / 'android_perf.c').read_text()
        frontend = (RUNTIME / 'frontend_android.c').read_text()
        source = r'''
#define _GNU_SOURCE
#include "games/totk/runtime/adpf_policy.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static int rate_on, fps60, framegen, lattice, auto40, hold_launch, hold_logged = -1;
static unsigned rate_request;
static _Atomic unsigned content_rate = 60, rate_override;
static _Atomic uint64_t generated_presents;
static uint64_t generated_seen, now = 1000000000;
static totk_rate_hold rate_hold;
typedef int totk_live_cache;
static totk_live_cache hold_live;
static int totk_live_cached(totk_live_cache *cache, const char *name, int launch) {
    (void)cache; (void)name; return launch;
}
static uint64_t mono_ns(void) { return now++; }
typedef struct { int unused; } ANativeWindow;
typedef struct { int unused; } ASurfaceControl;
typedef struct { int unused; } ASurfaceTransaction;
#define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE 1
static unsigned window_calls, surface_calls;
static float window_hz, surface_hz;
static int fake_window_rate(ANativeWindow *w, float hz, int8_t compatibility) {
    assert(w && compatibility == 1); window_calls++; window_hz = hz; return 0;
}
static void fake_surface_rate(ASurfaceTransaction *tx, ASurfaceControl *s, float hz, int8_t compatibility, int8_t strategy) {
    assert(tx && s && compatibility == 1 && strategy == 0); surface_calls++; surface_hz = hz;
}
static int (*window_rate)(ANativeWindow *, float, int8_t) = fake_window_rate;
static void (*surface_rate)(ASurfaceTransaction *, ASurfaceControl *, float, int8_t, int8_t) = fake_surface_rate;
'''
        source += section(perf, 'static int is_one(', 'static uint64_t mono_ns(')
        source += 'static void configure(void) {\n' + section(
            perf, '    rate_on = is_one("TOTK_FRAME_RATE_HINT")', '    span_applied = span_launch') + '}\n'
        source += section(perf, 'static void rate_frame(', 'void totk_perf_frame(')
        source += section(perf, 'unsigned totk_perf_rate_request(void)', '#endif')
        source += r'''
typedef void *jobject, *jclass, *jmethodID;
typedef unsigned char jboolean;
typedef int jint;
typedef const struct fake_jni *JNIEnv;
#define JNI_FALSE 0
typedef struct { JNIEnv *env; jobject clazz; } ANativeActivity;
struct fake_jni {
    jclass (*GetObjectClass)(JNIEnv *, jobject);
    jmethodID (*GetMethodID)(JNIEnv *, jclass, const char *, const char *);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
    jboolean (*ExceptionCheck)(JNIEnv *);
    void (*ExceptionClear)(JNIEnv *);
    jboolean (*CallBooleanMethod)(JNIEnv *, jobject, jmethodID, jint);
};
static int jni_failure, jni_exception, jni_queries, jni_clears;
static jclass object_class(JNIEnv *env, jobject obj) {
    (void)env; jni_queries++;
    if (jni_failure == 1) { jni_exception = 1; return NULL; }
    return obj;
}
static jmethodID method_id(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    (void)env; assert(!strcmp(name, "supportsFrameRateHint") && !strcmp(signature, "(I)Z"));
    if (jni_failure == 2) { jni_exception = 1; return NULL; }
    return cls;
}
static void delete_ref(JNIEnv *env, jobject obj) { (void)env; (void)obj; }
static jboolean exception_check(JNIEnv *env) { (void)env; return jni_exception; }
static void exception_clear(JNIEnv *env) { (void)env; jni_exception = 0; jni_clears++; }
static jboolean call_boolean(JNIEnv *env, jobject obj, jmethodID method, jint hz) {
    (void)env; assert(obj && method && hz == 120);
    if (jni_failure == 3) jni_exception = 1;
    return jni_failure != 4;
}
static const struct fake_jni jni = {
    object_class, method_id, delete_ref, exception_check, exception_clear, call_boolean
};
'''
        source += section(frontend, 'static jmethodID surface_parent_method(',
                          '/* Optional Java32 root hint listener')
        source += r'''
static void settings(const char *on, const char *hz) {
    if (on) assert(!setenv("TOTK_FRAME_RATE_HINT", on, 1)); else assert(!unsetenv("TOTK_FRAME_RATE_HINT"));
    if (hz) assert(!setenv("TOTK_FRAME_RATE_HINT_HZ", hz, 1)); else assert(!unsetenv("TOTK_FRAME_RATE_HINT_HZ"));
    configure(); totk_perf_rate_override(0);
}
int main(void) {
    unsigned parsed = 99;
    assert(totk_frame_rate_hint_parse(NULL, &parsed) && !parsed);
    assert(totk_frame_rate_hint_parse("", &parsed) && !parsed);
    assert(totk_frame_rate_hint_parse("0", &parsed) && !parsed);
    assert(totk_frame_rate_hint_parse("120", &parsed) && parsed == 120);
    assert(totk_frame_rate_hint_parse("00120", &parsed) && parsed == 120);
    assert(totk_frame_rate_hint_parse("2147483647", &parsed) && parsed == 2147483647u);
    const char *bad[] = {"120.0", "119.88", "+120", "-120", " 120", "120 ", "120Hz", "0x78", "nan", "inf", "2147483648", "9999999999999999999999999"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        parsed = 99; assert(!totk_frame_rate_hint_parse(bad[i], &parsed) && !parsed);
        settings("1", bad[i]); assert(!totk_perf_rate_request());
    }
    ANativeWindow window = {0}; ASurfaceTransaction tx = {0}; ASurfaceControl surface = {0};
    settings(NULL, "120");
    JNIEnv env = &jni; ANativeActivity activity = {&env, &window};
    frame_rate_hint_update(&activity); assert(!jni_queries); /* disabled means no JNI */
    assert(!totk_perf_rate_request()); totk_perf_rate_override(120); assert(!totk_perf_rate());
    totk_perf_window(&window); totk_perf_surface(&tx, &surface);
    assert(!window_calls && !surface_calls);
    settings("0", "120"); assert(!totk_perf_rate_request() && !totk_perf_rate());
    settings("true", "120"); assert(!totk_perf_rate_request() && !totk_perf_rate());
    const char *off[] = {NULL, "", "0"};
    for (size_t i = 0; i < sizeof off / sizeof *off; i++) {
        settings("1", off[i]); assert(!totk_perf_rate_request());
        for (int fg = 0; fg <= 1; fg++) for (int lat = 0; lat <= 1; lat++) {
            framegen = fg; lattice = lat;
            rate_frame(2, 33333333); assert(totk_perf_rate() == totk_frame_rate(30, fg, lat));
            rate_frame(1, 16666667); assert(totk_perf_rate() == 60);
        }
    }
    /* A device denying120 must keep auto40 at60, including its cutscene30 floor,
     * without changing the existing ADPF33.3ms work target. */
    assert(!setenv("TOTK_FPS60", "1", 1) && !setenv("TOTK_PRESENT_SLOTS", "auto", 1));
    assert(!setenv("TOTK_UNCAPPED", "1", 1) && !setenv("TOTK_UNCAPPED_HZ", "40", 1));
    settings("1", "120"); framegen = lattice = 0;
    jni_failure = 4; frame_rate_hint_update(&activity); jni_failure = 0;
    rate_frame(1, 33333333); assert(totk_perf_rate() == 60);
    rate_frame(2, 33333333); assert(totk_perf_rate() == 60);
    assert(totk_adpf_target(25000000, 1, 0) == 33333333);
    totk_perf_window(&window); totk_perf_surface(&tx, &surface);
    assert(window_calls == 1 && surface_calls == 1 && window_hz == 60 && surface_hz == 60);
    window_calls = surface_calls = 0;
    assert(totk_frame_rate_minimum(0, 30) == 30);
    assert(totk_frame_rate_minimum(0, 60) == 60);
    assert(totk_frame_rate_minimum(1, 30) == 60);
    assert(totk_frame_rate_minimum(1, 60) == 60);
    assert(totk_frame_rate_minimum(1, 120) == 120);
    for (int change = 0; change < 4; change++) {
        const char *key[] = {"TOTK_UNCAPPED", "TOTK_UNCAPPED_HZ", "TOTK_PRESENT_SLOTS", "TOTK_FPS60"};
        const char *before[] = {"1", "40", "auto", "1"};
        const char *after[] = {"0", "55", "0", "0"};
        assert(!setenv(key[change], after[change], 1));
        settings("1", "0"); framegen = lattice = 0;
        rate_frame(1, 33333333); assert(totk_perf_rate() == 30);
        assert(!setenv(key[change], before[change], 1));
    }
    assert(!unsetenv("TOTK_UNCAPPED") && !unsetenv("TOTK_UNCAPPED_HZ"));
    assert(!unsetenv("TOTK_PRESENT_SLOTS") && !unsetenv("TOTK_FPS60"));
    settings("1", "120"); framegen = lattice = 0;
    assert(totk_perf_rate_request() == 120);
    rate_frame(2, 33333333); assert(totk_perf_rate() == 30); /* not yet validated */
    totk_perf_rate_override(90); assert(totk_perf_rate() == 30); /* cannot authorize another rate */
    frame_rate_hint_update(&activity); assert(totk_perf_rate() == 120);
    for (int i = 0; i < 100; i++) {
        hold_launch = i % 2 ? 5000 : 0;
        rate_frame(i % 3 + 1, i % 2 ? 16666667 : 33333333);
        assert(totk_perf_rate() == 120);
    }
    totk_perf_window(&window); totk_perf_surface(&tx, &surface);
    assert(window_calls == 1 && surface_calls == 1 && window_hz == 120 && surface_hz == 120);
    for (jni_failure = 1; jni_failure <= 4; jni_failure++) {
        totk_perf_rate_override(120);
        frame_rate_hint_update(&activity);
        assert(totk_perf_rate() != 120 && !jni_exception);
    }
    assert(jni_clears == 3); /* object lookup, method lookup and Java query exceptions */
    jni_failure = 0;
    hold_launch = 0; rate_frame(2, 33333333); assert(totk_perf_rate() == 30);
    totk_perf_window(&window); totk_perf_surface(&tx, &surface);
    assert(window_calls == 2 && surface_calls == 2 && window_hz == 30 && surface_hz == 30);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='frame-rate-hint-') as directory:
            c = Path(directory) / 'hint.c'
            executable = Path(directory) / 'hint'
            c.write_text(source)
            flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                     for arg in shlex.split(os.environ.get(name, ''))]
            self.command([*compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                          '-I', str(ROOT), str(c), *flags, '-o', str(executable)])
            self.command([str(executable)])

    def test_current_window_display_matching(self):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            self.skipTest('A JDK 17+ is required')
        method = section(ACTIVITY.read_text(), '    public boolean supportsFrameRateHint(',
                         '    /** Native UI thread only, after its explicit parent transaction succeeded.')
        source = r'''
class Log { static int warnings; static void w(String tag, String text) { warnings++; } }
class Display {
    final int id; final Mode[] modes; boolean failed;
    Display(int id, float... hz) {
        this.id = id; modes = new Mode[hz.length];
        for (int i = 0; i < hz.length; i++) modes[i] = new Mode(hz[i]);
    }
    int getDisplayId() { return id; }
    Mode[] getSupportedModes() { if (failed) throw new IllegalStateException("query unavailable"); return modes; }
    static class Mode { final float hz; Mode(float hz) { this.hz = hz; } float getRefreshRate() { return hz; } }
}
class View { Display display; Display getDisplay() { return display; } }
class Window { final View decor = new View(); View getDecorView() { return decor; } }
public class HintProbe {
    final Window window = new Window(); Window getWindow() { return window; }
''' + method + r'''
    static void require(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) {
        HintProbe activity = new HintProbe();
        require(!activity.supportsFrameRateHint(120)); // no default-display fallback
        activity.window.decor.display = new Display(4, 60, 120.00001f);
        require(activity.supportsFrameRateHint(120));
        require(!activity.supportsFrameRateHint(90));
        require(!activity.supportsFrameRateHint(0));
        activity.window.decor.display = new Display(7, 60); // moved/recreated on another display
        require(!activity.supportsFrameRateHint(120));
        require(activity.supportsFrameRateHint(60));
        activity.window.decor.display = new Display(7, 119.88f, 120.02f, Float.NaN, Float.POSITIVE_INFINITY);
        require(!activity.supportsFrameRateHint(120));
        activity.window.decor.display = new Display(7);
        require(!activity.supportsFrameRateHint(120));
        activity.window.decor.display.failed = true;
        try { activity.supportsFrameRateHint(120); throw new AssertionError("exception must reach JNI refusal"); }
        catch (IllegalStateException expected) {}
        require(Log.warnings == 6);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix='frame-rate-display-') as directory:
            probe = Path(directory) / 'HintProbe.java'
            probe.write_text(source)
            self.command([javac, '--release', '17', '-d', directory, str(probe)])
            self.command([java, '-cp', directory, 'HintProbe'])


if __name__ == '__main__':
    unittest.main()

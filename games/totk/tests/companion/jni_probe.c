/* Host-JVM fixture for the production companion JNI bridge. No guest state is loaded. */
#include "../../runtime/companion.h"
#include "../../runtime/autoload.h"
#include "../../runtime/runtime.h"
#include "../../runtime/host_os.h"
#include <jni.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

static _Atomic int snapshots, reads, modules, presses, releases, requests;
static _Atomic int focus_calls, focus_flags;
#ifdef COMPANION_GATE_NATIVE_TEST
static _Atomic int allow_release;
#endif

int host_safe_read(uint64_t address, void *out, size_t size) {
    (void)address;
    atomic_fetch_add(&reads, 1);
    memset(out, 0, size);
    return 1;
}
uint8_t *runtime_module_base(const char *name) {
    (void)name;
    atomic_fetch_add(&modules, 1);
    return (uint8_t *)(uintptr_t)1;
}
size_t companion_snapshot(uintptr_t main_base, int detail, companion_read_fn read, CompanionBuf *out) {
    (void)main_base; (void)detail;
    atomic_fetch_add(&snapshots, 1);
    unsigned char byte;
    if (!read(1, &byte, sizeof byte)) return 0;
    out->data = strdup("{\"schema\":1,\"seq\":1,\"status\":\"ready\"}");
    if (!out->data) return 0;
    out->len = strlen(out->data); out->cap = out->len + 1;
    return out->len;
}
void runtime_input_key_synthetic(unsigned code, int down) {
    (void)code;
    if (down) atomic_fetch_add(&presses, 1);
    else {
#ifdef COMPANION_GATE_NATIVE_TEST
        /* Keep the pulse held until every concurrent action has returned,
         * independent of host scheduling or sanitizer startup cost. */
        while (!atomic_load(&allow_release)) sched_yield();
#endif
        atomic_fetch_add(&releases, 1);
    }
}
void runtime_android_present_transform_hint(uint64_t epoch, int hint) { (void)epoch; (void)hint; }
/* TotkActivity's text dialog and rumble/motion bridges (frontend_android.c, feedback_android.c): not part of this probe. */
void runtime_android_text_done(JNIEnv *env, jclass cls, jlong serial, jbyteArray utf8) { (void)env; (void)cls; (void)serial; (void)utf8; }
void runtime_android_game_focus(JNIEnv *env, jclass cls, jboolean focused, jboolean single_display) {
    (void)env; (void)cls;
    atomic_fetch_add(&focus_calls, 1);
    atomic_store(&focus_flags, (focused ? 1 : 0) | (single_display ? 2 : 0));
}
void runtime_android_game_input_focus(JNIEnv *env, jclass cls, jboolean top) { (void)env; (void)cls; (void)top; }
void runtime_android_motion(JNIEnv *env, jclass cls, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy, jfloat gz) {
    (void)env; (void)cls; (void)ax; (void)ay; (void)az; (void)gx; (void)gy; (void)gz;
}
void runtime_android_motion_live(JNIEnv *env, jclass cls, jboolean on, jstring source) { (void)env; (void)cls; (void)on; (void)source; }
jint runtime_android_feedback_enabled(JNIEnv *env, jclass cls) { (void)env; (void)cls; return 0; }
/* companion_actions.c and its runtime hooks: only counted, the fixture never runs a frame end */
_Atomic int runtime_companion_pending;
void (*_Atomic runtime_companion_tick)(uint64_t frame);
int runtime_fs_save_busy(void) { return 0; }
uint64_t runtime_now_ns(void) { return 0; }
void companion_actions_init(const CompanionEnv *env) { (void)env; }
void companion_tick(uintptr_t main_base, uint64_t frame) { (void)main_base; (void)frame; }
int companion_request(uintptr_t main_base, const char *action, const char *category, int slot, const char *actor,
                      int partition, long long pouch_gen) {
    (void)main_base; (void)action; (void)category; (void)slot; (void)actor; (void)partition; (void)pouch_gen;
    return atomic_fetch_add(&requests, 1) + 1;
}
size_t companion_result(int seq, CompanionBuf *out) {
    (void)seq;
    out->data = strdup("{\"seq\":1,\"state\":\"done\",\"reason\":\"\",\"status\":\"ready\"}");
    return out->data ? (out->len = strlen(out->data)) : 0;
}

JNIEXPORT void JNICALL Java_com_aure_totk_CompanionGateProbe_option(JNIEnv *env, jclass cls, jstring value) {
    (void)cls;
    if (!value) { unsetenv("TOTK_COMPANION"); return; }
    const char *text = (*env)->GetStringUTFChars(env, value, NULL);
    if (!text) return;
    setenv("TOTK_COMPANION", text, 1);
    (*env)->ReleaseStringUTFChars(env, value, text);
}
JNIEXPORT jint JNICALL Java_com_aure_totk_CompanionGateProbe_count(JNIEnv *env, jclass cls, jint kind) {
    (void)env; (void)cls;
    switch (kind) {
    case 0: return atomic_load(&snapshots);
    case 1: return atomic_load(&reads);
    case 2: return atomic_load(&modules);
    case 3: return atomic_load(&presses);
    case 4: return atomic_load(&releases);
    case 5: return atomic_load(&focus_calls);
    case 6: return atomic_load(&focus_flags);
    default: return -1;
    }
}

/* Exercise the real JNI entrypoints under native sanitizers without loading an
 * uninstrumented JVM (whose GC/JIT synchronization is not visible to TSan).
 * The ordinary host-JVM fixture above separately verifies class registration
 * and Java activity/poller ordering; this driver replaces only JNI plumbing. */
#ifdef COMPANION_GATE_NATIVE_TEST
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
extern jint JNI_OnLoad(JavaVM *, void *);
static jboolean (JNICALL *enabled_fn)(JNIEnv *, jclass);
static jstring (JNICALL *snapshot_fn)(JNIEnv *, jclass, jint);
static jboolean (JNICALL *action_fn)(JNIEnv *, jclass, jint, jint);
static jint (JNICALL *request_fn)(JNIEnv *, jclass, jstring, jstring, jint, jstring, jint, jlong);
static jstring (JNICALL *result_fn)(JNIEnv *, jclass, jint);
static JNIEnv fixture_env;
static jclass JNICALL find_class(JNIEnv *env, const char *name) {
    (void)env;
    if (!strcmp(name, "com/aure/totk/TotkActivity")) return NULL;   /* NativeActivity-only: the hint stays unbound */
    assert(!strcmp(name, "com/aure/totk/companion/CompanionNative"));
    return (jclass)(uintptr_t)1;
}
static void JNICALL exception_clear(JNIEnv *env) { (void)env; }
static jint JNICALL register_methods(JNIEnv *env, jclass cls, const JNINativeMethod *methods, jint count) {
    (void)env; (void)cls; assert(count == 5);
    for (jint i = 0; i < count; i++) {
        if (!strcmp(methods[i].name, "nativeEnabled")) enabled_fn = methods[i].fnPtr;
        else if (!strcmp(methods[i].name, "snapshot")) snapshot_fn = methods[i].fnPtr;
        else if (!strcmp(methods[i].name, "action")) action_fn = methods[i].fnPtr;
        else if (!strcmp(methods[i].name, "request")) request_fn = methods[i].fnPtr;
        else if (!strcmp(methods[i].name, "result")) result_fn = methods[i].fnPtr;
        else assert(0);
    }
    return 0;
}
static const char *JNICALL get_chars(JNIEnv *env, jstring s, jboolean *copy) { (void)env; (void)copy; return (const char *)s; }
static void JNICALL release_chars(JNIEnv *env, jstring s, const char *chars) { (void)env; (void)s; (void)chars; }
static jstring JNICALL new_string(JNIEnv *env, const char *text) {
    (void)env; assert(strstr(text, "\"status\":\"ready\"")); return (jstring)(uintptr_t)1;
}
static jint JNICALL get_env(JavaVM *vm, void **out, jint version) {
    (void)vm; assert(version == JNI_VERSION_1_6); *out = &fixture_env; return JNI_OK;
}
static void *press_action(void *unused) {
    (void)unused; assert(action_fn(&fixture_env, NULL, 1, 10)); return NULL;
}
int main(int argc, char **argv) {
    static const __typeof__(**(JNIEnv *)0) functions = {
        .FindClass = find_class, .RegisterNatives = register_methods, .NewStringUTF = new_string,
        .GetStringUTFChars = get_chars, .ReleaseStringUTFChars = release_chars, .ExceptionClear = exception_clear,
    };
    static const __typeof__(**(JavaVM *)0) invocation = {.GetEnv = get_env};
    fixture_env = &functions; JavaVM vm = &invocation;
    /* Registration must not cache the environment before NativeActivity loads it. */
    unsetenv("TOTK_COMPANION");
    assert(JNI_OnLoad(&vm, NULL) == JNI_VERSION_1_6);
    assert(enabled_fn && snapshot_fn && action_fn && request_fn && result_fn);
    assert(atomic_load(&runtime_companion_tick));   /* the frame-end hook is installed at load, nothing pending */
    assert(!atomic_load(&runtime_companion_pending));
    jstring action = (jstring)"equip", category = (jstring)"weapons", actor = (jstring)"Weapon_Sword_001";
    if (argc > 1 && strcmp(argv[1], "unset")) setenv("TOTK_COMPANION", argv[1], 1);
    int on = argc < 2 || strcmp(argv[1], "0");
    assert(enabled_fn(&fixture_env, NULL) == on);
    if (!on) {
        assert(!snapshot_fn(&fixture_env, NULL, COMPANION_FULL));
        assert(!action_fn(&fixture_env, NULL, 1, 10));
        assert(!atomic_load(&snapshots) && !atomic_load(&reads) && !atomic_load(&modules));
        assert(!atomic_load(&presses) && !atomic_load(&releases));
        /* off: request is "unsupported" and result unknown, without reaching the actions or the game */
        assert(request_fn(&fixture_env, NULL, action, category, 0, actor, 0, 1) == COMPANION_UNSUPPORTED);
        assert(!result_fn(&fixture_env, NULL, 1));
        assert(!atomic_load(&requests) && !atomic_load(&modules));
    } else {
        assert(snapshot_fn(&fixture_env, NULL, COMPANION_LIVE));
        assert(!action_fn(&fixture_env, NULL, 2, 10));
        assert(!action_fn(&fixture_env, NULL, 1, 16));
        assert(request_fn(&fixture_env, NULL, NULL, category, 0, actor, 0, 1) == COMPANION_INVALID);
        assert(request_fn(&fixture_env, NULL, action, category, 0, actor, 0, 1) == 1 && atomic_load(&requests) == 1);
        assert(result_fn(&fixture_env, NULL, 1));
        pthread_t threads[4];
        for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, press_action, NULL));
        for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
        assert(atomic_load(&presses) == 1); /* concurrent taps join one pulse */
        atomic_store(&allow_release, 1);
        struct timespec pause = {0, 1000000};
        for (unsigned i = 0; i < 2000 && !atomic_load(&releases); i++) nanosleep(&pause, NULL);
        assert(atomic_load(&releases) == 1);
    }
    printf("pass: native companion gate %s\n", argc > 1 ? argv[1] : "unset");
    return 0;
}
#endif

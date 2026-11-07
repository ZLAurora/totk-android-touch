/* JNI side of the dual-screen companion (Android app only): the Java companion
   (android/java/com/aure/totk/companion/CompanionNative.java) loads libtotk.so with System.loadLibrary, which runs
   JNI_OnLoad below; NativeActivity has already loaded the same library, so both share one runtime. Its drawings come
   from <game>/companion/, which the app's setup generates from the player's RomFS (Java AssetStore).
     boolean nativeEnabled()           -> TOTK_COMPANION != "0", queried after NativeActivity has read totk.env
     String snapshot(int detail)        -> companion_snapshot() JSON for the running game
     boolean action(int kind, int arg)  -> kind 1: press and release Switch button `arg` (libnx Npad bit index)
     int request(String action, String category, int slot, String actor, int partition, long pouchGen)
                                        -> companion_request(): an item action for the game's frame end (seq > 0, or
                                           -1 busy, -2 refused now, -3 unsupported / disabled, -4 invalid)
     String result(int seq)             -> companion_result() JSON, null for an unknown seq
   With TOTK_COMPANION=0 every entry point returns its "off" value without reading or calling anything in the game. */
#include "companion.h"
#include "autoload.h"
#include "host_os.h"
#include "runtime.h"
#include "world_survey.h"
#include "android_pause.h"
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
void runtime_android_present_transform_hint(uint64_t epoch, int hint);
static void JNICALL native_present_transform_hint(JNIEnv *env, jclass cls, jlong epoch, jint hint) {
    (void)env; (void)cls;
    runtime_android_present_transform_hint((uint64_t)epoch,hint);
}
void runtime_android_text_done(JNIEnv *env, jclass cls, jlong serial, jbyteArray utf8);   /* frontend_android.c: swkbd */
void runtime_android_game_focus(JNIEnv *env, jclass cls, jboolean focused, jboolean single_display);
void runtime_android_game_input_focus(JNIEnv *env, jclass cls, jboolean top);
/* feedback_android.c: the game's rumble and six-axis sensor */
void runtime_android_motion(JNIEnv *env, jclass cls, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy, jfloat gz);
void runtime_android_motion_live(JNIEnv *env, jclass cls, jboolean on, jstring source);
jint runtime_android_feedback_enabled(JNIEnv *env, jclass cls);

/* Do not cache this in JNI_OnLoad: NativeActivity's start_game reads totk.env
   synchronously during super.onCreate, before Java constructs its controller. */
static jboolean JNICALL native_enabled(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    const char *option = getenv("TOTK_COMPANION");
    return !option || strcmp(option, "0") ? JNI_TRUE : JNI_FALSE;
}

static int guest_read(uint64_t a, void *out, size_t n) { return host_safe_read(a, out, n); }

/* ---------- item actions: the host side of companion_actions.c ---------- */
/* Game code is native here: a guest function is called like a host one, on the frame-end thread only (the same way
   world_survey.c calls forceSetMatrix). */
static uint64_t game_call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))(uintptr_t)fn)(a0, a1, a2, a3, a4, a5);
}
static float game_call_f(uint64_t fn, uint64_t a0, float s0) { return ((float (*)(uint64_t, float))(uintptr_t)fn)(a0, s0); }
/* Only the battery fields the pause menu's own energy branch writes, on the thread that called the game for them. */
static int game_write(uint64_t a, const void *src, size_t n) {
    if (!a) return 0;
    memcpy((void *)(uintptr_t)a, src, n);
    return 1;
}
static uint64_t now_ms(void) { return runtime_now_ns() / 1000000u; }
static void set_pending(int on) { atomic_store_explicit(&runtime_companion_pending, on, memory_order_release); }
static void frame_end_tick(uint64_t frame) { companion_tick((uintptr_t)runtime_module_base("main"), frame); }
static void actions_init(void) {   /* JNI_OnLoad: before any snapshot or request */
    static const CompanionEnv env = {guest_read, game_call, game_call_f, game_write, runtime_fs_save_busy, now_ms, set_pending};
    companion_actions_init(&env);
    atomic_store_explicit(&runtime_companion_tick, frame_end_tick, memory_order_release);
}

static jint JNICALL native_request(JNIEnv *env, jclass cls, jstring action, jstring category, jint slot, jstring actor,
                                   jint partition, jlong pouch_gen) {
    if (!native_enabled(env, cls)) return COMPANION_UNSUPPORTED;
    if (runtime_android_game_paused()) return COMPANION_BUSY;
    if (!action || !category || !actor) return COMPANION_INVALID;
    const char *a = (*env)->GetStringUTFChars(env, action, NULL), *c = (*env)->GetStringUTFChars(env, category, NULL),
               *n = (*env)->GetStringUTFChars(env, actor, NULL);
    jint r = a && c && n ? companion_request((uintptr_t)runtime_module_base("main"), a, c, slot, n, partition, pouch_gen)
                         : COMPANION_INVALID;
    if (a) (*env)->ReleaseStringUTFChars(env, action, a);
    if (c) (*env)->ReleaseStringUTFChars(env, category, c);
    if (n) (*env)->ReleaseStringUTFChars(env, actor, n);
    return r;
}
static jstring JNICALL native_result(JNIEnv *env, jclass cls, jint seq) {
    if (!native_enabled(env, cls)) return NULL;
    CompanionBuf b = {0};
    jstring r = companion_result(seq, &b) ? (*env)->NewStringUTF(env, b.data) : NULL;
    free(b.data);
    return r;
}

/* TOTK_COMPANION_LOG=1: log every status change and, once a save is loaded, one full snapshot (for checking a new
   device or game build without the second screen). */
static void log_snapshot(const char *json, int detail) {
    static int on = -1; static char last[16]; static _Atomic int full_logged;
    if (on < 0) { const char *e = getenv("TOTK_COMPANION_LOG"); on = e && *e && strcmp(e, "0"); }
    if (!on) return;
    const char *s = strstr(json, "\"status\":\"");
    char status[16] = {0};
    if (s) { s += 10; size_t n = strcspn(s, "\""); if (n < sizeof status) memcpy(status, s, n); }
    if (strcmp(status, last)) { fprintf(stderr, "[companion] status %s\n", status); snprintf(last, sizeof last, "%s", status); }
    if (detail == COMPANION_FULL && !strcmp(status, "ready") && !atomic_exchange(&full_logged, 1))
        fprintf(stderr, "[companion] snapshot %s\n", json);
}

static jstring JNICALL native_snapshot(JNIEnv *env, jclass cls, jint detail) {
    if (!native_enabled(env, cls)) return NULL;
    CompanionBuf b = {0};
    jstring r = NULL;
    if (companion_snapshot((uintptr_t)runtime_module_base("main"), detail == COMPANION_FULL ? COMPANION_FULL : COMPANION_LIVE,
                           guest_read, &b)) {
        log_snapshot(b.data, detail);
        r = (*env)->NewStringUTF(env, b.data);   /* the JSON is ASCII: names are identifiers and other text is escaped */
    }
    free(b.data);
    return r;
}

/* The Mac key codes input.c maps to Switch buttons, by libnx Npad bit index (A, B, X, Y, LS, RS, L, R, ZL, ZR, Plus,
   Minus, Left, Up, Right, Down). The synthetic key is separate from the controller's own state (input.c ORs both), so a
   pulse never releases a button the player is holding. */
static const unsigned char BUTTON_KEY[16] = {38, 40, 34, 32, 6, 8, 12, 14, 58, 56, 36, 48, 3, 17, 4, 5};
static _Atomic int pulsing[16];   /* a tap while the same button's pulse runs joins it: one press, one release */
static void *release_later(void *arg) {
    unsigned bit = (unsigned)(uintptr_t)arg;
    struct timespec t = {0, 120 * 1000000L};   /* several game frames at 30 fps: the press is seen as held, then released */
    nanosleep(&t, NULL);
    runtime_input_key_synthetic(BUTTON_KEY[bit], 0);
    atomic_store(&pulsing[bit], 0);
    return NULL;
}
/* Only while a save is loaded (status "ready"): the shortcuts are buttons the player could press, so the game's own rules
   decide what they do in a menu, a cutscene or a loading screen. */
static jboolean JNICALL native_action(JNIEnv *env, jclass cls, jint kind, jint arg) {
    if (!native_enabled(env, cls) || kind != 1 || arg < 0 || arg >= 16) return JNI_FALSE;
    if (runtime_android_game_paused()) return JNI_FALSE;
    CompanionBuf b = {0};
    int ready = companion_snapshot((uintptr_t)runtime_module_base("main"), COMPANION_LIVE, guest_read, &b) &&
                strstr(b.data, "\"status\":\"ready\"") != NULL;
    free(b.data);
    if (!ready) return JNI_FALSE;
    int idle = 0;
    if (!atomic_compare_exchange_strong(&pulsing[arg], &idle, 1)) return JNI_TRUE;
    pthread_t t;
    runtime_input_key_synthetic(BUTTON_KEY[arg], 1);
    if (pthread_create(&t, NULL, release_later, (void *)(uintptr_t)arg)) {
        runtime_input_key_synthetic(BUTTON_KEY[arg], 0); atomic_store(&pulsing[arg], 0); return JNI_FALSE;
    }
    pthread_detach(t);
    fprintf(stderr, "[companion] button %d pressed from the second screen\n", (int)arg);
    return JNI_TRUE;
}

__attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    /* System.loadLibrary from CompanionNative supplies the app class loader; without it (a plain NativeActivity start)
       the class is not found and the library load still succeeds. */
    jclass cls = (*env)->FindClass(env, "com/aure/totk/companion/CompanionNative");
    if (!cls) { (*env)->ExceptionClear(env); fprintf(stderr, "[companion] CompanionNative class not found\n"); return JNI_VERSION_1_6; }
    static const JNINativeMethod methods[] = {
        {"nativeEnabled", "()Z", (void *)native_enabled},
        {"snapshot", "(I)Ljava/lang/String;", (void *)native_snapshot},
        {"action", "(II)Z", (void *)native_action},
        {"request", "(Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;IJ)I", (void *)native_request},
        {"result", "(I)Ljava/lang/String;", (void *)native_result},
    };
    if ((*env)->RegisterNatives(env, cls, methods, sizeof methods / sizeof *methods)) {
        (*env)->ExceptionClear(env); fprintf(stderr, "[companion] RegisterNatives failed\n");
    } else actions_init();
    cls = (*env)->FindClass(env, "com/aure/totk/TotkActivity");
    if (cls) {
        static const JNINativeMethod present_methods[] = {
            {"nativePresentTransformHint", "(JI)V", (void *)native_present_transform_hint},
            {"nativeTextInputDone", "(J[B)V", (void *)runtime_android_text_done},
            {"nativeGameFocus", "(ZZ)V", (void *)runtime_android_game_focus},
            {"nativeGameInputFocus", "(Z)V", (void *)runtime_android_game_input_focus},
            {"nativeMotion", "(FFFFFF)V", (void *)runtime_android_motion},
            {"nativeMotionLive", "(ZLjava/lang/String;)V", (void *)runtime_android_motion_live},
            {"nativeFeedbackEnabled", "()I", (void *)runtime_android_feedback_enabled},
        };
        if ((*env)->RegisterNatives(env,cls,present_methods,sizeof present_methods/sizeof *present_methods)) {
            (*env)->ExceptionClear(env); fprintf(stderr,"[present-prerotate] RegisterNatives failed\n");
        }
    } else (*env)->ExceptionClear(env); /* NativeActivity-only APK: optional Java hint unavailable. */
    return JNI_VERSION_1_6;
}

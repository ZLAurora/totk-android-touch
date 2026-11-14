/* Touch overlay input (touchmod).
   TouchOverlay.java reports held buttons and sticks (nativeSet),
   conventional aim stick (nativeAim), and direct camera angles
   (nativeDirectAim). Natives are registered by hand in companion_jni.c. */
#pragma once

#include <time.h>
#include <math.h>
#include <stdint.h>

static pthread_mutex_t g_touch_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_touch_buttons, g_touch_edges;
static int32_t g_touch_axes[4];

static double g_aim_sx, g_aim_sy, g_aim_dt;
static int32_t g_aim_last[2];

/* Direct camera-angle input is kept separate from the right stick. */
static double g_direct_yaw, g_direct_pitch;
static uint64_t g_direct_last_ns;

extern void runtime_aspect_set(float ratio);
extern int runtime_ui_anchor;

static uint64_t touch_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

__attribute__((visibility("default")))
void Java_com_aure_totk_touch_TouchNative_nativeSet(
        JNIEnv *env, jclass cls,
        jlong buttons, jint lx, jint ly, jint rx, jint ry) {
    (void)env;
    (void)cls;

    pthread_mutex_lock(&g_touch_mu);
    g_touch_edges |= (uint64_t)buttons & ~g_touch_buttons;
    g_touch_buttons = (uint64_t)buttons;
    g_touch_axes[0] = lx;
    g_touch_axes[1] = ly;
    g_touch_axes[2] = rx;
    g_touch_axes[3] = ry;
    pthread_mutex_unlock(&g_touch_mu);
}

/* Conventional virtual right-stick aim, accumulated per display frame. */
__attribute__((visibility("default")))
void Java_com_aure_totk_touch_TouchNative_nativeAim(
        JNIEnv *env, jclass cls, jfloat sx, jfloat sy, jfloat dt) {
    (void)env;
    (void)cls;

    pthread_mutex_lock(&g_touch_mu);
    g_aim_sx += (double)sx * dt;
    g_aim_sy += (double)sy * dt;
    g_aim_dt += dt;
    pthread_mutex_unlock(&g_touch_mu);
}

/* Direct camera aim: values are angle deltas in degrees, not stick values. */
__attribute__((visibility("default")))
void Java_com_aure_totk_touch_TouchNative_nativeDirectAim(
        JNIEnv *env, jclass cls, jfloat yaw, jfloat pitch) {
    (void)env;
    (void)cls;

    pthread_mutex_lock(&g_touch_mu);
    g_direct_yaw += (double)yaw;
    g_direct_pitch += (double)pitch;
    g_direct_last_ns = touch_monotonic_ns();
    pthread_mutex_unlock(&g_touch_mu);
}

/*
 * Called by the free-orbit camera hook.
 * Returns accumulated direct-angle deltas and clears them atomically
 * with respect to incoming touch updates.
 *
 * Input expires after 150 ms so a stale drag cannot keep rotating
 * the camera after the overlay stops sending updates.
 */
int runtime_touch_direct_consume(float *yaw, float *pitch) {
    int valid = 0;

    if (!yaw || !pitch) return 0;

    *yaw = 0.0f;
    *pitch = 0.0f;

    pthread_mutex_lock(&g_touch_mu);

    if (g_direct_last_ns != 0) {
        uint64_t now = touch_monotonic_ns();

        if (now >= g_direct_last_ns &&
            now - g_direct_last_ns <= 150000000ull) {
            *yaw = (float)g_direct_yaw;
            *pitch = (float)g_direct_pitch;
            valid = (g_direct_yaw != 0.0 || g_direct_pitch != 0.0);
        }
    }

    g_direct_yaw = 0.0;
    g_direct_pitch = 0.0;

    pthread_mutex_unlock(&g_touch_mu);
    return valid;
}

__attribute__((visibility("default")))
void Java_com_aure_totk_touch_TouchNative_nativeAspect(
        JNIEnv *env, jclass cls, jfloat ratio) {
    (void)env;
    (void)cls;
    runtime_aspect_set(ratio);
}

__attribute__((visibility("default")))
void Java_com_aure_totk_touch_TouchNative_nativeAnchor(
        JNIEnv *env, jclass cls, jint mode) {
    (void)env;
    (void)cls;
    runtime_ui_anchor = mode ? 1 : 0;
}

static int32_t touch_pick(int32_t a, int32_t b) {
    return (b < 0 ? -b : b) > (a < 0 ? -a : a) ? b : a;
}

static void touch_merge(RuntimeGamepadState *out) {
    pthread_mutex_lock(&g_touch_mu);

    if (g_aim_dt > 0) {
        g_aim_last[0] =
                (int32_t)(g_aim_sx / g_aim_dt * 32767.0);
        g_aim_last[1] =
                (int32_t)(g_aim_sy / g_aim_dt * 32767.0);

        g_aim_sx = 0;
        g_aim_sy = 0;
        g_aim_dt = 0;
    }

    out->buttons |= g_touch_buttons | g_touch_edges;
    g_touch_edges = 0;

    out->lx = touch_pick(out->lx, g_touch_axes[0]);
    out->ly = touch_pick(out->ly, g_touch_axes[1]);
    out->rx = touch_pick(
            touch_pick(out->rx, g_touch_axes[2]),
            g_aim_last[0]);
    out->ry = touch_pick(
            touch_pick(out->ry, g_touch_axes[3]),
            g_aim_last[1]);

    if (g_touch_buttons ||
        g_touch_axes[0] || g_touch_axes[1] ||
        g_touch_axes[2] || g_touch_axes[3] ||
        g_aim_last[0] || g_aim_last[1]) {
        out->connected = 1;
    }

    pthread_mutex_unlock(&g_touch_mu);
}

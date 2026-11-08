/* Touch overlay input (touchmod): TouchOverlay.java reports held buttons and sticks; runtime_gamepad_read merges them. */
#pragma once
static pthread_mutex_t g_touch_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_touch_buttons, g_touch_edges;
static int32_t g_touch_axes[4];

__attribute__((visibility("default")))
void Java_com_aure_totk_touch_TouchNative_nativeSet(JNIEnv *env, jclass cls, jlong buttons, jint lx, jint ly, jint rx, jint ry) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_touch_mu);
    g_touch_edges |= (uint64_t)buttons & ~g_touch_buttons;   /* a press shorter than one read still arrives */
    g_touch_buttons = (uint64_t)buttons;
    g_touch_axes[0] = lx; g_touch_axes[1] = ly; g_touch_axes[2] = rx; g_touch_axes[3] = ry;
    pthread_mutex_unlock(&g_touch_mu);
}
static int32_t touch_pick(int32_t a, int32_t b) { return (b < 0 ? -b : b) > (a < 0 ? -a : a) ? b : a; }
static void touch_merge(RuntimeGamepadState *out) {
    pthread_mutex_lock(&g_touch_mu);
    out->buttons |= g_touch_buttons | g_touch_edges; g_touch_edges = 0;
    out->lx = touch_pick(out->lx, g_touch_axes[0]); out->ly = touch_pick(out->ly, g_touch_axes[1]);
    out->rx = touch_pick(out->rx, g_touch_axes[2]); out->ry = touch_pick(out->ry, g_touch_axes[3]);
    if (g_touch_buttons || g_touch_axes[0] || g_touch_axes[1] || g_touch_axes[2] || g_touch_axes[3]) out->connected = 1;
    pthread_mutex_unlock(&g_touch_mu);
}

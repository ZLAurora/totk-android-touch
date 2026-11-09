#!/usr/bin/env python3
"""Applies the touch-mod patches to the upstream sources at build time."""
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def patch(path, old, new, many=False):
    p = ROOT / path
    s = p.read_text(encoding='utf-8')
    n = s.count(old)
    if n < 1 or (n != 1 and not many):
        sys.exit(f'PATCH FAILED in {path}: expected 1 match, found {n} for: {old[:80]!r}')
    p.write_text(s.replace(old, new), encoding='utf-8')
    print('patched', path)


BASE = 'games/totk/android/java/com/aure/totk/setup/'
SA = BASE + 'SetupActivity.java'
ST = BASE + 'Strings.java'
PX = 'android.util.TypedValue.COMPLEX_UNIT_PX'
UL = 'android.graphics.Paint.UNDERLINE_TEXT_FLAG'

# ---- setup screen: tappable "other picker" hint ----
patch(ST, '"or a whole folder (X)"', '"or another folder (press X or tap here)"', many=True)
patch(ST, '"or pick the three files (X)"', '"or pick the three files (press X or tap here)"', many=True)
patch(SA, 'card.setTag("pick:" + role);',
      'card.setTag("pick:" + role);\n'
      '            card.detail.setFocusable(false);\n'
      '            card.detail.setOnClickListener(v -> pick(role, multi(role) ? !primaryFolder(role) : primaryFolder(role)));')
patch(SA, 'Widgets.Card card = slotCards.get(role);',
      'Widgets.Card card = slotCards.get(role);\n'
      '            if (card != null) {\n'
      '                if (card.detail.getTag() == null) card.detail.setTag(Float.valueOf(card.detail.getTextSize()));\n'
      f'                card.detail.setTextSize({PX}, (Float) card.detail.getTag());\n'
      '                card.detail.setPadding(0, 0, 0, 0);\n'
      f'                card.detail.setPaintFlags(card.detail.getPaintFlags() & ~{UL});\n'
      '            }')
patch(SA, 'private void updateFiles() {',
      'private static void tapHint(Widgets.Card card) {\n'
      f'        card.detail.setPaintFlags(card.detail.getPaintFlags() | {UL});\n'
      '        float tapSize = card.detail.getTextSize() * 1.5f;\n'
      f'        card.detail.setTextSize({PX}, tapSize);\n'
      '        card.detail.setPadding(0, (int) (tapSize * 0.35f), 0, (int) (tapSize * 0.35f));\n'
      '    }\n'
      '    private void updateFiles() {')
patch(SA, 'multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);',
      'multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);\n'
      '                if (multi(role)) tapHint(card);')
HINT = '(primaryFolder(role) ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER)'
patch(SA, 'if (lostRoles(role)) { card.state(Strings.CANT_OPEN, Theme.WARN, picked); problems = true; }',
      f'if (lostRoles(role)) {{ card.state(Strings.CANT_OPEN, Theme.WARN, multi(role) ? {HINT} : picked); if (multi(role)) tapHint(card); problems = true; }}')
patch(SA, 'else if (problem != null) { card.state(problem, Theme.WARN, picked); problems = true; }',
      f'else if (problem != null) {{ card.state(problem, Theme.WARN, multi(role) ? {HINT} : picked); if (multi(role)) tapHint(card); problems = true; }}')

# ---- touch overlay ----
FE = 'games/totk/runtime/frontend_android.c'
TA = 'games/totk/android/java/com/aure/totk/TotkActivity.java'
CJ = 'games/totk/runtime/companion_jni.c'
touch_dir = ROOT / 'games/totk/android/java/com/aure/totk/touch'
touch_dir.mkdir(parents=True, exist_ok=True)
shutil.copy(ROOT / 'touchmod/java/TouchOverlay.java', touch_dir / 'TouchOverlay.java')
shutil.copy(ROOT / 'touchmod/native/touch_input_android.h', ROOT / 'games/totk/runtime/touch_input_android.h')
patch(FE, '#include "overlay_chord.h"', '#include "overlay_chord.h"\n#include "touch_input_android.h"')
patch(FE, 'out->connected = g_pad_seen;', 'out->connected = g_pad_seen;\n    touch_merge(out);')
patch(TA, 'private PresentationController companion;', 'private PresentationController companion;\n    private com.aure.totk.touch.TouchOverlay touch;')
patch(TA, 'super.onCreate(state);', 'super.onCreate(state);\n        touch = new com.aure.totk.touch.TouchOverlay(this);')
patch(TA, 'super.onResume();', 'super.onResume();\n        if (touch != null) touch.resume();')
patch(TA, '@Override protected void onPause() {', '@Override protected void onPause() {\n        if (touch != null) touch.pause();')
patch(TA, '@Override protected void onDestroy() {', '@Override protected void onDestroy() {\n        if (touch != null) { touch.close(); touch = null; }')

# natives registered by hand, like the project's own
patch(CJ, 'void runtime_android_present_transform_hint(uint64_t epoch, int hint);',
      'void runtime_android_present_transform_hint(uint64_t epoch, int hint);\n'
      'void Java_com_aure_totk_touch_TouchNative_nativeSet(JNIEnv *env, jclass cls, jlong buttons, jint lx, jint ly, jint rx, jint ry);\n'
      'void Java_com_aure_totk_touch_TouchNative_nativeAim(JNIEnv *env, jclass cls, jfloat sx, jfloat sy, jfloat dt);\n'
      'void Java_com_aure_totk_touch_TouchNative_nativeAspect(JNIEnv *env, jclass cls, jfloat ratio);\n'
      'void Java_com_aure_totk_touch_TouchNative_nativeAnchor(JNIEnv *env, jclass cls, jint mode);\n')
_old = '} else (*env)->ExceptionClear(env); /* NativeActivity-only APK: optional Java hint unavailable. */'
patch(CJ, _old, _old + '''
    {   /* touchmod: the overlay's natives, registered by hand like the others */
        jclass tc = (*env)->FindClass(env, "com/aure/totk/touch/TouchNative");
        if (tc) {
            static const JNINativeMethod touch_methods[] = {
                {"nativeSet", "(JIIII)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeSet},
                {"nativeAim", "(FFF)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeAim},
                {"nativeAspect", "(F)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeAspect},
                {"nativeAnchor", "(I)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeAnchor},
            };
            if ((*env)->RegisterNatives(env, tc, touch_methods, sizeof touch_methods / sizeof *touch_methods)) {
                (*env)->ExceptionClear(env); fprintf(stderr, "[touch] RegisterNatives failed\\n");
            }
        } else { (*env)->ExceptionClear(env); fprintf(stderr, "[touch] TouchNative class not found\\n"); }
    }''')

# ---- widescreen: live aspect ratio, always-on HUD correction, optional edge anchoring ----
patch('games/totk/runtime/aspect_patch.h', 'static inline uint32_t aspect_branch(uint64_t from, uint64_t to) {',
      '''int runtime_ui_anchor = 0;   /* touchmod: 0 = HUD scaled about the canvas centre, 1 = about the nearer screen edge */
void runtime_aspect_set(float ratio) {   /* touchmod: live ratio from the overlay; 0 or 16:9 = the game's own */
    if (!g_aspect_slot) return;
    if (!(ratio >= 0.5f && ratio <= 8.0f) || (ratio > 1.7770f && ratio < 1.7785f)) ratio = 0;
    uint32_t bits; memcpy(&bits, &ratio, 4);
    atomic_store(g_aspect_slot, bits);
}
static inline uint32_t aspect_branch(uint64_t from, uint64_t to) {''')
UA = 'games/totk/android/gfx/vk_ui_aspect.h'
patch(UA, '/* A guest viewport (x, y, w, h; any sign of h) scaled about the canvas centre. */',
      '''extern int runtime_ui_anchor;
/* touchmod: anchor for a horizontal span: the canvas centre (as before); with edge anchoring the nearer edge for spans in
   the outer thirds; < 0 = a span over 60% of the canvas width stays unscaled. */
static inline float vk_ui_anchor_x(const vk_ui_aspect *a, float lo, float hi) {
    float W = a->cx * 2;
    if (!runtime_ui_anchor || a->sx >= 1 || W <= 0) return a->cx;
    if (hi - lo > W * 0.6f) return -1;
    float mid = (lo + hi) * 0.5f;
    return mid < W / 3 ? 0 : mid > W * 2 / 3 ? W : a->cx;
}
/* A guest viewport (x, y, w, h; any sign of h) scaled about the canvas centre. */''')
patch(UA, '*x = a->cx + (*x - a->cx) * a->sx; *w *= a->sx;',
      '''float lo = *w < 0 ? *x + *w : *x, hi = *w < 0 ? *x : *x + *w, ax = vk_ui_anchor_x(a, lo, hi);
    if (ax >= 0) { *x = ax + (*x - ax) * a->sx; *w *= a->sx; }''')
patch(UA, '*x0 = (int32_t)floorf(a->cx + (*x0 - a->cx) * a->sx); *x1 = (int32_t)ceilf(a->cx + (*x1 - a->cx) * a->sx);',
      '''float ax = vk_ui_anchor_x(a, (float)*x0, (float)*x1);
    if (ax >= 0) { *x0 = (int32_t)floorf(ax + (*x0 - ax) * a->sx); *x1 = (int32_t)ceilf(ax + (*x1 - ax) * a->sx); }''')
patch('games/totk/android/gfx/vk_post.c', 'vk_ui_aspect_tracks(vk_ui_aspect_requested(getenv("TOTK_ASPECT"))));', '1);')

# ---- Build 3b: the overlay classes move into their own files ----
_dst = touch_dir / 'TouchOverlay.java'
_s = _dst.read_text(encoding='utf-8')
_a = _s.index('/** Touch aim: finger velocity'); _b = _s.index('/** Aim settings panel')
_s = _s[:_a] + _s[_b:]
_s = _s[:_s.index('/** Default layout (your preview layout)')]
_old = 'new P("backlog", "Flick backlog (ms, 0 = off)", 0f, 600f, 0f, 10f),'
if _s.count(_old) != 1:
    sys.exit('PATCH FAILED: backlog parameter line not found')
_s = _s.replace(_old, _old + '\n        new P("headroom", "Aim headroom (x)", 1f, 4f, 1f, 0.25f),'
                '\n        new P("btnOp", "Button opacity (%)", 10f, 100f, 50f, 5f),'
                '\n        new P("txtOp", "Text and icon opacity (%)", 10f, 100f, 100f, 5f),')
_dst.write_text(_s, encoding='utf-8')
for _n in ('TouchAim', 'TouchLayout', 'TouchView'):
    shutil.copy(ROOT / f'touchmod/java/{_n}.java', touch_dir / f'{_n}.java')
print('patched touch overlay (3b)')

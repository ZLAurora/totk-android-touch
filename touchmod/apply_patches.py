#!/usr/bin/env python3
"""Applies the touch-mod patches to the upstream sources at build time."""
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

# hint wording
patch(ST, '"or a whole folder (X)"', '"or another folder (press X or tap here)"', many=True)
patch(ST, '"or pick the three files (X)"', '"or pick the three files (press X or tap here)"', many=True)

# tapping the hint line = the X action (the other picker), also after a partial pick
patch(SA, 'card.setTag("pick:" + role);',
      'card.setTag("pick:" + role);\n'
      '            card.detail.setFocusable(false);\n'
      '            card.detail.setOnClickListener(v -> pick(role, multi(role) ? !primaryFolder(role) : primaryFolder(role)));')

# reset the line to its normal look on every refresh
patch(SA, 'Widgets.Card card = slotCards.get(role);',
      'Widgets.Card card = slotCards.get(role);\n'
      '            if (card != null) {\n'
      '                if (card.detail.getTag() == null) card.detail.setTag(Float.valueOf(card.detail.getTextSize()));\n'
      f'                card.detail.setTextSize({PX}, (Float) card.detail.getTag());\n'
      '                card.detail.setPadding(0, 0, 0, 0);\n'
      f'                card.detail.setPaintFlags(card.detail.getPaintFlags() & ~{UL});\n'
      '            }')

# helper: underlined, 50% bigger, padded
patch(SA, 'private void updateFiles() {',
      'private static void tapHint(Widgets.Card card) {\n'
      f'        card.detail.setPaintFlags(card.detail.getPaintFlags() | {UL});\n'
      '        float tapSize = card.detail.getTextSize() * 1.5f;\n'
      f'        card.detail.setTextSize({PX}, tapSize);\n'
      '        card.detail.setPadding(0, (int) (tapSize * 0.35f), 0, (int) (tapSize * 0.35f));\n'
      '    }\n'
      '    private void updateFiles() {')

# empty state
patch(SA, 'multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);',
      'multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);\n'
      '                if (multi(role)) tapHint(card);')

# error states keep the tappable hint
HINT = '(primaryFolder(role) ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER)'
patch(SA, 'if (lostRoles(role)) { card.state(Strings.CANT_OPEN, Theme.WARN, picked); problems = true; }',
      f'if (lostRoles(role)) {{ card.state(Strings.CANT_OPEN, Theme.WARN, multi(role) ? {HINT} : picked); if (multi(role)) tapHint(card); problems = true; }}')
patch(SA, 'else if (problem != null) { card.state(problem, Theme.WARN, picked); problems = true; }',
      f'else if (problem != null) {{ card.state(problem, Theme.WARN, multi(role) ? {HINT} : picked); if (multi(role)) tapHint(card); problems = true; }}')

if '--setup-only' in sys.argv: sys.exit(0)
# ---- Build 2a: touch overlay ----
import shutil
FE = 'games/totk/runtime/frontend_android.c'
TA = 'games/totk/android/java/com/aure/totk/TotkActivity.java'
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

# ---- overlay black-screen bisect ----
VARIANT = 'c'   # a = as before, b = overlay added 20 s late, c = software-drawn window with simpler flags
TOF = 'games/totk/android/java/com/aure/totk/touch/TouchOverlay.java'
if VARIANT == 'b':
    patch(TOF, 'private boolean closed;', 'private boolean closed, delayed;')
    patch(TOF, 'if (view != null || closed) return;',
          'if (view != null || closed) return;\n        if (!delayed) { delayed = true; ui.postDelayed(this::show, 20000); return; }')
if VARIANT == 'c':
    patch(TOF, 'WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,',
          'WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,')
    patch(TOF, 'setSystemUiVisibility(0x1706);', 'setLayerType(View.LAYER_TYPE_SOFTWARE, null);')
    patch(TOF, 'lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;', '')

# ---- touch hook: register natives by hand + on-screen diagnostic ----
CJ = 'games/totk/runtime/companion_jni.c'
_hdr = (ROOT / 'games/totk/runtime/touch_input_android.h').read_text(encoding='utf-8')
_protos = 'void Java_com_aure_totk_touch_TouchNative_nativeSet(JNIEnv *env, jclass cls, jlong buttons, jint lx, jint ly, jint rx, jint ry);\n'
_entries = '{"nativeSet", "(JIIII)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeSet},\n'
if 'nativeAspect' in _hdr:
    _protos += ('void Java_com_aure_totk_touch_TouchNative_nativeAspect(JNIEnv *env, jclass cls, jfloat ratio);\n'
                'void Java_com_aure_totk_touch_TouchNative_nativeAnchor(JNIEnv *env, jclass cls, jint mode);\n')
    _entries += ('{"nativeAspect", "(F)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeAspect},\n'
                 '{"nativeAnchor", "(I)V", (void *)Java_com_aure_totk_touch_TouchNative_nativeAnchor},\n')
patch(CJ, 'void runtime_android_present_transform_hint(uint64_t epoch, int hint);',
      'void runtime_android_present_transform_hint(uint64_t epoch, int hint);\n' + _protos)
_old = '} else (*env)->ExceptionClear(env); /* NativeActivity-only APK: optional Java hint unavailable. */'
patch(CJ, _old, _old + '''
    {   /* touchmod: the overlay's natives, registered by hand like the others */
        jclass tc = (*env)->FindClass(env, "com/aure/totk/touch/TouchNative");
        if (tc) {
            static const JNINativeMethod touch_methods[] = {
''' + _entries + '''            };
            if ((*env)->RegisterNatives(env, tc, touch_methods, sizeof touch_methods / sizeof *touch_methods)) {
                (*env)->ExceptionClear(env); fprintf(stderr, "[touch] RegisterNatives failed\\n");
            }
        } else { (*env)->ExceptionClear(env); fprintf(stderr, "[touch] TouchNative class not found\\n"); }
    }''')

TOF2 = 'games/totk/android/java/com/aure/totk/touch/TouchOverlay.java'
patch(TOF2, 'private boolean hidden, forceShow, controller, nativeOk = true;', 'private boolean hidden, forceShow, controller, nativeOk = true, sentOk;')
patch(TOF2, 'try { TouchNative.nativeSet(m, lx, ly, rx, ry); }', 'try { TouchNative.nativeSet(m, lx, ly, rx, ry); sentOk = true; }')
patch(TOF2, 'text(c, controller && !forceShow ? "Show" : hidden ? "Show" : "Hide", 57.5f * u, 5.56f * H / 100f, u, false, 0, false, false);',
      '''text(c, controller && !forceShow ? "Show" : hidden ? "Show" : "Hide", 57.5f * u, 5.56f * H / 100f, u, false, 0, false, false);
        p.setShader(null); p.setStyle(Paint.Style.FILL); p.setColor(0xAA000000); c.drawRect(0, H - 3.2f * u, 28 * u, H, p);
        tp.setShader(null); tp.setTypeface(Typeface.DEFAULT); tp.setTextSkewX(0); tp.setTextSize(1.8f * u); tp.setColor(0xFFFFFFFF);
        c.drawText("touch " + (nativeOk ? (sentOk ? "ok" : "idle") : "HOOK MISSING") + " mask=" + Long.toHexString(lastMask), .5f * u, H - 1f * u, tp);''')

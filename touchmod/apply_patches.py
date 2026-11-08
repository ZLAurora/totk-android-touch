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

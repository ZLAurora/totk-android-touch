#!/usr/bin/env python3
"""Applies the touch-mod patches to the upstream sources at build time."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def patch(path, old, new):
    p = ROOT / path
    s = p.read_text(encoding='utf-8')
    n = s.count(old)
    if n != 1:
        sys.exit(f'PATCH FAILED in {path}: expected 1 match, found {n} for: {old[:80]!r}')
    p.write_text(s.replace(old, new), encoding='utf-8')
    print('patched', path)


SA = 'games/totk/android/java/com/aure/totk/setup/SetupActivity.java'
UL = 'android.graphics.Paint.UNDERLINE_TEXT_FLAG'
PX = 'android.util.TypedValue.COMPLEX_UNIT_PX'

# 1. tapping the "or a whole folder / or pick the three files" line opens the other picker
patch(SA, 'card.setTag("pick:" + role);',
      'card.setTag("pick:" + role);\n'
      '            card.detail.setFocusable(false);\n'
      '            card.detail.setOnClickListener(v -> pick(role, multi(role) && inputs.get(role).isEmpty() ? !primaryFolder(role) : primaryFolder(role)));')

# 2. reset the line to its normal look on every refresh
patch(SA, 'Widgets.Card card = slotCards.get(role);',
      'Widgets.Card card = slotCards.get(role);\n'
      '            if (card != null) {\n'
      '                if (card.detail.getTag() == null) card.detail.setTag(Float.valueOf(card.detail.getTextSize()));\n'
      f'                card.detail.setTextSize({PX}, (Float) card.detail.getTag());\n'
      '                card.detail.setPadding(0, 0, 0, 0);\n'
      f'                card.detail.setPaintFlags(card.detail.getPaintFlags() & ~{UL});\n'
      '            }')

# 3. while nothing is picked: underlined, 50% bigger, with padding so it is easy to hit
patch(SA, 'multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);',
      'multi(role) ? (folder ? Strings.PICK_OR_FILES : Strings.PICK_OR_FOLDER) : null);\n'
      f'                card.detail.setPaintFlags(card.detail.getPaintFlags() | {UL});\n'
      '                float tapSize = card.detail.getTextSize() * 1.5f;\n'
      f'                card.detail.setTextSize({PX}, tapSize);\n'
      '                card.detail.setPadding(0, (int) (tapSize * 0.35f), 0, (int) (tapSize * 0.35f));')

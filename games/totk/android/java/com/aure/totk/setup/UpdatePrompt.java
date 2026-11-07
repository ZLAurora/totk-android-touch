package com.aure.totk.setup;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.system.Os;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.widget.LinearLayout;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Same typography, glass and focusable rows as the menu, without loading the setup native library. */
public final class UpdatePrompt {
    private UpdatePrompt() { }
    public static void show(Activity activity, String version, Consumer<Boolean> answer) {
        Theme.init(activity);
        Rect bounds = activity.getWindowManager().getCurrentWindowMetrics().getBounds();
        Theme.fit(bounds.width(), bounds.height());
        Dialog dialog = new Dialog(activity);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        Widgets.Panel box = new Widgets.Panel(activity, true, 44, 36);
        box.addView(Theme.text(activity, Strings.updateAvailable(version), true, 32, Theme.TEXT));
        box.addView(Theme.paragraph(activity, Strings.UPDATE_GAME_BODY, 22, Theme.TEXT2), Widgets.margins(Widgets.size(-1, -2), 0, 22, 0, 24));
        AtomicBoolean answered = new AtomicBoolean();
        Consumer<Boolean> finish = now -> {
            if (answered.compareAndSet(false, true)) { dialog.dismiss(); answer.accept(now); }
        };
        Widgets.Row update = new Widgets.Row(activity, Strings.UPDATE_NOW, Strings.UPDATE_CLOSE_HINT, 27, 18);
        update.setOnClickListener(v -> finish.accept(true));
        box.addView(update, Widgets.rowParams(94, 10));
        Widgets.Row later = new Widgets.Row(activity, Strings.UPDATE_NEXT_TIME, Strings.UPDATE_NEXT_HINT, 27, 18);
        later.setOnClickListener(v -> finish.accept(false));
        box.addView(later, Widgets.rowParams(94, 0));
        dialog.setContentView(box);
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnCancelListener(d -> finish.accept(false));
        boolean position = "position".equals(Os.getenv("TOTK_FACE_BUTTONS"));
        int a = position ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A;
        int b = position ? KeyEvent.KEYCODE_BUTTON_A : KeyEvent.KEYCODE_BUTTON_B;
        dialog.setOnKeyListener((d, code, event) -> {
            if (code != a && code != b) return false;
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) {
                if (code == b) finish.accept(false);
                else {
                    View focused = dialog.getCurrentFocus();
                    if (focused instanceof Widgets.Row) focused.performClick();
                    else update.performClick();
                }
            }
            return true;
        });
        dialog.getWindow().setBackgroundDrawable(new ColorDrawable(Theme.INK));
        dialog.show();
        dialog.getWindow().setLayout(Theme.u(1000), -2);
        update.post(() -> update.requestFocusFromTouch());
    }
}

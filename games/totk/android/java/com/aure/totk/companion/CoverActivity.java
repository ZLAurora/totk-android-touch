package com.aure.totk.companion;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import java.util.HashMap;

/** An opaque activity record occludes the launcher; the Presentation still owns every companion pixel and touch.
 * All registry access is on the main thread. Tokens are process-local: restored/orphaned tasks cannot stay alive. */
public final class CoverActivity extends Activity {
    static final String TOKEN = "com.aure.totk.companion.COVER_TOKEN";
    interface Listener {
        void attached();
        void finished(long token);
        void refused(long token);
        void topResumedChanged(long token, boolean top);
    }
    private static final class Launch {
        final int displayId;
        final Listener listener;
        CoverActivity activity;
        Launch(int displayId, Listener listener) { this.displayId = displayId; this.listener = listener; }
    }
    private static final HashMap<Long, Launch> launches = new HashMap<>();
    private long token;
    private String finishReason = "unexpected";
    private boolean topResumed;
    private boolean finishLogged;

    private static void logCover(String message) {
        if (!CompanionNative.logCover(message)) Log.i("TotkCompanion", message);
    }

    static void prepare(int displayId, long token, Listener listener) {
        launches.put(token, new Launch(displayId, listener));
    }
    static void cancel(long token) {
        Launch launch = launches.remove(token);
        if (launch == null) return;
        if (launch.activity != null) {
            launch.activity.finishReason = "session_end";
            launch.activity.finishCover();
        } else logCover("cover finish token=" + token + " reason=pending_cancel");
    }
    /** Activity/task focus authority, not window focus: this window is deliberately non-focusable. */
    static boolean isTopResumed(long token) {
        Launch launch = launches.get(token);
        return launch != null && launch.activity != null && launch.activity.topResumed
                && !launch.activity.isFinishing() && !launch.activity.isDestroyed();
    }

    @Override protected void onCreate(Bundle state) {
        // Before installing decor, so the cover can never acquire key/gamepad focus, even on its first frame.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
        super.onCreate(state);
        PresentationController.immersive(getWindow(), true);
        View black = new View(this);
        black.setBackgroundColor(Color.BLACK);
        setContentView(black);
        overridePendingTransition(0, 0);
        bind(state != null);
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        reportFinish("new_intent");
        release();
        setIntent(intent);
        bind(false);
        overridePendingTransition(0, 0);
    }

    private void bind(boolean restored) {
        token = getIntent().getLongExtra(TOKEN, 0);
        finishLogged = false; finishReason = "unexpected";
        Launch launch = launches.get(token);
        if (restored || launch == null || launch.activity != null) {
            finishReason = restored ? "restored" : "no_live_session";
            if (launch != null && launch.activity == null) launch.listener.refused(token);
            finishCover();
            return;
        }
        if (getDisplay() == null || getDisplay().getDisplayId() != launch.displayId) {
            Log.w("TotkCompanion", "cover refusal token=" + token + " reason=wrong_display");
            logCover("cover refusal token=" + token + " reason=wrong_display");
            launch.listener.refused(token);
            finishReason = "wrong_display";
            finishCover();
            return;
        }
        launch.activity = this;
        launch.listener.attached();
        if (topResumed) launch.listener.topResumedChanged(token, true);
    }

    @Override public void onTopResumedActivityChanged(boolean top) {
        super.onTopResumedActivityChanged(top);
        topResumed = top;
        Launch launch = launches.get(token);
        if (launch != null && launch.activity == this) launch.listener.topResumedChanged(token, top);
    }

    private void reportFinish(String reason) {
        if (finishLogged) return;
        finishLogged = true;
        logCover("cover finish token=" + token + " reason=" + reason);
    }
    private void finishCover() {
        // NativeActivity teardown can exit before onDestroy reaches this Activity; persist the finish request now.
        reportFinish(finishReason);
        finishAndRemoveTask(); overridePendingTransition(0, 0);
    }

    private void release() {
        Launch launch = launches.get(token);
        if (launch != null && launch.activity == this) {
            launches.remove(token);
            launch.listener.finished(token);
        }
    }
    @Override protected void onDestroy() {
        reportFinish(finishReason);
        release();
        super.onDestroy();
    }
}

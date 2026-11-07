package com.aure.totk.companion;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.ColorDrawable;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.hardware.display.DeviceProductInfo;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

/** Shared by the NativeActivity and the native-library-free preview, including hot-plug behavior. The companion is shown
 * only while the owner is started (a backgrounded game leaves the secondary display to the system), and only on a
 * built-in second panel (DisplayClass): an external monitor, a cast session or a developer overlay never gets it. */
public final class PresentationController implements DisplayManager.DisplayListener {
    private final Activity owner;
    private final DataSource source;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final DisplayManager displays;
    private Presentation shown;
    private int displayId = -1;
    private final int initialPage;
    private final Runnable changed;
    private boolean closed, stopped, resumed, topResumed, returnQueued, returnRequested, reported, reportQueued;
    private ActivityManager.AppTask inputTask;
    private long returnRequestedAt;
    private int rapidAutoReturns;
    private long autoReturnedAt = -1000;
    private boolean autoReturnBlocked, touchInProgress;
    private final Runnable autoReturnInput = () -> {
        if (!autoReturnBlocked && !touchInProgress && shown != null && shown.isShowing() && returnInputToOwner())
            autoReturnedAt = SystemClock.uptimeMillis();
    };
    private final Runnable returnInput = () -> { returnQueued = false; returnInputToOwner(); };
    private final Application.ActivityLifecycleCallbacks lifecycle = new Application.ActivityLifecycleCallbacks() {
        @Override public void onActivityStarted(Activity activity) { if (activity == owner && stopped) { stopped = false; refresh(); } }
        @Override public void onActivityStopped(Activity activity) { if (activity == owner) { stopped = true; dismiss(); } }
        @Override public void onActivityCreated(Activity activity, Bundle state) {}
        @Override public void onActivityResumed(Activity activity) {
            if (activity == owner) { resumed = true; resetAutoReturn(); }
        }
        @Override public void onActivityPaused(Activity activity) {
            if (activity == owner) {
                resumed = false; returnRequested = false;
                main.removeCallbacks(autoReturnInput);
            }
        }
        @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
        @Override public void onActivityDestroyed(Activity activity) {}
    };

    public PresentationController(Activity owner, DataSource source) { this(owner, source, 0, null); }
    /** changed runs on the main thread whenever showing() flips, so a preview owner can keep its own display free of
     * the companion while a secondary display shows it. */
    public PresentationController(Activity owner, DataSource source, int initialPage, Runnable changed) {
        this.owner = owner; this.source = source; this.initialPage = initialPage; this.changed = changed;
        displays = (DisplayManager) owner.getSystemService(Context.DISPLAY_SERVICE);
        owner.registerActivityLifecycleCallbacks(lifecycle);
        if (displays != null) { displays.registerDisplayListener(this, main); refresh(); }
    }
    /** Whether the companion is currently presented on a secondary display. */
    public boolean showing() { return shown != null; }
    /** Owners forward Activity.onTopResumedActivityChanged: window focus alone is per-display on multi-resume devices. */
    public void ownerTopResumed(boolean top) {
        topResumed = top;
        main.removeCallbacks(autoReturnInput);
        if (top) returnRequested = false;
        else {
            if (SystemClock.uptimeMillis() - autoReturnedAt < 1000) rapidAutoReturns++;
            else rapidAutoReturns = 0;
            if (rapidAutoReturns >= 3) autoReturnBlocked = true;
            if (!autoReturnBlocked && !touchInProgress) main.postDelayed(autoReturnInput, 300);
        }
    }
    private void resetAutoReturn() {
        main.removeCallbacks(autoReturnInput);
        rapidAutoReturns = 0; autoReturnedAt = -1000; autoReturnBlocked = false;
    }
    /** Coalesced on the main thread: a display change dismisses and recreates the Presentation in one pass, which must
     * not flash the companion onto the owner's display. Stop/start is not reported: the owner's own display is not
     * visible then, and swapping its content would flash on return. */
    private void report() {
        if (changed == null || reportQueued) return;
        reportQueued = true;
        main.post(() -> {
            reportQueued = false;
            boolean now = shown != null;
            if (closed || stopped || now == reported) return;
            reported = now;
            changed.run();
        });
    }
    public static void immersive(Window window, boolean secondary) {
        // Install the themed decor first: PhoneWindow otherwise restores theme animations over a zero value.
        View decor = window.getDecorView();
        window.setFormat(PixelFormat.OPAQUE);
        window.setBackgroundDrawable(new ColorDrawable(Color.BLACK));
        window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (secondary) window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL);
        window.setWindowAnimations(0);
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
    /** A non-focusable window on the companion display can still make it top-focused. Return our own task only while
     * its activity remains resumed and window-focused. Repeated competing focus requests stop automatic recovery. */
    private boolean returnInputToOwner() {
        if (closed || stopped || !resumed || topResumed
                || (returnRequested && SystemClock.uptimeMillis() - returnRequestedAt < 250)
                || !owner.hasWindowFocus() || owner.isFinishing() || owner.isDestroyed()) return false;
        try {
            if (inputTask == null) {
                ActivityManager manager = owner.getSystemService(ActivityManager.class);
                if (manager == null) return false;
                for (ActivityManager.AppTask task : manager.getAppTasks()) {
                    if (task.getTaskInfo().taskId == owner.getTaskId()) { inputTask = task; break; }
                }
            }
            if (inputTask == null || !owner.getComponentName().equals(inputTask.getTaskInfo().topActivity)) return false;
            returnRequested = true;
            returnRequestedAt = SystemClock.uptimeMillis();
            inputTask.moveToFront();
            return true;
        } catch (IllegalArgumentException | SecurityException unavailable) {
            returnRequested = false;
            Log.w("TotkCompanion", "Could not return controller focus to the owner task", unavailable);
            return false;
        }
    }
    private void dismiss() {
        Presentation previous = shown;
        shown = null; displayId = -1;
        touchInProgress = false;
        main.removeCallbacks(autoReturnInput);
        if (previous != null) { previous.setOnDismissListener(null); previous.dismiss(); }
        report();
    }
    /** These presentation displays as DisplayClass sees them (own = the caller's display, never a candidate). */
    private static DisplayClass.Screen[] screens(Display[] all, Display own) {
        DisplayClass.Screen[] screens = new DisplayClass.Screen[all.length];
        for (int i = 0; i < all.length; i++) {
            Display d = all[i];
            DeviceProductInfo info = d.getDeviceProductInfo();
            boolean usable = d.isValid() && (d.getFlags() & Display.FLAG_PRIVATE) == 0 && (own == null || d.getDisplayId() != own.getDisplayId());
            screens[i] = new DisplayClass.Screen(d.getDisplayId(), usable, info == null ? DisplayClass.NO_INFO : info.getConnectionToSinkType(),
                    info == null ? null : info.getManufacturerPnpId());
        }
        return screens;
    }
    private static String mainPnp(DisplayManager displays) {
        Display main = displays.getDisplay(Display.DEFAULT_DISPLAY);
        DeviceProductInfo info = main == null ? null : main.getDeviceProductInfo();
        return info == null ? null : info.getManufacturerPnpId();
    }
    /** DisplayClass.NONE, BUILT_IN or EXTERNAL for the displays present now (own = the caller's display). */
    public static int secondScreen(Context context, Display own) {
        DisplayManager displays = context.getSystemService(DisplayManager.class);
        if (displays == null) return DisplayClass.NONE;
        return DisplayClass.classify(screens(displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION), own), mainPnp(displays));
    }
    /** Device capability, independent of the menu/game's current display. A menu on the second panel must still
     * offer its art/settings and route play to the main panel. Presentation selection remains owner-relative. */
    public static int deviceSecondScreen(Context context) {
        DisplayManager displays = context.getSystemService(DisplayManager.class);
        return displays == null ? DisplayClass.NONE : secondScreen(context, displays.getDisplay(Display.DEFAULT_DISPLAY));
    }
    /** The built-in second panel the companion presents on, or null. */
    private static Display builtInDisplay(DisplayManager displays, Display own) {
        Display[] all = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        int id = DisplayClass.select(screens(all, own), mainPnp(displays));
        for (Display d : all) if (d.getDisplayId() == id) return d;
        return null;
    }
    private void refresh() {
        if (closed || stopped || displays == null || owner.isFinishing() || owner.isDestroyed()) return;
        Display selected = builtInDisplay(displays, owner.getDisplay());
        if (selected != null && selected.getDisplayId() == displayId && shown != null && shown.isShowing()) return;
        dismiss();
        if (selected == null) return;
        Presentation next = new Presentation(owner, selected) {
            private boolean touching;
            @Override public boolean dispatchTouchEvent(MotionEvent event) {
                int action = event.getActionMasked();
                if (action == MotionEvent.ACTION_DOWN) { touching = true; touchInProgress = true; resetAutoReturn(); }
                boolean handled = super.dispatchTouchEvent(event);
                if (action == MotionEvent.ACTION_CANCEL) { touching = false; touchInProgress = false; }
                if (touching && action == MotionEvent.ACTION_UP) {
                    touching = false; touchInProgress = false;
                    // Let Android deliver the top-resumed change caused by DOWN before checking it.
                    if (shown == this && isShowing() && !returnQueued) {
                        returnQueued = true;
                        main.post(returnInput);
                    }
                }
                return handled;
            }
            @Override protected void onCreate(Bundle state) {
                super.onCreate(state);
                immersive(getWindow(), true);
                CompanionView view = new CompanionView(getContext(), source);
                view.setPage(initialPage);
                setContentView(view);
            }
        };
        next.setOnDismissListener(dialog -> {
            if (shown == next) { shown = null; displayId = -1; report(); if (!closed) main.post(this::refresh); }
        });
        shown = next; displayId = selected.getDisplayId();
        try { next.show(); report(); }
        catch (WindowManager.InvalidDisplayException unavailable) {
            Log.w("TotkCompanion", "Presentation display disappeared", unavailable);
            dismiss();
        }
    }
    @Override public void onDisplayAdded(int id) { refresh(); }
    @Override public void onDisplayRemoved(int id) { if (id == displayId) dismiss(); refresh(); }
    @Override public void onDisplayChanged(int id) { if (id == displayId) dismiss(); refresh(); }
    public void close() {
        closed = true;
        main.removeCallbacksAndMessages(null);
        owner.unregisterActivityLifecycleCallbacks(lifecycle);
        if (displays != null) displays.unregisterDisplayListener(this);
        dismiss();
    }
}

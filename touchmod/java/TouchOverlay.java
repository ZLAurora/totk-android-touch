package com.aure.totk.touch;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.text.InputType;
import android.util.Log;
import android.util.SparseArray;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.util.Arrays;

/** Touch controls in a window of their own above the game (the game owns its activity window's surface). */
public final class TouchOverlay implements InputManager.InputDeviceListener {
    private static final String TAG = "TotkTouch";
    private final Activity activity;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final InputManager input;
    private TouchView view;
    private int tries;
    private boolean closed;

    public TouchOverlay(Activity activity) {
        this.activity = activity;
        input = activity.getSystemService(InputManager.class);
        input.registerInputDeviceListener(this, ui);
    }

    public void resume() {
        closed = false;
        show();
        TouchDisplay.applySaved(activity);
        if (view != null) view.setVisibility(View.VISIBLE);
    }

    public void pause() {
        if (view != null) {
            view.setVisibility(View.INVISIBLE);
            view.release();
        }
    }

    public void close() {
        closed = true;
        input.unregisterInputDeviceListener(this);
        if (view != null) {
            view.release();
            try {
                activity.getWindowManager().removeViewImmediate(view);
            } catch (RuntimeException ignored) {
            }
            view = null;
        }
    }

    private void show() {
        if (view != null || closed) return;
        View decor = activity.getWindow().getDecorView();
        if (decor.getWindowToken() == null) {
            if (tries++ < 50) ui.postDelayed(this::show, 100);
            return;
        }

        final TouchView v = new TouchView(activity);
        v.setController(controllerConnected());
        v.displayHook = () -> TouchDisplay.open(activity);
        v.aimHook = () -> TouchSettings.open(activity, v::reloadAim);

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                PixelFormat.TRANSLUCENT);

        lp.token = decor.getWindowToken();
        lp.setTitle("TotkTouch");

        try {
            activity.getWindowManager().addView(v, lp);
            view = v;
        } catch (RuntimeException failed) {
            Log.w(TAG, "overlay window failed", failed);
        }
    }

    private static boolean controllerConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual()
                    && (d.getSources() & InputDevice.SOURCE_GAMEPAD)
                    == InputDevice.SOURCE_GAMEPAD) return true;
        }
        return false;
    }

    private void refresh() {
        if (view != null) view.setController(controllerConnected());
    }

    @Override public void onInputDeviceAdded(int id) { refresh(); }
    @Override public void onInputDeviceRemoved(int id) { refresh(); }
    @Override public void onInputDeviceChanged(int id) { refresh(); }
}

/** JNI: runtime/touch_input_android.h (registered by hand in companion_jni.c). */
final class TouchNative {
    static {
        System.loadLibrary("totk");
    }

    static native void nativeSet(long buttons, int lx, int ly, int rx, int ry);
    static native void nativeAim(float sx, float sy, float dt);
    static native void nativeDirectAim(float yawDegrees, float pitchDegrees);
    static native void nativeAspect(float ratio);
    static native void nativeAnchor(int mode);
}

/** Aim settings panel: every value is saved on the device and applied at once. */
final class TouchSettings {
    private static final class P {
        final String key, label;
        final float min, max, def, step;

        P(String key, String label, float min, float max, float def, float step) {
            this.key = key;
            this.label = label;
            this.min = min;
            this.max = max;
            this.def = def;
            this.step = step;
        }
    }

    private static final P[] PARAMS = {
        new P("sensX", "Aim sensitivity X", 0.2f, 4f, 1f, 0.05f),
        new P("sensY", "Aim sensitivity Y", 0.2f, 4f, 1f, 0.05f),
        new P("curve", "Response curve (1 = linear)", 0.6f, 2f, 1.2f, 0.05f),
        new P("offset", "Dead-zone compensation", 0f, 0.4f, 0.1f, 0.01f),
        new P("smooth", "Smoothing (ms)", 0f, 60f, 15f, 1f),
        new P("tau", "Fling time constant (ms)", 20f, 400f, 90f, 5f),
        new P("cut", "Fling hard cut-off (ms)", 50f, 800f, 300f, 10f),
        new P("ramp", "Cut-off ramp to zero (ms)", 10f, 300f, 60f, 5f),
        new P("minfling", "Minimum fling speed", 200f, 3000f, 700f, 50f),
        new P("backlog", "Flick backlog (ms, 0 = off)", 0f, 600f, 0f, 10f),
        new P("headroom", "Aim headroom (x)", 1f, 4f, 1f, 0.25f),
        new P("btnOp", "Button opacity (%)", 10f, 100f, 50f, 5f),
        new P("txtOp", "Text and icon opacity (%)", 10f, 100f, 100f, 5f),
        new P("stickSensX", "Virtual right-stick sensitivity X", 0.2f, 4f, 1f, 0.05f),
        new P("stickSensY", "Virtual right-stick sensitivity Y", 0.2f, 4f, 1f, 0.05f),
    };

    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("touch", Context.MODE_PRIVATE);
    }

    private static String text(P p, float v) {
        return p.label + ": " + (Math.round(v * 100) / 100f)
                + "   (default " + p.def + ")";
    }

    private static CheckBox check(
            Context a, SharedPreferences sp, String key,
            String label, Runnable changed) {
        CheckBox c = new CheckBox(a);
        c.setText(label);
        c.setChecked(sp.getBoolean(key, false));
        c.setOnCheckedChangeListener((b, on) -> {
            sp.edit().putBoolean(key, on).apply();
            changed.run();
        });
        return c;
    }

    static void open(Activity a, Runnable changed) {
        SharedPreferences sp = prefs(a);
        int pad = (int) (12 * a.getResources().getDisplayMetrics().density);

        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(pad, pad, pad, pad);

        for (P p : PARAMS) {
            TextView label = new TextView(a);
            SeekBar bar = new SeekBar(a);

            bar.setMax(Math.round((p.max - p.min) / p.step));
            float cur = sp.getFloat(p.key, p.def);
            bar.setProgress(Math.round((cur - p.min) / p.step));
            label.setText(text(p, cur));

            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override
                public void onProgressChanged(SeekBar s, int v, boolean user) {
                    float val = p.min + v * p.step;
                    label.setText(text(p, val));
                    if (user) {
                        sp.edit().putFloat(p.key, val).apply();
                        changed.run();
                    }
                }

                @Override public void onStartTrackingTouch(SeekBar s) { }
                @Override public void onStopTrackingTouch(SeekBar s) { }
            });

            col.addView(label);
            col.addView(bar);
        }

        col.addView(check(a, sp, "invertY", "Invert vertical aim", changed));
        col.addView(check(a, sp, "joystick",
                "Right side: joystick instead of touch aim", changed));

        ScrollView sv = new ScrollView(a);
        sv.addView(col);

        new AlertDialog.Builder(a)
                .setTitle("Touch aim")
                .setView(sv)
                .setPositiveButton("Done", null)
                .setNeutralButton("Reset", (d, w) -> {
                    SharedPreferences.Editor e = sp.edit();
                    for (P p : PARAMS) e.remove(p.key);
                    e.remove("invertY");
                    e.remove("joystick");
                    e.apply();
                    changed.run();
                })
                .show();
    }
}

/** Display menu: aspect ratio (live, through the game's camera patch) and HUD anchoring. */
final class TouchDisplay {
    private static final String[] NAMES = {
        "16:9 (the game's own)", "18:9", "19.5:9", "20:9",
        "21:9", "16:10", "Fit my screen", "Custom..."
    };

    private static final float[] RATIOS = {
        0f, 2f, 19.5f / 9f, 20f / 9f,
        21f / 9f, 1.6f, -1f, -2f
    };

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("touch", Context.MODE_PRIVATE);
    }

    private static float ratio(Activity a) {
        float r = prefs(a).getFloat("aspect", 0f);
        if (r == -1f) {
            View d = a.getWindow().getDecorView();
            return d.getHeight() > 0 ? d.getWidth() / (float) d.getHeight() : 0f;
        }
        return r;
    }

    static void apply(Activity a) {
        try {
            TouchNative.nativeAspect(ratio(a));
            TouchNative.nativeAnchor(prefs(a).getInt("anchor", 0));
        } catch (LinkageError missing) {
        }
    }

    /** The game installs its camera patch while it starts, so the saved ratio is applied a few times. */
    static void applySaved(Activity a) {
        Handler h = new Handler(Looper.getMainLooper());
        for (int ms : new int[]{1500, 5000, 15000, 40000}) {
            h.postDelayed(() -> apply(a), ms);
        }
    }

    static void open(Activity a) {
        float cur = prefs(a).getFloat("aspect", 0f);
        int anchor = prefs(a).getInt("anchor", 0);
        String[] items = new String[NAMES.length + 1];

        for (int i = 0; i < NAMES.length; i++) {
            items[i] = (RATIOS[i] == cur ? "\u2713 " : "") + NAMES[i];
        }

        items[NAMES.length] = "HUD anchor: "
                + (anchor == 1 ? "screen edges (experimental)" : "centre");

        new AlertDialog.Builder(a).setTitle("Display").setItems(items, (d, which) -> {
            if (which == NAMES.length) {
                prefs(a).edit().putInt("anchor", 1 - anchor).apply();
                apply(a);
                open(a);
            } else if (RATIOS[which] == -2f) {
                custom(a);
            } else {
                prefs(a).edit().putFloat("aspect", RATIOS[which]).apply();
                apply(a);
            }
        }).show();
    }

    private static void custom(Activity a) {
        EditText t = new EditText(a);
        t.setHint("21:9, 2.1 or 3440x1440");
        t.setInputType(InputType.TYPE_CLASS_TEXT);

        new AlertDialog.Builder(a).setTitle("Custom ratio").setView(t)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Set", (d, w) -> {
                    float r = parse(t.getText().toString());
                    if (r > 0) {
                        prefs(a).edit().putFloat("aspect", r).apply();
                        apply(a);
                    }
                }).show();
    }

    static float parse(String s) {
        try {
            s = s.trim().toLowerCase().replace('x', ':');
            float r = s.contains(":")
                    ? Float.parseFloat(s.split(":")[0])
                            / Float.parseFloat(s.split(":")[1])
                    : Float.parseFloat(s);
            return r >= 0.5f && r <= 8f ? r : 0f;
        } catch (RuntimeException bad) {
            return 0f;
        }
    }
}

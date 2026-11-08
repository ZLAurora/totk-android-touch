package com.aure.totk.touch;

import android.app.Activity;
import android.content.Context;
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
import android.util.Log;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
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
    public void resume() { closed = false; show(); if (view != null) view.setVisibility(View.VISIBLE); }
    public void pause() { if (view != null) { view.setVisibility(View.INVISIBLE); view.release(); } }
    public void close() {
        closed = true;
        input.unregisterInputDeviceListener(this);
        if (view != null) {
            view.release();
            try { activity.getWindowManager().removeViewImmediate(view); } catch (RuntimeException ignored) { }
            view = null;
        }
    }
    private void show() {
        if (view != null || closed) return;
        View decor = activity.getWindow().getDecorView();
        if (decor.getWindowToken() == null) { if (tries++ < 50) ui.postDelayed(this::show, 100); return; }
        TouchView v = new TouchView(activity);
        v.setController(controllerConnected());
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                PixelFormat.TRANSLUCENT);
        lp.token = decor.getWindowToken();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        lp.setTitle("TotkTouch");
        try { activity.getWindowManager().addView(v, lp); view = v; }
        catch (RuntimeException failed) { Log.w(TAG, "overlay window failed", failed); }
    }
    private static boolean controllerConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && (d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) return true;
        }
        return false;
    }
    private void refresh() { if (view != null) view.setController(controllerConnected()); }
    @Override public void onInputDeviceAdded(int id) { refresh(); }
    @Override public void onInputDeviceRemoved(int id) { refresh(); }
    @Override public void onInputDeviceChanged(int id) { refresh(); }
}

/** JNI: runtime/touch_input_android.h. */
final class TouchNative {
    static { System.loadLibrary("totk"); }
    static native void nativeSet(long buttons, int lx, int ly, int rx, int ry);
}

/** Default layout (your preview layout): x, y = centre in % of the screen; w, h in % of the screen width. */
final class TouchLayout {
    static final int TOG = 1, HOV = 2, THRU_BTN = 4, THRU_CAM = 8, NOSKEW = 16;
    static final class Btn {
        final String label; final float x, y, w, h, rp; final int bit, flags, icon;
        Btn(String label, float x, float y, float w, float h, float rp, int bit, int flags, int icon) {
            this.label = label; this.x = x; this.y = y; this.w = w; this.h = h; this.rp = rp; this.bit = bit; this.flags = flags; this.icon = icon;
        }
    }
    private static Btn b(String l, double x, double y, double w, double h, double rp, int bit, int flags, int icon) {
        return new Btn(l, (float) x, (float) y, (float) w, (float) h, (float) rp, bit, flags, icon);
    }
    /* bit = libnx Npad bit: A0 B1 X2 Y3 L3 4 R3 5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; -1 = no button yet */
    static Btn[] defaults() {
        return new Btn[] {
            b("ZL", 8.1, 7.7, 16.2, 6.9, 80, 8, 0, 0), b("L", 6.25, 27.7, 12.5, 8.5, 40, 6, 0, 0),
            b("\u2191", 25, 5.55, 5, 3.5, 100, 13, NOSKEW, 0), b("\u2193", 25, 16.65, 5, 3.5, 100, 15, NOSKEW, 0),
            b("\u2190", 20, 11.1, 3.5, 5, 100, 12, NOSKEW, 0), b("\u2192", 30, 11.1, 3.5, 5, 100, 14, NOSKEW, 0),
            b("Camera", 35, 11.1, 3.2, 4.4, 100, -1, 0, 0xFFA8D020),
            b("\u2013", 67.5, 5.55, 6, 5, 100, 11, 0, 0), b("+", 75, 5.55, 6, 5, 100, 10, 0, 0),
            b("Autobuild", 77.5, 22.2, 3.2, 4.4, 100, -1, 0, 0xFFA78BFA),
            b("Ascend", 2.5, 44.4, 2.8, 4.2, 100, -1, 0, 0xFF8FC0A8), b("Recall", 12.5, 44.4, 2.8, 4.2, 100, -1, 0, 0xFFF0A030),
            b("Ultrahand", 7.5, 49.95, 2.8, 4.3, 100, -1, 0, 0xFFFF5A3D), b("Fuse", 2.5, 55.5, 2.8, 4.2, 100, -1, 0, 0xFF38D6D0),
            b("L3", 45, 88.8, 5.5, 5.5, 100, 4, 0, 0), b("R3", 55, 88.8, 5.5, 5.5, 100, 5, 0, 0),
            b("R", 93.75, 27.7, 12.5, 8.5, 40, 7, 0, 0), b("ZR", 91.9, 7.7, 16.2, 6.9, 80, 9, 0, 0),
            b("Surf", 65, 88.8, 8.5, 5.3, 45, -1, 0, 0),
            b("A", 95, 66.668, 7.5, 7.5, 100, 0, 0, 0), b("B", 92.5, 83.334, 7.5, 7.5, 100, 1, TOG | THRU_BTN, 0),
            b("X", 87.5, 66.668, 7.5, 7.5, 100, 2, HOV | THRU_BTN | THRU_CAM, 0), b("Y", 85, 83.334, 7.5, 7.5, 100, 3, 0, 0),
            b("Gyro", 96.5, 44.4, 7, 3.5, 100, -1, TOG, 0), b("No gyro", 25, 27.778, 8, 3.5, 100, -1, HOV, 0),
        };
    }
}

final class TouchView extends View {
    private static final String TAG = "TotkTouch";
    private static final int PRESS_FILL = 0xFF065E25, PRESS_LINE = 0xFF89E8A9, BLACK = 0xB8000000;
    private static final float STICK_R = 6.5f;   // in % of the screen width
    private final TouchLayout.Btn[] bs = TouchLayout.defaults();
    private final boolean[] toggled = new boolean[bs.length];
    private final SparseArray<Ptr> ptrs = new SparseArray<>();
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG), tp = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF(), rr = new RectF();
    private final Typeface face = Typeface.create(Typeface.SANS_SERIF, 250, false);
    private final Vibrator vib;
    private boolean hidden, forceShow, controller, nativeOk = true;
    private long lastMask = -1;
    private final int[] lastAx = {1, 1, 1, 1};

    private static final class Ptr {
        final boolean[] own, hov; int stick = -1; float cx, cy, px, py;
        Ptr(int n) { own = new boolean[n]; hov = new boolean[n]; }
    }

    TouchView(Context context) {
        super(context);
        VibratorManager vm = context.getSystemService(VibratorManager.class);
        vib = vm == null ? null : vm.getDefaultVibrator();
        setSystemUiVisibility(0x1706);
    }
    void setController(boolean c) { controller = c; publish(); invalidate(); }
    private boolean showButtons() { return !hidden && (!controller || forceShow); }
    void release() { ptrs.clear(); lastMask = -1; send(0, 0, 0, 0, 0); invalidate(); }

    @Override protected void onLayout(boolean changed, int l, int t, int rt, int b) {
        super.onLayout(changed, l, t, rt, b);
        int w = rt - l, h = b - t, m = (int) (w * 0.06f);   // keep the system back swipe off the screen edges
        setSystemGestureExclusionRects(Arrays.asList(new Rect(0, 0, m, h), new Rect(w - m, 0, w, h)));
    }

    // ---- touch ----
    private boolean contains(int i, float x, float y) {
        float u = getWidth() / 100f, cx = bs[i].x * u, cy = bs[i].y * getHeight() / 100f;
        return Math.abs(x - cx) <= bs[i].w * u / 2 && Math.abs(y - cy) <= bs[i].h * u / 2;
    }
    private boolean inHide(float x, float y) {
        float u = getWidth() / 100f;
        return Math.abs(x - 57.5f * u) <= 2.45f * u && Math.abs(y - 5.56f * getHeight() / 100f) <= 1.75f * u;
    }
    private boolean pressed(int i) {
        if ((bs[i].flags & TouchLayout.TOG) != 0) return toggled[i];
        for (int k = 0; k < ptrs.size(); k++) { Ptr q = ptrs.valueAt(k); if (q.own[i] || q.hov[i]) return true; }
        return false;
    }
    private void tick() { if (vib != null) vib.vibrate(VibrationEffect.createOneShot(12, 120)); }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int act = e.getActionMasked(), idx = e.getActionIndex();
        switch (act) {
            case MotionEvent.ACTION_DOWN: requestUnbufferedDispatch(e); down(e.getPointerId(0), e.getX(0), e.getY(0)); break;
            case MotionEvent.ACTION_POINTER_DOWN: down(e.getPointerId(idx), e.getX(idx), e.getY(idx)); break;
            case MotionEvent.ACTION_MOVE: for (int k = 0; k < e.getPointerCount(); k++) move(e.getPointerId(k), e.getX(k), e.getY(k)); break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: ptrs.remove(e.getPointerId(idx)); break;
            case MotionEvent.ACTION_CANCEL: ptrs.clear(); break;
            default: break;
        }
        publish(); invalidate();
        return true;
    }
    private void down(int id, float x, float y) {
        if (inHide(x, y)) { if (controller && !forceShow) forceShow = true; else hidden = !hidden; tick(); return; }
        if (!showButtons()) return;
        Ptr q = new Ptr(bs.length); ptrs.put(id, q);
        int top = -1;
        for (int i = bs.length - 1; i >= 0; i--) {   // topmost first; a button passes the touch on only with THRU_BTN
            if (!contains(i, x, y)) continue;
            if (top < 0) top = i;
            q.own[i] = true;
            if ((bs[i].flags & TouchLayout.TOG) != 0) toggled[i] = !toggled[i];
            if ((bs[i].flags & TouchLayout.THRU_BTN) == 0) break;
        }
        if (top >= 0) tick();
        if (top < 0 || (bs[top].flags & TouchLayout.THRU_CAM) != 0) {
            q.stick = x < getWidth() / 2f ? 0 : 1; q.cx = x; q.cy = y; q.px = x; q.py = y;
        }
    }
    private void move(int id, float x, float y) {
        Ptr q = ptrs.get(id);
        if (q == null) return;
        for (int i = 0; i < bs.length; i++) {
            if ((bs[i].flags & TouchLayout.HOV) == 0 || q.own[i]) continue;
            boolean in = contains(i, x, y);
            if (in && !q.hov[i]) { q.hov[i] = true; if ((bs[i].flags & TouchLayout.TOG) != 0) toggled[i] = !toggled[i]; tick(); }
            else if (!in) q.hov[i] = false;
        }
        q.px = x; q.py = y;
    }

    // ---- output ----
    private void publish() {
        long mask = 0; int[] ax = new int[4];
        if (showButtons()) {
            for (int i = 0; i < bs.length; i++) if (bs[i].bit >= 0 && pressed(i)) mask |= 1L << bs[i].bit;
            float rad = STICK_R * getWidth() / 100f;
            for (int k = 0; k < ptrs.size(); k++) {
                Ptr q = ptrs.valueAt(k);
                if (q.stick < 0) continue;
                float dx = q.px - q.cx, dy = q.py - q.cy, len = (float) Math.hypot(dx, dy), m = Math.min(1f, len / rad);
                float v = m <= 0.12f ? 0 : (m - 0.12f) / 0.88f;
                ax[q.stick * 2] = len > 0 ? (int) (dx / len * v * 32767) : 0;
                ax[q.stick * 2 + 1] = len > 0 ? (int) (-dy / len * v * 32767) : 0;   // Android: down is +y; the Switch: up is +
            }
        }
        if (mask == lastMask && Arrays.equals(ax, lastAx)) return;
        lastMask = mask; System.arraycopy(ax, 0, lastAx, 0, 4);
        send(mask, ax[0], ax[1], ax[2], ax[3]);
    }
    private void send(long m, int lx, int ly, int rx, int ry) {
        if (!nativeOk) return;
        try { TouchNative.nativeSet(m, lx, ly, rx, ry); }
        catch (LinkageError missing) { nativeOk = false; Log.e(TAG, "touch hook missing in libtotk.so", missing); }
    }

    // ---- drawing ----
    @Override protected void onDraw(Canvas c) {
        float W = getWidth(), H = getHeight(), u = W / 100f;
        if (W <= 0) return;
        if (showButtons()) {
            for (int i = 0; i < bs.length; i++) button(c, i, u, W, H);
            for (int k = 0; k < ptrs.size(); k++) { Ptr q = ptrs.valueAt(k); if (q.stick >= 0) stick(c, q, u); }
        }
        box(c, 57.5f * u, 5.56f * H / 100f, 2.45f * u, 1.75f * u, 1.75f * u, u, hidden, false, false);
        text(c, controller && !forceShow ? "Show" : hidden ? "Show" : "Hide", 57.5f * u, 5.56f * H / 100f, u, false, 0, false, false);
    }
    private void box(Canvas c, float cx, float cy, float hw, float hh, float rad, float u, boolean on, boolean hov, boolean thru) {
        float g = .144f * u, k = .28f * u;
        r.set(cx - hw, cy - hh, cx + hw, cy + hh);
        p.setShader(null);
        if (hov) {   // hover buttons: the gap is filled and there is no outer ring
            rr.set(r.left - g, r.top - g, r.right + g, r.bottom + g);
            p.setStyle(Paint.Style.FILL); p.setColor(BLACK); c.drawRoundRect(rr, rad + g, rad + g, p);
        } else {
            float e = g + k / 2;
            rr.set(r.left - e, r.top - e, r.right + e, r.bottom + e);
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(k); p.setColor(BLACK); c.drawRoundRect(rr, rad + e, rad + e, p);
        }
        p.setStyle(Paint.Style.FILL);
        if (on) p.setColor(PRESS_FILL);
        else if (thru) p.setShader(new RadialGradient(cx, cy, Math.max(hw, hh), new int[]{0x00000000, 0x00000000, BLACK},
                new float[]{0f, .28f, .92f}, Shader.TileMode.CLAMP));
        else p.setColor(BLACK);
        c.drawRoundRect(r, rad, rad, p);
        p.setShader(null);
        if (!on) {
            p.setShader(new LinearGradient(0, r.top, 0, r.top + hh, 0x14FFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP));
            c.drawRoundRect(r, rad, rad, p); p.setShader(null);
        } else {
            float ow = .25f * u;
            rr.set(r.left - ow / 2, r.top - ow / 2, r.right + ow / 2, r.bottom + ow / 2);
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(ow); p.setColor(PRESS_LINE);
            c.drawRoundRect(rr, rad + ow / 2, rad + ow / 2, p);
            p.setStyle(Paint.Style.FILL);
        }
    }
    private void button(Canvas c, int i, float u, float W, float H) {
        TouchLayout.Btn b = bs[i];
        float cx = b.x * W / 100f, cy = b.y * H / 100f, hw = b.w * u / 2, hh = b.h * u / 2, rad = Math.min(b.w, b.h) * u * b.rp / 200f;
        boolean on = pressed(i), hov = (b.flags & TouchLayout.HOV) != 0, thru = (b.flags & (TouchLayout.THRU_BTN | TouchLayout.THRU_CAM)) != 0;
        box(c, cx, cy, hw, hh, rad, u, on, hov, thru);
        text(c, b.label, cx, cy, u, (b.flags & TouchLayout.NOSKEW) != 0, b.icon, (b.flags & TouchLayout.TOG) != 0, toggled[i]);
    }
    private void text(Canvas c, String s, float cx, float cy, float u, boolean noSkew, int icon, boolean tog, boolean on) {
        tp.setTypeface(face); tp.setTextSize(1.5f * u * 1.3f); tp.setTextSkewX(noSkew ? 0 : -0.158f);
        float tw = tp.measureText(s), x = cx - tw / 2;
        if (icon != 0) {   // placeholder: a coloured dot in the button, the label to its right
            p.setShader(null); p.setStyle(Paint.Style.FILL); p.setColor(icon); c.drawCircle(cx, cy, 1.6f * u, p);
            x = cx + 2.0f * u;
        } else if (tog) {
            float dw = 1.6f * u, start = cx - (tw + dw) / 2;
            p.setShader(null); p.setStyle(on ? Paint.Style.FILL : Paint.Style.STROKE); p.setStrokeWidth(.2f * u); p.setColor(0xFFFFFFFF);
            c.drawCircle(start + .5f * u, cy, .5f * u, p);
            x = start + dw;
        }
        float base = cy - (tp.ascent() + tp.descent()) / 2;
        tp.setShader(new LinearGradient(0, base + tp.ascent(), 0, base + tp.descent(), 0xFFFFFFFF, 0xFFCCCCCC, Shader.TileMode.CLAMP));   // 100% to 80% brightness
        c.drawText(s, x, base, tp);
        tp.setShader(null);
    }
    private void stick(Canvas c, Ptr q, float u) {
        float R = STICK_R * u, dx = q.px - q.cx, dy = q.py - q.cy, len = (float) Math.hypot(dx, dy), k = len > R ? R / len : 1f;
        p.setShader(null); p.setStyle(Paint.Style.FILL); p.setColor(0x55000000); c.drawCircle(q.cx, q.cy, R, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(.4f * u); p.setColor(0xA6FFFFFF); c.drawCircle(q.cx, q.cy, R, p);
        float kx = q.cx + dx * k, ky = q.cy + dy * k, kr = R * 0.42f;
        p.setStyle(Paint.Style.FILL);
        p.setShader(new RadialGradient(kx, ky, kr, 0xB3FFFFFF, 0xFFFFFFFF, Shader.TileMode.CLAMP));   // centre 0.7x the edge opacity
        c.drawCircle(kx, ky, kr, p);
        p.setShader(null);
    }
}

package com.aure.totk.touch;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.CornerPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.util.SparseArray;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import java.util.Arrays;

final class TouchView extends View {
    private static final String TAG = "TotkTouch";
    private static final int PRESS_FILL = 0xFF065E25, PRESS_LINE = 0xFF89E8A9;
    private static final float STICK_R = 6.5f, THRU_INNER = 0.4f, SKEW = -0.158f;
    private static final float[] SYS_X = {42.5f, 50f, 57.5f};
    private final TouchLayout.Btn[] bs = TouchLayout.defaults();
    private final TouchLayout.Btn sys = new TouchLayout.Btn("", 0, 0, 4.9f, 3.5f, 100, -1, 0, 0);
    private final boolean[] toggled = new boolean[bs.length];
    private final float[] anim = new float[bs.length];
    private final SparseArray<Ptr> ptrs = new SparseArray<>();
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG), tp = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final Matrix mx = new Matrix();
    private final RectF rs = new RectF();
    private final float[] hp = new float[96];
    private final Vibrator vib;
    private final float density;
    private boolean hidden, forceShow, controller, nativeOk = true, joystick, looping, animating;
    private long lastMask = -1, lastFrame, animLast;
    private final int[] lastAx = {1, 1, 1, 1};
    private float btnA = 0.5f, txtA = 1f;
    final TouchAim aim = new TouchAim();
    Runnable displayHook, aimHook;
    private final float[] aimOut = new float[2];

    private static final class Ptr {
        final boolean[] own, hov; int stick = -1; boolean aim; float cx, cy, px, py;
        Ptr(int n) { own = new boolean[n]; hov = new boolean[n]; }
    }

    TouchView(Context context) {
        super(context);
        VibratorManager vm = context.getSystemService(VibratorManager.class);
        vib = vm == null ? null : vm.getDefaultVibrator();
        density = context.getResources().getDisplayMetrics().density;
        tp.setTypeface(Typeface.SANS_SERIF);
        setLayerType(View.LAYER_TYPE_SOFTWARE, null);
        reloadAim();
    }
    void reloadAim() {
        SharedPreferences sp = TouchSettings.prefs(getContext());
        aim.load(sp); joystick = sp.getBoolean("joystick", false);
        btnA = sp.getFloat("btnOp", 50f) / 100f; txtA = sp.getFloat("txtOp", 100f) / 100f;
        invalidate();
    }
    void setController(boolean c) { controller = c; publish(); invalidate(); }
    private boolean showButtons() { return !hidden && (!controller || forceShow); }
    void release() { ptrs.clear(); lastMask = -1; aim.cancel(); send(0, 0, 0, 0, 0); sendAim(0, 0, 0.01f); invalidate(); }
    private static int al(int c, float m) { return (c & 0xFFFFFF) | (Math.round(Math.min(255f, (c >>> 24) * m)) << 24); }

    @Override protected void onLayout(boolean changed, int l, int t, int rt, int b) {
        super.onLayout(changed, l, t, rt, b);
        int w = rt - l, h = b - t, m = (int) (w * 0.06f);   // keep the system back swipe off the screen edges
        setSystemGestureExclusionRects(Arrays.asList(new Rect(0, 0, m, h), new Rect(w - m, 0, w, h)));
    }

    // ---- loops: aim (per display frame while aiming or coasting) and button animation ----
    private final Choreographer.FrameCallback frame = new Choreographer.FrameCallback() {
        @Override public void doFrame(long t) {
            float dt = lastFrame == 0 ? 0.0083f : Math.min(0.05f, Math.max(0.0005f, (t - lastFrame) / 1e9f));
            lastFrame = t;
            aim.step(dt, aimOut);
            sendAim(aimOut[0], aimOut[1], dt);
            if (aim.active()) Choreographer.getInstance().postFrameCallback(this); else { looping = false; lastFrame = 0; }
        }
    };
    private void startLoop() { if (!looping) { looping = true; lastFrame = 0; Choreographer.getInstance().postFrameCallback(frame); } }
    private boolean aimBusy() { for (int k = 0; k < ptrs.size(); k++) if (ptrs.valueAt(k).aim) return true; return false; }
    private final Choreographer.FrameCallback animCb = new Choreographer.FrameCallback() {
        @Override public void doFrame(long t) {
            float dt = animLast == 0 ? 0.0083f : Math.min(0.05f, (t - animLast) / 1e9f);
            animLast = t;
            boolean more = false;
            for (int i = 0; i < bs.length; i++) {
                float target = pressed(i) ? 1f : 0f, a = anim[i];
                if (a == target) continue;
                boolean up = target > a;
                a += up ? dt / 0.09f : -dt / 0.15f;
                a = up ? Math.min(a, 1f) : Math.max(a, 0f);
                anim[i] = a;
                if (a != target) more = true;
            }
            invalidate();
            if (more) Choreographer.getInstance().postFrameCallback(this); else { animating = false; animLast = 0; }
        }
    };
    private void kickAnim() { if (!animating) { animating = true; animLast = 0; Choreographer.getInstance().postFrameCallback(animCb); } }

    // ---- shapes ----
    private static float d2(float[] q, int a, int b) { float dx = q[2 * a] - q[2 * b], dy = q[2 * a + 1] - q[2 * b + 1]; return dx * dx + dy * dy; }
    /** Convex hull (gift wrapping) of cnt points; returns the number of floats written to out. */
    private static int convexHull(float[] q, int cnt, float[] out) {
        int start = 0;
        for (int i = 1; i < cnt; i++)
            if (q[2 * i] < q[2 * start] - 1e-3f || (Math.abs(q[2 * i] - q[2 * start]) <= 1e-3f && q[2 * i + 1] < q[2 * start + 1])) start = i;
        int cur = start, m = 0;
        do {
            out[m++] = q[2 * cur]; out[m++] = q[2 * cur + 1];
            int next = -1;
            for (int i = 0; i < cnt; i++) {
                if (i == cur || d2(q, i, cur) < 1e-6f) continue;
                if (next < 0) { next = i; continue; }
                float cr = (q[2 * next] - q[2 * cur]) * (q[2 * i + 1] - q[2 * cur + 1]) - (q[2 * next + 1] - q[2 * cur + 1]) * (q[2 * i] - q[2 * cur]);
                if (cr < -1e-3f || (Math.abs(cr) <= 1e-3f && d2(q, cur, i) > d2(q, cur, next))) next = i;
            }
            if (next < 0) break;
            cur = next;
        } while (cur != start && m < out.length - 2);
        return m;
    }
    /** Polygon button outline: two regular n-gons (rotated by rot) joined by their convex hull, so a wide or tall button is a
     * rectangle between two hemi-polygons and rot turns only the polygons. grow offsets the outline outwards. */
    private int hull(TouchLayout.Btn b, float cx, float cy, float u, float grow, float[] out) {
        int n = b.sides;
        float hw = b.w * u / 2, hh = b.h * u / 2, a = Math.min(hw, hh) + grow, r = a / (float) Math.cos(Math.PI / n), d = Math.abs(hw - hh);
        boolean wide = hw >= hh;
        double base = -Math.PI / 2 + (n % 2 == 0 ? Math.PI / n : 0) + Math.toRadians(b.rot);
        float[] pts = new float[4 * n]; int m = 0;
        for (int s = -1; s <= 1; s += 2) for (int k = 0; k < n; k++) {
            double t = base + 2 * Math.PI * k / n;
            pts[m++] = cx + (wide ? s * d : 0) + r * (float) Math.cos(t);
            pts[m++] = cy + (wide ? 0 : s * d) + r * (float) Math.sin(t);
        }
        return convexHull(pts, 2 * n, out);
    }
    private Path shape(TouchLayout.Btn b, float cx, float cy, float u, float grow) {
        path.rewind();
        if (b.sides == 0) {
            float hw = b.w * u / 2, hh = b.h * u / 2, rad = Math.min(hw, hh) * b.rp / 100f + grow;
            rs.set(cx - hw - grow, cy - hh - grow, cx + hw + grow, cy + hh + grow);
            path.addRoundRect(rs, rad, rad, Path.Direction.CW);
            if (b.rot != 0) { mx.setRotate(b.rot, cx, cy); path.transform(mx); }
        } else {
            int m = hull(b, cx, cy, u, grow, hp);
            if (m >= 6) { path.moveTo(hp[0], hp[1]); for (int i = 2; i < m; i += 2) path.lineTo(hp[i], hp[i + 1]); path.close(); }
        }
        return path;
    }
    private void effect(TouchLayout.Btn b, float u, float grow) {
        p.setPathEffect(b.sides == 0 ? null : new CornerPathEffect(Math.max(0.01f, b.rp / 100f * Math.min(b.w, b.h) * u / 2 + grow)));
    }

    // ---- touch ----
    private boolean contains(int i, float x, float y) {
        TouchLayout.Btn b = bs[i];
        float u = getWidth() / 100f, cx = b.x * u, cy = b.y * getHeight() / 100f, hw = b.w * u / 2, hh = b.h * u / 2;
        if (b.sides == 0) {
            double t = Math.toRadians(-b.rot); float dx = x - cx, dy = y - cy;
            float rx = (float) (dx * Math.cos(t) - dy * Math.sin(t)), ry = (float) (dx * Math.sin(t) + dy * Math.cos(t));
            return Math.abs(rx) <= hw && Math.abs(ry) <= hh;
        }
        int m = hull(b, cx, cy, u, 0, hp);
        if (m < 6) return false;
        boolean pos = false, neg = false;
        for (int k = 0; k < m; k += 2) {
            int n2 = (k + 2) % m;
            float cr = (hp[n2] - hp[k]) * (y - hp[k + 1]) - (hp[n2 + 1] - hp[k + 1]) * (x - hp[k]);
            if (cr > 0) pos = true; else if (cr < 0) neg = true;
        }
        return !(pos && neg);
    }
    private int sysAt(float x, float y) {
        float u = getWidth() / 100f, cy = 5.56f * getHeight() / 100f;
        for (int i = 0; i < SYS_X.length; i++) if (Math.abs(x - SYS_X[i] * u) <= 2.45f * u && Math.abs(y - cy) <= 1.75f * u) return i;
        return -1;
    }
    private boolean pressed(int i) {
        if ((bs[i].flags & TouchLayout.TOG) != 0) return toggled[i];
        for (int k = 0; k < ptrs.size(); k++) { Ptr q = ptrs.valueAt(k); if (q.own[i] || q.hov[i]) return true; }
        return false;
    }
    private void tick() { if (vib != null) vib.vibrate(VibrationEffect.createOneShot(12, 120)); }
    private void end(int id) {
        Ptr q = ptrs.get(id);
        if (q != null && q.aim) { aim.touchUp(); startLoop(); }
        ptrs.remove(id);
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        int act = e.getActionMasked(), idx = e.getActionIndex();
        switch (act) {
            case MotionEvent.ACTION_DOWN: requestUnbufferedDispatch(e); down(e.getPointerId(0), e.getX(0), e.getY(0)); break;
            case MotionEvent.ACTION_POINTER_DOWN: down(e.getPointerId(idx), e.getX(idx), e.getY(idx)); break;
            case MotionEvent.ACTION_MOVE: for (int k = 0; k < e.getPointerCount(); k++) move(e.getPointerId(k), e.getX(k), e.getY(k)); break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: end(e.getPointerId(idx)); break;
            case MotionEvent.ACTION_CANCEL: while (ptrs.size() > 0) end(ptrs.keyAt(0)); break;
            default: break;
        }
        publish(); kickAnim(); invalidate();
        return true;
    }
    private void down(int id, float x, float y) {
        int s = sysAt(x, y);
        if (s >= 0) {
            tick();
            if (s == 0) { if (aimHook != null) aimHook.run(); }
            else if (s == 1) { if (displayHook != null) displayHook.run(); }
            else if (controller && !forceShow) forceShow = true; else hidden = !hidden;
            return;
        }
        if (!showButtons()) return;
        Ptr q = new Ptr(bs.length); ptrs.put(id, q);
        q.px = x; q.py = y;
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
            boolean right = x >= getWidth() / 2f;
            if (right && !joystick) { if (!aimBusy()) { q.aim = true; aim.touchDown(); startLoop(); } }
            else { q.stick = right ? 1 : 0; q.cx = x; q.cy = y; }
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
        if (q.aim) aim.move((x - q.px) / density, (y - q.py) / density);
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
    private void sendAim(float sx, float sy, float dt) {
        if (!nativeOk) return;
        try { TouchNative.nativeAim(sx, sy, dt); } catch (LinkageError missing) { nativeOk = false; }
    }

    // ---- drawing ----
    @Override protected void onDraw(Canvas c) {
        float W = getWidth(), H = getHeight(), u = W / 100f;
        if (W <= 0) return;
        if (showButtons()) {
            for (int i = 0; i < bs.length; i++) { TouchLayout.Btn b = bs[i]; drawButton(c, b, b.label, b.x * u, b.y * H / 100f, u, anim[i], toggled[i]); }
            for (int k = 0; k < ptrs.size(); k++) { Ptr q = ptrs.valueAt(k); if (q.stick >= 0) stick(c, q, u); }
        }
        String[] names = {"Aim", "Display", controller && !forceShow ? "Show" : hidden ? "Show" : "Hide"};
        for (int i = 0; i < SYS_X.length; i++) drawButton(c, sys, names[i], SYS_X[i] * u, 5.56f * H / 100f, u, 0f, false);
    }
    private void drawButton(Canvas c, TouchLayout.Btn b, String label, float cx, float cy, float u, float an, boolean togOn) {
        float k = .28f * u, g = .144f * u, e = g + k / 2, ow = .25f * u, hw = b.w * u / 2, hh = b.h * u / 2;
        boolean hov = (b.flags & TouchLayout.HOV) != 0, thru = (b.flags & (TouchLayout.THRU_BTN | TouchLayout.THRU_CAM)) != 0;
        int black = al(0xB8000000, btnA);
        float sc = 1f - .05f * an;
        c.save(); c.scale(sc, sc, cx, cy);
        p.setShader(null);
        if (hov) {   // hover buttons: the gap is filled and there is no outer ring
            effect(b, u, g); p.setStyle(Paint.Style.FILL); p.setColor(black); c.drawPath(shape(b, cx, cy, u, g), p);
        } else {
            effect(b, u, e); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(k); p.setColor(black); c.drawPath(shape(b, cx, cy, u, e), p);
        }
        effect(b, u, 0); p.setStyle(Paint.Style.FILL);
        if (thru) p.setShader(new RadialGradient(cx, cy, Math.max(hw, hh), new int[]{al(black, THRU_INNER), al(black, THRU_INNER), black},
                new float[]{0f, .28f, .92f}, Shader.TileMode.CLAMP));
        else p.setColor(black);
        c.drawPath(shape(b, cx, cy, u, 0), p);
        if (an < 1f) {
            p.setShader(new LinearGradient(0, cy - hh, 0, cy, al(0x14FFFFFF, btnA * (1f - an)), 0x00FFFFFF, Shader.TileMode.CLAMP));
            c.drawPath(shape(b, cx, cy, u, 0), p);
        }
        p.setShader(null);
        if (an > 0f) {   // press highlight, faded by the animation value
            p.setColor(al(PRESS_FILL, an)); c.drawPath(shape(b, cx, cy, u, 0), p);
            effect(b, u, ow / 2); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(ow); p.setColor(al(PRESS_LINE, an));
            c.drawPath(shape(b, cx, cy, u, ow / 2), p);
        }
        p.setPathEffect(null); p.setStyle(Paint.Style.FILL);
        drawLabel(c, b, label, cx, cy, u, togOn);
        c.restore();
    }
    /** Sets tp to the font at the widest width that keeps s within avail, and returns the text width. A variable font
     * narrows its width axis; a font without one is squeezed as a fallback. */
    private float fitWidth(String s, float avail, boolean autofit) {
        tp.setTextScaleX(1f);
        boolean var = tp.setFontVariationSettings("'wght' 150, 'wdth' 112");
        float w = tp.measureText(s);
        if (!autofit || w <= avail) return w;
        if (var) {
            for (int wd = 104; wd >= 62; wd -= 6) {
                tp.setFontVariationSettings("'wght' 150, 'wdth' " + wd);
                w = tp.measureText(s);
                if (w <= avail) return w;
            }
        }
        tp.setTextScaleX(Math.max(0.62f, avail / w));
        return tp.measureText(s);
    }
    private void putText(Canvas c, String s, float x, float base) {
        tp.setShader(new LinearGradient(0, base + tp.ascent(), 0, base + tp.descent(), al(0xFFFFFFFF, txtA), al(0xFFCCCCCC, txtA), Shader.TileMode.CLAMP));   // 100% to 80% brightness
        c.drawText(s, x, base, tp);
        tp.setShader(null);
    }
    private void glyph(Canvas c, TouchLayout.Btn b, boolean togOn, float gx, float gy, float u) {
        p.setShader(null); p.setPathEffect(null);
        if (b.icon != 0) { p.setStyle(Paint.Style.FILL); p.setColor(al(b.icon, txtA)); c.drawCircle(gx, gy, 1.6f * u, p); }
        else { p.setStyle(togOn ? Paint.Style.FILL : Paint.Style.STROKE); p.setStrokeWidth(.2f * u); p.setColor(al(0xFFFFFFFF, txtA)); c.drawCircle(gx, gy, .5f * u, p); }
    }
    private void drawLabel(Canvas c, TouchLayout.Btn b, String s, float cx, float cy, float u, boolean togOn) {
        boolean out = (b.flags & TouchLayout.OUT) != 0, below = (b.flags & TouchLayout.BELOW) != 0, fit = (b.flags & TouchLayout.NOFIT) == 0;
        boolean has = b.icon != 0 || (b.flags & TouchLayout.TOG) != 0;
        float size = 1.95f * u, hw = b.w * u / 2, hh = b.h * u / 2, gd = b.icon != 0 ? 3.2f * u : 1.0f * u, gap = 0.6f * u;
        tp.setTextSize(size); tp.setTextSkewX((b.flags & TouchLayout.NOSKEW) != 0 ? 0 : SKEW);
        if (s.isEmpty()) { if (has) glyph(c, b, togOn, cx, cy, u); return; }
        float cap = size * 0.72f;
        if (out) {   // the glyph stays in the button; the label is always outside, a regular gap away
            if (has) glyph(c, b, togOn, cx, cy, u);
            float w = fitWidth(s, 1e6f, false), gp = 1.0f * u;
            if (below) putText(c, s, cx - w / 2, cy + hh + gp + cap); else putText(c, s, cx + hw + gp, cy - (tp.ascent() + tp.descent()) / 2 + cy - cy);
            return;
        }
        float full = 2 * hw * 0.84f;
        if (has && below) {   // glyph above, label below it, both inside
            float w = fitWidth(s, full, fit), stack = gd + 0.4f * u + cap, top = cy - stack / 2;
            glyph(c, b, togOn, cx, top + gd / 2, u); putText(c, s, cx - w / 2, top + gd + 0.4f * u + cap);
        } else if (has) {
            float w = fitWidth(s, full - gd - gap, fit), x0 = cx - (gd + gap + w) / 2;
            glyph(c, b, togOn, x0 + gd / 2, cy, u); putText(c, s, x0 + gd + gap, cy - (tp.ascent() + tp.descent()) / 2);
        } else {
            float w = fitWidth(s, full, fit); putText(c, s, cx - w / 2, cy - (tp.ascent() + tp.descent()) / 2);
        }
    }
    private void stick(Canvas c, Ptr q, float u) {
        float R = STICK_R * u, dx = q.px - q.cx, dy = q.py - q.cy, len = (float) Math.hypot(dx, dy), k = len > R ? R / len : 1f;
        p.setShader(null); p.setPathEffect(null); p.setStyle(Paint.Style.FILL); p.setColor(0x55000000); c.drawCircle(q.cx, q.cy, R, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(.4f * u); p.setColor(0xA6FFFFFF); c.drawCircle(q.cx, q.cy, R, p);
        float kx = q.cx + dx * k, ky = q.cy + dy * k, kr = R * 0.42f;
        p.setStyle(Paint.Style.FILL);
        p.setShader(new RadialGradient(kx, ky, kr, 0xB3FFFFFF, 0xFFFFFFFF, Shader.TileMode.CLAMP));   // centre 0.7x the edge opacity
        c.drawCircle(kx, ky, kr, p);
        p.setShader(null);
    }
}

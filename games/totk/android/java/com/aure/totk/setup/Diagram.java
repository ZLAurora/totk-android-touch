package com.aure.totk.setup;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.DashPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.View;

/** The small explanatory picture in a detail panel: one original, procedural drawing per option, showing the value
 * that is currently selected (frames for frame rate and generation, a split landscape for the upscaler and sharpness,
 * the overlay's corner on a handheld, and so on). Drawn in design pixels; no game art. */
final class Diagram extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final RectF box = new RectF();
    private String key = "";
    private Settings settings;
    private boolean secondScreen;
    private Bitmap low, full;
    private int bitmapW, bitmapH;

    Diagram(Context context) { super(context); }
    /** Height in design pixels for an option's picture. */
    static float height(String key) {
        switch (key) {
            case "frame_rate": case "frame_gen": return 150;
            case "latency": case "audio_latency": case "aa": return 200;
            case "game_language": return 170;
            case "gpu_feed": case "rumble": case "motion": return 0;   // text and facts only
            default: return 300;
        }
    }
    Diagram show(String key, Settings settings, boolean secondScreen) {
        this.key = key; this.settings = settings; this.secondScreen = secondScreen;
        invalidate();
        return this;
    }
    private String value(String name) { return settings == null ? "" : settings.get(name); }
    private float s(float v) { return Theme.f(v); }

    @Override protected void onDraw(Canvas c) {
        if (settings == null) return;
        float w = getWidth(), h = getHeight(), natural = s(height(key));
        // A squeezed panel shrinks the whole picture uniformly (centred) instead of letting parts spill out.
        if (h < natural) {
            float k = h / natural;
            c.translate(w * (1 - k) / 2, 0); c.scale(k, k);
            h = natural;
        }
        switch (key) {
            case "frame_rate": frames(c, w, h, value("frame_rate").equals("unlimited") ? 8 : value("frame_rate").equals("60") ? 6 : 3, false); break;
            case "frame_gen": frames(c, w, h, 3, value("frame_gen").equals("on")); break;
            case "render_res": resolution(c, w, h); break;
            case "latency": queue(c, w, h); break;
            case "upscaler": split(c, w, h, true); break;
            case "sharpness_fsr": case "sharpness_sgsr": split(c, w, h, false); break;
            case "aa": edges(c, w, h); break;
            case "shadows": shadows(c, w, h); break;
            case "console_mode": consoles(c, w, h); break;
            case "native_hud": hud(c, w, h); break;
            case "overlay": case "overlay_pos": handheld(c, w, h); break;
            case "second_screen": dual(c, w, h); break;
            case "buttons": buttons(c, w, h); break;
            case "audio_latency": audio(c, w, h); break;
            case "game_language": language(c, w, h); break;
            default: break;
        }
    }

    // ---------- shared scenery ----------

    /** A tiny scene in a 180 x 150 box: ground ridge, a floating island at x, a lime light above it and the sun. */
    private void mini(Canvas c, RectF r, float islandX) {
        c.save(); c.clipRect(r); c.translate(r.left, r.top); c.scale(r.width() / 180f, r.height() / 150f);
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new LinearGradient(0, 0, 0, 150, new int[]{0xff24443c, 0xff1a2f29, 0xff13201c}, new float[]{0, .55f, 1}, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, 180, 150, paint);
        paint.setShader(null);
        Path ground = new Path();
        ground.moveTo(0, 118); ground.lineTo(40, 92); ground.lineTo(70, 104); ground.lineTo(110, 80); ground.lineTo(150, 100); ground.lineTo(180, 90);
        ground.lineTo(180, 150); ground.lineTo(0, 150); ground.close();
        paint.setColor(0xff0f1a17); c.drawPath(ground, paint);
        Path island = new Path();
        island.moveTo(islandX - 30, 52); island.lineTo(islandX + 30, 52); island.lineTo(islandX + 16, 66); island.lineTo(islandX, 74); island.lineTo(islandX - 16, 66); island.close();
        paint.setColor(0xff2c4a40); c.drawPath(island, paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.5f); paint.setColor(Theme.ZONAI); c.drawPath(island, paint);
        paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL); c.drawCircle(islandX, 38, 5, paint);
        paint.setColor(Theme.alpha(Theme.AMBER, .5f)); c.drawCircle(150, 28, 10, paint);
        c.restore();
    }
    /** The larger landscape (1200 x 600 units): sky, far ridge, hills, a floating island with a waterfall and grass. */
    private void landscape(Canvas c, float w, float h) {
        c.save(); c.scale(w / 1200f, h / 600f);
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new LinearGradient(0, 0, 0, 600, new int[]{0xff7fb7c9, 0xffcfe0c8, 0xffe8dcb0}, new float[]{0, .6f, 1}, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, 1200, 600, paint);
        paint.setShader(null);
        paint.setColor(0xd9fff4cf); c.drawCircle(980, 120, 54, paint);
        paint.setColor(0xb38aa7a0);
        c.drawPath(poly(0, 360, 120, 300, 260, 340, 420, 260, 560, 330, 700, 280, 860, 340, 1000, 290, 1200, 330, 1200, 600, 0, 600), paint);
        paint.setShader(new LinearGradient(0, 360, 0, 600, 0xff5f8a55, 0xff2f4a33, Shader.TileMode.CLAMP));
        c.drawPath(poly(0, 430, 160, 380, 300, 420, 480, 360, 640, 420, 820, 370, 1000, 430, 1200, 400, 1200, 600, 0, 600), paint);
        paint.setShader(null);
        Path rock = poly(440, 150, 760, 150, 700, 196, 600, 236, 500, 196);
        paint.setColor(0xff6a5a44); c.drawPath(rock, paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(4); paint.setColor(0xff3a3024); c.drawPath(rock, paint);
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff6f9a5c);
        Path top = new Path(); top.moveTo(440, 150); top.cubicTo(520, 128, 680, 128, 760, 150); top.close(); c.drawPath(top, paint);
        paint.setColor(0xccd9eef0); c.drawRect(612, 196, 622, 356, paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(3); paint.setColor(0xff9fc27a);
        for (int i = 0; i < 140; i++) {
            float x = (i * 97) % 1200, y = 470 + (i * 53) % 120;
            Path blade = new Path(); blade.moveTo(x, y); blade.lineTo(x + 6, y - 22); blade.lineTo(x + 10, y); c.drawPath(blade, paint);
        }
        c.restore();
    }
    private static Path poly(float... xy) {
        Path p = new Path(); p.moveTo(xy[0], xy[1]);
        for (int i = 2; i < xy.length; i += 2) p.lineTo(xy[i], xy[i + 1]);
        p.close();
        return p;
    }
    /** The landscape at full size and at a third of it (enlarged with filtering: the soft, plain-scaled look). */
    private void bitmaps(int w, int h) {
        if (full != null && bitmapW == w && bitmapH == h) return;
        bitmapW = w; bitmapH = h;
        full = Bitmap.createBitmap(Math.max(1, w), Math.max(1, h), Bitmap.Config.ARGB_8888);
        landscape(new Canvas(full), w, h);
        int lw = Math.max(1, w / 3), lh = Math.max(1, h / 3);
        low = Bitmap.createBitmap(lw, lh, Bitmap.Config.ARGB_8888);
        landscape(new Canvas(low), lw, lh);
    }
    private void frameBorder(Canvas c, RectF r, float radius, int color, float width, boolean dashed) {
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(width); paint.setColor(color);
        paint.setPathEffect(dashed ? new DashPathEffect(new float[]{s(5), s(4)}, 0) : null);
        c.drawRoundRect(r, radius, radius, paint);
        paint.setPathEffect(null);
    }
    private void tag(Canvas c, String text, float x, float y, boolean right) {
        paint.setTypeface(Theme.regular); paint.setTextSize(s(15)); paint.setTextAlign(Paint.Align.LEFT);
        float tw = paint.measureText(text), pw = s(8), ph = s(3), th = s(19);
        float left = right ? x - tw - 2 * pw : x;
        paint.setStyle(Paint.Style.FILL); paint.setColor(0x80000000);
        c.drawRoundRect(left, y - th - 2 * ph, left + tw + 2 * pw, y, s(6), s(6), paint);
        paint.setColor(Theme.TEXT);
        c.drawText(text, left + pw, y - ph - s(5), paint);
    }
    private void label(Canvas c, String text, float x, float y, float size, int color, Paint.Align align) {
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Theme.regular); paint.setTextSize(s(size)); paint.setTextAlign(align); paint.setColor(color);
        c.drawText(text, x, y, paint);
        paint.setTextAlign(Paint.Align.LEFT);
    }

    // ---------- per option ----------

    /** Frame rate / frame generation: a strip of frames; generated ones are dashed and glow. */
    private void frames(Canvas c, float w, float h, int count, boolean generated) {
        float gap = s(14), fw = (w - gap * (count - 1)) / count, travel = 56f / Math.max(1, count - 1);
        for (int i = 0; i < count; i++) {
            box.set(i * (fw + gap), 0, i * (fw + gap) + fw, h);
            boolean gen = generated && i == 1;
            c.save();
            Path clip = new Path(); clip.addRoundRect(box, s(10), s(10), Path.Direction.CW); c.clipPath(clip);
            mini(c, box, 52 + i * travel);
            c.restore();
            if (gen) { paint.setShadowLayer(s(10), 0, 0, Theme.SEL_GLOW); frameBorder(c, box, s(10), Theme.SEL, s(1.5f), true); paint.clearShadowLayer(); }
            else frameBorder(c, box, s(10), Theme.RULE, s(1), false);
            tag(c, gen ? Strings.DIAG_GENERATED : count > 3 ? String.valueOf(i + 1) : Strings.DIAG_REAL, box.left + s(10), box.bottom - s(8), false);
        }
    }
    /** Render resolution: the screen, and the scene drawn at the chosen size inside it. */
    private void resolution(Canvas c, float w, float h) {
        float sw = Math.min(w * .8f, h * .9f * 16 / 9f), sh = sw * 9 / 16, x0 = (w - sw) / 2, y0 = (h - sh) / 2;
        RectF screen = new RectF(x0, y0, x0 + sw, y0 + sh);
        String res = value("render_res");
        float k = res.equals("1280x720") || res.equals("game") ? 1 : res.equals("1152x648") ? .9f : .75f;
        RectF scene = new RectF(x0, y0 + sh * (1 - k), x0 + sw * k, y0 + sh);
        bitmaps(Math.round(sw), Math.round(sh));
        c.save(); c.clipRect(scene);
        c.drawBitmap(full, null, scene, paint);
        c.restore();
        frameBorder(c, screen, s(8), Theme.RULE, s(2), false);
        frameBorder(c, scene, s(4), res.equals("game") ? Theme.ZONAI : Theme.SEL, s(2), res.equals("game"));
        String text = res.equals("game") ? Strings.DIAG_DYNAMIC : res.replace("x", " × ");
        tag(c, text, scene.left + s(10), scene.bottom - s(10), false);
        tag(c, Strings.DIAG_SCREEN, screen.right - s(10), screen.top + s(34), true);
    }
    /** Latency: game -> waiting pictures -> screen; more slots smooth uneven frames but add delay. */
    private void queue(Canvas c, float w, float h) {
        boolean locked = settings.latencyLocked();
        int slots = locked ? 1 : value("latency").equals("responsive") ? 1 : value("latency").equals("balanced") ? 2 : 3;
        float cy = h / 2, bw = s(84), bh = s(60), gap = s(16);
        float total = 3 * bw + 2 * gap, start = (w - total) / 2;
        label(c, Strings.DIAG_GAME, start - s(24), cy + s(7), 20, Theme.TEXT2, Paint.Align.RIGHT);
        label(c, Strings.DIAG_SCREEN_SHORT, start + total + s(24), cy + s(7), 20, Theme.TEXT2, Paint.Align.LEFT);
        for (int i = 0; i < 3; i++) {
            box.set(start + i * (bw + gap), cy - bh / 2, start + i * (bw + gap) + bw, cy + bh / 2);
            boolean used = i >= 3 - slots;
            if (used) { c.save(); Path clip = new Path(); clip.addRoundRect(box, s(8), s(8), Path.Direction.CW); c.clipPath(clip); mini(c, box, 60 + i * 30); c.restore(); }
            frameBorder(c, box, s(8), used ? Theme.SEL : 0xff33433b, s(used ? 1.5f : 1), !used);
        }
        paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL);
        Path arrow = new Path();
        float ax = start + total + s(10);
        arrow.moveTo(ax, cy - s(8)); arrow.lineTo(ax + s(10), cy); arrow.lineTo(ax, cy + s(8)); arrow.close();
        c.drawPath(arrow, paint);
        label(c, Strings.waiting(slots), w / 2, cy + bh / 2 + s(40), 18, Theme.TEXT3, Paint.Align.CENTER);
    }
    /** Upscaler (blocky vs filtered) and sharpness (soft vs restored detail): one landscape split down the middle. */
    private void split(Canvas c, float w, float h, boolean upscaler) {
        box.set(0, 0, w, h);
        bitmaps(Math.round(w), Math.round(h));
        c.save();
        Path clip = new Path(); clip.addRoundRect(box, s(12), s(12), Path.Direction.CW); c.clipPath(clip);
        String scaler = value("upscaler");
        int sharpness = upscaler ? 50 : settings.number(key);
        // left: the picture without it (plain enlargement, or sharpness 0): soft
        c.save(); c.clipRect(0, 0, w / 2, h);
        c.drawBitmap(low, null, box, paint);
        c.restore();
        // right: with it (an upscaler gives a clean edge; sharpness adds contrast with its value)
        c.save(); c.clipRect(w / 2, 0, w, h);
        boolean on = !upscaler || !scaler.equals("off");
        if (on) {
            float contrast = 1 + .3f * sharpness / 100f;
            ColorMatrix m = new ColorMatrix(new float[]{contrast, 0, 0, 0, 128 * (1 - contrast), 0, contrast, 0, 0, 128 * (1 - contrast),
                    0, 0, contrast, 0, 128 * (1 - contrast), 0, 0, 0, 1, 0});
            ColorMatrix sat = new ColorMatrix(); sat.setSaturation(1 + .1f * sharpness / 100f); m.postConcat(sat);
            paint.setColorFilter(new ColorMatrixColorFilter(m));
            c.drawBitmap(full, null, box, paint);
            paint.setColorFilter(null);
        } else {
            c.drawBitmap(low, null, box, paint);
        }
        c.restore();
        c.restore();
        paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL); paint.setShadowLayer(s(8), 0, 0, Theme.SEL_GLOW);
        c.drawRect(w / 2 - s(1), 0, w / 2 + s(1), h, paint);
        paint.clearShadowLayer();
        frameBorder(c, box, s(12), Theme.RULE, s(1), false);
        if (upscaler) {
            tag(c, Strings.DIAG_PLAIN, s(12), h - s(12), false);
            tag(c, Strings.upscalerName(scaler), w - s(12), h - s(12), true);
        } else {
            tag(c, "0", s(12), h - s(12), false);
            tag(c, String.valueOf(sharpness), w - s(12), h - s(12), true);
        }
    }
    /** Anti-aliasing: a zoomed diagonal edge as pixels, stepped (off), partly blended (FXAA) or smooth (SMAA). */
    private void edges(Canvas c, float w, float h) {
        String aa = value("aa");
        int n = 16;
        float cell = Math.min(h / 9f, w / (n + 2f)), x0 = (w - n * cell) / 2, y0 = (h - 8 * cell) / 2;
        for (int y = 0; y < 8; y++) for (int x = 0; x < n; x++) {
            double edge = (x + .5) * 8.0 / n;   // the true edge: a gentle diagonal
            double cover = Math.max(0, Math.min(1, (y + 1) - (8 - edge)));
            if (aa.equals("off")) cover = cover >= .5 ? 1 : 0;
            else if (aa.equals("fxaa")) cover = Math.round(cover * 2) / 2.0;
            int a = (int) Math.round(cover * 255);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xff1a2a25);
            c.drawRect(x0 + x * cell, y0 + y * cell, x0 + (x + 1) * cell - s(1), y0 + (y + 1) * cell - s(1), paint);
            paint.setColor((a << 24) | (Theme.SEL & 0xffffff));
            c.drawRect(x0 + x * cell, y0 + y * cell, x0 + (x + 1) * cell - s(1), y0 + (y + 1) * cell - s(1), paint);
        }
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(s(1.5f)); paint.setColor(Theme.alpha(Theme.TEXT, .5f));
        paint.setPathEffect(new DashPathEffect(new float[]{s(6), s(5)}, 0));
        c.drawLine(x0, y0 + 8 * cell, x0 + n * cell, y0, paint);
        paint.setPathEffect(null);
    }
    /** Shadows: a pillar's shadow on the ground, blockier at lower shadow-map resolution. */
    private void shadows(Canvas c, float w, float h) {
        String q = value("shadows");
        float block = s(q.equals("high") ? 3 : q.equals("medium") ? 7 : 12);
        box.set(0, 0, w, h);
        c.save(); Path clip = new Path(); clip.addRoundRect(box, s(12), s(12), Path.Direction.CW); c.clipPath(clip);
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new LinearGradient(0, 0, 0, h * .45f, 0xff7fb7c9, 0xffcfe0c8, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, w, h * .45f, paint);
        paint.setShader(new LinearGradient(0, h * .45f, 0, h, 0xff5f8a55, 0xff2f4a33, Shader.TileMode.CLAMP));
        c.drawRect(0, h * .45f, w, h, paint);
        paint.setShader(null);
        float px = w * .3f, base = h * .78f, pw = w * .07f, ph = h * .5f;
        // the shadow, cast to the lower right and rasterised in blocks of the chosen size (a smaller shadow map = bigger blocks)
        paint.setColor(0xa0142018);
        float length = w * .55f, slope = .18f;
        for (float y = base - pw; y < base + length * slope + pw; y += block)
            for (float x = px - pw / 2; x < px + length; x += block) {
                float cx = x + block / 2, cy = y + block / 2, t = (cx - px) / length;
                float centre = base - pw * .05f + Math.max(0, t) * length * slope, thick = pw * .45f * (1 + .5f * Math.max(0, t));
                if (t <= 1 && cx >= px - pw / 2 && Math.abs(cy - centre) <= thick / 2) c.drawRect(x, y, x + block, y + block, paint);
            }
        paint.setColor(0xffcfc2a0); c.drawRect(px - pw / 2, base - ph, px + pw / 2, base, paint);
        paint.setColor(0xffa89c7c); c.drawRect(px + pw * .15f, base - ph, px + pw / 2, base, paint);
        paint.setColor(0xd9fff4cf); c.drawCircle(w * .12f, h * .14f, s(26), paint);
        c.restore();
        frameBorder(c, box, s(12), Theme.RULE, s(1), false);
        tag(c, Strings.shadowRes(q), w - s(12), h - s(12), true);
    }
    /** Console mode: the handheld and the TV, the chosen one lit, with the resolution the game targets. */
    private void consoles(Canvas c, float w, float h) {
        boolean docked = value("console_mode").equals("docked");
        float cy = h / 2;
        RectF hand = new RectF(w * .06f, cy - s(60), w * .44f, cy + s(60));
        RectF tv = new RectF(w * .54f, cy - s(95), w * .94f, cy + s(70));
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff0d1412);
        c.drawRoundRect(hand, s(30), s(30), paint);
        frameBorder(c, hand, s(30), docked ? 0xff34433b : Theme.SEL, s(docked ? 2 : 3), false);
        RectF hs = new RectF(hand.left + s(44), hand.top + s(14), hand.right - s(44), hand.bottom - s(14));
        c.save(); Path clip = new Path(); clip.addRoundRect(hs, s(8), s(8), Path.Direction.CW); c.clipPath(clip); mini(c, hs, 90); c.restore();
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff1d2824);
        c.drawCircle(hand.left + s(22), cy - s(12), s(12), paint); c.drawCircle(hand.right - s(22), cy + s(12), s(12), paint);
        paint.setColor(0xff0d1412); c.drawRoundRect(tv, s(10), s(10), paint);
        frameBorder(c, tv, s(10), docked ? Theme.SEL : 0xff34433b, s(docked ? 3 : 2), false);
        RectF ts = new RectF(tv.left + s(10), tv.top + s(10), tv.right - s(10), tv.bottom - s(10));
        c.save(); clip = new Path(); clip.addRoundRect(ts, s(4), s(4), Path.Direction.CW); c.clipPath(clip); mini(c, ts, 90); c.restore();
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff34433b);
        c.drawRect(tv.centerX() - s(30), tv.bottom + s(6), tv.centerX() + s(30), tv.bottom + s(10), paint);
        label(c, Strings.DIAG_HANDHELD_RES, hand.centerX(), hand.bottom + s(36), 18, docked ? Theme.TEXT3 : Theme.SEL, Paint.Align.CENTER);
        label(c, Strings.DIAG_DOCKED_RES, tv.centerX(), tv.bottom + s(40), 18, docked ? Theme.SEL : Theme.TEXT3, Paint.Align.CENTER);
    }
    /** Native HUD: the same heart row and caption, enlarged from the game's size (soft) or drawn at the panel's (crisp). */
    private void hud(Canvas c, float w, float h) {
        boolean on = value("native_hud").equals("on");
        box.set(0, 0, w, h);
        bitmaps(Math.round(w), Math.round(h));
        c.save(); Path clip = new Path(); clip.addRoundRect(box, s(12), s(12), Path.Direction.CW); c.clipPath(clip);
        c.drawBitmap(full, null, box, paint);
        // the HUD layer: drawn small and enlarged (off) or at full size (on)
        int scale = on ? 1 : 3;
        Bitmap layer = Bitmap.createBitmap(Math.max(1, Math.round(w / scale)), Math.max(1, Math.round(h / scale)), Bitmap.Config.ARGB_8888);
        Canvas lc = new Canvas(layer);
        lc.scale(1f / scale, 1f / scale);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        for (int i = 0; i < 5; i++) {
            float x = s(28) + i * s(40), y = s(34);
            Path heart = new Path();
            heart.moveTo(x, y + s(10)); heart.cubicTo(x - s(16), y - s(2), x - s(6), y - s(16), x, y - s(6));
            heart.cubicTo(x + s(6), y - s(16), x + s(16), y - s(2), x, y + s(10)); heart.close();
            p.setColor(i < 4 ? 0xffe0524a : 0x66000000); lc.drawPath(heart, p);
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(s(2)); p.setColor(0xfff3eed8); lc.drawPath(heart, p); p.setStyle(Paint.Style.FILL);
        }
        p.setTypeface(Theme.semibold); p.setTextSize(s(24)); p.setColor(0xfff3eed8); p.setShadowLayer(s(3), 0, s(1), 0xcc000000);
        lc.drawText(Strings.DIAG_HUD_TEXT, s(28), h - s(34), p);
        paint.setFilterBitmap(true);
        c.drawBitmap(layer, null, box, paint);
        layer.recycle();
        c.restore();
        frameBorder(c, box, s(12), Theme.RULE, s(1), false);
        tag(c, on ? Strings.DIAG_NATIVE : Strings.DIAG_ENLARGED, w - s(12), h - s(12), true);
    }
    /** Overlay and its corner: a handheld with the readout in the chosen corner; the other corners dashed. */
    private void handheld(Canvas c, float w, float h) {
        float dw = Math.min(w * .88f, s(520)), dh = dw * 300 / 520, x0 = (w - dw) / 2, y0 = (h - dh) / 2;
        RectF dev = new RectF(x0, y0, x0 + dw, y0 + dh);
        float k = dw / 520;
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff0d1412); paint.setShadowLayer(s(15), 0, s(10), 0x66000000);
        c.drawRoundRect(dev, 30 * k, 30 * k, paint); paint.clearShadowLayer();
        frameBorder(c, dev, 30 * k, 0xff34433b, 3 * k, false);
        RectF inner = new RectF(dev); inner.inset(6 * k, 6 * k);
        frameBorder(c, inner, 26 * k, 0xff121b18, 8 * k, false);
        RectF scr = new RectF(dev.left + 56 * k, dev.top + 22 * k, dev.right - 56 * k, dev.bottom - 22 * k);
        c.save(); Path clip = new Path(); clip.addRoundRect(scr, 10 * k, 10 * k, Path.Direction.CW); c.clipPath(clip);
        c.save(); c.translate(scr.left, scr.top); landscape(c, scr.width(), scr.height()); c.restore();
        c.restore();
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff1d2824);
        c.drawCircle(dev.left + 29 * k, dev.top + 137 * k, 17 * k, paint); c.drawCircle(dev.right - 29 * k, dev.top + 187 * k, 17 * k, paint);
        String pos = value("overlay_pos"), mode = value("overlay");
        boolean showPos = key.equals("overlay_pos");
        String[] corners = {"tl", "tr", "bl", "br"};
        for (String corner : corners) {
            boolean right = corner.endsWith("r"), bottom = corner.startsWith("b");
            float cx = right ? scr.right - 10 * k : scr.left + 10 * k, cy = bottom ? scr.bottom - 10 * k : scr.top + 10 * k;
            if (corner.equals(pos) && (!mode.equals("off") || showPos)) {
                String[] lines = mode.equals("full") ? new String[]{"30 fps", "33.3 ms · HH", "1280×720 → 1920×1080"} : new String[]{"30 fps"};
                paint.setTypeface(Theme.regular); paint.setTextSize(13 * k * Theme.scale);
                float tw = 0;
                for (String line : lines) tw = Math.max(tw, paint.measureText(line));
                float bw = tw + 20 * k, bh = lines.length * 18 * k * Theme.scale + 12 * k;
                RectF b = new RectF(right ? cx - bw : cx, bottom ? cy - bh : cy, right ? cx : cx + bw, bottom ? cy : cy + bh);
                paint.setStyle(Paint.Style.FILL); paint.setColor(0xc7080c0b); c.drawRoundRect(b, 6 * k, 6 * k, paint);
                paint.setShadowLayer(s(7), 0, 0, Theme.SEL_GLOW);
                frameBorder(c, b, 6 * k, Theme.alpha(Theme.SEL, mode.equals("off") ? .3f : .6f), s(1), mode.equals("off"));
                paint.clearShadowLayer();
                paint.setStyle(Paint.Style.FILL);
                for (int i = 0; i < lines.length; i++) {
                    paint.setColor(mode.equals("off") ? Theme.TEXT3 : i == 0 ? Theme.SEL : 0xfff3eed8);
                    c.drawText(lines[i], b.left + 10 * k, b.top + 6 * k + (i + 1) * 18 * k * Theme.scale - 4 * k, paint);
                }
            } else {
                RectF b = new RectF(right ? cx - 16 * k : cx, bottom ? cy - 16 * k : cy, right ? cx : cx + 16 * k, bottom ? cy : cy + 16 * k);
                frameBorder(c, b, 4 * k, Theme.alpha(Theme.TEXT, .35f), 2 * k, true);
            }
        }
    }
    /** Second screen: a two-screen handheld; the lower screen shows a map grid when on. */
    private void dual(Canvas c, float w, float h) {
        boolean on = value("second_screen").equals("on");
        float dw = Math.min(w * .6f, h * 1.1f), x0 = (w - dw) / 2;
        RectF top = new RectF(x0, 0, x0 + dw, h * .48f), bottom = new RectF(x0 + dw * .08f, h * .52f, x0 + dw * .92f, h);
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff0d1412);
        c.drawRoundRect(top, s(16), s(16), paint); c.drawRoundRect(bottom, s(16), s(16), paint);
        frameBorder(c, top, s(16), 0xff34433b, s(2), false);
        frameBorder(c, bottom, s(16), on ? Theme.SEL : 0xff34433b, s(on ? 2.5f : 2), false);
        RectF ts = new RectF(top); ts.inset(s(12), s(12));
        c.save(); Path clip = new Path(); clip.addRoundRect(ts, s(6), s(6), Path.Direction.CW); c.clipPath(clip); mini(c, ts, 90); c.restore();
        RectF bs = new RectF(bottom); bs.inset(s(12), s(12));
        paint.setStyle(Paint.Style.FILL); paint.setColor(on ? 0xff1b2c26 : 0xff080c0b); c.drawRoundRect(bs, s(6), s(6), paint);
        if (on) {
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(s(1)); paint.setColor(0x4057dfbf);
            for (float x = bs.left + s(18); x < bs.right; x += s(18)) c.drawLine(x, bs.top, x, bs.bottom, paint);
            for (float y = bs.top + s(18); y < bs.bottom; y += s(18)) c.drawLine(bs.left, y, bs.right, y, paint);
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL); paint.setShadowLayer(s(6), 0, 0, Theme.SEL_GLOW);
            Theme.diamond(c, bs.centerX(), bs.centerY(), s(7), paint);
            paint.clearShadowLayer();
            paint.setColor(Theme.alpha(Theme.AMBER, .8f));
            c.drawCircle(bs.left + bs.width() * .25f, bs.top + bs.height() * .35f, s(4), paint);
            c.drawCircle(bs.left + bs.width() * .7f, bs.top + bs.height() * .7f, s(4), paint);
        } else label(c, Strings.DIAG_OFF, bs.centerX(), bs.centerY() + s(7), 18, Theme.TEXT3, Paint.Align.CENTER);
        if (!secondScreen) label(c, Strings.DIAG_NO_SECOND, w / 2, h - s(4), 16, Theme.AMBER, Paint.Align.CENTER);
    }
    /** Button layout: the four face buttons with their labels; the one that acts as the Switch's A glows. */
    private void buttons(Canvas c, float w, float h) {
        boolean xbox = value("buttons").equals("xbox");
        // positions: top, left, right, bottom
        String[] nintendo = {"X", "Y", "A", "B"}, xboxLabels = {"Y", "X", "B", "A"};
        String[] labels = xbox ? xboxLabels : nintendo;
        float cx = w / 2, cy = h / 2, d = Math.min(h * .3f, s(80)), r = d * .52f;
        float[][] at = {{cx, cy - d}, {cx - d, cy}, {cx + d, cy}, {cx, cy + d}};
        // The Switch's A is the right-hand button in both pictures: labelled A on a Nintendo pad (by label), labelled B on
        // an Xbox pad (by position).
        int switchA = 2;
        for (int i = 0; i < 4; i++) {
            boolean lit = i == switchA;
            paint.setStyle(Paint.Style.FILL); paint.setColor(lit ? Theme.KEY : 0xff26312c);
            if (lit) paint.setShadowLayer(s(14), 0, 0, Theme.SEL_GLOW);
            c.drawCircle(at[i][0], at[i][1], r, paint);
            paint.clearShadowLayer();
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(s(2)); paint.setColor(lit ? Theme.SEL : Theme.RULE);
            c.drawCircle(at[i][0], at[i][1], r, paint);
            paint.setStyle(Paint.Style.FILL); paint.setTypeface(Theme.semibold); paint.setTextSize(r); paint.setTextAlign(Paint.Align.CENTER);
            paint.setColor(lit ? Theme.KEY_INK : Theme.TEXT2);
            Paint.FontMetrics fm = paint.getFontMetrics();
            c.drawText(labels[i], at[i][0], at[i][1] - (fm.ascent + fm.descent) / 2, paint);
        }
        label(c, xbox ? Strings.DIAG_ACTS_AS_A_POS : Strings.DIAG_ACTS_AS_A_LABEL, at[2][0] + r + s(16), at[2][1] + s(7), 18, Theme.SEL, Paint.Align.LEFT);
        paint.setTextAlign(Paint.Align.LEFT);
    }
    /** Audio latency: 5 ms blocks queued before the sound starts. */
    private void audio(Canvas c, float w, float h) {
        String v = value("audio_latency");
        int blocks = v.equals("low") ? 2 : v.equals("balanced") ? 4 : 8;
        float cy = h * .42f;
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(s(2)); paint.setColor(Theme.ZONAI); paint.setStrokeCap(Paint.Cap.ROUND);
        Path wave = new Path();
        float wx = w * .55f;
        for (int i = 0; i <= 120; i++) {
            float x = wx + (w - wx) * i / 120f;
            float y = cy + (float) (Math.sin(i * .35) * Math.sin(i * .05) * s(30));
            if (i == 0) wave.moveTo(x, y); else wave.lineTo(x, y);
        }
        c.drawPath(wave, paint);
        float bw = s(26), gap = s(5);
        for (int i = 0; i < 8; i++) {
            float bx = wx - s(18) - (i + 1) * (bw + gap) + gap;
            box.set(bx, cy - s(26), bx + bw, cy + s(26));
            boolean used = i < blocks;
            paint.setStyle(Paint.Style.FILL); paint.setColor(used ? 0x33d4ea74 : 0);
            c.drawRoundRect(box, s(5), s(5), paint);
            frameBorder(c, box, s(5), used ? Theme.SEL : 0xff33433b, s(used ? 1.5f : 1), !used);
        }
        label(c, Strings.buffer(blocks * 5), wx - s(18) - blocks * (bw + gap) / 2f, cy + s(64), 18, Theme.TEXT2, Paint.Align.CENTER);
    }
    /** Game language: a speech bubble with the chosen language's name. */
    private void language(Canvas c, float w, float h) {
        String lang = value("game_language");
        box.set(w * .12f, h * .08f, w * .88f, h * .72f);
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xff17221e); c.drawRoundRect(box, s(18), s(18), paint);
        Path tail = new Path();
        tail.moveTo(box.left + s(60), box.bottom - s(1)); tail.lineTo(box.left + s(48), h * .95f); tail.lineTo(box.left + s(100), box.bottom - s(1)); tail.close();
        c.drawPath(tail, paint);
        frameBorder(c, box, s(18), Theme.ZONAI, s(1.5f), false);
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Theme.semibold); paint.setTextSize(s(34)); paint.setTextAlign(Paint.Align.CENTER); paint.setColor(Theme.TEXT);
        Paint.FontMetrics fm = paint.getFontMetrics();
        c.drawText(Strings.languageName(lang), box.centerX(), box.centerY() - (fm.ascent + fm.descent) / 2, paint);
        paint.setTextAlign(Paint.Align.LEFT);
    }
}

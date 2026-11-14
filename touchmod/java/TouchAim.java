
package com.aure.totk.touch;

import android.content.SharedPreferences;

/**
 * Touch aim controller.
 *
 * Direct mode returns camera-angle deltas rather than right-stick deflection.
 * Fling momentum and overlapping fling carry are preserved.
 */
final class TouchAim {
    float sensX = 1, sensY = 1, curve = 1.2f, offset = 0.1f,
            smoothMs = 15, tau = 90, cutMs = 300, rampMs = 60,
            minFling = 700, backlogMs = 0, headroom = 1;
    boolean invertY;

    private static final float V0 = 1200f;

    private boolean down, coasting;
    private float accX, accY, vx, vy, v0x, v0y, coastT;
    private float debt, dirX, dirY, carryX, carryY, carryT;

    void load(SharedPreferences sp) {
        sensX = sp.getFloat("sensX", 1f);
        sensY = sp.getFloat("sensY", 1f);
        curve = sp.getFloat("curve", 1.2f);
        offset = sp.getFloat("offset", 0.1f);
        smoothMs = sp.getFloat("smooth", 15f);
        tau = Math.max(1f, sp.getFloat("tau", 90f));
        cutMs = sp.getFloat("cut", 300f);
        rampMs = Math.max(1f, sp.getFloat("ramp", 60f));
        minFling = sp.getFloat("minfling", 700f);
        backlogMs = sp.getFloat("backlog", 0f);
        headroom = Math.max(1f, sp.getFloat("headroom", 1f));
        invertY = sp.getBoolean("invertY", false);
    }

    private float coastFactor() {
        float ms = coastT * 1000f;
        float f = (float) Math.exp(-ms / tau);
        return ms > cutMs
                ? f * Math.max(0f, 1f - (ms - cutMs) / rampMs)
                : f;
    }

    void touchDown() {
        if (coasting) {
            float f = coastFactor();
            carryX = v0x * f;
            carryY = v0y * f;
        } else {
            carryX = carryY = 0;
        }

        carryT = 0;
        down = true;
        coasting = false;
        vx = vy = 0;
        accX = accY = 0;
    }

    void move(float dxDp, float dyDp) {
        accX += dxDp;
        accY += dyDp;
    }

    void touchUp() {
        down = false;

        if (Math.hypot(vx, vy) >= minFling) {
            float cf = (float) Math.exp(-carryT * 1000f / tau);
            v0x = vx + carryX * cf;
            v0y = vy + carryY * cf;
            coastT = 0;
            coasting = true;
        }

        vx = vy = 0;
        carryX = carryY = 0;
    }

    void cancel() {
        down = false;
        coasting = false;
        vx = vy = 0;
        carryX = carryY = 0;
        debt = 0;
        accX = accY = 0;
    }

    boolean active() {
        return down || coasting || debt > 0;
    }

    /**
     * Direct camera mode.
     * out[0] = yaw delta in degrees.
     * out[1] = pitch delta in degrees.
     *
     * Finger displacement is not normalized or clamped to stick range.
     */
    void stepDirect(float dt, float[] out) {
        if (dt <= 0f) {
            out[0] = out[1] = 0f;
            return;
        }

        float dx = 0f;
        float dy = 0f;

        if (down) {
            dx = accX;
            dy = accY;
            accX = accY = 0f;

            float ix = dx / dt;
            float iy = dy / dt;

            float a = smoothMs <= 0f
                    ? 1f
                    : 1f - (float) Math.exp(-dt * 1000f / smoothMs);

            vx += (ix - vx) * a;
            vy += (iy - vy) * a;

            carryT += dt;
            float cf = (float) Math.exp(-carryT * 1000f / tau);

            dx += carryX * cf * dt;
            dy += carryY * cf * dt;
        } else if (coasting) {
            coastT += dt;
            float f = coastFactor();

            if (f < 0.01f) {
                coasting = false;
            } else {
                dx = v0x * f * dt;
                dy = v0y * f * dt;
            }
        }

        float speed = (float) Math.hypot(vx, vy);
        float gain = 1f;

        if (curve > 1f && speed > V0) {
            gain = (float) Math.pow(speed / V0, curve - 1f);
        }

        float yaw = -dx * 0.08f * sensX * gain;
        float pitch = dy * 0.08f * sensY * gain
                * (invertY ? -1f : 1f);

        out[0] = yaw;
        out[1] = pitch;
    }

    /** Legacy stick-output mode retained for compatibility. */
    void step(float dt, float[] out) {
        float ox = 0, oy = 0;

        if (down) {
            float ix = accX / dt;
            float iy = accY / dt;
            accX = accY = 0;

            float a = smoothMs <= 0
                    ? 1f
                    : 1f - (float) Math.exp(-dt * 1000f / smoothMs);

            vx += (ix - vx) * a;
            vy += (iy - vy) * a;

            carryT += dt;
            float cf = (float) Math.exp(-carryT * 1000f / tau);
            ox = vx + carryX * cf;
            oy = vy + carryY * cf;
        } else if (coasting) {
            coastT += dt;
            float f = coastFactor();

            if (f < 0.01f) {
                coasting = false;
            } else {
                ox = v0x * f;
                oy = v0y * f;
            }
        }

        float fx = shape(ox) * sensX / headroom;
        float fy = -shape(oy) * sensY / headroom
                * (invertY ? -1f : 1f);

        float m = (float) Math.hypot(fx, fy);

        if (m > 0.001f) {
            dirX = fx / m;
            dirY = fy / m;
        }

        float cap = backlogMs / 1000f;

        if (m > 1f) {
            debt = Math.min(cap, debt + (m - 1f) * dt);
            m = 1f;
        } else if (debt > 0f && cap > 0f) {
            float add = Math.min(1f - m, debt / dt);
            m += add;
            debt -= add * dt;
        } else {
            debt = 0f;
        }

        if (m < 0.01f) {
            m = 0f;
        } else {
            m = offset + (1f - offset) * Math.min(1f, m);
        }

        out[0] = dirX * m;
        out[1] = dirY * m;
    }

    private float shape(float v) {
        return Math.signum(v)
                * (float) Math.pow(Math.abs(v) / V0, curve);
    }
}

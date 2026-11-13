package com.aure.totk.touch;

/** Default layout: x, y = centre in % of the screen; w, h in % of the screen width. */
final class TouchLayout {
    static final int TOG = 1, HOV = 2, THRU_BTN = 4, THRU_CAM = 8, NOSKEW = 16, OUT = 32, BELOW = 64, NOFIT = 128;
    static final class Btn {
        final String label; final float x, y, w, h, rp; final int bit, flags, icon;
        int sides; float rot;   // sides: 0 = rounded rectangle, 6..9 = polygon; rot in degrees
        Btn(String label, float x, float y, float w, float h, float rp, int bit, int flags, int icon) {
            this.label = label; this.x = x; this.y = y; this.w = w; this.h = h; this.rp = rp; this.bit = bit; this.flags = flags; this.icon = icon;
        }
        Btn shape(int sides, float rot) { this.sides = sides; this.rot = rot; return this; }
    }
    private static Btn b(String l, double x, double y, double w, double h, double rp, int bit, int flags, int icon) {
        return new Btn(l, (float) x, (float) y, (float) w, (float) h, (float) rp, bit, flags, icon);
    }
    /* bit = libnx Npad bit: A0 B1 X2 Y3 L3 4 R3 5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; -1 = no button yet */
    static Btn[] defaults() {
        final int O = OUT;
        return new Btn[] {
            b("ZL", 8.1, 7.7, 16.2, 6.9, 80, 8, 0, 0), b("L", 6.25, 27.7, 12.5, 8.5, 40, 6, 0, 0),
            b("\u2191", 25, 5.55, 5, 3.5, 100, 13, NOSKEW, 0), b("\u2193", 25, 16.65, 5, 3.5, 100, 15, NOSKEW, 0),
            b("\u2190", 20, 11.1, 3.5, 5, 100, 12, NOSKEW, 0), b("\u2192", 30, 11.1, 3.5, 5, 100, 14, NOSKEW, 0),
            b("Camera", 35, 11.1, 3.2, 4.4, 100, -1, O, 0xFFA8D020),
            b("\u2013", 67.5, 5.55, 6, 5, 100, 11, 0, 0), b("+", 75, 5.55, 6, 5, 100, 10, 0, 0),
            b("Autobuild", 77.5, 22.2, 3.2, 4.4, 100, -1, O, 0xFFA78BFA),
            b("Ascend", 2.5, 44.4, 2.8, 4.2, 100, -1, O, 0xFF8FC0A8), b("Recall", 12.5, 44.4, 2.8, 4.2, 100, -1, O, 0xFFF0A030),
            b("Ultrahand", 7.5, 49.95, 2.8, 4.3, 100, -1, O, 0xFFFF5A3D), b("Fuse", 2.5, 55.5, 2.8, 4.2, 100, -1, O, 0xFF38D6D0),
            b("L3", 45, 88.8, 5.5, 5.5, 100, 4, 0, 0), b("R3", 55, 88.8, 5.5, 5.5, 100, 5, 0, 0),
            b("R", 93.75, 27.7, 12.5, 8.5, 40, 7, 0, 0), b("ZR", 91.9, 7.7, 16.2, 6.9, 80, 9, 0, 0),
            b("Surf", 65, 88.8, 8.5, 5.3, 45, -1, 0, 0),
            b("A", 95, 66.668, 7.5, 7.5, 100, 0, 0, 0),
            b("B", 92.5, 83.334, 7.5, 7.5, 100, 1, TOG | THRU_BTN | THRU_CAM, 0),
            b("X", 87.5, 66.668, 7.5, 7.5, 100, 2, HOV | THRU_BTN | THRU_CAM, 0), b("Y", 85, 83.334, 7.5, 7.5, 100, 3, 0, 0),
            b("Gyro", 96.5, 44.4, 7, 3.5, 100, -1, TOG, 0), b("Gyro", 25, 27.778, 8, 3.5, 100, -1, HOV, 0),
        };
    }
}

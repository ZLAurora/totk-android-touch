package com.aure.totk.companion;

/** JNI_OnLoad in runtime/companion_jni.c binds these names and signatures with RegisterNatives. */
public final class CompanionNative implements DataSource {
    private static boolean loaded;
    static {
        try { System.loadLibrary("totk"); loaded = true; }
        catch (UnsatisfiedLinkError ignored) { loaded = false; }
    }
    private static native boolean nativeEnabled();
    private static native boolean nativeCoverEnabled();
    private static native String nativeCoverSetting();
    private static native void nativeCoverLog(String message);
    /** Uses the runtime's native stderr, already redirected into <game>/totk.log; Java Log/System.err do not. */
    public static boolean logCover(String message) {
        if (!loaded) return false;
        try { nativeCoverLog(message); return true; }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return false; }
    }
    /** Cover defaults on after native initialization; only exact TOTK_COMPANION_COVER=0 opts out. */
    public static boolean isCoverEnabled() {
        if (!loaded) return false;
        try { return nativeCoverEnabled(); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return false; }
    }
    /** Raw startup request for diagnostics, read from the same native environment as the enablement query. */
    public static String coverSetting() {
        if (!loaded) return "unavailable";
        try { return nativeCoverSetting(); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return "unavailable"; }
    }
    /** Call after NativeActivity.onCreate has initialized the runtime options. */
    public static boolean isEnabled() {
        if (!loaded) return false;
        try { return nativeEnabled(); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return false; }
    }
    @Override public boolean enabled() { return isEnabled(); }
    public static native String snapshot(int detail);
    public static native boolean action(int kind, int arg);
    /** Item action for the game's frame end: seq > 0, or -1 busy, -2 refused now, -3 unsupported, -4 invalid. */
    public static native int request(String action, String category, int slot, String actor, int partition, long pouchGen);
    /** {"seq":n,"state":"pending|done|refused|failed","reason":"…"} for the latest request, null for any other seq. */
    public static native String result(int seq);

    @Override public String read(int detail) {
        if (!loaded) return "{\"schema\":1,\"seq\":0,\"status\":\"unsupported\"}";
        try { return snapshot(detail); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return read(detail); }
    }
    @Override public boolean send(int kind, int arg) {
        if (!loaded) return false;
        try { return action(kind, arg); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return false; }
    }
    @Override public int submit(String action, String category, int slot, String actor, int partition, long pouchGen) {
        if (!loaded) return -3;
        try { return request(action, category, slot, actor, partition, pouchGen); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return -3; }
    }
    @Override public String outcome(int seq) {
        if (!loaded) return null;
        try { return result(seq); }
        catch (UnsatisfiedLinkError unavailable) { loaded = false; return null; }
    }
}

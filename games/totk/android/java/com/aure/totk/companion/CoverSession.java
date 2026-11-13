package com.aure.totk.companion;

/** Main-thread cover lifecycle decisions, independent of the Android activity/window machinery. A refused launch
 * stays refused until a new companion activation; an unexpected finish gets one restart, never a retry loop. */
public final class CoverSession {
    public interface Host {
        boolean start(int displayId, long token);
        /** Also cancels a pending start. Must be idempotent for late launch completion. */
        void finish(long token);
    }
    private static long nextToken;
    private final boolean enabled;
    private final Host host;
    private boolean closed, resumed, refused, restarted;
    private int displayId = -1;
    private long token;

    public CoverSession(boolean enabled, Host host) { this.enabled = enabled; this.host = host; }

    public void refresh(int kind, int id, boolean ownerResumed) {
        resumed = ownerResumed;
        if (closed) return;
        if (!enabled || kind != DisplayClass.BUILT_IN || id < 0) { dismiss(); return; }
        if (displayId != id) { dismiss(); displayId = id; }
        start();
    }

    /** Pausing alone does not uncover the panel, but cannot launch an activity in the background. */
    public void ownerResumed(boolean value) { resumed = value; if (value) start(); }

    private void start() {
        if (closed || !enabled || !resumed || displayId < 0 || token != 0 || refused) return;
        long launching = ++nextToken;
        token = launching;
        boolean accepted;
        try { accepted = host.start(displayId, launching); }
        catch (RuntimeException unavailable) { accepted = false; }
        if (token != launching) {
            // A synchronous callback can dismiss/close while the host is still accepting the launch.
            if (accepted) host.finish(launching);
            return;
        }
        if (!accepted) {
            token = 0;
            refused = true;
            host.finish(launching);
        }
    }

    public void hostFinished(long finished) {
        if (finished == 0 || token != finished) return;
        token = 0;
        if (closed || displayId < 0 || restarted) { refused = true; return; }
        restarted = true;
        start();
    }

    /** A launch accepted by startActivity can still land on the wrong display; do not treat that as a crash/restart. */
    public void startRefused(long refusedToken) {
        if (token != refusedToken || token == 0) return;
        token = 0; refused = true;
        host.finish(refusedToken);
    }
    /** Whether the current start is the sole recovery attempt, for the Android host's diagnostic line. */
    public boolean isRestart() { return restarted; }

    public void dismiss() {
        long previous = token;
        token = 0; displayId = -1; refused = false; restarted = false;
        if (previous != 0) host.finish(previous);
    }

    public void close() { closed = true; dismiss(); }
}

package com.aure.totk.companion;

import java.util.ArrayList;
import java.util.List;

/** Host driver for tests/test_android_cover.py; reports only calls made to the cover host. */
public final class CoverProbe {
    private static final class RecordingHost implements CoverSession.Host {
        final List<String> calls = new ArrayList<>();
        boolean accepted = true, throwsOnStart;
        Runnable duringStart;
        @Override public boolean start(int displayId, long token) {
            calls.add("start:" + displayId + ":" + token);
            if (duringStart != null) { Runnable callback = duringStart; duringStart = null; callback.run(); }
            if (throwsOnStart) throw new IllegalStateException("display unavailable");
            return accepted;
        }
        @Override public void finish(long token) { calls.add("finish:" + token); }
    }

    private static CoverSession session(boolean enabled, RecordingHost host) {
        return new CoverSession(enabled, host);
    }
    private static void run(String scenario) {
        RecordingHost host = new RecordingHost();
        CoverSession cover = session(true, host);
        switch (scenario) {
            case "off":
                cover = session(false, host);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.ownerResumed(true);
                break;
            case "none":
                cover.refresh(DisplayClass.NONE, -1, true);
                cover.refresh(DisplayClass.BUILT_IN, -1, true);
                break;
            case "external":
                cover.refresh(DisplayClass.EXTERNAL, 4, true);
                break;
            case "not-resumed":
                cover.refresh(DisplayClass.BUILT_IN, 4, false);
                break;
            case "initial-and-keep":
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            case "display-change":
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.BUILT_IN, 7, true);
                break;
            case "dismiss-owner-stop-return":
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.dismiss();
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.dismiss();
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            case "display-removed":
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.NONE, -1, true);
                break;
            case "display-became-external":
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.EXTERNAL, 4, true);
                break;
            case "pause-keeps-cover":
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.ownerResumed(false);
                cover.refresh(DisplayClass.BUILT_IN, 4, false);
                break;
            case "close": {
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                long stopped = token(host, 0);
                cover.close();
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.ownerResumed(true);
                cover.hostFinished(stopped);
                break;
            }
            case "stale-finish": {
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                long stale = token(host, 0);
                cover.refresh(DisplayClass.BUILT_IN, 7, true);
                cover.hostFinished(stale);
                cover.startRefused(stale);
                break;
            }
            case "one-restart": {
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                long first = token(host, 0);
                cover.hostFinished(first);
                long restart = token(host, 1);
                cover.hostFinished(restart);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            }
            case "start-refused": {
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                long refused = token(host, 0);
                cover.startRefused(refused);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            }
            case "refused":
                host.accepted = false;
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            case "throws":
                host.throwsOnStart = true;
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            case "refused-reset-by-display-change":
                host.accepted = false;
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                host.accepted = true;
                cover.refresh(DisplayClass.BUILT_IN, 7, true);
                break;
            case "pause-and-resume": {
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                long first = token(host, 0);
                cover.ownerResumed(false);
                cover.hostFinished(first);
                cover.refresh(DisplayClass.BUILT_IN, 4, false);
                cover.ownerResumed(true);
                break;
            }
            case "dismiss-during-start":
                host.duringStart = cover::dismiss;
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            case "close-during-start":
                host.duringStart = cover::close;
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                break;
            case "tokens-global": {
                cover.refresh(DisplayClass.BUILT_IN, 4, true);
                RecordingHost secondHost = new RecordingHost();
                session(true, secondHost).refresh(DisplayClass.BUILT_IN, 4, true);
                host.calls.addAll(secondHost.calls);
                break;
            }
            default: throw new IllegalArgumentException("unknown scenario: " + scenario);
        }
        System.out.println(String.join(" ", host.calls));
    }
    private static long token(RecordingHost host, int startNumber) {
        int found = 0;
        for (String call : host.calls) if (call.startsWith("start:")) {
            if (found++ == startNumber) return Long.parseLong(call.substring(call.lastIndexOf(':') + 1));
        }
        throw new AssertionError("missing start call " + startNumber);
    }
    public static void main(String[] args) {
        for (String scenario : args) run(scenario);
    }
}

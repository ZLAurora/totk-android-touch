package com.aure.totk;

import android.app.NativeActivity;
import android.os.Bundle;
import android.os.HandlerThread;
import com.aure.totk.companion.CompanionNative;
import com.aure.totk.companion.DataSource;
import com.aure.totk.companion.PresentationController;
import com.aure.totk.companion.SnapshotPoller;

/** Runs production activity/source/poller code with a host JVM and lifecycle/queue observers. */
public final class CompanionGateProbe {
    private static native void option(String value);
    private static native int count(int kind);
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        String setting = "unset".equals(args[0]) ? null : args[0];
        boolean enabled = !"0".equals(setting);
        // JNI_OnLoad/class initialization must not freeze the option before NativeActivity loads totk.env.
        check(CompanionNative.isEnabled(), "unset option must default on before native initialization");
        NativeActivity.initialize = () -> option(setting);
        TotkActivity activity = new TotkActivity();
        activity.onCreate(new Bundle());
        check(PresentationController.created == (enabled ? 1 : 0), "gate must precede controller construction");
        check(CompanionNative.isEnabled() == enabled, "option must reflect native initialization");
        DataSource source = new CompanionNative();
        check(source.enabled() == enabled, "source must expose the runtime gate");
        for (int i = 0; i < 4; i++) check(count(i) == 0, "gate must not inspect guest state or press buttons");
        String coverSetting = System.getenv("TOTK_COMPANION_COVER");
        if (coverSetting == null) coverSetting = "unset";
        check(CompanionNative.coverSetting().equals(coverSetting), "cover diagnostics must retain the raw request");
        boolean coverEnabled = enabled && !"0".equals(coverSetting);
        check(CompanionNative.isCoverEnabled() == coverEnabled, "cover must default on with exact-zero opt-out");
        check(CompanionNative.logCover("cover requested=" + coverSetting + " effective=" + coverEnabled + " reason=host_probe"),
                "cover diagnostics must bind native stderr even when the companion is disabled");
        // A live recents tile can lose/regain focus without onPause/onResume.
        // Only a single display admits that extra pause facet.
        for (int kind = 0; kind < 3; kind++) {
            PresentationController.secondScreenKind = kind;
            activity.onWindowFocusChanged(false);
            check(count(6) == (kind == 0 ? 2 : 0), "focus-loss bridge must preserve multi-resume");
            activity.onWindowFocusChanged(true);
            check(count(6) == (kind == 0 ? 3 : 1), "focus return must wake without onResume");
        }
        check(count(5) == 6, "all focus transitions must reach the native bridge");
        activity.onTopResumedActivityChanged(false);
        activity.onTopResumedActivityChanged(true);
        check(count(5) == 6, "controller routing must not change the pause-focus facet");

        check((CompanionNative.snapshot(0) != null) == enabled, "direct live snapshot must honor gate");
        check((source.read(1) != null) == enabled, "source full snapshot must honor gate");
        check(CompanionNative.action(1, 10) == enabled, "direct action must honor gate");
        check(source.send(1, 11) == enabled, "source action must honor gate");
        check(!CompanionNative.action(2, 10), "unknown action must remain rejected");
        check(!CompanionNative.action(1, 16), "out-of-range button must remain rejected");
        for (int i = 0; i < 3; i++) check(count(i) == (enabled ? 4 : 0), "disabled JNI must not read guest state");
        check(count(3) == (enabled ? 2 : 0), "disabled JNI must not synthesize input");

        SnapshotPoller poller = new SnapshotPoller(source, (snapshot, stale) -> {});
        poller.start(); poller.start();
        check(HandlerThread.started == (enabled ? 1 : 0), "disabled poller must not start a worker");
        poller.stop();
        check(HandlerThread.stopped == HandlerThread.started, "poller teardown must stop its worker");
        int started = HandlerThread.started;
        int snapshots = count(0), presses = count(3);
        option("0");
        poller.start(); poller.action(1, 10);
        check(HandlerThread.started == started, "direct poller restart must not bypass off state");
        check(source.read(0) == null && !source.send(1, 10), "source must recheck the native option");
        check(count(0) == snapshots && count(3) == presses, "off state must not perform native work");
        option(null);
        check(source.enabled(), "off state must not be cached either");

        DataSource fixture = new DataSource() {
            @Override public String read(int detail) { return null; }
            @Override public boolean send(int kind, int arg) { return false; }
        };
        SnapshotPoller preview = new SnapshotPoller(fixture, (snapshot, stale) -> {});
        preview.start();
        check(HandlerThread.started == started + 1, "preview sources must retain default-on behavior");
        preview.stop();
        NativeActivity.destroy = () -> check(PresentationController.closed == PresentationController.created,
                "controller teardown must precede native destruction");
        activity.onDestroy();
        System.out.println("pass: companion gate " + args[0]);
    }
}

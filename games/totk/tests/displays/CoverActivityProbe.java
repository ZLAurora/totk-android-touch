package com.aure.totk.companion;

import android.content.Intent;
import android.os.Bundle;
import android.view.Display;
import java.util.ArrayList;
import java.util.List;

/** Drives production CoverActivity callbacks through minimal host framework doubles. */
public final class CoverActivityProbe {
    private static final class Events implements CoverActivity.Listener {
        final List<String> calls = new ArrayList<>();
        public void attached() { calls.add("attached"); }
        public void finished(long token) { calls.add("finished:" + token); }
        public void refused(long token) { calls.add("refused:" + token); }
        public void topResumedChanged(long token, boolean top) { calls.add("top:" + token + ":" + top); }
    }
    private static Intent intent(long token) { return new Intent().putExtra(CoverActivity.TOKEN, token); }
    private static CoverActivity create(long token, int displayId, boolean restored) {
        CoverActivity activity = new CoverActivity();
        activity.setIntent(intent(token));
        activity.setDisplayForTest(new Display(displayId));
        activity.createForTest(restored ? new Bundle() : null);
        return activity;
    }
    private static void out(String label, Events events, CoverActivity activity) {
        System.out.println(label + " " + String.join(",", events.calls) + " finished=" + activity.isFinishedForTest());
    }
    public static void main(String[] args) {
        switch (args[0]) {
            case "canceled-pending": {
                Events events = new Events();
                CoverActivity.prepare(4, 101, events);
                CoverActivity.cancel(101);
                out("canceled", events, create(101, 4, false));
                break;
            }
            case "two-intents": {
                Events events = new Events();
                CoverActivity.prepare(4, 201, events);
                CoverActivity.cancel(201);
                CoverActivity.prepare(4, 202, events);
                CoverActivity stale = create(201, 4, false);
                out("stale", events, stale);
                CoverActivity next = create(202, 4, false); // A finishing instance must not be revived by a fake reset.
                stale.destroyForTest();
                out("next", events, next);
                break;
            }
            case "rebind": {
                Events events = new Events();
                CoverActivity.prepare(4, 301, events);
                CoverActivity first = create(301, 4, false);
                first.topResumedForTest(true);
                CoverActivity.prepare(4, 302, events);
                first.newIntentForTest(intent(302));
                CoverActivity.cancel(301);
                out("rebound", events, first);
                System.out.println("top301=" + CoverActivity.isTopResumed(301) + " top302=" + CoverActivity.isTopResumed(302));
                break;
            }
            case "stale-destroy": {
                Events events = new Events();
                CoverActivity.prepare(4, 311, events);
                CoverActivity stale = create(311, 4, false);
                CoverActivity.prepare(4, 312, events);
                CoverActivity current = create(312, 4, false);
                stale.destroyForTest();
                current.topResumedForTest(true);
                System.out.println("stale311=" + CoverActivity.isTopResumed(311) + " current312="
                        + CoverActivity.isTopResumed(312) + " events=" + events.calls);
                break;
            }
            case "restored": {
                Events events = new Events();
                out("restored", events, create(401, 4, true));
                System.out.println("events=" + events.calls);
                break;
            }
            case "displays": {
                Events events = new Events();
                CoverActivity.prepare(4, 501, events);
                out("right", events, create(501, 4, false));
                CoverActivity.prepare(4, 502, events);
                out("wrong", events, create(502, 9, false));
                break;
            }
            case "top-resumed": {
                Events events = new Events();
                CoverActivity.prepare(4, 601, events);
                CoverActivity activity = create(601, 4, false);
                activity.topResumedForTest(true);
                activity.topResumedForTest(false);
                activity.topResumedForTest(true);
                System.out.println("top601=" + CoverActivity.isTopResumed(601) + " events=" + events.calls);
                CoverActivity.cancel(601);
                System.out.println("afterCancel=" + CoverActivity.isTopResumed(601));
                CoverActivity.prepare(4, 602, events);
                CoverActivity stale = create(602, 4, false);
                CoverActivity.prepare(4, 603, events);
                create(603, 4, false);
                CoverActivity.cancel(602);
                stale.topResumedForTest(true);
                System.out.println("stale602=" + CoverActivity.isTopResumed(602) + " current603=" + CoverActivity.isTopResumed(603));
                System.out.println("afterStaleCallback=" + CoverActivity.isTopResumed(603) + " events=" + events.calls);
                break;
            }
            default: throw new IllegalArgumentException(args[0]);
        }
    }
}

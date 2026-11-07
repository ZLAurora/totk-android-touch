package com.aure.totk.setup;

/** Runs the production policy without Android or a fake install-state implementation. */
public final class ArtPolicyProbe {
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    public static void main(String[] args) {
        int combinations = 0;
        for (boolean second : new boolean[]{false, true}) {
            check(ArtPolicy.skipSetup(second, false) == !second, "automatic setup follows built-in display");
            check(!ArtPolicy.skipSetup(second, true), "explicit builds remain available on any device");
            for (ArtPolicy.State state : ArtPolicy.State.values()) {
                for (boolean declined : new boolean[]{false, true}) {
                    boolean present = state == ArtPolicy.State.DONE || state == ArtPolicy.State.OUTDATED;
                    check(ArtPolicy.offerBeforePlay(second, state, declined) == (second && !present && !declined),
                            "offer: " + second + " " + state + " " + declined);
                    check(ArtPolicy.updateAutomatically(second, state) == (second && state == ArtPolicy.State.OUTDATED),
                            "update: " + second + " " + state + " " + declined);
                    combinations++;
                }
            }
        }
        check(ArtPolicy.state(false, false, true, true) == ArtPolicy.State.DONE, "usable art is ready");
        check(ArtPolicy.state(false, false, true, false) == ArtPolicy.State.OUTDATED, "old art is retained");
        check(ArtPolicy.state(true, false, false, false) == ArtPolicy.State.SKIPPED, "skip is not a failure");
        check(ArtPolicy.state(false, true, false, false) == ArtPolicy.State.FAILED, "failed stage");
        check(ArtPolicy.state(false, false, false, false) == ArtPolicy.State.ABSENT, "missing files, even with stale done flag");
        check(ArtPolicy.state(true, true, true, true) == ArtPolicy.State.DONE, "actual assets outrank stale stage flags");
        check(ArtPolicy.state(false, true, true, false) == ArtPolicy.State.OUTDATED, "failed update retains usable old art");
        System.out.println(combinations + " combinations; asset-state precedence passed");
    }
}

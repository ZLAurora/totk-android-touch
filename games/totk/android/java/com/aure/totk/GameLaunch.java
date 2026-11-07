package com.aure.totk;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.os.Bundle;
import android.view.Display;
import com.aure.totk.companion.DisplayClass;
import com.aure.totk.companion.PresentationController;

/** Route only dual-panel devices to their main screen; keep Android's normal task/recents behavior otherwise. */
public final class GameLaunch {
    private GameLaunch() { }
    static final String OPEN_SETTINGS = "com.aure.totk.OPEN_SETTINGS";

    public static void start(Activity owner) { start(owner, false); }

    /** Return through the live game so Settings shares its task, rather than leaving a stale game task behind. */
    public static void startSettings(Activity owner) { start(owner, true); }

    private static void start(Activity owner, boolean settings) {
        Intent game = new Intent(owner, TotkActivity.class);
        if (settings) game.putExtra(OPEN_SETTINGS, true);
        Bundle options = null;
        if (PresentationController.deviceSecondScreen(owner) == DisplayClass.BUILT_IN) {
            game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            options = ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle();
        }
        owner.startActivity(game, options);
    }

    /** A launcher may bring an existing task to its own display without going through SetupActivity again. */
    static boolean onMainScreen(Activity owner) {
        Display current = owner.getDisplay();
        return current == null || current.getDisplayId() == Display.DEFAULT_DISPLAY
                || PresentationController.deviceSecondScreen(owner) != DisplayClass.BUILT_IN;
    }
}

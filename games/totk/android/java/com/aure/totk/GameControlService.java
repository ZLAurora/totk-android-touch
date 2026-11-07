package com.aure.totk;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

/** A short-lived foreground-menu binding keeps the default process out of Android's cached-app freezer while closing.
 * No game/runtime initialization: a bind racing with game exit may create this service in an otherwise empty process. */
public final class GameControlService extends Service {
    private final IBinder binder = new Binder();

    @Override public IBinder onBind(Intent intent) { return binder; }
}

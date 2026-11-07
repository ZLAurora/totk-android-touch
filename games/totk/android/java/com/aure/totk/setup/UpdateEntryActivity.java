package com.aure.totk.setup;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Only our game can enter this non-exported :setup activity after the player chooses Update now. */
public final class UpdateEntryActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        startActivity(new Intent(this, SetupActivity.class).setAction(Intent.ACTION_VIEW)
                .putExtra(SetupActivity.EXTRA_UPDATE, true)
                .putExtra(SetupActivity.EXTRA_UPDATE_AUTH, UpdateAuthorization.issue())
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
        finish();
    }
}

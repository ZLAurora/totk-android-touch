package com.aure.totk.setup;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import com.aure.totk.UpdateRules;
import java.io.File;

/** Explicit, non-exported PackageInstaller callback; no externally supplied update URL or version is accepted. */
public final class UpdateReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
            try {
                if (confirm == null) throw new IllegalStateException("Android did not provide an installation confirmation.");
                context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                return;
            } catch (RuntimeException failed) {
                result(context, "Update not installed: could not open Android’s installer. Try again next launch.");
            }
        } else if (status == PackageInstaller.STATUS_SUCCESS) {
            try { UpdateRules.clearPending(context.getFilesDir()); } catch (java.io.IOException ignored) { }
            result(context, "Updated to " + intent.getStringExtra("update_version") + ". Your saves and game data were kept.");
        } else {
            String why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            result(context, status == PackageInstaller.STATUS_FAILURE_ABORTED
                    ? "Update cancelled in Android’s installer. You can try again next launch."
                    : "Update not installed: " + (why == null ? "Android refused the installation." : why));
        }
        clean(context);
    }
    private static void result(Context context, String message) {
        Inputs.prefs(context).edit().putString("updateResult", message).commit();
    }
    static String takeResult(Context context) {
        String result = Inputs.prefs(context).getString("updateResult", "");
        if (!result.isEmpty()) Inputs.prefs(context).edit().remove("updateResult").apply();
        return result;
    }
    /** Leftovers of an interrupted update; files a running update in this process still uses are kept. */
    static void clean(Context context) {
        File[] files = context.getCacheDir().listFiles((dir, name) -> name.startsWith("totk-update-")
                && (name.endsWith(".apk") || name.endsWith(".sha256")) && !UpdateInstaller.owned(name));
        if (files != null) for (File file : files) file.delete();
    }
}

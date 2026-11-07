package com.aure.totk.setup;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import com.aure.totk.UpdateRules;
import com.aure.totk.Updates;
import com.aure.totk.UpdateTransfer;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** One foreground, cancellable download; PackageInstaller owns installation and its final confirmation. */
final class UpdateInstaller {
    interface Listener {
        void changed();
        void permission();
        void finished(String note);
    }
    static final int REQUEST_SOURCES = 908;
    private final Activity activity;
    private final Listener listener;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private UpdateTransfer transfer;
    private File apk, checksum;
    private volatile boolean cancelled, disposed;
    private boolean waitingPermission;
    private int session = -1;
    private volatile boolean committed;
    String version, status = "";
    private String expectedSha256;
    volatile long bytes, total;
    UpdateInstaller(Activity activity, Listener listener) { this.activity = activity; this.listener = listener; }
    void begin(String version) {
        this.version = version;
        cancelled = false;
        transfer = new UpdateTransfer();
        status = "Downloading update…";
        listener.changed();
        worker.execute(() -> {
            try {
                Updates.Source source = Updates.source(activity);
                apk = own(File.createTempFile("totk-update-", ".apk", activity.getCacheDir()));
                checksum = own(File.createTempFile("totk-update-", ".sha256", activity.getCacheDir()));
                transfer.download(source.base, source.override, source.asset(version, false), apk, 2L * 1024 * 1024 * 1024, (count, size) -> {
                    bytes = count; total = size;
                    // The menu refresh timer reads these; don't queue one UI message per chunk.
                });
                postStatus("Verifying download…");
                transfer.download(source.base, source.override, source.asset(version, true), checksum, 4096, (count, size) -> { });
                transfer.checkCancelled();
                transfer.finishDownloads();
                expectedSha256 = UpdateRules.sha256(new String(java.nio.file.Files.readAllBytes(checksum.toPath()), java.nio.charset.StandardCharsets.UTF_8), UpdateRules.asset(version));
                if (!expectedSha256.equals(Updates.digest(apk))) throw new IOException("The update checksum does not match. Nothing was installed.");
                verifyArchive();
                transfer.checkCancelled();
                activity.runOnUiThread(() -> {
                    if (cancelled || disposed) return;
                    if (!activity.getPackageManager().canRequestPackageInstalls()) {
                        waitingPermission = true;
                        listener.permission();
                    } else install();
                });
            } catch (Exception failed) { fail(failed); }
            finally { transfer.finishDownloads(); }
        });
    }
    private static byte[][] certificates(PackageInfo info) throws Exception {
        if (info == null || info.signingInfo == null) return null;
        Signature[] signatures = info.signingInfo.getApkContentsSigners();
        if (signatures == null) return null;
        byte[][] hashes = new byte[signatures.length][];
        for (int i = 0; i < hashes.length; i++) hashes[i] = MessageDigest.getInstance("SHA-256").digest(signatures[i].toByteArray());
        return hashes;
    }
    private void verifyArchive() throws Exception {
        PackageManager pm = activity.getPackageManager();
        PackageInfo own = pm.getPackageInfo(activity.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
        PackageInfo archive = pm.getPackageArchiveInfo(apk.getPath(), PackageManager.GET_SIGNING_CERTIFICATES);
        if (archive == null) throw new IOException("The downloaded file is not a valid APK.");
        String problem = UpdateRules.verify(own.packageName, own.getLongVersionCode(), certificates(own),
                archive.packageName, archive.getLongVersionCode(), certificates(archive));
        if (problem != null) throw new IOException(problem + " Nothing was installed.");
    }
    void openPermission() {
        try {
            activity.startActivityForResult(new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + activity.getPackageName())), REQUEST_SOURCES);
        } catch (RuntimeException failed) { fail(failed); }
    }
    void permissionReturned() {
        if (!waitingPermission || cancelled || disposed) return;
        waitingPermission = false;
        if (activity.getPackageManager().canRequestPackageInstalls()) install();
        else finish("Update not installed: permission was not allowed. You can try again next launch.");
    }
    private void install() {
        status = "Preparing Android installer…"; listener.changed();
        worker.execute(() -> {
            try {
                transfer.checkCancelled();
                // Recheck immediately before handing bytes to Android (also verifies a file kept during Settings).
                verifyArchive();
                PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                params.setAppPackageName(activity.getPackageName());
                params.setSize(apk.length());
                params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED);
                session = installer.createSession(params);
                try (PackageInstaller.Session target = installer.openSession(session)) {
                    try (FileInputStream in = new FileInputStream(apk);
                         OutputStream out = target.openWrite("base.apk", 0, apk.length())) {
                        transfer.copyVerified(in, out, expectedSha256);
                        target.fsync(out);
                    }
                    transfer.checkCancelled();
                    Intent callback = new Intent(activity, UpdateReceiver.class).setAction(activity.getPackageName() + ".UPDATE_INSTALL")
                            .putExtra("update_version", version);
                    PendingIntent result = PendingIntent.getBroadcast(activity, session, callback, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                    target.commit(result.getIntentSender());
                    committed = true;
                }
                cleanFiles();
                postStatus("Confirm the update in Android’s installer.");
            } catch (Exception failed) { fail(failed); }
        });
    }
    private void postStatus(String value) {
        activity.runOnUiThread(() -> { if (!cancelled && !disposed) { status = value; listener.changed(); } });
    }
    private void fail(Exception failed) {
        activity.runOnUiThread(() -> { if (!cancelled && !disposed) finish("Update not installed: " + (failed.getMessage() == null ? failed.toString() : failed.getMessage())); });
        cleanFiles();
        abandon();
    }
    void cancel() {
        if (cancelled) return;
        cancelled = true;
        if (transfer != null) transfer.cancel();
        worker.execute(() -> {
            if (session >= 0) {
                try { activity.getPackageManager().getPackageInstaller().abandonSession(session); } catch (RuntimeException ignored) { }
            }
            cleanFiles();
        });
        listener.finished("Update cancelled. You can try again next launch.");
    }
    private void finish(String note) { cancelWork(); listener.finished(note); }
    private void cancelWork() {
        cancelled = true;
        if (transfer != null) transfer.cancel();
        worker.execute(() -> { abandon(); cleanFiles(); });
    }
    private void abandon() {
        if (session >= 0 && !committed) {
            try { activity.getPackageManager().getPackageInstaller().abandonSession(session); } catch (RuntimeException ignored) { }
        }
    }
    private void cleanFiles() {
        if (apk != null) { apk.delete(); release(apk); }
        if (checksum != null) { checksum.delete(); release(checksum); }
    }
    /** Files of a running update in this process: another menu instance's leftover sweep must not delete them. */
    private static final java.util.Set<String> OWNED = new java.util.HashSet<>();
    private static File own(File file) { synchronized (OWNED) { OWNED.add(file.getName()); } return file; }
    private static void release(File file) { synchronized (OWNED) { OWNED.remove(file.getName()); } }
    static boolean owned(String name) { synchronized (OWNED) { return OWNED.contains(name); } }
    void dispose() {
        disposed = true;
        if (!cancelled) cancelWork();
        worker.shutdown();
    }
}

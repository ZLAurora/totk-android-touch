package com.aure.totk;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** No automatic GET, redirect, retry, telemetry or body read: only one bounded HEAD on a validated connection. */
public final class Updates {
    private Updates() { }
    public static final class Source {
        public final URI base;
        public final boolean override;
        Source(URI base, boolean override) { this.base = base; this.override = override; }
        public URI latest() { return base.resolve(UpdateRules.PROJECT + "/releases/latest"); }
        public URI asset(String version, boolean checksum) {
            return base.resolve(UpdateRules.PROJECT + "/releases/download/v" + version + "/" + UpdateRules.asset(version) + (checksum ? ".sha256" : ""));
        }
    }
    public static Source source(Context context) throws IOException {
        File override = new File(context.getFilesDir(), "update-source.txt");
        if (!override.exists()) return new Source(URI.create("https://github.com"), false);
        try {
            URI uri = new URI(UpdateRules.read(context.getFilesDir(), "update-source.txt"));
            if ((!"http".equals(uri.getScheme()) && !"https".equals(uri.getScheme())) || uri.getHost() == null
                    || uri.getRawUserInfo() != null || uri.getRawQuery() != null || uri.getRawFragment() != null
                    || !(uri.getRawPath().isEmpty() || uri.getRawPath().equals("/"))) throw new IOException("Invalid private update source");
            return new Source(uri, true);
        } catch (java.net.URISyntaxException malformed) { throw new IOException("Invalid private update source", malformed); }
    }
    public static String version(Context context) {
        try { return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (android.content.pm.PackageManager.NameNotFoundException impossible) { return ""; }
    }
    public static boolean online(Context context) {
        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
        NetworkCapabilities caps = manager == null ? null : manager.getNetworkCapabilities(manager.getActiveNetwork());
        return caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
    }
    public static void check(Context context, Consumer<String> result) {
        Context app = context.getApplicationContext();
        Handler main = new Handler(Looper.getMainLooper());
        AtomicBoolean finished = new AtomicBoolean();
        UpdateTransfer transfer = new UpdateTransfer();
        // Includes DNS, connect and read time, not merely each socket operation. A late worker never posts a prompt.
        Runnable deadline = () -> { if (finished.compareAndSet(false, true)) { transfer.cancel(); result.accept(null); } };
        main.postDelayed(deadline, 5000);
        Thread worker = new Thread(() -> {
            String answer = null;
            try {
                Source source = source(app);
                answer = UpdateRules.checkAndStore(app.getFilesDir(), UpdateRules.enabled(app.getFilesDir()), online(app), version(app), app.getPackageName(),
                        source.override, source.base, () -> {
                            HttpURLConnection connection = transfer.connect(source.latest(), "HEAD", 2400);
                            try {
                                int status = connection.getResponseCode();
                                transfer.checkCancelled();
                                return status == 302 ? connection.getHeaderField("Location") : null;
                            } finally { transfer.release(connection); }
                        });
            } catch (IOException | RuntimeException ignored) { }
            String value = answer;
            main.post(() -> { if (finished.compareAndSet(false, true)) { main.removeCallbacks(deadline); result.accept(value); } });
        }, "update-check");
        worker.setDaemon(true);
        worker.start();
    }
    public static String digest(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new java.io.FileInputStream(file)) {
            byte[] bytes = new byte[128 * 1024]; int n;
            while ((n = in.read(bytes)) != -1) digest.update(bytes, 0, n);
        }
        return hex(digest.digest());
    }
    public static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) text.append(Character.forDigit((b >>> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return text.toString();
    }
}

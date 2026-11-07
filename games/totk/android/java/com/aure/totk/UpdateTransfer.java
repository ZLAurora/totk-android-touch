package com.aure.totk;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** SDK-independent HTTP transport. One deadline covers both assets and all their redirects, not each socket read. */
public final class UpdateTransfer implements AutoCloseable {
    private final long overallNanos, progressNanos, minimumBytes;
    private volatile String failure;
    private HttpURLConnection active;
    private ScheduledExecutorService watchdog;
    private long started, windowStarted, windowBytes;
    public UpdateTransfer() { this(10 * 60_000L, 30_000L, 64 * 1024L); }
    /** Shorter bounds for deterministic host transport tests; production always uses the no-argument constructor. */
    UpdateTransfer(long overallMillis, long progressMillis, long minimumBytes) {
        if (overallMillis <= 0 || progressMillis <= 0 || minimumBytes <= 0) throw new IllegalArgumentException("Invalid download bounds");
        overallNanos = TimeUnit.MILLISECONDS.toNanos(overallMillis);
        progressNanos = TimeUnit.MILLISECONDS.toNanos(progressMillis);
        this.minimumBytes = minimumBytes;
    }
    public void cancel() { cancel("Update cancelled."); }
    private void cancel(String reason) {
        HttpURLConnection connection;
        synchronized (this) {
            if (failure == null) failure = reason;
            connection = active;
        }
        // Do not run on the download worker: aborts a blocked connect/header/body read, including a slow drip.
        if (connection != null) connection.disconnect();
    }
    public synchronized void checkCancelled() throws IOException {
        expire(System.nanoTime());
        if (failure != null) throw new IOException(failure);
    }
    HttpURLConnection connect(URI uri, String method, int timeout) throws IOException {
        checkCancelled();
        HttpURLConnection connection = (HttpURLConnection) uri.toURL().openConnection();
        synchronized (this) {
            checkCancelled();
            active = connection;
            if (watchdog != null) {
                long now = System.nanoTime();
                long remaining = Math.min(overallNanos - (now - started), progressNanos - (now - windowStarted));
                timeout = (int) Math.max(1, Math.min(timeout, TimeUnit.NANOSECONDS.toMillis(remaining)));
            }
        }
        connection.setInstanceFollowRedirects(false);
        connection.setConnectTimeout(timeout); connection.setReadTimeout(timeout);
        connection.setRequestProperty("User-Agent", "TotK-Native-Updater");
        connection.setRequestMethod(method);
        connection.setUseCaches(false);
        checkCancelled();
        return connection;
    }
    void release(HttpURLConnection connection) {
        synchronized (this) { if (active == connection) active = null; }
        connection.disconnect();
    }
    private synchronized void beginAsset() throws IOException {
        checkCancelled();
        long now = System.nanoTime();
        if (watchdog == null) {
            started = now;
            watchdog = Executors.newSingleThreadScheduledExecutor(run -> {
                Thread thread = new Thread(run, "update-download-watchdog");
                thread.setDaemon(true);
                return thread;
            });
            long tick = Math.max(1, Math.min(250, TimeUnit.NANOSECONDS.toMillis(Math.min(overallNanos, progressNanos)) / 4));
            watchdog.scheduleWithFixedDelay(this::watch, tick, tick, TimeUnit.MILLISECONDS);
        }
        // A tiny checksum must not inherit the APK's earlier fast progress. Redirects do not reset either window.
        windowStarted = now;
        windowBytes = 0;
    }
    private void watch() {
        String reason;
        synchronized (this) {
            if (watchdog == null || failure != null) return;
            expire(System.nanoTime());
            reason = failure;
        }
        if (reason != null) cancel(reason);
    }
    private void expire(long now) {
        if (watchdog == null || failure != null) return;
        if (now - started >= overallNanos) failure = "The update download exceeded its time limit. Try again on a faster connection.";
        else if (now - windowStarted >= progressNanos) {
            if (windowBytes < minimumBytes) failure = "The update download stopped because the connection was too slow. Try again on a faster connection.";
            else { windowStarted = now; windowBytes = 0; }
        }
    }
    private synchronized void progressed(int bytes) { windowBytes += bytes; }
    /** Stop the download watchdog before checksum hashing / Android Settings. Cancellation remains available. */
    public synchronized void finishDownloads() {
        if (watchdog != null) { watchdog.shutdownNow(); watchdog = null; }
    }
    public interface Progress { void changed(long bytes, long total); }
    public void download(URI source, boolean override, URI start, File target, long maxBytes, Progress progress) throws IOException {
        beginAsset();
        boolean complete = false;
        try {
            URI uri = start;
            for (int redirects = 0; redirects <= 8; redirects++) {
                checkCancelled();
                if (!UpdateRules.allowedDownload(uri, source, override)) throw new IOException("Unsafe update download address.");
                HttpURLConnection connection = connect(uri, "GET", 15000);
                try {
                    int status = connection.getResponseCode();
                    checkCancelled();
                    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                        String location = connection.getHeaderField("Location");
                        if (location == null) throw new IOException("Missing download redirect.");
                        try { uri = uri.resolve(location); } catch (IllegalArgumentException malformed) { throw new IOException("Invalid download redirect.", malformed); }
                        continue;
                    }
                    if (status != 200) throw new IOException("Update download failed (HTTP " + status + ").");
                    long total = connection.getContentLengthLong(), count = 0;
                    if (total > maxBytes) throw new IOException("Update download is too large.");
                    try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(target)) {
                        byte[] bytes = new byte[128 * 1024];
                        int n;
                        while ((n = in.read(bytes)) != -1) {
                            checkCancelled(); count += n;
                            if (count > maxBytes) throw new IOException("Update download is too large.");
                            out.write(bytes, 0, n); progressed(n); progress.changed(count, total);
                        }
                        checkCancelled();
                        if (total >= 0 && count != total) throw new IOException("The update download was incomplete.");
                    }
                    complete = true;
                    return;
                } finally { release(connection); }
            }
            throw new IOException("Too many update download redirects.");
        } catch (IOException failed) {
            // Prefer the watchdog's useful reason to the socket-closed exception caused by its disconnect.
            checkCancelled();
            throw failed;
        } finally { if (!complete) Files.deleteIfExists(target.toPath()); }
    }
    /** Hash precisely the bytes handed to the PackageInstaller session, not a separate earlier read of the file. */
    public void copyVerified(InputStream in, OutputStream out, String expected) throws IOException {
        MessageDigest digest;
        try { digest = MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException missing) { throw new IOException("SHA-256 is unavailable.", missing); }
        byte[] bytes = new byte[128 * 1024]; int n;
        while ((n = in.read(bytes)) != -1) {
            checkCancelled();
            out.write(bytes, 0, n);
            digest.update(bytes, 0, n);
        }
        checkCancelled();
        StringBuilder actual = new StringBuilder(64);
        for (byte b : digest.digest()) actual.append(Character.forDigit((b >>> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        if (!actual.toString().equals(expected)) throw new IOException("The update changed before installation. Nothing was installed.");
    }
    @Override public void close() { finishDownloads(); cancel(); }
}

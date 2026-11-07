package com.aure.totk.setup;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;

/** One descriptor/record lock per private directory in this JVM. In particular, closing a probe must never close
 * another descriptor for the same inode: POSIX locks are process-owned and that would release a save swap's lock.
 * The game's and standalone mover's OFD locks conflict with this record lock. No Android types: host-tested. */
final class GameLock {
    enum State { FREE, GAME, ERROR }
    static final class Result {
        final State state;
        final String reason;
        Result(State state, String reason) { this.state = state; this.reason = reason; }
        boolean free() { return state == State.FREE; }
    }
    private static final Object MUTEX = new Object();
    private static final Map<String, Owner> owners = new HashMap<>();
    private static final Map<String, Long> launching = new HashMap<>();
    interface Logger { void log(String message); }
    static volatile Logger logger = message -> System.err.println("TotkSetup: " + message);
    private static final class Owner {
        final String key;
        final RandomAccessFile file;
        final FileLock lock;
        int references = 1;
        Owner(String key, RandomAccessFile file, FileLock lock) { this.key = key; this.file = file; this.lock = lock; }
    }
    static final class Hold implements Closeable {
        final Result result;
        private Owner owner;
        Hold(Result result, Owner owner) { this.result = result; this.owner = owner; }
        void requireFree() throws IOException {
            if (result.state == State.GAME) throw new SaveManager.Invalid("game_running", result.reason);
            if (result.state == State.ERROR) throw new IOException("Game lock: " + result.reason);
        }
        @Override public void close() throws IOException {
            synchronized (MUTEX) {
                Owner held = owner;
                owner = null;
                if (held == null || --held.references != 0) return;
                owners.remove(held.key);
                try { held.lock.release(); } finally { held.file.close(); }
            }
        }
    }
    private static Result error(String operation, Exception failed) {
        String detail = operation + ": " + failed.getClass().getName() + ": " + failed.getMessage();
        Throwable cause = failed.getCause();
        if (cause != null) detail += " (" + cause.getClass().getName() + ": " + cause.getMessage() + ")";
        logger.log("game decision=ERROR " + detail);
        return new Result(State.ERROR, detail);
    }
    static Hold hold(File filesDir) {
        synchronized (MUTEX) {
            RandomAccessFile file = null;
            try {
                File state = new File(filesDir, "state");
                if (!state.isDirectory() && !state.mkdirs() && !state.isDirectory()) throw new IOException("cannot create " + state);
                String key = new File(state, "game.lock").getCanonicalPath();
                Owner held = owners.get(key);
                if (held != null) {
                    held.references++;
                    return new Hold(new Result(State.FREE, "lock=shared-setup references=" + held.references), held);
                }
                file = new RandomAccessFile(key, "rw");
                FileLock lock = file.getChannel().tryLock();
                if (lock == null) {
                    file.close(); file = null;
                    logger.log("game decision=GAME branch=lock lock=contended (tryLock=null; external OFD/POSIX owner)");
                    return new Hold(new Result(State.GAME, "lock=contended"), null);
                }
                held = new Owner(key, file, lock);
                owners.put(key, held);
                return new Hold(new Result(State.FREE, "lock=acquired"), held);
            } catch (IOException | OverlappingFileLockException | SecurityException failed) {
                if (file != null) try { file.close(); } catch (IOException close) { failed.addSuppressed(close); }
                return new Hold(error("open/tryLock state/game.lock", failed), null);
            }
        }
    }
    /** A bare default process is not proof of a running game. The marker bridges Activity launch to OFD acquisition.
     * A local launch has four seconds to get a process, matching the native lock wait; abandoned markers are removed. */
    static Result check(File filesDir, boolean defaultProcessAlive) {
        synchronized (MUTEX) {
            try (Hold held = hold(filesDir)) {
                if (held.result.state == State.ERROR) return new Result(State.ERROR, held.result.reason + " marker=unchecked");
                File marker = new File(filesDir, "state/game-starting");
                String key = marker.getCanonicalPath();
                boolean present;
                try { Files.readAttributes(marker.toPath(), java.nio.file.attribute.BasicFileAttributes.class); present = true; }
                catch (java.nio.file.NoSuchFileException missing) { present = false; }
                Long deadline = launching.get(key);
                boolean pending = deadline != null && System.nanoTime() < deadline;
                String markerState = present ? new String(Files.readAllBytes(marker.toPath()), StandardCharsets.US_ASCII).trim() : "absent";
                if (!present || !pending) launching.remove(key);
                if (present && !defaultProcessAlive && !pending) {
                    Files.delete(marker.toPath());
                    markerState = "stale-removed " + markerState;
                    present = false;
                }
                Result result = held.result;
                if (result.free() && present && (defaultProcessAlive || pending)) result = new Result(State.GAME, "branch=start-marker");
                String reason = result.reason + " marker=" + markerState + " defaultProcessAlive=" + defaultProcessAlive + " launchPending=" + pending;
                if (result.state != State.FREE) logger.log("game decision=" + result.state + " " + reason);
                return new Result(result.state, reason);
            } catch (IOException | SecurityException failed) { return error("inspect/remove game-starting or release game.lock", failed); }
        }
    }
    /** Publish before calling Android. The same mutex excludes marker inspection until startActivity has returned. */
    static void start(File filesDir, long time, int pid, Runnable startActivity) throws IOException {
        synchronized (MUTEX) {
            File state = new File(filesDir, "state");
            if (!state.isDirectory() && !state.mkdirs() && !state.isDirectory()) throw new IOException("cannot create " + state);
            File marker = new File(state, "game-starting");
            String key = marker.getCanonicalPath();
            Files.deleteIfExists(new File(state, "close-request").toPath());
            Files.deleteIfExists(new File(state, "close-response").toPath());
            Files.write(marker.toPath(), ("time=" + time + " pid=" + pid + "\n").getBytes(StandardCharsets.US_ASCII));
            launching.put(key, System.nanoTime() + 4_000_000_000L);
            logger.log("game start marker written time=" + time + " pid=" + pid);
            try { startActivity.run(); }
            catch (RuntimeException failed) { launching.remove(key); Files.deleteIfExists(marker.toPath()); throw failed; }
        }
    }
    /** Private cross-process close transport: unlike a SINGLE_TASK activity intent, it cannot destroy the menu. */
    static void requestClose(File filesDir, String id) throws IOException {
        synchronized (MUTEX) {
            File state = new File(filesDir, "state");
            Files.deleteIfExists(new File(state, "close-response").toPath());
            Settings.writeAtomic(state, "close-request", id + "\n");
            logger.log("close game: request=" + id + " marker=published");
        }
    }
    static String closeResponse(File filesDir, String id) throws IOException {
        String response;
        try { response = new String(Files.readAllBytes(new File(filesDir, "state/close-response").toPath()), StandardCharsets.US_ASCII); }
        catch (java.nio.file.NoSuchFileException none) { return ""; }
        if (response.startsWith(id + "\n")) return response.substring(id.length() + 1).trim();
        return response.startsWith("\nerror:") ? response.trim() : "";
    }
    static void clearClose(File filesDir, String id) throws IOException {
        synchronized (MUTEX) {
            File request = new File(filesDir, "state/close-request"), response = new File(filesDir, "state/close-response");
            try { if (new String(Files.readAllBytes(request.toPath()), StandardCharsets.US_ASCII).trim().equals(id)) Files.deleteIfExists(request.toPath()); }
            catch (java.nio.file.NoSuchFileException gone) { }
            try {
                String text = new String(Files.readAllBytes(response.toPath()), StandardCharsets.US_ASCII);
                if (text.startsWith(id + "\n") || text.startsWith("\nerror:")) Files.deleteIfExists(response.toPath());
            } catch (java.nio.file.NoSuchFileException gone) { }
        }
    }

    private GameLock() { }
}

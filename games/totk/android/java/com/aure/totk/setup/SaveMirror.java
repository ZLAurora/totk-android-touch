package com.aure.totk.setup;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** The save folder ("mirror"): the game keeps saving privately; this copies the save set to a folder the player picked
 * (Storage Access Framework tree, persisted grant) after every in-game save and from the menu, and brings a newer copy
 * from the folder back before the game starts. Layout: <picked>/TotK Native saves/sync-NNNNNN/ holding the save root's
 * files and totk-native-saves.json; each sync writes a new generation and its manifest last, then removes the older
 * ones (SaveManager.readFolder/push), so a crash mid-upload never leaves a copy that could be taken for complete and no
 * provider rename is needed. <private>/save-sync.json records the content at the last successful sync.
 * Runs in both processes: the menu (":setup") and the game (Watcher); state/mirror.lock serialises them.
 * Nothing is deleted when the folder is unavailable, incomplete or a step fails. */
public final class SaveMirror {
    private SaveMirror() { }
    private static final String TAG = "TotkSaves";
    private static final String PREFS = "saves", URI_KEY = "save_mirror_uri", NAME_KEY = "save_mirror_name";
    static final String SYNC_FILE = "save-sync.json";
    /** Mirror states shown by the menu (state/save-mirror-status: state, time, message). */
    static final String OK = "ok", UNAVAILABLE = "unavailable", INCOMPLETE = "incomplete", CONFLICT = "conflict", ERROR = "error";

    static SharedPreferences prefs(Context context) { return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE); }
    static String folderString(Context context) { return prefs(context).getString(URI_KEY, null); }
    static Uri folder(Context context) { String s = folderString(context); return s == null ? null : Uri.parse(s); }
    static boolean enabled(Context context) { return folderString(context) != null; }
    static String folderName(Context context) { return prefs(context).getString(NAME_KEY, "?"); }

    /** Use a newly picked tree: keep read+write access across reboots and start with no sync record (the first sync
     * compares both sides from scratch). */
    static void setFolder(Context context, Uri tree) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        try { resolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION); }
        catch (SecurityException refused) { throw new Unavailable("the folder cannot be kept: " + refused.getMessage()); }
        String old = folderString(context);
        if (old != null && !old.equals(tree.toString())) release(resolver, Uri.parse(old));
        String name = "?";
        try {
            Folder folder = new Folder(resolver, tree);
            Doc base = folder.doc(folder.base);
            if (base != null && base.name != null) name = base.name;
        } catch (IOException unreadable) { }
        prefs(context).edit().putString(URI_KEY, tree.toString()).putString(NAME_KEY, name).commit();
        new File(context.getFilesDir(), SYNC_FILE).delete();
        status(context, OK, "");
    }
    /** Stop mirroring. The folder and its files are left as they are. */
    static void turnOff(Context context) {
        Uri uri = folder(context);
        if (uri != null) release(context.getContentResolver(), uri);
        prefs(context).edit().remove(URI_KEY).remove(NAME_KEY).commit();
        new File(context.getFilesDir(), SYNC_FILE).delete();
        new File(stateDir(context), "save-mirror-status").delete();
    }
    private static void release(ContentResolver resolver, Uri uri) {
        try { resolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION); }
        catch (SecurityException notHeld) { }
    }

    // ---------- status (a file, not prefs: both processes write it and each would cache prefs) ----------
    static final class Status {
        final String state, message; final long at;
        Status(String state, long at, String message) { this.state = state; this.at = at; this.message = message; }
    }
    private static File stateDir(Context context) { File dir = new File(context.getFilesDir(), "state"); dir.mkdirs(); return dir; }
    static void status(Context context, String state, String message) {
        try { Settings.writeAtomic(stateDir(context), "save-mirror-status", state + "\n" + System.currentTimeMillis() + "\n" + message.replace('\n', ' ') + "\n"); }
        catch (IOException unwritable) { Log.w(TAG, "mirror status not written", unwritable); }
    }
    static Status status(Context context) {
        try {
            String[] lines = new String(Files.readAllBytes(new File(stateDir(context), "save-mirror-status").toPath()), StandardCharsets.UTF_8).split("\n", -1);
            return new Status(lines[0], Long.parseLong(lines[1]), lines.length > 2 ? lines[2] : "");
        } catch (IOException | RuntimeException none) { return new Status("", 0, ""); }
    }
    /** Time of the last successful sync with the current folder, 0 if none. */
    static long syncedAt(Context context) {
        SaveManager.Manifest last = readSync(context);
        return last == null ? 0 : last.exportedMillis();
    }
    private static SaveManager.Manifest readSync(Context context) {
        try {
            SaveManager.Manifest m = SaveManager.Manifest.parse(new String(Files.readAllBytes(new File(context.getFilesDir(), SYNC_FILE).toPath()), StandardCharsets.UTF_8));
            return m.folder.equals(folderString(context)) ? m : null;
        } catch (IOException missing) { return null; }
    }
    private static void writeSync(Context context, SaveManager.Manifest live) throws IOException {
        SaveManager.Manifest m = new SaveManager.Manifest();
        m.exportedAt = SaveManager.isoTime(System.currentTimeMillis()); m.appVersion = appVersion(context); m.device = Build.MODEL;
        m.folder = folderString(context);
        m.files.addAll(live.files);
        Settings.writeAtomic(context.getFilesDir(), SYNC_FILE, m.toJson());
    }
    static String appVersion(Context context) {
        try { return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (android.content.pm.PackageManager.NameNotFoundException impossible) { return "?"; }
    }

    // ---------- sync ----------
    /** What a sync did. action: off | none | pushed | pulled | pull_pending | busy | conflict | incomplete |
     * unavailable | error. busy = a save was in progress, nothing was uploaded (retry later). */
    static final class Result {
        String action, error = "";
        /** Conflict details: slots, newest slot time and device on each side. */
        long liveTime, folderTime; String folderDevice = "";
        int liveSlots, folderSlots;
        Result(String action) { this.action = action; }
    }
    /** Both sides as they are now (the folder's newest complete generation, by its manifest). With All files access
     * and a folder that has a real path, the folder is read and written directly (direct != null); otherwise through
     * the Storage Access Framework grant. */
    private static final class State {
        File root, direct;
        SaveManager.Manifest live, last;
        Folder folder;
        SaveManager.Store store;
        SaveManager.FolderState folderState = new SaveManager.FolderState();

        void read(Context context) throws IOException {
            root = SaveManager.saveRoot(context.getFilesDir());
            SaveManager.recover(root);
            last = readSync(context);
            live = SaveManager.manifest(root, last, appVersion(context), Build.MODEL, System.currentTimeMillis());
            direct = FileAccess.directFolder(context, folder(context));
            if (direct != null) {
                File top = new File(direct, SaveManager.TOP);
                if (!top.isDirectory()) return;
                store = new SaveManager.FileStore(top);
                folderState = SaveManager.readFolder(store);
                return;
            }
            folder = new Folder(context.getContentResolver(), folder(context));
            folder.checkGrant();
            Doc top = folder.child(folder.base, SaveManager.TOP);
            if (top == null || !top.dir()) return;
            store = new SafStore(folder, top.id);
            folderState = SaveManager.readFolder(store);
        }
        SaveManager.Manifest folderSet() { return folderState.manifest; }
        /** The folder's TOP, created when missing. */
        SaveManager.Store ensureStore() throws IOException {
            if (store != null) return store;
            if (direct != null) {
                File top = new File(direct, SaveManager.TOP);
                if (!top.mkdirs() && !top.isDirectory()) throw new Unavailable("cannot create " + top);
                return store = new SaveManager.FileStore(top);
            }
            return store = new SafStore(folder, folder.ensureDir(folder.base, SaveManager.TOP));
        }
    }
    private static final Object LOCAL = new Object();
    private interface Work { Result run(State s) throws IOException; }
    /** One mirror operation at a time across both processes (state/mirror.lock, plus a monitor in this process),
     * on a fresh view of both sides. */
    private static Result locked(Context context, Work work) {
        synchronized (LOCAL) {
            try (RandomAccessFile file = new RandomAccessFile(new File(stateDir(context), "mirror.lock"), "rw");
                 FileLock lock = file.getChannel().lock()) {
                State state = new State();
                state.read(context);
                return work.run(state);
            } catch (Unavailable gone) {
                Log.w(TAG, "save folder unavailable: " + gone.getMessage());
                status(context, UNAVAILABLE, String.valueOf(gone.getMessage()));
                Result r = new Result(UNAVAILABLE); r.error = String.valueOf(gone.getMessage()); return r;
            } catch (SaveManager.Invalid invalid) {
                if (invalid.code.equals("game_running")) return new Result("pull_pending");
                if (invalid.code.equals("incomplete")) return incomplete(context, invalid.getMessage());
                Log.w(TAG, "save folder content refused: " + invalid.getMessage());
                status(context, ERROR, invalid.getMessage());
                Result r = new Result(ERROR); r.error = invalid.getMessage(); return r;
            } catch (IOException | RuntimeException failed) {
                Log.w(TAG, "save sync failed", failed);
                status(context, ERROR, String.valueOf(failed.getMessage()));
                Result r = new Result(ERROR); r.error = String.valueOf(failed.getMessage()); return r;
            }
        }
    }
    private static Result incomplete(Context context, String why) {
        Log.w(TAG, "save folder incomplete, kept the device's saves: " + why);
        status(context, INCOMPLETE, why);
        Result r = new Result(INCOMPLETE); r.error = why; return r;
    }
    /** Compare the device, the folder and the last sync, then act (worker thread). mayPull: the menu with the game
     * closed; the game process never pulls. */
    static Result sync(Context context, boolean mayPull) {
        if (!enabled(context)) return new Result("off");
        return locked(context, s -> {
            if (s.folderState.incomplete) return incomplete(context, s.folderState.problem);
            SaveManager.Decision decision = SaveManager.decide(s.live, s.folderSet(), s.last);
            Log.i(TAG, "sync: " + decision + " (live " + s.live.files.size() + " files, folder " + (s.folderSet() == null ? "none" : s.folderState.generation)
                    + ", last " + (s.last == null ? "none" : "recorded") + ")");
            switch (decision) {
                case NONE:
                    if (!s.live.files.isEmpty()) writeSync(context, s.live);
                    status(context, OK, "");
                    return new Result("none");
                case PUSH: return push(context, s);
                case PULL:
                    if (!mayPull) return new Result("pull_pending");
                    GameLock.Result game = InstallState.gameState(context);
                    if (game.state == GameLock.State.GAME) return new Result("pull_pending");
                    if (game.state == GameLock.State.ERROR) throw new IOException("Game lock: " + game.reason);
                    return pull(context, s);
                default:
                    status(context, CONFLICT, "");
                    return conflict(s);
            }
        });
    }
    /** The conflict screen's (or an incomplete folder's) answer: keepDevice = the device's saves go to the folder (the
     * folder's copy is backed up here first, as far as it can be read), else the folder's saves replace the device's
     * (backed up first). */
    static Result resolve(Context context, boolean keepDevice) {
        if (!enabled(context)) return new Result("off");
        return locked(context, s -> {
            if (keepDevice) {
                if (s.folderSet() != null) {
                    File copy = SaveManager.staging(context.getFilesDir(), "folder-backup");
                    try {
                        SaveManager.fetch(s.store, s.folderState, copy, false);
                        SaveManager.backup(copy, SaveManager.backupsDir(context.getFilesDir()), "mirror", System.currentTimeMillis());
                    } catch (IOException unreadable) { Log.w(TAG, "the folder's copy could not be backed up", unreadable); }
                    finally { SaveManager.delete(copy); }
                }
                return push(context, s);
            }
            if (s.folderState.incomplete) return incomplete(context, s.folderState.problem);
            if (s.folderSet() == null || s.folderSet().files.isEmpty()) return push(context, s);
            return pull(context, s);
        });
    }
    /** Conflict details for the screen, without changing anything. */
    static Result inspectConflict(Context context) {
        if (!enabled(context)) return new Result("off");
        return locked(context, SaveMirror::conflict);
    }
    private static Result conflict(State s) {
        Result r = new Result(CONFLICT);
        List<SaveManager.Slot> live = SaveManager.slots(s.root), folder = SaveManager.slots(s.folderSet());
        r.liveSlots = live.size(); r.liveTime = SaveManager.newestModified(live);   // both sides by modification time: the folder's manifest has no captions
        r.folderSlots = folder.size(); r.folderTime = SaveManager.newestModified(folder);
        r.folderDevice = s.folderSet() == null ? "" : s.folderSet().device;
        return r;
    }

    /** Upload a stable snapshot of the live set as a new generation. */
    private static Result push(Context context, State s) throws IOException {
        File files = context.getFilesDir(), snap = SaveManager.staging(files, "push");
        long started = SystemClock.elapsedRealtime();
        SaveManager.Manifest m = SaveManager.stableSnapshot(s.root, snap, SaveManager.fileSignal(files), appVersion(context), Build.MODEL, System.currentTimeMillis());
        if (m == null) { Log.i(TAG, "push postponed: a save is in progress"); return new Result("busy"); }
        try {
            if (m.files.isEmpty()) { status(context, OK, ""); return new Result("none"); }
            s.ensureStore();
            Log.i(TAG, "push: start generation " + (s.folderState.highest + 1) + ", " + m.files.size() + " files" + (s.direct != null ? " (direct)" : " (provider)"));
            String gen = SaveManager.push(s.store, snap, m, s.folderState);
            writeSync(context, m);
            status(context, OK, "");
            Log.i(TAG, "pushed " + gen + " (" + m.files.size() + " files) in " + (SystemClock.elapsedRealtime() - started) + " ms");
            return new Result("pushed");
        } finally { SaveManager.delete(snap); }
    }
    /** Replace the device's saves with the folder's generation, verified file by file (live backed up first). */
    private static Result pull(Context context, State s) throws IOException {
        File files = context.getFilesDir(), fetched = SaveManager.staging(files, "fetched");
        try {
            SaveManager.fetch(s.store, s.folderState, fetched, true);
            // The live set must still be the one the decision saw (the game could have run in between).
            SaveManager.Manifest now = SaveManager.manifest(s.root, s.live, "", "", System.currentTimeMillis());
            if (!now.sameFiles(s.live)) throw new IOException("the device's saves changed during the sync");
            SaveManager.replace(files, fetched, "mirror", System.currentTimeMillis());
            SaveManager.Manifest live = SaveManager.manifest(s.root, null, appVersion(context), Build.MODEL, System.currentTimeMillis());
            writeSync(context, live);
            status(context, OK, "");
            prefs(context).edit().putLong("pulled_at", System.currentTimeMillis()).commit();
            Log.i(TAG, "pulled " + s.folderState.generation + " (" + live.files.size() + " files) from the save folder");
            return new Result("pulled");
        } finally { SaveManager.delete(fetched); }
    }

    // ---------- import from a picked folder or zip ----------
    /** Stage the save set in a picked folder tree (worker thread): by its real path with All files access, else
     * through the provider. */
    static SaveManager.Selection stageFolder(Context context, Uri tree, File staged) throws IOException {
        File direct = FileAccess.directFolder(context, tree);
        if (direct != null) {
            SaveManager.Selection selection = SaveManager.select(SaveManager.candidates(direct));
            SaveManager.delete(staged);
            if (!staged.mkdirs()) throw new IOException("cannot create " + staged);
            for (SaveManager.Candidate c : selection.files)
                try (InputStream in = new FileInputStream(new File(direct, selection.sources.get(c.path)))) { SaveManager.copyLimited(in, new File(staged, c.path), c.size, c.mtime); }
            if (selection.manifestSource != null)
                try (InputStream in = new FileInputStream(new File(direct, selection.manifestSource))) { SaveManager.verifyAgainst(staged, SaveManager.Manifest.parse(SaveManager.readText(in, 4L << 20))); }
            Log.i(TAG, "import: staged " + selection.files.size() + " files from " + direct + " (direct)");
            return selection;
        }
        Folder folder = new Folder(context.getContentResolver(), tree);
        Map<String, Doc> docs = new HashMap<>();
        folder.list(folder.base, "", docs);
        List<SaveManager.Candidate> candidates = new ArrayList<>();
        for (Map.Entry<String, Doc> e : docs.entrySet()) candidates.add(new SaveManager.Candidate(e.getKey(), e.getValue().size, false, e.getValue().mtime));
        SaveManager.Selection selection = SaveManager.select(candidates);
        SaveManager.delete(staged);
        if (!staged.mkdirs()) throw new IOException("cannot create " + staged);
        for (SaveManager.Candidate c : selection.files)
            try (InputStream in = folder.open(docs.get(selection.sources.get(c.path)))) { SaveManager.copyLimited(in, new File(staged, c.path), c.size, c.mtime); }
        if (selection.manifestSource != null)
            try (InputStream in = folder.open(docs.get(selection.manifestSource))) { SaveManager.verifyAgainst(staged, SaveManager.Manifest.parse(SaveManager.readText(in, 4L << 20))); }
        return selection;
    }
    /** Stage the save set in a picked zip. With All files access the zip is read where it is; otherwise it is copied to
     * private storage first (the central directory needs random access). Then validated and extracted. */
    static SaveManager.Selection stageZip(Context context, Uri uri, File staged) throws IOException {
        File direct = FileAccess.directFile(context, uri, -1);
        if (direct != null) {
            if (direct.length() > 2 * SaveManager.MAX_TOTAL) throw new SaveManager.Invalid("too_big", direct.getName() + " is too large for a save set");
            SaveManager.delete(staged);
            staged.getParentFile().mkdirs();
            Log.i(TAG, "import: reading " + direct + " (direct)");
            return SaveManager.extractZip(direct, staged);
        }
        File copy = new File(context.getCacheDir(), "import.zip");
        try {
            try (InputStream in = context.getContentResolver().openInputStream(uri)) {
                if (in == null) throw new FileNotFoundException(uri.toString());
                SaveManager.copyLimited(in, copy, 2 * SaveManager.MAX_TOTAL, 0);
            } catch (SecurityException revoked) { throw new Unavailable(revoked.getMessage()); }
            SaveManager.delete(staged);
            staged.getParentFile().mkdirs();
            return SaveManager.extractZip(copy, staged);
        } finally { copy.delete(); }
    }
    /** The export zip (a consistent snapshot of the live set with its manifest) into the document the player created:
     * by its real path with All files access, else through the provider. */
    static void export(Context context, Uri document) throws IOException {
        String path = FileAccess.granted() ? FileAccess.documentPath(context, document) : null;
        File direct = path == null ? null : new File(path);
        if (direct != null && direct.isFile() && direct.canWrite()) {
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(direct)) {
                // writeZip closes its sink: keep the file open so its data can be synced before the close.
                export(context, new java.io.FilterOutputStream(out) {
                    @Override public void write(byte[] b, int off, int len) throws IOException { out.write(b, off, len); }
                    @Override public void close() throws IOException { flush(); }
                });
                out.getFD().sync();
            }
            Log.i(TAG, "export: wrote " + direct + " (direct)");
            return;
        }
        try (OutputStream out = context.getContentResolver().openOutputStream(document, "wt")) {
            if (out == null) throw new IOException("cannot write the export");
            export(context, out);
        } catch (SecurityException revoked) { throw new Unavailable(revoked.getMessage()); }
    }
    static void export(Context context, OutputStream out) throws IOException {
        File files = context.getFilesDir(), snap = SaveManager.staging(files, "export");
        try {
            File root = SaveManager.saveRoot(files);
            SaveManager.recover(root);
            SaveManager.Manifest m = SaveManager.snapshot(root, snap, appVersion(context), Build.MODEL, System.currentTimeMillis(), 6);
            if (m.files.isEmpty()) throw new SaveManager.Invalid("no_saves", null);
            SaveManager.writeZip(snap, m, out);
        } finally { SaveManager.delete(snap); }
    }
    /** The Home chip "Saves updated from your save folder": shown once after a pull. */
    static boolean takePulled(Context context) {
        if (prefs(context).getLong("pulled_at", 0) == 0) return false;
        prefs(context).edit().remove("pulled_at").apply();
        return true;
    }

    // ---------- in-game: push after each save ----------
    /** Game process: watches <private>/state/save-generation (rewritten by the runtime after each completed save)
     * once a second; 3 s after the last change it uploads a stable snapshot on its own thread (a save in progress
     * postpones it by a second at a time). flush() pushes a pending change right away (the game is leaving the
     * foreground); close() waits briefly for an upload. */
    public static final class Watcher {
        private static final long DEBOUNCE_MS = 3000, POLL_MS = 1000, RETRY_MS = 1000;
        private final Context context;
        private final File generation;
        private final Thread thread;
        private final Object wake = new Object();
        /** Guarded by wake. Interrupts are never used: they would close the FileChannels of an upload in progress. */
        private boolean stop, flush, busy;
        private Watcher(Context context) {
            this.context = context.getApplicationContext();
            generation = new File(new File(this.context.getFilesDir(), "state"), "save-generation");
            thread = new Thread(this::loop, "totk-save-mirror");
            thread.setDaemon(true);
        }
        /** Null when no save folder is set. */
        public static Watcher start(Context context) {
            if (!enabled(context)) return null;
            Watcher w = new Watcher(context);
            w.thread.start();
            return w;
        }
        private String read() {
            try { return new String(Files.readAllBytes(generation.toPath()), StandardCharsets.US_ASCII).trim(); }
            catch (IOException missing) { return ""; }
        }
        private void loop() {
            String seen = read();
            long due = 0;
            while (true) {
                String now = read();
                long t = SystemClock.elapsedRealtime();
                if (!now.equals(seen)) { seen = now; due = t + DEBOUNCE_MS; }
                boolean run;
                synchronized (wake) {
                    if (stop) return;
                    run = due != 0 && (t >= due || flush);
                    flush = false;
                    busy = run;
                    if (!run) {
                        try { wake.wait(POLL_MS); } catch (InterruptedException interrupted) { return; }
                        continue;
                    }
                }
                due = 0;
                Result r = sync(context, false);
                Log.i(TAG, "in-game save " + seen + ": " + r.action + (r.error.isEmpty() ? "" : " (" + r.error + ")"));
                if (r.action.equals("busy")) due = SystemClock.elapsedRealtime() + RETRY_MS;
                synchronized (wake) { busy = false; wake.notifyAll(); }
            }
        }
        /** Push a pending save now instead of after the debounce (the game is leaving the foreground). */
        public void flush() { synchronized (wake) { flush = true; wake.notifyAll(); } }
        /** Stop watching; waits up to waitMs for an upload in progress (and a pending save picked up by flush). */
        public void close(long waitMs) {
            long end = SystemClock.elapsedRealtime() + waitMs;
            synchronized (wake) {
                flush = true; wake.notifyAll();
                try {
                    // give the loop one poll to notice a last save, then wait for its upload
                    wake.wait(50);
                    for (long left; busy && (left = end - SystemClock.elapsedRealtime()) > 0;) wake.wait(left);
                } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
                stop = true; wake.notifyAll();
            }
        }
    }

    // ---------- Storage Access Framework ----------
    /** The folder is gone, the grant was revoked, or its volume is unmounted. */
    static final class Unavailable extends IOException { Unavailable(String why) { super(why); } }
    static final class Doc {
        final String id, name, mime; final long size, mtime;
        Doc(String id, String name, String mime, long size, long mtime) { this.id = id; this.name = name; this.mime = mime; this.size = size; this.mtime = mtime; }
        boolean dir() { return Document.MIME_TYPE_DIR.equals(mime); }
    }
    private static final String[] COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
            Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};
    private static final class Folder {
        final ContentResolver resolver; final Uri tree; final String base;
        Folder(ContentResolver resolver, Uri tree) throws IOException {
            if (tree == null) throw new Unavailable("no folder");
            this.resolver = resolver; this.tree = tree;
            try { base = DocumentsContract.getTreeDocumentId(tree); } catch (IllegalArgumentException bad) { throw new Unavailable("not a folder: " + tree); }
            if (doc(base) == null) throw new Unavailable("the folder cannot be opened");
        }
        void checkGrant() throws Unavailable {
            for (UriPermission p : resolver.getPersistedUriPermissions())
                if (p.getUri().equals(tree) && p.isReadPermission() && p.isWritePermission()) return;
            throw new Unavailable("access to the folder was removed");
        }
        Uri uri(String id) { return DocumentsContract.buildDocumentUriUsingTree(tree, id); }
        private static Doc row(Cursor c) {
            return new Doc(c.getString(0), c.getString(1), c.getString(2), c.isNull(3) ? -1 : c.getLong(3), c.isNull(4) ? 0 : c.getLong(4));
        }
        Doc doc(String id) throws IOException {
            try (Cursor c = resolver.query(uri(id), COLUMNS, null, null, null)) {
                if (c == null) throw new Unavailable("the folder's storage does not answer");
                return c.moveToFirst() ? row(c) : null;
            } catch (SecurityException | IllegalArgumentException | IllegalStateException gone) { throw new Unavailable(gone.getMessage()); }
        }
        List<Doc> children(String id) throws IOException {
            List<Doc> out = new ArrayList<>();
            try (Cursor c = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), COLUMNS, null, null, null)) {
                if (c == null) throw new Unavailable("the folder's storage does not answer");
                while (c.moveToNext()) out.add(row(c));
            } catch (SecurityException | IllegalArgumentException | IllegalStateException gone) { throw new Unavailable(gone.getMessage()); }
            return out;
        }
        Doc child(String parent, String name) throws IOException {
            for (Doc d : children(parent)) if (name.equals(d.name)) return d;
            return null;
        }
        /** Files under id by relative path ('/'). */
        void list(String id, String prefix, Map<String, Doc> out) throws IOException {
            for (Doc d : children(id)) {
                if (d.name == null) continue;
                if (out.size() > SaveManager.MAX_ENTRIES) throw new SaveManager.Invalid("too_many", "more than " + SaveManager.MAX_ENTRIES + " entries");
                if (d.dir()) list(d.id, prefix + d.name + "/", out);
                else out.put(prefix + d.name, d);
            }
        }
        String ensureDir(String parent, String name) throws IOException {
            Doc d = child(parent, name);
            if (d != null && d.dir()) return d.id;
            return create(parent, Document.MIME_TYPE_DIR, name);
        }
        /** A new document; refuses when the provider gave it another name (the name was taken). */
        String create(String parent, String mime, String name) throws IOException {
            try {
                Uri made = DocumentsContract.createDocument(resolver, uri(parent), mime, name);
                if (made == null) throw new IOException("cannot create " + name);
                String id = DocumentsContract.getDocumentId(made);
                Doc d = doc(id);
                if (d == null || !name.equals(d.name)) throw new IOException("the folder named " + name + " " + (d == null ? "?" : d.name));
                return id;
            } catch (SecurityException | IllegalArgumentException | IllegalStateException gone) { throw new Unavailable(gone.getMessage()); }
        }
        InputStream open(Doc d) throws IOException {
            try {
                InputStream in = resolver.openInputStream(uri(d.id));
                if (in == null) throw new Unavailable("cannot open " + d.name);
                return in;
            } catch (SecurityException | IllegalArgumentException | IllegalStateException gone) { throw new Unavailable(gone.getMessage()); }
        }
        void delete(String id) throws IOException {
            try { DocumentsContract.deleteDocument(resolver, uri(id)); }
            catch (SecurityException | IllegalArgumentException | IllegalStateException gone) { throw new Unavailable(gone.getMessage()); }
        }
        void write(String id, File source) throws IOException {
            try (InputStream in = new FileInputStream(source); OutputStream out = resolver.openOutputStream(uri(id), "wt")) {
                if (out == null) throw new IOException("cannot write " + source.getName());
                in.transferTo(out);
            } catch (SecurityException | IllegalArgumentException | IllegalStateException gone) { throw new Unavailable(gone.getMessage()); }
        }
    }
    /** SaveManager.Store over TOP of the picked tree: only creates, writes and deletes documents (no rename). */
    private static final class SafStore implements SaveManager.Store {
        private final Folder folder; private final String top;
        private final Map<String, String> dirIds = new HashMap<>();
        private final Map<String, Doc> fileDocs = new HashMap<>();
        SafStore(Folder folder, String top) { this.folder = folder; this.top = top; }
        @Override public List<String> dirs() throws IOException {
            List<String> out = new ArrayList<>();
            for (Doc d : folder.children(top)) if (d.dir() && d.name != null) { out.add(d.name); dirIds.put(d.name, d.id); }
            return out;
        }
        private String dirId(String dir) throws IOException {
            String id = dirIds.get(dir);
            if (id != null) return id;
            int cut = dir.lastIndexOf('/');
            String parent = cut < 0 ? top : dirId(dir.substring(0, cut));
            Doc d = folder.child(parent, dir.substring(cut + 1));
            if (d == null || !d.dir()) throw new FileNotFoundException(dir);
            dirIds.put(dir, d.id);
            return d.id;
        }
        @Override public Map<String, long[]> files(String dir) throws IOException {
            Map<String, Doc> docs = new HashMap<>();
            folder.list(dirId(dir), "", docs);
            Map<String, long[]> out = new HashMap<>();
            for (Map.Entry<String, Doc> e : docs.entrySet()) {
                out.put(e.getKey(), new long[]{e.getValue().size, e.getValue().mtime});
                fileDocs.put(dir + "/" + e.getKey(), e.getValue());
            }
            return out;
        }
        @Override public InputStream open(String dir, String path) throws IOException {
            Doc d = fileDocs.get(dir + "/" + path);
            if (d == null) { files(dir); d = fileDocs.get(dir + "/" + path); }
            if (d == null) throw new FileNotFoundException(dir + "/" + path);
            return folder.open(d);
        }
        @Override public void createDir(String dir) throws IOException { dirIds.put(dir, folder.create(top, Document.MIME_TYPE_DIR, dir)); }
        @Override public void write(String dir, String path, File source) throws IOException {
            int cut = path.lastIndexOf('/');
            String parent = dirId(dir);
            if (cut >= 0) {
                String sub = dir;
                for (String part : path.substring(0, cut).split("/")) {
                    String next = sub + "/" + part;
                    if (!dirIds.containsKey(next)) dirIds.put(next, folder.ensureDir(dirId(sub), part));
                    sub = next;
                }
                parent = dirId(sub);
            }
            folder.write(folder.create(parent, "application/octet-stream", path.substring(cut + 1)), source);
        }
        @Override public void deleteDir(String dir) throws IOException {
            folder.delete(dirId(dir));
            dirIds.keySet().removeIf(k -> k.equals(dir) || k.startsWith(dir + "/"));
        }
    }
}

package com.aure.totk.setup;

import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Host driver for tests/test_android_saves.py: one command per stdin line (arguments separated by tabs), one answer
 * line per command. "-" stands for "none".
 *    select DIR                      SaveManager.select over DIR's files (symlinks flagged): root/files/total or invalid CODE
 *    zip ZIP STAGING                 SaveManager.extractZip: root/files or invalid CODE
 *    export ROOT ZIP                 manifest + writeZip: number of files
 *    decide LIVE FOLDER LAST         SaveManager.decide over three directories (each scanned; "-" = none)
 *    backup ROOT BACKUPS REASON T    SaveManager.backup: the backup folder name or none
 *    backups BACKUPS                 SaveManager.backups: name:reason:slots ... newest first
 *    replace FILES STAGED REASON T   SaveManager.replace: ok BACKUPNAME|none, or invalid CODE
 *    root FILES                      SaveManager.saveRoot (creates the identity when missing): the folder name
 *    jpeg FILE                       SaveManager.captionJpeg: offset length, or none
 *    captime FILE ZONE               SaveManager.captionTime in time zone ZONE: yyyy-MM-ddTHH:mm:ss, or none
 *    slots ROOT ZONE                 SaveManager.slots: index:time ... (time as above, in ZONE as the default zone)
 *    push TOP SNAPSHOT FAILAFTER     SaveManager.readFolder + push on a plain-directory store without renames; FAILAFTER
 *                                    (-1 = never) makes that write store half of its bytes and fail, like a killed upload
 *    read TOP                        SaveManager.readFolder: gen NAME FILES | incomplete PROBLEM | none
 *    fetch TOP STAGING               SaveManager.fetch with verification: ok FILES or invalid CODE
 *    stable ROOT TARGET BUSY GEN     stableSnapshot with a scripted signal: BUSY and GEN are comma lists of the values
 *                                    returned by successive busy()/generation() calls: ok FILES or unstable
 *    locked FILES                    SaveManager.gameLocked */
public final class SavesProbe {
    private static List<SaveManager.Candidate> listing(File dir) throws IOException {
        List<SaveManager.Candidate> out = new ArrayList<>();
        walk(dir, "", out);
        return out;
    }
    private static void walk(File dir, String prefix, List<SaveManager.Candidate> out) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) return;
        Arrays.sort(children);
        for (File child : children) {
            boolean link = Files.isSymbolicLink(child.toPath());
            if (child.isDirectory() && !link) walk(child, prefix + child.getName() + "/", out);
            else out.add(new SaveManager.Candidate(prefix + child.getName(), link ? 0 : child.length(), link, child.lastModified()));
        }
    }
    private static SaveManager.Manifest manifest(String dir) throws IOException {
        return dir.equals("-") ? null : SaveManager.manifest(new File(dir), null, "probe", "host", 0);
    }
    /** A save folder on a plain directory, with no rename at all (like a provider without FLAG_SUPPORTS_RENAME). */
    private static final class DirStore implements SaveManager.Store {
        final File top; int failAfter;
        DirStore(File top, int failAfter) { this.top = top; this.failAfter = failAfter; }
        @Override public List<String> dirs() {
            List<String> out = new ArrayList<>();
            File[] children = top.listFiles();
            if (children != null) for (File f : children) if (f.isDirectory()) out.add(f.getName());
            return out;
        }
        @Override public Map<String, long[]> files(String dir) throws IOException {
            Map<String, long[]> out = new HashMap<>();
            for (SaveManager.Candidate c : listing(new File(top, dir))) out.put(c.path, new long[]{c.size, c.mtime});
            return out;
        }
        @Override public InputStream open(String dir, String path) throws IOException { return new FileInputStream(new File(new File(top, dir), path)); }
        @Override public void createDir(String dir) throws IOException { if (!new File(top, dir).mkdirs()) throw new IOException("exists " + dir); }
        @Override public void write(String dir, String path, File source) throws IOException {
            File target = new File(new File(top, dir), path);
            target.getParentFile().mkdirs();
            if (target.exists()) throw new IOException("exists " + path);
            byte[] bytes = Files.readAllBytes(source.toPath());
            if (failAfter == 0) {
                try (OutputStream out = new FileOutputStream(target)) { out.write(bytes, 0, bytes.length / 2); }
                throw new IOException("killed during " + path);
            }
            if (failAfter > 0) failAfter--;
            try (OutputStream out = new FileOutputStream(target)) { out.write(bytes); }
        }
        @Override public void deleteDir(String dir) { SaveManager.delete(new File(top, dir)); }
    }
    private static Deque<String> sequence(String list) { return new ArrayDeque<>(Arrays.asList(list.split(","))); }
    private static String stamp(long millis, String zone) {
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", java.util.Locale.ROOT);
        f.setTimeZone(java.util.TimeZone.getTimeZone(zone));
        return f.format(new java.util.Date(millis));
    }

    public static void main(String[] args) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        for (String line; (line = in.readLine()) != null;) {
            String[] w = line.split("\t");
            String answer;
            try {
                switch (w[0]) {
                    case "select": {
                        SaveManager.Selection s = SaveManager.select(listing(new File(w[1])));
                        answer = "root=" + s.root + " files=" + s.files.size() + " total=" + s.total + " manifest=" + (s.manifestSource != null);
                        break;
                    }
                    case "zip": {
                        SaveManager.Selection s = SaveManager.extractZip(new File(w[1]), new File(w[2]));
                        answer = "root=" + s.root + " files=" + s.files.size();
                        break;
                    }
                    case "export": {
                        SaveManager.Manifest m = SaveManager.manifest(new File(w[1]), null, "probe", "host", 1_700_000_000_000L);
                        try (OutputStream out = new FileOutputStream(w[2])) { SaveManager.writeZip(new File(w[1]), m, out); }
                        answer = String.valueOf(m.files.size());
                        break;
                    }
                    case "decide": answer = SaveManager.decide(manifest(w[1]), manifest(w[2]), manifest(w[3])).name(); break;
                    case "backup": {
                        SaveManager.Backup b = SaveManager.backup(new File(w[1]), new File(w[2]), w[3], Long.parseLong(w[4]));
                        answer = b == null ? "none" : b.dir.getName();
                        break;
                    }
                    case "backups": {
                        StringBuilder out = new StringBuilder();
                        for (SaveManager.Backup b : SaveManager.backups(new File(w[1])))
                            out.append(out.length() == 0 ? "" : " ").append(b.dir.getName()).append(':').append(b.reason).append(':').append(b.slots);
                        answer = out.toString();
                        break;
                    }
                    case "replace": {
                        SaveManager.Backup b = SaveManager.replace(new File(w[1]), new File(w[2]), w[3], Long.parseLong(w[4]));
                        answer = "ok " + (b == null ? "none" : b.dir.getName());
                        break;
                    }
                    case "root": answer = SaveManager.saveRoot(new File(w[1])).getName(); break;
                    case "jpeg": {
                        int[] j = SaveManager.captionJpeg(Files.readAllBytes(new File(w[1]).toPath()));
                        answer = j == null ? "none" : j[0] + " " + j[1];
                        break;
                    }
                    case "captime": {
                        long t = SaveManager.captionTime(new File(w[1]), java.util.TimeZone.getTimeZone(w[2]));
                        answer = t == 0 ? "none" : stamp(t, w[2]);
                        break;
                    }
                    case "slots": {
                        java.util.TimeZone.setDefault(java.util.TimeZone.getTimeZone(w[2]));
                        StringBuilder out = new StringBuilder();
                        for (SaveManager.Slot s : SaveManager.slots(new File(w[1])))
                            out.append(out.length() == 0 ? "" : " ").append(s.index).append(':').append(stamp(s.time, w[2]));
                        answer = out.toString();
                        break;
                    }
                    case "push": {
                        DirStore store = new DirStore(new File(w[1]), Integer.parseInt(w[3]));
                        SaveManager.FolderState state = SaveManager.readFolder(store);
                        SaveManager.Manifest m = SaveManager.manifest(new File(w[2]), null, "probe", "host", 0);
                        try { answer = SaveManager.push(store, new File(w[2]), m, state); }
                        catch (IOException killed) { answer = "killed"; }
                        break;
                    }
                    case "read": {
                        SaveManager.FolderState s = SaveManager.readFolder(new DirStore(new File(w[1]), -1));
                        answer = s.incomplete ? "incomplete " + s.problem : s.manifest == null ? "none" : "gen " + s.generation + " " + s.manifest.files.size();
                        break;
                    }
                    case "fetch": {
                        DirStore store = new DirStore(new File(w[1]), -1);
                        SaveManager.FolderState s = SaveManager.readFolder(store);
                        SaveManager.fetch(store, s, new File(w[2]), true);
                        answer = "ok " + s.manifest.files.size();
                        break;
                    }
                    case "stable": {
                        Deque<String> busy = sequence(w[3]), gen = sequence(w[4]);
                        SaveManager.Signal signal = new SaveManager.Signal() {
                            @Override public String generation() { return gen.size() > 1 ? gen.poll() : gen.peek(); }
                            @Override public boolean busy() { return Boolean.parseBoolean(busy.size() > 1 ? busy.poll() : busy.peek()); }
                        };
                        SaveManager.Manifest m = SaveManager.stableSnapshot(new File(w[1]), new File(w[2]), signal, "probe", "host", 0);
                        answer = m == null ? "unstable" + (new File(w[2]).exists() ? " leftover" : "") : "ok " + m.files.size();
                        break;
                    }
                    case "game-state": answer = GameLock.check(new File(w[1]), false).state.name(); break;
                    default: answer = "unknown command " + w[0];
                }
            } catch (SaveManager.Invalid invalid) {
                answer = "invalid " + invalid.code;
            }
            System.out.println(answer);
        }
    }
}

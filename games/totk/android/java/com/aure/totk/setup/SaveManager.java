package com.aure.totk.setup;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Date;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/** The game's save set: the live save root (<private>/user-data/save/<uid>, runtime/fs_host.c), export and import as a
 * zip or folder, local backups, the atomic swap, and the mirror's sync decision. Plain Java without Android types so
 * tests/test_android_saves.py runs it on a host JVM; SaveMirror and SetupActivity are the Android glue. */
final class SaveManager {
    private SaveManager() { }
    static final String TITLE_ID = "0100F2C0115B6000";
    /** Top folder of an export zip and of the mirror folder; the manifest sits inside it next to the save files. */
    static final String TOP = "TotK Native saves", MANIFEST = "totk-native-saves.json";
    static final int MAX_FILES = 500, KEEP_BACKUPS = 10;
    static final long MAX_TOTAL = 128L << 20;
    /** Game save files are a few MB (progress.sav ~2.3 MB); a large file with another name is not part of a save set. */
    static final long MAX_KNOWN_FILE = 32L << 20, MAX_OTHER_FILE = 1L << 20;
    /** Zip entries anywhere in the archive (outside the save root too): a cap against archives that are not save dumps. */
    static final int MAX_ENTRIES = 10_000;
    private static final Pattern SLOT_PROGRESS = Pattern.compile("(?:.*/)?slot_\\d\\d/progress\\.sav");
    private static final Pattern KNOWN = Pattern.compile("(?i).*\\.(sav|dat|jpg|jpeg|bin)");

    // ---------- errors ----------
    /** A save set that must not be imported. code: absolute | traversal | symlink | too_many | too_big | huge_file |
     * no_saves | corrupt | not_zip | game_running. */
    static final class Invalid extends IOException {
        final String code;
        Invalid(String code, String detail) { super(code + (detail == null ? "" : ": " + detail)); this.code = code; }
    }

    // ---------- manifest ----------
    static final class FileEntry {
        final String path; final long size; final String sha256; final long mtime;
        FileEntry(String path, long size, String sha256, long mtime) { this.path = path; this.size = size; this.sha256 = sha256; this.mtime = mtime; }
        boolean sameContent(FileEntry o) { return o != null && path.equals(o.path) && size == o.size && sha256 != null && sha256.equals(o.sha256); }
    }
    /** totk-native-saves.json {format:1,title_id,exported_at,app_version,device,files:[{path,size,sha256,mtime}]}; the
     * private save-sync.json uses the same shape plus folder (the mirror tree URI). */
    static final class Manifest {
        int format = 1;
        String titleId = TITLE_ID, exportedAt = "", appVersion = "", device = "", folder = "";
        final List<FileEntry> files = new ArrayList<>();

        Map<String, FileEntry> byPath() {
            Map<String, FileEntry> map = new HashMap<>();
            for (FileEntry e : files) map.put(e.path, e);
            return map;
        }
        /** Same save content: the same paths with the same sizes and digests (dates and devices do not count). */
        boolean sameFiles(Manifest o) {
            if (o == null || o.files.size() != files.size()) return false;
            Map<String, FileEntry> other = o.byPath();
            for (FileEntry e : files) if (!e.sameContent(other.get(e.path))) return false;
            return true;
        }
        long exportedMillis() { return parseTime(exportedAt); }
        String toJson() {
            StringBuilder out = new StringBuilder("{\"format\":").append(format);
            field(out, "title_id", titleId); field(out, "exported_at", exportedAt); field(out, "app_version", appVersion);
            field(out, "device", device);
            if (!folder.isEmpty()) field(out, "folder", folder);
            out.append(",\"files\":");
            entries(out, files, true);
            return out.append("}\n").toString();
        }
        private static void entries(StringBuilder out, List<FileEntry> list, boolean digests) {
            out.append('[');
            for (int i = 0; i < list.size(); i++) {
                FileEntry e = list.get(i);
                out.append(i == 0 ? "\n" : ",\n").append("{\"path\":");
                quote(out, e.path);
                out.append(",\"size\":").append(e.size);
                if (digests) { out.append(",\"sha256\":"); quote(out, e.sha256 == null ? "" : e.sha256); }
                out.append(",\"mtime\":").append(e.mtime).append('}');
            }
            out.append(']');
        }
        private static void field(StringBuilder out, String key, String value) { out.append(",\"").append(key).append("\":"); quote(out, value); }

        static Manifest parse(String text) throws Invalid {
            Object root;
            try { root = new Json(text).document(); } catch (RuntimeException bad) { throw new Invalid("corrupt", "manifest: " + bad.getMessage()); }
            if (!(root instanceof Map)) throw new Invalid("corrupt", "manifest is not an object");
            Map<?, ?> map = (Map<?, ?>) root;
            Manifest m = new Manifest();
            m.format = (int) number(map.get("format"), 0);
            if (m.format != 1) throw new Invalid("corrupt", "unknown manifest format " + map.get("format"));
            m.titleId = string(map.get("title_id")); m.exportedAt = string(map.get("exported_at"));
            m.appVersion = string(map.get("app_version")); m.device = string(map.get("device")); m.folder = string(map.get("folder"));
            if (!m.titleId.isEmpty() && !m.titleId.equalsIgnoreCase(TITLE_ID)) throw new Invalid("corrupt", "saves of another game (" + m.titleId + ")");
            readEntries(map.get("files"), m.files, true);
            return m;
        }
        private static void readEntries(Object value, List<FileEntry> out, boolean digests) throws Invalid {
            if (value == null) return;
            if (!(value instanceof List)) throw new Invalid("corrupt", "manifest file list");
            for (Object item : (List<?>) value) {
                if (!(item instanceof Map)) throw new Invalid("corrupt", "manifest file entry");
                Map<?, ?> e = (Map<?, ?>) item;
                String path = normalize(string(e.get("path")));
                String sha = string(e.get("sha256")).toLowerCase(Locale.ROOT);
                if (digests && !sha.matches("[0-9a-f]{64}")) throw new Invalid("corrupt", "manifest digest of " + path);
                out.add(new FileEntry(path, number(e.get("size"), -1), digests ? sha : null, number(e.get("mtime"), 0)));
            }
        }
        private static String string(Object value) { return value instanceof String ? (String) value : ""; }
        private static long number(Object value, long fallback) { return value instanceof Number ? ((Number) value).longValue() : fallback; }
    }
    static void quote(StringBuilder out, String s) {
        out.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20) out.append(String.format(Locale.ROOT, "\\u%04x", (int) c)); else out.append(c);
            }
        }
        out.append('"');
    }

    /** A small strict JSON reader (org.json is not on the host test classpath): objects, arrays, strings, numbers,
     * true/false/null. */
    static final class Json {
        private final String s; private int i;
        Json(String s) { this.s = s; }
        Object document() { Object v = value(); space(); if (i != s.length()) throw error("trailing data"); return v; }
        private RuntimeException error(String what) { return new IllegalArgumentException(what + " at " + i); }
        private void space() { while (i < s.length() && " \t\r\n".indexOf(s.charAt(i)) >= 0) i++; }
        private Object value() {
            space();
            if (i >= s.length()) throw error("end of text");
            char c = s.charAt(i);
            if (c == '{') {
                i++; Map<String, Object> map = new LinkedHashMap<>(); space();
                if (peek('}')) return map;
                do { space(); String key = string(); space(); expect(':'); map.put(key, value()); space(); } while (peek(','));
                expect('}'); return map;
            }
            if (c == '[') {
                i++; List<Object> list = new ArrayList<>(); space();
                if (peek(']')) return list;
                do { list.add(value()); space(); } while (peek(','));
                expect(']'); return list;
            }
            if (c == '"') return string();
            if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
            if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
            if (s.startsWith("null", i)) { i += 4; return null; }
            int start = i;
            while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
            String number = s.substring(start, i);
            if (number.isEmpty()) throw error("unexpected '" + c + "'");
            try { return number.matches("-?\\d{1,18}") ? (Object) Long.parseLong(number) : (Object) Double.parseDouble(number); }
            catch (NumberFormatException bad) { throw error("bad number"); }
        }
        private boolean peek(char c) { if (i < s.length() && s.charAt(i) == c) { i++; return true; } return false; }
        private void expect(char c) { if (!peek(c)) throw error("expected '" + c + "'"); }
        private String string() {
            expect('"');
            StringBuilder out = new StringBuilder();
            while (true) {
                if (i >= s.length()) throw error("unterminated string");
                char c = s.charAt(i++);
                if (c == '"') return out.toString();
                if (c < 0x20) throw error("control character in string");
                if (c != '\\') { out.append(c); continue; }
                if (i >= s.length()) throw error("unterminated escape");
                char e = s.charAt(i++);
                switch (e) {
                    case '"': case '\\': case '/': out.append(e); break;
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'n': out.append('\n'); break;
                    case 'r': out.append('\r'); break;
                    case 't': out.append('\t'); break;
                    case 'u':
                        if (i + 4 > s.length()) throw error("short \\u escape");
                        try { out.append((char) Integer.parseInt(s.substring(i, i + 4), 16)); } catch (NumberFormatException bad) { throw error("bad \\u escape"); }
                        i += 4; break;
                    default: throw error("bad escape");
                }
            }
        }
    }

    // ---------- time ----------
    static String isoTime(long millis) {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date(millis));
    }
    static long parseTime(String iso) {
        if (iso == null || iso.isEmpty()) return 0;
        try {
            SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
            format.setTimeZone(TimeZone.getTimeZone("UTC"));
            return format.parse(iso).getTime();
        } catch (java.text.ParseException bad) { return 0; }
    }
    /** Export file name: "TotK Native saves 2026-11-02 1403.zip" (local time). */
    static String exportName(long millis) { return TOP + " " + new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.ROOT).format(new Date(millis)) + ".zip"; }

    // ---------- the live save root ----------
    /** The live save root when the local account exists (listing only: never creates the identity), else null. */
    static File existingRoot(File filesDir) {
        File uid = new File(new File(filesDir, "user-data"), "account.uid");
        if (!uid.isFile() || uid.length() != 16) return null;
        try { return saveRoot(filesDir); } catch (IOException unreadable) { return null; }
    }
    /** <private>/user-data/save/<hex(hi)><hex(lo)>, from user-data/account.uid exactly like runtime/account.c and
     * fs_host.c. A missing identity is created (16 random non-zero bytes, as the runtime would on its first start), so
     * saves imported before the first game start land where the game will look. */
    static File saveRoot(File filesDir) throws IOException {
        File profile = new File(filesDir, "user-data"), uid = new File(profile, "account.uid");
        byte[] id = uid.isFile() ? Files.readAllBytes(uid.toPath()) : null;
        if (id == null || id.length == 0) {
            if (!profile.isDirectory() && !profile.mkdirs()) throw new IOException("cannot create " + profile);
            id = new byte[16];
            java.security.SecureRandom random = new java.security.SecureRandom();
            do random.nextBytes(id); while (allZero(id));
            File temp = new File(profile, "account.uid.tmp");
            try (FileOutputStream out = new FileOutputStream(temp)) { out.write(id); out.getFD().sync(); }
            temp.setReadable(false, false); temp.setReadable(true, true); temp.setWritable(false, false); temp.setWritable(true, true);
            Files.move(temp.toPath(), uid.toPath(), StandardCopyOption.ATOMIC_MOVE);
        }
        if (id.length != 16 || allZero(id)) throw new IOException("corrupt account.uid");
        return new File(new File(profile, "save"), uidName(id));
    }
    private static boolean allZero(byte[] bytes) { for (byte b : bytes) if (b != 0) return false; return true; }
    /** account.uid holds {lo, hi} as two little-endian u64; the folder is "%016llx%016llx" of hi then lo. */
    static String uidName(byte[] id) {
        long lo = 0, hi = 0;
        for (int b = 7; b >= 0; b--) { lo = lo << 8 | (id[b] & 0xff); hi = hi << 8 | (id[8 + b] & 0xff); }
        return String.format(Locale.ROOT, "%016x%016x", hi, lo);
    }

    /** Every regular file under root (symbolic links are skipped), sorted by path, with SHA-256. A digest is reused from
     * cache when the file's size and modification time are unchanged, so a sync check does not rehash the set. */
    static List<FileEntry> scan(File root, Manifest cache) throws IOException {
        Map<String, FileEntry> known = cache == null ? new HashMap<>() : cache.byPath();
        List<String> paths = new ArrayList<>();
        walk(root, "", paths);
        java.util.Collections.sort(paths);
        List<FileEntry> out = new ArrayList<>();
        for (String path : paths) {
            File file = new File(root, path);
            long size = file.length(), mtime = file.lastModified();
            FileEntry k = known.get(path);
            String sha = k != null && k.size == size && k.mtime == mtime && k.sha256 != null ? k.sha256 : sha256(file);
            out.add(new FileEntry(path, size, sha, mtime));
        }
        return out;
    }
    private static void walk(File dir, String prefix, List<String> out) {
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) continue;
            String path = prefix + child.getName();
            if (child.isDirectory()) walk(child, path + "/", out);
            else if (child.isFile()) out.add(path);
        }
    }
    static Manifest manifest(File root, Manifest cache, String appVersion, String device, long now) throws IOException {
        Manifest m = new Manifest();
        m.exportedAt = isoTime(now); m.appVersion = appVersion == null ? "" : appVersion; m.device = device == null ? "" : device;
        if (root.isDirectory()) m.files.addAll(scan(root, cache));
        return m;
    }
    static String sha256(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) { return sha256(in); }
    }
    static String sha256(InputStream in) throws IOException {
        MessageDigest digest = digest();
        byte[] buffer = new byte[64 * 1024];
        for (int n; (n = in.read(buffer)) > 0;) digest.update(buffer, 0, n);
        return hex(digest.digest());
    }
    static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException impossible) { throw new IllegalStateException(impossible); }
    }
    static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return out.toString();
    }

    // ---------- slots ----------
    static final class Slot {
        /** time: when the game saved this slot (caption.sav's own timestamp, else progress.sav's modification time);
         * mtime: progress.sav's modification time. */
        final int index; final long time, mtime; final File caption;
        Slot(int index, long time, long mtime, File caption) { this.index = index; this.time = time; this.mtime = mtime; this.caption = caption; }
    }
    /** slot_NN folders holding a progress.sav, by slot number. */
    static List<Slot> slots(File root) {
        List<Slot> out = new ArrayList<>();
        File[] children = root == null ? null : root.listFiles();
        if (children == null) return out;
        for (File child : children) {
            String name = child.getName();
            if (!name.matches("slot_\\d\\d") || !child.isDirectory()) continue;
            File progress = new File(child, "progress.sav"), caption = new File(child, "caption.sav");
            if (!progress.isFile()) continue;
            long mtime = progress.lastModified(), saved = caption.isFile() ? captionTime(caption, TimeZone.getDefault()) : 0;
            out.add(new Slot(Integer.parseInt(name.substring(5)), saved > 0 ? saved : mtime, mtime, caption.isFile() ? caption : null));
        }
        out.sort((a, b) -> Integer.compare(a.index, b.index));
        return out;
    }
    static long newest(List<Slot> slots) { long t = 0; for (Slot s : slots) t = Math.max(t, s.time); return t; }
    /** Newest progress.sav modification time (comparable with a save folder's manifest, which has no captions). */
    static long newestModified(List<Slot> slots) { long t = 0; for (Slot s : slots) t = Math.max(t, s.mtime); return t; }

    /** caption.sav is the game's hash/value save table: u32 magic 0x01020304, u32 version, u32 data offset, then from
     * 0x20 up to the data offset 8-byte {u32 key hash, u32 value} entries. The slot's save timestamp is six s32
     * entries (murmur3 key hashes below) holding the device's local wall-clock time when the game saved. */
    private static final int CAPTION_MAGIC = 0x01020304, CAPTION_TABLE = 0x20, CAPTION_TABLE_MAX = 0x10000;
    private static final int[] CAPTION_TIME_KEYS = {0x9811a3f7, 0xdfd840d3, 0xbd46f485, 0x23f3d75e, 0x27853bf7, 0x5ee50bed};   // year, month, day, hour, minute, second
    /** The in-game save time of a caption.sav as epoch milliseconds in zone, or 0 when it cannot be read. */
    static long captionTime(File caption, TimeZone zone) {
        try (RandomAccessFile in = new RandomAccessFile(caption, "r")) {
            if (in.length() < CAPTION_TABLE) return 0;
            in.seek(8);
            int end = Integer.reverseBytes(in.readInt());   // the data offset ends the entry table
            if (end < CAPTION_TABLE || end > CAPTION_TABLE_MAX || end > in.length()) return 0;
            byte[] table = new byte[end];
            in.seek(0);
            in.readFully(table);
            return captionTime(table, end, zone);
        } catch (IOException unreadable) { return 0; }
    }
    static long captionTime(byte[] data, int length, TimeZone zone) {
        if (length < CAPTION_TABLE) return 0;
        java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(data, 0, length).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        if (b.getInt(0) != CAPTION_MAGIC) return 0;
        int end = b.getInt(8);
        if (end < CAPTION_TABLE || end > length) return 0;
        int[] v = new int[CAPTION_TIME_KEYS.length];
        int found = 0;
        for (int p = CAPTION_TABLE; p + 8 <= end; p += 8) {
            int key = b.getInt(p);
            for (int k = 0; k < CAPTION_TIME_KEYS.length; k++)
                if (key == CAPTION_TIME_KEYS[k] && (found & (1 << k)) == 0) { v[k] = b.getInt(p + 4); found |= 1 << k; }
        }
        if (found != (1 << CAPTION_TIME_KEYS.length) - 1) return 0;
        if (v[0] < 2000 || v[0] > 2999 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] < 0 || v[3] > 23
                || v[4] < 0 || v[4] > 59 || v[5] < 0 || v[5] > 59) return 0;
        java.util.Calendar c = java.util.Calendar.getInstance(zone, Locale.ROOT);
        c.clear();
        c.setLenient(false);
        c.set(v[0], v[1] - 1, v[2], v[3], v[4], v[5]);
        try { return c.getTimeInMillis(); } catch (IllegalArgumentException impossible) { return 0; }
    }
    /** The picture inside caption.sav: the game stores a 256 x 144 baseline JPEG after a small header. Returns
     * {offset, length} of the bytes from the SOI marker to the last EOI marker, or null when there is none. */
    static int[] captionJpeg(byte[] data) {
        if (data == null) return null;
        int start = -1;
        for (int i = 0; i + 2 < data.length; i++)
            if ((data[i] & 0xff) == 0xff && (data[i + 1] & 0xff) == 0xd8 && (data[i + 2] & 0xff) == 0xff) { start = i; break; }
        if (start < 0) return null;
        for (int j = data.length - 2; j > start; j--)
            if ((data[j] & 0xff) == 0xff && (data[j + 1] & 0xff) == 0xd9) return new int[]{start, j + 2 - start};
        return null;
    }

    // ---------- import: validation and save-root detection ----------
    /** One file (or symbolic link) of a dump, as listed by a zip's central directory or a picked folder. */
    static final class Candidate {
        final String path; final long size; final boolean link; final long mtime;
        Candidate(String path, long size, boolean link, long mtime) { this.path = path; this.size = size; this.link = link; this.mtime = mtime; }
    }
    /** A validated save set to import: the root prefix inside the dump ("" = top) and its files relative to that root. */
    static final class Selection {
        final String root; final List<Candidate> files = new ArrayList<>(); final Map<String, String> sources = new HashMap<>();
        String manifestSource;
        long total;
        Selection(String root) { this.root = root; }
    }
    /** A dump path as a clean relative path ('/' separators, no "." segments); rejects absolute paths and "..". */
    static String normalize(String raw) throws Invalid {
        if (raw == null || raw.isEmpty()) throw new Invalid("corrupt", "empty name");
        String p = raw.replace('\\', '/');
        if (p.startsWith("/") || p.matches("^[A-Za-z]:.*")) throw new Invalid("absolute", raw);
        StringBuilder out = new StringBuilder();
        for (String segment : p.split("/")) {
            if (segment.isEmpty() || segment.equals(".")) continue;
            if (segment.equals("..")) throw new Invalid("traversal", raw);
            if (segment.indexOf('\0') >= 0) throw new Invalid("corrupt", raw);
            out.append(out.length() == 0 ? "" : "/").append(segment);
        }
        if (out.length() == 0) throw new Invalid("corrupt", "empty name");
        return out.toString();
    }
    private static boolean junk(String path) {
        String name = path.substring(path.lastIndexOf('/') + 1);
        return path.startsWith("__MACOSX/") || path.contains("/__MACOSX/") || name.equals(".DS_Store") || name.equals("Thumbs.db")
                || name.equals("desktop.ini") || name.startsWith("._");
    }
    /** Root directory prefix of a save set: "" or "dir/sub/". */
    private static String rootOf(String path) {
        if (path.equals("option.sav") || path.endsWith("/option.sav")) return path.substring(0, path.length() - "option.sav".length());
        if (SLOT_PROGRESS.matcher(path).matches()) {
            String slotDir = path.substring(0, path.length() - "/progress.sav".length());
            int cut = slotDir.lastIndexOf('/');
            return cut < 0 ? "" : slotDir.substring(0, cut + 1);
        }
        return null;
    }
    /** Validate a whole dump and pick its save root: the shallowest directory holding option.sav or a
     * slot_NN/progress.sav (when several sit at that depth — e.g. dated backups side by side — the one whose
     * progress.sav is newest). Any absolute path, "..", or symbolic link anywhere rejects the dump; under the root,
     * more than MAX_FILES files, more than MAX_TOTAL bytes, or an oversized file rejects it too. */
    static Selection select(Collection<Candidate> all) throws Invalid {
        if (all.size() > MAX_ENTRIES) throw new Invalid("too_many", all.size() + " entries");
        List<Candidate> clean = new ArrayList<>();
        for (Candidate c : all) {
            String path = normalize(c.path);
            if (c.link) throw new Invalid("symlink", path);
            if (!junk(path)) clean.add(new Candidate(path, c.size, false, c.mtime));
        }
        Map<String, Long> roots = new LinkedHashMap<>();
        for (Candidate c : clean) {
            String root = rootOf(c.path);
            if (root != null) roots.merge(root, c.path.endsWith("progress.sav") ? c.mtime : Long.MIN_VALUE, Math::max);
        }
        if (roots.isEmpty()) throw new Invalid("no_saves", null);
        String best = null;
        for (Map.Entry<String, Long> e : roots.entrySet()) {
            if (best == null) { best = e.getKey(); continue; }
            int depth = depth(e.getKey()), bestDepth = depth(best);
            if (depth < bestDepth || depth == bestDepth && e.getValue() > roots.get(best)) best = e.getKey();
        }
        Selection selection = new Selection(best);
        for (Candidate c : clean) {
            if (!c.path.startsWith(best)) continue;
            String relative = c.path.substring(best.length());
            if (relative.equals(MANIFEST)) { selection.manifestSource = c.path; continue; }
            long limit = KNOWN.matcher(relative).matches() ? MAX_KNOWN_FILE : MAX_OTHER_FILE;
            if (c.size > limit) throw new Invalid("huge_file", relative + " (" + c.size + " bytes)");
            if (c.size < 0) throw new Invalid("corrupt", relative + " has no size");
            selection.total += c.size;
            selection.files.add(new Candidate(relative, c.size, false, c.mtime));
            selection.sources.put(relative, c.path);
            if (selection.files.size() > MAX_FILES) throw new Invalid("too_many", "more than " + MAX_FILES + " files");
            if (selection.total > MAX_TOTAL) throw new Invalid("too_big", selection.total + " bytes");
        }
        return selection;
    }
    private static int depth(String prefix) { int n = 0; for (int i = 0; i < prefix.length(); i++) if (prefix.charAt(i) == '/') n++; return n; }

    /** Copy a stream into out, failing once more than limit bytes arrive (sizes in a dump can lie). */
    static void copyLimited(InputStream in, File out, long limit, long mtime) throws IOException {
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        long written = 0;
        try (FileOutputStream o = new FileOutputStream(out)) {
            byte[] buffer = new byte[64 * 1024];
            for (int n; (n = in.read(buffer)) > 0;) {
                written += n;
                if (written > limit) throw new Invalid("too_big", out.getName() + " is larger than its listed size");
                o.write(buffer, 0, n);
            }
            o.getFD().sync();
        }
        if (mtime > 0) out.setLastModified(mtime);
    }
    /** After staging: when the dump carries our manifest, every file must match it (a damaged or edited export is refused). */
    static void verifyAgainst(File staged, Manifest manifest) throws IOException {
        if (manifest == null) return;
        Map<String, FileEntry> want = manifest.byPath();
        List<FileEntry> got = scan(staged, null);
        if (got.size() != want.size()) throw new Invalid("corrupt", "the files do not match the dump's manifest");
        for (FileEntry e : got) if (!e.sameContent(want.get(e.path))) throw new Invalid("corrupt", e.path + " does not match the dump's manifest");
    }

    // ---------- zip ----------
    /** Zip entry names with the symbolic-link bit from the central directory (java.util.zip does not expose the
     * external attributes): name -> {size, isLink}. Handles ZIP64 end records. */
    static Map<String, long[]> zipDirectory(File zip) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(zip, "r")) {
            long length = f.length();
            int tail = (int) Math.min(length, 65_557);
            byte[] end = new byte[tail];
            f.seek(length - tail); f.readFully(end);
            int eocd = -1;
            for (int i = tail - 22; i >= 0; i--) if (le32(end, i) == 0x06054b50L) { eocd = i; break; }
            if (eocd < 0) throw new Invalid("not_zip", "no end of central directory");
            long count = le16(end, eocd + 10), cdSize = le32(end, eocd + 12), cdOffset = le32(end, eocd + 16);
            if (count == 0xffff || cdSize == 0xffffffffL || cdOffset == 0xffffffffL) {
                if (eocd < 20 || le32(end, eocd - 20) != 0x07064b50L) throw new Invalid("not_zip", "zip64 locator missing");
                long record = le64(end, eocd - 20 + 8);
                byte[] z = new byte[56];
                f.seek(record); f.readFully(z);
                if (le32(z, 0) != 0x06064b50L) throw new Invalid("not_zip", "zip64 end record");
                count = le64(z, 32); cdSize = le64(z, 40); cdOffset = le64(z, 48);
            }
            if (count > MAX_ENTRIES) throw new Invalid("too_many", count + " entries");
            if (cdSize > 16L << 20 || cdOffset + cdSize > length) throw new Invalid("not_zip", "central directory out of range");
            byte[] cd = new byte[(int) cdSize];
            f.seek(cdOffset); f.readFully(cd);
            Map<String, long[]> out = new LinkedHashMap<>();
            int p = 0;
            for (long n = 0; n < count; n++) {
                if (p + 46 > cd.length || le32(cd, p) != 0x02014b50L) throw new Invalid("not_zip", "central directory entry " + n);
                int madeBy = (int) le16(cd, p + 4) >> 8, flags = (int) le16(cd, p + 8);
                long size = le32(cd, p + 24), external = le32(cd, p + 38);
                int nameLength = (int) le16(cd, p + 28), extra = (int) le16(cd, p + 30), comment = (int) le16(cd, p + 32);
                if (p + 46 + nameLength + extra + comment > cd.length) throw new Invalid("not_zip", "central directory entry " + n);
                String name = new String(cd, p + 46, nameLength, (flags & 0x800) != 0 ? StandardCharsets.UTF_8 : StandardCharsets.ISO_8859_1);
                if (size == 0xffffffffL) size = zip64Size(cd, p + 46 + nameLength, extra);
                // Unix hosts (3) keep st_mode in the high half of the external attributes; S_IFLNK = 0120000.
                boolean link = madeBy == 3 && ((external >>> 16) & 0170000) == 0120000;
                out.put(name, new long[]{size, link ? 1 : 0});
                p += 46 + nameLength + extra + comment;
            }
            return out;
        }
    }
    private static long zip64Size(byte[] cd, int at, int length) {
        for (int p = at; p + 4 <= at + length;) {
            int id = (int) le16(cd, p), size = (int) le16(cd, p + 2);
            if (id == 1 && size >= 8) return le64(cd, p + 4);
            p += 4 + size;
        }
        return -1;
    }
    private static long le16(byte[] b, int i) { return (b[i] & 0xff) | (b[i + 1] & 0xff) << 8; }
    private static long le32(byte[] b, int i) { return le16(b, i) | le16(b, i + 2) << 16; }
    private static long le64(byte[] b, int i) { return le32(b, i) | le32(b, i + 4) << 32; }

    /** Validate a zip dump and extract its save root into staging (which must not exist yet). */
    static Selection extractZip(File zip, File staging) throws IOException {
        Map<String, long[]> directory = zipDirectory(zip);
        List<Candidate> candidates = new ArrayList<>();
        try (ZipFile file = new ZipFile(zip)) {
            Map<String, Long> times = new HashMap<>();
            for (Enumeration<? extends ZipEntry> e = file.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                times.put(entry.getName(), entry.getTime());
            }
            for (Map.Entry<String, long[]> e : directory.entrySet()) {
                if (e.getKey().endsWith("/") && e.getValue()[1] == 0) { normalize(e.getKey()); continue; }  // directory entry
                Long time = times.get(e.getKey());
                candidates.add(new Candidate(e.getKey(), e.getValue()[0], e.getValue()[1] != 0, time == null ? 0 : time));
            }
            Selection selection = select(candidates);
            Map<String, String> original = new HashMap<>();
            for (Map.Entry<String, long[]> e : directory.entrySet()) original.put(normalize(e.getKey()), e.getKey());
            if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
            for (Candidate c : selection.files) {
                ZipEntry entry = file.getEntry(original.get(selection.sources.get(c.path)));
                if (entry == null) throw new Invalid("corrupt", c.path);
                try (InputStream in = file.getInputStream(entry)) { copyLimited(in, new File(staging, c.path), c.size, c.mtime); }
            }
            if (selection.manifestSource != null) {
                ZipEntry entry = file.getEntry(original.get(selection.manifestSource));
                try (InputStream in = file.getInputStream(entry)) { verifyAgainst(staging, Manifest.parse(readText(in, 4L << 20))); }
            }
            return selection;
        } catch (java.util.zip.ZipException bad) {
            throw new Invalid("not_zip", bad.getMessage());
        }
    }
    static String readText(InputStream in, long limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16 * 1024];
        for (int n; (n = in.read(buffer)) > 0;) {
            out.write(buffer, 0, n);
            if (out.size() > limit) throw new Invalid("too_big", "manifest");
        }
        return out.toString("UTF-8");
    }
    /** The export: TOP/ with the save root's files and TOP/MANIFEST (written last). root is a consistent snapshot. */
    static void writeZip(File root, Manifest manifest, OutputStream sink) throws IOException {
        try (ZipOutputStream zip = new ZipOutputStream(sink)) {
            for (FileEntry e : manifest.files) {
                ZipEntry entry = new ZipEntry(TOP + "/" + e.path);
                entry.setTime(e.mtime);
                zip.putNextEntry(entry);
                try (InputStream in = new FileInputStream(new File(root, e.path))) { in.transferTo(zip); }
                zip.closeEntry();
            }
            ZipEntry entry = new ZipEntry(TOP + "/" + MANIFEST);
            entry.setTime(manifest.exportedMillis());
            zip.putNextEntry(entry);
            zip.write(manifest.toJson().getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
        }
    }

    // ---------- consistent snapshot of the live root ----------
    /** Copy root into an empty target while the game may be saving: each file's size and modification time are read
     * before and after its copy, and the whole tree is listed again at the end; any change (or an IOException from a
     * file that disappeared) discards the copy and starts over, up to attempts times. Returns the manifest of the copy. */
    static Manifest snapshot(File root, File target, String appVersion, String device, long now, int attempts) throws IOException {
        IOException last = null;
        for (int attempt = 0; attempt < attempts; attempt++) {
            delete(target);
            if (!target.mkdirs()) throw new IOException("cannot create " + target);
            try {
                Map<String, long[]> before = stats(root);
                boolean changed = false;
                for (Map.Entry<String, long[]> e : before.entrySet()) {
                    File from = new File(root, e.getKey()), to = new File(target, e.getKey());
                    to.getParentFile().mkdirs();
                    try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) { in.transferTo(out); }
                    if (from.length() != e.getValue()[0] || from.lastModified() != e.getValue()[1] || to.length() != e.getValue()[0]) { changed = true; break; }
                    to.setLastModified(e.getValue()[1]);
                }
                if (!changed && sameStats(before, stats(root))) {
                    Manifest m = manifest(target, null, appVersion, device, now);
                    // keep the live modification times (the cache key for later scans of the live root)
                    List<FileEntry> files = new ArrayList<>();
                    for (FileEntry e : m.files) files.add(new FileEntry(e.path, e.size, e.sha256, before.get(e.path)[1]));
                    m.files.clear(); m.files.addAll(files);
                    return m;
                }
            } catch (IOException raced) { last = raced; }
            try { Thread.sleep(300L * (attempt + 1)); } catch (InterruptedException stop) { Thread.currentThread().interrupt(); break; }
        }
        delete(target);
        throw last != null ? last : new IOException("the saves kept changing while they were copied");
    }
    private static Map<String, long[]> stats(File root) {
        List<String> paths = new ArrayList<>();
        walk(root, "", paths);
        java.util.Collections.sort(paths);
        Map<String, long[]> out = new LinkedHashMap<>();
        for (String p : paths) { File f = new File(root, p); out.put(p, new long[]{f.length(), f.lastModified()}); }
        return out;
    }
    private static boolean sameStats(Map<String, long[]> a, Map<String, long[]> b) {
        if (!a.keySet().equals(b.keySet())) return false;
        for (Map.Entry<String, long[]> e : a.entrySet()) if (!java.util.Arrays.equals(e.getValue(), b.get(e.getKey()))) return false;
        return true;
    }

    // ---------- swap ----------
    static File incoming(File root) { return new File(root.getPath() + ".incoming"); }
    private static File ready(File root) { return new File(root.getPath() + ".ready"); }
    private static File old(File root) { return new File(root.getPath() + ".old"); }
    /** Replace the live root with a fully written incoming tree: incoming is renamed to .ready (complete), the live
     * root to .old, .ready to the root, then .old is removed. Every step is a rename in one directory, and recover()
     * finishes or undoes an interrupted swap, so a crash never leaves the game without a complete save set. */
    static void install(File incoming, File root) throws IOException {
        syncTree(incoming);
        File ready = ready(root), old = old(root);
        delete(ready); delete(old);
        Files.move(incoming.toPath(), ready.toPath(), StandardCopyOption.ATOMIC_MOVE);
        if (root.exists()) Files.move(root.toPath(), old.toPath(), StandardCopyOption.ATOMIC_MOVE);
        Files.move(ready.toPath(), root.toPath(), StandardCopyOption.ATOMIC_MOVE);
        syncDir(root.getParentFile());
        delete(old);
    }
    /** Finish or roll back a swap interrupted by process death; drops half-written incoming trees. */
    static void recover(File root) throws IOException {
        File ready = ready(root), old = old(root), incoming = incoming(root);
        delete(incoming);
        if (ready.isDirectory()) {
            if (root.exists()) { delete(old); Files.move(root.toPath(), old.toPath(), StandardCopyOption.ATOMIC_MOVE); }
            Files.move(ready.toPath(), root.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } else if (!root.exists() && old.isDirectory()) Files.move(old.toPath(), root.toPath(), StandardCopyOption.ATOMIC_MOVE);
        delete(old);
    }
    private static void syncTree(File dir) {
        File[] children = dir.listFiles();
        if (children != null) for (File child : children) {
            if (child.isDirectory()) syncTree(child);
            else try (FileChannel channel = FileChannel.open(child.toPath(), StandardOpenOption.READ)) { channel.force(true); } catch (IOException ignored) { }
        }
        syncDir(dir);
    }
    /** fsync a directory where the platform allows opening one (Linux does); best effort elsewhere. */
    private static void syncDir(File dir) {
        if (dir == null) return;
        try (FileChannel channel = FileChannel.open(dir.toPath(), StandardOpenOption.READ)) { channel.force(true); } catch (IOException | RuntimeException ignored) { }
    }
    static void copyTree(File from, File to) throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot create " + to);
        File[] children = from.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) continue;
            File target = new File(to, child.getName());
            if (child.isDirectory()) copyTree(child, target);
            else if (child.isFile()) try (InputStream in = new FileInputStream(child)) { copyLimited(in, target, Long.MAX_VALUE, child.lastModified()); }
        }
    }
    static void delete(File file) {
        File[] children = file.isDirectory() && !Files.isSymbolicLink(file.toPath()) ? file.listFiles() : null;
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }

    // ---------- backups ----------
    static final class Backup {
        final File dir; final String reason; final long created; final int slots;
        Backup(File dir, String reason, long created, int slots) { this.dir = dir; this.reason = reason; this.created = created; this.slots = slots; }
        File saves() { return new File(dir, "save"); }
    }
    /** Copy the live root to <backups>/<yyyyMMdd-HHmmss-SSS>/save with backup.json {reason, created, slots}; the folder
     * is written as .partial and renamed when complete. Keeps the newest KEEP_BACKUPS. Returns null when there is
     * nothing to back up. reason: import | mirror | restore | manual. */
    static Backup backup(File root, File backups, String reason, long now) throws IOException {
        if (root == null || !root.isDirectory() || scanCount(root) == 0) return null;
        String name = new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.ROOT).format(new Date(now));
        File dir = new File(backups, name), partial = new File(backups, name + ".partial");
        for (int n = 1; dir.exists(); n++) { dir = new File(backups, name + "-" + n); partial = new File(backups, dir.getName() + ".partial"); }
        delete(partial);
        copyTree(root, new File(partial, "save"));
        int slots = slots(new File(partial, "save")).size();
        StringBuilder json = new StringBuilder("{\"reason\":");
        quote(json, reason);
        json.append(",\"created\":").append(now).append(",\"slots\":").append(slots).append("}\n");
        Settings.writeAtomic(partial, "backup.json", json.toString());
        syncTree(partial);
        Files.move(partial.toPath(), dir.toPath(), StandardCopyOption.ATOMIC_MOVE);
        rotate(backups, KEEP_BACKUPS);
        return new Backup(dir, reason, now, slots);
    }
    private static int scanCount(File root) { List<String> paths = new ArrayList<>(); walk(root, "", paths); return paths.size(); }
    /** Newest first; leftovers of interrupted backups (.partial) are not listed. */
    static List<Backup> backups(File backups) {
        List<Backup> out = new ArrayList<>();
        File[] dirs = backups.listFiles();
        if (dirs == null) return out;
        for (File dir : dirs) {
            if (!dir.isDirectory() || dir.getName().endsWith(".partial")) continue;
            String reason = "manual"; long created = dir.lastModified(); int slots = -1;
            try {
                Object parsed = new Json(new String(Files.readAllBytes(new File(dir, "backup.json").toPath()), StandardCharsets.UTF_8)).document();
                if (parsed instanceof Map) {
                    Map<?, ?> m = (Map<?, ?>) parsed;
                    if (m.get("reason") instanceof String) reason = (String) m.get("reason");
                    if (m.get("created") instanceof Number) created = ((Number) m.get("created")).longValue();
                    if (m.get("slots") instanceof Number) slots = ((Number) m.get("slots")).intValue();
                }
            } catch (IOException | RuntimeException unreadable) { continue; }
            out.add(new Backup(dir, reason, created, slots));
        }
        out.sort((a, b) -> b.dir.getName().compareTo(a.dir.getName()));
        return out;
    }
    static void rotate(File backups, int keep) {
        List<Backup> list = backups(backups);
        for (int i = keep; i < list.size(); i++) delete(list.get(i).dir);
        File[] dirs = backups.listFiles();
        if (dirs != null) for (File dir : dirs) if (dir.getName().endsWith(".partial")) delete(dir);
    }

    // ---------- import, restore ----------
    static File backupsDir(File filesDir) { return new File(filesDir, "save-backups"); }
    /** Private scratch trees (same filesystem as the live root, so a staged set can be renamed into place). */
    static File staging(File filesDir, String name) { return new File(new File(filesDir, "save-staging"), name); }
    /** Make a staged tree the live save set: refuses while the game runs, backs the live set up first (reason), then
     * moves staged into place with install(). The game lock is held only for the backup copy and the renames. */
    static Backup replace(File filesDir, File staged, String reason, long now) throws IOException {
        File root = saveRoot(filesDir);
        try (GameLock.Hold hold = GameLock.hold(filesDir)) {
            hold.requireFree();
            recover(root);
            Backup backup = backup(root, backupsDir(filesDir), reason, now);
            File incoming = incoming(root);
            delete(incoming);
            if (!root.getParentFile().isDirectory() && !root.getParentFile().mkdirs()) throw new IOException("cannot create " + root.getParentFile());
            Files.move(staged.toPath(), incoming.toPath(), StandardCopyOption.ATOMIC_MOVE);
            install(incoming, root);
            return backup;
        }
    }
    /** Put a backup back (the current set is backed up first, reason restore); the backup itself stays. */
    static Backup restore(File filesDir, Backup backup, long now) throws IOException {
        File staged = staging(filesDir, "restore");
        delete(staged);
        copyTree(backup.saves(), staged);
        try { return replace(filesDir, staged, "restore", now); }
        finally { delete(staged); }
    }

    // ---------- the save folder: one generation folder per sync ----------
    /** The save folder's top directory (TOP) through a provider that may have no atomic rename (Storage Access
     * Framework). Paths are '/'-separated and relative to a generation folder. */
    interface Store {
        /** Names of the folders directly inside TOP. */
        List<String> dirs() throws IOException;
        /** Every file under TOP/dir: relative path -> {size, modification time}. */
        Map<String, long[]> files(String dir) throws IOException;
        InputStream open(String dir, String path) throws IOException;
        void createDir(String dir) throws IOException;
        /** Create TOP/dir/path (a new file; parent folders are created) with source's bytes. */
        void write(String dir, String path, File source) throws IOException;
        void deleteDir(String dir) throws IOException;
    }
    /** The save folder as a real directory (All files access, a picked folder with a real path): plain file I/O instead
     * of the provider. Each file is written to a hidden temporary name, synced and renamed into place, so a file of a
     * generation is either complete or absent; temporary leftovers of a killed upload are never listed. */
    static final class FileStore implements Store {
        private static final String PARTIAL = ".partial";
        final File top;
        FileStore(File top) { this.top = top; }
        private File dir(String dir) throws IOException {
            if (dir.isEmpty() || dir.startsWith("/") || dir.contains("..")) throw new IOException("bad folder name " + dir);
            return new File(top, dir);
        }
        @Override public List<String> dirs() throws IOException {
            File[] children = top.listFiles();
            if (children == null) throw new IOException("cannot list " + top);
            List<String> out = new ArrayList<>();
            for (File c : children) if (c.isDirectory() && !Files.isSymbolicLink(c.toPath())) out.add(c.getName());
            return out;
        }
        @Override public Map<String, long[]> files(String dir) throws IOException {
            Map<String, long[]> out = new HashMap<>();
            list(dir(dir), "", out);
            return out;
        }
        private static void list(File d, String prefix, Map<String, long[]> out) throws IOException {
            File[] children = d.listFiles();
            if (children == null) throw new FileNotFoundException(d.getPath());
            for (File c : children) {
                if (out.size() > MAX_ENTRIES) throw new Invalid("too_many", "more than " + MAX_ENTRIES + " entries");
                if (Files.isSymbolicLink(c.toPath())) continue;
                if (c.isDirectory()) list(c, prefix + c.getName() + "/", out);
                else if (!(c.getName().startsWith(".") && c.getName().endsWith(PARTIAL))) out.put(prefix + c.getName(), new long[]{c.length(), c.lastModified()});
            }
        }
        @Override public InputStream open(String dir, String path) throws IOException {
            if (path.startsWith("/") || path.contains("..")) throw new IOException("bad file name " + path);
            return new FileInputStream(new File(dir(dir), path));
        }
        @Override public void createDir(String dir) throws IOException {
            File d = dir(dir);
            if (!d.mkdirs() && !d.isDirectory()) throw new IOException("cannot create " + d);
        }
        @Override public void write(String dir, String path, File source) throws IOException {
            if (path.startsWith("/") || path.contains("..")) throw new IOException("bad file name " + path);
            File out = new File(dir(dir), path), parent = out.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            File temp = new File(parent, "." + out.getName() + PARTIAL);
            try (InputStream in = new FileInputStream(source); FileOutputStream o = new FileOutputStream(temp)) {
                in.transferTo(o);
                o.getFD().sync();
            } catch (IOException failed) { temp.delete(); throw failed; }
            Files.move(temp.toPath(), out.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE);
        }
        @Override public void deleteDir(String dir) throws IOException {
            File d = dir(dir);
            delete(d);
            if (d.exists()) throw new IOException("cannot remove " + d);
        }
    }
    /** Every file under base (a picked folder with a real path) as import candidates; symbolic links are listed as
     * links (select() refuses them) and not followed. */
    static List<Candidate> candidates(File base) throws IOException {
        List<Candidate> out = new ArrayList<>();
        collect(base, "", out);
        return out;
    }
    private static void collect(File d, String prefix, List<Candidate> out) throws IOException {
        File[] children = d.listFiles();
        if (children == null) throw new FileNotFoundException(d.getPath());
        for (File c : children) {
            if (out.size() > MAX_ENTRIES) throw new Invalid("too_many", "more than " + MAX_ENTRIES + " entries");
            String rel = prefix + c.getName();
            if (Files.isSymbolicLink(c.toPath())) out.add(new Candidate(rel, 0, true, 0));
            else if (c.isDirectory()) collect(c, rel + "/", out);
            else out.add(new Candidate(rel, c.length(), false, c.lastModified()));
        }
    }
    static final String GENERATION = "sync-";
    private static final Pattern GENERATION_NAME = Pattern.compile("sync-(\\d{1,18})");
    static long generationNumber(String dir) {
        java.util.regex.Matcher m = GENERATION_NAME.matcher(dir);
        return m.matches() ? Long.parseLong(m.group(1)) : -1;
    }
    /** The folder as read: the newest complete generation (its manifest), or incomplete = the newest generation that
     * has a manifest does not match it (a mixed or damaged copy), which must not be used. */
    static final class FolderState {
        String generation; Manifest manifest; boolean incomplete; String problem = "";
        /** The highest generation number present (complete or not); the next push uses one more. */
        long highest;
    }
    /** Each push writes a new sync-NNNNNN folder, its files first and its manifest (every file's size and SHA-256)
     * last, and only then removes the older generations. So a generation without a readable manifest is an upload
     * that never finished: it is skipped and the previous one still stands. A generation whose manifest names files
     * that are missing or have another size is mixed: the folder is reported incomplete and nothing is taken from it. */
    static FolderState readFolder(Store store) throws IOException {
        FolderState s = new FolderState();
        List<String> gens = new ArrayList<>();
        for (String d : store.dirs()) {
            long n = generationNumber(d);
            if (n < 0) continue;
            gens.add(d);
            s.highest = Math.max(s.highest, n);
        }
        gens.sort((a, b) -> Long.compare(generationNumber(b), generationNumber(a)));
        for (String gen : gens) {
            Map<String, long[]> files = store.files(gen);
            if (!files.containsKey(MANIFEST)) continue;
            Manifest m;
            try (InputStream in = store.open(gen, MANIFEST)) { m = Manifest.parse(readText(in, 4L << 20)); }
            catch (Invalid torn) { continue; }   // the manifest write itself was interrupted
            for (FileEntry e : m.files) {
                long[] f = files.get(e.path);
                if (f == null || f[0] != e.size) { s.incomplete = true; s.problem = gen + "/" + e.path; return s; }
            }
            s.generation = gen; s.manifest = m;
            return s;
        }
        return s;
    }
    /** Download the current generation into staging (a new directory). verify: every file must match the manifest's
     * size and SHA-256 (Invalid "incomplete" otherwise); the paths pass the same checks as an import. */
    static void fetch(Store store, FolderState s, File staging, boolean verify) throws IOException {
        if (s.manifest == null) throw new Invalid("incomplete", "no complete copy in the folder");
        List<Candidate> candidates = new ArrayList<>();
        for (FileEntry e : s.manifest.files) candidates.add(new Candidate(e.path, e.size, false, e.mtime));
        Selection selection = select(candidates);
        if (!selection.root.isEmpty() || selection.files.size() != s.manifest.files.size()) throw new Invalid("incomplete", "unexpected layout in " + s.generation);
        delete(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        try {
            for (Candidate c : selection.files)
                try (InputStream in = store.open(s.generation, c.path)) { copyLimited(in, new File(staging, c.path), c.size, c.mtime); }
            if (verify) {
                Map<String, FileEntry> want = s.manifest.byPath();
                for (FileEntry got : scan(staging, null))
                    if (!got.sameContent(want.get(got.path))) throw new Invalid("incomplete", s.generation + "/" + got.path + " does not match its manifest");
            }
        } catch (Invalid invalid) {
            delete(staging);
            throw invalid.code.equals("incomplete") ? invalid : new Invalid("incomplete", invalid.getMessage());
        } catch (IOException failed) { delete(staging); throw failed; }
    }
    /** Upload a snapshot as generation highest + 1: files, then the manifest, then remove every other generation
     * (complete, unfinished or mixed). Returns the new generation's name. */
    static String push(Store store, File snapshot, Manifest m, FolderState current) throws IOException {
        String gen = String.format(Locale.ROOT, GENERATION + "%06d", current.highest + 1);
        store.createDir(gen);
        for (FileEntry e : m.files) store.write(gen, e.path, new File(snapshot, e.path));
        File manifest = new File(snapshot.getParentFile(), snapshot.getName() + "-" + MANIFEST);
        try {
            Settings.writeAtomic(manifest.getParentFile(), manifest.getName(), m.toJson());
            store.write(gen, MANIFEST, manifest);
        } finally { manifest.delete(); }
        for (String d : store.dirs())
            if (!d.equals(gen) && generationNumber(d) >= 0)
                try { store.deleteDir(d); } catch (IOException kept) { /* harmless: the newest complete generation wins */ }
        return gen;
    }
    /** Slots described by a manifest (no pictures): slot_NN/progress.sav entries and their times. */
    static List<Slot> slots(Manifest m) {
        List<Slot> out = new ArrayList<>();
        if (m == null) return out;
        for (FileEntry e : m.files)
            if (e.path.matches("slot_\\d\\d/progress\\.sav")) out.add(new Slot(Integer.parseInt(e.path.substring(5, 7)), e.mtime, e.mtime, null));
        out.sort((a, b) -> Integer.compare(a.index, b.index));
        return out;
    }

    // ---------- in-game snapshots ----------
    /** The runtime's save signal: state/save-generation (bumped after each completed save) and state/save-busy
     * (present from the first change of a save until its commit has finished). */
    interface Signal { String generation(); boolean busy(); }
    static Signal fileSignal(File filesDir) {
        File state = new File(filesDir, "state");
        return new Signal() {
            @Override public String generation() {
                try { return new String(Files.readAllBytes(new File(state, "save-generation").toPath()), StandardCharsets.US_ASCII).trim(); }
                catch (IOException none) { return ""; }
            }
            @Override public boolean busy() { return new File(state, "save-busy").exists(); }
        };
    }
    /** A snapshot that is known to be one whole save: the generation is recorded and the game must not be saving
     * before the copy, and afterwards the generation must be unchanged and still no save in progress (plus the
     * per-file size/time checks of snapshot()). Returns null when that does not hold: nothing is kept, retry later. */
    static Manifest stableSnapshot(File root, File target, Signal signal, String appVersion, String device, long now) {
        String before = signal.generation();
        if (signal.busy()) return null;
        Manifest m;
        try { m = snapshot(root, target, appVersion, device, now, 1); }
        catch (IOException changed) { delete(target); return null; }
        if (signal.busy() || !before.equals(signal.generation())) { delete(target); return null; }
        return m;
    }

    // ---------- the mirror's decision ----------
    enum Decision { NONE, PUSH, PULL, CONFLICT }
    /** live = the device's saves, folder = the save folder's set (null when it holds none), last = both sides at the
     * last successful sync with this folder (null = never synced with it).
     * In sync, or nothing anywhere: NONE. Empty folder: PUSH. Empty device: PULL (an empty side never overwrites the
     * other, so neither side's saves can be wiped by a sync). First sync with a folder that already holds different
     * saves: CONFLICT. Afterwards whichever side changed since the last sync wins; both changed: CONFLICT. */
    static Decision decide(Manifest live, Manifest folder, Manifest last) {
        boolean liveEmpty = live == null || live.files.isEmpty(), folderEmpty = folder == null || folder.files.isEmpty();
        if (liveEmpty && folderEmpty) return Decision.NONE;
        if (folderEmpty) return Decision.PUSH;
        if (liveEmpty) return Decision.PULL;
        if (live.sameFiles(folder)) return Decision.NONE;
        if (last == null) return Decision.CONFLICT;
        boolean liveChanged = !live.sameFiles(last), folderChanged = !folder.sameFiles(last);
        if (!liveChanged) return folderChanged ? Decision.PULL : Decision.NONE;
        return folderChanged ? Decision.CONFLICT : Decision.PUSH;
    }

}

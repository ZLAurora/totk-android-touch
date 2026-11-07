package com.aure.totk.setup;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipException;
import java.util.zip.ZipFile;

/** Android-free custom-driver import. Archive paths are lookup keys only, never output paths. */
final class DriverImport {
    static final long MAX_DRIVER = 128L << 20, MAX_ARCHIVE = 160L << 20;
    static final int MAX_ENTRIES = 256, MAX_META = 64 << 10;

    static final class Invalid extends IOException {
        final boolean zip;
        Invalid(boolean zip) { super(zip ? "no usable driver in zip" : "not an ELF64 AArch64 driver"); this.zip = zip; }
    }

    /** Owns/closes source. Until every read and close succeeds, target and the caller's label remain untouched.
     * The caller holds GameLock throughout this operation and stores the returned label only on success. */
    static String install(InputStream source, File target, File archive, String fallback) throws IOException {
        File temp = new File(target.getPath() + ".tmp");
        String label = fallback;
        try {
            try (InputStream in = new BufferedInputStream(source)) {
                in.mark(4);
                byte[] magic = in.readNBytes(4);
                in.reset();
                if (magic.length >= 2 && magic[0] == 'P' && magic[1] == 'K') {
                    try {
                        try (OutputStream out = new FileOutputStream(archive)) { copy(in, out, MAX_ARCHIVE, null); }
                        checkDirectory(archive);
                        try (ZipFile zip = new ZipFile(archive)) {
                            label = extract(zip, temp, fallback);
                        }
                    } catch (Invalid | ZipException | EOFException | RuntimeException | OutOfMemoryError badZip) {
                        throw new Invalid(true);
                    }
                } else {
                    try (OutputStream out = new FileOutputStream(temp)) { driver(in, out, null); }
                }
            }
            if (!temp.renameTo(target)) throw new IOException("driver rename failed");
            return label;
        } finally {
            temp.delete();
            archive.delete();
        }
    }

    /** Bound the directory before ZipFile allocates its index. Small driver packages need neither ZIP64 nor disks. */
    private static void checkDirectory(File archive) throws IOException {
        try (RandomAccessFile file = new RandomAccessFile(archive, "r")) {
            int size = (int) Math.min(file.length(), 65557);
            byte[] end = new byte[size];
            file.seek(file.length() - size); file.readFully(end);
            int footer = -1;
            for (int p = size - 22; p >= 0; p--) {
                if (u32(end, p) != 0x06054b50L) continue;
                // Android may accept an EOCD whose comment does not reach EOF. Never let ZipFile choose a
                // different, unchecked directory hidden in the validated record's comment (or elsewhere in the tail).
                if (footer != -1) throw new Invalid(true);
                footer = p;
            }
            if (footer < 0 || footer + 22 + u16(end, footer + 20) != size) throw new Invalid(true);
            int count = u16(end, footer + 10);
            if (u16(end, footer + 4) != 0 || u16(end, footer + 6) != 0 || u16(end, footer + 8) != count
                    || count > MAX_ENTRIES || u32(end, footer + 12) > (2 << 20)
                    || u32(end, footer + 16) + u32(end, footer + 12) != file.length() - size + footer)
                throw new Invalid(true);
        }
    }
    private static int u16(byte[] b, int p) { return (b[p] & 255) | (b[p + 1] & 255) << 8; }
    private static long u32(byte[] b, int p) { return (u16(b, p) & 0xffffL) | (long) u16(b, p + 2) << 16; }

    private static String extract(ZipFile zip, File temp, String fallback) throws IOException {
        if (zip.size() > MAX_ENTRIES) throw new Invalid(true);
        ZipEntry meta = null, only = null;
        int libraries = 0;
        Set<String> names = new HashSet<>();
        for (Enumeration<? extends ZipEntry> entries = zip.entries(); entries.hasMoreElements();) {
            ZipEntry entry = entries.nextElement();
            if (!names.add(entry.getName())) throw new Invalid(true);
            if (entry.isDirectory()) continue;
            if (entry.getName().equals("meta.json")) meta = entry;
            if (entry.getName().toLowerCase(Locale.ROOT).endsWith(".so")) { only = entry; libraries++; }
        }
        String label = fallback;
        ZipEntry chosen;
        if (meta != null) {
            if (meta.getSize() > MAX_META) throw new Invalid(true);
            ByteArrayOutputStream text = new ByteArrayOutputStream();
            try (InputStream in = zip.getInputStream(meta)) { copy(in, text, MAX_META, meta); }
            Map<?, ?> values = metadata(text.toString(StandardCharsets.UTF_8));
            String library = string(values, "libraryName");
            chosen = library.isEmpty() ? null : zip.getEntry(library);
            String name = string(values, "name"), version = string(values, "driverVersion");
            if (!name.isEmpty()) label = name + (version.isEmpty() ? "" : " · " + version);
        } else chosen = libraries == 1 ? only : null;
        if (chosen == null || chosen.isDirectory() || chosen == meta || chosen.getSize() > MAX_DRIVER)
            throw new Invalid(true);
        try (InputStream in = zip.getInputStream(chosen); OutputStream out = new FileOutputStream(temp)) {
            driver(in, out, chosen);
        }
        return label;
    }

    private static Map<?, ?> metadata(String text) throws Invalid {
        // Bound recursive parsing as well as bytes; reuse the save manifest's host-tested JSON reader.
        boolean quoted = false, escape = false;
        int depth = 0;
        for (int n = 0; n < text.length(); n++) {
            char c = text.charAt(n);
            if (quoted) {
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if ((c == '{' || c == '[') && ++depth > 32) throw new Invalid(true);
            else if (c == '}' || c == ']') depth--;
        }
        try {
            Object values = new SaveManager.Json(text).document();
            if (values instanceof Map) return (Map<?, ?>) values;
        } catch (RuntimeException malformed) { throw new Invalid(true); }
        throw new Invalid(true);
    }
    private static String string(Map<?, ?> values, String key) {
        Object value = values.get(key);
        return value instanceof String ? ((String) value).trim() : "";
    }

    private static void driver(InputStream in, OutputStream out, ZipEntry entry) throws IOException {
        byte[] header = in.readNBytes(64);
        if (header.length != 64 || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F'
                || header[4] != 2 || header[5] != 1 || (header[18] & 255) != 183 || header[19] != 0)
            throw new Invalid(entry != null);
        CRC32 crc = new CRC32(); crc.update(header);
        out.write(header);
        long size = header.length + copy(in, out, MAX_DRIVER - header.length, null, crc);
        checkEntry(entry, size, crc);
    }
    private static long copy(InputStream in, OutputStream out, long limit, ZipEntry entry) throws IOException {
        CRC32 crc = new CRC32();
        long size = copy(in, out, limit, entry, crc);
        checkEntry(entry, size, crc);
        return size;
    }
    private static long copy(InputStream in, OutputStream out, long limit, ZipEntry entry, CRC32 crc) throws IOException {
        byte[] buffer = new byte[64 << 10];
        long size = 0;
        for (int read; (read = in.read(buffer)) != -1;) {
            size += read;
            if (size > limit) throw new Invalid(entry != null);
            out.write(buffer, 0, read); crc.update(buffer, 0, read);
        }
        return size;
    }
    private static void checkEntry(ZipEntry entry, long size, CRC32 crc) throws Invalid {
        if (entry != null && (size != entry.getSize() || crc.getValue() != entry.getCrc())) throw new Invalid(true);
    }
}

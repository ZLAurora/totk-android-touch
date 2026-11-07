package com.aure.totk.setup;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Whether the second-screen art in <game>/companion was made by this app's generator. The art is generated on the
 * device once (setup's companion stage); an app update that changes the generator (names, icons) leaves the old art in
 * place. InstallState distinguishes missing art from old-but-present art, which Home updates only on dual-screen
 * devices. Plain Java without Android types so the host test can run it (tests/test_android_companion_art.py). */
final class CompanionArt {
    private CompanionArt() { }

    /** setup/companion/stage_companion.c GENERATOR_VERSION (and scripts/make_companion_assets.py); the host test keeps
     * the three equal. */
    static final int GENERATOR = 5;
    /** index.json begins {"schema":1,"generator":N,...}; the rest of the file (hundreds of KB) is never read. */
    private static final int HEAD = 4096;
    private static final Pattern FIELD = Pattern.compile("\"generator\"\\s*:\\s*(\\d{1,9})[\\s,}]");

    /** The generator that wrote dir/index.json, or -1 if the index is missing or unreadable. */
    static int generator(File dir) {
        byte[] head = new byte[HEAD];
        int n = 0;
        try (InputStream in = new FileInputStream(new File(dir, "index.json"))) {
            for (int r; n < head.length && (r = in.read(head, n, head.length - n)) > 0; ) n += r;
        } catch (IOException missing) {
            return -1;
        }
        String text = new String(head, 0, n, StandardCharsets.UTF_8);
        if (!text.startsWith("{")) return -1;
        Matcher m = FIELD.matcher(text);
        return m.find() ? Integer.parseInt(m.group(1)) : -1;
    }
    /** The art in dir was made by this generator or a newer one. */
    static boolean current(File dir) { return generator(dir) >= GENERATOR; }
}

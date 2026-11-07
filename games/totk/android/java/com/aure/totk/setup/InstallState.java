package com.aure.totk.setup;

import android.app.ActivityManager;
import android.content.Context;
import android.os.StatFs;
import org.json.JSONObject;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Read-only view of <game>/install.json (format 1, written by the native setup) plus the on-disk facts the UI needs. */
final class InstallState {
    /** CORE: the base import. PAIR: the two stages built after it, side by side (setup.c run_companion_and_shaders). The
     *  game needs CORE and the shader seed (without it nothing is drawn); the companion art is the only optional stage. */
    static final String[] CORE = {"code", "romfs", "index", "fonts"}, PAIR = {"companion", "shaders"};
    static final String OPTIONAL = "companion";
    static final long ROMFS_SIZE = 17_052_625_232L;
    // Rough output sizes for the free-space check; staging covers temporary files inside <game>/.import.
    private static final long CODE_BYTES = 300_000_000L, COMPANION_BYTES = 400_000_000L, SHADER_BYTES = 300_000_000L, STAGING_BYTES = 1_000_000_000L;

    final JSONObject json;
    private final File game;
    private ArtPolicy.State art;   // read once per snapshot (index.json head)

    private InstallState(File game, JSONObject json) { this.game = game; this.json = json; }

    static File gameDir(Context context) { return StorageLocation.gameDir(context); }
    static InstallState read(Context context) {
        File game = gameDir(context);
        JSONObject json = new JSONObject();
        try { json = new JSONObject(new String(Files.readAllBytes(new File(game, "install.json").toPath()), StandardCharsets.UTF_8)); }
        catch (Exception missing) { }
        return new InstallState(game, json);
    }
    private JSONObject stage(String name) {
        JSONObject stages = json.optJSONObject("stages");
        JSONObject stage = stages == null ? null : stages.optJSONObject(name);
        return stage == null ? new JSONObject() : stage;
    }
    /** Stage done per install.json; the second-screen art also has to be from this app's generator (artOutdated). */
    boolean done(String name) { return OPTIONAL.equals(name) ? artState() == ArtPolicy.State.DONE : stage(name).optBoolean("done"); }
    /** Old-but-present art remains usable until a dual-screen device updates it. Missing art is offered before play. */
    boolean artOutdated() { return artState() == ArtPolicy.State.OUTDATED; }
    boolean skipped(String name) { return stage(name).optBoolean("skipped"); }
    ArtPolicy.State artState() {
        if (art == null) {
            File dir = new File(game, "companion");
            art = ArtPolicy.state(skipped(OPTIONAL), !errorCode(OPTIONAL).isEmpty(),
                    new File(dir, "index.json").isFile(), CompanionArt.current(dir));
        }
        return art;
    }
    String errorCode(String name) { return stage(name).optString("code", ""); }
    String errorText(String name) { return stage(name).optString("error", ""); }
    int shaderVariants() { return stage("shaders").optInt("variants"); }
    String route() { return json.optString("route", ""); }
    int setupVersion() { return json.optInt("setup_version"); }
    boolean anyDone() {
        for (String name : CORE) if (done(name)) return true;
        return false;
    }

    /** The base import is in place: the manifest's stages plus a cheap check that the big outputs are still there (users
     * can delete files by hand). The shader seed may still be missing (installs from before it was required). */
    boolean installed() {
        if (json.optInt("format") != 1 || !"totk".equals(json.optString("game")) || !"v143".equals(json.optString("version"))) return false;
        for (String name : CORE) if (!done(name)) return false;
        if (new File(game, "extracted/v143/romfs/romfs.bin").length() != ROMFS_SIZE) return false;
        for (String module : new String[]{"main", "sdk", "subsdk0"})
            if (!new File(game, "rewritten/v143/" + module + ".patched.bin").isFile()) return false;
        return new File(game, "game-data/v143/runtime-tables.bin").isFile();
    }
    /** Installed with the shader seed built, and the manifest's word. */
    boolean playable() { return installed() && done("shaders") && json.optBoolean("playable"); }
    /** Installed, but the shader seed was never built (or its stage failed or was cancelled): Home offers Build shaders. */
    boolean shadersMissing() { return installed() && !done("shaders"); }

    /** Whether <game>/shaders/v143 holds what the runtime's seed reader needs (gfx/vk_seed_io.c: at least one .spv module,
     * from a TOTKVKP1 seed.vkpack or loose files); without one the game starts on a black screen. Checked right before
     * the game starts, so a seed removed or emptied by hand is refused with a message. */
    boolean seedUsable() {
        File dir = new File(game, "shaders/v143");
        if (packHasModule(new File(dir, "seed.vkpack"))) return true;
        String[] names = dir.list();
        if (names == null) return false;
        for (String name : names) if (name.endsWith(".spv") && new File(dir, name).isFile()) return true;
        return false;
    }
    /** TOTKVKP1 (scripts/pack_shader_seed.py): <8sIIQQ> header (files at 16), then <84sIQQ> records named *.spv/.vk/.vv. */
    private static boolean packHasModule(File pack) {
        try (java.io.DataInputStream in = new java.io.DataInputStream(new java.io.BufferedInputStream(new java.io.FileInputStream(pack), 1 << 16))) {
            byte[] header = new byte[32];
            in.readFully(header);
            if (!new String(header, 0, 8, StandardCharsets.US_ASCII).equals("TOTKVKP1")) return false;
            long files = java.nio.ByteBuffer.wrap(header, 16, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN).getLong();
            byte[] record = new byte[104];
            for (long i = 0; i < files && i < (1 << 24); i++) {
                in.readFully(record);
                int n = 0;
                while (n < 84 && record[n] != 0) n++;
                if (n > 4 && record[n - 4] == '.' && record[n - 3] == 's' && record[n - 2] == 'p' && record[n - 1] == 'v') return true;
            }
        } catch (java.io.IOException unreadable) { }
        return false;
    }

    long freeBytes() {
        try { game.mkdirs(); return new StatFs(game.getPath()).getAvailableBytes(); }
        catch (IllegalArgumentException unavailable) { return 0; }
    }
    long totalBytes() {
        try { game.mkdirs(); return new StatFs(game.getPath()).getTotalBytes(); }
        catch (IllegalArgumentException unavailable) { return 0; }
    }
    /** Bytes still to be written by the pending stages, plus staging headroom; leftover staging is discarded on resume. */
    long neededBytes() {
        long need = STAGING_BYTES;
        if (!done("romfs")) need += ROMFS_SIZE - Math.max(0, Math.min(ROMFS_SIZE, new File(game, "extracted/v143/romfs/romfs.bin").length()));
        if (!done("code") || !done("index") || !done("fonts")) need += CODE_BYTES;
        if (!done("companion")) need += COMPANION_BYTES;
        if (!done("shaders")) need += SHADER_BYTES;
        return Math.max(0, need - size(new File(game, ".import")));
    }
    private static long size(File file) {
        File[] children = file.listFiles();
        if (children == null) return file.isFile() ? file.length() : 0;
        long total = 0;
        for (File child : children) total += size(child);
        return total;
    }

    /** Busy/error decisions are operational warnings, not optional INFO chatter. Include effective levels so a plain
     * `logcat -s TotkSetup` capture can diagnose a device/property filter rather than silently losing the branch. */
    static void logGameDecision(String message) {
        android.util.Log.w("TotkSetup", message + " logInfoEnabled=" + android.util.Log.isLoggable("TotkSetup", android.util.Log.INFO)
                + " logWarnEnabled=" + android.util.Log.isLoggable("TotkSetup", android.util.Log.WARN));
    }

    /** Only an acquired game lock or a live start handshake proves the game is running, not a cached default process. */
    static GameLock.Result gameState(Context context) {
        GameLock.logger = InstallState::logGameDecision;
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        boolean alive = false;
        StringBuilder processes = new StringBuilder();
        java.util.List<ActivityManager.RunningAppProcessInfo> running = manager == null ? null : manager.getRunningAppProcesses();
        if (running != null) for (ActivityManager.RunningAppProcessInfo process : running)
            if (context.getPackageName().equals(process.processName)) {
                alive = true;
                processes.append(" pid=").append(process.pid).append(" process=").append(process.processName)
                        .append(" importance=").append(process.importance);
            }
        GameLock.Result result = GameLock.check(context.getFilesDir(), alive);
        if (result.state != GameLock.State.FREE)
            logGameDecision("game decision=" + result.state + " checkerPid=" + android.os.Process.myPid()
                    + (alive ? processes : " defaultProcess=absent") + " " + result.reason);
        return result;
    }

    /** Whether libtotk_setup.so can be loaded (a preview build may lack it). */
    static boolean libraryAvailable() {
        try { Class.forName(NativeSetup.class.getName(), true, NativeSetup.class.getClassLoader()); return true; }
        catch (ClassNotFoundException | LinkageError missing) { return false; }
    }
}

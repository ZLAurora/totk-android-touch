package com.aure.totk.setup;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.Icon;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.List;

/** Foreground (dataSync) service that runs NativeSetup.run on a worker thread, in the ":setup" process.
 * It survives the activity going to the background; if the process dies, the persisted job lets the UI offer a resume. */
public final class SetupService extends Service {
    static final String ACTION_RUN = "com.aure.totk.setup.RUN", ACTION_CANCEL = "com.aure.totk.setup.CANCEL";
    static final String EXTRA_STAGES = "stages", EXTRA_MOVE_TO = "moveTo", EXTRA_MOVE_NAME = "moveName";
    /** status.stages / jobStages of a game-data move (Game data > Storage); NativeSetup.move instead of run. */
    static final String[] MOVE = {"move"};
    private static final String CHANNEL = "setup";
    private static final int NOTIFY_PROGRESS = 1, NOTIFY_RESULT = 2;

    /** Live state for SetupActivity (same process). Mutated and read on the main thread only. stage/done/total/detail
     *  follow the stage the ring and the notification show; lanes hold every stage's own progress (companion and shaders
     *  run at the same time and report in turns). */
    static final class Status {
        boolean running, cancelling;
        String stage = "", detail = "";
        long done, total, stageStartedAt, stageStartDone;
        String[] stages;
        final java.util.Map<String, Lane> lanes = new java.util.HashMap<>();
    }
    /** One stage's progress; finished once native reports it "done", "skipped" (companion) or "failed" (shaders). */
    static final class Lane {
        long done, total, startedAt, startDone;
        String detail = "";
        boolean finished;
    }
    interface Listener { void onStatus(); }
    static final Status status = new Status();
    private static final List<Listener> listeners = new ArrayList<>();
    static void listen(Listener listener) { listeners.add(listener); }
    static void unlisten(Listener listener) { listeners.remove(listener); }
    private static void changed() { for (Listener listener : new ArrayList<>(listeners)) listener.onStatus(); }

    /** stages == null runs every pending stage (the base import, then companion + shaders). */
    static void start(Context context, String[] stages) {
        Intent intent = new Intent(context, SetupService.class).setAction(ACTION_RUN);
        if (stages != null) intent.putExtra(EXTRA_STAGES, stages);
        context.startForegroundService(intent);
    }
    /** Move the game data to another place (one of StorageLocation.volumes, or a picked folder). */
    static void startMove(Context context, StorageLocation.Volume to) {
        StorageLocation.recordMove(context, StorageLocation.gameDir(context), to.dir);
        context.startForegroundService(new Intent(context, SetupService.class).setAction(ACTION_RUN)
                .putExtra(EXTRA_MOVE_TO, to.dir.getPath()).putExtra(EXTRA_MOVE_NAME, to.name));
    }
    static boolean moving() { return status.stages != null && java.util.Arrays.equals(status.stages, MOVE); }
    static boolean moveJob(Context context) { return java.util.Arrays.equals(jobStages(context), MOVE); }
    /** Forget an interrupted move: a move is never resumed (it starts from scratch, and the source was never touched). */
    static void clearJob(Context context) { Inputs.prefs(context).edit().putBoolean("jobActive", false).remove("jobStages").commit(); }
    static void cancel(Context context) {
        if (!status.running) return;
        context.startService(new Intent(context, SetupService.class).setAction(ACTION_CANCEL));
    }

    // Persisted job/result: survive process death so the UI can tell "interrupted" from "never started".
    static boolean jobActive(Context context) { return Inputs.prefs(context).getBoolean("jobActive", false); }
    static String[] jobStages(Context context) {
        try {
            String raw = Inputs.prefs(context).getString("jobStages", null);
            if (raw == null) return null;
            JSONArray list = new JSONArray(raw);
            String[] stages = new String[list.length()];
            for (int i = 0; i < stages.length; i++) stages[i] = list.getString(i);
            return stages;
        } catch (JSONException corrupt) { return null; }
    }
    static JSONObject lastResult(Context context) {
        try { return new JSONObject(Inputs.prefs(context).getString("result", "{}")); }
        catch (JSONException corrupt) { return new JSONObject(); }
    }
    static void clearResult(Context context) {
        Inputs.prefs(context).edit().remove("result").apply();
        NotificationManager notifications = context.getSystemService(NotificationManager.class);
        if (notifications != null) notifications.cancel(NOTIFY_RESULT);
    }
    static String failure(String stage, String code, String message) {
        try { return new JSONObject().put("ok", false).put("stage", stage).put("code", code).put("message", message).toString(); }
        catch (JSONException impossible) { throw new IllegalStateException(impossible); }
    }

    private static final Handler main = new Handler(Looper.getMainLooper());
    private PowerManager.WakeLock wake;
    private Notification.Builder progress;
    private long notifiedAt;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int id) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_CANCEL.equals(action)) {
            if (status.running) {
                status.cancelling = true; changed();
                try { NativeSetup.cancel(); } catch (LinkageError missing) { }
            } else stopSelf(id);
            return START_NOT_STICKY;
        }
        // Every startForegroundService call must be answered with startForeground, even a duplicate.
        startForeground(NOTIFY_PROGRESS, progressNotification().build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        if (status.running) return START_NOT_STICKY;
        String moveTo = intent == null ? null : intent.getStringExtra(EXTRA_MOVE_TO);
        String[] stages = moveTo != null ? MOVE : intent == null ? null : intent.getStringArrayExtra(EXTRA_STAGES);
        begin(stages, moveTo, intent == null ? null : intent.getStringExtra(EXTRA_MOVE_NAME));
        return START_NOT_STICKY;
    }

    private void begin(String[] stages, String moveTo, String moveName) {
        status.running = true; status.cancelling = false; status.stages = stages;
        status.stage = moveTo != null ? "move" : "inspect"; status.detail = ""; status.done = status.total = 0;
        status.stageStartedAt = SystemClock.elapsedRealtime(); status.stageStartDone = 0; status.lanes.clear();
        SharedPreferences.Editor job = Inputs.prefs(this).edit().putBoolean("jobActive", true).remove("result");
        if (stages != null) job.putString("jobStages", new JSONArray(java.util.Arrays.asList(stages)).toString()); else job.remove("jobStages");
        job.commit();  // must be on disk before native work starts: a crash mid-import has to read as "interrupted"
        changed();
        PowerManager power = getSystemService(PowerManager.class);
        wake = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "totk:setup");
        wake.acquire(6 * 3600 * 1000L);
        Context context = getApplicationContext();
        new Thread(() -> {
            String result;
            GameLock.Result state = InstallState.gameState(context);
            if (!state.free()) result = failure(moveTo != null ? "move" : "",
                    state.state == GameLock.State.GAME ? "game_running" : "game_lock", state.reason);
            else try (GameLock.Hold held = GameLock.hold(context.getFilesDir())) {
                if (!held.result.free()) result = failure(moveTo != null ? "move" : "",
                        held.result.state == GameLock.State.GAME ? "game_running" : "game_lock", held.result.reason);
                else result = moveTo != null ? move(context, moveTo, moveName) : work(context, stages);
            } catch (java.io.IOException failed) { result = failure(moveTo != null ? "move" : "", "game_lock", failed.toString()); }
            String completed = result;
            main.post(() -> finish(completed));
        }, "totk-setup").start();
    }

    private static String work(Context context, String[] stages) {
        if (!InstallState.libraryAvailable()) return failure("", "library", "libtotk_setup.so could not be loaded");
        Inputs inputs = Inputs.load(context);
        try (Inputs.Opened opened = inputs.open(context, true, stages)) {
            android.util.Log.i("TotkSetup", "inputs: " + opened.byPath + " by path (All files access " + (FileAccess.granted() ? "on" : "off") + ")");
            opened.request.put("skipCompanion", ArtPolicy.skipSetup(SettingsStore.secondaryDisplay(context), stages != null));
            String result = NativeSetup.run(opened.request.toString(), (stage, done, total, detail) ->
                    main.post(() -> progress(context, stage, done, total, detail)));
            JSONObject parsed = new JSONObject(result == null ? "{}" : result);
            // A lost SAF grant shows up natively as a missing input; name the real cause.
            if (!parsed.optBoolean("ok") && "missing_input".equals(parsed.optString("code")) && !opened.unreadable.isEmpty())
                return failure(parsed.optString("stage"), "permission", "cannot open: " + String.join(", ", opened.unreadable));
            return parsed.toString();
        } catch (JSONException | RuntimeException failed) {
            return failure("", "internal", String.valueOf(failed.getMessage()));
        }
    }
    /** Copy the game data to `to` (native: verified, install.json last), then switch to it — the pref and settings.env
     * first, so the game can never start on a copy that is about to be removed — and only then remove the old copy. */
    private static String move(Context context, String to, String name) {
        if (!InstallState.libraryAvailable()) return failure("move", "library", "libtotk_setup.so could not be loaded");
        java.io.File from = InstallState.gameDir(context), target = new java.io.File(to);
        StorageLocation.Volume volume = StorageLocation.target(context, target, name);
        if (volume == null) return failure("move", StorageLocation.kind(context, target) == StoragePaths.Kind.FOLDER && !FileAccess.granted()
                ? "access" : "not_available", "the target storage is not available");
        try {
            JSONObject request = new JSONObject().put("from", from.getPath()).put("to", to).put("privateDir", context.getFilesDir().getPath())
                    .put("romfsPartBytes", StorageLocation.partBytes(context, target)).put("gameLockHeld", true);
            JSONObject parsed = new JSONObject(String.valueOf(NativeSetup.move(request.toString(), (stage, done, total, detail) ->
                    main.post(() -> progress(context, stage, done, total, detail)))));
            if (!parsed.optBoolean("ok")) return parsed.toString();
            String[] old = StorageLocation.saved(context);
            String oldLabel = StorageLocation.label(context);
            StorageLocation.choose(context, volume);
            String envFailed = SettingsStore.writeEnv(context, SettingsStore.load(context));
            if (envFailed != null) {
                // The game would still read the old folder: switch back and keep it (the new copy is cleaned by the next move).
                StorageLocation.restore(context, old);
                return failure("move", "io", "settings.env not written: " + envFailed + " (still on " + oldLabel + ")");
            }
            // The switch is done; a failed removal only leaves a stale copy behind, so it never turns the move into a failure.
            JSONObject removed;
            try { removed = new JSONObject(String.valueOf(NativeSetup.removeGameData(from.getPath()))); }
            catch (JSONException | RuntimeException | LinkageError failed) { removed = new JSONObject().put("ok", false).put("message", String.valueOf(failed.getMessage())); }
            if (!removed.optBoolean("ok")) android.util.Log.w("TotkSetup", "old game data not removed: " + removed.optString("message"));
            return new JSONObject().put("ok", true).put("stage", "move").put("to", to).put("name", volume.name)
                    .put("removedOld", removed.optBoolean("ok")).put("removeMessage", removed.optString("message", "")).toString();
        } catch (JSONException | RuntimeException | LinkageError failed) {
            return failure("move", "internal", String.valueOf(failed.getMessage()));
        }
    }

    private static SetupService instance;
    @Override public void onCreate() { super.onCreate(); instance = this; }
    @Override public void onDestroy() { if (instance == this) instance = null; super.onDestroy(); }

    private static void progress(Context context, String stage, long done, long total, String detail) {
        detail = detail == null ? "" : detail;
        Lane lane = status.lanes.get(stage);
        if (lane == null) {
            lane = new Lane(); lane.startedAt = SystemClock.elapsedRealtime(); lane.startDone = done;
            status.lanes.put(stage, lane);
            android.util.Log.i("TotkSetup", "stage " + stage);  // one line per stage: lets a device log show the timeline
        }
        lane.done = done; lane.total = total; lane.detail = detail;
        lane.finished = detail.equals("done") || detail.equals("skipped") || detail.equals("failed");
        // The ring and the notification follow the shader stage while it runs next to the companion stage (it is the
        // longer one); when the shown one of the two ends, they move to the other one if it still works.
        Lane shown = status.lanes.get(status.stage);
        boolean keep = !stage.equals(status.stage) && shown != null && !shown.finished && status.stage.equals("shaders");
        if (lane.finished && stage.equals(status.stage)) {
            for (String other : InstallState.PAIR) {
                Lane l = status.lanes.get(other);
                if (l != null && !l.finished) { stage = other; lane = l; break; }
            }
        }
        if (!keep) {
            status.stage = stage; status.done = lane.done; status.total = lane.total; status.detail = lane.detail;
            status.stageStartedAt = lane.startedAt; status.stageStartDone = lane.startDone;
        }
        changed();
        if (instance != null) instance.updateNotification();
    }

    private void finish(String result) {
        status.running = false; status.cancelling = false;
        // jobStages stays: it is also the "last requested stages" an error screen resumes with.
        Inputs.prefs(this).edit().putBoolean("jobActive", false).putString("result", result).commit();
        if (wake != null && wake.isHeld()) wake.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        JSONObject parsed;
        try { parsed = new JSONObject(result); } catch (JSONException corrupt) { parsed = new JSONObject(); }
        boolean ok = parsed.optBoolean("ok");
        if (!"cancelled".equals(parsed.optString("code"))) {
            boolean move = moving();
            Notification.Builder done = builder().setContentTitle(ok ? (move ? Strings.NOTIFY_MOVED : Strings.NOTIFY_DONE) : move ? Strings.NOTIFY_MOVE_FAILED : Strings.NOTIFY_FAILED)
                    .setContentText(ok ? (move ? Strings.moved(parsed.optString("name")) : Strings.DONE_TITLE) : Strings.error(parsed.optString("code"))).setAutoCancel(true);
            NotificationManager notifications = getSystemService(NotificationManager.class);
            if (notifications != null) notifications.notify(NOTIFY_RESULT, done.build());
        }
        changed();
        stopSelf();
    }

    private Notification.Builder builder() {
        NotificationManager notifications = getSystemService(NotificationManager.class);
        if (notifications != null && notifications.getNotificationChannel(CHANNEL) == null)
            notifications.createNotificationChannel(new NotificationChannel(CHANNEL, Strings.CHANNEL, NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, SetupActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL).setSmallIcon(smallIcon()).setContentIntent(open)
                .setColor(Theme.ZONAI).setShowWhen(false).setOnlyAlertOnce(true);
    }
    private Notification.Builder progressNotification() {
        if (progress == null) {
            PendingIntent cancel = PendingIntent.getService(this, 1, new Intent(this, SetupService.class).setAction(ACTION_CANCEL), PendingIntent.FLAG_IMMUTABLE);
            progress = builder().setContentTitle(Strings.NOTIFY_RUNNING).setOngoing(true).setCategory(Notification.CATEGORY_PROGRESS)
                    .addAction(new Notification.Action.Builder(null, Strings.CANCEL, cancel).build());
        }
        boolean known = status.total > 0;
        int permille = known ? (int) Math.min(1000, status.done * 1000 / status.total) : 0;
        return progress.setProgress(1000, permille, !known).setContentTitle(moving() ? Strings.NOTIFY_MOVING : Strings.NOTIFY_RUNNING)
                .setContentText(Strings.stage(status.stage) + (known ? " · " + permille / 10 + "%" : ""));
    }
    private void updateNotification() {
        long now = SystemClock.elapsedRealtime();
        if (now - notifiedAt < 1000 || !status.running) return;
        notifiedAt = now;
        NotificationManager notifications = getSystemService(NotificationManager.class);
        if (notifications != null) notifications.notify(NOTIFY_PROGRESS, progressNotification().build());
    }

    /** Status-bar glyph drawn at runtime (the APK has no drawable resources): the emblem's peaks under a floating dot. */
    private static Icon smallIcon() {
        int size = 96;
        Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xffffffff); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(9);
        paint.setStrokeJoin(Paint.Join.ROUND); paint.setStrokeCap(Paint.Cap.ROUND);
        android.graphics.Path path = new android.graphics.Path();
        path.moveTo(10, 82); path.lineTo(32, 46); path.lineTo(44, 60); path.lineTo(62, 30); path.lineTo(86, 82);
        canvas.drawPath(path, paint);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(62, 12, 7, paint);
        return Icon.createWithBitmap(bitmap);
    }
}

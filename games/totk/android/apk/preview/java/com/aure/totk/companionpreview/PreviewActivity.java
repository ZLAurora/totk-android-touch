package com.aure.totk.companionpreview;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemClock;
import com.aure.totk.companion.CompanionView;
import com.aure.totk.companion.DataSource;
import com.aure.totk.companion.PresentationController;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Developer-only entrypoint; no libtotk, game files or device access. Item actions are simulated on fixture data. */
public final class PreviewActivity extends Activity {
    private PresentationController presentations;
    private DataSource source;
    private int page;
    private Boolean standIn;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        PresentationController.immersive(getWindow(), false);
        mirrorCompanionAssets();
        String name = getIntent().getStringExtra("fixture");
        if (name == null || !name.matches("[a-z]+")) name = "ready";
        source = new FixtureSource(name, Math.max(0, getIntent().getIntExtra("delayMs", 900)));
        page = getIntent().getIntExtra("page", 0);
        // --ez here true: layout checks at an overridden main-panel size; no Presentation, so still exactly once.
        if (getIntent().getBooleanExtra("here", false)) { showCompanion(); return; }
        presentations = new PresentationController(this, source, page, this::placeCompanion);
        placeCompanion();
    }
    @Override public void onTopResumedActivityChanged(boolean top) {
        super.onTopResumedActivityChanged(top);
        if (presentations != null) presentations.ownerTopResumed(top);
    }
    /** The companion appears exactly once and never on this activity's display while a secondary display shows it:
     * this display then stands in for the game. Without a secondary display (a single-screen emulator) it is drawn
     * here so the fixtures stay inspectable. */
    private void placeCompanion() {
        if (presentations == null) return;
        boolean secondary = presentations.showing();
        if (standIn != null && standIn == secondary) return;
        standIn = secondary;
        if (secondary) {
            android.widget.TextView game = new android.widget.TextView(this);
            game.setGravity(android.view.Gravity.CENTER);
            game.setTextColor(0xffaaa997);
            game.setTextSize(18);
            game.setBackgroundColor(0xff000000);
            game.setText("Game screen stand-in\nThe Purah Pad companion is on the second display.");
            setContentView(game);
        } else showCompanion();
    }
    private void showCompanion() {
        CompanionView view = new CompanionView(this, source);
        view.setPage(page);
        setContentView(view);
    }

    /** Fixture JSON with a simulated action backend. An action's optional "simulate" field selects the outcome:
     * "done" (default, applied to the fixture after delayMs), "busy" (request returns -1), or "refused:<reason>" /
     * "failed:<reason>". Fixture "actionOverrides" ({"<id>|*": {fields}}) rewrite every matching item action. */
    private final class FixtureSource implements DataSource {
        private final String fixture;
        private final long delay;
        private JSONObject root;
        private String payload;
        private long loadedAt, loadingAfter;
        private int lastSeq, seq;
        private long dueAt;
        private String outcome, reason;
        private JSONObject target;
        private String targetAction, targetCategory;

        FixtureSource(String fixture, long delay) { this.fixture = fixture; this.delay = delay; }

        @Override public synchronized String read(int detail) throws Exception {
            if (root == null) {
                root = fixture(fixture);
                if (root.optBoolean("omitPouch")) root.remove("pouch");
                JSONObject overrides = root.optJSONObject("actionOverrides");
                root.remove("actionOverrides");
                if (overrides != null) overrideActions(overrides);
                loadingAfter = root.optLong("loadingAfterMs", 0);
                loadedAt = SystemClock.elapsedRealtime();
                payload = root.toString();
            }
            settle();
            if (loadingAfter > 0 && SystemClock.elapsedRealtime() - loadedAt >= loadingAfter)
                return "{\"schema\":1,\"seq\":2,\"status\":\"starting\"}";
            return payload;
        }
        @Override public boolean send(int kind, int arg) { return false; }

        @Override public synchronized int submit(String action, String category, int slot, String actor, int partition, long pouchGen) {
            if (root == null) return -2;
            if ("pending".equals(outcome)) return -1;
            JSONObject item = item(category, slot);
            JSONObject entry = item == null ? null : find(item.optJSONArray("actions"), action);
            if (entry == null && item != null && actor.equals(item.optString("name"))) return -4;
            if (entry != null && !entry.optBoolean("enabled")) return -2;
            String simulate = entry == null ? "" : entry.optString("simulate", "done");
            if (simulate.equals("busy")) return -1;
            seq = ++lastSeq;
            target = item; targetAction = action; targetCategory = category;
            if (item == null || !actor.equals(item.optString("name")) || partition != root.optInt("partition", -1)
                    || pouchGen != root.optLong("pouchGen", -1)) {
                outcome = "refused"; reason = "itemMoved";
            } else {
                outcome = "pending"; reason = simulate; dueAt = SystemClock.elapsedRealtime() + delay;
            }
            return seq;
        }
        @Override public synchronized String outcome(int value) {
            if (value != seq || seq == 0) return null;
            settle();
            JSONObject answer = new JSONObject();
            try { answer.put("seq", seq).put("state", outcome).put("reason", "pending".equals(outcome) || "done".equals(outcome) ? "" : reason); }
            catch (JSONException impossible) { return null; }
            return answer.toString();
        }

        private void settle() {
            if (!"pending".equals(outcome) || SystemClock.elapsedRealtime() < dueAt) return;
            String simulate = reason;
            int colon = simulate.indexOf(':');
            if (colon > 0) { outcome = simulate.substring(0, colon); reason = simulate.substring(colon + 1); return; }
            outcome = "done"; reason = "";
            try { apply(); } catch (JSONException impossible) { /* fixture shapes are fixed */ }
            payload = root.toString();
        }
        /** What the game would show afterwards, enough for the companion to see its snapshot change. */
        private void apply() throws JSONException {
            JSONArray list = root.getJSONObject("pouch").getJSONArray(targetCategory);
            String name = target.optString("name"), part = name.substring(name.lastIndexOf('_') + 1);
            if (targetAction.equals("equip") || targetAction.equals("unequip")) {
                for (int i = 0; i < list.length(); i++) {
                    JSONObject item = list.getJSONObject(i);
                    String other = item.optString("name");
                    boolean group = item == target || !targetCategory.equals("armor") || other.endsWith("_" + part);
                    if (!group) continue;
                    boolean worn = item == target && targetAction.equals("equip");
                    if (!worn && !item.optBoolean("equipped") && item != target) continue;
                    item.put("equipped", worn);
                    JSONArray actions = item.optJSONArray("actions");
                    JSONObject entry = actions == null ? null : find(actions, worn ? "equip" : "unequip");
                    if (entry != null) entry.put("id", worn ? "unequip" : "equip");
                    // Weapons, bows and shields: the game offers no unequip, so the worn item has no action.
                    if (!targetCategory.equals("armor") && actions != null) {
                        if (worn) item.put("actions", new JSONArray());
                        else if (find(actions, "equip") == null) actions.put(new JSONObject().put("id", "equip").put("enabled", true));
                    }
                }
            } else if (!targetAction.equals("fuseArrow") && target.has("count")) {
                int count = target.optInt("count") - 1;
                if (count > 0) target.put("count", count);
                else for (int i = 0; i < list.length(); i++) if (list.get(i) == target) { list.remove(i); break; }
            }
            root.put("pouchGen", root.optLong("pouchGen") + 1);
        }
        private JSONObject item(String category, int slot) {
            JSONArray list = root.optJSONObject("pouch") == null ? null : root.optJSONObject("pouch").optJSONArray(category);
            if (list == null) return null;
            for (int i = 0; i < list.length(); i++) {
                JSONObject item = list.optJSONObject(i);
                if (item != null && item.optInt("slot", -1) == slot) return item;
            }
            return null;
        }
        private JSONObject find(JSONArray actions, String id) {
            if (actions == null) return null;
            for (int i = 0; i < actions.length(); i++) {
                JSONObject entry = actions.optJSONObject(i);
                if (entry != null && id.equals(entry.optString("id"))) return entry;
            }
            return null;
        }
        private void overrideActions(JSONObject overrides) throws JSONException {
            JSONObject pouch = root.optJSONObject("pouch");
            if (pouch == null) return;
            for (java.util.Iterator<String> categories = pouch.keys(); categories.hasNext();) {
                JSONArray list = pouch.optJSONArray(categories.next());
                for (int i = 0; list != null && i < list.length(); i++) {
                    JSONArray actions = list.getJSONObject(i).optJSONArray("actions");
                    for (int k = 0; actions != null && k < actions.length(); k++) {
                        JSONObject entry = actions.getJSONObject(k);
                        JSONObject change = overrides.optJSONObject(entry.optString("id"));
                        if (change == null) change = overrides.optJSONObject("*");
                        if (change == null) continue;
                        for (java.util.Iterator<String> keys = change.keys(); keys.hasNext();) {
                            String key = keys.next(); entry.put(key, change.get(key));
                        }
                    }
                }
            }
        }
        private JSONObject fixture(String file) throws Exception {
            JSONObject value = load(file);
            if (!value.has("base")) return value;
            JSONObject base = fixture(value.getString("base"));
            for (java.util.Iterator<String> it = value.keys(); it.hasNext();) {
                String key = it.next(); if (!key.equals("base")) base.put(key, value.get(key));
            }
            return base;
        }
        private JSONObject load(String file) throws Exception {
            try (InputStream in = getAssets().open("fixtures/" + file + ".json")) {
                return new JSONObject(new String(in.readAllBytes(), StandardCharsets.UTF_8));
            }
        }
    }

    /** The app reads companion art from <game>/companion/; the preview ships it as APK assets (build_preview.py --assets)
     * and copies it there once per install, so both use the same loader. */
    private void mirrorCompanionAssets() {
        File game = getExternalFilesDir(null);
        if (game == null) return;
        File root = new File(game, "companion"), stamp = new File(game, "companion.stamp");
        try {
            String version = String.valueOf(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
            if (stamp.isFile() && new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.UTF_8).equals(version)) return;
            delete(root);
            String[] top = getAssets().list("companion");
            if (top != null && top.length > 0) copy("companion", root);
            Files.write(stamp.toPath(), version.getBytes(StandardCharsets.UTF_8));
        } catch (Exception failed) { android.util.Log.w("TotkPreview", "companion asset mirror failed", failed); }
    }
    private void copy(String asset, File out) throws java.io.IOException {
        String[] children = getAssets().list(asset);
        if (children != null && children.length > 0) {
            out.mkdirs();
            for (String child : children) copy(asset + "/" + child, new File(out, child));
            return;
        }
        try (InputStream in = getAssets().open(asset); OutputStream o = new FileOutputStream(out)) { in.transferTo(o); }
    }
    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }
    @Override protected void onDestroy() {
        if (presentations != null) presentations.close();
        super.onDestroy();
    }
}

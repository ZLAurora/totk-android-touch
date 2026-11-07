package com.aure.totk.setup;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

/** Host driver for tests/test_android_settings.py: one command per stdin line, one answer line per command.
 *    stored k=v...                 the values after loading this store (invalid entries fall back)
 *    storedsoc soc k=v...          stored values with chip-default migration
 *    stored101 soc k=v...          stored values plus typed 1.0.1 preference metadata
 *    set k=v...                    defaults, then Settings.set for each pair in order; prints the values
 *    step key dir k=v...           Settings.step(key, dir) on a store; prints the values
 *    preset k=v...                 Settings.preset()
 *    apply name k=v...             Settings.applyPreset(name); prints the values
 *    applysoc name soc k=v...      firstRun(soc), optional overrides, then apply a preset
 *    profile soc                   Settings.profile, profileKnown and socClass ('-' = null SOC_MODEL), space-separated
 *    first soc                     Settings.firstRun values
 *    latency k=v...                whether the effective latency is fixed by the selected timing mode
 *    env locale k=v...             Settings.env(locale) with newlines as '|' ('-' = no locale)
 *    envset locale k=v...          defaults, sequential Settings.set, then env text (for mode transitions)
 *    envsoc soc locale k=v...      Settings.env(locale) from a stored map with this chip
 *    envpreset name locale k=v...  apply a preset, then Settings.env(locale)
 *    envsocpreset name soc locale k=v...  firstRun(soc), apply a preset, optional overrides, then Settings.env(locale)
 *    envdir locale dir k=v...      Settings.env(locale, dir) ('-' = primary folder, no TOTK_DATA_DIR)
 *    write dir name text           Settings.writeAtomic(dir, name, text): "ok" or "error <exception message>" */
public final class SettingsProbe {
    private static Map<String, String> pairs(String[] words, int from) {
        Map<String, String> map = new HashMap<>();
        for (String word : Arrays.copyOfRange(words, from, words.length)) {
            int eq = word.indexOf('=');
            map.put(word.substring(0, eq), word.substring(eq + 1));
        }
        return map;
    }
    private static String dump(Settings settings) {
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, String> e : settings.values().entrySet()) out.append(out.length() == 0 ? "" : " ").append(e.getKey()).append('=').append(e.getValue());
        return out.toString();
    }
    public static void main(String[] args) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        for (String line; (line = in.readLine()) != null;) {
            String[] w = line.trim().split("\\s+");
            String answer;
            switch (w[0]) {
                case "stored": answer = dump(new Settings(pairs(w, 1), null)); break;
                case "storedsoc": answer = dump(new Settings(pairs(w, 2), w[1])); break;
                case "stored101": {
                    Map<String, Object> stored = new HashMap<>(pairs(w, 2));
                    stored.put("initialized", true); stored.put("skip_menu", true);
                    stored.put("update_checks", false); stored.put("update_checked_at", 1793808000000L);
                    answer = dump(new Settings(stored, w[1])); break;
                }
                case "set": {
                    Settings s = new Settings(null, null);
                    for (Map.Entry<String, String> e : pairsInOrder(w, 1)) s.set(e.getKey(), e.getValue());
                    answer = dump(s); break;
                }
                case "step": { Settings s = new Settings(pairs(w, 3), null); s.step(w[1], Integer.parseInt(w[2])); answer = dump(s); break; }
                case "preset": answer = new Settings(pairs(w, 1), null).preset(); break;
                case "apply": { Settings s = new Settings(pairs(w, 2), null); s.applyPreset(w[1]); answer = dump(s); break; }
                case "applysoc": {
                    Settings s = Settings.firstRun(w[2]);
                    for (Map.Entry<String, String> e : pairsInOrder(w, 3)) s.set(e.getKey(), e.getValue());
                    s.applyPreset(w[1]); answer = dump(s); break;
                }
                case "profile": {
                    String soc = w.length < 2 || w[1].equals("-") ? null : line.trim().substring(line.trim().indexOf(' ') + 1);
                    answer = Settings.profile(soc) + " " + Settings.profileKnown(soc) + " " + Settings.socClass(soc);
                    break;
                }
                case "first": answer = dump(Settings.firstRun(w.length > 1 ? w[1] : null)); break;
                case "latency": answer = String.valueOf(new Settings(pairs(w, 1), null).latencyLocked()); break;
                case "defaults": { Settings s = new Settings(pairs(w, 3), w[2]); s.defaults(w[1], w[2]); answer = dump(s); break; }
                case "env": answer = new Settings(pairs(w, 2), null).env(w[1].equals("-") ? null : w[1]).replace('\n', '|'); break;
                case "envset": {
                    Settings s = new Settings(null, null);
                    for (Map.Entry<String, String> e : pairsInOrder(w, 2)) s.set(e.getKey(), e.getValue());
                    answer = s.env(w[1].equals("-") ? null : w[1]).replace('\n', '|'); break;
                }
                case "envsoc": answer = new Settings(pairs(w, 3), w[1]).env(w[2].equals("-") ? null : w[2]).replace('\n', '|'); break;
                case "envpreset": {
                    Settings s = new Settings(pairs(w, 3), null);
                    s.applyPreset(w[1]);
                    answer = s.env(w[2].equals("-") ? null : w[2]).replace('\n', '|'); break;
                }
                case "envsocpreset": {
                    Settings s = Settings.firstRun(w[2]);
                    s.applyPreset(w[1]);
                    for (Map.Entry<String, String> e : pairsInOrder(w, 4)) s.set(e.getKey(), e.getValue());
                    answer = s.env(w[3].equals("-") ? null : w[3]).replace('\n', '|'); break;
                }
                case "envdir": answer = new Settings(pairs(w, 3), null).env(w[1].equals("-") ? null : w[1], w[2].equals("-") ? null : w[2]).replace('\n', '|'); break;
                case "write":
                    try { Settings.writeAtomic(new java.io.File(w[1]), w[2], w[3]); answer = "ok"; }
                    catch (java.io.IOException failed) { answer = "error " + failed; }
                    break;
                default: answer = "unknown command " + w[0];
            }
            System.out.println(answer);
        }
    }
    private static java.util.List<Map.Entry<String, String>> pairsInOrder(String[] words, int from) {
        java.util.List<Map.Entry<String, String>> list = new java.util.ArrayList<>();
        for (String word : Arrays.copyOfRange(words, from, words.length)) {
            int eq = word.indexOf('=');
            list.add(Map.entry(word.substring(0, eq), word.substring(eq + 1)));
        }
        return list;
    }
}

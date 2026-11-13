"""The benchmark tools (tools/bench): the immutable baselines match their pins, the baseline write guard, frequency
limits and signatures, automatic per-signature baselines, the comparison verdicts and signature checks, and the ledger
stays consistent with its stored results; no device needed."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import signal
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / 'tools/bench'
sys.path[:0] = [str(BENCH)]
import analyze  # noqa: E402
import abtest  # noqa: E402
import benchlib  # noqa: E402
import check_baselines  # noqa: E402
import ledger  # noqa: E402
import sampler  # noqa: E402
import provenance  # noqa: E402
import warmup  # noqa: E402

BASELINE = benchlib.BENCH_DOCS / 'baseline-1.0.0.json'
# A Thor snapshot as `bench_input.sh freqs` prints it (no cap below the hardware maximum).
THOR_FREQS = ('cpu0=0,1,2:902400:2016000:2016000 cpu3=3,4,5,6:1651200:2803200:2803200 cpu7=7:1843200:3187200:3187200 '
              'gpu_maxclk=680000000 gpu_freqs=680000000,615000000,550000000,475000000,401000000,348000000,295000000,'
              '220000000,124800000 gpu_devfreq_max=680000000 gpu_min_pwrlevel=4 gpu_max_pwrlevel=0 gpu_thermal_pwrlevel=0')


def run(*args):
    return subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True, cwd=ROOT)


def snapshot(text=THOR_FREQS):
    return benchlib.parse_freq_tokens(text.split())


def thor_result(fl=None, stable=True):
    """The 1.0.0 Thor result restricted to Thor, with recorded frequency limits (a new-style result)."""
    doc = ledger.device_doc(benchlib.load_result(BASELINE), 'thor')
    doc['devices']['thor']['freq_limits'] = fl or snapshot()
    for r in doc['runs']:
        r['freq_stable'] = stable
    return doc


class Baselines(unittest.TestCase):
    def test_baselines_match_their_pins(self):
        self.assertEqual(check_baselines.problems(), [])

    def test_analyze_and_compare_refuse_to_write_a_baseline(self):
        for target in (BASELINE, benchlib.BENCH_DOCS / 'baselines/test/x.json'):
            r = run(BENCH / 'analyze.py', ROOT / 'docs', '--json', target)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('immutable', r.stderr)
        r = run(BENCH / 'compare.py', BASELINE, BASELINE, '--out', ROOT / 'docs/benchmarks/baseline-test.md')
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((ROOT / 'docs/benchmarks/baseline-test.md').exists())

    def test_index_registers_the_1_0_0_runs_without_a_gpu_cap(self):
        idx = {b['device']: b for b in benchlib.load_index()['baselines'] if b['file'] == 'baseline-1.0.0.json'}
        self.assertEqual(set(idx), {'thor', 'odin', 'duo'})
        self.assertTrue(all(b['gpu_cap_hz'] is None for b in idx.values()))
        self.assertEqual(idx['odin']['cpu_caps_khz'], [2227200, 2246400])
        self.assertEqual(idx['odin']['cpu_hw_max_khz'], [3532800, 4320000])
        self.assertEqual(idx['thor']['cpu_caps_khz'], idx['thor']['cpu_hw_max_khz'])


class Signatures(unittest.TestCase):
    def test_parse_snapshot(self):
        fl = snapshot()
        self.assertEqual([c['related_cpus'] for c in fl['cpu']], [[0, 1, 2], [3, 4, 5, 6], [7]])
        self.assertEqual(fl['cpu'][0]['scaling_min_khz'], 902400)
        self.assertEqual(benchlib.caps_of_limits(fl), ((2016000, 2803200, 3187200), 680000000))
        self.assertEqual(fl['gpu']['hw_max_hz'], 680000000)
        # kgsl max_pwrlevel 2 caps the GPU at the third table entry, below max_gpuclk
        capped = snapshot(THOR_FREQS.replace('gpu_max_pwrlevel=0', 'gpu_max_pwrlevel=2'))
        self.assertEqual(capped['gpu']['cap_hz'], 550000000)

    def test_signature_fields(self):
        env = {'TOTK_FPS60': '0', 'TOTK_DATA_DIR': '/x'}
        base, fields = benchlib.signature_of('AYN Thor', 'QTI QCS8550', env, [2016000, 2803200], 680000000)
        self.assertEqual(base, benchlib.signature_of('AYN Thor', 'QTI QCS8550', dict(env, TOTK_DATA_DIR='/y'),
                                                     [2016000, 2803200], 680000000)[0])   # the data dir is ignored
        self.assertEqual(base, benchlib.signature_of_fields(fields))
        for changed in (('AYN Odin 3', 'QTI QCS8550', env, [2016000, 2803200], 680000000),
                        ('AYN Thor', 'QTI QCS8550', dict(env, TOTK_FPS60='1'), [2016000, 2803200], 680000000),
                        ('AYN Thor', 'QTI QCS8550', env, [1804800, 2803200], 680000000),
                        ('AYN Thor', 'QTI QCS8550', env, [2016000, 2803200], 615000000),
                        ('AYN Thor', 'QTI QCS8550', env, [2016000, 2803200], None)):
            self.assertNotEqual(base, benchlib.signature_of(*changed)[0], changed)

    def test_caps_text_marks_hardware_maximum(self):
        self.assertEqual(benchlib.caps_text([2016000, 2803200], [2016000, 2803200], 680000000, 680000000),
                         'CPU 2.02/2.80 GHz (max), GPU 680 MHz (max)')
        self.assertEqual(benchlib.caps_text([2227200, 2246400], [3532800, 4320000], None, None),
                         'CPU 2.23/2.25 GHz (hw 3.53/4.32), GPU cap unknown')


class Compare(unittest.TestCase):
    def setUp(self):
        self.doc = benchlib.load_result(BASELINE)

    def test_baseline_against_itself_is_all_same(self):
        r = run(BENCH / 'compare.py', BASELINE, BASELINE)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('frequency signatures match', r.stdout)
        self.assertNotIn('better', r.stdout.replace('0 better', ''))
        self.assertNotIn('worse', r.stdout.replace('0 worse', ''))
        self.assertNotIn('WARNING', r.stdout)

    def test_thresholds(self):
        base = {'windows': 4, 'fps_mean': 30.0, 'p50_ms_median': 33.3, 'p95_ms_max': 38.0, 'gpu_ms_mean': 20.0,
                'over50_total': 4}
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_mean=30.5))[1], 'same')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_mean=28.0))[1], 'worse')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, gpu_ms_mean=18.0))[1], 'better')
        # >50 ms frames compare per window: 8 over 8 windows is the same rate as 4 over 4
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, windows=8, over50_total=8))[1], 'same')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, over50_total=12))[1], 'worse')

    def test_settings_and_cpu_cap_differences_are_flagged(self):
        new = json.loads(json.dumps(self.doc))
        label = next(iter(new['devices']))
        new['devices'][label]['settings_env']['TOTK_FPS60'] = 'changed'
        for r in new['runs']:
            if r['device'] == label:
                for t in r['thermal']:
                    t['cpufreq_caps_khz'] = [{'scaling_max': 1, 'cpuinfo_max': 2}]
                r['freq_stable'] = False
        warns = benchlib.comparability(self.doc, label, new, label)
        self.assertTrue(any('settings_env' in w for w in warns))
        self.assertTrue(any('cpufreq caps' in w for w in warns))
        self.assertTrue(any('freq_stable false' in w for w in warns))

    def compare(self, new, base=None, base_path=BASELINE):
        buf = io.StringIO()
        verdicts = benchlib.compare_docs(base or self.doc, new, buf, base_path=base_path)
        return {rel for _, rel in verdicts.values()}, buf.getvalue()

    def test_reference_only_against_1_0_0_when_cpu_caps_match(self):
        rels, text = self.compare(thor_result())
        self.assertEqual(rels, {'reference-only'})
        self.assertIn('REFERENCE ONLY', text)

    def test_signature_mismatch_is_not_comparable(self):
        capped = snapshot(THOR_FREQS.replace(':2016000:2016000', ':1804800:2016000'))
        rels, text = self.compare(thor_result(capped))
        self.assertEqual(rels, {'mismatch'})
        self.assertIn('NOT COMPARABLE', text)
        # the same mismatch between two new-style results (different GPU caps)
        gpu = snapshot(THOR_FREQS.replace('gpu_maxclk=680000000', 'gpu_maxclk=615000000'))
        rels, text = self.compare(thor_result(gpu), base=thor_result(), base_path=None)
        self.assertEqual(rels, {'mismatch'})
        self.assertIn('gpu_cap_hz: 680000000 -> 615000000', text)
        rels, _ = self.compare(thor_result(), base=thor_result(), base_path=None)
        self.assertEqual(rels, {'match'})


class AutoBaseline(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.docs = Path(self.tmp.name)
        (self.docs / 'BASELINES.sha256').write_text('')

    def tearDown(self):
        self.tmp.cleanup()

    def test_new_signature_becomes_its_baseline_once(self):
        doc = thor_result()
        sig, stable, base, became = ledger.resolve_baseline(doc, 'thor', 'run-a', 'c' * 40, self.docs)
        self.assertTrue(stable and became)
        self.assertEqual(base['signature'], sig['sha256'])
        path = self.docs / base['file']
        self.assertEqual(base['file'], f'baselines/ayn-thor/{sig["short"]}.json')
        self.assertTrue(path.exists())
        self.assertIn(base['file'], benchlib.read_pins(self.docs))
        self.assertEqual(check_baselines.problems(self.docs), [])
        self.assertEqual(json.loads(path.read_text())['baseline']['run_id'], 'run-a')
        before = path.read_bytes()
        # a later run with the same signature is compared against it, never replaces it
        _, _, base2, became2 = ledger.resolve_baseline(thor_result(), 'thor', 'run-b', 'd' * 40, self.docs)
        self.assertFalse(became2)
        self.assertEqual(base2['file'], base['file'])
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(len(benchlib.load_index(self.docs)['baselines']), 1)
        # another GPU cap is a new signature with its own baseline
        gpu = snapshot(THOR_FREQS.replace('gpu_maxclk=680000000', 'gpu_maxclk=615000000'))
        _, _, base3, became3 = ledger.resolve_baseline(thor_result(gpu), 'thor', 'run-c', 'e' * 40, self.docs)
        self.assertTrue(became3)
        self.assertNotEqual(base3['file'], base['file'])
        self.assertEqual(check_baselines.problems(self.docs), [])

    def test_refuses_to_overwrite_or_replace(self):
        doc = thor_result()
        _, _, base, _ = ledger.resolve_baseline(doc, 'thor', 'run-a', 'c' * 40, self.docs)
        with self.assertRaises(SystemExit):
            benchlib.write_baseline(doc, 'thor', 'run-b', 'd' * 40, self.docs)   # signature already has a baseline
        idx = benchlib.load_index(self.docs)
        idx['baselines'] = []
        benchlib.save_index(idx, self.docs)
        with self.assertRaises(SystemExit):
            benchlib.write_baseline(doc, 'thor', 'run-b', 'd' * 40, self.docs)   # the file exists / is pinned
        with self.assertRaises(SystemExit):
            benchlib.pin(base['file'], self.docs)
        self.assertTrue(benchlib.is_baseline_path(self.docs / base['file'], self.docs))
        (self.docs / base['file']).write_text('{}')
        self.assertTrue(any('CHANGED' in p for p in check_baselines.problems(self.docs)))

    def test_unstable_or_unrecorded_runs_never_become_baselines(self):
        _, stable, base, became = ledger.resolve_baseline(thor_result(stable=False), 'thor', 'r', 'c' * 40, self.docs)
        self.assertEqual((stable, base, became), (False, None, False))
        doc = thor_result()
        del doc['devices']['thor']['freq_limits']
        sig, _, base, became = ledger.resolve_baseline(doc, 'thor', 'r', 'c' * 40, self.docs)
        self.assertEqual((sig, base, became), (None, None, False))
        self.assertFalse((self.docs / 'baselines').exists())


class FreqStability(unittest.TestCase):
    def rows(self, *gcaps, caps=((2016000, 2016000), (2803200, 2803200), (3187200, 3187200))):
        return [{'caps': list(caps), 'gcap': g} for g in gcaps]

    def test_stable_run(self):
        s = snapshot()
        self.assertEqual(analyze.freq_stability([s, s], self.rows(680000000, 680000000)), (True, []))
        self.assertEqual(analyze.freq_stability([], []), (None, []))

    def test_cap_change_during_a_run(self):
        s = snapshot()
        lowered = snapshot(THOR_FREQS.replace(':3187200:3187200', ':2496000:3187200'))
        stable, changes = analyze.freq_stability([s, lowered], self.rows(680000000))
        self.assertFalse(stable)
        self.assertTrue(any('start / end' in c for c in changes))
        stable, changes = analyze.freq_stability([s, s], self.rows(680000000, 550000000))
        self.assertFalse(stable)
        self.assertTrue(any('max_gpuclk' in c for c in changes))
        rows = self.rows(680000000, caps=((2016000, 2016000), (2803200, 2803200), (2496000, 3187200)))
        self.assertFalse(analyze.freq_stability([s, s], self.rows(680000000) + rows)[0])
        # caps different from the device's first scene
        self.assertFalse(analyze.freq_stability([lowered, lowered], [], benchlib.caps_of_limits(s))[0])

    def test_parse_freqs_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'freqs.txt'
            p.write_text(f'thor rotsumamu T0=100 start {THOR_FREQS}\nthor rotsumamu T0=100 end \n')
            got = analyze.parse_freqs(p)
        self.assertEqual(list(got), [('thor', 100)])
        self.assertEqual(set(got[('thor', 100)]), {'start'})


class Ledger(unittest.TestCase):
    def test_entries_point_at_existing_results_of_clean_commits(self):
        path = benchlib.BENCH_DOCS / 'ledger.jsonl'
        seen = set()
        for line in path.read_text(encoding='utf-8').splitlines():
            e = json.loads(line)
            key = (e['run_id'], e['device'])
            self.assertNotIn(key, seen)
            seen.add(key)
            self.assertFalse(e['dirty'])
            self.assertRegex(e['commit'], r'^[0-9a-f]{40}$')
            result = benchlib.load_result(ROOT / e['result'])
            self.assertEqual(list(result['devices']), [e['device']])
            self.assertEqual({r['scene']: r['summary'] for r in result['runs']}, e['summary'])
            if e.get('freq_signature'):
                sig = benchlib.device_signature(result, e['device'])
                self.assertEqual(sig['sha256'], e['freq_signature'])
                if e.get('became_baseline'):
                    b = benchlib.index_entry(e['freq_signature'])
                    self.assertEqual(ROOT / e['baseline'], benchlib.BENCH_DOCS / b['file'])


def ab_log(windows, ctl):
    """A synthetic totk.log: per 300-frame window end f, its timing / fps / GPU / stutter lines (values from
    windows[f] = (fps, p95, gpu, over50)), with the [totk-ctl] lines (frame, text) placed where they happened."""
    lines, events = [], sorted(ctl)
    for f in range(300, max(windows) + 1, 300):
        while events and events[0][0] < f:
            lines.append(events.pop(0)[1])
        fps, p95, gpu, over50 = windows.get(f, (30.0, 40.0, 20.0, 0))
        lines += [f'[nvn] 计时 至帧 {f}： 帧时间 p50 33.30 / p95 {p95:.2f} / 最大 60.00 ms（300 帧）；',
                  f'[nvn-vk] frame {f}: {fps:.1f} fps over the last 300, 1000 draws',
                  f'[nvn-vk] frame {f}, per frame: recording 2.0 ms, waiting for the GPU 1.0 ms, GPU {gpu:.1f} ms',
                  f'[nvn-vk] frame {f}, stutter: longest frame 60.0 ms, 3 frames over 33 ms, {over50} over 50 ms']
    return '\n'.join(lines + [e[1] for e in events]) + '\n'


class UncappedReporting(unittest.TestCase):
    @staticmethod
    def make_run(d, spikes=False, setting='1', override=None, force=False, speedup=False, cap=None):
        (d / 'logs').mkdir(parents=True)
        lines = [f'[totk-ctl] gen={i + 1} frame={i * 900 + 1} mark ab arm={arm} block={i + 1}'
                 for i, arm in enumerate('ABBA')]
        for frame in range(300, 3601, 300):
            arm_b = (frame - 1) // 900 in (1, 2)
            spike = spikes and arm_b
            fps = 44 if speedup and arm_b else 40
            p95, p99, worst, over33, over50 = (55, 90, 120, 20, 4) if spike else (27, 35, 40, 3, 0)
            lines += [f'[nvn] 计时 至帧 {frame}： 帧时间 p50 25.00 / p95 {p95} / 最大 {worst} ms（300 帧）；',
                      f'[nvn] 帧间隔 至帧 {frame}：>16.7ms 300 / 300 帧，p99 {p99} ms',
                      f'[nvn-vk] frame {frame}: {fps}.0 fps over the last 300, {frame * 5000} draws',
                      f'[nvn-vk] frame {frame}, per frame: recording 2.0 ms, waiting for the GPU 1.0 ms, GPU 10.0 ms',
                      f'[nvn-vk] frame {frame}, stutter: longest frame {worst} ms, {over33} frames over 33 ms, {over50} over 50 ms']
        (d / 'logs/thor.log').write_text('\n'.join(lines) + '\n')
        (d / 'settings-thor.env').write_text(f'TOTK_FPS60=1\nTOTK_UNCAPPED={setting}\n')
        if cap is not None:
            with (d / 'settings-thor.env').open('a') as stream:
                stream.write(f'TOTK_UNCAPPED_HZ={cap}\n')
        if override is not None:
            (d / 'totk-env-thor.txt').write_text(f'TOTK_UNCAPPED={override}\n')
        (d / 'pans.txt').write_text(''.join(f'thor hateno F0={i * 900} F1={(i + 1) * 900} '
                                          f'T0={100 + i * 100} T1={130 + i * 100} V=16000\n' for i in range(4)))
        result = run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'result.json', *(['--uncapped'] if force else []))
        if result.returncode:
            raise AssertionError(result.stderr)
        return json.loads((d / 'result.json').read_text()), result.stdout

    def test_effective_env_and_explicit_flag_skip_dips_at_40_fps(self):
        for setting, override, force, expected in (('1', None, False, True), ('0', '1', False, True),
                                                  ('1', '0', False, False), ('0', '0', True, True)):
            with self.subTest(setting=setting, override=override, force=force), tempfile.TemporaryDirectory() as tmp:
                doc, text = self.make_run(Path(tmp), setting=setting, override=override, force=force)
                record = doc['runs'][0]
                summary = record['summary']
                self.assertEqual(doc['devices']['thor']['uncapped'], expected)
                self.assertEqual(summary['throughput_fps'], 40)
                self.assertIs(summary['dip_metrics_valid'], not expected)
                if expected:
                    self.assertIsNone(summary['dip_windows'])
                    self.assertTrue(all(w['pacing_state'] == 'uncapped' for w in record['windows']))
                    self.assertIn('uncapped throughput 40.000 fps', text)
                else:
                    self.assertEqual(summary['dip_windows'], 8)
                self.assertEqual((summary['p50_ms_max'], summary['p95_ms_max'], summary['p99_ms_max']), (25, 27, 35))
                self.assertEqual(summary['p99_p50_ratio_max'], 1.4)
                self.assertEqual(summary['fps_stddev'], 0)
                self.assertEqual(summary['over33_total'], 24)
                self.assertEqual(record['windows'][0]['p99_samples'], 300)
        s = analyze.summarize([{'fps': 30, 'uncapped': True}, {'fps': 50, 'uncapped': True}])
        self.assertEqual(s['throughput_fps'], 37.5)  # frames/time, not arithmetic mean 40
        self.assertEqual(s['fps_stddev'], 10)
        self.assertIsNone(s['p99_ms_max'])
        self.assertEqual(s['p99_windows'], 0)

    def test_forty_target_uses_effective_cap_and_cli_override(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, override, target in (('forty', None, 40), ('disabled', '0', 60)):
                doc, _ = self.make_run(root / name, cap='40', override=override)
                self.assertEqual(doc['runs'][0]['pans'][0]['target_fps'], target)
                self.assertTrue(all(w['target_fps'] == target for w in doc['runs'][0]['windows']))
            result = run(BENCH / 'analyze.py', root / 'disabled', '--target-fps', '40',
                         '--json', root / 'explicit.json')
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = json.loads((root / 'explicit.json').read_text())
            self.assertEqual(doc['runs'][0]['pans'][0]['target_fps'], 40)

    def test_uncapped_comparisons_use_throughput_and_tails_never_dip_confounding(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            quiet, _ = self.make_run(root / 'quiet')
            spiky, _ = self.make_run(root / 'spiky', spikes=True)
            self.make_run(root / 'faster', speedup=True)
            for label, expected in (('quiet', 'SAME'), ('spiky', 'WORSE'), ('faster', 'BETTER')):
                result = run(BENCH / 'abtest.py', root / label / 'result.json')
                self.assertEqual(result.returncode, expected == 'WORSE', result.stdout + result.stderr)
                self.assertIn(f'arm B vs A: {expected}', result.stdout)
                self.assertNotIn('verdict confounded by dips', result.stdout)
                self.assertRegex(result.stdout, rf'throughput fps .* {"better" if label == "faster" else "same"} \*')
            qs, ss = quiet['runs'][0]['summary'], spiky['runs'][0]['summary']
            self.assertEqual(benchlib.scene_verdicts(qs, ss)[1], 'worse')
            self.assertEqual(benchlib.scene_verdicts(qs, dict(qs, throughput_fps=44))[1], 'better')
            self.assertEqual(benchlib.scene_verdicts(dict(qs, dip_time_fraction=0),
                                                   dict(qs, dip_time_fraction=1))[1], 'same')
            capped = dict(qs, uncapped=False, speed_mode='capped')
            self.assertEqual(benchlib.scene_verdicts(qs, capped)[1], 'inconclusive')
            result = run(BENCH / 'compare.py', root / 'quiet/result.json', root / 'spiky/result.json')
            self.assertEqual(result.returncode, 0, result.stderr)  # no frequency signature in this fixture
            self.assertIn('WORSE', result.stdout)
            self.assertIn('time-weighted throughput and frame-time tails are primary', result.stdout)
            self.assertNotIn('verdict confounded by dips', result.stdout)


class PresentationReporting(unittest.TestCase):
    def test_per_pan_and_arm_recording_quantiles_and_weighted_bin_shares(self):
        ctl = [(1, '[totk-ctl] gen=1 frame=1 mark ab arm=A block=1'),
               (601, '[totk-ctl] gen=2 frame=601 mark ab arm=B block=2')]
        text = ab_log({600: (57, 20, 10, 0), 1200: (56, 21, 10, 0)}, ctl)
        for frame, p95, p99, bins in ((600, 25, 34, '0/240/30/20/5/5'), (1200, 35, 45, '0/120/15/10/3/2')):
            needle = next(line for line in text.splitlines() if line.startswith(f'[nvn-vk] frame {frame}:'))
            text = text.replace(needle, needle + '\n'
                                f'[nvn-vk] window present-fence cadence, last 300 results: '
                                f'interval min/mean/max=16/20/50ms p50/p95/p99=17/{p95}/{p99}ms samples=300/300\n'
                                '[nvn-vk] window present-fence interval bins ms '
                                '[<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]: '
                                f'all={bins}; adjacent immutable interval2=0/0/0/0/0/0')
        text = text.replace('frame 1200, per frame: recording 2.0', 'frame 1200, per frame: recording 4.0')
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'logs').mkdir()
            (d / 'logs/odin.log').write_text(text)
            (d / 'pans.txt').write_text('odin hateno F0=0 F1=600 T0=100 T1=120 V=16000\n'
                                      'odin hateno2 F0=600 F1=1200 T0=200 T1=220 V=16000\n')
            result = run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'result.json')
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads((d / 'result.json').read_text())['runs'][0]
        s = record['summary']
        self.assertEqual(s['recording_ms_mean'], 3)
        self.assertEqual((s['present_p95_ms_max'], s['present_p99_ms_max']), (35, 45))
        self.assertEqual(s['present_interval_bin_counts'], [0, 360, 45, 30, 8, 7])
        self.assertEqual(s['present_interval_pairs'], 450)  # weight by observed pairs, not one vote/window
        self.assertEqual(s['present_judder_proxy_fraction'], 0.1)
        self.assertAlmostEqual(sum(s['present_interval_bin_shares']), 1)
        self.assertEqual(record['pans'][0]['summary']['present_p99_ms_max'], 34)
        self.assertEqual(record['pans'][1]['summary']['recording_ms_mean'], 4)
        self.assertEqual(record['ab']['arms']['A']['summary']['present_p95_ms_max'], 25)
        self.assertEqual(record['ab']['arms']['B']['summary']['recording_ms_mean'], 4)
        self.assertIsNone(s['present_judder_exact_fraction'])
        self.assertIn('20.9-29 ms 10.00%', result.stdout)
        self.assertIn('fence p95/p99 window maxima 35.000/45.000', result.stdout)

    def test_exact_trace_joins_ids_not_callback_order_and_keeps_strict_boundaries(self):
        rows = [(1, 1, 300, 1_000_000_000), (2, 1, 301, 1_016_700_000),
                (3, 1, 302, 1_036_700_000), (4, 1, 303, 1_066_700_000),
                (5, 1, 304, 0), (6, 1, 305, 1_100_000_000), (8, 1, 306, 1_120_000_000),
                (9, 2, 307, 1_140_000_000), (10, 2, 308, 1_169_999_999),
                (11, 2, 309, 1_160_000_000), (12, 2, 310, 1_200_000_000),
                (12, 2, 310, 1_210_000_000), (13, 2, 311, 1_230_000_000)]
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'totk.log'
            # Deliberately reverse callback/log delivery. A conflicting duplicate invalidates ID12.
            p.write_text(''.join(f'[nvn-vk] [present-trace] id={i} generation={g} source_frame={f} '
                                 f'kind=real desired_ns=0 apply_ns=0 display_ns={ns} refresh_ns=16666667 interval=1 reason=0\n'
                                 for i, g, f, ns in reversed(rows)))
            info = analyze.parse_log(p)[5]
        window = analyze.presentation_of(301, 600, info)
        self.assertEqual(window['present_intervals_ns'], [16_700_000, 20_000_000, 30_000_000, 29_999_999])
        s = analyze.presentation_summary([window])
        self.assertEqual((s['present_judder_exact_hits'], s['present_judder_exact_pairs']), (2, 4))
        self.assertEqual(s['present_judder_exact_fraction'], 0.5)
        self.assertIsNone(s['present_judder_proxy_fraction'])  # cannot reverse-infer a histogram from sparse traces
        # Never attribute a pair across a pan/window's incoming source-frame boundary.
        self.assertEqual(analyze.presentation_of(308, 600, info)['present_intervals_ns'], [29_999_999])


class Pacing(unittest.TestCase):
    def test_bimodal_arms_do_not_turn_mean_changes_into_speed_verdicts(self):
        base = {'windows': 12, 'fps_mean': 50, 'fps_steady': 57, 'dip_windows': 3,
                'dip_time_fraction': 0.3, 'frames_near33': 200}
        per, verdict = benchlib.scene_verdicts(base, dict(base, fps_mean=55))
        self.assertEqual(verdict, 'same')
        self.assertEqual(per['fps_mean'][1], 'better')  # informational only
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_steady=59))[1], 'better')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_steady=55))[1], 'worse')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, dip_time_fraction=0.35))[1], 'same')
        self.assertEqual(benchlib.scene_verdicts(base, dict(base, fps_mean=58, dip_time_fraction=0.1))[1],
                         'verdict confounded by dips')
        self.assertEqual(benchlib.scene_verdicts(dict(base, fps_steady=None), base)[1], 'inconclusive')
        self.assertEqual(benchlib.scene_verdicts({'fps_mean': 57}, base)[1], 'inconclusive')

    def test_paired_bimodal_arms_and_cli_report_confounding(self):
        ref, arm = InSessionAB.records([[1, 57, 57, 17, 34, 40, 10, 0, 0]] * 2,
                                      [[1, 57, 57, 17, 34, 40, 10, 0, 0]] * 2)
        for record in (ref, arm):
            for s in [record['summary']] + [b['summary'] for b in record['blocks']]:
                s.update(fps_steady=57, dip_time_fraction=0.1, dip_windows=0, frames_near33=0)
        arm['summary'].update(fps_mean=40, dip_time_fraction=0.4)
        per = {key: abtest.metric_verdict(key, ref, arm) for key in analyze.AB_METRICS}
        self.assertEqual(per['dip_time_fraction'][3], 'same')  # pairs alone miss the pooled mismatch
        self.assertEqual(abtest.arm_verdict(per, ref, arm), 'verdict confounded by dips')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'ab.json'
            path.write_text(json.dumps({'runs': [{'device': 'odin', 'scene': 'hateno', 'ab': {
                'windows': {'assigned': 4, 'straddling': 0, 'unlabelled': 0}, 'arms': {'A': ref, 'B': arm}}}]}))
            result = run(BENCH / 'abtest.py', path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('arm B vs A: verdict confounded by dips', result.stdout)
        self.assertIn('fps steady', result.stdout)
        self.assertIn('dip time fraction', result.stdout)
        arm['summary']['dip_time_fraction'] = 0.1
        for b in arm['blocks']:
            b['summary']['dip_time_fraction'] = 0.4
        per = {key: abtest.metric_verdict(key, ref, arm) for key in analyze.AB_METRICS}
        self.assertEqual(abtest.arm_verdict(per, ref, arm), 'verdict confounded by dips')

    def test_cumulative_draw_counter_differences_and_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'totk.log'
            p.write_text(''.join(f'[nvn-vk] frame {f}: 57.0 fps over the last 300, {d} draws\n'
                                 for f, d in ((300, 1200000), (600, 2700000), (900, 100),
                                              (1200, 1500100), (1800, 4500100), (2100, 6000100))))
            rates = analyze.parse_log(p)[5]['draw_rates']
        self.assertEqual(rates, {600: 5000, 1200: 5000, 2100: 5000})
        self.assertNotIn(300, rates)  # no assumed zero origin
        self.assertNotIn(900, rates)  # reset
        self.assertNotIn(1800, rates)  # missing intermediate 300-frame sample

    def test_steady_and_dip_report_separate_time_weighted_states(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'logs').mkdir()
            (d / 'logs/odin.log').write_text(ab_log(
                {600: (57, 20, 10, 0), 900: (34, 34, 10, 0)}, []), encoding='utf-8')
            (d / 'settings-odin.env').write_text('TOTK_FPS60=1\n')
            (d / 'pans.txt').write_text('odin hateno F0=0 F1=900 T0=100 T1=120 V=16000\n')
            result = run(BENCH / 'analyze.py', d, '--json', d / 'result.json')
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads((d / 'result.json').read_text())['runs'][0]
        s = record['summary']
        self.assertEqual(s['fps_steady'], 57)
        self.assertEqual(s['fps_mean'], 45.5)
        self.assertEqual(s['dip_windows'], 1)
        self.assertAlmostEqual(s['dip_time_fraction'], (300 / 34) / (300 / 57 + 300 / 34), places=6)
        self.assertIsNone(s['frames_near33'])  # fps cannot recover individual frame counts
        self.assertEqual([w['pacing_source'] for w in record['windows']], ['window-fps'] * 2)

    def test_raw_durations_and_fence_histogram_precede_window_fps(self):
        raw = {f: 33.33 if f <= 75 else 16.67 for f in range(1, 301)}
        w = analyze.pacing_of(300, 51, 60, {'frame_ms': raw})
        self.assertEqual((w['pacing_state'], w['frames_near33']), ('dip', 75))
        self.assertEqual(w['pacing_source'], 'frame-durations')
        self.assertEqual(analyze.pacing_of(300, 30, 30, {'frame_ms': raw})['pacing_state'], 'steady')
        # Incomplete traces are not evidence that the unlogged frames were steady.
        w = analyze.pacing_of(300, 34, 60, {'frame_ms': {1: 33.3}})
        self.assertEqual((w['pacing_state'], w['pacing_source']), ('dip', 'window-fps'))
        self.assertEqual(w['frames_near33'], 1)
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'totk.log'
            p.write_text('[nvn-vk] frame 300: 51.0 fps over the last 300, 1000 draws\n'
                         '[nvn-vk] window present-fence interval bins ms '
                         '[<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]: '
                         'all=0/225/0/75/0/0; adjacent immutable interval2=0/0/0/70/0/0\n')
            info = analyze.parse_log(p)[5]
        w = analyze.pacing_of(300, 51, 60, info)
        self.assertEqual((w['pacing_state'], w['frames_near33_source']), ('dip', 'present-fence-histogram'))
        self.assertEqual(w['frames_near33'], 75)


class InSessionAB(unittest.TestCase):
    CTL = [(1000, '[totk-ctl] gen=1 frame=1000 applied unset TOTK_VK_DESC_STOCK'),
           (1000, '[totk-ctl] gen=1 frame=1000 mark ab arm=A block=1 scene=hateno'),
           (2500, '[totk-ctl] gen=2 frame=2500 applied TOTK_VK_DESC_STOCK=1'),
           (2500, '[totk-ctl] gen=2 frame=2500 mark ab arm=B block=2 scene=hateno'),
           (4000, '[totk-ctl] gen=3 frame=4000 mark ab arm=B block=3 scene=hateno'),
           (5200, '[totk-ctl] gen=4 frame=5200 applied unset TOTK_VK_DESC_STOCK'),
           (5200, '[totk-ctl] gen=4 frame=5200 mark ab arm=A block=4 scene=hateno'),
           (6500, '[totk-ctl] gen=5 frame=6500 mark ab done scene=hateno')]

    def test_parse_ctl(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.log'
            p.write_text('\n'.join(t for _, t in self.CTL) + '\n[totk-ctl] refused "x": unknown command\n', encoding='utf-8')
            gens = analyze.parse_ctl(p)
        self.assertEqual([(g['gen'], g['frame'], g['arm'], g['block']) for g in gens],
                         [(1, 1000, 'A', 1), (2, 2500, 'B', 2), (3, 4000, 'B', 3), (4, 5200, 'A', 4), (5, 6500, None, None)])
        self.assertEqual(gens[1]['applied'], ['TOTK_VK_DESC_STOCK=1'])
        self.assertEqual([g['direction'] for g in gens], ['right', 'left', 'right', 'left', None])

    def test_explicit_pan_direction(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.log'
            p.write_text('[totk-ctl] gen=1 frame=1000 mark ab arm=A block=1 direction=left\n'
                         '[totk-ctl] gen=2 frame=2000 mark ab arm=B direction=right\n', encoding='utf-8')
            gens = analyze.parse_ctl(p)
        self.assertEqual([g['direction'] for g in gens], ['left', 'right'])
        self.assertIsNone(analyze.pan_direction(None))

    def test_window_generation(self):
        gens = [{'gen': 1, 'frame': 1000, 'arm': 'A'}, {'gen': 2, 'frame': 2500, 'arm': 'B'}]
        wg = lambda s, e, guard=2: (lambda g: g and g['gen'])(analyze.window_generation(s, e, gens, guard))
        self.assertEqual(wg(1, 300), 0)            # before any live change
        self.assertEqual(wg(701, 1000), 0)         # the change applies after frame 1000: the window ends before it
        self.assertEqual(wg(901, 1200), None)      # straddles frame 1000
        self.assertEqual(wg(1003, 1302), 1)        # starts after the change plus the guard
        self.assertEqual(wg(1002, 1301), None)     # inside the guard
        self.assertEqual(wg(1002, 1301, guard=0), 1)
        self.assertEqual(wg(2201, 2500), 1)        # ends on the frame the next change follows
        self.assertEqual(wg(2202, 2501), None)
        self.assertEqual(wg(2801, 3100), 2)

    def make_run(self, d, a_fps=(30.0, 30.2), b_fps=(28.0, 28.1)):
        """Four ab.sh blocks A B B A, one pan each: block 3's last window straddles the change to block 4."""
        w = {}
        for f in (1800, 2100, 2400):
            w[f] = (a_fps[0], 40.0, 20.0, 1)
        for f in (3000, 3300, 3600, 3900):
            w[f] = (b_fps[0], 42.0, 20.0, 2)
        for f in (4500, 4800, 5100, 5400):
            w[f] = (b_fps[1], 43.0, 20.1, 2)
        w[6000] = (a_fps[1], 40.5, 19.9, 1)   # the last pan holds one whole window
        (d / 'logs').mkdir()
        (d / 'logs/thor.log').write_text(ab_log(w, self.CTL), encoding='utf-8')
        (d / 'pans.txt').write_text(''.join(f'thor hateno F0={f0} F1={f1} T0={t} T1={t + 30} V=16000 D={dd}\n' for f0, f1, t, dd in
                                            ((1200, 2400, 100, 1), (2400, 3900, 200, -1), (3900, 5400, 300, 1), (5400, 6000, 400, -1))))

    def test_analyze_arms_and_abtest(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.make_run(d)
            r = run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'ab.json')
            self.assertEqual(r.returncode, 0, r.stderr)
            doc = json.loads((d / 'ab.json').read_text(encoding='utf-8'))
            ab = doc['runs'][0]['ab']
            self.assertEqual(ab['windows'], {'assigned': 11, 'straddling': 1, 'unlabelled': 0})
            arms = ab['arms']
            self.assertEqual([b['block'] for b in arms['A']['blocks']], [1, 4])
            self.assertEqual([b['block'] for b in arms['B']['blocks']], [2, 3])
            self.assertEqual([b['direction'] for b in arms['A']['blocks']], ['right', 'left'])
            self.assertEqual([g['direction'] for g in ab['generations']], ['right', 'left', 'right', 'left', None])
            self.assertEqual(arms['B']['summary']['windows'], 7)   # 5400 straddles the change at frame 5200
            self.assertEqual(arms['A']['summary']['fps_mean'], round((30.0 * 3 + 30.2) / 4, 2))
            self.assertEqual(arms['A']['spread']['fps_mean'], 0.2)
            self.assertEqual(arms['B']['spread']['over50_total'], 0.0)
            straddler = next(w for w in doc['runs'][0]['windows'] if w['frames'][1] == 5400)
            self.assertIsNone(straddler['gen'])
            self.assertIn('arm B: 2 blocks, 7 windows', r.stdout)
            r = run(BENCH / 'abtest.py', d / 'ab.json')
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)   # B is 2 fps slower, beyond tolerance and noise
            self.assertIn('arm B vs A: WORSE', r.stdout)
            self.assertRegex(r.stdout, r'fps mean .* worse \*')
            self.assertIn('sign agreement 2/2 (ties 0); exact sign p=0.5000', r.stdout)
            self.assertRegex(r.stdout, r'>50 ms frames /win +1 +2 .* worse \*')

    def test_abtest_mixed_signs_and_single_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.make_run(d, a_fps=(30.0, 27.0), b_fps=(28.0, 28.4))   # A's own blocks differ by 3 fps
            self.assertEqual(run(BENCH / 'analyze.py', d, '--arms', '--json', d / 'ab.json').returncode, 0)
            r = run(BENCH / 'abtest.py', d / 'ab.json')
            self.assertRegex(r.stdout, r'fps mean .* same \*')
            self.assertIn('sign agreement 1/2 (ties 0); exact sign p=1.0000', r.stdout)
            doc = json.loads((d / 'ab.json').read_text(encoding='utf-8'))
            for a in doc['runs'][0]['ab']['arms'].values():   # incomplete quartet: no direction-matched pairs
                a['blocks'] = a['blocks'][:1]; a['spread'] = {k: None for k in a['spread']}
            (d / 'one.json').write_text(json.dumps(doc), encoding='utf-8')
            r = run(BENCH / 'abtest.py', d / 'one.json')
            self.assertRegex(r.stdout, r'fps mean .* worse\?')
            self.assertIn('arm B vs A: INCONCLUSIVE', r.stdout)
            self.assertEqual(r.returncode, 0)

    @staticmethod
    def records(a_rows, b_rows):
        """Compact block-summary fixtures; old runs intentionally omit direction metadata."""
        keys = ('windows', 'fps_mean', 'fps_min', 'p50_ms_median', 'p95_ms_max', 'max_ms', 'gpu_ms_mean',
                'over50_total', 'long_ge45_total')
        arms = []
        for positions, rows in (((1, 4), a_rows), ((2, 3), b_rows)):
            blocks, windows = [], []
            for i, row in enumerate(rows):
                summary = dict(zip(keys, row))
                blocks.append({'block': 4 * (i // 2) + positions[i % 2], 'summary': summary})
                windows += [dict(fps=summary['fps_mean'], p50_ms=summary['p50_ms_median'],
                                 p95_ms=summary['p95_ms_max'], max_ms=summary['max_ms'], gpu_ms=summary['gpu_ms_mean'],
                                 over50=summary['over50_total'] / summary['windows'],
                                 long_ge45=summary['long_ge45_total'] / summary['windows'])] * summary['windows']
            spreads = {}
            for key in analyze.AB_METRICS:
                values = [value for b in blocks if (value := analyze.block_metric(b['summary'], key)) is not None]
                spreads[key] = max(values) - min(values) if len(values) > 1 else None
            pooled = analyze.summarize(windows)
            pooled['fps_min'] = min(b['summary']['fps_min'] for b in blocks)
            arms.append({'blocks': blocks, 'summary': pooled, 'spread': spreads})
        return arms

    def fps_pairs(self, deltas):
        """Each quartet has a 20 fps view-content effect and two B-A deltas (right, left)."""
        a, b = [], []
        for right, left in zip(deltas[::2], deltas[1::2]):
            a += [[1, fps, 20, 20, 30, 40, 10, 0, 0] for fps in (30, 50)]
            b += [[1, fps, 20, 20, 30, 40, 10, 0, 0] for fps in (50 + left, 30 + right)]
        return self.records(a, b)

    def test_direction_confounded_win_and_loss(self):
        for delta, expected in ((10, 'better'), (-10, 'worse'), (0, 'same')):
            with self.subTest(delta=delta):
                ref, arm = self.fps_pairs([delta] * 4)
                stats = abtest.paired_metric('fps_mean', ref, arm)
                self.assertEqual([(p['reference_block'], p['arm_block']) for p in stats['pairs']],
                                 [(1, 3), (4, 2), (5, 7), (8, 6)])
                self.assertEqual(stats['delta'], delta)
                self.assertEqual(stats['directions'], {'right': delta, 'left': delta})
                self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)
                self.assertEqual(abtest.arm_verdict({k: abtest.metric_verdict(k, ref, arm)
                                                    for k in analyze.AB_METRICS}), expected)
                self.assertGreater(ref['spread']['fps_mean'], abs(delta))
                if delta:
                    self.assertEqual(abtest.metric_verdict('fps_mean', arm, ref)[3],
                                     'worse' if delta > 0 else 'better')

    def test_both_pooled_and_paired_delta_must_cross_tolerance(self):
        for delta in (10, -10):
            ref, arm = self.fps_pairs([delta] * 4)
            arm['summary']['fps_mean'] = ref['summary']['fps_mean']
            expected = 'lean better' if delta > 0 else 'lean worse'
            self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)
            self.assertEqual(abtest.arm_verdict({'fps_mean': abtest.metric_verdict('fps_mean', ref, arm)}), 'same')
            arm['summary']['fps_mean'] -= delta
            self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)  # opposite pooled sign
        ref, arm = self.fps_pairs([0] * 4)
        arm['summary']['fps_mean'] += 10
        self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], 'same')

    def test_sign_test_and_unanimity(self):
        for deltas, expected, probability in (
                ([4, 4, 4, 0], 'same', 0.25),       # a tie prevents unanimous confirmation
                ([4, 4, 4, -1], 'same', 0.625),     # mixed signs, too few pairs
                ([4] * 7 + [-1], 'same', 0.0703125),
                ([4] * 9 + [-1], 'better', 0.021484375),
                ([-4] * 9 + [1], 'worse', 0.021484375)):
            with self.subTest(deltas=deltas):
                ref, arm = self.fps_pairs(deltas)
                stats = abtest.paired_metric('fps_mean', ref, arm)
                self.assertAlmostEqual(stats['p_value'], probability)
                self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], expected)

    def test_pairs_require_quartet_and_matching_direction(self):
        ref, arm = self.fps_pairs([10] * 4)
        # Explicit direction wins over parity; mismatched directions cannot form a pair.
        arm['blocks'][1]['direction'] = 'left'
        stats = abtest.paired_metric('fps_mean', ref, arm)
        self.assertEqual(len(stats['pairs']), 3)
        ref['blocks'].pop()  # do not pair across the resulting incomplete quartet
        self.assertEqual(len(abtest.paired_metric('fps_mean', ref, arm)['pairs']), 1)
        self.assertEqual(abtest.metric_verdict('fps_mean', ref, arm)[3], 'better?')
        ref, arm = self.fps_pairs([10] * 4)
        for record in (ref, arm):
            for block in record['blocks']:
                block['block'] = None
        self.assertEqual(abtest.paired_metric('fps_mean', ref, arm)['pairs'], [])

    def test_odin_plan1_regressions(self):
        # Observed block summaries from .scratch/bench/live/odin-ab-e2d3609/<name>/ab.json.
        # Preserve windows/counts and all compared metrics, not only the headline fps values.
        cases = (
            ('ab1-split512', 'better', (22.58, 18.06),
             [[3, 38.63, 36.6, 22.58, 37.33, 42.7, 15.83, 0, 0],
              [4, 44.55, 34.8, 31.4, 37.79, 43.4, 14.95, 0, 0],
              [3, 37.1, 33.7, 28.88, 37.44, 44.7, 16.07, 0, 0],
              [4, 47.17, 38.4, 21.39, 37.37, 41.8, 14.43, 0, 0]],
             [[5, 54.18, 50.4, 18.27, 32.59, 37.7, 13.94, 0, 0],
              [4, 48.17, 40, 19.8, 38.77, 95.4, 14.6, 1, 1],
              [5, 51.16, 35.6, 17.93, 37.34, 42.7, 14.46, 0, 0],
              [4, 55.28, 54, 17.96, 21.63, 37.9, 13.88, 0, 0]]),
            ('ab2-prepmask', 'same', (20.64, 21.1),
             [[3, 37.6, 33.9, 22.02, 37.44, 48, 16.07, 0, 2],
              [4, 45.05, 31.8, 20.23, 37.44, 41.7, 14.8, 0, 0],
              [3, 41.43, 36.2, 27.34, 37.64, 52.5, 15.43, 2, 3],
              [4, 47.75, 38.6, 20.06, 37.02, 41.3, 14.4, 0, 0]],
             [[3, 42.37, 32.9, 20.4, 37.21, 40.3, 15.27, 0, 0],
              [3, 39.6, 34.1, 30.55, 37.43, 43, 15.8, 0, 0],
              [4, 48.05, 37, 20.42, 37.21, 40.2, 14.47, 0, 0],
              [3, 39.77, 36.4, 21.3, 36.82, 55, 15.47, 4, 8]]),
            ('ab6-lookout-split512', 'same', (18.04, 18.15),
             [[4, 53.12, 48.6, 18.23, 33.72, 40.3, 13.53, 0, 0],
              [5, 53.56, 49.8, 18.04, 22.78, 38, 13.72, 0, 0],
              [5, 53.64, 49, 18.12, 30.46, 37.4, 13.68, 0, 0],
              [5, 51.42, 45.2, 17.88, 34.53, 40.5, 13.66, 0, 0]],
             [[4, 54.6, 50.3, 18.15, 33.77, 39.5, 13.35, 0, 0],
              [4, 52.4, 48.5, 18.46, 32.74, 40.4, 13.72, 0, 0],
              [4, 52.75, 46.9, 18.34, 34.61, 37.2, 13.57, 0, 0],
              [4, 50.42, 44.8, 19.03, 36.35, 42.8, 13.88, 0, 0]]))
        for name, expected, medians, a_rows, b_rows in cases:
            with self.subTest(run=name):
                ref, arm = self.records(a_rows, b_rows)
                # A pooled median cannot be reconstructed from block medians: retain the observed pooled values.
                ref['summary']['p50_ms_median'], arm['summary']['p50_ms_median'] = medians
                per = {k: abtest.metric_verdict(k, ref, arm) for k in analyze.AB_METRICS}
                self.assertEqual(abtest.arm_verdict(per), expected, per)
                if name == 'ab6-lookout-split512':
                    self.assertEqual(per['p50_ms_median'][3], 'lean worse')
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / 'ab.json'
                    path.write_text(json.dumps({'runs': [{'device': 'odin', 'scene': name, 'ab': {
                        'windows': {'assigned': ref['summary']['windows'] + arm['summary']['windows'],
                                    'straddling': 0, 'unlabelled': 0}, 'arms': {'A': ref, 'B': arm}}}]}),
                                    encoding='utf-8')
                    result = run(BENCH / 'abtest.py', path)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f'arm B vs A: {expected.upper()}', result.stdout)
                    if name == 'ab1-split512':
                        self.assertIn('direction Δ: right +13.86, left +6.81; sign agreement 4/4', result.stdout)

    def test_ab_driver_records_alternating_direction(self):
        # Exercise the host driver with stand-in scripts only: never touch adb or a device.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'ab.sh').write_text((BENCH / 'ab.sh').read_text(encoding='utf-8'), encoding='utf-8')
            for script, text in (
                    ('live.sh', '#!/bin/sh\nfor line do printf "[totk-ctl] gen=1 frame=1000 %s\\n" "$line"; done\n'),
                    ('pan.sh', '#!/bin/sh\nprintf "pan args: %s\\n" "$*"\n'),
                    ('sleep', '#!/bin/sh\nexit 0\n')):
                (d / script).write_text(text, encoding='utf-8')
                (d / script).chmod(0o755)
            result = subprocess.run(['sh', str(d / 'ab.sh'), '-s', 'fake', '--device', 'test', '--scene', 'hateno',
                                     '--arms', 'A:unset TOTK_VK_DESC_STOCK;B:set TOTK_VK_DESC_STOCK=1',
                                     '--order', 'ABBA', '--settle', '0', '--event', '/fake', '--display', '0',
                                     '--out', str(d / 'out')],
                                    env={**os.environ, 'PATH': str(d) + os.pathsep + os.environ['PATH']},
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = (d / 'out/arms.txt').read_text(encoding='utf-8').splitlines()
            self.assertEqual(len(lines), 4)
            for index, direction in enumerate(('right', 'left', 'right', 'left'), 1):
                self.assertIn(f'block={index} arm={"ABBA"[index - 1]} direction={direction} gen=1 frame=1000', lines[index - 1])
            pans = [line for line in result.stdout.splitlines() if line.startswith('pan args:')]
            self.assertEqual(len(pans), 4)
            for line, direction in zip(pans, ('right', 'left', 'right', 'left')):
                self.assertIn(f'--first {direction} ', line)

    def test_scripts_parse(self):
        for script in ('live.sh', 'ab.sh', 'pan.sh'):
            r = subprocess.run(['sh', '-n', str(BENCH / script)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f'{script}: {r.stderr}')
        for script in ('live.sh', 'ab.sh'):
            r = subprocess.run(['sh', str(BENCH / script), '--help'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn('-s <serial>', r.stderr)
        # live.sh validates its lines before touching adb
        r = subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', 'set', 'TOTK_VK_DESC_STOCK=on'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn('value must be a decimal', r.stderr)
        r = subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', 'mark', 'a"b'], capture_output=True, text=True)
        self.assertIn('mark text', r.stderr)

    def test_live_write(self):
        """live.sh atomically publishes only its newline-terminated batch, with permissions/context prepared before
        appearance; old commands are not replayed, refusals are reported, and all commands share one root shell."""
        with fake_device() as dev:
            log, ctl = dev.game / 'totk.log', dev.game / 'totk.ctl'
            # chcon must see a nonempty, mode-correct temp, never the published path.
            (dev.dir / 'bin/chcon').write_text(
                '#!/bin/sh\nprintf "%s\\n" "$*" >> "$FAKE_CHCON"\n'
                '[ "$#" -eq 2 ] && [ "$1" = "$FAKE_CONTEXT" ] || exit 2\n'
                'for arg do file=$arg; done\n'
                f'[ "$file" != "{ctl}" ] && [ -s "$file" ] && [ "$(stat -c %a "$file")" = 600 ]\n')
            log.write_text('[totk-ctl] following\n')
            stop = threading.Event()
            game = threading.Thread(target=fake_game, args=(ctl, log, stop), daemon=True)
            game.start()
            try:
                def live(*lines):
                    return subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', '--game-dir', str(dev.game),
                                           '--wait', '5', *lines], capture_output=True, text=True, env=dev.env)
                r = live('set', 'TOTK_VK_PIPE_BOOST=1', 'mark', 'a', 'b')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('gen=1 frame=10 mark a b', r.stdout)
                first_inode = ctl.stat().st_ino
                self.assertEqual(ctl.read_text(), 'set TOTK_VK_PIPE_BOOST=1\nmark a b\n')
                r = live('unset TOTK_VK_PIPE_BOOST', 'mark ok')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('gen=2 frame=20 mark ok', r.stdout)
                self.assertEqual(ctl.read_text(), 'unset TOTK_VK_PIPE_BOOST\nmark ok\n')
                self.assertNotEqual(ctl.stat().st_ino, first_inode)
                self.assertFalse(list(dev.game.glob('.totk.ctl.*')))
                self.assertEqual(oct(ctl.stat().st_mode & 0o777), '0o600')
                r = live('set', 'TOTK_X=1', 'mark', 'r')
                self.assertEqual(r.returncode, 4, r.stderr)
                self.assertIn('refused "set TOTK_X=1"', r.stdout)
                attempts = (dev.dir / 'chcon-calls').read_text().splitlines()
                self.assertEqual(len(attempts), 3)
                for attempt in attempts:
                    context, temporary = attempt.split()
                    self.assertEqual(context, dev.env['FAKE_CONTEXT'])
                    self.assertEqual(Path(temporary).parent, dev.game)
                    self.assertTrue(Path(temporary).name.startswith('.totk.ctl.'))
            finally:
                stop.set()
            self.assertEqual(dev.root_shells(), 1)

    def test_live_prepare_failure_keeps_previous_batch(self):
        with fake_device() as dev:
            ctl = dev.game / 'totk.ctl'
            ctl.write_text('mark previous\n')
            (dev.game / 'totk.log').write_text('')
            (dev.dir / 'bin/chcon').write_text('#!/bin/sh\nexit 1\n')
            r = subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', '--game-dir', str(dev.game),
                                'mark next'], capture_output=True, text=True, env=dev.env)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn('could not write', r.stderr)
            self.assertEqual(ctl.read_text(), 'mark previous\n')
            self.assertFalse(list(dev.game.glob('.totk.ctl.*')))

    def test_live_context_failure_stops_before_appending(self):
        with fake_device() as dev:
            dev.env['FAKE_CONTEXT'] = '?'
            result = subprocess.run(['sh', str(BENCH / 'live.sh'), '-s', 'x', '--game-dir', str(dev.game),
                                     '--wait', '0', 'set', 'TOTK_VK_PIPE_BOOST=1'],
                                    capture_output=True, text=True, env=dev.env)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertFalse((dev.game / 'totk.ctl').exists())
            self.assertFalse(list(dev.game.glob('.totk.ctl.*')))
            self.assertFalse((dev.dir / 'chcon-calls').exists())


FAKE_ADB = r'''#!/bin/sh
# adb -s <serial> shell <command> with this host as the rooted device; every call is logged to $FAKE_CALLS
echo "$*" >> "$FAKE_CALLS"
shift 3
case $1 in
  pidof) exit 0 ;;
  'su -c sh') exec sh ;;
  'su -c '*) script=${1#su -c }; eval "exec sh -c $script" ;;
  *) echo "fake adb: unexpected $1" >&2; exit 1 ;;
esac
'''


class fake_device:
    """A temp dir with a fake adb (and chcon) on PATH and its own rootsh.py socket dir; stops the broker on exit."""

    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.dir = Path(self.tmp.name)
        (d / 'bin').mkdir(); (d / 'game').mkdir()
        for name, text in (
                ('adb', FAKE_ADB),
                ('ls', '#!/bin/sh\nif [ "$1" = "-Zd" ]; then printf "%s %s\\n" "$FAKE_CONTEXT" "$2"; '
                       'else exec /bin/ls "$@"; fi\n'),
                ('chcon', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$FAKE_CHCON"\n'
                          '[ "$#" -eq 2 ] && [ "$1" = "$FAKE_CONTEXT" ] || exit 2\n')):
            (d / 'bin' / name).write_text(text); (d / 'bin' / name).chmod(0o755)
        self.game, self.calls = d / 'game', d / 'calls'
        self.env = dict(os.environ, PATH=f"{d / 'bin'}:{os.environ['PATH']}", FAKE_CALLS=str(self.calls),
                        XDG_RUNTIME_DIR=str(d), FAKE_CONTEXT='u:object_r:app_data_file:s0:c123,c456',
                        FAKE_CHCON=str(d / 'chcon-calls'))
        return self

    def rootsh(self, *args, stdin=None):
        return subprocess.run([sys.executable, str(BENCH / 'rootsh.py'), '-s', 'x', *args], input=stdin,
                              capture_output=True, env=self.env)

    def root_shells(self):
        return self.calls.read_text().count('su -c sh') if self.calls.exists() else 0

    def __exit__(self, *exc):
        self.rootsh('stop')
        self.tmp.cleanup()


def fake_game(ctl, log, stop):
    """The game's side of totk.ctl: each poll that finds new complete lines is one generation."""
    seen, gen, inode = 0, 0, None
    while not stop.is_set():
        try:
            with ctl.open('rb') as f:
                current = os.fstat(f.fileno()).st_ino
                data = f.read()
            if current != inode:
                seen, inode = 0, current
        except FileNotFoundError:
            data = b''
        end = data.rfind(b'\n') + 1
        if end > seen:
            gen += 1
            out = []
            for line in data[seen:end].decode().splitlines():
                if line.startswith('set TOTK_X'):
                    out.append(f'[totk-ctl] refused "{line}": TOTK_X is not a live switch')
                elif line.startswith('mark '):
                    out.append(f'[totk-ctl] gen={gen} frame={gen * 10} {line}')
                else:
                    out.append(f'[totk-ctl] gen={gen} frame={gen * 10} applied {line.removeprefix("set ")}')
            seen = end
            with log.open('a') as f:
                f.write(''.join(x + '\n' for x in out))
        time.sleep(0.02)


class RootShell(unittest.TestCase):
    def test_broker(self):
        """One root shell serves every request; each script's output, status, exit and syntax errors stay its own."""
        with fake_device() as dev:
            r = dev.rootsh('run', '-c', 'echo out; echo err >&2; exit 3')
            self.assertEqual((r.returncode, r.stdout, r.stderr), (3, b'out\n', b'err\n'))
            r = dev.rootsh('run', '-c', 'if then')   # a syntax error ends only that script
            self.assertNotEqual(r.returncode, 0)
            r = dev.rootsh('run', stdin=b"printf '\\000\\377abc'")   # binary output, no final newline
            self.assertEqual((r.returncode, r.stdout), (0, b'\x00\xffabc'))
            r = dev.rootsh('run', '-c', 'cat; echo done')   # stdin is /dev/null, not the root shell's
            self.assertEqual((r.returncode, r.stdout), (0, b'done\n'))
            self.assertRegex(dev.rootsh('status').stdout.decode(), r'^rootsh pid=\d+ cpus=')
            self.assertEqual(dev.root_shells(), 1)
            self.assertEqual(dev.rootsh('stop').returncode, 0)
            self.assertFalse((dev.dir / 'totk-rootsh-x.sock').exists())
            self.assertIn(b'not running', dev.rootsh('status').stdout)


class PanMeasurement(unittest.TestCase):
    def test_long_default_and_repeats_keep_independent_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'pan.sh').write_text((BENCH / 'pan.sh').read_text())
            scripts = {
                'adb': '#!/bin/sh\nexit 0\n',
                'bench_input.sh': '#!/bin/sh\necho "input $*" >> "$CALLS"\ncase "$*" in *freqs) echo "cpu0=0:1:2:2";; esac\n',
                'sleep': '#!/bin/sh\necho "sleep $*" >> "$CALLS"\n',
                'date': '#!/bin/sh\nn=$(cat "$CLOCK" 2>/dev/null); n=$(( ${n:-100} + 1 )); echo "$n" > "$CLOCK"; echo "$n"\n',
                'python3': '#!/bin/sh\necho "python $*" >> "$CALLS"\n'
                           'case "$1" in *rootsh.py) case "$*" in *settings.env*) printf "%s\\n" "${PRESET_ENV:-TOTK_FPS60=1}";; '
                           '*) n=$(cat "$FRAME" 2>/dev/null); n=$(( ${n:-0} + 300 )); '
                           'echo "$n" > "$FRAME"; echo "至帧 $n";; esac;; esac\n',
            }
            for name, content in scripts.items():
                (d / name).write_text(content)
                (d / name).chmod(0o755)
            env = dict(os.environ, PATH=str(d) + os.pathsep + os.environ['PATH'], CALLS=str(d / 'calls'),
                       CLOCK=str(d / 'clock'), FRAME=str(d / 'frame'))
            common = ['sh', str(d / 'pan.sh'), '-s', 'fake', '--device', 'odin', '--scene', 'hateno',
                      '--target-fps', '60', '--no-shot', '--event', '/fake', '--display', '0']
            result = subprocess.run(common + ['--out', str(d / 'long')], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('pan 16000 72\n', (d / 'calls').read_text())
            (d / 'calls').write_text('')
            result = subprocess.run(common + ['--seconds', '48', '--repeat', '3', '--out', str(d / 'repeats')],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = (d / 'repeats/pans.txt').read_text().splitlines()
            self.assertEqual(len(rows), 3)
            identities = [dict(t.split('=', 1) for t in row.split()[2:]) for row in rows]
            self.assertEqual(len({r['T0'] for r in identities}), 3)
            self.assertTrue(all(r['TARGET_FPS'] == '60' for r in identities))
            calls = (d / 'calls').read_text()
            self.assertEqual(calls.count('pan 16000 24\n'), 3)
            self.assertNotIn('sleep 24\n', calls)  # sleeps are inside one device command now
            pan_index = calls.index('pan 16000 24\n')
            final_read = calls.index("grep -ao", pan_index)
            final_gpu = calls.index('--boundary end', pan_index)
            self.assertLess(pan_index, final_read)
            self.assertLess(final_read, final_gpu)
            self.assertEqual(calls.count('--boundary start'), 3)
            self.assertEqual(calls.count('--boundary end'), 3)
            self.assertEqual(len((d / 'repeats/freqs.txt').read_text().splitlines()), 6)
            # Existing callers (including ab.sh) omit --target-fps: infer the live 60 preset, never stamp 30.
            index = common.index('--target-fps')
            auto = common[:index] + common[index + 2:]
            (d / 'calls').write_text('')
            result = subprocess.run(auto + ['--out', str(d / 'auto')], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('TARGET_FPS=60', (d / 'auto/pans.txt').read_text())
            self.assertIn('pan 16000 72\n', (d / 'calls').read_text())
            for name, preset, target in (
                    ('forty', 'TOTK_FPS60=1\nTOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=40', 40),
                    ('thirty', 'TOTK_FPS60=0\nTOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=40', 30),
                    ('disabled', 'TOTK_FPS60=1\nTOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=40\nTOTK_UNCAPPED=0', 60),
                    ('override', 'TOTK_FPS60=1\nTOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=40\nTOTK_UNCAPPED_HZ=55', 60)):
                result = subprocess.run(auto + ['--out', str(d / name)], env={**env, 'PRESET_ENV': preset},
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f'TARGET_FPS={target}', (d / name / 'pans.txt').read_text())
            explicit = [*common]
            explicit[explicit.index('--target-fps') + 1] = '40'
            result = subprocess.run(explicit + ['--out', str(d / 'explicit40')], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('TARGET_FPS=40', (d / 'explicit40/pans.txt').read_text())


class GpuResidency(unittest.TestCase):
    SOURCE = '/sys/class/kgsl/kgsl-3d0/devfreq/trans_stat'

    @staticmethod
    def trans_stat(high, low):
        # Kernel uses fixed 10-character columns: adjacent 10-digit frequencies have no separating whitespace.
        return ('     From  :   To\n           :11000000001050000000   time(ms)\n'
                f'*1100000000:         0        80 {high:10d}\n'
                f' 1050000000:        90         0 {low:10d}\nTotal transition : 170\n')

    def record(self, boundary, high, low):
        return {'schema': sampler.GPU_SNAPSHOT_SCHEMA, 'device': 'odin', 'scene': 'hateno', 'T0': 100,
                'boundary': boundary, 'serial_sha256': 'a' * 64, 'boot_id': 'boot', 'status': 'ok',
                'source': self.SOURCE, 'reset_count': '0', 'uptime_start': 1 if boundary == 'start' else 50,
                'uptime_end': 2 if boundary == 'start' else 51, 'raw': self.trans_stat(high, low)}

    def test_trans_stat_uses_residency_column_not_transitions(self):
        parsed = sampler.parse_gpu_time_in_state(self.trans_stat(1000, 2000), self.SOURCE)
        self.assertEqual(parsed['counters'], {1100000000: 1000, 1050000000: 2000})
        self.assertEqual(parsed['counter_unit'], 'ms')
        a, b = self.record('start', 1000, 2000), self.record('end', 1300, 2100)
        delta = sampler.gpu_residency_delta(a, b)
        self.assertEqual(delta['deltas'], {1100000000: 300, 1050000000: 100})
        self.assertEqual(delta['fractions'], {1100000000: 0.75, 1050000000: 0.25})
        self.assertEqual(delta['counter_kind'], 'residency')
        for broken in (dict(b, raw=self.trans_stat(999, 2100)), dict(b, boot_id='reboot'),
                       dict(b, uptime_start=0), dict(b, status='error')):
            with self.subTest(broken=broken):
                with self.assertRaises(ValueError):
                    sampler.gpu_residency_delta(a, broken)
        with self.assertRaises(ValueError):
            sampler.gpu_residency_delta(None, b)

    def test_gpu_start_count_change_keeps_monotonic_sane_residency(self):
        a = dict(self.record('start', 1000, 2000), reset_count='73936', uptime_start=100, uptime_end=100.01)
        b = dict(self.record('end', 21000, 126700), reset_count='73937', uptime_start=244.70, uptime_end=244.71)
        delta = sampler.gpu_residency_delta(a, b)
        self.assertEqual(delta['reset_count_delta'], 1)
        self.assertEqual(delta['total_delta'], 144700)
        self.assertAlmostEqual(delta['time_sanity']['counter_seconds'], 144.7)
        self.assertEqual(delta['time_sanity']['status'], 'passed')
        # Actual counter regression remains invalid independently of GPU-start count.
        with self.assertRaisesRegex(ValueError, 'counter reset'):
            sampler.gpu_residency_delta(a, dict(b, raw=self.trans_stat(999, 126700)))
        with self.assertRaisesRegex(ValueError, 'exceeds elapsed'):
            sampler.gpu_residency_delta(a, dict(b, raw=self.trans_stat(1000000, 126700)))
        unknown = dict(b, raw=b['raw'].replace('time(ms)', 'time(jiffies)'))
        old_unknown = dict(a, raw=a['raw'].replace('time(ms)', 'time(jiffies)'))
        self.assertEqual(sampler.gpu_residency_delta(old_unknown, unknown)['time_sanity']['status'], 'unavailable')

    def test_clock_stats_vector_is_active_time_not_wall_residency(self):
        parsed = sampler.parse_gpu_time_in_state('100 300\n', '/sys/class/kgsl/kgsl-3d0/gpu_clock_stats',
                                                 '660000000 734000000')
        self.assertEqual(parsed['counters'], {660000000: 100, 734000000: 300})
        self.assertEqual(parsed['counter_kind'], 'active')
        self.assertEqual(parsed['counter_unit'], 'unknown')
        with self.assertRaises(ValueError):
            sampler.parse_gpu_time_in_state('100 300', '/sys/class/kgsl/kgsl-3d0/gpu_clock_stats')

    def test_analyzer_reports_per_pan_residency_and_battery(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'logs').mkdir()
            (d / 'logs/odin.log').write_text(ab_log({600: (57, 20, 10, 0)}, []))
            (d / 'pans.txt').write_text('odin hateno F0=0 F1=600 T0=100 T1=120 V=16000 TARGET_FPS=60\n')
            (d / 'samples-odin.txt').write_text('101 f=300 ac= usb=1 level=82 '
                                              'supply.usb.type=USB supply.usb.online=1 caps= 1/1\n')
            path = d / sampler.GPU_SNAPSHOT_FILE
            records = [self.record('start', 1000, 2000), self.record('end', 1300, 2100)]
            path.write_text(''.join(json.dumps(r) + '\n' for r in records))
            self.assertEqual(set(sampler.parse_gpu_snapshots(path)), {('odin', 100)})
            result = run(BENCH / 'analyze.py', d, '--json', d / 'result.json')
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads((d / 'result.json').read_text())['runs'][0]
            path.write_text(json.dumps(records[0]) + '\n' + json.dumps(records[0]) + '\n')
            with self.assertRaises(ValueError):
                sampler.parse_gpu_snapshots(path)
        self.assertEqual(record['pans'][0]['gpu_residency']['fractions']['1100000000'], 0.75)
        self.assertEqual(record['pans'][0]['battery'], [{'epoch': 101, 'ac': None, 'usb': 1, 'level': 82,
                         'external_power': True, 'power_supplies': {'usb': {'type': 'USB', 'online': 1}}}])
        self.assertIn('1100 MHz 75.00%', result.stdout)
        self.assertIn('only 1 complete windows', result.stdout)


class MemorySampling(unittest.TestCase):
    def test_external_power_is_any_kernel_supply_not_android_charger_category(self):
        cases = [
            ({'ac': '', 'usb': '1', 'supply.usb.type': 'USB', 'supply.usb.online': '1'}, True),
            ({'supply.dc.type': 'Mains', 'supply.dc.online': '0',
              'supply.wireless.type': 'Wireless', 'supply.wireless.online': '1'}, True),
            ({'supply.dc.type': 'Mains', 'supply.dc.online': '0'}, False),
            ({'supply.usb.type': 'USB', 'supply.usb.online': ''}, None),
            ({'supply.battery.type': 'Battery', 'supply.battery.online': '1'}, None),
            ({'ac': '', 'usb': '1'}, True),  # historical rows
            ({}, None)]
        for tokens, expected in cases:
            with self.subTest(tokens=tokens):
                self.assertIs(sampler.power_of_tokens(tokens)['external_power'], expected)
        line = sampler.add_power('f=300 ac= usb=1 supply.usb.type=USB supply.usb.online=1 caps= 1/1')
        self.assertIn('external_power=1 caps=', line)
        self.assertIn('supply.usb.type=USB supply.usb.online=1', line)

    LINE = ('f={f} cpu=55000 gpu=60000 cool=0 tpl=0 gclk=1 gcap=2 mav={mav} rss=2000000 swp={swp} shm=1000 kgk=1048576 '
            'kgm=3145728 kgi=2097152 pcs=1.50 pcf=0.00 pms={pms} pmf=3.25 kst={kst} caps= 2016000/2016000')

    def test_imported_mem_path_and_legacy_fallback_are_generated(self):
        self.assertIn('kv imported_mem', sampler.LOOP)
        self.assertIn('kv imported)', sampler.LOOP)
        self.assertIn('kgi=$(kv_imported)', sampler.LOOP)
        self.assertIn('pgrep kswapd', sampler.LOOP)
        self.assertIn("cut -d' ' -f14,15 /proc/$k/stat", sampler.LOOP)
        self.assertIn('kst=$ks', sampler.LOOP)

    def test_physical_kgsl_accounting_requires_kernel_and_imported(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'samples-duo.txt'
            p.write_text(
                '1.0 f=300 kgk=1048576 kgm=3145728 kgi=2097152 caps= 1/1\n'
                '2.0 f=300 kgk=0 kgm=0 kgi=0 caps= 1/1\n'
                '3.0 f=300 kgk=1048576 kgm=3145728 caps= 1/1\n'
                '4.0 f=300 kgk= kgm=3145728 kgi=2097152 caps= 1/1\n',
                encoding='utf-8')
            rows = analyze.parse_sample_rows(p)
        self.assertEqual(rows[0]['kgsl'], 3 * (1 << 20))
        self.assertEqual(rows[0]['kgm'], 3 * (1 << 20))
        self.assertEqual(rows[1]['kgsl'], 0)
        self.assertIsNone(rows[2]['kgsl'])
        self.assertIsNone(rows[3]['kgsl'])
        self.assertEqual(analyze.memory_of(rows[:1])['kgsl_max_mb'], 3.0)
        self.assertEqual(analyze.memory_of(rows[2:])['kgsl_kernel_max_mb'], 1.0)
        self.assertNotIn('kgsl_max_mb', analyze.memory_of(rows[2:]))

    def test_kswapd_percent(self):
        import sampler
        line, st = sampler.add_kswapd(self.LINE.format(f=300, mav=1, swp=1, pms=1, kst=1000), 100.0, None)
        self.assertIn(' ksw= caps=', line)
        line, st = sampler.add_kswapd(self.LINE.format(f=300, mav=1, swp=1, pms=1, kst=1150), 103.0, st)
        self.assertIn(' ksw=50.0 caps=', line)   # 150 ticks of 100 Hz in 3 s
        self.assertEqual(st, (103.0, 1150))
        line, st = sampler.add_kswapd('f=1 cpu= kst= caps= 1/1', 106.0, st)   # no kswapd readable
        self.assertIn(' ksw= caps=', line)
        self.assertIsNone(st)

    def test_window_memory_and_old_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'samples-duo.txt'
            rows = [self.LINE.format(f=f, mav=mav, swp=swp, pms=pms, kst=0) + '\n' for f, mav, swp, pms in
                    ((300, 900000, 100000, '2.00'), (300, 400000, 500000, '9.50'), (600, 350000, 600000, '1.00'))]
            p.write_text(''.join(f'{100 + i * 3}.0 {r}' for i, r in enumerate(rows))
                         + '200.0 f=600 cpu=1 gpu=2 cool=0 tpl=0 gclk=1 gcap=2 caps= 1/1\n', encoding='utf-8')   # an old-format row
            got = analyze.parse_sample_rows(p)
        self.assertEqual(len(got), 4)
        self.assertIsNone(got[3]['mav'])
        m = analyze.window_memory(600, got)   # samples taken while the window ending at 600 ran: f in [300, 600)
        self.assertEqual(m['mem_available_min_mb'], round(400000 / 1024, 1))
        self.assertEqual(m['swap_max_mb'], round(500000 / 1024, 1))
        self.assertEqual(m['psi_mem_some_max'], 9.5)
        self.assertEqual(m['kgsl_max_mb'], 3.0)   # kernel + imported; mapped overlaps kernel
        self.assertEqual(m['kgsl_mapped_max_mb'], 3.0)
        self.assertEqual(analyze.window_memory(300, got), {})
        self.assertEqual(analyze.worst_memory([m, {'mem_available_min_mb': 100.0, 'swap_max_mb': 1.0}])['mem_available_min_mb'], 100.0)


class PanRelease(unittest.TestCase):
    def test_device_batch_is_symmetric_and_releases_before_return(self):
        for first in (16000, -16000):
            with self.subTest(first=first), fake_device() as dev:
                events = dev.dir / 'events'
                for name, source in (
                        ('sendevent', '#!/bin/sh\nprintf "event %s\\n" "$*" >> "$EVENTS"\n'),
                        ('sleep', '#!/bin/sh\nprintf "sleep %s\\n" "$*" >> "$EVENTS"\n')):
                    path = dev.dir / 'bin' / name
                    path.write_text(source)
                    path.chmod(0o755)
                dev.env['EVENTS'] = str(events)
                result = subprocess.run(['sh', str(BENCH / 'bench_input.sh'), '-s', 'x', '--event', '/fake',
                                         '--game-dir', str(dev.game), 'pan', str(first), '72'],
                                        env=dev.env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(events.read_text().splitlines(), [
                    f'event /fake 3 2 {first}', 'event /fake 3 5 0', 'event /fake 0 0 0', 'sleep 72',
                    f'event /fake 3 2 {-first}', 'event /fake 3 5 0', 'event /fake 0 0 0', 'sleep 72',
                    'event /fake 3 2 0', 'event /fake 3 5 0', 'event /fake 0 0 0'])
                self.assertEqual(dev.root_shells(), 1)

    def test_interrupted_sleep_releases_stick(self):
        with fake_device() as dev:
            for name, source in (
                    ('sendevent', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$EVENTS"\n'),
                    ('sleep', '#!/bin/sh\nexit 1\n')):
                path = dev.dir / 'bin' / name
                path.write_text(source)
                path.chmod(0o755)
            events = dev.dir / 'events'
            dev.env['EVENTS'] = str(events)
            result = subprocess.run(['sh', str(BENCH / 'bench_input.sh'), '-s', 'x', '--event', '/fake',
                                     '--game-dir', str(dev.game), 'pan', '16000', '.1'],
                                    env=dev.env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(events.read_text().splitlines()[-3:], ['/fake 3 2 0', '/fake 3 5 0', '/fake 0 0 0'])
            self.assertEqual(list(dev.game.glob('.bench-pan-*')), [])

    def test_host_signal_cancels_owned_batch_without_waiting_for_its_broker(self):
        for group in (False, True):
            with self.subTest(process_group=group), fake_device() as dev:
                events = dev.dir / 'events'
                releases = dev.dir / 'releases'
                event_script = dev.dir / 'bin/sendevent'
                # Timestamp the completed zero-axis/SYN sequence in the fake device,
                # not the later host/broker teardown or the test runner's next timeslice.
                event_script.write_text(f'#!{sys.executable}\n' + '''import os, sys, time
event = ' '.join(sys.argv[1:])
with open(os.environ['EVENTS'], 'a') as output:
    output.write(event + '\\n')
if event == '/fake 0 0 0':
    # Batch and direct-root cleanup may interleave identical zero events.
    axes = {}
    released = False
    with open(os.environ['EVENTS']) as events:
        for line in events:
            node, kind, code, value = line.split()
            if kind == '3':
                axes[code] = value
            elif kind == '0' and code == '0' and axes.get('2') == axes.get('5') == '0':
                released = True
    if released:
        with open(os.environ['RELEASES'], 'a') as output:
            output.write(str(time.monotonic()) + '\\n')
''')
                event_script.chmod(0o755)
                dev.env.update(EVENTS=str(events), RELEASES=str(releases))
                (dev.game / 'totk.log').write_text('[nvn] 计时 至帧 300： timing\n')
                process = subprocess.Popen(['sh', str(BENCH / 'pan.sh'), '-s', 'x', '--device', 'test',
                    '--scene', 'hateno', '--event', '/fake', '--display', '0', '--game-dir', str(dev.game),
                    '--target-fps', '60', '--seconds', '20', '--no-shot', '--out', str(dev.dir / 'out')],
                    env=dev.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if events.exists() and '/fake 3 2 16000' in events.read_text():
                        break
                    time.sleep(.02)
                else:
                    process.kill()
                    out, err = process.communicate(timeout=5)
                    self.fail(f'pan never started: {out!r} {err!r}')
                started = time.monotonic()
                if group:
                    os.killpg(process.pid, signal.SIGINT)
                else:
                    process.terminate()
                # Release must still finish within 6 s (before the 10 s first half).
                # Allow 30 s only for draining pipes, reaping children and broker cleanup.
                out, err = process.communicate(timeout=30)
                self.assertNotEqual(process.returncode, 0, out)
                self.assertTrue(releases.exists(), err)
                released = min(map(float, releases.read_text().splitlines()))
                self.assertGreaterEqual(released, started, err)
                self.assertLess(released - started, 6, err)
                self.assertEqual(events.read_text().splitlines()[-3:],
                                 ['/fake 3 2 0', '/fake 3 5 0', '/fake 0 0 0'])
                self.assertNotIn('/fake 3 2 -16000', events.read_text())
                self.assertEqual(list(dev.game.glob('.bench-pan-*')), [])


class RestartQuartet(unittest.TestCase):
    @staticmethod
    def documents(fps_values, counts=(2, 2, 2, 2)):
        documents = []
        for fps, count in zip(fps_values, counts):
            windows = [dict(fps=fps, uncapped=True, p50_ms=25, p95_ms=30, max_ms=40,
                            over33=0, over50=0, long_ge45=0)] * count
            documents.append({'runs': [{'device': 'thor', 'scene': 'hateno', 'windows': windows,
                                        'summary': analyze.summarize(windows),
                                        'pans': [{'direction': 'right', 'stick_value': 16000}]}]})
        return documents

    def records(self, fps_values, counts=(2, 2, 2, 2)):
        doc = abtest.restart_analysis(self.documents(fps_values, counts), 'O')
        return doc, doc['runs'][0]['ab']['arms']['O'], doc['runs'][0]['ab']['arms']['B']

    def test_explicit_restart_pairs_confirm_same_direction_sessions(self):
        for fps, expected in (((40, 42, 42, 40), 'better'), ((40, 38, 38, 40), 'worse'),
                              ((40, 40.8, 40.8, 40), 'same')):
            _, ref, arm = self.records(fps)
            stats = abtest.paired_metric('throughput_fps', ref, arm, 'restart-quartet')
            self.assertEqual([(p['reference_block'], p['arm_block']) for p in stats['pairs']], [(1, 2), (4, 3)])
            self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm, stats)[3], expected)
            # The default in-session contract is unchanged, not silently selected by JSON metadata.
            self.assertEqual(len(abtest.paired_metric('throughput_fps', ref, arm)['pairs']), 0)
            if expected != 'same':
                self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm)[3], expected + '?')

    def test_reference_spread_is_an_admission_gate(self):
        for fps in ((40, 44, 44, 42), (40, 44, 44, 40.81)):
            _, ref, arm = self.records(fps)
            stats = abtest.paired_metric('throughput_fps', ref, arm, 'restart-quartet')
            self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm, stats)[3], 'inconclusive')
        _, ref, arm = self.records((40, 44, 44, 40.8))
        stats = abtest.paired_metric('throughput_fps', ref, arm, 'restart-quartet')
        self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm, stats)[3], 'better')

    def test_pooled_pair_signs_and_missing_pairs_are_not_confirmed(self):
        for values, counts in (((40, 44, 39, 40), (2, 2, 2, 2)),
                               ((40, 44, 39.9, 40), (2, 1, 100, 2))):
            _, ref, arm = self.records(values, counts)
            stats = abtest.paired_metric('throughput_fps', ref, arm, 'restart-quartet')
            self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm, stats)[3], 'inconclusive')
        _, ref, arm = self.records((40, 44, 44, 40))
        arm['blocks'].pop()
        stats = abtest.paired_metric('throughput_fps', ref, arm, 'restart-quartet')
        self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm, stats)[3], 'inconclusive')
        _, ref, arm = self.records((40, 44, 44, 40))
        arm['summary']['throughput_fps'] = 40.1  # paired speedup alone is insufficient
        stats = abtest.paired_metric('throughput_fps', ref, arm, 'restart-quartet')
        self.assertEqual(abtest.metric_verdict('throughput_fps', ref, arm, stats)[3], 'inconclusive')

    def test_restart_cli_labels_mode_and_rejects_incomplete_or_unmatched_schedules(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            documents = self.documents((40, 42, 42, 40))
            for i, document in enumerate(documents):
                path = Path(tmp) / f'{i}.json'
                path.write_text(json.dumps(document))
                paths.append(path)
            result = run(BENCH / 'abtest.py', '--restart-quartet', *paths, '--ref', 'O')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('comparison mode: restart-quartet', result.stdout)
            self.assertIn('arm B vs O: BETTER', result.stdout)
            self.assertIn('O-spread:', result.stdout)
            self.assertIn('both adjacent pair signs must agree', result.stdout)
            result = run(BENCH / 'abtest.py', '--restart-quartet', *paths[:3])
            self.assertEqual(result.returncode, 2)
            documents[1]['runs'][0]['pans'][0]['direction'] = 'left'
            paths[1].write_text(json.dumps(documents[1]))
            result = run(BENCH / 'abtest.py', '--restart-quartet', *paths)
            self.assertEqual(result.returncode, 2)
            self.assertIn('identical, explicit pan', result.stderr)


    def test_missing_session_metric_telemetry_cannot_confirm(self):
        documents = self.documents((40, 44, 44, 40))
        documents[0]['runs'][0]['summary']['throughput_fps'] = None
        arms = abtest.restart_analysis(documents, 'O')['runs'][0]['ab']['arms']
        stats = abtest.paired_metric('throughput_fps', arms['O'], arms['B'], 'restart-quartet')
        self.assertIsNone(stats['reference_spread'])
        self.assertEqual(abtest.metric_verdict('throughput_fps', arms['O'], arms['B'], stats)[3], 'inconclusive')
        documents[1]['runs'][0]['pans'][0]['direction'] = None
        with self.assertRaisesRegex(ValueError, 'explicit pan'):
            abtest.restart_analysis(documents, 'O')

    def clock_paths(self, tmp, clocks):
        """Session files for a 40/44/44/40 fps quartet whose pans carry the given per-session GPU residency."""
        paths = []
        for i, (document, fractions) in enumerate(zip(self.documents((40, 44, 44, 40)), clocks)):
            if fractions is not None:
                document['runs'][0]['pans'][0]['gpu_residency'] = {
                    'status': 'ok', 'counter_kind': 'residency', 'fractions': fractions}
            path = Path(tmp) / f'clock{i}.json'
            path.write_text(json.dumps(document))
            paths.append(path)
        return paths

    def test_arms_at_different_gpu_clocks_do_not_get_a_verdict(self):
        low, high = {'615000000': 1.0}, {'680000000': 1.0}
        mixed = {'680000000': 0.7, '615000000': 0.3}  # 660.5 MHz: 2.9% under 680, an ordinary pan
        with tempfile.TemporaryDirectory() as tmp:
            for clocks, flags, headline, note in (
                    ((low, high, high, low), (), 'verdict confounded by GPU clock', 'arms differ by 9.6%'),
                    ((low, high, high, low), ('--clock-lever',), 'BETTER', 'intended (--clock-lever)'),
                    ((high, mixed, mixed, high), (), 'BETTER', 'arms differ by 2.9%'),
                    ((high, high, None, high), (), 'BETTER', 'clock confound not checked')):
                with self.subTest(clocks=clocks, flags=flags):
                    result = run(BENCH / 'abtest.py', '--restart-quartet', *self.clock_paths(tmp, clocks),
                                 '--ref', 'O', *flags)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f'arm B vs O: {headline}', result.stdout)
                    self.assertIn(note, result.stdout)


class CompileProvenance(unittest.TestCase):
    @staticmethod
    def line(frame, sync=0, worker=0, new=0):
        return (f'[nvn-vk] frame {frame}, stutter: longest frame 40.0 ms, 0 frames over 33 ms, 0 over 50 ms; '
                f'{new} new pipelines took 1.2 ms (longest 0.8 ms); {worker} compiled on worker threads in 3.4 ms; '
                f'sync compiles {sync} in 5.6 ms; prewarm waits 0 in 0.0 ms')

    def test_compile_counts_are_frame_keyed_and_missing_is_not_zero(self):
        frame, record = provenance.compile_record(self.line(600, 2, 3, 4))
        self.assertEqual(frame, 600)
        self.assertEqual((record['pipeline_sync'], record['pipeline_async'], record['pipeline_creations']), (2, 3, 4))
        self.assertEqual(record['pipeline_sync_ms'], 5.6)
        self.assertTrue(record['compiles_present'])
        self.assertIsNone(record['shader_compiles'])
        self.assertFalse(provenance.compile_record(self.line(900))[1]['compiles_present'])
        legacy = provenance.compile_record('[nvn-vk] frame 300, stutter: longest frame 40.0 ms')[1]
        self.assertIsNone(legacy['compiles_present'])
        self.assertIsNone(provenance.compilation_of(1200, {})['pipeline_sync'])
        self.assertIsNone(provenance.compile_record('COMPILE 2 5.6MS'))  # not a 300-frame log counter

    def test_analyzer_retains_compile_windows_and_scene_unavailability(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'logs').mkdir()
            (d / 'logs/thor.log').write_text(ab_log({600: (40, 30, 10, 0), 900: (40, 30, 10, 0)}, []) +
                                            self.line(600, 2, 3, 4) + '\n' + self.line(900) + '\n')
            (d / 'pans.txt').write_text('thor hateno F0=0 F1=900 T0=100 T1=130 V=16000 D=1\n')
            (d / 'warmups.jsonl').write_text(json.dumps({'device': 'thor', 'scene': 'hateno',
                'status': 'timeout', 'elapsed_s': 30, 'observations': []}) + '\n')
            result = run(BENCH / 'analyze.py', d, '--json', d / 'result.json')
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads((d / 'result.json').read_text())['runs'][0]
            self.assertEqual(len(record['windows']), 2)
            self.assertEqual(record['summary']['compile_windows'], 1)
            self.assertEqual(record['pans'][0]['summary']['pipeline_async_total'], 3)
            self.assertEqual(record['summary']['compile_covered_windows'], 2)
            self.assertEqual(record['scene_state']['status'], 'unavailable')
            self.assertIsNone(record['scene_state']['weather'])
            self.assertEqual(record['warmups'][0]['status'], 'timeout')
            self.assertIn('COMPILES', result.stdout)
            self.assertIn('retained in every summary', result.stdout)
            self.assertIn('optional graphics warm-up: timeout', result.stdout)

    def wait(self, snapshots, timeout=15):
        ticks = [0]
        snapshots = iter(snapshots)
        latest = ['']
        def read_log(remaining):
            latest[0] = next(snapshots, latest[0])
            return latest[0]
        return warmup.wait_for_calm(read_log, 2, timeout, interval=3, clock=lambda: ticks[0],
                                    sleep=lambda seconds: ticks.__setitem__(0, ticks[0] + seconds))

    def test_warmup_needs_fresh_consecutive_known_zero_windows(self):
        rows = [self.line(300), self.line(600, sync=1), self.line(900), self.line(1200)]
        result = self.wait(['\n'.join(rows[:n]) for n in range(1, 5)])
        self.assertEqual(result['status'], 'calm')
        self.assertEqual(result['elapsed_s'], 9)
        self.assertEqual([r['frame'] for r in result['observations']], [600, 900, 1200])
        self.assertEqual(result['observations'][0]['pipeline_sync'], 1)
        for snapshots in ([self.line(300)], [''], [self.line(300), self.line(900), self.line(1500)],
                          [self.line(300), '[nvn-vk] frame 600, stutter: longest frame 40.0 ms']):
            with self.subTest(snapshots=snapshots):
                result = self.wait(snapshots)
                self.assertEqual(result['status'], 'timeout')
                self.assertEqual(result['elapsed_s'], 15)

    def test_warmup_counter_reset_and_read_failure_are_unavailable(self):
        self.assertEqual(self.wait([self.line(900), self.line(300)])['status'], 'unavailable')
        def fail(remaining):
            raise OSError('no log')
        result = warmup.wait_for_calm(fail, 2, 15)
        self.assertEqual(result['status'], 'unavailable')
        self.assertIn('no log', result['reason'])


if __name__ == '__main__':
    unittest.main()

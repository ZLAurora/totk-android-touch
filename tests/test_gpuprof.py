"""GPU profile schema/analysis fixtures; optional real lavapipe matrix, no devices.

Pure suite: python -m unittest discover -s tests -p test_gpuprof.py
Real matrix (build vkgpuproftest first): GPUPROF_TEST_BIN=<absolute binary>
TOTK_SHADER_SEED_DIR=<absolute vkdraw-seed> python -m unittest discover -s tests -p test_gpuprof.py
The caller runs the matrix under the repository's taskset/nice/flock policy.
"""
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/bench'))
import gpuprof  # noqa: E402


def attachment(width=1280, height=720, reference_width=1280, reference_height=720, **changes):
    width_gcd, height_gcd = math.gcd(width, reference_width), math.gcd(height, reference_height)
    row = dict(slot=0, format=37, width=width, height=height,
               width_num=width // width_gcd, width_den=reference_width // width_gcd,
               height_num=height // height_gcd, height_den=reference_height // height_gcd,
               load=1, store=0)
    row.update(changes)
    return row


def counters(vertices=30, primitives=10, fragments=100):
    return dict(ia_vertices=vertices, ia_primitives=primitives, vs_invocations=vertices,
                clipping_invocations=primitives, clipping_primitives=primitives, fs_invocations=fragments)


def pass_row(**changes):
    row: dict = dict(schema=3, type='pass', window_start=1, window_end=300, key='a', kind='render', ordinal=1,
               gpu_ms=10.0, count=2, draws=10, stats_valid=2, samples=1, scene_relative=False,
               render_width=1280, render_height=720, reference_width=1280, reference_height=720,
               shader='shader-a', stats=counters())
    row.update(changes)
    if row['kind'] != 'render' and 'render_width' not in changes and 'render_height' not in changes:
        row.update(render_width=0, render_height=0)
    if 'attachments' not in changes:
        row['attachments'] = [attachment(width=row['render_width'], height=row['render_height'],
                                         reference_width=row['reference_width'],
                                         reference_height=row['reference_height'])]
    return row


def scene_row(width, height, reference_width=None, reference_height=None, depth_only=False, **changes):
    reference_width = width if reference_width is None else reference_width
    reference_height = height if reference_height is None else reference_height
    attachments = [attachment(width, height, reference_width, reference_height, slot=slot,
                              format=126 if slot == 8 else 37)
                   for slot in ((8,) if depth_only else (0, 1, 8))]
    changes.setdefault('scene_relative', True)
    return pass_row(render_width=width, render_height=height, reference_width=reference_width,
                    reference_height=reference_height, attachments=attachments, **changes)


def window(**changes):
    row = dict(schema=3, type='window', window_start=1, window_end=300, frames=300, partial=False,
               span_ms=12.0, measured_ms=10.0, gap_ms=2.0, dropped=0, failed_slots=0, stats_failed_slots=0)
    row.update(changes)
    return row


def frame(**changes):
    row = dict(schema=3, type='frame', frame=1, span_ms=12.0, measured_ms=10.0, gap_ms=2.0,
               dropped=0, reconciliation_failed=False, scene_width=0, scene_height=0)
    row.update(changes)
    return row


def header(**changes):
    row = dict(schema=3, type='header', launch_id='fixture', rotation=0, profile=1, stats=False,
               ordinals=False, max_bytes=134217728, retained_files=4, window_frames=300, span_limit_ms=10000,
               build_commit=None, package=None, version_code=None, driver={}, env={})
    row.update(changes)
    return row


def session_end(**changes):
    row = dict(schema=3, type='session_end', launch_id='fixture', clean_end=True, all_queries_retired=None,
               last_frame=300, last_window_sequence=1, last_window_end=300, output_failures=0)
    row.update(changes)
    return row


def query_retirement(**changes):
    row = dict(leases_acquired=5, leases_submitted=3, leases_retired=3, leases_cancelled=2,
               leases_outstanding=0, queries_submitted=12, queries_retired=12, queries_cancelled=6,
               queries_outstanding=0)
    row.update(changes)
    return row


def sampled_pass(durations=(0.001024, 0.004096), *, observed_count=20, observed_frames=20,
                 sampled_frames=None, budget=2, stats=False, **changes):
    histogram = [0] * 272
    for duration in durations:
        index = max(0, min(271, math.ceil(math.log2(duration * 1e6) * 8))) if duration else 0
        histogram[index] += 1
    count = len(durations)
    row = pass_row(ordinal=0, count=observed_count, draws=observed_count * 2, gpu_ms=sum(durations),
                   stats_valid=count if stats else 0, sampling=dict(
                       budget=budget, observed_count=observed_count, sample_count=count,
                       observed_frames=observed_frames,
                       sampled_frames=count if sampled_frames is None else sampled_frames,
                       histogram=histogram))
    if stats and count:
        row.update(stats=counters(count * 6, count * 2, count * 20),
                   stats_draws=count * 2, stats_gpu_ms=sum(durations))
    else:
        row.pop('stats')
    row.update(changes)
    return row


def sampled_frame(**changes):
    row = frame(sample_budget=2, observed_passes=4, sampled_passes=1, timestamp_count=4,
                timing_scope='main-frame')
    row.update(changes)
    return row


def sampled_window(**changes):
    row: dict = window(sample_budget=2, observed_passes=20, sample_count=2, timestamp_count=604,
                       timing_scope='main-frame')
    row.update(changes)
    if 'measured_ms' in changes and 'gap_ms' not in changes and row['span_ms'] is not None:
        row['gap_ms'] = row['span_ms'] - row['measured_ms']
    return row


def pipeline_row(**changes):
    row = dict(schema=3, type='pipeline', frame=1, group=0, pass_ordinal=1, pass_key='a' * 16,
               kind='render', pipeline='1' * 16, pipeline_identity='launch-local-vulkan-handle',
               vs='0123456789abcdef_vs_d16', fs='fedcba9876543210_fs_alpha7', cs=None,
               draws=3, dispatches=0, known_primitives=3, unknown_primitive_draws=0,
               primitives=3, primitive_reason=None, selected=True, status='ok', gpu_ms=0.25,
               timestamp_count=2, selection_numerator=2, selection_denominator=2048)
    row.update(changes)
    return row


def pipeline_coverage(**changes):
    row = dict(schema=3, type='pipeline_coverage', frame=1, lease=1, slot_sequence=1,
               observed_groups=1, dropped_groups=0, selected_groups=1, timestamp_count=2, discarded=False,
               unretained_groups=0, out_of_range_groups=0, population=2048, phase=0)
    row.update(changes)
    return row


def compact_header(**changes):
    return header(pipeline_budget=4, pipeline_population=2048, pipeline_phase=17,
                  pipeline_random_phase=True, pipeline_output='compact', pipeline_compact_version=1,
                  pipeline_window_columns=list(gpuprof.PIPELINE_WINDOW_COLUMNS), **changes)


def compact_runs(*, frames=1207, losses=False, truth=False):
    """Serialize one logical run twice; never handmatch independent expected totals.

    The 4096-key pool has full variants and launch-local handles. Sparse raw rows
    retain selected work and one optional unselected observation of a selected key;
    coverage explicitly accounts for all other unselected metadata omissions.
    Truth mode changes active sets only at complete 512-frame N/budget rotation
    blocks, crossing 300-frame output boundaries. Each active key then has a
    fixed known cost in every frame; estimator truth is independent of encoding.
    """
    pool = []
    for key in range(4096):
        compute = key % 7 == 0
        pool.append(dict(schema=3, type='pipeline_key', id=key + 1,
                         pipeline=f'{key + 1:016x}', kind='compute' if compute else 'render',
                         vs=None if compute else f'{key % 512:016x}_vs_full_variant_d16_s8',
                         fs=None if compute else f'{key % 193:016x}_fs_full_variant_alpha7',
                         cs=f'{key % 97:016x}_cs_full_variant_8x8' if compute else None))
    raw: list[dict] = [header(pipeline_budget=4, pipeline_population=2048, pipeline_phase=17,
                  pipeline_random_phase=True, pipeline_output='raw')]
    compact: list[dict] = [compact_header()]
    for start in range(1, frames + 1, 300):
        end = min(frames, start + 299)
        groups, exposures = {}, {}
        cov: dict = dict(schema=3, type='pipeline_window_coverage', window_start=start, window_end=end,
                   frames=end - start + 1, eligible_frames=0, domain_excluded_frames=0,
                   observed_groups=0, selected_groups=0, valid_samples=0, missing_selected=0,
                   timestamp_count=0, dropped_groups=0, unretained_groups=0, out_of_range_groups=0,
                   discarded_groups=0, counts_complete=False, aggregation_cpu_ms=0.5,
                   aggregation_flush_cpu_ms=0.1, serialized_bytes=1234, worker_peak_backlog=2,
                   aggregation_memory_bytes=8192, **dict.fromkeys(gpuprof.PIPELINE_COMPACT_LOSS, 0))
        for f in range(start, end + 1):
            population = 2048 if truth or f <= 101 else 4096
            observed = 2048 if truth else 1600 if f <= 100 else 3072 if f == 101 else 4096
            phase = (17 + (f - 1) * 4) % population
            fps = 20 + (f * 13) % 41
            discarded = losses and f == 131
            dropped = int(losses and f == 151)
            outside = max(0, observed - population)
            eligible = not (outside or dropped or discarded)
            selected_ordinals = [(phase + i) % population for i in range(4)
                                 if (phase + i) % population < observed]
            logical = []
            for i, ordinal in enumerate(selected_ordinals):
                key = ((ordinal + ((f - 1) // 512) * 2048) if truth else
                       ordinal + ((f - 1) // 300) * 769 + f % fps) % len(pool)
                identity = pool[key]
                compute = identity['kind'] == 'compute'
                unknown = not compute and key % 11 == 0
                status = 'discarded' if discarded else (
                    'unavailable-timestamp' if losses and f == 23 and i == 0 else
                    'query-failed' if losses and f == 41 and i == 0 else
                    'implausible-timestamp' if losses and f == 83 and i == 0 else 'ok')
                duration = (0.01 + (key % 19) * 0.001) if truth else (key % 13 + 1) * 0.0001 * fps / 20
                row = pipeline_row(frame=f, group=ordinal, kind=identity['kind'], pipeline=identity['pipeline'],
                                   vs=identity['vs'], fs=identity['fs'], cs=identity['cs'],
                                   selected=True, selection_numerator=4, selection_denominator=population,
                                   status=status, gpu_ms=duration if status == 'ok' else None,
                                   draws=0 if compute else 3, dispatches=int(compute),
                                   known_primitives=0 if compute or unknown else 3,
                                   unknown_primitive_draws=3 if unknown else 0,
                                   primitives=None if compute or unknown else 3,
                                   primitive_reason='not-graphics' if compute else 'unknown' if unknown else None)
                logical.append((key, row))
            if logical and not discarded and not truth:
                key, row = logical[0]
                optional = dict(row, group=(phase + 4) % population, selected=False,
                                status='not-selected', gpu_ms=None, timestamp_count=0)
                logical.append((key, optional))
            selected = sum(r['selected'] for _, r in logical)
            stamps = sum(r['timestamp_count'] for _, r in logical)
            omitted = observed - len(logical) - dropped
            raw.append(pipeline_coverage(frame=f, lease=f, slot_sequence=f, observed_groups=observed,
                                         selected_groups=selected, timestamp_count=stamps, discarded=discarded,
                                         dropped_groups=dropped, unretained_groups=omitted,
                                         out_of_range_groups=outside, population=population, phase=phase))
            raw.extend(r for _, r in logical)
            exposure = exposures.setdefault(population, [population, 4, population, 0, 0, 0, 0])
            exposure[3] += 1
            exposure[4] += eligible
            cov['eligible_frames'] += eligible
            cov['domain_excluded_frames'] += not eligible
            if not discarded:
                exposure[5] += observed
                exposure[6] += selected
                for k, value in (('observed_groups', observed), ('selected_groups', selected),
                                 ('timestamp_count', stamps), ('unretained_groups', omitted),
                                 ('out_of_range_groups', outside), ('dropped_groups', dropped)):
                    cov[k] += value
            for key, row in logical:
                c = groups.setdefault(key, dict.fromkeys(gpuprof.PIPELINE_WINDOW_COLUMNS, 0))
                if discarded:
                    c['discarded_groups'] += 1
                    cov['discarded_groups'] += 1
                    continue
                c['observed_groups'] += 1
                c['selected_groups'] += row['selected']
                for k in ('draws', 'dispatches', 'known_primitives', 'unknown_primitive_draws'):
                    c[k] += row[k]
                if row['status'] == 'ok':
                    duration = row['gpu_ms']
                    weighted = duration * population / 4
                    c['valid_samples'] += 1
                    cov['valid_samples'] += 1
                    c['gpu_ms_sum'] += duration
                    c['gpu_ms_square_sum'] += duration ** 2
                    c['weighted_gpu_ms_sum'] += weighted
                    c['sampled_draws'] += row['draws']
                    c['sampled_dispatches'] += row['dispatches']
                    if eligible:
                        c['eligible_samples'] += 1
                        c['eligible_gpu_ms_sum'] += duration
                        c['eligible_weighted_gpu_ms_sum'] += weighted
                        c['eligible_weighted_gpu_ms_square_sum'] += weighted ** 2
                elif row['selected']:
                    c['missing_selected'] += 1
                    c['eligible_missing_selected'] += eligible
                    cov['missing_selected'] += 1
                    loss_key = {'unavailable-timestamp': 'unavailable_timestamps',
                                'query-failed': 'query_failures', 'implausible-timestamp': 'invalid_timestamps'}[row['status']]
                    cov[loss_key] += 1
        for key, c in sorted(groups.items()):
            compact.append(pool[key])
            compact.append(dict(schema=3, type='pipeline_window', window_start=start, window_end=end,
                                key_id=key + 1, v=[c[k] for k in gpuprof.PIPELINE_WINDOW_COLUMNS]))
        cov['population_exposure'] = list(exposures.values())
        compact.append(cov)
        parent = window(window_start=start, window_end=end, frames=end - start + 1,
                        partial=end - start + 1 < 300, sequence=(start - 1) // 300 + 1,
                        measured_ms=0, span_ms=0, gap_ms=0)
        raw.append(parent)
        compact.append(parent)
    footer = session_end(last_frame=frames, last_window_end=frames, last_window_sequence=(frames - 1) // 300 + 1)
    return [*raw, footer], [*compact, footer]


class Analyzer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def file(self, name, rows, tail=''):
        path = self.root / name
        path.write_text(''.join(json.dumps(row) + '\n' for row in rows) + tail, encoding='utf-8')
        return path

    def analyze(self, rows):
        return gpuprof.analyze([self.file('gpuprof-fixture.jsonl', rows)])

    def pipeline_analysis(self, rows, coverage=None, **header_changes):
        return self.analyze([header(pipeline_budget=2, pipeline_population=2048,
                                    pipeline_phase=0, pipeline_random_phase=True, **header_changes),
                             *(coverage if coverage is not None else [
                                 pipeline_coverage(observed_groups=len(rows),
                                                   selected_groups=sum(r['selected'] for r in rows),
                                                   timestamp_count=sum(r['timestamp_count'] for r in rows))]),
                             *rows, session_end(last_frame=max((r['frame'] for r in rows), default=0),
                                                last_window_sequence=0, last_window_end=0)])['pipelines']

    def test_pipeline_weights_do_not_apportion_pass_draws(self):
        rows = [pipeline_row(), pipeline_row(group=1, pipeline='2' * 16, draws=9,
                                            known_primitives=9, primitives=9, gpu_ms=0.75)]
        report = self.pipeline_analysis(rows)
        self.assertEqual(len(report['pipelines']), 2)
        self.assertEqual(len(report['shader_pairs']), 1)
        pair = report['shader_pairs'][0]
        self.assertEqual(pair['gpu_ms'], 1)
        self.assertEqual(pair['estimated_gpu_ms'], 1024)
        self.assertEqual(pair['draws'], 12)
        self.assertTrue(pair['thin'])
        self.assertEqual(pair['vs'], rows[0]['vs'])
        self.assertEqual(pair['fs'], rows[0]['fs'])
        self.assertNotIn('pipelines', self.analyze([pass_row()]),
                         'first-draw pass shader/draws cannot reconstruct pipeline work')

    def test_pipeline_unsampled_missing_and_unknown_are_not_zero_cost(self):
        unsampled = pipeline_row(group=2, selected=False, status='not-selected', gpu_ms=None, timestamp_count=0)
        group = self.pipeline_analysis([unsampled])['shader_pairs'][0]
        self.assertIsNone(group['estimated_gpu_ms'])
        missing = pipeline_row(status='unavailable-timestamp', gpu_ms=None)
        group = self.pipeline_analysis([missing])['shader_pairs'][0]
        self.assertEqual(group['missing_selected'], 1)
        self.assertIsNone(group['estimated_gpu_ms'])
        unknown = pipeline_row(primitives=None, unknown_primitive_draws=3, known_primitives=0,
                               primitive_reason='indexed-restart-or-unknown-count')
        group = self.pipeline_analysis([unknown])['shader_pairs'][0]
        self.assertIsNone(group['primitives'])
        self.assertEqual(group['unknown_primitive_draws'], 3)
        for changes in (dict(status='not-selected', gpu_ms=0, selected=False, timestamp_count=0),
                        dict(selected=False), dict(timestamp_count=3), dict(selection_numerator=17),
                        dict(primitives=0, unknown_primitive_draws=1),
                        dict(kind='compute'), dict(group=2048)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                gpuprof.validate(pipeline_row(**changes))

    def test_pipeline_compute_and_censored_overflow(self):
        compute = pipeline_row(kind='compute', vs=None, fs=None, cs='compute_variant_8x8',
                               draws=0, dispatches=1, primitives=None, known_primitives=0,
                               primitive_reason='not-graphics')
        self.assertEqual(gpuprof.validate(compute)['dispatches'], 1)
        report = self.pipeline_analysis([pipeline_row()], coverage=[
            pipeline_coverage(observed_groups=2, dropped_groups=1)])
        self.assertEqual(report['coverage']['dropped_groups'], 1)
        self.assertIsNone(report['shader_pairs'][0]['estimated_gpu_ms'])
        report = self.pipeline_analysis([pipeline_row(status='discarded', gpu_ms=None)], coverage=[
            pipeline_coverage(discarded=True)])
        self.assertEqual(report['shader_pairs'][0]['discarded_groups'], 1)
        self.assertIsNone(report['shader_pairs'][0]['estimated_gpu_ms'])

    def test_pipeline_selection_authority_and_local_handles(self):
        report = self.pipeline_analysis([pipeline_row(group=2)])
        self.assertGreater(report['coverage']['invalid_coverage'], 0)
        self.assertIsNone(report['shader_pairs'][0]['estimated_gpu_ms'])
        tail = pipeline_row(group=2048, selection_numerator=0, selected=False,
                            status='out-of-range', gpu_ms=None, timestamp_count=0)
        self.assertIsNone(self.pipeline_analysis([tail])['shader_pairs'][0]['estimated_gpu_ms'])
        report = self.pipeline_analysis([pipeline_row()], coverage=[])
        self.assertGreater(report['coverage']['invalid_coverage'], 0)
        self.assertIsNone(report['shader_pairs'][0]['estimated_gpu_ms'])

    def test_pipeline_tail_domain_growth_preserves_selection_weighting(self):
        rows = [header(pipeline_budget=2, pipeline_population=2048, pipeline_phase=2998, pipeline_random_phase=True),
                pipeline_coverage(observed_groups=4000, unretained_groups=3998, out_of_range_groups=1952,
                                  phase=950, selected_groups=2, timestamp_count=4),
                pipeline_row(group=950), pipeline_row(group=951),
                pipeline_coverage(frame=2, lease=2, slot_sequence=2, observed_groups=4000,
                                  unretained_groups=3998, population=4096, phase=3000,
                                  selected_groups=2, timestamp_count=4),
                pipeline_row(frame=2, group=3000, vs='tail_variant', selection_denominator=4096),
                pipeline_row(frame=2, group=3001, vs='tail_variant', selection_denominator=4096),
                session_end(last_frame=2, last_window_sequence=0, last_window_end=0)]
        report = self.analyze(rows)['pipelines']
        self.assertEqual(report['coverage']['invalid_coverage'], 0)
        self.assertEqual(report['eligible_frames'], 1)
        self.assertEqual(report['domain_excluded_frames'][0]['frame'], 1)
        tail = next(pair for pair in report['shader_pairs'] if pair['vs'] == 'tail_variant')
        self.assertIsNone(tail['estimated_gpu_ms'], 'growing first frame is not extrapolated')
        self.assertEqual(tail['eligible_estimated_gpu_ms'], 1024, 'tail is rankable over the named fully covered population')
        self.assertFalse(tail['counts_complete'])

    def compare_pipeline_runs(self, raw_rows, compact_rows, *, rotate=False):
        def analyze_rows(rows, name):
            if not rotate:
                return self.analyze(rows)
            chunks, chunk = [], []
            for row in rows[1:]:
                chunk.append(row)
                if row['type'] == 'window':
                    chunks.append(chunk)
                    chunk = []
            chunks[-1].extend(chunk)
            self.assertLessEqual(len(chunks), 4, 'truth run retains every rotated file')
            paths = []
            for rotation, chunk in enumerate(chunks):
                suffix = len(chunks) - rotation - 1
                filename = f'{name}.jsonl' + (f'.{suffix}' if suffix else '')
                paths.append(self.file(filename, [dict(rows[0], rotation=rotation), *chunk]))
            return gpuprof.analyze(paths)
        raw = analyze_rows(raw_rows, 'raw-equivalence')
        compact = analyze_rows(compact_rows, 'compact-equivalence')
        for report in (raw, compact):
            self.assertEqual(report['coverage']['malformed_rows'], 0, report['issues'])
            self.assertEqual(report['pipelines']['coverage']['invalid_coverage'], 0, report['issues'])
        a, b = raw['pipelines'], compact['pipelines']
        for key in ('eligible_frames', 'domain_excluded_frame_count', 'coverage_complete', 'censored'):
            self.assertEqual(a[key], b[key], key)
        for key in ('observed_groups', 'selected_groups', 'valid_samples', 'missing_selected',
                    'timestamp_count', 'dropped_groups', 'unretained_groups', 'out_of_range_groups', 'discarded_groups'):
            self.assertEqual(a['coverage'][key], b['coverage'][key], key)
        counters = ('groups', 'selected_groups', 'samples', 'missing_selected', 'out_of_range', 'discarded_groups',
                    'eligible_samples', 'eligible_missing_selected', 'draws', 'dispatches', 'known_primitives',
                    'unknown_primitive_draws', 'sampled_draws', 'sampled_dispatches', 'primitives',
                    'counts_complete', 'coverage_complete', 'thin')
        moments = ('gpu_ms', 'weighted_gpu_ms', 'eligible_gpu_ms', 'gpu_ms_square_sum',
                   'eligible_weighted_gpu_ms', 'eligible_weighted_gpu_ms_square_sum',
                   'estimated_gpu_ms', 'eligible_estimated_gpu_ms', 'mean_group_ms')
        for view in ('pipelines', 'shader_pairs'):
            def identity(g):
                return (g['kind'], g['vs'], g['fs'], g['cs'],
                        tuple(i['pipeline'] for i in g['identities']) if view == 'pipelines' else ())
            left, right = ({identity(g): g for g in report[view]} for report in (a, b))
            self.assertEqual(left.keys(), right.keys(), view)
            for key, g in left.items():
                h = right[key]
                for field in counters:
                    self.assertEqual(g[field], h[field], (view, key, field))
                for field in moments:
                    if g[field] is None:
                        self.assertIsNone(h[field], (view, key, field))
                    else:
                        self.assertTrue(math.isclose(g[field], h[field], rel_tol=1e-9, abs_tol=1e-9),
                                        (view, key, field, g[field], h[field]))
                self.assertIsNone(h['pass_keys'], 'compact output cannot reconstruct pass keys')
        return raw, compact

    def test_pipeline_compact_and_raw_recover_known_truth_across_churn_blocks(self):
        reports = self.compare_pipeline_runs(*compact_runs(frames=1024, truth=True), rotate=True)
        truth = {f'{key + 1:016x}': 512 * (0.01 + (key % 19) * 0.001) for key in range(4096)}
        for report in reports:
            self.assertEqual(len(report['files']), 4)
            self.assertEqual(report['coverage']['rotation_evictions'], 0)
            pipeline = report['pipelines']
            self.assertFalse(pipeline['censored'], pipeline['censor_reasons'])
            self.assertTrue(pipeline['coverage_complete'])
            self.assertEqual(len(pipeline['pipelines']), len(truth))
            for view in ('pipelines', 'shader_pairs'):
                estimated_total = 0
                for group in pipeline[view]:
                    expected = sum(truth[i['pipeline']] for i in group['identities'])
                    for field in ('estimated_gpu_ms', 'eligible_estimated_gpu_ms'):
                        self.assertTrue(math.isclose(group[field], expected, rel_tol=1e-10, abs_tol=1e-10),
                                        (view, field, group['identities'], group[field], expected))
                    estimated_total += group['estimated_gpu_ms']
                self.assertTrue(math.isclose(estimated_total, sum(truth.values()), rel_tol=1e-10, abs_tol=1e-8),
                                'known fixed-key truth, not merely raw/compact agreement')

    def test_pipeline_rotation_prefix_unselected_loss_has_explicit_reason(self):
        # Retained P1 shape: rotation20 begins mid-lease with 63/70 optional
        # rows, then intact86+11. No selected/query loss explains the seven
        # absent rows; independent rotation/backlog authority still censors.
        rows: list[dict] = [header(pipeline_budget=2, pipeline_population=2048, pipeline_phase=0,
                       pipeline_random_phase=True, rotation=rotation) for rotation in range(20, 24)]
        f = 5296
        phase = ((f - 1) * 2) % 2048
        for lease, first, count, observed in ((63799, 1194, 63, 70), (63800, 1257, 86, 86),
                                             (63801, 1343, 11, 11)):
            rows.extend(pipeline_row(frame=f, group=g, selected=False, status='not-selected',
                                     gpu_ms=None, timestamp_count=0) for g in range(first, first + count))
            rows.append(pipeline_coverage(frame=f, lease=lease, slot_sequence=lease, observed_groups=observed,
                                           selected_groups=0, timestamp_count=0, phase=phase))
        for f in range(5297, 5434):
            phase = ((f - 1) * 2) % 2048
            rows.extend([pipeline_row(frame=f, group=phase),
                         pipeline_coverage(frame=f, lease=f, slot_sequence=f, observed_groups=phase + 1,
                                           unretained_groups=phase, phase=phase)])
        rows.extend([window(window_start=5101, window_end=5400, sequence=18, measured_ms=0,
                            span_ms=0, gap_ms=0, background_dropped_slots=73),
                     window(window_start=5401, window_end=5433, sequence=19, frames=33, partial=True,
                            measured_ms=0, span_ms=0, gap_ms=0, background_dropped_slots=73),
                     session_end(last_frame=5433, last_window_end=5433, last_window_sequence=19,
                                 background_dropped_slots=73)])
        report = self.analyze(rows)
        pipeline = report['pipelines']
        self.assertEqual(pipeline['coverage']['invalid_coverage'], 1)
        self.assertEqual(pipeline['coverage']['missing_selected'], 0)
        self.assertEqual(report['coverage']['rotation_evictions'], 20)
        self.assertEqual(report['coverage']['missing_windows'], 17)
        self.assertEqual(report['coverage']['background_dropped_slots'], 73)
        reason, = pipeline['invalid_reasons']
        self.assertEqual(reason['reason'], 'retained-row-count-mismatch')
        self.assertEqual(reason['frame'], 5296)
        self.assertEqual(reason['expected_retained_rows'] - reason['retained_rows'], 7)
        self.assertTrue({'rotation_evictions', 'missing_windows', 'background_dropped_slots', 'invalid_coverage'}
                        <= set(pipeline['censor_reasons']))
        group, = pipeline['shader_pairs']
        self.assertEqual(group['samples'], 137)
        self.assertIsNone(group['estimated_gpu_ms'])
        self.assertIsNone(group['eligible_estimated_gpu_ms'])

    def test_pipeline_compact_same_run_churn_growth_and_partial_window(self):
        raw, compact = self.compare_pipeline_runs(*compact_runs())
        self.assertEqual(compact['coverage']['frame_rows'], 0)
        self.assertEqual(compact['coverage']['missing_frame_ids'], 0, 'advertised omission is not capture loss')
        self.assertIsNone(compact['pipelines']['domain_excluded_frames'])
        self.assertEqual(compact['pipelines']['domain_excluded_windows'][0]['frames'], 1)
        self.assertTrue(any(g['eligible_estimated_gpu_ms'] is not None for g in compact['pipelines']['pipelines']))
        self.assertTrue(all(g['estimated_gpu_ms'] is None for g in compact['pipelines']['pipelines']))
        self.assertEqual(compact['sessions'][0]['end']['last_frame'], 1207)
        self.assertEqual(raw['pipelines']['eligible_frames'], 1206)

    def test_pipeline_compact_same_run_true_loss_and_discard(self):
        _, compact = self.compare_pipeline_runs(*compact_runs(losses=True))
        self.assertTrue(compact['pipelines']['censored'])
        self.assertEqual(compact['pipelines']['coverage']['missing_selected'], 3)
        self.assertGreater(compact['pipelines']['coverage']['discarded_groups'], 0)
        self.assertTrue(all(g['eligible_estimated_gpu_ms'] is None for g in compact['pipelines']['pipelines']))

    def test_pipeline_compact_same_run_rotation_eviction(self):
        raw_rows, compact_rows = compact_runs(frames=307)
        for rows in (raw_rows, compact_rows):
            rows[0] = dict(rows[0], rotation=4)
        raw, compact = self.compare_pipeline_runs(raw_rows, compact_rows)
        self.assertGreater(compact['coverage']['rotation_evictions'], 0)
        self.assertTrue(raw['pipelines']['censored'])
        self.assertTrue(all(g['eligible_estimated_gpu_ms'] is None for g in compact['pipelines']['shader_pairs']))

    def test_missing_writer_end_censors_raw_and_compact_even_without_other_loss(self):
        for rows in compact_runs(frames=1024, truth=True):
            with self.subTest(output=rows[0].get('pipeline_output', 'raw')):
                complete = self.analyze(rows)['pipelines']
                self.assertFalse(complete['censored'])
                self.assertTrue(any(g['eligible_estimated_gpu_ms'] is not None for g in complete['pipelines']))
                for tail in ('', '{"schema":3,"type":"session_end"'):
                    path = self.file('unfinished.jsonl', rows[:-1], tail=tail)
                    report = gpuprof.analyze([path])
                    self.assertEqual(report['coverage']['streams_without_end'], 1)
                    pipeline = report['pipelines']
                    self.assertTrue(pipeline['censored'])
                    self.assertFalse(pipeline['coverage_complete'])
                    self.assertIn('streams_without_end', pipeline['censor_reasons'])
                    self.assertTrue(all(g['estimated_gpu_ms'] is None and g['eligible_estimated_gpu_ms'] is None
                                        and not g['counts_complete'] for g in pipeline['pipelines']))

    def test_sink_overflow_is_independent_fail_closed_authority(self):
        for rows in compact_runs(frames=1024, truth=True):
            changed = [dict(r, output_sink_overflows=1) if r['type'] == 'session_end' else r for r in rows]
            report = self.analyze(changed)
            self.assertEqual(report['coverage']['output_failures'], 0)
            self.assertEqual(report['coverage']['output_sink_overflows'], 1)
            self.assertIn('output_sink_overflows', report['pipelines']['censor_reasons'])
            self.assertTrue(all(g['eligible_estimated_gpu_ms'] is None and not g['counts_complete']
                                for g in report['pipelines']['pipelines']))
        for value in (-1, True, 0.5):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gpuprof.validate(session_end(output_sink_overflows=value))

    def test_pipeline_compact_strict_numeric_columns_and_exposure(self):
        _, rows = compact_runs(frames=1)
        summary = next(r for r in rows if r['type'] == 'pipeline_window')
        coverage = next(r for r in rows if r['type'] == 'pipeline_window_coverage')
        for values in (summary['v'][:-1], [*summary['v'], 0]):
            with self.subTest(values=len(values)), self.assertRaises(ValueError):
                gpuprof.validate(dict(summary, v=values))
        for column, value in (('valid_samples', 100), ('missing_selected', 100),
                              ('eligible_samples', 100), ('gpu_ms_sum', math.inf),
                              ('gpu_ms_square_sum', 0), ('eligible_weighted_gpu_ms_sum', math.nan),
                              ('sampled_draws', -1), ('draws', True)):
            values = list(summary['v'])
            values[gpuprof.PIPELINE_WINDOW_COLUMNS.index(column)] = value
            with self.subTest(column=column), self.assertRaises(ValueError):
                gpuprof.validate(dict(summary, v=values))
        for changes in (dict(population_exposure=[[2048, 4, 4096, 1, 1, 1600, 4]]),
                        dict(population_exposure=[]), dict(counts_complete=True),
                        dict(eligible_frames=2), dict(timestamp_count=0),
                        dict(aggregation_cpu_ms=math.inf), dict(frames=2)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                gpuprof.validate(dict(coverage, **changes))
        bad = dict(compact_header(), pipeline_window_columns=list(reversed(gpuprof.PIPELINE_WINDOW_COLUMNS)))
        with self.assertRaises(ValueError):
            gpuprof.validate(bad)

    def test_pipeline_compact_dictionary_authority_and_overflow_are_not_estimates(self):
        _, rows = compact_runs(frames=2)
        summary = next(r for r in rows if r['type'] == 'pipeline_window')
        key = next(r for r in rows if r['type'] == 'pipeline_key' and r['id'] == summary['key_id'])
        no_key = [r for r in rows if r is not key]
        late_key = [r for r in no_key if r is not summary] + [summary, key]
        for broken in (no_key, late_key):
            with self.subTest(authority='missing' if broken is no_key else 'late'):
                report = self.analyze(broken)['pipelines']
                self.assertGreater(report['coverage']['missing_dictionary'], 0)
                self.assertTrue(all(g['eligible_estimated_gpu_ms'] is None for g in report['pipelines']))
        for field in gpuprof.PIPELINE_COMPACT_LOSS:
            changed = [dict(r, **{field: 1}, counts_complete=False)
                       if r['type'] == 'pipeline_window_coverage' else r for r in rows]
            with self.subTest(loss=field):
                report = self.analyze(changed)['pipelines']
                self.assertTrue(report['censored'])
                self.assertTrue(all(g['estimated_gpu_ms'] is None and g['eligible_estimated_gpu_ms'] is None
                                    for g in report['pipelines']))
        missing_coverage = self.analyze([r for r in rows if r['type'] != 'pipeline_window_coverage'])['pipelines']
        self.assertGreater(missing_coverage['coverage']['missing_compact_windows'], 0)
        first = self.file('dict-rotation.jsonl.1', [rows[0], key])
        second = self.file('dict-rotation.jsonl', [dict(rows[0], rotation=1), *no_key[1:]])
        report = gpuprof.analyze([first, second])['pipelines']
        self.assertGreater(report['coverage']['missing_dictionary'], 0,
                           'dictionary from a different file cannot authorize a rotation-local reference')
        conflict = self.analyze([*rows, dict(key, vs='different_full_variant')])
        self.assertGreater(conflict['coverage']['conflicting_rows'], 0)
        self.assertTrue(conflict['pipelines']['censored'])

    def test_pipeline_unfinished_exposure_is_explicitly_censored_not_completed(self):
        _, rows = compact_runs(frames=1)
        changed = []
        for original in rows:
            row = dict(original)
            if row['type'] == 'pipeline_window':
                c = dict(zip(gpuprof.PIPELINE_WINDOW_COLUMNS, row['v']))
                for key in ('eligible_samples', 'eligible_gpu_ms_sum', 'eligible_weighted_gpu_ms_sum',
                            'eligible_weighted_gpu_ms_square_sum', 'eligible_missing_selected'):
                    c[key] = 0
                c['weighted_gpu_ms_sum'] *= 2  # unfinished population4096 rather than2048
                row['v'] = [c[k] for k in gpuprof.PIPELINE_WINDOW_COLUMNS]
            elif row['type'] == 'pipeline_window_coverage':
                # One completed empty frame plus unfinished selected observations;
                # the latter carry no completed/eligible frame authority.
                row.update(frame_overflow=1, population_exposure=[
                    [2048, 4, 2048, 1, 1, 0, 0],
                    [4096, 4, 4096, 0, 0, row['observed_groups'], row['selected_groups']]])
                with self.assertRaises(ValueError):
                    gpuprof.validate(dict(row, frame_overflow=0))
            changed.append(row)
        report = self.analyze(changed)
        self.assertEqual(report['coverage']['malformed_rows'], 0, report['issues'])
        self.assertEqual(report['pipelines']['coverage']['invalid_coverage'], 0)
        self.assertEqual(report['pipelines']['eligible_frames'], 1)
        self.assertIn('frame_overflow', report['pipelines']['censor_reasons'])
        self.assertTrue(all(g['eligible_estimated_gpu_ms'] is None for g in report['pipelines']['pipelines']))
        footer = session_end(last_frame=0, last_window_sequence=0, last_window_end=0,
                             unclosed_pipeline_groups=3, unclosed_pipeline_frames=1)
        no_completed = self.analyze([compact_header(), footer])['pipelines']
        self.assertEqual(no_completed['eligible_frames'], 0)
        self.assertEqual(no_completed['pipelines'], [])
        self.assertTrue(no_completed['censored'])
        self.assertIn('unclosed_pipeline_groups', no_completed['censor_reasons'])

    def test_sums_precede_ratios_not_ratio_averages(self):
        a = pass_row(gpu_ms=2, count=1, stats_valid=1, draws=1, stats=counters(3, 1, 100))
        b = pass_row(window_start=301, window_end=600, gpu_ms=18, count=9, stats_valid=9,
                     draws=9, stats=counters(90, 30, 200), ratios={'verts_per_draw': 999})
        report = self.analyze([window(measured_ms=2, span_ms=4), a,
                               window(window_start=301, window_end=600, measured_ms=18, span_ms=20), b])
        group = report['passes'][0]
        self.assertEqual(group['gpu_ms'], 20)
        self.assertEqual(group['stats']['ia_vertices'], 93)
        self.assertEqual(group['ratios']['verts_per_draw'], 9.3)
        self.assertEqual(group['ratios']['prims_per_draw'], 3.1)
        self.assertAlmostEqual(group['ratios']['fragments_per_prim'], 300 / 31)
        self.assertAlmostEqual(group['ratios']['ns_per_vertex'], 20e6 / 93)
        self.assertAlmostEqual(group['ratios']['ns_per_primitive'], 20e6 / 31)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)

    def test_compact_ordinals_reduce_rows_without_changing_sums_or_matched_stats(self):
        detailed = [pass_row(ordinal=n, count=1, stats_valid=1, draws=2, gpu_ms=1,
                             stats=counters(6, 2, 20)) for n in range(1, 9)]
        compact = pass_row(ordinal=0, count=8, stats_valid=8, draws=16, gpu_ms=8,
                           stats=counters(48, 16, 160), stats_gpu_ms=8, stats_draws=16)
        a = self.analyze([window(ordinals=True, measured_ms=8, span_ms=10), *detailed])
        b = self.analyze([window(ordinals=False, measured_ms=8, span_ms=10), compact])
        self.assertEqual(a['coverage']['pass_rows'], 8)
        self.assertEqual(b['coverage']['pass_rows'], 1)
        for key in ('gpu_ms', 'count', 'draws', 'stats', 'ratios', 'ratio_gpu_ms', 'ratio_draws'):
            self.assertEqual(a['families'][0][key], b['families'][0][key], key)
        self.assertLess(len(json.dumps(compact)), len(json.dumps(detailed)))
        self.assertEqual(b['passes'][0]['ordinals'], [0])
        self.assertEqual(b['coverage']['ordinal_metadata_mismatches'], 0)
        text = io.StringIO()
        gpuprof.render(b, text)
        self.assertIn('all (0)', text.getvalue())

    def test_zero_ordinal_is_only_valid_for_window_pass_aggregates(self):
        self.assertEqual(gpuprof.validate(pass_row(ordinal=0))['ordinal'], 0)
        for value in (0, -1, True, 1.5):
            with self.subTest(ordinal=value), self.assertRaises(ValueError):
                gpuprof.validate(pass_row(type='long_frame', frame=1, ordinal=value))
        self.assertEqual(gpuprof.validate(pass_row(type='long_frame', frame=1, ordinal=7))['ordinal'], 7)
        for value in (-1, True, 1.5):
            with self.subTest(ordinal=value), self.assertRaises(ValueError):
                gpuprof.validate(pass_row(ordinal=value))

    def test_compact_dedup_keeps_reference_and_scene_role(self):
        rows = [scene_row(64, 36, ordinal=0), scene_row(64, 36, 128, 72, ordinal=0),
                scene_row(64, 36, ordinal=0, scene_relative=False)]
        self.file('gpuprof-compact.jsonl.1', rows)
        self.file('gpuprof-compact.jsonl', [rows[0]])
        report = gpuprof.analyze([str(self.root / 'gpuprof-compact.jsonl*')])
        self.assertEqual(report['coverage']['duplicate_rows'], 1)
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['pass_rows'], 3)
        self.assertEqual(len(report['passes'][0]['areas']), 3)
        self.assertEqual(report['passes'][0]['gpu_ms'], 30)

    def test_window_ordinal_metadata_is_optional_boolean_and_checked(self):
        for value in (0, 1, 'false', None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gpuprof.validate(window(ordinals=value))
        report = self.analyze([window(ordinals=False), pass_row(ordinal=1)])
        self.assertEqual(report['coverage']['ordinal_metadata_mismatches'], 1)
        self.assertEqual(self.analyze([window(), pass_row()])['coverage']['malformed_rows'], 0)

    def test_partial_compact_statistics_retain_explicitly_matched_denominators(self):
        row = pass_row(ordinal=0, count=10, stats_valid=2, draws=100, gpu_ms=100,
                       stats_gpu_ms=2, stats_draws=10)
        group = self.analyze([row])['passes'][0]
        self.assertEqual(group['stats_missing'], 8)
        self.assertEqual(group['ratio_draws'], 10)
        self.assertEqual(group['ratio_gpu_ms'], 2)
        self.assertEqual(group['ratios']['verts_per_draw'], 3)
        self.assertAlmostEqual(group['ratios']['ns_per_vertex'], 2e6 / 30)
        for changes in ({'stats_gpu_ms': -1}, {'stats_draws': 101}, {'stats_gpu_ms': 101}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                gpuprof.validate({**row, **changes})

    def test_headers_rotation_sequences_and_writer_end_expose_capture_loss(self):
        self.file('gpuprof-loss.jsonl.1', [header(rotation=4),
                  window(window_start=301, window_end=600, sequence=2)])
        self.file('gpuprof-loss.jsonl', [header(rotation=5),
                  window(window_start=901, window_end=1200, sequence=4),
                  session_end(last_frame=1200, last_window_sequence=4, last_window_end=1200)])
        report = gpuprof.analyze([str(self.root / 'gpuprof-loss.jsonl*')])
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(report['coverage']['missing_windows'], 2)
        self.assertEqual(report['coverage']['rotation_evictions'], 2)
        self.assertEqual(report['coverage']['missing_rotation_files'], 4)
        self.assertEqual(report['coverage']['streams_without_end'], 0)
        self.assertTrue(report['sessions'][0]['clean_end'])
        self.assertIsNone(report['sessions'][0]['all_queries_retired'])
        self.assertEqual(len(report['sessions'][0]['headers']), 2)

    def test_unmarked_session_is_not_a_clean_exit_and_old_capture_only_proves_interior_holes(self):
        report = self.analyze([header(), window(), pass_row(ordinal=0)])
        self.assertFalse(report['sessions'][0]['clean_end'])
        self.assertEqual(report['coverage']['streams_without_end'], 1)
        self.assertTrue(any('no session_end' in issue for issue in report['issues']))
        old = self.analyze([window(window_start=301, window_end=600),
                            window(window_start=901, window_end=1200)])
        self.assertEqual(old['coverage']['missing_windows'], 1)
        self.assertFalse(old['sessions'][0]['leading_coverage_known'])
        frames_only = self.analyze([header(), frame(frame=601)])
        self.assertEqual(frames_only['coverage']['missing_windows'], 2)

    def test_background_backlog_is_explicit_without_fabricating_a_clean_end(self):
        rows = [header(background_slots=16), window(background_dropped_slots=3),
                window(window_start=301, window_end=600, background_dropped_slots=7)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['background_dropped_slots'], 7, 'cumulative count is not summed')
        self.assertEqual(report['coverage']['streams_without_end'], 1)
        self.assertEqual(report['coverage']['censored_windows'], 2)
        self.assertTrue(any('background backlog' in issue for issue in report['issues']))
        ended = self.analyze(rows + [session_end(background_dropped_slots=9)])
        self.assertEqual(ended['coverage']['background_dropped_slots'], 9, 'footer includes final pending drops')
        self.assertEqual(ended['coverage']['streams_without_end'], 0)
        self.assertIsNone(ended['sessions'][0]['all_queries_retired'])
        for value in (-1, True, 0.5, '3'):
            for row in (window(background_dropped_slots=value), session_end(background_dropped_slots=value)):
                with self.subTest(value=value, row=row), self.assertRaises(ValueError):
                    gpuprof.validate(row)

    def test_cpu_closure_backlog_and_pending_footer_remain_explicit(self):
        rows = [header(background_closures=256), window(background_dropped_closures=2),
                window(window_start=301, window_end=600, background_dropped_closures=5),
                session_end(last_frame=600, last_window_sequence=2, last_window_end=600,
                            background_dropped_closures=7, pending_frame_closures=3)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['background_dropped_closures'], 7,
                         'cumulative closure loss is a per-launch maximum, never summed or repaired')
        self.assertEqual(report['sessions'][0]['background_dropped_closures'], 7)
        self.assertEqual(report['sessions'][0]['pending_frame_closures'], 3)
        self.assertEqual(report['coverage']['censored_windows'], 2)
        self.assertIsNone(report['sessions'][0]['all_queries_retired'])
        self.assertIsNone(self.analyze([header(), session_end()])['sessions'][0]['pending_frame_closures'])
        for value in (-1, True, 0.5, '3'):
            for row in (window(background_dropped_closures=value),
                        session_end(background_dropped_closures=value),
                        session_end(pending_frame_closures=value)):
                with self.subTest(value=value, row=row), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for value in (0, -1, True, 0.5, '256'):
            with self.subTest(capacity=value), self.assertRaises(ValueError):
                gpuprof.validate(header(background_closures=value))

    def test_header_conflicts_and_invalid_lifecycle_fields_are_explicit(self):
        report = self.analyze([header(), header(rotation=1, ordinals=True), session_end()])
        self.assertEqual(report['coverage']['header_conflicts'], 1)
        self.assertFalse(report['sessions'][0]['clean_end'])
        for row in (header(rotation=-1), header(ordinals=1), header(driver=None),
                    window(sequence=2), session_end(clean_end=False)):
            with self.subTest(row=row), self.assertRaises(ValueError):
                gpuprof.validate(row)

    def test_query_retirement_true_reports_counts_separately_from_writer_closure(self):
        counts = query_retirement()
        footer = session_end(all_queries_retired=True, query_retirement=counts)
        self.assertIs(gpuprof.validate(footer), footer)
        report = self.analyze([header(), window(), footer])
        session = report['sessions'][0]
        self.assertTrue(session['clean_end'])
        self.assertIs(session['all_queries_retired'], True)
        self.assertEqual(session['query_retirement'], counts)
        self.assertEqual(session['end']['query_retirement'], counts)
        self.assertEqual(report['coverage']['streams_without_end'], 0)
        text = io.StringIO()
        gpuprof.render(report, text)
        self.assertIn('clean_end=true all_queries_retired=true', text.getvalue())
        for key, value in counts.items():
            self.assertIn(f'{key}={value}', text.getvalue())
        for empty in (dict.fromkeys(gpuprof.RETIREMENT_COUNTS, 0),
                      query_retirement(leases_acquired=2, leases_submitted=0, leases_retired=0,
                                       queries_submitted=0, queries_retired=0)):
            with self.subTest(counts=empty):
                self.assertIs(gpuprof.validate(session_end(
                    all_queries_retired=True, query_retirement=empty))['all_queries_retired'], True)

    def test_query_retirement_false_does_not_make_a_clean_writer_end_incomplete(self):
        cases = (
            # A submitted lease still has fenced results to process.
            query_retirement(leases_acquired=6, leases_submitted=4, leases_outstanding=1,
                             queries_submitted=14, queries_outstanding=2),
            # An acquired recording has not been submitted or cancelled.
            query_retirement(leases_acquired=6, leases_outstanding=1),
            # Fenced lease results were attempted, but a query result was unavailable.
            query_retirement(queries_retired=11, queries_outstanding=1),
        )
        for counts in cases:
            with self.subTest(counts=counts):
                report = self.analyze([header(), window(), session_end(
                    all_queries_retired=False, query_retirement=counts)])
                session = report['sessions'][0]
                self.assertTrue(session['clean_end'])
                self.assertIs(session['all_queries_retired'], False)
                self.assertEqual(session['query_retirement'], counts)
                self.assertEqual(report['coverage']['malformed_rows'], 0, report['issues'])
                self.assertEqual(report['coverage']['streams_without_end'], 0)
                text = io.StringIO()
                gpuprof.render(report, text)
                self.assertIn('clean_end=true all_queries_retired=false', text.getvalue())
                self.assertIn(f'queries_outstanding={counts["queries_outstanding"]}', text.getvalue())

    def test_historical_nullable_retirement_is_unknown_without_fabricated_counts(self):
        footer = session_end()
        self.assertIs(gpuprof.validate(footer), footer)
        for rows, clean in (([header(), window(), footer], True), ([header(), window()], False)):
            with self.subTest(clean=clean):
                report = self.analyze(rows)
                session = report['sessions'][0]
                self.assertEqual(session['clean_end'], clean)
                self.assertIsNone(session['all_queries_retired'])
                self.assertIsNone(session['query_retirement'])
                self.assertIsNone(session['query_drain_timed_out'])
                if clean:
                    self.assertNotIn('query_retirement', session['end'])
                text = io.StringIO()
                gpuprof.render(report, text)
                self.assertIn('all_queries_retired=unknown', text.getvalue())
                self.assertIn('query_retirement: unknown (no accounting footer)', text.getvalue())
                self.assertNotIn('queries_outstanding=0', text.getvalue())

    def test_query_retirement_rejects_invalid_objects_fields_and_boolean(self):
        for value in (None, False, [], 0, 'counts'):
            with self.subTest(object=value), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=True, query_retirement=value))
        for key in gpuprof.RETIREMENT_COUNTS:
            for value in (-1, True, 0.5, '3', None):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(session_end(all_queries_retired=True,
                                                query_retirement=query_retirement(**{key: value})))
            missing = query_retirement()
            del missing[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=True, query_retirement=missing))
        for value in (None, 0, 1, 'true', 'false'):
            with self.subTest(boolean=value), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=value, query_retirement=query_retirement()))
        missing_boolean = session_end(query_retirement=query_retirement())
        del missing_boolean['all_queries_retired']
        with self.assertRaises(ValueError):
            gpuprof.validate(missing_boolean)
        for value in (True, False, 0, 'true'):
            with self.subTest(missing_counts=value), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=value))

    def test_query_retirement_rejects_inconsistent_accounting_and_boolean(self):
        inconsistent = (
            query_retirement(leases_acquired=6),
            query_retirement(leases_acquired=6, leases_retired=4),
            query_retirement(leases_submitted=4),  # Would submit a cancelled lease.
            query_retirement(queries_submitted=11),
            query_retirement(queries_retired=13),
            query_retirement(queries_outstanding=1),
        )
        for counts in inconsistent:
            with self.subTest(counts=counts), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=True, query_retirement=counts))
        for retired, counts in ((False, query_retirement()),
                                (True, query_retirement(leases_acquired=6, leases_outstanding=1)),
                                (True, query_retirement(queries_retired=11, queries_outstanding=1))):
            with self.subTest(retired=retired, counts=counts), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=retired, query_retirement=counts))
        report = self.analyze([header(), window(), session_end(
            all_queries_retired=True, query_retirement=query_retirement(queries_retired=11, queries_outstanding=1))])
        self.assertEqual(report['coverage']['malformed_rows'], 1)
        self.assertEqual(report['coverage']['streams_without_end'], 1)
        self.assertIsNone(report['sessions'][0]['all_queries_retired'])
        self.assertIsNone(report['sessions'][0]['query_retirement'])

    def test_query_drain_timeout_is_boolean_and_keeps_outstanding_accounting(self):
        counts = query_retirement(leases_acquired=6, leases_submitted=4, leases_outstanding=1,
                                  queries_submitted=14, queries_outstanding=2)
        report = self.analyze([header(), window(), session_end(
            all_queries_retired=False, query_retirement=counts, query_drain_timed_out=True)])
        session = report['sessions'][0]
        self.assertTrue(session['clean_end'])
        self.assertIs(session['query_drain_timed_out'], True)
        self.assertIs(session['all_queries_retired'], False)
        self.assertEqual(session['query_retirement'], counts)
        text = io.StringIO()
        gpuprof.render(report, text)
        self.assertIn('query_drain_timed_out=true', text.getvalue())
        self.assertIn('leases_outstanding=1', text.getvalue())
        self.assertIs(gpuprof.validate(session_end(
            all_queries_retired=True, query_retirement=query_retirement(),
            query_drain_timed_out=False))['query_drain_timed_out'], False)
        for value in (None, 0, 1, 'true', [], {}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gpuprof.validate(session_end(all_queries_retired=False, query_retirement=counts,
                                            query_drain_timed_out=value))

    def test_invalid_span_fields_require_explicit_nulls_not_missing_or_numeric_time(self):
        invalid = frame(span_valid=False, span_ms=None, gap_ms=None)
        for key in ('span_ms', 'gap_ms'):
            for omit in (False, True):
                row = dict(invalid)
                if omit:
                    del row[key]
                else:
                    row[key] = 0
                with self.subTest(key=key, omit=omit), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        with self.assertRaises(ValueError):
            gpuprof.validate(window(valid_span_ms=None))

    def test_invalid_spans_keep_measured_sums_but_not_fake_span_gap_totals(self):
        rows = [window(span_valid=False, span_ms=None, gap_ms=None, valid_span_ms=12,
                       valid_gap_ms=2, invalid_spans=1, unavailable_timestamps=1),
                pass_row(ordinal=0), frame(),
                frame(frame=2, span_valid=False, span_ms=None, gap_ms=None, raw_span_ms=5e10,
                      span_reason='implausible-timestamp', unavailable_timestamps=1, invalid_timestamps=2,
                      reconciliation_failed=True)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(report['coverage']['invalid_frame_spans'], 1)
        self.assertEqual(report['coverage']['invalid_window_spans'], 1)
        self.assertEqual(report['coverage']['invalid_timestamps'], 2)
        self.assertEqual(report['timing']['frame']['measured_ms'], 20)
        self.assertEqual(report['timing']['frame']['valid_span_ms'], 12)
        self.assertIsNone(report['timing']['window']['span_ms'])
        self.assertIsNone(report['timing']['frame']['gap_ms'])
        self.assertEqual(report['rejected_spans'][0]['raw_span_ms'], 5e10)

    def test_legacy_span_replay_rejects_huge_positive_spans_without_reinterpreting_passes(self):
        bad = self.file('gpuprof-old.jsonl', [dict(frame(span_ms=58640630904, gap_ms=58640630894), schema=2),
                        dict(window(span_ms=58640630904, gap_ms=58640630894), schema=2),
                        dict(pass_row(), schema=2)])
        report = gpuprof.analyze([bad], legacy_span_replay=True)
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(report['coverage']['legacy_ignored_rows'], 1)
        self.assertEqual(report['coverage']['invalid_frame_spans'], 1)
        self.assertEqual(report['coverage']['invalid_window_spans'], 1)
        self.assertEqual(report['timing']['frame']['measured_ms'], 10)
        self.assertEqual(report['passes'], [])
        self.assertEqual(report['rejected_spans'][0]['source_schema'], 2)
        self.assertEqual(gpuprof.analyze([bad])['coverage']['malformed_rows'], 3)
        result = subprocess.run([sys.executable, str(ROOT / 'tools/bench/gpuprof.py'),
                                 '--legacy-span-replay', str(bad), '--json', '-'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['legacy_span_replay'])
        plausible = self.file('gpuprof-b1.jsonl', [
            dict(frame(span_ms=55, measured_ms=44, gap_ms=11), schema=2),
            dict(window(frames=600, span_ms=32900, measured_ms=26000, gap_ms=6900), schema=2)])
        replay = gpuprof.analyze([plausible], legacy_span_replay=True)
        self.assertEqual(replay['coverage']['invalid_frame_spans'], 0)
        self.assertEqual(replay['coverage']['invalid_window_spans'], 0)
        self.assertEqual(replay['timing']['window']['span_ms'], 32900)

    def test_schema_three_old_false_success_still_rejects_implausible_span(self):
        report = self.analyze([frame(span_ms=5e10, gap_ms=5e10 - 10),
                               window(span_ms=12, measured_ms=10, gap_ms=2), pass_row()])
        self.assertEqual(report['coverage']['invalid_frame_spans'], 1)
        self.assertEqual(report['coverage']['invalid_window_spans'], 1)
        self.assertIsNone(report['timing']['window']['span_ms'])

    def test_family_keeps_structure_ignores_shader_key_and_ordinal(self):
        rows = [pass_row(gpu_ms=1), pass_row(key='b', ordinal=2, shader='shader-b', gpu_ms=2),
                pass_row(key='c', attachments=[attachment(width=2560)], gpu_ms=3),
                pass_row(key='d', samples=4, gpu_ms=4),
                pass_row(key='e', kind='compute', attachments=[], samples=0, gpu_ms=5)]
        report = self.analyze([window(measured_ms=15, span_ms=17), *rows])
        self.assertEqual(len(report['passes']), 5)
        self.assertEqual(len(report['families']), 4)
        merged = next(g for g in report['families'] if g['keys'] == ['a', 'b'])
        self.assertEqual(merged['gpu_ms'], 3)
        self.assertEqual(merged['ordinals'], [1, 2])
        self.assertEqual(merged['shaders'], ['shader-a', 'shader-b'])
        self.assertEqual(merged['stats']['ia_vertices'], 60)

    def test_exact_rationals_do_not_round_nearby_attachment_shapes(self):
        a, b = scene_row(100000, 720), scene_row(100000, 720, key='b')
        a['attachments'][0] = attachment(100001, 720, 100000, 720)
        b['attachments'][0] = attachment(100002, 720, 100000, 720)
        report = self.analyze([a, b])
        self.assertEqual(len(report['families']), 2)
        self.assertEqual(report['coverage']['missing_window_summaries'], 1)

    def test_depth_only_absolute_sizes_are_separate_families(self):
        rows = [pass_row(key=str(size), render_width=size, render_height=size,
                         attachments=[attachment(size, size, slot=8, format=126)])
                for size in (360, 512)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(len(report['passes']), 2)
        self.assertEqual(len(report['families']), 2)
        self.assertEqual({(g['render_width'], g['render_height']) for g in report['passes']},
                         {(360, 360), (512, 512)})
        for group in report['families']:
            self.assertFalse(group['scene_relative'])
            self.assertEqual(len(group['areas']), 1)
            area = group['areas'][0]
            self.assertEqual(area['keys'], group['keys'])
            self.assertEqual(area['attachments'][0]['width'], area['render_width'])

    def test_changing_scene_merges_geometry_but_keeps_fixed_shadow_absolute(self):
        rows = []
        for index, (width, height) in enumerate(((1280, 720), (1600, 900)), 1):
            rows.append(scene_row(width, height, key=f'geometry-{width}', count=1, stats_valid=1))
            rows.append(pass_row(key='shadow', render_width=360, render_height=360,
                                 reference_width=width, reference_height=height, count=1, stats_valid=1,
                                 attachments=[attachment(360, 360, width, height, slot=8, format=126)]))
            rows.append(frame(frame=index, scene_width=width, scene_height=height))
        report = self.analyze([window(measured_ms=40, span_ms=42), *rows])
        self.assertEqual(report['schema'], 'totk-gpuprof-analysis/3')
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)
        self.assertEqual(len(report['passes']), 3)
        self.assertEqual(len(report['families']), 2)
        geometry = next(g for g in report['families'] if g['scene_relative'])
        self.assertEqual(geometry['keys'], ['geometry-1280', 'geometry-1600'])
        self.assertEqual({(a['render_width'], a['render_height']) for a in geometry['areas']},
                         {(1280, 720), (1600, 900)})
        self.assertEqual({tuple(a['keys']) for a in geometry['areas']},
                         {('geometry-1280',), ('geometry-1600',)})
        shadow = next(g for g in report['families'] if not g['scene_relative'])
        self.assertEqual(shadow['keys'], ['shadow'])
        self.assertEqual(shadow['count'], 2)
        self.assertEqual({(a['render_width'], a['render_height']) for a in shadow['areas']}, {(360, 360)})
        self.assertEqual({(a['reference_width'], a['reference_height']) for a in shadow['areas']},
                         {(1280, 720), (1600, 900)})
        exact = next(g for g in report['passes'] if g['key'] == 'shadow')
        self.assertEqual((exact['render_width'], exact['render_height']), (360, 360))
        self.assertEqual((exact['attachments'][0]['width'], exact['attachments'][0]['height']), (360, 360))
        self.assertEqual(len(exact['areas']), 2)

    def test_scene_sized_depth_prepasses_merge_across_extents(self):
        report = self.analyze([scene_row(64, 36, depth_only=True, key='prepass-64'),
                               scene_row(128, 72, depth_only=True, key='prepass-128')])
        self.assertEqual(len(report['passes']), 2)
        self.assertEqual(len(report['families']), 1)
        group = report['families'][0]
        self.assertTrue(group['scene_relative'])
        self.assertEqual(group['keys'], ['prepass-128', 'prepass-64'])
        self.assertEqual({(a['render_width'], a['render_height']) for a in group['areas']},
                         {(64, 36), (128, 72)})

    def test_reference_variants_keep_one_exact_key_but_split_full_and_half_scene_families(self):
        for kind in ('pass', 'long_frame'):
            with self.subTest(kind=kind):
                rows = [scene_row(64, 36, key='geometry-64', type=kind, frame=1),
                        scene_row(128, 72, key='geometry-128', type=kind, frame=1),
                        scene_row(64, 36, 128, 72, key='geometry-64', type=kind, frame=1)]
                report = self.analyze(rows)
                prefix = 'long_frame_' if kind == 'long_frame' else ''
                self.assertEqual(report['coverage']['conflicting_rows'], 0)
                self.assertEqual(report['coverage']['duplicate_rows'], 0)
                self.assertEqual(len(report[prefix + 'passes']), 2)
                self.assertEqual(len(report[prefix + 'families']), 2)
                exact = next(g for g in report[prefix + 'passes'] if g['key'] == 'geometry-64')
                self.assertEqual(exact['gpu_ms'], 20)
                self.assertEqual(len(exact['areas']), 2)
                self.assertEqual({(a['reference_width'], a['reference_height']) for a in exact['areas']},
                                 {(64, 36), (128, 72)})
                full = next(g for g in report[prefix + 'families'] if len(g['keys']) == 2)
                self.assertEqual(full['gpu_ms'], 20)
                half = next(g for g in report[prefix + 'families'] if len(g['keys']) == 1)
                self.assertEqual(half['areas'][0]['attachments'][0]['width_num'], 1)
                self.assertEqual(half['areas'][0]['attachments'][0]['width_den'], 2)

    def test_scene_relative_flag_is_an_observation_and_family_identity(self):
        relative = scene_row(1280, 720, depth_only=True)
        absolute = {**relative, 'scene_relative': False}
        report = self.analyze([relative, absolute])
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['pass_rows'], 2)
        self.assertEqual(len(report['passes']), 1)
        self.assertEqual(len(report['families']), 2)
        self.assertEqual({a['scene_relative'] for a in report['passes'][0]['areas']}, {False, True})

    def test_absolute_render_area_is_not_replaced_by_attachment_area(self):
        rows = [pass_row(key='same-driver-key', render_width=width, render_height=height,
                         attachments=[attachment(), attachment(slot=1), attachment(slot=8, format=126)])
                for width, height in ((640, 360), (1280, 720))]
        rows[1].update(window_start=301, window_end=600)
        report = self.analyze(rows)
        self.assertEqual(len(report['passes']), 2, 'different absolute areas must not disappear even for a reused key')
        self.assertEqual(len(report['families']), 2)
        self.assertEqual({g['attachments'][0]['width'] for g in report['passes']}, {1280})
        relative = [dict(row, scene_relative=True) for row in rows]
        self.assertEqual(len(self.analyze(relative)['families']), 2, 'render/reference crop ratio is structural')

    def test_attachment_order_is_not_a_family_difference(self):
        a, b = attachment(), attachment(); b['slot'] = 1
        report = self.analyze([pass_row(attachments=[a, b]), pass_row(key='b', attachments=[b, a])])
        self.assertEqual(len(report['families']), 1)

    def test_disabled_statistics_are_absent_not_synthetic_zero(self):
        row = pass_row(stats_valid=0); del row['stats']
        group = self.analyze([row])['passes'][0]
        self.assertIsNone(group['stats'])
        self.assertIsNone(group['ratios'])
        self.assertEqual(group['stats_missing'], 2)
        self.assertEqual(group['ns_per_draw'], 1e6)

    def test_nonrender_scope_kinds_have_separate_families_and_optional_stats(self):
        kinds = ('compute', 'upload', 'transfer', 'post', 'upscale', 'present', 'framegen')
        rows = [pass_row()]
        for kind in kinds:
            row = pass_row(kind=kind, key=kind, attachments=[], samples=0, stats_valid=0, count=1, draws=0)
            del row['stats']
            rows.append(row)
        report = self.analyze([window(measured_ms=80, span_ms=82), *rows])
        self.assertEqual(report['coverage']['malformed_rows'], 0)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)
        self.assertEqual(len(report['families']), 8)
        for group in report['families']:
            if group['kind'] != 'render':
                self.assertIsNone(group['stats'])
                self.assertIsNone(group['ratios'])
        self.assertEqual(report['coverage']['stats_missing'], 7)

    def test_mixed_availability_uses_only_matched_time_and_draws(self):
        measured = pass_row(gpu_ms=2, count=1, stats_valid=1, draws=1, stats=counters(3, 1, 20))
        missing = pass_row(window_start=301, window_end=600, gpu_ms=100, count=4, stats_valid=0, draws=100)
        del missing['stats']
        group = self.analyze([measured, missing])['passes'][0]
        self.assertEqual(group['gpu_ms'], 102)
        self.assertEqual(group['stats_missing'], 4)
        self.assertEqual(group['ratios']['verts_per_draw'], 3)
        self.assertAlmostEqual(group['ratios']['ns_per_vertex'], 2e6 / 3)
        self.assertEqual(group['ratio_draws'], 1)
        self.assertEqual(group['ratio_gpu_ms'], 2)

    def test_partial_statistics_keep_counter_ratios_not_unmatched_denominators(self):
        group = self.analyze([pass_row(count=10, stats_valid=2)])['passes'][0]
        self.assertEqual(group['stats']['ia_vertices'], 30)
        self.assertEqual(group['ratios']['fragments_per_prim'], 10)
        self.assertIsNone(group['ratios']['verts_per_draw'])
        self.assertIsNone(group['ratios']['ns_per_vertex'])
        self.assertEqual(group['stats_partial_rows'], 1)
        self.assertEqual(group['stats_missing'], 8)

    def test_zero_denominators_are_unavailable(self):
        group = self.analyze([pass_row(draws=0, stats=counters(0, 0, 0))])['passes'][0]
        self.assertTrue(all(value is None for value in group['ratios'].values()))
        self.assertIsNone(group['ns_per_draw'])

    def test_long_frame_population_is_never_double_counted(self):
        selected = pass_row(type='long_frame', frame=12, gpu_ms=99)
        del selected['window_start']; del selected['window_end']
        report = self.analyze([window(), pass_row(), frame(), selected])
        self.assertEqual(report['passes'][0]['gpu_ms'], 10)
        self.assertEqual(report['families'][0]['gpu_ms'], 10)
        self.assertEqual(report['long_frame_passes'][0]['gpu_ms'], 99)
        self.assertEqual(report['coverage']['long_frames'], 1)
        self.assertEqual(report['timing']['window']['measured_ms'], 10)
        self.assertEqual(report['timing']['frame']['measured_ms'], 10)

    def test_long_frame_history_does_not_impose_a_dump_limit(self):
        rows = [pass_row(type='long_frame', frame=n, count=1, stats_valid=1, gpu_ms=n) for n in range(1, 10)]
        report = self.analyze(rows)
        self.assertEqual(report['coverage']['long_frames'], 9)
        self.assertEqual(report['long_frame_passes'][0]['gpu_ms'], 45)
        self.assertEqual(report['passes'], [])

    def test_censoring_frame_gaps_and_reconciliation_are_explicit(self):
        report = self.analyze([window(dropped=3, failed_slots=1, stats_failed_slots=2, partial=True), pass_row(),
                               frame(), frame(frame=4, dropped=2, reconciliation_failed=True),
                               frame(frame=5, gap_ms=0)])
        coverage = report['coverage']
        self.assertEqual(coverage['censored_windows'], 1)
        self.assertEqual(coverage['censored_frames'], 1)
        self.assertEqual(coverage['missing_frame_ids'], 2)
        self.assertEqual(coverage['reconciliation_failures'], 1)
        self.assertEqual(coverage['dropped'], 3)
        self.assertEqual(coverage['failed_slots'], 1)
        self.assertEqual(coverage['stats_failed_slots'], 2)
        self.assertEqual(report['timing']['frame']['gap_ms'], 4)

    def test_missing_rotated_pass_rows_are_censored(self):
        report = self.analyze([window(), pass_row(gpu_ms=4)])
        self.assertEqual(report['coverage']['window_sum_mismatches'], 1)
        self.assertTrue(any('censored' in issue for issue in report['issues']))

    def test_rotation_globs_duplicate_inputs_and_duplicate_rows(self):
        self.file('gpuprof-run.jsonl.3', [window(), pass_row()])
        current = self.file('gpuprof-run.jsonl', [pass_row(), frame()])
        report = gpuprof.analyze([str(self.root / '*.jsonl*'), current])
        self.assertEqual(len(report['files']), 2)
        self.assertEqual(report['coverage']['streams'], 1)
        self.assertEqual(report['coverage']['duplicate_rows'], 1)
        self.assertEqual(report['passes'][0]['gpu_ms'], 10)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)

    def test_separate_launches_keep_identical_window_observations(self):
        a = self.file('gpuprof-one.jsonl', [window(), pass_row()])
        b = self.file('gpuprof-two.jsonl', [window(), pass_row()])
        report = gpuprof.analyze([a, b])
        self.assertEqual(report['coverage']['streams'], 2)
        self.assertEqual(report['coverage']['duplicate_rows'], 0)
        self.assertEqual(report['passes'][0]['gpu_ms'], 20)

    def test_conflicts_exclude_both_not_arbitrary_first_wins(self):
        report = self.analyze([window(), pass_row(), pass_row(gpu_ms=999)])
        self.assertEqual(report['coverage']['conflicting_rows'], 1)
        self.assertEqual(report['passes'], [])
        self.assertEqual(report['coverage']['window_sum_mismatches'], 1)

    def test_malformed_rows_and_incomplete_rotation_tail_are_reported(self):
        path = self.file('gpuprof-run.jsonl', [window(), pass_row()], '\nnot json\n{"schema":3')
        report = gpuprof.analyze([path])
        self.assertEqual(report['coverage']['malformed_rows'], 1)
        self.assertEqual(report['coverage']['truncated_rows'], 1)
        self.assertEqual(report['coverage']['blank_lines'], 1)
        self.assertEqual(report['coverage']['pass_rows'], 1)

    def test_invalid_schema_fields_are_rejected_without_losing_valid_rows(self):
        invalid = []
        for changes in ({'schema': 1}, {'schema': 2}, {'gpu_ms': -1},
                        {'gpu_ms': float('nan')}, {'gpu_ms': float('inf')},
                        {'draws': True}, {'stats_valid': 3}, {'stats': None}, {'type': 'other'},
                        {'attachments': [{**attachment(), 'width_den': 0}]},
                        {'attachments': [{**attachment(), 'width_num': 2, 'width_den': 2}]},
                        {'attachments': [attachment(), attachment()]}, {'ratios': {'ns_per_vertex': -1}}):
            invalid.append(pass_row(**changes))
        report = self.analyze([*invalid, pass_row()])
        self.assertEqual(report['coverage']['malformed_rows'], len(invalid))
        self.assertEqual(report['coverage']['pass_rows'], 1)

    def test_schema_two_is_rejected_for_every_row_type(self):
        rows = [window(), pass_row(), frame(), pass_row(type='long_frame', frame=1)]
        report = self.analyze([dict(row, schema=2) for row in rows])
        self.assertEqual(report['coverage']['malformed_rows'], 4)
        self.assertEqual(report['coverage']['pass_rows'], 0)
        self.assertEqual(report['coverage']['frame_rows'], 0)
        self.assertEqual(report['coverage']['windows'], 0)
        self.assertEqual(report['coverage']['long_frame_rows'], 0)

    def test_absolute_reference_and_scene_relative_fields_are_required(self):
        for kind in ('pass', 'long_frame'):
            for field in ('render_width', 'render_height', *gpuprof.REFERENCE):
                row = pass_row(type=kind, frame=1)
                del row[field]
                with self.subTest(kind=kind, field=field), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for field in ('width', 'height'):
            row = pass_row()
            del row['attachments'][0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                gpuprof.validate(row)

    def test_dimensions_enforce_uint32_bounds_and_zero_pairing(self):
        for field in ('render_width', 'render_height', 'reference_width', 'reference_height'):
            for value in (0, -1, True, 1.5, gpuprof.UINT32_MAX + 1):
                row = pass_row()
                row[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for field in ('width', 'height'):
            for value in (0, -1, True, 1.5, gpuprof.UINT32_MAX + 1):
                row = pass_row()
                row['attachments'][0][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for value in (0, 1, 'true', None):
            with self.subTest(scene_relative=value), self.assertRaises(ValueError):
                gpuprof.validate(pass_row(scene_relative=value))
        self.assertEqual(gpuprof.validate(pass_row(kind='compute', attachments=[], samples=0))['render_width'], 0)
        for width, height in ((0, 1), (1, 0)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                gpuprof.validate(pass_row(kind='compute', render_width=width, render_height=height, attachments=[]))

    def test_reference_bounds_are_not_limited_to_fallback_game_configuration(self):
        for width, height in ((1, 1), (4097, 16384), (gpuprof.UINT32_MAX, gpuprof.UINT32_MAX)):
            row = pass_row(render_width=width, render_height=height,
                           reference_width=width, reference_height=height)
            with self.subTest(width=width, height=height):
                self.assertIs(gpuprof.validate(row), row)
        row = pass_row(reference_width=1, reference_height=1)
        self.assertGreater(row['attachments'][0]['width_num'], 1, 'attachments may exceed the scene reference')
        self.assertIs(gpuprof.validate(row), row)

    def test_attachment_rationals_must_match_absolute_reference_dimensions(self):
        for axis in ('width', 'height'):
            for field, value in ((axis, 1), (f'{axis}_num', 0), (f'{axis}_den', 2)):
                row = pass_row()
                row['attachments'][0][field] = value
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'rational'):
                    gpuprof.validate(row)
        row = pass_row(render_width=360, render_height=360,
                       attachments=[attachment(360, 360, slot=8, format=126)])
        self.assertEqual((row['attachments'][0]['width_num'], row['attachments'][0]['width_den']), (9, 32))
        self.assertEqual((row['attachments'][0]['height_num'], row['attachments'][0]['height_den']), (1, 2))
        self.assertIs(gpuprof.validate(row), row, 'ratios are against the common reference, not pass area')

    def test_frame_scene_dimensions_are_required_paired_and_preserved(self):
        rows = [frame(), frame(frame=2, scene_width=1280, scene_height=720),
                frame(frame=3, scene_width=gpuprof.UINT32_MAX, scene_height=gpuprof.UINT32_MAX)]
        path = self.file('gpuprof-scenes.jsonl', rows)
        observations, counts, _ = gpuprof.load([path])
        self.assertEqual(counts['malformed_rows'], 0)
        self.assertEqual([o['row'] for o in observations], rows)
        for field in ('scene_width', 'scene_height'):
            for value in (None, -1, True, 1.5, gpuprof.UINT32_MAX + 1):
                row = frame(scene_width=1280, scene_height=720)
                if value is None:
                    del row[field]
                else:
                    row[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    gpuprof.validate(row)
        for width, height in ((0, 720), (1280, 0)):
            with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                gpuprof.validate(frame(scene_width=width, scene_height=height))

    def test_rotated_reference_variants_are_not_duplicate_or_conflicting_rows(self):
        a = scene_row(64, 36)
        b = scene_row(64, 36, 128, 72)
        self.file('gpuprof-refs.jsonl.1', [a])
        self.file('gpuprof-refs.jsonl', [a, b])
        report = gpuprof.analyze([str(self.root / '*.jsonl*')])
        self.assertEqual(report['coverage']['duplicate_rows'], 1)
        self.assertEqual(report['coverage']['conflicting_rows'], 0)
        self.assertEqual(report['coverage']['pass_rows'], 2)
        self.assertEqual(len(report['passes'][0]['areas']), 2)

    def test_invalid_stats_presence_and_incomplete_stats_are_rejected(self):
        missing = pass_row(); del missing['stats']
        zero = pass_row(stats_valid=0)
        incomplete = pass_row(stats={k: v for k, v in counters().items() if k != 'fs_invocations'})
        self.assertEqual(self.analyze([missing, zero, incomplete])['coverage']['malformed_rows'], 3)

    def test_text_and_json_cli_and_input_errors(self):
        path = self.file('gpuprof-run.jsonl', [window(), pass_row()])
        command = [sys.executable, str(ROOT / 'tools/bench/gpuprof.py')]
        result = subprocess.run([*command, str(path), '--json', '-'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['passes'][0]['key'], 'a')
        output = self.root / 'analysis.json'
        result = subprocess.run([*command, str(path), '--json', str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Structural families', result.stdout)
        self.assertIn('stats valid/missing', result.stdout)
        self.assertTrue(output.is_file())
        for arguments in ([str(self.root / 'absent*')], [str(path), '--json', str(path)]):
            result = subprocess.run([*command, *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(json.loads(path.read_text().splitlines()[0])['schema'], 3)

    def test_cli_malformed_interior_is_failure_but_partial_tail_is_warning(self):
        command = [sys.executable, str(ROOT / 'tools/bench/gpuprof.py')]
        for tail, status in [('bad\n', 1), ('{"schema":', 0)]:
            path = self.file('gpuprof-run.jsonl', [pass_row()], tail)
            result = subprocess.run([*command, str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, status, result.stderr)
            self.assertIn('WARNING:', result.stdout)

    def test_render_includes_all_counters_and_missing_data_not_zero_ratios(self):
        row = pass_row(stats_valid=0); del row['stats']
        text = io.StringIO(); gpuprof.render(self.analyze([row]), text)
        self.assertIn('counters: unavailable', text.getvalue())
        self.assertIn(' | - | - | - | - | -', text.getvalue())
        text = io.StringIO(); gpuprof.render(self.analyze([pass_row()]), text)
        for key in gpuprof.STATS:
            self.assertIn(key + '=', text.getvalue())

    def test_render_includes_absolute_join_fields_and_all_reference_variants(self):
        rows = [scene_row(64, 36), scene_row(64, 36, 128, 72)]
        report = self.analyze(rows)
        group = report['passes'][0]
        self.assertEqual((group['render_width'], group['render_height']), (64, 36))
        self.assertEqual({(a['width'], a['height']) for a in group['attachments']}, {(64, 36)})
        text = io.StringIO()
        gpuprof.render(report, text)
        rendered = text.getvalue()
        self.assertIn('schema 3', rendered)
        self.assertIn('render area(s)', rendered)
        self.assertIn('64x36', rendered)
        self.assertIn('"width":64', rendered)
        self.assertIn('"height":36', rendered)
        self.assertIn('"reference_width":64', rendered)
        self.assertIn('"reference_width":128', rendered)

    def test_sampled_interval_merges_histograms_before_mean_and_percentiles(self):
        first = sampled_pass(durations=(0.001024,), observed_count=30, observed_frames=30)
        second = sampled_pass(durations=(0.004096,) * 3, observed_count=30, observed_frames=30,
                              window_start=301, window_end=600)
        report = self.analyze([sampled_window(measured_ms=first['gpu_ms'], observed_passes=30,
                                              sample_count=1, timestamp_count=602), first,
                               sampled_window(window_start=301, window_end=600, measured_ms=second['gpu_ms'],
                                              observed_passes=30, sample_count=3, timestamp_count=606), second])
        group = report['passes'][0]
        self.assertEqual(len(report['passes']), 1, 'default must aggregate the whole input interval')
        self.assertEqual(group['count'], 60)
        self.assertEqual(group['sampling']['observed_count'], 60)
        self.assertEqual(group['sampling']['sample_count'], 4)
        self.assertEqual(group['sampling']['observed_frames'], 60)
        self.assertEqual(group['sampling']['sampled_frames'], 4)
        self.assertEqual(group['sampling']['histogram'][80], 1)
        self.assertEqual(group['sampling']['histogram'][96], 3)
        self.assertEqual(sum(group['sampling']['histogram']), 4)
        self.assertAlmostEqual(group['mean_ms'], (0.001024 + 3 * 0.004096) / 4)
        for percentile in ('p50_ms', 'p90_ms', 'p99_ms'):
            self.assertAlmostEqual(group[percentile], 0.004096)
        self.assertEqual(group['gpu_ms'], first['gpu_ms'] + second['gpu_ms'])
        self.assertNotEqual(group['gpu_ms'], group['mean_ms'] * group['count'],
                            'sampled totals must never be extrapolated to all observations')
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)

    def test_sampled_thin_warning_uses_interval_not_each_window(self):
        rows = []
        for index in range(5):
            start, end = index * 300 + 1, (index + 1) * 300
            rows.extend([sampled_window(window_start=start, window_end=end, sample_count=3,
                                        observed_passes=300, timestamp_count=606, measured_ms=0.003072),
                         sampled_pass(durations=(0.001024,) * 3, observed_count=300, observed_frames=300,
                                      window_start=start, window_end=end)])
        path = self.file('gpuprof-interval.jsonl', rows)
        report = gpuprof.analyze([path], per_window=True)
        group = report['passes'][0]
        self.assertEqual(group['sampling']['sample_count'], 15)
        self.assertEqual(group['sampling']['sampled_frames'], 15)
        self.assertAlmostEqual(group['sampling']['frame_coverage'], 0.01)
        self.assertFalse(group['sampling']['thin'])
        self.assertFalse(any('thin' in issue.lower() for issue in report['issues']), report['issues'])
        self.assertEqual(len(report['per_window']), 5)
        self.assertTrue(all(w['passes'][0]['sampling']['sample_count'] == 3 for w in report['per_window']))
        result = subprocess.run([sys.executable, str(ROOT / 'tools/bench/gpuprof.py'), str(path),
                                 '--per-window', '--json', '-'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(json.loads(result.stdout)['per_window']), 5)
        for samples, observed in ((0, 300), (9, 300)):
            with self.subTest(samples=samples, observed=observed):
                thin = self.analyze([sampled_pass(durations=(0.001024,) * samples,
                                                 observed_count=observed, observed_frames=observed)])
                self.assertTrue(thin['passes'][0]['sampling']['thin'])
                self.assertTrue(any('thin' in issue.lower() for issue in thin['issues']))
                self.assertEqual(thin['coverage']['malformed_rows'], 0, 'thin coverage is only a warning')
        threshold = self.analyze([sampled_pass(durations=(0.001024,) * 10,
                                              observed_count=300, observed_frames=300)])
        self.assertFalse(threshold['passes'][0]['sampling']['thin'],
                         '10 sampled frames suffice even when coverage is below 5%')

    def test_sampled_percentiles_expose_bucket_precision_and_sparse_tail_confidence(self):
        row = sampled_pass(durations=(0.003,))
        group = self.analyze([row])['passes'][0]
        upper_bound = 2 ** (math.ceil(math.log2(3000) * 8) / 8) / 1e6
        self.assertAlmostEqual(group['p99_ms'], upper_bound)
        self.assertGreaterEqual(group['p99_ms'], 0.003)
        self.assertLessEqual(group['p99_ms'], 0.003 * 2 ** (1 / 8))
        self.assertTrue(group['p99_descriptive_only'])
        for samples, descriptive in ((0, True), (99, True), (100, False)):
            with self.subTest(samples=samples):
                report = self.analyze([sampled_pass(durations=(0.001024,) * samples,
                                                   observed_count=300, observed_frames=300)])
                self.assertEqual(report['passes'][0]['p99_descriptive_only'], descriptive)
                self.assertEqual(report['passes'][0]['p99_ms'] is None, samples == 0)

    def test_sampled_zero_sample_keys_keep_observed_metadata_without_fake_times(self):
        row = sampled_pass(durations=(), observed_count=30, observed_frames=30)
        report = self.analyze([row])
        group = report['passes'][0]
        for field in ('key', 'kind', 'render_width', 'render_height', 'draws', 'count'):
            self.assertEqual(group[field], row[field])
        self.assertEqual(group['areas'][0]['attachments'], row['attachments'])
        for actual, original in zip(group['attachments'], row['attachments']):
            self.assertEqual(actual, {key: value for key, value in original.items()
                                      if key not in ('width_num', 'width_den', 'height_num', 'height_den')})
        for field in ('reference_width', 'reference_height', 'scene_relative'):
            self.assertEqual(group['areas'][0][field], row[field])
        self.assertEqual(group['gpu_ms'], 0)
        self.assertEqual(group['sampling']['sample_count'], 0)
        self.assertEqual(sum(group['sampling']['histogram']), 0)
        self.assertIsNone(group['stats'])
        for field in ('mean_ms', 'p50_ms', 'p90_ms', 'p99_ms', 'ns_per_draw'):
            self.assertIsNone(group[field], field)

    def test_sampled_statistics_use_only_matched_sample_draws_and_duration(self):
        row = sampled_pass(stats=True)
        group = self.analyze([row])['passes'][0]
        self.assertEqual(group['count'], 20)
        self.assertEqual(group['draws'], 40)
        self.assertEqual(group['stats_valid'], 2)
        self.assertEqual(group['ratio_draws'], 4)
        self.assertEqual(group['ratio_gpu_ms'], row['gpu_ms'])
        self.assertEqual(group['ratios']['verts_per_draw'], 3)
        self.assertAlmostEqual(group['ratios']['ns_per_vertex'], row['gpu_ms'] * 1e6 / 12)
        self.assertAlmostEqual(group['ns_per_draw'], row['gpu_ms'] * 1e6 / 4)

    def test_sampled_histograms_and_coverage_are_strictly_validated(self):
        valid = sampled_pass()
        malformed = [
            {'budget': value} for value in (0, 17, True, 1.5, '2')
        ] + [
            {'observed_count': 21}, {'sample_count': 21}, {'sample_count': True},
            {'observed_frames': 301}, {'observed_frames': 21}, {'sampled_frames': 3},
            {'sampled_frames': True}, {'sampled_frames': -1}, {'sampled_frames': 0},
            {'histogram': [0] * 271}, {'histogram': [0] * 273}, {'histogram': [0] * 272},
            {'histogram': [-1, 3] + [0] * 270}, {'histogram': [True, 1] + [0] * 270},
            {'histogram': [1.0, 1] + [0] * 270},
        ]
        for changes in malformed:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                gpuprof.validate({**valid, 'sampling': {**valid['sampling'], **changes}})
        for field in valid['sampling']:
            sampling = dict(valid['sampling']); del sampling[field]
            with self.subTest(missing=field), self.assertRaises(ValueError):
                gpuprof.validate({**valid, 'sampling': sampling})
        for row in (sampled_pass(durations=(), gpu_ms=1), sampled_pass(stats_valid=3),
                    sampled_pass(type='long_frame', frame=1, ordinal=1),
                    {**valid, 'sampling': None}):
            with self.subTest(row=row), self.assertRaises(ValueError):
                gpuprof.validate(row)
        self.assertIs(gpuprof.validate(valid), valid)

    def test_sampled_timestamp_count_is_actual_per_frame_and_bounded(self):
        for budget in (1, 2):
            for samples in range(budget + 1):
                row = sampled_frame()
                row.update(sample_budget=budget, sampled_passes=samples, timestamp_count=2 * samples + 2)
                with self.subTest(budget=budget, samples=samples):
                    self.assertIs(gpuprof.validate(row), row)
            invalid = sampled_frame()
            invalid.update(sample_budget=budget, sampled_passes=budget + 1, timestamp_count=2 * budget + 4)
            with self.assertRaises(ValueError):
                gpuprof.validate(invalid)
        for changes in ({'timestamp_count': 1}, {'timestamp_count': 7}, {'timestamp_count': True},
                        {'observed_passes': 0}, {'sampled_passes': -1}, {'timing_scope': 'pre-main-slot'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                gpuprof.validate({**sampled_frame(), **changes})
        for field in ('sample_budget', 'timestamp_count', 'observed_passes', 'sampled_passes', 'timing_scope'):
            row = sampled_frame(); del row[field]
            with self.subTest(missing=field), self.assertRaises(ValueError):
                gpuprof.validate(row)
        report = self.analyze([sampled_frame(), sampled_frame(frame=2, timestamp_count=6)])
        self.assertEqual(report['timing']['frame']['timestamp_count'], 10)
        self.assertEqual(report['timing']['frame']['timestamp_count_min'], 4)
        self.assertEqual(report['timing']['frame']['timestamp_count_max'], 6)
        for changes in ({'timestamp_count': 599}, {'timestamp_count': 1801},
                        {'sample_count': 21}, {'sample_budget': 17}, {'timing_scope': 'pre-main-slot'}):
            with self.subTest(window_changes=changes), self.assertRaises(ValueError):
                gpuprof.validate({**sampled_window(), **changes})

    def test_sampled_family_histograms_merge_without_fabricating_exact_frame_union(self):
        rows = [sampled_pass(durations=(duration,) * 15, observed_count=30, observed_frames=30, key=key)
                for key, duration in (('a', 0.001024), ('b', 0.004096))]
        group = self.analyze(rows)['families'][0]
        sampling = group['sampling']
        self.assertEqual(sampling['sample_count'], 30)
        self.assertEqual(sampling['observed_count'], 60)
        self.assertEqual(sampling['histogram'][80], 15)
        self.assertEqual(sampling['histogram'][96], 15)
        self.assertEqual(sampling['observed_frames_bounds'], [30, 60])
        self.assertEqual(sampling['sampled_frames_bounds'], [15, 30])
        self.assertIsNone(sampling['observed_frames'])
        self.assertIsNone(sampling['sampled_frames'])
        self.assertAlmostEqual(group['mean_ms'], 0.00256)
        self.assertAlmostEqual(group['p50_ms'], 0.001024)
        self.assertAlmostEqual(group['p90_ms'], 0.004096)

    def test_sampled_negative_residual_does_not_falsely_reject_main_frame_span(self):
        report = self.analyze([sampled_frame(span_ms=2, measured_ms=3, gap_ms=-1),
                               sampled_window(span_ms=2, measured_ms=3, gap_ms=-1)])
        self.assertEqual(report['coverage']['invalid_frame_spans'], 0)
        self.assertEqual(report['coverage']['invalid_window_spans'], 0)
        self.assertEqual(report['timing']['frame']['gap_ms'], -1)
        full = self.analyze([frame(span_ms=2, measured_ms=3, gap_ms=-1)])
        self.assertEqual(full['coverage']['invalid_frame_spans'], 1)

    def test_sampled_incomplete_pass_sum_does_not_disable_span_authority(self):
        report = self.analyze([sampled_window(measured_ms=9), sampled_pass(),
                               sampled_frame(span_valid=False, span_ms=None, gap_ms=None,
                                             span_reason='unavailable-timestamp', unavailable_timestamps=1)])
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0,
                         'sampled rows are not a complete whole-pass accounting')
        self.assertEqual(report['coverage']['invalid_frame_spans'], 1)
        self.assertEqual(report['coverage']['invalid_window_spans'], 1)
        self.assertIsNone(report['timing']['frame']['span_ms'])

    def test_full_mode_schema_and_ratios_remain_unchanged(self):
        report = self.analyze([window(), pass_row(), frame()])
        for field in ('sampling', 'mean_ms', 'p50_ms', 'p90_ms', 'p99_ms'):
            self.assertNotIn(field, report['passes'][0])
        self.assertEqual(report['passes'][0]['ns_per_draw'], 1e6)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0)

    def frame_close_rows(self):
        """A complete old full-mode window; no post-fix closure event is invented."""
        commit = 'd003f2db137c'
        identity = header(build_commit=commit, build_asset=f'commit {commit}\n',
                          build_identity_source='apk:assets/build.txt', build_identity_reason=None,
                          sample_budget=0, timestamp_bits=48, timestamp_period_ns=1)
        members = [frame(frame=n, span_ms=None, gap_ms=None, raw_span_ms=12,
                         span_valid=False, span_reason='incomplete-records', reconciliation_failed=True,
                         first_tick=n * 20000000, last_tick=n * 20000000 + 12000000,
                         timestamp_bits=48, timestamp_period=1, queue_family=0,
                         unavailable_timestamps=0, invalid_timestamps=0, timestamp_wraps=0)
                   for n in range(1, 301)]
        summary = window(sequence=1, ordinals=False, span_valid=False, span_ms=None, gap_ms=None,
                         measured_ms=3000, invalid_spans=300, span_frames=0,
                         valid_span_ms=0, valid_gap_ms=0, unavailable_timestamps=0, invalid_timestamps=0,
                         output_failures=0, background_dropped_slots=0)
        passes = pass_row(ordinal=0, count=300, draws=3000, gpu_ms=3000, stats_valid=0)
        passes.pop('stats')
        return [identity, *members, summary, passes, session_end(background_dropped_slots=0)]

    def frame_close_replay(self, rows):
        return gpuprof.analyze([self.file('gpuprof-frame-close.jsonl', rows)], frame_close_replay=True)

    def test_frame_close_replay_is_opt_in_counterfactual_and_preserves_original_authority(self):
        rows = self.frame_close_rows()
        path = self.file('gpuprof-frame-close.jsonl', rows)
        before = path.read_bytes()
        ordinary = gpuprof.analyze([path])
        report = gpuprof.analyze([path], frame_close_replay=True)
        replay = report.pop('frame_close_replay')
        self.assertEqual(report, ordinary, 'opt-in replay must not silently repair ordinary timing or coverage')
        self.assertEqual(path.read_bytes(), before, 'original capture is never rewritten')
        self.assertEqual(replay['schema'], 'totk-gpuprof-frame-close-replay/1')
        self.assertEqual(replay['mode'], 'counterfactual-only')
        self.assertFalse(replay['device_correction_verified'])
        self.assertEqual(len(replay['files'][0]['sha256']), 64)
        self.assertEqual(replay['files'][0]['bytes'], len(before))
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], list(range(1, 301)))
        self.assertEqual(replay['streams'][0]['rejected_frame_ids'], [])
        self.assertEqual(replay['streams'][0]['eligible_windows'], [[1, 300]])
        self.assertEqual(replay['windows'][0]['eligible_frame_ids'], list(range(1, 301)))
        self.assertEqual(replay['windows'][0]['counterfactual_span_ms'], 3600)
        self.assertEqual(replay['windows'][0]['counterfactual_gap_ms'], 600)
        self.assertEqual(replay['windows'][0]['frame_measured_ms'], 3000)
        self.assertEqual(replay['windows'][0]['pass_measured_ms'], 3000)
        for result, original in zip(replay['frames'], rows[1:301]):
            self.assertTrue(result['eligible'])
            self.assertEqual(result['original'], original)
            self.assertFalse(result['original']['span_valid'])
            self.assertEqual(result['original']['span_reason'], 'incomplete-records')
            self.assertEqual(result['counterfactual_span_ms'], 12)
            self.assertEqual(result['counterfactual_gap_ms'], 2)
        self.assertEqual(report['coverage']['invalid_frame_spans'], 300)
        self.assertEqual(report['coverage']['invalid_window_spans'], 1)
        self.assertIsNone(report['timing']['frame']['span_ms'])
        self.assertEqual(report['timing']['frame']['measured_ms'], 3000)
        self.assertEqual(report['passes'][0]['gpu_ms'], 3000)
        self.assertIsNone(report['sessions'][0]['all_queries_retired'])
        with self.assertRaises(ValueError):
            gpuprof.analyze([path], legacy_span_replay=True, frame_close_replay=True)

    def test_frame_close_replay_requires_authenticated_build_and_header_domain(self):
        for change, reason in (
                ({'build_commit': None}, 'unknown-d003f2d-build-authority'),
                ({'build_asset': 'commit another-build\n'}, 'unknown-d003f2d-build-authority'),
                ({'build_identity_source': 'filename'}, 'unknown-d003f2d-build-authority'),
                ({'build_identity_reason': 'unavailable'}, 'unknown-d003f2d-build-authority'),
                ({'sample_budget': 1}, 'not-authenticated-full-mode'),
                ({'timestamp_period_ns': 0}, 'unknown-header-timestamp-domain')):
            with self.subTest(change=change):
                rows = self.frame_close_rows()
                rows[0].update(change)
                report = self.frame_close_replay(rows)
                replay = report['frame_close_replay']
                self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
                self.assertIn(reason, replay['streams'][0]['authority_reasons'])
                if change.get('sample_budget') == 1:
                    # The sampled header contradicts every full-mode data row.
                    # Ordinary validation excludes them before replay; do not
                    # manufacture frame identities from rejected observations.
                    self.assertEqual(report['coverage']['malformed_rows'], 302)
                    self.assertEqual(replay['streams'][0]['rejected_frame_ids'], [])
                    self.assertEqual(replay['frames'], [])
                    self.assertEqual(replay['windows'], [])
                    self.assertIn('unknown-input-malformed_rows', replay['streams'][0]['authority_reasons'])
                else:
                    self.assertEqual(replay['streams'][0]['rejected_frame_ids'], list(range(1, 301)))
                    self.assertFalse(replay['windows'][0]['eligible'])
        rows = self.frame_close_rows()[1:]
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertIn('unknown-build-header', replay['streams'][0]['authority_reasons'])
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
        rows = self.frame_close_rows()
        rows[1]['schema'] = 2
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
        self.assertIn('unknown-input-malformed_rows', replay['streams'][0]['authority_reasons'])

    def test_frame_close_replay_never_corrects_missing_corrupt_or_genuinely_invalid_endpoints(self):
        cases = [
            ('first_tick', None, 'unknown-endpoint-authority'),
            ('last_tick', None, 'unknown-endpoint-authority'),
            ('timestamp_bits', None, 'unknown-endpoint-authority'),
            ('timestamp_period', None, 'unknown-endpoint-authority'),
            ('queue_family', None, 'unknown-endpoint-authority'),
            ('raw_span_ms', None, 'unknown-endpoint-authority'),
            ('timestamp_period', 2, 'timestamp-domain-header-mismatch'),
            ('timestamp_bits', 47, 'timestamp-domain-header-mismatch'),
            ('first_tick', 1 << 48, 'invalid-timestamp-domain'),
            ('last_tick', 19999999, 'backwards-or-wrapped-endpoints'),
            ('last_tick', 10021000000, 'implausible-timestamp'),
            ('raw_span_ms', 13, 'raw-span-endpoint-mismatch'),
            ('last_tick', 29000000, 'negative-full-mode-gap'),
            ('unavailable_timestamps', 1, 'frame-unavailable_timestamps-nonzero'),
            ('invalid_timestamps', 1, 'frame-invalid_timestamps-nonzero'),
            ('timestamp_wraps', 1, 'frame-timestamp_wraps-nonzero'),
            ('dropped', 1, 'frame-dropped-nonzero'),
            ('span_reason', 'unavailable-timestamp', 'not-incomplete-records-only'),
            ('reconciliation_failed', False, 'unknown-original-reconciliation'),
            ('failed_slots', 1, 'frame-failed_slots-nonzero'),
            ('stats_failed_slots', 1, 'frame-stats_failed_slots-nonzero'),
            ('output_failures', 1, 'frame-output_failures-nonzero'),
            ('background_dropped_slots', 1, 'frame-background_dropped_slots-nonzero'),
            ('background_dropped_closures', 1, 'frame-background_dropped_closures-nonzero'),
        ]
        for key, value, reason in cases:
            with self.subTest(key=key, value=value):
                rows = self.frame_close_rows()
                if value is None:
                    rows[1].pop(key)
                else:
                    rows[1][key] = value
                replay = self.frame_close_replay(rows)['frame_close_replay']
                first = replay['frames'][0]
                self.assertEqual(first['frame'], 1)
                self.assertFalse(first['eligible'])
                self.assertTrue(any(reason in result for result in first['reasons']), first['reasons'])
                self.assertIsNone(first['counterfactual_span_ms'])
                self.assertEqual(replay['streams'][0]['eligible_frame_ids'], list(range(2, 301)))
                self.assertEqual(replay['streams'][0]['rejected_frame_ids'], [1])
                self.assertFalse(replay['windows'][0]['eligible'])
                self.assertIn('contains-rejected-frames', replay['windows'][0]['reasons'])

    def test_frame_close_replay_requires_explicit_zero_window_loss_and_complete_sums(self):
        for key in ('dropped', 'failed_slots', 'stats_failed_slots', 'unavailable_timestamps',
                    'invalid_timestamps', 'output_failures', 'background_dropped_slots',
                    'background_dropped_closures'):
            for missing in (False, True):
                if missing and key == 'background_dropped_closures':
                    continue  # This counter did not exist in d003f2d; the documented sole exception.
                with self.subTest(key=key, missing=missing):
                    rows = self.frame_close_rows()
                    if missing:
                        rows[301].pop(key)
                    else:
                        rows[301][key] = 1
                    report = self.frame_close_replay(rows)
                    replay = report['frame_close_replay']
                    self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
                    if missing and key == 'dropped':
                        # dropped is mandatory in the ordinary schema: its
                        # absence removes the summary, not merely its authority.
                        self.assertEqual(report['coverage']['malformed_rows'], 1)
                        self.assertEqual(replay['windows'], [])
                        self.assertEqual(replay['streams'][0]['rejected_frame_ids'], list(range(1, 301)))
                        self.assertIn('unknown-input-malformed_rows', replay['streams'][0]['authority_reasons'])
                        for result in replay['frames']:
                            self.assertIn('unknown-window-summary', result['reasons'])
                    else:
                        self.assertFalse(replay['windows'][0]['eligible'])
                        reason = f'unknown-window-{key}' if missing else f'window-{key}-nonzero'
                        self.assertIn(reason, replay['windows'][0]['reasons'])
        for key, value, reason in (
                ('partial', True, 'incomplete-or-noncanonical-window'),
                ('frames', 299, 'incomplete-or-noncanonical-window'),
                ('invalid_spans', 299, 'window-span-count-mismatch'),
                ('valid_span_ms', 1, 'window-valid_span_ms-mismatch'),
                ('valid_gap_ms', 1, 'window-valid_gap_ms-mismatch'),
                ('measured_ms', 3001, 'frame-window-measured-mismatch')):
            with self.subTest(key=key):
                rows = self.frame_close_rows()
                rows[301][key] = value
                replay = self.frame_close_replay(rows)['frame_close_replay']
                self.assertIn(reason, replay['windows'][0]['reasons'])
                self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
        rows = self.frame_close_rows()
        rows[302]['gpu_ms'] = 2999
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertIn('pass-window-measured-mismatch', replay['windows'][0]['reasons'])
        rows = self.frame_close_rows()
        del rows[302]
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertIn('unknown-pass-coverage', replay['windows'][0]['reasons'])

    def test_frame_close_replay_rejects_missing_frames_uncovered_tail_and_footer_loss(self):
        rows = self.frame_close_rows()
        del rows[150]
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertEqual(replay['windows'][0]['missing_frame_ids'], [150])
        self.assertIn('missing-frame-ids', replay['windows'][0]['reasons'])
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
        rows = self.frame_close_rows()
        tail = dict(rows[300], frame=301)
        rows.insert(301, tail)
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], list(range(1, 301)))
        self.assertEqual(replay['streams'][0]['rejected_frame_ids'], [301])
        self.assertIn('unknown-window-summary', replay['frames'][-1]['reasons'])
        for key in ('output_failures', 'background_dropped_slots', 'background_dropped_closures'):
            with self.subTest(footer_loss=key):
                rows = self.frame_close_rows()
                rows[-1][key] = 1
                replay = self.frame_close_replay(rows)['frame_close_replay']
                self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
                self.assertIn('session-end-reports-loss', replay['streams'][0]['authority_reasons'])

    def test_frame_close_replay_rejects_malformed_queue_domains_without_exception(self):
        for value in (-1, 1.5, True, [], {}, gpuprof.UINT32_MAX + 1):
            with self.subTest(queue_family=value):
                rows = self.frame_close_rows()
                rows[1]['queue_family'] = value
                replay = self.frame_close_replay(rows)['frame_close_replay']
                self.assertIn(1, replay['streams'][0]['rejected_frame_ids'])
                self.assertFalse(replay['frames'][0]['eligible'])
                self.assertTrue(any('endpoint-authority' in reason or 'invalid-timestamp-domain' in reason
                                    for reason in replay['frames'][0]['reasons']))
                self.assertFalse(replay['windows'][0]['eligible'])
        rows = self.frame_close_rows()
        rows[0]['build_asset'] = None
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
        self.assertIn('unknown-d003f2d-build-authority', replay['streams'][0]['authority_reasons'])
        rows = self.frame_close_rows()
        rows[-1]['launch_id'] = 'unrelated-capture'
        replay = self.frame_close_replay(rows)['frame_close_replay']
        self.assertEqual(replay['streams'][0]['eligible_frame_ids'], [])
        self.assertIn('inconsistent-session-launch', replay['streams'][0]['authority_reasons'])


@unittest.skipUnless(os.environ.get('GPUPROF_TEST_BIN'), 'set GPUPROF_TEST_BIN for the host worker fixture')
class CompactSink(unittest.TestCase):
    def test_report_snapshot_and_bounded_pump_contract(self):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        result = subprocess.run([str(binary), '--compact-contract'], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r'gpuprof-compact-contract checks=\d+ failures=0')

    def test_sink_is_bounded_nonblocking_and_preserves_bytes_across_wrap(self):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        result = subprocess.run([str(binary), '--sink-contract'], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('failures=0 capacity_bytes=1048576 peak_bytes=1048576 overflows=1 output_failures=1',
                      result.stdout)
        self.assertIn('bytes_checked=2097252', result.stdout)

    def test_device_rate_gpu_holds_and_writer_stall_do_not_block_lease_consumer(self):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        for coupled in (1, 0):
            with self.subTest(coupled=coupled), tempfile.TemporaryDirectory(dir=binary.parent) as temporary:
                env = dict(os.environ, TOTK_VK_GPU_PROFILE_PIPELINES_RAW='0')
                args = ['--compact-fixture', '--frames', '330', '--fps', '30', '--pace', '1',
                        '--parts', '18', '--gpu-held', '8', '--nonblocking', '1',
                        '--writer-stall-ms', '200', '--coupled-stall', str(coupled), '--sample-budget', '2',
                        '--spread-percent', '60']
                run = subprocess.run([str(binary), *args], cwd=temporary, env=env,
                                     capture_output=True, text=True, timeout=40)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                report = gpuprof.analyze([str(Path(temporary) / 'gpuprof-*.jsonl*')])
                footer = report['sessions'][0]['end']
                self.assertTrue(footer['clean_end'])
                self.assertEqual(footer['output_sink_overflows'], 0)
                self.assertLessEqual(footer['output_sink_peak_bytes'], 1 << 20)
                if coupled:
                    self.assertGreater(footer['background_dropped_slots'], 0)
                    self.assertEqual(footer['background_dropped_inflight_only'], 0)
                    self.assertEqual(footer['background_dropped_with_worker_backlog'],
                                     footer['background_dropped_slots'])
                    self.assertTrue(footer['background_drop_frames'])
                    self.assertTrue(all(300 <= d[0] <= 330 and d[2] >= 15
                                        for d in footer['background_drop_frames']))
                    self.assertTrue(any(d[5] & 16 for d in footer['background_drop_frames']))
                    self.assertTrue(report['pipelines']['censored'])
                else:
                    self.assertEqual(footer['background_dropped_slots'], 0)
                    self.assertEqual(footer['pending_frame_closures'], 0)
                    self.assertEqual(footer['background_drop_frames'], [])
                    self.assertEqual(report['coverage']['streams_without_end'], 0)
                    self.assertEqual(report['pipelines']['coverage']['invalid_coverage'], 0)
                    self.assertFalse(report['pipelines']['censored'], report['pipelines']['censor_reasons'])

    def test_pipeline_endpoint_retirement_counts_only_available_activated_queries(self):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        for raw in (0, 1):
            for budget in (0, 1):
                for unavailable in (0, 2):
                    with self.subTest(raw=raw, budget=budget, unavailable=unavailable), \
                            tempfile.TemporaryDirectory(dir=binary.parent) as temporary:
                        env = dict(os.environ, TOTK_VK_GPU_PROFILE_PIPELINES_RAW=str(raw))
                        env.pop('TOTK_VK_GPU_PROFILE_MAX_BYTES', None)
                        args = ['--compact-fixture', '--frames', '2', '--parts', '2',
                                '--sample-budget', str(budget), '--unavailable-frame', str(unavailable)]
                        run = subprocess.run([str(binary), *args], cwd=temporary, env=env,
                                             capture_output=True, text=True, timeout=30)
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        rows = [json.loads(line) for path in sorted(Path(temporary).glob('gpuprof-*.jsonl*'))
                                for line in path.read_text(encoding='utf-8').splitlines()]
                        footers = [row for row in rows if row['type'] == 'session_end']
                        self.assertEqual(len(footers), 1)
                        footer = footers[0]
                        counts = footer['query_retirement']
                        # Two frames, two leases/frame. Full parent: 2 pass +
                        # 2 span endpoints/lease; sampled: 2 pass + 2 span/frame.
                        # Each frame also activates four pipeline query pairs.
                        submitted = 2 * ((4 if budget else 8) + 8)
                        missing = 4 if unavailable else 0
                        self.assertEqual(counts, dict(
                            leases_acquired=4, leases_submitted=4, leases_retired=4,
                            leases_cancelled=0, leases_outstanding=0,
                            queries_submitted=submitted, queries_retired=submitted - missing,
                            queries_cancelled=0, queries_outstanding=missing))
                        self.assertTrue(footer['clean_end'])
                        self.assertFalse(footer['query_drain_timed_out'])
                        self.assertIs(footer['all_queries_retired'], not unavailable)
                        self.assertEqual(rows[-1], footer, 'writer closure follows all deferred rows')

    def test_deferred_pipeline_reports_drain_complete_and_partial_windows_before_footer(self):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        for frames in (330, 600):
            with self.subTest(frames=frames), tempfile.TemporaryDirectory(dir=binary.parent) as temporary:
                env = dict(os.environ, TOTK_VK_GPU_PROFILE_PIPELINES_RAW='0')
                env.pop('TOTK_VK_GPU_PROFILE_MAX_BYTES', None)
                args = ['--compact-fixture', '--frames', str(frames), '--sample-budget', '1']
                run = subprocess.run([str(binary), *args], cwd=temporary, env=env,
                                     capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                paths = sorted(Path(temporary).glob('gpuprof-*.jsonl*'))
                rows = [json.loads(line) for path in paths
                        for line in path.read_text(encoding='utf-8').splitlines()]
                coverage = [r for r in rows if r['type'] == 'pipeline_window_coverage']
                self.assertEqual([(r['window_start'], r['window_end'], r['frames']) for r in coverage],
                                 [(1, 300, 300), (301, frames, frames - 300)])
                for row in coverage:
                    self.assertEqual(row['selected_groups'], row['frames'] * 4)
                    self.assertEqual(row['valid_samples'], row['frames'] * 4)
                    self.assertEqual(row['timestamp_count'], row['frames'] * 8)
                    self.assertEqual(row['missing_selected'], 0)
                    groups = [r for r in rows if r['type'] == 'pipeline_window' and
                              r['window_start'] == row['window_start']]
                    self.assertTrue(groups, 'snapshot rows cannot disappear at the next report or shutdown')
                    self.assertEqual(sum(r['v'][gpuprof.PIPELINE_WINDOW_COLUMNS.index('selected_groups')]
                                         for r in groups), row['selected_groups'])
                    self.assertEqual(sum(r['v'][gpuprof.PIPELINE_WINDOW_COLUMNS.index('valid_samples')]
                                         for r in groups), row['valid_samples'])
                self.assertEqual(rows[-1]['type'], 'session_end')
                self.assertTrue(rows[-1]['clean_end'])
                self.assertTrue(rows[-1]['all_queries_retired'])
                self.assertEqual(rows[-1]['last_window_end'], frames)
                self.assertEqual(rows[-1]['output_failures'], 0)


@unittest.skipUnless(os.environ.get('GPUPROF_TEST_BIN'), 'set GPUPROF_TEST_BIN to run the real Vulkan fixture')
class Lavapipe(unittest.TestCase):
    def launch(self, mode, workers, stats, *, unsupported=False, rotate=False, areas=False, ordinals=None,
               unavailable_span=False, sampling=False, sample_budget=None, backpressure=False, cancel=False,
               sealed_present=False, last_closed=False, shutdown_inflight=False, shutdown_timeout=False,
               pipeline_budget=None, pipeline_groups=False, pipeline_overflow=False, pipeline_raw=None):
        binary = Path(os.environ['GPUPROF_TEST_BIN']).resolve()
        self.assertTrue(binary.is_file(), f'no fixture binary: {binary}')
        seed = Path(os.environ.get('TOTK_SHADER_SEED_DIR', binary.parent / 'vkdraw-seed')).resolve()
        self.assertTrue(seed.is_dir(), f'no shader seed: {seed}')
        with tempfile.TemporaryDirectory(prefix='gpuprof-test-') as temporary:
            env = dict(os.environ, TOTK_SHADER_SEED_DIR=str(seed), TOTK_VK_GPU_PROFILE=str(mode),
                       TOTK_VK_REC_THREADS=str(workers), TOTK_VK_GPU_PROFILE_STATS=str(stats),
                       TOTK_VK_GPU_PROFILE_LONG_MS='0', TOTK_VK_GPU_PROFILE_LONG_FRAMES='2',
                       TOTK_VK_GPU_PROFILE_MAX_BYTES='4096' if rotate else '134217728',
                       TOTK_RENDER_RES='1280x720')
            if mode is None:
                env.pop('TOTK_VK_GPU_PROFILE', None)  # exercise the true launch-time default
            # Isolate launch policy from a developer's shell diagnostics.
            for key in ('TOTK_VK_OCCLUSION', 'TOTK_VK_PASS_DUMP', 'TOTK_VK_PASS_PROFILE', 'TOTK_VK_PASSPROBE',
                        'TOTK_VK_PASS_MERGE', 'GPUPROF_TEST_UNSUPPORTED', 'TOTK_VK_OFF', 'TOTK_VK_WRITEBACK',
                        'TOTK_FRAME_LAG', 'TOTK_VK_LAG_CAPTURE', 'TOTK_VK_SYNC'):
                env.pop(key, None)
            env.pop('TOTK_VK_GPU_PROFILE_ORDINALS', None)
            env.pop('TOTK_VK_GPU_PROFILE_SAMPLE', None)
            env.pop('TOTK_VK_GPU_PROFILE_PIPELINES', None)
            env.pop('TOTK_VK_GPU_PROFILE_PIPELINES_RAW', None)
            if pipeline_budget is not None:
                env['TOTK_VK_GPU_PROFILE_PIPELINES'] = str(pipeline_budget)
            if pipeline_raw is not None:
                env['TOTK_VK_GPU_PROFILE_PIPELINES_RAW'] = str(int(pipeline_raw))
            if sample_budget is not None:
                env['TOTK_VK_GPU_PROFILE_SAMPLE'] = str(sample_budget)
            if ordinals is not None:
                env['TOTK_VK_GPU_PROFILE_ORDINALS'] = str(ordinals)
            if not rotate:
                env.pop('TOTK_VK_GPU_PROFILE_MAX_BYTES', None)  # exercise the actual 128 MiB default
            if unsupported:
                env['GPUPROF_TEST_UNSUPPORTED'] = '1'
            arguments = (['--unavailable-span'] if unavailable_span else ['--areas'] if areas else
                         ['--backpressure'] if backpressure else ['--cancel'] if cancel else
                         ['--sealed-present'] if sealed_present else ['--last-closed'] if last_closed else
                         ['--shutdown-timeout'] if shutdown_timeout else
                         ['--shutdown-inflight'] if shutdown_inflight else
                         ['--pipeline-groups'] if pipeline_groups else ['--pipeline-overflow'] if pipeline_overflow else
                         ['--sampling'] if sampling else ['--frames', '302'])
            result = subprocess.run([str(binary), *arguments], cwd=temporary, env=env,
                                    capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            match = re.search(r'gpuprof-fixture frames=(\d+) passes=(\d+) draws=(\d+) mode=(\d+) workers=(\d+) '
                              r'stats_supported=(\d+) stats_enabled=(\d+)', result.stdout)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            frames, passes, draws, effective_mode, _, supported, enabled = map(int, match.groups())
            paths = sorted(Path(temporary).glob('gpuprof-*.jsonl*'))
            if mode not in (1, 2, 3):
                self.assertEqual(effective_mode, 0)
                self.assertEqual(enabled, 0, 'statistics subflag cannot enable a disabled master profiler')
                self.assertIn('[gpu-profile] disabled (TOTK_VK_GPU_PROFILE=', result.stderr)
                self.assertIn('[gpu-profile] pipeline-budget=0 ', result.stderr)
                self.assertEqual(paths, [], 'off mode must not create a profiler output file')
                if shutdown_inflight or shutdown_timeout:
                    self.assertEqual((frames, passes, draws), (1, 2, 4))
                    self.assertIn('gpuprof-shutdown-inflight pending_before=1 drained_after=0', result.stdout)
                return None
            self.assertTrue(paths, result.stdout + result.stderr)
            report = gpuprof.analyze(paths)
            self.assertEqual(report['coverage']['malformed_rows'], 0, report['issues'])
            self.assertEqual(report['coverage']['truncated_rows'], 0, report['issues'])
            self.assertEqual(report['coverage']['conflicting_rows'], 0, report['issues'])
            observations, _, _ = gpuprof.load(gpuprof.inputs(paths))
            self.assertIn(f'window ordinals={int(bool(ordinals))}', result.stderr)
            self.assertEqual(report['coverage']['ordinal_metadata_mismatches'], 0)
            self.assertEqual(report['coverage']['streams_without_end'], 0)
            self.assertEqual(len(report['sessions']), 1)
            session = report['sessions'][0]
            self.assertTrue(session['clean_end'])
            self.assertIs(session['all_queries_retired'], not (unavailable_span or shutdown_timeout))
            self.assertIs(session['query_drain_timed_out'], shutdown_timeout)
            retirement = session['query_retirement']
            self.assertIsInstance(retirement, dict, 'new fixture footers must carry retirement accounting')
            for key in gpuprof.RETIREMENT_COUNTS:
                self.assertIs(type(retirement[key]), int, key)
                self.assertGreaterEqual(retirement[key], 0, key)
            self.assertEqual(retirement['leases_acquired'], retirement['leases_retired'] +
                             retirement['leases_cancelled'] + retirement['leases_outstanding'])
            self.assertGreaterEqual(retirement['leases_submitted'], retirement['leases_retired'])
            self.assertLessEqual(retirement['leases_submitted'],
                                 retirement['leases_acquired'] - retirement['leases_cancelled'])
            if shutdown_timeout:
                self.assertGreater(retirement['leases_outstanding'], 0)
                self.assertGreater(retirement['leases_submitted'], retirement['leases_retired'])
            else:
                self.assertEqual(retirement['leases_submitted'], retirement['leases_retired'])
                self.assertEqual(retirement['leases_outstanding'], 0)
            self.assertGreater(retirement['leases_submitted'], 0)
            self.assertGreater(retirement['queries_submitted'], 0)
            self.assertEqual(retirement['queries_outstanding'],
                             retirement['queries_submitted'] - retirement['queries_retired'])
            if unavailable_span or shutdown_timeout:
                self.assertGreater(retirement['queries_outstanding'], 0)
            else:
                self.assertEqual(retirement['queries_outstanding'], 0)
            if cancel:
                self.assertGreater(retirement['leases_cancelled'], 0)
                self.assertGreater(retirement['queries_cancelled'], 0)
            report['_fixture_output_bytes'] = sum(path.stat().st_size for path in paths)
            report['_fixture_rows'] = [o['row'] for o in observations]
            for path in paths:
                file_rows = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
                first = file_rows[0]
                for index, row in enumerate(file_rows):
                    if row['type'] == 'session_end':
                        self.assertEqual(index, len(file_rows) - 1, 'late CPU present cannot emit rows after session_end')
                self.assertEqual(first['type'], 'header', 'every rotated file must carry its launch identity')
                self.assertEqual(first['ordinals'], bool(ordinals))
                self.assertEqual(first['max_bytes'], 4096 if rotate else 134217728)
                self.assertEqual(first['span_limit_ms'], 10000)
                self.assertEqual(first['background_slots'], 32)
                self.assertEqual(first['background_worker'], 'totk-gpuprof')
                self.assertEqual(first['background_closures'], 256)
                budget = sample_budget or 0
                self.assertEqual(first['parent_query_count_per_lease'],
                                 (budget * 2 * 64 if budget else 4096) + 130)
                pipe_budget = pipeline_budget if isinstance(pipeline_budget, int) and 1 <= pipeline_budget <= 16 else 0
                self.assertEqual(first['pipeline_query_count_per_lease'], pipe_budget * 2 * 64)
                self.assertEqual(first['pipeline_record_capacity'], 1024 + pipe_budget * 64 if pipe_budget else 0)
                self.assertGreaterEqual(first['worker_effective_nice'], -20)
                self.assertLessEqual(first['worker_effective_nice'], 19)
                self.assertTrue(first['worker_affinity'])
                if sample_budget:
                    self.assertEqual(first['sample_budget'], sample_budget)
                    self.assertEqual(first['env']['TOTK_VK_GPU_PROFILE_SAMPLE'], str(sample_budget))
            if mode == 2:
                raw_ordinals = re.findall(r'\[gpu-split\] frame=\d+ ordinal=(\d+)', result.stderr)
                self.assertTrue(raw_ordinals)
                self.assertTrue(all(int(ordinal) > 0 for ordinal in raw_ordinals))
            for observation in observations:
                row = observation['row']
                self.assertEqual(row['schema'], 3)
                if row['type'] in ('pass', 'long_frame'):
                    if row['type'] == 'pass':
                        self.assertEqual(row['ordinal'] > 0, bool(ordinals))
                    else:
                        self.assertGreater(row['ordinal'], 0, 'selected raw frames keep their actual ordinals')
                    for axis in ('width', 'height'):
                        self.assertGreater(row[f'reference_{axis}'], 0)
                        self.assertLessEqual(row[f'reference_{axis}'], gpuprof.UINT32_MAX)
                        self.assertEqual(row[f'render_{axis}'] > 0, row['kind'] == 'render')
                        for attachment in row['attachments']:
                            self.assertEqual(attachment[f'{axis}_num'] * row[f'reference_{axis}'],
                                             attachment[axis] * attachment[f'{axis}_den'])
                    self.assertIsInstance(row['scene_relative'], bool)
                elif row['type'] == 'window':
                    self.assertEqual(row['ordinals'], bool(ordinals))
                    self.assertEqual(row['sequence'], (row['window_start'] - 1) // 300 + 1)
            if shutdown_timeout:
                self.assertEqual((frames, passes, draws), (1, 2, 4))
                self.assertIn('gpuprof-shutdown-inflight pending_before=1 drained_after=0', result.stdout)
                self.assertEqual(retirement['leases_cancelled'], 0)
                self.assertEqual(retirement['queries_cancelled'], 0)
                return report
            if unavailable_span:
                self.assertEqual(frames, 1)
                frame_rows = [o['row'] for o in observations if o['row']['type'] == 'frame']
                windows = [o['row'] for o in observations if o['row']['type'] == 'window']
                self.assertEqual(len(frame_rows), 1)
                self.assertEqual(len(windows), 1)
                for row in (frame_rows[0], windows[0]):
                    self.assertFalse(row['span_valid'])
                    self.assertIsNone(row['span_ms'])
                    self.assertIsNone(row['gap_ms'])
                    self.assertGreater(row['unavailable_timestamps'], 0)
                self.assertIsNone(frame_rows[0]['raw_span_ms'])
                self.assertEqual(frame_rows[0]['span_reason'], 'unavailable-timestamp')
                self.assertTrue(frame_rows[0]['reconciliation_failed'])
                self.assertEqual(windows[0]['invalid_spans'], 1)
                self.assertGreater(windows[0]['failed_slots'], 0)
                self.assertEqual(windows[0]['valid_span_ms'], 0)
                self.assertEqual(report['coverage']['invalid_frame_spans'], 1)
                self.assertEqual(report['coverage']['invalid_window_spans'], 1)
                self.assertEqual(report['sessions'][0]['end']['last_frame'], 1)
                return report
            if areas:
                self.assertEqual((frames, passes, draws), (3, 11, 0))
                self.assertEqual(bool(enabled), bool(stats and supported and not unsupported))
                self.check_areas(report, observations, result.stdout, bool(enabled))
                return report
            if backpressure:
                self.assertEqual(frames, 42)
                self.assertIn('gpuprof-backpressure frames=40 queued=32 processed=0', result.stdout)
                self.assertGreater(report['coverage']['background_dropped_slots'], 0)
                self.assertLess(report['coverage']['frame_rows'], frames)
                self.assertGreater(report['coverage']['missing_frame_ids'], 0)
                self.assertTrue(any('background backlog' in issue for issue in report['issues']))
                self.assertGreater(report['coverage']['invalid_frame_spans'] + report['coverage']['missing_frame_ids'], 0,
                                   'actual backlog loss must not be reclassified as an ordinary CPU-closed frame')
                self.assertTrue(any(not row['span_valid'] or row['dropped'] for row in report['_fixture_rows']
                                    if row['type'] == 'frame') or report['coverage']['missing_frame_ids'])
                return report
            self.assertEqual(report['coverage']['background_dropped_slots'], 0, report['issues'])
            self.assertEqual(report['coverage']['background_dropped_closures'], 0, report['issues'])
            self.assertEqual(report['sessions'][0]['end']['pending_frame_closures'], 0)
            if 'pipelines' in report and observations[0]['row'].get('pipeline_output') == 'compact':
                self.check_compact_pipeline(report, frames, draws)
                return report
            if sealed_present or last_closed or shutdown_inflight:
                self.assertEqual(frames, 1 if last_closed or shutdown_inflight else 302)
                self.assertEqual(bool(enabled), bool(stats and supported and not unsupported))
                self.check_sealed_present(report, observations, result.stdout, frames, passes, draws,
                                          sample_budget or 0, bool(enabled), shutdown_inflight=shutdown_inflight)
                if shutdown_inflight:
                    self.assertEqual(retirement['leases_cancelled'], 0)
                    self.assertEqual(retirement['queries_cancelled'], 0)
                    self.assertEqual(report['coverage']['long_frames'], frames if mode == 3 else 0)
                return report
            if cancel:
                self.assertEqual(frames, 6)
                self.assertEqual(report['coverage']['frame_rows'], frames)
                self.assertEqual(report['coverage']['invalid_frame_spans'], 0, report['issues'])
                self.assertEqual(report['coverage']['failed_slots'], 0, report['issues'])
                self.assertEqual(sum(g['count'] for g in report['passes'] if g['kind'] == 'render'), passes)
                return report
            if rotate:
                self.assertEqual({p.suffix for p in paths}, {'.jsonl', '.1', '.2', '.3'})
                self.assertTrue(all(p.stat().st_size <= 4096 for p in paths))
                self.assertLess(report['coverage']['frame_rows'], frames, 'bounded rotation must discard old rows')
                self.assertGreater(report['coverage']['rotation_evictions'], 0)
                return report
            if sample_budget:
                self.check_sampling(report, observations, frames, passes, draws, sample_budget,
                                    bool(enabled), mode)
                return report
            if pipeline_overflow:
                self.assertEqual(report['coverage']['frame_rows'], frames)
                self.assertEqual(report['coverage']['windows'], 1)
                self.assertEqual(sum(g['draws'] for g in report['passes'] if g['kind'] == 'render'), draws)
                return report
            self.assertEqual(report['coverage']['frame_rows'], frames)
            self.assertEqual(report['coverage']['windows'], 2)
            self.assertEqual(report['coverage']['window_sum_mismatches'], 0, report['issues'])
            self.assertEqual(report['coverage']['missing_frame_ids'], 0)
            self.assertEqual(report['coverage']['reconciliation_failures'], 0, report['issues'])
            self.assertEqual(report['coverage']['dropped'], 0)
            self.assertEqual(report['coverage']['failed_slots'], 0)
            render_groups = [g for g in report['passes'] if g['kind'] == 'render']
            other_groups = [g for g in report['passes'] if g['kind'] != 'render']
            self.assertEqual(sum(g['count'] for g in render_groups), passes)
            self.assertEqual(sum(g['draws'] for g in render_groups), draws)
            self.assertGreater(sum(g['gpu_ms'] for g in render_groups), 0)
            self.assertEqual(len({g['key'] for g in render_groups}), 2,
                             'absolute 64/128 extents must have different exact render keys')
            self.assertEqual({(g['render_width'], g['render_height']) for g in render_groups},
                             {(64, 64), (128, 128)})
            render_families = [g for g in report['families'] if g['kind'] == 'render']
            self.assertEqual(len(render_families), 2, 'color-only scopes retain absolute family dimensions')
            self.assertGreaterEqual(sum(g['count'] for g in other_groups if g['kind'] == 'transfer'), passes,
                                    'each real readback must have a measured transfer scope')
            self.assertTrue(any(g['kind'] == 'upload' for g in other_groups),
                            'direct pre upload while a main record is open must survive')
            self.assertAlmostEqual(report['timing']['window']['measured_ms'],
                                   report['timing']['frame']['measured_ms'], delta=0.01,
                                   msg='all measured kinds must reconcile, not only render rows')
            self.assertEqual(bool(enabled), bool(stats and supported and not unsupported))
            for observation in observations:
                row = observation['row']
                if row['type'] in ('pass', 'long_frame'):
                    self.assertEqual((row['reference_width'], row['reference_height']), (1280, 720))
                    self.assertFalse(row['scene_relative'])
                    available = bool(enabled and row['kind'] == 'render')
                    self.assertEqual('stats' in row, available)
                    self.assertEqual(row['stats_valid'], row['count'] if available else 0)
                    if available:
                        self.assertEqual(row['stats']['ia_vertices'], row['draws'] * 3)
                        self.assertEqual(row['stats']['ia_primitives'], row['draws'])
                        self.assertGreater(row['stats']['fs_invocations'], 0)
                if row['type'] == 'frame':
                    self.assertFalse(row['reconciliation_failed'])
                    self.assertEqual(row['dropped'], 0)
                    self.assertEqual((row['scene_width'], row['scene_height']), (0, 0))
            missing = sum(g['count'] for g in other_groups) + (0 if enabled else passes)
            self.assertEqual(report['coverage']['stats_missing'], missing)
            if mode == 3:
                self.assertEqual(report['coverage']['long_frames'], frames,
                                 'threshold zero must dump every frame, independent of history ring retention')
            else:
                self.assertEqual(report['coverage']['long_frames'], 0)
            return report

    def test_pipeline_groups_direct_and_deferred(self):
        for workers in (0, 2):
            with self.subTest(workers=workers):
                report = self.launch(1, workers, 0, pipeline_budget=16, pipeline_groups=True, pipeline_raw=True)
                rows = [r for r in report['_fixture_rows'] if r['type'] == 'pipeline']
                first = [r for r in rows if r['frame'] == 1 and r['kind'] == 'render']
                self.assertEqual([r['draws'] for r in first], [3, 2, 4])
                self.assertEqual(first[0]['pipeline'], first[2]['pipeline'])
                self.assertNotEqual(first[0]['pipeline'], first[1]['pipeline'])
                self.assertEqual({(r['vs'], r['fs']) for r in first}, {(first[0]['vs'], first[0]['fs'])})
                self.assertTrue(all(r['primitives'] == r['draws'] for r in rows if r['kind'] == 'render'))
                continued = [r for r in rows if r['frame'] == 303 and r['kind'] == 'render']
                self.assertEqual([r['draws'] for r in continued], [2],
                                 'nested pre upload cannot terminate the main pipeline group')
                compute = [r for r in rows if r['kind'] == 'compute']
                self.assertTrue(compute)
                self.assertTrue(all(r['dispatches'] == 1 and r['draws'] == 0 and r['primitives'] is None and
                                    r['cs'] and r['vs'] is None and r['fs'] is None for r in compute))
                self.assertTrue(all(r['timestamp_count'] == (2 if r['selected'] else 0) for r in rows))
                self.assertTrue(all(r['status'] == ('ok' if r['selected'] else 'not-selected') for r in rows))
                self.assertGreater(sum(r['selected'] for r in rows), 0, 'rotation reaches groups across passes')
                coverage = report['pipelines']['coverage']
                self.assertEqual(coverage['invalid_coverage'], 0)
                self.assertEqual(coverage['dropped_groups'], 0)
                self.assertEqual(sum(r['draws'] for r in rows),
                                 sum(g['draws'] for g in report['passes'] if g['kind'] == 'render'))

    def test_pipeline_sealed_deferred_ownership(self):
        report = self.launch(1, 2, 0, pipeline_budget=16, sealed_present=True, pipeline_raw=True)
        rows = [r for r in report['_fixture_rows'] if r['type'] == 'pipeline']
        self.assertTrue(rows)
        self.assertEqual(report['pipelines']['coverage']['invalid_coverage'], 0)
        self.assertTrue(all(r['timestamp_count'] == (2 if r['selected'] else 0) for r in rows))
        self.assertTrue(all(r['status'] == ('ok' if r['selected'] else 'not-selected') for r in rows))

    def test_pipeline_metadata_bound_is_explicit(self):
        report = self.launch(1, 2, 0, pipeline_budget=16, pipeline_overflow=True, pipeline_raw=True)
        self.assertGreater(report['pipelines']['coverage']['unretained_groups'], 0)
        self.assertEqual(report['pipelines']['coverage']['dropped_groups'], 0)
        self.assertEqual(report['pipelines']['coverage']['invalid_coverage'], 0)
        coverage = [r for r in report['_fixture_rows'] if r['type'] == 'pipeline_coverage']
        self.assertTrue(any(r['frame'] == 2 and r['population'] == 4096 for r in coverage))
        self.assertTrue(all(g['estimated_gpu_ms'] is None for g in report['pipelines']['shader_pairs']))

    def check_compact_pipeline(self, report, frames, draws):
        rows = report['_fixture_rows']
        self.assertFalse(any(r['type'] in ('pipeline', 'pipeline_coverage', 'frame') for r in rows),
                         'default pipeline compact mode suppresses per-group and global per-frame output')
        headers = [r for r in rows if r['type'] == 'header']
        self.assertTrue(all(r['pipeline_output'] == 'compact' and r['pipeline_compact_version'] == 1 for r in headers))
        self.assertTrue(all(r['pipeline_window_columns'] == list(gpuprof.PIPELINE_WINDOW_COLUMNS) for r in headers))
        coverage = [r for r in rows if r['type'] == 'pipeline_window_coverage']
        self.assertEqual(sum(r['frames'] for r in coverage), frames)
        self.assertEqual(len(coverage), (frames + 299) // 300)
        self.assertEqual(report['coverage']['frame_rows'], 0)
        self.assertEqual(report['coverage']['missing_frame_ids'], 0)
        self.assertEqual(report['coverage']['window_sum_mismatches'], 0, report['issues'])
        for key in ('invalid_coverage', 'missing_dictionary', 'missing_headers', 'missing_compact_windows',
                    'missing_compact_frames', 'dropped_groups', 'missing_selected'):
            self.assertEqual(report['pipelines']['coverage'][key], 0, key)
        self.assertTrue(any(r['type'] == 'pipeline_key' for r in rows))
        self.assertTrue(any(r['type'] == 'pipeline_window' for r in rows))
        self.assertGreater(sum(g['samples'] for g in report['pipelines']['pipelines']), 0)
        self.assertLessEqual(sum(g['draws'] for g in report['pipelines']['pipelines']), draws)
        for g in report['pipelines']['pipelines']:
            self.assertIsNone(g['pass_keys'])
            self.assertEqual(g['thin'], g['eligible_samples'] < 10)
        self.assertIsNone(report['pipelines']['domain_excluded_frames'])
        self.assertTrue(all(r[k] == 0 for r in coverage for k in gpuprof.PIPELINE_COMPACT_LOSS))

    def test_pipeline_default_compact_real_vulkan_groups(self):
        for workers in (0, 2):
            with self.subTest(workers=workers):
                compact = self.launch(1, workers, 0, pipeline_budget=16, pipeline_groups=True)
                raw = self.launch(1, workers, 0, pipeline_budget=16, pipeline_groups=True, pipeline_raw=True)
                self.assertLess(compact['_fixture_output_bytes'], raw['_fixture_output_bytes'])
                for field in ('count', 'draws'):
                    self.assertEqual(sum(g[field] for g in compact['passes']), sum(g[field] for g in raw['passes']))


    def test_pipeline_default_off_and_invalid_budgets(self):
        for value in (None, 0, -1, 17, 'invalid'):
            with self.subTest(value=value):
                report = self.launch(1, 2, 0, pipeline_budget=value, last_closed=True)
                self.assertNotIn('pipelines', report)
        self.launch(0, 2, 0, pipeline_budget=16)

    def test_master_default_off_and_invalid_modes_ignore_pipeline_requests(self):
        for mode in (None, 0, '', 'invalid', -1, 4, '01', '1 '):
            for workers in (0, 2):
                with self.subTest(mode=mode, workers=workers):
                    self.launch(mode, workers, 1, pipeline_budget=16, pipeline_raw=True,
                                sample_budget=2, last_closed=True)


    def check_sealed_present(self, report, observations, stdout, frames, passes, draws, budget, stats_enabled,
                             *, shutdown_inflight=False):
        if shutdown_inflight:
            self.assertIn('gpuprof-shutdown-inflight pending_before=1 drained_after=1', stdout)
        else:
            self.assertIn(
                f'gpuprof-sealed-present frames={frames} main_submissions={frames * 3} pre_uploads={frames * 2} '
                f'completed_closes={(frames + 1) // 2} pending_closes={frames // 2} '
                f'marker_submissions={frames if budget else 0} last_closed_before_flush=1', stdout)
            self.assertEqual(report['sessions'][0]['end']['query_retirement']['leases_cancelled'],
                             frames // 2, 'every GPU-blocked close must cancel its independent lease')
        self.assertEqual((passes, draws), (frames * 2, frames * 4))
        rows = [o['row'] for o in observations]
        frame_rows = [r for r in rows if r['type'] == 'frame']
        pass_rows = [r for r in rows if r['type'] == 'pass']
        windows = [r for r in rows if r['type'] == 'window']
        self.assertEqual([r['frame'] for r in frame_rows], list(range(1, frames + 1)))
        self.assertEqual(len(windows), (frames + 299) // 300)
        for field in ('missing_frame_ids', 'reconciliation_failures', 'dropped', 'failed_slots',
                      'stats_failed_slots', 'invalid_frame_spans', 'invalid_window_spans',
                      'unavailable_timestamps', 'invalid_timestamps', 'window_sum_mismatches',
                      'background_dropped_slots', 'background_dropped_closures'):
            self.assertEqual(report['coverage'][field], 0, (field, report['issues']))
        self.assertEqual(report['sessions'][0]['end']['last_frame'], frames)
        self.assertEqual(report['sessions'][0]['end']['last_window_end'], frames)
        self.assertEqual({kind: sum(r['count'] for r in pass_rows if r['kind'] == kind)
                          for kind in ('render', 'upload', 'transfer', 'present')},
                         dict(render=frames * 2, upload=frames * 2, transfer=frames * 2, present=frames))
        self.assertEqual(sum(r['draws'] for r in pass_rows), draws)
        self.assertEqual({(r['render_width'], r['render_height']) for r in pass_rows if r['kind'] == 'render'},
                         {(64, 64), (128, 128)})
        sequence = 0
        for row in frame_rows:
            self.assertTrue(row['span_valid'], row)
            self.assertFalse(row['reconciliation_failed'])
            self.assertGreater(row['span_ms'], 0)
            self.assertEqual(row['unavailable_timestamps'], 0)
            self.assertEqual(row['invalid_timestamps'], 0)
            self.assertEqual(row['closure_source'], 'gpu-marker' if budget else 'cpu-sealed')
            self.assertGreater(row['closure_sequence'], sequence)
            sequence = row['closure_sequence']
            ticks = (row['last_tick'] - row['first_tick']) & ((1 << row['timestamp_bits']) - 1)
            self.assertAlmostEqual(row['span_ms'], ticks * row['timestamp_period'] / 1e6, delta=0.000002)
            self.assertAlmostEqual(row['gap_ms'], row['span_ms'] - row['measured_ms'], delta=0.000002)
            if budget:
                self.assertEqual(row['sample_budget'], budget)
                self.assertEqual(row['timing_scope'], 'main-frame')
                self.assertEqual(row['observed_passes'], 7)
                self.assertLessEqual(row['sampled_passes'], budget)
                self.assertEqual(row['timestamp_count'], 2 * row['sampled_passes'] + 2,
                                 'count actual profiler timestamp commands, not pool capacity or queue submits')
                self.assertLessEqual(row['timestamp_count'], 2 * budget + 2)
            else:
                self.assertGreaterEqual(row['gap_ms'], -0.000002)
        for row in windows:
            members = [r for r in frame_rows if row['window_start'] <= r['frame'] <= row['window_end']]
            aggregates = [r for r in pass_rows if r['window_start'] == row['window_start']]
            self.assertTrue(row['span_valid'])
            self.assertEqual(row['frames'], len(members))
            self.assertEqual(row['invalid_spans'], 0)
            self.assertAlmostEqual(row['measured_ms'], math.fsum(r['measured_ms'] for r in members), delta=0.00001)
            self.assertAlmostEqual(row['measured_ms'], math.fsum(r['gpu_ms'] for r in aggregates), delta=0.00001,
                                   msg='all actual pre/render/transfer/present pass sums retain the same authority')
            self.assertAlmostEqual(row['span_ms'], math.fsum(r['span_ms'] for r in members), delta=0.00001)
            if budget:
                self.assertEqual(row['timestamp_count'], sum(r['timestamp_count'] for r in members))
                self.assertEqual(row['observed_passes'], sum(r['count'] for r in aggregates))
                self.assertEqual(row['sample_count'], sum(r['sampling']['sample_count'] for r in aggregates))
        for row in pass_rows:
            timed = row['sampling']['sample_count'] if budget else row['count']
            valid_stats = timed if stats_enabled and row['kind'] == 'render' else 0
            self.assertEqual(row['stats_valid'], valid_stats)
            self.assertEqual('stats' in row, bool(valid_stats))
            if valid_stats:
                stats_draws = row['stats_draws'] if budget else row['draws']
                self.assertEqual(row['stats']['ia_vertices'], stats_draws * 3)
                self.assertEqual(row['stats']['ia_primitives'], stats_draws)
                self.assertGreater(row['stats']['fs_invocations'], 0)
        self.assertAlmostEqual(report['timing']['window']['measured_ms'],
                               report['timing']['frame']['measured_ms'], delta=0.00001)

    def test_sealed_present_full_and_sampled_workers_statistics(self):
        fields = ('key', 'kind', 'count', 'draws', 'window_start', 'window_end', 'ordinal',
                  'render_width', 'render_height', 'reference_width', 'reference_height',
                  'scene_relative', 'shader', 'samples', 'attachments')
        def metadata(report):
            return {json.dumps({key: row[key] for key in fields}, sort_keys=True)
                    for row in report['_fixture_rows'] if row['type'] == 'pass'}
        reference = None
        for workers in (0, 2):
            for stats in (0, 1):
                for budget in (0, 1, 2):
                    with self.subTest(workers=workers, stats=stats, budget=budget):
                        report = self.launch(1, workers, stats, sealed_present=True, sample_budget=budget)
                        observed = metadata(report)
                        if reference is None:
                            reference = observed
                        self.assertEqual(observed, reference,
                                         'CPU closure, workers and sampling cannot change observed pass authority')

    def test_last_closed_frame_publishes_without_next_frame_or_terminal_flush(self):
        for workers in (0, 2):
            for stats in (0, 1):
                for budget in (0, 1, 2):
                    with self.subTest(workers=workers, stats=stats, budget=budget):
                        self.launch(1, workers, stats, last_closed=True, sample_budget=budget)

    def test_shutdown_drains_genuinely_inflight_gpu_work_before_closing_writer(self):
        for mode in (1, 3):
            for workers in (0, 2):
                for stats in (0, 1):
                    for budget in (0, 2):
                        with self.subTest(mode=mode, workers=workers, stats=stats, budget=budget):
                            report = self.launch(mode, workers, stats, shutdown_inflight=True, sample_budget=budget)
                            retirement = report['sessions'][0]['query_retirement']
                            self.assertEqual(retirement['leases_acquired'], retirement['leases_submitted'])
                            self.assertEqual(retirement['queries_submitted'], retirement['queries_retired'])

    def test_disabled_shutdown_does_not_wait_for_pending_renderer_work(self):
        for workers in (0, 2):
            for budget in (0, 2):
                for mode, pipeline_budget in ((None, 16), (0, None)):
                    with self.subTest(workers=workers, budget=budget, mode=mode):
                        self.launch(mode, workers, 1, shutdown_inflight=True, sample_budget=budget,
                                    pipeline_budget=pipeline_budget)

    def test_shutdown_timeout_preserves_unretired_work_in_clean_footer(self):
        for workers in (0, 2):
            for budget in (0, 2):
                with self.subTest(workers=workers, budget=budget):
                    self.launch(1, workers, 1, shutdown_timeout=True, sample_budget=budget)

    def test_background_backlog_never_blocks_game_completion(self):
        for workers in (0, 2):
            for budget in (0, 2):
                with self.subTest(workers=workers, budget=budget):
                    self.launch(1, workers, 1, sample_budget=budget, backpressure=True)

    def test_canceled_unsubmitted_leases_are_reset_off_walker_and_reused(self):
        for workers in (0, 2):
            for budget in (0, 2):
                with self.subTest(workers=workers, budget=budget):
                    self.launch(1, workers, 1, sample_budget=budget, cancel=True)

    def check_sampling(self, report, observations, frames, passes, draws, budget, stats_enabled, mode):
        rows = [o['row'] for o in observations]
        frame_rows = [r for r in rows if r['type'] == 'frame']
        pass_rows = [r for r in rows if r['type'] == 'pass']
        windows = [r for r in rows if r['type'] == 'window']
        self.assertEqual(len(frame_rows), frames)
        self.assertEqual(len(windows), 2)
        for field in ('missing_frame_ids', 'reconciliation_failures', 'dropped', 'failed_slots',
                      'invalid_frame_spans', 'invalid_window_spans', 'window_sum_mismatches'):
            self.assertEqual(report['coverage'][field], 0, (field, report['issues']))
        renders = [r for r in pass_rows if r['kind'] == 'render']
        self.assertEqual(sum(r['count'] for r in renders), passes)
        self.assertEqual(sum(r['draws'] for r in renders), draws)
        self.assertEqual({(r['render_width'], r['render_height']) for r in renders}, {(64, 64), (128, 128)})
        self.assertTrue(any(r['kind'] == 'upload' for r in pass_rows),
                        'even a rare unsampled pre upload must retain metadata')
        self.assertTrue(any(r['sampling']['sample_count'] < r['count'] for r in renders))
        # Per-ordinal diagnostics may expose one-off positions (for example an
        # initial upload shifts the first frame). Rotation covers stable keys,
        # not every transient ordinal/key pairing from a frame already gone.
        sampled_keys = {g['key'] for g in report['passes'] if g['sampling']['sample_count']}
        for group in report['passes']:
            if group['kind'] in ('render', 'transfer') and group['sampling']['observed_frames'] >= 10:
                self.assertIn(group['key'], sampled_keys, 'rotation must eventually time every stable observed key')
        for row in frame_rows:
            self.assertEqual(row['sample_budget'], budget)
            self.assertEqual(row['timing_scope'], 'main-frame')
            self.assertIs(type(row['timestamp_count']), int)
            self.assertGreaterEqual(row['timestamp_count'], 2, 'whole main frame always has its own pair')
            self.assertLessEqual(row['timestamp_count'], 2 * budget + 2)
            self.assertLessEqual(row['sampled_passes'], budget)
            self.assertLessEqual(row['sampled_passes'], row['observed_passes'])
            self.assertEqual(row['timestamp_count'], 2 * row['sampled_passes'] + 2,
                             'real completed fixture counts commands written, not query-pool capacity')
            self.assertTrue(row['span_valid'])
            self.assertFalse(row['reconciliation_failed'])
            self.assertGreater(row['span_ms'], 0)
            self.assertEqual((row['scene_width'], row['scene_height']), (0, 0))
            mask = (1 << row['timestamp_bits']) - 1
            ticks = (row['last_tick'] - row['first_tick']) & mask
            self.assertAlmostEqual(row['span_ms'], ticks * row['timestamp_period'] / 1e6, delta=0.000002)
        for row in windows:
            members = [r for r in frame_rows if row['window_start'] <= r['frame'] <= row['window_end']]
            aggregates = [r for r in pass_rows if r['window_start'] == row['window_start']]
            self.assertEqual(row['sample_budget'], budget)
            self.assertEqual(row['timestamp_count'], sum(r['timestamp_count'] for r in members))
            self.assertEqual(row['observed_passes'], sum(r['observed_passes'] for r in members))
            self.assertEqual(row['observed_passes'], sum(r['count'] for r in aggregates))
            self.assertEqual(row['sample_count'], sum(r['sampling']['sample_count'] for r in aggregates))
        for row in rows:
            if row['type'] not in ('pass', 'long_frame'):
                continue
            coverage = row['sampling']
            self.assertEqual(coverage['budget'], budget)
            self.assertEqual(coverage['observed_count'], row['count'])
            self.assertLessEqual(coverage['sample_count'], row['count'])
            self.assertLessEqual(coverage['sampled_frames'], coverage['observed_frames'])
            self.assertEqual(len(coverage['histogram']), 272)
            self.assertEqual(sum(coverage['histogram']), coverage['sample_count'])
            self.assertLessEqual(row['stats_valid'], coverage['sample_count'])
            self.assertEqual('stats' in row, bool(row['stats_valid']))
            if not coverage['sample_count']:
                self.assertEqual(row['gpu_ms'], 0)
                self.assertEqual(row['stats_valid'], 0)
            available = stats_enabled and row['kind'] == 'render'
            self.assertEqual(row['stats_valid'], coverage['sample_count'] if available else 0)
            if row['stats_valid']:
                self.assertLessEqual(row['stats_draws'], row['draws'])
                self.assertEqual(row['stats']['ia_vertices'], row['stats_draws'] * 3)
                self.assertEqual(row['stats']['ia_primitives'], row['stats_draws'])
                self.assertAlmostEqual(row['stats_gpu_ms'], row['gpu_ms'], delta=0.000002)
            if row['type'] == 'long_frame':
                self.assertEqual(row['count'], 1)
                self.assertIn(coverage['sample_count'], (0, 1))
                self.assertGreater(row['ordinal'], 0)
        self.assertEqual(report['coverage']['stats_missing'],
                         sum(r['sampling']['sample_count'] - r['stats_valid'] for r in pass_rows))
        self.assertEqual(report['timing']['frame']['timestamp_count'], sum(r['timestamp_count'] for r in frame_rows))
        self.assertEqual(report['timing']['frame']['timestamp_count_max'], max(r['timestamp_count'] for r in frame_rows))
        self.assertEqual(report['coverage']['long_frames'], frames if mode == 3 else 0)
        if mode == 3:
            self.assertTrue(any(r['type'] == 'long_frame' and r['kind'] == 'render' and
                                r['sampling']['sample_count'] == 0 for r in rows),
                            'raw frames must expose untimed render metadata, never drop those passes')

    def test_sampled_budgets_workers_statistics_and_whole_main_frame_authority(self):
        fields = ('key', 'kind', 'count', 'draws', 'window_start', 'window_end', 'ordinal',
                  'render_width', 'render_height', 'reference_width', 'reference_height',
                  'scene_relative', 'shader', 'samples', 'attachments')
        def metadata(report):
            return {json.dumps({key: row[key] for key in fields}, sort_keys=True)
                    for row in report['_fixture_rows'] if row['type'] == 'pass'}
        for workers in (0, 2):
            for stats in (0, 1):
                full = self.launch(3, workers, stats, sampling=True)
                reference = metadata(full)
                # Ignore only the final pre-upload frames: full mode includes pre;
                # sampling explicitly promises main-frame authority, not pre.
                full_frames = [r for r in full['_fixture_rows'] if r['type'] == 'frame' and r['frame'] <= 302]
                for budget in (1, 2):
                    with self.subTest(budget=budget, workers=workers, stats=stats):
                        report = self.launch(3, workers, stats, sampling=True, sample_budget=budget)
                        observed = metadata(report)
                        self.assertEqual(observed, reference, 'sampling cannot change observed pass identity or counts')
                        sampled_frames = [r for r in report['_fixture_rows']
                                          if r['type'] == 'frame' and r['frame'] <= 302]
                        self.assertEqual([r['frame'] for r in sampled_frames], [r['frame'] for r in full_frames])
                        self.assertEqual({(r['timestamp_bits'], r['timestamp_period'], r['queue_family'])
                                          for r in sampled_frames},
                                         {(r['timestamp_bits'], r['timestamp_period'], r['queue_family'])
                                          for r in full_frames})
                        full_mean = sum(r['span_ms'] for r in full_frames) / len(full_frames)
                        sampled_mean = sum(r['span_ms'] for r in sampled_frames) / len(sampled_frames)
                        self.assertGreater(full_mean, 0)
                        self.assertGreater(sampled_mean, 0)
                        self.assertAlmostEqual(sampled_mean, full_mean, delta=max(0.25, full_mean * 2),
                                               msg='lavapipe authority/extent comparison, not identical GPU durations')

    def test_explicit_zero_sampling_keeps_full_mode_and_sampled_options(self):
        absent = self.launch(1, 2, 1, sampling=True)
        zero = self.launch(1, 2, 1, sampling=True, sample_budget=0)
        for report in (absent, zero):
            self.assertTrue(all('sampling' not in r for r in report['_fixture_rows']
                                if r['type'] in ('pass', 'long_frame')))
        def counts(report):
            return sorted((g['key'], g['count'], g['draws'], g['stats']) for g in report['passes'])
        self.assertEqual(counts(absent), counts(zero))
        self.launch(0, 2, 1, sampling=True, sample_budget=2)
        for mode, ordinal_mode, unsupported in ((1, None, False), (2, 1, False), (1, None, True)):
            with self.subTest(mode=mode, ordinals=ordinal_mode, unsupported=unsupported):
                self.launch(mode, 2, 1, sampling=True, sample_budget=2, ordinals=ordinal_mode,
                            unsupported=unsupported)

    def check_areas(self, report, observations, stdout, stats_enabled):
        self.assertIn('gpuprof-areas frames=3 passes=11 draws=0 exact_keys=6 families=5 transfers=17', stdout)
        metadata = re.findall(
            r'gpuprof-areas-frame frame=(\d+) scene_width=(\d+) scene_height=(\d+) passes=(\d+) '
            r'prepass=(\d+) geometry=(\d+) shadow=1 other_depth=1 transfers=(\d+)', stdout)
        self.assertEqual([tuple(map(int, row)) for row in metadata],
                         [(1, 64, 36, 4, 1, 1, 6), (2, 128, 72, 5, 1, 2, 9), (3, 0, 0, 2, 0, 0, 2)])
        for key in ('window_sum_mismatches', 'missing_frame_ids', 'reconciliation_failures', 'dropped',
                    'failed_slots', 'long_frame_rows'):
            self.assertEqual(report['coverage'][key], 0, report['issues'])
        self.assertEqual(report['coverage']['frame_rows'], 3)
        self.assertEqual(report['coverage']['windows'], 1)
        self.assertAlmostEqual(report['timing']['window']['measured_ms'],
                               report['timing']['frame']['measured_ms'], delta=0.01)
        groups = [g for g in report['passes'] if g['kind'] == 'render']
        families = [g for g in report['families'] if g['kind'] == 'render']
        self.assertEqual(sum(g['count'] for g in groups), 11)
        self.assertEqual(sum(g['draws'] for g in groups), 0)
        self.assertEqual(len({g['key'] for g in groups}), 6)
        self.assertEqual(len(families), 5)
        self.assertEqual(sum(g['count'] for g in report['passes'] if g['kind'] == 'transfer'), 17)
        relative = [g for g in families if g['scene_relative']]
        self.assertEqual(len(relative), 3)
        geometry = [g for g in relative if len(g['areas'][0]['attachments']) == 3]
        prepass = [g for g in relative if len(g['areas'][0]['attachments']) == 1]
        self.assertEqual(len(geometry), 2)
        self.assertEqual(len(prepass), 1)
        self.assertEqual(prepass[0]['count'], 2)
        self.assertEqual(len(prepass[0]['keys']), 2)
        self.assertEqual({(a['render_width'], a['render_height']) for a in prepass[0]['areas']},
                         {(64, 36), (128, 72)})
        full = next(g for g in geometry if len(g['keys']) == 2)
        half = next(g for g in geometry if len(g['keys']) == 1)
        self.assertEqual(full['count'], 2)
        self.assertEqual(half['count'], 1)
        self.assertEqual({(a['render_width'], a['render_height']) for a in full['areas']}, {(64, 36), (128, 72)})
        self.assertEqual((half['areas'][0]['render_width'], half['areas'][0]['render_height']), (64, 36))
        for attachment in half['areas'][0]['attachments']:
            self.assertEqual((attachment['width_num'], attachment['width_den']), (1, 2))
            self.assertEqual((attachment['height_num'], attachment['height_den']), (1, 2))
        smaller_key = half['keys'][0]
        smaller_areas = [a for g in groups if g['key'] == smaller_key for a in g['areas']]
        self.assertEqual({(a['reference_width'], a['reference_height']) for a in smaller_areas},
                         {(64, 36), (128, 72)})
        fixed = [g for g in families if not g['scene_relative']]
        self.assertEqual(len(fixed), 2)
        self.assertEqual({a['render_width'] for g in fixed for a in g['areas']}, {128, 360})
        for group in fixed:
            self.assertEqual(group['count'], 3)
            self.assertEqual(len(group['keys']), 1, 'fixed depth key must not follow changing scene references')
            self.assertEqual({(a['reference_width'], a['reference_height']) for a in group['areas']},
                             {(64, 36), (128, 72), (1280, 720)})
            for area in group['areas']:
                self.assertEqual(area['keys'], group['keys'])
                self.assertEqual(len(area['attachments']), 1)
                attachment = area['attachments'][0]
                self.assertEqual(attachment['slot'], 8)
                self.assertEqual((attachment['width'], attachment['height']),
                                 (area['render_width'], area['render_height']))
        expected_scenes = {1: (64, 36), 2: (128, 72), 3: (0, 0)}
        frame_rows = [o['row'] for o in observations if o['row']['type'] == 'frame']
        self.assertEqual({r['frame']: (r['scene_width'], r['scene_height']) for r in frame_rows}, expected_scenes)
        for row in frame_rows:
            self.assertFalse(row['reconciliation_failed'])
            self.assertEqual(row['dropped'], 0)
        for observation in observations:
            row = observation['row']
            if row['type'] != 'pass':
                continue
            self.assertEqual('stats' in row, bool(row['stats_valid']))
            self.assertLessEqual(row['stats_valid'], row['count'])
            if not stats_enabled or row['kind'] != 'render':
                self.assertEqual(row['stats_valid'], 0)
            if row['stats_valid']:
                self.assertTrue(all(row['stats'][key] == 0 for key in gpuprof.STATS))
        self.assertEqual(report['coverage']['stats_missing'],
                         sum(o['row'].get('sampling', {}).get('sample_count', o['row']['count']) -
                             o['row']['stats_valid']
                             for o in observations if o['row']['type'] == 'pass'))

    def test_scene_areas_prepasses_and_fixed_depth_families(self):
        reference = None
        for mode in (1, 2):
            for workers in (0, 2):
                for stats in (0, 1):
                    for ordinals in (0, 1):
                        with self.subTest(mode=mode, workers=workers, stats=stats, ordinals=ordinals):
                            report = self.launch(mode, workers, stats, areas=True, ordinals=ordinals)
                            keys = {(g['key'], g['render_width'], g['render_height'])
                                    for g in report['passes'] if g['kind'] == 'render'}
                            if reference is None:
                                reference = keys
                            self.assertEqual(keys, reference)

    def test_sampled_scene_reference_includes_unselected_mrt_and_depth_metadata(self):
        for budget in (1, 2):
            for workers in (0, 2):
                for stats in (0, 1):
                    with self.subTest(budget=budget, workers=workers, stats=stats):
                        # The existing real --areas scenario observes its largest
                        # scene even when ordinal rotation does not time that MRT.
                        report = self.launch(1, workers, stats, areas=True, sample_budget=budget)
                        rows = report['_fixture_rows']
                        for row in rows:
                            if row['type'] == 'frame':
                                self.assertEqual(row['timing_scope'], 'main-frame')
                                self.assertGreaterEqual(row['timestamp_count'], 2)
                                self.assertLessEqual(row['timestamp_count'], 2 * budget + 2)
                                self.assertLessEqual(row['sampled_passes'], budget)
                            elif row['type'] == 'pass':
                                self.assertLessEqual(row['stats_valid'], row['sampling']['sample_count'])
                        self.assertTrue(any(row['type'] == 'pass' and row['kind'] == 'render' and
                                            not row['sampling']['sample_count'] for row in rows),
                                        'short scene/reference changes must retain unsampled render metadata')

    def test_modes_workers_statistics_and_stable_keys(self):
        reference = None
        for mode in range(4):
            for workers in (0, 2):
                for stats in (0, 1):
                    for ordinals in (0, 1):
                        with self.subTest(mode=mode, workers=workers, stats=stats, ordinals=ordinals):
                            report = self.launch(mode, workers, stats, ordinals=ordinals)
                            if report:
                                keys = {(g['key'], g['shader']) for g in report['passes'] if g['kind'] == 'render'}
                                if reference is None:
                                    reference = keys
                                self.assertEqual(keys, reference, 'recorder/mode/stats/ordinals must not change pass keys')
        for workers in (0, 2):
            for ordinals in (0, 1):
                with self.subTest(unsupported=True, workers=workers, ordinals=ordinals):
                    report = self.launch(1, workers, 1, unsupported=True, ordinals=ordinals)
                    self.assertEqual({(g['key'], g['shader']) for g in report['passes'] if g['kind'] == 'render'}, reference)

    def test_default_compact_output_reduces_window_rows_and_bytes(self):
        compact = self.launch(1, 2, 1)
        detailed = self.launch(1, 2, 1, ordinals=1)
        self.assertLess(compact['coverage']['pass_rows'], detailed['coverage']['pass_rows'])
        self.assertLess(compact['_fixture_output_bytes'], detailed['_fixture_output_bytes'])
        for report in (compact, detailed):
            self.assertEqual(report['coverage']['window_sum_mismatches'], 0)
        def totals(report):
            rows = [(g['kind'], g['samples'], g['count'], g['draws'], g['stats'], g['stats_missing'],
                     g['ratio_draws']) for g in report['families']]
            return sorted(rows, key=lambda row: json.dumps(row, sort_keys=True))
        self.assertEqual(totals(compact), totals(detailed))

    def test_real_unsubmitted_span_query_is_unavailable_not_zero_time(self):
        self.launch(1, 0, 0, unavailable_span=True)

    def test_bounded_output_rotation_retains_whole_schema_rows(self):
        for ordinals in (0, 1):
            with self.subTest(ordinals=ordinals):
                self.launch(3, 2, 1, rotate=True, ordinals=ordinals)


if __name__ == '__main__':
    unittest.main()

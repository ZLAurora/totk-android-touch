#!/usr/bin/env python3
"""Analyze schema-3 GPU-profiler JSONL (host only, Python standard library).

Pass gpu_ms and counters are window SUMS, not per-frame averages. Sampled gpu_ms
contains measured samples only, never an extrapolated total. Scene-relative
families use exact scene ratios; other families retain absolute attachment areas.
Long-frame rows are a separate, selected population and never added to windows.
"""
import argparse
import glob
import hashlib
from fractions import Fraction
import json
import math
from pathlib import Path
import re
import sys

from benchlib import guard_output

STATS = ('ia_vertices', 'ia_primitives', 'vs_invocations', 'clipping_invocations',
         'clipping_primitives', 'fs_invocations')
ATTACHMENT = ('slot', 'format', 'width', 'height', 'load', 'store')
ATTACHMENT_RATIOS = ('width_num', 'width_den', 'height_num', 'height_den')
RETIREMENT_COUNTS = ('leases_acquired', 'leases_submitted', 'leases_retired', 'leases_cancelled',
                     'leases_outstanding', 'queries_submitted', 'queries_retired', 'queries_cancelled',
                     'queries_outstanding')
REFERENCE = ('reference_width', 'reference_height', 'scene_relative')
UINT32_MAX = (1 << 32) - 1
WINDOW_FRAMES = 300
SPAN_LIMIT_MS = 10000
HISTOGRAM_BUCKETS = 272
MAX_SAMPLE_BUDGET = 16
RATIOS = ('verts_per_draw', 'prims_per_draw', 'fragments_per_prim', 'ns_per_vertex', 'ns_per_primitive')


def number(row, key, integer=False, minimum: float = 0):
    value = row.get(key)
    if isinstance(value, bool) or not isinstance(value, int if integer else (int, float)):
        raise ValueError(f'{key}: expected {"integer" if integer else "number"}')
    try:
        finite = integer or math.isfinite(value)
    except OverflowError:
        finite = False
    if value < minimum or not finite:
        raise ValueError(f'{key}: expected finite value >= {minimum}')
    return value


def dimensions(row, prefix, minimum=0):
    width, height = (number(row, f'{prefix}_{axis}', True, minimum) for axis in ('width', 'height'))
    if width > UINT32_MAX or height > UINT32_MAX:
        raise ValueError(f'{prefix}: dimensions exceed uint32')
    if bool(width) != bool(height):
        raise ValueError(f'{prefix}: zero dimensions must be paired')
    return width, height


def sample_budget(row, key='sample_budget', minimum=1):
    budget = number(row, key, True, minimum)
    if budget > MAX_SAMPLE_BUDGET:
        raise ValueError(f'{key}: exceeds {MAX_SAMPLE_BUDGET}')
    return budget


def validate_sampling(row):
    sampling = row['sampling']
    if not isinstance(sampling, dict):
        raise ValueError('sampling: expected object')
    sample_budget(sampling, 'budget')
    observed = number(sampling, 'observed_count', True, 1)
    sampled = number(sampling, 'sample_count', True)
    observed_frames = number(sampling, 'observed_frames', True, 1)
    sampled_frames = number(sampling, 'sampled_frames', True)
    frames = 1 if row['type'] == 'long_frame' else row['window_end'] - row['window_start'] + 1
    if observed != row['count'] or sampled > observed:
        raise ValueError('sampling: observed_count must equal count and sample_count must not exceed it')
    if not sampled_frames <= observed_frames <= frames:
        raise ValueError('sampling: invalid observed/sampled frame coverage')
    if observed_frames > observed or sampled_frames > sampled or bool(sampled_frames) != bool(sampled):
        raise ValueError('sampling: frame coverage differs from observed/sample counts')
    if sampled > sampling['budget'] * sampled_frames:
        raise ValueError('sampling: sample_count exceeds budget per sampled frame')
    if row['type'] == 'long_frame' and (observed != 1 or sampled > 1):
        raise ValueError('sampling: long_frame requires count=1 and sample_count=0 or 1')
    if not sampled and row['gpu_ms'] != 0:
        raise ValueError('sampling: unsampled rows require gpu_ms=0')
    if row['stats_valid'] > sampled:
        raise ValueError('sampling: stats_valid exceeds sample_count')
    histogram = sampling.get('histogram')
    if not isinstance(histogram, list) or len(histogram) != HISTOGRAM_BUCKETS:
        raise ValueError(f'sampling: histogram requires {HISTOGRAM_BUCKETS} buckets')
    if any(type(count) is not int or count < 0 for count in histogram):
        raise ValueError('sampling: histogram counts must be nonnegative integers')
    if sum(histogram) != sampled:
        raise ValueError('sampling: histogram sum differs from sample_count')


def validate_sampled_timing(row):
    budget = sample_budget(row)
    frames = 1 if row['type'] == 'frame' else row.get(
        'frames', row['window_end'] - row['window_start'] + 1)
    observed = number(row, 'observed_passes', True)
    sampled = number(row, 'sampled_passes' if row['type'] == 'frame' else 'sample_count', True)
    if sampled > min(observed, budget * frames):
        raise ValueError('sample count exceeds observed passes or frame budget')
    minimum_timestamps = 0 if row.get('span_valid') is False else 2 * frames
    if not minimum_timestamps <= number(row, 'timestamp_count', True) <= (2 * budget + 2) * frames:
        raise ValueError('timestamp_count outside whole-frame/sample timestamp budget')
    if row.get('timing_scope') != 'main-frame':
        raise ValueError('sampled timing requires timing_scope=main-frame')


def validate_query_retirement(row):
    retirement = row['query_retirement']
    if not isinstance(retirement, dict):
        raise ValueError('query_retirement: expected object')
    for key in RETIREMENT_COUNTS:
        number(retirement, key, True)
    acquired, submitted, retired, cancelled, outstanding = (
        retirement[f'leases_{key}'] for key in ('acquired', 'submitted', 'retired', 'cancelled', 'outstanding'))
    if acquired != retired + cancelled + outstanding:
        raise ValueError('query_retirement: acquired leases must equal retired + cancelled + outstanding')
    if not retired <= submitted <= acquired - cancelled:
        raise ValueError('query_retirement: submitted leases must include retired leases and exclude cancelled leases')
    if retirement['queries_outstanding'] != retirement['queries_submitted'] - retirement['queries_retired']:
        raise ValueError('query_retirement: outstanding queries must equal submitted - retired')
    all_retired = row['all_queries_retired']
    if not isinstance(all_retired, bool) or all_retired != (
            outstanding == 0 and retirement['queries_outstanding'] == 0):
        raise ValueError('query_retirement: all_queries_retired must be boolean and match outstanding counts')


def validate(row):
    """Reject malformed v3 rows; preserve additive producer metadata verbatim."""
    if not isinstance(row, dict) or type(row.get('schema')) is not int or row['schema'] != 3:
        raise ValueError('expected object with schema=3')
    kind = row.get('type')
    if kind == 'header':
        if not isinstance(row.get('launch_id'), str) or not row['launch_id']:
            raise ValueError('launch_id: expected nonempty string')
        number(row, 'rotation', True)
        for key in ('ordinals', 'stats'):
            if not isinstance(row.get(key), bool):
                raise ValueError(f'{key}: expected boolean')
        for key in ('profile', 'max_bytes', 'retained_files', 'window_frames'):
            number(row, key, True, 1)
        number(row, 'span_limit_ms', minimum=1)
        if not isinstance(row.get('driver'), dict) or not isinstance(row.get('env'), dict):
            raise ValueError('header requires driver and env objects')
        if 'sample_budget' in row:
            sample_budget(row, minimum=0)
        if 'background_slots' in row:
            number(row, 'background_slots', True, 1)
        if 'background_closures' in row:
            number(row, 'background_closures', True, 1)
        return row
    if kind == 'session_end':
        if not isinstance(row.get('launch_id'), str) or not row['launch_id']:
            raise ValueError('launch_id: expected nonempty string')
        if row.get('clean_end') is not True or 'all_queries_retired' not in row:
            raise ValueError('session_end requires clean_end=true and all_queries_retired')
        if 'query_retirement' in row:
            validate_query_retirement(row)
        elif row['all_queries_retired'] is not None:
            raise ValueError('session_end without query_retirement requires historical all_queries_retired=null')
        if 'query_drain_timed_out' in row and not isinstance(row['query_drain_timed_out'], bool):
            raise ValueError('query_drain_timed_out: expected boolean')
        for key in ('last_frame', 'last_window_sequence', 'last_window_end', 'output_failures'):
            number(row, key, True)
        if 'background_dropped_slots' in row:
            number(row, 'background_dropped_slots', True)
        for key in ('background_dropped_closures', 'pending_frame_closures'):
            if key in row:
                number(row, key, True)
        return row
    if kind not in ('window', 'pass', 'frame', 'long_frame'):
        raise ValueError('unknown row type')
    if kind in ('window', 'pass'):
        start, end = (number(row, k, True, 1) for k in ('window_start', 'window_end'))
        if end < start:
            raise ValueError('window_end precedes window_start')
    if kind in ('frame', 'long_frame'):
        number(row, 'frame', True, 1)
    if kind == 'frame':
        dimensions(row, 'scene')
    if kind in ('window', 'frame'):
        valid = row.get('span_valid', True)
        if not isinstance(valid, bool):
            raise ValueError('span_valid: expected boolean')
        number(row, 'measured_ms')
        for key in ('span_ms', 'gap_ms'):
            if valid:
                number(row, key, minimum=-math.inf if key == 'gap_ms' else 0)
            elif key not in row or row[key] is not None:
                raise ValueError(f'{key}: invalid span must be present and null')
        if row.get('raw_span_ms') is not None:
            number(row, 'raw_span_ms')
        for key in ('valid_span_ms', 'valid_gap_ms'):
            if key in row:
                number(row, key, minimum=-math.inf if key == 'valid_gap_ms' else 0)
        if 'span_reason' in row and (not isinstance(row['span_reason'], str) or not row['span_reason']):
            raise ValueError('span_reason: expected nonempty string')
        number(row, 'dropped', True)
        for key in ('failed_slots', 'stats_failed_slots', 'frames', 'invalid_spans', 'span_frames',
                    'unavailable_timestamps', 'invalid_timestamps', 'timestamp_wraps', 'output_failures',
                    'background_dropped_slots', 'background_dropped_closures'):
            if key in row:
                number(row, key, True)
        if 'sequence' in row:
            if number(row, 'sequence', True, 1) != (row['window_start'] - 1) // WINDOW_FRAMES + 1:
                raise ValueError('sequence differs from window_start')
        for key in ('partial', 'reconciliation_failed', 'ordinals'):
            if key in row and not isinstance(row[key], bool):
                raise ValueError(f'{key}: expected boolean')
        budget = sample_budget(row, minimum=0) if 'sample_budget' in row else 0
        if budget or any(key in row for key in ('observed_passes', 'sampled_passes', 'sample_count')):
            validate_sampled_timing(row)
    else:
        for key in ('key', 'shader', 'kind'):
            if not isinstance(row.get(key), str) or not row[key]:
                raise ValueError(f'{key}: expected nonempty string')
        if row['kind'] not in ('render', 'compute', 'upload', 'transfer', 'post', 'upscale', 'present', 'framegen'):
            raise ValueError('kind: unknown GPU scope kind')
        dimensions(row, 'render', 1 if row['kind'] == 'render' else 0)
        dimensions(row, 'reference', 1)
        if not isinstance(row.get('scene_relative'), bool):
            raise ValueError('scene_relative: expected boolean')
        number(row, 'ordinal', True, 1 if kind == 'long_frame' else 0)
        number(row, 'gpu_ms')
        count = number(row, 'count', True, 1)
        number(row, 'draws', True)
        number(row, 'samples', True)
        valid = number(row, 'stats_valid', True)
        if valid > count:
            raise ValueError('stats_valid exceeds count')
        if ('stats' in row) != bool(valid):
            raise ValueError('stats must be present exactly when stats_valid > 0')
        if valid:
            if not isinstance(row['stats'], dict):
                raise ValueError('stats: expected object')
            for key in STATS:
                number(row['stats'], key, True)
        if ('stats_gpu_ms' in row) != ('stats_draws' in row):
            raise ValueError('stats_gpu_ms and stats_draws must be paired')
        if 'stats_gpu_ms' in row:
            if not valid:
                raise ValueError('matched statistics require stats_valid > 0')
            if number(row, 'stats_gpu_ms') > row['gpu_ms'] + 0.01:
                raise ValueError('stats_gpu_ms exceeds gpu_ms')
            if number(row, 'stats_draws', True) > row['draws']:
                raise ValueError('stats_draws exceeds draws')
        if 'ratios' in row:
            if not valid or not isinstance(row['ratios'], dict):
                raise ValueError('ratios require available stats')
            for key, value in row['ratios'].items():
                if key in RATIOS and value is not None:
                    number(row['ratios'], key)
        attachments = row.get('attachments')
        if not isinstance(attachments, list):
            raise ValueError('attachments: expected list')
        slots = set()
        for attachment in attachments:
            if not isinstance(attachment, dict):
                raise ValueError('attachment: expected object')
            for key in (*ATTACHMENT, *ATTACHMENT_RATIOS):
                number(attachment, key, True, 1 if key in ('width', 'height') else 0)
            if attachment['width'] > UINT32_MAX or attachment['height'] > UINT32_MAX:
                raise ValueError('attachment: dimensions exceed uint32')
            if attachment['slot'] in slots:
                raise ValueError('duplicate attachment slot')
            slots.add(attachment['slot'])
            for axis in ('width', 'height'):
                n, d = (attachment[f'{axis}_{part}'] for part in ('num', 'den'))
                if d == 0 or math.gcd(n, d) != 1:
                    raise ValueError(f'{axis}: expected reduced rational with positive denominator')
                if n * row[f'reference_{axis}'] != attachment[axis] * d:
                    raise ValueError(f'{axis}: rational differs from attachment/reference dimensions')
        if 'sampling' in row:
            validate_sampling(row)
    return row


def stream_path(path):
    """One producer launch plus its .1/.2/.3 rotations is one observation stream."""
    return re.sub(r'(?<=\.jsonl)\.[1-3]$', '', str(path.resolve()))


def inputs(patterns):
    paths = {}
    for pattern in patterns:
        matches = sorted(glob.glob(str(pattern)))
        if not matches:
            raise ValueError(f'no input files match {pattern!r}')
        for name in matches:
            path = Path(name).resolve()
            if not path.is_file():
                raise ValueError(f'not a file: {name}')
            paths[path] = None
    # Oldest rotation first, current file last. Aggregation never relies on order.
    return sorted(paths, key=lambda p: (stream_path(p), -int(p.suffix[1:]) if p.suffix[1:].isdigit() else 0))


def identity(stream, row):
    kind = row['type']
    if kind == 'header':
        fields = ('launch_id', 'rotation')
    elif kind == 'session_end':
        fields = ('launch_id',)
    elif kind == 'window':
        fields = ('window_start', 'window_end')
    elif kind == 'pass':
        fields = ('window_start', 'window_end', 'key', 'ordinal', *REFERENCE)
    elif kind == 'frame':
        fields = ('frame',)
    else:
        fields = ('frame', 'key', 'ordinal', *REFERENCE)
    return (stream, kind, *(row[key] for key in fields))


def load(paths, legacy_span_replay=False):
    def reject_constant(text):
        raise ValueError(f'nonfinite JSON number {text}')

    observations, conflicts = {}, set()
    issues = []
    counts = dict(malformed_rows=0, truncated_rows=0, duplicate_rows=0, conflicting_rows=0, blank_lines=0,
                  legacy_ignored_rows=0)
    for path in paths:
        stream = stream_path(path)
        with path.open(encoding='utf-8', errors='replace') as file:
            for line_no, line in enumerate(file, 1):
                if not line.strip():
                    counts['blank_lines'] += 1
                    continue
                where = f'{path}:{line_no}'
                try:
                    row = json.loads(line, parse_constant=reject_constant)
                    if legacy_span_replay and isinstance(row, dict) and row.get('schema') in (2, 3):
                        if row.get('type') not in ('frame', 'window', 'header', 'session_end'):
                            counts['legacy_ignored_rows'] += 1
                            continue
                        if row['schema'] == 2:
                            row = dict(row, schema=3, source_schema=2)
                            if row['type'] == 'frame':
                                row.setdefault('scene_width', 0)
                                row.setdefault('scene_height', 0)
                    row = validate(row)
                except (ValueError, TypeError) as error:
                    # A writer may still own the final line. Do not silently accept
                    # it, but distinguish that censored tail from an interior error.
                    tail = (isinstance(error, json.JSONDecodeError) and not line.endswith('\n')
                            and line.lstrip().startswith('{') and not line.rstrip().endswith('}'))
                    counts['truncated_rows' if tail else 'malformed_rows'] += 1
                    issues.append(f'{where}: {"incomplete tail" if tail else "malformed row"}: {error}')
                    continue
                ident = identity(stream, row)
                if ident in conflicts:
                    counts['conflicting_rows'] += 1
                elif ident in observations:
                    if observations[ident]['row'] == row:
                        counts['duplicate_rows'] += 1
                    else:
                        counts['conflicting_rows'] += 1
                        issues.append(f'{where}: conflicting identity; excluding both observations {ident[1:]}')
                        del observations[ident]
                        conflicts.add(ident)
                else:
                    observations[ident] = {'stream': stream, 'source': where, 'row': row}
    # Rotations can begin with data rows, so apply launch-mode consistency only
    # after collecting the entire stream. A missing header is censored, not fatal.
    declared, inferred = {}, {}
    for observation in observations.values():
        row, stream = observation['row'], observation['stream']
        if row['type'] == 'header':
            declared.setdefault(stream, set()).add(row.get('sample_budget', 0))
        elif row.get('sample_budget') or 'sampling' in row:
            inferred.setdefault(stream, set()).add(
                row.get('sample_budget', row.get('sampling', {}).get('budget')))
    accepted = []
    for observation in observations.values():
        row, stream = observation['row'], observation['stream']
        budgets = declared.get(stream, inferred.get(stream, set()))
        if row['type'] in ('pass', 'long_frame', 'frame', 'window') and len(budgets) == 1:
            expected = next(iter(budgets))
            actual = row.get('sampling', {}).get('budget', 0) if row['type'] in (
                'pass', 'long_frame') else row.get('sample_budget', 0)
            if actual != expected:
                counts['malformed_rows'] += 1
                issues.append(f'{observation["source"]}: malformed row: sampling budget/mode differs from stream')
                continue
        accepted.append(observation)
    return accepted, counts, issues


def absolute_area(row):
    attachments = sorted(row['attachments'], key=lambda a: a['slot'])
    return (row['render_width'], row['render_height'],
            tuple(tuple(a[k] for k in ATTACHMENT) for a in attachments))


def family(row):
    if not row['scene_relative']:
        area = absolute_area(row)
    else:
        attachments = sorted(row['attachments'], key=lambda a: a['slot'])
        area = (Fraction(row['render_width'], row['reference_width']),
                Fraction(row['render_height'], row['reference_height']),
                tuple((a['slot'], a['format'], *(a[k] for k in ATTACHMENT_RATIOS), a['load'], a['store'])
                      for a in attachments))
    return (row['kind'], row['samples'], row['scene_relative'], area)


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def frame_coverage(contexts):
    """Bound distinct-frame unions; aggregate counts do not identify frame IDs."""
    streams = {}
    for (stream, start, end), coverage in contexts.items():
        observed = [counts[0] for counts in coverage]
        sampled = [counts[1] for counts in coverage]
        extent = end - start + 1
        streams.setdefault(stream, []).append((
            start, end, max(observed), min(extent, sum(observed)),
            max(sampled), min(extent, sum(sampled))))
    bounds = [0, 0, 0, 0]
    for intervals in streams.values():
        component = None
        for start, end, *counts in sorted(intervals):
            if component is None or start > component[1]:
                if component is not None:
                    for index, count in enumerate(component[2:]):
                        bounds[index] += count
                component = [start, end, *counts]
            else:
                component[1] = max(component[1], end)
                extent = component[1] - component[0] + 1
                component[2] = max(component[2], counts[0])
                component[3] = min(extent, component[3] + counts[1])
                component[4] = max(component[4], counts[2])
                component[5] = min(extent, component[5] + counts[3])
        if component is not None:
            for index, count in enumerate(component[2:]):
                bounds[index] += count
    observed_low, observed_high, sampled_low, sampled_high = bounds
    sampled_high = min(sampled_high, observed_high)
    coverage_low = ratio(sampled_low, observed_high)
    coverage_high = min(1.0, sampled_high / observed_low) if observed_low else None
    thin = sampled_high < 10
    sufficient = sampled_low >= 10
    return dict(
        observed_frames=observed_low if observed_low == observed_high else None,
        sampled_frames=sampled_low if sampled_low == sampled_high else None,
        observed_frames_bounds=[observed_low, observed_high],
        sampled_frames_bounds=[sampled_low, sampled_high],
        frame_coverage=coverage_low if coverage_low == coverage_high else None,
        frame_coverage_bounds=[coverage_low, coverage_high],
        thin=True if thin else False if sufficient else None,
        coverage_status='thin' if thin else 'sufficient' if sufficient else 'uncertain')


def sample_percentile(histogram, percent):
    count = sum(histogram)
    if not count:
        return None
    rank = (count * percent + 99) // 100
    cumulative = 0
    for index, frequency in enumerate(histogram):
        cumulative += frequency
        if cumulative >= rank:
            return 2 ** (index / 8) / 1e6


def sampling_context(row, stream=''):
    if row['type'] == 'long_frame':
        return stream, row['frame'], row['frame']
    return stream, row['window_start'], row['window_end']


def aggregate(rows, per_family=False, contexts=None):
    groups = {}
    for index, row in enumerate(rows):
        context = (contexts[index] if contexts is not None else sampling_context(row)) if 'sampling' in row else None
        absolute = absolute_area(row)
        ident = (family(row) if per_family else (
            row['key'], row['ordinal'], row['kind'], row['samples'], absolute, row['shader']))
        ident = ident, 'sampling' in row
        if ident not in groups:
            groups[ident] = dict(kind=row['kind'], samples=row['samples'], areas={},
                                 keys=set(), ordinals=set(), shaders=set(), rows=0, count=0, draws=0,
                                 gpu_ms=0.0, stats_valid=0, stats_missing=0, stats_partial_rows=0,
                                 stats={key: 0 for key in STATS}, ratio_draws=0, ratio_gpu_ms=0.0,
                                 ratio_stats={key: 0 for key in STATS})
            if per_family:
                groups[ident]['scene_relative'] = row['scene_relative']
            else:
                groups[ident].update(render_width=row['render_width'], render_height=row['render_height'],
                                     attachments=[{k: a[k] for k in ATTACHMENT}
                                                  for a in sorted(row['attachments'], key=lambda a: a['slot'])])
            if 'sampling' in row:
                groups[ident]['sampling'] = dict(
                    budgets=set(), observed_count=0, sample_count=0,
                    histogram=[0] * HISTOGRAM_BUCKETS, contexts={})
        group = groups[ident]
        variant = (absolute, *(row[k] for k in REFERENCE))
        if variant not in group['areas']:
            group['areas'][variant] = dict(
                keys=set(), render_width=row['render_width'], render_height=row['render_height'],
                **{k: row[k] for k in REFERENCE},
                attachments=sorted(row['attachments'], key=lambda a: a['slot']))
        group['areas'][variant]['keys'].add(row['key'])
        group['keys'].add(row['key'])
        group['ordinals'].add(row['ordinal'])
        group['shaders'].add(row['shader'])
        group['rows'] += 1
        for key in ('count', 'draws', 'gpu_ms', 'stats_valid'):
            group[key] += row[key]
        expected_stats = row.get('sampling', {}).get('sample_count', row['count'])
        group['stats_missing'] += expected_stats - row['stats_valid']
        if 'sampling' in row:
            sampling = group['sampling']
            source = row['sampling']
            sampling['budgets'].add(source['budget'])
            for key in ('observed_count', 'sample_count'):
                sampling[key] += source[key]
            for index, count in enumerate(source['histogram']):
                sampling['histogram'][index] += count
            sampling['contexts'].setdefault(context, []).append(
                (source['observed_frames'], source['sampled_frames']))
        if row['stats_valid']:
            for key in STATS:
                group['stats'][key] += row['stats'][key]
            if row['stats_valid'] == row['count'] or 'stats_gpu_ms' in row:
                group['ratio_draws'] += row.get('stats_draws', row['draws'])
                group['ratio_gpu_ms'] += row.get('stats_gpu_ms', row['gpu_ms'])
                for key in STATS:
                    group['ratio_stats'][key] += row['stats'][key]
            if row['stats_valid'] < expected_stats:
                group['stats_partial_rows'] += 1
    result = []
    for group in groups.values():
        group['areas'] = [{**area, 'keys': sorted(area['keys'])}
                          for _, area in sorted(group['areas'].items())]
        for key in ('keys', 'ordinals', 'shaders'):
            group[key] = sorted(group[key])
        if not per_family:
            group['key'], group['ordinal'], group['shader'] = (
                group['keys'][0], group['ordinals'][0], group['shaders'][0])
        group['draws_per_pass'] = ratio(group['draws'], group['count'])
        group['ns_per_draw'] = ratio(group['gpu_ms'] * 1e6, group['draws'])
        if 'sampling' in group:
            sampling = group['sampling']
            sampling['budgets'] = sorted(sampling['budgets'])
            sampling['budget'] = sampling['budgets'][0] if len(sampling['budgets']) == 1 else None
            sampling.update(frame_coverage(sampling.pop('contexts')))
            group['mean_ms'] = ratio(group['gpu_ms'], sampling['sample_count'])
            for percent in (50, 90, 99):
                group[f'p{percent}_ms'] = sample_percentile(sampling['histogram'], percent)
            group['p99_descriptive_only'] = sampling['sample_count'] < 100
            group['ns_per_draw'] = ratio(group['ratio_gpu_ms'] * 1e6, group['ratio_draws'])
            group['ns_per_draw_scope'] = 'matched-statistics'
        stats = group['stats']
        matched = group.pop('ratio_stats')
        group['ratios'] = {
            'verts_per_draw': ratio(matched['ia_vertices'], group['ratio_draws']),
            'prims_per_draw': ratio(matched['ia_primitives'], group['ratio_draws']),
            'fragments_per_prim': ratio(stats['fs_invocations'], stats['ia_primitives']),
            'ns_per_vertex': ratio(group['ratio_gpu_ms'] * 1e6, matched['ia_vertices']),
            'ns_per_primitive': ratio(group['ratio_gpu_ms'] * 1e6, matched['ia_primitives']),
        } if group['stats_valid'] else None
        if not group['stats_valid']:
            group['stats'] = None
        result.append(group)
    return sorted(result, key=lambda g: (-g['gpu_ms'], g['keys'], g['ordinals']))


def retention(observations, issues):
    """Use producer sequence/rotation facts, never infer a clean process exit."""
    streams = {}
    for observation in observations:
        streams.setdefault(observation['stream'], []).append(observation['row'])
    sessions = []
    for stream, rows in sorted(streams.items()):
        headers = sorted((r for r in rows if r['type'] == 'header'), key=lambda r: r['rotation'])
        ends = [r for r in rows if r['type'] == 'session_end']
        summaries = [r for r in rows if r['type'] == 'window']
        sequences = sorted({r.get('sequence', (r['window_start'] - 1) // WINDOW_FRAMES + 1)
                            for r in summaries})
        rotations = sorted({r['rotation'] for r in headers})
        frame_ids = [r['frame'] for r in rows if r['type'] == 'frame']
        proven_completed = (max(frame_ids, default=1) - 1) // WINDOW_FRAMES
        last = max([proven_completed, *sequences, *(r['last_window_sequence'] for r in ends)], default=0)
        # Without a header, old captures only prove holes within captured bounds.
        first = 1 if headers else min([*sequences, *((f - 1) // WINDOW_FRAMES + 1 for f in frame_ids)], default=1)
        missing = max(0, last - first + 1 - len(sequences))
        evictions = max(0, max(rotations, default=0) - 3)
        missing_files = max(rotations, default=-1) + 1 - len(rotations)
        header_conflict = bool(headers and any(
            {k: v for k, v in h.items() if k != 'rotation'} !=
            {k: v for k, v in headers[0].items() if k != 'rotation'} for h in headers[1:]))
        launch_ids = {r['launch_id'] for r in (*headers, *ends)}
        if header_conflict or len(launch_ids) > 1:
            issues.append(f'{stream}: inconsistent launch headers/session end')
        if missing:
            issues.append(f'{stream}: {missing} missing complete window sequence(s)')
        if evictions:
            issues.append(f'{stream}: rotation index proves {evictions} earlier file(s) exceeded retention')
        if missing_files:
            issues.append(f'{stream}: {missing_files} rotation file(s) absent from this capture')
        if headers and not ends:
            issues.append(f'{stream}: no session_end; process may be running, crashed, killed or capture incomplete')
        if ends and not headers:
            issues.append(f'{stream}: session_end present without a retained header')
        output_failures = max((r.get('output_failures', 0) for r in rows), default=0)
        if output_failures:
            issues.append(f'{stream}: producer reports {output_failures} cumulative output failure(s)')
        background_drops = max((r.get('background_dropped_slots', 0) for r in rows), default=0)
        if background_drops:
            issues.append(f'{stream}: profiler background backlog dropped {background_drops} slot(s); '
                          'pass/frame coverage is incomplete, game recording was not blocked')
        closure_drops = max((r.get('background_dropped_closures', 0) for r in rows), default=0)
        if closure_drops:
            issues.append(f'{stream}: profiler CPU-closure backlog dropped {closure_drops} closure(s); '
                          'frame coverage is incomplete, game recording was not blocked')
        sessions.append(dict(stream=stream, headers=headers, end=ends[0] if len(ends) == 1 else None,
                             clean_end=bool(len(ends) == 1 and len(launch_ids) == 1 and not header_conflict),
                             missing_windows=missing, rotation_evictions=evictions,
                             missing_rotation_files=missing_files, header_conflict=header_conflict,
                             output_failures=output_failures, background_dropped_slots=background_drops,
                             background_dropped_closures=closure_drops,
                             query_drain_timed_out=ends[0].get('query_drain_timed_out') if len(ends) == 1 else None,
                             pending_frame_closures=ends[0].get('pending_frame_closures') if len(ends) == 1 else None,
                             leading_coverage_known=bool(headers), all_queries_retired=
                             ends[0].get('all_queries_retired') if len(ends) == 1 else None,
                             query_retirement=ends[0].get('query_retirement') if len(ends) == 1 else None))
    return sessions


def span_analysis(by_type, issues):
    invalid_frames = set()
    rejected = []
    timing = {}
    reconciliation_failures = 0
    for kind in ('frame', 'window'):
        valid_span = valid_gap = measured = 0.0
        invalid = valid = 0
        for observation in by_type[kind]:
            row = observation['row']
            measured += row['measured_ms']
            span, gap = row['span_ms'], row['gap_ms']
            frames = 1 if kind == 'frame' else row.get('frames', row['window_end'] - row['window_start'] + 1)
            reason = None
            if row.get('span_valid') is False:
                reason = row.get('span_reason') or 'producer-invalid-span'
            elif span is not None and span > SPAN_LIMIT_MS * max(1, frames):
                reason = 'implausible-timestamp'
            elif span is None or gap is None:
                reason = 'missing-endpoints'
            elif gap < -0.01 and not row.get('sample_budget'):
                reason = 'negative-gap'
            elif kind == 'window' and any(
                    stream == observation['stream'] and row['window_start'] <= frame <= row['window_end']
                    for stream, frame in invalid_frames):
                reason = 'contains-invalid-frame-span'
            if reason:
                invalid += 1
                if kind == 'frame':
                    invalid_frames.add((observation['stream'], row['frame']))
                valid_span += row.get('valid_span_ms', 0)
                valid_gap += row.get('valid_gap_ms', 0)
                rejected.append(dict(source=observation['source'], type=kind, reason=reason,
                                     frame=row.get('frame'), window_start=row.get('window_start'),
                                     raw_span_ms=row.get('raw_span_ms', span),
                                     measured_ms=row['measured_ms'], source_schema=row.get('source_schema', 3),
                                     endpoint_validity='not reconstructible from aggregate capture',
                                     timestamps={key: row[key] for key in (
                                         'first_tick', 'last_tick', 'timestamp_bits', 'timestamp_period',
                                         'timestamp_period_ns', 'queue_family', 'unavailable_timestamps',
                                         'invalid_timestamps', 'timestamp_wraps', 'span_reason') if key in row}))
                continue
            assert span is not None and gap is not None
            valid += 1
            valid_span += span
            valid_gap += gap
            if not math.isclose(span, row['measured_ms'] + gap, abs_tol=0.01, rel_tol=1e-5):
                reconciliation_failures += 1
                issues.append(f'{observation["source"]}: span != measured + gap')
        timing[kind] = dict(measured_ms=measured, span_ms=None if invalid else valid_span,
                            gap_ms=None if invalid else valid_gap, valid_span_ms=valid_span,
                            valid_gap_ms=valid_gap, valid_rows=valid, invalid_rows=invalid)
    if rejected:
        issues.append(f'{len(rejected)} invalid/implausible frame or window spans excluded; measured pass sums preserved')
    return timing, rejected, invalid_frames, reconciliation_failures


def sampled_report(by_type, timing):
    budgets = sorted({o['row'].get('sample_budget', o['row'].get('sampling', {}).get('budget', 0))
                      for observations in by_type.values() for o in observations} - {0})
    if not budgets:
        return None
    frames = []
    for kind in ('frame', 'window'):
        rows = [o['row'] for o in by_type[kind] if o['row'].get('sample_budget')]
        if not rows:
            continue
        bounds = [(2 * row['sample_budget'] + 2) * (
            1 if kind == 'frame' else row.get('frames', row['window_end'] - row['window_start'] + 1))
            for row in rows]
        counts = [row['timestamp_count'] for row in rows]
        timing[kind].update(
            timestamp_count=sum(counts), timestamp_count_min=min(counts), timestamp_count_max=max(counts),
            timestamp_count_bound=sum(bounds), timestamp_count_max_bound=max(bounds),
            timestamp_count_rows=len(rows), sample_budgets=sorted({row['sample_budget'] for row in rows}),
            observed_passes=sum(row['observed_passes'] for row in rows),
            sample_count=sum(row['sampled_passes' if kind == 'frame' else 'sample_count'] for row in rows),
            timing_scope='main-frame' if len(rows) == len(by_type[kind]) else 'mixed',
            measured_ms_scope='sampled-pass-sum' if len(rows) == len(by_type[kind]) else 'mixed-full-and-sampled-pass-sum',
            gap_ms_scope='span-minus-sampled-pass-sum' if len(rows) == len(by_type[kind]) else 'mixed')
        if kind == 'frame':
            timing[kind]['sampled_passes'] = timing[kind].pop('sample_count')
    for observation in sorted(by_type['frame'], key=lambda o: (o['stream'], o['row']['frame'])):
        row = observation['row']
        if row.get('sample_budget'):
            frames.append(dict(
                stream=observation['stream'], **{key: row[key] for key in (
                    'frame', 'sample_budget', 'observed_passes', 'sampled_passes', 'timestamp_count', 'timing_scope')},
                timestamp_count_bound=2 * row['sample_budget'] + 2))
    sampled_passes = [o['row'] for o in by_type['pass'] if 'sampling' in o['row']]
    mixed = any('sampling' not in o['row'] for kind in ('pass', 'long_frame') for o in by_type[kind]) or any(
        not o['row'].get('sample_budget') for kind in ('frame', 'window') for o in by_type[kind])
    return dict(mode='mixed' if mixed else 'sampled', budgets=budgets, aggregation='whole-input',
                gpu_ms_scope='sampled-pass-sum', frame_span_scope='main-frame',
                observed_count=sum(row['count'] for row in sampled_passes),
                sample_count=sum(row['sampling']['sample_count'] for row in sampled_passes),
                histogram_buckets=HISTOGRAM_BUCKETS, percentile_scope='histogram-bucket-upper-bound',
                frames=frames)


def warn_thin(groups, label, issues):
    for group in groups:
        sampling = group.get('sampling')
        if sampling and sampling['thin'] is True:
            issues.append(
                f'{label} {",".join(group["keys"])} ordinal(s) {group["ordinals"]}: thin sampled coverage '
                f'over whole input; sampled_frames={sampling["sampled_frames_bounds"]}, '
                f'observed_frames={sampling["observed_frames_bounds"]}, '
                f'frame_coverage={sampling["frame_coverage_bounds"]} '
                '(fewer than 10 sampled frames)')


def warn_p99(groups, label, issues):
    for group in groups:
        if group.get('p99_descriptive_only') and group['sampling']['sample_count']:
            issues.append(
                f'{label} {",".join(group["keys"])} ordinal(s) {group["ordinals"]}: '
                f'p99 has only {group["sampling"]["sample_count"]} samples over whole input; '
                'fewer than 100 samples gives a descriptive histogram quantile, '
                'not a reliable stutter-tail estimate')


def replay_zero_reasons(row, keys, prefix):
    """Missing old authority is unknown, never an implicit zero."""
    return [f'{prefix}-{key}-nonzero' if key in row else f'unknown-{prefix}-{key}'
            for key in keys if type(row.get(key)) is not int or row[key] != 0]


def frame_close_interval(row, header):
    """Recompute only unwrapped, explicitly available full-mode GPU endpoints."""
    reasons = replay_zero_reasons(
        row, ('unavailable_timestamps', 'invalid_timestamps', 'timestamp_wraps', 'dropped'), 'frame')
    if row.get('span_valid') is not False or row.get('span_reason') != 'incomplete-records':
        reasons.append('not-incomplete-records-only')
    if row.get('reconciliation_failed') is not True:
        reasons.append('unknown-original-reconciliation')
    if row.get('sample_budget', 0):
        reasons.append('sampled-endpoint-not-reconstructible')
    for key in ('failed_slots', 'stats_failed_slots', 'output_failures', 'background_dropped_slots',
                'background_dropped_closures', 'long_dropped'):
        if row.get(key, 0) != 0:
            reasons.append(f'frame-{key}-nonzero')
    try:
        first, last = (number(row, key, True) for key in ('first_tick', 'last_tick'))
        bits = int(number(row, 'timestamp_bits', True, 1))
        period = number(row, 'timestamp_period', minimum=0)
        queue_family = number(row, 'queue_family', True)
        raw = number(row, 'raw_span_ms')
    except ValueError as error:
        return None, None, sorted(set([*reasons, f'unknown-endpoint-authority: {error}']))
    if bits > 64 or period <= 0 or queue_family > UINT32_MAX or first >= 1 << bits or last >= 1 << bits:
        return None, None, sorted(set([*reasons, 'invalid-timestamp-domain']))
    if (bits != header.get('timestamp_bits') or
            not math.isclose(period, header.get('timestamp_period_ns', 0), rel_tol=1e-9, abs_tol=1e-9)):
        reasons.append('timestamp-domain-header-mismatch')
    # Do not reinterpret a backwards/corrupt timestamp as a possible counter wrap.
    if last < first:
        return None, None, sorted(set([*reasons, 'backwards-or-wrapped-endpoints']))
    span = (last - first) * period / 1e6
    if not math.isfinite(span) or span > min(SPAN_LIMIT_MS, header.get('span_limit_ms', SPAN_LIMIT_MS)):
        reasons.append('implausible-timestamp')
    if not math.isclose(raw, span, rel_tol=1e-9, abs_tol=1e-6):
        reasons.append('raw-span-endpoint-mismatch')
    gap = span - row['measured_ms']
    if gap < 0:
        reasons.append('negative-full-mode-gap')
    return span, gap, sorted(set(reasons))


def frame_close_replay_report(paths, by_type, counts, sessions):
    """A conditional historical experiment, never replacement timing authority."""
    frames, windows, streams = [], [], []
    input_reasons = [f'unknown-input-{key}' for key in (
        'malformed_rows', 'truncated_rows', 'conflicting_rows') if counts[key]]
    for session in sessions:
        stream = session['stream']
        headers = [o for o in by_type['header'] if o['stream'] == stream]
        authority_reasons = list(input_reasons)
        header = headers[0]['row'] if headers else {}
        if not headers:
            authority_reasons.append('unknown-build-header')
        if session['header_conflict']:
            authority_reasons.append('conflicting-build-headers')
        commit = header.get('build_commit')
        asset = header.get('build_asset')
        if (not isinstance(commit, str) or not re.fullmatch(r'd003f2db137c[0-9a-f]{0,28}', commit) or
                not isinstance(asset, str) or asset.strip() != f'commit {commit}' or
                header.get('build_identity_source') != 'apk:assets/build.txt' or
                header.get('build_identity_reason') is not None):
            authority_reasons.append('unknown-d003f2d-build-authority')
        if header.get('sample_budget') != 0:
            authority_reasons.append('not-authenticated-full-mode')
        if header.get('window_frames') != WINDOW_FRAMES:
            authority_reasons.append('unknown-window-size')
        try:
            bits = number(header, 'timestamp_bits', True, 1)
            period = number(header, 'timestamp_period_ns')
            if bits > 64 or period <= 0:
                raise ValueError('invalid timestamp domain')
        except ValueError:
            authority_reasons.append('unknown-header-timestamp-domain')
        # Footer failures have no frame-local extent, so they cannot be assigned away.
        end = session['end'] or {}
        if end.get('output_failures') or end.get('background_dropped_slots') or end.get('background_dropped_closures'):
            authority_reasons.append('session-end-reports-loss')
        if end and end['launch_id'] != header.get('launch_id'):
            authority_reasons.append('inconsistent-session-launch')
        stream_frames = sorted((o for o in by_type['frame'] if o['stream'] == stream),
                               key=lambda o: o['row']['frame'])
        indexed = {o['row']['frame']: o for o in stream_frames}
        stream_windows = sorted((o for o in by_type['window'] if o['stream'] == stream),
                                key=lambda o: o['row']['window_start'])
        contexts = {}
        for observation in stream_windows:
            row = observation['row']
            start, stop = row['window_start'], row['window_end']
            local = [o for frame, o in indexed.items() if start <= frame <= stop]
            local.sort(key=lambda o: o['row']['frame'])
            ids = [o['row']['frame'] for o in local]
            reasons = list(authority_reasons)
            if (stop - start + 1 != WINDOW_FRAMES or (start - 1) % WINDOW_FRAMES or
                    row.get('frames') != WINDOW_FRAMES or row.get('partial') is not False or
                    row.get('sequence') != (start - 1) // WINDOW_FRAMES + 1):
                reasons.append('incomplete-or-noncanonical-window')
            missing = (sorted(set(range(start, stop + 1)) - set(ids))
                       if stop - start + 1 == WINDOW_FRAMES else [])
            if missing:
                reasons.append('missing-frame-ids')
            if row.get('sample_budget', 0):
                reasons.append('sampled-window-not-reconstructible')
            reasons.extend(replay_zero_reasons(row, (
                'dropped', 'failed_slots', 'stats_failed_slots', 'unavailable_timestamps',
                'invalid_timestamps', 'output_failures', 'background_dropped_slots'), 'window'))
            # This additive counter did not exist in the affected d003f2d producer.
            if row.get('background_dropped_closures', 0) != 0:
                reasons.append('window-background_dropped_closures-nonzero')
            if len({o['row']['queue_family'] for o in local if type(o['row'].get('queue_family')) is int}) > 1:
                reasons.append('mixed-queue-domains')
            invalid = sum(o['row'].get('span_valid') is False for o in local)
            if row.get('invalid_spans') != invalid or row.get('span_frames') != len(local) - invalid:
                reasons.append('window-span-count-mismatch')
            valid_rows = [o['row'] for o in local if o['row'].get('span_valid') is not False]
            for key, frame_key in (('valid_span_ms', 'span_ms'), ('valid_gap_ms', 'gap_ms')):
                if key not in row or not math.isclose(
                        row[key], math.fsum(r[frame_key] for r in valid_rows), abs_tol=0.01, rel_tol=1e-5):
                    reasons.append(f'window-{key}-mismatch')
            pass_rows = [o['row'] for o in by_type['pass'] if o['stream'] == stream and
                         o['row']['window_start'] == start and o['row']['window_end'] == stop]
            measured = math.fsum(o['row']['measured_ms'] for o in local)
            pass_measured = math.fsum(r['gpu_ms'] for r in pass_rows)
            if not pass_rows:
                reasons.append('unknown-pass-coverage')
            if not math.isclose(measured, row['measured_ms'], abs_tol=0.01, rel_tol=1e-5):
                reasons.append('frame-window-measured-mismatch')
            if not math.isclose(pass_measured, row['measured_ms'], abs_tol=0.01, rel_tol=1e-5):
                reasons.append('pass-window-measured-mismatch')
            if any(bool(r['ordinal']) != row.get('ordinals') for r in pass_rows):
                reasons.append('pass-window-ordinal-mismatch')
            context = dict(stream=stream, source=observation['source'], window_start=start, window_end=stop,
                           original=row, reasons=sorted(set(reasons)), missing_frame_ids=missing,
                           frame_measured_ms=measured, pass_measured_ms=pass_measured, frame_results=[])
            windows.append(context)
            for frame in ids:
                contexts.setdefault(frame, []).append(context)
        stream_results = []
        for observation in stream_frames:
            row = observation['row']
            matching = contexts.get(row['frame'], [])
            reasons = list(authority_reasons)
            span = gap = None
            if len(matching) != 1:
                reasons.append('unknown-window-summary' if not matching else 'overlapping-window-summaries')
            else:
                reasons.extend(matching[0]['reasons'])
            # Invalid/missing header domains are not safe arguments to arithmetic.
            if 'unknown-header-timestamp-domain' not in authority_reasons:
                span, gap, interval_reasons = frame_close_interval(row, header)
                reasons.extend(interval_reasons)
            result = dict(stream=stream, source=observation['source'], frame=row['frame'], original=row,
                          eligible=not reasons, reasons=sorted(set(reasons)),
                          window_start=matching[0]['window_start'] if len(matching) == 1 else None,
                          window_end=matching[0]['window_end'] if len(matching) == 1 else None,
                          counterfactual_span_ms=span if not reasons else None,
                          counterfactual_gap_ms=gap if not reasons else None)
            frames.append(result)
            stream_results.append(result)
            for context in matching:
                context['frame_results'].append(result)
        stream_window_results = [w for w in windows if w['stream'] == stream]
        for context in stream_window_results:
            results = context.pop('frame_results')
            context['eligible_frame_ids'] = [r['frame'] for r in results if r['eligible']]
            context['rejected_frame_ids'] = [r['frame'] for r in results if not r['eligible']]
            if context['rejected_frame_ids']:
                context['reasons'] = sorted(set([*context['reasons'], 'contains-rejected-frames']))
            context['eligible'] = not context['reasons'] and len(results) == WINDOW_FRAMES
            context['counterfactual_span_ms'] = (
                math.fsum(r['counterfactual_span_ms'] for r in results) if context['eligible'] else None)
            context['counterfactual_gap_ms'] = (
                context['counterfactual_span_ms'] - context['original']['measured_ms'] if context['eligible'] else None)
        streams.append(dict(
            stream=stream, header_sources=[o['source'] for o in headers],
            authority_reasons=sorted(set(authority_reasons)),
            eligible_frame_ids=[r['frame'] for r in stream_results if r['eligible']],
            rejected_frame_ids=[r['frame'] for r in stream_results if not r['eligible']],
            eligible_windows=[[w['window_start'], w['window_end']] for w in stream_window_results if w['eligible']],
            rejected_windows=[[w['window_start'], w['window_end']] for w in stream_window_results if not w['eligible']]))
    provenance = []
    for path in paths:
        digest = hashlib.sha256()
        with path.open('rb') as file:
            for chunk in iter(lambda: file.read(1024 * 1024), b''):
                digest.update(chunk)
        provenance.append(dict(path=str(path), sha256=digest.hexdigest(), bytes=path.stat().st_size))
    return dict(
        schema='totk-gpuprof-frame-close-replay/1', mode='counterfactual-only', device_correction_verified=False,
        affected_build='d003f2db137c', files=provenance, streams=streams, frames=frames, windows=windows,
        method='Conditional CPU-closure-only replay of authenticated d003f2d full-mode captures. '
               'Eligibility is not proof of missing CPU closure versus unrecorded work: individual lease/closure '
               'events were not captured. Require complete contiguous 300-frame windows, explicit zero loss/query/'
               'failure counters, available unwrapped GPU endpoints with matching header domain, bounded '
               'recomputed raw span, nonnegative full-mode gap and frame/pass/window sum consistency. '
               'Missing authority is rejected/unknown, never assumed zero (except the then-nonexistent '
               'background_dropped_closures field). No sampled endpoint is fabricated. Original rows, invalidity, '
               'ordinary analysis, capture closure/retirement uncertainty and measured pass sums remain unchanged. '
               'No device rerun or post-fix device correctness/performance measurement is implied.')


def analyze(paths, legacy_span_replay=False, per_window=False, frame_close_replay=False):
    if legacy_span_replay and frame_close_replay:
        raise ValueError('frame-close replay requires schema-3 pass/window authority, not legacy timing-only replay')
    paths = inputs(paths)
    observations, counts, issues = load(paths, legacy_span_replay)
    by_type = {kind: [] for kind in ('header', 'session_end', 'window', 'pass', 'frame', 'long_frame')}
    for observation in observations:
        by_type[observation['row']['type']].append(observation)
    windows = {(o['stream'], o['row']['window_start'], o['row']['window_end']): o['row']
               for o in by_type['window']}
    pass_sums = {}
    for observation in by_type['pass']:
        row = observation['row']
        ident = (observation['stream'], row['window_start'], row['window_end'])
        pass_sums[ident] = pass_sums.get(ident, 0.0) + row['gpu_ms']
    missing_windows = sorted(set(pass_sums) - set(windows))
    mismatches = [] if legacy_span_replay else [
        ident for ident, row in windows.items()
        if not row.get('sample_budget') and
        not math.isclose(pass_sums.get(ident, 0.0), row['measured_ms'], abs_tol=0.01, rel_tol=1e-5)]
    if missing_windows:
        issues.append(f'{len(missing_windows)} pass window(s) have no summary (rotation/capture may be incomplete)')
    if mismatches:
        issues.append(f'{len(mismatches)} window(s) have pass sums different from measured_ms (censored rows)')
    sessions = retention(observations, issues)
    timing, rejected, invalid_frames, reconciliation_failures = span_analysis(by_type, issues)
    sampling = sampled_report(by_type, timing)
    censored_frames = sum(bool(o['row']['dropped'] or o['row'].get('reconciliation_failed') or
                               (o['stream'], o['row']['frame']) in invalid_frames) for o in by_type['frame'])
    rejected_windows = {r['source'] for r in rejected if r['type'] == 'window'}
    censored_windows = sum(bool(o['row']['dropped'] or o['row'].get('failed_slots') or o['row'].get('partial') or
                                o['row'].get('background_dropped_slots') or o['row'].get('background_dropped_closures') or
                                o['source'] in rejected_windows) for o in by_type['window'])
    ordinal_mismatches = sum(
        'ordinals' in windows.get((o['stream'], o['row']['window_start'], o['row']['window_end']), {}) and
        bool(o['row']['ordinal']) != windows[(o['stream'], o['row']['window_start'], o['row']['window_end'])]['ordinals']
        for o in by_type['pass'])
    if ordinal_mismatches:
        issues.append(f'{ordinal_mismatches} pass row(s) disagree with window ordinals metadata')
    frame_ids = {}
    for observation in by_type['frame']:
        frame_ids.setdefault(observation['stream'], []).append(observation['row']['frame'])
    missing_frames = sum(b - a - 1 for values in frame_ids.values()
                         for a, b in zip(sorted(values), sorted(values)[1:]) if b > a + 1)
    pass_rows = [o['row'] for o in by_type['pass']]
    long_rows = [o['row'] for o in by_type['long_frame']]
    pass_contexts = [sampling_context(o['row'], o['stream']) for o in by_type['pass']]
    long_contexts = [sampling_context(o['row'], o['stream']) for o in by_type['long_frame']]
    passes = aggregate(pass_rows, contexts=pass_contexts)
    families = aggregate(pass_rows, True, pass_contexts)
    warn_thin(passes, 'Pass', issues)
    warn_thin(families, 'Family', issues)
    warn_p99(passes, 'Pass', issues)
    warn_p99(families, 'Family', issues)
    report = {
        'schema': 'totk-gpuprof-analysis/3', 'files': list(map(str, paths)), 'sessions': sessions,
        'legacy_span_replay': legacy_span_replay, 'rejected_spans': rejected,
        'coverage': {**counts, 'streams': len({o['stream'] for o in observations}),
                     'windows': len(windows), 'pass_rows': len(pass_rows), 'frame_rows': len(by_type['frame']),
                     'long_frame_rows': len(long_rows), 'long_frames': len({(o['stream'], o['row']['frame'])
                                                                          for o in by_type['long_frame']}),
                     'missing_frame_ids': missing_frames, 'missing_window_summaries': len(missing_windows),
                     'window_sum_mismatches': len(mismatches), 'censored_frames': censored_frames,
                     'censored_windows': censored_windows, 'reconciliation_failures': reconciliation_failures,
                     'invalid_frame_spans': timing['frame']['invalid_rows'],
                     'invalid_window_spans': timing['window']['invalid_rows'],
                     'unavailable_timestamps': sum(o['row'].get('unavailable_timestamps', 0) for o in by_type['frame']),
                     'invalid_timestamps': sum(o['row'].get('invalid_timestamps', 0) for o in by_type['frame']),
                     'timestamp_wraps': sum(o['row'].get('timestamp_wraps', 0) for o in by_type['frame']),
                     'producer_invalid_spans': sum(r.get('invalid_spans', 0) for r in windows.values()),
                     'missing_windows': sum(s['missing_windows'] for s in sessions),
                     'rotation_evictions': sum(s['rotation_evictions'] for s in sessions),
                     'missing_rotation_files': sum(s['missing_rotation_files'] for s in sessions),
                     'streams_without_end': sum(not s['clean_end'] for s in sessions),
                     'header_conflicts': sum(s['header_conflict'] for s in sessions),
                     'output_failures': sum(s['output_failures'] for s in sessions),
                     'background_dropped_slots': sum(s['background_dropped_slots'] for s in sessions),
                     'background_dropped_closures': sum(s['background_dropped_closures'] for s in sessions),
                     'ordinal_metadata_mismatches': ordinal_mismatches,
                     'stats_missing': sum(r.get('sampling', {}).get('sample_count', r['count']) - r['stats_valid']
                                          for r in pass_rows),
                     'dropped': sum(r['dropped'] for r in windows.values()),
                     'failed_slots': sum(r.get('failed_slots', 0) for r in windows.values()),
                     'stats_failed_slots': sum(r.get('stats_failed_slots', 0) for r in windows.values())},
        'timing': timing,
        'passes': passes, 'families': families,
        'long_frame_passes': aggregate(long_rows, contexts=long_contexts),
        'long_frame_families': aggregate(long_rows, True, long_contexts),
        'issues': issues,
        'method': 'Sum times/counters before division; ratios never averaged. Ordinal 0 means all frame ordinals '
                  '(window aggregates only); positive ordinals identify individual frame positions. Time/draw ratios '
                  'use explicitly matched statistics time/draws, or complete-stat rows when absent. Families ignore '
                  'shader/key/ordinal but keep kind, samples, scene_relative and render/attachment layout. '
                  'Scene-relative layouts use exact scene-reference ratios; other layouts use absolute dimensions. '
                  'All contributing exact keys, absolute areas and scene-reference variants remain in areas. '
                  'Long-frame rows are threshold-selected, not a complete frame population. Window/frame timing '
                  'are separate views, never added together. clean_end proves writer closure only; '
                  'all_queries_retired is independently checked against lease/query retirement counts. '
                  'Historical footers without counts leave retirement unknown. Retired leases have fenced '
                  'results read/attempted; retired queries require available results (successful statistics reads). '
                  'Counts cover activated query instances, not reserved pools or individual statistics counters. '
                  'Spans above 10000 ms/frame are rejected even if an older producer reported reconciliation success. '
                  'Legacy replay reads frame/window timing only; it cannot reconstruct query availability, original '
                  'endpoints or schema-3 attachment/reference semantics.',
    }
    if sampling is not None:
        report['sampling'] = sampling
        report['method'] += (
            ' Sampled gpu_ms/measured_ms are sums of valid samples only, never estimates of total pass work; '
            'whole-main-frame spans remain separate. Sampled gap_ms is a numerical span-minus-samples '
            'residual, not total untimed work; pre-frame uploads or overlap may make it negative without '
            'invalidating the span. Means divide sampled sums by sample_count. Histograms '
            'are merged before percentile selection; percentiles are bucket upper bounds, never averages. '
            'P99 with fewer than 100 samples is a descriptive histogram quantile, not a reliable stutter-tail '
            'estimate. Coverage/thin warnings use the whole input interval, not individual windows. '
            'Thin means fewer than 10 sampled frames; coverage fractions are descriptive, not a hard '
            'percentage gate for deliberately sparse sampling. Distinct-frame '
            'unions across keys/ordinals/reference variants use bounds when exact IDs cannot be recovered; '
            'null coverage/thin means the bounds do not determine an exact value/status. Missing key '
            'observations remain censored, not zero. Sampled time/draw uses matched statistics only.')
    if per_window:
        grouped = {ident: [] for ident in windows}
        for observation in by_type['pass']:
            row = observation['row']
            grouped.setdefault((observation['stream'], row['window_start'], row['window_end']), []).append(row)
        report['per_window'] = [
            dict(stream=stream, window_start=start, window_end=end,
                 passes=aggregate(rows), families=aggregate(rows, True))
            for (stream, start, end), rows in sorted(grouped.items())]
    if frame_close_replay:
        report['frame_close_replay'] = frame_close_replay_report(paths, by_type, counts, sessions)
    return report


def fmt(value):
    return '-' if value is None else f'{value:.3f}' if isinstance(value, float) else str(value)


def render_table(title, groups, out):
    print(f'\n## {title}', file=out)
    print('key(s) / ordinal(s) | kind | render area(s) | GPU sum ms | passes | draws | stats valid/missing | '
          'verts/draw | prims/draw | fragments/prim | ns/vertex | ns/primitive', file=out)
    for group in groups:
        ratios = group['ratios'] or {}
        render_areas = sorted({(a['render_width'], a['render_height']) for a in group['areas']})
        ordinals = ','.join('all (0)' if ordinal == 0 else str(ordinal) for ordinal in group['ordinals'])
        values = [','.join(group['keys']) + ' / ' + ordinals, group['kind'],
                  ','.join(f'{w}x{h}' for w, h in render_areas), group['gpu_ms'], group['count'], group['draws'],
                  f'{group["stats_valid"]}/{group["stats_missing"]}', *(ratios.get(k) for k in RATIOS)]
        print(' | '.join(map(fmt, values)), file=out)
        print(f'  structure: samples={group["samples"]} areas='
              f'{json.dumps(group["areas"], sort_keys=True, separators=(",", ":"))} '
              f'shaders={",".join(group["shaders"])}', file=out)
        print('  counters: ' + ('unavailable' if group['stats'] is None else
              ' '.join(f'{k}={group["stats"][k]}' for k in STATS)) +
              f'; ratio matched draws={group["ratio_draws"]} ms={group["ratio_gpu_ms"]:.3f}'
              f'; partial-stat rows={group["stats_partial_rows"]}', file=out)
        if 'sampling' in group:
            sampling = group['sampling']
            print(
                f'  sampling: budgets={sampling["budgets"]} observed_count={sampling["observed_count"]} '
                f'sample_count={sampling["sample_count"]} sampled GPU sum ms={group["gpu_ms"]:.6f} '
                f'mean_ms={fmt(group["mean_ms"])} p50/p90/p99_ms (bucket upper bounds)='
                f'{fmt(group["p50_ms"])}/{fmt(group["p90_ms"])}/{fmt(group["p99_ms"])} '
                f'observed_frames={sampling["observed_frames_bounds"]} '
                f'sampled_frames={sampling["sampled_frames_bounds"]} '
                f'frame_coverage={sampling["frame_coverage_bounds"]} coverage_status={sampling["coverage_status"]} '
                f'p99_descriptive_only={group["p99_descriptive_only"]}',
                file=out)


def render(report, out=sys.stdout):
    print('# GPU profile schema 3', file=out)
    print(report['method'], file=out)
    print('Coverage: ' + ' '.join(f'{k}={v}' for k, v in report['coverage'].items()), file=out)
    for session in report['sessions']:
        retired = session['all_queries_retired']
        timed_out = session['query_drain_timed_out']
        print(f'Session: {session["stream"]} clean_end={str(session["clean_end"]).lower()} '
              f'all_queries_retired={"unknown" if retired is None else str(retired).lower()} '
              f'query_drain_timed_out={"unknown" if timed_out is None else str(timed_out).lower()}', file=out)
        retirement = session['query_retirement']
        print('  query_retirement: ' + ('unknown (no accounting footer)' if retirement is None else
              ' '.join(f'{key}={retirement[key]}' for key in RETIREMENT_COUNTS)), file=out)
    for kind, timing in report['timing'].items():
        print(f'{kind} timing sums: ' + ' '.join(f'{k}={fmt(v)}' for k, v in timing.items()), file=out)
    if 'frame_close_replay' in report:
        replay = report['frame_close_replay']
        print('\n## Counterfactual d003f2d CPU frame-close replay (NOT device correction)', file=out)
        print(replay['method'], file=out)
        for stream in replay['streams']:
            print('Replay stream: ' + json.dumps(stream, sort_keys=True), file=out)
        rejected = {}
        for frame in replay['frames']:
            if not frame['eligible']:
                rejected.setdefault((frame['stream'], tuple(frame['reasons'])), []).append(frame['frame'])
        for (stream, reasons), frame_ids in sorted(rejected.items()):
            print('Replay rejected frames: ' + json.dumps(
                dict(stream=stream, reasons=reasons, frame_ids=frame_ids), sort_keys=True), file=out)
        for window in replay['windows']:
            print('Replay window: ' + json.dumps(window, sort_keys=True), file=out)
    if 'sampling' in report:
        sampling = report['sampling']
        print('Sampling: ' + ' '.join(f'{key}={value}' for key, value in sampling.items() if key != 'frames'), file=out)
        for frame in sampling['frames']:
            print('Sampled frame: ' + ' '.join(f'{key}={value}' for key, value in frame.items()), file=out)
    for issue in report['issues']:
        print(f'WARNING: {issue}', file=out)
    render_table('Per pass (exact key; ordinal 0 = all)', report['passes'], out)
    render_table('Structural families (shader ignored)', report['families'], out)
    if report['long_frame_passes']:
        render_table('Selected long-frame passes (not window totals)', report['long_frame_passes'], out)
        render_table('Selected long-frame families', report['long_frame_families'], out)
    for window in report.get('per_window', []):
        title = f'{window["stream"]} window {window["window_start"]}-{window["window_end"]}'
        render_table(f'Per-window passes: {title}', window['passes'], out)
        render_table(f'Per-window families: {title}', window['families'], out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('inputs', nargs='+', help='JSONL files or quoted globs; include .jsonl.* for rotations')
    parser.add_argument('--json', metavar='FILE', help='write machine-readable analysis; - writes JSON only to stdout')
    parser.add_argument('--legacy-span-replay', action='store_true',
                        help='accept schema-2 frame/window timing only; do not reinterpret old pass areas')
    parser.add_argument('--per-window', action='store_true',
                        help='add window-local pass/family views; default aggregates the whole input interval')
    parser.add_argument('--frame-close-replay', action='store_true',
                        help='add strict counterfactual d003f2d CPU-closure replay; original invalidity is unchanged')
    args = parser.parse_args(argv)
    try:
        report = analyze(args.inputs, args.legacy_span_replay, args.per_window, args.frame_close_replay)
        if args.json:
            text = json.dumps(report, indent=2, allow_nan=False) + '\n'
            if args.json == '-':
                sys.stdout.write(text)
            else:
                target = Path(args.json).resolve()
                guard_output(target)
                if str(target) in report['files']:
                    raise ValueError('JSON output must not overwrite an input')
                target.write_text(text, encoding='utf-8')
        if args.json != '-':
            render(report)
    except (OSError, ValueError) as error:
        parser.exit(2, f'gpuprof: {error}\n')
    return int(bool(report['coverage']['malformed_rows'] or report['coverage']['conflicting_rows']))


if __name__ == '__main__':
    sys.exit(main())

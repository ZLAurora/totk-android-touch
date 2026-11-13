"""Compile provenance from existing 300-present graphics stutter reports, never inferred from FPS.

The overlay COMPILE row is a rolling one-second, synchronous-only image, not a log counter.
Shader translation messages and optional compute events have no frame identity; do not assign them
by nearby text order or claim their absence establishes zero compilation.
"""
import re


STUTTER = re.compile(r'\[nvn-vk\] frame (\d+), stutter:')


def compile_record(line):
    marker = STUTTER.search(line)
    if not marker:
        return None
    record = {'source': 'nvn-vk-stutter', 'scope': 'graphics-300-present',
              'shader_compiles': None, 'compute_compiles': None,
              'unavailable': 'shader translation and compute compilation have no always-on frame-keyed counts'}
    for key, pattern in (
            ('pipeline_creations', r'; (\d+) new pipelines took ([\d.]+) ms'),
            ('pipeline_async', r'; (\d+) compiled on worker threads in ([\d.]+) ms'),
            ('pipeline_sync', r'; sync compiles (\d+) in ([\d.]+) ms')):
        match = re.search(pattern, line)
        record[key] = int(match[1]) if match else None
        record[key + '_ms'] = float(match[2]) if match else None
    counts = [record[k] for k in ('pipeline_sync', 'pipeline_async')]
    record['compiles_present'] = True if any(v for v in counts) else False if None not in counts else None
    return int(marker[1]), record


def compilation_of(end, records):
    return records.get(end, {'source': None, 'scope': 'unavailable', 'shader_compiles': None,
                             'compute_compiles': None, 'pipeline_creations': None,
                             'pipeline_sync': None, 'pipeline_async': None, 'compiles_present': None,
                             'unavailable': 'no frame-keyed compile report for this window'})


def compilation_summary(windows):
    records = [w.get('compilation', {}) for w in windows]
    total = lambda key: sum(r[key] for r in records if r.get(key) is not None) if any(
        r.get(key) is not None for r in records) else None
    return {'compile_windows': sum(r.get('compiles_present') is True for r in records),
            'compile_covered_windows': sum(r.get('compiles_present') is not None for r in records),
            'pipeline_sync_total': total('pipeline_sync'), 'pipeline_async_total': total('pipeline_async'),
            'pipeline_creations_total': total('pipeline_creations'),
            'shader_compiles_total': None, 'compute_compiles_total': None}


def scene_state():
    return {'time_of_day': None, 'weather': None, 'status': 'unavailable',
            'reason': 'current game log and performance overlay expose neither time of day nor weather'}

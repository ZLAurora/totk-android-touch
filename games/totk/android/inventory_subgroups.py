#!/usr/bin/env python3
"""Inventory actual subgroup instructions/builtins, not unused capabilities.

Run over a loose seed. Optional --romfs/--modules uses the existing seed scanner
for source-package provenance; it never translates or modifies game files.
"""
import argparse
from collections import Counter
import hashlib
from contextlib import redirect_stdout
import json
from pathlib import Path
import struct
import sys

OPS = {333: 'elect', 334: 'all', 335: 'any', 336: 'all_equal',
       337: 'broadcast', 338: 'broadcast_first', 339: 'ballot',
       340: 'inverse_ballot', 341: 'ballot_bit_extract', 342: 'ballot_bit_count',
       343: 'ballot_find_lsb', 344: 'ballot_find_msb', 345: 'shuffle',
       346: 'shuffle_xor', 347: 'shuffle_up', 348: 'shuffle_down'}
BUILTINS = {36: 'SubgroupSize', 37: 'SubgroupMaxSize', 38: 'NumSubgroups',
            39: 'NumEnqueuedSubgroups', 40: 'SubgroupId', 41: 'SubgroupLocalInvocationId',
            4416: 'SubgroupEqMask', 4417: 'SubgroupGeMask', 4418: 'SubgroupGtMask',
            4419: 'SubgroupLeMask', 4420: 'SubgroupLtMask'}


def inspect(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('invalid SPIR-V length')
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    ops, builtins, stages = Counter(), Counter(), set()
    builtin_ids, loads = {}, Counter()
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 65535
        if not n or i + n > len(words):
            raise ValueError('invalid SPIR-V instruction')
        if 333 <= op <= 366 or 4421 <= op <= 4432:
            ops[OPS.get(op, 'opcode_%d' % op)] += 1
        if op == 71 and n == 4 and words[i+2] == 11 and words[i+3] in BUILTINS:
            builtins[BUILTINS[words[i+3]]] += 1
            builtin_ids[words[i+1]] = BUILTINS[words[i+3]]
        if op == 72 and n == 5 and words[i+3] == 11 and words[i+4] in BUILTINS:
            builtins[BUILTINS[words[i+4]]] += 1
        if op == 61 and n >= 4 and words[i+3] in builtin_ids:
            loads[builtin_ids[words[i+3]]] += 1
        if op == 15:
            stages.add({0: 'vertex', 1: 'tess_control', 2: 'tess_eval', 3: 'geometry',
                        4: 'fragment', 5: 'compute'}.get(words[i+1], str(words[i+1])))
        i += n
    return dict(operations=dict(ops), builtins=dict(builtins), builtin_loads=dict(loads), stages=sorted(stages))


def inventory(seed, sources=None):
    rows, total = [], 0
    for path in sorted(seed.glob('*.spv')):
        total += 1
        data = path.read_bytes()
        row = inspect(data)
        if not row['operations'] and not row['builtins']:
            continue
        key = path.stem.split('-')[0]
        rows.append(dict(variant=path.stem, key=key, sha256=hashlib.sha256(data).hexdigest(),
                         source=(sources or {}).get(key), **row))
    return dict(modules=total, affected_modules=len(rows),
                operation_modules=sum(bool(r['operations']) for r in rows),
                used_modules=sum(bool(r['operations'] or r['builtin_loads']) for r in rows),
                declaration_only_modules=sum(not r['operations'] and not r['builtin_loads'] for r in rows),
                affected_keys=len({r['key'] for r in rows}), shaders=rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('seed', type=Path)
    p.add_argument('--romfs', type=Path)
    p.add_argument('--modules', type=Path)
    args = p.parse_args()
    sources = {}
    if args.romfs:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
        from prepare_shader_seed import inventory as scan_sources
        with redirect_stdout(sys.stderr):
            _, shaders, _, _ = scan_sources(args.romfs, args.modules)
        sources = {key: value[3] for key, value in shaders.items()}
    if not args.seed.is_dir():
        p.error('seed must be an existing loose SPIR-V directory')
    result = inventory(args.seed, sources)
    if not result['modules']:
        p.error('seed contains no .spv modules')
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()

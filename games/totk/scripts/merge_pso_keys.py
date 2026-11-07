#!/usr/bin/env python3
"""Merge native Vulkan key collections into the canonical identifier/render-state-only TSV.

Use one final cumulative snapshot per device, or independent session collections. Identical
input files are ignored; overlapping cumulative snapshots otherwise double-count frequency.
The 16 MiB shipped budget holds every one of the runtime's bounded 16384 keys (792 B/key).
Asset policy: root64 hashes must occur in shader_list.tsv V rows. Preserve the seed pack's
existing 12-hex binding-signature suffix for exact lookup; it is an identifier, not shader
bytes. Devices missing that exact variant skip the key rather than substituting a shader.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct

LIMIT = 16384
WORDS = 155
KEY_BYTES = 160 + 4 + 4 * WORDS
RECORD = struct.Struct(f'<{KEY_BYTES}sII')
HEADER = struct.Struct('<8sIIII32s8s')
NAME = re.compile(rb'(?:[0-9a-f]{64})(?:-[0-9a-f]{12})?\Z')
BUDGET = 16 << 20
TSV_HEADER = (
    b'# TOTKPSO-TSV 1; identifiers and render state only, no shader code\n'
    b'# Exact root64[-suffix12] seed names; - means no fragment stage.\n'
    b'# state: 155 comma-separated minimal lowercase uint32 hex words, binary order.\n'
    b'# Rows: capture descending, frequency descending, then canonical key bytes.\n'
    b'vs\tfs\tearly\tstate\tcapture\tfrequency\n'
)
TSV_MAX_LINE = 2048
TSV_MAX_BYTES = len(TSV_HEADER) + LIMIT * TSV_MAX_LINE


def name(raw, optional=False):
    value, _, padding = raw.partition(b'\0')
    if padding.strip(b'\0') or len(value) == 80 or not (optional and not value or NAME.fullmatch(value)):
        raise ValueError('invalid shader identity')
    return value


def validate(key, frequency, capture):
    name(key[:80]); name(key[80:160], True)
    early, *s = struct.unpack_from('<156I', key, 160)
    # Stage word: bit 0 early-tests, bits 1..3 alpha comparison (zero is off).
    if early > 15 or not frequency or capture > 1:
        raise ValueError('invalid metadata')
    samples = s[11]
    if (s[10] > 8 or samples > 64 or samples & (samples - 1) or s[12] > 255 or
            s[14] > 1 or s[15] > 2 or s[16] > 2 or s[17] > 1 or s[18] > 16):
        raise ValueError('invalid render state')
    for i in range(8):
        b = s[19+7*i:26+7*i]
        if b[0] > 7 or any(x > 127 for x in b[1:5]) or any(x > 7 for x in b[5:]):
            raise ValueError('invalid blend state')
    for i in range(16):
        if s[75+3*i+2] > 15 or s[123+2*i] > 1:
            raise ValueError('invalid vertex stream')


def read_bounded(path):
    """Read at most one admitted list plus a trailing-byte sentinel, even for a corrupt input."""
    maximum = HEADER.size + LIMIT * RECORD.size
    with path.open('rb') as source:
        data = source.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError(f'{path}: key list exceeds the runtime size limit')
    return data


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated header')
    magic, version, size, count, reserved, sha, tail = HEADER.unpack_from(data)
    payload = data[HEADER.size:]
    if (magic != b'TOTKPS01' or version != 1 or size != RECORD.size or count > LIMIT or
            reserved or tail != bytes(8) or len(payload) != count * size or hashlib.sha256(payload).digest() != sha):
        raise ValueError('incompatible, truncated or checksummed key list')
    records = list(RECORD.iter_unpack(payload))
    for record in records:
        validate(*record)
    return records


def encode(records):
    if len(records) > LIMIT:
        raise ValueError('key count exceeds runtime limit')
    for record in records:
        validate(*record)
    data = b''.join(RECORD.pack(*r) for r in records)
    return HEADER.pack(b'TOTKPS01', 1, RECORD.size, len(records), 0, hashlib.sha256(data).digest(), bytes(8)) + data


def encode_tsv(records):
    """Canonical text source for TOTKPS01; preserve every identifier and all 156 key words."""
    if len(records) > LIMIT:
        raise ValueError('key count exceeds runtime limit')
    rows = [TSV_HEADER]
    seen = set()
    for key, frequency, capture in sorted(records, key=lambda r: (-r[2], -r[1], r[0])):
        validate(key, frequency, capture)
        if not 0 < frequency <= 0xffffffff or key in seen:
            raise ValueError('invalid frequency or duplicate text key')
        seen.add(key)
        early, *state = struct.unpack_from('<156I', key, 160)
        rows.append(b'\t'.join((
            name(key[:80]), name(key[80:160], True) or b'-', str(early).encode(),
            ','.join(f'{word:x}' for word in state).encode(), str(capture).encode(),
            str(frequency).encode())) + b'\n')
    return b''.join(rows)


def decode_tsv(data):
    """Reject noncanonical text, including missing words, duplicate keys and unstable ordering."""
    if len(data) > TSV_MAX_BYTES or not data.startswith(TSV_HEADER):
        raise ValueError('incompatible or oversized PSO TSV')
    rows = data[len(TSV_HEADER):].splitlines()
    if len(rows) > LIMIT:
        raise ValueError('key count exceeds runtime limit')
    records = []
    for row in rows:
        fields = row.split(b'\t')
        if len(row) > TSV_MAX_LINE or len(fields) != 6:
            raise ValueError('invalid PSO TSV row')
        vs, fs, early, state, capture, frequency = fields
        fs = b'' if fs == b'-' else fs
        if not NAME.fullmatch(vs) or fs and not NAME.fullmatch(fs):
            raise ValueError('invalid shader identity')
        words = state.split(b',')
        if len(words) != WORDS:
            raise ValueError('invalid PSO TSV state word count')
        # Width checks precede integer conversion; never accept unbounded decimal/hex inputs.
        if (not re.fullmatch(rb'[0-9]|1[0-5]', early) or capture not in (b'0', b'1') or
                not re.fullmatch(rb'[1-9][0-9]{0,9}', frequency) or
                any(not re.fullmatch(rb'0|[1-9a-f][0-9a-f]{0,7}', word) for word in words)):
            raise ValueError('invalid PSO TSV integer')
        key = vs.ljust(80, b'\0') + fs.ljust(80, b'\0') + struct.pack(
            '<156I', int(early), *(int(word, 16) for word in words))
        records.append((key, int(frequency), int(capture)))
    if encode_tsv(records) != data:
        raise ValueError('noncanonical PSO TSV')
    return records


def read_tsv(path):
    with path.open('rb') as source:
        data = source.read(TSV_MAX_BYTES + 1)
    return decode_tsv(data)


def shader_ids(path):
    # Streaming: the shared list is large; do not retain translation descriptors or game data.
    result = set()
    with path.open(encoding='utf-8') as source:
        for line in source:
            fields = line.split('\t', 2)
            if len(fields) == 3 and fields[0] == 'V' and re.fullmatch('[0-9a-f]{64}', fields[1]):
                result.add(fields[1].encode())
    return result


def merge(inputs, allowed):
    keys = {}; seen = set(); dropped = 0
    for data in inputs:
        digest = hashlib.sha256(data).digest()
        if digest in seen:
            continue
        seen.add(digest)
        for key, frequency, capture in decode(data):
            vs, fs = name(key[:80]), name(key[80:160], True)
            if vs[:64] not in allowed or fs and fs[:64] not in allowed:
                dropped += 1
                continue
            old = keys.get(key, (0, 0))
            if key not in keys and len(keys) == LIMIT:
                raise ValueError(f'unique keys exceed {LIMIT}; split/curate collection explicitly')
            keys[key] = (min(0xffffffff, old[0] + frequency), old[1] | capture)
    records = [(key, *counts) for key, counts in keys.items()]
    records.sort(key=lambda r: (-r[2], -r[1], r[0]))
    return records, dropped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--shader-list', type=Path, default=Path(__file__).resolve().parents[1] / 'assets/shader_list.tsv')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        records, dropped = merge((read_bounded(p) for p in args.inputs), shader_ids(args.shader_list))
        binary = encode(records)
        if len(binary) > BUDGET:
            raise ValueError('shipped list exceeds 16 MiB budget')
        data = encode_tsv(records)
        tmp = args.output.with_name(args.output.name + '.tmp')
        with tmp.open('wb') as output:
            output.write(data); output.flush(); os.fsync(output.fileno())
        os.replace(tmp, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(f'{len(records)} keys, capture={sum(r[2] for r in records)}, dropped_missing_shader={dropped}, '
          f'tsv_bytes={len(data)}, binary_bytes={len(binary)}/{BUDGET}, '
          f'binary_sha256={hashlib.sha256(binary).hexdigest()}')


if __name__ == '__main__':
    main()

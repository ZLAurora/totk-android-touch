"""Portable identifier-only graphics keys: strict format, merge policy and deterministic shipping."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / '.scratch'  # git-ignored; a checkout need not have a sibling .scratch
SCRATCH.mkdir(exist_ok=True)
SPEC = importlib.util.spec_from_file_location('merge_pso_keys', ROOT / 'games/totk/scripts/merge_pso_keys.py')
assert SPEC is not None and SPEC.loader is not None
pso = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pso)


def key(vs='a', fs='b', mask=15):
    result = bytearray(pso.KEY_BYTES)
    result[:64] = (vs * 64).encode()
    if fs:
        result[80:144] = (fs * 64).encode()
    state = [0] * 156
    state[1+10] = 1
    state[1+11] = 1
    state[1+13] = mask
    struct.pack_into('<156I', result, 160, *state)
    return bytes(result)


class PsoKeysTests(unittest.TestCase):
    def test_roundtrip(self):
        records = [(key(), 5, 1), (key(fs='', mask=7), 1, 0)]
        data = pso.encode(records)
        self.assertEqual(pso.decode(data), records)
        self.assertEqual(len(data), 64 + 2 * 792)
        self.assertEqual(data[:8], b'TOTKPS01')
        self.assertEqual(data[24:56], hashlib.sha256(data[64:]).digest())
        self.assertEqual(pso.decode(pso.encode([])), [])

    def test_alpha_stage_word(self):
        for comparison in range(8):
            for early in range(2):
                packed = bytearray(key())
                struct.pack_into('<I', packed, 160, early | comparison << 1)
                records = [(bytes(packed), 1, 0)]
                self.assertEqual(pso.decode(pso.encode(records)), records)
                self.assertEqual(pso.decode_tsv(pso.encode_tsv(records)), records)

    def test_strict_envelope(self):
        data = pso.encode([(key(), 1, 0)])
        for i in (0, 8, 12, 16, 20, 24, 55, 56, 63, 100, len(data)-1):
            bad = bytearray(data); bad[i] ^= 0xff
            with self.subTest(byte=i), self.assertRaises(ValueError):
                pso.decode(bad)
        for bad in (data[:63], data[:-1], data + b'\0'):
            with self.assertRaises(ValueError):
                pso.decode(bad)

    def test_key_validation(self):
        for at, value in ((160, 16), (164+10*4, 9), (164+11*4, 3),
                          (164+16*4, 3), (164+(75+2)*4, 16), (164+123*4, 2)):
            bad = bytearray(key()); struct.pack_into('<I', bad, at, value)
            with self.subTest(byte=at), self.assertRaises(ValueError):
                pso.encode([(bytes(bad), 1, 0)])
        bad = bytearray(key()); bad[79] = 1
        with self.assertRaises(ValueError): pso.encode([(bytes(bad), 1, 0)])
        for frequency, capture in ((0, 0), (1, 2)):
            with self.assertRaises(ValueError): pso.encode([(key(), frequency, capture)])

    def test_merge_dedupe_priority_frequency(self):
        screen = (key(mask=1), 1000, 0)
        capture = (key(mask=2), 2, 1)
        frequent = (key(mask=3), 3, 1)
        a = pso.encode([screen, capture, frequent])
        b = pso.encode([(capture[0], 2, 0), (frequent[0], 1, 0)])
        merged, dropped = pso.merge([a, a, b], {b'a'*64, b'b'*64})
        self.assertEqual(dropped, 0)
        self.assertEqual(merged, [(capture[0], 4, 1), (frequent[0], 4, 1), screen])
        self.assertEqual(pso.encode(merged), pso.encode(pso.merge([b, a], {b'a'*64, b'b'*64})[0]))

    def test_missing_seed_shader_and_empty_fragment(self):
        data = pso.encode([(key(), 1, 1), (key(fs='c'), 1, 0), (key(fs=''), 1, 0), (key(vs='d'), 1, 0)])
        result, dropped = pso.merge([data], {b'a'*64, b'b'*64})
        self.assertEqual(dropped, 2)
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0][2], 1)

    def test_shader_manifest_only_v_identifiers(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as scratch:
            manifest = Path(scratch) / 'shader_list.tsv'
            manifest.write_text('# comment\nV\t' + 'a'*64 + '\t{}\nS\t' + 'b'*64 + '\t0\nV\tnot-a-shader\t{}\n')
            self.assertEqual(pso.shader_ids(manifest), {b'a'*64})

    def test_saturation_and_budget(self):
        a = pso.encode([(key(), 0xffffffff, 0)])
        b = pso.encode([(key(), 1, 1)])
        self.assertEqual(pso.merge([a, b], {b'a'*64, b'b'*64})[0][0][1:], (0xffffffff, 1))
        self.assertLessEqual(pso.HEADER.size + pso.LIMIT * pso.RECORD.size, pso.BUDGET)
        with self.assertRaises(ValueError): pso.encode([(key(), 1, 0)] * (pso.LIMIT + 1))

    def test_bounded_input_and_union(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as scratch:
            path = Path(scratch) / 'list.bin'
            data = pso.encode([(key(), 1, 0)])
            path.write_bytes(data)
            self.assertEqual(pso.read_bounded(path), data)
            with path.open('wb') as output:
                output.truncate(pso.HEADER.size + pso.LIMIT * pso.RECORD.size + 1)
            with self.assertRaises(ValueError):
                pso.read_bounded(path)
        first = pso.encode([(key(mask=1), 1, 0), (key(mask=2), 1, 0)])
        second = pso.encode([(key(mask=3), 1, 0)])
        with mock.patch.object(pso, 'LIMIT', 2), self.assertRaises(ValueError):
            pso.merge([first, second], {b'a'*64, b'b'*64})

    def test_text_roundtrip_and_exact_suffixes(self):
        variant = bytearray(key(mask=0xffffffff))
        variant[64:77] = b'-0123456789ab'
        variant[144:157] = b'-fedcba987654'
        struct.pack_into('<I', variant, 164, 0xffffffff)
        records = [(key(fs=''), 1, 0), (bytes(variant), 0xffffffff, 1)]
        text = pso.encode_tsv(records)
        self.assertEqual(pso.decode_tsv(text), records[::-1])
        self.assertEqual(pso.encode_tsv(records[::-1]), text)
        self.assertIn(b'-0123456789ab\t', text)
        self.assertIn(b'-fedcba987654\t0\tffffffff,', text)
        row = text[len(pso.TSV_HEADER):].splitlines()[0].split(b'\t')
        self.assertEqual(len(row[3].split(b',')), 155)
        self.assertNotIn(b'00000000', row[3])
        self.assertEqual(pso.encode(pso.decode_tsv(text)), pso.encode(records[::-1]))
        self.assertEqual(pso.decode_tsv(pso.encode_tsv([])), [])

    def test_text_rejects_noncanonical_and_incomplete_rows(self):
        text = pso.encode_tsv([(key(), 1, 0)])
        fields = text[len(pso.TSV_HEADER):].rstrip(b'\n').split(b'\t')
        invalid = [text[:-1], text + b'\n', text.replace(b'\n', b'\r\n'),
                   text.replace(b'TOTKPSO-TSV 1', b'TOTKPSO-TSV 2'),
                   text + text[len(pso.TSV_HEADER):]]
        for column, replacement in ((0, b'a'*64 + b'-123'), (1, b''),
                                    (2, b'16'), (3, b'0,' + fields[3]),
                                    (3, fields[3].replace(b'0', b'00', 1)),
                                    (3, fields[3].replace(b'0', b'F', 1)),
                                    (3, fields[3].replace(b'0', b'100000000', 1)),
                                    (4, b'2'), (5, b'0'), (5, b'01'),
                                    (5, b'4294967296')):
            bad = fields.copy(); bad[column] = replacement
            invalid.append(pso.TSV_HEADER + b'\t'.join(bad) + b'\n')
        ordered = pso.encode_tsv([(key(mask=1), 2, 1), (key(mask=2), 1, 0)])
        invalid.append(pso.TSV_HEADER + b'\n'.join(
            ordered[len(pso.TSV_HEADER):].splitlines()[::-1]) + b'\n')
        for data in invalid:
            with self.subTest(data=data[-80:]), self.assertRaises(ValueError):
                pso.decode_tsv(data)
        with mock.patch.object(pso, 'LIMIT', 0), self.assertRaises(ValueError):
            pso.decode_tsv(text)

    def test_text_bounded_read_and_merge_cli(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as scratch:
            path = Path(scratch)
            source, output = path / 'collection.bin', path / 'pso_keys.tsv'
            source.write_bytes(pso.encode([(key(), 7, 1)]))
            manifest = path / 'shader_list.tsv'
            manifest.write_text('V\t' + 'a'*64 + '\t{}\nV\t' + 'b'*64 + '\t{}\n')
            result = subprocess.run([sys.executable, str(ROOT / 'games/totk/scripts/merge_pso_keys.py'),
                                     '--shader-list', str(manifest), '--output', str(output),
                                     str(source), str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(pso.read_tsv(output), [(key(), 7, 1)])
            self.assertIn('binary_sha256=' + hashlib.sha256(source.read_bytes()).hexdigest(), result.stdout)
            self.assertEqual(output.read_bytes(), pso.encode_tsv([(key(), 7, 1)]))
            with mock.patch.object(pso, 'TSV_MAX_BYTES', len(pso.TSV_HEADER)), self.assertRaises(ValueError):
                pso.read_tsv(output)

    def test_shipped_text_recreates_final_union(self):
        source = ROOT / 'games/totk/assets/pso_keys.tsv'
        data = pso.encode(pso.read_tsv(source))
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         '762cca1bfa5cc001ce0df44e62f146f53eb441cbb308aeb3dc1a241f9111ef1b')
        self.assertEqual(len(data), 2054512)
        self.assertLessEqual(source.stat().st_size, 2000000)
        self.assertFalse(source.with_suffix('.bin').exists())


if __name__ == '__main__':
    unittest.main()

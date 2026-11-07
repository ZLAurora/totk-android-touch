"""CPU profile parsing/aggregation and exact-symbol identity, with no NDK/device required.

The fake implements the native ReportLib object interface, not report stdout. ELF fixtures
are constructed data, never compiled or executed. Run with unittest discovery from the repo.
"""
import contextlib
import io
import hashlib
import importlib.util
import json
from pathlib import Path
import runpy
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / 'tools/bench'
SPEC = importlib.util.spec_from_file_location('cpuprof_report', BENCH / 'cpuprof_report.py')
assert SPEC is not None and SPEC.loader is not None
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)

BUILD_ID = '0123456789abcdef0123456789abcdef01234567'
OTHER_ID = '1123456789abcdef0123456789abcdef01234567'
TOTK = '/data/app/example/base.apk!/lib/arm64-v8a/libtotk.so'
LIBC = '/apex/com.android.runtime/lib64/bionic/libc.so'
IDENTITY = {'build_id': BUILD_ID, 'has_symtab': True, 'has_debug_info': True}


def symbol(name='work()', dso=TOTK):
    return SimpleNamespace(symbol_name=name, dso_name=dso)


def record(name='totk-walk', tid=101, pid=100, period=1_000_000, leaf=None, callers=(), event='cpu-clock:u'):
    return SimpleNamespace(
        sample=SimpleNamespace(thread_comm=name, tid=tid, pid=pid, period=period),
        leaf=leaf if leaf is not None else symbol(), callers=list(callers), event=event)


class FakeReportLib:
    def __init__(self, records=(), build_ids=None):
        self.records = iter(records)
        self.build_ids = {TOTK: '0x' + BUILD_ID} if build_ids is None else build_ids
        self.build_id_queries = []
        self.closed = False
        self.symfs = None
        self.record_file = None

    def GetNextSample(self):
        self.current = next(self.records, None)
        return self.current.sample if self.current is not None else None

    def GetEventOfCurrentSample(self):
        assert self.current is not None
        return SimpleNamespace(name=self.current.event)

    def GetSymbolOfCurrentSample(self):
        assert self.current is not None
        return self.current.leaf

    def GetCallChainOfCurrentSample(self):
        assert self.current is not None
        return SimpleNamespace(nr=len(self.current.callers),
                               entries=[SimpleNamespace(symbol=s) for s in self.current.callers])

    def GetBuildIdForPath(self, path):
        self.build_id_queries.append(path)
        return self.build_ids.get(path, '')

    def SetSymfs(self, path):
        self.symfs = Path(path)
        self.index = (self.symfs / 'build_id_list').read_text()
        # Copy resolved targets now: the private symfs is intentionally short lived.
        self.index_targets = {}
        for line in self.index.splitlines():
            if not line:
                continue
            key, relative = line.split('=', 1)
            self.index_targets[key] = (self.symfs / relative).resolve()
        self.symfs_files = {p.name for p in self.symfs.iterdir()}
        self.symfs_libtotk = (self.symfs / 'libtotk.so').resolve()

    def SetRecordFile(self, path):
        self.record_file = path

    def Close(self):
        self.closed = True


def elf_note(build_id, endian='<'):
    if build_id is None:
        return b''
    desc = bytes.fromhex(build_id)
    return struct.pack(endian + 'III', 4, len(desc), 3) + b'GNU\0' + desc + b'\0' * (-len(desc) % 4)


def elf_bytes(build_id: str | None = BUILD_ID, symtab=True, debug_info=True, elf_class=2, endian='<', program_only=False,
              notes=None, text=b'\x11\x22\x33\x44', alloc_name='.text', alloc_type=1, alloc_flags=6,
              alloc_addr=0x1000, bss_size=32):
    """Minimal ELF32/64 metadata for the identity reader; no executable instructions."""
    is_64 = elf_class == 2
    header_size = 64 if is_64 else 52
    header_fmt = 'HHIQQQIHHHHHH' if is_64 else 'HHIIIIIHHHHHH'
    section_fmt = 'IIQQQQIIQQ' if is_64 else 'IIIIIIIIII'
    program_fmt = 'IIQQQQQQ' if is_64 else 'IIIIIIII'
    section_size = struct.calcsize(endian + section_fmt)
    program_size = struct.calcsize(endian + program_fmt)
    ident = b'\x7fELF' + bytes([elf_class, 1 if endian == '<' else 2, 1]) + bytes(9)
    notes = elf_note(build_id, endian) if notes is None else notes
    if program_only:
        note_offset = header_size + program_size
        header = (3, 183, 1, 0, header_size, 0, 0, header_size, program_size, 1, section_size, 0, 0)
        program = ((4, 0, note_offset, 0, 0, len(notes), len(notes), 4) if is_64 else
                   (4, note_offset, 0, 0, len(notes), len(notes), 0, 4))
        return ident + struct.pack(endian + header_fmt, *header) + struct.pack(endian + program_fmt, *program) + notes

    names = b'\0.shstrtab\0.note.gnu.build-id\0.symtab\0.debug_info\0.text\0.code\0.bss\0'
    blob = bytearray(header_size)
    sections = [(0,) * 10]

    def section(name, kind, payload, flags=0, addr=0, size=None):
        offset = len(blob)
        blob.extend(payload)
        blob.extend(bytes(-len(blob) % 4))
        sections.append((names.index(name.encode()), kind, flags, addr, offset,
                         len(payload) if size is None else size, 0, 0, 4, 0))

    section('.shstrtab', 3, names)
    section('.note.gnu.build-id', 7, notes)
    if symtab:
        section('.symtab', 2, bytes(24 if is_64 else 16))
    if debug_info:
        section('.debug_info', 1, bytes(4))
    section(alloc_name, alloc_type, text, flags=alloc_flags, addr=alloc_addr)
    section('.bss', 8, b'', flags=3, addr=0x2000, size=bss_size)
    shoff = len(blob)
    for entry in sections:
        blob.extend(struct.pack(endian + section_fmt, *entry))
    header = (3, 183, 1, 0, 0, shoff, 0, header_size, program_size, 0, section_size, len(sections), 1)
    blob[:header_size] = ident + struct.pack(endian + header_fmt, *header)
    return bytes(blob)


def rows_by_symbol(thread, field):
    return {(r['dso'], r['symbol']): r for r in thread[field]}


class Aggregation(unittest.TestCase):
    def aggregate(self, records, top=20, build_ids=None):
        return report.aggregate_report(FakeReportLib(records, build_ids), IDENTITY, top)

    def test_weights_are_periods_not_sample_counts(self):
        result = self.aggregate([
            record(leaf=symbol('frequent()'), period=1_000_000, callers=[symbol('root()')]),
            record(leaf=symbol('expensive()'), period=8_000_000, callers=[symbol('root()')]),
            record(leaf=symbol('frequent()'), period=1_000_000, callers=[symbol('root()')]),
        ])
        self.assertEqual(result['sample_count'], 3)
        self.assertEqual(result['period'], 10_000_000)
        self.assertEqual(result['cpu_ms'], 10.0)
        self.assertEqual(result['period_unit'], 'nanoseconds')
        thread = result['threads'][0]
        self.assertEqual((thread['period'], thread['sample_count'], thread['cpu_ms']), (10_000_000, 3, 10.0))
        self.assertEqual(thread['self'][0]['symbol'], 'expensive()')
        self.assertEqual(thread['self'][0]['sample_count'], 1)
        self.assertEqual(thread['self'][0]['percent'], 80.0)
        self.assertEqual(thread['self'][1]['sample_count'], 2)
        self.assertEqual(thread['self'][1]['percent'], 20.0)
        root = rows_by_symbol(thread, 'inclusive')[TOTK, 'root()']
        self.assertEqual((root['period'], root['sample_count'], root['percent']), (10_000_000, 3, 100.0))
        self.assertEqual(thread['coverage']['with_callchain_percent'], 100.0)

    def test_recursive_frames_are_charged_once_inclusive_including_leaf(self):
        result = self.aggregate([record(leaf=symbol('recursive()'), period=4_000_000,
                                        callers=[symbol('recursive()'), symbol('root()'), symbol('root()')])])
        thread = result['threads'][0]
        inclusive = rows_by_symbol(thread, 'inclusive')
        self.assertEqual(len(inclusive), 2)
        for row in inclusive.values():
            self.assertEqual((row['period'], row['sample_count'], row['percent']), (4_000_000, 1, 100.0))
        self.assertEqual(len(thread['self']), 1)
        self.assertEqual(thread['self'][0]['symbol'], 'recursive()')

    def test_dso_is_part_of_symbol_identity(self):
        result = self.aggregate([record(leaf=symbol('same()'), callers=[symbol('same()', LIBC), symbol('same()')])])
        self.assertEqual(set(rows_by_symbol(result['threads'][0], 'inclusive')),
                         {(TOTK, 'same()'), (LIBC, 'same()')})

    def test_equal_names_do_not_merge_distinct_tids_or_processes(self):
        result = self.aggregate([record(tid=101), record(tid=102), record(tid=101, pid=200)])
        self.assertEqual({(t['pid'], t['tid']) for t in result['threads']}, {(100, 101), (100, 102), (200, 101)})
        self.assertTrue(all(t['self'][0]['percent'] == 100.0 for t in result['threads']))
        walker = result['groups'][0]
        self.assertEqual(len(walker['threads']), 3)
        self.assertEqual(walker['sample_count'], 3)

    def test_percent_denominator_is_per_thread_before_top_truncation(self):
        result = self.aggregate([record(leaf=symbol('large()'), period=3_000_000),
                                 record(leaf=symbol('small()'), period=1_000_000),
                                 record(tid=102, period=12_000_000)], top=1)
        threads = {t['tid']: t for t in result['threads']}
        self.assertEqual(len(threads[101]['self']), 1)
        self.assertEqual(threads[101]['self'][0]['percent'], 75.0)
        self.assertEqual(threads[101]['profile_percent'], 25.0)
        self.assertEqual(threads[102]['self'][0]['percent'], 100.0)

    def test_unknown_symbols_and_missing_chains_remain_visible(self):
        result = self.aggregate([
            record(leaf=symbol(''), period=2_000_000),
            record(leaf=symbol('known()'), period=6_000_000, callers=[symbol('[unknown]', LIBC)]),
        ])
        thread = result['threads'][0]
        unknown = rows_by_symbol(thread, 'self')[TOTK, '[unknown]']
        self.assertEqual((unknown['period'], unknown['percent']), (2_000_000, 25.0))
        coverage = thread['coverage']
        self.assertEqual(coverage['unknown_self_samples'], 1)
        self.assertEqual(coverage['unknown_self_percent'], 25.0)
        self.assertEqual(coverage['unknown_stack_percent'], 100.0)
        self.assertEqual(coverage['with_callchain_samples'], 1)
        self.assertEqual(coverage['with_callchain_period'], 6_000_000)
        self.assertEqual(coverage['with_callchain_percent'], 75.0)
        self.assertTrue(any('complete unwind' in text for text in result['limitations']))
        self.assertTrue(any('critical path' in text for text in result['limitations']))

    def test_unknown_dso_and_address_only_symbols(self):
        result = self.aggregate([record(leaf=symbol('0x1234', ''), callers=[symbol()])])
        self.assertEqual(result['threads'][0]['self'][0]['symbol'], '[unknown]')
        self.assertEqual(result['threads'][0]['self'][0]['dso'], '[unknown]')
        self.assertEqual(result['threads'][0]['coverage']['unknown_self_percent'], 100.0)

    def test_groups_and_totals(self):
        names = ['totk-walk', 'totk-recw0', 'totk-recw12', 'totk-submit', 'g-main', 'g-worker', 'RenderThread']
        expected = ['walker', 'recorders', 'recorders', 'submit', 'game', 'game', 'other']
        result = self.aggregate([record(name=name, tid=100 + i, period=(i + 1) * 1_000_000)
                                 for i, name in enumerate(names)])
        self.assertEqual({t['name']: t['group'] for t in result['threads']}, dict(zip(names, expected)))
        groups = {g['name']: g for g in result['groups']}
        self.assertEqual(set(groups), set(report.GROUPS))
        self.assertEqual(groups['recorders']['period'], 5_000_000)
        self.assertEqual(groups['game']['period'], 11_000_000)
        self.assertEqual(sum(g['period'] for g in groups.values()), result['period'])
        self.assertEqual(sum(g['sample_count'] for g in groups.values()), len(names))
        for name in ['totk-walker', 'xg-main', 'totk-submit-extra', 'totk-rec']:
            self.assertEqual(report.thread_group(name), 'other')

    def test_renamed_thread_keeps_names_and_uses_latest_group(self):
        result = self.aggregate([record(name='initial'), record(name='totk-walk')])
        self.assertEqual(len(result['threads']), 1)
        thread = result['threads'][0]
        self.assertEqual(thread['names'], ['initial', 'totk-walk'])
        self.assertEqual((thread['name'], thread['group']), ('totk-walk', 'walker'))

    def test_zero_periods_do_not_divide_by_zero(self):
        result = self.aggregate([record(period=0)])
        self.assertEqual((result['period'], result['sample_count'], result['cpu_ms']), (0, 1, 0.0))
        self.assertEqual(result['threads'][0]['self'][0]['percent'], 0.0)

    def test_ranking_ties_are_deterministic(self):
        result = self.aggregate([record(leaf=symbol('z()')), record(leaf=symbol('a()'))])
        self.assertEqual([r['symbol'] for r in result['threads'][0]['self']], ['a()', 'z()'])

    def test_identity_queried_once_per_dso_even_with_recursion(self):
        lib = FakeReportLib([record(callers=[symbol(), symbol()]), record()])
        result = report.aggregate_report(lib, IDENTITY)
        self.assertEqual(lib.build_id_queries, [TOTK])
        self.assertEqual(result['symbols']['recorded_dsos'], {TOTK: '0x' + BUILD_ID})

    def test_non_clock_or_multiple_events_cannot_be_converted_to_cpu_ms(self):
        for event in ['cpu-cycles', 'task-clock', 'sched:sched_switch']:
            with self.subTest(event=event), self.assertRaisesRegex(report.ReportError, 'expected cpu-clock'):
                self.aggregate([record(event=event)])
        with self.assertRaisesRegex(report.ReportError, 'single cpu-clock event'):
            self.aggregate([record(), record(event='cpu-clock:k')])

    def test_empty_recording_and_invalid_top_fail(self):
        with self.assertRaisesRegex(report.ReportError, 'no samples'):
            self.aggregate([])
        with self.assertRaisesRegex(report.ReportError, '--top must be positive'):
            self.aggregate([record()], top=0)


class RecordedIdentity(unittest.TestCase):
    def test_wrong_build_id_fails_instead_of_returning_rankings(self):
        lib = FakeReportLib([record()], {TOTK: '0x' + OTHER_ID})
        with self.assertRaisesRegex(report.ReportError, 'build ID mismatch: recorded .*supplied'):
            report.aggregate_report(lib, IDENTITY)

    def test_missing_malformed_and_zero_recorded_ids_fail(self):
        for value in ['', '0x' + '0' * 40, 'not-an-id', '0x123']:
            with self.subTest(value=value), self.assertRaisesRegex(report.ReportError, 'recorded libtotk.so build ID'):
                report.aggregate_report(FakeReportLib([record()], {TOTK: value}), IDENTITY)

    def test_native_missing_id_assertion_becomes_actionable_error(self):
        lib = FakeReportLib([record()])
        with mock.patch.object(lib, 'GetBuildIdForPath', side_effect=AssertionError):
            with self.assertRaisesRegex(report.ReportError, 'cannot verify exact symbols'):
                report.aggregate_report(lib, IDENTITY)

    def test_missing_libtotk_evidence_does_not_claim_verified_identity(self):
        with self.assertRaisesRegex(report.ReportError, 'no sampled libtotk.so'):
            report.aggregate_report(FakeReportLib([record(leaf=symbol('memcpy', LIBC))]), IDENTITY)

    def test_libtotk_in_callchain_also_requires_matching_id(self):
        records = [record(leaf=symbol('memcpy', LIBC), callers=[symbol()])]
        self.assertEqual(report.aggregate_report(FakeReportLib(records), IDENTITY)['sample_count'], 1)
        with self.assertRaisesRegex(report.ReportError, 'mismatch'):
            report.aggregate_report(FakeReportLib(records, {TOTK: '0x' + OTHER_ID}), IDENTITY)

    def test_each_recorded_libtotk_path_is_verified(self):
        second = '/data/old/libtotk.so'
        records = [record(), record(callers=[symbol(dso=second)])]
        lib = FakeReportLib(records, {TOTK: '0x' + BUILD_ID, second: '0x' + OTHER_ID})
        with self.assertRaisesRegex(report.ReportError, 'mismatch'):
            report.aggregate_report(lib, IDENTITY)
        self.assertEqual(lib.build_id_queries, [TOTK, second])

    def test_native_build_id_padding_does_not_strip_significant_zeroes(self):
        self.assertEqual(report.simpleperf_build_id('0xAABB0000'), '0xaabb0000' + '0' * 32)
        short = dict(IDENTITY, build_id='aabb0000')
        lib = FakeReportLib([record()], {TOTK: '0xaabb0000' + '0' * 32})
        self.assertEqual(report.aggregate_report(lib, short)['sample_count'], 1)
        with self.assertRaisesRegex(report.ReportError, '20-byte identity'):
            report.simpleperf_build_id('ab' * 21)


class ElfIdentity(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.library = self.root / 'libtotk.so'

    def save(self, **kwargs):
        self.library.write_bytes(elf_bytes(**kwargs))
        return self.library

    def test_reads_build_id_and_unstripped_metadata_in_each_elf_encoding(self):
        for elf_class in [1, 2]:
            for endian in ['<', '>']:
                with self.subTest(elf_class=elf_class, endian=endian):
                    self.assertEqual(report.read_elf_identity(self.save(elf_class=elf_class, endian=endian)), IDENTITY)

    def test_program_note_survives_without_section_headers(self):
        for elf_class in [1, 2]:
            with self.subTest(elf_class=elf_class):
                result = report.read_elf_identity(self.save(elf_class=elf_class, program_only=True))
                self.assertEqual(result, {'build_id': BUILD_ID, 'has_symtab': False, 'has_debug_info': False})

    def test_stripped_or_missing_identity_is_explicit(self):
        result = report.read_elf_identity(self.save(build_id=None, symtab=False, debug_info=False))
        self.assertEqual(result, {'build_id': None, 'has_symtab': False, 'has_debug_info': False})
        with self.assertRaisesRegex(report.ReportError, 'missing GNU build ID'):
            report.find_symbols(self.root)
        self.save(symtab=False)
        with self.assertRaisesRegex(report.ReportError, 'unstripped libtotk.so'):
            report.find_symbols(self.root)

    def test_debug_info_absence_is_reported_but_symtab_is_sufficient_for_symbols(self):
        self.save(debug_info=False)
        path, result = report.find_symbols(self.root)
        self.assertEqual(path, self.library)
        self.assertTrue(result['has_symtab'])
        self.assertFalse(result['has_debug_info'])

    def test_allocated_hash_is_stable_when_debug_and_symbol_tables_are_stripped(self):
        full = report.elf_alloc_sha256(self.save(build_id=None))
        stripped = report.elf_alloc_sha256(self.save(build_id=None, symtab=False, debug_info=False))
        self.assertEqual(full, stripped)
        self.assertRegex(full, '^[0-9a-f]{64}$')

    def test_allocated_hash_has_a_deterministic_metadata_and_bytes_encoding(self):
        expected = hashlib.sha256(b'totk-elf-alloc-sha256-v1\0')
        expected.update(struct.pack('<BBHHI', 2, 1, 3, 183, 0))
        for name, kind, flags, addr, size, content in (
                (b'.text', 1, 6, 0x1000, 4, b'\x11\x22\x33\x44'),
                (b'.bss', 8, 3, 0x2000, 32, b'')):
            expected.update(struct.pack('<I', len(name)) + name)
            expected.update(struct.pack('<IQQQ', kind, flags, addr, size))
            expected.update(content)
        self.assertEqual(report.elf_alloc_sha256(self.save(build_id=None)), expected.hexdigest())

    def test_allocated_bytes_and_all_identity_metadata_change_the_hash(self):
        baseline = report.elf_alloc_sha256(self.save(build_id=None))
        mutations = [
            {'text': b'\x11\x22\x33\x45'}, {'text': b'\x11\x22'}, {'alloc_name': '.code'},
            {'alloc_type': 7}, {'alloc_flags': 2}, {'alloc_addr': 0x1004}, {'bss_size': 33},
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.assertNotEqual(report.elf_alloc_sha256(self.save(build_id=None, **mutation)), baseline)

    def test_allocated_hash_rejects_a_sectionless_elf(self):
        with self.assertRaisesRegex(report.ReportError, 'SHF_ALLOC sections'):
            report.elf_alloc_sha256(self.save(program_only=True))

    def test_conflicting_notes_and_truncation_are_rejected(self):
        notes = elf_note(BUILD_ID) + elf_note(OTHER_ID)
        with self.assertRaisesRegex(report.ReportError, 'conflicting GNU build IDs'):
            report.read_elf_identity(self.save(notes=notes))
        malformed = struct.pack('<III', 4, 100, 3) + b'GNU\0'
        with self.assertRaisesRegex(report.ReportError, 'truncated ELF note'):
            report.read_elf_identity(self.save(notes=malformed))
        self.library.write_bytes(elf_bytes()[:-1])
        with self.assertRaisesRegex(report.ReportError, 'truncated or invalid ELF range'):
            report.read_elf_identity(self.library)
        self.library.write_bytes(bytes(64))
        with self.assertRaisesRegex(report.ReportError, 'expected an ELF'):
            report.read_elf_identity(self.library)

    def test_directory_requires_one_unambiguous_libtotk(self):
        with self.assertRaisesRegex(report.ReportError, 'found 0'):
            report.find_symbols(self.root)
        self.save()
        nested = self.root / 'arm64'
        nested.mkdir()
        (nested / 'libtotk.so').write_bytes(elf_bytes(build_id=OTHER_ID))
        with self.assertRaisesRegex(report.ReportError, 'found 2'):
            report.find_symbols(self.root)
        with self.assertRaisesRegex(report.ReportError, 'does not exist'):
            report.find_symbols(self.root / 'absent')

    def test_symbol_cache_aliases_to_same_file_are_not_ambiguous(self):
        self.save()
        nested = self.root / 'cache'
        nested.mkdir()
        (nested / 'libtotk.so').symlink_to(self.library)
        self.assertEqual(report.find_symbols(self.root)[0], self.library)


class HostIntegration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.data = self.root / 'perf.data'
        self.data.write_bytes(b'native input is consumed by FakeReportLib')
        self.symbols = self.root / 'symbols'
        self.symbols.mkdir()
        self.library = self.symbols / 'libtotk.so'
        self.library.write_bytes(elf_bytes())

    def capture_manifest(self, **overrides):
        manifest = {
            'data_sha256': hashlib.sha256(self.data.read_bytes()).hexdigest(),
            'symbols_sha256': hashlib.sha256(self.library.read_bytes()).hexdigest(),
            'elf_alloc_sha256': report.elf_alloc_sha256(self.library),
            'apk_sha256': 'a' * 64,
            'commit': '342107300000',
        }
        manifest.update(overrides)
        path = self.root / 'capture.json'
        path.write_text(json.dumps(manifest))
        return path

    def test_missing_gnu_ids_require_manifest_and_verified_basename_symfs(self):
        self.library.write_bytes(elf_bytes(build_id=None))
        manifest = self.capture_manifest()
        lib = FakeReportLib([record()], {TOTK: ''})
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
            result = report.build_report(self.data, self.symbols, capture_manifest=manifest)
        self.assertEqual(result['symbols']['identity_method'], 'capture-manifest')
        self.assertIsNone(result['symbols']['build_id'])
        self.assertEqual(result['symbols']['recorded_dsos'], {TOTK: None})
        self.assertEqual(result['capture_manifest']['commit'], '342107300000')
        self.assertEqual(result['capture_manifest']['file'], str(manifest))
        self.assertEqual(lib.symfs_libtotk, self.library)
        self.assertEqual(lib.index_targets, {})
        self.assertTrue(lib.closed)
        with mock.patch.object(report, 'load_report_lib') as load:
            with self.assertRaisesRegex(report.ReportError, '--capture-manifest'):
                report.build_report(self.data, self.symbols)
        load.assert_not_called()

    def test_manifest_never_bypasses_present_build_id_mismatch(self):
        for supplied_id in [BUILD_ID, None]:
            with self.subTest(supplied_id=supplied_id):
                self.library.write_bytes(elf_bytes(build_id=supplied_id))
                manifest = self.capture_manifest()
                lib = FakeReportLib([record()], {TOTK: '0x' + OTHER_ID})
                with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
                    with self.assertRaisesRegex(report.ReportError, 'build ID mismatch'):
                        report.build_report(self.data, self.symbols, capture_manifest=manifest)

    def test_manifest_accepts_zero_missing_ids_but_not_malformed_recorded_ids(self):
        self.library.write_bytes(elf_bytes(build_id=None))
        manifest = self.capture_manifest()
        lib = FakeReportLib([record()], {TOTK: '0x' + '0' * 40})
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
            result = report.build_report(self.data, self.symbols, capture_manifest=manifest)
        self.assertEqual(result['symbols']['recorded_dsos'], {TOTK: None})
        lib = FakeReportLib([record()], {TOTK: 'not-an-id'})
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
            with self.assertRaisesRegex(report.ReportError, 'unusable recorded'):
                report.build_report(self.data, self.symbols, capture_manifest=manifest)

    def test_manifest_catches_mutated_recording_before_loading_native_library(self):
        manifest = self.capture_manifest()
        self.data.write_bytes(self.data.read_bytes() + b'mutation')
        with mock.patch.object(report, 'load_report_lib') as load:
            with self.assertRaisesRegex(report.ReportError, 'recording SHA-256 mismatch'):
                report.build_report(self.data, self.symbols, capture_manifest=manifest)
        load.assert_not_called()

    def test_manifest_catches_mutated_symbols_even_if_allocated_image_is_unchanged(self):
        self.library.write_bytes(elf_bytes(build_id=None))
        manifest = self.capture_manifest()
        previous_alloc = report.elf_alloc_sha256(self.library)
        self.library.write_bytes(elf_bytes(build_id=None, debug_info=False))
        self.assertEqual(report.elf_alloc_sha256(self.library), previous_alloc)
        with mock.patch.object(report, 'load_report_lib') as load:
            with self.assertRaisesRegex(report.ReportError, 'symbols SHA-256 mismatch'):
                report.build_report(self.data, self.symbols, capture_manifest=manifest)
        load.assert_not_called()

    def test_manifest_requires_allocated_identity_in_addition_to_exact_symbols_hash(self):
        self.library.write_bytes(elf_bytes(build_id=None))
        manifest = self.capture_manifest(elf_alloc_sha256='f' * 64)
        with mock.patch.object(report, 'load_report_lib') as load:
            with self.assertRaisesRegex(report.ReportError, 'allocated ELF SHA-256 mismatch'):
                report.build_report(self.data, self.symbols, capture_manifest=manifest)
        load.assert_not_called()

    def test_manifest_requires_complete_capture_provenance(self):
        manifest = self.capture_manifest()
        complete = json.loads(manifest.read_text())
        for key in ['data_sha256', 'symbols_sha256', 'elf_alloc_sha256', 'apk_sha256', 'commit']:
            for value in [None, 'bad']:
                with self.subTest(key=key, value=value):
                    changed = dict(complete)
                    changed[key] = value
                    manifest.write_text(json.dumps(changed))
                    with self.assertRaisesRegex(report.ReportError, 'capture manifest requires'):
                        report.validate_capture_manifest(manifest, self.data, self.library)
        for value in ['{bad', '[]']:
            manifest.write_text(value)
            with self.assertRaisesRegex(report.ReportError, 'capture manifest'):
                report.validate_capture_manifest(manifest, self.data, self.library)

    def test_cli_passes_capture_manifest_and_reports_legacy_identity(self):
        self.library.write_bytes(elf_bytes(build_id=None))
        manifest = self.capture_manifest()
        lib = FakeReportLib([record()], {TOTK: ''})
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
            with contextlib.redirect_stdout(io.StringIO()):
                status = report.main([str(self.data), '--symbols', str(self.symbols),
                                      '--capture-manifest', str(manifest)])
        self.assertEqual(status, 0)
        result = json.loads(self.data.with_suffix('.json').read_text())
        self.assertEqual(result['symbols']['identity_method'], 'capture-manifest')
        self.assertEqual(result['capture_manifest']['data_sha256'], hashlib.sha256(self.data.read_bytes()).hexdigest())

    def test_import_is_lazy_and_requires_no_ndk_or_native_library(self):
        with mock.patch.dict(sys.modules, {'simpleperf_report_lib': None, 'simpleperf_utils': None}):
            with mock.patch.object(report.importlib, 'import_module', side_effect=AssertionError('unexpected NDK import')):
                runpy.run_path(str(BENCH / 'cpuprof_report.py'), run_name='cpuprof_import_test')

    def test_missing_ndk_directory_has_cli_remediation(self):
        with self.assertRaisesRegex(report.ReportError, '--ndk-simpleperf DIR'):
            report.load_report_lib(self.root / 'missing-ndk')

    def test_loading_a_previously_imported_different_wrapper_is_rejected(self):
        ndk = self.root / 'simpleperf'
        ndk.mkdir()
        (ndk / 'simpleperf_report_lib.py').write_text('')
        original_path = list(sys.path)
        with mock.patch.object(report.importlib, 'import_module', return_value=SimpleNamespace(__file__='/different/ndk.py')):
            with self.assertRaisesRegex(report.ReportError, 'different NDK ReportLib'):
                report.load_report_lib(ndk)
        self.assertEqual(sys.path, original_path)

    def test_native_wrapper_gets_private_symfs_index_and_closes(self):
        (self.symbols / 'other.so').write_bytes(b'other symbols')
        (self.symbols / 'build_id_list').write_text('0x' + OTHER_ID + '=other.so\n')
        before = (self.symbols / 'build_id_list').read_text()
        lib = FakeReportLib([record()])
        ndk = self.root / 'chosen-ndk'
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib) as load:
            result = report.build_report(self.data, self.symbols, ndk, top=7)
        load.assert_called_once_with(ndk)
        self.assertEqual(lib.record_file, str(self.data))
        self.assertEqual(lib.index_targets['0x' + BUILD_ID], self.library)
        self.assertEqual(lib.index_targets['0x' + OTHER_ID], self.symbols / 'other.so')
        self.assertIn('other.so', lib.symfs_files)
        self.assertTrue(lib.closed)
        assert lib.symfs is not None
        self.assertFalse(lib.symfs.exists())
        self.assertEqual((self.symbols / 'build_id_list').read_text(), before)
        self.assertEqual(result['symbols']['file'], str(self.library))
        self.assertEqual(result['top'], 7)
        self.assertEqual(result['data'], str(self.data))

    def test_identity_failure_closes_native_library_and_removes_symfs(self):
        lib = FakeReportLib([record()], {TOTK: '0x' + OTHER_ID})
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
            with self.assertRaisesRegex(report.ReportError, 'mismatch'):
                report.build_report(self.data, self.symbols)
        self.assertTrue(lib.closed)
        assert lib.symfs is not None
        self.assertFalse(lib.symfs.exists())

    def test_bad_symbols_fail_before_native_loading(self):
        self.library.write_bytes(elf_bytes(symtab=False))
        with mock.patch.object(report, 'load_report_lib') as load:
            with self.assertRaisesRegex(report.ReportError, 'unstripped'):
                report.build_report(self.data, self.symbols)
        load.assert_not_called()

    def test_cli_writes_json_and_readable_stdout(self):
        output = self.root / 'report.json'
        lib = FakeReportLib([record(period=3_000_000)])
        stdout = io.StringIO()
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib), contextlib.redirect_stdout(stdout):
            status = report.main([str(self.data), '--symbols', str(self.symbols), '--out', str(output), '--top', '2'])
        self.assertEqual(status, 0)
        result = json.loads(output.read_text())
        self.assertEqual(result['schema'], report.SCHEMA)
        self.assertEqual((result['cpu_ms'], result['sample_count']), (3.0, 1))
        self.assertIn('walker 100/101 totk-walk', stdout.getvalue())
        self.assertIn('3.000 CPU ms', stdout.getvalue())
        self.assertIn('Inclusive is not wall-time critical path', stdout.getvalue())

    def test_cli_default_output_is_adjacent_json(self):
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: FakeReportLib([record()])):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(report.main([str(self.data), '--symbols', str(self.symbols)]), 0)
        self.assertTrue(self.data.with_suffix('.json').is_file())

    def test_cli_identity_error_does_not_emit_a_report(self):
        output = self.root / 'report.json'
        stderr, stdout = io.StringIO(), io.StringIO()
        lib = FakeReportLib([record()], {TOTK: ''})
        with mock.patch.object(report, 'load_report_lib', return_value=lambda: lib):
            with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(stdout):
                status = report.main([str(self.data), '--symbols', str(self.symbols), '--out', str(output)])
        self.assertEqual(status, 1)
        self.assertIn('cannot verify exact symbols', stderr.getvalue())
        self.assertEqual(stdout.getvalue(), '')
        self.assertFalse(output.exists())

    def test_cli_refuses_invalid_top_and_input_overwrite(self):
        for extra in [['--top', '0'], ['--out', str(self.data)]]:
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    report.main([str(self.data), '--symbols', str(self.symbols), *extra])
            self.assertEqual(error.exception.code, 2)
        self.assertEqual(self.data.read_bytes(), b'native input is consumed by FakeReportLib')


if __name__ == '__main__':
    unittest.main()

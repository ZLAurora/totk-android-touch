"""Production dirty/alias ownership and real MapVirtual replacement integration.

Run normally, or with WB_ALIAS_TEST_CFLAGS='-fsanitize=thread -fno-omit-frame-pointer'
for fully instrumented fixtures (including nvn_vk.c/nvn_dirty.c), no suppressions.
Artifacts stay below the checkout's enclosing .scratch, never in /tmp.
The backend links totk_vpool.c directly: anonymous replacements, overlapping
last writers, mremap fallback, fatal partial failure and pinned remap/unmap
threads all use production operations, not synthetic alias metadata repairs.
Counter snapshots exercise the real Site2 selector for STORE and PUBLICATION,
including byte partitions across real owner/anonymous MapVirtual boundaries.
The counter-only mode needs no Vulkan and emits one explicit report plus exit.
"""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
FIXTURE = TOTK / 'tests/wb_alias'
COUNTER_SITES = ('landing', 'store', 'publication')
COUNTER_REASONS = ('alias', 'registry', 'ownership', 'mixed',
                   'non-dmabuf', 'retired', 'other')


class WritebackAlias(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != 'linux':
            raise unittest.SkipTest('real Linux VMAs and SIGSEGV are required')
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('a C compiler is required')
        scratch = next((p / '.scratch' for p in (ROOT, *ROOT.parents)
                        if (p / '.scratch').is_dir()), ROOT / '.scratch')
        scratch.mkdir(exist_ok=True)
        cls.work = tempfile.TemporaryDirectory(prefix='wb-alias-', dir=scratch)
        cls.addClassCleanup(cls.work.cleanup)
        cls.directory = Path(cls.work.name)
        build_environment = os.environ.copy()
        build_environment['TMPDIR'] = str(cls.directory)
        cls.executables = {}
        flags = [
            '-std=gnu11', '-O1', '-g', '-D_GNU_SOURCE', '-DTOTK_VK_TESTS',
            '-DGPU_VK', '-ffunction-sections', '-fdata-sections',
            '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-parameter',
            '-Wno-missing-field-initializers', '-Wno-format',
            '-Wno-misleading-indentation',
            '-I' + str(TOTK / 'runtime'),
            '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn'),
            '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn/include'),
            '-I' + str(TOTK / 'third_party/d2r_gfx/host'),
            '-I' + str(TOTK / 'android/compat/include'),
            '-include', str(TOTK / 'android/compat/include/darwin_compat.h'),
            *shlex.split(os.environ.get('WB_ALIAS_TEST_CFLAGS', '')),
        ]
        sources = {
            'dirty': [FIXTURE / 'probe.c'],
            'backend': [FIXTURE / 'backend.c', TOTK / 'android/gfx/nvn_vk.c',
                        TOTK / 'third_party/d2r_gfx/gfx/nvn/totk_vpool.c'],
            'swizzle': [FIXTURE / 'swizzle.c',
                        TOTK / 'third_party/d2r_gfx/gfx/nvn/nvn_tex.c',
                        TOTK / 'third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c'],
        }
        for name, paths in sources.items():
            executable = cls.directory / name
            fixture_flags = ['-DTOTK_VK_ALIAS_FIXTURE=1'] if name == 'backend' else []
            command = [*compiler, *flags, *fixture_flags, *(str(p) for p in paths),
                       '-Wl,--gc-sections', '-pthread', '-ldl', '-lm',
                       '-o', str(executable)]
            result = subprocess.run(command, env=build_environment, capture_output=True,
                                    text=True, timeout=180)
            if result.returncode:
                raise AssertionError('fixture compile failed:\n' + shlex.join(command)
                                     + '\n' + result.stdout + result.stderr)
            cls.executables[name] = executable

    def run_fixture(self, name, switch, *, lag='1', gem=False, args=()):
        environment = os.environ.copy()
        environment['TMPDIR'] = str(self.directory)
        for key in ('TOTK_WB_ALIAS', 'TOTK_FRAME_LAG', 'TOTK_DIRTY_UNIT',
                    'TOTK_DIRTY_UNIT_KB', 'DM_NVN_TILELOG'):
            environment.pop(key, None)
        if switch is not None:
            environment['TOTK_WB_ALIAS'] = switch
        environment['TOTK_FRAME_LAG'] = lag
        environment['TOTK_DIRTY_UNIT'] = '16'
        command = [str(self.executables[name])]
        if gem:
            command.append('i915')
        command.extend(args)
        result = subprocess.run(command, env=environment, capture_output=True,
                                text=True, timeout=45)
        if gem and result.returncode == 77:
            self.assertIn('UNAVAILABLE:', result.stdout)
            self.skipTest(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('ThreadSanitizer:', result.stderr, result.stdout + result.stderr)
        self.assertNotIn('FAIL', result.stdout + result.stderr)
        print(result.stdout, end='')
        return result

    def test_real_dirty_switch_off_on(self):
        for lag in ('0', '1'):
            digests = []
            for switch in ('0', '1'):
                with self.subTest(lag=lag, alias=switch):
                    result = self.run_fixture('dirty', switch, lag=lag)
                    self.assertIn('WB_ALIAS PASS', result.stdout)
                    self.assertIn('UNWIND PASS', result.stdout)
                    self.assertIn('CONCURRENT PASS SIGSEGV PG_BUSY token-end', result.stdout)
                    self.assertIn('SA_ONSTACK', result.stdout)
                    if lag == '1':
                        self.assertIn('TICKETS PASS', result.stdout)
                    if switch == '1':
                        self.assertIn('COMPLETION PASS', result.stdout)
                    digest = re.search(r'digest=([0-9a-f]+)', result.stdout)
                    if digest is None:
                        self.fail('missing exact landing digest: ' + result.stdout)
                    digests.append(digest.group(1))
            self.assertEqual(digests[0], digests[1], 'off/on exact landing contents differ')

    def test_backend_switch_offsets_lifetime_and_site2(self):
        for switch in (None, '', '0', '01', 'true', '1 ', '1'):
            with self.subTest(switch=switch):
                result = self.run_fixture('backend', switch)
                self.assertIn('WB_ALIAS_BACKEND PASS', result.stdout)
                self.assertIn('BACKEND_OFFSETS PASS KOWN KVIEW KVIRT', result.stdout)
                self.assertIn('SITE2 PASS uncontended same-thread-trylock second-thread-trylock', result.stdout)
                self.assertIn('LIFETIME PASS pinned-token removal-excluded', result.stdout)
                self.assertEqual('write-back alias: enabled' in result.stderr, switch == '1')

    def test_production_mapvirtual_replacement_contents(self):
        for lag in ('0', '1'):
            digests = []
            for switch in ('0', '1'):
                with self.subTest(lag=lag, alias=switch):
                    result = self.run_fixture('backend', switch, lag=lag)
                    for marker in (
                        'MAPVIRTUAL_ANONYMOUS PASS real-batch two-batch fresh-zero current-guest',
                        'donor-untouched other-alias resolver-hole',
                        'MAPVIRTUAL_FALLBACK PASS real-mremap unsupported-current-source',
                        'MAPVIRTUAL_REBACK PASS owner-destroy stale-identity same-VA-new-fd',
                        'MAPVIRTUAL_PARTIAL PASS real-private-source-EINVAL fatal-batch conservative-authority',
                        'PINNED_MAPVIRTUAL PASS real-remap real-unmap admission-blocked',
                    ):
                        self.assertIn(marker, result.stdout)
                    digest = re.search(r'MAPVIRTUAL_CONTENTS digest=([0-9a-f]+)', result.stdout)
                    if digest is None:
                        self.fail('missing complete production mapping content digest: ' + result.stdout)
                    digests.append(digest.group(1))
            if len(digests) == 2:  # a failed subtest already reports its fixture failure
                self.assertEqual(digests[0], digests[1], 'off/on real MapVirtual guest/donor/other-alias contents differ')

    def test_site2_counter_deltas_snapshots_and_cumulative_reports(self):
        for switch in (None, '', '0', '01', 'true', '1 ', '1'):
            with self.subTest(switch=switch):
                result = self.run_fixture('backend', switch, args=('counters',))
                marker = re.search(r'WB_ALIAS_COUNTERS PASS alias=(\d) unit=(\d+)', result.stdout)
                if marker is None:
                    self.fail('missing counter selector summary: ' + result.stdout)
                enabled = switch == '1'
                self.assertEqual(int(marker.group(1)), enabled)
                unit = int(marker.group(2))
                rows = re.findall(
                    r'^WB_ALIAS_COUNTS_SNAPSHOT (\S+) (\S+) (\d+) (\d+)$',
                    result.stdout, re.MULTILINE)
                self.assertEqual(len(rows), len(COUNTER_SITES) * len(COUNTER_REASONS))
                snapshot = {(site, reason): (int(ops), int(size))
                            for site, reason, ops, size in rows}
                self.assertEqual(len(snapshot), len(rows), 'duplicate snapshot bucket')
                selected = {
                    'alias': (5, 4 * unit + 209),
                    'registry': (2, 101),
                    'ownership': (0, 0),
                    'mixed': (2, 2 * unit + 97),
                    'non-dmabuf': (1, 97),
                    'retired': (2, 194),
                    'other': (1, 97),
                }
                expected = {
                    (site, reason): selected[reason] if enabled and site != 'landing' else (0, 0)
                    for site in COUNTER_SITES for reason in COUNTER_REASONS
                }
                self.assertEqual(snapshot, expected, result.stdout + result.stderr)
                for site in ('store', 'publication'):
                    self.assertEqual(sum(snapshot[site, reason][1] for reason in COUNTER_REASONS),
                                     6 * unit + 795 if enabled else 0,
                                     'logical store bytes must be partitioned, never duplicated')
                prefix = 'memory write-back alias paths: '
                lines = [line for line in result.stderr.splitlines() if prefix in line]
                self.assertEqual(len(lines), 2 if enabled else 0, result.stderr)
                for line in lines:
                    self.assertLessEqual(len(line.encode()), 3 * 1024)
                payload = ' '.join(
                    site + '=' + ','.join(
                        f'{reason}:{snapshot[site, reason][0]}/{snapshot[site, reason][1]}'
                        for reason in COUNTER_REASONS)
                    for site in COUNTER_SITES)
                self.assertEqual(
                    [line.split(prefix, 1)[1] for line in lines],
                    [f'phase={phase} cumulative=1 {payload}' for phase in ('periodic', 'exit')]
                    if enabled else [],
                    'explicit report and exit must each emit exactly one identical cumulative line')

    def test_real_pause_reporter_concurrent_initialization(self):
        for switch in (None, '0', '1'):
            with self.subTest(switch=switch):
                result = self.run_fixture('backend', switch, args=('pause-init',))
                enabled = switch == '1'
                self.assertIn(f'WB_ALIAS_PAUSE_INIT PASS alias={int(enabled)} '
                              'real-reporter concurrent-real-init', result.stdout)
                lines = [line for line in result.stderr.splitlines()
                         if 'memory write-back alias paths:' in line]
                self.assertEqual(bool(lines), enabled)
                if enabled:
                    self.assertTrue(any('phase=pause cumulative=1' in line for line in lines))
                    self.assertIn('phase=exit cumulative=1', lines[-1])
                    for line in lines:
                        self.assertNotRegex(line, r':(?!0/0(?:,| |$))\d+/\d+')

    def test_i915_nonmerging_guest_rows(self):
        digests = []
        for switch in ('0', '1'):
            result = self.run_fixture('dirty', switch, gem=True)
            rows = re.search(r'new_guest_rows=(\d+) protection_calls=(\d+)', result.stdout)
            if rows is None:
                self.fail('missing actual kernel row evidence: ' + result.stdout)
            if switch == '1':
                self.assertEqual(tuple(map(int, rows.groups())), (0, 0))
            else:
                self.assertGreater(int(rows.group(1)), 0)
                self.assertGreater(int(rows.group(2)), 0)
            digest = re.search(r'digest=([0-9a-f]+)', result.stdout)
            if digest is None:
                self.fail('missing i915 landing digest: ' + result.stdout)
            digests.append(digest.group(1))
            backend = self.run_fixture('backend', switch, gem=True)
            self.assertIn('WB_ALIAS_BACKEND PASS', backend.stdout)
        self.assertEqual(digests[0], digests[1], 'i915 off/on full-byte contents differ')

    def test_actual_swizzler_independent_byte_spans(self):
        result = self.run_fixture('swizzle', '1')
        self.assertIn('WB_ALIAS_SWIZZLE PASS arbitrary-byte-spans independent-mappings', result.stdout)


if __name__ == '__main__':
    unittest.main()

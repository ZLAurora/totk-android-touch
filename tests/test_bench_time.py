"""Launch-only clock guards, strict parsing/logging and native ARM64 seam.

Dataless guards always run. Real-word emulation needs TOTK_MAIN_NSO and Unicorn.
CC accepts a shell-quoted command (including a ThreadSanitizer compiler).
"""
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
SCRIPTS = GAME / 'scripts'
MAIN = Path(os.environ.get('TOTK_MAIN_NSO', ROOT.parent / 'build/TOTK_build/totk/extracted/v143/exefs/main'))


class BenchTime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not shutil.which(compiler[0]):
            raise unittest.SkipTest('C compiler required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='bench-time-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.binary = cls.directory / 'clock'
        subprocess.run([*compiler, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-format',
                        '-DCODE_GUARD_STANDIN=1', '-I' + str(GAME / 'runtime'),
                        str(SCRIPTS / 'v143_bench_time_fixture.c'),
                        str(GAME / 'android/gfx/host_test_support.c'), '-lm', '-o', str(cls.binary)], check=True)

    def run_fixture(self, *args):
        run = subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run

    def test_parse_all_minutes_and_verify_before_write(self):
        self.assertIn('1440 valid times, 23 rejected values, 45 word corruptions', self.run_fixture().stdout)

    def test_one_launch_log_and_rejected_values_off(self):
        for value in ('absent', 'off', '', '0', 'OFF', '24:00', '23:60', '02:0', '2:00', '02:00 ', '02:00\n'):
            with self.subTest(value=value):
                log = self.run_fixture('--value', value).stderr.splitlines()
                self.assertEqual(len(log), 1, log)
                self.assertIn('[totk-bench-time]', log[0])
                self.assertIn('off', log[0])
                if value not in ('absent', 'off'):
                    self.assertIn('rejected', log[0])

    def test_effective_time_logged_once(self):
        for value, seconds in (('00:00', 0), ('00:01', 60), ('02:00', 7200),
                               ('12:34', 45240), ('23:59', 86340)):
            with self.subTest(value=value):
                log = self.run_fixture('--value', value).stderr.splitlines()
                self.assertEqual(len(log), 1, log)
                self.assertIn(f'TOTK_BENCH_TIME={value} effective_seconds={seconds}', log[0])

    @unittest.skipUnless(importlib.util.find_spec('unicorn') and importlib.util.find_spec('lz4') and MAIN.is_file(),
                         'Unicorn/lz4 and authenticated 1.4.3 main NSO ($TOTK_MAIN_NSO) required')
    def test_native_exact_state_oracle(self):
        sys.path.insert(0, str(SCRIPTS))
        try:
            import verify_v143_bench_time as verify
            result = verify.verify(self.directory / 'real', MAIN)
            self.assertEqual(result['veneer_full_state'], 16)
            self.assertEqual(result['native_exact_state_and_memory'], 64)
        finally:
            sys.path.remove(str(SCRIPTS))

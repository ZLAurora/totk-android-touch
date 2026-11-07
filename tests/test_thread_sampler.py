"""Thread sampler (games/totk/runtime/thread_sampler.h, TOTK_THREAD_SAMPLER): the host probe
(games/totk/tests/thread_sampler/probe.c) drives the production header on a synthetic /proc task tree (exact top three,
run-queue wait, new / ended / stat-only threads, the 300-present report, nothing started or written while off, idle and
resumed live), on this process's own threads (two spinners fill a long frame), and with a frame thread toggling the
switch while the sampler answers. Both also run under ThreadSanitizer when the compiler has it.

CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
FLAGS = ['-D_GNU_SOURCE', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', f'-I{TOTK / "runtime"}']


def compiler():
    cc = shlex.split(os.environ.get('CC', 'cc'))
    return cc if shutil.which(cc[0]) else None


@unittest.skipUnless(compiler() and sys.platform.startswith('linux'), 'needs Linux and a C compiler')
class ThreadSampler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.probe = cls.dir / 'thread-sampler'
        subprocess.run([*compiler(), *FLAGS, str(TOTK / 'tests/thread_sampler/probe.c'), '-o', str(cls.probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_mode(self, binary, mode, env=None):
        work = Path(tempfile.mkdtemp(dir=self.dir))
        r = subprocess.run([str(binary), mode, str(work)], capture_output=True, text=True, timeout=300, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f'ok: thread sampler {mode}', r.stdout)
        return r.stdout

    def tsan_probe(self):
        binary = self.dir / 'thread-sampler-tsan'
        if not binary.exists() and subprocess.run([*compiler(), *FLAGS, '-fsanitize=thread', str(TOTK / 'tests/thread_sampler/probe.c'),
                                                   '-o', str(binary)], capture_output=True).returncode:
            self.skipTest('the compiler has no -fsanitize=thread')
        return binary

    def test_synthetic(self):
        out = self.run_mode(self.probe, 'synthetic')
        self.assertIn(': worker-a(101) run 300.0 wait 5.0, worker-c(103) run 200.0 wait 1.0, new-thread(107) run 150.0 wait 3.0 ms;', out)

    def test_real_threads(self):
        if not Path('/proc/self/task').is_dir() or not list(Path('/proc/self/task').glob('*/schedstat')):
            self.skipTest('no /proc/self/task/*/schedstat on this kernel')
        self.assertIn('real: [totk-threads] long frame 3 ', self.run_mode(self.probe, 'real'))

    def test_race(self):
        self.run_mode(self.probe, 'race')

    def test_thread_sanitizer(self):
        binary = self.tsan_probe()
        env = os.environ | {'TSAN_OPTIONS': 'halt_on_error=1 exitcode=66'}
        for mode in ('synthetic', 'race'):
            self.run_mode(binary, mode, env=env)


if __name__ == '__main__':
    unittest.main()

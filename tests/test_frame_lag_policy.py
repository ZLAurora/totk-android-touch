"""Observe production startup policy in fresh native processes, without a GPU.

CC accepts a shell-quoted compiler/wrapper command; CPPFLAGS, CFLAGS and LDFLAGS
accept explicit flags (including sanitizer flags). TMPDIR selects scratch output.
Real backend chunk thresholds, slots, pixels and startup diagnostics are exercised
separately by vkdrawtest --chunk-policy, not by a second implementation here.
The GPU-time probe compiles the actual nvn_vk.c startup selection block: it needs
neither Vulkan initialization nor a duplicate implementation of the resolver.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Test macros still select recorder-thread policy, but cannot enable frame lag.
PLATFORM_MACROS = ('__ANDROID__', 'GAME_BOTW', 'TOTK_VK_TESTS', 'TOTK_TEST_ANDROID_FRAME_POLICY')
VARIANTS = tuple(
    ('platform-' + str(mask),
     tuple(macro for bit, macro in enumerate(PLATFORM_MACROS) if mask & (1 << bit)))
    for mask in range(1 << len(PLATFORM_MACROS))
)
FPS60_VALUES = (None, '0', '1', '', 'false', '01')
# Explicit consumer-visible goldens; FPS60/platform never changes these results.
LAG_CASES = ((None, 0), ('1', 1), ('0', 0), ('', 0), ('false', 0), ('01', 0))
GPU_CASES = (None, '', 'hardware', 'latency', 'unknown')


class FrameLagPolicy(unittest.TestCase):
    @classmethod
    def command(cls, command, env=None):
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(
                f'{shlex.join(command)} exited {result.returncode}\n{result.stdout}{result.stderr}')
        return result

    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            if 'CC' in os.environ:
                raise RuntimeError('CC must name an available compiler or compiler wrapper')
            raise unittest.SkipTest('A native C compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='frame-lag-policy-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        # Keep the platform #if and startup log in the compiled production block.
        # These surrounding calls deliberately fail loudly if initialization moves.
        source = (ROOT / 'games/totk/android/gfx/nvn_vk.c').read_text()
        start = source.index('    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);\n')
        start = source.index('\n', start) + 1
        end = source.index('    ts_clock_init();', start)
        (directory / 'gpu_time_policy.inc').write_text(source[start:end])
        cls.probes = {}
        compile_flags = [shlex.split(os.environ.get(name, ''))
                         for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')]
        cppflags, cflags, ldflags = compile_flags
        for name, defines in VARIANTS:
            executable = directory / name
            # Wrappers may supply the host test define; make every variant exact.
            cls.command([
                *compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                *cppflags, *cflags,
                *('-U' + macro for macro in PLATFORM_MACROS),
                *('-D' + macro for macro in defines), '-I', str(directory),
                str(ROOT / 'tests/test_frame_lag_policy.c'), *ldflags, '-o', str(executable),
            ])
            cls.probes[name] = (executable, defines)

    @staticmethod
    def environment(fps60, lag=None, gpu_time=None, median=None):
        env = os.environ.copy()
        for name, value in (('TOTK_FPS60', fps60), ('TOTK_FRAME_LAG', lag),
                            ('TOTK_GPU_TIME', gpu_time), ('TOTK_GPU_TIME_FPS_MEDIAN', median)):
            # None means actual absence, not an empty/false stand-in.
            env.pop(name, None)
            if value is not None:
                env[name] = value
        return env

    def test_lag_defaults_and_exact_one_overrides_on_every_platform(self):
        for name, (executable, _) in self.probes.items():
            for fps60 in FPS60_VALUES:
                for lag, expected in LAG_CASES:
                    with self.subTest(platform=name, fps60=fps60, frame_lag=lag):
                        result = self.command([str(executable)], self.environment(fps60, lag))
                        self.assertEqual(result.stdout, f'FRAME_LAG={expected}\n', result.stderr)

    def test_gpu_time_defaults_overrides_and_startup_reasons(self):
        for name, (executable, defines) in self.probes.items():
            for fps60 in FPS60_VALUES:
                android30 = ('__ANDROID__' in defines and 'GAME_BOTW' not in defines
                             and fps60 in (None, '0'))
                for gpu_time in GPU_CASES:
                    with self.subTest(platform=name, fps60=fps60, gpu_time=gpu_time):
                        if gpu_time in (None, ''):
                            mode = 'hardware' if android30 else 'latency'
                            reason = ('default Android 30-fps; TOTK_GPU_TIME=latency opts out'
                                      if android30 else
                                      'default latency; TOTK_GPU_TIME=hardware opts in')
                        elif gpu_time in ('hardware', 'latency'):
                            mode, reason = gpu_time, 'explicit'
                        else:
                            mode, reason = 'latency', 'unknown value; retaining latency'
                        result = self.command([str(executable), '--gpu-time'],
                                              self.environment(fps60, gpu_time=gpu_time))
                        self.assertEqual(result.stdout,
                                         f'[fps-policy] GPU report time: {mode} ({reason})\n'
                                         '[fps-policy] TOTK_GPU_TIME_FPS_MEDIAN=0: FPS END consumer only, median of three unique valid reports; hardware required\n'
                                         f'GPU_TIME={mode}\n', result.stderr)

    def test_gpu_fps_median_requires_exact_one_and_hardware(self):
        for name, (executable, _) in self.probes.items():
            for gpu_time in ('hardware', 'latency'):
                for median in (None, '', '0', '1', '01', 'true', '1 '):
                    with self.subTest(platform=name, gpu_time=gpu_time, median=median):
                        enabled = int(gpu_time == 'hardware' and median == '1')
                        result = self.command([str(executable), '--gpu-time'],
                            self.environment('1', gpu_time=gpu_time, median=median))
                        self.assertIn(f'TOTK_GPU_TIME_FPS_MEDIAN={enabled}:', result.stdout)


if __name__ == '__main__':
    unittest.main()

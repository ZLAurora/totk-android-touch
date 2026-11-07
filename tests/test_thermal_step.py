"""Dataless thermal hysteresis and production guest-floor veneer regression."""
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ThermalStep(unittest.TestCase):
    def compile_run(self, source, patch=False):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('A native C compiler is required')
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        if patch:
            compat = ROOT / 'games/totk/android/compat/include'
            flags += ['-DTOTK_GAME_V143=1', '-I' + str(compat), '-include',
                      str(compat / 'darwin_compat.h'),
                      '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/host'),
                      '-I' + str(ROOT / 'games/totk/runtime')]
        with tempfile.TemporaryDirectory(prefix='thermal-step-') as directory:
            executable = Path(directory) / 'test'
            result = subprocess.run([*compiler, '-std=gnu11', '-D_GNU_SOURCE', '-O2',
                                     '-Wall', '-Wextra', *flags, str(ROOT / source),
                                     '-lm', '-pthread', '-o', str(executable)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_policy(self):
        self.compile_run('tests/test_thermal_step.c')

    def test_startup_thread_gate(self):
        self.compile_run('tests/test_thermal_startup.c')

    def test_patch_installation(self):
        payload = json.loads(self.compile_run('games/totk/runtime/test_thermal_patch.c', patch=True))
        self.assertEqual(len(bytes.fromhex(payload['code'])), 108)

    def test_patch_machine_semantics(self):
        try:
            from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
            from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_NZCV
        except ImportError:
            self.skipTest('Unicorn is required for ARM64 instruction semantics')
        payload = json.loads(self.compile_run('games/totk/runtime/test_thermal_patch.c', patch=True))
        code = bytes.fromhex(payload['code'])
        for entry, (site, original) in enumerate(payload['entries']):
            dest = (10, 21, 12)[entry]
            for floor in (1, 2):
                for flags in range(16):
                    for value in (0, 1, 2, 3, 0x7fffffff):
                        registers = [0x12340000 + r for r in range(31)]
                        registers[9] = registers[10] = registers[13] = value
                        def execute(thermal):
                            cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                            for page in {site & ~4095, payload['stub'] & ~4095, payload['floor'] & ~4095, 0x10000000}:
                                cpu.mem_map(page, 4096)
                            cpu.mem_write(site, struct.pack('<I', original))
                            cpu.mem_write(payload['stub'], code)
                            cpu.mem_write(payload['floor'], struct.pack('<I', floor))
                            for reg, number in enumerate(registers[:29]):
                                cpu.reg_write(UC_ARM64_REG_X0 + reg, number)
                            cpu.reg_write(UC_ARM64_REG_SP, 0x10000800)
                            cpu.reg_write(UC_ARM64_REG_NZCV, flags << 28)
                            cpu.emu_start(payload['stub'] + entry * 36 if thermal else site,
                                          site + 4, count=20)
                            return ([cpu.reg_read(UC_ARM64_REG_X0 + reg) for reg in range(29)],
                                    cpu.reg_read(UC_ARM64_REG_SP), cpu.reg_read(UC_ARM64_REG_NZCV))
                        expected, sp, nzcv = execute(False)
                        expected[dest] = max(expected[dest], floor)
                        self.assertEqual(execute(True), (expected, sp, nzcv),
                                         (entry, floor, flags, value))


if __name__ == '__main__':
    unittest.main()

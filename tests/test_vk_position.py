"""Vertex Position precision, without Vulkan/devices or game data for fixtures.

Run with: python3 -m unittest discover -s tests -p test_vk_position.py -v
Requires a C compiler; spirv-val additionally validates the synthetic modules.
Set TOTK_TEST_POSITION_SEED to a loose seed directory or seed.vkpack to validate
real vertex samples in every switch arm (read-only input, temporary outputs).
TOTK_TEST_POSITION_SEED_LIMIT defaults to 8; CC accepts a shell-quoted command.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
CC = shlex.split(os.environ.get('CC', 'cc'))


@unittest.skipUnless(CC and shutil.which(CC[0]), 'needs a host C compiler')
class PositionPrecisionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.probe = cls.work / 'vkpositiontest'
        subprocess.run([*CC, '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        str(GFX / 'test_vk_position.c'), '-o', str(cls.probe)], check=True)

    def test_policy_decorations_and_slice(self):
        result = subprocess.run([str(self.probe)], check=True, capture_output=True, text=True)
        self.assertIn('failure atomicity passed', result.stdout)

    @unittest.skipUnless(shutil.which('spirv-val'), 'needs spirv-val (spirv-tools)')
    def test_fixture_validation(self):
        output = self.work / 'fixtures'
        output.mkdir()
        subprocess.run([str(self.probe), str(output)], check=True)
        modules = sorted(output.glob('*.spv'))
        self.assertEqual(len(modules), 8)
        for module in modules:
            with self.subTest(module=module.name):
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(module)], check=True)

    @unittest.skipUnless(os.environ.get('TOTK_TEST_POSITION_SEED'), 'set TOTK_TEST_POSITION_SEED for real seed validation')
    @unittest.skipUnless(shutil.which('spirv-val'), 'needs spirv-val (spirv-tools)')
    def test_seed_validation(self):
        seed = Path(os.environ['TOTK_TEST_POSITION_SEED']).resolve(strict=True)
        limit = int(os.environ.get('TOTK_TEST_POSITION_SEED_LIMIT', '8'))
        self.assertGreater(limit, 0)
        output = self.work / 'seed'
        output.mkdir()
        if seed.is_dir() and (seed / 'seed.vkpack').is_file():
            seed = seed / 'seed.vkpack'
        if seed.is_file():
            result = subprocess.run([str(self.probe), '--sample-pack', str(seed), str(output), str(limit)],
                                    check=True, capture_output=True, text=True)
            samples = [output / name for name in result.stdout.splitlines()]
        else:
            samples = []
            for reflection in sorted(seed.glob('*.vk')):
                if 'stage 0' not in reflection.read_text().splitlines():
                    continue
                module = reflection.with_suffix('.spv')
                self.assertTrue(module.is_file(), str(module))
                samples.append(module)
                if len(samples) == limit:
                    break
        self.assertTrue(samples, 'seed contains no vertex modules')
        print(f'position validation seed: {seed}, {len(samples)} vertex samples', flush=True)
        for source in samples:
            # Validate the baseline separately; no legalization/optimization hides
            # a broken input or changes the exact production postprocessing path.
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(source)], check=True)
            for invariant, nocontract in (('0', '0'), ('1', '0'), ('0', '1'), ('1', '1')):
                with self.subTest(module=source.name, invariant=invariant, nocontract=nocontract):
                    target = output / f'{source.stem}-{invariant}{nocontract}.spv'
                    subprocess.run([str(self.probe), str(source), str(target), invariant, nocontract], check=True)
                    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(target)], check=True)
                    if invariant == nocontract == '0':
                        self.assertEqual(source.read_bytes(), target.read_bytes())


if __name__ == '__main__':
    unittest.main()

"""Exercise production alternate seed I/O and namespace selection in fresh processes."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'needs Linux cc')
class ShaderSeedTests(unittest.TestCase):
    def test_alternate_seed_and_isolated_state(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / 'shader-seed'
            command = [
                'cc', '-std=gnu11', '-D_GNU_SOURCE', '-DGPU_VK', '-O1',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-I' + str(TOTK / 'runtime'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn/include'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/host'),
                '-I' + str(TOTK / 'android/compat/include'),
                '-include', str(TOTK / 'android/compat/include/darwin_compat.h'),
                str(TOTK / 'android/gfx/test_vk_shader_seed.c'),
                str(TOTK / 'android/gfx/vk_seed_io.c'),
                str(TOTK / 'android/gfx/vk_shader_xlate.c'),
                str(TOTK / 'android/gfx/host_test_support.c'), '-lpthread', '-ldl', '-o', str(probe),
            ]
            subprocess.run([*command, '-DTOTK_GAME_V143=1'], check=True, capture_output=True, text=True)
            for folder, marker in (('shaders', 'original'), ('shaders/v143', 'canonical-v143'),
                                   ('shaders/v0', 'canonical-v0'), ('seed-opt/v143', 'optimized')):
                dest = root / folder
                dest.mkdir(parents=True)
                (dest / 'probe.spv').write_text(marker)
            env = dict(os.environ)
            env.pop('TOTK_SHADER_SEED_DIR', None)
            env.pop('TOTK_VK_POS_INVARIANT', None)
            env.pop('TOTK_VK_POS_NOCONTRACT', None)
            env['TEST_GAME_DIR'] = str(root)
            launch = root / 'unrelated-working-directory'
            launch.mkdir()

            def run(folder, marker, indexed=False, position=('0', '0'), mutate=False):
                e = dict(env)
                e['TOTK_VK_SEED_IO'] = '1' if indexed else '0'
                e['TOTK_VK_POS_INVARIANT'], e['TOTK_VK_POS_NOCONTRACT'] = position
                if mutate:
                    e['TEST_POSITION_MUTATE'] = '1'
                if folder is not None:
                    e['TOTK_SHADER_SEED_DIR'] = folder
                result = subprocess.run([str(probe), marker], cwd=launch, env=e, text=True, capture_output=True, check=True)
                lines = result.stdout.splitlines()
                self.assertIn(f'shader seed: {lines[0]}', result.stderr)
                for name, value in zip(('INVARIANT', 'NOCONTRACT'), position):
                    self.assertEqual(result.stderr.count(f'TOTK_VK_POS_{name}={value} (startup-only)'), 1)
                self.assertTrue(Path(lines[0]).is_absolute())
                return lines

            original = run(None, 'original')
            alternate = run('seed-opt/v143', 'optimized')
            self.assertEqual(original[0], str(root / 'shaders'))
            self.assertEqual(run(None, 'original', indexed=True)[1:4], original[1:4])
            self.assertEqual(run('seed-opt/v143', 'optimized', indexed=True)[1:4], alternate[1:4])
            legacy = ['state/vk-pipeline-cache.bin', 'state/vk-prewarm.bin', 'custom-import']
            self.assertEqual(original[1:4], legacy)
            # All four immutable precision arms isolate pipeline, prewarm and
            # explicit import paths, for canonical and alternate seeds alike.
            for folder, marker, baseline in ((None, 'original', original),
                                             ('seed-opt/v143', 'optimized', alternate)):
                paths = []
                for invariant, nocontract in (('0', '0'), ('1', '0'), ('0', '1'), ('1', '1')):
                    position = (invariant, nocontract)
                    arm = run(folder, marker, position=position, mutate=True)
                    suffix = '' if position == ('0', '0') else f'.pos-v1-{invariant}{nocontract}'
                    self.assertEqual(arm[1:4], [path + suffix for path in baseline[1:4]])
                    paths.append(tuple(arm[1:4]))
                self.assertEqual(len(set(paths)), 4)
            for folder in ('', 'shaders', './shaders', str(root / 'shaders')):
                with self.subTest(default_alias=folder):
                    self.assertEqual(run(folder, 'original')[1:4], legacy)
            (root / 'default-alias').symlink_to(root / 'shaders/v143', target_is_directory=True)
            (root / 'shaders/v143/seed-id.txt').write_text('b' * 64 + '\n')
            for folder in ('shaders/v143', str(root / 'shaders/v143'), 'default-alias'):
                with self.subTest(versioned_default_alias=folder):
                    self.assertEqual(run(folder, 'canonical-v143')[1:4], legacy)
            self.assertEqual(alternate[0], str(root / 'seed-opt/v143'))
            for idx in (1, 2, 3):
                self.assertNotEqual(original[idx], alternate[idx])
            alias = run(str(root / 'seed-opt/v143'), 'optimized')
            self.assertEqual(alternate[1:4], alias[1:4])
            (root / 'seed-opt/v143/seed-id.txt').write_text('a' * 64 + '\n')
            revision = run('seed-opt/v143', 'optimized')
            for idx in (1, 2, 3):
                self.assertNotEqual(alternate[idx], revision[idx])
            self.assertEqual(len(set(line.rsplit('.', 1)[1] for line in revision[1:4])), 1)
            empty = root / 'empty-seed'
            empty.mkdir()
            (empty / 'not-a-module.spv').mkdir()  # a directory with that suffix is not a module
            for folder, reason in (('missing-seed', 'directory missing or inaccessible'),
                                   (str(root / 'missing-seed'), 'directory missing or inaccessible'),
                                   ('empty-seed', 'no .spv modules')):
                with self.subTest(unready=folder):
                    e = dict(env, TOTK_SHADER_SEED_DIR=folder, TOTK_VK_SEED_IO='0')
                    result = subprocess.run([str(probe), '--unready'], cwd=launch, env=e,
                                            text=True, capture_output=True, check=True)
                    expected = Path(folder) if Path(folder).is_absolute() else root / folder
                    self.assertEqual(result.stdout.strip(), str(expected))
                    self.assertEqual(result.stderr.count('shader seed error:'), 1)
                    self.assertIn(str(expected), result.stderr)
                    self.assertIn(reason, result.stderr)
            # Non-v143 builds use the canonical v0 folder without a suffix too.
            subprocess.run(command, check=True, capture_output=True, text=True)
            for folder in ('shaders/v0', str(root / 'shaders/v0')):
                with self.subTest(v0_default_alias=folder):
                    self.assertEqual(run(folder, 'canonical-v0')[1:4], legacy)


if __name__ == '__main__':
    unittest.main()

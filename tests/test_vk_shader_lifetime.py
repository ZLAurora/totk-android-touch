"""Pin borrowed profiler names to production shader-cache ownership, without a GPU."""
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
class ShaderLifetimeTests(unittest.TestCase):
    def test_names_survive_publication_growth_release_and_seed_shutdown(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / 'shader-lifetime'
            subprocess.run([
                'cc', '-std=gnu11', '-D_GNU_SOURCE', '-DGPU_VK', '-O1',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-I' + str(TOTK / 'runtime'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/gfx/nvn/include'),
                '-I' + str(TOTK / 'third_party/d2r_gfx/host'),
                '-I' + str(TOTK / 'android/compat/include'),
                '-include', str(TOTK / 'android/compat/include/darwin_compat.h'),
                str(TOTK / 'android/gfx/test_vk_shader_lifetime.c'),
                str(TOTK / 'android/gfx/vk_seed_io.c'),
                str(TOTK / 'android/gfx/host_test_support.c'),
                '-lpthread', '-ldl', '-o', str(probe),
            ], check=True, capture_output=True, text=True, timeout=120)
            for indexed in (0, 1):
                with self.subTest(indexed_seed_io=indexed):
                    game = root / f'game-{indexed}'
                    game.mkdir()
                    env = dict(os.environ, TOTK_SHADER_SEED_DIR='shaders',
                               TOTK_VK_SEED_IO=str(indexed), TOTK_VK_EARLYZ='1')
                    run = subprocess.run([str(probe)], cwd=game, env=env, text=True,
                                         capture_output=True, check=True, timeout=60)
                    self.assertIn('borrowed shader lifetime passed', run.stdout)


if __name__ == '__main__':
    unittest.main()

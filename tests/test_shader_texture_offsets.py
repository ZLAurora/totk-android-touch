"""Synthetic Maxwell gathers through the production translator; no game data."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'games/totk/tools/totk_xlate'
GFX = ROOT / 'games/totk/android/gfx'
DOTNET = shutil.which(os.environ.get('DOTNET', 'dotnet'))
TRANSLATOR = os.environ.get('TOTK_TEST_TRANSLATOR')


def guest_code(values=False, four_offsets=True):
    """Hand-encode instruction fields, including one scheduling word per three ops.

    The reduction is LDC R254,c[2][0]; TLD4.PTP R0,RZ,R254;
    STS [RZ],R0; EXIT. One uniform word supplies four signed-six-bit
    offsets, the second is zero. The value fixture uses two LDC.64s for
    coordinates/offsets, LDC.64 for the output address, and STG.128.
    """
    def ldc(dest, slot, offset, size=5):
        return (0xef90000000000000 | (size << 48) | (1 << 44) |
                (slot << 36) | (offset << 20) | (7 << 16) | (255 << 8) | dest)

    def gather(dest, ra, rb, mask):
        return (0xc800000000000000 | ((2 if four_offsets else 1) << 54) |
                (7 << 51) | (0x5a << 36) | (mask << 31) | (2 << 28) |
                (rb << 20) | (7 << 16) | (ra << 8) | dest)

    if values:
        ops = [ldc(0, 2, 0), ldc(2, 2, 8), ldc(8, 0, 0x110), gather(0, 0, 2, 15),
               0xeed8000000000000 | (6 << 48) | (1 << 45) | (7 << 16) | (8 << 8)]
    else:
        ops = [ldc(254, 2, 0, 4), gather(0, 255, 254, 1),
               0xef58000000000000 | (4 << 48) | (7 << 16) | (255 << 8)]
    ops.append(0xe30000000007000f)
    words = []
    for i in range(0, len(ops), 3):
        words.extend([0, *ops[i:i + 3]])
    words.extend([0] * (-len(words) % 4))
    return struct.pack('<' + 'Q' * len(words), *words)


@unittest.skipUnless((TRANSLATOR or DOTNET) and shutil.which('spirv-val'),
                     'needs .NET 10 SDK (or TOTK_TEST_TRANSLATOR) and SPIRV-Tools')
class TextureOffsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.translator = TRANSLATOR or ''
        if not cls.translator:
            assert DOTNET is not None
            out = cls.work / 'translator'
            result = subprocess.run(['sh', str(TOOLS / 'publish_native.sh'), '--dotnet', DOTNET,
                                     '--out', str(out)], capture_output=True, text=True, timeout=300)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.translator = str(out / 'totk_xlate')

    def translate(self, name, **kwargs):
        job = dict(key=name, stage='cs', code_hex=guest_code(**kwargs).hex(), c1='',
                   local=[1, 1, 1], shared=4, depth_minus_w=True, gather_bias=True,
                   tex={'0': '2d'}, softfloat64=True)
        result = subprocess.run([self.translator, '--serve'],
                                input=json.dumps(dict(job=job, out=str(self.work))) + '\n',
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ok ' + name, result.stdout)
        path = self.work / (name + '.spv')
        validation = subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(path)],
                                    capture_output=True, text=True, timeout=30)
        self.assertEqual(validation.returncode, 0, validation.stdout + validation.stderr)
        # This FP32 fixture must not require shaderFloat64 on the Vulkan device.
        words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
        index = 5
        while index < len(words):
            count, opcode = words[index] >> 16, words[index] & 0xffff
            if opcode == 17:  # OpCapability
                self.assertNotEqual(words[index + 1], 10, 'unexpected Float64 capability')
            index += count
        return path, json.loads((self.work / (name + '.res.json')).read_text())

    def test_runtime_single_and_per_texel_offsets_validate(self):
        for four in (False, True):
            with self.subTest(four_offsets=four):
                self.translate('offsets-' + str(int(four)), four_offsets=four)

    @unittest.skipUnless(shutil.which('cc'), 'needs C compiler and Vulkan development files')
    def test_lavapipe_values(self):
        icds = sorted(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'))
        if not icds:
            self.skipTest('lavapipe ICD not installed')
        path, resources = self.translate('offset-values', values=True)
        probe = self.work / 'texture-offsets'
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        str(GFX / 'test_vk_texture_offsets.c'), '-lvulkan', '-o', str(probe)],
                       capture_output=True, text=True, check=True, timeout=60)
        cbuf = next(row['binding'] for row in resources['cbufs'] if row['slot'] == 2)
        self.assertEqual(len(resources['textures']), 1)
        self.assertEqual(len(resources['sbufs']), 1)
        args = [str(probe), str(path), str(cbuf), str(resources['textures'][0]['binding']),
                str(resources['sbufs'][0]['binding'])]
        result = subprocess.run(args, env=dict(os.environ, VK_ICD_FILENAMES=str(icds[0])),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('Validation Error', result.stdout + result.stderr)
        self.assertIn('32/32 exact results (8 inputs x 4 components)', result.stdout)
        print(result.stdout.strip())
        print(result.stderr.strip())


if __name__ == '__main__':
    unittest.main()

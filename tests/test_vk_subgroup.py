"""Production SPIR-V rewrite: validator, wave32/64/128 oracle and lavapipe."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
SPEC = importlib.util.spec_from_file_location('subgroup_inventory', GFX.parent / 'inventory_subgroups.py')
assert SPEC is not None and SPEC.loader is not None
INVENTORY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INVENTORY)


def words(path):
    data = path.read_bytes()
    return struct.unpack('<%dI' % (len(data) // 4), data)


def execute(w, width, active=None):
    """Small fixture-only SIMD interpreter. Execute emitted instructions, not
    a second implementation of the rewrite, at physical sizes unavailable on CI.
    The game shader translator is never emulated by this oracle.
    """
    lanes = 128
    active = [True] * lanes if active is None else active
    value, builtin, output = {}, {}, {}
    def constant(x):
        return [x] * lanes
    def binary(a, b, fn):
        return [fn(x, y) & 0xffffffff for x, y in zip(value[a], value[b])]
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 65535
        a = w[i+1:i+n]
        if op == 71 and a[1] == 11:
            builtin[a[0]] = a[2]
        elif op == 41:
            value[a[1]] = constant(1)
        elif op == 43:
            value[a[1]] = constant(a[2])
        elif op == 61:
            kind = builtin[a[2]]
            value[a[1]] = ([j % width for j in range(lanes)] if kind == 41 else
                           constant(width) if kind == 36 else [(j, 0, 0) for j in range(lanes)])
        elif op in (77, 81):
            index = value[a[3]] if op == 77 else constant(a[3])
            value[a[1]] = [v[j] for v, j in zip(value[a[2]], index)]
        elif op == 80:
            value[a[1]] = list(zip(*(value[x] for x in a[2:])))
        elif op == 83:
            value[a[1]] = value[a[2]]
        elif op in (128, 132, 134, 170, 171, 194, 197, 198, 199):
            fn = {128: lambda x, y: x+y, 132: lambda x, y: x*y, 134: lambda x, y: x//y,
                  170: lambda x, y: int(x == y), 171: lambda x, y: int(x != y),
                  194: lambda x, y: x >> y, 197: lambda x, y: x | y,
                  198: lambda x, y: x ^ y, 199: lambda x, y: x & y}[op]
            value[a[1]] = binary(a[2], a[3], fn)
        elif op == 169:
            value[a[1]] = [x if p else y for p, x, y in zip(value[a[2]], value[a[3]], value[a[4]])]
        elif op == 339:
            result = []
            for base in range(0, lanes, width):
                bits = sum(int(active[base+j] and bool(value[a[3]][base+j])) << j for j in range(width))
                result.extend([tuple((bits >> (32*k)) & 0xffffffff for k in range(4))] * width)
            value[a[1]] = result
        elif op == 345:
            value[a[1]] = [value[a[3]][j//width*width + value[a[4]][j]] for j in range(lanes)]
        elif op == 65:
            value[a[1]] = value[a[-1]]
        elif op == 62:
            for j, (at, v) in enumerate(zip(value[a[0]], value[a[1]])):
                if active[j]:
                    output[at] = v
        elif op in (334, 335):
            raise AssertionError('unrewritten vote')
        elif op not in (14, 15, 16, 17, 19, 20, 21, 23, 29, 30, 32, 33, 54, 56, 59, 71, 72, 248, 253):
            raise AssertionError('unexpected fixture opcode %d' % op)
        i += n
    return [[output[j*7+k] for k in range(7)] if active[j] else None for j in range(lanes)]


@unittest.skipUnless(all(shutil.which(x) for x in ('cc', 'spirv-as', 'spirv-val')), 'needs C compiler and SPIRV-Tools')
class SubgroupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.temp.name)
        cls.probe = cls.path / 'probe'
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-I'+str(GFX),
                        str(GFX/'test_vk_subgroup.c'), '-lvulkan', '-o', str(cls.probe)],
                       check=True, capture_output=True, text=True)
        cls.source, cls.fixed = cls.path/'source.spv', cls.path/'fixed.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.2', str(GFX/'subgroup_test.spvasm'), '-o', str(cls.source)], check=True)
        subprocess.run([str(cls.probe), str(cls.source), str(cls.fixed)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_validator_and_inventory(self):
        for path in (self.source, self.fixed):
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(path)], check=True, capture_output=True, text=True)
        row = INVENTORY.inspect(self.source.read_bytes())
        self.assertEqual(row['operations'], dict(ballot=1, shuffle=1, all=1, any=1))
        self.assertEqual(row['stages'], ['compute'])
        row = INVENTORY.inspect(self.fixed.read_bytes())
        self.assertNotIn('all', row['operations'])
        self.assertNotIn('any', row['operations'])

    def test_physical_wave32_64_128(self):
        for width in (32, 64, 128):
            with self.subTest(width=width):
                result = execute(words(self.fixed), width)
                for j, actual in enumerate(result):
                    pred = int(not ((j//32) & 1))
                    self.assertEqual(actual, [j & 31, 0xffffffff if pred else 0, 0, j ^ 1, pred, pred, 32])

    def test_divergent_active_masks(self):
        active = [(j & 31) < 32 - 4 * (j // 32) and (j // 2) % 3 != 1 for j in range(128)]
        for width in (32, 64, 128):
            result = execute(words(self.fixed), width, active)
            for j, actual in enumerate(result):
                if not active[j]:
                    continue
                base = j & ~31
                mask = sum(int(active[base+k]) << k for k in range(32))
                pred = int(not ((j//32) & 1))
                self.assertEqual(actual, [j & 31, mask if pred else 0, 0, j ^ 1, pred, pred, 32])

    def test_lavapipe(self):
        icds = sorted(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'))
        if not icds:
            self.skipTest('lavapipe ICD not installed')
        env = dict(os.environ, VK_ICD_FILENAMES=str(icds[0]))
        result = subprocess.run([str(self.probe), str(self.source), str(self.path/'gpu.spv'), 'gpu'],
                                env=env, check=True, capture_output=True, text=True)
        self.assertIn('outputs match 32-lane active-mask semantics', result.stderr)
        print(result.stderr.strip())

    def test_reject_unsupported_and_truncated(self):
        w = list(words(self.source))
        i = 5
        while w[i] & 65535 != 345:
            i += w[i] >> 16
        w[i] = (w[i] & 0xffff0000) | 346
        bad = self.path/'bad.spv'
        bad.write_bytes(struct.pack('<%dI' % len(w), *w))
        self.assertEqual(subprocess.run([str(self.probe), str(bad), str(self.path/'unused.spv')]).returncode, 2)
        raw = list(words(self.source)); raw[5] = (65535 << 16) | 17
        bad.write_bytes(struct.pack('<%dI' % len(raw), *raw))
        self.assertEqual(subprocess.run([str(self.probe), str(bad), str(self.path/'unused.spv')]).returncode, 2)

    def test_injected_lane_builtin(self):
        assembly = (GFX/'subgroup_test.spvasm').read_text()
        # Vote-only input has no lane builtin; the rewrite must add its interface,
        # declaration, decoration and ballot capability in legal module order.
        assembly = assembly.replace(' %lane %global %size', ' %global %size')
        assembly = '\n'.join(line for line in assembly.splitlines() if not (
            line.startswith('OpDecorate %lane ') or line.startswith('%lane =') or line.startswith('%lid =')))
        assembly = assembly.replace('%target = OpBitwiseXor %uint %lid %one', '%target = OpBitwiseXor %uint %gid %one')
        assembly = assembly.replace('OpStore %p0 %lid', 'OpStore %p0 %gid')
        asm = self.path/'injected.spvasm'; asm.write_text(assembly)
        raw, fixed = self.path/'injected.spv', self.path/'injected-fixed.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.2', str(asm), '-o', str(raw)], check=True)
        subprocess.run([str(self.probe), str(raw), str(fixed)], check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(fixed)], check=True, capture_output=True, text=True)

    def test_fragment_vote_injects_flat_lane(self):
        assembly = '''OpCapability Shader
OpCapability GroupNonUniformVote
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main"
OpExecutionMode %main OriginUpperLeft
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%scope = OpConstant %uint 3
%true = OpConstantTrue %bool
%main = OpFunction %void None %fn
%entry = OpLabel
%all = OpGroupNonUniformAll %bool %scope %true
OpReturn
OpFunctionEnd
'''
        asm = self.path/'fragment.spvasm'; asm.write_text(assembly)
        raw, fixed = self.path/'fragment.spv', self.path/'fragment-fixed.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.2', str(asm), '-o', str(raw)], check=True)
        subprocess.run([str(self.probe), str(raw), str(fixed)], check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(fixed)], check=True)

    def test_unused_builtin_keeps_bytes(self):
        assembly = (GFX/'subgroup_test.spvasm').read_text().split('%main = OpFunction')[0]
        assembly += '%main = OpFunction %void None %fn\n%entry = OpLabel\nOpReturn\nOpFunctionEnd\n'
        asm = self.path/'unused.spvasm'; asm.write_text(assembly)
        raw, fixed = self.path/'unused.spv', self.path/'unused-fixed.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.2', str(asm), '-o', str(raw)], check=True)
        subprocess.run([str(self.probe), str(raw), str(fixed)], check=True)
        self.assertEqual(raw.read_bytes(), fixed.read_bytes())
        row = INVENTORY.inspect(raw.read_bytes())
        self.assertTrue(row['builtins'])
        self.assertFalse(row['builtin_loads'])


class InventoryTests(unittest.TestCase):
    def test_unused_capability_not_counted(self):
        w = [0x07230203, 0x00010300, 0, 1, 0, 2 << 16 | 17, 64]
        row = INVENTORY.inspect(struct.pack('<7I', *w))
        self.assertFalse(row['operations'])
        self.assertFalse(row['builtins'])

    def test_bad_instruction(self):
        with self.assertRaises(ValueError):
            INVENTORY.inspect(struct.pack('<6I', 0x07230203, 0x00010300, 0, 1, 0, 0))

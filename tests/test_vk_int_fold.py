"""Integer-fold proof boundaries and consumed synthetic lavapipe pixels; no game data."""
from pathlib import Path
import hashlib
import importlib.util
import os
import shlex
import shutil
import subprocess
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
CC = shlex.split(os.environ.get('CC', 'cc'))


def decoration(target, name, grouped):
    if grouped:
        return f'''%decorations = OpDecorationGroup
OpDecorate %decorations {name}
OpGroupDecorate %decorations %unrelated {target}
'''
    return f'OpDecorate {target} {name}\n'


def compute_module(body, annotations='', local='', extension='', unsigned=False, second='%b'):
    """Both observed values escape to storage; the input is genuinely dynamic."""
    scalar = '%uint' if unsigned else '%int'
    source = f'''OpCapability Shader
{extension}OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %invocations
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %invocations BuiltIn GlobalInvocationId
OpDecorate %sink Block
OpMemberDecorate %sink 0 Offset 0
OpMemberDecorate %sink 1 Offset 4
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
{annotations}%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v3uint = OpTypeVector %uint 3
%input_ptr = OpTypePointer Input %v3uint
%sink = OpTypeStruct %scalar %scalar
%sink_ptr = OpTypePointer StorageBuffer %sink
%output_ptr = OpTypePointer StorageBuffer %scalar
%local_int = OpTypePointer Function %int
%local_float = OpTypePointer Function %float
%invocations = OpVariable %input_ptr Input
%output = OpVariable %sink_ptr StorageBuffer
%zero = OpConstant %scalar 0
%one = OpConstant %scalar 1
%high = OpConstant %scalar 65536
%main = OpFunction %void None %fn
%entry = OpLabel
{local}%invocation = OpLoad %v3uint %invocations
%raw = OpCompositeExtract %uint %invocation 0
%value = {'OpCopyObject' if unsigned else 'OpBitcast'} %scalar %raw
%unrelated = OpIAdd %scalar %value %zero
{body}%first = OpAccessChain %output_ptr %output %zero
%second = OpAccessChain %output_ptr %output %one
OpStore %first %a
OpStore %second {second}
OpReturn
OpFunctionEnd
'''
    return source.replace('%scalar', scalar)


def instructions(path):
    data = path.read_bytes()
    words = struct.unpack(f'<{len(data) // 4}I', data)
    offset = 5
    while offset < len(words):
        count = words[offset] >> 16
        yield offset, words[offset] & 0xffff, words[offset:offset + count]
        offset += count


@unittest.skipUnless(CC and shutil.which(CC[0]), 'needs a host C compiler')
class IntegerFoldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.probe = cls.work / 'vkintfoldtest'
        subprocess.run([*CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(GFX / 'test_vk_int_fold.c'), '-o', str(cls.probe)], check=True)

    def test_bit_algebra_and_conservative_boundaries(self):
        result = subprocess.run([str(self.probe)], check=True, capture_output=True, text=True)
        self.assertIn('boundaries, escape, volatile, malformed PASS', result.stdout)
        self.assertIn('decorations PASS', result.stdout)

    def fold_module(self, name, original):
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(original)], check=True)
        folded = self.work / f'{name}-folded.spv'
        result = subprocess.run([str(self.probe), str(original), str(folded)],
                                check=True, capture_output=True, text=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(folded)], check=True)
        return folded, int(result.stdout.strip())

    def assert_module_folds(self, name, source, expected):
        assembly = self.work / f'{name}.spvasm'
        assembly.write_text(source)
        original = assembly.with_suffix('.spv')
        subprocess.run(['spirv-as', '--target-env', 'spv1.3', str(assembly), '-o', str(original)],
                       check=True)
        folded, count = self.fold_module(name, original)
        self.assertEqual(count, expected)
        if not expected:
            self.assertEqual(original.read_bytes(), folded.read_bytes(),
                             'conservative decorated modules must remain byte-identical')

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIR-V tools')
    def test_valid_module_and_wraparound_address(self):
        source = self.work / 'address.spvasm'
        source.write_text('''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %position %index
OpDecorate %position BuiltIn Position
OpDecorate %index BuiltIn VertexIndex
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%vec = OpTypeVector %float 4
%input = OpTypePointer Input %int
%output = OpTypePointer Output %vec
%local = OpTypePointer Function %uint
%index = OpVariable %input Input
%position = OpVariable %output Output
%zero = OpConstant %uint 0
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%stride = OpConstant %uint 48
%one = OpConstant %float 1
%floatzero = OpConstant %float 0
%main = OpFunction %void None %fn
%entry = OpLabel
%temp = OpVariable %local Function
%i = OpLoad %int %index
%u = OpBitcast %uint %i
%addr = OpIMul %uint %u %stride
OpStore %temp %addr
%reload = OpLoad %uint %temp
%words = OpShiftRightLogical %uint %reload %two
%component = OpBitwiseAnd %uint %words %three
%x = OpConvertUToF %float %component
%pos = OpCompositeConstruct %vec %x %floatzero %floatzero %one
OpStore %position %pos
OpReturn
OpFunctionEnd
''')
        original, folded = self.work / 'address.spv', self.work / 'folded.spv'
        subprocess.run(['spirv-as', '--target-env', 'spv1.3', str(source), '-o', str(original)], check=True)
        result = subprocess.run([str(self.probe), str(original), str(folded)], check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), '1')
        for path in (original, folded):
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(path)], check=True)

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIR-V tools')
    def test_relaxed_precision_correlated_outputs(self):
        # With input zero, RP %a may execute at 16 bits and produce zero. An
        # undecorated %b must not become 65536 while that same %a is observed.
        body = '''%a = OpBitwiseOr %scalar %value %high
%b = OpBitwiseAnd %scalar %a %high
'''
        self.assert_module_folds('correlated-exact', compute_module(body), 1)
        for grouped in (False, True):
            with self.subTest(grouped=grouped):
                self.assert_module_folds(f'correlated-rp-{grouped}', compute_module(
                    body, decoration('%a', 'RelaxedPrecision', grouped)), 0)

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIR-V tools')
    def test_relaxed_precision_local_facts(self):
        for floating in (False, True):
            local_type = '%local_float' if floating else '%local_int'
            value_type = '%float' if floating else '%scalar'
            copy = 'OpBitcast' if floating else 'OpCopyObject'
            local = f'%temp = OpVariable {local_type} Function\n'
            body = f'''%bits = OpBitwiseOr %scalar %value %high
%stored = {copy} {value_type} %bits
OpStore %temp %stored
%reload = OpLoad {value_type} %temp
%a = {copy} %scalar %reload
%b = OpBitwiseAnd %scalar %a %high
'''
            self.assert_module_folds(f'local-exact-{floating}', compute_module(body, local=local), 1)
            for target in ('%temp', '%stored', '%reload'):
                for grouped in (False, True):
                    with self.subTest(floating=floating, target=target, grouped=grouped):
                        self.assert_module_folds(f'local-rp-{floating}-{target[1:]}-{grouped}',
                                                 compute_module(body, decoration(
                                                     target, 'RelaxedPrecision', grouped), local), 0)

        # Fully known local values must not turn a decorated load (or a load
        # through a decorated variable) into CopyObject either.
        body = '''OpStore %temp %high
%a = OpLoad %scalar %temp
%b = OpBitwiseAnd %scalar %a %high
'''
        local = '%temp = OpVariable %local_int Function\n'
        self.assert_module_folds('constant-local-exact', compute_module(body, local=local), 2)
        for target in ('%temp', '%a'):
            for grouped in (False, True):
                with self.subTest(constant_local=target, grouped=grouped):
                    self.assert_module_folds(f'constant-local-rp-{target[1:]}-{grouped}',
                                             compute_module(body, decoration(
                                                 target, 'RelaxedPrecision', grouped), local), 0)

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIR-V tools')
    def test_decorated_arithmetic_results_keep_opcode(self):
        body = '%a = OpIAdd %scalar %zero %zero\n'
        extension = 'OpExtension "SPV_KHR_no_integer_wrap_decoration"\n'
        for name in ('RelaxedPrecision', 'NoSignedWrap', 'NoUnsignedWrap'):
            unsigned = name == 'NoUnsignedWrap'
            self.assert_module_folds(f'add-exact-{name}', compute_module(
                body, extension=extension, unsigned=unsigned, second='%a'), 1)
            for grouped in (False, True):
                with self.subTest(decoration=name, grouped=grouped):
                    # In particular, the wrap decorations are legal on IAdd,
                    # but illegal on the CopyObject the pass otherwise emits.
                    self.assert_module_folds(f'add-{name}-{grouped}', compute_module(
                        body, decoration('%a', name, grouped), extension=extension,
                        unsigned=unsigned, second='%a'), 0)

    @unittest.skipUnless(all(shutil.which(x) for x in ('make', 'glslangValidator', 'spirv-val')),
                         'needs make, glslangValidator and SPIR-V validator')
    def test_consumed_ubo_lane_lavapipe_pixels(self):
        icds = sorted(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'))
        if not icds or not Path('/usr/include/vulkan/vulkan.h').is_file():
            self.skipTest('needs lavapipe ICD and Vulkan headers')
        build = self.work / 'renderer'
        subprocess.run(['make', '-C', str(GFX.parent), 'TARGET=linux',
                        f'CC={shlex.join(CC)}', f'OUT={build}', '-j2',
                        'vkpipetest', str(build / 'vkdrawtest')], check=True)
        seed = build / 'vkdraw-seed'
        spec = importlib.util.spec_from_file_location('int_fold_draw_seed', GFX / 'test_vkdraw_seed.py')
        assert spec is not None and spec.loader is not None
        seed_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(seed_module)
        key = seed_module.key_of(*seed_module.shader_binary(0, 1))
        original = seed / f'{key}-000000000000.spv'
        folded, count = self.fold_module('consumed-lane', original)
        self.assertGreater(count, 0, 'the live UBO component must exercise integer folding')
        before, after = dict((i, (op, w)) for i, op, w in instructions(original)), dict(
            (i, (op, w)) for i, op, w in instructions(folded))
        components = [(i, w) for i, (op, w) in before.items() if op == 199]
        self.assertEqual(len(components), 1, 'the shader must retain its live component AND')
        offset, component = components[0]
        op, replacement = after[offset]
        self.assertEqual((op, replacement[1:3]), (83, component[1:3]))
        constants = {w[2]: (w[1], w[3]) for op, w in before.values() if op == 43 and len(w) == 4}
        self.assertEqual(constants[replacement[3]], (component[1], 0),
                         'the consumed component, not merely a dead expression, must fold to zero')

        # Reuse the existing actual-backend renderer fixtures, not a substitute
        # shader runner: the production loader selects the off/on pass policy.
        env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        env.update(VK_DRIVER_FILES=str(icds[0]), VK_ICD_FILENAMES=str(icds[0]),
                   TOTK_SHADER_SEED_DIR=str(seed), TOTK_FRAME_LAG='0', TOTK_VK_REC_THREADS='0',
                   TOTK_VK_PIPELINE_CACHE='0', TOTK_PSO_PREWARM='0', TOTK_VK_PIPELINE_LIBRARY='0',
                   XDG_CACHE_HOME=str(self.work / 'cache'))
        pixels = []
        for arm in ('0', '1'):
            cwd = build / f'arm-{arm}'
            cwd.mkdir()
            arm_env = dict(env, TOTK_VK_INT_FOLD=arm)
            seed_tag = hashlib.sha256(str(seed.resolve()).encode() + b'\0' + bytes(65)).hexdigest()
            import_base = f'vk-pipeline-cache.bin.{seed_tag}'
            effective = import_base + ('.intfold-v1' if arm == '1' else '')
            opposite = import_base + ('.intfold-v1' if arm == '0' else '')
            wrong_arm = cwd / opposite
            wrong_arm.write_bytes(b'other-arm cache must never be opened or relabelled')
            # This fixture checks every RGBA8 and real D32 pixel against exact
            # expected values, including persistence without another draw.
            result = subprocess.run([str(build / 'vkpipetest'), '--screen-classify'], cwd=cwd,
                                    env=arm_env, check=True, capture_output=True, text=True, timeout=180)
            self.assertIn('0 failed', result.stdout)
            output = cwd / 'identity.pixels'
            message = f'[pipeline-cache] import skipped: {opposite} does not match effective namespace {effective}'
            self.assertEqual((result.stdout + result.stderr).count(message), 1)
            self.assertEqual(wrong_arm.read_bytes(), b'other-arm cache must never be opened or relabelled')
            self.assertFalse((cwd / (opposite + '.stale')).exists())
            self.assertFalse((cwd / (opposite + '.imported')).exists())
            result = subprocess.run([str(build / 'vkdrawtest'), '--rec-identity', str(output)],
                                    cwd=cwd, env=arm_env, check=True, capture_output=True,
                                    text=True, timeout=180)
            self.assertIn('0 failed', result.stdout)
            data = output.read_bytes()
            # Twelve rounds of four 64x64 RGBA8 passes plus exact query/draw
            # counters and a footer. Empty or truncated equality cannot pass.
            self.assertEqual(len(data), 12 * (24 + 4 * (64 * 64 * 4 + 24)) + 16)
            self.assertEqual(data[-16:], struct.pack('<QQ', 0x5245434944454e33, 12))
            pixels.append(data)
        self.assertEqual(pixels[0], pixels[1], 'off/on rendered pixels and counters must be bit-identical')


if __name__ == '__main__':
    unittest.main()

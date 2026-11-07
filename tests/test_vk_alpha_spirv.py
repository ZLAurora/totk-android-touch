"""Validate production alpha epilogues, including arrays and early-test removal."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'games/totk/android/gfx/vk_alpha_test.h'


@unittest.skipUnless(all(shutil.which(x) for x in ('cc', 'glslangValidator', 'spirv-val')),
                     'requires cc, glslang-tools and spirv-tools')
class AlphaSpirvTests(unittest.TestCase):
    def test_comparisons_and_output_shapes(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            helper = work / 'patch.c'
            helper.write_text(r'''
#include <stdio.h>
#include "vk_alpha_test.h"
int main(int argc, char **argv) {
    FILE *f=fopen(argv[1],"rb"); if (!f) return 2;
    fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    uint32_t *w=malloc((size_t)size); if (fread(w,1,(size_t)size,f)!=(size_t)size) return 3;
    fclose(f); size_t count=0;
    uint32_t *out=vk_spirv_alpha_test(w,(size_t)size/4,(unsigned)atoi(argv[3]),7,&count);
    free(w); if (!out) return 4;
    f=fopen(argv[2],"wb"); if (!f || fwrite(out,4,count,f)!=count) return 5;
    fclose(f); free(out); return 0;
}
''')
            binary = work / 'patch'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I', str(HEADER.parent),
                            str(helper), '-o', str(binary)], check=True, capture_output=True)
            shapes = (
                ('vec4', 'color', 'color=vec4(1,0,0,.5);'),
                ('vec4', 'color[2]', 'color[0]=vec4(1,0,0,.5);color[1]=vec4(0);'),
                ('ivec4', 'color', 'color=ivec4(1,0,0,1);'),
                ('uvec4', 'color', 'color=uvec4(1,0,0,1);'),
            )
            for shape, (kind, name, body) in enumerate(shapes):
                source = work / 'input.frag'
                source.write_text('#version 450\nlayout(early_fragment_tests) in;\n'
                                  f'layout(location=0) out {kind} {name};\nvoid main(){{{body}}}\n')
                original = work / 'original.spv'
                subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2',
                                str(source), '-o', str(original)], check=True, capture_output=True)
                for comparison in range(1, 8):
                    with self.subTest(shape=shape, comparison=comparison):
                        patched = work / 'alpha.spv'
                        subprocess.run([str(binary), str(original), str(patched), str(comparison)],
                                       check=True, capture_output=True)
                        validation = subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(patched)],
                                                    text=True, capture_output=True)
                        self.assertEqual(validation.returncode, 0, validation.stdout + validation.stderr)
                        words = patched.read_bytes()
                        # OpExecutionMode <entry> EarlyFragmentTests must be gone.
                        import struct
                        module = struct.unpack(f'<{len(words)//4}I', words)
                        offset = 5
                        while offset < len(module):
                            count, opcode = module[offset] >> 16, module[offset] & 65535
                            self.assertFalse(opcode == 16 and count >= 3 and module[offset + 2] == 9)
                            offset += count

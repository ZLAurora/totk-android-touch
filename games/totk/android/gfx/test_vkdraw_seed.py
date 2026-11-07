#!/usr/bin/env python3
"""Shader seed for android/gfx/test_vkdraw.c and test_vkpipeline.c: graphics and compute shaders with the Ryujinx resource names,
compiled to SPIR-V with glslangValidator and reflected by android/vk_reflect.py, filed under the keys the test's
synthetic NVN shader binaries hash to (SHA-256 of the 0x880-byte control section and the code area, as the runtime).
usage: test_vkdraw_seed.py <output folder>"""
import hashlib, json, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vk_reflect

VS = '''#version 450
layout(std140, set=0, binding=0) uniform support_buffer_t { vec4 alpha; } support_buffer;
layout(std140, set=0, binding=1) uniform vp_c3_t { vec4 off; } vp_c3;
layout(location=0) in vec2 pos;
layout(location=1) in vec2 delta;
void main() { gl_Position = vec4(pos + delta + vp_c3.off.xy, 0.0, 1.0); }
'''
FS = '''#version 450
layout(std140, set=0, binding=0) uniform support_buffer_t { vec4 x; } support_buffer;
layout(std140, set=0, binding=1) uniform fp_c3_t { vec4 color; } fp_c3;
layout(set=0, binding=2) uniform sampler2D fp_t_tcb_5A;
layout(location=0) out vec4 o0;
void main() { o0 = texture(fp_t_tcb_5A, gl_FragCoord.xy / 64.0) * fp_c3.color; }
'''
CS = '''#version 450
layout(local_size_x=64) in;
layout(std430, set=0, binding=0) buffer cp_s0_t { uint values[]; } cp_s0;
void main() { uint i = gl_GlobalInvocationID.x; cp_s0.values[i] = i ^ 0x12345678u; }
'''
FS_STORAGE = FS.replace('layout(location=0)', '''layout(std430, set=0, binding=3) buffer fp_s0_t { uint values[]; } fp_s0;
layout(location=0)''').replace(
    'void main() {', 'void main() { fp_s0.values[uint(gl_FragCoord.y) * 64u + uint(gl_FragCoord.x)] = 0x89abcde1u;')
FS_READONLY = '''#version 450
layout(std430, set=0, binding=0) readonly buffer fp_s0_t { vec4 color; } fp_s0;
layout(location=0) out vec4 o0;
void main() { o0 = fp_s0.color; }
'''

# Keep the walking fixture's identities separate from the small graphics/pipeline
# fixtures (salts 1..7 are reserved there). Every declared input affects a result.
VS_WALK = '''#version 450
layout(std140, set=0, binding=0) uniform vp_c3_t { vec4 off; } vp_c3;
layout(std140, set=0, binding=1) uniform vp_c4_t { vec4 off; } vp_c4;
layout(std140, set=0, binding=2) uniform vp_c5_t { vec4 off; } vp_c5;
layout(std140, set=0, binding=3) uniform vp_c6_t { vec4 off; } vp_c6;
layout(location=0) in vec2 pos;
layout(location=1) in vec2 delta1;
layout(location=2) in vec2 delta2;
layout(location=3) in vec2 delta3;
void main() {
    gl_Position = vec4(pos + delta1 + delta2 + delta3 +
        vp_c3.off.xy + vp_c4.off.xy + vp_c5.off.xy + vp_c6.off.xy, 0.0, 1.0);
}
'''
FS_WALK = '''#version 450
layout(std140, set=0, binding=0) uniform fp_c3_t { vec4 color; } fp_c3;
layout(std140, set=0, binding=1) uniform fp_c4_t { vec4 color; } fp_c4;
layout(std140, set=0, binding=2) uniform fp_c5_t { vec4 color; } fp_c5;
layout(std140, set=0, binding=3) uniform fp_c6_t { vec4 color; } fp_c6;
layout(std430, set=0, binding=4) readonly buffer fp_s0_t { vec4 color; } fp_s0;
layout(std430, set=0, binding=5) readonly buffer fp_s1_t { vec4 color; } fp_s1;
layout(std430, set=0, binding=6) readonly buffer fp_s2_t { vec4 color; } fp_s2;
layout(std430, set=0, binding=7) readonly buffer fp_s3_t { vec4 color; } fp_s3;
layout(location=0) out vec4 o0;
void main() {
    o0 = (fp_c3.color + fp_c4.color + fp_c5.color + fp_c6.color +
          fp_s0.color + fp_s1.color + fp_s2.color + fp_s3.color) * 0.125;
}
'''
FS_WALK_WRITE = FS_WALK.replace('layout(location=0)', '''
layout(std430, set=0, binding=8) buffer fp_s4_t { uint marker; } fp_s4;
layout(location=0)''').replace(
    'void main() {', 'void main() { atomicExchange(fp_s4.marker, 0x89abcde1u);')

# Three genuinely consumed streams, with per-triangle colours so the device-shaped
# fixture checks both changing vertex addresses and changing index contents.
VS_WALK_DEVICE = '''#version 450
layout(location=0) in vec2 pos;
layout(location=1) in vec2 color_rg;
layout(location=2) in vec2 color_ba;
layout(location=0) out vec4 color;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    color = vec4(color_rg, color_ba);
}
'''
FS_WALK_DEVICE = '''#version 450
layout(location=0) in vec4 color;
layout(location=0) out vec4 o0;
void main() { o0 = color; }
'''

# Owned-span sharing: adjacent and overlapping 1KiB bindings all consume both ends,
# so neither the offset sweep nor a partial first/last-vector overlay is dead input.
VS_WALK_SHARE = '''#version 450
layout(location=0) in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
'''
FS_WALK_SHARE = '''#version 450
layout(std140, set=0, binding=0) uniform fp_c3_t { vec4 color[64]; } fp_c3;
layout(std140, set=0, binding=1) uniform fp_c4_t { vec4 color[64]; } fp_c4;
layout(std140, set=0, binding=2) uniform fp_c5_t { vec4 color[64]; } fp_c5;
layout(std140, set=0, binding=3) uniform fp_c6_t { vec4 color[64]; } fp_c6;
layout(location=0) out vec4 o0;
void main() {
    o0 = (fp_c3.color[0] + fp_c3.color[63] + fp_c4.color[0] + fp_c4.color[63] +
          fp_c5.color[0] + fp_c5.color[63] + fp_c6.color[0] + fp_c6.color[63]) * 0.125;
}
'''


VS_NATIVE_UI = '''#version 450
layout(location=0) in vec2 pos;
layout(location=0) out vec2 uv;
void main() { gl_Position=vec4(pos,0,1); uv=vec2(pos.x*.5+.5,.5-pos.y*.5); }
'''
FS_NATIVE_UI = '''#version 450
layout(location=0) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D fp_t_tcb_8;
layout(location=0) out vec4 o0;
void main() { o0=texture(fp_t_tcb_8,uv); }
'''
FS_NATIVE_COORD = '''#version 450
layout(location=0) out vec4 o0;
void main() { o0=vec4(gl_FragCoord.x/32.0,gl_FragCoord.y/32.0,0,1); }
'''

# Scheduler-owned terminal corpus: all payloads are consumed, unlike the
# descriptor-only ownership fixture. Keep salts40/41 byte-for-byte unchanged.
VS_TERMINAL_OWNED = VS_NATIVE_UI
FS_TERMINAL_OWNED = '''#version 450
layout(location=0) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D fp_t_tcb_8;
layout(std140,set=0,binding=1) uniform fp_c3_t { vec4 tint; } fp_c3;
layout(location=0) out vec4 o0;
void main() { o0=texture(fp_t_tcb_8,uv)*fp_c3.tint; }
'''

FS_ATLAS_UV = '''#version 450
layout(std140,set=0,binding=0) uniform fp_c3_t { vec4 uv; } fp_c3;
layout(set=0,binding=1) uniform sampler2D fp_t_tcb_5A;
layout(location=0) out vec4 o0;
void main() { o0=texture(fp_t_tcb_5A,fp_c3.uv.xy); }
'''
# One array output spanning locations 0 and 1: the reflection's fout line names location 0 only
# (test_vkpipeline.c unwritten_probe: TOTK_VK_UNWRITTEN_MASK must not mask location 1).
FS_ARRAY_OUT = '''#version 450
layout(location=0) out vec4 o[2];
void main() { o[0]=vec4(1,1,1,1); o[1]=vec4(1,0,1,1); }
'''

def shader_binary(stage, salt):
    """The test's NVN shader: control section (stage at 0x714, code length at 0x704, c1 offset at 0x700) and code."""
    code = bytes([salt]) * 0x30 + bytes((i * 7 + salt) & 255 for i in range(0x200))
    control = bytearray(0x880)
    control[0x20:0x24] = (0x880).to_bytes(4, 'little')
    control[0x700:0x704] = len(code).to_bytes(4, 'little')
    control[0x704:0x708] = len(code).to_bytes(4, 'little')
    control[0x714:0x718] = stage.to_bytes(4, 'little')
    return bytes(control), code


def key_of(control, code):
    return hashlib.sha256(control + code).hexdigest()


def main(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        shaders = ((0, VS, 1, 'vert', []), (1, FS, 2, 'frag', []),
                   (5, CS, 3, 'comp', [{'binding': 0, 'cb_slot': 0, 'cb_offset': 0x210 // 4, 'flags': 1}]),
                   (1, FS_STORAGE, 4, 'frag', [{'binding': 3, 'cb_slot': 0, 'cb_offset': 0x510 // 4, 'flags': 1}]),
                   (1, FS_READONLY, 5, 'frag', [{'binding': 0, 'cb_slot': 0, 'cb_offset': 0x510 // 4}]),
                   (1, FS_READONLY, 6, 'frag', [{'binding': 0, 'cb_slot': 3, 'cb_offset': 0}]),
                   (0, VS_WALK, 32, 'vert', []),
                   (1, FS_WALK, 33, 'frag',
                    [{'binding': 4 + i, 'cb_slot': 0, 'cb_offset': (0x510 + 16 * i) // 4} for i in range(4)]),
                   (1, FS_WALK_WRITE, 34, 'frag',
                    [{'binding': 4 + i, 'cb_slot': 0, 'cb_offset': (0x510 + 16 * i) // 4,
                      'flags': int(i == 4)} for i in range(5)]),
                   (0, VS_WALK_DEVICE, 35, 'vert', []),
                   (1, FS_WALK_DEVICE, 36, 'frag', []),
                   (0, VS_WALK_SHARE, 37, 'vert', []),
                   (1, FS_WALK_SHARE, 38, 'frag', []),
                   (0, VS_NATIVE_UI, 40, 'vert', []),
                   (1, FS_NATIVE_UI, 41, 'frag', []),
                   (1, FS_NATIVE_COORD, 42, 'frag', []),
                   (1, FS_ATLAS_UV, 43, 'frag', []),
                   (0, VS_TERMINAL_OWNED, 44, 'vert', []),
                   (1, FS_TERMINAL_OWNED, 45, 'frag', []),
                   (1, FS_ARRAY_OUT, 46, 'frag', []))
        for stage, src, salt, ext, sbufs in shaders:
            control, code = shader_binary(stage, salt)
            key = key_of(control, code)
            variant = f'{key}-000000000000'
            g = Path(tmp) / f's.{ext}'; g.write_text(src)
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', str(g), '-o', str(out / f'{variant}.spv')], check=True, stdout=subprocess.DEVNULL)
            r = vk_reflect.reflect((out / f'{variant}.spv').read_bytes(), {'sbufs': sbufs}, stage)
            (out / f'{variant}.vk').write_text(vk_reflect.vk_text(r, stage, ''))
            (out / f'{key}.vv').write_text(f'{variant}\t\n')
            print(stage, key)


if __name__ == '__main__':
    main(sys.argv[1])

#!/usr/bin/env python3
"""Host-only fixed-time guard, register and native-continuation oracle.

Requires the player's authenticated 1.4.3 main NSO, lz4 and Unicorn. The repo
stores addresses/digests, not game words. No device, save or visual claim.
"""
import argparse
import json
from pathlib import Path
import struct

import fixture_seeds
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[1]
SITES = (list(range(0x68c0bc, 0x68c12c, 4)) + [0x68c540, 0x68c544] +
         list(range(0x68c760, 0x68c77c, 4)) + list(range(0x68c290, 0x68c2b0, 4)))
SHA256 = 'c509b61832d5d59862e3c167499e4aab31b4cbddac5923469ad8aff880b7ca7c'
STUB = 0x6f01b0
GPRS = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(31)] + [reg.UC_ARM64_REG_SP]
SIMD = [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
STATE = GPRS + SIMD + [reg.UC_ARM64_REG_NZCV, reg.UC_ARM64_REG_FPCR, reg.UC_ARM64_REG_FPSR]
DATA = 0x10000000
CLOCK, BANK = DATA + 0x100, DATA + 0x1000


def machine(image):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for page in (0x68c000, STUB & ~0xfff):
        mu.mem_map(page, 0x1000)
        mu.mem_write(page, image[page:page + 0x1000])
    mu.mem_map(DATA, 0x3000)
    return mu


def seed(mu, nzcv, flags):
    for i, r in enumerate(GPRS):
        mu.reg_write(r, 0x1122334455660000 + i * 0x101)
    for i, r in enumerate(SIMD):
        mu.reg_write(r, (0x8877665544332211 << 64) | (0x1234567800000000 + i))
    mu.reg_write(reg.UC_ARM64_REG_SP, DATA + 0x2ff0)
    mu.reg_write(reg.UC_ARM64_REG_X19, CLOCK)
    mu.reg_write(reg.UC_ARM64_REG_X21, BANK)
    mu.reg_write(reg.UC_ARM64_REG_X24, DATA + 0x2000)
    mu.reg_write(reg.UC_ARM64_REG_X9, 0x8f)
    mu.reg_write(reg.UC_ARM64_REG_X10, flags)
    mu.reg_write(reg.UC_ARM64_REG_NZCV, nzcv << 28)
    mu.reg_write(reg.UC_ARM64_REG_FPCR, 0x1c00000 if nzcv & 1 else 0)
    mu.reg_write(reg.UC_ARM64_REG_FPSR, 0x800009f if nzcv & 2 else 0)
    mu.mem_write(DATA, bytes([0x5a]) * 0x3000)
    mu.mem_write(CLOCK + 0x2c, struct.pack('<I', 12345))
    mu.mem_write(DATA + 0x21a0, struct.pack('<f', 86400.0))


def state(mu):
    return [mu.reg_read(r) for r in STATE]


def operand_checks(image, original):
    # Native baseline uses the authenticated source load, never a hand-coded
    # substitute. Compare all architectural state, not only caller-saved ABI.
    native = bytearray(image)
    for off in (0x68c124, 0x68c76c):
        native[off:off + 4] = original(off, 4)
    # Keep different stop boundaries in different Unicorn machines: cached
    # translation blocks otherwise retain an earlier run's stop boundary.
    veneer, patched, baseline = machine(image), machine(image), machine(bytes(native))
    expected = struct.unpack('<I', struct.pack('<f', 7200.0))[0]
    veneer_cases = seam_cases = 0
    for nzcv in range(16):
        seed(veneer, nzcv, 0)
        before = state(veneer)
        memory = bytes(veneer.mem_read(DATA, 0x3000))
        veneer.emu_start(0x68c76c, 0x68c770, count=4)
        assert veneer.reg_read(reg.UC_ARM64_REG_PC) == 0x68c770
        after = state(veneer)
        for i, value in enumerate(after):
            assert value == (expected if STATE[i] == SIMD[0] else before[i]), (nzcv, STATE[i])
        assert bytes(veneer.mem_read(DATA, 0x3000)) == memory
        veneer_cases += 1
        for flags in (0, 1 << 29, 1 << 30, (1 << 29) | (1 << 30)):
            for mu in (patched, baseline):
                seed(mu, nzcv, flags)
                mu.mem_write(BANK + 0xcb4, struct.pack('<f', 7200.0))
            # Stop before derived arithmetic, which deliberately uses other
            # engine constants. Both execute the native day/seconds publication
            # and effective-record selector unchanged.
            patched.emu_start(0x68c124, 0x68c2ac, count=20)
            baseline.emu_start(0x68c760, 0x68c2ac, count=20)
            assert patched.reg_read(reg.UC_ARM64_REG_PC) == 0x68c2ac
            assert state(patched) == state(baseline)
            assert bytes(patched.mem_read(DATA, 0x3000)) == bytes(baseline.mem_read(DATA, 0x3000))
            assert struct.unpack('<III', patched.mem_read(CLOCK + 0x58, 12)) == (expected, 12345, 0)
            assert patched.reg_read(reg.UC_ARM64_REG_X10) == CLOCK + 0x50
            seam_cases += 1
    return {'veneer_full_state': veneer_cases, 'native_exact_state_and_memory': seam_cases}


def verify(out, main, rewritten=None):
    out.mkdir(parents=True, exist_ok=True)
    original = fixture_seeds.read_main(main)
    fixture_seeds.check(original, SITES, SHA256)
    seeds = fixture_seeds.write_seeds(out / 'clock-seeds.bin', original, SITES)
    binary, dump = out / 'clock-fixture', out / 'clock.bin'
    fixture_seeds.build(ROOT / 'scripts/v143_bench_time_fixture.c', binary)
    run = fixture_seeds.run(binary, [dump], seeds)
    image = dump.read_bytes()
    result = {'status': 'pass', 'guards': len(SITES), 'fixture': run.stdout.strip(),
              **operand_checks(image, original),
              'scope': 'host guards and exact native seam state; no device/performance/save claim'}
    if rewritten is not None:
        real = fixture_seeds.run(binary, ['--image', rewritten])
        result['rewritten_image'] = json.loads(real.stdout)
        (out / 'clock-real-image.log').write_text(real.stdout + real.stderr)
    (out / 'clock-fixture.log').write_text(run.stdout + run.stderr)
    (out / 'clock-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main-nso', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--rewritten', type=Path, help='also check the pinned Android rewritten main and its executable tail')
    args = parser.parse_args()
    print(json.dumps(verify(args.output_dir, args.main_nso, args.rewritten)))

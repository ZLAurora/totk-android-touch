#!/usr/bin/env python3
"""Run the production v143 FPS patch against real controller instructions (Unicorn).

Accepts a caller-owned rewritten main, writes only --out, never runs the game.
The constructed listener exercises the original indirect-callback ABI, including
caller-saved register clobbers, veto and candidate override.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_SP,
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X8, UC_ARM64_REG_X10,
    UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X29, UC_ARM64_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
SITES = (0x2db204, 0x2db208, 0x2db22c, 0x2db360, 0x2db36c, 0x2db340, 0x2db34c)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--text-end', type=lambda s: int(s, 0), required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture, patched_path = out / 'fps60-fixture', out / 'main-fps60.bin'
    compat = ROOT / 'android/compat/include'
    subprocess.run([args.cc, '-std=gnu11', '-O1', '-D_GNU_SOURCE', '-DTOTK_GAME_V143=1',
        '-I' + str(compat), '-include', str(compat / 'darwin_compat.h'),
        '-I' + str(ROOT / 'third_party/d2r_gfx/host'), '-I' + str(ROOT / 'runtime'),
        str(ROOT / 'scripts/v143_timing_fixture.c'), str(ROOT / 'runtime/game_patches_v143.c'),
        str(ROOT / 'android/gfx/host_test_support.c'),
        '-lm', '-o', str(fixture)], check=True)
    env = os.environ | {'TOTK_FPS60': '1', 'TOTK_REALTIME': '0', 'TOTK_OUTPUT_RES': '0',
        'TOTK_ASPECT': '0', 'TOTK_RENDER_RES': '0', 'TOTK_NATIVE_UI': '0',
        'TOTK_HIGHFPS_FIX': '0', 'TOTK_DRS60': '0', 'TOTK_SHADOW_RES': '0',
        'TOTK_FAR_CLAMP': '0', 'TOTK_FXAA': '0', 'TOTK_FPS60_HYSTERESIS': '0'}
    raw = args.image.read_bytes()
    run = subprocess.run([str(fixture), str(args.image), str(patched_path), hex(args.text_end)],
        env=env, capture_output=True, text=True, check=True)
    (out / 'fixture.log').write_text(run.stdout + run.stderr)
    controller_offset = json.loads(run.stdout)['controller_offset']
    patched = patched_path.read_bytes()
    defaults = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    defaults.mem_map(0, (len(raw) + 0xfff) & ~0xfff)
    defaults.mem_write(0, raw)
    defaults.mem_map(0x10000000, 0x1000)
    defaults.reg_write(UC_ARM64_REG_X0, 0x10000000)
    defaults.reg_write(UC_ARM64_REG_LR, 0x10000800)
    defaults.emu_start(0xbdc060, 0x10000800, count=100)
    default_min, default_max, bias_a, bias_b, over, cooldown = struct.unpack(
        '<IIffII', defaults.mem_read(0x10000000, 24))
    assert (default_min, default_max, over, cooldown) == (1, 2, 2, 6)
    assert abs(bias_a - .05) < 1e-8 and bias_a == bias_b
    assert struct.unpack_from('<I', patched, 0x2db22c)[0] == 0xd503201f
    no_trigger = bytearray(patched)
    no_trigger[0x2db22c:0x2db230] = raw[0x2db22c:0x2db230]  # remove the existing lower-trigger patch: the caller's own word
    # Only the controller is executed. The rest of the image supplies its real
    # global tick-frequency load; all guest objects live in this private arena.
    def execute(image, listener='none', current=2, candidate=2, work_ms=5,
                cooldown=0, frozen=0, bias=0., threshold=0, cd_init=0, plan=None):
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.mem_map(0, (len(image) + 0xfff) & ~0xfff)
        mu.mem_write(0, bytes(image))
        arena = 0x10000000
        mu.mem_map(arena, 0x20000)
        system, window = arena + 0x1000, arena + 0x2000
        controller = system + controller_offset
        node, obj, table, callback = (arena + x for x in (0x3000, 0x4000, 0x5000, 0x6000))
        sp, stop = arena + 0x18000, arena + 0x7000
        def put(address, fmt, *values):
            mu.mem_write(address, struct.pack('<' + fmt, *values))
        put(0x3acceb8, 'Q', 19200000)
        put(controller, 'IIffII', 2, 3, bias, bias, threshold, cd_init)
        put(controller + 0x28, 'II', 60, 0)
        put(controller + 0x40, 'IIIIII', current, candidate, 0, cooldown, 60 // current, 60 // current)
        put(controller + 0x58, 'B', frozen)
        put(system + 0x150, 'Q', window)
        put(window + 0xa8, 'B', current)
        # Exercise the original indirect window call, not just sys+0x88.
        window_call, window_slot = arena + 0x8000, arena + 0x9000
        put(window + 0x70, 'Q', 0x1234)
        put(0x396e5d0, 'Q', window_slot)
        put(window_slot, 'Q', window_call)
        put(window_call, 'I', 0xd65f03c0)
        last_window = [current]
        sentinel = controller + 0x68
        put(controller + 0x70, 'Q', sentinel if listener == 'none' else node)
        put(node + 8, 'QQ', sentinel, obj)
        put(obj + 0x28, 'Q', table)
        put(table, 'Q', callback)
        put(callback, 'I', 0xd65f03c0)  # ret: returning listener with caller-saved clobbers
        calls = []
        def hook(uc, pc, size, user):
            if pc == window_call:
                last_window[0] = uc.reg_read(UC_ARM64_REG_X1)
                fp = uc.reg_read(UC_ARM64_REG_X29)
                saved_x20, saved_x19 = struct.unpack('<QQ', uc.mem_read(fp + 0x30, 16))
                assert saved_x19 == saved_x20 == system
                assert saved_x20 != controller, 'saved caller x20 is not the live controller'
                return
            if pc != callback:
                return
            calls.append(struct.unpack('<I', uc.mem_read(controller + 0x44, 4))[0])
            uc.reg_write(UC_ARM64_REG_X8, 0xbad)
            uc.reg_write(UC_ARM64_REG_X10, 0xbad)
            if listener == 'veto':
                uc.mem_write(uc.reg_read(UC_ARM64_REG_X1) + 0x18, b'\1')
            elif listener == 'override':
                put(controller + 0x44, 'I', 3)
        mu.hook_add(UC_HOOK_CODE, hook)
        def frame(a, b=None):
            put(system + 0x118, 'QQQ', 1, int(a * 19200), int((a if b is None else b) * 19200))
            mu.reg_write(UC_ARM64_REG_SP, sp)
            # Execute the real caller, not a manually supplied x0. This checks
            # the production address helper against add x0,x19,#0x200.
            mu.reg_write(UC_ARM64_REG_X19, system)
            mu.emu_start(0x2daa48, 0x2db174, count=3)
            assert mu.reg_read(UC_ARM64_REG_PC) == 0x2db174
            assert mu.reg_read(UC_ARM64_REG_X0) == controller
            assert mu.reg_read(UC_ARM64_REG_X1) == system
            mu.reg_write(UC_ARM64_REG_LR, stop)
            mu.reg_write(UC_ARM64_REG_X19, system)
            mu.reg_write(UC_ARM64_REG_X20, system)
            mu.emu_start(0x2db174, stop, count=2000)
            assert mu.reg_read(UC_ARM64_REG_PC) == stop
            assert mu.reg_read(UC_ARM64_REG_SP) == sp
            assert mu.reg_read(UC_ARM64_REG_X19) == system
            assert mu.reg_read(UC_ARM64_REG_X20) == system
            ticks = struct.unpack('<I', mu.mem_read(system + 0x88, 4))[0]
            selected = struct.unpack('<I', mu.mem_read(controller + 0x40, 4))[0]
            assert ticks == selected == last_window[0], (ticks, selected, last_window[0])
            # FPS60 changes comparison/select instructions, not stored config.
            # min/max=2/3 remains correct even when the selected interval is 1.
            assert struct.unpack('<II', mu.mem_read(controller, 8)) == (2, 3)
            nominal = struct.unpack('<I', mu.mem_read(controller + 0x50, 4))[0]
            return ticks, nominal, calls
        if plan is not None:
            return [frame(*work)[0] for work, count in plan for _ in range(count)]
        return frame(*work_ms) if isinstance(work_ms, tuple) else frame(work_ms)
    cases = []
    for listener, expected in [('none', 1), ('noop', 2), ('veto', 2), ('override', 3)]:
        result = execute(patched, listener)
        assert result[0] == expected, (listener, result)
        if listener == 'noop':
            assert result[2] == [2], result
        cases.append({'listener': listener, 'interval': result[0]})
    # A returning listener can retain 2 for one frame, but the continuation
    # lowers +0x44 and the next frame reaches 1. Do not mistake that reload for
    # a persistent fallback bug (a single-frame test gave that false lead).
    assert execute(patched, 'noop', plan=[((8.,), 3)]) == [2, 1, 1]
    assert execute(no_trigger, 'noop')[:2] == (2, 30)
    assert execute(raw, 'noop')[:2] == (2, 30)  # shipping mode unchanged
    assert execute(patched, 'noop', frozen=1)[0] == 2
    assert execute(patched, 'noop', cooldown=2)[0] == 2
    assert execute(patched, 'noop', work_ms=20)[0] == 2
    assert execute(patched, 'noop', work_ms=45)[0] == 3
    # Preserve the original verifier's persistent light/medium/heavy/recovery
    # matrix (including initial candidate 0, biases and independent work inputs).
    transitions = []
    workload = [((8.,), 200), ((25.,), 60), ((40.,), 60), ((8.,), 300)]
    for candidate in (0, 2, 3):
        for cooldown in (0, 30):
            for listener in ('none', 'noop'):
                seq = execute(patched, listener, candidate=candidate, cooldown=cooldown,
                    threshold=3, cd_init=30, plan=workload)
                assert min(seq) >= 1 and max(seq) <= 3
                first = seq.index(1)
                assert first < 200 and all(v == 1 for v in seq[first:200])
                assert (seq[259], seq[319], seq[-1]) == (2, 3, 1)
                shipping = execute(raw, listener, candidate=max(candidate, 2), cooldown=cooldown,
                    threshold=3, cd_init=30, plan=workload)
                assert min(shipping) >= 2
                transitions.append({'candidate': candidate, 'cooldown': cooldown, 'listener': listener})
    assert set(execute(no_trigger, 'noop', plan=[((8.,), 300)])) == {2}
    assert set(execute(patched, 'veto', plan=[((8.,), 300)])) == {2}
    biased = execute(patched, bias=.5, threshold=3, cd_init=30,
        plan=[((8.,), 200), ((12.,), 200)])
    assert 1 in biased[:200] and biased[-1] == 2
    for work in ((8., 40.), (40., 8.)):
        assert execute(patched, threshold=3, cd_init=30, plan=[(work, 120)])[-1] == 3
    # Patch through the real production fixture, not a test-only byte table.
    hysteresis_path = out / 'main-fps60-hysteresis.bin'
    hysteresis_env = env | {'TOTK_FPS60_HYSTERESIS': '1'}
    hysteresis_run = subprocess.run([str(fixture), str(args.image), str(hysteresis_path),
        hex(args.text_end)], env=hysteresis_env, capture_output=True, text=True, check=True)
    (out / 'hysteresis-fixture.log').write_text(hysteresis_run.stdout + hysteresis_run.stderr)
    hysteresis = hysteresis_path.read_bytes()
    hysteresis_sites = (0x2db2b8, 0x2db354, 0x2db3f0)
    for site, word in zip(hysteresis_sites, (0x528000cc, 0x5280004c, 0x52800049)):
        assert struct.unpack_from('<I', hysteresis, site)[0] == word
    native_over = execute(patched, current=1, candidate=1, threshold=2, cd_init=6,
        plan=[((18.,), 12), ((8.,), 12)])
    experiment_over = execute(hysteresis, current=1, candidate=1, threshold=2, cd_init=6,
        plan=[((18.,), 12), ((8.,), 12)])
    assert native_over[:3] == [1, 1, 2], native_over
    assert experiment_over[:7] == [1, 1, 1, 1, 1, 1, 2], experiment_over
    assert native_over[12:19] == [2, 2, 2, 2, 2, 2, 1], native_over
    assert experiment_over[12:15] == [2, 2, 1], experiment_over
    assert execute(hysteresis, current=2, candidate=2, frozen=1, plan=[((8.,), 12)]) == [2] * 12
    assert execute(hysteresis, 'veto', current=2, candidate=1, plan=[((8.,), 12)]) == [2] * 12
    assert execute(hysteresis, current=1, candidate=1, plan=[((40.,), 30)])[-1] == 3
    # Shipping-30 ignores the experiment entirely.
    shipping_path = out / 'main-shipping-hysteresis.bin'
    subprocess.run([str(fixture), str(args.image), str(shipping_path), hex(args.text_end)],
        env=hysteresis_env | {'TOTK_FPS60': '0'}, capture_output=True, text=True, check=True)
    shipping = shipping_path.read_bytes()
    for site in SITES + hysteresis_sites:
        assert shipping[site:site + 4] == raw[site:site + 4]
    # Verify every FPS site still refuses a mismatched source before patching.
    bad = out / 'invalid-main.bin'
    try:
        for site in SITES:
            image = bytearray(raw)
            image[site] ^= 1
            bad.write_bytes(image)
            rejected = subprocess.run([str(fixture), str(bad), str(out / 'rejected.bin'),
                hex(args.text_end)], env=env, capture_output=True, text=True)
            assert rejected.returncode and 'FPS patch original instruction mismatch' in rejected.stderr
        for site in hysteresis_sites:
            image = bytearray(raw)
            image[site] ^= 1
            bad.write_bytes(image)
            rejected = subprocess.run([str(fixture), str(bad), str(out / 'rejected.bin'),
                hex(args.text_end)], env=hysteresis_env, capture_output=True, text=True)
            assert rejected.returncode and 'hysteresis original instruction mismatch' in rejected.stderr
    finally:
        bad.unlink(missing_ok=True)
    report = {'status': 'pass', 'source_sha256': hashlib.sha256(raw).hexdigest(),
        'cases': cases, 'callback_recovery': [2, 1, 1], 'missing_lower_trigger_negative': '2 ticks / 30 FPS',
        'shipping_frozen_cooldown_overload_controls': 5, 'instruction_guard_controls': len(SITES),
        'persistent_transition_cases': transitions, 'bias_and_independent_work_controls': 3,
        'controller_offset_verified_by_original_caller': controller_offset,
        'stored_min_max_2_3_with_interval_1_verified': True,
        'native_constructor_defaults': {'bias': bias_a, 'one_tick_work_limit_ms': (1 - bias_a) * 1000 / 60,
            'over_threshold': over, 'cooldown': cooldown},
        'saved_x20_negative_control': 'caller sys, not live controller',
        'hysteresis': {'native_sequence': native_over, 'experiment_sequence': experiment_over,
            'guard_refusals': len(hysteresis_sites), 'shipping_frozen_veto_overload_controls': 4}}
    (out / 'fps60-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()

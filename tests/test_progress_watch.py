"""Stall watchdog (games/totk/runtime/progress_watch.h), the decisions threads.c's watchdog thread takes every 250 ms.
A small C driver replays polls through the production header; the test builds the timelines: presented frames that
stop, held stages, boot, the background, a stopped process, the dump limits and TOTK_WATCH_STALL's exit.

CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Input: "exit <ms>" (0: no TOTK_WATCH_STALL), then "<now ms> <last progress ms, 0: none> <held> <paused>" per poll.
# Output per poll: "-" nothing, "D <stalled ms>" thread dump, "X <stalled ms>" exit.
DRIVER = r'''
#include <stdio.h>
#include "progress_watch.h"
int main(void) {
    progress_watch w = {0}; unsigned long long exit_ms, now, progress; int held, paused;
    if (scanf(" exit %llu", &exit_ms) != 1) return 2;
    w.exit_ns = exit_ms * 1000000ull;
    while (scanf(" %llu %llu %d %d", &now, &progress, &held, &paused) == 4) {
        uint64_t stalled;
        unsigned a = progress_watch_poll(&w, now * 1000000ull, progress * 1000000ull, held, paused, &stalled);
        if (a == PROGRESS_WATCH_NONE) puts("-");
        else printf("%c %llu\n", a == PROGRESS_WATCH_DUMP ? 'D' : 'X', (unsigned long long)(stalled / 1000000ull));
    }
    return 0;
}
'''
POLL = 250

# The production watchdog thread in threads.c (CPU-only linkage of tests/guest_waits): mark frames for 300 ms from this
# thread as the present path does, then stop and wait 2.5 s. Also probe exact caller-sample reuse and disabled marks.
THREADS_PROBE = r'''
#include <time.h>
#include "../../runtime/threads.c"
static _Thread_local unsigned clock_reads;
uint64_t runtime_now_ns(void) { clock_reads++; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); exit(1); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
static void mark(uint64_t at, uint64_t frame, const char *stage, int held) {
    unsigned before = clock_reads;
    runtime_progress_mark(at, frame, stage, held);
    if (clock_reads != before) runtime_fail("a progress mark read the clock again");
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "sample")) {
        if (g_progress_writer.state || atomic_load(&g_progress.ns)) runtime_fail("watchdog started before progress");
        uint64_t at = runtime_now_ns(), frame = UINT64_C(0x100000007);
        mark(at, frame, "first-world pipeline warm-up", 1);
        if (g_progress_writer.state != 1 || atomic_load(&g_progress.ns) != at ||
            atomic_load(&g_progress.frame) != frame || !atomic_load(&g_progress.held) ||
            strcmp(atomic_load(&g_progress.stage), "first-world pipeline warm-up"))
            runtime_fail("held mark lost the caller's sample");
        runtime_progress_pause(1);
        mark(at + 1, frame, "host movie", 1);   /* another stage, not another graphics frame */
        mark(at + 2, frame + 3, "present", 0);
        if (atomic_load(&g_progress.ns) != at + 2 || atomic_load(&g_progress.frame) != frame + 3 ||
            atomic_load(&g_progress.held) || !atomic_load(&g_progress.paused) ||
            strcmp(atomic_load(&g_progress.stage), "present"))
            runtime_fail("progress did not reuse the caller's frame / timestamp / stage");
        return 0;
    }
    if (!strcmp(argv[1], "disabled")) {
        mark(100, 77, "loading screen", 1);
        setenv("TOTK_WATCHDOG", "1", 1);
        mark(200, 99, "present", 0);
        if (g_progress_writer.state != -1 || atomic_load(&g_progress.ns) || atomic_load(&g_progress.frame) ||
            atomic_load(&g_progress.stage) || atomic_load(&g_progress.held))
            runtime_fail("disabled marks published progress or restarted the watchdog");
        return 0;
    }
    int held = atoi(argv[1]);
    for (int i = 0; i < 20; i++) {
        const char *stage = held == 1 ? "loading screen" :
            held == 3 && i < 7 ? "first-world pipeline warm-up" : held == 3 && i < 14 ? "host movie" : "present";
        mark(runtime_now_ns(), (uint64_t)i + 1, stage, held == 1 || (held == 3 && i < 14));
        if (held == 4 && !i) setenv("TOTK_WATCHDOG", "0", 1);
        usleep(16000);
    }
    if (held == 2) runtime_progress_pause(1);
    usleep(2500000);
    fprintf(stderr, "probe end\n");
    return 0;
}
'''


class ProgressWatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='progress-watch-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                                 str(ROOT / 'games/totk/runtime'), str(source), '-o', str(cls.driver)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        game = ROOT / 'games/totk'
        cls.threads_probe = Path(cls.temp.name) / 'threads-probe'
        probe_source = Path(cls.temp.name) / 'threads_probe.c'
        probe_source.write_text(THREADS_PROBE.replace('../../runtime/threads.c', str(game / 'runtime/threads.c')))
        result = subprocess.run([*compiler, '-D_GNU_SOURCE', '-DDM_THREADS_HOST_TEST', '-std=gnu11', '-O1', '-Wall',
                                 '-Wextra', '-Werror', '-Wno-format', '-Wno-unused-parameter', '-Wno-unused-function',
                                 '-Wno-misleading-indentation', '-pthread', '-I', str(game / 'android/compat/include'),
                                 '-I', str(game / 'runtime'), '-I', str(game / 'third_party/d2r_gfx/gfx/nvn/include'),
                                 str(probe_source), str(game / 'tests/guest_waits/host_support.c'),
                                 '-o', str(cls.threads_probe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_polls(self, polls, exit_ms=0):
        """polls: (now, progress, held, paused) per poll; returns [(now, action, stalled ms)] for the non-empty ones."""
        text = f'exit {exit_ms}\n' + ''.join(f'{n} {p} {int(h)} {int(s)}\n' for n, p, h, s in polls)
        result = subprocess.run([str(self.driver)], input=text, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), len(polls))
        return [(polls[i][0], line[0], int(line[2:])) for i, line in enumerate(lines) if line != '-']

    @staticmethod
    def timeline(end_ms, frames, held=lambda t: 0, paused=lambda t: 0, start_ms=POLL):
        """Polls every 250 ms; frames(t) says whether frames are presented at time t (one each 16 ms)."""
        polls, last = [], 0
        for now in range(start_ms, end_ms + 1, POLL):
            for t in range(now - POLL + 16, now + 1, 16):
                if frames(t):
                    last = t
            polls.append((now, last, held(now), paused(now)))
        return polls

    def test_stopped_progress_gives_exactly_one_dump(self):
        actions = self.run_polls(self.timeline(120000, lambda t: t < 10000))
        self.assertEqual([a for _, a, _ in actions], ['D'])
        now, _, stalled = actions[0]
        self.assertGreaterEqual(stalled, 1000)
        self.assertLess(now, 11500)

    def test_held_stage_gives_none(self):
        # A loading stage that stops frames for 50 s: never a dump.
        stop = lambda t: t < 10000 or t > 60000
        self.assertEqual(self.run_polls(self.timeline(90000, stop, held=lambda t: 1)), [])
        # Held for longer than the 60 s ceiling: then it is a stall after all, once.
        actions = self.run_polls(self.timeline(120000, lambda t: t < 10000, held=lambda t: 1))
        self.assertEqual([(a, s >= 60000) for _, a, s in actions], [('D', True)])

    def test_boot_before_the_first_frame_gives_none(self):
        self.assertEqual(self.run_polls(self.timeline(60000, lambda t: False)), [])

    def test_background_gives_none_and_restarts_the_stall(self):
        paused = lambda t: 10000 <= t < 40000
        actions = self.run_polls(self.timeline(60000, lambda t: t < 10000, paused=paused))
        # Paused from 10 s: nothing; the stall counts again from the resume at 40 s.
        self.assertEqual([(a, now > 40000) for now, a, _ in actions], [('D', True)])
        self.assertEqual(self.run_polls(self.timeline(60000, lambda t: t < 10000 or t >= 40100, paused=paused)), [])

    def test_at_most_three_dumps_ten_seconds_apart(self):
        # 2 s stalls every 4 s: each is a stall, but dumps come only every >= 10 s and stop after three.
        frames = lambda t: t < 10000 or (t % 4000) >= 2000
        actions = self.run_polls(self.timeline(200000, frames))
        times = [now for now, _, _ in actions]
        self.assertEqual([a for _, a, _ in actions], ['D', 'D', 'D'])
        self.assertTrue(all(b - a >= 10000 for a, b in zip(times, times[1:])), times)

    def test_watch_stall_exit(self):
        actions = self.run_polls(self.timeline(30000, lambda t: t < 10000), exit_ms=5000)
        self.assertEqual([a for _, a, _ in actions][:2], ['D', 'X'])
        self.assertGreaterEqual(actions[1][2], 5000)
        # Held stages hold the exit too (up to the ceiling).
        self.assertEqual(self.run_polls(self.timeline(50000, lambda t: t < 10000, held=lambda t: 1), exit_ms=5000), [])

    def test_a_stopped_process_is_not_a_stall(self):
        # Polls up to 10 s with frames, then the whole process stops for 30 s (no poll), then frames come again.
        before = self.timeline(10000, lambda t: True)
        after = [(now, now - 100, 0, 0) for now in range(40000, 50001, POLL)]
        after[0] = (40000, 10000, 0, 0)   # the first poll after the stop: the game has not presented yet
        self.assertEqual(self.run_polls(before + after), [])

    def run_threads(self, held, **env):
        clean = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        clean.update(env)
        result = subprocess.run([str(self.threads_probe), str(held)], capture_output=True, text=True, timeout=60,
                                env=clean)
        return result.returncode, [line for line in result.stderr.splitlines() if line.startswith(('[totk-watch]', '[totk-dump] guest'))]

    def test_watchdog_thread_dumps_a_stopped_present_once(self):
        code, lines = self.run_threads(0)
        self.assertEqual(code, 0)
        self.assertEqual(len(lines), 3, lines)
        self.assertTrue(lines[0].startswith('[totk-watch] on: thread dump after >= 1 s without a presented frame'), lines)
        self.assertIn('exit after a stall: off', lines[0])
        self.assertRegex(lines[1], r'^\[totk-watch\] no presented frame for 1\.\d s at stage "present" \(frame 20\): thread dump 1/3$')
        self.assertEqual(lines[2], '[totk-dump] guest threads after 1 s')

    def test_progress_marks_reuse_caller_samples_without_extra_clocks(self):
        code, lines = self.run_threads('sample')
        self.assertEqual(code, 0, lines)
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith('[totk-watch] on:'), lines)

    def test_disabled_marks_do_not_publish_or_restart(self):
        self.assertEqual(self.run_threads('disabled', TOTK_WATCHDOG='0'),
                         (0, ['[totk-watch] off (TOTK_WATCHDOG=0)']))

    def test_watchdog_reports_latest_stage_after_held_transitions(self):
        code, lines = self.run_threads(3)
        self.assertEqual(code, 0, lines)
        self.assertEqual(len(lines), 3, lines)
        self.assertRegex(lines[1], r'at stage "present" \(frame 20\): thread dump 1/3$')
        self.assertEqual(lines[2], '[totk-dump] guest threads after 1 s')

    def test_watchdog_switch_is_latched_at_first_frame(self):
        code, lines = self.run_threads(4)
        self.assertEqual(code, 0, lines)
        self.assertEqual(len(lines), 3, lines)
        self.assertTrue(lines[0].startswith('[totk-watch] on:'), lines)
        self.assertRegex(lines[1], r'at stage "present" \(frame 20\): thread dump 1/3$')

    def test_present_supplies_existing_timestamp_and_graphics_frame(self):
        source = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c').read_text()
        self.assertIn('runtime_progress_mark(game_present_ns, g_frame + 1, "first-world pipeline warm-up", 1);', source)
        self.assertIn('runtime_progress_mark(t0, g_frame + 1,', source)
        source = (ROOT / 'games/totk/runtime/threads.c').read_text()
        mark = source.split('void runtime_progress_mark(', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('pthread_once(', mark)
        self.assertNotIn('atomic_fetch_', mark)

    def test_watchdog_thread_holds_a_loading_stage(self):
        code, lines = self.run_threads(1)
        self.assertEqual((code, len(lines)), (0, 1), lines)
        self.assertTrue(lines[0].startswith('[totk-watch] on:'), lines)

    def test_watchdog_thread_never_dumps_or_exits_while_paused(self):
        code, lines = self.run_threads(2, TOTK_WATCH_STALL='0.6')
        self.assertEqual((code, len(lines)), (0, 1), lines)
        self.assertTrue(lines[0].startswith('[totk-watch] on:'), lines)

    def test_101_lifecycle_and_single_display_focus_reach_watchdog(self):
        source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        update = source.split('static void android_game_pause_update(void) {', 1)[1].split('\n}', 1)[0]
        self.assertIn('game_activity_paused || game_focus_paused', update)
        self.assertIn('present_timeline_lifecycle(-1, paused);', update)
        lifecycle = source.split('static void present_timeline_lifecycle(int window, int paused) {', 1)[1].split('\n}', 1)[0]
        self.assertIn('!present_timeline_window || present_timeline_paused', lifecycle)
        self.assertIn('runtime_progress_pause(watch_paused);', lifecycle)
        self.assertIn('game_focus_paused = !focused && single_display;', source)
        self.assertIn('game_activity_paused = paused;\n    android_game_pause_update();', source)
        for callback in ('on_pause', 'on_stop'):
            self.assertIn(f'static void {callback}(ANativeActivity *a) {{ (void)a; android_game_lifecycle_pause(1); }}', source)
        resume = source.split('static void on_resume(ANativeActivity *a) {', 1)[1].split('\n}', 1)[0]
        self.assertIn('android_game_lifecycle_pause(0);', resume)

    def test_watchdog_switches(self):
        self.assertEqual(self.run_threads(0, TOTK_WATCHDOG='0'), (0, ['[totk-watch] off (TOTK_WATCHDOG=0)']))
        code, lines = self.run_threads(0, TOTK_WATCH_STALL='0.6')
        self.assertEqual(code, 128 + 14)   # SIGALRM
        self.assertIn('exit after a stall: 0.6 s (TOTK_WATCH_STALL)', lines[0])
        self.assertRegex(lines[-1], r'at stage "present" \(frame 20\): exiting with 142 \(TOTK_WATCH_STALL\)$')
        code, lines = self.run_threads(0, TOTK_WATCH_STALL='soon')
        self.assertEqual(code, 0)
        self.assertEqual(lines[0], '[totk-watch] TOTK_WATCH_STALL=soon ignored: seconds > 0 expected')


if __name__ == '__main__':
    unittest.main()

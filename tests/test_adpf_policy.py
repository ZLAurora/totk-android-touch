"""Compile the production ADPF state machine against an allocation-counted fake API."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AdpfPolicy(unittest.TestCase):
    def compile_run(self, source):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('A native C compiler is required')
        scratch = ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='adpf-policy-', dir=scratch) as directory:
            executable = Path(directory) / 'adpf-policy'
            flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                     for arg in shlex.split(os.environ.get(name, ''))]
            compiled = subprocess.run([
                *compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT), '-I', str(ROOT / 'tests'), '-x', 'c', '-', *flags, '-o', str(executable),
            ], input=source, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_fake_api(self):
        self.assertIn('steady-state passed',
                      self.compile_run((ROOT / 'tests/test_adpf_policy.c').read_text()))

    def test_load_hint_policy(self):
        self.compile_run(r'''
            #include "games/totk/runtime/adpf_policy.h"
            #include <assert.h>
            int main(void) {
                totk_adpf_load s = {0};
                const uint64_t target = 10000000, over = 10500001, half = 500000000;
                assert(!totk_adpf_load_step(&s, 0, over, target));
                assert(!totk_adpf_load_step(&s, 1, over, target));
                assert(totk_adpf_load_step(&s, 2, over, target));
                assert(!totk_adpf_load_step(&s, half + 1, over, target));
                assert(totk_adpf_load_step(&s, half + 2, over, target));
                /* Exact threshold, invalid frames and target reset the streak. */
                assert(!totk_adpf_load_step(&s, half + 3, 10500000, target) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 4, over, target));
                assert(!totk_adpf_load_step(&s, half + 5, 0, target) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 6, 250000000, target) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 7, over, 0) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 8, over, -1) && !s.over);
                assert(!totk_adpf_load_step(&s, half + 9, over, target));
                assert(!totk_adpf_load_step(&s, half + 10, over, target));
                assert(!totk_adpf_load_step(&s, 1, over, target)); /* reversed clock */
                assert(totk_adpf_load_step(&s, 2 * half + 2, over, target));
                assert(!totk_adpf_load_gpu(7000000, target));
                assert(totk_adpf_load_gpu(7000001, target));
                assert(!totk_adpf_load_gpu(0, target));
                assert(!totk_adpf_load_gpu(7000001, 0));
                assert(!totk_adpf_load_gpu(7000001, -1));
                /* First request at timestamp zero still rate-limits subsequent attempts. */
                s = (totk_adpf_load){.over = 2};
                assert(totk_adpf_load_step(&s, 0, over, target));
                assert(!totk_adpf_load_step(&s, half - 1, over, target));
                assert(totk_adpf_load_step(&s, half, over, target));
            }
        ''')

    def test_cpu_split_policy(self):
        self.compile_run(r'''
            #include "games/totk/runtime/adpf_policy.h"
            #include <assert.h>
            static int64_t total, cpu, gpu;
            static unsigned reports;
            static void start_set(void *w, int64_t n) { (void)w; assert(n > 0); }
            static void total_set(void *w, int64_t n) { (void)w; total = n; }
            static void cpu_set(void *w, int64_t n) { (void)w; cpu = n; }
            static void gpu_set(void *w, int64_t n) { (void)w; gpu = n; }
            static int report(void *s, void *w) { (void)s; (void)w; reports++; return 0; }
            static int legacy(void *s, int64_t n) { (void)s; total = n; reports++; return 0; }
            int main(void) {
                const uint64_t start = 1000000000, end = start + 20000000;
                /* 20ms wall, 5ms acquire/vsync pacing already removed = 15ms span.
                 * A disjoint 3ms texture queue wait removes only CPU work: 12ms, not 7ms. */
                uint64_t span = totk_adpf_span(start, end, 5000000);
                totk_adpf_pacing waits = {0};
                totk_adpf_pacing_add(&waits, start - 9000000, start - 1000000);
                totk_adpf_pacing_add(&waits, start + 1000000, start + 4000000);
                uint64_t queue = totk_adpf_pacing_in_span(&waits, start, end);
                assert(span == 15000000 && queue == 3000000);
                assert(totk_adpf_cpu(span, queue, 1) == 12000000);
                assert(totk_adpf_cpu(span, queue, 0) == span);
                assert(totk_adpf_cpu(span, span, 1) == 0);
                assert(totk_adpf_cpu(span, UINT64_MAX, 1) == 0);
                assert(totk_adpf_cpu(0, queue, 1) == 0);
                totk_adpf_state s = {.session = &s};
                totk_adpf_api api = {.report = legacy, .report2 = report, .work = &s,
                    .set_start = start_set, .set_total = total_set, .set_cpu = cpu_set, .set_gpu = gpu_set};
                assert(!totk_adpf_report_span(&s, &api, start, span, totk_adpf_cpu(span, queue, 1), 8000000));
                assert(total == 15000000 && cpu == 12000000 && gpu == 8000000 && reports == 1);
                assert(!totk_adpf_report_span(&s, &api, start, span, 0, 8000000));
                assert(total == 15000000 && cpu == 0 && gpu == 8000000 && reports == 2);
                assert(!totk_adpf_report_span(&s, &api, start, span, 0, 0) && reports == 2);
                api.report2 = 0;
                assert(!totk_adpf_report_span(&s, &api, start, span, 12000000, 8000000));
                assert(total == 15000000 && reports == 3); /* pre-35 control unchanged */
            }
        ''')

    def test_target_percentage_policy(self):
        self.compile_run(r'''
            #define _GNU_SOURCE
            #include "games/totk/runtime/adpf_policy.h"
            #include "games/totk/runtime/live_ctl.h"
            #include <assert.h>
            int main(void) {
                for (int pct = 50; pct <= 100; pct++) {
                    assert(totk_adpf_span_target(1, 0, pct) == 16666667LL * pct / 100);
                    assert(totk_adpf_span_target(0, 0, pct) == 33333333LL * pct / 100);
                    assert(totk_adpf_span_target(1, 1, pct) == 33333333LL * pct / 100);
                }
                assert(totk_adpf_span_target(1, 0, 80) == 13333333);
                assert(totk_adpf_span_target(1, 0, 90) == 15000000);
                assert(totk_adpf_span_target(0, 0, 90) == 29999999);
                assert(totk_adpf_span_target(0, 0, 100) == 33333333);
                assert(totk_adpf_span_target(1, 0, 49) == 15000000);
                assert(totk_adpf_span_target(1, 0, 101) == 15000000);
                assert(totk_adpf_span_target(1, 0, 0) == 15000000);
                int parsed;
                assert(totk_live_decimal("50", 50, 100, &parsed) && parsed == 50);
                assert(totk_live_decimal("100", 50, 100, &parsed) && parsed == 100);
                const char *bad[] = {"49", "101", "80junk", "80.0", "-80", "0x50", "", "99999999999999"};
                for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
                    assert(!totk_live_decimal(bad[i], 50, 100, &parsed));
            }
        ''')


if __name__ == '__main__':
    unittest.main()

"""totk.log -> logcat mirror (games/totk/runtime/log_mirror.h), the copy frontend_android.c runs on a thread of its own.
A small C driver runs the production header: line splitting and classification over chunked input, and the tail
thread on a real file, measuring its wakeups (the loop's own count and the kernel's voluntary context switches of the
thread) while the log is idle.

CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# feed <chunk>: stdin through log_mirror_feed in <chunk>-byte pieces; one output line per emitted line, "<E|W|I> <text>".
# tail <path> <inotify|poll>: the tail thread on <path>; prints "idle_wakeups=<n> idle_switches=<n> latency_ms=<n>".
DRIVER = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "log_mirror.h"
static void print_line(void *context, int level, const char *line) {
    (void)context; printf("%c %s\n", level == LOG_MIRROR_ERROR ? 'E' : level == LOG_MIRROR_WARN ? 'W' : 'I', line);
}
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned seen;
static void count_line(void *context, int level, const char *line) {
    (void)context; (void)level; (void)line; pthread_mutex_lock(&mu); seen++; pthread_mutex_unlock(&mu);
}
static unsigned lines_seen(void) { pthread_mutex_lock(&mu); unsigned n = seen; pthread_mutex_unlock(&mu); return n; }
static log_mirror_tail tail;
static void *run(void *arg) { (void)arg; log_mirror_run(&tail); return NULL; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static long switches(long tid) {
    char path[64], line[256]; long n = -1; snprintf(path, sizeof path, "/proc/self/task/%ld/status", tid);
    FILE *f = fopen(path, "r"); if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "voluntary_ctxt_switches: %ld", &n) == 1) break;
    fclose(f); return n;
}
static int wait_lines(unsigned n, double *ms) {
    double start = now_ms();
    while (lines_seen() < n) { if (now_ms() - start > 5000) return 0; usleep(1000); }
    *ms = now_ms() - start; return 1;
}
int main(int argc, char **argv) {
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "enabled")) {
        printf("%d\n", log_mirror_enabled(argc == 3 ? argv[2] : NULL));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "feed")) {
        size_t chunk = (size_t)atoi(argv[2]); static char data[1 << 20]; size_t n = fread(data, 1, sizeof data, stdin);
        log_mirror m = {0};
        for (size_t at = 0; at < n; at += chunk) log_mirror_feed(&m, data + at, n - at < chunk ? n - at : chunk, print_line, NULL);
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "tail")) return 2;
    FILE *log = fopen(argv[2], "w"); if (!log) return 3;
    setvbuf(log, NULL, _IONBF, 0);
    fputs("[totk] before the thread\n", log);
    tail.path = argv[2]; tail.emit = count_line;
    if (log_mirror_open(&tail)) return 4;
    if (!strcmp(argv[3], "poll") && tail.watch >= 0) { close(tail.watch); tail.watch = -1; }
    if (!strcmp(argv[3], "inotify") && tail.watch < 0) return 5;
    pthread_t thread; if (pthread_create(&thread, NULL, run, NULL)) return 6;
    double ms; if (!wait_lines(1, &ms)) return 7;
    fputs("[totk] one\n[totk] two", log); usleep(20000); fputs("\n", log);
    if (!wait_lines(3, &ms)) return 8;
    usleep(100000);   /* let the thread go back to sleep */
    long tid = __atomic_load_n(&tail.tid, __ATOMIC_ACQUIRE);
    unsigned long w0 = __atomic_load_n(&tail.wakeups, __ATOMIC_RELAXED); long s0 = switches(tid);
    usleep(1000000);
    unsigned long w1 = __atomic_load_n(&tail.wakeups, __ATOMIC_RELAXED); long s1 = switches(tid);
    fputs("[totk] after the idle second\n", log);
    if (!wait_lines(4, &ms)) return 9;
    printf("idle_wakeups=%lu idle_switches=%ld latency_ms=%.0f\n", w1 - w0, s0 < 0 || s1 < 0 ? -1 : s1 - s0, ms);
    return 0;
}
'''


class LogMirror(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not sys.platform.startswith('linux'):
            raise unittest.SkipTest('inotify and /proc task status are Linux only')
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='log-mirror-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        source = cls.directory / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = cls.directory / 'driver'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-I',
                                 str(ROOT / 'games/totk/runtime'), str(source), '-o', str(cls.driver)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def feed(self, data, chunk=1):
        result = subprocess.run([str(self.driver), 'feed', str(chunk)], input=data.encode(), capture_output=True,
                                timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.decode().splitlines()

    def test_only_explicit_one_enables_the_mirror(self):
        for value in (None, '', '0', '1', '2', '-1', 'true', '01', '1 '):
            with self.subTest(value=value):
                result = subprocess.run([str(self.driver), 'enabled', *([] if value is None else [value])],
                                        capture_output=True, text=True, check=True, timeout=30)
                self.assertEqual(result.stdout.strip(), '1' if value == '1' else '0')

    def test_frontend_gates_admission_before_opening_or_starting_the_worker(self):
        source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        start = source.split('static void log_mirror_start(void) {', 1)[1].split('\n}', 1)[0]
        gate = start.index('if (!log_mirror_enabled(value))')
        self.assertLess(gate, start.index('log_mirror_open('))
        self.assertLess(gate, start.index('pthread_create('))

    def test_classification(self):
        lines = ('[totk] FATAL guest exited\n[nvn-vk] vkQueueSubmit failed (device lost)\n[totk] CRASH report\n'
                 '[totk-app] 【红】no game in /x\n[totk-app] [RED] storage missing\n[totk] game version v143\n'
                 '[totk] fatal in lower case is information\n')
        self.assertEqual(self.feed(lines), ['E [totk] FATAL guest exited', 'E [nvn-vk] vkQueueSubmit failed (device lost)',
                                            'E [totk] CRASH report', 'W [totk-app] 【红】no game in /x',
                                            'W [totk-app] [RED] storage missing', 'I [totk] game version v143',
                                            'I [totk] fatal in lower case is information'])

    def test_complete_lines_only_whatever_the_read_size(self):
        data = 'first\r\n\nsecond line\nthird\x00part\npartial without newline'
        for chunk in (1, 3, 7, 4096):
            with self.subTest(chunk=chunk):
                self.assertEqual(self.feed(data, chunk), ['I first', 'I second line', 'I third part'])

    def test_long_lines_are_cut_at_a_character_boundary(self):
        exact = 'a' * 1000
        self.assertEqual(self.feed(exact + '\nnext\n'), ['I ' + exact, 'I next'])
        # 999 ASCII bytes, then a three-byte character that would straddle the 1000-byte bound: cut before it.
        long_line = 'b' * 999 + '红' * 10 + ' FATAL at the end\nnext\n'
        out = self.feed(long_line, 5)
        self.assertEqual(out, ['I ' + 'b' * 999 + ' [...]', 'I next'])
        out = self.feed('c' * 997 + '红红' + '\n', 2)
        self.assertEqual(out, ['I ' + 'c' * 997 + '红 [...]'])

    def test_idle_log_costs_no_wakeup(self):
        result = subprocess.run([str(self.driver), 'tail', str(self.directory / 'inotify.log'), 'inotify'],
                                capture_output=True, text=True, timeout=60)
        if result.returncode == 5:
            self.skipTest('inotify unavailable on this host')
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = dict(item.split('=') for item in result.stdout.split())
        self.assertEqual(fields['idle_wakeups'], '0', result.stdout)
        self.assertEqual(fields['idle_switches'], '0', result.stdout)
        self.assertLess(float(fields['latency_ms']), 1000, result.stdout)

    def test_polling_fallback_wakes_every_20_ms(self):
        result = subprocess.run([str(self.driver), 'tail', str(self.directory / 'poll.log'), 'poll'],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = dict(item.split('=') for item in result.stdout.split())
        self.assertGreater(int(fields['idle_wakeups']), 20, result.stdout)   # ~50 in the idle second
        self.assertLessEqual(int(fields['idle_wakeups']), 55, result.stdout)


if __name__ == '__main__':
    unittest.main()

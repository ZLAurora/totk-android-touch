"""Crash handler chaining (games/totk/runtime/crash_chain.h), the bookkeeping loader.c's crash and abort_report use.
A small C driver installs a fake "previous" handler (standing in for Android's debuggerd handler), then ours with
crash_chain_install, and crashes: the previous handler must be called exactly once after our report, with our
siginfo, and the process must then end whatever the previous handler did (returned, repaired the fault, aborted, or
was absent), and even if the first of two crashing threads never finishes its report.

CC accepts a shell-quoted compiler command; TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

# The driver prints one line per event, through write(2) so nothing is lost at the end:
#   ours <first|nested|other> <thread>          our handler's claim
#   prev <signal> code=<kernel|sent> <thread>   the fake previous handler (SA_SIGINFO), which then returns
#   prev plain <signal>                         the fake previous handler without SA_SIGINFO
#   returned <mode>                             the process went on after the crash (never expected)
# The second crasher's wait is shortened to 300 ms (CRASH_CHAIN_WAIT_NS) to keep the test fast.
DRIVER = r'''
#define _GNU_SOURCE
#define CRASH_CHAIN_WAIT_NS 300000000ll
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "crash_chain.h"
static __thread const char *who = "main";
static void say(const char *a, const char *b, const char *c, const char *d) {
    char line[160]; size_t n = 0;
    for (const char *p[] = {a, " ", b, " ", c, " ", d}, **q = p; q < p + 7; q++) { size_t k = strlen(*q); memcpy(line + n, *q, k); n += k; }
    while (n && line[n - 1] == ' ') n--;
    line[n++] = '\n';
    if (write(1, line, n) != (ssize_t)n) _exit(9);
}
static const char *name(int num) { return num == SIGSEGV ? "SEGV" : num == SIGABRT ? "ABRT" : num == SIGTRAP ? "TRAP" : "?"; }
static char *page;
static int prev_mode;   /* 0 return, 1 _exit(40), 2 repair the fault and return, 3 abort */
static void previous(int num, siginfo_t *info, void *context) {
    (void)context; say("prev", name(num), info->si_code > 0 ? "code=kernel" : "code=sent", who);
    if (prev_mode == 1) _exit(40);
    if (prev_mode == 2) mprotect(page, 4096, PROT_READ | PROT_WRITE);   /* the fault would not happen again */
    if (prev_mode == 3) abort();
}
static void previous_plain(int num) { say("prev", "plain", name(num), ""); }
static int hold_ms;
static void ours(int num, siginfo_t *info, void *context) {
    int claim = crash_chain_claim();
    say("ours", claim == CRASH_CHAIN_FIRST ? "first" : claim == CRASH_CHAIN_NESTED ? "nested" : "other", who, "");
    if (claim == CRASH_CHAIN_OTHER) crash_chain_wait(num);
    if (claim == CRASH_CHAIN_FIRST && hold_ms) usleep(hold_ms * 1000);   /* a long (or hung) report */
    crash_chain_forward(num, info, context);
}
enum { PREV_NONE, PREV_INFO, PREV_PLAIN, PREV_IGNORE };
static void install(int num, int kind) {
    struct sigaction a = {0}; sigemptyset(&a.sa_mask);
    if (kind == PREV_INFO) { a.sa_sigaction = previous; a.sa_flags = SA_SIGINFO; sigaction(num, &a, NULL); }
    if (kind == PREV_PLAIN) { a.sa_handler = previous_plain; a.sa_flags = 0; sigaction(num, &a, NULL); }
    if (kind == PREV_IGNORE) { a.sa_handler = SIG_IGN; a.sa_flags = 0; sigaction(num, &a, NULL); }
    a.sa_sigaction = ours; a.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (crash_chain_install(num, &a)) _exit(3);
}
static void fault(void) { *(volatile int *)(uintptr_t)8 = 1; }
static void *crasher(void *arg) { who = arg; fault(); return NULL; }
static void *claimer(void *arg) { (void)arg; say("claim", crash_chain_claim() == CRASH_CHAIN_OTHER ? "other" : "?", "worker", ""); return NULL; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *mode = argv[1];
    if (!strcmp(mode, "claims")) {
        say("claim", crash_chain_claim() == CRASH_CHAIN_FIRST ? "first" : "?", "main", "");
        say("claim", crash_chain_claim() == CRASH_CHAIN_NESTED ? "nested" : "?", "main", "");
        pthread_t t; pthread_create(&t, NULL, claimer, NULL); pthread_join(t, NULL);
        return 0;
    }
    if (!strcmp(mode, "fault")) { install(SIGSEGV, PREV_INFO); fault(); }
    if (!strcmp(mode, "killing")) { install(SIGSEGV, PREV_INFO); prev_mode = 1; fault(); }
    if (!strcmp(mode, "repaired")) {
        install(SIGSEGV, PREV_INFO); prev_mode = 2;
        page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (page == MAP_FAILED) return 4;
        page[0] = 1;
    }
    if (!strcmp(mode, "plain")) { install(SIGSEGV, PREV_PLAIN); fault(); }
    if (!strcmp(mode, "sent")) { install(SIGSEGV, PREV_INFO); raise(SIGSEGV); }
    if (!strcmp(mode, "abort")) { install(SIGABRT, PREV_INFO); abort(); }
    if (!strcmp(mode, "trap")) { install(SIGTRAP, PREV_INFO); raise(SIGTRAP); }
    if (!strcmp(mode, "default")) { install(SIGSEGV, PREV_NONE); fault(); }
    if (!strcmp(mode, "ignored")) { install(SIGABRT, PREV_IGNORE); raise(SIGABRT); }
    if (!strcmp(mode, "nested")) {
        install(SIGSEGV, PREV_INFO); install(SIGABRT, PREV_INFO);
        crash_chain_claim();   /* this thread is already reporting a crash */
        abort();
    }
    if (!strcmp(mode, "prev-aborts")) { install(SIGSEGV, PREV_INFO); install(SIGABRT, PREV_INFO); prev_mode = 3; fault(); }
    if (!strcmp(mode, "race") || !strcmp(mode, "hung")) {
        install(SIGSEGV, PREV_INFO); hold_ms = !strcmp(mode, "hung") ? 60000 : 100;
        pthread_t a, b; pthread_create(&a, NULL, crasher, "A"); usleep(50000); pthread_create(&b, NULL, crasher, "B");
        pthread_join(a, NULL);
    }
    say("returned", mode, "", "");   /* the crash was swallowed */
    return 1;
}
'''


class CrashChain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='crash-chain-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'driver.c'
        source.write_text(DRIVER)
        cls.driver = Path(cls.temp.name) / 'driver'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-I',
                                 str(ROOT / 'games/totk/runtime'), str(source), '-o', str(cls.driver)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def crash(self, mode):
        # No core file from the default-action cases.
        result = subprocess.run([str(self.driver), mode], capture_output=True, text=True, timeout=30, cwd=self.temp.name,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        return result.returncode, result.stdout.splitlines()

    def test_claims(self):
        self.assertEqual(self.crash('claims'), (0, ['claim first main', 'claim nested main', 'claim other worker']))

    def test_previous_handler_is_called_once_then_the_process_ends(self):
        self.assertEqual(self.crash('fault'), (-signal.SIGSEGV, ['ours first main', 'prev SEGV code=kernel main']))
        self.assertEqual(self.crash('killing'), (40, ['ours first main', 'prev SEGV code=kernel main']))
        self.assertEqual(self.crash('plain'), (-signal.SIGSEGV, ['ours first main', 'prev plain SEGV']))

    def test_a_fault_that_does_not_happen_again_still_ends_the_process(self):
        self.assertEqual(self.crash('repaired'), (-signal.SIGSEGV, ['ours first main', 'prev SEGV code=kernel main']))

    def test_sent_signals_and_aborts(self):
        self.assertEqual(self.crash('sent'), (-signal.SIGSEGV, ['ours first main', 'prev SEGV code=sent main']))
        self.assertEqual(self.crash('abort'), (-signal.SIGABRT, ['ours first main', 'prev ABRT code=sent main']))
        self.assertEqual(self.crash('trap'), (-signal.SIGTRAP, ['ours first main', 'prev TRAP code=sent main']))

    def test_default_and_ignored_previous_actions_end_the_process(self):
        self.assertEqual(self.crash('default'), (-signal.SIGSEGV, ['ours first main']))
        self.assertEqual(self.crash('ignored'), (-signal.SIGABRT, ['ours first main']))

    def test_a_crash_inside_the_report_or_the_previous_handler_goes_straight_on(self):
        self.assertEqual(self.crash('nested'), (-signal.SIGABRT, ['ours nested main', 'prev ABRT code=sent main']))
        # The previous handler aborts: our SIGABRT handler sees a nested crash and does not call a handler again.
        self.assertEqual(self.crash('prev-aborts'), (-signal.SIGABRT, ['ours first main', 'prev SEGV code=kernel main',
                                                                         'ours nested main']))

    def test_only_the_first_crashing_thread_reports(self):
        self.assertEqual(self.crash('race'), (-signal.SIGSEGV, ['ours first A', 'ours other B', 'prev SEGV code=kernel A']))

    def test_a_hung_report_does_not_keep_the_process_alive(self):
        start = time.monotonic()
        self.assertEqual(self.crash('hung'), (128 + signal.SIGSEGV, ['ours first A', 'ours other B']))
        self.assertLess(time.monotonic() - start, 10)


if __name__ == '__main__':
    unittest.main()

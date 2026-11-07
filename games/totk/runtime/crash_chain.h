/* The crash handlers' bookkeeping (loader.c crash and abort_report), as code the host test (tests/test_crash_chain.py)
   runs unchanged. Our handlers replace whatever the process had for SIGSEGV, SIGBUS, SIGILL, SIGTRAP and SIGABRT
   (Android: the debuggerd handler that writes the tombstone and the logcat backtrace; macOS: none, the crash reporter
   sees the signal's default action). That previous action is kept, and after our report the signal goes to it; the
   process then ends whatever it did, never relying on a fault happening again:
   - crash_chain_install: sigaction keeping the previous action;
   - crash_chain_claim: the first crashing thread reports; a nested crash of that thread skips its report; any other
     crashing thread waits in crash_chain_wait, at most CRASH_CHAIN_WAIT_NS (a hung report must not keep the process
     alive), then ends the process with _exit(128 + its signal);
   - crash_chain_forward: flush stdout, call the previous handler once, directly, with our siginfo and context
     (sa_sigaction, or sa_handler without SA_SIGINFO; nothing for SIG_DFL / SIG_IGN; a crash inside it skips it), then
     crash_chain_end: the default action, unblocked, sent to this thread with tgkill, and _exit(128 + signal) if even
     that returns. debuggerd's handler may be called this way: it dumps, and either kills or re-sends by itself. */
#pragma once
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#ifndef CRASH_CHAIN_WAIT_NS
#define CRASH_CHAIN_WAIT_NS 5000000000ll   /* how long another crashing thread waits for the first one's report */
#endif
enum { CRASH_CHAIN_SIGNALS = 5 };
enum { CRASH_CHAIN_FIRST, CRASH_CHAIN_NESTED, CRASH_CHAIN_OTHER };
static const int crash_chain_signal[CRASH_CHAIN_SIGNALS] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT };
static struct sigaction crash_chain_prev[CRASH_CHAIN_SIGNALS];
/* The first crashing thread (pthread_self, never 0), 0 until then. Not a __thread variable: in a dlopen'ed library
   (Android's libtotk.so) the first touch of TLS on a thread may allocate, which a crash inside malloc cannot afford. */
static uintptr_t crash_chain_owner;
static int crash_chain_forwarded;          /* the previous handler has been called (at most once per process) */

static inline int crash_chain_slot(int num) {
    for (int i = 0; i < CRASH_CHAIN_SIGNALS; i++) if (crash_chain_signal[i] == num) return i;
    return -1;
}
static inline int crash_chain_install(int num, const struct sigaction *ours) {
    int i = crash_chain_slot(num);
    return i < 0 ? -1 : sigaction(num, ours, &crash_chain_prev[i]);
}
static inline int crash_chain_claim(void) {
    uintptr_t self = (uintptr_t)pthread_self(), expected = 0;
    if (__atomic_compare_exchange_n(&crash_chain_owner, &expected, self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return CRASH_CHAIN_FIRST;
    return expected == self ? CRASH_CHAIN_NESTED : CRASH_CHAIN_OTHER;
}
/* End the process by num's default action from this thread; only async-signal-safe calls. */
__attribute__((noreturn)) static inline void crash_chain_end(int num) {
    struct sigaction d = {0}; d.sa_handler = SIG_DFL; sigemptyset(&d.sa_mask);
    sigaction(num, &d, NULL);
    sigset_t unblock; sigemptyset(&unblock); sigaddset(&unblock, num);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
#if defined(__linux__)
    syscall(SYS_tgkill, getpid(), (pid_t)syscall(SYS_gettid), num);
#else
    raise(num);   /* the calling thread */
#endif
    _exit(128 + num);
}
/* Another thread is reporting: give it CRASH_CHAIN_WAIT_NS to hand the process on, then end it ourselves. */
__attribute__((noreturn)) static inline void crash_chain_wait(int num) {
    struct timespec now, end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += (time_t)(CRASH_CHAIN_WAIT_NS / 1000000000ll);
    end.tv_nsec += (long)(CRASH_CHAIN_WAIT_NS % 1000000000ll);
    if (end.tv_nsec >= 1000000000l) { end.tv_sec++; end.tv_nsec -= 1000000000l; }
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) break;
        struct timespec step = {0, 50000000l};   /* 50 ms steps: an interrupted or stopped sleep rechecks the clock */
        nanosleep(&step, NULL);
    }
    _exit(128 + num);
}
__attribute__((noreturn)) static inline void crash_chain_forward(int num, siginfo_t *info, void *context) {
    /* stderr is unbuffered; stdout may hold a line. Never wait for a stdio lock another (stopped) thread holds. */
    if (!ftrylockfile(stdout)) { fflush(stdout); funlockfile(stdout); }
    int i = crash_chain_slot(num);
    if (i >= 0 && !__atomic_exchange_n(&crash_chain_forwarded, 1, __ATOMIC_ACQ_REL)) {
        struct sigaction prev = crash_chain_prev[i];
        if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
            if (prev.sa_flags & SA_SIGINFO) prev.sa_sigaction(num, info, context);
            else prev.sa_handler(num);
        }
    }
    crash_chain_end(num);
}

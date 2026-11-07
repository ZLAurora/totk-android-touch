/* Host probe of the thread sampler (runtime/thread_sampler.h), the production header unchanged:
 *   synthetic <dir>   a synthetic /proc/<pid>/task tree in <dir>/task: switched off it starts nothing and writes
 *                     nothing; a long frame names the three threads that ran most in its window with their run-queue
 *                     wait (a thread seen for the first time counts whole, one that ended is gone, one without
 *                     schedstat falls back to stat ticks with its wait unknown); the 300-present report; switched off
 *                     live it goes idle, switched on again it resumes
 *   real <dir>        this process's own /proc/self/task: two spinning threads fill a long frame and are named
 *   race <dir>        a frame thread toggling the switch with a stream of long frames while the sampler answers
 * Every check prints FAIL: on failure; the last line is "ok: ..." or "FAIL: ...". Build it plain and with
 * -fsanitize=thread: the concurrent sections are the race check. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "thread_sampler.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stdout, "FAIL: "); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } } while (0)

static void sleep_ms(unsigned ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; while (nanosleep(&t, &t) && errno == EINTR) {} }
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return calloc(1, 1);
    static char buffer[1 << 20]; size_t n = fread(buffer, 1, sizeof buffer - 1, f); fclose(f); buffer[n] = 0;
    return strdup(buffer);
}
static int count(const char *text, const char *needle) {
    int n = 0; for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
/* Waits up to 20 s (ThreadSanitizer is slow) for the counter to reach at least want. */
static int wait_for(_Atomic uint64_t *counter, uint64_t want) {
    for (int i = 0; i < 2000; i++) { if (atomic_load_explicit(counter, memory_order_acquire) >= want) return 1; sleep_ms(10); }
    return 0;
}
static int wait_text(const char *path, const char *needle, int times) {
    for (int i = 0; i < 2000; i++) { char *t = slurp(path); int n = count(t, needle); free(t); if (n >= times) return 1; sleep_ms(10); }
    return 0;
}
static void put(const char *dir, int tid, const char *file, const char *text) {
    char path[512]; snprintf(path, sizeof path, "%s/%d", dir, tid); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%d/%s", dir, tid, file);
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); }
    fputs(text, f); fclose(f);
}
static void thread_files(const char *dir, int tid, const char *name, unsigned long long run, unsigned long long wait) {
    char text[128]; snprintf(text, sizeof text, "%llu %llu 7\n", run, wait); put(dir, tid, "schedstat", text);
    snprintf(text, sizeof text, "%s\n", name); put(dir, tid, "comm", text);
}
static void stat_files(const char *dir, int tid, unsigned long long user, unsigned long long sys) {
    char text[256];   /* a name holding a space and a parenthesis: fields are counted after the last ')' */
    snprintf(text, sizeof text, "%d (odd) name) S 1 1 1 0 -1 4194560 10 0 0 0 %llu %llu 0 0 20 0 1 0 5 0 0\n", tid, user, sys);
    put(dir, tid, "stat", text); put(dir, tid, "comm", "odd) name\n");
}
static void gone(const char *dir, int tid) {
    char path[512];
    snprintf(path, sizeof path, "%s/%d/schedstat", dir, tid); unlink(path);
    snprintf(path, sizeof path, "%s/%d/comm", dir, tid); unlink(path);
    snprintf(path, sizeof path, "%s/%d", dir, tid); rmdir(path);
}

static int synthetic(const char *work) {
    char dir[400], log[400]; snprintf(dir, sizeof dir, "%s/task", work); snprintf(log, sizeof log, "%s/sampler.log", work);
    mkdir(dir, 0755);
    FILE *out = fopen(log, "w"); if (!out) { perror(log); return 2; }
    static totk_sampler s; s.dir = dir; s.out = out; s.long_ns = 1000000000ull;   /* 1 s: the probe's frames are sleeps */
    /* Off: nothing starts, nothing is written, and the hook is one compare. */
    uint64_t t0 = totk_sampler_now();
    for (uint64_t f = 1; f <= 1000000; f++) totk_sampler_frame(&s, f, 0);
    double off_ns = (double)(totk_sampler_now() - t0) / 1e6;
    char *text = slurp(log);
    CHECK(!atomic_load(&s.started) && !*text, "switched off, no thread starts and nothing is written");
    free(text);
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    thread_files(dir, 101, "worker-a", 1000000000ull, 100000000ull);
    thread_files(dir, 102, "worker-b", 500000000ull, 200000000ull);
    thread_files(dir, 103, "worker-c", 700000000ull, 0);
    thread_files(dir, 104, "ends", 100000000ull, 10000000ull);
    thread_files(dir, 105, "idle", 0, 0);
    stat_files(dir, 106, 100, 50);
    totk_sampler_frame(&s, 1, 1);   /* starts the sampler: its first snapshot is the periodic report's base */
    CHECK(wait_for(&s.ticks, 1), "the sampler takes its first snapshot");
    CHECK(wait_text(log, "TOTK_THREAD_SAMPLER=1: sampling 6 threads", 1), "the start line names the thread count");
    totk_sampler_frame(&s, 2, 1);   /* the long frame starts here: every snapshot so far holds the old values */
    thread_files(dir, 101, "worker-a", 1300000000ull, 105000000ull);   /* +300 ms run, +5 ms wait */
    thread_files(dir, 102, "worker-b", 600000000ull, 240000000ull);    /* +100, +40 */
    thread_files(dir, 103, "worker-c", 900000000ull, 1000000ull);      /* +200, +1 */
    gone(dir, 104);
    stat_files(dir, 106, 103, 52);                                     /* +5 ticks */
    thread_files(dir, 107, "new-thread", 150000000ull, 3000000ull);    /* first seen: counts whole */
    sleep_ms(1300);
    totk_sampler_frame(&s, 3, 1);
    CHECK(wait_for(&s.answered, 1), "the long frame is answered");
    char total[96]; snprintf(total, sizeof total, "all 6 threads run %.1f wait 49.0 ms (wait unknown for 1: no schedstat)",
                             600.0 + 150.0 + 5.0 * 1000.0 / (double)hz);
    text = slurp(log);
    const char *line = strstr(text, "[totk-threads] long frame 3 ");
    CHECK(line != NULL, "a long frame line:\n%s", text);
    if (line) {
        char one[1024]; snprintf(one, sizeof one, "%.*s", (int)strcspn(line, "\n"), line);
        CHECK(strstr(one, "window ") && strstr(one, ": worker-a(101) run 300.0 wait 5.0, worker-c(103) run 200.0 wait 1.0, "
                                                    "new-thread(107) run 150.0 wait 3.0 ms;"), "top three in order: %s", one);
        CHECK(strstr(one, total) != NULL, "totals (%s): %s", total, one);
        CHECK(!strstr(one, "no sample before"), "the window starts before the frame: %s", one);
        printf("long frame: %s\n", one);
    }
    free(text);
    /* The 300-present report covers the time since the sampler started, the same changes. */
    totk_sampler_frame(&s, 300, 1);
    CHECK(wait_for(&s.answered, 2), "the 300-present report is answered");
    text = slurp(log); line = strstr(text, "[totk-threads] frames 1-300, window ");
    CHECK(line && strstr(line, "worker-a(101) run 300.0 wait 5.0, worker-c(103) run 200.0 wait 1.0, new-thread(107)"),
          "periodic line:\n%s", text);
    free(text);
    /* A slower one: the odd stat-only name, whose wait is unknown, when it runs most. */
    stat_files(dir, 106, 203, 52);
    sleep_ms(1100); totk_sampler_frame(&s, 301, 1);
    CHECK(wait_for(&s.answered, 3), "the second long frame is answered");
    text = slurp(log); line = strstr(text, "[totk-threads] long frame 301 ");
    char odd[96]; snprintf(odd, sizeof odd, ": odd) name(106) run %.1f wait ?", 100.0 * 1000.0 / (double)hz);
    CHECK(line && strstr(line, odd), "stat fallback (%s):\n%s", odd, text);
    free(text);
    /* Live off: the sampler goes idle and answers nothing; on again: it resumes from a fresh snapshot. */
    totk_sampler_frame(&s, 302, 0);
    CHECK(wait_text(log, "TOTK_THREAD_SAMPLER=0 (live): sampler idle", 1), "switched off live, the sampler goes idle");
    uint64_t ticks = atomic_load(&s.ticks);
    for (uint64_t f = 303; f < 600; f++) totk_sampler_frame(&s, f, 0);
    sleep_ms(700);
    CHECK(atomic_load(&s.ticks) == ticks && atomic_load(&s.answered) == 3, "idle: no sampling and no answers");
    totk_sampler_frame(&s, 601, 1);
    CHECK(wait_text(log, "TOTK_THREAD_SAMPLER=1: sampling", 2), "switched on again, it resumes");
    CHECK(wait_for(&s.ticks, ticks + 1), "and samples again");
    printf("off: %.2f ns per frame hook call\n", off_ns);
    fclose(out);
    return 0;
}

static _Atomic int spin_stop;
static void *spin(void *arg) {
    pthread_setname_np(pthread_self(), (const char *)arg);
    volatile uint64_t x = 0; while (!atomic_load_explicit(&spin_stop, memory_order_relaxed)) x++;
    return NULL;
}
static void *nap(void *arg) { (void)arg; pthread_setname_np(pthread_self(), "napper"); sleep_ms(400); return NULL; }
static int real(const char *work) {
    char log[400]; snprintf(log, sizeof log, "%s/real.log", work);
    FILE *out = fopen(log, "w"); if (!out) { perror(log); return 2; }
    static totk_sampler s; s.out = out; s.long_ns = 100000000ull;
    totk_sampler_frame(&s, 1, 1);
    CHECK(wait_for(&s.ticks, 1), "the sampler takes its first snapshot");
    totk_sampler_frame(&s, 2, 1);
    pthread_t t[3];
    pthread_create(&t[0], NULL, spin, "spin-a"); pthread_create(&t[1], NULL, spin, "spin-b"); pthread_create(&t[2], NULL, nap, NULL);
    sleep_ms(400);
    totk_sampler_frame(&s, 3, 1);   /* while they still run: a thread that ended inside the window is not counted */
    CHECK(wait_for(&s.answered, 1), "the long frame is answered");
    atomic_store(&spin_stop, 1);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    char *text = slurp(log);
    const char *line = strstr(text, "[totk-threads] long frame 3 ");
    CHECK(line && strstr(text, "from schedstat"), "a long frame line from schedstat:\n%s", text);
    if (line) {
        char one[1024]; snprintf(one, sizeof one, "%.*s", (int)strcspn(line, "\n"), line);
        /* Both spinners were runnable for the whole 400 ms, so they lead and each one's run plus run-queue wait covers
           most of the window; on a loaded host one may spend most of it waiting, so run alone is not asserted. */
        const char *colon = strstr(one, ": ");
        double a = 0, b = 0, wa = 0, wb = 0; char n1[64] = "", n2[64] = "";
        CHECK(colon && sscanf(colon + 2, "%63[^(](%*d) run %lf wait %lf, %63[^(](%*d) run %lf wait %lf", n1, &a, &wa, n2, &b, &wb) == 6,
              "two leading threads: %s", one);
        printf("real: %s\n", one);
        CHECK(!strncmp(n1, "spin-", 5) && !strncmp(n2, "spin-", 5) && strcmp(n1, n2), "the spinners lead: %s", one);
        CHECK(a + wa >= 200 && b + wb >= 200, "both spinners were runnable for most of the window (%.1f+%.1f, %.1f+%.1f)", a, wa, b, wb);
        CHECK(a + b >= 100, "the spinners ran (%.1f, %.1f)", a, b);
        CHECK(!strstr(one, "wait ?"), "schedstat gives the run-queue wait: %s", one);
    }
    free(text);
    fclose(out);
    return 0;
}

static int race(const char *work) {
    char log[400]; snprintf(log, sizeof log, "%s/race.log", work);
    FILE *out = fopen(log, "w"); if (!out) { perror(log); return 2; }
    static totk_sampler s; s.out = out; s.long_ns = 1000000ull;   /* every 2 ms frame is "long" */
    uint64_t posted = 0;
    for (uint64_t f = 1; f <= 3000; f++) {
        int on = (f / 250) % 2 == 0;
        totk_sampler_frame(&s, f, on);
        posted += on && f % 250 != 0;
        sleep_ms(2);
    }
    totk_sampler_frame(&s, 3001, 1);
    sleep_ms(600);
    uint64_t answered = atomic_load(&s.answered), dropped = atomic_load(&s.dropped);
    CHECK(answered > 100, "the sampler answers while toggled (%llu answered)", (unsigned long long)answered);
    char *text = slurp(log);
    int idle = count(text, "sampler idle"), resumed = count(text, "TOTK_THREAD_SAMPLER=1: sampling");
    CHECK(idle >= 3 && resumed >= 4, "idle %d / resumed %d times", idle, resumed);
    printf("race: %llu frames posted, %llu answered, %llu dropped (not yet reported), idle %d, resumed %d\n",
           (unsigned long long)posted, (unsigned long long)answered, (unsigned long long)dropped, idle, resumed);
    free(text);
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s synthetic|real|race <dir>\n", argv[0]); return 2; }
    int r = !strcmp(argv[1], "synthetic") ? synthetic(argv[2]) : !strcmp(argv[1], "real") ? real(argv[2])
          : !strcmp(argv[1], "race") ? race(argv[2]) : 2;
    if (r) return r;
    if (failures) { printf("FAIL: thread sampler %s, %d failed\n", argv[1], failures); return 1; }
    printf("ok: thread sampler %s\n", argv[1]);
    return 0;
}

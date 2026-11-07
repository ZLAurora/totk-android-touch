/* Thread sampler (TOTK_THREAD_SAMPLER=1, default off, live through totk.ctl): which threads ran during a long frame.
   A low-priority thread reads every thread's run time and run-queue wait from <task dir>/<tid>/schedstat (ns; without
   schedstat, utime + stime from <tid>/stat in clock ticks, wait unknown) 4 times a second into a ring of snapshots. The
   present path (totk_sampler_frame, once per presented frame) only reads the clock and posts a request:
     - a frame longer than 50 ms: the sampler takes a fresh snapshot and diffs it against the newest one taken before the
       frame started, so the window holds the frame plus at most one sampling period before it;
     - every 300th frame: the window since the previous one (or since the sampler started).
   It writes one line per request to stderr (totk.log on Android: no root, no logcat needed):
     [totk-threads] long frame F 87.3 ms, window 312 ms: totk-walk(812) run 180.2 wait 12.1, ... ms; all 57 threads run
     520.0 wait 80.0 ms
   naming the three threads that ran most in the window, with their run-queue wait (time runnable but not running).
   Threads that ended inside the window are not counted; a tid seen for the first time counts all its run time.

   Off costs nothing: the thread starts at the first frame with the switch on, never before; with the switch off the
   frame hook returns after one compare and the started thread blocks on its semaphore. The sampler only reads /proc
   and keeps its own state; the present path never scans /proc, never takes a lock (sem_post only). Linux only. */
#pragma once
#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum { TOTK_SAMPLER_TIDS = 512, TOTK_SAMPLER_RING = 24, TOTK_SAMPLER_REQUESTS = 16, TOTK_SAMPLER_PERIOD = 300 };
#define TOTK_SAMPLER_TICK_NS 250000000ull   /* 4 Hz */
#define TOTK_SAMPLER_LONG_NS 50000000ull    /* the benchmarks' "over 50 ms" frames */

typedef struct { int tid; uint64_t run, wait; } totk_sampler_entry;   /* wait UINT64_MAX: unknown (no schedstat) */
typedef struct { uint64_t t; unsigned n, skipped; totk_sampler_entry e[TOTK_SAMPLER_TIDS]; } totk_sampler_snap;
typedef struct { uint64_t frame, first, start, end; int periodic; } totk_sampler_request;
typedef struct {
    /* configuration, set before the first frame */
    const char *dir;              /* NULL: /proc/self/task */
    FILE *out;                    /* NULL: stderr */
    uint64_t long_ns;             /* 0: TOTK_SAMPLER_LONG_NS */
    void (*thread_init)(void);    /* runs first on the sampler thread (name, nice, CPUs); NULL: name and nice 10 */
    /* present path only */
    int f_on, f_failed; uint64_t f_last;
    /* shared */
    _Atomic int on, started;
    sem_t wake;
    _Atomic uint64_t head, tail, dropped, answered, ticks;
    totk_sampler_request req[TOTK_SAMPLER_REQUESTS];
    /* sampler thread only */
    totk_sampler_snap *ring, *base;   /* TOTK_SAMPLER_RING snapshots, and the periodic report's base */
    unsigned nring; int have_base;
} totk_sampler;

static inline uint64_t totk_sampler_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline int totk_sampler_entry_cmp(const void *a, const void *b) {
    int x = ((const totk_sampler_entry *)a)->tid, y = ((const totk_sampler_entry *)b)->tid;
    return (x > y) - (x < y);
}
/* One thread's counters: schedstat "run wait timeslices" in ns, else utime + stime (fields 14, 15 of stat) in ticks. */
static inline int totk_sampler_read_tid(int dir, const char *tid, uint64_t *run, uint64_t *wait) {
    char path[64], text[1024]; snprintf(path, sizeof path, "%s/schedstat", tid);
    int fd = openat(dir, path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, text, sizeof text - 1); close(fd);
        unsigned long long r, w;
        if (n > 0) { text[n] = 0; if (sscanf(text, "%llu %llu", &r, &w) == 2) { *run = r; *wait = w; return 1; } }
    }
    snprintf(path, sizeof path, "%s/stat", tid);
    if ((fd = openat(dir, path, O_RDONLY | O_CLOEXEC)) < 0) return 0;
    ssize_t n = read(fd, text, sizeof text - 1); close(fd);
    if (n <= 0) return 0;
    text[n] = 0;
    const char *p = strrchr(text, ')');   /* the name may hold spaces and parentheses */
    unsigned long long user, sys;
    if (!p || sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &user, &sys) != 2) return 0;
    static long hz; if (!hz) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    *run = (user + sys) * (1000000000ull / (uint64_t)hz); *wait = UINT64_MAX;
    return 1;
}
static inline void totk_sampler_read(const char *path, totk_sampler_snap *s) {
    s->t = totk_sampler_now(); s->n = s->skipped = 0;
    DIR *d = opendir(path); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        if (s->n == TOTK_SAMPLER_TIDS) { s->skipped++; continue; }
        totk_sampler_entry *e = &s->e[s->n];
        if (totk_sampler_read_tid(dirfd(d), de->d_name, &e->run, &e->wait)) { e->tid = atoi(de->d_name); s->n++; }
    }
    closedir(d);
    qsort(s->e, s->n, sizeof *s->e, totk_sampler_entry_cmp);
}
static inline void totk_sampler_name(const char *path, int tid, char *name, size_t size) {
    char file[PATH_MAX]; snprintf(file, sizeof file, "%s/%d/comm", path, tid);
    FILE *f = fopen(file, "re"); size_t n = 0;
    if (f) { if (fgets(name, (int)size, f)) n = strcspn(name, "\n"); fclose(f); }
    if (!n) snprintf(name, size, "tid %d", tid); else name[n] = 0;
}
/* The three threads that ran most between two snapshots, and the totals; one line. */
static inline void totk_sampler_answer(totk_sampler *s, const totk_sampler_request *r, const totk_sampler_snap *a,
                                       const totk_sampler_snap *b, int history_short) {
    const char *path = s->dir ? s->dir : "/proc/self/task";
    struct { int tid; uint64_t run, wait; } top[3] = {{0}};
    uint64_t run = 0, wait = 0; unsigned unknown = 0;
    for (unsigned i = 0; i < b->n; i++) {
        const totk_sampler_entry *e = &b->e[i];
        const totk_sampler_entry *o = bsearch(e, a->e, a->n, sizeof *e, totk_sampler_entry_cmp);
        uint64_t dr = o && o->run <= e->run ? e->run - o->run : e->run, dw = UINT64_MAX;   /* new (or reused) tid */
        if (e->wait != UINT64_MAX && (!o || o->wait != UINT64_MAX)) dw = o && o->wait <= e->wait ? e->wait - o->wait : e->wait;
        run += dr; if (dw == UINT64_MAX) unknown++; else wait += dw;
        for (int k = 0; k < 3; k++) if (!top[k].tid || dr > top[k].run) {   /* insert, keeping the three largest */
            memmove(&top[k + 1], &top[k], (size_t)(2 - k) * sizeof *top);
            top[k].tid = e->tid; top[k].run = dr; top[k].wait = dw;
            break;
        }
    }
    char line[768]; int k;
    double window = (double)(b->t - a->t) / 1e6;
    if (r->periodic) k = snprintf(line, sizeof line, "[totk-threads] frames %llu-%llu, window %.0f ms:",
                                  (unsigned long long)r->first, (unsigned long long)r->frame, window);
    else k = snprintf(line, sizeof line, "[totk-threads] long frame %llu %.1f ms, window %.0f ms%s:", (unsigned long long)r->frame,
                      (double)(r->end - r->start) / 1e6, window, history_short ? " (no sample before the frame: starts after it)" : "");
    for (int i = 0; i < 3 && top[i].tid && k > 0 && (size_t)k < sizeof line; i++) {
        char name[32], w[32]; totk_sampler_name(path, top[i].tid, name, sizeof name);
        if (top[i].wait == UINT64_MAX) snprintf(w, sizeof w, "?"); else snprintf(w, sizeof w, "%.1f", (double)top[i].wait / 1e6);
        k += snprintf(line + k, sizeof line - (size_t)k, "%s %s(%d) run %.1f wait %s", i ? "," : "", name, top[i].tid,
                      (double)top[i].run / 1e6, w);
    }
    if (k > 0 && (size_t)k < sizeof line)
        k += snprintf(line + k, sizeof line - (size_t)k, " ms; all %u threads run %.1f wait %.1f ms", b->n, (double)run / 1e6, (double)wait / 1e6);
    if (k > 0 && (size_t)k < sizeof line && unknown)
        k += snprintf(line + k, sizeof line - (size_t)k, " (wait unknown for %u: no schedstat)", unknown);
    if (k > 0 && (size_t)k < sizeof line && b->skipped)
        k += snprintf(line + k, sizeof line - (size_t)k, " (%u threads over %d not read)", b->skipped, TOTK_SAMPLER_TIDS);
    uint64_t dropped = atomic_exchange_explicit(&s->dropped, 0, memory_order_relaxed);
    if (k > 0 && (size_t)k < sizeof line && dropped)
        snprintf(line + k, sizeof line - (size_t)k, "; %llu requests dropped (sampler behind)", (unsigned long long)dropped);
    FILE *out = s->out ? s->out : stderr;
    fprintf(out, "%s\n", line); fflush(out);
}
static inline void totk_sampler_serve(totk_sampler *s, const totk_sampler_snap *fresh) {
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&s->head, memory_order_acquire);
    for (; tail != head; tail++) {
        totk_sampler_request r = s->req[tail % TOTK_SAMPLER_REQUESTS];
        if (r.periodic) {
            if (s->have_base) totk_sampler_answer(s, &r, s->base, fresh, 0);
            memcpy(s->base, fresh, sizeof *fresh); s->have_base = 1;
        } else {
            /* the newest snapshot taken before the frame started, else the oldest kept */
            const totk_sampler_snap *base = NULL, *oldest = NULL;
            unsigned n = s->nring < TOTK_SAMPLER_RING ? s->nring : TOTK_SAMPLER_RING;
            for (unsigned i = 1; i <= n; i++) {
                const totk_sampler_snap *c = &s->ring[(s->nring - i) % TOTK_SAMPLER_RING];
                if (c == fresh) continue;
                oldest = c;
                if (c->t <= r.start) { base = c; break; }
            }
            if (base || oldest) totk_sampler_answer(s, &r, base ? base : oldest, fresh, !base);
        }
        atomic_fetch_add_explicit(&s->answered, 1, memory_order_release);
        atomic_store_explicit(&s->tail, tail + 1, memory_order_release);
    }
}
static inline void *totk_sampler_main(void *arg) {
    totk_sampler *s = arg; const char *path = s->dir ? s->dir : "/proc/self/task";
    FILE *out = s->out ? s->out : stderr;
    if (s->thread_init) s->thread_init();
    else { pthread_setname_np(pthread_self(), "totk-tsample"); setpriority(PRIO_PROCESS, 0, 10); }
    s->ring = calloc(TOTK_SAMPLER_RING + 1, sizeof *s->ring);
    if (!s->ring) { fprintf(out, "[totk-threads] off: no memory for the snapshots\n"); fflush(out); return NULL; }
    s->base = &s->ring[TOTK_SAMPLER_RING];
    for (int active = 0;;) {
        if (!atomic_load_explicit(&s->on, memory_order_acquire)) {
            if (active) {   /* switched off: drop what is pending and sleep until switched on again */
                active = 0;
                atomic_store_explicit(&s->tail, atomic_load_explicit(&s->head, memory_order_acquire), memory_order_release);
                fprintf(out, "[totk-threads] TOTK_THREAD_SAMPLER=0 (live): sampler idle\n"); fflush(out);
            }
            while (sem_wait(&s->wake) && errno == EINTR) {}
            continue;
        }
        if (!active) {
            active = 1; s->nring = 0; s->have_base = 0;
            atomic_store_explicit(&s->tail, atomic_load_explicit(&s->head, memory_order_acquire), memory_order_release);
        }
        totk_sampler_snap *fresh = &s->ring[s->nring % TOTK_SAMPLER_RING];
        totk_sampler_read(path, fresh); s->nring++;
        if (s->nring == 1) {
            int source = 0; char probe[64];
            if (fresh->n) { snprintf(probe, sizeof probe, "%s/%d/schedstat", path, fresh->e[0].tid); source = !access(probe, R_OK); }
            fprintf(out, "[totk-threads] TOTK_THREAD_SAMPLER=1: sampling %u threads of %s at 4 Hz from %s, nice=%d; long frames over %.0f ms and every %d presents\n",
                    fresh->n, path, source ? "schedstat (run and run-queue wait, ns)" : "stat (utime+stime ticks; wait unknown)",
                    getpriority(PRIO_PROCESS, 0), (double)(s->long_ns ? s->long_ns : TOTK_SAMPLER_LONG_NS) / 1e6, TOTK_SAMPLER_PERIOD);
            fflush(out);
        }
        if (!s->have_base) { memcpy(s->base, fresh, sizeof *fresh); s->have_base = 1; }
        totk_sampler_serve(s, fresh);
        atomic_fetch_add_explicit(&s->ticks, 1, memory_order_release);
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)TOTK_SAMPLER_TICK_NS; if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        while (sem_timedwait(&s->wake, &until) && errno == EINTR) {}
    }
}
static inline void totk_sampler_post(totk_sampler *s, const totk_sampler_request *r) {
    uint64_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&s->tail, memory_order_acquire) >= TOTK_SAMPLER_REQUESTS) {
        atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed); return;
    }
    s->req[head % TOTK_SAMPLER_REQUESTS] = *r;
    atomic_store_explicit(&s->head, head + 1, memory_order_release);
    sem_post(&s->wake);
}
/* The present path, once per presented frame, with the switch's current value. Off: one compare. */
static inline void totk_sampler_frame(totk_sampler *s, uint64_t frame, int on) {
    if (__builtin_expect(!on && !s->f_on, 1)) return;
    if (!on) {   /* the sampler logs its idle line at its next wake */
        s->f_on = 0; atomic_store_explicit(&s->on, 0, memory_order_release); return;
    }
    if (s->f_failed) return;
    uint64_t now = totk_sampler_now();
    if (!s->f_on) {
        s->f_on = 1; s->f_last = 0;
        atomic_store_explicit(&s->on, 1, memory_order_release);
        if (!atomic_load_explicit(&s->started, memory_order_relaxed)) {
            pthread_t thread; pthread_attr_t attr; int error = sem_init(&s->wake, 0, 0) ? errno : 0;
            if (!error) { pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                          error = pthread_create(&thread, &attr, totk_sampler_main, s); pthread_attr_destroy(&attr); }
            if (error) {
                FILE *out = s->out ? s->out : stderr;
                fprintf(out, "[totk-threads] off: cannot start the sampler: %s\n", strerror(error)); fflush(out);
                s->f_failed = 1; return;
            }
            atomic_store_explicit(&s->started, 1, memory_order_relaxed);
        } else sem_post(&s->wake);
    }
    if (s->f_last && now - s->f_last > (s->long_ns ? s->long_ns : TOTK_SAMPLER_LONG_NS))
        totk_sampler_post(s, &(totk_sampler_request){frame, frame, s->f_last, now, 0});
    if (frame % TOTK_SAMPLER_PERIOD == 0 && s->f_last)   /* not on the frame that switched it on: an empty window */
        totk_sampler_post(s, &(totk_sampler_request){frame, frame >= TOTK_SAMPLER_PERIOD ? frame - TOTK_SAMPLER_PERIOD + 1 : 0, 0, now, 1});
    s->f_last = now;
}
#endif

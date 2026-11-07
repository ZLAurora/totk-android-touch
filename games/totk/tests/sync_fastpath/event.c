/* MIT. Production core admission, owner release, CV park and HLE checkpoint.
 * Kernel signals are blocked to model an owner inside an unsafe host call:
 * only the outermost host->guest checkpoint can make immediate progress. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
static int observed_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#define pthread_cond_timedwait observed_timedwait
#include "../../runtime/threads.c"
#undef pthread_cond_timedwait
#include "../guest_waits/host_support.c"
static _Atomic unsigned parks, safety_expiries;
static _Atomic int release_started;
enum { EVENT_SAMPLES = 1024, EVENT_P95 = EVENT_SAMPLES * 95 / 100 };
static uint64_t dispatch_ns[EVENT_SAMPLES];
static unsigned round_index;
uint64_t runtime_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); abort(); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest entry"); }
void runtime_thread_init(void) {}
static void need(int ok, const char *s) { if (!ok) runtime_fail(s); }
static int observed_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex, const struct timespec *deadline) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    int64_t left = (deadline->tv_sec - ts.tv_sec) * 1000000000ll + deadline->tv_nsec - ts.tv_nsec;
    need(left > 10000000 && left <= 20000000, "event parks use the long safety deadline, not one millisecond");
    atomic_fetch_add(&parks, 1);
    /* Widen the check->park window while G is still held. The owner observes
     * parks without taking G, and races its release against the actual atomic
     * CV registration/unlock below. No wake may be lost in that interval. */
    while (!atomic_load(&release_started)) sched_yield();
    int rc = pthread_cond_timedwait(cv, mutex, deadline);
    if (rc == ETIMEDOUT) atomic_fetch_add(&safety_expiries, 1);
    return rc;
}
static void init(thr_t *t, unsigned id, int priority, uint64_t mask) {
    memset(t, 0, sizeof *t); t->handle = id; t->prio = priority; t->mask = mask;
    t->core = 0; t->cur_core = -1; t->started = 1; pthread_cond_init(&t->cv, NULL);
}
static void *waiter_main(void *arg) {
    thr_t *t = arg; pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&G); t->pt = pthread_self(); core_take(t);
    dispatch_ns[round_index] = now_ns() - t->core_since;
    need(t->cur_core == 0 && g_core_owner[0] == t && !t->cqueued, "event dispatch owns the reserved eligible core");
    core_put(t); pthread_mutex_unlock(&G); return NULL;
}
static void await_park(thr_t *t, unsigned before) {
    uint64_t start = now_ns();
    for (;;) {
        int ready = atomic_load(&parks) > before;
        if (ready) return;
        need(now_ns() - start < 1000000000ull, "waiter parks before release"); sched_yield();
    }
}
static int cmp_ns(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y);
}
int main(void) {
    alarm(30); setenv("TOTK_CORE_WAIT_EVENT", "1", 1); setenv("TOTK_ASYNC_PREEMPT", "1", 1); setenv("TOTK_STARVE", "0", 1);
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR2); pthread_sigmask(SIG_BLOCK, &set, NULL);
    g_cores_on = 1; g_ready_cores = 0; g_svc_profile = 0;
    need(!pthread_key_create(&g_keythr, NULL), "scheduler TLS");
    pthread_mutex_lock(&G); sync_config(); need(tk_async_on(), "production signal setup"); pthread_mutex_unlock(&G);
    need(runtime_core_wait_event, "event opt-in parsed"); setenv("TOTK_CORE_WAIT_EVENT", "0", 1); sync_config();
    need(runtime_core_wait_event, "event policy is immutable after startup");
    thr_t owner, waiter; init(&owner, 1, 48, 3); init(&waiter, 2, 30, 1); owner.pt = pthread_self();
    pthread_setspecific(g_keythr, &owner);
    for (round_index = 0; round_index < EVENT_SAMPLES; round_index++) {
        pthread_mutex_lock(&G); core_take(&owner); unsigned before = atomic_load(&parks); pthread_mutex_unlock(&G);
        atomic_store(&release_started, 0);
        pthread_t worker; need(!pthread_create(&worker, NULL, waiter_main, &waiter), "start event waiter"); await_park(&waiter, before);
        if (!round_index) {
            struct timespec delay = {0, 5000000}; nanosleep(&delay, NULL);
            need(atomic_load(&parks) == before + 1, "blocked host owner does not cause millisecond polling");
        }
        atomic_store(&release_started, 1);
        errno = EDOM; runtime_core_hle_return(); need(errno == EDOM, "HLE checkpoint preserves errno");
        need(!pthread_join(worker, NULL), "waiter completes through release event");
        need(owner.cur_core == 0 && !owner.host_resume, "opaque HLE return reacquires original core, not free core one");
        pthread_mutex_lock(&G); core_put(&owner); pthread_mutex_unlock(&G);
    }
    /* A stale, masked, inherited or protected request must never hand off. */
    pthread_mutex_lock(&G); core_take(&owner); tk_core_queue(&waiter); waiter.mask = 2;
    __atomic_store_n(&owner.preempt, 1, __ATOMIC_RELEASE); uint64_t generation = owner.core_generation; pthread_mutex_unlock(&G);
    runtime_core_hle_return(); need(owner.core_generation == generation && !owner.preempt, "HLE reentry rejects a stale mask");
    pthread_mutex_lock(&G); waiter.mask = 1; owner.tk_protect_until = UINT64_MAX;
    __atomic_store_n(&owner.preempt, 1, __ATOMIC_RELEASE); pthread_mutex_unlock(&G);
    runtime_core_hle_return(); need(owner.core_generation == generation, "HLE reentry respects protection");
    pthread_mutex_lock(&G); owner.tk_protect_until = 0;
    uint32_t word = owner.handle | WAITERS_BIT; thr_t donor; init(&donor, 3, 28, 1); donor.wkind = 1; donor.waddr = (uintptr_t)&word; g_wait = &donor;
    __atomic_store_n(&owner.preempt, 1, __ATOMIC_RELEASE); pthread_mutex_unlock(&G);
    runtime_core_hle_return(); need(owner.core_generation == generation, "HLE reentry respects inherited priority");
    pthread_mutex_lock(&G); g_wait = NULL; tk_core_unqueue(&waiter); core_put(&owner); pthread_mutex_unlock(&G);
    need(!atomic_load(&safety_expiries), "steady-state release/check-to-park race never needs a safety timeout");
    qsort(dispatch_ns, EVENT_SAMPLES, sizeof *dispatch_ns, cmp_ns);
    printf("EVENT release_to_running_us p50=%.3f p95=%.3f max=%.3f safety_retry_us=20000 safety_expiries=%u samples=%u\n",
        dispatch_ns[EVENT_SAMPLES / 2] / 1e3, dispatch_ns[EVENT_P95] / 1e3,
        dispatch_ns[EVENT_SAMPLES - 1] / 1e3, atomic_load(&safety_expiries), EVENT_SAMPLES);
    /* Linux scheduling is not realtime; assert the tail of a repeated host
     * sample, and report the maximum rather than masking scheduling outliers. */
    need(dispatch_ns[EVENT_P95] < 200000, "release-to-waiter-running p95 is below 200 us");
    need(!g_core_wait && !g_core_owner[0] && !g_core_owner[1], "event probe drains all ownership");
    pthread_cond_destroy(&owner.cv); pthread_cond_destroy(&waiter.cv); pthread_cond_destroy(&donor.cv);
    puts("PASS core-wait event: release latency, long safety park, HLE return pinning, cached switch and eligibility revalidation"); return 0;
}

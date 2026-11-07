/* Deterministic CPU-only scheduler test: use the production queue and wake
 * operations with G held. No signal handler, guest code, graphics or device. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../runtime/threads.c"

static _Atomic uint64_t clock_ns = 100;
uint64_t runtime_now_ns(void) { return atomic_load(&clock_ns); }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); exit(1); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
static void need(int ok, const char *s) { if (!ok) runtime_fail(s); }
static void init(thr_t *t, uint32_t h, int priority, uint64_t mask) {
    memset(t, 0, sizeof *t); t->handle = h; t->prio = priority; t->mask = mask;
    t->core = 0; t->cur_core = -1; t->started = 1; pthread_cond_init(&t->cv, NULL);
}
static void reset(thr_t *owner, thr_t *high, thr_t *low) {
    memset(g_core_owner, 0, sizeof g_core_owner); g_core_wait = g_wait = NULL;
    owner->cur_core = 0; owner->preempt = 0; owner->tk_protect_until = 0; g_core_owner[0] = owner;
    high->cur_core = low->cur_core = -1; high->cqueued = low->cqueued = 0;
    high->cnext = low->cnext = NULL; high->next = NULL; high->wkind = 3; g_wait = high;
}
static void *object_wait(void *arg) {
    thr_t *t = arg; pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&G); need(block(t, 3, -1) == 0, "address wake result"); pthread_mutex_unlock(&G);
    return NULL;
}
static void await_state(thr_t *t, int queued) {
    struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pthread_mutex_lock(&G); int ok = queued ? t->cqueued : t->wkind == 3; pthread_mutex_unlock(&G);
        if (ok) return;
        clock_gettime(CLOCK_MONOTONIC, &now); need(now.tv_sec - start.tv_sec < 3, "wait-state deadline"); sched_yield();
    }
}
int main(void) {
    setenv("TOTK_STARVE", "0", 1); setenv("TOTK_ASYNC_PREEMPT", "0", 1);
    g_cores_on = 1; g_tk_async = 0; g_svc_profile = 0;
    pthread_key_create(&g_keythr, NULL);
    thr_t owner, high, low; init(&owner, 0x1001, 48, 1); init(&high, 0x1002, 30, 1); init(&low, 0x1003, 52, 1);
    pthread_setspecific(g_keythr, &owner);
    pthread_mutex_lock(&G);
    reset(&owner, &high, &low); g_ready_cores = 0; tk_core_queue(&low); wake(&high, 0); core_put(&owner);
    need(low.cur_core == 0 && high.cur_core < 0, "negative control: late-ready thread loses free core");
    reset(&owner, &high, &low); g_ready_cores = 1; tk_core_queue(&low); wake(&high, 0);
    need(high.cqueued && owner.preempt && tk_core_should_yield(&owner), "ready high priority requests safe handover");
    uint64_t sequence = high.cseq; wake_queue(&high); need(high.cseq == sequence && high.cnext == &low, "no duplicate ready enqueue");
    core_put(&owner); need(high.cur_core == 0 && !high.cqueued && low.cqueued, "ready high priority wins before host schedules it");
    core_take(&high); need(g_core_owner[0] == &high && g_core_wait == &low, "core_take accepts prior reservation");
    reset(&owner, &high, &low); high.mask = 2; wake(&high, 0);
    need(high.cur_core == 1 && !owner.preempt && g_core_owner[0] == &owner, "respect masks and reserve free eligible core"); high.mask = 1;
    reset(&owner, &high, &low); high.prio = owner.prio; wake(&high, 0);
    need(!owner.preempt && !tk_core_should_yield(&owner), "equal priority does not preempt");
    reset(&owner, &high, &low); high.prio = 30; owner.tk_protect_until = 1000; wake(&high, 0);
    need(!owner.preempt && !tk_core_should_yield(&owner), "existing starvation protection preserved");
    reset(&owner, &high, &low); wake(&high, 0); high.mask = 2;
    need(!tk_core_should_yield(&owner), "stale preemption mask revalidated"); high.mask = 1;
    reset(&owner, &high, &low); high.prio = 52; wake(&high, 0);
    need(!tk_core_should_yield(&owner), "lower priority cannot preempt"); high.prio = 30;
    reset(&owner, &high, &low); low.prio = high.prio; tk_core_queue(&low); wake(&high, 0); core_put(&owner);
    need(low.cur_core == 0 && high.cqueued, "FIFO among equal priorities"); low.prio = 52;
    reset(&owner, &high, &low); g_tk_async = 1; uint32_t mutex_word = owner.handle | WAITERS_BIT;
    low.wkind = 1; low.waddr = (uintptr_t)&mutex_word; low.prio = 28; low.next = g_wait; g_wait = &low; wake(&high, 0);
    need(tk_prio(&owner) == 28 && !owner.preempt && !tk_core_should_yield(&owner), "mutex inheritance outranks ready consumer");
    g_tk_async = 0; low.prio = 52;
    reset(&owner, &high, &low); g_cores_on = 0; wake(&high, 0); need(!high.cqueued && high.cur_core < 0, "FREE_CORES control bypasses arbitration");
    g_cores_on = 1; g_ready_cores = 0; g_wait = g_core_wait = NULL; memset(g_core_owner, 0, sizeof g_core_owner);
    owner.cur_core = 0; g_core_owner[0] = &owner; high.cur_core = -1; high.cqueued = high.wkind = 0; high.profile = calloc(1, sizeof *high.profile);
    need(high.profile != NULL, "profile allocation"); high.last_svc = 0x34; high.waddr = 0x12340000;
    atomic_store(&clock_ns, 100); pthread_t worker; need(!pthread_create(&worker, NULL, object_wait, &high), "object wait pthread");
    pthread_mutex_unlock(&G); await_state(&high, 0);
    pthread_mutex_lock(&G); atomic_store(&clock_ns, 200); wake(&high, 0); atomic_store(&clock_ns, 300); pthread_mutex_unlock(&G);
    await_state(&high, 1);
    pthread_mutex_lock(&G); atomic_store(&clock_ns, 500); core_put(&owner); pthread_mutex_unlock(&G);
    pthread_join(worker, NULL);
    need(high.profile->object[SP_ADDR].n == 1 && high.profile->object[SP_ADDR].ns == 100, "object time ends at wake");
    need(high.profile->dispatch_ns == 100 && high.profile->core[SP_ADDR].ns == 200, "dispatch and virtual-core time disjoint");
    need(high.profile->edge[0].waker == owner.handle && high.profile->edge[0].addr == high.waddr, "actual signaling guest and object address");
    need(high.profile->holder[0].handle == owner.handle && high.profile->holder[0].ns == 200, "core-holder snapshot attribution");
    pthread_mutex_lock(&G); g_all = &high; high.pt = pthread_self(); high.mport = host_thread_port_self(); pthread_setspecific(g_keythr, &high); g_svc_profile = 1; pthread_mutex_unlock(&G);
    runtime_svc_profile_frame(300); need(high.profile->walking && !high.profile->object[SP_ADDR].n, "300-present walking report resets window");
    /* Short-timeout histogram: zero-timeout polls, a 4 us timed wait that times out, and the 20 us boundary. */
    pthread_mutex_lock(&G);
    need(block(&high, 3, 0) == R_TIMEOUT && high.profile->polls == 1, "zero-timeout poll counted");
    need(block(&high, 3, 4000) == R_TIMEOUT, "4 us timed wait times out");
    need(high.profile->tiny[2] == 1 && high.profile->tiny_timeouts == 1, "timed wait in the 2-5 us bin, timed out");
    need(block(&high, 3, 20000) == R_TIMEOUT && high.profile->tiny[0] + high.profile->tiny[1] + high.profile->tiny[2] +
         high.profile->tiny[3] + high.profile->tiny[4] == 1, "20 us is not a short timeout");
    pthread_mutex_unlock(&G);
    runtime_svc_profile_frame(600); need(!high.profile->polls && !high.profile->tiny[2] && !high.profile->tiny_timeouts, "report resets short-timeout counters");
    free(high.profile); pthread_cond_destroy(&owner.cv); pthread_cond_destroy(&high.cv); pthread_cond_destroy(&low.cv);
    puts("PASS guest-waits: late-wake negative control, ready reservation, masks, priority/FIFO/inheritance/protection, stale flag, FREE_CORES, exact object/dispatch/core/peer accounting");
    return 0;
}

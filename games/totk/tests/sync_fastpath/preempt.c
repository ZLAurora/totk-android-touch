/* MIT. Real SIGUSR2 delivery into a bounded synthetic guest-code region.
 * Loader PC classification and host slots are test fixtures; all admission,
 * kicks, handler parking, priority inheritance and handover are production. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define DM_THREADS_SIGNAL_TEST
#include "../../runtime/threads.c"
#define runtime_game_code_pc support_game_code_pc
#define runtime_guest_code_pc support_guest_code_pc
#include "../guest_waits/host_support.c"
#undef runtime_game_code_pc
#undef runtime_guest_code_pc
static _Thread_local struct { uintptr_t pad, host_stack; } test_slots = {0, 1};
static _Thread_local int in_game = 1;
uintptr_t runtime_test_host_slots(void) { return (uintptr_t)&test_slots; }
extern const char sync_spin_begin[], sync_spin_end[];
int runtime_guest_code_pc(uint64_t pc) { return pc >= (uintptr_t)sync_spin_begin && pc < (uintptr_t)sync_spin_end; }
int runtime_game_code_pc(uint64_t pc) { return in_game && runtime_guest_code_pc(pc); }
uint64_t runtime_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); abort(); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest entry"); }
void runtime_thread_init(void) {}
static void need(int ok, const char *s) { if (!ok) runtime_fail(s); }
static void init(thr_t *t, unsigned id, int priority, uint64_t mask) {
    memset(t, 0, sizeof *t); t->handle = id; t->prio = priority; t->mask = mask;
    t->core = 0; t->cur_core = -1; t->started = 1; pthread_cond_init(&t->cv, NULL);
}
/* Like translated guest code, this region has no host/sanitizer callouts.
 * Its only memory access is an aligned atomic stop-word load; every scheduler,
 * sender and handler operation remains instrumented by TSan. */
__attribute__((noinline)) static void spin_guest(_Atomic int *stop) {
#if defined(__x86_64__)
    __asm__ volatile(".globl sync_spin_begin\nsync_spin_begin:\n1: cmpl $0, (%0)\njnz 2f\npause\njmp 1b\n2:\n.globl sync_spin_end\nsync_spin_end:"
        :: "r"(stop) : "cc", "memory");
#elif defined(__aarch64__)
    __asm__ volatile(".globl sync_spin_begin\nsync_spin_begin:\n1: ldr w9, [%0]\ncbnz w9, 2f\nyield\nb 1b\n2:\n.globl sync_spin_end\nsync_spin_end:"
        :: "r"(stop) : "x9", "memory");
#endif
}
static void drain_signal(const sigset_t *set) { struct timespec zero = {0}; while (sigtimedwait(set, NULL, &zero) >= 0) {} }
static void deterministic(void) {
    thr_t owner, high; init(&owner, 1, 48, 1); init(&high, 2, 30, 1); owner.pt = pthread_self();
    pthread_setspecific(g_keythr, &owner);
    sigset_t set, old; sigemptyset(&set); sigaddset(&set, SIGUSR2); pthread_sigmask(SIG_BLOCK, &set, &old);
    pthread_mutex_lock(&G); core_take(&owner); tk_core_queue(&high);
    uint64_t sent = g_tk_kicks, epoch = owner.core_generation;
    for (unsigned i = 0; i < 16; i++) need(tk_kick_owner(&owner, 1000000000ull), "eligible kick uses bounded retry");
    need(g_tk_kicks - sent == (g_sync_kick ? 1u : 16u), "same-owner contenders share one pending kick");
    need(tk_kick_owner(&owner, 1001000000ull) && g_tk_kicks - sent == (g_sync_kick ? 2u : 17u), "unacknowledged kick retries at one millisecond");
    owner.kick_safe = 1; sent = g_tk_kicks; tk_kick_owner(&owner, 1002000000ull);
    need(g_tk_kicks - sent == (g_sync_kick ? 0u : 1u), "committed safe point coalesces request"); owner.kick_safe = 0;
    tk_core_unqueue(&high); core_put(&owner); core_take(&owner);
    need(owner.core_generation == epoch + 1 && !owner.kick_pending && !owner.kick_last_ns, "ownership change invalidates pending request and retry clock");
    sent = g_tk_kicks; tk_kick_owner(&owner, 1002000001ull); need(g_tk_kicks == sent + 1, "new generation kicks immediately");
    pthread_mutex_unlock(&G); drain_signal(&set);
    ucontext_t context; memset(&context, 0, sizeof context);
#if defined(__x86_64__)
    context.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)sync_spin_begin;
#elif defined(__aarch64__)
    context.uc_mcontext.pc = (uintptr_t)sync_spin_begin;
#else
#error signal fixture requires Linux x86_64 or aarch64
#endif
    test_slots.host_stack = 0; tk_preempt_signal(SIGUSR2, NULL, &context); test_slots.host_stack = 1;
    need(owner.sig_refused && !owner.kick_pending, "unsafe host-context refusal acknowledges but does not park");
    pthread_mutex_lock(&G); tk_core_queue(&high); high.tk_wait_since = 0;
    uint64_t refused = g_tk_refused; pthread_mutex_unlock(&G);
    in_game = 0; tk_preempt_signal(SIGUSR2, NULL, &context); in_game = 1;
    need(g_tk_refused == refused + 1 && owner.cur_core == 0 && !owner.preempt, "safe SDK priority refusal is counted without a handover");
    pthread_mutex_lock(&G); owner.tk_protect_until = UINT64_MAX;
    __atomic_store_n(&owner.preempt, 1, __ATOMIC_RELEASE); refused = g_tk_refused; pthread_mutex_unlock(&G);
    tk_preempt_signal(SIGUSR2, NULL, &context);
    need(g_tk_refused == refused && owner.cur_core == 0 && !owner.preempt, "protection-window rejection is not a refusal counter entry");
    pthread_mutex_lock(&G); owner.tk_protect_until = 0; tk_core_unqueue(&high);
    __atomic_store_n(&owner.preempt, 1, __ATOMIC_RELEASE); pthread_mutex_unlock(&G);
    pthread_mutex_lock(&G); owner.core_since = now_ns() - TK_STARVE_NS; tk_core_queue(&high); high.mask = 2;
    uint64_t before = owner.core_since; pthread_mutex_unlock(&G);
    runtime_core_checkpoint();
    need(!owner.preempt && ((g_sync_kick || runtime_core_wait_event) ? owner.core_since == before : owner.core_since != before), "stale checkpoint preserves validated paths and reproduces old handover when disabled");
    pthread_mutex_lock(&G); tk_core_unqueue(&high);
    high.mask = 1; tk_core_queue(&high); owner.tk_protect_until = now_ns() + TK_PROTECT_NS;
    need(!tk_core_should_yield(&owner), "protected owner cannot be preempted"); owner.tk_protect_until = 0;
    uint32_t mutex_word = owner.handle | WAITERS_BIT; thr_t donor; init(&donor, 3, 28, 1);
    donor.wkind = 1; donor.waddr = (uintptr_t)&mutex_word; g_wait = &donor;
    need(tk_prio(&owner) == 28 && !tk_core_should_yield(&owner), "inherited priority blocks a nominally better waiter");
    g_wait = NULL; tk_core_unqueue(&high); core_put(&owner); pthread_mutex_unlock(&G);
    drain_signal(&set); pthread_sigmask(SIG_SETMASK, &old, NULL); pthread_setspecific(g_keythr, NULL);
    pthread_cond_destroy(&owner.cv); pthread_cond_destroy(&high.cv); pthread_cond_destroy(&donor.cv);
}
static _Atomic unsigned ready, completed;
static _Atomic int stop;
static unsigned load_rounds;
static int sdk_mode, inject_signals;
static void *owner_main(void *arg) {
    thr_t *t = arg; pthread_setspecific(g_keythr, t); in_game = !sdk_mode;
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR2);
    if (inject_signals) pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_mutex_lock(&G); t->pt = pthread_self(); core_take(t); pthread_mutex_unlock(&G);
    atomic_fetch_add(&ready, 1);
    if (!inject_signals) spin_guest(&stop);
    else {
        /* TSan defers real async callbacks until an interceptor boundary,
         * which cannot model a no-SVC translated guest spin. Keep kernel
         * sends real but blocked here; synchronously enter the fully
         * instrumented production handler only at this lock-free safe point. */
        ucontext_t context; memset(&context, 0, sizeof context);
#if defined(__x86_64__)
        context.uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)sync_spin_begin;
#elif defined(__aarch64__)
        context.uc_mcontext.pc = (uintptr_t)sync_spin_begin;
#endif
        while (!atomic_load(&stop)) {
            if (__atomic_load_n(&t->preempt, __ATOMIC_ACQUIRE)) tk_preempt_signal(SIGUSR2, NULL, &context);
            else sched_yield();
        }
        drain_signal(&set);
    }
    pthread_mutex_lock(&G); core_put(t); pthread_mutex_unlock(&G); return NULL;
}
static void *contender_main(void *arg) {
    thr_t *t = arg; pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&G); t->pt = pthread_self();
    for (unsigned i = 0; i < load_rounds; i++) {
        core_take(t); need(g_core_owner[t->cur_core] == t && core_allowed(t, t->cur_core), "load owns eligible exclusive core");
        atomic_fetch_add(&completed, 1); core_put(t);
        pthread_mutex_unlock(&G); sched_yield(); pthread_mutex_lock(&G);
    }
    pthread_mutex_unlock(&G); return NULL;
}
static void load(int sdk, unsigned rounds) {
    thr_t owners[3], contenders[12]; pthread_t op[3], cp[12];
    atomic_store(&ready, 0); atomic_store(&completed, 0); atomic_store(&stop, 0); sdk_mode = sdk; load_rounds = rounds;
    uint64_t begin = now_ns(), preempts = g_tk_preempts, refused = g_tk_refused, starved = g_tk_starve_grants;
    for (unsigned i = 0; i < 3; i++) { init(&owners[i], 100 + i, 50, 1ull << i); need(!pthread_create(&op[i], NULL, owner_main, &owners[i]), "spinning owner"); }
    while (atomic_load(&ready) != 3) { need(now_ns() - begin < 3000000000ull, "owners start deadline"); sched_yield(); }
    for (unsigned i = 0; i < 12; i++) { init(&contenders[i], 200 + i, 30 + i % 4, i % 4 ? 1ull << (i % 3) : 7); need(!pthread_create(&cp[i], NULL, contender_main, &contenders[i]), "load contender"); }
    for (unsigned i = 0; i < 12; i++) need(!pthread_join(cp[i], NULL), "load contender join");
    atomic_store(&stop, 1); for (unsigned i = 0; i < 3; i++) need(!pthread_join(op[i], NULL), "spinning owner join");
    need(atomic_load(&completed) == rounds * 12 && g_tk_preempts > preempts, "no-SVC load completes through production signal-handler handover");
    if (sdk) need(g_tk_starve_grants > starved, "SDK spin retains starvation escape");
    for (unsigned i = 0; i < 3; i++) { need(!owners[i].kick_safe && !owners[i].kick_pending, "handler reentry drains owner state"); pthread_cond_destroy(&owners[i].cv); }
    for (unsigned i = 0; i < 12; i++) pthread_cond_destroy(&contenders[i].cv);
    need(!g_core_wait && !g_core_owner[0] && !g_core_owner[1] && !g_core_owner[2], "preemption load drains three cores");
    printf("PREEMPT sdk=%d injected=%d completed=%u wall_ms=%.3f handoffs=%llu refused=%llu starvation=%llu\n", sdk, inject_signals, atomic_load(&completed), (now_ns() - begin) / 1e6,
        (unsigned long long)(g_tk_preempts - preempts), (unsigned long long)(g_tk_refused - refused), (unsigned long long)(g_tk_starve_grants - starved));
}
int main(int argc, char **argv) {
    alarm(120); unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1000;
    const char *inject = getenv("SYNC_SIGNAL_INJECT"); inject_signals = inject && !strcmp(inject, "1");
    setenv("TOTK_ASYNC_PREEMPT", "1", 1); setenv("TOTK_STARVE", "1", 1);
    g_cores_on = 1; g_ready_cores = 0; g_svc_profile = 0; runtime_hle_tsd_off = sizeof(uintptr_t);
    need(!pthread_key_create(&g_keythr, NULL), "scheduler TLS"); pthread_mutex_lock(&G); sync_config(); need(tk_async_on(), "install production handler"); pthread_mutex_unlock(&G);
    deterministic(); load(0, rounds); load(1, rounds);
    printf("COUNTS kick=%d issued=%llu block=%llu core=%llu sent=%llu coalesced=%llu ack=%llu refused=%llu\n", g_sync_kick,
        (unsigned long long)g_sync_counts.wake_issued, (unsigned long long)g_sync_counts.block_parks,
        (unsigned long long)g_sync_counts.core_parks, (unsigned long long)g_sync_counts.kick_sent, (unsigned long long)g_sync_counts.kick_coalesced,
        (unsigned long long)g_sync_ack, (unsigned long long)g_sync_refused);
    sync_stats_frame(300); puts("PASS preemption: owner generations, bounded retry, safe-context refusal, stale flags, inheritance, protection, three-core guest/SDK spins, handler reentry"); return 0;
}

/* MIT. Standalone real nvn_dirty.c: real signal hooks, SA_ONSTACK and shared
 * physical backing. No substituted permission calls or attribution model. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define TOTK_VK_TESTS 1
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
#include "backing.h"
#include "test_guest_store.h"
#include <signal.h>

enum { TEST_PAGES = 64, ALT_BYTES = 128 * 1024 };
static int (*fault_hook)(void *, int, void *);
static void (*prep_hook)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fault_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { prep_hook = fn; }
static _Atomic unsigned signal_entries, signal_exits, busy_entries, off_stack;
static _Thread_local uintptr_t alt_lo, alt_hi;
static void alt_install(void) {
    void *memory = mmap(NULL, ALT_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    required(memory != MAP_FAILED, "alternate signal stack mmap");
    alt_lo = (uintptr_t)memory; alt_hi = alt_lo + ALT_BYTES;
    stack_t stack = {.ss_sp = memory, .ss_size = ALT_BYTES};
    required(!sigaltstack(&stack, NULL), "install alternate signal stack");
}
static void alt_remove(void) {
    stack_t stack = {.ss_flags = SS_DISABLE}; required(!sigaltstack(&stack, NULL), "disable alternate stack");
    required(!munmap((void *)alt_lo, ALT_BYTES), "unmap alternate stack"); alt_lo = alt_hi = 0;
}
static void signal_handler(int signal_number, siginfo_t *info, void *context) {
    char local;
    if ((uintptr_t)&local < alt_lo || (uintptr_t)&local >= alt_hi) atomic_fetch_add(&off_stack, 1);
    range_t *range = find((uintptr_t)info->si_addr);
    if (range && atomic_load(&range->state[((uintptr_t)info->si_addr - range->base) >> g_shift]) == PG_BUSY)
        atomic_fetch_add_explicit(&busy_entries, 1, memory_order_release);
    atomic_fetch_add_explicit(&signal_entries, 1, memory_order_release);
    if (signal_number != SIGSEGV || !fault_hook || !fault_hook(info->si_addr, 1, context)) _exit(90);
    atomic_fetch_add_explicit(&signal_exits, 1, memory_order_release);
}
static void signal_install(void) {
    alt_install();
    struct sigaction action = {.sa_sigaction = signal_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask); required(!sigaction(SIGSEGV, &action, NULL), "install SIGSEGV handler");
}
static void await_unsigned(_Atomic unsigned *value, unsigned minimum, const char *message) {
    uint64_t deadline = mach_absolute_time() + 5000000000ull;
    while (atomic_load_explicit(value, memory_order_acquire) < minimum) {
        if (mach_absolute_time() > deadline) { fprintf(stderr, "timeout: %s\n", message); exit(1); }
        sched_yield();
    }
}
static void await_int(_Atomic int *value, const char *message) {
    uint64_t deadline = mach_absolute_time() + 5000000000ull;
    while (!atomic_load_explicit(value, memory_order_acquire)) {
        if (mach_absolute_time() > deadline) { fprintf(stderr, "timeout: %s\n", message); exit(1); }
        sched_yield();
    }
}
static int alias_on;
static void *begin_write(void *guest, size_t size) {
    return alias_on ? nvn_dirty_gpu_write_alias_begin(guest, size) : nvn_dirty_gpu_write_begin(guest, size);
}
static int try_write(void *guest, size_t size, void **token) {
    return alias_on ? nvn_dirty_gpu_write_alias_try_begin(guest, size, token) : nvn_dirty_gpu_write_try_begin(guest, size, token);
}
static uint8_t *write_pointer(backing *b, size_t offset) { return (alias_on ? b->alias : b->guest) + offset; }
typedef struct {
    uint8_t state[TEST_PAGES], exact[TEST_PAGES];
    uint64_t gen[TEST_PAGES], gates[TEST_PAGES], latest, now;
} dirty_snapshot;
static void snapshot(backing *b, dirty_snapshot *s) {
    memset(s, 0, sizeof *s); range_t *r = find((uintptr_t)b->guest);
    required(r != NULL, "find real dirty range");
    for (unsigned p = 0; p < TEST_PAGES; ++p) {
        s->state[p] = atomic_load(&r->state[p]); s->exact[p] = atomic_load(&r->exact[p]); s->gen[p] = atomic_load(&r->gen[p]);
    }
    if (r->lag) for (size_t u = 0; u < (TEST_PAGES + g_unit - 1) / g_unit; ++u) s->gates[u] = atomic_load(&r->lag[u].gate);
    s->latest = atomic_load(&r->latest); s->now = nvn_dirty_now();
}
static void unchanged(backing *b, const dirty_snapshot *before, const char *message) {
    dirty_snapshot after; snapshot(b, &after); CHECK(!memcmp(before, &after, sizeof after), message);
}
static uint64_t hash_bytes(const uint8_t *p, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static uint64_t landing(backing *b) {
    uint8_t *expected = malloc(b->bytes); required(expected != NULL, "expected contents");
    for (size_t i = 0; i < b->bytes; ++i) b->guest[i] = (uint8_t)(i * 29 + i / g_pg);
    memcpy(expected, b->guest, b->bytes);
    uint64_t stamp = nvn_dirty_now(); void *watch = nvn_dirty_wb_watch(b->guest, b->bytes, stamp);
    CHECK(watch && !nvn_dirty_wb_written(watch), "real storage-wide WB watch arms");
    dirty_snapshot before; snapshot(b, &before);
    uint64_t rows = guest_rows(b), calls = atomic_load(&g_mp_calls);
    for (unsigned n = 1; n < 17; n += 2) {
        size_t offset = n * g_pg + 17, size = g_pg - 34;
        void *token = NULL; CHECK(try_write(b->guest + offset, size, &token) && token, "landing obtains actual try token");
        range_t *r = find((uintptr_t)b->guest);
        CHECK(atomic_load(&r->state[n]) == PG_BUSY, "landing owns covered guest page BUSY");
        CHECK(guest_permissions(b->guest + offset) == (alias_on ? PROT_READ : PROT_READ | PROT_WRITE), "kernel guest permissions match selected store path");
        memset(write_pointer(b, offset), (int)(0x60 + n), size); memset(expected + offset, (int)(0x60 + n), size);
        nvn_dirty_gpu_write_end(token);
        unchanged(b, &before, "GPU landing preserves exact generations, watch bits, states and gates");
        CHECK(!nvn_dirty_wb_written(watch) && !nvn_dirty_wb_cpu_address(watch), "GPU landing leaves WB ownership armed without CPU provenance");
    }
    uint64_t after_rows = guest_rows(b), after_calls = atomic_load(&g_mp_calls);
    CHECK(!memcmp(expected, b->guest, b->bytes) && !memcmp(expected, b->alias, b->bytes), "every byte including landing prefix/suffix is exact through both mappings");
    CHECK(!nvn_dirty_since(b->guest, b->bytes, stamp), "GPU landing is not a CPU dirty generation");
    CHECK(alias_on ? after_calls == calls : after_calls > calls, "alias makes zero guest protection syscalls; legacy makes real calls");
    if (b->gem) CHECK(alias_on ? after_rows == rows : after_rows > rows, "actual nonmerging GEM: alias creates zero guest rows; legacy creates rows");
    printf("LANDING backing=%s alias=%d rows_before=%" PRIu64 " rows_after=%" PRIu64 " new_guest_rows=%" PRIu64 " protection_calls=%" PRIu64 "\n",
           b->gem ? "i915" : "memfd", alias_on, rows, after_rows, after_rows - rows, after_calls - calls);
    uint64_t digest = hash_bytes(b->guest, b->bytes); free(expected); nvn_dirty_wb_unwatch(watch); return digest;
}
static void mixed_and_unwind(backing *b) {
    nvn_dirty_remove_range(b->guest); nvn_dirty_add_range(b->guest, b->bytes);
    nvn_dirty_protect_exact(b->guest, g_pg); nvn_dirty_protect_exact(b->guest + 2 * g_pg, g_pg);
    dirty_snapshot before; snapshot(b, &before);
    void *token = begin_write(b->guest + 13, 3 * g_pg - 26); CHECK(token != NULL, "mixed protected/writable pages acquire token");
    memset(write_pointer(b, 13), 0x53, 3 * g_pg - 26); nvn_dirty_gpu_write_end(token);
    unchanged(b, &before, "mixed token restores original NONE and PROT without inventing protection");
    void *owner = begin_write(b->guest + 2 * g_pg, g_pg), *failed = (void *)1;
    CHECK(owner != NULL, "real competing owner claims later page");
    CHECK(!try_write(b->guest, 3 * g_pg, &failed) && !failed, "failed page try clears token and unwinds earlier page/gate claims");
    range_t *r = find((uintptr_t)b->guest);
    CHECK(atomic_load(&r->state[0]) == PG_PROT && atomic_load(&r->state[2]) == PG_BUSY, "try unwind restores own earlier page, never another owner's BUSY page");
    nvn_dirty_gpu_write_end(owner); unchanged(b, &before, "failed try and competing end leave exact original dirty state");
    if (r->lag && b->bytes > g_unit * g_pg) {
        owner = begin_write(b->guest + g_unit * g_pg, g_pg); CHECK(owner != NULL, "later unit held by real token");
        uint64_t gate = atomic_load(&r->lag[0].gate);
        failed = (void *)1; CHECK(!try_write(b->guest, (g_unit + 1) * g_pg, &failed) && !failed, "failed multi-unit try rejects another BUSY gate");
        CHECK(atomic_load(&r->lag[0].gate) == gate, "failed multi-unit try releases earlier successfully claimed unit");
        nvn_dirty_gpu_write_end(owner); unchanged(b, &before, "multi-unit failure preserves states/generations/gates");
    }
    puts("UNWIND PASS mixed-pages page-owner unit-owner");
}
typedef struct { backing *b; uint8_t *guest; _Atomic int done; } guest_writer;
static void *guest_writer_run(void *arg) {
    guest_writer *w = arg; alt_install();
    lag_guest_store(w->guest, 0xd7); memset(w->guest, 0xd7, 64);
    atomic_store_explicit(&w->done, 1, memory_order_release); alt_remove(); return NULL;
}
typedef struct { backing *b; uint8_t *guest; _Atomic int acquired, finish; } landing_worker;
static void *landing_worker_run(void *arg) {
    landing_worker *w = arg; void *token = begin_write(w->guest, g_pg); required(token != NULL, "worker begin token");
    atomic_store_explicit(&w->acquired, 1, memory_order_release); await_int(&w->finish, "landing release");
    size_t offset = (size_t)(w->guest - w->b->guest); memset(write_pointer(w->b, offset), 0xa6, 64);
    nvn_dirty_gpu_write_end(token); return NULL;
}
static void concurrent_fault(backing *b) {
    nvn_dirty_remove_range(b->guest); nvn_dirty_add_range(b->guest, b->bytes);
    uint64_t stamp = nvn_dirty_now(); void *watch = nvn_dirty_wb_watch(b->guest, b->bytes, stamp);
    CHECK(watch && !nvn_dirty_wb_written(watch), "race arms real WB watch");
    uint8_t *address = b->guest + 7 * g_pg;
    nvn_dirty_test_wb_handoff handoff = {.addr = address, .phase = NVN_DIRTY_TEST_WB_PROTECTED};
    /* Legacy must catch its writer before opening RW. Alias mode deliberately
       launches the guest only AFTER begin returns, inside the live store token. */
    nvn_dirty_test_wb_set(alias_on ? NULL : &handoff);
    landing_worker landing = {.b = b, .guest = address}; pthread_t gpu, cpu;
    unsigned entries = atomic_load(&signal_entries), exits = atomic_load(&signal_exits), busy = atomic_load(&busy_entries);
    required(!pthread_create(&gpu, NULL, landing_worker_run, &landing), "create landing worker");
    if (alias_on) await_int(&landing.acquired, "returned alias token remains BUSY");
    else await_int(&handoff.arrived, "production PROTECTED token handoff");
    guest_writer writer = {.b = b, .guest = address}; required(!pthread_create(&cpu, NULL, guest_writer_run, &writer), "create guest writer");
    await_unsigned(&signal_entries, entries + 1, "actual guest SIGSEGV during PG_BUSY");
    CHECK(atomic_load(&busy_entries) == busy + 1 && !atomic_load(&writer.done), "real guest machine store faults on PG_BUSY and has not completed");
    CHECK(atomic_load(&signal_exits) == exits, "actual fault handler remains inside ownership wait");
    if (!alias_on) atomic_store_explicit(&handoff.release, 1, memory_order_release);
    await_int(&landing.acquired, "landing acquires token");
    CHECK(!atomic_load(&writer.done) && atomic_load(&signal_exits) == exits, "faulting guest cannot pass the live token, including legacy RW interval");
    if (alias_on) CHECK(guest_permissions(address) == PROT_READ, "alias token keeps the real guest mapping read-only while held");
    atomic_store_explicit(&landing.finish, 1, memory_order_release);
    required(!pthread_join(gpu, NULL) && !pthread_join(cpu, NULL), "join actual concurrent writers"); nvn_dirty_test_wb_set(NULL);
    CHECK(atomic_load(&writer.done) && atomic_load(&signal_exits) > exits, "guest fault returns only after token end");
    CHECK(nvn_dirty_wb_written(watch) && nvn_dirty_wb_cpu_address(watch) == (uintptr_t)address, "guest takeover cancels whole WB watch and retains precise CPU address");
    CHECK(nvn_dirty_since(b->guest, b->bytes, stamp), "waited guest store advances CPU generation");
    int exact = 1; for (unsigned i = 0; i < 64; ++i) exact &= address[i] == 0xd7;
    CHECK(exact && !atomic_load(&off_stack), "guest contents win after landing; all real faults used alternate stacks");
    nvn_dirty_wb_unwatch(watch); puts("CONCURRENT PASS SIGSEGV PG_BUSY token-end CPU-generation WB-cancel SA_ONSTACK");
}
static void ticket_wait(backing *b) {
    if (!nvn_dirty_lag_enabled()) return;
    nvn_dirty_remove_range(b->guest); nvn_dirty_add_range(b->guest, b->bytes);
    uint8_t *address = b->guest + g_unit * g_pg;
    uint64_t ticket = atomic_load(&g_lag_done) + 1;
    CHECK(nvn_dirty_lag_protect(address, g_pg, ticket), "real read ticket registers"); nvn_dirty_lag_submitted(ticket);
    dirty_snapshot before; snapshot(b, &before);
    void *token = (void *)1; CHECK(!try_write(b->guest, (g_unit + 1) * g_pg, &token) && !token, "try honors outstanding GPU ticket");
    unchanged(b, &before, "ticket rejection unwinds earlier unit exactly");
    uint64_t faults, hosts, ns, start_hosts; nvn_dirty_lag_stats(&faults, &start_hosts, &ns);
    landing_worker worker = {.b = b, .guest = address}; pthread_t thread;
    required(!pthread_create(&thread, NULL, landing_worker_run, &worker), "create ticket waiter");
    uint64_t deadline = mach_absolute_time() + 5000000000ull;
    do { nvn_dirty_lag_stats(&faults, &hosts, &ns); required(mach_absolute_time() < deadline, "ticket wait timeout"); sched_yield(); } while (hosts == start_hosts);
    CHECK(!atomic_load(&worker.acquired), "blocking GPU write actually waits for incomplete ticket");
    nvn_dirty_lag_complete(ticket); await_int(&worker.acquired, "ticket completion permits token");
    atomic_store_explicit(&worker.finish, 1, memory_order_release); required(!pthread_join(thread, NULL), "join ticket waiter");
    unchanged(b, &before, "ticket wait and landing preserve dirty ownership"); puts("TICKETS PASS rejected-try blocking-wait explicit-complete");
}
typedef struct { backing *b; size_t offset; uint32_t value; int result; _Atomic int done; } completion_worker;
static void *completion_worker_run(void *argument) {
    completion_worker *w = argument;
    w->result = nvn_dirty_lag_store_alias(w->b->guest + w->offset, w->b->alias + w->offset,
                                          &w->value, sizeof w->value, 0);
    atomic_store_explicit(&w->done, 1, memory_order_release); return NULL;
}
static void alias_completion_store(backing *b) {
    if (!alias_on) return;
    nvn_dirty_remove_range(b->guest); nvn_dirty_add_range(b->guest, b->bytes);
    void *watch = nvn_dirty_wb_watch(b->guest, b->bytes, nvn_dirty_now()); dirty_snapshot before; snapshot(b, &before);
    uint8_t data[97]; for (unsigned i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(i * 7);
    uint64_t calls = atomic_load(&g_mp_calls); size_t offset = g_unit * g_pg - 31;
    CHECK(nvn_dirty_lag_store_alias(b->guest + offset, b->alias + offset, data, sizeof data, 0), "alias completion store crosses pages/units");
    CHECK(!memcmp(b->guest + offset, data, sizeof data), "completion contents visible at exact guest offset");
    uint32_t value = 0x12345678, add = 19, got;
    CHECK(nvn_dirty_lag_store_alias(b->guest + 128, b->alias + 128, &value, sizeof value, 0), "alias completion writes 32-bit report");
    CHECK(nvn_dirty_lag_store_alias(b->guest + 128, b->alias + 128, &add, sizeof add, 1), "alias completion adds 32-bit report");
    memcpy(&got, b->guest + 128, sizeof got); CHECK(got == value + add, "add32 completion computes exact report value");
    unchanged(b, &before, "completion stores preserve page states, generations, watch bits and gates");
    CHECK(!nvn_dirty_wb_written(watch) && atomic_load(&g_mp_calls) == calls, "completion stores preserve WB watch with zero guest mprotect");
    if (nvn_dirty_lag_enabled()) {
        uint64_t ticket = atomic_load(&g_lag_done) + 1;
        CHECK(nvn_dirty_lag_protect(b->guest, g_pg, ticket), "completion store protects real outstanding read ticket");
        nvn_dirty_lag_submitted(ticket); snapshot(b, &before); calls = atomic_load(&g_mp_calls);
        uint64_t faults, hosts, ns, original_hosts;
        nvn_dirty_lag_stats(&faults, &original_hosts, &ns);
        completion_worker w = {.b = b, .offset = 256, .value = 0x67ab2194}; pthread_t thread;
        required(!pthread_create(&thread, NULL, completion_worker_run, &w), "create alias completion ticket waiter");
        uint64_t deadline = mach_absolute_time() + 5000000000ull;
        do {
            nvn_dirty_lag_stats(&faults, &hosts, &ns);
            required(mach_absolute_time() < deadline, "completion ticket wait timeout"); sched_yield();
        } while (hosts == original_hosts);
        CHECK(!atomic_load(&w.done), "alias completion store waits for actual incomplete GPU read ticket");
        nvn_dirty_lag_complete(ticket); required(!pthread_join(thread, NULL), "join alias completion ticket waiter");
        memcpy(&got, b->guest + 256, sizeof got);
        CHECK(w.result && got == w.value, "alias completion lands exact value only after explicit ticket completion");
        unchanged(b, &before, "waited completion retains original page states, generations, exact bits and gates");
        CHECK(atomic_load(&g_mp_calls) == calls && !nvn_dirty_wb_written(watch), "waited completion never mprotects guest or revokes WB ownership");
    }
    nvn_dirty_wb_unwatch(watch); puts("COMPLETION PASS copy add32 no-guest-mprotect");
}
int main(int argc, char **argv) {
    int gem = argc > 1 && !strcmp(argv[1], "i915");
    const char *option = getenv("TOTK_WB_ALIAS"); alias_on = option && !strcmp(option, "1");
    if (getenv("TOTK_FRAME_LAG") && !strcmp(getenv("TOTK_FRAME_LAG"), "1")) nvn_dirty_lag_enable();
    size_t page = (size_t)getpagesize(); backing b; int result = backing_open(&b, TEST_PAGES * page, gem, 1);
    if (result) { backing_close(&b); return result; }
    signal_install(); nvn_dirty_add_range(b.guest, b.bytes);
    CHECK(fault_hook && prep_hook, "production registers both actual runtime hooks");
    uint64_t digest = landing(&b); mixed_and_unwind(&b); concurrent_fault(&b); ticket_wait(&b); alias_completion_store(&b);
    nvn_dirty_remove_range(b.guest); backing_close(&b); alt_remove();
    printf("WB_ALIAS %s alias=%d lag=%d digest=%016" PRIx64 " checks=%u failures=%u\n", failures ? "FAIL" : "PASS", alias_on, nvn_dirty_lag_enabled(), digest, checks, failures);
    return failures != 0;
}

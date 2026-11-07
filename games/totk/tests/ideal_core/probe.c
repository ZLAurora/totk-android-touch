/* Core-id semantics of the production SVC dispatcher on the TotK path: CreateThread, Get/SetThreadCoreMask and
 * GetCurrentProcessorNumber, with TOTK_IDEAL_CORE_FIX unset (old bookkeeping, pinned here) and =1 (Horizon).
 * TOTK_FREE_CORES=1 exercises the report of a thread that holds no virtual core. */
#include "../../runtime/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R_BAD_CORE  0xE201u
#define R_BAD_COMBO 0xE801u
#define R_BAD_STATE 0xFA01u
#define CURRENT     0xFFFF8000u
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
uint64_t runtime_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void need(int ok, const char *message) { if (!ok) runtime_fail(message); }
static void kernel(unsigned id, uint64_t *r) { need(svc_thread((int)id, r) == 1, "service dispatch"); }
static void child(uint64_t arg) { (void)arg; uint64_t r[31] = {0}; kernel(0x0a, r); runtime_fail("ExitThread returned"); }
static _Alignas(16) unsigned char stack[64];
static uint32_t create(int core, uint32_t *handle) {
    uint64_t r[31] = {0};
    r[1] = (uintptr_t)child; r[3] = (uintptr_t)(stack + sizeof stack); r[4] = 44; r[5] = (uint32_t)core;
    kernel(0x08, r); *handle = (uint32_t)r[1]; return (uint32_t)r[0];
}
/* Starts the thread, which exits at once; the caller's wait gives up its core so a one-core mask can run. */
static void finish(uint32_t handle) {
    uint64_t r[31] = {handle}; kernel(0x09, r); need(!r[0], "start thread");
    r[1] = (uintptr_t)&handle; r[2] = 1; r[3] = 2000000000; kernel(0x18, r); need(!r[0], "thread exits");
    r[0] = handle; kernel(0x16, r); need(!r[0], "close thread");
}
static void affinity(uint32_t handle, int *core, uint64_t *mask) {
    uint64_t r[31] = {0}; r[2] = handle; kernel(0x0e, r); need(!r[0], "GetThreadCoreMask");
    *core = (int)(uint32_t)r[1]; *mask = r[2];
}
static void expect(uint32_t handle, int core, uint64_t mask, const char *message) {
    int c; uint64_t m; affinity(handle, &c, &m);
    if (c != core || m != mask) {
        fprintf(stderr, "FAIL %s: ideal %d mask %#llx, expected %d %#llx\n", message, c, (unsigned long long)m, core, (unsigned long long)mask);
        exit(1);
    }
}
static uint32_t set_mask(uint32_t handle, int core, uint64_t mask) {
    uint64_t r[31] = {handle, (uint32_t)core, mask}; kernel(0x0f, r); return (uint32_t)r[0];
}
static int processor(void) { uint64_t r[31] = {0}; kernel(0x10, r); return (int)(uint32_t)r[0]; }
static void processor_in(uint64_t mask, const char *message) {
    int c = processor(); if (c < 0 || c > 2 || !((mask >> c) & 1)) { fprintf(stderr, "FAIL %s: core %d, mask %#llx\n", message, c, (unsigned long long)mask); exit(1); }
}
int main(void) {
    const char *e = getenv("TOTK_IDEAL_CORE_FIX"), *f = getenv("TOTK_FREE_CORES");
    int fix = e && !strcmp(e, "1"), free_cores = f && *f && strcmp(f, "0");
    uint32_t main_handle = svc_thread_main_init(), h;
    if (!fix) {   /* The old bookkeeping, unchanged by default. */
        expect(main_handle, 0, 7, "legacy main thread");
        need(!create(-2, &h), "legacy CreateThread -2"); expect(h, -2, 7, "legacy -2 stored verbatim with mask 7"); finish(h);
        need(!create(-3, &h), "legacy CreateThread -3"); expect(h, -3, 7, "legacy -3 stored verbatim"); finish(h);
        need(!create(-1, &h), "legacy CreateThread -1"); expect(h, -1, 7, "legacy -1 stored"); finish(h);
        need(!create(2, &h), "legacy CreateThread 2"); expect(h, 2, 4, "legacy explicit core"); finish(h);
        need(create(3, &h) == R_BAD_STATE && create(-4, &h) == R_BAD_STATE, "legacy range check");
        need(!set_mask(CURRENT, -3, 6), "legacy NoUpdate outside the mask"); expect(CURRENT, 0, 6, "legacy keeps ideal core 0 outside mask 6");
        if (free_cores) need(processor() == 0, "legacy reports core 0 outside mask 6");
        need(!set_mask(CURRENT, -1, 6), "legacy DontCare"); expect(CURRENT, 0, 6, "legacy DontCare means core 0");
        need(!set_mask(CURRENT, 1, 0), "legacy zero mask"); expect(CURRENT, 1, 6, "legacy zero mask keeps the mask");
        printf("ok: ideal core legacy semantics (free cores %d)\n", free_cores);
        return 0;
    }
    expect(main_handle, 0, 1, "main thread starts on the ideal core alone");
    need(!create(-2, &h), "CreateThread -2"); expect(h, 0, 1, "-2 is the process ideal core with a one-core mask"); finish(h);
    need(!create(1, &h), "CreateThread 1"); expect(h, 1, 2, "explicit core alone"); finish(h);
    need(create(-1, &h) == R_BAD_CORE && create(-3, &h) == R_BAD_CORE, "CreateThread -1/-3 are invalid core ids");
    need(create(3, &h) == R_BAD_STATE, "CreateThread core outside the process");
    processor_in(1, "main thread on its one-core mask");
    need(set_mask(CURRENT, -3, 6) == R_BAD_COMBO, "NoUpdate needs the ideal core in the new mask"); expect(CURRENT, 0, 1, "rejected change keeps state");
    need(set_mask(CURRENT, 2, 3) == R_BAD_COMBO, "ideal core outside the mask");
    need(set_mask(CURRENT, 1, 8) == R_BAD_CORE, "mask outside the process cores");
    need(set_mask(CURRENT, 1, 0) == R_BAD_COMBO, "empty mask");
    need(set_mask(CURRENT, -4, 6) == R_BAD_CORE, "unknown negative core");
    expect(CURRENT, 0, 1, "rejected changes keep state");
    need(!set_mask(CURRENT, -1, 6), "DontCare"); expect(CURRENT, -1, 6, "DontCare has no ideal core");
    processor_in(6, "DontCare reports a core of the mask");
    if (free_cores) need(processor() == 2, "a core left its mask: the highest core of the mask");
    need(!set_mask(CURRENT, 1, 3), "explicit ideal core"); expect(CURRENT, 1, 3, "explicit ideal core"); processor_in(3, "explicit mask");
    if (free_cores) need(processor() == 1, "ideal core reported when it holds none");
    need(!set_mask(CURRENT, -3, 2), "NoUpdate inside the mask"); expect(CURRENT, 1, 2, "NoUpdate keeps the ideal core");
    processor_in(2, "NoUpdate mask");
    need(!set_mask(CURRENT, -2, 6), "UseProcessValue"); expect(CURRENT, 0, 1, "-2 resets to the ideal core alone");
    processor_in(1, "process ideal core");
    need(!create(2, &h), "CreateThread 2"); need(!set_mask(h, -1, 3), "another thread's mask"); expect(h, -1, 3, "other thread");
    finish(h);
    printf("ok: ideal core Horizon semantics (free cores %d)\n", free_cores);
    return 0;
}

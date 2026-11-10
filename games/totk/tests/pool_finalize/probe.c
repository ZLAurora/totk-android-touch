/* MIT. Synthetic guest threads, production scheduler and real lavapipe reads. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../runtime/threads.c"
#include "../../android/gfx/vk_int.h"
#include "../../android/gfx/vk_record.h"

_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL pool-finalize: %s\n", s); abort(); }
void *runtime_symbol(const char *s) { (void)s; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
static unsigned checks;
static void need(int yes, const char *s) { checks++; if (!yes) runtime_fail(s); }
static void init(thr_t *t) {
    memset(t, 0, sizeof *t); t->prio = 40; t->mask = 1; t->core = 0; t->cur_core = -1; t->started = 1;
    pthread_cond_init(&t->cv, NULL);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); pthread_mutex_unlock(&G);
}
static void fini(thr_t *t) {
    need(t->cur_core < 0 && !t->cqueued && !t->host_resume, "thread/core drained");
    pthread_mutex_lock(&G); g_h[t->handle - 0x1000].type = O_FREE; pthread_mutex_unlock(&G);
    pthread_cond_destroy(&t->cv);
}
static void take(thr_t *t) {
    pthread_setspecific(g_keythr, t); pthread_mutex_lock(&G); core_take(t); pthread_mutex_unlock(&G);
}
static void put(thr_t *t) {
    pthread_mutex_lock(&G); core_put(t); pthread_mutex_unlock(&G); pthread_setspecific(g_keythr, NULL);
}
static void poll_deadline(uint64_t start) { if (now_ns() - start > 10000000000ull) runtime_fail("fixture deadline"); sched_yield(); }
static void await(_Atomic int *p) { uint64_t start = now_ns(); while (!atomic_load(p)) poll_deadline(start); }
static uint8_t *pages(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) runtime_fail("guest mapping"); return p;
}
static void register_pool(uint8_t *p, size_t n) { mtl_pool_add(p, n); nvn_dirty_add_range(p, n); }
static void finalize(uint8_t *p) { mtl_pool_finalize(p); }
typedef struct { thr_t t; uint8_t *p; _Atomic int done, waited, retry; _Atomic uint64_t ticket; } finalizer;
static void *finalize_run(void *arg) {
    finalizer *f = arg; take(&f->t); int original = f->t.cur_core;
    vk_pool_test_wait_ticket(&f->ticket);
    if (g_vcore_release) vk_pool_test_pause_after_wait(&f->waited, &f->retry);
    finalize(f->p);
    if (f->t.cur_core != original) runtime_fail("same-core HLE return");
    put(&f->t); atomic_store(&f->done, 1); return NULL;
}
static void await_ticket(finalizer *f, uint64_t ticket) {
    uint64_t start = now_ns(); while (atomic_load(&f->ticket) != ticket) poll_deadline(start);
}
static uint64_t copy_gate(uint8_t *src, VkBuffer dst, uint64_t dst_offset, VkEvent *event) {
    VkBuffer buffer; uint64_t offset;
    if (!vk_resolve((uint64_t)(uintptr_t)src, 256, &buffer, &offset)) runtime_fail("live source resolve");
    VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev, &ei, NULL, event));
    mtl_sub_begin();
    vkCmdWaitEvents(vk_cb(), 1, event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
    VkBufferCopy c = {.srcOffset = offset, .dstOffset = dst_offset, .size = 256};
    vkCmdCopyBuffer(vk_cb(), buffer, dst, 1, &c);
    return mtl_sub_commit();
}
static void inflight(int shutdown) {
    size_t n = 131072, pg = (size_t)getpagesize(); uint8_t *src = pages(n), *alias = pages(n);
    memset(src, 0x4d, n); register_pool(src, n); mtl_pool_add(src + pg, pg);
    if (mremap(src, 0, n, MREMAP_MAYMOVE | MREMAP_FIXED, alias) != alias) runtime_fail("shared alias");
    vk_record_test_alias(alias, (uint64_t)(uintptr_t)src, n);
    nvn_dirty_protect(src, n);
    VkBuffer dst; VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 512, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &dst));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, dst, &req);
    int type = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) runtime_fail("coherent readback");
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    VkDeviceMemory memory; VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &memory)); VKCHECK(vkBindBufferMemory(V.dev, dst, memory, 0));
    uint8_t *out; VKCHECK(vkMapMemory(V.dev, memory, 0, 512, 0, (void **)&out));
    VkEvent first, second; uint64_t ticket = copy_gate(src + pg, dst, 0, &first);
    if (nvn_dirty_lag_enabled())
        need(nvn_dirty_lag_protect(src + pg, 256, ticket), "arm genuine in-flight GPU guard before finalization");
    finalizer f = {.p = src}; init(&f.t); pthread_t thread; pthread_create(&thread, NULL, finalize_run, &f);
    await_ticket(&f, ticket);
    need(!atomic_load(&f.done) && !mtl_wait(ticket, 0), "finalize waits for a genuine gated GPU read");
    if (shutdown) { puts("PASS pool-finalize shutdown during genuine GPU wait"); fflush(stdout); _exit(0); }
    if (g_vcore_release) {
        uint64_t start = now_ns();
        for (;;) { pthread_mutex_lock(&G); int released = f.t.host_resume && f.t.cur_core < 0; pthread_mutex_unlock(&G); if (released) break; poll_deadline(start); }
        finalizer concurrent[3] = {0}; pthread_t concurrent_threads[3];
        for (unsigned i = 0; i < 3; i++) {
            concurrent[i].p = pages(n); register_pool(concurrent[i].p, n);
            atomic_store(&concurrent[i].retry, 1); init(&concurrent[i].t);
            pthread_create(&concurrent_threads[i], NULL, finalize_run, &concurrent[i]);
            uint64_t began = now_ns();
            for (;;) {
                pthread_mutex_lock(&G);
                int released = concurrent[i].t.host_resume && concurrent[i].t.cur_core < 0;
                pthread_mutex_unlock(&G);
                if (released) break; poll_deadline(began);
            }
        }
        thr_t borrower; init(&borrower); take(&borrower);
        need(borrower.cur_core == 0, "another guest takes the freed core");
        uint8_t *other = pages(n); register_pool(other, n);
        VkBuffer b; uint64_t off;
        need(vk_resolve((uint64_t)(uintptr_t)other, n, &b, &off), "borrower initializes and resolves a pool while finalize waits");
        put(&borrower); fini(&borrower);
        /* A real later submission still reads the retiring pool. The old snapshot
           must not authorize destruction; recording exclusion must force retry. */
        VKCHECK(vkSetEvent(V.dev, first)); await(&f.waited);
        uint64_t later = copy_gate(alias + pg, dst, 256, &second);
        need(later > ticket, "new submission extends the snapshot frontier");
        atomic_store(&f.retry, 1); await_ticket(&f, later);
        need(!atomic_load(&f.done) && !mtl_wait(later, 0), "new references are waited safely after the first snapshot");
        VKCHECK(vkSetEvent(V.dev, second)); pthread_join(thread, NULL);
        for (unsigned i = 0; i < 3; i++) {
            pthread_join(concurrent_threads[i], NULL);
            need(atomic_load(&concurrent[i].done), "concurrent finalizers finish without retaining the borrowed core");
            fini(&concurrent[i].t); munmap(concurrent[i].p, n);
        }
        vkDestroyEvent(V.dev, second, NULL); finalize(other); munmap(other, n);
    } else { VKCHECK(vkSetEvent(V.dev, first)); pthread_join(thread, NULL); }
    need(atomic_load(&f.done) && mtl_wait(ticket, 0), "finalize returns only after actual read completion");
    for (unsigned i = 0; i < (g_vcore_release ? 512u : 256u); i++) if (out[i] != 0x4d) runtime_fail("GPU read data changed before retirement");
    need(1, "all in-flight GPU reads preserved bytes");
    VkBuffer b; uint64_t off;
    need(!vk_resolve((uint64_t)(uintptr_t)src, n, &b, &off) && !vk_resolve((uint64_t)(uintptr_t)alias, n, &b, &off), "owner/view/alias lookup invalidated");
    need(!nvn_dirty_tracked(src, n) && src[pg] == 0x4d && alias[pg] == 0x4d, "dirty removal restores writable storage and preserves aliases");
    memset(src, 0xa5, n); need(alias[pg] == 0xa5, "immediate guest stores remain coherent before address reuse");
    need(mmap(src, n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS, -1, 0) == src, "guest immediately replaces finalized address with MAP_FIXED");
    memset(src, 0x39, n); register_pool(src, n); uint64_t stamp = nvn_dirty_now(); nvn_dirty_protect(src, n);
    dm_guest_write_prepare(src, 1); src[0] = 0x73;
    need(nvn_dirty_since(src, 1, stamp) && alias[0] == 0xa5 && src[pg] == 0x39, "reused mapping has independent bytes and fresh dirty tracking");
    finalize(src); mtl_pool_del(src + pg); munmap(src, n); munmap(alias, n); fini(&f.t);
    vkDestroyEvent(V.dev, first, NULL); vkUnmapMemory(V.dev, memory); vkDestroyBuffer(V.dev, dst, NULL); vkFreeMemory(V.dev, memory, NULL);
}
typedef struct { thr_t t; unsigned rounds; } worker;
static void *stress_run(void *arg) {
    worker *w = arg; size_t n = 65536; uint8_t *p = pages(n);
    for (unsigned i = 0; i < w->rounds; i++) {
        take(&w->t); memset(p, (int)(i & 255), n); register_pool(p, n); finalize(p);
        if (p[n - 1] != (i & 255)) runtime_fail("concurrent finalize preserves bytes");
        put(&w->t);
    }
    munmap(p, n); return NULL;
}
int main(int argc, char **argv) {
    setenv("TOTK_ASYNC_PREEMPT", "0", 1); setenv("TOTK_STARVE", "0", 1);
    g_cores_on = 1; g_svc_profile = 0; g_ready_cores = 0; g_tk_async = 0;
    pthread_key_create(&g_keythr, NULL); vcore_config(); mtl_init();
    inflight(getenv("POOL_SHUTDOWN") != NULL);
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 30;
    worker w[8]; pthread_t pt[8];
    for (unsigned i = 0; i < 8; i++) { init(&w[i].t); w[i].t.mask = 7; w[i].rounds = rounds; pthread_create(&pt[i], NULL, stress_run, &w[i]); }
    for (unsigned i = 0; i < 8; i++) { pthread_join(pt[i], NULL); fini(&w[i].t); }
    uint64_t s[4]; vk_pool_test_stats(s);
    int stats = getenv("TOTK_POOL_STATS") && !strcmp(getenv("TOTK_POOL_STATS"), "1");
    need(stats ? s[0] >= 8 * rounds + 2 && s[1] && s[2] && (!g_vcore_release || s[3]) : !(s[0] | s[1] | s[2] | s[3]), "opt-in pool statistics");
    printf("PASS pool-finalize release=%d stats=%d guests=8 jobs=%u checks=%u\n", g_vcore_release, stats, 8 * rounds, checks);
    return 0;
}

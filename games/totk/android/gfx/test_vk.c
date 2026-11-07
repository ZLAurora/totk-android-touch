/* Checks the Vulkan backend's stage 1 without the game: memory pools over "guest" memory, GPU copies and uniform
   updates in command order, CPU steps, events and counters released only after the GPU finished, pool removal, and
   virtual pool aliases. Runs on the device (TOTK_VK_DRIVER picks the driver) or on a desktop with lavapipe.
     make -C android vktest NDK=...   then   adb shell TOTK_VK_DRIVER=/data/local/tmp/driver.so /data/local/tmp/vktest */
#include "vk_int.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "test_lag.h"
#include "vk_record.h"
#include "mprotect_stats.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"


static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t rec[1 << 16]; static size_t rn;
static void op(uint32_t code, const void *payload, uint32_t n) {
    uint32_t size = (8 + n + 7) & ~7u; op_hdr h = {code, size};
    memcpy(rec + rn, &h, 8); if (n) memcpy(rec + rn + 8, payload, n); memset(rec + rn + 8 + n, 0, size - 8 - n); rn += size;
}
static void op_copy(uint64_t s, uint64_t d, uint64_t n) { uint64_t a[3] = {s, d, n}; op(OP_COPY_B2B, a, 24); }
static void op_event(volatile uint32_t *v, uint32_t mode, uint32_t val) { uint8_t a[16]; memcpy(a, &v, 8); memcpy(a + 8, &mode, 4); memcpy(a + 12, &val, 4); op(OP_SIGNAL_EVENT, a, 16); }
static void op_counter(uint64_t type, uint64_t addr) { uint64_t a[2] = {type, addr}; op(OP_REPORT_COUNTER, a, 16); }
static void op_ub(uint64_t dst, const void *data, uint64_t n) { uint8_t a[16 + 256]; memcpy(a, &dst, 8); memcpy(a + 8, &n, 8); memcpy(a + 16, data, n); op(OP_UPDATE_UB, a, (uint32_t)(16 + n)); }
typedef struct {
    const void *addr; size_t len; uint64_t fence; unsigned reason; size_t range_size;
    _Atomic int *start; int result; _Atomic int done;
    const nvn_dirty_lag_batch *batch; unsigned shard, nshards;
} lag_registrar;
static void *lag_register_run(void *arg) {
    lag_registrar *r = arg;
    nvn_dirty_lag_thread_role("test-registrar");
    while (!atomic_load_explicit(r->start, memory_order_acquire)) sched_yield();
    r->result = r->batch ? nvn_dirty_lag_batch_register(r->batch, r->fence, r->shard, r->nshards) :
                          nvn_dirty_lag_register(r->addr, r->len, r->fence, r->reason, r->range_size);
    atomic_store_explicit(&r->done, 1, memory_order_release);
    return NULL;
}
/* A future-ticket walking-store regression cannot be rescued by completing a made-up ticket: that would hide
   the deadlock. Bound the real operation instead and fail the process with its exact stuck phase. */
typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv; int done; const char *phase;
} lag_liveness_watch;
static void *lag_liveness_timeout(void *arg) {
    lag_liveness_watch *w = arg;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 10;
    pthread_mutex_lock(&w->mu);
    while (!w->done) if (pthread_cond_timedwait(&w->cv, &w->mu, &until) && !w->done) {
        fprintf(stderr, "FAIL: native-policy guard liveness timed out during %s\n", w->phase);
        fflush(stderr); _Exit(1);
    }
    pthread_mutex_unlock(&w->mu); return NULL;
}
static void lag_liveness_phase(lag_liveness_watch *w, const char *phase) {
    pthread_mutex_lock(&w->mu); w->phase = phase; pthread_mutex_unlock(&w->mu);
}
typedef struct {
    nvn_dirty_lag_test_handoff hook;
    lag_liveness_watch *watch;
    void *completed_owner;
    uint8_t *src; VkEvent gate; uint64_t fallback_before;
    _Atomic int commit_returned;
    int completed_busy, future_armed, writer_waited, writer_held_after_queue, fallback, returned_early;
} lag_busy_handoff;
static void *lag_handoff_writer_run(void *arg) {
    nvn_dirty_lag_thread_role("test-guest");
    return lag_writer_run(arg);
}
static void *lag_busy_handoff_run(void *arg) {
    lag_busy_handoff *h = arg;
    nvn_dirty_lag_thread_role("test-handoff");
    while (atomic_load_explicit(&h->hook.busy_seen, memory_order_acquire) != 1) sched_yield();
    h->completed_busy = h->hook.busy_ticket == h->hook.fence - 1 &&
                        h->hook.busy_ticket <= h->hook.completed_ticket;
    /* The registrar retains the completed BUSY sample, not an incomplete-ticket shortcut. Publish a new
       ARMED owner and let a real guest fault take its BUSY gate before the registrar can continue. */
    lag_liveness_phase(h->watch, "completed-BUSY to future-ARMED handoff");
    nvn_dirty_gpu_write_end(h->completed_owner);
    h->future_armed = nvn_dirty_lag_register(h->src, 256, h->hook.fence, NVN_DIRTY_LAG_READONLY, 256) &&
                      nvn_dirty_lag_registered(h->src, 256, h->hook.fence);
    uint64_t faults0, hosts0, faults, hosts, ns;
    nvn_dirty_lag_stats(&faults0, &hosts0, &ns);
    lag_writer writer = {.dst = h->src, .n = 256, .byte = 0xa5};
    pthread_t thread; if (pthread_create(&thread, NULL, lag_handoff_writer_run, &writer)) abort();
    do {
        nvn_dirty_lag_stats(&faults, &hosts, &ns);
        if (faults != faults0 || hosts != hosts0) break;
        sched_yield();
    } while (!atomic_load_explicit(&writer.done, memory_order_acquire));
    h->writer_waited = faults != faults0 && !atomic_load_explicit(&writer.done, memory_order_acquire);
    lag_liveness_phase(h->watch, "future-BUSY registrar join before QueueSubmit2");
    atomic_store_explicit(&h->hook.release, 1, memory_order_release);
    /* This observation is incremented only after a real QueueSubmit2 and only for its failed-guard fallback.
       Suppressing protection must not pass merely because the command eventually happens to preserve bytes. */
    while (vk_record_test_lag_fallbacks() == h->fallback_before &&
           !atomic_load_explicit(&h->commit_returned, memory_order_acquire)) sched_yield();
    h->fallback = vk_record_test_lag_fallbacks() > h->fallback_before;
    h->returned_early = atomic_load_explicit(&h->commit_returned, memory_order_acquire);
    h->writer_held_after_queue = !atomic_load_explicit(&writer.done, memory_order_acquire);
    lag_liveness_phase(h->watch, "queued fallback and genuine GPU completion");
    VKCHECK(vkSetEvent(V.dev, h->gate));
    pthread_join(thread, NULL);
    return NULL;
}
typedef struct {
    lag_liveness_watch *watch;
    uint8_t *src; VkEvent gate; uint64_t ticket, fallback_before;
    _Atomic int commit_returned;
    int host, registered, waited, held;
} lag_postqueue_reuse;
static void *lag_postqueue_reuse_run(void *arg) {
    lag_postqueue_reuse *p = arg;
    nvn_dirty_lag_thread_role("test-postqueue");
    /* A duplicate BUSY registration may legitimately select the synchronous fallback. Do not require commit
       to return before opening its real GPU gate: both observations below occur strictly after QueueSubmit2. */
    while (vk_record_test_lag_fallbacks() == p->fallback_before &&
           !atomic_load_explicit(&p->commit_returned, memory_order_acquire)) sched_yield();
    p->registered = nvn_dirty_lag_registered(p->src, 160, p->ticket);
    lag_liveness_phase(p->watch, "post-submit protected reuse");
    uint64_t faults0, hosts0, ns, faults, hosts;
    nvn_dirty_lag_stats(&faults0, &hosts0, &ns);
    lag_writer writer = {.dst = p->src, .n = 256, .byte = 0xa5, .host = p->host};
    pthread_t thread; if (pthread_create(&thread, NULL, lag_writer_run, &writer)) abort();
    do {
        nvn_dirty_lag_stats(&faults, &hosts, &ns);
        if (faults != faults0 || hosts != hosts0) break;
        sched_yield();
    } while (!atomic_load_explicit(&writer.done, memory_order_acquire));
    p->waited = !atomic_load_explicit(&writer.done, memory_order_acquire);
    if (!p->host && p->waited) {
        printf("native-policy guard fixture: holding a submitted guest wait for 600ms; slow-wait diagnostic expected\n");
        fflush(stdout);
        struct timespec delay = {.tv_nsec = 600000000};
        while (nanosleep(&delay, &delay)) {}
    }
    p->held = !atomic_load_explicit(&writer.done, memory_order_acquire);
    VKCHECK(vkSetEvent(V.dev, p->gate)); pthread_join(thread, NULL);
    return NULL;
}
typedef struct {
    uint8_t *physical, *alias; size_t size, page;
    _Atomic int entered, drained, done, remove_allowed;
} lag_pool_retirement;
static void lag_lifetime_add(lag_pool_retirement *r) {
    mtl_pool_add(r->physical, r->size); nvn_dirty_add_range(r->physical, r->size);
    mtl_pool_add(r->physical + r->page, r->page);
    if (!mtl_pool_alias(r->alias, r->physical, r->size) &&
        mremap(r->physical, 0, r->size, MREMAP_MAYMOVE | MREMAP_FIXED, r->alias) != r->alias)
        DIE("lifetime fixture could not restore its shared alias");
    mtl_vpool_add(r->alias, r->size); nvn_dirty_add_range(r->alias, r->size);
    vk_record_test_alias(r->alias, (uint64_t)(uintptr_t)r->physical, r->size);
}
static void lag_lifetime_remove(lag_pool_retirement *r) {
    nvn_dirty_remove_range(r->alias); mtl_vpool_del(r->alias);
    mtl_pool_del(r->physical + r->page);
    nvn_dirty_remove_range(r->physical); mtl_pool_del(r->physical);
}
static void *lag_lifetime_retire(void *arg) {
    lag_pool_retirement *r = arg;
    const char *role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("test-retirement");
    vk_rec_lock();
    atomic_store_explicit(&r->entered, 1, memory_order_release);
    /* Same drain-before-remove ordering as nvnMemoryPoolFinalize's x_drain -> dirty remove -> pool_gpu_del.
       The real submitted fence, not a synthetic completion, must finish before any mapping can be reused. */
    mtl_drain();
    atomic_store_explicit(&r->drained, 1, memory_order_release);
    /* Explicit CPU handshake models the event/fence happens-before edge for instrumentation of the following
       metadata queries; it cannot release the real drain or substitute for GPU completion. */
    while (!atomic_load_explicit(&r->remove_allowed, memory_order_acquire)) sched_yield();
    lag_lifetime_remove(r); lag_lifetime_add(r);
    vk_rec_unlock();
    nvn_dirty_lag_thread_role(role);
    atomic_store_explicit(&r->done, 1, memory_order_release);
    return NULL;
}
static void lag_pool_lifetime(VkBuffer destination, uint8_t *out, lag_liveness_watch *watch) {
    size_t page = (size_t)getpagesize(), size = 2 * 65536;
    uint8_t *physical = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint8_t *alias = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (physical == MAP_FAILED || alias == MAP_FAILED) abort();
    lag_pool_retirement retire = {.physical = physical, .alias = alias, .size = size, .page = page, .remove_allowed = 1};
    lag_lifetime_add(&retire);
    uint8_t *source = physical + page + 32;
    memset(source, 0x4d, 256);
    CHECK(!memcmp(alias + page + 32, source, 256), "lifetime fixture aliases actual physical bytes");
    VkBuffer buffer; uint64_t offset;
    if (!vk_resolve((uint64_t)(uintptr_t)source, 256, &buffer, &offset)) abort();
    const vk_record_spans *frozen = vk_record_spans_capture(0);
    uint64_t birth = nvn_dirty_lag_epoch(), ticket = mtl_last_fence() + 1, projected = 0;
    nvn_dirty_lag_batch collected = {0}, late = {0};
    vk_record_lag_intent *unused = NULL; uint32_t count = 0, capacity = 0;
    CHECK(vk_record_lag_collect(frozen, buffer, offset, 256, ticket, NVN_DIRTY_LAG_READONLY,
              &collected, &unused, &count, &capacity, &projected) && projected >= 3 &&
          nvn_dirty_lag_batch_count(&collected) == 2,
          "collected lifetime batch retains physical, alias and nested-view projections without arming");
    lag_liveness_phase(watch, "collected pool/alias/view retirement");
    lag_lifetime_retire(&retire);
    CHECK(!nvn_dirty_lag_batch_register(&collected, ticket, 0, 1) &&
          !nvn_dirty_lag_registered(source, 256, ticket) &&
          !nvn_dirty_lag_registered(alias + page + 32, 256, ticket),
          "collected-but-unarmed refs reject same-address pool and alias/view reuse");
    CHECK(!nvn_dirty_lag_batch_add_before(&late, source, 256, NVN_DIRTY_LAG_READONLY, 256, birth),
          "old frozen birth cannot first-collect a recycled pool");
    nvn_dirty_lag_batch_destroy(&late); nvn_dirty_lag_batch_destroy(&collected);
    CHECK(vk_resolve((uint64_t)(uintptr_t)source, 256, &buffer, &offset), "recreated pool has a current GPU buffer");
    VkBuffer alias_buffer; uint64_t alias_offset;
    CHECK(vk_resolve((uint64_t)(uintptr_t)(alias + page + 32), 256, &alias_buffer, &alias_offset),
          "recreated alias has a current GPU mapping");
    frozen = vk_record_spans_capture(0); projected = 0; count = 0;
    nvn_dirty_lag_batch armed = {0};
    memset(source, 0x4d, 256); memset(out, 0, 256);
    mtl_sub_begin(); ticket = vk_record_lag_ticket();
    VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
    vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
    VkBufferCopy copy = {.srcOffset = offset, .size = 256};
    vkCmdCopyBuffer(vk_cb(), buffer, destination, 1, &copy);
    CHECK(vk_record_lag_collect(frozen, buffer, offset, 256, ticket, NVN_DIRTY_LAG_READONLY,
              &armed, &unused, &count, &capacity, &projected) &&
          nvn_dirty_lag_batch_register(&armed, ticket, 0, 1),
          "lifetime batch arms actual physical/alias units for its queued read");
    atomic_store(&retire.entered, 0); atomic_store(&retire.drained, 0);
    atomic_store(&retire.done, 0); atomic_store(&retire.remove_allowed, 0);
    pthread_t thread; if (pthread_create(&thread, NULL, lag_lifetime_retire, &retire)) abort();
    uint64_t fence = mtl_sub_commit();
    lag_liveness_phase(watch, "armed pool retirement waiting for genuine GPU completion");
    while (vk_record_test_wait_value() != fence && !atomic_load_explicit(&retire.drained, memory_order_acquire))
        sched_yield();
    CHECK(vk_record_test_wait_value() == fence && !atomic_load_explicit(&retire.drained, memory_order_acquire) &&
          !atomic_load_explicit(&retire.done, memory_order_acquire) && !mtl_wait(fence, 0),
          "retirement cannot recycle armed pools while the real GPU read is gated");
    VkBuffer live; uint64_t live_offset;
    CHECK(nvn_dirty_lag_registered(source, 256, ticket) &&
          nvn_dirty_lag_registered(alias + page + 32, 256, ticket) &&
          vk_resolve((uint64_t)(uintptr_t)source, 256, &live, &live_offset) && live == buffer &&
          vk_resolve((uint64_t)(uintptr_t)(alias + page + 32), 256, &live, &live_offset) && live == alias_buffer,
          "drain excludes physical/view/alias destruction until submitted reads finish");
    atomic_store_explicit(&retire.remove_allowed, 1, memory_order_release);
    VKCHECK(vkSetEvent(V.dev, gate)); pthread_join(thread, NULL);
    int bad = 0; for (int i = 0; i < 256; i++) bad += out[i] != 0x4d;
    CHECK(!bad && mtl_wait(fence, 0), "retired pool's genuine queued read finishes with correct bytes");
    CHECK(!nvn_dirty_lag_batch_register(&armed, fence + 1, 0, 1) &&
          !nvn_dirty_lag_registered(source, 256, fence + 1),
          "post-completion same-address reuse still rejects old armed refs");
    nvn_dirty_lag_batch_destroy(&armed); free(unused); vkDestroyEvent(V.dev, gate, NULL);
    lag_lifetime_remove(&retire); munmap(alias, size); munmap(physical, size);
}
static void lag_native_guard_liveness(uint8_t *base) {
    /* Native GPU policy is safe on lavapipe only for these read-only inputs. The destination has its own
       Vulkan allocation, never a protected guest VA or an alias of the source's dirty unit. */
    VkBuffer destination;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &destination));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, destination, &req);
    int type = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) DIE("native-policy guard test needs coherent host-visible memory");
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    VkDeviceMemory memory; VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &memory));
    VKCHECK(vkBindBufferMemory(V.dev, destination, memory, 0));
    uint8_t *out; VKCHECK(vkMapMemory(V.dev, memory, 0, 256, 0, (void **)&out));
    lag_liveness_watch watch = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .phase = "fixture setup"};
    pthread_t watchdog; if (pthread_create(&watchdog, NULL, lag_liveness_timeout, &watch)) abort();
    vk_record_test_native_guards(1);
    for (int host = 0; host < 2; host++) {
        uint8_t *src = base + (size_t)host * 65536;
        memset(src, 0x5a, 256); memset(out, 0, 256);
        VkBuffer source; uint64_t offset;
        if (!vk_resolve((uint64_t)(uintptr_t)src, 256, &source, &offset)) DIE("native-policy guard test source is not mapped");
        VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t ticket = vk_record_lag_ticket();
        int split = host && vk_record_test_async_submit();
        uint64_t fallback_before = vk_record_test_lag_fallbacks();
        if (!host) {
            /* Exceed the primary guard-only job limit. Assembly used to join these already-armed jobs. */
            for (uint32_t i = 0; i < 40; i++)
                vk_record_lag_guard(source, offset + 4 * i, 4, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_COLD);
        } else {
            if (split) vk_record_test_pause();
            /* Exercise the recording worker's frozen vertex lookup and guard collection, without shaders or
               shader-written destinations: this secondary only binds state, while the transfer below reads it. */
            VkCommandBuffer primary = vk_cb();
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
                .renderArea = {{0, 0}, {1, 1}}, .layerCount = 1};
            vkCmdBeginRendering(primary, &ri);
            VkFormat colors[8] = {0};
            vk_record_pass_begin(0, colors, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
            vk_record_draw *d = vk_record_draw_begin(VK_NULL_HANDLE, VK_NULL_HANDLE);
            d->dynamic = (vk_record_dynamic){.vp = {0, 0, 1, 1, 0, 1}, .sc = {{0, 0}, {1, 1}},
                .front = VK_FRONT_FACE_COUNTER_CLOCKWISE, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .dc = VK_COMPARE_OP_ALWAYS};
            d->vertex_mask = 1;
            d->vertex[0].address = (uint64_t)(uintptr_t)src; d->vertex[0].size = 256;
            d->vertex[0].buffer = VK_NULL_HANDLE; d->vertex[0].offset = d->vertex[0].stride = 0;
            d->vertex[0].lag_reason = NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_COLD;
            d->count = 0; d->instances = 1; d->base_instance = 0;
            vk_record_draw_commit();
            int parked = vk_gpuprof_on ? vk_record_pass_end_profiled(primary, NULL) : vk_record_pass_end(primary);
            CHECK(parked, "bind-only recorder pass is parked for worker assembly");
            (void)vk_record_next_primary();
        }
        VkBufferCopy copy = {.srcOffset = offset, .size = 256};
        vkCmdCopyBuffer(vk_cb(), source, destination, 1, &copy);
        lag_liveness_phase(&watch, "pre-submit recording assembly");
        if (!split) vk_record_test_assemble();
        CHECK(!nvn_dirty_lag_registered(src, 160, ticket), "recording/assembly does not arm an unsubmitted future ticket");
        lag_liveness_phase(&watch, "walking CPU store before QueueSubmit2");
        /* This is the walking thread itself, not a writer that submit could leave behind. It must finish before
           this same thread can queue the command buffer. The old ordering blocks here until the watchdog fails. */
        lag_guest_store(src, 0x6b); memset(src, 0x6b, 256);
        CHECK(src[0] == 0x6b && src[255] == 0x6b, "walking CPU writeback completes before the actual submission");
        if (split) {
            VkCommandBuffer previous = vk_cb(); vk_submit_chunk();
            CHECK(vk_record_test_wait_paused(), "native slot projection worker remains gated after seal");
            VkCommandBuffer next = vk_cb(); uint64_t reserved, queued;
            vk_record_test_submit_status(&reserved, &queued);
            CHECK(reserved == ticket && queued < ticket && next != previous && vk_record_lag_ticket() == ticket + 1,
                "native walk begins the next slot while sealed registration/queue publication remains blocked");
            vk_record_test_release();
        }
        lag_postqueue_reuse reuse = {.watch = &watch, .src = src, .gate = gate, .ticket = ticket,
            .fallback_before = fallback_before, .host = host};
        pthread_t opener; if (pthread_create(&opener, NULL, lag_postqueue_reuse_run, &reuse)) abort();
        lag_liveness_phase(&watch, "slot registration/join/QueueSubmit2");
        uint64_t fence = mtl_sub_commit();
        atomic_store_explicit(&reuse.commit_returned, 1, memory_order_release);
        pthread_join(opener, NULL);
        CHECK(fence == ticket + (split != 0), "guard registration keeps its actual slot ticket before the final slot");
        CHECK(reuse.registered, "submit registers the readonly unit before postqueue reuse");
        CHECK(reuse.waited, "native-policy %s reuse is protected after submission", host ? "host" : "guest");
        if (!host) CHECK(reuse.held, "real submitted reuse stays blocked while the GPU gate is closed");
        CHECK(mtl_wait(fence, 2000000000ull), "native-policy gated read completes");
        int bad = 0; for (int i = 0; i < 256; i++) bad += out[i] != 0x6b;
        CHECK(!bad, "native-policy read preserves the walking store through later reuse (%d wrong bytes)", bad);
        vkDestroyEvent(V.dev, gate, NULL);
    }
    /* The exact remaining ownership cycle: an old completed owner is BUSY when the submit registrar samples it,
       then a future ARMED ticket is claimed by a guest writer before that registrar resumes. The only release is
       the real queued read's completion. Reinstating the old completed-ticket BUSY wait hangs the process watchdog. */
    {
        uint8_t *src = base + 2 * 65536;
        memset(src, 0x7c, 256); memset(out, 0, 256);
        uint64_t completed = mtl_last_fence();
        CHECK(mtl_wait(completed, 0), "handoff fixture starts from a genuinely completed GPU ticket");
        CHECK(nvn_dirty_lag_register(src, 256, completed, NVN_DIRTY_LAG_READONLY, 256),
              "handoff fixture arms the completed ticket before taking BUSY");
        void *owner = nvn_dirty_gpu_write_begin(src, 256);
        if (!owner) DIE("completed-BUSY handoff test needs a tracked GPU-write token");
        VkBuffer source; uint64_t offset;
        if (!vk_resolve((uint64_t)(uintptr_t)src, 256, &source, &offset)) DIE("handoff fixture source is not mapped");
        VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
        const char *caller_role = nvn_dirty_lag_thread_role_get();
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t ticket = vk_record_lag_ticket();
        vk_record_lag_guard(source, offset, 256, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_COLD);
        VkBufferCopy copy = {.srcOffset = offset, .size = 256};
        vkCmdCopyBuffer(vk_cb(), source, destination, 1, &copy);
        lag_busy_handoff handoff = {
            .hook = {.addr = src, .fence = ticket}, .watch = &watch, .completed_owner = owner,
            .src = src, .gate = gate, .fallback_before = vk_record_test_lag_fallbacks()
        };
        nvn_dirty_lag_test_handoff_set(&handoff.hook);
        lag_liveness_phase(&watch, "registrar sampling completed-BUSY");
        pthread_t coordinator; if (pthread_create(&coordinator, NULL, lag_busy_handoff_run, &handoff)) abort();
        uint64_t fence = mtl_sub_commit();
        atomic_store_explicit(&handoff.commit_returned, 1, memory_order_release);
        pthread_join(coordinator, NULL);
        nvn_dirty_lag_test_handoff_set(NULL);
        CHECK(handoff.completed_busy, "registrar captures BUSY with the exact completed old ticket");
        CHECK(handoff.future_armed, "a future ARMED ticket is published before the registrar resumes");
        CHECK(handoff.writer_waited, "guest takes future BUSY and really waits before registrar wake");
        CHECK(atomic_load_explicit(&handoff.hook.rejected, memory_order_acquire),
              "completed-BUSY registration returns failure instead of following its future owner");
        CHECK(handoff.fallback && !handoff.returned_early,
              "BUSY failure reaches the actual postqueue fallback, not suppressed protection");
        CHECK(handoff.writer_held_after_queue, "future BUSY writer remains protected until genuine GPU completion");
        CHECK(fence == ticket && mtl_wait(fence, 0), "fallback returns only after its actual queued ticket completes");
        int bad = 0; for (int i = 0; i < 256; i++) bad += out[i] != 0x7c;
        CHECK(!bad, "handoff GPU read retains pre-reuse bytes (%d wrong bytes)", bad);
        CHECK(src[0] == 0xa5 && src[255] == 0xa5, "real queued completion releases the guest writer");
        CHECK(nvn_dirty_lag_thread_role_get() == caller_role, "submit restores its guest caller's diagnostic role");
        vkDestroyEvent(V.dev, gate, NULL);
    }
    lag_pool_lifetime(destination, out, &watch);
    vk_record_test_native_guards(0);
    pthread_mutex_lock(&watch.mu); watch.done = 1; pthread_cond_signal(&watch.cv); pthread_mutex_unlock(&watch.mu);
    pthread_join(watchdog, NULL); pthread_cond_destroy(&watch.cv); pthread_mutex_destroy(&watch.mu);
    vkUnmapMemory(V.dev, memory); vkDestroyBuffer(V.dev, destination, NULL); vkFreeMemory(V.dev, memory, NULL);
}
/* These protocol checks run only after real GPU work, or in the isolated --lag-bench process. */
static void lag_batch_checks(size_t unit, uint64_t ticket) {
    size_t pg = (size_t)getpagesize(), len = 2 * unit + pg, mapping_len = len + 3 * pg;
    uint8_t *mapping = mmap(NULL, mapping_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) abort();
    uint8_t *base = mapping + pg;
    if (unit > pg && (uintptr_t)base % unit == 0) base += pg;
    uint64_t before_add = nvn_dirty_lag_epoch();
    nvn_dirty_add_range(base, len);
    nvn_dirty_lag_batch batch = {0}, other = {0}, stale = {0}, bad = {0}, late = {0};
    CHECK(!nvn_dirty_lag_batch_add_before(&late, base + 17, 32, 0, 0, before_add) &&
          !nvn_dirty_lag_batch_register(&late, ticket, 0, 1), "freeze before initial range birth cannot certify its later incarnation");
    nvn_dirty_lag_batch_reset(&late);
    uint64_t rearms0, updates0, promotions, demotions, rearms1, updates1, calls0, pages0, ns0, calls1, pages1, ns1;
    nvn_dirty_lag_sticky_stats(&rearms0, &updates0, &promotions, &demotions);
    nvn_dirty_mp_stats(&calls0, &pages0, &ns0);
    CHECK(nvn_dirty_lag_batch_add(&batch, base + 17, 32, NVN_DIRTY_LAG_READONLY, 64) &&
          nvn_dirty_lag_batch_add(&batch, base + 25, 64, NVN_DIRTY_LAG_ALIAS, 512) &&
          nvn_dirty_lag_batch_add(&batch, base + unit - 16, 32, NVN_DIRTY_LAG_GPU, 1024) &&
          nvn_dirty_lag_batch_add(&batch, base + 2 * unit + 7, 16, NVN_DIRTY_LAG_WRITABLE, 2048),
          "batch collects overlapping and cross-unit spans");
    CHECK(nvn_dirty_lag_batch_count(&batch) == 3 && nvn_dirty_lag_batch_bytes(&batch) == len,
          "batch deduplicates range-relative units and clips the final unit to %zu bytes", pg);
    CHECK(nvn_dirty_lag_batch_add(&other, base + 19, 32, NVN_DIRTY_LAG_COLD, 4096) &&
          nvn_dirty_lag_batch_add(&other, base + 2 * unit + 9, 16, NVN_DIRTY_LAG_ALIAS, 65536) &&
          nvn_dirty_lag_batch_merge(&batch, &other) && nvn_dirty_lag_batch_merge(&batch, &batch),
          "merge combines retained refs, including a harmless self-merge");
    CHECK(nvn_dirty_lag_batch_count(&batch) == 3 && nvn_dirty_lag_batch_count(&other) == 2 &&
          nvn_dirty_lag_batch_bytes(&batch) == len, "merge preserves unique extents and leaves its source unchanged");
    nvn_dirty_lag_sticky_stats(&rearms1, &updates1, &promotions, &demotions);
    nvn_dirty_mp_stats(&calls1, &pages1, &ns1);
    CHECK(rearms0 == rearms1 && updates0 == updates1 && calls0 == calls1 &&
          !nvn_dirty_lag_registered(base, len, ticket), "collect/merge never arm or change protection counters");
    CHECK(!nvn_dirty_lag_batch_register(&batch, ticket, 0, 0) &&
          !nvn_dirty_lag_batch_register(&batch, ticket, 3, 3) &&
          !nvn_dirty_lag_batch_register(&batch, 0, 0, 1) &&
          !nvn_dirty_lag_batch_register(&batch, UINT64_MAX, 0, 1), "batch rejects invalid shards and tickets");
    CHECK(nvn_dirty_lag_batch_register(&batch, ticket, 0, 3), "first registration shard succeeds");
    CHECK(nvn_dirty_lag_registered(base, 1, ticket) && !nvn_dirty_lag_registered(base + unit, 1, ticket) &&
          !nvn_dirty_lag_registered(base + 2 * unit, 1, ticket), "one shard touches only its dense entry index");
    _Atomic int start = 0;
    lag_registrar regs[2] = {
        {.fence = ticket, .start = &start, .batch = &batch, .shard = 1, .nshards = 3},
        {.fence = ticket, .start = &start, .batch = &batch, .shard = 2, .nshards = 3}
    };
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) if (pthread_create(&threads[i], NULL, lag_register_run, &regs[i])) abort();
    atomic_store_explicit(&start, 1, memory_order_release);
    for (unsigned i = 0; i < 2; i++) pthread_join(threads[i], NULL);
    nvn_dirty_lag_sticky_stats(&rearms1, &updates1, &promotions, &demotions);
    CHECK(regs[0].result && regs[1].result && nvn_dirty_lag_registered(base, len, ticket) &&
          rearms1 == rearms0 + 3 && updates1 == updates0, "disjoint concurrent shards arm every unit exactly once");
    nvn_dirty_mp_stats(&calls0, &pages0, &ns0);
    for (unsigned i = 0; i < 32; i++) CHECK(nvn_dirty_lag_batch_register(&batch, ticket, 0, 1),
                                           "duplicate equal-ticket batch succeeds");
    CHECK(nvn_dirty_lag_batch_register(&batch, ticket - 1, 0, 1), "later ARMED tickets satisfy an older batch ticket");
    nvn_dirty_lag_sticky_stats(&rearms0, &updates0, &promotions, &demotions);
    nvn_dirty_mp_stats(&calls1, &pages1, &ns1);
    CHECK(rearms1 == rearms0 && updates1 == updates0 && calls0 == calls1 && pages0 == pages1,
          "equal/older batch tickets change neither ticket counters nor page protection");
    nvn_dirty_lag_batch race = {0};
    CHECK(nvn_dirty_lag_batch_add(&race, base + 19, 48, 0, 0), "collects a shared immutable ticket-race batch");
    _Atomic int race_start = 0; pthread_t race_threads[4];
    lag_registrar racers[4];
    for (unsigned i = 0; i < 4; i++) {
        racers[i] = (lag_registrar){.addr = base + 17, .len = 64, .fence = ticket + 1 + i, .start = &race_start,
            .batch = i & 1 ? &race : NULL, .nshards = 1};
        if (pthread_create(&race_threads[i], NULL, lag_register_run, &racers[i])) abort();
    }
    atomic_store_explicit(&race_start, 1, memory_order_release);
    int race_ok = 1;
    for (unsigned i = 0; i < 4; i++) { pthread_join(race_threads[i], NULL); race_ok &= racers[i].result; }
    nvn_dirty_lag_sticky_stats(&rearms1, &updates1, &promotions, &demotions);
    nvn_dirty_mp_stats(&calls1, &pages1, &ns1);
    CHECK(race_ok && nvn_dirty_lag_registered(base, 64, ticket + 4) &&
          !nvn_dirty_lag_registered(base, 64, ticket + 5) && rearms0 == rearms1 && calls0 == calls1 &&
          updates1 > updates0 && updates1 <= updates0 + 4,
          "mixed batch/legacy concurrent CAS raises only the maximum ticket, without rearming");
    ticket += 4;
    nvn_dirty_lag_complete(ticket);
    void *owner = nvn_dirty_gpu_write_begin(base, 16);
    if (!owner) abort();
    CHECK(!nvn_dirty_lag_batch_register(&race, ticket + 1, 0, 1),
          "batch rejects BUSY nonblocking even when its sampled owner ticket is completed");
    nvn_dirty_gpu_write_end(owner);
    CHECK(nvn_dirty_lag_batch_register(&race, ticket + 1, 0, 1), "batch can increase a ticket after BUSY has ended");
    nvn_dirty_lag_batch_destroy(&race); ticket++;
    nvn_dirty_lag_complete(ticket);
    for (unsigned i = 0; i < 3; i++) lag_guest_store(base + i * unit, (uint8_t)i);
    nvn_dirty_lag_unit top[16]; int n = nvn_dirty_lag_top(top, 16), found = 0;
    unsigned reasons[3] = {NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_ALIAS | NVN_DIRTY_LAG_GPU | NVN_DIRTY_LAG_COLD,
                          NVN_DIRTY_LAG_GPU, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_ALIAS};
    size_t maximum[3] = {4096, 1024, 65536};
    for (int i = 0; i < n; i++) for (unsigned j = 0; j < 3; j++)
        if (top[i].addr == (uintptr_t)(base + j * unit) && top[i].reasons == reasons[j] &&
            top[i].max_range == maximum[j] && top[i].len == (j == 2 ? pg : unit)) found++;
    CHECK(found == 3, "batch diagnostics preserve per-unit reason OR, maximum descriptor and clipped extent");
    nvn_dirty_lag_watch(base, len);
    for (unsigned i = 0; i < 3; i++) lag_guest_store(base + i * unit, (uint8_t)i);
    n = nvn_dirty_lag_top(top, 16); found = 0;
    for (int i = 0; i < n; i++) if (top[i].addr >= (uintptr_t)base && top[i].addr < (uintptr_t)base + len)
        found += !top[i].reasons && !top[i].max_range;
    CHECK(found == 3, "top reset clears registration diagnostics without losing subsequent writes");
    CHECK(nvn_dirty_lag_batch_add(&stale, base + 17, 32, NVN_DIRTY_LAG_ALIAS, 128), "collects old slot incarnation");
    uint64_t frozen_epoch = nvn_dirty_lag_epoch();
    nvn_dirty_add_range(mapping, pg);
    CHECK(nvn_dirty_lag_epoch() > frozen_epoch &&
          nvn_dirty_lag_batch_add_before(&late, base + 17, 32, 0, 0, frozen_epoch),
          "unrelated new range birth does not invalidate a frozen existing range");
    nvn_dirty_lag_batch_reset(&late);
    nvn_dirty_remove_range(base); nvn_dirty_add_range(base, len);
    CHECK(!nvn_dirty_lag_batch_add_before(&late, base + 17, 32, 0, 0, frozen_epoch) &&
          !nvn_dirty_lag_batch_register(&late, ticket, 0, 1) &&
          !nvn_dirty_lag_registered(base, len, ticket),
          "recycling before first collection rejects the old frozen epoch without arming the new alias");
    CHECK(!nvn_dirty_lag_batch_register(&stale, ticket, 0, 1) &&
          !nvn_dirty_lag_registered(base, len, ticket), "same-address slot reuse rejects stale refs before arming");
    CHECK(!nvn_dirty_lag_batch_merge(&bad, &stale) && !nvn_dirty_lag_batch_register(&bad, ticket, 0, 1),
          "merge rejects and latches a stale source generation");
    CHECK(!nvn_dirty_lag_batch_add(&stale, base + 17, 32, 0, 0),
          "cached range cannot silently acquire a reused slot generation");
    void *storage = batch.impl; nvn_dirty_lag_batch_reset(&batch);
    CHECK(batch.impl == storage && !nvn_dirty_lag_batch_count(&batch) && !nvn_dirty_lag_batch_bytes(&batch) &&
          nvn_dirty_lag_batch_register(&batch, ticket, 0, 1), "reset retains storage and produces an empty valid batch");
    nvn_dirty_lag_batch_reset(&bad);
    CHECK(nvn_dirty_lag_batch_add(&bad, base, 16, 0, 0) &&
          !nvn_dirty_lag_batch_add(&bad, base + len, 16, 0, 0) &&
          !nvn_dirty_lag_batch_register(&bad, ticket, 0, 1), "unknown range failure cannot silently omit protection");
    nvn_dirty_lag_batch_reset(&bad);
    CHECK(!nvn_dirty_lag_batch_add(&bad, base + len - 8, 16, 0, 0) &&
          !nvn_dirty_lag_batch_register(&bad, ticket, 0, 1), "partly untracked spans fail closed");
    nvn_dirty_lag_batch_reset(&stale);
    CHECK(nvn_dirty_lag_batch_add(&stale, base + 17, 32, 0, 0) &&
          nvn_dirty_lag_batch_register(&stale, ticket, 0, 1), "reset allows a fresh current-generation reference");
    nvn_dirty_remove_range(base);
    nvn_dirty_remove_range(mapping);
    nvn_dirty_lag_batch_destroy(&batch); nvn_dirty_lag_batch_destroy(&other);
    nvn_dirty_lag_batch_destroy(&stale); nvn_dirty_lag_batch_destroy(&bad);
    nvn_dirty_lag_batch_destroy(&late);
    munmap(mapping, mapping_len);
}
typedef struct { const void *addr; size_t len; unsigned reason; size_t max_range; unsigned job; } lag_bench_input;
typedef struct {
    const nvn_dirty_lag_batch *batch; uint64_t ticket; unsigned shard, nshards; _Atomic int *start;
    uint64_t cpu_ns, wall_ns; int ok;
} lag_bench_arm;
static uint64_t lag_bench_cpu_ns(void) {
    struct timespec t; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static void *lag_bench_arm_run(void *arg) {
    lag_bench_arm *arm = arg;
    nvn_dirty_lag_thread_role("lag-bench-arm");
    while (!atomic_load_explicit(arm->start, memory_order_acquire)) sched_yield();
    uint64_t cpu = lag_bench_cpu_ns(), wall = host_now_ns();
    arm->ok = nvn_dirty_lag_batch_register(arm->batch, arm->ticket, arm->shard, arm->nshards);
    arm->wall_ns = host_now_ns() - wall; arm->cpu_ns = lag_bench_cpu_ns() - cpu;
    return NULL;
}
static void runtime_mprotect_checks(void) {
    size_t pg = getpagesize();
    uint8_t *memory = mmap(NULL, pg * 2, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED, "runtime protection pre-init fixture"); if (memory == MAP_FAILED) return;
    nvn_dirty_mp_count before[NVN_DIRTY_MP_REASON_COUNT], after[NVN_DIRTY_MP_REASON_COUNT];
    nvn_dirty_mp_reason_stats(before);
    errno = EDOM;
    CHECK(!runtime_mprotect(memory, pg, PROT_READ, NVN_DIRTY_MP_VM_COMMIT) && errno == EDOM,
          "counted runtime mprotect preserves successful return and incoming errno before dirty init");
    CHECK(mprotect(memory + 1, pg, PROT_READ) == -1, "raw unaligned protection oracle rejects input");
    int expected_errno = errno;
    CHECK(runtime_mprotect(memory + 1, pg, PROT_READ, NVN_DIRTY_MP_VM_COMMIT) == -1 && errno == expected_errno,
          "counted runtime mprotect preserves actual failure return and errno");
    CHECK(!runtime_mprotect(memory, pg + 1, PROT_READ, NVN_DIRTY_MP_VM_COMMIT), "raw runtime extent rounds to system pages");
    CHECK(!runtime_mprotect(memory, pg * 2, PROT_READ | PROT_WRITE, NVN_DIRTY_MP_VM_DECOMMIT), "runtime restoration remains a raw syscall");
    CHECK(!runtime_mprotect(memory, pg, PROT_READ, NVN_DIRTY_MP_STACK) &&
          !runtime_mprotect(memory, pg, PROT_READ | PROT_WRITE, NVN_DIRTY_MP_MODULE) &&
          !runtime_mprotect(memory, pg, PROT_READ | PROT_WRITE, NVN_DIRTY_MP_DEBUG_WATCH),
          "stack/module/diagnostic owners retain separate reason counters");
    memory[pg] = 0x52;
    uint8_t byte = memory[pg];
    CHECK(!munmap(memory + pg, pg) && mprotect(memory + pg, pg, PROT_READ) == -1, "unmapped protection failure oracle");
    expected_errno = errno;
    CHECK(runtime_mprotect(memory + pg, pg, PROT_READ, NVN_DIRTY_MP_VM_DECOMMIT) == -1 && errno == expected_errno,
          "runtime ENOMEM-style failure does not enter guard pressure handling");
    nvn_dirty_mp_reason_stats(after);
    CHECK(after[NVN_DIRTY_MP_VM_COMMIT].calls - before[NVN_DIRTY_MP_VM_COMMIT].calls == 3 &&
          after[NVN_DIRTY_MP_VM_COMMIT].pages - before[NVN_DIRTY_MP_VM_COMMIT].pages == 4 &&
          after[NVN_DIRTY_MP_VM_DECOMMIT].calls - before[NVN_DIRTY_MP_VM_DECOMMIT].calls == 2 &&
          after[NVN_DIRTY_MP_VM_DECOMMIT].pages - before[NVN_DIRTY_MP_VM_DECOMMIT].pages == 3,
          "pre-init runtime counters use system pages and include the failed syscall");
    CHECK(after[NVN_DIRTY_MP_VM_COMMIT].ns > before[NVN_DIRTY_MP_VM_COMMIT].ns,
          "runtime protection time is reportable before dirty-pool initialization");
    CHECK(byte == 0x52 && !nvn_dirty_lag_vma_pressure(), "runtime error handling does not invent lag guard pressure");
    munmap(memory, pg * 2);
}

static void direct_watch_bench(void) {
    enum { UNITS = 512, UNIT = 65536 };
    uint64_t samples[2][3]; unsigned count[2] = {0};
    for (unsigned pass = 0; pass < 6; pass++) {
        unsigned bulk = pass == 1 || pass == 2 || pass == 5;
        size_t bytes = (size_t)UNITS * UNIT, pg = getpagesize();
        uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(memory != MAP_FAILED, "direct watch benchmark mapping"); if (memory == MAP_FAILED) return;
        for (size_t i = 0; i < bytes; i += pg) memory[i] = 0x3a; /* populated PTEs, not only empty VMAs */
        nvn_dirty_add_range(memory, bytes);
        nvn_dirty_mp_count before[NVN_DIRTY_MP_REASON_COUNT], after[NVN_DIRTY_MP_REASON_COUNT];
        nvn_dirty_mp_reason_stats(before); uint64_t cpu = lag_bench_cpu_ns(); int ok = 1;
        if (bulk) ok = nvn_dirty_lag_protect(memory, bytes, 0);
        else for (unsigned i = 0; i < UNITS; i++) ok &= nvn_dirty_lag_protect(memory + (size_t)i * UNIT, UNIT, 0);
        samples[bulk][count[bulk]++] = lag_bench_cpu_ns() - cpu;
        nvn_dirty_mp_reason_stats(after);
        CHECK(ok && nvn_dirty_lag_registered(memory, bytes, 0), "direct %s watch owns every unit", bulk ? "bulk" : "scalar");
        CHECK(after[NVN_DIRTY_MP_SNAPSHOT].calls - before[NVN_DIRTY_MP_SNAPSHOT].calls == (bulk ? UNITS / 64 : UNITS) &&
              after[NVN_DIRTY_MP_SNAPSHOT].pages - before[NVN_DIRTY_MP_SNAPSHOT].pages == bytes / pg,
              "direct %s watch batches only syscall count, not covered pages", bulk ? "bulk" : "scalar");
        nvn_dirty_mp_reason_stats(before);
        CHECK(nvn_dirty_lag_protect(memory, bytes, 0), "repeated watched span remains valid");
        nvn_dirty_mp_reason_stats(after);
        CHECK(after[NVN_DIRTY_MP_SNAPSHOT].calls == before[NVN_DIRTY_MP_SNAPSHOT].calls, "already-RO direct watch performs no syscall");
        uint64_t stamp = nvn_dirty_now();
        lag_guest_store(memory + UNIT / 2, 0x62); lag_guest_store(memory + bytes - pg, 0x73);
        CHECK(nvn_dirty_since(memory + UNIT / 2, 1, stamp) && nvn_dirty_since(memory + bytes - pg, 1, stamp) &&
              !nvn_dirty_since(memory + UNIT + UNIT / 2, 1, stamp), "batched direct watch retains page dirty authority");
        nvn_dirty_remove_range(memory); munmap(memory, bytes);
    }
    for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned i = 1; i < 3; i++) for (unsigned j = i; j && samples[mode][j] < samples[mode][j - 1]; j--) {
            uint64_t swap = samples[mode][j]; samples[mode][j] = samples[mode][j - 1]; samples[mode][j - 1] = swap;
        }
    printf("mprotect-bench direct-watch: populated_units=%u scalar_calls=%u bulk_calls=%u scalar_cpu_ns/unit=%.2f bulk_cpu_ns/unit=%.2f (interleaved medians)\n",
           UNITS, UNITS, UNITS / 64, samples[0][1] / (double)UNITS, samples[1][1] / (double)UNITS);
}

/* Protocol-only completed tickets: measures actual protection syscalls, never initializes Vulkan.
   2400 tracked units approximate route inventory; 240 rewritten units/frame isolate both writer paths. */
static int mprotect_bench(void) {
    enum { UNITS = 2400, WRITES = 240, FRAMES = 8, WARMUP = 2, UNIT = 65536 };
    setenv("TOTK_DIRTY_UNIT_KB", "64", 1); unsetenv("TOTK_DIRTY_UNIT");
    runtime_mprotect_checks();
    nvn_dirty_lag_enable();
    uint8_t *memory = mmap(NULL, (size_t)UNITS * UNIT, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED, "mprotect benchmark mapping");
    if (memory == MAP_FAILED) return 1;
    nvn_dirty_add_range(memory, (size_t)UNITS * UNIT);
    nvn_dirty_lag_batch batch = {0};
    CHECK(nvn_dirty_lag_batch_add(&batch, memory, (size_t)UNITS * UNIT, NVN_DIRTY_LAG_READONLY, UNIT),
          "mprotect benchmark collects all 2400 units");
    uint64_t ticket = 1, calls0, pages0, ns0, calls1, pages1, ns1;
    nvn_dirty_mp_stats(&calls0, &pages0, &ns0);
    CHECK(nvn_dirty_lag_batch_register(&batch, ticket, 0, 1), "mprotect benchmark cold batch");
    nvn_dirty_lag_complete(ticket);
    nvn_dirty_mp_stats(&calls1, &pages1, &ns1);
    printf("mprotect-bench cold: units=%d calls=%llu pages=%llu ms=%.3f\n", UNITS,
        (unsigned long long)(calls1 - calls0), (unsigned long long)(pages1 - pages0), (ns1 - ns0) / 1e6);
    for (int mode = 0; mode < 3; mode++) {
        const char *name = mode == 0 ? "guest-interior" : mode == 1 ? "host-interior" : "guest-edge";
        uint64_t writer_ns = 0, arm_ns = 0, writer_calls = 0, arm_calls = 0, writer_pages = 0;
        size_t offset = mode == 2 ? 0 : UNIT / 2;
        for (int frame = 0; frame < FRAMES; frame++) {
            nvn_dirty_mp_count before[NVN_DIRTY_MP_REASON_COUNT], after[NVN_DIRTY_MP_REASON_COUNT], armed[NVN_DIRTY_MP_REASON_COUNT];
            nvn_dirty_mp_reason_stats(before);
            uint64_t stamp = nvn_dirty_now(), cpu = lag_bench_cpu_ns();
            for (int i = 0; i < WRITES; i++) {
                uint8_t *p = memory + (size_t)i * (UNITS / WRITES) * UNIT + offset;
                if (mode == 1) dm_guest_write_prepare(p, 1);
                lag_guest_store(p, (uint8_t)(frame + 1));
            }
            uint64_t writer = lag_bench_cpu_ns() - cpu;
            nvn_dirty_mp_reason_stats(after); cpu = lag_bench_cpu_ns();
            CHECK(nvn_dirty_lag_batch_register(&batch, ++ticket, 0, 1), "mprotect %s frame %d rearm", name, frame);
            uint64_t arm = lag_bench_cpu_ns() - cpu;
            nvn_dirty_lag_complete(ticket); nvn_dirty_mp_reason_stats(armed);
            nvn_dirty_mp_reason reason = mode == 1 ? NVN_DIRTY_MP_PREPARE : NVN_DIRTY_MP_FAULT;
            CHECK(after[reason].calls - before[reason].calls == WRITES && after[reason].pages - before[reason].pages == (uint64_t)WRITES * UNIT / getpagesize(),
                  "mprotect %s frame %d counts only writer syscalls and full dirty units", name, frame);
            CHECK(nvn_dirty_since(memory + offset, 1, stamp) && memory[offset] == (uint8_t)(frame + 1),
                  "mprotect %s frame %d retains dirty generations and actual bytes", name, frame);
            CHECK(!nvn_dirty_since(memory + UNIT + offset, 1, stamp), "mprotect %s frame %d leaves an unwritten unit clean", name, frame);
            if (frame >= WARMUP) {
                writer_ns += writer; arm_ns += arm;
                writer_calls += after[reason].calls - before[reason].calls;
                writer_pages += after[reason].pages - before[reason].pages;
                arm_calls += armed[NVN_DIRTY_MP_GUARD].calls - after[NVN_DIRTY_MP_GUARD].calls;
            }
        }
        printf("mprotect-bench %s: units=%d writes/frame=%d frames=%d writer_calls/frame=%.2f writer_pages/frame=%.2f "
               "guard_calls/frame=%.2f writer_cpu_ns/write=%.2f guard_cpu_ns/frame=%.2f\n",
               name, UNITS, WRITES, FRAMES - WARMUP, writer_calls / (double)(FRAMES - WARMUP),
               writer_pages / (double)(FRAMES - WARMUP), arm_calls / (double)(FRAMES - WARMUP),
               writer_ns / (double)((FRAMES - WARMUP) * WRITES), arm_ns / (double)(FRAMES - WARMUP));
    }
    /* Exact GPU writeback neighbours keep independent dirty authority even inside a combined RW unit. */
    size_t pg = (size_t)getpagesize();
    uint8_t *exact = memory + pg, *writer = memory + UNIT / 2;
    nvn_dirty_protect_exact(exact, 1);
    uint64_t stamp = nvn_dirty_now(); nvn_dirty_mp_count before[NVN_DIRTY_MP_REASON_COUNT], after[NVN_DIRTY_MP_REASON_COUNT];
    nvn_dirty_mp_reason_stats(before); lag_guest_store(writer, 0x75); nvn_dirty_mp_reason_stats(after);
    CHECK(after[NVN_DIRTY_MP_FAULT].calls - before[NVN_DIRTY_MP_FAULT].calls == 2,
          "exact-watch hole is the only barrier splitting an interior writer's permission run");
    CHECK(!nvn_dirty_since(exact, 1, stamp) && nvn_dirty_since(writer, 1, stamp),
          "combined unprotect preserves exact-neighbour dirty generation");
    nvn_dirty_mp_reason_stats(before); lag_guest_store(exact, 0x62); nvn_dirty_mp_reason_stats(after);
    CHECK(after[NVN_DIRTY_MP_FAULT].calls - before[NVN_DIRTY_MP_FAULT].calls == 1 &&
          nvn_dirty_since(exact, 1, stamp), "exact page remains protected and faults on its own actual write");
    direct_watch_bench();
    nvn_dirty_mp_count totals[NVN_DIRTY_MP_REASON_COUNT]; nvn_dirty_mp_reason_stats(totals);
    uint64_t calls = 0, pages = 0;
    for (unsigned i = 0; i < NVN_DIRTY_MP_REASON_COUNT; i++) { calls += totals[i].calls; pages += totals[i].pages; }
    nvn_dirty_mp_stats(&calls1, &pages1, &ns1);
    CHECK(calls == calls1 && pages == pages1, "per-thread/reason syscall counters exactly cover aggregate calls/pages");
    nvn_dirty_mp_report(300);
    nvn_dirty_lag_batch_destroy(&batch); nvn_dirty_remove_range(memory); munmap(memory, (size_t)UNITS * UNIT);
    printf("%s: mprotect-bench %d checks, %d failed (CPU protocol fixture, not device mmap-lock timing)\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

/* Lock-free index readers against a concurrent writer: a churner thread adds/removes one-page ranges over pages
   2.. while this thread looks up page 0 (tracked throughout) and page 1 (never tracked). Torn or stale index reads
   may fall back to the scan but must never change either answer. Run under TSan as well. */
typedef struct { uint8_t *memory; size_t pg, pages; _Atomic int stop; } dirty_churn;
static void *dirty_churn_thread(void *arg) {
    dirty_churn *c = arg; uint64_t rng = 0x9e3779b97f4a7c15u; uint8_t live[512] = {0};
    size_t n = c->pages < 512 ? c->pages : 512;
    while (!atomic_load(&c->stop)) {
        rng = rng * 6364136223846793005u + 1442695040888963407u; size_t q = 2 + (rng >> 33) % (n - 2);
        if (live[q]) nvn_dirty_remove_range(c->memory + q * c->pg); else nvn_dirty_add_range(c->memory + q * c->pg, c->pg);
        live[q] ^= 1;
    }
    for (size_t q = 2; q < n; q++) if (live[q]) nvn_dirty_remove_range(c->memory + q * c->pg);
    return NULL;
}
static void dirty_lookup_concurrent(uint8_t *memory, size_t pg, size_t pages) {
    nvn_dirty_add_range(memory, pg);
    dirty_churn c = {memory, pg, pages, 0}; pthread_t th;
    if (pthread_create(&th, NULL, dirty_churn_thread, &c)) { CHECK(0, "concurrent index churner starts"); nvn_dirty_remove_range(memory); return; }
    unsigned wrong = 0, reads = 0;
    for (unsigned i = 0; i < 200000; i++, reads += 2) {
        wrong += !nvn_dirty_tracked(memory + (i & 255), 1);
        wrong += nvn_dirty_tracked(memory + pg + (i & 255), 1);
    }
    atomic_store(&c.stop, 1); pthread_join(th, NULL);
    nvn_dirty_remove_range(memory);
    CHECK(!wrong, "lock-free index lookups stay exact under concurrent add/remove (%u of %u wrong)", wrong, reads);
}
/* More pools than the recent-ring capacity, with 64-KiB window hints and reused slots.
   Hints may accelerate candidate selection but must never extend a pool or retain a dead incarnation. */
static void dirty_lookup_checks(void) {
    enum { POOLS = 32, STRIDE = 65536 };
    size_t pg = (size_t)getpagesize(), bytes = (size_t)POOLS * STRIDE;
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED, "dirty lookup fixture mapping");
    if (memory == MAP_FAILED) return;
    for (unsigned i = 0; i < POOLS; i++) nvn_dirty_add_range(memory + i * STRIDE, pg);
    for (unsigned repeat = 0; repeat < 3; repeat++) for (unsigned i = 0; i < POOLS; i++) {
        uint8_t *p = memory + ((i * 17) % POOLS) * STRIDE;
        CHECK(nvn_dirty_tracked(p + 16, pg - 16) && !nvn_dirty_tracked(p + 16, pg),
              "dirty lookup %u/%u preserves exact half-open extent", repeat, i);
        CHECK(!nvn_dirty_since(p, pg, nvn_dirty_now()) && nvn_dirty_since(p + pg, 1, nvn_dirty_now()),
              "dirty lookup %u/%u preserves tracked/untracked answers", repeat, i);
    }
    for (unsigned i = 0; i < POOLS; i++) nvn_dirty_remove_range(memory + i * STRIDE);
    for (unsigned i = 0; i < POOLS; i++) {
        uint8_t *p = memory + i * STRIDE;
        CHECK(!nvn_dirty_tracked(p, 1), "dead cached pool %u is not tracked", i);
        nvn_dirty_add_range(p + pg, pg);
        CHECK(!nvn_dirty_tracked(p, 1) && nvn_dirty_tracked(p + pg, pg),
              "reused cached pool slot %u has only its new bounds", i);
    }
    for (unsigned i = 0; i < POOLS; i++) nvn_dirty_remove_range(memory + i * STRIDE + pg);
    /* A range added over a page just found untracked (twice) must be seen at once, and so must its removal
       (the sorted live-range index and the lag_hot interval cache are both invalidated by adds/removes). */
    for (unsigned i = 0; i < POOLS; i++) {
        uint8_t *p = memory + i * STRIDE + 2 * pg;
        CHECK(!nvn_dirty_tracked(p, 1) && !nvn_dirty_tracked(p, 1) && !nvn_dirty_lag_hot(p, 2 * pg) &&
              !nvn_dirty_lag_hot(p, 2 * pg), "untracked page %u is untracked on repeated lookups", i);
        nvn_dirty_add_range(p, pg);
        CHECK(nvn_dirty_tracked(p, 1), "range added over cached-untracked page %u is found", i);
        nvn_dirty_remove_range(p);
        CHECK(!nvn_dirty_tracked(p, 1), "removed range over page %u is untracked again", i);
    }
    /* Sorted index against a shadow model: random one-page adds/removes over all pages of the mapping, with a lookup
       of a random page (hit or miss) after every operation and a full sweep every 512 operations. */
    size_t pages = bytes / pg; uint8_t *shadow = calloc(pages, 1); unsigned bad = 0, lookups = 0; uint64_t rng = 0x2545f4914f6cdd1du;
    if (!shadow) abort();
    for (unsigned op = 0; op < 20000; op++) {
        rng = rng * 6364136223846793005u + 1442695040888963407u; size_t q = (rng >> 33) % pages;
        if (shadow[q]) { nvn_dirty_remove_range(memory + q * pg); shadow[q] = 0; }
        else { nvn_dirty_add_range(memory + q * pg, pg); shadow[q] = (uint8_t)nvn_dirty_tracked(memory + q * pg, 1); bad += !shadow[q]; }
        rng = rng * 6364136223846793005u + 1442695040888963407u; size_t probe = (rng >> 33) % pages;
        bad += nvn_dirty_tracked(memory + probe * pg + 7, 1) != shadow[probe]; lookups++;
        if (op % 512 == 0) for (size_t k = 0; k < pages; k++, lookups++) bad += nvn_dirty_tracked(memory + k * pg, 1) != shadow[k];
    }
    CHECK(!bad, "sorted range index matches the shadow model over %u lookups (%u mismatches)", lookups, bad);
    for (size_t k = 0; k < pages; k++) if (shadow[k]) nvn_dirty_remove_range(memory + k * pg);
    free(shadow);
    dirty_lookup_concurrent(memory, pg, pages);
    munmap(memory, bytes);
}

/* Deliberately exits before mtl_init: made-up protocol tickets must never enter a real GPU suite. All measurements
   use the production tracker, including actual mprotect calls. This is a CPU microbenchmark, not GPU/frame timing. */
static int lag_bench(void) {
    enum { NINPUT = 7000, NPOOL = 2000, NJOB = 230, MAX_SHARDS = 4, NREPEAT = 32 };
    const char *threads_env = getenv("TOTK_VK_REC_THREADS");
    int configured = threads_env ? atoi(threads_env) : MAX_SHARDS;
    unsigned nshards = configured < 1 ? 1 : configured > MAX_SHARDS ? MAX_SHARDS : (unsigned)configured;
    size_t pg = (size_t)getpagesize(), probe_len = 64u << 20;
    uint8_t *probe = mmap(NULL, probe_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED) abort();
    nvn_dirty_lag_enable(); nvn_dirty_add_range(probe, probe_len);
    dirty_lookup_checks();
    uintptr_t lo, hi;
    if (!nvn_dirty_lag_bounds(probe, 1, &lo, &hi) || hi - lo >= probe_len) {
        fprintf(stderr, "lag-bench: cannot determine an unclipped dirty unit below 64 MiB\n"); return 1;
    }
    size_t unit = hi - lo;
    nvn_dirty_remove_range(probe); munmap(probe, probe_len);
    lag_batch_checks(unit, 2);
    size_t stride = unit + pg, mapping_len = NPOOL * stride + 2 * pg;
    uint8_t *mapping = mmap(NULL, mapping_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) abort();
    uint8_t *base = mapping + pg;
    if (unit > pg && (uintptr_t)base % unit == 0) base += pg;
    for (unsigned i = 0; i < NPOOL; i++) nvn_dirty_add_range(base + i * stride, unit - (unit > pg && !(i % 4) ? pg : 0));
    lag_bench_input *inputs = calloc(NINPUT, sizeof *inputs);
    if (!inputs) abort();
    for (unsigned i = 0; i < NINPUT; i++) {
        unsigned pool = (uint64_t)i * NPOOL / NINPUT;
        inputs[i] = (lag_bench_input){base + pool * stride + 32 + (i % 3) * 16, 128,
            NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_COLD | (i % 7 ? 0 : NVN_DIRTY_LAG_ALIAS), 128 + (i % 5) * 128,
            pool % NJOB};
    }
    int live, slots; uint64_t adds;
    nvn_dirty_slot_stats(&live, &slots, &adds);
    printf("lag-bench layout: inputs=%u unique_units=%u pools=%u live=%d used_slots=%d jobs=%u shards=%u "
           "system_page=%zu dirty_unit=%zu one_range_relative_unit/pool clipped_tails=%u mapping_bytes=%zu\n",
           NINPUT, NPOOL, NPOOL, live, slots, NJOB, nshards, pg, unit, unit > pg ? NPOOL / 4 : 0, mapping_len);
    printf("lag-bench scope: fresh process, protocol-only tickets, no Vulkan/GPU work; collect/merge serial CPU, "
           "arm parallel; cold=writable+new batch storage, warm=ARMED ticket increase+retained storage\n");
    printf("lag-bench storage: cold collect/merge include lazy allocation and growth, reported separately from arm; "
           "warm reuses identical-capacity storage; no allocator-only timing is inferred; "
           "TOTK_VK_REC_THREADS=%s (benchmark clamps to 1..4, absent defaults to 4)\n",
           threads_env ? threads_env : "unset");
    printf("lag-bench accounting: aggregate_cpu=collect+merge+sum(shard CPU); critical_cpu_model="
           "collect+merge+max(shard CPU), not measured frame time; arm_dispatch_wall includes release/join, "
           "excludes thread creation; reset is included in collect\n");
    nvn_dirty_lag_batch jobs[NJOB] = {{0}}, merged = {0};
    for (unsigned phase = 0; phase < 2; phase++) {
        const char *name = phase ? "warm" : "cold";
        uint64_t ticket = 10 + phase * 2, r0, u0, promotions, demotions, r1, u1, mc0, mpages, mns, mc1;
        nvn_dirty_lag_sticky_stats(&r0, &u0, &promotions, &demotions);
        nvn_dirty_mp_stats(&mc0, &mpages, &mns);
        uint64_t cpu = lag_bench_cpu_ns(), wall = host_now_ns(); int ok = 1;
        for (unsigned i = 0; i < NINPUT; i++) ok &= nvn_dirty_lag_register(inputs[i].addr, inputs[i].len,
            ticket, inputs[i].reason, inputs[i].max_range);
        uint64_t legacy_wall = host_now_ns() - wall, legacy_cpu = lag_bench_cpu_ns() - cpu;
        nvn_dirty_lag_sticky_stats(&r1, &u1, &promotions, &demotions);
        nvn_dirty_mp_stats(&mc1, &mpages, &mns);
        CHECK(ok, "lag-bench %s legacy ranges all register", name);
        printf("lag-bench %s legacy: ns/inputrange=%.2f wall_us=%.2f aggregate_cpu_us=%.2f critical_cpu_us=%.2f "
               "rearms=%llu ticket_updates=%llu mprotect_calls=%llu\n", name, (double)legacy_cpu / NINPUT,
               legacy_wall / 1000.0, legacy_cpu / 1000.0, legacy_cpu / 1000.0,
               (unsigned long long)(r1 - r0), (unsigned long long)(u1 - u0), (unsigned long long)(mc1 - mc0));
        nvn_dirty_lag_complete(ticket);
        /* Give both cold algorithms genuinely writable units. Do not include this fixture reset in either result. */
        if (!phase) for (unsigned i = 0; i < NPOOL; i++) dm_guest_write_prepare(base + i * stride, 1);
        ticket++;
        cpu = lag_bench_cpu_ns(); wall = host_now_ns();
        for (unsigned i = 0; i < NJOB; i++) nvn_dirty_lag_batch_reset(&jobs[i]);
        nvn_dirty_lag_batch_reset(&merged); ok = 1;
        for (unsigned i = 0; i < NINPUT; i++) ok &= nvn_dirty_lag_batch_add(&jobs[inputs[i].job], inputs[i].addr,
            inputs[i].len, inputs[i].reason, inputs[i].max_range);
        uint64_t collect_wall = host_now_ns() - wall, collect_cpu = lag_bench_cpu_ns() - cpu;
        size_t refs = 0;
        cpu = lag_bench_cpu_ns(); wall = host_now_ns();
        for (unsigned i = 0; i < NJOB; i++) {
            refs += nvn_dirty_lag_batch_count(&jobs[i]);
            ok &= nvn_dirty_lag_batch_merge(&merged, &jobs[i]);
        }
        uint64_t merge_wall = host_now_ns() - wall, merge_cpu = lag_bench_cpu_ns() - cpu;
        CHECK(ok && nvn_dirty_lag_batch_count(&merged) == NPOOL, "lag-bench %s collection/merge has 2000 unique units", name);
        _Atomic int start = 0; lag_bench_arm arms[MAX_SHARDS]; pthread_t threads[MAX_SHARDS];
        for (unsigned i = 0; i < nshards; i++) {
            arms[i] = (lag_bench_arm){.batch = &merged, .ticket = ticket, .shard = i, .nshards = nshards, .start = &start};
            if (pthread_create(&threads[i], NULL, lag_bench_arm_run, &arms[i])) abort();
        }
        nvn_dirty_lag_sticky_stats(&r0, &u0, &promotions, &demotions);
        nvn_dirty_mp_stats(&mc0, &mpages, &mns);
        wall = host_now_ns(); atomic_store_explicit(&start, 1, memory_order_release);
        uint64_t arm_cpu = 0, arm_max_cpu = 0, arm_max_wall = 0;
        for (unsigned i = 0; i < nshards; i++) {
            pthread_join(threads[i], NULL); ok &= arms[i].ok;
            arm_cpu += arms[i].cpu_ns;
            if (arm_max_cpu < arms[i].cpu_ns) arm_max_cpu = arms[i].cpu_ns;
            if (arm_max_wall < arms[i].wall_ns) arm_max_wall = arms[i].wall_ns;
        }
        uint64_t arm_wall = host_now_ns() - wall;
        nvn_dirty_lag_sticky_stats(&r1, &u1, &promotions, &demotions);
        nvn_dirty_mp_stats(&mc1, &mpages, &mns);
        CHECK(ok, "lag-bench %s disjoint registration shards all succeed", name);
        printf("lag-bench %s batch: collect_ns/inputrange=%.2f collect_wall_us=%.2f merge_us=%.2f merge_ns/ref=%.2f "
               "arm_ns/uniqueunit=%.2f arm_cpu_us=%.2f arm_max_cpu_us=%.2f arm_max_wall_us=%.2f arm_dispatch_wall_us=%.2f "
               "aggregate_cpu_us=%.2f critical_cpu_model_us=%.2f observed_pipeline_wall_us=%.2f unique_bytes=%llu "
               "rearms=%llu ticket_updates=%llu mprotect_calls=%llu\n", name, (double)collect_cpu / NINPUT,
               collect_wall / 1000.0, merge_cpu / 1000.0, refs ? (double)merge_cpu / refs : 0,
               (double)arm_cpu / NPOOL, arm_cpu / 1000.0, arm_max_cpu / 1000.0, arm_max_wall / 1000.0,
               arm_wall / 1000.0, (collect_cpu + merge_cpu + arm_cpu) / 1000.0,
               (collect_cpu + merge_cpu + arm_max_cpu) / 1000.0, (collect_wall + merge_wall + arm_wall) / 1000.0,
               (unsigned long long)nvn_dirty_lag_batch_bytes(&merged), (unsigned long long)(r1 - r0),
               (unsigned long long)(u1 - u0), (unsigned long long)(mc1 - mc0));
        nvn_dirty_lag_complete(ticket);
        cpu = lag_bench_cpu_ns(); wall = host_now_ns();
        for (unsigned repeat = 0; repeat < NREPEAT; repeat++) for (unsigned i = 0; i < NINPUT; i++)
            ok &= nvn_dirty_lag_register(inputs[i].addr, inputs[i].len, ticket, inputs[i].reason, inputs[i].max_range);
        uint64_t dup_legacy_cpu = lag_bench_cpu_ns() - cpu, dup_legacy_wall = host_now_ns() - wall;
        cpu = lag_bench_cpu_ns(); wall = host_now_ns();
        for (unsigned repeat = 0; repeat < NREPEAT; repeat++)
            ok &= nvn_dirty_lag_batch_register(&merged, ticket, 0, 1);
        uint64_t dup_batch_cpu = lag_bench_cpu_ns() - cpu, dup_batch_wall = host_now_ns() - wall;
        nvn_dirty_lag_sticky_stats(&r0, &u0, &promotions, &demotions);
        nvn_dirty_mp_stats(&mc0, &mpages, &mns);
        CHECK(ok && r0 == r1 && u0 == u1 && mc0 == mc1, "lag-bench equal-ticket duplicates do no rearm/update/mprotect");
        printf("lag-bench %s duplicate_equal_ticket: repeats=%u legacy_ns/inputrange=%.2f batch_ns/uniqueunit=%.2f "
               "legacy_cpu_us/pass=%.2f batch_cpu_us/pass=%.2f legacy_wall_us/pass=%.2f batch_wall_us/pass=%.2f\n",
               name, NREPEAT, (double)dup_legacy_cpu / (NREPEAT * NINPUT), (double)dup_batch_cpu / (NREPEAT * NPOOL),
               (double)dup_legacy_cpu / (NREPEAT * 1000), (double)dup_batch_cpu / (NREPEAT * 1000),
               (double)dup_legacy_wall / (NREPEAT * 1000), (double)dup_batch_wall / (NREPEAT * 1000));
    }
    for (unsigned i = 0; i < NJOB; i++) nvn_dirty_lag_batch_destroy(&jobs[i]);
    nvn_dirty_lag_batch_destroy(&merged);
    for (unsigned i = 0; i < NPOOL; i++) nvn_dirty_remove_range(base + i * stride);
    free(inputs); munmap(mapping, mapping_len);
    printf("lag-bench %s: %d checks, %d failed (CPU observations only)\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

static unsigned vma_range_count(const void *address, size_t size) {
    FILE *file = fopen("/proc/self/maps", "re"); if (!file) return 0;
    unsigned count = 0; unsigned long lo, hi; char line[1024];
    while (fgets(line, sizeof line, file)) if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
        lo < (uintptr_t)address + size && hi > (uintptr_t)address) count++;
    fclose(file); return count;
}
/* Fault each differently protected piece before restoring it: private anonymous VMAs can retain distinct
   anon_vmas, but guest reservations are shared anonymous. Report both rather than assuming reversibility. */
static int vma_merge_checks(void) {
    const size_t bytes = 64u << 20, unit = 65536;
    for (int shared = 0; shared <= 1; shared++) {
        uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS, -1, 0);
        CHECK(memory != MAP_FAILED, "merge fixture maps %s memory", shared ? "shared" : "private");
        if (memory == MAP_FAILED) continue;
        unsigned before = vma_range_count(memory, bytes); int ok = 1;
        for (size_t i = 0; i < bytes; i += unit) ok &= !mprotect(memory + i, unit / 2, PROT_READ);
        CHECK(ok, "split fixture protection succeeds");
        for (size_t i = 0; i < bytes; i += unit) {
            volatile uint8_t read = memory[i]; (void)read; memory[i + unit / 2] = 1;
        }
        unsigned split = vma_range_count(memory, bytes);
        for (size_t i = 0; i < bytes; i += unit) ok &= !mprotect(memory + i, unit / 2, PROT_READ | PROT_WRITE);
        unsigned restored = vma_range_count(memory, bytes);
        ok &= !mprotect(memory, bytes, PROT_READ | PROT_WRITE);
        unsigned whole = vma_range_count(memory, bytes);
        CHECK(ok, "piecewise and whole-range writable restoration succeeds");
        CHECK(before && split >= bytes / unit, "fixture creates many actual VMAs (%u -> %u)", before, split);
        CHECK(!shared || whole <= before + 1, "shared guest-style pieces re-merge after faults (%u -> %u)", before, whole);
        printf("VMA merge %s: before/split+fault/piece-restored/whole-restored %u/%u/%u/%u\n",
            shared ? "shared-anon" : "private-anon", before, split, restored, whole);
        munmap(memory, bytes);
    }
    nvn_dirty_lag_vma_report();
    printf("%s: VMA merge %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

/* Pure tracker fixture: completed synthetic tickets, no Vulkan/device. A low test-only budget exercises
   the same protection refusal and drain used by the real queued-slot fallback without exhausting the host. */
static int lag_vma_checks(void) {
    const size_t bytes = 64u << 20, budget = 128;
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED, "VMA fixture maps its input"); if (memory == MAP_FAILED) return 1;
    nvn_dirty_lag_enable(); nvn_dirty_add_range(memory, bytes);
    uintptr_t lo, hi;
    CHECK(nvn_dirty_lag_bounds(memory, 1, &lo, &hi), "VMA fixture obtains the real dirty unit");
    size_t unit = hi - lo;
    if (!unit || unit > bytes / 1024) return 1;
    nvn_dirty_lag_batch batch = {0};
    for (int i = 127; i >= 0; i--) CHECK(nvn_dirty_lag_batch_add(&batch, memory + i * unit, 1, 1, 1), "collect reversed contiguous unit %d", i);
    CHECK(nvn_dirty_lag_batch_seal(&batch), "seal sorts a complete contiguous batch");
    uint64_t calls0, calls1, pages, ns;
    nvn_dirty_mp_stats(&calls0, &pages, &ns);
    CHECK(nvn_dirty_lag_batch_register(&batch, 1, 0, 1), "contiguous units register");
    nvn_dirty_mp_stats(&calls1, &pages, &ns);
    CHECK(calls1 - calls0 <= 2, "128 contiguous units use two bounded run mprotects, not 128 (%llu calls)", (unsigned long long)(calls1 - calls0));
    nvn_dirty_lag_complete(1); dm_guest_write_prepare(memory, 128 * unit);
    uint8_t *exact = memory + 3 * unit + getpagesize();
    uint64_t stamp = nvn_dirty_now(); nvn_dirty_protect_exact(exact, 1);
    uint8_t *texture = memory + 5 * unit + getpagesize();
    uint64_t texture_stamp = nvn_dirty_now(); nvn_dirty_protect(texture, 1);
    nvn_dirty_lag_vma_refresh(); uint64_t before[5], peak[5], drained[5]; nvn_dirty_lag_vma_stats(before);
    nvn_dirty_lag_test_vma_limit(budget); nvn_dirty_lag_complete(2);
    int admitted = 0, refused = 0;
    for (int i = 0; i < 1024; i += 2) {
        int ok = nvn_dirty_lag_register(memory + i * unit, 1, 2, 1, 1);
        admitted += ok; refused += !ok;
    }
    nvn_dirty_lag_vma_refresh(); nvn_dirty_lag_vma_stats(peak);
    CHECK(admitted && refused && peak[4], "sparse guards hit the real region budget and latch fallback (%d/%d)", admitted, refused);
    CHECK(peak[2] <= budget, "guarded-run credit remains bounded (%llu <= %zu)", (unsigned long long)peak[2], budget);
    CHECK(peak[0] <= before[0] + 2 * budget + 16, "actual VMAs remain bounded (%llu -> %llu)", (unsigned long long)before[0], (unsigned long long)peak[0]);
    nvn_dirty_lag_vma_drain(); nvn_dirty_lag_vma_stats(drained);
    CHECK(drained[0] <= before[0] + 4 && drained[2] == 2, "drain coalesces extra lag watches and retains baseline texture/exact watches (%llu maps, %llu runs)",
        (unsigned long long)drained[0], (unsigned long long)drained[2]);
    CHECK(!nvn_dirty_since(exact, 1, stamp), "pressure drain does not invent a CPU write to an exact GPU write-back page");
    lag_guest_store(exact, 0x7b);
    CHECK(nvn_dirty_since(exact, 1, stamp), "a real CPU write to that exact page remains tracked");
    nvn_dirty_protect_exact(exact, 1); stamp = nvn_dirty_now();
    CHECK(!nvn_dirty_since(exact, 1, stamp), "new exact watches still work in conservative fallback");
    lag_guest_store(exact, 0x6a); CHECK(nvn_dirty_since(exact, 1, stamp), "new fallback exact watches detect a real write");
    CHECK(!nvn_dirty_since(texture, 1, texture_stamp), "drain preserves the baseline texture dirty-watch stamp");
    lag_guest_store(texture, 0x42);
    CHECK(nvn_dirty_since(texture, 1, texture_stamp), "baseline texture watch still detects a real CPU write");
    texture_stamp = nvn_dirty_now(); nvn_dirty_protect(texture, 1);
    CHECK(!nvn_dirty_since(texture, 1, texture_stamp), "new baseline texture watches remain active after fallback");
    lag_guest_store(texture, 0x43);
    CHECK(nvn_dirty_since(texture, 1, texture_stamp), "new baseline watch detects later CPU texture reuse");
    CHECK(!nvn_dirty_lag_register(memory, 1, 3, 1, 1), "new GPU guards remain fail-closed after draining");
    void *probe = mmap(NULL, 8u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(probe != MAP_FAILED, "later mappings still succeed under guard pressure"); if (probe != MAP_FAILED) munmap(probe, 8u << 20);
    printf("VMA guard fixture: maps baseline/peak/drained %llu/%llu/%llu, admitted/refused %d/%d, budget %zu\n",
        (unsigned long long)before[0], (unsigned long long)peak[0], (unsigned long long)drained[0], admitted, refused, budget);
    nvn_dirty_remove_range(memory); munmap(memory, bytes); nvn_dirty_lag_batch_destroy(&batch);
    printf("%s: VMA guard %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}


static uint64_t run(void) { op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(rec); rn = 0; return mtl_sub_commit(); }

static int lag_vma_policy_checks(void) {
    nvn_dirty_lag_test_vma_sample(2600, 0);
    nvn_dirty_lag_enable();
    uint64_t initial[5], missing[5], world[5], pressure[5];
    nvn_dirty_lag_vma_stats(initial);
    CHECK(initial[0] == 2600 && initial[1] == 65530 && initial[3] && !initial[4],
        "unreadable Android sysctl uses Linux default without disabling lag");
    nvn_dirty_lag_test_vma_sample(0, 0); nvn_dirty_lag_vma_refresh();
    nvn_dirty_lag_vma_stats(missing);
    CHECK(missing[0] == initial[0] && missing[1] == initial[1] && missing[3] == initial[3] && !missing[4],
        "unavailable samples retain known counts/budget without replenishing credit or latching pressure");
    nvn_dirty_lag_test_vma_sample(48000, 65530); nvn_dirty_lag_vma_refresh();
    nvn_dirty_lag_vma_stats(world);
    CHECK(world[0] == 48000 && world[3] && !world[4],
        "world-load baseline above the obsolete fixed target retains real headroom");
    nvn_dirty_lag_test_vma_sample(61000, 65530); nvn_dirty_lag_vma_refresh();
    nvn_dirty_lag_vma_stats(pressure);
    CHECK(pressure[4], "true live mapping headroom pressure latches fallback");
    nvn_dirty_lag_test_vma_sample(2600, 65530); nvn_dirty_lag_vma_refresh();
    nvn_dirty_lag_vma_stats(pressure);
    CHECK(pressure[4], "completed lag-off cutover cannot oscillate back into capture");
    printf("%s: VMA policy %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

static int vma_wait_sample(uint64_t maps, int pressure) {
    uint64_t deadline = host_now_ns() + 5000000000ull, values[5];
    do {
        nvn_dirty_lag_vma_stats(values);
        if (values[0] == maps && values[4] == (uint64_t)pressure) return 1;
        usleep(1000);
    } while (host_now_ns() < deadline);
    return 0;
}
static int vma_wait_scans(uint64_t count, uint64_t values[4]) {
    uint64_t deadline = host_now_ns() + 5000000000ull;
    do {
        nvn_dirty_lag_vma_scan_stats(values, 0);
        if (values[0] >= count) return 1;
        usleep(1000);
    } while (host_now_ns() < deadline);
    return 0;
}
/* Isolated process: pressure is intentionally irreversible. Only startup samples synchronously;
   fake proc values are published under the sampler mutex, then the real worker must consume them. */
static int lag_vma_async_checks(void) {
    const struct { const char *value; unsigned expected; } periods[] = {
        {"500", 1000}, {"60000", 30000}, {"0", 1000}, {"-1", 1000},
        {"184467440737095516160", 30000}, {"1000", 1000}, {"30000", 30000},
        {"10000", 10000}, {"bad", 10000}, {NULL, 10000}
    };
    for (unsigned i = 0; i < sizeof periods / sizeof periods[0]; i++)
        CHECK(nvn_dirty_lag_test_vma_period(periods[i].value) == periods[i].expected,
            "scan period %s parses/clamps to %u ms", periods[i].value ? periods[i].value : "<unset>", periods[i].expected);
    setenv("TOTK_FRAME_LAG", "1", 1); setenv("TOTK_VMA_CLASSES", "1", 1);
    const uint64_t period_ns = 1000000000;
    setenv("TOTK_VMA_SCAN_MS", "500", 1);
    nvn_dirty_lag_test_vma_sample(2600, 65530);
    uint64_t worker_start = host_now_ns();
    mtl_init();
    CHECK(nvn_dirty_lag_enabled(), "lag startup enables the sampler");
    uint64_t reads = nvn_dirty_lag_test_vma_reads();
    CHECK(reads >= 2, "startup establishes an initial synchronous budget");
    nvn_dirty_lag_test_vma_sample(48000, 65530);
    CHECK(vma_wait_sample(48000, 0), "periodic worker updates maps without any refresh/request on caller");
    uint64_t values[5]; nvn_dirty_lag_vma_stats(values);
    CHECK(values[3] == (65530 - 48000 - 65530 / 8) / 2, "worker publishes the headroom-minus-reserve run limit");
    uint64_t scans[4];
    CHECK(vma_wait_scans(1, scans), "worker records completed scan telemetry");
    CHECK(scans[0] && !scans[1] && scans[2] >= scans[3] && scans[3],
        "periodic scan records count, no epoch request, and total/max duration");
    uint64_t first_scans = scans[0];

    nvn_dirty_lag_test_vma_sample(49000, 65530);
    { void vk_mem_report(void); vk_mem_report(); } /* Includes opt-in classes, not just the default count report. */
    CHECK(nvn_dirty_lag_test_vma_reads() == reads, "vk_mem_report performs no maps read, including opt-in classes");
    CHECK(vma_wait_sample(49000, 0), "report request is serviced by the background sampler");
    /* Flood requests for longer than one test period: the old wake-on-every-epoch worker scanned
       repeatedly here. Include startup elapsed time so scheduler delays cannot falsely exceed the cap. */
    uint64_t storm_end = host_now_ns() + period_ns * 3 / 2;
    do {
        nvn_dirty_lag_vma_request(0); nvn_dirty_lag_vma_request(1);
        usleep(1000);
    } while (host_now_ns() < storm_end);
    nvn_dirty_lag_vma_scan_stats(scans, 0);
    CHECK(scans[0] + first_scans <= (host_now_ns() - worker_start) / period_ns,
        "epoch/classification request storms cannot exceed one scan per configured period");
    CHECK(scans[0] && scans[1] && scans[1] <= scans[0] && scans[2] >= scans[3] && scans[3],
        "coalesced epoch scans are counted once with worker-owned total/max timing");

    uint8_t *memory = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED, "epoch request fixture maps input");
    if (memory != MAP_FAILED) {
        nvn_dirty_add_range(memory, 65536);
        nvn_dirty_lag_test_vma_sample(50000, 65530);
        nvn_dirty_lag_batch empty = {0};
        uint64_t before_epoch[4]; nvn_dirty_lag_vma_scan_stats(before_epoch, 0);
        CHECK(nvn_dirty_lag_batch_register(&empty, 1, 0, 1), "epoch-changing registration only requests a refresh");
        CHECK(nvn_dirty_lag_test_vma_reads() == reads, "recording-path epoch change performs no maps read");
        CHECK(vma_wait_sample(50000, 0), "worker services the epoch-change request");
        CHECK(vma_wait_scans(before_epoch[0] + 1, scans), "epoch-requested scan telemetry completes");
        nvn_dirty_remove_range(memory); munmap(memory, 65536);
    }
    nvn_dirty_lag_test_vma_sample(61000, 65530);
    CHECK(vma_wait_sample(61000, 1), "periodic worker latches pressure with no caller-side refresh");
    nvn_dirty_lag_test_vma_sample(2600, 65530);
    CHECK(vma_wait_sample(2600, 1), "later async samples cannot clear the pressure latch");
    CHECK(nvn_dirty_lag_test_vma_reads() == reads, "all post-startup sampling belongs to the worker");
    nvn_dirty_lag_vma_scan_stats(scans, 1);
    CHECK(scans[0] && scans[1] <= scans[0] && scans[2] >= scans[3] && scans[3],
        "report reset returns coherent interval count/subset/total/max telemetry");
    printf("%s: VMA async %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
static void arena_check_copy(VkBuffer source, VkDeviceSize offset, VkBuffer destination, int index) {
    VkBufferCopy copy = {.srcOffset = offset, .dstOffset = (VkDeviceSize)index * 256, .size = 256};
    vkCmdCopyBuffer(vk_cb(), source, destination, 1, &copy);
}
uint64_t vk_test_slot_arena_bytes(void);
/* TOTK_VK_STAGING_RETAIN_MB: a 45 MiB burst of staging in one slot (five 9 MiB uploads' worth), then 16 quiet
   submissions through every slot. Retained slot-arena bytes before/after; with a cap, the burst slot keeps at most it. */
static int staging_retain_checks(void) {
    mtl_init();
    const char *e = getenv("TOTK_VK_STAGING_RETAIN_MB"); long cap = e && *e ? atol(e) : -1;
    uint64_t base = vk_test_slot_arena_bytes();
    mtl_sub_begin();
    for (int i = 0; i < 5; i++) { VkBuffer b; VkDeviceSize o; memset(vk_transient(9u << 20, 256, &b, &o), 0x30 + i, 256); }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "staging burst completes");
    uint64_t burst = vk_test_slot_arena_bytes();
    for (int i = 0; i < 16; i++) {
        mtl_sub_begin(); VkBuffer b; VkDeviceSize o; memset(vk_transient(1u << 20, 256, &b, &o), 0x71, 256);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "quiet submission %d completes", i);
    }
    uint64_t settled = vk_test_slot_arena_bytes();
    printf("staging-retain cap=%ld MiB/slot: slot arenas %.1f MiB before, %.1f MiB after the burst, %.1f MiB after 16 quiet submissions\n",
        cap, base / 1048576.0, burst / 1048576.0, settled / 1048576.0);
    if (cap < 0) CHECK(settled >= (45u << 20), "without a cap the burst slot keeps its high-water (%.1f MiB)", settled / 1048576.0);
    else CHECK(settled <= (uint64_t)8 * ((uint64_t)(cap < 8 ? 8 : cap) << 20), "each slot keeps at most the cap (%.1f MiB)", settled / 1048576.0);
    printf("%s: staging retain %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
static int arena_bo_checks(void) {
    setenv("TOTK_FRAME_LAG", "1", 1); setenv("TOTK_VK_LAG_MODE", "copy", 1);
    mtl_init();
    VkBuffer destination;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &destination));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, destination, &req);
    int type = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) DIE("arena test needs coherent host-visible memory");
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    VkDeviceMemory memory; VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &memory));
    VKCHECK(vkBindBufferMemory(V.dev, destination, memory, 0));
    uint8_t *out; VKCHECK(vkMapMemory(V.dev, memory, 0, VK_WHOLE_SIZE, 0, (void **)&out));
    uint64_t before[VK_MEMORY_PURPOSES], peak[VK_MEMORY_PURPOSES], compact[VK_MEMORY_PURPOSES];
    vk_memory_counts(before);
    mtl_sub_begin();
    for (int i = 0; i < 5; i++) {
        VkBuffer buffer; VkDeviceSize offset;
        void *ptr = vk_transient(9u << 20, 256, &buffer, &offset);
        memset(ptr, 0x30 + i, 256); arena_check_copy(buffer, offset, destination, i);
    }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "fragmented slot arena copies complete");
    vk_memory_counts(peak);
    CHECK(peak[VK_MEMORY_SLOT] == before[VK_MEMORY_SLOT] + 5, "slot burst creates five labeled BOs");
    for (int i = 0; i < 5; i++) {
        int bad = 0; for (int k = 0; k < 256; k++) bad += out[256 * i + k] != 0x30 + i;
        CHECK(!bad, "slot burst BO %d retains copied bytes", i);
    }
    for (int i = 0; i < 7; i++) { mtl_sub_begin(); (void)vk_cb(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "rotate idle slot %d", i); }
    mtl_sub_begin();
    VkBuffer buffer; VkDeviceSize offset; void *ptr = vk_transient(9u << 20, 256, &buffer, &offset);
    memset(ptr, 0x71, 256); arena_check_copy(buffer, offset, destination, 0);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "compacted slot arena copies complete");
    vk_memory_counts(compact);
    CHECK(compact[VK_MEMORY_SLOT] == before[VK_MEMORY_SLOT] + 1, "retired slot compacts five BOs into one warm high-water BO");
    CHECK(out[0] == 0x71 && out[255] == 0x71, "compacted slot copies retain bytes");
    const size_t bytes = 32u << 20;
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(guest != MAP_FAILED, "snapshot arena test maps guest pool");
    if (guest == MAP_FAILED) return 1;
    mtl_pool_add(guest, bytes);
    tex_t tex = {.serial = 90001, .mem = guest};
    tex.d.target = 10; tex.d.format = 0x25; tex.d.width = 64; mtl_reg_tex(91, &tex, NULL);
    mtl_sub_begin();
    for (int i = 0; i < 3; i++) {
        memset(guest + (size_t)i * (9u << 20), 0x40 + i, 256);
        VkDescriptorBufferInfo frozen;
        CHECK(vk_lag_read((uint64_t)(uintptr_t)(guest + (size_t)i * (9u << 20)), (9u << 20) - (i == 2 ? 256 : 0), 256, &frozen, VK_LAG_COPY),
            "snapshot burst owns input %d", i);
        arena_check_copy(frozen.buffer, frozen.offset, destination, i);
    }
    CHECK(vk_tbuf_by_id(91, 0) != VK_NULL_HANDLE, "snapshot arena owns a cached texel view before retirement");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "fragmented snapshot copies complete");
    vk_memory_counts(peak);
    CHECK(peak[VK_MEMORY_SNAPSHOT] == before[VK_MEMORY_SNAPSHOT] + 3, "snapshot burst creates three labeled BOs");
    for (int i = 0; i < 3; i++) {
        int bad = 0; for (int k = 0; k < 256; k++) bad += out[256 * i + k] != 0x40 + i;
        CHECK(!bad, "snapshot burst BO %d retains copied bytes", i);
    }
    for (int i = 0; i < 4; i++) {
        mtl_sub_begin(); VkDescriptorBufferInfo frozen;
        if (i == 3) CHECK(vk_tbuf_by_id(91, 0) != VK_NULL_HANDLE, "texel resolution compacts its snapshot arena while holding the recursive resource lock");
        CHECK(vk_lag_read((uint64_t)(uintptr_t)(guest + 4096 + i * 256), 256, 256, &frozen, VK_LAG_COPY),
            "rotate snapshot arena %d", i);
        arena_check_copy(frozen.buffer, frozen.offset, destination, 0);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "rotated snapshot arena %d completes", i);
    }
    vk_memory_counts(compact);
    CHECK(compact[VK_MEMORY_SNAPSHOT] == before[VK_MEMORY_SNAPSHOT] + 4,
        "four snapshot arenas retain four BOs, not the fragmented burst's six");
    vk_record_test_costs warmed, steady;
    vk_record_test_costs_get(&warmed);
    CHECK(warmed.arena_compacts[0] == 1 && warmed.arena_compacts[1] == 1 &&
        warmed.arena_ns[0] && warmed.arena_ns[1], "arena instrumentation counts both actual consolidations and their cost");
    for (int frame = 0; frame < 64; frame++) {
        mtl_sub_begin();
        void *ptr = vk_transient(9u << 20, 256, &buffer, &offset);
        memset(ptr, 0x72, 256); arena_check_copy(buffer, offset, destination, 0);
        VkDescriptorBufferInfo frozen;
        CHECK(vk_lag_read((uint64_t)(uintptr_t)(guest + 4096), 256, 256, &frozen, VK_LAG_COPY),
            "warm snapshot frame %d owns its input", frame);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "warm arena frame %d completes", frame);
        CHECK(out[0] == 0x72 && out[255] == 0x72, "warm arena frame %d retains all copied bytes", frame);
    }
    vk_record_test_costs_get(&steady);
    CHECK(!memcmp(warmed.arena_compacts, steady.arena_compacts, sizeof warmed.arena_compacts) &&
        !memcmp(warmed.arena_ns, steady.arena_ns, sizeof warmed.arena_ns),
        "64 warmed frames incur zero slot/snapshot consolidation operations or timed work");
    printf("arena consolidation ns once slot/snapshot %llu/%llu; steady ns/frame 0/0\n",
        (unsigned long long)warmed.arena_ns[0], (unsigned long long)warmed.arena_ns[1]);
    vk_record_stats(300); /* exercise the real per300 diagnostic paths */
    mtl_pool_del(guest); munmap(guest, bytes);
    vkUnmapMemory(V.dev, memory); vkDestroyBuffer(V.dev, destination, NULL); vkFreeMemory(V.dev, memory, NULL);
    printf("%s: arena BO %d checks, %d failed; slot BOs %llu -> %llu, snapshot BOs %llu -> %llu\n",
        fails ? "FAILED" : "ok", checks, fails, (unsigned long long)5, (unsigned long long)compact[VK_MEMORY_SLOT],
        (unsigned long long)3, (unsigned long long)compact[VK_MEMORY_SNAPSHOT]);
    return fails != 0;
}
typedef struct { _Atomic int start, done; int tid; } kind_writer;
static void *kind_writer_run(void *arg) {
    kind_writer *w = arg;
    while (!atomic_load_explicit(&w->start, memory_order_acquire)) sched_yield();
    for (int i = 0; i < 20000; i++) {
        tex_t t = {.d = {.format = i & 1 ? 0x32 : 0x3a, .target = i & 1 ? 4 : 1}};
        mtl_reg_tex(w->tid, i % 3 ? &t : NULL, NULL);
    }
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}
/* No Vulkan/device initialization: measure the production registry lookup, grouped as 32 slots/draw.
   The wide case deliberately crosses the old 8192-entry thread cache's collision boundary. */
static int registry_kind_bench(void) {
    int f = -17, target = -19;
    CHECK(!vk_treg_kind(-1, &f, &target) && !vk_treg_kind(0, &f, &target) &&
        !vk_treg_kind(1 << 20, &f, &target) && f == -17 && target == -19,
        "invalid registry IDs leave outputs untouched");
    tex_t t = {.d = {.format = 0x3a, .target = 1}};
    view_t v = {.set_format = 1, .set_target = 1, .format = 0x32, .target = 4};
    mtl_reg_tex(7, &t, NULL);
    CHECK(vk_treg_kind(7, &f, &target) && f == 0x3a && target == 1, "registry kind without view");
    mtl_reg_tex(7, &t, &v);
    CHECK(vk_treg_kind(7, &f, &target) && f == 0x32 && target == 4, "view format and target override registry kind");
    v.set_format = v.set_target = 0; mtl_reg_tex(7, &t, &v);
    CHECK(vk_treg_kind(7, &f, &target) && f == 0x3a && target == 1, "unset view fields preserve texture kind");
    mtl_set_tex_pool_n(7);
    CHECK(!vk_treg_kind(7, &f, &target), "pool shrink hides an existing registration");
    mtl_set_tex_pool_n(8);
    CHECK(vk_treg_kind(7, &f, &target) && f == 0x3a && target == 1, "pool growth restores an existing registration");
    mtl_set_tex_pool_n(0);
    t.d.format = INT32_MIN; t.d.target = INT32_MAX; mtl_reg_tex(7, &t, NULL);
    CHECK(vk_treg_kind(7, &f, &target) && f == INT32_MIN && target == INT32_MAX, "full-width texture kind is preserved");
    mtl_reg_tex(7, NULL, NULL); f = -17; target = -19;
    CHECK(!vk_treg_kind(7, &f, &target) && f == -17 && target == -19, "unregister invalidates kind without writing outputs");
    kind_writer writer = {.tid = 7}; pthread_t thread;
    int started = !pthread_create(&thread, NULL, kind_writer_run, &writer), coherent = 1;
    CHECK(started, "registry writer starts");
    if (started) {
        atomic_store_explicit(&writer.start, 1, memory_order_release);
        do {
            if (vk_treg_kind(7, &f, &target) &&
                !((f == 0x32 && target == 4) || (f == 0x3a && target == 1))) coherent = 0;
        } while (!atomic_load_explicit(&writer.done, memory_order_acquire));
        pthread_join(thread, NULL);
    }
    CHECK(coherent, "concurrent registration never tears format/target pairs");
    enum { NSLOTS = 32, NDRAWS = 200000, NIDS = 16384 };
    for (int i = 1; i <= NIDS; i++) {
        t.d.format = i & 1 ? 0x32 : 0x3a; t.d.target = i % 11;
        mtl_reg_tex(i, &t, NULL);
    }
    uint64_t sum = 0;
    for (int wide = 0; wide < 2; wide++) {
        uint64_t start = host_now_ns();
        for (int draw = 0; draw < NDRAWS; draw++) for (int slot = 0; slot < NSLOTS; slot++) {
            int tid = wide ? 1 + ((draw * NSLOTS + slot) & (NIDS - 1)) : slot + 1;
            if (vk_treg_kind(tid, &f, &target)) sum += (unsigned)f + (unsigned)target;
        }
        uint64_t ns = host_now_ns() - start;
        printf("registry-kind-bench %s: %.2f ns/draw (%d slots), %.2f ns/lookup, checksum=%llu\n",
            wide ? "wide" : "hot", (double)ns / NDRAWS, NSLOTS, (double)ns / (NDRAWS * NSLOTS),
            (unsigned long long)sum);
    }
    printf("%s: registry kind %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
/* Real registered descriptors/views, but no draws in the timed region. All IDs alias one
   texture so the wide case isolates cache conflict resolution rather than image allocation. */
static int texture_cache_bench(void) {
    enum { NSLOTS = 32, NDRAWS = 100000, NIDS = 2048 };
    size_t bytes = 1u << 20;
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(guest != MAP_FAILED, "texture cache guest allocation");
    if (guest == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(guest, bytes);
    pool_t pool = {.mem = guest, .size = bytes, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t texture = {.d = {.target = 1, .format = 0x25, .width = 4, .height = 4, .depth = 1, .levels = 1},
        .mem = guest, .pool = &pool, .serial = 7001};
    nvn_tex_layout(&texture.d, &texture.L);
    for (int tid = 1; tid <= NIDS; tid++) mtl_reg_tex(tid, &texture, NULL);
    mtl_sub_begin();
    vk_tex *object = vk_tex_get(&texture);
    CHECK(object != NULL, "texture cache fixture realizes its image");
    if (!object) return 1;
    VkImageView expected = vk_tex_view(object, &texture, NULL, 0);
    int valid = expected != VK_NULL_HANDLE;
    for (int wide = 0; wide < 2; wide++) {
        for (int tid = 1; tid <= NIDS; tid++) { int want = 1; (void)vk_tex_by_id(tid, &want, NULL); }
        uint64_t memo0[5], memo1[5]; vk_tex_memo_counts(memo0);
        uint64_t start = host_now_ns();
        for (int draw = 0; draw < NDRAWS; draw++) for (int slot = 0; slot < NSLOTS; slot++) {
            int tid = wide ? 1 + ((draw * NSLOTS + slot) & (NIDS - 1)) : slot + 1, want = 1;
            vk_tex *resolved = NULL; VkImageView view = vk_tex_by_id(tid, &want, &resolved);
            valid &= view == expected && resolved == object;
        }
        uint64_t ns = host_now_ns() - start; vk_tex_memo_counts(memo1);
        uint64_t hits = memo1[0] - memo0[0], misses = memo1[1] - memo0[1];
        printf("texture-cache-bench %s: %.2f ns/draw (%d slots), %.2f ns/lookup; TOTK_VK_TEX_MEMO=%d hits %llu misses %llu\n",
            wide ? "wide" : "hot", (double)ns / NDRAWS, NSLOTS, (double)ns / (NDRAWS * NSLOTS), vk_tex_memo_on(),
            (unsigned long long)hits, (unsigned long long)misses);
        /* One submission, no registration: after the warm pass nearly every ID is a memo hit (direct-mapped: the wide set
           of 2048 IDs has a few slot collisions). Counts are published every 4096 lookups. */
        if (vk_tex_memo_on()) CHECK(hits + misses + 8192 >= (uint64_t)NDRAWS * NSLOTS && misses * 100 <= (uint64_t)NDRAWS * NSLOTS,
            "%s lookups hit the texture memo (%llu hits, %llu misses)", wide ? "wide" : "hot", (unsigned long long)hits, (unsigned long long)misses);
        else CHECK(!hits && !misses, "the texture memo stays out of the way while it is off");
    }
    CHECK(valid, "all hot/wide cached descriptors return the exact expected view and object");
    { uint64_t revision = vk_treg_revision(); mtl_reg_tex(2, &texture, NULL);
      CHECK(vk_tex_memo_on() ? vk_treg_revision() == revision : vk_treg_revision() != revision,
          "an unchanged re-registration keeps every revision with the texture memo on, and only then");
      int want2 = 1; CHECK(vk_tex_by_id(2, &want2, NULL) == expected, "an unchanged re-registration resolves the same view"); }
    view_t swizzle = {.set_swizzle = 1, .swz = {3, 2, 4, 5}};
    mtl_reg_tex(1, &texture, &swizzle);
    int want = 1; expected = vk_tex_view(object, &texture, &swizzle, 0);
    CHECK(expected && vk_tex_by_id(1, &want, NULL) == expected, "registration change invalidates the cached view");
    int array_target = 4; VkImageView array = vk_tex_by_id(1, &array_target, NULL);
    CHECK(array && array != expected && vk_tex_by_id(1, &want, NULL) == expected,
        "shader target selects distinct cached views for one descriptor");
    mtl_reg_tex(1, NULL, NULL); vk_tex *empty = object;
    CHECK(!vk_tex_by_id(1, &want, &empty) && !empty, "unregister clears a cached texture and view");
    mtl_set_tex_pool_n(NIDS);
    CHECK(!vk_tex_by_id(NIDS, &want, NULL), "pool bounds invalidate a cached descriptor");
    mtl_set_tex_pool_n(0);
    CHECK(vk_tex_by_id(NIDS, &want, NULL) != VK_NULL_HANDLE, "pool growth restores its cached descriptor");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "texture cache fixture GPU work completes");
    mtl_tex_release(&texture); mtl_pool_del(guest); munmap(guest, bytes);
    printf("%s: texture cache %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
/* Continuous creation/finalization of unrelated objects used to invalidate every
   sampled ID. Time only real lookups, not allocation, submission or the oracle. */
static int texture_churn_bench(void) {
    enum { FRAMES = 24, CHURN = 32, IDS = 1024, REPEATS = 1 };
    const size_t bytes = 1u << 20;
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(guest != MAP_FAILED, "texture churn guest allocation");
    if (guest == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(guest, bytes); nvn_dirty_add_range(guest, bytes);
    pool_t cpu = {.mem = guest, .size = bytes, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_t gpu = cpu; gpu.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    tex_t stable = {.d = {.target = 1, .format = 0x25, .width = 4, .height = 4, .depth = 1, .levels = 1},
        .mem = guest, .pool = &cpu, .serial = 7100};
    nvn_tex_layout(&stable.d, &stable.L);
    tex_t transient = stable; transient.mem = guest + 0x10000; transient.pool = &gpu;
    for (int id = 1; id <= IDS; id++) mtl_reg_tex(id, &stable, NULL);
    uint64_t before[3], after[3], resources0[7], resources[7], elapsed = 0, queries = 0;
    vk_tex_lookup_counters(before); vk_res_counters(resources0);
    vk_tex *object = NULL; VkImageView expected = VK_NULL_HANDLE; int valid = 1;
    for (int frame = 0; frame < FRAMES; frame++) {
        memset(guest, frame + 1, stable.L.size); /* actual CPU write to the watched mirrored texture */
        mtl_sub_begin();
        if (!object) {
            object = vk_tex_get(&stable); CHECK(object != NULL, "stable mirrored texture realizes");
            if (!object) return 1;
            expected = vk_tex_view(object, &stable, NULL, 0);
        }
        for (int churn = 0; churn < CHURN; churn++) {
            transient.serial = 7200 + frame * CHURN + churn;
            CHECK(vk_tex_get(&transient) != NULL, "churn object realizes");
            mtl_tex_release(&transient);
            uint64_t start = host_now_ns();
            for (int repeat = 0; repeat < REPEATS; repeat++) for (int id = 1; id <= IDS; id++) {
                int want = 1; vk_tex *resolved = NULL;
                VkImageView view = vk_tex_by_id(id, &want, &resolved);
                valid &= view == expected && resolved == object;
            }
            elapsed += host_now_ns() - start; queries += IDS * REPEATS;
        }
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "churn frame GPU work completes");
        uint8_t pixels[64] = {0};
        NVNcopyRegion region = {.width = 4, .height = 4, .depth = 1};
        mtl_read_texels(&stable, 0, 0, &region, pixels, 16, 64);
        int fresh = 1; for (size_t i = 0; i < sizeof pixels; i++) fresh &= pixels[i] == frame + 1;
        CHECK(fresh, "cached identity uploads current CPU-written content each frame");
    }
    vk_tex_lookup_counters(after); vk_res_counters(resources);
    CHECK(valid && expected, "churn preserves exact stable image view and owner");
    CHECK(resources[2] - resources0[2] == 1 && resources[3] - resources0[3] == FRAMES - 1,
        "one first upload and one reupload per CPU-written frame");
    printf("texture-churn-bench: %.3f ns/lookup, %llu/%llu slow (%.3f%%), %llu readiness, %llu uploads + %llu reuploads, %d frames, %d releases/frame\n",
        (double)elapsed / queries, (unsigned long long)(after[1] - before[1]),
        (unsigned long long)(after[0] - before[0]), 100.0 * (after[1] - before[1]) / (after[0] - before[0]),
        (unsigned long long)(after[2] - before[2]), (unsigned long long)(resources[2] - resources0[2]),
        (unsigned long long)(resources[3] - resources0[3]), FRAMES, CHURN);
    mtl_sub_begin(); int want = 1; vk_tex *released = object;
    CHECK(vk_tex_by_id(1, &want, NULL) == expected, "new submission retains the stable view");
    vk_tex_lookup_counters(before);
    vk_tex_pool_gone((uintptr_t)transient.mem, (uintptr_t)transient.mem + transient.L.size);
    CHECK(vk_tex_by_id(1, &want, NULL) == expected, "unrelated pool range preserves the stable view");
    vk_tex_lookup_counters(after);
    CHECK(after[1] - before[1] == (vk_off("texgen") ? 1u : 0u), "only the legacy cache invalidates an unrelated pool range");
    vk_tex_pool_gone((uintptr_t)stable.mem, (uintptr_t)stable.mem + stable.L.size);
    CHECK(vk_tex_by_id(1, &want, NULL) == expected, "overlapping pool range refreshes identity without changing its view");
    vk_tex_lookup_counters(resources);
    CHECK(resources[1] == after[1] + 1, "affected pool range forces a fresh texture lookup");
    mtl_tex_release(&stable);
    CHECK(vk_tex_by_id(1, &want, &object) && object != released, "released registered serial gets a new texture incarnation");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "recreated texture GPU work completes");
    for (int id = 1; id <= IDS; id++) mtl_reg_tex(id, NULL, NULL);
    mtl_tex_release(&stable); nvn_dirty_remove_range(guest); mtl_pool_del(guest); munmap(guest, bytes);
    printf("%s: texture churn %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
/* Actual mirrored uploads on the walker, not memcpy stand-ins. Sparse writes
   dirty one protection unit; full writes dirty the complete block-linear image. */
static int texture_upload_bench(void) {
    enum { FRAMES = 128, NTEX = 8, SIDE = 256, STRIDE = 1 << 20, HOT_READS = 32 };
    size_t bytes = NTEX * STRIDE;
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(guest != MAP_FAILED, "upload benchmark guest allocation");
    if (guest == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(guest, bytes); nvn_dirty_add_range(guest, bytes);
    pool_t pool = {.mem = guest, .size = bytes, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t textures[NTEX] = {{0}};
    for (int i = 0; i < NTEX; i++) {
        tex_t *t = &textures[i];
        t->d = (nvn_texdesc){.target = 1, .format = 0x25, .width = SIDE, .height = SIDE, .depth = 1, .levels = 1};
        t->mem = guest + (size_t)i * STRIDE; t->pool = &pool; t->serial = 9000 + i;
        nvn_tex_layout(&t->d, &t->L); memset(t->mem, 0x17, t->L.size);
        mtl_reg_tex(i + 1, t, NULL);
    }
    for (int full = 0; full < 2; full++) {
        uint64_t before[7], after[7], elapsed = 0, cpu_elapsed = 0;
        nvn_dirty_mp_count mp0[NVN_DIRTY_MP_REASON_COUNT], mp1[NVN_DIRTY_MP_REASON_COUNT];
        vk_res_counters(before); nvn_dirty_mp_reason_stats(mp0);
        for (int frame = 0; frame < FRAMES; frame++) {
            for (int i = 0; i < NTEX; i++) {
                if (full) memset(textures[i].mem, frame + 1, textures[i].L.size);
                else textures[i].mem[0] = frame + 1;
            }
            mtl_sub_begin();
            for (int i = 0; i < NTEX; i++) {
                int want = 1; uint64_t start = host_now_ns(), cpu_start = lag_bench_cpu_ns();
                CHECK(vk_tex_by_id(i + 1, &want, NULL) != VK_NULL_HANDLE, "CPU-written mirrored upload resolves");
                elapsed += host_now_ns() - start;
                cpu_elapsed += lag_bench_cpu_ns() - cpu_start;
            }
            vk_res_counters(after);
            for (int draw = 0; draw < HOT_READS; draw++) for (int i = 0; i < NTEX; i++) {
                int want = 1; (void)vk_tex_by_id(i + 1, &want, NULL);
            }
            uint64_t hot[7]; vk_res_counters(hot);
            CHECK(after[2] == hot[2] && after[3] == hot[3], "unchanged draws do not cause more uploads");
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "upload benchmark frame GPU work completes");
        }
        nvn_dirty_mp_reason_stats(mp1); vk_res_counters(after);
        uint64_t uploads = after[2] + after[3] - before[2] - before[3];
        CHECK(uploads == FRAMES * NTEX, "one actual upload per CPU-written texture per frame");
        /* Re-protect an already-current image: existing page state must avoid
           every redundant syscall, with either protection admission scheme. */
        nvn_dirty_mp_count sticky0[NVN_DIRTY_MP_REASON_COUNT], sticky1[NVN_DIRTY_MP_REASON_COUNT];
        nvn_dirty_mp_reason_stats(sticky0); uint64_t start = host_now_ns();
        for (int repeat = 0; repeat < 1000; repeat++) nvn_dirty_protect(textures[0].mem, textures[0].L.size);
        uint64_t sticky_ns = host_now_ns() - start; nvn_dirty_mp_reason_stats(sticky1);
        CHECK(sticky1[NVN_DIRTY_MP_TEXTURE].calls == sticky0[NVN_DIRTY_MP_TEXTURE].calls,
            "existing readonly state already avoids redundant texture mprotect");
        uint8_t pixels[SIDE * SIDE * 4]; NVNcopyRegion region = {.width = SIDE, .height = SIDE, .depth = 1};
        mtl_read_texels(&textures[0], 0, 0, &region, pixels, SIDE * 4, sizeof pixels);
        CHECK(pixels[0] == FRAMES, "uploaded first texel matches the latest CPU write");
        int valid = 1; for (size_t j = 1; j < sizeof pixels; j++) valid &= pixels[j] == (full ? FRAMES : 0x17);
        CHECK(valid, "upload preserves both rewritten and unchanged texels");
        /* ReadTexels opens/flushes its own recording; retire that submission
           before the next phase rewrites its source bytes. */
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "upload readback submission retires before the next CPU rewrite");
        nvn_dirty_mp_count mp = mp1[NVN_DIRTY_MP_TEXTURE], initial = mp0[NVN_DIRTY_MP_TEXTURE];
        printf("texture-upload-bench %s: %.3f us/upload wall, %.3f us/upload CPU, %llu uploads, %.3f/frame, %llu protect calls, %llu pages, %.3f us/syscall, %.3f ns/sticky-watch, %zu bytes/image\n",
            full ? "full" : "sparse", (double)elapsed / uploads / 1000.0, (double)cpu_elapsed / uploads / 1000.0,
            (unsigned long long)uploads,
            (double)uploads / FRAMES, (unsigned long long)(mp.calls - initial.calls),
            (unsigned long long)(mp.pages - initial.pages),
            mp.calls != initial.calls ? (double)(mp.ns - initial.ns) / (mp.calls - initial.calls) / 1000.0 : 0.0,
            (double)sticky_ns / 1000, textures[0].L.size);
    }
    for (int i = 0; i < NTEX; i++) { mtl_reg_tex(i + 1, NULL, NULL); mtl_tex_release(&textures[i]); }
    nvn_dirty_remove_range(guest); mtl_pool_del(guest); munmap(guest, bytes);
    printf("%s: texture uploads %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
/* Replacement keeps guest bindings and the vk_tex/vk_img owners unchanged. Both
   descriptor freezes and TLS ID lookups must nevertheless see the new image. */
static int texture_replacement_checks(void) {
    const size_t bytes = 1u << 20;
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(guest != MAP_FAILED, "replacement guest allocation");
    if (guest == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(guest, bytes);
    pool_t pool = {.mem = guest, .size = bytes, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t texture = {.d = {.target = 1, .format = 0x25, .width = 4, .height = 4, .depth = 1, .levels = 1},
        .mem = guest, .pool = &pool, .serial = 7002};
    nvn_tex_layout(&texture.d, &texture.L);
    mtl_reg_tex(71, &texture, NULL); mtl_reg_tex(72, &texture, NULL);
    mtl_sub_begin();
    vk_tex *object = vk_tex_get(&texture);
    CHECK(object != NULL, "replacement realizes its image");
    if (!object) return 1;
    int want = 1;
    VkImage image = vk_tex_image(object);
    VkImageView old = vk_tex_by_id(71, &want, NULL);
    CHECK(old && vk_tex_by_id(72, &want, NULL) == old, "two unchanged bindings cache the original view");
    uint64_t revision = vk_treg_revision();
    view_t integer = {.set_format = 1, .format = 0x27};
    CHECK(vk_tex_view(object, &texture, &integer, 0) != VK_NULL_HANDLE, "out-of-list integer view upgrades the image");
    CHECK(vk_tex_image(object) != image, "format-list upgrade actually replaces the image");
    CHECK(vk_treg_revision() != revision, "replacement invalidates recorder resource freeze identity");
    VkImageView current = vk_tex_view(object, &texture, NULL, 0);
    CHECK(current && current != old, "replacement owns a new default view");
    CHECK(vk_tex_by_id(71, &want, NULL) == current, "unchanged first binding resolves the replacement view");
    CHECK(vk_tex_by_id(72, &want, NULL) == current, "unchanged alias binding resolves the replacement view");
    revision = vk_treg_revision();
    CHECK(vk_tex_view(object, &texture, &integer, 0) && vk_treg_revision() == revision,
        "already-supported views do not invalidate resource identity");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "replacement GPU work completes");
    mtl_reg_tex(71, NULL, NULL); mtl_reg_tex(72, NULL, NULL);
    mtl_tex_release(&texture); mtl_pool_del(guest); munmap(guest, bytes);
    printf("%s: texture replacement %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
/* A real owning pool plus many nested views: all elementary segments have the same
   furthest-reaching winner. Exercise both live TLS and immutable recorder lookups. */
static int span_resolve_bench(void) {
    enum { NVIEWS = 1024, NSLOTS = 32, NDRAWS = 50000, STRIDE = 65536 };
    size_t bytes = (size_t)NVIEWS * STRIDE;
    uint8_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (guest == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(guest, bytes);
    for (int i = 0; i < NVIEWS; i++) mtl_pool_add(guest + (size_t)i * STRIDE + 16384, 16384);
    const vk_record_spans *spans = vk_record_spans_capture(0);
    VkBuffer expected; uint64_t base_offset;
    CHECK(vk_resolve((uint64_t)(uintptr_t)guest, bytes, &expected, &base_offset), "owning pool resolves its complete extent");
    int valid = checks && !fails;
    for (int mode = 0; mode < 3; mode++) {
        int hints[64] = {0};
        uint64_t start = host_now_ns();
        for (int draw = 0; draw < NDRAWS; draw++) for (int slot = 0; slot < NSLOTS; slot++) {
            uint64_t relative = (uint64_t)((draw * NSLOTS + slot) & (NVIEWS - 1)) * STRIDE + 32768;
            uint64_t address = (uint64_t)(uintptr_t)guest + relative, offset, available; VkBuffer buffer;
            int found = mode ? vk_record_resolve(spans, address, 16, &buffer, &offset, &available, mode == 1 ? hints : NULL) :
                vk_resolve_for(address, 16, &buffer, &offset, &available, VK_LOOKUP_OTHER);
            valid &= found && buffer == expected && offset == base_offset + relative && available == bytes - relative;
        }
        printf("span-resolve-bench %s: %.2f ns/draw (%d slots)\n",
            mode == 0 ? "live" : mode == 1 ? "frozen-hints" : "frozen-no-hints",
            (double)(host_now_ns() - start) / NDRAWS, NSLOTS);
    }
    CHECK(valid, "nested-view lookup returns identical owner, offset and available extent");
    mtl_pool_del(guest); munmap(guest, bytes);
    printf("%s: span resolution %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
/* Exercise backend coordinate conversion, strided multi-layer/3D mip access,
   dirty re-upload, and the GPU-written upload/readback branch. */
static int texel_regions(void) {
    const size_t size = 1u << 20;
    uint8_t *guest = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(guest != MAP_FAILED, "texel region pool allocated");
    if (guest == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(guest, size); nvn_dirty_add_range(guest, size);
    pool_t pool = {.mem=guest, .size=size, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    for (unsigned volume = 0; volume < 2; volume++) for (unsigned gpu = 0; gpu < 2; gpu++) {
        tex_t t = {.d={.target=volume ? 2 : 4, .format=0x25, .width=53, .height=37, .depth=5, .levels=3},
                   .mem=guest, .pool=&pool, .serial=80000 + volume * 2 + gpu};
        nvn_tex_layout(&t.d, &t.L);
        dm_guest_write_prepare(guest, size); memset(guest, 0x37, size);
        uint8_t *expected = malloc(t.L.size), *linear = malloc(53 * 37 * 5 * 4);
        CHECK(expected && linear, "texel region oracle allocated"); if (!expected || !linear) abort();
        uint64_t before[7], after[7]; uint8_t first[4] = {0x37,0x37,0x37,0x37}, got[4] = {0};
        NVNcopyRegion one = {.width=1,.height=1,.depth=1};
        vk_res_counters(before);
        mtl_write_texels(&t, 0, 0, &one, first, 4, 4);
        mtl_read_texels(&t, 0, 0, &one, got, 4, 4);
        vk_res_counters(after);
        CHECK(!memcmp(got, first, 4), "unrealized mirrored texture supports CPU texel roundtrip");
        CHECK(before[0] == after[0] && before[2] == after[2] && before[3] == after[3],
            "pool-only CPU access neither realizes an image nor converts it for upload");
        memcpy(expected, guest, t.L.size);
        mtl_sub_begin(); vk_tex *x = vk_tex_get(&t);
        CHECK(x && mtl_wait(mtl_sub_commit(), 5000000000ull), "initial mirrored upload completes");
        if (gpu) { mtl_sub_begin(); vk_tex_mark_gpu_written(x); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU-written ownership published"); }
        tex_t alias = t; alias.serial += 1000;
        if (gpu) {
            mtl_sub_begin();
            mtl_write_texels(&alias, 0, 0, &one, first, 4, 4);
            memset(got, 0, sizeof got);
            mtl_read_texels(&alias, 0, 0, &one, got, 4, 4);
            CHECK(got[0] == 0x37 && got[3] == 0x37, "new serial over GPU-owned storage preserves shared-image texel ordering");
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "shared-image texel commands retire");
            mtl_tex_release(&alias);
        }
        for (unsigned level = 0; level < 3; level++) {
            uint32_t w = t.d.width >> level, h = t.d.height >> level, depth = volume ? t.d.depth >> level : 5;
            NVNcopyRegion r = {.xoffset=3, .yoffset=2, .zoffset=depth > 2 ? 1 : 0,
                .width=7, .height=5, .depth=depth > 1 ? 2 : 1};
            unsigned layer0 = volume ? 0 : 1;
            size_t row = 7 * 4 + 9, image = row * 5 + 13, bytes = image * r.depth + 16;
            uint8_t src[512], out[512], want[512];
            memset(src, 0xa3 + level, bytes); memset(out, 0xcd, bytes); memset(want, 0xcd, bytes);
            for (int z = 0; z < r.depth; z++) {
                unsigned layer = volume ? 0 : layer0 + r.zoffset + z;
                if (!volume || !z) CHECK(!nvn_deswizzle(&t.d, level, layer, expected, linear, w * 4), "backend oracle decode");
                for (int y = 0; y < r.height; y++) {
                    uint8_t *p = linear + ((volume ? (size_t)(r.zoffset + z) * h : 0) + r.yoffset + y) * w * 4 + r.xoffset * 4;
                    memcpy(p, src + z * image + y * row, r.width * 4);
                    memcpy(want + z * image + y * row, src + z * image + y * row, r.width * 4);
                }
                if (!volume || z == r.depth - 1) CHECK(!nvn_swizzle(&t.d, level, layer, linear, w * 4, expected), "backend oracle encode");
            }
            mtl_sub_begin();
            mtl_write_texels(&t, level, layer0, &r, src, row, image);
            CHECK(!memcmp(guest, expected, t.L.size), "backend region preserves every other byte, mip and layer");
            mtl_read_texels(&t, level, layer0, &r, out, row, image);
            CHECK(!memcmp(out, want, bytes), "backend region roundtrip preserves stride and image padding");
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "texel region commands retire");
            /* New submission must see dirty pool data, not the pre-patch upload. */
            mtl_sub_begin(); x = vk_tex_get(&t);
            unsigned layer = volume ? 0 : layer0 + r.zoffset;
            VkBuffer buffer; VkDeviceSize offset;
            uint8_t *pixels = vk_transient((size_t)w * h * 4, 16, &buffer, &offset);
            VkBufferImageCopy copy = {.bufferOffset=offset, .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                .imageOffset={0, 0, volume ? r.zoffset : 0}, .imageExtent={w, h, 1}};
            vk_barrier_all(vk_cb());
            vkCmdCopyImageToBuffer(vk_cb(), vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
            vk_flush_wait();
            nvn_deswizzle(&t.d, level, volume ? 0 : layer, expected, linear, w * 4);
            CHECK(!memcmp(pixels, linear + (volume ? (size_t)r.zoffset * w * h * 4 : 0), (size_t)w * h * 4),
                "GPU readback agrees with whole-level oracle, including untouched pixels");
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "texel validation readback retires");
        }
        free(expected); free(linear); mtl_tex_release(&t);
    }
    nvn_dirty_remove_range(guest); mtl_pool_del(guest); munmap(guest, size);
    vk_texel_stats(300);
    printf("%s: backend texel regions %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return !!fails;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--texel-regions")) return texel_regions();
    if (argc == 2 && !strcmp(argv[1], "--mprotect-bench")) return mprotect_bench();
    if (argc == 2 && !strcmp(argv[1], "--registry-kind-bench")) return registry_kind_bench();
    if (argc == 2 && !strcmp(argv[1], "--texture-cache-bench")) return texture_cache_bench();
    if (argc == 2 && !strcmp(argv[1], "--texture-churn-bench")) return texture_churn_bench();
    if (argc == 2 && !strcmp(argv[1], "--texture-upload-bench")) return texture_upload_bench();
    if (argc == 2 && !strcmp(argv[1], "--texture-replacement")) return texture_replacement_checks();
    if (argc == 2 && !strcmp(argv[1], "--record-binding-bench")) return vk_draw_test_binding_bench();
    if (argc == 2 && !strcmp(argv[1], "--span-resolve-bench")) return span_resolve_bench();
    if (argc == 2 && !strcmp(argv[1], "--lag-query-bench")) return vk_lag_test_query_bench();
    if (argc == 2 && !strcmp(argv[1], "--lag-touch-bench")) return vk_draw_test_touch_bench();
    if (argc == 2 && !strcmp(argv[1], "--lag-bench")) return lag_bench();
    if (argc == 2 && !strcmp(argv[1], "--lag-vma")) return lag_vma_checks();
    if (argc == 2 && !strcmp(argv[1], "--vma-merge")) return vma_merge_checks();
    if (argc == 2 && !strcmp(argv[1], "--lag-vma-policy")) return lag_vma_policy_checks();
    if (argc == 2 && !strcmp(argv[1], "--lag-vma-async")) return lag_vma_async_checks();
    if (argc == 2 && !strcmp(argv[1], "--arena-bo")) return arena_bo_checks();
    if (argc == 2 && !strcmp(argv[1], "--staging-retain")) return staging_retain_checks();
    const size_t G = 64u << 20;   /* guest memory: shared anonymous, like runtime/services.c */
    uint8_t *g = mmap(NULL, G, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    for (size_t i = 0; i < 16u << 20; i += 4) *(uint32_t *)(g + i) = (uint32_t)i ^ 0x9e3779b9u;
    uint8_t *P = g, *Q = g + (32u << 20);            /* pool P: 16 MiB at the start; Q: 1 MiB, later */
    mtl_init();
    uint64_t failure_ticket = mtl_last_fence() + 1;
    vk_record_lag_fail(failure_ticket); vk_record_lag_fail(failure_ticket + 1);
    CHECK(vk_record_test_take_failure(failure_ticket), "later failure cannot hide an earlier slot failure");
    CHECK(vk_record_test_take_failure(failure_ticket + 1) && !vk_record_test_take_failure(failure_ticket),
        "failure consumption clears only its exact ticket");
    mtl_pool_add(P, 16u << 20);
    if (runtime_frame_lag_requested()) nvn_dirty_add_range(P, 16u << 20);
    int same = 1; for (size_t i = 0; i < 16u << 20; i += 4) if (*(uint32_t *)(P + i) != ((uint32_t)i ^ 0x9e3779b9u)) { same = 0; break; }
    CHECK(same, "pool contents changed when the pool was registered");
    void *b; uint64_t off;
    CHECK(mtl_addr((uint64_t)(uintptr_t)(P + 0x1234), 16, &b, &off) && off == 0x1234, "mtl_addr inside the pool");
    CHECK(!mtl_addr((uint64_t)(uintptr_t)(Q), 16, &b, &off), "mtl_addr outside every pool");
    mtl_pool_add(P + (1u << 20), 1u << 20);   /* a pool carved from P: a view */
    CHECK(mtl_addr((uint64_t)(uintptr_t)(P + (1u << 20) + 8), 8, &b, &off) && off == (1u << 20) + 8, "view pool resolves into the owner");
    CHECK(vk_lag_bound((uint64_t)(uintptr_t)(P + (1u << 20) + 8), 16u << 20) == (1u << 20) - 8, "unknown storage extent stops at the containing registered sub-pool");

    /* 1. GPU copy, uniform update, event and counter in one submission; the event must not be set before completion. */
    volatile uint32_t *ev = (volatile uint32_t *)(Q + 0x100); *ev = 7;
    uint64_t ctr = (uint64_t)(uintptr_t)(Q + 0x200); memset(Q + 0x200, 0xee, 16);
    memset(P + (4u << 20), 0, 4096);
    op_copy((uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)(P + (4u << 20)), 4096);
    uint32_t ub[16]; for (int i = 0; i < 16; i++) ub[i] = 0xc0de0000u + (uint32_t)i;
    op_ub((uint64_t)(uintptr_t)(P + (5u << 20)), ub, sizeof ub);
    op_copy((uint64_t)(uintptr_t)(P + (5u << 20)), (uint64_t)(uintptr_t)(P + (5u << 20) + 4096), sizeof ub);   /* ordered after the update */
    op_event(ev, 0, 42);
    op_event(ev, 1, 1);
    op_counter(NVN_COUNTER_TYPE_SAMPLES_PASSED, ctr);
    op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(rec); rn = 0;
    uint64_t pending; int real_samples = !vk_counter_value(ctr, &pending);   /* the enabled path has no draws: exactly zero */
    CHECK(real_samples || pending == 0, "pending empty synthetic sample report is fixed at recording");
    uint64_t f = mtl_sub_commit();
    CHECK(mtl_wait(f, 2000000000ull), "submission 1 completes");
    CHECK(*ev == 43, "event write then add: %u", *ev);
    uint64_t c[2]; memcpy(c, Q + 0x200, 16);
    CHECK(c[0] == 0 && c[1] != 0xeeeeeeeeeeeeeeeeull, "counter report written (no draw before it: 0 samples)");
    CHECK(!memcmp(P + (4u << 20), P, 4096), "GPU copy inside the pool");
    CHECK(!memcmp(P + (5u << 20), ub, sizeof ub), "GPU uniform update");
    CHECK(!memcmp(P + (5u << 20) + 4096, ub, sizeof ub), "copy after the update sees the update");

    /* A frame-lag regression: hold the GPU at a host event, submit a guest read, then recycle the source.
       The destination must contain submit-time bytes, not the next frame's values. No scheduler timing assumption. */
    if (runtime_frame_lag_requested()) for (int host = 0; host < 2; host++) {
        VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
        uint8_t *src = P + (10u << 20), *dst = src + 65536;
        memset(src, 0x5a, 256); memset(dst, 0, 256);
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        mtl_sub_copy((uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)dst, 256);
        /* A later read of a GPU-produced range must stay on the queue timeline, not snapshot stale CPU bytes. */
        mtl_sub_copy((uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)(dst + 65536), 256);
        f = mtl_sub_commit();
        int waited = lag_test_rewrite(src, 256, 0xa5, host, gate);
        const char *mode = getenv("TOTK_VK_LAG_MODE");
        if (vk_lag_capture_on() && (!mode || strcmp(mode, "copy"))) CHECK(waited, "actual %s reuse waits for the GPU", host ? "host" : "guest");
        CHECK(mtl_wait(f, 2000000000ull), "lagged read completes after the gate opens");
        int bad = 0; for (int i = 0; i < 256; i++) bad += dst[i] != 0x5a;
        CHECK(!bad, "lagged GPU read retains submit-time bytes after guest reuse (%d wrong bytes)", bad);
        CHECK(!memcmp(dst, dst + 65536, 256), "GPU-produced source retains ordered GPU data, not a stale CPU snapshot");
        CHECK(src[0] == 0xa5, "lag capture does not write an old version back to guest memory");
        vkDestroyEvent(V.dev, gate, NULL);
    }
    /* An earlier CPU post-op must not deadlock the only completion cursor when a later queued read guards its page.
       GPU-only fence publication must advance independently of post-op delivery. */
    if (vk_lag_capture_on() && (!getenv("TOTK_VK_LAG_MODE") || strcmp(getenv("TOTK_VK_LAG_MODE"), "copy"))) {
        uint8_t *src = P + (12u << 20), *dst = src + 65536;
        memset(src, 0x5a, 256);
        VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
        mtl_sub_counter((uint64_t)(uintptr_t)src, NVN_COUNTER_TYPE_SAMPLES_PASSED);
        uint64_t first = mtl_sub_commit();
        mtl_sub_begin(); mtl_sub_copy((uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)dst, 256);
        uint64_t second = mtl_sub_commit();
        VKCHECK(vkSetEvent(V.dev, gate));
        CHECK(mtl_wait(second, 2000000000ull) && mtl_wait(first, 0), "later GPU read releases an earlier blocked CPU post-op");
        CHECK(*(uint64_t *)src == 0 && dst[0] == 0x5a, "post-op writes only after the later read completes");
        vkDestroyEvent(V.dev, gate, NULL);
    }

    /* 2. Copies the GPU cannot do run on the CPU in order: GPU writes X, CPU copies X out of the pool, GPU copies back. */
    memset(Q, 0, 4096);
    op_ub((uint64_t)(uintptr_t)(P + (6u << 20)), ub, 64);
    op_copy((uint64_t)(uintptr_t)(P + (6u << 20)), (uint64_t)(uintptr_t)Q, 64);          /* destination outside: CPU step */
    op_copy((uint64_t)(uintptr_t)Q, (uint64_t)(uintptr_t)(P + (6u << 20) + 128), 64);    /* source outside: CPU step */
    op_event(ev, 0, 100);
    f = run(); mtl_wait(f, 2000000000ull);
    CHECK(!memcmp(Q, ub, 64), "CPU step sees the GPU update before it");
    CHECK(!memcmp(P + (6u << 20) + 128, ub, 64), "CPU step back into the pool");
    CHECK(*ev == 100, "event after CPU steps");

    /* 3. Many submissions in flight: fences complete in order and mtl_finish waits for all. */
    for (int i = 0; i < 200; i++) { op_copy((uint64_t)(uintptr_t)P, (uint64_t)(uintptr_t)(P + (8u << 20) + (size_t)(i % 16) * 65536), 65536); op_event(ev, 1, 1); f = run(); }
    mtl_finish();
    CHECK(*ev == 300, "200 submissions each add 1: %u", *ev);
    CHECK(mtl_wait(f, 0), "last fence done after mtl_finish");

    /* Sticky guards do no page/mprotect work for unchanged input; a real CPU reuse invalidates and re-arms the unit.
       Two writing frames promote the ring to private copies; watched copies keep activity observable without waits. */
    if (vk_lag_capture_on() && (!getenv("TOTK_VK_LAG_MODE") || strcmp(getenv("TOTK_VK_LAG_MODE"), "copy"))) {
        vk_lag_stats(0);   /* report setup, hot-copy bytes and waits for just the four synthetic frames below */
        uint8_t *src = P + (13u << 20), *dst = src + 65536;
        uint64_t r0, u0, p0, d0, r1, u1, p1, d1, mc0, mp0, mn0, mc1, mp1, mn1;
        const char *hybrid_env = getenv("TOTK_VK_LAG_HYBRID");
        int hybrid = !(hybrid_env && !atoi(hybrid_env));
        for (int frame = 0; frame < 4; frame++) {
            if (frame == 3) {
                nvn_dirty_lag_frame(19);
                CHECK(nvn_dirty_lag_hot(src, 256), "hot ring remains promoted through seven quiet frames");
                nvn_dirty_lag_frame(20);
                CHECK(!nvn_dirty_lag_hot(src, 256), "hot ring demotes after eight quiet frames");
            } else nvn_dirty_lag_frame(10 + (uint64_t)frame);
            int value = 0x5a + frame;
            memset(src, value, 256);
            nvn_dirty_lag_sticky_stats(&r0, &u0, &p0, &d0);
            VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
            uint64_t copied, copy_ns, copy_ranges, excluded;
            vk_lag_capture_take(&copied, &copy_ns, &copy_ranges, &excluded);
            mtl_sub_begin();
            vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
            mtl_sub_copy((uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)dst, 256);
            f = mtl_sub_commit();
            vk_lag_capture_take(&copied, &copy_ns, &copy_ranges, &excluded);
            nvn_dirty_lag_sticky_stats(&r1, &u1, &p1, &d1);
            if (frame < 2 || frame == 3) CHECK(r1 == r0 + 1, "rewritten cold unit re-arms once before frame %d", frame);
            if (!frame) {
                nvn_dirty_mp_stats(&mc0, &mp0, &mn0);
                for (int repeat = 0; repeat < 16; repeat++) nvn_dirty_lag_protect(src, 256, f);
                nvn_dirty_mp_stats(&mc1, &mp1, &mn1);
                nvn_dirty_lag_sticky_stats(&r0, &u0, &p0, &d0);
                CHECK(mc0 == mc1 && mp0 == mp1 && r0 == r1 && u0 == u1, "duplicate same-ticket registration changes neither ticket stats nor page protection");
            }
            int waited = lag_test_rewrite(src, 256, 0xa5, 0, gate);
            CHECK(mtl_wait(f, 2000000000ull), "sticky/hybrid frame %d completes", frame);
            CHECK(dst[0] == value && dst[255] == value, "sticky/hybrid frame %d retains submitted bytes", frame);
            if (frame == 2 && hybrid) {
                CHECK(copied == 256 && copy_ranges == 1 && !waited, "hot ring uses one bounded private copy and no GPU reuse wait");
            } else CHECK(waited, "cold or hybrid-disabled frame %d waits only on actual reuse", frame);
            if (frame == 1) CHECK(nvn_dirty_lag_hot(src, 256), "two distinct writing frames promote the hot ring");
            vkDestroyEvent(V.dev, gate, NULL);
        }
        nvn_dirty_lag_unit top[8]; int ntop = nvn_dirty_lag_top(top, 8), found = 0;
        for (int i = 0; i < ntop; i++) if ((uintptr_t)src >= top[i].addr && (uintptr_t)src < top[i].addr + top[i].len) {
            found = top[i].faults >= 4 && top[i].waits >= (hybrid ? 3u : 4u) && top[i].last_frame == 20;
        }
        CHECK(found, "top-unit diagnostics identify the rewritten guest ring with its size, writes, waits and frame");
        CHECK(!nvn_dirty_lag_hot(src, 256), "diagnostic reset does not alter the demoted hot history");
        vk_lag_stats(4);
    }
    /* 4. Virtual pool aliases (export / heap pools only: the host path aliases with mremap in totk_vpool.c). */
    uint8_t *V = g + (48u << 20);
    if (mtl_pool_alias(V, P + (2u << 20), 1u << 20)) {
        mtl_vpool_add(V, 2u << 20);
        CHECK(!memcmp(V, P + (2u << 20), 1u << 20), "alias shows the physical pages");
        P[(2u << 20) + 5] ^= 0xff; CHECK(V[5] == P[(2u << 20) + 5], "CPU write through the physical view shows in the alias");
        CHECK(mtl_addr((uint64_t)(uintptr_t)(V + 64), 64, &b, &off) && off == (2u << 20) + 64, "alias resolves to the physical pool's buffer");
        op_ub((uint64_t)(uintptr_t)(V + 256), ub, 64);   /* GPU write through the virtual address */
        f = run(); mtl_wait(f, 2000000000ull);
        CHECK(!memcmp(P + (2u << 20) + 256, ub, 64), "GPU write via the virtual pool lands in the physical pool");
        mtl_vpool_del(V);
        printf("virtual pool aliases: checked\n");
    } else printf("virtual pool aliases: host-pointer pools (aliased by totk_vpool.c with mremap), not checked here\n");


    if (vk_record_enabled() && vk_lag_capture_on() &&
        (!getenv("TOTK_VK_LAG_MODE") || strcmp(getenv("TOTK_VK_LAG_MODE"), "copy")))
        lag_native_guard_liveness(P + (15u << 20));

    /* Protocol-only tail, after all real GPU work: concurrent workers register overlapping frozen ranges with
       explicit tickets. The newest ticket must remain authoritative, duplicates must be true no-ops, and a writer
       must stay blocked until that newest ticket completes. */
    if (vk_lag_capture_on() && (!getenv("TOTK_VK_LAG_MODE") || strcmp(getenv("TOTK_VK_LAG_MODE"), "copy"))) {
        mtl_finish();
        uint8_t *page = P + (14u << 20);
        uint64_t ticket = mtl_last_fence() + 1, faults0, hosts0, ns;
        CHECK(nvn_dirty_lag_register(page, 96, ticket - 1, NVN_DIRTY_LAG_COLD, 32),
              "synthetic unit is armed at the completed ticket before worker races");
        _Atomic int start = 0;
        lag_registrar regs[] = {
            {page + 32, 64, ticket + 3, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_GPU, 128, &start, 0},
            {page, 96, ticket + 1, NVN_DIRTY_LAG_COLD | NVN_DIRTY_LAG_ALIAS, 256, &start, 0},
            {page + 16, 80, ticket + 4, NVN_DIRTY_LAG_WRITABLE, 512, &start, 0},
            {page + 8, 48, ticket + 2, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_ALIAS, 384, &start, 0},
        };
        nvn_dirty_lag_batch race_batches[sizeof regs / sizeof regs[0]] = {{0}};
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i += 2) {
            CHECK(nvn_dirty_lag_batch_add(&race_batches[i], regs[i].addr, regs[i].len, regs[i].reason, regs[i].range_size),
                  "concurrent worker freezes batch refs before arming");
            regs[i].batch = &race_batches[i]; regs[i].nshards = 1;
        }
        pthread_t rt[sizeof regs / sizeof regs[0]];
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
            if (pthread_create(&rt[i], NULL, lag_register_run, &regs[i])) abort();
        atomic_store_explicit(&start, 1, memory_order_release);
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) pthread_join(rt[i], NULL);
        int all_registered = 1;
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) all_registered &= regs[i].result;
        CHECK(all_registered, "concurrent overlapping explicit-ticket registrations succeed");
        CHECK(nvn_dirty_lag_registered(page, 96, ticket + 4), "overlap query observes the maximum registered ticket");
        CHECK(!nvn_dirty_lag_registered(page, 96, ticket + 5), "overlap query rejects a ticket above the maximum");
        CHECK(!nvn_dirty_lag_register(page, 0, ticket + 4, 0, 0), "unsupported empty registration is rejected");
        CHECK(!nvn_dirty_lag_register(g + (60u << 20), 16, ticket + 4, 0, 0), "untracked registration is rejected");

        uint64_t rearm0, updates0, promo0, demo0, rearm1, updates1, promo1, demo1;
        nvn_dirty_lag_sticky_stats(&rearm0, &updates0, &promo0, &demo0);
        for (int i = 0; i < 32; i++) nvn_dirty_lag_protect(page, 96, ticket + 4);
        nvn_dirty_lag_register(page, 96, ticket + 4, NVN_DIRTY_LAG_READONLY, 512);
        nvn_dirty_lag_sticky_stats(&rearm1, &updates1, &promo1, &demo1);
        CHECK(rearm0 == rearm1 && updates0 == updates1, "same-ticket protects/register leave ticket-update stats unchanged");

        /* Keep timing outside the production fast path: compare one first registration to repeated same-ticket
           registrations and report observations without a scheduler-sensitive performance assertion. */
        uint8_t *timed_page = page + 65536;
        uint64_t t0 = host_now_ns();
        int first_ok = nvn_dirty_lag_register(timed_page, 16, ticket + 4, NVN_DIRTY_LAG_GPU, 16);
        uint64_t first_ns = host_now_ns() - t0;
        const int repeats = 2000;
        t0 = host_now_ns();
        for (int i = 0; i < repeats; i++)
            nvn_dirty_lag_register(timed_page, 16, ticket + 4, NVN_DIRTY_LAG_GPU, 16);
        uint64_t repeat_ns = host_now_ns() - t0;
        CHECK(first_ok, "timing fixture first registration succeeds");
        printf("lag registration timing: first=%llu ns, repeated=%llu ns/call (%d calls)\n",
               (unsigned long long)first_ns, (unsigned long long)(repeat_ns / repeats), repeats);

        nvn_dirty_lag_stats(&faults0, &hosts0, &ns);
        lag_writer writer = {.dst = page, .n = 16, .byte = 0x3c};
        pthread_t wt; if (pthread_create(&wt, NULL, lag_writer_run, &writer)) abort();
        uint64_t faults, hosts;
        do { nvn_dirty_lag_stats(&faults, &hosts, &ns); sched_yield(); }
        while (faults == faults0 && hosts == hosts0 && !atomic_load(&writer.done));
        CHECK(!atomic_load(&writer.done), "writer blocks on the maximum ticket, not an earlier overlapping ticket");
        _Atomic int registrar_start = 1;
        lag_registrar contender = {
            .addr = page + 8, .len = 48, .fence = ticket + 4,
            .reason = NVN_DIRTY_LAG_READONLY, .range_size = 64, .start = &registrar_start
        };
        nvn_dirty_lag_batch busy_batch = {0};
        CHECK(nvn_dirty_lag_batch_add(&busy_batch, contender.addr, contender.len, contender.reason, contender.range_size),
              "BUSY contender collects without touching protection");
        contender.batch = &busy_batch; contender.nshards = 1;
        pthread_t ct; if (pthread_create(&ct, NULL, lag_register_run, &contender)) abort();
        uint64_t reg_deadline = host_now_ns() + 2000000000ull;
        while (!atomic_load_explicit(&contender.done, memory_order_acquire) && host_now_ns() < reg_deadline) sched_yield();
        int rejected_before_completion = atomic_load_explicit(&contender.done, memory_order_acquire) && !contender.result;
        CHECK(rejected_before_completion, "BUSY future-ticket registration returns promptly so submit can proceed");
        nvn_dirty_lag_complete(ticket + 3);
        CHECK(!atomic_load(&writer.done), "writer remains blocked after a lower ticket completes");
        nvn_dirty_lag_complete(ticket + 4);
        pthread_join(ct, NULL); pthread_join(wt, NULL);
        CHECK(!contender.result, "BUSY registration does not turn into a blocking join after ticket completion");
        CHECK(atomic_load(&writer.done) && page[0] == 0x3c, "writer completes only after the maximum ticket completes");

        nvn_dirty_lag_unit top[16]; int ntop = nvn_dirty_lag_top(top, 16), found = 0;
        unsigned expected_reasons = NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_COLD | NVN_DIRTY_LAG_GPU |
                                    NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_ALIAS;
        for (int i = 0; i < ntop; i++) if ((uintptr_t)page >= top[i].addr && (uintptr_t)page < top[i].addr + top[i].len)
            found = (top[i].reasons & expected_reasons) == expected_reasons && top[i].max_range >= 512;
        CHECK(found, "top diagnostics retain readonly/alias/writable reason bits and maximum descriptor size");
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) nvn_dirty_lag_batch_destroy(&race_batches[i]);
        nvn_dirty_lag_batch_destroy(&busy_batch);
        uintptr_t unit_lo, unit_hi;
        CHECK(nvn_dirty_lag_bounds(P, 1, &unit_lo, &unit_hi), "batch fixtures obtain the actual configured dirty unit");
        if (nvn_dirty_lag_bounds(P, 1, &unit_lo, &unit_hi)) lag_batch_checks(unit_hi - unit_lo, ticket + 5);
    }

    /* No real GPU work follows the synthetic ticket protocol tail. */

    /* 5. Removing pools keeps their contents in ordinary memory. */
    uint8_t keep[4096]; memcpy(keep, P + (4u << 20), 4096);
    mtl_pool_del(P + (1u << 20));
    CHECK(vk_lag_bound((uint64_t)(uintptr_t)(P + (1u << 20) + 8), 16u << 20) == (15u << 20) - 8, "sub-pool destruction invalidates the extent cache");
    mtl_pool_del(P);
    CHECK(!memcmp(P + (4u << 20), keep, 4096), "contents kept when the pool is removed");
    CHECK(!mtl_addr((uint64_t)(uintptr_t)P, 16, &b, &off), "removed pool no longer resolves");
    P[0] = 0x55; CHECK(P[0] == 0x55, "guest memory writable after removal");
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

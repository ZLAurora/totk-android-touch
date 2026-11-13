/* Graphics secondary recording. Queue ownership is transferred under rec_mu; no worker reads guest state. */
#include "vk_record.h"
#include "nvn_dirty.h"
#include "nvn_hmap.h"
#include "../../runtime/frame_timeline.h"
#include <errno.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define REC_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(REC_ASAN)
#include <sanitizer/asan_interface.h>
#define STORAGE_POISON(p, n) __asan_poison_memory_region((p), (n))
#define STORAGE_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define STORAGE_POISON(p, n) ((void)0)
#define STORAGE_UNPOISON(p, n) ((void)0)
#endif
#ifdef TOTK_VK_TESTS
#include <time.h>
#endif

enum { REC_WORKERS = 4, REC_RING = 8, REC_DS_CACHE = 2048, REC_POOL_SETS = 1024, REC_GUARD_CACHE = 128 };
enum { REC_STORAGE_CLASSES = 24, REC_STORAGE_MIN = 256, REC_STORAGE_SLAB = 1 << 20 };
typedef struct rec_storage_block {
    struct rec_storage_block *next; struct rec_storage_slab *slab; size_t bytes;
    _Alignas(16) unsigned char data[];
} rec_storage_block;
typedef struct rec_storage_slab {
    struct rec_storage_slab *next; size_t bytes, used, live;
    _Alignas(16) unsigned char data[];
} rec_storage_slab;
typedef struct {
    rec_storage_slab *slabs; rec_storage_block *free[REC_STORAGE_CLASSES]; uint64_t bytes;
} rec_storage;
typedef struct {
    const vk_record_spans *spans; VkBuffer buffer; uint64_t offset, size; uint32_t reason;
} rec_guard;
typedef struct {
    uint32_t before;
    enum { REC_CLEAR, REC_TIMESTAMP } kind;
    union {
        struct { VkClearAttachment attachment; VkClearRect rect; } clear;
        struct { VkQueryPool pool; uint32_t query; VkPipelineStageFlags2 stage; } timestamp;
    };
} rec_event;
typedef struct rec_job {
    struct rec_job *all_next, *pass_next, *queue_next, *batch_next;
    vk_record_draw *draw; vk_record_descriptor *info;
    uint32_t count, limit, capdraw, ninfo, capinfo, info_mark, slot, ncolor;
    VkFormat color[8], depth, stencil; VkSampleCountFlagBits samples;
    VkCommandBuffer cb; atomic_int done; int parallel;
    rec_event *event; uint32_t nevent, capevent;
    uint64_t generation, reset_time, id, ready_ns;
    uint8_t small_draw_uses, small_info_uses; /* consecutive oversized packet uses, capped at32 */
    uint64_t frame; /* immutable originating walk, also used after worker queueing */
    rec_guard *guards; uint32_t nguards, capguards, *guard_cache; uint64_t guard_ticket; int registering;
    vk_record_lag_intent *projected; uint32_t nprojected, capprojected;
    nvn_dirty_lag_batch units; const nvn_dirty_lag_batch *registration;
    uint32_t shard, nshards; uint64_t projected_ranges, prep_ns;
} rec_job;
typedef struct rec_pass {
    struct rec_pass *all_next, *next; rec_job *jobs; VkCommandBuffer primary;
    uint32_t draws;
} rec_pass;
/* The launch-time profiler switch is immutable before any job/pass allocation.
   Only enabled runs allocate these tails; disabled objects retain the original
   sizes/member offsets, including the inline registrar array and slot stride. */
typedef struct {
    rec_job job;
    VkQueryPool pool; uint32_t query; /* this secondary's own immutable stats query */
} rec_profile_job;
typedef struct {
    rec_pass pass;
    vk_gpuprof_end_token end; /* assembler never reads the walking profiler */
} rec_profile_pass;
typedef struct rec_context {
    uint32_t lease_head, lease_tail, ncb;
    struct rec_context *retired_next; int retired; /* rec_mu: return-list publication */
    VkDescriptorPool *dp; uint32_t ndp, capdp, curdp, sets;
    uint64_t descriptor_sets, descriptor_calls, descriptor_resets;
    uint64_t stock_events[VK_DESC_STOCK_REASONS + 1];   /* per reason, then pools destroyed; published with the counts */
    vk_desc_stock *stock; /* TOTK_VK_DESC_STOCK=1 only */
    int stock_on;         /* this owner's stock policy, switched only at its generation handoff (descriptors_recycle) */
    struct {
        uint64_t hash[2]; VkDescriptorSetLayout layout; uint32_t count; VkDescriptorSet set;
        uint64_t guard_ticket; const vk_record_spans *guard_spans;
    } cache[REC_DS_CACHE];
    uint64_t generation;
    VkDescriptorBufferInfo zero;
    vk_record_descriptor *info; uint32_t capinfo;
    const vk_record_spans *spans; int hints[64];
} rec_context;
static rec_context *contexts[REC_WORKERS + 1][REC_RING]; /* context zero belongs to the walking thread */
typedef struct {
    VkCommandBuffer cb; uint32_t next; uint64_t last_use; int dirty;
} rec_command;
typedef struct {
    VkCommandPool pool; rec_command *command;
    uint32_t capacity, ready, vacant, live, in_use, peak, recent_peak;
    uint64_t next_maintenance, returns, cold_resets, batches, lease_ns, return_ns, reset_ns;
    rec_context *retired_head, *retired_tail; /* rec_mu; all remaining fields recording-owner-only */
    pthread_t owner; int owner_set;
} rec_commands;
static rec_commands command_owner[REC_WORKERS + 1];
static _Atomic int command_returns[REC_WORKERS + 1];
static _Atomic uint64_t command_epoch, return_publications, return_publish_ns;
static struct {
    _Atomic uint64_t in_use, peak, returns, cold_resets, batches, lease_ns, return_ns, reset_ns;
} command_stats[REC_WORKERS + 1];
static struct {
    rec_job *jobs, *cursor; rec_pass *passes, *pass_cursor, *pending; uint64_t generation, sealed_generation, sealed_ticket, frame;
    nvn_dirty_lag_batch units; rec_job registrars[REC_WORKERS];
    rec_storage draw_storage, info_storage;
    unsigned prune_uses;
} slots[REC_RING];
static pthread_mutex_t rec_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_work = PTHREAD_COND_INITIALIZER, rec_done = PTHREAD_COND_INITIALIZER;
/* Sleepers on rec_work / rec_done, counted under rec_mu: signal only when someone actually waits. Bionic's
   pthread_cond_signal/broadcast make a futex syscall even with no waiter, and the walker paid that kernel wake path
   in dispatch_building for every batch (~2 % of its time on the Thor). Waiters re-check their condition under
   rec_mu before sleeping, so a skipped signal can never be a lost wakeup. */
static unsigned rec_work_waiters, rec_done_waiters;
/* TOTK_VK_REC_WAKE_BATCH (default 0 = off: every enqueue with a parked worker signals one). n >= 1: the walker signals
   only when no worker is already on its way to the queue (a signalled worker not yet running, or one polling it), and
   a worker that takes a batch and leaves more queued wakes the next parked worker itself, so a burst of chunks costs
   the walker one futex wake. n >= 2 also lets a worker that found the queue empty poll it for up to n µs before it
   parks, so a chunk published meanwhile needs no wake at all. Live (runtime/live_ctl.h): read at each enqueue and each
   time a worker finds the queue empty. All under rec_mu: rec_wake_pending counts signals whose worker has not yet
   taken rec_mu again (each wake, spurious or not, takes one off, so it never overstates the workers on their way:
   a signal under rec_mu with a parked, unsignalled waiter always wakes one), rec_spinning the polling workers. A
   queued batch therefore always has a worker that will look at the queue before it parks. */
static unsigned rec_wake_pending, rec_spinning;
static _Atomic uint32_t rec_queue_seq;   /* enqueues so far: what polling workers watch (written under rec_mu) */
static uint64_t rec_wakes_walker, rec_wakes_chain, rec_spin_hits, rec_parks;   /* per-300 report, under rec_mu */
static int rec_wake_launch;
static totk_live_cache rec_wake_live;
static int rec_wake_batch(void) { return totk_live_cached(&rec_wake_live, "TOTK_VK_REC_WAKE_BATCH", rec_wake_launch); }
static rec_job *queue_head, *queue_tail, *pass_head, *pass_tail, *building;
static rec_pass *pending_head, *pending_tail;
static rec_job *small_head, *small_tail; static uint32_t small_jobs, small_draws;
static rec_job *guard_building;
static uint32_t current_slot, chunk_draws = 128, lead_draws = 32, inline_draws, pass_draws;
static int threads = -1, ds_off, dyn_off, vma_names, descriptor_stock;
static uint32_t descriptor_stock_max;   /* TOTK_VK_DESC_STOCK_MAX_SETS launch value, 0: no ceiling */
static struct {
    uint32_t ncolor; VkFormat color[8], depth, stencil; VkSampleCountFlagBits samples;
} pass;
static uint64_t busy_ns[REC_WORKERS], chunks_parallel, chunks_inline, passes_parallel, passes_inline;
static uint64_t next_chunk_id;
static uint64_t wait_ns, walk_ns, walk_wait_mark, inline_ns, execute_ns, reset_ns, worker_reset_ns, job_ns;
static uint64_t ready_delay_ns, service_ns, max_ready_delay_ns, max_service_ns, jobs_serviced;
static uint64_t join_jobs, join_draws, join_reset_ns, max_join_draws;
static uint64_t join_passes, join_pass_draws, max_join_pass_draws;
static uint64_t background_wait_ns, background_slots;
static uint64_t guard_jobs, guard_intents, guard_units, guard_prep_ns, guard_merge_ns, guard_service_ns, guard_wait_ns, guard_critical_ns;
static int pass_parallel;
/* Requested retained capacity, not RSS/driver bytes. Rare growth updates only; reports never inspect live jobs. */
static _Atomic uint64_t memory_draws, memory_info, memory_cbs, memory_dpools;
/* Retained descriptor pools of the primary slots (nvn_vk.c) beside memory_dpools (recorder contexts), and the sets held
   in every owner's TOTK_VK_DESC_STOCK stock: the descriptors line reports both arms' retained descriptor memory. */
static _Atomic uint64_t memory_primary_dpools, stock_sets_retained;
void vk_record_primary_pools(int64_t delta) {
    atomic_fetch_add_explicit(&memory_primary_dpools, (uint64_t)delta, memory_order_relaxed);
}
static uint64_t storage_limit = UINT64_C(64) << 20;
static _Atomic uint64_t storage_reclaims, storage_reclaimed, cb_reclaimed;
static uint64_t storage_trim_ns, storage_referenced;
/* Descriptor accounting is cumulative; publish owner-local driver counts once per job, not per set. */
static struct { _Atomic uint64_t sets, calls, resets, stock[VK_DESC_STOCK_REASONS + 1]; } descriptor_stats[REC_WORKERS + 1];
/* Elapsed timers run only with TOTK_VK_REC_PROFILE=1 or frame tracing (vk_rec_timing_on); the counts are always on. */
static uint64_t descriptor_walk_ns, descriptor_list_ns, descriptor_lists, descriptor_template_ns, descriptor_templates;
static uint64_t descriptor_primary[3]; /* serialized primary/walking owner, also presentation/compute */
void vk_record_descriptor_primary(uint64_t sets, uint64_t calls, uint64_t resets) {
    descriptor_primary[0] += sets; descriptor_primary[1] += calls; descriptor_primary[2] += resets;
}
static uint64_t descriptor_primary_stock[VK_DESC_STOCK_REASONS + 1]; /* the same owner's stock resets, pools destroyed */
void vk_record_descriptor_stock_event(unsigned reason, uint64_t pools_destroyed) {
    descriptor_primary_stock[reason]++; descriptor_primary_stock[VK_DESC_STOCK_REASONS] += pools_destroyed;
}
static void stock_totals(uint64_t out[VK_DESC_STOCK_REASONS + 1]) {
    for (unsigned k = 0; k <= VK_DESC_STOCK_REASONS; k++) {
        out[k] = descriptor_primary_stock[k];
        for (unsigned w = 0; w <= REC_WORKERS; w++) out[k] += atomic_load_explicit(&descriptor_stats[w].stock[k], memory_order_relaxed);
    }
}
void vk_record_descriptor_walk(uint64_t ns) { descriptor_walk_ns += ns; }
static void descriptor_totals(uint64_t out[8]) {
    memcpy(out, descriptor_primary, sizeof descriptor_primary);
    for (unsigned w = 0; w <= REC_WORKERS; w++) {
        out[0] += atomic_load_explicit(&descriptor_stats[w].sets, memory_order_relaxed);
        out[1] += atomic_load_explicit(&descriptor_stats[w].calls, memory_order_relaxed);
        out[2] += atomic_load_explicit(&descriptor_stats[w].resets, memory_order_relaxed);
    }
    out[3] = descriptor_walk_ns; out[4] = descriptor_list_ns; out[5] = descriptor_lists;
    out[6] = descriptor_template_ns; out[7] = descriptor_templates;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_descriptors(uint64_t out[8]) { descriptor_totals(out); }
void vk_record_test_wakes(uint64_t out[4]) {   /* cumulative: walker signals, worker-chained signals, polled hits, parks */
    pthread_mutex_lock(&rec_mu);
    out[0] = rec_wakes_walker; out[1] = rec_wakes_chain; out[2] = rec_spin_hits; out[3] = rec_parks;
    pthread_mutex_unlock(&rec_mu);
}
#endif
static void commands_publish(unsigned worker) {
    rec_commands *s = &command_owner[worker];
    atomic_store_explicit(&command_stats[worker].in_use, s->in_use, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].peak, s->peak, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].returns, s->returns, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].cold_resets, s->cold_resets, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].batches, s->batches, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].lease_ns, s->lease_ns, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].return_ns, s->return_ns, memory_order_relaxed);
    atomic_store_explicit(&command_stats[worker].reset_ns, s->reset_ns, memory_order_relaxed);
}
static void commands_costs(uint64_t lease[2], uint64_t returned[2], uint64_t reset[2], uint64_t batches[2]) {
    for (unsigned worker = 0; worker <= REC_WORKERS; worker++) {
        unsigned side = worker != 0;
        lease[side] += atomic_load_explicit(&command_stats[worker].lease_ns, memory_order_relaxed);
        returned[side] += atomic_load_explicit(&command_stats[worker].return_ns, memory_order_relaxed);
        reset[side] += atomic_load_explicit(&command_stats[worker].reset_ns, memory_order_relaxed);
        batches[side] += atomic_load_explicit(&command_stats[worker].batches, memory_order_relaxed);
    }
}
static void commands_check_owner(unsigned worker) {
    rec_commands *s = &command_owner[worker];
    if (!worker) return; /* walking ownership is serialized by the recording lock, not a fixed OS thread */
    if (!s->owner_set) { s->owner = pthread_self(); s->owner_set = 1; }
    else if (!pthread_equal(s->owner, pthread_self())) DIE("recorder command pool used outside its recording owner");
}
static void commands_maintain(unsigned worker, uint64_t epoch) {
    rec_commands *s = &command_owner[worker];
    if (epoch < s->next_maintenance) return;
    s->next_maintenance = epoch + 256;
    uint32_t keep = 512;
    while (keep < (uint64_t)s->recent_peak * 2 && keep <= UINT32_MAX / 2) keep *= 2;
    s->recent_peak = s->in_use;
    if (s->live <= keep) return; /* never scan/reset the ordinary warm working set */
    uint64_t start = host_now_ns();
    int reclaim = s->live >= (uint64_t)keep * 2;
    VkCommandBuffer freed[64]; uint32_t nfree = 0, total = 0, excess = s->live - keep;
    for (uint32_t *link = &s->ready; *link && excess; ) {
        uint32_t id = *link; rec_command *n = &s->command[id - 1];
        if (epoch - n->last_use < 4096) { link = &n->next; continue; }
        excess--;
        if (reclaim) {
            *link = n->next; freed[nfree++] = n->cb;
            n->cb = VK_NULL_HANDLE; n->dirty = 0; n->next = s->vacant; s->vacant = id;
            s->live--; total++;
            if (nfree == 64) { vkFreeCommandBuffers(V.dev, s->pool, nfree, freed); nfree = 0; }
        } else {
            if (n->dirty) {
                VKCHECK(vkResetCommandBuffer(n->cb, 0)); n->dirty = 0; s->cold_resets++;
            }
            link = &n->next;
        }
    }
    if (nfree) vkFreeCommandBuffers(V.dev, s->pool, nfree, freed);
    if (total) {
        vkTrimCommandPool(V.dev, s->pool, 0);
        atomic_fetch_sub(&memory_cbs, total); atomic_fetch_add(&cb_reclaimed, total);
    }
    s->reset_ns += host_now_ns() - start;
}
/* Only the recording owner mutates its pool/free lists. Retirement publishes
   one stable lease chain per owner/slot; draining is O(slots), not O(handles). */
static uint64_t commands_recycle(unsigned worker) {
    if (!atomic_load_explicit(&command_returns[worker], memory_order_acquire)) return 0;
    uint64_t start = host_now_ns(); commands_check_owner(worker);
    rec_commands *s = &command_owner[worker];
    pthread_mutex_lock(&rec_mu);
    for (rec_context *c = s->retired_head; c; ) {
        rec_context *next = c->retired_next;
        s->command[c->lease_tail - 1].next = s->ready; s->ready = c->lease_head;
        s->in_use -= c->ncb; s->returns += c->ncb; s->batches++;
        c->lease_head = c->lease_tail = c->ncb = 0; c->retired_next = NULL; c->retired = 0;
        c = next;
    }
    s->retired_head = s->retired_tail = NULL;
    atomic_store_explicit(&command_returns[worker], 0, memory_order_release);
    pthread_mutex_unlock(&rec_mu);
    s->return_ns += host_now_ns() - start;
    commands_maintain(worker, atomic_load_explicit(&command_epoch, memory_order_relaxed));
    commands_publish(worker);
    return host_now_ns() - start;
}
static VkCommandBuffer commands_acquire(unsigned worker, rec_context *c) {
    rec_commands *s = &command_owner[worker]; commands_check_owner(worker);
    if (!s->ready) (void)commands_recycle(worker);
    uint64_t start = host_now_ns();
    if (!s->pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &pi, NULL, &s->pool));
    }
    if (!s->ready) {
        if (!s->vacant) {
            uint32_t old = s->capacity, capacity = old ? old * 2 : 8;
            if (capacity < old) DIE("recorder command lease index overflow");
            s->command = VK_REALLOC(s->command, (size_t)capacity * sizeof *s->command);
            memset(s->command + old, 0, (size_t)(capacity - old) * sizeof *s->command);
            for (uint32_t i = old; i < capacity; i++) { s->command[i].next = s->vacant; s->vacant = i + 1; }
            s->capacity = capacity;
        }
        uint32_t ids[8], count = 0; VkCommandBuffer cb[8];
        while (count < 8 && s->vacant) {
            ids[count++] = s->vacant; s->vacant = s->command[s->vacant - 1].next;
        }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = count};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, cb));
        for (uint32_t i = 0; i < count; i++) {
            rec_command *n = &s->command[ids[i] - 1];
            n->cb = cb[i]; n->next = s->ready; s->ready = ids[i];
        }
        s->live += count; atomic_fetch_add(&memory_cbs, count);
    }
    uint32_t id = s->ready; rec_command *n = &s->command[id - 1]; s->ready = n->next; n->dirty = 1;
    n->last_use = atomic_load_explicit(&command_epoch, memory_order_relaxed); n->next = 0;
    if (c->lease_tail) s->command[c->lease_tail - 1].next = id; else c->lease_head = id;
    c->lease_tail = id; c->ncb++;
    s->in_use++; if (s->in_use > s->peak) s->peak = s->in_use;
    if (s->in_use > s->recent_peak) s->recent_peak = s->in_use;
    s->lease_ns += host_now_ns() - start;
    return n->cb;
}
/* Called under the backend slot mutex by whichever actual retirement pin
   clears last. No pool operation occurs here, even for an idle worker. */
void vk_record_slot_retire(unsigned slot) {
    if (threads <= 0) return;
    uint64_t start = host_now_ns();
    pthread_mutex_lock(&rec_mu);
    atomic_fetch_add_explicit(&command_epoch, 1, memory_order_relaxed);
    int published = 0;
    for (int worker = 0; worker <= threads; worker++) {
        rec_context *c = contexts[worker][slot];
        if (!c || c->retired || c->generation != slots[slot].generation || !c->ncb) continue;
        rec_commands *s = &command_owner[worker]; c->retired = 1; c->retired_next = NULL;
        if (s->retired_tail) s->retired_tail->retired_next = c; else s->retired_head = c;
        s->retired_tail = c; atomic_store_explicit(&command_returns[worker], 1, memory_order_release);
        published = 1;
    }
    pthread_mutex_unlock(&rec_mu);
    if (published) {
        atomic_fetch_add_explicit(&return_publications, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&return_publish_ns, host_now_ns() - start, memory_order_relaxed);
    }
}
/* The walk owns these size-class lists. A worker only reads blocks of a dispatched job; growth/free
   concerns the unpublished building job, and pruning requires the retired slot handoff. */
static unsigned storage_class(size_t bytes) {
    size_t capacity = REC_STORAGE_MIN; unsigned index = 0;
    while (capacity < bytes && index + 1 < REC_STORAGE_CLASSES) { capacity *= 2; index++; }
    if (capacity < bytes) DIE("recorder storage request too large: %zu bytes", bytes);
    return index;
}
static void *storage_get(rec_storage *storage, size_t bytes, int info, const char *function, const char *name) {
#ifdef TOTK_VK_TESTS
    if (vk_test_heap_failure(name)) DIE("host allocation failed in %s: %s, requested %zu bytes (recorder slab)", function, name, bytes);
#else
    (void)function; (void)name;
#endif
    if (bytes > SIZE_MAX - sizeof(rec_storage_block)) DIE("recorder storage size overflow: %zu bytes", bytes);
    unsigned index = storage_class(bytes + sizeof(rec_storage_block));
    size_t capacity = (size_t)REC_STORAGE_MIN << index;
    rec_storage_block *block = storage->free[index];
    if (block) storage->free[index] = block->next;
    else {
        rec_storage_slab *slab = storage->slabs;
        while (slab && (slab->bytes > REC_STORAGE_SLAB || capacity > slab->bytes - slab->used)) slab = slab->next;
        if (!slab) {
            size_t need = sizeof *slab + capacity;
            size_t mapped = (need + REC_STORAGE_SLAB - 1) & ~(size_t)(REC_STORAGE_SLAB - 1);
#ifdef TOTK_VK_TESTS
            if (vk_test_heap_failure("recorder-slab")) { errno = ENOMEM; slab = MAP_FAILED; }
            else
#endif
            slab = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (slab == MAP_FAILED) DIE("recorder %s slab mmap failed: %zu bytes: %s", info ? "descriptor" : "draw", mapped, strerror(errno));
            slab->next = storage->slabs; slab->bytes = mapped; slab->used = sizeof *slab; slab->live = 0; storage->slabs = slab;
            STORAGE_POISON(slab->data, mapped - sizeof *slab);
            storage->bytes += mapped;
            atomic_fetch_add_explicit(info ? &memory_info : &memory_draws, mapped, memory_order_relaxed);
#if defined(__linux__) && defined(PR_SET_VMA) && defined(PR_SET_VMA_ANON_NAME)
            /* Naming takes mmap_lock for write. Keep it out of ordinary slab growth on the walker. */
            if (vma_names) (void)prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, slab, mapped, info ? "totk-rec-info" : "totk-rec-draw");
#endif
        }
        block = (rec_storage_block *)((unsigned char *)slab + slab->used); slab->used += capacity;
        STORAGE_UNPOISON(block, sizeof *block);
        block->bytes = capacity; block->slab = slab;
    }
    block->slab->live += capacity; storage_referenced += capacity;
    STORAGE_UNPOISON(block + 1, info ? capacity - sizeof *block : bytes);
    return block + 1;
}
static void storage_put(rec_storage *storage, void *address) {
    if (!address) return;
    rec_storage_block *block = (rec_storage_block *)address - 1;
    unsigned index = storage_class(block->bytes);
    block->slab->live -= block->bytes; storage_referenced -= block->bytes;
    STORAGE_POISON(address, block->bytes - sizeof *block);
    block->next = storage->free[index]; storage->free[index] = block;
}
static uint32_t storage_capacity(const void *address, size_t element) {
    const rec_storage_block *block = (const rec_storage_block *)address - 1;
    size_t count = (block->bytes - sizeof *block) / element;
    if (count > UINT32_MAX) DIE("recorder storage capacity overflow");
    return (uint32_t)count;
}
/* Preserve current-scene arrays even when their working set exceeds the idle retention
   allowance. Unlink all free blocks from an empty slab before releasing its mapping. */
static uint64_t storage_trim(rec_storage *storage, int info) {
    int empty = 0;
    for (rec_storage_slab *s = storage->slabs; s; s = s->next) empty |= !s->live;
    if (!empty) return 0;
    for (unsigned i = 0; i < REC_STORAGE_CLASSES; i++) {
        for (rec_storage_block **link = &storage->free[i]; *link; ) {
            rec_storage_block *b = *link;
            if (!b->slab->live) *link = b->next; else link = &b->next;
        }
    }
    uint64_t bytes = 0;
    for (rec_storage_slab **link = &storage->slabs; *link; ) {
        rec_storage_slab *s = *link;
        if (s->live) { link = &s->next; continue; }
        *link = s->next; size_t size = s->bytes; bytes += size;
        STORAGE_UNPOISON(s, size);
        if (munmap(s, size)) DIE("recorder slab munmap failed: %s", strerror(errno));
    }
    storage->bytes -= bytes;
    atomic_fetch_sub_explicit(info ? &memory_info : &memory_draws, bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&storage_reclaimed, bytes, memory_order_relaxed);
    return bytes;
}
void vk_record_memory(uint64_t bytes[4]) {
    bytes[0] = atomic_load_explicit(&memory_draws, memory_order_relaxed);
    bytes[1] = atomic_load_explicit(&memory_info, memory_order_relaxed);
    bytes[2] = atomic_load_explicit(&memory_cbs, memory_order_relaxed);
    bytes[3] = atomic_load_explicit(&memory_dpools, memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
void vk_record_test_storage(uint64_t values[4]) {
    values[0] = storage_limit; values[1] = atomic_load(&storage_reclaims);
    values[2] = atomic_load(&storage_reclaimed); values[3] = atomic_load(&cb_reclaimed);
}
void vk_record_test_leases(uint64_t values[4]) {
    memset(values, 0, 4 * sizeof *values);
    for (int worker = 0; worker <= REC_WORKERS; worker++) {
        values[0] += atomic_load(&command_stats[worker].in_use);
        values[1] += atomic_load(&command_stats[worker].peak);
        values[2] += atomic_load(&command_stats[worker].returns);
        values[3] += atomic_load(&command_stats[worker].cold_resets);
    }
}
void vk_record_test_costs_get(vk_record_test_costs *out) {
    memset(out, 0, sizeof *out);
    commands_costs(out->lease_ns, out->return_ns, out->reset_ns, out->return_batches);
    out->publish_ns = atomic_load_explicit(&return_publish_ns, memory_order_relaxed);
    out->publications = atomic_load_explicit(&return_publications, memory_order_relaxed);
    out->slab_trim_ns = storage_trim_ns; out->referenced_bytes = storage_referenced;
    vk_record_test_arena_costs(out);
}
#endif
static uint64_t memo_miss[2][VK_REC_M_N];
static uint64_t memo_saved[VK_REC_M_N];
void vk_record_memo_saved(unsigned cause) { memo_saved[cause]++; }
void vk_record_memo_miss(unsigned program, unsigned causes) {
    while (causes) { unsigned bit = (unsigned)__builtin_ctz(causes); memo_miss[program != 0][bit]++; causes &= causes - 1; }
}
static void memo_miss_log(unsigned program) {
    static const char *names[] = {"uniform", "binding", "span", "program", "signature", "registry", "sampler",
        "chunk", "submission", "shader", "dimensions", "cold", "disabled/writable"};
    unsigned top[3] = {VK_REC_M_N, VK_REC_M_N, VK_REC_M_N};
    for (unsigned i = 0; i < VK_REC_M_N; i++) for (unsigned k = 0; k < 3; k++)
        if (top[k] == VK_REC_M_N || memo_miss[program][i] > memo_miss[program][top[k]]) {
            for (unsigned n = 2; n > k; n--) top[n] = top[n - 1]; top[k] = i; break;
        }
    LOGV("recorder %s miss causes/frame (overlap): %s %.1f, %s %.1f, %s %.1f; uniform %.1f binding %.1f span %.1f program %.1f",
        program ? "program" : "freeze", names[top[0]], memo_miss[program][top[0]] / 300.0,
        names[top[1]], memo_miss[program][top[1]] / 300.0, names[top[2]], memo_miss[program][top[2]] / 300.0,
        memo_miss[program][VK_REC_M_UNIFORM] / 300.0, memo_miss[program][VK_REC_M_BINDING] / 300.0,
        memo_miss[program][VK_REC_M_SPAN] / 300.0, memo_miss[program][VK_REC_M_PROGRAM] / 300.0);
    memset(memo_miss[program], 0, sizeof memo_miss[program]);
}
#ifdef TOTK_VK_TESTS
static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cv = PTHREAD_COND_INITIALIZER;
static int test_paused, test_waiting;
static uint32_t test_min_draws;
static _Atomic uint32_t test_delay_us;
void vk_record_test_delay(uint32_t maximum_us) { atomic_store(&test_delay_us, maximum_us); }
static vk_record_test_vertex test_vertex;
static _Atomic uint64_t test_checks, test_failures, test_resets;
void vk_record_test_pause_draws(uint32_t minimum) {
    pthread_mutex_lock(&test_mu); test_paused = vk_record_enabled(); test_waiting = 0; test_min_draws = minimum;
    atomic_store(&test_checks, 0); atomic_store(&test_failures, 0); atomic_store(&test_resets, 0);
    pthread_mutex_unlock(&test_mu);
}
void vk_record_test_pause(void) { vk_record_test_pause_draws(0); }
static void test_worker_gate(rec_job *batch) {
    pthread_mutex_lock(&test_mu); int matches = !test_min_draws;
    for (rec_job *j = batch; !matches && j; j = j->batch_next) matches = j->count >= test_min_draws;
    if (test_paused && matches) {
        test_waiting++; pthread_cond_broadcast(&test_cv);
        while (test_paused) pthread_cond_wait(&test_cv, &test_mu);
        test_waiting--;
    }
    pthread_mutex_unlock(&test_mu);
    uint32_t maximum = atomic_load(&test_delay_us);
    if (maximum) {
        uint64_t random = batch->id * UINT64_C(0x9e3779b97f4a7c15) ^ batch->guard_ticket;
        random ^= random >> 30; random *= UINT64_C(0xbf58476d1ce4e5b9);
        uint32_t us = (uint32_t)(random % maximum);
        struct timespec delay = {.tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000};
        nanosleep(&delay, NULL);
    }
}
int vk_record_test_wait_paused(void) {
    if (!vk_record_enabled()) return 1;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 5;
    pthread_mutex_lock(&test_mu);
    while (!test_waiting) if (pthread_cond_timedwait(&test_cv, &test_mu, &until)) break;
    int waiting = test_waiting; pthread_mutex_unlock(&test_mu); return waiting != 0;
}
void vk_record_test_release(void) {
    pthread_mutex_lock(&test_mu); test_paused = 0; pthread_cond_broadcast(&test_cv); pthread_mutex_unlock(&test_mu);
}
void vk_record_test_expect_vertex(uint64_t address, VkBuffer buffer, uint64_t offset) { test_vertex = (vk_record_test_vertex){address, buffer, offset}; }
void vk_record_test_results(uint64_t *checks, uint64_t *failures, uint64_t *resets) {
    *checks = atomic_load(&test_checks); *failures = atomic_load(&test_failures); *resets = atomic_load(&test_resets);
}
#endif

/* SAMPLER matches SAMPLED_IMAGE (one separate sampler per sampled image): at 4096 it was one per set in a primary
   slot's 4096-set pool, so any layout with two sampler bindings filled that pool at half its sets. No layout uses
   INPUT_ATTACHMENT (rendering is dynamic, without subpasses) or the *_DYNAMIC types (offsets are resolved into each
   descriptor): they are not declared, since every declared descriptor is driver memory reserved in every pool. */
const VkDescriptorPoolSize vk_desc_pool_sizes[VK_DESC_POOL_TYPES] = {
    {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},
    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192}};
int vk_desc_layout_fits(const VkDescriptorSetLayoutBinding *bindings, uint32_t count, char *why, size_t size) {
    uint64_t need[VK_DESC_POOL_TYPES] = {0};
    for (uint32_t i = 0; i < count; i++) {
        unsigned t = 0;
        while (t < VK_DESC_POOL_TYPES && vk_desc_pool_sizes[t].type != bindings[i].descriptorType) t++;
        if (t == VK_DESC_POOL_TYPES) {
            snprintf(why, size, "binding %u has descriptor type %d, which no descriptor pool declares",
                bindings[i].binding, (int)bindings[i].descriptorType);
            return 0;
        }
        need[t] += bindings[i].descriptorCount;
        if (need[t] > vk_desc_pool_sizes[t].descriptorCount) {
            snprintf(why, size, "%llu descriptors of type %d exceed a pool's %u",
                (unsigned long long)need[t], (int)bindings[i].descriptorType, vk_desc_pool_sizes[t].descriptorCount);
            return 0;
        }
    }
    return 1;
}
/* Approximate driver memory of one pool, for the descriptors line: every declared descriptor at 64 bytes, a combined
   image sampler at 128 and a sampler at 16 (Adreno descriptor sizes; a driver that reserves a pool's descriptors at
   creation keeps them while the pool exists, used or reset). ~7.4 MiB. */
static uint64_t desc_pool_bytes(void) {
    uint64_t bytes = 0;
    for (unsigned t = 0; t < VK_DESC_POOL_TYPES; t++)
        bytes += (uint64_t)vk_desc_pool_sizes[t].descriptorCount *
            (vk_desc_pool_sizes[t].type == VK_DESCRIPTOR_TYPE_SAMPLER ? 16 :
             vk_desc_pool_sizes[t].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? 128 : 64);
    return bytes;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_descriptor_retained(uint64_t out[4]) {
    out[0] = atomic_load(&memory_dpools); out[1] = atomic_load(&memory_primary_dpools);
    out[2] = atomic_load(&stock_sets_retained); out[3] = desc_pool_bytes();
}
void vk_record_test_stock_events(uint64_t out[VK_DESC_STOCK_REASONS + 1]) { stock_totals(out); }
#endif
/* Allocates count (1..VK_DESC_STOCK_BATCH) sets, fewer near the end of a pool; never fewer than one. */
static uint32_t alloc_sets(void *owner, VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *out) {
    rec_context *c = owner;
    VkDescriptorSetLayout layouts[VK_DESC_STOCK_BATCH];
    for (uint32_t i = 0; i < count; i++) layouts[i] = layout;
    int fresh = 0;
    for (;;) {
        if (c->curdp < c->ndp && c->sets < REC_POOL_SETS) {
            if (count > REC_POOL_SETS - c->sets) count = REC_POOL_SETS - c->sets;
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = c->dp[c->curdp], .descriptorSetCount = count, .pSetLayouts = layouts};
            c->descriptor_calls++;
            VkResult r = vkAllocateDescriptorSets(V.dev, &ai, out);
            if (!r) { c->sets += count; c->descriptor_sets += count; return count; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) DIE("recorder descriptor allocation: %d", r);
            /* Large layouts may fit fewer than a whole batch, even in a fresh pool: never reject one that fits alone. */
            if (count > 1) { count /= 2; continue; }
        }
        if (c->curdp + 1 < c->ndp) { c->curdp++; c->sets = 0; continue; }
        if (fresh) DIE("recorder descriptor set exceeds a fresh pool");
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = REC_POOL_SETS,
            .poolSizeCount = VK_DESC_POOL_TYPES, .pPoolSizes = vk_desc_pool_sizes};
        if (c->ndp == c->capdp) { c->capdp = c->capdp ? c->capdp * 2 : 4; c->dp = VK_REALLOC(c->dp, c->capdp * sizeof *c->dp); }
        VKCHECK(vkCreateDescriptorPool(V.dev, &pi, NULL, &c->dp[c->ndp++]));
        atomic_fetch_add_explicit(&memory_dpools, 1, memory_order_relaxed);
        c->curdp = c->ndp - 1; c->sets = 0; fresh = 1;
    }
}
/* Descriptor-set stock (TOTK_VK_DESC_STOCK=1): per owner, per exact immutable layout. Shared by recorder contexts
   and the primary slots in nvn_vk.c; each owner rewinds its stock only where it used to reset its pools. */
typedef struct desc_layout_stock {
    struct desc_layout_stock *next;
    VkDescriptorSet *set; uint32_t count, used, capacity;
} desc_layout_stock;
struct vk_desc_stock {
    nvn_hmap layouts; desc_layout_stock *list;
    uint64_t sets, used; uint32_t pressure;
    uint32_t max, over;   /* this generation's ceiling (0: none); checkouts past it, allocated outside the stock */
};
int vk_desc_stock_enabled(void) { (void)vk_record_enabled(); return descriptor_stock; }
/* Live (runtime/live_ctl.h): every owner reads this at its slot generation handoff, where its pools used to be reset,
   and switches there (vk_desc_stock_want). Turned off, the owner frees its stock and destroys its pools, so the
   retained sets and their pool memory go back to the driver; the off policy then grows pools again as needed. */
static totk_live_cache stock_live;
static _Atomic int stock_reported = -1;
int vk_desc_stock_want(void) {
    int on = totk_live_cached(&stock_live, "TOTK_VK_DESC_STOCK", vk_desc_stock_enabled()) != 0;
    if (atomic_load_explicit(&stock_reported, memory_order_relaxed) != on &&
        atomic_exchange_explicit(&stock_reported, on, memory_order_relaxed) != on && totk_live_generation())
        LOGV("descriptor stock: %s (live; each recorder context and primary slot switches at its next slot generation%s)",
             on ? "on" : "off", on ? "" : " and returns its sets and pools");
    return on;
}
/* TOTK_VK_DESC_STOCK_MAX_SETS, live like the stock itself: every owner takes it at its generation handoff. */
static totk_live_cache ceiling_live;
static _Atomic int64_t ceiling_reported = -1;
uint32_t vk_desc_stock_ceiling(void) {
    (void)vk_record_enabled();
    int v = totk_live_cached(&ceiling_live, "TOTK_VK_DESC_STOCK_MAX_SETS", (int)descriptor_stock_max);
    uint32_t max = v > 0 ? (uint32_t)v : 0;
    if (atomic_load_explicit(&ceiling_reported, memory_order_relaxed) != max &&
        atomic_exchange_explicit(&ceiling_reported, max, memory_order_relaxed) != max && totk_live_generation())
        LOGV("descriptor stock ceiling: %u sets per owner (live; 0 = none; each owner takes it at its next slot generation)", max);
    return max;
}
/* Enough pools for the ceiling's sets, plus one for batches cut at a pool's end and layouts that fill a pool by a
   descriptor type before its sets. */
uint32_t vk_desc_stock_pool_limit(uint32_t ceiling, uint32_t pool_sets) {
    return ceiling ? (ceiling + pool_sets - 1) / pool_sets + 1 : UINT32_MAX;
}
void vk_desc_stock_free(vk_desc_stock **stock) {
    vk_desc_stock *st = *stock; if (!st) return;
    atomic_fetch_sub_explicit(&stock_sets_retained, st->sets, memory_order_relaxed);
    while (st->list) { desc_layout_stock *s = st->list; st->list = s->next; free(s->set); free(s); }
    free(st->layouts.keys); free(st->layouts.vals); free(st); *stock = NULL;
}
/* MIT mechanism reference: Nycolazs/LASRecomp-Android-Plus @cca791a,
   runtime/nvn/vulkan/record.c:308-436. Recycle storage, NOT descriptor contents: every checkout is rewritten.
   Each context belongs to one recording owner and one submission slot; generation changes follow that slot's
   own GPU/post/submit retirement, not a global completed-ticket estimate. */
VkDescriptorSet vk_desc_stock_take(vk_desc_stock **stock, VkDescriptorSetLayout layout, vk_desc_stock_alloc alloc, void *owner) {
    vk_desc_stock *st = *stock;
    if (!st) { st = *stock = VK_CALLOC(1, sizeof *st); st->layouts.klen = sizeof layout; st->max = vk_desc_stock_ceiling(); }
    desc_layout_stock *s = nvn_hm_get(&st->layouts, &layout);
    if (!s) { s = VK_CALLOC(1, sizeof *s); s->next = st->list; st->list = s; nvn_hm_put(&st->layouts, &layout, s); }
    if (s->used == s->count) {
        uint32_t batch = s->count ? (s->count < VK_DESC_STOCK_BATCH ? s->count : VK_DESC_STOCK_BATCH) : 1; /* 1,1,2,4,...32 */
        if (st->max && st->sets + batch > st->max) {
            if (st->sets >= st->max) {   /* full: this set lives only until the handoff, which resets the owner */
                VkDescriptorSet set; (void)alloc(owner, layout, 1, &set);
                st->over++; return set;
            }
            batch = (uint32_t)(st->max - st->sets);
        }
        if (s->count > UINT32_MAX - batch) DIE("descriptor stock overflow");
        uint32_t needed = s->count + batch;
        if (needed > s->capacity) {
            uint32_t capacity = s->capacity ? (s->capacity <= UINT32_MAX / 2 ? s->capacity * 2 : UINT32_MAX) : 64;
            s->capacity = capacity > needed ? capacity : needed;
            s->set = VK_REALLOC(s->set, (size_t)s->capacity * sizeof *s->set);
        }
        uint32_t n = alloc(owner, layout, batch, s->set + s->count);
        s->count += n; st->sets += n; atomic_fetch_add_explicit(&stock_sets_retained, n, memory_order_relaxed);
    }
    st->used++;
    return s->set[s->used++];
}
int vk_desc_stock_retire(vk_desc_stock *st, uint32_t ceiling, uint32_t pools, uint32_t pool_sets) {
    if (!st) return 0;
    /* Layout churn must not retain every historical peak forever. Rebuild only after 32 consecutive retired
       generations using less than a quarter of the stock (minimum retained budget 1024 sets). */
    uint64_t keep = st->used * 4;
    if (keep < 1024) keep = 1024;
    int reason = 0;
    if (st->sets > keep) { if (++st->pressure == 32) reason = VK_DESC_STOCK_PRESSURE; }
    else st->pressure = 0;
    /* The ceiling (the one just taken, so a lower value applies at once): a generation that needed more sets, a stock
       above it, or more pools than it allows resets the owner instead of growing. */
    if (ceiling && pools > vk_desc_stock_pool_limit(ceiling, pool_sets)) reason = VK_DESC_STOCK_POOL_LIMIT;
    if (ceiling && (st->over || st->sets > ceiling)) reason = VK_DESC_STOCK_SET_LIMIT;
    for (desc_layout_stock *s = st->list; s; s = s->next) { s->used = 0; if (reason) s->count = 0; }
    st->used = 0; st->over = 0; st->max = ceiling;
    if (reason) { atomic_fetch_sub_explicit(&stock_sets_retained, st->sets, memory_order_relaxed); st->sets = st->pressure = 0; }
    return reason;
}
static VkDescriptorSet alloc_set(rec_context *c, VkDescriptorSetLayout layout) {
    if (c->stock_on) return vk_desc_stock_take(&c->stock, layout, alloc_sets, c);
    VkDescriptorSet set; (void)alloc_sets(c, layout, 1, &set); return set;
}
/* Pools past keep are empty (reset, or never used since the last rebuild): destroyed, their driver memory goes back. */
static void descriptors_trim(rec_context *c, uint32_t keep) {
    if (keep >= c->ndp) return;
    for (uint32_t i = keep; i < c->ndp; i++) vkDestroyDescriptorPool(V.dev, c->dp[i], NULL);
    atomic_fetch_sub_explicit(&memory_dpools, c->ndp - keep, memory_order_relaxed);
    c->stock_events[VK_DESC_STOCK_REASONS] += c->ndp - keep; c->ndp = keep;
}
static void descriptors_recycle(rec_context *c) {
    int want = vk_desc_stock_want();
    if (c->stock_on && !want) {   /* switched off live: give the stock's sets and pools back */
        vk_desc_stock_free(&c->stock);
        descriptors_trim(c, 0); c->stock_events[VK_DESC_STOCK_OFF]++;
        c->curdp = c->sets = 0; c->stock_on = 0;
        return;
    }
    uint32_t ceiling = want ? vk_desc_stock_ceiling() : 0, keep = c->ndp;
    if (c->stock_on) {
        int reason = vk_desc_stock_retire(c->stock, ceiling, c->ndp, REC_POOL_SETS);
        if (!reason) return;
        c->stock_events[reason]++;
        if (reason != VK_DESC_STOCK_PRESSURE) keep = vk_desc_stock_pool_limit(ceiling, REC_POOL_SETS);
    } else if (want && c->ndp > vk_desc_stock_pool_limit(ceiling, REC_POOL_SETS)) {   /* switched on live over the limit */
        keep = vk_desc_stock_pool_limit(ceiling, REC_POOL_SETS); c->stock_events[VK_DESC_STOCK_POOL_LIMIT]++;
    }
    /* A stock rebuild resets only pools allocated since the previous one; the default keeps the old policy. */
    uint32_t dirty = c->stock_on ? (c->ndp ? c->curdp + 1 : 0) : c->ndp;
    if (dirty > keep) dirty = keep;
    for (uint32_t i = 0; i < dirty; i++) { VKCHECK(vkResetDescriptorPool(V.dev, c->dp[i], 0)); c->descriptor_resets++; }
    descriptors_trim(c, keep);
    c->curdp = c->sets = 0;
    c->stock_on = want;   /* switched on live: the stock starts empty on reset pools */
}
#ifdef TOTK_VK_TESTS
int vk_record_test_descriptor_stock(void) {
    (void)vk_record_enabled();
    rec_context *c = VK_CALLOC(1, sizeof *c); c->stock_on = vk_desc_stock_want();
    VkDescriptorSetLayout layout[2]; VkDescriptorSet first[2][100]; int ok = 1;
    int unlimited = descriptor_stock && !vk_desc_stock_ceiling();   /* the exact counts below; a ceiling has its own fixture */
    for (unsigned l = 0; l < 2; l++) {
        VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorCount = 1,
            .descriptorType = l ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &binding};
        VKCHECK(vkCreateDescriptorSetLayout(V.dev, &ci, NULL, &layout[l]));
    }
    /* No GPU uses these sets: each explicit recycle models one completed owning-slot generation.
       Actual fence/worker/writable behaviour is checked by the ordinary cross-mode draw/lag fixtures. */
    for (unsigned epoch = 0; epoch < 10; epoch++) {
        VkDescriptorSet current[2][100];
        for (unsigned l = 0; l < 2; l++) for (unsigned i = 0; i < 100; i++) {
            VkDescriptorSet set = current[l][i] = alloc_set(c, layout[l]);
            for (unsigned k = 0; k < i; k++) ok &= current[l][k] != set;
            if (l) for (unsigned k = 0; k < 100; k++) ok &= current[0][k] != set;
            if (!epoch) first[l][i] = set;
            else if (unlimited) ok &= first[l][i] == set;
        }
        descriptors_recycle(c);
    }
    if (unlimited) ok &= c->descriptor_sets == 256 && c->descriptor_calls == 18 && !c->descriptor_resets;
    else if (!descriptor_stock) ok &= c->descriptor_sets == 2000 && c->descriptor_calls == 2000 && c->descriptor_resets == 10;
    printf("descriptor-stock stock=%d ten-generations: sets=%llu calls=%llu resets=%llu pools=%u\n", descriptor_stock,
        (unsigned long long)c->descriptor_sets, (unsigned long long)c->descriptor_calls,
        (unsigned long long)c->descriptor_resets, c->ndp);
    if (unlimited) {
        for (unsigned i = 0; i < 1400; i++) (void)alloc_set(c, layout[0]);
        descriptors_recycle(c); uint64_t resets = c->descriptor_resets;
        for (unsigned epoch = 0; epoch < 31; epoch++) {
            (void)alloc_set(c, layout[1]); descriptors_recycle(c);
            ok &= c->descriptor_resets == resets;
        }
        (void)alloc_set(c, layout[1]); descriptors_recycle(c);
        ok &= c->descriptor_resets > resets && !c->stock->sets;
        uint64_t previous = c->descriptor_sets;
        (void)alloc_set(c, layout[0]); ok &= c->descriptor_sets == previous + 1;
        printf("descriptor-stock pressure: resets=%llu; rebuild invalidates every old stock before checkout\n",
            (unsigned long long)(c->descriptor_resets - resets));
    }
    for (uint32_t i = 0; i < c->ndp; i++) vkDestroyDescriptorPool(V.dev, c->dp[i], NULL);
    atomic_fetch_sub_explicit(&memory_dpools, c->ndp, memory_order_relaxed);
    vk_desc_stock_free(&c->stock);
    free(c->dp); free(c);
    for (unsigned l = 0; l < 2; l++) vkDestroyDescriptorSetLayout(V.dev, layout[l], NULL);
    return ok;
}
static void test_live(const char *lines) {   /* stage and apply like totk.ctl at a presented frame */
    totk_live_lines l = {{0}}; totk_live_feed(&l, lines, strlen(lines)); totk_live_frame(0);
}
/* TOTK_VK_DESC_STOCK_MAX_SETS on one recorder context, set live at its handoffs (no GPU uses these sets: each
   recycle models one completed owning-slot generation, as above). */
int vk_record_test_descriptor_ceiling(void) {
    (void)vk_record_enabled();
    test_live("set TOTK_VK_DESC_STOCK=1\nset TOTK_VK_DESC_STOCK_MAX_SETS=64\n");
    rec_context *c = VK_CALLOC(1, sizeof *c); c->stock_on = vk_desc_stock_want();
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout layout; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &ci, NULL, &layout));
    int ok = c->stock_on && vk_desc_stock_ceiling() == 64;
    /* A generation of 100 checkouts: the stock stops at 64, the rest are distinct sets outside it. */
    VkDescriptorSet seen[100];
    for (unsigned i = 0; i < 100; i++) {
        seen[i] = alloc_set(c, layout);
        for (unsigned k = 0; k < i; k++) ok &= seen[k] != seen[i];
    }
    ok &= c->stock->sets == 64 && c->stock->over == 36;
    descriptors_recycle(c);
    ok &= c->stock_events[VK_DESC_STOCK_SET_LIMIT] == 1 && !c->stock->sets && c->ndp <= vk_desc_stock_pool_limit(64, REC_POOL_SETS);
    /* Under the ceiling the stock is reused: 40 checkouts grow it in batches to 64 (the last one cut at the ceiling),
       and a second generation of 40 allocates nothing. */
    for (unsigned i = 0; i < 40; i++) (void)alloc_set(c, layout);
    descriptors_recycle(c); uint64_t allocated = c->descriptor_sets;
    for (unsigned i = 0; i < 40; i++) (void)alloc_set(c, layout);
    descriptors_recycle(c);
    ok &= c->descriptor_sets == allocated && c->stock->sets == 64 && c->stock_events[VK_DESC_STOCK_SET_LIMIT] == 1;
    /* Lowered live below the stock: the next handoff resets at once. */
    test_live("set TOTK_VK_DESC_STOCK_MAX_SETS=16\n");
    descriptors_recycle(c);
    ok &= c->stock_events[VK_DESC_STOCK_SET_LIMIT] == 2 && !c->stock->sets;
    /* One set: 3,000 checkouts need three 1,024-set pools; the handoff keeps two and destroys the third. */
    test_live("set TOTK_VK_DESC_STOCK_MAX_SETS=1\n");
    descriptors_recycle(c);
    for (unsigned i = 0; i < 3000; i++) (void)alloc_set(c, layout);
    uint32_t peak = c->ndp; uint64_t destroyed = c->stock_events[VK_DESC_STOCK_REASONS];
    descriptors_recycle(c);
    ok &= peak == 3 && c->ndp == vk_desc_stock_pool_limit(1, REC_POOL_SETS) && c->ndp == 2 &&
        c->stock_events[VK_DESC_STOCK_REASONS] == destroyed + 1;
    /* Switched off live: stock and every pool go. */
    test_live("set TOTK_VK_DESC_STOCK=0\n");
    descriptors_recycle(c);
    ok &= !c->stock && !c->ndp && !c->stock_on && c->stock_events[VK_DESC_STOCK_OFF] == 1;
    printf("descriptor-stock ceiling: set-limit resets %llu, pool-limit resets %llu, off %llu, pools destroyed %llu, peak pools %u\n",
        (unsigned long long)c->stock_events[VK_DESC_STOCK_SET_LIMIT], (unsigned long long)c->stock_events[VK_DESC_STOCK_POOL_LIMIT],
        (unsigned long long)c->stock_events[VK_DESC_STOCK_OFF], (unsigned long long)c->stock_events[VK_DESC_STOCK_REASONS], peak);
    test_live("unset TOTK_VK_DESC_STOCK\nunset TOTK_VK_DESC_STOCK_MAX_SETS\n");
    free(c->dp); free(c);
    vkDestroyDescriptorSetLayout(V.dev, layout, NULL);
    return ok;
}
#endif
static VkDescriptorBufferInfo zero_buffer(rec_context *c) {
    if (!c->zero.buffer) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &c->zero.buffer));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->zero.buffer, &req);
        int type = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) DIE("recorder zero buffer has no coherent host memory");
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
        VkDeviceMemory memory; VKCHECK(vk_memory_allocate(&ai, &memory, VK_MEMORY_ZERO));
        VKCHECK(vkBindBufferMemory(V.dev, c->zero.buffer, memory, 0));
        void *bytes; VKCHECK(vkMapMemory(V.dev, memory, 0, 65536, 0, &bytes)); memset(bytes, 0, 65536); vkUnmapMemory(V.dev, memory);
        c->zero.range = 256;
    }
    return c->zero;
}
/* Only retain resolved intervals during recording. Even a guard-only primary operation can be followed by
   a walking CPU write to the same dirty unit; no future ticket may be armed until the slot is ready to queue. */
static void collect_guard(rec_job *j, const vk_record_spans *spans, VkBuffer buffer,
                          uint64_t offset, uint64_t size, uint64_t ticket, uint32_t reason) {
    if (!ticket || !reason || !size) return;
    if (ticket != j->guard_ticket) { vk_record_lag_fail(ticket); return; }
    if (!j->guard_cache) {
        j->guard_cache = calloc(REC_GUARD_CACHE, sizeof *j->guard_cache);
        if (!j->guard_cache) { vk_record_lag_fail(ticket); return; }
    }
    unsigned h = (unsigned)(((uintptr_t)spans >> 4) ^ ((uintptr_t)buffer >> 4) ^
                            offset ^ (offset >> 32) ^ size ^ (size >> 32)) % REC_GUARD_CACHE;
    uint32_t at = j->guard_cache[h];
    if (at) {
        rec_guard *g = &j->guards[at - 1];
        if (g->spans == spans && g->buffer == buffer && g->offset == offset && g->size == size) {
            g->reason |= reason; return;
        }
    }
    if (j->nguards == j->capguards) {
        size_t cap = j->capguards ? (size_t)j->capguards * 2 : 32;
        rec_guard *p = cap <= UINT32_MAX && cap <= SIZE_MAX / sizeof *p ?
            realloc(j->guards, cap * sizeof *p) : NULL;
        if (!p) { vk_record_lag_fail(ticket); return; }
        j->guards = p; j->capguards = (uint32_t)cap;
    }
    j->guards[j->nguards++] = (rec_guard){spans, buffer, offset, size, reason};
    j->guard_cache[h] = j->nguards;
}
static void project_guards(rec_job *j) {
    if (!j->nguards) return;
    uint64_t start = host_now_ns();
    int native = vk_record_lag_native();
    for (uint32_t i = 0; i < j->nguards; i++) {
        const rec_guard *g = &j->guards[i];
        vk_record_lag_collect(g->spans, g->buffer, g->offset, g->size, j->guard_ticket, g->reason,
                              native ? &j->units : NULL, &j->projected, &j->nprojected, &j->capprojected,
                              &j->projected_ranges);
    }
    j->prep_ns += host_now_ns() - start;
    j->nguards = 0;
    memset(j->guard_cache, 0, REC_GUARD_CACHE * sizeof *j->guard_cache);
}
/* Shader layouts have immutable binding order and process lifetime. Only the walking owner touches this map. */
VkDescriptorUpdateTemplate vk_record_template(VkDescriptorSetLayout layout, const vk_record_descriptor *input, uint32_t count) {
    uint64_t start = __builtin_expect(vk_rec_timing_on, 0) ? host_now_ns() : 0; descriptor_templates++;
    static nvn_hmap templates = {.klen = sizeof layout};
    VkDescriptorUpdateTemplate *found = nvn_hm_get(&templates, &layout);
    if (found) { if (start) descriptor_template_ns += host_now_ns() - start; return *found; }
    static VkDescriptorUpdateTemplateEntry *entries; static uint32_t capacity;
    if (count > capacity) { capacity = count + 16; entries = VK_REALLOC(entries, capacity * sizeof *entries); }
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const vk_record_descriptor *d = &input[i];
        if (n && entries[n - 1].dstBinding == d->binding && entries[n - 1].descriptorType == d->type &&
            entries[n - 1].dstArrayElement + entries[n - 1].descriptorCount == d->element) { entries[n - 1].descriptorCount++; continue; }
        size_t offset = d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ?
            offsetof(vk_record_descriptor, b) : d->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ?
            offsetof(vk_record_descriptor, t) : offsetof(vk_record_descriptor, i);
        entries[n++] = (VkDescriptorUpdateTemplateEntry){.dstBinding = d->binding, .dstArrayElement = d->element,
            .descriptorCount = 1, .descriptorType = d->type, .offset = i * sizeof *input + offset, .stride = sizeof *input};
    }
    VkDescriptorUpdateTemplateCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = n, .pDescriptorUpdateEntries = entries,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = layout};
    found = VK_MALLOC(sizeof *found); VKCHECK(vkCreateDescriptorUpdateTemplate(V.dev, &ci, NULL, found));
    nvn_hm_put(&templates, &layout, found);
    if (start) descriptor_template_ns += host_now_ns() - start;
    return *found;
}
static VkDescriptorSet descriptor_set(rec_context *c, rec_job *j, VkDescriptorSetLayout layout, VkDescriptorUpdateTemplate update, const vk_record_descriptor *input, uint32_t count, uint64_t ticket) {
    if (count > c->capinfo) { c->capinfo = count + 16; c->info = VK_REALLOC(c->info, c->capinfo * sizeof *c->info); }
    vk_record_descriptor *info = c->info; memcpy(info, input, count * sizeof *info);
    for (uint32_t i = 0; i < count; i++) {
        vk_record_descriptor *d = &info[i];
        if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (!d->b.buffer) {
                VkBuffer buffer; uint64_t offset, available;
                if (d->raw.address && vk_record_resolve(c->spans, d->raw.address, 1, &buffer, &offset, &available, c->hints)) {
                    uint64_t limit = d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? V.props.limits.maxStorageBufferRange : V.props.limits.maxUniformBufferRange;
                    uint64_t range = !d->raw.size || d->raw.size > limit ? limit : d->raw.size;
                    d->b = (VkDescriptorBufferInfo){buffer, offset, range < available ? range : available};
                } else d->b = zero_buffer(c);
            }
        } else if (d->i.imageView) d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    uint64_t h[2] = {0xcbf29ce484222325ull, 0x6a09e667f3bcc909ull};
    const uint8_t *bytes = (const uint8_t *)info; size_t n = count * sizeof *info, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t word; memcpy(&word, bytes + i, 8);
        h[0] = (h[0] ^ word) * 0x9e3779b97f4a7c15ull; h[0] ^= h[0] >> 32;
        h[1] = (h[1] + word) * 0xc2b2ae3d27d4eb4full; h[1] = h[1] << 31 | h[1] >> 33;
    }
    for (; i < n; i++) {
        h[0] = (h[0] ^ bytes[i]) * 0x100000001b3ull;
        h[1] = (h[1] + bytes[i]) * 0xc2b2ae3d27d4eb4full; h[1] = h[1] << 31 | h[1] >> 33;
    }
    /* Four-way payload buckets retain guard collection identity per actual ticket and frozen span version. */
    unsigned at = (unsigned)(h[0] % (REC_DS_CACHE / 4)) * 4, replace = at;
    int cached = 0;
    for (unsigned k = at; k < at + 4; k++) {
        if (!ds_off && c->cache[k].set && c->cache[k].hash[0] == h[0] && c->cache[k].hash[1] == h[1] &&
            c->cache[k].layout == layout && c->cache[k].count == count) { at = k; cached = 1; break; }
        if (!c->cache[k].set) replace = k;
    }
    if (!cached) {
        if (c->cache[replace].set) replace += (unsigned)(h[1] & 3);
        at = replace;
    }
    if (ticket && (!cached || c->cache[at].guard_ticket != ticket || c->cache[at].guard_spans != c->spans)) {
        for (uint32_t k = 0; k < count; k++) if (info[k].lag_reason && info[k].lag_range.buffer)
            collect_guard(j, c->spans, info[k].lag_range.buffer, info[k].lag_range.offset,
                          info[k].lag_range.range, ticket, info[k].lag_reason);
    }
    if (cached) {
        c->cache[at].guard_ticket = ticket; c->cache[at].guard_spans = c->spans;
        return c->cache[at].set;
    }
    VkDescriptorSet set = alloc_set(c, layout);
    vkUpdateDescriptorSetWithTemplate(V.dev, set, update, info);
    c->cache[at].hash[0] = h[0]; c->cache[at].hash[1] = h[1]; c->cache[at].layout = layout;
    c->cache[at].count = count; c->cache[at].set = set;
    c->cache[at].guard_ticket = ticket; c->cache[at].guard_spans = c->spans;
    return set;
}
/* The constant worker specialization contains only raw Vulkan calls. The
 * shared walking entry resolves its token once, at the first emitted command;
 * an unchanged dynamic snapshot must not create an ordered section. */
static inline __attribute__((always_inline)) void record_dynamic_emit_inner(VkCommandBuffer cb,
        const vk_record_dynamic *state, vk_record_dynamic *previous, int all, int walking) {
    int routed = 0;
#define EMIT(command, ...) do { \
    if (walking && !routed) { cb = VK_SECTION_CB(cb); routed = 1; } \
    (command)(cb, __VA_ARGS__); \
} while (0)
#define D (*state)
#define CH(f) (all || memcmp(&D.f, &previous->f, sizeof D.f))
    if (CH(vp)) EMIT(vkCmdSetViewport, 0, 1, &D.vp);
    if (CH(sc)) EMIT(vkCmdSetScissor, 0, 1, &D.sc);
    if (all) EMIT(vkCmdSetLineWidth, 1.0f);
    if (CH(bias_en)) EMIT(vkCmdSetDepthBiasEnable, D.bias_en);
    if (CH(bias)) EMIT(vkCmdSetDepthBias, D.bias[0], D.bias[1], D.bias[2]);
    if (CH(blend)) EMIT(vkCmdSetBlendConstants, D.blend);
    if (CH(cmask)) { EMIT(vkCmdSetStencilCompareMask, VK_STENCIL_FACE_FRONT_BIT, D.cmask[0]); EMIT(vkCmdSetStencilCompareMask, VK_STENCIL_FACE_BACK_BIT, D.cmask[1]); }
    if (CH(wmask)) { EMIT(vkCmdSetStencilWriteMask, VK_STENCIL_FACE_FRONT_BIT, D.wmask[0]); EMIT(vkCmdSetStencilWriteMask, VK_STENCIL_FACE_BACK_BIT, D.wmask[1]); }
    if (CH(ref)) { EMIT(vkCmdSetStencilReference, VK_STENCIL_FACE_FRONT_BIT, D.ref[0]); EMIT(vkCmdSetStencilReference, VK_STENCIL_FACE_BACK_BIT, D.ref[1]); }
    if (CH(cull)) EMIT(vkCmdSetCullMode, D.cull);
    if (CH(front)) EMIT(vkCmdSetFrontFace, (VkFrontFace)D.front);
    if (CH(topo)) EMIT(vkCmdSetPrimitiveTopology, (VkPrimitiveTopology)D.topo);
    if (CH(dt)) EMIT(vkCmdSetDepthTestEnable, D.dt);
    if (CH(dw)) EMIT(vkCmdSetDepthWriteEnable, D.dw);
    if (CH(dc)) EMIT(vkCmdSetDepthCompareOp, (VkCompareOp)D.dc);
    if (CH(st)) EMIT(vkCmdSetStencilTestEnable, D.st);
    if (CH(sop)) {
        EMIT(vkCmdSetStencilOp, VK_STENCIL_FACE_FRONT_BIT, D.sop[0][0], D.sop[0][1], D.sop[0][2], D.sop[0][3]);
        EMIT(vkCmdSetStencilOp, VK_STENCIL_FACE_BACK_BIT, D.sop[1][0], D.sop[1][1], D.sop[1][2], D.sop[1][3]);
    }
    if (CH(restart)) EMIT(vkCmdSetPrimitiveRestartEnable, D.restart);
#undef CH
#undef EMIT
    *previous = D;
}
#undef D
void vk_record_dynamic_emit(VkCommandBuffer cb, const vk_record_dynamic *state, vk_record_dynamic *previous, int all) {
    record_dynamic_emit_inner(cb, state, previous, all, 1);
}
static void record_job(rec_job *j, unsigned worker) {
    if (j->registering) {
        j->reset_time = 0;
        vk_record_lag_batch_register(j->registration, j->guard_ticket, j->shard, j->nshards);
        return;
    }
    rec_context *c = contexts[worker][j->slot];
    if (!c) { c = VK_CALLOC(1, sizeof *c); c->stock_on = vk_desc_stock_want(); contexts[worker][j->slot] = c; }
    j->reset_time = 0;
    if (c->generation != j->generation) {
        uint64_t t0 = host_now_ns();
        (void)commands_recycle(worker);
        if (c->ncb) DIE("recorder slot reused before its secondary leases retired");
#ifdef TOTK_VK_TESTS
        vk_test_descriptor_pools_check(j->slot);
#endif
        descriptors_recycle(c);
        memset(c->cache, 0, sizeof c->cache); c->generation = j->generation; c->spans = NULL;
        #ifdef TOTK_VK_TESTS
        atomic_fetch_add(&test_resets, 1);
        #endif
        j->reset_time = host_now_ns() - t0;
    }
    j->cb = commands_acquire(worker, c);
    VkCommandBufferInheritanceRenderingInfo ri = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .colorAttachmentCount = j->ncolor, .pColorAttachmentFormats = j->color, .depthAttachmentFormat = j->depth,
        .stencilAttachmentFormat = j->stencil, .rasterizationSamples = j->samples};
    VkCommandBufferInheritanceInfo ii = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &ri};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, .pInheritanceInfo = &ii};
    VKCHECK(vkBeginCommandBuffer(j->cb, &bi));
    if (__builtin_expect(vk_gpuprof_on, 0)) {
        const rec_profile_job *profile = (const rec_profile_job *)j;
        if (profile->pool) (vkCmdBeginQuery)(j->cb, profile->pool, profile->query, 0);
    }
    VkPipeline pipeline = VK_NULL_HANDLE; vk_record_dynamic dynamic = {0};
    VkPipelineLayout bound_layout = VK_NULL_HANDLE; VkDescriptorSet bound_set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer vertices[17], index_buffer = VK_NULL_HANDLE;
    VkDeviceSize offsets[17], sizes[17], strides[17], index_offset = 0;
    uint64_t addresses[17], raw_sizes[17]; const vk_record_spans *vertex_spans[17];
    uint32_t vertex_valid = 0, raw_valid = 0; VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    uint32_t event = 0; uint64_t draws = 0;
    for (uint32_t i = 0; i <= j->count; i++) {
        while (event < j->nevent && j->event[event].before == i) {
            const rec_event *e = &j->event[event++];
            if (e->kind == REC_CLEAR) (vkCmdClearAttachments)(j->cb, 1, &e->clear.attachment, 1, &e->clear.rect);
            else (vkCmdWriteTimestamp2)(j->cb, e->timestamp.stage, e->timestamp.pool, e->timestamp.query);
        }
        if (i == j->count) break;
        const vk_record_draw *d = &j->draw[i];
        if (c->spans != d->spans) { c->spans = d->spans; memset(c->hints, 0, sizeof c->hints); }
        if (bound_layout != d->layout) { bound_layout = d->layout; bound_set[0] = bound_set[1] = VK_NULL_HANDLE; }
        if (pipeline != d->pipeline) { (vkCmdBindPipeline)(j->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipeline); pipeline = d->pipeline; }
        record_dynamic_emit_inner(j->cb, &d->dynamic, &dynamic, i == 0 || dyn_off, 0);
        /* Chunk-local binding memos are valid only while the exact pipeline layout stays unchanged. */
        for (uint32_t s = 0; s < 2; s++) if (d->sets[s].count) {
            VkDescriptorSet set = descriptor_set(c, j, d->sets[s].layout, d->sets[s].update, j->info + d->sets[s].first, d->sets[s].count, d->lag_ticket);
            if (ds_off || bound_set[s] != set) {
                (vkCmdBindDescriptorSets)(j->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->layout, s, 1, &set, 0, NULL); bound_set[s] = set;
            }
        }
        uint32_t changed = 0;
        for (uint32_t v = 0; v < 17; v++) if (d->vertex_mask >> v & 1) {
            uint32_t bit = 1u << v; VkBuffer buffer = d->vertex[v].buffer;
            uint64_t offset = d->vertex[v].offset, available; VkDeviceSize size = d->vertex[v].size;
            if (buffer) raw_valid &= ~bit; /* selected handles, especially private overrides, are not cached raw resolutions */
            else if ((raw_valid & bit) && addresses[v] == d->vertex[v].address &&
                     raw_sizes[v] == d->vertex[v].size && vertex_spans[v] == d->spans) {
                buffer = vertices[v]; offset = offsets[v]; size = sizes[v];
            } else {
                if (!d->vertex[v].address || !vk_record_resolve(d->spans, d->vertex[v].address, 1, &buffer, &offset, &available, c->hints)) {
                    VkDescriptorBufferInfo zero = zero_buffer(c); buffer = zero.buffer; offset = zero.offset; size = 64;
                } else if (size > available) size = available;
                addresses[v] = d->vertex[v].address; raw_sizes[v] = d->vertex[v].size; vertex_spans[v] = d->spans;
                raw_valid |= bit;
            }
            if (d->lag_ticket && d->vertex[v].lag_reason)
                collect_guard(j, d->spans, buffer, offset, size, d->lag_ticket, d->vertex[v].lag_reason);
#ifdef TOTK_VK_TESTS
            if (d->test_vertex.address && d->vertex[v].address == d->test_vertex.address) {
                atomic_fetch_add(&test_checks, 1);
                if (buffer != d->test_vertex.buffer || offset != d->test_vertex.offset) atomic_fetch_add(&test_failures, 1);
            }
#endif
            if (!(vertex_valid & bit) || vertices[v] != buffer || offsets[v] != offset || sizes[v] != size || strides[v] != d->vertex[v].stride) changed |= bit;
            vertex_valid |= bit; vertices[v] = buffer; offsets[v] = offset; sizes[v] = size; strides[v] = d->vertex[v].stride;
        }
        while (changed) {
            uint32_t first = (uint32_t)__builtin_ctz(changed), end = first + 1;
            while (end < 17 && (changed >> end & 1)) end++;
            (vkCmdBindVertexBuffers2)(j->cb, first, end - first, vertices + first, offsets + first, sizes + first, strides + first);
            changed &= ~(((1u << (end - first)) - 1) << first);
        }
        if (d->index || d->index_address) {
            VkBuffer buffer = d->index; uint64_t offset = d->index_offset;
            if (d->index_address) {
                uint32_t stride = d->index_type == VK_INDEX_TYPE_UINT8_EXT ? 1 : d->index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
                if (!vk_record_resolve(d->spans, d->index_address, (uint64_t)d->count * stride, &buffer, &offset, NULL, c->hints)) DIE("validated recorder index range disappeared from immutable spans");
            }
            if (d->lag_ticket && d->index_lag_reason)
                collect_guard(j, d->spans, buffer, offset, d->index_bytes, d->lag_ticket, d->index_lag_reason);
            if (index_buffer != buffer || index_offset != offset || index_type != d->index_type) {
                (vkCmdBindIndexBuffer)(j->cb, buffer, offset, d->index_type); index_buffer = buffer; index_offset = offset; index_type = d->index_type;
            }
            (vkCmdDrawIndexed)(j->cb, d->count, d->instances, d->first, d->vertex_offset, d->base_instance);
            draws++;
        } else if (d->count) { (vkCmdDraw)(j->cb, d->count, d->instances, d->first, d->base_instance); draws++; }
    }
    if (__builtin_expect(vk_gpuprof_on, 0)) {
        const rec_profile_job *profile = (const rec_profile_job *)j;
        if (profile->pool) (vkCmdEndQuery)(j->cb, profile->pool, profile->query);
    }
    VKCHECK(vkEndCommandBuffer(j->cb));
    project_guards(j);   /* freeze every owner/view/alias VA while this recording job still owns its inputs */
    /* Publish once per completed chunk, never a ring lock for each draw or descriptor update. */
    uint64_t desc_creates = c->descriptor_sets; /* this job's driver allocations: published and cleared below */
    if (j->frame) {
        if (j->frame == totk_ft_work_frame()) {
            totk_ft_count(FT_COUNT_DRAWS, draws);
            totk_ft_count(FT_COUNT_DESC_CREATES, desc_creates);
        } else {
            if (draws) totk_ft_count_frame(j->frame, FT_COUNT_DRAWS, draws);
            if (desc_creates) totk_ft_count_frame(j->frame, FT_COUNT_DESC_CREATES, desc_creates);
        }
    }
    if (!worker) commands_publish(0);
    atomic_fetch_add_explicit(&descriptor_stats[worker].sets, c->descriptor_sets, memory_order_relaxed);
    atomic_fetch_add_explicit(&descriptor_stats[worker].calls, c->descriptor_calls, memory_order_relaxed);
    atomic_fetch_add_explicit(&descriptor_stats[worker].resets, c->descriptor_resets, memory_order_relaxed);
    c->descriptor_sets = c->descriptor_calls = c->descriptor_resets = 0;
    for (unsigned k = 0; k <= VK_DESC_STOCK_REASONS; k++) if (c->stock_events[k]) {
        atomic_fetch_add_explicit(&descriptor_stats[worker].stock[k], c->stock_events[k], memory_order_relaxed);
        c->stock_events[k] = 0;
    }
}
static inline void rec_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
/* TOTK_VK_REC_WAKE_BATCH >= 2: an idle worker watches the queue for up to `us` µs before it parks (rec_mu held on entry
   and on return; released while polling). The walker sees it in rec_spinning and does not signal. */
static void rec_poll_queue(unsigned us) {
    uint32_t seen = atomic_load_explicit(&rec_queue_seq, memory_order_relaxed);
    rec_spinning++;
    pthread_mutex_unlock(&rec_mu);
    uint64_t until = host_now_ns() + (uint64_t)us * 1000;
    int published = 0;
    for (;;) {
        for (int i = 0; i < 32 && !published; i++) {
            rec_relax(); published = atomic_load_explicit(&rec_queue_seq, memory_order_relaxed) != seen;
        }
        if (published || host_now_ns() >= until) break;
    }
    pthread_mutex_lock(&rec_mu);
    rec_spinning--;
    if (published && queue_head) rec_spin_hits++;
}
static void *record_worker(void *arg) {
    unsigned worker = (unsigned)(uintptr_t)arg;
    char name[16]; snprintf(name, sizeof name, "totk-recw%u", worker - 1);
    totk_thread_start(name, "TOTK_AFFINITY_RECW");
    nvn_dirty_lag_thread_role("recorder");
    for (;;) {
        pthread_mutex_lock(&rec_mu);
        int wake_batch = queue_head ? 0 : rec_wake_batch();
        if (wake_batch >= 2) rec_poll_queue((unsigned)wake_batch);
        while (!queue_head) {
            rec_work_waiters++; rec_parks++;
            pthread_cond_wait(&rec_work, &rec_mu);
            rec_work_waiters--; if (rec_wake_pending) rec_wake_pending--;
        }
        rec_job *batch = queue_head;
        if (batch) { queue_head = batch->queue_next; if (!queue_head) queue_tail = NULL; }
        /* More work left behind and a parked worker nobody has signalled: wake it from here, not from the walker. */
        if (queue_head && rec_work_waiters > rec_wake_pending && !rec_spinning && rec_wake_batch()) {
            rec_wake_pending++; rec_wakes_chain++; pthread_cond_signal(&rec_work);
        }
        pthread_mutex_unlock(&rec_mu);
        totk_live_slack_tick();   /* a live TOTK_TIMER_SLACK_NS: this thread applies it itself */
        uint64_t recycled = commands_recycle(worker);
#ifdef TOTK_VK_TESTS
        test_worker_gate(batch);
#endif
        uint64_t t0 = host_now_ns(), resets = recycled, ready = 0, service = 0, max_ready = 0, max_service = 0, jobs = 0;
        uint64_t registered_jobs = 0, registered_ns = 0;
        for (rec_job *j = batch; j; j = j->batch_next) {
            nvn_dirty_lag_thread_role(j->registering ? "registrar" : "recorder");
            uint64_t start = host_now_ns(), delay = start - j->ready_ns;
            record_job(j, worker); uint64_t elapsed = host_now_ns() - start;
            if (j->registering) { registered_jobs++; registered_ns += elapsed; }
            else {
                ready += delay; service += elapsed; resets += j->reset_time; jobs++;
                if (delay > max_ready) max_ready = delay; if (elapsed > max_service) max_service = elapsed;
            }
        }
        uint64_t ns = host_now_ns() - t0 + recycled;
        pthread_mutex_lock(&rec_mu);
        busy_ns[worker - 1] += ns; worker_reset_ns += resets;
        ready_delay_ns += ready; service_ns += service; jobs_serviced += jobs;
        guard_jobs += registered_jobs; guard_service_ns += registered_ns;
        if (max_ready > max_ready_delay_ns) max_ready_delay_ns = max_ready;
        if (max_service > max_service_ns) max_service_ns = max_service;
        commands_publish(worker);
        for (rec_job *j = batch; j; ) {
            /* Publication permits fence-fast slot reuse: do not read this job again after its release store. */
            rec_job *next = j->batch_next;
            atomic_store_explicit(&j->done, 1, memory_order_release); j = next;
        }
        if (rec_done_waiters) pthread_cond_broadcast(&rec_done);
        pthread_mutex_unlock(&rec_mu);
    }
    return NULL;
}
int vk_record_enabled(void) {
    if (threads >= 0) return threads != 0;
#if defined(__ANDROID__) || (defined(TOTK_VK_TESTS) && defined(TOTK_TEST_ANDROID_FRAME_POLICY))
    const int default_threads = 2;
#else
    const int default_threads = 0;
#endif
    const char *e = getenv("TOTK_VK_DESC_STOCK"); descriptor_stock = e && atoi(e) != 0;
    LOGV("descriptor stock: %s (TOTK_VK_DESC_STOCK=1 keeps per-layout sets per recorder/primary slot; 0/unset resets every pool per slot generation)",
        descriptor_stock ? "on" : "off");
    e = getenv("TOTK_VK_DESC_STOCK_MAX_SETS");
    if (e && *e) { long n = atol(e); descriptor_stock_max = n < 0 ? 0 : n > 1048576 ? 1048576 : (uint32_t)n; }
    if (descriptor_stock_max)
        LOGV("descriptor stock ceiling: %u sets per owner (TOTK_VK_DESC_STOCK_MAX_SETS; with the stock on, an owner past it resets its pools and keeps at most %u recorder / %u primary pools)",
            descriptor_stock_max, vk_desc_stock_pool_limit(descriptor_stock_max, REC_POOL_SETS),
            vk_desc_stock_pool_limit(descriptor_stock_max, VK_DESC_PRIMARY_POOL_SETS));
    else LOGV("descriptor stock ceiling: none (TOTK_VK_DESC_STOCK_MAX_SETS unset or 0)");
    e = getenv("TOTK_VK_REC_THREADS"); threads = e ? atoi(e) : default_threads;
    if (threads < 0) threads = 0; if (threads > REC_WORKERS) threads = REC_WORKERS;
    const char *diagnostics[] = {"TOTK_VK_OCCLUSION", "TOTK_VK_PASS_DUMP", "TOTK_VK_PASS_PROFILE", "TOTK_VK_PASSPROBE", "TOTK_VK_PASS_MERGE"};
    for (size_t i = 0; threads && i < sizeof diagnostics / sizeof *diagnostics; i++) {
        e = getenv(diagnostics[i]); if (e && *e && strcmp(e, "0")) { LOGV("recorder: threads disabled by %s", diagnostics[i]); threads = 0; }
    }
    if (!threads) return 0;
    e = getenv("TOTK_VK_REC_WAKE_BATCH");
    if (e && *e) { long n = atol(e); rec_wake_launch = n < 0 ? 0 : n > 2000 ? 2000 : (int)n; }
    LOGV("TOTK_VK_REC_WAKE_BATCH=%d: %s", rec_wake_launch, !rec_wake_launch ? "every queued chunk batch wakes a parked recorder worker (default)" :
         rec_wake_launch == 1 ? "one walker wake per burst; a worker leaving work queued wakes the next" :
         "one walker wake per burst; idle workers poll the queue for that many µs before parking");
    e = getenv("TOTK_VK_REC_VMA_NAMES"); vma_names = e && atoi(e) != 0; /* opt-in memory-map diagnostics only */
    e = getenv("TOTK_VK_REC_CHUNK_DRAWS"); if (e && *e) { long n = atol(e); chunk_draws = n < 1 ? 1 : n > 512 ? 512 : (uint32_t)n; }
    e = getenv("TOTK_VK_REC_LEAD_DRAWS"); if (e && *e) { long n = atol(e); lead_draws = n < 0 ? 0 : n > chunk_draws ? chunk_draws : (uint32_t)n; }
    if (lead_draws > chunk_draws) lead_draws = chunk_draws;
    e = getenv("TOTK_VK_REC_INLINE_DRAWS"); if (e && *e) { long n = atol(e); inline_draws = n < 0 ? 0 : n > 512 ? 512 : (uint32_t)n; }
    e = getenv("TOTK_VK_REC_MEMORY_MB");
    if (e && *e) { long n = atol(e); storage_limit = (uint64_t)(n < 16 ? 16 : n > 512 ? 512 : n) << 20; }
    ds_off = vk_off("ds"); dyn_off = vk_off("dyn");
    int requested = threads;
    for (int i = 0; i < requested; i++) {
        pthread_t t; if (pthread_create(&t, NULL, record_worker, (void *)(uintptr_t)(i + 1))) { threads = i; break; } pthread_detach(t);
    }
    LOGV("recorder: %d workers, %u draws/chunk, first chunks %u draws, chunks below %u inline (TOTK_VK_REC_THREADS/REC_CHUNK_DRAWS/REC_LEAD_DRAWS/REC_INLINE_DRAWS)",
        threads, chunk_draws, lead_draws, inline_draws);
    LOGV("recorder storage: %llu MiB global retained packet/descriptor slab budget; cross-slot secondary leases, owner-only maintenance after4096 idle slot epochs, warm floor512/worker; active slots may exceed retention",
        (unsigned long long)(storage_limit >> 20));
    return threads != 0;
}
void vk_record_slot_reset(unsigned slot) {
    if (vk_one_primary_on) current_slot = slot; /* test early assembly also serves walker-only slots */
    if (!vk_record_enabled()) return;
    uint64_t t0 = host_now_ns();
    /* busy/submit_busy retirement joins all jobs, submitted GPU reads and CPU posts before this handoff.
       No worker can touch a context of this slot until the new generation is dispatched below. */
    uint64_t retained = atomic_load(&memory_draws) + atomic_load(&memory_info);
    if (retained > storage_limit && ++slots[slot].prune_uses == 8) {
        slots[slot].prune_uses = 0;
        uint64_t trim_start = host_now_ns();
        for (rec_job *j = slots[slot].jobs; j; j = j->all_next) {
            int idle = slots[slot].generation - j->generation >= 32;
            if (j->draw && (idle || j->small_draw_uses == 32)) {
                storage_put(&slots[slot].draw_storage, j->draw); j->draw = NULL; j->capdraw = j->small_draw_uses = 0;
            }
            if (j->info && (idle || j->small_info_uses == 32)) {
                storage_put(&slots[slot].info_storage, j->info); j->info = NULL; j->capinfo = j->ninfo = j->small_info_uses = 0;
            }
        }
        uint64_t freed = storage_trim(&slots[slot].draw_storage, 0) + storage_trim(&slots[slot].info_storage, 1);
        if (freed) atomic_fetch_add_explicit(&storage_reclaims, 1, memory_order_relaxed);
        storage_trim_ns += host_now_ns() - trim_start;
    }
    (void)commands_recycle(0); /* only the walking recording owner can touch its pool */
    current_slot = slot; slots[slot].cursor = slots[slot].jobs; slots[slot].generation++;
    slots[slot].pass_cursor = slots[slot].passes;
    vk_record_spans_reset(slot);
    reset_ns += host_now_ns() - t0;
}
void vk_record_pass_begin(uint32_t ncolor, const VkFormat *color, VkFormat depth, VkFormat stencil, VkSampleCountFlagBits samples) {
    pass.ncolor = ncolor; memcpy(pass.color, color, sizeof pass.color); pass.depth = depth; pass.stencil = stencil; pass.samples = samples;
    pass_head = pass_tail = building = NULL; pass_parallel = 0; pass_draws = 0;
}
static rec_job *job_allocate(void) {
    rec_job *j = slots[current_slot].cursor;
    if (j) slots[current_slot].cursor = j->all_next;
    else {
        j = VK_CALLOC(1, vk_gpuprof_on ? sizeof(rec_profile_job) : sizeof *j);
        j->all_next = slots[current_slot].jobs; slots[current_slot].jobs = j;
    }
    j->generation = slots[current_slot].generation; j->slot = current_slot;
    j->frame = totk_ft_work_frame();
    j->done = j->parallel = j->count = 0; j->pass_next = j->queue_next = j->batch_next = NULL;
    j->nguards = j->nprojected = 0; j->projected_ranges = j->prep_ns = 0;
    j->guard_ticket = vk_record_lag_ticket(); j->registering = 0;
    if (j->guard_ticket) nvn_dirty_lag_batch_reset(&j->units);
    if (j->guard_cache) memset(j->guard_cache, 0, REC_GUARD_CACHE * sizeof *j->guard_cache);
    return j;
}
static rec_job *job_begin(void) {
    uint64_t t0 = host_now_ns();
    rec_job *j = job_allocate();
    j->slot = current_slot; j->ncolor = pass.ncolor; memcpy(j->color, pass.color, sizeof j->color);
    j->depth = pass.depth; j->stencil = pass.stencil; j->samples = pass.samples;
    j->id = ++next_chunk_id;
    j->limit = lead_draws && pass_draws < lead_draws * (uint32_t)threads ? lead_draws : chunk_draws;
    j->count = j->ninfo = j->nevent = 0; j->done = j->parallel = 0; j->pass_next = j->queue_next = NULL;
    j->generation = slots[current_slot].generation;
    if (pass_tail) pass_tail->pass_next = j; else pass_head = j; pass_tail = building = j;
    job_ns += host_now_ns() - t0;
    return j;
}
vk_record_draw *vk_record_draw_begin(VkPipeline pipeline, VkPipelineLayout layout) {
    if (!building) job_begin();
    if (building->count == building->capdraw) {
        uint32_t capacity = building->capdraw ? building->capdraw * 2 : 4;
        if (capacity > building->limit) capacity = building->limit;
        size_t bytes = (size_t)capacity * sizeof *building->draw;
        rec_storage *storage = &slots[current_slot].draw_storage;
        vk_record_draw *draw = storage_get(storage, bytes, 0, __func__, "calloc");
        if (building->count) memcpy(draw, building->draw, (size_t)building->count * sizeof *draw);
        memset(draw + building->count, 0, (capacity - building->count) * sizeof *draw);
        storage_put(storage, building->draw); building->draw = draw; building->capdraw = capacity;
    }
    vk_record_draw *d = &building->draw[building->count];
    d->pipeline = pipeline; d->layout = layout; memset(d->sets, 0, sizeof d->sets);
    d->vertex_mask = 0; d->index = VK_NULL_HANDLE; d->index_address = d->index_offset = 0;
    d->index_type = VK_INDEX_TYPE_UINT32; d->first = 0; d->vertex_offset = 0;
    d->index_lag_reason = 0; d->index_bytes = 0;
    d->lag_ticket = vk_record_lag_enabled() ? vk_record_lag_ticket() : 0;
    /* Dynamic state and used raw bindings are overwritten by the walk; do not clear their entire packet twice. */
    d->spans = vk_record_spans_capture(current_slot);
#ifdef TOTK_VK_TESTS
    d->test_vertex = test_vertex;
#endif
    building->info_mark = building->ninfo;
    return d;
}
uint64_t vk_record_chunk_id(void) { return building ? building->id : 0; }
vk_record_descriptor *vk_record_descriptors(vk_record_draw *draw, unsigned set, VkDescriptorSetLayout layout, uint32_t count) {
    uint64_t start = __builtin_expect(vk_rec_timing_on, 0) ? host_now_ns() : 0; descriptor_lists++;
    rec_job *j = building; uint32_t first = j->ninfo;
    if (count > UINT32_MAX - first) DIE("recorder descriptor count overflow");
    j->ninfo += count;
    if (j->ninfo > j->capinfo) {
        rec_storage *storage = &slots[current_slot].info_storage;
        vk_record_descriptor *info = storage_get(storage, (size_t)j->ninfo * sizeof *info, 1, __func__, "j->info");
        if (first) memcpy(info, j->info, (size_t)first * sizeof *info);
        storage_put(storage, j->info); j->info = info;
        j->capinfo = storage_capacity(info, sizeof *info);
    }
    draw->sets[set].layout = layout; draw->sets[set].first = first; draw->sets[set].count = count;
    memset(j->info + first, 0, count * sizeof *j->info);
    if (start) descriptor_list_ns += host_now_ns() - start;
    return j->info + first;
}
static void enqueue_batch(rec_job *first) {
    first->queue_next = NULL;
    int batched = rec_wake_batch() != 0;
    pthread_mutex_lock(&rec_mu);
    if (queue_tail) queue_tail->queue_next = first; else queue_head = first;
    queue_tail = first;
    atomic_store_explicit(&rec_queue_seq, atomic_load_explicit(&rec_queue_seq, memory_order_relaxed) + 1, memory_order_relaxed);
    /* Batched: a signalled or polling worker takes this batch, and wakes the next parked worker if more are queued. */
    if (rec_work_waiters && (!batched || (!rec_wake_pending && !rec_spinning))) {
        if (rec_wake_pending < rec_work_waiters) rec_wake_pending++;
        rec_wakes_walker++; pthread_cond_signal(&rec_work);
    }
    pthread_mutex_unlock(&rec_mu);
}
static void flush_small(void) {
    if (!small_head) return;
    enqueue_batch(small_head); small_head = small_tail = NULL; small_jobs = small_draws = 0;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_flush_jobs(void) { flush_small(); }
#endif
void vk_record_lag_guard(VkBuffer buffer, uint64_t offset, uint64_t size, uint32_t reason) {
    if (!vk_record_enabled() || !vk_record_lag_enabled() || !size) return;
    (void)vk_cb();   /* initialize the actual submission slot before taking its frozen inventory */
    if (!guard_building) {
        guard_building = job_allocate();
    }
    rec_job *j = guard_building;
    collect_guard(j, vk_record_spans_capture(current_slot), buffer, offset, size, j->guard_ticket, reason);
    if (j->nguards == 32) project_guards(j); /* one reusable primary batch, never an early registrar */
}
static void dispatch_building(void) {
    rec_job *j = building; if (!j) return; building = NULL;
    if (j->capdraw > (uint64_t)j->count * 4) { if (j->small_draw_uses < 32) j->small_draw_uses++; }
    else j->small_draw_uses = 0;
    if (j->capinfo > (uint64_t)j->ninfo * 4) { if (j->small_info_uses < 32) j->small_info_uses++; }
    else j->small_info_uses = 0;
    uint32_t operations = j->count + j->nevent;
    if (!operations) { j->done = 1; return; }
    if (__builtin_expect(vk_gpuprof_on, 0)) {
        rec_profile_job *profile = (rec_profile_job *)j;
        profile->pool = VK_NULL_HANDLE;
        (void)vk_gpuprof_chunk_query(&profile->pool, &profile->query);
    }
    if (inline_draws && operations < inline_draws) { uint64_t t0 = host_now_ns(); record_job(j, 0); inline_ns += host_now_ns() - t0; reset_ns += j->reset_time; j->done = 1; chunks_inline++; return; }
    j->parallel = pass_parallel = 1; j->batch_next = NULL; chunks_parallel++;
    j->ready_ns = host_now_ns();
    if (operations < 16) {
        if (small_tail) small_tail->batch_next = j; else small_head = j;
        small_tail = j; small_jobs++; small_draws += operations;
        if (small_jobs == 4 || small_draws >= 32) flush_small();
    } else { flush_small(); enqueue_batch(j); }
}
void vk_record_pass_flush(void) { dispatch_building(); }
void vk_record_draw_commit(void) { pass_draws++; if (++building->count == building->limit) dispatch_building(); }
void vk_record_draw_abort(void) { if (building) { building->ninfo = building->info_mark; building->id = ++next_chunk_id; } }
static rec_event *event_begin(void) {
    rec_job *j = building ? building : job_begin();
    if (j->nevent == j->capevent) { j->capevent = j->capevent ? j->capevent * 2 : 4; j->event = VK_REALLOC(j->event, j->capevent * sizeof *j->event); }
    rec_event *e = &j->event[j->nevent++]; e->before = j->count; return e;
}
void vk_record_clear(const VkClearAttachment *attachment, const VkClearRect *rect) {
    rec_event *e = event_begin(); e->kind = REC_CLEAR; e->clear.attachment = *attachment; e->clear.rect = *rect;
}
void vk_record_timestamp(VkQueryPool pool, uint32_t query, VkPipelineStageFlags2 stage) {
    rec_event *e = event_begin(); e->kind = REC_TIMESTAMP; e->timestamp.pool = pool; e->timestamp.query = query; e->timestamp.stage = stage;
}
/* Constant specialization keeps token lifetimes, copy temporaries and variable
   allocation sizes out of the disabled entry point, not just their stores. */
static inline __attribute__((always_inline)) int record_pass_end_impl(VkCommandBuffer primary,
        const vk_gpuprof_end_token *profile_end, int profiling) {
    dispatch_building();
    if (!pass_head) {
        vkCmdEndRendering(primary);
        if (profiling && profile_end) vk_gpuprof_end_emit(primary, profile_end);
        passes_inline++; return 0;
    }
    rec_pass *p = slots[current_slot].pass_cursor;
    if (p) slots[current_slot].pass_cursor = p->all_next;
    else {
        p = VK_CALLOC(1, profiling ? sizeof(rec_profile_pass) : sizeof *p);
        p->all_next = slots[current_slot].passes; slots[current_slot].passes = p;
    }
    p->primary = primary; p->jobs = pass_head; p->next = NULL; p->draws = pass_draws;
    if (profiling) {
        rec_profile_pass *profile = (rec_profile_pass *)p;
        profile->end = profile_end ? *profile_end : (vk_gpuprof_end_token){0};
    }
    if (pending_tail) pending_tail->next = p; else pending_head = p; pending_tail = p;
    if (vk_one_primary_on) vk_sections_park(p);
    if (pass_parallel) passes_parallel++; else passes_inline++;
    pass_head = pass_tail = building = NULL;
    return 1;
}
/* The draw-side cached branch chooses distinct entries so the disabled core
   carries neither a token argument nor a switch value across dispatch. */
int vk_record_pass_end(VkCommandBuffer primary) {
    return record_pass_end_impl(primary, NULL, 0);
}
int vk_record_pass_end_profiled(VkCommandBuffer primary, const vk_gpuprof_end_token *profile_end) {
    return record_pass_end_impl(primary, profile_end, 1);
}
void vk_record_slot_seal(unsigned slot) {
    flush_small();
    if (guard_building) { project_guards(guard_building); guard_building->done = 1; guard_building = NULL; }
    slots[slot].sealed_generation = slots[slot].generation; slots[slot].sealed_ticket = vk_record_lag_ticket();
    slots[slot].frame = totk_ft_work_frame();
    slots[slot].pending = pending_head; pending_head = pending_tail = NULL;
}
static void record_assemble_inner(unsigned slot, int background) {
    if (!slots[slot].pending && !background && !vk_one_primary_on) return;
    uint64_t waited = 0, execute = 0, jobs = 0, draws = 0, resets = 0, largest_job = 0;
    uint64_t passes = 0, pass_draws = 0, largest_pass = 0;
    for (rec_pass *p = slots[slot].pending; p; p = p->next) {
        int waited_pass = 0;
        for (rec_job *j = p->jobs; j; j = j->pass_next) if (!atomic_load_explicit(&j->done, memory_order_acquire)) {
            if (!waited_pass) {
                waited_pass = 1; passes++; pass_draws += p->draws;
                if (p->draws > largest_pass) largest_pass = p->draws;
            }
            jobs++; draws += j->count;
            if (j->count > largest_job) largest_job = j->count;
            uint64_t t0 = host_now_ns();
            VK_REC_DO(VK_REC_O_WORKER_WAIT,
                pthread_mutex_lock(&rec_mu);
                while (!atomic_load_explicit(&j->done, memory_order_acquire)) { rec_done_waiters++; pthread_cond_wait(&rec_done, &rec_mu); rec_done_waiters--; }
                pthread_mutex_unlock(&rec_mu));
            waited += host_now_ns() - t0;
            resets += j->reset_time; /* waited-job reset total, not additive wait duration */
        }
        VkCommandBuffer commands[64]; uint32_t count = 0; uint64_t t0 = host_now_ns();
        if (vk_one_primary_on) vk_sections_until(slot, p);
        VK_REC_DO(VK_REC_O_EXECUTE,
            for (rec_job *j = p->jobs; j; j = j->pass_next) if (j->count || j->nevent) {
                commands[count++] = j->cb;
                if (count == 64) { (vkCmdExecuteCommands)(p->primary, count, commands); count = 0; }
            }
            if (count) (vkCmdExecuteCommands)(p->primary, count, commands);
            (vkCmdEndRendering)(p->primary);
            if (__builtin_expect(vk_gpuprof_on, 0)) vk_gpuprof_end_emit(p->primary, &((const rec_profile_pass *)p)->end);
            if (!vk_one_primary_on) VKCHECK(vkEndCommandBuffer(p->primary)));
        execute += host_now_ns() - t0;
    }
    if (vk_one_primary_on) vk_sections_until(slot, NULL);
    slots[slot].pending = NULL;
    pthread_mutex_lock(&rec_mu);
    if (background) { background_wait_ns += waited; background_slots++; }
    else wait_ns += waited;
    execute_ns += execute; join_jobs += jobs; join_draws += draws; join_reset_ns += resets;
    join_passes += passes; join_pass_draws += pass_draws;
    if (largest_job > max_join_draws) max_join_draws = largest_job;
    if (largest_pass > max_join_pass_draws) max_join_pass_draws = largest_pass;
    pthread_mutex_unlock(&rec_mu);
}
void vk_record_assemble(unsigned slot, int background) { VK_REC_DO(VK_REC_O_ASSEMBLE, record_assemble_inner(slot, background)); }
void vk_record_walk_wait(uint64_t ns) { wait_ns += ns; }
#ifdef TOTK_VK_TESTS
void vk_record_test_assemble(void) {
    if (vk_one_primary_on) vk_sections_seal();
    vk_record_slot_seal(current_slot); vk_record_assemble(current_slot, 0);
    if (vk_one_primary_on) vk_sections_resume(current_slot);
}
uint64_t vk_record_test_walk_wait(void) { return wait_ns; }
void vk_record_test_timing(uint64_t *busy, uint64_t *background_wait) {
    pthread_mutex_lock(&rec_mu); *busy = 0;
    for (unsigned i = 0; i < REC_WORKERS; i++) *busy += busy_ns[i];
    *background_wait = background_wait_ns; pthread_mutex_unlock(&rec_mu);
}
#endif
/* This slot's source CPU work and command recording ended. Merge its immutable identities once and join
   its disjoint arm shards before THIS ticket queues; later slots may still be walking/recording. */
void vk_record_lag_submit(unsigned slot, uint64_t ticket, const nvn_dirty_lag_batch *walking, int background) {
    if (!vk_record_enabled() || !vk_record_lag_enabled()) return;
    if (nvn_dirty_lag_vma_pressure()) { vk_record_lag_fail(ticket); return; }
    uint64_t critical = host_now_ns(), prep = 0, projected = 0, waited = 0;
    uint64_t generation = slots[slot].sealed_generation;
    if (ticket != slots[slot].sealed_ticket || generation != slots[slot].generation) { vk_record_lag_fail(ticket); return; }
    int native = walking != NULL; /* the sealed caller, not mutable walking policy, selects the representation */
    nvn_dirty_lag_batch *units = &slots[slot].units;
    uint64_t merge_start = host_now_ns();
    if (native) {
        nvn_dirty_lag_batch_reset(units);
        if (walking && !nvn_dirty_lag_batch_merge(units, walking)) vk_record_lag_fail(ticket);
    }
    for (rec_job *j = slots[slot].jobs; j; j = j->all_next) {
        if (j->generation != generation) continue;
        prep += j->prep_ns; projected += j->projected_ranges;
        if (j->guard_ticket != ticket) { vk_record_lag_fail(ticket); continue; }
        if (native) {
            if (!nvn_dirty_lag_batch_merge(units, &j->units)) vk_record_lag_fail(ticket);
        } else if (j->nprojected) vk_record_lag_register(j->projected, j->nprojected, ticket);
    }
    if (native && !nvn_dirty_lag_batch_seal(units)) vk_record_lag_fail(ticket);
    uint64_t merge_time = host_now_ns() - merge_start;
    size_t count = native ? nvn_dirty_lag_batch_count(units) : 0;
    /* Failed collection/merge latches fallback even with zero entries; valid empty batches need no workers. */
    unsigned nshards = count < (size_t)threads ? (unsigned)count : (unsigned)threads;
    for (unsigned i = 0; i < nshards; i++) {
        rec_job *j = &slots[slot].registrars[i];
        j->registering = 1; j->registration = units; j->guard_ticket = ticket;
        j->shard = i; j->nshards = nshards; j->batch_next = NULL; j->ready_ns = host_now_ns();
        atomic_store_explicit(&j->done, 0, memory_order_relaxed); enqueue_batch(j);
    }
    for (unsigned i = 0; i < nshards; i++) {
        rec_job *j = &slots[slot].registrars[i];
        if (atomic_load_explicit(&j->done, memory_order_acquire)) continue;
        uint64_t t0 = host_now_ns();
        VK_REC_DO(VK_REC_O_WORKER_WAIT,
            pthread_mutex_lock(&rec_mu);
            while (!atomic_load_explicit(&j->done, memory_order_acquire)) { rec_done_waiters++; pthread_cond_wait(&rec_done, &rec_mu); rec_done_waiters--; }
            pthread_mutex_unlock(&rec_mu));
        waited += host_now_ns() - t0;
    }
    /* GUARD_REGS counts submitted registration units, after global deduplication and actual shard execution.
       It does not count projected ranges or imply that a unit was newly armed rather than already guarded. */
    if (native && count && slots[slot].frame) {
        if (slots[slot].frame == totk_ft_work_frame()) totk_ft_count(FT_COUNT_GUARD_REGS, count);
        else totk_ft_count_frame(slots[slot].frame, FT_COUNT_GUARD_REGS, count);
    }
    pthread_mutex_lock(&rec_mu);
    if (!background) wait_ns += waited;
    guard_wait_ns += waited; guard_prep_ns += prep; guard_intents += projected; guard_merge_ns += merge_time; guard_units += count;
    guard_critical_ns += host_now_ns() - critical;
    pthread_mutex_unlock(&rec_mu);
}
void vk_record_walk_time(uint64_t ns) {
    if (threads <= 0) return;
    uint64_t waited = wait_ns - walk_wait_mark; walk_wait_mark = wait_ns;
    walk_ns += ns > waited ? ns - waited : 0;
}
void vk_record_stats(uint64_t frame) {
    vk_slot_stats(frame);
    if (!(frame % 300)) {
        static uint64_t previous[8], stock_previous[VK_DESC_STOCK_REASONS + 1];
        uint64_t now[8], stock_now[VK_DESC_STOCK_REASONS + 1];
        descriptor_totals(now); stock_totals(stock_now);
        uint64_t recorder_pools = atomic_load_explicit(&memory_dpools, memory_order_relaxed);
        uint64_t primary_pools = atomic_load_explicit(&memory_primary_dpools, memory_order_relaxed);
        LOGV("descriptors frame %llu, per-frame: sets %.2f allocate-calls %.2f pool-resets %.2f walker-ns %.0f list-ns %.0f lists %.2f template-ns %.0f templates %.2f; "
             "retained: pools %llu (recorder %llu, primary %llu) ~%.1f MiB, stock sets %llu (ceiling %u/owner); "
             "stock resets off/pressure/set-limit/pool-limit %.2f/%.2f/%.2f/%.2f pools-destroyed %.2f",
            (unsigned long long)frame, (now[0] - previous[0]) / 300.0, (now[1] - previous[1]) / 300.0,
            (now[2] - previous[2]) / 300.0, (now[3] - previous[3]) / 300.0, (now[4] - previous[4]) / 300.0,
            (now[5] - previous[5]) / 300.0, (now[6] - previous[6]) / 300.0, (now[7] - previous[7]) / 300.0,
            (unsigned long long)(recorder_pools + primary_pools), (unsigned long long)recorder_pools,
            (unsigned long long)primary_pools, (recorder_pools + primary_pools) * desc_pool_bytes() / 1048576.0,
            (unsigned long long)atomic_load_explicit(&stock_sets_retained, memory_order_relaxed), vk_desc_stock_ceiling(),
            (stock_now[VK_DESC_STOCK_OFF] - stock_previous[VK_DESC_STOCK_OFF]) / 300.0,
            (stock_now[VK_DESC_STOCK_PRESSURE] - stock_previous[VK_DESC_STOCK_PRESSURE]) / 300.0,
            (stock_now[VK_DESC_STOCK_SET_LIMIT] - stock_previous[VK_DESC_STOCK_SET_LIMIT]) / 300.0,
            (stock_now[VK_DESC_STOCK_POOL_LIMIT] - stock_previous[VK_DESC_STOCK_POOL_LIMIT]) / 300.0,
            (stock_now[VK_DESC_STOCK_REASONS] - stock_previous[VK_DESC_STOCK_REASONS]) / 300.0);
        memcpy(stock_previous, stock_now, sizeof stock_previous);
        memcpy(previous, now, sizeof previous);
    }
    if (threads <= 0 || frame % 300) return;
    pthread_mutex_lock(&rec_mu);
    static uint64_t section_previous[6], auxiliary_previous;
    uint64_t section_counts[6]; vk_sections_counts(section_counts);
    uint64_t auxiliary_count = vk_sections_auxiliary_count();
    LOGV("recorder frame %llu, ms/frame: walking %.3f workers %.3f %.3f %.3f %.3f wait %.3f; chunks parallel/inline %.2f/%.2f passes parallel/inline %.2f/%.2f; main/aux primaries %.2f/%.2f sections pass/immediate/rendering %.2f/%.2f/%.2f walker secondaries %.2f fallbacks %.2f",
        (unsigned long long)frame, walk_ns / 3e8, busy_ns[0] / 3e8, busy_ns[1] / 3e8, busy_ns[2] / 3e8, busy_ns[3] / 3e8,
        wait_ns / 3e8, chunks_parallel / 300.0, chunks_inline / 300.0, passes_parallel / 300.0, passes_inline / 300.0,
        (section_counts[0] - section_previous[0]) / 300.0, (auxiliary_count - auxiliary_previous) / 300.0,
        (section_counts[1] - section_previous[1]) / 300.0, (section_counts[2] - section_previous[2]) / 300.0,
        (section_counts[3] - section_previous[3]) / 300.0, (section_counts[4] - section_previous[4]) / 300.0,
        (section_counts[5] - section_previous[5]) / 300.0);
    memcpy(section_previous, section_counts, sizeof section_previous); auxiliary_previous = auxiliary_count;
    if (vk_one_primary_on) {
        static uint64_t previous[2];
        uint64_t times[2]; vk_sections_times(times);
        LOGV("recorder overhead ms/frame: inline %.3f execute %.3f reset walking/worker %.3f/%.3f job setup %.3f (wait above; reset overlaps inline/worker busy); section secondary begin/end %.3f section assembly %.3f",
            inline_ns / 3e8, execute_ns / 3e8, reset_ns / 3e8, worker_reset_ns / 3e8, job_ns / 3e8,
            (times[0] - previous[0]) / 3e8, (times[1] - previous[1]) / 3e8);
        memcpy(previous, times, sizeof previous);
        static uint64_t deferred_previous[3], command_previous[VK_SECTION_COMMAND_COUNT];
        uint64_t deferred[3], commands[VK_SECTION_COMMAND_COUNT];
        vk_sections_deferred_counts(deferred); vk_sections_command_counts(commands);
        LOGV("recorder sections per-frame: deferred records/frame %.2f deferred-only sections/frame %.2f secondary sections/frame %.2f",
            (deferred[0] - deferred_previous[0]) / 300.0,
            (deferred[1] - deferred_previous[1]) / 300.0,
            (deferred[2] - deferred_previous[2]) / 300.0);
        /* One line per nonzero command keeps exact identities without a fixed
         * formatting buffer; all lines use the existing 300-frame cadence. */
#define VK_SECTION_LOG(name) \
        if (commands[VK_SECTION_##name] != command_previous[VK_SECTION_##name]) \
            LOGV("recorder immediate commands/frame: " #name " %.2f", \
                (commands[VK_SECTION_##name] - command_previous[VK_SECTION_##name]) / 300.0);
        VK_SECTION_COMMANDS(VK_SECTION_LOG)
#undef VK_SECTION_LOG
        memcpy(deferred_previous, deferred, sizeof deferred_previous);
        memcpy(command_previous, commands, sizeof command_previous);
    } else LOGV("recorder overhead ms/frame: inline %.3f execute %.3f reset walking/worker %.3f/%.3f job setup %.3f (wait above; reset overlaps inline/worker busy)", inline_ns / 3e8, execute_ns / 3e8, reset_ns / 3e8, worker_reset_ns / 3e8, job_ns / 3e8);
    uint64_t retained = atomic_load(&memory_draws) + atomic_load(&memory_info);
    LOGV("recorder storage: %.1f/%llu MiB packet/descriptor slabs (active over-budget=%d, referenced block capacity %.1f MiB), retired-slot reclaims %llu/%.1f MiB, secondary CBs reclaimed %llu",
        retained / 1048576.0, (unsigned long long)(storage_limit >> 20), retained > storage_limit, storage_referenced / 1048576.0,
        (unsigned long long)atomic_load(&storage_reclaims), atomic_load(&storage_reclaimed) / 1048576.0,
        (unsigned long long)atomic_load(&cb_reclaimed));
    uint64_t active = 0, peak = 0, returned = 0, cold = 0;
    for (int worker = 0; worker <= threads; worker++) {
        active += atomic_load(&command_stats[worker].in_use); peak += atomic_load(&command_stats[worker].peak);
        returned += atomic_load(&command_stats[worker].returns); cold += atomic_load(&command_stats[worker].cold_resets);
    }
    LOGV("recorder secondary leases: retained %llu, active %llu, summed worker peak %llu, returned %llu, cold resets %llu; pools are recording-owner-only across slots",
        (unsigned long long)atomic_load(&memory_cbs), (unsigned long long)active, (unsigned long long)peak,
        (unsigned long long)returned, (unsigned long long)cold);
    static uint64_t previous_lease[2], previous_return[2], previous_reset[2], previous_batches[2];
    static uint64_t previous_publish, previous_publications, previous_trim;
    uint64_t lease[2] = {0}, recycle[2] = {0}, reset[2] = {0}, batches[2] = {0};
    commands_costs(lease, recycle, reset, batches);
    uint64_t publish = atomic_load_explicit(&return_publish_ns, memory_order_relaxed);
    uint64_t publications = atomic_load_explicit(&return_publications, memory_order_relaxed);
    LOGV("recorder lease overhead ns/frame walking/workers: acquire %.0f/%.0f return %.0f/%.0f cold maintenance %.0f/%.0f; background publication %.0f; slot publications/owner batches %.2f/%.2f; walking slab prune %.0f",
        (lease[0] - previous_lease[0]) / 300.0, (lease[1] - previous_lease[1]) / 300.0,
        (recycle[0] - previous_return[0]) / 300.0, (recycle[1] - previous_return[1]) / 300.0,
        (reset[0] - previous_reset[0]) / 300.0, (reset[1] - previous_reset[1]) / 300.0,
        (publish - previous_publish) / 300.0, (publications - previous_publications) / 300.0,
        (batches[0] + batches[1] - previous_batches[0] - previous_batches[1]) / 300.0,
        (storage_trim_ns - previous_trim) / 300.0);
    memcpy(previous_lease, lease, sizeof lease); memcpy(previous_return, recycle, sizeof recycle);
    memcpy(previous_reset, reset, sizeof reset); memcpy(previous_batches, batches, sizeof batches);
    previous_publish = publish; previous_publications = publications; previous_trim = storage_trim_ns;
    LOGV("recorder background assembly/frame: slots %.2f worker join %.3f ms (not walking wait)", background_slots / 300.0, background_wait_ns / 3e8);
    LOGV("recorder scheduling/frame: jobs %.1f ready-delay/service %.3f/%.3f ms (max %.3f/%.3f); waited jobs/draws %.1f/%.1f max chunk %llu waited-job resets %.3f ms",
        jobs_serviced / 300.0, ready_delay_ns / 3e8, service_ns / 3e8, max_ready_delay_ns / 1e6, max_service_ns / 1e6,
        join_jobs / 300.0, join_draws / 300.0, (unsigned long long)max_join_draws, join_reset_ns / 3e8);
    LOGV("recorder waited passes/frame %.2f, contained draws avg/max %.1f/%llu (not remaining draw instructions)",
        join_passes / 300.0, join_passes ? (double)join_pass_draws / join_passes : 0, (unsigned long long)max_join_pass_draws);
    LOGV("recorder submit guards/frame: shards %.1f projected VA ranges %.1f unique units %.1f; prep/merge/service/join/critical %.3f/%.3f/%.3f/%.3f/%.3f ms (prep overlaps recording; service is summed worker time)",
        guard_jobs / 300.0, guard_intents / 300.0, guard_units / 300.0, guard_prep_ns / 3e8,
        guard_merge_ns / 3e8, guard_service_ns / 3e8, guard_wait_ns / 3e8, guard_critical_ns / 3e8);
    { static uint64_t previous[4]; uint64_t now[4] = {rec_wakes_walker, rec_wakes_chain, rec_spin_hits, rec_parks};
      LOGV("recorder wakes/frame: walker %.2f worker-chained %.2f, polled hits %.2f, parks %.2f (TOTK_VK_REC_WAKE_BATCH=%d)",
          (now[0] - previous[0]) / 300.0, (now[1] - previous[1]) / 300.0, (now[2] - previous[2]) / 300.0,
          (now[3] - previous[3]) / 300.0, rec_wake_batch());
      memcpy(previous, now, sizeof now); }
    memset(busy_ns, 0, sizeof busy_ns); worker_reset_ns = 0;
    ready_delay_ns = service_ns = max_ready_delay_ns = max_service_ns = jobs_serviced = 0;
    guard_jobs = guard_intents = guard_units = guard_prep_ns = guard_merge_ns = guard_service_ns = guard_wait_ns = guard_critical_ns = 0;
    background_wait_ns = background_slots = execute_ns = 0;
    join_jobs = join_draws = join_reset_ns = max_join_draws = 0;
    join_passes = join_pass_draws = max_join_pass_draws = 0;
    pthread_mutex_unlock(&rec_mu);
    walk_ns = wait_ns = walk_wait_mark = chunks_parallel = chunks_inline = passes_parallel = passes_inline = 0;
    inline_ns = reset_ns = job_ns = 0;
    memo_miss_log(0); memo_miss_log(1);
    LOGV("recorder memo recovered/frame: unrelated uniform %.1f unused binding %.1f unchanged program signature %.1f",
        memo_saved[VK_REC_M_UNIFORM] / 300.0, memo_saved[VK_REC_M_BINDING] / 300.0, memo_saved[VK_REC_M_SIGNATURE] / 300.0);
    memset(memo_saved, 0, sizeof memo_saved);
}

/* Immutable recorder inputs: position-dependent versions stay on the walk; workers resolve frozen spans and build sets. */
#pragma once
#include "vk_int.h"
#include "nvn_dirty.h"

typedef struct vk_record_spans vk_record_spans;
const vk_record_spans *vk_record_spans_capture(unsigned slot);
uint64_t vk_record_spans_generation(const vk_record_spans *spans); /* mapping identity only; 0 for NULL, never byte authority */
void vk_record_spans_reset(unsigned slot);
int vk_record_resolve(const vk_record_spans *spans, uint64_t address, uint64_t size, VkBuffer *buffer, uint64_t *offset, uint64_t *available, int hints[64]);
int vk_record_storage_aligned(const vk_record_spans *spans, uint64_t address);
int vk_ub_snapshot_descriptor(uint64_t address, uint64_t size, VkDescriptorBufferInfo *out);
uint64_t vk_record_lag_ticket(void);
int vk_record_lag_enabled(void);
int vk_record_lag_native(void); /* includes the isolated native-guard test override */
void vk_record_lag_fail(uint64_t ticket); /* atomic fallback, consumed only after registration joins */
typedef struct { uint64_t lo, hi, max_range; uint32_t reason; } vk_record_lag_intent;
int vk_record_lag_project(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                          uint64_t ticket, uint32_t reason, vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity);
int vk_record_lag_collect(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                          uint64_t ticket, uint32_t reason, nvn_dirty_lag_batch *batch,
                          vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity, uint64_t *projected);
void vk_record_lag_register(const vk_record_lag_intent *intents, uint32_t count, uint64_t ticket); /* defer software VAs only */
void vk_record_lag_batch_register(const nvn_dirty_lag_batch *batch, uint64_t ticket, unsigned shard, unsigned nshards);
int vk_lag_read_record(uint64_t address, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out, uint32_t *reason, vk_lag_kind kind);
void vk_gpu_write_record(uint64_t address, uint64_t size);
VkBufferView vk_tbuf_by_id_record(int tid, int writable, VkDescriptorBufferInfo *live, uint32_t *reason);
void vk_record_lag_guard(VkBuffer buffer, uint64_t offset, uint64_t size, uint32_t reason);
void vk_record_memory(uint64_t counts[4]); /* mapped draw/info slab bytes, secondary buffers, worker descriptor pools */

typedef struct {
    VkViewport vp; VkRect2D sc; uint32_t bias_en; float bias[3], blend[4];
    uint32_t cmask[2], wmask[2], ref[2], cull, front, topo, dt, dw, dc, st, sop[2][4], restart;
} vk_record_dynamic;
typedef struct {
    union {
        VkDescriptorBufferInfo b;
        struct { VkBuffer unresolved; uint64_t address, size; } raw; /* null buffer: worker resolves this range */
    };
    VkDescriptorImageInfo i; VkBufferView t;
    VkDescriptorBufferInfo lag_range; uint32_t lag_reason;
    uint32_t binding, element; VkDescriptorType type;
} vk_record_descriptor;
#ifdef TOTK_VK_TESTS
typedef struct { uint64_t address; VkBuffer buffer; uint64_t offset; } vk_record_test_vertex;
typedef struct { uint64_t draws, passes, index_skips, widened, freeze_hits, program_hits; } vk_record_test_counts;
#endif
typedef struct {
    VkPipeline pipeline; VkPipelineLayout layout;
    vk_record_dynamic dynamic;
    struct { VkDescriptorSetLayout layout; VkDescriptorUpdateTemplate update; uint32_t first, count; } sets[2];
    /* Non-null buffer is the walk-selected live range or immutable override; null leaves raw lookup to the worker. */
    struct { uint64_t address, size; VkDeviceSize stride; VkBuffer buffer; VkDeviceSize offset; uint32_t lag_reason; } vertex[17];
    const vk_record_spans *spans;
    uint32_t vertex_mask;
    VkBuffer index; VkDeviceSize index_offset; VkIndexType index_type;
    uint64_t index_address; /* optional unresolved index range; walk-selected handles need no worker lookup */
    uint64_t lag_ticket, index_bytes; uint32_t index_lag_reason;
    uint32_t count, instances, first, base_instance; int32_t vertex_offset;
#ifdef TOTK_VK_TESTS
    vk_record_test_vertex test_vertex;
#endif
} vk_record_draw;

int vk_prog_resolve_record(const prog_t *program, vk_shader *out[6]);
void vk_prog_record_cache_take(uint64_t *hits, uint64_t *misses);
uint64_t vk_record_uniform_revision(void);
int vk_record_uniform_unchanged(uint64_t revision, uint64_t address, uint64_t size);
int vk_record_updates_pending(void);
void vk_record_touch_buffer(uint64_t address, uint64_t size);

/* Translation diagnostics: walking/recording-lock owner only, including the cached exact-"1" env initialization.
   Off mode allocates nothing. The immutable flag can be checked once per draw after vk_xlate_stats_on(). */
extern int vk_xlate_stats_enabled;
int vk_xlate_stats_on(void);
void vk_xlate_recording_stats_frame(uint64_t frame); /* EVERY present, lock held: rotate even empty frames */
int vk_xlate_deferred_slot(unsigned stage, unsigned kind, unsigned slot); /* last bind: kind 0 texture, 1 image, 2 sampler */
enum {
    XLATE_UNIFORM_UNKNOWN = 1u << 0, /* no complete immutable/log byte authority; never count as unchanged content */
    XLATE_UNIFORM_OWNED = 1u << 1,   /* [3],[1] is a comparable authority/version pair for this exact address/extent */
    XLATE_UNIFORM_SNAPSHOT = 1u << 2,
    XLATE_UNIFORM_LOGGED = 1u << 3,
    XLATE_UNIFORM_DIRTY = 1u << 4,   /* [2] valid observation, NOT a per-range content version */
    XLATE_UNIFORM_CAPTURE = 1u << 5  /* existing lag memo retains a reusable selected descriptor */
};
/* Diagnostic-only metadata observer; caller already chose the descriptor. No byte reads, resolution, protection,
   pruning or allocation. Words: [0] flags above; [1] newest overlapping retained inline-update sequence (0 none);
   [2] existing lag memo's dirty observation (global read/write-watch token, not a digest; unrelated writes can change
   it); [3] owned submission-snapshot incarnation (0 for complete log-only authority). UINT64_MAX marks unknown
   words. Only compare [3],[1] as a content-version pair when OWNED; this proves selected authority, not byte equality.
   Descriptor buffer/offset identity is deliberately left to the caller; equal native observations do not prove bytes.
   Missing/evicted canonical-mapping metadata is unknown rather than triggering a new live lookup. */
void vk_xlate_uniform_key(uint64_t address, uint64_t size, uint64_t out[4]);
enum {
    VK_REC_M_UNIFORM, VK_REC_M_BINDING, VK_REC_M_SPAN, VK_REC_M_PROGRAM, VK_REC_M_SIGNATURE,
    VK_REC_M_REGISTRY, VK_REC_M_SAMPLER, VK_REC_M_CHUNK, VK_REC_M_SUBMISSION, VK_REC_M_SHADER,
    VK_REC_M_DIMENSIONS, VK_REC_M_COLD, VK_REC_M_DISABLED, VK_REC_M_N
};
void vk_record_memo_miss(unsigned program, unsigned causes);
void vk_record_memo_saved(unsigned cause);
uint64_t vk_record_sampler_revision(void);
uint64_t vk_record_chunk_id(void);
int vk_record_enabled(void);  /* initialized on walking thread, immutable after first slot */
void vk_record_slot_reset(unsigned slot);  /* slot fence complete and all CPU jobs consumed */
void vk_record_slot_retire(unsigned slot); /* actual GPU/post completion AND submit-owner retirement; publishes returns only */
void vk_record_pass_begin(uint32_t ncolor, const VkFormat *color, VkFormat depth, VkFormat stencil, VkSampleCountFlagBits samples);
vk_record_draw *vk_record_draw_begin(VkPipeline pipeline, VkPipelineLayout layout);
/* The construction pointer is borrowed until commit/abort; the next begin may grow an unpublished job's array. */
vk_record_descriptor *vk_record_descriptors(vk_record_draw *draw, unsigned set, VkDescriptorSetLayout layout, uint32_t count);
VkDescriptorUpdateTemplate vk_record_template(VkDescriptorSetLayout layout, const vk_record_descriptor *input, uint32_t count); /* walking owner */
void vk_record_draw_commit(void);
void vk_record_draw_abort(void);
void vk_record_pass_flush(void); /* walking owner: dispatch final chunk before closing profiler CPU bookkeeping */
/* Caller selects the entry with cached vk_gpuprof_on; both return 1: parked, 0: empty pass ended.
   Distinct entries leave the disabled core free of token/switch lifetimes across chunk dispatch. */
int vk_record_pass_end(VkCommandBuffer primary);
/* Enabled profiler only: captures the optional end token and emits it after EndRendering on this exact primary,
   now or at assembly. Use this entry even for a NULL token when profiling is enabled. */
int vk_record_pass_end_profiled(VkCommandBuffer primary, const vk_gpuprof_end_token *profile_end);
void vk_record_slot_seal(unsigned slot); /* walking owner detaches immutable pending primaries and flushes queued chunks */
void vk_record_assemble(unsigned slot, int background); /* sole slot-pool owner, before QueueSubmit */
void vk_record_walk_wait(uint64_t ns); /* walking-only join of actual queue publication */
void vk_record_lag_submit(unsigned slot, uint64_t ticket, const nvn_dirty_lag_batch *walking, int background); /* sealed slot only; join arm shards before QueueSubmit */
VkCommandBuffer vk_record_next_primary(void); /* park current rendering primary; begin the next ordered section */
void vk_record_clear(const VkClearAttachment *attachment, const VkClearRect *rect);
void vk_record_timestamp(VkQueryPool pool, uint32_t query, VkPipelineStageFlags2 stage);
void vk_record_dynamic_emit(VkCommandBuffer cb, const vk_record_dynamic *state, vk_record_dynamic *last, int all);
void vk_record_walk_time(uint64_t ns);  /* submission elapsed, excluding GPU waits */
void vk_record_stats(uint64_t frame);
void vk_record_descriptor_walk(uint64_t ns); /* walking owner: complete bind/selection time, not just driver calls */
void vk_record_descriptor_primary(uint64_t sets, uint64_t calls, uint64_t resets); /* serialized primary owner */
void vk_record_primary_pools(int64_t delta); /* primary slots' retained descriptor pools, for the descriptors line */
/* The one descriptor-pool shape of every recorder context and primary slot (vk_record.c). It declares every type a
   backend layout uses: the reflected shader types (vk_reflect.py) and those of the post / presentation / frame-generation
   layouts. A type a pool does not declare has zero capacity, so a layout using it would fail in a fresh pool (DIE);
   vk_desc_layout_fits refuses such a layout where it is created instead, naming the type. */
enum { VK_DESC_POOL_TYPES = 8 };
extern const VkDescriptorPoolSize vk_desc_pool_sizes[VK_DESC_POOL_TYPES];
int vk_desc_layout_fits(const VkDescriptorSetLayoutBinding *bindings, uint32_t count, char *why, size_t size);
/* Descriptor-set stock (TOTK_VK_DESC_STOCK=1, read once with the recorder controls). One stock per owner that
   used to reset its pools each slot generation: take only checks out sets not yet issued this generation (the
   caller rewrites every one), retire rewinds them at that same retirement point and returns a reason when the owner
   must reset the pools it used since the last rebuild (the stock is already invalidated).
   TOTK_VK_DESC_STOCK_MAX_SETS (vk_desc_stock_ceiling, live at the same handoff) is a hard per-owner ceiling: the stock
   never holds more sets; a checkout past it is allocated outside the stock, and the handoff then resets the owner
   (set limit). An owner also keeps at most vk_desc_stock_pool_limit pools: one with more resets and destroys the rest
   (pool limit). Reasons are counted on the descriptors line. */
enum { VK_DESC_STOCK_BATCH = 32, VK_DESC_PRIMARY_POOL_SETS = 4096 /* a primary slot's pool (nvn_vk.c) */ };
enum { VK_DESC_STOCK_OFF, VK_DESC_STOCK_PRESSURE, VK_DESC_STOCK_SET_LIMIT, VK_DESC_STOCK_POOL_LIMIT, VK_DESC_STOCK_REASONS };
typedef struct vk_desc_stock vk_desc_stock;
typedef uint32_t (*vk_desc_stock_alloc)(void *owner, VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *out); /* 1..count */
int vk_desc_stock_enabled(void);   /* the launch value */
int vk_desc_stock_want(void);      /* the live value an owner takes at its generation handoff (runtime/live_ctl.h) */
uint32_t vk_desc_stock_ceiling(void);   /* live sets per owner, 0: no ceiling */
uint32_t vk_desc_stock_pool_limit(uint32_t ceiling, uint32_t pool_sets);   /* pools an owner keeps; UINT32_MAX without a ceiling */
VkDescriptorSet vk_desc_stock_take(vk_desc_stock **stock, VkDescriptorSetLayout layout, vk_desc_stock_alloc alloc, void *owner);
int vk_desc_stock_retire(vk_desc_stock *stock, uint32_t ceiling, uint32_t pools, uint32_t pool_sets); /* 0: keep */
void vk_desc_stock_free(vk_desc_stock **stock);   /* the owner switched the stock off: its bookkeeping, not its pools */
void vk_record_descriptor_stock_event(unsigned reason, uint64_t pools_destroyed); /* serialized primary owner */
int vk_record_counter_timestamp(VkQueryPool pool, uint32_t query, VkPipelineStageFlags2 stage);
#ifdef TOTK_VK_TESTS
void vk_record_test_descriptor_retained(uint64_t out[4]); /* recorder pools, primary pools, stock sets, bytes per pool */
void vk_record_test_descriptors(uint64_t out[8]); /* sets, allocate calls, resets, walk ns, list ns/count, template ns/count */
void vk_record_test_wakes(uint64_t out[4]); /* TOTK_VK_REC_WAKE_BATCH: walker signals, worker-chained signals, polled hits, parks */
int vk_record_test_descriptor_stock(void); /* isolated real-driver checkout/recycle/batch/pressure fixture */
int vk_record_test_descriptor_ceiling(void); /* the stock ceiling on one recorder context, set live at its handoffs */
void vk_test_descriptor_pools_check(unsigned slot); /* nvn_vk.c: slot's pools may be reset/destroyed now (counted) */
void vk_test_descriptor_pools_stats(uint64_t *checks, uint64_t *violations);
void vk_record_test_stock_events(uint64_t out[VK_DESC_STOCK_REASONS + 1]); /* published resets per reason, pools destroyed */
void vk_record_test_storage(uint64_t values[4]); /* retention budget, reclaimed slots/bytes/secondary CBs */
void vk_record_test_primary_memory(uint64_t values[2]); /* retained primary CBs, reclaimed primary CBs */
void vk_record_test_leases(uint64_t values[4]); /* published active leases, summed worker peak, returned leases, cold resets */
typedef struct {
    uint64_t lease_ns[2], return_ns[2], reset_ns[2], return_batches[2]; /* walking / summed workers */
    uint64_t publish_ns, publications, slab_trim_ns, referenced_bytes;
    uint64_t arena_ns[2], arena_compacts[2]; /* slot / snapshot, walking only */
} vk_record_test_costs;
void vk_record_test_costs_get(vk_record_test_costs *out);
void vk_record_test_arena_costs(vk_record_test_costs *out);
void vk_record_test_counts_get(vk_record_test_counts *out);
uint64_t vk_prog_record_test_hits(void);
void vk_record_test_submit_status(uint64_t *reserved, uint64_t *queued);
void vk_record_test_hold_submit(uint64_t ticket); /* isolated test: retain owner pin after queue publication; 0 releases */
uint64_t vk_record_test_submit_held(void);
void vk_record_test_timing(uint64_t *busy, uint64_t *background_wait);
uint64_t vk_record_test_walk_wait(void);
void vk_record_test_assemble(void);
int vk_record_test_async_submit(void);
int vk_record_test_take_failure(uint64_t ticket);
void vk_record_test_alias(void *address, uint64_t source, uint64_t size);
/* Test-only canonicalization/remap handoff; fixture owns the atomics through remapper join. */
void vk_record_test_gpu_write_pause(_Atomic int *ready, _Atomic int *resume);
uint64_t vk_record_test_gpu_write_guards(void);
void vk_record_test_pause_draws(uint32_t minimum);
void vk_record_test_delay(uint32_t maximum_us); /* deterministic varied worker timing; zero disables */
uint64_t vk_record_test_pointer_hits(void);
void vk_record_test_pause(void);
int vk_record_test_wait_paused(void);
void vk_record_test_release(void);
void vk_record_test_flush_jobs(void);
void vk_record_test_native_guards(int enabled); /* isolated read-only input fixture only; never GPU-written host VAs */
void vk_record_test_expect_vertex(uint64_t address, VkBuffer buffer, uint64_t offset);
void vk_record_test_results(uint64_t *checks, uint64_t *failures, uint64_t *resets);
uint64_t vk_record_test_lag_fallbacks(void);
uint64_t vk_record_test_wait_value(void);
/* Fixture-owned atomics remain live through finalizer join; no callback runs in teardown. */
void vk_pool_test_wait_ticket(_Atomic uint64_t *ticket);
void vk_pool_test_pause_after_wait(_Atomic int *waited, _Atomic int *retry);
void vk_pool_test_stats(uint64_t out[4]);
typedef struct { uint64_t lo, hi; VkBuffer buffer; uint64_t offset; } vk_record_test_span;
vk_record_spans *vk_record_test_spans_create(const vk_record_test_span *spans, uint32_t count, int linear);
void vk_record_test_spans_destroy(vk_record_spans *spans);
int vk_record_test_project(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                            uint32_t reason, int reference, vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity);
#endif

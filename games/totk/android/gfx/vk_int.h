/* Internals shared by the Vulkan backend's files:
     nvn_vk.c     device, memory pools, submissions and their completion, the command walker
     vk_res.c     textures (images, uploads from guest memory, views), samplers, memory sub-allocation
     vk_shader.c  the shader seed (variants, SPIR-V, reflection), programs, pipelines
     vk_draw.c    draw state, render passes, draws, dispatches, copies, clears, present */
#pragma once
#include "vk_load.h"
#include "vk_fmt.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include "thread_place.h"
#include "frame_lag.h"
#include "vk_canvas.h"
#include "../../runtime/host_wait.h"
#include "../../runtime/game_error.h"
#include "vk_pcache.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include <vulkan/vulkan_android.h>
#endif

uint64_t host_now_ns(void);
#define LOGV(...) do { fprintf(stderr, "[nvn-vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DIE(...) do { LOGV(__VA_ARGS__); fflush(stderr); abort(); } while (0)
/* A failed Vulkan call is fatal; device lost is also named in the app's menu (runtime/game_error.h) before the abort. */
#define VK_DIE_RESULT(r, ...) do { LOGV(__VA_ARGS__); fflush(stderr); \
    if ((r) == VK_ERROR_DEVICE_LOST) runtime_android_game_error(RUNTIME_GAME_ERROR_GPU_LOST); abort(); } while (0)
#define VKCHECK(call) do { VkResult r_ = (call); if (r_) VK_DIE_RESULT(r_, "%s failed: %d", #call, r_); } while (0)
/* Mandatory CPU storage must fail at its allocation site, never publish NULL into a packet/log.
   Optional guard collection keeps its existing fail-closed return paths instead. */
#ifdef TOTK_VK_TESTS
int vk_test_heap_failure(const char *name);
void vk_test_fail_heap(const char *name);
#endif
static inline void *vk_heap_resize(void *old, size_t bytes, const char *function, const char *name) {
#ifdef TOTK_VK_TESTS
    void *p = vk_test_heap_failure(name) ? NULL : realloc(old, bytes);
#else
    void *p = realloc(old, bytes);
#endif
    if (!p && bytes) DIE("host allocation failed in %s: %s, requested %zu bytes", function, name, bytes);
    return p;
}
static inline void *vk_heap_zero(size_t count, size_t size, const char *function, const char *name) {
    if (size && count > SIZE_MAX / size) DIE("host allocation overflow in %s: %s, %zu x %zu bytes", function, name, count, size);
#ifdef TOTK_VK_TESTS
    void *p = vk_test_heap_failure(name) ? NULL : calloc(count, size);
#else
    void *p = calloc(count, size);
#endif
    if (!p && count && size) DIE("host allocation failed in %s: %s, requested %zu bytes", function, name, count * size);
    return p;
}
#define VK_REALLOC(pointer, bytes) vk_heap_resize((pointer), (bytes), __func__, #pointer)
#define VK_MALLOC(bytes) vk_heap_resize(NULL, (bytes), __func__, "malloc")
#define VK_CALLOC(count, size) vk_heap_zero((count), (size), __func__, "calloc")
/* a red line the first few times (the Mac's RED_ONCE) */
#define RED(n, ...) do { static int said_; if (said_++ < (n)) LOGV("【红】" __VA_ARGS__); } while (0)

/* CPU recording split (TOTK_VK_REC_PROFILE=1 or frame-timeline sampled walk timing).
   With both disabled each probe is one predictable branch: no clock, TLS lookup, allocation or update.
   Scopes are exclusive, including nested S/O work. Only mtl_sub_begin/commit and explicit full-profile
   present activate the calling thread, under vk_rec_lock. Timeline-only timing samples opcode scopes
   at 1/64 with 64x weighted ns; setup/commit outside the opcode stream are exact, unweighted scopes. */
enum {
    VK_REC_S, VK_REC_RESOLVE, VK_REC_BIND, VK_REC_PROG, VK_REC_PIPE, VK_REC_VERTEX, VK_REC_CMD, VK_REC_O,
    VK_REC_S_STREAM, VK_REC_S_STATE, VK_REC_S_UNIFORM, VK_REC_S_COUNTER, VK_REC_S_PASS, VK_REC_S_SNAPSHOT,
    VK_REC_S_WRITE, VK_REC_S_RESOURCE, VK_REC_S_TOUCH,
    VK_REC_O_SLOT, VK_REC_O_RESET, VK_REC_O_PRIMARY, VK_REC_O_SUBMIT, VK_REC_O_QUEUE, VK_REC_O_GPU_WAIT,
    VK_REC_O_WORKER_WAIT, VK_REC_O_EXECUTE, VK_REC_O_ASSEMBLE,
    VK_REC_O_DISPATCH, VK_REC_O_TRANSFER, VK_REC_O_UPLOAD, VK_REC_O_PRESENT, VK_REC_N
};
extern int vk_rec_profile_on, vk_rec_timing_on; /* initialized once; timing includes either opt-in mode */
int vk_rec_profile_push(int category);
void vk_rec_profile_pop(int previous);
void vk_rec_profile_begin(int category);
void vk_rec_profile_end(void);
void vk_rec_profile_present(uint64_t frame);
void vk_rec_profile_pass_begin(void);
void vk_rec_profile_pass_end(void);
uint64_t vk_rec_profile_draw_begin(void);
void vk_rec_profile_draw_end(uint64_t p0, int recorded);
#define VK_REC_DO(category, ...) do { \
    if (__builtin_expect(vk_rec_timing_on, 0)) { \
        int rec_prev_ = vk_rec_profile_push(category); \
        __VA_ARGS__; \
        vk_rec_profile_pop(rec_prev_); \
    } else { __VA_ARGS__; } \
} while (0)
#define VK_REC_VALUE(category, expression) __extension__ ({ \
    __typeof__(expression) rec_value_; \
    VK_REC_DO(category, rec_value_ = (expression)); \
    rec_value_; \
})
/* Opt-in hitch attribution: initialized before any backend worker starts; disabled calls only test this flag.
   Counters are independent relaxed atomics, not an atomic snapshot or a causal proof of a frame's stall. */
extern int g_hitch_on;
#define VK_HITCH_FIELDS(X) \
    X(pipes) X(pipes_sync) X(pipe_ns) X(pipes_fail) X(pipe_skips) X(pipe_bounds) X(pipe_retries) \
    X(shloads) X(shbytes) X(vlists) X(sreq) X(dpools) X(chunks) X(cpu_steps) \
    X(idles) X(idle_ns) X(idle_fin) X(idle_flush) X(idle_pool) X(inval) X(inval_ns) X(wb) X(wb_bytes) \
    X(subs) X(replay_ns) X(wait_ns) X(gwaits) X(gwait_ns) X(comp_ns) X(wake_ns)
typedef struct {
#define HK_FIELD(name) _Atomic uint64_t name;
    VK_HITCH_FIELDS(HK_FIELD)
#undef HK_FIELD
} vk_hitch;
extern vk_hitch g_hk;
#define HK_ADD(field, n) do { if (g_hitch_on) atomic_fetch_add_explicit(&g_hk.field, (n), memory_order_relaxed); } while (0)
uint64_t vk_game_submit_serial(void);    /* walking thread only; GPU chunks do not advance it */

/* Every device-level entry point the backend calls, loaded once (nvn_vk.c). */
#define VK_DEVICE_FUNCS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueWaitIdle) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkGetImageSubresourceLayout) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) X(vkFlushMappedMemoryRanges) \
    X(vkGetMemoryFdKHR) X(vkGetMemoryFdPropertiesKHR) X(vkGetMemoryHostPointerPropertiesEXT) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkTrimCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdExecuteCommands) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateEvent) X(vkDestroyEvent) X(vkSetEvent) X(vkCmdWaitEvents) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkMergePipelineCaches) X(vkDestroyPipelineCache) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateDescriptorUpdateTemplate) X(vkUpdateDescriptorSetWithTemplate) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdCopyQueryPoolResults) X(vkCmdWriteTimestamp2) X(vkGetQueryPoolResults) \
    X(vkCmdPipelineBarrier2) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) \
    X(vkCmdPushConstants) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetLineWidth)
#define VK_DECL(name) extern PFN_##name name;
VK_DEVICE_FUNCS(VK_DECL)
#undef VK_DECL

/* ---------- device (nvn_vk.c) ---------- */
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue queue; uint32_t qfam;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp;
    uint32_t timestamp_bits;   /* selected queue family's timestampValidBits */
    int has_pipeline_statistics;   /* pipelineStatisticsQuery, already enabled with supported core features */
    int has_host_query_reset;   /* hostQueryReset, for fence-owned profiling pools shared by pre/main buffers */
    int has_custom_border, has_null_descriptor, has_index_u8, has_depth_clip_control, has_eds3, has_divisor, has_bc;
    int has_geometry, has_layer_output, has_logic_op, has_dual_src;
    int has_image_read_without_format, has_image_write_without_format;
    int has_cache_control;   /* pipelineCreationCacheControl: a pipeline can be asked of the pipeline cache alone (vk_draw.c pipe_get) */
    int has_pipeline_library;
    int pipeline_library_fast_linking, pipeline_library_independent_interpolation;
    int subgroup_fix, subgroup_control;
    VkPhysicalDeviceSubgroupProperties subgroup;
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup_sizes;
    VkSampleCountFlags samples;
    VkPipelineCache pcache;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_props;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_image_format_props;
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    int ahb_present;   /* opt-in explicit LINEAR dma-buf import + foreign queue ownership */
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format_props2;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_modifier;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props; /* optional compressed-present import only */
#endif
} vk_dev;
extern vk_dev V;
void mtl_init(void);
int vk_pick_type(uint32_t bits, VkMemoryPropertyFlags want);
/* External-engine split, under vk_rec_lock/outside rendering. Only opt-in
 * engine callers use this; ordinary submissions are unchanged.
 * Signal seals the current primary and publishes its binary semaphore signal.
 * Its descriptor pools, transient buffers and live snapshot arenas stay pinned
 * through the NEXT ordinary submission's completion (the terminal replay).
 * Zero refuses a split at the bounded pin limit without sealing anything.
 * Wait attaches a binary semaphore to that next slot; neither call GPU-waits.
 * Even a failed/canceled external job must seal a continuation before starting
 * another guest snapshot. Never use a signal as an unbounded replay lease. */
void vk_submit_external_enable(void); /* walking owner, only for an admitted handoff */
int vk_submit_external_disable(void); /* nonblocking; false until all leases/semaphores drain */
uint64_t vk_submit_signal(VkSemaphore signal);
void vk_submit_wait(VkSemaphore wait);
/* Optional prepared generation: recorder owner, under vk_rec_lock, NO open CB.
 * Atomically claims two free/unleased slots for signal + continuation, without
 * waiting or reserving a ticket. Failure leaves the recorder untouched.
 * Adopted storage belongs to the caller through the covering FIFO ticket.
 * End releases an unused spare after a failed pre-split handoff. */
int vk_submit_optional_begin(void);
/* Optional preclaimed split/consumer lane that retains transient/descriptor ownership only. */
int vk_submit_optional_begin_immutable(void);
void vk_submit_optional_cancel(void); /* before signal: return both claims, no ticket */
void vk_submit_adopt_primary(VkCommandBuffer closed);
void vk_submit_optional_end(void);
/* Walking owner, under vk_rec_lock, with a current recording slot. Begin
 * immediately before submitting the real recorder whose terminal bindings will
 * be replayed. No current slot or exhausted pin capacity returns zero.
 * A nonzero token keeps its entire slot (transient arena, descriptor/command pools, recorder leases)
 * and live UBO snapshot arenas until end's covering submission retires.
 * Zero refuses admission without sealing; skip adaptive replay in that case.
 * End once after ALL phase submissions, including no-output/cancel/failure,
 * before frame-end. Ticket 0 selects the latest reservation; older tickets are
 * extended to it. Open work/split continuations defer release to their next
 * seal. Future tickets or stale tokens return false without changing the pin.
 * Neither function GPU-waits or allocates; external-engine mode is not required. */
uint64_t vk_replay_lease_begin(void);
/* Same bounded slot/pool lease, but deliberately excludes submission snapshot arenas. */
uint64_t vk_replay_lease_begin_immutable(void);
/* Recorder-owner copy from an already-written host-mapped arena range; never reads guest memory. */
int vk_replay_frozen_buffer_copy(VkBuffer buffer,VkDeviceSize offset,VkDeviceSize bytes,void *destination);
int vk_replay_lease_end(uint64_t token,uint64_t covering_ticket);
/* Actual backend-owned VkDeviceMemory objects, not opaque driver-internal command/descriptor BOs. */
typedef enum {
    VK_MEMORY_POOL, VK_MEMORY_SLOT, VK_MEMORY_SNAPSHOT, VK_MEMORY_IMAGE_BLOCK, VK_MEMORY_IMAGE_DEDICATED,
    VK_MEMORY_PRESENT, VK_MEMORY_READBACK, VK_MEMORY_ZERO, VK_MEMORY_DIAGNOSTIC, VK_MEMORY_OVERLAY,
    VK_MEMORY_PURPOSES
} vk_memory_purpose;
VkResult vk_memory_allocate(const VkMemoryAllocateInfo *info, VkDeviceMemory *memory, vk_memory_purpose purpose);
void vk_memory_release(VkDeviceMemory memory, vk_memory_purpose purpose);
void vk_memory_counts(uint64_t live[VK_MEMORY_PURPOSES]);
/* Caller has completed all GPU/recording uses of this buffer; clears cached/retired texel views. */
void vk_res_buffer_released(VkBuffer buffer);

/* Which pool buffer holds [addr, addr + len): 1 with the buffer and offset (nvn_vk.c). */
int vk_resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off);
void vk_ub_touch(uint64_t addr, uint64_t n);   /* a non-uniform GPU access of [addr, +n): logged uniform updates there reach memory first */
void vk_ub_flush(void);
void vk_gpu_write(uint64_t addr, uint64_t n);   /* the GPU writes [addr, +n) in this submission: no uniform snapshot of it (now or while in flight) */
/* TOTK_VK_OFF=<name>,<name>,...: turn individual optimisations off (to find which one misbehaves). Names: rename (uniform
   renaming), ds (descriptor-set reuse), dyn (dynamic-state skipping), block (driver bank / c1 reuse), ts (GPU
   timestamps), span (pool index; off = the old linear scan), all. */
int vk_off(const char *name);
/* One thread records at a time: the replay thread holds this from mtl_sub_begin to mtl_sub_commit, and game threads
   take it for texel reads / writes (nvnTextureReadTexels / WriteTexels run on the game's threads while the replay
   thread may be recording the next submission). Recursive. */
void vk_rec_lock(void);
/* Audited texel/finalize contended acquisition. Complete host work and unlock
 * BEFORE core reacquisition; recursive/uncontended locks never release. */
void vk_rec_lock_texels(runtime_host_wait *wait);
void vk_rec_unlock_texels(runtime_host_wait *wait);
uint64_t vk_submit_now(void);   /* submit what is recorded without waiting; its fence (the last one if nothing was recorded) */
void vk_wait_fence(uint64_t ticket); /* wait this already-submitted ticket, not unrelated later work */
uint64_t vk_submit_readback(void); /* seal/queue an open stream before CPU readback ownership, without a global GPU wait */
uint64_t vk_present_ready_ns(uint64_t ticket); /* predict/framegen exact CPU completion observation; 0 if missing/expired */
void vk_submit_chunk(void);     /* inside a game submission: submit what is recorded so far (render pass starts) */
int vk_submit_chunk_possible(void); /* vk_submit_chunk would submit: inside a game submission with an open slot */
void vk_gpu_time_guest_submit(uint64_t ns, uint64_t origin_ticks, uint64_t frame); /* next walk's immutable guest entry context */
int vk_gpu_time_hardware(void);
uint64_t vk_gpu_time_controller_end(uint64_t addr, uint64_t timestamp, uint64_t origin); /* v143 FPS END getter only; returns ns preserving producer work */
long vk_frame_chunk_limit(void); /* walking/startup only; explicit limit or effective lag default */
int vk_in_pass(void);            /* a render pass is open on the recording thread (vk_draw.c) */
void vk_pd_write_note(const char *what, uint64_t dst, uint64_t n, const void *data);   /* TOTK_VK_PASS_DUMP: a buffer write in index.txt (vk_draw.c) */
int vk_counter_value(uint64_t addr, uint64_t *value);   /* newest pending report, else memory; 0 if real samples are still unknown */
void vk_rec_unlock(void);
/* Synchronous host readback handed to the current owner, never a blocking recording-lock acquisition.
   A chunk service may defer until its producer's slot seals; boundary=1 is a completed slot/ownership boundary.
   Empty checks use one relaxed load/branch, no clock, lock, allocation or syscall; the walker checks every64 opcodes. */
typedef int (*vk_rec_service_fn)(void *arg, int boundary);
uint64_t vk_rec_service_call(vk_rec_service_fn function, void *arg); /* enqueue-to-service ns, excluding callback */
extern _Atomic unsigned vk_rec_service_pending; /* bit0 runnable, bit1 deferred to a slot boundary */
void vk_rec_service_run(int boundary);
static inline void vk_rec_service_check(void) {
    if (__builtin_expect(atomic_load_explicit(&vk_rec_service_pending, memory_order_relaxed) & 1u, 0))
        vk_rec_service_run(0);
}
#ifdef TOTK_VK_TESTS
void vk_rec_service_test_stats(uint64_t v[8]); /* queued, inline, cooperative, deferred, delay/hold maxima, p50/p99 upper ns */
#endif
uint64_t vk_counter_samples(void);   /* recording-time cumulative synthetic samples (precise queries are separate) */
int vk_ub_pending(uint64_t addr, uint64_t n);   /* does [addr, +n) overlap logged updates not yet in memory */
/* Totals since the last call, under the recording lock. CPU and raw elapsed include waits;
   replay_ms retains the legacy elapsed-minus-explicit-GPU-waits definition. */
void vk_perf_take(double *replay_ms, double *wait_ms, double *gpu_ms, double *cpu_ms, double *elapsed_ms, uint64_t *subs);
void vk_slot_stats(uint64_t frame); /* actual sealed slot counts and walking/background submit time */
int vk_ub_rename(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);   /* a private copy of a uniform range with logged updates */
/* Submission-owned immutable readonly bytes for a delayed same-frame replay.
   Includes ordered inline CPU updates, regardless of frame-lag mode. Zero
   rejects an unavailable range or pending GPU-only producer, never guesses it. */
int vk_ub_replay_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out);
void vk_ub_read(uint64_t addr, void *dst, size_t size);   /* the bytes a uniform binding here reads: memory or snapshot, with logged updates */
/* Recorder pointer selection: identical bytes to vk_ub_read, plus parent-range authority for outer freeze reuse.
   Walking thread only; unstable/untracked/GPU-native parents advance vk_lag_copy_unwatched independently of children. */
void vk_ub_read_record(uint64_t addr, void *dst, size_t size);
/* Protected lag + rename OFF: immutable, whole-game-submission UBO descriptor, or 0 for a true live GPU producer.
   Owned eager snapshots bind unchanged subranges directly. Overlay windows share only within an actual ticket and
   snapshot incarnation; native captures retain all-writer watches and never expand outside the requested range.
   Walking thread only; use before generic uniform selection in inline and recorder paths. */
int vk_lag_uniform_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out);
/* Ordered pre-render materialization for true original-memory aliases only; never allocates a UBO copy. */
void vk_lag_uniform_prepare(uint64_t addr, uint64_t size);
/* TOTK_VK_PREPARE_MASK: walk-owned invalidation for that pre-render materialization, NOT selected-content authority.
   1 when an overlay, original-memory producer or mapping revision advanced since the last call. The check variant runs
   a preparation the masks skipped and dies if it still had to materialize (TOTK_VK_PREPARE_CHECK oracle). */
int vk_lag_uniform_prepare_changed(void);
void vk_lag_uniform_prepare_check(uint64_t addr, uint64_t size);
int vk_lag_uniform_private(void);
/* Walking-only monotonic counter: a capture without stable native writer-watch authority must not enter an outer
   freeze memo. Sample before/after a stage freeze and cache that stage only if the counter did not change. */
uint64_t vk_lag_copy_unwatched(void);
/* Walking thread only. Copy mode returns immutable buffer info; wait mode returns 0 and guards the original CPU-owned
   range at chunk submission. Call before publishing recorder packets; workers never call this or skip the registration. */
/* One acquire load on the hot path; pressure cutover publishes 0 only after draining protected work. */
extern _Atomic int vk_lag_capture_state;
int vk_lag_capture_init(void);
static inline int vk_lag_capture_on(void) {
    int state = atomic_load_explicit(&vk_lag_capture_state, memory_order_acquire);
    return state < 0 ? vk_lag_capture_init() : state;
}
typedef enum {
    VK_LAG_VERTEX, VK_LAG_INDEX, VK_LAG_STORAGE, VK_LAG_UNIFORM, VK_LAG_POINTER,
    VK_LAG_TEXEL, VK_LAG_COPY, VK_LAG_OTHER, VK_LAG_KIND_COUNT
} vk_lag_kind;
/* Cumulative walking-owner counts. Decisions exclude disabled/invalid reads. binding_hits are mapping/tracking
   identity hits, including metadata_cross_ticket/snapshot; key_ticket/snapshot are byte-key changes, not metadata
   misses. class_hits/misses classify new extents; hot_refreshes track real-frame/CPU-history refreshes.
   copy_hits/misses count exact descriptor reuse attempts; ticket/snapshot misses count retained captures revoked
   by those identities (nonexclusive), content misses count dirty/range-proof failures. Cold live guards do not
   attempt copied-byte reuse. uniform_snapshot_binds/cover_hits/misses exclude exact memo hits.
   Sampled ns/count measures every 64th whole decision, not an estimate. Snapshot bytes count requested arena
   allocation bytes (including alignment prefix), not arena capacity growth. */
typedef struct {
    uint64_t decisions, memo_hits, memo_misses, binding_hits, binding_misses;
    uint64_t key_range, key_ticket, key_mapping, key_snapshot, class_hits, class_misses;
    uint64_t cold_guards, hot_decisions, sample_count, sample_ns, size_buckets[5], max_size;
    uint64_t hot_copies, hot_bytes, hot_ns, snapshot_allocations, snapshot_bytes;
    uint64_t metadata_cross_ticket, metadata_cross_snapshot, unit_cross_ticket, hot_refreshes;
    uint64_t copy_hits, copy_misses, copy_ticket_misses, copy_snapshot_misses, copy_content_misses;
    uint64_t uniform_snapshot_binds, uniform_cover_hits, uniform_cover_misses;
} vk_lag_kind_stats;
int vk_lag_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out, vk_lag_kind kind);
uint64_t vk_lag_bound(uint64_t addr, uint64_t limit);   /* smallest containing registered pool/sub-pool, including aliases */
void vk_lag_capture_take(uint64_t *bytes, uint64_t *ns, uint64_t *ranges, uint64_t *gpu_owned);
#ifdef TOTK_VK_TESTS
/* Read after mtl_sub_commit has ended profiling, while no other walking thread records. */
void vk_record_test_profile_ns(uint64_t *out, size_t count);
int vk_record_test_replay_pin(uint64_t source,uint64_t *until,uint64_t *skips);
void vk_record_test_hold_completion(uint64_t ticket);
uint64_t vk_record_test_completion_held(void);
/* Read while the recording owner is idle or on that owner; no context mutation.
 * [in_sub, guest_ns, origin_ticks, guest_frame, pending_posts, game_slots, primary_open, slots]. */
void vk_record_test_guest_context(uint64_t v[8]);
/* Existing ring ticket under g_mu: [guest_ns, origin_ticks, guest_frame, npost, join_on_walk].
 * Returns 0 if the ring no longer retains that ticket. */
int vk_record_test_ticket_context(uint64_t ticket,uint64_t v[5]);
void vk_record_test_chunk_selector_reset(void); /* recording owner only; next qualified chunk re-reads env */
uint64_t vk_record_test_snapshot_next_owner(void);
uint32_t *vk_record_test_snapshot_allocate(uint32_t value);
void vk_record_test_snapshot_allocate_range(uint32_t value,VkBuffer *buffer,VkDeviceSize *offset);
int vk_record_test_replay_snapshots(uint64_t source,unsigned *snapshots);
uint64_t vk_record_test_snapshot_external_until(void);
unsigned vk_record_test_snapshot_mask(void);
unsigned vk_record_test_snapshot_next_index(void);
enum {
    VK_LAG_TEST_COLD_HITS, VK_LAG_TEST_COPY_HITS, VK_LAG_TEST_UNIFORM_HITS, VK_LAG_TEST_PRODUCER_SCANS,
    VK_LAG_TEST_PRODUCER_HITS, VK_LAG_TEST_PRODUCER_CROSS_TICKET, VK_LAG_TEST_N
};
void vk_record_test_lag_cache_stats(uint64_t *out, size_t count);
#define VK_LAG_TEST_STATS_VERSION 2
enum { VK_LAG_TEST_CROSS_FRAME_METADATA = 1, VK_LAG_TEST_UNIFORM_COVER = 2 };
uint32_t vk_lag_test_features(void); /* compiled capabilities, independent of runtime policy */
typedef struct { vk_lag_kind_stats kind[VK_LAG_KIND_COUNT]; } vk_lag_test_stats;
void vk_lag_test_stats_get(vk_lag_test_stats *out); /* no walking thread may mutate counters during the read */
int vk_lag_test_query_bench(void); /* standalone pre-initialization query correctness/timing fixture */
int vk_draw_test_touch_bench(void); /* standalone reflection-enumeration microbenchmark */
int vk_draw_test_binding_bench(void); /* standalone exact stage-binding key microbenchmark */
int vk_draw_test_prepare_bench(void); /* --lag-walk-bench: production preparation over eight bound ranges */
void vk_draw_test_prepare_counts(uint64_t *calls, uint64_t *skips, uint64_t *checks);
#endif
void vk_lag_stats(uint64_t frame);
int vk_resolve_span(uint64_t addr, VkBuffer *buf, uint64_t *off, uint64_t *avail);   /* the buffer holding addr and the bytes it holds from there */
enum { VK_LOOKUP_UBO, VK_LOOKUP_VERTEX, VK_LOOKUP_TOUCH, VK_LOOKUP_OTHER };
int vk_resolve_for(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, int kind);   /* same lookup, tagged for TOTK_VK_LOOKUP_STATS */
void vk_lookup_stats(uint64_t frame);   /* present's 300-frame report; no work unless enabled at device init */

/* ---------- submissions (nvn_vk.c) ----------
   A submission records into two command buffers of its ring slot: "pre" (texture uploads, image creation barriers:
   things that must happen before the game's commands, as the Metal backend's upload batch) and "main" (the game's
   commands). Both go to the queue together, pre first. Per-slot transient memory (staging bytes, small uniform data,
   descriptor sets) is recycled when the slot's work has completed. */
VkCommandBuffer vk_cb(void);       /* the main command buffer being recorded (begins one if needed) */
VkCommandBuffer vk_pre_cb(void);   /* the pre command buffer of the same submission */
void vk_cpu_step(void);            /* the GPU finishes everything recorded so far (then the CPU may act in order) */
void vk_gpu_idle(void);            /* everything handed to the queue is done */
/* transient host-visible memory of the current submission (staging, driver banks, support buffer): pointer + buffer/offset */
void *vk_transient(size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off);
/* a descriptor set for the current submission */
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);
/* a full memory barrier between whatever came before and after, in the main command buffer (outside rendering) */
void vk_barrier_all(VkCommandBuffer cb);
uint64_t vk_submit_serial(void);   /* counts submissions: "once per submission" checks */
void vk_flush_wait(void);          /* the backend's own work (present, CPU reads): submit what is recorded and wait */
void vk_note_draw(void);           /* statistics */
void vk_occlusion_draw(VkCommandBuffer cb);   /* inside rendering, before a draw: begin a precise query segment if needed */
void vk_occlusion_end(VkCommandBuffer cb);    /* inside rendering, before ending the pass: close the segment */
void vk_occlusion_reset(void);               /* NVN ResetCounter(SAMPLES_PASSED), recording thread */
void vk_occlusion_stats(uint64_t frame);     /* every 300 presents, recording lock held */

/* ---------- memory (vk_res.c): device memory for images, sub-allocated from large blocks ---------- */
typedef struct { VkDeviceMemory mem; VkDeviceSize off, size; int block; } vk_alloc;
int vk_mem_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, vk_alloc *out);
void vk_mem_free(vk_alloc *a);

/* Presentation-only post chain. Caller owns the recording lock and has ended rendering.
   A slot is reusable only after its existing presentation fence (and AHB release). */
typedef struct { VkImage image; VkImageView view; uint32_t w, h; VkFormat format; } vk_post_output;
void vk_post_init(void);
int vk_native_ui_enabled(void);   /* effective host composition, immutable before presentation threads */
int vk_post_enabled(void);
int vk_post_scale(void);
int vk_post_run(VkCommandBuffer cb, unsigned slot, VkImageView source, uint32_t source_w, uint32_t source_h,
                int32_t cx, int32_t cy, uint32_t w, uint32_t h, uint32_t output_w, uint32_t output_h,
                int srgb, vk_post_output *out);
void vk_post_complete(unsigned slot);   /* after the existing fence, never waits */
/* Scene-only scaler: caller owns both full-size targets until its normal GPU
   retirement; views match target format. Pipelines initialize once per format;
   no intermediate image allocation, wait or submission occurs here. */
int vk_scene_scale_enabled(void);
int vk_scene_scale_fsr(void);
int vk_scene_aa_enabled(void);
int vk_scene_post_enabled(void); /* scene AA or spatial scaling, never post-UI */
int vk_scene_scale_run(VkCommandBuffer cb, VkImageView source, uint32_t sw, uint32_t sh,
    int32_t cx, int32_t cy, uint32_t cw, uint32_t ch,
    const vk_post_output *target, const vk_post_output *scratch);
/* AA scratch: crop-sized RG8 edges, RGBA8 weights, scene-format blend.
   All targets are caller-owned sampled/color/transfer images, retired together. */
int vk_scene_post_run(VkCommandBuffer cb, VkImageView source, uint32_t sw, uint32_t sh,
    int32_t cx, int32_t cy, uint32_t cw, uint32_t ch,
    const vk_post_output *target, const vk_post_output *scratch, const vk_post_output aa[3]);
#ifdef TOTK_VK_TESTS
uint64_t vk_scene_test_aa_runs(void);
#endif

/* Native-UI work, recorded by the walking owner. Timings reuse the opt-in pass
   profiler; counters require no queries, waits or submission boundaries. */
enum {
    VK_NUI_CANVAS_SEED, VK_NUI_WINDOW_SEED, VK_NUI_RESOLVE, VK_NUI_PATCH,
    VK_NUI_UI_PASS, VK_NUI_WINDOW_PASS, VK_NUI_PRESENT_AHB, VK_NUI_PRESENT_COPY,
    VK_NUI_POST, VK_NUI_OPS
};
int vk_native_gpu_begin(VkCommandBuffer cb, unsigned op);
void vk_native_gpu_end(VkCommandBuffer cb, int scope);
void vk_native_gpu_skip(unsigned op); /* only a proven full load-clear */
#ifdef TOTK_VK_TESTS
void vk_native_gpu_counts(uint64_t executed[VK_NUI_OPS], uint64_t skipped[VK_NUI_OPS]);
#endif

/* ---------- textures (vk_res.c) ---------- */
typedef struct vk_tex vk_tex;
/* the Vulkan texture of an NVN texture object (created / uploaded / re-uploaded as needed), NULL if it cannot be made */
vk_tex *vk_tex_get(const tex_t *t);
/* an image view for sampling / attaching: view may be NULL (whole texture); for an attachment, one level and layer
   (layer_count layers for a layered attachment) */
VkImageView vk_tex_view(vk_tex *x, const tex_t *t, const view_t *v, int attach);
int vk_xlate_texture_view_type(vk_tex *x, VkImageView view); /* diagnostic-only cached metadata; -1 if absent */
VkImage vk_tex_image(vk_tex *x);
uint64_t vk_tex_serial(vk_tex *x);
uint32_t vk_tex_samples(vk_tex *x);
VkFormat vk_tex_format(vk_tex *x);
int vk_tex_nvn_format(vk_tex *x);
uint32_t vk_tex_width(vk_tex *x, unsigned level);
uint32_t vk_tex_height(vk_tex *x, unsigned level);
uint32_t vk_tex_layers(vk_tex *x);
uint32_t vk_tex_levels(vk_tex *x);
int vk_tex_is3d(vk_tex *x);
VkImageAspectFlags vk_tex_aspect(vk_tex *x);
void vk_tex_mark_gpu_written(vk_tex *x);
/* Opt-in copy diagnostic: image facts and bounded actual GPU-region samples. */
void vk_texcopy_image(const char *op, const char *side, const tex_t *t, vk_tex *x);
void vk_texcopy_readback(const char *op, const char *side, const tex_t *t, vk_tex *x,
                         unsigned level, unsigned layer, const NVNcopyRegion *r);
/* Native UI siblings preserve the guest-sized image for all ordinary reads.
   Call prepare before opening rendering; only the verified final transfer samples native. */
void vk_native_window_size(uint32_t w, uint32_t h);
void vk_native_frame_end(void);
int vk_tex_native_window(vk_tex *x);
int vk_tex_canvas_window(vk_tex *x);
vk_canvas_image vk_tex_canvas_identity(vk_tex *x, unsigned level, unsigned layer);
int vk_tex_view_identity(vk_tex *x, VkImageView view, vk_canvas_image *image, VkImageSubresourceRange *range);
VkImageView vk_tex_view_levels(vk_tex *x, VkImageView view, uint32_t levels); /* the view's first `levels` levels only */
void vk_pso_life_external_write(vk_tex *x, unsigned mip, unsigned layer, unsigned layers, const char *source);
void vk_pso_life_external_read(uint32_t width, uint32_t height, const char *source);
VkImage vk_tex_native_image(vk_tex *x); /* identity only; does not realize or synchronize */
void vk_tex_native_transfer(vk_tex *x, uint32_t canvas_w, uint32_t canvas_h);
void vk_tex_native_basis(vk_tex *x, uint32_t *w, uint32_t *h);
void vk_tex_native_promote(vk_tex *x);
void vk_tex_native_ui_begin(vk_tex *x);
int vk_tex_native_active(vk_tex *x);
int vk_tex_native_backed(vk_tex *x); /* no allocation/synchronization */
uint64_t vk_native_descriptor_revision(void);
int vk_native_descriptor(int tid); /* conservative registered-storage prefilter */
void vk_tex_native_read(vk_tex *x);
void vk_tex_native_written(vk_tex *x, int native);
void vk_tex_native_patch(vk_tex *x, unsigned level, unsigned layer, const NVNcopyRegion *region);
VkImageView vk_tex_native_view(vk_tex *x, const view_t *view, int attach);
/* Only for a queued whole-image load-clear: single colour, no depth, base
   subresource, unpadded basis. The immediate clear defines every pixel. */
void vk_tex_native_discard(vk_tex *x);
VkImageView vk_tex_native_clear_view(vk_tex *x, const view_t *view);
int vk_tex_native_output(vk_tex *x, vk_post_output *out);
void vk_res_sweep(void);
void vk_res_counters(uint64_t v[7]);   /* images made, storage-shared reuses, uploads, re-uploads, shared images swept, CPU-pool images shared, serials switched */                    /* after each commit: destroy what the GPU is done with */
void vk_tex_pool_gone(uintptr_t lo, uintptr_t hi);
void vk_wb_gpu_wrote(uint64_t a, uint64_t n);      /* GPU copies newer than noted renderings there: not written back */
void vk_wb_range_gone(uintptr_t lo, uintptr_t hi);  /* a pool's memory goes */
void vk_wb_record(void);                         /* before sealing an ordinary command stream */
void vk_wb_stats(void);                          /* present's 300-frame summary */
uint64_t vk_wb_pool_identity(const pool_t *pool, const void *address, uint64_t size);
int vk_wb_pool_lock(uint64_t identity, const void *address, uint64_t size);
void vk_wb_pool_unlock(void);
#ifdef TOTK_VK_TESTS
void vk_wb_test_stats(uint64_t v[7]); /* landed, bytes, pending, CPU skips, superseded, staging bytes, copies */
void vk_wb_test_memory(uint64_t v[5]); /* record capacity bytes, history bytes, pinned image bytes, WB-only bytes, histories */
void vk_wb_test_history(const tex_t *t, uint64_t v[2]); /* storage landings and next quiet interval */
uint64_t vk_wb_test_invalidates(void);
void vk_wb_test_sync(uint64_t v[7]); /* preflight lookups/hits, drains, owner calls, seals, WB submissions, peak wall ns */
void vk_wb_test_sweep(uint64_t v[6]); /* interval/peak WB-sweep ns, max bytes/chunks, budget yields, sweep calls */
void vk_wb_test_tile(const tex_t *t, uint64_t v[6]); /* last GPU/landed version, write sub/serial, invalidates/stale entries */
void vk_wb_test_fresh(uint64_t v[3]); /* landed before base quiet, bytes, relevant copy-fence wait ns */
uint64_t vk_wb_test_inflight(void); /* exact preflight matches on a synchronous owner's borrowed landing list */
void vk_res_mem(uint64_t v[8]); /* resource memory summary (also read by the present statistics) */
void vk_draw_test_split_counts(uint64_t out[5]); /* splits, reopened parts, refused passes, depth touches, LRZ refusals */
void vk_draw_test_clear_counts(uint64_t out[2]); /* ClearTexture calls cleared through the region path: partial, masked */
void vk_draw_test_feedback_counts(uint64_t out[2]); /* TOTK_VK_FEEDBACK_CUT sampled views cut, left whole */
#endif
void vk_res_stats(void);   /* rendered / copied / cleared by the GPU: never re-uploaded from guest memory */
void vk_upload_threads_init(void);         /* launch-time TOTK_VK_UPLOAD_THREADS, before any upload */
void vk_upload_report(uint64_t frame);     /* every 300 presents: uploads, their wall time and parallel deswizzles */
void vk_texel_stats(uint64_t frame); /* opt-in CPU texel access counters, per 300 frames */
/* the texture registered at a texture-pool id (vk_reg), resolved to its Vulkan view; NULL for an empty slot */
VkImageView vk_tex_by_id(int tid, int *target_out, vk_tex **x_out);
void vk_tex_memo_init(void);               /* launch-time TOTK_VK_TEX_MEMO, before any lookup */
int vk_tex_memo_on(void);                  /* the value in use (live) */
void vk_tex_memo_counts(uint64_t out[5]);  /* TOTK_VK_TEX_MEMO: hits, misses, retirements (published every 4096 lookups), registrations, unchanged ones */
#ifdef TOTK_VK_TESTS
void vk_tex_lookup_counters(uint64_t out[3]); /* calling thread: requests, registry/view slow paths, readiness checks */
#endif
VkBufferView vk_tbuf_by_id(int tid, int writable);
VkSampler vk_samp_by_id(int sid);
VkImageView vk_dummy_view(int target, int is_int);   /* zero texture of a target (unbound slots) */
VkSampler vk_dummy_sampler(void);
VkBufferView vk_dummy_tbuf(void);

/* ---------- shaders and programs (vk_shader.c) ---------- */
enum { RB_SUPPORT = 1, RB_CBUF, RB_C1, RB_DRV0, RB_DRV2, RB_SBUF, RB_SBUF_UBO, RB_UBO_PTR, RB_TEX, RB_TBUF, RB_IMG, RB_SAMP, RB_TEXSAMP, RB_ALPHA };
typedef struct {
    uint16_t binding, count;
    uint8_t kind;           /* RB_* */
    uint8_t dtype;          /* VkDescriptorType (fits: 0..10) */
    uint8_t target;         /* NVN texture target the shader declares (for dummies), image dim */
    uint8_t rw, is_int;
    int16_t reg, sreg;      /* NVN slot: uniform / storage / texture slot; sampler slot (RB_TEXSAMP: -1 = combined handle) */
    uint16_t off;           /* RB_SBUF_UBO: byte offset of the pointer in the uniform block */
    uint64_t extent;        /* complete static storage-block byte extent, including stride padding; 0 = unknown */
} vk_bind;
typedef struct vk_shader {
    char key[80];           /* variant name */
    size_t spv_bytes;       /* original seed module size, for opt-in compute diagnostics */
    int stage;              /* NVN stage 0 vertex, 1 fragment, 5 compute */
    VkShaderModule mod;
    uint32_t required_subgroup_size;
    VkDescriptorSetLayout dsl;
    int nb; vk_bind *b;
    uint8_t *c1; uint32_t c1_len;   /* the shader's own constant bank c1 (from its package) */
    uint32_t vin_mask;      /* vertex inputs read (location = NVN attribute) */
    uint8_t vin_class[32];  /* 0 float 1 uint 2 sint */
    uint32_t fout_mask;     /* fragment colour output locations declared (with TOTK_VK_UNWRITTEN_MASK, arrays span theirs) */
    uint32_t fout_span;     /* the SPIR-V Output locations, arrays spanning theirs: a live TOTK_VK_UNWRITTEN_MASK never masks them */
    int writes_depth;
    uint8_t load_position_attr; /* 1 + attribute of a certified straight-line vec4(xy,0,1) position; zero unknown */
    uint32_t load_full_outputs; /* unconditional complete fragment outputs, with no discard/control-flow */
    int early_tests;        /* original NVN control byte 0x718; honored by default (EARLYZ=0 is a diagnostic override) */
    int depth_spec;         /* fragment depth exports patched: + specialization constant D16_SPEC_ID (the DEPTH16 nudge, vk_draw.c) */
    unsigned alpha_func;    /* zero: original module; 1..7: alpha-reject fragment variant with a reference UBO */
    uint32_t local[3];      /* compute thread group size */
    uint64_t c1_sub; VkDescriptorBufferInfo c1_d;   /* vk_draw.c: this submission's copy of c1 */
    int has_storage_writes; /* reflection plus SPIR-V store marking; immutable after shader creation */
    int has_storage_reads; uint16_t uniform_prepare_mask; /* immutable draw-touch reflection summary */
    int fragcoord_xy;       /* actual xy pointer/vector access; FragCoord.w alone is scale-independent */
    uint64_t native_tex_mask[2]; uint32_t native_img_mask; /* immutable reflected image slots */
    uint8_t canvas_ui, canvas_transfer, native_1x; /* load-time classification; no per-draw string/reflection walk */
    uint8_t framegen_camera_layout; /* audited alternate camera convention; zero means unknown */
    uint8_t framegen_raster_depth; /* explicit depth export is only raster z or invalid 0/1 sentinel */
    uint8_t framegen_plane; /* audited VS view-position or FS plane-depth convention, not unconditional admission */
    uint8_t framegen_uncertified_plane; /* exact experimental VS candidate; no authority unless opt-in and runtime checks */
    uint64_t xlate_miss_ns; /* recorder clears after its first emitted draw/dispatch; not a GPU-completion timestamp */
} vk_shader;

/* ---------- opt-in per-slot GPU pass profiler (vk_gpuprof.c) ---------- */
typedef struct vk_gpuprof_slot vk_gpuprof_slot;
typedef enum {
    VK_PASS_END_OTHER, VK_PASS_END_NVN_BARRIER, VK_PASS_END_TEXTURE_BARRIER,
    VK_PASS_END_ATTACHMENTS, VK_PASS_END_CLEAR, VK_PASS_END_COPY, VK_PASS_END_QUERY,
    VK_PASS_END_COMPUTE, VK_PASS_END_CPU_WAIT, VK_PASS_END_SUBMISSION,
    VK_PASS_END_UNIFORM, VK_PASS_END_PRESENT, VK_PASS_END_DIAGNOSTIC,
    VK_PASS_END_MERGE_HAZARD, VK_PASS_END_SPLIT
} vk_pass_end_kind;
extern int vk_gpuprof_on;   /* initialized once at first slot; off hot paths are a single branch */
void vk_gpuprof_identity(const char *commit, const char *package, uint64_t version, const char *build);
void vk_gpuprof_shutdown(void); /* off: no-op; enabled: bounded renderer drain, reporter join, explicit retirement */
int vk_gpuprof_renderer_drain(uint64_t timeout_ns); /* normal-close-only bounded recorder admission/fence wait */
void vk_gpuprof_slot_begin(vk_gpuprof_slot **slot, VkCommandBuffer cb);
void vk_gpuprof_slot_seal(vk_gpuprof_slot *slot); /* advances submission sequence even for a dropped/null lease */
void vk_gpuprof_slot_complete(vk_gpuprof_slot **slot); /* fence completion: detach/enqueue only; no query reads or IO */
void vk_gpuprof_slot_pre(vk_gpuprof_slot *slot, VkCommandBuffer cb);
void vk_gpuprof_slot_end(vk_gpuprof_slot *slot, VkCommandBuffer cb);
void vk_gpuprof_slot_cancel(vk_gpuprof_slot **slot); /* discard unsubmitted lease off walker */
void vk_gpuprof_scope_begin(VkCommandBuffer cb, const char *label);
void vk_gpuprof_scope_end(VkCommandBuffer cb);
void vk_gpuprof_begin(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat formats[10],
                     const tex_t *const textures[10], const uint32_t widths[10], const uint32_t heights[10], uint32_t samples);
void vk_gpuprof_end(VkCommandBuffer cb, vk_pass_end_kind reason, uint32_t detail);
typedef struct {
    VkQueryPool pool, stats;
    uint32_t timestamp, stats_query1;
    uint32_t *timestamp_count; /* immutable retired-slot record; assembler writes before its fence */
} vk_gpuprof_end_token;
void vk_gpuprof_end_deferred(vk_pass_end_kind reason, uint32_t detail, vk_gpuprof_end_token *token);
void vk_gpuprof_end_emit(VkCommandBuffer cb, const vk_gpuprof_end_token *token);
VkQueryPipelineStatisticFlags vk_gpuprof_stats_mask(void);
void vk_gpuprof_secondary(int enabled);
int vk_gpuprof_chunk_query(VkQueryPool *pool, uint32_t *query);
void vk_gpuprof_draw(vk_shader *const shaders[6]);
void vk_gpuprof_dispatch_begin(VkCommandBuffer cb);
void vk_gpuprof_present(void); /* recording lock; CPU closure; sampled sealed-CB path submits one end-marker slot */
#ifdef TOTK_VK_TESTS
void vk_gpuprof_test_frame(VkCommandBuffer cb);
void vk_gpuprof_test_flush(void);
void vk_gpuprof_test_wait(void);
void vk_gpuprof_test_pause(int paused);
void vk_gpuprof_test_background(uint64_t counts[3]); /* enqueued, processed, dropped leases */
#endif
#define D16_SPEC_ID 9001
typedef struct vk_prog vk_prog;
/* The program's shaders for the current bind state (variant per stage): 0 ok, -1 a variant is missing (draw skipped) */
int vk_prog_resolve(const prog_t *p, vk_shader *out[6]);
void vk_stage_sig_invalidate(unsigned stages);   /* recording-thread state writes: one bit per NVN stage */
void vk_prog_prepare(const prog_t *p); /* bind-time compute compilation, no variant selection or graphics seed IO */
VkPipelineLayout vk_layout_of(vk_shader *vs, vk_shader *fs);
VkPipeline vk_compute_pipeline(vk_shader *cs, VkPipelineLayout *layout);
void vk_compute_start(void);
void vk_compute_prepare(vk_shader *cs); /* startup/replay only; immutable module/layout handed to worker */
void vk_compute_stop(void);
void vk_compute_report(void);
/* Priority lending for joins on background compiles (TOTK_VK_PIPE_BOOST=1, vk_draw.c). Graphics and compute workers
   run at VK_WORKER_NICE. A thread that must wait for a worker's in-flight build lends that worker its own nice until
   the build is published; the worker then restores itself. Lend under the queue mutex that publishes the build. */
#define VK_WORKER_NICE 10
typedef struct { int tid, lent, nice; } vk_lend;   /* tid: the worker building; lent: a waiter raised it to nice */
int vk_thread_tid(void);
int vk_pipe_boost_lend(vk_lend *l);             /* 1 raised, 0 not needed or switch off, -1 refused by the kernel */
void vk_pipe_boost_restore(const vk_lend *l);   /* the worker itself, after publishing: back to VK_WORKER_NICE */
void vk_pipe_boost_waited(uint64_t ns);         /* one join on a worker build, counted with the switch off too */
void vk_draw_switches_init(void);               /* launch-time vk_draw.c switches, read once before any worker */
int vk_unwritten_mask_on(void);                 /* TOTK_VK_UNWRITTEN_MASK: pipelines mask attachments without an output */
int vk_shaders_ready(void);
void vk_shader_seed_startup(const char *game_dir); /* freeze game-relative selection and diagnose seed before replay */
const char *vk_shader_cache_path(const char *base, char *out, size_t size); /* folder/content-isolated disk state */
vk_shader *vk_shader_seed(const char *variant, int stage, int early_tests); /* startup/replay exact seed, no guest state */
vk_shader *vk_shader_alpha(vk_shader *base, unsigned func);
void vk_pipeline_prewarm(void);   /* device/cache ready, before replay: load keys and queue the compile workers */
void vk_shader_io_take(uint64_t *n, uint64_t *ns, uint64_t *max_ns);   /* seed files read since the last call, their total and longest time */
uint64_t vk_compute_pipe_cum_ns(void); /* recording-side compute blocking only, never background build time */
uint64_t vk_compute_pipe_build_ns(void); /* diagnostic synchronous driver creation, excluding worker joins */
uint64_t vk_shader_io_cum_ns(void);                                      /* all seed file reads so far, ns */
void vk_perf_cum(uint64_t c[5]);   /* running totals: recording ns, GPU waits while recording ns, game sync waits ns, CPU steps, sync waits */
/* Launch-time diagnostic snapshots; zero when disabled. Shader libraries/translation are PC-side on Android. */
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns);
void mtl_wait_stats(uint64_t *lib_ns, uint64_t *pso_ns);
void mtl_scan_stats(uint64_t *shader_ns, uint64_t *pso_ns);

/* ---------- draw side (vk_draw.c) ---------- */
void vk_draw_exec(uint32_t op, const void *a);   /* every op the walker does not handle itself */
const uint8_t *vk_exec_draw_next(void);          /* next record only while replaying a draw; NULL outside mtl_exec */
void vk_end_rendering(void);                     /* end the render pass if one is open (transfers, submission end) */
void vk_draw_texture_read(int tid, const tex_t *texture); /* first sampled/storage read resolves an overlapping load-clear */
void vk_end_rendering_at(vk_pass_end_kind reason, uint32_t detail);
void vk_pass_merge_counters(uint64_t out[4]);   /* deferred, retained, hazard splits, avoided boundaries */
/* the bound state the shader variants are chosen by (vk_shader.c reads it) */
typedef struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][6]; float dr[2]; int has_vp; int depth_clamp, render_disabled, raster_discard;
    float blend_color[4], alpha_ref; int64_t copy_stride;
    p_scissor sc; int has_sc;
    p_set_rt rt; int has_rt;
    int tex_hi[6];
} vk_state;
extern vk_state S;
int vk_prog_predict(const vk_state *state, vk_shader *out[6]);   /* borrowed ready variants only; no loads or active-state/memo changes */
#ifdef TOTK_VK_TESTS
int vk_draw_test_pipeline_bench(vk_shader *vs, vk_shader *fs); /* ready-pipeline exact-key lookup microbenchmark */
void vk_fg_test_terminal_geometry_freeze(int enabled); /* default on; 0 reproduces pre-freeze owned pool geometry */
typedef struct {
    uint64_t frame, used_ns, sync_ns, scans;
    unsigned ops, keys, stop, scans_in_sub;
    uint64_t fast_links, fast_draws, fast_link_ns, fast_link_max_ns;
    uint64_t optimized_replacements, optimized_ns, library_skips;
    uint64_t parts[4], part_ns[4];
} vk_pipeline_test_stats;
void vk_pipeline_test_stats_get(vk_pipeline_test_stats *out);
void vk_pipeline_test_predict(const uint8_t *ops);   /* same bounded production entry, explicit test cursor */
#endif

/* texture-pool registry (vk_res.c) */
typedef struct { tex_t t; int ok; view_t v; int has_v; } vk_treg;
int vk_treg_get(int tid, vk_treg *out);      /* a copy, taken under the registry lock (the game registers from its own threads) */
int vk_treg_kind(int tid, int *format, int *target);   /* effective format/target, atomically published by registry writers */
uint64_t vk_treg_revision(void);                    /* acquire: every registration, texture epoch or pool-size change invalidates stage signatures */

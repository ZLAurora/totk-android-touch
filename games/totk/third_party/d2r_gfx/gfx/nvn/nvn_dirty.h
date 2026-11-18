// 客体内存写保护脏页跟踪（nvn_dirty.c）
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // 登记一个内存池为跟踪区间（首次调用时注册缺页钩子与宿主写前钩子） | Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // 在册的内存池数、峰值、跟踪表容量（内存读数行用） | number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // 池销毁：受保护页还原可写并标失效 | Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // 覆盖页设只读 | Set the covered pages read-only
void nvn_dirty_protect_exact(const void *addr, size_t len);   // TOTK: page by page even with protection units (renderings waiting for write-back)
/* Storage-wide exact watch for a GPU rendering. The first CPU fault/pre-write cancels the whole watch without
   publishing/waiting any WB GPU ticket; independent real buffer-reader guards retain their normal safety waits.
   Stable bounded metadata; NULL means untracked/exhausted. Caller samples dirty_now BEFORE arming and owns the
   handle until unwatch, including after written becomes true. No callback into the backend from a signal.
   Unwatch removes exact exclusions, not independent texture/lag protection. Re-arm only after a new GPU write. */
void *nvn_dirty_wb_watch(const void *addr, size_t len, uint64_t stamp);
int nvn_dirty_wb_written(const void *watch);
uintptr_t nvn_dirty_wb_cpu_address(const void *watch); /* first observed CPU fault/pre-write address; 0 if unavailable */
void nvn_dirty_wb_unwatch(void *watch);
void nvn_dirty_wb_stats(uint64_t *faults, uint64_t *preps); /* cumulative storage-wide CPU fault/pre-write disarms */
/* Vulkan lag guard: enable before pool registration; guard a read until its explicit GPU ticket completes.
   Completion publication is independent of CPU post-op delivery. Fault/pre-write waits use no locks or Vulkan calls.
   Protection is sticky per dirty unit: an already read-only unit only raises its newest ticket; duplicates do no gate
   CAS or global-stat update. register/protect/bounds/registered return 0 for unsupported or untracked ranges;
   register/protect/registered also reject tickets >= 2^62 and empty ranges.
   Workers may concurrently register frozen ranges, including overlapping ranges, without touching backend state.
   The caller retains each pool's lifetime and immutable tracking identity until all workers and guarded reads finish;
   range removal/reuse must not race these calls. No ownership is transferred to the tracker.
   A future ticket may be registered before QueueSubmit, but no CPU write (including completion stores/write-back)
   to its guarded units may be needed before that ticket is submitted: such a write waits for completion and can
   deadlock submission. Join all registrar workers before QueueSubmit and keep guarded units frozen until submit. */
enum {
    NVN_DIRTY_LAG_READONLY = 1, NVN_DIRTY_LAG_COLD = 2, NVN_DIRTY_LAG_OVER_CAP = 4,
    NVN_DIRTY_LAG_GPU = 8, NVN_DIRTY_LAG_WRITABLE = 16, NVN_DIRTY_LAG_ALIAS = 32
};
void nvn_dirty_lag_enable(void);
int nvn_dirty_lag_enabled(void);   /* set once by nvn_dirty_lag_enable before pool registration */
/* Linux VMA guard budget. Explicit refresh/report and drain are normal-thread-only, off the recurring
   frame/recording paths. Request is non-blocking with respect to scans; pressure is an acquire-load.
   Pressure is latched until restart: no new additional lag/private-copy watches; baseline texture
   and exact GPU write-back watches remain active. Affected pre-cutover tickets MUST queue then wait
   before returning. After draining at a safe NVN boundary, the backend may disable lag capture and
   restore raw frame-end waits; future lag-off slots need no guard failure/per-slot GPU wait.
   Drain only after the ordered failed slot's real GPU fence completed and no registrar owner remains. */
void nvn_dirty_lag_vma_refresh(void);
void nvn_dirty_lag_vma_request(int classes); /* mark next period: epoch change (0), optional classification (1) */
int nvn_dirty_lag_vma_pressure(void);
void nvn_dirty_lag_vma_drain(void);
void nvn_dirty_lag_vma_stats(uint64_t values[5]); /* last sampled maps/max, active reserved runs/limit, pressure */
/* Completed worker scans only: count, epoch-requested subset, total/max duration in ns.
   Optional reset returns one coherent reporting interval; no sampler/procfs mutex is taken. */
void nvn_dirty_lag_vma_scan_stats(uint64_t values[4], int reset);
/* Synchronous allocation-bounded classification: explicit diagnostics/worker only, never frame/fault/guard paths. */
void nvn_dirty_lag_vma_report(void);
#ifdef TOTK_VK_TESTS
void nvn_dirty_lag_test_vma_limit(uint64_t limit);
void nvn_dirty_lag_test_vma_sample(uint64_t maps, uint64_t maximum); /* serialized with background samples */
uint64_t nvn_dirty_lag_test_vma_reads(void); /* calling thread only, includes explicit classification */
unsigned nvn_dirty_lag_test_vma_period(const char *value); /* same parser/clamp used at worker startup */
#endif
/* Immutable caller-owned literal; NULL restores unknown/guest. Getter returns the borrowed current role.
   TLS pointer publication is lock-free and allocation-free, including when lag guards are disabled. */
void nvn_dirty_lag_thread_role(const char *role);
const char *nvn_dirty_lag_thread_role_get(void);
/* reason is an OR of the bits above; range_size is the original descriptor's byte size, not its rounded unit span.
   Registration atomically ORs reasons and raises the maximum descriptor size for each successfully guarded unit.
   register/protect can also return 0 after partial registration on a permission failure or, with a nonzero fence,
   any BUSY unit/page, even if its sampled ticket is complete. Ownership can change before a waiter wakes.
   Submit all registered tickets before waiting, retrying or doing any CPU-copy fallback on their units. */
int nvn_dirty_lag_register(const void *addr, size_t len, uint64_t fence, unsigned reason, size_t range_size);
/* Collection never protects memory or changes hot history. Zero initialization is sufficient; storage is allocated
   only with lag enabled and retained by reset. add requires one fully tracked span; overlapping units deduplicate
   by retained range-slot generation and range-relative index, OR reasons and retain the largest descriptor size.
   add/merge return 0 on invalid/untracked/stale input or allocation failure and poison the batch until reset.
   merge copies retained unit references, without resolving addresses or rescanning pools. Source is unchanged.
   After all collection/merges and command recording finish, register the immutable final batch before QueueSubmit.
   Shards own disjoint contiguous dense index intervals; no batch mutation/reset/destruction may race registration.
   register returns 0 for a poisoned/stale batch, zero/invalid ticket/shard or any arming failure, including BUSY with a
   completed sampled ticket. Partial arming is possible: join every shard and queue before failure fallback/waits.
   Empty valid batches succeed unless VMA pressure has latched. The existing pool lifetime contract still applies; generation checks reject
   sequential remove/re-add, not concurrent destruction. bytes sums unique whole-unit extents, clipped at pool tails. */
typedef struct nvn_dirty_lag_batch { void *impl; } nvn_dirty_lag_batch;
int nvn_dirty_lag_batch_add(nvn_dirty_lag_batch *batch, const void *addr, size_t len, unsigned reason, size_t max_range);
/* Capture an epoch alongside frozen spans before dispatching collection. add_before rejects an incarnation born
   after that snapshot (including same-VA recycling before the first add), without invalidating older ranges when
   unrelated mappings change. Snapshot is one acquire load, with no scan/allocation; it includes only published
   live births and also advances on removal, so normal-thread lookup memos can reject changed lifetimes.
   Existing pool/alias lifetime synchronization still applies during the snapshot and subsequent use.
   Plain add is add_before with UINT64_MAX, retaining the incarnation current at collection instead of freezing it. */
uint64_t nvn_dirty_lag_epoch(void);
int nvn_dirty_lag_batch_add_before(nvn_dirty_lag_batch *batch, const void *addr, size_t len, unsigned reason,
                                   size_t max_range, uint64_t epoch);
int nvn_dirty_lag_batch_merge(nvn_dirty_lag_batch *dst, const nvn_dirty_lag_batch *src);
int nvn_dirty_lag_batch_register(const nvn_dirty_lag_batch *batch, uint64_t ticket, unsigned shard, unsigned nshards);
int nvn_dirty_lag_batch_seal(nvn_dirty_lag_batch *batch); /* owner-only sort/rehash before immutable shard publication */
void nvn_dirty_lag_batch_reset(nvn_dirty_lag_batch *batch);
void nvn_dirty_lag_batch_destroy(nvn_dirty_lag_batch *batch);
size_t nvn_dirty_lag_batch_count(const nvn_dirty_lag_batch *batch);
uint64_t nvn_dirty_lag_batch_bytes(const nvn_dirty_lag_batch *batch);
int nvn_dirty_lag_protect(const void *addr, size_t len, uint64_t fence);   // register with reason=0, range_size=0
/* Acquire-load-only snapshot: 1 iff every covered unit is ARMED with ticket >= fence; no waits, cache updates,
   diagnostics or other side effects. Does not reserve the range against a subsequent CPU writer. */
int nvn_dirty_lag_registered(const void *addr, size_t len, uint64_t fence);
int nvn_dirty_lag_bounds(const void *addr, size_t len, uintptr_t *lo, uintptr_t *hi);
/* Diagnostic submission watermark only: call immediately after a successful vkQueueSubmit2, before publishing the
   queue entry. Atomic-max ignores older tickets; lag-off returns without touching the watermark.
   Ticket/gate waits >=500 ms emit one async-signal-safe stderr record per wait episode, without ending the wait:
   kind/origin, waiting TID, TLS role, kernel thread name (unknown where unsupported), elapsed ms, exact tracked
   unit VA+extent, ticket, GPUdone, latest submitted ticket/state, and current gate ticket/state.
   ticket_state distinguishes complete/submitted/unsubmitted waits. */
void nvn_dirty_lag_submitted(uint64_t ticket);
void nvn_dirty_lag_complete(uint64_t fence);
void nvn_dirty_lag_stats(uint64_t *fault_waits, uint64_t *host_waits, uint64_t *wait_ns);
void nvn_dirty_lag_frame(uint64_t frame);   // monotonic present frame, initially 1; old frame values are ignored
uint64_t nvn_dirty_lag_frame_now(void);   // acquire snapshot for hot-classification memos; pair with dirty_now for write history
int nvn_dirty_lag_hot(const void *addr, size_t len);   // any overlapping unit: writes in >=2 distinct rolling-8 frames; cold after 8 quiet frames
void nvn_dirty_lag_watch(const void *addr, size_t len);   // readonly write-watch; keeps the existing ticket, never extends it
void nvn_dirty_lag_sticky_stats(uint64_t *rearms, uint64_t *ticket_updates, uint64_t *promotions, uint64_t *demotions);   // cumulative; ticket_updates counts only ARMED ticket increases
typedef struct { uintptr_t addr; size_t len; uint64_t faults, waits, last_frame; int hot; unsigned reasons; size_t max_range; } nvn_dirty_lag_unit;
/* Top writes since the previous call, descending by count (faults includes host pre-write unprotections).
   Only units with writes or waits produce rows. Resets counts, reason bits and maximum descriptor size for all scanned
   live units, including those without writes/waits, but not their hot history. Normal thread only, caller owns out.
   History and interval diagnostics are per tracked virtual unit: aliases do not share them with their source. */
int nvn_dirty_lag_top(nvn_dirty_lag_unit *out, int max);
/* Adaptive lag protection. A unit is flagged once a CPU write there waited for in-flight GPU reads, or once it was
   promoted hot; flags live until the pool slot is reused. marks: cumulative flag count (signal-safe, changes whenever
   the set grows). collect: normal thread only; writes merged [lo, hi) runs up to max and returns the total run count
   (more than max: the caller's arrays were too small). stats: cumulative flags by cause. */
uint64_t nvn_dirty_lag_danger_marks(void);
int nvn_dirty_lag_danger_collect(uintptr_t *lo, uintptr_t *hi, int max);
void nvn_dirty_lag_danger_stats(uint64_t *waits, uint64_t *hots);
uint64_t nvn_dirty_now(void);                                      // 当前代号：上传前取，作为该份内容的戳 | Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp);
void *nvn_dirty_gpu_write_begin(void *addr, size_t len);   // write guest memory on behalf of the GPU: not counted as a CPU write
void nvn_dirty_gpu_write_end(void *tok);
/* Same ownership, but never wait for a GPU ticket or another page/unit owner. Zero means retry later. */
int nvn_dirty_gpu_write_try_begin(void *addr, size_t len, void **token);
/* Explicit private-alias ownership: the caller retains a writable alias of the SAME physical bytes plus
   backing/range lifetime through end, and performs stores through that alias, never the guest pointer.
   These use the same tickets, BUSY claims, try/unwind and generation semantics as the ordinary APIs, but
   keep guest permissions unchanged (including under VMA pressure), with no GPU-write/restore mprotect stats.
   The existing nvn_dirty_gpu_write_end accepts either token kind; no existing begin chooses alias mode. */
void *nvn_dirty_gpu_write_alias_begin(void *addr, size_t len);
int nvn_dirty_gpu_write_alias_try_begin(void *addr, size_t len, void **token);
/* Emulated GPU completion stores: retain unit ownership across the actual host store, without allocation.
   Return 0 on a permission-change failure. Unlike CPU writes, these keep dirty generations/hot history unchanged. */
int nvn_dirty_lag_store(void *addr, const void *data, size_t len);
int nvn_dirty_lag_store32(volatile uint32_t *addr, uint32_t value, int add);
/* Completion store through caller-proven private backing. guest identifies dirty/lag ownership; alias names
   the same physical bytes and must remain writable/live throughout. Retains guest PG_BUSY ownership even
   for lag-off baseline/exact watches; untracked bytes are copied directly. No backend callbacks or locks,
   allocation, guest mprotect or CPU-generation changes. add32 uses one aligned 32-bit word (len == 4), just
   like store32; otherwise copies len bytes. Existing store APIs always write the guest mapping. */
int nvn_dirty_lag_store_alias(void *guest, void *alias, const void *data, size_t len, int add32);
typedef struct { uintptr_t page; uint64_t pc, lr; uint32_t src; } nvn_dirty_rec;   // src 0: guest write (fault), 1: host write
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max);   // latest unprotections in [lo, hi) (DM_NVN_TILELOG) // 覆盖页在戳之后被写过 → 1 | Covered pages were written after the stamp -> 1
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累计：游戏写触发的缺页次数、宿主写前解保护的页数 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累计：本模块 mprotect 调用次数、覆盖页数、耗时 | Cumulative: this module's mprotect call count, pages covered, time spent
/* Actual syscalls (including failures), not descriptor/page-state probes. Signal-safe producers use
   bounded per-thread counters; normal-thread snapshots are cumulative, approximate at interval edges. */
typedef enum {
    NVN_DIRTY_MP_GUARD, NVN_DIRTY_MP_SNAPSHOT, NVN_DIRTY_MP_TEXTURE, NVN_DIRTY_MP_EXACT,
    NVN_DIRTY_MP_FAULT, NVN_DIRTY_MP_PREPARE, NVN_DIRTY_MP_GPU_WRITE, NVN_DIRTY_MP_RESTORE,
    NVN_DIRTY_MP_DRAIN, NVN_DIRTY_MP_VM_COMMIT, NVN_DIRTY_MP_VM_DECOMMIT, NVN_DIRTY_MP_STACK,
    NVN_DIRTY_MP_MODULE, NVN_DIRTY_MP_DEBUG_WATCH, NVN_DIRTY_MP_REASON_COUNT
} nvn_dirty_mp_reason;
typedef struct { uint64_t calls, pages, ns; } nvn_dirty_mp_count;
void nvn_dirty_mp_reason_stats(nvn_dirty_mp_count out[NVN_DIRTY_MP_REASON_COUNT]);
void nvn_dirty_mp_report(uint64_t frame); /* existing 300-frame reporter; never called in a signal */
/* Raw libc-compatible result/errno, including before tracker initialization. Does not request guard
   pressure fallback on failure: runtime allocation/decommit keeps its own original error handling. */
int nvn_dirty_mprotect(void *address, size_t size, int protection, nvn_dirty_mp_reason reason);
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 当前存活槽、槽表高水位、登记总次数 | Currently live slots, slot table high-water mark, total number of registrations
#ifdef TOTK_VK_TESTS
#include <stdatomic.h>
/* Fixture-owned storage remains live until all matching registration calls have joined and the hook is unset. */
typedef struct {
    const void *addr; uint64_t fence, busy_ticket, completed_ticket;
    _Atomic int busy_seen, release, rejected;
} nvn_dirty_lag_test_handoff;
void nvn_dirty_lag_test_handoff_set(nvn_dirty_lag_test_handoff *handoff);
/* Deterministic write-back race fixture. Only the first PROTECTED handoff is inside the dirty token; the other
   phases bracket the real Vulkan landing's freshness check/restoration. No callbacks/locks in the fault hook. */
enum { NVN_DIRTY_TEST_WB_BEFORE_OWNERSHIP = 1, NVN_DIRTY_TEST_WB_PROTECTED,
    NVN_DIRTY_TEST_WB_BEFORE_FRESHNESS, NVN_DIRTY_TEST_WB_AFTER_FRESHNESS, NVN_DIRTY_TEST_WB_RESTORED };
typedef struct {
    const void *addr; size_t size; int phase;
    _Atomic int arrived, release, faults;
} nvn_dirty_test_wb_handoff;
void nvn_dirty_test_wb_set(nvn_dirty_test_wb_handoff *handoff);
void nvn_dirty_test_wb_phase(const void *addr, int phase);
#endif

// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/futex.h>
#include <limits.h>
#include <sys/prctl.h>
#endif

// 页状态与 mprotect 的次序（改保护的一方先把状态改成 BUSY，mprotect 落地后才改成终态；别的线程见 BUSY 就等）：
//   设只读：NONE → BUSY → mprotect(只读) → PROT；解保护：PROT → BUSY → mprotect(可写) → NONE。
// 不变式：状态是 NONE 的页一定可写、是 PROT 的页一定只读。原先设只读是「先记 PROT、攒一段再 mprotect」，这段时间里另一线程的写前解保护
// 会把状态改回 NONE，随后攒批的 mprotect 落地 ⇒ 页只读而状态 NONE：游戏线程写它时缺页处理只回「重执行」永远空转；
// 宿主用系统调用往里读文件时 prep_write 看状态 NONE 跳过，系统调用不走缺页处理，直接 EFAULT、数据读不全。
// BUSY 最长持续 = 一次 mprotect 的时间（设只读时再加上给这一段逐页改状态的几次原子写）。等待有上限 BUSY_WAIT_MS：
// 缺页处理里超限按「不是我们的页」返回，交给崩溃处理如实报崩，不静默空转。等待只读原子量、不拿任何锁。
// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus the atomic page-state updates). The wait has an upper limit BUSY_WAIT_MS:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
/* A private-alias GPU store also owns PG_BUSY, but leaves the guest mapping read-only throughout:
   PG_PROT -> PG_BUSY -> PG_PROT without permission changes or CPU-generation publication. */
enum { WATCH_EXACT = 1, WATCH_TEXTURE = 2 }; /* retained in the existing per-page exact byte */
enum { BUSY_WAIT_MS = 2000 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 16384 // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing. TOTK (2026-10-17): 4096 was full in the user's own games and most long test games (about 3,900 live pools in the world, more when moving between regions); now 16384
/* The unit gate is also its newest GPU-read ticket. ARMED certifies that EVERY page is read-only; NONE makes no
   protection claim. BUSY excludes ticket publication until a writer/protector has finished changing permissions.
   Exact-page protection still lives in the page tables; a partly writable unit must never be published ARMED. */
enum { LAG_NONE = 0, LAG_ARMED = 1, LAG_BUSY = 2, LAG_STATE_MASK = 3 };
#define LAG_MAX_TICKET (UINT64_MAX >> 2)
typedef struct {
    _Atomic uint64_t gate;
    _Atomic uint32_t wake, waiters;
    _Atomic uint64_t last_frame, hot_frame, faults, waits;
    _Atomic unsigned reasons;
    _Atomic uint8_t danger; /* adaptive protection: a write here waited for GPU reads, or the unit was promoted hot */
    _Atomic size_t max_range;
    uintptr_t regions; /* charged RO runs plus temporary split credit; owned with the unit gate */
} lag_unit_t;
/* exact is a watch-bit byte: exact GPU write-back pages and baseline texture dirty watches.
   Extra lag/private-copy protections carry neither bit and can be drained without losing baseline authority. */
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; _Atomic uint8_t *exact; lag_unit_t *lag; size_t cap; _Atomic int live; _Atomic uint64_t latest, identity; } range_t;   // latest: highest page generation; identity: globally unique slot incarnation birth
static range_t g_r[MAX_RANGE];
/* Write-back watches are storage ranges, not GPU readers: they never publish a lag ticket. A first CPU
   write disarms the WHOLE rendering, so exact-page exclusions cannot turn later CPU frames into fault storms.
   Stable bounded slots let the signal handler claim a range using atomics only; no callback, lock or allocation.
   A retired slot stays reserved until an already-running fault has finished. All lookup fields are atomic to
   allow a stale scanner to revalidate after a slot was reused. No new per-guest-page metadata is needed. */
enum { WB_FREE, WB_ARMED, WB_CPU, WB_FAULT, WB_RETIRED, WB_INIT };
typedef struct {
    _Atomic unsigned state;
    _Atomic int arming;
    _Atomic(range_t *) range;
    _Atomic uint64_t identity;
    _Atomic uintptr_t first, last;
    _Atomic uintptr_t cpu_address; /* first fault/pre-write byte; zero when only a pre-publication generation is known */
} wb_watch;
enum { WB_WATCH_MAX = 4096 };
static wb_watch g_wb_watch[WB_WATCH_MAX];
static _Atomic unsigned g_wb_watch_used;
static _Atomic uint64_t g_wb_faults, g_wb_preps;
static _Atomic int g_nr;
/* Slot publication is serialized by g_slot_mu. Advance after live publication or removal: an acquire snapshot
   must never authorize a range incarnation that has not yet become live. Unrelated changes do not stale older refs. */
static _Atomic uint64_t g_range_epoch;
uint64_t nvn_dirty_lag_epoch(void) { return atomic_load_explicit(&g_range_epoch, memory_order_acquire); }
/* Sorted index of live g_r slots by base, for find_range. Two buffers: under g_slot_mu, an add/remove copies the
   published order into the other buffer with one slot inserted/removed (O(n), no sort), then flips g_rix_cur.
   g_rix_seq is odd while that happens. Readers, including the fault handler, take no lock: a hit is validated
   (live + bounds), so a buffer rewritten under a slow reader can only produce a rejected slot or a miss; a miss
   is trusted only if g_rix_seq did not move during the search, else the caller falls back to the linear scan.
   Live ranges are disjoint (nvn_dirty_add_range rejects overlaps), so the validated hit is the scan's answer.
   ~3,900 live pools made the old hint-miss scan the largest single cost in the Thor walker's lag lookups. */
static int32_t g_rix[2][MAX_RANGE];
static _Atomic int g_rix_n[2], g_rix_cur;
static _Atomic uint64_t g_rix_seq;
static void rix_update(int slot, int add) {   /* caller holds g_slot_mu; the slot's base is set */
    int cur = atomic_load_explicit(&g_rix_cur, memory_order_relaxed), next = !cur, m = 0, placed = !add;
    int n = atomic_load_explicit(&g_rix_n[cur], memory_order_relaxed);
    uintptr_t base = g_r[slot].base;
    atomic_fetch_add_explicit(&g_rix_seq, 1, memory_order_acq_rel);   /* odd: rebuild in progress */
    for (int i = 0; i < n; i++) {
        int32_t s = g_rix[cur][i];
        if (s == slot) continue;
        if (!placed && g_r[s].base > base) { __atomic_store_n(&g_rix[next][m++], slot, __ATOMIC_RELAXED); placed = 1; }
        __atomic_store_n(&g_rix[next][m++], s, __ATOMIC_RELAXED);
    }
    if (!placed) __atomic_store_n(&g_rix[next][m++], slot, __ATOMIC_RELAXED);
    atomic_store_explicit(&g_rix_n[next], m, memory_order_release);
    atomic_store_explicit(&g_rix_cur, next, memory_order_release);
    atomic_fetch_add_explicit(&g_rix_seq, 1, memory_order_release);   /* even: published */
}
/* Lock-free containment test for lookups that may race with slot reuse: live (acquire) plus relaxed-atomic
   bounds, matching nvn_dirty_add_range's atomic base/end stores. */
static inline int range_has(range_t *r, uintptr_t a) {
    return atomic_load_explicit(&r->live, memory_order_acquire) && a >= __atomic_load_n(&r->base, __ATOMIC_RELAXED) &&
           a < __atomic_load_n(&r->end, __ATOMIC_RELAXED);
}
/* Returns the live range containing a, or NULL with *trusted set when the index was stable during the search. */
static range_t *rix_find(uintptr_t a, int *trusted) {
    *trusted = 0;
    uint64_t seq = atomic_load_explicit(&g_rix_seq, memory_order_acquire);
    if (seq & 1) return NULL;
    int cur = atomic_load_explicit(&g_rix_cur, memory_order_acquire);
    int lo = 0, hi = atomic_load_explicit(&g_rix_n[cur], memory_order_acquire);
    if (hi < 0 || hi > MAX_RANGE) return NULL;
    while (lo < hi) {
        int mid = (lo + hi) >> 1; int32_t s = __atomic_load_n(&g_rix[cur][mid], __ATOMIC_RELAXED);
        /* Unvalidated read: a stale buffer may name a slot being re-initialised (relaxed atomic; validated below). */
        if ((uint32_t)s < MAX_RANGE && __atomic_load_n(&g_r[s].base, __ATOMIC_RELAXED) <= a) lo = mid + 1; else hi = mid;
    }
    if (lo) {
        int32_t s = __atomic_load_n(&g_rix[cur][lo - 1], __ATOMIC_RELAXED);
        if ((uint32_t)s < MAX_RANGE) {
            range_t *r = &g_r[s];
            if (range_has(r, a)) return r;
        }
    }
    atomic_thread_fence(memory_order_acquire);
    *trusted = atomic_load_explicit(&g_rix_seq, memory_order_relaxed) == seq;
    return NULL;
}
static uintptr_t g_pg, g_shift;
static inline uintptr_t unit_addr(const range_t *r, uintptr_t p) { return r->base + (p << g_shift); }
static inline size_t unit_len(const range_t *r, uintptr_t p, uintptr_t n) {
    uintptr_t a = unit_addr(r, p), e = a + (n << g_shift); return (e > r->end ? r->end : e) - a;
}
/* Protection unit (pages). Linux keeps one mapping per run of pages with the same protection, and a process may have
   only vm.max_map_count of them (65530 on Android): page-sized protect / unprotect over the world's pools reached 92,673
   mappings on the AYN Thor (2026-10-21). Protect whole units, but keep per-system-page state so a write-back's exact
   pages can be excluded. Linux / Android default to 64 KiB (TOTK_DIRTY_UNIT_KB); TOTK_DIRTY_UNIT overrides in pages.
   A write to one page makes its non-exact neighbours writable and new. Darwin defaults to single pages.
   Pages protected with nvn_dirty_protect_exact (GPU renderings waiting for write-back, vk_res.c) are left out of their
   unit: they fault on their own, so a write next to such a texture does not count as a write to it (that skipped the
   write-back of small textures sharing a unit with CPU-written memory). Their number stays small. */
static uintptr_t g_unit = 1;
static _Atomic int g_lag_enabled;
static void vma_pressure(void);
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
enum { MP_THREADS = 512 };
typedef struct {
    _Atomic int live;
    uint64_t tid; char name[16];
    struct { _Atomic uint64_t calls, pages, ticks; } reason[NVN_DIRTY_MP_REASON_COUNT];
} mp_thread;
static mp_thread g_mp_threads[MP_THREADS];
static _Atomic unsigned g_mp_threads_used;
static __thread unsigned g_mp_thread_slot;
static mp_thread *mp_thread_get(void) {
    if (g_mp_thread_slot) return &g_mp_threads[g_mp_thread_slot - 1];
    unsigned slot = atomic_fetch_add_explicit(&g_mp_threads_used, 1, memory_order_relaxed);
    if (slot >= MP_THREADS - 1) slot = MP_THREADS - 1;
    mp_thread *t = &g_mp_threads[slot];
    if (slot != MP_THREADS - 1) {
#ifdef __linux__
        t->tid = (uint64_t)syscall(SYS_gettid);
        if (syscall(SYS_prctl, PR_GET_NAME, t->name, 0, 0, 0) != 0) t->name[0] = 0;
#elif defined(SYS_thread_selfid)
        t->tid = (uint64_t)syscall(SYS_thread_selfid);
#endif
        t->name[15] = 0;
        atomic_store_explicit(&t->live, 1, memory_order_release);
    }
    g_mp_thread_slot = slot + 1;
    return t;
}
static void mp_detail(nvn_dirty_mp_reason reason, uint64_t pages, uint64_t ticks) {
    mp_thread *t = mp_thread_get();
    atomic_fetch_add_explicit(&t->reason[reason].calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->reason[reason].pages, pages, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->reason[reason].ticks, ticks, memory_order_relaxed);
}
typedef struct { uint64_t calls, pages, ticks; } mp_stats_t;
static void mp_account(uint64_t pages, uint64_t ticks, mp_stats_t *stats, nvn_dirty_mp_reason reason) {
    mp_detail(reason, pages, ticks);
    if (stats) { stats->ticks += ticks; stats->calls++; stats->pages += pages; }
    else {
        atomic_fetch_add(&g_mp_ticks, ticks); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, pages);
    }
}
static int mp_counted(uintptr_t a, size_t len, int prot, mp_stats_t *stats, nvn_dirty_mp_reason reason) {
    uint64_t t = mach_absolute_time(); int ok = mprotect((void *)a, len, prot) == 0;
    if (!ok && errno == ENOMEM && g_lag_enabled) vma_pressure();
    uint64_t ticks = mach_absolute_time() - t;
    mp_account(len >> g_shift, ticks, stats, reason);
    return ok;
}
int nvn_dirty_mprotect(void *address, size_t size, int protection, nvn_dirty_mp_reason reason) {
    static _Atomic size_t system_page;
    int incoming_errno = errno; uint64_t start = mach_absolute_time();
    errno = incoming_errno;
    int result = mprotect(address, size, protection), saved_errno = errno;
    uint64_t ticks = mach_absolute_time() - start;
    /* Module sealing initializes this before diagnostic watches can fault; no dirty-pool initialization
       or page-state mutation is needed for runtime permission accounting. Concurrent cold calls agree. */
    size_t page = atomic_load_explicit(&system_page, memory_order_relaxed);
    if (!page) { page = (size_t)getpagesize(); atomic_store_explicit(&system_page, page, memory_order_relaxed); }
    mp_account(size / page + (size % page != 0), ticks, NULL, reason);
    errno = saved_errno;
    return result;
}
static int mp(uintptr_t a, size_t len, int prot, nvn_dirty_mp_reason reason) {
    int ok = mp_counted(a, len, prot, NULL, reason);
    if (!ok && prot == (PROT_READ | PROT_WRITE)) {
        /* Never publish PG_NONE while the kernel still denies stores. This is also signal-safe. */
        static const char error[] = "[dirty-lag] mprotect writable restoration failed; refusing an invalid page-state publication\n";
        ssize_t written = write(STDERR_FILENO, error, sizeof error - 1); (void)written; _exit(1);
    }
    return ok;
}

static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
/* Lag guards keep GPU reads in the original pool. A CPU writer reusing an in-flight page waits for completion before
   making it writable. SIGSEGV shares these waits: diagnostics use lock-free atomics, a monotonic clock, stack-only
   integer formatting and raw write/syscall, never stdio, pthread or allocator calls. Timed futex waits report a stuck
   ticket/gate after 500 ms once per wait episode; reporting never releases ownership or abandons the wait. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2 &&
               ATOMIC_POINTER_LOCK_FREE == 2, "Lag waits require lock-free ticket, gate, wake and role atomics");
enum { LAG_WAIT_REPORT_NS = 500000000 };
static _Atomic uint64_t g_lag_done, g_lag_fault_waits, g_lag_host_waits, g_lag_wait_ticks;
static _Atomic uint64_t g_lag_submitted;
static _Atomic uint64_t g_lag_frame = 1, g_lag_rearms, g_lag_ticket_updates, g_lag_promotions, g_lag_demotions;
static _Atomic uint32_t g_lag_wake;
/* Adaptive lag protection: marks bump this signal-safe counter; the walking thread rebuilds its unit set from the
   per-unit flags (nvn_dirty_lag_danger_collect), so destroyed pools drop out and slot reuse starts unflagged. */
static _Atomic uint64_t g_danger_marks, g_danger_waits, g_danger_hots;
static void lag_danger(lag_unit_t *u, int waited) {
    if (atomic_exchange_explicit(&u->danger, 1, memory_order_relaxed)) return;
    atomic_fetch_add_explicit(waited ? &g_danger_waits : &g_danger_hots, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_danger_marks, 1, memory_order_release);
}
/* Each unit's RO runs upper-bound its contribution even when adjacent units kernel-merge.
   Writers reserve their worst-case split count too. No proc reads, allocation or mutex in the fault path. */
static _Atomic uint64_t g_vma_regions, g_vma_limit = UINT64_MAX, g_vma_count, g_vma_max;
static _Atomic int g_vma_pressure, g_vma_reported, g_vma_drained;
static _Atomic int g_vma_assumed_max, g_vma_missing_maps;
static int g_vma_missing_reported;
#ifdef TOTK_VK_TESTS
static _Atomic uint64_t g_vma_test_limit = UINT64_MAX;
static uint64_t g_vma_test_maps, g_vma_test_max;
static int g_vma_test_sample;
static _Thread_local uint64_t g_vma_test_reads;
#endif
static pthread_mutex_t g_vma_mu = PTHREAD_MUTEX_INITIALIZER;
/* Requests never take g_vma_mu: a scan (or classification) may hold it for hundreds of milliseconds.
   The detached worker owns only process-lifetime static storage; process exit tears it down, with no
   destructor/join that could race the backend's teardown. No request originates in a signal handler. */
static pthread_once_t g_vma_once = PTHREAD_ONCE_INIT;
enum { VMA_SCAN_DEFAULT_MS = 10000, VMA_SCAN_MIN_MS = 1000, VMA_SCAN_MAX_MS = 30000 };
/* Linux 5.15 proc maps holds mmap_read_lock while seq_file fills one read request, not during
   userspace parsing. Bound each fill rather than lengthening the interval/staling the VMA budget. */
enum { VMA_MAPS_READ_BYTES = 1024 };
static unsigned g_vma_period_ms = VMA_SCAN_DEFAULT_MS;
/* Telemetry only: neither holder scans procfs or takes g_vma_mu/g_slot_mu. Reports can copy/reset
   a coherent interval without ever waiting for a scan; the worker holds this for four integer updates. */
static pthread_mutex_t g_vma_scan_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_vma_scan_stats[4]; /* completed scans, epoch-requested subset, total ns, max ns */
static _Atomic unsigned g_vma_requests;
static _Atomic uint64_t g_vma_epoch = UINT64_MAX;
enum { VMA_EPOCH = 1, VMA_CLASSES = 2 };
static void vma_start(void);
static uint64_t vma_read(const char *path, int lines) {
#ifdef TOTK_VK_TESTS
    g_vma_test_reads++;
#endif
#ifdef __linux__
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    char bytes[VMA_MAPS_READ_BYTES]; uint64_t value = 0; ssize_t n;
    while ((n = read(fd, bytes, sizeof bytes)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; value = 0; break; }
        for (ssize_t i = 0; i < n; i++) {
            if (lines) value += bytes[i] == '\n';
            else if (bytes[i] >= '0' && bytes[i] <= '9') value = value * 10 + (unsigned)(bytes[i] - '0');
        }
    }
    close(fd); return value;
#else
    (void)path; (void)lines; return 0;
#endif
}
static void vma_pressure(void) {
    if (!atomic_exchange_explicit(&g_vma_pressure, 1, memory_order_acq_rel))
        atomic_fetch_add_explicit(&g_now, 1, memory_order_release); /* revoke every native write-watch memo */
}
/* Caller owns g_vma_mu. Classification reuses its maps traversal for this budget sample. */
static void vma_sample(uint64_t sampled_maps, uint64_t sampled_max) {
#ifdef TOTK_VK_TESTS
    if (g_vma_test_sample) { sampled_maps = g_vma_test_maps; sampled_max = g_vma_test_max; }
#endif
    uint64_t maximum = sampled_max ? sampled_max : atomic_load(&g_vma_max);
#ifdef __linux__
    if (!maximum) maximum = 65530; /* App policy can hide this sysctl while maps remains readable. */
#endif
    atomic_store(&g_vma_assumed_max, !sampled_max); atomic_store(&g_vma_missing_maps, !sampled_maps);
    atomic_store(&g_vma_max, maximum);
    uint64_t maps = sampled_maps ? sampled_maps : atomic_load(&g_vma_count), reserve = maximum / 8;
    if (reserve < 1024) reserve = 1024;
    if (g_lag_enabled && maximum) {
        uint64_t regions = atomic_load(&g_vma_regions), limit;
        if (sampled_maps) {
            uint64_t headroom = maps < maximum ? maximum - maps : 0;
            limit = regions + (headroom > reserve ? (headroom - reserve) / 2 : 0);
            if (headroom <= reserve) vma_pressure();
        } else {
            /* Missing samples must not replenish credit against old maps and newer run reservations. */
            limit = atomic_load(&g_vma_limit);
            if (limit == UINT64_MAX) limit = maximum / 16;
        }
#ifdef TOTK_VK_TESTS
        uint64_t test_limit = atomic_load(&g_vma_test_limit); if (limit > test_limit) limit = test_limit;
#endif
        atomic_store(&g_vma_limit, limit);
    }
    if ((!sampled_maps || !sampled_max) && !g_vma_missing_reported) {
        g_vma_missing_reported = 1;
        LOG("VMA sampling: maps %llu (missing=%d), kernel limit %llu (%s); missing samples do not latch lag fallback",
            (unsigned long long)maps, !sampled_maps, (unsigned long long)maximum,
            sampled_max ? "observed" : "last observed or Linux default");
    }
    if (atomic_load(&g_vma_pressure) && !atomic_exchange(&g_vma_reported, 1))
        LOG("lag VMA pressure: maps %llu/%llu, headroom %llu reserve %llu, guarded-run credit %llu/%llu; current frame waits its raw fence; switching to normal lag-off submission at the next safe boundary",
            (unsigned long long)maps, (unsigned long long)maximum,
            (unsigned long long)(maps < maximum ? maximum - maps : 0), (unsigned long long)reserve,
            (unsigned long long)atomic_load(&g_vma_regions), (unsigned long long)atomic_load(&g_vma_limit));
    /* Publish the sampled count after its budget/pressure, so observers of a new count see that update. */
    if (sampled_maps) atomic_store(&g_vma_count, sampled_maps);
}
void nvn_dirty_lag_vma_refresh(void) {
    pthread_mutex_lock(&g_vma_mu);
    vma_sample(vma_read("/proc/self/maps", 1), vma_read("/proc/sys/vm/max_map_count", 0));
    pthread_mutex_unlock(&g_vma_mu);
}
void nvn_dirty_lag_vma_request(int classes) {
    pthread_once(&g_vma_once, vma_start);
    /* Requests mark the next scheduled scan, never advance it. Epoch bursts must not cause repeated
       mmap_lock acquisition between periods (even nice+10 scans contend with the game's mprotect).
       Both epoch and diagnostic requests coalesce; no scan mutex or worker wake on this path. */
    atomic_fetch_or_explicit(&g_vma_requests, classes ? VMA_CLASSES : VMA_EPOCH, memory_order_release);
}
static void vma_refresh_changed(void) {
    uint64_t epoch = nvn_dirty_lag_epoch();
    if (g_lag_enabled && atomic_exchange_explicit(&g_vma_epoch, epoch, memory_order_relaxed) != epoch)
        nvn_dirty_lag_vma_request(0);
}
int nvn_dirty_lag_vma_pressure(void) { return atomic_load_explicit(&g_vma_pressure, memory_order_acquire); }
void nvn_dirty_lag_vma_stats(uint64_t values[5]) {
    values[0] = atomic_load(&g_vma_count); values[1] = atomic_load(&g_vma_max);
    values[2] = atomic_load(&g_vma_regions); values[3] = atomic_load(&g_vma_limit);
    values[4] = atomic_load(&g_vma_pressure);
}
void nvn_dirty_lag_vma_scan_stats(uint64_t values[4], int reset) {
    pthread_mutex_lock(&g_vma_scan_mu);
    for (unsigned i = 0; i < 4; i++) {
        values[i] = g_vma_scan_stats[i];
        if (reset) g_vma_scan_stats[i] = 0;
    }
    pthread_mutex_unlock(&g_vma_scan_mu);
}
/* One bounded snapshot per memory report. Range lifetime fields are copied under their existing owner lock;
   sorting/scanning procfs happens after releasing it. No allocator or procfs work enters a signal handler. */
typedef struct { uintptr_t lo, hi; } vma_range;
static vma_range g_vma_ranges[MAX_RANGE];
static int vma_range_cmp(const void *left, const void *right) {
    const vma_range *a = left, *b = right; return (a->lo > b->lo) - (a->lo < b->lo);
}
void nvn_dirty_lag_vma_report(void) {
#ifdef __linux__
#ifdef TOTK_VK_TESTS
    g_vma_test_reads++;
#endif
    pthread_mutex_lock(&g_vma_mu);
    size_t nr = 0;
    pthread_mutex_lock(&g_slot_mu);
    for (int i = 0, n = atomic_load(&g_nr); i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire))
        g_vma_ranges[nr++] = (vma_range){g_r[i].base, g_r[i].end};
    pthread_mutex_unlock(&g_slot_mu);
    qsort(g_vma_ranges, nr, sizeof *g_vma_ranges, vma_range_cmp);
    FILE *file = fopen("/proc/self/maps", "re");
    char input[VMA_MAPS_READ_BYTES];
    if (file && setvbuf(file, input, _IOFBF, sizeof input)) { fclose(file); file = NULL; }
    uint64_t prot[8] = {0}, anonymous = 0, backed = 0, shared = 0, driver = 0, heap = 0;
    uint64_t tracked = 0, tracked_ro = 0, tracked_rw = 0, total = 0;
    uintptr_t run_lo = 0, run_hi = 0, most_lo = 0, most_hi = 0;
    uint64_t run_count = 0, most_count = 0, run_tracked = 0, most_tracked = 0;
    unsigned long long run_inode = 0, run_offset = 0;
    unsigned run_major = 0, run_minor = 0; uint64_t run_name = 0;
    size_t at = 0; char line[1024];
    while (file && fgets(line, sizeof line, file)) {
        unsigned long lo, hi; char permission[5]; unsigned long long offset, inode;
        unsigned major, minor; int name_at = 0;
        int complete = strchr(line, '\n') != NULL;
        if (sscanf(line, "%lx-%lx %4s %llx %x:%x %llu %n", &lo, &hi, permission, &offset, &major, &minor, &inode, &name_at) != 7)
            continue;
        unsigned bits = (permission[0] == 'r' ? 4 : 0) | (permission[1] == 'w' ? 2 : 0) | (permission[2] == 'x' ? 1 : 0);
        prot[bits]++; total++; shared += permission[3] == 's';
        const char *name = line + name_at;
        int anon = !inode && (!*name || *name == '\n' || *name == '[');
        anonymous += anon; backed += !anon;
        driver += strstr(name, "/dev/kgsl") != NULL || strstr(name, "/dev/dri/") != NULL ||
                  strstr(name, "dmabuf") != NULL || strstr(name, "dma_buf") != NULL;
        heap += strstr(name, "[heap]") != NULL || strstr(name, "[anon:scudo") != NULL ||
                strstr(name, "[anon:libc_malloc") != NULL || strstr(name, "[anon:GWP-ASan") != NULL;
        while (at < nr && g_vma_ranges[at].hi <= lo) at++;
        int overlap = at < nr && g_vma_ranges[at].lo < hi;
        tracked += overlap; tracked_ro += overlap && bits == 4; tracked_rw += overlap && bits == 6;
        uint64_t name_hash = UINT64_C(1469598103934665603);
        for (const char *s = name; *s && *s != '\n'; s++) name_hash = (name_hash ^ (unsigned char)*s) * UINT64_C(1099511628211);
        /* Same backing plus linear file offset identifies a fragmented guest shmem reservation, not an
           unrelated row of Vulkan BOs. Anonymous runs have no meaningful file offset. */
        int same = run_hi == lo && run_inode == inode && run_major == major && run_minor == minor &&
                   run_name == name_hash && (!inode || run_offset == offset);
        if (!same) { run_lo = lo; run_count = run_tracked = 0; }
        run_hi = hi; run_count++; run_tracked += overlap;
        run_inode = inode; run_major = major; run_minor = minor; run_name = name_hash;
        run_offset = offset + hi - lo;
        if (run_count > most_count) {
            most_lo = run_lo; most_hi = hi; most_count = run_count; most_tracked = run_tracked;
        }
        /* A long pathname may fill the buffer; discard only its continuation, not another maps record. */
        while (!complete && fgets(line, sizeof line, file)) complete = strchr(line, '\n') != NULL;
    }
    int failed = !file || ferror(file); if (file) fclose(file);
    vma_sample(failed ? 0 : total, vma_read("/proc/sys/vm/max_map_count", 0));
    LOG("memory VMA classes: total %llu, r--/rw-/r-x/--- %llu/%llu/%llu/%llu, other %llu; anon/file %llu/%llu, shared %llu, driver %llu, native-heap %llu; tracked pools %llu (r--/rw- %llu/%llu), unavailable=%d",
        (unsigned long long)total, (unsigned long long)prot[4], (unsigned long long)prot[6],
        (unsigned long long)prot[5], (unsigned long long)prot[0],
        (unsigned long long)(total - prot[4] - prot[6] - prot[5] - prot[0]),
        (unsigned long long)anonymous, (unsigned long long)backed, (unsigned long long)shared,
        (unsigned long long)driver, (unsigned long long)heap, (unsigned long long)tracked,
        (unsigned long long)tracked_ro, (unsigned long long)tracked_rw, failed);
    LOG("memory VMA fragmentation: largest same-backing linear run %#lx-%#lx, %.1f MiB, %llu maps (%llu overlap tracked pools)",
        (unsigned long)most_lo, (unsigned long)most_hi, (most_hi - most_lo) / 1048576.0,
        (unsigned long long)most_count, (unsigned long long)most_tracked);
    uint64_t maps = atomic_load(&g_vma_count), maximum = atomic_load(&g_vma_max);
    LOG("memory VMA budget: sampled-total baseline upper-bound %llu, headroom %llu, reserve %llu, limit assumed=%d maps sample missing=%d",
        (unsigned long long)maps, (unsigned long long)(maps < maximum ? maximum - maps : 0),
        (unsigned long long)(maximum / 8 < 1024 ? 1024 : maximum / 8),
        atomic_load(&g_vma_assumed_max), atomic_load(&g_vma_missing_maps));
    pthread_mutex_unlock(&g_vma_mu);
#endif
}
static uint64_t vma_clock_ns(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
static void *vma_worker(void *unused) {
    (void)unused;
#ifdef __linux__
    prctl(PR_SET_NAME, "totk-vmascan", 0, 0, 0);
    /* Linux nice is per-thread. Failure to lower priority is harmless; never renice the game thread. */
    nice(10);
#endif
    for (;;) {
        /* End-to-start spacing also covers the first worker scan after the synchronous startup sample.
           Only this thread sleeps: requests cannot shorten the period or accumulate extra scans. */
        struct timespec delay = {g_vma_period_ms / 1000, (g_vma_period_ms % 1000) * 1000000L};
        while (nanosleep(&delay, &delay)) if (errno != EINTR) die("VMA sampler sleep: %d", errno);
        unsigned requests = atomic_exchange_explicit(&g_vma_requests, 0, memory_order_acquire);
        if (!g_lag_enabled && !requests) continue; /* lag-off diagnostic worker has no periodic scans */
        uint64_t start = vma_clock_ns();
        /* Exactly one maps traversal per scheduled scan, even when classification was requested. */
#ifdef __linux__
        if (requests & VMA_CLASSES) nvn_dirty_lag_vma_report();
        else
#endif
            nvn_dirty_lag_vma_refresh();
        uint64_t elapsed = vma_clock_ns() - start;
        pthread_mutex_lock(&g_vma_scan_mu);
        g_vma_scan_stats[0]++;
        g_vma_scan_stats[1] += !!(requests & VMA_EPOCH);
        g_vma_scan_stats[2] += elapsed;
        if (elapsed > g_vma_scan_stats[3]) g_vma_scan_stats[3] = elapsed;
        pthread_mutex_unlock(&g_vma_scan_mu);
    }
    return NULL;
}
static unsigned vma_scan_period(const char *period) {
    if (!period || !*period) return VMA_SCAN_DEFAULT_MS;
    char *end; errno = 0; long long value = strtoll(period, &end, 10);
    if (end == period || *end || (errno && errno != ERANGE)) {
        LOG("invalid TOTK_VMA_SCAN_MS '%s'; using %u ms", period, (unsigned)VMA_SCAN_DEFAULT_MS);
        return VMA_SCAN_DEFAULT_MS;
    }
    unsigned clamped = value < VMA_SCAN_MIN_MS ? VMA_SCAN_MIN_MS :
                       value > VMA_SCAN_MAX_MS ? VMA_SCAN_MAX_MS : (unsigned)value;
    if (value != clamped) LOG("clamped TOTK_VMA_SCAN_MS '%s' to %u ms", period, clamped);
    return clamped;
}
static void vma_start(void) {
    /* pthread_once makes a clamping diagnostic a startup-only message, never one per scan/request. */
    g_vma_period_ms = vma_scan_period(getenv("TOTK_VMA_SCAN_MS"));
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) die("VMA sampler thread attributes");
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED)) die("VMA sampler detach");
    pthread_t thread;
    int result = pthread_create(&thread, &attr, vma_worker, NULL);
    pthread_attr_destroy(&attr);
    if (result) die("VMA sampler thread: %d", result);
}
#ifdef TOTK_VK_TESTS
void nvn_dirty_lag_test_vma_limit(uint64_t limit) {
    atomic_store(&g_vma_test_limit, limit); atomic_store(&g_vma_limit, limit);
}
void nvn_dirty_lag_test_vma_sample(uint64_t maps, uint64_t maximum) {
    pthread_mutex_lock(&g_vma_mu);
    g_vma_test_maps = maps; g_vma_test_max = maximum; g_vma_test_sample = 1;
    pthread_mutex_unlock(&g_vma_mu);
}
uint64_t nvn_dirty_lag_test_vma_reads(void) { return g_vma_test_reads; }
unsigned nvn_dirty_lag_test_vma_period(const char *value) { return vma_scan_period(value); }
#endif
static int vma_charge(lag_unit_t *unit) {
    if (nvn_dirty_lag_vma_pressure()) return 0;
    uint64_t old = atomic_load_explicit(&g_vma_regions, memory_order_relaxed);
    do {
        uint64_t limit = atomic_load_explicit(&g_vma_limit, memory_order_relaxed);
        if (old > limit || g_unit > limit - old) { vma_pressure(); return 0; }
    } while (!atomic_compare_exchange_weak_explicit(&g_vma_regions, &old, old + g_unit, memory_order_acq_rel, memory_order_relaxed));
    unit->regions += g_unit; return 1;
}
static void vma_unprotect_unit(range_t *r, uintptr_t index); /* caller owns its BUSY gate */
static __thread _Atomic(const char *) g_lag_thread_role;
void nvn_dirty_lag_thread_role(const char *role) { atomic_store_explicit(&g_lag_thread_role, role, memory_order_relaxed); }
const char *nvn_dirty_lag_thread_role_get(void) {
    const char *role = atomic_load_explicit(&g_lag_thread_role, memory_order_relaxed);
    return role ? role : "unknown/guest";
}
#ifdef TOTK_VK_TESTS
static _Atomic(nvn_dirty_lag_test_handoff *) g_lag_test_handoff;
void nvn_dirty_lag_test_handoff_set(nvn_dirty_lag_test_handoff *handoff) {
    atomic_store_explicit(&g_lag_test_handoff, handoff, memory_order_release);
}
/* Pause only the first matching registrar, after it has sampled BUSY. The fixture changes ownership while it
   is parked; production has neither the hook nor its loads. No fault-path callback, lock or allocation. */
static nvn_dirty_lag_test_handoff *lag_test_busy(const range_t *r, uintptr_t index, uint64_t fence, uint64_t gate) {
    nvn_dirty_lag_test_handoff *h = atomic_load_explicit(&g_lag_test_handoff, memory_order_acquire);
    if (!h || h->fence != fence || (uintptr_t)h->addr < unit_addr(r, index * g_unit) ||
        (uintptr_t)h->addr >= unit_addr(r, index * g_unit) + unit_len(r, index * g_unit, g_unit)) return NULL;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&h->busy_seen, &expected, -1, memory_order_acq_rel, memory_order_acquire)) return NULL;
    h->busy_ticket = gate >> 2;
    h->completed_ticket = atomic_load_explicit(&g_lag_done, memory_order_acquire);
    atomic_store_explicit(&h->busy_seen, 1, memory_order_release);
    while (!atomic_load_explicit(&h->release, memory_order_acquire)) sched_yield();
    return h;
}
static _Atomic(nvn_dirty_test_wb_handoff *) g_test_wb;
void nvn_dirty_test_wb_set(nvn_dirty_test_wb_handoff *handoff) {
    atomic_store_explicit(&g_test_wb, handoff, memory_order_release);
}
void nvn_dirty_test_wb_phase(const void *addr, int phase) {
    nvn_dirty_test_wb_handoff *h = atomic_load_explicit(&g_test_wb, memory_order_acquire);
    if (!h || h->addr != addr || h->phase != phase) return;
    atomic_store_explicit(&h->arrived, 1, memory_order_release);
    while (!atomic_load_explicit(&h->release, memory_order_acquire)) sched_yield();
}
#endif
void nvn_dirty_lag_enable(void) {
    g_lag_enabled = 1;
    /* Startup, before pool registration/gameplay: establish a valid admission budget before any guard. */
    nvn_dirty_lag_vma_refresh();
    pthread_once(&g_vma_once, vma_start);
}
int nvn_dirty_lag_enabled(void) { return g_lag_enabled; }
void nvn_dirty_lag_frame(uint64_t frame) {
    uint64_t old = atomic_load_explicit(&g_lag_frame, memory_order_relaxed);
    while (old < frame && !atomic_compare_exchange_weak_explicit(&g_lag_frame, &old, frame, memory_order_release, memory_order_relaxed)) {}
}
uint64_t nvn_dirty_lag_frame_now(void) { return atomic_load_explicit(&g_lag_frame, memory_order_acquire); }
void nvn_dirty_lag_submitted(uint64_t ticket) {
    if (!g_lag_enabled) return;
    uint64_t old = atomic_load_explicit(&g_lag_submitted, memory_order_relaxed);
    while (old < ticket &&
           !atomic_compare_exchange_weak_explicit(&g_lag_submitted, &old, ticket, memory_order_release, memory_order_relaxed)) {}
}
void nvn_dirty_lag_complete(uint64_t fence) {
    atomic_store_explicit(&g_lag_done, fence, memory_order_release);
#ifdef __linux__
    atomic_fetch_add_explicit(&g_lag_wake, 1, memory_order_release);
    syscall(SYS_futex, &g_lag_wake, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#endif
}
static char *lag_log_text(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *lag_log_uint(char *p, uint64_t value, unsigned base) {
    char digits[20]; unsigned n = 0;
    do { digits[n++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
    while (n) *p++ = digits[--n];
    return p;
}
static char *lag_log_hex(char *p, uint64_t value) { return lag_log_uint(lag_log_text(p, "0x"), value, 16); }
/* Roles are immutable caller-owned literals, kernel names are bounded to 15 bytes. Keep fields single-token
   and bounded even if a caller supplies a longer label; no libc string/formatting operations in a signal. */
static char *lag_log_label(char *p, const char *s, unsigned limit) {
    for (unsigned i = 0; i < limit && s[i]; i++) {
        unsigned char c = (unsigned char)s[i]; *p++ = c > ' ' && c < 127 && c != '=' ? (char)c : '_';
    }
    return p;
}
static void lag_wait_report(const range_t *r, uintptr_t index, uint64_t ticket, int host, int gate_wait, uint64_t elapsed_ns) {
    int saved_errno = errno;
    uint64_t done = atomic_load_explicit(&g_lag_done, memory_order_acquire);
    uint64_t submitted = atomic_load_explicit(&g_lag_submitted, memory_order_acquire);
    uint64_t gate = atomic_load_explicit(&r->lag[index].gate, memory_order_acquire);
    char name[16] = "unknown";
#ifdef __linux__
    uint64_t tid = (uint64_t)syscall(SYS_gettid);
#if defined(SYS_prctl) && defined(PR_GET_NAME)
    if (syscall(SYS_prctl, PR_GET_NAME, name, 0, 0, 0) != 0) {
        name[0] = 'u'; name[1] = 'n'; name[2] = 'k'; name[3] = 'n';
        name[4] = 'o'; name[5] = 'w'; name[6] = 'n'; name[7] = 0;
    }
    name[sizeof name - 1] = 0;
#endif
#else
    uint64_t tid = (uint64_t)syscall(SYS_thread_selfid);
#endif
    /* Bounded labels and maximal 64-bit numbers keep this raw-write record below 512 bytes. The caller supplies its
       retained range identity: no signal-time slot scan, pthread name lookup or fabricated unit address. */
    char line[512], *p = line;
    p = lag_log_text(p, "[dirty-lag-wait] kind="); p = lag_log_text(p, gate_wait ? "gate" : host ? "host" : "fault");
    p = lag_log_text(p, " origin="); p = lag_log_text(p, host ? "host" : "fault");
    p = lag_log_text(p, " tid="); p = lag_log_uint(p, tid, 10);
    p = lag_log_text(p, " role="); p = lag_log_label(p, nvn_dirty_lag_thread_role_get(), 48);
    p = lag_log_text(p, " name="); p = lag_log_label(p, name, sizeof name - 1);
    p = lag_log_text(p, " wait_ms="); p = lag_log_uint(p, elapsed_ns / 1000000, 10);
    p = lag_log_text(p, " unit="); p = lag_log_hex(p, unit_addr(r, index * g_unit));
    p = lag_log_text(p, "+"); p = lag_log_hex(p, unit_len(r, index * g_unit, g_unit));
    p = lag_log_text(p, " ticket="); p = lag_log_uint(p, ticket, 10);
    p = lag_log_text(p, " GPUdone="); p = lag_log_uint(p, done, 10);
    p = lag_log_text(p, " submitted="); p = lag_log_uint(p, submitted, 10);
    p = lag_log_text(p, " ticket_state="); p = lag_log_text(p, ticket <= done ? "complete" : ticket <= submitted ? "submitted" : "unsubmitted");
    p = lag_log_text(p, " submitted_state="); p = lag_log_text(p, !submitted ? "none" : submitted <= done ? "complete" : "inflight");
    p = lag_log_text(p, " gate_ticket="); p = lag_log_uint(p, gate >> 2, 10);
    p = lag_log_text(p, " gate_state=");
    p = lag_log_text(p, (gate & LAG_STATE_MASK) == LAG_BUSY ? "BUSY" : (gate & LAG_STATE_MASK) == LAG_ARMED ? "ARMED" : "NONE");
    *p++ = '\n';
    for (size_t off = 0, len = (size_t)(p - line); off < len;) {
        ssize_t n = write(STDERR_FILENO, line + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (n >= 0 || errno != EINTR) break;
    }
    errno = saved_errno;
}
/* Remaining time until the first report, or zero once reported (then an indefinite futex wait is sufficient).
   Recheck on wakeups and exit too: completion may race the deadline or the scheduler may resume a waiter late. */
static uint64_t lag_wait_watchdog(uint64_t start, int *reported, const range_t *r, uintptr_t index,
                                  uint64_t ticket, int host, int gate_wait) {
    if (*reported) return 0;
    uint64_t elapsed_ns = (mach_absolute_time() - start) * g_tb.numer / g_tb.denom;
    if (elapsed_ns < LAG_WAIT_REPORT_NS) return LAG_WAIT_REPORT_NS - elapsed_ns;
    *reported = 1;
    lag_wait_report(r, index, ticket, host, gate_wait, elapsed_ns);
    return 0;
}
/* Wait only the captured ticket while owning the unit gate. In particular, never reload a future ticket published
   by the next replay: that replay cannot submit until this writer has released the gate. */
static void lag_wait(uint64_t fence, int host, lag_unit_t *u, const range_t *r, uintptr_t index) {
    uint64_t start = 0; int reported = 0;
    while (fence > atomic_load_explicit(&g_lag_done, memory_order_acquire)) {
        if (!start) {
            start = mach_absolute_time();
            atomic_fetch_add(host ? &g_lag_host_waits : &g_lag_fault_waits, 1);
            if (u) { atomic_fetch_add_explicit(&u->waits, 1, memory_order_relaxed); lag_danger(u, 1); }
        }
        uint64_t remaining = lag_wait_watchdog(start, &reported, r, index, fence, host, 0);
#ifdef __linux__
        uint32_t wake = atomic_load_explicit(&g_lag_wake, memory_order_acquire);
        struct timespec timeout = { 0, (long)remaining };
        if (fence > atomic_load_explicit(&g_lag_done, memory_order_acquire))
            syscall(SYS_futex, &g_lag_wake, FUTEX_WAIT_PRIVATE, wake, remaining ? &timeout : NULL, NULL, 0);
#else
        (void)remaining;
        sched_yield();
#endif
    }
    if (start) {
        lag_wait_watchdog(start, &reported, r, index, fence, host, 0);
        atomic_fetch_add(&g_lag_wait_ticks, mach_absolute_time() - start);
    }
}
static void lag_gate_wait(lag_unit_t *u, const range_t *r, uintptr_t index, int host) {
    uint64_t start = mach_absolute_time(), ticket = atomic_load_explicit(&u->gate, memory_order_acquire) >> 2;
    int reported = 0;
#ifdef __linux__
    atomic_fetch_add(&u->waiters, 1);
#endif
    for (;;) {
#ifdef __linux__
        uint32_t wake = atomic_load_explicit(&u->wake, memory_order_acquire);
#endif
        uint64_t gate = atomic_load_explicit(&u->gate, memory_order_acquire);
        if ((gate & LAG_STATE_MASK) != LAG_BUSY) break;
        ticket = gate >> 2;
        uint64_t remaining = lag_wait_watchdog(start, &reported, r, index, ticket, host, 1);
#ifdef __linux__
        struct timespec timeout = { 0, (long)remaining };
        if ((atomic_load(&u->gate) & LAG_STATE_MASK) == LAG_BUSY)
            syscall(SYS_futex, &u->wake, FUTEX_WAIT_PRIVATE, wake, remaining ? &timeout : NULL, NULL, 0);
#else
        (void)remaining; sched_yield();
#endif
    }
#ifdef __linux__
    atomic_fetch_sub(&u->waiters, 1);
#endif
    lag_wait_watchdog(start, &reported, r, index, ticket, host, 1);
}
/* Claim only ownership, without changing permissions or reserving VMA split credit. Alias writers use
   this directly: even a pressure-triggered lag-only drain would violate their read-only guest contract. */
static uint64_t lag_lock_gate(range_t *r, uintptr_t index, int host) {
    lag_unit_t *u = &r->lag[index];
    uint64_t old = atomic_load_explicit(&u->gate, memory_order_acquire);
    for (;;) {
        if ((old & LAG_STATE_MASK) == LAG_BUSY) {
            lag_gate_wait(u, r, index, host); old = atomic_load_explicit(&u->gate, memory_order_acquire); continue;
        }
        uint64_t busy = (old & ~(uint64_t)LAG_STATE_MASK) | LAG_BUSY;
        if (atomic_compare_exchange_weak_explicit(&u->gate, &old, busy, memory_order_acquire, memory_order_acquire)) return old;
    }
}
static uint64_t lag_lock_pages(range_t *r, uintptr_t index, int host, uintptr_t watch_first, uintptr_t watch_end, unsigned watch_flags) {
    lag_unit_t *u = &r->lag[index];
    uint64_t old = lag_lock_gate(r, index, host);
    /* Watch stamps precede this call. Preserve baseline authority before a pressure drain can
       dirty additional lag-only neighbours; only a real CPU store may revoke these stamps. */
    for (uintptr_t p = watch_first; p < watch_end; p++) {
        unsigned old_flags = atomic_load_explicit(&r->exact[p], memory_order_relaxed);
        atomic_store_explicit(&r->exact[p], old_flags | watch_flags, memory_order_relaxed);
    }
    if (!vma_charge(u)) {
        /* Baseline texture/exact watches must remain valid. Temporary writer/split credit is
           not a new lag guard, and is returned when the gate is released. */
        atomic_fetch_add_explicit(&g_vma_regions, g_unit, memory_order_acq_rel); u->regions += g_unit;
        lag_wait(old >> 2, host, u, r, index);
        vma_unprotect_unit(r, index);
        old &= ~(uint64_t)LAG_STATE_MASK;
    }
    return old;
}
static uint64_t lag_lock(range_t *r, uintptr_t index, int host) { return lag_lock_pages(r, index, host, 0, 0, 0); }
static void lag_unlock(range_t *r, uintptr_t index, uint64_t value) {
    lag_unit_t *u = &r->lag[index];
    uintptr_t first = index * g_unit, last = first + g_unit, pages = (r->end - r->base) >> g_shift;
    if (last > pages) last = pages;
    uintptr_t runs = 0; int prior = 0;
    for (uintptr_t p = first; p < last; p++) {
        int protected = atomic_load_explicit(&r->state[p], memory_order_relaxed) == PG_PROT;
        runs += protected && !prior; prior = protected;
    }
    atomic_fetch_sub_explicit(&g_vma_regions, u->regions - runs, memory_order_release); u->regions = runs;
    /* The seq_cst gate store / waiter check pair matches the waiter's increment / gate check: either it observes the
       unlocked gate or we observe its registration and wake it. No futex syscall for an uncontended rearm/write. */
    atomic_store(&u->gate, value);
#ifdef __linux__
    if (atomic_load(&u->waiters)) {
        atomic_fetch_add_explicit(&u->wake, 1, memory_order_release);
        syscall(SYS_futex, &u->wake, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    }
#endif
}
static int lag_hot_age(lag_unit_t *u, uint64_t frame) {
    uint64_t hot = atomic_load_explicit(&u->hot_frame, memory_order_acquire);
    while (hot && frame >= hot && frame - hot >= 8) {
        if (atomic_compare_exchange_weak_explicit(&u->hot_frame, &hot, 0, memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&g_lag_demotions, 1, memory_order_relaxed); return 0;
        }
    }
    return hot != 0;
}
static void lag_note_write(lag_unit_t *u) {
    uint64_t frame = atomic_load_explicit(&g_lag_frame, memory_order_acquire);
    uint64_t previous = atomic_exchange_explicit(&u->last_frame, frame, memory_order_acq_rel);
    atomic_fetch_add_explicit(&u->faults, 1, memory_order_relaxed);
    /* Only the latest distinct writing frame is needed before promotion. Once hot, every write refreshes its quiet
       deadline. Queries may concurrently age hot_frame; CAS preserves their demotions and counts each transition once. */
    for (;;) {
        uint64_t now = atomic_load_explicit(&g_lag_frame, memory_order_acquire);
        if (now >= frame && now - frame >= 8) { lag_hot_age(u, now); return; }
        uint64_t hot = atomic_load_explicit(&u->hot_frame, memory_order_acquire);
        if (hot && frame >= hot && frame - hot >= 8) { lag_hot_age(u, frame); continue; }
        if (!hot && !(previous && frame > previous && frame - previous < 8)) return;
        if (hot >= frame) return;
        uint64_t old = hot;
        if (atomic_compare_exchange_weak_explicit(&u->hot_frame, &hot, frame, memory_order_acq_rel, memory_order_acquire)) {
            if (!old) { atomic_fetch_add_explicit(&g_lag_promotions, 1, memory_order_relaxed); lag_danger(u, 0); }
            return;
        }
    }
}
void nvn_dirty_lag_stats(uint64_t *faults, uint64_t *host, uint64_t *ns) {
    *faults = atomic_load(&g_lag_fault_waits); *host = atomic_load(&g_lag_host_waits);
    *ns = g_tb.denom ? atomic_load(&g_lag_wait_ticks) * g_tb.numer / g_tb.denom : 0;
}
void nvn_dirty_lag_sticky_stats(uint64_t *rearms, uint64_t *ticket_updates, uint64_t *promotions, uint64_t *demotions) {
    *rearms = atomic_load(&g_lag_rearms); *ticket_updates = atomic_load(&g_lag_ticket_updates);
    *promotions = atomic_load(&g_lag_promotions); *demotions = atomic_load(&g_lag_demotions);
}
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限。缺页路径不记录日志。
// Wait for this page to leave BUSY. Returns 1 on success, 0 on timeout. Never log from the fault path.
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) >= PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) return 0;
        sched_yield();
    }
    return 1;
}
static range_t *find_range(uintptr_t a, int cache) {
    /* Page-window hints avoid even the index search for the common repeated pools. Slots are only hints: recheck
       live and the actual bounds on every hit, including after pool-slot reuse. Read-only registration queries
       (cache 0) keep their own hint table, so they never displace the fault path's hints. Behind the hints, the
       sorted live-range index (rix_find) answers both hits and misses; the linear scan remains only for a search
       that overlapped an index rebuild. Live ranges are disjoint, so any validated range containing the address
       is the range the scan would find. */
    enum { NH = 256 };
    static __thread struct { range_t *hint[NH], *recent[8], *query[NH]; unsigned next; } local;
    __typeof__(local) *t = &local;
    unsigned h = (unsigned)((a >> 16) ^ (a >> 24) ^ (a >> 32)) & (NH - 1);
    range_t *r = t->hint[h];
    if (r && range_has(r, a)) return r;
    r = t->query[h];
    if (!cache && r && range_has(r, a)) return r;
    for (int k = 0; k < 8; k++) {
        range_t *l = t->recent[k];
        if (l && range_has(l, a)) {
            if (cache) t->hint[h] = l; else t->query[h] = l;
            return l;
        }
    }
    int trusted;
    r = rix_find(a, &trusted);
    if (!r && !trusted) {
        int n = atomic_load_explicit(&g_nr, memory_order_acquire);
        for (int i = 0; i < n && !r; i++) if (range_has(&g_r[i], a)) r = &g_r[i];
    }
    if (r) {
        if (cache) { t->hint[h] = r; t->recent[t->next++ & 7] = r; }
        else t->query[h] = r;
    }
    return r;
}
static range_t *find(uintptr_t a) { return find_range(a, 1); }
/* TOTK diagnostic (on with DM_NVN_TILELOG): a ring of the latest page unprotections with the writer, read by
   nvn_dirty_recent: game writes (fault: pc / lr of the faulting code) and host writes (dm_guest_write_prepare caller). */
#include <sys/ucontext.h>
#include "../../../../runtime/host_os.h"   /* UC_PC / UC_LR for Mach-O and Linux signal frames */
static nvn_dirty_rec g_drec[4096]; static _Atomic uint64_t g_drec_n; static int g_drec_on;
uint64_t dm_host_write_pc;   // set by dm_guest_write_prepare (runtime/gfx_bridge.c) before calling prep_write
static void drec_add(uintptr_t page, uint64_t pc, uint64_t lr, uint32_t src) {
    if (!g_drec_on) return;
    uint64_t i = atomic_fetch_add(&g_drec_n, 1) % 4096; g_drec[i] = (nvn_dirty_rec){ page, pc, lr, src };
}
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) {
    uint64_t n = atomic_load(&g_drec_n); int k = 0;
    for (uint64_t j = n > 4096 ? n - 4096 : 0; j < n && k < max; j++) { nvn_dirty_rec r = g_drec[j % 4096]; if (r.page >= lo && r.page < hi) out[k++] = r; }
    return k;
}
static void vma_unprotect_unit(range_t *r, uintptr_t index) {
    uintptr_t first = index * g_unit, last = first + g_unit, pages = (r->end - r->base) >> g_shift;
    if (last > pages) last = pages;
    uint64_t generation = 0;
    for (uintptr_t p = first, run = first; p <= last; p++) {
        int remove = p < last && !atomic_load(&r->exact[p]) && atomic_load(&r->state[p]) == PG_PROT;
        if (remove) { atomic_store(&r->state[p], PG_BUSY); continue; }
        if (run < p) {
            mp(unit_addr(r, run), unit_len(r, run, p - run), PROT_READ | PROT_WRITE, NVN_DIRTY_MP_DRAIN);
            if (!generation) generation = atomic_fetch_add(&g_now, 1) + 1;
            for (uintptr_t k = run; k < p; k++) { atomic_store(&r->gen[k], generation); atomic_store(&r->state[k], PG_NONE); }
        }
        run = p + 1;
    }
    if (generation) {
        uint64_t latest = atomic_load(&r->latest);
        while (latest < generation && !atomic_compare_exchange_weak(&r->latest, &latest, generation)) {}
        atomic_fetch_add(&g_now, 1);
    }
}
static uintptr_t range_pages(const range_t *r) { return (r->end - r->base) >> g_shift; }
/* Only the ordered submission owner calls this, AFTER its failed slot has really queued and its GPU fence
   completed. Additional lag watches are disabled globally, so no later unsubmitted ticket can appear in this sweep. */
void nvn_dirty_lag_vma_drain(void) {
    if (!nvn_dirty_lag_vma_pressure()) return;
    int expected = 0; if (!atomic_compare_exchange_strong(&g_vma_drained, &expected, 1)) return;
    /* One-shot safe-boundary cutover, after all owners/GPU work joined, not a recurring frame sample:
       retain the trigger counts before coalescing the old protections. Never hold g_slot_mu here:
       classification takes g_vma_mu then g_slot_mu, so the reverse order would deadlock. */
    nvn_dirty_lag_vma_refresh();
    pthread_mutex_lock(&g_slot_mu);
    for (int i = 0, count = atomic_load(&g_nr); i < count; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || !r->lag) continue;
        uintptr_t units = (range_pages(r) + g_unit - 1) / g_unit;
        for (uintptr_t u = 0; u < units; u++) {
            uint64_t gate = lag_lock(r, u, 1); /* pressure restores lag-only pages before returning */
            lag_unlock(r, u, gate & ~(uint64_t)LAG_STATE_MASK);
        }
    }
    pthread_mutex_unlock(&g_slot_mu);
    atomic_store(&g_vma_drained, 2);
    /* Same one-shot cutover, outside g_slot_mu: publish actual post-drain counts for its diagnostic. */
    nvn_dirty_lag_vma_refresh();
}
/* Include the already-owned fault/prepared page in its unit's contiguous permission run. Excluding
   it used to split an ordinary interior write into before/after/page mprotects (three mmap write locks).
   Other exact-watch pages and pages owned by another operation remain barriers, not writable neighbours. */
static void unprotect_unit(range_t *r, uintptr_t p, int host) {
    uintptr_t u0 = p / g_unit * g_unit, u1 = u0 + g_unit, np = range_pages(r); if (u1 > np) u1 = np;
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = u0; i <= u1; i++) {
        uint8_t st = PG_PROT;
        int took = i < u1 && (i == p || (!(atomic_load(&r->exact[i]) & WATCH_EXACT) &&
                   atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY)));
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE,
               host ? NVN_DIRTY_MP_PREPARE : NVN_DIRTY_MP_FAULT);
            uint64_t g = atomic_fetch_add(&g_now, 1) + 1, l = atomic_load(&r->latest);
            for (uintptr_t k = run; k < i; k++) { atomic_store(&r->gen[k], g); atomic_store(&r->exact[k], 0); }
            while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
            /* A checker can sample now between the generation allocation and page stores. Advance again
               before publishing writable state, so that sample cannot hide a later real CPU write. */
            atomic_fetch_add(&g_now, 1);
            for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_NONE);
            open = 0;
        }
    }
}
/* Take the unit BEFORE a page becomes BUSY. A replay either updates ARMED first (and we wait that ticket), or sees
   BUSY and cannot publish its future ticket until all of this writer's mprotects/generation stores have completed. */
static uint8_t unprotect_for_write(range_t *r, uintptr_t p, int host) {
    lag_unit_t *u = r->lag ? &r->lag[p / g_unit] : NULL;
    uint64_t gate = u ? lag_lock(r, p / g_unit, host) : 0;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) {
        if (u) { lag_note_write(u); lag_wait(gate >> 2, host, u, r, p / g_unit); }
        unprotect_unit(r, p, host);
        if (u) lag_unlock(r, p / g_unit, gate & ~(uint64_t)LAG_STATE_MASK);
        return PG_PROT;
    }
    if (u) lag_unlock(r, p / g_unit, gate);
    return st;
}
/* A WB-only fault takes no GPU ticket. If these pages independently guard a real GPU buffer read,
   unprotect_for_write still honours THAT reader's gate; removing write-back must not weaken frame-lag safety.
   Exact neighbours belonging to another rendering remain barriers, as in the ordinary unit fault path. */
static int wb_unprotect_write(range_t *r, uintptr_t p, int host, uintptr_t address) {
    if (!(atomic_load_explicit(&r->exact[p], memory_order_relaxed) & WATCH_EXACT)) return 0;
    unsigned count = atomic_load_explicit(&g_wb_watch_used, memory_order_acquire);
    for (unsigned i = 0; i < count; i++) {
        wb_watch *w = &g_wb_watch[i];
        if (atomic_load_explicit(&w->state, memory_order_acquire) != WB_ARMED ||
            atomic_load(&w->range) != r || p < atomic_load(&w->first) || p > atomic_load(&w->last)) continue;
        unsigned state = WB_ARMED;
        if (!atomic_compare_exchange_strong_explicit(&w->state, &state, WB_FAULT, memory_order_acquire, memory_order_acquire)) continue;
        uintptr_t first = atomic_load(&w->first), last = atomic_load(&w->last);
        /* Re-read AFTER claiming: a scanner may have observed an earlier incarnation of this fixed slot. */
        if (atomic_load(&w->range) != r || atomic_load(&w->identity) != atomic_load(&r->identity) ||
            p < first || p > last) {
            state = WB_FAULT;
            if (!atomic_compare_exchange_strong(&w->state, &state, WB_ARMED)) atomic_store(&w->state, WB_FREE);
            continue;
        }
        atomic_store(&w->cpu_address, address);
        /* Finish the finite permission update before disarming; otherwise its tail could re-protect pages
           after this CPU write won. This waits only for mprotect/page publication, never a WB GPU ticket. */
        uint64_t t0 = mach_absolute_time();
        while (atomic_load_explicit(&w->arming, memory_order_acquire)) {
            if ((mach_absolute_time() - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
                state = WB_FAULT;
                if (!atomic_compare_exchange_strong(&w->state, &state, WB_CPU)) atomic_store(&w->state, WB_FREE);
                return 0; /* existing page BUSY timeout/fault reporting handles a stuck protector */
            }
            sched_yield();
        }
        for (uintptr_t k = first; k <= last; k++) atomic_fetch_and(&r->exact[k], (uint8_t)~WATCH_EXACT);
        for (uintptr_t k = first; k <= last; k++) for (;;) {
            if (atomic_load(&r->state[k]) == PG_NONE) break;
            uint8_t st = unprotect_for_write(r, k, host);
            if (st < PG_BUSY || !busy_wait(r, k)) break;
        }
        atomic_fetch_add(host ? &g_wb_preps : &g_wb_faults, 1);
        state = WB_FAULT;
        if (!atomic_compare_exchange_strong_explicit(&w->state, &state, WB_CPU, memory_order_release, memory_order_relaxed))
            atomic_store_explicit(&w->state, WB_FREE, memory_order_release); /* record retired while we owned it */
        return 1;
    }
    return 0;
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    range_t *r = is_write ? find((uintptr_t)addr) : NULL;
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
#ifdef TOTK_VK_TESTS
    nvn_dirty_test_wb_handoff *handoff = atomic_load_explicit(&g_test_wb, memory_order_acquire);
    if (handoff && (uintptr_t)addr >= (uintptr_t)handoff->addr &&
        (uintptr_t)addr - (uintptr_t)handoff->addr < handoff->size)
        atomic_fetch_add_explicit(&handoff->faults, 1, memory_order_release);
#endif
    uint8_t st = wb_unprotect_write(r, p, 0, (uintptr_t)addr) ? PG_PROT : unprotect_for_write(r, p, 0);
    if (st == PG_PROT) { atomic_fetch_add(&g_faults, 1);
        if (g_drec_on && uc) { ucontext_t *u = uc; drec_add(r->base + (p << g_shift), UC_PC(u), UC_LR(u), 0); }
        return 1; }
    if (st >= PG_BUSY) return busy_wait(r, p);
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            for (;;) { uint8_t st = wb_unprotect_write(r, p, 1, a > unit_addr(r, p) ? a : unit_addr(r, p)) ? PG_PROT : unprotect_for_write(r, p, 1);
                if (st == PG_PROT) { atomic_fetch_add(&g_preps, 1); drec_add(r->base + (p << g_shift), dm_host_write_pc, 0, 1); break; }
                if (st < PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
}

/* Writes on behalf of the GPU (a rendering written back into CPU-visible memory): the bytes change without counting as
   a CPU write. Protected pages are taken to BUSY (a guest write meanwhile waits in the fault handler, then faults again
   and is recorded as usual). Ordinary stores temporarily unprotect; private-alias stores leave the guest read-only.
   End restores each taken page to PROT with its generation kept. Pages already unprotected are left as they are.
   Returns a token for nvn_dirty_gpu_write_end (NULL: nothing tracked). */
typedef struct { range_t *r; uintptr_t p0, p1, u0, nu; uint8_t *was; uint64_t *gates; int alias; } gpu_write_tok;
static void *gpu_write_begin(void *addr, size_t len, int wait, int alias, int *ready) {
    *ready = 1;
    if (!len) return NULL;
    range_t *r = find((uintptr_t)addr); if (!r || len > r->end - (uintptr_t)addr) return NULL;
    uintptr_t a = (uintptr_t)addr, p0 = (a - r->base) >> g_shift, p1 = (a + len - 1 - r->base) >> g_shift;
    uintptr_t u0 = p0 / g_unit, nu = p1 / g_unit - u0 + 1;
    gpu_write_tok *t = malloc(sizeof *t); uint8_t *was = calloc(p1 - p0 + 1, 1);
    uint64_t *gates = r->lag ? malloc(nu * sizeof *gates) : NULL;
    if (!t || !was || (r->lag && !gates)) { free(t); free(was); free(gates); *ready = 0; return NULL; }
    *t = (gpu_write_tok){ r, p0, p1, u0, nu, was, gates, alias };
    /* Retain gates through end. Asynchronous texture landing never waits for GPU readers or a BUSY owner:
       unwind and retry at a later sweep instead. The blocking API retains its existing semantics. */
    if (gates) for (uintptr_t i = 0; i < nu; i++) {
        if (wait) {
            gates[i] = alias ? lag_lock_gate(r, u0 + i, 1) : lag_lock(r, u0 + i, 1);
            lag_wait(gates[i] >> 2, 1, NULL, r, u0 + i);
        }
        else {
            lag_unit_t *u = &r->lag[u0 + i];
            uint64_t old = atomic_load_explicit(&u->gate, memory_order_acquire);
            if ((old & LAG_STATE_MASK) == LAG_BUSY || (old >> 2) > atomic_load_explicit(&g_lag_done, memory_order_acquire) ||
                !atomic_compare_exchange_strong_explicit(&u->gate, &old,
                    (old & ~(uint64_t)LAG_STATE_MASK) | LAG_BUSY, memory_order_acquire, memory_order_acquire)) {
                for (uintptr_t j = 0; j < i; j++) lag_unlock(r, u0 + j, gates[j]);
                free(gates); free(was); free(t); *ready = 0; return NULL;
            }
            gates[i] = old;
        }
    }
    for (uintptr_t p = p0; p <= p1; p++) {
        for (;;) { uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { was[p - p0] = 1; break; }
            if (st < PG_BUSY) break;
            if (!wait || !busy_wait(r, p)) {
                for (uintptr_t q = p0; q < p; q++) if (was[q - p0]) atomic_store(&r->state[q], PG_PROT);
                if (gates) for (uintptr_t i = 0; i < nu; i++) lag_unlock(r, u0 + i, gates[i]);
                free(gates); free(was); free(t); *ready = 0; return NULL;
            }
        }
    }
#ifdef TOTK_VK_TESTS
    nvn_dirty_test_wb_phase(addr, NVN_DIRTY_TEST_WB_PROTECTED);
#endif
    if (!alias) for (uintptr_t p = p0, run = 0, open = 0; p <= p1 + 1; p++) {   /* runs of taken pages: one mprotect each */
        int w = p <= p1 && was[p - p0];
        if (w && !open) { run = p; open = 1; }
        if (!w && open) { mp(r->base + (run << g_shift), (p - run) << g_shift, PROT_READ | PROT_WRITE, NVN_DIRTY_MP_GPU_WRITE); open = 0; }
    }
    return t;
}
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    int ready; return gpu_write_begin(addr, len, 1, 0, &ready);
}
int nvn_dirty_gpu_write_try_begin(void *addr, size_t len, void **token) {
    int ready; *token = gpu_write_begin(addr, len, 0, 0, &ready); return ready;
}
void *nvn_dirty_gpu_write_alias_begin(void *addr, size_t len) {
    int ready; return gpu_write_begin(addr, len, 1, 1, &ready);
}
int nvn_dirty_gpu_write_alias_try_begin(void *addr, size_t len, void **token) {
    int ready; *token = gpu_write_begin(addr, len, 0, 1, &ready); return ready;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    int restored = 1;
    if (t->alias) {
        for (uintptr_t p = t->p0; p <= t->p1; p++)
            if (t->was[p - t->p0]) atomic_store(&t->r->state[p], PG_PROT);
    } else for (uintptr_t p = t->p0, run = 0, open = 0; p <= t->p1 + 1; p++) {
        int w = p <= t->p1 && t->was[p - t->p0];
        if (w && !open) { run = p; open = 1; }
        if (!w && open) {
            int ok = mp(t->r->base + (run << g_shift), (p - run) << g_shift, PROT_READ, NVN_DIRTY_MP_RESTORE);
            for (uintptr_t k = run; k < p; k++) atomic_store(&t->r->state[k], ok ? PG_PROT : PG_NONE);
            restored &= ok; open = 0;
        }
    }
    if (t->gates) for (uintptr_t i = 0; i < t->nu; i++)
        lag_unlock(t->r, t->u0 + i, restored ? t->gates[i] : t->gates[i] & ~(uint64_t)LAG_STATE_MASK);
    free(t->gates); free(t->was); free(t);
}
/* GPU reports/events are implemented with host C stores. A prepare-before-write alone is insufficient: a replay can
   re-arm between the prepare and the actual store (and libc/TSan describes memcpy before its first fault). Keep the
   unit gate through the store itself, restoring only pages that were protected. No token allocation is needed. */
static int lag_store(void *address, void *write_address, const void *data, size_t len, int add32, int alias) {
    uintptr_t a = (uintptr_t)address;
    uint8_t *dst = write_address;
    const uint8_t *src = data;
    while (len) {
        range_t *r = find(a);
        if (!r || !r->lag) {
            size_t n = g_pg ? g_pg - (a & (g_pg - 1)) : len; if (n > len) n = len;
            /* Lag-off still has baseline/exact dirty watches. Claim this guest page while the alias
               store runs so a CPU fault cannot revoke ownership concurrently; one page needs no token allocation. */
            uintptr_t p = 0; int taken = 0;
            if (alias && r) {
                p = (a - r->base) >> g_shift;
                for (;;) {
                    uint8_t st = PG_PROT;
                    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { taken = 1; break; }
                    if (st < PG_BUSY) break;
                    if (!busy_wait(r, p)) return 0;
                }
            }
            if (add32) { uint32_t v; memcpy(&v, dst, 4); v += *(const uint32_t *)src; memcpy(dst, &v, 4); }
            else memcpy(dst, src, n);
            if (taken) atomic_store(&r->state[p], PG_PROT);
            if (add32) return 1;
            a += n; dst += n; src += n; len -= n; continue;
        }
        uintptr_t p0 = (a - r->base) >> g_shift, index = p0 / g_unit;
        lag_unit_t *u = &r->lag[index];
        uint64_t gate = alias ? lag_lock_gate(r, index, 1) : lag_lock(r, index, 1);
        lag_wait(gate >> 2, 1, u, r, index);
        uintptr_t end = unit_addr(r, (index + 1) * g_unit); if (end > r->end) end = r->end;
        size_t n = end - a; if (n > len) n = len;
        uintptr_t p1 = (a + n - 1 - r->base) >> g_shift;
        for (uintptr_t p = p0; p <= p1; p++) {
            uint8_t st = PG_PROT;
            while (!atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY) && st >= PG_BUSY) {
                if (!busy_wait(r, p)) {
                    /* Permission changes have not started. Earlier BUSY pages are ours; leave the
                       timed-out page with its original owner and unwind only our successful claims. */
                    for (uintptr_t q = p0; q < p; q++)
                        if (atomic_load(&r->state[q]) == PG_BUSY) atomic_store(&r->state[q], PG_PROT);
                    lag_unlock(r, index, gate); return 0;
                }
                st = PG_PROT;
            }
        }
        int ok = 1, restored = 1;
        if (!alias) for (uintptr_t p = p0, run = p0, open = 0; p <= p1 + 1; p++) {
            int protected = p <= p1 && atomic_load(&r->state[p]) == PG_BUSY;
            if (protected && !open) { run = p; open = 1; }
            if (!protected && open) { ok &= mp(unit_addr(r, run), unit_len(r, run, p - run), PROT_READ | PROT_WRITE, NVN_DIRTY_MP_GPU_WRITE); open = 0; }
        }
        if (ok) {
            if (add32) { uint32_t v; memcpy(&v, dst, 4); v += *(const uint32_t *)src; memcpy(dst, &v, 4); }
            else memcpy(dst, src, n);
        }
        for (uintptr_t p = p0, run = p0, open = 0; p <= p1 + 1; p++) {
            int protected = p <= p1 && atomic_load(&r->state[p]) == PG_BUSY;
            if (protected && !open) { run = p; open = 1; }
            if (!protected && open) {
                int read_only = alias || mp(unit_addr(r, run), unit_len(r, run, p - run), PROT_READ, NVN_DIRTY_MP_RESTORE);
                for (uintptr_t k = run; k < p; k++) atomic_store(&r->state[k], read_only ? PG_PROT : PG_NONE);
                restored &= read_only; open = 0;
            }
        }
        lag_unlock(r, index, restored ? gate : gate & ~(uint64_t)LAG_STATE_MASK);
        if (!ok || !restored) return 0;
        if (add32) return 1;   /* NVN events are aligned 32-bit words and cannot cross a protection unit. */
        a += n; dst += n; src += n; len -= n;
    }
    return 1;
}
int nvn_dirty_lag_store(void *addr, const void *data, size_t len) { return lag_store(addr, addr, data, len, 0, 0); }
int nvn_dirty_lag_store32(volatile uint32_t *addr, uint32_t value, int add) { return lag_store((void *)addr, (void *)addr, &value, 4, add, 0); }
int nvn_dirty_lag_store_alias(void *guest, void *alias, const void *data, size_t len, int add32) {
    return lag_store(guest, alias, data, len, add32, 1);
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) {
        inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg);
#ifndef __APPLE__
        { const char *e = getenv("TOTK_DIRTY_UNIT_KB"); unsigned long kb = e && *e ? strtoul(e, NULL, 10) : 64;
          uintptr_t bytes = g_pg; while (bytes < kb * 1024 && bytes < (1u << 24)) bytes <<= 1; g_unit = bytes / g_pg; }
#endif
        { const char *e = getenv("TOTK_DIRTY_UNIT"); if (e && atoi(e) > 0) g_unit = (uintptr_t)atoi(e); }
        mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL;
        LOG("dirty tracking: unit %lu KiB (system page %lu KiB), exact pages for GPU write-back",
            (unsigned long)((g_unit * g_pg) >> 10), (unsigned long)(g_pg >> 10));
    }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    {   /* TOTK: a pool created over memory that another live pool already covers must not get a second tracking range: find() returns
           the first live range for an address, so a fault on a page protected under the older range would be looked up in the newer one
           (state NONE, generation 0) and reported as a crash. Inside an existing range the pages are tracked already; any other overlap
           leaves the new pool untracked (its textures are treated as always dirty, which is safe). */
        uintptr_t nb = (uintptr_t)mem & ~(g_pg - 1), ne = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
        for (int i = 0; i < n; i++) {
            if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire) || ne <= g_r[i].base || nb >= g_r[i].end) continue;
            static _Atomic int said; int expected = 0;
            if (atomic_compare_exchange_strong(&said, &expected, 1))
                LOG("脏页跟踪：池 %p+%#zx 与已跟踪区间 %#lx-%#lx 重叠，%s", mem, size, (unsigned long)g_r[i].base, (unsigned long)g_r[i].end,
                    nb >= g_r[i].base && ne <= g_r[i].end ? "已由该区间跟踪" : "不另行跟踪");
            pthread_mutex_unlock(&g_slot_mu); return;
        }
    }
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base + g_pg - 1) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    _Atomic uint8_t *exact = r->exact;
    lag_unit_t *lag = r->lag;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); exact = calloc(np, 1);
        lag = g_lag_enabled ? calloc((np + g_unit - 1) / g_unit, sizeof *lag) : NULL;
        if (!state || !gen || !exact || (g_lag_enabled && !lag)) die("脏页表分配失败（%zu 页）", np);
        /* Old arrays stay allocated for signal readers. Lag-off keeps only the original 10 bytes/page;
           lag-on adds one sticky/history record per protection unit, not another per-page array. */
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)exact, 0, np);
        if (g_lag_enabled && !lag) lag = calloc((r->cap + g_unit - 1) / g_unit, sizeof *lag);
        if (g_lag_enabled && !lag) die("脏页表分配失败（%zu 页）", np);
        if (lag) memset(lag, 0, ((np + g_unit - 1) / g_unit) * sizeof *lag);
    }
    /* base/end are read without the live acquire by rix_find's search, hence atomic stores (relaxed: live publishes). */
    __atomic_store_n(&r->base, base, __ATOMIC_RELAXED); __atomic_store_n(&r->end, end, __ATOMIC_RELAXED);
    r->state = state; r->gen = gen; r->exact = exact; r->lag = lag; atomic_store(&r->latest, 0);
    uint64_t identity = atomic_load_explicit(&g_range_epoch, memory_order_relaxed) + 1;
    atomic_store_explicit(&r->identity, identity, memory_order_release);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    atomic_store_explicit(&g_range_epoch, identity, memory_order_release);
    rix_update(slot, 1);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) {
        prep_write((void *)r->base, r->end - r->base);
        atomic_store_explicit(&r->live, 0, memory_order_release);
        atomic_fetch_add_explicit(&g_range_epoch, 1, memory_order_release);
        rix_update((int)(r - g_r), 0);
        atomic_fetch_sub(&g_live, 1);
    }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && len <= r->end - (uintptr_t)addr;
}
/* Caller holds the unit gate when lag metadata exists. Lag-off retains the original cross-unit mprotect batching.
   Publish PG_PROT only after a successful syscall; a failed lag rearm remains NONE and returns 0 for copy fallback.
   Explicit-ticket registrars never wait for PG_BUSY either: the caller must queue before any ownership wait. */
static int protect_span(range_t *r, uintptr_t p, uintptr_t q, int wait_busy, mp_stats_t *stats, int baseline_watch,
                        nvn_dirty_mp_reason reason) {
    if (r->lag && !baseline_watch && nvn_dirty_lag_vma_pressure()) return 0;
    uintptr_t run = 0; int open = 0, ok = 1;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            int protected = mp_counted(unit_addr(r, run), unit_len(r, run, i - run), PROT_READ, stats, reason);
            for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], protected ? PG_PROT : PG_NONE);
            ok &= protected; open = 0;
        }
        if (!took && st >= PG_BUSY) {
            if (!wait_busy) { ok = 0; continue; }
            if (busy_wait(r, i)) i--;   /* retry: the other owner may have left the page writable */
            else ok = 0;
        }
    }
    return ok;
}
typedef struct { uint64_t rearms, updates; mp_stats_t mp; } lag_registration_stats;
static void lag_registration_flush(const lag_registration_stats *stats) {
    if (stats->rearms) atomic_fetch_add_explicit(&g_lag_rearms, stats->rearms, memory_order_relaxed);
    if (stats->updates) atomic_fetch_add_explicit(&g_lag_ticket_updates, stats->updates, memory_order_relaxed);
    if (stats->mp.calls) {
        atomic_fetch_add_explicit(&g_mp_calls, stats->mp.calls, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_mp_pages, stats->mp.pages, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_mp_ticks, stats->mp.ticks, memory_order_relaxed);
    }
}
static int lag_protect_unit(range_t *r, uintptr_t index, uint64_t fence, lag_registration_stats *stats) {
    lag_unit_t *u = &r->lag[index];
    uint64_t old = atomic_load_explicit(&u->gate, memory_order_acquire);
    for (;;) {
        if ((old & LAG_STATE_MASK) == LAG_BUSY) {
            /* Even a completed captured ticket is unsafe: by the time a waiter wakes, a new owner can be BUSY
               waiting for this future ticket. Explicit-ticket registrars must return for postqueue fallback,
               never follow any BUSY ownership transition while QueueSubmit is waiting for their join. */
#ifdef TOTK_VK_TESTS
            nvn_dirty_lag_test_handoff *handoff = fence ? lag_test_busy(r, index, fence, old) : NULL;
#endif
            if (fence) {
#ifdef TOTK_VK_TESTS
                if (handoff) atomic_store_explicit(&handoff->rejected, 1, memory_order_release);
#endif
                return 0;
            }
            lag_gate_wait(u, r, index, 1); old = atomic_load_explicit(&u->gate, memory_order_acquire); continue;
        }
        uint64_t ticket = old >> 2;
        if ((old & LAG_STATE_MASK) == LAG_ARMED) {
            /* True duplicates only load the gate: no CAS, page scan, clock or global counter update.
               A newer ticket is published by one CAS; retrying after a competing update preserves its maximum. */
            if (ticket >= fence) return 1;
            uint64_t armed = (fence << 2) | LAG_ARMED;
            if (!atomic_compare_exchange_weak_explicit(&u->gate, &old, armed, memory_order_acq_rel, memory_order_acquire)) continue;
            stats->updates++; return 1;
        }
        if (ticket < fence) ticket = fence;
        uint64_t busy = (old & ~(uint64_t)LAG_STATE_MASK) | LAG_BUSY;
        if (!atomic_compare_exchange_weak_explicit(&u->gate, &old, busy, memory_order_acquire, memory_order_acquire)) continue;
        uintptr_t first = index * g_unit, last = first + g_unit, np = range_pages(r); if (last > np) last = np;
        if (!vma_charge(u)) { lag_unlock(r, index, old & ~(uint64_t)LAG_STATE_MASK); return 0; }
        int ok = protect_span(r, first, last - 1, !fence, &stats->mp, 0, fence ? NVN_DIRTY_MP_GUARD : NVN_DIRTY_MP_SNAPSHOT);
        lag_unlock(r, index, ok ? (ticket << 2) | LAG_ARMED : old & ~(uint64_t)LAG_STATE_MASK);
        if (ok) stats->rearms++;
        return ok;
    }
}
/* All gates are acquired without waiting, and all permission changes precede ARMED publication.
   A bounded run shares one mprotect across adjacent units; already-armed units retain their load/CAS fast path. */
static int lag_protect_run(range_t *r, uintptr_t first, uintptr_t count, uint64_t fence, lag_registration_stats *stats) {
    for (uintptr_t at = first, end = first + count; at < end;) {
        uint64_t gate[64]; uintptr_t n = 0;
        while (at + n < end && n < sizeof gate / sizeof *gate) {
            lag_unit_t *u = &r->lag[at + n];
            uint64_t old = atomic_load_explicit(&u->gate, memory_order_acquire);
            if ((old & LAG_STATE_MASK) != LAG_NONE) break;
            uint64_t busy = (old & ~(uint64_t)LAG_STATE_MASK) | LAG_BUSY;
            if (!atomic_compare_exchange_strong_explicit(&u->gate, &old, busy, memory_order_acquire, memory_order_acquire)) break;
            gate[n++] = old;
            if (!vma_charge(u)) {
                for (uintptr_t k = 0; k < n; k++) lag_unlock(r, at + k, gate[k]);
                return 0;
            }
        }
        if (!n) { if (!lag_protect_unit(r, at++, fence, stats)) return 0; continue; }
        uintptr_t p = at * g_unit, q = (at + n) * g_unit, pages = range_pages(r); if (q > pages) q = pages;
        int ok = protect_span(r, p, q - 1, 0, &stats->mp, 0, fence ? NVN_DIRTY_MP_GUARD : NVN_DIRTY_MP_SNAPSHOT);
        for (uintptr_t k = 0; k < n; k++) {
            uint64_t ticket = gate[k] >> 2; if (ticket < fence) ticket = fence;
            lag_unlock(r, at + k, ok ? (ticket << 2) | LAG_ARMED : gate[k]);
        }
        if (!ok) return 0;
        stats->rearms += n; at += n;
    }
    return 1;
}
/* Interval diagnostics do not serialize the gate. Unchanged bits/sizes only load; top may concurrently reset them,
   so use CAS rather than a load/store pair that could overwrite another registrar's bits or larger descriptor. */
static void lag_note_registration(lag_unit_t *u, unsigned reason, size_t range_size) {
    if (reason) {
        unsigned old = atomic_load_explicit(&u->reasons, memory_order_relaxed);
        while ((old | reason) != old &&
               !atomic_compare_exchange_weak_explicit(&u->reasons, &old, old | reason, memory_order_relaxed, memory_order_relaxed)) {}
    }
    if (range_size) {
        size_t old = atomic_load_explicit(&u->max_range, memory_order_relaxed);
        while (old < range_size &&
               !atomic_compare_exchange_weak_explicit(&u->max_range, &old, range_size, memory_order_relaxed, memory_order_relaxed)) {}
    }
}
/* Dense retained references make registration read-only and shardable. The open-address table is used only while
   collecting/merging; a small sorted range cache avoids find()'s pool-table scan on every repeated descriptor.
   Neither table assumes that a pool begins on a global protection-unit boundary. */
typedef struct {
    range_t *range; uint64_t identity; uintptr_t index;
    size_t extent, max_range; unsigned reasons;
} lag_batch_unit;
typedef struct { range_t *range; uint64_t identity; uintptr_t base, end; } lag_batch_range;
typedef struct {
    lag_batch_unit *units; size_t *slots; size_t count, capacity;
    lag_batch_range *ranges; size_t nranges, range_capacity, last_range;
    uint64_t bytes; int failed;
} lag_batch_impl;
/* A failure before the first allocation must remain visible to register, even if a caller ignores add's result. */
#define LAG_BATCH_FAILED ((void *)(uintptr_t)1)
static int lag_batch_fail(nvn_dirty_lag_batch *batch) {
    if (batch) {
        if (batch->impl && batch->impl != LAG_BATCH_FAILED) ((lag_batch_impl *)batch->impl)->failed = 1;
        else batch->impl = LAG_BATCH_FAILED;
    }
    return 0;
}
static lag_batch_impl *lag_batch_get(nvn_dirty_lag_batch *batch) {
    if (!batch || batch->impl == LAG_BATCH_FAILED) return NULL;
    if (!batch->impl) batch->impl = calloc(1, sizeof(lag_batch_impl));
    return batch->impl;
}
static int lag_batch_live(const range_t *range, uint64_t identity) {
    return atomic_load_explicit(&range->live, memory_order_acquire) &&
           atomic_load_explicit(&range->identity, memory_order_acquire) == identity;
}
static size_t lag_batch_hash(const lag_batch_unit *unit) {
    uint64_t h = (uint64_t)(unit->range - g_r) ^ (unit->identity * UINT64_C(0x9e3779b97f4a7c15)) ^
                 ((uint64_t)unit->index * UINT64_C(0xbf58476d1ce4e5b9));
    h ^= h >> 30; h *= UINT64_C(0xbf58476d1ce4e5b9); h ^= h >> 27;
    return (size_t)(h ^ (h >> 31));
}
static size_t lag_batch_slot(const lag_batch_impl *batch, const lag_batch_unit *unit) {
    size_t mask = batch->capacity * 2 - 1, slot = lag_batch_hash(unit) & mask;
    while (batch->slots[slot]) {
        const lag_batch_unit *old = &batch->units[batch->slots[slot] - 1];
        if (old->range == unit->range && old->identity == unit->identity && old->index == unit->index) break;
        slot = (slot + 1) & mask;
    }
    return slot;
}
static int lag_batch_grow(lag_batch_impl *batch) {
    size_t capacity = batch->capacity ? batch->capacity * 2 : 32;
    if (capacity < batch->capacity || capacity > SIZE_MAX / (2 * sizeof(size_t)) ||
        capacity > SIZE_MAX / sizeof(lag_batch_unit)) return 0;
    size_t *slots = calloc(capacity * 2, sizeof *slots);
    if (!slots) return 0;
    lag_batch_unit *units = realloc(batch->units, capacity * sizeof *units);
    if (!units) { free(slots); return 0; }
    free(batch->slots); batch->slots = slots; batch->units = units; batch->capacity = capacity;
    for (size_t i = 0; i < batch->count; i++) batch->slots[lag_batch_slot(batch, &batch->units[i])] = i + 1;
    return 1;
}
static int lag_batch_insert(lag_batch_impl *batch, const lag_batch_unit *unit) {
    size_t slot = batch->capacity ? lag_batch_slot(batch, unit) : 0;
    if (batch->capacity && batch->slots[slot]) {
        lag_batch_unit *old = &batch->units[batch->slots[slot] - 1];
        old->reasons |= unit->reasons;
        if (old->max_range < unit->max_range) old->max_range = unit->max_range;
        return 1;
    }
    if (batch->count == batch->capacity) {
        if (!lag_batch_grow(batch)) return 0;
        slot = lag_batch_slot(batch, unit);
    }
    batch->units[batch->count] = *unit; batch->slots[slot] = ++batch->count; batch->bytes += unit->extent;
    return 1;
}
static lag_batch_range *lag_batch_find_range(lag_batch_impl *batch, uintptr_t addr) {
    if (batch->nranges) {
        lag_batch_range *last = &batch->ranges[batch->last_range];
        if (addr >= last->base && addr < last->end) return lag_batch_live(last->range, last->identity) ? last : NULL;
    }
    size_t lo = 0, hi = batch->nranges;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (batch->ranges[mid].base <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo && addr < batch->ranges[lo - 1].end) {
        batch->last_range = lo - 1;
        lag_batch_range *hit = &batch->ranges[lo - 1];
        return lag_batch_live(hit->range, hit->identity) ? hit : NULL;
    }
    range_t *range = find_range(addr, 0);
    if (!range || !range->lag) return NULL;
    uint64_t identity = atomic_load_explicit(&range->identity, memory_order_acquire);
    if (batch->nranges == batch->range_capacity) {
        size_t capacity = batch->range_capacity ? batch->range_capacity * 2 : 8;
        if (capacity < batch->range_capacity || capacity > SIZE_MAX / sizeof(lag_batch_range)) return NULL;
        lag_batch_range *ranges = realloc(batch->ranges, capacity * sizeof *ranges);
        if (!ranges) return NULL;
        batch->ranges = ranges; batch->range_capacity = capacity;
    }
    memmove(batch->ranges + lo + 1, batch->ranges + lo, (batch->nranges - lo) * sizeof *batch->ranges);
    batch->ranges[lo] = (lag_batch_range){range, identity, range->base, range->end};
    batch->nranges++; batch->last_range = lo;
    return &batch->ranges[lo];
}
int nvn_dirty_lag_batch_add_before(nvn_dirty_lag_batch *batch, const void *addr, size_t len, unsigned reason,
                                   size_t max_range, uint64_t epoch) {
    if (!g_lag_enabled || !len) return lag_batch_fail(batch);
    lag_batch_impl *impl = lag_batch_get(batch);
    if (!impl || impl->failed) return lag_batch_fail(batch);
    uintptr_t a = (uintptr_t)addr;
    lag_batch_range *range = lag_batch_find_range(impl, a);
    if (!range || range->identity > epoch || len > range->end - a) return lag_batch_fail(batch);
    uintptr_t first = ((a - range->base) >> g_shift) / g_unit;
    uintptr_t last = ((a + len - 1 - range->base) >> g_shift) / g_unit;
    for (uintptr_t i = first; i <= last; i++) {
        lag_batch_unit unit = {range->range, range->identity, i, unit_len(range->range, i * g_unit, g_unit), max_range, reason};
        if (!lag_batch_insert(impl, &unit)) return lag_batch_fail(batch);
    }
    return 1;
}
int nvn_dirty_lag_batch_add(nvn_dirty_lag_batch *batch, const void *addr, size_t len, unsigned reason, size_t max_range) {
    return nvn_dirty_lag_batch_add_before(batch, addr, len, reason, max_range, UINT64_MAX);
}
int nvn_dirty_lag_batch_merge(nvn_dirty_lag_batch *dst, const nvn_dirty_lag_batch *src) {
    if (!g_lag_enabled || !dst || !src || src->impl == LAG_BATCH_FAILED) return lag_batch_fail(dst);
    const lag_batch_impl *source = src->impl;
    if (source && source->failed) return lag_batch_fail(dst);
    if (dst->impl == LAG_BATCH_FAILED || (dst->impl && ((lag_batch_impl *)dst->impl)->failed)) return 0;
    if (!source || !source->count) return 1;
    lag_batch_impl *target = lag_batch_get(dst);
    if (!target) return lag_batch_fail(dst);
    for (size_t i = 0; i < source->count; i++) {
        const lag_batch_unit *unit = &source->units[i];
        if (!lag_batch_live(unit->range, unit->identity) ||
            (source != target && !lag_batch_insert(target, unit))) return lag_batch_fail(dst);
    }
    return 1;
}
static int lag_batch_order(const void *left, const void *right) {
    const lag_batch_unit *a = left, *b = right;
    if (a->range != b->range) return a->range < b->range ? -1 : 1;
    if (a->identity != b->identity) return a->identity < b->identity ? -1 : 1;
    return a->index < b->index ? -1 : a->index != b->index;
}
int nvn_dirty_lag_batch_seal(nvn_dirty_lag_batch *batch) {
    if (!batch || batch->impl == LAG_BATCH_FAILED) return 0;
    lag_batch_impl *impl = batch->impl; if (!impl) return 1;
    if (impl->failed) return 0;
    if (impl->count < 2) return 1;
    qsort(impl->units, impl->count, sizeof *impl->units, lag_batch_order);
    if (impl->count) memset(impl->slots, 0, impl->capacity * 2 * sizeof *impl->slots);
    for (size_t i = 0; i < impl->count; i++) impl->slots[lag_batch_slot(impl, &impl->units[i])] = i + 1;
    return 1;
}
int nvn_dirty_lag_batch_register(const nvn_dirty_lag_batch *batch, uint64_t ticket, unsigned shard, unsigned nshards) {
    vma_refresh_changed();
    if (nvn_dirty_lag_vma_pressure()) return 0;
    if (!g_lag_enabled || !batch || !ticket || ticket > LAG_MAX_TICKET || !nshards || shard >= nshards ||
        batch->impl == LAG_BATCH_FAILED) return 0;
    const lag_batch_impl *impl = batch->impl;
    if (!impl) return 1;
    if (impl->failed) return 0;
    lag_registration_stats stats = {0}; int ok = 1;
    size_t start = impl->count / nshards * shard + (impl->count % nshards) * shard / nshards;
    size_t end = impl->count / nshards * (shard + 1) + (impl->count % nshards) * (shard + 1) / nshards;
    for (size_t i = start; i < end;) {
        const lag_batch_unit *unit = &impl->units[i]; size_t next = i + 1;
        if (!lag_batch_live(unit->range, unit->identity)) { ok = 0; break; }
        while (next < end && impl->units[next].range == unit->range && impl->units[next].identity == unit->identity &&
               impl->units[next].index == impl->units[next - 1].index + 1) next++;
        if (!lag_protect_run(unit->range, unit->index, next - i, ticket, &stats)) { ok = 0; break; }
        for (; i < next; i++) {
            const lag_batch_unit *u = &impl->units[i];
            lag_note_registration(&u->range->lag[u->index], u->reasons, u->max_range);
        }
    }
    lag_registration_flush(&stats);
    return ok;
}
void nvn_dirty_lag_batch_reset(nvn_dirty_lag_batch *batch) {
    if (!batch) return;
    if (batch->impl == LAG_BATCH_FAILED) { batch->impl = NULL; return; }
    lag_batch_impl *impl = batch->impl;
    if (!impl) return;
    if (impl->count) memset(impl->slots, 0, impl->capacity * 2 * sizeof *impl->slots);
    impl->count = impl->nranges = impl->last_range = 0; impl->bytes = 0; impl->failed = 0;
}
void nvn_dirty_lag_batch_destroy(nvn_dirty_lag_batch *batch) {
    if (!batch) return;
    if (batch->impl && batch->impl != LAG_BATCH_FAILED) {
        lag_batch_impl *impl = batch->impl;
        free(impl->units); free(impl->slots); free(impl->ranges); free(impl);
    }
    batch->impl = NULL;
}
size_t nvn_dirty_lag_batch_count(const nvn_dirty_lag_batch *batch) {
    return batch && batch->impl && batch->impl != LAG_BATCH_FAILED ? ((const lag_batch_impl *)batch->impl)->count : 0;
}
uint64_t nvn_dirty_lag_batch_bytes(const nvn_dirty_lag_batch *batch) {
    return batch && batch->impl && batch->impl != LAG_BATCH_FAILED ? ((const lag_batch_impl *)batch->impl)->bytes : 0;
}
static void protect_pages(const void *addr, size_t len, int exact) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    if (r->lag) {
        for (uintptr_t u = p / g_unit; u <= q / g_unit; u++) {
            uintptr_t first = u * g_unit, last = first + g_unit - 1, pages = range_pages(r);
            if (exact) { if (first < p) first = p; if (last > q) last = q; }
            else if (last >= pages) last = pages - 1;
            uint64_t gate = lag_lock_pages(r, u, 1, first, last + 1, exact ? WATCH_EXACT : WATCH_TEXTURE);
            int ok = protect_span(r, first, last, 1, NULL, 1, exact ? NVN_DIRTY_MP_EXACT : NVN_DIRTY_MP_TEXTURE);
            /* Only a complete ordinary watch certifies the unit, and never becomes a new GPU guard on pressure. */
            uint64_t state = !exact && !nvn_dirty_lag_vma_pressure() ? LAG_ARMED : gate & LAG_STATE_MASK;
            lag_unlock(r, u, ok ? (gate & ~(uint64_t)LAG_STATE_MASK) | state : gate & ~(uint64_t)LAG_STATE_MASK);
            if (ok && !exact && state == LAG_ARMED && (gate & LAG_STATE_MASK) != LAG_ARMED)
                atomic_fetch_add_explicit(&g_lag_rearms, 1, memory_order_relaxed);
            if (!ok) die("dirty baseline watch mprotect failed");
        }
        return;
    }
    if (exact) for (uintptr_t i = p; i <= q; i++) atomic_store(&r->exact[i], WATCH_EXACT);
    else if (g_unit > 1) { uintptr_t np = range_pages(r); p = p / g_unit * g_unit; q = (q / g_unit + 1) * g_unit - 1; if (q >= np) q = np - 1; }
    protect_span(r, p, q, 1, NULL, exact, exact ? NVN_DIRTY_MP_EXACT : NVN_DIRTY_MP_TEXTURE);
}
void nvn_dirty_protect(const void *addr, size_t len) { protect_pages(addr, len, 0); }
void nvn_dirty_protect_exact(const void *addr, size_t len) { protect_pages(addr, len, 1); }
void *nvn_dirty_wb_watch(const void *addr, size_t len, uint64_t stamp) {
    if (!len || !nvn_dirty_tracked(addr, len)) return NULL;
    range_t *r = find((uintptr_t)addr);
    for (unsigned i = 0; i < WB_WATCH_MAX; i++) {
        wb_watch *w = &g_wb_watch[i]; unsigned state = WB_FREE;
        if (!atomic_compare_exchange_strong(&w->state, &state, WB_INIT)) continue;
        uintptr_t first = ((uintptr_t)addr - r->base) >> g_shift, last = ((uintptr_t)addr + len - 1 - r->base) >> g_shift;
        atomic_store(&w->range, r); atomic_store(&w->identity, atomic_load(&r->identity));
        atomic_store(&w->first, first); atomic_store(&w->last, last);
        atomic_store(&w->cpu_address, 0);
        unsigned used = atomic_load(&g_wb_watch_used);
        while (used <= i && !atomic_compare_exchange_weak(&g_wb_watch_used, &used, i + 1)) {}
        /* Publish the group before arming pages: a fault during protection must cancel the rendering too.
           The backend takes its stamp before this call, so later generation stores conservatively reject it. */
        atomic_store(&w->arming, 1);
        atomic_store_explicit(&w->state, WB_ARMED, memory_order_release);
        nvn_dirty_protect_exact(addr, len);
        atomic_store_explicit(&w->arming, 0, memory_order_release);
        /* A CPU fault on an already-RO baseline page can precede publication of its exact bit. Do not leave
           that CPU-owned storage re-protected by the tail of this arm operation. */
        if (nvn_dirty_since(addr, len, stamp)) wb_unprotect_write(r, first, 1, 0);
        return w;
    }
    return NULL; /* bounded metadata exhausted: caller must not claim write-watch authority */
}
int nvn_dirty_wb_written(const void *watch) {
    const wb_watch *w = watch;
    return !w || atomic_load_explicit(&w->state, memory_order_acquire) != WB_ARMED;
}
uintptr_t nvn_dirty_wb_cpu_address(const void *watch) {
    const wb_watch *w = watch;
    return w ? atomic_load_explicit(&w->cpu_address, memory_order_acquire) : 0;
}
void nvn_dirty_wb_unwatch(void *watch) {
    wb_watch *w = watch; if (!w) return;
    unsigned state = atomic_load_explicit(&w->state, memory_order_acquire);
    for (;;) {
        if (state == WB_FAULT) {
            if (atomic_compare_exchange_weak(&w->state, &state, WB_RETIRED)) return;
        } else {
            /* Own the slot until its exact bits are gone. Do not make actual GPU-reader pages writable here.
               A completed/abandoned WB watch must not leave exact exclusions on ordinary texture watches. */
            if (!atomic_compare_exchange_weak(&w->state, &state, WB_INIT)) continue;
            range_t *r = atomic_load(&w->range);
            if (atomic_load(&r->live) && atomic_load(&r->identity) == atomic_load(&w->identity))
                for (uintptr_t p = atomic_load(&w->first), last = atomic_load(&w->last); p <= last; p++)
                    atomic_fetch_and(&r->exact[p], (uint8_t)~WATCH_EXACT);
            atomic_store_explicit(&w->state, WB_FREE, memory_order_release);
            return;
        }
    }
}
void nvn_dirty_wb_stats(uint64_t *faults, uint64_t *preps) {
    *faults = atomic_load(&g_wb_faults); *preps = atomic_load(&g_wb_preps);
}
/* Use the same whole-unit bounds as normal dirty protection, clipped at the pool's final system page. */
int nvn_dirty_lag_bounds(const void *addr, size_t len, uintptr_t *lo, uintptr_t *hi) {
    if (!len || !nvn_dirty_tracked(addr, len)) return 0;
    range_t *r = find((uintptr_t)addr); if (!r->lag) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift, q = ((uintptr_t)addr + len - 1 - r->base) >> g_shift;
    p = p / g_unit * g_unit; q = (q / g_unit + 1) * g_unit;
    uintptr_t np = range_pages(r); if (q > np) q = np;
    *lo = unit_addr(r, p); *hi = unit_addr(r, q);
    return 1;
}
int nvn_dirty_lag_register(const void *addr, size_t len, uint64_t fence, unsigned reason, size_t range_size) {
    if (nvn_dirty_lag_vma_pressure()) return 0;
    if (!len || fence > LAG_MAX_TICKET) return 0;
    range_t *r = find((uintptr_t)addr);
    if (!r || !r->lag || len > r->end - (uintptr_t)addr) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift, q = ((uintptr_t)addr + len - 1 - r->base) >> g_shift;
    lag_registration_stats stats = {0};
    uintptr_t first = p / g_unit, last = q / g_unit;
    /* Native snapshot watches and direct guards need the same contiguous-unit batching as frozen
       submission batches. Keep the one-unit fast path and the existing 64-gate ownership bound. */
    int ok = first == last ? lag_protect_unit(r, first, fence, &stats) :
                            lag_protect_run(r, first, last - first + 1, fence, &stats);
    if (ok) for (uintptr_t u = first; u <= last; u++) lag_note_registration(&r->lag[u], reason, range_size);
    lag_registration_flush(&stats);
    return ok;
}
int nvn_dirty_lag_protect(const void *addr, size_t len, uint64_t fence) {
    return nvn_dirty_lag_register(addr, len, fence, 0, 0);
}
int nvn_dirty_lag_registered(const void *addr, size_t len, uint64_t fence) {
    if (nvn_dirty_lag_vma_pressure()) return 0;
    if (!len || fence > LAG_MAX_TICKET) return 0;
    range_t *r = find_range((uintptr_t)addr, 0);
    if (!r || !r->lag || len > r->end - (uintptr_t)addr) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift, q = ((uintptr_t)addr + len - 1 - r->base) >> g_shift;
    for (uintptr_t u = p / g_unit; u <= q / g_unit; u++) {
        uint64_t gate = atomic_load_explicit(&r->lag[u].gate, memory_order_acquire);
        if ((gate & LAG_STATE_MASK) != LAG_ARMED || (gate >> 2) < fence) return 0;
    }
    return 1;
}
void nvn_dirty_lag_watch(const void *addr, size_t len) { nvn_dirty_lag_protect(addr, len, 0); }
static int lag_hot_range(range_t *r, uintptr_t a, uintptr_t e, uint64_t frame) {
    if (!r->lag || e <= r->base || a >= r->end) return 0;
    if (a < r->base) a = r->base;
    if (e > r->end) e = r->end;
    uintptr_t p = (a - r->base) >> g_shift, q = (e - 1 - r->base) >> g_shift;
    int hot = 0;
    for (uintptr_t u = p / g_unit; u <= q / g_unit; u++) hot |= lag_hot_age(&r->lag[u], frame);
    return hot;
}
int nvn_dirty_lag_hot(const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr;
    if (!g_lag_enabled || !len || len > UINTPTR_MAX - a) return 0;
    uintptr_t e = a + len; uint64_t frame = atomic_load_explicit(&g_lag_frame, memory_order_acquire);
    range_t *r = find(a);
    if (r && e <= r->end) return lag_hot_range(r, a, e, frame);
    /* An alias/ring query may straddle tracked pools or start outside one. Inspect every overlapping live range.
       A query overlapping no live range at epoch E overlaps none until a range is added (which bumps the epoch),
       and an empty scan ages no unit, so it is remembered per thread instead of rescanned on every lag read. */
    enum { NI = 256 };
    static __thread struct { uintptr_t a, e; uint64_t epoch; } none[NI];
    uint64_t epoch = atomic_load_explicit(&g_range_epoch, memory_order_acquire);
    unsigned k = (unsigned)((a >> 12) ^ (a >> 24) ^ len) & (NI - 1);
    if (none[k].a == a && none[k].e == e && none[k].epoch == epoch) return 0;
    int hot = 0, any = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && e > g_r[i].base && a < g_r[i].end) {
        any = 1;
        hot |= lag_hot_range(&g_r[i], a, e, frame);
    }
    if (!any) { none[k].a = a; none[k].e = e; none[k].epoch = epoch; }
    return hot;
}
int nvn_dirty_lag_top(nvn_dirty_lag_unit *out, int max) {
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g_slot_mu);   /* normal-thread diagnostics: keep pool slot identities stable while scanning */
    int count = 0, n = atomic_load(&g_nr); uint64_t frame = atomic_load_explicit(&g_lag_frame, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || !r->lag) continue;
        uintptr_t nu = (range_pages(r) + g_unit - 1) / g_unit;
        for (uintptr_t u = 0; u < nu; u++) {
            lag_unit_t *unit = &r->lag[u];
            uint64_t faults = atomic_exchange_explicit(&unit->faults, 0, memory_order_relaxed);
            uint64_t waits = atomic_exchange_explicit(&unit->waits, 0, memory_order_relaxed);
            unsigned reasons = atomic_exchange_explicit(&unit->reasons, 0, memory_order_relaxed);
            size_t max_range = atomic_exchange_explicit(&unit->max_range, 0, memory_order_relaxed);
            int hot = lag_hot_age(unit, frame);
            if (!faults && !waits) continue;
            nvn_dirty_lag_unit item = { unit_addr(r, u * g_unit), unit_len(r, u * g_unit, g_unit),
                faults, waits, atomic_load_explicit(&unit->last_frame, memory_order_acquire), hot, reasons, max_range };
            int at = 0;
            while (at < count && (out[at].faults > faults || (out[at].faults == faults &&
                   (out[at].waits > waits || (out[at].waits == waits && out[at].addr < item.addr))))) at++;
            if (at >= max) continue;
            if (count < max) count++;
            for (int k = count - 1; k > at; k--) out[k] = out[k - 1];
            out[at] = item;
        }
    }
    pthread_mutex_unlock(&g_slot_mu);
    return count;
}
uint64_t nvn_dirty_lag_danger_marks(void) { return atomic_load_explicit(&g_danger_marks, memory_order_acquire); }
void nvn_dirty_lag_danger_stats(uint64_t *waits, uint64_t *hots) {
    *waits = atomic_load_explicit(&g_danger_waits, memory_order_relaxed);
    *hots = atomic_load_explicit(&g_danger_hots, memory_order_relaxed);
}
int nvn_dirty_lag_danger_collect(uintptr_t *lo, uintptr_t *hi, int max) {
    pthread_mutex_lock(&g_slot_mu);   /* stable slot identities while scanning, as nvn_dirty_lag_top */
    int count = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || !r->lag) continue;
        uintptr_t nu = (range_pages(r) + g_unit - 1) / g_unit;
        for (uintptr_t u = 0; u < nu; u++) {
            if (!atomic_load_explicit(&r->lag[u].danger, memory_order_relaxed)) continue;
            uintptr_t a = unit_addr(r, u * g_unit), z = a + unit_len(r, u * g_unit, g_unit);
            if (count && count <= max && hi[count - 1] == a) { hi[count - 1] = z; continue; }   /* adjacent units */
            if (count < max) { lo[count] = a; hi[count] = z; }
            count++;
        }
    }
    pthread_mutex_unlock(&g_slot_mu);
    return count;
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = find((uintptr_t)addr);
    if (!r || len > r->end - (uintptr_t)addr) return 1; /* untracked remains conservatively dirty */
    if (atomic_load(&r->latest) <= stamp) return 0;   // TOTK: nothing in the whole pool was written since the stamp (no per-page scan)
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // 上传前最后一次写的代号 ≤ 戳；之后的写必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
/* Reporting can precede the first dirty pool (module sealing already uses the raw wrapper). */
static mach_timebase_info_data_t g_mp_report_tb;
static pthread_once_t g_mp_report_once = PTHREAD_ONCE_INIT;
static void mp_report_clock_init(void) { mach_timebase_info(&g_mp_report_tb); }
static nvn_dirty_mp_count mp_reason_read(const mp_thread *t, unsigned reason) {
    uint64_t ticks = atomic_load_explicit(&t->reason[reason].ticks, memory_order_relaxed);
    return (nvn_dirty_mp_count){
        atomic_load_explicit(&t->reason[reason].calls, memory_order_relaxed),
        atomic_load_explicit(&t->reason[reason].pages, memory_order_relaxed),
        ticks * g_mp_report_tb.numer / g_mp_report_tb.denom};
}
void nvn_dirty_mp_reason_stats(nvn_dirty_mp_count out[NVN_DIRTY_MP_REASON_COUNT]) {
    pthread_once(&g_mp_report_once, mp_report_clock_init);
    for (unsigned reason = 0; reason < NVN_DIRTY_MP_REASON_COUNT; reason++) out[reason] = (nvn_dirty_mp_count){0};
    for (unsigned i = 0; i < MP_THREADS; i++) {
        const mp_thread *t = &g_mp_threads[i];
        if (i != MP_THREADS - 1 && !atomic_load_explicit(&t->live, memory_order_acquire)) continue;
        for (unsigned reason = 0; reason < NVN_DIRTY_MP_REASON_COUNT; reason++) {
            nvn_dirty_mp_count c = mp_reason_read(t, reason);
            out[reason].calls += c.calls; out[reason].pages += c.pages; out[reason].ns += c.ns;
        }
    }
}
void nvn_dirty_mp_report(uint64_t frame) {
    static uint64_t last_frame;
    static nvn_dirty_mp_count previous[MP_THREADS][NVN_DIRTY_MP_REASON_COUNT];
    static const char *const names[NVN_DIRTY_MP_REASON_COUNT] = {
        "guard-rearm", "snapshot-watch", "texture-watch", "exact-watch", "fault-unprotect",
        "host-unprotect", "gpu-unprotect", "gpu-restore", "pressure-drain",
        "vm-commit", "vm-decommit", "stack-permission", "module-permission", "diagnostic-watch"
    };
    if (frame <= last_frame) return;
    pthread_once(&g_mp_report_once, mp_report_clock_init);
    uint64_t frames = frame - last_frame; last_frame = frame;
    for (unsigned i = 0; i < MP_THREADS; i++) {
        const mp_thread *t = &g_mp_threads[i];
        if (i != MP_THREADS - 1 && !atomic_load_explicit(&t->live, memory_order_acquire)) continue;
        for (unsigned reason = 0; reason < NVN_DIRTY_MP_REASON_COUNT; reason++) {
            nvn_dirty_mp_count c = mp_reason_read(t, reason), p = previous[i][reason];
            previous[i][reason] = c;
            if (c.calls == p.calls && c.pages == p.pages && c.ns == p.ns) continue;
            LOG("[dirty-mprotect] frame=%llu frames=%llu tid=%llu thread=%s reason=%s calls=%llu pages=%llu ms=%.3f calls/frame=%.3f pages/frame=%.3f",
                (unsigned long long)frame, (unsigned long long)frames, (unsigned long long)t->tid,
                i == MP_THREADS - 1 ? "overflow" : t->name[0] ? t->name : "unknown", names[reason],
                (unsigned long long)(c.calls - p.calls), (unsigned long long)(c.pages - p.pages), (c.ns - p.ns) / 1e6,
                (double)(c.calls - p.calls) / frames, (double)(c.pages - p.pages) / frames);
        }
    }
}

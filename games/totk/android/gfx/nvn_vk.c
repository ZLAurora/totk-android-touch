/* The NVN replay interface (third_party/d2r_gfx/gfx/nvn/nvn_mtl.h) on Vulkan: Linux / Android. This file: the device,
   memory pools, submissions and their completion, and the command walker; textures, shaders and drawing are in
   vk_res.c, vk_shader.c and vk_draw.c (vk_int.h). android/VULKAN.md has the plan and the reasons.

   Every effect the game can observe of GPU completion (event values, counter reports, fences) happens only after the
   GPU has really finished the submission, as nvn_mtl.m does on Metal.

   Memory pools stay zero-copy: the game and the GPU use the same pages. Three ways, chosen at start
   (TOTK_VK_POOLS=host|export|heap forces one):
     host    import the guest's pages as device memory           (VK_EXT_external_memory_host; not on Adreno drivers)
     export  device memory exported as a dma-buf, mmap'd over the pool's guest address range (Turnip)
     heap    a dma-buf from /dev/dma_heap, imported, mmap'd the same way
   With export / heap, registering a pool moves its contents into the dma-buf and maps the dma-buf at the same address,
   so the game keeps its pointers; removing the pool moves the contents back into ordinary shared memory. Virtual pools
   (totk_vpool.c) alias physical pool pages with a second mmap of the dma-buf (mtl_pool_alias).

   The driver is the system one, or the file named by TOTK_VK_DRIVER (a Turnip build, android/gfx/vk_load.h). */
#include <malloc.h>
#include "vk_int.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_framegen_qcom.h"
#include "vk_framegen_lsfg.h"
#include "framegen_dispatch_trace.h"
#include "framegen_adaptive_defaults.h"
#include "vk_timestamp.h"
#include "vk_gpu_time.h"
#include "vk_present_policy.h"
#include "nvn_dirty.h"
#include "totk_lifelog.h"
#include "../../runtime/frame_timeline.h"
#include "../../runtime/frame_fence.h"
#include "../../runtime/present_predict.h"
#include "../../runtime/fps_auto_runtime.h"
#include "../../runtime/android_perf.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* Implemented by the adaptive frame-generation backend. */
extern void vk_fg_snapshot_pressure_trace(unsigned owner_states,unsigned generation_states,
        unsigned cancelled,int append);

#define VK_DEF(name) PFN_##name name;
VK_DEVICE_FUNCS(VK_DEF)
#undef VK_DEF
vk_dev V;
#include "vk_sections.inc"
#define g_dev V.dev
static _Atomic uint64_t g_memory_live[VK_MEMORY_PURPOSES], g_memory_created[VK_MEMORY_PURPOSES];
VkResult vk_memory_allocate(const VkMemoryAllocateInfo *info, VkDeviceMemory *memory, vk_memory_purpose purpose) {
    VkResult result = vkAllocateMemory(V.dev, info, NULL, memory);
    if (!result) {
        atomic_fetch_add_explicit(&g_memory_live[purpose], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_memory_created[purpose], 1, memory_order_relaxed);
    }
    return result;
}
void vk_memory_release(VkDeviceMemory memory, vk_memory_purpose purpose) {
    if (!memory) return;
    vkFreeMemory(V.dev, memory, NULL);
    atomic_fetch_sub_explicit(&g_memory_live[purpose], 1, memory_order_relaxed);
}
void vk_memory_counts(uint64_t live[VK_MEMORY_PURPOSES]) {
    for (int i = 0; i < VK_MEMORY_PURPOSES; i++) live[i] = atomic_load_explicit(&g_memory_live[i], memory_order_relaxed);
}
static void memory_objects_report(void) {
    uint64_t n[VK_MEMORY_PURPOSES], made = 0; vk_memory_counts(n);
    for (int i = 0; i < VK_MEMORY_PURPOSES; i++) made += atomic_exchange_explicit(&g_memory_created[i], 0, memory_order_relaxed);
    LOGV("memory Vulkan objects: live pool/slot/snapshot/image-block/image-dedicated/present/readback/zero/diagnostic/overlay %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; %llu allocations since report (driver-internal BOs excluded)",
        (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3],
        (unsigned long long)n[4], (unsigned long long)n[5], (unsigned long long)n[6], (unsigned long long)n[7],
        (unsigned long long)n[8], (unsigned long long)n[9], (unsigned long long)made);
}

/* ---------- opt-in translation measurements; all mutable state is recording-lock owned ----------
   Hash tables store complete 128-bit keys + byte/op counts, never recording/handle pointers. Hash equality is
   probabilistic byte equality, not proof of equivalent resource contents or Vulkan command-buffer reusability. */
enum { XLATE_HASH_SLOTS = 16384, XLATE_HASH_LIMIT = 8192, XLATE_HASH_BYTES = 64 << 20, XLATE_HASH_OPS = 1 << 20 };
typedef struct { uint64_t h[2], bytes, ops; } xlate_hash;
typedef struct {
    uint64_t frames, frame, submissions, recordings, draws, repeated[2], repeated_draws[2];
    uint64_t covered[2], covered_draws[2], ops, bytes, unknown_ops, incomplete, overflow[2], previous_incomplete[2];
} xlate_record_counts;
static struct {
    xlate_hash table[2][2][XLATE_HASH_SLOTS]; /* frame bank, exact/abstract, open-addressed slot */
    unsigned count[2][2], current, depth;
    int previous_overflow[2], current_overflow[2], repeated[2], covered[2];
    unsigned char deferred[6][3][128];
    xlate_record_counts counts;
} *g_xlate;
int vk_xlate_stats_enabled;
/* Exit reads only a completed published counter snapshot, never mutable streams, uniforms or hash tables.
   This separate mutex cannot recurse into Vulkan or wait for the walker/recording lock during process teardown. */
static pthread_mutex_t g_xlate_publish_mu = PTHREAD_MUTEX_INITIALIZER;
static xlate_record_counts g_xlate_published;
static void xlate_publish(void) {
    pthread_mutex_lock(&g_xlate_publish_mu);
    g_xlate_published = g_xlate->counts;
    pthread_mutex_unlock(&g_xlate_publish_mu);
}
static void xlate_report(const xlate_record_counts *s, int partial) {
    LOGV("xlate recordings: frame=%llu frames=%llu partial=%d submissions=%llu recordings=%llu repeated_exact=%llu repeated_abstract=%llu draws=%llu repeated_draws_exact=%llu repeated_draws_abstract=%llu",
        (unsigned long long)s->frame, (unsigned long long)s->frames, partial, (unsigned long long)s->submissions,
        (unsigned long long)s->recordings, (unsigned long long)s->repeated[0], (unsigned long long)s->repeated[1],
        (unsigned long long)s->draws, (unsigned long long)s->repeated_draws[0], (unsigned long long)s->repeated_draws[1]);
    LOGV("xlate recording coverage: exact=%llu abstract=%llu draws_exact=%llu draws_abstract=%llu ops=%llu bytes=%llu unknown_ops=%llu incomplete=%llu overflow_exact=%llu overflow_abstract=%llu previous_incomplete_exact=%llu previous_incomplete_abstract=%llu; limit=8192 keys/lane/frame, hashes=128-bit+bytes+ops (collisions possible; address abstraction is not semantic equivalence)",
        (unsigned long long)s->covered[0], (unsigned long long)s->covered[1],
        (unsigned long long)s->covered_draws[0], (unsigned long long)s->covered_draws[1],
        (unsigned long long)s->ops, (unsigned long long)s->bytes, (unsigned long long)s->unknown_ops,
        (unsigned long long)s->incomplete, (unsigned long long)s->overflow[0], (unsigned long long)s->overflow[1],
        (unsigned long long)s->previous_incomplete[0], (unsigned long long)s->previous_incomplete[1]);
}
static void xlate_exit(void) {
    xlate_record_counts s;
    pthread_mutex_lock(&g_xlate_publish_mu); s = g_xlate_published; pthread_mutex_unlock(&g_xlate_publish_mu);
    if (s.frames || s.submissions || s.recordings) xlate_report(&s, 1);
}
int vk_xlate_stats_on(void) {
    static int initialized;
    if (!initialized) {
        const char *e = getenv("TOTK_VK_XLATE_STATS");
        vk_xlate_stats_enabled = e && !strcmp(e, "1"); initialized = 1;
        if (vk_xlate_stats_enabled) {
            g_xlate = VK_CALLOC(1, sizeof *g_xlate);
            if (atexit(xlate_exit)) LOGV("xlate recordings: exit summary registration failed");
            LOGV("xlate recordings: enabled; previous-present-frame membership, flattened submitted op bytes including resolved deferred handles; scan limits 64 MiB/1048576 ops per recording; native resource contents/inherited state are not hashed");
        }
    }
    return vk_xlate_stats_enabled;
}
void vk_xlate_recording_stats_frame(uint64_t frame) {
    if (!vk_xlate_stats_on()) return;
    xlate_record_counts *s = &g_xlate->counts;
    s->frame = frame; s->frames++;
    g_xlate->current ^= 1;
    memset(g_xlate->table[g_xlate->current], 0, sizeof g_xlate->table[0]);
    for (unsigned lane = 0; lane < 2; lane++) {
        g_xlate->count[g_xlate->current][lane] = 0;
        g_xlate->previous_overflow[lane] = g_xlate->current_overflow[lane];
        g_xlate->current_overflow[lane] = 0;
    }
    if (s->frames == 300) { xlate_report(s, 0); memset(s, 0, sizeof *s); s->frame = frame; }
    xlate_publish();
}
int vk_xlate_deferred_slot(unsigned stage, unsigned kind, unsigned slot) {
    return g_xlate && stage < 6 && kind < 3 && slot < (kind == 1 ? 32u : 128u) && g_xlate->deferred[stage][kind][slot];
}
static void xlate_deferred_bind(unsigned op, const uint8_t *a) {
    const p_bind_handles *b = (const void *)a;
    if ((unsigned)b->stage >= 6 || b->n < 0 || b->n > 32) return;
    unsigned kind = op == OP_BIND_TEX ? 0 : op == OP_BIND_IMG ? 1 : 2;
    for (int i = 0; i < b->n; i++)
        if ((unsigned)b->first + (unsigned)i < (kind == 1 ? 32u : 128u)) g_xlate->deferred[b->stage][kind][b->first + i] = b->src != 0;
}
static void xlate_hash_bytes(xlate_hash *h, const void *bytes, size_t n) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < n; i++) {
        h->h[0] = (h->h[0] ^ p[i]) * UINT64_C(1099511628211);
        h->h[1] = (h->h[1] ^ p[i]) * UINT64_C(0x9e3779b185ebca87);
        h->h[1] = (h->h[1] << 13) | (h->h[1] >> 51);
    }
}
typedef struct { size_t offset, size; } xlate_address_field;
/* Raw records have no named payload type in nvn_obj.h; these layouts follow their memcpy writers in hle_nvn.c.
   All other offsets/sizes come directly from the shared schema, including variable arrays (never address masks). */
typedef struct { uint64_t address; uint32_t mode, value; } xlate_signal;
typedef struct { uint64_t type, address; } xlate_counter;
typedef struct { uint64_t source, destination, size; } xlate_copy;
typedef struct { uint64_t destination, size; } xlate_update;
static int xlate_address_fields(unsigned op, const uint8_t *a, size_t size, xlate_address_field fields[17]) {
    unsigned n = 0;
#define XLATE_NEED(type) do { if (size < sizeof(type)) return -1; } while (0)
#define XLATE_FIELD(type, member) do { fields[n++] = (xlate_address_field){offsetof(type, member), sizeof(((type *)0)->member)}; } while (0)
#define XLATE_ARRAY(type, member, maximum) do { \
    if (size < offsetof(type, member)) return -1; \
    const type *b = (const void *)a; \
    if (b->n < 0 || b->n > (maximum) || (size_t)b->n > (size - offsetof(type, member)) / sizeof b->member[0]) return -1; \
} while (0)
    switch (op) {
        case OP_END: case OP_BEGIN: break;
        case OP_JUMP: case OP_CALL:
            XLATE_NEED(uint64_t); fields[n++] = (xlate_address_field){0, sizeof(uint64_t)}; break;
        case OP_SIGNAL_EVENT: XLATE_NEED(xlate_signal); XLATE_FIELD(xlate_signal, address); break;
        case OP_REPORT_COUNTER: XLATE_NEED(xlate_counter); XLATE_FIELD(xlate_counter, address); break;
        case OP_COPY_B2B: XLATE_NEED(xlate_copy); XLATE_FIELD(xlate_copy, source); XLATE_FIELD(xlate_copy, destination); break;
        case OP_UPDATE_UB: {
            XLATE_NEED(xlate_update); const xlate_update *u = (const void *)a;
            if (u->size > size - sizeof *u) return -1;
            XLATE_FIELD(xlate_update, destination); break;
        }
        case OP_SET_RT:
            XLATE_NEED(p_set_rt);
            for (unsigned i = 0; i < 8; i++) fields[n++] = (xlate_address_field){
                offsetof(p_set_rt, color) + i * sizeof(((p_set_rt *)0)->color[0]), sizeof(((p_set_rt *)0)->color[0])};
            XLATE_FIELD(p_set_rt, depth); break;
        case OP_BIND_PROGRAM: XLATE_NEED(p_bind_program); XLATE_FIELD(p_bind_program, prog); break;
        case OP_BIND_VBUF: {
            XLATE_ARRAY(p_bind_vbuf, r, 16); const p_bind_vbuf *b = (const void *)a;
            for (int i = 0; i < b->n; i++) fields[n++] = (xlate_address_field){
                offsetof(p_bind_vbuf, r) + (size_t)i * sizeof b->r[0] + offsetof(NVNbufferRange, address), sizeof b->r[0].address};
            break;
        }
        case OP_BIND_UBO: case OP_BIND_SSBO: XLATE_NEED(p_bind_buf); XLATE_FIELD(p_bind_buf, addr); break;
        case OP_BIND_TEX: case OP_BIND_IMG: case OP_BIND_SAMP:
            XLATE_ARRAY(p_bind_handles, h, 32); XLATE_FIELD(p_bind_handles, src); break;
        case OP_DRAW_IDX: XLATE_NEED(p_draw_idx); XLATE_FIELD(p_draw_idx, ibuf); break;
        case OP_DISPATCH: XLATE_NEED(p_dispatch); XLATE_FIELD(p_dispatch, caller); break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: XLATE_NEED(p_set_pool); XLATE_FIELD(p_set_pool, pool); break;
        case OP_COPY_B2T: case OP_COPY_T2B:
            XLATE_NEED(p_copy_b2t); XLATE_FIELD(p_copy_b2t, src); XLATE_FIELD(p_copy_b2t, tex); break;
        case OP_COPY_T2T: XLATE_NEED(p_copy_t2t); XLATE_FIELD(p_copy_t2t, src); XLATE_FIELD(p_copy_t2t, dst); break;
        case OP_CLEAR_TEX: XLATE_NEED(p_clear_tex); XLATE_FIELD(p_clear_tex, tex); break;
        case OP_RENDER_COND: XLATE_NEED(p_render_cond); XLATE_FIELD(p_render_cond, addr); break;
        case OP_CLEAR_COLOR: XLATE_NEED(p_clear_color); break;
        case OP_CLEAR_DS: XLATE_NEED(p_clear_ds); break;
        case OP_ALPHA_REF: XLATE_NEED(p_alpha_ref); break;
        case OP_VIEWPORTS: XLATE_ARRAY(p_viewports, v, 16); break;
        case OP_SCISSOR: XLATE_NEED(p_scissor); break;
        case OP_DEPTH_RANGE: XLATE_NEED(p_depth_range); break;
        case OP_BIND_STATE: XLATE_NEED(p_bind_state); break;
        case OP_BIND_VATTR: XLATE_ARRAY(p_bind_vattr, st, 16); break;
        case OP_BIND_VSTREAM: XLATE_ARRAY(p_bind_vstream, st, 16); break;
        case OP_STENCIL: XLATE_NEED(p_stencil); break;
        case OP_POLY_OFFSET: XLATE_NEED(p_poly_offset); break;
        case OP_DRAW: XLATE_NEED(p_draw); break;
        case OP_BARRIER: XLATE_NEED(int32_t); break;
        case OP_COPY_STRIDE: XLATE_NEED(p_copy_stride); break;
        case OP_BLEND_COLOR: XLATE_NEED(p_blend_color); break;
        case OP_RESET_COUNTER: XLATE_NEED(uint64_t); break;
        case OP_DEPTH_CLAMP: case OP_RENDER_ENABLE: case OP_RASTER_DISCARD: XLATE_NEED(p_flag); break;
        default: return -1;
    }
#undef XLATE_ARRAY
#undef XLATE_FIELD
#undef XLATE_NEED
    return (int)n;
}
static int xlate_hash_equal(const xlate_hash *a, const xlate_hash *b) {
    return a->h[0] == b->h[0] && a->h[1] == b->h[1] && a->bytes == b->bytes && a->ops == b->ops;
}
static unsigned xlate_hash_slot(const xlate_hash *h) {
    uint64_t x = h->h[0] ^ h->h[1] ^ h->bytes ^ h->ops;
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd); x ^= x >> 33;
    return (unsigned)x & (XLATE_HASH_SLOTS - 1);
}
static int xlate_previous(const xlate_hash *key, unsigned lane) {
    const xlate_hash *table = g_xlate->table[g_xlate->current ^ 1][lane];
    unsigned at = xlate_hash_slot(key);
    for (unsigned i = 0; i < XLATE_HASH_SLOTS; i++, at = (at + 1) & (XLATE_HASH_SLOTS - 1)) {
        if (!table[at].bytes) return 0;
        if (xlate_hash_equal(&table[at], key)) return 1;
    }
    return 0;
}
static void xlate_remember(const xlate_hash *key, unsigned lane) {
    unsigned bank = g_xlate->current, at = xlate_hash_slot(key);
    xlate_hash *table = g_xlate->table[bank][lane];
    for (unsigned i = 0; i < XLATE_HASH_SLOTS; i++, at = (at + 1) & (XLATE_HASH_SLOTS - 1)) {
        if (table[at].bytes) { if (xlate_hash_equal(&table[at], key)) return; continue; }
        if (g_xlate->count[bank][lane] < XLATE_HASH_LIMIT) {
            table[at] = *key; g_xlate->count[bank][lane]++; return;
        }
        break;
    }
    g_xlate->counts.overflow[lane]++; g_xlate->current_overflow[lane] = 1;
}
static void xlate_recording(const uint8_t *p) {
    xlate_hash hashes[2] = {
        {.h = {UINT64_C(14695981039346656037), UINT64_C(0x6a09e667f3bcc909)}},
        {.h = {UINT64_C(14695981039346656037), UINT64_C(0x6a09e667f3bcc909)}}};
    int complete = 0, abstract = 1;
    xlate_record_counts *s = &g_xlate->counts; s->recordings++;
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < sizeof h || (h.size & 7) || h.size > XLATE_HASH_BYTES - hashes[0].bytes ||
            hashes[0].ops == XLATE_HASH_OPS || h.op >= OP_COUNT_) break;
        xlate_hash_bytes(&hashes[0], p, h.size);
        xlate_address_field fields[17]; int n = xlate_address_fields(h.op, p + sizeof h, h.size - sizeof h, fields);
        if (n < 0) { s->unknown_ops++; abstract = 0; }
        else if (abstract) {
            size_t cursor = 0;
            for (int i = 0; i < n; i++) {
                size_t at = sizeof h + fields[i].offset;
                xlate_hash_bytes(&hashes[1], p + cursor, at - cursor);
                uint64_t value = 0; memcpy(&value, p + at, fields[i].size);
                /* Nullness is semantic (notably immediate versus deferred); retain it, erase only the address. */
                value = value != 0; xlate_hash_bytes(&hashes[1], &value, fields[i].size);
                cursor = at + fields[i].size;
            }
            xlate_hash_bytes(&hashes[1], p + cursor, h.size - cursor);
        }
        for (unsigned lane = 0; lane < 2; lane++) { hashes[lane].bytes += h.size; hashes[lane].ops++; }
        if (h.op == OP_END) { complete = 1; break; }
        /* EndRecording flattens jumps and Call/CopyCommands inline their bodies. A residual control edge has
           unknown flattened coverage; replay still counts its actual draws, but neither hash is admitted. */
        if (h.op == OP_JUMP || h.op == OP_CALL) break;
        p += h.size;
    }
    s->ops += hashes[0].ops; s->bytes += hashes[0].bytes; s->incomplete += !complete;
    for (unsigned lane = 0; lane < 2; lane++) {
        g_xlate->covered[lane] = complete && (!lane || abstract);
        g_xlate->repeated[lane] = 0;
        if (!g_xlate->covered[lane]) continue;
        s->covered[lane]++;
        int repeat = xlate_previous(&hashes[lane], lane);
        g_xlate->repeated[lane] = repeat; s->repeated[lane] += repeat;
        s->previous_incomplete[lane] += !repeat && g_xlate->previous_overflow[lane];
        xlate_remember(&hashes[lane], lane);
    }
}

/* ---------- CPU recording split; all aggregates are protected by vk_rec_lock ----------
   CLOCK_MONOTONIC uses Android's vDSO when the kernel permits its counter fast path. Do not assume
   CNTVCT_EL0 is safe on every vendor kernel: arm_arch_timer may trap it for counter errata. */
int vk_rec_profile_on, vk_rec_timing_on;
static __thread struct {
    int active, category, weight, timeline;
    unsigned rate;
    uint64_t last, coarse_start;
} T_rec;
static unsigned g_rec_sample_rate = 8;
static uint64_t g_rec_sample_seq[OP_COUNT_];
static struct {
    uint64_t ns[VK_REC_N], count[VK_REC_N], reads, frames, draws, attempts, passes, hist[6], max_draws, ops, submissions, cpu_steps;
    uint64_t pass_ns, largest_ns, max_pass_ns;
    double max_pass_share;
    uint64_t ub_allocated, ub_reused;
    uint64_t coarse_ns, sampled_draws, weighted_reads;
    uint64_t snapshot_bindings, snapshot_unique, snapshot_direct, snapshot_staged;
    uint64_t touch_calls, touch_no_updates, touch_ranges, touch_resolve_avoided, touch_negative_hits, pointer_hits, pointer_misses;
} g_rec;
static struct { int open; uint64_t draws, ns; } g_rec_pass;
static uint64_t g_rec_frame_largest;
static double g_rec_probe_ns;
static uint64_t rec_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int rec_sequential(int category) { return category == VK_REC_S || (category >= VK_REC_S_STREAM && category <= VK_REC_S_TOUCH); }
static int rec_other(int category) { return category == VK_REC_O || category >= VK_REC_O_SLOT; }
static uint64_t rec_category_ns(int other) {
    uint64_t ns = g_rec.ns[other ? VK_REC_O : VK_REC_S];
    for (int i = other ? VK_REC_O_SLOT : VK_REC_S_STREAM; i < (other ? VK_REC_N : VK_REC_O_SLOT); i++) ns += g_rec.ns[i];
    return ns;
}
static void rec_switch_weight(int category, int weight) {
    if (!T_rec.weight && !weight) { T_rec.category = category; return; }
    uint64_t now = rec_now();
    uint64_t ns = (now - T_rec.last) * (unsigned)T_rec.weight;
    if (vk_rec_profile_on) {
        g_rec.ns[T_rec.category] += ns;
        g_rec.weighted_reads += (unsigned)T_rec.weight;
        g_rec.reads++;
    }
    if (T_rec.timeline) {
        enum totk_ft_counter counter = rec_sequential(T_rec.category) ? FT_COUNT_REC_S_NS :
            rec_other(T_rec.category) ? FT_COUNT_REC_O_NS : FT_COUNT_REC_P_NS;
        totk_ft_count(counter, ns);
        (void)counter;
    }
    T_rec.last = now; T_rec.category = category; T_rec.weight = weight;
}
static void rec_switch(int category) { rec_switch_weight(category, T_rec.weight); }
int vk_rec_profile_push(int category) {
    if (!T_rec.active || (!T_rec.weight && (!vk_rec_profile_on || !rec_other(category)))) return -1;
    int previous = T_rec.category, weight = vk_rec_profile_on && rec_other(category) ? 1 : T_rec.weight;
    if (category == VK_REC_RESOLVE && (rec_sequential(previous) || rec_other(previous))) return -1;
    if (vk_rec_profile_on) g_rec.count[category] += (unsigned)weight;
    if (previous == category && T_rec.weight == weight) return -1;
    int token = previous | T_rec.weight << 8;
    rec_switch_weight(category, weight); return token;
}
void vk_rec_profile_pop(int previous) {
    if (previous >= 0) rec_switch_weight(previous & 255, previous >> 8);
}
static int rec_stream_begin(void) {
    if (!T_rec.active || T_rec.rate == 1) return -1;
    int token = T_rec.category | T_rec.weight << 8;
    rec_switch_weight(VK_REC_S_STREAM, 0); return token;
}
static void rec_sample_begin(unsigned op) {
    if (!T_rec.active) return;
    if (T_rec.rate == 1) { if (T_rec.timeline) totk_ft_count(FT_COUNT_REC_SAMPLES, 1); return; }
    if (op == OP_DRAW_IDX) op = OP_DRAW; /* sample graphics attempts together, not one index type's phase */
    uint64_t seq = ++g_rec_sample_seq[op], hash = seq * 0x9e3779b97f4a7c15ull;
    hash ^= hash >> 32;
    if (!(hash & (T_rec.rate - 1))) {
        rec_switch_weight(VK_REC_S_STREAM, (int)T_rec.rate);
        if (vk_rec_profile_on) g_rec.count[VK_REC_S_STREAM] += T_rec.rate;
        if (T_rec.timeline) totk_ft_count(FT_COUNT_REC_SAMPLES, 1);
    }
}
static void rec_sample_end(void) { if (T_rec.active && T_rec.rate != 1) rec_switch_weight(VK_REC_S_STREAM, 0); }
static uint64_t rec_parallel_ns(void) {
    uint64_t n = 0; for (int i = VK_REC_RESOLVE; i < VK_REC_O; i++) n += g_rec.ns[i]; return n;
}
void vk_rec_profile_begin(int category) {
    T_rec.timeline = 0; /* calibration is never walk work */
    if (!g_rec_probe_ns) {
        /* Calibrate the actual timer + exclusive-accounting path on this recording CPU, once.
           This is an estimate, not an overhead subtraction or a device benchmark. */
        T_rec.active = T_rec.weight = 1; T_rec.category = VK_REC_S; T_rec.last = rec_now();
        uint64_t t0 = rec_now();
        for (int i = 0; i < 4096; i++) { int prev = vk_rec_profile_push(VK_REC_PROG); vk_rec_profile_pop(prev); }
        g_rec_probe_ns = (double)(rec_now() - t0) / 8192.0;
        memset(&g_rec, 0, sizeof g_rec);
        LOGV("rec profile: CLOCK_MONOTONIC, %.1f ns/transition; opcode/draw sampling 1/%u (TOTK_VK_REC_PROFILE_RATE); O/lifecycle unsampled", g_rec_probe_ns, g_rec_sample_rate);
    }
    T_rec.active = T_rec.weight = 1; T_rec.category = category; T_rec.last = T_rec.coarse_start = rec_now();
    T_rec.rate = g_rec_sample_rate; T_rec.timeline = totk_ft_work_frame() != 0;
    g_rec.reads++; g_rec.count[category]++;
    if (rec_sequential(category)) g_rec.submissions++;
}
void vk_rec_profile_end(void) {
    if (!T_rec.active) return;
    rec_switch(T_rec.category);
    if (vk_rec_profile_on) g_rec.coarse_ns += T_rec.last - T_rec.coarse_start;
    T_rec.active = T_rec.weight = T_rec.timeline = 0;
}
/* Timeline-only mode reuses the exclusive scopes without enabling the full profiler, calibration or logs.
   Opcode S/P/O intervals are sampled at 1/64 and weighted 64x. Outside mtl_exec the lifecycle is exact,
   at weight one; unselected opcode scopes take no clock reads, including O. */
static void rec_timeline_begin(void) {
    T_rec.active = T_rec.weight = T_rec.timeline = 1;
    T_rec.rate = 64; T_rec.category = VK_REC_S_STREAM; T_rec.last = rec_now();
}
#ifdef TOTK_VK_TESTS
void vk_record_test_profile_ns(uint64_t *out, size_t count) {
    if (count > VK_REC_N) count = VK_REC_N;
    memcpy(out, g_rec.ns, count * sizeof *out);
}
static uint64_t g_lag_test_cache[VK_LAG_TEST_N];
void vk_record_test_lag_cache_stats(uint64_t *out, size_t count) {
    if (count > VK_LAG_TEST_N) count = VK_LAG_TEST_N;
    memcpy(out, g_lag_test_cache, count * sizeof *out);
}
#define LAG_TEST_INC(field) (g_lag_test_cache[field]++)
#else
#define LAG_TEST_INC(field) ((void)0)
#endif
uint64_t vk_rec_profile_draw_begin(void) {
    if (!T_rec.active) return UINT64_MAX;
    g_rec.attempts++;
    if (!T_rec.weight) return UINT64_MAX;
    g_rec.sampled_draws++; return rec_parallel_ns();
}
void vk_rec_profile_draw_end(uint64_t p0, int recorded) {
    if (!T_rec.active) return;
    if (recorded) g_rec.draws++;
    if (g_rec_pass.open) {
        g_rec_pass.draws += recorded;
        if (p0 != UINT64_MAX) g_rec_pass.ns += rec_parallel_ns() - p0;
    }
}
void vk_rec_profile_pass_begin(void) {
    if (T_rec.active) { g_rec_pass.open = 1; g_rec_pass.draws = g_rec_pass.ns = 0; }
}
static void rec_pass_finish(void) {
    if (!g_rec_pass.open) return;
    uint64_t d = g_rec_pass.draws, ns = g_rec_pass.ns;
    int bin = !d ? 0 : d < 16 ? 1 : d < 64 ? 2 : d < 256 ? 3 : d < 1024 ? 4 : 5;
    g_rec.passes++; g_rec.hist[bin]++; g_rec.pass_ns += ns;
    if (d > g_rec.max_draws) g_rec.max_draws = d;
    if (ns > g_rec_frame_largest) g_rec_frame_largest = ns;
    if (ns > g_rec.max_pass_ns) g_rec.max_pass_ns = ns;
    g_rec_pass.open = 0;
}
void vk_rec_profile_pass_end(void) { if (T_rec.active) rec_pass_finish(); }
void vk_rec_profile_present(uint64_t frame) {
    /* A rare pass crossing a present is accounted as frame-local slices, without changing rendering. */
    int open = g_rec_pass.open;
    rec_pass_finish();
    if (open) { g_rec_pass.open = 1; g_rec_pass.draws = g_rec_pass.ns = 0; }
    static uint64_t prev_p;
    uint64_t p = rec_parallel_ns(), frame_p = p - prev_p; prev_p = p;
    double share = frame_p ? (double)g_rec_frame_largest / frame_p : 0;
    if (share > g_rec.max_pass_share) g_rec.max_pass_share = share;
    g_rec.largest_ns += g_rec_frame_largest; g_rec_frame_largest = 0; g_rec.frames++;
    if (frame % 300) return;
    double f = (double)g_rec.frames, ms = 1e-6 / f, s = (double)rec_category_ns(0), o = (double)rec_category_ns(1);
    double total = s + p, estimate = g_rec.reads * g_rec_probe_ns;
    uint64_t sc = g_rec.count[VK_REC_S], oc = g_rec.count[VK_REC_O];
    for (int i = VK_REC_S_STREAM; i < VK_REC_O_SLOT; i++) sc += g_rec.count[i];
    for (int i = VK_REC_O_SLOT; i < VK_REC_N; i++) oc += g_rec.count[i];
    LOGV("rec profile frame %llu, ms/frame: S %.3f P %.3f [resolve %.3f bind/desc %.3f prog/variant %.3f pipeline %.3f vertex/index %.3f cmd %.3f] O %.3f; D=not-separated; draws %.1f/%.1f attempted, passes %.1f; ideal 2/3/4 %.2fx/%.2fx/%.2fx",
         (unsigned long long)frame, s * ms, p * ms, g_rec.ns[VK_REC_RESOLVE] * ms, g_rec.ns[VK_REC_BIND] * ms,
         g_rec.ns[VK_REC_PROG] * ms, g_rec.ns[VK_REC_PIPE] * ms, g_rec.ns[VK_REC_VERTEX] * ms, g_rec.ns[VK_REC_CMD] * ms,
         o * ms, g_rec.draws / f, g_rec.attempts / f, g_rec.passes / f,
         total ? total / (s + p / 2.0) : 1, total ? total / (s + p / 3.0) : 1, total ? total / (s + p / 4.0) : 1);
    LOGV("rec profile granularity: passes/frame draws=0/1-15/16-63/64-255/256-1023/1024+ %.2f/%.2f/%.2f/%.2f/%.2f/%.2f; max draws/pass %llu; largest pass P %.3f ms/frame (%.1f%% of P, max frame share %.1f%%), max pass P %.3f ms; pass-attributed %.1f%% of P",
         g_rec.hist[0] / f, g_rec.hist[1] / f, g_rec.hist[2] / f, g_rec.hist[3] / f, g_rec.hist[4] / f, g_rec.hist[5] / f,
         (unsigned long long)g_rec.max_draws, g_rec.largest_ns * ms, p ? 100.0 * g_rec.largest_ns / p : 0,
         100 * g_rec.max_pass_share, g_rec.max_pass_ns / 1e6, p ? 100.0 * g_rec.pass_ns / p : 0);
    LOGV("rec profile scopes/frame S/resolve/bind/prog/pipeline/vertex/cmd/O %.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f; probes %.1f/draw, estimated overhead %.1f ns/draw (%.3f ms/frame, %.1f%%); O-fixed ideal 2/3/4 %.2fx/%.2fx/%.2fx",
         sc / f, g_rec.count[1] / f, g_rec.count[2] / f, g_rec.count[3] / f,
         g_rec.count[4] / f, g_rec.count[5] / f, g_rec.count[6] / f, oc / f,
         g_rec.attempts ? (double)g_rec.reads / g_rec.attempts : 0, g_rec.attempts ? estimate / g_rec.attempts : 0,
         estimate * ms, total + o ? 100 * estimate / (total + o) : 0,
         total + o ? (total + o) / (s + o + p / 2.0) : 1, total + o ? (total + o) / (s + o + p / 3.0) : 1,
         total + o ? (total + o) / (s + o + p / 4.0) : 1);
    LOGV("rec profile work/frame: %.1f ops, %.1f submissions, %.1f CPU steps",
         g_rec.ops / f, g_rec.submissions / f, g_rec.cpu_steps / f);
    LOGV("rec profile S ms/frame: stream %.3f state %.3f uniform-maintenance %.3f counters %.3f pass %.3f snapshots %.3f GPU-write %.3f resources %.3f draw-touch %.3f residual %.3f",
        g_rec.ns[VK_REC_S_STREAM] * ms, g_rec.ns[VK_REC_S_STATE] * ms, g_rec.ns[VK_REC_S_UNIFORM] * ms,
        g_rec.ns[VK_REC_S_COUNTER] * ms, g_rec.ns[VK_REC_S_PASS] * ms, g_rec.ns[VK_REC_S_SNAPSHOT] * ms,
        g_rec.ns[VK_REC_S_WRITE] * ms, g_rec.ns[VK_REC_S_RESOURCE] * ms, g_rec.ns[VK_REC_S_TOUCH] * ms, g_rec.ns[VK_REC_S] * ms);
    LOGV("rec profile O ms/frame: slot %.3f reset %.3f primary %.3f submit %.3f queue %.3f GPU-wait %.3f worker-wait %.3f execute %.3f assembly %.3f dispatch %.3f transfer %.3f upload %.3f present %.3f residual %.3f",
        g_rec.ns[VK_REC_O_SLOT] * ms, g_rec.ns[VK_REC_O_RESET] * ms, g_rec.ns[VK_REC_O_PRIMARY] * ms,
        g_rec.ns[VK_REC_O_SUBMIT] * ms, g_rec.ns[VK_REC_O_QUEUE] * ms, g_rec.ns[VK_REC_O_GPU_WAIT] * ms,
        g_rec.ns[VK_REC_O_WORKER_WAIT] * ms, g_rec.ns[VK_REC_O_EXECUTE] * ms, g_rec.ns[VK_REC_O_ASSEMBLE] * ms,
        g_rec.ns[VK_REC_O_DISPATCH] * ms, g_rec.ns[VK_REC_O_TRANSFER] * ms, g_rec.ns[VK_REC_O_UPLOAD] * ms,
        g_rec.ns[VK_REC_O_PRESENT] * ms, g_rec.ns[VK_REC_O] * ms);
    LOGV("rec profile uniform update payloads/frame: allocated %.1f reused %.1f", g_rec.ub_allocated / f, g_rec.ub_reused / f);
    LOGV("rec profile sampling: 1/%u ops/draws, %.1f/%.1f draws sampled; category ms are weighted estimates; coarse measured %.3f ms/frame; actual/weighted probe estimates %.3f/%.3f ms/frame",
        g_rec_sample_rate, g_rec.sampled_draws / f, g_rec.attempts / f, g_rec.coarse_ns * ms,
        estimate * ms, g_rec.weighted_reads * g_rec_probe_ns * ms);
    LOGV("rec profile snapshots/frame: binds %.1f unique candidates %.1f; direct/staged %.3f/%.3f KiB (async captures own eager bytes)",
        g_rec.snapshot_bindings / f, g_rec.snapshot_unique / f, g_rec.snapshot_direct / (1024 * f), g_rec.snapshot_staged / (1024 * f));
    LOGV("rec profile draw-touch/frame: calls/no-updates %.1f/%.1f, storage ranges/resolve-avoided %.1f/%.1f, negative-range hits %.1f, frozen pointer hit/miss %.1f/%.1f",
        g_rec.touch_calls / f, g_rec.touch_no_updates / f, g_rec.touch_ranges / f, g_rec.touch_resolve_avoided / f,
        g_rec.touch_negative_hits / f, g_rec.pointer_hits / f, g_rec.pointer_misses / f);
    memset(&g_rec, 0, sizeof g_rec); prev_p = 0;
}
/* ---------- device ---------- */
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetCalibratedTimestampsEXT g_calibrated_timestamps;
static int g_gpu_time_hardware;
static int g_gpu_time_fps_median;
static void ts_clock_init(void);
enum { POOLS_HOST = 1, POOLS_EXPORT, POOLS_HEAP };
static int g_pool_mode; static const char *g_pool_mode_name;
static VkDeviceSize g_host_align = 4096;
static int g_lookup_stats;   /* immutable after vk_init_once: one predictable branch per lookup when disabled */
static _Atomic uintptr_t g_lookup_thread;
static PFN_vkGetPhysicalDeviceExternalBufferProperties vkGetPhysicalDeviceExternalBufferProperties;

static VkExtensionProperties *g_ext; static uint32_t g_next;
static int has_ext(const char *n) { for (uint32_t i = 0; i < g_next; i++) if (!strcmp(g_ext[i].extensionName, n)) return 1; return 0; }

int vk_pick_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static int pick_host_type(uint32_t bits) {   /* CPU reads guest memory constantly: cached + coherent first */
    int t = vk_pick_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = vk_pick_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(bits, 0);
    return t;
}

static const VkBufferUsageFlags POOL_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

static void *completion_thread(void *);
static void *gpu_guard_thread(void *);
static int lag_wait_on(void);
static int g_lag_wait = -1;   /* initialized on the walk before any recorder worker is dispatched */
_Atomic int vk_lag_capture_state = -1; /* -1 uninitialized, 0 off/cut over, 1 protected capture */
static void lag_pressure_fallback(void); /* walking owner, only between NVN streams */
static void lag_guard_mark(uint64_t addr, uint64_t size, unsigned reason);

/* ---------- pipeline cache on disk ----------
   Driver/context-matched opaque blobs are loaded at device creation. vk_pcache.c owns shared validation,
   quarantine and coalesced, quiet-aware persistence with retained scratch; no save IO/cache snapshot runs
   on a game/recording/UI thread. TOTK_VK_PIPELINE_CACHE defaults to state/vk-pipeline-cache.bin; "0" disables it.
   TOTK_VK_PIPELINE_CACHE_IMPORT (default vk-pipeline-cache.bin in the game folder): a compatible context
   merged once at start and renamed .imported. Both paths accept the documented empty-debug legacy exception. */
static const char *pcache_path(void) {
    static char path[1100]; static const char *result; static int init;
    if (!init) {
        const char *p = getenv("TOTK_VK_PIPELINE_CACHE");
        result = vk_shader_cache_path(p && *p ? p : "state/vk-pipeline-cache.bin", path, sizeof path);
        init = 1;
    }
    return result;
}
static int export_ok(VkExternalMemoryHandleTypeFlagBits ht, int *dedicated) {
    VkPhysicalDeviceExternalBufferInfo q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = POOL_USAGE, .handleType = ht};
    VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(V.pd, &q, &ep);
    VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
    if (dedicated) *dedicated = (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    return (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}
static int g_export_dedicated;
static int heap_open(void);

/* Device features: everything the driver has, except robust buffer access (costly; NVN has none) -- the translated
   shaders use what Ryujinx's Vulkan output needs (storage image formats, clip distances, int64, ...). */
static VkPhysicalDeviceCustomBorderColorFeaturesEXT f_cbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
static VkPhysicalDeviceRobustness2FeaturesEXT f_rob2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
static VkPhysicalDeviceIndexTypeUint8FeaturesEXT f_u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
static VkPhysicalDeviceDepthClipControlFeaturesEXT f_dcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
static VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT f_div = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT f_gpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
static VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
static int g_occl_on;   /* fixed at device creation: precise occlusion + host query reset, explicitly opted in */

static VkFormat depth_choice(VkFormat want, VkFormat fallback) {
    VkFormatProperties fp; V.get_format_props(V.pd, want, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? want : fallback;
}

static void vk_init_once(void) {
    vk_draw_switches_init();   /* before any compile worker or shader load reads them */
    vk_upload_threads_init();
    vk_tex_memo_init();
    (void)vk_shaders_ready();   /* headless startup too; the app already froze selection against its game folder */
    { const char *fps60 = getenv("TOTK_FPS60");
      LOGV("frame policy: FPS60=%d FRAME_LAG=%d CHUNK_DRAWS=%ld",
           fps60 && strcmp(fps60, "0") != 0, runtime_frame_lag_requested(), vk_frame_chunk_limit()); }
    { const char *e = getenv("TOTK_VK_HITCH"); g_hitch_on = e && atoi(e) > 0; }
    { const char *e = getenv("TOTK_VK_LOOKUP_STATS"); g_lookup_stats = e && atoi(e); }
    { const char *e = getenv("TOTK_VK_REC_PROFILE"); vk_rec_profile_on = e && !strcmp(e, "1"); }
    vk_rec_timing_on = vk_rec_profile_on || totk_ft_on();
    const char *gpl_env = getenv("TOTK_VK_PIPELINE_LIBRARY");
    int gpl_requested = gpl_env && !strcmp(gpl_env, "1");
    V.has_pipeline_library = 0;
    V.pipeline_library_fast_linking = V.pipeline_library_independent_interpolation = 0;
    { const char *e = getenv("TOTK_VK_REC_PROFILE_RATE"); long requested = e ? strtol(e, NULL, 10) : 8;
      if (requested < 1) requested = 1; if (requested > 64) requested = 64;
      g_rec_sample_rate = 1; while (g_rec_sample_rate < requested) g_rec_sample_rate <<= 1; }
    const char *kind, *why, *drv = getenv("TOTK_VK_DRIVER");
    if (drv && (!*drv || !strcmp(drv, "system"))) drv = NULL;
    gipa = vk_load_driver(drv, &kind, &why);
    if (!gipa) DIE("Vulkan driver %s: %s", drv ? drv : "(system)", why);
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "totk-native", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VKCHECK(vkCreateInstance(&ici, NULL, &V.inst));
#define IL(name) PFN_##name name = (PFN_##name)gipa(V.inst, #name)
    IL(vkEnumeratePhysicalDevices); IL(vkCreateDevice); IL(vkGetDeviceProcAddr); IL(vkEnumerateDeviceExtensionProperties);
    IL(vkGetPhysicalDeviceProperties2); IL(vkGetPhysicalDeviceFeatures2); IL(vkGetPhysicalDeviceMemoryProperties);
    IL(vkGetPhysicalDeviceQueueFamilyProperties);
#undef IL
    vkGetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(V.inst, "vkGetPhysicalDeviceExternalBufferProperties");
    V.get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(V.inst, "vkGetPhysicalDeviceFormatProperties");
    V.get_image_format_props = (PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(V.inst, "vkGetPhysicalDeviceImageFormatProperties");
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(V.inst, &n, &V.pd);
    if ((r && r != VK_INCOMPLETE) || !n) DIE("no Vulkan device");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties drvp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &hostp};
    V.subgroup = (VkPhysicalDeviceSubgroupProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    V.subgroup_sizes = (VkPhysicalDeviceSubgroupSizeControlProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    hostp.pNext = &V.subgroup; V.subgroup.pNext = &V.subgroup_sizes;
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drvp};
    vkGetPhysicalDeviceProperties2(V.pd, &p2);
    V.props = p2.properties;
    if (V.props.apiVersion < VK_API_VERSION_1_3) DIE("%s offers Vulkan %u.%u; 1.3 is needed", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion));
    LOGV("device %s, driver %s %s (%s%s%s)", V.props.deviceName, drvp.driverName, drvp.driverInfo, kind, drv ? " " : "", drv ? drv : "");
    vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mp);
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &g_next, NULL);
    g_ext = VK_CALLOC(g_next, sizeof *g_ext);
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &g_next, g_ext);
    int gpl_extensions = has_ext("VK_EXT_graphics_pipeline_library") && has_ext("VK_KHR_pipeline_library");
    if (gpl_extensions) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT
        };
        VkPhysicalDeviceProperties2 gplp2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gplp
        };
        vkGetPhysicalDeviceProperties2(V.pd, &gplp2);
        V.pipeline_library_fast_linking = gplp.graphicsPipelineLibraryFastLinking;
        V.pipeline_library_independent_interpolation = gplp.graphicsPipelineLibraryIndependentInterpolationDecoration;
    }

#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    /* Import the AHB's dma-buf with an explicit layout, not an AHB memory handle: Turnip's deferred AHB path
       discards caller tiling/modifier and replaces them with gralloc metadata. No loader swapchain is needed. */
    const char *present = getenv("TOTK_VK_PRESENT");
    int want_ahb = !present || !*present || !strcmp(present, "ahb");
    V.ahb_present = want_ahb && has_ext("VK_EXT_image_drm_format_modifier") &&
        has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && has_ext("VK_EXT_queue_family_foreign");
    if (want_ahb && !V.ahb_present) LOGV("window present: explicit AHB layout unavailable (modifier %d, fd %d, dma-buf %d, foreign queue %d); using copy",
        has_ext("VK_EXT_image_drm_format_modifier"), has_ext("VK_KHR_external_memory_fd"),
        has_ext("VK_EXT_external_memory_dma_buf"), has_ext("VK_EXT_queue_family_foreign"));
    else if (present && *present && strcmp(present, "copy") && !want_ahb) LOGV("window present: unknown TOTK_VK_PRESENT=%s; using copy", present);
    if (!V.ahb_present && !want_ahb) LOGV("window present: copy (Android default ahb; explicit copy override)");
#endif
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, NULL);
    VkQueueFamilyProperties *qf = VK_CALLOC(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, qf);
    V.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { V.qfam = i; break; }
    V.timestamp_bits = V.qfam < qn ? qf[V.qfam].timestampValidBits : 0;
    free(qf);
    if (V.qfam == UINT32_MAX) DIE("no graphics + compute queue");
    if (vk_fg_qcom_requested()) {
        /* Report only. A float queue priority is device-relative, not KGSL
           global priority. Do not enable features/extensions or alter qci. */
        int khr=has_ext("VK_KHR_global_priority"), ext=has_ext("VK_EXT_global_priority");
        int query_ext=has_ext("VK_EXT_global_priority_query");
        VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR priority_query={
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR};
        if (khr || query_ext) {
            VkPhysicalDeviceFeatures2 query={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&priority_query};
            vkGetPhysicalDeviceFeatures2(V.pd,&query);
        }
        int low=-1,medium=-1,high=-1,realtime=-1; uint32_t advertised=0;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties2 properties2=
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)gipa(V.inst,"vkGetPhysicalDeviceQueueFamilyProperties2");
        if (priority_query.globalPriorityQuery && properties2) {
            VkQueueFamilyGlobalPriorityPropertiesKHR priorities[qn];
            VkQueueFamilyProperties2 properties[qn];
            for (uint32_t i=0;i<qn;i++) {
                priorities[i]=(VkQueueFamilyGlobalPriorityPropertiesKHR){VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR};
                properties[i]=(VkQueueFamilyProperties2){VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2,.pNext=&priorities[i]};
            }
            uint32_t count=qn; properties2(V.pd,&count,properties);
            if (V.qfam<count) {
                const VkQueueFamilyGlobalPriorityPropertiesKHR *p=&priorities[V.qfam];
                advertised=p->priorityCount;
                if (advertised && advertised<=VK_MAX_GLOBAL_PRIORITY_SIZE_KHR) {
                    low=medium=high=realtime=0;
                    for (uint32_t i=0;i<advertised;i++) {
                        low|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
                        medium|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
                        high|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
                        realtime|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR;
                    }
                }
            }
        }
        LOGV("framegen qcom Vulkan priority: KHR_global_priority=%d EXT_global_priority=%d EXT_global_priority_query=%d query-feature=%u family=%u advertised-count=%u LOW/MEDIUM/HIGH/REALTIME=%d/%d/%d/%d (-1=unqueried/invalid); report-only, render queue unchanged: no global request (API default MEDIUM), device-relative priority=1.0; changing global priority requires queue/device creation and app permission is untested",
            khr,ext,query_ext,priority_query.globalPriorityQuery,V.qfam,advertised,low,medium,high,realtime);
    }

    /* Pool path. */
    const char *want = getenv("TOTK_VK_POOLS");
    int can_host = has_ext("VK_EXT_external_memory_host");
    int can_export = has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && export_ok(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &g_export_dedicated);
    int can_heap = has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && heap_open() >= 0;
    if (want && !strcmp(want, "host")) { if (!can_host) DIE("TOTK_VK_POOLS=host: the driver has no VK_EXT_external_memory_host"); g_pool_mode = POOLS_HOST; }
    else if (want && !strcmp(want, "export")) { if (!can_export) DIE("TOTK_VK_POOLS=export: the driver cannot export dma-bufs"); g_pool_mode = POOLS_EXPORT; }
    else if (want && !strcmp(want, "heap")) { if (!can_heap) DIE("TOTK_VK_POOLS=heap: no dma-buf heap or no dma-buf import"); g_pool_mode = POOLS_HEAP; }
    else g_pool_mode = can_host ? POOLS_HOST : can_export ? POOLS_EXPORT : can_heap ? POOLS_HEAP : 0;
    if (!g_pool_mode) DIE("this driver cannot share memory with the game (no host import, dma-buf export or dma-buf heap import): try a Turnip driver with TOTK_VK_DRIVER");
    g_pool_mode_name = g_pool_mode == POOLS_HOST ? "host-pointer import" : g_pool_mode == POOLS_EXPORT ? "dma-buf export mapped at the guest address" : "dma-buf heap mapped at the guest address";
    if (g_pool_mode == POOLS_HOST) g_host_align = hostp.minImportedHostPointerAlignment ? hostp.minImportedHostPointerAlignment : 4096;

    /* Features: query the whole chain the backend knows, then enable what is there. */
    void *chain = NULL;
#define LINK(s, ext) if (!(ext) || has_ext(ext)) { (s).pNext = chain; chain = &(s); }
    LINK(f_cbc, "VK_EXT_custom_border_color") LINK(f_rob2, "VK_EXT_robustness2") LINK(f_u8, "VK_EXT_index_type_uint8")
    LINK(f_dcc, "VK_EXT_depth_clip_control") LINK(f_div, "VK_EXT_vertex_attribute_divisor")
    if (gpl_extensions) { f_gpl.pNext = chain; chain = &f_gpl; }
    LINK(f13, NULL) LINK(f12, NULL) LINK(f11, NULL)
#undef LINK
    f2.pNext = chain;
    vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    const char *subgroup_fix = getenv("TOTK_VK_SUBGROUP_FIX");
    V.subgroup_fix = subgroup_fix && !strcmp(subgroup_fix, "1");
    V.subgroup_control = V.subgroup_fix && f13.subgroupSizeControl;
    f13.subgroupSizeControl = V.subgroup_control;
    f13.computeFullSubgroups = VK_FALSE;
    LOGV("subgroup fix: %d (startup-only), native %u, range %u-%u, control %d, required stages 0x%x",
        V.subgroup_fix, V.subgroup.subgroupSize, V.subgroup_sizes.minSubgroupSize,
        V.subgroup_sizes.maxSubgroupSize, V.subgroup_control, V.subgroup_sizes.requiredSubgroupSizeStages);
    int gpl_supported = gpl_extensions && f_gpl.graphicsPipelineLibrary;
    V.has_pipeline_library = gpl_requested && gpl_supported;
    f_gpl.graphicsPipelineLibrary = V.has_pipeline_library;
    if (gpl_extensions && !V.has_pipeline_library) f13.pNext = f_gpl.pNext; /* query only: no opt-in feature structure at device creation */
    LOGV("graphics pipeline library: requested %d, supported %d, enabled %d, fast-linking %u, independent-interpolation %u",
        gpl_requested, gpl_supported, V.has_pipeline_library,
        V.pipeline_library_fast_linking, V.pipeline_library_independent_interpolation);
    if (!f13.synchronization2 || !f13.dynamicRendering) DIE("the driver lacks synchronization2 or dynamic rendering");
    f2.features.robustBufferAccess = VK_FALSE; f_rob2.robustBufferAccess2 = VK_FALSE; f_rob2.robustImageAccess2 = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE; f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f_cbc.customBorderColorWithoutFormat = f_cbc.customBorderColorWithoutFormat && f_cbc.customBorderColors;
    V.has_custom_border = has_ext("VK_EXT_custom_border_color") && f_cbc.customBorderColors && f_cbc.customBorderColorWithoutFormat;
    V.has_null_descriptor = has_ext("VK_EXT_robustness2") && f_rob2.nullDescriptor;
    V.has_index_u8 = has_ext("VK_EXT_index_type_uint8") && f_u8.indexTypeUint8;
    V.has_depth_clip_control = has_ext("VK_EXT_depth_clip_control") && f_dcc.depthClipControl;
    V.has_divisor = has_ext("VK_EXT_vertex_attribute_divisor") && f_div.vertexAttributeInstanceRateDivisor;
    V.has_bc = f2.features.textureCompressionBC; V.has_geometry = f2.features.geometryShader;
    V.has_layer_output = f12.shaderOutputLayer; V.has_logic_op = f2.features.logicOp; V.has_dual_src = f2.features.dualSrcBlend;
    V.has_cache_control = f13.pipelineCreationCacheControl;
    V.has_pipeline_statistics = f2.features.pipelineStatisticsQuery;
    V.has_host_query_reset = f12.hostQueryReset;
    V.has_image_read_without_format = f2.features.shaderStorageImageReadWithoutFormat;
    V.has_image_write_without_format = f2.features.shaderStorageImageWriteWithoutFormat;
    V.samples = V.props.limits.framebufferColorSampleCounts & V.props.limits.framebufferDepthSampleCounts;

    static const char *want_ext[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_custom_border_color", "VK_EXT_robustness2", "VK_EXT_index_type_uint8", "VK_EXT_depth_clip_control",
        "VK_EXT_vertex_attribute_divisor", "VK_EXT_shader_stencil_export", "VK_EXT_shader_viewport_index_layer",
        "VK_KHR_shader_float_controls", "VK_EXT_provoking_vertex", "VK_EXT_primitive_topology_list_restart"};
    const char *exts[32]; uint32_t ne = 0;
    for (unsigned i = 0; i < sizeof want_ext / sizeof *want_ext; i++) {
        if (!has_ext(want_ext[i])) continue;
        if (!strcmp(want_ext[i], "VK_EXT_external_memory_host") && g_pool_mode != POOLS_HOST) continue;
        exts[ne++] = want_ext[i];
    }
    if (V.has_pipeline_library) {
        exts[ne++] = "VK_EXT_graphics_pipeline_library";
        exts[ne++] = "VK_KHR_pipeline_library";
    }
    int calibrated = 0;
    int broken_device_clock = 0;
#ifdef __ANDROID__
    /* Turnip's KGSL device_get_gpu_timestamp is UNREACHABLE/return-success
       without writing the timestamp, despite advertising this extension. */
    broken_device_clock = drvp.driverID == VK_DRIVER_ID_MESA_TURNIP;
#endif
    if (broken_device_clock)
        LOGV("[fps-policy] GPU timestamp clock: bypassing unsupported Turnip/KGSL DEVICE calibration");
    if (!broken_device_clock && has_ext("VK_EXT_calibrated_timestamps")) {
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains =
            (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)gipa(V.inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
        uint32_t count = 0;
        if (domains && domains(V.pd, &count, NULL) == VK_SUCCESS && count) {
            VkTimeDomainEXT *available = VK_CALLOC(count, sizeof *available);
            if (domains(V.pd, &count, available) == VK_SUCCESS) {
                int device = 0, monotonic = 0;
                for (uint32_t i = 0; i < count; i++) {
                    device |= available[i] == VK_TIME_DOMAIN_DEVICE_EXT;
                    monotonic |= available[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
                }
                calibrated = device && monotonic;
            }
            free(available);
        }
        if (calibrated) exts[ne++] = "VK_EXT_calibrated_timestamps";
    }
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    if (V.ahb_present) { exts[ne++] = "VK_EXT_image_drm_format_modifier"; exts[ne++] = "VK_EXT_queue_family_foreign"; }
    int qcom_interop=vk_fg_qcom_requested() &&
        has_ext("VK_ANDROID_external_memory_android_hardware_buffer") &&
        has_ext("VK_KHR_external_semaphore_fd") && has_ext("VK_EXT_queue_family_foreign");
    const char *ubwc=getenv("TOTK_PRESENT_UBWC");
    int ubwc_qualified=vk_present_ubwc_driver_known(drvp.driverInfo);
    int present_ahb_import=V.ahb_present && ubwc && !strcmp(ubwc,"1") && ubwc_qualified &&
        has_ext("VK_ANDROID_external_memory_android_hardware_buffer");
    if (present_ahb_import && !qcom_interop)
        exts[ne++]="VK_ANDROID_external_memory_android_hardware_buffer";
    if (qcom_interop) {
        exts[ne++]="VK_ANDROID_external_memory_android_hardware_buffer";
        exts[ne++]="VK_KHR_external_semaphore_fd";
        if (!V.ahb_present) exts[ne++]="VK_EXT_queue_family_foreign";
    }
#endif
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts};
    VKCHECK(vkCreateDevice(V.pd, &dci, NULL, &V.dev));
#define DL(name) name = (PFN_##name)vkGetDeviceProcAddr(V.dev, #name);
    VK_DEVICE_FUNCS(DL)
#undef DL
    vk_sections_init();
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    vk_fg_qcom_device(gipa,vkGetDeviceProcAddr,qcom_interop);
    vk_fg_lsfg_device(gipa,vkGetDeviceProcAddr,qcom_interop);
#else
    vk_fg_qcom_device(gipa,vkGetDeviceProcAddr,0);
    vk_fg_lsfg_device(gipa,vkGetDeviceProcAddr,0);
#endif
    if (calibrated) g_calibrated_timestamps =
        (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(V.dev, "vkGetCalibratedTimestampsEXT");
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    if (V.ahb_present) {
        V.get_image_format_props2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(V.inst, "vkGetPhysicalDeviceImageFormatProperties2");
        V.get_image_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(V.dev, "vkGetImageDrmFormatModifierPropertiesEXT");
        if (present_ahb_import) V.get_ahb_props=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
            vkGetDeviceProcAddr(V.dev,"vkGetAndroidHardwareBufferPropertiesANDROID");
        if (!V.get_image_format_props2 || !V.get_image_modifier || !vkGetMemoryFdPropertiesKHR) {
            V.ahb_present = 0; LOGV("window present: missing explicit dma-buf import entry points; using copy");
        } else LOGV("window present: ahb (explicit LINEAR dma-buf import, modifier=0; device measurement pending)");
    }
    if (ubwc && !strcmp(ubwc,"1")) {
        LOGV("[present-ubwc] driverInfo=\"%s\" qualified=%d; AHB import API %s; nonidentity root hint required",
            drvp.driverInfo,ubwc_qualified,V.get_ahb_props ? "enabled" : "unavailable");
        if (!ubwc_qualified) LOGV("[present-ubwc] unknown driverInfo: retaining explicit LINEAR presentation");
        else if (!V.get_ahb_props) LOGV("[present-ubwc] import capabilities unavailable: retaining explicit LINEAR presentation");
    }
#endif
    { static int requested = -1; if (requested < 0) { const char *e = getenv("TOTK_VK_OCCLUSION"); requested = e && atoi(e) != 0; }
      g_occl_on = requested && f12.hostQueryReset && f2.features.occlusionQueryPrecise;
      if (requested) LOGV("[occl] TOTK_VK_OCCLUSION: %s (hostQueryReset %u, occlusionQueryPrecise %u); pending sample conditions stay visible",
                         g_occl_on ? "on" : "off: required features missing", f12.hostQueryReset, f2.features.occlusionQueryPrecise); }
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
    int gpu_time_default_hardware = 0;
#if defined(__ANDROID__) && !defined(GAME_BOTW)
    const char *fps60 = getenv("TOTK_FPS60");
    gpu_time_default_hardware = !fps60 || !strcmp(fps60, "0");
#endif
    const char *gpu_time = getenv("TOTK_GPU_TIME");
    int gpu_time_explicit = gpu_time && *gpu_time;
    g_gpu_time_hardware = gpu_time_explicit ? !strcmp(gpu_time, "hardware") : gpu_time_default_hardware;
    LOGV("[fps-policy] GPU report time: %s (%s)",
         g_gpu_time_hardware ? "hardware" : "latency",
         !gpu_time_explicit ? (gpu_time_default_hardware ?
             "default Android 30-fps; TOTK_GPU_TIME=latency opts out" :
             "default latency; TOTK_GPU_TIME=hardware opts in") :
         g_gpu_time_hardware || !strcmp(gpu_time, "latency") ? "explicit" :
             "unknown value; retaining latency");
    g_gpu_time_fps_median = g_gpu_time_hardware &&
        vk_gpu_time_fps_median_enabled(getenv("TOTK_GPU_TIME_FPS_MEDIAN"));
    LOGV("[fps-policy] TOTK_GPU_TIME_FPS_MEDIAN=%d: FPS END consumer only, median of three unique valid reports; hardware required",
         g_gpu_time_fps_median);
    ts_clock_init();
    size_t pc_size; void *pc_data = vk_pcache_load(pcache_path(), &pc_size, 1);
    VkPipelineCacheCreateInfo pcc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = pc_size, .pInitialData = pc_data};
    VKCHECK(vkCreatePipelineCache(V.dev, &pcc, NULL, &V.pcache));
    free(pc_data);
    { const char *base = getenv("TOTK_VK_PIPELINE_CACHE_IMPORT"); if (!base || !*base) base = "vk-pipeline-cache.bin";
      char import_path[1100]; const char *ip = vk_shader_cache_path(base, import_path, sizeof import_path);
      size_t isz = 0; void *idata = vk_pcache_load(ip, &isz, 0); VkPipelineCache ic;
      VkPipelineCacheCreateInfo icc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = isz, .pInitialData = idata};
      if (idata && !vkCreatePipelineCache(V.dev, &icc, NULL, &ic)) {
          VkResult r = vkMergePipelineCaches(V.dev, V.pcache, 1, &ic); vkDestroyPipelineCache(V.dev, ic, NULL);
          if (!r) vk_pcache_changed();
          char done[1100]; snprintf(done, sizeof done, "%s.imported", ip);
          if (!r && !rename(ip, done)) LOGV("pipeline cache: merged %zu KiB from %s (now %s)", isz >> 10, ip, done);
          else RED(4, "pipeline cache: importing %s: merge %d, rename %s", ip, r, strerror(errno));
      }
      free(idata); }
    vk_pcache_start(pcache_path(), pc_size);
    vk_fmt_set_depth(depth_choice(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT), depth_choice(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT));
    vk_post_init();   /* optional internal pipelines are ready before game recording starts */
    if (lag_wait_on()) {
        nvn_dirty_lag_enable();
        pthread_t guard; if (pthread_create(&guard, NULL, gpu_guard_thread, NULL)) DIE("lag GPU-fence thread");
        pthread_detach(guard);
    }
    pthread_t t; if (pthread_create(&t, NULL, completion_thread, NULL)) DIE("completion thread");
    pthread_detach(t);
    vk_compute_start();
    vk_pipeline_prewarm();
    LOGV("memory pools: %s; %u extensions, BC %s, custom border colours %s, 8-bit indices %s, null descriptors %s", g_pool_mode_name, ne,
         V.has_bc ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_index_u8 ? "yes" : "no", V.has_null_descriptor ? "yes" : "no");
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
void mtl_init(void) { pthread_once(&g_once, vk_init_once); }

/* ---------- dma-buf heap ---------- */
struct dma_heap_alloc { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_alloc)
static int g_heap_fd = -1;
static int heap_open(void) {
    static const char *heaps[] = {"/dev/dma_heap/system", "/dev/dma_heap/qcom,system"};
    for (unsigned i = 0; i < 2 && g_heap_fd < 0; i++) g_heap_fd = open(heaps[i], O_RDONLY | O_CLOEXEC);
    return g_heap_fd;
}

/* ---------- memory pools ----------
   kind OWN: the pool has its own device memory (and, for export / heap, the dma-buf mapped at its address).
   kind VIEW: the pool lies inside another pool's pages (pools may be carved from a bigger pool): same buffer, offset.
   kind VIRT: a virtual pool; addresses resolve through the alias table (physical pool pages mapped into it).
   kind CPU: no GPU backing (partial overlap with another pool): GPU-ordered work on it runs on the CPU in order. */
enum { K_OWN = 1, K_VIEW, K_VIRT, K_CPU };
typedef struct pool_ent {
    uintptr_t lo, hi, base, end;    /* registered range; page-rounded range */
    int kind, fd;
    VkBuffer buf; VkDeviceMemory mem;
    uint64_t identity;             /* registration lifetime, not a guest pool object's reusable address */
    struct pool_ent *owner; uint64_t off;   /* VIEW: owner pool, offset of base in owner's buffer */
} pool_ent;
typedef struct { uintptr_t va, end; pool_ent *owner; uint64_t off; } alias_ent;   /* virtual pool pages -> owner's buffer offset */
static pool_ent **g_pools; static int g_npool, g_cappool;
static alias_ent *g_alias; static int g_nalias, g_capalias;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pool_identity;
static uint64_t g_pool_bytes, g_pool_cpu;
/* Address index for resolve(): GPU-backed spans sorted by start, plus elementary segments naming the furthest-reaching
   span. An early large pool kept the old prefix-max backscan linear; resolve_local was 10.6% of recording at Lookout Landing. */
typedef struct { uintptr_t lo, end; VkBuffer buf; uint64_t off; } span_t;   /* off: buffer offset of lo */
typedef struct { uintptr_t lo; int best; } span_seg_t;   /* best covers [lo, next lo); the last breakpoint has no span */
static span_t *g_span; static span_seg_t *g_span_seg; static int *g_span_heap; static int g_nspan, g_nseg, g_capspan, g_span_dirty = 1;
static _Atomic uint64_t g_span_gen = 1;   /* bumped (under g_pool_mu) with every change of the pools or aliases */
static _Atomic uint64_t g_owner_gen = 1; /* physical buffer ownership, independent of virtual mapping churn */
static void span_changed(void) { g_span_dirty = 1; atomic_fetch_add_explicit(&g_span_gen, 1, memory_order_release); }

static uintptr_t page_sz(void) { static uintptr_t p; if (!p) p = (uintptr_t)getpagesize(); return p; }
static pool_ent *own_containing(uintptr_t a, uintptr_t b) {   /* caller holds g_pool_mu */
    for (int i = 0; i < g_npool; i++) { pool_ent *p = g_pools[i]; if (p->kind == K_OWN && a >= p->base && b <= p->end) return p; }
    return NULL;
}
static int own_overlapping(uintptr_t a, uintptr_t b) {
    for (int i = 0; i < g_npool; i++) { pool_ent *p = g_pools[i]; if (p->kind == K_OWN && a < p->end && b > p->base) return 1; }
    return 0;
}
static VkBuffer make_buffer(VkDeviceSize size, VkExternalMemoryHandleTypeFlags ht, VkMemoryRequirements *req) {
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = ht};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, ht ? &eb : NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bci, NULL, &b));
    vkGetBufferMemoryRequirements(g_dev, b, req);
    return b;
}
/* Give [base, end) its own device memory. 0 on failure (the pool then runs on the CPU path). */
static int back_pool(pool_ent *e) {
    size_t size = e->end - e->base;
    VkMemoryRequirements req;
    if (g_pool_mode == POOLS_HOST) {
        VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)e->base, &hp) || !hp.memoryTypeBits) return 0;
        e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &req);
        int t = pick_host_type(hp.memoryTypeBits & req.memoryTypeBits);
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)e->base};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, (uint32_t)t};
        if (t < 0 || vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL)) { vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
        e->fd = -1;
        return 1;
    }
    int fd = -1;
    e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &req);
    if (g_pool_mode == POOLS_EXPORT) {
        int t = pick_host_type(req.memoryTypeBits);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = e->buf};
        VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, g_export_dedicated ? &ded : NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, (uint32_t)t};
        VkResult r = t < 0 ? VK_ERROR_UNKNOWN : vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL);
        if (r) { LOGV("pool %#llx+%#zx: export allocation failed: %d", (unsigned long long)e->base, size, r); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = e->mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        if (vkGetMemoryFdKHR(g_dev, &gi, &fd) || fd < 0) { vk_memory_release(e->mem, VK_MEMORY_POOL); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    } else {
        struct dma_heap_alloc a = {.len = req.size > size ? req.size : size, .fd_flags = O_RDWR | O_CLOEXEC};
        if (ioctl(g_heap_fd, DMA_HEAP_IOCTL_ALLOC, &a)) { LOGV("pool %#llx+%#zx: dma-buf heap allocation: %s", (unsigned long long)e->base, size, strerror(errno)); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        fd = (int)a.fd;
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        int t = vkGetMemoryFdPropertiesKHR(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) ? -1 : pick_host_type(fp.memoryTypeBits & req.memoryTypeBits);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, a.len, (uint32_t)t};
        if (t < 0 || vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL)) { close(imp.fd); close(fd); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    }
    VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
    /* Move the contents in, then put the dma-buf where they were: one mmap replaces the pages at once. */
    void *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (tmp == MAP_FAILED) DIE("pool %#llx+%#zx: mapping the dma-buf: %s", (unsigned long long)e->base, size, strerror(errno));
    memcpy(tmp, (void *)e->base, size);
    if (mmap((void *)e->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != (void *)e->base)
        DIE("pool %#llx+%#zx: mapping the dma-buf at the guest address: %s", (unsigned long long)e->base, size, strerror(errno));
    munmap(tmp, size);
    e->fd = fd;
    return 1;
}
static void unback_pool(pool_ent *e) {
    size_t size = e->end - e->base;
    if (e->fd >= 0) {   /* contents back into ordinary shared memory, moved into place in one step */
        void *fresh = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (fresh == MAP_FAILED) DIE("pool %#llx: fresh pages: %s", (unsigned long long)e->base, strerror(errno));
        memcpy(fresh, (void *)e->base, size);
        if (mremap(fresh, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)e->base) != (void *)e->base)
            DIE("pool %#llx: restoring guest pages: %s", (unsigned long long)e->base, strerror(errno));
        close(e->fd); e->fd = -1;
    }
    vkDestroyBuffer(g_dev, e->buf, NULL); vk_memory_release(e->mem, VK_MEMORY_POOL);
}
static void pool_insert(pool_ent *e) {
    span_changed();
    if (e->kind == K_OWN) atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    e->identity = ++g_pool_identity;
    if (g_npool == g_cappool) { g_cappool = g_cappool ? 2 * g_cappool : 64; g_pools = VK_REALLOC(g_pools, (size_t)g_cappool * sizeof *g_pools); }
    g_pools[g_npool++] = e;
}
static void add_range(const void *mem, uint64_t size, int virt) {
    mtl_init();
    uintptr_t pg = page_sz(), lo = (uintptr_t)mem, hi = lo + size;
    if (g_pool_mode == POOLS_HOST && pg < g_host_align) pg = (uintptr_t)g_host_align;
    pool_ent *e = VK_CALLOC(1, sizeof *e);
    e->lo = lo; e->hi = hi; e->base = lo & ~(pg - 1); e->end = (hi + pg - 1) & ~(pg - 1); e->fd = -1;
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *owner = own_containing(e->base, e->end);
    if (virt && g_pool_mode != POOLS_HOST) e->kind = K_VIRT;
    else if (owner) { e->kind = K_VIEW; e->owner = owner; e->off = e->base - owner->base; }
    else if (own_overlapping(e->base, e->end)) e->kind = K_CPU;
    else e->kind = back_pool(e) ? K_OWN : K_CPU;
    if (e->kind == K_CPU) { static int said; if (said++ < 8) LOGV("【红】pool %p+%#llx gets no GPU memory (overlaps another pool or allocation failed): its GPU work runs on the CPU", mem, (unsigned long long)size); g_pool_cpu++; }
    if (e->kind == K_OWN) g_pool_bytes += e->end - e->base;
    pool_insert(e);
    pthread_mutex_unlock(&g_pool_mu);
}
static int pool_gpu_quiet(void);
static void del_range(const void *mem, int wait_gpu) {
    pthread_mutex_lock(&g_pool_mu);
    int idx = -1;
    for (int i = g_npool - 1; i >= 0; i--) if (g_pools[i]->lo == (uintptr_t)mem) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&g_pool_mu); return; }
    /* Finalize owns recording and has waited outside the registry; no producer
       can reserve another ticket before destruction. MapVirtual keeps its old path. */
    if (!wait_gpu && !pool_gpu_quiet()) DIE("pool finalize lost recording exclusion");
    pool_ent *e = g_pools[idx]; g_pools[idx] = g_pools[--g_npool]; span_changed();
    if (e->kind == K_VIRT) {   /* its aliases go with it (MapVirtual re-adds the pool after remapping) */
        for (int i = 0; i < g_nalias; ) { if (g_alias[i].va >= e->base && g_alias[i].end <= e->end) g_alias[i] = g_alias[--g_nalias]; else i++; }
    }
    if (e->kind == K_OWN) {
        atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
        /* Pools inside it and virtual pages mapped from it lose their GPU backing; the CPU still sees the same bytes. */
        for (int i = 0; i < g_npool; i++) if (g_pools[i]->owner == e) { g_pools[i]->kind = K_CPU; g_pools[i]->owner = NULL; }
        for (int i = 0; i < g_nalias; ) { if (g_alias[i].owner == e) g_alias[i] = g_alias[--g_nalias]; else i++; }
        if (wait_gpu) vk_gpu_idle();
        HK_ADD(idle_pool, 1);
        unback_pool(e);
        g_pool_bytes -= e->end - e->base;
    }
    pthread_mutex_unlock(&g_pool_mu);
    vk_wb_range_gone(e->base, e->end);
    if (e->kind == K_OWN) vk_tex_pool_gone(e->base, e->end);   /* after the pool lock: texture code takes the locks the other way round */
    free(e);
}
void mtl_pool_add(const void *mem, uint64_t size) { add_range(mem, size, 0); }
void mtl_pool_del(const void *mem) { del_range(mem, 1); }
/* Write-back retains a registration identity, never a dereferenceable pool_t. Landing holds this lock through
   its last guest store, so deletion/remapping cannot race it; use exact bounds, not page-rounded GPU backing. */
uint64_t vk_wb_pool_identity(const pool_t *pool, const void *address, uint64_t size) {
    uint64_t identity = 0; uintptr_t a = (uintptr_t)address;
    if (!pool || !pool->mem || !size || size > UINTPTR_MAX - a) return 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = g_npool - 1; i >= 0; i--) {
        pool_ent *p = g_pools[i];
        if (p->lo == (uintptr_t)pool->mem && p->hi - p->lo == pool->size && a >= p->lo && a + size <= p->hi) {
            identity = p->identity; break;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    return identity;
}
int vk_wb_pool_lock(uint64_t identity, const void *address, uint64_t size) {
    uintptr_t a = (uintptr_t)address;
    if (!identity || !size || size > UINTPTR_MAX - a) return 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        pool_ent *p = g_pools[i];
        if (p->identity == identity && a >= p->lo && a + size <= p->hi) return 1;
    }
    pthread_mutex_unlock(&g_pool_mu);
    return 0;
}
void vk_wb_pool_unlock(void) { pthread_mutex_unlock(&g_pool_mu); }
/* totk_vpool.c: a virtual pool's range, and its page aliases. */
void mtl_vpool_add(const void *mem, uint64_t size) { add_range(mem, size, 1); }
void mtl_vpool_del(const void *mem) { del_range(mem, 1); }
/* MapVirtual accepts overlapping requests in one batch: mmap replaces the old pages. Keep disjoint aliases matching
   that last-writer order, otherwise the alias-first scan and furthest-end index can select different (stale) mappings. */
static void alias_replace(uintptr_t va, uintptr_t end, pool_ent *owner, uint64_t off) {
    if (g_nalias + 2 > g_capalias) { g_capalias = g_capalias ? 2 * g_capalias : 256; g_alias = VK_REALLOC(g_alias, (size_t)g_capalias * sizeof *g_alias); }
    for (int i = 0; i < g_nalias; ) {
        alias_ent x = g_alias[i];
        if (va >= x.end || end <= x.va) { i++; continue; }
        if (va <= x.va && end >= x.end) { g_alias[i] = g_alias[--g_nalias]; continue; }
        if (x.va < va) {
            g_alias[i].end = va;
            if (x.end > end) g_alias[g_nalias++] = (alias_ent){end, x.end, x.owner, x.off + (end - x.va)};
        } else { g_alias[i].va = end; g_alias[i].off += end - x.va; }
        i++;
    }
    g_alias[g_nalias++] = (alias_ent){va, end, owner, off}; span_changed();
}
int mtl_pool_alias(void *dst, const void *src, uint64_t size) {
    mtl_init();
    if (g_pool_mode == POOLS_HOST) return 0;   /* plain shared pages: totk_vpool.c's mremap aliases them */
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *o = own_containing((uintptr_t)src, (uintptr_t)src + size);
    int ok = 0;
    if (o && o->fd >= 0) {
        uint64_t off = (uintptr_t)src - o->base;
        if (mmap(dst, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, o->fd, (off_t)off) == dst) {
            alias_replace((uintptr_t)dst, (uintptr_t)dst + size, o, off);
            ok = 1;
        } else LOGV("【红】virtual pool alias at %p (+%#llx): %s", dst, (unsigned long long)size, strerror(errno));
    }
    pthread_mutex_unlock(&g_pool_mu);
    return ok;
}
#ifdef TOTK_VK_TESTS
/* Replace lookup metadata without remapping memory: donor buffers stay alive throughout the delayed-worker test. */
void vk_record_test_alias(void *address, uint64_t source, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *owner = own_containing(source, source + size);
    if (!owner) DIE("recorder test alias has no donor");
    alias_replace((uintptr_t)address, (uintptr_t)address + size, owner, source - owner->base);
    pthread_mutex_unlock(&g_pool_mu);
}
#endif
static int span_cmp(const void *x, const void *y) { const span_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int span_seg_cmp(const void *x, const void *y) { const span_seg_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int span_further(int a, int b) { return g_span[a].end > g_span[b].end || (g_span[a].end == g_span[b].end && a > b); }
static void span_rebuild(void) {   /* caller holds g_pool_mu */
    int n = g_nalias + g_npool;
    if (n > g_capspan) { g_capspan = n + 256; g_span = VK_REALLOC(g_span, (size_t)g_capspan * sizeof *g_span); g_span_seg = VK_REALLOC(g_span_seg, (size_t)g_capspan * 2 * sizeof *g_span_seg); g_span_heap = VK_REALLOC(g_span_heap, (size_t)g_capspan * sizeof *g_span_heap); }
    g_nspan = 0;
    for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; g_span[g_nspan++] = (span_t){x->va, x->end, x->owner->buf, x->off}; }
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if (p->kind == K_OWN) g_span[g_nspan++] = (span_t){p->base, p->end, p->buf, 0};
        else if (p->kind == K_VIEW) g_span[g_nspan++] = (span_t){p->base, p->end, p->owner->buf, p->off};
    }
    qsort(g_span, (size_t)g_nspan, sizeof *g_span, span_cmp);
    for (int i = 0; i < g_nspan; i++) { g_span_seg[2 * i] = (span_seg_t){g_span[i].lo, -1}; g_span_seg[2 * i + 1] = (span_seg_t){g_span[i].end, -1}; }
    qsort(g_span_seg, (size_t)g_nspan * 2, sizeof *g_span_seg, span_seg_cmp);
    g_nseg = 0;
    for (int i = 0; i < 2 * g_nspan; i++) if (!g_nseg || g_span_seg[i].lo != g_span_seg[g_nseg - 1].lo) g_span_seg[g_nseg++].lo = g_span_seg[i].lo;
    /* Heap order matches the old backward scan exactly: largest end, then highest sorted span index. */
    int next = 0, nh = 0;
    for (int i = 0; i < g_nseg; i++) {
        uintptr_t x = g_span_seg[i].lo;
        while (next < g_nspan && g_span[next].lo <= x) {
            int v = next++, h = nh++;
            while (h && span_further(v, g_span_heap[(h - 1) / 2])) { g_span_heap[h] = g_span_heap[(h - 1) / 2]; h = (h - 1) / 2; }
            g_span_heap[h] = v;
        }
        while (nh && g_span[g_span_heap[0]].end <= x) {
            int v = g_span_heap[--nh], h = 0;
            while (2 * h + 1 < nh) {
                int c = 2 * h + 1;
                if (c + 1 < nh && span_further(g_span_heap[c + 1], g_span_heap[c])) c++;
                if (!span_further(g_span_heap[c], v)) break;
                g_span_heap[h] = g_span_heap[c]; h = c;
            }
            if (nh) g_span_heap[h] = v;
        }
        g_span_seg[i].best = nh ? g_span_heap[0] : -1;
    }
    /* Nested views often add endpoints without changing the furthest-reaching
       winner. Remove those non-boundaries: lookup and hint validation need only
       the maximal intervals over which that exact winning span stays the same. */
    int merged = 0;
    for (int i = 0; i < g_nseg; i++)
        if (!merged || g_span_seg[merged - 1].best != g_span_seg[i].best)
            g_span_seg[merged++] = g_span_seg[i];
    g_nseg = merged;
    g_span_dirty = 0;
}
/* A hint names an elementary segment, not the whole winning span: a nested span can win in only part of that pool. */
static int span_hint_matches(const span_seg_t *seg, int nseg, uintptr_t a, int hint) {
    return hint > 0 && hint < nseg && seg[hint - 1].lo <= a && a < seg[hint].lo;
}
static const span_t *span_best(const span_t *s, const span_seg_t *seg, int nseg, uintptr_t a, uintptr_t b, int *hint) {
    int lo = hint ? *hint : 0;
    if (!hint || !span_hint_matches(seg, nseg, a, lo)) {
        int hi = nseg; lo = 0; while (lo < hi) { int m = (lo + hi) >> 1; if (seg[m].lo <= a) lo = m + 1; else hi = m; }
        if (hint) *hint = lo;
    }
    if (!lo || seg[lo - 1].best < 0) return NULL;
    const span_t *best = &s[seg[lo - 1].best];
    return best->end >= b ? best : NULL;   /* if the furthest span containing a cannot reach b, none can */
}
static int span_linear(void) { static int linear = -1; if (linear < 0) linear = vk_off("span"); return linear; }
/* Which buffer holds [addr, addr + len), and how many bytes from addr it holds (*avail): caller holds g_pool_mu. */
static int resolve_span(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail) {
    uintptr_t a = (uintptr_t)addr, b = a + len;
    if (b < a) return 0;
    if (span_linear()) {   /* the old scan (TOTK_VK_OFF=span) */
        for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; if (a >= x->va && b <= x->end) { *buf = x->owner->buf; *off = x->off + (a - x->va); if (avail) *avail = x->end - a; return 1; } }
        for (int i = 0; i < g_npool; i++) {
            const pool_ent *p = g_pools[i];
            if (a < p->base || b > p->end) continue;
            if (p->kind == K_OWN) { *buf = p->buf; *off = a - p->base; if (avail) *avail = p->end - a; return 1; }
            if (p->kind == K_VIEW) { *buf = p->owner->buf; *off = p->off + (a - p->base); if (avail) *avail = p->end - a; return 1; }
        }
        return 0;
    }
    if (g_span_dirty) span_rebuild();
    const span_t *best = span_best(g_span, g_span_seg, g_nseg, a, b, NULL);
    if (!best) return 0;
    *buf = best->buf; *off = best->off + (a - best->lo); if (avail) *avail = best->end - a;
    return 1;
}
static int resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) { return resolve_span(addr, len, buf, off, NULL); }
/* The public lookups use the calling thread's own copy of the span index, refreshed when the pools change (rarely):
   the recording thread resolves several addresses per draw, and taking g_pool_mu for each (contended by the game's
   threads) was a tenth of its time. The lock never kept a buffer alive past the lookup, so a copy is as safe. */
enum { SPAN_HINTS = 64 };
typedef struct { uint64_t gen; span_t *s; span_seg_t *seg; int nseg, cap, hint[SPAN_HINTS]; } span_cache_t;
static __thread span_cache_t T_span;
/* Diagnostics count only the replay thread (including its snapshot collection), not concurrent game-thread lookups.
   Atomic counters allow present to report them even when async replay records on another thread. */
static _Atomic uint64_t g_lookup_calls[4], g_lookup_hits, g_lookup_tests;
static void lookup_count(int kind, const span_cache_t *t, uintptr_t a, int hint, int indexed) {
    if ((uintptr_t)pthread_self() != atomic_load_explicit(&g_lookup_thread, memory_order_relaxed)) return;
    atomic_fetch_add_explicit(&g_lookup_calls[kind], 1, memory_order_relaxed);
    if (indexed) {
        atomic_fetch_add_explicit(&g_lookup_tests, 1, memory_order_relaxed);
        if (span_hint_matches(t->seg, t->nseg, a, hint)) atomic_fetch_add_explicit(&g_lookup_hits, 1, memory_order_relaxed);
    }
}
void vk_lookup_stats(uint64_t frame) {
    if (!g_lookup_stats) return;
    static uint64_t last; uint64_t frames = frame - last; last = frame; if (!frames) return;
    uint64_t c[4]; for (int i = 0; i < 4; i++) c[i] = atomic_exchange_explicit(&g_lookup_calls[i], 0, memory_order_relaxed);
    uint64_t hit = atomic_exchange_explicit(&g_lookup_hits, 0, memory_order_relaxed), tests = atomic_exchange_explicit(&g_lookup_tests, 0, memory_order_relaxed);
    LOGV("frame %llu, pool lookups per frame: ubo/sbuf %.1f, vbuf/index %.1f, draw_touch %.1f, other %.1f; segment hints %.1f%% (%llu/%llu)",
         (unsigned long long)frame, (double)c[0] / frames, (double)c[1] / frames, (double)c[2] / frames, (double)c[3] / frames,
         tests ? 100.0 * hit / tests : 0, (unsigned long long)hit, (unsigned long long)tests);
}
static int resolve_local(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, int kind) {
    uintptr_t a = (uintptr_t)addr, b = a + len;
    if (b < a) return 0;
    if (span_linear()) { if (g_lookup_stats) lookup_count(kind, NULL, a, 0, 0); pthread_mutex_lock(&g_pool_mu); int ok = resolve_span(addr, len, buf, off, avail); pthread_mutex_unlock(&g_pool_mu); return ok; }
    span_cache_t *t = &T_span;   /* one dynamic TLS resolution per lookup */
    if (t->gen != atomic_load_explicit(&g_span_gen, memory_order_acquire)) {
        pthread_mutex_lock(&g_pool_mu);
        if (g_span_dirty) span_rebuild();
        if (g_nspan > t->cap) { t->cap = g_nspan + 256; t->s = VK_REALLOC(t->s, (size_t)t->cap * sizeof *t->s); t->seg = VK_REALLOC(t->seg, (size_t)t->cap * 2 * sizeof *t->seg); }
        memcpy(t->s, g_span, (size_t)g_nspan * sizeof *g_span); memcpy(t->seg, g_span_seg, (size_t)g_nseg * sizeof *g_span_seg);
        t->nseg = g_nseg; memset(t->hint, 0, sizeof t->hint); t->gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
        pthread_mutex_unlock(&g_pool_mu);
    }
    /* One last-segment hint thrashes between a draw's uniform, vertex and index pools: 69% of resolve_local samples at
       Lookout Landing were still in the binary search. Keep page-hashed hints; collisions only fall back to that search. */
    unsigned h = (unsigned)((a >> 12) ^ (a >> 24)) & (SPAN_HINTS - 1);
    if (g_lookup_stats) lookup_count(kind, t, a, t->hint[h], 1);
    const span_t *best = span_best(t->s, t->seg, t->nseg, a, b, &t->hint[h]);
    if (!best) return 0;
    *buf = best->buf; *off = best->off + (a - best->lo); if (avail) *avail = best->end - a;
    return 1;
}
/* Recorder snapshots never refresh from live globals on a worker. Keep replaced versions until slot reuse. */
struct vk_record_spans {
    struct vk_record_spans *next; uint64_t generation, bias_bits, dirty_epoch;
    span_t *span; span_seg_t *segment; int count, segments, linear;
    uint32_t *buffer_heads, *buffer_next, buffer_mask;
};
uint64_t vk_record_spans_generation(const vk_record_spans *spans) { return spans ? spans->generation : 0; }
static vk_record_spans *g_record_spans[8];
static uint64_t g_record_index_ns, g_record_index_versions;
static unsigned record_buffer_hash(VkBuffer buffer) {
    uint64_t x = (uint64_t)(uintptr_t)buffer; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    return (unsigned)x;
}
/* Keep all mappings, including lookup losers. O(n) construction matters when alias versions churn during a walk. */
static int record_spans_index(vk_record_spans *s) {
    if (!s->count) return 1;
    size_t cap = 1;
    while (cap < (size_t)s->count * 2) cap *= 2;
    if (cap > UINT32_MAX || cap > SIZE_MAX / sizeof *s->buffer_heads - (size_t)s->count) return 0;
    s->buffer_heads = malloc((cap + (size_t)s->count) * sizeof *s->buffer_heads);
    if (!s->buffer_heads) return 0;
    memset(s->buffer_heads, 0, cap * sizeof *s->buffer_heads);
    s->buffer_next = s->buffer_heads + cap; /* one allocation per immutable version; links need no zeroing */
    s->buffer_mask = (uint32_t)cap - 1;
    for (int i = s->count; i-- > 0; ) {
        unsigned h = record_buffer_hash(s->span[i].buf) & s->buffer_mask;
        s->buffer_next[i] = s->buffer_heads[h]; s->buffer_heads[h] = (uint32_t)i + 1;
    }
    return 1;
}
const vk_record_spans *vk_record_spans_capture(unsigned slot) {
    int lag = lag_wait_on(); /* initialize before the first frozen version can be reused by a lag packet */
    vk_record_spans *s = g_record_spans[slot];
    if (s && s->generation == atomic_load_explicit(&g_span_gen, memory_order_acquire)) return s;
    pthread_mutex_lock(&g_pool_mu);
    s = VK_CALLOC(1, sizeof *s); s->generation = atomic_load_explicit(&g_span_gen, memory_order_relaxed); s->linear = span_linear();
    if (lag) s->dirty_epoch = nvn_dirty_lag_epoch(); /* no per-span dirty-table lookups, and no arming */
    if (s->linear) {
        s->span = VK_MALLOC((size_t)(g_nalias + g_npool) * sizeof *s->span);
        for (int i = 0; i < g_nalias; i++) { const alias_ent *a = &g_alias[i]; s->span[s->count++] = (span_t){a->va, a->end, a->owner->buf, a->off}; }
        for (int i = 0; i < g_npool; i++) { const pool_ent *p = g_pools[i];
            if (p->kind == K_OWN) s->span[s->count++] = (span_t){p->base, p->end, p->buf, 0};
            else if (p->kind == K_VIEW) s->span[s->count++] = (span_t){p->base, p->end, p->owner->buf, p->off};
        }
    } else {
        if (g_span_dirty) span_rebuild();
        s->count = g_nspan; s->segments = g_nseg;
        s->span = VK_MALLOC((size_t)g_nspan * sizeof *s->span); s->segment = VK_MALLOC((size_t)g_nseg * sizeof *s->segment);
        if (g_nspan) memcpy(s->span, g_span, (size_t)g_nspan * sizeof *s->span);
        if (g_nseg) memcpy(s->segment, g_span_seg, (size_t)g_nseg * sizeof *s->segment);
    }
    for (int i = 0; i < s->count; i++) s->bias_bits |= s->span[i].off - s->span[i].lo;
    if (lag) {
        uint64_t start = host_now_ns();
        if (!record_spans_index(s)) {
            free(s->buffer_heads); s->buffer_heads = s->buffer_next = NULL;
        }   /* projection reports failure, never silently drops aliases or scans every span */
        g_record_index_ns += host_now_ns() - start; g_record_index_versions++;
    }
    pthread_mutex_unlock(&g_pool_mu);
    s->next = g_record_spans[slot]; g_record_spans[slot] = s; return s;
}
void vk_record_spans_reset(unsigned slot) {
    vk_record_spans *keep = g_record_spans[slot]; if (!keep) return;
    vk_record_spans *s = keep->next; keep->next = NULL;
    while (s) {
        vk_record_spans *next = s->next; free(s->span); free(s->segment);
        free(s->buffer_heads); free(s); s = next;
    }
}
int vk_record_storage_aligned(const vk_record_spans *s, uint64_t address) {
    uint64_t align = V.props.limits.minStorageBufferOffsetAlignment;
    return !(address % align) && !(s->bias_bits % align);
}
static int record_resolve_inner(const vk_record_spans *s, uint64_t a, uint64_t n, VkBuffer *b, uint64_t *o, uint64_t *available, int hints[64]) {
    if (a + n < a) return 0;
    const span_t *best = NULL;
    if (s->linear) { for (int i = 0; i < s->count; i++) if (s->span[i].lo <= a && s->span[i].end >= a + n) { best = &s->span[i]; break; } }
    else { unsigned h = (unsigned)((a >> 12) ^ (a >> 24)) & 63u; best = span_best(s->span, s->segment, s->segments, a, a + n, hints ? &hints[h] : NULL); }
    if (!best) return 0;
    *b = best->buf; *o = best->off + a - best->lo; if (available) *available = best->end - a; return 1;
}
int vk_record_resolve(const vk_record_spans *s, uint64_t a, uint64_t n, VkBuffer *b, uint64_t *o, uint64_t *available, int hints[64]) {
    return VK_REC_VALUE(VK_REC_RESOLVE, record_resolve_inner(s, a, n, b, o, available, hints));
}
int vk_resolve_for(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, int kind) { return VK_REC_VALUE(VK_REC_RESOLVE, resolve_local(addr, len, buf, off, avail, kind)); }
int vk_resolve_span(uint64_t addr, VkBuffer *buf, uint64_t *off, uint64_t *avail) { return vk_resolve_for(addr, 1, buf, off, avail, VK_LOOKUP_OTHER); }
int vk_resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) { return vk_resolve_for(addr, len, buf, off, NULL, VK_LOOKUP_OTHER); }
uint64_t vk_lag_bound(uint64_t addr, uint64_t limit) {
    enum { NB = 1024 };
    static struct { uint64_t addr, gen, bound; } cache[NB];
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    unsigned h = (unsigned)((addr >> 4) ^ (addr >> 20)) % NB;
    if (cache[h].addr == addr && cache[h].gen == gen) return limit < cache[h].bound ? limit : cache[h].bound;
    pthread_mutex_lock(&g_pool_mu);
    uint64_t physical = addr, bound = UINT64_MAX;
    for (int i = 0; i < g_nalias; i++) if (addr >= g_alias[i].va && addr < g_alias[i].end) {
        physical = g_alias[i].owner->base + g_alias[i].off + addr - g_alias[i].va;
        bound = g_alias[i].end - addr; break;
    }
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if ((p->kind == K_OWN || p->kind == K_VIEW) && physical >= p->lo && physical < p->hi && p->hi - physical < bound) bound = p->hi - physical;
    }
    gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    cache[h].addr = addr; cache[h].gen = gen; cache[h].bound = bound;
    return limit < bound ? limit : bound;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b; int ok = resolve_local(addr, len, &b, off, NULL, VK_LOOKUP_OTHER);
    if (ok) *buf = (void *)(uintptr_t)b;
    return ok;
}
/* mtl_pool_invalidate: vk_res.c (GPU write-back of renderings into CPU-pool textures) */
void gpu_pool_totals(uint64_t *n, uint64_t *bytes) { pthread_mutex_lock(&g_pool_mu); *n = (uint64_t)g_npool; *bytes = g_pool_bytes; pthread_mutex_unlock(&g_pool_mu); }

/* ---------- submissions ----------
   A submission is recorded into the "pre" and "main" command buffers of one slot of a small ring (vk_int.h). Its
   CPU-visible results (post ops) are applied by the completion thread once the slot's VkFence signals, in submission
   order, and only then is its fence value done. A binary fence per slot, not a timeline semaphore: Turnip on KGSL emulates
   timeline semaphores (Mesa vk_sync_timeline), and signalling one in vkQueueSubmit2 garbage-collects its points by waiting
   for the GPU, which held the recording thread about 30 ms a frame (half its time) and serialised CPU and GPU. Work the GPU
   cannot do (an address outside every pool) is done on the CPU at its place in the order: what is recorded so far is
   submitted and waited for first ("CPU step"). A slot's transient memory and descriptor sets are recycled when it is begun
   again (after completion). */
enum { OQ = 256 };
typedef struct occl_node { struct occl_node *prev; uint64_t total; unsigned refs; int ready; } occl_node;
typedef struct occl_page { struct occl_page *next; VkQueryPool qp; uint32_t used; occl_node *node[OQ]; } occl_page;
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type, cval; uint32_t tsq1; occl_node *occl; uint64_t report_ticks, report_origin; } post_op;   /* 1 event, 2 counter; cval fixed at recording, tsq1: timestamp query + 1; report provenance staged until g_mu publication */
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } chunk_t;
typedef struct { chunk_t *chunks; int nchunk, capchunk; VkDeviceSize min, reserve; } arena_t;   /* host-visible bump allocator; min: chunk size (0: 8 MiB), reserve: compacted high-water */
static uint64_t arena_compact_ns[2], arena_compacts[2]; /* walking only: slot / snapshot */
#define RING 8
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, pre; int pre_open;
    VkCommandBuffer adopted; /* closed, externally owned; never reset/freed by this slot */
    VkCommandBuffer *primary; VkCommandBufferSubmitInfo *submit; uint32_t nprimary, capprimary, capsubmit;
    uint32_t primary_trim_uses, recent_primary_peak;
    uint64_t value; post_op *post; int npost, cap; int busy; VkFence fence;
    int publication, join_on_walk, native_lag, submit_busy;
    uint64_t ft_frame, ft_gpu_done; /* walking-captured owner; fence observation published by the GPU cursor */
    uint64_t guest_submit_ns, guest_origin_ticks, guest_frame, queue_ns;
    uint32_t tsq_first, tsq_count;
    nvn_dirty_lag_batch walking_guards; /* walking-built, immutable until this slot's queue publication */
    arena_t ar;   /* transient memory */
    VkDescriptorPool *dpools; int ndpool, capdpool, curdpool, curdsets;   /* walk every reset pool before allocating another */
    vk_desc_stock *dstock;   /* TOTK_VK_DESC_STOCK=1: per-layout sets kept across this slot's generations */
    int dstock_on;           /* this slot's stock policy, switched only in slot_descriptors_recycle */
    VkQueryPool ts; int ts_on;   /* GPU timestamps at the start and end of the main command buffer */
    occl_page *oq, *oq_cur;   /* reset/reused only after this slot's fence and occl_complete */
    vk_gpuprof_slot *gpuprof;   /* query pool and records, reused only after the slot fence */
} slot_t;
static void chunk_release(chunk_t *c, vk_memory_purpose purpose) {
    vk_res_buffer_released(c->buf);
    vkUnmapMemory(V.dev, c->mem);
    vkDestroyBuffer(V.dev, c->buf, NULL);
    vk_memory_release(c->mem, purpose);
}
static void arena_release(arena_t *a) {
    for (int i = 0; i < a->nchunk; i++) chunk_release(&a->chunks[i], a->min ? VK_MEMORY_SNAPSHOT : VK_MEMORY_SLOT);
    a->nchunk = 0; a->reserve = 0;
}
/* Only called after the arena's last GPU fence AND CPU recording ownership
   have retired. Retain byte capacity, but collapse fragmented BO high-water
   once; steady-state reuse never allocates/frees merely to shrink memory. */
static void arena_reset(arena_t *a, int max_bos) {
    if (a->nchunk > max_bos) {
        uint64_t start = host_now_ns();
        VkDeviceSize reserve = 0;
        for (int i = 0; i < a->nchunk; i++) reserve += a->chunks[i].size;
        arena_release(a);
        a->reserve = reserve;
        unsigned kind = a->min != 0;
        arena_compacts[kind]++; arena_compact_ns[kind] += host_now_ns() - start;
    } else for (int i = 0; i < a->nchunk; i++) a->chunks[i].used = 0;
}
/* TOTK_VK_STAGING_RETAIN_MB (default unset: no cap). A slot's transient arena holds the staging of its texture uploads
   (vk_res.c), and arena_reset keeps whatever a submission needed, compacted into one high-water chunk: a burst of
   uploads at an area transition stays allocated in that slot for the session. With N, each slot keeps at most N MiB
   after its reset (whole chunks, oldest first, and a high-water hint clipped to N); a later submission that needs more
   allocates it again. Live (totk.ctl) at each slot open, after the slot's fence. */
static int g_staging_retain_launch = -1;   /* MiB, -1: no cap; parsed once (pthread_once) */
static _Atomic uint64_t g_staging_trims, g_staging_trimmed;
static pthread_once_t g_staging_once = PTHREAD_ONCE_INIT;
static void staging_retain_parse(void) {
    const char *e = getenv("TOTK_VK_STAGING_RETAIN_MB");
    if (e && *e) { long n = atol(e); g_staging_retain_launch = n < 8 ? 8 : n > 4096 ? 4096 : (int)n; }
    if (g_staging_retain_launch < 0) LOGV("staging retention: no cap (TOTK_VK_STAGING_RETAIN_MB unset: each slot keeps its upload high-water)");
    else LOGV("staging retention: %d MiB per slot (TOTK_VK_STAGING_RETAIN_MB; %d slots)", g_staging_retain_launch, RING);
}
static int staging_retain_mb(void) {
    static totk_live_cache live; static _Atomic int reported = -2;
    pthread_once(&g_staging_once, staging_retain_parse);
    int mb = totk_live_cached(&live, "TOTK_VK_STAGING_RETAIN_MB", g_staging_retain_launch);
    if (atomic_load_explicit(&reported, memory_order_relaxed) != mb &&
        atomic_exchange_explicit(&reported, mb, memory_order_relaxed) != mb && totk_live_generation())
        LOGV("staging retention: %d MiB per slot (live; -1 = no cap; each slot applies it at its next open)", mb);
    return mb;
}
static void arena_retain_cap(arena_t *a) {   /* a slot arena just reset: no chunk is in use */
    int mb = staging_retain_mb(); if (mb < 0) return;
    VkDeviceSize cap = (VkDeviceSize)mb << 20, kept = 0, trimmed = 0;
    if (a->reserve > cap) { trimmed += a->reserve - cap; a->reserve = cap; }
    int n = 0;
    for (int i = 0; i < a->nchunk; i++) {
        if (kept + a->chunks[i].size <= cap) { kept += a->chunks[i].size; a->chunks[n++] = a->chunks[i]; continue; }
        trimmed += a->chunks[i].size; chunk_release(&a->chunks[i], VK_MEMORY_SLOT);
    }
    a->nchunk = n;
    if (trimmed) {
        atomic_fetch_add_explicit(&g_staging_trims, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_staging_trimmed, trimmed, memory_order_relaxed);
    }
}
static slot_t g_ring[RING]; static int g_ring_next;
#ifdef TOTK_VK_TESTS
uint64_t vk_test_slot_arena_bytes(void) {   /* walking owner: every slot arena's chunk bytes */
    uint64_t bytes = 0;
    for (int i = 0; i < RING; i++) for (int c = 0; c < g_ring[i].ar.nchunk; c++) bytes += g_ring[i].ar.chunks[c].size;
    return bytes;
}
#endif
/* Separate from the ordinary slot layout and never consulted before an
 * external engine has passed its runtime admission checks. */
static struct { VkSemaphore signal, wait; uint64_t until; } g_external[RING];
static _Atomic int g_external_enabled;
void vk_submit_external_enable(void) { atomic_store_explicit(&g_external_enabled,1,memory_order_release); }
static uint64_t g_primary_reclaimed;
static void primary_memory(uint64_t values[2]) {
    values[0] = 0; values[1] = g_primary_reclaimed;
    for (int i = 0; i < RING; i++) values[0] += g_ring[i].capprimary ? g_ring[i].capprimary : !!g_ring[i].pool;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_primary_memory(uint64_t values[2]) { primary_memory(values); }
void vk_record_test_arena_costs(vk_record_test_costs *out) {
    memcpy(out->arena_ns, arena_compact_ns, sizeof arena_compact_ns);
    memcpy(out->arena_compacts, arena_compacts, sizeof arena_compacts);
}
#endif
static slot_t *g_cur;
static slot_t *g_optional_next; /* recorder-owned preclaimed external continuation */
static post_op *g_carry; static int g_ncarry, g_capcarry;
static uint64_t g_fence, g_reserved_fence; /* g_mu: actual queue publication versus walking reservations */
static _Atomic uint64_t g_done;
static uint64_t g_ub_publication;   /* g_mu: final ticket whose CPU-known UBO bytes must publish before posts/done */
static uint64_t g_done_at;   /* g_mu: publication time, sampled only by a waiter which actually blocked */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_cv_work = PTHREAD_COND_INITIALIZER;
/* Predict/auto exact-ticket history. Sampled independently of presenter FIFO
 * residence, AFTER CPU posts/publication prerequisites, at g_done availability.
 * This is a CPU observation, not the GPU's hardware signal timestamp. */
enum { PRESENT_READY_HISTORY = 1024 };
static struct { uint64_t ticket, ready_ns; } g_present_ready[PRESENT_READY_HISTORY];
uint64_t vk_present_ready_ns(uint64_t ticket) {
    if (!ticket) return 0;
    pthread_mutex_lock(&g_mu);
    unsigned index = (unsigned)(ticket % PRESENT_READY_HISTORY);
    uint64_t ready = g_present_ready[index].ticket == ticket ? g_present_ready[index].ready_ns : 0;
    pthread_mutex_unlock(&g_mu);
    return ready; /* frontend feedback must happen only AFTER releasing g_mu */
}
static slot_t *g_queue_[RING]; static int g_qhead, g_qtail;
static int g_gpu_head;   /* guard thread's GPU-only completion cursor; CPU post ops may wait on later GPU reads */
/* CPU handoff and GPU completion have separate cursors: a reserved ticket is not yet queue-visible. */
static slot_t *g_sealed[RING]; static uint64_t g_sealed_head, g_sealed_tail, g_submit_done_value, g_fallback_value;
static int g_submit_started;
static pthread_cond_t g_sealed_cv = PTHREAD_COND_INITIALIZER;
static _Atomic unsigned g_replay_pins; /* g_mu: distinct owners, bounded by RING-1 */
static slot_t *g_replay_pending; /* walking owner; at most one unsealed continuation */
/* Independent of external-engine admission: a terminal draw may retain the
 * real recorder's bindings across several split/continuation pairs. */
static struct { uint64_t token, until; unsigned snapshots; int pending; } g_replay_owner[RING];
static uint64_t g_replay_token;
static int g_optional_immutable;
static unsigned g_replay_releases; /* walking owner: releases awaiting an ordinary seal */
static void snapshot_replay_pin(uint64_t until);
static unsigned snapshot_replay_mask(void);
static uint64_t replay_until(unsigned slot) { /* g_mu */
    uint64_t split=g_external[slot].until, owner=g_replay_owner[slot].until;
    return split>owner ? split : owner;
}
static void replay_retire(uint64_t done) { /* g_mu; also called after submit-owner retirement */
    if (!g_replay_pins) return;
    for (unsigned i=0;i<RING;i++) {
        if (g_ring[i].busy || g_ring[i].submit_busy || !replay_until(i)) continue;
        if (g_external[i].until && g_external[i].until<=done) g_external[i].until=0;
        if (g_replay_owner[i].until && g_replay_owner[i].until<=done) {
            g_replay_owner[i].until=0; g_replay_owner[i].snapshots=0;
        }
        if (!replay_until(i)) { g_replay_pins--; vk_record_slot_retire(i); }
    }
}
int vk_submit_external_disable(void) {
    if (!atomic_load_explicit(&g_external_enabled,memory_order_acquire)) return 1;
    pthread_mutex_lock(&g_mu);
    int idle=!g_replay_pins && !g_replay_pending;
    for (unsigned i=0;idle && i<RING;i++)
        idle=!g_external[i].until && !g_external[i].signal && !g_external[i].wait;
    if (idle) atomic_store_explicit(&g_external_enabled,0,memory_order_release);
    pthread_mutex_unlock(&g_mu);
    return idle; /* no GPU wait: retry after genuine continuation retirement */
}
#ifdef TOTK_VK_TESTS
static uint64_t g_test_replay_skips; /* g_mu */
static uint64_t g_test_completion_hold; /* g_mu; actual GPU fence precedes this gate */
static _Atomic uint64_t g_test_completion_held;
#endif
static int g_in_sub;
static __thread struct { uint64_t ns, origin_ticks, frame; } T_gpu_time_pending;
static uint64_t g_gpu_guest_ns, g_gpu_origin_ticks, g_gpu_guest_frame;
void vk_gpu_time_guest_submit(uint64_t ns, uint64_t origin_ticks, uint64_t frame) {
    T_gpu_time_pending.ns = ns; T_gpu_time_pending.origin_ticks = origin_ticks; T_gpu_time_pending.frame = frame;
}
static __thread uint64_t T_ft_walk_frame; /* this thread's NVN walk, including its final publication join */
static uint64_t g_slot_count, g_game_slot_count, g_handoff_count, g_submit_join_ns, g_fallback_consumed;
static _Atomic uint64_t g_queue_call_ns;
static uint64_t g_stat_sub, g_stat_draw, g_stat_gpu_copy, g_stat_cpu_step, g_stat_ub, g_stat_ub_renamed;
static _Atomic uint64_t g_seal_serial = 1; /* cache generation follows stream seals, never asynchronous publication */
uint64_t vk_submit_serial(void) { return atomic_load(&g_seal_serial); }
uint64_t vk_game_submit_serial(void) { return g_stat_sub + 1; }

/* Submission timing, recording-lock owned. Raw elapsed brackets the thread CPU clock pair; the legacy
   recording value subtracts explicit slot/GPU waits only. Worker/publication joins remain in elapsed.
   submit_exec calls begin/commit synchronously on the same walking thread, even with async replay. */
static uint64_t g_perf_replay_ns, g_perf_wait_ns, g_perf_sub0, g_perf_t0, g_perf_cpu_t0;
static uint64_t g_perf_cpu_ns, g_perf_elapsed_ns; static _Atomic uint64_t g_perf_gpu_ns; static uint64_t g_perf_subs;
static uint64_t walker_cpu_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) DIE("walker thread CPU clock");
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* running totals for the long-frame log (vk_draw.c): recording ns, recording-thread waits for the GPU ns, the game's
   blocking sync waits ns (mtl_wait), CPU steps (a flush and wait so the CPU reads GPU results), blocking sync waits */
static _Atomic uint64_t g_cum[5];
void vk_perf_cum(uint64_t c[5]) { for (int i = 0; i < 5; i++) c[i] = atomic_load(&g_cum[i]); }
static int ts_ok(void) { static int ok = -1; if (ok < 0) ok = V.props.limits.timestampComputeAndGraphics && V.props.limits.timestampPeriod > 0 && !vk_off("ts"); return ok; }
void vk_perf_take(double *replay_ms, double *wait_ms, double *gpu_ms, double *cpu_ms, double *elapsed_ms, uint64_t *subs) {
    *replay_ms = g_perf_replay_ns / 1e6; *wait_ms = g_perf_wait_ns / 1e6; *gpu_ms = atomic_exchange(&g_perf_gpu_ns, 0) / 1e6; *subs = g_perf_subs;
    *cpu_ms = g_perf_cpu_ns / 1e6; *elapsed_ms = g_perf_elapsed_ns / 1e6;
    g_perf_replay_ns = g_perf_wait_ns = g_perf_cpu_ns = g_perf_elapsed_ns = 0; g_perf_subs = 0;
}
static uint64_t gpu_ticks(void) { return guest_clock_ns_to_ticks(runtime_now_ns()); }
/* Counter timestamps must share GetSystemTick's CLOCK_MONOTONIC epoch: the game
   subtracts its CPU frame origin from the GPU END report, not GPU end-start.
   Calibrate the clocks directly, never against a completed submission. */
enum { TSQ = 1024 };
static VkQueryPool g_tsq; static uint32_t g_tsq_next; static _Atomic uint8_t g_tsq_busy[TSQ]; static int g_ts_on = -1;
static int64_t g_ts_off; static int g_ts_cal;
static uint64_t g_ts_gpu_sample, g_ts_host_sample, g_ts_refresh_ns;
static int g_ts_clock_valid;
static vk_gpu_time_queue g_gpu_time_queue; /* single Vulkan queue, ordered completion owner */
static int g_gpu_time_uncertain;
static vk_gpu_time_samples g_gpu_time_samples; /* publication and FPS reads under g_mu */
enum { GC_SAMPLES, GC_REBASED, GC_INVALID, GC_WITHHELD, GC_COUNT };
static uint64_t g_gpu_controller_stats[GC_COUNT]; /* g_mu, not the completion-only stats */
static vk_gpu_time_fps_filter g_gpu_fps_filter; /* g_mu, optional FPS-only policy */
static uint64_t g_gpu_fps_filter_reads, g_gpu_fps_filter_changed;
static uint64_t g_gpu_fps_filter_before, g_gpu_fps_filter_after;
int vk_gpu_time_hardware(void) { return g_gpu_time_hardware; }
uint64_t vk_gpu_time_controller_end(uint64_t addr, uint64_t timestamp, uint64_t origin) {
    int valid, rebased;
    pthread_mutex_lock(&g_mu);
    uint64_t end = vk_gpu_time_sample_consume(&g_gpu_time_samples, addr, timestamp, origin, &valid, &rebased);
    g_gpu_controller_stats[GC_SAMPLES]++;
    g_gpu_controller_stats[GC_REBASED] += rebased;
    g_gpu_controller_stats[GC_INVALID] += !valid;
    g_gpu_controller_stats[GC_WITHHELD] += !valid;
    if (g_gpu_time_fps_median) {
        uint64_t work = end - origin;
        uint64_t filtered = vk_gpu_time_fps_median(&g_gpu_fps_filter, addr, timestamp, work, valid);
        g_gpu_fps_filter_reads++;
        g_gpu_fps_filter_changed += work != filtered;
        g_gpu_fps_filter_before += work;
        g_gpu_fps_filter_after += filtered;
        end = origin + filtered;
    }
    pthread_mutex_unlock(&g_mu);
    /* The listener converts ns back to ticks before subtracting. Round UP here
       so a held input, including initial zero, survives that round trip exactly. */
    return (uint64_t)(((__uint128_t)end * 625 + 11) / 12);
}
static void gpu_time_unavailable(uint64_t frame, const char *reason) {
    if (!g_gpu_time_hardware) return;
    if (!g_gpu_time_uncertain)
        LOGV("[fps-policy] GPU report time hardware unavailable at producer frame %llu: %s; latency reports until restart",
             (unsigned long long)frame, reason);
    g_gpu_time_uncertain = 1; /* Unknown GPU work must never disappear from later backlog estimates. */
}
static struct {
    vk_gpu_time_slot slot;
    uint64_t origin_ticks, frame, clock_gpu, clock_guest;
    int valid;
} g_tsq_time[TSQ]; /* retained until the carried post consumes this query, even after its slot is reused */
enum { GT_REPORTS, GT_REMOVED, GT_WORK_COUNT, GT_WORK_BEFORE, GT_WORK_AFTER, GT_FALLBACK,
       GT_SLOTS, GT_SUBMIT_DELAY, GT_MONOTONIC, GT_COUNT };
static uint64_t g_gpu_time_stats[GT_COUNT]; /* completion-owned */
static uint64_t g_gpu_time_published[GT_COUNT]; /* snapshot under existing g_mu */
static void gpu_time_stats(uint64_t frame) {
    uint64_t v[GT_COUNT], c[GC_COUNT]; static uint64_t previous[GT_COUNT], previous_controller[GC_COUNT];
    uint64_t filter[4]; static uint64_t previous_filter[4];
    pthread_mutex_lock(&g_mu);
    memcpy(v, g_gpu_time_published, sizeof v); memcpy(c, g_gpu_controller_stats, sizeof c);
    filter[0] = g_gpu_fps_filter_reads; filter[1] = g_gpu_fps_filter_changed;
    filter[2] = g_gpu_fps_filter_before; filter[3] = g_gpu_fps_filter_after;
    pthread_mutex_unlock(&g_mu);
    for (unsigned i = 0; i < GT_COUNT; i++) { uint64_t current = v[i]; v[i] -= previous[i]; previous[i] = current; }
    for (unsigned i = 0; i < GC_COUNT; i++) { uint64_t current = c[i]; c[i] -= previous_controller[i]; previous_controller[i] = current; }
    LOGV("[fps-policy] GPU report time frame %llu mode=%s: %llu reports, removed avg %.3f ms; guest-submit -> Vulkan-submit avg %.3f ms (%llu slots); controller GPU end-origin before/after %.3f/%.3f ms (%llu end-report samples, captured origin); fallback %llu monotonic-clamps %llu; controller samples %llu rebased %llu invalid %llu withheld %llu",
         (unsigned long long)frame, g_gpu_time_hardware ? "hardware" : "latency",
         (unsigned long long)v[GT_REPORTS], v[GT_REPORTS] ? v[GT_REMOVED] / (1e6 * v[GT_REPORTS]) : 0,
         v[GT_SLOTS] ? v[GT_SUBMIT_DELAY] / (1e6 * v[GT_SLOTS]) : 0, (unsigned long long)v[GT_SLOTS],
         v[GT_WORK_COUNT] ? v[GT_WORK_BEFORE] / (1e6 * v[GT_WORK_COUNT]) : 0,
         v[GT_WORK_COUNT] ? v[GT_WORK_AFTER] / (1e6 * v[GT_WORK_COUNT]) : 0,
         (unsigned long long)v[GT_WORK_COUNT], (unsigned long long)v[GT_FALLBACK],
         (unsigned long long)v[GT_MONOTONIC], (unsigned long long)c[GC_SAMPLES],
         (unsigned long long)c[GC_REBASED], (unsigned long long)c[GC_INVALID], (unsigned long long)c[GC_WITHHELD]);
    if (g_gpu_time_fps_median) {
        for (unsigned i = 0; i < 4; i++) {
            uint64_t current = filter[i]; filter[i] -= previous_filter[i]; previous_filter[i] = current;
        }
        LOGV("[fps-policy] GPU FPS median frame %llu: %llu reads, %llu changed; raw/filtered avg %.3f/%.3f ms",
             (unsigned long long)frame, (unsigned long long)filter[0], (unsigned long long)filter[1],
             filter[0] ? filter[2] / (19200.0 * filter[0]) : 0,
             filter[0] ? filter[3] / (19200.0 * filter[0]) : 0);
    }
}
/* Before any real submission or completion worker exists, bracket three tiny
   empty timestamp submissions with the SAME clock provider as the timer gates.
   The narrowest bracket supplies an honest midpoint and uncertainty, not the
   completion time of an arbitrary production submission. No hot-path wait or
   periodic queue drain is introduced on drivers without DEVICE calibration. */
static void ts_clock_init(void) {
    const char *enabled = getenv("TOTK_VK_GPU_TIMESTAMPS");
    if ((enabled && !strcmp(enabled, "0")) || !f12.hostQueryReset || !ts_ok()) return;
    VkQueryPool pool; VkCommandPool commands; VkCommandBuffer cb; VkFence fence;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 1};
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = V.qfam};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &pool));
    VKCHECK(vkCreateCommandPool(V.dev, &pi, NULL, &commands));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &cb));
    VKCHECK(vkCreateFence(V.dev, &fi, NULL, &fence));
    uint64_t width = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        VKCHECK(vkResetCommandPool(V.dev, commands, 0));
        VKCHECK(vkResetFences(V.dev, 1, &fence));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(cb, &bi));
        vkCmdResetQueryPool(cb, pool, 0, 1);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, 0);
        VKCHECK(vkEndCommandBuffer(cb));
        VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
        uint64_t before = runtime_now_ns();
        VKCHECK(vkQueueSubmit2(V.queue, 1, &si, fence));
        VKCHECK(vkWaitForFences(V.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        uint64_t after = runtime_now_ns(), device;
        VKCHECK(vkGetQueryPoolResults(V.dev, pool, 0, 1, sizeof device, &device, sizeof device, VK_QUERY_RESULT_64_BIT));
        if (after - before < width) {
            width = after - before; g_ts_gpu_sample = device;
            g_ts_host_sample = before + width / 2;
        }
    }
    vkDestroyFence(V.dev, fence, NULL); vkDestroyCommandPool(V.dev, commands, NULL);
    vkDestroyQueryPool(V.dev, pool, NULL); g_ts_clock_valid = 1;
    LOGV("[fps-policy] GPU timestamp clock: bounded guest-clock query, uncertainty <= %llu ns; device %llu guest-ns %llu host-ns %llu period %.9g ns bits %u",
         (unsigned long long)((width + 1) / 2), (unsigned long long)g_ts_gpu_sample,
         (unsigned long long)g_ts_host_sample, (unsigned long long)host_now_ns(),
         V.props.limits.timestampPeriod, V.timestamp_bits);
}
static void ts_calibrate(void) {
    if (!g_calibrated_timestamps) return;
    uint64_t now = host_now_ns();
    if (g_ts_refresh_ns && now - g_ts_refresh_ns < UINT64_C(1000000000)) return;
    g_ts_refresh_ns = now;
    VkCalibratedTimestampInfoEXT info[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}
    };
    uint64_t best[2] = {0}, deviation = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        uint64_t sample[2], error, host_before = host_now_ns();
        uint64_t guest_before = runtime_now_ns();
        if (g_calibrated_timestamps(V.dev, 2, info, sample, &error) == VK_SUCCESS && error < deviation) {
            deviation = error; best[0] = sample[0];
            best[1] = guest_clock_rebase_ns(sample[1], host_before, guest_before);
        }
    }
    if (deviation == UINT64_MAX) {
        if (!g_ts_clock_valid) LOGV("[fps-policy] GPU timestamp clock calibration failed; using completion-anchored fallback");
        return;
    }
    static int reported;
    if (!reported) {
        LOGV("[fps-policy] GPU timestamp clock: calibrated CLOCK_MONOTONIC mapped to guest clock, maximum deviation %llu ns; device %llu guest-ns %llu period %.9g ns, valid bits %u",
             (unsigned long long)deviation, (unsigned long long)best[0], (unsigned long long)best[1],
             V.props.limits.timestampPeriod, V.timestamp_bits);
        reported = 1;
    }
    g_ts_gpu_sample = best[0]; g_ts_host_sample = best[1]; g_ts_clock_valid = 1;
}
static int ts_ok(void);
static int ts_reports_on(void) {
    if (g_ts_on < 0) {
        const char *e = getenv("TOTK_VK_GPU_TIMESTAMPS");
        g_ts_on = !(e && !strcmp(e, "0")) && f12.hostQueryReset && ts_ok();
        if (g_ts_on) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TSQ};
            if (vkCreateQueryPool(V.dev, &qi, NULL, &g_tsq)) g_ts_on = 0; else vkResetQueryPool(V.dev, g_tsq, 0, TSQ);
        }
        LOGV("GPU timestamps for the game's counter reports: %s", g_ts_on ? "on" : f12.hostQueryReset ? "off" : "off (no hostQueryReset)");
    }
    return g_ts_on;
}

/* ---------- cumulative samples-passed counter (TOTK_VK_OCCLUSION=1) ----------
   A Vulkan query cannot cross a dynamic-rendering instance. Close it at pass end, ResetCounter and ReportCounter;
   the next draw opens a new segment, without splitting the pass at a reset/report. Queries are precise, not boolean.
   Slot-local pools are host-reset only after completion (hostQueryReset is enabled in f12), so starting a segment in
   the middle of a pass needs no illegal vkCmdResetQueryPool there. New pool pages are host-reset before first use.

   Each segment owns a prefix node: its predecessor's cumulative total plus this query. The completion thread resolves
   nodes in queue/query order BEFORE applying posts or recycling the slot. Reports retain their exact prefix, not an
   address lookup or the live counter. Thus reports survive later resets, duplicate addresses, chunk/ring reuse, and
   submissions without a reset. Resolving a node detaches its predecessor; even a counter never reset keeps only its
   last resolved total plus in-flight nodes, not the whole history. Nodes/pool pages are recycled at their high-water mark. */
static pthread_mutex_t g_occl_mu = PTHREAD_MUTEX_INITIALIZER;
static occl_node *g_occl_tail, *g_occl_free;
static VkQueryPool g_occl_active; static uint32_t g_occl_q;
static uint64_t g_occl_reports, g_occl_zero, g_occl_segments;   /* g_occl_mu: keep zero/total snapshots consistent */
static uint64_t g_occl_unknown;   /* recording-thread pending sample lookups used by conditions */
static void occl_drop(occl_node *p) {   /* g_occl_mu held; each live node owns its predecessor */
    while (p && !--p->refs) { occl_node *prev = p->prev; p->prev = g_occl_free; g_occl_free = p; p = prev; }
}
void vk_occlusion_draw(VkCommandBuffer cb) {   /* immediately before a draw, inside rendering */
    if (!g_occl_on || g_occl_active) return;
    occl_page **at = g_cur->oq_cur ? &g_cur->oq_cur->next : &g_cur->oq;
    if (!g_cur->oq_cur || g_cur->oq_cur->used == OQ) {
        if (!*at) {
            *at = calloc(1, sizeof **at); if (!*at) DIE("occlusion query page allocation");
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ};
            VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &(*at)->qp));
            vkResetQueryPool(V.dev, (*at)->qp, 0, OQ);
        }
        g_cur->oq_cur = *at;
    }
    occl_page *p = g_cur->oq_cur;
    pthread_mutex_lock(&g_occl_mu);
    if (!g_occl_free) {
        occl_node *b = calloc(OQ, sizeof *b); if (!b) DIE("occlusion prefix allocation");
        for (int i = 0; i < OQ; i++) { b[i].prev = g_occl_free; g_occl_free = &b[i]; }
    }
    occl_node *n = g_occl_free; g_occl_free = n->prev;
    *n = (occl_node){.prev = g_occl_tail, .refs = 2};   /* slot completion + current prefix; transfer the old prefix's ref */
    g_occl_tail = n; p->node[p->used] = n;
    pthread_mutex_unlock(&g_occl_mu);
    g_occl_active = p->qp; g_occl_q = p->used++;
    vkCmdBeginQuery(cb, g_occl_active, g_occl_q, VK_QUERY_CONTROL_PRECISE_BIT);
}
void vk_occlusion_end(VkCommandBuffer cb) {   /* still inside the SAME rendering instance as BeginQuery */
    if (!g_occl_active) return;
    vkCmdEndQuery(cb, g_occl_active, g_occl_q); g_occl_active = VK_NULL_HANDLE;
}
void vk_occlusion_reset(void) {
    if (!g_occl_on) return;
    if (g_occl_active) vk_occlusion_end(vk_cb());
    pthread_mutex_lock(&g_occl_mu); occl_drop(g_occl_tail); g_occl_tail = NULL; pthread_mutex_unlock(&g_occl_mu);
}
static occl_node *occl_snapshot(void) {
    if (!g_occl_on) return NULL;
    if (g_occl_active) vk_occlusion_end(vk_cb());
    pthread_mutex_lock(&g_occl_mu);
    occl_node *n = g_occl_tail; if (n) n->refs++;
    pthread_mutex_unlock(&g_occl_mu);
    return n;
}
static void occl_complete(slot_t *s) {   /* fence signalled; every earlier slot has already been resolved */
    for (occl_page *p = s->oq; p && p->used; p = p->next) {
        uint64_t r[OQ][2];
        VKCHECK(vkGetQueryPoolResults(V.dev, p->qp, 0, p->used, (size_t)p->used * sizeof r[0], r, sizeof r[0],
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
        pthread_mutex_lock(&g_occl_mu);
        for (uint32_t i = 0; i < p->used; i++) {
            occl_node *n = p->node[i], *prev = n->prev;
            if (!r[i][1] || (prev && !prev->ready)) DIE("occlusion query/prefix unavailable after fence");
            n->total = r[i][0] + (prev ? prev->total : 0); n->ready = 1; n->prev = NULL;
            occl_drop(prev); occl_drop(n);   /* release predecessor and slot ownership; reports/current prefix retain totals */
        }
        g_occl_segments += p->used;
        pthread_mutex_unlock(&g_occl_mu);
    }
}
static uint64_t occl_result(occl_node *n) {   /* apply_posts, after all query segments up to the report have completed */
    if (!g_occl_on) return 1u << 20;
    pthread_mutex_lock(&g_occl_mu);
    if (n && !n->ready) DIE("occlusion report prefix unavailable after fence");
    uint64_t total = n ? n->total : 0; occl_drop(n);
    g_occl_reports++; g_occl_zero += total == 0;
    pthread_mutex_unlock(&g_occl_mu);
    return total;
}
void vk_occlusion_stats(uint64_t frame) {   /* present, recording lock held; completions never waited for here */
    if (!g_occl_on) return;
    pthread_mutex_lock(&g_occl_mu);
    uint64_t n = g_occl_reports, zero = g_occl_zero, seg = g_occl_segments; g_occl_reports = g_occl_zero = g_occl_segments = 0;
    pthread_mutex_unlock(&g_occl_mu);
    LOGV("[occl] frame %llu: %.1f queries/frame (%llu completed reports), %.1f%% reported 0; %.1f Vulkan segments/frame; %llu pending sample condition reads kept visible",
         (unsigned long long)frame, n / 300.0, (unsigned long long)n, n ? 100.0 * zero / n : 0, seg / 300.0, (unsigned long long)g_occl_unknown);
    g_occl_unknown = 0;
}
/* Counter reports recorded but not yet in memory (apply_posts writes them when their submission completes), oldest first.
   A render condition reads the newest pending report of an address instead of memory (nvn_mtl.m cpend): TOTK guards a
   clear with "the counters at a and a + 16 differ" and right after it reports a counter to a, so the clear runs once;
   reading memory, every guarded clear in the submission ran again (the linear depth the terrain tests against was wiped:
   no terrain, the sky layer showed through as a flat wedge). Non-sample values are known at recording. Pending real
   sample values are unknown: conditions involving them stay enabled, as on Metal, rather than comparing stale memory. */
enum { CPEND_N = 4096 };
static struct cpend_ent { uint64_t addr, val; int known; } *g_cpend;
static unsigned g_cpend_cap, g_cpend_head, g_cpend_tail;
static pthread_mutex_t g_cpend_mu = PTHREAD_MUTEX_INITIALIZER;
static void cpend_push(uint64_t addr, uint64_t val, int known) {
    pthread_mutex_lock(&g_cpend_mu);
    if (g_cpend_head - g_cpend_tail == g_cpend_cap) {   /* never discard an unknown report and accidentally read stale memory */
        unsigned cap = g_cpend_cap ? 2 * g_cpend_cap : CPEND_N;
        struct cpend_ent *p = malloc((size_t)cap * sizeof *p); if (!p) DIE("pending counter allocation");
        for (unsigned i = g_cpend_tail; i != g_cpend_head; i++) p[i % cap] = g_cpend[i % g_cpend_cap];
        free(g_cpend); g_cpend = p; g_cpend_cap = cap;
    }
    g_cpend[g_cpend_head++ % g_cpend_cap] = (struct cpend_ent){addr, val, known};
    pthread_mutex_unlock(&g_cpend_mu);
}
static void cpend_pop(uint64_t addr) {   /* the oldest pending report is now in memory */
    pthread_mutex_lock(&g_cpend_mu);
    if (g_cpend_tail != g_cpend_head && g_cpend[g_cpend_tail % g_cpend_cap].addr == addr) g_cpend_tail++;
    pthread_mutex_unlock(&g_cpend_mu);
}
int vk_counter_value(uint64_t addr, uint64_t *value) {   /* 0: pending real samples, whose GPU value is not known yet */
    static int ordered = -1; if (ordered < 0) ordered = !vk_off("condorder");
    if (!ordered) { memcpy(value, (const void *)(uintptr_t)addr, 8); return 1; }
    int found = 0, known = 1; *value = 0;
    pthread_mutex_lock(&g_cpend_mu);
    for (unsigned i = g_cpend_head; i != g_cpend_tail && !found; i--) if (g_cpend[(i - 1) % g_cpend_cap].addr == addr) {
        const struct cpend_ent *p = &g_cpend[(i - 1) % g_cpend_cap]; *value = p->val; known = p->known; found = 1;
    }
    if (!found) memcpy(value, (const void *)(uintptr_t)addr, 8);
    pthread_mutex_unlock(&g_cpend_mu);
    if (!known) g_occl_unknown++;
    return known;
}
static void apply_posts(slot_t *owner, const vk_gpu_time_slot *last_slot, int last_valid) {
    post_op *p = owner->post; int n = owner->npost;
    for (int i = 0; i < n; i++) {
        post_op *o = &p[i];
        if (o->kind == 1) {
            if (lag_wait_on()) { if (!nvn_dirty_lag_store32(o->val, o->v, o->mode == 1)) DIE("lag event store could not restore protection"); }
            else if (o->mode == 1) *o->val += o->v; else *o->val = o->v;
        }
        else {
            uint64_t ts = gpu_ticks();
            const vk_gpu_time_slot *timing = last_slot;
            uint64_t origin_ticks = owner->guest_origin_ticks;
            int valid = last_valid;
            if (o->tsq1) {
                unsigned q = o->tsq1 - 1;
                timing = &g_tsq_time[q].slot; origin_ticks = g_tsq_time[q].origin_ticks;
                valid = g_tsq_time[q].valid;
            }
            if (!o->tsq1) gpu_time_unavailable(owner->guest_frame, "point query disabled or ring full");
            if (g_gpu_time_uncertain) valid = 0;
            if (o->tsq1) {
                uint32_t q = o->tsq1 - 1; uint64_t v;
                if (!vkGetQueryPoolResults(V.dev, g_tsq, q, 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT)) {
                    if (g_ts_clock_valid) {
                        unsigned q = o->tsq1 - 1;
                        uint64_t clock_gpu = g_gpu_time_hardware && valid ? g_tsq_time[q].clock_gpu : g_ts_gpu_sample;
                        uint64_t clock_guest = g_gpu_time_hardware && valid ? g_tsq_time[q].clock_guest : g_ts_host_sample;
                        ts = vk_timestamp_ticks(v, clock_gpu, clock_guest, V.props.limits.timestampPeriod, V.timestamp_bits);
                    } else {
                        uint64_t t = (uint64_t)((__uint128_t)(uint64_t)((double)v * V.props.limits.timestampPeriod) * 12 / 625);
                        if (!g_ts_cal) { g_ts_off = (int64_t)ts - (int64_t)t; g_ts_cal = 1; }
                        ts = (uint64_t)((int64_t)t + g_ts_off);
                    }
                } else {
                    valid = 0;
                    gpu_time_unavailable(g_tsq_time[q].frame, "point query read failed");
                }
                vkResetQueryPool(V.dev, g_tsq, q, 1);
            }
            uint64_t raw_ns = guest_clock_ticks_to_ns(ts), report_ns = raw_ns;
            if (g_gpu_time_hardware && valid) {
                uint64_t unclamped = timing->start_ns + (raw_ns > timing->raw_start_ns ? raw_ns - timing->raw_start_ns : 0);
                if (unclamped < g_gpu_time_queue.report_ns) g_gpu_time_stats[GT_MONOTONIC]++;
                report_ns = vk_gpu_time_report(&g_gpu_time_queue, timing, raw_ns, 0);
                ts = guest_clock_ns_to_ticks(report_ns);
            } else if (g_gpu_time_hardware) {
                g_gpu_time_stats[GT_FALLBACK]++;
                if (report_ns < timing->guest_ns) report_ns = timing->guest_ns;
                if (report_ns < g_gpu_time_queue.report_ns) report_ns = g_gpu_time_queue.report_ns;
                g_gpu_time_queue.report_ns = report_ns;
                ts = guest_clock_ns_to_ticks(report_ns);
            }
            o->report_ticks = ts;
            o->report_origin = valid ? origin_ticks : 0;
            g_gpu_time_stats[GT_REPORTS]++;
            if (raw_ns > report_ns) g_gpu_time_stats[GT_REMOVED] += raw_ns - report_ns;
            if (origin_ticks && o->type == NVN_COUNTER_TYPE_TIMESTAMP) {
                uint64_t origin = guest_clock_ticks_to_ns(origin_ticks);
                if (raw_ns >= origin && report_ns >= origin) {
                    g_gpu_time_stats[GT_WORK_COUNT]++;
                    g_gpu_time_stats[GT_WORK_BEFORE] += raw_ns - origin;
                    g_gpu_time_stats[GT_WORK_AFTER] += report_ns - origin;
                }
            }
            if (o->tsq1) atomic_store(&g_tsq_busy[o->tsq1 - 1], 0);
            uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? (g_occl_on ? occl_result(o->occl) : o->cval) : 0, ts};
            if (lag_wait_on()) { if (!nvn_dirty_lag_store((void *)(uintptr_t)o->addr, rec, sizeof rec)) DIE("lag counter store could not restore protection"); }
            else memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
            cpend_pop(o->addr);
        }
    }
}
static void gpu_wait_slot(slot_t *s) {
    VkResult r = vkWaitForFences(V.dev, 1, &s->fence, VK_TRUE, 5000000000ull);
    for (int waited = 5; r == VK_TIMEOUT; waited += 5) {
        LOGV("【红】the GPU has not finished submission %llu after %d s (last finished: %llu)", (unsigned long long)s->value, waited, (unsigned long long)atomic_load(&g_done));
        r = vkWaitForFences(V.dev, 1, &s->fence, VK_TRUE, 5000000000ull);
    }
    if (r) VK_DIE_RESULT(r, "waiting for the GPU (submission %llu): %d%s", (unsigned long long)s->value, r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
}
/* Never writes guest memory. A post-op write may fault on a page a LATER submitted frame reads. The original
   completion thread cannot publish that later GPU fence while blocked in its own post op, so a GPU-only cursor is
   required; publishing before this submission's posts alone is insufficient. Slots stay busy until both cursors pass. */
static void *gpu_guard_thread(void *unused) {
    (void)unused;
    totk_thread_start("totk-gpufence", NULL);
    nvn_dirty_lag_thread_role("gpu-completion");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_gpu_head == g_qtail) pthread_cond_wait(&g_cv_work, &g_mu);
        slot_t *s = g_queue_[g_gpu_head % RING];
        pthread_mutex_unlock(&g_mu);
        totk_live_slack_tick();   /* a live TOTK_TIMER_SLACK_NS: this thread applies it itself */
        gpu_wait_slot(s);
        if (s->ft_frame && totk_ft_on()) s->ft_gpu_done = totk_ft_now();
        nvn_dirty_lag_complete(s->value);
        pthread_mutex_lock(&g_mu);
        g_gpu_head++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void *completion_thread(void *arg) {
    (void)arg;
    totk_thread_start("totk-post", NULL);
    nvn_dirty_lag_thread_role("completion-post");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        /* The GPU-only cursor's existence is immutable, even after capture is disabled at runtime.
           It must pass a slot before posts release that slot for reuse. */
        if (g_lag_wait == 1) { while (g_qhead == g_qtail || g_gpu_head == g_qhead) pthread_cond_wait(&g_cv, &g_mu); }
        else while (g_qhead == g_qtail) pthread_cond_wait(&g_cv_work, &g_mu);
        slot_t *s = g_queue_[g_qhead % RING];
        while (g_ub_publication == s->value) pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        totk_live_slack_tick();
        if (g_lag_wait != 1) {
            gpu_wait_slot(s);
            if (s->ft_frame && totk_ft_on()) s->ft_gpu_done = totk_ft_now();
        }
        uint64_t t1 = g_hitch_on ? host_now_ns() : 0;   /* observed GPU completion handoff, not the driver's actual signal time */
        uint64_t gpu_ns = 0;
        int framegen=vk_fg_enabled() && vk_fg_interpolating(), framegen_valid=0;
        double framegen_ms=0;
        if (g_ts_on > 0 || g_gpu_time_hardware) ts_calibrate();
        uint64_t raw_start_ns = 0, raw_end_ns = 0; int timing_valid = 0;
        if (s->ts_on) {
            uint64_t t[2];
            if (!vkGetQueryPoolResults(V.dev, s->ts, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT)) {
                if (t[1] > t[0]) {
                    gpu_ns = (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod);
                    atomic_fetch_add(&g_perf_gpu_ns, gpu_ns);
                    vk_pipeline_keys_pressure(host_now_ns(), gpu_ns > 25000000);
                }
                /* Reuse this slot's existing bracket. Pre uploads execute before
                   all main primaries; include them in headroom, not host waits. */
                uint64_t pre=t[0];
                int pre_valid=1;
                if (s->pre_open && (g_gpu_time_hardware || framegen))
                    pre_valid=vkGetQueryPoolResults(V.dev,s->ts,2,1,sizeof pre,&pre,sizeof pre,VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
                if (framegen && pre_valid && V.timestamp_bits && V.props.limits.timestampPeriod > 0) {
                    uint64_t mask=V.timestamp_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << V.timestamp_bits)-1;
                    framegen_ms=(double)((t[1]-pre) & mask)*V.props.limits.timestampPeriod/1e6;
                    framegen_valid=1;
                }
                if (g_gpu_time_hardware && g_ts_clock_valid) {
                    raw_start_ns = guest_clock_ticks_to_ns(vk_timestamp_ticks(t[0], g_ts_gpu_sample, g_ts_host_sample, V.props.limits.timestampPeriod, V.timestamp_bits));
                    raw_end_ns = guest_clock_ticks_to_ns(vk_timestamp_ticks(t[1], g_ts_gpu_sample, g_ts_host_sample, V.props.limits.timestampPeriod, V.timestamp_bits));
                    timing_valid = raw_end_ns >= raw_start_ns;
                    if (g_gpu_time_hardware && s->pre_open) {
                        if (pre_valid)
                            raw_start_ns = guest_clock_ticks_to_ns(vk_timestamp_ticks(pre, g_ts_gpu_sample, g_ts_host_sample, V.props.limits.timestampPeriod, V.timestamp_bits));
                        else timing_valid = 0;
                    }
                    timing_valid &= raw_end_ns >= raw_start_ns;
                }
            }
        }
        if (framegen) vk_fg_gpu_sample(s->value,framegen_ms,framegen_valid);
        totk_perf_gpu(s->guest_frame, gpu_ns);   /* TOTK_ADPF_GPU: per guest frame, in completion order */
        /* The guard cursor publishes its fence observation under g_mu before we can reach this slot.
           Report before CPU post ops: their completion is not GPU finish. gpu_ns covers main buffers, not pre. */
        if (s->ft_frame && totk_ft_on()) totk_ft_slot_gpu(s->ft_frame, s->value, s->ft_gpu_done, gpu_ns);
        occl_complete(s);
        if (vk_gpuprof_on) vk_gpuprof_slot_complete(&s->gpuprof);
        if (!timing_valid) gpu_time_unavailable(s->guest_frame, "slot GPU bracket unavailable");
        vk_gpu_time_slot timing = {.guest_ns = s->guest_submit_ns, .raw_start_ns = raw_start_ns, .raw_end_ns = raw_end_ns};
        if (g_gpu_time_hardware && timing_valid && !g_gpu_time_uncertain)
            timing = vk_gpu_time_begin(&g_gpu_time_queue, s->guest_submit_ns, raw_start_ns, raw_end_ns);
        for (uint32_t i = 0; i < s->tsq_count; i++) {
            unsigned q = (s->tsq_first + i) % TSQ;
            g_tsq_time[q].origin_ticks = s->guest_origin_ticks;
            if (g_gpu_time_hardware) {
                g_tsq_time[q].slot = timing; g_tsq_time[q].frame = s->guest_frame;
                g_tsq_time[q].valid = timing_valid && !g_gpu_time_uncertain;
                g_tsq_time[q].clock_gpu = g_ts_gpu_sample; g_tsq_time[q].clock_guest = g_ts_host_sample;
            }
        }
        if (s->guest_frame && s->queue_ns >= s->guest_submit_ns) {
            g_gpu_time_stats[GT_SLOTS]++;
            g_gpu_time_stats[GT_SUBMIT_DELAY] += s->queue_ns - s->guest_submit_ns;
        }
        apply_posts(s, &timing, timing_valid);
        int track_ready = runtime_android_predict_active() || runtime_android_fps_auto_active() ||
                          vk_fg_enabled() || totk_ft_on(); /* never enter frontend under g_mu */
        pthread_mutex_lock(&g_mu);
#ifdef TOTK_VK_TESTS
        if (g_test_completion_hold==s->value) {
            atomic_store_explicit(&g_test_completion_held,s->value,memory_order_release);
            pthread_cond_broadcast(&g_cv);
            while (g_test_completion_hold==s->value) pthread_cond_wait(&g_cv,&g_mu);
            atomic_store_explicit(&g_test_completion_held,0,memory_order_release);
        }
#endif
        if (g_gpu_time_hardware) for (int i = 0; i < s->npost; i++) {
            const post_op *p = &s->post[i];
            if (p->kind == 2 && p->type == NVN_COUNTER_TYPE_TIMESTAMP)
                vk_gpu_time_sample_publish(&g_gpu_time_samples, p->addr, p->report_ticks, p->report_origin);
        }
        memcpy(g_gpu_time_published, g_gpu_time_stats, sizeof g_gpu_time_published);
        /* Posts and the UBO-publication gate have passed. Keep slot ownership
           under g_mu until this pre-done-store anchor and g_done are published;
           neither timestamp is a manual NVN signal or a hardware signal time. */
        if (s->ft_frame && totk_ft_on()) totk_ff_ticket_publish(s->value, totk_ft_now(), (unsigned)s->npost);
        s->npost = 0; s->busy = 0; g_qhead++;
        if (!s->submit_busy && !replay_until((unsigned)(s-g_ring))) vk_record_slot_retire((unsigned)(s-g_ring));
        atomic_store(&g_done, s->value);
        replay_retire(s->value);
        if (track_ready) {
            unsigned index = (unsigned)(s->value % PRESENT_READY_HISTORY);
            g_present_ready[index].ticket = s->value;
            g_present_ready[index].ready_ns = host_now_ns();
        }
        if (g_hitch_on) { g_done_at = host_now_ns(); HK_ADD(comp_ns, g_done_at - t1); }
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void slot_open(slot_t *s);
/* Diagnostic facts are recorded only at the original short-circuit predicates.
 * -1 means unqueried; no allocator state is probed just for logging. */
typedef struct {
    int enabled,open_primary,optional_next,mutex_result,replay_pending,replay_pins;
    int busy,submit_busy,replay,scanned,free_slots;
    unsigned busy_checked,busy_skips,submit_busy_checked,submit_busy_skips,replay_checked,replay_skips;
} optional_begin_trace;
static void submit_optional_trace(const fg_dispatch_trace *dispatch,
        const optional_begin_trace *t,const char *reason) {
    if (!t) return;
    LOGV("[adaptive-dispatch-optional] source=%llu charge=%llu deadline-ns=%llu stage=optional-begin reason=%s open-primary=%d optional-next=%d mutex-result=%d replay-pending=%d replay-pins=%d pin-cap=%u scanned=%d free-slots=%d busy-checked=%u busy-skips=%u submit-busy-checked=%u submit-busy-skips=%u replay-checked=%u replay-skips=%u unqueried=-1 clock=host-monotonic",
        (unsigned long long)dispatch->source,(unsigned long long)dispatch->charge,
        (unsigned long long)dispatch->deadline,reason,t->open_primary,t->optional_next,
        t->mutex_result,t->replay_pending,t->replay_pins,(unsigned)(RING-1),
        t->scanned,t->free_slots,t->busy_checked,t->busy_skips,
        t->submit_busy_checked,t->submit_busy_skips,t->replay_checked,t->replay_skips);
}
/* Claim BOTH split and consumer before optional GL work. Busy or leased slots
 * are skipped, never waited on; tickets still come only from slot_submit. */
static int submit_optional_begin(int immutable) {
    fg_dispatch_trace *dispatch=vk_fg_dispatch_trace_current;
    optional_begin_trace facts,*t=NULL;
    if (dispatch && dispatch->enabled) {
        facts=(optional_begin_trace){.enabled=1,.open_primary=-1,.optional_next=-1,
            .mutex_result=-1,.replay_pending=-1,.replay_pins=-1,.scanned=-1,.free_slots=-1};
        t=&facts;
    }
    if (FG_DISPATCH_FACT(t,open_primary,g_cur!=NULL) ||
        FG_DISPATCH_FACT(t,optional_next,g_optional_next!=NULL)) {
        submit_optional_trace(dispatch,t,t && t->open_primary ? "open-primary" : "optional-next");
        return 0;
    }
    g_optional_immutable=0;
    if (FG_DISPATCH_FACT(t,mutex_result,pthread_mutex_trylock(&g_mu))) {
        submit_optional_trace(dispatch,t,"mutex-busy");
        return 0;
    }
    replay_retire(atomic_load(&g_done));
    slot_t *first=NULL,*second=NULL;
    if (!FG_DISPATCH_FACT(t,replay_pending,g_replay_pending!=NULL) &&
        FG_DISPATCH_FACT(t,replay_pins,g_replay_pins)<RING-1) {
        if (t) t->scanned=t->free_slots=0;
        for (unsigned n=0;n<RING;n++) {
            unsigned i=((unsigned)g_ring_next+n)%RING;
            slot_t *s=&g_ring[i];
            if (t) { t->scanned++; t->busy=t->submit_busy=t->replay=-1; }
            int skip=FG_DISPATCH_FACT(t,busy,s->busy) ||
                FG_DISPATCH_FACT(t,submit_busy,s->submit_busy) ||
                FG_DISPATCH_FACT(t,replay,replay_until(i)!=0);
            if (t) {
                t->busy_checked+=t->busy>=0; t->busy_skips+=t->busy>0;
                t->submit_busy_checked+=t->submit_busy>=0; t->submit_busy_skips+=t->submit_busy>0;
                t->replay_checked+=t->replay>=0; t->replay_skips+=t->replay>0;
            }
            if (skip) continue;
            if (t) t->free_slots++;
            if (!first) first=s;
            else { second=s; break; }
        }
    }
    if (second) {
        first->busy=second->busy=1;
        g_optional_next=second;
        g_ring_next=((unsigned)(second-g_ring)+1)%RING;
    }
    pthread_mutex_unlock(&g_mu);
    submit_optional_trace(dispatch,t,second ? "accepted" :
        t && t->replay_pending==1 ? "replay-pending" :
        t && t->replay_pins>=RING-1 ? "pin-cap" : "free-slots");
    if (!second) return 0;
    slot_open(first);
    g_optional_immutable=immutable;
    return 1;
}
int vk_submit_optional_begin(void) { return submit_optional_begin(0); }
int vk_submit_optional_begin_immutable(void) { return submit_optional_begin(1); }
void vk_submit_optional_cancel(void) {
    /* Only before signal/adoption: neither claimed slot has a submitted use. */
    if (!g_cur || !g_optional_next || g_replay_pending || g_cur->adopted)
        DIE("invalid optional pre-submit cancellation");
    if (vk_one_primary_on) vk_sections_cancel();
    else VKCHECK(vkEndCommandBuffer(g_cur->cb));
    if (vk_gpuprof_on) vk_gpuprof_slot_cancel(&g_cur->gpuprof);
    pthread_mutex_lock(&g_mu);
    g_cur->busy=g_optional_next->busy=0;
    g_cur=g_optional_next=NULL;
    g_optional_immutable=0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
void vk_submit_adopt_primary(VkCommandBuffer closed) {
    if (!g_cur || !closed || g_cur->adopted) DIE("invalid prepared primary adoption");
    g_cur->adopted=closed;
    /* The closed primary precedes the ordinary main CB's timestamp start.
     * Reuse the existing pre-primary timestamp envelope so framegen's GPU
     * sample includes ALL adopted work, not just the empty continuation. */
    (void)vk_pre_cb();
    if (vk_gpuprof_on) {
        /* Optional adoption starts with an otherwise empty ordinary primary.
         * Queue order is pre, adopted, main: bracket the immutable primary
         * without adding queries to a reusable, pre-recorded command stream. */
        vk_gpuprof_scope_begin(g_cur->pre, "framegen");
        vk_gpuprof_scope_end(g_cur->cb);
    }
}
void vk_submit_optional_end(void) {
    /* Failure before the split leaves the spare slot unused. After the split,
     * sealing the consumer is mandatory even for a discarded picture. */
    if (!g_optional_next) { g_optional_immutable=0; return; }
    pthread_mutex_lock(&g_mu);
    g_optional_next->busy=0; g_optional_next=NULL;
    g_optional_immutable=0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
static void slot_begin_inner(void) {
    if (g_optional_next) {
        slot_t *s=g_optional_next; g_optional_next=NULL;
        slot_open(s); return;
    }
    slot_t *s = &g_ring[g_ring_next]; g_ring_next = (g_ring_next + 1) % RING;
    uint64_t w0 = host_now_ns(); int blocked;
    VK_REC_DO(VK_REC_O_GPU_WAIT,
        pthread_mutex_lock(&g_mu);
        if (g_replay_pins) {
            unsigned next=(unsigned)(s-g_ring);
            /* Admission always leaves one unpinned slot. Never wait on an
             * open lease: only its walking owner can seal the covering work. */
            while (replay_until(next)) {
#ifdef TOTK_VK_TESTS
                g_test_replay_skips++;
#endif
                next=(next+1)%RING;
            }
            s=&g_ring[next]; g_ring_next=(next+1)%RING;
        }
        blocked=s->busy || s->submit_busy;
        while (s->busy || s->submit_busy) pthread_cond_wait(&g_cv,&g_mu);
        s->busy=1; pthread_mutex_unlock(&g_mu));
    { uint64_t d = host_now_ns() - w0; g_perf_wait_ns += d; g_cum[1] += d;
      if (blocked && T_ft_walk_frame && totk_ft_on()) totk_ft_walk_wait(T_ft_walk_frame, d); }
    slot_open(s);
}
/* Primary-slot descriptors (immediate graphics, compute, post, presentation, frame generation) are rewound here,
   at the same point the pools used to be reset: busy and submit_busy have retired, replay pins have released the
   slot, or a cancelled optional pre-submit never reached the GPU. Every caller writes the set it gets.
   A live TOTK_VK_DESC_STOCK (vk_desc_stock_want) switches the slot here too; switched off, the slot frees its stock and
   destroys its pools (nothing of the retired generation is in use), and grows pools again as the off policy needs.
   With TOTK_VK_DESC_STOCK_MAX_SETS the stock is capped (vk_record.h): a reset for the ceiling also destroys the pools
   past vk_desc_stock_pool_limit, at this same point. */
static void slot_descriptors_trim(slot_t *s, int keep) {
    if (keep >= s->ndpool) return;
    for (int i = keep; i < s->ndpool; i++) vkDestroyDescriptorPool(V.dev, s->dpools[i], NULL);
    vk_record_primary_pools(-(int64_t)(s->ndpool - keep));
    s->ndpool = keep;
}
#ifdef TOTK_VK_TESTS
/* Both descriptor owners count, right before they reset or destroy a slot's pools, whether every submission that may
   reference those sets has retired. The primary slot checks at its open: the previous incarnation's ticket is done,
   its submit owner returned and no replay pin holds it; that ticket is kept for the recorder contexts of the slot,
   which recycle at their first job of the new incarnation, when the slot may already hold its next (unretired)
   ticket: theirs is the kept one. */
static _Atomic uint64_t g_test_pool_checks, g_test_pool_violations;
static uint64_t g_test_pool_retired[RING]; /* g_mu */
static void test_descriptor_pools_open(unsigned slot) {
    pthread_mutex_lock(&g_mu);
    const slot_t *s = &g_ring[slot];
    int idle = s->value <= atomic_load(&g_done) && !s->submit_busy && !replay_until(slot);
    g_test_pool_retired[slot] = s->value;
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_test_pool_checks, 1);
    if (!idle) atomic_fetch_add(&g_test_pool_violations, 1);
}
void vk_test_descriptor_pools_check(unsigned slot) {
    pthread_mutex_lock(&g_mu);
    int idle = g_test_pool_retired[slot] <= atomic_load(&g_done);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_test_pool_checks, 1);
    if (!idle) atomic_fetch_add(&g_test_pool_violations, 1);
}
void vk_test_descriptor_pools_stats(uint64_t *checks, uint64_t *violations) {
    *checks = atomic_load(&g_test_pool_checks); *violations = atomic_load(&g_test_pool_violations);
}
#endif
static void slot_descriptors_recycle(slot_t *s) {
#ifdef TOTK_VK_TESTS
    test_descriptor_pools_open((unsigned)(s - g_ring));
#endif
    int want = vk_desc_stock_want();
    if (s->dstock_on && !want) {
        vk_desc_stock_free(&s->dstock);
        vk_record_descriptor_stock_event(VK_DESC_STOCK_OFF, (uint64_t)s->ndpool);
        slot_descriptors_trim(s, 0);
        s->curdpool = s->curdsets = 0; s->dstock_on = 0;
        return;
    }
    uint32_t ceiling = want ? vk_desc_stock_ceiling() : 0, limit = vk_desc_stock_pool_limit(ceiling, VK_DESC_PRIMARY_POOL_SETS);
    int keep = s->ndpool, reason = 0;
    if (s->dstock_on) {
        reason = vk_desc_stock_retire(s->dstock, ceiling, (uint32_t)s->ndpool, VK_DESC_PRIMARY_POOL_SETS);
        if (!reason) return;
    } else if (want && (uint32_t)s->ndpool > limit) reason = VK_DESC_STOCK_POOL_LIMIT;   /* switched on live over the limit */
    if (reason && reason != VK_DESC_STOCK_PRESSURE && (uint32_t)keep > limit) keep = (int)limit;
    int dirty = s->dstock_on ? (s->ndpool ? s->curdpool + 1 : 0) : s->ndpool;   /* a stock rebuild: pools used since the last */
    if (dirty > keep) dirty = keep;
    for (int i = 0; i < dirty; i++) VKCHECK(vkResetDescriptorPool(V.dev, s->dpools[i], 0));
    vk_record_descriptor_primary(0, 0, (uint64_t)dirty);
    if (reason) vk_record_descriptor_stock_event((unsigned)reason, (uint64_t)(s->ndpool - keep));
    slot_descriptors_trim(s, keep);
    s->curdpool = 0; s->curdsets = 0;
    s->dstock_on = want;   /* switched on: the stock starts empty on reset pools */
}
static void slot_open(slot_t *s) {
    if (ordered_fallback) atomic_fetch_add_explicit(&ordered_counts[5], 1, memory_order_relaxed);
    VK_REC_DO(VK_REC_O_RESET,
    if (!s->pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | (vk_record_enabled() ? VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT : 0), .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &s->pool));
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &cai, cbs)); s->cb = cbs[0]; s->pre = cbs[1];
    } else if (!vk_record_enabled()) VKCHECK(vkResetCommandPool(V.dev, s->pool, 0));
    if (!s->fence) { VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHECK(vkCreateFence(V.dev, &fci, NULL, &s->fence)); }
    else VKCHECK(vkResetFences(V.dev, 1, &s->fence));   /* the completion thread has waited for it (busy was cleared) */
    if (s->nprimary > s->recent_primary_peak) s->recent_primary_peak = s->nprimary;
    if (++s->primary_trim_uses == 32) {
        uint32_t keep = 256;
        while (keep < (uint64_t)s->recent_primary_peak * 2 && keep <= UINT32_MAX / 2) keep *= 2;
        if (s->capprimary >= (uint64_t)keep * 2) {
            uint32_t freed = s->capprimary - keep;
            vkFreeCommandBuffers(V.dev, s->pool, freed, s->primary + keep);
            vkTrimCommandPool(V.dev, s->pool, 0); s->capprimary = keep;
            g_primary_reclaimed += freed;
        }
        s->primary_trim_uses = s->recent_primary_peak = 0;
    }
    arena_reset(&s->ar, 4); arena_retain_cap(&s->ar);
    slot_descriptors_recycle(s);
    );
    s->oq_cur = NULL;
    s->tsq_first = s->tsq_count = 0;
    for (occl_page *p = s->oq; p; p = p->next) { vkResetQueryPool(V.dev, p->qp, 0, OQ); p->used = 0; }
    if (s->primary) s->cb = s->primary[0];
    s->nprimary = 1;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk_one_primary_on) vk_sections_reset((unsigned)(s - g_ring), s->cb);
    else {
        VK_REC_DO(VK_REC_O_PRIMARY, VKCHECK(vkBeginCommandBuffer(s->cb, &bi)));
        ordered_note_primary(0);
    }
    s->pre_open = 0; s->ts_on = 0;
    s->adopted=VK_NULL_HANDLE;
    if (ts_ok()) {
        if (!s->ts) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = g_gpu_time_hardware || (vk_fg_enabled() && vk_fg_interpolating()) ? 3 : 2}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &s->ts)); }
        vkCmdResetQueryPool(s->cb, s->ts, 0, 2);
        vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->ts, 0);
        s->ts_on = 1;
    }
    g_cur = s;
    vk_record_slot_reset((unsigned)(s - g_ring));
    vk_gpuprof_slot_begin(&s->gpuprof, s->cb); /* also reads the launch-time switch once */
}
static void slot_begin(void) { VK_REC_DO(VK_REC_O_SLOT, slot_begin_inner()); }
void vk_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
static void barrier_host(VkCommandBuffer cb) {   /* GPU writes become visible to the game */
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
VkCommandBuffer vk_cb(void) { if (!g_cur) slot_begin(); return g_cur->cb; }
static VkCommandBuffer record_next_primary_inner(void) {
    slot_t *s = g_cur; if (!s) DIE("deferred rendering has no submission slot");
    if (vk_one_primary_on) return s->cb;
    if (s->nprimary >= s->capprimary) {
        uint32_t old = s->capprimary, count = old ? old * 2 : 16;
        s->primary = VK_REALLOC(s->primary, (size_t)count * sizeof *s->primary);
        if (!old) { s->primary[0] = s->cb; old = 1; }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = count - old};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, s->primary + old)); s->capprimary = count;
    }
    s->cb = s->primary[s->nprimary++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(s->cb, &bi)); ordered_note_primary(0); return s->cb;
}
VkCommandBuffer vk_record_next_primary(void) { return VK_REC_VALUE(VK_REC_O_PRIMARY, record_next_primary_inner()); }
VkCommandBuffer vk_pre_cb(void) {
    if (!g_cur) slot_begin();
    if (!g_cur->pre_open) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(g_cur->pre, &bi)); g_cur->pre_open = 1;
        ordered_note_primary(1);
        if (vk_gpuprof_on) vk_gpuprof_slot_pre(g_cur->gpuprof, g_cur->pre);
        if ((g_gpu_time_hardware || (vk_fg_enabled() && vk_fg_interpolating())) && g_cur->ts_on) {
            vkCmdResetQueryPool(g_cur->pre, g_cur->ts, 2, 1);
            vkCmdWriteTimestamp2(g_cur->pre, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_cur->ts, 2);
        }
    }
    return g_cur->pre;
}
static void *arena_alloc(arena_t *a, size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off) {
    if (!align) align = 16;
    chunk_t *c = a->nchunk ? &a->chunks[a->nchunk - 1] : NULL;
    VkDeviceSize at = c ? (c->used + align - 1) / align * align : 0;
    if (!c || at + size > c->size) {
        /* a free chunk (all are reset when the arena is) is used before a new one is made */
        int found = -1;
        for (int i = 0; i < a->nchunk - 1 && found < 0; i++) if (!a->chunks[i].used && a->chunks[i].size >= size) found = i;
        if (found >= 0) { chunk_t tmp = a->chunks[found]; a->chunks[found] = a->chunks[a->nchunk - 1]; a->chunks[a->nchunk - 1] = tmp; c = &a->chunks[a->nchunk - 1]; at = 0; }
        else {
            uint64_t compact_start = a->reserve ? host_now_ns() : 0;
            if (a->nchunk == a->capchunk) { a->capchunk = a->capchunk ? 2 * a->capchunk : 8; a->chunks = VK_REALLOC(a->chunks, (size_t)a->capchunk * sizeof *a->chunks); }
            c = &a->chunks[a->nchunk++]; memset(c, 0, sizeof *c);
            VkDeviceSize min = a->min ? a->min : (8u << 20);
            if (a->reserve > min) min = a->reserve;
            a->reserve = 0;
            c->size = size > min ? (size + 0xffff) & ~(VkDeviceSize)0xffff : min;
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c->size, .usage = POOL_USAGE, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &c->buf));
            HK_ADD(chunks, 1);
            VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buf, &req);
            int t = pick_host_type(req.memoryTypeBits);
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
            VkResult result = vk_memory_allocate(&mai, &c->mem, a->min ? VK_MEMORY_SNAPSHOT : VK_MEMORY_SLOT);
            if (result) DIE("arena vkAllocateMemory failed: %d; buffer %llu, allocation %llu bytes, memory type %d, %s arena",
                result, (unsigned long long)c->size, (unsigned long long)req.size, t, a->min ? "snapshot" : "slot");
            VKCHECK(vkBindBufferMemory(V.dev, c->buf, c->mem, 0));
            result = vkMapMemory(V.dev, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->ptr);
            if (result) DIE("arena vkMapMemory failed: %d; allocation %llu bytes, memory type %d, %s arena",
                result, (unsigned long long)req.size, t, a->min ? "snapshot" : "slot");
            at = 0;
            if (compact_start) arena_compact_ns[a->min != 0] += host_now_ns() - compact_start;
        }
    }
    c->used = at + size;
    *buf = c->buf; *off = at;
    return c->ptr + at;
}
void *vk_transient(size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_cur) slot_begin();
    return arena_alloc(&g_cur->ar, size, align, buf, off);
}
#define DPOOL_SETS VK_DESC_PRIMARY_POOL_SETS
/* Allocates count (1..VK_DESC_STOCK_BATCH) sets from slot s, fewer near the end of a pool; never fewer than one. */
static uint32_t slot_alloc_sets(void *owner, VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *out) {
    slot_t *s = owner;
    VkDescriptorSetLayout layouts[VK_DESC_STOCK_BATCH];
    for (uint32_t i = 0; i < count; i++) layouts[i] = layout;
    /* TOTK (2026-10-22): sets came only from the slot's last pool, so a submission needing more than one pool made a new
       pool every time and the reset ones before it were never used again: one more pool per frame in the world (~4,400
       sets a frame), each with its driver memory, until Android ran out (~45 MB/s). Now the slot's pools are used in
       turn and a new one is made only past the last. */
    int fresh = 0;
    for (;;) {
        if (s->curdpool < s->ndpool && s->curdsets >= DPOOL_SETS) {   /* counted here too: some drivers (lavapipe) never report a full pool */
            if (s->curdpool + 1 < s->ndpool) { s->curdpool++; s->curdsets = 0; continue; }
        } else if (s->curdpool < s->ndpool) {
            if (count > (uint32_t)(DPOOL_SETS - s->curdsets)) count = (uint32_t)(DPOOL_SETS - s->curdsets);
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dpools[s->curdpool], .descriptorSetCount = count, .pSetLayouts = layouts};
            VkResult r = vkAllocateDescriptorSets(V.dev, &ai, out);
            vk_record_descriptor_primary(r ? 0 : count, 1, 0);
            if (!r) { s->curdsets += (int)count; totk_ft_count(FT_COUNT_DESC_CREATES, count); return count; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) DIE("vkAllocateDescriptorSets: %d", r);
            if (count > 1) { count /= 2; continue; }   /* large layouts may fit fewer than a whole batch */
            if (s->curdpool + 1 < s->ndpool) { s->curdpool++; s->curdsets = 0; continue; }
        }
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = DPOOL_SETS, .poolSizeCount = VK_DESC_POOL_TYPES, .pPoolSizes = vk_desc_pool_sizes};
        if (s->ndpool == s->capdpool) { s->capdpool = s->capdpool ? 2 * s->capdpool : 4; s->dpools = VK_REALLOC(s->dpools, (size_t)s->capdpool * sizeof *s->dpools); }
        if (fresh) break;   /* a set that does not fit an empty pool */
        VKCHECK(vkCreateDescriptorPool(V.dev, &pci, NULL, &s->dpools[s->ndpool++]));
        HK_ADD(dpools, 1); vk_record_primary_pools(1);
        s->curdpool = s->ndpool - 1; s->curdsets = 0; fresh = 1;
    }
    DIE("descriptor set allocation failed in a fresh pool");
}
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    if (!g_cur) slot_begin();
    slot_t *s = g_cur;
    if (s->dstock_on) return vk_desc_stock_take(&s->dstock, layout, slot_alloc_sets, s);
    VkDescriptorSet set; (void)slot_alloc_sets(s, layout, 1, &set); return set;
}
/* Submit what is recorded (pre, then main); with posts, the current submission's post ops ride along. */
static void ub_seal(uint64_t fence); static void ub_flush(void); static void gw_seal(uint64_t fence); static void gw_prune(void);
static int ub_private_on(void);
static void ub_publish(void);
static int lag_guard_submit(unsigned slot, uint64_t fence);
static void lag_guard_seal(unsigned slot, uint64_t fence);
static int lag_guard_submit_native(unsigned slot, uint64_t fence, int background);
static void wait_value(uint64_t fence);
#ifdef TOTK_VK_TESTS
static _Atomic(const char *) g_test_heap_failure;
void vk_test_fail_heap(const char *name) { atomic_store(&g_test_heap_failure, name); }
int vk_test_heap_failure(const char *name) {
    const char *fail = atomic_load(&g_test_heap_failure);
    return fail && !strcmp(fail, name);
}
static _Atomic uint64_t g_test_lag_fallbacks, g_test_wait_value;
static uint64_t g_test_submit_hold; /* g_mu */
static _Atomic uint64_t g_test_submit_held;
#endif
static int submit_async_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_REC_ASYNC_SUBMIT");
        on = vk_record_enabled() && (!e || atoi(e) != 0);
        if (on) LOGV("recorder slots: ordered asynchronous assembly/QueueSubmit (TOTK_VK_REC_ASYNC_SUBMIT=0 disables)");
    }
    /* Lavapipe producer exclusions still consume mutable walking logs; keep that diagnostic path synchronous. */
    if (on && lag_wait_on() && !vk_record_lag_native()) {
        static int logged;
        if (!logged) { logged = 1; LOGV("recorder slots: software-GPU lag producer exclusions require synchronous submission"); }
        return 0;
    }
    return on;
}
/* Exactly one queue caller per mode: the walking owner in inline mode, this FIFO owner in async mode. */
static void slot_queue(slot_t *s, int background) {
    if (vk_one_primary_on) vk_sections_finish((unsigned)(s - g_ring));
    VkCommandBufferSubmitInfo cbi[2] = {{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->pre}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb}};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = s->pre_open ? 2 : 1, .pCommandBufferInfos = s->pre_open ? cbi : cbi + 1};
    VkSemaphoreSubmitInfo signal, wait;
    int external=atomic_load_explicit(&g_external_enabled,memory_order_acquire);
    if (external) {
        signal=(VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore=g_external[s-g_ring].signal,.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        wait=(VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore=g_external[s-g_ring].wait,.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        if (signal.semaphore) { si.signalSemaphoreInfoCount=1; si.pSignalSemaphoreInfos=&signal; }
        if (wait.semaphore) { si.waitSemaphoreInfoCount=1; si.pWaitSemaphoreInfos=&wait; }
    }
    if (s->nprimary > 1 || s->adopted) {
        uint32_t count = s->nprimary + (s->pre_open != 0) + (s->adopted != VK_NULL_HANDLE);
        if (count > s->capsubmit) { s->capsubmit = count + 16; s->submit = VK_REALLOC(s->submit, (size_t)s->capsubmit * sizeof *s->submit); }
        uint32_t at = 0;
        if (s->pre_open) s->submit[at++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->pre};
        if (s->adopted) s->submit[at++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->adopted};
        for (uint32_t i = 0; i < s->nprimary; i++) s->submit[at++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->nprimary>1 ? s->primary[i] : s->cb};
        si.commandBufferInfoCount = count; si.pCommandBufferInfos = s->submit;
    }
    if (!background) pthread_mutex_lock(&g_mu);
    int ft_on = s->ft_frame && totk_ft_on();
    uint64_t start = ft_on ? totk_ft_now() : host_now_ns();
    s->queue_ns = runtime_now_ns();
    VkResult r = vkQueueSubmit2(V.queue, 1, &si, s->fence);
    uint64_t end = ft_on ? totk_ft_now() : host_now_ns();
    atomic_fetch_add_explicit(&g_queue_call_ns, end - start, memory_order_relaxed);
    if (ft_on) totk_ft_slot_queue(s->ft_frame, s->value, start, end);
    if (r) VK_DIE_RESULT(r, "vkQueueSubmit2: %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
    if (external) g_external[s-g_ring].signal=g_external[s-g_ring].wait=VK_NULL_HANDLE;
    nvn_dirty_lag_submitted(s->value);
    if (background) pthread_mutex_lock(&g_mu);
    if (s->publication) g_ub_publication = s->value;
    g_queue_[g_qtail++ % RING] = s; g_fence = s->value;
    if (!background) g_submit_done_value = s->value;
    pthread_cond_signal(&g_cv_work); pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    /* GPU completion may clear busy immediately; the async owner's submit_busy pin still prevents reuse. */
}
static void submit_failure(uint64_t value) {
#ifdef TOTK_VK_TESTS
    atomic_fetch_add_explicit(&g_test_lag_fallbacks, 1, memory_order_relaxed);
#endif
    pthread_mutex_lock(&g_mu); if (value > g_fallback_value) g_fallback_value = value; pthread_mutex_unlock(&g_mu);
}
static void *submit_worker(void *unused) {
    (void)unused; totk_thread_start("totk-submit", NULL);
    nvn_dirty_lag_thread_role("submit-assembly");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_sealed_head == g_sealed_tail) pthread_cond_wait(&g_sealed_cv, &g_mu);
        slot_t *s = g_sealed[g_sealed_head % RING];
        pthread_mutex_unlock(&g_mu);
        totk_live_slack_tick();
        unsigned slot = (unsigned)(s - g_ring); uint64_t value = s->value; int join_on_walk = s->join_on_walk;
        vk_record_assemble(slot, 1);
        int failed = s->native_lag ? lag_guard_submit_native(slot, value, 1) : 0;
        slot_queue(s, 1);
        if (failed) {
            submit_failure(value);
            /* A slot pin prevents fence reset while this owner waits. Do not wait for guest post ops here:
               an older post may fault on a later ticket this same FIFO owner still needs to queue. */
            if (!join_on_walk || nvn_dirty_lag_vma_pressure()) gpu_wait_slot(s);
            if (nvn_dirty_lag_vma_pressure()) nvn_dirty_lag_vma_drain();
        }
#ifdef TOTK_VK_TESTS
        pthread_mutex_lock(&g_mu);
        if (g_test_submit_hold == value) {
            atomic_store_explicit(&g_test_submit_held, value, memory_order_release);
            pthread_cond_broadcast(&g_cv);
            while (g_test_submit_hold == value) pthread_cond_wait(&g_cv, &g_mu);
            atomic_store_explicit(&g_test_submit_held, 0, memory_order_release);
        }
        pthread_mutex_unlock(&g_mu);
#endif
        pthread_mutex_lock(&g_mu);
        s->submit_busy = 0; g_submit_done_value = value; g_sealed_head++;
        if (!s->busy && !replay_until(slot)) vk_record_slot_retire(slot);
        replay_retire(atomic_load(&g_done));
        pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void submit_worker_start(void) {
    pthread_t thread; if (pthread_create(&thread, NULL, submit_worker, NULL)) DIE("slot submit thread");
    pthread_detach(thread);
    g_submit_started = 1;
}
static void slot_handoff(slot_t *s) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, submit_worker_start);
    pthread_mutex_lock(&g_mu);
    while (g_sealed_tail - g_sealed_head == RING) pthread_cond_wait(&g_cv, &g_mu);
    g_sealed[g_sealed_tail++ % RING] = s; pthread_cond_signal(&g_sealed_cv);
    pthread_mutex_unlock(&g_mu); g_handoff_count++;
}
static void submit_join(uint64_t value) {
    uint64_t start = host_now_ns(); int blocked;
    VK_REC_DO(VK_REC_O_WORKER_WAIT,
        pthread_mutex_lock(&g_mu);
        blocked = g_submit_done_value < value;
        while (g_submit_done_value < value) pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu));
    uint64_t ns = host_now_ns() - start; g_submit_join_ns += ns; vk_record_walk_wait(ns);
    /* host_now_ns is CLOCK_MONOTONIC too; reuse this disjoint wait scope, not an enclosing CPU-step wait. */
    if (blocked && T_ft_walk_frame && totk_ft_on()) totk_ft_walk_wait(T_ft_walk_frame, ns);
}
static void rec_service_report(uint64_t frame);
void vk_slot_stats(uint64_t frame) {
    if (frame % 300) return;
    vk_wb_stats();
    vk_texel_stats(frame);
    rec_service_report(frame);
    gpu_time_stats(frame);
    static uint64_t previous_subs; uint64_t subs = g_stat_sub - previous_subs; previous_subs = g_stat_sub;
    LOGV("slot submission/frame: all/game/handed %.2f/%.2f/%.2f, game slots/NVN submit %.2f; walking publication join %.3f ms, driver QueueSubmit %.3f ms (background when async)",
        g_slot_count / 300.0, g_game_slot_count / 300.0, g_handoff_count / 300.0, subs ? (double)g_game_slot_count / subs : 0,
        g_submit_join_ns / 3e8, atomic_exchange_explicit(&g_queue_call_ns, 0, memory_order_relaxed) / 3e8);
    g_slot_count = g_game_slot_count = g_handoff_count = g_submit_join_ns = 0;
    static uint64_t previous_arena_ns[2], previous_arena_compacts[2];
    LOGV("recorder walking arena compaction ns/frame slot/snapshot: %.0f/%.0f; compactions/frame %.3f/%.3f (retired high-water consolidation only)",
        (arena_compact_ns[0] - previous_arena_ns[0]) / 300.0, (arena_compact_ns[1] - previous_arena_ns[1]) / 300.0,
        (arena_compacts[0] - previous_arena_compacts[0]) / 300.0, (arena_compacts[1] - previous_arena_compacts[1]) / 300.0);
    memcpy(previous_arena_ns, arena_compact_ns, sizeof arena_compact_ns);
    memcpy(previous_arena_compacts, arena_compacts, sizeof arena_compacts);
}
#ifdef TOTK_VK_TESTS
int vk_record_test_async_submit(void) { return submit_async_on(); }
void vk_record_test_submit_status(uint64_t *reserved, uint64_t *queued) {
    pthread_mutex_lock(&g_mu); *reserved = g_reserved_fence; *queued = g_fence; pthread_mutex_unlock(&g_mu);
}
void vk_record_test_hold_submit(uint64_t ticket) {
    pthread_mutex_lock(&g_mu); g_test_submit_hold = ticket; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
uint64_t vk_record_test_submit_held(void) { return atomic_load_explicit(&g_test_submit_held, memory_order_acquire); }
void vk_record_test_hold_completion(uint64_t ticket) {
    pthread_mutex_lock(&g_mu); g_test_completion_hold=ticket;
    pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
uint64_t vk_record_test_completion_held(void) {
    return atomic_load_explicit(&g_test_completion_held,memory_order_acquire);
}
int vk_record_test_replay_pin(uint64_t source,uint64_t *until,uint64_t *skips) {
    int found=0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i=0;i<RING;i++) if (g_ring[i].value==source) {
        *until=replay_until(i); found=1; break;
    }
    *skips=g_test_replay_skips;
    pthread_mutex_unlock(&g_mu);
    return found;
}
int vk_record_test_replay_snapshots(uint64_t source,unsigned *snapshots) {
    int found=0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i=0;i<RING;i++) if (g_ring[i].value==source) {
        *snapshots=g_replay_owner[i].snapshots; found=1; break;
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}
void vk_record_test_guest_context(uint64_t v[8]) {
    v[0]=g_in_sub; v[1]=g_gpu_guest_ns; v[2]=g_gpu_origin_ticks; v[3]=g_gpu_guest_frame;
    v[4]=(uint64_t)g_ncarry; v[5]=g_game_slot_count; v[6]=g_cur!=NULL; v[7]=g_slot_count;
}
int vk_record_test_ticket_context(uint64_t ticket,uint64_t v[5]) {
    int found=0;
    pthread_mutex_lock(&g_mu);
    if (ticket) for (unsigned i=0;i<RING;i++) if (g_ring[i].value==ticket) {
        const slot_t *s=&g_ring[i];
        v[0]=s->guest_submit_ns; v[1]=s->guest_origin_ticks; v[2]=s->guest_frame;
        v[3]=(uint64_t)s->npost; v[4]=s->join_on_walk; found=1; break;
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}
#endif
static uint64_t slot_submit_inner(int with_posts, vk_pass_end_kind reason) {
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("submit");
    if (!g_cur) slot_begin();
    int publish = with_posts && ub_private_on();
    if (!ub_private_on()) ub_flush();
    vk_end_rendering_at(reason, 0);
    vk_wb_record();   /* settled copies share this ordinary slot, after the last pass and before sealing */
    if (vk_one_primary_on) {
        barrier_host(g_cur->cb);
        if (g_cur->ts_on) vkCmdWriteTimestamp2(g_cur->cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_cur->ts, 1);
        if (vk_gpuprof_on) vk_gpuprof_slot_end(g_cur->gpuprof, g_cur->cb);
        vk_sections_seal();
    }
    vk_record_slot_seal((unsigned)(g_cur - g_ring));
    totk_ft_count(FT_COUNT_CHUNKS, 1); /* one logical sealed slot, not each primary/secondary buffer */
    int asynchronous = submit_async_on();
    if (!asynchronous && g_submit_started) submit_join(g_reserved_fence);
    if (!asynchronous) vk_record_assemble((unsigned)(g_cur - g_ring), 0);
    VK_REC_DO(VK_REC_S_UNIFORM,
        ub_seal(g_reserved_fence + 1);
        gw_seal(g_reserved_fence + 1));
    slot_t *s = g_cur; g_cur = NULL;
    if (vk_gpuprof_on) vk_gpuprof_slot_seal(s->gpuprof);
    /* Bind a preceding external split to this actual resource consumer before
     * queue publication can complete either ticket. Its own GPU/CPU completion
     * remains independent; only physical resource retirement is extended. */
    if (atomic_load_explicit(&g_external_enabled,memory_order_acquire) && g_replay_pending && g_replay_pending!=s) {
        pthread_mutex_lock(&g_mu);
        g_external[g_replay_pending-g_ring].until=g_reserved_fence+1;
        g_replay_pending=NULL;
        pthread_mutex_unlock(&g_mu);
        if (!g_optional_immutable) snapshot_replay_pin(g_reserved_fence+1);
    }
    if (g_replay_releases) {
        pthread_mutex_lock(&g_mu);
        for (unsigned i=0;i<RING;i++) if (g_replay_owner[i].pending) {
            g_replay_owner[i].until=g_reserved_fence+1; g_replay_owner[i].pending=0;
        }
        g_replay_releases=0;
        pthread_mutex_unlock(&g_mu);
    }
    if (!vk_one_primary_on) {
        barrier_host(s->cb);
        if (s->ts_on) vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, s->ts, 1);
        if (vk_gpuprof_on) vk_gpuprof_slot_end(s->gpuprof, s->cb);
        VKCHECK(vkEndCommandBuffer(s->cb));
    }
    if (s->pre_open) { barrier_host(s->pre); VKCHECK(vkEndCommandBuffer(s->pre)); }
    if (with_posts && g_ncarry) {
        if (s->cap < g_ncarry) { s->cap = g_ncarry; s->post = VK_REALLOC(s->post, (size_t)s->cap * sizeof *s->post); }
        memcpy(s->post, g_carry, (size_t)g_ncarry * sizeof *g_carry); s->npost = g_ncarry; g_ncarry = 0;
    }
    unsigned slot = (unsigned)(s - g_ring); uint64_t value = g_reserved_fence + 1;
    s->native_lag = lag_wait_on() && vk_record_lag_native();
    if (s->native_lag) lag_guard_seal(slot, value);
    int untracked_wait = !asynchronous && lag_wait_on() ? lag_guard_submit(slot, value) : 0;
    int join_required = with_posts || !g_in_sub || reason == VK_PASS_END_CPU_WAIT;
    s->publication = publish; s->join_on_walk = join_required;
    pthread_mutex_lock(&g_mu); s->value = ++g_reserved_fence; s->submit_busy = asynchronous; pthread_mutex_unlock(&g_mu);
    if (vk_fg_lsfg_probe_requested())
        vk_fg_lsfg_sealed(s->nprimary>1 ? s->primary : &s->cb,s->nprimary>1 ? s->nprimary : 1,s->value);
    s->ft_frame = 0; s->ft_gpu_done = 0;
    /* Backend-only work contributes GPU duration, never a late host-time floor. */
    s->guest_submit_ns = g_in_sub || with_posts ? g_gpu_guest_ns : 0;
    s->guest_origin_ticks = g_in_sub || with_posts ? g_gpu_origin_ticks : 0;
    s->guest_frame = g_in_sub || with_posts ? g_gpu_guest_frame : 0;
    if (totk_ft_on()) {
        s->ft_frame = totk_ft_current();
        totk_ft_slot_seal(s->ft_frame, s->value, totk_ft_now());
    }
    g_slot_count++; g_game_slot_count += with_posts || g_in_sub;
    if (asynchronous) slot_handoff(s);
    else {
        VK_REC_DO(VK_REC_O_QUEUE, slot_queue(s, 0));
        if (untracked_wait) submit_failure(value);
        if (untracked_wait && nvn_dirty_lag_vma_pressure()) { gpu_wait_slot(s); nvn_dirty_lag_vma_drain(); }
    }
    atomic_fetch_add(&g_seal_serial, 1);
    if (asynchronous && join_required) submit_join(value);
    if (publish) {
        ub_publish();   /* QueueSubmit and worker join precede every host store and genuine-alias wait */
        pthread_mutex_lock(&g_mu);
        g_ub_publication = 0; pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    if (!asynchronous || with_posts || !g_in_sub || reason == VK_PASS_END_CPU_WAIT) {
        /* Final publication precedes host stores; every failed earlier slot also retains its post-op frontier. */
        pthread_mutex_lock(&g_mu); uint64_t failed = g_fallback_value; pthread_mutex_unlock(&g_mu);
        if (failed > g_fallback_consumed && failed <= value) { wait_value(failed); g_fallback_consumed = failed; }
    }
    nvn_dirty_lag_thread_role(previous_role);
    return value;
}
static uint64_t slot_submit(int with_posts, vk_pass_end_kind reason) { return VK_REC_VALUE(VK_REC_O_SUBMIT, slot_submit_inner(with_posts, reason)); }
uint64_t vk_replay_lease_begin(void) {
    if (!g_cur) return 0;
    unsigned slot=(unsigned)(g_cur-g_ring), snapshots=snapshot_replay_mask();
    pthread_mutex_lock(&g_mu);
    uint64_t token=0;
    if (!g_replay_owner[slot].until && (replay_until(slot) || g_replay_pins<RING-1)) {
        if (!replay_until(slot)) g_replay_pins++;
        token=++g_replay_token;
        if (!token) token=++g_replay_token;
        g_replay_owner[slot].token=token; g_replay_owner[slot].until=UINT64_MAX;
        g_replay_owner[slot].snapshots=snapshots;
    }
    pthread_mutex_unlock(&g_mu);
    return token;
}
uint64_t vk_replay_lease_begin_immutable(void) {
    if (!g_cur) return 0;
    unsigned slot=(unsigned)(g_cur-g_ring);
    pthread_mutex_lock(&g_mu);
    uint64_t token=0;
    if (!g_replay_owner[slot].until && (replay_until(slot) || g_replay_pins<RING-1)) {
        if (!replay_until(slot)) g_replay_pins++;
        token=++g_replay_token;
        if (!token) token=++g_replay_token;
        g_replay_owner[slot].token=token; g_replay_owner[slot].until=UINT64_MAX;
        g_replay_owner[slot].snapshots=0;
    }
    pthread_mutex_unlock(&g_mu);
    return token;
}
int vk_replay_lease_end(uint64_t token,uint64_t covering_ticket) {
    if (!token) return 0;
    pthread_mutex_lock(&g_mu);
    int ended=0;
    if (covering_ticket<=g_reserved_fence) for (unsigned i=0;i<RING;i++) {
        if (g_replay_owner[i].token!=token) continue;
        /* Latest reservation also covers canceled output and split-only
         * failures. Open work must first seal; frame-end's ordinary submission
         * resolves this pending release before it can retire on the GPU. */
        g_replay_owner[i].token=0;
        g_replay_owner[i].pending=g_cur || g_replay_pending;
        g_replay_releases+=g_replay_owner[i].pending;
        g_replay_owner[i].until=g_replay_owner[i].pending ? UINT64_MAX : g_reserved_fence;
        replay_retire(atomic_load(&g_done));
        pthread_cond_broadcast(&g_cv);
        ended=1; break;
    }
    pthread_mutex_unlock(&g_mu);
    return ended;
}
uint64_t vk_submit_signal(VkSemaphore signal) {
    if (!atomic_load_explicit(&g_external_enabled,memory_order_acquire) || !signal || g_replay_pending) return 0;
    (void)vk_cb();
    pthread_mutex_lock(&g_mu);
    if (!replay_until((unsigned)(g_cur-g_ring)) && g_replay_pins>=RING-1) { pthread_mutex_unlock(&g_mu); return 0; }
    if (!replay_until((unsigned)(g_cur-g_ring))) g_replay_pins++;
    g_external[g_cur-g_ring].until=UINT64_MAX;
    g_replay_pending=g_cur;
    pthread_mutex_unlock(&g_mu);
    if (!g_optional_immutable) snapshot_replay_pin(UINT64_MAX);
    if (g_external[g_cur-g_ring].signal) DIE("duplicate external signal in recorder slot");
    g_external[g_cur-g_ring].signal=signal;
    uint64_t value=slot_submit(0,VK_PASS_END_PRESENT);
    /* Only CPU queue publication: exporting SYNC_FD requires a pending signal.
     * This never waits for the GPU or for the GL consumer. */
    if (g_submit_started) submit_join(value);
    return value;
}
void vk_submit_wait(VkSemaphore wait) {
    if (!atomic_load_explicit(&g_external_enabled,memory_order_acquire)) DIE("external wait before engine admission");
    (void)vk_cb();
    if (g_external[g_cur-g_ring].wait) DIE("duplicate external wait in recorder slot");
    g_external[g_cur-g_ring].wait=wait;
}
static void wait_value_inner(uint64_t v) {
    uint64_t w0 = host_now_ns();
    pthread_mutex_lock(&g_mu);
    int blocked = atomic_load(&g_done) < v;
#ifdef TOTK_VK_TESTS
    if (v==UINT64_MAX) DIE("test invariant: open replay owner is not a submitted wait ticket");
    if (blocked) atomic_store_explicit(&g_test_wait_value, v, memory_order_release);
#endif
    while (atomic_load(&g_done) < v) pthread_cond_wait(&g_cv, &g_mu);
    if (blocked && g_done_at) HK_ADD(wake_ns, host_now_ns() - g_done_at);
    pthread_mutex_unlock(&g_mu);
    { uint64_t d = host_now_ns() - w0; g_perf_wait_ns += d; g_cum[1] += d;
      if (blocked && T_ft_walk_frame && totk_ft_on()) totk_ft_walk_wait(T_ft_walk_frame, d); }
}
static void wait_value(uint64_t v) { VK_REC_DO(VK_REC_O_GPU_WAIT, wait_value_inner(v)); }
void vk_wait_fence(uint64_t ticket) { wait_value(ticket); } /* exact ticket, never a latest-submission/global idle */
void vk_gpu_idle(void) { uint64_t v, t0 = g_hitch_on ? host_now_ns() : 0; pthread_mutex_lock(&g_mu); v = g_reserved_fence; pthread_mutex_unlock(&g_mu); wait_value(v); HK_ADD(idles, 1); HK_ADD(idle_ns, host_now_ns() - t0); }
static void cpu_step_inner(void) { HK_ADD(cpu_steps, 1); g_stat_cpu_step++; g_cum[3]++; totk_ft_count(FT_COUNT_CPU_STEPS, 1); if (vk_rec_profile_on && T_rec.active) g_rec.cpu_steps++; ub_flush(); if (g_cur) wait_value(slot_submit(0, VK_PASS_END_CPU_WAIT)); else vk_gpu_idle(); }
void vk_cpu_step(void) { VK_REC_DO(VK_REC_S, cpu_step_inner()); }

static pthread_mutex_t g_rec_mu;
static pthread_once_t g_rec_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned T_rec_depth;
static _Thread_local vk_pcache_span T_pcache_span;
static _Thread_local int T_rec_servicing;
static _Thread_local uint64_t T_rec_hold;
typedef struct rec_service_request {
    struct rec_service_request *next;
    vk_rec_service_fn function; void *arg;
    pthread_cond_t ready;
    uint64_t queued, started;
    int done, deferred, inline_call;
} rec_service_request;
/* This mutex only publishes stack requests/completions and optional statistics. Never hold it across callbacks,
   renderer work, GPU waits or logging. The recording owner alone touches renderer state in each callback. */
static pthread_mutex_t g_rec_service_mu = PTHREAD_MUTEX_INITIALIZER;
static rec_service_request *g_rec_service_head, *g_rec_service_tail;
_Atomic unsigned vk_rec_service_pending;
static int g_rec_service_stats_on;
enum { SERVICE_BINS = 512, SERVICE_BIN_NS = 100000 };
static struct {
    uint64_t calls, inline_calls, cooperative, deferred, delay_max, hold_max, hold_peak, samples;
    uint64_t histogram[SERVICE_BINS];
} g_rec_service_stats;
static void rec_mu_init(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rec_mu, &a); pthread_mutexattr_destroy(&a);
    const char *e = getenv("TOTK_VK_WB_SERVICE_STATS"); g_rec_service_stats_on = e && atoi(e) > 0;
}
static void rec_service_append(rec_service_request *r) { /* g_rec_service_mu */
    r->next = NULL;
    if (g_rec_service_tail) g_rec_service_tail->next = r; else g_rec_service_head = r;
    g_rec_service_tail = r;
    atomic_fetch_or_explicit(&vk_rec_service_pending, r->deferred ? 2u : 1u, memory_order_relaxed);
}
void vk_rec_service_run(int boundary) {
    if (!T_rec_depth || T_rec_servicing) return;
    if (!atomic_load_explicit(&vk_rec_service_pending, memory_order_relaxed)) return;
    T_rec_servicing = 1;
    uint64_t seal = vk_submit_serial();
    pthread_mutex_lock(&g_rec_service_mu);
    rec_service_request *list = g_rec_service_head;
    g_rec_service_head = g_rec_service_tail = NULL;
    atomic_store_explicit(&vk_rec_service_pending, 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_rec_service_mu);
    while (list) {
        rec_service_request *r = list; list = r->next;
        uint64_t started = host_now_ns();
        int done = (!r->deferred || boundary) && r->function(r->arg, boundary);
        pthread_mutex_lock(&g_rec_service_mu);
        if (!done) {
            if (!r->deferred) g_rec_service_stats.deferred++;
            r->deferred = 1; rec_service_append(r);
        } else {
            r->started = started; r->done = 1;
            g_rec_service_stats.inline_calls += r->inline_call;
            g_rec_service_stats.cooperative += !r->inline_call;
            if (g_rec_service_stats_on) {
                uint64_t delay = started - r->queued;
                unsigned bin = delay / SERVICE_BIN_NS;
                if (bin >= SERVICE_BINS) bin = SERVICE_BINS - 1;
                g_rec_service_stats.histogram[bin]++; g_rec_service_stats.samples++;
                if (delay > g_rec_service_stats.delay_max) g_rec_service_stats.delay_max = delay;
            }
            pthread_cond_signal(&r->ready);
        }
        pthread_mutex_unlock(&g_rec_service_mu);
    }
    T_rec_servicing = 0;
    /* Another ready readback may itself seal the deferred producer while this batch is being serviced.
       Drain those newly-ready requests now, not at the end of unrelated later walking work. */
    if (!boundary && seal != vk_submit_serial() &&
        (atomic_load_explicit(&vk_rec_service_pending, memory_order_relaxed) & 2u))
        vk_rec_service_run(1);
}
/* Standalone backend tests have no guest scheduler. App builds always link
 * threads.c; keep these optional only for backend-only executables. */
extern void runtime_host_wait_begin(runtime_host_wait *) __attribute__((weak));
extern void runtime_host_wait_acquired(runtime_host_wait *) __attribute__((weak));
extern void runtime_host_wait_end(runtime_host_wait *) __attribute__((weak));
static void rec_enter(void) {
    if (!T_rec_depth++ && g_rec_service_stats_on) T_rec_hold = host_now_ns();
    if (T_rec_depth == 1) vk_rec_service_run(1);
}
void vk_rec_lock(void) {
    if (!T_rec_depth) T_pcache_span = vk_pcache_span_begin();
    pthread_once(&g_rec_once, rec_mu_init); pthread_mutex_lock(&g_rec_mu);
    rec_enter();
}
void vk_rec_lock_texels(runtime_host_wait *wait) {
    if (!T_rec_depth) T_pcache_span = vk_pcache_span_begin();
    wait->active = 0;
    pthread_once(&g_rec_once, rec_mu_init);
    int rc = pthread_mutex_trylock(&g_rec_mu);
    if (rc) {
        if (rc != EBUSY) DIE("texel recording try-lock failed: %d", rc);
        if (runtime_host_wait_begin) runtime_host_wait_begin(wait);
        pthread_mutex_lock(&g_rec_mu);
        if (wait->active) runtime_host_wait_acquired(wait);
    }
    rec_enter();
}
void vk_rec_unlock_texels(runtime_host_wait *wait) {
    vk_rec_unlock();
    if (wait->active) runtime_host_wait_end(wait);
}
void vk_rec_unlock(void) {
    if (T_rec_depth > 1) { T_rec_depth--; pthread_mutex_unlock(&g_rec_mu); return; }
    for (;;) {
        vk_rec_service_run(1);
        pthread_mutex_lock(&g_rec_service_mu);
        if (g_rec_service_head) { pthread_mutex_unlock(&g_rec_service_mu); continue; }
        if (g_rec_service_stats_on) {
            uint64_t hold = host_now_ns() - T_rec_hold;
            if (hold > g_rec_service_stats.hold_max) g_rec_service_stats.hold_max = hold;
            if (hold > g_rec_service_stats.hold_peak) g_rec_service_stats.hold_peak = hold;
        }
        /* Serialize the final EMPTY check and actual unlock with enqueue+try-lock. A relaxed idle poll is
           only a hint: it must never be the authority that closes this last-owner/lost-wakeup race.
           This adds one short mutex scope per ownership release, never per idle opcode or draw. */
        T_rec_depth = 0; pthread_mutex_unlock(&g_rec_mu);
        pthread_mutex_unlock(&g_rec_service_mu);
        vk_pcache_span_end(T_pcache_span, "recording-scope");
        return;
    }
}
uint64_t vk_rec_service_call(vk_rec_service_fn function, void *arg) {
    pthread_once(&g_rec_once, rec_mu_init);
    if (T_rec_depth) {
        if (!function(arg, 1)) DIE("owner readback cannot defer itself");
        return 0;
    }
    rec_service_request request = {.function = function, .arg = arg, .ready = PTHREAD_COND_INITIALIZER,
        .queued = host_now_ns()};
    pthread_mutex_lock(&g_rec_service_mu);
    g_rec_service_stats.calls++; rec_service_append(&request);
    int owned = !pthread_mutex_trylock(&g_rec_mu); request.inline_call = owned;
    pthread_mutex_unlock(&g_rec_service_mu);
    if (owned) {
        T_pcache_span = vk_pcache_span_begin();
        T_rec_depth = 1; if (g_rec_service_stats_on) T_rec_hold = host_now_ns();
        vk_rec_service_run(1); vk_rec_unlock();
    }
    pthread_mutex_lock(&g_rec_service_mu);
    while (!request.done) pthread_cond_wait(&request.ready, &g_rec_service_mu);
    pthread_mutex_unlock(&g_rec_service_mu);
    pthread_cond_destroy(&request.ready);
    return request.started - request.queued;
}

/* Finalize never waits for GPU completion with registry or recording ownership.
   The recording lock closes the snapshot-to-destruction gap: it also seals any
   currently recorded work, and all asynchronous worker submissions have already
   reserved tickets before handing their immutable spans to those workers. */
static int pool_gpu_quiet(void) {
    pthread_mutex_lock(&g_mu);
    int quiet = atomic_load(&g_done) >= g_reserved_fence;
    pthread_mutex_unlock(&g_mu);
    return quiet;
}
static pthread_once_t g_pool_stats_once = PTHREAD_ONCE_INIT;
static int g_pool_stats;
static _Atomic uint64_t g_pool_finalizes, g_pool_wait_ns, g_pool_wait_max_ns, g_pool_released_ns;
static void pool_stats_init(void) {
    const char *s = getenv("TOTK_POOL_STATS"); g_pool_stats = s && !strcmp(s, "1");
}
static void pool_stats_report(void) {
    if (!g_pool_stats) return;
    LOGV("[pool-stats] finalize=%llu wait_ms=%.3f wait_max_ms=%.3f core_released_ms=%.3f (cumulative completed calls)",
        (unsigned long long)atomic_load(&g_pool_finalizes), atomic_load(&g_pool_wait_ns) / 1e6,
        atomic_load(&g_pool_wait_max_ns) / 1e6, atomic_load(&g_pool_released_ns) / 1e6);
}
#ifdef TOTK_VK_TESTS
static _Thread_local _Atomic int *T_pool_waited, *T_pool_retry;
static _Thread_local _Atomic uint64_t *T_pool_ticket;
void vk_pool_test_wait_ticket(_Atomic uint64_t *ticket) { T_pool_ticket = ticket; }
void vk_pool_test_pause_after_wait(_Atomic int *waited, _Atomic int *retry) {
    T_pool_waited = waited; T_pool_retry = retry;
}
void vk_pool_test_stats(uint64_t out[4]) {
    out[0] = atomic_load(&g_pool_finalizes); out[1] = atomic_load(&g_pool_wait_ns);
    out[2] = atomic_load(&g_pool_wait_max_ns); out[3] = atomic_load(&g_pool_released_ns);
}
#endif
static uint64_t pool_wait_ticket(uint64_t ticket) {
    uint64_t start = g_pool_stats ? host_now_ns() : 0;
    pthread_mutex_lock(&g_mu);
#ifdef TOTK_VK_TESTS
    atomic_store_explicit(&g_test_wait_value, ticket, memory_order_release);
    if (T_pool_ticket) atomic_store_explicit(T_pool_ticket, ticket, memory_order_release);
#endif
    while (atomic_load(&g_done) < ticket) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    uint64_t elapsed = g_pool_stats ? host_now_ns() - start : 0;
#ifdef TOTK_VK_TESTS
    if (T_pool_waited) {
        atomic_store(T_pool_waited, 1);
        while (!atomic_load(T_pool_retry)) sched_yield();
        T_pool_waited = T_pool_retry = NULL;
    }
#endif
    return elapsed;
}
void mtl_pool_finalize(const void *mem) {
    pthread_once(&g_pool_stats_once, pool_stats_init);
    runtime_host_wait wait = {0};
    uint64_t waited = 0;
    /* Dirty retirement can itself wait for an armed GPU reader. Release before
       entering it, preserving dirty-remove-before-unback ordering. */
    uint64_t initial = mtl_last_fence();
    int pending = atomic_load(&g_done) < initial;
    if (pending && runtime_host_wait_begin) runtime_host_wait_begin(&wait);
    if (pending) waited += pool_wait_ticket(initial);
    uint64_t dirty_start = g_pool_stats && pending ? host_now_ns() : 0;
    nvn_dirty_remove_range(mem);
    if (dirty_start) waited += host_now_ns() - dirty_start;
    for (;;) {
        /* Keep one token through retries: no core reacquisition under rec/pool.
           The first contended recording acquisition can itself release the core. */
        if (wait.active) vk_rec_lock(); else vk_rec_lock_texels(&wait);
        uint64_t ticket = vk_submit_now();
        if (pool_gpu_quiet()) {
            del_range(mem, 0);
            vk_rec_unlock();
            break;
        }
        vk_rec_unlock();
        if (!wait.active && runtime_host_wait_begin) runtime_host_wait_begin(&wait);
        waited += pool_wait_ticket(ticket);
        /* A different producer may have submitted after our snapshot. Reacquire
           recording and recheck, rather than destroying behind that producer. */
    }
    if (wait.active) runtime_host_wait_acquired(&wait);
    uint64_t released = g_pool_stats && wait.released_at_ns ? runtime_now_ns() - wait.released_at_ns : 0;
    if (wait.active) runtime_host_wait_end(&wait);
    if (g_pool_stats) {
        atomic_fetch_add(&g_pool_finalizes, 1);
        atomic_fetch_add(&g_pool_wait_ns, waited);
        atomic_fetch_add(&g_pool_released_ns, released);
        uint64_t maximum = atomic_load(&g_pool_wait_max_ns);
        while (maximum < waited && !atomic_compare_exchange_weak(&g_pool_wait_max_ns, &maximum, waited)) {}
        pool_stats_report();
    }
}
static uint64_t rec_service_percentile(unsigned percent) { /* g_rec_service_mu; upper bound, including overflow */
    uint64_t want = (g_rec_service_stats.samples * percent + 99) / 100, total = 0;
    if (!want) return 0;
    for (unsigned i = 0; i < SERVICE_BINS; i++) {
        total += g_rec_service_stats.histogram[i];
        if (total >= want) return i == SERVICE_BINS - 1 ? g_rec_service_stats.delay_max : (uint64_t)(i + 1) * SERVICE_BIN_NS;
    }
    return 0;
}
static void rec_service_report(uint64_t frame) {
    if (!g_rec_service_stats_on || frame % 300) return;
    pthread_mutex_lock(&g_rec_service_mu);
    uint64_t p50 = rec_service_percentile(50), p99 = rec_service_percentile(99);
    __typeof__(g_rec_service_stats) s = g_rec_service_stats;
    memset(&g_rec_service_stats, 0, sizeof g_rec_service_stats); g_rec_service_stats.hold_peak = s.hold_peak;
    pthread_mutex_unlock(&g_rec_service_mu);
    LOGV("write-back owner service: calls %llu inline/cooperative %llu/%llu deferred %llu; delay p50<=%.3f p99<=%.3f max %.3f ms; rec-lock hold max %.3f ms (peak %.3f); <=64-opcode chunks, same-producer defers to slot seal",
        (unsigned long long)s.calls, (unsigned long long)s.inline_calls, (unsigned long long)s.cooperative,
        (unsigned long long)s.deferred, p50 / 1e6, p99 / 1e6, s.delay_max / 1e6, s.hold_max / 1e6, s.hold_peak / 1e6);
}
#ifdef TOTK_VK_TESTS
void vk_rec_service_test_stats(uint64_t v[8]) {
    pthread_mutex_lock(&g_rec_service_mu);
    v[0] = g_rec_service_stats.calls; v[1] = g_rec_service_stats.inline_calls;
    v[2] = g_rec_service_stats.cooperative; v[3] = g_rec_service_stats.deferred;
    v[4] = g_rec_service_stats.delay_max; v[5] = g_rec_service_stats.hold_max;
    v[6] = rec_service_percentile(50); v[7] = rec_service_percentile(99);
    pthread_mutex_unlock(&g_rec_service_mu);
}
#endif
/* Chunked submission (vk_draw.c, at the start of a render pass): inside a game submission, what is recorded so far goes to
   the GPU, so it works while the rest of the frame is recorded. The game waits for its previous frame's GPU work before
   the next one, so with one Vulkan submission per frame the GPU idled during recording and the CPU during the GPU's work
   (frame = recording + GPU time). Post ops stay with the game submission's last part (mtl_sub_commit). */
static __thread const char *g_lag_guest_role;
#ifdef TOTK_VK_TESTS
uint64_t vk_record_test_lag_fallbacks(void) {
    return atomic_load_explicit(&g_test_lag_fallbacks, memory_order_relaxed);
}
uint64_t vk_record_test_wait_value(void) {
    return atomic_load_explicit(&g_test_wait_value, memory_order_acquire);
}
#endif
/* Recording-owner selector. Qualification must precede even its first getenv.
 * Scheduler default: absent means on; exact "0" restores the begin/commit-only
 * r6b path; an invalid spelling is off (logged once). */
static int g_fg_adaptive_chunk=-1;
static int fg_adaptive_chunk_on(void) {
    if (g_fg_adaptive_chunk<0) {
        const char *e=getenv("TOTK_FG_ADAPTIVE_CHUNK");
        int request=fga_default_on_request(e);
        if (request<0) LOGV("[adaptive-chunk] selector=invalid raw=\"%.32s\" enabled=0 (absent or exact \"1\" enables; \"0\" disables)",e);
        g_fg_adaptive_chunk=request==1;
    }
    return g_fg_adaptive_chunk;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_chunk_selector_reset(void) { g_fg_adaptive_chunk=-1; }
#endif
int vk_submit_chunk_possible(void) { return g_in_sub && g_cur; }
void vk_submit_chunk(void) {
    if (!g_in_sub || !g_cur) return;
    int chunk=__builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        fg_adaptive_chunk_on();
    int slot_trace=chunk && vk_fg_job_trace_enabled();
    uint64_t slot_submit_begin=slot_trace ? host_now_ns() : 0;
    uint64_t v=slot_submit(0,VK_PASS_END_SUBMISSION);
    uint64_t slot_submit_return=slot_trace ? host_now_ns() : 0;
    vk_rec_service_run(1);
    uint64_t slot_service_return=slot_trace ? host_now_ns() : 0;
    if (!g_cur && __builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        fg_adaptive_chunk_on()) {
        uint64_t enter=slot_trace ? host_now_ns() : 0;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=enter kind=chunk boundary_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=0 clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v);
        /* Optional children are backend-only. Leave immutable guest context and
         * pending posts with the interrupted walk; its original final seal owns
         * all posts, UBO publication and guest writeback. No allocator lock is held. */
        int in_sub=g_in_sub;
        g_in_sub=0;
        vk_fg_slot_boundary();
        g_in_sub=in_sub;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=exit kind=chunk enter_ns=%llu exit_ns=%llu real_ticket=%llu clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)host_now_ns(),(unsigned long long)v);
    } else if (slot_trace) {
        LOGV("[adaptive-slot-boundary] event=skip kind=chunk now_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=%u active=%u clock=CLOCK_MONOTONIC",
            (unsigned long long)host_now_ns(),(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v,g_cur!=NULL,
            atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed)!=0);
    }
}
static void snap_reset(void);
void mtl_sub_begin(void) {
    uint64_t guest_ns = T_gpu_time_pending.ns ? T_gpu_time_pending.ns : runtime_now_ns();
    uint64_t origin_ticks = T_gpu_time_pending.origin_ticks, frame = T_gpu_time_pending.frame;
    memset(&T_gpu_time_pending, 0, sizeof T_gpu_time_pending);
    totk_thread_walk();
    g_lag_guest_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("walking");
    int slot_trace=__builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        vk_fg_job_trace_enabled();
    uint64_t slot_lock_request=slot_trace ? host_now_ns() : 0;
    vk_rec_lock(); mtl_init();
    if (!g_cur && __builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0)) {
        uint64_t enter=slot_trace ? host_now_ns() : 0;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=enter kind=begin boundary_ns=%llu lock_request_ns=%llu open_primary=0 clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)slot_lock_request);
        vk_fg_slot_boundary();
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=exit kind=begin enter_ns=%llu exit_ns=%llu clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)host_now_ns());
    } else if (slot_trace) {
        LOGV("[adaptive-slot-boundary] event=skip kind=begin now_ns=%llu lock_request_ns=%llu open_primary=%u active=%u clock=CLOCK_MONOTONIC",
            (unsigned long long)host_now_ns(),(unsigned long long)slot_lock_request,
            g_cur!=NULL,atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed)!=0);
    }
    if (vk_xlate_stats_on()) { g_xlate->counts.submissions++; xlate_publish(); }
    g_gpu_guest_ns = guest_ns; g_gpu_origin_ticks = origin_ticks; g_gpu_guest_frame = frame;
    lag_pressure_fallback();
    T_ft_walk_frame = totk_ft_on() ? totk_ft_current() : 0;
    totk_ft_work_begin(T_ft_walk_frame);
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_begin(VK_REC_S_STREAM);
    else if (T_ft_walk_frame) rec_timeline_begin();
    if (g_lookup_stats) atomic_store_explicit(&g_lookup_thread, (uintptr_t)pthread_self(), memory_order_relaxed);
    g_ncarry = 0; snap_reset(); g_perf_t0 = host_now_ns(); g_perf_cpu_t0 = walker_cpu_now();
    g_perf_sub0 = g_perf_wait_ns; g_in_sub = 1;
}
void vk_gpu_write(uint64_t a, uint64_t n);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n) return;
    tex_t trace_src, trace_dst;
    int trace = 0;
    if (totk_texcopy_on()) {
        memset(&trace_src, 0, sizeof trace_src); memset(&trace_dst, 0, sizeof trace_dst);
        trace = totk_texcopy_overlap(src, n, &trace_src) | totk_texcopy_overlap(dst, n, &trace_dst);
    }
    if (trace)
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToBuffer result=attempt src_ser=%llu dst_ser=%llu src_addr=%#llx dst_addr=%#llx bytes=%llu",
            (unsigned long long)trace_src.serial, (unsigned long long)trace_dst.serial,
            (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    vk_pd_write_note("buffer copy to", dst, n, NULL);
    { int vk_write_range_hit(uint64_t a, uint64_t n); extern uint64_t g_frame_pub __attribute__((weak)); static unsigned nb;
      if ((vk_write_range_hit(dst, n) || vk_write_range_hit(src, n)) && nb++ < 4000)
          LOGV("[b2b] fp=%llu src=%#llx dst=%#llx bytes=%#llx src_nonzero=%zu", (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n,
               totk_texcopy_nonzero((const void *)(uintptr_t)src, (size_t)(n > (1u << 20) ? (1u << 20) : n))); }
    VkBuffer sb, db; uint64_t so, dof;
    int ok = vk_resolve(src, n, &sb, &so) && vk_resolve(dst, n, &db, &dof);
    if (ub_private_on()) { vk_ub_touch(src, n); vk_ub_touch(dst, n); }
    else ub_flush();
    vk_gpu_write(dst, n);
    if (!ok || (sb == db && so < dof + n && dof < so + n)) {   /* outside the pools, or overlapping in one buffer (Vulkan forbids it): the CPU does it in order */
        vk_cpu_step(); VK_REC_DO(VK_REC_S, memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n));
        if (trace) {
            size_t counted = (size_t)(n > (1u << 20) ? (1u << 20) : n);
            totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToBuffer result=success path=cpu src_ser=%llu dst_ser=%llu counted=%zu nonzero=%zu truncated=%d",
                (unsigned long long)trace_src.serial, (unsigned long long)trace_dst.serial, counted,
                totk_texcopy_nonzero((const void *)(uintptr_t)dst, counted), n > counted);
        }
        return;
    }
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_B2B);   /* transfers happen outside rendering */
    VkDescriptorBufferInfo frozen;
    if (vk_lag_read(src, n, 16, &frozen, VK_LAG_COPY)) { sb = frozen.buffer; so = frozen.offset; }
    VkCommandBuffer cb = vk_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vk_barrier_all(cb);
    VkBufferCopy c = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (trace)
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToBuffer result=success path=gpu src_ser=%llu dst_ser=%llu src_buffer=%#llx src_offset=%#llx dst_buffer=%#llx dst_offset=%#llx bytes=%llu content=not-sampled",
            (unsigned long long)trace_src.serial, (unsigned long long)trace_dst.serial,
            (unsigned long long)(uintptr_t)sb, (unsigned long long)so, (unsigned long long)(uintptr_t)db, (unsigned long long)dof, (unsigned long long)n);
    g_stat_gpu_copy++;
}
/* ---------- inline uniform updates without splitting the render pass (nvn_mtl.m "renaming") ----------
   The game updates small uniform ranges between draws (nvnCommandBufferUpdateUniformBuffer) over a hundred times per
   submission. A buffer write cannot happen inside a render pass, and on a tiled GPU every pass split stores and reloads
   all attachments. So an update is only logged: a later uniform binding that overlaps logged updates gets a private
   copy in the submission's transient memory (the pool's bytes with the logged updates laid over them in order), so draws
   before an update read the old bytes and draws after it the new ones. The logged updates reach the pool at the end of
   the submission, or earlier when anything other than a uniform binding may read or write an updated range
   (vk_ub_touch: vertex, index and storage buffers of a draw; copies, dispatches and other work outside passes).
   An entry stays logged, as an overlay for later copies, until the GPU has finished its submission (the CPU's view of
   the pool only has it from then). Renaming is opt-in (TOTK_VK_UB_RENAME=1); by default updates split the pass and write
   in place, except protected frame lag. There, UBO-only updates remain CPU-known logical shadows and bindings use
   immutable submission-arena copies. Only real aliases materialize GPU writes; remaining CPU-known bytes publish
   after the final QueueSubmit, before synchronous replay returns to the CPU producer. This is not global renaming. */
typedef struct { uint64_t lo, hi, seq, fence; int flushed; uint8_t *data; } ub_ent;   /* fence: submission it reached memory in (0: not yet) */
static uint64_t ub_address(uint64_t addr, uint64_t size);
static ub_ent *g_ub; static uint64_t *g_ub_maxhi; static int g_nub, g_capub, g_ub_unflushed; static uint64_t g_ub_seq, g_ub_lo = UINT64_MAX, g_ub_hi;
static uint64_t g_ub_maxlen, g_ub_pruned_at;   /* longest logged entry (bounds the overlap scan); g_done at the last prune */
static uint64_t g_ub_gen = 1;   /* rename memo: every log or snapshot change invalidates its answer */
uint64_t vk_record_uniform_revision(void) { return g_ub_gen; }
int vk_record_updates_pending(void) {
    if (vk_rec_profile_on && T_rec.active) { g_rec.touch_calls++; g_rec.touch_no_updates += g_ub_unflushed == 0; }
    return g_ub_unflushed != 0;
}
/* Exact range invalidations for recorder freezes. Missing/overwritten journal entries conservatively miss. */
enum { REC_UNIFORM_CHANGES = 256 };
static struct { uint64_t revision, lo, hi; } g_record_uniform_changes[REC_UNIFORM_CHANGES];
static void ub_changed(uint64_t address, uint64_t size) {
    g_ub_gen++;
    if (vk_record_enabled() || vk_lag_capture_on()) {
        uint64_t hi = address + size; if (hi < address) hi = UINT64_MAX;
        g_record_uniform_changes[g_ub_gen % REC_UNIFORM_CHANGES] = (__typeof__(*g_record_uniform_changes)){g_ub_gen, address, hi};
    }
}
static int ub_range_unchanged(uint64_t revision, uint64_t address, uint64_t size) {
    if (revision == g_ub_gen) return 1;
    if (!revision || revision > g_ub_gen || g_ub_gen - revision > REC_UNIFORM_CHANGES || address + size < address) return 0;
    for (uint64_t i = revision + 1; i <= g_ub_gen; i++) {
        const __typeof__(*g_record_uniform_changes) *c = &g_record_uniform_changes[i % REC_UNIFORM_CHANGES];
        if (c->revision != i || (address < c->hi && c->lo < address + size)) return 0;
    }
    return 1;
}
/* Pending-original authority is independent of snapshot resets and CPU-only logical overlays. Only an event
   introducing a queued original-memory writer advances this journal; completion can only remove producers.
   Keep repeated writes before seen-range deduplication. Exact history loss conservatively misses unless the
   spatial revision summary independently proves that no producer could have touched the requested range. */
enum { LAG_PRODUCER_CHANGES = 4096, LAG_PRODUCER_BUCKETS = 4096, LAG_PRODUCER_MAX_WINDOWS = 64 };
static uint64_t g_lag_producer_revision = 1;
static struct { uint64_t revision, lo, hi; } g_lag_producer_changes[LAG_PRODUCER_CHANGES];
/* Walk-owned monotonic revisions, not dirty/watch or byte authority. Hash collisions only make a query
   consult the exact journal. Large/overflowing writes advance the global floor instead of walking unbounded VAs. */
static uint64_t g_lag_producer_buckets[LAG_PRODUCER_BUCKETS], g_lag_producer_wide;
static unsigned lag_producer_bucket(uint64_t window) {
    return (unsigned)(window ^ (window >> 12) ^ (window >> 24)) & (LAG_PRODUCER_BUCKETS - 1);
}
static void lag_producer_changed(uint64_t address, uint64_t size) {
    if (!vk_lag_capture_on()) return;
    uint64_t revision = ++g_lag_producer_revision, hi = address + size;
    if (hi < address) hi = UINT64_MAX;
    g_lag_producer_changes[revision % LAG_PRODUCER_CHANGES] =
        (__typeof__(*g_lag_producer_changes)){revision, address, hi};
    uint64_t first = address >> 16, last = (hi - 1) >> 16;
    if (!size || hi <= address || last - first >= LAG_PRODUCER_MAX_WINDOWS) g_lag_producer_wide = revision;
    else for (uint64_t w = first; w <= last; w++) g_lag_producer_buckets[lag_producer_bucket(w)] = revision;
}
static int lag_producer_unchanged(uint64_t revision, uint64_t address, uint64_t size) {
    if (revision == g_lag_producer_revision) return 1;
    if (!revision || revision > g_lag_producer_revision || address + size < address) return 0;
    uint64_t first = address >> 16, last = (address + size - 1) >> 16;
    if (size && last - first < LAG_PRODUCER_MAX_WINDOWS && g_lag_producer_wide <= revision) {
        int unchanged = 1;
        for (uint64_t w = first; w <= last; w++)
            if (g_lag_producer_buckets[lag_producer_bucket(w)] > revision) { unchanged = 0; break; }
        if (unchanged) return 1;
    }
    if (g_lag_producer_revision - revision > LAG_PRODUCER_CHANGES) return 0;
    for (uint64_t i = revision + 1; i <= g_lag_producer_revision; i++) {
        const __typeof__(*g_lag_producer_changes) *c = &g_lag_producer_changes[i % LAG_PRODUCER_CHANGES];
        if (c->revision != i || (address < c->hi && c->lo < address + size)) return 0;
    }
    return 1;
}
int vk_record_uniform_unchanged(uint64_t revision, uint64_t address, uint64_t size) {
    return ub_range_unchanged(revision, ub_address(address, size), size);
}
int vk_off(const char *name) {
    const char *e = getenv("TOTK_VK_OFF"); if (!e || !*e) return 0;
    size_t n = strlen(name);
    for (const char *p = e; *p; ) {
        const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p);
        if ((l == n && !strncmp(p, name, n)) || (l == 3 && !strncmp(p, "all", 3))) return 1;
        p += l; if (*p == ',') p++;
    }
    return 0;
}
static int ub_rename_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_UB_RENAME"); on = e && atoi(e) && !vk_off("rename"); } return on; }
static int ub_private_on(void) { return vk_lag_capture_on() && !ub_rename_on(); }
int vk_lag_uniform_private(void) { return ub_private_on(); }
/* The replay lock gives these counters one walking owner; workers never increment them. Keep cumulative totals
   for host fixtures and take interval deltas only at the existing 300-frame report. Disabled lag never visits them. */
static vk_lag_kind_stats g_lag_kind[VK_LAG_KIND_COUNT];
static uint64_t lag_hot_limit(void);
static int lag_timing_on(void);
#ifdef TOTK_VK_TESTS
void vk_lag_test_stats_get(vk_lag_test_stats *out) { memcpy(out->kind, g_lag_kind, sizeof g_lag_kind); }
uint32_t vk_lag_test_features(void) { return VK_LAG_TEST_CROSS_FRAME_METADATA | VK_LAG_TEST_UNIFORM_COVER; }
#endif
/* Match the existing lag memo semantics, not the nested producer/watch/copy cache probes. */
static inline void ft_lag_count(vk_lag_kind kind, int miss) {
    if (__builtin_expect(totk_ft_work_frame() == 0, 1)) return;
    static const enum totk_ft_counter counters[] = {
        FT_COUNT_LAG_VERTEX, FT_COUNT_LAG_INDEX, FT_COUNT_LAG_STORAGE, FT_COUNT_LAG_UNIFORM
    };
    if ((unsigned)kind < sizeof counters / sizeof *counters) totk_ft_count(counters[kind] + miss, 1);
    (void)miss; /* host builds compile out the counter API */
}
static inline void ft_snapshot_allocation(uint64_t size) {
    totk_ft_count(FT_COUNT_SNAP_ALLOCS, 1);
    totk_ft_count(FT_COUNT_SNAP_BYTES, size);
    (void)size;
}
static uint64_t lag_decision_begin(vk_lag_kind kind, uint64_t size) {
    vk_lag_kind_stats *s = &g_lag_kind[kind];
    ft_lag_count(kind, 0);
    s->decisions++;
    if (size > s->max_size) s->max_size = size;
    unsigned bucket = size <= 256 ? 0 : size <= 4096 ? 1 : size <= 65536 ? 2 : size <= lag_hot_limit() ? 3 : 4;
    s->size_buckets[bucket]++;
    return (s->decisions & 63) == 1 && lag_timing_on() ? host_now_ns() : 0;
}
static void lag_decision_end(vk_lag_kind kind, uint64_t start) {
    if (start) { g_lag_kind[kind].sample_ns += host_now_ns() - start; g_lag_kind[kind].sample_count++; }
}
static void lag_snapshot_allocation(vk_lag_kind kind, uint64_t size) {
    g_lag_kind[kind].snapshot_allocations++; g_lag_kind[kind].snapshot_bytes += size;
    if (kind == VK_LAG_UNIFORM) ft_snapshot_allocation(size);
}
/* Fold every address lane before the table mask: equal-sized streams 64 KiB apart must not collide. */
static unsigned lag_hash(uint64_t address, uint64_t size, uint64_t align) {
    uint64_t x = (address >> 4) ^ (address >> 16) ^ (address >> 32) ^ size ^ (align << 7);
    x ^= x >> 23; x *= UINT64_C(0x9e3779b1); x ^= x >> 16;
    return (unsigned)x;
}
/* One direct-mapped collision used to trigger a scan of every physical pool on every alternating
   binding. Four ways retain those owners without changing mapping/window or byte-authority keys. */
static uint64_t lag_owner_base(VkBuffer buffer) {
    enum { NB = 4096, WAYS = 4 };
    static struct { VkBuffer buffer; uint64_t generation, base; } owners[NB][WAYS];
    static unsigned char next[NB];
    unsigned h = lag_hash((uint64_t)(uintptr_t)buffer, 0, 0) % NB;
    uint64_t generation = atomic_load_explicit(&g_owner_gen, memory_order_acquire);
    unsigned replace = next[h] % WAYS;
    for (unsigned i = 0; i < WAYS; i++) {
        if (owners[h][i].generation != generation) { replace = i; continue; }
        if (owners[h][i].buffer == buffer) return owners[h][i].base;
    }
    uint64_t base = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i]->kind == K_OWN && g_pools[i]->buf == buffer) { base = g_pools[i]->base; break; }
    generation = atomic_load_explicit(&g_owner_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    owners[h][replace] = (__typeof__(owners[0][0])){buffer, generation, base};
    next[h] = (unsigned char)(replace + 1);
    return base;
}
#ifdef TOTK_VK_TESTS
static uint64_t lag_test_owner_direct(VkBuffer buffer) {
    static struct { VkBuffer buffer; uint64_t generation, base; } owners[4096];
    unsigned h = lag_hash((uint64_t)(uintptr_t)buffer, 0, 0) % 4096;
    uint64_t generation = atomic_load_explicit(&g_owner_gen, memory_order_acquire);
    if (owners[h].buffer == buffer && owners[h].generation == generation) return owners[h].base;
    uint64_t base = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i]->kind == K_OWN && g_pools[i]->buf == buffer) { base = g_pools[i]->base; break; }
    generation = atomic_load_explicit(&g_owner_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    owners[h] = (__typeof__(owners[0])){buffer, generation, base};
    return base;
}
static int lag_test_owner_bench(void) {
    if (g_npool || g_pools) return 1; /* isolated pre-init query fixture only */
    enum { POOLS = 2048, CALLS = 20000 };
    pool_ent *pools = calloc(POOLS, sizeof *pools);
    g_pools = calloc(POOLS, sizeof *g_pools); g_npool = POOLS;
    if (!pools || !g_pools) abort();
    for (int i = 0; i < POOLS; i++) { g_pools[i] = &pools[i]; pools[i].kind = K_OWN; }
    VkBuffer collision[4]; unsigned count = 0;
    for (uintptr_t value = 16; count < 4; value += 16) if (lag_hash(value, 0, 0) % 4096 == 0)
        collision[count++] = (VkBuffer)value;
    for (int i = 0; i < 4; i++) { pools[POOLS - 4 + i].buf = collision[i]; pools[POOLS - 4 + i].base = (uint64_t)(i + 1) * 65536; }
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    uint64_t ns[2][3], checksum[2] = {0}; unsigned samples[2] = {0};
    for (unsigned pass = 0; pass < 6; pass++) {
        unsigned mode = (pass == 1 || pass == 2 || pass == 5);
        uint64_t sum = 0, start = host_now_ns();
        for (unsigned i = 0; i < CALLS; i++) sum += mode ? lag_owner_base(collision[i % 4]) : lag_test_owner_direct(collision[i % 4]);
        ns[mode][samples[mode]++] = host_now_ns() - start; checksum[mode] += sum;
    }
    unsigned errors = checksum[0] != checksum[1];
    for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned i = 1; i < 3; i++) for (unsigned j = i; j && ns[mode][j] < ns[mode][j - 1]; j--) {
            uint64_t swap = ns[mode][j]; ns[mode][j] = ns[mode][j - 1]; ns[mode][j - 1] = swap;
        }
    printf("lag-query owner collision stress: %u pools, 4 colliding handles; direct %.2f four-way %.2f ns/query "
           "(interleaved medians, not an observed device collision rate); checksums %llu/%llu\n",
           POOLS, ns[0][1] / (double)CALLS, ns[1][1] / (double)CALLS,
           (unsigned long long)checksum[0], (unsigned long long)checksum[1]);
    pools[POOLS - 4].base += 16;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    errors += lag_owner_base(collision[0]) != pools[POOLS - 4].base;
    pools[POOLS - 4].kind = K_CPU;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    errors += lag_owner_base(collision[0]) != 0; /* negative answers are generation-scoped too */
    free(pools); free(g_pools); g_pools = NULL; g_npool = 0;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    return errors != 0;
}
#endif
static uint64_t lag_physical(uint64_t addr, uint64_t size);
static uint64_t ub_address(uint64_t addr, uint64_t size) { return ub_private_on() ? lag_physical(addr, size) : addr; }
static void ub_maxhi_from(int i) { for (; i < g_nub; i++) { uint64_t prev = i ? g_ub_maxhi[i - 1] : 0; g_ub_maxhi[i] = g_ub[i].hi > prev ? g_ub[i].hi : prev; } }
static uint64_t g_prune_floor = UINT64_MAX;
static void ub_prune_inner(void) {   /* drop entries whose submission the GPU has finished, and that no later snapshot still needs */
    /* Only a finished submission or a changed snapshot floor can drop entries; vk_ub_rename calls this for every uniform
       binding, so without either it returns at once. */
    uint64_t done = atomic_load(&g_done); int j = 0;
    if (done > g_prune_floor) done = g_prune_floor;   /* a submission queued after the one replaying captured no earlier than it */
    if (done == g_ub_pruned_at) return;
    g_ub_pruned_at = done;
    g_ub_lo = UINT64_MAX; g_ub_hi = 0; g_ub_maxlen = 0;
    for (int i = 0; i < g_nub; i++) {
        if (g_ub[i].fence && g_ub[i].fence <= done) { free(g_ub[i].data); continue; }
        g_ub[j++] = g_ub[i];
        if (g_ub[i].lo < g_ub_lo) g_ub_lo = g_ub[i].lo; if (g_ub[i].hi > g_ub_hi) g_ub_hi = g_ub[i].hi;
        if (g_ub[i].hi - g_ub[i].lo > g_ub_maxlen) g_ub_maxlen = g_ub[i].hi - g_ub[i].lo;
    }
    if (j != g_nub) { g_nub = j; ub_maxhi_from(0); ub_changed(0, 0); }
}
static void ub_prune(void) { VK_REC_DO(VK_REC_S_UNIFORM, ub_prune_inner()); }
static void snap_note_update(uint64_t lo, uint64_t n);
static void ub_log(uint64_t lo, const uint8_t *data, uint64_t n, int flushed) {
    lo = ub_address(lo, n);
    /* Entries the new update covers entirely can never show through again: renames, flushes and overlays all apply the
       newer one after them, and bindings recorded before it already have their copies. The game rewrites the same
       constant blocks many times a frame; keeping only the live ones keeps every lookup short. */
    int a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < lo) a = m + 1; else b = m; }
    uint8_t *reused = NULL; int recorder = vk_record_enabled();
    int first = a, j = a;
    for (int i = a; i < g_nub; i++) {
        if (g_ub[i].lo < lo + n && g_ub[i].hi <= lo + n) {
            if (!g_ub[i].flushed) g_ub_unflushed--;
            if (recorder && !reused && g_ub[i].hi - g_ub[i].lo == n) reused = g_ub[i].data;
            else free(g_ub[i].data);
            continue;
        }
        if (g_ub[i].lo >= lo + n) {   /* past the new range: the rest stays, moved down only if something was dropped */
            if (j != i) { int rest = g_nub - i; memmove(g_ub + j, g_ub + i, (size_t)rest * sizeof *g_ub); j += rest; } else j = g_nub;
            break;
        }
        g_ub[j++] = g_ub[i];
    }
    if (j != g_nub) { g_nub = j; ub_maxhi_from(first); }
    if (g_nub == g_capub) { g_capub = g_capub ? 2 * g_capub : 256; g_ub = VK_REALLOC(g_ub, (size_t)g_capub * sizeof *g_ub); g_ub_maxhi = VK_REALLOC(g_ub_maxhi, (size_t)g_capub * sizeof *g_ub_maxhi); }
    a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo <= lo) a = m + 1; else b = m; }   /* sorted by lo */
    memmove(g_ub + a + 1, g_ub + a, (size_t)(g_nub - a) * sizeof *g_ub);
    uint8_t *d = reused ? reused : VK_MALLOC((size_t)n); memcpy(d, data, (size_t)n);
    if (vk_rec_profile_on && T_rec.active) { if (reused) g_rec.ub_reused++; else g_rec.ub_allocated++; }
    g_ub[a] = (ub_ent){lo, lo + n, ++g_ub_seq, 0, flushed, d}; g_nub++; g_ub_unflushed += !flushed; ub_changed(lo, n);
    snap_note_update(lo, n);
    ub_maxhi_from(a);
    if (lo < g_ub_lo) g_ub_lo = lo; if (lo + n > g_ub_hi) g_ub_hi = lo + n; if (n > g_ub_maxlen) g_ub_maxlen = n;
}
/* the logged entries overlapping [lo, hi) (only_unflushed: not yet written), in update order; returns how many.
   Entries are sorted by start: walking back from the last one starting below hi, the walk ends where no earlier entry
   can reach lo, by the running maximum of ends or, tighter when one long update keeps that high, by the longest entry. */
static int ub_overlaps(uint64_t lo, uint64_t hi, int only_unflushed, int *idx, int cap) {
    if (!g_nub || hi <= g_ub_lo || lo >= g_ub_hi) return 0;
    int a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < hi) a = m + 1; else b = m; }
    int n = 0; uint64_t floor = lo > g_ub_maxlen ? lo - g_ub_maxlen : 0;   /* an entry starting at or below floor ends by lo */
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > lo && (g_ub[i].lo > floor || !floor); i--) if (g_ub[i].hi > lo && (!only_unflushed || !g_ub[i].flushed)) { if (n == cap) return -1; idx[n++] = i; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[idx[j - 1]].seq > g_ub[idx[j]].seq; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    return n;
}
static int ub_entry_cmp(const void *x, const void *y) {
    const ub_ent *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}
static void ub_reindex(void) {
    g_ub_lo = UINT64_MAX; g_ub_hi = g_ub_maxlen = 0; g_ub_unflushed = 0;
    for (int i = 0; i < g_nub; i++) {
        ub_ent *e = &g_ub[i];
        if (e->lo < g_ub_lo) g_ub_lo = e->lo;
        if (e->hi > g_ub_hi) g_ub_hi = e->hi;
        if (e->hi - e->lo > g_ub_maxlen) g_ub_maxlen = e->hi - e->lo;
        g_ub_unflushed += !e->flushed;
    }
    ub_maxhi_from(0);
}
/* Revoke only bytes replaced by a real producer. A partial producer must not erase neighbouring private updates. */
static void ub_forget(uint64_t a, uint64_t n) {
    int changed = 0;
    for (int i = 0; i < g_nub; i++) {
        ub_ent *e = &g_ub[i];
        if (e->lo >= a + n || e->hi <= a) continue;
        changed = 1;
        if (a <= e->lo && e->hi <= a + n) {
            free(e->data); memmove(e, e + 1, (size_t)(g_nub - i - 1) * sizeof *e); g_nub--; i--;
        } else if (a <= e->lo) {
            uint64_t skip = a + n - e->lo;
            memmove(e->data, e->data + skip, (size_t)(e->hi - a - n)); e->lo = a + n;
        } else if (e->hi <= a + n) e->hi = a;
        else {
            ub_ent right = *e;
            right.lo = a + n; right.data = VK_MALLOC((size_t)(right.hi - right.lo));
            memcpy(right.data, e->data + right.lo - e->lo, (size_t)(right.hi - right.lo)); e->hi = a;
            if (g_nub == g_capub) {
                g_capub = g_capub ? 2 * g_capub : 256;
                g_ub = VK_REALLOC(g_ub, (size_t)g_capub * sizeof *g_ub);
                g_ub_maxhi = VK_REALLOC(g_ub_maxhi, (size_t)g_capub * sizeof *g_ub_maxhi);
            }
            g_ub[g_nub++] = right;
        }
    }
    if (changed) { qsort(g_ub, (size_t)g_nub, sizeof *g_ub, ub_entry_cmp); ub_reindex(); ub_changed(a, n); }
}
/* Protected lag already forces synchronous replay in hle_nvn.c. Publish CPU-known, UBO-only updates after ALL
   recorder jobs join and their final ticket is submitted, before the producer can rewrite the next frame.
   This is a host publication, not a queued original-buffer GPU write: old immutable descriptors cannot be clobbered.
   lag_store retains genuine non-UBO unit guards (including neighbouring aliases), and may wait only on submitted work.
   Never use dirty-unit changes to discard authority: an unrelated CPU write in the same unit is not an overwrite. */
static void ub_publish(void) {
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("uniform-publication");
    if (!g_ub_unflushed) {
        nvn_dirty_lag_thread_role(previous_role);
        return;
    }
    static int *order, cap;
    if (g_ub_unflushed > cap) { cap = g_ub_unflushed; order = VK_REALLOC(order, (size_t)cap * sizeof *order); }
    int n = 0;
    for (int i = 0; i < g_nub; i++) if (!g_ub[i].flushed) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[order[j - 1]].seq > g_ub[order[j]].seq; j--) {
        int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
    for (int k = 0; k < n; k++) {
        ub_ent *e = &g_ub[order[k]];
        if (!nvn_dirty_lag_store((void *)(uintptr_t)e->lo, e->data, (size_t)(e->hi - e->lo)))
            DIE("lag uniform publication could not restore protection");
    }
    int j = 0;
    for (int i = 0; i < g_nub; i++) {
        if (!g_ub[i].flushed) free(g_ub[i].data);
        else g_ub[j++] = g_ub[i];
    }
    g_nub = j; ub_reindex(); ub_changed(0, UINT64_MAX);
    nvn_dirty_lag_thread_role(previous_role);
}
/* Materialize only updates a real consumer can observe. Chunk seals are not consumers of private UBO bytes. */
static void ub_flush_inner(uint64_t lo, uint64_t hi) {
    if (!g_ub_unflushed) return;
    int cnt = 0;
    for (int i = 0; i < g_nub; i++) cnt += !g_ub[i].flushed && g_ub[i].lo < hi && lo < g_ub[i].hi;
    if (!cnt) return;
    vk_end_rendering_at(VK_PASS_END_UNIFORM, 0);
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    static int *order, cap;
    if (cnt > cap) { cap = cnt; order = VK_REALLOC(order, (size_t)cap * sizeof *order); }
    int n = 0;
    for (int i = 0; i < g_nub; i++) if (!g_ub[i].flushed && g_ub[i].lo < hi && lo < g_ub[i].hi) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[order[j - 1]].seq > g_ub[order[j]].seq; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    uint64_t prev_lo = UINT64_MAX, prev_hi = 0;
    for (int k = 0; k < n; k++) {
        ub_ent *e = &g_ub[order[k]]; VkBuffer db; uint64_t dof;
        if (vk_resolve(e->lo, e->hi - e->lo, &db, &dof)) {
            lag_guard_mark(e->lo, e->hi - e->lo, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_GPU);
            if (e->lo < prev_hi && prev_lo < e->hi) {   /* overlapping writes in order */
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
                VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
                vkCmdPipelineBarrier2(cb, &di);
                prev_lo = UINT64_MAX; prev_hi = 0;
            }
            vkCmdUpdateBuffer(cb, db, dof, e->hi - e->lo, e->data);
            if (e->lo < prev_lo) prev_lo = e->lo;
            if (e->hi > prev_hi) prev_hi = e->hi;
        }
        e->flushed = 1; g_ub_unflushed--;
        ub_changed(e->lo, e->hi - e->lo); /* a private overlay became a real original-memory producer */
        lag_producer_changed(e->lo, e->hi - e->lo);
    }
    vk_barrier_all(cb);
}
static void ub_flush(void) { VK_REC_DO(VK_REC_S_UNIFORM, ub_flush_inner(0, UINT64_MAX)); }
static int ub_pending_physical(uint64_t addr, uint64_t n);
void vk_ub_touch(uint64_t addr, uint64_t n) {   /* a non-uniform access: the updates it may see go to memory first */
    if (!g_ub_unflushed || !n) return;
    addr = ub_address(addr, n);
    if (ub_pending_physical(addr, n)) {
        if (ub_private_on()) VK_REC_DO(VK_REC_S_UNIFORM, ub_flush_inner(addr, addr + n));
        else ub_flush();
    }
}
void vk_ub_flush(void) { ub_flush(); }
int vk_ub_pending(uint64_t addr, uint64_t n) {
    if (!g_ub_unflushed || !n) return 0; /* no updates: no physical mapping can change the answer */
    return ub_pending_physical(ub_address(addr, n), n);
}
/* addr is already canonical (ub_address). Mapping is idempotent, so vk_ub_touch need not map a second time: that
   repeated lookup missed the window memo (keyed by the virtual address) and was ~18% of lag_physical_window. */
static int ub_pending_physical(uint64_t addr, uint64_t n) {
    if (!g_ub_unflushed || !n) return 0;
#ifdef TOTK_VK_TESTS
    if (ub_address(addr, n) != addr) DIE("ub_pending_physical: address mapping is not idempotent at 0x%llx+%llu",
                                         (unsigned long long)addr, (unsigned long long)n);
#endif
    if (!vk_record_enabled()) { int idx[1]; return ub_overlaps(addr, addr + n, 1, idx, 1) != 0; }
    enum { NC = 256 }; static struct { uint64_t address, size, revision; } negative[NC];
    __typeof__(negative[0]) *m = &negative[(unsigned)((addr >> 4) ^ (addr >> 18) ^ n) % NC];
    if (m->address == addr && m->size == n && m->revision &&
        (m->revision == g_ub_gen || vk_record_uniform_unchanged(m->revision, addr, n))) {
        m->revision = g_ub_gen;
        if (vk_rec_profile_on && T_rec.active) g_rec.touch_negative_hits++;
        return 0;
    }
    int idx[1], pending = ub_overlaps(addr, addr + n, 1, idx, 1) != 0;
    if (!pending) *m = (__typeof__(*m)){addr, n, g_ub_gen};
    return pending;
}
void vk_record_touch_buffer(uint64_t address, uint64_t size) {
    if (!address || !g_ub_unflushed) return;
    if (vk_rec_profile_on && T_rec.active) g_rec.touch_ranges++;
    if (size && address + size >= address && !vk_ub_pending(address, size)) {
        if (vk_rec_profile_on && T_rec.active) g_rec.touch_resolve_avoided++;
        return;
    }
    VkBuffer buffer; uint64_t offset, available;
    if (vk_resolve_for(address, 1, &buffer, &offset, &available, VK_LOOKUP_TOUCH))
        vk_ub_touch(address, size && size < available ? size : available);
}
/* ---------- uniform snapshots at submit (nvn_mtl.m usnap) ----------
   The GPU reads a submission's uniforms when it gets to them, which here can be a few frames after the submit call,
   while the game rewrites its constant pools right after submitting (the next frame's values): objects then drew with
   the wrong frame's uniforms (black flicker). So the uniform ranges a submission binds are copied when it is submitted
   (mtl_sub_capture_ubos on the game thread with TOTK_ASYNC_REPLAY, else at the start of the replay, which then runs inside
   the submit call), merged per pool, into the submission's transient memory; bindings read those copies. Logged inline
   updates of earlier submissions that were not in memory at the capture are laid over the copies; this submission's own
   updates go through renaming on top of them. A GPU copy into a range drops its snapshot (later bindings read memory on
   the GPU timeline). Ordinary snapshots require opt-in renaming; protected rename-OFF UBO captures are mandatory. */
typedef struct { uint64_t done; int n; uint64_t *lo, *hi; size_t *off; uint8_t *bytes; } ucap_t;
typedef struct { uint64_t lo, hi; VkBuffer buf; VkDeviceSize off; uint8_t *cpu; int dead, updated; } snap_t;
static snap_t *g_snap; static int g_nsnap, g_capsnap;
static uint64_t g_snap_seq, g_snap_generation;   /* overlay watermark and immutable game-submission base identity */
static void snap_reset(void) { g_nsnap = 0; g_snap_seq = g_ub_seq; g_snap_generation++; g_prune_floor = UINT64_MAX; ub_changed(0, UINT64_MAX); }
static uint64_t g_stat_snap_bytes, g_stat_snap_ranges, g_stat_snap_binds;
static int snap_on(void) { return (ub_rename_on() && !vk_off("snap")) || ub_private_on(); }
/* GPU writes (writable storage bindings, buffer copies, texture-to-buffer copies) with the submission they belong to
   (0 while it is being recorded): a capture taken while one of them may still be executing would miss its result, so
   ranges they overlap are not copied (bindings there read memory on the GPU timeline). */
/* Sorted by lo, with g_gw_maxhi[i] the largest hi among entries 0..i: an overlap query is a binary search plus a
   backward walk that stops once no earlier entry can reach the query (the uniform log's index, ub_original_pending).
   Captures query once per UBO range and lag reads once per memo miss, so the previous linear scan per query cost
   ~0.8 ms/frame on the Thor walker. Sealing only assigns fences and pruning is a stable filter: both keep the order. */
typedef struct { uint64_t lo, hi, fence; } gw_t;
static gw_t *g_gw; static uint64_t *g_gw_maxhi; static int g_ngw, g_capgw; static pthread_mutex_t g_gw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stat_snap_skipped;
static void gw_reindex_from(int i) {   /* caller holds g_gw_mu */
    uint64_t m = i ? g_gw_maxhi[i - 1] : 0;
    for (; i < g_ngw; i++) { if (g_gw[i].hi > m) m = g_gw[i].hi; g_gw_maxhi[i] = m; }
}
static int gw_lower(uint64_t lo) {   /* first entry with lo >= the argument; caller holds g_gw_mu */
    int a = 0, b = g_ngw;
    while (a < b) { int m = (a + b) >> 1; if (g_gw[m].lo < lo) a = m + 1; else b = m; }
    return a;
}
static int gw_pending(uint64_t lo, uint64_t hi, uint64_t done) {   /* caller holds g_gw_mu */
    for (int i = gw_lower(hi) - 1; i >= 0 && g_gw_maxhi[i] > lo; i--)
        if (lo < g_gw[i].hi && (!g_gw[i].fence || g_gw[i].fence > done)) return 1;
    return 0;
}
/* An identical unsealed range is already pending and receives the same fence at the next seal, so it is not added
   again (one buffer for many dispatches). Caller holds g_gw_mu. */
static void gw_add(uint64_t lo, uint64_t hi) {
    int at = gw_lower(lo);
    for (int j = at; j < g_ngw && g_gw[j].lo == lo; j++) if (g_gw[j].hi == hi && !g_gw[j].fence) return;
    if (g_ngw == g_capgw) {
        g_capgw = g_capgw ? 2 * g_capgw : 1024;
        g_gw = VK_REALLOC(g_gw, (size_t)g_capgw * sizeof *g_gw);
        g_gw_maxhi = VK_REALLOC(g_gw_maxhi, (size_t)g_capgw * sizeof *g_gw_maxhi);
    }
    memmove(g_gw + at + 1, g_gw + at, (size_t)(g_ngw - at) * sizeof *g_gw);
    g_gw[at] = (gw_t){lo, hi, 0}; g_ngw++;
    gw_reindex_from(at);
}
static void gw_seal(uint64_t fence) { pthread_mutex_lock(&g_gw_mu); for (int i = 0; i < g_ngw; i++) if (!g_gw[i].fence) g_gw[i].fence = fence; pthread_mutex_unlock(&g_gw_mu); }
static void gw_prune(void) {
    uint64_t done = atomic_load(&g_done); int j = 0;
    pthread_mutex_lock(&g_gw_mu);
    for (int i = 0; i < g_ngw; i++) if (!g_gw[i].fence || g_gw[i].fence > done) g_gw[j++] = g_gw[i];
    g_ngw = j; gw_reindex_from(0);
    pthread_mutex_unlock(&g_gw_mu);
}
static void snap_drop(uint64_t a, uint64_t n);
static int ub_original_pending(uint64_t a, uint64_t n) {
    uint64_t hi = a + n;
    if (!g_nub || hi <= g_ub_lo || a >= g_ub_hi) return 0;
    uint64_t done = atomic_load(&g_done);
    /* The same sorted overlap index used by private overlays also bounds real producers. Do not
       scan every unrelated logical update for each UBO/vertex/index read after a memo collision. */
    int lo = 0, end = g_nub;
    while (lo < end) { int m = (lo + end) >> 1; if (g_ub[m].lo < hi) lo = m + 1; else end = m; }
    uint64_t floor = a > g_ub_maxlen ? a - g_ub_maxlen : 0;
    for (int i = lo - 1; i >= 0 && g_ub_maxhi[i] > a && (g_ub[i].lo > floor || !floor); i--)
        if (g_ub[i].flushed && a < g_ub[i].hi && (!g_ub[i].fence || g_ub[i].fence > done)) return 1;
    return 0;
}
/* Negative results survive actual submissions when mapping/lifetime and the relevant producer journal agree.
   A positive additionally expires whenever completion moves; sealing an unsubmitted producer does not add one. */
static int lag_original_pending(uint64_t address, uint64_t size) {
    enum { NP = 1024 };
    static struct { uint64_t address, size, revision, ticket, spans, lifetime, done; int pending; } memo[NP];
    __typeof__(memo[0]) *m = &memo[lag_hash(address, size, 0) % NP];
    uint64_t ticket = g_reserved_fence + 1, spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    uint64_t lifetime = nvn_dirty_lag_epoch();
    if (m->address == address && m->size == size && m->spans == spans && m->lifetime == lifetime &&
        (!m->pending || m->done == atomic_load(&g_done)) && lag_producer_unchanged(m->revision, address, size)) {
        LAG_TEST_INC(VK_LAG_TEST_PRODUCER_HITS);
        if (!m->pending && m->ticket != ticket) LAG_TEST_INC(VK_LAG_TEST_PRODUCER_CROSS_TICKET);
        m->revision = g_lag_producer_revision; m->ticket = ticket;
        return m->pending;
    }
    LAG_TEST_INC(VK_LAG_TEST_PRODUCER_SCANS);
    uint64_t done = atomic_load(&g_done);
    pthread_mutex_lock(&g_gw_mu);
    int pending = gw_pending(address, address + size, done);
    pthread_mutex_unlock(&g_gw_mu);
    if (!pending) pending = ub_original_pending(address, size);
    *m = (__typeof__(*m)){address, size, g_lag_producer_revision, ticket, spans, lifetime, done, pending};
    return pending;
}
#ifdef TOTK_VK_TESTS
static int lag_test_collect_bench(void);
/* Standalone host fixture: exercise the production query against the pre-index scalar oracle,
   including private entries, completed/unsubmitted writers and exact half-open boundaries. */
static int lag_test_pending_linear(uint64_t a, uint64_t n) {
    uint64_t done = atomic_load(&g_done);
    for (int i = 0; i < g_nub; i++) if (g_ub[i].flushed && g_ub[i].lo < a + n && a < g_ub[i].hi &&
        (!g_ub[i].fence || g_ub[i].fence > done)) return 1;
    return 0;
}
static int lag_test_producer_linear(uint64_t revision, uint64_t address, uint64_t size) {
    if (revision == g_lag_producer_revision) return 1;
    if (!revision || revision > g_lag_producer_revision ||
        g_lag_producer_revision - revision > LAG_PRODUCER_CHANGES || address + size < address) return 0;
    for (uint64_t i = revision + 1; i <= g_lag_producer_revision; i++) {
        const __typeof__(*g_lag_producer_changes) *c = &g_lag_producer_changes[i % LAG_PRODUCER_CHANGES];
        if (c->revision != i || (address < c->hi && c->lo < address + size)) return 0;
    }
    return 1;
}
/* The sorted GPU-write index against the previous unsorted list and linear scan (including its last-8 duplicate
   rule), over random overlapping/duplicate writes, seals, completions and prunes; then per-query cost. */
static unsigned lag_test_gw_index(unsigned *checks) {
    enum { MAXW = 4096, OPS = 200000 };
    if (g_ngw) return 1;
    gw_t *shadow = calloc(MAXW, sizeof *shadow); int nshadow = 0;
    if (!shadow) abort();
    unsigned errors = 0; uint64_t rng = 0x9e3779b97f4a7c15u, fence = 0, done = 0;
    #define GW_RAND() (rng = rng * 6364136223846793005u + 1442695040888963407u, rng >> 33)
    atomic_store(&g_done, 0);
    for (unsigned op = 0; op < OPS; op++) {
        unsigned kind = GW_RAND() % 100;
        if (kind < 30 && nshadow < MAXW - 8) {   /* clustered starts give overlaps and exact duplicates */
            uint64_t lo = UINT64_C(0x340000000) + (GW_RAND() % 512) * 256, hi = lo + 64 + (GW_RAND() % 8) * 512;
            if (GW_RAND() % 16 == 0) hi = lo + 65536;   /* occasional long range keeps the running maximum high */
            int dup = 0;
            for (int j = nshadow - 1; j >= 0 && j >= nshadow - 8; j--) if (shadow[j].lo == lo && shadow[j].hi == hi && !shadow[j].fence) dup = 1;
            if (!dup) shadow[nshadow++] = (gw_t){lo, hi, 0};
            pthread_mutex_lock(&g_gw_mu); gw_add(lo, hi); pthread_mutex_unlock(&g_gw_mu);
        } else if (kind < 34) {
            fence++; gw_seal(fence);
            for (int j = 0; j < nshadow; j++) if (!shadow[j].fence) shadow[j].fence = fence;
        } else if (kind < 37 && done < fence) {
            done += 1 + GW_RAND() % (fence - done); atomic_store(&g_done, done);
        } else if (kind < 39) {
            gw_prune(); int k = 0;
            for (int j = 0; j < nshadow; j++) if (!shadow[j].fence || shadow[j].fence > done) shadow[k++] = shadow[j];
            nshadow = k;
        } else {
            uint64_t lo = UINT64_C(0x340000000) - 4096 + (GW_RAND() % 140000) * 1, hi = lo + 1 + GW_RAND() % 4096;
            uint64_t at = GW_RAND() % 4 ? done : (done ? done - 1 : 0);   /* captures may use an older completion */
            int oracle = 0;
            for (int j = 0; j < nshadow; j++) if (shadow[j].lo < hi && lo < shadow[j].hi && (!shadow[j].fence || shadow[j].fence > at)) { oracle = 1; break; }
            pthread_mutex_lock(&g_gw_mu); int indexed = gw_pending(lo, hi, at); pthread_mutex_unlock(&g_gw_mu);
            errors += indexed != oracle; (*checks)++;
        }
    }
    #undef GW_RAND
    /* Per-query cost at the route's size: ~100 pending writes, 1,308 capture ranges per submission. */
    for (int count = 32; count <= 512; count *= 4) {
        pthread_mutex_lock(&g_gw_mu); g_ngw = 0;
        for (int i = 0; i < count; i++) gw_add(UINT64_C(0x360000000) + (uint64_t)i * 65536 * 3, UINT64_C(0x360000000) + (uint64_t)i * 65536 * 3 + 4096);
        pthread_mutex_unlock(&g_gw_mu);
        uint64_t ns[2]; unsigned hits[2] = {0};
        for (unsigned indexed = 0; indexed < 2; indexed++) {
            uint64_t start = host_now_ns();
            for (unsigned i = 0; i < 200000; i++) {
                uint64_t lo = UINT64_C(0x360000000) + (uint64_t)(i % 4096) * 65536 + 1024;
                if (indexed) hits[1] += gw_pending(lo, lo + 256, 0);
                else { for (int j = 0; j < g_ngw; j++) if (g_gw[j].lo < lo + 256 && lo < g_gw[j].hi) { hits[0]++; break; } }
            }
            ns[indexed] = host_now_ns() - start;
        }
        errors += hits[0] != hits[1]; (*checks)++;
        printf("lag-query GPU writes %d pending: linear %.2f indexed %.2f ns/query; 6540 capture ranges/frame %.3f -> %.3f ms\n",
               count, ns[0] / 2e5, ns[1] / 2e5, ns[0] / 2e5 * 6540 / 1e6, ns[1] / 2e5 * 6540 / 1e6);
    }
    pthread_mutex_lock(&g_gw_mu); g_ngw = 0; pthread_mutex_unlock(&g_gw_mu);
    atomic_store(&g_done, 0);
    free(shadow);
    return errors;
}
static unsigned lag_test_rng_sort(unsigned *checks);
int vk_lag_test_query_bench(void) {
    if (g_nub || g_ub || g_ub_maxhi) return 1; /* run before Vulkan/runtime initialization */
    enum { CALLS = 200000 };
    unsigned checks = 0, errors = 0;
    for (int count = 256; count <= 4096; count *= 16) {
        g_nub = count; g_ub = VK_CALLOC((size_t)count, sizeof *g_ub);
        g_ub_maxhi = VK_CALLOC((size_t)count, sizeof *g_ub_maxhi);
        for (int i = 0; i < count; i++) g_ub[i] = (ub_ent){
            .lo = UINT64_C(0x330000000) + (uint64_t)i * 4096,
            .hi = UINT64_C(0x330000000) + (uint64_t)i * 4096 + (i % 7 ? 256 : 32768),
            .flushed = i % 3 == 0, .fence = i % 5 ? 101 : 0
        };
        ub_reindex();
        for (unsigned done = 100; done <= 101; done++) {
            atomic_store(&g_done, done);
            for (int i = -1; i <= count; i++) for (unsigned k = 0; k < 5; k++) {
                uint64_t a = UINT64_C(0x330000000) + (uint64_t)(i + 1) * 4096 - 4096 + k * 128;
                uint64_t n = k == 4 ? 65536 : 128;
                errors += ub_original_pending(a, n) != lag_test_pending_linear(a, n); checks++;
            }
        }
        atomic_store(&g_done, 100);
        uint64_t ns[2]; unsigned hits[2] = {0};
        for (unsigned indexed = 0; indexed < 2; indexed++) {
            uint64_t start = host_now_ns();
            for (unsigned i = 0; i < CALLS; i++) {
                uint64_t a = UINT64_C(0x330000000) + (uint64_t)((i * 509u) % (unsigned)count) * 4096 + 512;
                hits[indexed] += indexed ? ub_original_pending(a, 256) : lag_test_pending_linear(a, 256);
            }
            ns[indexed] = host_now_ns() - start;
        }
        errors += hits[0] != hits[1]; checks++;
        printf("lag-query pending %d entries: scalar %.2f indexed %.2f ns/query; 20000 queries/frame %.3f -> %.3f ms; hits %u/%u\n",
            count, ns[0] / (double)CALLS, ns[1] / (double)CALLS,
            ns[0] / (double)CALLS * .02, ns[1] / (double)CALLS * .02, hits[0], hits[1]);
        free(g_ub); free(g_ub_maxhi); g_ub = NULL; g_ub_maxhi = NULL; g_nub = 0;
    }
    setenv("TOTK_FRAME_LAG", "1", 1);
    for (unsigned gap = 64; gap <= LAG_PRODUCER_CHANGES; gap *= 8) {
        uint64_t revision = g_lag_producer_revision;
        for (unsigned i = 0; i < gap; i++)
            lag_producer_changed(UINT64_C(0x330000000) + (uint64_t)(i % 32) * 65536, 512);
        for (unsigned i = 0; i < 128; i++) for (unsigned k = 0; k < 4; k++) {
            uint64_t address = UINT64_C(0x330000000) + (uint64_t)i * 32768 + k * 256;
            uint64_t size = k == 3 ? UINT64_C(0x400001) : 256;
            errors += lag_producer_unchanged(revision, address, size) != lag_test_producer_linear(revision, address, size); checks++;
        }
        uint64_t ns[2]; unsigned hits[2] = {0};
        for (unsigned accelerated = 0; accelerated < 2; accelerated++) {
            uint64_t start = host_now_ns();
            for (unsigned i = 0; i < CALLS; i++) {
                uint64_t address = UINT64_C(0x350000000) + (uint64_t)(i % 1024) * 65536;
                hits[accelerated] += accelerated ? lag_producer_unchanged(revision, address, 256) :
                    lag_test_producer_linear(revision, address, 256);
            }
            ns[accelerated] = host_now_ns() - start;
        }
        errors += hits[0] != hits[1]; checks++;
        printf("lag-query journal %u unrelated writes: scalar %.2f spatial %.2f ns/query; 20000 queries/frame %.3f -> %.3f ms\n",
            gap, ns[0] / (double)CALLS, ns[1] / (double)CALLS,
            ns[0] / (double)CALLS * .02, ns[1] / (double)CALLS * .02);
    }
    uint64_t old = g_lag_producer_revision, target = UINT64_C(0x390000000);
    for (unsigned i = 0; i <= LAG_PRODUCER_CHANGES; i++) lag_producer_changed(UINT64_C(0x330000000), 512);
    errors += !lag_producer_unchanged(old, target, 256); checks++; /* independent proof survives exact history loss */
    lag_producer_changed(target + 128, 1);
    errors += lag_producer_unchanged(old, target, 256); checks++;
    old = g_lag_producer_revision;
    lag_producer_changed(target + 65535, 2);
    errors += lag_producer_unchanged(old, target + 65536, 1); checks++; /* both boundary buckets updated */
    old = g_lag_producer_revision;
    lag_producer_changed(target, UINT64_C(0x500000));
    errors += lag_producer_unchanged(old, target + UINT64_C(0x400000), 1); checks++;
    old = g_lag_producer_revision;
    lag_producer_changed(UINT64_MAX - 32, 64);
    errors += lag_producer_unchanged(old, UINT64_MAX - 16, 8); checks++;
    errors += lag_producer_unchanged(old, UINT64_MAX - 16, 32); checks++;
    errors += lag_test_owner_bench(); checks++;
    errors += lag_test_collect_bench(); checks++;
    errors += lag_test_gw_index(&checks);
    errors += lag_test_rng_sort(&checks);
    printf("%s: lag-query %u checks, %u failed\n", errors ? "FAILED" : "ok", checks, errors);
    return errors != 0;
}
#endif
#ifdef TOTK_VK_TESTS
static _Atomic int *g_test_gpu_write_ready, *g_test_gpu_write_resume;
static uint64_t g_test_gpu_write_guards;
void vk_record_test_gpu_write_pause(_Atomic int *ready, _Atomic int *resume) {
    g_test_gpu_write_ready = ready; g_test_gpu_write_resume = resume;
}
uint64_t vk_record_test_gpu_write_guards(void) { return g_test_gpu_write_guards; }
#endif
static void gpu_write_inner(uint64_t a, uint64_t n, int guard) {
    if (!n) return;
    vk_wb_gpu_wrote(a, n);
    int replay=vk_fg_enabled();
    if (ub_private_on() || replay) vk_ub_touch(a, n);
    uint64_t physical = ub_address(a, n);
    if (ub_private_on() || replay) ub_forget(physical, n);
    snap_drop(physical, n);
    if (vk_lag_capture_on() || replay) ub_changed(physical, n);
    lag_producer_changed(physical, n);
#ifdef TOTK_VK_TESTS
    /* Fixture-owned gates remain alive until the remapping thread joins. Pause after canonicalization
       to pin the otherwise narrow donor/generation handoff; production has no hook or counter. */
    if (g_test_gpu_write_ready) {
        atomic_store_explicit(g_test_gpu_write_ready, 1, memory_order_release);
        while (!atomic_load_explicit(g_test_gpu_write_resume, memory_order_acquire)) sched_yield();
    }
#endif
    /* Keep both the selected canonical donor and current alias graph. A remap can complete after
       canonicalization but before this generation load, so generation alone can label the old donor
       with the new graph. New aliases must also revoke guarded=true even when the donor is unchanged. */
    uint64_t spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    { enum { NG = 256 }; static struct { uint64_t a, n, sub, physical, spans; int guarded; } seen[NG];
      uint64_t sub = vk_submit_serial(); unsigned h = (unsigned)((a >> 6) ^ (a >> 20) ^ n) % NG;
      if (seen[h].a == a && seen[h].n == n && seen[h].sub == sub &&
          seen[h].physical == physical && seen[h].spans == spans) {
          if (guard && !seen[h].guarded) {
#ifdef TOTK_VK_TESTS
              g_test_gpu_write_guards++;
#endif
              lag_guard_mark(a, n, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_GPU); seen[h].guarded = 1;
          }
          return;
      }
      seen[h].a = a; seen[h].n = n; seen[h].sub = sub; seen[h].physical = physical;
      seen[h].spans = spans; seen[h].guarded = guard; }
    /* Count guard forwarding rather than eventual registrations: fresh frozen alias projection is
       required here; the existing late submit path still owns ticket arming and its failures. */
#ifdef TOTK_VK_TESTS
    if (guard) g_test_gpu_write_guards++;
#endif
    if (guard) lag_guard_mark(a, n, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_GPU);
    a = physical;
    if (!snap_on() && !vk_lag_capture_on() && !replay) return;
    pthread_mutex_lock(&g_gw_mu);
    gw_add(a, a + n);
    pthread_mutex_unlock(&g_gw_mu);
}
void vk_gpu_write(uint64_t a, uint64_t n) { VK_REC_DO(VK_REC_S_WRITE, gpu_write_inner(a, n, 1)); }
void vk_gpu_write_record(uint64_t a, uint64_t n) { VK_REC_DO(VK_REC_S_WRITE, gpu_write_inner(a, n, 0)); }
typedef struct { uint64_t lo, hi; } rng_t;
enum { UBO_SEEN = 1024 }; /* shared across recursive command lists, allocated once per capture */
static int rng_cmp(const void *x, const void *y) { const rng_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
/* Stable LSD radix sort by lo, 11-bit digits, only over digits whose bits differ between keys (guest addresses share
   their high bits). qsort's indirect comparator was ~0.36 ms/frame of the Thor walker for ~4,500 ranges per capture.
   Distinct lo come out in qsort's order; equal lo keep input order, which the capture merge does not depend on.
   Scratch is per call: asynchronous capture can run on a game thread while the walker captures. */
static void rng_sort(rng_t *r, int n) {
    if (n < 2) return;
    if (n <= 32) {
        for (int i = 1; i < n; i++) { rng_t x = r[i]; int j = i; for (; j > 0 && r[j - 1].lo > x.lo; j--) r[j] = r[j - 1]; r[j] = x; }
        return;
    }
    uint64_t any = 0, all = UINT64_MAX;
    for (int i = 0; i < n; i++) { any |= r[i].lo; all &= r[i].lo; }
    uint64_t vary = any ^ all;
    rng_t *tmp = VK_MALLOC((size_t)n * sizeof *tmp), *src = r, *dst = tmp;
    unsigned count[2048];
    for (unsigned shift = 0; shift < 64; shift += 11) {
        if (!((vary >> shift) & 2047)) continue;
        memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[(src[i].lo >> shift) & 2047]++;
        unsigned sum = 0;
        for (unsigned b = 0; b < 2048; b++) { unsigned c = count[b]; count[b] = sum; sum += c; }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo >> shift) & 2047]++] = src[i];
        rng_t *swap = src; src = dst; dst = swap;
    }
    if (src != r) memcpy(r, src, (size_t)n * sizeof *r);
    free(tmp);
}
#ifdef TOTK_VK_TESTS
/* rng_sort against qsort: identical key order, and a permutation of the input (hi travels with its lo). */
static unsigned lag_test_rng_sort(unsigned *checks) {
    unsigned errors = 0; uint64_t rng = 0x243f6a8885a308d3u;
    rng_t *a = malloc(9000 * sizeof *a), *b = malloc(9000 * sizeof *b);
    if (!a || !b) abort();
    static const int sizes[] = {0, 1, 2, 31, 32, 33, 100, 1308, 4500, 9000};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) for (unsigned shape = 0; shape < 4; shape++) {
        int n = sizes[s];
        for (int i = 0; i < n; i++) {
            rng = rng * 6364136223846793005u + 1442695040888963407u;
            uint64_t x = rng >> 20;
            uint64_t lo = shape == 0 ? UINT64_C(0x330000000) + (x % 65536) * 256        /* clustered guest UBOs */
                        : shape == 1 ? UINT64_C(0x330000000) + (x % 16) * 64            /* many equal keys */
                        : shape == 2 ? x << 20                                          /* only high bits vary */
                        : rng;                                                          /* all bits vary */
            a[i] = b[i] = (rng_t){lo, lo + 1 + (x & 0xfff)};
        }
        rng_sort(a, n); if (n) qsort(b, (size_t)n, sizeof *b, rng_cmp);
        uint64_t sum_a = 0, sum_b = 0;
        for (int i = 0; i < n; i++) {
            errors += a[i].lo != b[i].lo; sum_a += a[i].hi * 31 + a[i].lo; sum_b += b[i].hi * 31 + b[i].lo;
            if (i) errors += a[i - 1].lo > a[i].lo;
        }
        errors += sum_a != sum_b; (*checks)++;
    }
    free(a); free(b);
    return errors;
}
#endif
static void collect_ubos(rng_t **r, int *n, int *cap, const uint8_t *p, int depth, rng_t seen[UBO_SEEN]) {
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) return;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { const uint8_t *t; memcpy(&t, p + 8, 8); if (depth < 16) collect_ubos(r, n, cap, t, depth + 1, seen); }
        else if (h.op == OP_BIND_UBO) {
            const p_bind_buf *b = (const p_bind_buf *)(p + 8);
            if (b->addr && b->size && b->size <= 65536) {
                uint64_t address = ub_address(b->addr, b->size);
                unsigned slot = lag_hash(address, b->size, 0) % UBO_SEEN;
                if (vk_rec_profile_on && T_rec.active) g_rec.snapshot_bindings++;
                if (seen[slot].lo == address && seen[slot].hi == address + b->size) { p += h.size; continue; }
                seen[slot] = (rng_t){address, address + b->size};
                if (vk_rec_profile_on && T_rec.active) g_rec.snapshot_unique++;
                if (*n == *cap) { *cap = *cap ? 2 * *cap : 4096; *r = VK_REALLOC(*r, (size_t)*cap * sizeof **r); }
                (*r)[(*n)++] = (rng_t){address, address + b->size};
            }
        }
        p += h.size;
    }
}
#ifdef TOTK_VK_TESTS
static int lag_test_collect_bench(void) {
    enum { BINDS = 2048, DISTINCT = 16, REPEATS = 16 };
    struct command { op_hdr h; p_bind_buf b; } *stream = calloc(BINDS + 1, sizeof *stream);
    if (!stream) abort();
    for (int i = 0; i < BINDS; i++) stream[i] = (struct command){
        {OP_BIND_UBO, sizeof *stream}, {i & 1, 0, UINT64_C(0x100000000) + (uint64_t)(i % DISTINCT) * 65536, 256}};
    stream[BINDS].h = (op_hdr){OP_END, sizeof *stream};
    rng_t *r = malloc(4096 * sizeof *r), seen[UBO_SEEN]; int capacity = 4096;
    if (!r) abort();
    uint64_t ns[2][3]; unsigned samples[2] = {0}, errors = 0; int candidates[2] = {0};
    for (unsigned pass = 0; pass < 6; pass++) {
        unsigned mode = pass == 1 || pass == 2 || pass == 5;
        int n = 0; uint64_t start = host_now_ns();
        for (unsigned repeat = 0; repeat < REPEATS; repeat++) {
            n = 0; memset(seen, 0, (mode ? UBO_SEEN : 64) * sizeof *seen);
            if (mode) collect_ubos(&r, &n, &capacity, (const uint8_t *)stream, 0, seen);
            else for (int i = 0; i < BINDS; i++) {
                uint64_t a = ub_address(stream[i].b.addr, stream[i].b.size);
                unsigned slot = (unsigned)((a >> 8) ^ (a >> 20) ^ stream[i].b.size) & 63;
                if (seen[slot].lo == a && seen[slot].hi == a + stream[i].b.size) continue;
                r[n++] = seen[slot] = (rng_t){a, a + stream[i].b.size};
            }
            qsort(r, (size_t)n, sizeof *r, rng_cmp);
        }
        ns[mode][samples[mode]++] = host_now_ns() - start; candidates[mode] = n;
        int distinct = 0;
        for (int i = 0; i < n; i++) {
            errors += r[i].hi != r[i].lo + 256;
            if (i && r[i].lo == r[i - 1].lo) continue;
            errors += r[i].lo != UINT64_C(0x100000000) + (uint64_t)distinct * 65536; distinct++;
        }
        errors += distinct != DISTINCT;
    }
    for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned i = 1; i < 3; i++) for (unsigned j = i; j && ns[mode][j] < ns[mode][j - 1]; j--) {
            uint64_t swap = ns[mode][j]; ns[mode][j] = ns[mode][j - 1]; ns[mode][j - 1] = swap;
        }
    errors += candidates[1] >= candidates[0];
    printf("lag-query UBO collect/sort: %d repeated binds, %d distinct; candidates %d -> %d; "
           "old %.2f compact %.2f ns/bind (interleaved medians); identical canonical extents\n",
           BINDS, DISTINCT, candidates[0], candidates[1],
           ns[0][1] / (double)(BINDS * REPEATS), ns[1][1] / (double)(BINDS * REPEATS));
    free(r); free(stream); return errors != 0;
}
#endif
static ucap_t *ucap_build(const uint64_t *h, int n, int copy_bytes) {
    ucap_t *c = VK_CALLOC(1, sizeof *c);
    c->done = atomic_load(&g_done);   /* before the bytes: a write completing meanwhile only makes this conservative */
    rng_t *r = NULL, seen[UBO_SEEN] = {0}; int nr = 0, cap = 0;
    for (int i = 0; i < n; i++) collect_ubos(&r, &nr, &cap, (const uint8_t *)(uintptr_t)h[i], 0, seen);
    if (nr) {
        rng_sort(r, nr);
        int k = -1;   /* merge overlapping / touching ranges inside one pool; drop ranges outside every pool */
        for (int i = 0; i < nr; i++) {
            VkBuffer b; uint64_t o;
            if (k >= 0 && r[i].lo <= r[k].hi) { if (r[i].hi <= r[k].hi) continue; if (vk_resolve(r[k].lo, r[i].hi - r[k].lo, &b, &o)) { r[k].hi = r[i].hi; continue; } }
            if (!vk_resolve(r[i].lo, r[i].hi - r[i].lo, &b, &o)) continue;
            if (k >= 0 && r[i].lo < r[k].hi) r[i].lo = r[k].hi;   /* keep the copies disjoint */
            r[++k] = r[i];
        }
        /* ranges a GPU write still in flight overlaps are left to memory (the GPU timeline) */
        pthread_mutex_lock(&g_gw_mu);
        int m = 0; for (int i = 0; i <= k; i++) {
            if ((g_ngw && gw_pending(r[i].lo, r[i].hi, c->done)) ||
                (ub_private_on() && ub_original_pending(r[i].lo, r[i].hi - r[i].lo))) {
                g_stat_snap_skipped++; continue;
            }
            r[m++] = r[i];
        }
        pthread_mutex_unlock(&g_gw_mu);
        k = m - 1;
        c->n = k + 1;
        c->lo = VK_MALLOC(sizeof *c->lo * (size_t)(c->n + 1)); c->hi = VK_MALLOC(sizeof *c->hi * (size_t)(c->n + 1));
        if (copy_bytes) c->off = VK_MALLOC(sizeof *c->off * (size_t)(c->n + 1));
        size_t total = 0; for (int i = 0; i < c->n; i++) {
            c->lo[i] = r[i].lo; c->hi[i] = r[i].hi;
            if (copy_bytes) { c->off[i] = total; total += (size_t)(r[i].hi - r[i].lo); }
        }
        if (copy_bytes) {
            c->bytes = VK_MALLOC(total ? total : 1);
            for (int i = 0; i < c->n; i++) memcpy(c->bytes + c->off[i], (const void *)(uintptr_t)c->lo[i], (size_t)(c->hi[i] - c->lo[i]));
        }
    }
    free(r);
    return c;
}
static void ucap_free(ucap_t *c) { if (c) { free(c->lo); free(c->hi); free(c->off); free(c->bytes); free(c); } }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { return snap_on() ? ucap_build(h, n, 1) : NULL; } /* async: eagerly own the bytes */
static int snap_first(uint64_t a) { int lo = 0, hi = g_nsnap; while (lo < hi) { int m = (lo + hi) >> 1; if (g_snap[m].hi <= a) lo = m + 1; else hi = m; } return lo; }
static const snap_t *snap_find(uint64_t a, uint64_t n) {   /* the live copy holding all of [a, a+n) */
    int i = snap_first(a);
    return i < g_nsnap && !g_snap[i].dead && g_snap[i].lo <= a && a + n <= g_snap[i].hi ? &g_snap[i] : NULL;
}
/* A snapshot already contains all updates logged at its creation. Only a later overlapping update can make a binding
   need an overlay: mark once at the update instead of searching the whole log at every binding of untouched snapshots.
   The flag stays conservative after chunk submissions seal / prune those updates. */
static void snap_note_update(uint64_t lo, uint64_t n) {
    for (int i = snap_first(lo); i < g_nsnap && g_snap[i].lo < lo + n; i++) g_snap[i].updated = 1;
}
/* updated is only a cheap rejection hint. A merged snapshot can contain thousands of untouched bindings after
   one tiny update. The retained ordered log proves the requested subrange, including partial overlays; the
   snapshot prune floor keeps every post-base update available until that immutable incarnation is reset. */
static int snap_range_updated(const snap_t *sn, uint64_t address, uint64_t size) {
    uint64_t end = address + size;
    if (!sn->updated || !g_nub || end <= g_ub_lo || address >= g_ub_hi) return 0;
    int a = 0, b = g_nub;
    while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < end) a = m + 1; else b = m; }
    uint64_t floor = address > g_ub_maxlen ? address - g_ub_maxlen : 0;
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > address && (g_ub[i].lo > floor || !floor); i--)
        if (g_ub[i].hi > address && g_ub[i].seq > g_snap_seq) return 1;
    return 0;
}
static void snap_drop(uint64_t a, uint64_t n) {   /* a GPU / CPU-step write into [a, a+n): those copies no longer hold the truth */
    for (int i = snap_first(a); i < g_nsnap && g_snap[i].lo < a + n; i++) if (!g_snap[i].dead) {
        g_snap[i].dead = 1; ub_changed(g_snap[i].lo, g_snap[i].hi - g_snap[i].lo);
    }
}
/* The copies are bound for the whole game submission, which chunked submission (vk_submit_chunk) spreads over several ring
   slots, and the ring can wrap within one: a slot's transient memory would be reused while later parts still read it. So
   they live in an arena of their own, reused once the submission's last part has finished (one queue: all parts have). */
#define SNAP_ARENAS 4
static struct { arena_t ar; uint64_t release; } g_sar[SNAP_ARENAS];
static uint64_t g_sar_replay_until[SNAP_ARENAS]; /* external split sidecar */
static int g_sar_cur = -1, g_sar_next;
static unsigned snapshot_replay_mask(void) {
    unsigned mask=0;
    for (unsigned i=0;i<SNAP_ARENAS;i++) if (g_sar[i].ar.nchunk) mask|=1u<<i;
    return mask;
}
static int arena_frozen_copy(arena_t *arena,VkBuffer buffer,VkDeviceSize offset,
        VkDeviceSize bytes,void *destination) {
    for (int i=0;i<arena->nchunk;i++) {
        const chunk_t *chunk=&arena->chunks[i];
        if (chunk->buf!=buffer || offset>chunk->used || bytes>chunk->used-offset) continue;
        memcpy(destination,chunk->ptr+offset,(size_t)bytes);
        return 1;
    }
    return 0;
}
int vk_replay_frozen_buffer_copy(VkBuffer buffer,VkDeviceSize offset,VkDeviceSize bytes,
        void *destination) {
    if (!buffer || !destination || !bytes || offset>UINT64_MAX-bytes || bytes>SIZE_MAX) return 0;
    for (unsigned i=0;i<RING;i++)
        if (arena_frozen_copy(&g_ring[i].ar,buffer,offset,bytes,destination)) return 1;
    for (unsigned i=0;i<SNAP_ARENAS;i++)
        if (arena_frozen_copy(&g_sar[i].ar,buffer,offset,bytes,destination)) return 1;
    return 0;
}
static uint64_t snapshot_owner_until(unsigned arena) {
    uint64_t until=0;
    if (!atomic_load_explicit(&g_replay_pins,memory_order_relaxed)) return 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i=0;i<RING;i++)
        if ((g_replay_owner[i].snapshots & (1u<<arena)) && g_replay_owner[i].until>until)
            until=g_replay_owner[i].until;
    pthread_mutex_unlock(&g_mu);
    return until;
}
static void snapshot_replay_pin(uint64_t until) {
    for (unsigned i=0;i<SNAP_ARENAS;i++) {
        if (until==UINT64_MAX) {
            if (g_sar[i].ar.nchunk) g_sar_replay_until[i]=until;
        } else if (g_sar_replay_until[i]==UINT64_MAX) g_sar_replay_until[i]=until;
    }
}
static arena_t *snap_arena(void) {   /* the current submission's arena (one more replay start in it appends) */
    if (g_sar_cur < 0) {
        int i = g_sar_next; g_sar_next = (i + 1) % SNAP_ARENAS;
        uint64_t release=g_sar[i].release, owner=snapshot_owner_until((unsigned)i);
        if (owner>release) release=owner;
        if (atomic_load_explicit(&g_external_enabled,memory_order_acquire) && g_sar_replay_until[i]>release)
            release=g_sar_replay_until[i];
        if (release==UINT64_MAX &&
                __builtin_expect(atomic_load_explicit(&vk_fg_generation_snapshot_retained,memory_order_relaxed),0)) {
            /* This recorder alone can resolve open/pending replay leases.
             * Cancel optional templates, never submitted/quarantined work.
             * Earlier cancellation may already have emptied every template;
             * quarantine may outlive the presenter queue/gate altogether. */
            unsigned cancelled=vk_fg_generation_cancel_prepared(),states=0;
            pthread_mutex_lock(&g_mu);
            for (unsigned n=0;n<RING;n++)
                if ((g_replay_owner[n].snapshots&(1u<<i)) && g_replay_owner[n].until==UINT64_MAX)
                    states|=g_replay_owner[n].pending?1u:2u;
            pthread_mutex_unlock(&g_mu);
            owner=snapshot_owner_until((unsigned)i);
            release=g_sar[i].release;
            if (owner>release) release=owner;
            if (atomic_load_explicit(&g_external_enabled,memory_order_acquire) && g_sar_replay_until[i]>release)
                release=g_sar_replay_until[i];
            vk_fg_snapshot_pressure_trace(states,vk_fg_generation_state_mask(),cancelled,
                release==UINT64_MAX);
            static int reported;
            if (!reported) {
                reported=1;
                LOGV("[adaptive-slot-dispatch] event=snapshot-open-owner wait-ns=%llu arena=%d owner-state-mask=%u generation-state-mask=%u cancelled=%u after-ns=%llu; pending=1 open=2 append-not-recycle-if-still-open",
                    (unsigned long long)UINT64_MAX,i,states,vk_fg_generation_state_mask(),
                    cancelled,(unsigned long long)release);
            }
            if (release==UINT64_MAX) {
                /* Do not split from descriptor capture: its caller can cache
                 * that primary. Append without reset/compaction until normal
                 * sealing resolves pending release. Quarantine keeps immutable
                 * prefixes indefinitely, even after scheduler disable. */
                g_sar_cur=i;
                return &g_sar[i].ar;
            }
        }
        wait_value(release); g_sar_replay_until[i]=0;
        g_sar[i].ar.min = 1u << 20;
        arena_reset(&g_sar[i].ar, 2);
        g_sar_cur = i;
    }
    return &g_sar[g_sar_cur].ar;
}
#ifdef TOTK_VK_TESTS
uint64_t vk_record_test_snapshot_next_owner(void) {
    return snapshot_owner_until((unsigned)g_sar_next);
}
void vk_record_test_snapshot_allocate_range(uint32_t value,VkBuffer *buffer,VkDeviceSize *offset) {
    uint32_t *bytes=arena_alloc(snap_arena(),64,16,buffer,offset);
    for (unsigned i=0;i<16;i++) bytes[i]=value+i;
}
unsigned vk_record_test_snapshot_mask(void) { return snapshot_replay_mask(); }
uint64_t vk_record_test_snapshot_external_until(void) {
    uint64_t until=0;
    for (unsigned i=0;i<SNAP_ARENAS;i++)
        if (g_sar_replay_until[i]>until) until=g_sar_replay_until[i];
    return until;
}
unsigned vk_record_test_snapshot_next_index(void) { return (unsigned)g_sar_next; }
uint32_t *vk_record_test_snapshot_allocate(uint32_t value) {
    VkBuffer buffer; VkDeviceSize offset;
    uint32_t *bytes=arena_alloc(snap_arena(),64,16,&buffer,&offset);
    for (unsigned i=0;i<16;i++) bytes[i]=value+i;
    return bytes;
}
#endif
static void snapshot_ubos_inner(const uint64_t *h, int n, void *capv) {   /* the start of a replay */
    ucap_t *cap = capv;
    snap_reset();
    if (!snap_on()) { ucap_free(cap); return; }
    if (!cap) {
        /* Avoid a staging copy only if acquiring the replay-owned snapshot arena cannot wait for the GPU.
           Async capture and an unavailable arena retain eager capture before any wait. */
        int direct = vk_record_enabled() && (g_sar_cur >= 0 || atomic_load(&g_done) >= g_sar[g_sar_next].release);
        if (direct && g_sar_cur<0 && atomic_load_explicit(&g_external_enabled,memory_order_acquire))
            direct=atomic_load(&g_done)>=g_sar_replay_until[g_sar_next];
        if (direct && g_sar_cur<0)
            direct=atomic_load(&g_done)>=snapshot_owner_until((unsigned)g_sar_next);
        cap = ucap_build(h, n, !direct);
    }
    g_prune_floor = cap->done;
    if (cap->n > g_capsnap) { g_capsnap = cap->n + 256; g_snap = VK_REALLOC(g_snap, (size_t)g_capsnap * sizeof *g_snap); }
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 256;
    static int *idx; static int icap;
    for (int i = 0; i < cap->n; i++) {
        uint64_t lo = cap->lo[i], hi = cap->hi[i], sz = hi - lo, shift = lo % align;
        VkBuffer b; VkDeviceSize o; uint8_t *p = arena_alloc(snap_arena(), (size_t)(sz + shift), align, &b, &o);
        if (vk_lag_capture_on()) lag_snapshot_allocation(VK_LAG_UNIFORM, sz + shift);
        else ft_snapshot_allocation(sz + shift);
        p += shift; o += shift;   /* the copy keeps the guest address's offset within the alignment: aligned bindings stay aligned */
        memcpy(p, cap->bytes ? cap->bytes + cap->off[i] : (const void *)(uintptr_t)lo, (size_t)sz);
        if (vk_rec_profile_on && T_rec.active) {
            if (cap->bytes) g_rec.snapshot_staged += sz; else g_rec.snapshot_direct += sz;
        }
        /* inline updates of earlier submissions that were not in memory at the capture */
        int m; for (;;) { m = ub_overlaps(lo, hi, 0, idx, icap); if (m >= 0) break; icap = icap ? 2 * icap : 64; idx = VK_REALLOC(idx, (size_t)icap * sizeof *idx); }
        for (int k = 0; k < m; k++) {
            const ub_ent *e = &g_ub[idx[k]];
            if (e->fence && e->fence <= cap->done) continue;
            uint64_t a = e->lo > lo ? e->lo : lo, z = e->hi < hi ? e->hi : hi;
            memcpy(p + (a - lo), e->data + (a - e->lo), (size_t)(z - a));
        }
        g_snap[g_nsnap++] = (snap_t){lo, hi, b, o, p, 0, 0};
        g_stat_snap_bytes += sz;
    }
    g_stat_snap_ranges += (uint64_t)g_nsnap;
    ucap_free(cap);
}
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *capv) { VK_REC_DO(VK_REC_S_SNAPSHOT, snapshot_ubos_inner(h, n, capv)); }
static int ub_rename_slow(uint64_t addr, uint64_t size, const snap_t *sn, VkDescriptorBufferInfo *out);
int vk_ub_rename(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!ub_rename_on()) return 0;   /* ordinary renaming stays opt-in; protected UBOs use vk_lag_uniform_read */
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL;
    /* Memo (TOTK, 2026-10-22: the overlap search was ~10% of the CPU, for ~15k uniform bindings a frame against ~100
       logged updates): with the same range, snapshot and submission and no change to the log since, the answer is the
       same as last time. */
    enum { NM = 1024 };
    static struct { uint64_t addr, size, gen, sub; const void *sn; int ret; VkDescriptorBufferInfo d; } memo[NM];
    uint64_t msub = vk_submit_serial(); unsigned mh = (unsigned)((addr >> 4) ^ (addr >> 14) ^ (size * 2654435761u)) % NM;
    if (memo[mh].addr == addr && memo[mh].size == size && memo[mh].gen == g_ub_gen && memo[mh].sub == msub && memo[mh].sn == (const void *)sn) { if (memo[mh].ret) *out = memo[mh].d; return memo[mh].ret; }
    int ret = ub_rename_slow(addr, size, sn, out);
    memo[mh].addr = addr; memo[mh].size = size; memo[mh].gen = g_ub_gen; memo[mh].sub = msub; memo[mh].sn = sn; memo[mh].ret = ret; if (ret) memo[mh].d = *out;
    return ret;
}
static int ub_rename_slow(uint64_t addr, uint64_t size, const snap_t *sn, VkDescriptorBufferInfo *out) {
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 256;
    if (sn && !snap_range_updated(sn, addr, size)) {
        VkDeviceSize o = sn->off + (addr - sn->lo);
        if (o % align == 0) { *out = (VkDescriptorBufferInfo){sn->buf, o, size}; g_stat_snap_binds++; return 1; }
    }
    int n = 0;
    static int *idx; static int cap;
    if (g_nub && addr + size > g_ub_lo && addr < g_ub_hi) {
        { static uint64_t last_done = UINT64_MAX; uint64_t d = atomic_load(&g_done); if (d != last_done) { last_done = d; ub_prune(); } }   /* prune only when the GPU moved on */
        for (;;) { n = ub_overlaps(addr, addr + size, 0, idx, cap); if (n >= 0) break; cap = cap ? 2 * cap : 64; idx = VK_REALLOC(idx, (size_t)cap * sizeof *idx); }
        /* A chunk split seals updates from this game submission too: fence != 0 does not mean the snapshot has them.
           Keep every update newer than its base, even after it has reached an earlier Vulkan submission. */
        if (sn) { int k = 0; for (int i = 0; i < n; i++) if (g_ub[idx[i]].seq > g_snap_seq) idx[k++] = idx[i]; n = k; }
    }
    if (!n) {
        if (!sn) return 0;
        VkDeviceSize o = sn->off + (addr - sn->lo);
        if (o % align == 0) { *out = (VkDescriptorBufferInfo){sn->buf, o, size}; g_stat_snap_binds++; return 1; }
    }
    /* the same range with the same newest update in this submission: the copy made for it is still right */
    enum { NC = 256 };
    static struct { uint64_t addr, size, seq, sub; const void *base; VkDescriptorBufferInfo d; } cache[NC];
    uint64_t newest = n ? g_ub[idx[n - 1]].seq : 0, sub = vk_submit_serial();
    unsigned h = (unsigned)((addr >> 4) ^ (addr >> 16) ^ size) % NC;
    if (cache[h].addr == addr && cache[h].size == size && cache[h].seq == newest && cache[h].sub == sub && cache[h].base == (const void *)sn) { *out = cache[h].d; return 1; }
    VkBuffer tb; VkDeviceSize to;
    uint8_t *p = vk_transient(size, align, &tb, &to);
    ft_snapshot_allocation(size);
    memcpy(p, sn ? (const void *)(sn->cpu + (addr - sn->lo)) : (const void *)(uintptr_t)addr, (size_t)size);
    for (int k = 0; k < n; k++) {
        const ub_ent *e = &g_ub[idx[k]];
        uint64_t lo = e->lo > addr ? e->lo : addr, hi = e->hi < addr + size ? e->hi : addr + size;
        memcpy(p + (lo - addr), e->data + (lo - e->lo), (size_t)(hi - lo));
    }
    *out = (VkDescriptorBufferInfo){tb, to, size};
    cache[h].addr = addr; cache[h].size = size; cache[h].seq = newest; cache[h].sub = sub; cache[h].base = sn; cache[h].d = *out;
    g_stat_ub_renamed++;
    return 1;
}
/* A covering submission snapshot proves the range exists: avoid re-resolving its guest pool on every descriptor.
   Only an overlapping post-base update requires the ordinary ordered overlay/rename memo. */
int vk_ub_snapshot_descriptor(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL; if (!sn) return 0;
    VkDeviceSize offset = sn->off + addr - sn->lo, align = V.props.limits.minUniformBufferOffsetAlignment;
    if (!align) align = 256;
    if (!snap_range_updated(sn, addr, size) && !(offset % align)) { *out = (VkDescriptorBufferInfo){sn->buf, offset, size}; g_stat_snap_binds++; return 1; }
    return vk_ub_rename(addr, size, out);
}
/* The bytes a uniform binding at this point of the submission reads (the same base and overlay as vk_ub_rename's copy).
   For values the backend itself takes from a uniform block: a storage buffer's address (RB_SBUF_UBO) set by an inline
   update reaches memory only when the GPU runs the update, so memory still held an earlier draw's address (nvn_mtl.m
   resolves the same pointer with pend_read). */
static void ub_read_from(uint64_t addr, void *dst, size_t size, const snap_t *sn) {
    if (sn) {
        memcpy(dst, sn->cpu + (addr - sn->lo), size);
        if (!snap_range_updated(sn, addr, size)) return;
    }
    if (!g_nub || addr + size <= g_ub_lo || addr >= g_ub_hi) {
        if (!sn) memcpy(dst, (const void *)(uintptr_t)addr, size);
        return;
    }
    static int *idx; static int cap; int n;
    ub_prune();
    for (;;) { n = ub_overlaps(addr, addr + size, 0, idx, cap); if (n >= 0) break; cap = cap ? 2 * cap : 64; idx = VK_REALLOC(idx, (size_t)cap * sizeof *idx); }
    if (!sn) {
        /* Direct updates may still be writing the original pool on the GPU. CPU-known log bytes are
           authoritative: read only uncovered native gaps, not a racing base that is overwritten below. */
        uint64_t cursor = addr, end = addr + size;
        while (cursor < end) {
            uint64_t covered = cursor, next = end;
            for (int k = 0; k < n; k++) {
                const ub_ent *e = &g_ub[idx[k]];
                if (e->lo <= cursor && e->hi > covered) covered = e->hi;
                else if (e->lo > cursor && e->lo < next) next = e->lo;
            }
            if (covered > cursor) cursor = covered < end ? covered : end;
            else {
                memcpy((uint8_t *)dst + (cursor - addr), (const void *)(uintptr_t)cursor, (size_t)(next - cursor));
                cursor = next;
            }
        }
    }
    for (int k = 0; k < n; k++) {
        const ub_ent *e = &g_ub[idx[k]];
        if (sn && e->seq <= g_snap_seq) continue;   /* only updates already included in the snapshot's base */
        uint64_t lo = e->lo > addr ? e->lo : addr, hi = e->hi < addr + size ? e->hi : addr + size;
        memcpy((uint8_t *)dst + (lo - addr), e->data + (lo - e->lo), (size_t)(hi - lo));
    }
}
/* Frame lag exposes every direct read of CPU-written pool memory, not just UBO descriptors. Freeze only the ranges
   actually consumed, on the walking thread before submit returns. GPU-produced ranges must not be replaced with stale
   CPU bytes: leave them on the queue timeline. Inline updates use exactly the uniform snapshot/overlay semantics.
   The submission arena (not the chunk/slot arena) keeps copies alive across chunk submissions and recorder jobs. */
int vk_lag_capture_init(void) {
    int current = atomic_load_explicit(&vk_lag_capture_state, memory_order_acquire);
    if (current >= 0) return current;
    const char *capture = getenv("TOTK_VK_LAG_CAPTURE");
    int on = runtime_frame_lag_requested() && !(capture && !atoi(capture)), expected = -1;
    /* A delayed initializer must not overwrite a completed pressure cutover or another initializer. */
    if (!atomic_compare_exchange_strong_explicit(&vk_lag_capture_state, &expected, on,
                                                 memory_order_acq_rel, memory_order_acquire)) return expected;
    if (on) LOGV("lag protection: read-only guest ranges protected before submit returns (TOTK_VK_LAG_CAPTURE=0 disables)");
    return on;
}
static int lag_wait_on(void) {
    if (!vk_lag_capture_on()) return 0;
    if (g_lag_wait < 0) {
        const char *mode = getenv("TOTK_VK_LAG_MODE");
        if (mode && strcmp(mode, "wait") && strcmp(mode, "copy")) DIE("TOTK_VK_LAG_MODE must be wait or copy");
        g_lag_wait = !(mode && !strcmp(mode, "copy"));
        LOGV("lag protection mode: %s (TOTK_VK_LAG_MODE)", g_lag_wait ? "wait on actual CPU reuse" : "eager copy");
    }
    return g_lag_wait;
}
static int lag_hybrid_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_LAG_HYBRID"); on = lag_wait_on() && !(e && !atoi(e)); }
    return on;
}
static uint64_t lag_hot_limit(void) {
    static uint64_t bytes;
    if (!bytes) { const char *e = getenv("TOTK_VK_LAG_HOT_MAX_KB"); unsigned long n = e ? strtoul(e, NULL, 10) : 256; bytes = n && n <= 1048576 ? (uint64_t)n * 1024 : 256u * 1024; }
    return bytes;
}
typedef struct { uint64_t lo, hi; unsigned reason; uint64_t max_range; } lag_pending;
static int lag_pending_cmp(const void *x, const void *y) {
    const lag_pending *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}
static lag_pending *g_lag_pending; static int g_lag_npending, g_lag_cpending;
static uint64_t g_lag_epoch = 1;
static _Atomic uint64_t g_lag_guard_bytes, g_lag_guard_ns, g_lag_guard_ranges;
static uint64_t g_lag_bytes, g_lag_ns, g_lag_ranges, g_lag_gpu;
static _Atomic uint64_t g_lag_failed_ticket[RING], g_lag_record_bytes, g_lag_record_ranges;
/* At most RING ticket owners exist, including the current walking slot. A cell is not reused until that
   slot's jobs/registration and GPU fence retire. A later ticket cannot hide an earlier failed projection. */
static void lag_guard_failed(uint64_t ticket) {
    if (ticket) atomic_store_explicit(&g_lag_failed_ticket[ticket % RING], ticket, memory_order_relaxed);
}
static int lag_guard_has_failed(uint64_t ticket) {
    return atomic_load_explicit(&g_lag_failed_ticket[ticket % RING], memory_order_relaxed) == ticket;
}
static int lag_guard_take_failed(uint64_t ticket) {
    return atomic_compare_exchange_strong_explicit(&g_lag_failed_ticket[ticket % RING], &ticket, 0,
                                                    memory_order_relaxed, memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
int vk_record_test_take_failure(uint64_t ticket) { return lag_guard_take_failed(ticket); }
#endif
void vk_record_lag_fail(uint64_t ticket) { lag_guard_failed(ticket); }
uint64_t vk_record_lag_ticket(void) { return lag_wait_on() ? g_reserved_fence + 1 : 0; }
int vk_record_lag_enabled(void) { return g_lag_wait == 1 && vk_lag_capture_on(); }
/* Recording freezes resolved owner/view/alias VA intents without arming them. Only the separate submit
   registration phase may publish their exact ticket; it performs no span/map or snapshot lookup.
   Lavapipe still needs complete producer exclusions because it stores through guest host VAs. */
typedef struct { uint64_t lo, hi, ticket, max_range; uint32_t reason; } lag_record_pending;
static lag_record_pending *g_lag_record_pending;
static size_t g_lag_record_count, g_lag_record_capacity;
static pthread_mutex_t g_lag_record_mu = PTHREAD_MUTEX_INITIALIZER;
#ifdef TOTK_VK_TESTS
static int g_record_test_native_guards;
void vk_record_test_native_guards(int enabled) { g_record_test_native_guards = enabled; }
#endif
static int lag_software_gpu(void) {
#ifdef TOTK_VK_TESTS
    if (g_record_test_native_guards) return 0;
#endif
    return V.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}
int vk_record_lag_native(void) { return !lag_software_gpu(); }
static int lag_record_defer(const vk_record_lag_intent *intents, uint32_t count, uint64_t ticket) {
    int ok = 1;
    pthread_mutex_lock(&g_lag_record_mu);
    size_t end = g_lag_record_count + count;
    if (end < g_lag_record_count || end > SIZE_MAX / sizeof *g_lag_record_pending) ok = 0;
    if (ok && end > g_lag_record_capacity) {
        size_t cap = g_lag_record_capacity ? g_lag_record_capacity : 256;
        while (cap < end && cap <= SIZE_MAX / 2 / sizeof *g_lag_record_pending) cap *= 2;
        if (cap < end) cap = end;
        lag_record_pending *p = realloc(g_lag_record_pending, cap * sizeof *p);
        if (p) { g_lag_record_pending = p; g_lag_record_capacity = cap; } else ok = 0;
    }
    if (ok) for (uint32_t i = 0; i < count; i++) {
        const vk_record_lag_intent *r = &intents[i];
        g_lag_record_pending[g_lag_record_count++] = (lag_record_pending){r->lo, r->hi, ticket, r->max_range, r->reason};
    }
    pthread_mutex_unlock(&g_lag_record_mu);
    return ok;
}
typedef struct {
    nvn_dirty_lag_batch *batch;
    vk_record_lag_intent **out; uint32_t *count, *capacity;
    uint64_t projected, epoch;
} lag_projection;
static int lag_project_span(const span_t *s, VkBuffer buffer, uint64_t offset, uint64_t size,
                            uint32_t reason, lag_projection *p, int *covered) {
    if (s->buf != buffer || s->end <= s->lo || s->off + s->end - s->lo < s->off) return 1;
    uint64_t lo = offset > s->off ? offset : s->off, end = s->off + s->end - s->lo;
    uint64_t hi = offset + size < end ? offset + size : end;
    if (lo >= hi) return 1;
    if (lo == offset && hi == offset + size) *covered = 1;
    uint64_t address = s->lo + lo - s->off;
    p->projected++;
    if (p->batch)
        return nvn_dirty_lag_batch_add_before(p->batch, (const void *)(uintptr_t)address, hi - lo,
                                              reason | NVN_DIRTY_LAG_ALIAS, size, p->epoch);
    if (*p->count == *p->capacity) {
        size_t cap = *p->capacity ? (size_t)*p->capacity * 2 : 32;
        vk_record_lag_intent *v = cap <= UINT32_MAX && cap <= SIZE_MAX / sizeof *v ?
            realloc(*p->out, cap * sizeof *v) : NULL;
        if (!v) return 0;
        *p->out = v; *p->capacity = (uint32_t)cap;
    }
    (*p->out)[(*p->count)++] = (vk_record_lag_intent){address, address + hi - lo, size, reason | NVN_DIRTY_LAG_ALIAS};
    return 1;
}
static int lag_project_indexed(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                               uint32_t reason, lag_projection *p) {
    if (!size) return 1;
    if (!spans || !buffer || offset + size < offset || (spans->count && !spans->buffer_heads)) return 0;
    int covered = 0;
    p->epoch = spans->dirty_epoch; /* a recycled alias cannot become the identity of an older frozen packet */
    /* Each bucket retains every physical owner, view and alias, independently of VA lookup winners and OFF=span. */
    uint32_t at = spans->count ? spans->buffer_heads[record_buffer_hash(buffer) & spans->buffer_mask] : 0;
    while (at) {
        uint32_t i = at - 1; at = spans->buffer_next[i];
        if (!lag_project_span(&spans->span[i], buffer, offset, size, reason, p, &covered)) return 0;
    }
    return covered;   /* union-only coverage was never sufficient: a resolved source span must cover the whole input */
}
int vk_record_lag_collect(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                          uint64_t ticket, uint32_t reason, nvn_dirty_lag_batch *batch,
                          vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity, uint64_t *projected) {
    if (!vk_record_lag_enabled() || !size) return 1;
    lag_projection p = {.batch = batch, .out = out, .count = count, .capacity = capacity};
    int ok = ticket && lag_project_indexed(spans, buffer, offset, size, reason, &p);
    if (projected) *projected += p.projected;
    if (!ok) lag_guard_failed(ticket);
    return ok;
}
int vk_record_lag_project(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                          uint64_t ticket, uint32_t reason, vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity) {
    return vk_record_lag_collect(spans, buffer, offset, size, ticket, reason, NULL, out, count, capacity, NULL);
}
void vk_record_lag_register(const vk_record_lag_intent *intents, uint32_t count, uint64_t ticket) {
    /* Only software-GPU callers retain VA vectors. Native callers collect and globally merge units before arming. */
    if (!lag_record_defer(intents, count, ticket)) lag_guard_failed(ticket);
}
void vk_record_lag_batch_register(const nvn_dirty_lag_batch *batch, uint64_t ticket, unsigned shard, unsigned nshards) {
    if (!nvn_dirty_lag_batch_register(batch, ticket, shard, nshards)) lag_guard_failed(ticket);
    if (!shard) {   /* aggregate once per immutable ticket batch, never atomics or clocks per unit */
        atomic_fetch_add_explicit(&g_lag_record_bytes, nvn_dirty_lag_batch_bytes(batch), memory_order_relaxed);
        atomic_fetch_add_explicit(&g_lag_record_ranges, nvn_dirty_lag_batch_count(batch), memory_order_relaxed);
    }
}
#ifdef TOTK_VK_TESTS
/* Synthetic host-only projection fixtures need neither a Vulkan device nor a dirty-tracking mapping. */
void vk_record_test_spans_destroy(vk_record_spans *s) {
    if (!s) return;
    free(s->span); free(s->segment); free(s->buffer_heads); free(s);
}
vk_record_spans *vk_record_test_spans_create(const vk_record_test_span *input, uint32_t count, int linear) {
    if (count > INT32_MAX) return NULL;
    vk_record_spans *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->count = (int)count; s->linear = linear;
    if (count) {
        s->span = malloc((size_t)count * sizeof *s->span);
        if (!s->span) { vk_record_test_spans_destroy(s); return NULL; }
        for (uint32_t i = 0; i < count; i++)
            s->span[i] = (span_t){input[i].lo, input[i].hi, input[i].buffer, input[i].offset};
    }
    if (!record_spans_index(s)) { vk_record_test_spans_destroy(s); return NULL; }
    return s;
}
int vk_record_test_project(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                            uint32_t reason, int reference, vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity) {
    lag_projection p = {.out = out, .count = count, .capacity = capacity};
    if (!reference) return lag_project_indexed(spans, buffer, offset, size, reason, &p);
    /* The old complete scan exists only in tests. Keep its exact coverage and partial-overlap semantics as an oracle. */
    if (!size) return 1;
    if (!spans || !buffer || offset + size < offset) return 0;
    int covered = 0;
    for (int i = 0; i < spans->count; i++)
        if (!lag_project_span(&spans->span[i], buffer, offset, size, reason, &p, &covered)) return 0;
    return covered;
}
#endif
/* Per-binding clocks, including copy and sampled classification timing, are opt-in.
   Chunk registration remains timed; byte/count diagnostics never require a clock. */
static int lag_timing_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_LAG_TIMING"); on = e && atoi(e); }
    return on;
}
static void lag_guard_mark(uint64_t addr, uint64_t size, unsigned reason) {
    if (!lag_wait_on() || !addr || !size || addr + size < addr) return;
    if (!lag_software_gpu() && vk_record_enabled()) {
        VkBuffer buffer; uint64_t offset;
        if (vk_resolve(addr, size, &buffer, &offset)) vk_record_lag_guard(buffer, offset, size, reason);
        return;   /* frozen guard-only jobs also cover primary copies, uploads and direct uniform writes */
    }
    if (!nvn_dirty_tracked((const void *)(uintptr_t)addr, (size_t)size)) {
        VkBuffer b; uint64_t off;
        if (vk_resolve(addr, size, &b, &off)) { lag_guard_failed(g_reserved_fence + 1); RED(4, "lag guard: untracked writable GPU range forces its submission to complete"); }
        return;
    }
    enum { NG = 4096 };
    static struct { uint64_t addr, size, epoch; unsigned reason; } marked[NG];
    unsigned h = lag_hash(addr, size, 0) % NG;
    if (marked[h].addr == addr && marked[h].size == size && marked[h].epoch == g_lag_epoch &&
        (marked[h].reason & reason) == reason) return;
    uint64_t start = lag_timing_on() ? host_now_ns() : 0;
    if (g_lag_npending == g_lag_cpending) {
        g_lag_cpending = g_lag_cpending ? g_lag_cpending * 2 : 256;
        g_lag_pending = VK_REALLOC(g_lag_pending, (size_t)g_lag_cpending * sizeof *g_lag_pending);
    }
    g_lag_pending[g_lag_npending++] = (lag_pending){addr, addr + size, reason, size};
    if (marked[h].addr == addr && marked[h].size == size && marked[h].epoch == g_lag_epoch) marked[h].reason |= reason;
    else { marked[h].addr = addr; marked[h].size = size; marked[h].epoch = g_lag_epoch; marked[h].reason = reason; }
    if (start) g_lag_guard_ns += host_now_ns() - start;
}
/* The alias graph changes only when pool mappings change. Reuse its exact range projection instead of scanning every
   virtual mapping for every frame's merged guards. Cached arrays are walking-thread-owned, like the guard list. */
typedef struct { uint64_t lo, hi, gen; rng_t *r; int n, cap; } lag_alias_cache;
static const rng_t *lag_alias_ranges(rng_t r, int *count) {
    enum { NA = 2048 };
    static lag_alias_cache cache[NA];
    unsigned h = (unsigned)((r.lo >> 12) ^ (r.lo >> 28) ^ (r.hi >> 6)) % NA;
    lag_alias_cache *c = &cache[h];
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (c->lo == r.lo && c->hi == r.hi && c->gen == gen) { *count = c->n; return c->r; }
    pthread_mutex_lock(&g_pool_mu);
    c->n = 0;
    for (uint64_t addr = r.lo; addr < r.hi; ) {
        VkBuffer b; uint64_t off, avail;
        if (!resolve_span(addr, 1, &b, &off, &avail)) { addr += page_sz(); continue; }
        uint64_t len = r.hi - addr; if (len > avail) len = avail;
        for (int i = 0; i < g_nalias; i++) {
            const alias_ent *a = &g_alias[i];
            if (a->owner->buf != b) continue;
            uint64_t lo = off > a->off ? off : a->off, end = a->off + a->end - a->va;
            uint64_t hi = off + len < end ? off + len : end;
            if (lo < hi) {
                if (c->n + 2 > c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->r = VK_REALLOC(c->r, (size_t)c->cap * sizeof *c->r); }
                c->r[c->n++] = (rng_t){a->va + lo - a->off, a->va + hi - a->off};
                if (addr >= a->va && addr < a->end && a->off + addr - a->va == off)
                    c->r[c->n++] = (rng_t){a->owner->base + off, a->owner->base + off + len};
            }
        }
        addr += len;
    }
    c->lo = r.lo; c->hi = r.hi; c->gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    *count = c->n; return c->r;
}
static int lag_software_excluded(uint64_t lo, uint64_t hi);
static void lag_guard_aliases(rng_t r, uint64_t fence, unsigned reason, uint64_t max_range) {
    int n; const rng_t *aliases = lag_alias_ranges(r, &n);
    for (int i = 0; i < n; i++) {
        if (lag_software_gpu()) {
            uintptr_t lo, hi;
            if (nvn_dirty_lag_bounds((void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo, &lo, &hi) &&
                lag_software_excluded(lo, hi)) continue;
        }
        if (nvn_dirty_lag_registered((void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo, fence)) continue;
        totk_ft_count(FT_COUNT_GUARD_REGS, 1);
        if (!nvn_dirty_lag_register((void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo,
                                   fence, reason | NVN_DIRTY_LAG_ALIAS, max_range)) {
            lag_guard_failed(fence);
            RED(4, "lag guard: pool alias untracked/busy or re-arm failed; its submission must complete before reuse");
        }
    }
}
/* Most bindings already name their physical pool. A virtual-pool binding learns from the physical CPU writer too;
   cache buffer->owner instead of scanning all virtual mappings for every cold, read-only range each frame. */
static uint64_t lag_physical_window(uint64_t addr, uint64_t size, uint64_t *lo, uint64_t *hi) {
    enum { NP = 4096 };
    static struct { uint64_t lo, hi, gen, physical; } ranges[NP];
    /* Fixed windows are hash/containment boundaries, never dirty-unit identities. Elementary segments clip
       them: a containing pool can lose to another mapping partway through its address span. */
    unsigned slot = lag_hash(addr & ~UINT64_C(65535), 0, 0) % NP;
    __typeof__(ranges[0]) *r = &ranges[slot];
    uint64_t spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (r->gen == spans && addr >= r->lo && addr < r->hi && size <= r->hi - addr) {
        *lo = r->physical; *hi = r->physical + r->hi - r->lo;
        return r->physical + addr - r->lo;
    }
    *lo = *hi = 0;
    VkBuffer b; uint64_t off, avail, physical = addr;
    if (vk_resolve_span(addr, &b, &off, &avail) && size <= avail) {
        uint64_t base = lag_owner_base(b);
        if (base) {
            physical = base + off;
            *lo = physical; *hi = physical + size; /* the full request resolved even across a breakpoint */
            if (!span_linear() && T_span.gen == spans) {
                unsigned hint = (unsigned)((addr >> 12) ^ (addr >> 24)) & (SPAN_HINTS - 1);
                int at = T_span.hint[hint];
                if (span_hint_matches(T_span.seg, T_span.nseg, addr, at)) {
                    uint64_t a = addr & ~UINT64_C(65535), z = a + 65536;
                    if (a < T_span.seg[at - 1].lo) a = T_span.seg[at - 1].lo;
                    if (z > T_span.seg[at].lo) z = T_span.seg[at].lo;
                    *r = (__typeof__(*r)){a, z, spans, physical - (addr - a)};
                    if (size <= z - addr) { *lo = r->physical; *hi = r->physical + z - a; }
                }
            }
        }
    }
    return physical;
}
static uint64_t lag_physical(uint64_t addr, uint64_t size) {
    uint64_t lo, hi;
    return lag_physical_window(addr, size, &lo, &hi);
}
static int lag_range_hot(uint64_t addr, uint64_t size) {
    if (!lag_hybrid_on() || size > lag_hot_limit()) return 0;
    int hot = nvn_dirty_lag_hot((const void *)(uintptr_t)addr, size);
    if (!hot) {
        uint64_t physical = lag_physical(addr, size);
        hot = physical != addr && nvn_dirty_lag_hot((const void *)(uintptr_t)physical, size);
    }
    return hot;
}
/* Protect every known CPU writer, not only the descriptor VA. The global dirty token is a content token only
   while all writer aliases remain watched. Failure/untracked input permits a copy, but never reusable authority.
   Ticket zero is the existing write-watch operation, not registration of a future GPU read. */
static int lag_watch_copy(uint64_t addr, uint64_t size) {
    int watched = nvn_dirty_lag_protect((const void *)(uintptr_t)addr, size, 0);
    int n; const rng_t *aliases = lag_alias_ranges((rng_t){addr, addr + size}, &n);
    for (int i = 0; i < n; i++)
        watched &= nvn_dirty_lag_protect((const void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo, 0);
    return watched;
}
static uint64_t g_lag_copy_unwatched;
uint64_t vk_lag_copy_unwatched(void) { return g_lag_copy_unwatched; }
/* Walk-only software exclusions, assembled after joining workers. A producer discovered after an earlier packet
   must still exclude the whole dirty unit, including every frozen projected VA, before any future ticket is armed. */
static rng_t *g_lag_software_exclusions;
static size_t g_lag_software_count, g_lag_software_capacity;
static int lag_software_exclude(uint64_t lo, uint64_t hi) {
    uintptr_t a, b;
    if (!nvn_dirty_lag_bounds((const void *)(uintptr_t)lo, hi - lo, &a, &b)) return 1;
    if (g_lag_software_count == g_lag_software_capacity) {
        size_t cap = g_lag_software_capacity ? 2 * g_lag_software_capacity : 256;
        rng_t *p = cap > g_lag_software_capacity && cap <= SIZE_MAX / sizeof *p ?
            realloc(g_lag_software_exclusions, cap * sizeof *p) : NULL;
        if (!p) return 0;
        g_lag_software_exclusions = p; g_lag_software_capacity = cap;
    }
    g_lag_software_exclusions[g_lag_software_count++] = (rng_t){a, b};
    return 1;
}
static int lag_software_exclude_aliases(uint64_t lo, uint64_t hi) {
    if (!lag_software_exclude(lo, hi)) return 0;
    int count; const rng_t *aliases = lag_alias_ranges((rng_t){lo, hi}, &count);
    for (int i = 0; i < count; i++) if (!lag_software_exclude(aliases[i].lo, aliases[i].hi)) return 0;
    return 1;
}
static int lag_software_build_exclusions(uint64_t done) {
    g_lag_software_count = 0;
    int ok = 1;
    pthread_mutex_lock(&g_gw_mu);
    for (int i = 0; ok && i < g_ngw; i++)
        if (!g_gw[i].fence || g_gw[i].fence > done) ok = lag_software_exclude_aliases(g_gw[i].lo, g_gw[i].hi);
    pthread_mutex_unlock(&g_gw_mu);
    for (int i = 0; ok && i < g_nub; i++)
        if (g_ub[i].flushed && (!g_ub[i].fence || g_ub[i].fence > done)) ok = lag_software_exclude_aliases(g_ub[i].lo, g_ub[i].hi);
    for (size_t i = 0; ok && i < g_lag_record_count; i++) {
        const lag_record_pending *r = &g_lag_record_pending[i];
        if (r->reason & (NVN_DIRTY_LAG_GPU | NVN_DIRTY_LAG_WRITABLE)) ok = lag_software_exclude(r->lo, r->hi);
    }
    if (!ok) return 0;   /* no software registration may proceed with incomplete exclusions */
    if (g_lag_software_count > 1)
        qsort(g_lag_software_exclusions, g_lag_software_count, sizeof *g_lag_software_exclusions, rng_cmp);
    size_t count = 0;
    for (size_t i = 0; i < g_lag_software_count; i++) {
        rng_t r = g_lag_software_exclusions[i];
        if (count && r.lo <= g_lag_software_exclusions[count - 1].hi) {
            if (r.hi > g_lag_software_exclusions[count - 1].hi) g_lag_software_exclusions[count - 1].hi = r.hi;
        } else g_lag_software_exclusions[count++] = r;
    }
    g_lag_software_count = count;
    return 1;
}
static int lag_software_excluded(uint64_t lo, uint64_t hi) {
    size_t a = 0, b = g_lag_software_count;
    while (a < b) { size_t m = a + (b - a) / 2; if (g_lag_software_exclusions[m].hi <= lo) a = m + 1; else b = m; }
    return a < g_lag_software_count && g_lag_software_exclusions[a].lo < hi;
}
static uint64_t g_lag_hot_bytes, g_lag_hot_ranges;
/* Walking-only projection; no protection/ticket is armed until the sealed owner joins all recording jobs. */
static void lag_guard_seal(unsigned slot, uint64_t fence) {
    uint64_t start = host_now_ns(); nvn_dirty_lag_batch *walking = &g_ring[slot].walking_guards;
    static uint64_t sampled_spans;
    uint64_t generation = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (generation != sampled_spans) { nvn_dirty_lag_vma_request(0); sampled_spans = generation; }
    nvn_dirty_lag_batch_reset(walking);
    for (int i = 0; i < g_lag_npending; i++) {
        const lag_pending *r = &g_lag_pending[i]; uintptr_t lo, hi;
        if (!nvn_dirty_lag_bounds((const void *)(uintptr_t)r->lo, r->hi - r->lo, &lo, &hi)) continue;
        if (!nvn_dirty_lag_batch_add(walking, (const void *)(uintptr_t)lo, hi - lo, r->reason, r->max_range))
            lag_guard_failed(fence);
        int count; const rng_t *aliases = lag_alias_ranges((rng_t){lo, hi}, &count);
        for (int a = 0; a < count; a++)
            if (!nvn_dirty_lag_batch_add(walking, (const void *)(uintptr_t)aliases[a].lo,
                                        aliases[a].hi - aliases[a].lo, r->reason | NVN_DIRTY_LAG_ALIAS, r->max_range))
                lag_guard_failed(fence);
    }
    g_lag_npending = 0; g_lag_epoch++;
    g_lag_guard_ns += host_now_ns() - start;
}
/* Sole sealed-slot owner: explicit identities, immutable units, and no walking lock/map/log accesses. */
static int lag_guard_submit_native(unsigned slot, uint64_t fence, int background) {
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("submit-registration"); uint64_t start = host_now_ns();
    nvn_dirty_lag_batch *walking = &g_ring[slot].walking_guards;
    vk_record_lag_submit(slot, fence, walking, background);
    if (!vk_record_enabled()) {
        if (!nvn_dirty_lag_batch_seal(walking)) lag_guard_failed(fence);
        vk_record_lag_batch_register(walking, fence, 0, 1);
        if (background) {
            if (g_ring[slot].ft_frame)
                totk_ft_count_frame(g_ring[slot].ft_frame, FT_COUNT_GUARD_REGS, nvn_dirty_lag_batch_count(walking));
        } else if (totk_ft_work_frame())
            totk_ft_count(FT_COUNT_GUARD_REGS, nvn_dirty_lag_batch_count(walking));
    }
    g_lag_guard_bytes += atomic_exchange_explicit(&g_lag_record_bytes, 0, memory_order_relaxed);
    g_lag_guard_ranges += atomic_exchange_explicit(&g_lag_record_ranges, 0, memory_order_relaxed);
    g_lag_guard_ns += host_now_ns() - start;
    if (nvn_dirty_lag_vma_pressure()) lag_guard_failed(fence);
    int failed = lag_guard_take_failed(fence);
    nvn_dirty_lag_thread_role(previous_role); return failed;
}
/* Software Vulkan can write through host VAs. Its producer exclusions still require the complete live walk;
   this path deliberately remains synchronous rather than arming a CPU driver's own destination pages. */
static int lag_guard_submit(unsigned slot, uint64_t fence) {
    if (vk_record_lag_native()) return lag_guard_submit_native(slot, fence, 0);
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("submit-registration");
    uint64_t start = host_now_ns(), done = atomic_load(&g_done); int nr = 0;
    vk_record_lag_submit(slot, fence, NULL, 0);
    if (nvn_dirty_lag_vma_pressure()) lag_guard_failed(fence);
    int protect = !lag_guard_has_failed(fence) && lag_software_build_exclusions(done);
    if (!protect) lag_guard_failed(fence);
    if (g_lag_npending > 1) qsort(g_lag_pending, (size_t)g_lag_npending, sizeof *g_lag_pending, lag_pending_cmp);
    for (int i = 0; protect && i < g_lag_npending; i++) {
        lag_pending r = g_lag_pending[i]; uintptr_t lo, hi;
        if (!nvn_dirty_lag_bounds((const void *)(uintptr_t)r.lo, r.hi - r.lo, &lo, &hi)) continue;
        r.lo = lo; r.hi = hi;
        if (lag_software_excluded(r.lo, r.hi)) { g_lag_gpu++; continue; }
        if (nr && r.lo <= g_lag_pending[nr - 1].hi &&
            nvn_dirty_tracked((void *)(uintptr_t)g_lag_pending[nr - 1].lo,
                             (r.hi > g_lag_pending[nr - 1].hi ? r.hi : g_lag_pending[nr - 1].hi) - g_lag_pending[nr - 1].lo)) {
            lag_pending *prev = &g_lag_pending[nr - 1];
            if (r.hi > prev->hi) prev->hi = r.hi;
            prev->reason |= r.reason;
            if (r.max_range > prev->max_range) prev->max_range = r.max_range;
        } else g_lag_pending[nr++] = r;
    }
    for (int i = 0; i < nr; i++) {
        lag_pending r = g_lag_pending[i];
        totk_ft_count(FT_COUNT_GUARD_REGS, 1);
        if (nvn_dirty_lag_register((void *)(uintptr_t)r.lo, r.hi - r.lo, fence, r.reason, r.max_range)) {
            lag_guard_aliases((rng_t){r.lo, r.hi}, fence, r.reason, r.max_range);
            g_lag_guard_bytes += r.hi - r.lo; g_lag_guard_ranges++;
        } else {
            lag_guard_failed(fence);
            RED(4, "lag guard: unit busy or re-arm failed; its submission must complete before reuse");
        }
    }
    for (size_t i = 0; protect && i < g_lag_record_count; i++) {
        const lag_record_pending *r = &g_lag_record_pending[i]; uintptr_t lo, hi;
        if (nvn_dirty_lag_bounds((const void *)(uintptr_t)r->lo, r->hi - r->lo, &lo, &hi) &&
            lag_software_excluded(lo, hi)) { g_lag_gpu++; continue; }
        totk_ft_count(FT_COUNT_GUARD_REGS, 1);
        if (!nvn_dirty_lag_register((const void *)(uintptr_t)r->lo, r->hi - r->lo, r->ticket, r->reason, r->max_range))
            lag_guard_failed(r->ticket);
        else { g_lag_guard_bytes += r->hi - r->lo; g_lag_guard_ranges++; }
    }
    g_lag_record_count = 0; g_lag_npending = 0; g_lag_epoch++;
    g_lag_guard_bytes += atomic_exchange_explicit(&g_lag_record_bytes, 0, memory_order_relaxed);
    g_lag_guard_ranges += atomic_exchange_explicit(&g_lag_record_ranges, 0, memory_order_relaxed);
    g_lag_guard_ns += host_now_ns() - start;
    int failed = lag_guard_take_failed(fence);
    nvn_dirty_lag_thread_role(previous_role); return failed;
}
/* Mapping/tracking metadata is independent of queue tickets and snapshot arenas. Byte authority is not:
   every actual ticket or snapshot incarnation change revokes captures, even when the metadata remains a hit.
   Native captures additionally require every writer watch and dirty token; owned snapshots ignore native CPU reuse. */
typedef struct {
    uint64_t raw, size, align, submission, ticket, spans, lifetime, snapshot, physical, revision, dirty;
    uint64_t map_lo, map_hi, unit_lo, unit_hi, hot_frame, hot_dirty;
    VkDescriptorBufferInfo descriptor;
    int tracked, hot, captured, immutable;
} lag_binding_memo;
static int lag_binding_key(lag_binding_memo *m, uint64_t addr, uint64_t size, uint64_t align, vk_lag_kind_stats *s) {
    uint64_t submission = vk_submit_serial(), ticket = g_reserved_fence + 1;
    uint64_t spans = atomic_load_explicit(&g_span_gen, memory_order_acquire), lifetime = nvn_dirty_lag_epoch();
    int range = m->raw == addr && m->size == size && m->align == align;
    int chunk = m->submission == submission && m->ticket == ticket;
    int mapping = m->spans == spans && m->lifetime == lifetime;
    int snapshot = m->snapshot == g_snap_generation;
    /* Exact warm keys change neither metadata nor byte authority. Avoid zero-increment counter stores
       and rewriting three unchanged lifetime fields; still count every observable binding hit. */
    if (range && mapping && chunk && snapshot) { s->binding_hits++; return 1; }
    s->key_ticket += !chunk; s->key_snapshot += !snapshot;
    if (range && mapping) {
        s->binding_hits++; s->metadata_cross_ticket += !chunk; s->metadata_cross_snapshot += !snapshot;
        s->copy_ticket_misses += m->captured && !chunk && m->immutable != 1;
        s->copy_snapshot_misses += m->captured && !snapshot;
        /* Owned snapshots and their overlay copies live through the GAME submission's final fence, not
           an intermediate Vulkan chunk. Native captures retain the stricter actual-ticket lifetime. */
        if (!snapshot || (!chunk && m->immutable != 1)) { m->captured = 0; m->immutable = 0; }
        m->submission = submission; m->ticket = ticket; m->snapshot = g_snap_generation;
        return 1;
    }
    s->binding_misses++; s->key_range += !range; s->key_mapping += !mapping;
    *m = (lag_binding_memo){.raw = addr, .size = size, .align = align, .submission = submission,
        .ticket = ticket, .spans = spans, .lifetime = lifetime, .snapshot = g_snap_generation, .physical = addr};
    if (ub_private_on()) m->physical = lag_physical_window(addr, size, &m->map_lo, &m->map_hi);
    return 0;
}
static int lag_binding_copy(lag_binding_memo *m, VkDescriptorBufferInfo *out, vk_lag_kind_stats *s) {
    if (!m->captured) { s->copy_misses++; return 0; }
    if ((!m->immutable && m->dirty != nvn_dirty_now()) || !ub_range_unchanged(m->revision, m->physical, m->size)) {
        s->copy_misses++; s->copy_content_misses++; return 0;
    }
    s->copy_hits++; m->revision = g_ub_gen; *out = m->descriptor;
    return 1;
}
/* Units retain geometry across tickets, never watch authority or bytes. Hot history is refreshed against both
   real present frames (quiet demotion) and CPU dirty history (promotion), independently of queue submissions. */
typedef struct {
    uint64_t lo, hi, unit_lo, unit_hi, spans, lifetime, ticket, hot_frame, hot_dirty;
    int hot;
} lag_unit_memo;
static lag_unit_memo *lag_classify(lag_binding_memo *m, int same, vk_lag_kind_stats *s, vk_lag_kind kind) {
    enum { NU = 4096 };
    static lag_unit_memo units[NU];
    uint64_t addr = m->physical, size = m->size, window = addr & ~UINT64_C(65535);
    uint64_t frame = nvn_dirty_lag_frame_now(), dirty = nvn_dirty_now();
    /* The common exact cold binding already owns valid tracking metadata. With unchanged frame/history,
       avoid a second hash/table lookup just to recover the same conservative guarded decision. */
    if (same && !m->hot && m->hot_frame == frame && m->hot_dirty == dirty) return NULL;
    lag_unit_memo *u = &units[lag_hash(window, 0, 0) % NU];
    int mapped = m->map_hi > m->map_lo;
    if (mapped && u->spans == m->spans && u->lifetime == m->lifetime &&
        addr >= u->lo && addr < u->hi && size <= u->hi - addr &&
        m->map_lo <= u->lo && m->map_hi >= u->hi) {
        if (!same) { s->class_hits++; s->memo_hits++; }
        s->unit_cross_ticket += u->ticket != m->ticket; u->ticket = m->ticket;
        if (u->hot_frame != frame || u->hot_dirty != dirty) {
            u->hot = lag_hybrid_on() && nvn_dirty_lag_hot((const void *)(uintptr_t)addr, 1);
            u->hot_frame = frame; u->hot_dirty = dirty; s->hot_refreshes++;
        }
        m->tracked = 1; m->hot = u->hot && size <= lag_hot_limit();
        m->unit_lo = u->unit_lo; m->unit_hi = u->unit_hi; m->hot_frame = frame; m->hot_dirty = dirty;
        return u;
    }
    if (same) {
        if (m->hot_frame != frame || m->hot_dirty != dirty) {
            m->hot = m->tracked && lag_range_hot(addr, size);
            m->hot_frame = frame; m->hot_dirty = dirty; s->hot_refreshes++;
        }
        return NULL; /* retained exact tracking survives unit eviction; no watch may be inferred from it */
    }
    s->class_misses++; s->memo_misses++;
    ft_lag_count(kind, 1);
    if (!lag_wait_on()) return NULL;
    uintptr_t lo, hi;
    m->hot_frame = frame; m->hot_dirty = dirty; s->hot_refreshes++;
    int bounds = nvn_dirty_lag_bounds((const void *)(uintptr_t)addr, 1, &lo, &hi);
    if (bounds && size <= hi - addr) {
        m->tracked = 1; m->unit_lo = lo; m->unit_hi = hi;
        if (mapped) {
            uint64_t a = lo > m->map_lo ? lo : m->map_lo, z = hi < m->map_hi ? hi : m->map_hi;
            if (a < window) a = window;
            if (z > window + 65536) z = window + 65536;
            if (addr >= a && addr < z && size <= z - addr) {
                *u = (lag_unit_memo){.lo = a, .hi = z, .unit_lo = lo, .unit_hi = hi,
                    .spans = m->spans, .lifetime = m->lifetime, .ticket = m->ticket,
                    .hot_frame = frame, .hot_dirty = dirty,
                    .hot = lag_hybrid_on() && nvn_dirty_lag_hot((const void *)(uintptr_t)addr, 1)};
                m->hot = u->hot && size <= lag_hot_limit();
                return u;
            }
        }
        m->hot = lag_range_hot(addr, size);
    } else {
        m->tracked = nvn_dirty_tracked((const void *)(uintptr_t)addr, (size_t)size);
        m->hot = m->tracked && lag_range_hot(addr, size);
        if (m->tracked && nvn_dirty_lag_bounds((const void *)(uintptr_t)addr, size, &lo, &hi)) {
            m->unit_lo = lo; m->unit_hi = hi;
        }
    }
    return NULL;
}
static int lag_binding_pending(const lag_binding_memo *m, int whole_unit) {
    if (m->unit_hi > m->unit_lo) {
        int pending = lag_original_pending(m->unit_lo, m->unit_hi - m->unit_lo);
        if (!pending || whole_unit) return pending;
    }
    return lag_original_pending(m->physical, m->size);
}
static uint32_t lag_record_read_reason(const lag_binding_memo *m, uint32_t reason) {
    return reason | (lag_binding_pending(m, lag_software_gpu()) ? NVN_DIRTY_LAG_GPU : 0);
}
static int lag_watch_unit(const lag_unit_memo *u, uint64_t addr, uint64_t size) {
    /* Reuse only projection geometry. Every new native capture still acquires ALL writer gates, including BUSY
       synchronization; dirty_now alone cannot certify a watch across intentional GPU completion stores. */
    return u ? lag_watch_copy(u->lo, u->hi - u->lo) : lag_watch_copy(addr, size);
}
static int lag_read_select(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                           uint32_t *record_reason, vk_lag_kind kind) {
    if (!align) align = 16;
    vk_lag_kind_stats *s = &g_lag_kind[kind];
    enum { NL = 4096 };
    static lag_binding_memo memo[NL];
    lag_binding_memo *m = &memo[lag_hash(addr, size, align) % NL];
    int same = lag_binding_key(m, addr, size, align, s);
    if (same) s->memo_hits++;
    lag_unit_memo *u = lag_classify(m, same, s, kind);
    addr = m->physical;
    s->hot_decisions += m->hot;
    if (m->tracked && !m->hot) {
        unsigned reason = NVN_DIRTY_LAG_READONLY | (size > lag_hot_limit() ? NVN_DIRTY_LAG_OVER_CAP : NVN_DIRTY_LAG_COLD);
        s->cold_guards++;
        if (same) LAG_TEST_INC(VK_LAG_TEST_COLD_HITS);
        if (record_reason) *record_reason = lag_record_read_reason(m, reason);
        else lag_guard_mark(addr, size, reason);
        return 0; /* every live descriptor still registers its full actual range/aliases at its actual ticket */
    }
    if (lag_binding_copy(m, out, s)) { LAG_TEST_INC(VK_LAG_TEST_COPY_HITS); return 1; }
    uint64_t t = lag_timing_on() ? host_now_ns() : 0;
    int gpu = lag_binding_pending(m, 0), captured = 0;
    VkBuffer b; uint64_t o;
    m->captured = 0;
    if (!gpu && (m->map_hi > m->map_lo || vk_resolve(addr, size, &b, &o))) {
        /* Sample before arming any writer projection: an early alias may fault while later aliases are armed. */
        uint64_t dirty = nvn_dirty_now();
        int watched = lag_watch_unit(u, addr, size);
        uint8_t *p = arena_alloc(snap_arena(), (size_t)size, align, &b, &o);
        lag_snapshot_allocation(kind, size);
        vk_ub_read(addr, p, (size_t)size);
        watched &= dirty == nvn_dirty_now();
        *out = (VkDescriptorBufferInfo){b, o, size};
        m->captured = watched; m->dirty = dirty; m->descriptor = *out; m->revision = g_ub_gen;
        if (!watched) g_lag_copy_unwatched++;
        g_lag_bytes += size; g_lag_ranges++; captured = 1;
    } else if (gpu) g_lag_gpu++;
    uint64_t elapsed = t ? host_now_ns() - t : 0;
    if (captured && m->hot) {
        g_lag_hot_bytes += size; g_lag_hot_ranges++;
        s->hot_copies++; s->hot_bytes += size; s->hot_ns += elapsed;
        totk_ft_count(FT_COUNT_HOT_COPIES, 1);
        totk_ft_count(FT_COUNT_HOT_BYTES, size);
    } else if (!captured) {
        if (record_reason) *record_reason = lag_record_read_reason(m, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_GPU);
        else if (m->tracked) lag_guard_mark(addr, size, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_GPU);
    }
    g_lag_ns += elapsed;
    return captured;
}
/* ---------- adaptive lag protection (TOTK_VK_LAG_ADAPTIVE=K, K >= 2; unset/0 = full protection) ----------
   Full protection decides every binding (~26k/frame on the Thor route, ~7 ms of walking), yet CPU writes that race
   in-flight GPU reads concentrate in a few stable units. Vertex/index/storage bindings outside the learned danger set
   (nvn_dirty_lag_danger_*, expanded to every pool alias) skip the decision and bind original memory, exactly as with
   capture off. Each frame still fully protects a rotating 1/K of 64 KiB windows (no periodic slow frame), so every
   window is guarded at least every K frames and the sticky guards keep reporting writes; a newly dangerous unit is
   learned within about K frames, but its racing writes before that are unprotected. Uniform bindings stay fully
   protected (capture also changes where inline uniform updates live). A set larger than the arrays falls back to
   full protection. Wait mode only. Walking thread only. */
static unsigned lag_adaptive_period(void) {
    static int period = -1;
    if (period < 0) {
        const char *e = getenv("TOTK_VK_LAG_ADAPTIVE");
        int requested = e ? atoi(e) : 0;
        period = requested >= 2 && lag_wait_on() ? requested : 0;
        if (e) LOGV("lag adaptive protection: %s (TOTK_VK_LAG_ADAPTIVE=%s)", period ? "on, rotating 1/K windows" :
                    requested >= 2 ? "off: requires TOTK_VK_LAG_MODE=wait" : "off: K must be at least 2", e);
    }
    return (unsigned)period;
}
enum { LAG_DANGER_RUNS = 2048, LAG_DANGER_RANGES = 8192 };
static struct {
    rng_t r[LAG_DANGER_RANGES];
    int n, runs, overflow;
    uint64_t marks, spans, bytes, rebuilds, rebuild_ns;
    uint64_t skips, set_hits, rotation, overflowed;   /* decisions, cumulative */
} g_ad;
static int lag_rng_cmp(const void *x, const void *y) {
    uint64_t a = ((const rng_t *)x)->lo, b = ((const rng_t *)y)->lo;
    return (a > b) - (a < b);
}
static void lag_danger_rebuild(uint64_t marks, uint64_t spans) {
    static uintptr_t lo[LAG_DANGER_RUNS], hi[LAG_DANGER_RUNS];
    uint64_t start = host_now_ns(), bytes = 0;
    int runs = nvn_dirty_lag_danger_collect(lo, hi, LAG_DANGER_RUNS), n = 0, overflow = runs > LAG_DANGER_RUNS;
    for (int i = 0; !overflow && i < runs; i++) {
        bytes += hi[i] - lo[i];
        int count; const rng_t *aliases = lag_alias_ranges((rng_t){lo[i], hi[i]}, &count);
        if (n + 1 + count > LAG_DANGER_RANGES) { overflow = 1; break; }
        g_ad.r[n++] = (rng_t){lo[i], hi[i]};
        for (int k = 0; k < count; k++) g_ad.r[n++] = aliases[k];
    }
    if (!overflow) {
        qsort(g_ad.r, (size_t)n, sizeof *g_ad.r, lag_rng_cmp);
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (m && g_ad.r[i].lo <= g_ad.r[m - 1].hi) { if (g_ad.r[i].hi > g_ad.r[m - 1].hi) g_ad.r[m - 1].hi = g_ad.r[i].hi; }
            else g_ad.r[m++] = g_ad.r[i];
        }
        n = m;
    }
    g_ad.n = overflow ? 0 : n; g_ad.overflow = overflow; g_ad.runs = runs; g_ad.bytes = bytes;
    g_ad.marks = marks; g_ad.spans = spans; g_ad.rebuilds++; g_ad.rebuild_ns += host_now_ns() - start;
}
static int lag_adaptive_skip(uint64_t addr, uint64_t size) {
    unsigned period = lag_adaptive_period();
    if (!period) return 0;
    if (lag_hash(addr & ~UINT64_C(65535), 0, 0) % period == nvn_dirty_lag_frame_now() % period) { g_ad.rotation++; return 0; }
    uint64_t marks = nvn_dirty_lag_danger_marks(), spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (!g_ad.rebuilds || marks != g_ad.marks || spans != g_ad.spans) lag_danger_rebuild(marks, spans);
    if (g_ad.overflow) { g_ad.overflowed++; return 0; }
    int lo = 0, hi = g_ad.n;   /* first range ending after addr */
    while (lo < hi) { int mid = (lo + hi) >> 1; if (g_ad.r[mid].hi <= addr) lo = mid + 1; else hi = mid; }
    if (lo < g_ad.n && g_ad.r[lo].lo < addr + size) { g_ad.set_hits++; return 0; }
    g_ad.skips++;
    return 1;
}
static int lag_read_inner(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                          uint32_t *record_reason, vk_lag_kind kind) {
    if (record_reason) *record_reason = 0;
    if (!vk_lag_capture_on() || !addr || !size || addr + size < addr) return 0;
    if ((kind == VK_LAG_VERTEX || kind == VK_LAG_INDEX || kind == VK_LAG_STORAGE) && lag_adaptive_skip(addr, size)) return 0;
    uint64_t start = lag_decision_begin(kind, size);
    int captured = lag_read_select(addr, size, align, out, record_reason, kind);
    lag_decision_end(kind, start);
    return captured;
}
int vk_lag_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out, vk_lag_kind kind) {
    return lag_read_inner(addr, size, align, out, NULL, kind);
}
int vk_lag_read_record(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                       uint32_t *reason, vk_lag_kind kind) {
    if (!vk_record_lag_enabled()) { *reason = 0; return lag_read_inner(addr, size, align, out, NULL, kind); }
    return lag_read_inner(addr, size, align, out, reason, kind);
}
void vk_lag_capture_take(uint64_t *bytes, uint64_t *ns, uint64_t *ranges, uint64_t *gpu_owned) {
    *bytes = g_lag_bytes; *ns = g_lag_ns; *ranges = g_lag_ranges; *gpu_owned = g_lag_gpu;
    g_lag_bytes = g_lag_ns = g_lag_ranges = g_lag_gpu = 0;
}
void vk_lag_stats(uint64_t frame) {
    if (!vk_lag_capture_on()) return;
    vk_rec_lock();
    static uint64_t last;
    uint64_t n = frame - last, bytes, ns, ranges, gpu; last = frame;
    vk_lag_capture_take(&bytes, &ns, &ranges, &gpu);
    uint64_t guard_bytes = atomic_exchange_explicit(&g_lag_guard_bytes, 0, memory_order_relaxed);
    uint64_t guard_ns = atomic_exchange_explicit(&g_lag_guard_ns, 0, memory_order_relaxed);
    uint64_t guard_ranges = atomic_exchange_explicit(&g_lag_guard_ranges, 0, memory_order_relaxed);
    static uint64_t last_faults, last_host, last_ns;
    uint64_t faults, hosts, wait_ns; nvn_dirty_lag_stats(&faults, &hosts, &wait_ns);
    uint64_t df = faults - last_faults, dh = hosts - last_host, dn = wait_ns - last_ns;
    last_faults = faults; last_host = hosts; last_ns = wait_ns;
    uint64_t hot_bytes = g_lag_hot_bytes, hot_ranges = g_lag_hot_ranges;
    g_lag_hot_bytes = g_lag_hot_ranges = 0;
    static uint64_t old_rearms, old_updates, old_promoted, old_demoted;
    uint64_t rearms, updates, promoted, demoted;
    nvn_dirty_lag_sticky_stats(&rearms, &updates, &promoted, &demoted);
    uint64_t dr = rearms - old_rearms, du = updates - old_updates, dp = promoted - old_promoted, dd = demoted - old_demoted;
    old_rearms = rearms; old_updates = updates; old_promoted = promoted; old_demoted = demoted;
    uint64_t index_ns = g_record_index_ns, index_versions = g_record_index_versions;
    g_record_index_ns = g_record_index_versions = 0;
    static vk_lag_kind_stats last_counts[VK_LAG_KIND_COUNT];
    vk_lag_kind_stats counts[VK_LAG_KIND_COUNT], old_counts[VK_LAG_KIND_COUNT];
    memcpy(counts, g_lag_kind, sizeof counts); memcpy(old_counts, last_counts, sizeof old_counts);
    memcpy(last_counts, counts, sizeof counts);
    static uint64_t ad_old[4], ad_old_rebuilds, ad_old_rebuild_ns, ad_old_waits, ad_old_hots;
    uint64_t ad_now[4] = {g_ad.skips, g_ad.set_hits, g_ad.rotation, g_ad.overflowed}, ad_d[4];
    for (int i = 0; i < 4; i++) { ad_d[i] = ad_now[i] - ad_old[i]; ad_old[i] = ad_now[i]; }
    uint64_t ad_rebuilds = g_ad.rebuilds - ad_old_rebuilds, ad_rebuild_ns = g_ad.rebuild_ns - ad_old_rebuild_ns;
    ad_old_rebuilds = g_ad.rebuilds; ad_old_rebuild_ns = g_ad.rebuild_ns;
    int ad_ranges = g_ad.n, ad_runs = g_ad.runs, ad_overflow = g_ad.overflow; uint64_t ad_bytes = g_ad.bytes;
    uint64_t danger_waits, danger_hots; nvn_dirty_lag_danger_stats(&danger_waits, &danger_hots);
    uint64_t ad_waits = danger_waits - ad_old_waits, ad_hots = danger_hots - ad_old_hots;
    ad_old_waits = danger_waits; ad_old_hots = danger_hots;
    vk_rec_unlock();
    if (n && lag_adaptive_period())
        LOGV("lag adaptive: K=%u; decisions skipped %.1f, danger set %.1f, rotation %.1f, overflow %.1f/frame; set %d unit runs "
             "(%.3f MiB) as %d alias-expanded ranges%s; new units %llu waited, %llu hot this window; rebuilds %.2f/frame, %.3f ms/frame",
             lag_adaptive_period(), (double)ad_d[0] / n, (double)ad_d[1] / n, (double)ad_d[2] / n, (double)ad_d[3] / n,
             ad_runs, (double)ad_bytes / 1048576.0, ad_ranges, ad_overflow ? " OVERFLOW: full protection" : "",
             (unsigned long long)ad_waits, (unsigned long long)ad_hots, (double)ad_rebuilds / n, (double)ad_rebuild_ns / 1e6 / n);
    static const char *const names[VK_LAG_KIND_COUNT] = {"vertex", "index", "storage", "uniform", "pointer", "texel", "copy", "other"};
    for (unsigned k = 0; n && k < VK_LAG_KIND_COUNT; k++) {
        const vk_lag_kind_stats *s = &counts[k], *p = &old_counts[k];
        if (s->decisions == p->decisions && s->snapshot_allocations == p->snapshot_allocations) continue;
#define LAG_DELTA(field) (s->field - p->field)
        uint64_t samples = LAG_DELTA(sample_count);
        LOGV("lag decisions %s: %.1f/frame; binding hit/miss %.1f/%.1f, unit hit/miss %.1f/%.1f, memo hit/miss %.1f/%.1f per frame; cold %.1f, hot %.1f/frame",
             names[k], (double)LAG_DELTA(decisions) / n, (double)LAG_DELTA(binding_hits) / n,
             (double)LAG_DELTA(binding_misses) / n, (double)LAG_DELTA(class_hits) / n, (double)LAG_DELTA(class_misses) / n,
             (double)LAG_DELTA(memo_hits) / n, (double)LAG_DELTA(memo_misses) / n,
             (double)LAG_DELTA(cold_guards) / n, (double)LAG_DELTA(hot_decisions) / n);
        LOGV("lag keys %s: range/align %.1f, mapping/lifetime %.1f metadata misses; submission/ticket %.1f, snapshot %.1f byte-key changes/frame (nonexclusive)",
             names[k], (double)LAG_DELTA(key_range) / n, (double)LAG_DELTA(key_mapping) / n,
             (double)LAG_DELTA(key_ticket) / n, (double)LAG_DELTA(key_snapshot) / n);
        LOGV("lag reuse %s: cross-ticket/cross-snapshot metadata %.1f/%.1f, cross-ticket units %.1f, hot-history refreshes %.1f/frame; copy authority hit/miss %.1f/%.1f, ticket/snapshot/content invalidations %.1f/%.1f/%.1f/frame",
             names[k], (double)LAG_DELTA(metadata_cross_ticket) / n, (double)LAG_DELTA(metadata_cross_snapshot) / n,
             (double)LAG_DELTA(unit_cross_ticket) / n, (double)LAG_DELTA(hot_refreshes) / n,
             (double)LAG_DELTA(copy_hits) / n, (double)LAG_DELTA(copy_misses) / n,
             (double)LAG_DELTA(copy_ticket_misses) / n, (double)LAG_DELTA(copy_snapshot_misses) / n,
             (double)LAG_DELTA(copy_content_misses) / n);
        if (k == VK_LAG_UNIFORM)
            LOGV("lag uniform sharing: direct snapshot binds %.1f, covering hit/miss %.1f/%.1f per frame (exact copy hits above; allocation bytes below)",
                 (double)LAG_DELTA(uniform_snapshot_binds) / n, (double)LAG_DELTA(uniform_cover_hits) / n,
                 (double)LAG_DELTA(uniform_cover_misses) / n);
        LOGV("lag cost %s: timing=%s %.1f samples/frame, %.1f ns/sample (1/64 decisions); hot copies %.1f, %.3f KiB, %.3f ms/frame; snapshot allocations %.1f, %.3f KiB/frame",
             names[k], lag_timing_on() ? "on" : "off", (double)samples / n, samples ? (double)LAG_DELTA(sample_ns) / samples : 0,
             (double)LAG_DELTA(hot_copies) / n, (double)LAG_DELTA(hot_bytes) / 1024.0 / n,
             (double)LAG_DELTA(hot_ns) / 1e6 / n, (double)LAG_DELTA(snapshot_allocations) / n,
             (double)LAG_DELTA(snapshot_bytes) / 1024.0 / n);
        LOGV("lag sizes %s: <=256B %.1f, <=4KiB %.1f, <=64KiB %.1f, <=hotcap %.1f, >hotcap %.1f/frame; cumulative max %llu B",
             names[k], (double)LAG_DELTA(size_buckets[0]) / n, (double)LAG_DELTA(size_buckets[1]) / n,
             (double)LAG_DELTA(size_buckets[2]) / n, (double)LAG_DELTA(size_buckets[3]) / n,
             (double)LAG_DELTA(size_buckets[4]) / n, (unsigned long long)s->max_size);
#undef LAG_DELTA
    }
    if (n) LOGV("lag capture: %.3f MiB, %.3f ms, %.1f ranges/frame; %.1f GPU-owned ranges/frame kept on queue timeline",
                (double)bytes / 1048576.0 / n, (double)ns / 1e6 / n, (double)ranges / n, (double)gpu / n);
    if (n && lag_wait_on()) LOGV("lag guard: %.3f MiB, %.3f ms, %.1f range registrations/frame; reuse waits fault %.3f, host/contended %.3f/frame, %.3f ms/frame",
        (double)guard_bytes / 1048576.0 / n, (double)guard_ns / 1e6 / n, (double)guard_ranges / n,
        (double)df / n, (double)dh / n, (double)dn / 1e6 / n);
    if (n && lag_wait_on()) LOGV("lag projection index: %.1f frozen versions/frame, %.3f ms/frame build (separate from recorder prep/service)",
        (double)index_versions / n, (double)index_ns / 1e6 / n);
    if (n && lag_wait_on()) {
        LOGV("lag sticky: %.2f re-arms/frame, %.1f armed-ticket updates/frame; hot copies %.2f ranges, %.3f KiB/frame; units promoted %llu, demoted %llu",
             (double)dr / n, (double)du / n, (double)hot_ranges / n, (double)hot_bytes / 1024.0 / n,
             (unsigned long long)dp, (unsigned long long)dd);
        nvn_dirty_lag_unit top[8]; int ntop = nvn_dirty_lag_top(top, 8);
        for (int i = 0; i < ntop; i++) LOGV("lag fault unit %p+%zu: %llu writes, %llu waits, last frame %llu, hot %d; guard reasons 0x%x [readonly=%d cold=%d over-cap=%d GPU=%d writable=%d alias=%d], max range %zu",
            (void *)top[i].addr, top[i].len, (unsigned long long)top[i].faults, (unsigned long long)top[i].waits,
            (unsigned long long)top[i].last_frame, top[i].hot, top[i].reasons,
            !!(top[i].reasons & NVN_DIRTY_LAG_READONLY), !!(top[i].reasons & NVN_DIRTY_LAG_COLD),
            !!(top[i].reasons & NVN_DIRTY_LAG_OVER_CAP), !!(top[i].reasons & NVN_DIRTY_LAG_GPU),
            !!(top[i].reasons & NVN_DIRTY_LAG_WRITABLE), !!(top[i].reasons & NVN_DIRTY_LAG_ALIAS), top[i].max_range);
    }
}
/* Unlike ordinary optional renaming this protects every CPU-known UBO, even a cold range. Both inline drawing and
   recorder freezes call it on the walking thread. The native base is freshly captured per game submission; sequence
   (g_snap_seq), not a Vulkan fence, says which direct CPU overlays that immutable base already contains. */
void vk_lag_uniform_prepare(uint64_t addr, uint64_t size) {
    if (!g_ub_unflushed || !ub_private_on() || !addr || !size || addr + size < addr) return;
    addr = ub_address(addr, size);
    if (lag_original_pending(addr, size)) vk_ub_touch(addr, size);
}
/* TOTK_VK_PREPARE_MASK (vk_draw.c). Preparation only materializes private inline overlays when an original-memory
   producer overlaps. It does not select bytes, capture memory, or acquire a writer watch. Completion/CPU writes can
   never introduce such a producer. All new overlays, snapshot resets and original producers advance these walk-owned
   revisions; mapping changes revoke the physical identity independently. */
int vk_lag_uniform_prepare_changed(void) {
    static uint64_t uniform, producer, spans;
    uint64_t generation = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (uniform == g_ub_gen && producer == g_lag_producer_revision && spans == generation) return 0;
    uniform = g_ub_gen; producer = g_lag_producer_revision; spans = generation;
    return 1;
}
void vk_lag_uniform_prepare_check(uint64_t addr, uint64_t size) {
    uint64_t revision = g_ub_gen;
    vk_lag_uniform_prepare(addr, size);
    if (revision != g_ub_gen) DIE("lag prepare oracle: skipped required materialization at %#llx+%#llx",
        (unsigned long long)addr, (unsigned long long)size);
}
/* A tiny fixed-way memo shares only owned snapshot bytes. Expansion is at most 2x the requested extent and
   4 KiB, clipped to the captured span: never read unowned neighbouring bytes or blindly copy a dirty unit.
   Entries describe immutable allocations; a new partial overlay appends a new version, never patches an old one.
   The two candidate pages cover every <=4 KiB window; large requests retain exact-range memoization only. */
static void lag_uniform_snapshot_read(const snap_t *sn, const lag_binding_memo *m,
                                      VkDescriptorBufferInfo *out, vk_lag_kind_stats *s) {
    uint64_t address = m->physical, size = m->size, align = m->align, offset = sn->off + address - sn->lo;
    if (!(offset % align) && !snap_range_updated(sn, address, size)) {
        *out = (VkDescriptorBufferInfo){sn->buf, offset, size};
        g_stat_snap_binds++; s->uniform_snapshot_binds++;
        return;
    }
    enum { NB = 256, WAYS = 4 };
    static struct {
        uint64_t lo, hi, spans, lifetime, snapshot, revision;
        VkDescriptorBufferInfo descriptor;
    } cover[NB][WAYS];
    static unsigned next[NB];
    uint64_t page = address & ~UINT64_C(4095);
    if (size <= 4096) for (unsigned lane = 0; lane < 2 && (!lane || page >= 4096); lane++) {
        unsigned bucket = lag_hash(page - lane * 4096, 0, 0) % NB;
        for (unsigned i = 0; i < WAYS; i++) {
            const __typeof__(cover[0][0]) *c = &cover[bucket][i];
            if (c->snapshot != m->snapshot || c->spans != m->spans || c->lifetime != m->lifetime || address < c->lo ||
                address >= c->hi || size > c->hi - address) continue;
            uint64_t at = c->descriptor.offset + address - c->lo;
            if (at % align || !ub_range_unchanged(c->revision, address, size)) continue;
            *out = (VkDescriptorBufferInfo){c->descriptor.buffer, at, size};
            s->uniform_cover_hits++;
            return; /* never advance the whole window's revision after proving only this requested subrange */
        }
    }
    s->uniform_cover_misses++;
    uint64_t lo = address, hi = address + size;
    if (size <= 4096) {
        uint64_t width = 4096;
        while (width > size * 2) width /= 2;
        uint64_t step = width / 2, start = address - address % width, end = start + width;
        if (hi > end) { start = address - address % step; end = start + width; }
        if (end >= start && hi <= end) {
            lo = start > sn->lo ? start : sn->lo; hi = end < sn->hi ? end : sn->hi;
            /* The returned binding, not the guest span's beginning, must satisfy the requested alignment. */
            lo = address - (address - lo) / align * align;
        }
    }
    VkBuffer buffer; VkDeviceSize at;
    uint8_t *bytes = arena_alloc(snap_arena(), (size_t)(hi - lo), align, &buffer, &at);
    lag_snapshot_allocation(VK_LAG_UNIFORM, hi - lo);
    ub_read_from(lo, bytes, (size_t)(hi - lo), sn);
    *out = (VkDescriptorBufferInfo){buffer, at + address - lo, size};
    if (hi - lo <= 4096) {
        unsigned bucket = lag_hash(lo & ~UINT64_C(4095), 0, 0) % NB;
        __typeof__(cover[0][0]) *c = &cover[bucket][next[bucket]++ % WAYS];
        *c = (__typeof__(*c)){lo, hi, m->spans, m->lifetime, m->snapshot,
            g_ub_gen, {buffer, at, hi - lo}};
    }
    g_stat_ub_renamed++;
}
enum { XLATE_UNIFORM_MEMOS = 1024 };
static lag_binding_memo g_lag_uniform_memo[XLATE_UNIFORM_MEMOS];
static int lag_uniform_select(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                              int replay) {
    if (!align) align = 16;
    /* Shared with the diagnostic observer; selection remains the sole writer. */
    vk_lag_kind_stats *s = &g_lag_kind[VK_LAG_UNIFORM];
    lag_binding_memo *m = &g_lag_uniform_memo[lag_hash(addr, size, align) % XLATE_UNIFORM_MEMOS];
    lag_binding_key(m, addr, size, align, s);
    if (lag_binding_copy(m, out, s)) { s->memo_hits++; LAG_TEST_INC(VK_LAG_TEST_UNIFORM_HITS); return 1; }
    s->memo_misses++;
    totk_ft_count(FT_COUNT_LAG_UNIFORM_MISS, 1);
    addr = m->physical; m->captured = 0;
    if (!replay && lag_original_pending(addr, size)) { vk_ub_touch(addr, size); return 0; }
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL;
    VkBuffer b; uint64_t offset;
    if (!sn && !vk_resolve(addr, size, &b, &offset)) return 0;
    uint64_t dirty = nvn_dirty_now();
    int reusable;
    if (sn) {
        lag_uniform_snapshot_read(sn, m, out, s);
        reusable = 1; /* CPU rewrites cannot replace an owned eager snapshot's old-frame base. */
    } else {
        /* No expansion or shared watch booleans for mutable native bases. Every actual copy reacquires ALL
           writer gates, even after a producer completed without a CPU dirty-generation transition. */
        reusable = lag_watch_copy(addr, size);
        uint8_t *p = arena_alloc(snap_arena(), (size_t)size, align, &b, &offset);
        lag_snapshot_allocation(VK_LAG_UNIFORM, size);
        ub_read_from(addr, p, (size_t)size, NULL);
        *out = (VkDescriptorBufferInfo){b, offset, size};
        g_stat_ub_renamed++;
        reusable &= dirty == nvn_dirty_now();
    }
    m->revision = g_ub_gen; m->dirty = dirty; m->descriptor = *out; m->captured = reusable; m->immutable = sn != NULL;
    if (!reusable) g_lag_copy_unwatched++;
    return 1;
}
int vk_lag_uniform_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!ub_private_on() || !addr || !size || addr + size < addr) return 0;
    uint64_t start = lag_decision_begin(VK_LAG_UNIFORM, size);
    int captured = lag_uniform_select(addr, size, align, out, 0);
    lag_decision_end(VK_LAG_UNIFORM, start);
    return captured;
}
/* A frozen packet can execute after later inline updates to its original
   buffer. Descriptor handles alone do not freeze those bytes. Reuse the
   existing submission-owned immutable selector even when frame lag is off.
   Logged CPU updates are authoritative overlays; a pending GPU-only writer
   is not CPU-readable and cannot be guessed from the guest mapping. */
int vk_ub_replay_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!addr || !size || addr+size<addr) return 0;
    addr=ub_address(addr,size);
    if (!addr || addr+size<addr) return 0;
    pthread_mutex_lock(&g_gw_mu);
    int pending=gw_pending(addr,addr+size,atomic_load_explicit(&g_done,memory_order_acquire));
    pthread_mutex_unlock(&g_gw_mu);
    if (pending) return 0;
    return lag_uniform_select(addr,size,align,out,1);
}
void vk_xlate_uniform_key(uint64_t address, uint64_t size, uint64_t out[4]) {
    out[0] = XLATE_UNIFORM_UNKNOWN; out[1] = out[2] = out[3] = UINT64_MAX;
    if (!address || !size || address + size < address) return;
    uint64_t align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 16;
    const lag_binding_memo *m = &g_lag_uniform_memo[lag_hash(address, size, align) % XLATE_UNIFORM_MEMOS];
    int memo = m->raw == address && m->size == size && m->align == align &&
        m->submission == vk_submit_serial() && m->snapshot == g_snap_generation &&
        m->spans == atomic_load_explicit(&g_span_gen, memory_order_acquire) && m->lifetime == nvn_dirty_lag_epoch();
    uint64_t physical = address;
    if (ub_private_on()) {
        /* Only an existing mapping decision can canonicalize aliases here. Never resolve a new live resource. */
        if (!memo) return;
        physical = m->physical;
    }
    if (physical + size < physical) return;
    if (memo && m->descriptor.buffer) {
        out[0] |= XLATE_UNIFORM_DIRTY; out[2] = m->dirty;
        if (m->captured && (m->immutable || m->ticket == g_reserved_fence + 1) &&
            (m->immutable || m->dirty == nvn_dirty_now()) && ub_range_unchanged(m->revision, physical, size))
            out[0] |= XLATE_UNIFORM_CAPTURE;
    }
    const snap_t *sn = g_nsnap ? snap_find(physical, size) : NULL;
    uint64_t end = physical + size, newest = 0, done = atomic_load_explicit(&g_done, memory_order_acquire);
    uint64_t cursor = end;
    if (g_nub && end > g_ub_lo && physical < g_ub_hi) {
        int a = 0, b = g_nub;
        while (a < b) { int mid = (a + b) >> 1; if (g_ub[mid].lo < end) a = mid + 1; else b = mid; }
        uint64_t floor = physical > g_ub_maxlen ? physical - g_ub_maxlen : 0;
        for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > physical && (g_ub[i].lo > floor || !floor); i--) {
            const ub_ent *e = &g_ub[i];
            if (e->hi <= physical || (sn ? e->seq <= g_snap_seq : e->fence && e->fence <= done)) continue;
            if (e->seq > newest) newest = e->seq;
            if (e->hi >= cursor) cursor = e->lo; /* sorted-by-start union proves complete log coverage */
        }
    }
    if (newest) out[0] |= XLATE_UNIFORM_LOGGED;
    /* Private mode removes superseded log slices on every GPU write. Other modes can retain old log entries
       alongside a live descriptor: without an owned base, their apparent coverage is not selected-byte authority. */
    if (sn || (ub_private_on() && cursor <= physical)) {
        out[0] = (out[0] & ~XLATE_UNIFORM_UNKNOWN) | XLATE_UNIFORM_OWNED;
        if (sn) out[0] |= XLATE_UNIFORM_SNAPSHOT;
        out[1] = newest; out[3] = sn ? g_snap_generation : 0;
    }
}
void vk_ub_read(uint64_t addr, void *dst, size_t size) {
    addr = ub_address(addr, size);
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL;
    enum { NC = 128, BYTES = 32 };
    static struct { uint64_t address, size, revision, submission; const snap_t *snapshot; uint8_t bytes[BYTES]; } memo[NC];
    if (!vk_record_enabled() || !sn || !sn->updated || size > BYTES) { ub_read_from(addr, dst, size, sn); return; }
    __typeof__(memo[0]) *m = &memo[(unsigned)((addr >> 3) ^ (addr >> 16) ^ size) % NC];
    uint64_t submission = vk_submit_serial();
    if (m->address == addr && m->size == size && m->snapshot == sn && m->submission == submission &&
        (m->revision == g_ub_gen || vk_record_uniform_unchanged(m->revision, addr, size))) {
        memcpy(dst, m->bytes, size); m->revision = g_ub_gen;
        if (vk_rec_profile_on && T_rec.active) g_rec.pointer_hits++;
        return;
    }
    ub_read_from(addr, dst, size, sn);
    m->address = addr; m->size = size; m->snapshot = sn; m->submission = submission; m->revision = g_ub_gen; memcpy(m->bytes, dst, size);
    if (vk_rec_profile_on && T_rec.active) g_rec.pointer_misses++;
}
/* Return the authority that supplies every byte without a mutable native gap: explicit immutable snapshot (1),
   or complete CPU-known log coverage (2). Log authority expires on any log generation change, including pruning;
   a same-value publication is not permission to certify a now-native parent against future CPU writes. */
static int ub_read_owned(uint64_t address, uint64_t size) {
    if (g_nsnap && snap_find(address, size)) return 1;
    ub_prune();
    uint64_t end = address + size;
    if (!g_nub || end <= g_ub_lo || address >= g_ub_hi) return 0;
    int a = 0, b = g_nub;
    while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < end) a = m + 1; else b = m; }
    uint64_t cursor = end, floor = address > g_ub_maxlen ? address - g_ub_maxlen : 0;
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > address && (g_ub[i].lo > floor || !floor); i--) {
        if (g_ub[i].hi >= cursor) cursor = g_ub[i].lo;
        if (cursor <= address) return 2;
    }
    return 0;
}
void vk_ub_read_record(uint64_t addr, void *dst, size_t size) {
    if (!size) { vk_ub_read(addr, dst, size); return; }
    if (!vk_lag_capture_on()) {
        /* Without write watching, only owned bytes certify an indirect pointer for stage-freeze reuse.
           The selected child's tracking cannot make a native parent pointer immutable. */
        if (!addr || addr + size < addr || !ub_read_owned(addr, size)) g_lag_copy_unwatched++;
        vk_ub_read(addr, dst, size); return;
    }
    if (!addr || addr + size < addr) {
        g_lag_copy_unwatched++; vk_ub_read(addr, dst, size); return;
    }
    uint64_t start = lag_decision_begin(VK_LAG_POINTER, size);
    vk_lag_kind_stats *s = &g_lag_kind[VK_LAG_POINTER];
    enum { NP = 128 };
    static lag_binding_memo memo[NP];
    lag_binding_memo *m = &memo[lag_hash(addr, size, 0) % NP];
    lag_binding_key(m, addr, size, 0, s);
    addr = m->physical;
    uint64_t dirty = nvn_dirty_now(), revision = g_ub_gen;
    int authority = m->immutable;
    int stable = m->captured && (authority || m->dirty == dirty) &&
        (authority == 2 ? m->revision == revision : ub_range_unchanged(m->revision, addr, size));
    if (stable) s->memo_hits++; else s->memo_misses++;
    if (!stable) {
        authority = ub_read_owned(addr, size);
        /* Sample before any watch: an early writer projection can fault while later ones are being armed. */
        dirty = nvn_dirty_now();
        stable = authority || (!lag_original_pending(addr, size) && lag_watch_copy(addr, size));
        revision = g_ub_gen;
    }
    vk_ub_read(addr, dst, size); /* native/unwatchable parents never use vk_ub_read's snapshot-only byte memo */
    if (!authority) stable &= dirty == nvn_dirty_now();
    else if (authority == 2) stable &= revision == g_ub_gen;
    m->captured = stable; m->immutable = authority; m->dirty = dirty; m->revision = g_ub_gen;
    if (!stable) g_lag_copy_unwatched++;
    lag_decision_end(VK_LAG_POINTER, start);
}
#ifdef TOTK_VK_TESTS
uint64_t vk_record_test_pointer_hits(void) { return g_rec.pointer_hits; }
#endif
static void update_ub(uint64_t dst, const uint8_t *data, uint64_t n) {
    VkBuffer db; uint64_t dof;
    if (n <= 64) vk_pd_write_note("inline update", dst, n, data);
    int ok = vk_resolve(dst, n, &db, &dof);
    if (!ok || (dof & 3) || (n & 3) || n > 65536 || !n) { vk_cpu_step(); memcpy((void *)(uintptr_t)dst, data, (size_t)n); vk_gpu_write(dst, n); return; }
    g_stat_ub++;
    if (ub_rename_on()) { ub_log(dst, data, n, 0); return; }
    if (ub_private_on()) {
        uint64_t a = ub_address(dst, n);
        pthread_mutex_lock(&g_gw_mu);
        int gpu = gw_pending(a, a + n, atomic_load(&g_done));
        pthread_mutex_unlock(&g_gw_mu);
        if (!gpu && !ub_original_pending(a, n)) { ub_log(dst, data, n, 0); return; }
        vk_gpu_write(dst, n);   /* a true original-memory producer cannot be replaced by a stale CPU base */
    }
    vk_end_rendering_at(VK_PASS_END_UNIFORM, 0);
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    vkCmdUpdateBuffer(cb, db, dof, n, data);
    vk_barrier_all(cb);
    /* CPU-resolved storage pointers / recorder captures need these bytes before the GPU executes the update too.
       This entry is already recorded: retain it through its fence, but never flush it again or rename a descriptor. */
    ub_log(dst, data, n, 1);
    lag_producer_changed(ub_address(dst, n), n);
}
static void post(post_op o) {
    if (g_ncarry == g_capcarry) { g_capcarry = g_capcarry ? 2 * g_capcarry : 64; g_carry = VK_REALLOC(g_carry, (size_t)g_capcarry * sizeof *g_carry); }
    g_carry[g_ncarry++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    uint32_t q1 = 0;
    if ((g_gpu_time_hardware || type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) && ts_reports_on() && !atomic_load(&g_tsq_busy[g_tsq_next])) {
        /* Hardware mode timestamps every counter type too: a samples report's
           timestamp field must not jump ahead to the final chunk's CPU delivery. */
        uint32_t q = g_tsq_next; g_tsq_next = (q + 1) % TSQ; atomic_store(&g_tsq_busy[q], 1);
        VkPipelineStageFlags2 stage = type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        if (!vk_record_counter_timestamp(g_tsq, q, stage)) vkCmdWriteTimestamp2(vk_cb(), stage, g_tsq, q);
        q1 = q + 1;
        if (!g_cur->tsq_count) g_cur->tsq_first = q;
        g_cur->tsq_count++;
        g_tsq_time[q].valid = 0;
    }
    occl_node *samples = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? occl_snapshot() : NULL;
    uint64_t val = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? vk_counter_samples() : 0;
    post((post_op){.kind = 2, .addr = addr, .type = type, .cval = val, .tsq1 = q1, .occl = samples});
    cpend_push(addr, val, type != NVN_COUNTER_TYPE_SAMPLES_PASSED || !g_occl_on);
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void vk_note_draw(void) { g_stat_draw++; }
static void ub_seal(uint64_t fence) { for (int i = 0; i < g_nub; i++) if (g_ub[i].flushed && !g_ub[i].fence) g_ub[i].fence = fence; ub_changed(0, 0); }

/* A bounded draw-side lookahead never escapes the current recording or follows calls. Restored on recursive return. */
static const uint8_t *g_exec_next; static uint32_t g_exec_op;
const uint8_t *vk_exec_draw_next(void) { return g_exec_op == OP_DRAW || g_exec_op == OP_DRAW_IDX ? g_exec_next : NULL; }
/* Constant specialization removes all diagnostic work from the disabled opcode loop, including bind checks. */
static __attribute__((always_inline)) inline void mtl_exec_impl(const uint8_t *p, int diagnostics) {
    const uint8_t *saved_next = g_exec_next; uint32_t saved_op = g_exec_op;
    int profile_stream = -1; if (__builtin_expect(vk_rec_timing_on, 0)) profile_stream = rec_stream_begin();
    /* Poll at bounded work chunks, not every binding opcode. Calls start a
       fresh chunk and jumps still consume its budget, so draw-free streams
       cannot starve an invalidate. The flag remains a relaxed atomic hint;
       the ownership-release handshake supplies the no-lost-wakeup guarantee. */
    unsigned service_left = 0;
    for (;;) {
        if (__builtin_expect(!service_left--, 0)) {
            vk_rec_service_check();
            service_left = 63;
        }
        if (__builtin_expect(vk_rec_timing_on, 0)) {
            uint32_t code; memcpy(&code, p, sizeof code);
            if (code < OP_COUNT_) rec_sample_begin(code);
        }
        op_hdr h; memcpy(&h, p, sizeof h);
        if (__builtin_expect(vk_rec_profile_on, 0) && T_rec.active) g_rec.ops++;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) DIE("【红】corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const uint8_t *a = p + 8;
        const uint8_t *next = p + h.size;
        g_exec_next = next; g_exec_op = h.op;
        if (diagnostics) {
            if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) xlate_deferred_bind(h.op, a);
            if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) {
                g_xlate->counts.draws++;
                for (unsigned lane = 0; lane < 2; lane++) {
                    g_xlate->counts.repeated_draws[lane] += g_xlate->repeated[lane];
                    g_xlate->counts.covered_draws[lane] += g_xlate->covered[lane];
                }
            }
        }
        switch (h.op) {
            case OP_END:
                if (__builtin_expect(vk_rec_timing_on, 0)) { rec_sample_end(); vk_rec_profile_pop(profile_stream); }
                g_exec_next = saved_next; g_exec_op = saved_op; return;
            case OP_BEGIN: break;
            case OP_JUMP: memcpy(&next, a, sizeof next); break;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); if (vk_fg_extract_active) vk_fg_extract_memory_write((uint64_t)(uintptr_t)val,4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); if (vk_fg_extract_active) vk_fg_extract_memory_write(ad,16); VK_REC_DO(VK_REC_S_COUNTER, vk_draw_exec(h.op, a); mtl_sub_counter(ad, (uint32_t)t)); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); if (vk_fg_extract_active) vk_fg_extract_memory_write(d,n); VK_REC_DO(VK_REC_O_TRANSFER, mtl_sub_copy(s, d, n)); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (vk_fg_extract_active) vk_fg_extract_memory_write(d,n); VK_REC_DO(VK_REC_S_UNIFORM, update_ub(d, a + 16, n)); break; }
            default:
                if (__builtin_expect(vk_rec_timing_on, 0)) {
                    int category = h.op == OP_RESET_COUNTER || h.op == OP_RENDER_COND ? VK_REC_S_COUNTER :
                        h.op == OP_SET_RT || h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS || h.op == OP_BARRIER ? VK_REC_S_PASS :
                        h.op == OP_DRAW || h.op == OP_DRAW_IDX ? VK_REC_S_TOUCH : VK_REC_S_STATE;
                    VK_REC_DO(category, vk_draw_exec(h.op, a));
                } else vk_draw_exec(h.op, a);
                break;
        }
        if (__builtin_expect(vk_rec_timing_on, 0)) rec_sample_end();
        p = next;
    }
}
void mtl_exec(const uint8_t *p) {
    if (__builtin_expect(vk_xlate_stats_on(), 0)) {
        if (!g_xlate->depth++) xlate_recording(p);
        mtl_exec_impl(p, 1);
        if (!--g_xlate->depth) xlate_publish();
    } else mtl_exec_impl(p, 0);
}
uint64_t mtl_sub_commit(void) {
    mtl_init();
    int slot_trace=__builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        vk_fg_job_trace_enabled();
    uint64_t slot_submit_begin=slot_trace ? host_now_ns() : 0;
    uint64_t v = slot_submit(1, VK_PASS_END_SUBMISSION);
    uint64_t slot_submit_return=slot_trace ? host_now_ns() : 0;
    vk_rec_service_run(1); /* producer complete: service before unrelated future submissions can acquire ownership */
    uint64_t slot_service_return=slot_trace ? host_now_ns() : 0;
    g_in_sub = 0;
    g_gpu_guest_ns = g_gpu_origin_ticks = g_gpu_guest_frame = 0;
    if (!g_cur && __builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0)) {
        uint64_t enter=slot_trace ? host_now_ns() : 0;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=enter kind=commit boundary_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=0 clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v);
        vk_fg_slot_boundary();
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=exit kind=commit enter_ns=%llu exit_ns=%llu real_ticket=%llu clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)host_now_ns(),(unsigned long long)v);
    } else if (slot_trace) {
        LOGV("[adaptive-slot-boundary] event=skip kind=commit now_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=%u active=%u clock=CLOCK_MONOTONIC",
            (unsigned long long)host_now_ns(),(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v,g_cur!=NULL,
            atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed)!=0);
    }
    if (ub_private_on()) snap_reset();
    if (g_sar_cur >= 0) { g_sar[g_sar_cur].release = v; g_sar_cur = -1; }
    g_stat_sub++;
    /* TOTK_VK_SYNC=1 (diagnostic): every submission finishes on the GPU before the game goes on, as if the GPU were
       infinitely fast; whatever still differs between frames then is not a race with the GPU. */
    { static int sync = -1; if (sync < 0) { const char *e = getenv("TOTK_VK_SYNC"); sync = e && atoi(e) > 0; if (sync) LOGV("TOTK_VK_SYNC: each submission is waited for"); }
      if (sync) mtl_wait(v, 5000000000ull); }
    ub_prune(); gw_prune();
    if (g_perf_t0) {
        uint64_t cpu = walker_cpu_now() - g_perf_cpu_t0, dt = host_now_ns() - g_perf_t0;
        uint64_t w = g_perf_wait_ns - g_perf_sub0, r = dt > w ? dt - w : 0;
        g_perf_cpu_ns += cpu; g_perf_elapsed_ns += dt;
        g_perf_replay_ns += r; g_cum[0] += r; g_perf_t0 = 0; g_perf_subs++; vk_record_walk_time(r);
        HK_ADD(subs, 1); HK_ADD(replay_ns, r); HK_ADD(wait_ns, w);
    }
    if (__builtin_expect(vk_rec_timing_on, 0)) vk_rec_profile_end();
    vk_res_sweep();   /* landing also owns the recording lock: no new GPU guards while guest bytes are stored */
    vk_rec_unlock();   /* taken in mtl_sub_begin */
    if (g_stat_sub % 3000 == 0) {
        uint64_t n, b; gpu_pool_totals(&n, &b);
        LOGV("uniform snapshots: %.1f KiB in %.0f ranges per submission, %llu bindings read them, %llu ranges left to memory (GPU writes in flight)", g_stat_snap_bytes / 1024.0 / g_stat_sub, (double)g_stat_snap_ranges / g_stat_sub, (unsigned long long)g_stat_snap_binds, (unsigned long long)g_stat_snap_skipped);
        LOGV("%llu submissions: %llu GPU copies, %llu uniform updates (%llu renamed copies), %llu CPU steps; %llu pools (%llu MiB on the GPU, %llu CPU-only)",
             (unsigned long long)g_stat_sub, (unsigned long long)g_stat_gpu_copy, (unsigned long long)g_stat_ub, (unsigned long long)g_stat_ub_renamed, (unsigned long long)g_stat_cpu_step,
             (unsigned long long)n, (unsigned long long)(b >> 20), (unsigned long long)g_pool_cpu);
        vk_res_stats();
    }
    nvn_dirty_lag_thread_role(g_lag_guest_role);
    totk_ft_work_end();
    T_ft_walk_frame = 0;
    return v;
}
/* A submission of the backend's own (present, CPU reads): whatever is recorded goes to the GPU and is waited for. */
/* Memory the backend holds (TOTK, 2026-10-22: the app's memory grew ~45 MB/s in the world, native heap and GPU-mapped
   memory together, until Android ran out). Logged with the per-300-frame lines: whichever number climbs is the leak. */
void vk_res_mem(uint64_t v[8]);
int vk_dpool_count(void) { int n = 0; for (int i = 0; i < RING; i++) n += g_ring[i].ndpool; return n; }
/* Stop capture at a game-submission boundary, never in the middle of a stream
   whose later draws may still use private uniform/texel incarnations. The GPU
   cursor remains enabled for its original lifetime-pin contract. */
static void lag_pressure_fallback(void) {
    if (!nvn_dirty_lag_vma_pressure() || !vk_lag_capture_on()) return;
    vk_flush_wait();
    submit_join(mtl_last_fence());
    nvn_dirty_lag_vma_drain();
    ub_publish();
    snap_reset();
    for (int i = 0; i < SNAP_ARENAS; i++) {
        arena_release(&g_sar[i].ar);
        g_sar[i].release = 0;
    }
    g_sar_cur = -1; g_sar_next = 0;
    ub_prune(); gw_prune();
    atomic_store_explicit(&vk_lag_capture_state, 0, memory_order_release);
    uint64_t vma[5]; nvn_dirty_lag_vma_stats(vma);
    LOGV("lag pressure cutover complete: maps %llu/%llu, guarded runs %llu; capture off, ordinary submission and raw frame-end waits restored",
        (unsigned long long)vma[0], (unsigned long long)vma[1], (unsigned long long)vma[2]);
}
/* /proc/self/maps holds ~30-40k lines in-game. Both budget counts and opt-in classification
   (TOTK_VMA_CLASSES=1) run on the low-priority sampler; this present-path report only reads cached
   samples/telemetry and optionally requests background work, never waits for or performs a maps scan. */
void vk_mem_report(void) {
    static int classes = -1;
    if (classes < 0) { const char *e = getenv("TOTK_VMA_CLASSES"); classes = e && !strcmp(e, "1"); }
    if (nvn_dirty_lag_enabled()) {
        uint64_t vma[5], scans[4]; nvn_dirty_lag_vma_stats(vma); nvn_dirty_lag_vma_scan_stats(scans, 1);
        LOGV("memory VMAs: %llu/%llu, guarded-run credit %llu/%llu, pressure fallback %llu; scans %llu (epoch-requested %llu), scan ms total/max %.3f/%.3f",
            (unsigned long long)vma[0], (unsigned long long)vma[1], (unsigned long long)vma[2],
            (unsigned long long)vma[3], (unsigned long long)vma[4],
            (unsigned long long)scans[0], (unsigned long long)scans[1], scans[2] / 1e6, scans[3] / 1e6);
    }
    if (classes) nvn_dirty_lag_vma_request(1);
    memory_objects_report();
    uint64_t chunk = 0, nchunk = 0, ndp = 0, posts = 0;
    for (int i = 0; i < RING; i++) { slot_t *s = &g_ring[i]; nchunk += (uint64_t)s->ar.nchunk; ndp += (uint64_t)s->ndpool; posts += (uint64_t)s->cap; for (int c = 0; c < s->ar.nchunk; c++) chunk += s->ar.chunks[c].size; }
    uint64_t slot_chunks = chunk;
    for (int i = 0; i < SNAP_ARENAS; i++) { nchunk += (uint64_t)g_sar[i].ar.nchunk; for (int c = 0; c < g_sar[i].ar.nchunk; c++) chunk += g_sar[i].ar.chunks[c].size; }
    uint64_t recorder[4]; vk_record_memory(recorder);
    LOGV("memory recording: slot/snapshot arenas retained %.1f/%.1f MiB; draw packet slabs %.1f MiB, descriptor input slabs %.1f MiB; worker secondary buffers %llu, descriptor pools %llu (driver bytes not counted)",
        slot_chunks / 1048576.0, (chunk - slot_chunks) / 1048576.0, recorder[0] / 1048576.0, recorder[1] / 1048576.0,
        (unsigned long long)recorder[2], (unsigned long long)recorder[3]);
    LOGV("memory staging retention: cap %d MiB per slot (-1: none), trims %llu, %.1f MiB not retained (TOTK_VK_STAGING_RETAIN_MB)",
        staging_retain_mb(), (unsigned long long)atomic_load_explicit(&g_staging_trims, memory_order_relaxed),
        atomic_load_explicit(&g_staging_trimmed, memory_order_relaxed) / 1048576.0);
    uint64_t primary[2]; primary_memory(primary);
    LOGV("memory primary command buffers: retained %llu, reclaimed %llu; retired-only trim after32 uses, warm floor256/slot (driver-private BO bytes not observable)",
        (unsigned long long)primary[0], (unsigned long long)primary[1]);
    uint64_t pn, pb; gpu_pool_totals(&pn, &pb);
    uint64_t r[8]; vk_res_mem(r);
    LOGV("memory image blocks: used/free %.3f/%.3f MiB (empty blocks retained for reuse)",
        r[7] / 1048576.0, (r[0] - r[7]) / 1048576.0);
#ifdef __GLIBC__
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    LOGV("memory: malloc %zu MiB in use; image blocks %llu MiB + dedicated %llu MiB, %llu images retired waiting, %llu texture objects, %llu shared images, %llu write-back records, %llu retired list capacity; "
         "pools %llu (%llu MiB); transient chunks %llu (%llu MiB), descriptor pools %llu, post-op capacity %llu; uniform log %d (cap %d), snapshots %d (cap %d), GPU-write ranges %d (cap %d)",
         (size_t)mi.uordblks >> 20, (unsigned long long)(r[0] >> 20), (unsigned long long)(r[1] >> 20), (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6],
         (unsigned long long)pn, (unsigned long long)(pb >> 20), (unsigned long long)nchunk, (unsigned long long)(chunk >> 20), (unsigned long long)ndp, (unsigned long long)posts,
         g_nub, g_capub, g_nsnap, g_capsnap, g_ngw, g_capgw);
}
uint64_t vk_submit_now(void) { return g_cur ? slot_submit(0, VK_PASS_END_SUBMISSION) : mtl_last_fence(); }
uint64_t vk_submit_readback(void) { return g_cur ? slot_submit(0, VK_PASS_END_CPU_WAIT) : mtl_last_fence(); }
void vk_flush_wait(void) { HK_ADD(idle_flush, 1); if (ub_private_on()) ub_flush(); if (g_cur) { uint64_t v = slot_submit(0, VK_PASS_END_CPU_WAIT); wait_value(v); } else vk_gpu_idle(); }
uint64_t mtl_last_fence(void) { pthread_mutex_lock(&g_mu); uint64_t f = g_reserved_fence; pthread_mutex_unlock(&g_mu); return f; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000u); dl.tv_nsec = (long)(ns % 1000000000u);
    uint64_t w0 = host_now_ns();
    pthread_mutex_lock(&g_mu);
    int blocked = atomic_load(&g_done) < fence;
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
    int ok = atomic_load(&g_done) >= fence;
    if (blocked && ok && g_done_at) HK_ADD(wake_ns, host_now_ns() - g_done_at);
    pthread_mutex_unlock(&g_mu);
    g_cum[2] += host_now_ns() - w0; g_cum[4]++;
    if (blocked) { HK_ADD(gwaits, 1); HK_ADD(gwait_ns, host_now_ns() - w0); }
    return ok;
}
int vk_gpuprof_renderer_drain(uint64_t timeout_ns) {
    uint64_t start = host_now_ns();
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t ns = (uint64_t)deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t)(ns / UINT64_C(1000000000));
    deadline.tv_nsec = (long)(ns % UINT64_C(1000000000));
    pthread_once(&g_rec_once, rec_mu_init);
    if (pthread_mutex_timedlock(&g_rec_mu, &deadline)) return 0;
    /* Enter the normal recursive owner bookkeeping after bounded admission. */
    vk_rec_lock();
    pthread_mutex_unlock(&g_rec_mu);
    uint64_t ticket = vk_submit_now();
    vk_rec_unlock();
    uint64_t elapsed = host_now_ns() - start;
    return mtl_wait(ticket, elapsed < timeout_ns ? timeout_ns - elapsed : 0);
}
void mtl_finish(void) { HK_ADD(idle_fin, 1); if (ub_private_on()) vk_flush_wait(); else vk_gpu_idle(); }
void mtl_drain(void) { if (ub_private_on()) vk_flush_wait(); else vk_gpu_idle(); }
uint64_t gpu_stat_submits(void) { return g_stat_sub; }
uint64_t gpu_stat_draws(void) { return g_stat_draw; }

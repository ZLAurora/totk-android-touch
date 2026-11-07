/* Textures and samplers of the Vulkan backend (vk_int.h); the Metal counterpart is the texture half of nvn_mtl.m
   (tex_mtl, tex_by_id, samp_by_id, pkg_upload, stor_*), whose rules this follows:

   - A texture in a CPU-visible pool ("mirrored": tex_mirrored) is uploaded from the guest's bytes at its first use and
     uploaded again when the game wrote any of its pages since (nvn_dirty.c page protection), checked once per
     submission -- unless the GPU has written it (render target, image store, copy, clear): from then on the image is
     the content, as on Metal.
   - Every other texture is GPU-only. Texture objects that describe the same storage with the same layout share one
     image (storage-shared textures: the game samples through one serial what it renders through another, re-created
     every frame); a different format of the same texel size is a view of the shared image.
   - Images stay in VK_IMAGE_LAYOUT_GENERAL; ordering between passes, copies and dispatches is by full memory barriers
     (vk_barrier_all). Uploads are recorded into the submission's "pre" command buffer, ahead of the game's commands.
   - CPU-pool renderings first settle after 60 quiet submissions; image copies ride the ordinary stream and completed
     bytes land once in live guest storage. Repeated landings of the same storage/layout back off (cap 4096 submissions).
     An explicit invalidate completes any overlapping unlanded rendering, even before the quiet interval. Only the
     automatic path waits for quiet; per-frame targets never stage without a CPU consumer. TOTK_VK_OFF=settledwb disables it.
     The first CPU write disarms the whole storage watch; only a new GPU write may re-arm it. Learned delays own no
     protection, and write-back watches never publish GPU-reader tickets.
     Default invalidates first test exact pending ranges under a short metadata-only lock: hull holes and already-landed
     renderings never drain the game frontend or acquire the recording lock.
     Automatic landing resumes in contiguous 16-KiB guest spans, at most 1 MiB/~2 ms per sweep; an explicit
     invalidate completes the remaining rendering. Images use the ordinary idle cache; empty blocks stay reusable.
   - Unbound or empty texture slots read zero (null descriptors where the driver has them, else 1x1 zero images). */
#include "vk_int.h"
#include "vk_framegen.h"
#include "vk_record.h"
#include "nvn_dirty.h"
#include "nvn_hmap.h"
#include "totk_lifelog.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include "vk_upload_pool.h"
#include "../../runtime/frame_timeline.h"
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <math.h>

#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define MINV(a, b) ((a) < (b) ? (a) : (b))

/* Independent diagnostic cutouts; defaults retain the current settled-writeback policies. */
static int backoff_on(void) { static int on = -1; if (on < 0) on = !vk_off("settledbackoff"); return on; }
static int invalidate_on(void) { static int on = -1; if (on < 0) on = !vk_off("settledinval"); return on; }
static int drain_on(void) { static int on = -1; if (on < 0) on = !vk_off("settleddrain"); return on; }
static int seal_on(void) { static int on = -1; if (on < 0) on = !vk_off("settledseal"); return on; }
static int cpu_disarm_on(void) { static int on = -1; if (on < 0) on = !vk_off("settledcpu"); return on; }

enum { WB_D_INVALIDATE, WB_D_DRAIN, WB_D_LOCK, WB_D_SEAL, WB_D_WAIT, WB_D_LAND, WB_D_COPY, WB_D_SWEEP, WB_D_FREE, WB_D_WB_SWEEP, WB_D_COUNT };
enum { WB_OP_DRAIN, WB_OP_LOCK, WB_OP_SEAL, WB_OP_SUBMIT, WB_OP_COUNT };
static _Atomic uint64_t g_wb_ops[WB_OP_COUNT];
static pthread_mutex_t g_wb_diag_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t phase[WB_D_COUNT], max, peak, tid;
    uint64_t land_peak, land_bytes, land_chunks, land_yields, land_calls;
    int kind; char role[32], thread[16];
} g_wb_diag;
/* Wall time includes CPU work and waits; overlapping scopes are maxima, never summed. Not a GPU timestamp. */
static void wb_diag_duration(int kind, uint64_t ns) {
    pthread_mutex_lock(&g_wb_diag_mu);
    if (ns > g_wb_diag.phase[kind]) g_wb_diag.phase[kind] = ns;
    if (ns > g_wb_diag.peak) g_wb_diag.peak = ns;
    if (ns > g_wb_diag.max) {
        g_wb_diag.max = ns; g_wb_diag.kind = kind;
#ifdef __linux__
        g_wb_diag.tid = (uint64_t)syscall(SYS_gettid);
        if (pthread_getname_np(pthread_self(), g_wb_diag.thread, sizeof g_wb_diag.thread)) g_wb_diag.thread[0] = 0;
#else
        g_wb_diag.tid = (uint64_t)(uintptr_t)pthread_self();
        g_wb_diag.thread[0] = 0;
#endif
        const char *role = nvn_dirty_lag_thread_role_get();
        snprintf(g_wb_diag.role, sizeof g_wb_diag.role, "%s", role ? role : "guest/unknown");
    }
    pthread_mutex_unlock(&g_wb_diag_mu);
}
static void wb_diag_end(int kind, uint64_t start) { wb_diag_duration(kind, host_now_ns() - start); }
static void wb_diag_sweep(uint64_t start, uint64_t bytes, uint64_t chunks, int yielded) {
    wb_diag_end(WB_D_WB_SWEEP, start);
    pthread_mutex_lock(&g_wb_diag_mu);
    if (g_wb_diag.phase[WB_D_WB_SWEEP] > g_wb_diag.land_peak) g_wb_diag.land_peak = g_wb_diag.phase[WB_D_WB_SWEEP];
    if (bytes > g_wb_diag.land_bytes) g_wb_diag.land_bytes = bytes;
    if (chunks > g_wb_diag.land_chunks) g_wb_diag.land_chunks = chunks;
    g_wb_diag.land_yields += yielded; g_wb_diag.land_calls++;
    pthread_mutex_unlock(&g_wb_diag_mu);
}

/* ---------- device memory: 64 MiB blocks per memory type, first-fit free lists; big images get their own ---------- */
#define BLOCK_SIZE (64ull << 20)
typedef struct { VkDeviceSize off, size; } span_t;
typedef struct { VkDeviceMemory mem; uint32_t type; span_t *free; int nfree, capfree; VkDeviceSize used; } block_t;
static block_t *g_blocks; static int g_nblocks;
static pthread_mutex_t g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_mem_dedicated_bytes, g_mem_block_bytes;

static void block_free_insert(block_t *b, VkDeviceSize off, VkDeviceSize size) {
    int i = 0; while (i < b->nfree && b->free[i].off < off) i++;
    if (b->nfree == b->capfree) { b->capfree = b->capfree ? 2 * b->capfree : 16; b->free = realloc(b->free, (size_t)b->capfree * sizeof *b->free); }
    memmove(b->free + i + 1, b->free + i, (size_t)(b->nfree - i) * sizeof *b->free); b->free[i] = (span_t){off, size}; b->nfree++;
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) { b->free[i].size += b->free[i + 1].size; memmove(b->free + i + 1, b->free + i + 2, (size_t)(b->nfree - i - 2) * sizeof *b->free); b->nfree--; }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) { b->free[i - 1].size += b->free[i].size; memmove(b->free + i, b->free + i + 1, (size_t)(b->nfree - i - 1) * sizeof *b->free); b->nfree--; }
}
int vk_mem_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, vk_alloc *out) {
    int type = vk_pick_type(req->memoryTypeBits, want);
    if (type < 0) type = vk_pick_type(req->memoryTypeBits, 0);
    if (type < 0) return 0;
    memset(out, 0, sizeof *out);
    VkDeviceSize align = req->alignment ? req->alignment : 1;
    if (align < V.props.limits.bufferImageGranularity) align = V.props.limits.bufferImageGranularity;
    if (req->size > BLOCK_SIZE / 4) {
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req->size, .memoryTypeIndex = (uint32_t)type};
        if (vk_memory_allocate(&mai, &out->mem, VK_MEMORY_IMAGE_DEDICATED)) return 0;
        out->size = req->size; out->block = -1;
        pthread_mutex_lock(&g_mem_mu); g_mem_dedicated_bytes += req->size; pthread_mutex_unlock(&g_mem_mu);
        return 1;
    }
    pthread_mutex_lock(&g_mem_mu);
    for (int pass = 0; pass < 2; pass++) {
        for (int bi = 0; bi < g_nblocks; bi++) {
            block_t *b = &g_blocks[bi];
            if (b->type != (uint32_t)type) continue;
            for (int i = 0; i < b->nfree; i++) {
                VkDeviceSize at = (b->free[i].off + align - 1) / align * align, end = b->free[i].off + b->free[i].size;
                if (at + req->size > end) continue;
                span_t f = b->free[i];
                memmove(b->free + i, b->free + i + 1, (size_t)(b->nfree - i - 1) * sizeof *b->free); b->nfree--;
                if (at > f.off) block_free_insert(b, f.off, at - f.off);
                if (at + req->size < end) block_free_insert(b, at + req->size, end - at - req->size);
                b->used += req->size;
                out->mem = b->mem; out->off = at; out->size = req->size; out->block = bi;
                pthread_mutex_unlock(&g_mem_mu);
                return 1;
            }
        }
        if (pass) break;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = BLOCK_SIZE, .memoryTypeIndex = (uint32_t)type};
        VkDeviceMemory m;
        if (vk_memory_allocate(&mai, &m, VK_MEMORY_IMAGE_BLOCK)) break;
        int bi = g_nblocks++;
        g_blocks = realloc(g_blocks, (size_t)g_nblocks * sizeof *g_blocks);
        block_t *b = &g_blocks[bi]; memset(b, 0, sizeof *b); b->mem = m; b->type = (uint32_t)type;
        block_free_insert(b, 0, BLOCK_SIZE);
        g_mem_block_bytes += BLOCK_SIZE;
    }
    pthread_mutex_unlock(&g_mem_mu);
    return 0;
}
void vk_mem_free(vk_alloc *a) {
    if (!a->mem) return;
    if (a->block < 0) { vk_memory_release(a->mem, VK_MEMORY_IMAGE_DEDICATED); pthread_mutex_lock(&g_mem_mu); g_mem_dedicated_bytes -= a->size; pthread_mutex_unlock(&g_mem_mu); }
    else {
        pthread_mutex_lock(&g_mem_mu);
        block_t *b = &g_blocks[a->block]; block_free_insert(b, a->off, a->size); b->used -= a->size;
        pthread_mutex_unlock(&g_mem_mu);
    }
    memset(a, 0, sizeof *a);
}

/* ---------- format support ---------- */
static VkFormatFeatureFlags fmt_features(VkFormat f) {
    static VkFormatFeatureFlags cache[256]; static uint8_t known[256];
    unsigned i = f < 185 ? (unsigned)f : f >= 1000340000 && f <= 1000340001 ? 185 + (unsigned)(f - 1000340000) : 255;
    if (i < 255 && known[i]) return cache[i];
    VkFormatProperties fp; V.get_format_props(V.pd, f, &fp);
    if (i < 255) { cache[i] = fp.optimalTilingFeatures; known[i] = 1; }
    return fp.optimalTilingFeatures;
}

/* ---------- images: shared by storage for GPU-only textures, one per serial for mirrored ones ---------- */
/* Stable generation cells outlive TLS cache entries, unlike the retired objects
   they identify. Recycle cells, not their storage: memory follows peak live
   textures/images rather than the number created over a run. g_tex_mu owns the
   free list and stamps; readers touch only the atomic cell value. */
typedef struct tex_generation { _Atomic uint64_t value; struct tex_generation *next; } tex_generation;
typedef struct { tex_generation *cell; uint64_t value; } tex_stamp;
static tex_generation *g_free_generations;
/* Any texture or image identity bumped or dropped (TOTK_VK_TEX_MEMO: one load instead of two cells per lookup). */
static _Atomic uint64_t g_tex_ident_rev = 1;
static tex_stamp tex_stamp_new(void) {
    if (!g_free_generations) {
        tex_generation *page = calloc(256, sizeof *page);
        if (!page) abort();
        for (int i = 0; i < 256; i++) {
            atomic_init(&page[i].value, 1);
            page[i].next = g_free_generations; g_free_generations = &page[i];
        }
    }
    tex_generation *cell = g_free_generations; g_free_generations = cell->next;
    return (tex_stamp){cell, atomic_load_explicit(&cell->value, memory_order_relaxed)};
}
static void tex_stamp_bump(tex_stamp *stamp) {
    stamp->value = atomic_fetch_add_explicit(&stamp->cell->value, 1, memory_order_release) + 1;
    atomic_fetch_add_explicit(&g_tex_ident_rev, 1, memory_order_release);
}
static void tex_stamp_drop(tex_stamp stamp) {
    atomic_fetch_add_explicit(&stamp.cell->value, 1, memory_order_release);
    atomic_fetch_add_explicit(&g_tex_ident_rev, 1, memory_order_release);
    stamp.cell->next = g_free_generations; g_free_generations = stamp.cell;
}
static int tex_stamp_current(tex_stamp stamp) {
    return stamp.cell && atomic_load_explicit(&stamp.cell->value, memory_order_acquire) == stamp.value;
}
typedef struct { uint64_t mem, size; uint32_t w, h, d, levels, target, samples, bpb, depth, fcls; } stor_key;   /* fcls: compressible storage only */
typedef struct { uint64_t k0, k1; VkImageView v; } view_ent;
typedef struct vk_img {
    VkImage img; vk_alloc mem; VkFormat fmt; int nvn_fmt; VkImageUsageFlags usage; VkImageType type;
    tex_stamp identity;
    uint32_t w, h, d, levels, layers, samples; VkImageAspectFlags aspect;
    int shared, refs; stor_key k; uint64_t idle_sub;
    int trace_selected; uint64_t trace_serial, trace_storage; /* populated only by the opt-in copy diagnostic */
    int wbrefs, orphan;   /* write-back pins are separate from actual texture-object references */
    view_ent *views; int nviews, capviews;
    int gpu_written;
    int arr3d;      /* 3D, created 2D_ARRAY_COMPATIBLE: its slices can be render targets */
    int blk_rt;     /* block-compressed, created BLOCK_TEXEL_VIEW_COMPATIBLE | EXTENDED_USAGE with attachment usage */
    VkFormat lst[2]; int nlst;   /* nlst > 0: created with this view-format list (keeps UBWC), not every format */
    struct vk_img *native, *scene_scratch; /* host siblings retire with this backing */
    struct vk_img *scene_aa[3]; /* crop-sized SMAA storage, same deferred retirement */
    int native_newer, native_valid, native_window, native_canvas;
    uint64_t native_ui_frame, scene_scaled_frame;
    uint64_t incarnation; /* distinguishes recycled VkImage handles/storage in shared canvas facts */
    uint32_t native_guest_w, native_guest_h; /* window allocation may exceed its active canvas/crop */
    struct vk_img *native_next; /* promoted-storage prefilter, protected by g_reg_mu */
    uint64_t native_storage, native_size;
} vk_img;
struct vk_tex {
    uint64_t serial; tex_t t; vk_img *im;
    tex_stamp identity;
    int mirrored; uint64_t stamp, csub;   /* mirrored: dirty-page stamp of the uploaded content, last submission checked */
    _Atomic uint64_t memo_sub;            /* TOTK_VK_TEX_MEMO: last submission whose tex_get_inner work for it is done */
    int cpu_pool;                         /* tex_mirrored at creation: GPU writes re-arm settled/opt-in invalidate write-back */
    int canvas_window;                   /* registration cached at object creation, not on every draw */
    VkBufferView tbv;                     /* TARGET_BUFFER textures */
    VkBuffer tb_buffer; VkDeviceSize tb_offset, tb_range;
};
static nvn_hmap g_by_serial = {.klen = 8};          /* serial -> vk_tex */
static nvn_hmap g_by_storage = {.klen = sizeof(stor_key)};   /* stor_key -> vk_img (shared) */
static nvn_hmap g_window_serials = {.klen = 8};
static _Atomic uint64_t g_native_extent = (uint64_t)1080 << 32 | 1920;
static uint64_t g_native_frame = 1;
/* Texture upload/texel resolution may acquire a retired arena while holding
   this lock; arena compaction re-enters only to invalidate its cached views. */
static pthread_mutex_t g_tex_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static uint64_t g_stat_uploads, g_stat_reuploads, g_stat_upload_bytes, g_stat_images, g_stat_shared_hits, g_stat_swept, g_stat_published, g_stat_switched;
/* GPU-written CPU-pool textures shared by storage (TOTK_VK_OFF=mirshare: off). A texture in a CPU-visible pool mirrors the
   pool's bytes into its own image (one per serial). When the GPU renders into one, the pool bytes are stale; another
   serial at the same storage with the same layout (the game re-initialises render-target objects at the storage that
   a persistent sampling object describes, as nvn_mtl.m's storage sharing notes) would read its own stale upload: the
   deferred lighting then read empty G-buffer textures and drew lit geometry black. So a GPU-written mirrored image is
   published under its storage key, and every serial at that storage reads it from then on. */
static int mirshare_on(void) { static int on = -1; if (on < 0) on = !vk_off("mirshare"); return on; }

static void wb_age_locked(void);   /* GPU write-back records (below) */
static void wb_sweep_locked(void);
static void native_index_remove(vk_img *im);
/* retired objects: destroyed once the GPU is past the submission that last could use them */
typedef struct { VkImage img; vk_alloc mem; view_ent *views; int nviews; VkBufferView bv; VkBuffer bv_buffer; void *host; uint64_t after; } retired_t;   /* host: a vk_img / vk_tex the replay thread may still hold */
static retired_t *g_retired; static int g_nretired, g_capretired;
static void retire(retired_t r) {
    if (g_nretired == g_capretired) { g_capretired = g_capretired ? 2 * g_capretired : 64; g_retired = realloc(g_retired, (size_t)g_capretired * sizeof *g_retired); }
    r.after = mtl_last_fence() + 1;   /* the submission being recorded now */
    g_retired[g_nretired++] = r;
}
/* The game finalises textures on its own threads while the replay thread may be recording with them, so the host
   objects go only when the GPU is past the submission being recorded (the replay thread drops them at each commit). */
static void texcopy_retire(vk_img *im, const char *event) {
    if (!totk_texcopy_on() || !im->trace_selected) return;
    totk_texcopy_log(vk_submit_serial(), "phase=res event=%s ser=%llu mem=%p image=%#llx shared=%d gpu_written=%d refs=%d",
        event, (unsigned long long)im->trace_serial, (void *)(uintptr_t)im->trace_storage,
        (unsigned long long)(uintptr_t)im->img, im->shared, im->gpu_written, im->refs);
}
static void retire_img(vk_img *im) {
    tex_stamp_drop(im->identity);
    texcopy_retire(im, "retire");
    native_index_remove(im);
    if (im->native) retire_img(im->native);
    if (im->scene_scratch) retire_img(im->scene_scratch);
    for (unsigned j=0;j<3;j++) if (im->scene_aa[j]) retire_img(im->scene_aa[j]);
    retire((retired_t){.img = im->img, .mem = im->mem, .views = im->views, .nviews = im->nviews, .host = im});
}
static void retire_bview(VkBufferView bv, VkBuffer buffer) { if (bv) retire((retired_t){.bv = bv, .bv_buffer = buffer}); }
static void retire_host(void *p) { if (p) retire((retired_t){.host = p}); }
/* Caller has retired every recording/GPU use of this arena buffer. Cached
   texel views must not survive its destruction or the lag-off cutover. */
void vk_res_buffer_released(VkBuffer buffer) {
    pthread_mutex_lock(&g_tex_mu);
    for (uint32_t i = 0; i < g_by_serial.cap; i++) {
        vk_tex *x = g_by_serial.vals ? g_by_serial.vals[i] : NULL;
        if (!x || x->tb_buffer != buffer) continue;
        if (x->tbv) vkDestroyBufferView(V.dev, x->tbv, NULL);
        x->tbv = VK_NULL_HANDLE; x->tb_buffer = VK_NULL_HANDLE;
        x->tb_offset = x->tb_range = 0;
    }
    for (int i = 0; i < g_nretired; ) {
        retired_t *r = &g_retired[i];
        if (r->bv_buffer != buffer) { i++; continue; }
        vkDestroyBufferView(V.dev, r->bv, NULL);
        g_retired[i] = g_retired[--g_nretired];
    }
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_res_sweep(void) {   /* nvn_vk.c, after each commit */
    uint64_t start = host_now_ns();
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nretired; ) {
        vk_rec_service_check();
        if (i >= g_nretired) break; /* a serviced readback may compact retired buffer views through slot reuse */
        retired_t *r = &g_retired[i];
        if (!mtl_wait(r->after, 0)) { i++; continue; }
        if (r->img && r->host) texcopy_retire(r->host, "destroy");
        for (int v = 0; v < r->nviews; v++) vkDestroyImageView(V.dev, r->views[v].v, NULL);
        free(r->views);
        if (r->img) vkDestroyImage(V.dev, r->img, NULL);
        vk_mem_free(&r->mem);
        if (r->bv) vkDestroyBufferView(V.dev, r->bv, NULL);
        free(r->host);
        g_retired[i] = g_retired[--g_nretired];
    }
    wb_sweep_locked();
    wb_age_locked();
    /* shared images nobody holds for a while (the game re-creates per-frame textures at the same storage) */
    uint64_t sub = vk_submit_serial();
    for (uint32_t i = 0; i < g_by_storage.cap; i++) {
        vk_rec_service_check();
        vk_img *im = g_by_storage.vals ? g_by_storage.vals[i] : NULL;
        if (!im || im->refs || im->wbrefs || sub - im->idle_sub < 600) continue;
        texcopy_retire(im, "sweep");
        stor_key k = im->k; nvn_hm_del(&g_by_storage, &k); retire_img(im); i--; g_stat_swept++;   /* deletion shifts entries back */
    }
    pthread_mutex_unlock(&g_tex_mu);
    wb_diag_end(WB_D_SWEEP, start);
}

/* NVN compression (nvn_mtl.m stor_compressed, the 2026-10-15 frame-wide flashes): a compressible texture in a compressible
   pool keeps its contents compressed, and a clear only sets the compression state, so on hardware a descriptor reading
   the same storage in another format does not see such writes. When the game moves the linear depth to new memory, the
   deferred lighting keeps reading the old depth descriptor for a frame or two while the old memory already belongs to an
   RGBA8 target cleared to white: with one shared image every depth texel reads 0xFFFFFFFF = NaN and the lit geometry goes
   black for that frame. So compressible textures share an image only with the same format or its sRGB / linear twin;
   each format class keeps its own image (and content) at that storage. TOTK_VK_OFF=compalias shares across formats. */
static int compalias_on(void) { static int on = -1; if (on < 0) on = !vk_off("compalias"); return on; }
/* TOTK_VK_FMT_SPLIT=1 (diagnostic): no storage sharing across format classes at all, compressible or not. On hardware
   plain memory aliases across formats, so this can break intended reinterpretation; it tells whether any cross-format
   sharing is behind a picture problem. */
static int fmtsplit_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_FMT_SPLIT"); on = e && atoi(e); if (on) LOGV("TOTK_VK_FMT_SPLIT: textures share storage only within a format class"); } return on; }
static int stor_compressed(const tex_t *t) { return (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE); }
static uint32_t fmt_class(int nvn_fmt) {
    VkFormat f; VkComponentMapping sw;
    if (!vk_fmt(nvn_fmt, &f, &sw) || !f) return 0x80000000u | (uint32_t)nvn_fmt;
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: f = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_B8G8R8A8_SRGB: f = VK_FORMAT_B8G8R8A8_UNORM; break;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: f = VK_FORMAT_A8B8G8R8_UNORM_PACK32; break;
        case VK_FORMAT_R8_SRGB: f = VK_FORMAT_R8_UNORM; break;
        case VK_FORMAT_R8G8_SRGB: f = VK_FORMAT_R8G8_UNORM; break;
        default: break;
    }
    return (uint32_t)f + 1;
}
static uint64_t g_stat_compsplit, g_stat_xfmt;   /* compressible storage taken by another format class */
static nvn_hmap g_comp_owner = {.klen = sizeof(stor_key)};   /* key without fcls -> the last format class (as a pointer) */
static int stor_key_of(const tex_t *t, stor_key *k) {
    if (!t->mem || !t->L.size) return 0;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || bw != 1 || bh != 1) return 0;   /* block-compressed formats keep one image per serial, as on Metal */
    memset(k, 0, sizeof *k);
    k->mem = (uint64_t)(uintptr_t)t->mem; k->size = t->L.size; k->w = t->d.width ? t->d.width : 1; k->h = t->d.height ? t->d.height : 1;
    k->d = t->d.depth ? t->d.depth : 1; k->levels = t->d.levels ? t->d.levels : 1; k->target = (uint32_t)t->d.target;
    k->samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1; k->bpb = (uint32_t)bpb; k->depth = (uint32_t)vk_fmt_is_depth(t->d.format) | (uint32_t)vk_fmt_has_stencil(t->d.format) << 1;
    if ((compalias_on() && stor_compressed(t)) || fmtsplit_on()) k->fcls = fmt_class(t->d.format);
    return 1;
}
static int tex_mirrored(const tex_t *t) {   /* nvn_mtl.m tex_mirrored */
    if (!t || !t->mem) return 0;
    if (t->has_layout) return 1;
    const pool_t *p = t->pool;
    if (!p || p->host || !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)) return 0;
    if (t->d.samples > 1 || t->d.target == 10) return 0;
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return 0;
    return 1;
}
extern uint64_t g_frame_pub __attribute__((weak));
static int wb_tile_match(const tex_t *t);
static void texcopy_resource(const char *event, vk_tex *x) {
    if (x && wb_tile_match(&x->t))
        LOGV("[settledwb-tile-view] event=%s frame=%llu sub=%llu storage=%p bytes=%llu ser=%llu image=%#llx mirrored=%d cpu_pool=%d shared=%d gpu_written=%d",
            event, (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)vk_submit_serial(),
            (void *)x->t.mem, (unsigned long long)x->t.L.size, (unsigned long long)x->serial,
            (unsigned long long)(uintptr_t)(x->im ? x->im->img : VK_NULL_HANDLE), x->mirrored, x->cpu_pool,
            x->im ? x->im->shared : 0, x->im ? x->im->gpu_written : 0);
    if (!x || !totk_texcopy_match(&x->t)) return;
    if (x->im) {
        x->im->trace_selected = 1; x->im->trace_serial = x->serial;
        x->im->trace_storage = (uint64_t)(uintptr_t)x->t.mem;
    }
    totk_texcopy_log(vk_submit_serial(), "phase=res event=%s ser=%llu mem=%p image=%#llx mirrored=%d cpu_pool=%d shared=%d gpu_written=%d",
        event, (unsigned long long)x->serial, (void *)x->t.mem, (unsigned long long)(uintptr_t)(x->im ? x->im->img : VK_NULL_HANDLE),
        x->mirrored, x->cpu_pool, x->im ? x->im->shared : 0, x->im ? x->im->gpu_written : 0);
}
void vk_texcopy_image(const char *op, const char *side, const tex_t *t, vk_tex *x) {
    if (!totk_texcopy_on()) return;
    extern int dm_nvn_pin_has_serial(uint64_t serial) __attribute__((weak));
    extern int nvn_tex_live_at(const void *mem, tex_t *out) __attribute__((weak));
    tex_t live = {0}; int have_live = t && nvn_tex_live_at && nvn_tex_live_at(t->mem, &live), retired = 0;
    int pinned = t && dm_nvn_pin_has_serial ? dm_nvn_pin_has_serial(t->serial) : -1;
    int finalized = t ? totk_texcopy_finalized(t->serial) : -1;
    pthread_mutex_lock(&g_tex_mu);
    if (x && x->im) for (int i = 0; i < g_nretired; i++) if (g_retired[i].img == x->im->img) { retired = 1; break; }
    totk_texcopy_log(vk_submit_serial(), "phase=image op=%s side=%s ser=%llu mem=%p image=%#llx mirrored=%d cpu_pool=%d shared=%d gpu_written=%d pin=%d finalized=%d live_ser=%llu retired=%d",
        op, side, (unsigned long long)(t ? t->serial : 0), t ? (void *)t->mem : NULL,
        (unsigned long long)(uintptr_t)(x && x->im ? x->im->img : VK_NULL_HANDLE),
        x ? x->mirrored : 0, x ? x->cpu_pool : 0, x && x->im ? x->im->shared : 0, x && x->im ? x->im->gpu_written : 0,
        pinned, finalized,
        (unsigned long long)(have_live ? live.serial : 0), retired);
    pthread_mutex_unlock(&g_tex_mu);
}
static void image_layers(const tex_t *t, VkImageType *type, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *layers, uint32_t *samples) {
    *w = t->d.width ? t->d.width : 1; *h = t->d.height ? t->d.height : 1; *d = 1; *layers = 1; *samples = 1; *type = VK_IMAGE_TYPE_2D;
    switch (t->d.target) {
        case 0: *type = VK_IMAGE_TYPE_1D; *h = 1; break;
        case 2: *type = VK_IMAGE_TYPE_3D; *d = t->d.depth ? t->d.depth : 1; break;
        case 3: *type = VK_IMAGE_TYPE_1D; *layers = *h; *h = 1; break;
        case 4: *layers = t->d.depth ? t->d.depth : 1; break;
        case 5: *samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1; break;
        case 6: *samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1; *layers = t->d.depth ? t->d.depth : 1; break;
        case 8: *layers = 6; break;
        case 9: *layers = 6 * MAXV((t->d.depth ? t->d.depth : 1) / 6, 1u); break;   /* NVN depth already counts layer-faces (nvn_tex.c) */
    }
    if (*w > 16384) *w = 16384; if (*h > 16384) *h = 16384; if (*d > 2048) *d = 2048; if (*layers > 2048) *layers = 2048;
}
static void image_barrier_init(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}
/* UBWC (Adreno's lossless framebuffer compression) and mutable formats. Turnip on the Adreno 740 (a7xx gen2) turns UBWC off
   for a MUTABLE_FORMAT image unless a format list names formats of one compatibility class; every render target lost it
   (TU_DEBUG=perf) and each pass moved its attachments uncompressed. The game itself reinterprets almost nothing (logged
   in the world: an RGBA8 output viewed as sRGB, BC4 rendered through RGBA16UI), as on the Mac (nvn_mtl.m: 17473
   textures, 0 format views). So an image lists its own format and its sRGB/UNORM twin; a view of any other format
   upgrades the image (img_upgrade), like nvn_mtl.m tex_upgrade_pfv. TOTK_VK_UBWC=0 gives every image the full set. */
static int ubwc_lists_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_UBWC"); on = !(e && !strcmp(e, "0")); } return on; }
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB; case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB; case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32; case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}
static vk_img *img_create_ex(const tex_t *t, int all_formats) {
    VkFormat f; VkComponentMapping fs;
    if (!vk_fmt(t->d.format, &f, &fs) || !f) { RED(8, "texture format %#x has no Vulkan format (ser %llu %ux%u)", t->d.format, (unsigned long long)t->serial, t->d.width, t->d.height); return NULL; }
    VkFormatFeatureFlags ff = fmt_features(f);
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !(ff & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        RED(8, "texture format %#x (Vulkan %d) is not supported by this driver", t->d.format, f); return NULL;
    }
    vk_img *im = calloc(1, sizeof *im);
    image_layers(t, &im->type, &im->w, &im->h, &im->d, &im->layers, &im->samples);
    uint32_t biggest = MAXV(im->w, im->h); if (im->type == VK_IMAGE_TYPE_3D) biggest = MAXV(biggest, im->d);
    uint32_t allowed = 1; for (uint32_t v = biggest; v > 1; v >>= 1) allowed++;
    im->levels = t->d.levels ? t->d.levels : 1; if (im->levels > allowed) im->levels = allowed; if (im->samples > 1) im->levels = 1;
    im->fmt = f; im->nvn_fmt = t->d.format; im->aspect = vk_fmt_aspect(t->d.format);
    im->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) im->usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) im->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) im->usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) im->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = 0;
    int compressed; { int bw, bh; nvn_fmt_bpb(t->d.format, &bw, &bh); compressed = bw > 1 || bh > 1; }
    if (!compressed && !vk_fmt_is_depth(t->d.format) && t->d.format != 0x31) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   /* views of another format, same texel size */
    if (compressed) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   /* sRGB / UNORM views of the same blocks */
    /* TOTK renders into BC4 textures through an RGBA16UI view whose texel is one block (nvn_mtl.m blk_aux). Vulkan
       has that view natively; the image must allow attachment usage for the view's format. */
    int blk_rt = 0;
    if (compressed && im->samples == 1 && (im->type == VK_IMAGE_TYPE_2D || im->type == VK_IMAGE_TYPE_3D) && V.get_image_format_props) {
        VkImageFormatProperties ip;
        VkImageCreateFlags bf = flags | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (!V.get_image_format_props(V.pd, f, im->type, VK_IMAGE_TILING_OPTIMAL, im->usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, bf, &ip)) { flags = bf; blk_rt = 1; }
    }
    if (im->type == VK_IMAGE_TYPE_2D && im->w == im->h && im->layers >= 6 && im->samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (im->type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = im->type, .format = f, .extent = {im->w, im->h, im->d},
        .mipLevels = im->levels, .arrayLayers = im->layers, .samples = (VkSampleCountFlagBits)im->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = im->usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if ((flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && !compressed && !all_formats && ubwc_lists_on()) {
        im->lst[0] = f; im->nlst = 1; if ((im->lst[1] = srgb_twin(f))) im->nlst = 2;
        fl.viewFormatCount = (uint32_t)im->nlst; fl.pViewFormats = im->lst; ci.pNext = &fl;
    }
    if (blk_rt) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult r = vkCreateImage(V.dev, &ci, NULL, &im->img);
    if (r && ci.pNext) { ci.pNext = NULL; im->nlst = 0; r = vkCreateImage(V.dev, &ci, NULL, &im->img); }
    if (r && blk_rt) { blk_rt = 0; ci.usage = im->usage; ci.flags &= ~(VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT); r = vkCreateImage(V.dev, &ci, NULL, &im->img); }
    if (r && (ci.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) { ci.flags &= ~VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; r = vkCreateImage(V.dev, &ci, NULL, &im->img); }
    im->usage = ci.usage; im->blk_rt = blk_rt; im->arr3d = !!(ci.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT);
    if (r) { RED(8, "vkCreateImage %ux%ux%u fmt %d levels %u layers %u: %d", im->w, im->h, im->d, f, im->levels, im->layers, r); free(im); return NULL; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, im->img, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->mem)) { RED(8, "out of GPU memory for a %ux%u texture (%llu MiB)", im->w, im->h, (unsigned long long)(req.size >> 20)); vkDestroyImage(V.dev, im->img, NULL); free(im); return NULL; }
    VKCHECK(vkBindImageMemory(V.dev, im->img, im->mem.mem, im->mem.off));
    image_barrier_init(vk_pre_cb(), im->img, im->aspect);
    g_stat_images++;
    im->identity = tex_stamp_new();
    im->incarnation = g_stat_images; /* lifetime identity must also exist when canvas/framegen diagnostics are off */
    totk_ft_count(FT_COUNT_TEX_REALIZATIONS, 1);
    return im;
}
static vk_img *img_create(const tex_t *t) { return img_create_ex(t, 0); }
static void tex_epoch_bump(void);   /* every replacement invalidates ID lookups and recorder freezes */
static uint64_t g_stat_image_replacements;
/* A view of a format outside the image's list: the image is made again with every format allowed (UBWC off), its
   contents copied over in command order, and the old image retired once the GPU is past it. The vk_img itself stays (shared
   images are held by several textures). Inside a render pass the copy cannot be recorded in order: it goes to the
   submission's pre buffer, so writes recorded since the last chunk submission (at most TOTK_VK_CHUNK_DRAWS draws) are not
   carried over. On the Mac this path was never taken in the world. */
static int img_upgrade_inner(vk_tex *x, VkFormat want) {
    vk_img *old = x->im, *nu = img_create_ex(&x->t, 1);
    if (!nu) return 0;
    if (nu->w != old->w || nu->h != old->h || nu->d != old->d || nu->levels != old->levels || nu->layers != old->layers) {
        RED(4, "texture upgrade for format views: size changed (ser %llu)", (unsigned long long)x->t.serial); vkDestroyImage(V.dev, nu->img, NULL); vk_mem_free(&nu->mem); tex_stamp_drop(nu->identity); free(nu); return 0;
    }
    int in_pass = vk_in_pass();
    VkCommandBuffer cb = in_pass ? vk_pre_cb() : vk_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vk_barrier_all(cb);
    VkImageCopy rg[16]; uint32_t n = MINV(old->levels, 16u);
    for (uint32_t l = 0; l < n; l++)
        rg[l] = (VkImageCopy){{old->aspect, l, 0, old->layers}, {0, 0, 0}, {old->aspect, l, 0, old->layers}, {0, 0, 0},
                              {MAXV(old->w >> l, 1u), MAXV(old->h >> l, 1u), old->type == VK_IMAGE_TYPE_3D ? MAXV(old->d >> l, 1u) : 1}};
    vkCmdCopyImage(cb, old->img, VK_IMAGE_LAYOUT_GENERAL, nu->img, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (totk_texcopy_match(&x->t))
        totk_texcopy_log(vk_submit_serial(), "phase=res event=image-upgrade ser=%llu old_image=%#llx new_image=%#llx in_pass=%d",
            (unsigned long long)x->serial, (unsigned long long)(uintptr_t)old->img, (unsigned long long)(uintptr_t)nu->img, in_pass);
    retire((retired_t){.img = old->img, .mem = old->mem, .views = old->views, .nviews = old->nviews});
    old->img = nu->img; old->mem = nu->mem; old->views = NULL; old->nviews = old->capviews = 0; old->nlst = 0;
    old->usage = nu->usage; old->blk_rt = nu->blk_rt; old->arr3d = nu->arr3d;
    tex_stamp_bump(&old->identity);
    tex_stamp_drop(nu->identity);
    tex_epoch_bump();
    g_stat_image_replacements++;
    for (unsigned level = 0; level < n; level++) {
        vk_pso_life_external_write(x, level, 0, old->layers, "image-upgrade");
        vk_pso_life_external_read(MAXV(old->w >> level, 1u), MAXV(old->h >> level, 1u), "image-upgrade-read");
    }
    free(nu);
    LOGV("%stexture %ux%u fmt %d made viewable as fmt %d (UBWC off for it; ser %llu); resource replacement invalidations %llu", in_pass ? "【红】inside a pass: " : "", old->w, old->h, (int)old->fmt, (int)want, (unsigned long long)x->t.serial, (unsigned long long)g_stat_image_replacements);
    return 1;
}
static int img_upgrade(vk_tex *x, VkFormat want) { return VK_REC_VALUE(VK_REC_S_RESOURCE, img_upgrade_inner(x, want)); }

/* Upload cost per present window, any switch value: count, bytes and wall time of upload_inner on its caller. */
static _Atomic uint64_t g_win_uploads, g_win_upload_bytes, g_win_upload_ns;
/* TOTK_VK_UPLOAD_THREADS=<1..3> (default unset/0 = serial): block-linear uploads of at least UPLOAD_POOL_MIN_BYTES
   deswizzle on that many helper threads plus the caller (vk_upload_pool.c); the copy is recorded after all runs.
   Live (runtime/live_ctl.h): each upload takes the current value before it starts; helpers are still started by the
   first parallel upload, and at 0 (or below their index) they sleep without work. */
static int g_upload_launch;
static totk_live_cache g_upload_live;
static _Atomic int g_upload_configured;
static pthread_mutex_t g_upload_config_mu = PTHREAD_MUTEX_INITIALIZER;
static void upload_threads_refresh(void) {
    if (atomic_load_explicit(&g_upload_configured, memory_order_relaxed) ==
        totk_live_cached(&g_upload_live, "TOTK_VK_UPLOAD_THREADS", g_upload_launch)) return;
    pthread_mutex_lock(&g_upload_config_mu);   /* re-read under the lock: the latest value is configured last */
    int n = totk_live_cached(&g_upload_live, "TOTK_VK_UPLOAD_THREADS", g_upload_launch);
    if (atomic_load_explicit(&g_upload_configured, memory_order_relaxed) != n) {
        upload_pool_configure(n); atomic_store_explicit(&g_upload_configured, n, memory_order_relaxed);
        LOGV("TOTK_VK_UPLOAD_THREADS=%d (live): from the next upload, %s", n,
             n ? "large block-linear uploads deswizzle on helpers too" : "every deswizzle on the calling thread");
    }
    pthread_mutex_unlock(&g_upload_config_mu);
}
void vk_upload_threads_init(void) {
    const char *e = getenv("TOTK_VK_UPLOAD_THREADS"); int n = e && *e ? atoi(e) : 0;
    n = n < 0 ? 0 : n > UPLOAD_POOL_MAX_HELPERS ? UPLOAD_POOL_MAX_HELPERS : n;
    g_upload_launch = n; atomic_store_explicit(&g_upload_configured, n, memory_order_relaxed);
    upload_pool_configure(n);
    if (n) LOGV("TOTK_VK_UPLOAD_THREADS=%d: block-linear uploads of %u KiB or more deswizzle on %d helper thread(s) plus the caller",
                n, UPLOAD_POOL_MIN_BYTES >> 10, n);
    else LOGV("TOTK_VK_UPLOAD_THREADS=0: texture deswizzle stays on the calling thread (default)");
}
void vk_upload_report(uint64_t frame) {
    upload_pool_stats s; upload_pool_stats_take(&s);
    uint64_t n = atomic_exchange(&g_win_uploads, 0), bytes = atomic_exchange(&g_win_upload_bytes, 0), ns = atomic_exchange(&g_win_upload_ns, 0);
    LOGV("frame %llu, texture uploads: %llu (%.1f MiB) in %.1f ms, %.3f ms/frame; parallel deswizzles %llu (%llu runs, %llu on helpers) (TOTK_VK_UPLOAD_THREADS=%d)",
         (unsigned long long)frame, (unsigned long long)n, bytes / 1048576.0, ns / 1e6, ns / 300e6,
         (unsigned long long)s.jobs, (unsigned long long)s.runs, (unsigned long long)s.helper_runs, upload_pool_helpers());
}
/* Upload a mirrored texture from the guest's bytes (pre command buffer). */
static void upload_inner(vk_tex *x) {
    const tex_t *t = &x->t; vk_img *im = x->im;
    if (totk_texcopy_match(t))
        totk_texcopy_log(vk_submit_serial(), "phase=res event=upload ser=%llu mem=%p image=%#llx guest_bytes=%llu guest_nonzero=%zu",
            (unsigned long long)t->serial, (void *)t->mem, (unsigned long long)(uintptr_t)im->img,
            (unsigned long long)t->L.size, totk_texcopy_nonzero(t->mem, (size_t)t->L.size));
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) return;
    x->stamp = nvn_dirty_now();   /* before the copy: a write during it is newer than this content */
    nvn_dirty_protect(t->mem, t->L.size);
    int is3d = im->type == VK_IMAGE_TYPE_3D, linear = nvn_tex_is_linear(&t->d);
    VkCommandBuffer cb = vk_pre_cb();
    vk_barrier_all(cb);
    if (linear && !is3d && im->layers == 1) {   /* pitch textures: the pool bytes are already rows */
        size_t stride = nvn_tex_linear_stride(&t->d); VkBuffer pb; uint64_t po;
        uint32_t w = im->w, h = im->h; size_t rows = (h + bh - 1) / bh;
        if (stride % bpb == 0 && vk_resolve((uint64_t)(uintptr_t)t->mem, stride * rows, &pb, &po) && po % 16 == 0) {
            VkDescriptorBufferInfo frozen;
            if (vk_lag_read((uint64_t)(uintptr_t)t->mem, stride * rows, 16, &frozen, VK_LAG_COPY)) { pb = frozen.buffer; po = frozen.offset; }
            VkBufferImageCopy c = {.bufferOffset = po, .bufferRowLength = (uint32_t)(stride / bpb * bw), .imageSubresource = {im->aspect, 0, 0, 1}, .imageExtent = {w, h, 1}};
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "upload");
            vkCmdCopyBufferToImage(cb, pb, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            vk_pso_life_external_write(x, 0, 0, 1, "upload");
            totk_ft_count(FT_COUNT_TEX_UPLOADS, 1);
            vk_barrier_all(cb); g_stat_upload_bytes += stride * rows; atomic_fetch_add(&g_win_upload_bytes, stride * rows);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            return;
        }
    }
    size_t total = 0;
    for (unsigned l = 0; l < im->levels; l++) {
        uint32_t w = MAXV(im->w >> l, 1u), h = MAXV(im->h >> l, 1u), z = is3d ? MAXV(im->d >> l, 1u) : 1;
        total += (((size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z + 15) & ~(size_t)15) * (is3d ? 1 : im->layers);
    }
    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(total ? total : 16, 16, &sb, &so);
    VkBufferImageCopy *regions = calloc((size_t)im->levels * im->layers, sizeof *regions); int nr = 0;
    /* With helpers, block-linear subresources are only collected here and decoded together below. */
    if (!linear) upload_threads_refresh();
    int pooled = !linear && upload_pool_helpers();
    upload_sub *subs = pooled ? malloc((size_t)im->levels * im->layers * sizeof *subs) : NULL; unsigned ns = 0;
    if (!subs) pooled = 0;
    size_t off = 0;
    for (unsigned l = 0; l < im->levels; l++) {
        uint32_t w = MAXV(im->w >> l, 1u), h = MAXV(im->h >> l, 1u), z = is3d ? MAXV(im->d >> l, 1u) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < (is3d ? 1u : im->layers); y++) {
            int bad;
            if (linear) { size_t stride = nvn_tex_linear_stride(&t->d); bad = 0; for (size_t r = 0; r < rows * z; r++) memcpy(stg + off + r * pitch, t->mem + r * stride, pitch); }
            else if (pooled) { subs[ns++] = (upload_sub){l, y, stg + off, pitch}; bad = 0; }
            else bad = nvn_deswizzle(&t->d, l, y, t->mem, stg + off, pitch);
            if (bad) { RED(8, "deswizzle failed: format %#x target %d", t->d.format, t->d.target); free(regions); return; }
            regions[nr++] = (VkBufferImageCopy){.bufferOffset = so + off, .imageSubresource = {im->aspect, l, y, 1}, .imageExtent = {w, h, z}};
            off += (pitch * rows * z + 15) & ~(size_t)15;
        }
    }
    if (pooled) {
        int bad = upload_pool_deswizzle(&t->d, t->mem, subs, ns, UPLOAD_POOL_MIN_BYTES);
        free(subs);
        if (bad) { RED(8, "deswizzle failed: format %#x target %d", t->d.format, t->d.target); free(regions); return; }
    }
    if (nr) {
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "upload");
        vkCmdCopyBufferToImage(cb, sb, im->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)nr, regions);
        for (unsigned level = 0; level < im->levels; level++)
            vk_pso_life_external_write(x, level, 0, im->layers, "upload");
        totk_ft_count(FT_COUNT_TEX_UPLOADS, 1);
    }
    free(regions);
    vk_barrier_all(cb);
    if (vk_gpuprof_on && nr) vk_gpuprof_scope_end(cb);
    g_stat_upload_bytes += total; atomic_fetch_add(&g_win_upload_bytes, total);
}
static void upload(vk_tex *x) {
    uint64_t started = host_now_ns();
    VK_REC_DO(VK_REC_O_UPLOAD, upload_inner(x));
    atomic_fetch_add(&g_win_uploads, 1); atomic_fetch_add(&g_win_upload_ns, host_now_ns() - started);
}

static void tex_drop_locked(vk_tex *x) {
    if (!x) return;
    tex_stamp_drop(x->identity);
    texcopy_resource("drop", x);
    if (x->tbv) retire_bview(x->tbv, x->tb_buffer);
    if (x->im) {
        if (x->im->shared) { if (--x->im->refs == 0) x->im->idle_sub = vk_submit_serial(); }
        else if (x->im->wbrefs) x->im->orphan = 1;   /* a rendering waiting for write-back keeps it */
        else retire_img(x->im);
    }
    retire_host(x);
}
static vk_tex *tex_get_inner(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    uint64_t sub = vk_submit_serial();
    if (x && x->im && x->mirrored && !x->im->gpu_written && x->csub != sub) {   /* once per submission: changed since the upload? */
        x->csub = sub;
        stor_key k; vk_img *g = NULL;
        if (mirshare_on() && stor_key_of(t, &k) && (g = nvn_hm_get(&g_by_storage, &k)) && g != x->im && g->gpu_written) {
            retire_img(x->im); x->im = g; g->refs++; __atomic_store_n(&x->mirrored, 0, __ATOMIC_RELAXED); g_stat_switched++;   /* the GPU renders this storage now */
            tex_stamp_bump(&x->identity);
            texcopy_resource("switch", x);
        } else if (!nvn_dirty_tracked(t->mem, t->L.size) || nvn_dirty_since(t->mem, t->L.size, x->stamp)) {
            texcopy_resource("reupload", x); upload(x); g_stat_reuploads++;
        }
    }
    /* TOTK_VK_TEX_MEMO: whatever this submission needs for x (above) is done; memo hits on mirrored textures wait for it. */
    if (x && vk_tex_memo_on() && atomic_load_explicit(&x->memo_sub, memory_order_relaxed) != sub)
        atomic_store_explicit(&x->memo_sub, sub, memory_order_release);
    if (x) { pthread_mutex_unlock(&g_tex_mu); return x->im || x->tbv ? x : NULL; }
    x = calloc(1, sizeof *x); x->serial = t->serial; x->t = *t; x->csub = sub; atomic_init(&x->memo_sub, sub);
    x->identity = tex_stamp_new();
    if (vk_canvas_on) x->canvas_window = nvn_hm_get(&g_window_serials, &t->serial) != NULL;
    nvn_hm_put(&g_by_serial, &t->serial, x);
    if (t->d.target == 10) { pthread_mutex_unlock(&g_tex_mu); return x; }   /* buffer textures: vk_tbuf_by_id */
    x->mirrored = x->cpu_pool = tex_mirrored(t);
    stor_key k;
    if (x->mirrored && mirshare_on() && stor_key_of(t, &k)) {   /* a GPU-written image already holds this storage */
        vk_img *g = nvn_hm_get(&g_by_storage, &k);
        if (g && g->gpu_written) {
            x->mirrored = 0; g->refs++; x->im = g; g_stat_shared_hits++;
            texcopy_resource("realize-shared-gpu-hit", x); pthread_mutex_unlock(&g_tex_mu); return x;
        }
    }
    if (!x->mirrored && stor_key_of(t, &k)) {
        vk_img *im = nvn_hm_get(&g_by_storage, &k);
        int storage_hit = im != NULL;
        if (im) { g_stat_shared_hits++;
            if (fmt_class(im->nvn_fmt) != fmt_class(t->d.format)) { uint64_t n = ++g_stat_xfmt;
                if (n <= 8) LOGV("storage %p (%ux%u) shared across formats: image NVN format %#x, texture %#x%s (%llu so far)", (void *)t->mem, t->d.width, t->d.height, im->nvn_fmt, t->d.format, stor_compressed(t) ? ", compressible" : "", (unsigned long long)n); } }
        else if ((im = img_create(t))) { im->shared = 1; im->k = k; nvn_hm_put(&g_by_storage, &k, im); if (k.fcls) {
            stor_key k0 = k; k0.fcls = 0; uintptr_t prev = (uintptr_t)nvn_hm_get(&g_comp_owner, &k0);
            if (prev && prev != k.fcls) { uint64_t n = ++g_stat_compsplit;
                if (n <= 8) LOGV("compressible storage %p (%ux%u) now NVN format %#x: it gets its own image, the other format keeps its content (%llu so far)", (void *)t->mem, t->d.width, t->d.height, t->d.format, (unsigned long long)n); }
            nvn_hm_put(&g_comp_owner, &k0, (void *)(uintptr_t)k.fcls);
        } }
        if (im) {
            im->refs++; x->im = im;
            texcopy_resource(storage_hit ? "realize-storage-share" : "realize-gpu-only-undefined", x);
        }
    } else {
        x->im = img_create(t);
        texcopy_resource(x->mirrored ? "realize-own-upload" : "realize-gpu-only-undefined", x);
        if (x->im && x->mirrored) { upload(x); g_stat_uploads++; }
    }
    pthread_mutex_unlock(&g_tex_mu);
    return x->im ? x : NULL;
}
vk_tex *vk_tex_get(const tex_t *t) { return VK_REC_VALUE(VK_REC_S_RESOURCE, tex_get_inner(t)); }
void mtl_xv_window_tex(uint64_t serial) {
    if (!vk_canvas_on) return;
    pthread_mutex_lock(&g_tex_mu);
    nvn_hm_put(&g_window_serials, &serial, (void *)1);
    vk_tex *x = nvn_hm_get(&g_by_serial, &serial);
    if (x) { x->canvas_window = 1; if (x->im) x->im->native_window = 1; }
    pthread_mutex_unlock(&g_tex_mu);
}

void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = nvn_hm_del(&g_by_serial, &t->serial);
    nvn_hm_del(&g_window_serials, &t->serial);
    tex_drop_locked(x);
    tex_epoch_bump();   /* cached id lookups may hold it (vk_tex_by_id) */
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_tex_pool_gone(uintptr_t lo, uintptr_t hi) {   /* nvn_vk.c: a pool was finalised: shared images over it are dropped */
    pthread_mutex_lock(&g_tex_mu);
    for (uint32_t i = 0; i < g_by_serial.cap; i++) {
        vk_tex *x = g_by_serial.vals ? g_by_serial.vals[i] : NULL;
        if (!x || (uintptr_t)x->t.mem >= hi || (uintptr_t)x->t.mem + x->t.L.size <= lo) continue;
        tex_stamp_bump(&x->identity); x->csub = 0; x->stamp = 0; atomic_store_explicit(&x->memo_sub, 0, memory_order_relaxed);
    }
    for (uint32_t i = 0; i < g_by_storage.cap; i++) {
        vk_img *im = g_by_storage.vals ? g_by_storage.vals[i] : NULL;
        if (!im || im->k.mem < lo || im->k.mem >= hi) continue;
        if (im->refs || im->wbrefs) continue;
        stor_key k = im->k; nvn_hm_del(&g_by_storage, &k); retire_img(im); i--;
    }
    pthread_mutex_unlock(&g_tex_mu);
    tex_epoch_bump();
}

VkImage vk_tex_image(vk_tex *x) { return x->im->img; }
VkImage vk_tex_native_image(vk_tex *x) { return x && x->im && x->im->native ? x->im->native->img : VK_NULL_HANDLE; }
vk_canvas_image vk_tex_canvas_identity(vk_tex *x, unsigned level, unsigned layer) {
    if (!x || !x->im) return (vk_canvas_image){0};
    vk_img *im=x->im;
    return (vk_canvas_image){.image=(uint64_t)(uintptr_t)im->img,.incarnation=im->incarnation,
        .storage=im->shared?im->k.mem:(uint64_t)(uintptr_t)x->t.mem,.size=im->shared?im->k.size:x->t.L.size,
        .serial=x->serial,.width=MAXV(im->w>>level,1u),.height=MAXV(im->h>>level,1u),
        .format=im->fmt,.level=level,.layer=layer};
}
uint64_t vk_tex_serial(vk_tex *x) { return x->serial; }
uint32_t vk_tex_samples(vk_tex *x) { return x->im->samples; }
VkFormat vk_tex_format(vk_tex *x) { return x->im->fmt; }
int vk_tex_nvn_format(vk_tex *x) { return x->im->nvn_fmt; }
uint32_t vk_tex_width(vk_tex *x, unsigned l) { return MAXV(x->im->w >> l, 1u); }
uint32_t vk_tex_height(vk_tex *x, unsigned l) { return MAXV(x->im->h >> l, 1u); }
uint32_t vk_tex_layers(vk_tex *x) { return x->im->layers; }
uint32_t vk_tex_levels(vk_tex *x) { return x->im->levels; }
int vk_tex_is3d(vk_tex *x) { return x->im->type == VK_IMAGE_TYPE_3D; }
VkImageAspectFlags vk_tex_aspect(vk_tex *x) { return x->im->aspect; }
void vk_end_rendering(void);
/* ---------- GPU write-back into CPU-visible pools ----------
   A VkImage does not alias NVN block-linear memory. TOTK bakes 64 terrain-normal tiles after load, then CPU-copies
   them ~9600 submissions later: the old invalidate-only records expired at 3000 and the copied normals were zero.
   Default-on settledwb stages only renderings whose producer completed and which were not rewritten for K submissions
   (TOTK_VK_SETTLED_WB_SUBS, default 60). Successful landings double the quiet interval for that pool/storage/layout,
   capped at 4096 submissions: intermittently rewritten feedback targets must not become periodic readbacks.
   Explicit invalidates bypass both learned and base delays: even first-use tiles can be CPU-read before K submissions.
   First-use storage still lands promptly. Pool lifetime/layout changes reset learning; CPU stores and new serials
   do not, since CPU-updated feedback rings would otherwise defeat backoff. A CPU write DOES immediately disarm the
   whole exact watch and cancels its record at the next commit, independently of that learned interval. It never
   waits for a write-back GPU ticket. Copies ride the normal stream.
   Explicit consumers execute through cooperative recording-owner service (idle try-lock, otherwise opcode/slot
   boundaries), never a blocking recording-lock acquisition behind unrelated walking work.
   TOTK_VK_WRITEBACK=1 retains blocking invalidate readback independently. Both paths share copies/layout/landing.
   One active rendering per storage range; newer image or buffer writes cancel any older staged result. */
typedef struct {
    tex_t t; vk_img *im; uint64_t stamp, sub, producer, pool, quiet, tile_version;
    VkBuffer buffer; VkDeviceMemory memory; uint8_t *bytes; uint64_t fence, allocation;
    void *watch; int cancelled, tracked, fresh_invalidate;
    uint64_t land_pos, land_src;
    unsigned land_level, land_layer;
} wb_ent;
enum wb_land_mode { WB_SYNC, WB_IDLE, WB_INVALIDATE, WB_INVALIDATE_TRY };
static wb_ent *g_wb; static int g_nwb, g_capwb;   /* g_tex_mu */
static _Atomic uint64_t g_wb_lo = UINT64_MAX, g_wb_hi; /* unlocked quick reject, conservative until sweep */
static uint64_t g_wb_done, g_wb_skip_cpu, g_wb_skip_fmt, g_wb_superseded, g_wb_bytes, g_wb_aged;
static uint64_t g_set_done, g_set_bytes, g_set_skip_cpu, g_set_superseded, g_set_staging, g_set_copies, g_set_invalidate;
static uint64_t g_set_fresh, g_set_fresh_bytes, g_set_fresh_wait_ns;
/* Learning owns metadata, never VkImages/staging. Exact keys survive temporary texture serials. Bounded to 4096
   live storage histories (<1 MiB including hash capacity), deleted with the pool; no image references are retained. */
typedef struct { uint64_t pool, mem, size; nvn_texdesc desc; } wb_hist_key;
typedef struct { uint64_t landed, quiet, last_sub; } wb_history;
static nvn_hmap g_wb_history = {.klen = sizeof(wb_hist_key)};
#define WB_HISTORY_MAX 4096u
#define WB_QUIET_MAX UINT64_C(4096)
#define WB_LAND_CHUNK UINT64_C(16384)
#define WB_SWEEP_BYTES UINT64_C(1048576)
#define WB_SWEEP_NS UINT64_C(2000000)
/* Read-only preflight index. Its lock protects ONLY these small rows, never recording, Vulkan calls,
   swizzling or texture/image lifetime work. Updates are owned by g_tex_mu; readers never acquire it. */
typedef struct { uint64_t address, size; } wb_lookup_ent;
static pthread_mutex_t g_wb_lookup_mu = PTHREAD_MUTEX_INITIALIZER;
static wb_lookup_ent *g_wb_lookup; static int g_wb_lookup_n, g_wb_lookup_cap;
static uint64_t g_wb_lookup_calls, g_wb_lookup_hits;
/* The rec-lock owner may detach records for synchronous landing, but consumers must still see them until it
   finishes. Borrow its EXISTING list: .t.mem/.t.L.size are immutable while published, no allocation/image ref.
   Writers hold g_tex_mu; readers hold only g_wb_lookup_mu. Publish before removing rows, clear before freeing. */
static const wb_ent *g_wb_landing; static int g_wb_landing_n;
static uint64_t g_wb_inflight_hits;
static void wb_landing_publish(const wb_ent *list, int n) {
    pthread_mutex_lock(&g_wb_lookup_mu);
    g_wb_landing = list; g_wb_landing_n = n;
    pthread_mutex_unlock(&g_wb_lookup_mu);
}
/* Always-on, bounded evidence for the 64 terrain-normal tiles. Unlike pending rows, these survive a landing/drop:
   invalidate must distinguish "already landed this GPU version" from "no record". No image refs or guest reads.
   The same short lookup mutex publishes snapshots; never take recording/resource locks just for diagnostics. */
enum { WB_TILE_MAX = 512 };
typedef struct {
    uint64_t address, size, pool, serial, image, version, write_sub, landed, land_sub, staged;
    uint64_t reported_version, reported_landed;
    const char *reported_reason;
    const char *reason;
} wb_tile;
static wb_tile g_wb_tiles[WB_TILE_MAX];
static unsigned g_wb_ntiles, g_wb_tile_next;
static uint64_t g_wb_tile_version, g_wb_tile_invalidates, g_wb_tile_stale;
static _Atomic uint64_t g_wb_tile_lo = UINT64_MAX, g_wb_tile_hi;
static int wb_tile_match(const tex_t *t) {
    return t->d.width == 260 && t->d.height == 260 && t->d.format == 0xd && t->d.levels == 9;
}
static void wb_tile_log(const wb_tile *d, const char *event, uint64_t sub, uint64_t address, uint64_t n) {
    LOGV("[settledwb-tile] event=%s frame=%llu sub=%llu storage=%p bytes=%llu pool=%llu ser=%llu image=%#llx gpu=%llu gpu_sub=%llu landed=%llu land_sub=%llu staged=%llu reason=%s current=%d invalidate_addr=%p invalidate_bytes=%llu",
        event, (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)sub,
        (void *)(uintptr_t)d->address, (unsigned long long)d->size, (unsigned long long)d->pool,
        (unsigned long long)d->serial, (unsigned long long)d->image, (unsigned long long)d->version,
        (unsigned long long)d->write_sub, (unsigned long long)d->landed, (unsigned long long)d->land_sub,
        (unsigned long long)d->staged, d->reason, d->landed == d->version,
        (void *)(uintptr_t)address, (unsigned long long)n);
}
static uint64_t wb_tile_note(const vk_tex *x) {
    if (!wb_tile_match(&x->t)) return 0;
    uint64_t address = (uintptr_t)x->t.mem, sub = vk_submit_serial();
    uint64_t pool = vk_wb_pool_identity(x->t.pool, x->t.mem, x->t.L.size);
    pthread_mutex_lock(&g_wb_lookup_mu);
    unsigned i = 0;
    for (; i < g_wb_ntiles; i++) if (g_wb_tiles[i].address == address && g_wb_tiles[i].pool == pool) break;
    if (i == g_wb_ntiles) {
        if (g_wb_ntiles < WB_TILE_MAX) g_wb_ntiles++;
        else i = g_wb_tile_next++ % WB_TILE_MAX;
        g_wb_tiles[i] = (wb_tile){0};
    }
    wb_tile *d = &g_wb_tiles[i];
    d->address = address; d->size = x->t.L.size; d->pool = pool; d->serial = x->t.serial;
    d->image = (uintptr_t)x->im->img; d->version = ++g_wb_tile_version; d->write_sub = sub;
    d->reason = "pending"; d->staged = 0;
    if (address < atomic_load(&g_wb_tile_lo)) atomic_store(&g_wb_tile_lo, address);
    if (address + d->size > atomic_load(&g_wb_tile_hi)) atomic_store(&g_wb_tile_hi, address + d->size);
    uint64_t version = d->version;
    pthread_mutex_unlock(&g_wb_lookup_mu);
    return version;
}
static void wb_tile_event(uint64_t version, const char *reason, int landed, int staged) {
    if (!version) return;
    wb_tile snapshot = {0};
    pthread_mutex_lock(&g_wb_lookup_mu);
    for (unsigned i = 0; i < g_wb_ntiles; i++) {
        wb_tile *d = &g_wb_tiles[i]; if (d->version != version) continue;
        d->reason = reason;
        if (landed) { d->landed = version; d->land_sub = vk_submit_serial(); }
        if (staged) d->staged = version;
        snapshot = *d; break;
    }
    pthread_mutex_unlock(&g_wb_lookup_mu);
    if (snapshot.version) wb_tile_log(&snapshot, reason, vk_submit_serial(), 0, 0);
}
static void wb_tile_invalidate(uint64_t address, uint64_t n, const char *event) {
    if (address >= atomic_load(&g_wb_tile_hi) || address + n <= atomic_load(&g_wb_tile_lo)) return;
    /* Bounded stack batches, even for whole-pool invalidates. No page access, allocation, drain or rec lock. */
    unsigned next = 0; int more;
    do {
        wb_tile snapshots[8]; unsigned count = 0;
        pthread_mutex_lock(&g_wb_lookup_mu);
        while (next < g_wb_ntiles && count < 8) {
            wb_tile *d = &g_wb_tiles[next++];
            if (d->address >= address + n || address >= d->address + d->size) continue;
            if (!strcmp(event, "invalidate-enter")) { g_wb_tile_invalidates++; g_wb_tile_stale += d->landed != d->version; }
            /* Repeated consumers of an unchanged version need only periodic counters, not per-frame logging. */
            if (d->reported_version == d->version && d->reported_landed == d->landed && d->reported_reason == d->reason) continue;
            d->reported_version = d->version; d->reported_landed = d->landed; d->reported_reason = d->reason;
            snapshots[count++] = *d;
        }
        more = next < g_wb_ntiles;
        pthread_mutex_unlock(&g_wb_lookup_mu);
        for (unsigned i = 0; i < count; i++) wb_tile_log(&snapshots[i], event, vk_submit_serial(), address, n);
    } while (more);
}
static void wb_lookup_set(int i, const wb_ent *e) {
    pthread_mutex_lock(&g_wb_lookup_mu);
    if (i == g_wb_lookup_cap) {
        g_wb_lookup_cap = g_wb_lookup_cap ? g_wb_lookup_cap * 2 : 64;
        g_wb_lookup = realloc(g_wb_lookup, (size_t)g_wb_lookup_cap * sizeof *g_wb_lookup);
    }
    g_wb_lookup[i] = (wb_lookup_ent){(uintptr_t)e->t.mem, e->cancelled ? 0 : e->t.L.size};
    if (i == g_wb_lookup_n) g_wb_lookup_n++;
    pthread_mutex_unlock(&g_wb_lookup_mu);
}
static void wb_lookup_remove(int i) {
    pthread_mutex_lock(&g_wb_lookup_mu);
    g_wb_lookup[i] = g_wb_lookup[--g_wb_lookup_n];
    if (!g_wb_lookup_n) { free(g_wb_lookup); g_wb_lookup = NULL; g_wb_lookup_cap = 0; }
    pthread_mutex_unlock(&g_wb_lookup_mu);
}
static int wb_invalidate_pending(uint64_t address, uint64_t n) {
    int found = 0, inflight = 0;
    pthread_mutex_lock(&g_wb_lookup_mu); g_wb_lookup_calls++;
    for (int i = 0; i < g_wb_lookup_n; i++) {
        const wb_lookup_ent *e = &g_wb_lookup[i];
        if (e->size && e->address < address + n && address < e->address + e->size) {
            found = 1; break;
        }
    }
    if (!found) for (int i = 0; i < g_wb_landing_n; i++) {
        const tex_t *t = &g_wb_landing[i].t; uint64_t m = (uintptr_t)t->mem;
        if (m < address + n && address < m + t->L.size) { found = inflight = 1; break; }
    }
    if (found) g_wb_lookup_hits++;
    if (inflight) g_wb_inflight_hits++;
    pthread_mutex_unlock(&g_wb_lookup_mu);
    if (inflight)
        LOGV("[settledwb-invalidate-inflight] frame=%llu sub=%llu invalidate_addr=%p invalidate_bytes=%llu waiting=serialized-landing",
            (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)vk_submit_serial(),
            (void *)(uintptr_t)address, (unsigned long long)n);
    return found;
}
static int wb_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_WRITEBACK"); on = e && atoi(e) > 0 && !vk_off("writeback"); } return on; }
static int settled_on(void) { static int on = -1; if (on < 0) on = !vk_off("settledwb"); return on; }
static uint64_t settled_subs(void) {
    static uint64_t subs;
    if (!subs) {
        const char *e = getenv("TOTK_VK_SETTLED_WB_SUBS"); char *end = NULL;
        unsigned long long n = e && *e != '-' ? strtoull(e, &end, 10) : 0;
        subs = n && end && !*end ? n : 60;
        LOGV("settled write-back: enabled, quiet submissions %llu, backoff cap %llu (TOTK_VK_OFF=settledwb disables)",
            (unsigned long long)subs, (unsigned long long)MAXV(subs, WB_QUIET_MAX));
        LOGV("settled write-back features: backoff=%d invalidate=%d drain=%d seal=%d cpu-disarm=%d; sweep budget 1 MiB/2 ms, chunks 16 KiB",
            backoff_on(), invalidate_on(), drain_on(), seal_on(), cpu_disarm_on());
    }
    return subs;
}
static wb_hist_key wb_history_key(const tex_t *t, uint64_t pool) {
    wb_hist_key key = {.pool = pool, .mem = (uintptr_t)t->mem, .size = t->L.size, .desc = t->d};
    key.desc.swizzle_r = key.desc.swizzle_g = key.desc.swizzle_b = key.desc.swizzle_a = key.desc.swizzle_pad = 0;
    return key;
}
static void wb_history_empty(void) {
    if (g_wb_history.n) return;
    free(g_wb_history.keys); free(g_wb_history.vals);
    g_wb_history = (nvn_hmap){.klen = sizeof(wb_hist_key)};
}
static uint64_t wb_quiet_for(const tex_t *t, uint64_t pool) {
    if (!settled_on()) return 0;
    if (!backoff_on()) return settled_subs();
    wb_hist_key key = wb_history_key(t, pool);
    wb_history *h = nvn_hm_get(&g_wb_history, &key);
    return h ? h->quiet : settled_subs();
}
static wb_history *wb_history_landed(const wb_ent *e) {
    if (!backoff_on()) return NULL;
    wb_hist_key key = wb_history_key(&e->t, e->pool);
    wb_history *h = nvn_hm_get(&g_wb_history, &key);
    if (!h) {
        if (g_wb_history.n == WB_HISTORY_MAX) {
            uint32_t oldest = 0; uint64_t sub = UINT64_MAX;
            for (uint32_t i = 0; i < g_wb_history.cap; i++) {
                wb_history *candidate = g_wb_history.vals[i];
                if (candidate && candidate->last_sub < sub) { sub = candidate->last_sub; oldest = i; }
            }
            wb_hist_key gone; memcpy(&gone, g_wb_history.keys + oldest * sizeof gone, sizeof gone);
            free(nvn_hm_del(&g_wb_history, &gone));
        }
        h = calloc(1, sizeof *h); nvn_hm_put(&g_wb_history, &key, h);
    }
    uint64_t cap = MAXV(settled_subs(), WB_QUIET_MAX);
    h->landed++; h->quiet = e->quiet < cap / 2 ? e->quiet * 2 : cap;
    h->last_sub = vk_submit_serial();
    return h;
}
static void wb_detail(const wb_ent *e, const wb_history *h, enum wb_land_mode mode) {
    static int on = -1; static uint64_t window = UINT64_MAX; static unsigned lines;
    if (on < 0) { const char *v = getenv("TOTK_VK_SETTLED_WB_DETAIL"); on = v && !strcmp(v, "1"); }
    if (!on) return;
    uint64_t frame = &g_frame_pub ? g_frame_pub : 0, interval = frame ? frame / 300 : vk_submit_serial() / 300;
    if (interval != window) { window = interval; lines = 0; }
    if (lines++ >= 64) return;
    LOGV("[settledwb] frame=%llu sub=%llu ser=%llu storage=%p pool=%llu %ux%ux%u fmt=%#x levels=%u layers=%u bytes=%llu storage_landed=%llu quiet=%llu next_quiet=%llu age=%llu image_bytes=%llu refs=%d wbrefs=%d cause=%s",
        (unsigned long long)frame, (unsigned long long)vk_submit_serial(), (unsigned long long)e->t.serial,
        (void *)e->t.mem, (unsigned long long)e->pool, e->t.d.width, e->t.d.height, e->t.d.depth, e->t.d.format,
        e->t.d.levels, e->t.L.layers, (unsigned long long)e->t.L.size, (unsigned long long)(h ? h->landed : 0),
        (unsigned long long)e->quiet, (unsigned long long)(h ? h->quiet : settled_subs()), (unsigned long long)(vk_submit_serial() - e->sub),
        (unsigned long long)e->im->mem.size, e->im->refs, e->im->wbrefs, mode == WB_IDLE ? "idle" : "invalidate");
}
static void wb_ref(vk_img *im) { im->wbrefs++; }
static void wb_unref(vk_img *im) {
    im->wbrefs--;
    if (im->shared) { if (!im->refs) im->idle_sub = vk_submit_serial(); }
    else if (!im->wbrefs && im->orphan) retire_img(im);
}
static void wb_staging_free(wb_ent *e) {
    if (!e->buffer) return;
    uint64_t start = host_now_ns();
    vkUnmapMemory(V.dev, e->memory); vkDestroyBuffer(V.dev, e->buffer, NULL);
    vk_memory_release(e->memory, VK_MEMORY_READBACK); g_set_staging -= e->allocation;
    e->buffer = VK_NULL_HANDLE; e->memory = VK_NULL_HANDLE; e->bytes = NULL; e->allocation = e->fence = 0;
    wb_diag_end(WB_D_FREE, start);
}
static void wb_ent_drop_locked(int i) {
    wb_lookup_remove(i);
    nvn_dirty_wb_unwatch(g_wb[i].watch);
    wb_staging_free(&g_wb[i]); wb_unref(g_wb[i].im);
    g_wb[i] = g_wb[--g_nwb];
}
static void wb_cancel_locked(int i) {
    g_wb[i].cancelled = 1;
    wb_lookup_set(i, &g_wb[i]);
    nvn_dirty_wb_unwatch(g_wb[i].watch); g_wb[i].watch = NULL;
    /* The GPU may still own the copy destination. Keep cancelled staging and its image only until that fence. */
    if (!g_wb[i].fence || mtl_wait(g_wb[i].fence, 0)) wb_ent_drop_locked(i);
}
static int wb_overlap(const wb_ent *e, uint64_t a, uint64_t n) {
    uint64_t m = (uint64_t)(uintptr_t)e->t.mem;
    return m < a + n && a < m + e->t.L.size;
}
static void *wb_watch_arm(const tex_t *t, uint64_t stamp) {
    if (cpu_disarm_on()) return nvn_dirty_wb_watch(t->mem, t->L.size, stamp);
    nvn_dirty_protect_exact(t->mem, t->L.size); return NULL;
}
static int wb_watch_tracked(const tex_t *t, void *watch) {
    return cpu_disarm_on() ? watch != NULL : nvn_dirty_tracked(t->mem, t->L.size);
}
static void wb_note_locked(vk_tex *x) {
    const tex_t *t = &x->t;
    uint64_t tile_version = wb_tile_note(x);
    if ((!wb_on() && !settled_on()) || !x->cpu_pool || !x->im || !t->mem || !t->L.size) {
        wb_tile_event(tile_version, !x->cpu_pool ? "not-cpu-pool" : "disabled", 0, 0); return;
    }
    int bw, bh; if (!nvn_fmt_bpb(t->d.format, &bw, &bh) || t->d.target == 2 || t->d.samples > 1 || (x->im->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {
        wb_tile_event(tile_version, "unsupported", 0, 0); return;
    }
    uint64_t pool = vk_wb_pool_identity(t->pool, t->mem, t->L.size);
    if (!pool) { wb_tile_event(tile_version, "invalid-pool", 0, 0); return; }
    uint64_t stamp = nvn_dirty_now(), sub = vk_submit_serial();
    uint64_t m = (uint64_t)(uintptr_t)t->mem;
    if (m < g_wb_lo) g_wb_lo = m;
    if (m + t->L.size > g_wb_hi) g_wb_hi = m + t->L.size;
    for (int i = 0; i < g_nwb; ) {
        wb_ent *e = &g_wb[i];
        if (e->cancelled || (e->im != x->im && !wb_overlap(e, (uintptr_t)t->mem, t->L.size))) { i++; continue; }
        if (!e->fence && e->t.mem == t->mem) {
            if (e->pool != pool || e->im != x->im) e->quiet = wb_quiet_for(t, pool);
            if (e->im != x->im) { wb_ref(x->im); wb_unref(e->im); e->im = x->im; g_set_superseded++; }
            /* A hot GPU-only target retains its already-armed range; no per-write allocation/protection walk. */
            if (e->pool != pool || e->t.L.size != t->L.size || nvn_dirty_wb_written(e->watch)) {
                nvn_dirty_wb_unwatch(e->watch);
                e->watch = wb_watch_arm(t, stamp);
            }
            e->t = *t; e->stamp = stamp; e->sub = sub; e->producer = mtl_last_fence() + 1; e->pool = pool; e->tile_version = tile_version;
            e->tracked = wb_watch_tracked(t, e->watch); wb_lookup_set(i, e); return;
        }
        wb_tile_event(e->tile_version, "image-superseded", 0, 0);
        int before = g_nwb; g_set_superseded++; wb_cancel_locked(i);
        if (before == g_nwb) i++;
    }
    if (g_nwb == g_capwb) { g_capwb = g_capwb ? 2 * g_capwb : 64; g_wb = realloc(g_wb, (size_t)g_capwb * sizeof *g_wb); }
    wb_ref(x->im);
    g_wb[g_nwb++] = (wb_ent){.t = *t, .im = x->im, .stamp = stamp, .sub = sub, .producer = mtl_last_fence() + 1,
        .pool = pool, .quiet = wb_quiet_for(t, pool), .watch = wb_watch_arm(t, stamp), .tile_version = tile_version};
    g_wb[g_nwb - 1].tracked = wb_watch_tracked(t, g_wb[g_nwb - 1].watch);
    wb_lookup_set(g_nwb - 1, &g_wb[g_nwb - 1]);
}
/* GPU copies into guest storage are newer than any rendering covering it. */
void vk_wb_gpu_wrote(uint64_t a, uint64_t n) {
    if (!n || a >= atomic_load(&g_wb_hi) || a + n <= atomic_load(&g_wb_lo)) return;
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nwb; ) {
        if (!g_wb[i].cancelled && wb_overlap(&g_wb[i], a, n)) {
            wb_tile_event(g_wb[i].tile_version, "buffer-write", 0, 0);
            int before = g_nwb; g_wb_superseded++; g_set_superseded++; wb_cancel_locked(i);
            if (before == g_nwb) i++;
        } else i++;
    }
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_wb_range_gone(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nwb; ) {
        if (!g_wb[i].cancelled && wb_overlap(&g_wb[i], lo, hi - lo)) {
            wb_tile_event(g_wb[i].tile_version, "pool-gone", 0, 0);
            int before = g_nwb; wb_cancel_locked(i); if (before == g_nwb) i++;
        } else i++;
    }
    for (uint32_t i = 0; i < g_wb_history.cap; i++) {
        if (!g_wb_history.vals[i]) continue;
        wb_hist_key key; memcpy(&key, g_wb_history.keys + i * sizeof key, sizeof key);
        if (key.mem < hi && lo < key.mem + key.size) { free(nvn_hm_del(&g_wb_history, &key)); i--; }
    }
    wb_history_empty();
    pthread_mutex_unlock(&g_tex_mu);
}
typedef struct { unsigned levels, layers; int bw, bh, bpb; } wb_shape;
static wb_shape wb_layout(const wb_ent *e) {
    wb_shape s = {.levels = MINV(e->t.d.levels ? e->t.d.levels : 1u, e->im->levels),
        .layers = MINV(e->t.L.layers ? e->t.L.layers : 1u, e->im->layers)};
    s.bpb = nvn_fmt_bpb(e->t.d.format, &s.bw, &s.bh);
    if (nvn_tex_is_linear(&e->t.d)) s.levels = s.layers = 1;
    return s;
}
static size_t wb_level_size(const wb_ent *e, wb_shape s, unsigned l) {
    return ((size_t)((MAXV(e->im->w >> l, 1u) + s.bw - 1) / s.bw) * s.bpb *
        ((MAXV(e->im->h >> l, 1u) + s.bh - 1) / s.bh) + 15) & ~(size_t)15;
}
static int wb_cpu_newer(const wb_ent *e) {
    return e->tracked && ((cpu_disarm_on() && nvn_dirty_wb_written(e->watch)) ||
        !nvn_dirty_tracked(e->t.mem, e->t.L.size) || nvn_dirty_since(e->t.mem, e->t.L.size, e->stamp));
}
static void wb_cpu_detail(const wb_ent *e, const char *reason) {
    if (!e->tile_version) return;
    uintptr_t address = nvn_dirty_wb_cpu_address(e->watch), first = (uintptr_t)e->t.mem;
    LOGV("[settledwb-tile-cpu] reason=%s frame=%llu sub=%llu storage=%p bytes=%llu ser=%llu gpu=%llu cpu_address=%p cpu_inside=%d watch_written=%d dirty_since=%d tracked=%d gpu_stamp=%llu now=%llu",
        reason, (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)vk_submit_serial(),
        (void *)first, (unsigned long long)e->t.L.size, (unsigned long long)e->t.serial,
        (unsigned long long)e->tile_version, (void *)address, address >= first && address - first < e->t.L.size,
        nvn_dirty_wb_written(e->watch), nvn_dirty_since(e->t.mem, e->t.L.size, e->stamp), e->tracked,
        (unsigned long long)e->stamp, (unsigned long long)nvn_dirty_now());
    wb_tile_event(e->tile_version, reason, 0, 0);
}
static int wb_copy_inner(wb_ent *e) {
    wb_shape s = wb_layout(e); size_t total = 0;
    for (unsigned l = 0; l < s.levels; l++) total += wb_level_size(e, s, l) * s.layers;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, &e->buffer)) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, e->buffer, &req);
    int type = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    if (type < 0 || vk_memory_allocate(&mai, &e->memory, VK_MEMORY_READBACK)) {
        vkDestroyBuffer(V.dev, e->buffer, NULL); e->buffer = VK_NULL_HANDLE; return 0;
    }
    VKCHECK(vkBindBufferMemory(V.dev, e->buffer, e->memory, 0));
    VKCHECK(vkMapMemory(V.dev, e->memory, 0, VK_WHOLE_SIZE, 0, (void **)&e->bytes));
    e->allocation = req.size; g_set_staging += req.size;
    vk_tex alias = {.im = e->im}; vk_tex_native_read(&alias);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    size_t o = 0;
    for (unsigned l = 0; l < s.levels; l++) for (unsigned y = 0; y < s.layers; y++) {
        VkBufferImageCopy c = {.bufferOffset = o, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, y, 1},
            .imageExtent = {MAXV(e->im->w >> l, 1u), MAXV(e->im->h >> l, 1u), 1}};
        vkCmdCopyImageToBuffer(cb, e->im->img, VK_IMAGE_LAYOUT_GENERAL, e->buffer, 1, &c);
        vk_pso_life_external_read(c.imageExtent.width, c.imageExtent.height, "writeback-read");
        o += wb_level_size(e, s, l);
    }
    vk_barrier_all(cb); e->fence = mtl_last_fence() + 1;
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    return 1;
}
static int wb_copy(wb_ent *e) {
    uint64_t start = host_now_ns(); int result = wb_copy_inner(e);
    if (!result) wb_tile_event(e->tile_version, "copy-allocation-failed", 0, 0);
    wb_diag_end(WB_D_COPY, start); return result;
}
/* Returns 3 for partial progress, 2 to defer without waiting, 1 if complete, 0 if CPU-newer, -1 if invalid. */
static int wb_land_inner(wb_ent *e, enum wb_land_mode mode, uint64_t limit, uint64_t *advanced) {
    const tex_t *t = &e->t; int settled = mode != WB_SYNC; wb_shape s = wb_layout(e);
    uint64_t offset = 0, span = t->L.size;
    if (advanced) *advanced = 0;
    if (mode == WB_IDLE) {
        uint64_t level = nvn_tex_level_offset(&t->d, e->land_level);
        uint64_t end = e->land_level + 1 < s.levels ? nvn_tex_level_offset(&t->d, e->land_level + 1) : t->L.layer_size;
        offset = (uint64_t)e->land_layer * t->L.layer_size + level + e->land_pos;
        span = MINV(end - level - e->land_pos, MINV(limit, WB_LAND_CHUNK));
    }
    if (!vk_wb_pool_lock(e->pool, t->mem, t->L.size)) {
        wb_tile_event(e->tile_version, "pool-not-live", 0, 0); return -1;
    }
#ifdef TOTK_VK_TESTS
    if (settled) nvn_dirty_test_wb_phase(t->mem, NVN_DIRTY_TEST_WB_BEFORE_OWNERSHIP);
#endif
    void *token = NULL;
    if (mode == WB_IDLE || mode == WB_INVALIDATE_TRY) {
        if (!nvn_dirty_gpu_write_try_begin((void *)(t->mem + offset), span, &token)) { vk_wb_pool_unlock(); return 2; }
    } else if (e->tracked) {
        token = nvn_dirty_gpu_write_begin(t->mem, t->L.size);
        if (!token) { vk_wb_pool_unlock(); wb_tile_event(e->tile_version, "ownership-untracked", 0, 0); return -1; }
    }
#ifdef TOTK_VK_TESTS
    if (settled) nvn_dirty_test_wb_phase(t->mem, NVN_DIRTY_TEST_WB_BEFORE_FRESHNESS);
#endif
    /* Check AFTER taking the existing dirty page/unit ownership: a writer already in the fault handler either wins
       first (and its newer stamp rejects us) or waits until restoration. Keep the pool lifetime locked through end.
       CPU-visible physical pools have no private RW alias (totk_vpool_backing names the game's physical view), so this
       uses the synchronous write-back's temporary-RW protocol. Residual window: a fresh direct game store while these
       pages are temporarily RW can bypass the handler, including just BEFORE this freshness check. Only writers
       which fault while RO participate in dirty ownership; this is not strict exclusion of arbitrary stores. */
    int fresh = !(settled && !e->tracked) && !wb_cpu_newer(e);
#ifdef TOTK_VK_TESTS
    if (settled) nvn_dirty_test_wb_phase(t->mem, NVN_DIRTY_TEST_WB_AFTER_FRESHNESS);
#endif
    if (!settled && totk_texcopy_match(t))
        totk_texcopy_log(vk_submit_serial(), "phase=res event=invalidate-freshness ser=%llu image=%#llx result=%s",
            (unsigned long long)t->serial, (unsigned long long)(uintptr_t)e->im->img, fresh ? "gpu-authoritative" : "skip-cpu-newer");
    int ok = 1;
    if (fresh) do {
        unsigned l = e->land_level;
        uint64_t level = nvn_tex_level_offset(&t->d, l);
        uint64_t end = l + 1 < s.levels ? nvn_tex_level_offset(&t->d, l + 1) : t->L.layer_size;
        uint64_t n = mode == WB_IDLE ? span : end - level - e->land_pos;
        size_t pitch = (size_t)((MAXV(e->im->w >> l, 1u) + s.bw - 1) / s.bw) * s.bpb;
        if (nvn_swizzle_span(&t->d, l, e->land_layer, e->bytes + e->land_src, pitch, (void *)t->mem, e->land_pos, n)) { ok = 0; break; }
        e->land_pos += n; if (advanced) *advanced += n;
        if (e->land_pos == end - level) {
            e->land_pos = 0; e->land_src += wb_level_size(e, s, l);
            if (++e->land_layer == s.layers) { e->land_layer = 0; e->land_level++; }
        }
        if (mode == WB_IDLE) break;
    } while (e->land_level < s.levels);
    nvn_dirty_gpu_write_end(token);
#ifdef TOTK_VK_TESTS
    if (settled) nvn_dirty_test_wb_phase(t->mem, NVN_DIRTY_TEST_WB_RESTORED);
#endif
    vk_wb_pool_unlock();
    if (!fresh) { wb_cpu_detail(e, "cpu-newer-land"); g_wb_skip_cpu++; if (settled) g_set_skip_cpu++; return 0; }
    if (!ok) { wb_tile_event(e->tile_version, "layout-error", 0, 0); if (++g_wb_skip_fmt <= 8) RED(4, "GPU write-back: unsupported layout ser=%llu format %#x", (unsigned long long)t->serial, t->d.format); return -1; }
    if (e->land_level < s.levels) return 3;
    wb_tile_event(e->tile_version, mode == WB_IDLE ? "landed-idle" : "landed-invalidate", 1, 0);
    if (settled) {
        g_set_done++; g_set_bytes += t->L.size; g_set_invalidate += mode == WB_INVALIDATE || mode == WB_INVALIDATE_TRY;
        if (e->fresh_invalidate) { g_set_fresh++; g_set_fresh_bytes += t->L.size; }
        wb_history *h = wb_history_landed(e); wb_detail(e, h, mode);
    }
    else {
        g_wb_done++; g_wb_bytes += t->L.size; HK_ADD(wb, 1); HK_ADD(wb_bytes, t->L.size);
        if (s.bw > 1) { static int nbc; if (++nbc <= 8) LOGV("[writeback] block-compressed ser=%llu %ux%u nvnfmt=%#x lv%u written back (%d so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.levels, nbc); }
        if (g_wb_done <= 8 || g_wb_done % 500 == 0)
            LOGV("[writeback] ser=%llu %ux%u nvnfmt=%#x lv%u: GPU rendering written back to memory (%llu so far, %llu MiB)",
                (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.levels,
                (unsigned long long)g_wb_done, (unsigned long long)(g_wb_bytes >> 20));
    }
    return 1;
}
static int wb_land(wb_ent *e, enum wb_land_mode mode, uint64_t limit, uint64_t *advanced) {
    uint64_t start = host_now_ns(); int result = wb_land_inner(e, mode, limit, advanced);
    wb_diag_end(WB_D_LAND, start); return result;
}
void vk_wb_record(void) {
    if (!settled_on()) return;
    uint64_t sub = vk_submit_serial();
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_nwb; ) {
        wb_ent *e = &g_wb[i];
        /* CPU ownership cancels immediately, not at the learned deadline. The fault has already disarmed the
           whole watch; only in-flight staging may remain until its fence. Learning itself retains no protection. */
        if (cpu_disarm_on() && !e->cancelled && (wb_cpu_newer(e) || !e->tracked)) {
            wb_cpu_detail(e, e->tracked ? "cpu-newer-record" : "untracked");
            g_set_skip_cpu++; g_wb_skip_cpu++;
            int before = g_nwb; wb_cancel_locked(i); if (before == g_nwb) i++;
            continue;
        }
        if (e->cancelled || e->fence || sub - e->sub < e->quiet || !mtl_wait(e->producer, 0)) { i++; continue; }
        if (wb_cpu_newer(e) || !e->tracked) {
            wb_cpu_detail(e, e->tracked ? "cpu-newer-stage" : "untracked");
            g_set_skip_cpu++; wb_ent_drop_locked(i); continue;
        }
        if (wb_copy(e)) { g_set_copies++; wb_tile_event(e->tile_version, "staged", 0, 1); }
        i++;
    }
    pthread_mutex_unlock(&g_tex_mu);
}
static void wb_sweep_locked(void) {
    static int next;
    uint64_t start = host_now_ns(), bytes = 0, chunks = 0; int yielded = 0, work = 0, scans = 0, count = g_nwb;
    int i = g_nwb ? next % g_nwb : 0;
    while (g_nwb && scans < count) {
        /* Cooperative wall/byte budget, checked between bounded destination spans and cleanup calls.
           It cannot preempt one driver syscall or OS deschedule; the separate WB-sweep/free maxima expose those. */
        if (work && (bytes + 512 > WB_SWEEP_BYTES || host_now_ns() - start >= WB_SWEEP_NS)) {
            yielded = 1; i = (i + 1) % g_nwb; break;
        }
        wb_ent *e = &g_wb[i];
        if (!e->fence || !mtl_wait(e->fence, 0)) { i = (i + 1) % g_nwb; scans++; continue; }
        uint64_t advanced = 0;
        int result = e->cancelled ? 0 : wb_land(e, WB_IDLE, (WB_SWEEP_BYTES - bytes) & ~UINT64_C(511), &advanced);
        bytes += advanced; chunks += advanced != 0;
        if (result == 3) { work++; continue; } /* resume this image unless this sweep runs out of budget */
        scans++;
        if (result == 2) { i = (i + 1) % g_nwb; continue; }
        wb_ent_drop_locked(i); work++;
        if (g_nwb && i >= g_nwb) i = 0;
    }
    next = i; wb_diag_sweep(start, bytes, chunks, yielded);
}
static void wb_age_locked(void) {
    uint64_t sub = vk_submit_serial();
    for (int i = 0; i < g_nwb; )
        if (!settled_on() && !g_wb[i].fence && sub - g_wb[i].sub > 3000) {
            wb_tile_event(g_wb[i].tile_version, "aged-sync-only", 0, 0); wb_ent_drop_locked(i); g_wb_aged++;
        } else i++;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < g_nwb; i++) { uint64_t m = (uintptr_t)g_wb[i].t.mem; if (m < lo) lo = m; if (m + g_wb[i].t.L.size > hi) hi = m + g_wb[i].t.L.size; }
    for (int i = 0; i < g_wb_landing_n; i++) {
        uint64_t m = (uintptr_t)g_wb_landing[i].t.mem;
        if (m < lo) lo = m;
        if (m + g_wb_landing[i].t.L.size > hi) hi = m + g_wb_landing[i].t.L.size;
    }
    g_wb_lo = lo; g_wb_hi = hi;
    if (!g_nwb) { free(g_wb); g_wb = NULL; g_capwb = 0; }
}
static uint64_t wb_image_bytes(const vk_img *im) {
    return im->mem.size + (im->native ? wb_image_bytes(im->native) : 0) +
        (im->scene_scratch ? wb_image_bytes(im->scene_scratch) : 0) +
        (im->scene_aa[0] ? wb_image_bytes(im->scene_aa[0]) : 0) +
        (im->scene_aa[1] ? wb_image_bytes(im->scene_aa[1]) : 0) +
        (im->scene_aa[2] ? wb_image_bytes(im->scene_aa[2]) : 0);
}
static void wb_memory_locked(uint64_t v[5]) {
    v[0] = (uint64_t)g_capwb * sizeof *g_wb;
    pthread_mutex_lock(&g_wb_lookup_mu);
    v[0] += (uint64_t)g_wb_lookup_cap * sizeof *g_wb_lookup;
    pthread_mutex_unlock(&g_wb_lookup_mu);
    v[1] = (uint64_t)g_wb_history.cap * (sizeof(wb_hist_key) + sizeof(void *)) +
        (uint64_t)g_wb_history.n * sizeof(wb_history);
    v[2] = v[3] = 0; v[4] = g_wb_history.n;
    for (int i = 0; i < g_nwb; i++) {
        vk_img *im = g_wb[i].im; int duplicate = 0;
        for (int j = 0; j < i; j++) if (g_wb[j].im == im) { duplicate = 1; break; }
        if (duplicate) continue;
        uint64_t bytes = wb_image_bytes(im); v[2] += bytes;
        if (im->shared ? !im->refs : im->orphan) v[3] += bytes;
    }
}
static void wb_diag_report(void) {
    static uint64_t previous[WB_OP_COUNT], previous_lookups, previous_hits;
    static const char *const names[] = {"invalidate", "drain", "owner-delay", "seal", "copy-wait", "land", "copy-record", "resource-sweep", "staging-free", "WB-sweep"};
    uint64_t total[WB_OP_COUNT], delta[WB_OP_COUNT], lookups, hits;
    for (int i = 0; i < WB_OP_COUNT; i++) {
        total[i] = atomic_load_explicit(&g_wb_ops[i], memory_order_relaxed);
        delta[i] = total[i] - previous[i]; previous[i] = total[i];
    }
    pthread_mutex_lock(&g_wb_lookup_mu);
    lookups = g_wb_lookup_calls; hits = g_wb_lookup_hits;
    pthread_mutex_unlock(&g_wb_lookup_mu);
    pthread_mutex_lock(&g_wb_diag_mu);
    __typeof__(g_wb_diag) d = g_wb_diag;
    memset(&g_wb_diag, 0, sizeof g_wb_diag); g_wb_diag.peak = d.peak; g_wb_diag.land_peak = d.land_peak;
    pthread_mutex_unlock(&g_wb_diag_mu);
    LOGV("settled write-back stalls: max %.3f ms phase=%s tid=%llu thread=%s role=%s, peak %.3f ms; drain/owner-delay/seal/wait/land/copy/sweep/staging-free %.3f/%.3f/%.3f/%.3f/%.3f/%.3f/%.3f/%.3f ms; WB sweep max %.3f ms (peak %.3f), bytes max %llu, chunks max %llu, budget yields %llu; WB submissions %llu (total %llu), seals %llu, drains %llu, owner-calls %llu, preflight hits/lookups %llu/%llu",
        d.max / 1e6, d.max ? names[d.kind] : "none", (unsigned long long)d.tid,
        *d.thread ? d.thread : "-", *d.role ? d.role : "-", d.peak / 1e6,
        d.phase[WB_D_DRAIN] / 1e6, d.phase[WB_D_LOCK] / 1e6, d.phase[WB_D_SEAL] / 1e6,
        d.phase[WB_D_WAIT] / 1e6, d.phase[WB_D_LAND] / 1e6, d.phase[WB_D_COPY] / 1e6,
        d.phase[WB_D_SWEEP] / 1e6, d.phase[WB_D_FREE] / 1e6, d.phase[WB_D_WB_SWEEP] / 1e6, d.land_peak / 1e6,
        (unsigned long long)d.land_bytes, (unsigned long long)d.land_chunks, (unsigned long long)d.land_yields,
        (unsigned long long)delta[WB_OP_SUBMIT], (unsigned long long)total[WB_OP_SUBMIT],
        (unsigned long long)delta[WB_OP_SEAL], (unsigned long long)delta[WB_OP_DRAIN], (unsigned long long)delta[WB_OP_LOCK],
        (unsigned long long)(hits - previous_hits), (unsigned long long)(lookups - previous_lookups));
    previous_lookups = lookups; previous_hits = hits;
}
void vk_wb_stats(void) {
    wb_diag_report(); /* called every 300 presents, including the settledwb-off control */
    if (!settled_on()) return;
    static uint64_t previous[6], previous_frame, previous_faults, previous_dirty;
    pthread_mutex_lock(&g_tex_mu);
    uint64_t now[6] = {g_set_done, g_set_bytes, g_set_skip_cpu, g_set_superseded, g_set_copies, g_set_invalidate};
    uint64_t faults, preps, dirty, dirty_preps;
    nvn_dirty_wb_stats(&faults, &preps); nvn_dirty_stats(&dirty, &dirty_preps);
    uint64_t frame = &g_frame_pub ? g_frame_pub : 0, frames = frame > previous_frame ? frame - previous_frame : 0;
    if (g_nwb || g_set_staging || memcmp(previous, now, sizeof now)) {
        LOGV("settled write-back: landed %llu (%.3f MiB), pending %d, skipped %llu (CPU wrote since), superseded %llu, staging %llu bytes, copies %llu, at invalidate %llu, WB faults %llu (%.3f/frame), dirty faults %llu (%.3f/frame)",
            (unsigned long long)g_set_done, g_set_bytes / 1048576.0, g_nwb, (unsigned long long)g_set_skip_cpu,
            (unsigned long long)g_set_superseded, (unsigned long long)g_set_staging, (unsigned long long)g_set_copies,
            (unsigned long long)g_set_invalidate, (unsigned long long)faults,
            frames ? (double)(faults - previous_faults) / frames : 0,
            (unsigned long long)dirty, frames ? (double)(dirty - previous_dirty) / frames : 0);
        uint64_t memory[5]; wb_memory_locked(memory);
        LOGV("settled write-back memory: records %llu bytes, history %llu bytes/%llu storages, pinned %.3f MiB (WB-only %.3f MiB), staging %llu bytes",
            (unsigned long long)memory[0], (unsigned long long)memory[1], (unsigned long long)memory[4],
            memory[2] / 1048576.0, memory[3] / 1048576.0, (unsigned long long)g_set_staging);
    }
    if (g_set_fresh || g_set_fresh_wait_ns)
        LOGV("settled write-back invalidate fresh landings: %llu (%llu bytes), copy-wait %.3f ms cumulative",
            (unsigned long long)g_set_fresh, (unsigned long long)g_set_fresh_bytes, g_set_fresh_wait_ns / 1e6);
    pthread_mutex_lock(&g_wb_lookup_mu);
    uint64_t inflight = g_wb_inflight_hits, versions = g_wb_tile_version, invalidates = g_wb_tile_invalidates, stale = g_wb_tile_stale;
    unsigned tiles = g_wb_ntiles;
    pthread_mutex_unlock(&g_wb_lookup_mu); /* Never keep the preflight metadata lock across log I/O. */
    if (inflight) LOGV("settled write-back inflight consumers: %llu cumulative", (unsigned long long)inflight);
    if (tiles)
        LOGV("settled write-back tiles: tracked %u, GPU versions %llu, invalidates %llu, stale-at-entry %llu",
            tiles, (unsigned long long)versions, (unsigned long long)invalidates, (unsigned long long)stale);
    previous_frame = frame; previous_faults = faults; previous_dirty = dirty;
    memcpy(previous, now, sizeof now); pthread_mutex_unlock(&g_tex_mu);
}
#ifdef TOTK_VK_TESTS
uint64_t vk_wb_test_inflight(void) {
    pthread_mutex_lock(&g_wb_lookup_mu); uint64_t n = g_wb_inflight_hits; pthread_mutex_unlock(&g_wb_lookup_mu); return n;
}
void vk_wb_test_fresh(uint64_t v[3]) {
    pthread_mutex_lock(&g_tex_mu);
    v[0] = g_set_fresh; v[1] = g_set_fresh_bytes; v[2] = g_set_fresh_wait_ns;
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_wb_test_tile(const tex_t *t, uint64_t v[6]) {
    memset(v, 0, 6 * sizeof *v); pthread_mutex_lock(&g_wb_lookup_mu);
    for (unsigned i = 0; i < g_wb_ntiles; i++) if (g_wb_tiles[i].address == (uintptr_t)t->mem) {
        const wb_tile *d = &g_wb_tiles[i]; if (d->version < v[0]) continue;
        v[0] = d->version; v[1] = d->landed; v[2] = d->write_sub; v[3] = d->serial;
    }
    v[4] = g_wb_tile_invalidates; v[5] = g_wb_tile_stale; pthread_mutex_unlock(&g_wb_lookup_mu);
}
void vk_wb_test_stats(uint64_t v[7]) {
    pthread_mutex_lock(&g_tex_mu);
    v[0] = g_set_done; v[1] = g_set_bytes; v[2] = (uint64_t)g_nwb; v[3] = g_set_skip_cpu;
    v[4] = g_set_superseded; v[5] = g_set_staging; v[6] = g_set_copies;
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_wb_test_memory(uint64_t v[5]) {
    pthread_mutex_lock(&g_tex_mu); wb_memory_locked(v); pthread_mutex_unlock(&g_tex_mu);
}
void vk_wb_test_history(const tex_t *t, uint64_t v[2]) {
    pthread_mutex_lock(&g_tex_mu);
    wb_hist_key key = wb_history_key(t, vk_wb_pool_identity(t->pool, t->mem, t->L.size));
    wb_history *h = nvn_hm_get(&g_wb_history, &key);
    v[0] = h ? h->landed : 0; v[1] = h ? h->quiet : settled_subs();
    pthread_mutex_unlock(&g_tex_mu);
}
uint64_t vk_wb_test_invalidates(void) {
    pthread_mutex_lock(&g_tex_mu); uint64_t n = g_set_invalidate; pthread_mutex_unlock(&g_tex_mu); return n;
}
void vk_wb_test_sync(uint64_t v[7]) {
    pthread_mutex_lock(&g_wb_lookup_mu); v[0] = g_wb_lookup_calls; v[1] = g_wb_lookup_hits; pthread_mutex_unlock(&g_wb_lookup_mu);
    for (int i = 0; i < WB_OP_COUNT; i++) v[2 + i] = atomic_load(&g_wb_ops[i]);
    pthread_mutex_lock(&g_wb_diag_mu); v[6] = g_wb_diag.peak; pthread_mutex_unlock(&g_wb_diag_mu);
}
void vk_wb_test_sweep(uint64_t v[6]) {
    pthread_mutex_lock(&g_wb_diag_mu);
    v[0] = g_wb_diag.phase[WB_D_WB_SWEEP]; v[1] = g_wb_diag.land_peak;
    v[2] = g_wb_diag.land_bytes; v[3] = g_wb_diag.land_chunks;
    v[4] = g_wb_diag.land_yields; v[5] = g_wb_diag.land_calls;
    pthread_mutex_unlock(&g_wb_diag_mu);
}
#endif
static void wb_seal(int full) {
    uint64_t start = host_now_ns(), before = mtl_last_fence();
    atomic_fetch_add_explicit(&g_wb_ops[WB_OP_SEAL], 1, memory_order_relaxed);
    if (full) vk_flush_wait(); else vk_submit_readback();
    atomic_fetch_add_explicit(&g_wb_ops[WB_OP_SUBMIT], mtl_last_fence() - before, memory_order_relaxed);
    wb_diag_end(WB_D_SEAL, start);
}
typedef struct { uint64_t address, n; int full, count; } wb_invalidate_request;
static int wb_invalidate_owner(void *arg, int boundary) {
    wb_invalidate_request *request = arg;
    uint64_t address = request->address, n = request->n, phase; int full = request->full;
    pthread_mutex_lock(&g_tex_mu);
    if (!boundary && !full) {
        uint64_t sealed = mtl_last_fence();
        for (int i = 0; i < g_nwb; i++) {
            wb_ent *e = &g_wb[i];
            /* A note at pass begin covers ALL its draws. Do not snapshot an unfinished same-image producer
               merely to meet a latency target. An unrelated already-sealed producer can be serviced between ops. */
            if (!e->cancelled && wb_overlap(e, address, n) && !e->fence && e->producer > sealed &&
                e->tracked && !wb_cpu_newer(e)) { pthread_mutex_unlock(&g_tex_mu); return 0; }
        }
    }

    int nl = 0; wb_ent *list = NULL;
    uint64_t fresh_before = g_set_fresh, fresh_bytes_before = g_set_fresh_bytes;
    for (int i = 0; i < g_nwb; i++) {
        wb_ent *e = &g_wb[i];
        if (e->cancelled || !wb_overlap(e, address, n)) continue;
        e->fresh_invalidate = !full && vk_submit_serial() - e->sub < settled_subs();
        if (totk_texcopy_match(&e->t))
            totk_texcopy_log(vk_submit_serial(), "phase=res event=invalidate-select ser=%llu mem=%p image=%#llx producer_sub=%llu",
                (unsigned long long)e->t.serial, (void *)e->t.mem, (unsigned long long)(uintptr_t)e->im->img, (unsigned long long)e->sub);
        list = realloc(list, (size_t)(nl + 1) * sizeof *list); list[nl++] = *e;
    }
    wb_landing_publish(list, nl);
    for (int i = 0; i < g_nwb; ) {
        if (g_wb[i].cancelled || !wb_overlap(&g_wb[i], address, n)) { i++; continue; }
        wb_lookup_remove(i);
        g_wb[i] = g_wb[--g_nwb]; /* transfer reference/staging ownership after publishing the borrowed ranges */
    }
    uint64_t copy_fence = 0, fresh_wait_ns = 0; int fresh_copy = 0;
    for (int i = 0; i < nl; i++) {
        wb_ent *e = &list[i];
        if (wb_cpu_newer(e) || (!full && !e->tracked)) {
            wb_cpu_detail(e, e->tracked ? "cpu-newer-invalidate" : "untracked");
            g_wb_skip_cpu++; if (!full) g_set_skip_cpu++; e->cancelled = 1;
        } else if (!e->fence) {
            vk_end_rendering_at(VK_PASS_END_CPU_WAIT, 1);
            if (wb_copy(e)) { if (!full) g_set_copies++; wb_tile_event(e->tile_version, "staged-invalidate", 0, 1); }
        }
        fresh_copy |= e->fresh_invalidate && !e->cancelled;
        if (e->fence > copy_fence) copy_fence = e->fence;
    }
    pthread_mutex_unlock(&g_tex_mu);
    if (copy_fence) {
        /* With settledseal off, omit only OPTIONAL seals for an already-submitted copy. An unsubmitted
           copy still must queue. If future range-reader ownership prevents a nonblocking landing below,
           publish the open stream before taking the blocking token, so the switch cannot create a deadlock. */
        if (full || seal_on() || copy_fence > mtl_last_fence()) wb_seal(full);
        if (!full) {
            /* The copy follows its producer in stream/queue order. Waiting this exact copy ticket includes that
               producer, not unrelated later work, and requires no full GPU idle even for an age-zero consumer. */
            phase = host_now_ns(); vk_wait_fence(copy_fence);
            fresh_wait_ns = fresh_copy ? host_now_ns() - phase : 0; wb_diag_end(WB_D_WAIT, phase);
        }
    }
    pthread_mutex_lock(&g_tex_mu);
    g_set_fresh_wait_ns += fresh_wait_ns;
    for (int i = 0; i < nl; i++) {
        if (!list[i].cancelled && list[i].fence && mtl_wait(list[i].fence, 0)) {
            int landed = wb_land(&list[i], full ? WB_SYNC : seal_on() ? WB_INVALIDATE : WB_INVALIDATE_TRY, 0, NULL);
            if (landed == 2) {
                pthread_mutex_unlock(&g_tex_mu); wb_seal(0); pthread_mutex_lock(&g_tex_mu);
                wb_land(&list[i], WB_INVALIDATE, 0, NULL);
            }
        }
        wb_staging_free(&list[i]); wb_unref(list[i].im);
        nvn_dirty_wb_unwatch(list[i].watch);
    }
    if (g_set_fresh != fresh_before || fresh_wait_ns)
        LOGV("[settledwb-invalidate-fresh] frame=%llu sub=%llu invalidate_addr=%p invalidate_bytes=%llu landed=%llu bytes=%llu copy_fence=%llu copy_wait_ms=%.3f",
            (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)vk_submit_serial(),
            (void *)(uintptr_t)address, (unsigned long long)n, (unsigned long long)(g_set_fresh - fresh_before),
            (unsigned long long)(g_set_fresh_bytes - fresh_bytes_before), (unsigned long long)copy_fence, fresh_wait_ns / 1e6);
    wb_landing_publish(NULL, 0);
    wb_age_locked();
    request->count = nl;
    pthread_mutex_unlock(&g_tex_mu); free(list);
    return 1;
}
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    if (totk_texcopy_on() && p && p->mem && n) {
        tex_t selected;
        if (totk_texcopy_overlap((uintptr_t)p->mem + off, n, &selected))
            totk_texcopy_log(vk_submit_serial(), "phase=replay op=MemoryPoolInvalidateMappedRange ser=%llu addr=%p bytes=%llu writeback=%d settled=%d result=%s",
                (unsigned long long)selected.serial, (void *)(p->mem + off), (unsigned long long)n, wb_on(), settled_on(),
                wb_on() ? "scan-records" : settled_on() ? "scan-settled" : "disabled");
    }
    if (!p || !p->mem || !n || off > p->size || n > p->size - off) return;
    int full = wb_on();
    uint64_t address = (uintptr_t)p->mem + off;
    wb_tile_invalidate(address, n, "invalidate-enter");
    if (!full && (!settled_on() || !invalidate_on() ||
        address >= atomic_load(&g_wb_hi) || address + n <= atomic_load(&g_wb_lo))) return;
    /* A hull hit is NOT a rendering overlap. This tiny index lock never waits for the recording owner,
       resources or the GPU. Unrelated invalidates must not drain the asynchronous game frontend.
       Revalidate under the real locks after draining: a concurrent new GPU note can supersede this candidate. */
    if (!full && !wb_invalidate_pending(address, n)) return;
    uint64_t t0 = host_now_ns(), phase = t0;
    if (full || drain_on()) {
        extern void nvn_x_drain(void) __attribute__((weak));
        if (nvn_x_drain) { atomic_fetch_add_explicit(&g_wb_ops[WB_OP_DRAIN], 1, memory_order_relaxed); nvn_x_drain(); }
        wb_diag_end(WB_D_DRAIN, phase);
    }
    wb_invalidate_request request = {.address = address, .n = n, .full = full};
    atomic_fetch_add_explicit(&g_wb_ops[WB_OP_LOCK], 1, memory_order_relaxed);
    uint64_t delay = vk_rec_service_call(wb_invalidate_owner, &request);
    wb_diag_duration(WB_D_LOCK, delay);
    int nl = request.count;
    wb_tile_invalidate(address, n, "invalidate-return");
    if (nl) { HK_ADD(inval, 1); HK_ADD(inval_ns, host_now_ns() - t0); }
    wb_diag_end(WB_D_INVALIDATE, t0);
}
/* Diagnostic TOTK_VK_WRITE_RANGE=<lo hex>-<hi hex>: the first GPU write to each image whose texture storage overlaps
   [lo, hi), at any size or format (render target, copy destination, storage image), for finding what fills memory. */
static uint64_t g_wr_lo, g_wr_hi;
int vk_write_range_hit(uint64_t a, uint64_t n) {   /* TOTK_VK_WRITE_RANGE overlaps [a, a + n) (nvn_vk.c buffer copies too) */
    static int init;
    if (!init) { const char *e = getenv("TOTK_VK_WRITE_RANGE"); if (e) { char *d; g_wr_lo = strtoull(e, &d, 16); if (*d == '-') g_wr_hi = strtoull(d + 1, NULL, 16); } init = 1; }
    return g_wr_hi && a < g_wr_hi && a + n > g_wr_lo;
}
static void gpu_write_range_note(const vk_tex *x) {
    static unsigned n;
    uint64_t m = (uint64_t)(uintptr_t)x->t.mem;
    if (!vk_write_range_hit(m, x->t.L.size) || x->im->gpu_written || n >= 4000) return;
    n++;
    LOGV("[gpuwrite] fp=%llu ser=%llu mem=%#llx size=%#llx %ux%ux%u fmt=%#x lv=%u tgt=%d mirrored=%d cpu_pool=%d shared=%d%s",
         (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)x->t.serial, (unsigned long long)m, (unsigned long long)x->t.L.size,
         x->t.d.width, x->t.d.height, x->t.d.depth, x->t.d.format, x->t.d.levels, (int)x->t.d.target, x->mirrored, x->cpu_pool, x->im->shared, n == 4000 ? " (cap reached)" : "");
}
static void tex_mark_gpu_written_inner(vk_tex *x) {
    if (!x || !x->im) return;
    gpu_write_range_note(x);
    if (x->im->gpu_written && !x->mirrored && !x->cpu_pool && !wb_tile_match(&x->t)) return;
    pthread_mutex_lock(&g_tex_mu);
    x->im->gpu_written = 1;
    stor_key k;
    if (x->mirrored && !x->im->shared && mirshare_on() && stor_key_of(&x->t, &k) && !nvn_hm_get(&g_by_storage, &k)) {
        x->im->shared = 1; x->im->k = k; x->im->refs = 1; nvn_hm_put(&g_by_storage, &k, x->im); __atomic_store_n(&x->mirrored, 0, __ATOMIC_RELAXED); g_stat_published++;
        texcopy_resource("publish", x);
    }
    if (x->cpu_pool || wb_tile_match(&x->t)) wb_note_locked(x);
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_tex_mark_gpu_written(vk_tex *x) { VK_REC_DO(VK_REC_S_WRITE, tex_mark_gpu_written_inner(x)); }

/* ---------- views ---------- */
static VkImageViewType view_type_of(int target, const vk_img *im, uint32_t nlayers) {
    switch (target) {
        case 0: return VK_IMAGE_VIEW_TYPE_1D; case 2: return VK_IMAGE_VIEW_TYPE_3D; case 3: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case 4: case 6: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 8: return nlayers >= 6 && (im->w == im->h) ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        case 9: return nlayers >= 6 && nlayers % 6 == 0 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}
/* attach: 0 sampling (want_target: the shader's declared target, -1 = the texture's own), 1 attachment, 2 storage */
static VkImageView view_get(vk_tex *x, const view_t *v, int attach, int want_target) {
    vk_img *im = x->im; const tex_t *t = &x->t;
    int fmt = v && v->set_format ? v->format : t->d.format;
    VkFormat vf = im->fmt; VkComponentMapping fs = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    if (v && v->set_format) {
        int bw, bh, vbw, vbh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
        VkFormat f2;
        int blk = im->blk_rt && attach == 1 && vbpb == bpb && vbw == 1 && vbh == 1;   /* one texel per block */
        if (!vk_fmt(v->format, &f2, &fs) || !f2 || vbpb != bpb || (!blk && (vbw != bw || vbh != bh)) || vk_fmt_aspect(v->format) != vk_fmt_aspect(t->d.format)) { RED(16, "texture view format %#x refused on a %#x texture (ser %llu)", v->format, t->d.format, (unsigned long long)t->serial); return VK_NULL_HANDLE; }
        vf = f2;
        if (im->nlst && vf != im->lst[0] && !(im->nlst > 1 && vf == im->lst[1]) && !img_upgrade(x, vf)) return VK_NULL_HANDLE;
        if (vf != im->fmt) {   /* which reinterpretations the game uses decides which images could keep UBWC (Turnip on a7xx gen2) */
            static uint32_t seen[256]; static int nseen;
            uint32_t key = (uint32_t)im->fmt << 16 ^ (uint32_t)vf << 2 ^ (uint32_t)attach; int k = 0; while (k < nseen && seen[k] != key) k++;
            if (k == nseen && nseen < 256) { seen[nseen++] = key; LOGV("view format: %ux%u image fmt %d viewed as fmt %d (%s)", im->w, im->h, (int)im->fmt, (int)vf, attach == 1 ? "render target" : attach == 2 ? "storage" : "sampled"); }
        }
    } else vk_fmt(fmt, NULL, &fs);
    unsigned l0 = v && v->set_levels ? v->base_level : 0, nl = v && v->set_levels && v->nlevels ? v->nlevels : im->levels - MINV(l0, im->levels);
    unsigned s0 = v && v->set_layers ? v->min_layer : 0;
    /* layers of a 3D texture are its depth slices at the level: render targets of one slice (2D_ARRAY_COMPATIBLE);
       sampling always takes the whole volume */
    unsigned nslices = im->type == VK_IMAGE_TYPE_3D ? (attach == 1 && l0 < im->levels ? MAXV(im->d >> l0, 1u) : 1) : im->layers;
    if (im->type == VK_IMAGE_TYPE_3D && attach != 1) s0 = 0;
    if (im->type == VK_IMAGE_TYPE_3D && attach == 1 && !im->arr3d) { RED(8, "render target in a 3D texture slice: this driver has no 2D views of 3D images (ser %llu)", (unsigned long long)t->serial); return VK_NULL_HANDLE; }
    unsigned ns = v && v->set_layers && v->nlayers ? v->nlayers : nslices - MINV(s0, nslices);
    if (im->type == VK_IMAGE_TYPE_3D && attach != 1) ns = 1;
    if (l0 >= im->levels || !nl || l0 + nl > im->levels || s0 + ns > nslices) { RED(16, "invalid texture view (levels %u+%u of %u, layers %u+%u of %u, ser %llu)", l0, nl, im->levels, s0, ns, im->layers, (unsigned long long)t->serial); return VK_NULL_HANDLE; }
    int target = v && v->set_target ? v->target : t->d.target;
    VkImageViewType vt;
    VkComponentMapping sw = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageAspectFlags aspect = im->aspect;
    if (attach == 1) {   /* one level; the layers of the view (a layered pass) or one */
        nl = 1; vt = ns > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (im->type == VK_IMAGE_TYPE_1D) vt = ns > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    } else {
        if (want_target >= 0) {   /* the shader's declaration wins where the layout allows it (the Mac's tex_type_view) */
            if (want_target == 1 && (target == 4 || target == 8 || target == 9)) { target = 1; ns = 1; }
            else if (want_target == 4 && (target == 1 || target == 7 || target == 8 || target == 9)) target = 4;
            else if (want_target == 8 && target == 9) { target = 8; ns = 6; }
            else if (want_target == 9 && target == 8) target = 9;
            else if (want_target == 2 && target != 2) { RED(8, "a 3D texture slot holds a %d texture", target); return VK_NULL_HANDLE; }
        }
        vt = view_type_of(target, im, ns);
        if (vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D) ns = 1;
        if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ns = 6;
        if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ns -= ns % 6;
        if (s0 + ns > im->layers && im->type != VK_IMAGE_TYPE_3D) { RED(16, "texture view of %u layers from %u of %u (ser %llu)", ns, s0, im->layers, (unsigned long long)t->serial); return VK_NULL_HANDLE; }
        if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;   /* sampling a depth-stencil texture reads depth */
        if (attach == 0) {   /* NVN swizzle selectors over the format's own remap (nvn_mtl.m tex_swizzle_sel) */
            unsigned sel[4] = {t->d.swizzle_r, t->d.swizzle_g, t->d.swizzle_b, t->d.swizzle_a};
            if (v && v->set_swizzle) for (unsigned i = 0; i < 4; i++) sel[i] = v->swz[i];
            const VkComponentSwizzle fc[4] = {fs.r, fs.g, fs.b, fs.a};
            VkComponentSwizzle c[4];
            for (unsigned i = 0; i < 4; i++) c[i] = sel[i] == 0 ? VK_COMPONENT_SWIZZLE_ZERO : sel[i] == 1 ? VK_COMPONENT_SWIZZLE_ONE : sel[i] <= 5 ? fc[sel[i] - 2] : VK_COMPONENT_SWIZZLE_IDENTITY;
            if (!t->d.swizzle_r && !t->d.swizzle_g && !t->d.swizzle_b && !t->d.swizzle_a && !(v && v->set_swizzle)) { c[0] = fs.r; c[1] = fs.g; c[2] = fs.b; c[3] = fs.a; }   /* no selectors: the format's remap */
            sw = (VkComponentMapping){c[0], c[1], c[2], c[3]};
        }
    }
    if (vt == VK_IMAGE_VIEW_TYPE_3D && im->type != VK_IMAGE_TYPE_3D) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (im->type == VK_IMAGE_TYPE_3D && attach != 1) { vt = VK_IMAGE_VIEW_TYPE_3D; s0 = 0; ns = 1; }
    uint64_t k0 = (uint64_t)vf | (uint64_t)vt << 32 | (uint64_t)aspect << 40 | (uint64_t)(attach & 3) << 48, k1 = (uint64_t)l0 | (uint64_t)nl << 8 | (uint64_t)s0 << 16 | (uint64_t)ns << 28
        | (uint64_t)(sw.r & 7) << 40 | (uint64_t)(sw.g & 7) << 43 | (uint64_t)(sw.b & 7) << 46 | (uint64_t)(sw.a & 7) << 49;
    for (int i = 0; i < im->nviews; i++) if (im->views[i].k0 == k0 && im->views[i].k1 == k1) return im->views[i].v;
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = attach == 1 ? im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
               : attach == 2 ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT};
    if (!(im->usage & uci.usage)) { RED(16, "texture ser %llu cannot be %s (format %#x)", (unsigned long long)t->serial, attach == 1 ? "a render target" : attach == 2 ? "a storage image" : "sampled", t->d.format); return VK_NULL_HANDLE; }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, .image = im->img, .viewType = vt, .format = vf, .components = sw,
        .subresourceRange = {aspect, l0, nl, s0, ns}};
    VkImageView view;
    if (vkCreateImageView(V.dev, &ci, NULL, &view)) { RED(16, "vkCreateImageView failed (ser %llu fmt %d type %d)", (unsigned long long)t->serial, vf, vt); return VK_NULL_HANDLE; }
    if (im->nviews == im->capviews) { im->capviews = im->capviews ? 2 * im->capviews : 4; im->views = realloc(im->views, (size_t)im->capviews * sizeof *im->views); }
    im->views[im->nviews++] = (view_ent){k0, k1, view};
    return view;
}
/* Diagnostic identity comes from the actual cached Vulkan view, not a guessed registry range.
   No resource is created, content resolved or pass ended by this metadata query. */
int vk_tex_view_identity(vk_tex *x, VkImageView view, vk_canvas_image *image, VkImageSubresourceRange *range) {
    int found = 0;
    pthread_mutex_lock(&g_tex_mu);
    vk_img *images[2] = {x ? x->im : NULL, x && x->im ? x->im->native : NULL};
    for (unsigned k = 0; k < 2 && !found; k++) if (images[k]) {
        vk_img *im = images[k];
        for (int i = 0; i < im->nviews; i++) if (im->views[i].v == view) {
            uint64_t a = im->views[i].k0, b = im->views[i].k1;
            *range = (VkImageSubresourceRange){(VkImageAspectFlags)((a >> 40) & 255),
                (uint32_t)(b & 255), (uint32_t)((b >> 8) & 255),
                (uint32_t)((b >> 16) & 4095), (uint32_t)((b >> 28) & 4095)};
            *image = (vk_canvas_image){.image=(uint64_t)(uintptr_t)im->img, .incarnation=im->incarnation,
                .storage=im->shared ? im->k.mem : (uint64_t)(uintptr_t)x->t.mem,
                .size=im->shared ? im->k.size : x->t.L.size, .serial=x->serial,
                .width=im->w, .height=im->h, .format=(VkFormat)(uint32_t)a};
            found = 1; break;
        }
    }
    pthread_mutex_unlock(&g_tex_mu);
    return found;
}
/* TOTK_VK_FEEDBACK_CUT: the same cached view (format, type, aspect, swizzle, layers, base level) with only its first
   `levels` levels, cached beside it. The original view when it is unknown or already that short. */
VkImageView vk_tex_view_levels(vk_tex *x, VkImageView view, uint32_t levels) {
    VkImageView result = view;
    pthread_mutex_lock(&g_tex_mu);
    vk_img *im = x ? x->im : NULL;
    for (int i = 0; im && levels && i < im->nviews; i++) if (im->views[i].v == view) {
        uint64_t k0 = im->views[i].k0, k1 = im->views[i].k1;
        uint32_t l0 = (uint32_t)(k1 & 255), nl = (uint32_t)((k1 >> 8) & 255);
        if (levels >= nl) break;
        uint64_t cut = (k1 & ~(255ull << 8)) | (uint64_t)levels << 8;
        for (int j = 0; j < im->nviews; j++) if (im->views[j].k0 == k0 && im->views[j].k1 == cut) { result = im->views[j].v; goto done; }
        VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = (k0 >> 48 & 3) == 2 ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT};
        VkComponentMapping sw = {(VkComponentSwizzle)(k1 >> 40 & 7), (VkComponentSwizzle)(k1 >> 43 & 7),
            (VkComponentSwizzle)(k1 >> 46 & 7), (VkComponentSwizzle)(k1 >> 49 & 7)};
        VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, .image = im->img,
            .viewType = (VkImageViewType)(k0 >> 32 & 0xff), .format = (VkFormat)(uint32_t)k0, .components = sw,
            .subresourceRange = {(VkImageAspectFlags)(k0 >> 40 & 0xff), l0, levels, (uint32_t)(k1 >> 16 & 4095), (uint32_t)(k1 >> 28 & 4095)}};
        VkImageView v;
        if (vkCreateImageView(V.dev, &ci, NULL, &v)) { RED(16, "feedback-cut view failed (ser %llu)", (unsigned long long)x->serial); break; }
        if (im->nviews == im->capviews) { im->capviews = im->capviews ? 2 * im->capviews : 4; im->views = realloc(im->views, (size_t)im->capviews * sizeof *im->views); }
        im->views[im->nviews++] = (view_ent){k0, cut, v};
        result = v; break;
    }
done:
    pthread_mutex_unlock(&g_tex_mu);
    return result;
}
int vk_xlate_texture_view_type(vk_tex *x, VkImageView view) {
    int type = -1;
    pthread_mutex_lock(&g_tex_mu);
    vk_img *images[2] = {x ? x->im : NULL, x && x->im ? x->im->native : NULL};
    for (int k = 0; k < 2; k++) if (images[k]) {
        for (int i = 0; i < images[k]->nviews; i++) if (images[k]->views[i].v == view)
            type = (int)((images[k]->views[i].k0 >> 32) & 0xff);
    }
    pthread_mutex_unlock(&g_tex_mu);
    return type;
}
VkImageView vk_tex_view(vk_tex *x, const tex_t *t, const view_t *v, int attach) {
    if (!attach) vk_draw_texture_read(0,t);
    if (!attach) vk_tex_native_read(x);
    (void)t;
    pthread_mutex_lock(&g_tex_mu); VkImageView r = x && x->im ? view_get(x, v, attach, -1) : VK_NULL_HANDLE; pthread_mutex_unlock(&g_tex_mu);
    return r;
}

void vk_native_window_size(uint32_t w, uint32_t h) {
    if (w && h) atomic_store(&g_native_extent, (uint64_t)h << 32 | w);
}
void vk_native_frame_end(void) { if (vk_native_ui_enabled()) g_native_frame++; }
int vk_tex_canvas_window(vk_tex *x) {
    if (!vk_canvas_on || !x || !x->im) return 0;
    if (x->canvas_window) x->im->native_window = 1;
    return x->im->native_window;
}
int vk_tex_native_window(vk_tex *x) { return vk_native_ui_enabled() && vk_tex_canvas_window(x); }
void vk_tex_native_basis(vk_tex *x, uint32_t *w, uint32_t *h) {
    *w = x->im->native_guest_w ? x->im->native_guest_w : x->im->w;
    *h = x->im->native_guest_h ? x->im->native_guest_h : x->im->h;
}
void vk_tex_native_transfer(vk_tex *x, uint32_t w, uint32_t h) {
    if (!vk_native_ui_enabled() || !x || !x->im || !w || !h || w>x->im->w || h>x->im->h) return;
    if (x->im->native_guest_w == w && x->im->native_guest_h == h) return;
    vk_tex_native_read(x);
    x->im->native_guest_w=w; x->im->native_guest_h=h; x->im->native_valid=0;
    LOGV("native UI v2: window ser=%llu allocation=%ux%u active transfer=%ux%u",
        (unsigned long long)x->serial,x->im->w,x->im->h,w,h);
}
void vk_tex_native_promote(vk_tex *x) {
    if (!vk_native_ui_enabled() || !x || !x->im || x->im->native_canvas) return;
    x->im->native_canvas = 1;
    LOGV("native UI v2: final canvas ser=%llu backing=%p %ux%u fmt=%d (discovered through window transfer; native from first UI draw)",
        (unsigned long long)x->serial, (void *)x->t.mem, x->im->w, x->im->h, x->im->fmt);
}
void vk_tex_native_ui_begin(vk_tex *x) {
    if (x && x->im && x->im->native_canvas) x->im->native_ui_frame = g_native_frame;
}
int vk_tex_native_active(vk_tex *x) {
    return vk_native_ui_enabled() && x && x->im &&
        (vk_tex_native_window(x) || (x->im->native_canvas && x->im->native_ui_frame == g_native_frame));
}
/* Both representations are GENERAL, same format, single-level/layer colour.
   All synchronization is recorded on the walking owner's primary, never a worker. */
static void native_blit(vk_img *src, vk_img *dst, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, unsigned op) {
    VkCommandBuffer cb = vk_cb(); int scope = vk_native_gpu_begin(cb,op); vk_barrier_all(cb);
    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets = {{0,0,0},{(int32_t)sw,(int32_t)sh,1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets = {{0,0,0},{(int32_t)dw,(int32_t)dh,1}}};
    vkCmdBlitImage(cb,src->img,VK_IMAGE_LAYOUT_GENERAL,dst->img,VK_IMAGE_LAYOUT_GENERAL,1,&r,VK_FILTER_LINEAR);
    vk_pso_life_external_read(src->w, src->h, "native-image-read");
    vk_barrier_all(cb); vk_native_gpu_end(cb,scope);
}
void vk_tex_native_read(vk_tex *x) {
    if (!x || !x->im || !x->im->native_newer) return;
    /* Ending the pass can itself request diagnostic shadow reads. Publish the
       transition first to prevent reentrant resolves; the copy follows its end. */
    vk_img *im = x->im; im->native_newer = 0;
    if (vk_in_pass()) vk_end_rendering_at(VK_PASS_END_COPY, 0);
    uint32_t w,h; vk_tex_native_basis(x,&w,&h);
    native_blit(im->native, im, im->native->w, im->native->h, w, h, VK_NUI_RESOLVE); im->native_newer = 0;
    vk_pso_life_external_write(x, 0, 0, 1, "native-resolve");
}
void vk_tex_native_written(vk_tex *x, int native) {
    if (!x || !x->im) return;
    x->im->native_newer = native;
    x->im->native_valid = native;
}
void vk_tex_native_discard(vk_tex *x) {
    if (!x || !x->im) return;
    if (x->im->native_newer) vk_native_gpu_skip(VK_NUI_RESOLVE);
    vk_tex_native_written(x,0);
}
static void native_index_add(vk_tex *x);
static vk_post_output scene_image(vk_tex *owner, vk_img *im, int attach) {
    vk_tex host=*owner; host.im=im;
    host.t.d.format=im->nvn_fmt;
    view_t view={.set_levels=1,.nlevels=1,.set_layers=1,.nlayers=1,.set_swizzle=1,.swz={2,3,4,5}};
    pthread_mutex_lock(&g_tex_mu);
    VkImageView v=view_get(&host,&view,attach,-1);
    pthread_mutex_unlock(&g_tex_mu);
    return (vk_post_output){im->img,v,im->w,im->h,im->fmt};
}
static int native_scene_seed(vk_tex *x, vk_img *host, uint32_t w, uint32_t h) {
    if (!vk_scene_post_enabled() || !x->im->native_canvas || x->im->native_ui_frame!=g_native_frame ||
        x->im->scene_scaled_frame==g_native_frame) return 0;
    /* Later shadow resolves, partial uploads or 1x shader fallbacks can contain
       UI. Never filter those: only the first pre-UI seed owns AA/upscaling. */
    x->im->scene_scaled_frame=g_native_frame;
    vk_post_output source=scene_image(x,x->im,0),target=scene_image(x,host,1),scratch={0},aa[3]={0};
    if (!source.view || !target.view) return 0;
    if (vk_scene_scale_fsr()) {
        if (!host->scene_scratch) {
            tex_t t=x->t; t.d.width=host->w; t.d.height=host->h;
            host->scene_scratch=img_create(&t);
            if (!host->scene_scratch) return 0;
        }
        scratch=scene_image(x,host->scene_scratch,1);
    }
    if (vk_scene_aa_enabled()) for (unsigned j=0;j<3;j++) {
        vk_img **image=&host->scene_aa[j];
        if (*image && ((*image)->w!=w || (*image)->h!=h)) { retire_img(*image); *image=NULL; }
        if (!*image) {
            tex_t t=x->t; t.d.width=w; t.d.height=h;
            if (j<2) t.d.format=j==0?0x0d:0x25; /* RG8 edges / RGBA8 weights */
            *image=img_create(&t);
            if (!*image) return 0;
        }
        aa[j]=scene_image(x,*image,1);
    }
    VkCommandBuffer cb=vk_cb(); int scope=vk_native_gpu_begin(cb,VK_NUI_CANVAS_SEED);
    int scaled=vk_scene_post_run(cb,source.view,source.w,source.h,0,0,w,h,&target,&scratch,aa);
    vk_native_gpu_end(cb,scope);
    if (scaled) {
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        (void)vk_fg_capture_native_scene(cb,&target);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    return scaled;
}
static vk_img *native_prepare(vk_tex *x, int preserve) {
    if (!vk_native_ui_enabled() || !x || !x->im) return NULL;
    vk_img *im = x->im;
    if (vk_in_pass()) return im->native;
    if (im->type != VK_IMAGE_TYPE_2D || im->levels != 1 || im->layers != 1 || im->samples != 1 ||
        im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !(fmt_features(im->fmt) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return NULL;
    uint64_t extent = atomic_load(&g_native_extent); uint32_t ow = (uint32_t)extent, oh = extent >> 32;
    uint32_t gw,gh; vk_tex_native_basis(x,&gw,&gh);
    double scale = fmin((double)ow / gw, (double)oh / gh);
    uint32_t w = (uint32_t)(gw * scale + .5), h = (uint32_t)(gh * scale + .5);
    if (scale < 1 || (scale == 1 && !vk_scene_post_enabled()) || w > V.props.limits.maxImageDimension2D || h > V.props.limits.maxImageDimension2D) {
        if (preserve) vk_tex_native_read(x); else vk_tex_native_discard(x);
        return NULL;
    }
    if (im->native && (im->native->w != w || im->native->h != h)) {
        if (preserve) vk_tex_native_read(x); else vk_tex_native_discard(x);
        retire_img(im->native); im->native = NULL; im->native_valid = 0;
    }
    if (!im->native) {
        tex_t t = x->t; t.d.width = w; t.d.height = h;
        im->native = img_create(&t); if (!im->native) return NULL;
        LOGV("native UI v2: host sibling ser=%llu guest=%ux%u host=%ux%u fmt=%d",
            (unsigned long long)x->serial, im->w, im->h, w, h, im->fmt);
        native_index_add(x);
    }
    if (!im->native_valid) {
        unsigned op = im->native_window ? VK_NUI_WINDOW_SEED : VK_NUI_CANVAS_SEED;
        if (preserve) {
            if (!native_scene_seed(x,im->native,gw,gh)) native_blit(im,im->native,gw,gh,w,h,op);
        }
        else vk_native_gpu_skip(op);
        /* Without preservation, the caller immediately opens a whole-image
           load-clear, including on first use and after a host extent change. */
        im->native_valid = 1;
    }
    return im->native;
}
int vk_tex_native_backed(vk_tex *x) { return x && x->im && x->im->native; }
static VkImageView native_view(vk_tex *x, const view_t *view, int attach, int preserve) {
    vk_img *im = native_prepare(x,preserve); if (!im) return VK_NULL_HANDLE;
    vk_tex host = *x; host.im = im;
    pthread_mutex_lock(&g_tex_mu);
    VkImageView result = view_get(&host, view, attach, -1);
    pthread_mutex_unlock(&g_tex_mu);
    return result;
}
VkImageView vk_tex_native_view(vk_tex *x, const view_t *view, int attach) { return native_view(x,view,attach,1); }
VkImageView vk_tex_native_clear_view(vk_tex *x, const view_t *view) {
    VkImageView result = native_view(x,view,1,0);
    /* If the view cannot be made, the caller clears the original image. */
    if (!result) vk_tex_native_discard(x);
    return result;
}
int vk_tex_native_output(vk_tex *x, vk_post_output *out) {
    if (!x || !x->im || !x->im->native_valid) return 0;
    vk_img *im = native_prepare(x,1); if (!im) return 0;
    view_t view = {.set_swizzle=1,.swz={2,3,4,5}};
    VkImageView v = vk_tex_native_view(x,&view,0); if (!v) return 0;
    *out = (vk_post_output){im->img,v,im->w,im->h,im->fmt}; return 1;
}
/* A guest upload/copy updates only its transformed rectangle; never enlarge a
   1x readback over untouched sharp UI. The caller resolved before its write. */
void vk_tex_native_patch(vk_tex *x, unsigned level, unsigned layer, const NVNcopyRegion *r) {
    if (!x || !x->im) return;
    vk_img *im = x->im, *host = im->native;
    if (!host || !im->native_valid || !vk_tex_native_active(x) || level || layer || r->zoffset || r->depth > 1) {
        vk_tex_native_written(x,0); return;
    }
    uint32_t gw,gh; vk_tex_native_basis(x,&gw,&gh);
    int32_t sx0=MAXV(0,r->xoffset),sy0=MAXV(0,r->yoffset);
    int32_t sx1=MINV((int32_t)gw,r->xoffset+r->width),sy1=MINV((int32_t)gh,r->yoffset+r->height);
    if (sx1<=sx0 || sy1<=sy0) return; /* guest padding is not displayed */
    VkCommandBuffer cb = vk_cb(); int scope = vk_native_gpu_begin(cb,VK_NUI_PATCH); vk_barrier_all(cb);
    int32_t x0 = (int32_t)(((int64_t)sx0*host->w + gw/2)/gw);
    int32_t y0 = (int32_t)(((int64_t)sy0*host->h + gh/2)/gh);
    int32_t x1 = (int32_t)(((int64_t)sx1*host->w + gw/2)/gw);
    int32_t y1 = (int32_t)(((int64_t)sy1*host->h + gh/2)/gh);
    VkImageBlit bl = {.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{sx0,sy0,0},{sx1,sy1,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .dstOffsets={{x0,y0,0},{x1,y1,1}}};
    /* Upload/copy pixels are already filtered by their guest operation. A
       second linear filter would bleed texels OUTSIDE the copied rectangle. */
    if (x1>x0 && y1>y0) vkCmdBlitImage(cb,im->img,VK_IMAGE_LAYOUT_GENERAL,host->img,VK_IMAGE_LAYOUT_GENERAL,1,&bl,VK_FILTER_NEAREST);
    vk_barrier_all(cb); vk_native_gpu_end(cb,scope); vk_tex_native_written(x,1);
}

/* ---------- texture / sampler pools: id -> registered object ---------- */
static vk_treg *g_treg; static int g_ntreg; static _Atomic int g_tpool_n;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static VkSampler *g_sobj; static int g_nsreg;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
/* Registration, texture lifetime/mapping and shared-image replacement are
   independent identities. Only content readiness expires at submit: retaining a
   view never skips the existing mirrored dirty-page/upload check. */
enum { TID_GEN_N = 1 << 20 };   /* texture handles carry 20-bit ids */
static _Atomic uint32_t g_treg_gen[TID_GEN_N];
/* Shader signatures need only these two fields, not the reallocating registry entry.
   Publish each full-width pair atomically; a separate bitmap preserves all int values. */
static _Atomic uint64_t g_treg_kind[TID_GEN_N], g_treg_kind_ok[TID_GEN_N / 64];
static _Atomic uint64_t g_tex_epoch = 1;
static _Atomic uint64_t g_treg_revision = 1;   /* stage_sig's single check for all per-id generations and texture epochs */
uint64_t vk_treg_revision(void) { return atomic_load_explicit(&g_treg_revision, memory_order_acquire); }
/* Only descriptors overlapping a realized native sibling can require shadow
   work. Registration maintains this conservative bitmap; negative lookups never
   realize a texture, lock a map, upload memory, or split a game pass. */
static vk_img *g_native_index;
static _Atomic uint64_t g_native_tids[TID_GEN_N / 64];
static _Atomic uint64_t g_native_tid_revision;
uint64_t vk_native_descriptor_revision(void) { return atomic_load_explicit(&g_native_tid_revision, memory_order_acquire); }
int vk_native_descriptor(int tid) {
    return tid > 0 && tid < TID_GEN_N &&
        (atomic_load_explicit(&g_native_tids[(unsigned)tid / 64], memory_order_relaxed) >> (tid & 63) & 1);
}
static void native_index_tid(int tid, const tex_t *t) { /* g_reg_mu */
    if (tid <= 0 || tid >= TID_GEN_N) return;
    uint64_t a = t ? (uint64_t)(uintptr_t)t->mem : 0, n = t ? t->L.size : 0;
    int candidate = 0;
    for (vk_img *im = g_native_index; im && a && n; im = im->native_next)
        if (a < im->native_storage + im->native_size && im->native_storage < a + n) { candidate = 1; break; }
    _Atomic uint64_t *word = &g_native_tids[(unsigned)tid / 64];
    uint64_t bit = 1ull << (tid & 63), old = atomic_load_explicit(word, memory_order_relaxed);
    uint64_t next = candidate ? old | bit : old & ~bit;
    if (old != next) {
        atomic_store_explicit(word, next, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_native_tid_revision, 1, memory_order_release);
    }
}
static void native_index_refresh(void) { /* g_reg_mu; promotion/retirement only */
    for (int i = 1; i < g_ntreg; i++) native_index_tid(i, g_treg[i].ok ? &g_treg[i].t : NULL);
}
static void native_index_add(vk_tex *x) {
    vk_img *im = x->im;
    pthread_mutex_lock(&g_reg_mu);
    if (!im->native_size) {
        im->native_storage = (uint64_t)(uintptr_t)x->t.mem; im->native_size = x->t.L.size;
        im->native_next = g_native_index; g_native_index = im;
        native_index_refresh();
    }
    pthread_mutex_unlock(&g_reg_mu);
}
static void native_index_remove(vk_img *im) {
    if (!im->native_size) return;
    pthread_mutex_lock(&g_reg_mu);
    vk_img **p = &g_native_index; while (*p && *p != im) p = &(*p)->native_next;
    if (*p) { *p = im->native_next; native_index_refresh(); }
    im->native_next = NULL; im->native_size = 0;
    pthread_mutex_unlock(&g_reg_mu);
}
static void tex_epoch_bump(void) { atomic_fetch_add_explicit(&g_tex_epoch, 1, memory_order_release); atomic_fetch_add_explicit(&g_treg_revision, 1, memory_order_release); }
void mtl_set_tex_pool_n(int n) { atomic_store_explicit(&g_tpool_n, n, memory_order_release); tex_epoch_bump(); }
static _Atomic uint32_t g_samp_epoch = 1;
uint64_t vk_record_sampler_revision(void) { return atomic_load_explicit(&g_samp_epoch, memory_order_acquire); }
static int texcache_on(void) { static int on = -1; if (on < 0) on = !vk_off("texcache"); return on; }
static _Atomic uint64_t g_treg_calls, g_treg_unchanged;   /* TOTK_VK_TEX_MEMO: registrations, of those identical */
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    if (tid < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (tid >= g_ntreg) { int n = tid + 1024; g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    vk_treg *r = &g_treg[tid];
    /* TOTK_VK_TEX_MEMO: the same object (its serial is in tex_t) and view written again change no lookup result, so
       the revisions that retire every memo entry, stage signature and freeze key stay as they are. */
    if (vk_tex_memo_on()) {
        atomic_fetch_add_explicit(&g_treg_calls, 1, memory_order_relaxed);
        if (t ? r->ok && r->has_v == (v != NULL) && !memcmp(&r->t, t, sizeof *t) && (!v || !memcmp(&r->v, v, sizeof *v)) : !r->ok) {
            atomic_fetch_add_explicit(&g_treg_unchanged, 1, memory_order_relaxed);
            pthread_mutex_unlock(&g_reg_mu); return;
        }
    }
    r->ok = t != NULL; if (t) r->t = *t; r->has_v = v != NULL;
    if (v) r->v = *v; else memset(&r->v, 0, sizeof r->v);
    if (vk_native_ui_enabled()) native_index_tid(tid, t);
    if (tid < TID_GEN_N) {
        unsigned word = (unsigned)tid / 64; uint64_t bit = 1ull << (tid & 63);
        uint64_t ok = atomic_load_explicit(&g_treg_kind_ok[word], memory_order_relaxed);
        if (t) {
            int format = v && v->set_format ? v->format : t->d.format;
            int target = v && v->set_target ? v->target : t->d.target;
            atomic_store_explicit(&g_treg_kind[tid], (uint32_t)format | (uint64_t)(uint32_t)target << 32, memory_order_release);
        }
        atomic_store_explicit(&g_treg_kind_ok[word], t ? ok | bit : ok & ~bit, memory_order_release);
        atomic_fetch_add_explicit(&g_treg_gen[tid], 1, memory_order_release);
        atomic_fetch_add_explicit(&g_treg_revision, 1, memory_order_release);
    } else tex_epoch_bump();
    pthread_mutex_unlock(&g_reg_mu);
}
int vk_treg_get(int tid, vk_treg *out) {   /* g_treg grows by realloc on the game's threads: never hand out a pointer into it */
    pthread_mutex_lock(&g_reg_mu);
    int pool_n = atomic_load_explicit(&g_tpool_n, memory_order_acquire);
    int ok = tid > 0 && tid < g_ntreg && !(pool_n && tid >= pool_n) && g_treg[tid].ok;
    if (ok) *out = g_treg[tid];
    pthread_mutex_unlock(&g_reg_mu);
    return ok;
}
/* Registry writers hold g_reg_mu. A lookup can observe an older or newer registration,
   as before, but the pair cannot tear. Unregister leaves the old pair intact until reuse.
   Pool bounds are independent of object lifetime: releasing a realized image changes
   neither its descriptor's kind nor this table. No TLS/hash/epoch validation is needed. */
int vk_treg_kind(int tid, int *format, int *target) {
    if (tid <= 0 || tid >= TID_GEN_N) { vk_treg r; if (!vk_treg_get(tid, &r)) return 0; *format = r.has_v && r.v.set_format ? r.v.format : r.t.d.format; *target = r.has_v && r.v.set_target ? r.v.target : r.t.d.target; return 1; }
    int pool_n = atomic_load_explicit(&g_tpool_n, memory_order_acquire);
    if ((pool_n && tid >= pool_n) ||
        !(atomic_load_explicit(&g_treg_kind_ok[(unsigned)tid / 64], memory_order_acquire) >> (tid & 63) & 1)) return 0;
    uint64_t kind = atomic_load_explicit(&g_treg_kind[tid], memory_order_acquire);
    *format = (int32_t)kind; *target = (int32_t)(kind >> 32);
    return 1;
}
typedef struct {
    int tid, want, in_pool; uint32_t gen; uint64_t sub, epoch;
    VkImageView v; vk_tex *x; tex_stamp texture, image;
} tidc_t;
enum { TIDC_N = 4096 };
static __thread tidc_t *T_tidc;
#ifdef TOTK_VK_TESTS
static __thread uint64_t T_tex_lookup[3];
void vk_tex_lookup_counters(uint64_t out[3]) { memcpy(out, T_tex_lookup, sizeof T_tex_lookup); }
#define TEX_LOOKUP_COUNT(i) (T_tex_lookup[i]++)
#else
#define TEX_LOOKUP_COUNT(i) ((void)0)
#endif
static int texgen_on(void) { static int on = -1; if (on < 0) on = !vk_off("texgen"); return on; }
static int texgen_check(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_TEXGEN_CHECK"); on = e && atoi(e); }
    return on;
}
static VkImageView tex_by_id_slow(int tid, int want, vk_tex **x_out, tidc_t *cache) {
    vk_treg rc, *r = &rc;
    *x_out = NULL;
    if (!vk_treg_get(tid, r)) return VK_NULL_HANDLE;
    vk_tex *x = vk_tex_get(&r->t);
    if (!x || !x->im) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_tex_mu);
    VkImageView v = view_get(x, r->has_v ? &r->v : NULL, want == -2 ? 2 : 0, want == -2 ? -1 : want);
    /* Capture the object's own stamp, not the cell's current value: a release
       between registry lookup and this lock must not bless a recycled cell. */
    if (cache) { cache->texture = x->identity; cache->image = x->im->identity; }
    pthread_mutex_unlock(&g_tex_mu);
    *x_out = x;
    return v;
}
static VkImageView tex_by_id_cached(int tid, int want, vk_tex **x_out) {
    if (!texcache_on() || tid <= 0 || tid >= TID_GEN_N) {
        TEX_LOOKUP_COUNT(1);
        vk_tex *x; VkImageView v = tex_by_id_slow(tid, want, &x, NULL); if (x_out) *x_out = x; return v;
    }
    if (!T_tidc) T_tidc = calloc(TIDC_N, sizeof *T_tidc);
    uint32_t gen = atomic_load_explicit(&g_treg_gen[tid], memory_order_acquire);
    uint64_t sub = vk_submit_serial(), epoch = 0;
    int precise = texgen_on(), pool_n = atomic_load_explicit(&g_tpool_n, memory_order_acquire);
    int in_pool = !pool_n || tid < pool_n;
    if (!precise) epoch = atomic_load_explicit(&g_tex_epoch, memory_order_acquire);
    /* Preserve every low ID bit; salt the shader's target to avoid collisions
       between IDs 1024 apart and between different views of the same ID. */
    unsigned hash = (unsigned)tid ^ ((unsigned)tid >> 12) ^ ((unsigned)want + 2u) * 0x9e3779b9u;
    tidc_t *c = &T_tidc[hash % TIDC_N];
    int valid = c->tid == tid && c->want == want && c->gen == gen;
    if (precise) {
        valid = valid && c->in_pool == in_pool &&
            (c->x ? tex_stamp_current(c->texture) && tex_stamp_current(c->image) : c->sub == sub);
        if (valid && c->x && c->sub != sub) {
            TEX_LOOKUP_COUNT(2);
            /* Resolve content, not the registration or view. A switch to GPU
               storage bumps the texture stamp and rebuilds the view below. */
            (void)vk_tex_get(&c->x->t);
            valid = tex_stamp_current(c->texture) && tex_stamp_current(c->image);
            c->sub = sub;
        }
    } else valid = valid && c->epoch == epoch && c->sub == sub;
    if (!valid) {
        TEX_LOOKUP_COUNT(1);
        *c = (tidc_t){.tid = tid, .want = want, .in_pool = in_pool, .gen = gen, .sub = sub, .epoch = epoch};
        c->v = tex_by_id_slow(tid, want, &c->x, precise ? c : NULL);
    } else if (precise && texgen_check()) {
        vk_tex *oracle; VkImageView view = tex_by_id_slow(tid, want, &oracle, NULL);
        int current_pool_n = atomic_load_explicit(&g_tpool_n, memory_order_acquire);
        int stable = gen == atomic_load_explicit(&g_treg_gen[tid], memory_order_acquire) &&
            in_pool == (!current_pool_n || tid < current_pool_n) &&
            (!c->x || (tex_stamp_current(c->texture) && tex_stamp_current(c->image)));
        if (stable && (view != c->v || oracle != c->x)) {
            LOGV("texture generation mismatch: tid=%d target=%d", tid, want); abort();
        }
        if (!stable) { c->tid = 0; if (x_out) *x_out = oracle; return view; }
    }
    if (x_out) *x_out = c->x;
    return c->v;
}
/* TOTK_VK_TEX_MEMO=1 (default 0): a per-thread memo in front of the cache above. Every entry was filled under one
   (registry revision, texture identity revision) pair, read before its lookup; while the current pair equals it, the
   cache above would return the same view and object (its per-ID generation and pool bound are covered by the registry
   revision, its texture/image stamps by the identity revision), so a hit costs one small direct-mapped probe instead of
   the ID's generation, the cache entry and two stamp cells. A changed pair retires every entry at once (a new memo
   generation). The cache's once-per-submission work is kept per entry: an empty result holds for its submission only,
   and a CPU-mirrored texture only once tex_get_inner finished with it in the current submission (x->memo_sub, stored
   after its content check; the check itself stays in the cache path). Other textures never need it: a texture is
   mirrored only from its creation, so one seen unmirrored stays so. Live (runtime/live_ctl.h): each lookup reads it.
   [tex-memo] reports hits, misses and retirements. */
typedef struct { uint32_t gen, key; uint64_t sub; VkImageView v; vk_tex *x; } texmemo_ent;   /* sub: 0 = any submission */
enum { TEXMEMO_N = 2048 };
typedef struct {
    uint64_t registry, identity; uint32_t gen;
    uint64_t counts[3], published[3];   /* hits, misses, retirements; published to g_tex_memo_counts every 4096 */
    texmemo_ent e[TEXMEMO_N];
} texmemo_t;
static __thread texmemo_t *T_texmemo;
static int g_tex_memo_launch;
static totk_live_cache g_tex_memo_live;
static _Atomic uint64_t g_tex_memo_counts[3];
void vk_tex_memo_init(void) {
    const char *e = getenv("TOTK_VK_TEX_MEMO"); g_tex_memo_launch = e && atoi(e) == 1;
    LOGV("TOTK_VK_TEX_MEMO=%d: %s", g_tex_memo_launch, g_tex_memo_launch ?
         "texture ID lookups memoized per thread until the registry, a texture identity or the submission changes" :
         "every texture ID lookup validates its cache entry (default)");
}
int vk_tex_memo_on(void) { return totk_live_cached(&g_tex_memo_live, "TOTK_VK_TEX_MEMO", g_tex_memo_launch); }
void vk_tex_memo_counts(uint64_t out[5]) {
    for (int i = 0; i < 3; i++) out[i] = atomic_load_explicit(&g_tex_memo_counts[i], memory_order_relaxed);
    out[3] = atomic_load_explicit(&g_treg_calls, memory_order_relaxed);
    out[4] = atomic_load_explicit(&g_treg_unchanged, memory_order_relaxed);
}
static void texmemo_publish(texmemo_t *m) {
    for (int i = 0; i < 3; i++) {
        atomic_fetch_add_explicit(&g_tex_memo_counts[i], m->counts[i] - m->published[i], memory_order_relaxed);
        m->published[i] = m->counts[i];
    }
}
VkImageView vk_tex_by_id(int tid, int *target_out, vk_tex **x_out) {
    vk_draw_texture_read(tid,NULL); /* before either descriptor cache can bypass resource preparation */
    int want = target_out ? *target_out : -1;
    TEX_LOOKUP_COUNT(0);
    texmemo_t *m = T_texmemo;
    if (!vk_tex_memo_on() || tid <= 0 || tid >= TID_GEN_N || !texcache_on() ||
        (!m && !(m = T_texmemo = calloc(1, sizeof *m))))
        return tex_by_id_cached(tid, want, x_out);
    uint64_t registry = vk_treg_revision(), identity = atomic_load_explicit(&g_tex_ident_rev, memory_order_acquire);
    uint64_t sub = vk_submit_serial();
    if (registry != m->registry || identity != m->identity) {
        m->registry = registry; m->identity = identity; m->counts[2]++;
        if (!++m->gen) { memset(m->e, 0, sizeof m->e); m->gen = 1; }
    }
    if (want < -2 || want >= 4094) return tex_by_id_cached(tid, want, x_out);   /* outside the packed key */
    uint32_t key = (uint32_t)tid | (uint32_t)(want + 2) << 20;   /* tid < 2^20 */
    texmemo_ent *e = &m->e[(key ^ key >> 11 ^ key >> 20) % TEXMEMO_N];
    VkImageView v = VK_NULL_HANDLE; vk_tex *x = NULL;
    int hit = e->gen == m->gen && e->key == key &&
        (e->x ? e->sub != UINT64_MAX || atomic_load_explicit(&e->x->memo_sub, memory_order_acquire) == sub : e->sub == sub);
    if (hit) {
        v = e->v; x = e->x; m->counts[0]++;
        if (texgen_check()) {   /* the cache above is the oracle while nothing it depends on moved */
            vk_tex *oracle; VkImageView view = tex_by_id_cached(tid, want, &oracle);
            if (registry == vk_treg_revision() && identity == atomic_load_explicit(&g_tex_ident_rev, memory_order_acquire) &&
                sub == vk_submit_serial() && (view != v || oracle != x)) {
                LOGV("texture memo mismatch: tid=%d target=%d", tid, want); abort();
            }
        }
    } else {
        v = tex_by_id_cached(tid, want, &x); m->counts[1]++;
        /* An empty result: this submission only. A mirrored texture: UINT64_MAX, checked against x->memo_sub per hit. */
        *e = (texmemo_ent){m->gen, key, !x ? sub : __atomic_load_n(&x->mirrored, __ATOMIC_RELAXED) ? UINT64_MAX : 0, v, x};
    }
    if (!((m->counts[0] + m->counts[1]) & 4095)) texmemo_publish(m);
    if (x_out) *x_out = x;
    return v;
}
static VkBufferView tbuf_by_id(int tid, int writable, VkDescriptorBufferInfo *live, uint32_t *reason) {
    vk_treg rc, *r = &rc;
    if (!vk_treg_get(tid, r) || r->t.d.target != 10) return VK_NULL_HANDLE;
    if (live) { memset(live, 0, sizeof *live); *reason = 0; }
    uint64_t address = (uint64_t)(uintptr_t)r->t.mem;
    int block_w, block_h, bytes_per_texel = nvn_fmt_bpb(r->t.d.format, &block_w, &block_h);
    if (address && bytes_per_texel > 0) vk_ub_touch(address, (uint64_t)r->t.d.width * bytes_per_texel);
    vk_tex *x = vk_tex_get(&r->t);
    pthread_mutex_lock(&g_tex_mu);
    if (!x) { x = nvn_hm_get(&g_by_serial, &r->t.serial); }
    if (x && x->tbv && !vk_lag_capture_on()) { VkBufferView cached = x->tbv; pthread_mutex_unlock(&g_tex_mu); return cached; }
    VkBufferView bv = VK_NULL_HANDLE;
    if (x) {
        int bw, bh, bpb = nvn_fmt_bpb(r->t.d.format, &bw, &bh); VkFormat f; VkBuffer b; uint64_t off;
        uint64_t addr = (uint64_t)(uintptr_t)r->t.mem, size = (uint64_t)r->t.d.width * bpb;
        if (!vk_fmt(r->t.d.format, &f, NULL) || !f || !bpb) RED(4, "buffer texture format %#x has no Vulkan format", r->t.d.format);
        else if (!vk_resolve(addr, size, &b, &off)) RED(4, "buffer texture storage is outside the memory pools");
        else {
            size = MINV(size, (uint64_t)V.props.limits.maxTexelBufferElements * bpb);
            VkDescriptorBufferInfo frozen;
            if (writable && vk_lag_capture_on()) {
                if (live) { vk_gpu_write_record(addr, size); *reason = NVN_DIRTY_LAG_GPU | NVN_DIRTY_LAG_WRITABLE; }
                else vk_gpu_write(addr, size);
            } else if (live ? vk_lag_read_record(addr, size, V.props.limits.minTexelBufferOffsetAlignment, &frozen, reason, VK_LAG_TEXEL) :
                vk_lag_read(addr, size, V.props.limits.minTexelBufferOffsetAlignment, &frozen, VK_LAG_TEXEL)) { b = frozen.buffer; off = frozen.offset; }
            if (live && *reason) *live = (VkDescriptorBufferInfo){b, off, size};
            if (off % V.props.limits.minTexelBufferOffsetAlignment) RED(4, "buffer texture offset %#llx is not aligned", (unsigned long long)off);
            else {
                if (x->tbv && (x->tb_buffer != b || x->tb_offset != off || x->tb_range != size)) { retire_bview(x->tbv, x->tb_buffer); x->tbv = VK_NULL_HANDLE; }
                if (!x->tbv) {
                    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = f, .offset = off, .range = size};
                    if (!vkCreateBufferView(V.dev, &ci, NULL, &x->tbv)) { x->tb_buffer = b; x->tb_offset = off; x->tb_range = size; }
                }
                bv = x->tbv;
            }
        }
    }
    pthread_mutex_unlock(&g_tex_mu);
    return bv;
}
VkBufferView vk_tbuf_by_id(int tid, int writable) { return tbuf_by_id(tid, writable, NULL, NULL); }
VkBufferView vk_tbuf_by_id_record(int tid, int writable, VkDescriptorBufferInfo *live, uint32_t *reason) { return tbuf_by_id(tid, writable, live, reason); }
void mtl_reg_samp(int sid, const samp_par *sp) {
    if (sid < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (sid >= g_nsreg) {
        int n = sid + 256; g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, (size_t)n); g_sobj = realloc(g_sobj, (size_t)n * sizeof *g_sobj);
        memset(g_sreg_ok + g_nsreg, 0, (size_t)(n - g_nsreg)); memset(g_sobj + g_nsreg, 0, (size_t)(n - g_nsreg) * sizeof *g_sobj); g_nsreg = n;
    }
    if (g_sobj[sid] && !memcmp(&g_sreg[sid], sp, sizeof *sp)) { pthread_mutex_unlock(&g_reg_mu); return; }
    if (g_sobj[sid]) { /* the old sampler may still be in recorded descriptor sets: keep it (samplers are few) */ g_sobj[sid] = VK_NULL_HANDLE; }
    g_sreg[sid] = *sp; g_sreg_ok[sid] = 1;
    atomic_fetch_add(&g_samp_epoch, 1);
    pthread_mutex_unlock(&g_reg_mu);
}
static VkSampler make_sampler(const samp_par *p) {
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (p->minf == 1 || p->minf == 3 || p->minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (p->minf == 4 || p->minf == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = vk_wrap(p->ws); ci.addressModeV = vk_wrap(p->wt); ci.addressModeW = vk_wrap(p->wr);
    ci.mipLodBias = p->lod_bias;
    ci.minLod = p->minf <= 1 ? 0 : (p->lod_min < 0 ? 0 : p->lod_min);
    ci.maxLod = p->minf <= 1 ? 0.25f : p->lod_max;   /* not mipmapped: level 0 only (the Mac's MipFilterNotMipmapped) */
    if (p->aniso > 1 && V.props.limits.maxSamplerAnisotropy > 1) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = MINV(p->aniso, V.props.limits.maxSamplerAnisotropy); }
    if (p->cmp_mode) { ci.compareEnable = VK_TRUE; ci.compareOp = vk_compare(p->cmp_func); }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = (VkSamplerReductionMode)(p->reduction <= 2 ? p->reduction : 0)};
    const void *chain = NULL;
    if (p->reduction) { rm.pNext = chain; chain = &rm; }
    const float *b = p->border; int grey = b[0] == b[1] && b[1] == b[2];
    if (grey && b[0] == 0 && b[3] == 0) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (grey && b[0] == 0 && b[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (grey && b[0] == 1 && b[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (V.has_custom_border) { ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; memcpy(cb.customBorderColor.float32, b, 16); cb.format = VK_FORMAT_UNDEFINED; cb.pNext = chain; chain = &cb; }
    else ci.borderColor = b[3] > 0.5f ? (b[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.pNext = chain;
    VkSampler s;
    if (vkCreateSampler(V.dev, &ci, NULL, &s)) { RED(4, "vkCreateSampler failed"); return VK_NULL_HANDLE; }
    return s;
}
VkSampler vk_samp_by_id(int sid) {
    enum { SC = 1 << 12 };   /* sampler ids change rarely: cached until any sampler registration changes one */
    static struct { int sid; uint32_t ep; VkSampler s; } sc[SC];
    uint32_t ep = atomic_load(&g_samp_epoch); unsigned h = (unsigned)sid & (SC - 1);
    if (texcache_on() && sid > 0 && sc[h].sid == sid && sc[h].ep == ep) return sc[h].s;
    pthread_mutex_lock(&g_reg_mu);
    VkSampler s = VK_NULL_HANDLE;
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
        if (!g_sobj[sid]) {
            /* identical parameters share a sampler: drivers limit how many exist (maxSamplerAllocationCount) */
            static nvn_hmap by_par = {.klen = sizeof(samp_par)};
            VkSampler have = nvn_hm_get(&by_par, &g_sreg[sid]);
            if (!have && (have = make_sampler(&g_sreg[sid]))) nvn_hm_put(&by_par, &g_sreg[sid], (void *)have);
            g_sobj[sid] = have;
        }
        s = g_sobj[sid];
    }
    pthread_mutex_unlock(&g_reg_mu);
    if (s) { sc[h].sid = sid; sc[h].ep = ep; sc[h].s = s; }
    return s;
}
VkSampler vk_dummy_sampler(void) {
    static VkSampler s;
    if (!s) { VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 0.25f}; VKCHECK(vkCreateSampler(V.dev, &ci, NULL, &s)); }
    return s;
}
/* Empty slots: null descriptors when the driver has them, else a 1x1 zero image of the declared type. */
VkImageView vk_dummy_view(int target, int is_int) {
    if (V.has_null_descriptor) return VK_NULL_HANDLE;
    static VkImageView views[11][2]; static VkImage imgs[2][2]; static vk_alloc mems[2][2];
    int t = target < 0 || target > 10 ? 1 : target, ii = is_int ? 1 : 0;
    if (views[t][ii]) return views[t][ii];
    int cube = t == 8 || t == 9, three = t == 2;
    int slot = three ? 1 : 0;
    if (!imgs[slot][ii]) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = slot ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, .imageType = slot ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = ii ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = slot ? 1 : 6, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateImage(V.dev, &ci, NULL, &imgs[slot][ii]));
        VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, imgs[slot][ii], &req);
        if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mems[slot][ii])) DIE("dummy texture memory");
        VKCHECK(vkBindImageMemory(V.dev, imgs[slot][ii], mems[slot][ii].mem, mems[slot][ii].off));
        VkCommandBuffer cb = vk_pre_cb(); image_barrier_init(cb, imgs[slot][ii], VK_IMAGE_ASPECT_COLOR_BIT);
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "upload");
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, imgs[slot][ii], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); vk_barrier_all(cb);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
        totk_ft_count(FT_COUNT_TEX_REALIZATIONS, 1);
    }
    VkImageViewType vt = t == 0 ? VK_IMAGE_VIEW_TYPE_1D : t == 3 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : t == 4 || t == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : cube ? (t == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : three ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) vt = vt == VK_IMAGE_VIEW_TYPE_1D ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;   /* a 2D image has no 1D views; 1D slots are rare */
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[slot][ii], .viewType = vt, .format = ii ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1}};
    VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &views[t][ii]));
    return views[t][ii];
}
VkBufferView vk_dummy_tbuf(void) {
    if (V.has_null_descriptor) return VK_NULL_HANDLE;
    static VkBufferView bv;
    if (!bv) {
        static VkBuffer keep; static VkDeviceMemory km;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
        VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &keep)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, keep, &req);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
        VKCHECK(vk_memory_allocate(&mai, &km, VK_MEMORY_ZERO)); VKCHECK(vkBindBufferMemory(V.dev, keep, km, 0));
        void *m; VKCHECK(vkMapMemory(V.dev, km, 0, VK_WHOLE_SIZE, 0, &m)); memset(m, 0, 256);
        VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = keep, .format = VK_FORMAT_R32_UINT, .range = 256};
        VKCHECK(vkCreateBufferView(V.dev, &ci, NULL, &bv)); }
    return bv;
}

/* Diagnostic reads the resolved image, never pool bytes or a re-realized serial. */
void vk_texcopy_readback(const char *op, const char *side, const tex_t *t, vk_tex *x,
                         unsigned level, unsigned layer, const NVNcopyRegion *r) {
    static unsigned samples;
    if (!totk_texcopy_on() || samples >= 32) return;
    unsigned sample = ++samples;
    if (!x || !x->im || !t || !r) {
        totk_texcopy_log(vk_submit_serial(), "phase=readback op=%s side=%s sample=%u result=skip-unresolved", op, side, sample);
        return;
    }
    vk_img *im = x->im;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    int nz = r->depth ? r->depth : 1, is3d = im->type == VK_IMAGE_TYPE_3D;
    if (!bpb || im->samples != 1 || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || level >= im->levels ||
        r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 || r->width <= 0 || r->height <= 0 || nz <= 0 ||
        (uint64_t)r->xoffset + r->width > MAXV(im->w >> level, 1u) ||
        (uint64_t)r->yoffset + r->height > MAXV(im->h >> level, 1u) ||
        (uint64_t)r->zoffset + nz + (is3d ? 0 : layer) > (is3d ? MAXV(im->d >> level, 1u) : im->layers) ||
        r->xoffset % bw || r->yoffset % bh) {
        totk_texcopy_log(vk_submit_serial(), "phase=readback op=%s side=%s sample=%u ser=%llu result=skip-unsupported-region",
            op, side, sample, (unsigned long long)t->serial);
        return;
    }
    size_t row = (size_t)((r->width + bw - 1) / bw) * bpb, rows = (r->height + bh - 1) / bh;
    uint64_t total = (uint64_t)row * rows * nz;
    if (total > (64u << 20)) {
        totk_texcopy_log(vk_submit_serial(), "phase=readback op=%s side=%s sample=%u ser=%llu result=skip-size bytes=%llu",
            op, side, sample, (unsigned long long)t->serial, (unsigned long long)total);
        return;
    }
    uint64_t sub = vk_submit_serial(), serial = t->serial, storage = (uint64_t)(uintptr_t)t->mem;
    VkImage image = im->img;
    vk_end_rendering_at(VK_PASS_END_CPU_WAIT, 2);
    vk_tex_native_read(x);
    VkCommandBuffer cb = vk_cb(); VkBuffer buffer; VkDeviceSize offset;
    uint8_t *bytes = vk_transient((size_t)total, 16, &buffer, &offset);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vk_barrier_all(cb);
    VkBufferImageCopy copy = {.bufferOffset = offset,
        .imageSubresource = {im->aspect, level, is3d ? 0 : layer + r->zoffset, is3d ? 1 : (unsigned)nz},
        .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset : 0},
        .imageExtent = {(unsigned)r->width, (unsigned)r->height, is3d ? (unsigned)nz : 1}};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    vk_pso_life_external_read(MAXV(x->im->w >> level, 1u), MAXV(x->im->h >> level, 1u), "diagnostic-readback");
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    vk_flush_wait();
    totk_texcopy_log(sub, "phase=readback op=%s side=%s ser=%llu mem=%p image=%#llx level=%u layer=%u region=%d,%d,%d:%dx%dx%d bytes=%zu nonzero=%zu sample=%u result=complete",
        op, side, (unsigned long long)serial, (void *)(uintptr_t)storage, (unsigned long long)(uintptr_t)image,
        level, layer, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, nz, (size_t)total,
        totk_texcopy_nonzero(bytes, (size_t)total), sample);
}

/* ---------- CPU texel access (ReadTexels / WriteTexels): nvn_mtl.m mtl_read_texels / mtl_write_texels ---------- */
void vk_end_rendering(void);
enum { TS_CALLS, TS_REGION, TS_OLD_CONVERT, TS_CONVERT, TS_CALL_NS, TS_LOCK_NS,
       TS_UPLOADS, TS_GPU_WRITTEN, TS_FLUSHES, TS_FLUSH_NS, TS_FALLBACKS, TS_COUNT };
typedef struct { uint64_t v[TS_COUNT]; } texel_sample;
static _Atomic uint64_t g_texel_stats[2][TS_COUNT];
static _Atomic int g_texel_stats_on = -1;
static int texel_stats_on(void) {
    int on = atomic_load_explicit(&g_texel_stats_on, memory_order_relaxed);
    if (on < 0) { const char *e = getenv("TOTK_TEXEL_STATS"); on = e && !strcmp(e, "1"); atomic_store_explicit(&g_texel_stats_on, on, memory_order_relaxed); }
    return on;
}
void vk_texel_stats(uint64_t frame) {
    if (!texel_stats_on() || frame % 300) return;
    for (unsigned write = 0; write < 2; write++) {
        uint64_t v[TS_COUNT];
        for (unsigned i = 0; i < TS_COUNT; i++) v[i] = atomic_exchange_explicit(&g_texel_stats[write][i], 0, memory_order_relaxed);
        LOGV("texel stats frame=%llu op=%s calls=%llu region_bytes=%llu old_convert_bytes=%llu converted_bytes=%llu call_ms=%.3f lock_ms=%.3f uploads=%llu gpu_written=%llu flush_waits=%llu flush_ms=%.3f fallbacks=%llu",
            (unsigned long long)frame, write ? "write" : "read", (unsigned long long)v[TS_CALLS],
            (unsigned long long)v[TS_REGION], (unsigned long long)v[TS_OLD_CONVERT], (unsigned long long)v[TS_CONVERT],
            v[TS_CALL_NS] / 1e6, v[TS_LOCK_NS] / 1e6, (unsigned long long)v[TS_UPLOADS],
            (unsigned long long)v[TS_GPU_WRITTEN], (unsigned long long)v[TS_FLUSHES], v[TS_FLUSH_NS] / 1e6,
            (unsigned long long)v[TS_FALLBACKS]);
    }
}
static void texel_flush(texel_sample *s) {
    uint64_t start = s ? host_now_ns() : 0;
    vk_flush_wait();
    if (s) { s->v[TS_FLUSHES]++; s->v[TS_FLUSH_NS] += host_now_ns() - start; }
}
/* A pool-only CPU access must not eagerly realize/re-upload a whole VkImage.
   Existing GPU ownership (including another serial over this storage) still
   goes through the ordinary resolver. Caller holds recording ownership. */
static vk_tex *texel_image(const tex_t *t) {
    if (!tex_mirrored(t)) return vk_tex_get(t);
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = nvn_hm_get(&g_by_serial, &t->serial);
    stor_key k; vk_img *shared = NULL;
    if (!(x && x->im && x->im->gpu_written) && mirshare_on() && stor_key_of(t, &k))
        shared = nvn_hm_get(&g_by_storage, &k);
    int resolve = shared && shared->gpu_written;
    pthread_mutex_unlock(&g_tex_mu);
    return resolve ? vk_tex_get(t) : x;
}
/* The fallback is only for layouts not admitted by the region engine. It retains
   the old whole-level behavior; supported layouts never allocate a linear level. */
static int pool_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                       uint8_t *bytes, size_t row, size_t img, int write, texel_sample *s) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t wb = (r->width + bw - 1) / bw * bpb, hb = (r->height + bh - 1) / bh;
    uint32_t x0 = r->xoffset / bw * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    uint64_t old_bytes = 0;
    if (s) {
        uint32_t lw = MAXV(t->d.width >> level, 1u), lh = t->d.target == 0 || t->d.target == 3 ? 1 : MAXV(t->d.height >> level, 1u), lz = is3d ? MAXV(t->d.depth >> level, 1u) : 1;
        old_bytes = (uint64_t)((lw + bw - 1) / bw) * bpb * ((lh + bh - 1) / bh) * lz * (is3d ? 1 : nz) * (write ? 2 : 1);
        s->v[TS_OLD_CONVERT] += old_bytes;
        static unsigned shapes[2];
        if (shapes[write] < 8) {
            shapes[write]++;
            LOGV("texel shape op=%s format=%#x bpb=%d block=%dx%d level=%u layer=%u extent=%ux%ux%u region=%d,%d,%d:%dx%dx%d row=%zu img=%zu",
                write ? "write" : "read", t->d.format, bpb, bw, bh, level, layer0, lw, lh, lz,
                r->xoffset, r->yoffset, r->zoffset, r->width, r->height, nz, row, img);
        }
    }
    int bad = 0;
    for (int z = 0; z < (is3d ? 1 : nz); z++) {
        unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
        if (write) bad = nvn_swizzle_region(&t->d, level, layer, x0, y0, is3d ? r->zoffset : 0,
            wb, hb, is3d ? nz : 1, bytes + (size_t)z * img, row, img, (void *)t->mem);
        else bad = nvn_deswizzle_region(&t->d, level, layer, x0, y0, is3d ? r->zoffset : 0,
            wb, hb, is3d ? nz : 1, t->mem, bytes + (size_t)z * img, row, img);
        if (bad) break;
    }
    if (!bad) { if (s) s->v[TS_CONVERT] += (uint64_t)wb * hb * nz; return 1; }
    static unsigned warned;
    if (!(warned & (1u << write))) {
        warned |= 1u << write;
        LOGV("texel region fallback op=%s format=%#x target=%d flags=%#x level=%u region=%d,%d,%d:%dx%dx%d",
            write ? "write" : "read", t->d.format, t->d.target, t->d.flags, level,
            r->xoffset, r->yoffset, r->zoffset, r->width, r->height, nz);
    }
    if (s) { s->v[TS_FALLBACKS]++; s->v[TS_CONVERT] += old_bytes; }
    uint32_t lw = MAXV(t->d.width >> level, 1u), lh = MAXV(t->d.height >> level, 1u), lz = is3d ? MAXV(t->d.depth >> level, 1u) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    uint8_t *lin = malloc(lp * lr * lz);
    if (!lin) { RED(4, "texel fallback: allocation failed"); return 0; }
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
        if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) {
            free(lin); RED(4, "texel fallback: unsupported layout"); return 0;
        }
        uint8_t *p = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) {
            if (write) memcpy(p + (y0 + y) * lp + x0, bytes + (size_t)z * img + y * row, wb);
            else memcpy(bytes + (size_t)z * img + y * row, p + (y0 + y) * lp + x0, wb);
        }
        if (write && (!is3d || z == nz - 1)) nvn_swizzle(&t->d, level, layer, lin, lp, (void *)t->mem);
    }
    free(lin); return 1;
}
static void read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img, texel_sample *s) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !r->width || !r->height) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (s) s->v[TS_REGION] = (uint64_t)wb * hb * nz;
    vk_tex *x = texel_image(t);
    if (totk_texcopy_match(t)) {
        vk_texcopy_image("TextureReadTexels", "src", t, x);
        texcopy_resource(tex_mirrored(t) && !(x && x->im && x->im->gpu_written) ? "read-guest" : x && x->im ? "read-gpu" : "read-zero-unresolved", x);
    }
    if (tex_mirrored(t) && !(x && x->im && x->im->gpu_written)) {   /* the pool bytes are the content */
        pool_texels(t, level, layer0, r, dst, row, img, 0, s);
        return;
    }
    if (!x || !x->im) { memset(dst, 0, img * nz); return; }
    vk_end_rendering_at(VK_PASS_END_CPU_WAIT, 2);   /* CPU texel read */
    vk_tex_native_read(x);
    VkCommandBuffer cb = vk_cb();
    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(wb * hb * nz, 16, &sb, &so);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vk_barrier_all(cb);
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy c = {.bufferOffset = so + (VkDeviceSize)z * wb * hb, .imageSubresource = {x->im->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : x->im->aspect, level, is3d ? 0 : layer0 + r->zoffset + z, 1},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
        vkCmdCopyImageToBuffer(cb, x->im->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
        vk_pso_life_external_read(MAXV(x->im->w >> level, 1u), MAXV(x->im->h >> level, 1u), "cpu-texel-read");
    }
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    texel_flush(s);
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, stg + (size_t)z * wb * hb + y * wb, wb);
}
static void write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img, texel_sample *s) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !r->width || !r->height) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (s) s->v[TS_REGION] = (uint64_t)wb * hb * nz;
    vk_tex *x = texel_image(t);
    if (totk_texcopy_match(t)) {
        vk_texcopy_image("TextureWriteTexels", "dst", t, x);
        if (x) texcopy_resource(tex_mirrored(t) ? "write-guest" : "write-gpu", x);
    }
    if (tex_mirrored(t)) {   /* pool bytes in block-linear layout; dirty tracking re-uploads them */
        if (!pool_texels(t, level, layer0, r, (uint8_t *)src, row, img, 1, s)) return;
        if (!(x && x->im && x->im->gpu_written)) return;
    }
    if (!x || !x->im) return;
    if (s) { s->v[TS_UPLOADS]++; s->v[TS_GPU_WRITTEN] += !!x->im->gpu_written; }
    vk_end_rendering_at(VK_PASS_END_COPY, 3);   /* CPU texel upload */
    vk_tex_native_read(x);
    VkCommandBuffer cb = vk_cb();
    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_transient(wb * hb * nz, 16, &sb, &so);
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(stg + (size_t)z * wb * hb + y * wb, src + z * img + y * row, wb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "upload");
    vk_barrier_all(cb);
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy c = {.bufferOffset = so + (VkDeviceSize)z * wb * hb, .imageSubresource = {x->im->aspect, level, is3d ? 0 : layer0 + r->zoffset + z, 1},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
        vkCmdCopyBufferToImage(cb, sb, x->im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        vk_pso_life_external_write(x, level, is3d ? 0 : layer0 + r->zoffset + z, 1, "cpu-texel-upload");
        totk_ft_count(FT_COUNT_TEX_UPLOADS, 1);
    }
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    vk_tex_native_patch(x, level, layer0, r);
    texel_flush(s);
}
void vk_res_counters(uint64_t v[7]) { v[0] = g_stat_images; v[1] = g_stat_shared_hits; v[2] = g_stat_uploads; v[3] = g_stat_reuploads; v[4] = g_stat_swept; v[5] = g_stat_published; v[6] = g_stat_switched; }
void vk_res_mem(uint64_t v[8]) {
    pthread_mutex_lock(&g_mem_mu); v[0] = g_mem_block_bytes; v[1] = g_mem_dedicated_bytes; v[7] = 0;
    for (int i = 0; i < g_nblocks; i++) v[7] += g_blocks[i].used;
    pthread_mutex_unlock(&g_mem_mu);
    pthread_mutex_lock(&g_tex_mu); v[2] = (uint64_t)g_nretired; v[3] = g_by_serial.n; v[4] = g_by_storage.n; v[5] = (uint64_t)g_nwb; v[6] = (uint64_t)g_capretired; pthread_mutex_unlock(&g_tex_mu);
}
void vk_res_stats(void) {
    pthread_mutex_lock(&g_tex_mu);
    if (g_wb_done || g_nwb || g_wb_skip_cpu) LOGV("GPU write-back: %llu renderings written back (%llu MiB), %llu skipped (CPU wrote since), %llu superseded by GPU copies, %llu aged out, %llu unsupported; %d waiting",
        (unsigned long long)g_wb_done, (unsigned long long)(g_wb_bytes >> 20), (unsigned long long)g_wb_skip_cpu, (unsigned long long)g_wb_superseded, (unsigned long long)g_wb_aged, (unsigned long long)g_wb_skip_fmt, g_nwb);
    LOGV("textures: %u live, %llu images made (%llu storage-shared reuses; %llu CPU-pool images the GPU rendered into shared, %llu serials switched to them; %llu compressible format splits, %llu cross-format shares), %llu uploads + %llu re-uploads (%llu MiB); GPU memory %llu MiB in blocks + %llu MiB dedicated",
         g_by_serial.n, (unsigned long long)g_stat_images, (unsigned long long)g_stat_shared_hits, (unsigned long long)g_stat_published, (unsigned long long)g_stat_switched, (unsigned long long)g_stat_compsplit, (unsigned long long)g_stat_xfmt, (unsigned long long)g_stat_uploads, (unsigned long long)g_stat_reuploads,
         (unsigned long long)(g_stat_upload_bytes >> 20), (unsigned long long)(g_mem_block_bytes >> 20), (unsigned long long)(g_mem_dedicated_bytes >> 20));
    pthread_mutex_unlock(&g_tex_mu);
}

/* Keep recording ownership: dirty stamps detect a future upload, but do not
   exclude an in-progress texture snapshot or a temporary-RW write-back landing.
   Region stores shorten this critical section without changing its ordering. */
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    runtime_host_wait wait;
    if (!texel_stats_on()) { vk_rec_lock_texels(&wait); read_texels(t, level, layer0, r, dst, row, img, NULL); vk_rec_unlock_texels(&wait); return; }
    texel_sample s = {0}; uint64_t start = host_now_ns();
    vk_rec_lock_texels(&wait); s.v[TS_LOCK_NS] = host_now_ns() - start;
    read_texels(t, level, layer0, r, dst, row, img, &s); vk_rec_unlock_texels(&wait);
    s.v[TS_CALLS] = 1; s.v[TS_CALL_NS] = host_now_ns() - start;
    for (unsigned i = 0; i < TS_COUNT; i++) atomic_fetch_add_explicit(&g_texel_stats[0][i], s.v[i], memory_order_relaxed);
}
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    runtime_host_wait wait;
    if (!texel_stats_on()) { vk_rec_lock_texels(&wait); write_texels(t, level, layer0, r, src, row, img, NULL); vk_rec_unlock_texels(&wait); return; }
    texel_sample s = {0}; uint64_t start = host_now_ns();
    vk_rec_lock_texels(&wait); s.v[TS_LOCK_NS] = host_now_ns() - start;
    write_texels(t, level, layer0, r, src, row, img, &s); vk_rec_unlock_texels(&wait);
    s.v[TS_CALLS] = 1; s.v[TS_CALL_NS] = host_now_ns() - start;
    for (unsigned i = 0; i < TS_COUNT; i++) atomic_fetch_add_explicit(&g_texel_stats[1][i], s.v[i], memory_order_relaxed);
}

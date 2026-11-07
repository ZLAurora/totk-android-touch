/* Drawing on Vulkan (vk_int.h): the bound NVN state, render passes as dynamic rendering, graphics pipelines,
   descriptor sets, draws, compute dispatches, texture copies and clears, and present. The Metal counterpart is
   nvn_mtl.m state_op / draw_inner / encode_args / pso_desc / copy_* / clear_tex / dispatch; the rules are the same,
   the many Metal-only optimisations (tile memory, pass merging, extrapolation) are not carried over.

   Conventions shared with the translated shaders (tools/totk_xlate, Metal's): clip depth 0..1 (NEAR_IS_MINUS_W is
   converted in the vertex shader), framebuffer origin at the top left, clip-space y up. Vulkan's clip y points down,
   so the viewport is flipped (negative height), which places every pixel where Metal places it. */
#include "vk_int.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_framegen_qcom.h"
#include "vk_framegen_lsfg.h"
#include "vk_overlay.h"
#include "framegen_math.h"
#include "framegen_dependencies.h"
#include "framegen_passes.h"
#include "vk_canvas.h"
#include "vk_ui_aspect.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "totk_lifelog.h"
#include "dm_wait_stats.h"
#include "nvn_scan_budget.h"
#include "../../runtime/frame_timeline.h"
#include "../../runtime/present_clock.h"
#include "../../runtime/present_predict.h"
#include "../../runtime/fps_auto_runtime.h"
#include "../../runtime/fps_hybrid_runtime.h"
#include "../../runtime/fps_adaptive_runtime.h"
#include "framegen_adaptive.h"
#include "framegen_adaptive_plan.h"
#include "framegen_adaptive_present.h"
#include "framegen_adaptive_slots.h"
#include "framegen_adaptive_publication.h"
#include "framegen_adaptive_latch.h"
#include "framegen_adaptive_defaults.h"
#include "framegen_job_trace.h"
static void fg_adaptive_present_cancel(void);
/* TOTK_ASPECT / key 4 (runtime/aspect_patch.h): the ratio the canvas is shown at, 0 = its own shape. Weak: GPU-only
   tests link no runtime. */
extern float runtime_aspect_now(void) __attribute__((weak));
static float pres_shown_aspect(void) { return runtime_aspect_now ? runtime_aspect_now() : 0; }
/* This draw's HUD scale for that ratio (vk_ui_aspect.h), set by draw_inner and applied by dynamic_state. */
static vk_ui_aspect g_ui_aspect;
static int g_ui_aspect_on;
#include "../../runtime/pgo_dump.h"
#include "../../runtime/android_perf.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>
#include "pso_trace_key.h"
#include "vk_screen_classify.h"

#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define MINV(a, b) ((a) < (b) ? (a) : (b))
vk_state S;
static uint64_t g_stat_draws, g_stat_skips[16], g_stat_passes, g_stat_pipes, g_stat_dispatch;
vk_hitch g_hk;
int g_hitch_on;
/* Per 300 frames: graphics pipeline creation (count, time, longest) and frame times (longest, over 33 / 50 ms): stutter. */
static uint64_t g_win_pipes, g_win_pipe_ns, g_win_pipe_max_ns, g_win_frame_max_ns, g_win_over33, g_win_over50, g_last_present_ns;
static uint64_t g_win_sync, g_win_sync_ns, g_win_prewarm_wait, g_win_prewarm_wait_ns;
static _Atomic uint64_t g_cum_pipe_ns;   /* all graphics pipeline creation, for the long-frame log */
static uint64_t g_chunk_draws;   /* draws recorded since the last chunk submission (vk_submit_chunk) */
static int g_pipe_dirty = 1;   /* key-feeding state ops and pass rebuilds; resolved shaders and topology are checked separately */
/* Exact inputs of the generated driver banks; advance only when the bytes they encode change. */
static uint64_t g_driver_revision[2][6];
static uint64_t g_record_bind_revision[6], g_record_freeze_hits, g_record_freeze_misses;
static struct { uint64_t tex[2]; uint32_t img; } g_native_bound[6];
static uint32_t g_native_bound_any;
static uint64_t g_native_bound_revision;
/* TOTK_VK_PREPARE_MASK=1 (default off): pre-render uniform preparation (vk_lag_uniform_prepare) runs only for consumed
   registers marked dirty here. A register is dirtied when its bound address or size changes; every register is dirtied
   when an overlay, original-memory producer or mapping revision advances (vk_lag_uniform_prepare_changed). Full bound
   ranges are prepared, so shader switches need no invalidation: newly consumed registers keep their dirty bits. This
   skips only pre-render materialization, never lag selection, snapshots or guards. TOTK_VK_OFF=prepare (or all) keeps
   it off; TOTK_VK_PREPARE_CHECK=1 runs every skipped preparation as an oracle and dies on a missed materialization.
   Live (runtime/live_ctl.h): read at each draw's preparation; a change dirties every register, so the first draw after
   it prepares in full. The masks and counters are walking-lock owned. */
static int g_prepare_mask_launch, g_prepare_off, g_prepare_check, g_prepare_used;
static totk_live_cache g_prepare_mask_live;
static int g_alpha_launch;
static totk_live_cache g_alpha_live;
static uint64_t g_alpha_enabled, g_alpha_ignored;
static int g_clear_read_launch;
static totk_live_cache g_clear_read_live;
static uint64_t g_clear_read_forced;
static uint16_t g_uniform_prepare_dirty[2] = {UINT16_MAX, UINT16_MAX};
static uint64_t g_prepare_calls, g_prepare_skips, g_prepare_checks;
static int prepare_mask_live(void) {
    return !g_prepare_off && totk_live_cached(&g_prepare_mask_live, "TOTK_VK_PREPARE_MASK", g_prepare_mask_launch);
}
#ifdef TOTK_VK_TESTS
static uint64_t g_test_widened;
void vk_record_test_counts_get(vk_record_test_counts *out) {
    *out = (vk_record_test_counts){g_stat_draws, g_stat_passes, g_stat_skips[8], g_test_widened,
        g_record_freeze_hits, vk_prog_record_test_hits()};
}
#endif

static void state_reset(void) {
    g_pipe_dirty = 1;
    vk_stage_sig_invalidate(0x3fu);
    for (int b = 0; b < 2; b++) for (int st = 0; st < 6; st++) g_driver_revision[b][st]++;
    memset(&S, 0, sizeof S);
    memset(g_native_bound,0,sizeof g_native_bound); g_native_bound_any=0; g_native_bound_revision=0;
    g_uniform_prepare_dirty[0] = g_uniform_prepare_dirty[1] = UINT16_MAX;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
    for (int i = 0; i < 6; i++) S.tex_hi[i] = 1;
}
static void pso_life_init(void);
static void state_init(void) { static int done; if (!done) { done = 1; pso_life_init(); state_reset(); } }

/* ---------- render passes ---------- */
static struct {
    int open, secondary;                        /* primary rendering contents: inline or worker secondaries */
    int clear_c[8]; VkClearValue cc[8]; int clear_d, clear_s; float cd; uint32_t cs;
    uint32_t w, h;                              /* extent of the open pass */
    uint32_t host_w, host_h; int native, canvas; /* logical support-buffer size remains w/h */
    uint32_t native_w, native_h;                /* active coordinate basis, distinct from padded window allocation */
    VkFormat cfmt[8]; VkFormat dfmt, sfmt; uint32_t ncolor; int dclass[8];
    vk_tex *ctex[8]; vk_tex *dtex;
    const void *caddr[8], *daddr; uint32_t cleared;   /* the attachments' guest memory, and which were cleared at the start (bit 8: depth) (TOTK_VK_PASS_DUMP) */
    uint8_t clevel[8], dlevel; uint16_t clayer[8], dlayer;   /* the attachments' view level and layer (TOTK_VK_PASS_DUMP) */
    uint64_t c_serial[8], d_serial; int c_nvnfmt[8];
    unsigned c0_level, c0_layer; int c0_cleared; uint64_t c0_serial; int c0_fmt; uint64_t draws0;   /* pass probe */
    VkSampleCountFlagBits samples;
    uint32_t merge_pending, merge_armed, merge_storage, merge_continued;
    int store_discard, split_refused;           /* a storeOp DONT_CARE was chosen at begin; the split was refused once */
    uint32_t split_draws;                       /* TOTK_VK_SPLIT_PASS_DRAWS taken when this pass (part) opened */
    uint32_t feedback_pending, feedback_written, feedback_sampled, feedback_continued;
    int feedback_on, feedback_unknown, feedback_armed, feedback_restore;
} R;
static VkPipeline g_bound_pipe; static int g_dyn_dirty = 1;
static vk_record_draw *g_record_draw;  /* walking-only packet under construction, NULL on original path */
static int g_native_draw_1x;
static vk_tex *g_native_sample;                 /* only the verified terminal normalized transfer */
static void descriptor_unbind(void);
static void fg_extract_pass_begin(uint32_t cleared);
static void fg_extract_pass_end(void);
static void fg_extract_authority_span_end(VkCommandBuffer cb);
static void fg_extract_writer_end(VkCommandBuffer cb);
#ifdef TOTK_VK_TESTS
static int fg_test_writer_classification=-1;
#endif
static int fg_extract_inline(void);
static void fg_extract_refresh(int begin_pass);
static int fg_extract_ui_clear(const VkClearAttachment *attachment, const VkClearRect *rect);

/* Bounded, opt-in lifetime evidence. All event order is the recording owner's order, not GPU completion.
   No state is allocated and no descriptor walk is added with TOTK_PSO_LIFE unset. */
typedef struct {
    uint64_t image, incarnation;
    uint32_t mip, layer, aspect, stage, binding;
} pso_life_read_key;
static struct {
    int enabled;
    unsigned w, h, limit;
    uint64_t frame, start, reads, writes, unknown, total_unknown, memo_pass, memo_sub;
    uint64_t color_draws[8], depth_draws, stencil_draws;
    pso_life_read_key *memo;
    size_t nmemo;
    VkImageView color_view[8], depth_view, stencil_view;
    uint32_t color_clear, depth_clear;
} PL;
static uint64_t pso_life_number(const char *name, uint64_t fallback, uint64_t maximum) {
    const char *value = getenv(name); char *end = NULL;
    if (!value || !*value) return fallback;
    unsigned long long number = strtoull(value, &end, 10);
    return end && !*end && number && number <= maximum ? number : fallback;
}
static void pso_life_init(void) {
    const char *value = getenv("TOTK_PSO_LIFE"); char tail;
    if (!value || !*value || !strcmp(value, "0")) return;
    if (sscanf(value, "%ux%u%c", &PL.w, &PL.h, &tail) != 2 || !PL.w || !PL.h || PL.w > 16384 || PL.h > 16384) {
        LOGV("[pso-life] kind=unknown frame=1 reason=invalid-extent"); return;
    }
    PL.frame = 1; PL.start = pso_life_number("TOTK_PSO_LIFE_START", 1, UINT32_MAX);
    PL.limit = (unsigned)pso_life_number("TOTK_PSO_LIFE_FRAMES", 12000, 30000);
    PL.memo = calloc(2048, sizeof *PL.memo);
    LOGV("[pso-life] kind=begin w=%u h=%u start=%llu limit=%u", PL.w, PL.h, (unsigned long long)PL.start, PL.limit);
    if (!PL.memo) {
        LOGV("[pso-life] kind=unknown frame=%llu reason=allocation-failed", (unsigned long long)PL.start);
        LOGV("[pso-life] kind=end frame=%llu complete=0 unknown=1", (unsigned long long)PL.start);
        return;
    }
    PL.enabled = 1;
}
static int pso_life_active(void) {
    return PL.enabled && PL.frame >= PL.start && PL.frame - PL.start < PL.limit;
}
static void pso_life_unknown(const char *reason) {
    if (!pso_life_active()) return;
    PL.unknown++; PL.total_unknown++;
    LOGV("[pso-life] kind=unknown frame=%llu reason=%s", (unsigned long long)PL.frame, reason);
}
static int pso_life_match(const vk_canvas_image *image, unsigned mip) {
    return mip < 32 && MAXV(image->width >> mip, 1u) == PL.w && MAXV(image->height >> mip, 1u) == PL.h;
}
static void pso_life_write(vk_canvas_image image, unsigned mip, unsigned layer, unsigned aspect,
                           int cleared, uint64_t draws, const char *source) {
    if (!pso_life_active() || !pso_life_match(&image, mip)) return;
    PL.writes++;
    LOGV("[pso-life] kind=write frame=%llu sub=%llu pass=%llu image=%#llx incarnation=%llu mip=%u layer=%u aspect=%u "
         "w=%u h=%u format=%u serial=%llu storage=%#llx cleared=%d draws=%llu source=%s",
         (unsigned long long)PL.frame, (unsigned long long)vk_game_submit_serial(), (unsigned long long)g_stat_passes,
         (unsigned long long)image.image, (unsigned long long)image.incarnation, mip, layer, aspect, PL.w, PL.h,
         image.format, (unsigned long long)image.serial, (unsigned long long)image.storage, cleared,
         (unsigned long long)draws, source);
}
void vk_pso_life_external_write(vk_tex *x, unsigned mip, unsigned layer, unsigned layers, const char *source) {
    if (!pso_life_active() || !x) return;
    vk_canvas_image image = vk_tex_canvas_identity(x, 0, 0);
    if (!pso_life_match(&image, mip)) return;
    if (R.open) pso_life_unknown("external-write-inside-pass");
    if (layers > 256) { pso_life_unknown("write-layer-range"); return; }
    if (vk_tex_is3d(x)) { pso_life_unknown("3d-write-range"); return; }
    unsigned aspects = vk_tex_aspect(x);
    for (unsigned i = 0; i < layers; i++) for (unsigned bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
        if (aspects & bit) pso_life_write(image, mip, layer + i, bit, 0, 0, source);
}
/* Non-descriptor consumers are deliberately not certified by this diagnostic's descriptor lifetime proof. */
void vk_pso_life_external_read(uint32_t width, uint32_t height, const char *source) {
    if (pso_life_active() && width == PL.w && height == PL.h) pso_life_unknown(source);
}
static void pso_life_view_write(vk_tex *object, VkImageView view, unsigned aspect,
                                int cleared, uint64_t draws, const char *source) {
    if (!pso_life_active() || !object) return;
    if ((R.w != PL.w || R.h != PL.h) && (R.host_w != PL.w || R.host_h != PL.h)) return;
    vk_canvas_image image; VkImageSubresourceRange range;
    if (!vk_tex_view_identity(object, view, &image, &range)) { pso_life_unknown("writer-view-identity"); return; }
    int full = cleared && R.host_w == MAXV(image.width >> range.baseMipLevel, 1u) &&
        R.host_h == MAXV(image.height >> range.baseMipLevel, 1u);
    pso_life_write(image, range.baseMipLevel, range.baseArrayLayer, aspect, full, draws, source);
}
static void pso_life_pass_write(int opening) {
    if (!pso_life_active()) return;
    for (unsigned i = 0; i < R.ncolor && i < 8; i++)
        if (opening ? (PL.color_clear & (1u << i)) != 0 : PL.color_draws[i] != 0)
            pso_life_view_write(R.ctex[i], PL.color_view[i], VK_IMAGE_ASPECT_COLOR_BIT,
                (PL.color_clear & (1u << i)) != 0, opening ? 0 : PL.color_draws[i], opening ? "load-clear" : "render");
    for (unsigned bit = VK_IMAGE_ASPECT_DEPTH_BIT; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1) {
        uint64_t draws = bit == VK_IMAGE_ASPECT_DEPTH_BIT ? PL.depth_draws : PL.stencil_draws;
        if (R.dtex && (vk_tex_aspect(R.dtex) & bit) && (opening ? (PL.depth_clear & bit) != 0 : draws != 0))
            pso_life_view_write(R.dtex, bit == VK_IMAGE_ASPECT_DEPTH_BIT ? PL.depth_view : PL.stencil_view,
                bit, (PL.depth_clear & bit) != 0, opening ? 0 : draws, opening ? "load-clear" : "render");
    }
}
static void pso_life_present(void) {
    if (!PL.enabled) return;
    /* Close the old frame before advancing even when a delayed window has not started yet. */
    vk_end_rendering_at(VK_PASS_END_PRESENT, 0);
    if (pso_life_active()) {
        LOGV("[pso-life] kind=frame-end frame=%llu reads=%llu writes=%llu unknown=%llu",
             (unsigned long long)PL.frame, (unsigned long long)PL.reads, (unsigned long long)PL.writes, (unsigned long long)PL.unknown);
        if (PL.frame - PL.start + 1 == PL.limit) {
            LOGV("[pso-life] kind=end frame=%llu complete=1 unknown=%llu", (unsigned long long)PL.frame,
                 (unsigned long long)PL.total_unknown);
            PL.enabled = 0; free(PL.memo); PL.memo = NULL;
        }
    }
    PL.frame++; PL.reads = PL.writes = PL.unknown = 0; PL.nmemo = 0;
}
/* Opt-in until measured on Turnip: splitting a full clear can recover LRZ but costs another attachment load/store. */
static int store_clear_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_STORE_CLEAR"); v = e && atoi(e) == 1; } return v; }
static int clear_load_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_CLEAR_LOAD"); v = (e && atoi(e) == 1) || store_clear_on(); } return v; }
static uint64_t g_clear_load_c, g_clear_load_ds, g_clear_inline_d, g_store_clear_c, g_store_clear_ds, g_store_clear_pass;
static void gpu_work_stats(uint64_t frame) {   /* recording lock held; same 300-frame window as the GPU time */
    if (!clear_load_on()) return;
    LOGV("frame %llu, GPU work per frame: full-clear splits color %.2f, depth/stencil %.2f; inline depth clears %.2f; stores skipped color %.2f, depth/stencil %.2f in %.2f passes (CLEAR_LOAD %d STORE_CLEAR %d)",
         (unsigned long long)frame, g_clear_load_c / 300.0, g_clear_load_ds / 300.0, g_clear_inline_d / 300.0,
         g_store_clear_c / 300.0, g_store_clear_ds / 300.0, g_store_clear_pass / 300.0, clear_load_on(), store_clear_on());
    g_clear_load_c = g_clear_load_ds = g_clear_inline_d = g_store_clear_c = g_store_clear_ds = g_store_clear_pass = 0;
}

/* ---------- GPU pass profile (TOTK_VK_PASS_PROFILE=<every n-th frame>, off by default) ----------
   On a profiled frame every render pass and compute dispatch gets a timestamp before and after it (passes and dispatches
   already have a full barrier on both sides, so the timestamps add no stall of their own). Once the frame's fence has
   signalled (checked at later presents, never waited for) the log gets the frame's GPU span, the time in passes and in
   dispatches, and the passes grouped by target size and formats, costliest first. */
enum { PP_Q = 4096, PP_REC = PP_Q / 2 };
typedef struct { uint32_t q0, w, h, draws, vpw, vph; VkFormat f0, df; uint8_t kind, ncolor, native_op; } pp_rec; /* kind 0 pass, 1 dispatch, 2+ native operation; native_op is op+1 */
static struct { long every; VkQueryPool qp; uint32_t nq; pp_rec rec[PP_REC]; int nrec, open, active, pending; uint32_t dropped; uint64_t frame, fence, draws0; } PP = {.every = -1, .open = -1};
static const char *const native_op_name[VK_NUI_OPS] = {
    "canvas-seed", "window-seed", "shadow-resolve", "region-patch", "ui-pass",
    "window-pass", "present-ahb", "present-copy", "host-post"
};
static uint64_t native_count[VK_NUI_OPS], native_skipped[VK_NUI_OPS];
static uint64_t native_report_count[VK_NUI_OPS], native_report_skipped[VK_NUI_OPS];
static void pp_ts(VkCommandBuffer cb, uint32_t q) { vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, PP.qp, q); }
static int pp_begin(VkCommandBuffer cb, int kind) {   /* a timestamp before the work; the record index, or -1 */
    if (!PP.active) return -1;
    if (PP.nq + 2 > PP_Q || PP.nrec >= PP_REC) { PP.dropped++; return -1; }
    pp_rec *r = &PP.rec[PP.nrec]; memset(r, 0, sizeof *r); r->q0 = PP.nq; r->kind = (uint8_t)kind; PP.nq += 2;
    pp_ts(cb, r->q0); return PP.nrec++;
}
static void pp_end(VkCommandBuffer cb, int i) { if (i >= 0) pp_ts(cb, PP.rec[i].q0 + 1); }
int vk_native_gpu_begin(VkCommandBuffer cb, unsigned op) {
    if (op >= VK_NUI_OPS) return -1;
    native_count[op]++;
    if (vk_gpuprof_on) {
        static const char *const labels[VK_NUI_OPS] = {
            "upscale", "upscale", "transfer", "transfer", "upscale",
            "upscale", "present", "present", "post"
        };
        vk_gpuprof_scope_begin(cb, labels[op]);
    }
    return pp_begin(cb,2+(int)op);
}
void vk_native_gpu_end(VkCommandBuffer cb, int scope) {
    pp_end(cb,scope);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
}
void vk_native_gpu_skip(unsigned op) { if (op < VK_NUI_OPS) native_skipped[op]++; }
#ifdef TOTK_VK_TESTS
void vk_native_gpu_counts(uint64_t executed[VK_NUI_OPS], uint64_t skipped[VK_NUI_OPS]) {
    memcpy(executed,native_count,sizeof native_count); memcpy(skipped,native_skipped,sizeof native_skipped);
}
#endif
static void native_gpu_stats(uint64_t frame) {
    if (!vk_native_ui_enabled()) return;
    for (unsigned op=0;op<VK_NUI_OPS;op++) {
        uint64_t n=native_count[op]-native_report_count[op], skipped=native_skipped[op]-native_report_skipped[op];
        LOGV("frame %llu, native UI work per frame: %s %.3f, full-clear avoided %.3f",
            (unsigned long long)frame,native_op_name[op],n/300.0,skipped/300.0);
        native_report_count[op]=native_count[op]; native_report_skipped[op]=native_skipped[op];
    }
}
typedef struct { uint32_t w, h, n, draws, vpw, vph; VkFormat f0, df; uint8_t ncolor; double ms; } pp_group;
static int pp_group_cmp(const void *a, const void *b) { double x = ((const pp_group *)a)->ms, y = ((const pp_group *)b)->ms; return x < y ? 1 : x > y ? -1 : 0; }
static void pp_report(void) {
    PP.pending = 0;
    uint64_t *t = malloc((size_t)PP.nq * 8); if (!t) return;
    VkResult r = vkGetQueryPoolResults(V.dev, PP.qp, 0, PP.nq, (size_t)PP.nq * 8, t, 8, VK_QUERY_RESULT_64_BIT);
    if (r) { LOGV("pass profile frame %llu: query results not available (%d)", (unsigned long long)PP.frame, r); free(t); return; }
    double tick = V.props.limits.timestampPeriod / 1e6, span = (double)(t[1] - t[0]) * tick, pass_ms = 0, disp_ms = 0;
    double native_ms[VK_NUI_OPS]={0}, operation_ms=0; uint32_t native_n[VK_NUI_OPS]={0};
    static pp_group g[512]; int ng = 0, npass = 0, ndisp = 0;
    for (int i = 0; i < PP.nrec; i++) {
        const pp_rec *e = &PP.rec[i]; double ms = t[e->q0 + 1] > t[e->q0] ? (double)(t[e->q0 + 1] - t[e->q0]) * tick : 0;
        if (e->kind>=2) {
            unsigned op=e->kind-2; native_ms[op]+=ms; native_n[op]++; operation_ms+=ms; continue;
        }
        if (e->native_op) { unsigned op=e->native_op-1; native_ms[op]+=ms; native_n[op]++; }
        if (e->kind==1) { disp_ms += ms; ndisp++; continue; }
        pass_ms += ms; npass++;
        int k = 0; while (k < ng && !(g[k].w == e->w && g[k].h == e->h && g[k].f0 == e->f0 && g[k].df == e->df && g[k].ncolor == e->ncolor)) k++;
        if (k == ng) { if (ng == 512) continue; g[ng++] = (pp_group){e->w, e->h, 0, 0, 0, 0, e->f0, e->df, e->ncolor, 0}; }
        g[k].n++; g[k].draws += e->draws; g[k].ms += ms; g[k].vpw = MAXV(g[k].vpw, e->vpw); g[k].vph = MAXV(g[k].vph, e->vph);
    }
    free(t);
    qsort(g, (size_t)ng, sizeof *g, pp_group_cmp);
    LOGV("pass profile frame %llu: GPU span %.2f ms; %d passes %.2f ms, %d dispatches %.2f ms, native/host transfers %.3f ms; unscoped (copies, barriers, idle) %.2f ms, dropped scopes %u",
         (unsigned long long)PP.frame,span,npass,pass_ms,ndisp,disp_ms,operation_ms,span-pass_ms-disp_ms-operation_ms,PP.dropped);
    for (unsigned op=0;op<VK_NUI_OPS;op++) if (native_n[op])
        LOGV("pass profile frame %llu: native UI %s: %u operations, %.3f ms%s",
            (unsigned long long)PP.frame,native_op_name[op],native_n[op],native_ms[op],
            op==VK_NUI_UI_PASS || op==VK_NUI_WINDOW_PASS ? " (also included in render passes)" : "");
    for (int k = 0; k < ng && k < 16; k++)
        LOGV("pass profile frame %llu: %ux%u color %u (fmt %d) depth fmt %d: %u passes, %u draws, %.2f ms, viewport up to %ux%u",
             (unsigned long long)PP.frame, g[k].w, g[k].h, g[k].ncolor, (int)g[k].f0, (int)g[k].df, g[k].n, g[k].draws, g[k].ms, g[k].vpw, g[k].vph);
}
/* At each present (the game's frame boundary), with the recording lock held. */
static void pp_present(uint64_t n) {
    if (PP.every < 0) { const char *e = getenv("TOTK_VK_PASS_PROFILE"); PP.every = e && *e ? atol(e) : 0; if (PP.every > 0) LOGV("pass profile every %ld frames (TOTK_VK_PASS_PROFILE)", PP.every); }
    if (PP.every <= 0) return;
    if (PP.pending && mtl_wait(PP.fence, 0)) pp_report();
    if (PP.active) {   /* the profiled frame ends here */
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 1); pp_ts(vk_cb(), 1);
        PP.active = 0; PP.fence = vk_submit_now(); PP.pending = 1;
    } else if (!PP.pending && n % (uint64_t)PP.every == 0) {
        if (!PP.qp) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PP_Q}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &PP.qp)); }
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 1);
        VkCommandBuffer cb = vk_cb(); vkCmdResetQueryPool(cb, PP.qp, 0, PP_Q); pp_ts(cb, 0);
        PP.nq = 2; PP.nrec = 0; PP.dropped = 0; PP.open = -1; PP.active = 1; PP.frame = n;
    }
}
/* ---------- pass dump (TOTK_VK_PASS_DUMP=<frame>[-<last frame>][,<min width>], off by default) ----------
   For finding the pass that first draws something wrong: on that presented frame (or each frame of the range, passes
   numbered on across them, index.txt split by "== frame" lines) every pass at least <min width> (default
   640; a huge width keeps only the log) wide has its colour and depth attachments copied out when it ends. After the frame
   they are written to <TOTK_VK_SHOT_DIR>/passdump/p<pass>_<c<i>|d>_<VkFormat>_<w>x<h>.raw (tightly packed texels of the
   attachment's level and layer; the depth aspect of depth/stencil formats). index.txt lists, in recording order, every
   pass (size, attachments' guest memory, which were cleared and with what), every draw (state, vertex attributes and
   streams, textures of both stages, storage buffers), dispatch, colour clear, render condition (with the counter values
   it compared), counter report and small buffer write, and the frame's storage buffers are saved as sbuf_<address>.bin.
   android/passdump_png.py turns the attachments into PNGs. Further switches for the dumped frame:
     TOTK_VK_PASS_DUMP_SPLIT=<VkFormat of colour 0>[,<width>]  each draw of such passes ends its pass (dumped on its own)
     TOTK_VK_PASS_DUMP_BUF=<guest address>,<size>               that range copied on the GPU after every pass and dispatch
     TOTK_VK_PASS_DUMP_VSTEX=<vertex shader key prefix>         the first such draw's vertex textures and bound uniforms */
typedef struct { VkBuffer buf; vk_alloc mem; size_t n; char name[80]; } pd_ent;
static struct { long frame, last, minw; int active, npass; pd_ent *e; int ne, cap; uint64_t draws0; char *index; size_t nindex; } PD = {.frame = -2};
static void pd_note(const char *line, int n) {   /* a line of index.txt */
    if (n <= 0) return;
    PD.index = realloc(PD.index, PD.nindex + (size_t)n + 1); memcpy(PD.index + PD.nindex, line, (size_t)n + 1); PD.nindex += (size_t)n;
}
/* The vertex stage's uniform ranges as bound (renamed / snapshot copies included), recorded for TOTK_VK_PASS_DUMP_VSTEX's
   draw so the dump has the exact bytes its vertex shader read. */
static struct { int binding, kind, reg; VkDescriptorBufferInfo b; } g_pd_ubo[32]; static int g_pd_nubo, g_pd_cap, g_pd_vstex_done;
static uint32_t pd_texel(VkFormat f) {   /* bytes per texel of a copy (depth aspect for depth/stencil), 0: not dumped */
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 0;
    }
}
static void pd_copy(VkCommandBuffer cb, vk_tex *x, unsigned level, unsigned layer, int depth, const char *tag) {
    if (!R.open) vk_tex_native_read(x);
    VkFormat f = vk_tex_format(x); uint32_t bpt = pd_texel(f), w = vk_tex_width(x, level), h = vk_tex_height(x, level);
    if (!bpt || vk_tex_is3d(x)) return;
    pd_ent e = {.n = (size_t)w * h * bpt};
    snprintf(e.name, sizeof e.name, "p%04d_%s_%d_%ux%u.raw", PD.npass, tag, (int)f, w, h);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = e.n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &e.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, e.buf, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &e.mem)) { vkDestroyBuffer(V.dev, e.buf, NULL); LOGV("pass dump: no memory for %s", e.name); return; }
    VKCHECK(vkBindBufferMemory(V.dev, e.buf, e.mem.mem, e.mem.off));
    VkBufferImageCopy r = {0, 0, 0, {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}, {0, 0, 0}, {w, h, 1}};
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, e.buf, 1, &r);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (PD.ne == PD.cap) { PD.cap = PD.cap ? 2 * PD.cap : 64; PD.e = realloc(PD.e, (size_t)PD.cap * sizeof *PD.e); }
    PD.e[PD.ne++] = e;
}
static void pd_copy_range(VkCommandBuffer cb, VkBuffer src, uint64_t so, uint64_t size, const char *name) {   /* a GPU copy of a buffer range, written after the frame */
    pd_ent e = {.n = (size_t)size};
    snprintf(e.name, sizeof e.name, "%s", name);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &e.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, e.buf, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &e.mem)) { vkDestroyBuffer(V.dev, e.buf, NULL); return; }
    VKCHECK(vkBindBufferMemory(V.dev, e.buf, e.mem.mem, e.mem.off));
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    VkBufferCopy r = {so, 0, size}; vkCmdCopyBuffer(cb, src, e.buf, 1, &r);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (PD.ne == PD.cap) { PD.cap = PD.cap ? 2 * PD.cap : 64; PD.e = realloc(PD.e, (size_t)PD.cap * sizeof *PD.e); }
    PD.e[PD.ne++] = e;
}
/* TOTK_VK_PASS_DUMP_BUF=<guest address>,<size>: in the dumped frame that range is copied on the GPU after every pass and
   every dispatch (buf_<n>_<p|d><index>.bin, in order), to follow what writes it. */
static void pd_buf(VkCommandBuffer cb, char what, int index) {
    static uint64_t addr = 1, size; if (addr == 1) { const char *e = getenv("TOTK_VK_PASS_DUMP_BUF"); addr = e ? strtoull(e, NULL, 0) : 0; const char *c = e ? strchr(e, ',') : NULL; size = c ? strtoull(c + 1, NULL, 0) : 4096; }
    static int seq;
    if (!PD.active || !addr) return;
    VkBuffer src; uint64_t so; if (!vk_resolve(addr, size, &src, &so)) return;
    char name[80]; snprintf(name, sizeof name, "buf_%04d_%c%04d.bin", seq++, what, index);
    pd_copy_range(cb, src, so, size, name);
    vk_barrier_all(cb);
}
static void pd_pass_end(VkCommandBuffer cb) {   /* after vkCmdEndRendering and its barrier */
    if (!PD.active) return;
    if (R.w >= PD.minw) {
        char tag[8];
        for (uint32_t i = 0; i < R.ncolor; i++) if (R.ctex[i]) { snprintf(tag, sizeof tag, "c%u", i); pd_copy(cb, R.ctex[i], R.clevel[i], R.clayer[i], 0, tag); }
        if (R.dtex && (vk_tex_aspect(R.dtex) & VK_IMAGE_ASPECT_DEPTH_BIT)) pd_copy(cb, R.dtex, R.dlevel, R.dlayer, 1, "d");
        vk_barrier_all(cb);
    }
    char line[512]; int n = snprintf(line, sizeof line, "p%04d %ux%u colour %u depth fmt %d draws %llu cleared %#x", PD.npass, R.w, R.h, R.ncolor, (int)R.dfmt, (unsigned long long)(g_stat_draws - PD.draws0), R.cleared);
    for (uint32_t i = 0; i < R.ncolor && i < 8 && n < (int)sizeof line - 40; i++) if (R.caddr[i]) n += snprintf(line + n, sizeof line - (size_t)n, " c%u@%p", i, R.caddr[i]);
    if (R.daddr && n < (int)sizeof line - 40) n += snprintf(line + n, sizeof line - (size_t)n, " d@%p", R.daddr);
    for (uint32_t i = 0; i < 8 && n < (int)sizeof line - 60; i++) if (R.cleared >> i & 1)   /* the clear value's raw words (and its float reading) */
        n += snprintf(line + n, sizeof line - (size_t)n, " clear%u=%08x,%08x,%08x,%08x(%g)", i, R.cc[i].color.uint32[0], R.cc[i].color.uint32[1], R.cc[i].color.uint32[2], R.cc[i].color.uint32[3], R.cc[i].color.float32[0]);
    if (n < (int)sizeof line - 1) { line[n++] = '\n'; line[n] = 0; }
    pd_note(line, n);
    pd_buf(cb, 'p', PD.npass);
    PD.npass++;
}
/* TOTK_VK_PASS_DUMP_SPLIT=<VkFormat of colour 0>[,<width>]: in the dumped frame each draw of such passes ends its pass, so
   every draw's result is dumped on its own, with the draw's state in index.txt above its pass line. The first draw of such
   a pass finds colour 0 filled with magenta, so what it leaves magenta it did not write (and nothing earlier did). */
static int pd_split(void) {
    if (!PD.active) return 0;
    static long fmt = -2, width; if (fmt == -2) { const char *e = getenv("TOTK_VK_PASS_DUMP_SPLIT"); fmt = e && *e ? atol(e) : -1; const char *c = e ? strchr(e, ',') : NULL; width = c ? atol(c + 1) : 0; }
    return fmt >= 0 && (long)R.cfmt[0] == fmt && (!width || (long)R.w == width);
}
static int g_pd_split_seen;   /* a split pass has had its first draw this frame */
static void pd_pre_draw(VkCommandBuffer cb) {
    if (!pd_split() || g_pd_split_seen) return;
    g_pd_split_seen = 1;
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color.float32 = {1, 0, 1, 1}}};
    VkClearRect cr = {{{0, 0}, {R.w, R.h}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
}
/* Storage buffers a stage binds (as draw_touch resolves them): noted in index.txt, and their bytes (up to 64 KiB each) are
   written as sbuf_<address>.bin after the frame, to see what a draw or dispatch read. */
static struct { uint64_t addr, size; } g_pd_sb[64]; static int g_pd_nsb;
static int pd_sbufs(int st, const vk_shader *s, char *out, size_t cap) {
    int n = 0;
    for (int i = 0; s && i < s->nb && (size_t)n < cap; i++) {
        const vk_bind *b = &s->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->reg < 0 || b->reg >= 16) continue;
        uint64_t addr = 0, size = 0;
        if (b->kind == RB_SBUF) { addr = S.ssbo[st][b->reg].addr; size = S.ssbo[st][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { addr = S.ubo[st][b->reg].addr; size = S.ubo[st][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) { const p_bind_buf *u = &S.ubo[st][b->reg]; if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_read(u->addr + b->off, &addr, 8); }
        n += snprintf(out + n, cap - (size_t)n, " s%d.%d@%#llx+%#llx", st, b->reg, (unsigned long long)addr, (unsigned long long)size);
        if (!addr) continue;
        int k = 0; while (k < g_pd_nsb && g_pd_sb[k].addr != addr) k++;
        if (k == g_pd_nsb && g_pd_nsb < 64) g_pd_sb[g_pd_nsb++].addr = addr;
        if (k < 64) g_pd_sb[k].size = MAXV(g_pd_sb[k].size, MINV(size ? size : 65536, 65536));
    }
    return n;
}
static void pd_draw(int prim, uint32_t count, uint32_t inst, int indexed, vk_shader *const *sh) {   /* every draw of the dumped frame */
    if (!PD.active) return;
    char line[2600], sb[2048] = "";
    int ns = pd_sbufs(0, sh[0], sb, sizeof sb); if (sh[1] && (size_t)ns < sizeof sb) ns += pd_sbufs(1, sh[1], sb + ns, sizeof sb - (size_t)ns);
    for (int a = 0; a < 16 && (size_t)ns < sizeof sb; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) {   /* vertex attributes: NVN format, stream, offset, stride */
        int st = S.attr[a].stream & 15;
        ns += snprintf(sb + ns, sizeof sb - (size_t)ns, " a%d=%#x/s%d+%u/%u", a, (unsigned)S.attr[a].fmt, st, (unsigned)S.attr[a].offset, (unsigned)S.stream[st].stride);
        VkBuffer vb_; uint64_t vo_, avail_ = 0; int res = S.vbuf[st].address && vk_resolve_span(S.vbuf[st].address, &vb_, &vo_, &avail_);
        if ((size_t)ns < sizeof sb) ns += snprintf(sb + ns, sizeof sb - (size_t)ns, "@%#llx+%#llx(pool %d, avail %#llx)", (unsigned long long)S.vbuf[st].address, (unsigned long long)S.vbuf[st].size, res, (unsigned long long)avail_);
    }
    for (int st = 0; st < 2; st++) for (int i = 0; sh[st] && i < sh[st]->nb && (size_t)ns < sizeof sb; i++) {   /* textures: stage, slot, handle, and what it resolves to */
        const vk_bind *b = &sh[st]->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        uint64_t h = b->reg >= 0 && b->reg < 128 ? S.tex[st][b->reg] : 0; vk_treg r = {0};
        int ok = h && vk_treg_get((int)(h & 0xfffff), &r);
        ns += snprintf(sb + ns, sizeof sb - (size_t)ns, " %ct%d=%#llx", st ? 'f' : 'v', b->reg, (unsigned long long)h);
        if (ok && (size_t)ns < sizeof sb) ns += snprintf(sb + ns, sizeof sb - (size_t)ns, "(fmt %#x %ux%ux%u target %d @%p)", r.t.d.format, r.t.d.width, r.t.d.height, r.t.d.depth, r.t.d.target, (const void *)r.t.mem);
    }
    int n = snprintf(line, sizeof line, "  draw prim %d count %u inst %u%s%.0d cull %d front %d depth test %d write %d func %d clamp %d fs depth %d blend %#x vp %.0f,%.0f %.0fx%.0f vs %s fs %s%s\n",
                     prim, count, inst, indexed ? " idx-bytes " : "", indexed == 1 ? 1 : indexed == 2 ? 2 : indexed ? 4 : 0, (int)S.poly.cull, (int)S.poly.front, S.ds.dt, S.ds.dw, (int)S.ds.df, S.depth_clamp, sh[1] ? sh[1]->writes_depth : 0,
                     (unsigned)S.color.blend_en, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], sh[0]->key, sh[1] ? sh[1]->key : "-", sb);
    pd_note(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
    if (g_pd_cap) {   /* TOTK_VK_PASS_DUMP_VSTEX's draw: its vertex-stage textures (every layer of level 0) and its bound uniform ranges */
        g_pd_cap = 0; g_pd_vstex_done = 1; vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2); VkCommandBuffer cb = vk_cb();
        for (int i = 0; i < sh[0]->nb; i++) {
            const vk_bind *b = &sh[0]->b[i];
            if ((b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) || b->reg < 0 || b->reg >= 128 || !S.tex[0][b->reg]) continue;
            int want = b->target; vk_tex *x = NULL; vk_tex_by_id((int)(S.tex[0][b->reg] & 0xfffff), &want, &x); if (!x) continue;
            uint32_t nl = vk_tex_layers(x); if (nl > 16) nl = 16;
            for (uint32_t l = 0; l < nl; l++) { char tag[24]; snprintf(tag, sizeof tag, "vt%d-%u", b->reg, l); pd_copy(cb, x, 0, l, 0, tag); }
        }
        for (int i = 0; i < g_pd_nubo; i++) {
            char name[80]; snprintf(name, sizeof name, "p%04d_ubo-binding%d-kind%d-reg%d.bin", PD.npass, g_pd_ubo[i].binding, g_pd_ubo[i].kind, g_pd_ubo[i].reg);
            if (g_pd_ubo[i].b.buffer && g_pd_ubo[i].b.range && g_pd_ubo[i].b.range != VK_WHOLE_SIZE) pd_copy_range(cb, g_pd_ubo[i].b.buffer, g_pd_ubo[i].b.offset, g_pd_ubo[i].b.range, name);
        }
        vk_barrier_all(cb);
    }
    if (pd_split()) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2);
}
static void pd_dispatch(const vk_shader *cs, const uint32_t grid[3]) {
    if (!PD.active) return;
    char line[640], sb[384] = ""; pd_sbufs(5, cs, sb, sizeof sb);
    int n = snprintf(line, sizeof line, "  dispatch %ux%ux%u cs %s%s\n", grid[0], grid[1], grid[2], cs->key, sb);
    pd_note(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
}
static void pd_present(uint64_t n) {   /* at each present, with the recording lock held */
    if (PD.frame == -2) {
        const char *e = getenv("TOTK_VK_PASS_DUMP"); PD.frame = e && *e ? atol(e) : -1; PD.minw = 640;
        const char *d = e ? strchr(e, '-') : NULL, *c = e ? strchr(e, ',') : NULL;
        PD.last = d && (!c || d < c) ? atol(d + 1) : PD.frame; if (PD.last < PD.frame) PD.last = PD.frame;
        if (c) PD.minw = atol(c + 1);
        if (PD.frame >= 0) LOGV("pass dump of frames %ld-%ld, passes at least %ld wide (TOTK_VK_PASS_DUMP)", PD.frame, PD.last, PD.minw);
    }
    if (PD.frame < 0) return;
    if (PD.active && PD.frame < PD.last) {   /* a range: the next frame goes on in the same dump, its passes numbered on */
        PD.frame++; char l[48]; pd_note(l, snprintf(l, sizeof l, "== frame %ld\n", PD.frame));
    } else if (PD.active) {   /* the (last) dumped frame ends here */
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2); PD.active = 0;
        if (!mtl_wait(vk_submit_now(), 10000000000ull)) LOGV("pass dump: the GPU did not finish the frame");
        const char *dir = getenv("TOTK_VK_SHOT_DIR"); if (!dir || !*dir) dir = "shots";
        char path[1024]; snprintf(path, sizeof path, "%s/passdump", dir); mkdir(dir, 0755); mkdir(path, 0755);
        size_t bytes = 0;
        for (int i = 0; i < PD.ne; i++) {
            pd_ent *e = &PD.e[i]; void *p;
            snprintf(path, sizeof path, "%s/passdump/%s", dir, e->name);
            if (!vkMapMemory(V.dev, e->mem.mem, e->mem.off, e->n, 0, &p)) {
                FILE *f = fopen(path, "wb"); if (f) { fwrite(p, 1, e->n, f); fclose(f); bytes += e->n; }
                vkUnmapMemory(V.dev, e->mem.mem);
            }
            vkDestroyBuffer(V.dev, e->buf, NULL); vk_mem_free(&e->mem);
        }
        for (int i = 0; i < g_pd_nsb; i++) {   /* guest memory: the pools are mapped at the guest address */
            snprintf(path, sizeof path, "%s/passdump/sbuf_%llx.bin", dir, (unsigned long long)g_pd_sb[i].addr);
            VkBuffer b; uint64_t o;
            if (!vk_resolve(g_pd_sb[i].addr, g_pd_sb[i].size, &b, &o)) continue;
            FILE *f = fopen(path, "wb"); if (f) { fwrite((const void *)(uintptr_t)g_pd_sb[i].addr, 1, (size_t)g_pd_sb[i].size, f); fclose(f); }
        }
        g_pd_nsb = 0;
        snprintf(path, sizeof path, "%s/passdump/index.txt", dir);
        FILE *f = fopen(path, "w"); if (f) { if (PD.index) fputs(PD.index, f); fclose(f); }
        LOGV("pass dump of frame %ld: %d passes, %d attachments, %.1f MiB in %s/passdump", PD.frame, PD.npass, PD.ne, bytes / 1048576.0, dir);
        free(PD.e); PD.e = NULL; PD.ne = PD.cap = 0; free(PD.index); PD.index = NULL; PD.nindex = 0; PD.frame = -1;
    } else if (n + 1 == (uint64_t)PD.frame) {   /* the next frame is dumped */
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2);
        PD.active = 1; PD.npass = 0; g_pd_split_seen = 0;
        char l[48]; pd_note(l, snprintf(l, sizeof l, "== frame %ld\n", PD.frame));
    }
}
static int begin_rendering(int draw_tail);
int vk_in_pass(void) { return R.open; }
static void pd_pass(VkCommandBuffer cb);
static void pd_draw_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int pd_split_match(const char *key);
static int pd_active(void);
static uint64_t g_samples_cum;   /* default synthetic SAMPLES_PASSED: Reset zeroes, each emitted draw adds */
uint64_t vk_counter_samples(void) {
    static int ordered = -1; if (ordered < 0) ordered = !vk_off("condorder");
    return ordered ? g_samples_cum : 1u << 20;
}
/* DEPTH16 EQUAL class markers land on rounding ties. Nudge rasterized AND fragment-written depth together:
   the raster-only experiment broke clothes, but the complete fix was confirmed on the Thor (DEBUGGING.md). */
#define D16_NUDGE_UNITS 0.2f
/* On by default (2026-10-22: the dark-water fix, confirmed on the Thor with fragment-written depth nudged too);
   TOTK_VK_D16TIE=0 or TOTK_VK_OFF=d16tie: off. */
static int d16tie_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_D16TIE"); on = !(e && !atoi(e)) && !vk_off("d16tie"); } return on; }
static float d16_clear(float v) { double t = (double)v * 65535.0; return d16tie_on() && fabs(t - floor(t) - 0.5) < 1e-3 ? (float)(v + D16_NUDGE_UNITS / 65535.0) : v; }
/* what the open pass's draws read (pass probe report) and what could not be found: a texture id that resolves to nothing
   is bound as an all-zero dummy, a buffer outside the pools as zeros (both counted, and logged the first times) */
typedef struct { uint64_t img; uint32_t w, h; int fmt; vk_tex *x; } probe_in;
static probe_in g_pin[16]; static int g_npin, g_pdummy, g_pzero; static char g_pkey[24];
static uint64_t g_stat_dummy_tex, g_stat_zero_buf;
typedef struct { VkBuffer b; VkDeviceSize off, n; uint64_t addr; uint8_t stage, reg, storage; } probe_buf;
enum { PROBE_BUFS = 12, PROBE_BUF_BYTES = 256 };
static probe_buf g_pbuf[PROBE_BUFS]; static int g_npbuf; static uint8_t g_pbuf_data[PROBE_BUFS][PROBE_BUF_BYTES];
typedef struct { char vs[17]; int prim; uint32_t count, first, inst; float vp[4], dr[2], bias[3]; int clamp, cull, has_sc; int32_t sc[4]; float v0[8]; float c11_25[4], c4_147[4], c4_7[4]; } probe_draw;
static probe_draw g_pdraw[4]; static int g_npdraw;
typedef struct { int clears, clears_skipped, clears_inpass, setrt, cond, cond_off, disabled_at_start; float cc[4]; } probe_ops;   /* between the previous pass's end and this pass's draws */
static probe_ops g_pops;
static void pass_inputs_reset(void) { g_npin = 0; g_pdummy = g_pzero = 0; g_pkey[0] = 0; g_npbuf = 0; g_npdraw = 0; }
static void pass_buf_note(int stage, int reg, int storage, uint64_t addr, const VkDescriptorBufferInfo *d) {
    if (!R.open || stage > 1) return;
    for (int k = 0; k < g_npbuf; k++) if (g_pbuf[k].stage == stage && g_pbuf[k].reg == reg && g_pbuf[k].b == d->buffer && g_pbuf[k].off == d->offset) return;
    if (g_npbuf >= PROBE_BUFS || !addr) return;
    /* Uniform diagnostics use the ordered CPU-known updates/snapshot, not guest bytes that an earlier chunk's
       OP_UPDATE_UB may still be writing. Storage diagnostics retain their existing raw guest-memory semantics. */
    uint64_t n = MINV(d->range, (VkDeviceSize)PROBE_BUF_BYTES); VkBuffer pb; uint64_t po, avail;
    if (!vk_resolve_span(addr, &pb, &po, &avail)) return;
    n = MINV(n, avail);
    if (storage) memcpy(g_pbuf_data[g_npbuf], (const void *)(uintptr_t)addr, (size_t)n);
    else vk_ub_read(addr, g_pbuf_data[g_npbuf], (size_t)n);
    g_pbuf[g_npbuf++] = (probe_buf){d->buffer, d->offset, n, addr, (uint8_t)stage, (uint8_t)reg, (uint8_t)(storage | (vk_ub_pending(addr, n) ? 2 : 0))};
}
/* ---------- pass probe (TOTK_VK_PASSPROBE=1, diagnostic) ----------
   After every pass, colour target 0 is shrunk to 8x8 texels (blit, nearest) into a row of an RGBA16F atlas; the rows of
   the last PROBE_FRAMES frames are kept. When the flash detector (black_check) confirms a dark frame, the probes of that
   frame and the good frame after it (the same view, after a camera cut) are compared pass by pass and the passes whose output differs most are logged: the first
   pass that goes dark (or NaN) in the flash frame is where the picture is lost. */
enum { PROBE_FRAMES = 16, PROBE_SLOTS = 256 };
typedef struct { uint64_t serial, draws, img; uint32_t w, h; int fmt; probe_in in[16]; int nin, dummy, zero; char key[24]; probe_buf buf[12]; int nbuf; uint32_t dt, dw, df, st, sf, sref, svm, chmask, blend; probe_draw dr[4]; int ndr, cleared; uint64_t dimg; probe_ops ops; } probe_meta;
/* other writes into textures (depth passes, copies, clears, compute stores), in order: `at` is the number of colour
   passes probed before it, so "written before pass N" can be told */
enum { PROBE_WRITES = 384 };
typedef struct { uint64_t img, src; uint32_t w, h, sw, sh; int fmt, sfmt, at; char kind; uint32_t draws; int dsamp; VkFormat vf; probe_in in[16]; int nin; char key[24]; probe_draw d0; int nd; } probe_write;   /* dsamp: depth samples taken (D) */
typedef struct { _Atomic uint64_t frame; uint64_t fence; int n, nw; probe_meta m[PROBE_SLOTS]; probe_write wr[PROBE_WRITES]; } probe_frame;
static probe_frame g_probe[PROBE_FRAMES];
static VkImage g_probe_img; static vk_alloc g_probe_mem; static VkBuffer g_probe_buf; static VkDeviceMemory g_probe_bmem; static uint16_t *g_probe_ptr; static int g_probe_coherent;
#define PROBE_DEPTH_OFF ((VkDeviceSize)PROBE_FRAMES * PROBE_SLOTS * 64 * 8)   /* depth samples after the colour rows: 64 x 4 bytes per write */
#define PROBE_BUF_OFF (PROBE_DEPTH_OFF + (VkDeviceSize)PROBE_FRAMES * PROBE_WRITES * 64 * 4)   /* then the first 256 bytes of each pass's uniform / storage buffers */
static uint64_t g_probe_frame = 1; static int g_probe_n;
static int probe_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_PASSPROBE"); on = e && atoi(e); if (on) LOGV("TOTK_VK_PASSPROBE: every pass's colour target is sampled (8x8) for the flash report"); } return on; }
static int probe_bufs_on(void) { return probe_on(); }   /* CPU-side reads: cheap (a GPU copy per buffer stalled the game for tens of seconds per frame) */
static int probe_init(void) {
    static int state; if (state) return state > 0; state = -1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {8, 8 * PROBE_SLOTS, 1}, .mipLevels = 1,
        .arrayLayers = PROBE_FRAMES, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (vkCreateImage(V.dev, &ci, NULL, &g_probe_img)) return 0;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, g_probe_img, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_probe_mem)) return 0;
    vkBindImageMemory(V.dev, g_probe_img, g_probe_mem.mem, g_probe_mem.off);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PROBE_BUF_OFF + (VkDeviceSize)PROBE_FRAMES * PROBE_SLOTS * 12 * 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, &g_probe_buf)) return 0;
    vkGetBufferMemoryRequirements(V.dev, g_probe_buf, &req);
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vk_memory_allocate(&mai, &g_probe_bmem, VK_MEMORY_DIAGNOSTIC)) return 0;
    vkBindBufferMemory(V.dev, g_probe_buf, g_probe_bmem, 0);
    if (vkMapMemory(V.dev, g_probe_bmem, 0, VK_WHOLE_SIZE, 0, (void **)&g_probe_ptr)) return 0;
    g_probe_coherent = !!(V.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_probe_img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, PROBE_FRAMES}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(vk_cb(), &di);
    state = 1; return 1;
}
static void probe_pass(VkCommandBuffer cb) {
    if (!probe_on() || !R.ctex[0] || g_probe_n >= PROBE_SLOTS || !probe_init()) return;
    vk_tex *x = R.ctex[0];
    vk_tex_native_read(x);
    if (vk_tex_is3d(x) || !(vk_tex_aspect(x) & VK_IMAGE_ASPECT_COLOR_BIT) || R.samples > 1) return;   /* multisampled images cannot be blitted */
    VkFormat f = vk_tex_format(x); VkFormatProperties fp; V.get_format_props(V.pd, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return;
    { int bw, bh; nvn_fmt_bpb(vk_tex_nvn_format(x), &bw, &bh); if (bw > 1 || bh > 1) return; }
    if ((f >= VK_FORMAT_R8_UINT && f <= VK_FORMAT_R8_SINT) || f == VK_FORMAT_R8G8_UINT || f == VK_FORMAT_R8G8_SINT || f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R8G8B8A8_SINT ||
        f == VK_FORMAT_R16_UINT || f == VK_FORMAT_R16_SINT || f == VK_FORMAT_R16G16_UINT || f == VK_FORMAT_R16G16_SINT || f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R16G16B16A16_SINT ||
        f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32_SINT || f == VK_FORMAT_R32G32_UINT || f == VK_FORMAT_R32G32_SINT || f == VK_FORMAT_R32G32B32A32_UINT || f == VK_FORMAT_R32G32B32A32_SINT ||
        f == VK_FORMAT_A2B10G10R10_UINT_PACK32) return;   /* integer formats cannot be blitted to float */
    if (R.c0_layer >= vk_tex_layers(x) || R.c0_level >= vk_tex_levels(x)) return;
    int slot = g_probe_n++, fr = (int)(g_probe_frame % PROBE_FRAMES);
    uint32_t w = vk_tex_width(x, R.c0_level), h = vk_tex_height(x, R.c0_level);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, R.c0_level, R.c0_layer, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)fr, 1}, .dstOffsets = {{0, slot * 8, 0}, {8, slot * 8 + 8, 1}}};
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdBlitImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_probe_img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    probe_meta *pm = &g_probe[fr].m[slot];
    *pm = (probe_meta){.serial = R.c0_serial, .draws = g_stat_draws - R.draws0, .img = (uint64_t)(uintptr_t)vk_tex_image(x), .w = w, .h = h, .fmt = R.c0_fmt, .nin = g_npin, .dummy = g_pdummy, .zero = g_pzero};
    memcpy(pm->in, g_pin, sizeof g_pin); memcpy(pm->key, g_pkey, sizeof g_pkey);
    if (!probe_bufs_on() || g_stat_draws - R.draws0 > 8) g_npbuf = 0;   /* buffers: screen-space passes only */
    pm->nbuf = g_npbuf; memcpy(pm->buf, g_pbuf, sizeof g_pbuf);
    pm->ndr = g_npdraw; memcpy(pm->dr, g_pdraw, sizeof g_pdraw); pm->cleared = R.c0_cleared; pm->ops = g_pops; memset(&g_pops, 0, sizeof g_pops); pm->dimg = R.dtex ? (uint64_t)(uintptr_t)vk_tex_image(R.dtex) : 0;
    pm->dt = S.ds.dt; pm->dw = S.ds.dw; pm->df = S.ds.df; pm->st = S.ds.st; pm->sf = S.ds.sf_f; pm->sref = S.st_ref[0]; pm->svm = S.st_vmask[0]; pm->chmask = S.chmask.mask; pm->blend = S.color.blend_en;   /* as of its last draw */
    for (int k = 0; k < g_npbuf; k++)   /* into the probe buffer's host memory (no GPU work) */
        memcpy((uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)fr * PROBE_SLOTS + (size_t)slot) * 12 + (size_t)k) * 256, g_pbuf_data[k], (size_t)g_pbuf[k].n);
}
static void probe_frame_end(uint64_t frame_no) {   /* at present: copy this frame's rows out, start the next frame */
    if (!probe_on() || !g_probe_img) { g_probe_n = 0; return; }
    int fr = (int)(g_probe_frame % PROBE_FRAMES);
    probe_frame *p = &g_probe[fr];
    atomic_store(&p->frame, 0);
    if (g_probe_n) {
        VkCommandBuffer cb = vk_cb();
        VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)fr * PROBE_SLOTS * 64 * 8, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)fr, 1}, .imageExtent = {8, 8 * (uint32_t)g_probe_n, 1}};
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
        vkCmdCopyImageToBuffer(cb, g_probe_img, VK_IMAGE_LAYOUT_GENERAL, g_probe_buf, 1, &c);
        vk_barrier_all(cb);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    p->n = g_probe_n; p->fence = mtl_last_fence() + 1;
    atomic_store(&p->frame, frame_no);
    g_probe_frame++; g_probe_n = 0;
    probe_frame *q = &g_probe[g_probe_frame % PROBE_FRAMES]; atomic_store(&q->frame, 0); q->nw = 0;
}
static void probe_write_note(char kind, vk_tex *x, vk_tex *src, uint32_t draws) {   /* replay thread */
    if (!probe_on() || !x) return;
    probe_frame *p = &g_probe[g_probe_frame % PROBE_FRAMES];
    if (p->nw >= PROBE_WRITES) return;
    int wi = p->nw++;
    probe_write *w = &p->wr[wi];
    *w = (probe_write){(uint64_t)(uintptr_t)vk_tex_image(x), src ? (uint64_t)(uintptr_t)vk_tex_image(src) : 0, vk_tex_width(x, 0), vk_tex_height(x, 0),
        src ? vk_tex_width(src, 0) : 0, src ? vk_tex_height(src, 0) : 0, vk_tex_nvn_format(x), src ? vk_tex_nvn_format(src) : 0, g_probe_n, kind, draws};
    w->vf = vk_tex_format(x);
    if (kind == 'D') {   /* what the depth pass read, and its depth at 8x8 points */
        w->nin = g_npin; memcpy(w->in, g_pin, sizeof g_pin); memcpy(w->key, g_pkey, sizeof g_pkey); w->nd = g_npdraw; if (g_npdraw) w->d0 = g_pdraw[0];
        if (draws <= 8 && probe_init() && R.samples <= 1 && !vk_tex_is3d(x) && (vk_tex_aspect(x) & VK_IMAGE_ASPECT_DEPTH_BIT) && R.dlayer < vk_tex_layers(x) && R.dlevel < vk_tex_levels(x)) {
            uint32_t lw = vk_tex_width(x, R.dlevel), lh = vk_tex_height(x, R.dlevel);
            VkBufferImageCopy r[64]; VkDeviceSize base = PROBE_DEPTH_OFF + ((VkDeviceSize)(p - g_probe) * PROBE_WRITES + (VkDeviceSize)wi) * 64 * 4;
            for (int k = 0; k < 64; k++) r[k] = (VkBufferImageCopy){.bufferOffset = base + (VkDeviceSize)k * 4, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, R.dlevel, R.dlayer, 1},
                .imageOffset = {(int32_t)(lw * (2 * (k % 8) + 1) / 16), (int32_t)(lh * (2 * (k / 8) + 1) / 16), 0}, .imageExtent = {1, 1, 1}};
            VkCommandBuffer cb = vk_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
            vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_probe_buf, 64, r);
            vk_barrier_all(cb);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            w->dsamp = 1;
        }
    }
}
static uint32_t g_d16_lo, g_d16_hi;
static int depth_stats(const probe_frame *p, int wi, double *mn, double *mean, double *mx) {
    const probe_write *w = &p->wr[wi]; if (!w->dsamp) return 0;
    const uint8_t *q = (const uint8_t *)g_probe_ptr + PROBE_DEPTH_OFF + ((size_t)(p - g_probe) * PROBE_WRITES + (size_t)wi) * 64 * 4;
    double s = 0; *mn = 1e30; *mx = -1e30;
    for (int k = 0; k < 64; k++) {
        uint32_t u; memcpy(&u, q + k * 4, 4); double v;
        if (w->vf == VK_FORMAT_D16_UNORM || w->vf == VK_FORMAT_D16_UNORM_S8_UINT) { v = (u & 0xffff) / 65535.0; if (k == 0 || (u & 0xffff) < g_d16_lo) g_d16_lo = u & 0xffff; if (k == 0 || (u & 0xffff) > g_d16_hi) g_d16_hi = u & 0xffff; }
        else if (w->vf == VK_FORMAT_D32_SFLOAT || w->vf == VK_FORMAT_D32_SFLOAT_S8_UINT) { float f; memcpy(&f, &u, 4); v = f; }
        else v = (u & 0xffffff) / 16777215.0;
        s += v; if (v < *mn) *mn = v; if (v > *mx) *mx = v;
    }
    *mean = s / 64; return 1;
}
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | e << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13; else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static const probe_frame *probe_find(uint64_t frame_no) { for (int i = 0; i < PROBE_FRAMES; i++) if (atomic_load(&g_probe[i].frame) == frame_no) return &g_probe[i]; return NULL; }
static void probe_stats(const probe_frame *p, int slot, double *mean, int *nan) {
    const uint16_t *q = g_probe_ptr + ((size_t)(p - g_probe) * PROBE_SLOTS + (size_t)slot) * 64 * 4;
    double s = 0; int n = 0;
    for (int i = 0; i < 64; i++) { for (int c = 0; c < 3; c++) { float v = half_f(q[i * 4 + c]); if (v != v || v > 65504.f || v < -65504.f) n++; else s += v; } }
    *mean = s / 192; *nan = n;
}
static int probe_report_inner(uint64_t flash, uint64_t before) {   /* caller serializes report scratch */
    if (!probe_on() || !g_probe_ptr) return -1;
    const probe_frame *a = probe_find(flash), *b = probe_find(before);
    if (!a || !b) { LOGV("pass probe: frames %llu / %llu are no longer kept", (unsigned long long)flash, (unsigned long long)before); return -1; }
    mtl_wait(a->fence, 2000000000ull); mtl_wait(b->fence, 2000000000ull);
    if (!g_probe_coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = g_probe_bmem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
    LOGV("pass probe, flash frame %llu (%d passes probed) vs the good frame %llu (%d): passes whose colour output differs (mean RGB, bad = NaN/inf texels of 192)",
         (unsigned long long)flash, a->n, (unsigned long long)before, b->n);
    /* the two frames need not have the same passes (shadow / cube faces take turns): pair them up by size and format
       (longest common subsequence), then compare the pairs */
    static uint16_t L[PROBE_SLOTS + 1][PROBE_SLOTS + 1];
    int na_ = a->n, nb_ = b->n;
    #define PSAME(i, j) (a->m[i].w == b->m[j].w && a->m[i].h == b->m[j].h && a->m[i].fmt == b->m[j].fmt)
    for (int i = 0; i <= na_; i++) L[i][nb_] = 0; for (int j = 0; j <= nb_; j++) L[na_][j] = 0;
    for (int i = na_ - 1; i >= 0; i--) for (int j = nb_ - 1; j >= 0; j--) L[i][j] = PSAME(i, j) ? (uint16_t)(L[i + 1][j + 1] + 1) : MAXV(L[i + 1][j], L[i][j + 1]);
    int shown = 0, paired = 0;
    for (int i = 0, j = 0; i < na_ && j < nb_; ) {
        if (PSAME(i, j)) {
            paired++;
            double ma, mb; int bad_a, bad_b; probe_stats(a, i, &ma, &bad_a); probe_stats(b, j, &mb, &bad_b);
            if ((bad_a != bad_b || fabs(ma - mb) > 0.1 * (fabs(ma) + fabs(mb)) + 0.01) && shown < 60) {
                shown++;
                const probe_meta *x = &a->m[i], *y = &b->m[j];
                LOGV("  pass %3d (good %3d): %ux%u fmt %#x, %llu draws (good %llu): mean %.4f bad %d, good %.4f bad %d",
                     i, j, x->w, x->h, x->fmt, (unsigned long long)x->draws, (unsigned long long)y->draws, ma, bad_a, mb, bad_b);
                for (int f = 0; f < 2; f++) {   /* what the pass read, in the flash frame and in the good one */
                    const probe_frame *pf = f ? b : a; const probe_meta *m = f ? y : x; int at = f ? j : i; const probe_write *dps[4]; int ndp = 0;
                    char line[2400]; size_t o = (size_t)snprintf(line, sizeof line, "      %s: fs %s, %d missing textures, %d buffers outside the pools; reads", f ? "good " : "flash", m->key[0] ? m->key : "?", m->dummy, m->zero);
                    for (int k = 0; k < m->nin && o + 80 < sizeof line; k++) {
                        char fb[160]; int from = -1; for (int q = at - 1; q >= 0; q--) if (pf->m[q].img == m->in[k].img) { from = q; break; }
                        const probe_write *wl = NULL; for (int q = pf->nw - 1; q >= 0; q--) if (pf->wr[q].img == m->in[k].img && pf->wr[q].at <= at && (from < 0 || pf->wr[q].at > from)) { wl = &pf->wr[q]; break; }
                        if (wl) snprintf(fb, sizeof fb, "%s before pass %d%s", wl->kind == 'D' ? "depth pass" : wl->kind == 'C' ? "copy" : wl->kind == 'K' ? "clear" : wl->kind == 'X' ? "image store" : "upload", wl->at,
                                         wl->kind == 'C' ? "" : "");
                        if (wl && wl->kind == 'C') snprintf(fb + strlen(fb), sizeof fb - strlen(fb), " from %ux%u %#x", wl->sw, wl->sh, wl->sfmt);
                        if (wl && wl->kind == 'D') { double dn, dm, dx; snprintf(fb + strlen(fb), sizeof fb - strlen(fb), ", %u draws", wl->draws);
                            if (depth_stats(pf, (int)(wl - pf->wr), &dn, &dm, &dx)) snprintf(fb + strlen(fb), sizeof fb - strlen(fb), ", depth %.4f..%.4f mean %.4f", dn, dx, dm);
                            if (ndp < 4) dps[ndp++] = wl; }
                        else if (from >= 0) snprintf(fb, sizeof fb, "from pass %d", from);
                        o += (size_t)snprintf(line + o, sizeof line - o, " [%ux%u %#x %s]", m->in[k].w, m->in[k].h, m->in[k].fmt, (wl || from >= 0) ? fb : "not written this frame");
                    }
                    if (!m->nin) snprintf(line + o, sizeof line - o, " no textures");
                    LOGV("%s", line);
                    LOGV("        state (last draw): depth test %u func %u write %u, stencil %u func %u ref %#x mask %#x, colour mask %#x, blend %#x",
                         m->dt, m->df, m->dw, m->st, m->sf, m->sref, m->svm, m->chmask, m->blend);
                    { int seen_c = 0, seen_d = 0, wrote_c = -1, wrote_d = -1; const probe_frame *o = f ? a : b;   /* the colour / depth images: also in the other frame? written earlier in this one? */
                      for (int q = 0; q < o->n; q++) { if (o->m[q].img == m->img) seen_c = 1; if (m->dimg && o->m[q].dimg == m->dimg) seen_d = 1; }
                      for (int q = 0; q < at; q++) { if (pf->m[q].img == m->img) wrote_c = q; if (m->dimg && pf->m[q].dimg == m->dimg) wrote_d = q; }
                      LOGV("        since the pass before: %d colour clears issued (%d skipped: render disabled, %d inside a pass), last clear colour %g %g %g %g; %d SetRenderTargets, %d conditional-render ops (%d disabled); render %s at pass start",
                           m->ops.clears, m->ops.clears_skipped, m->ops.clears_inpass, m->ops.cc[0], m->ops.cc[1], m->ops.cc[2], m->ops.cc[3], m->ops.setrt, m->ops.cond, m->ops.cond_off, m->ops.disabled_at_start ? "DISABLED" : "enabled");
                      LOGV("        colour target %#llx: %s, %s the other frame, last drawn earlier this frame at pass %d; depth target %#llx: %s the other frame, used earlier at pass %d",
                           (unsigned long long)m->img, m->cleared ? "cleared at the pass start" : "loaded (not cleared)", seen_c ? "also in" : "NOT in", wrote_c,
                           (unsigned long long)m->dimg, seen_d ? "also in" : "NOT in", wrote_d); }
                    for (int q = 0; q < m->ndr; q++) { const probe_draw *d = &m->dr[q];
                        LOGV("        draw %d: vs %s prim %d count %u first %u inst %u, viewport %g %g %g %g, depth range %g..%g, bias %g/%g on %g, clamp %d, cull %d, scissor %s%d %d %d %d, vertex 0: %g %g %g %g %g %g %g %g",
                             q, d->vs, d->prim, d->count, d->first, d->inst, d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->dr[0], d->dr[1], d->bias[0], d->bias[1], d->bias[2], d->clamp, d->cull,
                             d->has_sc ? "" : "(off) ", d->sc[0], d->sc[1], d->sc[2], d->sc[3], d->v0[0], d->v0[1], d->v0[2], d->v0[3], d->v0[4], d->v0[5], d->v0[6], d->v0[7]);
                        LOGV("          vs slot 8 [25] = %g %g %g %g, slot 1 [147] = %g %g %g %g, slot 1 [7] = %g %g %g %g", d->c11_25[0], d->c11_25[1], d->c11_25[2], d->c11_25[3],
                             d->c4_147[0], d->c4_147[1], d->c4_147[2], d->c4_147[3], d->c4_7[0], d->c4_7[1], d->c4_7[2], d->c4_7[3]); }
                    for (int q = 0; q < pf->nw; q++) if (pf->wr[q].at == at) {   /* depth passes (its own depth attachment too), copies and clears since the colour pass before */
                        const probe_write *w = &pf->wr[q]; double dn, dm, dx; char ds[80] = "";
                        if (w->kind == 'D' && depth_stats(pf, q, &dn, &dm, &dx)) { if (w->vf == VK_FORMAT_D16_UNORM) snprintf(ds, sizeof ds, ", depth %u..%u of 65535", g_d16_lo, g_d16_hi); else snprintf(ds, sizeof ds, ", depth %.4f..%.4f mean %.4f", dn, dx, dm); }
                        if (w->kind == 'D' && w->nd) LOGV("          its first draw: vs %s, slot 8 [25] = %g %g %g %g, slot 1 [147] = %g %g %g %g", w->d0.vs, w->d0.c11_25[0], w->d0.c11_25[1], w->d0.c11_25[2], w->d0.c11_25[3], w->d0.c4_147[0], w->d0.c4_147[1], w->d0.c4_147[2], w->d0.c4_147[3]);
                        LOGV("        just before / with it: %s %ux%u %#x%s%s", w->kind == 'D' ? "depth pass on" : w->kind == 'C' ? "copy into" : w->kind == 'K' ? "clear of" : w->kind == 'X' ? "image store to" : "upload to",
                             w->w, w->h, w->fmt, w->kind == 'D' ? (snprintf(ds + strlen(ds), sizeof ds - strlen(ds), ", %u draws", w->draws), ds) : "", "");
                    }
                    if (f == 1) {   /* the buffers both frames' passes read, side by side, where their bytes differ */
                        for (int k = 0; k < x->nbuf; k++) {
                            const probe_buf *ba = &x->buf[k]; const probe_buf *bb = NULL; int kb = -1;
                            for (int q = 0; q < y->nbuf; q++) if (y->buf[q].stage == ba->stage && y->buf[q].reg == ba->reg) { bb = &y->buf[q]; kb = q; break; }
                            const uint8_t *pa = (const uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)(a - g_probe) * PROBE_SLOTS + (size_t)i) * 12 + (size_t)k) * 256;
                            const uint8_t *pb = bb ? (const uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)(b - g_probe) * PROBE_SLOTS + (size_t)j) * 12 + (size_t)kb) * 256 : NULL;
                            size_t nn = bb ? MINV(ba->n, bb->n) : 0;
                            if (bb && ba->n == bb->n && !memcmp(pa, pb, nn)) continue;
                            int first = -1; for (size_t q = 0; q < nn; q += 4) if (memcmp(pa + q, pb + q, 4)) { first = (int)q; break; }
                            char l3[900]; size_t o3 = (size_t)snprintf(l3, sizeof l3, "        %s %s %d (%#llx, %llu bytes probed%s) differs%s", ba->stage ? "fs" : "vs", ba->reg >= 16 ? "storage" : "uniform", ba->reg % 16,
                                (unsigned long long)ba->addr, (unsigned long long)ba->n, (ba->storage & 2) || (bb && (bb->storage & 2)) ? ", inline updates pending" : "", bb ? "" : " (not bound in the good frame)");
                            if (first >= 0) {
                                int st = first / 16 * 16; o3 += (size_t)snprintf(l3 + o3, sizeof l3 - o3, " from byte %d; flash/good floats at %d:", first, st);
                                for (int q = st; q < st + 32 && (size_t)q + 4 <= nn && o3 + 40 < sizeof l3; q += 4) { float fa, fb2; memcpy(&fa, pa + q, 4); memcpy(&fb2, pb + q, 4); o3 += (size_t)snprintf(l3 + o3, sizeof l3 - o3, " %g/%g", fa, fb2); }
                            }
                            LOGV("%s", l3);
                        }
                    }
                    for (int d = 0; d < ndp; d++) {   /* the depth passes it read: what they read */
                        const probe_write *w = dps[d]; char l2[600]; size_t o2 = (size_t)snprintf(l2, sizeof l2, "        that %ux%u %#x depth pass: fs %s, reads", w->w, w->h, w->fmt, w->key[0] ? w->key : "(none: depth only)");
                        for (int k = 0; k < w->nin && o2 + 60 < sizeof l2; k++) {
                            int from = -1; for (int q = 0; q < w->at && q < pf->n; q++) if (pf->m[q].img == w->in[k].img) from = q;
                            const probe_write *ww = NULL; for (int q = (int)(w - pf->wr) - 1; q >= 0; q--) if (pf->wr[q].img == w->in[k].img) { ww = &pf->wr[q]; break; }
                            o2 += (size_t)snprintf(l2 + o2, sizeof l2 - o2, " [%ux%u %#x%s%s]", w->in[k].w, w->in[k].h, w->in[k].fmt, ww ? " written by " : "", ww ? (ww->kind == 'D' ? "a depth pass" : ww->kind == 'C' ? "a copy" : ww->kind == 'K' ? "a clear" : ww->kind == 'X' ? "an image store" : "an upload") : (from >= 0 ? " a colour pass" : " not written this frame"));
                            if (ww && ww->kind == 'D') { double dn, dm, dx; if (depth_stats(pf, (int)(ww - pf->wr), &dn, &dm, &dx)) o2 += (size_t)snprintf(l2 + o2, sizeof l2 - o2, " (depth %.4f..%.4f)", dn, dx); }
                        }
                        if (!w->nin) snprintf(l2 + o2, sizeof l2 - o2, " no textures");
                        LOGV("%s", l2);
                    }
                    ndp = 0;
                }
            }
            i++; j++;
        } else if (L[i + 1][j] >= L[i][j + 1]) i++; else j++;
    }
    #undef PSAME
    LOGV("  %d passes paired, %d differ%s", paired, shown, shown >= 60 ? " (first 60 shown)" : "");
    return shown;
}
/* Presenter diagnostics and explicit test reports share LCS/depth scratch. This diagnostic-only
   lock never participates in recording or queue publication. */
static int probe_report(uint64_t flash, uint64_t before) {
    static pthread_mutex_t report_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&report_mu);
    int shown = probe_report_inner(flash, before);
    pthread_mutex_unlock(&report_mu);
    return shown;
}
int vk_probe_report(uint64_t flash, uint64_t before) { return probe_report(flash, before); }   /* tests */
/* depth target sizes per frame: a change is logged with the frame number, to line up with the dark
   flashes (dynamic resolution re-creates the scene targets; their storage then holds last frame's content on hardware) */
static uint32_t g_fr[16], g_prev[16]; static int g_fr_n, g_prev_n; static uint64_t g_stat_rescg;
/* Overlay-only best effort: sample actual depth-backed MRT draws, never a clear
   that may have opened the pass with the preceding viewport still bound. */
static uint32_t g_overlay_scene_w, g_overlay_scene_h;
static int g_overlay_enabled, g_overlay_scene_dirty;
static void overlay_scene_draw(void) {
    if (!g_overlay_enabled || !g_overlay_scene_dirty) return;
    g_overlay_scene_dirty = 0;
    if (!R.dtex || R.ncolor < 2 || !S.has_vp || S.vp[0][0] != 0 || S.vp[0][1] != 0) return;
    float vw = fabsf(S.vp[0][2]), vh = fabsf(S.vp[0][3]);
    if (!isfinite(vw) || !isfinite(vh)) return;
    uint32_t sw = (uint32_t)(fminf(R.w, vw) + .5f), sh = (uint32_t)(fminf(R.h, vh) + .5f);
    /* DRS aligns axes independently, by up to one 16-texel quantum. Rounding
       each axis by at most 15 gives |9*w - 16*h| <= (9+16)*15, not exact 16:9. */
    const int alignment = 16, tolerance = (9 + 16) * (alignment - 1);
    int64_t error = (int64_t)sw * 9 - (int64_t)sh * 16;
    if (sw > sh && sh && error >= -tolerance && error <= tolerance &&
        (uint64_t)sw * sh > (uint64_t)g_overlay_scene_w * g_overlay_scene_h) {
        g_overlay_scene_w = sw; g_overlay_scene_h = sh;
    }
}
#ifdef TOTK_VK_TESTS
void vk_overlay_test_scene_reset(int enabled) {
    g_overlay_enabled = enabled; g_overlay_scene_w = g_overlay_scene_h = 0;
    g_overlay_scene_dirty = 1;
}
void vk_overlay_test_scene_size(uint32_t *w, uint32_t *h) {
    *w = g_overlay_scene_w; *h = g_overlay_scene_h;
}
#endif
static int u32cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void sizes_str(const uint32_t *v, int n, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < n && o + 16 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%ux%u", i ? " " : "", v[i] >> 16, v[i] & 0xffff);
}
/* NVN barriers between draws on the same render targets (TOTK_VK_OFF=passbarrier: dropped, as before). The game orders
   fragment work and invalidates texture caches between draws that feed each other without changing targets: the
   material shaders store per-tile flags that the lighting quads read (nvn_mtl.m msl_stores_through). Inside one Vulkan
   pass draws have no such ordering (turnip in sysmem mode runs them overlapped), so the barrier ends the pass; the next
   draw reopens it with load. TOTK_VK_SERIAL=1 (diagnostic): end the pass after every draw. */
static uint64_t g_stat_inpass_barriers;
static int pass_barrier_on(void) { static int on = -1; if (on < 0) on = !vk_off("passbarrier"); return on; }
static int serial_draws(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_SERIAL"); on = e && atoi(e); if (on) LOGV("TOTK_VK_SERIAL: every draw is its own pass"); } return on; }
static uint64_t g_merge_deferred, g_merge_retained, g_merge_hazards, g_merge_avoided;
static int pass_merge_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_PASS_MERGE");
        int requested = e && !strcmp(e, "1");
        on = requested && pass_barrier_on() && !store_clear_on();
        if (on) LOGV("[pass-merge] enabled: defer read-only barriers; retain storage, feedback, unknown, indirect and ZCULL dependencies");
        else if (requested) LOGV("[pass-merge] disabled: requires passbarrier enabled and STORE_CLEAR off (begin-time discard assumptions)");
    }
    return on;
}
void vk_pass_merge_counters(uint64_t out[4]) {
    out[0] = g_merge_deferred; out[1] = g_merge_retained; out[2] = g_merge_hazards; out[3] = g_merge_avoided;
}
/* No rw inference: even a nominally read-only storage descriptor is a hazard. This also
   protects readers before a deferred barrier from a later storage writer (WAR). */
static int pass_merge_draw_hazard(vk_shader *const sh[6]) {
    for (int st = 0; st < 2; st++) for (int i = 0; sh[st] && i < sh[st]->nb; i++) {
        const vk_bind *b = &sh[st]->b[i];
        if (!b->kind || b->kind == RB_IMG) return 1;
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
        if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            b->dtype != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) return 1;   /* includes dynamic storage and unknown types */
        if (b->reg < 0 || b->reg >= 128) return 1;
        uint64_t handle = S.tex[st][b->reg]; if (!handle) continue;   /* dummy texture */
        vk_treg t; if (!vk_treg_get((int)(handle & 0xfffff), &t) || !t.t.mem || !t.t.L.size) return 1;
        uint64_t lo = (uint64_t)(uintptr_t)t.t.mem, hi = lo + t.t.L.size; if (hi < lo) return 1;
        for (int a = 0; S.has_rt && a < 9; a++) {
            const tex_t *att = a < 8 ? (a < S.rt.n ? S.rt.color[a] : NULL) : S.rt.depth;
            if (!att) continue;
            uint64_t alo = (uint64_t)(uintptr_t)att->mem, ahi = alo + att->L.size;
            if (att->serial == t.t.serial || !att->L.size || ahi < alo || (lo < ahi && alo < hi)) return 1;
        }
    }
    return 0;
}
/* Texture-only invalidations can move to the existing full barrier at pass end
   only while all intervening reads refer to unchanged images. In particular,
   a loaded, read-only attachment is not an attachment write. Storage, unknown
   descriptors and memory aliases retain the original boundary. */
static int g_feedback_narrow_launch;
static totk_live_cache g_feedback_narrow_live;
static uint64_t g_feedback_deferred, g_feedback_avoided, g_feedback_hazards;
typedef struct { uint32_t read, written; int unknown; } feedback_usage;
static int feedback_overlap(const tex_t *a, const tex_t *b) {
    uint64_t lo=(uintptr_t)a->mem, hi=lo+a->L.size, blo=(uintptr_t)b->mem, bhi=blo+b->L.size;
    return !lo || !blo || !a->L.size || !b->L.size || hi<lo || bhi<blo || (lo<bhi && blo<hi);
}
static feedback_usage feedback_draw_usage(vk_shader *const sh[6]) {
    feedback_usage use = {0};
    if (!S.has_rt) { use.unknown=1; return use; }
    for (unsigned i=0;i<9;i++) {
        const tex_t *a=i<8 ? (i<(unsigned)S.rt.n ? S.rt.color[i] : NULL) : S.rt.depth;
        if (!a) continue;
        for (unsigned j=0;j<i;j++) {
            const tex_t *b=j<8 ? (j<(unsigned)S.rt.n ? S.rt.color[j] : NULL) : S.rt.depth;
            if (b && feedback_overlap(a,b)) use.unknown=1;
        }
        if (i<8 && sh[1] && ((S.chmask.mask>>(4*i))&15) &&
            ((sh[1]->fout_mask|sh[1]->fout_span)==0 || ((sh[1]->fout_mask|sh[1]->fout_span)>>i&1)))
            use.written |= 1u<<i;
    }
    if (S.rt.depth) {
        if (S.ds.dt && S.ds.dw) use.written |= 1u<<8;
        if (S.ds.st && (S.st_wmask[0] || S.st_wmask[1])) use.written |= 1u<<9;
    }
    for (unsigned st=0;st<2;st++)
        if (sh[st] && (sh[st]->has_storage_writes || sh[st]->has_storage_reads)) use.unknown=1;
    for (unsigned st=0;st<2;st++) for (int i=0;sh[st] && i<sh[st]->nb;i++) {
        const vk_bind *b=&sh[st]->b[i];
        if (b->dtype==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (b->kind!=RB_SUPPORT && b->kind!=RB_CBUF && b->kind!=RB_C1 && b->kind!=RB_DRV0 && b->kind!=RB_DRV2)
                use.unknown=1;
            continue;
        }
        if (b->dtype==VK_DESCRIPTOR_TYPE_SAMPLER) continue;
        if ((b->dtype!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
            (b->kind!=RB_TEX && b->kind!=RB_TEXSAMP) || b->count!=1 || b->reg<0 || b->reg>=128) {
            use.unknown=1; continue;
        }
        uint64_t handle=S.tex[st][b->reg]; if (!handle) continue;
        vk_treg t;
        if (!vk_treg_get((int)(handle&0xfffff),&t) || !t.t.mem || !t.t.L.size) { use.unknown=1; continue; }
        for (unsigned a=0;a<9;a++) {
            const tex_t *att=a<8 ? (a<(unsigned)S.rt.n ? S.rt.color[a] : NULL) : S.rt.depth;
            if (!att) continue;
            if (att->serial==t.t.serial && att->mem==t.t.mem && att->L.size==t.t.L.size)
                use.read |= a<8 ? 1u<<a : 3u<<8; /* all levels/aspects: no view/LOD guesses */
            else if (feedback_overlap(att,&t.t)) use.unknown=1;
        }
    }
    return use;
}
static int feedback_narrow_barrier(uint32_t bits) {
    const uint32_t allowed=NVN_BARRIER_INVALIDATE_TEXTURE_BIT|NVN_BARRIER_ORDER_PRIMITIVES_BIT;
    if (!R.feedback_on || R.feedback_unknown || !(bits&NVN_BARRIER_INVALIDATE_TEXTURE_BIT) ||
        (bits&~allowed) || R.native || R.canvas || vk_fg_extract_active || PD.active || PP.active ||
        probe_on() || serial_draws() || store_clear_on() || pass_merge_on()) return 0;
    R.feedback_pending |= bits; R.feedback_armed=1; g_feedback_deferred++;
    return 1;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_feedback_narrow_counts(uint64_t out[3]) {
    out[0]=g_feedback_deferred; out[1]=g_feedback_avoided; out[2]=g_feedback_hazards;
}
#endif
static int pass_merge_barrier(uint32_t bits) {
    const uint32_t allowed = NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT |
        NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT |
        NVN_BARRIER_INVALIDATE_SHADER_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT;
    if (!pass_merge_on()) return 0;
    if (R.merge_storage || (bits & ~allowed) || PD.active || PP.active || probe_on() || serial_draws()) {
        g_merge_retained++; return 0;
    }
    R.merge_pending |= bits; R.merge_armed = 1; g_merge_deferred++;
    return 1;   /* actual barrier remains at the eventual pass end; never issue one inside dynamic rendering */
}
static void end_rendering_inner(vk_pass_end_kind reason, uint32_t detail) {
    if (!R.open && S.has_rt) {   /* clears still waiting for a draw become a pass of their own (load op CLEAR) */
        int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
        if (pend) begin_rendering(0);
    }
    if (!R.open) return;
    if (pass_merge_on()) {
        int restored = (reason == VK_PASS_END_MERGE_HAZARD || reason == VK_PASS_END_QUERY) &&
            !R.merge_armed && R.merge_continued != 0;
        g_merge_avoided += R.merge_continued - (uint32_t)restored;
    }
    g_feedback_avoided += R.feedback_continued - (uint32_t)R.feedback_restore;
    VkCommandBuffer cb = vk_cb();
    if (R.secondary) {
        int parked;
        if (__builtin_expect(vk_gpuprof_on, 0)) {
            vk_gpuprof_end_token profile_end;
            vk_record_pass_flush(); /* final chunk still belongs to the profiler's current open pass */
            vk_gpuprof_end_deferred(reason, detail, &profile_end);
            parked = VK_REC_VALUE(VK_REC_O_ASSEMBLE, vk_record_pass_end_profiled(cb, &profile_end));
        } else {
            parked = VK_REC_VALUE(VK_REC_O_ASSEMBLE, vk_record_pass_end(cb));
        }
        if (parked) cb = vk_record_next_primary();
        descriptor_unbind();
    } else {
        vk_occlusion_end(cb);   /* precise queries remain on the original inline diagnostic path */
        vkCmdEndRendering(cb);
        if (vk_gpuprof_on) vk_gpuprof_end(cb, reason, detail);
    }
    vk_barrier_all(cb);
    /* This full barrier also issues every deferred texture invalidation before
       the next pass's reads. No deferred dependency survives an ownership end. */
    R.feedback_pending=0;
    if (PP.open >= 0) { PP.rec[PP.open].draws = (uint32_t)(g_stat_draws - PP.draws0); pp_end(cb, PP.open); PP.open = -1; }
    pso_life_pass_write(0);
    R.open = 0; g_bound_pipe = VK_NULL_HANDLE;
    if (vk_native_ui_enabled()) for (unsigned i = 0; i < R.ncolor; i++) if (R.ctex[i]) vk_tex_native_written(R.ctex[i], R.native);
    pd_pass_end(cb);
    if (vk_fg_extract_active) fg_extract_pass_end();
    fg_extract_authority_span_end(cb); /* rejected frames must consume/poison pending authority */
    fg_extract_writer_end(cb); /* rejected probes must also close/poison diagnostics */
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_pass_end();
    if (R.dtex) probe_write_note('D', R.dtex, NULL, (uint32_t)(g_stat_draws - R.draws0));
    probe_pass(cb);
    pd_pass(cb);
}
void vk_end_rendering_at(vk_pass_end_kind reason, uint32_t detail) { VK_REC_DO(VK_REC_S_PASS, end_rendering_inner(reason, detail)); }
void vk_end_rendering(void) { vk_end_rendering_at(VK_PASS_END_OTHER, 0); }
/* A pending load-clear is already an NVN write, even before a draw attaches
 * the target. A descriptor read must not observe the preceding image version.
 * Resolve via the same attachment path (views, integer formats, native canvas,
 * MSAA and depth/stencil), rather than clearing an imprecise whole VkImage.
 * Walking-owner only; the guard excludes diagnostic reads during pass setup. */
void vk_draw_texture_read(int tid, const tex_t *texture) {
    static int resolving;
    if (R.open || resolving || !S.has_rt) return;
    int pending=R.clear_d || R.clear_s;
    for (unsigned i=0;i<8;i++) pending|=R.clear_c[i];
    if (!pending || !totk_live_cached(&g_clear_read_live,"TOTK_VK_CLEAR_ON_READ",g_clear_read_launch)) return;
    vk_treg registration;
    if (!texture) {
        if (!vk_treg_get(tid,&registration)) return;
        texture=&registration.t;
    }
    int overlaps=(R.clear_d || R.clear_s) && S.rt.depth && feedback_overlap(texture,S.rt.depth);
    for (unsigned i=0;i<8 && !overlaps;i++)
        overlaps=R.clear_c[i] && S.rt.color[i] && feedback_overlap(texture,S.rt.color[i]);
    if (!overlaps) return;
    resolving=1;
    vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER,NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
    resolving=0;
    g_clear_read_forced++;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_correctness_counts(uint64_t out[2]) {
    out[0]=g_alpha_enabled; out[1]=g_alpha_ignored;
}
uint64_t vk_draw_test_clear_read_count(void) { return g_clear_read_forced; }
#endif
int vk_record_counter_timestamp(VkQueryPool pool, uint32_t query, VkPipelineStageFlags2 stage) {
    if (!R.open || !R.secondary) return 0;
    vk_record_timestamp(pool, query, stage); return 1;
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || a->depth != b->depth || (a->depth && a->depth->serial != b->depth->serial)) return 0;
    if (a->has_dview != b->has_dview || (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview))) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (!a->color[i] != !b->color[i] || (a->color[i] && a->color[i]->serial != b->color[i]->serial)) return 0;
        if (a->has_cview[i] != b->has_cview[i] || (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i]))) return 0;
    }
    return 1;
}
/* Only a proven full clear kills an attachment. Treat all bound texture/image slots as reads, including aliases
   at the same guest address; uncertainty retains the store. This also excludes feedback in the opening draw. */
static uint32_t store_read_mask(uint64_t slots[2][160], uint32_t possible) {
    uint32_t read = 0;
    for (int st = 0; st < 2; st++) for (int i = 0; i < 160 && (possible & ~read); i++) {
        uint64_t handle = slots[st][i]; if (!handle) continue;
        vk_treg t; if (!vk_treg_get((int)(handle & 0xfffff), &t)) return possible;
        for (int k = 0; k < 9; k++) {
            uint32_t bit = k < 8 ? 1u << k : 3u << 8; if (!(possible & bit)) continue;
            const tex_t *att = k < 8 ? S.rt.color[k] : S.rt.depth;
            if (att && (att->serial == t.t.serial || att->mem == t.t.mem)) read |= bit;
        }
    }
    return read;
}
/* Dynamic rendering fixes storeOp at BEGIN, not END (unlike Metal). A future draw with this attachment would need
   its store if an implicit split occurs, so don't elide it even when it might stay in this pass. Unrelated draws
   may be crossed, but no copies/dispatches, calls, conditions, events, updates, submissions or unknown operations.
   CLEAR_LOAD splits at a full clear of the exact same target set: DONT_CARE never discards the clear or later writes.
   Bits 0..7: colors, 8: depth, 9: stencil. Pass dumps read the pre-clear pixels and always retain stores. */
static uint32_t store_clear_mask(void) {
    if (!store_clear_on() || PD.active || !S.has_rt || !vk_exec_draw_next()) return 0;
    uint32_t possible = 0;
    for (int i = 0; i < 8; i++) if (R.ctex[i]) possible |= 1u << i;
    if (R.dfmt != VK_FORMAT_UNDEFINED) possible |= 1u << 8;
    if (R.sfmt != VK_FORMAT_UNDEFINED) possible |= 1u << 9;
    /* Aliased slots have independent load/store operations; don't reason about their write order. */
    for (int i = 0; i < 9; i++) for (int j = 0; j < i; j++) {
        vk_tex *x = i < 8 ? R.ctex[i] : R.dtex, *y = j < 8 ? R.ctex[j] : R.dtex;
        if (x && x == y) return 0;
    }
    /* First scan just finds candidates. Only if one exists, copy bindings and resolve their identities on the
       second scan. Most passes reach another draw on their own attachments and return without registry lookups. */
    uint64_t slots[2][160];
    for (int check_reads = 0; check_reads < 2; check_reads++) {
        if (check_reads) {
            for (int st = 0; st < 2; st++) { memcpy(slots[st], S.tex[st], sizeof S.tex[st]); memcpy(slots[st] + 128, S.img[st], sizeof S.img[st]); }
            possible &= ~store_read_mask(slots, possible); if (!possible) return 0;
        }
        const uint8_t *p = vk_exec_draw_next();
        const p_set_rt *rt = &S.rt; p_scissor sc = S.sc; int has_sc = S.has_sc, disabled = S.render_disabled;
        uint32_t mask = 0, live = possible;
        for (int guard = 0; guard < 128 && (live & ~mask); guard++) {
            op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
            if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) break;
            switch (h.op) {
                case OP_SET_RT: rt = a; break;
                case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
                case OP_RENDER_ENABLE: disabled = !((const p_flag *)a)->value; break;
                case OP_BIND_TEX: case OP_BIND_IMG: {
                    if (!check_reads) break;
                    const p_bind_handles *b = a; int n = h.op == OP_BIND_TEX ? 128 : 32, base = h.op == OP_BIND_TEX ? 0 : 128;
                    if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->first + b->n > n) goto scanned;
                    if (b->stage < 2) memcpy(&slots[b->stage][base + b->first], b->h, (size_t)b->n * 8);
                    break;
                }
                case OP_DRAW: case OP_DRAW_IDX: {
                    uint32_t need = 0;
                    for (int k = 0; k < 9; k++) {
                        uint32_t bit = k < 8 ? 1u << k : 3u << 8; if (!(live & ~mask & bit)) continue;
                        const tex_t *t = k < 8 ? S.rt.color[k] : S.rt.depth; if (!t) continue;
                        for (int j = 0; j < 9; j++) {
                            const tex_t *u = j < 8 ? (j < rt->n ? rt->color[j] : NULL) : rt->depth;
                            if (u && (t->serial == u->serial || t->mem == u->mem)) need |= bit;
                        }
                    }
                    if (check_reads) need |= store_read_mask(slots, live & ~mask & ~need);
                    live &= ~need; break;   /* even disabled draws are conservatively considered reads */
                }
                case OP_CLEAR_COLOR: case OP_CLEAR_DS: {
                    if (disabled || !rt_same(&S.rt, rt)) break;
                    if (has_sc && !(sc.x <= 0 && sc.y <= 0 && (int64_t)sc.x + sc.w >= R.w && (int64_t)sc.y + sc.h >= R.h)) break;
                    if (h.op == OP_CLEAR_COLOR) {
                        const p_clear_color *c = a;
                        if (c->index >= 0 && c->index < S.rt.n && c->index < 8 && (c->mask & 15) == 15) mask |= live & (1u << c->index);
                    } else {
                        const p_clear_ds *c = a; int sm = c->stencil_mask & 255;
                        if (sm && sm != 255) break;   /* clear_ds cannot move a masked stencil clear to loadOp */
                        if (c->depth_mask) mask |= live & (1u << 8);
                        if (sm == 255) mask |= live & (1u << 9);
                    }
                    break;
                }
                case OP_BEGIN: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE:
                case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO:
                case OP_BIND_SAMP: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP:
                case OP_RASTER_DISCARD: case OP_COPY_STRIDE: case OP_BARRIER: break;
                default: goto scanned;
            }
            p += h.size;
        }
scanned:
        if (!mask || check_reads) return mask;
        possible = mask;
    }
    return 0;
}
long vk_frame_chunk_limit(void) {
    static int initialized, explicit_limit, lag_requested;
    static long configured_limit;
    if (!initialized) {
        const char *limit = getenv("TOTK_VK_CHUNK_DRAWS");
        explicit_limit = limit && *limit;
        configured_limit = explicit_limit ? atol(limit) : 0;
        lag_requested = runtime_frame_lag_requested();
        initialized = 1;
    }
    if (explicit_limit) return configured_limit;
    return lag_requested && !nvn_dirty_lag_vma_pressure() ? 600 : 75;
}
/* TOTK_VK_SPLIT_PASS_DRAWS=<n> (default 0 = off): the chunk threshold above is only evaluated when a pass opens, so a
   long pass (the main scene pass, hundreds of draws) reaches the GPU only after its last draw is recorded. With n set,
   the n-th draw since the pass (or its previous part) opened ends the pass; the next pass start submits the recorded
   part through the same chunk submission and reopens the same targets with loadOp LOAD (the clears were consumed by
   the first part). Each split costs one extra store and load of every attachment on a tiler, so it stays opt-in.
   Refused (once per pass) where the open pass cannot be resumed as it was: a store chosen DONT_CARE at its start,
   native/canvas UI passes, framegen extraction, pass dumps/probes. Nothing is split while chunking is disabled
   (TOTK_VK_CHUNK_DRAWS=0) or outside a game submission, since there would be nothing to submit. */
static uint32_t g_split_draws = UINT32_MAX;
static int g_split_submit;   /* the next pass start submits the recorded part (and reopens the split targets) */
static uint64_t g_split_count, g_split_reopen, g_split_refused, g_split_touches, g_split_lrz_refused;
static int g_split_safe_launch = 1;
static totk_live_cache g_split_safe_live;
static int split_lrz_safe(void) { return totk_live_cached(&g_split_safe_live, "TOTK_VK_SPLIT_LRZ_SAFE", g_split_safe_launch); }
/* Walking-owned depth epochs. A cache collision loses only a touch elision, never safety. Track even in the
   control arm so a live enable repairs depth written before it. VkImage, not guest texture/view, is the identity. */
static struct { VkImage image; uint64_t game, seal; } g_split_depth[64];
static uint64_t g_split_game;
static int g_split_cut, g_split_unrepairable;
static int split_depth_touchable(vk_tex *x) {
    VkFormat f = vk_tex_format(x);
    return vk_tex_samples(x) == 1 && (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT ||
        f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT);
}
static void split_depth_begin(VkCommandBuffer cb, VkAttachmentLoadOp load) {
    if (!R.dtex || R.dfmt == VK_FORMAT_UNDEFINED) return;
    VkImage image = vk_tex_image(R.dtex);
    unsigned i = (unsigned)(((uintptr_t)image >> 4) % 64);
    uint64_t game = vk_game_submit_serial(), seal = vk_submit_serial();
    int known = g_split_depth[i].image == image && g_split_depth[i].game == game;
    int crossed = known ? g_split_depth[i].seal != seal : g_split_cut;
    int touchable = split_depth_touchable(R.dtex);
    if (!touchable) g_split_unrepairable = 1;
    if ((R.split_draws || g_split_cut) && split_lrz_safe() && crossed && load == VK_ATTACHMENT_LOAD_OP_LOAD && touchable) {
        /* A transfer depth write invalidates LRZ on Turnip and the Qualcomm driver. Preserve the exact texel
           bits (including D24); no CPU readback, clear, fragment shader, or change to the guest's depth state.
           This is outside rendering, on the NEW primary, before its LOAD and any secondary draw executes. */
        VkBuffer buffer; VkDeviceSize offset;
        (void)vk_transient(4, 4, &buffer, &offset);
        VkBufferImageCopy copy = {.bufferOffset = offset,
            .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, R.dlevel, R.dlayer, 1}, .imageExtent = {1, 1, 1}};
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
        vk_barrier_all(cb);
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        vk_barrier_all(cb);
        vkCmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        vk_barrier_all(cb);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
        g_split_touches++;
    }
    g_split_depth[i].image = image; g_split_depth[i].game = game; g_split_depth[i].seal = seal;
}
static uint32_t split_pass_draws(void) {
    if (g_split_draws == UINT32_MAX) {
        const char *e = getenv("TOTK_VK_SPLIT_PASS_DRAWS"); long n = e && *e ? atol(e) : 0;
        g_split_draws = n <= 0 ? 0 : n > 1000000 ? 1000000 : (uint32_t)n;
        LOGV("split pass draws: %u (TOTK_VK_SPLIT_PASS_DRAWS; 0 = off: long passes reach the GPU only when they end)", g_split_draws);
    }
    return g_split_draws;
}
/* Live (runtime/live_ctl.h): taken when a pass (or a split part) opens, so one part never changes its split point. */
static totk_live_cache g_split_live;
static uint32_t split_pass_live(void) { return (uint32_t)totk_live_cached(&g_split_live, "TOTK_VK_SPLIT_PASS_DRAWS", (int)split_pass_draws()); }
#ifdef TOTK_VK_TESTS
void vk_draw_test_split_counts(uint64_t out[5]) {
    out[0] = g_split_count; out[1] = g_split_reopen; out[2] = g_split_refused;
    out[3] = g_split_touches; out[4] = g_split_lrz_refused;
}
#endif
/* TOTK_VK_FEEDBACK_CUT (see feedback_cut): the launch value, read once in vk_draw_switches_init, and the value in use.
   Live (runtime/live_ctl.h): taken at each descriptor build (record_stage_impl); the freeze memo's key carries the open
   pass's serial only while it is on, so a change never reuses sets built under the other value. */
static int g_feedback_cut_launch;
static _Atomic int g_feedback_cut;
static totk_live_cache g_feedback_cut_live;
static inline int feedback_cut_refresh(void) {   /* at the start of each descriptor build */
    int on = totk_live_cached(&g_feedback_cut_live, "TOTK_VK_FEEDBACK_CUT", g_feedback_cut_launch);
    if (atomic_load_explicit(&g_feedback_cut, memory_order_relaxed) != on) atomic_store_explicit(&g_feedback_cut, on, memory_order_relaxed);
    return on;
}
static uint64_t g_feedback_cuts, g_feedback_uncut;   /* sampled views cut below an attached level; left whole (base attached) */
#ifdef TOTK_VK_TESTS
void vk_draw_test_feedback_counts(uint64_t out[2]) { out[0] = g_feedback_cuts; out[1] = g_feedback_uncut; }
#endif
static unsigned view_level(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
/* Opt-in, next-pass-open coverage proof. A certificate describes actual SPIR-V,
   not a shader key. Geometry is read through ordered CPU/snapshot authority;
   GPU-only producers, unknown layouts and all destination dependencies retain LOAD. */
static int g_load_elide_launch;
static totk_live_cache g_load_elide_live;
static uint64_t g_load_proven, g_load_elided;
static const p_draw *g_load_draw;
static const p_draw_idx *g_load_indexed;
static vk_shader *const *g_load_sh;
static VkPipeline g_load_pipeline;
static VkPipelineLayout g_load_layout;
static uint8_t g_load_vertices[48];
static unsigned g_load_vertex_bytes;
static VkDescriptorBufferInfo g_load_geometry;
static unsigned g_load_stream;
static int g_load_override;
static VkPipeline pipe_get(vk_shader *vs, vk_shader *fs, int prim, VkPipelineLayout layout);
static uint32_t load_coverage_mask(void) {
    vk_shader *vs = g_load_sh ? g_load_sh[0] : NULL, *fs = g_load_sh ? g_load_sh[1] : NULL;
    if (!vs || !fs || !vs->load_position_attr || !fs->load_full_outputs ||
        vs->has_storage_writes || fs->has_storage_writes || S.ds.dt || S.ds.st ||
        S.color.blend_en || S.color.logic_op != NVN_LOGIC_OP_COPY || (S.ms.a2c & 1) ||
        S.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS || S.poly.mode != NVN_POLYGON_MODE_FILL ||
        S.poly.cull != NVN_FACE_NONE || R.native || R.canvas || vk_fg_extract_active ||
        PD.active || PP.active || probe_on() || R.samples != VK_SAMPLE_COUNT_1_BIT ||
        !S.has_vp || S.vp[0][0] != 0 || S.vp[0][1] != 0 ||
        S.vp[0][2] != R.w || S.vp[0][3] != R.h ||
        (S.has_sc && (S.sc.x > 0 || S.sc.y > 0 || (int64_t)S.sc.x + S.sc.w < R.w ||
                     (int64_t)S.sc.y + S.sc.h < R.h))) return 0;
    unsigned attr = vs->load_position_attr - 1;
    if (vs->vin_mask != (1u << attr) || attr >= (unsigned)S.nattr || S.attr[attr].offset) return 0;
    VkFormat format = vk_vertex_fmt(S.attr[attr].fmt);
    if (format != VK_FORMAT_R32G32_SFLOAT && format != VK_FORMAT_R16G16_SFLOAT) return 0;
    unsigned vertex_bytes = format == VK_FORMAT_R32G32_SFLOAT ? 8 : 4;
    unsigned stream = S.attr[attr].stream;
    if (stream >= 16 || S.stream[stream].divisor) return 0;
    int prim = g_load_draw ? g_load_draw->prim : g_load_indexed->prim;
    unsigned count = g_load_draw ? g_load_draw->count : g_load_indexed->count;
    if (prim != 4 || (count != 3 && count != 6)) return 0; /* triangle list only */
    if (g_load_indexed && (g_load_indexed->itype < 0 || g_load_indexed->itype > 2)) return 0;
    float xy[6][2];
    for (unsigned i = 0; i < count; i++) {
        int64_t vertex = g_load_draw ? (int64_t)g_load_draw->first + i : 0;
        if (g_load_indexed) {
            unsigned bytes = 1u << g_load_indexed->itype; uint32_t index = 0;
            uint64_t address = g_load_indexed->ibuf + (uint64_t)i * bytes;
            VkDescriptorBufferInfo frozen;
            if (address < g_load_indexed->ibuf || !vk_ub_replay_read(address, bytes, 16, &frozen)) return 0;
            vk_ub_read(address, &index, bytes); vertex = (int64_t)index + g_load_indexed->base_vertex;
        }
        if (vertex < 0 || (S.stream[stream].stride && (uint64_t)vertex > UINT64_MAX / S.stream[stream].stride)) return 0;
        uint64_t offset = (uint64_t)vertex * S.stream[stream].stride + S.attr[attr].offset;
        const NVNbufferRange *b = &S.vbuf[stream]; VkDescriptorBufferInfo frozen;
        if (offset > b->size || b->size - offset < vertex_bytes ||
            b->address + offset < b->address || !vk_ub_replay_read(b->address + offset, vertex_bytes, 16, &frozen)) return 0;
        uint8_t *raw = g_load_vertices + i * vertex_bytes;
        vk_ub_read(b->address + offset, raw, vertex_bytes);
        if (vertex_bytes == 8) memcpy(xy[i], raw, 8);
        else for (unsigned k = 0; k < 2; k++) {
            uint16_t h; memcpy(&h, raw + 2*k, 2);
            unsigned exponent = h >> 10 & 31, mantissa = h & 1023;
            if (exponent == 31) return 0;
            xy[i][k] = ldexpf((float)(exponent ? mantissa + 1024 : mantissa), exponent ? (int)exponent - 25 : -24);
            if (h & 0x8000) xy[i][k] = -xy[i][k];
        }
        for (unsigned k = 0; k < 2; k++)
            if (!isfinite(xy[i][k]) || fabsf(xy[i][k]) > 16 || xy[i][k] != truncf(xy[i][k])) return 0;
    }
    if (count == 3) {
        double area = ((double)xy[1][0] - xy[0][0]) * ((double)xy[2][1] - xy[0][1]) -
                      ((double)xy[1][1] - xy[0][1]) * ((double)xy[2][0] - xy[0][0]);
        if (!area) return 0;
        for (unsigned corner = 0; corner < 4; corner++) for (unsigned edge = 0; edge < 3; edge++) {
            unsigned j = (edge + 1) % 3;
            double cross = ((double)xy[j][0] - xy[edge][0]) * ((corner & 2 ? 1.0 : -1.0) - xy[edge][1]) -
                           ((double)xy[j][1] - xy[edge][1]) * ((corner & 1 ? 1.0 : -1.0) - xy[edge][0]);
            if (area > 0 ? cross < 0 : cross > 0) return 0;
        }
    } else {
        unsigned corners[2] = {0}, shared;
        for (unsigned i = 0; i < 6; i++) {
            if ((xy[i][0] != -1 && xy[i][0] != 1) || (xy[i][1] != -1 && xy[i][1] != 1)) return 0;
            unsigned bit = 1u << ((xy[i][0] == 1) | ((xy[i][1] == 1) << 1));
            if (corners[i / 3] & bit) return 0;
            corners[i / 3] |= bit;
        }
        shared = corners[0] & corners[1];
        if ((corners[0] | corners[1]) != 15 || (shared != 6 && shared != 9)) return 0;
    }
    uint32_t mask = fs->load_full_outputs & 255u;
    for (unsigned i = 0; i < 8; i++) if ((S.chmask.mask >> (4 * i) & 15) != 15) mask &= ~(1u << i);
    /* Aliases and every bound sampled/storage view are MAY-reads. */
    for (unsigned st = 0; st < 2; st++) for (unsigned slot = 0; slot < 160; slot++) {
        uint64_t handle = slot < 128 ? S.tex[st][slot] : S.img[st][slot - 128];
        if (!handle) continue;
        vk_treg t; if (!vk_treg_get((int)(handle & 0xfffff), &t)) return 0;
        for (unsigned i = 0; i < 8; i++) {
            const tex_t *a = S.rt.color[i];
            if (a && (a->serial == t.t.serial || !a->L.size || !t.t.L.size ||
                ((uintptr_t)a->mem < (uintptr_t)t.t.mem + t.t.L.size &&
                 (uintptr_t)t.t.mem < (uintptr_t)a->mem + a->L.size))) mask &= ~(1u << i);
        }
    }
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 9; j++) {
        const tex_t *a = S.rt.color[i], *b = j < 8 ? S.rt.color[j] : S.rt.depth;
        if (i != j && a && b && (a->serial == b->serial || a->mem == b->mem)) mask &= ~(1u << i);
    }
    if (mask) { g_load_vertex_bytes = vertex_bytes; g_load_stream = stream; }
    return mask;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_load_counts(uint64_t out[2]) { out[0] = g_load_proven; out[1] = g_load_elided; }
#endif
/* Open the pass for the current targets with the pending clears as load operations. 0 when nothing can be attached. */
static int begin_rendering_inner(int draw_tail) {
    if (R.open) return 1;
    fg_extract_refresh(1);   /* configuration/late-state lookup at pass boundaries only */
    uint64_t game = vk_game_submit_serial();
    if (g_split_game != game) { g_split_game = game; g_split_cut = g_split_unrepairable = 0; }
    /* The default follows resolved lag, including the native-pressure cutover.
       An explicit threshold (including disabled=0) remains authoritative. */
    { long lim = vk_frame_chunk_limit();
      g_split_reopen += (uint64_t)g_split_submit;
      if (lim > 0 && (g_chunk_draws >= (uint64_t)lim || g_split_submit)) { g_chunk_draws = 0; vk_submit_chunk(); }
      g_split_submit = 0; }
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = 16384, h = 16384, nc = 0; int have = 0;
    g_pipe_dirty = 1;
    memset(R.cfmt, 0, sizeof R.cfmt); R.dfmt = R.sfmt = VK_FORMAT_UNDEFINED; memset(R.ctex, 0, sizeof R.ctex); R.dtex = NULL; R.samples = VK_SAMPLE_COUNT_1_BIT;
    R.native = R.canvas = 0;
    memset(R.caddr, 0, sizeof R.caddr); R.daddr = NULL; R.cleared = 0;
    if (S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        nc = (uint32_t)i + 1;
        const tex_t *t = S.rt.color[i]; if (!t) continue;
        vk_tex *x = vk_tex_get(t); if (!x) continue;
        const view_t *view = S.rt.has_cview[i] ? &S.rt.cview[i] : NULL;
        int native = !g_native_draw_1x && S.rt.n == 1 && !S.rt.depth &&
            (!view || ((!view->set_levels || !view->base_level) && (!view->set_layers || (!view->min_layer && view->nlayers <= 1)))) &&
            vk_tex_native_active(x);
        if (vk_canvas_on && (vk_tex_native_backed(x) || vk_tex_canvas_window(x))) R.canvas = 1;
        /* A queued full load-clear replaces every pixel, so neither yesterday's
           native contents nor a seed is needed. Reject padded windows, MRT,
           depth, alias formats and non-base subresources: their untouched
           pixels must still be preserved by the ordinary resolve/seed path. */
        uint32_t bw=0,bh=0;
        int full_clear = R.clear_c[i] && S.rt.n==1 && !S.rt.depth &&
            (native || vk_tex_native_backed(x)) &&
            (!view || ((!view->set_levels || (!view->base_level && view->nlevels<=1)) &&
             (!view->set_layers || (!view->min_layer && view->nlayers<=1)) &&
             (!view->set_format || view->format==t->d.format))) &&
            vk_tex_levels(x)==1 && vk_tex_layers(x)==1 && vk_tex_samples(x)==1 &&
            vk_tex_aspect(x)==VK_IMAGE_ASPECT_COLOR_BIT;
        if (full_clear) {
            vk_tex_native_basis(x,&bw,&bh);
            if (bw!=vk_tex_width(x,0) || bh!=vk_tex_height(x,0)) {
                /* Native pixels represent only the active basis. A whole
                   guest clear must also define the allocation's padding. */
                full_clear=0; native=0;
            }
        }
        if (!native) { if (full_clear) vk_tex_native_discard(x); else vk_tex_native_read(x); }
        VkImageView v = native ? (full_clear ? vk_tex_native_clear_view(x,view) : vk_tex_native_view(x,view,1)) : VK_NULL_HANDLE;
        R.native = v != VK_NULL_HANDLE;
        if (!v) { vk_tex_native_read(x); v = vk_tex_view(x, t, view, 1); }
        if (!v) continue;
        unsigned l = view_level(&S.rt.cview[i], S.rt.has_cview[i]);
        R.clevel[i] = (uint8_t)l; R.clayer[i] = S.rt.has_cview[i] && S.rt.cview[i].set_layers ? S.rt.cview[i].min_layer : 0;
        int f = S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : t->d.format;
        uint32_t tw = vk_tex_width(x, l), th = vk_tex_height(x, l);
        { int bw, bh, vbw, vbh; nvn_fmt_bpb(t->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh);   /* a one-texel-per-block view of a BC texture is sized in blocks */
          if (bw > vbw && vbw == 1) tw = (tw + bw - 1) / bw; if (bh > vbh && vbh == 1) th = (th + bh - 1) / bh; }
        w = MINV(w, tw); h = MINV(h, th);
        VkFormat vf; vk_fmt(f, &vf, NULL);
        R.c_serial[i] = t->serial; R.c_nvnfmt[i] = f;
        if (i == 0) { R.c0_level = l; R.c0_layer = S.rt.has_cview[0] && S.rt.cview[0].set_layers ? S.rt.cview[0].min_layer : 0; R.c0_serial = t->serial; R.c0_fmt = f; }
        ca[i].imageView = v; R.cfmt[i] = vf; R.ctex[i] = x; R.dclass[i] = vk_fmt_class(f); vk_tex_mark_gpu_written(x); have = 1;
        R.caddr[i] = t->mem; if (R.clear_c[i]) R.cleared |= 1u << i;
        if (t->d.samples > 1) R.samples = (VkSampleCountFlagBits)t->d.samples;
        if (i == 0) R.c0_cleared = R.clear_c[0] ? 1 : 0;
        if (R.clear_c[i]) { ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[i].clearValue = R.cc[i]; R.clear_c[i] = 0; }
    }
    if (S.has_rt && S.rt.depth) {
        const tex_t *t = S.rt.depth; vk_tex *x = vk_tex_get(t);
        VkImageView v = x ? vk_tex_view(x, t, S.rt.has_dview ? &S.rt.dview : NULL, 1) : VK_NULL_HANDLE;
        if (v) {
            unsigned l = view_level(&S.rt.dview, S.rt.has_dview);
            R.dlevel = (uint8_t)l; R.dlayer = S.rt.has_dview && S.rt.dview.set_layers ? S.rt.dview.min_layer : 0;
            w = MINV(w, vk_tex_width(x, l)); h = MINV(h, vk_tex_height(x, l));
            { uint32_t key = vk_tex_width(x, l) << 16 | vk_tex_height(x, l); int k = 0;   /* the frame's depth target sizes */
              while (k < g_fr_n && g_fr[k] != key) k++;
              if (k == g_fr_n && g_fr_n < 16) g_fr[g_fr_n++] = key; }
            VkImageAspectFlags as = vk_tex_aspect(x); R.dtex = x; R.d_serial = t->serial; vk_tex_mark_gpu_written(x); have = 1;
            R.daddr = t->mem; if (R.clear_d || R.clear_s) R.cleared |= 1u << 8;
            if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { da.imageView = v; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = R.clear_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; R.dfmt = vk_tex_format(x); da.clearValue.depthStencil.depth = R.dfmt == VK_FORMAT_D16_UNORM ? d16_clear(R.cd) : R.cd; }
            if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.imageView = v; sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL; sa.loadOp = R.clear_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE; sa.clearValue.depthStencil.stencil = R.cs; R.sfmt = vk_tex_format(x); }
            if (t->d.samples > 1) R.samples = (VkSampleCountFlagBits)t->d.samples;
            R.clear_d = R.clear_s = 0;
        }
    }
    if (!have) {   /* no target: a pass of rasterisation only, sized by viewport 0 (or the scissor) */
        double x1 = S.has_vp ? ceil(fmax(S.vp[0][0], S.vp[0][0] + S.vp[0][2])) : 0, y1 = S.has_vp ? ceil(fmax(S.vp[0][1], S.vp[0][1] + S.vp[0][3])) : 0;
        if ((x1 < 1 || y1 < 1) && S.has_sc) { x1 = (double)S.sc.x + S.sc.w; y1 = (double)S.sc.y + S.sc.h; }
        w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1; h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
    }
    R.ncolor = nc; R.w = w; R.h = h;
    R.host_w = R.native_w = w; R.host_h = R.native_h = h;
    if (R.native) {
        vk_post_output host;
        if (!vk_tex_native_output(R.ctex[0], &host)) DIE("native UI: missing active attachment");
        R.host_w = host.w; R.host_h = host.h;
        vk_tex_native_basis(R.ctex[0], &R.native_w, &R.native_h);
    }
    /* Injected scene/depth reads are not in the guest lookahead's dependency proof. */
    uint32_t discard = draw_tail && !vk_fg_extract_active ? store_clear_mask() : 0; int discarded = 0;
    for (uint32_t i = 0; i < nc; i++) if (ca[i].imageView && (discard >> i & 1)) { ca[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; g_store_clear_c++; discarded = 1; }
    if (da.imageView && (discard >> 8 & 1)) { da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; g_store_clear_ds++; discarded = 1; }
    if (sa.imageView && (discard >> 9 & 1)) { sa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; g_store_clear_ds++; discarded = 1; }
    g_store_clear_pass += discarded; R.store_discard = discarded; R.split_refused = 0; R.split_draws = split_pass_live();
    if (draw_tail && totk_live_cached(&g_load_elide_live, "TOTK_VK_LOAD_ELIDE", g_load_elide_launch)) {
        uint32_t mask = load_coverage_mask();
        for (unsigned i = 0; i < nc; i++) if (!ca[i].imageView || ca[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) mask &= ~(1u << i);
        if (mask) {
            g_load_proven++;
            /* A skipped asynchronous/failed pipeline must never discard old pixels. */
            g_load_layout = vk_layout_of(g_load_sh[0], g_load_sh[1]);
            g_load_pipeline = pipe_get(g_load_sh[0], g_load_sh[1], g_load_draw ? g_load_draw->prim : g_load_indexed->prim, g_load_layout);
            if (g_load_pipeline && g_load_pipeline != (VkPipeline)(uintptr_t)1) {
                unsigned count = g_load_draw ? g_load_draw->count : g_load_indexed->count;
                VkDeviceSize offset;
                void *vertices = vk_transient(count * g_load_vertex_bytes, 16, &g_load_geometry.buffer, &offset);
                memcpy(vertices, g_load_vertices, count * g_load_vertex_bytes);
                g_load_geometry.offset = offset; g_load_geometry.range = count * g_load_vertex_bytes;
                g_load_override = 1; /* the submitted geometry is exactly the certified immutable bytes */
                for (unsigned i = 0; i < nc; i++) if (mask >> i & 1) ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                g_load_elided++;
            }
        }
    }
    VkCommandBuffer cb = vk_cb();
    split_depth_begin(cb, da.loadOp);
    vk_barrier_all(cb);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {R.host_w, R.host_h}}, .layerCount = 1, .colorAttachmentCount = nc, .pColorAttachments = ca,
        .pDepthAttachment = da.imageView ? &da : NULL, .pStencilAttachment = sa.imageView ? &sa : NULL};
    { int i = pp_begin(cb, 0); PP.open = i;
      if (i >= 0) { pp_rec *r = &PP.rec[i]; r->w = R.host_w; r->h = R.host_h; r->f0 = R.cfmt[0]; r->df = R.dfmt != VK_FORMAT_UNDEFINED ? R.dfmt : R.sfmt; r->ncolor = (uint8_t)nc; PP.draws0 = g_stat_draws;
          if (R.native) r->native_op=(uint8_t)(1+(vk_tex_native_window(R.ctex[0]) ? VK_NUI_WINDOW_PASS : VK_NUI_UI_PASS)); } }
    if (R.native) native_count[vk_tex_native_window(R.ctex[0]) ? VK_NUI_WINDOW_PASS : VK_NUI_UI_PASS]++;
    R.secondary = vk_record_enabled() && !PD.active && !PP.active && !probe_on() &&
        !(vk_fg_extract_active && fg_extract_inline());
    if (vk_gpuprof_on) {
        vk_gpuprof_secondary(R.secondary); /* stats stay inside secondaries; no inheritedQueries requirement */
        VkFormat fmt[10]; const tex_t *tex[10]; uint32_t widths[10], heights[10];
        for (int j = 0; j < 10; j++) {
            vk_tex *x = j < 8 ? R.ctex[j] : R.dtex;
            unsigned level = j < 8 ? R.clevel[j] : R.dlevel;
            fmt[j] = j < 8 ? R.cfmt[j] : j == 8 ? R.dfmt : R.sfmt;
            tex[j] = S.has_rt && x ? (j < 8 ? S.rt.color[j] : S.rt.depth) : NULL;
            widths[j] = x ? vk_tex_width(x, level) : 0; heights[j] = x ? vk_tex_height(x, level) : 0;
            if (j < 8 && tex[j]) {
                int bw, bh, vbw, vbh;
                int f = S.rt.has_cview[j] && S.rt.cview[j].set_format ? S.rt.cview[j].format : tex[j]->d.format;
                nvn_fmt_bpb(tex[j]->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh);
                if (bw > vbw && vbw == 1) widths[j] = (widths[j] + bw - 1) / bw;
                if (bh > vbh && vbh == 1) heights[j] = (heights[j] + bh - 1) / bh;
            }
        }
        vk_gpuprof_begin(cb, &ri, fmt, tex, widths, heights, R.samples);
    }
    PD.draws0 = g_stat_draws;
    if (R.secondary) { ri.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT; vk_record_pass_begin(nc, R.cfmt, R.dfmt, R.sfmt, R.samples); }
    R.merge_pending = R.merge_armed = R.merge_storage = R.merge_continued = 0;
    R.feedback_on=totk_live_cached(&g_feedback_narrow_live,"TOTK_VK_FEEDBACK_NARROW",g_feedback_narrow_launch);
    R.feedback_pending=R.feedback_sampled=R.feedback_continued=0;
    R.feedback_unknown=R.feedback_armed=R.feedback_restore=0;
    R.feedback_written=R.cleared&255u;
    if (da.imageView && da.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) R.feedback_written|=1u<<8;
    if (sa.imageView && sa.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) R.feedback_written|=1u<<9;
    if (pso_life_active()) {
        memset(PL.color_draws, 0, sizeof PL.color_draws); PL.depth_draws = PL.stencil_draws = 0;
        for (unsigned i = 0; i < 8; i++) PL.color_view[i] = i < nc ? ca[i].imageView : VK_NULL_HANDLE;
        PL.depth_view = da.imageView; PL.stencil_view = sa.imageView; PL.color_clear = R.cleared & 255u;
        PL.depth_clear = (da.imageView && da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
            (sa.imageView && sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }
    vkCmdBeginRendering(cb, &ri);
    totk_ft_count(FT_COUNT_PASSES, 1);
    R.open = 1; g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; g_stat_passes++; R.draws0 = g_stat_draws; pass_inputs_reset(); g_pops.disabled_at_start = S.render_disabled;
    pso_life_pass_write(1);
    g_overlay_scene_dirty = 1;
    if (vk_fg_extract_active) fg_extract_pass_begin((R.cleared & 255u) |
        (da.imageView && da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1u << 8 : 0) |
        (sa.imageView && sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1u << 9 : 0));
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_pass_begin();
    return 1;
}
static int begin_rendering(int draw_tail) { return VK_REC_VALUE(VK_REC_S_PASS, begin_rendering_inner(draw_tail)); }

/* Walking-owned, current-real-frame image versions. Material textures are
   outside the scene root, not "missed contexts". Only accepted camera/depth
   roots and their actual sampled descendants participate in provenance. */
enum { FGX_WATCHES = 256 };
typedef struct {
    tex_t depth_desc;
    fg_dep_key depth_key;
    float camera[84];
    float admitted_viewport[4];
    int admitted_scissor[4];
    uint64_t signature;
    unsigned valid, group;
    struct {
        const char *reason, *vs, *fs;
        unsigned admission_pass, revoke_pass, writer, depth_func, camera_declared;
        uint64_t version, ubo_size;
        const char *camera_check;
        const char *unproven_vs, *unproven_fs, *unproven_check;
        uint64_t unproven_draws, unproven_depth_draws;
        unsigned unproven_depth_func, unproven_camera_declared;
        float viewport[4], range[2];
        int scissor[4]; unsigned has_viewport, has_scissor;
    } diag;
} fg_extract_scene;
typedef struct {
    fg_dep_key key;
    uint64_t address, size;
} fg_extract_watch;
typedef struct {
    vk_record_draw draw;
    VkDescriptorSet sets[2];
    unsigned width, height, final, clear;
    VkClearAttachment attachment;
    VkClearRect rect;
    VkFormat format;
    VkImage image; /* Actual attachment, not its guest-sized logical identity. */
    unsigned native;
} fg_extract_replay_draw;
int vk_fg_extract_active;
static struct {
    fg_dep_frame deps;
    fg_pass_frame passes;
    fg_pass_guard guard;
    fg_pass_probe probe;
    unsigned requested, snapshot_ok, probe_started;
    struct {
        unsigned probes, backoff, transfers, nominations, attempts, snapshots, roots;
        unsigned dummy, missing, bounds, storage, image_writes, writes;
        uint64_t cpu_ns[2], cpu_max[2];
        unsigned cpu_frames[2], physical, groups, contexts, missed;
        uint64_t binding_reflected[3], binding_view[3], binding_old_view[3], binding_calls[3];
        uint64_t binding_cpu_ns, binding_cpu_max, binding_top_frame, binding_top_signature;
        unsigned binding_overflows[3], binding_top_work, binding_top_width, binding_top_height;
        int binding_top_group;
        vk_canvas_image canvas;
        fg_dep_value value;
        unsigned root_count, result;
    } diag_window;
    unsigned binding_class, binding_group_work[FG_PASS_MAX_GROUPS + 1];
    struct {
        uint64_t draws, accepted, scissor_pixels, viewport_pixels;
        double max_scissor_fraction;
    } plane_coverage;
    fg_extract_scene scene[FG_DEP_MAX_CANDIDATES];
    fg_extract_watch watch[FGX_WATCHES];
    vk_canvas_image watch_image[FGX_WATCHES], scene_depth_image[FG_DEP_MAX_CANDIDATES];
    /* Existing dependency tags carry diagnostic cause bits through the exact
       sampled versions. They never participate in eligibility or taint tests. */
    struct {
        fg_dep_key key;
        vk_canvas_image image;
        fg_dep_value before;
        const char *kind;
        unsigned writer, pass;
    } unknown[64];
    unsigned nunknown, unknown_overflow;
    unsigned nscene, nwatch, operation, selected, captured, invalid, ui_seen;
    unsigned authority_ready, authority_gpu_ready, authority_root; /* CPU root / GPU span state */
    fg_dep_key authority_key;
    unsigned refusal;
    unsigned transfer, eligible, diag, nreplay, draw_kind;
    fg_extract_replay_draw replay_draw[1024], *record;
    vk_tex *ui_depth;
    unsigned ui_depth_level, ui_depth_layer;
    fg_dep_key canvas_key, ui_depth_key;
    uint64_t ui_depth_address, ui_depth_size;
    unsigned ui_width, ui_height;
    VkFormat ui_format;
    uint64_t frame, signature, stable_frames;
    uint64_t cpu_start;
    const char *last_snapshot_check;
    unsigned last_snapshot_result, last_snapshot_taint;
    struct {
        vk_tex *depth;
        unsigned open, level, layer, experimental;
        unsigned x, y, width, height;
    } authority_span;
    struct {
        vk_tex *depth;
        const char *vs, *fs, *check;
        unsigned open, level, layer, classification;
        uint64_t draw0;
    } writer;
    const char *reason, *last_reason;
    struct {
        fg_dep_key key[9];
        fg_dep_value old[9];
        fg_dep_sources reads, context;
        uint64_t span0, depth_draws, published_draws;
        unsigned outputs, context_outputs, depth_write, root_committed, camera_context, authority_context;
        unsigned unproven_depth;
        unsigned uncertified_context;
        const char *unproven_vs, *unproven_fs, *unproven_check;
        uint64_t unproven_draws, unproven_depth_draws;
        unsigned unproven_depth_func, unproven_camera_declared;
        uint64_t camera_addr;
        uint64_t camera_size, camera_aux_addr;
        uint64_t camera_fragment_addr;
        unsigned plane_context;
        uint64_t scissor_pixels, viewport_pixels;
        const char *vs, *fs; unsigned camera_declared;
        const char *camera_check;
        int group, root;
    } pass;
#ifdef TOTK_VK_TESTS
    unsigned test_authority_inline;
#endif
} FGX;
static int fg_extract_inline(void) {
#ifdef TOTK_VK_TESTS
    if (FGX.test_authority_inline) return 1;
#endif
    return FGX.captured && !FGX.invalid && vk_fg_canvas_view();
}

static fg_dep_key fg_extract_image_key(vk_tex *x, unsigned mip, unsigned layer, unsigned aspect) {
    return (fg_dep_key){.backing = x ? vk_tex_canvas_identity(x,mip,layer).incarnation : 0,
        .mip = mip, .layer = layer, .aspect = aspect};
}
static int fg_extract_same_key(fg_dep_key a, fg_dep_key b) {
    return a.backing && fg_dep_key_equal(a, b);
}
static int fg_extract_canvas(vk_tex *x, unsigned mip, unsigned layer) {
    return fg_extract_same_key(FGX.canvas_key,fg_extract_image_key(x,mip,layer,VK_IMAGE_ASPECT_COLOR_BIT));
}
/* Frozen dynamic state is already in host coordinates. Both UI and terminal
   replay must use the attachment extent that produced that state. */
static int fg_extract_replay_target(vk_post_output *out) {
    if (!R.open || R.ncolor != 1 || !R.ctex[0] || !R.host_w || !R.host_h) return 0;
    VkImage image = R.native ? vk_tex_native_image(R.ctex[0]) : vk_tex_image(R.ctex[0]);
    if (!image) return 0;
    *out = (vk_post_output){image,VK_NULL_HANDLE,R.host_w,R.host_h,R.cfmt[0]};
    return 1;
}
#ifdef TOTK_VK_TESTS
int vk_fg_test_replay_target(vk_post_output *out) {
    return out && fg_extract_replay_target(out);
}
#endif
static int fg_extract_ui_depth(vk_tex *x, unsigned mip, unsigned layer) {
    return !x ? !FGX.ui_depth_key.backing :
        fg_extract_same_key(FGX.ui_depth_key,fg_extract_image_key(x,mip,layer,vk_tex_aspect(x)));
}
static uint64_t fg_extract_cpu_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t) ? 0 : (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
static int fg_extract_requested_config(void) {
    static int requested=-1;
    if (requested<0) {
        const char *e=getenv("TOTK_FRAMEGEN");
        requested=e && (!strcmp(e,"1") || !strcmp(e,"interp") || !strcmp(e,"hybrid") ||
            !strcmp(e,"locked") || !strcmp(e,"adaptive"));
    }
    return requested;
}
static void fg_extract_refresh(int begin_pass) {
    static int requested = -1;
    if (requested < 0) {
        requested=fg_extract_requested_config(); FGX.requested=requested;
        const char *e=getenv("TOTK_FRAMEGEN_DIAG"); FGX.diag=e && !strcmp(e,"1");
    }
    /* No resource work, clocks, registry traversal or callback when unset. */
    int capture=requested && vk_fg_capturing();
    static int hybrid_capture=-1;
    if (requested && runtime_fps_hybrid_selected() && hybrid_capture!=capture) {
        vk_fg_history_cut(); FGX.probe=(fg_pass_probe){0};
        hybrid_capture=capture;
    }
    vk_fg_extract_active=capture && !FGX.invalid && fg_pass_probe_due(&FGX.probe);
    if (begin_pass && requested && FGX.diag && !FGX.cpu_start) FGX.cpu_start=fg_extract_cpu_ns();
}
static void fg_extract_reject(unsigned result, const char *reason) {
    if (!FGX.invalid) {
        FGX.reason = reason; FGX.refusal = result;
        if (!FGX.last_reason || strcmp(reason,FGX.last_reason)) {
            LOGV("[framegen-extract] real %llu %s",(unsigned long long)FGX.frame,reason);
            FGX.last_reason=reason;
        }
    }
    FGX.invalid = 1;
    vk_fg_extract_active = 0; /* failed proof must not keep scanning/serializing the rest of this probe */
    vk_fg_skip();
}
static int fg_extract_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a && b && as && bs && (a <= b ? b - a < as : a - b < bs);
}
static void fg_extract_revoke(fg_dep_key key, const char *reason, unsigned writer) {
    for (unsigned i = 0; i < FGX.nscene; i++) if (fg_extract_same_key(key, FGX.scene[i].depth_key)) {
        if (FGX.captured && FGX.selected == i) continue; /* immutable depth copy already retained */
        fg_extract_scene *c=&FGX.scene[i];
        if (FGX.diag && c->valid) {
            c->diag.reason=reason; c->diag.writer=writer;
            c->diag.revoke_pass=FGX.passes.physical_passes;
            c->diag.version=fg_dep_read(&FGX.deps,key).version;
            c->diag.vs=FGX.pass.vs; c->diag.fs=FGX.pass.fs;
            c->diag.depth_func=S.ds.df; c->diag.ubo_size=S.ubo[0][1].size;
            c->diag.camera_declared=FGX.pass.camera_declared;
            c->diag.camera_check=FGX.pass.camera_check;
            c->diag.unproven_vs=FGX.pass.unproven_vs;
            c->diag.unproven_fs=FGX.pass.unproven_fs;
            c->diag.unproven_check=FGX.pass.unproven_check;
            c->diag.unproven_draws=FGX.pass.unproven_draws;
            c->diag.unproven_depth_draws=FGX.pass.unproven_depth_draws;
            c->diag.unproven_depth_func=FGX.pass.unproven_depth_func;
            c->diag.unproven_camera_declared=FGX.pass.unproven_camera_declared;
            memcpy(c->diag.viewport,S.vp[0],sizeof c->diag.viewport);
            memcpy(c->diag.range,S.dr,sizeof c->diag.range);
            c->diag.has_viewport=S.has_vp; c->diag.has_scissor=S.has_sc;
            c->diag.scissor[0]=S.sc.x; c->diag.scissor[1]=S.sc.y;
            c->diag.scissor[2]=S.sc.w; c->diag.scissor[3]=S.sc.h;
        }
        FGX.scene[i].valid = 0;
        fg_dep_candidate_invalidate(&FGX.deps, UINT64_C(1) << i);
    }
}
static void fg_extract_invalidate(fg_dep_key key, unsigned writer,
                                  const vk_canvas_image *image, const char *kind) {
    fg_dep_value before = FGX.diag ? fg_dep_read(&FGX.deps,key) : (fg_dep_value){0};
    fg_dep_invalidate(&FGX.deps,key,writer);
    if (!FGX.diag) return;
    unsigned i=FGX.nunknown;
    if (i==64) {
        FGX.unknown_overflow++;
        LOGV("[framegen-unknown] real %llu overflow image=%#llx incarnation=%llu size=%ux%u format=%u mip=%u layer=%u aspect=%#x cause=%s writer=%#x at-pass=%u previous-known=%u previous-version=%llu previous-writer=%#x; causal list incomplete",
            (unsigned long long)FGX.frame,(unsigned long long)(image ? image->image : 0),(unsigned long long)key.backing,
            image ? image->width : 0,image ? image->height : 0,image ? image->format : 0,key.mip,key.layer,key.aspect,
            kind,writer,FGX.passes.physical_passes,before.known,(unsigned long long)before.version,before.writer);
        return;
    }
    FGX.nunknown++;
    FGX.unknown[i].key=key;
    FGX.unknown[i].image=image ? *image : (vk_canvas_image){0};
    FGX.unknown[i].before=before;
    FGX.unknown[i].kind=kind;
    FGX.unknown[i].writer=writer;
    FGX.unknown[i].pass=FGX.passes.physical_passes;
    fg_dep_resource *r=fg_dep_find(&FGX.deps,key,0);
    if (r) r->value.tags=UINT64_C(1)<<i;
}

static int fg_extract_camera_equal(const float a[84], const float b[84]);
static uint64_t fg_extract_camera_hash(const float camera[84]) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for (unsigned i=0;i<84;i++) if (i<58 || i==62 || i==70 || i==72 || i==73) {
        uint32_t bits; memcpy(&bits,camera+i,sizeof bits);
        hash=(hash^bits)*UINT64_C(1099511628211);
    }
    return hash;
}
static void fg_extract_refusal_resources(vk_tex *canvas, unsigned level, unsigned layer,
                                         fg_dep_value value) {
    if (!FGX.diag) return;
    vk_canvas_image image=vk_tex_canvas_identity(canvas,level,layer);
    LOGV("[framegen-ancestry] real %llu canvas=%#llx incarnation=%llu size=%ux%u format=%u version=%llu writer=%#x cause-mask=%#llx causes=%u overflow=%u deps-failed=%u pass-flags=%#x",
        (unsigned long long)FGX.frame,(unsigned long long)image.image,(unsigned long long)image.incarnation,
        image.width,image.height,image.format,(unsigned long long)value.version,value.writer,
        (unsigned long long)value.tags,FGX.nunknown,FGX.unknown_overflow,FGX.deps.failed,FGX.passes.failed);
    for (unsigned i=0;i<FGX.nunknown;i++) if (value.tags & (UINT64_C(1)<<i)) {
        const vk_canvas_image *x=&FGX.unknown[i].image;
        const fg_dep_key *key=&FGX.unknown[i].key;
        const fg_dep_value *before=&FGX.unknown[i].before;
        LOGV("[framegen-unknown] real %llu cause-bit=%u image=%#llx incarnation=%llu size=%ux%u format=%u storage=%#llx bytes=%llu mip=%u layer=%u aspect=%#x cause=%s writer=%#x at-pass=%u previous-known=%u previous-version=%llu previous-writer=%#x previous-kind=%s previous-origins=%#llx previous-taint=%#x",
            (unsigned long long)FGX.frame,i,(unsigned long long)x->image,(unsigned long long)key->backing,
            x->width,x->height,x->format,(unsigned long long)x->storage,(unsigned long long)x->size,
            key->mip,key->layer,key->aspect,FGX.unknown[i].kind,FGX.unknown[i].writer,FGX.unknown[i].pass,
            before->known,(unsigned long long)before->version,before->writer,
            !before->known ? "untracked" : before->writer & FG_PASS_WRITER_OPERATION ? "operation" : "raster-pass",
            (unsigned long long)before->origins,before->tainted);
    }
    if (value.tainted && !value.tags)
        LOGV("[framegen-unknown] real %llu untagged-taint=%#x (%s)",
            (unsigned long long)FGX.frame,value.tainted,value.tainted & FG_DEP_TAINT_MAPPING ?
            "unsupported scene/canvas viewport or copy rectangle" : "merge/epoch/identity or diagnostic overflow, not an identified texture");
    for (unsigned i=0;i<FGX.nscene;i++) {
        const fg_extract_scene *c=&FGX.scene[i];
        const fg_pass_key *p=&FGX.passes.group[c->group].key;
        fg_dep_value depth=fg_dep_read(&FGX.deps,c->depth_key);
        LOGV("[framegen-root] real %llu bit=%u valid=%u in-origins=%u eligible=%u unresolved=%u revoked=%u depth-image=%#llx depth-incarnation=%llu mip=%u layer=%u size=%ux%u format=%u group=%u signature=%#llx depth-version=%llu depth-writer=%#x depth-writer-kind=%s depth-taint=%#x",
            (unsigned long long)FGX.frame,i,c->valid,(unsigned)(value.origins>>i&1),
            (unsigned)(value.eligible>>i&1),(unsigned)(value.unresolved>>i&1),
            (unsigned)(FGX.deps.revoked_candidates>>i&1),(unsigned long long)FGX.scene_depth_image[i].image,(unsigned long long)c->depth_key.backing,
            c->depth_key.mip,c->depth_key.layer,p->depth.width,p->depth.height,p->depth.vk_format,c->group,
            (unsigned long long)c->signature,(unsigned long long)depth.version,depth.writer,
            !depth.known ? "untracked" : depth.writer & FG_PASS_WRITER_OPERATION ? "operation" : "raster-pass",depth.tainted);
        int live=-1;
        for (unsigned j=0;j<FGX.nscene;j++) if (FGX.scene[j].valid &&
            fg_extract_same_key(c->depth_key,FGX.scene[j].depth_key)) { live=(int)j; break; }
        LOGV("[framegen-root-camera] real %llu bit=%u camera=%#llx pose=%.9g,%.9g,%.9g near/far=%.9g/%.9g aspect/fov=%.9g/%.9g projection=%.9g,%.9g,%.9g,%.9g camera-extent=%.0fx%.0f live-same-depth=%d exact-camera-match=%d admitted-pass=%u first-revoke=%s writer=%#x at-pass=%u depth-version-at-revoke=%llu depth-func-at-revoke=%u last-context-vs=%.80s last-context-fs=%.80s last-vs-camera-declared=%u vs-ubo1-bytes-at-revoke=%llu",
            (unsigned long long)FGX.frame,i,(unsigned long long)fg_extract_camera_hash(c->camera),
            (double)c->camera[47],(double)c->camera[51],(double)c->camera[55],
            (double)c->camera[56],(double)c->camera[57],(double)c->camera[62],(double)c->camera[70],
            (double)c->camera[28],(double)c->camera[33],(double)c->camera[30],(double)c->camera[34],
            (double)c->camera[72],(double)c->camera[73],live,
            live>=0 ? fg_extract_camera_equal(c->camera,FGX.scene[live].camera) : -1,
            c->diag.admission_pass,c->diag.reason ? c->diag.reason : "none",c->diag.writer,c->diag.revoke_pass,
            (unsigned long long)c->diag.version,c->diag.depth_func,c->diag.vs ? c->diag.vs : "-",
            c->diag.fs ? c->diag.fs : "-",c->diag.camera_declared,(unsigned long long)c->diag.ubo_size);
        if (!c->valid) LOGV("[framegen-root-viewport] real %llu bit=%u camera-check=%s has-viewport=%u viewport=%.9g,%.9g,%.9g,%.9g depth-range=%.9g,%.9g has-scissor=%u scissor=%d,%d,%d,%d (state-at-first-revoke)",
            (unsigned long long)FGX.frame,i,c->diag.camera_check ? c->diag.camera_check : "none",
            c->diag.has_viewport,(double)c->diag.viewport[0],(double)c->diag.viewport[1],
            (double)c->diag.viewport[2],(double)c->diag.viewport[3],
            (double)c->diag.range[0],(double)c->diag.range[1],c->diag.has_scissor,
            c->diag.scissor[0],c->diag.scissor[1],c->diag.scissor[2],c->diag.scissor[3]);
        if (c->diag.reason && !strcmp(c->diag.reason,"raster-depth-without-accepted-camera"))
            LOGV("[framegen-root-unproven] real %llu bit=%u span-draws=%llu span-depth-draws=%llu vs=%.80s fs=%.80s camera-check=%s camera-declared=%u depth-func=%u camera-context=0 (first depth-writing span without accepted camera)",
                (unsigned long long)FGX.frame,i,
                (unsigned long long)c->diag.unproven_draws,
                (unsigned long long)c->diag.unproven_depth_draws,
                c->diag.unproven_vs ? c->diag.unproven_vs : "-",
                c->diag.unproven_fs ? c->diag.unproven_fs : "-",
                c->diag.unproven_check ? c->diag.unproven_check : "none",
                c->diag.unproven_camera_declared,c->diag.unproven_depth_func);
    }
}
/* Image incarnation/subresource is the GPU alias authority. Independent images
   may overlap guest storage (different format/extent); their GPU content is
   deliberately independent in vk_res. Guest/buffer writes still watch ranges. */
static void fg_extract_publish(fg_dep_key key, const tex_t *t, const fg_dep_sources *sources,
                                unsigned writer, fg_dep_mask seed) {
    if (!key.backing || !t) return;
    uint64_t address = (uint64_t)(uintptr_t)t->mem;
    fg_dep_value value = fg_dep_write(&FGX.deps, key, sources, writer, seed, 0, 0, 0);
    if (!value.origins) return;
    unsigned i = 0;
    while (i < FGX.nwatch && !fg_extract_same_key(FGX.watch[i].key, key)) i++;
    if (i == FGX.nwatch) {
        if (i == FGX_WATCHES) { fg_dep_fail(&FGX.deps); return; }
        FGX.nwatch++;
    }
    FGX.watch[i] = (fg_extract_watch){key, address, t->L.size};
    if (FGX.diag) FGX.watch_image[i]=vk_tex_canvas_identity(vk_tex_get(t),key.mip,key.layer);
}
void vk_fg_extract_memory_write(uint64_t address, uint64_t size) {
    if (!vk_fg_extract_active || !address) return;
    if (!size) size = UINT64_MAX;
    if (FGX.captured && fg_extract_overlap(address,size,FGX.ui_depth_address,FGX.ui_depth_size))
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"CPU/storage write to captured UI depth");
    if (R.open && ((FGX.pass.camera_addr &&
        fg_extract_overlap(address,size,FGX.pass.camera_addr,FGX.pass.camera_size)) ||
        (FGX.pass.camera_aux_addr &&
        fg_extract_overlap(address,size,FGX.pass.camera_aux_addr,sizeof(float))) ||
        (FGX.pass.camera_fragment_addr &&
        fg_extract_overlap(address,size,FGX.pass.camera_fragment_addr,84*sizeof(float))))) {
        fg_pass_guard_invalidate(&FGX.guard);
    }
    for (unsigned i = 0; i < FGX.nwatch; i++) {
        const fg_extract_watch *w = &FGX.watch[i];
        if (!fg_extract_overlap(address, size, w->address, w->size)) continue;
        FGX.diag_window.writes++;
        if (FGX.captured && fg_extract_same_key(w->key,FGX.canvas_key))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"CPU/storage write to captured canvas");
        fg_extract_invalidate(w->key,FG_PASS_WRITER_OPERATION | ++FGX.operation,&FGX.watch_image[i],"CPU/buffer-storage-write");
        fg_extract_revoke(w->key,"CPU/buffer-storage-write",FG_PASS_WRITER_OPERATION|FGX.operation);
    }
}
static void fg_extract_span(void) {
    uint64_t draws = g_stat_draws - FGX.pass.span0;
    if (draws) {
        if (FGX.diag && FGX.pass.plane_context && FGX.pass.depth_write) {
            FGX.plane_coverage.draws+=draws;
            FGX.plane_coverage.accepted+=FGX.pass.camera_context ? draws : 0;
            FGX.plane_coverage.scissor_pixels+=draws*FGX.pass.scissor_pixels;
            FGX.plane_coverage.viewport_pixels+=draws*FGX.pass.viewport_pixels;
            double fraction=FGX.pass.viewport_pixels ?
                (double)FGX.pass.scissor_pixels/FGX.pass.viewport_pixels : 0;
            if (fraction>FGX.plane_coverage.max_scissor_fraction)
                FGX.plane_coverage.max_scissor_fraction=fraction;
        }
        FGX.pass.outputs |= FGX.pass.context_outputs;
        if (FGX.pass.depth_write) {
            FGX.pass.depth_draws += draws;
            if (!FGX.pass.camera_context) {
                if (!FGX.pass.unproven_depth) {
                    FGX.pass.unproven_vs=FGX.pass.vs;
                    FGX.pass.unproven_fs=FGX.pass.fs;
                    FGX.pass.unproven_check=FGX.pass.camera_check;
                    FGX.pass.unproven_draws=draws;
                    FGX.pass.unproven_depth_draws=FGX.pass.depth_draws;
                    FGX.pass.unproven_depth_func=S.ds.df;
                    FGX.pass.unproven_camera_declared=FGX.pass.camera_declared;
                }
                FGX.pass.unproven_depth=1;
            }
            if (FGX.pass.camera_context || FGX.pass.authority_context) FGX.pass.root_committed = 1;
        }
        for (unsigned i = 0; i < FGX.pass.context.count; i++)
            fg_dep_sources_add(&FGX.deps,&FGX.pass.reads,FGX.pass.context.source[i]);
        FGX.pass.reads.value.tainted |= FGX.pass.context.value.tainted;
    }
    FGX.pass.span0 = g_stat_draws;
}
static void fg_extract_state_change(int inputs) {
    if (!vk_fg_extract_active || !R.open) return;
    fg_extract_span();
    if (inputs) {
        fg_pass_guard_invalidate(&FGX.guard);
    }
}
static void fg_extract_pass_begin(uint32_t cleared) {
    FGX.probe_started = 1;
    if (!FGX.deps.epoch) { fg_dep_reset(&FGX.deps); fg_pass_reset(&FGX.passes); }
    memset(FGX.pass.key,0,sizeof FGX.pass.key);
    FGX.pass.depth_draws = FGX.pass.camera_addr = 0;
    FGX.pass.unproven_depth=0;
    FGX.pass.camera_size=FGX.pass.camera_aux_addr=0;
    FGX.pass.camera_fragment_addr=0;
    FGX.pass.plane_context=0;
    FGX.pass.outputs = FGX.pass.context_outputs = FGX.pass.root_committed = FGX.pass.camera_context = 0;
    FGX.pass.authority_context=0;
    FGX.pass.uncertified_context=0;
    fg_dep_sources_init(&FGX.pass.reads); fg_dep_sources_init(&FGX.pass.context);
    if (FGX.diag) { FGX.pass.vs=FGX.pass.fs=NULL; FGX.pass.camera_declared=0; }
    FGX.pass.camera_check="no-camera-context";
    FGX.pass.root = -1;
    FGX.pass.span0 = FGX.pass.published_draws = g_stat_draws;
    FGX.pass.depth_write = R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
    fg_pass_key key = {.width = R.w, .height = R.h};
    for (unsigned i = 0; i < 9; i++) {
        vk_tex *x = i < 8 ? R.ctex[i] : R.dtex;
        if (!x) continue;
        unsigned mip = i < 8 ? R.clevel[i] : R.dlevel, layer = i < 8 ? R.clayer[i] : R.dlayer;
        unsigned aspect = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : vk_tex_aspect(x);
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        fg_dep_key k = fg_extract_image_key(x, mip, layer, aspect);
        FGX.pass.key[i] = k;
        fg_pass_attachment *a = i < 8 ? &key.color[i] : &key.depth;
        *a = (fg_pass_attachment){.key = k, .vk_format = i < 8 ? R.cfmt[i] : R.dfmt,
            .width = vk_tex_width(x, mip), .height = vk_tex_height(x, mip), .samples = vk_tex_samples(x)};
        if (cleared >> i & 1) {
            fg_dep_clear(&FGX.deps, k, FGX.passes.physical_passes + 1);
            fg_extract_revoke(k,"attachment-load-clear",FGX.passes.physical_passes+1);
        }
        FGX.pass.old[i] = fg_dep_read(&FGX.deps, k);
    }
    FGX.pass.group = fg_pass_begin(&FGX.passes, &key);
    fg_pass_guard_reset(&FGX.guard, UINT_MAX);
    if (FGX.captured && !FGX.invalid) {
        VkClearRect rect = {{{0,0},{R.host_w,R.host_h}},0,1};
        if (cleared & 1) {
            VkClearAttachment attachment = {VK_IMAGE_ASPECT_COLOR_BIT,0,R.cc[0]};
            (void)fg_extract_ui_clear(&attachment,&rect);
        }
        VkImageAspectFlags aspect = (cleared & (1u << 8) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
            (cleared & (1u << 9) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (aspect) {
            VkClearAttachment attachment = {aspect,0,{.depthStencil = {R.dfmt == VK_FORMAT_D16_UNORM ? d16_clear(R.cd) : R.cd,R.cs}}};
            (void)fg_extract_ui_clear(&attachment,&rect);
        }
    }
}
static void fg_extract_publish_pass(void) {
    fg_extract_span();
    if (FGX.pass.published_draws == g_stat_draws) return;
    unsigned writer = FGX.passes.physical_passes;
    /* Ordered span observations certify a depth VALUE, not an untouched pixel.
       Same-value writes/restoration preserve value authority; proven geometry
       after an unproven run is not a snapshot-to-final unknown. Colour changes
       remain the already accepted non-depth-writing-pass concern. */
    int masked=FGX.authority_ready && FGX.pass.root==(int)FGX.authority_root &&
        fg_extract_same_key(FGX.pass.key[8],FGX.authority_key);
    fg_dep_mask seed = FGX.pass.root >= 0 && FGX.pass.root_committed && FGX.pass.depth_draws &&
        (!FGX.pass.unproven_depth || masked) ? UINT64_C(1) << FGX.pass.root : 0;
    for (unsigned i = 0; g_stat_draws != R.draws0 && i < 9; i++) {
        if (!FGX.pass.key[i].backing || !(i < 8 ? FGX.pass.outputs >> i & 1 : FGX.pass.depth_draws != 0)) continue;
        fg_dep_sources sources; fg_dep_sources_init(&sources);
        if (!seed) sources.value = FGX.pass.reads.value;
        fg_dep_sources_add(&FGX.deps, &sources, FGX.pass.old[i]);
        if (i == 8 && !seed) fg_extract_revoke(FGX.pass.key[i],"raster-depth-without-accepted-camera",writer);
        if (FGX.nscene)
            fg_extract_publish(FGX.pass.key[i], i < 8 ? S.rt.color[i] : S.rt.depth, &sources, writer, seed);
    }
    FGX.pass.published_draws = g_stat_draws;
}
static void fg_extract_pass_end(void) {
    fg_extract_publish_pass();
    fg_pass_end(&FGX.passes, FGX.pass.group, g_stat_draws - R.draws0, FGX.pass.depth_draws);
}
static int fg_extract_reads_camera(const vk_shader *sh) {
    for (int i = 0; sh && i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->reg == 1 && (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) &&
            (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) return 1;
    }
    return 0;
}
static int fg_extract_camera_equal(const float a[84], const float b[84]) {
    /* Other banks in this block can change with a material. Only the camera
       fields consumed by prediction are camera identity. */
    return !memcmp(a,b,58*sizeof(float)) && a[62] == b[62] && a[70] == b[70] &&
        a[72] == b[72] && a[73] == b[73];
}
static void fg_extract_camera(vk_shader *const sh[6]) {
    if (FGX.captured || !FGX.pass.depth_write || FGX.pass.group < 0) return;
    const fg_pass_key *key = &FGX.passes.group[FGX.pass.group].key;
    const p_bind_buf *u = &S.ubo[0][1];
    FGX.pass.camera_check="no-declared-camera";
    if (!key->depth.key.backing) return;
    int standard=fg_extract_reads_camera(sh[0]) && u->addr && u->size==2368;
    unsigned layout=sh[0] ? sh[0]->framegen_camera_layout : 0;
    if (!standard && !layout) return;
    int proved_plane=sh[0] && sh[1] && sh[0]->framegen_plane && sh[1]->framegen_plane;
    int experimental_plane=sh[0] && sh[1] && sh[0]->framegen_uncertified_plane &&
        sh[1]->framegen_plane && vk_fg_uncertified_plane();
    int plane=proved_plane || experimental_plane;
    if (sh[1] && sh[1]->writes_depth && !sh[1]->framegen_raster_depth && !plane) {
        FGX.pass.camera_check="fragment-depth-export"; return;
    }
    if (key->depth.width != R.w || key->depth.height != R.h || key->depth.samples != 1 ||
        key->depth.vk_format != VK_FORMAT_D32_SFLOAT || !S.has_vp ||
        fabsf(S.vp[0][0]) > 0.5f || fabsf(S.vp[0][1]) > 0.5f ||
        fabsf(fabsf(S.vp[0][2]) - R.w) > 1 || fabsf(fabsf(S.vp[0][3]) - R.h) > 1) {
        FGX.pass.camera_check="unsupported-depth-viewport";
        if (FGX.pass.root >= 0) fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"scene camera source or viewport no longer accepted");
        return;
    }
    if (S.dr[0]!=0 || S.dr[1]!=1) { FGX.pass.camera_check="unsupported-depth-range"; return; }
    FGX.pass.camera_fragment_addr=0;
    if (plane) {
        /* The audited e87/ecbe pair exports the farther of raster depth and
           the SAME camera's projection of the view ray's world-Y=4 intersection.
           A shader name nominates this proof; it never bypasses runtime checks. */
        float camera[84]; vk_ub_read(u->addr,camera,sizeof camera);
        float gate=NAN,fragment_a=NAN;
        const char *why="plane-camera-binding"; int root=-1;
        const p_bind_buf *fragment=&S.ubo[1][1], *control=&S.ubo[0][11];
        FGX.pass.camera_addr=u->addr; FGX.pass.camera_size=sizeof camera;
        FGX.pass.camera_aux_addr=0;
        do {
            if (!standard || !fragment->addr || fragment->size!=2368 ||
                !control->addr || control->size<2*sizeof(float)) break;
            FGX.pass.camera_fragment_addr=fragment->addr;
            FGX.pass.camera_aux_addr=control->addr+sizeof(float);
            vk_ub_read(FGX.pass.camera_aux_addr,&gate,sizeof gate);
            why="plane-clip-shrink-active";
            if (!isfinite(gate) || (gate!=0 && !experimental_plane)) break;
            if (gate==0 && !proved_plane) { why="uncertified-plane-inactive"; break; }
            why="plane-projection-coefficients";
            if (!fg_math_plane_projection(camera)) break;
            float storage[84]; const float *fragment_camera=camera;
            if (fragment->addr!=u->addr) {
                vk_ub_read(fragment->addr,storage,sizeof storage); fragment_camera=storage;
            }
            fragment_a=fragment_camera[65];
            why="plane-stage-camera-mismatch";
            if (!fg_extract_camera_equal(camera,fragment_camera) ||
                camera[60]!=fragment_camera[60] || camera[65]!=fragment_camera[65]) break;
            why="plane-live-camera-mismatch";
            for (unsigned i=0;i<FGX.nscene;i++) if (FGX.scene[i].valid &&
                fg_extract_same_key(FGX.scene[i].depth_key,key->depth.key) &&
                fg_extract_camera_equal(camera,FGX.scene[i].camera) &&
                camera[60]==FGX.scene[i].camera[60] && camera[65]==FGX.scene[i].camera[65]) {
                root=(int)i; break;
            }
            if (root<0) break; /* cannot introduce a root through a plane fill */
            if (FGX.pass.root>=0 && FGX.pass.root!=root) {
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"plane camera changed within a physical pass");
                why="plane-pass-camera-mismatch"; break;
            }
            FGX.pass.root=root;
            if (gate!=0) {
                /* Opt-in assumption ONLY: keep the ordinary unknown-depth
                   baseline and require isolated before/after value attribution. */
                FGX.pass.uncertified_context=1;
                why="UNCERTIFIED-active-plane-candidate";
            } else {
                FGX.pass.camera_context=1;
                why="accepted-world-plane-camera";
            }
        } while (0);
        FGX.pass.camera_check=why;
        if (FGX.diag) {
            static uint64_t reported[2]={UINT64_MAX,UINT64_MAX};
            unsigned accepted=FGX.pass.camera_context!=0;
            if (reported[accepted]!=FGX.frame) {
                reported[accepted]=FGX.frame;
                LOGV("[framegen-plane] real %llu result=%s root=%d shrink-scale=%.9g A=%.9g FS-A=%.9g projection-z=%.9g,%.9g near=%.9g world-Y=4 vs=%.80s fs=%.80s",
                    (unsigned long long)FGX.frame,why,root,(double)gate,(double)camera[65],
                    (double)fragment_a,(double)camera[38],(double)camera[39],(double)camera[56],
                    sh[0]->key,sh[1]->key);
            }
        }
        return;
    }
    if (!standard) {
        const p_bind_buf *alternate=&S.ubo[0][5], *material=&S.ubo[0][6];
        FGX.pass.camera_check="alternate-camera-binding";
        if (!alternate->addr || alternate->size<32*sizeof(float)) return;
        FGX.pass.camera_addr=alternate->addr; FGX.pass.camera_size=32*sizeof(float);
        FGX.pass.camera_aux_addr=0;
        float matrices[32]; vk_ub_read(alternate->addr,matrices,sizeof matrices);
        if (layout==2) {
            if (!material->addr || material->size<68*sizeof(float)) return;
            FGX.pass.camera_aux_addr=material->addr+67*sizeof(float);
            float bias; vk_ub_read(material->addr+67*sizeof(float),&bias,sizeof bias);
            if (bias!=0) { FGX.pass.camera_check="alternate-camera-depth-bias"; return; }
        }
        FGX.pass.camera_check="alternate-camera-mismatch";
        for (unsigned i=0;i<FGX.nscene;i++) {
            const fg_extract_scene *scene=&FGX.scene[i];
            if (!scene->valid || !fg_extract_same_key(scene->depth_key,key->depth.key) ||
                memcmp(matrices,scene->camera,12*sizeof(float)) ||
                matrices[12]!=0 || matrices[13]!=0 || matrices[14]!=0 || matrices[15]!=1 ||
                memcmp(matrices+16,scene->camera+28,16*sizeof(float))) continue;
            if (FGX.pass.root>=0 && FGX.pass.root!=(int)i) {
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"alternate scene camera changed within a physical pass");
                return;
            }
            FGX.pass.root=(int)i; FGX.pass.camera_context=1;
            FGX.pass.camera_check="accepted-alternate-view-projection";
            return;
        }
        if (FGX.pass.root>=0)
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"alternate camera mismatch within a physical pass");
        return; /* alternate layout can continue a proven root, never invent one */
    }
    FGX.pass.camera_addr = u->addr;
    FGX.pass.camera_size=84*sizeof(float); FGX.pass.camera_aux_addr=0;
    float camera[84]; vk_ub_read(u->addr, camera, sizeof camera);
    for (unsigned i = 0; i < FGX.nscene; i++) if (FGX.scene[i].valid &&
        fg_extract_same_key(FGX.scene[i].depth_key, key->depth.key) &&
        fg_extract_camera_equal(camera, FGX.scene[i].camera)) {
        FGX.pass.root = (int)i; FGX.pass.camera_context = 1;
        FGX.pass.camera_check="accepted-standard-camera"; return;
    }
    if (FGX.pass.root >= 0) {
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"accepted scene camera changed within a physical pass"); return;
    }
    if (fg_pass_color_count(key) < 2 || fg_math_camera(camera,R.w,R.h) != FG_MATH_OK) return;
    for (unsigned i = 0; i < 8; i++) if (key->color[i].key.backing &&
        (key->color[i].width != R.w || key->color[i].height != R.h || key->color[i].samples != 1)) return;
    fg_dep_mask bit = fg_dep_candidate(&FGX.deps, 1);
    if (!bit) return;
    unsigned index = FGX.nscene++;
    fg_extract_scene *c = &FGX.scene[index];
    *c = (fg_extract_scene){.depth_desc = *S.rt.depth, .depth_key = key->depth.key,
        .group = (unsigned)FGX.pass.group, .signature = fg_pass_scene_signature(key), .valid = 1};
    if (FGX.diag) c->diag.admission_pass=FGX.passes.physical_passes;
    if (FGX.diag) FGX.scene_depth_image[index]=vk_tex_canvas_identity(R.dtex,key->depth.key.mip,key->depth.key.layer);
    memcpy(c->camera, camera, sizeof c->camera);
    if (vk_fg_dump_requested()) {
        memcpy(c->admitted_viewport,S.vp[0],sizeof c->admitted_viewport);
        c->admitted_scissor[0]=S.sc.x; c->admitted_scissor[1]=S.sc.y;
        c->admitted_scissor[2]=S.sc.w; c->admitted_scissor[3]=S.sc.h;
    }
    FGX.pass.root = (int)index; FGX.pass.camera_context = 1;
}
static void fg_extract_sample(fg_dep_sources *sources, const vk_treg *r, int target) {
    vk_tex *x = vk_tex_get(&r->t);
    if (!x) { sources->value.tainted |= FG_DEP_TAINT_IDENTITY; FGX.diag_window.missing++; return; }
    unsigned mip = r->has_v && r->v.set_levels ? r->v.base_level : 0;
    unsigned layer = r->has_v && r->v.set_layers ? r->v.min_layer : 0;
    unsigned levels = r->has_v && r->v.set_levels && r->v.nlevels ? r->v.nlevels : vk_tex_levels(x)-MINV(mip,vk_tex_levels(x));
    unsigned layers = r->has_v && r->v.set_layers && r->v.nlayers ? r->v.nlayers : vk_tex_layers(x)-MINV(layer,vk_tex_layers(x));
    if (vk_tex_is3d(x)) { layer=0; layers=1; }
    else if (target == 1) layers = 1;
    else if (target == 8 && (r->has_v && r->v.set_target ? r->v.target : r->t.d.target) == 9) layers=6;
    if (!levels || !layers || mip >= vk_tex_levels(x) || levels > vk_tex_levels(x)-mip ||
        layer >= vk_tex_layers(x) || layers > vk_tex_layers(x)-layer) {
        sources->value.tainted |= FG_DEP_TAINT_VIEW; FGX.diag_window.bounds++; return;
    }
    unsigned aspect = vk_tex_aspect(x);
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    unsigned work=fg_dep_sample_work(&FGX.deps,levels,layers);
    if (FGX.diag) {
        FGX.diag_window.binding_view[FGX.binding_class]+=work;
        FGX.diag_window.binding_old_view[FGX.binding_class]+=
            levels == 1 && layers == 1 ? 1 : FGX.deps.count;
    }
    if (!fg_pass_take_bindings(&FGX.passes,work)) {
        if (FGX.diag) FGX.diag_window.binding_overflows[FGX.binding_class]++;
        sources->value.tainted |= FG_DEP_TAINT_UNKNOWN; return;
    }
    (void)fg_dep_sample_view(&FGX.deps,sources,fg_extract_image_key(x,mip,layer,aspect).backing,
        mip,levels,layer,layers,aspect);
}
static void fg_extract_storage_buffers(const vk_shader *shader, unsigned stage, fg_dep_sources *sources) {
    if (!FGX.nscene || !shader || !shader->has_storage_writes) return;
    for (int i = 0; i < shader->nb; i++) {
        const vk_bind *b = &shader->b[i];
        if (!b->rw) continue;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            if (stage == 5) continue; /* compute outputs are versioned by fg_extract_compute */
            uint64_t handle = b->reg < 0 ? 0 : b->kind == RB_IMG ?
                (b->reg < 32 ? S.img[stage][b->reg] : 0) : (b->reg < 128 ? S.tex[stage][b->reg] : 0);
            vk_treg r;
            if (!handle || !vk_treg_get((int)(handle & 0xfffff),&r)) continue; /* actual dummy */
            vk_tex *x=vk_tex_get(&r.t);
            if (!x) continue;
            uint64_t backing=fg_extract_image_key(x,0,0,vk_tex_aspect(x)).backing;
            for (unsigned w=0;w<FGX.nwatch;w++) if (FGX.watch[w].key.backing == backing) {
                fg_extract_invalidate(FGX.watch[w].key,FG_PASS_WRITER_OPERATION | ++FGX.operation,
                    &FGX.watch_image[w],"graphics-storage-image-write");
                fg_extract_revoke(FGX.watch[w].key,"graphics-storage-image-write",FG_PASS_WRITER_OPERATION|FGX.operation);
                sources->value.tainted |= FG_DEP_TAINT_STORAGE;
                FGX.diag_window.image_writes++; FGX.diag_window.storage++;
                if (FGX.captured && (backing == FGX.canvas_key.backing || backing == FGX.ui_depth_key.backing))
                    fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"graphics storage image modifies captured canvas/depth");
            }
            continue;
        }
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) continue;
        uint64_t address = 0, size = 0;
        if (b->reg < 0 || b->reg >= 16) { fg_dep_fail(&FGX.deps); continue; }
        if (b->kind == RB_SBUF) { address=S.ssbo[stage][b->reg].addr; size=S.ssbo[stage][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { address=S.ubo[stage][b->reg].addr; size=S.ubo[stage][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) {
            const p_bind_buf *u=&S.ubo[stage][b->reg];
            if (u->addr && u->size >= (uint64_t)b->off+8) vk_ub_read(u->addr+b->off,&address,8);
            else fg_dep_fail(&FGX.deps);
        } else fg_dep_fail(&FGX.deps);
        unsigned before=FGX.operation;
        vk_fg_extract_memory_write(address,size);
        if (FGX.operation != before) { sources->value.tainted |= FG_DEP_TAINT_STORAGE; FGX.diag_window.storage++; }
    }
}
static void fg_extract_reads(vk_shader *const sh[6], unsigned first, unsigned end, fg_dep_sources *sources) {
    if (!FGX.nscene) return;
    unsigned before=FGX.passes.bindings_used;
    uint64_t start=0;
    if (FGX.diag) {
        FGX.binding_class=first == 5 ? 2 : FGX.pass.depth_write ? 1 : 0;
        FGX.diag_window.binding_calls[FGX.binding_class]++;
        start=fg_extract_cpu_ns();
    }
    for (unsigned st = first; st < end; st++) {
        if (!sh[st]) continue;
        if (FGX.diag && sh[st]->nb > 0)
            FGX.diag_window.binding_reflected[FGX.binding_class]+=(unsigned)sh[st]->nb;
        if (sh[st]->nb < 0 || !fg_pass_take_bindings(&FGX.passes, (unsigned)sh[st]->nb)) {
            if (FGX.diag) FGX.diag_window.binding_overflows[FGX.binding_class]++;
            sources->value.tainted = 1; goto done;
        }
        for (int i = 0; i < sh[st]->nb; i++) {
            const vk_bind *b = &sh[st]->b[i];
            if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
            if (b->rw) continue;
            uint64_t handle = b->reg < 0 ? 0 : b->kind == RB_IMG ?
                (b->reg < 32 ? S.img[st][b->reg] : 0) : (b->reg < 128 ? S.tex[st][b->reg] : 0);
            vk_treg r;
            /* Match the real emitter's deterministic dummy for an unbound
               reflected slot; it cannot supply a scene dependency. */
            if (!handle) { FGX.diag_window.dummy++; continue; }
            if (!vk_treg_get((int)(handle & 0xfffff), &r)) {
                sources->value.tainted |= FG_DEP_TAINT_IDENTITY; FGX.diag_window.missing++; continue;
            }
            fg_extract_sample(sources, &r, b->target);
        }
    }
done:
    if (FGX.diag) {
        uint64_t end=fg_extract_cpu_ns(), elapsed=end >= start ? end-start : 0;
        FGX.diag_window.binding_cpu_ns+=elapsed;
        if (elapsed>FGX.diag_window.binding_cpu_max) FGX.diag_window.binding_cpu_max=elapsed;
        unsigned group=first == 5 || FGX.pass.group < 0 ?
            FG_PASS_MAX_GROUPS : (unsigned)FGX.pass.group;
        unsigned work=FGX.binding_group_work[group]+=FGX.passes.bindings_used-before;
        if (work>FGX.diag_window.binding_top_work) {
            FGX.diag_window.binding_top_work=work;
            FGX.diag_window.binding_top_frame=FGX.frame;
            FGX.diag_window.binding_top_group=group == FG_PASS_MAX_GROUPS ? -1 : (int)group;
            const fg_pass_key *key=group == FG_PASS_MAX_GROUPS ? NULL : &FGX.passes.group[group].key;
            FGX.diag_window.binding_top_signature=key ? fg_pass_scene_signature(key) : 0;
            FGX.diag_window.binding_top_width=key ? key->width : 0;
            FGX.diag_window.binding_top_height=key ? key->height : 0;
        }
    }
}
static void fg_extract_snapshot_state(const char *check, fg_dep_result result, fg_dep_value value) {
    if (!FGX.diag || (FGX.last_snapshot_check && !strcmp(check,FGX.last_snapshot_check) &&
        FGX.last_snapshot_result == (unsigned)result && FGX.last_snapshot_taint == value.tainted)) return;
    LOGV("[framegen-extract] real %llu snapshot check=%s result=%s roots=%u origins=%#llx eligible=%#llx unresolved=%#llx taint=%#x (unknown=1 identity=2 view=4 storage=8 clear=16 mapping=32) tracking-flags=%#x",
        (unsigned long long)FGX.frame,check,fg_dep_reason(result),FGX.nscene,
        (unsigned long long)value.origins,(unsigned long long)value.eligible,
        (unsigned long long)value.unresolved,value.tainted,FGX.passes.failed);
    FGX.last_snapshot_check=check; FGX.last_snapshot_result=result; FGX.last_snapshot_taint=value.tainted;
}
static int g_fg_job_trace; /* tentative; defined with the presenter state below */
static void fg_extract_capture(vk_tex *canvas, unsigned level, unsigned layer) {
    if (R.open) fg_extract_publish_pass(); /* logical checkpoint, not a GMEM store/reload */
    fg_dep_value before = fg_dep_read(&FGX.deps,
        fg_extract_image_key(canvas, level, layer, VK_IMAGE_ASPECT_COLOR_BIT));
    unsigned root;
    fg_dep_result result = fg_dep_select(&FGX.deps, before, &root);
    FGX.diag_window.value = before; FGX.diag_window.result = result; FGX.diag_window.root_count = FGX.nscene;
    if (result != FG_DEP_SELECTED || FGX.passes.failed) {
        fg_extract_refusal_resources(canvas,level,layer,before);
        fg_extract_snapshot_state("canvas-ancestry",result,before);
        fg_extract_reject(result == FG_DEP_FAILED || FGX.passes.failed ? VK_FG_TRACK_CAPACITY : VK_FG_CHAIN_BROKEN,
            fg_dep_reason(result)); return;
    }
    fg_extract_scene *c = &FGX.scene[root];
    vk_tex *depth = vk_tex_get(&c->depth_desc);
    if (!c->valid || !depth || !fg_extract_same_key(c->depth_key,
        fg_extract_image_key(depth,c->depth_key.mip,c->depth_key.layer,VK_IMAGE_ASPECT_DEPTH_BIT))) {
        fg_extract_snapshot_state("live-depth-incarnation",result,before);
        fg_extract_reject(VK_FG_DEPTH_LOST,"scene depth no longer live"); return;
    }
    if (FGX.authority_ready && root!=FGX.authority_root) {
        fg_extract_reject(VK_FG_DEPTH_LOST,"selected camera differs from depth certificate"); return;
    }
    vk_end_rendering_at(VK_PASS_END_COPY,0); /* proof succeeded; only now split for the actual snapshot */
    FGX.selected = root; FGX.captured = 1;
    FGX.canvas_key = fg_extract_image_key(canvas,level,layer,VK_IMAGE_ASPECT_COLOR_BIT);
    if (FGX.signature == c->signature) FGX.stable_frames++;
    else {
        if (g_fg_job_trace)
            LOGV("[adaptive-scene] event=history-cut reason=scene-signature extract_frame=%llu now_ns=%llu old_signature=0x%016llx new_signature=0x%016llx old_stable_frames=%llu",
                (unsigned long long)FGX.frame,(unsigned long long)host_now_ns(),
                (unsigned long long)FGX.signature,(unsigned long long)c->signature,(unsigned long long)FGX.stable_frames);
        FGX.signature = c->signature; FGX.stable_frames = 1; vk_fg_history_cut();
    }
    vk_fg_camera(c->camera);
    VkCommandBuffer cb = vk_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
    if (!vk_fg_capture(cb,canvas,level,layer,depth,c->depth_key.mip,c->depth_key.layer,
        vk_tex_width(canvas,level),vk_tex_height(canvas,level))) {
        fg_extract_snapshot_state("copy-format-ring-resources",result,before);
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"generation source format, ring or GPU resources unavailable");
    } else {
        FGX.snapshot_ok = 1;
        vk_fg_capture_geometry(FGX.frame,c->admitted_viewport,c->admitted_scissor);
        fg_extract_snapshot_state("captured",result,before);
    }
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = g_pipe_dirty = 1;
    descriptor_unbind(); vk_stage_sig_invalidate(0x3fu);
}
static void fg_extract_boundary(vk_tex *canvas, unsigned level, unsigned layer) {
    fg_extract_capture(canvas,level,layer);
    FGX.ui_seen = 1;
    if (!FGX.snapshot_ok) return;
    FGX.ui_depth = S.rt.depth ? vk_tex_get(S.rt.depth) : NULL;
    FGX.ui_depth_level = view_level(&S.rt.dview,S.rt.has_dview);
    FGX.ui_depth_layer = S.rt.has_dview && S.rt.dview.set_layers ? S.rt.dview.min_layer : 0;
    FGX.ui_depth_key = fg_extract_image_key(FGX.ui_depth,FGX.ui_depth_level,FGX.ui_depth_layer,
        FGX.ui_depth ? vk_tex_aspect(FGX.ui_depth) : 0);
    FGX.ui_depth_address = S.rt.depth ? (uint64_t)(uintptr_t)S.rt.depth->mem : 0;
    FGX.ui_depth_size = S.rt.depth ? S.rt.depth->L.size : 0;
}
#ifdef TOTK_VK_TESTS
/* Resolve only explicitly registered, fixture-owned programs. Real walker
 * geometry/binding/native attachment checks and emitted packet freezing remain
 * untouched; production builds contain neither this registry nor this hook. */
int vk_fg_test_canvas_programs(prog_t *ui,prog_t *terminal) {
    static vk_shader *vertex,*fragment;
    static uint8_t old_ui,old_transfer;
    if (vertex) vertex->canvas_ui=old_ui;
    if (fragment) fragment->canvas_transfer=old_transfer;
    vertex=fragment=NULL;
    (void)vk_canvas_test_shaders(NULL,NULL);
    if (!ui && !terminal) return 1;
    vk_shader *u[6]={0},*t[6]={0};
    if (!ui || !terminal || vk_prog_resolve(ui,u) || vk_prog_resolve(terminal,t) ||
        !u[0] || !t[1] || !vk_canvas_test_shaders(u[0]->key,t[1]->key)) return 0;
    vertex=u[0]; fragment=t[1];
    old_ui=vertex->canvas_ui; old_transfer=fragment->canvas_transfer;
    vertex->canvas_ui=2; fragment->canvas_transfer=1;
    return 1;
}
void vk_fg_test_alias_lineage(const tex_t *root, const tex_t *other, unsigned results[3]) {
    fg_extract_refresh(0);
    fg_dep_reset(&FGX.deps); FGX.nwatch = FGX.nscene = 0;
    FGX.nunknown=FGX.unknown_overflow=0;
    fg_dep_key a=fg_extract_image_key(vk_tex_get(root),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_key b=fg_extract_image_key(vk_tex_get(other),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_extract_publish(a,root,NULL,1,fg_dep_candidate(&FGX.deps,1));
    results[0]=fg_dep_select(&FGX.deps,fg_dep_read(&FGX.deps,a),NULL);
    fg_extract_publish(b,other,NULL,2,0);
    results[1]=fg_dep_select(&FGX.deps,fg_dep_read(&FGX.deps,a),NULL);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)root->mem,root->L.size);
    results[2]=fg_dep_select(&FGX.deps,fg_dep_read(&FGX.deps,a),NULL);
    if (FGX.diag) fg_extract_refusal_resources(vk_tex_get(root),0,0,fg_dep_read(&FGX.deps,a));
    FGX.nwatch=0; fg_dep_reset(&FGX.deps);
}
unsigned vk_fg_test_nominate_canvas(vk_tex *canvas) {
    fg_extract_boundary(canvas,0,0);
    return !!FGX.captured | (!!FGX.invalid << 1) | (!!R.open << 2) |
        (!!fg_extract_inline() << 3) | (!!vk_fg_extract_active << 4);
}
void vk_fg_test_scene_begin(const tex_t *scene, const tex_t *depth, const float camera[84]) {
    vk_end_rendering();
    fg_dep_reset(&FGX.deps); fg_pass_reset(&FGX.passes);
    FGX.nscene=FGX.nwatch=FGX.operation=FGX.nunknown=FGX.unknown_overflow=0;
    FGX.invalid=FGX.captured=FGX.snapshot_ok=FGX.eligible=0; FGX.probe.wait=0;
    FGX.authority_ready=FGX.authority_gpu_ready=0; FGX.authority_key=(fg_dep_key){0};
    fg_extract_refresh(0);
    vk_tex *color=vk_tex_get(scene), *z=vk_tex_get(depth);
    fg_dep_key ck=fg_extract_image_key(color,0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_key dk=fg_extract_image_key(z,0,0,VK_IMAGE_ASPECT_DEPTH_BIT);
    fg_pass_key key={.width=vk_tex_width(color,0),.height=vk_tex_height(color,0)};
    key.color[0]=(fg_pass_attachment){.key=ck,.vk_format=vk_tex_format(color),
        .width=key.width,.height=key.height,.samples=1};
    key.depth=(fg_pass_attachment){.key=dk,.vk_format=vk_tex_format(z),
        .width=vk_tex_width(z,0),.height=vk_tex_height(z,0),.samples=1};
    int group=fg_pass_begin(&FGX.passes,&key);
    FGX.scene[0]=(fg_extract_scene){.depth_desc=*depth,.depth_key=dk,.valid=1,
        .group=(unsigned)group,.signature=fg_pass_scene_signature(&key)};
    memcpy(FGX.scene[0].camera,camera,sizeof FGX.scene[0].camera);
    if (FGX.diag) FGX.scene_depth_image[0]=vk_tex_canvas_identity(z,0,0);
    FGX.nscene=1;
    fg_dep_mask bit=fg_dep_candidate(&FGX.deps,1);
    fg_extract_publish(ck,scene,NULL,1,bit);
    fg_extract_publish(dk,depth,NULL,1,bit);
    fg_pass_end(&FGX.passes,group,1,1);
}
int vk_fg_test_capture_reads(const tex_t *canvas, const vk_treg *inputs, unsigned count) {
    vk_end_rendering();
    vk_tex *x=vk_tex_get(canvas);
    fg_dep_key key=fg_extract_image_key(x,0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    fg_dep_sources_add(&FGX.deps,&sources,fg_dep_read(&FGX.deps,key));
    for (unsigned i=0;i<count;i++) fg_extract_sample(&sources,&inputs[i],1);
    fg_extract_publish(key,canvas,&sources,FG_PASS_WRITER_OPERATION|++FGX.operation,0);
    fg_extract_capture(x,0,0);
    FGX.eligible=FGX.snapshot_ok && !FGX.invalid;
    return FGX.eligible;
}
#endif
static void fg_extract_draw_context(void *context, unsigned event) {
    (void)event;
    vk_shader *const *sh = context;
    fg_extract_span();
    fg_dep_sources_init(&FGX.pass.context);
    FGX.pass.camera_context = 0;
    FGX.pass.authority_context=0;
    FGX.pass.uncertified_context=0;
    if (FGX.diag) {
        FGX.pass.plane_context=sh[1] && sh[1]->framegen_plane;
        FGX.pass.viewport_pixels=(uint64_t)R.w*R.h;
        int64_t x0=0,y0=0,x1=R.w,y1=R.h;
        if (S.has_sc) {
            x0=MAXV(x0,(int64_t)S.sc.x); y0=MAXV(y0,(int64_t)S.sc.y);
            x1=MINV(x1,(int64_t)S.sc.x+S.sc.w); y1=MINV(y1,(int64_t)S.sc.y+S.sc.h);
        }
        FGX.pass.scissor_pixels=x1>x0 && y1>y0 ? (uint64_t)(x1-x0)*(uint64_t)(y1-y0) : 0;
    }
    /* Admission must use this context's depth state, not the previous shader
       context's cached flag. In-pass depth-off/on transitions are legal. */
    FGX.pass.depth_write = R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
    if (FGX.diag) {
        FGX.pass.vs=sh[0]->key; FGX.pass.fs=sh[1] ? sh[1]->key : NULL;
        FGX.pass.camera_declared=fg_extract_reads_camera(sh[0]);
    }
    fg_extract_camera(sh);
#ifdef TOTK_VK_TESTS
    if (fg_test_writer_classification>=0) {
        unsigned classification=(unsigned)fg_test_writer_classification;
        FGX.pass.camera_context=classification==VK_FG_WRITER_PROVEN;
        FGX.pass.uncertified_context=classification==VK_FG_WRITER_UNCERTIFIED && vk_fg_uncertified_plane();
        FGX.pass.camera_check=FGX.pass.camera_context ? "fixture-proven-camera" : "fixture-unproven-camera";
        unsigned root=FGX.authority_ready ? FGX.authority_root : 0;
        if (FGX.pass.camera_context && root<FGX.nscene && FGX.scene[root].valid &&
            fg_extract_same_key(FGX.scene[root].depth_key,FGX.pass.key[8]))
            FGX.pass.root=(int)root;
    }
#endif
    if (!FGX.pass.camera_context && FGX.pass.depth_write && FGX.authority_ready &&
        FGX.authority_root<FGX.nscene && FGX.scene[FGX.authority_root].valid &&
        fg_extract_same_key(FGX.pass.key[8],FGX.authority_key)) {
        FGX.pass.root=(int)FGX.authority_root;
        FGX.pass.authority_context=1;
    }
    if (FGX.invalid) return;
    int ui = fg_pass_ui_context(FGX.ui_seen,sh[0]->canvas_ui,
        fg_extract_canvas(R.ctex[0],R.clevel[0],R.clayer[0]));
    unsigned outputs = sh[1] ? sh[1]->fout_mask : 0;
    for (unsigned i = 0; i < 8; i++) if (!(S.chmask.mask >> (4 * i) & 15)) outputs &= ~(1u << i);
    FGX.pass.context_outputs = outputs;
    if (FGX.pass.root < 0 && !ui) fg_extract_reads(sh, 0, 2, &FGX.pass.context);
    /* Extent-derived normalized scaling only models full, axis-aligned
       composition. A contributing cropped/offset viewport is not that map.
       UI and terminal transfer have their own separately frozen coordinates. */
    if (!FGX.captured && FGX.pass.context.value.origins && outputs &&
        (!S.has_vp || fabsf(S.vp[0][0]) > 0.5f || fabsf(S.vp[0][1]) > 0.5f ||
         fabsf(S.vp[0][2]-R.w) > 0.5f || fabsf(S.vp[0][3]-R.h) > 0.5f))
        FGX.pass.context.value.tainted |= FG_DEP_TAINT_MAPPING;
    fg_extract_storage_buffers(sh[0],0,&FGX.pass.context);
    fg_extract_storage_buffers(sh[1],1,&FGX.pass.context);
    if (FGX.captured) {
        int canvas = 0;
        for (unsigned i = 0; i < R.ncolor; i++) if (fg_extract_canvas(R.ctex[i],R.clevel[i],R.clayer[i])) {
            canvas = 1;
            if (!ui || i) fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"non-UI work after scene/UI boundary");
        }
        if (!canvas && FGX.pass.depth_write && R.dtex && fg_extract_ui_depth(R.dtex,R.dlevel,R.dlayer))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"unrecorded work modifies captured UI depth");
    }
}
/* Identify the unique existing camera root logically. GPU reservation and the
   first clipped before-copy share ONE later physical boundary; reserving here
   would manufacture an empty store/reload pass before the first unknown draw. */
static int fg_extract_authority_before_draw(vk_shader *const sh[6]) {
    if (!vk_fg_extract_active || FGX.invalid || FGX.captured ||
        !FGX.pass.depth_write || FGX.pass.camera_context || FGX.pass.authority_context ||
        !vk_fg_interpolating()) return 1;
    fg_dep_key key=FGX.pass.key[8];
    int root=-1;
    for (unsigned i=0;i<FGX.nscene;i++) if (FGX.scene[i].valid &&
        fg_extract_same_key(FGX.scene[i].depth_key,key)) {
        if (root>=0) return 1; /* ambiguous camera authority: old refusal applies */
        root=(int)i;
    }
    if (root<0 || FGX.authority_ready) return 1;
    FGX.authority_ready=1; FGX.authority_root=(unsigned)root; FGX.authority_key=key;
    fg_pass_guard_invalidate(&FGX.guard);
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,(void *)sh);
    return 1;
}
static void fg_extract_authority_span_end(VkCommandBuffer cb) {
    if (!FGX.authority_span.open) return;
    vk_tex *depth=FGX.authority_span.depth;
    unsigned level=FGX.authority_span.level,layer=FGX.authority_span.layer;
    FGX.authority_span.open=0;
    if (FGX.invalid) vk_fg_skip();
    if (!vk_fg_authority_span_end(cb,depth,level,layer))
        fg_extract_reject(VK_FG_DEPTH_LOST,"ordered depth authority after-copy unavailable");
}
static void fg_extract_writer_end(VkCommandBuffer cb) {
    if (!FGX.writer.open) return;
    FGX.writer.open=0;
    if (FGX.invalid || !vk_fg_extract_active) {
        vk_fg_writer_poison("extractor rejected with pending writer span");
        return;
    }
    (void)vk_fg_writer_end(cb,FGX.writer.depth,FGX.writer.level,FGX.writer.layer,
        g_stat_draws-FGX.writer.draw0);
}
static int fg_extract_depth_spans_before_draw(vk_shader *const sh[6]) {
    /* Track VALUE changes only inside unproven raster spans. A clipped scissor
       is an upper bound on writes, NEVER evidence every covered pixel was hit.
       Proven draws between spans are observed by the next before-copy or final
       copy, not compared against an obsolete first-unknown snapshot. */
    int writing=R.dfmt!=VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
    int target=writing && vk_fg_extract_active && !FGX.invalid && !FGX.captured &&
        FGX.authority_ready && fg_extract_same_key(FGX.pass.key[8],FGX.authority_key);
    int unknown=target && !FGX.pass.camera_context && FGX.pass.authority_context;
    unsigned experimental=unknown && FGX.pass.uncertified_context;
    int64_t x0=0,y0=0,x1=R.w,y1=R.h;
    if (S.has_sc) {
        x0=MAXV(x0,(int64_t)S.sc.x); y0=MAXV(y0,(int64_t)S.sc.y);
        x1=MINV(x1,(int64_t)S.sc.x+S.sc.w); y1=MINV(y1,(int64_t)S.sc.y+S.sc.h);
    }
    /* No depth command can affect the pending span on an empty scissor or with
       writes disabled. Invalid extraction must still close/poison its state. */
    if (!FGX.invalid && (!writing || x1<=x0 || y1<=y0)) return 1;
    unsigned x=(unsigned)x0,y=(unsigned)y0,width=(unsigned)(x1-x0),height=(unsigned)(y1-y0);
    /* TOTK_FG_ADAPTIVE_LEAN (adaptive QCOM scheduler): the CPU authority
       classification above still admits this writer exactly as before, but no
       consumer reads the ordered VALUE result, so no GPU span or pass split. */
    if (unknown && vk_fg_adaptive_lean_span()) unknown=0, experimental=0;
    int same=unknown && FGX.authority_span.open && FGX.authority_span.depth==R.dtex &&
        FGX.authority_span.level==R.dlevel && FGX.authority_span.layer==R.dlayer &&
        FGX.authority_span.experimental==experimental &&
        x>=FGX.authority_span.x && y>=FGX.authority_span.y &&
        (uint64_t)x+width<=(uint64_t)FGX.authority_span.x+FGX.authority_span.width &&
        (uint64_t)y+height<=(uint64_t)FGX.authority_span.y+FGX.authority_span.height;
    unsigned classification=FGX.pass.camera_context ? VK_FG_WRITER_PROVEN :
        experimental ? VK_FG_WRITER_UNCERTIFIED : VK_FG_WRITER_UNPROVEN;
    const char *vs=sh[0] ? sh[0]->key : "-", *fs=sh[1] ? sh[1]->key : "-";
    const char *check=FGX.pass.camera_check ? FGX.pass.camera_check : "no-camera-check";
    int diagnostic=target && vk_fg_writer_diag() &&
        (FGX.authority_gpu_ready ? vk_fg_writer_available() : unknown);
    int same_writer=diagnostic && FGX.writer.open && FGX.writer.depth==R.dtex &&
        FGX.writer.level==R.dlevel && FGX.writer.layer==R.dlayer &&
        FGX.writer.classification==classification && !strcmp(FGX.writer.vs,vs) &&
        !strcmp(FGX.writer.fs,fs) && !strcmp(FGX.writer.check,check);
    int boundary=(FGX.authority_span.open && !same) || (unknown && !FGX.authority_span.open) ||
        (FGX.writer.open && !same_writer) || (diagnostic && !FGX.writer.open);
    if (!boundary) return 1;
    /* One physical boundary serves production and optional diagnostics together.
       Either hook splitting independently would create empty/stale spans. */
    vk_tex *depth=R.dtex; unsigned level=R.dlevel,layer=R.dlayer;
    vk_end_rendering_at(diagnostic ? VK_PASS_END_DIAGNOSTIC : VK_PASS_END_COPY,0);
    if (target && !FGX.invalid) {
        if (unknown) {
            if (!vk_fg_authority_span_begin(vk_cb(),depth,level,layer,x,y,width,height,experimental))
                fg_extract_reject(VK_FG_DEPTH_LOST,"ordered depth authority before-copy unavailable");
            else {
                FGX.authority_gpu_ready=1;
                FGX.authority_span.depth=depth;
                FGX.authority_span.level=level; FGX.authority_span.layer=layer;
                FGX.authority_span.x=x; FGX.authority_span.y=y;
                FGX.authority_span.width=width; FGX.authority_span.height=height;
                FGX.authority_span.experimental=experimental; FGX.authority_span.open=1;
            }
        }
        if (diagnostic && !FGX.invalid &&
            vk_fg_writer_begin(vk_cb(),depth,level,layer,vs,fs,check,classification)) {
            FGX.writer.depth=depth; FGX.writer.level=level; FGX.writer.layer=layer;
            FGX.writer.vs=vs; FGX.writer.fs=fs; FGX.writer.check=check;
            FGX.writer.classification=classification;
            FGX.writer.draw0=g_stat_draws; FGX.writer.open=1;
        }
    }
    if (!begin_rendering(1)) {
        vk_fg_writer_poison("real rendering failed to resume after ordered span copy");
        FGX.writer.open=0;
        return 0; /* real command recording failed, not a framegen-only refusal */
    }
    if (vk_fg_extract_active && !FGX.invalid)
        (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
            (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,(void *)sh);
    return 1;
}
#ifdef TOTK_VK_TESTS
int vk_fg_test_authority_draw_begin(const tex_t *scene, const tex_t *depth,
                                  const float camera[84], int seed, const VkRect2D *scissor) {
    if (seed) vk_fg_test_scene_begin(scene,depth,camera);
    FGX.test_authority_inline=1;
    p_set_rt rt={.n=1,.color={scene},.depth=depth};
    vk_draw_exec(OP_SET_RT,&rt);
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0;
    S.vp[0][2]=vk_tex_width(vk_tex_get(depth),0);
    S.vp[0][3]=vk_tex_height(vk_tex_get(depth),0);
    S.dr[0]=0; S.dr[1]=1; S.has_sc=scissor!=NULL;
    if (scissor) { S.sc.x=scissor->offset.x; S.sc.y=scissor->offset.y;
        S.sc.w=scissor->extent.width; S.sc.h=scissor->extent.height; }
    S.ds.dt=S.ds.dw=1; S.ds.df=7; S.chmask.mask=15;
    if (!begin_rendering(1)) return 0;
    static vk_shader vertex={.key="host-unknown-camera"};
    static vk_shader fragment={.key="host-unknown-depth-export",.writes_depth=1,.fout_mask=1};
    vk_shader *sh[6]={&vertex,&fragment};
    fg_pass_guard_invalidate(&FGX.guard);
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,sh);
    return fg_extract_authority_before_draw(sh) && fg_extract_depth_spans_before_draw(sh) &&
        !FGX.invalid && FGX.pass.authority_context;
}
void vk_fg_test_authority_draw_end(void) {
    g_stat_draws++;
    vk_end_rendering();
    FGX.test_authority_inline=0;
}
int vk_fg_test_writer_draw_begin(const tex_t *scene, const tex_t *depth,
                                const float camera[84], int seed, unsigned identity,
                                unsigned classification, const VkRect2D *scissor) {
    if (seed) vk_fg_test_scene_begin(scene,depth,camera);
    FGX.test_authority_inline=1;
    p_set_rt rt={.n=1,.color={scene},.depth=depth}; vk_draw_exec(OP_SET_RT,&rt);
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0;
    S.vp[0][2]=vk_tex_width(vk_tex_get(depth),0); S.vp[0][3]=vk_tex_height(vk_tex_get(depth),0);
    S.dr[0]=0; S.dr[1]=1; S.has_sc=scissor!=NULL;
    if (scissor) { S.sc.x=scissor->offset.x; S.sc.y=scissor->offset.y;
        S.sc.w=scissor->extent.width; S.sc.h=scissor->extent.height; }
    S.ds.dt=S.ds.dw=1; S.ds.df=7; S.chmask.mask=15;
    if (!begin_rendering(1)) return 0;
    static vk_shader vertex[2]={{.key="writer-fixture-vs-A"},{.key="writer-fixture-vs-B"}};
    static vk_shader fragment[2]={{.key="writer-fixture-fs-A",.writes_depth=1,.fout_mask=1},
        {.key="writer-fixture-fs-B",.writes_depth=1,.fout_mask=1}};
    vk_shader *sh[6]={&vertex[identity&1],&fragment[identity&1]};
    int saved_classification=fg_test_writer_classification;
    fg_test_writer_classification=(int)classification;
    fg_pass_guard_invalidate(&FGX.guard);
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,sh);
    int accepted=fg_extract_authority_before_draw(sh) && fg_extract_depth_spans_before_draw(sh) && !FGX.invalid;
    fg_test_writer_classification=saved_classification;
    return accepted;
}
void vk_fg_test_writer_draw_note(int close) {
    g_stat_draws++;
    if (close) { vk_end_rendering(); FGX.test_authority_inline=0; }
}
/* Metadata follows the exact experimental admission path; the test then emits
   a real explicit-depth Vulkan draw before note(). No guest shader is executed. */
unsigned vk_fg_test_uncertified_draw_begin(const tex_t *scene, const tex_t *depth,
                                         const float input[84], int seed, unsigned kind,
                                         const VkRect2D *scissor) {
    static float uniform[592],fragment_uniform[592],control[2];
    float camera[84]; memcpy(camera,input,sizeof camera);
    camera[65]=(1-camera[38])*0.5f;
    if (seed) vk_fg_test_scene_begin(scene,depth,camera);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)uniform,sizeof uniform);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)fragment_uniform,sizeof fragment_uniform);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)control,sizeof control);
    memset(uniform,0,sizeof uniform); memcpy(uniform,camera,sizeof camera);
    memset(fragment_uniform,0,sizeof fragment_uniform); memcpy(fragment_uniform,camera,sizeof camera);
    control[0]=0; control[1]=kind==3 ? NAN : 800;
    if (kind==2) fragment_uniform[47]+=1;
    if (kind==7) uniform[47]+=1;
    FGX.test_authority_inline=1;
    p_set_rt rt={.n=1,.color={scene},.depth=depth}; vk_draw_exec(OP_SET_RT,&rt);
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0;
    S.vp[0][2]=vk_tex_width(vk_tex_get(depth),0); S.vp[0][3]=vk_tex_height(vk_tex_get(depth),0);
    p_depth_range range={kind==4 ? 0.2f : 0,1}; vk_draw_exec(OP_DEPTH_RANGE,&range);
    S.has_sc=scissor!=NULL;
    if (scissor) { S.sc.x=scissor->offset.x; S.sc.y=scissor->offset.y;
        S.sc.w=scissor->extent.width; S.sc.h=scissor->extent.height; }
    S.ds.dt=S.ds.dw=1; S.ds.df=7; S.chmask.mask=15;
    p_bind_buf u={.stage=0,.index=1,.addr=(uint64_t)(uintptr_t)uniform,.size=sizeof uniform};
    vk_draw_exec(OP_BIND_UBO,&u);
    u.stage=1; u.addr=(uint64_t)(uintptr_t)fragment_uniform; vk_draw_exec(OP_BIND_UBO,&u);
    u.stage=0; u.index=11; u.addr=kind==8 ? 0 : (uint64_t)(uintptr_t)control; u.size=sizeof control;
    vk_draw_exec(OP_BIND_UBO,&u);
    if (!begin_rendering(1)) return 0;
    static vk_bind camera_binding={.reg=1,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    static vk_shader vertex[3]={
        {.key="e87dbf67974d71e68f4ec4586d5c70ec6fccaf7515d54280c3a765f8b7cc8c79-b7530f698e76",.nb=1,.b=&camera_binding},
        {.key="6ec214111aa5b8bc691f63fd0214aa5d4ed08e6d433f7973c9535f4a0d00f620-b7530f698e76",.nb=1,.b=&camera_binding},
        {.key="host-unlisted-plane-vertex",.nb=1,.b=&camera_binding}};
    static vk_shader fragment[2]={
        {.key="ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9",.writes_depth=1,.fout_mask=1},
        {.key="host-unlisted-plane-fragment",.writes_depth=1,.fout_mask=1}};
    vk_shader *vs=&vertex[kind==5 ? 2 : kind==1 ? 1 : 0], *fs=&fragment[kind==6];
    vs->framegen_plane=(uint8_t)vk_fg_plane_shader(vs->key,0);
    vs->framegen_uncertified_plane=(uint8_t)vk_fg_plane_shader(vs->key,2);
    fs->framegen_plane=(uint8_t)vk_fg_plane_shader(fs->key,1);
    vk_shader *sh[6]={vs,fs};
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)vs,(uintptr_t)fs,fg_extract_draw_context,sh);
    if (!fg_extract_authority_before_draw(sh) || !fg_extract_depth_spans_before_draw(sh)) return 0;
    return !FGX.invalid && FGX.pass.authority_context ?
        1u | ((FGX.pass.uncertified_context && FGX.authority_span.open &&
            FGX.authority_span.experimental) ? 2u : 0u) : 0;
}
void vk_fg_test_uncertified_draw_note(int close) {
    g_stat_draws++;
    if (close) { vk_end_rendering(); FGX.test_authority_inline=0; }
}
unsigned vk_fg_test_depth_contexts(const tex_t *scene, const tex_t *depth, const float camera[84]) {
    vk_fg_test_scene_begin(scene,depth,camera);
    vk_state saved=S; __typeof__(R) saved_render=R;
    memset(&R,0,sizeof R);
    R.w=vk_tex_width(vk_tex_get(depth),0); R.h=vk_tex_height(vk_tex_get(depth),0);
    R.dfmt=VK_FORMAT_D32_SFLOAT;
    S.rt.depth=depth; S.has_vp=1;
    S.vp[0][0]=S.vp[0][1]=0; S.vp[0][2]=R.w; S.vp[0][3]=R.h;
    float uniform[592]={0}; memcpy(uniform,camera,84*sizeof(float));
    S.ubo[0][1]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)uniform,.size=sizeof uniform};
    vk_bind bind={.reg=1,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    vk_shader vertex={.nb=1,.b=&bind};
    vk_shader *sh[6]={&vertex};
    FGX.pass.group=(int)FGX.scene[0].group; FGX.pass.root=-1;
    FGX.pass.depth_write=0; FGX.pass.span0=g_stat_draws;
    fg_dep_sources_init(&FGX.pass.reads);
    S.ds.dt=S.ds.dw=1;
    S.dr[0]=0; S.dr[1]=1;
    fg_extract_draw_context(sh,0);
    unsigned result=FGX.pass.root==0 && FGX.pass.camera_context ? 1 : 0;
    S.ds.dw=0;
    fg_extract_draw_context(sh,0);
    if (!FGX.pass.camera_context && !FGX.pass.depth_write) result|=2;
    S=saved; R=saved_render;
    return result;
}
/* Synthetic draw spans drive the production context/pass/version tracker; no
   synthetic span is submitted to Vulkan or counted as a real renderer draw. */
int vk_fg_test_scene_continuations(const tex_t *scene, const tex_t *depth,
                                   const tex_t *canvas, const float camera[84], unsigned negative) {
    float plane_camera[84]; memcpy(plane_camera,camera,sizeof plane_camera);
    plane_camera[65]=(1.0f-plane_camera[38])*0.5f; camera=plane_camera;
    vk_fg_test_scene_begin(scene,depth,camera);
    if (negative==17) FGX.scene[0].camera[65]*=1.01f;
    vk_state saved=S; __typeof__(R) saved_render=R; uint64_t saved_draws=g_stat_draws;
    memset(&R,0,sizeof R);
    R.w=vk_tex_width(vk_tex_get(depth),0); R.h=vk_tex_height(vk_tex_get(depth),0);
    R.dfmt=VK_FORMAT_D32_SFLOAT; R.dtex=vk_tex_get(depth);
    S.rt=(p_set_rt){.n=2,.color={scene,scene},.depth=depth};
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0; S.vp[0][2]=R.w; S.vp[0][3]=R.h;
    S.ds.dt=S.ds.dw=1; S.chmask.mask=UINT32_MAX;
    S.dr[0]=0; S.dr[1]=1;
    float uniform[592]={0}, fragment_uniform[592]={0}, matrices[32]={0}, material[68]={0}, control[2]={0};
    memcpy(uniform,camera,84*sizeof(float));
    memcpy(fragment_uniform,uniform,sizeof fragment_uniform);
    if (negative==12) control[1]=1;
    if (negative==19 || negative==20) control[1]=800; /* observed active displacement amplitude */
    if (negative==13) uniform[65]*=1.01f;
    if (negative==14) fragment_uniform[65]*=1.01f;
    memcpy(matrices,camera,12*sizeof(float)); matrices[15]=1;
    memcpy(matrices+16,camera+28,16*sizeof(float));
    if (negative==1) matrices[3]+=1;
    if (negative==2) material[67]=0.5f;
    S.ubo[0][1]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)uniform,.size=sizeof uniform};
    S.ubo[0][5]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)matrices,.size=sizeof matrices};
    S.ubo[0][6]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)material,.size=sizeof material};
    S.ubo[0][11]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)control,.size=sizeof control};
    S.ubo[1][1]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)fragment_uniform,.size=sizeof fragment_uniform};
    if (negative==15) S.ubo[1][1].size=256;
    vk_bind bindings[2]={{.reg=5,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                         {.reg=6,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}};
    vk_bind standard_binding={.reg=1,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    vk_shader standard={.nb=1,.b=&standard_binding}, alternate={.nb=2,.b=bindings}, fragment={0};
    vk_shader plane_vertex={.nb=1,.b=&standard_binding};
    strcpy(plane_vertex.key,"e87dbf67974d71e68f4ec4586d5c70ec6fccaf7515d54280c3a765f8b7cc8c79-b7530f698e76");
    if (negative==20)
        strcpy(plane_vertex.key,"6ec214111aa5b8bc691f63fd0214aa5d4ed08e6d433f7973c9535f4a0d00f620-b7530f698e76");
    plane_vertex.framegen_plane=(uint8_t)vk_fg_plane_shader(plane_vertex.key,0);
    if (negative==16) plane_vertex.framegen_plane=0;
    strcpy(alternate.key,"3e9fe6db781f37eb22f1acb0bcd73731cb8ffadaddd45bedc5aeb8ebb353fa18-468e580cd620");
    if (negative==6) strcpy(alternate.key,"de3ac20c93c60cf782e8c55a40b89688aead03de7d67373bd4cd90c1963a95d9-466ef310f9bb");
    if (negative==7) strcpy(alternate.key,"c5b26871948ae177db2518a0204b6f718d09b9079ebb70ff865a7b5aa8688b56-1a4d008ae5b4");
    if (negative==3 || negative==8) strcpy(alternate.key,"unclassified-camera-layout");
    alternate.framegen_camera_layout=(uint8_t)vk_fg_camera_layout(alternate.key);
    for (unsigned stage=0;stage<6;stage++) {
        R.ncolor=stage==0 ? 0 : stage==1 ? 1 : stage==5 ? 8 : 2;
        S.rt.n=R.ncolor; R.draws0=g_stat_draws;
        S.has_sc=1; S.sc=(p_scissor){0,0,(int)R.w,(int)R.h};
        if (stage==1) S.sc=(p_scissor){(int)R.w/2,(int)R.h/2,16,17};
        for (unsigned i=0;i<8;i++) {
            int bound=stage==5 ? i==1 || i==3 || i==7 : i<R.ncolor;
            R.ctex[i]=bound ? vk_tex_get(scene) : NULL;
            R.cfmt[i]=bound ? vk_tex_format(R.ctex[i]) : VK_FORMAT_UNDEFINED;
            S.rt.color[i]=bound ? scene : NULL;
        }
        fragment.fout_mask=stage==0 ? 0 : stage==1 ? 1 : stage==5 ? 0x8a : 3;
        S.ds.df=stage<=1 ? NVN_DEPTH_FUNC_ALWAYS : stage==5 ? 4 : NVN_DEPTH_FUNC_GREATER;
        S.vp[0][0]=stage==3 && negative==4 ? 1 : 0;
        S.dr[0]=stage==3 && negative==9 ? 0.25f : 0;
        fragment.writes_depth=stage==1 || stage==5 || (stage==3 && negative==5);
        strcpy(fragment.key,stage==1 ?
            "c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a" :
            stage==5 ? (negative==18 ?
            "ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-b7530f698e76" :
            "ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9") : "unknown-fragment-depth");
        fragment.framegen_raster_depth=(uint8_t)vk_fg_raster_depth_shader(fragment.key);
        fragment.framegen_plane=(uint8_t)vk_fg_plane_shader(fragment.key,1);
        fg_extract_pass_begin(0);
        vk_shader *sh[6]={stage==3 ? &alternate : stage==5 ? &plane_vertex : &standard,&fragment};
        if (stage==3 && negative==8) {
            vk_shader *accepted[6]={&standard,&fragment};
            fg_extract_draw_context(accepted,0); g_stat_draws++;
        }
        if (stage==3 && negative==10) {
            /* An actual depth-writing draw with an unaudited FragDepth export
               precedes a later camera-accepted draw in this same physical pass. */
            fragment.writes_depth=1;
            strcpy(fragment.key,"ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9");
            fragment.framegen_raster_depth=(uint8_t)vk_fg_raster_depth_shader(fragment.key);
            fragment.framegen_plane=(uint8_t)vk_fg_plane_shader(fragment.key,1);
            vk_shader *unaccepted[6]={&standard,&fragment};
            fg_extract_draw_context(unaccepted,0); g_stat_draws++;
            fragment.writes_depth=0;
            strcpy(fragment.key,"c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a");
            fragment.framegen_raster_depth=(uint8_t)vk_fg_raster_depth_shader(fragment.key);
            fragment.framegen_plane=0;
            vk_shader *accepted[6]={&standard,&fragment};
            fg_extract_draw_context(accepted,0); g_stat_draws++;
        }
        fg_extract_draw_context(sh,0); g_stat_draws++;
        if (stage==5) {
            /* A later accepted context must not erase an earlier failed plane
               proof; conversely a proven plane span must not invent a failure. */
            strcpy(fragment.key,"c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a");
            fragment.framegen_raster_depth=1; fragment.framegen_plane=0;
            vk_shader *accepted[6]={&standard,&fragment};
            fg_extract_draw_context(accepted,0); g_stat_draws++;
        }
        fg_extract_pass_end();
    }
    fg_dep_key source=fg_extract_image_key(vk_tex_get(scene),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_key destination=fg_extract_image_key(vk_tex_get(canvas),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    fg_dep_sources_add(&FGX.deps,&sources,fg_dep_read(&FGX.deps,source));
    fg_extract_publish(destination,canvas,&sources,FG_PASS_WRITER_OPERATION|++FGX.operation,0);
    fg_dep_value value=fg_dep_read(&FGX.deps,destination);
    int selected=fg_dep_select(&FGX.deps,value,NULL)==FG_DEP_SELECTED && !FGX.invalid;
    if (!selected && FGX.diag) fg_extract_refusal_resources(vk_tex_get(canvas),0,0,value);
    g_stat_draws=saved_draws; FGX.pass.span0=FGX.pass.published_draws=saved_draws;
    S=saved; R=saved_render;
    return selected ? (FGX.nscene==1 ? 1 : 2) : 0;
}
#endif
/* Retain attachment clears in order with the frozen draws, without breaking
   the real tiled pass. A full color overwrite is not a scene composition. */
static int fg_extract_ui_clear(const VkClearAttachment *attachment, const VkClearRect *rect) {
    if (!FGX.captured || FGX.invalid) return 0;
    int color = attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT;
    if (color ? attachment->colorAttachment || !fg_extract_canvas(R.ctex[0],R.clevel[0],R.clayer[0]) :
        !R.dtex || !fg_extract_ui_depth(R.dtex,R.dlevel,R.dlayer)) return 0;
    if (rect->baseArrayLayer || rect->layerCount != 1 || R.host_w != FGX.ui_width || R.host_h != FGX.ui_height ||
        (color && rect->rect.offset.x == 0 && rect->rect.offset.y == 0 &&
         rect->rect.extent.width == R.host_w && rect->rect.extent.height == R.host_h)) {
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI clear changes canvas structure"); return 0;
    }
    if (!vk_fg_canvas_view()) return 1; /* recognized warm-up operation, no private target yet */
    if (FGX.nreplay == sizeof FGX.replay_draw / sizeof FGX.replay_draw[0]) {
        fg_extract_reject(VK_FG_TRACK_CAPACITY,"UI replay packet capacity exhausted"); return 0;
    }
    fg_extract_replay_draw *packet = &FGX.replay_draw[FGX.nreplay++];
    packet->width = FGX.ui_width; packet->height = FGX.ui_height; packet->format = FGX.ui_format;
    packet->final = 0; packet->clear = 1; packet->attachment = *attachment; packet->rect = *rect;
    return 1;
}
static void fg_extract_texture_write(const tex_t *t, unsigned mip, unsigned layer, unsigned count,
                                     unsigned aspect, int clear, int partial, int replayed_ui_clear) {
    if (!FGX.nscene) return;
    vk_tex *x = vk_tex_get(t);
    if (FGX.captured && !replayed_ui_clear && (fg_extract_canvas(x,mip,layer) || fg_extract_ui_depth(x,mip,layer)))
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"unrecorded canvas/UI depth clear or copy");
    /* Publish preceding draws before versioning the preserved remainder.
       This is a logical checkpoint, never an extra Vulkan render pass. */
    if (R.open) {
        if (clear && partial) fg_extract_publish_pass();
        else fg_extract_span();
    }
    unsigned writer = FG_PASS_WRITER_OPERATION | ++FGX.operation;
    for (unsigned a = 1; a <= VK_IMAGE_ASPECT_STENCIL_BIT; a <<= 1) if (aspect & a)
        for (unsigned i = 0; i < count; i++) {
            fg_dep_key key = fg_extract_image_key(x, mip, layer + i, a);
            if (clear && !partial) fg_dep_clear(&FGX.deps, key, writer);
            else if (clear) fg_dep_partial_clear(&FGX.deps,key,writer);
            else {
                vk_canvas_image image=FGX.diag ? vk_tex_canvas_identity(x,mip,layer+i) : (vk_canvas_image){0};
                fg_extract_invalidate(key,writer,&image,"buffer-to-texture-write");
            }
            fg_extract_revoke(key,clear ? "attachment-clear" : "buffer-to-texture-write",writer);
            if (R.open) for (unsigned j = 0; j < 9; j++) if (fg_extract_same_key(key,FGX.pass.key[j])) {
                FGX.pass.old[j] = fg_dep_read(&FGX.deps,key);
                if (g_stat_draws != R.draws0 && !(clear && partial)) FGX.pass.reads.value.tainted |= FG_DEP_TAINT_CLEAR;
            }
        }
    if (R.open) fg_pass_guard_invalidate(&FGX.guard);
}
static void fg_extract_compute(vk_shader *const sh[6]) {
    if (!FGX.nscene) return;
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    fg_extract_reads(sh, 5, 6, &sources);
    fg_extract_storage_buffers(sh[5],5,&sources);
    unsigned writer = FG_PASS_WRITER_OPERATION | ++FGX.operation;
    const vk_shader *shader = sh[5];
    for (int i = 0; i < shader->nb; i++) {
        const vk_bind *b = &shader->b[i];
        if (!b->rw || b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
        uint64_t handle = b->reg < 0 ? 0 : b->kind == RB_IMG ?
            (b->reg < 32 ? S.img[5][b->reg] : 0) : (b->reg < 128 ? S.tex[5][b->reg] : 0);
        vk_treg r;
        if (!handle || !vk_treg_get((int)(handle & 0xfffff), &r)) continue;
        vk_tex *x = vk_tex_get(&r.t);
        unsigned mip = r.has_v && r.v.set_levels ? r.v.base_level : 0;
        unsigned layer = r.has_v && r.v.set_layers ? r.v.min_layer : 0;
        fg_dep_key key = fg_extract_image_key(x, mip, layer, vk_tex_aspect(x));
        if (FGX.captured && (fg_extract_canvas(x,mip,layer) || fg_extract_ui_depth(x,mip,layer)))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"compute modifies captured canvas/UI depth");
        fg_dep_sources output; fg_dep_sources_init(&output); output.value = sources.value;
        fg_dep_sources_add(&FGX.deps, &output, fg_dep_read(&FGX.deps, key));
        fg_extract_publish(key, &r.t, &output, writer, 0);
        fg_extract_revoke(key,"compute-image-write",writer);
    }
}
static void fg_extract_copy(const p_copy_t2t *c, vk_tex *s, vk_tex *d, unsigned layers) {
    if (!FGX.nscene) return;
    unsigned sl = view_level(&c->sv, c->has_sv), dl = view_level(&c->dv, c->has_dv);
    unsigned sa = (c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0) + c->sr.zoffset;
    unsigned da = (c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0) + c->dr.zoffset;
    int partial = c->dr.xoffset || c->dr.yoffset || (unsigned)c->dr.width != vk_tex_width(d, dl) ||
        (unsigned)c->dr.height != vk_tex_height(d, dl);
    unsigned writer = FG_PASS_WRITER_OPERATION | ++FGX.operation;
    for (unsigned i = 0; i < layers; i++) {
        fg_dep_key source = fg_extract_image_key(s, sl, sa + i, vk_tex_aspect(s));
        fg_dep_key destination = fg_extract_image_key(d, dl, da + i, vk_tex_aspect(d));
        if (FGX.captured && (fg_extract_canvas(d,dl,da+i) || fg_extract_ui_depth(d,dl,da+i)))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"copy modifies captured canvas/UI depth");
        fg_dep_sources reads; fg_dep_sources_init(&reads);
        fg_dep_sources_add(&FGX.deps, &reads, fg_dep_read(&FGX.deps, source));
        if (partial) fg_dep_sources_add(&FGX.deps, &reads, fg_dep_read(&FGX.deps, destination));
        if (reads.value.origins && (partial || c->sr.xoffset || c->sr.yoffset ||
            (unsigned)c->sr.width != vk_tex_width(s,sl) || (unsigned)c->sr.height != vk_tex_height(s,sl)))
            reads.value.tainted |= FG_DEP_TAINT_MAPPING;
        fg_extract_publish(destination, c->dst, &reads, writer, 0);
        fg_extract_revoke(destination,"image-copy",writer);
    }
}
static void fg_extract_replay_batch(void) {
    if (FGX.invalid || !vk_fg_canvas_view()) return;
    if (FGX.nreplay < 2 || FGX.replay_draw[FGX.nreplay - 1].final != 2) {
        fg_extract_reject(VK_FG_CHAIN_BROKEN,"no frozen terminal canvas-transfer descriptor"); return;
    }
    const fg_extract_replay_draw *first = &FGX.replay_draw[0];
    for (unsigned i = 0; i + 1 < FGX.nreplay; i++) {
        const fg_extract_replay_draw *p = &FGX.replay_draw[i];
        if (p->final || p->width != first->width || p->height != first->height || p->format != first->format) {
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI replay target changed within canvas"); return;
        }
    }
    VkCommandBuffer cb = vk_cb();
    int interpolating = vk_fg_interpolating();
    for (unsigned pass = 0; pass < (interpolating ? 2u : 1u); pass++) {
        int final = pass ? 2 : 0; /* Capture UI's affine RGB blend over black/white. */
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        if (!vk_fg_replay_begin(cb,first->width,first->height,first->format,final)) {
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"private UI canvas unavailable"); return;
        }
        for (unsigned i = 0; i + 1 < FGX.nreplay; i++) {
            const fg_extract_replay_draw *packet = &FGX.replay_draw[i];
            if (packet->clear) vkCmdClearAttachments(cb,1,&packet->attachment,1,&packet->rect);
            else vk_fg_replay_draw(cb,&packet->draw,packet->sets);
        }
        vk_fg_replay_end(cb,final);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    int terminal_capture=vk_fg_adaptive();
#ifdef TOTK_VK_TESTS
    /* The owned Vulkan fixture substitutes only unavailable AFME/GL work;
     * production binding/extraction still supplies this exact terminal. */
    terminal_capture=terminal_capture || vk_fg_test_generation_bridge_active();
#endif
    if (terminal_capture) {
        const fg_extract_replay_draw *last=&FGX.replay_draw[FGX.nreplay-1];
        vk_fg_adaptive_terminal(&last->draw,last->sets,last->width,last->height,last->format);
    }
    /* Warmup/refusal keeps today's UI capture for the next adjacent pair.
       A midpoint uses retained older UI, never the newer frame's live HUD. */
    if (!interpolating || vk_fg_interp_compose(cb)) {
        if (vk_fg_qcom_requested() || vk_fg_lsfg_requested()) cb=vk_cb(); /* External handoff can seal the primary. */
        const fg_extract_replay_draw *last = &FGX.replay_draw[FGX.nreplay - 1];
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        if (!vk_fg_replay_begin(cb,last->width,last->height,last->format,1)) {
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"private terminal image unavailable"); return;
        }
        vk_fg_replay_draw(cb,&last->draw,last->sets);
        vk_fg_replay_end(cb,1);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = g_pipe_dirty = 1;
    descriptor_unbind(); vk_stage_sig_invalidate(0x3fu);
}
void vk_fg_extract_present(const tex_t *t) {
    if (!vk_fg_extract_active || !t) return;
    FGX.probe_started = 1;
    vk_end_rendering_at(VK_PASS_END_PRESENT, 0);
    const vk_canvas_frame *chain = vk_canvas_current();
    vk_tex *window = vk_tex_get(t);
    vk_canvas_image identity = vk_tex_canvas_identity(window, 0, 0);
    int presented = vk_canvas_present(&identity);
    if (!FGX.captured) fg_extract_reject(VK_FG_NO_BOUNDARY,"scene/canvas/UI boundary not captured");
    else if (!chain->ui_committed || !chain->transfer_committed || !presented || !FGX.transfer)
        fg_extract_reject(VK_FG_CHAIN_BROKEN,"canvas/final-transfer/present chain broken");
    else if (!FGX.scene[FGX.selected].valid) fg_extract_reject(VK_FG_DEPTH_LOST,"scene depth overwritten before present");
    else if (FGX.deps.failed || FGX.passes.failed) fg_extract_reject(VK_FG_TRACK_CAPACITY,"tracking capacity exhausted");
    if (!FGX.invalid && vk_fg_canvas_view() && vk_native_ui_enabled() && vk_tex_native_active(window)) {
        const fg_extract_replay_draw *last = FGX.nreplay ? &FGX.replay_draw[FGX.nreplay-1] : NULL;
        vk_post_output native={0};
        int native_ready=vk_tex_native_output(window,&native);
        if (!last || last->final != 2 || !last->native ||
            !native_ready || native.image != last->image ||
            native.w != last->width || native.h != last->height ||
            vk_fg_raw_format(native.format) != vk_fg_raw_format(last->format)) {
            LOGV("[framegen-extract] native terminal mismatch: frozen image=%p native=%u final=%u extent=%ux%u format=%u; presented ready=%d image=%p extent=%ux%u format=%u",
                last ? (void *)last->image : NULL,last ? last->native : 0,last ? last->final : 0,
                last ? last->width : 0,last ? last->height : 0,last ? last->format : 0,
                native_ready,(void *)native.image,native.w,native.h,native.format);
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"native terminal attachment/extent/encoding does not match presented sibling");
        }
        else if (vk_fg_interpolating()) {
            VkCommandBuffer cb = vk_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
            int captured = vk_fg_capture_real(cb,window,0,0);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            if (!captured)
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"native presented real image could not be retained exactly");
        }
    } else if (!FGX.invalid && vk_fg_interpolating()) {
        VkCommandBuffer cb = vk_cb();
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        (void)vk_fg_capture_real(cb,window,0,0); /* masked pairs require this; known pairs need no fallback */
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    if (!FGX.invalid) fg_extract_replay_batch();
    FGX.eligible = !FGX.invalid;
    const char *reason = FGX.invalid ? FGX.reason : "eligible scene -> canvas -> final transfer -> present";
    if (!FGX.last_reason || strcmp(reason, FGX.last_reason)) {
        LOGV("[framegen-extract] real %llu %s", (unsigned long long)FGX.frame, reason);
        FGX.last_reason = reason;
    }
}
void vk_fg_extract_end(void) {
    if (!FGX.requested) return;
    if (FGX.writer.open) {
        vk_fg_writer_poison("extractor frame ended before physical writer closure");
        FGX.writer.open=0;
    }
    if (FGX.authority_span.open) {
        FGX.authority_span.open=0;
        fg_extract_reject(VK_FG_DEPTH_LOST,"ordered depth authority span left open at frame end");
    }
    int active=FGX.probe_started || vk_fg_extract_active, enabled=vk_fg_capturing();
    if (FGX.diag && FGX.cpu_start) {
        uint64_t end=fg_extract_cpu_ns();
        if (end >= FGX.cpu_start) {
            uint64_t elapsed=end-FGX.cpu_start; unsigned index=active != 0;
            FGX.diag_window.cpu_ns[index] += elapsed; FGX.diag_window.cpu_frames[index]++;
            if (elapsed > FGX.diag_window.cpu_max[index]) FGX.diag_window.cpu_max[index]=elapsed;
        }
    }
    if (active) {
        FGX.diag_window.physical=FGX.passes.physical_passes; FGX.diag_window.groups=FGX.passes.count;
        FGX.diag_window.contexts=FGX.passes.draw_events; FGX.diag_window.missed=FGX.passes.incomplete_contexts;
    }
    const vk_canvas_frame *chain=vk_canvas_current();
    FGX.diag_window.probes += active;
    FGX.diag_window.backoff += !active && enabled;
    FGX.diag_window.transfers += chain->transfer_committed != 0;
    FGX.diag_window.nominations += chain->first_ui != 0;
    FGX.diag_window.attempts += FGX.ui_seen != 0;
    FGX.diag_window.snapshots += FGX.snapshot_ok != 0;
    FGX.diag_window.roots += FGX.nscene;
    if (chain->canvas.image) FGX.diag_window.canvas=chain->canvas;
    if (!FGX.eligible) vk_fg_skip();
    unsigned reason = !active ? (enabled ? VK_FG_BACKOFF : VK_FG_SUSPENDED) :
        FGX.eligible ? VK_FG_ELIGIBLE : FGX.invalid ? FGX.refusal : VK_FG_NO_BOUNDARY;
    vk_fg_track_result(reason);
    if (enabled) fg_pass_probe_end(&FGX.probe,FGX.eligible,reason == VK_FG_TRACK_CAPACITY);
    if (FGX.diag && FGX.plane_coverage.draws)
        LOGV("[framegen-plane-coverage] real %llu depth-draws=%llu accepted=%llu scissor-sum-pixels=%llu viewport-sum-pixels=%llu max-scissor-fraction=%.6f (tracked probe prefix; overlapping upper bounds, NOT raster coverage)",
            (unsigned long long)FGX.frame,(unsigned long long)FGX.plane_coverage.draws,
            (unsigned long long)FGX.plane_coverage.accepted,(unsigned long long)FGX.plane_coverage.scissor_pixels,
            (unsigned long long)FGX.plane_coverage.viewport_pixels,FGX.plane_coverage.max_scissor_fraction);
    if (++FGX.frame % 300 == 0 && FGX.diag) {
        const vk_canvas_image *c=&FGX.diag_window.canvas;
        const fg_dep_value *v=&FGX.diag_window.value;
        LOGV("[framegen-diag] canvas last300 probes=%u backoff=%u id=%#llx incarnation=%llu size=%ux%u format=%u transfer-seen=%u first-ui=%u snapshot-attempt=%u snapshot-taken=%u roots=%u",
            FGX.diag_window.probes,FGX.diag_window.backoff,(unsigned long long)c->image,
            (unsigned long long)c->incarnation,c->width,c->height,c->format,FGX.diag_window.transfers,
            FGX.diag_window.nominations,FGX.diag_window.attempts,FGX.diag_window.snapshots,FGX.diag_window.roots);
        LOGV("[framegen-diag] canvas dependency last-attempt result=%s roots=%u known=%u version=%llu origins=%#llx eligible=%#llx unresolved=%#llx tainted=%#x; last300 dummy=%u missing=%u bounds=%u storage=%u image-writes=%u writes=%u",
            FGX.diag_window.attempts ? fg_dep_reason(FGX.diag_window.result) : "not attempted",FGX.diag_window.root_count,v->known,
            (unsigned long long)v->version,(unsigned long long)v->origins,(unsigned long long)v->eligible,
            (unsigned long long)v->unresolved,v->tainted,FGX.diag_window.dummy,FGX.diag_window.missing,
            FGX.diag_window.bounds,FGX.diag_window.storage,FGX.diag_window.image_writes,FGX.diag_window.writes);
        LOGV("[framegen-diag] scene-signature %#llx stable-frames %llu physical %u groups %u contexts %u contexts-missed %u",
            (unsigned long long)FGX.signature,(unsigned long long)FGX.stable_frames,
            FGX.diag_window.physical,FGX.diag_window.groups,FGX.diag_window.contexts,FGX.diag_window.missed);
        for (unsigned i=0;i<3;i++)
            LOGV("[framegen-binding] last 300 real class=%s contexts=%llu reflected=%llu view-work=%llu old-view-work=%llu overflow=%u (attempted work; fixed budget=%u)",
                i == 2 ? "compute" : i == 1 ? "raster-depth" : "raster-no-depth",
                (unsigned long long)FGX.diag_window.binding_calls[i],
                (unsigned long long)FGX.diag_window.binding_reflected[i],
                (unsigned long long)FGX.diag_window.binding_view[i],
                (unsigned long long)FGX.diag_window.binding_old_view[i],
                FGX.diag_window.binding_overflows[i],FG_PASS_MAX_BINDINGS);
        LOGV("[framegen-binding] last 300 real busiest-frame=%llu group=%d shape=%#llx extent=%ux%u reserved-work=%u; binding-walk thread CPU total/max-context=%.3f/%.3fms (not whole tracker; DIAG clock overhead included)",
            (unsigned long long)FGX.diag_window.binding_top_frame,FGX.diag_window.binding_top_group,
            (unsigned long long)FGX.diag_window.binding_top_signature,
            FGX.diag_window.binding_top_width,FGX.diag_window.binding_top_height,FGX.diag_window.binding_top_work,
            FGX.diag_window.binding_cpu_ns/1e6,FGX.diag_window.binding_cpu_max/1e6);
        LOGV("[framegen-diag] first-pass-to-end thread CPU (whole walking window, NOT tracking alone): probes n=%u mean/max=%.3f/%.3fms backoff n=%u mean/max=%.3f/%.3fms",
            FGX.diag_window.cpu_frames[1],FGX.diag_window.cpu_frames[1] ?
            FGX.diag_window.cpu_ns[1]/(1e6*FGX.diag_window.cpu_frames[1]) : 0,FGX.diag_window.cpu_max[1]/1e6,
            FGX.diag_window.cpu_frames[0],FGX.diag_window.cpu_frames[0] ?
            FGX.diag_window.cpu_ns[0]/(1e6*FGX.diag_window.cpu_frames[0]) : 0,FGX.diag_window.cpu_max[0]/1e6);
        vk_canvas_image last=FGX.diag_window.canvas;
        memset(&FGX.diag_window,0,sizeof FGX.diag_window);
        FGX.diag_window.canvas=last; FGX.diag_window.result=FG_DEP_NO_ORIGIN;
    }
    FGX.nscene = FGX.nwatch = FGX.operation = 0;
    FGX.nunknown = FGX.unknown_overflow = 0;
    FGX.captured = FGX.invalid = FGX.ui_seen = FGX.transfer = FGX.eligible = FGX.nreplay = FGX.snapshot_ok = FGX.probe_started = 0;
    FGX.authority_ready=FGX.authority_gpu_ready=0; FGX.authority_key=(fg_dep_key){0};
    FGX.reason = NULL;
    FGX.canvas_key = FGX.ui_depth_key = (fg_dep_key){0};
    FGX.ui_depth_address = FGX.ui_depth_size = 0;
    FGX.cpu_start=0;
    memset(&FGX.plane_coverage,0,sizeof FGX.plane_coverage);
    if (FGX.diag) memset(FGX.binding_group_work,0,sizeof FGX.binding_group_work);
    if (active) { fg_dep_reset(&FGX.deps); fg_pass_reset(&FGX.passes); }
    fg_extract_refresh(0);
}
void vk_fg_present_begin(const tex_t *t) {
    if (fg_extract_requested_config()) vk_fg_extract_present(t);
}
void vk_fg_present_end(uint64_t fence) {
    if (!fg_extract_requested_config()) return;
    vk_fg_extract_end(); vk_fg_frame_end(fence); vk_fg_stats();
}
#ifdef TOTK_VK_TESTS
void vk_fg_test_present_begin(const tex_t *t) { vk_fg_present_begin(t); }
void vk_fg_test_present_end(uint64_t fence) { vk_fg_present_end(fence); }
void vk_fg_test_lifecycle(uint64_t facts[8]) {
    facts[0]=(uint64_t)fg_extract_requested_config(); facts[1]=(uint64_t)vk_fg_extract_active;
    facts[2]=FGX.frame; facts[3]=FGX.snapshot_ok; facts[4]=FGX.eligible;
    vk_fg_test_source_lifecycle(facts+5);
}
#endif
static int scissor_full(void) {
    if (!S.has_sc) return 1;
    uint32_t w = 0, h = 0; const tex_t *t = S.has_rt && S.rt.n && S.rt.color[0] ? S.rt.color[0] : S.has_rt ? S.rt.depth : NULL;
    if (t) { w = t->d.width; h = t->d.height; }
    return S.sc.x <= 0 && S.sc.y <= 0 && (uint32_t)(S.sc.x + S.sc.w) >= w && (uint32_t)(S.sc.y + S.sc.h) >= h;
}
static VkRect2D native_rect(VkRect2D r) {
    if (!R.native) return r;
    int64_t x0 = r.offset.x, y0 = r.offset.y, x1 = x0 + r.extent.width, y1 = y0 + r.extent.height;
    x0 = MINV((x0 * R.host_w + R.native_w/2) / R.native_w,R.host_w); x1 = MINV((x1 * R.host_w + R.native_w/2) / R.native_w,R.host_w);
    y0 = MINV((y0 * R.host_h + R.native_h/2) / R.native_h,R.host_h); y1 = MINV((y1 * R.host_h + R.native_h/2) / R.native_h,R.host_h);
    return (VkRect2D){{(int32_t)x0,(int32_t)y0},{(uint32_t)(x1-x0),(uint32_t)(y1-y0)}};
}
static VkRect2D clear_rect(void) {
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)R.w, y1 = (int32_t)R.h;
    if (S.has_sc) { x0 = MAXV(x0, S.sc.x); y0 = MAXV(y0, S.sc.y); x1 = MINV(x1, S.sc.x + S.sc.w); y1 = MINV(y1, S.sc.y + S.sc.h); }
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    return native_rect((VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}});
}
static int clear_full(void) {   /* same render area as vkCmdClearAttachments, including mip views and unequal targets */
    if (!R.open) return scissor_full();
    VkRect2D r = clear_rect();
    return !r.offset.x && !r.offset.y && r.extent.width == R.host_w && r.extent.height == R.host_h;
}
static VkClearValue color_value(int fmt_class, const float rgba[4]) {
    VkClearValue v;
    if (fmt_class == 1) for (int i = 0; i < 4; i++) v.color.uint32[i] = (uint32_t)rgba[i];
    else if (fmt_class == 2) for (int i = 0; i < 4; i++) v.color.int32[i] = (int32_t)rgba[i];
    else memcpy(v.color.float32, rgba, 16);
    return v;
}
static void clear_color(const p_clear_color *c) {
    if (S.render_disabled) { g_pops.clears_skipped++; return; }
    g_pops.clears++; memcpy(g_pops.cc, c->rgba, 16);
    if (c->index < 0 || c->index >= S.rt.n || c->index >= 8 || !S.rt.color[c->index]) { RED(4, "colour clear of target %d, which is not set", c->index); return; }
    if (!(c->mask & 0xf)) return;
    int f = S.rt.has_cview[c->index] && S.rt.cview[c->index].set_format ? S.rt.cview[c->index].format : S.rt.color[c->index]->d.format;
    VkClearValue v = color_value(vk_fmt_class(f), c->rgba);
    int full = clear_full();
    if (R.open && (c->mask & 0xf) == 0xf && full && clear_load_on()) { vk_end_rendering_at(VK_PASS_END_CLEAR, 1u << c->index); g_clear_load_c++; }
    if (!R.open && (c->mask & 0xf) == 0xf && full) { R.clear_c[c->index] = 1; R.cc[c->index] = v; return; }
    if ((c->mask & 0xf) != 0xf) RED(4, "colour clear with channel mask %#x: cleared whole (masked clears not done yet)", c->mask);
    g_pops.clears_inpass++;
    begin_rendering(0);
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, v};
    VkClearRect cr = {clear_rect(), 0, 1};
    if (cr.rect.extent.width && cr.rect.extent.height) {
        if (R.secondary) vk_record_clear(&ca, &cr); else vkCmdClearAttachments(vk_cb(), 1, &ca, 1, &cr);
        if (pso_life_active() && full && (c->mask & 15) == 15) PL.color_clear |= 1u << c->index;
        pso_life_view_write(R.ctex[c->index], PL.color_view[c->index], VK_IMAGE_ASPECT_COLOR_BIT,
            full && (c->mask & 15) == 15, 0, "inline-clear");
        if (vk_fg_extract_active) fg_extract_texture_write(S.rt.color[c->index], R.clevel[c->index],
            R.clayer[c->index], 1, VK_IMAGE_ASPECT_COLOR_BIT, 1, !full, fg_extract_ui_clear(&ca,&cr));
    }
}
static void clear_ds(const p_clear_ds *c) {
    if (S.render_disabled || !S.has_rt || !S.rt.depth) return;
    int sm = c->stencil_mask & 255; if (!c->depth_mask && !sm) return;
    int fmt = S.rt.depth->d.format;
    VkImageAspectFlags as = (c->depth_mask && vk_fmt_has_depth(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (sm && vk_fmt_has_stencil(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!as) return;
    if ((as&VK_IMAGE_ASPECT_DEPTH_BIT) && FGX.writer.open)
        vk_end_rendering_at(VK_PASS_END_CLEAR,(uint32_t)as); /* close BEFORE the actual depth clear */
    int full = clear_full();
    if (R.open && (!sm || sm == 255) && full && clear_load_on()) { vk_end_rendering_at(VK_PASS_END_CLEAR, (uint32_t)as); g_clear_load_ds++; }
    if (!R.open && (!sm || sm == 255) && full) {
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { R.clear_d = 1; R.cd = c->depth; }
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
        return;
    }
    if (sm && sm != 255) RED(4, "stencil clear with mask %#x: cleared whole", sm);
    begin_rendering(0);
    VkClearAttachment ca = {as, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect cr = {clear_rect(), 0, 1};
    if (cr.rect.extent.width && cr.rect.extent.height) {
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) g_clear_inline_d++;
        if (R.secondary) vk_record_clear(&ca, &cr); else vkCmdClearAttachments(vk_cb(), 1, &ca, 1, &cr);
        if (pso_life_active() && full && (!sm || sm == 255)) PL.depth_clear |= as;
        for (unsigned bit = VK_IMAGE_ASPECT_DEPTH_BIT; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
            if (as & bit) pso_life_view_write(R.dtex, bit == VK_IMAGE_ASPECT_DEPTH_BIT ? PL.depth_view : PL.stencil_view,
                bit, full && (!sm || sm == 255), 0, "inline-clear");
        if (vk_fg_extract_active) fg_extract_texture_write(S.rt.depth,R.dlevel,R.dlayer,1,as,1,!full,fg_extract_ui_clear(&ca,&cr));
    }
}

/* ---------- graphics pipelines ---------- */
typedef struct {
    void *vs, *fs;
    VkFormat cfmt[8], dfmt, sfmt; uint32_t ncolor, samples;
    blend_st blend[8]; uint32_t blend_en, chmask; uint8_t a2c, poly_mode, topo_class, depth_clamp, logic_op, pad[3];
    struct { uint32_t fmt, offset, stream; } attr[16];
    struct { uint32_t instanced, divisor; } stream[16];
} pipe_key;
static pthread_once_t g_pso_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_pso_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_pso_trace_enabled;
static void pso_trace_init(void) {
    const char *value = getenv("TOTK_PSO_TRACE");
    g_pso_trace_enabled = value && !strcmp(value, "1");
}
static int pso_trace_on(void) {
    pthread_once(&g_pso_trace_once, pso_trace_init);
    return g_pso_trace_enabled;
}
static TotkPsoTraceKey pso_trace_key(const pipe_key *k) {
    TotkPsoTraceKey key = {0};
    const vk_shader *vs = k->vs, *fs = k->fs;
    key.vs_id = vs ? vs->key : NULL; key.fs_id = fs ? fs->key : NULL;
    key.fs_early_tests = fs ? (uint32_t)fs->early_tests | fs->alpha_func<<1 : 0;
    for (unsigned i = 0; i < 8; i++) key.color[i] = k->cfmt[i];
    key.depth = k->dfmt; key.stencil = k->sfmt; key.color_count = k->ncolor; key.samples = k->samples;
    for (unsigned i = 0; i < 8; i++) {
        key.blend[i][0] = (uint32_t)k->blend[i].target;
        key.blend[i][1] = (uint32_t)k->blend[i].src_rgb;
        key.blend[i][2] = (uint32_t)k->blend[i].dst_rgb;
        key.blend[i][3] = (uint32_t)k->blend[i].src_a;
        key.blend[i][4] = (uint32_t)k->blend[i].dst_a;
        key.blend[i][5] = (uint32_t)k->blend[i].eq_rgb;
        key.blend[i][6] = (uint32_t)k->blend[i].eq_a;
    }
    key.blend_enabled = k->blend_en; key.channel_mask = k->chmask; key.a2c = k->a2c;
    key.polygon_mode = k->poly_mode; key.topology_class = k->topo_class;
    key.depth_clamp = k->depth_clamp; key.logic_op = k->logic_op;
    for (unsigned i = 0; i < 16; i++) {
        key.attributes[i][0] = k->attr[i].fmt; key.attributes[i][1] = k->attr[i].offset; key.attributes[i][2] = k->attr[i].stream;
        key.streams[i][0] = k->stream[i].instanced; key.streams[i][1] = k->stream[i].divisor;
    }
    return key;
}
static void pso_trace_log(const pipe_key *k, const char *phase, const char *reason, uint64_t frame,
                          VkResult result, uint64_t elapsed_ns, VkPipelineCreateFlags flags, int part) {
    if (!pso_trace_on()) return;
    TotkPsoTraceKey key = pso_trace_key(k);
    uint64_t id = totk_pso_trace_key_hash(&key);
    const vk_shader *vs = k->vs, *fs = k->fs;
    pthread_mutex_lock(&g_pso_trace_mu);
    LOGV("[pso-trace] key=%016llx phase=%s reason=%s frame=%llu result=%d ms=%.3f flags=%x part=%d vs=%s fs=%s early=%u",
         (unsigned long long)id, phase, reason, (unsigned long long)frame, result, elapsed_ns / 1e6,
         (unsigned)flags, part, vs ? vs->key : "-", fs ? fs->key : "-", fs ? (unsigned)fs->early_tests : 0);
    pthread_mutex_unlock(&g_pso_trace_mu);
}
/* Recording-owner memo: target changes, not shader changes, determine the screen extent class.
   Runtime resolution configuration is immutable after env loading; keep parsing out of the draw hot path. */
static int pipe_screen_class(void) {
    static unsigned configured_w, configured_h, last_w = UINT32_MAX, last_h;
    static int initialized, last_class;
    if (last_w == R.w && last_h == R.h) return last_class;
    if (!initialized) {
        const char *value = getenv("TOTK_RENDER_RES"); unsigned w, h; char tail;
        if (value && sscanf(value, "%ux%u%c", &w, &h, &tail) == 2 &&
            w >= 64 && h >= 64 && w <= 4096 && h <= 4096) {
            configured_w = w; configured_h = h;
        }
        initialized = 1;
    }
    last_w = R.w; last_h = R.h;
    last_class = totk_screen_classify(R.w, R.h, configured_w, configured_h);
    return last_class;
}
/* Only called by the recording owner. Workers must never inspect mutable R/S to describe their origin.
   Cold-demand rows associate later worker trace records with actual attachment identity without per-entry copies. */
static void pso_trace_target_log(const pipe_key *k, const char *phase, uint64_t frame, VkResult result, uint64_t elapsed_ns) {
    if (!pso_trace_on()) return;
    TotkPsoTraceKey key = pso_trace_key(k);
    uint64_t id = totk_pso_trace_key_hash(&key);
    int cls = pipe_screen_class();
    int64_t delta = (int64_t)R.w * 9 - (int64_t)R.h * 16;
    const char *reason = !R.ncolor ? "depth-only" : cls == 1 ? "screen-exact" :
        cls == 8 ? "screen-aligned8" : cls == 16 ? "screen-aligned16" : cls == 32 ? "screen-aligned32" : "capture-guard";
    pthread_mutex_lock(&g_pso_trace_mu);
    LOGV("[pso-target] key=%016llx phase=%s frame=%llu sub=%llu pass_cursor=%llu open=%d result=%d ms=%.3f "
         "w=%u h=%u class=%s old_screen=%d delta=%lld colors=%u samples=%u "
         "cfmt=%u,%u,%u,%u,%u,%u,%u,%u dfmt=%u sfmt=%u depth_serial=%llu depth_mem=%p depth_mip=%u depth_layer=%u",
         (unsigned long long)id, phase, (unsigned long long)frame, (unsigned long long)vk_game_submit_serial(),
         (unsigned long long)g_stat_passes, R.open, result, elapsed_ns / 1e6, R.w, R.h, reason,
         delta > -32 && delta < 32, (long long)delta, R.ncolor, R.samples,
         k->cfmt[0], k->cfmt[1], k->cfmt[2], k->cfmt[3], k->cfmt[4], k->cfmt[5], k->cfmt[6], k->cfmt[7],
         k->dfmt, k->sfmt, (unsigned long long)(R.daddr ? R.d_serial : 0), R.daddr,
         R.daddr ? R.dlevel : 0, R.daddr ? R.dlayer : 0);
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.caddr[i])
        LOGV("[pso-target-color] key=%016llx phase=%s pass_cursor=%llu index=%u serial=%llu mem=%p fmt=%u mip=%u layer=%u",
             (unsigned long long)id, phase, (unsigned long long)g_stat_passes, i,
             (unsigned long long)R.c_serial[i], R.caddr[i], k->cfmt[i], R.clevel[i], R.clayer[i]);
    pthread_mutex_unlock(&g_pso_trace_mu);
}
/* g_pipes: pipe_key -> pipe_ent. A pipeline the pipeline cache holds is made at its first draw; one the driver has to
   compile (VK_PIPELINE_COMPILE_REQUIRED: on Turnip 70-150 ms for each new shader, freezes of up to 3 s on a first visit)
   goes to TOTK_VK_COMPILE_THREADS worker threads (default 2; 0 compiles at the draw, as before) and its draws are skipped
   until it is ready, so new objects show a few frames late instead (offscreen captures excepted, see pipe_get). Only the
   recording thread (under vk_rec_lock) uses the map; a worker publishes through the entry's state. */
enum { PE_QUEUED, PE_BUILDING, PE_READY, PE_FAILED };
typedef struct pipe_ent {
    pipe_key k; VkPipelineLayout layout; VkPipeline p; _Atomic int state;
    unsigned failures;                 /* published before PE_FAILED/PE_READY, read with acquire */
    VkPipeline fast;                   /* walking-thread fallback, retained for in-flight command buffers */
    uint64_t optimized_ns;             /* published with PE_READY */
    int fast_attempted, replaced;      /* walking thread only */
    uint64_t wait_sub, retry_at; unsigned skipped, seen_failure;   /* walking thread only */
    uint64_t frame;                    /* originating request; immutable while queued/building */
    int predicted, capture; uint32_t list_index, collected; struct pipe_ent *next;
    vk_lend lend;                      /* g_pq_mu: the worker building it and any nice a waiter lent it */
} pipe_ent;
#define PIPE_FAILED ((VkPipeline)(uintptr_t)1)
static nvn_hmap g_pipes = {.klen = sizeof(pipe_key)};
/* Separate immutable subset caches. The mutex protects map/publication only, never driver compilation.
   Missing subsets compile on workers; the walker only links already-ready parts (no shader compilation). */
enum { PL_VERTEX_INPUT, PL_PRE_RASTER, PL_FRAGMENT, PL_OUTPUT, PL_COUNT };
typedef struct { pipe_key k; VkPipelineLayout layout; } pipe_library_key;
typedef struct { VkPipeline p; int building, failed; } pipe_library;
static nvn_hmap g_libraries[PL_COUNT] = {
    {.klen = sizeof(pipe_library_key)}, {.klen = sizeof(pipe_library_key)},
    {.klen = sizeof(pipe_library_key)}, {.klen = sizeof(pipe_library_key)}};
static pthread_mutex_t g_library_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_library_cv = PTHREAD_COND_INITIALIZER;
static struct {
    _Atomic uint64_t fast_links, fast_link_ns, fast_link_max_ns, fast_draws;
    _Atomic uint64_t fast_link_window_max_ns;
    _Atomic uint64_t optimized_replacements, optimized_ns, library_skips;
    _Atomic uint64_t parts[PL_COUNT], part_ns[PL_COUNT];
} g_library_stats;
static int g_draw_fast;                /* reset by pipe_get; actual issued draws counted below */
/* Android prediction is opt-in: keep the shipped compiler/draw/pacing defaults unchanged.
   Only actual synchronous graphics driver builds earn adaptive credit, never worker/cache waits. */
static nvn_scan_budget g_pp_budget;
static uint64_t g_pp_frame = 1, g_pp_submit, g_pp_pass;
static unsigned g_pp_scans_in_sub;
static int g_pp_more;
static _Atomic uint64_t g_pp_build_ns, g_pp_wait_ns, g_pp_scan_ns;
static int pipe_predict_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PSO_PREDICT"); on = e && atoi(e) != 0; } return on;
}
static int pipe_predict_adapt(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PSO_SCAN_ADAPT"); on = !e || atoi(e) != 0; } return on;
}
static void pipe_diag_add(_Atomic uint64_t *counter, uint64_t ns) {
    /* One recording-thread writer; readers may be on the guest presenting thread. */
    atomic_store_explicit(counter, nvn_scan_budget_add(atomic_load_explicit(counter, memory_order_relaxed), ns), memory_order_relaxed);
}
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) {
    *lib_ns = 0; *pso_ns = dm_wait_stats_enabled() ?
        nvn_scan_budget_add(atomic_load_explicit(&g_pp_build_ns, memory_order_relaxed), vk_compute_pipe_build_ns()) : 0;
}
void mtl_wait_stats(uint64_t *lib_ns, uint64_t *pso_ns) {
    *lib_ns = 0; *pso_ns = dm_wait_stats_enabled() ? atomic_load_explicit(&g_pp_wait_ns, memory_order_relaxed) : 0;
}
void mtl_scan_stats(uint64_t *shader_ns, uint64_t *pso_ns) {
    *shader_ns = 0; *pso_ns = dm_wait_stats_enabled() ? atomic_load_explicit(&g_pp_scan_ns, memory_order_relaxed) : 0;
}
#ifdef TOTK_VK_TESTS
static vk_pipeline_test_stats g_pp_test;
void vk_pipeline_test_stats_get(vk_pipeline_test_stats *out) {
    *out = g_pp_test; out->frame = g_pp_budget.frame; out->used_ns = g_pp_budget.used_ns; out->sync_ns = g_pp_budget.sync_ns;
    out->scans_in_sub = g_pp_scans_in_sub;
    out->fast_links = atomic_load(&g_library_stats.fast_links);
    out->fast_draws = atomic_load(&g_library_stats.fast_draws);
    out->fast_link_ns = atomic_load(&g_library_stats.fast_link_ns);
    out->fast_link_max_ns = atomic_load(&g_library_stats.fast_link_max_ns);
    out->optimized_replacements = atomic_load(&g_library_stats.optimized_replacements);
    out->optimized_ns = atomic_load(&g_library_stats.optimized_ns);
    out->library_skips = atomic_load(&g_library_stats.library_skips);
    for (int i = 0; i < PL_COUNT; i++) {
        out->parts[i] = atomic_load(&g_library_stats.parts[i]);
        out->part_ns[i] = atomic_load(&g_library_stats.part_ns[i]);
    }
}
#endif
static void pipe_predict_maybe(const uint8_t *next);
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pipe_ent *g_pq_head, *g_pq_tail;
static _Atomic uint64_t g_win_async, g_win_async_ns;   /* compiled on worker threads, per stutter window */
/* Priority lending (TOTK_VK_PIPE_BOOST=1, default off). Workers run at VK_WORKER_NICE so background builds yield to the
   game. Once a recording thread joins a worker's in-flight build, that build is on the frame's critical path: the
   waiter lends the worker its own nice until the build is published, then the worker returns to VK_WORKER_NICE.
   Joins and their wall time are counted with the switch off too, so both arms report the same line.
   Live (runtime/live_ctl.h): each join reads the current value; a loan already made is still restored. */
static int g_pipe_boost;   /* the launch value */
static totk_live_cache g_pipe_boost_live;
static int pipe_boost_on(void) { return totk_live_cached(&g_pipe_boost_live, "TOTK_VK_PIPE_BOOST", g_pipe_boost); }
static _Atomic uint64_t g_boost_joins, g_boost_join_ns, g_boost_raised, g_boost_refused;
static _Atomic int g_boost_refusal_logged;
#ifdef TOTK_VK_TESTS
static int (*g_boost_test_nice)(void), (*g_boost_test_set)(int tid, int nice);
void vk_pipe_boost_test_hooks(int (*self_nice)(void), int (*set)(int tid, int nice)) {
    g_boost_test_nice = self_nice; g_boost_test_set = set;
}
void vk_pipe_boost_test_stats(uint64_t out[4]) {
    out[0] = atomic_load(&g_boost_joins); out[1] = atomic_load(&g_boost_join_ns);
    out[2] = atomic_load(&g_boost_raised); out[3] = atomic_load(&g_boost_refused);
}
#endif
static int boost_self_nice(void) {
#ifdef TOTK_VK_TESTS
    if (g_boost_test_nice) return g_boost_test_nice();
#endif
    int saved = errno; errno = 0;
    int nice = getpriority(PRIO_PROCESS, 0);   /* Linux nice is per thread */
    if (nice == -1 && errno) nice = VK_WORKER_NICE;
    errno = saved; return nice;
}
static int boost_set(int tid, int nice) {   /* 0 or the errno; tid 0 is the calling thread */
#ifdef TOTK_VK_TESTS
    if (g_boost_test_set) return g_boost_test_set(tid, nice);
#endif
    int saved = errno, error = setpriority(PRIO_PROCESS, (id_t)tid, nice) ? errno : 0;
    errno = saved; return error;
}
int vk_thread_tid(void) { return (int)syscall(SYS_gettid); }
int vk_pipe_boost_lend(vk_lend *l) {
    if (!l->tid || !pipe_boost_on()) return 0;
    int nice = boost_self_nice();
    if (nice >= (l->lent ? l->nice : VK_WORKER_NICE)) return 0;
    int error = boost_set(l->tid, nice);
    if (error) {
        atomic_fetch_add_explicit(&g_boost_refused, 1, memory_order_relaxed);
        if (!atomic_exchange_explicit(&g_boost_refusal_logged, 1, memory_order_relaxed))
            LOGV("pipe boost: raising worker %d to nice %d refused (%s); joins keep the worker's nice", l->tid, nice, strerror(error));
        return -1;
    }
    l->lent = 1; l->nice = nice;
    atomic_fetch_add_explicit(&g_boost_raised, 1, memory_order_relaxed);
    return 1;
}
void vk_pipe_boost_restore(const vk_lend *l) { if (l->lent) boost_set(0, VK_WORKER_NICE); }
void vk_pipe_boost_waited(uint64_t ns) {
    atomic_fetch_add_explicit(&g_boost_joins, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_boost_join_ns, ns, memory_order_relaxed);
}
/* TOTK_VK_UNWRITTEN_MASK=1 (default off): colour attachments the fragment shader has no output for keep their contents.
   NVN/Maxwell leaves such a bound target untouched; Vulkan leaves it undefined and Adreno writes black. The write mask
   derives from the key's fragment shader alone, so equal keys still build equal pipelines and the PSO key lists stay
   valid in both arms; only the output-interface library key carries the effective mask.
   Live (runtime/live_ctl.h): pipelines and output libraries built after a change use the new value; those already
   built keep their masks (the key does not carry the switch), so a live arm covers the pipelines it builds. The
   SPIR-V output span (vk_shader.fout_span) is kept for every fragment shader, so array outputs are never masked. */
static int g_unwritten_mask;   /* the launch value */
static totk_live_cache g_unwritten_live;
static _Atomic uint64_t g_unwritten_pipes, g_unwritten_attachments;
int vk_unwritten_mask_on(void) { return g_unwritten_mask; }
static int unwritten_mask_live(void) { return totk_live_cached(&g_unwritten_live, "TOTK_VK_UNWRITTEN_MASK", g_unwritten_mask); }
void vk_draw_switches_init(void) {
    const char *e = getenv("TOTK_VK_PIPE_BOOST"); g_pipe_boost = e && atoi(e) == 1;
    LOGV("TOTK_VK_PIPE_BOOST=%d: %s", g_pipe_boost, g_pipe_boost ?
         "a join on a compile worker's build lends the worker the waiter's nice until it publishes" :
         "compile workers keep nice 10 while joined (default)");
    e = getenv("TOTK_VK_UNWRITTEN_MASK"); g_unwritten_mask = e && atoi(e) == 1;
    LOGV("TOTK_VK_UNWRITTEN_MASK=%d: %s", g_unwritten_mask, g_unwritten_mask ?
         "colour attachments the fragment shader does not output get colorWriteMask 0" :
         "colour write masks follow the game's channel mask only (default)");
    (void)split_pass_draws();
    e = getenv("TOTK_VK_SPLIT_LRZ_SAFE"); g_split_safe_launch = !e || !*e || atoi(e) != 0;
    LOGV("TOTK_VK_SPLIT_LRZ_SAFE=%d: %s", g_split_safe_launch, g_split_safe_launch ?
         "depth epoch transfer touches and conservative split refusals (default)" : "unsafe split control arm");
    e = getenv("TOTK_VK_LOAD_ELIDE"); g_load_elide_launch = e && atoi(e) == 1;
    LOGV("[nvn-vk] TOTK_VK_LOAD_ELIDE=%d: conservative first-draw coverage proof, next pass open", g_load_elide_launch);
    e=getenv("TOTK_VK_FEEDBACK_NARROW"); g_feedback_narrow_launch=e && atoi(e)==1;
    LOGV("[nvn-vk] TOTK_VK_FEEDBACK_NARROW=%d: texture-only feedback boundaries, next pass open",g_feedback_narrow_launch);
    e=getenv("TOTK_VK_ALPHA_TEST"); g_alpha_launch=e && atoi(e)==1;
    LOGV("[nvn-vk] TOTK_VK_ALPHA_TEST=%d: next draw, fragment alpha rejection with immutable reference blocks",g_alpha_launch);
    e=getenv("TOTK_VK_CLEAR_ON_READ"); g_clear_read_launch=e && atoi(e)==1;
    LOGV("[nvn-vk] TOTK_VK_CLEAR_ON_READ=%d: next texture read, resolve pending attachment clears",g_clear_read_launch);
    e = getenv("TOTK_VK_FEEDBACK_CUT"); g_feedback_cut_launch = e && atoi(e) == 1;
    atomic_store_explicit(&g_feedback_cut, g_feedback_cut_launch, memory_order_relaxed);
    LOGV("TOTK_VK_FEEDBACK_CUT=%d: %s", g_feedback_cut_launch, g_feedback_cut_launch ?
         "sampled views of an image the pass renders to stop below the attached level" :
         "sampled views may include the attached level (default)");
    e = getenv("TOTK_VK_PREPARE_MASK"); g_prepare_off = vk_off("prepare"); g_prepare_mask_launch = e && atoi(e) == 1;
    e = getenv("TOTK_VK_PREPARE_CHECK"); g_prepare_check = e && atoi(e) == 1;
    LOGV("TOTK_VK_PREPARE_MASK=%d%s: %s%s", g_prepare_mask_launch, g_prepare_off ? " (TOTK_VK_OFF=prepare)" : "",
         g_prepare_mask_launch && !g_prepare_off ? "lagged uniform ranges are prepared only after a rebind or invalidation" :
         "every consumed lagged uniform range is prepared at each draw (default)",
         g_prepare_check ? "; TOTK_VK_PREPARE_CHECK=1 runs each skipped preparation as an oracle" : "");
}
static uint32_t pipe_write_mask(const pipe_key *k) {   /* the key's channel mask minus attachments without an output */
    if (!unwritten_mask_live()) return k->chmask;
    const vk_shader *fs = k->fs;
    uint32_t outputs = fs ? fs->fout_mask | fs->fout_span : 0, mask = k->chmask;
    for (unsigned i = 0; i < 8; i++) if (!(outputs >> i & 1)) mask &= ~(0xfu << (4 * i));
    return mask;
}
#include "vk_pso_runtime.inc"
static void pipe_enqueue(pipe_ent *e) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_pq_tail) g_pq_tail->next = e; else g_pq_head = e;
    g_pq_tail = e; pthread_cond_signal(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
}
static unsigned pipe_wait_max(void) {
    static int n = -1;
    if (n < 0) { const char *v = getenv("TOTK_VK_PIPE_WAIT_MAX"); n = v && *v ? atoi(v) : 90; n = n < 0 ? 0 : n > 10000 ? 10000 : n; }
    return (unsigned)n;   /* zero disables the bound; count distinct game submissions, not draws or GPU chunks */
}
static int topo_class(int prim) { return prim == 0 ? 0 : prim == 1 || prim == 2 || prim == 3 ? 1 : 2; }
static VkPrimitiveTopology topology(int prim) {
    switch (prim) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   /* triangles, quads (drawn as indexed triangles) */
}
/* No blending into integer targets; inactive targets must not distinguish pipelines (SavageWindRecomp's finding). */
static uint32_t pipeline_blend_enable_for(const vk_state *state) {
    uint32_t be = state->color.blend_en;
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.dclass[i]) be &= ~(1u << i);
    return be & (R.ncolor >= 8 ? 0xffu : (1u << R.ncolor) - 1);
}
static uint32_t pipeline_blend_enable(void) { return pipeline_blend_enable_for(&S); }
static void pipe_key_fill(pipe_key *k, const vk_state *state, vk_shader *vs, vk_shader *fs, int prim) {
    memset(k, 0, sizeof *k);
    k->vs = vs; k->fs = fs;
    memcpy(k->cfmt, R.cfmt, sizeof k->cfmt); k->dfmt = R.dfmt; k->sfmt = R.sfmt; k->ncolor = R.ncolor; k->samples = R.samples;
    k->blend_en = pipeline_blend_enable_for(state); k->chmask = state->chmask.mask; k->a2c = state->ms.a2c; k->poly_mode = (uint8_t)state->poly.mode; k->topo_class = (uint8_t)topo_class(prim);
    k->depth_clamp = (uint8_t)state->depth_clamp; k->logic_op = (uint8_t)state->color.logic_op;
    if (k->ncolor < 8) k->chmask &= (1u << (4 * k->ncolor)) - 1;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) if (k->blend_en >> i & 1) k->blend[i] = state->blend[i];
    for (int a = 0; a < 16; a++) if (vs->vin_mask >> a & 1) {
        if (a < state->nattr && state->attr[a].fmt) {
            k->attr[a].fmt = state->attr[a].fmt; k->attr[a].offset = state->attr[a].offset; k->attr[a].stream = state->attr[a].stream;
            int st = state->attr[a].stream; if (st < 16) { k->stream[st].divisor = state->stream[st].divisor; k->stream[st].instanced = state->stream[st].divisor != 0 && state->stream[st].stride != 0; }
        }
    }
}
#define ZERO_BINDING 16   /* a zero vertex buffer for attributes the shader reads but the game left unset */
/* the pipeline of a key; flags VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT asks the pipeline cache alone */
static VkResult pipe_create_kind(const pipe_key *k, VkPipelineLayout layout, VkPipelineCreateFlags flags, VkPipeline *out, uint64_t frame, int synchronous, int part) {
    vk_shader *vs = k->vs, *fs = k->fs; VkPipeline p = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs->mod, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs ? fs->mod : VK_NULL_HANDLE, .pName = "main"}};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vs->required_subgroup_size},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = fs ? fs->required_subgroup_size : 0}};
    if (vs->required_subgroup_size) st[0].pNext = &subgroup[0];
    if (fs && fs->required_subgroup_size) st[1].pNext = &subgroup[1];
    /* depth the fragment shader writes gets the same DEPTH16 nudge as rasterized depth (see d16tie_on) */
    float d16spec = D16_NUDGE_UNITS / 65535.0f; VkSpecializationMapEntry sme = {D16_SPEC_ID, 0, 4}; VkSpecializationInfo spi = {1, &sme, 4, &d16spec};
    if (fs && fs->depth_spec && k->dfmt == VK_FORMAT_D16_UNORM && d16tie_on()) st[1].pSpecializationInfo = &spi;
    VkVertexInputBindingDescription bd[17]; VkVertexInputAttributeDescription ad[32]; VkVertexInputBindingDivisorDescriptionEXT dd[16];
    uint32_t nb = 0, na = 0, nd = 0, used_streams = 0; int zero_used = 0;
    for (int a = 0; a < 32; a++) if (vs->vin_mask >> a & 1) {
        VkFormat vf = a < 16 && k->attr[a].fmt ? vk_vertex_fmt((int)k->attr[a].fmt) : VK_FORMAT_UNDEFINED;
        if (a < 16 && k->attr[a].fmt && !vf) RED(8, "vertex attribute format %#x has no Vulkan format", k->attr[a].fmt);
        if (!vf) {
            int cls = vs->vin_class[a];
            ad[na++] = (VkVertexInputAttributeDescription){(uint32_t)a, ZERO_BINDING, cls == 1 ? VK_FORMAT_R32G32B32A32_UINT : cls == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            zero_used = 1; continue;
        }
        uint32_t s = k->attr[a].stream & 15;
        ad[na++] = (VkVertexInputAttributeDescription){(uint32_t)a, s, vf, k->attr[a].offset};
        used_streams |= 1u << s;
    }
    for (uint32_t s = 0; s < 16; s++) if (used_streams >> s & 1) {
        bd[nb++] = (VkVertexInputBindingDescription){s, 0, k->stream[s].instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->stream[s].instanced && k->stream[s].divisor > 1) {
            if (V.has_divisor) dd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->stream[s].divisor};
            else RED(4, "instance divisor %u needs VK_EXT_vertex_attribute_divisor", k->stream[s].divisor);
        }
    }
    if (zero_used) bd[nb++] = (VkVertexInputBindingDescription){ZERO_BINDING, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dsi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nd ? &dsi : NULL, 0, nb, bd, na, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = k->depth_clamp && V.props.limits.maxViewports, .polygonMode = k->poly_mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : k->poly_mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = k->samples ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT, .alphaToCoverageEnable = k->a2c & 1};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    uint32_t chmask = pipe_write_mask(k), unwritten = 0;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        uint32_t m = chmask >> (4 * i) & 0xf;
        unwritten += k->cfmt[i] && (k->chmask >> (4 * i) & 0xf) && !m;
        cba[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = (m & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (m & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) | (m & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) | (m & 8 ? VK_COLOR_COMPONENT_A_BIT : 0)};
        if (k->blend_en >> i & 1 && k->cfmt[i]) {
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = vk_blend_factor(k->blend[i].src_rgb); cba[i].dstColorBlendFactor = vk_blend_factor(k->blend[i].dst_rgb); cba[i].colorBlendOp = vk_blend_op(k->blend[i].eq_rgb);
            cba[i].srcAlphaBlendFactor = vk_blend_factor(k->blend[i].src_a); cba[i].dstAlphaBlendFactor = vk_blend_factor(k->blend[i].dst_a); cba[i].alphaBlendOp = vk_blend_op(k->blend[i].eq_a);
        }
        if (!k->cfmt[i]) cba[i].colorWriteMask = 0;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba};
    if (k->logic_op != NVN_LOGIC_OP_COPY && k->logic_op && V.has_logic_op) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic_op - 1 > 15 ? 3 : k->logic_op - 1); }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->cfmt,
        .depthAttachmentFormat = k->dfmt, .stencilAttachmentFormat = k->sfmt};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr, .flags = flags, .stageCount = fs ? 2 : 1, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = layout};
    VkGraphicsPipelineLibraryCreateInfoEXT subset = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    VkDynamicState subset_dyn[sizeof dyn / sizeof *dyn];
    if (part >= 0) {
        static const VkGraphicsPipelineLibraryFlagBitsEXT bits[PL_COUNT] = {
            VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
        subset.flags = bits[part]; subset.pNext = &pr; ci.pNext = &subset;
        ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
        ci.stageCount = part == PL_PRE_RASTER ? 1 : part == PL_FRAGMENT && fs ? 1 : 0;
        ci.pStages = part == PL_PRE_RASTER ? st : part == PL_FRAGMENT && fs ? st + 1 : NULL;
        ci.pVertexInputState = part == PL_VERTEX_INPUT ? &vi : NULL;
        ci.pInputAssemblyState = part == PL_VERTEX_INPUT ? &ia : NULL;
        ci.pViewportState = part == PL_PRE_RASTER ? &vp : NULL;
        ci.pRasterizationState = part == PL_PRE_RASTER ? &rs : NULL;
        ci.pMultisampleState = part == PL_FRAGMENT || part == PL_OUTPUT ? &ms : NULL;
        ci.pDepthStencilState = part == PL_FRAGMENT ? &ds : NULL;
        ci.pColorBlendState = part == PL_OUTPUT ? &cb : NULL;
        ci.layout = part == PL_PRE_RASTER || part == PL_FRAGMENT ? layout : VK_NULL_HANDLE;
        if (part != PL_OUTPUT) { pr.colorAttachmentCount = 0; pr.pColorAttachmentFormats = NULL; }
        if (part != PL_FRAGMENT && part != PL_OUTPUT) {
            pr.depthAttachmentFormat = VK_FORMAT_UNDEFINED; pr.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        }
        dy.dynamicStateCount = 0; dy.pDynamicStates = subset_dyn;
        for (unsigned i = 0; i < sizeof dyn / sizeof *dyn; i++) {
            VkDynamicState d = dyn[i];
            int owner = d == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY || d == VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE ||
                        d == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE ? PL_VERTEX_INPUT :
                        d == VK_DYNAMIC_STATE_BLEND_CONSTANTS ? PL_OUTPUT :
                        d == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK || d == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK ||
                        d == VK_DYNAMIC_STATE_STENCIL_REFERENCE || d == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE ||
                        d == VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE || d == VK_DYNAMIC_STATE_DEPTH_COMPARE_OP ||
                        d == VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE || d == VK_DYNAMIC_STATE_STENCIL_OP ? PL_FRAGMENT : PL_PRE_RASTER;
            if (owner == part) subset_dyn[dy.dynamicStateCount++] = d;
        }
    }
    uint64_t t0 = g_hitch_on ? host_now_ns() : 0;
    /* flags=0 cache: Vulkan internally synchronizes creation and concurrent snapshot extraction.
       Do not add a coarse backend lock across compiles. Opt-in pcache overlap telemetry measures interference. */
    int diag = synchronous && dm_wait_stats_enabled();
    int credit = synchronous && pipe_predict_on() && pipe_predict_adapt();
    int probe = (flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) != 0;
    int trace = synchronous && !probe && pso_trace_on();
    int measure = !probe && (diag || credit || trace);
    uint64_t build_start = measure ? host_now_ns() : 0;
    VkResult r = vk_pcache_graphics(&ci, &p);
    if (measure) {
        uint64_t elapsed = host_now_ns() - build_start;
        if (diag && !probe) pipe_diag_add(&g_pp_build_ns, elapsed);
        if (credit && !probe) nvn_scan_budget_build(&g_pp_budget, g_pp_frame, elapsed);
    }
    if (trace) pso_trace_target_log(k, "sync-build", frame, r, host_now_ns() - build_start);
    if (!r && (runtime_android_fps_auto_active() || runtime_fps_adaptive_selected()))
        runtime_android_fps_auto_pipeline(host_now_ns());
    if (!r && frame) {
        if (frame == totk_ft_work_frame()) totk_ft_count(FT_COUNT_PIPE_CREATES, 1);
        else totk_ft_count_frame(frame, FT_COUNT_PIPE_CREATES, 1);
    }
    if (r != VK_PIPELINE_COMPILE_REQUIRED) { HK_ADD(pipes, 1); HK_ADD(pipe_ns, host_now_ns() - t0); }
    if (part >= 0 && r && p) vkDestroyPipeline(V.dev, p, NULL);
    if (!r && part < 0 && unwritten) {
        atomic_fetch_add_explicit(&g_unwritten_pipes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_unwritten_attachments, unwritten, memory_order_relaxed);
    }
    *out = r ? VK_NULL_HANDLE : p;
    return r;
}
static VkResult pipe_create(const pipe_key *k, VkPipelineLayout layout, VkPipelineCreateFlags flags, VkPipeline *out, uint64_t frame, int synchronous) {
    return pipe_create_kind(k, layout, flags, out, frame, synchronous, -1);
}
static pipe_library_key pipe_library_key_for(const pipe_key *k, VkPipelineLayout layout, int part) {
    pipe_library_key key = {0};
    if (part == PL_VERTEX_INPUT) {
        key.k.vs = k->vs; key.k.topo_class = k->topo_class;
        memcpy(key.k.attr, k->attr, sizeof k->attr); memcpy(key.k.stream, k->stream, sizeof k->stream);
    } else if (part == PL_PRE_RASTER) {
        key.k.vs = k->vs; key.layout = layout; key.k.poly_mode = k->poly_mode; key.k.depth_clamp = k->depth_clamp;
    } else if (part == PL_FRAGMENT) {
        key.k.fs = k->fs; key.layout = layout; key.k.dfmt = k->dfmt; key.k.sfmt = k->sfmt;
        key.k.samples = k->samples; key.k.a2c = k->a2c;
    } else {
        memcpy(key.k.cfmt, k->cfmt, sizeof k->cfmt); key.k.dfmt = k->dfmt; key.k.sfmt = k->sfmt;
        key.k.ncolor = k->ncolor; key.k.samples = k->samples; key.k.a2c = k->a2c;
        memcpy(key.k.blend, k->blend, sizeof k->blend);
        key.k.blend_en = k->blend_en; key.k.chmask = pipe_write_mask(k); key.k.logic_op = k->logic_op;
    }
    return key;
}
static VkResult pipe_library_build(pipe_ent *e) {
    for (int part = 0; part < PL_COUNT; part++) {
        pipe_library_key key = pipe_library_key_for(&e->k, e->layout, part);
        pthread_mutex_lock(&g_library_mu);
        pipe_library *lib = nvn_hm_get(&g_libraries[part], &key);
        if (!lib) {
            lib = calloc(1, sizeof *lib);
            if (!lib) { pthread_mutex_unlock(&g_library_mu); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            nvn_hm_put(&g_libraries[part], &key, lib);
        }
        while (lib->building) pthread_cond_wait(&g_library_cv, &g_library_mu);
        if (lib->failed) { pthread_mutex_unlock(&g_library_mu); return VK_ERROR_UNKNOWN; }   /* no retry per key */
        if (lib->p) { pthread_mutex_unlock(&g_library_mu); continue; }
        lib->building = 1;
        pthread_mutex_unlock(&g_library_mu);
        VkPipeline p; uint64_t started = host_now_ns();
        VkResult r = pipe_create_kind(&e->k, e->layout, 0, &p, 0, 0, part);
        uint64_t elapsed = host_now_ns() - started;
        pso_trace_log(&e->k, "build", "library-build", e->frame, r, elapsed, 0, part);
        atomic_fetch_add(&g_library_stats.parts[part], 1);
        atomic_fetch_add(&g_library_stats.part_ns[part], elapsed);
        pthread_mutex_lock(&g_library_mu);
        lib->p = p; lib->building = 0; lib->failed = r != VK_SUCCESS;
        pthread_cond_broadcast(&g_library_cv); pthread_mutex_unlock(&g_library_mu);
        if (r) return r;
    }
    return VK_SUCCESS;
}
static VkPipeline pipe_fast_get(pipe_ent *e) {
    if (!V.has_pipeline_library) return VK_NULL_HANDLE;
    if (!e->fast && !e->fast_attempted) {
        VkPipeline parts[PL_COUNT]; int ready = 1;
        pthread_mutex_lock(&g_library_mu);
        for (int part = 0; part < PL_COUNT; part++) {
            pipe_library_key key = pipe_library_key_for(&e->k, e->layout, part);
            pipe_library *lib = nvn_hm_get(&g_libraries[part], &key);
            parts[part] = lib ? lib->p : VK_NULL_HANDLE;
            if (!parts[part]) ready = 0;
        }
        pthread_mutex_unlock(&g_library_mu);
        if (!ready) return VK_NULL_HANDLE;
        e->fast_attempted = 1;
        VkPipelineLibraryCreateInfoKHR libs = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
            .libraryCount = PL_COUNT, .pLibraries = parts};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &libs, .layout = e->layout};
        uint64_t started = host_now_ns();
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vk_pcache_graphics(&ci, &p);
        uint64_t elapsed = host_now_ns() - started;
        pso_trace_log(&e->k, "build", "library-build", e->frame, r, elapsed, 0, PL_COUNT);
        if (dm_wait_stats_enabled()) pipe_diag_add(&g_pp_build_ns, elapsed);
        atomic_fetch_add(&g_library_stats.fast_link_ns, elapsed);
        uint64_t max = atomic_load(&g_library_stats.fast_link_max_ns);
        while (elapsed > max && !atomic_compare_exchange_weak(&g_library_stats.fast_link_max_ns, &max, elapsed)) {}
        max = atomic_load(&g_library_stats.fast_link_window_max_ns);
        while (elapsed > max && !atomic_compare_exchange_weak(&g_library_stats.fast_link_window_max_ns, &max, elapsed)) {}
        if (!r) {
            e->fast = p; atomic_fetch_add(&g_library_stats.fast_links, 1);
            if (g_pso_collect) { pthread_mutex_lock(&g_pq_mu); pso_collect_success(e); pthread_mutex_unlock(&g_pq_mu); }
        } else {
            if (p) vkDestroyPipeline(V.dev, p, NULL);
            LOGV("[pipeline-library] fast link failed: %d; keeping ordinary worker/backoff path", r);
        }
    }
    g_draw_fast = e->fast != VK_NULL_HANDLE;
    return e->fast;
}
static VkPipeline pipe_ready_get(pipe_ent *e) {
    g_draw_fast = 0;
    if (e->fast && !e->replaced) {
        e->replaced = 1;
        atomic_fetch_add(&g_library_stats.optimized_replacements, 1);
        atomic_fetch_add(&g_library_stats.optimized_ns, e->optimized_ns);
        /* Keep the old handle with its cache entry, just like the optimized handle: submissions/secondary
           recorders can still reference it. No device idle or unsafe immediate destruction at replacement. */
    }
    return e->p;
}
/* Exactly one builder owns an entry. Publication and prewarm accounting are shared by workers and bounded takeover.
   Returns the nice a waiter lent the building worker; once published no waiter can lend it again. */
static vk_lend pipe_publish(pipe_ent *e, VkResult r, VkPipeline p, uint64_t dt) {
    pthread_mutex_lock(&g_pq_mu);
    if (r) {
        HK_ADD(pipes_fail, 1);
        if (!e->failures)
            LOGV("pipeline failure key %p (vs %s fs %s): %d; retry next submission, then back off 30..300 submissions (never permanent skip)",
                 (void *)e, ((vk_shader *)e->k.vs)->key, e->k.fs ? ((vk_shader *)e->k.fs)->key : "-", r);
        e->failures++;
    } else if (e->failures) LOGV("pipeline recovered key %p after %u failed attempts", (void *)e, e->failures);
    if (!r) pso_collect_success(e);
    pso_complete(e, r, dt);
    e->optimized_ns = dt;
    e->p = p;
    atomic_store_explicit(&e->state, r ? PE_FAILED : PE_READY, memory_order_release);
    vk_lend lend = e->lend; e->lend = (vk_lend){0};
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return lend;
}
static int pipe_pending(int state) { return state == PE_QUEUED || state == PE_BUILDING; }
/* Remove a queued entry to build it on the walk. An in-flight build is joined, never duplicated or overwritten. */
static int pipe_build_now(pipe_ent *e) {
    pthread_mutex_lock(&g_pq_mu);
    int state = atomic_load_explicit(&e->state, memory_order_acquire), build = state == PE_FAILED;
    if (build) e->frame = totk_ft_work_frame(); /* a retry is a new request; queued takeovers retain their origin */
    if (state == PE_QUEUED) {
        pipe_ent *prev = NULL, *q = g_pq_head;
        while (q && q != e) { prev = q; q = q->next; }
        if (q) {
            if (prev) prev->next = e->next; else g_pq_head = e->next;
            if (g_pq_tail == e) g_pq_tail = prev;
            e->next = NULL; build = 1;
        }
    }
    if (build) atomic_store_explicit(&e->state, PE_BUILDING, memory_order_release);
    else if (pipe_pending(state)) {
        int diag = dm_wait_stats_enabled(), refused = 0;
        uint64_t started = host_now_ns();
        while (pipe_pending(state)) {
            /* A queued entry gets its worker when one takes it; lend again after any wake while unlent. */
            if (!refused && vk_pipe_boost_lend(&e->lend) < 0) refused = 1;
            pthread_cond_wait(&g_pq_cv, &g_pq_mu);
            state = atomic_load_explicit(&e->state, memory_order_acquire);
        }
        uint64_t waited = host_now_ns() - started;
        vk_pipe_boost_waited(waited);
        if (diag) pipe_diag_add(&g_pp_wait_ns, waited);
    }
    pthread_mutex_unlock(&g_pq_mu);
    if (build) {
        uint64_t t0 = host_now_ns(); VkPipeline p;
        HK_ADD(pipes_sync, 1);
        int warming = pso_sync_begin();
        VkResult r = pipe_create(&e->k, e->layout, 0, &p, e->frame, 1);
        pso_sync_end(warming);
        uint64_t dt = host_now_ns() - t0;
        pso_trace_log(&e->k, "build", state == PE_FAILED ? "retry" : "takeover",
                      e->frame, r, dt, 0, -1);
        g_win_sync++; g_win_sync_ns += dt;
        vk_overlay_note_compile(t0 + dt, dt);
        pipe_publish(e, r, p, dt);
        state = r ? PE_FAILED : PE_READY;
    }
    return state;
}
static void *pipe_worker(void *arg) {
    char name[16]; snprintf(name, sizeof name, "totk-pipe%u", (unsigned)(uintptr_t)arg);
    totk_thread_start(name, NULL);
    setpriority(PRIO_PROCESS, 0, VK_WORKER_NICE);   /* this thread: below the game's threads */
    int self = vk_thread_tid();
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        int background = 0;
        for (;;) {
            if (g_pq_head) break;
            if (g_pw_head && (uintptr_t)arg == 0 && pso_allow_background(g_pq_active != 0, g_pso_sync,
                    host_now_ns() < atomic_load_explicit(&g_pso_pressure_until, memory_order_relaxed),
                    g_pw_active, host_now_ns(), g_pw_pause_until)) { background = 1; break; }
            if (!g_pw_head) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
            else {
                struct timespec wake; clock_gettime(CLOCK_REALTIME, &wake); wake.tv_nsec += 100000000;
                if (wake.tv_nsec >= 1000000000) { wake.tv_nsec -= 1000000000; wake.tv_sec++; }
                pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &wake);
            }
        }
        pipe_ent *e;
        if (background) {
            e = g_pw_head; g_pw_head = e->next; if (!g_pw_head) g_pw_tail = NULL;
            g_pw_active = 1;
#ifdef TOTK_VK_TESTS
            if (!e->capture) g_pso_test_screen_started = 1;
            else if (g_pso_test_screen_started) g_pso_test_order_bad = 1;
#endif
        } else { e = g_pq_head; g_pq_head = e->next; if (!g_pq_head) g_pq_tail = NULL; }
        int demand_active = !background && atomic_load_explicit(&g_pso_warming, memory_order_relaxed);
        g_pq_active += demand_active;
        e->next = NULL; e->lend = (vk_lend){.tid = self}; atomic_store_explicit(&e->state, PE_BUILDING, memory_order_release);
        pthread_mutex_unlock(&g_pq_mu);
        uint64_t t0 = host_now_ns(); VkPipeline p;
        /* Library parts only enable fast links for later draws; a part failure never fails the entry itself. */
        if (!e->list_index && V.has_pipeline_library && pipe_library_build(e)) RED(4, "[pipeline-library] part creation failed; ordinary pipeline build continues");
        uint64_t build_started = pso_trace_on() ? host_now_ns() : 0;
        VkResult r = e->list_index && V.has_cache_control ? pipe_create(&e->k, e->layout,
            VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, &p, e->frame, 0) : VK_PIPELINE_COMPILE_REQUIRED;
        if (r == VK_PIPELINE_COMPILE_REQUIRED) r = pipe_create(&e->k, e->layout, 0, &p, e->frame, 0);
        if (build_started) pso_trace_log(&e->k, "build", e->list_index ? "prewarm-worker" : "async-worker",
                                        e->frame, r, host_now_ns() - build_started, 0, -1);
        uint64_t dt = host_now_ns() - t0; g_win_async++; g_win_async_ns += dt;
        vk_lend lent = pipe_publish(e, r, p, dt);
        vk_pipe_boost_restore(&lent);
        if (background || demand_active) {
            pthread_mutex_lock(&g_pq_mu);
            if (background) { g_pw_active = 0; g_pw_pause_until = host_now_ns()+10000000; }
            g_pq_active -= demand_active;
            if (!g_pw_head && !g_pw_active) atomic_store_explicit(&g_pso_warming, 0, memory_order_relaxed);
            pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
        }
    }
    return NULL;
}
static int compile_threads(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("TOTK_VK_COMPILE_THREADS"); n = e && *e ? atoi(e) : 2; n = n < 0 ? 0 : n > 8 ? 8 : n;
        e = getenv("TOTK_PIPE_WORKERS");
        if (e && *e) { n = atoi(e); n = n < 1 ? 1 : n > 4 ? 4 : n; }
        if (n && !V.has_cache_control) { LOGV("pipelines compile at the draw: the driver lacks pipelineCreationCacheControl"); n = 0; }
        for (int i = 0; i < n; i++) { pthread_t t; if (pthread_create(&t, NULL, pipe_worker, (void *)(uintptr_t)i)) { n = i; break; } pthread_detach(t); }
        LOGV("pipelines the driver has to compile: %s", n ? "on worker threads, their draws skipped until ready (TOTK_VK_COMPILE_THREADS)" : "at the draw");
    }
    return n;
}
/* A Vulkan pass already has exact attachment formats. Predict only within that target set:
   crossing an attachment change would require speculative resource/view realization. Calls, jumps,
   conditions, transfers, updates and registry changes are boundaries, never executed by this scan.
   The shadow is private: no live state, shader selection or recorder/lag memo is changed. */
static int pipe_predict_limit(const char *name, int dflt, int lo, int hi) {
    const char *e = getenv(name); int n = e ? atoi(e) : dflt; return n < lo ? lo : n > hi ? hi : n;
}
static void pipe_predict_maybe(const uint8_t *next) {
    if (!pipe_predict_on() || !next || !R.open || !compile_threads()) return;
    static int max_ops, max_keys, max_us, frame_us, rescans, initialized;
    if (!initialized) {
        max_ops = pipe_predict_limit("TOTK_PSO_PREDICT_SCAN", 8192, 1, 1 << 20);
        max_keys = pipe_predict_limit("TOTK_PSO_PREDICT_KEYS", 32, 1, 160);
        max_us = pipe_predict_limit("TOTK_PSO_PREDICT_US", 2000, 1, 1000000);
        frame_us = pipe_predict_limit("TOTK_PSO_PREDICT_FRAME_US", 4000, 0, 1000000);
        rescans = pipe_predict_limit("TOTK_PSO_RESCANS", 32, 0, 1024);
        initialized = 1;
    }
    uint64_t sub = vk_game_submit_serial();
    if (g_pp_submit != sub) { g_pp_submit = sub; g_pp_scans_in_sub = 0; g_pp_more = 1; }
    if (g_pp_scans_in_sub && (!g_pp_more && g_pp_pass == g_stat_passes)) return;
    if (g_pp_scans_in_sub > (unsigned)rescans) return;   /* first scan + bounded rescans */
    int adapt = pipe_predict_adapt();
    uint64_t fixed = (uint64_t)frame_us * 1000;
    if (!nvn_scan_budget_left(&g_pp_budget, g_pp_frame, fixed, adapt)) {
        g_pp_more = adapt; return;   /* a later actual build may earn another scan */
    }
    uint64_t allowance = adapt && g_pp_budget.sync_ns > fixed ? g_pp_budget.sync_ns : fixed;
    uint64_t scan_ns = MINV((uint64_t)max_us * 1000, allowance - g_pp_budget.used_ns);
    uint64_t started = host_now_ns();
    vk_state predicted = S;
    unsigned ops = 0, keys = 0, stop = 0;
    g_pp_pass = g_stat_passes; g_pp_scans_in_sub++;
    for (;;) {
        if (ops >= (unsigned)max_ops) { stop = 1; break; }
        if (keys >= (unsigned)max_keys) { stop = 2; break; }
        if (host_now_ns() - started >= scan_ns) { stop = 3; break; }
        op_hdr h; memcpy(&h, next, sizeof h); const void *a = next + sizeof h;
        if (h.size < sizeof h || (h.size & 7) || h.op >= OP_COUNT_) break;
        ops++;
#define PP_NEED(type) do { if (h.size < sizeof h + sizeof(type)) goto predicted_done; } while (0)
        switch (h.op) {
            case OP_BIND_PROGRAM: PP_NEED(p_bind_program); predicted.prog = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_STATE: {
                PP_NEED(p_bind_state); const p_bind_state *b = a; blend_st bs;
                switch (b->kind) {
                    case 0: memcpy(&bs, b->bytes, sizeof bs); predicted.blend[bs.target & 7] = bs; break;
                    case 1: memcpy(&predicted.color, b->bytes, sizeof predicted.color); break;
                    case 2: memcpy(&predicted.chmask, b->bytes, sizeof predicted.chmask); break;
                    case 3: memcpy(&predicted.ms, b->bytes, sizeof predicted.ms); break;
                    case 4: memcpy(&predicted.poly, b->bytes, sizeof predicted.poly); break;
                    case 5: break;   /* dynamic depth/stencil state is not a Vulkan pipeline key input */
                    default: goto predicted_done;
                }
                break;
            }
            case OP_BIND_VATTR: {
                PP_NEED(p_bind_vattr); const p_bind_vattr *b = a;
                if (b->n < 0 || b->n > 16) goto predicted_done;
                memcpy(predicted.attr, b->st, (size_t)b->n * sizeof *b->st); predicted.nattr = b->n; break;
            }
            case OP_BIND_VSTREAM: {
                PP_NEED(p_bind_vstream); const p_bind_vstream *b = a;
                if (b->n < 0 || b->n > 16) goto predicted_done;
                memcpy(predicted.stream, b->st, (size_t)b->n * sizeof *b->st); predicted.nstream = b->n; break;
            }
            case OP_BIND_TEX: case OP_BIND_IMG: {
                if (h.size < sizeof h + offsetof(p_bind_handles, h)) goto predicted_done;
                const p_bind_handles *b = a; int count = h.op == OP_BIND_TEX ? 128 : 32;
                if (b->stage < 0 || b->stage >= 6 || b->n < 0 || b->n > count ||
                    b->first < 0 || b->first > count - b->n ||
                    h.size < sizeof h + offsetof(p_bind_handles, h) + (size_t)b->n * 8) goto predicted_done;
                if (h.op == OP_BIND_TEX) {
                    memcpy(&predicted.tex[b->stage][b->first], b->h, (size_t)b->n * 8);
                    if (b->first + b->n > predicted.tex_hi[b->stage]) predicted.tex_hi[b->stage] = b->first + b->n;
                } else memcpy(&predicted.img[b->stage][b->first], b->h, (size_t)b->n * 8);
                break;
            }
            case OP_DEPTH_CLAMP: PP_NEED(p_flag); predicted.depth_clamp = ((const p_flag *)a)->value; break;
            case OP_RENDER_ENABLE: PP_NEED(p_flag); predicted.render_disabled = !((const p_flag *)a)->value; break;
            case OP_RASTER_DISCARD: PP_NEED(p_flag); predicted.raster_discard = ((const p_flag *)a)->value; break;
            case OP_ALPHA_REF: PP_NEED(p_alpha_ref); break; /* reference bytes are not a pipeline input */
            case OP_SET_RT:
                PP_NEED(p_set_rt);
                if (!rt_same(&predicted.rt, a)) goto predicted_done;
                break;
            case OP_DRAW: case OP_DRAW_IDX: {
                int prim, count;
                if (h.op == OP_DRAW) { PP_NEED(p_draw); prim = ((const p_draw *)a)->prim; count = ((const p_draw *)a)->count; }
                else { PP_NEED(p_draw_idx); prim = ((const p_draw_idx *)a)->prim; count = ((const p_draw_idx *)a)->count; }
                if (!count || prim < 0 || prim == 2 || prim >= 8 || (h.op == OP_DRAW_IDX && prim == 7) ||
                    predicted.render_disabled || predicted.raster_discard || predicted.poly.cull == NVN_FACE_FRONT_AND_BACK) break;
                /* Prediction never loads modules. Alpha variants are demand-created;
                 * do not speculate the base module's (wrong) pipeline for this draw. */
                if (predicted.color.alpha_func && predicted.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS &&
                    totk_live_cached(&g_alpha_live,"TOTK_VK_ALPHA_TEST",g_alpha_launch)) break;
                vk_shader *sh[6]; if (!vk_prog_predict(&predicted, sh)) break;
                pipe_key k; pipe_key_fill(&k, &predicted, sh[0], sh[1], prim);
                if (nvn_hm_get(&g_pipes, &k)) break;   /* ready and pending entries remain authoritative */
                pipe_ent *e = calloc(1, sizeof *e); if (!e) goto predicted_done;
                e->k = k; e->layout = vk_layout_of(sh[0], sh[1]); e->frame = totk_ft_work_frame(); e->predicted = 1;
                nvn_hm_put(&g_pipes, &k, e); g_stat_pipes++; keys++;
                pipe_enqueue(e);
                break;
            }
            case OP_BEGIN: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_SCISSOR: case OP_VIEWPORTS:
            case OP_DEPTH_RANGE: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_SAMP:
            case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
                break;   /* these do not select Vulkan shader variants or fixed pipeline state */
            default: goto predicted_done;
        }
#undef PP_NEED
        next += h.size;
    }
predicted_done:;
    uint64_t elapsed = host_now_ns() - started;
    nvn_scan_budget_spend(&g_pp_budget, g_pp_frame, elapsed);
    if (dm_wait_stats_enabled()) pipe_diag_add(&g_pp_scan_ns, elapsed);
    g_pp_more = stop && keys;   /* only a productive bounded scan earns a rescan, not more time */
#ifdef TOTK_VK_TESTS
    g_pp_test.scans++; g_pp_test.ops = ops; g_pp_test.keys = keys; g_pp_test.stop = stop;
#endif
}
#ifdef TOTK_VK_TESTS
void vk_pipeline_test_predict(const uint8_t *ops) { pipe_predict_maybe(ops); }
#endif

void vk_pipeline_prewarm(void) {
#ifndef TOTK_PSO_COLLECT_DEFAULT
#define TOTK_PSO_COLLECT_DEFAULT 0
#endif
    const char *collect = getenv("TOTK_PSO_COLLECT");
    g_pso_collect = collect ? atoi(collect) != 0 : TOTK_PSO_COLLECT_DEFAULT;
    const char *current_cache = vk_pcache_current_path();
    if (g_pso_collect && !pso_collection_path(current_cache)) {
        RED(4, "[pso-collect] cache path too long; collection disabled without touching the cache"); g_pso_collect = 0;
    }
    if (g_pso_collect) {
        g_pso_records = pso_read(g_pso_path, &g_pso_count, NULL);
        g_pso_capacity = g_pso_count;
        for (uint32_t i = 0; i < g_pso_count; i++) nvn_hm_put(&g_pso_keys, g_pso_records[i].key, (void *)(uintptr_t)(i+1));
        LOGV("[pso-collect] enabled; retained %u successful graphics keys", g_pso_count);
    }
    const char *path = getenv("TOTK_PSO_KEYS"), *enable = getenv("TOTK_PSO_PREWARM");
    pso_record *records = path && (!enable || atoi(enable)) ? pso_read(path, &g_pw_total, g_pw_digest) : NULL;
    if (path && (!enable || atoi(enable)) && !records) LOGV("[pso-prewarm] rejected unreadable/incompatible/checksum-failed list: %s", path);
    const char *cache = vk_pcache_current_path();
    if (cache) snprintf(g_pw_path, sizeof g_pw_path, "%s.pso-state", cache);
    g_pw_started = host_now_ns();
    pso_restore();
    pipe_ent *head = NULL, *tail = NULL;
    if (records && g_pw_total > g_pw_completed && compile_threads()) {
        for (int priority = 1; priority >= 0; priority--) for (uint32_t i = 0; i < g_pw_total; i++) {
            if (records[i].capture != (uint32_t)priority) continue;
            if (g_pw_done[i/8] >> (i%8) & 1) continue;
            pso_record *r = records+i; pipe_key k; pso_decode(r, &k);
            k.vs = vk_shader_seed((const char *)r->key, 0, 0);
            k.fs = r->key[80] ? vk_shader_seed((const char *)r->key+80, 1, pso_u32(r->key+160)) : NULL;
            if (!k.vs || (r->key[80] && !k.fs)) { g_pw_missing++; continue; }
            if (nvn_hm_get(&g_pipes, &k)) continue;
            pipe_ent *e = calloc(1, sizeof *e);
            if (!e) { RED(4, "[pso-prewarm] entry allocation failed"); break; }
            e->k = k; e->layout = vk_layout_of(k.vs, k.fs); e->list_index = i+1;
            e->capture = r->capture; e->predicted = 1; /* first demand must build/join, never skip a formerly cached draw */
            nvn_hm_put(&g_pipes, &k, e);
            if (tail) tail->next = e; else head = e; tail = e;
        }
    }
    free(records);
    if (path) LOGV("[pso-prewarm] list=%s resumed=%u/%u missing=%u startup_ms=%.1f; background_worker_limit=1, demand first", path, g_pw_completed, g_pw_total, g_pw_missing, (host_now_ns()-g_pw_started)/1e6);
    pthread_mutex_lock(&g_pq_mu);
    g_pw_head = head; g_pw_tail = tail;
    atomic_store_explicit(&g_pso_warming, head != NULL, memory_order_relaxed);
    atomic_store_explicit(&g_pso_ready, g_pso_collect || g_pw_total, memory_order_release);
    pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
static VkPipeline pipe_get(vk_shader *vs, vk_shader *fs, int prim, VkPipelineLayout layout) {
    static pipe_ent *last;   /* entries never move or die; only the recording thread reads this memo */
    static pipe_ent *memo[64];   /* bounded exact-key front cache for alternating shaders/topologies */
    pipe_ent *e = last; pipe_key k;
    g_draw_fast = 0;
    int screen_aspect = pipe_screen_class() != 0;
    int capture = !screen_aspect && R.ncolor;
    if (g_pipe_dirty || !e || e->layout != layout || e->k.vs != vs || e->k.fs != fs || e->k.topo_class != topo_class(prim)) {
        pipe_key_fill(&k, &S, vs, fs, prim); g_pipe_dirty = 0;
        if (!e || e->layout != layout || memcmp(&e->k, &k, sizeof k)) {
            unsigned h = (unsigned)(((uintptr_t)vs >> 4) ^ ((uintptr_t)fs >> 8) ^ k.topo_class) & 63u;
            pipe_ent *candidate = memo[h];
            e = candidate && candidate->layout == layout && !memcmp(&candidate->k, &k, sizeof k) ?
                candidate : nvn_hm_get(&g_pipes, &k);
            if (e) memo[h] = e;
        }
    }
    /* A memo hit still observes a worker's publication, including queued and failed entries. */
    if (e) {
        pso_demand(e, capture);
        last = e; int s = atomic_load_explicit(&e->state, memory_order_acquire);
        if (s == PE_READY) {
            return pipe_ready_get(e);
        }
        VkPipeline fast = !e->predicted && !capture ? pipe_fast_get(e) : VK_NULL_HANDLE;
        if (fast && pipe_pending(s)) return fast;
        uint64_t sub = vk_game_submit_serial();
        unsigned bound = pipe_wait_max();
        /* Only opt-in prediction-origin entries join at their first demand (upstream pso_pend_take).
           Skipping one could hide a draw that the original cache-only probe would have satisfied.
           Ordinary screen misses retain async/skipped-submission bounds; captures always join/build. */
        if (pipe_pending(s) && (e->predicted || capture || (bound && e->wait_sub != sub && e->skipped >= bound))) {
            uint64_t t0 = host_now_ns();
            if (!capture && !e->predicted) { HK_ADD(pipe_bounds, 1); LOGV("pipeline key %p reached %u skipped submissions: build/join synchronously", (void *)e, e->skipped); }
            pipe_predict_maybe(vk_exec_draw_next());
            s = pipe_build_now(e);
            uint64_t dt = host_now_ns() - t0; g_win_prewarm_wait++; g_win_prewarm_wait_ns += dt; g_cum_pipe_ns += dt;
        }
        if (s == PE_FAILED) {
            if (e->seen_failure != e->failures) {
                e->seen_failure = e->failures;
                unsigned shift = e->failures > 5 ? 4 : e->failures > 1 ? e->failures - 2 : 0;
                unsigned delay = e->failures == 1 ? 1 : 30u << shift;
                e->retry_at = sub + (delay > 300 ? 300 : delay);
            }
            if (sub >= e->retry_at) {
                pipe_predict_maybe(vk_exec_draw_next());
                HK_ADD(pipe_retries, 1); uint64_t t0 = host_now_ns();
                s = pipe_build_now(e);   /* one retry per backoff, not per draw; never cache a permanent failure */
                g_cum_pipe_ns += host_now_ns() - t0;
            }
        }
        if (s != PE_READY && fast) return fast;
        if (pipe_pending(s)) {
            if (e->wait_sub != sub) { e->wait_sub = sub; e->skipped++; }
            HK_ADD(pipe_skips, 1);
            if (V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
        }
        if (s == PE_FAILED && V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
        return s == PE_READY ? pipe_ready_get(e) : s == PE_FAILED ? PIPE_FAILED : VK_NULL_HANDLE;
    }
    e = calloc(1, sizeof *e); e->k = k; e->layout = layout; e->frame = totk_ft_work_frame(); last = e;
    e->capture = capture;
    nvn_hm_put(&g_pipes, &k, e);
    unsigned h = (unsigned)(((uintptr_t)vs >> 4) ^ ((uintptr_t)fs >> 8) ^ k.topo_class) & 63u;
    memo[h] = e;
    g_stat_pipes++;
    /* Non-screen colour targets retain the Gerudo capture guard: skipping 64x64 B10G11R11 + depth
       draws permanently changed plaza lighting (09a79bca6). Recognize aligned scene extents before this
       guard: e.g. requested960x540 renders960x536 and must not be mistaken for a persistent probe.
       Depth-only shadow maps retain the existing worker policy; no capture replay/deferral is added. */
    int async = compile_threads() > 0 && (screen_aspect || !R.ncolor);
    pso_trace_target_log(&e->k, "demand", e->frame, VK_SUCCESS, 0);
    uint64_t t0 = host_now_ns(); VkPipeline p;
    int cache_probe = V.has_cache_control;
    uint64_t probe_started = pso_trace_on() ? host_now_ns() : 0;
    VkResult r = pipe_create(&e->k, layout, cache_probe ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0,
                             &p, e->frame, !V.has_cache_control);
    if (probe_started) pso_trace_log(&e->k, cache_probe ? "probe" : "build",
                                     cache_probe ? "cache-probe" : "workers-disabled-sync", e->frame,
                                     r, host_now_ns() - probe_started,
                                     cache_probe ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0, -1);
    if (r == VK_PIPELINE_COMPILE_REQUIRED) {
        VkPipeline fast = async ? pipe_fast_get(e) : VK_NULL_HANDLE;
        /* Queue the actual draw before speculative jobs; a synchronous miss can overlap their builds. */
        if (async) { e->wait_sub = vk_game_submit_serial(); e->skipped = fast ? 0 : 1; pipe_enqueue(e); }
        pipe_predict_maybe(vk_exec_draw_next());
        if (fast) {
            uint64_t dt = host_now_ns() - t0;
            g_win_pipes++; g_win_pipe_ns += dt; g_cum_pipe_ns += dt;
            if (dt > g_win_pipe_max_ns) g_win_pipe_max_ns = dt;
            return fast;
        }
    }
    if (!async && (r == VK_PIPELINE_COMPILE_REQUIRED || !V.has_cache_control)) {
        uint64_t c0 = host_now_ns();
        int warming = pso_sync_begin();
        if (r == VK_PIPELINE_COMPILE_REQUIRED) {
            uint64_t build_started = pso_trace_on() ? host_now_ns() : 0;
            r = pipe_create(&e->k, layout, 0, &p, e->frame, 1);
            if (build_started) pso_trace_log(&e->k, "build", capture ? "capture-sync" : "workers-disabled-sync",
                                            e->frame, r, host_now_ns() - build_started, 0, -1);
        }
        pso_sync_end(warming);
        HK_ADD(pipes_sync, 1);
        uint64_t ended = host_now_ns(), elapsed = ended - (V.has_cache_control ? c0 : t0);
        g_win_sync++; g_win_sync_ns += elapsed;
        vk_overlay_note_compile(ended, elapsed);
    }
    uint64_t dt = host_now_ns() - t0;
    g_win_pipes++; g_win_pipe_ns += dt; g_cum_pipe_ns += dt; if (dt > g_win_pipe_max_ns) g_win_pipe_max_ns = dt;
    if (r == VK_PIPELINE_COMPILE_REQUIRED) {
        HK_ADD(pipe_skips, 1);
        if (V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
        return VK_NULL_HANDLE;
    }
    pipe_publish(e, r, p, dt);
    if (r && V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
    return r ? PIPE_FAILED : p;
}
#ifdef TOTK_VK_TESTS
int vk_draw_test_pipeline_bench(vk_shader *vs, vk_shader *fs) {
    enum { NDRAWS = 200000 };
    const int prim[3] = {0, 1, 4};
    VkPipelineLayout layout = vk_layout_of(vs, fs); VkPipeline expected[3];
    int ok = 1;
    for (int i = 0; i < 3; i++) { expected[i] = pipe_get(vs, fs, prim[i], layout); ok &= expected[i] && expected[i] != PIPE_FAILED; }
    for (int churn = 0; churn < 2; churn++) {
        uint64_t start = host_now_ns();
        for (int draw = 0; draw < NDRAWS; draw++) {
            int i = churn ? draw % 3 : 2;
            ok &= pipe_get(vs, fs, prim[i], layout) == expected[i];
        }
        printf("pipeline-resolve-bench %s: %.2f ns/draw\n", churn ? "alternating" : "stable",
            (double)(host_now_ns() - start) / NDRAWS);
    }
    printf("%s: pipeline lookup preserves exact ready handles for all topology classes\n", ok ? "ok" : "FAIL");
    return !ok;
}
#endif

/* ---------- descriptor sets ---------- */
static void driver_bank_fill(uint8_t *d, int bank, int stage, const uint32_t grid[3]) {   /* nvn_mtl.m driver_bank_fill */
    memset(d, 0, 0xA00);
    if (bank == 0) {
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (stage == 5) {
            if (grid) memcpy(d + 0x200, grid, 12);
            for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
        } else if (stage == 0) { const float ps_max = 2047.9375f; memcpy(d + 0x610, &ps_max, sizeof ps_max); }
    } else {
        for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[stage][r], sh_ = (uint32_t)S.samp[stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh_, 4); }
        for (int r = 0; r < 32; r++) { uint32_t th = (uint32_t)S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &th, 4); }
    }
}
static void support_fill(uint8_t *d, uint32_t w, uint32_t h) {   /* Ryujinx SupportBuffer: render scale 1, no BGRA swap */
    memset(d, 0, 1392);
    float vi[4] = {2.0f / (w ? w : 1), 2.0f / (h ? h : 1), 1, 1}, vs[4] = {(float)w, (float)h, 1, 1};
    memcpy(d + 144, vi, 16); memcpy(d + 160, vs, 16);
    for (int i = 0; i < 73; i++) { float one = 1; memcpy(d + 192 + 16 * i, &one, 4); }
}
typedef struct { VkDescriptorBufferInfo b; VkDescriptorImageInfo i; VkBufferView t; } dinfo;
static int ubo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out, int storage, int rw, uint64_t extent, vk_record_descriptor *record) {
    VkBuffer b; uint64_t off, avail; uint32_t reason = 0;
    if (!addr || !vk_resolve_for(addr, 1, &b, &off, &avail, VK_LOOKUP_UBO)) return 0;
    if (vk_fg_extract_active && FGX.record && !rw) {
        uint64_t limit=storage ? V.props.limits.maxStorageBufferRange : V.props.limits.maxUniformBufferRange;
        uint64_t n=MINV(size ? size : limit,MINV(avail,limit));
        if (extent) n=MINV(n,extent);
        VkDeviceSize align=storage ? V.props.limits.minStorageBufferOffsetAlignment :
            V.props.limits.minUniformBufferOffsetAlignment;
        if (vk_ub_replay_read(addr,n,align,out)) return 1;
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI/transfer readonly buffer cannot be frozen before a pending GPU writer");
    }
    if (storage && vk_lag_capture_on()) {
        if (extent) size = size ? MINV(size, extent) : extent;
        else if (!size) size = vk_lag_bound(addr, avail);
    }
    if (!storage) {
        uint64_t n = MINV(size ? size : 0x10000, MINV(avail, V.props.limits.maxUniformBufferRange));
        if (vk_lag_uniform_read(addr, n, V.props.limits.minUniformBufferOffsetAlignment, out)) return 1;
        if (vk_ub_rename(addr, n, out) || (record ?
            vk_lag_read_record(addr, n, V.props.limits.minUniformBufferOffsetAlignment, out, &reason, VK_LAG_UNIFORM) :
            vk_lag_read(addr, n, V.props.limits.minUniformBufferOffsetAlignment, out, VK_LAG_UNIFORM))) return 1;
    }
    else {   /* a storage binding sees the logged updates in memory; draw_touch wrote them before the pass opened */
        uint64_t n = MINV(size ? size : avail, avail);
        if (!rw && (record ? vk_lag_read_record(addr, MINV(n, V.props.limits.maxStorageBufferRange),
            V.props.limits.minStorageBufferOffsetAlignment, out, &reason, VK_LAG_STORAGE) :
            vk_lag_read(addr, MINV(n, V.props.limits.maxStorageBufferRange), V.props.limits.minStorageBufferOffsetAlignment, out, VK_LAG_STORAGE))) return 1;
        if (rw) {
            if (record) { vk_gpu_write_record(addr, n); reason = NVN_DIRTY_LAG_GPU | NVN_DIRTY_LAG_WRITABLE; }
            else vk_gpu_write(addr, n);
        }
        if (!R.open) VK_REC_DO(VK_REC_S, vk_ub_touch(addr, n));
        else if (VK_REC_VALUE(VK_REC_S, vk_ub_pending(addr, n))) RED(4, "storage buffer %#llx overlaps a uniform update logged inside the pass: it reads the bytes from before", (unsigned long long)addr);
    }
    VkDeviceSize align = storage ? V.props.limits.minStorageBufferOffsetAlignment : V.props.limits.minUniformBufferOffsetAlignment;
    uint64_t lim = storage ? V.props.limits.maxStorageBufferRange : V.props.limits.maxUniformBufferRange;
    if (!size || size > lim) size = lim;
    if (off % align) {   /* not aligned for the device: a copy in this submission's memory (uniforms only change at submission boundaries) */
        uint64_t n = MINV(size, avail);
        VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, align, &tb, &to);
        memcpy(p, (const void *)(uintptr_t)addr, n);
        if (storage) RED(4, "storage buffer at %#llx is not %llu-aligned: bound as a copy (GPU writes to it are lost)", (unsigned long long)addr, (unsigned long long)align);
        *out = (VkDescriptorBufferInfo){tb, to, n}; return 1;
    }
    *out = (VkDescriptorBufferInfo){b, off, MINV(size, avail)};   /* clamped to the pool's buffer */
    if (record && reason) { record->lag_range = *out; record->lag_reason = reason; }
    return 1;
}
static VkBuffer g_zero_ubo; static VkDeviceSize g_zero_ubo_off;
static void zero_buffer(VkDescriptorBufferInfo *out, size_t size) {
    VkBuffer b; VkDeviceSize o; void *p = vk_transient(size, V.props.limits.minUniformBufferOffsetAlignment, &b, &o); memset(p, 0, size);
    *out = (VkDescriptorBufferInfo){b, o, size};
}
/* Small uniform blocks the backend fills per draw (driver banks, support buffer): a block with the same bytes as the
   last one of its kind in this submission reuses that copy, so the descriptor sets of consecutive draws can match.
   The transient bytes are immutable until slot retirement; the seal serial expires this pointer before slot reuse. */
static void block_desc(int key, const void *data, size_t n, VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub; size_t n; uint8_t *bytes; VkDescriptorBufferInfo d; } last[32];
    uint64_t sub = vk_submit_serial();
    static int off = -1; if (off < 0) off = vk_off("block");
    if (!off && key >= 0 && key < 32 && last[key].sub == sub && last[key].n == n && !memcmp(last[key].bytes, data, n)) { *out = last[key].d; return; }
    VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memcpy(p, data, n);
    *out = (VkDescriptorBufferInfo){tb, to, n};
    if (key >= 0 && key < 32) { last[key].bytes = p; last[key].n = n; last[key].sub = sub; last[key].d = *out; }
}
/* Most draws use the same generated blocks. Check their semantic inputs before filling / comparing 1.4--2.5 KiB:
   bind hooks cover every bank input, while support dimensions and compute grid are checked here. On a changed input
   block_desc still compares the bytes, preserving reuse when several state changes cancel before the next draw. */
static void generated_desc(int kind, int stage, const uint32_t grid[3], VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub, revision; uint32_t w, h, grid[3]; VkDescriptorBufferInfo d; } last[13];
    int support = kind == RB_SUPPORT, bank = kind == RB_DRV2, key = support ? 0 : 1 + bank * 6 + stage;
    uint64_t sub = vk_submit_serial(), revision = support ? 0 : g_driver_revision[bank][stage];
    int compute = !support && !bank && stage == 5;
    uint32_t g[3] = {0}; if (compute && grid) memcpy(g, grid, sizeof g);
    static int off = -1; if (off < 0) off = vk_off("block");
    if (!off && last[key].sub == sub && last[key].revision == revision &&
        (!support || (last[key].w == R.w && last[key].h == R.h)) && (!compute || !memcmp(last[key].grid, g, sizeof g))) { *out = last[key].d; return; }
    uint8_t blk[0xA00]; size_t n = support ? 1392 : sizeof blk;
    if (support) support_fill(blk, R.w, R.h); else driver_bank_fill(blk, bank ? 2 : 0, stage, grid);
    block_desc(key, blk, n, out);
    last[key].sub = sub; last[key].revision = revision; last[key].w = R.w; last[key].h = R.h; memcpy(last[key].grid, g, sizeof g); last[key].d = *out;
}
/* References are immutable in the submission's arena. Reuse up to 64 distinct
 * reference words, including non-consecutive draws and hash collisions. */
static void alpha_desc(VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub; uint32_t bits; VkDescriptorBufferInfo d; } memo[64];
    uint32_t bits; memcpy(&bits,&S.alpha_ref,4);
    uint64_t sub=vk_submit_serial();
    unsigned index=(bits*2654435761u)>>26;
    for (unsigned probe=0;probe<63 && memo[index].sub==sub && memo[index].bits!=bits;probe++)
        index=(index+1)&63u;
    if (memo[index].sub!=sub || memo[index].bits!=bits) {
        VkBuffer buffer; VkDeviceSize offset;
        float block[4]={S.alpha_ref,0,0,0};
        void *bytes=vk_transient(sizeof block,V.props.limits.minUniformBufferOffsetAlignment,&buffer,&offset);
        memcpy(bytes,block,sizeof block);
        memo[index].sub=sub; memo[index].bits=bits;
        memo[index].d=(VkDescriptorBufferInfo){buffer,offset,sizeof block};
    }
    *out=memo[index].d;
}
/* Descriptor sets already written in this submission, by their contents. Two independent 64-bit hashes, layout and
   count identify a set; no retained info arrays to compare, copy or reallocate. Pass dumps read the live infos below. */
enum { DS_CACHE = 2048 };
static struct { uint64_t sub, hash[2]; VkDescriptorSetLayout dsl; size_t n; VkDescriptorSet set; } g_dsc[DS_CACHE];
static struct { uint64_t sub; VkPipelineLayout layout; VkDescriptorSet set; } g_bound_set[2][6];   /* [graphics, compute][set index] */
static void descriptor_unbind(void) { memset(g_bound_set, 0, sizeof g_bound_set); }
static uint64_t g_stat_sets_new, g_stat_sets_reused;
/* Hash both identities in one word-wise pass over the zero-filled infos. The second uses add/multiply/rotate rather
   than the first's xor/multiply/shift, not just another seed of the same hash. */
static void hash_words(const void *p, size_t n, uint64_t h[2]) {
    const uint8_t *b = p; size_t i = 0; uint64_t a = h[0], c = h[1];
    for (; i + 8 <= n; i += 8) {
        uint64_t w; memcpy(&w, b + i, 8);
        a = (a ^ w) * 0x9e3779b97f4a7c15ull; a ^= a >> 32;
        c = (c + w) * 0xc2b2ae3d27d4eb4full; c = c << 31 | c >> 33;
    }
    for (; i < n; i++) { a = (a ^ b[i]) * 0x100000001b3ull; c = (c + b[i]) * 0xc2b2ae3d27d4eb4full; c = c << 31 | c >> 33; }
    h[0] = a; h[1] = c;
}
/* Exact shader-used binding values. Every kind uses at most two words; write
   directly to the destination instead of returning/copying a larger zero-padded key. */
typedef struct { uint64_t value[2]; } record_binding;
static void record_binding_of(const vk_bind *b, int stage, record_binding *r) {
    *r = (record_binding){{0}};
    if (b->kind == RB_SUPPORT || b->kind == RB_C1) return;
    if (b->kind == RB_ALPHA) { uint32_t bits; memcpy(&bits,&S.alpha_ref,4); r->value[0]=bits; return; }
    if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        if (b->kind == RB_DRV0 || b->kind == RB_DRV2) r->value[0] = g_driver_revision[b->kind == RB_DRV2][stage];
        else if (b->reg >= 0 && b->reg < 16) {
            const p_bind_buf *u = b->kind == RB_SBUF ? &S.ssbo[stage][b->reg] : &S.ubo[stage][b->reg];
            r->value[0] = u->addr; r->value[1] = u->size;
        }
    } else {
        uint64_t texture = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) :
            (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        r->value[0] = texture;
        if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) r->value[0] = b->reg >= 0 && b->reg < 128 ?
            (b->sreg == -2 ? S.tex[stage][b->reg] : S.samp[stage][b->reg]) >> 20 : 0;
        else if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            r->value[1] = b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] >> 20 : texture >> 20;
    }
}
#ifdef TOTK_VK_TESTS
int vk_draw_test_binding_bench(void) {
    enum { NB = 32, NDRAWS = 200000 };
    vk_bind bindings[NB] = {{0}}; record_binding saved[NB];
    g_driver_revision[0][1] = 17;
    int ok = 1;
    for (int i = 0; i < NB; i++) {
        S.ubo[1][i & 15] = (p_bind_buf){1, i & 15, 0x10000u + (unsigned)(i & 15) * 256u, 256};
        S.ssbo[1][i & 15] = (p_bind_buf){1, i & 15, 0x20000u + (unsigned)(i & 15) * 512u, 512};
        S.tex[1][i] = (uint64_t)(i + 1) | (uint64_t)(i + 3) << 20;
        S.samp[1][i] = (uint64_t)(i + 5) << 20;
        S.img[1][i] = (uint64_t)(i + 7);
    }
    for (int i = 0; i < NB; i++) {
        vk_bind *b = &bindings[i]; b->reg = (int16_t)i; b->count = 1;
        uint64_t first = 0, second = 0;
        switch (i & 7) {
            case 0: b->kind = RB_SUPPORT; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case 1: b->kind = RB_C1; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case 2: b->kind = RB_DRV0; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; first = 17; break;
            case 3: b->kind = RB_CBUF; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b->reg &= 15; first = 0x10000u + b->reg * 256u; second = 256; break;
            case 4: b->kind = RB_SBUF; b->dtype = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b->reg &= 15; first = 0x20000u + b->reg * 512u; second = 512; break;
            case 5: b->kind = RB_SAMP; b->dtype = VK_DESCRIPTOR_TYPE_SAMPLER; first = i + 5; break;
            case 6: b->kind = RB_TEX; b->dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b->sreg = (int16_t)i; first = S.tex[1][i]; second = i + 5; break;
            case 7: b->kind = RB_IMG; b->dtype = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; first = i + 7; break;
        }
        record_binding_of(b, 1, &saved[i]);
        ok &= saved[i].value[0] == first && saved[i].value[1] == second;
    }
    uint64_t same = 0, start = host_now_ns();
    for (int draw = 0; draw < NDRAWS; draw++) {
        /* NVN state may change between draws; prevent the fixture from hoisting immutable reads. */
        __asm__ volatile("" ::: "memory");
        for (int i = 0; i < NB; i++) {
            record_binding value; record_binding_of(&bindings[i], 1, &value);
            same += !memcmp(&saved[i], &value, sizeof value);
        }
    }
    printf("record-binding-bench: %.2f ns/draw (%d bindings), key_bytes=%zu, matches=%llu\n",
        (double)(host_now_ns() - start) / NDRAWS, NB, sizeof(record_binding), (unsigned long long)same);
    ok &= same == (uint64_t)NDRAWS * NB;
    printf("%s: exact generated/uniform/storage/texture/sampler/image binding values\n", ok ? "ok" : "FAIL");
    return !ok;
}
#endif
static int record_uniforms_unchanged(vk_shader *sh, int stage, uint64_t revision) {
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        /* Read-only storage can acquire additional frozen-content dependencies (frame-lag integration). */
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return 0;
        if (b->dtype != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->reg < 0 || b->reg >= 16 ||
            (b->kind != RB_CBUF && b->kind != RB_UBO_PTR)) continue;
        const p_bind_buf *u = &S.ubo[stage][b->reg];
        uint64_t range = MINV(u->size ? u->size : 0x10000, V.props.limits.maxUniformBufferRange);
        if (u->addr && !vk_record_uniform_unchanged(revision, u->addr, range)) return 0;
    }
    return 1;
}
/* TOTK_VK_FEEDBACK_CUT=1: a pass that samples the image it renders to (a mip chain generated level by level) gets a
   sampled view without the attached level. Feedback through a view that includes the attachment's level is undefined
   in Vulkan even if the shader only reads other levels; Adreno shows it as NaN blotches. Only views whose base level
   is below the attached level are cut, to [base, attached), keeping LOD numbering; a view based at the attached level
   is left alone (counted). Sampled views only; storage images are the game's own explicit read/write. */
static VkImageView feedback_cut(vk_tex *x, VkImageView view) {
    VkImage image = vk_tex_image(x); int attached = 0;
    for (unsigned a = 0; a < 9 && !attached; a++) {
        vk_tex *t = a < 8 ? (a < R.ncolor ? R.ctex[a] : NULL) : R.dtex;
        attached = t && vk_tex_image(t) == image;
    }
    if (!attached) return view;
    vk_canvas_image identity; VkImageSubresourceRange range;
    if (!vk_tex_view_identity(x, view, &identity, &range)) return view;
    uint32_t keep = range.levelCount; int is3d = vk_tex_is3d(x);
    for (unsigned a = 0; a < 9; a++) {
        vk_tex *t = a < 8 ? (a < R.ncolor ? R.ctex[a] : NULL) : R.dtex;
        if (!t || vk_tex_image(t) != image) continue;
        uint32_t level = a < 8 ? R.clevel[a] : R.dlevel, layer = a < 8 ? R.clayer[a] : R.dlayer;
        if (level < range.baseMipLevel || level >= range.baseMipLevel + range.levelCount) continue;
        if (!is3d && (layer < range.baseArrayLayer || layer >= range.baseArrayLayer + range.layerCount)) continue;
        if (level - range.baseMipLevel < keep) keep = level - range.baseMipLevel;
    }
    if (keep == range.levelCount) return view;
    if (!keep) { g_feedback_uncut++; return view; }
    VkImageView cut = vk_tex_view_levels(x, view, keep);
    if (cut != view) g_feedback_cuts++; else g_feedback_uncut++;
    return cut;
}
/* Freeze only stream-position decisions. Descriptor construction, range resolution and zero bindings are worker work. */
static VkImageView native_texture_by_id(int tid, int *want, vk_tex **object) {
    VkImageView view = vk_tex_by_id(tid, want, object);
    if (g_native_sample && object && *object == g_native_sample && *want != -2) {
        vk_treg reg;
        if (vk_treg_get(tid, &reg)) {
            VkImageView native = vk_tex_native_view(*object, reg.has_v ? &reg.v : NULL, 0);
            if (native) return native;
        }
    }
    if (__builtin_expect(atomic_load_explicit(&g_feedback_cut, memory_order_relaxed), 0) && R.open && view && object && *object && *want != -2)
        view = feedback_cut(*object, view);
    return view;
}
/* Enumerate descriptor MAY-reads before freeze-memo reuse. The actual view cache supplies exact ranges,
   so a changed registration/serial cannot be mistaken for a new lifetime or the wrong mip/layer. */
static void pso_life_reads(const vk_shader *sh, unsigned stage) {
    if (!pso_life_active()) return;
    uint64_t sub = vk_game_submit_serial();
    if (!R.open || PL.memo_sub != sub || PL.memo_pass != g_stat_passes) {
        PL.nmemo = 0; PL.memo_sub = sub; PL.memo_pass = g_stat_passes;
    }
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
        uint64_t handle = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) :
            (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        if (!handle) continue;
        int want = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? -2 : b->target;
        vk_tex *object = NULL;
        VkImageView view = native_texture_by_id((int)(handle & 0xfffff), &want, &object);
        if (!object) continue;
        vk_canvas_image image; VkImageSubresourceRange range;
        if (!view || !vk_tex_view_identity(object, view, &image, &range)) {
            vk_canvas_image base = vk_tex_canvas_identity(object, 0, 0);
            for (unsigned mip = 0; mip < vk_tex_levels(object) && mip < 32; mip++)
                if (pso_life_match(&base, mip)) { pso_life_unknown("sample-view-identity"); break; }
            continue;
        }
        if (range.levelCount > 32 || range.layerCount > 256 ||
            range.baseMipLevel >= 32 || range.levelCount > 32 - range.baseMipLevel) {
            pso_life_unknown("sample-view-range"); continue;
        }
        const char *access = b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "sampled" : b->rw ? "storage-write" : "storage-read";
        for (unsigned mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; mip++) {
            if (!pso_life_match(&image, mip)) continue;
            if (vk_tex_is3d(object)) { pso_life_unknown("3d-sampled-range"); continue; }
            for (unsigned layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; layer++)
                for (unsigned aspect = 1; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) if (range.aspectMask & aspect) {
                    pso_life_read_key key = {image.image, image.incarnation, mip, layer, aspect, stage, b->binding};
                    int duplicate = 0;
                    if (R.open && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                        for (size_t j = 0; j < PL.nmemo; j++) {
                            const pso_life_read_key *old = &PL.memo[j];
                            if (old->image == key.image && old->incarnation == key.incarnation && old->mip == key.mip &&
                                old->layer == key.layer && old->aspect == key.aspect && old->stage == key.stage &&
                                old->binding == key.binding) { duplicate = 1; break; }
                        }
                        if (!duplicate && PL.nmemo < 2048) PL.memo[PL.nmemo++] = key;
                        else if (!duplicate) pso_life_unknown("read-memo-capacity");
                    }
                    if (duplicate) continue;
                    PL.reads++;
                    LOGV("[pso-life] kind=read frame=%llu sub=%llu pass=%llu image=%#llx incarnation=%llu mip=%u layer=%u aspect=%u "
                         "w=%u h=%u format=%u serial=%llu storage=%#llx stage=%u binding=%u access=%s shader=%s consumer_w=%u consumer_h=%u",
                         (unsigned long long)PL.frame, (unsigned long long)sub, (unsigned long long)g_stat_passes,
                         (unsigned long long)image.image, (unsigned long long)image.incarnation, mip, layer, aspect,
                         PL.w, PL.h, image.format, (unsigned long long)image.serial, (unsigned long long)image.storage,
                         stage, b->binding, access, sh->key, R.open ? R.w : 0, R.open ? R.h : 0);
                }
        }
    }
}
struct freeze_key {
    uint64_t chunk, submission, uniforms, bindings, resources, samplers, cpu_dirty, dirty_epoch;
    const vk_record_spans *spans; vk_shader *shader; uint32_t width, height, stage; vk_tex *native_sample;
    uint64_t feedback_pass;   /* TOTK_VK_FEEDBACK_CUT: views depend on the open pass's attachments */
    VkImage native_image;
};
static struct { struct freeze_key key; __typeof__(g_record_draw->sets[0]) set; record_binding *bindings; int cap, valid; } g_freeze_memo[128];
#include "vk_xlate_draw_stats.h"
/* Constant-specialized entry points keep all diagnostic work out of the normal binding walk. */
static inline __attribute__((always_inline)) int record_stage_impl(vk_shader *sh, int stage, uint32_t set, int stats) {
    static int c1off = -1; if (c1off < 0) c1off = vk_off("block");
    struct freeze_key key;
    memset(&key, 0, sizeof key); key.chunk = vk_record_chunk_id(); key.submission = vk_submit_serial();
    key.uniforms = vk_record_uniform_revision(); key.bindings = g_record_bind_revision[stage];
    key.resources = vk_treg_revision(); key.samplers = vk_record_sampler_revision();
    /* A freeze hit bypasses the core copy memo. CPU writes through any physical alias must make it call
       the core again; a dirty-unit birth is only lifetime identity, never a content revision. */
    if (vk_lag_capture_on()) { key.cpu_dirty = nvn_dirty_now(); key.dirty_epoch = nvn_dirty_lag_epoch(); }
    key.spans = g_record_draw->spans; key.shader = sh; key.width = R.w; key.height = R.h; key.stage = (uint32_t)stage;
    key.native_sample = g_native_sample; if (feedback_cut_refresh()) key.feedback_pass = g_stat_passes;
    if (g_native_sample) key.native_image = vk_tex_native_image(g_native_sample);
    __typeof__(g_freeze_memo[0]) *last = &g_freeze_memo[((uintptr_t)sh >> 4) % 128];
    unsigned causes = 0;
    if (!c1off && last->valid && last->key.shader == sh && last->key.chunk == key.chunk &&
        last->key.submission == key.submission && last->key.stage == key.stage) {
        if (last->key.bindings != key.bindings) {
            int same = 1;
            for (int i = 0; i < sh->nb; i++) { record_binding value; record_binding_of(&sh->b[i], stage, &value);
                if (memcmp(&last->bindings[i], &value, sizeof value)) { same = 0; break; } }
            if (same) { last->key.bindings = key.bindings; vk_record_memo_saved(VK_REC_M_BINDING); }
        }
        if (last->key.uniforms != key.uniforms && record_uniforms_unchanged(sh, stage, last->key.uniforms)) {
            last->key.uniforms = key.uniforms; vk_record_memo_saved(VK_REC_M_UNIFORM);
        }
        if (!memcmp(&last->key, &key, sizeof key)) {
            g_record_draw->sets[set] = last->set; g_record_freeze_hits++;
            if (stats) xlate_stage(sh, stage, &key, 1, 0, NULL);
            return 1;
        }
    }
    if (c1off) causes |= 1u << VK_REC_M_DISABLED;
    else if (!last->valid) causes |= 1u << VK_REC_M_COLD;
    else {
        if (last->key.uniforms != key.uniforms || last->key.cpu_dirty != key.cpu_dirty ||
            last->key.dirty_epoch != key.dirty_epoch) causes |= 1u << VK_REC_M_UNIFORM;
        if (last->key.bindings != key.bindings) causes |= 1u << VK_REC_M_BINDING;
        if (last->key.spans != key.spans) causes |= 1u << VK_REC_M_SPAN;
        if (last->key.chunk != key.chunk) causes |= 1u << VK_REC_M_CHUNK;
        if (last->key.submission != key.submission) causes |= 1u << VK_REC_M_SUBMISSION;
        if (last->key.shader != key.shader) causes |= 1u << VK_REC_M_SHADER;
        if (last->key.resources != key.resources) causes |= 1u << VK_REC_M_REGISTRY;
        if (last->key.samplers != key.samplers) causes |= 1u << VK_REC_M_SAMPLER;
        if (last->key.width != key.width || last->key.height != key.height) causes |= 1u << VK_REC_M_DIMENSIONS;
    }
    if (stats) xlate_stage(sh, stage, &key, 0, c1off, NULL);
    vk_record_memo_miss(0, causes);
    g_record_freeze_misses++;
    int cacheable = !c1off;
    uint32_t count = 0; for (int i = 0; i < sh->nb; i++) { count += sh->b[i].count; if (sh->b[i].rw) cacheable = 0; }
    uint64_t unwatched = vk_lag_copy_unwatched();
    vk_record_descriptor *input = vk_record_descriptors(g_record_draw, set, sh->dsl, count);
    for (int i = 0, k = 0; i < sh->nb; i++) for (int e = 0; e < sh->b[i].count; e++, k++) {
        const vk_bind *b = &sh->b[i]; vk_record_descriptor *d = &input[k];
        d->binding = b->binding; d->element = (uint32_t)e; d->type = (VkDescriptorType)b->dtype;
        int storage = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (storage || b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (b->kind == RB_ALPHA) { alpha_desc(&d->b); continue; }
            if (b->kind == RB_SUPPORT || b->kind == RB_DRV0 || b->kind == RB_DRV2) { generated_desc(b->kind, stage, NULL, &d->b); continue; }
            if (b->kind == RB_C1) {
                uint64_t sub = vk_submit_serial();
                if (c1off || sh->c1_sub != sub) {
                    size_t n = (sh->c1_len + 15) & ~(size_t)15; if (!n) n = 16;
                    VkBuffer buffer; VkDeviceSize offset; void *bytes = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &buffer, &offset);
                    memset(bytes, 0, n); if (sh->c1_len) memcpy(bytes, sh->c1, sh->c1_len);
                    sh->c1_d = (VkDescriptorBufferInfo){buffer, offset, n}; sh->c1_sub = sub;
                }
                d->b = sh->c1_d; continue;
            }
            uint64_t address = 0, size = 0;
            if (b->reg >= 0 && b->reg < 16) {
                if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { address = S.ubo[stage][b->reg].addr; size = S.ubo[stage][b->reg].size; }
                else if (b->kind == RB_SBUF) { address = S.ssbo[stage][b->reg].addr; size = S.ssbo[stage][b->reg].size; }
                else if (b->kind == RB_SBUF_UBO) { const p_bind_buf *u = &S.ubo[stage][b->reg]; if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_read_record(u->addr + b->off, &address, 8); }
            }
            if (!storage) {
                uint64_t range = MINV(size ? size : 0x10000, V.props.limits.maxUniformBufferRange);
                if (address && !vk_lag_uniform_read(address, range, V.props.limits.minUniformBufferOffsetAlignment, &d->b) &&
                    !vk_ub_snapshot_descriptor(address, range, &d->b)) ubo_desc(address, size, &d->b, 0, 0, b->extent, d);
            } else if (address) {
                /* Lag registration/capture belongs to the walk, once per actual submission (freeze key above).
                   Wait mode keeps a live descriptor; copy mode returns the immutable arena override. */
                if (vk_lag_capture_on() || !vk_record_storage_aligned(g_record_draw->spans, address)) ubo_desc(address, size, &d->b, 1, b->rw, b->extent, d);
                else {
                    d->raw.address = address; d->raw.size = size;
                    if (b->rw) { VkBuffer buffer; uint64_t offset, available;
                        if (vk_record_resolve(g_record_draw->spans, address, 1, &buffer, &offset, &available, NULL))
                            vk_gpu_write(address, MINV(size ? size : available, available));
                    }
                }
            }
            continue;
        }
        uint64_t texture = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) : (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            d->t = texture ? vk_tbuf_by_id_record((int)(texture & 0xfffff), b->rw, &d->lag_range, &d->lag_reason) : VK_NULL_HANDLE;
            if (!d->t) d->t = vk_dummy_tbuf(); continue;
        }
        if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) {
            int sid = b->kind == RB_SAMP && b->reg >= 0 && b->reg < 128 ? (int)((b->sreg == -2 ? S.tex[stage][b->reg] : S.samp[stage][b->reg]) >> 20) : 0;
            d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
            if (!d->i.sampler) d->i.sampler = vk_dummy_sampler(); continue;
        }
        int want = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? -2 : b->target; vk_tex *object = NULL;
        d->i.imageView = texture ? native_texture_by_id((int)(texture & 0xfffff), &want, &object) : VK_NULL_HANDLE;
        if (stats) xlate_texture_view(texture, object, d->i.imageView);
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && object) vk_tex_mark_gpu_written(object);
        if (!d->i.imageView) d->i.imageView = vk_dummy_view(b->target, b->is_int);
        if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            int sid = b->reg >= 0 && b->reg < 128 ? (int)((b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] : texture) >> 20) : 0;
            d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
            if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
        }
    }
    if (stats) xlate_selected(sh, stage, input);
    /* A positive copy without complete CPU write-watch authority cannot be reused by a stage freeze,
       even when every observable revision is unchanged. The core reports those selections without
       requiring another alias scan here. */
    if (vk_lag_copy_unwatched() != unwatched) { cacheable = 0; last->valid = 0; }
    if (stats) xlate_uncacheable(sh, stage, cacheable, vk_lag_copy_unwatched() != unwatched);
    if (R.open && count) g_record_draw->sets[set].update = vk_record_template(sh->dsl, input, count);
    if (R.open && cacheable) {
        if (last->cap < sh->nb) { last->cap = sh->nb; last->bindings = realloc(last->bindings, (size_t)last->cap * sizeof *last->bindings); }
        for (int i = 0; i < sh->nb; i++) record_binding_of(&sh->b[i], stage, &last->bindings[i]);
        last->key = key; last->set = g_record_draw->sets[set]; last->valid = 1;
    } else if (!cacheable) vk_record_memo_miss(0, 1u << VK_REC_M_DISABLED);
    return R.open;
}
/* Fill and bind one stage's descriptor set; stage: NVN stage. Returns 0 when a binding cannot be satisfied. */
static inline __attribute__((always_inline)) int bind_stage_body(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3], int stats) {
    if (!sh->nb) return 1;
    (void)feedback_cut_refresh();   /* a live TOTK_VK_FEEDBACK_CUT takes effect at this descriptor build */
    if (__builtin_expect(PL.enabled, 0)) pso_life_reads(sh, (unsigned)stage);
    if (g_record_draw) return R.open ? record_stage_impl(sh, stage, set_index, stats) : 0;
    size_t total = 0; for (int i = 0; i < sh->nb; i++) total += sh->b[i].count;
    VkImageView replay_source = VK_NULL_HANDLE;
    static dinfo *info; static size_t capi;
    if (total > capi) { capi = total + 64; info = realloc(info, capi * sizeof *info); }
    memset(info, 0, total * sizeof *info);
    int k = 0;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        dinfo *in = &info[k]; k += b->count;
        for (int e = 0; e < b->count; e++) {
            dinfo *d = &in[e];
            /* buffers */
            if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                int storage = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ok = 0;
                if (b->kind == RB_ALPHA) { alpha_desc(&d->b); continue; }
                if (b->kind == RB_SUPPORT) { generated_desc(b->kind, stage, grid, &d->b); ok = 1; }
                else if (b->kind == RB_CBUF && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ubo[stage][b->reg].addr, S.ubo[stage][b->reg].size, &d->b, storage, storage && b->rw, b->extent, NULL);
                else if (b->kind == RB_C1) {
                    uint64_t sub = vk_submit_serial();
                    static int c1off = -1; if (c1off < 0) c1off = vk_off("block");
                    if (c1off || sh->c1_sub != sub) { size_t n = (sh->c1_len + 15) & ~(size_t)15; if (!n) n = 16; VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memset(p, 0, n); if (sh->c1_len) memcpy(p, sh->c1, sh->c1_len); sh->c1_d = (VkDescriptorBufferInfo){tb, to, n}; sh->c1_sub = sub; }
                    d->b = sh->c1_d; ok = 1;
                }
                else if (b->kind == RB_DRV0 || b->kind == RB_DRV2) { generated_desc(b->kind, stage, grid, &d->b); ok = 1; }
                else if (b->kind == RB_SBUF && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ssbo[stage][b->reg].addr, S.ssbo[stage][b->reg].size, &d->b, storage, storage && b->rw, b->extent, NULL);
                else if (b->kind == RB_UBO_PTR && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ubo[stage][b->reg].addr, S.ubo[stage][b->reg].size, &d->b, storage, storage && b->rw, b->extent, NULL);
                else if (b->kind == RB_SBUF_UBO && b->reg >= 0 && b->reg < 16) {
                    const p_bind_buf *u = &S.ubo[stage][b->reg]; uint64_t va = 0;
                    if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_read(u->addr + b->off, &va, 8);   /* with this submission's inline updates */
                    ok = va && ubo_desc(va, 0, &d->b, storage, storage && b->rw, b->extent, NULL);
                    if (!ok) RED(8, "storage buffer pointer in uniform slot %d+%u (%#llx) is outside the pools", b->reg, b->off, (unsigned long long)va);
                }
                if (!ok) { zero_buffer(&d->b, storage ? 256 : 256); if (b->kind != RB_SBUF_UBO) { g_pzero++; g_stat_zero_buf++; } }
                else if (probe_on() && (b->kind == RB_CBUF || b->kind == RB_SBUF || b->kind == RB_UBO_PTR || b->kind == RB_SBUF_UBO) && b->reg >= 0 && b->reg < 16)
                    pass_buf_note(stage, b->reg + (b->kind == RB_SBUF ? 16 : 0), storage, b->kind == RB_SBUF ? S.ssbo[stage][b->reg].addr : S.ubo[stage][b->reg].addr, &d->b);
                continue;
            }
            /* texel buffers */
            if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int slot = b->kind == RB_IMG ? b->reg : b->reg;
                uint64_t h = slot >= 0 ? (b->kind == RB_IMG ? (slot < 32 ? S.img[stage][slot] : 0) : (slot < 128 ? S.tex[stage][slot] : 0)) : 0;
                d->t = h ? vk_tbuf_by_id((int)(h & 0xfffff), b->rw) : VK_NULL_HANDLE;
                if (!d->t) d->t = vk_dummy_tbuf();
                continue;
            }
            /* images and samplers */
            int tslot = b->reg, target = b->target;
            if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) {
                int sid = 0;
                if (b->kind == RB_SAMP && tslot >= 0 && tslot < 128) sid = (int)((b->sreg == -2 ? S.tex[stage][tslot] : S.samp[stage][tslot]) >> 20);
                d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
                if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
                continue;
            }
            VkImageView view = VK_NULL_HANDLE; vk_tex *x = NULL;
            if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                if (tslot >= 0 && tslot < 32 && S.img[stage][tslot]) { int want = -2; view = native_texture_by_id((int)(S.img[stage][tslot] & 0xfffff), &want, &x); }
                if (stats && tslot >= 0 && tslot < 32) xlate_texture_view(S.img[stage][tslot], x, view);
                if (x) { vk_tex_mark_gpu_written(x); probe_write_note('X', x, NULL, 0); }
                d->i.imageView = view ? view : vk_dummy_view(target, b->is_int); d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                continue;
            }
            if (tslot >= 0 && tslot < 128 && S.tex[stage][tslot]) {
                int want = target; view = native_texture_by_id((int)(S.tex[stage][tslot] & 0xfffff), &want, &x);
                if (stats) xlate_texture_view(S.tex[stage][tslot], x, view);
                if (!view) { g_pdummy++; uint64_t n = ++g_stat_dummy_tex;
                    if (n <= 16) LOGV("texture id %d (stage %d slot %d, shader %.16s) resolves to nothing: an all-zero dummy is bound (%llu so far)", (int)(S.tex[stage][tslot] & 0xfffff), stage, tslot, sh->key, (unsigned long long)n); }
                else if (x && R.open) {
                    uint64_t im = (uint64_t)(uintptr_t)vk_tex_image(x); int k = 0; while (k < g_npin && g_pin[k].img != im) k++;
                    if (k == g_npin && g_npin < 16) g_pin[g_npin++] = (probe_in){im, vk_tex_width(x, 0), vk_tex_height(x, 0), vk_tex_nvn_format(x), x};
                }
            }
            if (stage == 1 && R.open && !g_pkey[0]) snprintf(g_pkey, sizeof g_pkey, "%.16s", sh->key);
            if (vk_fg_extract_active && FGX.record && FGX.record->final &&
                fg_extract_canvas(x,0,0))
                replay_source = view;
            d->i.imageView = view ? view : vk_dummy_view(target, b->is_int); d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                int sid = 0;
                if (tslot >= 0 && tslot < 128) sid = (int)((b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] : S.tex[stage][tslot]) >> 20);
                d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
                if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
            }
            if (b->count > 1 && e == 0) RED(4, "texture array binding (%u elements, slot %d): every element shows the first texture", b->count, tslot);
        }
        (void)g_zero_ubo; (void)g_zero_ubo_off;
    }
    if (stats) xlate_immediate_stage(sh, stage, info);
    if (g_pd_cap && stage == 0) {
        g_pd_nubo = 0;
        for (int i = 0, kk = 0; i < sh->nb; kk += sh->b[i].count, i++)
            if (sh->b[i].dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && g_pd_nubo < 32) g_pd_ubo[g_pd_nubo++] = (__typeof__(g_pd_ubo[0])){sh->b[i].binding, sh->b[i].kind, sh->b[i].reg, info[kk].b};
    }
    uint64_t sub = vk_submit_serial(), h[2] = {0xcbf29ce484222325ull, 0x6a09e667f3bcc909ull};
    hash_words(&sh->dsl, sizeof sh->dsl, h); hash_words(info, total * sizeof *info, h);
    unsigned slot = (unsigned)(h[0] % DS_CACHE);
    VkDescriptorSet set;
    static int dsoff = -1; if (dsoff < 0) dsoff = vk_off("ds");
    if (!dsoff && g_dsc[slot].sub == sub && g_dsc[slot].hash[0] == h[0] && g_dsc[slot].hash[1] == h[1] && g_dsc[slot].dsl == sh->dsl && g_dsc[slot].n == total) {
        set = g_dsc[slot].set; g_stat_sets_reused++;
    } else {
        set = vk_alloc_set(sh->dsl);
        /* Writes are only consumed on a cache miss; hits need only the resolved infos (also used by pass dumps). */
        static VkWriteDescriptorSet *w; static size_t capw;
        if ((size_t)sh->nb > capw) { capw = (size_t)sh->nb + 16; w = realloc(w, capw * sizeof *w); }
        for (int i = 0, kk = 0; i < sh->nb; i++) {
            const vk_bind *b = &sh->b[i]; dinfo *in = &info[kk]; kk += b->count;
            w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->binding, .descriptorCount = b->count, .descriptorType = (VkDescriptorType)b->dtype,
                .pBufferInfo = &in->b, .pImageInfo = &in->i, .pTexelBufferView = &in->t};
        }
        vkUpdateDescriptorSets(V.dev, (uint32_t)sh->nb, w, 0, NULL);
        g_dsc[slot].sub = sub; g_dsc[slot].hash[0] = h[0]; g_dsc[slot].hash[1] = h[1]; g_dsc[slot].dsl = sh->dsl; g_dsc[slot].n = total; g_dsc[slot].set = set;
        g_stat_sets_new++;
    }
    if (vk_fg_extract_active && FGX.record && set_index < 2) {
        VkDescriptorSet replay_set = set;
        if (replay_source) {
            replay_set = vk_alloc_set(sh->dsl);
            for (int i = 0, first = 0; i < sh->nb; i++) {
                const vk_bind *b = &sh->b[i]; dinfo *in = &info[first]; first += b->count;
                if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                    for (int e = 0; e < b->count; e++)
                        if (in[e].i.imageView == replay_source) in[e].i.imageView = vk_fg_canvas_view();
                VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet=replay_set, .dstBinding=b->binding, .descriptorCount=b->count,
                    .descriptorType=(VkDescriptorType)b->dtype, .pBufferInfo=&in->b,
                    .pImageInfo=&in->i, .pTexelBufferView=&in->t};
                vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
            }
            FGX.record->final = 2; /* exact canvas input was replaced, not a shader-name-only guess */
        }
        if (FGX.record->final &&
                atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed)) {
            for (int i=0,first=0;i<sh->nb;i++) {
                const vk_bind *b=&sh->b[i]; dinfo *in=&info[first]; first+=b->count;
                VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding=b->binding,.descriptorCount=b->count,
                    .descriptorType=(VkDescriptorType)b->dtype,.pBufferInfo=&in->b,
                    .pImageInfo=&in->i,.pTexelBufferView=&in->t};
                if (!vk_fg_terminal_owned_binding(set_index,sh->dsl,&write,b->rw))
                    vk_fg_terminal_owned_fail();
            }
        }
        FGX.record->sets[set_index] = replay_set;
    }
    int bi = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (!dsoff && set_index < 6 && g_bound_set[bi][set_index].sub == sub && g_bound_set[bi][set_index].set == set && g_bound_set[bi][set_index].layout == layout) return 1;
    vkCmdBindDescriptorSets(cb, bp, layout, set_index, 1, &set, 0, NULL);
    if (set_index < 6) { g_bound_set[bi][set_index].sub = sub; g_bound_set[bi][set_index].set = set; g_bound_set[bi][set_index].layout = layout; }
    for (uint32_t j = set_index + 1; j < 6; j++) g_bound_set[bi][j].sub = 0;   /* a later set may have been disturbed */
    return 1;
}
/* Descriptor counters' walker-ns: complete nonempty stage selection/binding, only while timing is enabled. */
static inline __attribute__((always_inline)) int bind_stage_impl(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3], int stats) {
    int timed = __builtin_expect(vk_rec_timing_on, 0) && sh->nb;
    uint64_t start = timed ? host_now_ns() : 0;
    int ok = bind_stage_body(cb, bp, layout, set_index, sh, stage, grid, stats);
    if (timed) vk_record_descriptor_walk(host_now_ns() - start);
    return ok;
}
static int bind_stage(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3]) {
    return bind_stage_impl(cb, bp, layout, set_index, sh, stage, grid, 0);
}

/* ---------- draws ---------- */
static VkBuffer g_quad_ib; static uint32_t g_quad_max;
static void quad_indices(uint32_t quads) {
    if (g_quad_ib && quads <= g_quad_max) return;
    uint32_t n = quads < 65536 ? 65536 : quads;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)n * 24, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer b; VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    vk_alloc a; if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &a)) DIE("quad index buffer");
    VKCHECK(vkBindBufferMemory(V.dev, b, a.mem, a.off));
    uint32_t *ix; VKCHECK(vkMapMemory(V.dev, a.mem, a.off, (VkDeviceSize)n * 24, 0, (void **)&ix));
    for (uint32_t q = 0; q < n; q++) { uint32_t v = q * 4, t[6] = {v, v + 1, v + 2, v, v + 2, v + 3}; memcpy(ix + q * 6, t, 24); }
    vkUnmapMemory(V.dev, a.mem);
    g_quad_ib = b; g_quad_max = n;   /* the old buffer (if any) is left alive: it may be in flight */
}
static void dynamic_state(VkCommandBuffer cb, int prim) {
    /* everything the draw sets dynamically, compared with what this command buffer last set (unchanged: nothing issued) */
    vk_record_dynamic D;
    memset(&D, 0, sizeof D);
    float vx = S.has_vp ? S.vp[0][0] : 0, vy = S.has_vp ? S.vp[0][1] : 0, vw = S.has_vp ? S.vp[0][2] : (float)R.w, vh = S.has_vp ? S.vp[0][3] : (float)R.h;
    if (g_ui_aspect_on) vk_ui_aspect_viewport(&g_ui_aspect, &vx, &vy, &vw, &vh);   /* TOTK_ASPECT: HUD quad about the canvas centre */
    D.vp = (VkViewport){vx, vy + vh, vw, -vh, S.dr[0], S.dr[1]};
    if (D.vp.minDepth < 0) D.vp.minDepth = 0; if (D.vp.maxDepth > 1) D.vp.maxDepth = 1;
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)R.w, y1 = (int32_t)R.h;
    if (S.has_sc) { x0 = MAXV(0, S.sc.x); y0 = MAXV(0, S.sc.y); x1 = MINV(x1, S.sc.x + S.sc.w); y1 = MINV(y1, S.sc.y + S.sc.h); }
    if (g_ui_aspect_on && S.has_sc) {   /* the pane's clip follows its squeezed content */
        vk_ui_aspect_rect(&g_ui_aspect, &x0, &y0, &x1, &y1);
        x0 = MAXV(0, x0); y0 = MAXV(0, y0); x1 = MINV(x1, (int32_t)R.w); y1 = MINV(y1, (int32_t)R.h);
    }
    D.sc = (VkRect2D){{x0, y0}, {(uint32_t)MAXV(0, x1 - x0), (uint32_t)MAXV(0, y1 - y0)}};
    if (R.native) {
        float sx = (float)R.host_w/R.native_w, sy = (float)R.host_h/R.native_h;
        D.vp.x *= sx; D.vp.width *= sx; D.vp.y *= sy; D.vp.height *= sy;
        D.sc = native_rect(D.sc);
    }
    D.bias_en = (S.poly.offset_en & 4) != 0; D.bias[0] = S.po.units; D.bias[1] = S.po.clamp; D.bias[2] = S.po.factor;
    if (R.dfmt == VK_FORMAT_D16_UNORM && d16tie_on()) {   /* see d16tie_on */
        if (!D.bias_en) { D.bias[0] = D.bias[1] = D.bias[2] = 0; D.bias_en = 1; }
        D.bias[0] += D16_NUDGE_UNITS;
    }
    memcpy(D.blend, S.blend_color, sizeof D.blend);
    for (int f = 0; f < 2; f++) { D.cmask[f] = S.st_vmask[f]; D.wmask[f] = S.st_wmask[f]; D.ref[f] = S.st_ref[f]; }
    static int flip = -1; if (flip < 0) { const char *e = getenv("TOTK_VK_WINDING_FLIP"); flip = e && atoi(e); }
    D.cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    D.front = ((S.poly.front == NVN_FRONT_FACE_CCW) != (flip != 0)) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    D.topo = topology(prim);
    int has_d = R.dfmt != VK_FORMAT_UNDEFINED, has_s = R.sfmt != VK_FORMAT_UNDEFINED;
    D.dt = has_d && S.ds.dt; D.dw = has_d && S.ds.dw; D.dc = vk_compare(S.ds.df); D.st = has_s && S.ds.st;
    D.sop[0][0] = vk_stencil_op(S.ds.fail_f); D.sop[0][1] = vk_stencil_op(S.ds.zpass_f); D.sop[0][2] = vk_stencil_op(S.ds.zfail_f); D.sop[0][3] = vk_compare(S.ds.sf_f);
    D.sop[1][0] = vk_stencil_op(S.ds.fail_b); D.sop[1][1] = vk_stencil_op(S.ds.zpass_b); D.sop[1][2] = vk_stencil_op(S.ds.zfail_b); D.sop[1][3] = vk_compare(S.ds.sf_b);
    D.restart = prim == 3 || prim == 5;
    if (g_record_draw) { g_record_draw->dynamic = D; return; }
    if (vk_fg_extract_active && FGX.record) FGX.record->draw.dynamic = D;
    static __typeof__(D) last;
    static int dynoff = -1; if (dynoff < 0) dynoff = vk_off("dyn");
    if (!dynoff && !g_dyn_dirty && !memcmp(&D, &last, sizeof D)) return;
    int all = g_dyn_dirty || dynoff; g_dyn_dirty = 0;
    vk_record_dynamic_emit(cb, &D, &last, all);
}
/* Every stage's writable aliases precede any read-only preparation/capture. Repeated bindings still publish
   producer invalidations: unchanged addresses do not mean unchanged GPU contents. Only mapping lookup is reusable. */
static void storage_touch(int st, const vk_shader *sh, int writable) {
    if (!sh || (writable ? !sh->has_storage_writes : !sh->has_storage_reads)) return;
    int lag = vk_lag_capture_on(), updates = vk_record_updates_pending();
    if (!updates && (!writable || !lag)) return;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (!!b->rw != writable) continue;
        if (writable && b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER && lag) {
            int reg = b->reg;
            uint64_t h = b->kind == RB_IMG ? (reg >= 0 && reg < 32 ? S.img[st][reg] : 0) : (reg >= 0 && reg < 128 ? S.tex[st][reg] : 0);
            if (h) {
                if (vk_record_enabled()) { VkDescriptorBufferInfo live; uint32_t reason; (void)vk_tbuf_by_id_record((int)(h & 0xfffff), 1, &live, &reason); }
                else (void)vk_tbuf_by_id((int)(h & 0xfffff), 1);
            }
        }
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->reg < 0 || b->reg >= 16) continue;
        uint64_t addr = 0, size = 0;
        if (b->kind == RB_SBUF) { addr = S.ssbo[st][b->reg].addr; size = S.ssbo[st][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { addr = S.ubo[st][b->reg].addr; size = S.ubo[st][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) {
            const p_bind_buf *u = &S.ubo[st][b->reg];
            if (u->addr && u->size >= (uint64_t)b->off + 8) {
                if (vk_record_enabled()) vk_ub_read_record(u->addr + b->off, &addr, 8);
                else vk_ub_read(u->addr + b->off, &addr, 8);
            }
        }
        /* A known readonly range with no ordered update needs neither a pool lookup nor materialization.
           Writers must still resolve and invalidate contents on every draw, even with unchanged bindings. */
        uint64_t known = size;
        if (lag && b->extent) known = known ? MINV(known, b->extent) : b->extent;
        if (!writable && addr && known && !vk_ub_pending(addr, known)) continue;
        VkBuffer vb; uint64_t vo, avail;
        if (addr && vk_resolve_for(addr, 1, &vb, &vo, &avail, VK_LOOKUP_TOUCH)) {
            uint64_t n = MINV(size ? size : avail, avail);
            if (lag) {
                if (b->extent) n = MINV(n, b->extent);
                else if (!size) n = vk_lag_bound(addr, n);
            }
            if (writable && lag) {
                if (vk_record_enabled()) vk_gpu_write_record(addr, n);
                else vk_gpu_write(addr, n);
            }
            if (updates) vk_ub_touch(addr, n);
        }
    }
}
static void native_bound_update(unsigned st, int image, unsigned first, unsigned n) {
    for (unsigned i=first;i<first+n;i++) {
        uint64_t handle=image?S.img[st][i]:S.tex[st][i], bit=1ull<<(i&63);
        int candidate=handle && vk_native_descriptor((int)(handle&0xfffff));
        if (image) {
            if (candidate) g_native_bound[st].img|=(uint32_t)bit;
            else g_native_bound[st].img&=~(uint32_t)bit;
        } else {
            if (candidate) g_native_bound[st].tex[i/64]|=bit;
            else g_native_bound[st].tex[i/64]&=~bit;
        }
    }
    if (g_native_bound[st].tex[0] || g_native_bound[st].tex[1] || g_native_bound[st].img) g_native_bound_any|=1u<<st;
    else g_native_bound_any&=~(1u<<st);
}
/* Registration/promotion changes rebuild these tiny masks once, not once per
   draw. Ordinary descriptor changes update only their changed handle slots. */
static void native_bound_sync(void) {
    uint64_t revision=vk_native_descriptor_revision();
    if (revision==g_native_bound_revision) return;
    for (unsigned st=0;st<6;st++) { native_bound_update(st,0,0,S.tex_hi[st]); native_bound_update(st,1,0,32); }
    g_native_bound_revision=revision;
}
/* Resolve reflected reads only when they can touch a promoted backing. In
   particular, ordinary 1x feedback must NOT acquire an extra pass boundary. */
static void native_reads(vk_shader *const sh[6], unsigned first, unsigned end) {
    native_bound_sync();
    if (!g_native_bound_any) return;
    for (unsigned st=first;st<end;st++) {
        if (!sh[st] || !(g_native_bound_any&(1u<<st))) continue;
        if (!(g_native_bound[st].tex[0]&sh[st]->native_tex_mask[0]) &&
            !(g_native_bound[st].tex[1]&sh[st]->native_tex_mask[1]) &&
            !(g_native_bound[st].img&sh[st]->native_img_mask)) continue;
        for (int i=0;i<sh[st]->nb;i++) {
            const vk_bind *b=&sh[st]->b[i];
            if (b->dtype!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                b->dtype!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
            uint64_t handle=b->kind==RB_IMG ? (b->reg>=0 && b->reg<32 ? S.img[st][b->reg] : 0) :
                (b->reg>=0 && b->reg<128 ? S.tex[st][b->reg] : 0);
            if (!handle || !vk_native_descriptor((int)(handle&0xfffff))) continue;
            int want=b->dtype==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?-2:b->target; vk_tex *x=NULL;
            vk_tex_by_id((int)(handle&0xfffff),&want,&x);
            if (x==g_native_sample || !vk_tex_native_backed(x)) continue;
            if (R.open && R.native) for (unsigned a=0;a<R.ncolor;a++)
                if (R.ctex[a] && vk_tex_image(x)==vk_tex_image(R.ctex[a])) {
                    vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER,0); break;
                }
            vk_tex_native_read(x);
            if (b->dtype==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) vk_tex_native_written(x,0);
        }
    }
}
static int canvas_draw_prepare(vk_shader *const sh[6], vk_canvas_image *transfer_source, vk_canvas_image *transfer_window) {
    g_native_sample = NULL; int one = 0, transfer = 0;
    if (R.open && !R.native && !R.canvas && !sh[0]->canvas_ui && (!sh[1] || !sh[1]->canvas_transfer)) {
        g_native_draw_1x=0;
        if (vk_native_ui_enabled()) native_reads(sh,0,2);
        return 0;
    }
    vk_tex *target = S.has_rt && S.rt.n == 1 ?
        (R.open ? R.ctex[0] : vk_tex_get(S.rt.color[0])) : NULL;
    unsigned target_level=target?view_level(&S.rt.cview[0],S.rt.has_cview[0]):0;
    unsigned target_layer=target && S.rt.has_cview[0] && S.rt.cview[0].set_layers?S.rt.cview[0].min_layer:0;
    if (target && !S.rt.depth && !target_level && !target_layer && sh[1] && sh[1]->canvas_transfer && vk_tex_canvas_window(target)) {
        vk_end_rendering_at(VK_PASS_END_ATTACHMENTS,0);
        for (int i = 0; i < sh[1]->nb; i++) {
            const vk_bind *b = &sh[1]->b[i];
            if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->reg < 0 || b->reg >= 128) continue;
            int want = b->target; vk_tex *source = NULL;
            vk_tex_by_id((int)(S.tex[1][b->reg] & 0xfffff), &want, &source);
            if (source && source != target && b->target == 1 &&
                S.has_vp && !S.vp[0][0] && !S.vp[0][1] &&
                S.vp[0][2] == vk_tex_width(source,0) && S.vp[0][3] == vk_tex_height(source,0) &&
                vk_tex_width(source,0) <= vk_tex_width(target,0) && vk_tex_height(source,0) <= vk_tex_height(target,0) &&
                vk_tex_width(source,0) >= 640 && vk_tex_levels(source) == 1 && vk_tex_layers(source) == 1 &&
                !vk_tex_is3d(source) && vk_tex_samples(source) == 1 && vk_tex_aspect(source) == VK_IMAGE_ASPECT_COLOR_BIT) {
                *transfer_source=vk_tex_canvas_identity(source,0,0);
                *transfer_window=vk_tex_canvas_identity(target,0,0);
                vk_canvas_transfer(transfer_source,transfer_window,0); transfer=1;
                if (vk_native_ui_enabled()) {
                    vk_tex_native_promote(source); g_native_sample = source;
                    vk_tex_native_transfer(target,vk_tex_width(source,0),vk_tex_height(source,0));
                }
            }
        }
    }
    if (target) {
        /* Keep composition and ALL pre-UI full-screen work at 1x. One linear
           upscale at the first UI draw is cheaper than rerunning post at 2.25x. */
        vk_canvas_image identity=vk_tex_canvas_identity(target,target_level,target_layer);
        int first_ui = sh[0]->canvas_ui == 2 && vk_canvas_first_ui(&identity,sh[0]->key);
        int snapshot = first_ui && vk_fg_extract_active && !FGX.ui_seen;
        if (snapshot) fg_extract_boundary(target,target_level,target_layer);
        if (first_ui && vk_native_ui_enabled() && !S.rt.depth) vk_tex_native_ui_begin(target);
        if (vk_native_ui_enabled() && sh[1] && vk_tex_native_active(target)) one=sh[1]->native_1x;
        if (snapshot && FGX.captured && !FGX.invalid) {
            uint32_t width=vk_tex_width(target,target_level), height=vk_tex_height(target,target_level);
            if (vk_native_ui_enabled() && !S.rt.depth && !one && !target_level && !target_layer &&
                vk_tex_native_active(target) && vk_tex_native_view(target,S.rt.has_cview[0]?&S.rt.cview[0]:NULL,1)) {
                vk_post_output native;
                if (vk_tex_native_output(target,&native)) { width=native.w; height=native.h; }
            }
            FGX.ui_width = width; FGX.ui_height = height; FGX.ui_format = vk_tex_format(target);
            VkCommandBuffer cb = vk_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
            (void)vk_fg_prepare_canvas(cb,width,height);
            (void)vk_fg_replay_depth(cb,FGX.ui_depth,FGX.ui_depth_level,FGX.ui_depth_layer);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
        }
        if (one) RED(16,"native UI v2: FragCoord.xy/storage shader %.64s executes at 1x",sh[1]->key);
    }
    if (R.open && (one != g_native_draw_1x || (target && R.native != (!one && vk_tex_native_active(target)))))
        vk_end_rendering_at(VK_PASS_END_ATTACHMENTS, 0);
    g_native_draw_1x = one;
    if (g_native_sample) {
        vk_end_rendering_at(VK_PASS_END_COPY, 0);
        if (!vk_tex_native_view(g_native_sample, NULL, 0)) {
            vk_tex_native_read(g_native_sample); g_native_sample = NULL;
        }
    }
    if (vk_native_ui_enabled()) native_reads(sh,0,2);
    return transfer;
}
/* Reflection is immutable. Enumerate only consumed UBO registers, once each, rather than all of both shaders'
   image/sampler/driver-bank descriptors on every draw. Storage writers run before every preparation; with
   TOTK_VK_PREPARE_MASK the dirty masks above keep only "already prepared under these revisions", never contents. */
static void draw_uniform_prepare(vk_shader *const *sh) {
    int on = prepare_mask_live();
    if (on != g_prepare_used) { g_prepare_used = on; g_uniform_prepare_dirty[0] = g_uniform_prepare_dirty[1] = UINT16_MAX; }
    if (on && vk_lag_uniform_prepare_changed()) g_uniform_prepare_dirty[0] = g_uniform_prepare_dirty[1] = UINT16_MAX;
    for (int st = 0; st < 2; st++) {
        unsigned consumed = sh[st] ? sh[st]->uniform_prepare_mask : 0;
        unsigned pending = consumed & (on ? g_uniform_prepare_dirty[st] : UINT16_MAX);
        g_prepare_skips += (unsigned)__builtin_popcount(consumed & ~pending);
        for (unsigned mask = g_prepare_check ? consumed : pending; mask; mask &= mask - 1) {
            unsigned reg = (unsigned)__builtin_ctz(mask), bit = 1u << reg;
            const p_bind_buf *u = &S.ubo[st][reg];
            /* A broad UBO can alias a smaller true producer: materialize its untouched logical neighbours
               before opening rendering, rather than ending the pass later during descriptor setup. */
            uint64_t size = MINV(u->size ? u->size : 0x10000, V.props.limits.maxUniformBufferRange);
            if (pending & bit) {
                if (u->addr) vk_lag_uniform_prepare(u->addr, size);
                g_prepare_calls++;
                g_uniform_prepare_dirty[st] &= (uint16_t)~bit;
            } else {
                if (u->addr) vk_lag_uniform_prepare_check(u->addr, size);
                g_prepare_checks++;
            }
        }
    }
}
static void draw_touch(vk_shader *const *sh, const p_draw *dw, const p_draw_idx *di) {
    for (int st = 0; st < 2; st++) storage_touch(st, sh[st], 1);
    for (int st = 0; st < 2; st++) storage_touch(st, sh[st], 0);
    if (nvn_dirty_lag_vma_pressure()) for (int st = 0; st < 2; st++) for (int i = 0; sh[st] && i < sh[st]->nb; i++) {
        const vk_bind *b = &sh[st]->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        uint64_t texture = b->kind == RB_IMG ? (b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0) :
            (b->reg >= 0 && b->reg < 128 ? S.tex[st][b->reg] : 0);
        /* Baseline texture watches remain active during lag fallback. Realize any changed inherited
           bindings before rendering, not while a worker packet's descriptor array is being filled. */
        int target = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? -2 : b->target;
        if (texture) (void)vk_tex_by_id((int)(texture & 0xfffff), &target, NULL);
    }
    if (!vk_record_updates_pending()) return;
    if (vk_lag_uniform_private()) draw_uniform_prepare(sh);
    uint32_t touched = 0;
    for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) {
        unsigned stream = S.attr[a].stream & 15;
        if (touched >> stream & 1) continue;
        touched |= 1u << stream;
        const NVNbufferRange *v = &S.vbuf[stream];
        if (v->address) vk_ub_touch(v->address, v->size ? v->size : 16);
    }
    if (di) vk_ub_touch(di->ibuf, (uint64_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4));
    (void)dw;
}
#ifdef TOTK_VK_TESTS
/* Isolate reflection enumeration from Vulkan/producer queries. Both sides select the same twelve
   UBO registers; scalar is the former per-draw scan, summary is the production bit enumeration. */
int vk_draw_test_touch_bench(void) {
    enum { N = 128, DRAWS = 200000 };
    vk_bind bindings[N]; memset(bindings, 0, sizeof bindings);
    for (unsigned i = 0; i < N; i++) {
        bindings[i].dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].reg = (int16_t)(i % 128);
    }
    for (unsigned i = 0; i < 12; i++) {
        bindings[i * 9].dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i * 9].kind = RB_CBUF; bindings[i * 9].reg = (int16_t)i;
    }
    vk_shader sh = {.nb = N, .b = bindings, .uniform_prepare_mask = 0xfff};
    uint64_t ns[2], checksums[2] = {0};
    for (unsigned summary = 0; summary < 2; summary++) {
        uint64_t start = host_now_ns();
        for (unsigned draw = 0; draw < DRAWS; draw++) {
            __asm__ volatile("" : : "r"(&sh) : "memory"); /* identical inputs; forbid loop-invariant hoisting */
            for (unsigned st = 0; st < 2; st++) {
                if (summary) {
                    for (unsigned mask = sh.uniform_prepare_mask; mask; mask &= mask - 1)
                        checksums[summary] += 1u << __builtin_ctz(mask);
                } else for (int i = 0; i < sh.nb; i++) {
                    const vk_bind *b = &sh.b[i];
                    if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->reg >= 0 && b->reg < 16 &&
                        (b->kind == RB_CBUF || b->kind == RB_UBO_PTR)) checksums[summary] += 1u << b->reg;
                }
            }
        }
        ns[summary] = host_now_ns() - start;
    }
    int failed = checksums[0] != checksums[1] || checksums[1] != (uint64_t)DRAWS * 2 * 0xfff;
    printf("lag-touch reflection 2x128 bindings/12 consumed UBOs per stage: scalar %.2f summary %.2f ns/draw; 5600 draws %.3f -> %.3f ms (enumeration only)\n",
        ns[0] / (double)DRAWS, ns[1] / (double)DRAWS, ns[0] / (double)DRAWS * .0056, ns[1] / (double)DRAWS * .0056);
    printf("%s: lag-touch identical register checksums %llu/%llu\n", failed ? "FAILED" : "ok",
        (unsigned long long)checksums[0], (unsigned long long)checksums[1]);
    return failed;
}
/* Called with real registered bindings and an outstanding private inline update by --lag-walk-bench.
   Isolate the production preparation path from pipeline/descriptor/vertex packet work. */
int vk_draw_test_prepare_bench(void) {
    enum { DRAWS = 200000 };
    vk_shader shader = {.uniform_prepare_mask = 15}, *sh[2] = {&shader, &shader};
    uint64_t calls = g_prepare_calls, skips = g_prepare_skips, checks = g_prepare_checks;
    uint64_t start = host_now_ns();
    for (int draw = 0; draw < DRAWS; draw++) draw_uniform_prepare(sh);
    uint64_t elapsed = host_now_ns() - start;
    printf("lag-prepare micro: %.3f ns/draw, %llu calls, %llu skipped, %llu oracle checks (%d draws, eight bound ranges, TOTK_VK_PREPARE_MASK=%d)\n",
        elapsed / (double)DRAWS, (unsigned long long)(g_prepare_calls - calls),
        (unsigned long long)(g_prepare_skips - skips), (unsigned long long)(g_prepare_checks - checks), DRAWS, prepare_mask_live());
    uint64_t expected = prepare_mask_live() ? 8 : (uint64_t)DRAWS * 8;
    return g_prepare_calls - calls != expected || g_prepare_calls - calls + g_prepare_skips - skips != (uint64_t)DRAWS * 8;
}
void vk_draw_test_prepare_counts(uint64_t *calls, uint64_t *skips, uint64_t *checks) {
    *calls = g_prepare_calls; *skips = g_prepare_skips; *checks = g_prepare_checks;
}
#endif
/* The walk needs clamped vertex ranges and immediate full-index eligibility before hot/copy selection.
   Cache only the exact mapping answer, never contents or lag classification. The immutable span generation
   survives submissions but changes on pool/view/alias mutation, including removal and same-address reuse. */
static int record_buffer_resolve(const vk_record_spans *spans, uint64_t address, uint64_t size, int whole,
                                 VkBuffer *buffer, uint64_t *offset, uint64_t *available) {
    enum { NR = 256 };
    static struct {
        uint64_t generation, address, size, offset, available;
        VkBuffer buffer; int whole, found;
    } memo[NR];
    static uint64_t hint_generation;
    static int hints[64];
    uint64_t generation = vk_record_spans_generation(spans);
    if (hint_generation != generation) {
        memset(hints, 0, sizeof hints); hint_generation = generation;
    }
    uint64_t hash = (address >> 4) ^ (address >> 16) ^ (address >> 32) ^ size ^ (unsigned)whole;
    unsigned h = (unsigned)(hash ^ (hash >> 8)) % NR;
    __typeof__(memo[0]) *m = &memo[h];
    if (m->generation != generation || m->address != address || m->size != size || m->whole != whole) {
        VkBuffer live = VK_NULL_HANDLE; uint64_t off = 0, avail = 0;
        int found = address && vk_record_resolve(spans, address, whole ? size : 1, &live, &off, &avail, hints);
        *m = (__typeof__(*m)){generation, address, size, off, avail, live, whole, found};
    }
    if (!m->found) return 0;
    *buffer = m->buffer; *offset = m->offset; if (available) *available = m->available;
    return 1;
}
/* Command-call blocks are separate from CPU setup; D remains included, not separately timed. */
static int record_buffers(vk_shader *sh, const p_draw *dw, const p_draw_idx *di, int prim, uint32_t count, uint32_t instances, uint32_t base_instance) {
    vk_record_draw *d = g_record_draw;
    for (int a = 0; a < 16; a++) if ((sh->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) d->vertex_mask |= 1u << (S.attr[a].stream & 15);
    for (uint32_t s = 0; s < 16; s++) if (d->vertex_mask >> s & 1) {
        d->vertex[s].address = S.vbuf[s].address; d->vertex[s].size = S.vbuf[s].size ? S.vbuf[s].size : 16; d->vertex[s].stride = S.stream[s].stride;
        /* Packets survive slot reuse: a previous hot/copy draw may have overridden this stream.
           Clear that version before choosing between today's live range and a new immutable copy. */
        d->vertex[s].buffer = VK_NULL_HANDLE; d->vertex[s].offset = 0; d->vertex[s].lag_reason = 0;
        if (g_load_override && s == g_load_stream) {
            d->vertex[s].address = 0; d->vertex[s].buffer = g_load_geometry.buffer;
            d->vertex[s].offset = g_load_geometry.offset; d->vertex[s].size = g_load_geometry.range;
            d->vertex[s].stride = g_load_vertex_bytes; continue;
        }
        if (vk_lag_capture_on() && d->vertex[s].address) {
            VkBuffer buffer; uint64_t offset, available;
            if (record_buffer_resolve(d->spans, d->vertex[s].address, d->vertex[s].size, 0, &buffer, &offset, &available)) {
                d->vertex[s].size = MINV(d->vertex[s].size, available);
                d->vertex[s].buffer = buffer; d->vertex[s].offset = offset;
                VkDescriptorBufferInfo frozen;
                if (vk_lag_read_record(d->vertex[s].address, d->vertex[s].size, 16, &frozen, &d->vertex[s].lag_reason, VK_LAG_VERTEX)) {
                    d->vertex[s].buffer = frozen.buffer; d->vertex[s].offset = frozen.offset;
                }
            }
        }
    }
    if (sh->vin_mask) {
        d->vertex_mask |= 1u << ZERO_BINDING; d->vertex[ZERO_BINDING].address = 0; d->vertex[ZERO_BINDING].size = 64; d->vertex[ZERO_BINDING].stride = 0;
        d->vertex[ZERO_BINDING].buffer = VK_NULL_HANDLE; d->vertex[ZERO_BINDING].offset = 0; d->vertex[ZERO_BINDING].lag_reason = 0;
    }
    d->count = count; d->instances = instances; d->base_instance = base_instance;
    if (dw && prim == 7) {
        if (count < 4) d->count = 0;
        else { quad_indices(count / 4); d->index = g_quad_ib; d->index_type = VK_INDEX_TYPE_UINT32; d->count = count / 4 * 6; d->vertex_offset = dw->first; }
    } else if (dw) d->first = (uint32_t)dw->first;
    else {
        uint32_t stride = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; VkBuffer buffer; uint64_t offset;
        /* Eligibility affects immediate samples/conditions. Freeze the validated live handle too, so workers
           never repeat the mapping lookup or choose a version after this stream position. */
        if (!record_buffer_resolve(d->spans, di->ibuf, (uint64_t)count * stride, 1, &buffer, &offset, NULL)) { g_stat_skips[8]++; return 0; }
        d->index_type = di->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        d->index = buffer; d->index_offset = offset; d->vertex_offset = di->base_vertex;
        if (di->itype != 0 || V.has_index_u8) {
            VkDescriptorBufferInfo frozen;
            if (vk_lag_read_record(di->ibuf, (uint64_t)count * stride, 16, &frozen, &d->index_lag_reason, VK_LAG_INDEX)) {
                d->index = frozen.buffer; d->index_offset = offset = frozen.offset;
            }
            d->index_bytes = (uint64_t)count * stride;
        }
        if (di->itype == 0 && !V.has_index_u8) {
            #ifdef TOTK_VK_TESTS
            g_test_widened++;
            #endif
            VkDeviceSize to; uint16_t *copy = vk_transient((size_t)count * 2 + 2, 16, &d->index, &to);
            const uint8_t *source = (const uint8_t *)(uintptr_t)di->ibuf;
            for (uint32_t i = 0; i < count; i++) copy[i] = source[i] == 0xff ? 0xffff : source[i];
            d->index_address = 0; d->index_offset = offset = to; d->index_type = VK_INDEX_TYPE_UINT16;
        }
        if (offset % stride) { g_stat_skips[8]++; return 0; }
    }
    return 1;
}
#ifdef TOTK_VK_TESTS
static int g_test_terminal_geometry_freeze=1;
void vk_fg_test_terminal_geometry_freeze(int enabled) { g_test_terminal_geometry_freeze=!!enabled; }
#endif
/* Only the scheduler-owned terminal packet gets this stronger geometry
   authority. Real bindings keep their existing lag/live selection. A guest
   range always goes through the original-stream CPU/inline-update selector,
   even if lag selected an arena handle: its pending-GPU-writer guard must not
   be bypassed. address=0 is reserved for known CPU-generated zero and quad
   payloads at the callsites below, never an unknown buffer or live u8 source. */
static int fg_terminal_geometry_read(unsigned role,unsigned stream,uint64_t address,
        VkDescriptorBufferInfo source,VkDescriptorBufferInfo *out) {
    vk_fg_terminal_input input={.role=role,.stage=0,.stream=stream,
        .buffer=source.buffer,.offset=source.offset,.bytes=source.range,.freeze=0};
    if (!source.buffer || !source.range || source.range>VK_FG_TERMINAL_BYTES ||
            source.offset>UINT64_MAX-source.range) {
        vk_fg_terminal_owned_input_refuse(&input,"geometry-range");
        return 0;
    }
    if (address) {
        if (address>UINT64_MAX-source.range ||
                !vk_ub_replay_read(address,source.range,16,out)) {
            vk_fg_terminal_owned_input_refuse(&input,"replay-read-refused");
            return 0;
        }
    } else *out=source;
    return 1;
}
static inline __attribute__((always_inline)) int draw_buffers(VkCommandBuffer cb, vk_shader *sh, const p_draw *dw, const p_draw_idx *di, int prim, uint32_t count, uint32_t inst, uint32_t base_inst) {
    if (g_record_draw) return record_buffers(sh, dw, di, prim, count, inst, base_inst);
    VkBuffer vb[17]; VkDeviceSize vo[17], vsz[17], vst[17]; uint32_t used = 0;
    vk_record_draw *replay = vk_fg_extract_active && FGX.record ? &FGX.record->draw : NULL;
    int owned_geometry=replay && FGX.record->final &&
        atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed);
#ifdef TOTK_VK_TESTS
    owned_geometry=owned_geometry && g_test_terminal_geometry_freeze;
#endif
    if (replay) {
        replay->count = dw && prim == 7 ? (count / 4) * 6 : count;
        replay->instances = inst; replay->base_instance = base_inst;
        replay->first = dw && prim != 7 ? (uint32_t)dw->first : 0;
        replay->vertex_offset = dw ? dw->first : di->base_vertex;
    }
    for (int a = 0; a < 16; a++) if ((sh->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) used |= 1u << (S.attr[a].stream & 15);
    for (uint32_t s = 0; s < 16; s++) if (used >> s & 1) {
        VkBuffer b; uint64_t o, avail; uint64_t sz = S.vbuf[s].size ? S.vbuf[s].size : 16;
        int pool_backed=S.vbuf[s].address && vk_resolve_for(S.vbuf[s].address, 1, &b, &o, &avail, VK_LOOKUP_VERTEX);
        if (!pool_backed) { VkDeviceSize to; void *z = vk_transient(64, 16, &b, &to); memset(z, 0, 64); o = to; sz = 64; RED(4, "vertex stream %u is not in a memory pool", s); }
        else sz = MINV(sz, avail);
        VkDescriptorBufferInfo frozen;
        int lag_frozen=S.vbuf[s].address && vk_lag_read(S.vbuf[s].address, sz, 16, &frozen, VK_LAG_VERTEX);
        if (lag_frozen) { b = frozen.buffer; o = frozen.offset; }
        VkDeviceSize stride = S.stream[s].stride;
        if (g_load_override && s == g_load_stream) {
            b = g_load_geometry.buffer; o = g_load_geometry.offset; sz = g_load_geometry.range;
            stride = g_load_vertex_bytes;
        }
        vb[0] = b; vo[0] = o; vsz[0] = sz; vst[0] = stride;
        VK_REC_DO(VK_REC_CMD, vkCmdBindVertexBuffers2(cb, s, 1, vb, vo, vsz, vst));
        if (replay) {
            replay->vertex_mask |= 1u << s;
            replay->vertex[s].buffer = b; replay->vertex[s].offset = o;
            replay->vertex[s].size = sz; replay->vertex[s].stride = stride;
            if (owned_geometry && fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_VERTEX,s,
                    pool_backed || lag_frozen ? S.vbuf[s].address : 0,(VkDescriptorBufferInfo){b,o,sz},&frozen)) {
                replay->vertex[s].buffer=frozen.buffer; replay->vertex[s].offset=frozen.offset;
            }
        }
    }
    if (sh->vin_mask) {
        VkBuffer zb; VkDeviceSize zo; void *z = vk_transient(64, 16, &zb, &zo); memset(z, 0, 64);
        VkDeviceSize zs = 64, zst = 0;
        VK_REC_DO(VK_REC_CMD, vkCmdBindVertexBuffers2(cb, ZERO_BINDING, 1, &zb, &zo, &zs, &zst));
        if (replay) {
            replay->vertex_mask |= 1u << ZERO_BINDING;
            replay->vertex[ZERO_BINDING].buffer = zb; replay->vertex[ZERO_BINDING].offset = zo;
            replay->vertex[ZERO_BINDING].size = zs; replay->vertex[ZERO_BINDING].stride = zst;
        }
    }
    VK_REC_DO(VK_REC_S, vk_occlusion_draw(cb)); pd_pre_draw(cb);
    if (dw && prim == 7) {
        if (count >= 4) {
            quad_indices(count / 4);
            VK_REC_DO(VK_REC_CMD, vkCmdBindIndexBuffer(cb, g_quad_ib, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, (count / 4) * 6, inst, 0, dw->first, base_inst));
            if (replay) {
                replay->index = g_quad_ib; replay->index_type = VK_INDEX_TYPE_UINT32;
                if (owned_geometry) {
                    /* This static buffer is not an arena. Reproduce only its
                       bounded, known CPU-generated prefix at the original bind;
                       do not grant arbitrary host-visible buffers copy authority. */
                    VkDescriptorBufferInfo source={g_quad_ib,0,(uint64_t)(count/4)*24}, frozen;
                    if (fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_INDEX,0,0,source,&frozen)) {
                        uint32_t *ix=vk_transient((size_t)source.range,16,&replay->index,&replay->index_offset);
                        for (uint32_t q=0;q<count/4;q++) {
                            uint32_t v=q*4,t[6]={v,v+1,v+2,v,v+2,v+3}; memcpy(ix+q*6,t,24);
                        }
                    }
                }
            }
            totk_ft_count(FT_COUNT_DRAWS, 1);
        }
    } else if (dw) { VK_REC_DO(VK_REC_CMD, vkCmdDraw(cb, count, inst, (uint32_t)dw->first, base_inst)); totk_ft_count(FT_COUNT_DRAWS, 1); }
    else {
        VkBuffer ib; uint64_t io;
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!vk_resolve_for(di->ibuf, (uint64_t)count * isz, &ib, &io, NULL, VK_LOOKUP_VERTEX)) {
            if (owned_geometry) {
                vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,
                    .bytes=(uint64_t)count*isz,.freeze=0};
                vk_fg_terminal_owned_input_refuse(&input,"index-buffer-not-in-pool");
            }
            RED(4, "index buffer is not in a memory pool"); g_stat_skips[8]++; return 0;
        }
        VkDescriptorBufferInfo frozen;
        int frozen_u8=0;
        if (owned_geometry && di->itype==0 && !V.has_index_u8) {
            /* Real u8 widening below retains its existing live selection.
               Optional replay instead needs authority for the ORIGINAL bytes,
               including inline updates and the pending-GPU-writer guard. */
            if ((uint64_t)count*2>VK_FG_TERMINAL_BYTES) {
                vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,
                    .buffer=ib,.offset=io,.bytes=count,.freeze=0};
                vk_fg_terminal_owned_input_refuse(&input,"widened-index-range");
            } else frozen_u8=fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_INDEX,0,di->ibuf,
                (VkDescriptorBufferInfo){ib,io,count},&frozen);
        }
        if ((di->itype != 0 || V.has_index_u8) && vk_lag_read(di->ibuf, (uint64_t)count * isz, 16, &frozen, VK_LAG_INDEX)) { ib = frozen.buffer; io = frozen.offset; }
        VkIndexType it = di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (di->itype == 0) {
            if (V.has_index_u8) it = VK_INDEX_TYPE_UINT8_EXT;
            else {
                #ifdef TOTK_VK_TESTS
                g_test_widened++;
                #endif
                VkDeviceSize to; uint16_t *w = vk_transient((size_t)count * 2 + 2, 16, &ib, &to); const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
                for (uint32_t i = 0; i < count; i++) w[i] = src[i] == 0xff ? 0xffff : src[i];
                io = to; it = VK_INDEX_TYPE_UINT16;
            }
        }
        if (io % isz) { RED(4, "index buffer offset %#llx is not aligned", (unsigned long long)io); g_stat_skips[8]++; return 0; }
        if (replay) {
            replay->index = ib; replay->index_offset = io; replay->index_type = it;
            if (owned_geometry) {
                uint64_t bytes=(uint64_t)count*(it==VK_INDEX_TYPE_UINT8_EXT ? 1 : it==VK_INDEX_TYPE_UINT16 ? 2 : 4);
                if (di->itype==0 && !V.has_index_u8) {
                    if (frozen_u8) {
                        VkBuffer replay_index; VkDeviceSize replay_offset;
                        uint16_t *w=vk_transient((size_t)bytes,16,&replay_index,&replay_offset);
                        if (vk_replay_frozen_buffer_copy(frozen.buffer,frozen.offset,count,w)) {
                            /* Backwards expansion cannot overwrite unread raw bytes. */
                            const uint8_t *src=(const uint8_t *)w;
                            for (uint32_t i=count;i;i--) w[i-1]=src[i-1]==0xff ? 0xffff : src[i-1];
                            replay->index=replay_index; replay->index_offset=replay_offset;
                        } else {
                            vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,
                                .buffer=frozen.buffer,.offset=frozen.offset,.bytes=count,.freeze=1};
                            vk_fg_terminal_owned_input_refuse(&input,"frozen-index-copy-refused");
                        }
                    }
                } else if (fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_INDEX,0,di->ibuf,
                        (VkDescriptorBufferInfo){ib,io,bytes},&frozen)) {
                    replay->index=frozen.buffer; replay->index_offset=frozen.offset;
                }
            }
        }
        VK_REC_DO(VK_REC_CMD, vkCmdBindIndexBuffer(cb, ib, io, it); vkCmdDrawIndexed(cb, count, inst, 0, di->base_vertex, base_inst));
        totk_ft_count(FT_COUNT_DRAWS, 1);
    }
    return 1;
}
static void shader_xlate_first_command(vk_shader *sh, const char *kind) {
    if (!sh || !sh->xlate_miss_ns) return;
    LOGV("[shader-xlate] first-%s %s miss-to-command=%.3fms (recorded, not GPU completion)", kind, sh->key,
         (double)(host_now_ns() - sh->xlate_miss_ns) / 1e6);
    sh->xlate_miss_ns = 0;
}
/* TOTK_VK_SPLIT_PASS_DRAWS: end a long pass before its next draw so its recorded part reaches the GPU at the reopen
   (begin_rendering). Only at a draw on the same, still-open targets; never with a DONT_CARE store, UI/canvas passes,
   framegen extraction or pass diagnostics. Deferred merge barriers are emitted at this end like at a hazard split. */
static void split_pass_check(vk_shader *const sh[6], int canvas_transfer) {
    uint32_t n = R.split_draws;   /* taken at this pass (part)'s open */
    if (!n || !R.open || R.split_refused || g_stat_draws - R.draws0 < n) return;
    if (vk_frame_chunk_limit() <= 0 || !vk_submit_chunk_possible()) return;   /* nothing would be submitted */
    if (R.store_discard || R.native || R.canvas || canvas_transfer || sh[0]->canvas_ui || vk_fg_extract_active ||
        FGX.writer.open || FGX.authority_span.open || PD.active || probe_on() || serial_draws()) {
        R.split_refused = 1; g_split_refused++; return;
    }
    if (split_lrz_safe()) {
        if (g_split_unrepairable || (R.dtex && !split_depth_touchable(R.dtex))) {
            R.split_refused = 1; g_split_refused++; g_split_lrz_refused++; return;
        }
        int color_write = 0;
        uint32_t outputs = sh[1] ? sh[1]->fout_mask | sh[1]->fout_span : 0;
        for (unsigned i = 0; i < R.ncolor; i++)
            color_write |= R.ctex[i] && (outputs >> i & 1) && (S.chmask.mask >> (4 * i) & 15);
        if (S.raster_discard || S.render_disabled || (R.dtex && !color_write)) {
            /* Defer this candidate, not the whole scene pass: a depth prepass can become a color-writing region. */
            g_split_refused++; g_split_lrz_refused++; return;
        }
    }
    vk_end_rendering_at(VK_PASS_END_SPLIT, n);
    g_split_submit = 1; g_split_cut = 1; g_split_count++;
}
static void draw_inner(const p_draw *dw, const p_draw_idx *di) {
    g_load_override = 0;
    int prim = dw ? dw->prim : di->prim;
    if (!S.prog) { g_stat_skips[0]++; return; }
    if (S.render_disabled || S.raster_discard) { g_stat_skips[1]++; pd_draw_note("      draw skipped: %s", S.render_disabled ? "conditional rendering off" : "rasterizer discard"); return; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { g_stat_skips[2]++; pd_draw_note("      draw skipped: cull front and back"); return; }
    if (prim == 2 || prim >= 8) { RED(4, "primitive type %d not drawn", prim); g_stat_skips[3]++; return; }
    if (di && prim == 7) { RED(4, "indexed quads not drawn"); g_stat_skips[3]++; return; }
    uint32_t count = dw ? (uint32_t)dw->count : (uint32_t)di->count; if (!count) return;
    if (S.color.alpha_func && S.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS) {
        g_alpha_enabled++;
        if (!totk_live_cached(&g_alpha_live,"TOTK_VK_ALPHA_TEST",g_alpha_launch)) g_alpha_ignored++;
    }
    vk_shader *sh[6];
    if (VK_REC_VALUE(VK_REC_PROG, vk_record_enabled() ? vk_prog_resolve_record(S.prog, sh) : vk_prog_resolve(S.prog, sh)) || !sh[0]) { g_stat_skips[4]++; return; }
    if (sh[1] && S.color.alpha_func && S.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS &&
        totk_live_cached(&g_alpha_live,"TOTK_VK_ALPHA_TEST",g_alpha_launch)) {
        sh[1]=vk_shader_alpha(sh[1],S.color.alpha_func);
        if (!sh[1]) { g_stat_skips[4]++; return; }
    }
    vk_canvas_image transfer_source,transfer_window; int canvas_transfer=0;
    if (vk_canvas_on && (vk_native_ui_enabled() ||
        ((sh[0]->canvas_ui == 2 || (sh[1] && sh[1]->canvas_transfer)) &&
         (!R.open || !vk_canvas_current()->ui_committed || (sh[1] && sh[1]->canvas_transfer)))))
        canvas_transfer=canvas_draw_prepare(sh,&transfer_source,&transfer_window);
    int merge_hazard = pass_merge_on() ? pass_merge_draw_hazard(sh) : 0;
    if (R.open && R.merge_pending && merge_hazard) {
        uint32_t pending = R.merge_pending; g_merge_hazards++;
        vk_end_rendering_at(VK_PASS_END_MERGE_HAZARD, pending);
    }
    feedback_usage feedback={0}; int feedback_known=0;
    if (R.open && R.feedback_on) {
        feedback=feedback_draw_usage(sh); feedback_known=1;
        if (R.feedback_pending && (feedback.unknown ||
            (feedback.read&(R.feedback_written|feedback.written)) || (feedback.written&R.feedback_sampled))) {
            R.feedback_restore=!R.feedback_armed && R.feedback_continued!=0;
            g_feedback_hazards++;
            vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER,R.feedback_pending);
        }
    }
    split_pass_check(sh, canvas_transfer);
    VK_REC_DO(VK_REC_S_TOUCH, draw_touch(sh, dw, di));
    g_load_draw = dw; g_load_indexed = di; g_load_sh = sh; g_load_pipeline = VK_NULL_HANDLE;
    int opened = begin_rendering(1);
    g_load_sh = NULL; g_load_draw = NULL; g_load_indexed = NULL;
    if (!opened) { g_stat_skips[5]++; return; }
    if (R.feedback_on && !feedback_known) feedback=feedback_draw_usage(sh);
    p_draw certified;
    if (g_load_override) {
        certified = (p_draw){prim,0,(int32_t)count,dw ? dw->base_instance : di->base_instance,
            dw ? dw->instances : di->instances};
        dw = &certified; di = NULL; /* certificates exclude VertexIndex/InstanceIndex */
    }
    if (S.has_sc) { int32_t x1 = MINV((int32_t)R.w, S.sc.x + S.sc.w), y1 = MINV((int32_t)R.h, S.sc.y + S.sc.h); if (x1 <= MAXV(0, S.sc.x) || y1 <= MAXV(0, S.sc.y)) { g_stat_skips[6]++; pd_draw_note("      draw skipped: empty scissor"); return; } }
    VkPipelineLayout layout; VkPipeline p;
    static struct { vk_shader *vs, *fs; VkPipelineLayout layout; } last_layout;
    VK_REC_DO(VK_REC_PIPE,
        if (g_load_pipeline) {
            layout = g_load_layout; p = g_load_pipeline;
        } else {
            if (!last_layout.layout || last_layout.vs != sh[0] || last_layout.fs != sh[1]) {
                last_layout.layout = vk_layout_of(sh[0], sh[1]); last_layout.vs = sh[0]; last_layout.fs = sh[1];
            }
            layout = last_layout.layout;
            p = pipe_get(sh[0], sh[1], prim, layout);
        });
    if (!p) { g_stat_skips[10]++; pd_draw_note("      draw skipped: pipeline compiling (vs %.16s fs %.16s)", sh[0]->key, sh[1] ? sh[1]->key : "-"); return; }
    if (p == PIPE_FAILED) { g_stat_skips[7]++; pd_draw_note("      draw skipped: no pipeline (vs %.16s fs %.16s)", sh[0]->key, sh[1] ? sh[1]->key : "-"); return; }
    if (vk_fg_extract_active && (FGX.guard.first || FGX.guard.changed ||
        FGX.guard.program != (uintptr_t)sh[0] || FGX.guard.fragment != (uintptr_t)sh[1]))
        (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,(uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,sh);
    if (!fg_extract_authority_before_draw(sh)) { g_stat_skips[5]++; return; }
    if (!fg_extract_depth_spans_before_draw(sh)) { g_stat_skips[5]++; return; }
    if (vk_fg_extract_active) { FGX.record = NULL; FGX.draw_kind = 0; }
    if (vk_fg_extract_active && FGX.captured && !FGX.invalid) {
        if (fg_extract_canvas(R.ctex[0],R.clevel[0],R.clayer[0]) && vk_canvas_ui_shader(sh[0]->key,1)) FGX.draw_kind = 1;
        else if (canvas_transfer || (sh[1] && vk_canvas_transfer_shader(sh[1]->key) && vk_tex_canvas_window(R.ctex[0])))
            FGX.draw_kind = 2;
        if (FGX.draw_kind && vk_fg_canvas_view()) {
            if (R.ncolor != 1 || R.samples != VK_SAMPLE_COUNT_1_BIT ||
                (FGX.draw_kind == 1 && !fg_extract_ui_depth(R.dtex,R.dlevel,R.dlayer)) ||
                (FGX.draw_kind == 2 && R.dtex) || sh[0]->has_storage_writes || (sh[1] && sh[1]->has_storage_writes))
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI/transfer replay attachment or shader structure changed");
            else if (FGX.nreplay == sizeof FGX.replay_draw / sizeof FGX.replay_draw[0])
                fg_extract_reject(VK_FG_TRACK_CAPACITY,"UI replay packet capacity exhausted");
            else {
                vk_post_output target;
                if (!fg_extract_replay_target(&target)) {
                    fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI/transfer replay attachment geometry unavailable");
                } else {
                    FGX.record = &FGX.replay_draw[FGX.nreplay];
                    memset(FGX.record, 0, sizeof *FGX.record);
                    FGX.record->draw.pipeline = p; FGX.record->draw.layout = layout;
                    FGX.record->width = target.w; FGX.record->height = target.h;
                    FGX.record->format = target.format; FGX.record->image = target.image;
                    FGX.record->native = R.native; FGX.record->final = FGX.draw_kind == 2;
                    if (FGX.draw_kind==2 &&
                            atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed))
                        (void)vk_fg_terminal_owned_begin();
                }
            }
        }
    }
    VkCommandBuffer cb = vk_cb();
    if (pd_active()) {
        float c25[4] = {0}; const p_bind_buf *u8 = &S.ubo[0][8]; if (u8->addr && u8->size >= 26 * 16) vk_ub_read(u8->addr + 25 * 16, c25, 16);
        pd_draw_note("      draw: vs %.16s fs %.16s prim %d count %u inst %u first %u%s; depth test %d func %d write %d; stencil %d func %d ref %#x mask %#x; colour mask %#x blend %#x; scissor %s %d,%d %dx%d; vs c11[25] %g %g %g %g",
            sh[0]->key, sh[1] ? sh[1]->key : "-", prim, count, dw ? (uint32_t)dw->instances : (uint32_t)di->instances, dw ? (uint32_t)dw->first : (uint32_t)di->base_vertex, di ? " indexed" : "",
            S.ds.dt, S.ds.df, S.ds.dw, S.ds.st, S.ds.sf_f, S.st_ref[0], S.st_vmask[0], S.chmask.mask, S.color.blend_en, S.has_sc ? "on" : "off", S.sc.x, S.sc.y, S.sc.w, S.sc.h,
            (double)c25[0], (double)c25[1], (double)c25[2], (double)c25[3]);
        /* The driver's "Depth write + blending" class also includes masks/missing targets/logic ops. Keep raw NVN
           state beside effective Vulkan state so a dump can distinguish translation artifacts from real requests. */
        if (S.ds.dt && S.ds.dw && R.dfmt != VK_FORMAT_UNDEFINED) {
            uint32_t be = pipeline_blend_enable();
            for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (!R.cfmt[i]) be &= ~(1u << i); /* pipe_create skips holes */
            pd_draw_note("      lrz: pipeline %#llx NVN test/write %d/%d -> Vulkan test/write 1/1 compare %u; blend raw/effective %#x/%#x logic %d a2c %u; FS NVN-early %d depth %d outputs %#x; c%u depthfmt %u formats %u,%u,%u,%u,%u,%u,%u,%u",
                (unsigned long long)(uintptr_t)p, S.ds.dt, S.ds.dw, (unsigned)vk_compare(S.ds.df), S.color.blend_en, be, S.color.logic_op, (unsigned)(S.ms.a2c & 1),
                sh[1] ? sh[1]->early_tests : 0, sh[1] ? sh[1]->writes_depth : 0, sh[1] ? sh[1]->fout_mask : 0, R.ncolor, (unsigned)R.dfmt,
                (unsigned)R.cfmt[0], (unsigned)R.cfmt[1], (unsigned)R.cfmt[2], (unsigned)R.cfmt[3], (unsigned)R.cfmt[4], (unsigned)R.cfmt[5], (unsigned)R.cfmt[6], (unsigned)R.cfmt[7]);
            for (uint32_t i = 0; i < R.ncolor && i < 8; i++) {
                const blend_st *b = &S.blend[i];
                pd_draw_note("        lrz c%u fmt %u mask %#x blend %u RGB %d/%d eq %d alpha %d/%d eq %d",
                    i, (unsigned)R.cfmt[i], R.cfmt[i] ? (unsigned)(S.chmask.mask >> (4 * i) & 15) : 0u, (unsigned)(be >> i & 1),
                    b->src_rgb, b->dst_rgb, b->eq_rgb, b->src_a, b->dst_a, b->eq_a);
            }
        }
    }
    if (R.secondary) g_record_draw = vk_record_draw_begin(p, layout);
    else { VK_REC_DO(VK_REC_CMD, if (p != g_bound_pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_bound_pipe = p; }); }
    { static const char *vt = (const char *)1; if (vt == (const char *)1) vt = getenv("TOTK_VK_PASS_DUMP_VSTEX");
      g_pd_cap = PD.active && !g_pd_vstex_done && vt && *vt && !strncmp(sh[0]->key, vt, strlen(vt)); }
    g_ui_aspect_on = 0;
    if (vk_canvas_on && sh[0]->canvas_ui && count == 6 && R.ncolor && R.ctex[0]) {   /* vk_ui_aspect.h: a HUD quad */
        float shown = pres_shown_aspect();
        if (shown > 0) {
            vk_canvas_image target = vk_tex_canvas_identity(R.ctex[0], R.clevel[0], R.clayer[0]);
            g_ui_aspect = vk_ui_aspect_draw(shown, &target, sh[0]->canvas_ui, count);
            g_ui_aspect_on = vk_ui_aspect_on(&g_ui_aspect);
        }
    }
    VK_REC_DO(VK_REC_CMD, dynamic_state(cb, prim));
    VK_REC_DO(VK_REC_BIND,
        if (__builtin_expect(vk_xlate_stats_enabled, 0)) {
            xlate_begin(); g_xlate->draws++; xlate_buffers(sh[0], di);
            for (int stage = 0; stage < 2; stage++) if (sh[stage])
                bind_stage_impl(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, (uint32_t)stage, sh[stage], stage, NULL, 1);
            xlate_end();
        } else {
            bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, sh[0], 0, NULL);
            if (sh[1]) bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, sh[1], 1, NULL);
        });
    if (!R.open) { RED(4, "the render pass ended while a draw was being set up: draw skipped"); g_stat_skips[5]++; return; }
    if (probe_on() && g_npdraw < 4) {   /* pass probe: what each of the pass's first draws was */
        probe_draw *q = &g_pdraw[g_npdraw++]; memset(q, 0, sizeof *q);
        snprintf(q->vs, sizeof q->vs, "%.16s", sh[0]->key); q->prim = prim; q->count = dw ? (uint32_t)dw->count : (uint32_t)di->count; q->first = dw ? (uint32_t)dw->first : (uint32_t)di->base_vertex;
        q->inst = dw ? (uint32_t)dw->instances : (uint32_t)di->instances;
        if (S.has_vp) memcpy(q->vp, S.vp[0], 16); memcpy(q->dr, S.dr, 8); q->bias[0] = S.po.factor; q->bias[1] = S.po.units; q->bias[2] = (S.poly.offset_en & 4) ? 1.f : 0.f;
        q->clamp = S.depth_clamp; q->cull = (int)S.poly.cull; q->has_sc = S.has_sc; if (S.has_sc) { q->sc[0] = S.sc.x; q->sc[1] = S.sc.y; q->sc[2] = S.sc.w; q->sc[3] = S.sc.h; }
        /* the words the class-shading vertex shaders (345bb4abe6b6bbf4 / 97c2cb79ae8c75a5) turn into depth: vp_c11[25].x
           (the class, uniform slot 8) and vp_c4[147].z (slot 1); depth = k + (1 - k) * nibble_swap(class) / 255 */
        { const p_bind_buf *u8 = &S.ubo[0][8], *u1 = &S.ubo[0][1];
          if (u8->addr && u8->size >= 26 * 16) vk_ub_read(u8->addr + 25 * 16, q->c11_25, 16);
          if (u1->addr && u1->size >= 148 * 16) vk_ub_read(u1->addr + 147 * 16, q->c4_147, 16);
          if (u1->addr && u1->size >= 8 * 16) vk_ub_read(u1->addr + 7 * 16, q->c4_7, 16); }
        int a0 = -1; for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) { a0 = a; break; }
        if (a0 >= 0) { uint32_t st = S.attr[a0].stream & 15; uint64_t va = S.vbuf[st].address; VkBuffer bb; uint64_t oo, av;
            if (va && vk_resolve_span(va, &bb, &oo, &av) && av >= 32) memcpy(q->v0, (const void *)(uintptr_t)(va + (uint64_t)q->first * S.stream[st].stride), 32); }
    }
    uint32_t inst = dw ? (dw->instances > 1 ? (uint32_t)dw->instances : 1) : (di->instances > 1 ? (uint32_t)di->instances : 1);
    uint32_t base_inst = dw ? (uint32_t)dw->base_instance : (uint32_t)di->base_instance;
    if (!VK_REC_VALUE(VK_REC_VERTEX, draw_buffers(cb, sh[0], dw, di, prim, count, inst, base_inst))) return;
    if (g_record_draw) { vk_record_draw_commit(); g_record_draw = NULL; }
    overlay_scene_draw();

    if (vk_canvas_on) {
        if (canvas_transfer) vk_canvas_transfer(&transfer_source,&transfer_window,1);
        if (R.ctex[0] && sh[0]->canvas_ui && !vk_canvas_current()->ui_committed) {
            vk_canvas_image target=vk_tex_canvas_identity(R.ctex[0],R.clevel[0],R.clayer[0]);
            vk_canvas_ui_commit(&target);
        }
    }
    g_stat_draws++; g_chunk_draws++; vk_note_draw();
    if (__builtin_expect(PL.enabled, 0) && pso_life_active()) {
        for (unsigned i = 0; i < R.ncolor && i < 8; i++)
            if (sh[1] && ((S.chmask.mask >> (4 * i)) & 15u)) PL.color_draws[i]++;
        if (R.dtex && S.ds.dt && S.ds.dw) PL.depth_draws++;
        if (R.dtex && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])) PL.stencil_draws++;
    }
    if (g_draw_fast) atomic_fetch_add(&g_library_stats.fast_draws, 1);
    if (vk_fg_extract_active) {
        if (FGX.draw_kind == 2) FGX.transfer = 1;
        if (FGX.record) { FGX.nreplay++; FGX.record = NULL; }
    }
    shader_xlate_first_command(sh[0], "draw"); shader_xlate_first_command(sh[1], "draw");
    if (pass_merge_on()) {
        R.merge_storage |= (uint32_t)merge_hazard;
        if (R.merge_armed) { R.merge_continued++; R.merge_armed = 0; }
    }
    if (R.feedback_on) {
        R.feedback_written |= feedback.written; R.feedback_sampled |= feedback.read;
        R.feedback_unknown |= feedback.unknown;
        if (R.feedback_armed) { R.feedback_continued++; R.feedback_armed=0; }
    }
    if (vk_gpuprof_on) vk_gpuprof_draw(sh);
    g_samples_cum += (uint64_t)count * inst;
    pd_draw(prim, count, inst, di ? di->itype + 1 : 0, sh);
    if (PP.open >= 0 && S.has_vp) { pp_rec *pr = &PP.rec[PP.open]; uint32_t vw = (uint32_t)fabsf(S.vp[0][2]), vh = (uint32_t)fabsf(S.vp[0][3]); if (vw > pr->vpw) pr->vpw = vw; if (vh > pr->vph) pr->vph = vh; }
    if (sh[1] && pd_split_match(sh[1]->key)) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2);
}
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (__builtin_expect(vk_rec_profile_on, 0)) {
        uint64_t p0 = vk_rec_profile_draw_begin(), before = g_stat_draws;
        draw_inner(dw, di);
        vk_rec_profile_draw_end(p0, g_stat_draws != before);
    } else draw_inner(dw, di);
    if (g_record_draw) vk_record_draw_abort();
    if (vk_fg_extract_active) FGX.record = NULL;
    g_record_draw = NULL;
}
static void dispatch(const p_dispatch *dp) {
    if (!vk_lag_uniform_private()) vk_ub_flush();   /* private UBOs materialize only for actual non-UBO consumers */
    if (!S.prog) return;
    vk_shader *sh[6];
    if (vk_prog_resolve(S.prog, sh) || !sh[5]) { g_stat_skips[9]++; return; }
    VkPipelineLayout layout; VkPipeline p = vk_compute_pipeline(sh[5], &layout);
    if (!p) return;
    if (vk_lag_capture_on()) { storage_touch(5, sh[5], 1); storage_touch(5, sh[5], 0); }
    vk_end_rendering_at(VK_PASS_END_COMPUTE, 0);
    g_native_sample = NULL;
    if (vk_native_ui_enabled()) native_reads(sh,5,6);
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    uint32_t grid[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
    if (__builtin_expect(vk_xlate_stats_enabled, 0)) {
        xlate_begin();
        bind_stage_impl(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, sh[5], 5, grid, 1);
        xlate_end();
    } else bind_stage(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, sh[5], 5, grid);
    pd_dispatch(sh[5], grid);
    int pi = pp_begin(cb, 1);
    if (vk_gpuprof_on) { vk_gpuprof_dispatch_begin(cb); vk_gpuprof_draw(sh); }
    vkCmdDispatch(cb, grid[0], grid[1], grid[2]);
    shader_xlate_first_command(sh[5], "dispatch");
    if (vk_fg_extract_active && grid[0] && grid[1] && grid[2]) fg_extract_compute(sh);
    vk_barrier_all(cb);
    pp_end(cb, pi);
    if (vk_gpuprof_on) vk_gpuprof_end(cb, VK_PASS_END_COMPUTE, 0);
    pd_buf(cb, 'd', (int)(g_stat_dispatch % 10000));
    g_bound_pipe = VK_NULL_HANDLE; g_stat_dispatch++;
}

/* ---------- copies and clears of textures ---------- */
static unsigned vlevel(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
static unsigned vlayer(const view_t *v, int has) { return has && v->set_layers ? v->min_layer : 0; }
static VkImageAspectFlags copy_aspect(vk_tex *x) { VkImageAspectFlags a = vk_tex_aspect(x); return a == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : a; }
/* Replay diagnostics only: no texture following, buffer substitution, or per-draw work. */
static void texcopy_region(const char *op, const char *side, const tex_t *t, const view_t *v, int has, const NVNcopyRegion *r) {
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s side=%s serial=%llu mem=%p storage_size=%zu format=%#x level=%u layer=%u region=%d,%d,%d,%dx%dx%d",
        op, side, (unsigned long long)t->serial, t->mem, (size_t)t->L.size, t->d.format,
        vlevel(v, has), vlayer(v, has), r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth);
}
static int texcopy_buffer_attempt(const p_copy_b2t *c, const char *op, int upload) {
    size_t bytes = (size_t)totk_texcopy_buffer_size(c->tex, &c->r, S.copy_stride, 0);
    int texture_match = totk_texcopy_match(c->tex);
    totk_texcopy_track(c->tex);
    tex_t overlap;
    int buffer_match = totk_texcopy_overlap(c->src, bytes ? bytes : 1, &overlap);
    if (!texture_match && !buffer_match) return 0;
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s result=attempt texture_side=%s serial=%llu buffer_side=%s buffer=%#llx bytes=%zu bytes_known=%d copy_stride=%lld flags=%#x match_texture=%d match_buffer=%d",
        op, upload ? "dst" : "src", (unsigned long long)c->tex->serial, upload ? "src" : "dst",
        (unsigned long long)c->src, bytes, bytes != 0, (long long)S.copy_stride, c->flags, texture_match, buffer_match);
    texcopy_region(op, upload ? "dst" : "src", c->tex, &c->view, c->has_view, &c->r);
    if (buffer_match) totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s buffer_side=%s overlap_serial=%llu overlap_mem=%p overlap_size=%zu",
        op, upload ? "src" : "dst", (unsigned long long)overlap.serial, overlap.mem, (size_t)overlap.L.size);
    return 1;
}
static void texcopy_buffer_resolved(const char *op, const char *side, const p_copy_b2t *c, VkBuffer b, uint64_t o, size_t row, size_t rows, size_t img, int nz) {
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s buffer_side=%s buffer=%#llx bytes=%zu vk_buffer=%#llx offset=%#llx row=%zu rows=%zu image_stride=%zu depth=%d",
        op, side, (unsigned long long)c->src, img * nz, (unsigned long long)(uintptr_t)b,
        (unsigned long long)o, row, rows, img, nz);
}
static void texcopy_guest_source(const p_copy_b2t *c, size_t row, size_t img) {
    static unsigned samples;
    if (samples >= 32) {
        totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyBufferToTexture buffer_side=src data=guest-source-snapshot status=skip reason=snapshot-budget gpu_authoritative=0");
        return;
    }
    samples++;
    size_t bytes = (size_t)totk_texcopy_buffer_size(c->tex, &c->r, (ptrdiff_t)row, (ptrdiff_t)img);
    if (bytes > (size_t)1024 * 1024) {
        totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyBufferToTexture buffer_side=src data=guest-source-snapshot status=skip reason=snapshot-size bytes=%zu gpu_authoritative=0", bytes);
        return;
    }
    totk_texcopy_stats s = totk_texcopy_payload(c->tex, &c->r, (const void *)(uintptr_t)c->src, (ptrdiff_t)row, (ptrdiff_t)img);
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=CopyBufferToTexture buffer_side=src data=guest-source-snapshot status=%s buffer=%#llx bytes=%zu sample_bytes=%zu nonzero=%zu zero=%zu rg=%d r_min=%u r_max=%u g_min=%u g_max=%u gpu_authoritative=0",
        s.valid ? "sampled" : "invalid", (unsigned long long)c->src, bytes, s.bytes, s.nonzero, s.bytes - s.nonzero,
        s.rg, s.min[0], s.max[0], s.min[1], s.max[1]);
}
static void copy_b2t(const p_copy_b2t *c) {   /* buffer -> texture (row stride from SetCopyRowStride, 0 = packed) */
    int trace = totk_texcopy_on() && texcopy_buffer_attempt(c, "CopyBufferToTexture", 1);
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else vk_ub_touch((uint64_t)(uintptr_t)c->tex->mem, c->tex->L.size);
    vk_tex *x = vk_tex_get(c->tex);
    if (trace && !x) vk_texcopy_image("CopyBufferToTexture", "dst", c->tex, x);
    if (!x) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=texture-unavailable");
        return;
    }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=unsupported-format");
        return;
    }
    unsigned level = vlevel(&c->view, c->has_view), layer0 = vlayer(&c->view, c->has_view);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int nz = c->r.depth ? c->r.depth : 1, is3d = vk_tex_is3d(x);
    if (vk_lag_uniform_private()) vk_ub_touch(c->src, img * nz);
    VkBuffer b; uint64_t o;
    if (!vk_resolve(c->src, img * nz, &b, &o)) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=buffer-unresolved buffer=%#llx bytes=%zu", (unsigned long long)c->src, img * nz);
        RED(4, "buffer-to-texture copy: the source is not in a memory pool"); return;
    }
    VkDescriptorBufferInfo frozen;
    if (vk_lag_read(c->src, img * nz, 16, &frozen, VK_LAG_COPY)) {
        b = frozen.buffer; o = frozen.offset;
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture buffer_side=src source=lag-snapshot");
    }
    if (trace) texcopy_buffer_resolved("CopyBufferToTexture", "src", c, b, o, row, rows, img, nz);
    if (row % bpb || o % 4) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=buffer-layout-unexpressible row=%zu offset=%#llx", row, (unsigned long long)o);
        RED(4, "buffer-to-texture copy with stride %zu / offset %#llx not expressible", row, (unsigned long long)o); return;
    }
    if (trace) texcopy_guest_source(c, row, img);
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_B2T);
    vk_tex_native_read(x);
    if (trace) vk_texcopy_image("CopyBufferToTexture", "dst", c->tex, x);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "upload");
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {copy_aspect(x), level, is3d ? 0 : layer0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyBufferToImage(cb, b, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    totk_ft_count(FT_COUNT_TEX_UPLOADS, 1);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    vk_tex_native_patch(x, level, layer0, &c->r);
    if (vk_fg_extract_active) fg_extract_texture_write(c->tex, level, is3d ? 0 : layer0 + c->r.zoffset,
        is3d ? 1 : (unsigned)nz, copy_aspect(x),
        !c->r.xoffset && !c->r.yoffset && (unsigned)c->r.width == vk_tex_width(x, level) &&
        (unsigned)c->r.height == vk_tex_height(x, level), 0, 0);
    vk_tex_mark_gpu_written(x); probe_write_note('B', x, NULL, 0);
    vk_pso_life_external_write(x, level, r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount, "buffer-copy");
    if (trace) {
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=success encoded=1 dst_serial=%llu bytes=%zu", (unsigned long long)c->tex->serial, img * nz);
        vk_texcopy_readback("CopyBufferToTexture", "dst", c->tex, x, level, layer0, &c->r);
    }
}
static void copy_t2b(const p_copy_b2t *c) {   /* texture -> buffer */
    int trace = totk_texcopy_on() && texcopy_buffer_attempt(c, "CopyTextureToBuffer", 0);
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else vk_ub_touch((uint64_t)(uintptr_t)c->tex->mem, c->tex->L.size);
    vk_tex *x = vk_tex_get(c->tex);
    if (trace && !x) vk_texcopy_image("CopyTextureToBuffer", "src", c->tex, x);
    if (!x) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=texture-unavailable");
        return;
    }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=unsupported-format");
        return;
    }
    unsigned level = vlevel(&c->view, c->has_view), layer0 = vlayer(&c->view, c->has_view);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int nz = c->r.depth ? c->r.depth : 1, is3d = vk_tex_is3d(x);
    VkBuffer b; uint64_t o;
    if (!vk_resolve(c->src, img * nz, &b, &o)) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=buffer-unresolved buffer=%#llx bytes=%zu", (unsigned long long)c->src, img * nz);
        RED(4, "texture-to-buffer copy: the destination is not in a memory pool"); return;
    }
    if (trace) texcopy_buffer_resolved("CopyTextureToBuffer", "dst", c, b, o, row, rows, img, nz);
    vk_gpu_write(c->src, img * nz);
    if (row % bpb || o % 4) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=buffer-layout-unexpressible row=%zu offset=%#llx", row, (unsigned long long)o);
        RED(4, "texture-to-buffer copy with stride %zu / offset %#llx not expressible", row, (unsigned long long)o); return;
    }
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_T2B);
    vk_tex_native_read(x);
    if (trace) vk_texcopy_image("CopyTextureToBuffer", "src", c->tex, x);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {copy_aspect(x), level, is3d ? 0 : layer0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    vk_pso_life_external_read(vk_tex_width(x, level), vk_tex_height(x, level), "texture-to-buffer-read");
    vk_fg_extract_memory_write(c->src, img * nz);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (trace) {
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=success encoded=1 src_serial=%llu bytes=%zu", (unsigned long long)c->tex->serial, img * nz);
        vk_texcopy_readback("CopyTextureToBuffer", "src", c->tex, x, level, layer0, &c->r);
    }
}
static void copy_t2t(const p_copy_t2t *c) {   /* texture -> texture: same size as a copy, scaled as a blit */
    int trace = 0;
    if (totk_texcopy_on()) {
        int sm = totk_texcopy_match(c->src), dm = totk_texcopy_match(c->dst);
        trace = sm || dm;
        if (trace) {
            totk_texcopy_track(c->src); totk_texcopy_track(c->dst);
            totk_texcopy_log(vk_submit_serial(),
                "phase=replay op=CopyTextureToTexture result=attempt src_serial=%llu dst_serial=%llu flags=%#x match_src=%d match_dst=%d",
                (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial, c->flags, sm, dm);
            texcopy_region("CopyTextureToTexture", "src", c->src, &c->sv, c->has_sv, &c->sr);
            texcopy_region("CopyTextureToTexture", "dst", c->dst, &c->dv, c->has_dv, &c->dr);
        }
    }
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else {
        vk_ub_touch((uint64_t)(uintptr_t)c->src->mem, c->src->L.size);
        vk_ub_touch((uint64_t)(uintptr_t)c->dst->mem, c->dst->L.size);
    }
    vk_tex *s = vk_tex_get(c->src), *d = vk_tex_get(c->dst);
    if (trace && (!s || !d)) {
        vk_texcopy_image("CopyTextureToTexture", "src", c->src, s);
        vk_texcopy_image("CopyTextureToTexture", "dst", c->dst, d);
    }
    if (!s || !d) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToTexture result=skip reason=texture-unavailable src_resolved=%d dst_resolved=%d", s != NULL, d != NULL);
        return;
    }
    unsigned sl = vlevel(&c->sv, c->has_sv), dl = vlevel(&c->dv, c->has_dv), sz0 = vlayer(&c->sv, c->has_sv), dz0 = vlayer(&c->dv, c->has_dv);
    int s3 = vk_tex_is3d(s), d3 = vk_tex_is3d(d), nz = c->sr.depth ? c->sr.depth : 1;
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_T2T);
    vk_tex_native_read(s); vk_tex_native_read(d);
    if (trace) {
        vk_texcopy_image("CopyTextureToTexture", "src", c->src, s);
        vk_texcopy_image("CopyTextureToTexture", "dst", c->dst, d);
    }
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1) && s3 == d3;
    int bwa, bha, bwb, bhb, bpa = nvn_fmt_bpb(c->src->d.format, &bwa, &bha), bpbb = nvn_fmt_bpb(c->dst->d.format, &bwb, &bhb);
    unsigned copied_layers = 0;
    if (same && bpa == bpbb && bwa == bwb && bha == bhb) {
        VkImageCopy r = {.srcSubresource = {copy_aspect(s), sl, s3 ? 0 : sz0 + c->sr.zoffset, s3 ? 1 : (uint32_t)nz}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
            .dstSubresource = {copy_aspect(d), dl, d3 ? 0 : dz0 + c->dr.zoffset, d3 ? 1 : (uint32_t)nz}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
            .extent = {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 ? (uint32_t)nz : 1}};
        vkCmdCopyImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        copied_layers = (unsigned)nz;
        if (trace) totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyTextureToTexture result=success kind=copy encoded=1 src_serial=%llu dst_serial=%llu layers=%u",
            (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial, copied_layers);
    } else if (!s3 && !d3 && copy_aspect(s) == copy_aspect(d)) {
        VkImageBlit r = {.srcSubresource = {copy_aspect(s), sl, sz0 + c->sr.zoffset, 1}, .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
            .dstSubresource = {copy_aspect(d), dl, dz0 + c->dr.zoffset, 1}, .dstOffsets = {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
        int linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && copy_aspect(s) == VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdBlitImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        copied_layers = 1;
        if (trace) totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyTextureToTexture result=success kind=blit encoded=1 src_serial=%llu dst_serial=%llu layers=%u",
            (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial, copied_layers);
    } else {
        if (trace) totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyTextureToTexture result=skip reason=unsupported-copy src_3d=%d dst_3d=%d src_aspect=%#x dst_aspect=%#x",
            s3, d3, copy_aspect(s), copy_aspect(d));
        RED(4, "texture copy %dx%dx%d -> %dx%dx%d (3D %d -> %d) not done", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, s3, d3);
    }
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (copied_layers) vk_tex_native_patch(d, dl, dz0, &c->dr);
    if (vk_fg_extract_active && copied_layers) fg_extract_copy(c, s, d, copied_layers);
    vk_tex_mark_gpu_written(d); probe_write_note('C', d, s, 0);
    if (copied_layers) vk_pso_life_external_write(d, dl, dz0, copied_layers, "image-copy");
    if (copied_layers) vk_pso_life_external_read(vk_tex_width(s, sl), vk_tex_height(s, sl), "texture-copy-read");
    if (trace && copied_layers) {
        NVNcopyRegion sr = c->sr, dr = c->dr;
        sr.depth = dr.depth = (int)copied_layers;
        vk_texcopy_readback("CopyTextureToTexture", "src", c->src, s, sl, sz0, &sr);
        vk_texcopy_readback("CopyTextureToTexture", "dst", c->dst, d, dl, dz0, &dr);
    }
}
/* NVN ClearTexture clears a region of one level (x/y/width/height, layers or 3D slices zoffset/depth) and only the
   channels in its mask. vkCmdClearColorImage/DepthStencilImage clear whole subresources and every channel, so a
   region or a colour channel mask is cleared in a temporary rendering pass on that level: vkCmdClearAttachments with
   the region as its rect, or, for a channel mask, one triangle drawing the value through a pipeline whose
   colorWriteMask is the mask, scissored to the region. Whole unmasked clears keep the image clears. */
static VkPipelineLayout g_clear_mask_layout;
static VkShaderModule g_clear_mask_module[4];   /* vertex; fragment float, uint, int */
static struct { VkFormat format; uint32_t mask; VkSampleCountFlagBits samples; VkPipeline pipeline; } g_clear_mask_pipe[32];
static int g_clear_mask_npipe;
static uint64_t g_clear_region, g_clear_masked;   /* ClearTexture calls that needed the region/mask path (tests) */
#include "vk_clear_mask_spv.h"
static VkPipeline clear_mask_pipeline(VkFormat format, uint32_t mask, VkSampleCountFlagBits samples, int type) {
    for (int i = 0; i < g_clear_mask_npipe; i++)
        if (g_clear_mask_pipe[i].format == format && g_clear_mask_pipe[i].mask == mask && g_clear_mask_pipe[i].samples == samples)
            return g_clear_mask_pipe[i].pipeline;
    if (g_clear_mask_npipe == (int)(sizeof g_clear_mask_pipe / sizeof *g_clear_mask_pipe)) return VK_NULL_HANDLE;
    if (!g_clear_mask_layout) {
        VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .size = 16};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        if (vkCreatePipelineLayout(V.dev, &lci, NULL, &g_clear_mask_layout)) { g_clear_mask_layout = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    static const uint32_t *const code[4] = {clear_mask_vert_spv, clear_mask_frag0_spv, clear_mask_frag1_spv, clear_mask_frag2_spv};
    static const size_t size[4] = {sizeof clear_mask_vert_spv, sizeof clear_mask_frag0_spv, sizeof clear_mask_frag1_spv, sizeof clear_mask_frag2_spv};
    const int modules[2] = {0, 1 + type};   /* the vertex shader and this value type's fragment shader */
    for (int k = 0, i; k < 2; k++) if (!g_clear_mask_module[i = modules[k]]) {
        VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[i], .pCode = code[i]};
        if (vkCreateShaderModule(V.dev, &sm, NULL, &g_clear_mask_module[i])) { g_clear_mask_module[i] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_clear_mask_module[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_clear_mask_module[1 + type], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = mask};
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att};
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dy, .layout = g_clear_mask_layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vk_pcache_graphics(&ci, &pipeline)) return VK_NULL_HANDLE;
    g_clear_mask_pipe[g_clear_mask_npipe++] = (__typeof__(g_clear_mask_pipe[0])){format, mask, samples, pipeline};
    return pipeline;
}
/* One rendering pass per layer/slice over level lv, LOAD/STORE, clearing only the region (and channels). 0 when an
   attachment view or pipeline is unavailable: the caller falls back to the whole-subresource clear. */
static int clear_tex_region(const p_clear_tex *c, vk_tex *x, VkCommandBuffer cb, unsigned lv, unsigned first, unsigned layers,
                            VkImageAspectFlags as, const VkClearValue *value, uint32_t mask) {
    uint32_t w = vk_tex_width(x, lv), h = vk_tex_height(x, lv);
    int f = c->has_view && c->view.set_format ? c->view.format : c->tex->d.format;
    { int bw, bh, vbw, vbh; nvn_fmt_bpb(c->tex->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh); if (bw != vbw || bh != vbh) return 0; }
    int64_t x0 = MAXV(c->r.xoffset, 0), y0 = MAXV(c->r.yoffset, 0);
    int64_t x1 = MINV((int64_t)c->r.xoffset + c->r.width, (int64_t)w), y1 = MINV((int64_t)c->r.yoffset + c->r.height, (int64_t)h);
    if (x1 <= x0 || y1 <= y0) return 1;   /* nothing of the level is inside the region */
    VkRect2D rect = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    int color = !(as & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (color && mask != 0xf) {
        VkFormat vf; if (!vk_fmt(f, &vf, NULL) || !vf) return 0;
        int cls = vk_fmt_class(f);
        pipeline = clear_mask_pipeline(vf, mask, (VkSampleCountFlagBits)vk_tex_samples(x), cls == 1 ? 1 : cls == 2 ? 2 : 0);
        if (!pipeline) return 0;
    }
    VkImageView views[64]; if (layers > 64) return 0;
    for (unsigned k = 0; k < layers; k++) {
        view_t v = c->has_view ? c->view : (view_t){0};
        v.set_levels = 1; v.base_level = lv; v.nlevels = 1; v.set_layers = 1; v.min_layer = first + k; v.nlayers = 1;
        if (!(views[k] = vk_tex_view(x, c->tex, &v, 1))) return 0;
    }
    for (unsigned k = 0; k < layers; k++) {
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = views[k], .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
            .colorAttachmentCount = color, .pColorAttachments = color ? &a : NULL,
            .pDepthAttachment = as & VK_IMAGE_ASPECT_DEPTH_BIT ? &a : NULL, .pStencilAttachment = as & VK_IMAGE_ASPECT_STENCIL_BIT ? &a : NULL};
        vkCmdBeginRendering(cb, &ri);
        if (pipeline) {
            VkViewport viewport = {0, 0, (float)w, (float)h, 0, 1};
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetViewport(cb, 0, 1, &viewport); vkCmdSetScissor(cb, 0, 1, &rect);
            vkCmdPushConstants(cb, g_clear_mask_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, value->color.uint32);
            vkCmdDraw(cb, 3, 1, 0, 0);
        } else {
            VkClearAttachment ca = {as, 0, *value}; VkClearRect cr = {rect, 0, 1};
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }
        vkCmdEndRendering(cb);
    }
    if (pipeline) { g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = g_pipe_dirty = 1; descriptor_unbind(); }
    return 1;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_clear_counts(uint64_t out[2]) { out[0] = g_clear_region; out[1] = g_clear_masked; }
#endif
static void clear_tex(const p_clear_tex *c) {   /* clear a texture region with a value, outside any pass */
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else vk_ub_touch((uint64_t)(uintptr_t)c->tex->mem, c->tex->L.size);
    vk_tex *x = vk_tex_get(c->tex); if (!x) return;
    unsigned lv = vlevel(&c->view, c->has_view), l0 = vlayer(&c->view, c->has_view);
    int nz = c->r.depth ? c->r.depth : 1;
    VkImageAspectFlags as = vk_tex_aspect(x);
    int color = !(as & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    uint32_t mask = color ? (uint32_t)c->mask & 0xf : 0xf;
    if (!mask) return;   /* no channel is written */
    int partial = c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width != vk_tex_width(x,lv) || (uint32_t)c->r.height != vk_tex_height(x,lv);
    int native_region = partial && mask == 0xf && vk_tex_native_active(x) && !lv && !l0 && !c->r.zoffset && nz == 1;
    vk_end_rendering_at(VK_PASS_END_CLEAR, OP_CLEAR_TEX);
    vk_tex_native_read(x);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    VkImageSubresourceRange rr = {as, lv, 1, vk_tex_is3d(x) ? 0 : l0 + c->r.zoffset, vk_tex_is3d(x) ? 1 : (uint32_t)nz};
    VkClearValue value;
    if (!color) value.depthStencil = (VkClearDepthStencilValue){vk_tex_format(x) == VK_FORMAT_D16_UNORM ? d16_clear(c->c.f[0]) : c->c.f[0], (as & VK_IMAGE_ASPECT_DEPTH_BIT) ? c->c.u[1] : c->c.u[0]};
    else {
        memcpy(&value.color, c->c.u, 16);
        if (!c->is_uint) { int f = c->has_view && c->view.set_format ? c->view.format : c->tex->d.format; int cls = vk_fmt_class(f); if (cls) { for (int i = 0; i < 4; i++) value.color.uint32[i] = (uint32_t)c->c.f[i]; } }
    }
    int region = (partial || mask != 0xf) && !native_region;
    if (region) {
        /* 3D: slices zoffset.. of the level through 2D views; arrays: layers from the view's first layer */
        unsigned first = vk_tex_is3d(x) ? (unsigned)c->r.zoffset : l0 + (unsigned)c->r.zoffset;
        if (clear_tex_region(c, x, cb, lv, first, (unsigned)nz, as, &value, mask)) {
            g_clear_region += partial; g_clear_masked += mask != 0xf;
            rr.baseArrayLayer = first; rr.layerCount = (uint32_t)nz;
        } else {
            region = 0;
            RED(4, "texture clear of a region (%d) or channel mask %#x without a render-target view: cleared whole", partial, mask);
        }
    }
    if (!region) {
        if (!color) vkCmdClearDepthStencilImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &value.depthStencil, 1, &rr);
        else if (native_region) {
            VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView=vk_tex_view(x,c->tex,c->has_view?&c->view:NULL,1),.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
                .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea={{0,0},{vk_tex_width(x,0),vk_tex_height(x,0)}},.layerCount=1,
                .colorAttachmentCount=1,.pColorAttachments=&a};
            VkClearAttachment ca={VK_IMAGE_ASPECT_COLOR_BIT,0,value};
            VkClearRect rect={{{c->r.xoffset,c->r.yoffset},{c->r.width,c->r.height}},0,1};
            vkCmdBeginRendering(cb,&ri); vkCmdClearAttachments(cb,1,&ca,1,&rect); vkCmdEndRendering(cb);
        } else vkCmdClearColorImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &value.color, 1, &rr);
    }
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (native_region) vk_tex_native_patch(x,lv,l0,&c->r);
    else vk_tex_native_written(x, 0);
    if (vk_fg_extract_active) fg_extract_texture_write(c->tex, lv, rr.baseArrayLayer, rr.layerCount, as, 1, region, 0);
    vk_tex_mark_gpu_written(x); probe_write_note('K', x, NULL, 0);
    vk_pso_life_external_write(x, lv, rr.baseArrayLayer, rr.layerCount, "texture-clear");
}

/* ---------- the walker's other ops ---------- */

void vk_draw_exec(uint32_t op, const void *a) {
    state_init();
    if (R.open && (op==OP_CLEAR_COLOR || op==OP_CLEAR_DS)) R.feedback_written|=0x3ffu;
    if (R.open && R.merge_pending && (op == OP_REPORT_COUNTER || op == OP_RESET_COUNTER || op == OP_RENDER_COND))
        vk_end_rendering_at(VK_PASS_END_QUERY, op);
    if (vk_fg_extract_active && R.open) switch (op) {
        case OP_BIND_STATE: fg_extract_state_change(0); break;
        default: break;
    }
    switch (op) {
        case OP_SET_RT: {
            const p_set_rt *r = a; g_pops.setrt++;
            vk_stage_sig_invalidate(1u << 1);   /* also covers the same-target branch's state copy */
            int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
            if (R.open && !pend && S.has_rt && rt_same(&S.rt, r)) { S.rt = *r; break; }   /* the same targets again: keep the pass */
            vk_end_rendering_at(VK_PASS_END_ATTACHMENTS, 0);
            memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
            S.rt = *r; S.has_rt = 1;
            break;
        }
        case OP_CLEAR_COLOR: { const p_clear_color *c = a;
            if (PD.active) { char l[160]; pd_note(l, snprintf(l, sizeof l, "  clear colour %d mask %#x value %g,%g,%g,%g%s\n", c->index, c->mask, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3], S.render_disabled ? " (render disabled: skipped)" : "")); }
            clear_color(a); break; }
        case OP_CLEAR_DS: {
            const p_clear_ds *c=a;
            if (FGX.authority_span.open && c->depth_mask && !S.render_disabled)
                vk_end_rendering_at(VK_PASS_END_COPY,0); /* clear is not an unproven raster write */
            clear_ds(a); break;
        }
        case OP_DRAW: draw(a, NULL); if (serial_draws()) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 3); break;
        case OP_DRAW_IDX: draw(NULL, a); if (serial_draws()) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 3); break;
        case OP_DISPATCH: VK_REC_DO(VK_REC_O_DISPATCH, dispatch(a)); break;
        case OP_COPY_B2T: VK_REC_DO(VK_REC_O_TRANSFER, copy_b2t(a)); break;
        case OP_COPY_T2B: VK_REC_DO(VK_REC_O_TRANSFER, copy_t2b(a)); break;
        case OP_COPY_T2T: VK_REC_DO(VK_REC_O_TRANSFER, copy_t2t(a)); break;
        case OP_CLEAR_TEX: VK_REC_DO(VK_REC_O_TRANSFER, clear_tex(a)); break;
        case OP_BARRIER: {
            int bits; memcpy(&bits, a, 4);
            if (!R.open) { VK_REC_DO(VK_REC_O, vk_barrier_all(vk_cb())); }
            else if ((bits & ~NVN_BARRIER_ORDER_PRIMITIVES_BIT) && pass_barrier_on()) {
                if (feedback_narrow_barrier((uint32_t)bits)) break;
                if (pass_merge_barrier((uint32_t)bits)) break;
                vk_end_rendering_at(bits & NVN_BARRIER_INVALIDATE_TEXTURE_BIT ? VK_PASS_END_TEXTURE_BARRIER : VK_PASS_END_NVN_BARRIER, (uint32_t)bits);
                g_stat_inpass_barriers++;
            }   /* the next draw reopens the pass (load) */
            break; }
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
        case OP_BIND_PROGRAM: {
            const p_bind_program *b = a;
            if (vk_fg_extract_active && R.open && S.prog != b->prog) {
                fg_extract_state_change(1);
            }
            S.prog = b->prog; g_pipe_dirty = 1;
            vk_prog_prepare(S.prog);
            break;
        }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; blend_st bs;
            if (b->kind >= 0 && b->kind < 5) g_pipe_dirty = 1;
            if (vk_fg_extract_active && (b->kind == 2 || b->kind == 5)) fg_extract_state_change(1);
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target & 7] = bs; break;
                case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
                case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
                case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
                case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
            }
            if (vk_fg_extract_active && R.open) {
                if (b->kind == 2 || FGX.pass.depth_write != (unsigned)(R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw))
                    fg_pass_guard_invalidate(&FGX.guard);
                FGX.pass.depth_write = R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
            }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->n >= 0 && b->n <= 16) {
            int changed = !vk_record_enabled() || S.nattr != b->n || memcmp(S.attr, b->st, 4 * (size_t)b->n);
            memcpy(S.attr, b->st, 4 * (size_t)b->n); S.nattr = b->n; g_pipe_dirty = 1;
            if (changed) vk_stage_sig_invalidate(1u << 0);
        } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->n >= 0 && b->n <= 16) { memcpy(S.stream, b->st, 8 * (size_t)b->n); S.nstream = b->n; g_pipe_dirty = 1; } break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) {
            p_bind_buf *old = &S.ubo[b->stage][b->index];
            int camera_bank=(b->stage==0 && (b->index==1 || b->index==5 ||
                b->index==6 || b->index==11)) || (b->stage==1 && b->index==1);
            if (vk_fg_extract_active && R.open && camera_bank &&
                (old->addr != b->addr || old->size != b->size)) {
                fg_pass_guard_invalidate(&FGX.guard);
            }
            if (b->stage < 2 && (old->addr != b->addr || old->size != b->size))
                g_uniform_prepare_dirty[b->stage] |= (uint16_t)(1u << b->index);
            if (vk_record_enabled() && (old->addr != b->addr || old->size != b->size)) g_record_bind_revision[b->stage]++;
            if (b->stage == 5 && (old->addr != b->addr || (uint32_t)old->size != (uint32_t)b->size)) g_driver_revision[0][5]++;
            *old = *b;
        } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) {
            p_bind_buf *old = &S.ssbo[b->stage][b->index];
            if (vk_record_enabled() && (old->addr != b->addr || old->size != b->size)) g_record_bind_revision[b->stage]++;
            if (old->addr != b->addr || (uint32_t)old->size != (uint32_t)b->size) g_driver_revision[0][b->stage]++;
            *old = *b;
        } break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) {
            int changed = memcmp(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n) || b->first + b->n > S.tex_hi[b->stage];
            if (vk_fg_extract_active && R.open && changed) fg_extract_state_change(1);
            if (vk_record_enabled() && changed) g_record_bind_revision[b->stage]++;
            int signature_changed = !vk_record_enabled();
            for (int i = 0; !signature_changed && i < b->n; i++)
                signature_changed = (S.tex[b->stage][b->first + i] & 0xfffff) != (b->h[i] & 0xfffff);
            for (int i = 0; i < b->n; i++) if ((uint32_t)S.tex[b->stage][b->first + i] != (uint32_t)b->h[i]) { g_driver_revision[1][b->stage]++; break; }
            memcpy(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); if (b->first + b->n > S.tex_hi[b->stage]) S.tex_hi[b->stage] = b->first + b->n;
            if (vk_native_ui_enabled() && vk_native_descriptor_revision()) native_bound_update(b->stage,0,b->first,b->n);
            if (signature_changed) vk_stage_sig_invalidate(1u << b->stage);
        } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) {
            if (vk_record_enabled() && memcmp(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n)) g_record_bind_revision[b->stage]++;
            for (int i = 0; i < b->n; i++) if ((uint32_t)S.samp[b->stage][b->first + i] != (uint32_t)b->h[i]) { g_driver_revision[1][b->stage]++; break; }
            memcpy(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n);
        } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) {
            if (vk_fg_extract_active && R.open && memcmp(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n))
                fg_extract_state_change(1);
            if (vk_record_enabled() && memcmp(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n)) g_record_bind_revision[b->stage]++;
            memcpy(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n);
            if (vk_native_ui_enabled() && vk_native_descriptor_revision()) native_bound_update(b->stage,1,b->first,b->n);
        } break; }
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) S.st_wmask[f] = (uint32_t)b->mask; else S.st_ref[f] = (uint32_t)b->ref; } break; }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a;
            if (vk_fg_extract_active && R.open && FGX.pass.camera_addr && b->first == 0 && b->n > 0 &&
                memcmp(S.vp[0], b->v[0], 16)) {
                fg_pass_guard_invalidate(&FGX.guard);
            }
            if (g_overlay_enabled && b->first == 0 && b->n > 0 &&
                (!S.has_vp || memcmp(S.vp[0], b->v[0], 16))) g_overlay_scene_dirty = 1;
            for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], 16);
            S.has_vp = 1; break;
        }
        case OP_DEPTH_RANGE: {
            const p_depth_range *b=a;
            if (vk_fg_extract_active && R.open && (S.dr[0]!=b->n || S.dr[1]!=b->f))
                fg_pass_guard_invalidate(&FGX.guard);
            S.dr[0]=b->n; S.dr[1]=b->f; break;
        }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_ALPHA_REF: {
            float value=((const p_alpha_ref *)a)->value;
            value=fminf(1,fmaxf(0,value));
            if (memcmp(&S.alpha_ref,&value,4)) g_record_bind_revision[1]++;
            S.alpha_ref=value; break;
        }
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value; g_pipe_dirty = 1; break;
        case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value;
            if (PD.active) { char l[64]; pd_note(l, snprintf(l, sizeof l, "  render enable %d\n", !S.render_disabled)); }
            break;
        case OP_RENDER_COND: {   /* render if the counters at addr / addr + 16 are equal (mode 0) or not (1), with reports still pending counted (nvn_vk.c) */
            const p_render_cond *c = a; uint64_t x = 0, y = 0; int known = 1;
            if (c->addr) { known = vk_counter_value(c->addr, &x); known &= vk_counter_value(c->addr + 16, &y); }
            S.render_disabled = known && (c->mode == 0 ? x != y : x == y);   /* pending real samples: visible, as Metal's cond_eval */
            g_pops.cond++; if (S.render_disabled) g_pops.cond_off++;
            if (PD.active) { char l[200]; pd_note(l, snprintf(l, sizeof l, "  render cond %#llx mode %d: %llu vs %llu -> %s%s\n", (unsigned long long)c->addr, c->mode, (unsigned long long)x, (unsigned long long)y, S.render_disabled ? "off" : "on", known ? "" : " (pending samples: visible)")); }
            break;
        }
        case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value; break;
        case OP_REPORT_COUNTER:
            if (PD.active) { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); char l[96]; pd_note(l, snprintf(l, sizeof l, "  report counter type %llu at %#llx\n", (unsigned long long)t, (unsigned long long)ad)); }
            break;
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { g_samples_cum = 0; vk_occlusion_reset(); } break; }
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
        default: break;
    }
}
/* the dump's note of a buffer write made outside the draw stream (nvn_vk.c: inline updates, buffer copies) */
void vk_pd_write_note(const char *what, uint64_t dst, uint64_t n, const void *data) {
    if (!PD.active || !n) return;
    char l[200]; int k = snprintf(l, sizeof l, "  %s %#llx +%#llx", what, (unsigned long long)dst, (unsigned long long)n);
    if (data) for (uint64_t i = 0; i < n && i < 32 && k < (int)sizeof l - 12; i += 8) { uint64_t v = 0; memcpy(&v, (const uint8_t *)data + i, (size_t)MINV(8, n - i)); k += snprintf(l + k, sizeof l - (size_t)k, " %llx", (unsigned long long)v); }
    if (k < (int)sizeof l - 1) { l[k++] = '\n'; l[k] = 0; }
    pd_note(l, k);
}

/* ---------- present: count frames; TOTK_VK_SHOT=<n> saves every n-th presented image as PNG (headless check) ---------- */
static uint64_t g_present_n, g_present_t0, g_last_ticket; static double g_pres_t[64];
static uint32_t crc_table[256];
static uint32_t crc32_(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8); return ~c;
}

/* ---------- pass dump (TOTK_INPUT_FILE "passdump <n>", n frames from the one after next) ----------
   Every pass's attachments (each colour target and the depth) as PNG files in passdump/f<frame>/, plus index.txt (pass
   number, targets with serial / size / NVN format, draws, first fragment shader). For finding the pass where a picture
   problem first appears. Rows are sampled on the GPU (216 rows per attachment, full width) into one host buffer and
   converted on the CPU at the frame's present; float formats are scaled from their own min..max per channel (so linear
   depth or HDR colour is visible), depth likewise. Slow; a diagnostic only. */
#define PD_ROWS 216
#define PD_COLS 384
#define PD_BUF (1024ull << 20)
typedef struct { int pass, att; VkFormat f; int nvnfmt; uint32_t w, h, rows; VkDeviceSize off, rowbytes; uint64_t serial; unsigned level, layer; uint32_t tb; } pd_img;
static long g_pd_first, g_pd_last;   /* frames (present numbers) to dump */
static char g_pdd[32][240]; static int g_npdd;   /* the open pass's draws (index.txt lists them for passes of up to 32) */
static int pd_active(void);
static void pd_draw_note(const char *fmt, ...) {
    if (!pd_active() || g_npdd >= 32) { if (pd_active()) g_npdd++; return; }
    va_list ap; va_start(ap, fmt); vsnprintf(g_pdd[g_npdd], sizeof g_pdd[0], fmt, ap); va_end(ap); g_npdd++;
}
/* TOTK_VK_PD_SPLIT=<fs prefix>[,...] or d16 (diagnostic, with passdump): during a dumped frame, the pass ends after every draw
   whose fragment shader key starts with one of these (d16: every draw in a pass with colour targets and a DEPTH16 depth),
   so each draw's result is dumped on its own. */
static int pd_split_match(const char *key) {
    static const char *e; static int init; if (!init) { init = 1; e = getenv("TOTK_VK_PD_SPLIT"); }
    if (!e || !*e || !key || !pd_active()) return 0;
    if (!strcmp(e, "d16")) return R.dfmt == VK_FORMAT_D16_UNORM && R.ncolor > 0;   /* every pass with colour targets and a DEPTH16 depth (the deferred lighting) */
    for (const char *p = e; *p; ) { const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p); if (l && !strncmp(key, p, l)) return 1; p += l; if (*p == ',') p++; }
    return 0;
}
static VkBuffer g_pd_buf; static VkDeviceMemory g_pd_mem; static uint8_t *g_pd_ptr; static VkDeviceSize g_pd_used;
static pd_img *g_pd; static int g_npd, g_cappd, g_pd_pass; static char *g_pd_idx; static size_t g_pd_idxn, g_pd_idxcap; static int g_pd_full;
void dm_gfx_passdump_next(long n) {
    g_pd_first = (long)g_present_n + 2; g_pd_last = g_pd_first + n - 1;
    LOGV("passdump: frames %ld to %ld into passdump/", g_pd_first, g_pd_last);
}
static int pd_active(void) {
    long f = (long)g_present_n + 1; return g_pd_first && f >= g_pd_first && f <= g_pd_last; }
static void pd_idx(const char *fmt, ...) {
    char line[600]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap); if (n < 0) return; if (n >= (int)sizeof line) n = sizeof line - 1;
    if (g_pd_idxn + (size_t)n + 1 > g_pd_idxcap) { g_pd_idxcap = (g_pd_idxcap + (size_t)n + 1) * 2; g_pd_idx = realloc(g_pd_idx, g_pd_idxcap); }
    memcpy(g_pd_idx + g_pd_idxn, line, (size_t)n); g_pd_idxn += (size_t)n; g_pd_idx[g_pd_idxn] = 0;
}
static uint32_t pd_texel_bytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SNORM: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16: return 2;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 4;
    }
}
static void pd_sample_copy(VkCommandBuffer cb, vk_tex *x, VkImageAspectFlags aspect, VkFormat f, int att, uint64_t serial, int nvnfmt, unsigned level, unsigned layer) {
    if (!x || vk_tex_samples(x) > 1) return;
    vk_tex_native_read(x);
    { int bw, bh; nvn_fmt_bpb(vk_tex_nvn_format(x), &bw, &bh); if (bw > 1 || bh > 1) { pd_idx("    att %d: block-compressed target, not dumped\n", att); return; } }
    if (level >= vk_tex_levels(x)) return;
    uint32_t w = vk_tex_width(x, level), h = vk_tex_height(x, level), rows = MINV(h, (uint32_t)PD_ROWS);
    uint32_t tb = aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? (f == VK_FORMAT_D16_UNORM ? 2 : 4) : aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : pd_texel_bytes(f);
    VkDeviceSize rb = ((VkDeviceSize)w * tb + 3) & ~(VkDeviceSize)3, need = rb * rows;
    if (!g_pd_buf) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PD_BUF, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vkCreateBuffer(V.dev, &bci, NULL, &g_pd_buf)) { g_pd_full = 1; return; }
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, g_pd_buf, &req);
        int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
        if (t < 0 || vk_memory_allocate(&mai, &g_pd_mem, VK_MEMORY_DIAGNOSTIC)) { vkDestroyBuffer(V.dev, g_pd_buf, NULL); g_pd_buf = VK_NULL_HANDLE; g_pd_full = 1; return; }
        vkBindBufferMemory(V.dev, g_pd_buf, g_pd_mem, 0); vkMapMemory(V.dev, g_pd_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_pd_ptr); g_pd_used = 0;
    }
    if (g_pd_used + need > PD_BUF) { if (!g_pd_full) LOGV("passdump: buffer full at pass %d", g_pd_pass); g_pd_full = 1; return; }
    int is3d = vk_tex_is3d(x);
    VkBufferImageCopy *rg = calloc(rows, sizeof *rg);
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y = rows == h ? r : (uint32_t)(((uint64_t)r * h + h / 2) / rows); if (y >= h) y = h - 1;
        rg[r] = (VkBufferImageCopy){.bufferOffset = g_pd_used + r * rb, .bufferRowLength = 0, .imageSubresource = {aspect, level, is3d ? 0 : layer, 1},
            .imageOffset = {0, (int32_t)y, is3d ? (int32_t)layer : 0}, .imageExtent = {w, 1, 1}};
    }
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_pd_buf, rows, rg);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    free(rg);
    if (g_npd == g_cappd) { g_cappd = g_cappd ? 2 * g_cappd : 256; g_pd = realloc(g_pd, (size_t)g_cappd * sizeof *g_pd); }
    g_pd[g_npd++] = (pd_img){g_pd_pass, att, aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? VK_FORMAT_S8_UINT : f, nvnfmt, w, h, rows, g_pd_used, rb, serial, level, layer, tb};
    g_pd_used += (need + 255) & ~(VkDeviceSize)255;
}
static void pd_pass(VkCommandBuffer cb) {
    if (!pd_active()) return;
    int k = g_pd_pass;
    pd_idx("f%llu pass %d: %llu draws, fs %s%s\n", (unsigned long long)(g_present_n + 1), k, (unsigned long long)(g_stat_draws - R.draws0), g_pkey[0] ? g_pkey : "-", R.samples > 1 ? ", multisampled (not dumped)" : "");
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.ctex[i]) {
        pd_idx("    c%u: ser %llu %ux%u nvnfmt %#x vk %d level %u layer %u\n", i, (unsigned long long)R.c_serial[i], vk_tex_width(R.ctex[i], R.clevel[i]), vk_tex_height(R.ctex[i], R.clevel[i]), R.c_nvnfmt[i], R.cfmt[i], R.clevel[i], R.clayer[i]);
        pd_sample_copy(cb, R.ctex[i], VK_IMAGE_ASPECT_COLOR_BIT, R.cfmt[i], (int)i, R.c_serial[i], R.c_nvnfmt[i], R.clevel[i], R.clayer[i]);
    }
    if (R.dtex) {
        VkImageAspectFlags as = vk_tex_aspect(R.dtex);
        pd_idx("    depth: ser %llu %ux%u vk %d level %u layer %u\n", (unsigned long long)R.d_serial, vk_tex_width(R.dtex, R.dlevel), vk_tex_height(R.dtex, R.dlevel), vk_tex_format(R.dtex), R.dlevel, R.dlayer);
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) pd_sample_copy(cb, R.dtex, VK_IMAGE_ASPECT_DEPTH_BIT, vk_tex_format(R.dtex), 8, R.d_serial, 0, R.dlevel, R.dlayer);
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) pd_sample_copy(cb, R.dtex, VK_IMAGE_ASPECT_STENCIL_BIT, vk_tex_format(R.dtex), 9, R.d_serial, 0, R.dlevel, R.dlayer);
    }
    /* what the pass's draws sampled; screen-space passes (few draws) also get their inputs dumped (level 0, layer 0) */
    uint64_t nd = g_stat_draws - R.draws0;
    if (g_npdd <= 32) for (int i = 0; i < g_npdd; i++) pd_idx("%s\n", g_pdd[i]);
    g_npdd = 0;
    for (int i = 0; i < g_npin; i++) {
        vk_tex *x = g_pin[i].x; if (!x) continue;
        pd_idx("    in%d: ser %llu %ux%u nvnfmt %#x vk %d%s\n", i, (unsigned long long)vk_tex_serial(x), g_pin[i].w, g_pin[i].h, g_pin[i].fmt, vk_tex_format(x), vk_tex_is3d(x) ? " 3D" : "");
        if (nd > 16 || vk_tex_is3d(x)) continue;
        VkImageAspectFlags as = vk_tex_aspect(x);
        pd_sample_copy(cb, x, as & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, vk_tex_format(x), 10 + i, vk_tex_serial(x), g_pin[i].fmt, 0, 0);
    }
    vk_barrier_all(cb);
    g_pd_pass++;
}
static float pd_h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, v;
    if (!e) { if (!m) v = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; v = s | e << 23 | m << 13; } }
    else if (e == 31) v = s | 0xff << 23 | m << 13; else v = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &v, 4); return f;
}
static float pd_uf(uint32_t bits, int mb) {   /* unsigned 11/10-bit float, 5-bit exponent */
    uint32_t e = bits >> mb, m = bits & ((1u << mb) - 1);
    if (!e) return ldexpf((float)m, -14 - mb); if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mb), (int)e - 15);
}
/* texel -> up to 4 channels (float); returns the channel count */
static int pd_decode_texel(VkFormat f, const uint8_t *p, float c[4]) {
    uint32_t u; memcpy(&u, p, 4);
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R8G8B8A8_UINT: for (int i = 0; i < 4; i++) c[i] = p[i] / 255.0f; return 4;
        case VK_FORMAT_R8G8B8A8_SNORM: for (int i = 0; i < 4; i++) c[i] = (int8_t)p[i] / 127.0f; return 4;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; c[3] = p[3] / 255.0f; return 4;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: c[0] = (u & 1023) / 1023.0f; c[1] = (u >> 10 & 1023) / 1023.0f; c[2] = (u >> 20 & 1023) / 1023.0f; c[3] = (u >> 30) / 3.0f; return 4;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: c[0] = pd_uf(u & 2047, 6); c[1] = pd_uf(u >> 11 & 2047, 6); c[2] = pd_uf(u >> 22, 5); return 3;
        case VK_FORMAT_R16G16B16A16_SFLOAT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = pd_h2f(h); } return 4;
        case VK_FORMAT_R16G16_SFLOAT: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = pd_h2f(h); } return 2;
        case VK_FORMAT_R16_SFLOAT: { uint16_t h; memcpy(&h, p, 2); c[0] = pd_h2f(h); return 1; }
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h; } return 4;
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SNORM: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h; } return 2;
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: { uint16_t h; memcpy(&h, p, 2); c[0] = h; return 1; }
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: memcpy(c, p, 4); return 1;
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: c[0] = (float)(u & 0xffffff); return 1;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: c[0] = (float)u; return 1;
        case VK_FORMAT_R32G32_SFLOAT: memcpy(c, p, 8); return 2;
        case VK_FORMAT_R32G32_UINT: { uint32_t v[2]; memcpy(v, p, 8); c[0] = (float)v[0]; c[1] = (float)v[1]; return 2; }
        case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(c, p, 16); return 4;
        case VK_FORMAT_R32G32B32A32_UINT: { uint32_t v[4]; memcpy(v, p, 16); for (int i = 0; i < 4; i++) c[i] = (float)v[i]; return 4; }
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: c[0] = p[0] / 255.0f; return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; return 2;
        default: c[0] = (u & 255) / 255.0f; c[1] = (u >> 8 & 255) / 255.0f; c[2] = (u >> 16 & 255) / 255.0f; c[3] = (u >> 24) / 255.0f; return 4;
    }
}
static int pd_unorm8(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8G8_UNORM;
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch);
static void pd_frame_end(uint64_t n) {
    if (!g_pd_first || (long)n < g_pd_first || (long)n > g_pd_last) return;
    vk_rec_lock(); vk_flush_wait();
    char dir[64]; mkdir("passdump", 0755); snprintf(dir, sizeof dir, "passdump/f%llu", (unsigned long long)n); mkdir(dir, 0755);
    for (int i = 0; i < g_npd; i++) {
        pd_img *d = &g_pd[i];
        uint32_t cols = MINV(d->w, (uint32_t)PD_COLS), tb = d->tb;
        float *v = malloc((size_t)cols * d->rows * 4 * sizeof *v); int nch = 1;
        float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        VkFormat cf = d->att == 9 ? VK_FORMAT_S8_UINT : d->f;
        for (uint32_t r = 0; r < d->rows; r++) for (uint32_t x = 0; x < cols; x++) {
            uint32_t sx = cols == d->w ? x : (uint32_t)(((uint64_t)x * d->w + d->w / 2) / cols); if (sx >= d->w) sx = d->w - 1;
            float c[4] = {0, 0, 0, 1}; nch = pd_decode_texel(cf, g_pd_ptr + d->off + r * d->rowbytes + (size_t)sx * tb, c);
            for (int k = 0; k < 4; k++) { v[((size_t)r * cols + x) * 4 + k] = c[k]; if (k < nch && isfinite(c[k])) { if (c[k] < lo[k]) lo[k] = c[k]; if (c[k] > hi[k]) hi[k] = c[k]; } }
        }
        int fixed = pd_unorm8(cf);
        uint8_t *px = malloc((size_t)cols * d->rows * 4);
        for (size_t t = 0; t < (size_t)cols * d->rows; t++) {
            for (int k = 0; k < 4; k++) {
                float c = v[t * 4 + k]; uint8_t o;
                if (k >= nch) o = k == 3 ? 255 : (nch == 1 ? px[t * 4] : 0);
                else if (!isfinite(c)) o = k == 0 ? 255 : k == 1 ? 0 : 255;   /* NaN / inf: magenta */
                else if (fixed) o = (uint8_t)(c < 0 ? 0 : c > 1 ? 255 : c * 255.0f + 0.5f);
                else { float a = lo[k], b = hi[k]; o = b > a ? (uint8_t)((c - a) / (b - a) * 255.0f + 0.5f) : 128; }
                px[t * 4 + k] = o;
            }
            if (nch < 4) px[t * 4 + 3] = 255;
            if (nch == 1) px[t * 4 + 1] = px[t * 4 + 2] = px[t * 4];   /* grey */
            if (nch >= 4 && !fixed) px[t * 4 + 3] = 255;
            if (fixed) px[t * 4 + 3] = 255;   /* alpha shown opaque; its range is in index.txt */
        }
        char path[160]; const char *kind = d->att == 8 ? "depth" : d->att == 9 ? "stencil" : "c";
        if (d->att >= 10) snprintf(path, sizeof path, "%s/p%03d_in%d_ser%llu_%ux%u.png", dir, d->pass, d->att - 10, (unsigned long long)d->serial, d->w, d->h);
        else if (d->att < 8) snprintf(path, sizeof path, "%s/p%03d_c%d_%ux%u.png", dir, d->pass, d->att, d->w, d->h);
        else snprintf(path, sizeof path, "%s/p%03d_%s_%ux%u.png", dir, d->pass, kind, d->w, d->h);
        write_png(path, px, cols, d->rows, (size_t)cols * 4);
        pd_idx("p%03d %s%d: ranges", d->pass, d->att >= 10 ? "in" : d->att < 8 ? "c" : kind, d->att >= 10 ? d->att - 10 : d->att < 8 ? d->att : 0);
        for (int k = 0; k < nch; k++) pd_idx(" [%g, %g]", (double)lo[k], (double)hi[k]);
        pd_idx("%s\n", fixed ? " (unorm: shown as is)" : " (scaled to its range; magenta = NaN / inf)");
        free(v); free(px);
    }
    char ip[96]; snprintf(ip, sizeof ip, "%s/index.txt", dir);
    FILE *f = fopen(ip, "w"); if (f) { if (g_pd_idx) fwrite(g_pd_idx, 1, g_pd_idxn, f); fclose(f); }
    LOGV("passdump: frame %llu, %d passes, %d images in %s%s", (unsigned long long)n, g_pd_pass, g_npd, dir, g_pd_full ? " (buffer full: later passes missing)" : "");
    g_npd = 0; g_pd_pass = 0; g_pd_idxn = 0; if (g_pd_idx) g_pd_idx[0] = 0; g_pd_used = 0; g_pd_full = 0;
    if ((long)n >= g_pd_last) {
        g_pd_first = g_pd_last = 0;
        if (g_pd_buf) { vkUnmapMemory(V.dev, g_pd_mem); vkDestroyBuffer(V.dev, g_pd_buf, NULL); vk_memory_release(g_pd_mem, VK_MEMORY_DIAGNOSTIC); g_pd_buf = VK_NULL_HANDLE; g_pd_mem = VK_NULL_HANDLE; g_pd_ptr = NULL; }
    }
    vk_rec_unlock();
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n}; fwrite(len, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_((const uint8_t *)type, 4, 0); c = crc32_(data, n, c);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(cb, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch) {   /* uncompressed (stored) deflate: no zlib needed */
    FILE *f = fopen(path, "wb"); if (!f) { RED(4, "cannot write %s", path); return; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4), blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + blocks * 5 + 4), *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) { raw[y * (1 + (size_t)w * 4)] = 0; memcpy(raw + y * (1 + (size_t)w * 4) + 1, rgba + y * pitch, (size_t)w * 4); }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw_n; at += 65535) {
        size_t n = raw_n - at < 65535 ? raw_n - at : 65535;
        z[o++] = at + n >= raw_n; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + at, n); o += n;
    }
    uint32_t ad = b << 16 | a; z[o++] = (uint8_t)(ad >> 24); z[o++] = (uint8_t)(ad >> 16); z[o++] = (uint8_t)(ad >> 8); z[o++] = (uint8_t)ad;
    png_chunk(f, "IDAT", z, (uint32_t)o); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(z); free(raw);
}
static void shot(const tex_t *t, uint64_t n, const int crop[4]) {
    vk_tex *x = vk_tex_get(t); if (!x) return;
    uint32_t w = vk_tex_width(x, 0), h = vk_tex_height(x, 0); int32_t cx = 0, cy = 0;
    /* the window crop (top-left origin, nvn_mtl.m present_encode): the game renders below 1080p into a 1080p texture */
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= w && (uint32_t)(crop[1] + crop[3]) <= h) {
        cx = crop[0]; cy = crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3];
    }
    vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 4);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    VkBuffer b; VkDeviceSize o; uint8_t *p = vk_transient((size_t)w * h * 4, 16, &b, &o);
    VkBufferImageCopy r = {.bufferOffset = o, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {cx, cy, 0}, .imageExtent = {w, h, 1}};
    int bpb = nvn_fmt_bpb(t->d.format, NULL, NULL);
    if (bpb != 4) { RED(2, "screenshot of a format %#x image (not 4 bytes per pixel) skipped", t->d.format); return; }
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    vk_flush_wait();
    VkFormat f = vk_tex_format(x);
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t c = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = c; }
    for (size_t i = 0; i < (size_t)w * h; i++) p[4 * i + 3] = 255;
    const char *dir = getenv("TOTK_VK_SHOT_DIR"); if (!dir || !*dir) dir = "shots";
    mkdir(dir, 0755);
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%06llu.png", dir, (unsigned long long)n);
    write_png(path, p, w, h, (size_t)w * 4);
    LOGV("saved %s (%ux%u)", path, w, h);
}
#ifdef TOTK_ANDROID_APP
/* ---------- the app's window (frontend_android.c) ----------
   Android default: render the final scale into imported AHardwareBuffers and give them to a SurfaceControl child.
   TOTK_VK_PRESENT=copy or any unavailable AHB capability falls back to host-visible readback and ANativeWindow post.
   Both paths enqueue without waiting on the recording thread; an occupied ring drops display copies, not game work.
   Three presenter buffers are active by default; slack 2 auto-selects four unless explicitly overridden.
   TOTK_PRESENT_SLOTS=1 reserves fixed display slots; copy waits only on this thread, AHB supplies a guarded desired time.
   The AHB path still waits for both GPU completion and the compositor's previous-buffer release before reuse. */
#include <android/native_window.h>
#include <stdatomic.h>
#ifdef __ANDROID__
#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include "vk_present_spv.h"
#include "vk_present_rotated_spv.h"
#include "vk_present_fence.h"
#include "vk_present_retire.h"
#endif
#include "vk_present_slots.h"
#include "vk_present_policy.h"
#include "vk_present_job.h"
#include "framegen_slots.h"
void front_frame_presented(void);
enum { NPRES = VK_PRESENT_SLOT_CAPACITY };
static int g_pres_active = VK_PRESENT_SLOT_DEFAULT_BUFFERS;
static int g_pres_guest_output; /* immutable before presenter startup */
/* Explicit experiment arm: 0 retains live frame queries for matched diagnostic A/B. */
static int g_guest_extent_cached = 1, g_guest_extent_diag;
enum { GUEST_QUERY_PRODUCER, GUEST_QUERY_PRESENTER, GUEST_QUERY_ROLES };
static struct { _Atomic uint64_t pairs, total_ns, max_ns; } g_guest_queries[GUEST_QUERY_ROLES];
/* Cached mode queries only on lifecycle events; the explicit legacy A/B arm polls through the timed wrapper. */
static vk_present_extent pres_window_extent(ANativeWindow *window) {
    if (!window) return (vk_present_extent){0};
    int32_t w=ANativeWindow_getWidth(window), h=ANativeWindow_getHeight(window);
    return w>0 && h>0 ? (vk_present_extent){(uint32_t)w,(uint32_t)h} : (vk_present_extent){0};
}
static vk_present_extent pres_frame_window_extent(ANativeWindow *window, unsigned role) {
    if (!window) return (vk_present_extent){0};
    uint64_t start = g_guest_extent_diag ? host_now_ns() : 0;
    vk_present_extent extent = pres_window_extent(window);
    if (g_guest_extent_diag) {
        uint64_t ns = host_now_ns() - start;
        atomic_fetch_add_explicit(&g_guest_queries[role].pairs, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_guest_queries[role].total_ns, ns, memory_order_relaxed);
        uint64_t max = atomic_load_explicit(&g_guest_queries[role].max_ns, memory_order_relaxed);
        while (max < ns && !atomic_compare_exchange_weak_explicit(&g_guest_queries[role].max_ns, &max, ns,
                memory_order_relaxed, memory_order_relaxed)) {}
    }
    return extent;
}
static void pres_extent_report(uint64_t frame) {
    if (!g_pres_guest_output || !g_guest_extent_diag || frame % 300) return;
    static const char *const names[] = {"producer", "presenter"};
    for (unsigned role = 0; role < GUEST_QUERY_ROLES; role++) {
        uint64_t pairs = atomic_exchange_explicit(&g_guest_queries[role].pairs, 0, memory_order_relaxed);
        uint64_t ns = atomic_exchange_explicit(&g_guest_queries[role].total_ns, 0, memory_order_relaxed);
        uint64_t max = atomic_exchange_explicit(&g_guest_queries[role].max_ns, 0, memory_order_relaxed);
        LOGV("[guest-extent] frame=%llu cache=%d role=%s query_pairs=%llu native_calls=%llu total_ms=%.6f max_us=%.3f (approximate 300-frame window; query wall time only)",
            (unsigned long long)frame, g_guest_extent_cached, names[role], (unsigned long long)pairs,
            (unsigned long long)(pairs * 2), ns / 1e6, max / 1e3);
    }
}
typedef struct {
    VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size; int coherent; _Atomic int busy, credited;
    uint64_t fence, generation, desired_ns, timeline_frame; uint32_t w, h; int swap, zero;
    vk_present_extent guest_window;
    vk_present_rect guest_fit; /* immutable cached geometry for this queued job */
    unsigned rotation; /* immutable clockwise quarter turns; w/h remain logical, AHB is native-oriented */
    int ubwc; /* independent compressed-allocation opt-in, admitted only by a nonidentity root hint */
    uint64_t refresh_ns, copy_open_ns, slot_skipped; uint32_t interval; /* immutable display-slot reservation; zero is legacy */
    uint64_t framegen_real; /* immutable real budget identity, independent of FT */
    runtime_present_timeline_entry display_timeline; /* immutable API tuple, or ID=-1 re-anchor */
    unsigned timeline_role; /* 0=legacy, 1=real, 2=generated; assigned with the reservation */
    int timeline_display; /* immutable window logical-display ID, -1 if attachment raced */
    uint64_t frame, ctr[10];   /* black_check: the frame and the counters it was drawn with */
    runtime_present_prediction prediction;
    runtime_fps_auto_frame auto_frame;
    uint64_t ready_ns; int prediction_done, auto_done, auto_cadence;
    uint64_t source_fence, source_slot; int generated, interpolated, discard;
    uint64_t adaptive_epoch;
    fgs_slot adaptive_slot;
    uint64_t adaptive_token;
    unsigned adaptive_slots, adaptive_eligible;
    unsigned adaptive_real_conflicts; /* an already-applied GEN cannot be retracted */
    uint64_t adaptive_release_ns; /* EARLY_APPLY=1 sticky earlier GEN hold end; zero is r7 */
    unsigned adaptive_latch_kind; /* fga_latch_kind of the last release evaluation */
    uint64_t adaptive_source_ready_ns, adaptive_output_ready_ns; /* GEN decision's CPU-ready observations; trace rows only */
    fgs_real_observation adaptive_real_observation; /* exact backend CPU publication, not GPU time */
    fgs_capacity_observation adaptive_capacity;
    unsigned adaptive_capacity_live_count;
    _Atomic int adaptive_suppressed;
    uint64_t record_done_ns, enqueue_ns, dequeue_ns, own_ready_ns, pair_ready_ns;
    unsigned queue_depth;
    vk_present_job job; /* reset for a NEW produced job, never a same-job YIELD */
    _Atomic uint64_t diag_apply_ns, diag_callback_ns, diag_previous_wait_ns;
} pres_buf;
static pres_buf g_pres[NPRES];
static unsigned char g_pres_qcom[NPRES]; /* immutable generated-picture provenance */
static unsigned char g_pres_lsfg[NPRES]; /* separate from current engine activation */
static int g_pres_trace; /* initialized before the presenter thread starts */
static int g_fg_job_trace; /* logging only; default off, same setting in both ABBA arms */
#ifdef __ANDROID__
static int g_pres_async_retire; /* exact opt-in, immutable before presenter startup */
#endif
static int g_pres_slot_diag; /* FRAMEGEN_DIAG only; no extra clocks/stores in ordinary play */
static int g_pres_timeline;
static unsigned g_pres_floor_ns;
static unsigned g_pres_guard_ns;
static int g_pres_distinct_refreshes;
static runtime_present_timeline_state g_pres_timeline_state;
static _Atomic uint64_t g_timeline_applied[2], g_timeline_deadline_late[2], g_timeline_ready_known[2];
static _Atomic int64_t g_timeline_ready_slack[2], g_timeline_apply_slack[2];
#ifdef __ANDROID__
static void (*g_pres_set_timeline)(ASurfaceTransaction *, int64_t);
#endif
static void pres_slot_diag_reset(pres_buf *b) {
    if (b->adaptive_slots && !b->generated)
        b->adaptive_real_observation=(fgs_real_observation){0};
    if (!g_pres_slot_diag) return;
    b->enqueue_ns=0;
    atomic_store_explicit(&b->diag_apply_ns,0,memory_order_relaxed);
    atomic_store_explicit(&b->diag_callback_ns,0,memory_order_relaxed);
    atomic_store_explicit(&b->diag_previous_wait_ns,0,memory_order_relaxed);
}
static vk_present_interp g_pres_interp = {.retained = -1};
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pres_cv = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_win; static uint32_t g_win_w, g_win_h, g_win_pw, g_win_ph;   /* buffer size set; the window's own size */
static int g_pres_q[NPRES * 2], g_pres_qh, g_pres_qt;
static uint64_t g_win_generation;
static int g_win_display_id = -1;
static uint64_t g_win_attachment_epoch;
static fg_adaptive_policy g_adaptive_policy;
static fg_adaptive_plan g_adaptive_plan;
static int g_adaptive_presenting=-1; /* protected by g_win_mu */
static fg_adaptive_present_state g_adaptive_publication; /* g_win_mu; committed handoffs only */
/* Scheduler metadata is protected by g_win_mu. Prepared GPU objects are only
 * recorded/dispatched/cancelled by the recorder owner at an empty boundary.
 * Publishing/cancelling metadata NEVER retires their command/source leases. */
static fgs_state g_slot_policy;
static uint64_t g_slot_clock_generation, g_slot_window_generation, g_slot_source_epoch;
typedef struct {
    vk_fg_generation *generation;
    fg_adaptive_policy phase;
    fgs_request request;
    uint64_t frame, epoch, window_generation, real_fence, real_target;
    int real_index, output_index, published, cancelled, dispatching;
} pres_slot_prepared;
static pres_slot_prepared g_slot_prepared[NPRES];
static unsigned g_slot_prepared_count; /* recorder owner only */
/* Recorder-owned pending-template gate. Lifecycle cancellation leaves it set
 * until the recorder has drained every private command/source lease. */
_Atomic int vk_fg_slot_boundary_active;
static int pres_slots_requested(void) {
    static _Atomic int request=-2;
    int value=atomic_load_explicit(&request,memory_order_relaxed);
    if (value==-2) {
        value=runtime_present_adaptive_slots_request(getenv("TOTK_FG_ADAPTIVE_SLOTS"));
        atomic_store_explicit(&request,value,memory_order_relaxed);
    }
    return value!=0 && vk_fg_adaptive();
}
static int pres_slots_desired(const pres_buf *b, uint64_t *desired_ns) {
    if (!desired_ns) return 0;
    if (b->adaptive_slots) {
        if (b->adaptive_slot.target_ns)
            return runtime_present_adaptive_slot_desired(b->desired_ns,b->refresh_ns,desired_ns);
        /* Only an explicitly staged REAL-only fallback may lack a slot. */
        if (b->generated || !b->desired_ns || b->desired_ns>(uint64_t)INT64_MAX) return 0;
    }
    *desired_ns=b->desired_ns;
    return 1;
}
static struct {
    uint64_t first_ns, boundaries, opportunities, dispatched, cancelled;
    uint64_t no_publication, no_request, late, phase, unavailable, gpu_refused;
} g_slot_dispatch_stats;
static _Atomic uint64_t g_slot_on_time_applies, g_slot_late_applies;
static _Atomic uint64_t g_slot_template_refused, g_slot_irrevocable_real_conflicts;
static fgs_publication_history g_slot_applied_generations; /* g_win_mu; actual apply returns */
static _Atomic uint64_t g_slot_late_real_counts[FGS_REAL_READY_TIME_UNKNOWN+1];
static _Atomic uint64_t g_slot_capacity_gen_waits, g_slot_capacity_gen_associations;
static _Atomic uint64_t g_slot_capacity_gen_spans, g_slot_capacity_all_real_waits;
static _Atomic uint64_t g_slot_capacity_snapshot_incomplete;
/* Scheduler selectors, each parsed once and only once the scheduler itself is
 * requested. LATTICE is default-on (absent=on, "0"=r7 path); EARLY_APPLY is
 * opt-in (absent/"0"=r7 path). Any other spelling is off and logged once. */
static int pres_latch_selector(_Atomic int *request, const char *name, const char *meaning,
        int (*parse)(const char *)) {
    if (!pres_slots_requested()) return 0;
    int value=atomic_load_explicit(request,memory_order_relaxed);
    if (value==-2) {
        const char *e=getenv(name);
        int expected=-2;
        value=parse(e);
        if (atomic_compare_exchange_strong_explicit(request,&expected,value,
                memory_order_relaxed,memory_order_relaxed)) {
            if (value==1)
                LOGV("[adaptive-latch] selector=%s value=1 enabled=1 source=%s lag_ns=%lld lag_min_ns=%lld lag_max_ns=%lld deadline_guard_ns=%llu (%s; r7 otherwise)",
                    name,e ? "explicit":"default",(long long)FGA_LATCH_LAG_NS,(long long)FGA_LATCH_LAG_MIN_NS,(long long)FGA_LATCH_LAG_MAX_NS,
                    (unsigned long long)FGA_LATCH_DEADLINE_GUARD_NS,meaning);
            else if (value<0)
                LOGV("[adaptive-latch] selector=%s value=invalid raw=\"%.32s\" enabled=0 (exact \"0\" or \"1\" required; r7 path)",name,e);
        } else value=expected;
    }
    return value==1;
}
/* TOTK_FG_ADAPTIVE_LATTICE: lattice phase re-anchor ONLY; scheduler default. */
static int pres_latch_enabled(void) {
    static _Atomic int request=-2;
    return pres_latch_selector(&request,"TOTK_FG_ADAPTIVE_LATTICE","phase re-anchor without warmup",
        fga_default_on_request);
}
/* TOTK_FG_ADAPTIVE_EARLY_APPLY: SF-deadline earlier GEN release ONLY. */
static int pres_early_enabled(void) {
    static _Atomic int request=-2;
    return pres_latch_selector(&request,"TOTK_FG_ADAPTIVE_EARLY_APPLY","SF-deadline GEN release",
        fga_latch_request);
}
static struct {
    uint64_t first_ns, last_report_ns, gen_applies, early_applies, kinds[FGA_LATCH_KINDS];
    uint64_t reanchors, reanchor_moved, reanchor_refused_pending, reanchor_refused_invalid;
    uint64_t reals, early_displaced_reals;
} g_latch_stats; /* g_win_mu; selector counters, plus the trace-only REAL count */
/* EARLY_APPLY=1 only, from GEN decisions. Sticky per job: once an early release
 * is found it is kept, so the final pre-apply re-check cannot fall back to a
 * later r7 hold (a WAIT there would cancel a staged transaction). Every other
 * guard is still evaluated at each decision by fgs_admit_gen_release. */
static uint64_t pres_early_release_locked(pres_buf *b, const runtime_present_timelines *clock,
        uint64_t now) {
    if (!pres_early_enabled()) return 0;
    if (b->adaptive_release_ns) return b->adaptive_release_ns;
    uint64_t desired;
    if (!runtime_present_adaptive_slot_desired(b->adaptive_slot.target_ns,b->refresh_ns,&desired)) {
        b->adaptive_latch_kind=FGA_LATCH_R7_INVALID;
        return 0;
    }
    fga_latch_release r=fga_latch_release_for(1,&g_slot_policy,b->adaptive_slot,desired,clock,now);
    b->adaptive_latch_kind=r.kind;
    b->adaptive_release_ns=r.release_ns;
    return r.release_ns;
}
/* LATTICE=1 only, while staging a REAL on an unchanged clock/window/epoch. */
static void pres_latch_reanchor_locked(const runtime_present_timelines *clock, const pres_buf *b) {
    const runtime_present_timeline_entry *anchor=&clock->entries[clock->preferred];
    fga_latch_phase phase=fga_latch_phase_check(&g_slot_policy,anchor->expected_ns);
    if (phase.valid && !phase.reanchor) return;
    fgs_reanchor_result r={0};
    fgs_status status=phase.valid ? fgs_reanchor(&g_slot_policy,anchor->expected_ns,
        FGA_LATCH_LAG_NS,clock->refresh_ns,&r) : FGS_INVALID;
    if (status==FGS_OK) {
        g_latch_stats.reanchors++;
        g_latch_stats.reanchor_moved+=r.moved;
        LOGV("[adaptive-latch-reanchor] event=applied frame=%llu anchor_vsync_id=%lld anchor_expected_ns=%llu old_lag_ns=%lld lag_ns=%lld lag_min_ns=%lld lag_max_ns=%lld ordinal=%llu old_generation=%llu generation=%llu old_base_ns=%llu base_ns=%llu period_ns=%llu live=%u moved=%u old_sealed_before=%llu sealed_before=%llu samples=%u ewma_ns=%llu mode=%u reanchors=%llu",
            (unsigned long long)b->frame,(long long)anchor->vsync_id,(unsigned long long)anchor->expected_ns,
            (long long)r.old_lag_ns,(long long)FGA_LATCH_LAG_NS,(long long)FGA_LATCH_LAG_MIN_NS,
            (long long)FGA_LATCH_LAG_MAX_NS,(unsigned long long)r.anchor_ordinal,
            (unsigned long long)r.old_generation,(unsigned long long)r.generation,
            (unsigned long long)r.old_base_ns,(unsigned long long)r.base_ns,(unsigned long long)r.period_ns,
            r.live,r.moved,(unsigned long long)r.old_sealed_before,(unsigned long long)r.sealed_before,
            g_slot_policy.samples,(unsigned long long)g_slot_policy.ewma_ns,(unsigned)g_slot_policy.mode,
            (unsigned long long)g_latch_stats.reanchors);
        return;
    }
    if (status==FGS_WAIT) g_latch_stats.reanchor_refused_pending++;
    else g_latch_stats.reanchor_refused_invalid++;
    if (g_fg_job_trace)
        LOGV("[adaptive-latch-reanchor] event=refused reason=%s frame=%llu anchor_vsync_id=%lld anchor_expected_ns=%llu lag_ns=%lld lag_valid=%u generation=%llu base_ns=%llu period_ns=%llu pending=%u status=%u",
            status==FGS_WAIT ? "gen-pending":"invalid",(unsigned long long)b->frame,
            (long long)anchor->vsync_id,(unsigned long long)anchor->expected_ns,
            (long long)phase.lag_ns,phase.valid,(unsigned long long)g_slot_policy.generation,
            (unsigned long long)g_slot_policy.base_ns,(unsigned long long)g_slot_policy.period_ns,
            g_slot_policy.pending_count,(unsigned)status);
}
/* EARLY_APPLY=1: a REAL that became ready after an EARLY GEN apply but before that
 * GEN's r7 hold would have taken the GEN's slot under r7 (it would have
 * yielded). Counts the conflict the early apply caused; never a decision. */
static void pres_latch_real_conflicts_locked(const pres_buf *b, uint64_t publication_ns) {
    const fgs_real_observation *real=&b->adaptive_real_observation;
    for (unsigned i=0;i<FGS_PUBLICATION_HISTORY;i++) {
        const fgs_applied_generation *g=&g_slot_applied_generations.generated[i];
        if (!g->id.sequence) continue;
        uint64_t hold=fga_latch_r7_hold(&g_slot_policy,g->slot.target_ns);
        if (g->apply_ns>=hold ||
                fgs_publication_late_real(g,*real)!=FGS_REAL_READY_AFTER_GEN_APPLY ||
                real->ready_ns>=hold) continue;
        g_latch_stats.early_displaced_reals++;
        if (g_fg_job_trace)
            LOGV("[adaptive-latch-early] event=real-displaced real_frame=%llu real_ready_ns=%llu real_target_ns=%llu real_apply_return_ns=%llu gen_source_frame=%llu gen_target_ns=%llu gen_apply_return_ns=%llu gen_r7_hold_ns=%llu total=%llu",
                (unsigned long long)b->frame,(unsigned long long)real->ready_ns,
                (unsigned long long)b->adaptive_slot.target_ns,(unsigned long long)publication_ns,
                (unsigned long long)g->source_frame,(unsigned long long)g->slot.target_ns,
                (unsigned long long)g->apply_ns,(unsigned long long)hold,
                (unsigned long long)g_latch_stats.early_displaced_reals);
    }
}
/* After a scheduler apply, still under g_win_mu. apply_ns was read immediately
 * before ASurfaceTransaction_apply. JOB_TRACE rows are passive: the snapshot
 * and period accessors copy values under the display mutex and never wait. */
static void pres_latch_applied_locked(pres_buf *b, uint64_t apply_ns, uint64_t publication_ns,
        uint64_t desired_ns) {
    int lattice=pres_latch_enabled(), early_apply=pres_early_enabled();
    uint64_t target=b->adaptive_slot.target_ns;
    uint64_t r7_hold=b->generated ? fga_latch_r7_hold(&g_slot_policy,target) : 0;
    int early=b->generated && b->adaptive_release_ns && apply_ns<r7_hold;
    if ((lattice || early_apply) && !g_latch_stats.first_ns)
        g_latch_stats.first_ns=g_latch_stats.last_report_ns=publication_ns;
    if (early_apply) {
        if (b->generated) {
            g_latch_stats.gen_applies++;
            g_latch_stats.early_applies+=(unsigned)early;
            g_latch_stats.kinds[b->adaptive_latch_kind<FGA_LATCH_KINDS ? b->adaptive_latch_kind:0]++;
        } else pres_latch_real_conflicts_locked(b,publication_ns);
    }
    if (!b->generated) g_latch_stats.reals++;
    /* CPU-ready values already observed by the decision path: GEN source/output
     * fences from its final pres_adaptive_decide_locked, REAL backend publication. */
    uint64_t source_ready=b->generated ? b->adaptive_source_ready_ns : 0;
    uint64_t output_ready=b->generated ? b->adaptive_output_ready_ns :
        b->adaptive_real_observation.ready_time_known ? b->adaptive_real_observation.ready_ns : 0;
    if (g_fg_job_trace) {
        runtime_present_timelines clock={0};
        runtime_display_period_observation period={0};
        (void)runtime_android_display_period_observe(&period);
        int fresh=runtime_android_adaptive_slot_timelines(&clock) &&
            runtime_present_adaptive_slot_clock_matches(&clock,g_slot_clock_generation,
                g_slot_policy.period_ns,runtime_android_adaptive_slots_display_id(),g_win_display_id);
        fga_latch_match match=fresh ? fga_latch_nearest(&clock,target) : (fga_latch_match){0};
        fga_latch_match latch=fresh ? fga_latch_eligible(&clock,desired_ns,target) : (fga_latch_match){0};
        int64_t sf_period=fresh && clock.count>1 ? (int64_t)((clock.entries[clock.count-1].expected_ns-
            clock.entries[0].expected_ns)/(clock.count-1)) : 0;
        LOGV("[adaptive-latch-phase] role=%s generated=%u frame=%llu token=%llu source_ready_ns=%llu output_ready_ns=%llu apply_ns=%llu publication_ns=%llu target_ns=%llu desired_ns=%llu lattice=%d early_apply=%d lattice_generation=%llu lattice_base_ns=%llu lattice_period_ns=%llu reanchors=%llu slot_generation=%llu ordinal=%llu snapshot=%s snapshot_generation=%llu snapshot_period_ns=%lld match=%s match_vsync_id=%lld match_expected_ns=%llu match_deadline_ns=%llu phase_ns=%lld phase_abs_ns=%llu apply_minus_match_deadline_ns=%lld latch=%s latch_vsync_id=%lld latch_expected_ns=%llu latch_deadline_ns=%llu latch_offset_ns=%lld apply_minus_latch_deadline_ns=%lld effective_period_ns=%llu fence_period_ns=%llu callback_period_ns=%llu fence_calibrated=%u callback_calibrated=%u release_ns=%llu r7_hold_ns=%llu release_kind=%s apply_decision=%s",
            b->generated ? "GEN":"REAL",(unsigned)(b->generated!=0),(unsigned long long)b->frame,
            (unsigned long long)b->adaptive_token,(unsigned long long)source_ready,
            (unsigned long long)output_ready,(unsigned long long)apply_ns,(unsigned long long)publication_ns,
            (unsigned long long)target,(unsigned long long)desired_ns,lattice,early_apply,
            (unsigned long long)g_slot_policy.generation,(unsigned long long)g_slot_policy.base_ns,
            (unsigned long long)g_slot_policy.period_ns,(unsigned long long)g_latch_stats.reanchors,
            (unsigned long long)b->adaptive_slot.generation,(unsigned long long)b->adaptive_slot.ordinal,
            fresh ? "fresh":"none",(unsigned long long)(fresh ? clock.generation : 0),(long long)sf_period,
            match.found ? "nearest":"none",(long long)(match.found ? match.entry.vsync_id : -1),
            (unsigned long long)match.entry.expected_ns,(unsigned long long)match.entry.deadline_ns,
            (long long)match.offset_ns,
            (unsigned long long)(match.offset_ns<0 ? 0-(uint64_t)match.offset_ns : (uint64_t)match.offset_ns),
            (long long)(match.found ? fga_latch_delta(apply_ns,match.entry.deadline_ns) : 0),
            latch.found ? "eligible":"none",(long long)(latch.found ? latch.entry.vsync_id : -1),
            (unsigned long long)latch.entry.expected_ns,(unsigned long long)latch.entry.deadline_ns,
            (long long)latch.offset_ns,
            (long long)(latch.found ? fga_latch_delta(apply_ns,latch.entry.deadline_ns) : 0),
            (unsigned long long)period.effective_ns,(unsigned long long)period.fence_measured_ns,
            (unsigned long long)period.callback_measured_ns,period.fence_calibrated,period.callback_calibrated,
            (unsigned long long)b->adaptive_release_ns,(unsigned long long)r7_hold,
            b->generated ? fga_latch_kind_name((fga_latch_kind)b->adaptive_latch_kind) : "real",
            !b->generated ? "real" : early ? "early":"r7");
        if (!b->generated && g_latch_stats.reals%300==1)
            LOGV("[adaptive-latch-clock] reals=%llu lattice=%d early_apply=%d effective_period_ns=%llu nominal_period_ns=%llu fence_period_ns=%llu fence_calibrated=%u callback_period_ns=%llu callback_calibrated=%u snapshot_period_ns=%lld lattice_period_ns=%llu lattice_base_ns=%llu lattice_generation=%llu reanchors=%llu",
                (unsigned long long)g_latch_stats.reals,lattice,early_apply,(unsigned long long)period.effective_ns,
                (unsigned long long)period.nominal_ns,(unsigned long long)period.fence_measured_ns,
                period.fence_calibrated,(unsigned long long)period.callback_measured_ns,
                period.callback_calibrated,(long long)sf_period,(unsigned long long)g_slot_policy.period_ns,
                (unsigned long long)g_slot_policy.base_ns,(unsigned long long)g_slot_policy.generation,
                (unsigned long long)g_latch_stats.reanchors);
    }
    if ((lattice || early_apply) && publication_ns-g_latch_stats.last_report_ns>=UINT64_C(1000000000)) {
        g_latch_stats.last_report_ns=publication_ns;
        LOGV("[adaptive-latch-summary] lattice=%d early_apply=%d elapsed_ns=%llu gen_applies=%llu early=%llu r7=%llu r7_no_match=%llu r7_late=%llu r7_not_earlier=%llu r7_invalid=%llu reanchors=%llu reanchor_moved=%llu reanchor_refused_pending=%llu reanchor_refused_invalid=%llu early_displaced_reals=%llu (cumulative app handoffs, not SF outcomes)",
            lattice,early_apply,
            (unsigned long long)(publication_ns-g_latch_stats.first_ns),(unsigned long long)g_latch_stats.gen_applies,
            (unsigned long long)g_latch_stats.early_applies,
            (unsigned long long)(g_latch_stats.gen_applies-g_latch_stats.early_applies),
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_NO_MATCH],
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_LATE],
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_NOT_EARLIER],
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_INVALID],
            (unsigned long long)g_latch_stats.reanchors,(unsigned long long)g_latch_stats.reanchor_moved,
            (unsigned long long)g_latch_stats.reanchor_refused_pending,
            (unsigned long long)g_latch_stats.reanchor_refused_invalid,
            (unsigned long long)g_latch_stats.early_displaced_reals);
    }
}
static void pres_slots_route_locked(const fgs_batch *batch) {
    for (unsigned n=0;n<batch->cancel_count;n++) {
        uint64_t token=batch->cancelled[n].request.token;
        for (unsigned k=0;k<NPRES;k++)
            if (g_slot_prepared[k].request.token==token) g_slot_prepared[k].cancelled=1;
        for (int q=g_pres_qh;q<g_pres_qt;q++) {
            pres_buf *b=&g_pres[g_pres_q[q%(NPRES*2)]];
            if (b->adaptive_token==token)
                atomic_store_explicit(&b->adaptive_suppressed,1,memory_order_release);
        }
        if (g_adaptive_presenting>=0 && g_pres[g_adaptive_presenting].adaptive_token==token)
            atomic_store_explicit(&g_pres[g_adaptive_presenting].adaptive_suppressed,1,memory_order_release);
    }
    for (unsigned n=0;n<batch->request_count;n++) {
        const fgs_request *request=&batch->requests[n];
        int matched=0,source_prepared=0;
        for (unsigned k=0;k<NPRES;k++) {
            pres_slot_prepared *p=&g_slot_prepared[k];
            if (!p->generation || p->cancelled || p->frame!=request->frame ||
                    p->epoch!=request->source_epoch) continue;
            source_prepared=1;
            if (p->request.token) continue;
            p->request=*request; matched=1; break;
        }
        if (!matched) {
            fgs_cancel unused;
            (void)fgs_cancel_gen(&g_slot_policy,request->token,FGS_CANCEL_OWNER,&unused);
            atomic_fetch_add_explicit(&g_slot_template_refused,1,memory_order_relaxed);
            if (g_fg_job_trace) LOGV("[adaptive-slot] event=request-refused frame=%llu token=%llu target_ns=%llu reason=%s",
                (unsigned long long)request->frame,(unsigned long long)request->token,
                (unsigned long long)request->slot.target_ns,
                source_prepared?"additional-child":"no-prepared-template");
        }
    }
}
static void pres_slots_end_locked(void) {
    fgs_batch batch;
    (void)fgs_end(&g_slot_policy,&batch);
    pres_slots_route_locked(&batch);
    for (unsigned k=0;k<NPRES;k++)
        if (g_slot_prepared[k].generation) g_slot_prepared[k].cancelled=1;
}
static pthread_cond_t g_adaptive_commit_cv;
static int g_adaptive_commit_ready; /* initialized once under g_win_mu, only by an adaptive wait */
/* Independent notification lock: never held over an Android/backend call,
 * callback wait, release poll or Vulkan wait. Lock order is window -> notify;
 * the worker takes notify alone, after dropping its retirement queue lock. */
static pthread_mutex_t g_pres_retire_notify_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pres_retire_notify_cv;
static _Atomic int g_pres_retire_notify_ready;
static void pres_retire_notify(void) {
    if (!atomic_load_explicit(&g_pres_retire_notify_ready,memory_order_acquire)) return;
    pthread_mutex_lock(&g_pres_retire_notify_mu);
    pthread_cond_broadcast(&g_pres_retire_notify_cv);
    pthread_mutex_unlock(&g_pres_retire_notify_mu);
}
static void pres_adaptive_wake_locked(void) {
    if (g_adaptive_commit_ready) pthread_cond_broadcast(&g_adaptive_commit_cv);
    pres_retire_notify();
}
static fg_adaptive_present_job pres_adaptive_job(const pres_buf *b) {
    return (fg_adaptive_present_job){b->desired_ns,b->frame,b->adaptive_epoch,b->generation,b->generated!=0};
}
/* Called before the first capacity wait, and again if backend provenance was
 * initially unavailable. Never replace a missing timestamp with dequeue/now. */
static void pres_slots_observe_real_locked(pres_buf *b, uint64_t now) {
    if (!b->adaptive_slots || b->generated) return;
    fgs_real_observation *real=&b->adaptive_real_observation;
    if (!real->first_attempt_ns) {
        *real=(fgs_real_observation){.frame=b->frame,.source_epoch=b->adaptive_epoch,
            .window_epoch=b->generation,.first_attempt_ns=now};
    }
    if (!real->ready_time_known) {
        uint64_t ready=vk_present_ready_ns(b->fence);
        if (ready && ready<=now) {
            real->ready_ns=ready;
            real->ready_time_known=1;
        }
    }
}
#ifdef __ANDROID__
static unsigned pres_slots_live_transactions(
    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS]);
#endif
/* A producer initially has a legacy callback-lattice key. Before allowing a
 * scheduler GEN apply, replan every newer READY real on the admitted API-clock
 * lattice. These are provisional queue keys, never publication/retirement.
 * g_win_mu covers the re-key, stable re-sort and final generated admission. */
static int pres_slots_ready_real_locked(const pres_buf *child, uint64_t now,
        fgs_real_plan *known_real) {
    int index=fgs_pending_index(&g_slot_policy,child->adaptive_token);
    if (index<0) return 0;
    fgs_request request=g_slot_policy.pending[index];
    int have=0;
    for (int q=g_pres_qh;q<g_pres_qt;q++) {
        pres_buf *real=&g_pres[g_pres_q[q%(NPRES*2)]];
        if (!real->adaptive_slots || real->generated || real->generation!=child->generation ||
                real->adaptive_epoch!=child->adaptive_epoch || real->frame<=child->frame) continue;
        uint64_t ready=vk_present_ready_ns(real->fence);
        /* A retired ticket whose CPU publication timestamp aged out still
         * wins arbitration; observed-now is not a claimed GPU completion. */
        if (!ready && mtl_wait(real->fence,0)) ready=now;
        fgs_real_observation observation={.frame=real->frame,.source_epoch=real->adaptive_epoch,
            .window_epoch=real->generation,.ready_ns=ready};
        fgs_real_plan plan;
        if (!fgs_publication_real_plan(&g_slot_policy,&request,observation,now,&plan)) continue;
        real->adaptive_slot=plan.slot;
        real->desired_ns=real->copy_open_ns=plan.slot.target_ns;
        real->refresh_ns=g_slot_policy.period_ns; real->interval=1;
        if (!have || plan.slot.ordinal<known_real->slot.ordinal) *known_real=plan;
        have=1;
    }
    if (have) for (int q=g_pres_qh+1;q<g_pres_qt;q++) {
        int item=g_pres_q[q%(NPRES*2)],position=q;
        const pres_buf *real=&g_pres[item];
        while (position>g_pres_qh) {
            int prior=g_pres_q[(position-1)%(NPRES*2)];
            const pres_buf *before=&g_pres[prior];
            if (!fgs_publication_queue_before(real->desired_ns,real->generated,
                    before->desired_ns,before->generated)) break;
            g_pres_q[position%(NPRES*2)]=prior; position--;
        }
        g_pres_q[position%(NPRES*2)]=item;
    }
    return have;
}
static fg_adaptive_present_result pres_adaptive_decide_locked(pres_buf *b) {
    runtime_fps_adaptive_frame stamp=runtime_fps_adaptive_snapshot();
    fg_adaptive_present_context context={.now=host_now_ns(),.epoch=stamp.epoch,
        .generation=g_win_generation,.window=g_win!=NULL,
        .suppressed=atomic_load_explicit(&b->adaptive_suppressed,memory_order_acquire)};
    pres_slots_observe_real_locked(b,context.now);
    if (b->adaptive_slots && !b->generated)
        return context.window && b->generation==context.generation ?
            FG_ADAPTIVE_PRESENT_APPLY:FG_ADAPTIVE_PRESENT_WINDOW;
    if (b->adaptive_slots && b->generated) {
        runtime_present_timelines clock;
        if (!runtime_android_adaptive_slot_timelines(&clock) ||
                !runtime_present_adaptive_slot_clock_matches(&clock,g_slot_clock_generation,
                    b->refresh_ns,runtime_android_adaptive_slots_display_id(),g_win_display_id))
            return FG_ADAPTIVE_PRESENT_SUPPRESSED;
        uint64_t ready=vk_present_ready_ns(b->fence);
        uint64_t source_ready=vk_present_ready_ns(b->source_fence);
        if (!ready || !source_ready || !b->adaptive_token)
            return FG_ADAPTIVE_PRESENT_SUPPRESSED;
        b->adaptive_output_ready_ns=ready; b->adaptive_source_ready_ns=source_ready; /* diagnostics only */
        if (source_ready>ready) ready=source_ready;
        fgs_cancel cancelled;
        if (fgs_mark_ready(&g_slot_policy,b->adaptive_token,ready,&cancelled)!=FGS_OK)
            return FG_ADAPTIVE_PRESENT_SUPPRESSED;
        fgs_request admitted;
        fgs_real_plan known_real;
        int have_real=pres_slots_ready_real_locked(b,context.now,&known_real);
        uint64_t release=pres_early_release_locked(b,&clock,context.now);
        fgs_status status=release ? fgs_admit_gen_release(&g_slot_policy,b->adaptive_token,
            context.now,have_real?&known_real.slot:NULL,release,&admitted) :
            fgs_admit_gen(&g_slot_policy,b->adaptive_token,context.now,
            have_real?&known_real.slot:NULL,&admitted);
        context.release=release;
        if (status==FGS_REAL_COLLISION) return FG_ADAPTIVE_PRESENT_YIELD;
        if (status!=FGS_OK && status!=FGS_WAIT)
            return status==FGS_LATE?FG_ADAPTIVE_PRESENT_EXPIRED:FG_ADAPTIVE_PRESENT_SUPPRESSED;
    }
    for (int q=g_pres_qh;b->generated && !b->adaptive_slots && q<g_pres_qt;q++) {
        const pres_buf *r=&g_pres[g_pres_q[q%(NPRES*2)]];
        if (r->generated || r->generation!=b->generation || r->adaptive_epoch!=b->adaptive_epoch ||
                r->frame<=b->frame || r->desired_ns>b->desired_ns) continue;
        context.queued_real=1;
        if (fg_adaptive_supersedes(b->desired_ns,b->frame,b->adaptive_epoch,r->desired_ns,
                r->frame,r->adaptive_epoch,vk_present_ready_ns(r->fence))) context.ready_real=1;
    }
    return fg_adaptive_present_decide(&g_adaptive_publication,pres_adaptive_job(b),context);
}
static void pres_adaptive_cond_locked(void) {
    if (g_adaptive_commit_ready) return;
    pthread_condattr_t attr;
    int error=pthread_condattr_init(&attr);
    if (error) DIE("adaptive presenter condattr: %d",error);
    error=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    if (!error) error=pthread_cond_init(&g_adaptive_commit_cv,&attr);
    pthread_condattr_destroy(&attr);
    if (error) DIE("adaptive presenter monotonic condition: %d",error);
    g_adaptive_commit_ready=1;
}
static fg_adaptive_present_result pres_adaptive_wait_locked(pres_buf *b) {
    for (;;) {
        fg_adaptive_present_result decision=pres_adaptive_decide_locked(b);
        if (decision!=FG_ADAPTIVE_PRESENT_WAIT) return decision;
        pres_adaptive_cond_locked();
        uint64_t deadline=fg_adaptive_present_hold(b->desired_ns,b->adaptive_release_ns);
        struct timespec absolute={(time_t)(deadline/UINT64_C(1000000000)),
            (long)(deadline%UINT64_C(1000000000))};
        int error=pthread_cond_timedwait(&g_adaptive_commit_cv,&g_win_mu,&absolute);
        if (error && error!=ETIMEDOUT) DIE("adaptive presenter deadline wait: %d",error);
        /* Queue/lifecycle/suppression wakes and spurious wakes arbitrate again;
         * timeout is the pre-slot deadline, never a display callback. */
    }
}
static struct {
    uint64_t real, first_ns, last_ns, generated, generator_refused, phase[4], refused[FG_ADAPTIVE_REASONS];
    _Atomic uint64_t shown, missed, suppressed;
} g_adaptive_stats;
/* The queue remains bounded by presenter ownership. Adaptive inserts by slot,
 * with real before generated at equality; canceled jobs remain until their
 * covering GPU fence retires. Legacy FIFO is unchanged. g_win_mu held. */
static void pres_queue_locked(int index) {
    pres_buf *b=&g_pres[index];
    int position=g_pres_qt++;
    if (b->adaptive_epoch) {
        while (position>g_pres_qh) {
            int prior=g_pres_q[(position-1)%(NPRES*2)];
            pres_buf *p=&g_pres[prior];
            if (!fgs_publication_queue_before(b->desired_ns,b->generated,
                    p->desired_ns,p->generated)) break;
            g_pres_q[position%(NPRES*2)]=prior;
            position--;
        }
        if (!b->generated && g_adaptive_presenting>=0) {
            pres_buf *p=&g_pres[g_adaptive_presenting];
            /* Scheduler jobs re-arbitrate on the admitted API lattice. Their
             * producer callback keys cannot suppress a different-clock GEN. */
            if ((!b->adaptive_slots || !p->adaptive_slots) &&
                fg_adaptive_supersedes(p->desired_ns,p->frame,p->adaptive_epoch,b->desired_ns,
                    b->frame,b->adaptive_epoch,vk_present_ready_ns(b->fence)))
                atomic_store_explicit(&p->adaptive_suppressed,1,memory_order_release);
        }
    }
    g_pres_q[position%(NPRES*2)]=index;
    if (b->adaptive_epoch) pres_adaptive_wake_locked();
}
static int g_win_prerotate, g_win_ubwc, g_win_transform_hint = -1; /* g_win_mu; no hint means the existing identity path */
/* The existing window mutex serializes lifecycle/canvas changes and snapshots.
   Packed atomic publications keep extents allocation-free and independently readable.
   Explicit-width/native-UI paths retain their existing g_win_pw/g_win_ph policy. */
static _Atomic uint64_t g_guest_window_wh, g_guest_fit_wh, g_guest_fit_xy;
static uint32_t g_guest_canvas_w, g_guest_canvas_h;
#ifdef TOTK_VK_TESTS
static _Atomic unsigned g_guest_fit_rebuilds;
unsigned vk_window_test_fit_rebuilds(void) {
    return atomic_load_explicit(&g_guest_fit_rebuilds, memory_order_relaxed);
}
uint64_t vk_window_test_cached_extent(void) {
    return atomic_load_explicit(&g_guest_window_wh, memory_order_relaxed);
}
#endif
static uint64_t pres_pack_extent(uint32_t w, uint32_t h) { return (uint64_t)w << 32 | h; }
static vk_present_extent pres_guest_window(void) {
    uint64_t wh = atomic_load_explicit(&g_guest_window_wh, memory_order_relaxed);
    return (vk_present_extent){(uint32_t)(wh >> 32), (uint32_t)wh};
}
static float g_guest_aspect;   /* g_win_mu: the ratio the cached fit was made for */
/* Caller holds g_win_mu. Compute only after a lifecycle, canvas/crop or shown-ratio change. */
static void pres_guest_fit_refresh(void) {
    vk_present_extent window = pres_guest_window();
    g_guest_aspect = pres_shown_aspect();
    vk_present_rect fit = vk_present_guest_fit(vk_present_shown_width(g_guest_canvas_w, g_guest_canvas_h, g_guest_aspect),
                                               g_guest_canvas_h, window.w, window.h);
    atomic_store_explicit(&g_guest_fit_wh, pres_pack_extent(fit.w, fit.h), memory_order_relaxed);
    atomic_store_explicit(&g_guest_fit_xy, pres_pack_extent((uint32_t)fit.x, (uint32_t)fit.y), memory_order_relaxed);
#ifdef TOTK_VK_TESTS
    atomic_fetch_add_explicit(&g_guest_fit_rebuilds, 1, memory_order_relaxed);
#endif
}
static vk_present_rect pres_guest_fit_cached(uint32_t canvas_w, uint32_t canvas_h) {
    if (canvas_w != g_guest_canvas_w || canvas_h != g_guest_canvas_h || pres_shown_aspect() != g_guest_aspect) {
        g_guest_canvas_w = canvas_w; g_guest_canvas_h = canvas_h;
        pres_guest_fit_refresh();
    }
    uint64_t wh = atomic_load_explicit(&g_guest_fit_wh, memory_order_relaxed);
    uint64_t xy = atomic_load_explicit(&g_guest_fit_xy, memory_order_relaxed);
    return (vk_present_rect){(int32_t)(xy >> 32), (int32_t)xy, (uint32_t)(wh >> 32), (uint32_t)wh};
}
static _Atomic uint64_t g_pres_shown, g_pres_dropped, g_pres_cancelled;
static _Atomic uint64_t g_fg_handoffs, g_fg_expired;
static _Atomic uint64_t g_fg_ready, g_fg_generated, g_fg_accepted, g_fg_pending, g_fg_no_buffer;
static _Atomic uint64_t g_fg_expiry[FRAMEGEN_CHILD_REASONS];
static _Atomic uint64_t g_hybrid_bins[6], g_hybrid_missing;
static struct {
    uint64_t pair[VK_INTERP_REASONS];
    uint64_t no_buffer, source_shape, generator_refused, recorded;
} g_fg_admission;
/* Queued slot credit bounds producer lead, not GPU/compositor ownership. It
 * retires at copy post acknowledgement or CURRENT AHB transaction completion;
 * busy and the existing release fences retain their separate lifetime rules. */
static void pres_credit_release(pres_buf *b) {
    if (b && b->refresh_ns && atomic_exchange_explicit(&b->credited, 0, memory_order_relaxed))
        runtime_android_present_credit(-1);
}
static void pres_predict_cancel(pres_buf *b) {
    if (b->prediction.id && !b->prediction_done) {
        runtime_android_predict_cancel(&b->prediction); b->prediction_done = 1;
    }
}
static void pres_capacity_trace(const pres_buf *b,const char *event) {
    if (!g_fg_job_trace) return;
    LOGV("[framegen-capacity] source-frame=%llu generated=%d target-ns=%llu generation=%llu event=%s now-ns=%llu capacity-wait-start-ns=%llu capacity-wait-end-ns=%llu count=%u async=%u",
        (unsigned long long)b->frame,b->generated!=0,(unsigned long long)b->desired_ns,
        (unsigned long long)b->generation,event,(unsigned long long)host_now_ns(),
        (unsigned long long)b->job.capacity_wait_start_ns,(unsigned long long)b->job.capacity_wait_end_ns,
        b->job.capacity_wait_count,b->job.retire_async);
}
static void pres_capacity_begin_live(pres_buf *b, const fgs_transaction_id *live,
        unsigned live_count) {
    uint64_t now=host_now_ns();
    pres_slots_observe_real_locked(b,now);
    if (!b->job.capacity_wait_start_ns) b->job.capacity_wait_start_ns=now;
    b->job.capacity_wait_end_ns=0; b->job.capacity_wait_count++;
    if (b->adaptive_slots && !b->generated) {
        b->adaptive_capacity_live_count=live_count;
        fgs_publication_capacity_begin(&b->adaptive_capacity,&g_slot_applied_generations,
            live,live_count,now);
    }
    pres_capacity_trace(b,"start");
}
#ifdef __ANDROID__
static void pres_capacity_begin(pres_buf *b) {
    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS];
    unsigned count=b->adaptive_slots && !b->generated ? pres_slots_live_transactions(live):0;
    pres_capacity_begin_live(b,live,count);
}
#endif
static void pres_capacity_end(pres_buf *b) {
    uint64_t end=host_now_ns();
    b->job.capacity_wait_end_ns=end;
    pres_capacity_trace(b,"end");
    if (!b->adaptive_slots || b->generated) return;
    const fgs_capacity_observation *wait=&b->adaptive_capacity;
    unsigned full=b->adaptive_capacity_live_count==FGS_PUBLICATION_CONTEXTS;
    if (!full) atomic_fetch_add_explicit(&g_slot_capacity_snapshot_incomplete,1,memory_order_relaxed);
    else atomic_fetch_add_explicit(wait->count ? &g_slot_capacity_gen_waits :
        &g_slot_capacity_all_real_waits,1,memory_order_relaxed);
    for (unsigned i=0;i<wait->count;i++) {
        const fgs_applied_generation *g=&wait->generated[i];
        int spans=fgs_publication_capacity_spans_target(wait,i,end);
        atomic_fetch_add_explicit(&g_slot_capacity_gen_associations,1,memory_order_relaxed);
        if (spans) atomic_fetch_add_explicit(&g_slot_capacity_gen_spans,1,memory_order_relaxed);
        if (g_fg_job_trace)
            LOGV("[adaptive-slot-capacity] event=gen-association real_frame=%llu first_attempt_ns=%llu real_ready_ns=%llu ready_provenance=%s tx_index=%u tx_sequence=%llu tx_generation=%llu gen_source_frame=%llu gen_target_ns=%llu gen_apply_return_ns=%llu wait_start_ns=%llu wait_end_ns=%llu wait_span_ns=%llu spans_target=%u live_contexts=%u full_snapshot=%u sf_outcome=UNKNOWN",
                (unsigned long long)b->frame,
                (unsigned long long)b->adaptive_real_observation.first_attempt_ns,
                (unsigned long long)b->adaptive_real_observation.ready_ns,
                b->adaptive_real_observation.ready_time_known?"backend-cpu-fence-publication":"UNKNOWN",
                g->id.index,(unsigned long long)g->id.sequence,(unsigned long long)g->id.generation,
                (unsigned long long)g->source_frame,(unsigned long long)g->slot.target_ns,
                (unsigned long long)g->apply_ns,(unsigned long long)wait->start_ns,
                (unsigned long long)end,(unsigned long long)(end>=wait->start_ns?end-wait->start_ns:0),
                (unsigned)spans,b->adaptive_capacity_live_count,full);
    }
    if (g_fg_job_trace)
        LOGV("[adaptive-slot-capacity] event=wait-summary real_frame=%llu live_contexts=%u gen_associations=%u full_snapshot=%u gen_waits_total=%llu gen_associations_total=%llu spans_target_total=%llu all_real_waits_total=%llu incomplete_snapshots_total=%llu sf_outcome=UNKNOWN",
            (unsigned long long)b->frame,b->adaptive_capacity_live_count,wait->count,full,
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_gen_waits,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_gen_associations,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_gen_spans,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_all_real_waits,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_snapshot_incomplete,memory_order_relaxed));
}
static void pres_job_trace(const pres_buf *b,const char *outcome) {
    if (!g_fg_job_trace || !b->generated) return;
    uint64_t decision_ns=host_now_ns();
    char line[640];
    fg_job_trace_format(line,sizeof line,(fg_job_trace){.frame=b->frame,
        .submit=b->record_done_ns,.pickup=b->dequeue_ns,
        .source_ready=b->pair_ready_ns,.output_ready=b->own_ready_ns,
        .target=b->desired_ns,.decision=decision_ns},
        b->adaptive_epoch ? "adaptive" : "locked",outcome);
    LOGV("%s",line);
    pres_capacity_trace(b,"terminal");
    LOGV("[adaptive-dispatch-stage] stage=presenter-terminal source=%llu output=%u source_ticket=%llu output_ticket=%llu record_done_ns=%llu enqueue_ns=%llu pickup_ns=%llu source_cpu_ready_ns=%llu output_cpu_ready_ns=%llu final_gpu_done_ns=0 final_gpu_done_authority=UNKNOWN-not-exported target_ns=%llu decision_ns=%llu outcome=%s clock=CLOCK_MONOTONIC ready_authority=CPU-publication-after-fence-and-posts",
        (unsigned long long)b->frame,(unsigned)(b-g_pres),
        (unsigned long long)b->source_fence,(unsigned long long)b->fence,
        (unsigned long long)b->record_done_ns,(unsigned long long)b->enqueue_ns,
        (unsigned long long)b->dequeue_ns,(unsigned long long)b->pair_ready_ns,
        (unsigned long long)b->own_ready_ns,(unsigned long long)b->desired_ns,
        (unsigned long long)decision_ns,outcome);
}
/* Every recorded child has exactly one terminal outcome. GPU completion
 * refusal (budget/authority) is distinct from missing the child's vsync. */
static void pres_child_expire(pres_buf *b, framegen_child_reason reason) {
    if (b->adaptive_epoch) {
        pthread_mutex_lock(&g_win_mu);
        if (g_adaptive_presenting==(int)(b-g_pres)) g_adaptive_presenting=-1;
        pthread_mutex_unlock(&g_win_mu);
        if (reason!=FRAMEGEN_CHILD_OUTPUT)
            atomic_fetch_add_explicit(&g_adaptive_stats.missed,1,memory_order_relaxed);
    }
    if (g_pres_trace) LOGV("[present-drop] source_frame=%llu kind=%s desired_ns=%llu record_done_ns=%llu enqueue_ns=%llu dequeue_ns=%llu own_ready_ns=%llu pair_ready_ns=%llu now_ns=%llu reason=%u queue_depth=%u",
        (unsigned long long)b->frame,b->generated ? (b->interpolated ? "interp" : "extrap") : "real",
        (unsigned long long)b->desired_ns,(unsigned long long)b->record_done_ns,
        (unsigned long long)b->enqueue_ns,(unsigned long long)b->dequeue_ns,
        (unsigned long long)b->own_ready_ns,(unsigned long long)b->pair_ready_ns,
        (unsigned long long)host_now_ns(),(unsigned)reason,b->queue_depth);
    pres_job_trace(b,reason==FRAMEGEN_CHILD_DEADLINE ? "deadline" :
        reason==FRAMEGEN_CHILD_COMPLETION ? "completion-refused" : "expired");
    if (b->generated) {
        (void)vk_fg_complete((unsigned)(b-g_pres)); /* idempotent; own/source fences already retired */
        g_fg_expired++; g_fg_expiry[reason]++; g_fg_pending--;
        if (reason == FRAMEGEN_CHILD_SOURCE_MISSING || reason == FRAMEGEN_CHILD_SOURCE_LATE ||
            (b->interpolated && reason == FRAMEGEN_CHILD_DEADLINE)) {
            vk_fg_late();
            if (g_pres_qcom[b-g_pres]) vk_fg_qcom_late();
            if (g_pres_lsfg[b-g_pres]) vk_fg_lsfg_late();
        }
    }
    g_pres_cancelled++; pres_predict_cancel(b); pres_credit_release(b);
    atomic_store(&b->busy, 0);
}
static void pres_adaptive_expire(pres_buf *b,fg_adaptive_present_result decision) {
    if (decision==FG_ADAPTIVE_PRESENT_SUPPRESSED)
        atomic_fetch_add_explicit(&g_adaptive_stats.suppressed,1,memory_order_relaxed);
    pres_child_expire(b,decision==FG_ADAPTIVE_PRESENT_WINDOW ? FRAMEGEN_CHILD_WINDOW :
        decision==FG_ADAPTIVE_PRESENT_EXPIRED ? FRAMEGEN_CHILD_DEADLINE : FRAMEGEN_CHILD_OUTPUT);
}
static void pres_child_handoff(void) {
    g_fg_handoffs++; g_fg_pending--;
}
static void pres_predict_feedback(pres_buf *b, unsigned path, uint64_t ready_ns,
        uint64_t preparation_ns, uint64_t allowance_ns) {
    if (b->auto_frame.id && !b->auto_done && b->ready_ns) {
        runtime_android_fps_auto_feedback(&b->auto_frame, b->ready_ns, preparation_ns, allowance_ns);
        b->auto_done = 1;
    }
    if (!b->prediction.id || b->prediction_done) return;
    /* C joins acquire release to independent GPU/publication readiness, then
     * appends only measured copy preparation. Presenter FIFO residence and its
     * explicit slot hold are deliberately absent, even if prep ran later. */
    uint64_t release = b->prediction.release_ns;
    if (!b->ready_ns || !release || b->ready_ns < release || ready_ns < b->ready_ns ||
        preparation_ns > UINT64_MAX - (b->ready_ns - release)) {
        static uint64_t missing; missing++;
        if (missing <= 8 || missing % 300 == 0)
            LOGV("window predict: invalid/missing independent readiness, ticket=%llu plan=%llu ready=%llu release=%llu; cancelled total=%llu (no presenter-dequeue fallback)",
                 (unsigned long long)b->fence, (unsigned long long)b->prediction.id,
                 (unsigned long long)b->ready_ns, (unsigned long long)release, (unsigned long long)missing);
        pres_predict_cancel(b); return;
    }
    runtime_android_predict_feedback(&b->prediction, path, ready_ns,
        b->ready_ns - release + preparation_ns, allowance_ns);
    b->prediction_done = 1;
}
/* This snapshot never waits for a callback. Reserve before enqueue so any
 * producer scheduling cost remains before timeline handoff, not presenter
 * delay. Only the presenter may block on the display clock. */
static void pres_slot_reserve(pres_buf *b, double requested_period) {
    static int on = -1, logged, unavailable;
    static vk_present_slots slots;
    static uint64_t count, late, skipped, worst_late;
    static int hybrid_was_interpolating;
    if (on < 0) { const char *e = getenv("TOTK_PRESENT_SLOTS"); on = e && !strcmp(e, "1"); }
    if (!on) return;
    uint64_t callback = 0, refresh = 0;
    if (!runtime_android_present_clock(&callback, &refresh)) refresh = 0;
    uint64_t now = host_now_ns();
    if (vk_fg_adaptive()) {
        static uint64_t last_real, generation;
        if (!callback || refresh<8000000 || refresh>18000000 || callback>now) return;
        if (generation!=b->generation) { last_real=0; generation=b->generation; }
        uint64_t target=fg_adaptive_first_slot(callback,refresh,now);
        if (target && target<=last_real) target=fg_adaptive_first_slot(last_real,refresh,now);
        if (!target) return;
        b->desired_ns=b->copy_open_ns=last_real=target;
        b->refresh_ns=refresh; b->interval=1;
        return;
    }
    int hybrid_interpolating=runtime_fps_hybrid_selected() && runtime_fps_hybrid_interpolating();
    /* A native60 lattice can be almost one native tick behind now. Real30
     * cannot catch that phase up while paced, so seed its first full-period
     * lead like fixed interpolation. Keep desired_ns as the queued-target
     * floor: no previously queued native/child picture is retimed/overtaken. */
    if (hybrid_interpolating && !hybrid_was_interpolating) slots.refresh_ns=0;
    hybrid_was_interpolating=hybrid_interpolating;
    vk_present_slot slot = vk_present_slots_reserve(&slots, 1, callback, refresh,
        vk_present_slots_interval(requested_period, refresh), now);
    if (slot.mode==VK_PRESENT_SLOTS_ACTIVE && runtime_fps_hybrid_selected() &&
        !runtime_fps_hybrid_interpolating() && g_pres_interp.retained>=0 &&
        slot.desired_ns<=vk_present_interp_child_floor(&g_pres_interp))
        slot=vk_present_slots_reserve(&slots,1,callback,refresh,slot.interval,now);
    if (slot.mode != VK_PRESENT_SLOTS_ACTIVE) {
        if (!unavailable) { LOGV("window present slots: display clock unavailable; using legacy presentation"); unavailable = 1; }
        return;
    }
    b->desired_ns = slot.desired_ns; b->copy_open_ns = slot.copy_open_ns;
    b->refresh_ns = slot.refresh_ns; b->interval = slot.interval;
    b->slot_skipped = slot.skipped;
    if (!logged) {
        LOGV("window present slots: requested %.3f ms, interval %u x refresh %.3f ms, initial lead %.3f ms; never deliberately early: copy post floor=slot, AHB desired=slot",
             requested_period * 1e3, slot.interval, slot.refresh_ns / 1e6,
             slot.interval * slot.refresh_ns / 1e6);
        logged = 1;
    }
    uint64_t delay = now > slot.desired_ns ? now - slot.desired_ns : 0;
    late += delay != 0; skipped += slot.skipped; if (delay > worst_late) worst_late = delay;
    if (++count % 300 == 0) {
        LOGV("window present slots, last 300 reservations: %llu late when reserved, worst %.3f ms; %llu whole phase slots skipped (not scanout measurements)",
             (unsigned long long)late, worst_late / 1e6, (unsigned long long)skipped);
        late = skipped = worst_late = 0;
    }
}
/* Only admitted locked AHB pairs use display-provided expected times and IDs.
 * Selection happens before enqueue; neither the presenter nor callbacks retime
 * an owned reservation. A transient short/stale snapshot may re-anchor to a
 * fresh legacy callback, but never invent an API vsync ID. */
static void pres_timeline_pair(vk_present_interp_pair *pair, const pres_buf *source,
        runtime_present_timeline_entry *real, runtime_present_timeline_entry *child) {
    if (!g_pres_timeline || !source->zero || pair->real < 0 ||
        (pair->reason != VK_INTERP_READY && pair->reason != VK_INTERP_LATE)) return;
    if (!runtime_present_timeline_display_allowed(runtime_android_present_timeline_display_id(),
            source->timeline_display)) {
        static uint64_t logged_generation=UINT64_MAX;
        if (logged_generation!=source->generation) {
            LOGV("[present-timeline] display guard: source=%d window=%d generation=%llu; legacy reservation/waits retained",
                runtime_android_present_timeline_display_id(),source->timeline_display,
                (unsigned long long)source->generation);
            logged_generation=source->generation;
        }
        return;
    }
    runtime_present_timelines snapshot;
    runtime_present_timeline_pair selected;
    uint64_t now=host_now_ns();
    int api=runtime_android_present_timelines(&snapshot) &&
        runtime_present_timeline_choose(&g_pres_timeline_state,&snapshot,now,
            vk_fg_lead_refreshes(),source->generation,g_pres_distinct_refreshes,&selected);
    int anchored=api;
    if (!anchored) {
        uint64_t callback=0,refresh=0;
        anchored=runtime_android_present_clock(&callback,&refresh) &&
            runtime_present_timeline_reanchor(&g_pres_timeline_state,callback,refresh,now,
                vk_fg_lead_refreshes(),source->generation,&selected);
    }
    static uint64_t pairs,chosen,fallback,unavailable,delay_sum,delay_min=UINT64_MAX,delay_max;
    pairs++; chosen+=api; fallback+=anchored && !api; unavailable+=!anchored;
    if (anchored) {
        *real=selected.real; *child=selected.child;
        pair->slot.desired_ns=pair->slot.copy_open_ns=selected.real.expected_ns;
        pair->slot.refresh_ns=selected.refresh_ns;
        pair->slot.interval=2; pair->slot.mode=VK_PRESENT_SLOTS_ACTIVE;
        pair->midpoint_ns=selected.child.expected_ns; pair->reason=VK_INTERP_READY;
        uint64_t delay=selected.real.expected_ns-now;
        delay_sum+=delay; if(delay<delay_min)delay_min=delay; if(delay>delay_max)delay_max=delay;
    } else {
        /* No fresh authority: publish only a real, beyond already reserved
         * targets in this surface generation. Never invent an API ID. */
        uint64_t floor=g_pres_timeline_state.window_generation==source->generation ?
            g_pres_timeline_state.last_child_ns:0;
        if (floor && pair->slot.refresh_ns<=INT64_MAX-floor && pair->slot.desired_ns<=floor)
            pair->slot.desired_ns=pair->slot.copy_open_ns=floor+pair->slot.refresh_ns;
        g_pres_timeline_state.window_generation=source->generation;
        g_pres_timeline_state.last_real_ns=g_pres_timeline_state.last_child_ns=pair->slot.desired_ns;
        pair->midpoint_ns=0; pair->reason=VK_INTERP_CLOCK;
    }
    if (pairs%300==0) {
        uint64_t n=chosen+fallback;
        LOGV("[present-timeline] last300 pairs API=%llu reanchor=%llu unavailable=%llu real-target-from-reserve mean/min/max=%.3f/%.3f/%.3fms; immutable real/child roles, not scanout",
            (unsigned long long)chosen,(unsigned long long)fallback,(unsigned long long)unavailable,
            n?delay_sum/(double)n/1e6:0,n?delay_min/1e6:0,delay_max/1e6);
        for(unsigned role=0;role<2;role++) {
            uint64_t count=atomic_exchange_explicit(&g_timeline_applied[role],0,memory_order_relaxed);
            uint64_t late=atomic_exchange_explicit(&g_timeline_deadline_late[role],0,memory_order_relaxed);
            int64_t ready=atomic_exchange_explicit(&g_timeline_ready_slack[role],0,memory_order_relaxed);
            int64_t apply=atomic_exchange_explicit(&g_timeline_apply_slack[role],0,memory_order_relaxed);
            uint64_t known=atomic_exchange_explicit(&g_timeline_ready_known[role],0,memory_order_relaxed);
            LOGV("[present-timeline] role=%s applied=%llu deadline-late=%llu ready-known=%llu deadline-minus-ready/apply mean=%.3f/%.3fms (async report; real late is retained, child late is omitted)",
                role?"generated":"real",(unsigned long long)count,(unsigned long long)late,(unsigned long long)known,
                known?ready/(double)known/1e6:0,count?apply/(double)count/1e6:0);
        }
        chosen=fallback=unavailable=delay_sum=delay_max=0; delay_min=UINT64_MAX;
    }
}
/* Producer-side reservations, not display samples. The interval and target
 * below are the exact values later handed to the presenter with these pixels. */
static void pres_auto_reservation(const pres_buf *b) {
    if (!b->auto_cadence) return;
    static uint64_t total, previous, refresh, generation;
    static uint64_t interval1, interval2, other, legacy, short_floor, gaps, irregular, boundaries;
    static uint64_t late, skipped, worst_late, ahb, copy;
    interval1 += b->interval == 1; interval2 += b->interval == 2;
    other += b->interval > 2; legacy += !b->interval;
    ahb += b->zero != 0; copy += !b->zero;
    short_floor += b->auto_frame.id && b->interval && b->interval < b->auto_frame.interval;
    if (previous && b->refresh_ns && refresh == b->refresh_ns && generation == b->generation) {
        uint64_t period = b->refresh_ns * b->interval;
        uint64_t delta = b->desired_ns > previous ? b->desired_ns - previous : 0;
        if (!period || !delta || delta % period) irregular++;
        else gaps += delta != period;
    } else boundaries++;
    previous = b->desired_ns; refresh = b->refresh_ns; generation = b->generation;
    uint64_t now = host_now_ns(), delay = now > b->desired_ns ? now - b->desired_ns : 0;
    late += delay != 0; skipped += b->slot_skipped;
    if (delay > worst_late) worst_late = delay;
    if (++total % 300) return;
    LOGV("window AUTO queued cadence, last 300: AHB/copy=%llu/%llu immutable interval 1/2/>2/legacy=%llu/%llu/%llu/%llu below-acquired-floor=%llu; desired delta gap/irregular/boundary=%llu/%llu/%llu; skipped=%llu enqueue-late=%llu worst=%.3fms (reservations, NOT display)",
         (unsigned long long)ahb, (unsigned long long)copy,
         (unsigned long long)interval1, (unsigned long long)interval2, (unsigned long long)other,
         (unsigned long long)legacy, (unsigned long long)short_floor, (unsigned long long)gaps,
         (unsigned long long)irregular, (unsigned long long)boundaries, (unsigned long long)skipped,
         (unsigned long long)late, worst_late / 1e6);
    interval1 = interval2 = other = legacy = short_floor = gaps = irregular = boundaries = 0;
    late = skipped = worst_late = ahb = copy = 0;
}
#ifdef __ANDROID__
typedef struct { AHardwareBuffer *buffer; VkImage image; VkDeviceMemory mem; VkImageView view[2]; uint32_t w, h; int foreign, ahb_import; } pres_ahb;
static pres_ahb g_ahb[NPRES];
static _Atomic int g_ahb_off;
/* NativeActivity's root producer is not parentable on some framework releases. A failed creation is scoped to
   this window generation: a UI-thread attachment callback can publish an explicit parent and recover from copy. */
static _Atomic int g_ahb_surface_off;
static ANativeWindow *g_win_parent;
static int pres_slots_real_fallback_locked(pres_buf *b, const char *reason) {
    pres_slots_end_locked();
    b->adaptive_slot=(fgs_slot){0};
    b->adaptive_token=0; b->adaptive_eligible=0; b->adaptive_real_conflicts=0;
    b->refresh_ns=0; b->interval=0; b->timeline_role=0;
    uint64_t now=host_now_ns();
    int valid=now && now<=(uint64_t)INT64_MAX;
    LOGV("[adaptive-slot] event=%s frame=%llu mode=REAL-only scheduler=0 host_now_ns=%llu reason=%s",
        valid ? "real-only-fallback":"real-only-refused",(unsigned long long)b->frame,
        (unsigned long long)now,reason);
    if (!valid) return 0;
    b->desired_ns=b->copy_open_ns=now;
    return 1;
}
static int pres_slots_stage_real_locked(pres_buf *b, int display_id) {
    runtime_present_timelines clock;
    if (!b->adaptive_slots || b->generated) return 1;
    b->adaptive_real_conflicts=0;
    if (!runtime_android_adaptive_slots_active() ||
            !runtime_present_timeline_display_allowed(runtime_android_adaptive_slots_display_id(),display_id) ||
            !runtime_android_adaptive_slot_timelines(&clock))
        return pres_slots_real_fallback_locked(b,"clock-unavailable");
    if (!g_slot_policy.configured || g_slot_clock_generation!=clock.generation ||
            g_slot_window_generation!=b->generation || g_slot_source_epoch!=b->adaptive_epoch ||
            g_slot_policy.period_ns!=clock.refresh_ns) {
        pres_slots_end_locked();
        fgs_batch batch;
        if (fgs_reset(&g_slot_policy,b->generation,clock.entries[clock.preferred].expected_ns,
                clock.refresh_ns,UINT64_C(2000000),&batch)!=FGS_OK)
            return pres_slots_real_fallback_locked(b,"clock-reset-invalid");
        g_slot_clock_generation=clock.generation;
        g_slot_window_generation=b->generation;
        g_slot_source_epoch=b->adaptive_epoch;
    }
    /* LATTICE=1 only: same-clock phase correction; a fresh reset is re-phased too. */
    if (pres_latch_enabled()) pres_latch_reanchor_locked(&clock,b);
    fgs_real_plan plan;
    if (fgs_peek_real(&g_slot_policy,host_now_ns(),&plan)!=FGS_OK)
        return pres_slots_real_fallback_locked(b,"real-plan-invalid");
    uint64_t desired;
    if (!runtime_present_adaptive_slot_desired(plan.slot.target_ns,clock.refresh_ns,&desired))
        return pres_slots_real_fallback_locked(b,"desired-encoding-invalid");
    b->adaptive_slot=plan.slot;
    b->adaptive_real_conflicts=plan.generated_conflicts;
    b->desired_ns=b->copy_open_ns=plan.slot.target_ns;
    b->refresh_ns=clock.refresh_ns; b->interval=1;
    return 1;
}
static int pres_transaction_set_desired_locked(ASurfaceTransaction *tx, pres_buf *b,
        uint64_t *admitted_desired) {
    if (b->adaptive_slots && !b->generated && !pres_slots_stage_real_locked(b,g_win_display_id)) return 0;
    uint64_t desired;
    if (!pres_slots_desired(b,&desired)) {
        LOGV("[adaptive-slot] event=desired-refused frame=%llu generated=%d target_ns=%llu refresh_ns=%llu",
            (unsigned long long)b->frame,b->generated!=0,
            (unsigned long long)b->desired_ns,(unsigned long long)b->refresh_ns);
        return 0;
    }
    /* Keep nominal legacy cadence and its existing optional eligibility floor.
     * Scheduler setters and cadence share the same admitted signed encoding. */
    ASurfaceTransaction_setDesiredPresentTime(tx,(int64_t)(b->adaptive_slots ? desired :
        vk_present_eligible_time(desired,g_pres_floor_ns,g_pres_guard_ns,b->refresh_ns)));
    *admitted_desired=desired;
    return 1;
}
static void pres_slots_published_locked(pres_buf *b, uint64_t publication_ns,
        vk_present_retire_token transaction) {
    if (!b->adaptive_slots) return;
    if (g_fg_job_trace) {
        uint64_t desired;
        if (pres_slots_desired(b,&desired))
            LOGV("[adaptive-slot] event=apply-metadata frame=%llu generated=%d target_ns=%llu desired_ns=%llu desired_admitted=1 refresh_ns=%llu publication_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu sf_outcome=UNKNOWN",
                (unsigned long long)b->frame,b->generated!=0,
                (unsigned long long)b->desired_ns,(unsigned long long)desired,
                (unsigned long long)b->refresh_ns,(unsigned long long)publication_ns,
                transaction.index,(unsigned long long)transaction.sequence,
                (unsigned long long)transaction.generation);
        else LOGV("[adaptive-slot] event=apply-metadata frame=%llu generated=%d target_ns=%llu desired_admitted=0 refresh_ns=%llu publication_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu sf_outcome=UNKNOWN",
            (unsigned long long)b->frame,b->generated!=0,(unsigned long long)b->desired_ns,
            (unsigned long long)b->refresh_ns,(unsigned long long)publication_ns,
            transaction.index,(unsigned long long)transaction.sequence,
            (unsigned long long)transaction.generation);
    }
    if (b->generated) {
        /* Even a failed scheduler commit cannot undo an actual Android apply.
         * Keep the immutable transaction identity, not a reusable image index. */
        fgs_publication_record(&g_slot_applied_generations,(fgs_applied_generation){
            .id={transaction.index,transaction.sequence,transaction.generation},
            .slot=b->adaptive_slot,.source_frame=b->frame,.source_epoch=b->adaptive_epoch,
            .apply_ns=publication_ns});
        fgs_gen_commit result;
        fgs_status status=fgs_commit_gen(&g_slot_policy,b->adaptive_token,publication_ns,&result);
        if (status==FGS_OK) atomic_fetch_add_explicit(result.target_missed?
            &g_slot_late_applies:&g_slot_on_time_applies,1,memory_order_relaxed);
        if (g_fg_job_trace) LOGV("[adaptive-slot] event=gen-commit frame=%llu token=%llu target_ns=%llu publication_ns=%llu status=%u deadline_missed=%u target_missed=%u",
            (unsigned long long)b->frame,(unsigned long long)b->adaptive_token,
            (unsigned long long)b->desired_ns,(unsigned long long)publication_ns,(unsigned)status,
            status==FGS_OK?result.deadline_missed:0,status==FGS_OK?result.target_missed:0);
        return;
    }
    pres_slots_observe_real_locked(b,publication_ns);
    for (unsigned i=0;i<FGS_PUBLICATION_HISTORY;i++) {
        const fgs_applied_generation *g=&g_slot_applied_generations.generated[i];
        fgs_late_real_kind kind=fgs_publication_late_real(g,b->adaptive_real_observation);
        if (kind==FGS_REAL_NOT_LATE) continue;
        atomic_fetch_add_explicit(&g_slot_late_real_counts[kind],1,memory_order_relaxed);
        if (g_fg_job_trace) {
            const char *reason=kind==FGS_REAL_READY_AFTER_GEN_APPLY ?
                "potential-late-real-after-gen" :
                kind==FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED ?
                "ready-before-gen-apply-unserviced" : "ready-provenance-unknown";
            LOGV("[adaptive-slot-residual] kind=%s real_frame=%llu real_ready_ns=%llu ready_provenance=%s first_attempt_ns=%llu real_apply_return_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu gen_source_frame=%llu gen_target_ns=%llu gen_apply_return_ns=%llu potential_late_real_total=%llu ready_unserviced_total=%llu unknown_ready_total=%llu sf_outcome=UNKNOWN",
                reason,(unsigned long long)b->frame,
                (unsigned long long)b->adaptive_real_observation.ready_ns,
                b->adaptive_real_observation.ready_time_known?"backend-cpu-fence-publication":"UNKNOWN",
                (unsigned long long)b->adaptive_real_observation.first_attempt_ns,
                (unsigned long long)publication_ns,g->id.index,
                (unsigned long long)g->id.sequence,(unsigned long long)g->id.generation,
                (unsigned long long)g->source_frame,(unsigned long long)g->slot.target_ns,
                (unsigned long long)g->apply_ns,
                (unsigned long long)atomic_load_explicit(&g_slot_late_real_counts[FGS_REAL_READY_AFTER_GEN_APPLY],memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&g_slot_late_real_counts[FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED],memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&g_slot_late_real_counts[FGS_REAL_READY_TIME_UNKNOWN],memory_order_relaxed));
        }
    }
    for (unsigned k=0;k<NPRES;k++) {
        pres_slot_prepared *p=&g_slot_prepared[k];
        if (p->generation && p->frame==b->frame && p->epoch==b->adaptive_epoch) {
            p->published=1; p->real_fence=b->fence; p->real_target=b->desired_ns;
            if (!b->adaptive_slot.target_ns) p->cancelled=1;
        }
    }
    if (!b->adaptive_slot.target_ns) return;
    fgs_batch batch;
    fgs_status status=fgs_commit_real(&g_slot_policy,b->adaptive_slot,publication_ns,
        b->frame,b->adaptive_epoch,(int)b->adaptive_eligible,&batch);
    if (status==FGS_OK) atomic_fetch_add_explicit(&g_slot_irrevocable_real_conflicts,
        b->adaptive_real_conflicts,memory_order_relaxed);
    if (status==FGS_OK) pres_slots_route_locked(&batch);
    else for (unsigned k=0;k<NPRES;k++)
        if (g_slot_prepared[k].frame==b->frame) g_slot_prepared[k].cancelled=1;
    if (g_fg_job_trace) LOGV("[adaptive-slot] event=real-commit frame=%llu target_ns=%llu publication_ns=%llu status=%u mode=%u requests=%u cancellations=%u target_missed=%u irrevocable_gen_conflicts=%u",
        (unsigned long long)b->frame,(unsigned long long)b->desired_ns,
        (unsigned long long)publication_ns,(unsigned)status,(unsigned)batch.mode,
        batch.request_count,batch.cancel_count,batch.target_missed,b->adaptive_real_conflicts);
}
static VkSampler g_pres_sampler, g_pres_pixel_sampler; static VkDescriptorSetLayout g_pres_dsl; static VkPipelineLayout g_pres_layout; static VkPipeline g_pres_pipe[2];
static VkPipelineLayout g_pres_rotated_layout; static VkPipeline g_pres_rotated_pipe[2];
/* Ordinary presentation remains serial. Locked interpolation and explicitly
 * selected adaptive request mode may have two distinct FUTURE transactions.
 * Contexts stay immutable through ordered completion and PREVIOUS release. */
typedef struct pres_complete {
    pthread_mutex_t mu; pthread_cond_t cv; ASurfaceControl *surface; int done, previous, release_fd;
    pres_buf *current;
    uint64_t timeline_frame, timeline_apply;
    struct pres_complete *observer;
    vk_present_fence_entry snapshot;
    uint64_t record_done_ns, enqueue_ns, dequeue_ns, own_ready_ns, pair_ready_ns;
    unsigned queue_depth, pending_transactions;
    vk_present_retire_token retire_token;
    unsigned retire_async;
    /* Shared observation state lives only in the first context. */
    vk_present_fences display_fences;
    unsigned display_diagnostics;
    uint64_t cadence_next;
    vk_present_cadence cadence;
} pres_complete;
/* Never borrow presenter-stack storage: a missing callback may outlive a
 * bounded drain. A quarantined owner is deliberately retained until process
 * death, including its contexts, old surface, transaction and window refs. */
static struct {
    vk_present_retire queue;
    pres_complete *contexts[VK_PRESENT_RETIRE_CAPACITY];
    _Atomic int initialized, running, exiting;
    ASurfaceControl *surface;
    ASurfaceTransaction *transaction;
    ANativeWindow *window, *parent;
} g_pres_retire;
_Static_assert(VK_PRESENT_RETIRE_CAPACITY==VK_PRESENT_FLIGHT_CAPACITY,"retirement must not add contexts");
_Static_assert(VK_PRESENT_RETIRE_CAPACITY==FGS_PUBLICATION_CONTEXTS,"publication snapshot must match live contexts");
static unsigned pres_slots_live_transactions(
        fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS]) {
    if (!g_pres_retire.initialized || !g_pres_retire.running) return 0;
    vk_present_retire_token tokens[VK_PRESENT_RETIRE_CAPACITY];
    unsigned count=vk_present_retire_inflight(&g_pres_retire.queue,tokens);
    for (unsigned i=0;i<count;i++)
        live[i]=(fgs_transaction_id){tokens[i].index,tokens[i].sequence,tokens[i].generation};
    return count;
}
static pthread_mutex_t g_pres_retire_lifecycle = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pres_retire_sequence, g_pres_sync_retires;
static void pres_retire_event(void *context,const vk_present_retire_event *event) {
    (void)context;
    if (!g_fg_job_trace) return;
    LOGV("[framegen-retire] event=%s sequence=%llu generation=%llu context=%u now-ns=%llu depth=%u worker-wakes=%llu worker-retires=%llu async=1",
        event->event,(unsigned long long)event->token.sequence,
        (unsigned long long)event->token.generation,event->token.index,
        (unsigned long long)event->now_ns,event->depth,
        (unsigned long long)event->wakes,(unsigned long long)event->retires);
}
static void pres_retire_sync_event(pres_complete *c,const char *event,unsigned depth) {
    if (!g_fg_job_trace) return;
    LOGV("[framegen-retire] event=%s sequence=%llu generation=%llu context=%u now-ns=%llu depth=%u worker-wakes=0 worker-retires=%llu async=0",
        event,(unsigned long long)c->retire_token.sequence,
        (unsigned long long)c->retire_token.generation,c->retire_token.index,
        (unsigned long long)host_now_ns(),depth,(unsigned long long)g_pres_sync_retires);
}
static void pres_retire_wake(void *context) {
    (void)context;
    pres_retire_notify();
}
/* OnCommit has NO present/release fences. OnComplete owns the CURRENT
 * transaction's present fd, which can still be pending when inspected. Keep
 * that fd and immutable frame/apply IDs, not the callback stats or pres_buf.
 * Latch, callback, and PREVIOUS release times never become display endpoints. */
static vk_present_fence_reason pres_display_query(int fd, uint64_t *display, int *error, void *context) {
    (void)context;
    struct sync_fence_info fences[VK_PRESENT_FENCE_CHILDREN];
    struct sync_file_info info = {.num_fences = VK_PRESENT_FENCE_CHILDREN,
        .sync_fence_info = (uint64_t)(uintptr_t)fences};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info)) {
        *error = errno; *display = 0; return VK_PF_IOCTL_ERROR;
    }
    return vk_present_fence_decode(&info, fences, VK_PRESENT_FENCE_CHILDREN, display);
}
static void pres_display_close(int fd, void *context) { (void)context; close(fd); }
static void pres_display_result(const vk_present_fence_entry *entry, vk_present_fence_reason reason,
        uint64_t display, void *context) {
    pres_complete *c = context;
    if (entry->cadence.id) {
        vk_present_cadence_sample sample = entry->cadence;
        sample.display_ns = reason == VK_PF_SIGNALED ? display : 0;
        int trace=g_pres_trace;
        if (trace) LOGV("[present-trace] id=%llu generation=%llu source_frame=%llu kind=%s desired_ns=%llu apply_ns=%llu display_ns=%llu refresh_ns=%llu interval=%u reason=%u",
            (unsigned long long)sample.id, (unsigned long long)sample.generation,
            (unsigned long long)entry->source_frame,
            entry->generated ? (entry->interpolated ? "interp" : "extrap") : "real",
            (unsigned long long)sample.desired_ns, (unsigned long long)sample.apply_ns,
            (unsigned long long)sample.display_ns, (unsigned long long)sample.refresh_ns,
            sample.interval, (unsigned)reason);
        int hybrid=runtime_fps_hybrid_selected();
        uint64_t previous_bins[6];
        if (hybrid) memcpy(previous_bins,c->cadence.counts.bins,sizeof previous_bins);
        vk_present_cadence_result(&c->cadence, sample);
        vk_present_cadence_counts *n = &c->cadence.counts;
        if (hybrid) {
            for (unsigned i=0;i<6;i++) g_hybrid_bins[i]+=n->bins[i]-previous_bins[i];
            g_hybrid_missing+=!sample.display_ns;
        }
        if (n->results == 300) {
            vk_present_cadence_percentile p = vk_present_cadence_percentiles(n);
            LOGV("window present-fence cadence, last 300 results: immutable interval 1/2/>2/legacy=%llu/%llu/%llu/%llu signaled/missing=%llu/%llu pairs=%llu missing-pairs=%llu boundary=%llu history-lost=%llu repeated/reversed=%llu/%llu; interval min/mean/max=%.3f/%.3f/%.3fms p50/p95/p99=%.3f/%.3f/%.3fms samples=%u/%llu sample-overflow=%llu; apply-late=%llu worst=%.3fms; fence-before-desired=%llu >=refresh-late=%llu worst=%.3fms (framework present-fence signals, NOT guaranteed hardware scanout)",
                 (unsigned long long)n->interval1, (unsigned long long)n->interval2,
                 (unsigned long long)n->other, (unsigned long long)n->legacy,
                 (unsigned long long)n->signaled, (unsigned long long)n->missing, (unsigned long long)n->pairs,
                 (unsigned long long)n->missing_pairs, (unsigned long long)n->boundaries, (unsigned long long)n->history_lost,
                 (unsigned long long)n->repeated, (unsigned long long)n->reversed,
                 n->min_ns / 1e6, n->pairs ? n->sum_ns / (double)n->pairs / 1e6 : 0, n->max_ns / 1e6,
                 p.p50_ns / 1e6, p.p95_ns / 1e6, p.p99_ns / 1e6, n->sample_count,
                 (unsigned long long)n->pairs, (unsigned long long)n->sample_lost,
                 (unsigned long long)n->apply_late, n->worst_apply_late_ns / 1e6,
                 (unsigned long long)n->before_desired, (unsigned long long)n->late_refresh, n->worst_late_ns / 1e6);
            LOGV("window present-fence interval bins ms [<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]: all=%llu/%llu/%llu/%llu/%llu/%llu; adjacent immutable interval2=%llu/%llu/%llu/%llu/%llu/%llu (only adjacent transaction IDs, no callback-order or missing-fence substitution; always on for AHB, independent of AUTO/FT)",
                 (unsigned long long)n->bins[0], (unsigned long long)n->bins[1], (unsigned long long)n->bins[2],
                 (unsigned long long)n->bins[3], (unsigned long long)n->bins[4], (unsigned long long)n->bins[5],
                 (unsigned long long)n->interval2_bins[0], (unsigned long long)n->interval2_bins[1], (unsigned long long)n->interval2_bins[2],
                 (unsigned long long)n->interval2_bins[3], (unsigned long long)n->interval2_bins[4],
                 (unsigned long long)n->interval2_bins[5]);
            for (unsigned i = 0; i < n->tail_count; i++) {
                const vk_present_cadence_tail *t = &n->tails[i];
                LOGV("window present-fence interval2 tail, first %u resolved pairs at most: ids=%llu/%llu preceding-interval=%u (0=legacy/unknown) desired-delta=%.3fms native-delta=%.3fms (immutable endpoint intervals 2/2; no transition or scanout inference)",
                     VK_PRESENT_CADENCE_TAILS, (unsigned long long)t->first_id, (unsigned long long)t->second_id,
                     t->before_interval, t->desired_delta_ns / 1e6, t->display_delta_ns / 1e6);
            }
            *n = (vk_present_cadence_counts){0};
        }
    }
    if (reason == VK_PF_SIGNALED) {
        if (entry->framegen_real && entry->cadence.interval==2)
            vk_fg_real_feedback(entry->framegen_real,entry->cadence.desired_ns,
                display,entry->cadence.refresh_ns);
        runtime_android_display_presented(display);
        if (entry->prediction.id) runtime_android_predict_presented(&entry->prediction, display);
        if (totk_ft_on() && entry->frame) totk_ft_compositor(entry->frame, entry->apply_ns, display);
    } else if (c->display_diagnostics++ < 8) {
        static const char *const names[VK_PF_REASON_COUNT] = {
            "signaled", "pending", "absent", "ioctl-error", "fence-error", "too-many-children",
            "invalid-timestamp", "evicted-pending", "teardown-pending"};
        LOGV("window present fence: frame=%llu apply_ns=%llu generation=%llu reason=%s errno=%d latch_ns=%lld (queried framework diagnostic, NOT display)",
             (unsigned long long)entry->frame, (unsigned long long)entry->apply_ns,
             (unsigned long long)entry->generation, names[reason], entry->error, (long long)entry->latch_ns);
    }
}
static void pres_display_sweep(pres_complete *c, int clear) {
    pthread_mutex_lock(&c->mu);
    vk_present_fences_sweep(&c->display_fences, pres_display_query, pres_display_close, pres_display_result, c);
    if (clear) vk_present_fences_clear(&c->display_fences, pres_display_close, pres_display_result, c);
    pthread_mutex_unlock(&c->mu);
}
static void pres_complete_cb(void *context, ASurfaceTransactionStats *stats) {
    pres_complete *c = context, *observer=c->observer;
    uint64_t callback_ns=g_pres_trace || g_pres_slot_diag ? host_now_ns() : 0;
    if (g_pres_slot_diag && c->current)
        atomic_store_explicit(&c->current->diag_callback_ns,callback_ns,memory_order_release);
    pres_credit_release(c->current); /* CURRENT completion, not PREVIOUS release */
    pthread_mutex_lock(&observer->mu);
    vk_present_fences *f = &observer->display_fences;
    vk_present_fences_sweep(f, pres_display_query, pres_display_close, pres_display_result, observer);
    if (c->current) {
        vk_present_fence_entry entry=c->snapshot;
        entry.fd=ASurfaceTransactionStats_getPresentFenceFd(stats);
        entry.latch_ns=ASurfaceTransactionStats_getLatchTime(stats);
        vk_present_fences_add(f, entry, pres_display_query, pres_display_close, pres_display_result, observer);
        if (g_pres_trace) LOGV("[present-pipeline] id=%llu generation=%llu source_frame=%llu kind=%s desired_ns=%llu record_done_ns=%llu enqueue_ns=%llu dequeue_ns=%llu own_ready_ns=%llu pair_ready_ns=%llu apply_ns=%llu callback_ns=%llu queue_depth=%u pending_transactions=%u",
            (unsigned long long)entry.cadence.id,(unsigned long long)entry.generation,
            (unsigned long long)entry.source_frame,entry.generated ? (entry.interpolated ? "interp" : "extrap") : "real",
            (unsigned long long)entry.cadence.desired_ns,(unsigned long long)c->record_done_ns,
            (unsigned long long)c->enqueue_ns,(unsigned long long)c->dequeue_ns,
            (unsigned long long)c->own_ready_ns,(unsigned long long)c->pair_ready_ns,
            (unsigned long long)entry.cadence.apply_ns,(unsigned long long)callback_ns,
            c->queue_depth,c->pending_transactions);
        if (f->accepted <= 3 || f->accepted % 300 == 0)
            LOGV("window present fences: total=%llu signaled=%llu deferred=%llu pending=%u pending_queries=%llu absent=%llu ioctl_error=%llu fence_error=%llu oversized=%llu invalid_time=%llu evicted=%llu teardown=%llu last_latch_ns=%lld (diagnostic only; no guaranteed hardware timestamp)",
                 (unsigned long long)f->accepted, (unsigned long long)f->reason[VK_PF_SIGNALED],
                 (unsigned long long)f->deferred, f->pending, (unsigned long long)f->reason[VK_PF_PENDING],
                 (unsigned long long)f->reason[VK_PF_NO_FD], (unsigned long long)f->reason[VK_PF_IOCTL_ERROR],
                 (unsigned long long)f->reason[VK_PF_FENCE_ERROR], (unsigned long long)f->reason[VK_PF_TOO_MANY],
                 (unsigned long long)f->reason[VK_PF_INVALID_TIME], (unsigned long long)f->reason[VK_PF_EVICTED],
                 (unsigned long long)f->reason[VK_PF_TEARDOWN], (long long)entry.latch_ns);
    }
    pthread_mutex_unlock(&observer->mu);
    int fd = c->previous >= 0 ? ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, c->surface) : -1;
    pthread_mutex_lock(&c->mu); c->release_fd = fd; c->done = 1; pthread_cond_signal(&c->cv); pthread_mutex_unlock(&c->mu);
}
static void pres_transaction_finish(pres_complete *c);
static fg_adaptive_present_result pres_transaction(ASurfaceTransaction *tx, pres_complete *c, ASurfaceControl *surface,
        int previous, pres_buf *current, uint64_t setup_ns, int defer, unsigned pending, int adaptive_locked) {
    /* Adaptive callers acquire g_win_mu only after blocking drains and before
     * staging setters. This routine releases it on rejection or just after
     * apply, never across the completion wait below. */
    if ((current && current->adaptive_epoch)!=adaptive_locked) DIE("adaptive transaction guard ownership");
    uint64_t admitted_desired=0;
    if (current && !pres_transaction_set_desired_locked(tx,current,&admitted_desired)) {
        c->current=NULL; c->snapshot=(vk_present_fence_entry){0}; c->done=1;
        if (adaptive_locked) pthread_mutex_unlock(&g_win_mu);
        return FG_ADAPTIVE_PRESENT_SUPPRESSED;
    }
    uint64_t frame = current ? current->timeline_frame : 0;
    c->surface = surface; c->previous = previous; c->current = current; c->done = 0; c->release_fd = -1;
    c->timeline_frame = frame; c->timeline_apply = 0;
    c->pending_transactions=pending;
    c->snapshot=(vk_present_fence_entry){0};
    if (!c->retire_async) {
        c->retire_token=(vk_present_retire_token){
            c==g_pres_retire.contexts[1],++g_pres_retire_sequence,
            current ? current->generation : previous>=0 ? g_pres[previous].generation : 0};
        pres_retire_sync_event(c,"reserve",pending);
    }
    uint64_t apply_start = current && (current->prediction.id || current->auto_frame.id) ? host_now_ns() : 0;
    ASurfaceTransaction_setOnComplete(tx, c, pres_complete_cb);
    if (current) {
        uint64_t apply_ns=host_now_ns();
        if (g_pres_timeline && current->timeline_role) {
            unsigned role=current->timeline_role-1;
            uint64_t deadline=current->display_timeline.deadline_ns;
            uint64_t ready=current->timeline_role==RUNTIME_PRESENT_TIMELINE_REAL ? current->own_ready_ns :
                current->own_ready_ns>current->pair_ready_ns ? current->own_ready_ns:current->pair_ready_ns;
            atomic_fetch_add_explicit(&g_timeline_applied[role],1,memory_order_relaxed);
            atomic_fetch_add_explicit(&g_timeline_deadline_late[role],apply_ns>=deadline,memory_order_relaxed);
            if (ready) {
                atomic_fetch_add_explicit(&g_timeline_ready_known[role],1,memory_order_relaxed);
                atomic_fetch_add_explicit(&g_timeline_ready_slack[role],(int64_t)deadline-(int64_t)ready,memory_order_relaxed);
            }
            atomic_fetch_add_explicit(&g_timeline_apply_slack[role],(int64_t)deadline-(int64_t)apply_ns,memory_order_relaxed);
        }
        if (g_pres_slot_diag) atomic_store_explicit(&current->diag_apply_ns,apply_ns,memory_order_release);
        c->snapshot=(vk_present_fence_entry){.frame=frame,
            .framegen_real=current->framegen_real,.source_frame=current->frame,
            .generated=current->generated,.interpolated=current->interpolated,
            .generation=current->generation,.prediction=current->prediction,
            .cadence={.id=++c->observer->cadence_next,.generation=current->generation,
                .desired_ns=admitted_desired,.refresh_ns=current->refresh_ns,
                .interval=current->interval,.apply_ns=apply_ns}};
        c->record_done_ns=current->record_done_ns; c->enqueue_ns=current->enqueue_ns;
        c->dequeue_ns=current->dequeue_ns; c->own_ready_ns=current->own_ready_ns;
        c->pair_ready_ns=current->pair_ready_ns; c->queue_depth=current->queue_depth;
    }
    if (totk_ft_on() && frame) c->timeline_apply = totk_ft_now();
    c->snapshot.apply_ns=c->timeline_apply;
    if (current && current->adaptive_epoch) {
        /* The caller still holds g_win_mu through staging. Repeat the epoch,
         * window and ready-real check immediately before the actual handoff. */
        fg_adaptive_present_result decision=pres_adaptive_decide_locked(current);
        uint64_t final_desired;
        if (decision==FG_ADAPTIVE_PRESENT_APPLY && current->adaptive_slots &&
                (!pres_slots_desired(current,&final_desired) || final_desired!=admitted_desired))
            decision=FG_ADAPTIVE_PRESENT_SUPPRESSED;
        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) {
            if (g_pres_slot_diag) atomic_store_explicit(&current->diag_apply_ns,0,memory_order_release);
            c->observer->cadence_next--; /* this staged context was never handed off */
            c->current=NULL; c->snapshot=(vk_present_fence_entry){0}; c->done=1;
            if (!c->retire_async) pres_retire_sync_event(c,"cancel",pending-1);
            pthread_mutex_unlock(&g_win_mu);
            return decision;
        }
    }
    /* JOB_TRACE rows / LATTICE or EARLY_APPLY counters only; no decision reads this clock. */
    uint64_t latch_apply_ns=current && current->adaptive_epoch && current->adaptive_slots &&
        (g_fg_job_trace || pres_latch_enabled() || pres_early_enabled()) ? host_now_ns() : 0;
    ASurfaceTransaction_apply(tx);
    if (current && current->adaptive_epoch) {
        uint64_t publication_ns=host_now_ns();
        pres_slots_published_locked(current,publication_ns,c->retire_token);
        if (g_fg_job_trace)
            LOGV("[adaptive-dispatch-stage] stage=transaction-published source=%llu generated=%u output=%u source_ticket=%llu output_ticket=%llu source_cpu_ready_ns=%llu output_cpu_ready_ns=%llu publication_ns=%llu target_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu final_gpu_done_ns=0 final_gpu_done_authority=UNKNOWN-not-exported clock=CLOCK_MONOTONIC publication_authority=ASurfaceTransaction-apply-return sf_outcome=UNKNOWN",
                (unsigned long long)current->frame,current->generated!=0,(unsigned)(current-g_pres),
                (unsigned long long)current->source_fence,(unsigned long long)current->fence,
                (unsigned long long)current->pair_ready_ns,(unsigned long long)current->own_ready_ns,
                (unsigned long long)publication_ns,(unsigned long long)current->desired_ns,
                c->retire_token.index,(unsigned long long)c->retire_token.sequence,
                (unsigned long long)c->retire_token.generation);
        if (latch_apply_ns) pres_latch_applied_locked(current,latch_apply_ns,publication_ns,admitted_desired);
    }
    if (current && current->adaptive_epoch) {
        fg_adaptive_present_committed(&g_adaptive_publication,pres_adaptive_job(current));
        if (g_adaptive_presenting==(int)(current-g_pres)) g_adaptive_presenting=-1;
        pthread_mutex_unlock(&g_win_mu); /* never hold the queue/lifecycle mutex through completion */
        vk_overlay_presented(host_now_ns(),current->generated);
        if (current->generated) pres_child_handoff();
    }
    if (current) pres_job_trace(current,"handed-off");
    if (apply_start) {
        uint64_t apply_ns = host_now_ns() - apply_start;
        pres_predict_feedback(current, PRESENT_PREDICT_AHB, current->ready_ns, 0, setup_ns + apply_ns);
    }
    if (totk_ft_on() && frame) {
        totk_ft_submission(frame, c->timeline_apply, FT_SUBMIT_AHB_APPLY);
        totk_ft_compositor(frame, c->timeline_apply, 0);
    }
    if (!c->retire_async) pres_retire_sync_event(c,"publish",pending);
    if (!defer) pres_transaction_finish(c);
    return FG_ADAPTIVE_PRESENT_APPLY;
}
static int pres_transaction_finish_until(pres_complete *c,uint64_t deadline,unsigned depth) {
    uint64_t callback_wait_begin=g_fg_job_trace ? host_now_ns() : 0;
    if (!c->retire_async) pres_retire_sync_event(c,"dequeue",depth);
    pthread_mutex_lock(&c->mu);
    int complete=1;
    while (!c->done) {
        int error=0;
        if (deadline) {
            struct timespec absolute=vk_present_retire_time(deadline);
            error=pthread_cond_timedwait(&c->cv,&c->mu,&absolute);
        } else pthread_cond_wait(&c->cv,&c->mu);
        if (error && !c->done) { complete=0; break; }
    }
    int fd=complete ? c->release_fd : -1;
    pthread_mutex_unlock(&c->mu);
    uint64_t callback_wait_end=g_fg_job_trace ? host_now_ns() : 0;
    uint64_t release_poll_begin=0,release_poll_end=0;
    uint64_t wait_start=g_pres_trace || g_pres_slot_diag ? host_now_ns() : 0;
    int released=complete;
    if (fd>=0) {
        if (g_pres_slot_diag && c->previous>=0)
            atomic_store_explicit(&g_pres[c->previous].diag_previous_wait_ns,wait_start,memory_order_release);
        struct pollfd p={.fd=fd,.events=POLLIN};
        int r;
        if (g_fg_job_trace) release_poll_begin=host_now_ns();
        do {
            int timeout=-1;
            if (deadline) {
                uint64_t now=host_now_ns(),remaining=deadline>now ? deadline-now : 0;
                uint64_t ms=remaining/UINT64_C(1000000)+(remaining%UINT64_C(1000000)!=0);
                timeout=ms>INT_MAX ? INT_MAX : (int)ms;
            }
            r=poll(&p,1,timeout);
        } while (r<0 && errno==EINTR && (!deadline || host_now_ns()<deadline));
        if (g_fg_job_trace) release_poll_end=host_now_ns();
        released=r>0 && (p.revents&POLLIN) &&
            (!deadline || !(p.revents&(POLLERR|POLLNVAL)));
        if (!r || (r<0 && errno==EINTR)) complete=0;
        close(fd);
        c->release_fd=-1; /* only the finish owner writes after done */
    }
    vk_present_retire_release_result(released,
        c->previous>=0 ? &g_pres[c->previous].busy : NULL,&g_ahb_off);
    if (!complete) {
        LOGV("window present: async retirement timeout sequence=%llu generation=%llu previous=%d callback=%d; quarantining contexts/buffers/surface, ownership retained (2s monotonic budget)",
            (unsigned long long)c->retire_token.sequence,(unsigned long long)c->retire_token.generation,
            c->previous,fd>=0);
    } else if (!released) {
        LOGV("window present: compositor release fence failed; using copy, previous retained");
    }
    if (g_pres_trace && c->current) LOGV("[present-release] id=%llu generation=%llu source_frame=%llu wait_start_ns=%llu released_ns=%llu previous=%d",
        (unsigned long long)c->snapshot.cadence.id,(unsigned long long)c->snapshot.generation,
        (unsigned long long)c->snapshot.source_frame,(unsigned long long)wait_start,
        (unsigned long long)(released ? host_now_ns() : 0),c->previous);
    if (g_fg_job_trace) LOGV("[framegen-wait] source-frame=%llu generated=%d target-ns=%llu finish-wait-start-ns=%llu finish-wait-end-ns=%llu previous-release-poll-start-ns=%llu previous-release-poll-end-ns=%llu finish-end-ns=%llu previous=%d released=%d sequence=%llu generation=%llu async=%u complete=%d; CLOCK_MONOTONIC blocking-intervals-not-cpu-scheduler-attribution",
        (unsigned long long)c->snapshot.source_frame,c->snapshot.generated,
        (unsigned long long)c->snapshot.cadence.desired_ns,
        (unsigned long long)callback_wait_begin,(unsigned long long)callback_wait_end,
        (unsigned long long)release_poll_begin,(unsigned long long)release_poll_end,
        (unsigned long long)host_now_ns(),c->previous,released,
        (unsigned long long)c->retire_token.sequence,(unsigned long long)c->retire_token.generation,
        c->retire_async,complete);
    if (!c->retire_async) {
        g_pres_sync_retires++;
        pres_retire_sync_event(c,"retire",depth-1);
    }
    return complete;
}
static void pres_transaction_finish(pres_complete *c) {
    (void)pres_transaction_finish_until(c,0,1); /* OFF retains its original unbounded ownership wait */
}
static int pres_retire_finish(void *context,unsigned index,uint64_t deadline) {
    (void)context;
    return pres_transaction_finish_until(g_pres_retire.contexts[index],deadline,0);
}
static void pres_retire_exit(void) {
    pthread_mutex_lock(&g_pres_retire_lifecycle);
    g_pres_retire.exiting=1;
    atomic_store(&g_ahb_off,1); /* stop admission before the bounded process-exit drain */
    if (g_pres_retire.initialized)
        (void)vk_present_retire_drain_join(&g_pres_retire.queue,vk_present_retire_deadline());
    pthread_mutex_unlock(&g_pres_retire_lifecycle);
    /* No context, surface, buffer or callback storage is freed at process exit. */
}
static int pres_retire_start(void) {
    pthread_mutex_lock(&g_pres_retire_lifecycle);
    if (g_pres_retire.running) { pthread_mutex_unlock(&g_pres_retire_lifecycle); return 1; }
    if (g_pres_retire.initialized || g_pres_retire.exiting) {
        pthread_mutex_unlock(&g_pres_retire_lifecycle); return 0; /* permanently quarantined/exiting */
    }
    int error=vk_present_retire_init(&g_pres_retire.queue,pres_retire_finish,
        pres_retire_wake,g_fg_job_trace ? pres_retire_event : NULL,NULL);
    if (error) { pthread_mutex_unlock(&g_pres_retire_lifecycle); return 0; }
    g_pres_retire.queue.sequence=g_pres_retire_sequence;
    error=vk_present_retire_start(&g_pres_retire.queue);
    if (error) {
        vk_present_retire_destroy(&g_pres_retire.queue);
        pthread_mutex_unlock(&g_pres_retire_lifecycle); return 0;
    }
    g_pres_retire.initialized=g_pres_retire.running=1;
    static int exit_registered;
    if (!exit_registered) { atexit(pres_retire_exit); exit_registered=1; }
    pthread_mutex_unlock(&g_pres_retire_lifecycle);
    return 1;
}
static int pres_retire_stop(uint64_t deadline) {
    pthread_mutex_lock(&g_pres_retire_lifecycle);
    if (!g_pres_retire.initialized) { pthread_mutex_unlock(&g_pres_retire_lifecycle); return 1; }
    int clean=vk_present_retire_drain_join(&g_pres_retire.queue,deadline);
    g_pres_retire.running=0;
    if (!clean) {
        atomic_store(&g_ahb_off,1);
        LOGV("window present: retirement drain quarantined; process-lifetime contexts/buffers/surface retained, copy fallback");
        pthread_mutex_unlock(&g_pres_retire_lifecycle);
        return 0;
    }
    g_pres_retire_sequence=g_pres_retire.queue.sequence;
    if (!vk_present_retire_destroy(&g_pres_retire.queue)) DIE("retirement destroy without clean join");
    g_pres_retire.initialized=0;
    pthread_mutex_unlock(&g_pres_retire_lifecycle);
    return 1;
}
/* Held job stays GPU-retired but presenter-owned. This is a queue/lifecycle
 * notification wait, never an Android callback invocation or release poll. */
static fg_adaptive_present_result pres_retire_reserve(pres_buf *b,
        vk_present_retire_token *token) {
    int waiting=0;
    fg_adaptive_present_result decision;
    pthread_mutex_lock(&g_win_mu);
    for (;;) {
        decision=pres_adaptive_decide_locked(b);
        if (decision==FG_ADAPTIVE_PRESENT_WAIT) decision=pres_adaptive_wait_locked(b);
        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) break;
        if (atomic_load(&g_ahb_off) || atomic_load(&g_ahb_surface_off) ||
                vk_present_retire_quarantined(&g_pres_retire.queue)) {
            decision=FG_ADAPTIVE_PRESENT_WINDOW; break;
        }
        /* Taking notify while still holding window closes both retirement
         * and lifecycle lost-wakeup gaps. No window lock survives the wait. */
        pthread_mutex_lock(&g_pres_retire_notify_mu);
        if (vk_present_retire_reserve(&g_pres_retire.queue,b->generation,token)) {
            pthread_mutex_unlock(&g_pres_retire_notify_mu); break;
        }
        if (!waiting) { pres_capacity_begin(b); waiting=1; }
        pthread_mutex_unlock(&g_win_mu);
        int error=0;
        if (b->generated) {
            struct timespec absolute=vk_present_retire_time(b->desired_ns);
            error=pthread_cond_timedwait(&g_pres_retire_notify_cv,&g_pres_retire_notify_mu,&absolute);
        } else pthread_cond_wait(&g_pres_retire_notify_cv,&g_pres_retire_notify_mu);
        pthread_mutex_unlock(&g_pres_retire_notify_mu);
        pthread_mutex_lock(&g_win_mu);
        if (error && error!=ETIMEDOUT) DIE("adaptive capacity notification wait: %d",error);
    }
    if (waiting) pres_capacity_end(b);
    if (decision==FG_ADAPTIVE_PRESENT_YIELD) {
        if (g_adaptive_presenting==(int)(b-g_pres)) g_adaptive_presenting=-1;
        pres_queue_locked((int)(b-g_pres));
    }
    pthread_mutex_unlock(&g_win_mu);
    return decision;
}
static void pres_retire_release_windows(void) {
    if (g_pres_retire.window) ANativeWindow_release(g_pres_retire.window);
    if (g_pres_retire.parent) ANativeWindow_release(g_pres_retire.parent);
    g_pres_retire.window=g_pres_retire.parent=NULL;
}
static void pres_retire_quarantine_surface(ASurfaceTransaction *tx,
        ASurfaceControl **surface,int *current) {
    atomic_store(&g_ahb_off,1);
    g_pres_retire.surface=*surface; g_pres_retire.transaction=tx;
    *surface=NULL; *current=-1;
    LOGV("window present: quarantined surface/context/buffer/window ownership retained until process exit; no removal/reuse/free");
}
static void pres_transaction_drain(pres_complete *contexts[VK_PRESENT_FLIGHT_CAPACITY],
        vk_present_flight *flight, int all) {
    while (flight->count && (all || flight->count == VK_PRESENT_FLIGHT_CAPACITY)) {
        unsigned index=flight->head;
        (void)pres_transaction_finish_until(contexts[index],0,flight->count);
        if (!vk_present_flight_retire(flight,index)) DIE("present transaction retirement order");
    }
}
static void pres_surface_remove(ASurfaceTransaction *tx, pres_complete *c, ASurfaceControl **surface, int *current) {
    if (!*surface) return;
    if (g_pres_retire.initialized || g_pres_retire.window) {
        uint64_t deadline=vk_present_retire_deadline();
        /* A mode cutover may already have joined the old worker. Removal of
         * that same async-owned surface still has bounded callback lifetime. */
        if (!g_pres_retire.initialized && !pres_retire_start()) {
            pres_retire_quarantine_surface(tx,surface,current); return;
        }
        vk_present_retire_token token;
        if (!vk_present_retire_drain(&g_pres_retire.queue,deadline) ||
                !vk_present_retire_reserve(&g_pres_retire.queue,
                    *current>=0 ? g_pres[*current].generation : 0,&token)) {
            (void)pres_retire_stop(deadline);
            pres_retire_quarantine_surface(tx,surface,current); return;
        }
        pres_complete *removal=g_pres_retire.contexts[token.index];
        removal->retire_async=1; removal->retire_token=token;
        ASurfaceTransaction_reparent(tx,*surface,NULL);
        pres_transaction(tx,removal,*surface,*current,NULL,0,1,1,0);
        if (!vk_present_retire_publish(&g_pres_retire.queue,token)) DIE("retirement removal publication");
        if (!pres_retire_stop(deadline)) {
            pres_retire_quarantine_surface(tx,surface,current); return;
        }
        for (unsigned i=0;i<VK_PRESENT_RETIRE_CAPACITY;i++) g_pres_retire.contexts[i]->retire_async=0;
    } else {
        c->retire_async=0;
        ASurfaceTransaction_reparent(tx, *surface, NULL);
        pres_transaction(tx, c, *surface, *current, NULL, 0, 0, 1, 0); /* legacy drain before removal */
    }
    pres_display_sweep(c, 1); /* query once more, then close unresolved old-generation fds */
    ASurfaceControl_release(*surface); *surface = NULL; *current = -1;
    pres_retire_release_windows();
}
/* Optional API 31 setter: never introduce a load-time dependency on it.
 * This presenter is the sole caller; request again on every new surface.
 * The setter returns void, so this logs a request, not verified readback. */
static int pres_backpressure(ASurfaceTransaction *tx, ASurfaceControl *surface, uint64_t generation) {
    static int resolved;
    static void (*set_backpressure)(ASurfaceTransaction *, ASurfaceControl *, bool);
    int api=android_get_device_api_level();
    if (!resolved) {
        resolved=1;
        if (api >= 31) set_backpressure=(void (*)(ASurfaceTransaction *, ASurfaceControl *, bool))
            dlsym(RTLD_DEFAULT,"ASurfaceTransaction_setEnableBackPressure");
    }
    int requested=vk_fg_locked() || vk_present_adaptive_backpressure_requested(vk_fg_adaptive(),
        getenv("TOTK_FG_ADAPTIVE_BACKPRESSURE"));
    int policy=vk_present_backpressure_policy(requested,getenv("TOTK_FRAMEGEN_BACKPRESSURE"),
        api,set_backpressure != NULL);
    if (policy == VK_PRESENT_BP_REQUEST) set_backpressure(tx,surface,true);
    if (vk_fg_locked() || vk_fg_adaptive()) LOGV("[framegen-backpressure] generation=%llu api=%d symbol=%d mode=%s; setter-void request only",
        (unsigned long long)generation,api,set_backpressure != NULL,
        policy == VK_PRESENT_BP_REQUEST ? "requested" :
        policy == VK_PRESENT_BP_DIAGNOSTIC_OFF ? "diagnostic-off-async" : "unavailable-serial");
    return policy;
}
static ASurfaceControl *pres_surface_create(ANativeWindow *window, ANativeWindow *parent, int display_id, uint64_t generation) {
    Dl_info provider = {0};
    dladdr((const void *)ASurfaceControl_createFromWindow, &provider);
    vk_present_extent extent = pres_frame_window_extent(window,GUEST_QUERY_PRESENTER);
    LOGV("window SurfaceControl: api=%d display=%d generation=%llu window=%p %dx%d format=%d explicit_parent=%p; direct NDK createFromWindow=%p transaction_create=%p provider=%s (no dlsym)",
         android_get_device_api_level(), display_id, (unsigned long long)generation, (void *)window,
         (int)extent.w, (int)extent.h, ANativeWindow_getFormat(window),
         (void *)parent, (void *)ASurfaceControl_createFromWindow, (void *)ASurfaceTransaction_create,
         provider.dli_fname ? provider.dli_fname : "unknown");
    errno = 0;
    ASurfaceControl *surface = ASurfaceControl_createFromWindow(window, "TOTK zero-copy");
    int error = errno;   /* This API returns only a pointer; errno is diagnostic, NOT a specified failure status. */
    LOGV("window SurfaceControl: ASurfaceControl_createFromWindow(NativeActivity window=%p) result=%p errno=%d (%s; errno not API status)",
         (void *)window, (void *)surface, error, strerror(error));
    if (!surface && parent) {
        errno = 0;
        surface = ASurfaceControl_createFromWindow(parent, "TOTK zero-copy attached");
        error = errno;
        LOGV("window SurfaceControl: ASurfaceControl_createFromWindow(attached Java Surface parent=%p) result=%p errno=%d (%s; errno not API status)",
             (void *)parent, (void *)surface, error, strerror(error));
    }
    if (!surface) LOGV("window SurfaceControl: no parentable surface api=%d display=%d generation=%llu explicit_parent=%p; using copy until window attachment changes (capture SurfaceComposerClient/SurfaceFlinger logcat)",
         android_get_device_api_level(), display_id, (unsigned long long)generation, (void *)parent);
    return surface;
}
static void pres_ahb_free(pres_ahb *a) {   /* only after both GPU and compositor have released this idle slot */
    for (int i = 0; i < 2; i++) if (a->view[i]) vkDestroyImageView(V.dev, a->view[i], NULL);
    if (a->image) vkDestroyImage(V.dev, a->image, NULL);
    vk_memory_release(a->mem, VK_MEMORY_PRESENT);
    if (a->buffer) AHardwareBuffer_release(a->buffer);
    memset(a, 0, sizeof *a);
}
/* native_handle_t and AHardwareBuffer_getNativeHandle are VNDK, not public NDK APIs. Match only the stable
   native_handle header ABI; resolve the accessor at runtime so a linker namespace denial safely selects copy. */
typedef struct { int version, numFds, numInts; int data[]; } pres_native_handle;
static const pres_native_handle *pres_ahb_handle(AHardwareBuffer *buffer) {
    static int tried;
    static const pres_native_handle *(*get_handle)(const AHardwareBuffer *);
    if (!tried) {
        tried = 1;
        void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
        get_handle = lib ? (const pres_native_handle *(*)(const AHardwareBuffer *))dlsym(lib, "AHardwareBuffer_getNativeHandle") : NULL;
        if (!get_handle) {
            if (lib) dlclose(lib);
            LOGV("window AHB: native handle accessor unavailable in app linker namespace; using copy");
        }
        /* Keep a successful library reference alive along with its function pointer. */
    }
    return get_handle ? get_handle(buffer) : NULL;
}
static int pres_ahb_alloc(pres_ahb *a, uint32_t w, uint32_t h) {
    if (a->buffer && a->w == w && a->h == h) return 1;
    pres_ahb_free(a);
    /* Qualcomm gralloc may otherwise choose a generation-specific UBWC layout which a separately loaded Turnip
       cannot reliably infer from legacy gralloc metadata. CPU-readable usage requests an uncompressed allocation;
       it does NOT map/read the buffer or add any CPU work to presentation. Import that allocation as linear too. */
    AHardwareBuffer_Desc desc = {.width = w, .height = h, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY};
    if (AHardwareBuffer_allocate(&desc, &a->buffer)) goto fail;
    AHardwareBuffer_Desc actual; AHardwareBuffer_describe(a->buffer, &actual);
    LOGV("window AHB allocation: %ux%u layers=%u format=%#x stride=%u usage=%#llx (requested CPU-readable linear)",
         actual.width, actual.height, actual.layers, actual.format, actual.stride, (unsigned long long)actual.usage);
    if (actual.width != w || actual.height != h || actual.layers != 1 || actual.format != desc.format || actual.stride < w ||
        (actual.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) != AHARDWAREBUFFER_USAGE_CPU_READ_RARELY) goto fail;
    const pres_native_handle *handle = pres_ahb_handle(a->buffer);
    if (!handle || handle->version != sizeof(pres_native_handle) || handle->numFds < 1 || handle->numInts < 0) {
        LOGV("window AHB: invalid/unavailable native handle; using copy"); goto fail;
    }
    /* Turnip's own AHB importer also uses data[0]; additional fds belong to gralloc metadata, not RGBA planes.
       Only the CPU-readable, single-layer RGBA allocation above is supported here. Never parse vendor integers. */
    int fd = handle->data[0];
    off_t allocation_size = lseek(fd, 0, SEEK_END);
    VkMemoryFdPropertiesKHR fd_props = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (allocation_size <= 0 || vkGetMemoryFdPropertiesKHR(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props)) {
        LOGV("window AHB: dma-buf size/memory properties unavailable; using copy"); goto fail;
    }
    /* RGBA8 storage with compatible UNORM/sRGB attachment views preserves the source transfer function,
       avoiding the decode-without-encode of an sRGB -> UNORM blit. */
    const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    for (unsigned i = 0; i < 2; i++) {
        VkFormatProperties properties; V.get_format_props(V.pd, formats[i], &properties);
        if (!(properties.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            LOGV("window AHB: format %d cannot render linear; using copy", formats[i]); goto fail;
        }
    }
    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = formats};
    /* DRM_FORMAT_MOD_LINEAR is zero. AHB-handle import cannot preserve this explicit layout in Mesa f7c93a1352:
       vk_android_init_deferred_image replaces tiling, and tu_AllocateMemory recomputes it from gralloc metadata.
       DMA_BUF import bypasses that deferred path and tu_image_init consumes our exact pitch, with UBWC disabled. */
    VkSubresourceLayout plane = {.offset = 0, .rowPitch = (VkDeviceSize)actual.stride * 4};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        &list, .drmFormatModifier = 0, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane};
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = formats[0], .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (vk_overlay_available() ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        &list, .drmFormatModifier = 0, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceExternalImageFormatInfo external_info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        &modifier_info, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external_info,
        .format = ci.format, .type = ci.imageType, .tiling = ci.tiling, .usage = ci.usage, .flags = ci.flags};
    VkExternalImageFormatProperties external_props = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_props};
    if (V.get_image_format_props2(V.pd, &query, &properties) ||
        !(external_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external_props.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
        properties.imageFormatProperties.maxExtent.width < w || properties.imageFormatProperties.maxExtent.height < h) {
        LOGV("window AHB: explicit LINEAR dma-buf color image unsupported; using copy"); goto fail;
    }
    if (vkCreateImage(V.dev, &ci, NULL, &a->image)) goto fail;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, a->image, &req);
    uint32_t memory_types = fd_props.memoryTypeBits & req.memoryTypeBits;
    int type = vk_pick_type(memory_types, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = vk_pick_type(memory_types, 0);
    if (type < 0 || req.size > (VkDeviceSize)allocation_size || plane.rowPitch * h > (VkDeviceSize)allocation_size) {
        LOGV("window AHB: dma-buf memory type/size incompatible (types=%#x need=%llu have=%llu); using copy",
             memory_types, (unsigned long long)req.size, (unsigned long long)allocation_size); goto fail;
    }
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
    if (imp.fd < 0) goto fail;
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, .image = a->image};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated,
        .allocationSize = (VkDeviceSize)allocation_size, .memoryTypeIndex = (uint32_t)type};
    VkResult result = vk_memory_allocate(&mai, &a->mem, VK_MEMORY_PRESENT);
    if (result) { close(imp.fd); goto fail; }   /* Vulkan owns the duplicate only after successful import. */
    if (vkBindImageMemory(V.dev, a->image, a->mem, 0)) goto fail;
    VkImageSubresource subresource = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0}; VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(V.dev, a->image, &subresource, &layout);
    VkImageDrmFormatModifierPropertiesEXT imported_modifier = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    if (V.get_image_modifier(V.dev, a->image, &imported_modifier)) goto fail;
    LOGV("window AHB imported layout: path=dma-buf-explicit tiling=DRM_FORMAT_MODIFIER_EXT modifier=%#llx planes=1 fds=%d stride=%u rowPitch=%llu expected=%llu offset=%llu size=%llu allocation=%llu memoryType=%d memoryTypes=%#x",
         (unsigned long long)imported_modifier.drmFormatModifier, handle->numFds, actual.stride,
         (unsigned long long)layout.rowPitch, (unsigned long long)plane.rowPitch, (unsigned long long)layout.offset,
         (unsigned long long)layout.size, (unsigned long long)allocation_size, type, memory_types);
    if (imported_modifier.drmFormatModifier != 0 || layout.offset || layout.rowPitch != plane.rowPitch ||
        layout.size > (VkDeviceSize)allocation_size || layout.rowPitch * h > (VkDeviceSize)allocation_size) {
        LOGV("window AHB: explicit LINEAR layout disagrees with gralloc stride/allocation; using copy"); goto fail;
    }
    for (int i = 0; i < 2; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = a->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(V.dev, &vi, NULL, &a->view[i])) goto fail;
    }
    a->w = w; a->h = h;
    LOGV("window AHB picture %ux%u (explicit LINEAR dma-buf, CPU-readable allocation, UNORM/sRGB views, no readback)", w, h);
    return 1;
fail:
    pres_ahb_free(a); LOGV("window present: AHB allocation/import %ux%u failed; using copy", w, h);
    atomic_store(&g_ahb_off, 1); return 0;
}
/* Separate opt-in gralloc-compressed path. Never reinterpret a compressed fd
   as LINEAR: import the AHB itself so the qualified driver's metadata parser
   owns the layout. Any refusal retires these temporary resources and retries
   the unchanged CPU-readable explicit-LINEAR path, not the copy presenter. */
static int pres_ahb_alloc_ubwc(pres_ahb *a, uint32_t w, uint32_t h) {
    const char *reason="AHB allocation";
    /* Qualcomm GRALLOC_USAGE_PRIVATE_ALLOC_UBWC; a request, not compression
       proof. V.get_ahb_props is admitted only for a qualified driver build. */
    const uint64_t compressed_usage=UINT64_C(1)<<28;
    AHardwareBuffer_Desc desc={.width=w,.height=h,.layers=1,.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage=AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | compressed_usage};
    if (AHardwareBuffer_allocate(&desc,&a->buffer)) goto fail;
    AHardwareBuffer_Desc actual; AHardwareBuffer_describe(a->buffer,&actual);
    LOGV("[present-ubwc] allocation request=%ux%u usage=%#llx actual=%ux%u format=%#x stride=%u usage=%#llx (UBWC requested, not public layout readback)",
        w,h,(unsigned long long)desc.usage,actual.width,actual.height,actual.format,actual.stride,
        (unsigned long long)actual.usage);
    reason="AHB descriptor";
    uint64_t gpu_usage=AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
        AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (actual.width!=w || actual.height!=h || actual.layers!=1 || actual.format!=desc.format || actual.stride<w ||
        (actual.usage & gpu_usage)!=gpu_usage ||
        (actual.usage & (AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK))) goto fail;
    VkAndroidHardwareBufferFormatPropertiesANDROID fp={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID ap={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&fp};
    reason="Vulkan AHB format/properties";
    VkFormatFeatureFlags features=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        (vk_overlay_available() ? VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0);
    if (V.get_ahb_props(V.dev,a->buffer,&ap) || fp.format!=VK_FORMAT_R8G8B8A8_UNORM ||
        !ap.allocationSize || !ap.memoryTypeBits || (fp.formatFeatures & features)!=features) goto fail;
    const VkFormat formats[2]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};
    for (unsigned i=0;i<2;i++) {
        VkFormatProperties properties; V.get_format_props(V.pd,formats[i],&properties);
        if ((properties.optimalTilingFeatures & features)!=features) goto fail;
    }
    VkImageFormatListCreateInfo list={VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount=2,.pViewFormats=formats};
    VkExternalMemoryImageCreateInfo ext={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.pNext=&list,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&ext,.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType=VK_IMAGE_TYPE_2D,.format=fp.format,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (vk_overlay_available() ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)};
    VkPhysicalDeviceExternalImageFormatInfo external={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext=&list,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkPhysicalDeviceImageFormatInfo2 query={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&external,
        .format=ci.format,.type=ci.imageType,.tiling=ci.tiling,.usage=ci.usage,.flags=ci.flags};
    VkAndroidHardwareBufferUsageANDROID usage={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
    VkExternalImageFormatProperties external_properties={VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,.pNext=&usage};
    VkImageFormatProperties2 properties={VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&external_properties};
    reason="external AHB image capabilities";
    if (V.get_image_format_props2(V.pd,&query,&properties) ||
        !(external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external_properties.externalMemoryProperties.compatibleHandleTypes & ext.handleTypes) ||
        properties.imageFormatProperties.maxExtent.width<w || properties.imageFormatProperties.maxExtent.height<h ||
        !(properties.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) ||
        (actual.usage & usage.androidHardwareBufferUsage)!=usage.androidHardwareBufferUsage) goto fail;
    reason="AHB image creation";
    if (vkCreateImage(V.dev,&ci,NULL,&a->image)) goto fail;
    /* AHB import size/type come from the actual allocation, never an unbound
       deferred image's requirements. Binding lets the driver decode layout. */
    int type=vk_pick_type(ap.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type<0) type=vk_pick_type(ap.memoryTypeBits,0);
    reason="AHB memory type";
    if (type<0) goto fail;
    VkImportAndroidHardwareBufferInfoANDROID import={VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.buffer=a->buffer};
    VkMemoryDedicatedAllocateInfo dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.pNext=&import,.image=a->image};
    VkMemoryAllocateInfo allocate={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&dedicated,
        .allocationSize=ap.allocationSize,.memoryTypeIndex=(uint32_t)type};
    reason="AHB allocation/import binding";
    if (vk_memory_allocate(&allocate,&a->mem,VK_MEMORY_PRESENT) || vkBindImageMemory(V.dev,a->image,a->mem,0)) goto fail;
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(V.dev,a->image,&requirements);
    reason="post-bind AHB layout bounds";
    /* This experiment rejects ambiguous padding too: a mistakenly LINEAR
       interpretation can be smaller than a compressed AHB, not just larger. */
    if (!vk_present_ahb_layout_matches(ap.allocationSize,requirements.size,requirements.memoryTypeBits,(unsigned)type)) {
        LOGV("[present-ubwc] refused: post-bind requirement=%llu allocation=%llu memoryType=%d allowed=%#x",
            (unsigned long long)requirements.size,(unsigned long long)ap.allocationSize,type,requirements.memoryTypeBits);
        goto fail;
    }
    reason="AHB UNORM/sRGB views";
    for (unsigned i=0;i<2;i++) {
        VkImageViewCreateInfo view={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=a->image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=formats[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if (vkCreateImageView(V.dev,&view,NULL,&a->view[i])) goto fail;
    }
    a->w=w; a->h=h; a->ahb_import=1;
    LOGV("[present-ubwc] imported=%ux%u format=%u allocation=%llu post-bind=%llu memoryType=%d usage=%#llx; qualified-driver AHB layout, UNORM/sRGB views; actual compression/HWC admission require device evidence",
        w,h,fp.format,(unsigned long long)ap.allocationSize,(unsigned long long)requirements.size,type,
        (unsigned long long)actual.usage);
    return 1;
fail:
    pres_ahb_free(a);
    LOGV("[present-ubwc] %s failed/refused for %ux%u; retaining explicit LINEAR presentation",reason,w,h);
    return 0;
}
static int pres_ahb_prepare(pres_ahb *a, uint32_t w, uint32_t h, int ubwc) {
    static int refused;
    int compressed=ubwc && V.get_ahb_props && !refused;
    if (compressed) {
        if (a->buffer && a->w==w && a->h==h && a->ahb_import) return 1;
        pres_ahb_free(a); /* the caller owns an idle GPU/compositor-released slot */
        if (pres_ahb_alloc_ubwc(a,w,h)) return 1;
        refused=1; /* no repeated compressed allocations after a compatibility refusal */
    } else if (a->ahb_import) pres_ahb_free(a);
    return pres_ahb_alloc(a,w,h);
}
static int pres_pipeline(unsigned rotation) {
    VkPipeline *pipe = rotation ? g_pres_rotated_pipe : g_pres_pipe;
    VkPipelineLayout *layout = rotation ? &g_pres_rotated_layout : &g_pres_layout;
    if (pipe[0] && pipe[1]) return 1;
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!g_pres_sampler && vkCreateSampler(V.dev, &sci, NULL, &g_pres_sampler)) goto fail;
    if (!g_pres_pixel_sampler && vk_native_ui_enabled()) {
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(V.dev, &sci, NULL, &g_pres_pixel_sampler)) goto fail;
    }
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    if (!g_pres_dsl && vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &g_pres_dsl)) goto fail;
    VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = (rotation ? 8 : 4) * sizeof(float)};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_pres_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    if (vkCreatePipelineLayout(V.dev, &lci, NULL, layout)) goto fail;
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = i ? sizeof pres_frag_spv : rotation ? sizeof pres_rotated_vert_spv : sizeof pres_vert_spv,
            .pCode = i ? pres_frag_spv : rotation ? pres_rotated_vert_spv : pres_vert_spv};
        if (vkCreateShaderModule(V.dev, &sm, NULL, &modules[i])) goto fail;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att};
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic};
    for (int i = 0; i < 2; i++) {
        VkFormat format = i ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dy, .layout = *layout};
        if (vk_pcache_graphics(&ci, &pipe[i])) goto fail;
    }
    for (int i = 0; i < 2; i++) vkDestroyShaderModule(V.dev, modules[i], NULL);
    return 1;
fail:
    for (int i = 0; i < 2; i++) {
        if (modules[i]) vkDestroyShaderModule(V.dev, modules[i], NULL);
        if (pipe[i]) { vkDestroyPipeline(V.dev, pipe[i], NULL); pipe[i] = VK_NULL_HANDLE; }
    }
    if (*layout) { vkDestroyPipelineLayout(V.dev, *layout, NULL); *layout = VK_NULL_HANDLE; }
    if (!g_pres_layout && !g_pres_rotated_layout) {
        if (g_pres_dsl) { vkDestroyDescriptorSetLayout(V.dev, g_pres_dsl, NULL); g_pres_dsl = VK_NULL_HANDLE; }
        if (g_pres_sampler) { vkDestroySampler(V.dev, g_pres_sampler, NULL); g_pres_sampler = VK_NULL_HANDLE; }
        if (g_pres_pixel_sampler) { vkDestroySampler(V.dev, g_pres_pixel_sampler, NULL); g_pres_pixel_sampler = VK_NULL_HANDLE; }
    }
    LOGV("window present: AHB blit pipeline failed; using copy"); atomic_store(&g_ahb_off, 1); return 0;
}
static int pres_ahb_draw(int i, const tex_t *t, vk_tex *x, int srgb, uint32_t ow, uint32_t oh,
                         int32_t cx, int32_t cy, uint32_t w, uint32_t h, int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         const vk_post_output *post, unsigned rotation, int ubwc, VkCommandBuffer prepared_cb) {
    if (!V.ahb_present || atomic_load(&g_ahb_off) || atomic_load(&g_ahb_surface_off)) return 0;
    VkFormat f = vk_tex_format(x);
    if (vk_tex_is3d(x) || (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB &&
        f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_B8G8R8A8_SRGB &&
        f != VK_FORMAT_A8B8G8R8_UNORM_PACK32 && f != VK_FORMAT_A8B8G8R8_SRGB_PACK32)) goto fail;
    view_t view = {.set_levels = 1, .nlevels = 1, .set_layers = 1, .nlayers = 1, .set_target = 1, .target = 1,
        .set_swizzle = 1, .swz = {2, 3, 4, 5}};   /* raw RGBA, like the old image blit, not the game's sampling swizzle */
    VkImageView src = post ? post->view : vk_tex_view(x, t, &view, 0); if (!src) goto fail;
    pres_ahb *a = &g_ahb[i];
    vk_present_extent native = {ow,oh};
    vk_present_rect draw = {dx,dy,(uint32_t)dw,(uint32_t)dh};
    if (rotation) {
        native = vk_present_rotate_extent(ow,oh,rotation);
        draw = vk_present_rotate_rect(draw,ow,oh,rotation);
    }
    if (!pres_pipeline(rotation) || !pres_ahb_prepare(a, native.w, native.h, ubwc)) return 0;
    VkPipelineLayout layout = rotation ? g_pres_rotated_layout : g_pres_layout;
    VkPipeline pipeline = rotation ? g_pres_rotated_pipe[srgb] : g_pres_pipe[srgb];
    if (!prepared_cb) {
        if (vk_lag_uniform_private()) vk_ub_touch((uint64_t)(uintptr_t)t->mem, t->L.size); else vk_ub_flush();
        vk_end_rendering_at(VK_PASS_END_PRESENT, 1);
    }
    VkCommandBuffer cb = prepared_cb ? prepared_cb : vk_cb();
    int scope = prepared_cb ? -1 : vk_native_gpu_begin(cb,VK_NUI_PRESENT_AHB); vk_barrier_all(cb);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = a->foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = V.qfam, .image = a->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view[srgb],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = {{0, 0, 0, 1}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {native.w, native.h}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
    VkDescriptorSet set = vk_alloc_set(g_pres_dsl);
    VkSampler sampler = g_pres_pixel_sampler && w == (uint32_t)dw && h == (uint32_t)dh ? g_pres_pixel_sampler : g_pres_sampler;
    VkDescriptorImageInfo image = {.sampler = sampler, .imageView = src, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
    vkUpdateDescriptorSets(V.dev, 1, &write, 0, NULL);
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    float iw = post ? post->w : vk_tex_width(x, 0), ih = post ? post->h : vk_tex_height(x, 0);
    if (rotation) {
        float push[8] = {(float)cx / iw, (float)cy / ih, (float)w / iw, (float)h / ih};
        vk_present_rotation_matrix(rotation,push+4);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    } else {
        float push[4] = {(float)cx / iw, (float)cy / ih, (float)w / iw, (float)h / ih};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    }
    VkViewport vp = {(float)draw.x, (float)draw.y, (float)draw.w, (float)draw.h, 0, 1};
    VkRect2D sc = {{draw.x, draw.y}, {draw.w, draw.h}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc); vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_overlay_record(cb, a->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ow, oh, (unsigned)i, rotation);
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_NONE; ib.dstAccessMask = 0;
    ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = V.qfam; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier2(cb, &di);
    /* Ownership becomes foreign only after this command stream is submitted;
     * private prepare/cancel must retain the slot's actual prior GPU state. */
    if (!prepared_cb) vk_native_gpu_end(cb,scope);
    g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; memset(g_bound_set[0], 0, sizeof g_bound_set[0]);
    return 1;
fail:
    LOGV("window present: source cannot be sampled for AHB blit; using copy"); atomic_store(&g_ahb_off, 1); return 0;
}
#endif
uint64_t vk_window_set(ANativeWindow *w, ANativeWindow *surface_parent, int display_id) {
    pthread_mutex_lock(&g_win_mu);
    if (w) ANativeWindow_acquire(w);
    if (g_win) ANativeWindow_release(g_win);
#ifdef __ANDROID__
    if (surface_parent) ANativeWindow_acquire(surface_parent);
    if (g_win_parent) ANativeWindow_release(g_win_parent);
    g_win_parent = surface_parent; g_win_display_id = display_id;
    atomic_store(&g_ahb_surface_off, 0);
#endif
    g_win = w; g_win_w = g_win_h = 0; g_win_generation++; pthread_cond_signal(&g_pres_cv);
    if (pres_slots_requested()) pres_slots_end_locked();
    pres_adaptive_wake_locked();
    const char *prerotate = getenv("TOTK_PRESENT_PREROTATE");
    g_win_prerotate = prerotate && !strcmp(prerotate,"1"); g_win_transform_hint = -1;
    const char *ubwc = getenv("TOTK_PRESENT_UBWC");
    g_win_ubwc = ubwc && !strcmp(ubwc,"1");
    uint64_t epoch = ++g_win_attachment_epoch;
    vk_present_extent extent = pres_window_extent(w);
    atomic_store_explicit(&g_guest_window_wh, pres_pack_extent(extent.w, extent.h), memory_order_relaxed);
    pres_guest_fit_refresh();
    if (w) { g_win_pw = extent.w; g_win_ph = extent.h; }
#ifdef __ANDROID__
    if (w) totk_perf_window(w);
#endif
    if (w) vk_native_window_size(g_win_pw, g_win_ph);
    pthread_mutex_unlock(&g_win_mu);
    vk_fg_reset();   /* atomic request only; recording owner retires/invalidates history */
    fprintf(stderr, "[nvn-vk] window %s\n", w ? "attached" : "detached");
    return epoch;
}
/* UI-thread hint listener supplies the exact attached root's transform. A
   quarter-turn/180 change invalidates queued jobs even when width/height do not
   change; an old attachment's listener cannot rotate the replacement window. */
void vk_window_transform_hint(ANativeWindow *w, uint64_t epoch, int hint) {
    pthread_mutex_lock(&g_win_mu);
    if (w && w == g_win && epoch == g_win_attachment_epoch && (g_win_prerotate || g_win_ubwc) &&
        hint != g_win_transform_hint) {
        g_win_transform_hint = hint; g_win_generation++;
        if (pres_slots_requested()) pres_slots_end_locked();
        pres_adaptive_wake_locked();
        int turns = vk_present_transform_turns(hint);
        LOGV("[present-prerotate] generation=%llu hint=%d turns=%d rotate=%d inverse=%d ubwc=%d; %s (composer/HWC identity not verified)",
            (unsigned long long)g_win_generation,hint,turns,g_win_prerotate && turns>0,
            g_win_prerotate && turns>=0 ? vk_present_turns_transform((4-(unsigned)turns)&3) : 0,
            g_win_ubwc && turns>0,
            turns >= 0 ? "runtime root hint accepted" : "unavailable/unsupported hint: identity and LINEAR retained");
        pthread_cond_signal(&g_pres_cv);
    }
    pthread_mutex_unlock(&g_win_mu);
}
/* UI lifecycle notification, not frame polling. Ignore callbacks for retired
   windows; a changed guest extent invalidates queued jobs from the old epoch. */
void vk_window_resized(ANativeWindow *w, uint64_t epoch) {
    pthread_mutex_lock(&g_win_mu);
    if (w && w == g_win && epoch == g_win_attachment_epoch) {
        vk_present_extent old = pres_guest_window(), extent = pres_window_extent(w);
        if (old.w != extent.w || old.h != extent.h) {
            atomic_store_explicit(&g_guest_window_wh, pres_pack_extent(extent.w, extent.h), memory_order_relaxed);
            pres_guest_fit_refresh();
            if (g_win_prerotate || g_win_ubwc) {
                g_win_pw = extent.w; g_win_ph = extent.h;
                vk_native_window_size(extent.w,extent.h); /* native UI stays in logical-window coordinates */
            }
            if (g_pres_guest_output || g_win_prerotate || g_win_ubwc) {
                g_win_generation++;
                pthread_cond_signal(&g_pres_cv);
                pres_adaptive_wake_locked();
            }
        }
    }
    pthread_mutex_unlock(&g_win_mu);
}
/* Diagnostics: a frame whose picture is almost all black right after a normal one (the brief black flashes) is logged
   with what happened since the last frame shown, and saved as shots/black_<frame>.png. TOTK_VK_BLACK=0: off. */
static void black_check(const pres_buf *b) {
    /* A flash: a frame far darker than the last few, with the brightness back within five frames (a fade to black stays
       dark and is not one). The dark frame is kept until that is known; up to 40 are logged and 5 saved. */
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_BLACK"); on = !(e && !strcmp(e, "0")); }
    static double avg = -1, dark_m, dark_avg; static uint64_t pctr[10], dctr[10], pframe, dframe, dprev; static int saved, logged, pending;
    static uint8_t *keep; static uint32_t kw, kh;
    if (!on) return;
    double sum = 0;   /* brightness at 49 points across the picture */
    for (uint32_t gy = 1; gy < 8; gy++) for (uint32_t gx = 1; gx < 8; gx++) { const uint8_t *p = b->ptr + ((size_t)(b->h * gy / 8) * b->w + b->w * gx / 8) * 4; sum += (p[0] + p[1] + p[2]) / 3.0; }
    double m = sum / 49;
    if (pending) {
        if (m >= dark_avg * 0.75) {   /* back: it was a flash */
            if (logged++ < 40) {
                uint64_t d[10]; for (int i = 0; i < 10; i++) d[i] = dctr[i] - pctr[i];
                LOGV("【红】dark flash at frame %llu (after frame %llu; brightness %.1f, around it %.1f; back at frame %llu): before it %llu draws, %llu passes, %llu skipped, %llu images made, %llu shared reuses, %llu uploads, %llu re-uploads, %llu swept, %llu CPU-pool images shared, %llu serials switched",
                     (unsigned long long)dframe, (unsigned long long)dprev, dark_m, dark_avg, (unsigned long long)b->frame, (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
                     (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9]);
            }
            if (logged <= 12) probe_report(dframe, b->frame);   /* against the frame after it: the same view after a camera cut */
            if (saved < 5 && keep) { saved++; mkdir("shots", 0755); char path[256]; snprintf(path, sizeof path, "shots/flash_%06llu.png", (unsigned long long)dframe); write_png(path, keep, kw, kh, (size_t)kw * 4); }
            pending = 0;
        } else if (b->frame - dframe > 5) { pending = 0; avg = m; }   /* stayed dark: a fade (the new brightness is the normal one now) */
    } else if (avg > 6 && m < avg * 0.45) {
        pending = 1; dark_m = m; dark_avg = avg; dframe = b->frame; dprev = pframe; memcpy(dctr, b->ctr, sizeof dctr);
        if (saved < 5) { size_t n = (size_t)b->w * b->h * 4; if (!keep || kw * kh * 4 < n) keep = realloc(keep, n); memcpy(keep, b->ptr, n); kw = b->w; kh = b->h; }
        return;   /* pctr stays the frame before it */
    }
    if (!pending && m >= avg * 0.45) avg = avg < 0 ? m : avg * 0.75 + m * 0.25;
    if (!pending) { memcpy(pctr, b->ctr, sizeof pctr); pframe = b->frame; }
}
static void *presenter(void *arg) {
    (void)arg;
#ifdef __ANDROID__
    totk_thread_start("totk-present", NULL);   /* name, performance hint and frame-thread settings */
    ASurfaceControl *surface = NULL; ASurfaceTransaction *tx = NULL; uint64_t surface_generation = 0; int current = -1;
    int backpressure_policy=VK_PRESENT_BP_SERIAL;
    static pres_complete completion = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    static pres_complete second = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    pres_complete *contexts[VK_PRESENT_FLIGHT_CAPACITY]={&completion,&second};
    completion.observer=second.observer=&completion;
    g_pres_retire.contexts[0]=&completion; g_pres_retire.contexts[1]=&second;
    if (g_pres_async_retire) {
        pthread_condattr_t attr;
        int error=pthread_condattr_init(&attr);
        if (error) DIE("retirement condattr: %d",error);
        error=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
        for (unsigned i=0;!error && i<VK_PRESENT_RETIRE_CAPACITY;i++) {
            pthread_cond_destroy(&contexts[i]->cv);
            error=pthread_cond_init(&contexts[i]->cv,&attr);
        }
        if (!error) error=pthread_cond_init(&g_pres_retire_notify_cv,&attr);
        pthread_condattr_destroy(&attr);
        if (error) DIE("retirement monotonic condition: %d",error);
        atomic_store_explicit(&g_pres_retire_notify_ready,1,memory_order_release);
    }
    vk_present_flight flight={0};
    uint64_t retire_logged_generation=UINT64_MAX;
    int retire_logged_active=-1,retire_logged_qualified=-1;
    int retire_qualified=0; /* existing Thor/AFME admission, not transient scene readiness */
    unsigned hinted_rate = 0;
#endif
    for (;;) {
#ifdef __ANDROID__
        totk_live_slack_tick();   /* a live TOTK_TIMER_SLACK_NS: the presenter applies it itself, before its deadline waits */
        if (!g_pres_retire.running) {
            /* OFF keeps its original finish-before-pickup policy. Attribute
             * genuine full-context waiting to the still-queued immutable job. */
            pres_buf *capacity=NULL;
            if (g_fg_job_trace && flight.count==VK_PRESENT_FLIGHT_CAPACITY) {
                pthread_mutex_lock(&g_win_mu);
                if (g_pres_qh!=g_pres_qt) capacity=&g_pres[g_pres_q[g_pres_qh%(NPRES*2)]];
                if (capacity) {
                    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS];
                    for (unsigned n=0;n<flight.count;n++) {
                        unsigned index=(flight.head+n)%VK_PRESENT_FLIGHT_CAPACITY;
                        vk_present_retire_token token=contexts[index]->retire_token;
                        live[n]=(fgs_transaction_id){token.index,token.sequence,token.generation};
                    }
                    pres_capacity_begin_live(capacity,live,flight.count);
                }
                pthread_mutex_unlock(&g_win_mu);
            }
            pres_transaction_drain(contexts,&flight,0);
            if (capacity) pres_capacity_end(capacity);
        }
#endif
        pthread_mutex_lock(&g_win_mu);
        for (;;) {
#ifdef __ANDROID__
            int notifying=g_pres_retire.running;
            if (notifying) pthread_mutex_lock(&g_pres_retire_notify_mu);
#endif
            int update = g_pres_qh != g_pres_qt;
#ifdef __ANDROID__
            update |= surface && (surface_generation != g_win_generation || atomic_load(&g_ahb_off));
#endif
            if (update) {
#ifdef __ANDROID__
                if (notifying) pthread_mutex_unlock(&g_pres_retire_notify_mu);
#endif
                break;
            }
#ifdef __ANDROID__
            if (notifying) {
                pthread_mutex_unlock(&g_win_mu);
                pthread_cond_wait(&g_pres_retire_notify_cv,&g_pres_retire_notify_mu);
                pthread_mutex_unlock(&g_pres_retire_notify_mu);
                pthread_mutex_lock(&g_win_mu);
            } else
#endif
                pthread_cond_wait(&g_pres_cv, &g_win_mu);
        }
#ifdef __ANDROID__
        if (surface && (surface_generation != g_win_generation || atomic_load(&g_ahb_off))) {
            pthread_mutex_unlock(&g_win_mu);
            pres_transaction_drain(contexts,&flight,1);
            pres_surface_remove(tx, &completion, &surface, &current);
            if (g_pres_retire.transaction==tx) tx=NULL; /* retained quarantined transaction */
            continue;
        }
#endif
        int i = g_pres_q[g_pres_qh++ % (NPRES * 2)];
        if (g_pres[i].adaptive_epoch && g_pres[i].generated) g_adaptive_presenting=i;
        if (g_pres_trace || g_pres_timeline || g_fg_job_trace) {
            g_pres[i].dequeue_ns=host_now_ns();
            g_pres[i].queue_depth=(unsigned)(g_pres_qt-g_pres_qh);
        }
        pthread_mutex_unlock(&g_win_mu);
#ifdef __ANDROID__
        pres_display_sweep(&completion, 0); /* prior CURRENT fences, never a blocking display wait */
#endif
        pres_buf *b = &g_pres[i];
        /* A timeout is not completion: neither CPU nor compositor may read pixels the GPU is still writing. */
        while (!mtl_wait(b->fence, 2000000000ull)) RED(2, "window present: waiting over two seconds for the GPU");
        /* N was retained until N+1 was recorded. In admitted timeline mode N's
         * own immutable pixels may meet their earlier API deadline while N+1
         * is still rendering; children retain all own/source completion gates. */
        if (runtime_present_pair_wait_required(g_pres_timeline ? b->timeline_role:0,
                b->generated!=0,b->source_fence!=0))
            while (!mtl_wait(b->source_fence, 2000000000ull)) RED(2, "window present: waiting over two seconds for interpolation pair");
#ifdef __ANDROID__
        if (!b->adaptive_epoch && g_pres_retire.running) {
            if (!pres_retire_stop(vk_present_retire_deadline())) {
                pres_child_expire(b,FRAMEGEN_CHILD_WINDOW); continue;
            }
            for (unsigned k=0;k<VK_PRESENT_RETIRE_CAPACITY;k++) contexts[k]->retire_async=0;
        }
#endif
        b->ready_ns = b->prediction.id || b->auto_frame.id || totk_ft_on() ? vk_present_ready_ns(b->fence) : 0;
        if (g_pres_trace || g_pres_timeline || g_fg_job_trace) {
            b->own_ready_ns=vk_present_ready_ns(b->fence);
            b->pair_ready_ns=b->source_fence ? vk_present_ready_ns(b->source_fence) : b->own_ready_ns;
        }
        if (!b->generated && b->timeline_frame && totk_ft_on())
            totk_ft_present_ready(b->timeline_frame, b->ready_ns);
        fg_adaptive_present_result adaptive_decision=FG_ADAPTIVE_PRESENT_APPLY;
        if (b->adaptive_epoch && !b->discard) {
            pthread_mutex_lock(&g_win_mu);
            adaptive_decision=pres_adaptive_wait_locked(b);
            if (adaptive_decision==FG_ADAPTIVE_PRESENT_YIELD) {
                if (g_adaptive_presenting==i) g_adaptive_presenting=-1;
                pres_queue_locked(i); /* ownership/fences remain intact; earlier real is next */
            }
            pthread_mutex_unlock(&g_win_mu);
            if (adaptive_decision==FG_ADAPTIVE_PRESENT_YIELD) continue;
        }
        vk_post_complete((unsigned)i);   /* timing readback piggybacks on the existing GPU fence */
        if (b->discard) { pres_child_expire(b,FRAMEGEN_CHILD_OUTPUT); continue; }
        if (adaptive_decision!=FG_ADAPTIVE_PRESENT_APPLY) {
            pres_adaptive_expire(b,adaptive_decision); continue;
        }
        if (b->generated) {
            vk_present_job_completion generated_completion=vk_present_job_complete(&b->job,(unsigned)i,vk_fg_complete);
            int output_ok=generated_completion.accepted;
            if (generated_completion.first && output_ok) g_fg_accepted++;
            uint64_t source_ready = vk_present_ready_ns(b->source_fence);
            framegen_child_reason reason = !output_ok ? FRAMEGEN_CHILD_COMPLETION :
                b->generated == 2 ? FRAMEGEN_CHILD_OUTPUT :
                b->adaptive_epoch ? fg_adaptive_child_status(source_ready,
                    vk_present_ready_ns(b->fence),b->desired_ns,host_now_ns()) :
                b->interpolated ? framegen_interp_child_status(source_ready, b->desired_ns, host_now_ns()) :
                framegen_child_ready(source_ready, b->source_slot, b->desired_ns, host_now_ns()) ?
                    FRAMEGEN_CHILD_READY : !source_ready ? FRAMEGEN_CHILD_SOURCE_MISSING :
                    source_ready > b->source_slot ? FRAMEGEN_CHILD_SOURCE_LATE : FRAMEGEN_CHILD_DEADLINE;
            if (reason != FRAMEGEN_CHILD_READY) { pres_child_expire(b, reason); continue; }
        }
        pthread_mutex_lock(&g_win_mu);
        ANativeWindow *w = b->generation == g_win_generation ? g_win : NULL; if (w) ANativeWindow_acquire(w);
        vk_present_extent guest_window=g_pres_guest_output ?
            (g_guest_extent_cached ? pres_guest_window() : pres_frame_window_extent(w,GUEST_QUERY_PRESENTER)) :
            (vk_present_extent){0};
        int stale_copy=g_pres_guest_output && !b->zero && w &&
            (guest_window.w!=b->w || guest_window.h!=b->h);
#ifdef __ANDROID__
        uint32_t pw = g_win_pw, ph = g_win_ph;
        if (g_pres_guest_output) { pw=guest_window.w; ph=guest_window.h; }
        ANativeWindow *parent = w ? g_win_parent : NULL; if (parent) ANativeWindow_acquire(parent);
        int display_id = g_win_display_id;
#endif
        int resize = !stale_copy && !b->zero && w && (g_win_w != b->w || g_win_h != b->h);
        if (resize) { g_win_w = b->w; g_win_h = b->h; }
        pthread_mutex_unlock(&g_win_mu);
#ifdef __ANDROID__
        if (b->generated) totk_perf_generated();   /* TOTK_RATE_HINT_HOLD: frame generation is running */
        unsigned requested_rate = totk_perf_rate();
        if (w && requested_rate != hinted_rate) {
            totk_perf_window(w);
            if (surface && tx) totk_perf_surface(tx, surface);
            hinted_rate = requested_rate;
        }
#endif
        /* A resize can race a queued copy. Drop that old-sized backing before
         * publishing geometry; the next producer uses the live window size. */
        if (stale_copy) {
#ifdef __ANDROID__
            if (parent) ANativeWindow_release(parent);
#endif
            ANativeWindow_release(w); pres_child_expire(b,FRAMEGEN_CHILD_WINDOW); continue;
        }
#ifdef __ANDROID__
        if (b->zero) {
            if (w && !atomic_load(&g_ahb_off) && !atomic_load(&g_ahb_surface_off)) {
                if (!tx) {
                    errno = 0; tx = ASurfaceTransaction_create(); int error = errno;
                    LOGV("window SurfaceControl: ASurfaceTransaction_create result=%p errno=%d (errno not API status)", (void *)tx, error);
                }
                int created = !surface;
                if (tx && !surface) {
                    surface = pres_surface_create(w, parent, display_id, b->generation);
                    surface_generation = b->generation;
                    if (surface) {
                        backpressure_policy=pres_backpressure(tx,surface,b->generation);
                        totk_perf_surface(tx, surface);
                        hinted_rate = totk_perf_rate();
                    }
                }
                if (tx && surface) {
                    if (g_pres_guest_output && (!pw || !ph)) {
                        pres_child_expire(b,FRAMEGEN_CHILD_WINDOW);
                        if (parent) ANativeWindow_release(parent);
                        ANativeWindow_release(w); continue;
                    }
                    /* One-time bridge/surface creation is not steady presenter
                     * allowance. Its wall delay remains visible in diagnostics. */
                    uint64_t now=host_now_ns();
                    if (b->adaptive_epoch && vk_fg_qcom_active()) retire_qualified=1;
                    int qualified=retire_qualified;
                    int common_request=vk_present_adaptive_backpressure_requested(b->adaptive_epoch!=0,
                        getenv("TOTK_FG_ADAPTIVE_BACKPRESSURE")) && backpressure_policy==VK_PRESENT_BP_REQUEST;
                    int use_async=vk_present_retire_admitted(g_pres_async_retire,b->adaptive_epoch!=0,
                        qualified,common_request);
                    if (use_async && !g_pres_retire.window) {
                        g_pres_retire.window=w; ANativeWindow_acquire(w);
                        g_pres_retire.parent=parent; if (parent) ANativeWindow_acquire(parent);
                    }
                    if (use_async && !g_pres_retire.running) {
                        pres_transaction_drain(contexts,&flight,1);
                        if (!pres_retire_start()) { atomic_store(&g_ahb_off,1); use_async=0; }
                        if (!use_async) {
                            LOGV("[framegen-async-retire] generation=%llu requested=on active=0 capacity=2 qualified=%d backpressure=%s budget-ns=2000000000 reason=worker-unavailable",
                                (unsigned long long)b->generation,qualified,
                                common_request ? "requested" : "unavailable");
                            pres_retire_quarantine_surface(tx,&surface,&current); tx=NULL;
                            pres_child_expire(b,FRAMEGEN_CHILD_WINDOW);
                            if (parent) ANativeWindow_release(parent);
                            ANativeWindow_release(w); continue;
                        }
                    }
                    if (b->adaptive_epoch && (retire_logged_generation!=b->generation ||
                            retire_logged_active!=use_async || retire_logged_qualified!=qualified)) {
                        LOGV("[framegen-async-retire] generation=%llu requested=%s active=%d capacity=2 qualified=%d backpressure=%s budget-ns=2000000000",
                            (unsigned long long)b->generation,g_pres_async_retire ? "on" : "off",
                            use_async,qualified,common_request ? "requested" : "unavailable");
                        retire_logged_generation=b->generation;
                        retire_logged_active=use_async; retire_logged_qualified=qualified;
                    }
                    b->job.retire_async=use_async;
                    int defer=use_async || vk_present_flight_defer(backpressure_policy,b->refresh_ns,b->desired_ns,now);
                    if (!defer) pres_transaction_drain(contexts,&flight,1);
                    uint64_t child_deadline=b->timeline_role ? b->display_timeline.deadline_ns:b->desired_ns;
                    if (b->interpolated && host_now_ns() >= child_deadline) {
                        if (g_pres_timeline && b->timeline_role) {
                            static uint64_t missed;
                            atomic_fetch_add_explicit(&g_timeline_deadline_late[1],1,memory_order_relaxed);
                            if (++missed<=8 || missed%300==0)
                                LOGV("[present-timeline-miss] role=generated frame=%llu id=%lld expected=%llu deadline=%llu record=%llu enqueue=%llu dequeue=%llu own-ready=%llu pair-ready=%llu now=%llu total=%llu",
                                    (unsigned long long)b->frame,(long long)b->display_timeline.vsync_id,
                                    (unsigned long long)b->desired_ns,(unsigned long long)child_deadline,
                                    (unsigned long long)b->record_done_ns,(unsigned long long)b->enqueue_ns,
                                    (unsigned long long)b->dequeue_ns,(unsigned long long)b->own_ready_ns,
                                    (unsigned long long)b->pair_ready_ns,(unsigned long long)host_now_ns(),
                                    (unsigned long long)missed);
                        }
                        pres_child_expire(b, FRAMEGEN_CHILD_DEADLINE);
                        if (parent) ANativeWindow_release(parent);
                        ANativeWindow_release(w); continue;
                    }
                    vk_present_retire_token token={0};
                    if (use_async) {
                        fg_adaptive_present_result decision=pres_retire_reserve(b,&token);
                        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) {
                            if (decision!=FG_ADAPTIVE_PRESENT_YIELD) pres_adaptive_expire(b,decision);
                            if (parent) ANativeWindow_release(parent);
                            ANativeWindow_release(w); continue;
                        }
                    }
                    /* Cancel before staging any buffer in the reusable transaction;
                     * otherwise teardown could apply a canceled child's pixels. */
                    uint64_t setup_start = b->prediction.id || b->auto_frame.id ? host_now_ns() : 0;
                    if (vk_native_ui_enabled() && (!pw || !ph)) { pw = b->w; ph = b->h; }
                    /* Crop in the actual native AHB coordinates, then undo
                     * its rendering rotation into the parent's logical space.
                     * Display transform * inverse(hint) can then be identity. */
                    ARect src = {0, 0, (int32_t)g_ahb[i].w, (int32_t)g_ahb[i].h};
                    ARect dst = {0, 0, (int32_t)pw, (int32_t)ph};
                    if (g_pres_guest_output) {
                        vk_present_rect fit=g_guest_extent_cached ? b->guest_fit : vk_present_guest_fit(b->w,b->h,pw,ph);
                        dst=(ARect){fit.x,fit.y,fit.x+(int32_t)fit.w,fit.y+(int32_t)fit.h};
                    }
                    int use_timeline=g_pres_timeline && b->timeline_role && b->display_timeline.vsync_id>=0 &&
                        runtime_present_timeline_display_allowed(runtime_android_present_timeline_display_id(),display_id);
                    if (b->adaptive_epoch) {
                        pthread_mutex_lock(&g_win_mu);
                        fg_adaptive_present_result decision=pres_adaptive_decide_locked(b);
                        uint64_t desired;
                        if (b->adaptive_slots && b->generated && !pres_slots_desired(b,&desired))
                            decision=FG_ADAPTIVE_PRESENT_SUPPRESSED;
                        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) {
                            pthread_mutex_unlock(&g_win_mu);
                            if (use_async && !vk_present_retire_cancel(&g_pres_retire.queue,token))
                                DIE("retirement pre-stage cancellation");
                            pres_adaptive_expire(b,decision);
                            if (parent) ANativeWindow_release(parent);
                            ANativeWindow_release(w); continue;
                        }
                        /* Normal queue/drain cancellation costs no new
                         * transaction: nothing has been staged yet. */
                    }
                    ASurfaceTransaction_setGeometry(tx, surface, &src, &dst,
                        vk_present_turns_transform((4-b->rotation)&3));
                    ASurfaceTransaction_setBufferTransparency(tx, surface, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
                    ASurfaceTransaction_setBufferDataSpace(tx, surface, ADATASPACE_SRGB);
                    if (use_timeline && !b->adaptive_slots)
                        g_pres_set_timeline(tx,b->display_timeline.vsync_id);
                    ASurfaceTransaction_setBuffer(tx, surface, g_ahb[i].buffer, -1);   /* GPU fence was waited above */
                    uint64_t setup_ns = setup_start ? host_now_ns() - setup_start : 0;
                    if (created) LOGV("window SurfaceControl: transaction configured surface=%p logical=%ux%u buffer=%ux%u->%ux%u rotation=%u inverse=%d desired_ns=%llu; setters/apply return void; awaiting first completion",
                         (void *)surface, b->w, b->h, g_ahb[i].w, g_ahb[i].h, pw, ph, b->rotation,
                         vk_present_turns_transform((4-b->rotation)&3), (unsigned long long)b->desired_ns);
                    if (!b->adaptive_epoch) {
                        vk_overlay_presented(host_now_ns(), b->generated); /* legacy handoff timing */
                        if (b->generated) pres_child_handoff();
                    }
                    int context_index=use_async ? (int)token.index : defer ? vk_present_flight_next(&flight) : 0;
                    if (context_index < 0) DIE("present transaction capacity");
                    contexts[context_index]->retire_async=use_async;
                    if (use_async) contexts[context_index]->retire_token=token;
                    unsigned pending=use_async ? vk_present_retire_count(&g_pres_retire.queue) : defer ? flight.count+1 : 1;
                    fg_adaptive_present_result applied=pres_transaction(tx, contexts[context_index], surface, current, b, setup_ns,
                        defer,pending,b->adaptive_epoch!=0);
                    if (applied!=FG_ADAPTIVE_PRESENT_APPLY) {
                        /* Drop all staged buffer/geometry/callback state. A later
                         * teardown must never apply this canceled candidate. */
                        ASurfaceTransaction_delete(tx); tx=ASurfaceTransaction_create();
                        if (!tx) DIE("adaptive presenter cannot replace canceled transaction");
                        if (use_async && !vk_present_retire_cancel(&g_pres_retire.queue,token))
                            DIE("retirement staged cancellation");
                        pres_adaptive_expire(b,applied);
                        if (parent) ANativeWindow_release(parent);
                        ANativeWindow_release(w); continue;
                    }
                    if (use_async) {
                        if (!vk_present_retire_publish(&g_pres_retire.queue,token)) DIE("retirement publication order");
                    } else if (defer && !vk_present_flight_submit(&flight,(unsigned)context_index))
                        DIE("present transaction submission order");
                    current = i;
                    if (created) LOGV("window SurfaceControl: first transaction applied surface=%p api=%d display=%d generation=%llu",
                         (void *)surface, android_get_device_api_level(), display_id, (unsigned long long)b->generation);
                    g_pres_shown++;
                    if (b->adaptive_epoch) atomic_fetch_add_explicit(&g_adaptive_stats.shown,1,memory_order_relaxed);
                    if (!b->generated) front_frame_presented();
                    if (parent) ANativeWindow_release(parent);
                    ANativeWindow_release(w); continue;   /* still busy: this is the compositor's CURRENT buffer */
                }
                LOGV("window present: SurfaceControl setup failed (transaction=%p surface=%p); using copy for window generation %llu",
                     (void *)tx, (void *)surface, (unsigned long long)b->generation);
                pthread_mutex_lock(&g_win_mu);
                if (b->generation == g_win_generation) atomic_store(&g_ahb_surface_off, 1);
                pthread_mutex_unlock(&g_win_mu);
            }
            if (parent) ANativeWindow_release(parent);
            if (w) ANativeWindow_release(w);
            pres_child_expire(b, FRAMEGEN_CHILD_WINDOW); continue;
        }
        if (parent) ANativeWindow_release(parent);
#endif
        if (b->adaptive_epoch) {
            /* Public copy has no cancel-without-post operation or serialized
             * handoff. Adaptive requires the strict native-UI/AHB route. */
            if (w) ANativeWindow_release(w);
            pres_child_expire(b,FRAMEGEN_CHILD_WINDOW); continue;
        }
        uint64_t preparation_start = b->prediction.id || b->auto_frame.id ? host_now_ns() : 0;
        uint64_t preparation_ns = 0;
        int posted = 0;
        if (!b->coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->mem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
        if (w) {
            /* Public ANativeWindow copy has no buffer timestamp API. Do not
             * post before the nominal slot; scanout may consequently be one
             * refresh later. No graphics/recorder lock is held while waiting. */
            if (b->refresh_ns && !b->prediction.id && !b->interpolated) {
                /* Auto measures real preparation without moving the legacy
                 * wait-before-lock order or counting its deliberate slot hold. */
                if (preparation_start) preparation_ns = host_now_ns() - preparation_start;
                runtime_android_present_wait(b->copy_open_ns);
                if (preparation_start) preparation_start = host_now_ns();
            }
            if (resize) ANativeWindow_setBuffersGeometry(w, (int32_t)b->w, (int32_t)b->h, WINDOW_FORMAT_RGBX_8888);
            /* Interpolated copy prepares before its physical tick, like the
               prediction path. Refuse expired work before locking: the public
               window API has no unlock-without-post operation. FIFO retains
               temporal order even if lock/copy itself stalls. */
            if (b->interpolated && host_now_ns() >= b->desired_ns) {
                ANativeWindow_release(w); pres_child_expire(b, FRAMEGEN_CHILD_DEADLINE); continue;
            }
            ANativeWindow_Buffer out;
            if (!ANativeWindow_lock(w, &out, NULL)) {
                uint32_t rw = MINV(b->w, (uint32_t)out.width), rh = MINV(b->h, (uint32_t)out.height);
                for (uint32_t y = 0; y < rh; y++) {
                    const uint8_t *src = b->ptr + (size_t)y * b->w * 4; uint8_t *dst = (uint8_t *)out.bits + (size_t)y * (size_t)out.stride * 4;
                    if (!b->swap) memcpy(dst, src, (size_t)rw * 4);
                    else for (uint32_t x = 0; x < rw; x++) { dst[4 * x] = src[4 * x + 2]; dst[4 * x + 1] = src[4 * x + 1]; dst[4 * x + 2] = src[4 * x]; dst[4 * x + 3] = 255; }
                }
                uint64_t ready_ns = preparation_start ? host_now_ns() : 0;
                if (preparation_start) preparation_ns += ready_ns - preparation_start;
                /* Predict prepares pixels before its intentional no-earlier-than
                 * post floor. Holding this locked buffer preserves ANativeWindow
                 * lifetime; legacy modes retain wait-before-lock ordering. */
                if ((b->prediction.id || b->interpolated) && b->refresh_ns) runtime_android_present_wait(b->copy_open_ns);
                uint64_t ft_post = totk_ft_on() && b->timeline_frame ? totk_ft_now() : 0;
                uint64_t post_start = b->prediction.id || b->auto_frame.id ? host_now_ns() : 0;
                if (!ANativeWindow_unlockAndPost(w)) {
                    if (post_start) pres_predict_feedback(b, PRESENT_PREDICT_COPY, ready_ns,
                        preparation_ns, host_now_ns() - post_start);
                    /* Entry to a successful post, separate from the historical ACK endpoint below. */
                    if (ft_post) totk_ft_submission(b->timeline_frame, ft_post, FT_SUBMIT_COPY_POST);
                    pres_credit_release(b);   /* successful post acknowledgement, not scanout */
                    /* Successful post acknowledgement only; this API exposes no actual display timestamp. */
                    if (totk_ft_on() && b->timeline_frame) totk_ft_compositor(b->timeline_frame, totk_ft_now(), 0);
                    vk_overlay_presented(host_now_ns(), b->generated); g_pres_shown++;
                    if (b->adaptive_epoch) atomic_fetch_add_explicit(&g_adaptive_stats.shown,1,memory_order_relaxed);
                    posted = 1;
                    if (b->generated) pres_child_handoff(); else front_frame_presented();
                }
                else if (!b->generated) g_pres_cancelled++;
            }
            else if (!b->generated) g_pres_cancelled++;
            ANativeWindow_release(w);
        }
        else if (!b->generated) g_pres_cancelled++;
        if (b->generated && !posted) {
            pres_child_expire(b, w ? FRAMEGEN_CHILD_POST : FRAMEGEN_CHILD_WINDOW); continue;
        }
        pres_credit_release(b);   /* window/lock/post failures; success was already retired above */
        pres_predict_cancel(b); /* failed window/lock/post, or no usable readiness */
        if (!b->generated) black_check(b);
        atomic_store(&b->busy, 0);
    }
    return NULL;
}
static int pres_alloc(pres_buf *b, VkDeviceSize size) {
    if (b->size >= size) return 1;
    if (b->buf) { vkDestroyBuffer(V.dev, b->buf, NULL); vk_memory_release(b->mem, VK_MEMORY_READBACK); b->buf = VK_NULL_HANDLE; b->size = 0; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, &b->buf)) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->buf, &req);
    /* the CPU reads every byte: cached memory first */
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vk_memory_allocate(&mai, &b->mem, VK_MEMORY_READBACK)) { vkDestroyBuffer(V.dev, b->buf, NULL); b->buf = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(V.dev, b->buf, b->mem, 0);
    if (vkMapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->ptr)) return 0;
    b->coherent = !!(V.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    b->size = size;
    return 1;
}
static VkImage g_pimg; static vk_alloc g_pmem; static uint32_t g_pimg_w, g_pimg_h; static VkFormat g_pimg_f;
static int pres_image(uint32_t w, uint32_t h, VkFormat f) {   /* the window-sized image the frame is scaled into */
    if (g_pimg && g_pimg_w == w && g_pimg_h == h && g_pimg_f == f) return 1;
    if (g_pimg) { vk_flush_wait(); vkDestroyImage(V.dev, g_pimg, NULL); vk_mem_free(&g_pmem); g_pimg = VK_NULL_HANDLE; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (vkCreateImage(V.dev, &ci, NULL, &g_pimg)) { RED(2, "presentation image %ux%u could not be made", w, h); g_pimg = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, g_pimg, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_pmem)) { vkDestroyImage(V.dev, g_pimg, NULL); g_pimg = VK_NULL_HANDLE; return 0; }
    vkBindImageMemory(V.dev, g_pimg, g_pmem.mem, g_pmem.off);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_pimg, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(vk_cb(), &di);
    g_pimg_w = w; g_pimg_h = h; g_pimg_f = f;
    fprintf(stderr, "[nvn-vk] window picture %ux%u\n", w, h);
    return 1;
}
static void pres_enqueue(int i) {
    pres_buf *b = &g_pres[i];
    vk_present_job_begin(&b->job);
    pthread_mutex_lock(&g_win_mu);
    if (b->refresh_ns) {
        atomic_store_explicit(&b->credited, 1, memory_order_relaxed);
        runtime_android_present_credit(1);
    }
    if (b->timeline_frame) {
        totk_ft_handoff(b->timeline_frame, totk_ft_now());
        if (b->refresh_ns) totk_ft_present_slot(b->timeline_frame, b->desired_ns, b->slot_skipped);
    }
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline) b->enqueue_ns=host_now_ns();
    pres_queue_locked(i);
    pthread_cond_signal(&g_pres_cv); pthread_mutex_unlock(&g_win_mu);
}
/* Snapshot immutable producer metadata and atomic observer timestamps only on
 * exhaustion, once per 300-real reporting group. No waits, readback or heap
 * allocation; a slot may release during the snapshot, explicitly reported. */
static void pres_slot_exhaustion(uint64_t pair_real) {
    if (!g_pres_slot_diag) return;
    static uint64_t last_group=UINT64_MAX;
    uint64_t group=g_present_n/300;
    if (group==last_group) return;
    last_group=group;
    uint64_t now=host_now_ns();
    LOGV("[framegen-slot-exhaustion] producer=%llu pair_real=%llu now_ns=%llu buffers=%d",
        (unsigned long long)g_present_n,(unsigned long long)pair_real,(unsigned long long)now,g_pres_active);
    for (int i=0;i<g_pres_active;i++) {
        const pres_buf *b=&g_pres[i];
        if (!atomic_load_explicit(&b->busy,memory_order_acquire)) continue;
        uint64_t apply=atomic_load_explicit(&b->diag_apply_ns,memory_order_acquire);
        uint64_t callback=atomic_load_explicit(&b->diag_callback_ns,memory_order_acquire);
        uint64_t wait=atomic_load_explicit(&b->diag_previous_wait_ns,memory_order_acquire);
        const char *owner=wait ? "previous-release-wait" : apply ?
            (callback ? "applied-callback" : "applied-await-callback") :
            b->enqueue_ns ? "queued-or-presenter" : "pair-held";
        LOGV("[framegen-slot-owner] slot=%d frame=%llu kind=%s owner=%s desired_ns=%llu enqueue_ns=%llu apply_enter_ns=%llu callback_ns=%llu previous_wait_ns=%llu own_ready_ns=%llu pair_ready_ns=%llu busy_now=%d",
            i,(unsigned long long)b->frame,b->generated ? "generated" : "real",owner,
            (unsigned long long)b->desired_ns,(unsigned long long)b->enqueue_ns,
            (unsigned long long)apply,(unsigned long long)callback,(unsigned long long)wait,
            (unsigned long long)vk_present_ready_ns(b->fence),
            (unsigned long long)(b->source_fence ? vk_present_ready_ns(b->source_fence) : 0),
            atomic_load_explicit(&b->busy,memory_order_acquire));
    }
}

/* Synthetic output is a child of an immutable real reservation, never another
 * guest present/acquire or a second advancement of the real display lattice. */
static void present_generated(const tex_t *t, vk_tex *x, int srgb, const pres_buf *real,
        uint32_t ow, uint32_t oh, const int crop[4], uint64_t midpoint,
        const runtime_present_timeline_entry *timeline, float phase, int prepare_slots) {
    int interpolating = vk_fg_interpolating();
    int diagnose = interpolating && FGX.diag && FGX.eligible;
    if (!interpolating) midpoint = framegen_midpoint(real->desired_ns, real->refresh_ns, real->interval, host_now_ns());
    if (!midpoint && !prepare_slots) { if (!interpolating) vk_fg_skip(); return; }
    int i = -1;
    for (int k = 0; k < g_pres_active; k++) if (&g_pres[k]!=real && !atomic_load(&g_pres[k].busy)) { i = k; break; }
    int output_ok = 1;
    if (i < 0) {
        g_fg_no_buffer++;
        if (diagnose) { g_fg_admission.no_buffer++; pres_slot_exhaustion(real->frame); }
        if (!interpolating) vk_fg_skip();
        return;
    }
    uint32_t fw = vk_tex_width(x,0), fh = vk_tex_height(x,0);
    uint32_t native_guest_w=0,native_guest_h=0;
    if (vk_native_ui_enabled()) {
        vk_post_output native;
        if (!vk_tex_native_output(x,&native)) {
            LOGV("[framegen-present] refusing native generated image: presented sibling unavailable");
            if (diagnose) g_fg_admission.source_shape++;
            return;
        }
        fw=native.w; fh=native.h;
        vk_tex_native_basis(x,&native_guest_w,&native_guest_h);
    }
    if (vk_tex_samples(x) != VK_SAMPLE_COUNT_1_BIT || vk_tex_is3d(x)) {
        if (diagnose) g_fg_admission.source_shape++;
        if (!interpolating) vk_fg_skip();
        return;
    }
    vk_end_rendering_at(VK_PASS_END_PRESENT,3);
    VkCommandBuffer cb;
    vk_post_output generated;
    vk_fg_generation *prepared=NULL;
    if (prepare_slots) {
        vk_fg_generation_stamp stamp={real->framegen_real,real->adaptive_epoch};
        prepared=vk_fg_generation_prepare((unsigned)i,fw,fh,srgb,&stamp);
        if (!prepared || !vk_fg_generation_output(prepared,&generated)) {
            if (prepared) vk_fg_generation_cancel(prepared);
            return;
        }
        cb=vk_fg_generation_prepare_cb(prepared);
    } else {
        cb=vk_cb(); vk_barrier_all(cb);
        if (!vk_fg_generate_phase(cb,(unsigned)i,fw,fh,srgb,phase,&generated)) {
            if (diagnose) g_fg_admission.generator_refused++;
            return;
        }
        cb=vk_cb(); /* Legacy external handoff sealed the entry primary. */
        if (diagnose) g_fg_admission.recorded++;
        g_fg_generated++; g_fg_pending++;
    }
    uint32_t w = native_guest_w ? native_guest_w : fw, h = native_guest_h ? native_guest_h : fh;
    int32_t cx = 0, cy = 0;
    if (crop && crop[0]>=0 && crop[1]>=0 && crop[2]>0 && crop[3]>0 &&
        (uint32_t)(crop[0]+crop[2])<=w && (uint32_t)(crop[1]+crop[3])<=h) {
        cx=crop[0]; cy=crop[1]; w=(uint32_t)crop[2]; h=(uint32_t)crop[3];
    }
    if (native_guest_w && native_guest_h) {
        vk_present_rect mapped=vk_present_native_crop((vk_present_rect){cx,cy,w,h},
            native_guest_w,native_guest_h,fw,fh);
        cx=mapped.x; cy=mapped.y; w=mapped.w; h=mapped.h;
    }
    int32_t dw, dh, dx, dy;
    if (g_pres_guest_output) {
        dw=(int32_t)real->guest_fit.w; dh=(int32_t)real->guest_fit.h; dx=dy=0;
        if (ow==real->guest_window.w && oh==real->guest_window.h) {
            dx=real->guest_fit.x; dy=real->guest_fit.y;
        }
    } else {
        vk_present_rect fit=vk_present_fit_shown(w,h,pres_shown_aspect(),ow,oh);
        dw=(int32_t)fit.w; dh=(int32_t)fit.h; dx=fit.x; dy=fit.y;
    }
    vk_post_output post = generated;
    if (vk_post_enabled()) {
        if (vk_gpuprof_on && !prepare_slots) vk_gpuprof_scope_begin(cb, "post");
        if (vk_post_run(cb,(unsigned)i,vk_fg_raw_view((unsigned)i),
                fw,fh,cx,cy,w,h,(uint32_t)dw,(uint32_t)dh,srgb,&post)) {
            cx=cy=0; w=post.w; h=post.h;
        }
        if (vk_gpuprof_on && !prepare_slots) vk_gpuprof_scope_end(cb);
    }
    pres_buf *b=&g_pres[i]; b->zero=0;
    b->rotation=real->rotation;
    b->ubwc=real->ubwc;
    if (g_pres_guest_output) { b->guest_window=real->guest_window; b->guest_fit=real->guest_fit; }
    vk_fg_output_copy_begin(cb,(unsigned)i);
#ifdef __ANDROID__
    if (pres_ahb_draw(i,t,x,srgb,ow,oh,cx,cy,w,h,dx,dy,dw,dh,&post,b->rotation,b->ubwc,prepare_slots?cb:VK_NULL_HANDLE)) {
        b->zero=1; goto generated_enqueue;
    }
#endif
    if (prepare_slots) {
        /* The legacy copy path owns a shared scratch image. It cannot be
         * referenced by a delayed immutable job across later real frames. */
        vk_fg_generation_cancel(prepared);
        return;
    }
    if (g_pres_guest_output) {
        vk_present_extent extent=real->guest_window;
        if (!g_guest_extent_cached) {
            pthread_mutex_lock(&g_win_mu);
            extent=pres_frame_window_extent(g_win,GUEST_QUERY_PRODUCER);
            pthread_mutex_unlock(&g_win_mu);
        }
        if (!extent.w || !extent.h) { output_ok=0; goto generated_enqueue; }
        ow=extent.w; oh=extent.h;
        if (g_guest_extent_cached) {
            dw=(int32_t)real->guest_fit.w; dh=(int32_t)real->guest_fit.h;
            dx=real->guest_fit.x; dy=real->guest_fit.y;
        } else {
            uint32_t sw=vk_present_shown_width(w,h,pres_shown_aspect());
            double sc=fmin(1.0,fmin((double)ow/sw,(double)oh/h));
            dw=(int32_t)(sw*sc+0.5); dh=(int32_t)(h*sc+0.5);
            dx=((int32_t)ow-dw)/2; dy=((int32_t)oh-dh)/2;
        }
    }
    if (!pres_image(ow,oh,srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM) ||
        !pres_alloc(b,(VkDeviceSize)ow*oh*4)) {
        /* Queue discard-only retirement; never block game production to clean
         * up synthetic work that could not be handed to the window. */
        output_ok=0; goto generated_enqueue;
    }
    cb=vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "present");
    VkClearColorValue black={{0,0,0,1}};
    VkImageSubresourceRange rr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cb,g_pimg,VK_IMAGE_LAYOUT_GENERAL,&black,1,&rr); vk_barrier_all(cb);
    VkImageBlit bl={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{cx,cy,0},{cx+(int32_t)w,cy+(int32_t)h,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{dx,dy,0},{dx+dw,dy+dh,1}}};
    VkFilter filter = vk_native_ui_enabled() && w == (uint32_t)dw && h == (uint32_t)dh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    vkCmdBlitImage(cb,post.image,VK_IMAGE_LAYOUT_GENERAL,g_pimg,VK_IMAGE_LAYOUT_GENERAL,1,&bl,filter);
    vk_barrier_all(cb); vk_overlay_record(cb,g_pimg,VK_IMAGE_LAYOUT_GENERAL,ow,oh,(unsigned)i,0);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={ow,oh,1}};
    vkCmdCopyImageToBuffer(cb,g_pimg,VK_IMAGE_LAYOUT_GENERAL,b->buf,1,&copy); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
generated_enqueue:
    vk_present_job_begin(&b->job); /* generated jobs bypass pres_enqueue */
    b->generation=real->generation; b->desired_ns=b->copy_open_ns=midpoint;
    b->refresh_ns=real->refresh_ns; b->interval=1; b->source_fence=real->fence;
    if (g_pres_timeline) {
        b->timeline_role=timeline && timeline->expected_ns ? RUNTIME_PRESENT_TIMELINE_GENERATED:0;
        if (b->timeline_role) b->display_timeline=*timeline;
    }
    b->source_slot=real->desired_ns; b->generated=output_ok?1:2; b->prediction=(runtime_present_prediction){0};
    b->discard=0;
    b->adaptive_epoch=real->adaptive_epoch;
    b->adaptive_slots=(unsigned)prepare_slots;
    b->adaptive_slot=(fgs_slot){0}; b->adaptive_token=0;
    b->adaptive_release_ns=0; b->adaptive_latch_kind=0;
    atomic_store_explicit(&b->adaptive_suppressed,0,memory_order_relaxed);
    b->interpolated=interpolating;
    g_pres_qcom[i]=vk_fg_qcom_picture();
    g_pres_lsfg[i]=vk_fg_lsfg_picture();
    b->frame=real->frame;
    b->prediction_done=1; b->ready_ns=0; b->timeline_frame=0;
    /* A synthetic image owns no game-frame observation or real-slot skip credit. */
    b->framegen_real=0;
    b->auto_frame=(runtime_fps_auto_frame){0}; b->auto_done=1; b->auto_cadence=0; b->slot_skipped=0;
    b->w=ow; b->h=oh; b->swap=0; atomic_store(&b->credited,0);
    pres_slot_diag_reset(b);
    if (prepare_slots) {
        if (!output_ok || !b->zero || !vk_fg_generation_prepare_finish(prepared)) {
            vk_fg_generation_cancel(prepared); return;
        }
        atomic_store_explicit(&b->busy,1,memory_order_release);
        pthread_mutex_lock(&g_win_mu);
        g_slot_prepared[i]=(pres_slot_prepared){.generation=prepared,
            .phase=g_adaptive_policy,.frame=real->frame,.epoch=real->adaptive_epoch,
            .window_generation=real->generation,.real_index=(int)(real-g_pres),.output_index=i};
        g_slot_prepared_count++;
        atomic_store_explicit(&vk_fg_slot_boundary_active,1,memory_order_relaxed);
        pthread_mutex_unlock(&g_win_mu);
        return;
    }
    vk_fg_gpu_end(vk_cb(),(unsigned)i);
    vk_fg_diag_dump((unsigned)i);
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline || g_fg_job_trace) b->record_done_ns=host_now_ns();
    b->fence=vk_submit_now();
#ifdef __ANDROID__
    if (b->zero && b->fence) g_ahb[i].foreign=1;
#endif
    atomic_store(&b->busy,1);
    /* The real image was submitted before any generated commands. Classify
       this whole ticket range before the real-frame endpoint is sealed. */
    vk_fg_synthetic_range(real->fence+1,b->fence);
    g_bound_pipe=VK_NULL_HANDLE; g_dyn_dirty=1; memset(g_bound_set[0],0,sizeof g_bound_set[0]);
    pthread_mutex_lock(&g_win_mu);
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline) b->enqueue_ns=host_now_ns();
    pres_queue_locked(i); pthread_cond_signal(&g_pres_cv);
    pthread_mutex_unlock(&g_win_mu);
}
/* Called only by the recorder owner with no open guest primary. Optional
 * dispatch never waits for publication, a free recorder slot, or the GPU. */
void vk_fg_slot_boundary(void) {
    int requested=pres_slots_requested();
    if (!requested && !g_slot_prepared_count) return;
    uint64_t now=host_now_ns();
    g_slot_dispatch_stats.boundaries++;
    if (!g_slot_dispatch_stats.first_ns) g_slot_dispatch_stats.first_ns=now;
    for (unsigned k=0;k<NPRES;k++) {
        pthread_mutex_lock(&g_win_mu);
        pres_slot_prepared *p=&g_slot_prepared[k];
        if (!p->generation || p->dispatching) { pthread_mutex_unlock(&g_win_mu); continue; }
        runtime_fps_adaptive_frame current=runtime_fps_adaptive_snapshot();
        unsigned generation_state=vk_fg_generation_state(p->generation);
        unsigned backend_reason=g_fg_job_trace ? vk_fg_generation_cancel_reason(p->generation) : FG_SLOT_CANCEL_NONE;
        fg_slot_cancel_reason reason=!requested ? FG_SLOT_CANCEL_MODE :
            p->cancelled ? FG_SLOT_CANCEL_OWNER :
            (!g_win || p->window_generation!=g_win_generation) ? FG_SLOT_CANCEL_WINDOW :
            p->epoch!=current.epoch ? FG_SLOT_CANCEL_EPOCH :
            generation_state==VK_FG_GENERATION_EMPTY ?
                (backend_reason==FG_SLOT_CANCEL_SNAPSHOT_PRESSURE ?
                    FG_SLOT_CANCEL_SNAPSHOT_PRESSURE : FG_SLOT_CANCEL_EMPTY) : FG_SLOT_CANCEL_NONE;
        int cancel=reason!=FG_SLOT_CANCEL_NONE;
        if (!cancel && !p->published) {
            g_slot_dispatch_stats.no_publication++;
            pthread_mutex_unlock(&g_win_mu); continue;
        }
        if (!cancel && !p->request.token) {
            g_slot_dispatch_stats.no_request++; cancel=1; reason=FG_SLOT_CANCEL_NO_HOLE;
        }
        if (!cancel && (fgs_pending_index(&g_slot_policy,p->request.token)<0 ||
                now>=p->request.deadline_ns)) {
            g_slot_dispatch_stats.late++; cancel=1;
            reason=fgs_pending_index(&g_slot_policy,p->request.token)<0 ?
                FG_SLOT_CANCEL_POLICY : FG_SLOT_CANCEL_DEADLINE;
        }
        runtime_present_timelines clock;
        if (!cancel && (!runtime_android_adaptive_slot_timelines(&clock) ||
                !runtime_present_adaptive_slot_clock_matches(&clock,g_slot_clock_generation,
                    g_pres[p->output_index].refresh_ns,runtime_android_adaptive_slots_display_id(),g_win_display_id))) {
            g_slot_dispatch_stats.unavailable++; cancel=1; reason=FG_SLOT_CANCEL_CLOCK;
        }
        fg_adaptive_policy phase_policy=p->phase;
        phase_policy.display_anchor_ns=p->real_target;
        float phase=0;
        if (!cancel && !fg_adaptive_phase(&phase_policy,p->request.slot.target_ns,0,&phase)) {
            g_slot_dispatch_stats.phase++; cancel=1; reason=FG_SLOT_CANCEL_PHASE;
        }
        pres_slot_prepared job=*p;
        p->dispatching=1;
        if (cancel && p->request.token) {
            fgs_cancel dropped;
            (void)fgs_cancel_gen(&g_slot_policy,p->request.token,FGS_CANCEL_OWNER,&dropped);
        }
        pthread_mutex_unlock(&g_win_mu);
        int dispatched=0;
        uint64_t ticket=0;
        if (!cancel) {
            vk_post_output output;
            g_slot_dispatch_stats.opportunities++;
            dispatched=vk_fg_generation_dispatch(job.generation,phase,job.request.deadline_ns,&output);
            if (dispatched==1) ticket=vk_fg_generation_seal(job.generation);
            if (dispatched!=1 || !ticket) {
                g_slot_dispatch_stats.gpu_refused++; reason=FG_SLOT_CANCEL_DISPATCH_REFUSED;
            }
        }
        if (!ticket) vk_fg_generation_cancel(job.generation);
        pthread_mutex_lock(&g_win_mu);
        p=&g_slot_prepared[k];
        pres_buf *b=&g_pres[job.output_index];
        if (ticket) {
#ifdef __ANDROID__
            g_ahb[job.output_index].foreign=1;
#endif
            b->fence=ticket; b->source_fence=job.real_fence;
            b->source_slot=job.real_target;
            b->desired_ns=b->copy_open_ns=job.request.slot.target_ns;
            b->adaptive_slot=job.request.slot; b->adaptive_token=job.request.token;
            b->adaptive_release_ns=0; b->adaptive_latch_kind=0;
            b->record_done_ns=host_now_ns(); b->enqueue_ns=b->record_done_ns;
            b->timeline_role=0; b->timeline_frame=0;
            g_pres_qcom[job.output_index]=1; g_pres_lsfg[job.output_index]=0;
            if (p->cancelled || !g_win || job.window_generation!=g_win_generation ||
                    fgs_pending_index(&g_slot_policy,job.request.token)<0)
                atomic_store_explicit(&b->adaptive_suppressed,1,memory_order_release);
            g_fg_generated++; g_fg_pending++; g_adaptive_stats.generated++;
            unsigned bin=(unsigned)(phase*4); if (bin>3) bin=3;
            g_adaptive_stats.phase[bin]++;
            g_slot_dispatch_stats.dispatched++;
            pres_queue_locked(job.output_index); pthread_cond_signal(&g_pres_cv);
        } else {
            fgs_cancel dropped;
            (void)fgs_cancel_gen(&g_slot_policy,job.request.token,FGS_CANCEL_OWNER,&dropped);
            if (vk_fg_generation_state(job.generation)==VK_FG_GENERATION_EMPTY)
                atomic_store_explicit(&b->busy,0,memory_order_release);
            g_slot_dispatch_stats.cancelled++;
        }
        if (g_fg_job_trace) LOGV("[adaptive-slot-dispatch] event=job frame=%llu token=%llu target_ns=%llu boundary_ns=%llu phase=%.8f ticket=%llu cancel=%d submit=%d",
            (unsigned long long)job.frame,(unsigned long long)job.request.token,
            (unsigned long long)job.request.slot.target_ns,(unsigned long long)now,
            phase,(unsigned long long)ticket,cancel,dispatched);
        if (g_fg_job_trace) {
            vk_fg_snapshot_pressure_counts pressure=vk_fg_snapshot_pressure_get();
            char line[640];
            fg_slot_job_trace_format(line,sizeof line,(fg_slot_job_trace){
                .frame=job.frame,.token=job.request.token,.target=job.request.slot.target_ns,
                .boundary=now,.reason=reason,.generation_state=generation_state,
                .backend_reason=backend_reason,.pressure_observations=pressure.observations,
                .pressure_cancellations=pressure.cancellations,.pressure_appends=pressure.appends});
            LOGV("%s",line);
        }
        *p=(pres_slot_prepared){0};
        g_slot_prepared_count--;
        atomic_store_explicit(&vk_fg_slot_boundary_active,g_slot_prepared_count!=0,memory_order_relaxed);
        pthread_mutex_unlock(&g_win_mu);
        g_bound_pipe=VK_NULL_HANDLE; g_dyn_dirty=1; memset(g_bound_set[0],0,sizeof g_bound_set[0]);
    }
    uint64_t elapsed=host_now_ns()-g_slot_dispatch_stats.first_ns;
    if (elapsed>=UINT64_C(1000000000)) {
        LOGV("[adaptive-slot-dispatch] window_ns=%llu boundaries=%llu opportunities=%llu dispatched=%llu cancelled=%llu no-publication=%llu no-hole=%llu late-boundary=%llu phase-refused=%llu clock-refused=%llu gpu-refused=%llu on-time-gen-applies=%llu late-gen-applies=%llu template-refused=%llu irrevocable-real-conflicts=%llu (app handoffs, not SF slot hits)",
            (unsigned long long)elapsed,(unsigned long long)g_slot_dispatch_stats.boundaries,
            (unsigned long long)g_slot_dispatch_stats.opportunities,(unsigned long long)g_slot_dispatch_stats.dispatched,
            (unsigned long long)g_slot_dispatch_stats.cancelled,(unsigned long long)g_slot_dispatch_stats.no_publication,
            (unsigned long long)g_slot_dispatch_stats.no_request,(unsigned long long)g_slot_dispatch_stats.late,
            (unsigned long long)g_slot_dispatch_stats.phase,(unsigned long long)g_slot_dispatch_stats.unavailable,
            (unsigned long long)g_slot_dispatch_stats.gpu_refused,
            (unsigned long long)atomic_exchange_explicit(&g_slot_on_time_applies,0,memory_order_relaxed),
            (unsigned long long)atomic_exchange_explicit(&g_slot_late_applies,0,memory_order_relaxed),
            (unsigned long long)atomic_exchange_explicit(&g_slot_template_refused,0,memory_order_relaxed),
            (unsigned long long)atomic_exchange_explicit(&g_slot_irrevocable_real_conflicts,0,memory_order_relaxed));
        memset(&g_slot_dispatch_stats,0,sizeof g_slot_dispatch_stats);
    }
}
static void present_window(const tex_t *t, double period, const int crop[4]) {
    uint64_t adaptive_lease=0;
    unsigned slot_state=FG_ADAPTIVE_WARMUP;
    runtime_fps_adaptive_frame slot_stamp={0};
    runtime_fps_auto_frame auto_frame = {0};
    runtime_android_fps_auto_take((uintptr_t)t, &auto_frame);
    int auto_cadence = runtime_android_fps_auto_active(); /* never query frontend under g_mu */
    runtime_present_prediction prediction = {0};
    int predict = runtime_android_predict_active();
    unsigned expected_path = PRESENT_PREDICT_COPY;
#ifdef __ANDROID__
    if (V.ahb_present && !atomic_load(&g_ahb_off) && !atomic_load(&g_ahb_surface_off))
        expected_path = PRESENT_PREDICT_AHB;
#endif
    /* Consume by exact guest NVNtexture identity even when the frame will drop.
     * Recording may subsequently discover that this expected AHB path is copy. */
    runtime_android_predict_take((uintptr_t)t, expected_path, &prediction);
    if (predict && !prediction.id) {
        static uint64_t missing_plan; missing_plan++; g_pres_dropped++;
        if (missing_plan <= 8 || missing_plan % 300 == 0)
            LOGV("window predict: missing acquire plan for texture=%p; display copy dropped total=%llu (no independent reservation/fallback)",
                 (const void *)t, (unsigned long long)missing_plan);
        goto cancel;
    }
    g_overlay_enabled = vk_overlay_mode(); /* Per present (follows the live Minus+R toggle); draws read only this copy. */
    static int started;
    if (!started) {
        const char *e = getenv("TOTK_PRESENT_BUFFERS");
        int scene_fsr=vk_scene_scale_enabled() && vk_scene_scale_fsr();
        g_pres_active = (int)vk_present_slots_buffers(vk_fg_interpolating(),
            vk_fg_locked() ? vk_fg_lead_refreshes() : 0,
            vk_present_buffer_count(e, getenv("TOTK_ACQUIRE_SLACK")),scene_fsr);
        if (g_pres_active != VK_PRESENT_SLOT_DEFAULT_BUFFERS)
            LOGV("window present: %d presenter buffers (default %d; guest texture count unchanged)",
                 g_pres_active, VK_PRESENT_SLOT_DEFAULT_BUFFERS);
        if (vk_fg_interpolating())
            LOGV("[framegen-slots] buffers=%d real-pair=%d AHB-current/pending=%d queued-child=%d new-child=%d FSR-scene-phase=%d; sources=4; lazy bounded allocations, GPU and compositor release both required",
                g_pres_active, VK_PRESENT_INTERP_REALS, VK_PRESENT_INTERP_AHB,
                VK_PRESENT_INTERP_QUEUED_CHILD, VK_PRESENT_INTERP_NEW_CHILD,
                scene_fsr ? VK_PRESENT_INTERP_FSR_PHASE : 0);
        e=getenv("TOTK_PRESENT_CADENCE_TRACE"); g_pres_trace=e && !strcmp(e,"1");
        e=getenv("TOTK_FRAMEGEN_JOB_TRACE"); g_fg_job_trace=e && !strcmp(e,"1");
#ifdef __ANDROID__
        g_pres_async_retire=vk_present_retire_selected(getenv("TOTK_FG_ASYNC_RETIRE"));
#endif
        g_pres_floor_ns=vk_fg_locked() ? vk_present_floor_config(getenv("TOTK_PRESENT_FLOOR_NS")) : 0;
        g_pres_guard_ns=vk_fg_locked() ? vk_present_guard_config(getenv("TOTK_PRESENT_GUARD_NS")) : 0;
        e=getenv("TOTK_PRESENT_DEDUPE");
        g_pres_distinct_refreshes=vk_fg_locked() && e && !strcmp(e,"1");
        LOGV("[present-floor] ns=%u guard_ns=%u dedupe=%d; locked AHB eligibility only; API metadata unchanged",
            g_pres_floor_ns,g_pres_guard_ns,g_pres_distinct_refreshes);
        e=getenv("TOTK_APP_WIDTH");
        pthread_mutex_lock(&g_win_mu); /* one-time selection, shared with lifecycle callbacks */
        g_pres_guest_output=!vk_native_ui_enabled() && e && !strcmp(e,"guest");
        pthread_mutex_unlock(&g_win_mu);
        e=getenv("TOTK_FRAMEGEN_DIAG");
        g_pres_slot_diag=vk_fg_interpolating() && e && !strcmp(e,"1");
        g_guest_extent_diag=e && !strcmp(e,"1");
        e=getenv("TOTK_GUEST_EXTENT_CACHE"); g_guest_extent_cached=!(e && !strcmp(e,"0"));
#ifdef __ANDROID__
        if (vk_fg_locked() && runtime_android_present_timeline_active()) {
            g_pres_set_timeline=(void (*)(ASurfaceTransaction *,int64_t))
                dlsym(RTLD_DEFAULT,"ASurfaceTransaction_setFrameTimeline");
            g_pres_timeline=g_pres_set_timeline!=NULL;
            LOGV("[present-timeline] presenter=%s; locked AHB pairs only; lead=%u; existing buffer/flight bounds retained",
                g_pres_timeline?"admitted":"unavailable (legacy)",vk_fg_lead_refreshes());
        }
#endif
        if (vk_fg_locked()) LOGV("[framegen-pacing] locked%s: at most two future transactions with runtime backpressure guard, lead=%u physical refreshes; intentional image latency about%.3f ms at60Hz; %d bounded slots; defaults/other modes unchanged",
            vk_fg_qcom_active() ? " qcom extrapolation" : "",
            vk_fg_lead_refreshes(),(vk_fg_qcom_active() ? 0 : 1000.0/30.0)+vk_fg_lead_refreshes()*(1000.0/60.0),g_pres_active);
        pthread_t th; if (pthread_create(&th, NULL, presenter, NULL)) { RED(2, "window presenter thread could not start"); goto cancel; }
        pthread_detach(th); started = 1;
    }
    int have = 0; uint32_t pw = 0, ph = 0; uint64_t generation = 0;
    unsigned rotation = 0;
    int ubwc = 0;
    vk_present_rect guest_fit = {0};
    if (!g_pres_guest_output || !g_guest_extent_cached) {
        pthread_mutex_lock(&g_win_mu);
        have = g_win != NULL; pw = g_win_pw; ph = g_win_ph; generation = g_win_generation;
        int turns=vk_present_transform_turns(g_win_transform_hint);
        if (g_win_prerotate && turns>=0) rotation=(unsigned)turns;
        ubwc=g_win_ubwc && turns>0;
        if (g_pres_guest_output) {
            vk_present_extent extent=pres_frame_window_extent(g_win,GUEST_QUERY_PRODUCER); pw=extent.w; ph=extent.h;
        }
        pthread_mutex_unlock(&g_win_mu);
        if (!have || !t) goto cancel;
    } else {
        vk_present_extent extent = pres_guest_window();
        if (!t || !extent.w || !extent.h) goto cancel;
    }
    int i = -1; for (int k = 0; k < g_pres_active; k++) if (!atomic_load(&g_pres[k].busy)) { i = k; break; }
    if (i < 0) { g_pres_dropped++; goto cancel; }
    vk_tex *x = vk_tex_get(t); if (!x) goto cancel;
    VkFormat f = vk_tex_format(x);
    int srgb = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    uint32_t w = vk_tex_width(x, 0), h = vk_tex_height(x, 0); int32_t cx = 0, cy = 0;
    vk_pso_life_external_read(w, h, "presenter-read");
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= w && (uint32_t)(crop[1] + crop[3]) <= h) { cx = crop[0]; cy = crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3]; }
    if (g_pres_guest_output && g_guest_extent_cached) {
        pthread_mutex_lock(&g_win_mu);
        have = g_win != NULL; generation = g_win_generation;
        int turns=vk_present_transform_turns(g_win_transform_hint);
        if (g_win_prerotate && turns>=0) rotation=(unsigned)turns;
        ubwc=g_win_ubwc && turns>0;
        vk_present_extent extent = pres_guest_window(); pw = extent.w; ph = extent.h;
        guest_fit = pres_guest_fit_cached(w, h);
        pthread_mutex_unlock(&g_win_mu);
        if (!have || !guest_fit.w || !guest_fit.h) goto cancel;
    }
    else if (g_pres_guest_output) {
        guest_fit=vk_present_guest_fit(vk_present_shown_width(w,h,pres_shown_aspect()),h,pw,ph);
        if (!guest_fit.w || !guest_fit.h) goto cancel;
    }
    /* Relaxed telemetry only: never query resources, clocks or GPU state here.
       Without a scene MRT (menus/video), the valid game crop is the best known size. */
    vk_overlay_set_resolution(g_overlay_scene_w ? g_overlay_scene_w : w,
                              g_overlay_scene_h ? g_overlay_scene_h : h, w, h);
    /* Both front-buffer paths consume the native display sibling directly. */
    static uint32_t want_w;
    if (!want_w) {
        const char *e = getenv("TOTK_APP_WIDTH");
        want_w = vk_native_ui_enabled() ? VK_NATIVE_UI_WIDTH : e && atoi(e) >= 16 ? (uint32_t)atoi(e) : 1280;
        if (vk_native_ui_enabled()) {
            if (e && *e && strcmp(e, "1920"))
                LOGV("native UI v2: ignoring TOTK_APP_WIDTH=%s (720p guest canvas; window-sized host UI)", e);
            LOGV("native UI v2: 1280x720 game canvas; native UI begins at first UI draw; window-sized front buffers");
        }
    }
    vk_present_extent output = vk_present_output_size(vk_native_ui_enabled(), want_w, vk_post_scale(), pw, ph);
    if (g_pres_guest_output) {
        if (!guest_fit.w || !guest_fit.h) goto cancel;
        /* Copy fallback paints bars into a full window-sized buffer. AHB can
         * retain only the fitted guest pixels and place that surface centrally. */
        output=(vk_present_extent){pw,ph};
#ifdef __ANDROID__
        if (V.ahb_present && !atomic_load(&g_ahb_off) && !atomic_load(&g_ahb_surface_off))
            output=(vk_present_extent){guest_fit.w,guest_fit.h};
#endif
    }
    uint32_t ow = output.w, oh = output.h;
    if (vk_fg_interpolating()) {
        static uint32_t last_w, last_h;
        if (ow != last_w || oh != last_h) {
            LOGV("[framegen-slots] presenter RGBA8 payload=%llu bytes (%d x %ux%u x 4); excludes AHB stride/alignment, source/output/post images; actual allocations in image-memory report",
                (unsigned long long)((uint64_t)g_pres_active * ow * oh * 4), g_pres_active, ow, oh);
            last_w = ow; last_h = oh;
        }
    }
    int32_t dw, dh, dx, dy;
    if (g_pres_guest_output) {
        dw=(int32_t)guest_fit.w; dh=(int32_t)guest_fit.h; dx=dy=0;
        if (ow==pw && oh==ph) { dx=guest_fit.x; dy=guest_fit.y; }
    } else {
        vk_present_rect fit = vk_present_fit_shown(w, h, pres_shown_aspect(), ow, oh);
        dw = (int32_t)fit.w; dh = (int32_t)fit.h; dx = fit.x; dy = fit.y;
    }
    pres_buf *b = &g_pres[i]; b->zero = 0;
    b->rotation=rotation;
    b->ubwc=ubwc;
    if (g_pres_guest_output) { b->guest_window=(vk_present_extent){pw,ph}; b->guest_fit=guest_fit; }
    vk_post_output post, *post_result = NULL;
    if (vk_native_ui_enabled()) {
        vk_end_rendering_at(VK_PASS_END_PRESENT, 3);
        if (vk_tex_native_output(x, &post)) {
            uint32_t gw,gh; vk_tex_native_basis(x,&gw,&gh);
            vk_present_rect mapped=vk_present_native_crop((vk_present_rect){cx,cy,w,h},
                gw,gh,post.w,post.h);
            cx=mapped.x; cy=mapped.y; w=mapped.w; h=mapped.h; post_result=&post;
        }
    }
    if (vk_post_enabled()) {
        /* Sample raw display-encoded RGB; cached texture views retain the image's native channel order. */
        int raw_format = (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) ? 0x9a : 0x25;
        view_t view = {.set_levels = 1, .nlevels = 1, .set_layers = 1, .nlayers = 1, .set_target = 1, .target = 1,
            .set_swizzle = 1, .swz = {2,3,4,5}, .set_format = 1, .format = raw_format};
        if (vk_tex_is3d(x) || vk_tex_samples(x) != VK_SAMPLE_COUNT_1_BIT ||
            (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB &&
             f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_B8G8R8A8_SRGB &&
             f != VK_FORMAT_A8B8G8R8_UNORM_PACK32 && f != VK_FORMAT_A8B8G8R8_SRGB_PACK32))
            DIE("post: unsupported final game image format %d", f);
        VkImageView source = post_result ? vk_tex_native_view(x,&view,0) : vk_tex_view(x,t,&view,0);
        if (!source) DIE("post: final game image cannot be sampled");
        if (vk_lag_uniform_private()) vk_ub_touch((uint64_t)(uintptr_t)t->mem,t->L.size); else vk_ub_flush();
        vk_end_rendering_at(VK_PASS_END_PRESENT,3);
        VkCommandBuffer post_cb = vk_cb(); int scope = vk_native_gpu_begin(post_cb,VK_NUI_POST); vk_barrier_all(post_cb);
        if (vk_post_run(post_cb,(unsigned)i,source,post_result ? post.w : vk_tex_width(x,0),post_result ? post.h : vk_tex_height(x,0),cx,cy,w,h,
                        (uint32_t)dw,(uint32_t)dh,srgb,&post)) {
            post_result = &post; cx = cy = 0; w = post.w; h = post.h;
            g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; memset(g_bound_set[0],0,sizeof g_bound_set[0]);
        }
        vk_native_gpu_end(post_cb,scope);
    }
#ifdef __ANDROID__
    if (pres_ahb_draw(i, t, x, srgb, ow, oh, cx, cy, w, h, dx, dy, dw, dh, post_result, rotation, ubwc,VK_NULL_HANDLE)) { b->zero = 1; goto enqueue; }
#endif
    if (g_pres_guest_output && (ow!=pw || oh!=ph)) {
        ow=pw; oh=ph;
        dw=(int32_t)guest_fit.w; dh=(int32_t)guest_fit.h;
        dx=guest_fit.x; dy=guest_fit.y;
    }
    VkFormat of = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!pres_image(ow, oh, of)) goto cancel;
    if (!pres_alloc(b, (VkDeviceSize)ow * oh * 4)) { RED(2, "presentation buffer (%ux%u) could not be made", ow, oh); goto cancel; }
    if (vk_lag_uniform_private()) vk_ub_touch((uint64_t)(uintptr_t)t->mem, t->L.size); else vk_ub_flush();
    vk_end_rendering_at(VK_PASS_END_PRESENT, 2);
    VkCommandBuffer cb = vk_cb(); int scope = vk_native_gpu_begin(cb,VK_NUI_PRESENT_COPY); vk_barrier_all(cb);
    if (dw < (int32_t)ow || dh < (int32_t)oh) {
        VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); vk_barrier_all(cb);
    }
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{cx, cy, 0}, {cx + (int32_t)w, cy + (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    VkFilter filter = vk_native_ui_enabled() && w == (uint32_t)dw && h == (uint32_t)dh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    vkCmdBlitImage(cb, post_result ? post.image : vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_pimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, filter);
    vk_barrier_all(cb);
    vk_overlay_record(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, ow, oh, (unsigned)i, 0);
    VkBufferImageCopy r = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {ow, oh, 1}};
    vkCmdCopyImageToBuffer(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, b->buf, 1, &r);
    vk_barrier_all(cb);
    vk_native_gpu_end(cb,scope);
#ifdef __ANDROID__
enqueue:
#endif
    b->generation = generation; b->desired_ns = host_now_ns();
    b->refresh_ns = b->copy_open_ns = b->slot_skipped = 0; b->interval = 0;
    b->prediction = prediction; b->prediction_done = 0; b->ready_ns = 0;
    b->auto_frame = auto_frame; b->auto_done = 0; b->auto_cadence = auto_cadence;
    b->generated = b->interpolated = 0; b->source_fence = b->source_slot = 0;
    if (g_pres_timeline) {
        b->timeline_role=0; b->timeline_display=-1;
#ifdef __ANDROID__
        pthread_mutex_lock(&g_win_mu);
        if (generation==g_win_generation) b->timeline_display=g_win_display_id;
        pthread_mutex_unlock(&g_win_mu);
#endif
    }
    g_pres_qcom[i]=0;
    g_pres_lsfg[i]=0;
    b->discard=0;
    b->adaptive_epoch=0;
    b->adaptive_slots=(unsigned)pres_slots_requested(); b->adaptive_eligible=0;
    b->adaptive_real_conflicts=0;
    b->adaptive_slot=(fgs_slot){0}; b->adaptive_token=0;
    b->adaptive_release_ns=0; b->adaptive_latch_kind=0;
    atomic_store_explicit(&b->adaptive_suppressed,0,memory_order_relaxed);
    b->framegen_real=vk_fg_real_frame_id();
    b->w = ow; b->h = oh; b->swap = 0; atomic_store(&b->busy, 1);
    { uint64_t skipped = 0; for (int k = 0; k < 16; k++) skipped += g_stat_skips[k];
      b->ctr[0] = g_stat_draws; b->ctr[1] = g_stat_passes; b->ctr[2] = skipped; vk_res_counters(b->ctr + 3); b->frame = g_present_n; }
    pres_slot_diag_reset(b);
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline) b->record_done_ns=host_now_ns();
    int lsfg_probe=vk_fg_lsfg_probe_requested();
    if (lsfg_probe) vk_fg_lsfg_submitting(vk_cb());
    if (b->adaptive_slots) {
        runtime_present_timelines clock;
        slot_stamp=runtime_fps_adaptive_snapshot();
        b->adaptive_epoch=slot_stamp.epoch;
        b->adaptive_eligible=(unsigned)slot_stamp.eligible;
        if (g_fg_job_trace) {
            static uint64_t tr_epoch; static int tr_eligible, tr_have;
            if (!tr_have || tr_epoch!=slot_stamp.epoch || tr_eligible!=slot_stamp.eligible)
                LOGV("[adaptive-scene] event=stamp reason=%s frame=%llu now_ns=%llu epoch=%llu prev_epoch=%llu eligible=%d anchor_ns=%llu sim_ns=%llu",
                    !tr_have?"first":tr_epoch!=slot_stamp.epoch?"epoch":"eligibility",
                    (unsigned long long)b->frame,(unsigned long long)host_now_ns(),
                    (unsigned long long)slot_stamp.epoch,(unsigned long long)tr_epoch,slot_stamp.eligible,
                    (unsigned long long)slot_stamp.anchor_ns,(unsigned long long)slot_stamp.sim_ns);
            tr_have=1; tr_epoch=slot_stamp.epoch; tr_eligible=slot_stamp.eligible;
        }
        int clock_ok=runtime_android_adaptive_slots_requested() &&
            runtime_android_adaptive_slot_timelines(&clock);
        pthread_mutex_lock(&g_win_mu);
        clock_ok=clock_ok && generation==g_win_generation &&
            runtime_present_timeline_display_allowed(runtime_android_adaptive_slots_display_id(),g_win_display_id);
        pthread_mutex_unlock(&g_win_mu);
        slot_state=fg_adaptive_observe(&g_adaptive_policy,
            (fg_adaptive_stamp){slot_stamp.sim_ns,slot_stamp.anchor_ns,slot_stamp.epoch,slot_stamp.eligible},
            clock_ok?clock.refresh_ns:0);
        if (clock_ok && slot_state==FG_ADAPTIVE_ACTIVE && vk_fg_qcom_active() && vk_fg_enabled()) {
            b->refresh_ns=clock.refresh_ns; b->interval=1;
            present_generated(t,x,srgb,b,ow,oh,crop,0,NULL,0,1);
        }
    } else if (vk_fg_adaptive()) adaptive_lease=vk_replay_lease_begin();
    b->fence = vk_submit_now();
#ifdef __ANDROID__
    if (b->zero && b->fence) g_ahb[i].foreign=1;
#endif
    if (lsfg_probe) vk_fg_lsfg_submitted(b->fence); /* observation only; frame-end retirement still owns reuse */
    if (prediction.id) {
        /* The acquire plan owns this reservation. Never independently reserve
         * or advance the legacy lattice, and never adjust its immutable S/D. */
        b->desired_ns = b->copy_open_ns = prediction.slot_ns;
        b->refresh_ns = prediction.refresh_ns; b->interval = prediction.interval;
    } else if (!predict) pres_slot_reserve(b, period);
    { static unsigned last_interval; static int have;
      if (!have || last_interval != b->interval)
          LOGV("[fps-policy] frame %llu presenter effective interval %u -> %u; requested %.3f ms refresh %.3f ms path=%s plan=%llu (0=legacy; immutable queued slot, no guest writer)",
               (unsigned long long)b->frame, last_interval, b->interval, period * 1e3, b->refresh_ns / 1e6,
               b->zero ? "AHB" : "copy", (unsigned long long)prediction.id);
      have = 1; last_interval = b->interval; }
    pres_auto_reservation(b);
    b->timeline_frame = totk_ft_on() ? totk_ft_current() : 0;
    /* Ordinary/camera settings take the original retained-pair path. QCOM's
       direct real release is selected only after app-context admission. */
    int qcom=vk_fg_qcom_picture(); /* immutable engine of this already-composed picture */
    if (vk_fg_adaptive()) {
        runtime_fps_adaptive_frame stamp=b->adaptive_slots?slot_stamp:runtime_fps_adaptive_snapshot();
        b->adaptive_epoch=stamp.epoch;
        /* The simulated image at t_last is displayed at this real slot; map
         * its simulation clock there, not to GPU/render completion time. */
        unsigned state=b->adaptive_slots?slot_state:fg_adaptive_observe(&g_adaptive_policy,
            (fg_adaptive_stamp){stamp.sim_ns,stamp.anchor_ns,stamp.epoch,stamp.eligible},
            b->refresh_ns);
        g_adaptive_policy.display_anchor_ns=b->desired_ns;
        unsigned planned=fg_adaptive_plan_outputs(&g_adaptive_plan,stamp.anchor_ns,b->refresh_ns,state);
        g_adaptive_stats.refused[state]++;
        g_adaptive_stats.real++;
        if (!g_adaptive_stats.first_ns) g_adaptive_stats.first_ns=stamp.anchor_ns;
        g_adaptive_stats.last_ns=stamp.anchor_ns;
        pres_enqueue(i);
        if (adaptive_lease && planned && state==FG_ADAPTIVE_ACTIVE && vk_fg_qcom_active() && vk_fg_enabled()) {
            uint64_t target=fg_adaptive_first_slot(b->desired_ns,b->refresh_ns,host_now_ns());
            for (unsigned n=0;n<planned && target;n++) {
                float phase;
                if (!fg_adaptive_phase(&g_adaptive_policy,target,0,&phase)) {
                    g_adaptive_stats.refused[FG_ADAPTIVE_HORIZON]++; break;
                }
                uint64_t before=atomic_load_explicit(&g_fg_generated,memory_order_relaxed);
                present_generated(t,x,srgb,b,ow,oh,crop,target,NULL,phase,0);
                if (atomic_load_explicit(&g_fg_generated,memory_order_relaxed)!=before) {
                    unsigned bin=(unsigned)(phase*4); if (bin>3) bin=3;
                    g_adaptive_stats.phase[bin]++; g_adaptive_stats.generated++;
                } else g_adaptive_stats.generator_refused++;
                if (target>UINT64_MAX-b->refresh_ns) break;
                target+=b->refresh_ns;
            }
        }
        if (g_adaptive_stats.real==300) {
            double seconds=(g_adaptive_stats.last_ns-g_adaptive_stats.first_ns)/1e9;
            LOGV("[adaptive] last300-real fps=%.3f generated=%llu fraction=%.4f phase=[%llu,%llu,%llu,%llu] states warmup=%llu active=%llu passthrough=%llu scene=%llu floor=%llu stale=%llu clock=%llu horizon=%llu shown=%llu missed=%llu real-won=%llu generator-refused=%llu (app slots, not SF scanout)",
                seconds>0?299/seconds:0,(unsigned long long)g_adaptive_stats.generated,
                (double)g_adaptive_stats.generated/(300+g_adaptive_stats.generated),
                (unsigned long long)g_adaptive_stats.phase[0],(unsigned long long)g_adaptive_stats.phase[1],
                (unsigned long long)g_adaptive_stats.phase[2],(unsigned long long)g_adaptive_stats.phase[3],
                (unsigned long long)g_adaptive_stats.refused[0],(unsigned long long)g_adaptive_stats.refused[1],
                (unsigned long long)g_adaptive_stats.refused[2],(unsigned long long)g_adaptive_stats.refused[3],
                (unsigned long long)g_adaptive_stats.refused[4],(unsigned long long)g_adaptive_stats.refused[5],
                (unsigned long long)g_adaptive_stats.refused[6],(unsigned long long)g_adaptive_stats.refused[7],
                (unsigned long long)atomic_exchange(&g_adaptive_stats.shown,0),
                (unsigned long long)atomic_exchange(&g_adaptive_stats.missed,0),
                (unsigned long long)atomic_exchange(&g_adaptive_stats.suppressed,0),
                (unsigned long long)g_adaptive_stats.generator_refused);
            g_adaptive_stats.real=g_adaptive_stats.first_ns=g_adaptive_stats.last_ns=g_adaptive_stats.generated=0;
            g_adaptive_stats.generator_refused=0;
            memset(g_adaptive_stats.phase,0,sizeof g_adaptive_stats.phase);
            memset(g_adaptive_stats.refused,0,sizeof g_adaptive_stats.refused);
        }
        if (adaptive_lease) (void)vk_replay_lease_end(adaptive_lease,0);
        return;
    }
    int interpolate=vk_fg_interpolating() &&
        (!runtime_fps_hybrid_selected() || runtime_fps_hybrid_interpolating());
    if (qcom) interpolate=0;
    if (interpolate) {
        vk_present_slot slot = {.desired_ns = b->desired_ns, .copy_open_ns = b->copy_open_ns,
            .refresh_ns = b->refresh_ns, .skipped = b->slot_skipped, .interval = b->interval,
            .mode = b->refresh_ns ? VK_PRESENT_SLOTS_ACTIVE : VK_PRESENT_SLOTS_NO_CLOCK};
        vk_present_interp_pair pair = vk_present_interp_push(&g_pres_interp,i,b->frame,generation,slot,host_now_ns());
        if (vk_fg_locked()) pair=vk_present_interp_buffer(pair,vk_fg_lead_refreshes(),host_now_ns());
        runtime_present_timeline_entry real_timeline,child_timeline;
        if (g_pres_timeline) {
            real_timeline=(runtime_present_timeline_entry){0};
            child_timeline=(runtime_present_timeline_entry){0};
            pres_timeline_pair(&pair,b,&real_timeline,&child_timeline);
        }
        if (pair.reason == VK_INTERP_READY) g_fg_ready++;
        if (FGX.diag && FGX.eligible) g_fg_admission.pair[pair.reason]++;
        if (pair.real >= 0) {
            pres_buf *older = &g_pres[pair.real];
            older->desired_ns = pair.slot.desired_ns; older->copy_open_ns = pair.slot.copy_open_ns;
            older->refresh_ns = pair.slot.refresh_ns; older->interval = pair.slot.interval;
            older->slot_skipped = pair.slot.skipped; older->source_fence = b->fence;
            if (g_pres_timeline) {
                older->timeline_role=real_timeline.expected_ns ? RUNTIME_PRESENT_TIMELINE_REAL:0;
                if (older->timeline_role) older->display_timeline=real_timeline;
            }
            pres_enqueue(pair.real);
            if (pair.midpoint_ns && vk_fg_enabled()) present_generated(t,x,srgb,b,ow,oh,crop,pair.midpoint_ns,g_pres_timeline?&child_timeline:NULL,0.5f,0);
        }
    } else {
        int retained=vk_present_interp_leave(&g_pres_interp);
        if (retained>=0) {
            /* No compositor ownership yet. FIFO waits the covering GPU fence
             * before clearing busy; queued children retain their deadlines. */
            g_pres[retained].discard=1; pres_enqueue(retained);
            LOGV("[hybrid-present] native return: retiring retained real; reserved past queued child; latency step removes one unshown real");
        }
        if (qcom) {
            vk_present_interp_pair direct={.real=i,.reason=VK_INTERP_READY,
                .slot={.desired_ns=b->desired_ns,.copy_open_ns=b->copy_open_ns,
                    .refresh_ns=b->refresh_ns,.interval=b->interval,
                    .mode=b->refresh_ns ? VK_PRESENT_SLOTS_ACTIVE : VK_PRESENT_SLOTS_NO_CLOCK}};
            direct=vk_present_interp_buffer(direct,vk_fg_lead_refreshes(),host_now_ns());
            b->desired_ns=direct.slot.desired_ns; b->copy_open_ns=direct.slot.copy_open_ns;
            pres_enqueue(i);
            if (b->interval==2 && direct.midpoint_ns && vk_fg_enabled())
                present_generated(t,x,srgb,b,ow,oh,crop,direct.midpoint_ns,NULL,0.5f,0);
            return;
        }
        pres_enqueue(i);
        if (vk_fg_enabled() && !vk_fg_interpolating()) present_generated(t,x,srgb,b,ow,oh,crop,0,NULL,0.5f,0);
    }

    return;
cancel:
    if (adaptive_lease) (void)vk_replay_lease_end(adaptive_lease,0);
    if (vk_fg_adaptive()) fg_adaptive_present_cancel();
    runtime_android_predict_cancel(&prediction);
}
#endif
#ifndef TOTK_ANDROID_APP
_Atomic int vk_fg_slot_boundary_active; /* no app presenter: disabled unless a TEST fixture owns work */
#ifdef TOTK_VK_TESTS
static void (*fg_test_slot_boundary_callback)(void *);
static void *fg_test_slot_boundary_arg;
void vk_fg_test_slot_boundary_callback(void (*callback)(void *),void *arg) {
    fg_test_slot_boundary_callback=callback; fg_test_slot_boundary_arg=arg;
}
void vk_fg_slot_boundary(void) {
    if (fg_test_slot_boundary_callback)
        fg_test_slot_boundary_callback(fg_test_slot_boundary_arg);
}
#else
void vk_fg_slot_boundary(void) {}
#endif
#endif /* !TOTK_ANDROID_APP */
/* The recording owner invokes this on every bypassed real observation. Source
 * history and simulation policy must restart together; frame-end still owns
 * GPU/output retirement and budget accounting. The host fixture uses this same
 * helper even when the app-only presenter is not compiled. */
static void fg_adaptive_present_cancel(void) {
#ifdef TOTK_ANDROID_APP
    pthread_mutex_lock(&g_win_mu);
    g_adaptive_policy=(fg_adaptive_policy){0};
    if (pres_slots_requested()) pres_slots_end_locked();
    fg_adaptive_plan_reset(&g_adaptive_plan);
    if (g_adaptive_presenting>=0)
        atomic_store_explicit(&g_pres[g_adaptive_presenting].adaptive_suppressed,1,memory_order_release);
#endif
    runtime_fps_adaptive_reset();
    vk_fg_skip();
#ifdef TOTK_ANDROID_APP
    pres_adaptive_wake_locked();
    pthread_mutex_unlock(&g_win_mu);
#endif
}
#ifdef TOTK_VK_TESTS
void vk_fg_test_present_cancel(void) { fg_adaptive_present_cancel(); }
#endif
/* Upstream hitch attribution, opt-in because per-frame median sorting and cross-thread atomics are not free.
   Drain every present, not only on hitches: detail lines describe one frame rather than everything since the last hitch. */
static void hitch_present(uint64_t now, uint64_t frame) {
    if (!g_hitch_on) return;
    typedef struct {
#define HK_VALUE(name) uint64_t name;
        VK_HITCH_FIELDS(HK_VALUE)
#undef HK_VALUE
    } hitch_values;
    hitch_values v;
    static hitch_values total;
#define HK_TAKE(name) v.name = atomic_exchange_explicit(&g_hk.name, 0, memory_order_relaxed); total.name += v.name;
    VK_HITCH_FIELDS(HK_TAKE)
#undef HK_TAKE
    static uint64_t last, recent[64], bins[5], worst, count, sum;
    static unsigned nr, next, logged;
    uint64_t dt = last ? now - last : 0; last = now;
    if (!dt) return;
    recent[next] = dt; next = (next + 1) % 64; if (nr < 64) nr++;
    uint64_t sorted[64]; memcpy(sorted, recent, nr * sizeof *sorted);
    for (unsigned i = 1; i < nr; i++) for (unsigned j = i; j && sorted[j - 1] > sorted[j]; j--) {
        uint64_t t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
    }
    double median = (nr & 1 ? sorted[nr / 2] : sorted[nr / 2 - 1] / 2 + sorted[nr / 2] / 2) / 1e6;
    static const uint64_t thresholds[] = {40000000, 60000000, 100000000, 200000000, 400000000};
    for (int i = 4; i >= 0; i--) if (dt >= thresholds[i]) { bins[i]++; break; }
    if (dt > worst) worst = dt;
    count++; sum += dt;
    if (dt >= 40000000 && dt / 1e6 >= 2.5 * median && logged < 24) {
        logged++;
        LOGV("hitch at frame %llu: +%.1f ms (median %.1f): replay %.1f ms in %llu submits (GPU wait %.1f ms); pipelines %llu (%.1f ms, %llu sync-built, %llu failed, %llu retry, %llu bounded, %llu skipped draws); shader loads %llu (%.0f KiB), variant lists %llu, requests %llu; descriptor pools %llu, chunks %llu, cpu steps %llu; gpu idles %llu (%.1f ms, %llu finishes, %llu flush waits, %llu pool drops); invalidates %llu (%.1f ms, %llu writebacks, %.1f MiB); game waits %llu (%.1f ms); completion %.1f ms, wake %.1f ms",
             (unsigned long long)frame, dt / 1e6, median, v.replay_ns / 1e6, (unsigned long long)v.subs, v.wait_ns / 1e6,
             (unsigned long long)v.pipes, v.pipe_ns / 1e6, (unsigned long long)v.pipes_sync, (unsigned long long)v.pipes_fail,
             (unsigned long long)v.pipe_retries, (unsigned long long)v.pipe_bounds, (unsigned long long)v.pipe_skips,
             (unsigned long long)v.shloads, v.shbytes / 1024.0, (unsigned long long)v.vlists, (unsigned long long)v.sreq,
             (unsigned long long)v.dpools, (unsigned long long)v.chunks, (unsigned long long)v.cpu_steps,
             (unsigned long long)v.idles, v.idle_ns / 1e6, (unsigned long long)v.idle_fin, (unsigned long long)v.idle_flush, (unsigned long long)v.idle_pool,
             (unsigned long long)v.inval, v.inval_ns / 1e6, (unsigned long long)v.wb, v.wb_bytes / 1048576.0,
             (unsigned long long)v.gwaits, v.gwait_ns / 1e6, v.comp_ns / 1e6, v.wake_ns / 1e6);
    }
    if (frame % 300 == 0) {
        LOGV("hitches, last %llu frames: 40-60 ms %llu, 60-100 %llu, 100-200 %llu, 200-400 %llu, >=400 %llu; worst %.1f ms, median %.1f ms, mean %.1f ms; pipelines %llu failed, %llu retries, %llu bounded, %llu skipped draws%s",
             (unsigned long long)count, (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
             (unsigned long long)bins[3], (unsigned long long)bins[4], worst / 1e6, median, sum / (double)count / 1e6,
             (unsigned long long)total.pipes_fail, (unsigned long long)total.pipe_retries, (unsigned long long)total.pipe_bounds,
             (unsigned long long)total.pipe_skips, logged == 24 ? " (detail lines capped at 24)" : "");
        memset(&total, 0, sizeof total); memset(bins, 0, sizeof bins); worst = count = sum = 0; logged = 0;
    }
}
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
#ifndef TOTK_ANDROID_APP
    (void)period;
#endif
    uint64_t now = host_now_ns(), n = ++g_present_n;
    hitch_present(now, n);
    if (vk_lag_capture_on()) nvn_dirty_lag_frame(n + 1);
    probe_frame_end(n);
    pd_frame_end(n);
    if (g_fr_n) {
        qsort(g_fr, (size_t)g_fr_n, sizeof *g_fr, u32cmp);
        if (g_fr_n != g_prev_n || memcmp(g_fr, g_prev, (size_t)g_fr_n * sizeof *g_fr)) {
            uint64_t c = ++g_stat_rescg;
            if (g_prev_n && (c <= 64 || c % 100 == 0)) { char a[300], b[300]; sizes_str(g_prev, g_prev_n, a, sizeof a); sizes_str(g_fr, g_fr_n, b, sizeof b);
                LOGV("depth target sizes at frame %llu: %s (frame before: %s; %llu changes so far)", (unsigned long long)n, b, a, (unsigned long long)c); }
            memcpy(g_prev, g_fr, sizeof g_fr); g_prev_n = g_fr_n;
        }
    }
    g_fr_n = 0;
    g_pres_t[n % 64] = now / 1e9;
    if (!g_present_t0) g_present_t0 = now;
    if (g_last_present_ns) { uint64_t dt = now - g_last_present_ns; if (dt > g_win_frame_max_ns) g_win_frame_max_ns = dt; g_win_over33 += dt > 33333333; g_win_over50 += dt > 50000000; }
    { /* a frame over 100 ms: where its time went in the backend; the rest is the game's own work */
      static uint64_t prev[5], prev_pipe, prev_cp, prev_io; static int nlog;
      uint64_t c[5], pipe = g_cum_pipe_ns, cp = vk_compute_pipe_cum_ns(), io = vk_shader_io_cum_ns(); vk_perf_cum(c);
      if (g_last_present_ns && now - g_last_present_ns > 100000000 && nlog++ < 200)
          LOGV("long frame %llu: %.0f ms since the last present; recording %.1f ms (pipelines made at the draw %.1f, compute pipelines %.1f, seed files %.1f), "
               "GPU waits while recording %.1f ms, the game's sync waits %.1f ms (%llu), %llu CPU steps", (unsigned long long)n, (now - g_last_present_ns) / 1e6,
               (c[0] - prev[0]) / 1e6, (pipe - prev_pipe) / 1e6, (cp - prev_cp) / 1e6, (io - prev_io) / 1e6, (c[1] - prev[1]) / 1e6, (c[2] - prev[2]) / 1e6,
               (unsigned long long)(c[4] - prev[4]), (unsigned long long)(c[3] - prev[3]));
      vk_pipeline_keys_pressure(now, c[0]-prev[0] > 25000000 || c[1]-prev[1] > 4000000);
      memcpy(prev, c, sizeof prev); prev_pipe = pipe; prev_cp = cp; prev_io = io; }
    totk_perf_render(g_last_present_ns ? now - g_last_present_ns : 0);
    vk_pipeline_keys_pressure(now, g_last_present_ns && now-g_last_present_ns > 40000000);
    g_last_present_ns = now;
    static long every = -1; if (every < 0) { const char *e = getenv("TOTK_VK_SHOT"); every = e ? atol(e) : 0; if (every) LOGV("saving every %ld-th frame as PNG (TOTK_VK_SHOT)", every); }
    vk_rec_lock();
    static uint64_t pcache_draws;
    vk_pcache_present(now, g_stat_draws - pcache_draws); pcache_draws = g_stat_draws;
    if (pipe_predict_on()) {
        nvn_scan_budget_frame(&g_pp_budget, ++g_pp_frame);
        g_pp_scans_in_sub = 0; g_pp_submit = g_pp_pass = 0; g_pp_more = 0;
    }
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_begin(VK_REC_O_PRESENT);
    if (every > 0 && t && n % (uint64_t)every == 0) shot(t, n, crop);
    pp_present(n); pd_present(n);
    if (vk_canvas_on && t) {
        vk_canvas_image window=vk_tex_canvas_identity(vk_tex_get(t),0,0);
        vk_canvas_present(&window);
    }
    if (vk_gpuprof_on) vk_end_rendering_at(VK_PASS_END_PRESENT, 0);
#ifdef TOTK_ANDROID_APP
    int fg_requested=fg_extract_requested_config();
    vk_fg_present_begin(t);
    vk_overlay_target(period);
    present_window(t, period, crop);
    pres_extent_report(n);
    g_overlay_scene_w = g_overlay_scene_h = 0;
    if (fg_requested) {
        /* Include every source copy/read even when the window job was dropped.
         * A synthetic output never becomes a separate guest frame. */
        vk_fg_present_end(vk_submit_now());
    }
#endif
    /* The real-frame endpoint follows our post, window copy and synthetic GPU
     * recordings, including their slot submissions, rather than the game pass. */
    if (vk_gpuprof_on) vk_gpuprof_present();
    vk_native_frame_end();
    if (vk_canvas_on) vk_canvas_frame_end();
    if (__builtin_expect(vk_rec_profile_on, 0)) { vk_rec_profile_end(); vk_rec_profile_present(n); }
    vk_record_stats(n);
    xlate_draw_frame(n);
    vk_xlate_recording_stats_frame(n);
    if (n % 300 == 0 && vk_record_enabled()) {
        uint64_t hits, misses; vk_prog_record_cache_take(&hits, &misses);
        LOGV("recorder memo per frame: program hit/miss %.1f/%.1f, binding freeze hit/miss %.1f/%.1f",
            hits / 300.0, misses / 300.0, g_record_freeze_hits / 300.0, g_record_freeze_misses / 300.0);
        g_record_freeze_hits = g_record_freeze_misses = 0;
    }
    vk_rec_unlock();
    totk_pgo_frame(n);
    if (n % 300 == 0) {
        vk_rec_lock(); vk_occlusion_stats(n); gpu_work_stats(n); native_gpu_stats(n);
        LOGV("[load-elide] frame %llu: last 300 presents %llu proven, %llu elided passes",
             (unsigned long long)n, (unsigned long long)g_load_proven, (unsigned long long)g_load_elided);
        g_load_proven = g_load_elided = 0;
        LOGV("[feedback-narrow] frame %llu: last 300 presents %llu deferred, %llu avoided pass ends, %llu retained late hazards",
             (unsigned long long)n,(unsigned long long)g_feedback_deferred,
             (unsigned long long)g_feedback_avoided,(unsigned long long)g_feedback_hazards);
        g_feedback_deferred=g_feedback_avoided=g_feedback_hazards=0;
        LOGV("[alpha-test] frame %llu: last 300 presents enabled=%llu ignored=%llu",
             (unsigned long long)n,(unsigned long long)g_alpha_enabled,(unsigned long long)g_alpha_ignored);
        g_alpha_enabled=g_alpha_ignored=0;
        LOGV("[clear-on-read] frame %llu: last 300 presents forced=%llu reason=first-read",
             (unsigned long long)n,(unsigned long long)g_clear_read_forced);
        g_clear_read_forced=0;
#ifdef TOTK_ANDROID_APP
        if (g_guest_extent_diag && vk_overlay_mode()) {
            static vk_overlay_counters previous;
            vk_overlay_counters current=vk_overlay_get_counters(NULL);
            LOGV("[overlay] frame=%llu layout-rebuilds=%llu text-field-updates=%llu graph-appends=%llu (last 300 real; glyph fields <=1Hz; no per-frame layout/format/resampling)",
                (unsigned long long)n,(unsigned long long)(current.layout_rebuilds-previous.layout_rebuilds),
                (unsigned long long)(current.text_field_updates-previous.text_field_updates),
                (unsigned long long)(current.graph_appends-previous.graph_appends));
            previous=current;
        }
#endif
        if (pass_merge_on()) {
            static uint64_t previous[4]; uint64_t c[4]; vk_pass_merge_counters(c);
            LOGV("[pass-merge] frame %llu: deferred %.2f retained %.2f barriers/frame; late hazard splits %.2f/frame; avoided %.2f pass boundaries/frame (not a GMEM traffic counter)",
                 (unsigned long long)n, (c[0] - previous[0]) / 300.0, (c[1] - previous[1]) / 300.0,
                 (c[2] - previous[2]) / 300.0, (c[3] - previous[3]) / 300.0);
            memcpy(previous, c, sizeof previous);
        }
        if (split_pass_draws() || split_pass_live()) {
            static uint64_t previous[5];
            uint64_t c[5] = {g_split_count, g_split_reopen, g_split_refused, g_split_touches, g_split_lrz_refused};
            LOGV("[split-pass] frame %llu: last 300 presents %llu splits, %llu extra passes (reopened parts), %llu refused splits, %llu depth touches, %llu LRZ refusals; per frame %.2f/%.2f/%.2f (TOTK_VK_SPLIT_PASS_DRAWS=%u TOTK_VK_SPLIT_LRZ_SAFE=%d)",
                 (unsigned long long)n, (unsigned long long)(c[0] - previous[0]), (unsigned long long)(c[1] - previous[1]),
                 (unsigned long long)(c[2] - previous[2]), (unsigned long long)(c[3] - previous[3]),
                 (unsigned long long)(c[4] - previous[4]), (c[0] - previous[0]) / 300.0, (c[1] - previous[1]) / 300.0,
                 (c[2] - previous[2]) / 300.0, split_pass_live(), split_lrz_safe());
            memcpy(previous, c, sizeof previous);
        }
        if (atomic_load_explicit(&g_feedback_cut, memory_order_relaxed) || g_feedback_cut_launch) {
            static uint64_t previous[2]; uint64_t c[2] = {g_feedback_cuts, g_feedback_uncut};
            LOGV("[feedback-cut] frame %llu: last 300 presents %llu sampled views cut below an attached level, %llu left whole (based at it)",
                 (unsigned long long)n, (unsigned long long)(c[0] - previous[0]), (unsigned long long)(c[1] - previous[1]));
            memcpy(previous, c, sizeof previous);
        }
        if (g_prepare_mask_launch || prepare_mask_live() || g_prepare_check) {
            static uint64_t previous[3]; uint64_t c[3] = {g_prepare_calls, g_prepare_skips, g_prepare_checks};
            LOGV("[prepare-mask] frame %llu: last 300 presents %llu uniform preparations, %llu skipped, %llu oracle checks (TOTK_VK_PREPARE_MASK=%d)",
                 (unsigned long long)n, (unsigned long long)(c[0] - previous[0]), (unsigned long long)(c[1] - previous[1]),
                 (unsigned long long)(c[2] - previous[2]), prepare_mask_live());
            memcpy(previous, c, sizeof previous);
        }
        { static uint64_t previous[5]; static int shown; uint64_t c[5]; vk_tex_memo_counts(c);
          int on = vk_tex_memo_on();
          if (on || shown) {
              uint64_t hits = c[0] - previous[0], misses = c[1] - previous[1];
              LOGV("[tex-memo] frame %llu: per frame %.1f hits %.1f misses (%.1f%% hit), %.1f retirements; registrations %.1f, %.1f unchanged (kept every revision) (TOTK_VK_TEX_MEMO=%d)",
                   (unsigned long long)n, hits / 300.0, misses / 300.0, hits + misses ? 100.0 * hits / (hits + misses) : 0.0,
                   (c[2] - previous[2]) / 300.0, (c[3] - previous[3]) / 300.0, (c[4] - previous[4]) / 300.0, on);
              shown = on;
          }
          memcpy(previous, c, sizeof previous); }
        vk_rec_unlock();
        LOGV("frame %llu: %.1f fps over the last 300, %llu draws, %llu passes, %llu pipelines, %llu dispatches; skipped: no program %llu, disabled %llu, culled %llu, primitive %llu, shader missing %llu, scissor %llu, pipeline %llu, pipeline compiling %llu",
             (unsigned long long)n, 300e9 / (double)(now - g_present_t0), (unsigned long long)g_stat_draws, (unsigned long long)g_stat_passes, (unsigned long long)g_stat_pipes,
             (unsigned long long)g_stat_dispatch, (unsigned long long)g_stat_skips[0], (unsigned long long)g_stat_skips[1], (unsigned long long)g_stat_skips[2],
             (unsigned long long)g_stat_skips[3], (unsigned long long)g_stat_skips[4], (unsigned long long)g_stat_skips[6], (unsigned long long)g_stat_skips[7],
             (unsigned long long)g_stat_skips[10]);
        LOGV("frame %llu, stutter: longest frame %.1f ms, %llu frames over 33 ms, %llu over 50 ms; %llu new pipelines took %.1f ms (longest %.1f ms); %llu compiled on worker threads in %.1f ms; sync compiles %llu in %.1f ms; prewarm waits %llu in %.1f ms",
             (unsigned long long)n, g_win_frame_max_ns / 1e6, (unsigned long long)g_win_over33, (unsigned long long)g_win_over50,
             (unsigned long long)g_win_pipes, g_win_pipe_ns / 1e6, g_win_pipe_max_ns / 1e6, (unsigned long long)atomic_exchange(&g_win_async, 0), atomic_exchange(&g_win_async_ns, 0) / 1e6,
             (unsigned long long)g_win_sync, g_win_sync_ns / 1e6, (unsigned long long)g_win_prewarm_wait, g_win_prewarm_wait_ns / 1e6);
        LOGV("frame %llu, pipeline joins: %llu waits on worker builds (graphics and compute) in %.1f ms; boosts %llu, refused %llu (TOTK_VK_PIPE_BOOST=%d)",
             (unsigned long long)n, (unsigned long long)atomic_exchange(&g_boost_joins, 0), atomic_exchange(&g_boost_join_ns, 0) / 1e6,
             (unsigned long long)atomic_exchange(&g_boost_raised, 0), (unsigned long long)atomic_exchange(&g_boost_refused, 0), pipe_boost_on());
        if (g_unwritten_mask || unwritten_mask_live())
            LOGV("frame %llu, unwritten colour attachments: %llu new pipelines masked %llu attachments their fragment shader does not output (TOTK_VK_UNWRITTEN_MASK=%d)",
                 (unsigned long long)n, (unsigned long long)atomic_exchange(&g_unwritten_pipes, 0), (unsigned long long)atomic_exchange(&g_unwritten_attachments, 0),
                 unwritten_mask_live());
        if (V.has_pipeline_library) {
            static uint64_t previous[7], previous_parts[PL_COUNT], previous_ns[PL_COUNT];
            uint64_t current[7] = {atomic_load(&g_library_stats.fast_links), atomic_load(&g_library_stats.fast_link_ns),
                atomic_load(&g_library_stats.fast_draws), atomic_load(&g_library_stats.optimized_replacements),
                atomic_load(&g_library_stats.optimized_ns), atomic_load(&g_library_stats.library_skips),
                atomic_exchange(&g_library_stats.fast_link_window_max_ns, 0)};
            LOGV("[pipeline-library] fast links %llu in %.3f ms (max %.3f ms), fast draws %llu, optimized replacements %llu in %.3f ms, misses still skipped %llu",
                (unsigned long long)(current[0] - previous[0]), (current[1] - previous[1]) / 1e6, current[6] / 1e6,
                (unsigned long long)(current[2] - previous[2]), (unsigned long long)(current[3] - previous[3]),
                (current[4] - previous[4]) / 1e6, (unsigned long long)(current[5] - previous[5]));
            static const char *names[PL_COUNT] = {"vertex-input", "pre-raster", "fragment", "output"};
            for (int i = 0; i < PL_COUNT; i++) {
                uint64_t count = atomic_load(&g_library_stats.parts[i]), ns = atomic_load(&g_library_stats.part_ns[i]);
                LOGV("[pipeline-library] %s parts %llu in %.3f ms", names[i],
                    (unsigned long long)(count - previous_parts[i]), (ns - previous_ns[i]) / 1e6);
                previous_parts[i] = count; previous_ns[i] = ns;
            }
            memcpy(previous, current, sizeof current);
        }
        g_win_pipes = g_win_pipe_ns = g_win_pipe_max_ns = g_win_frame_max_ns = g_win_over33 = g_win_over50 = 0;
        g_win_sync = g_win_sync_ns = g_win_prewarm_wait = g_win_prewarm_wait_ns = 0;
        { uint64_t fn, fns, fmax; vk_shader_io_take(&fn, &fns, &fmax);
          LOGV("frame %llu, shader seed files read: %llu in %.1f ms (longest %.1f ms)", (unsigned long long)n, (unsigned long long)fn, fns / 1e6, fmax / 1e6); }
        LOGV("frame %llu, diagnostics: %llu passes split by barriers; missing textures %llu, buffers outside the pools %llu",
             (unsigned long long)n, (unsigned long long)g_stat_inpass_barriers, (unsigned long long)g_stat_dummy_tex, (unsigned long long)g_stat_zero_buf);
        double rp, wt, gp, cpu, elapsed; uint64_t ns;
        vk_rec_lock(); vk_perf_take(&rp, &wt, &gp, &cpu, &elapsed, &ns); vk_rec_unlock();
        LOGV("frame %llu, per frame: recording %.1f ms, waiting for the GPU %.1f ms, GPU %.1f ms, %.1f submissions; descriptor sets %llu written, %llu reused",
             (unsigned long long)n, rp / 300, wt / 300, gp / 300, ns / 300.0, (unsigned long long)g_stat_sets_new, (unsigned long long)g_stat_sets_reused);
        static uint64_t previous_draws;
        uint64_t draws = g_stat_draws - previous_draws; previous_draws = g_stat_draws;
        LOGV("walker cpu frame %llu: cpu %.3f ms/frame, wall %.3f ms/frame, draws %.3f/frame, cpu %.3f us/draw, offcpu %.3f ms/frame",
             (unsigned long long)n, cpu / 300, elapsed / 300, draws / 300.0,
             draws ? cpu * 1000 / (double)draws : 0, (elapsed - cpu) / 300);
        vk_upload_report(n);
        g_stat_sets_new = g_stat_sets_reused = 0;
        vk_lookup_stats(n);
        vk_lag_stats(n);
        { void vk_mem_report(void); vk_mem_report(); }
        nvn_dirty_mp_report(n);
#ifdef TOTK_ANDROID_APP
        LOGV("window: %llu frames shown, %llu not shown (all buffers busy)", (unsigned long long)g_pres_shown, (unsigned long long)g_pres_dropped);
        LOGV("window: %llu discarded (window changed or present failed)",
             (unsigned long long)g_pres_cancelled);
        if (runtime_fps_hybrid_selected())
            LOGV("[hybrid-display] last 300 real: mode=%s shown-total=%llu; framework-fence interval bins ms [<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]=%llu/%llu/%llu/%llu/%llu/%llu missing=%llu; AHB only, includes real+synthetic, asynchronous boundaries, NOT scanout",
                runtime_fps_hybrid_interpolating()?"real30+interp":"native60",(unsigned long long)g_pres_shown,
                (unsigned long long)atomic_exchange(&g_hybrid_bins[0],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[1],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[2],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[3],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[4],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[5],0),
                (unsigned long long)atomic_exchange(&g_hybrid_missing,0));
        if (fg_requested) {
            LOGV("[framegen-display] last 300 real frames: ready=%llu generated=%llu completion-accepted=%llu handed-off=%llu expired=%llu no-buffer=%llu pending=%llu; generated=recorded outputs, handoff is NOT scanout; async window boundaries",
                (unsigned long long)atomic_exchange(&g_fg_ready,0),
                (unsigned long long)atomic_exchange(&g_fg_generated,0),
                (unsigned long long)atomic_exchange(&g_fg_accepted,0),
                (unsigned long long)atomic_exchange(&g_fg_handoffs,0),
                (unsigned long long)atomic_exchange(&g_fg_expired,0),
                (unsigned long long)atomic_exchange(&g_fg_no_buffer,0),
                (unsigned long long)atomic_load(&g_fg_pending));
            LOGV("[framegen-expiry] source-missing=%llu source-late=%llu child-deadline=%llu completion-refused=%llu output=%llu window=%llu post=%llu",
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_SOURCE_MISSING],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_SOURCE_LATE],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_DEADLINE],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_COMPLETION],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_OUTPUT],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_WINDOW],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_POST],0));
        }
        if (fg_requested && FGX.diag) {
            LOGV("[framegen-admission] eligible-pairs ready=%llu no-midpoint(warmup/adjacency/clock/interval/refresh/late/slot)=%llu/%llu/%llu/%llu/%llu/%llu/%llu no-buffer=%llu source-shape=%llu generator-refused=%llu recorded=%llu",
                (unsigned long long)g_fg_admission.pair[VK_INTERP_READY],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_WARMUP],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_ADJACENCY],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_CLOCK],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_INTERVAL],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_REFRESH],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_LATE],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_SLOT],
                (unsigned long long)g_fg_admission.no_buffer,(unsigned long long)g_fg_admission.source_shape,
                (unsigned long long)g_fg_admission.generator_refused,(unsigned long long)g_fg_admission.recorded);
            memset(&g_fg_admission,0,sizeof g_fg_admission);
        }
#endif
        g_present_t0 = now;
    }
    /* The ticket is the fence of the work this present follows (the frame's submissions and the window copy):
       acquiring this swapchain texture again waits for it (mtl_present_wait), as on the Mac. Without that the game ran
       several frames ahead of the GPU and rewrote per-frame data (lights, constants) the GPU had not read yet: the lit
       scene went dark for a frame now and then. */
    pso_life_present();
    g_last_ticket = mtl_last_fence() ? mtl_last_fence() : 1;
    return g_last_ticket;
}
/* TOTK_VK_FRAMES_AHEAD=1: acquiring any swapchain texture waits for the latest present (the game at most one frame
   ahead of the GPU, as on the Switch where the GPU is rarely behind); default 2 (the texture's own last present). */
void mtl_present_wait(uint64_t tk) {
    static int off = -1, ahead = 2; if (off < 0) { off = vk_off("present_wait"); const char *e = getenv("TOTK_VK_FRAMES_AHEAD"); if (e && atoi(e) == 1) ahead = 1; }
    if (off) return;
    if (ahead == 1 && g_last_ticket > tk) tk = g_last_ticket;
    if (tk) mtl_wait(tk, 1000000000ull);
}
double mtl_presented_time(uint64_t n) { return n && n + 64 > g_present_n ? g_pres_t[n % 64] : 0; }

uint64_t vk_present_count(void) { return g_present_n; }   /* tests */

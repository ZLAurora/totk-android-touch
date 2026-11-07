/* Camera-only extrapolation or one-real-frame-delayed interpolation. No guest
   memory is read at presentation time. Source history and UI captures stay pinned
   until both their real ticket and every generated presentation have retired. */
#include "vk_framegen.h"
#include "../../runtime/fps_hybrid_runtime.h"
#include "../../runtime/fps_adaptive_runtime.h"
#include "../../runtime/present_clock.h"
#include "framegen_math.h"
#include "framegen_budget.h"
#include "vk_framegen_spv.h"
#include "vk_framegen_qcom_spv.h"
#include "vk_present_slots.h"
#include "vk_framegen_qcom.h"
#include "vk_framegen_lsfg.h"
#include "framegen_budget_external.h"
#include "vk_overlay.h"
#include "framegen_drs.h"
#include "../../runtime/drs_framegen.h"
#include "framegen_budget_lsfg.h"
#include <math.h>
#include "framegen_qcom_policy.h"
#include "framegen_job_trace.h"
#include "framegen_dispatch_trace.h"
#include "framegen_adaptive_defaults.h"
#include <sys/stat.h>

/* One deliberately blocking diagnostic capture, never an admission policy.
   Select the first generated real-frame ID at/after the requested positive ID. */
static int fg_dump_captured;
static uint64_t fg_dump_frame(void) {
    static uint64_t selected;
    static int initialized;
    if (!initialized) {
        initialized=1;
        const char *diag=getenv("TOTK_FRAMEGEN_DIAG"), *value=getenv("TOTK_FRAMEGEN_DUMP");
        if (diag && !strcmp(diag,"1") && value && *value && *value!='-') {
            char *end;
            unsigned long long n=strtoull(value,&end,10);
            if (!*end && n) selected=n;
        }
    }
    return selected;
}

unsigned vk_fg_camera_layout(const char *key) {
    /* Exact observed translator variants; no guessed layout for other shaders.
       Both matrices and the optional bias are revalidated per context. */
    if (!strcmp(key,"3e9fe6db781f37eb22f1acb0bcd73731cb8ffadaddd45bedc5aeb8ebb353fa18-468e580cd620")) return 2;
    if (!strcmp(key,"de3ac20c93c60cf782e8c55a40b89688aead03de7d67373bd4cd90c1963a95d9-466ef310f9bb") ||
        !strcmp(key,"c5b26871948ae177db2518a0204b6f718d09b9079ebb70ff865a7b5aa8688b56-1a4d008ae5b4")) return 1;
    return 0;
}
int vk_fg_raster_depth_shader(const char *key) {
    /* Audited FragDepth def-use: only gl_FragCoord.z, 0 or 1 reaches the
       export. Texture samples choose/discard branches, not numeric depth. */
    return !strcmp(key,"c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a");
}
int vk_fg_plane_shader(const char *key, unsigned stage) {
    /* e87 supplies view-space position; ecbe projects its ray onto world Y=4.
       Identity nominates the pair only; camera/coefficient/shrink proof is per draw. */
    if (stage==0) return !strcmp(key,"e87dbf67974d71e68f4ec4586d5c70ec6fccaf7515d54280c3a765f8b7cc8c79-b7530f698e76");
    if (stage==2) return vk_fg_plane_shader(key,0) ||
        !strcmp(key,"6ec214111aa5b8bc691f63fd0214aa5d4ed08e6d433f7973c9535f4a0d00f620-b7530f698e76");
    return stage==1 && (!strcmp(key,"ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9") ||
        !strcmp(key,"ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-b7530f698e76"));
}

/* Heavy retained images need four sources; presenter physical output capacity
   is independent (real/synthetic queue headroom must not multiply history RAM). */
enum {
    FG_SOURCES = 4, FG_OUTPUTS = VK_PRESENT_SLOT_CAPACITY, FG_AUTHORITY_WORDS = 16,
    FG_AUTHORITY_SPANS = 256, FG_AUTHORITY_SPAN_QUERIES = 4*FG_AUTHORITY_SPANS
};
enum {
    FG_AUTH_FAIL_DISABLED, FG_AUTH_FAIL_INIT, FG_AUTH_FAIL_INVALID,
    FG_AUTH_FAIL_CURRENT, FG_AUTH_FAIL_ATTEMPTED, FG_AUTH_FAIL_SUBRESOURCE,
    FG_AUTH_FAIL_LIMITS, FG_AUTH_FAIL_PIPELINE, FG_AUTH_FAIL_SOURCE,
    FG_AUTH_FAIL_BUFFERS, FG_AUTH_FAIL_POOL, FG_AUTH_FAIL_POOL_RESET,
    FG_AUTH_FAIL_SET, FG_AUTH_FAIL_INJECTED, FG_AUTH_FAILURES
};
/* DIAG-only attribution. The first eight source stages reuse existing budget
   brackets; the remaining stages are nested subspans, never an elapsed sum. */
enum {
    FG_PROFILE_PRESERVE, FG_PROFILE_WARP, FG_PROFILE_REPLAY, FG_PROFILE_UI_DEPTH,
    FG_PROFILE_AUTHORITY_INIT, FG_PROFILE_COMPARE, FG_PROFILE_REAL_COPY,
    FG_PROFILE_NATIVE_SCENE_COPY, FG_PROFILE_BASE,
    FG_PROFILE_SCENE_COPY=FG_PROFILE_BASE, FG_PROFILE_DEPTH_COPY,
    FG_PROFILE_FINAL_DEPTH_COPY, FG_PROFILE_FINAL_COMPARE,
    FG_PROFILE_FIRST_BOUND, FG_PROFILE_FIRST_X, FG_PROFILE_FIRST_Y, FG_PROFILE_FIRST_READBACK,
    FG_PROFILE_SECOND_BOUND, FG_PROFILE_SECOND_X, FG_PROFILE_SECOND_Y, FG_PROFILE_SECOND_READBACK,
    FG_PROFILE_INTERP_UI, FG_PROFILE_UI_BLACK, FG_PROFILE_UI_WHITE, FG_PROFILE_TERMINAL,
    FG_PROFILE_SOURCE_STAGES,
    FG_PROFILE_OUTPUT_RENDER=FG_PROFILE_SOURCE_STAGES, FG_PROFILE_OUTPUT_POST,
    FG_PROFILE_OUTPUT_COPY, FG_PROFILE_STAGES,
    FG_PROFILE_SOURCE_QUERIES=(FG_PROFILE_SOURCE_STAGES-FG_PROFILE_BASE)*2,
    FG_PROFILE_OUTPUT_QUERIES=(FG_PROFILE_STAGES-FG_PROFILE_SOURCE_STAGES)*2
};
typedef struct {
    VkImage image;
    VkImageView raw, display;
    vk_alloc memory;
    uint32_t w, h;
    VkFormat format;
    VkImageAspectFlags aspect;
    int initialized; /* left SHADER_READ_ONLY by a copy or a lean placeholder */
} fg_image;
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mapped;
    int coherent;
} fg_buffer;
typedef struct {
    fg_buffer bytes;
    VkDescriptorPool pool;
    VkDescriptorSet sets[2];
    VkDescriptorSetLayout layouts[2];
    VkDeviceSize used;
    uint64_t frame;
    unsigned begun,failed,ready,writes,descriptors[5],copies;
    vk_fg_terminal_input input,refusal; /* DIAG-only, never admission authority */
    struct {
        VkBuffer buffer;
        VkDeviceSize offset,bytes,owned_offset;
        uint64_t serial;
    } copy[VK_FG_TERMINAL_DESCRIPTORS];
} fg_terminal_owned;
typedef struct {
    fg_image scene, depth, canvas, final, real, fallback, ui_depth, ui_depth_white, ui_black, ui_white;
    fg_image scene_warp, scene_scaled, scene_scratch;
    /* Final D32 transfer words become immutable unknown-row prefixes plus a
       complete valid-depth min/max certificate. Pair masks are NEVER written
       into an old source: its previous output can still be reading on the CPU. */
    fg_buffer authority, authority_readback;
    /* Ordered before, last-after, last-proven VALUE and observed/state planes.
       All interpolation spans share this source-private allocation and set. */
    fg_buffer spans;
    VkDescriptorPool span_pool;
    VkDescriptorSet span_set;
    unsigned span_begins, span_ends, experimental_spans;
    uint32_t span_rect[4];
    int span_open, span_failed, span_experimental;
    fg_buffer pair_authority[2], pair_readback;
    VkImage authority_image;
    unsigned authority_level, authority_layer;
    uint32_t authority_w, authority_h;
    int authority_snapshot, authority_compared, real_ready, native_scene_ready;
    /* TOTK_FG_ADAPTIVE_LEAN capture: depth, real and (unless a generating host
       TEST bridge samples it) scene were NOT copied for this source. Their
       images stay allocated and SHADER_READ_ONLY so descriptors bind valid views. */
    int lean, real_skipped;
    _Atomic int authority_reported;
    float camera[84], transform[16], next_transform[16];
    uint64_t retire;
    uint64_t captured_real_frame;
    uint64_t capture_epoch,capture_ticket_predicted;
    VkCommandBuffer capture_cb;
    float captured_viewport[4];
    int captured_scissor[4];
    int captured_geometry_valid;
    _Atomic int referenced;
    vk_record_draw terminal;
    VkDescriptorSet terminal_sets[2];
    uint32_t terminal_w, terminal_h;
    VkFormat terminal_format;
    int terminal_ready;
    fg_terminal_owned owned;
    int predicted, timed, prepared, transferred, replay_depth, ui_ready;
    int generation_template; /* one private replay lease/template per captured real */
    int pair_source, depth_range_changed;
    uint32_t profile_mask;
    _Atomic int profile_reported;
} fg_source;
typedef struct {
    fg_image image;
    _Atomic(VkImageView) raw;
    _Atomic int busy, ended;
    unsigned source, previous;
    uint32_t profile_mask;
    int profile_copy_started;
} fg_output;
struct vk_fg_generation {
    VkCommandPool pool;
    VkCommandBuffer cb;
    _Atomic int state;
    _Atomic int canceled;
    _Atomic unsigned cancel_reason;
    int immutable_terminal;
    uint64_t lease,ticket,first_ticket,charge_frame;
    vk_fg_generation_stamp stamp;
    unsigned slot,source,previous;
    vk_post_output output;
    uint32_t profile_mask;
    int profile_copy_started;
    double external_ms; /* fg_stats_mu; NAN until exact GL/native-fence harvest */
    fg_dispatch_trace trace; /* JOB_TRACE only; never policy or GPU authority */
#ifdef TOTK_VK_TESTS
    VkSemaphore test_bridge;
    float test_phase;
#endif
};
static vk_fg_generation fg_generation[FG_OUTPUTS];
_Atomic int vk_fg_generation_snapshot_retained;
_Atomic int vk_fg_terminal_owned_active;
static struct {
    _Atomic uint64_t observations,cancellations,appends,pending,quarantined;
} fg_snapshot_pressure;
static uint64_t fg_generation_charge_frame; /* one optional dispatch/accounting interval */
#ifdef TOTK_VK_TESTS
static int fg_generation_test_bridge, fg_generation_test_refused;
#else
enum { fg_generation_test_bridge=0 };
#endif
/* Scheduler engine admission: the AFME bridge, or the host TEST bridge (which
 * may stand in for an adaptive QCOM refusal: real-only, no camera fallback). */
static int fg_generation_engine_ready(void) {
#ifdef TOTK_VK_TESTS
    if (fg_generation_test_bridge) return !fg_generation_test_refused;
#endif
    return vk_fg_qcom_can_generate();
}
static void fg_generation_cancel_all(void);
static _Atomic int fg_config = -1, fg_uncertified_config = -1, fg_suspended, fg_reset_requested;
static _Atomic int fg_lead_config = -1;
static pthread_mutex_t fg_stats_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int ready, current, history, authority, authority_attempted, have_camera, previous_valid, invalid, generated;
    int known_ready;
    _Atomic unsigned diag;
    _Atomic unsigned job_trace;
    uint64_t authority_failures[FG_AUTH_FAILURES], authority_failures_reported[FG_AUTH_FAILURES];
    float camera[84], previous[84];
    uint32_t previous_w, previous_h, previous_scene_w, previous_scene_h;
    double budget_ms, unknown_limit;
    int budget_override;
    fg_budget_policy budget;
    fg_qcom_prepared_policy prepared_budget; /* fg_stats_mu; predictive admission/immutable actual audit */
    uint64_t budget_frame, output_budget_frame[FG_OUTPUTS], budget_pre_frame;
    fg_budget_decision budget_pre, budget_last;
    uint64_t budget_pre_skips[2], budget_post_skips[2], budget_samples;
    double budget_real_sum, budget_incremental_sum, budget_total_sum, budget_margin_min;
    uint64_t budget_reserved_frame, budget_feedback_samples, budget_feedback_late, budget_feedback_pauses;
    /* TOTK_FG_ADAPTIVE_BUDGET=1 accounting only (fg_stats_mu); never read when off.
     * frames[] holds the latest accepted reservation per charge slot that used a
     * stale measured incremental, so its immutable audit can be attributed. */
    uint64_t adaptive_budget_frames[FG_BUDGET_FRAMES];
    uint64_t adaptive_budget_admits, adaptive_budget_refusals, adaptive_budget_reservations;
    uint64_t adaptive_budget_audits, adaptive_budget_overshoots, adaptive_budget_missing;
    _Atomic int drs_on;
    fg_drs_policy drs;
    uint64_t drs_log_ns, drs_level;
    double drs_published;
    VkSampler sampler[2]; /* linear scene/final color, nearest-only depth */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipeline, canvas_pipeline, interp_pipeline;
    VkDescriptorSetLayout authority_dsl;
    VkPipelineLayout authority_layout;
    VkPipeline authority_pipeline;
    fg_buffer known;
    VkFormat canvas_format;
    VkQueryPool queries;
    VkQueryPool profile_queries;
    uint64_t profile_samples[FG_PROFILE_STAGES], profile_unavailable[FG_PROFILE_STAGES];
    double profile_sum[FG_PROFILE_STAGES], profile_max[FG_PROFILE_STAGES];
    fg_source source[FG_SOURCES];
    fg_output output[FG_OUTPUTS];
    uint64_t eligible, refused[VK_FG_TRACK_RESULTS], passes[4];
    uint64_t real, generated_count, measured;
    uint64_t cuts, budget_skips, late_skips, history_skips;
    uint64_t depth_range_pairs, depth_range_generated;
    fg_camera_pair_window camera_pairs;
    unsigned projection_diag_logged;
    uint64_t admission[7]; /* state, prediction, capture, output busy, extent, allocation, recorded */
    double sum_ms, max_ms;
    uint64_t reservations, authority_copies, comparisons, authority_skips, recorded;
    uint64_t authority_samples, authority_cost_samples, init_only_samples;
    double raw_unknown_sum, raw_unknown_max, dilated_unknown_sum, dilated_unknown_max;
    double authority_sum_ms, authority_max_ms;
    double init_only_sum_ms, init_only_max_ms;
    double motion_sum[2], motion_max[2], radius_sum[2], radius_max[2];
    uint64_t motion_samples;
    uint64_t uncertified_pixels, uncertified_sources, uncertified_compared;
    uint64_t span_before_copies, span_after_copies, span_count, span_copied_texels, span_failed;
    uint64_t span_cost_samples;
    double span_sum_ms, span_max_ms;
    /* Recording owner only. TOTK_FG_ADAPTIVE_LEAN is decided once per real
       frame (first unknown-writer span decision or capture) and cleared by
       frame_end, so one frame never mixes skipped spans with copied sources. */
    int lean_latched, lean, lean_unknown, lean_refusal_logged;
    uint64_t lean_counts[VK_FG_LEAN_COUNTS], lean_reported[VK_FG_LEAN_COUNTS];
} F;
/* No external-engine state changes the ordinary source/policy layouts. */
static struct {
    VkPipeline pipeline;
    int copy[FG_SOURCES], generated[FG_SOURCES], budget_enabled;
    fg_budget_external budget;
    fg_budget_external_group adaptive_cost[FG_BUDGET_FRAMES];
} FQ;

/* Entirely separate diagnostic lane. Sources share ONLY retirement identities
   with ordinary authority; writer commands never write its certificate/masks. */
enum { FG_WRITER_GROUPS=64, FG_WRITER_SPANS=256, FG_WRITER_WORDS=4+256+64,
       FG_WRITER_QUERIES=2+2*FG_WRITER_SPANS };
typedef struct {
    char vs[80], fs[80], check[80];
    unsigned classification;
    uint64_t gross, residual, draws, spans;
} fg_writer_group;
typedef struct {
    fg_buffer gpu, readback;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    fg_writer_group group[FG_WRITER_GROUPS];
    unsigned span_group[FG_WRITER_SPANS], groups, spans, closed, endpoint;
    uint64_t span_draws[FG_WRITER_SPANS];
    unsigned initialized, open, final, failed, overflow;
    _Atomic int reported;
    const char *failure;
} fg_writer_source;
static _Atomic int fg_writer_config=-1;
static struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkQueryPool queries;
    fg_writer_source source[FG_SOURCES];
    fg_writer_group group[FG_WRITER_GROUPS];
    unsigned groups;
    uint64_t sources, incomplete, failed, overflow, aggregate_overflow, unknown, unowned;
    uint64_t before, after, timing_samples, timing_unavailable;
    double envelope_sum, envelope_max;
#ifdef TOTK_VK_TESTS
    unsigned test_failure, test_span_limit;
#endif
} FW;
static void fg_writer_publish(unsigned index);
static void fg_writer_recycle(unsigned index);
static void fg_writer_final(VkCommandBuffer cb, unsigned index);
static void fg_writer_harvest(void);
static void fg_writer_stats(void);

static void fg_suspend(const char *reason) {
    if (!atomic_exchange_explicit(&fg_suspended, 1, memory_order_relaxed))
        LOGV("framegen suspended: %s (real frames remain unchanged)", reason);
}
/* Walking-owned, DIAG-only: at most two rejected pairs per 300-real window.
   Coefficients are reported directly; p[2]/p[6] cannot distinguish temporal
   jitter from an authored off-center frustum without additional game data. */
static void fg_projection_diag(const float first[84], const float second[84], int interpolation) {
    if (F.projection_diag_logged>=2) return;
    F.projection_diag_logged++;
    fg_projection_reject reject=fg_math_projection_rejection(first,second,interpolation);
    static const char *const reason[]={"stable","extent","aspect","sign","fov/focal-ratio","coefficient"};
    unsigned changed=0;
    for (unsigned i=0;i<16;i++) if (first[28+i]!=second[28+i]) changed|=1u<<i;
    const double degrees=180.0/3.14159265358979323846;
    LOGV("[framegen-projection] real=%llu reject=%s changed-mask=%#x FOV-deg=%.6f->%.6f delta=%+.6f aspect=%.8g->%.8g delta=%+.8g effective-aspect=%.8g->%.8g extent=%.0fx%.0f->%.0fx%.0f (first two projection rejects/window)",
        (unsigned long long)F.real,reason[reject],changed,first[70]*degrees,second[70]*degrees,
        ((double)second[70]-first[70])*degrees,first[62],second[62],(double)second[62]-first[62],
        fabs((double)first[33]/first[28]),fabs((double)second[33]/second[28]),
        first[72],first[73],second[72],second[73]);
    for (unsigned i=0;i<16;i++) if (changed & (1u<<i)) {
        double delta=(double)second[28+i]-first[28+i], limit=.002*(1+fabs(second[28+i]));
        LOGV("[framegen-projection] real=%llu p[%u] class=%s from=%.9g to=%.9g delta=%+.9g legacy-limit=%.9g legacy-cut=%u",
            (unsigned long long)F.real,i,i==0 || i==5 ? "FOV/scale" :
            i==2 || i==6 ? "off-center/jitter" : i==10 || i==11 ? "depth-range" : "structure",
            first[28+i],second[28+i],delta,limit,(unsigned)(fabs(delta)>limit));
    }
}
/* TOTK_FG_ADAPTIVE_LEAN: default on, exact "0" opts out; TOTK_FG_ADAPTIVE_DIRECT_OUT:
 * default off, exact "1". Invalid spellings are logged once and stay off.
 * Resolved only when the adaptive slot scheduler starts (fg_config
 * initialization); every other mode leaves them unresolved (-1), which reads as
 * off. fg_lean_gate still refuses LEAN for a non-QCOM engine, scene AA/scale,
 * WRITER_DIAG and FRAMEGEN_DUMP. */
static _Atomic int fg_adaptive_lean_config=-1, fg_adaptive_direct_config=-1;
static void fg_adaptive_scheduler_selectors(void) {
    const char *values[FGA_SELECTORS];
    for (unsigned i=0;i<FGA_SELECTORS;i++) values[i]=getenv(fga_selector_env(i));
    char summary[512];
    int length=fga_selector_summary(summary,sizeof summary,values);
    if (length>0 && length<(int)sizeof summary)
        LOGV("[framegen-adaptive-selectors] %s (each consumer's exact parser; source default=absent explicit=exact 0/1 invalid=other spelling, treated as off)",summary);
    int lean=fga_default_on_request(values[FGA_SELECTOR_LEAN]);
    int direct=fga_default_off_request(values[FGA_SELECTOR_DIRECT_OUT]);
    atomic_store_explicit(&fg_adaptive_lean_config,lean==1,memory_order_relaxed);
    atomic_store_explicit(&fg_adaptive_direct_config,direct==1,memory_order_relaxed);
    if (lean<0) LOGV("[framegen-adaptive-lean] selector=invalid raw=\"%.32s\" enabled=0 (absent or exact \"1\" on, exact \"0\" off)",
        values[FGA_SELECTOR_LEAN]);
    else LOGV("[framegen-adaptive-lean] selector=%d enabled=%d%s",lean,lean,lean ?
        " (adaptive QCOM scheduler: no ordered depth authority init/spans/pass splits/final compare, no scene/depth source or real fallback copy; AFME input copy kept; scene post, WRITER_DIAG and FRAMEGEN_DUMP keep the copies; exact TOTK_FG_ADAPTIVE_LEAN=0 opts out)" : "");
    if (direct<0) LOGV("[framegen-adaptive-direct-out] selector=invalid raw=\"%.32s\" enabled=0 (absent or exact \"0\" off, exact \"1\" on)",
        values[FGA_SELECTOR_DIRECT_OUT]);
    else LOGV("[framegen-adaptive-direct-out] selector=%d enabled=%d%s",direct,direct,direct ?
        " (prepared child replays the terminal straight into its output when terminal format and extent equal the output; otherwise the mode-2 copy remains; FRAMEGEN_DUMP keeps the copy)" : "");
}
int vk_fg_enabled(void) {
    int on = atomic_load_explicit(&fg_config, memory_order_relaxed);
    if (on < 0) {
        const char *e = getenv("TOTK_FRAMEGEN");
        const char *diag = getenv("TOTK_FRAMEGEN_DIAG");
        F.diag = diag && !strcmp(diag,"1");
        const char *trace=getenv("TOTK_FRAMEGEN_JOB_TRACE");
        F.job_trace=trace && !strcmp(trace,"1");
        const char *drs=getenv("TOTK_DRS_FRAMEGEN");
        F.drs_on=drs && !strcmp(drs,"1");
        int hybrid=e && !strcmp(e,"hybrid");
        int locked=e && !strcmp(e,"locked");
        int adaptive=e && !strcmp(e,"adaptive");
        on=hybrid ? (runtime_fps_hybrid_selected() ? 3 : 0) :
            locked ? 4 : adaptive ? (runtime_fps_adaptive_selected() ? 5 : 0) :
            e && !strcmp(e,"interp") ? 2 : e && !strcmp(e,"1");
        const char *reason = NULL;
        if (on) {
            e = getenv("TOTK_FPS60");
            if (!hybrid && !adaptive && e && strcmp(e,"0")) reason="TOTK_FPS60 must be 0";
            e = getenv("TOTK_UNCAPPED");
            if (e && strcmp(e, "0")) reason = "TOTK_UNCAPPED must be 0";
            e = getenv("TOTK_FPS_AUTO");
            if (e && !strcmp(e, "1")) reason = "TOTK_FPS_AUTO must be off";
            e = getenv("TOTK_PRESENT_SLOTS");
            if (e && strcmp(e, "1")) reason = "TOTK_PRESENT_SLOTS must be 1";
            e = getenv("TOTK_REALTIME");
            if (locked && (!e || strcmp(e,"1"))) reason="locked interpolation requires TOTK_REALTIME=1";
            e = getenv("TOTK_FRAMEGEN_BUDGET_MS");
            /* Pure selector parsing: never recurse through vk_fg_adaptive()
             * while fg_config is being initialized. Flag-off adaptive retains
             * its existing override policy; only this scheduler adds a gate. */
            if ((locked || (adaptive && runtime_present_adaptive_slots_request(
                    getenv("TOTK_FG_ADAPTIVE_SLOTS"))!=0)) && e && *e)
                reason="locked/scheduler interpolation requires the total GPU budget (no override)";
            e = getenv("TOTK_FRAMEGEN_LEAD_REFRESHES");
            if (locked && e && strcmp(e,"1") && strcmp(e,"2"))
                reason="TOTK_FRAMEGEN_LEAD_REFRESHES must be 1 or 2";
        }
        if (reason) on = 0;
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(&fg_config, &expected, on,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            if (reason) LOGV("framegen refused: %s", reason);
            if (on==5 && runtime_present_adaptive_slots_request(getenv("TOTK_FG_ADAPTIVE_SLOTS"))==1) {
                atomic_store_explicit(&vk_fg_terminal_owned_active,1,memory_order_relaxed);
                fg_adaptive_scheduler_selectors();
            }
        } else on = expected;
    }
    if (on >= 2) (void)vk_fg_uncertified_plane();
    return on && !atomic_load_explicit(&fg_suspended, memory_order_relaxed);
}
int vk_fg_interpolating(void) {
    if (atomic_load_explicit(&fg_config,memory_order_relaxed) < 0) (void)vk_fg_enabled();
    return atomic_load_explicit(&fg_config,memory_order_relaxed) >= 2;
}
int vk_fg_locked(void) {
    if (atomic_load_explicit(&fg_config,memory_order_relaxed) < 0) (void)vk_fg_enabled();
    return atomic_load_explicit(&fg_config,memory_order_relaxed) == 4;
}
int vk_fg_adaptive(void) {
    if (atomic_load_explicit(&fg_config,memory_order_relaxed) < 0) (void)vk_fg_enabled();
    return atomic_load_explicit(&fg_config,memory_order_relaxed) == 5;
}
/* TOTK_FG_ADAPTIVE_BUDGET: absent/"0" off, "1" on, any other spelling is
 * invalid, treated as off and logged once. Parsed once, and only consulted by
 * the adaptive slot scheduler's admission (fg_budget_admit_stale). */
static _Atomic int fg_adaptive_budget_config=-1;
static int fg_adaptive_budget(void) {
    if (!vk_fg_adaptive() || !atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed))
        return 0;
    int on=atomic_load_explicit(&fg_adaptive_budget_config,memory_order_relaxed);
    if (on<0) {
        const char *e=getenv("TOTK_FG_ADAPTIVE_BUDGET");
        int parsed=!e || !strcmp(e,"0") ? 0 : !strcmp(e,"1") ? 1 : 2;
        int expected=-1;
        if (atomic_compare_exchange_strong_explicit(&fg_adaptive_budget_config,&expected,parsed,
                memory_order_relaxed,memory_order_relaxed)) {
            if (parsed==2) LOGV("[framegen-adaptive-budget] ignored: expected TOTK_FG_ADAPTIVE_BUDGET=0 or 1 (exact spelling); r7 probe admission retained");
            else if (parsed==1) LOGV("[framegen-adaptive-budget] enabled: stale measured incremental p90 (32-interval window) replaces the unestimated probe; ceiling_ms=%.3f, prepared back-off %u intervals, audits and probe-without-sample unchanged",
                FG_BUDGET_TARGET_MS,FG_QCOM_PREPARED_BACKOFF);
        } else parsed=expected;
        on=parsed;
    }
    return on==1;
}
/* Accounting only: true once the scheduler admission path resolved the selector on. */
static int fg_adaptive_budget_selected(void) {
    return atomic_load_explicit(&fg_adaptive_budget_config,memory_order_relaxed)==1;
}
/* Consumer audit (vk_framegen.c): adaptive generation is QCOM extrapolation
 * only. Its compose (mode 5, framegen_qcom.frag) samples the AFME output and
 * UI layers; the output pass (mode 2) returns the replayed terminal before any
 * scene/depth/fallback/earlier-real tap. Interpolation compose, masked output,
 * pair dilation and LSFG are never reached with fg_config==5 (QCOM refusal is
 * real-only, never a camera fallback), and the completion path forces the
 * pair status for every QCOM/scheduler output. The remaining readers are DIAG
 * dumps, writer attribution and native scene seeds, which keep lean off, and
 * the host TEST bridge, which samples s->scene in place of AFME pixels. */
static int fg_lean_gate(const char **why) {
    *why=NULL;
    if (!vk_fg_adaptive() || !atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed) ||
        atomic_load_explicit(&fg_adaptive_lean_config,memory_order_relaxed)!=1) return 0;
    if (!vk_fg_qcom_requested() && !fg_generation_test_bridge) *why="adaptive engine is not QCOM";
    else if (vk_scene_post_enabled()) *why="scene AA/scale keeps retained native seeds";
    else if (vk_fg_writer_diag()) *why="WRITER_DIAG attributes ordered authority spans";
    else if (fg_dump_frame()) *why="FRAMEGEN_DUMP reads scene/depth/real images";
    else return 1;
    return 0;
}
int vk_fg_adaptive_lean(void) {
    if (!F.lean_latched) {
        const char *why;
        F.lean=fg_lean_gate(&why); F.lean_latched=1;
        if (why && !F.lean_refusal_logged) {
            F.lean_refusal_logged=1;
            LOGV("[framegen-adaptive-lean] selected but inactive: %s; ordinary authority and source copies retained",why);
        }
    }
    return F.lean;
}
int vk_fg_adaptive_lean_span(void) {
    if (!vk_fg_adaptive_lean()) return 0;
    F.lean_counts[VK_FG_LEAN_UNKNOWN_DRAWS]++; F.lean_unknown=1;
    return 1;
}
unsigned vk_fg_lead_refreshes(void) {
    int lead=atomic_load_explicit(&fg_lead_config,memory_order_relaxed);
    if (lead < 0) {
        const char *e=getenv("TOTK_FRAMEGEN_LEAD_REFRESHES");
        lead=e && !strcmp(e,"2") ? 2 : 1;
        atomic_store_explicit(&fg_lead_config,lead,memory_order_relaxed);
    }
    return (unsigned)lead;
}
int vk_fg_capturing(void) {
    if (!vk_fg_enabled()) return 0;
    return atomic_load_explicit(&fg_config,memory_order_relaxed)!=3 ||
        runtime_fps_hybrid_interpolating();
}
int vk_fg_uncertified_plane(void) {
    int on=atomic_load_explicit(&fg_uncertified_config,memory_order_relaxed);
    if (on < 0) {
        const char *e=getenv("TOTK_FRAMEGEN_UNCERTIFIED_PLANE");
        on=vk_fg_interpolating() && e && !strcmp(e,"1");
        int expected=-1;
        if (atomic_compare_exchange_strong_explicit(&fg_uncertified_config,&expected,on,
                                                    memory_order_relaxed,memory_order_relaxed)) {
            if (on) LOGV("framegen UNCERTIFIED: TOTK_FRAMEGEN_UNCERTIFIED_PLANE=1 assumes exact nominated active-shrink plane depth VALUE authority; NOT a proof or error bound; other unknown final values remain fail-closed");
        } else on=expected;
    }
    return on;
}
void vk_fg_late(void) {
    if (vk_fg_interpolating()) {
        pthread_mutex_lock(&fg_stats_mu); F.late_skips++; pthread_mutex_unlock(&fg_stats_mu);
    } else fg_suspend("generated GPU work missed its display deadline");
}
void vk_fg_reset(void) {
    atomic_store_explicit(&fg_reset_requested,1,memory_order_release);
    runtime_fps_adaptive_reset();
}
/* The UI thread only publishes a request; recorder-owned history is touched here. */
static void fg_consume_reset(void) {
    if (!atomic_exchange_explicit(&fg_reset_requested,0,memory_order_acquire)) return;
    fg_generation_cancel_all();
    F.previous_valid=0;
    if (F.ready) F.history=-1;
    if (F.have_camera || (F.ready && (F.current >= 0 || F.authority >= 0))) F.invalid=1;
}
void vk_fg_history_cut(void) {
    runtime_fps_adaptive_reset();
    fg_consume_reset();
    fg_generation_cancel_all();
    F.previous_valid=0;
    if (F.ready) F.history=-1;
}

static int fg_result(VkResult result, const char *operation) {
    if (result == VK_SUCCESS) return 1;
    LOGV("framegen %s failed: %d", operation, result);
    fg_suspend("GPU resources unavailable");
    return 0;
}
static void fg_buffer_free(fg_buffer *b, int host) {
    if (b->mapped) vkUnmapMemory(V.dev,b->memory);
    if (b->buffer) vkDestroyBuffer(V.dev,b->buffer,NULL);
    vk_memory_release(b->memory,host ? VK_MEMORY_READBACK : VK_MEMORY_SNAPSHOT);
    memset(b,0,sizeof *b);
}
/* Only an idle source may resize these allocations. They are not submission
   arena temporaries: snapshots can precede capture by several submissions. */
static int fg_buffer_make(fg_buffer *b, VkDeviceSize bytes, int host) {
    if (b->buffer && b->size == bytes) return 1;
    fg_buffer_free(b,host);
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (!fg_result(vkCreateBuffer(V.dev,&ci,NULL,&b->buffer),"authority buffer creation")) goto fail;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.dev,b->buffer,&req);
    int type=vk_pick_type(req.memoryTypeBits,host ?
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (host && type < 0) type=vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type < 0) { fg_suspend("authority buffer memory type unavailable"); goto fail; }
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
    if (!fg_result(vk_memory_allocate(&ai,&b->memory,host ? VK_MEMORY_READBACK : VK_MEMORY_SNAPSHOT),
        "authority buffer memory allocation") ||
        !fg_result(vkBindBufferMemory(V.dev,b->buffer,b->memory,0),"authority buffer binding")) goto fail;
    if (host) {
        if (!fg_result(vkMapMemory(V.dev,b->memory,0,VK_WHOLE_SIZE,0,&b->mapped),"authority readback mapping")) goto fail;
        b->coherent=!!(V.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    b->size=bytes;
    return 1;
fail:
    fg_buffer_free(b,host);
    return 0;
}
static void fg_buffer_barrier(VkCommandBuffer cb, fg_buffer *b, VkPipelineStageFlags2 src,
                               VkAccessFlags2 read, VkPipelineStageFlags2 dst, VkAccessFlags2 write) {
    VkBufferMemoryBarrier2 barrier={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask=src,.srcAccessMask=read,.dstStageMask=dst,.dstAccessMask=write,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=b->buffer,.offset=0,.size=VK_WHOLE_SIZE};
    VkDependencyInfo info={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(cb,&info);
}
/* Scheduler-only terminal ownership. The source pool already owns every
 * canvas/UI image. Copy ONLY the terminal's frozen readonly host payload and
 * descriptor bindings at their original freeze point; never reread guest
 * memory at dispatch. Four sources bound data to 8 MiB, with two fixed-size
 * descriptor sets each. Source references + covering GPU tickets (including
 * quarantine) protect these allocations exactly like the source images.
 * Unowned images/buffers and capacity excess refuse optional generation;
 * the original real draw and the legacy borrowed replay path are untouched. */
static const char *fg_terminal_input_role(unsigned role) {
    return role==VK_FG_TERMINAL_INPUT_DESCRIPTOR ? "descriptor" :
        role==VK_FG_TERMINAL_INPUT_VERTEX ? "vertex" :
        role==VK_FG_TERMINAL_INPUT_INDEX ? "index" : "none";
}
static int fg_terminal_reject_detail(fg_source *s,const char *why,const char *detail) {
    if (!s->owned.failed && atomic_load_explicit(&F.job_trace,memory_order_relaxed)) {
        const vk_fg_terminal_input *in=&s->owned.input;
        s->owned.refusal=*in;
        LOGV("[adaptive-slot-owned] event=refused frame=%llu reason=%s bytes=%llu writes=%u detail=%s role=%s stage=%u set=%u binding=%u element=%u stream=%u descriptor-type=%u buffer=%#llx offset=%llu range=%llu freeze=%u serial=%llu",
            (unsigned long long)(F.budget_frame+1),why,
            (unsigned long long)s->owned.used,s->owned.writes,detail,
            fg_terminal_input_role(in->role),in->stage,in->set,in->binding,
            in->element,in->stream,in->descriptor_type,
            (unsigned long long)(uintptr_t)in->buffer,(unsigned long long)in->offset,
            (unsigned long long)in->bytes,in->freeze,(unsigned long long)vk_submit_serial());
    }
    s->owned.failed=1;
    return 0;
}
static int fg_terminal_reject(fg_source *s,const char *why) {
    return fg_terminal_reject_detail(s,why,"none");
}
void vk_fg_terminal_owned_input_refuse(const vk_fg_terminal_input *input,const char *detail) {
    if (!input || F.current<0 ||
            !atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed)) return;
    fg_source *s=&F.source[F.current];
    if (!s->owned.begun || s->owned.frame!=F.budget_frame+1) return;
    if (atomic_load_explicit(&F.job_trace,memory_order_relaxed)) s->owned.input=*input;
    (void)fg_terminal_reject_detail(s,"source-freeze-unavailable",detail);
}
void vk_fg_terminal_owned_fail(void) {
    if (F.current>=0 && atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed))
        (void)fg_terminal_reject(&F.source[F.current],"capture-incomplete");
}
static int fg_terminal_buffer(fg_source *s) {
    fg_buffer *b=&s->owned.bytes;
    if (b->buffer) return 1;
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=VK_FG_TERMINAL_BYTES,
        .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    if (!fg_result(vkCreateBuffer(V.dev,&ci,NULL,&b->buffer),"immutable terminal buffer")) goto fail;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.dev,b->buffer,&req);
    int type=vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type<0) type=vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type<0) goto fail;
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
    if (!fg_result(vk_memory_allocate(&ai,&b->memory,VK_MEMORY_SNAPSHOT),"immutable terminal memory") ||
        !fg_result(vkBindBufferMemory(V.dev,b->buffer,b->memory,0),"immutable terminal binding") ||
        !fg_result(vkMapMemory(V.dev,b->memory,0,VK_WHOLE_SIZE,0,&b->mapped),"immutable terminal mapping")) goto fail;
    b->size=VK_FG_TERMINAL_BYTES;
    b->coherent=!!(V.mp.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    return 1;
fail:
    fg_buffer_free(b,0);
    return fg_terminal_reject(s,"host-storage-unavailable");
}
int vk_fg_terminal_owned_begin(void) {
    if (!atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed) || F.current<0) return 0;
    fg_source *s=&F.source[F.current];
    fg_terminal_owned *o=&s->owned;
    if (o->begun || atomic_load_explicit(&s->referenced,memory_order_acquire))
        return fg_terminal_reject(s,"duplicate-or-retained-source");
    o->begun=1; o->frame=F.budget_frame+1;
    if (atomic_load_explicit(&F.job_trace,memory_order_relaxed)) {
        s->owned.input=(vk_fg_terminal_input){0};
        s->owned.refusal=(vk_fg_terminal_input){0};
    }
    if (o->pool) {
        if (!fg_result(vkResetDescriptorPool(V.dev,o->pool,0),"immutable terminal pool reset"))
            return fg_terminal_reject(s,"descriptor-reset");
    } else {
        VkDescriptorPoolSize sizes[]={
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_FG_TERMINAL_DESCRIPTORS},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_FG_TERMINAL_DESCRIPTORS},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_FG_TERMINAL_DESCRIPTORS},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_FG_TERMINAL_DESCRIPTORS},
            {VK_DESCRIPTOR_TYPE_SAMPLER,VK_FG_TERMINAL_DESCRIPTORS}};
        VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=2,.poolSizeCount=sizeof sizes/sizeof sizes[0],.pPoolSizes=sizes};
        if (!fg_result(vkCreateDescriptorPool(V.dev,&pi,NULL,&o->pool),"immutable terminal descriptors"))
            return fg_terminal_reject(s,"descriptor-pool");
    }
    return 1;
}
static int fg_terminal_copy(fg_source *s,VkDescriptorBufferInfo source,VkDeviceSize align,
        VkDescriptorBufferInfo *out,vk_fg_terminal_input origin) {
    fg_terminal_owned *o=&s->owned;
    int trace=atomic_load_explicit(&F.job_trace,memory_order_relaxed);
    if (trace) {
        o->input=origin; o->input.buffer=source.buffer;
        o->input.offset=source.offset; o->input.bytes=source.range;
    }
    if (!source.buffer) {
        *out=source;
        if (trace) o->input=(vk_fg_terminal_input){0};
        return 1;
    } /* supported Vulkan null descriptor */
    if (!source.range || source.range==VK_WHOLE_SIZE || source.range>VK_FG_TERMINAL_BYTES)
        return fg_terminal_reject(s,"buffer-range");
    if (!align) align=16;
    uint64_t serial=vk_submit_serial();
    for (unsigned i=0;i<o->copies;i++) if (o->copy[i].buffer==source.buffer &&
            o->copy[i].offset==source.offset && o->copy[i].bytes==source.range &&
            o->copy[i].serial==serial && o->copy[i].owned_offset%align==0) {
        *out=(VkDescriptorBufferInfo){o->bytes.buffer,o->copy[i].owned_offset,source.range};
        if (trace) o->input=(vk_fg_terminal_input){0};
        return 1;
    }
    if (align>VK_FG_TERMINAL_BYTES || o->used>VK_FG_TERMINAL_BYTES-(align-1) ||
            o->copies==VK_FG_TERMINAL_DESCRIPTORS)
        return fg_terminal_reject(s,"storage-capacity");
    VkDeviceSize at=(o->used+align-1)/align*align;
    if (at>VK_FG_TERMINAL_BYTES-source.range) return fg_terminal_reject(s,"storage-capacity");
    if (!fg_terminal_buffer(s)) return 0;
    if (!vk_replay_frozen_buffer_copy(source.buffer,source.offset,source.range,(uint8_t *)o->bytes.mapped+at)) {
        if (trace) o->input.freeze=0;
        return fg_terminal_reject_detail(s,"buffer-not-owned-frozen-storage","written-host-arena-miss");
    }
    o->copy[o->copies++]=(__typeof__(o->copy[0])){source.buffer,source.offset,source.range,at,serial};
    o->used=at+source.range;
    *out=(VkDescriptorBufferInfo){o->bytes.buffer,at,source.range};
    if (trace) o->input=(vk_fg_terminal_input){0};
    return 1;
}
int vk_fg_terminal_owned_binding(unsigned set,VkDescriptorSetLayout layout,
        const VkWriteDescriptorSet *write,int writable) {
    if (!atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed) || F.current<0) return 0;
    fg_source *s=&F.source[F.current]; fg_terminal_owned *o=&s->owned;
    if (!o->begun || o->failed || o->frame!=F.budget_frame+1) return 0;
    if (!write || !layout || set>=2 || writable || !write->descriptorCount ||
        write->descriptorCount>VK_FG_TERMINAL_DESCRIPTORS || o->writes>=VK_FG_TERMINAL_DESCRIPTORS)
        return fg_terminal_reject(s,"descriptor-shape-or-writable");
    unsigned kind;
    switch (write->descriptorType) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:kind=0;break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:kind=1;break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:kind=2;break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:kind=3;break;
    case VK_DESCRIPTOR_TYPE_SAMPLER:kind=4;break;
    default:return fg_terminal_reject(s,"descriptor-type");
    }
    if (o->descriptors[kind]>VK_FG_TERMINAL_DESCRIPTORS-write->descriptorCount ||
        (o->layouts[set] && o->layouts[set]!=layout))
        return fg_terminal_reject(s,"descriptor-capacity-or-layout");
    if (!o->sets[set]) {
        VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool=o->pool,.descriptorSetCount=1,.pSetLayouts=&layout};
        if (!fg_result(vkAllocateDescriptorSets(V.dev,&ai,&o->sets[set]),"immutable terminal set"))
            return fg_terminal_reject(s,"descriptor-allocation");
        o->layouts[set]=layout;
    }
    VkDescriptorBufferInfo buffers[VK_FG_TERMINAL_DESCRIPTORS];
    VkDescriptorImageInfo images[VK_FG_TERMINAL_DESCRIPTORS];
    VkWriteDescriptorSet owned=*write; owned.dstSet=o->sets[set];
    if (kind<2) {
        if (!write->pBufferInfo) return fg_terminal_reject(s,"missing-buffer-payload");
        VkDeviceSize align=kind ? V.props.limits.minStorageBufferOffsetAlignment : V.props.limits.minUniformBufferOffsetAlignment;
        for (unsigned i=0;i<write->descriptorCount;i++)
            if (!fg_terminal_copy(s,write->pBufferInfo[i],align,&buffers[i],(vk_fg_terminal_input){
                    .role=VK_FG_TERMINAL_INPUT_DESCRIPTOR,.stage=set,.set=set,
                    .binding=write->dstBinding,.element=write->dstArrayElement+i,
                    .descriptor_type=write->descriptorType})) return 0;
        owned.pBufferInfo=buffers;
    } else {
        if (!write->pImageInfo) return fg_terminal_reject(s,"missing-image-payload");
        for (unsigned i=0;i<write->descriptorCount;i++) {
            images[i]=write->pImageInfo[i];
            /* Terminal samples only the certified, source-private canvas.
             * Cached sampler objects live for the process (vk_res.c); no
             * mutable guest texture/view is borrowed by delayed generation. */
            if (kind!=4 && images[i].imageView && images[i].imageView!=s->canvas.raw &&
                    images[i].imageView!=s->canvas.display)
                return fg_terminal_reject(s,"image-not-source-canvas");
        }
        owned.pImageInfo=images;
    }
    vkUpdateDescriptorSets(V.dev,1,&owned,0,NULL);
    o->descriptors[kind]+=write->descriptorCount; o->writes++;
    return 1;
}
static void fg_image_free(fg_image *im) {
    if (im->display && im->display != im->raw) vkDestroyImageView(V.dev, im->display, NULL);
    if (im->raw) vkDestroyImageView(V.dev, im->raw, NULL);
    if (im->image) vkDestroyImage(V.dev, im->image, NULL);
    vk_mem_free(&im->memory);
    memset(im, 0, sizeof *im);
}
/* Called only for an idle ring entry. A resize never adds a retired allocation. */
static int fg_image_make(fg_image *im, uint32_t w, uint32_t h, VkFormat format,
                         VkImageAspectFlags aspect, int output) {
    if (im->image && im->w == w && im->h == h && im->format == format) return 1;
    fg_image_free(im);
    VkFormat raw = output ? vk_fg_raw_format(format) : format;
    VkFormat formats[2] = {raw, format};
    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = formats};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, raw != format ? &list : NULL,
        .flags = raw != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w,h,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | (output ?
            (aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
            (output && (aspect != VK_IMAGE_ASPECT_COLOR_BIT || output == 2) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
            (fg_dump_frame() ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)};
    if (!fg_result(vkCreateImage(V.dev, &ci, NULL, &im->image), "image creation")) goto fail;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.dev, im->image, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->memory)) {
        fg_suspend("image memory unavailable"); goto fail;
    }
    if (!fg_result(vkBindImageMemory(V.dev, im->image, im->memory.mem, im->memory.off), "image binding")) goto fail;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = raw, .subresourceRange = {aspect,0,1,0,1}};
    if (!fg_result(vkCreateImageView(V.dev, &vi, NULL, &im->raw), "raw view creation")) goto fail;
    im->display = im->raw;
    if (raw != format) {
        vi.format = format;
        if (!fg_result(vkCreateImageView(V.dev, &vi, NULL, &im->display), "display view creation")) goto fail;
    }
    im->w = w; im->h = h; im->format = format; im->aspect = aspect;
    return 1;
fail:
    fg_image_free(im);
    return 0;
}
static void fg_barrier(VkCommandBuffer cb, VkImage image, VkImageSubresourceRange range,
                        VkImageLayout old, VkImageLayout next, VkPipelineStageFlags2 src,
                        VkAccessFlags2 read, VkPipelineStageFlags2 dst, VkAccessFlags2 write) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=src, .srcAccessMask=read, .dstStageMask=dst, .dstAccessMask=write,
        .oldLayout=old, .newLayout=next, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=image, .subresourceRange=range};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount=1, .pImageMemoryBarriers=&b};
    vkCmdPipelineBarrier2(cb, &d);
}
/* A once-per-allocation defined state for an image lean leaves uncopied: no
   QCOM pass samples it, but every descriptor binding names a valid view in
   its declared SHADER_READ_ONLY layout and never exposes undefined memory. */
static void fg_image_placeholder(VkCommandBuffer cb, fg_image *im) {
    if (!im->image || im->initialized) return;
    VkImageSubresourceRange range={im->aspect,0,1,0,1};
    fg_barrier(cb,im->image,range,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (im->aspect==VK_IMAGE_ASPECT_COLOR_BIT) {
        const VkClearColorValue zero={{0,0,0,0}};
        vkCmdClearColorImage(cb,im->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
    } else {
        const VkClearDepthStencilValue zero={0,0};
        vkCmdClearDepthStencilImage(cb,im->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
    }
    fg_barrier(cb,im->image,range,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    im->initialized=1;
}
static int fg_make_pipeline(VkFormat format, VkPipeline *pipeline, int interp) {
    const uint32_t *code[] = {framegen_vert_spv, interp==2 ? framegen_qcom_frag_spv :
        interp ? framegen_interp_frag_spv : framegen_frag_spv};
    const size_t bytes[] = {sizeof framegen_vert_spv, interp==2 ? sizeof framegen_qcom_frag_spv :
        interp ? sizeof framegen_interp_frag_spv : sizeof framegen_frag_spv};
    int dual=interp==1;
    VkShaderModule module[2] = {0};
    for (unsigned i=0; i<2; i++) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=bytes[i], .pCode=code[i]};
        if (!fg_result(vkCreateShaderModule(V.dev, &mi, NULL, &module[i]), "shader creation")) {
            if (module[0]) vkDestroyShaderModule(V.dev, module[0], NULL);
            return 0;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_VERTEX_BIT, .module=module[0], .pName="main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=module[1], .pName="main"}};
    VkBool32 scene_scale=vk_scene_post_enabled();
    VkSpecializationMapEntry entry={.constantID=0,.offset=0,.size=sizeof scene_scale};
    VkSpecializationInfo spec={.mapEntryCount=1,.pMapEntries=&entry,.dataSize=sizeof scene_scale,.pData=&scene_scale};
    if (interp) stages[1].pSpecializationInfo=&spec;
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount=1, .scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL, .lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att[2] = {{.colorWriteMask=0xf},{.colorWriteMask=0xf}};
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=dual ? 2 : 1, .pAttachments=att};
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=2, .pDynamicStates=dynamic};
    VkFormat formats[2]={format,VK_FORMAT_R8_UNORM};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount=dual ? 2 : 1, .pColorAttachmentFormats=formats};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering,
        .stageCount=2, .pStages=stages, .pVertexInputState=&vi, .pInputAssemblyState=&ia, .pViewportState=&vp,
        .pRasterizationState=&rs, .pMultisampleState=&ms, .pColorBlendState=&blend, .pDynamicState=&dy, .layout=F.layout};
    VkResult result = vk_pcache_graphics(&pi, pipeline);
    for (unsigned i=0; i<2; i++) vkDestroyShaderModule(V.dev, module[i], NULL);
    if (!fg_result(result, "pipeline creation")) return 0;
    return 1;
}
static int fg_authority_init(void) {
    if (F.authority_pipeline) return 1;
    VkDescriptorSetLayoutBinding bindings[2];
    for (unsigned i=0;i<2;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo ds={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings};
    if (!fg_result(vkCreateDescriptorSetLayout(V.dev,&ds,NULL,&F.authority_dsl),"authority descriptor layout creation")) return 0;
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,96};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,
        .pSetLayouts=&F.authority_dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    if (!fg_result(vkCreatePipelineLayout(V.dev,&li,NULL,&F.authority_layout),"authority pipeline layout creation")) return 0;
    VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof framegen_authority_comp_spv,.pCode=framegen_authority_comp_spv};
    VkShaderModule module;
    if (!fg_result(vkCreateShaderModule(V.dev,&mi,NULL,&module),"authority shader creation")) return 0;
    VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
            .module=module,.pName="main"},.layout=F.authority_layout};
    VkResult result=vk_pcache_compute(&ci,&F.authority_pipeline);
    vkDestroyShaderModule(V.dev,module,NULL);
    return fg_result(result,"authority compute pipeline creation");
}
static int fg_init(void) {
    if (F.ready) return 1;
    (void)vk_fg_writer_diag(); /* one explicit on/off timing-arm status, no Vulkan work */
    F.current = -1;
    F.history = -1;
    F.authority = -1;
    F.unknown_limit = 35.0;
    const char *unknown = getenv("TOTK_FRAMEGEN_UNKNOWN_PERCENT");
    if (unknown && *unknown) {
        char *end; double n=strtod(unknown,&end);
        if (!*end && isfinite(n) && n >= 0 && n <= 100) F.unknown_limit=n;
    }
    F.budget_ms = vk_fg_interpolating() ? FG_BUDGET_TARGET_MS : 4.0;
    F.budget.feedback_gpu_only=atomic_load_explicit(&fg_config,memory_order_relaxed) == 4;
    const char *e = getenv("TOTK_FRAMEGEN_BUDGET_MS");
    if (e && *e) {
        char *end; double n = strtod(e, &end);
        if (!*end && isfinite(n) && n > 0) { F.budget_ms=n; F.budget_override=1; }
    }
    if (!V.timestamp_bits || !isfinite(V.props.limits.timestampPeriod) || V.props.limits.timestampPeriod <= 0) {
        fg_suspend("GPU timestamps unavailable on the graphics queue"); return 0;
    }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=(FG_SOURCES*8+FG_OUTPUTS)*2+
            (vk_fg_interpolating() ? FG_SOURCES*FG_AUTHORITY_SPAN_QUERIES : 0)};
    if (!fg_result(vkCreateQueryPool(V.dev, &qi, NULL, &F.queries), "timestamp pool creation")) return 0;
    if (F.diag) {
        VkQueryPoolCreateInfo pi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType=VK_QUERY_TYPE_TIMESTAMP,
            .queryCount=FG_SOURCES*FG_PROFILE_SOURCE_QUERIES+FG_OUTPUTS*FG_PROFILE_OUTPUT_QUERIES};
        VkResult result=vkCreateQueryPool(V.dev,&pi,NULL,&F.profile_queries);
        if (result != VK_SUCCESS) LOGV("[framegen-profile] phase queries unavailable: %d (budget timing unchanged)",result);
    }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (unsigned i=0; i<2; i++) {
        si.magFilter=si.minFilter=i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        if (!fg_result(vkCreateSampler(V.dev, &si, NULL, &F.sampler[i]), "sampler creation")) {
            for (unsigned j=0; j<i; j++) {
                vkDestroySampler(V.dev,F.sampler[j],NULL);
                F.sampler[j]=VK_NULL_HANDLE;
            }
            if (F.profile_queries) vkDestroyQueryPool(V.dev,F.profile_queries,NULL);
            F.profile_queries=VK_NULL_HANDLE;
            vkDestroyQueryPool(V.dev,F.queries,NULL);
            F.queries=VK_NULL_HANDLE;
            return 0;
        }
    }
    if (!fg_buffer_make(&F.known,16,0)) return 0;
    VkDescriptorSetLayoutBinding bindings[11];
    for (unsigned i=0; i<11; i++) {
        bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,
            .descriptorType=i == 7 || i == 8 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
        if (i != 7 && i != 8) bindings[i].pImmutableSamplers=&F.sampler[i == 1 || i == 4 || i >= 9];
    }
    VkDescriptorSetLayoutCreateInfo ds = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=11, .pBindings=bindings};
    if (!fg_result(vkCreateDescriptorSetLayout(V.dev, &ds, NULL, &F.dsl), "descriptor layout creation")) return 0;
    VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1, .pSetLayouts=&F.dsl, .pushConstantRangeCount=1, .pPushConstantRanges=&range};
    if (!fg_result(vkCreatePipelineLayout(V.dev, &li, NULL, &F.layout), "pipeline layout creation")) return 0;
    if (!fg_make_pipeline(VK_FORMAT_R8G8B8A8_UNORM, &F.pipeline,0)) return 0;
    F.ready = 1;
    LOGV("framegen: %s, GPU budget %s %.3f ms; dilated unknown limit %.3f%%",
        vk_fg_adaptive() ? "adaptive QCOM extrapolation (no added interpolation interval; real-only on refusal)" :
        vk_fg_locked() ? "camera locked interpolation (one real tick plus buffered physical refreshes)" :
        vk_fg_interpolating() ? "camera interpolation (one real frame latency)" : "camera extrapolation",
        F.budget_override ? "fixed-override incremental" : vk_fg_interpolating() ? "adaptive total" : "legacy incremental",
        F.budget_ms,F.unknown_limit);
    if (vk_fg_interpolating() && !F.budget_override)
        LOGV("framegen headroom: %.3f ms tick, %.3f ms reserved margin; recent %u-frame nearest-rank p90, real/incremental max age %u ticks; unknown-incremental probe at most once/%u ticks with measured real below target; real includes source/replay/authority/pair work, incremental is separate synthetic ticket range",
            FG_BUDGET_TICK_MS,FG_BUDGET_TICK_MS-FG_BUDGET_TARGET_MS,FG_BUDGET_WINDOW,FG_BUDGET_MAX_AGE,FG_BUDGET_PROBE_INTERVAL);
    if (vk_fg_interpolating() && F.budget_override)
        LOGV("framegen ordered authority fixed override: charges state initialization, clipped before/after copies and VALUE updates, final census; at most %u spans/source, no real-draw envelope double charge",FG_AUTHORITY_SPANS);
    return 1;
}

static int fg_timestamp(unsigned query, double *ms) {
    uint64_t t[2];
    VkResult r = vkGetQueryPoolResults(V.dev, F.queries, query, 2, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) { fg_suspend("GPU timestamps unavailable after completion fence"); return 0; }
    uint64_t mask = V.timestamp_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << V.timestamp_bits)-1;
    *ms = (double)((t[1]-t[0]) & mask) * V.props.limits.timestampPeriod / 1e6;
    return 1;
}
static unsigned fg_profile_source_query(unsigned index, unsigned stage) {
    return index*FG_PROFILE_SOURCE_QUERIES+(stage-FG_PROFILE_BASE)*2;
}
static unsigned fg_profile_output_query(unsigned slot, unsigned stage) {
    return FG_SOURCES*FG_PROFILE_SOURCE_QUERIES+slot*FG_PROFILE_OUTPUT_QUERIES+
        (stage-FG_PROFILE_SOURCE_STAGES)*2;
}
static void fg_profile_begin(VkCommandBuffer cb, unsigned query) {
    vkCmdResetQueryPool(cb,F.profile_queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,query);
}
static void fg_profile_source_begin(VkCommandBuffer cb, unsigned index, unsigned stage) {
    if (F.profile_queries) fg_profile_begin(cb,fg_profile_source_query(index,stage));
}
static void fg_profile_source_mark(unsigned index, unsigned stage) {
    if (F.profile_queries) F.source[index].profile_mask|=UINT32_C(1)<<stage;
}
static void fg_profile_source_end(VkCommandBuffer cb, unsigned index, unsigned stage) {
    if (!F.profile_queries) return;
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
        fg_profile_source_query(index,stage)+1);
    fg_profile_source_mark(index,stage);
}
/* Caller owns the covering fence. Diagnostic query failure never substitutes a
   duration or changes the existing budget/authority acceptance semantics. */
static int fg_profile_read(VkQueryPool pool, unsigned query, double *ms) {
    uint64_t t[2];
    if (vkGetQueryPoolResults(V.dev,pool,query,2,sizeof t,t,sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    uint64_t mask=V.timestamp_bits>=64 ? UINT64_MAX : (UINT64_C(1)<<V.timestamp_bits)-1;
    *ms=(double)((t[1]-t[0])&mask)*V.props.limits.timestampPeriod/1e6;
    return isfinite(*ms) && *ms>=0;
}
static int fg_profile_source_read(unsigned index, unsigned stage, double *ms) {
    if (stage>=FG_PROFILE_BASE)
        return fg_profile_read(F.profile_queries,fg_profile_source_query(index,stage),ms);
    unsigned query=(stage ? FG_SOURCES*stage+FG_OUTPUTS+index : index)*2;
    return fg_profile_read(F.queries,query,ms);
}
static void fg_profile_add(unsigned stage, double ms, int valid) {
    if (!valid) { F.profile_unavailable[stage]++; return; }
    F.profile_samples[stage]++;
    F.profile_sum[stage]+=ms;
    if (ms>F.profile_max[stage]) F.profile_max[stage]=ms;
}
static void fg_profile_source_publish(unsigned index) {
    fg_source *s=&F.source[index];
    if (!F.profile_queries || !s->profile_mask ||
        atomic_exchange_explicit(&s->profile_reported,1,memory_order_relaxed)) return;
    double ms[FG_PROFILE_SOURCE_STAGES]={0};
    uint32_t valid=0;
    for (unsigned stage=0;stage<FG_PROFILE_SOURCE_STAGES;stage++)
        if ((s->profile_mask&(UINT32_C(1)<<stage)) && fg_profile_source_read(index,stage,&ms[stage]))
            valid|=UINT32_C(1)<<stage;
    pthread_mutex_lock(&fg_stats_mu);
    for (unsigned stage=0;stage<FG_PROFILE_SOURCE_STAGES;stage++)
        if (s->profile_mask&(UINT32_C(1)<<stage))
            fg_profile_add(stage,ms[stage],!!(valid&(UINT32_C(1)<<stage)));
    pthread_mutex_unlock(&fg_stats_mu);
}
static void fg_profile_harvest(void) {
    if (!F.profile_queries) return;
    for (unsigned i=0;i<FG_SOURCES;i++) {
        fg_source *s=&F.source[i];
        if (!s->profile_mask || !s->retire || s->retire==UINT64_MAX ||
            atomic_load_explicit(&s->referenced,memory_order_acquire) ||
            atomic_load_explicit(&s->profile_reported,memory_order_relaxed) ||
            !mtl_wait(s->retire,0)) continue;
        fg_profile_source_publish(i);
    }
}
static void fg_profile_output_publish(unsigned slot) {
    fg_output *o=&F.output[slot];
    if (!F.profile_queries) return;
    double ms[FG_PROFILE_STAGES-FG_PROFILE_SOURCE_STAGES]={0};
    uint32_t valid=0;
    for (unsigned stage=FG_PROFILE_SOURCE_STAGES;stage<FG_PROFILE_STAGES;stage++)
        if ((o->profile_mask&(UINT32_C(1)<<(stage-FG_PROFILE_SOURCE_STAGES))) &&
            fg_profile_read(F.profile_queries,fg_profile_output_query(slot,stage),
                &ms[stage-FG_PROFILE_SOURCE_STAGES]))
            valid|=UINT32_C(1)<<(stage-FG_PROFILE_SOURCE_STAGES);
    pthread_mutex_lock(&fg_stats_mu);
    for (unsigned stage=FG_PROFILE_SOURCE_STAGES;stage<FG_PROFILE_STAGES;stage++) {
        uint32_t bit=UINT32_C(1)<<(stage-FG_PROFILE_SOURCE_STAGES);
        if (o->profile_mask&bit)
            fg_profile_add(stage,ms[stage-FG_PROFILE_SOURCE_STAGES],!!(valid&bit));
    }
    pthread_mutex_unlock(&fg_stats_mu);
}
/* Called with fg_stats_mu held at the existing 300-real reporting boundary. */
static void fg_profile_stats(void) {
    if (!F.profile_queries) return;
    static const char *const labels[FG_PROFILE_STAGES]={
        "real.source-preservation(scene+depth)", "real.extrapolation-warp",
        "real.replay-envelope(UI+pair+compose+terminal)", "real.UI-depth-copies",
        "real.authority-state-initialization(no-depth-copy)", "real.authority-final-copy+compare+readback",
        "real.encoded-fallback-preservation", "real.retained-native-scene-copy",
        "real.source-scene-copy", "real.source-depth-copy",
        "real.authority-final-depth-copy", "real.authority-final-VALUE-compare+range+readback",
        "real.pair-earlier-motion-bound", "real.pair-earlier-coarse4x4-reduce+X-dilate",
        "real.pair-earlier-coarse-Y-max+fullres-expand+weighted-census", "real.pair-earlier-census-readback",
        "real.pair-current-motion-bound", "real.pair-current-coarse4x4-reduce+X-dilate",
        "real.pair-current-coarse-Y-max+fullres-expand+weighted-census", "real.pair-current-census-readback",
        "real.bidirectional-reprojection+affine-UI", "real.UI-replay-black",
        "real.UI-replay-white", "real.guest-terminal-encoding",
        "synthetic.terminal-copy+authority-fallback", "synthetic.optional-post",
        "synthetic.presentation-output(AHB-or-blit+overlay+readback)"};
    LOGV("[framegen-profile] last 300 real: fenced stage subspans; real.* executes in real tickets, synthetic.* in synthetic tickets; replay/authority/preservation envelopes contain nested stages, NOT additive whole elapsed; reprojection+affine UI %s",
        vk_scene_post_enabled() ? "scope is native reprojection+affine UI from retained filtered real scenes; no midpoint AA/spatial filter" : "share one shader/draw");
    for (unsigned stage=0;stage<FG_PROFILE_STAGES;stage++) {
        uint64_t n=F.profile_samples[stage];
        if (n) LOGV("[framegen-profile] %s mean=%.6f ms max=%.6f ms samples=%llu unavailable=%llu",
            labels[stage],F.profile_sum[stage]/n,F.profile_max[stage],
            (unsigned long long)n,(unsigned long long)F.profile_unavailable[stage]);
        else LOGV("[framegen-profile] %s mean/max unavailable samples=0 unavailable=%llu",
            labels[stage],(unsigned long long)F.profile_unavailable[stage]);
    }
    memset(F.profile_samples,0,sizeof F.profile_samples);
    memset(F.profile_unavailable,0,sizeof F.profile_unavailable);
    memset(F.profile_sum,0,sizeof F.profile_sum);
    memset(F.profile_max,0,sizeof F.profile_max);
}
static void fg_budget(double ms) {
    if (vk_fg_interpolating() && !F.budget_override) return;
    if (ms > F.budget_ms) {
        LOGV("framegen GPU %.3f ms exceeds %.3f ms budget", ms, F.budget_ms);
        if (!vk_fg_interpolating()) fg_suspend("observed incremental GPU cost exceeded budget");
    }
}
/* Accounting callers hold fg_stats_mu. Audit only after the ordinary Vulkan
 * and external group finalizers have produced an immutable full interval. */
static void fg_prepared_budget_audit(void) {
    fg_qcom_prepared_policy *a=&F.prepared_budget;
    if (!fg_qcom_prepared_pending(a)) return;
    uint64_t now=fg_budget_after(F.budget_frame,1);
    for (unsigned i=0;i<FG_BUDGET_FRAMES;i++) {
        uint64_t missing=a->missing,overshoots=a->overshoots;
        if (!fg_qcom_prepared_audit(&F.budget,a,i,now)) continue;
        const fg_qcom_prepared_interval *c=&a->interval[i];
        int valid=a->missing==missing;
        LOGV("[framegen-prepared-budget-actual] charge=%llu children=%u predicted_real_ms=%.6f predicted_incremental_ms=%.6f predicted_total_ms=%.6f actual_valid=%d actual_real_ms=%.6f actual_incremental_ms=%.6f actual_total_ms=%.6f ceiling_ms=%.3f overshoot=%d audits=%llu overshoots=%llu missing=%llu backoff_intervals=%u cooldown_until=%llu (immutable ordinary-accounting aggregate; includes canceled/refused GPU work)",
            (unsigned long long)c->frame,c->children,c->predicted.real_ms,
            c->predicted.incremental_ms,c->predicted.total_ms,valid,
            valid ? c->actual.real_ms : NAN,valid ? c->actual.incremental_ms : NAN,
            valid ? c->actual.real_ms+c->actual.incremental_ms : NAN,FG_BUDGET_TARGET_MS,
            a->overshoots!=overshoots,(unsigned long long)a->audits,
            (unsigned long long)a->overshoots,(unsigned long long)a->missing,
            FG_QCOM_PREPARED_BACKOFF,(unsigned long long)a->cooldown_until);
        if (fg_adaptive_budget_selected() &&
            F.adaptive_budget_frames[c->frame%FG_BUDGET_FRAMES]==c->frame) {
            F.adaptive_budget_audits++; F.adaptive_budget_missing+=!valid;
            F.adaptive_budget_overshoots+=a->overshoots!=overshoots;
            LOGV("[framegen-adaptive-budget-actual] charge=%llu admission=stale-estimate children=%u actual_valid=%d actual_real_ms=%.6f actual_incremental_ms=%.6f actual_total_ms=%.6f ceiling_ms=%.3f overshoot=%d stale_audits=%llu stale_overshoots=%llu stale_missing=%llu backoff_intervals=%u cooldown_until=%llu (same immutable audit and back-off as every charge)",
                (unsigned long long)c->frame,c->children,valid,
                valid ? c->actual.real_ms : NAN,valid ? c->actual.incremental_ms : NAN,
                valid ? c->actual.real_ms+c->actual.incremental_ms : NAN,FG_BUDGET_TARGET_MS,
                a->overshoots!=overshoots,(unsigned long long)F.adaptive_budget_audits,
                (unsigned long long)F.adaptive_budget_overshoots,(unsigned long long)F.adaptive_budget_missing,
                FG_QCOM_PREPARED_BACKOFF,(unsigned long long)a->cooldown_until);
        }
    }
}
/* Slot completion publishes outside nvn's g_mu. This mutex never encloses
   mtl_wait or frontend callbacks, so it cannot invert backend ticket locking. */
void vk_fg_gpu_sample(uint64_t ticket, double ms, int valid) {
    if (!vk_fg_enabled() || !vk_fg_interpolating()) return;
    pthread_mutex_lock(&fg_stats_mu);
    if (FQ.budget_enabled) fg_budget_external_sample(&F.budget,&FQ.budget,ticket,ms,valid);
    else fg_budget_sample(&F.budget,ticket,ms,valid);
    fg_prepared_budget_audit();
    pthread_mutex_unlock(&fg_stats_mu);
}
void vk_fg_synthetic_range(uint64_t first, uint64_t last) {
    if (!vk_fg_enabled() || !vk_fg_interpolating()) return;
    pthread_mutex_lock(&fg_stats_mu);
    fg_budget_synthetic(&F.budget,first,last);
    pthread_mutex_unlock(&fg_stats_mu);
}
void vk_fg_qcom_cost_begin(uint64_t frame) {
    pthread_mutex_lock(&fg_stats_mu);
    FQ.budget_enabled=1;
    if (vk_fg_adaptive()) fg_budget_external_group_begin(&FQ.budget,
        &FQ.adaptive_cost[frame%FG_BUDGET_FRAMES],frame);
    else fg_budget_external_begin(&FQ.budget,frame);
    pthread_mutex_unlock(&fg_stats_mu);
}
void vk_fg_qcom_cost_end(uint64_t frame,double ms,int valid) {
    pthread_mutex_lock(&fg_stats_mu);
    if (vk_fg_adaptive()) fg_budget_external_group_end(&F.budget,&FQ.budget,
        &FQ.adaptive_cost[frame%FG_BUDGET_FRAMES],frame,ms,valid);
    else fg_budget_external_end(&F.budget,&FQ.budget,frame,ms,valid);
    for (unsigned i=0;i<FG_OUTPUTS;i++) {
        vk_fg_generation *g=&fg_generation[i];
        int state=vk_fg_generation_state(g);
        if ((state==VK_FG_GENERATION_RECORDING || state==VK_FG_GENERATION_SUBMITTED) &&
            g->charge_frame==frame && isnan(g->external_ms))
            g->external_ms=valid && isfinite(ms) && ms>=0 ? ms : -1;
    }
    fg_prepared_budget_audit();
    pthread_mutex_unlock(&fg_stats_mu);
}
uint64_t vk_fg_real_frame_id(void) {
    if (!vk_fg_enabled() || !vk_fg_interpolating()) return 0;
    pthread_mutex_lock(&fg_stats_mu);
    uint64_t frame=F.budget_frame+1;
    pthread_mutex_unlock(&fg_stats_mu);
    return frame;
}
void vk_fg_real_feedback(uint64_t frame, uint64_t desired, uint64_t display, uint64_t refresh) {
    if (!frame || !desired || !display || !refresh || !vk_fg_enabled() ||
        !vk_fg_interpolating() || display<desired) return;
    /* A signaled framework present fence at least one refresh after the exact
       immutable real target is a missed target, not a guaranteed scanout time.
       Missing/early fences and callback/dequeue latency never become lateness. */
    int late=display-desired>=refresh;
    pthread_mutex_lock(&fg_stats_mu);
    F.budget_feedback_samples++;
    F.budget_feedback_late+=late;
    int correlated=0;
    if (F.diag && late && F.budget.feedback_gpu_only &&
        F.budget.feedback_frames[frame % FG_BUDGET_FRAMES] != frame) {
        uint64_t first=frame > 2 ? frame-2 : 1;
        uint64_t last=frame == UINT64_MAX ? UINT64_MAX : frame+1;
        for (uint64_t id=first;;id++) {
            correlated |= F.budget.reserved_frames[id % FG_BUDGET_FRAMES] == id;
            if (id == last) break;
        }
    }
    int paused=fg_budget_feedback(&F.budget,frame,late);
    F.budget_feedback_pauses+=paused;
    if (correlated) {
        const fg_budget_frame *f=&F.budget.frames[frame % FG_BUDGET_FRAMES];
        int exact=f->frame == frame && f->status == FG_BUDGET_VALID;
        uint64_t now=F.budget.last_frame > F.budget.last_reserved ? F.budget.last_frame : F.budget.last_reserved;
        LOGV("[framegen-feedback] real=%llu cadence-late=1 legacy-would-pause=1 gpu-pause=%d exact=%d age=%llu real_ms=%.6f incremental_ms=%.6f total_ms=%.6f ceiling_ms=%.3f",
            (unsigned long long)frame,paused,exact,(unsigned long long)(now > frame ? now-frame : 0),
            exact ? f->real_ms : NAN,exact ? f->incremental_ms : NAN,
            exact ? f->real_ms+f->incremental_ms : NAN,FG_BUDGET_TARGET_MS);
    }
    pthread_mutex_unlock(&fg_stats_mu);
}
int vk_fg_job_trace_enabled(void) {
    return atomic_load_explicit(&F.job_trace,memory_order_relaxed)!=0;
}
_Thread_local fg_dispatch_trace *vk_fg_dispatch_trace_current;
void fg_dispatch_trace_event(const fg_dispatch_trace *t,const char *event,
        const char *stage,const char *reason) {
    if (!t || !t->enabled) return;
    LOGV("[adaptive-dispatch-%s] source=%llu charge=%llu output=%u previous-index=%u source-index=%u phase=%.9g deadline-ns=%llu stage=%s reason=%s decision-ns=%llu clock=host-monotonic budget-known=%d budget-frame=%llu budget-status=%d budget-reason=%s probe=%d prepared-cooling=%d prepared-capacity=%d reserved-frame=%llu cooldown-until=%llu qcom-warmed=%d qcom-suspended=%d qcom-cooling=%d qcom-quarantined=%d qcom-admitted=%d first-ticket=%llu ticket=%llu range-observed=%d charge-committed=%d charge-status=%s charge-authority=%s final-gpu-done=UNKNOWN",
        event,(unsigned long long)t->source,(unsigned long long)t->charge,t->output,
        t->previous,t->current,t->phase,(unsigned long long)t->deadline,stage,reason,
        (unsigned long long)host_now_ns(),t->budget_known,(unsigned long long)t->budget_frame,
        t->budget_status,fg_dispatch_budget_reason(t->budget_reason),t->probe,t->cooling,t->capacity,
        (unsigned long long)t->reserved_frame,(unsigned long long)t->cooldown_until,
        t->qcom_warmed,t->qcom_suspended,t->qcom_cooling,t->qcom_quarantined,t->qcom_admitted,
        (unsigned long long)t->first_ticket,(unsigned long long)t->ticket,t->range_observed,
        t->charge_status==1,fg_dispatch_charge_status(t->charge_status),
        t->charge_status ? "prepared-range-charge-result" : "prospective-until-range-charge");
}
/* Called under fg_stats_mu, copying the decision already queried by the real
 * guard. -1 means an original short circuit did not query that fact. */
static void fg_dispatch_budget_observe(void) {
    fg_dispatch_trace *t=vk_fg_dispatch_trace_current;
    if (!t || !t->enabled) return;
    t->budget_known=1; t->budget_frame=F.budget_pre_frame;
    t->budget_status=F.budget_pre.status; t->budget_reason=F.budget_pre.reason;
    t->probe=F.budget_pre.probe; t->reserved_frame=F.budget_reserved_frame;
    t->cooldown_until=F.prepared_budget.cooldown_until;
}
static int fg_budget_reserve_work(void) {
    if (!vk_fg_interpolating() || F.budget_override) return 1;
    int lsfg=vk_fg_lsfg_active();
    int stale=!lsfg && fg_adaptive_budget();
    pthread_mutex_lock(&fg_stats_mu);
    uint64_t frame=F.budget_frame+1;
    int accepted=F.budget_reserved_frame==frame;
    if (!accepted) {
        F.budget_pre=lsfg ? fg_budget_lsfg_reserve(&F.budget,frame) :
            stale ? fg_budget_reserve_stale(&F.budget,frame) : fg_budget_reserve(&F.budget,frame);
        F.budget_pre_frame=frame;
        accepted=F.budget_pre.status==FG_BUDGET_ACCEPT;
        if (accepted) F.budget_reserved_frame=frame;
        if (accepted && stale) {
            F.adaptive_budget_frames[frame%FG_BUDGET_FRAMES]=F.budget_pre.stale_incremental ? frame : 0;
            F.adaptive_budget_reservations+=F.budget_pre.stale_incremental;
        }
    }
    if (vk_fg_adaptive() &&
        (FG_DISPATCH_FACT(vk_fg_dispatch_trace_current,cooling,fg_qcom_prepared_cooling(&F.prepared_budget,frame)) ||
        !FG_DISPATCH_FACT(vk_fg_dispatch_trace_current,capacity,fg_qcom_prepared_capacity(&F.prepared_budget,frame))))
        accepted=0;
    fg_dispatch_budget_observe();
    pthread_mutex_unlock(&fg_stats_mu);
    return accepted;
}
static int fg_budget_pre_admit(void) {
    if (!vk_fg_interpolating() || F.budget_override) return 1;
    int lsfg=vk_fg_lsfg_active();
    int stale=!lsfg && fg_adaptive_budget();
    pthread_mutex_lock(&fg_stats_mu);
    uint64_t frame=F.budget_frame+1;
    if (F.budget_pre_frame != frame) {
        F.budget_pre_frame=frame;
        F.budget_pre=lsfg ? fg_budget_lsfg_admit(&F.budget,frame) :
            stale ? fg_budget_admit_stale(&F.budget,frame) : fg_budget_admit(&F.budget,frame);
        if (F.budget_pre.status != FG_BUDGET_ACCEPT) {
            F.budget_pre_skips[F.budget_pre.status == FG_BUDGET_OVERLOAD]++;
            F.budget_skips++;
        }
        if (F.budget_pre.stale_incremental) {
            const fg_budget_decision *d=&F.budget_pre;
            int ok=d->status==FG_BUDGET_ACCEPT;
            F.adaptive_budget_admits+=ok; F.adaptive_budget_refusals+=!ok;
            LOGV("[framegen-adaptive-budget-decision] frame=%llu status=%u reason=%s real_p90_ms=%.6f stale_incremental_p90_ms=%.6f total_ms=%.6f ceiling_ms=%.3f newest_real=%llu newest_incremental=%llu incremental_age=%llu real_samples=%u incremental_samples=%u stale_admits=%llu stale_refusals=%llu (budget decision only; prepared cooling/capacity still apply)",
                (unsigned long long)frame,d->status,fg_dispatch_budget_reason((int)d->reason),
                d->real_ms,d->incremental_ms,d->total_ms,FG_BUDGET_TARGET_MS,
                (unsigned long long)d->newest_real_frame,(unsigned long long)d->newest_incremental_frame,
                (unsigned long long)(frame-d->newest_incremental_frame),d->real_samples,d->incremental_samples,
                (unsigned long long)F.adaptive_budget_admits,(unsigned long long)F.adaptive_budget_refusals);
        }
    }
    int accepted=F.budget_pre.status == FG_BUDGET_ACCEPT;
    if (vk_fg_adaptive() &&
        (FG_DISPATCH_FACT(vk_fg_dispatch_trace_current,cooling,fg_qcom_prepared_cooling(&F.prepared_budget,frame)) ||
        !FG_DISPATCH_FACT(vk_fg_dispatch_trace_current,capacity,fg_qcom_prepared_capacity(&F.prepared_budget,frame))))
        accepted=0;
    fg_dispatch_budget_observe();
    pthread_mutex_unlock(&fg_stats_mu);
    return accepted;
}

/* Walking-owned publication after exact completion harvest and engine ticks.
 * Do not use admission's stale-incremental NaN as zero. Its historical p90
 * points at an authenticated completed interval; sparse probes refresh it. */
static void fg_drs_tick(void) {
    if (!F.drs_on && !F.diag) return;
    uint64_t now=host_now_ns();
    int scene=F.ready && F.current>=0 && !F.invalid;
    int active=F.drs_on && vk_fg_locked() && vk_fg_enabled() && scene;
    if (vk_fg_qcom_requested()) active=active && vk_fg_qcom_active();
    if (vk_fg_lsfg_requested()) active=active && vk_fg_lsfg_active();
    pthread_mutex_lock(&fg_stats_mu);
    fg_budget_decision d=fg_budget_admit(&F.budget,F.budget_frame+1);
    double incremental=NAN;
    if (d.incremental_samples) {
        const fg_budget_frame *f=&F.budget.frames[d.incremental_frame % FG_BUDGET_FRAMES];
        if (f->frame==d.incremental_frame && f->status==FG_BUDGET_VALID && f->synthetic)
            incremental=f->incremental_ms;
    }
    active=active && d.real_samples>=4 && d.status!=FG_BUDGET_MISSING &&
        d.reason!=FG_BUDGET_REASON_FEEDBACK;
    pthread_mutex_unlock(&fg_stats_mu);
    double target=fg_drs_update(&F.drs,now,active,d.newest_incremental_frame,incremental);
    if (target!=F.drs_published) {
        runtime_drs_framegen_budget(target);
        F.drs_published=target;
    }
    uint64_t level=F.diag ? runtime_drs_framegen_level() : 0;
    if (F.diag && (level!=F.drs_level || !F.drs_log_ns || now-F.drs_log_ns>=FG_DRS_SECOND)) {
        uint32_t bits=(uint32_t)level; float scale; memcpy(&scale,&bits,4);
        fg_source *s=scene ? &F.source[F.current] : NULL;
        LOGV("[drs-framegen] real=%llu time_ns=%llu enabled=%d active=%d target_ms=%.6f native_budget=%d incremental_ewma_ms=%.6f real_p90_ms=%.6f incremental_p90_ms=%.6f reason=%u level_valid=%d level=%d scale=%.6f scene=%ux%u canvas=%ux%u transition=%d (native priority -1=full; target is comparison cap, not GPU measurement)",
            (unsigned long long)F.budget_frame,(unsigned long long)now,F.drs_on,F.drs.active,
            F.drs.active?target:FG_BUDGET_TICK_MS,!F.drs.active,
            F.drs.active?F.drs.incremental_ms:NAN,d.real_ms,incremental,d.reason,
            level!=0,(int32_t)(level>>32),scale,s?s->depth.w:0,s?s->depth.h:0,
            s?s->scene.w:0,s?s->scene.h:0,level!=F.drs_level);
        F.drs_level=level; F.drs_log_ns=now;
    }
}
/* Only call after a covering fence, either on presentation completion or when
   the recorder reclaims an unreferenced source. There is no recording-time wait. */
static int fg_authority_read(fg_source *s, uint32_t result[8]) {
    memset(result,0,32);
    if (!s->authority_snapshot) return 1;
    if (!s->authority_compared || !s->authority_readback.mapped) return 0;
    if (!s->authority_readback.coherent) {
        VkMappedMemoryRange r={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=s->authority_readback.memory,.size=VK_WHOLE_SIZE};
        if (!fg_result(vkInvalidateMappedMemoryRanges(V.dev,1,&r),"authority readback invalidation")) return 0;
    }
    memcpy(result,s->authority_readback.mapped,s->authority_readback.size);
    uint64_t pixels=(uint64_t)s->authority_w*s->authority_h;
    return result[0] == s->authority_w && result[1] == s->authority_h &&
        result[2] <= pixels && result[3] == 0 && result[7] <= pixels &&
        (s->experimental_spans || result[7] == 0);
}
/* Pair readback is owned by the CURRENT source and pinned by its output.
   Reading source A's earlier pair never aliases the A->B masks written in B.
   Queue order alone would not protect CPU reads after A's GPU fence. */
static int fg_pair_read(fg_source *s, uint32_t result[2][FG_AUTHORITY_WORDS]) {
    memset(result,0,2*FG_AUTHORITY_WORDS*4);
    if (s->pair_source < 0) return !s->authority_snapshot;
    fg_source *sources[2]={&F.source[s->pair_source],s};
    int masked=sources[0]->authority_snapshot || sources[1]->authority_snapshot;
    if (!masked) return 1;
    if (!s->pair_readback.mapped) return 0;
    if (!s->pair_readback.coherent) {
        VkMappedMemoryRange range={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=s->pair_readback.memory,.size=VK_WHOLE_SIZE};
        if (!fg_result(vkInvalidateMappedMemoryRanges(V.dev,1,&range),"pair authority readback invalidation")) return 0;
    }
    for (unsigned side=0;side<2;side++) {
        fg_source *source=sources[side];
        if (!source->authority_snapshot) continue;
        if (!source->authority_compared) return 0;
        memcpy(result[side],(const uint32_t *)s->pair_readback.mapped+side*FG_AUTHORITY_WORDS,FG_AUTHORITY_WORDS*4);
        uint32_t *r=result[side], extent[2]={source->authority_w,source->authority_h};
        uint64_t pixels=(uint64_t)extent[0]*extent[1];
        if (r[0]!=extent[0] || r[1]!=extent[1] || r[2]>r[3] || r[3]>pixels) return 0;
        for (unsigned axis=0;axis<2;axis++) {
            float bound;
            memcpy(&bound,&r[8+axis],sizeof bound);
            if (!isfinite(bound) || bound<0 || bound>FG_MOTION_CAP ||
                !r[10+axis] || r[10+axis]>extent[axis]) return 0;
        }
    }
    return 1;
}
static int fg_pair_accept(fg_source *s) {
    uint32_t result[2][FG_AUTHORITY_WORDS];
    if (!fg_pair_read(s,result)) return -1;
    for (unsigned side=0;side<2;side++) if (result[side][0] &&
        (double)result[side][3]*100.0 > F.unknown_limit*(double)result[side][0]*result[side][1]) return 0;
    return 1;
}
static void fg_pair_stats(fg_source *s) {
    uint32_t result[2][FG_AUTHORITY_WORDS];
    if (!fg_pair_read(s,result)) return;
    pthread_mutex_lock(&fg_stats_mu);
    for (unsigned side=0;side<2;side++) {
        uint32_t *r=result[side];
        if (!r[0]) continue;
        double pixels=(double)r[0]*r[1], raw=r[2]/pixels, dilated=r[3]/pixels;
        F.authority_samples++;
        F.raw_unknown_sum+=raw; F.dilated_unknown_sum+=dilated;
        if (raw>F.raw_unknown_max) F.raw_unknown_max=raw;
        if (dilated>F.dilated_unknown_max) F.dilated_unknown_max=dilated;
        F.motion_samples++;
        for (unsigned axis=0;axis<2;axis++) {
            float bound;
            memcpy(&bound,&r[8+axis],sizeof bound);
            F.motion_sum[axis]+=bound; F.radius_sum[axis]+=r[10+axis];
            if (bound>F.motion_max[axis]) F.motion_max[axis]=bound;
            if (r[10+axis]>F.radius_max[axis]) F.radius_max[axis]=r[10+axis];
        }
    }
    pthread_mutex_unlock(&fg_stats_mu);
}
static unsigned fg_authority_span_query(unsigned index) {
    /* Eight source brackets, including retained native scene preservation. */
    return (FG_SOURCES*8+FG_OUTPUTS)*2+index*FG_AUTHORITY_SPAN_QUERIES;
}
static int fg_authority_span_timestamp(unsigned index, double *ms) {
    fg_source *s=&F.source[index];
    *ms=0;
    unsigned count=2*(s->span_begins+s->span_ends);
    if (!count) return 1;
    uint64_t t[FG_AUTHORITY_SPAN_QUERIES];
    if (count>FG_AUTHORITY_SPAN_QUERIES ||
        vkGetQueryPoolResults(V.dev,F.queries,fg_authority_span_query(index),count,
            count*sizeof *t,t,sizeof *t,VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS) {
        fg_suspend("ordered authority span GPU cost unavailable after completion fence"); return 0;
    }
    uint64_t mask=V.timestamp_bits>=64 ? UINT64_MAX : (UINT64_C(1)<<V.timestamp_bits)-1;
    for (unsigned i=0;i<count;i+=2)
        *ms+=(double)((t[i+1]-t[i])&mask)*V.props.limits.timestampPeriod/1e6;
    return 1;
}
static int fg_authority_timestamp(unsigned index, double *ms) {
    fg_source *s=&F.source[index];
    *ms=0;
    if (!s->authority_snapshot) return 1;
    double init, compare=0, spans;
    if (!fg_timestamp((FG_SOURCES*4+FG_OUTPUTS+index)*2,&init) ||
        (s->authority_compared && !fg_timestamp((FG_SOURCES*5+FG_OUTPUTS+index)*2,&compare)) ||
        !fg_authority_span_timestamp(index,&spans)) return 0;
    *ms=init+compare+spans;
    uint32_t result[8]={0};
    if (s->authority_compared && !fg_authority_read(s,result)) return 0;
    /* Both threads can observe a retained source; publish each sample once.
       Source reuse is excluded by referenced+retire, not by this stats flag. */
    if (!atomic_exchange_explicit(&s->authority_reported,1,memory_order_relaxed)) {
        pthread_mutex_lock(&fg_stats_mu);
        if (s->authority_compared) {
            F.authority_cost_samples++;
            F.authority_sum_ms+=*ms;
            if (*ms > F.authority_max_ms) F.authority_max_ms=*ms;
        } else {
            F.init_only_samples++;
            F.init_only_sum_ms+=*ms;
            if (*ms > F.init_only_max_ms) F.init_only_max_ms=*ms;
        }
        if (s->span_begins) {
            F.span_cost_samples++;
            F.span_sum_ms+=spans;
            if (spans>F.span_max_ms) F.span_max_ms=spans;
        }
        if (s->experimental_spans) {
            F.uncertified_sources++;
            F.uncertified_compared+=!!s->authority_compared;
            F.uncertified_pixels+=result[7];
        }
        pthread_mutex_unlock(&fg_stats_mu);
    }
    return 1;
}
static void fg_authority_harvest(void) {
    if (!vk_fg_interpolating() || !F.ready) return;
    for (unsigned i=0;i<FG_SOURCES;i++) {
        fg_source *s=&F.source[i];
        /* Unreferenced warmup/refused sources have no completion callback.
           A zero-time fence probe can harvest them, including retained history.
           Referenced sources are harvested by their output completion instead. */
        if (!s->authority_snapshot || !s->retire || s->retire==UINT64_MAX ||
            atomic_load_explicit(&s->referenced,memory_order_acquire) ||
            atomic_load_explicit(&s->authority_reported,memory_order_relaxed) ||
            !mtl_wait(s->retire,0)) continue;
        double ignored;
        (void)fg_authority_timestamp(i,&ignored);
    }
}
static int fg_source_timestamp(unsigned index, double *ms) {
    *ms=0;
    if (F.source[index].timed && !fg_timestamp(index*2, ms)) return 0;
    if (F.source[index].prepared && !vk_fg_interpolating()) {
        double warp;
        if (!fg_timestamp((FG_SOURCES+FG_OUTPUTS+index)*2, &warp)) return 0;
        *ms += warp;
    }
    if (F.source[index].transferred || F.source[index].ui_ready) {
        /* Interpolation's replay endpoint is the terminal transfer, so this
           single bracket also contains pair dilation and midpoint composition. */
        double replay;
        if (!fg_timestamp((FG_SOURCES*2+FG_OUTPUTS+index)*2, &replay)) return 0;
        *ms += replay;
    }
    if (F.source[index].replay_depth) {
        double depth_copy;
        if (!fg_timestamp((FG_SOURCES*3+FG_OUTPUTS+index)*2, &depth_copy)) return 0;
        *ms += depth_copy;
    }
    if (F.source[index].real_ready) {
        double real_copy;
        if (!fg_timestamp((FG_SOURCES*6+FG_OUTPUTS+index)*2,&real_copy)) return 0;
        *ms+=real_copy;
    }
    if (F.source[index].native_scene_ready) {
        double native_copy;
        if (!fg_timestamp((FG_SOURCES*7+FG_OUTPUTS+index)*2,&native_copy)) return 0;
        *ms+=native_copy;
    }
    double authority;
    if (!fg_authority_timestamp(index,&authority)) return 0;
    *ms+=authority;
    return 1;
}
static int fg_scene_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 1;
        default: return 0;
    }
}
static int fg_subresource(vk_tex *x, unsigned level, unsigned layer) {
    return x && vk_tex_image(x) && !vk_tex_is3d(x) && vk_tex_samples(x) == 1 &&
        level < vk_tex_levels(x) && layer < vk_tex_layers(x);
}
static int fg_supports(VkFormat format, VkFormatFeatureFlags required) {
    VkFormatProperties p;
    V.get_format_props(V.pd, format, &p);
    return (p.optimalTilingFeatures & required) == required;
}
static void fg_copy_image(VkCommandBuffer cb, VkImage source, unsigned level, unsigned layer, fg_image *dest) {
    VkImageSubresourceRange src_range = {dest->aspect,level,1,layer,1}, dst_range = {dest->aspect,0,1,0,1};
    fg_barrier(cb, source, src_range, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    fg_barrier(cb, dest->image, dst_range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (unsigned aspect = 1; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) if (dest->aspect & aspect) {
        VkImageCopy region = {.srcSubresource={aspect,level,layer,1}, .dstSubresource={aspect,0,0,1},
            .extent={dest->w,dest->h,1}};
        vkCmdCopyImage(cb, source, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    fg_barrier(cb, dest->image, dst_range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    fg_barrier(cb, source, src_range, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    dest->initialized=1;
}
static void fg_copy(VkCommandBuffer cb, vk_tex *source, unsigned level, unsigned layer, fg_image *dest) {
    fg_copy_image(cb,vk_tex_image(source),level,layer,dest);
}
static int fg_source_acquire(void) {
    for (unsigned i=0; i<FG_SOURCES; i++) {
        fg_source *s=&F.source[i];
        if ((vk_fg_interpolating() && F.history == (int)i) ||
            atomic_load_explicit(&s->referenced,memory_order_acquire) ||
            (s->retire && !mtl_wait(s->retire,0))) continue;
        if (s->timed || (s->authority_snapshot &&
            !atomic_load_explicit(&s->authority_reported,memory_order_relaxed))) {
            double ms;
            if (s->timed ? !fg_source_timestamp(i,&ms) : !fg_authority_timestamp(i,&ms)) return -1;
            fg_budget(ms);
            if (!vk_fg_enabled()) return -1;
        }
        fg_profile_source_publish(i);
        fg_writer_recycle(i);
        s->profile_mask=0;
        atomic_store_explicit(&s->profile_reported,0,memory_order_relaxed);
        s->timed=s->prepared=s->transferred=s->replay_depth=s->ui_ready=s->real_ready=s->native_scene_ready=0;
        s->lean=s->real_skipped=0;
        s->authority_snapshot=s->authority_compared=0;
        s->span_begins=s->span_ends=s->experimental_spans=0;
        s->span_open=s->span_failed=s->span_experimental=0;
        atomic_store_explicit(&s->authority_reported,0,memory_order_relaxed);
        s->authority_image=VK_NULL_HANDLE;
        s->pair_source=-1;
        s->retire=0;
        return (int)i;
    }
    return -1;
}
static const char *const fg_authority_failure_names[FG_AUTH_FAILURES]={
    "disabled","init","invalid","current-capture","attempted-mismatch","subresource",
    "limits","authority-init","source-unavailable","buffers","descriptor-pool",
    "descriptor-reset","descriptor-set","injected"
};
/* Observation only. mtl_wait(ticket,0) reads the published done counter; it
 * neither waits nor runs completion/posts (nvn_vk.c). Never acquire/recycle
 * sources or create resources to explain a refusal. Recorder owns this state. */
static int fg_authority_begin_failure(unsigned reason, VkCommandBuffer cb,
        vk_tex *depth, unsigned level, unsigned layer, int invalidate) {
    if (F.job_trace) {
        uint64_t count=++F.authority_failures[reason], real=F.budget_frame+1;
        int subresource=depth && level<vk_tex_levels(depth) && layer<vk_tex_layers(depth);
        LOGV("[framegen-authority-begin-failure] real=%llu now-ns=%llu reason=%s count=%llu ready=%d invalid=%d current=%d authority=%d attempted=%d history=%d previous-valid=%d have-camera=%d reset-pending=%d command=%d depth=%#llx level=%u layer=%u width=%u height=%u format=%u aspect=%u; observation-only",
            (unsigned long long)real,(unsigned long long)host_now_ns(),
            fg_authority_failure_names[reason],(unsigned long long)count,
            F.ready,F.invalid,F.current,F.authority,F.authority_attempted,F.history,
            F.previous_valid,F.have_camera,atomic_load_explicit(&fg_reset_requested,memory_order_relaxed),
            cb!=VK_NULL_HANDLE,(unsigned long long)(uintptr_t)(depth ? vk_tex_image(depth) : VK_NULL_HANDLE),
            level,layer,subresource ? vk_tex_width(depth,level) : 0,
            subresource ? vk_tex_height(depth,level) : 0,
            depth ? (unsigned)vk_tex_format(depth) : 0,depth ? vk_tex_aspect(depth) : 0);
        for (unsigned i=0;i<FG_SOURCES;i++) {
            const fg_source *s=&F.source[i];
            uint64_t retire=s->retire;
            int retired=!F.ready ? -3 : !retire ? -1 : retire==UINT64_MAX ? -2 : mtl_wait(retire,0);
            LOGV("[framegen-authority-source] real=%llu reason=%s count=%llu slot=%u history=%d references=%d retire=%llu retired=%d allocated=%d snapshot=%d compared=%d span-open=%u span-failed=%u; retired-minus1-unsealed-minus2-missing-minus3-uninitialized",
                (unsigned long long)real,fg_authority_failure_names[reason],(unsigned long long)count,
                i,F.history==(int)i,atomic_load_explicit(&s->referenced,memory_order_acquire),
                (unsigned long long)retire,retired,
                s->authority.buffer!=VK_NULL_HANDLE || s->scene.image!=VK_NULL_HANDLE,
                s->authority_snapshot,s->authority_compared,s->span_open,s->span_failed);
        }
    }
    if (invalidate) vk_fg_skip();
    return 0;
}
#ifdef TOTK_VK_TESTS
uint64_t vk_fg_test_authority_trace_count(const char *reason) {
    if (reason) for (unsigned i=0;i<FG_AUTH_FAILURES;i++)
        if (!strcmp(reason,fg_authority_failure_names[i])) return F.authority_failures[i];
    return 0;
}
#endif
static int fg_authority_matches(fg_source *s, vk_tex *depth, unsigned level, unsigned layer) {
    return fg_subresource(depth,level,layer) && s->authority_image == vk_tex_image(depth) &&
        s->authority_level == level && s->authority_layer == layer &&
        s->authority_w == vk_tex_width(depth,level) && s->authority_h == vk_tex_height(depth,level) &&
        vk_tex_aspect(depth) == VK_IMAGE_ASPECT_DEPTH_BIT && vk_tex_format(depth) == VK_FORMAT_D32_SFLOAT;
}
static void fg_authority_copy_rect(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer,
    fg_source *s, fg_buffer *buffer, VkDeviceSize offset,
    unsigned x, unsigned y, unsigned width, unsigned height) {
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_DEPTH_BIT,level,1,layer,1};
    fg_barrier(cb,vk_tex_image(depth),range,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy={.bufferOffset=offset+((VkDeviceSize)y*s->authority_w+x)*4,
        .bufferRowLength=s->authority_w,.bufferImageHeight=s->authority_h,
        .imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,level,layer,1},
        .imageOffset={(int32_t)x,(int32_t)y,0},.imageExtent={width,height,1}};
    vkCmdCopyImageToBuffer(cb,vk_tex_image(depth),VK_IMAGE_LAYOUT_GENERAL,buffer->buffer,1,&copy);
    fg_barrier(cb,vk_tex_image(depth),range,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
}
static void fg_authority_copy(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer,
                               fg_source *s, fg_buffer *buffer, VkDeviceSize offset) {
    fg_authority_copy_rect(cb,depth,level,layer,s,buffer,offset,0,0,s->authority_w,s->authority_h);
}

int vk_fg_writer_diag(void) {
    int on=atomic_load_explicit(&fg_writer_config,memory_order_relaxed);
    if (on<0) {
        const char *e=getenv("TOTK_FRAMEGEN_WRITER_DIAG");
        const char *diag=getenv("TOTK_FRAMEGEN_DIAG");
        on=vk_fg_interpolating() && diag && !strcmp(diag,"1") && e && !strcmp(e,"1");
        int expected=-1;
        if (atomic_compare_exchange_strong_explicit(&fg_writer_config,&expected,on,
                memory_order_relaxed,memory_order_relaxed)) {
            LOGV("[framegen-writer] startup WRITER_DIAG=%s (requires INTERP + DIAG=1 + exact WRITER_DIAG=1); %s",
                on ? "on" : "off",on ? "before/after D32 copies perturb real GPU budget; separate attribution arm, NOT clean timing arm" : "no writer diagnostic GPU work; normal timing arm");
        } else on=expected;
    }
    return on;
}
/* Diagnostic resource failures are unavailable data, NOT authority failures.
   No ordinary fg_result/fg_buffer_make here: those suspend interpolation. */
static int fg_writer_result(VkResult r, const char *operation) {
    if (r==VK_SUCCESS) return 1;
    LOGV("[framegen-writer] diagnostic unavailable: %s result=%d",operation,r);
    if (r==VK_ERROR_DEVICE_LOST) {
        vk_fg_skip();
        fg_suspend("writer diagnostic command device lost");
    }
    return 0;
}
static int fg_writer_buffer(fg_buffer *b, VkDeviceSize bytes, int host) {
    if (b->buffer && b->size==bytes) return 1;
    fg_buffer_free(b,host);
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (!fg_writer_result(vkCreateBuffer(V.dev,&ci,NULL,&b->buffer),"buffer")) goto fail;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev,b->buffer,&req);
    int type=vk_pick_type(req.memoryTypeBits,host ?
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (host && type<0) type=vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type<0) goto fail;
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
    if (!fg_writer_result(vk_memory_allocate(&ai,&b->memory,host ? VK_MEMORY_READBACK : VK_MEMORY_SNAPSHOT),"memory") ||
        !fg_writer_result(vkBindBufferMemory(V.dev,b->buffer,b->memory,0),"bind")) goto fail;
    if (host) {
        if (!fg_writer_result(vkMapMemory(V.dev,b->memory,0,VK_WHOLE_SIZE,0,&b->mapped),"map")) goto fail;
        b->coherent=!!(V.mp.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    b->size=bytes;
    return 1;
fail:
    fg_buffer_free(b,host);
    return 0;
}
static int fg_writer_init(void) {
    if (FW.pipeline && FW.queries) return 1;
    VkDescriptorSetLayoutBinding bindings[2];
    for (unsigned i=0;i<2;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo ds={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    if (!FW.dsl && !fg_writer_result(vkCreateDescriptorSetLayout(V.dev,&ds,NULL,&FW.dsl),"descriptor layout")) return 0;
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,32};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,
        .pSetLayouts=&FW.dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    if (!FW.layout && !fg_writer_result(vkCreatePipelineLayout(V.dev,&li,NULL,&FW.layout),"pipeline layout")) return 0;
    if (!FW.pipeline) {
        VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=sizeof framegen_writer_comp_spv,.pCode=framegen_writer_comp_spv};
        VkShaderModule module;
        if (!fg_writer_result(vkCreateShaderModule(V.dev,&mi,NULL,&module),"shader")) return 0;
        VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=module,.pName="main"},.layout=FW.layout};
        VkResult r=vk_pcache_compute(&ci,&FW.pipeline);
        vkDestroyShaderModule(V.dev,module,NULL);
        if (!fg_writer_result(r,"pipeline")) return 0;
    }
    VkQueryPoolCreateInfo qi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,
        .queryCount=FG_SOURCES*FG_WRITER_QUERIES};
    return FW.queries || fg_writer_result(vkCreateQueryPool(V.dev,&qi,NULL,&FW.queries),"queries");
}
static int fg_writer_fail(fg_writer_source *w, const char *reason, int overflow) {
    w->failed=1; w->open=0; w->overflow|=overflow;
    if (!w->failure) {
        w->failure=reason;
        LOGV("[framegen-writer] diagnostic incomplete: %s; ordinary depth authority unchanged",reason);
    }
    return 0;
}
static unsigned fg_writer_index(void) {
    return F.authority>=0 ? (unsigned)F.authority : F.current>=0 ? (unsigned)F.current : FG_SOURCES;
}
int vk_fg_writer_available(void) {
    if (!vk_fg_writer_diag()) return 0;
    unsigned index=fg_writer_index();
    return index<FG_SOURCES && !FW.source[index].failed;
}
void vk_fg_writer_poison(const char *reason) {
    if (!vk_fg_writer_diag()) return;
    unsigned index=fg_writer_index();
    if (index<FG_SOURCES && FW.source[index].open)
        (void)fg_writer_fail(&FW.source[index],reason,0);
}
static int fg_writer_same(const fg_writer_group *g, const char *vs, const char *fs,
                          const char *check, unsigned classification) {
    return g->classification==classification && !strcmp(g->vs,vs) &&
        !strcmp(g->fs,fs) && !strcmp(g->check,check);
}
static void fg_writer_endpoint(VkCommandBuffer cb, unsigned index) {
    fg_writer_source *w=&FW.source[index];
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,FW.queries,
        index*FG_WRITER_QUERIES+ ++w->endpoint);
}
int vk_fg_writer_begin(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer,
                       const char *vs, const char *fs, const char *check, unsigned classification) {
    if (!vk_fg_writer_diag()) return 1;
    unsigned index=fg_writer_index();
    if (index>=FG_SOURCES) {
        LOGV("[framegen-writer] diagnostic unavailable: writer without retained baseline");
        FW.failed++;
        return 0;
    }
    fg_writer_source *w=&FW.source[index]; fg_source *s=&F.source[index];
    if (w->failed) return 0;
    if (!cb || !s->authority_snapshot || s->authority_compared || w->open ||
        !fg_authority_matches(s,depth,level,layer))
        return fg_writer_fail(w,"span identity/state unsupported",0);
    vs=vs ? vs : "-"; fs=fs ? fs : "-"; check=check ? check : "no-camera-check";
    if (classification>VK_FG_WRITER_UNCERTIFIED || strlen(vs)>=80 || strlen(fs)>=80 || strlen(check)>=80)
        return fg_writer_fail(w,"exact identity exceeds bounded record",1);
    unsigned limit=FG_WRITER_SPANS;
#ifdef TOTK_VK_TESTS
    if (FW.test_span_limit) limit=FW.test_span_limit;
    if (FW.test_failure) return fg_writer_fail(w,"injected diagnostic allocation failure",0);
#endif
    if (w->spans>=limit) return fg_writer_fail(w,"span capacity exhausted (writers omitted)",1);
    unsigned group=0;
    /* Proven writers are observed too, solely to revoke stale last-value owners. */
    if (classification) {
        for (;group<w->groups;group++) if (fg_writer_same(&w->group[group],vs,fs,check,classification)) break;
        if (group==FG_WRITER_GROUPS) return fg_writer_fail(w,"group capacity exhausted (writers omitted)",1);
        if (group==w->groups) {
            fg_writer_group *g=&w->group[w->groups++];
            strcpy(g->vs,vs); strcpy(g->fs,fs); strcpy(g->check,check); g->classification=classification;
        }
        group++;
    }
    VkDeviceSize plane=(VkDeviceSize)s->authority_w*s->authority_h*4;
    if (!w->initialized) {
        VkDeviceSize bytes=4*plane+FG_WRITER_WORDS*4;
        if (bytes>V.props.limits.maxStorageBufferRange || !fg_writer_init() ||
            !fg_writer_buffer(&w->gpu,bytes,0) || !fg_writer_buffer(&w->readback,FG_WRITER_WORDS*4,1))
            return fg_writer_fail(w,"private GPU/readback storage unavailable",0);
        if (!w->pool) {
            VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
            VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
            if (!fg_writer_result(vkCreateDescriptorPool(V.dev,&pi,NULL,&w->pool),"private descriptor pool"))
                return fg_writer_fail(w,"private descriptor pool unavailable",0);
        }
        if (!fg_writer_result(vkResetDescriptorPool(V.dev,w->pool,0),"descriptor reset"))
            return fg_writer_fail(w,"private descriptor reset unavailable",0);
        VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=w->pool,
            .descriptorSetCount=1,.pSetLayouts=&FW.dsl};
        if (!fg_writer_result(vkAllocateDescriptorSets(V.dev,&ai,&w->set),"descriptor set"))
            return fg_writer_fail(w,"private descriptor set unavailable",0);
        VkDescriptorBufferInfo buffers[2]={{w->gpu.buffer,0,w->gpu.size},{s->authority.buffer,0,s->authority.size}};
        VkWriteDescriptorSet writes[2];
        for (unsigned i=0;i<2;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=w->set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
        vkUpdateDescriptorSets(V.dev,2,writes,0,NULL);
        vkCmdResetQueryPool(cb,FW.queries,index*FG_WRITER_QUERIES,FG_WRITER_QUERIES);
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,FW.queries,index*FG_WRITER_QUERIES);
        fg_buffer_barrier(cb,&w->gpu,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cb,w->gpu.buffer,2*plane,2*plane+FG_WRITER_WORDS*4,0);
        fg_buffer_barrier(cb,&w->gpu,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        uint32_t extent[2]={s->authority_w,s->authority_h};
        vkCmdUpdateBuffer(cb,w->gpu.buffer,4*plane,sizeof extent,extent);
        w->initialized=1;
    }
    fg_buffer_barrier(cb,&w->gpu,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fg_authority_copy(cb,depth,level,layer,s,&w->gpu,0);
    w->span_group[w->spans++]=group; w->open=1;
    fg_writer_endpoint(cb,index);
    return 1;
}
static void fg_writer_dispatch(VkCommandBuffer cb, unsigned index, unsigned phase) {
    fg_writer_source *w=&FW.source[index]; fg_source *s=&F.source[index];
    fg_buffer_barrier(cb,&w->gpu,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    uint32_t push[8]={phase,s->authority_w,s->authority_h,
        w->spans ? w->span_group[w->spans-1] : 0,w->spans ? w->spans-1 : 0,w->spans,w->groups,0};
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,FW.pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,FW.layout,0,1,&w->set,0,NULL);
    vkCmdPushConstants(cb,FW.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,push);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
    vkCmdDispatch(cb,(uint32_t)(((uint64_t)s->authority_w*s->authority_h+255)/256),1,1);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
}
int vk_fg_writer_end(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer, uint64_t draws) {
    if (!vk_fg_writer_diag()) return 1;
    unsigned index=fg_writer_index();
    if (index>=FG_SOURCES) return 0;
    fg_writer_source *w=&FW.source[index]; fg_source *s=&F.source[index];
    if (w->failed) return 0;
    if (!cb || !w->open || !fg_authority_matches(s,depth,level,layer))
        return fg_writer_fail(w,"after snapshot identity/state mismatch",0);
    fg_buffer_barrier(cb,&w->gpu,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fg_authority_copy(cb,depth,level,layer,s,&w->gpu,(VkDeviceSize)s->authority_w*s->authority_h*4);
    fg_writer_dispatch(cb,index,0);
    w->span_draws[w->spans-1]=draws; w->closed++; w->open=0;
    fg_writer_endpoint(cb,index);
    return 1;
}
static void fg_writer_readback(VkCommandBuffer cb, unsigned index) {
    fg_writer_source *w=&FW.source[index]; fg_source *s=&F.source[index];
    fg_buffer_barrier(cb,&w->gpu,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy={.srcOffset=(VkDeviceSize)s->authority_w*s->authority_h*16,.size=FG_WRITER_WORDS*4};
    vkCmdCopyBuffer(cb,w->gpu.buffer,w->readback.buffer,1,&copy);
    fg_buffer_barrier(cb,&w->readback,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
}
static void fg_writer_final(VkCommandBuffer cb, unsigned index) {
    if (!vk_fg_writer_diag()) return;
    fg_writer_source *w=&FW.source[index];
    if (!w->initialized || w->failed) return;
    if (w->open) { (void)fg_writer_fail(w,"final comparison reached pending span",0); return; }
    fg_writer_dispatch(cb,index,1);
    fg_writer_readback(cb,index);
    fg_writer_endpoint(cb,index);
    w->final=1;
}
static int fg_writer_read(unsigned index, uint32_t result[FG_WRITER_WORDS]) {
    fg_writer_source *w=&FW.source[index]; fg_source *s=&F.source[index];
    if (!w->initialized || !w->readback.mapped || w->open || w->failed || !w->final || w->closed!=w->spans) return 0;
    if (!w->readback.coherent) {
        VkMappedMemoryRange r={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=w->readback.memory,.size=VK_WHOLE_SIZE};
        if (!fg_writer_result(vkInvalidateMappedMemoryRanges(V.dev,1,&r),"readback invalidation")) return 0;
    }
    memcpy(result,w->readback.mapped,FG_WRITER_WORDS*4);
    uint64_t pixels=(uint64_t)s->authority_w*s->authority_h, owned=result[3];
    if (result[0]!=s->authority_w || result[1]!=s->authority_h || result[2]>pixels) return 0;
    for (unsigned i=0;i<w->spans;i++) if (result[4+i]>pixels) return 0;
    for (unsigned i=0;i<w->groups;i++) owned+=result[4+FG_WRITER_SPANS+i];
    return owned==result[2];
}
static void fg_writer_publish(unsigned index) {
    fg_writer_source *w=&FW.source[index];
    if ((!w->initialized && !w->failed) || atomic_exchange_explicit(&w->reported,1,memory_order_relaxed)) return;
    uint32_t result[FG_WRITER_WORDS]; int valid=fg_writer_read(index,result);
    FW.sources++; FW.failed+=!!w->failed; FW.overflow+=!!w->overflow; FW.incomplete+=!valid;
    FW.before+=w->spans; FW.after+=w->closed;
    if (!valid) {
        FW.timing_unavailable++;
        LOGV("[framegen-writer] fenced source=%u incomplete spans=%u closed=%u final=%u failed=%u overflow=%u reason=%s (no exclusive ownership claimed)",
            index,w->spans,w->closed,w->final,w->failed,w->overflow,w->failure ? w->failure : "no final authority comparison");
        return;
    }
    uint64_t t[2]; unsigned base=index*FG_WRITER_QUERIES;
    int timed=vkGetQueryPoolResults(V.dev,FW.queries,base,1,8,&t[0],8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS &&
        vkGetQueryPoolResults(V.dev,FW.queries,base+w->endpoint,1,8,&t[1],8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
    if (timed) {
        uint64_t mask=V.timestamp_bits>=64 ? UINT64_MAX : (UINT64_C(1)<<V.timestamp_bits)-1;
        double ms=(double)((t[1]-t[0])&mask)*V.props.limits.timestampPeriod/1e6;
        FW.timing_samples++; FW.envelope_sum+=ms;
        if (ms>FW.envelope_max) FW.envelope_max=ms;
    } else FW.timing_unavailable++;
    FW.unknown+=result[2]; FW.unowned+=result[3];
    for (unsigned i=0;i<w->groups;i++) {
        fg_writer_group *g=&w->group[i];
        g->residual=result[4+FG_WRITER_SPANS+i];
        for (unsigned span=0;span<w->spans;span++) if (w->span_group[span]==i+1) {
            g->gross+=result[4+span]; g->draws+=w->span_draws[span]; g->spans++;
        }
        unsigned j=0;
        for (;j<FW.groups;j++) if (fg_writer_same(&FW.group[j],g->vs,g->fs,g->check,g->classification)) break;
        if (j==FG_WRITER_GROUPS) {
            FW.aggregate_overflow++;
            LOGV("[framegen-writer] last300 aggregate group capacity exhausted; omitted vs=%s fs=%s check=%s class=%u gross-overlap=%llu final-residual=%llu",
                g->vs,g->fs,g->check,g->classification,(unsigned long long)g->gross,(unsigned long long)g->residual);
            continue;
        }
        if (j==FW.groups) { FW.group[j]=*g; FW.groups++; }
        else {
            FW.group[j].gross+=g->gross; FW.group[j].residual+=g->residual;
            FW.group[j].draws+=g->draws; FW.group[j].spans+=g->spans;
        }
    }
}
static void fg_writer_recycle(unsigned index) {
    if (!vk_fg_writer_diag()) return;
    fg_writer_source *w=&FW.source[index];
    fg_writer_publish(index); // caller has proven both retirement and no CPU/output pins
    memset(w->group,0,sizeof w->group);
    w->groups=w->spans=w->closed=w->endpoint=0;
    w->initialized=w->open=w->final=w->failed=w->overflow=0; w->failure=NULL;
    atomic_store_explicit(&w->reported,0,memory_order_relaxed);
}
static void fg_writer_harvest(void) {
    if (!vk_fg_writer_diag() || !F.ready) return;
    for (unsigned i=0;i<FG_SOURCES;i++) {
        fg_source *s=&F.source[i]; fg_writer_source *w=&FW.source[i];
        if ((!w->initialized && !w->failed) || atomic_load_explicit(&w->reported,memory_order_relaxed) ||
            !s->retire || s->retire==UINT64_MAX || !mtl_wait(s->retire,0)) continue;
        /* Writer CPU data is recorder-only. Source references protect the GPU
           allocation; zero-time covering ticket checks also protect readback. */
        fg_writer_publish(i);
    }
}
static void fg_writer_stats(void) {
    if (!vk_fg_writer_diag()) return;
    LOGV("[framegen-writer] last300 real: separate attribution arm, NOT clean timing arm; fenced-sources=%llu incomplete=%llu failed=%llu source-overflow=%llu aggregate-overflow=%llu before/after=%llu/%llu residual-unknown=%llu residual-unowned=%llu groups=%u (source coverage starts at ordinary baseline; gross changes overlap across spans, NOT exclusive coverage; residual owners require last-changed VALUE==final and authority unknown row bit; UNCERTIFIED exemptions excluded)",
        (unsigned long long)FW.sources,(unsigned long long)FW.incomplete,(unsigned long long)FW.failed,
        (unsigned long long)FW.overflow,(unsigned long long)FW.aggregate_overflow,
        (unsigned long long)FW.before,(unsigned long long)FW.after,(unsigned long long)FW.unknown,
        (unsigned long long)FW.unowned,FW.groups);
    for (unsigned i=0;i<FW.groups;i++) {
        fg_writer_group *g=&FW.group[i];
        LOGV("[framegen-writer] pair vs=%s fs=%s camera-check=%s authority=%s draws=%llu spans=%llu gross-changed-pixels-overlapping=%llu final-residual-last-changed-VALUE-owner-pixels=%llu",
            g->vs,g->fs,g->check,g->classification==VK_FG_WRITER_UNCERTIFIED ? "unproven-UNCERTIFIED-qualified" : "unproven",
            (unsigned long long)g->draws,(unsigned long long)g->spans,
            (unsigned long long)g->gross,(unsigned long long)g->residual);
    }
    if (FW.timing_samples) LOGV("[framegen-writer] GPU envelope first-before through final-census mean/max=%.6f/%.6f ms samples=%llu unavailable=%llu (includes instrumented real draws/intervening work; NOT incremental/additive; compare only against a separate WRITER_DIAG=0 timing arm)",
        FW.envelope_sum/FW.timing_samples,FW.envelope_max,(unsigned long long)FW.timing_samples,(unsigned long long)FW.timing_unavailable);
    else LOGV("[framegen-writer] GPU envelope unavailable samples=0 unavailable=%llu",(unsigned long long)FW.timing_unavailable);
    FW.groups=0; memset(FW.group,0,sizeof FW.group);
    FW.sources=FW.incomplete=FW.failed=FW.overflow=FW.aggregate_overflow=FW.unknown=FW.unowned=0;
    FW.before=FW.after=FW.timing_samples=FW.timing_unavailable=0; FW.envelope_sum=FW.envelope_max=0;
}
#ifdef TOTK_VK_TESTS
static int fg_test_authority_failure;
void vk_fg_test_authority_fail(int enabled) { fg_test_authority_failure=enabled; }
static int fg_test_uncertified_failure;
void vk_fg_test_uncertified_fail(int enabled) { fg_test_uncertified_failure=enabled; }
#endif
int vk_fg_authority_begin(VkCommandBuffer cb, vk_tex *depth, unsigned dl, unsigned da) {
    if (!vk_fg_enabled()) return fg_authority_begin_failure(FG_AUTH_FAIL_DISABLED,cb,depth,dl,da,0);
    fg_consume_reset();
    if (!fg_init()) return fg_authority_begin_failure(FG_AUTH_FAIL_INIT,cb,depth,dl,da,0);
    if (F.invalid) return fg_authority_begin_failure(FG_AUTH_FAIL_INVALID,cb,depth,dl,da,0);
    if (F.current >= 0) return fg_authority_begin_failure(FG_AUTH_FAIL_CURRENT,cb,depth,dl,da,1);
    if (F.authority_attempted) {
        if (cb && F.authority >= 0 && fg_authority_matches(&F.source[F.authority],depth,dl,da)) return 1;
        return fg_authority_begin_failure(FG_AUTH_FAIL_ATTEMPTED,cb,depth,dl,da,1);
    }
    F.authority_attempted=1;
    if (!vk_fg_interpolating() || !cb || !fg_subresource(depth,dl,da) ||
        vk_tex_format(depth) != VK_FORMAT_D32_SFLOAT || vk_tex_aspect(depth) != VK_IMAGE_ASPECT_DEPTH_BIT) {
        return fg_authority_begin_failure(FG_AUTH_FAIL_SUBRESOURCE,cb,depth,dl,da,1);
    }
#ifdef TOTK_VK_TESTS
    if (fg_test_authority_failure) return fg_authority_begin_failure(FG_AUTH_FAIL_INJECTED,cb,depth,dl,da,1);
#endif
    uint32_t w=vk_tex_width(depth,dl), h=vk_tex_height(depth,dl);
    VkDeviceSize plane=(VkDeviceSize)w*h*4, bytes=FG_AUTHORITY_WORDS*4+plane*2;
    if (!w || !h || bytes > V.props.limits.maxStorageBufferRange ||
        plane*4 > V.props.limits.maxStorageBufferRange ||
        h > V.props.limits.maxComputeWorkGroupCount[0] ||
        ((uint64_t)w*h+255)/256 > V.props.limits.maxComputeWorkGroupCount[0] ||
        V.props.limits.maxComputeWorkGroupInvocations < 256 || V.props.limits.maxComputeWorkGroupSize[0] < 256 ||
        !fg_supports(VK_FORMAT_D32_SFLOAT,VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        return fg_authority_begin_failure(FG_AUTH_FAIL_LIMITS,cb,depth,dl,da,1);
    if (!fg_authority_init()) return fg_authority_begin_failure(FG_AUTH_FAIL_PIPELINE,cb,depth,dl,da,1);
    int index=fg_source_acquire();
    if (index < 0) return fg_authority_begin_failure(FG_AUTH_FAIL_SOURCE,cb,depth,dl,da,1);
    fg_source *s=&F.source[index];
    if (!fg_buffer_make(&s->authority,bytes,0) ||
        !fg_buffer_make(&s->authority_readback,32,1) || !fg_buffer_make(&s->spans,plane*4,0)) {
        return fg_authority_begin_failure(FG_AUTH_FAIL_BUFFERS,cb,depth,dl,da,1);
    }
    /* Source retirement and CPU/output pins have cleared before acquire.
       A private immutable set survives physical submissions without allocating
       buffers, pools or sets for each draw/span. */
    if (!s->span_pool) {
        VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
        VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
        if (!fg_result(vkCreateDescriptorPool(V.dev,&pi,NULL,&s->span_pool),"ordered authority descriptor pool")) {
            return fg_authority_begin_failure(FG_AUTH_FAIL_POOL,cb,depth,dl,da,1);
        }
    }
    if (!fg_result(vkResetDescriptorPool(V.dev,s->span_pool,0),"ordered authority descriptor reset")) {
        return fg_authority_begin_failure(FG_AUTH_FAIL_POOL_RESET,cb,depth,dl,da,1);
    }
    VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s->span_pool,
        .descriptorSetCount=1,.pSetLayouts=&F.authority_dsl};
    if (!fg_result(vkAllocateDescriptorSets(V.dev,&ai,&s->span_set),"ordered authority descriptor set")) {
        return fg_authority_begin_failure(FG_AUTH_FAIL_SET,cb,depth,dl,da,1);
    }
    VkDescriptorBufferInfo buffers[2]={{s->authority.buffer,0,s->authority.size},{s->spans.buffer,0,s->spans.size}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i=0;i<2;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=s->span_set,.dstBinding=i,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
    vkUpdateDescriptorSets(V.dev,2,writes,0,NULL);
    s->authority_image=vk_tex_image(depth); s->authority_level=dl; s->authority_layer=da;
    s->authority_w=w; s->authority_h=h;
    F.authority=index;
    unsigned query=(FG_SOURCES*4+FG_OUTPUTS+(unsigned)index)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_buffer_barrier(cb,&s->authority,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fg_buffer_barrier(cb,&s->spans,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    uint32_t header[FG_AUTHORITY_WORDS]={w,h,0,0,0x3f800000u};
    vkCmdUpdateBuffer(cb,s->authority.buffer,0,sizeof header,header);
    /* Only state needs initialization. Proven VALUES are initialized lazily
       by the first clipped observation of each native pixel. No image copy. */
    vkCmdFillBuffer(cb,s->spans.buffer,plane*3,plane,0);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->authority_snapshot=1;
    fg_profile_source_mark((unsigned)index,FG_PROFILE_AUTHORITY_INIT);
    pthread_mutex_lock(&fg_stats_mu); F.reservations++; pthread_mutex_unlock(&fg_stats_mu);
    return 1;
}
static int fg_authority_span_fail(const char *reason) {
    if (F.authority >= 0) {
        fg_source *s=&F.source[F.authority];
        if (!s->span_failed) {
            pthread_mutex_lock(&fg_stats_mu); F.span_failed++; pthread_mutex_unlock(&fg_stats_mu);
        }
        s->span_open=0;
        s->span_failed=1;
    }
    LOGV("framegen ordered authority source refused: %s (no stale VALUE authority reused)",reason);
    vk_fg_skip();
    return 0;
}
static void fg_authority_span_dispatch(VkCommandBuffer cb, fg_source *s, unsigned phase) {
    fg_buffer_barrier(cb,&s->spans,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,F.authority_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,F.authority_layout,0,1,&s->span_set,0,NULL);
    uint32_t push[4]={phase,s->authority_w,s->authority_h,0};
    vkCmdPushConstants(cb,F.authority_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,push);
    vkCmdPushConstants(cb,F.authority_layout,VK_SHADER_STAGE_COMPUTE_BIT,80,sizeof s->span_rect,s->span_rect);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
    vkCmdDispatch(cb,(uint32_t)(((uint64_t)s->span_rect[2]*s->span_rect[3]+255)/256),1,1);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    fg_buffer_barrier(cb,&s->spans,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
}
int vk_fg_authority_span_begin(VkCommandBuffer cb, vk_tex *depth, unsigned dl, unsigned da,
    unsigned x, unsigned y, unsigned width, unsigned height, int experimental) {
    if (!vk_fg_enabled()) return 0;
    if (!vk_fg_authority_begin(cb,depth,dl,da))
        return fg_authority_span_fail("before observation without a live root");
    unsigned index=(unsigned)F.authority;
    fg_source *s=&F.source[index];
    if (s->authority_compared || s->span_open || s->span_failed ||
        !width || !height || x>=s->authority_w || y>=s->authority_h ||
        width>s->authority_w-x || height>s->authority_h-y ||
        (experimental!=0 && experimental!=1) || (experimental && !vk_fg_uncertified_plane()))
        return fg_authority_span_fail("before observation rect/classification/state mismatch");
    if (s->span_begins >= FG_AUTHORITY_SPANS)
        return fg_authority_span_fail("bounded 256-span capacity exhausted");
#ifdef TOTK_VK_TESTS
    if (fg_test_uncertified_failure) return fg_authority_span_fail("injected before-copy failure");
#endif
    s->span_rect[0]=x; s->span_rect[1]=y; s->span_rect[2]=width; s->span_rect[3]=height;
    s->span_experimental=experimental;
    unsigned query=fg_authority_span_query(index)+4*s->span_begins;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_buffer_barrier(cb,&s->spans,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fg_authority_copy_rect(cb,depth,dl,da,s,&s->spans,0,x,y,width,height);
    fg_authority_span_dispatch(cb,s,5);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->span_begins++; s->span_open=1;
    pthread_mutex_lock(&fg_stats_mu);
    F.span_before_copies++; F.span_copied_texels+=(uint64_t)width*height;
    pthread_mutex_unlock(&fg_stats_mu);
    return 1;
}
int vk_fg_authority_span_end(VkCommandBuffer cb, vk_tex *depth, unsigned dl, unsigned da) {
    if (!vk_fg_enabled()) return 0;
    fg_consume_reset();
    if (!cb || !F.ready || F.invalid || F.current >= 0 || F.authority < 0)
        return fg_authority_span_fail("after observation without a live root");
    unsigned index=(unsigned)F.authority;
    fg_source *s=&F.source[index];
    if (!s->authority_snapshot || s->authority_compared || !fg_authority_matches(s,depth,dl,da) ||
        !s->span_open || s->span_failed)
        return fg_authority_span_fail("after observation identity/state mismatch");
#ifdef TOTK_VK_TESTS
    if (fg_test_uncertified_failure) return fg_authority_span_fail("injected after-copy failure");
#endif
    VkDeviceSize plane=(VkDeviceSize)s->authority_w*s->authority_h*4;
    unsigned query=fg_authority_span_query(index)+4*s->span_ends+2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_buffer_barrier(cb,&s->spans,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fg_authority_copy_rect(cb,depth,dl,da,s,&s->spans,plane,
        s->span_rect[0],s->span_rect[1],s->span_rect[2],s->span_rect[3]);
    fg_authority_span_dispatch(cb,s,s->span_experimental ? 6 : 4);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->span_open=0; s->span_ends++; s->experimental_spans+=!!s->span_experimental;
    pthread_mutex_lock(&fg_stats_mu);
    F.span_after_copies++; F.span_count++;
    F.span_copied_texels+=(uint64_t)s->span_rect[2]*s->span_rect[3];
    pthread_mutex_unlock(&fg_stats_mu);
    return 1;
}
static void fg_authority_compare(VkCommandBuffer cb, vk_tex *depth, unsigned dl, unsigned da, unsigned index) {
    fg_source *s=&F.source[index];
    unsigned query=(FG_SOURCES*5+FG_OUTPUTS+index)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_profile_source_begin(cb,index,FG_PROFILE_FINAL_DEPTH_COPY);
    fg_buffer_barrier(cb,&s->authority,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    fg_authority_copy(cb,depth,dl,da,s,&s->authority,FG_AUTHORITY_WORDS*4+(VkDeviceSize)s->authority_w*s->authority_h*4);
    fg_profile_source_end(cb,index,FG_PROFILE_FINAL_DEPTH_COPY);
    fg_profile_source_begin(cb,index,FG_PROFILE_FINAL_COMPARE);
    fg_buffer_barrier(cb,&s->authority,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    fg_buffer_barrier(cb,&s->spans,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,F.authority_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,F.authority_layout,0,1,&s->span_set,0,NULL);
    /* Immutable prefix/census and EVERY shader-valid final depth texel's range.
       Only observed final==last-after unknown/experimental states contribute. */
    uint32_t push[4]={0,s->authority_w,s->authority_h,!!s->experimental_spans};
    vkCmdPushConstants(cb,F.authority_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,push);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
    vkCmdDispatch(cb,s->authority_h,1,1);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    fg_buffer_barrier(cb,&s->authority,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy={.size=s->authority_readback.size};
    vkCmdCopyBuffer(cb,s->authority.buffer,s->authority_readback.buffer,1,&copy);
    fg_buffer_barrier(cb,&s->authority_readback,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
    fg_profile_source_end(cb,index,FG_PROFILE_FINAL_COMPARE);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->authority_compared=1;
    fg_profile_source_mark(index,FG_PROFILE_COMPARE);
    fg_writer_final(cb,index);
    pthread_mutex_lock(&fg_stats_mu); F.authority_copies++; F.comparisons++; pthread_mutex_unlock(&fg_stats_mu);
}
/* This pair's two masks/censuses live in the new source until its output has
   retired. Old-source raw prefixes/range certificates are immutable, even while
   that old source is pinned by an earlier output awaiting host completion. */
static int fg_pair_dilate(VkCommandBuffer cb, fg_source *s, fg_source *old) {
    fg_source *sources[2]={old,s};
    if (!old->authority_snapshot && !s->authority_snapshot) return 1;
    for (unsigned side=0;side<2;side++) {
        fg_source *source=sources[side];
        if (!source->authority_snapshot) continue;
        /* Preserve the interpolator's fullres known-mask offset (header+pixels).
           Coarse scratch fits in the old prefix plane; no extra buffer is needed. */
        if (!source->authority_compared ||
            !fg_buffer_make(&s->pair_authority[side],source->authority.size,0)) return 0;
    }
    if (!fg_buffer_make(&s->pair_readback,2*FG_AUTHORITY_WORDS*4,1)) return 0;
    for (unsigned side=0;side<2;side++) {
        fg_source *source=sources[side];
        if (!source->authority_snapshot) continue;
        fg_buffer *pair=&s->pair_authority[side];
        VkDescriptorSet set=vk_alloc_set(F.authority_dsl);
        VkDescriptorBufferInfo buffers[2]={{source->authority.buffer,0,source->authority.size},
            {pair->buffer,0,pair->size}};
        VkWriteDescriptorSet writes[2];
        for (unsigned i=0;i<2;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=set,.dstBinding=i,.descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
        vkUpdateDescriptorSets(V.dev,2,writes,0,NULL);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,F.authority_pipeline);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,F.authority_layout,0,1,&set,0,NULL);
        struct { uint32_t phase,w,h,reserved; float transform[16]; } push={
            .w=source->authority_w,.h=source->authority_h};
        if (source->native_scene_ready) {
            /* Retained color contains a spatial kernel. Dilate once/pair in
               depth pixels. Four canvas texels cover SGSR/EASU plus RCAS.
               SMAA HIGH searches at most 16 two-pixel axial steps (diagonal
               search is shorter); 40 more cover that weight dependency plus
               edge/corner/bilinear/neighborhood taps, not just the final RGB tap. */
            uint32_t support=4+(vk_scene_aa_enabled()?40:0);
            uint32_t rx=(uint32_t)(((uint64_t)support*source->authority_w+source->scene.w-1)/source->scene.w);
            uint32_t ry=(uint32_t)(((uint64_t)support*source->authority_h+source->scene.h-1)/source->scene.h);
            if (rx>65535 || ry>65535) return 0;
            push.reserved=rx|(ry<<16);
        }
        memcpy(push.transform,side ? s->next_transform : s->transform,sizeof push.transform);
        /* Phase 3 derives radii from this source's complete certified depth
           domain and its CURRENT pair transform, not its capture-time pair. */
        const unsigned phases[3]={3,1,2};
        unsigned index=(unsigned)(s-F.source);
        unsigned profile=side ? FG_PROFILE_SECOND_BOUND : FG_PROFILE_FIRST_BOUND;
        uint64_t coarse_w=((uint64_t)source->authority_w+3)/4;
        uint64_t coarse_h=((uint64_t)source->authority_h+3)/4;
        uint32_t coarse_groups=(uint32_t)((coarse_w*coarse_h+255)/256);
        for (unsigned i=0;i<3;i++) {
            fg_profile_source_begin(cb,index,profile+i);
            push.phase=phases[i];
            vkCmdPushConstants(cb,F.authority_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
            vkCmdDispatch(cb,i==0 ? 1 : coarse_groups,1,1);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            fg_buffer_barrier(cb,pair,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_TRANSFER_READ_BIT);
            fg_profile_source_end(cb,index,profile+i);
        }
        fg_profile_source_begin(cb,index,profile+3);
        VkBufferCopy copy={.dstOffset=side*FG_AUTHORITY_WORDS*4,.size=FG_AUTHORITY_WORDS*4};
        vkCmdCopyBuffer(cb,pair->buffer,s->pair_readback.buffer,1,&copy);
        fg_profile_source_end(cb,index,profile+3);
    }
    fg_buffer_barrier(cb,&s->pair_readback,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
    return 1;
}
void vk_fg_camera(const float cam[84]) {
    if (!vk_fg_enabled()) return;
    int qcom=vk_fg_qcom_requested(), lsfg=vk_fg_lsfg_requested();
    if (((qcom && !vk_fg_qcom_active()) || (lsfg && !vk_fg_lsfg_active())) &&
        vk_submit_external_disable()) {
        if (FQ.budget_enabled) {
            /* No reset while callbacks/outputs still carry external identities. */
            pthread_mutex_lock(&fg_stats_mu);
            int idle=!vk_fg_qcom_pending() && !vk_fg_lsfg_pending();
            for (unsigned i=0;idle && i<FG_OUTPUTS;i++) idle=!atomic_load_explicit(&F.output[i].busy,memory_order_acquire);
            if (idle) idle=!fg_qcom_prepared_pending(&F.prepared_budget);
            if (idle) {
                F.budget=(fg_budget_policy){.feedback_gpu_only=vk_fg_locked()};
                FQ.budget=(fg_budget_external){0}; FQ.budget_enabled=0;
                F.budget_pre_frame=F.budget_reserved_frame=0;
            }
            pthread_mutex_unlock(&fg_stats_mu);
        }
        if (!FQ.budget_enabled) {
            if (qcom) vk_fg_qcom_rearm();
            if (lsfg) vk_fg_lsfg_rearm();
        }
    }
    fg_consume_reset();
    if (!cam || F.have_camera) return;
    memcpy(F.camera, cam, sizeof F.camera);
    F.have_camera = 1;
}
void vk_fg_skip(void) {
    if (atomic_load_explicit(&fg_config, memory_order_relaxed) < 1) return;
    F.invalid = 1;
    F.previous_valid = 0;
}
int vk_fg_dump_requested(void) { return fg_dump_frame()!=0; }
void vk_fg_capture_geometry(uint64_t real_frame,const float depth_viewport[4],const int depth_scissor[4]) {
    if (!fg_dump_frame() || F.current<0) return;
    fg_source *s=&F.source[F.current];
    s->captured_real_frame=real_frame;
    s->captured_geometry_valid=1;
    memcpy(s->captured_viewport,depth_viewport,sizeof s->captured_viewport);
    memcpy(s->captured_scissor,depth_scissor,sizeof s->captured_scissor);
}
int vk_fg_capture(VkCommandBuffer cb, vk_tex *scene, unsigned sl, unsigned sa,
                   vk_tex *depth, unsigned dl, unsigned da, uint32_t w, uint32_t h) {
    if (!vk_fg_enabled()) return 0;
    fg_consume_reset();
    if (!fg_init() || F.invalid) return 0;
    /* Even a second capture attempt must not let a different subresource keep
       the first capture's authority alive. The reservation is frame-local. */
    if (F.authority >= 0 && !fg_authority_matches(&F.source[F.authority],depth,dl,da)) {
        vk_fg_skip(); return 0;
    }
    if (F.authority >= 0 && (F.source[F.authority].span_open ||
        F.source[F.authority].span_failed))
        return fg_authority_span_fail("capture with an unclosed or failed unproven span");
    if (F.current >= 0) return 0;
    if (!cb || !F.have_camera || !w || !h || !fg_subresource(scene,sl,sa) || !fg_subresource(depth,dl,da) ||
        vk_tex_aspect(scene) != VK_IMAGE_ASPECT_COLOR_BIT || vk_tex_aspect(depth) != VK_IMAGE_ASPECT_DEPTH_BIT ||
        vk_tex_format(depth) != VK_FORMAT_D32_SFLOAT || !fg_scene_format(vk_tex_format(scene)) ||
        vk_tex_width(scene,sl) != w || vk_tex_height(scene,sl) != h) { vk_fg_skip(); return 0; }
    if (vk_fg_qcom_requested()) vk_fg_qcom_prepare(w,h);
    uint32_t dw = vk_tex_width(depth,dl), dh = vk_tex_height(depth,dl);
    /* Both captures are complete subresources. The game's composite stretches
       scene/depth independently in x and y; equal aspect is not required.
       A canvas pixel center maps to depth texel coordinates
       ((x+.5)*dw/w-.5, (y+.5)*dh/h-.5). In normalized UV these independent scales
       cancel exactly, so both shaders already sample the proper full-image
       coordinates without an isotropic factor or a half-pixel offset. Camera
       validation and reprojection remain in the actual depth extent. */
    if (!dw || !dh ||
        fg_math_camera(F.camera,dw,dh) != FG_MATH_OK ||
        !fg_supports(vk_tex_format(scene), VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
        !fg_supports(VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
        vk_fg_skip(); return 0;
    }
    if (F.diag && (F.previous_w != dw || F.previous_h != dh ||
        F.previous_scene_w != w || F.previous_scene_h != h))
        LOGV("[framegen-map] full-subresource depth=%ux%u canvas=%ux%u scale-x=%.9g scale-y=%.9g offset=0,0 (normalized UV, independent pixel-center scales)",
            dw,dh,w,h,(double)w/dw,(double)h/dh);
    int index=F.authority >= 0 ? F.authority : fg_source_acquire();
    if (index < 0) { vk_fg_skip(); return 0; }
    if (F.authority_attempted && (F.authority < 0 ||
        !fg_authority_matches(&F.source[index],depth,dl,da))) { vk_fg_skip(); return 0; }
    fg_source *s = &F.source[index];
    int native_scene=vk_fg_interpolating() && vk_scene_post_enabled();
    /* Scene AA feeds external engines from the successful native pre-UI seed.
       Legacy scale-only external captures retain their existing low-res path. */
    int copy_scene=!native_scene || (vk_fg_lsfg_active() && !vk_scene_aa_enabled());
    /* Lean (never with scene post) still allocates both images: descriptors
       bind them. A generating host TEST bridge samples s->scene in place of
       the AFME output, so there the scene copy stands in for the kept AFME
       input copy; a refused bridge, like production, reads no scene copy. */
    int lean=vk_fg_adaptive_lean();
    int scene_copy=copy_scene && (!lean || (fg_generation_test_bridge && fg_generation_engine_ready()));
    if ((copy_scene && !fg_image_make(&s->scene,w,h,vk_tex_format(scene),VK_IMAGE_ASPECT_COLOR_BIT,0)) ||
        !fg_image_make(&s->depth,dw,dh,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT,0)) return 0;
    s->lean=lean;
    if (native_scene && !copy_scene) {
        /* Preserve only the logical low-res mapping, not an unused color copy.
           The successful real seed will supply immutable native scene pixels. */
        s->scene.w=w; s->scene.h=h; s->scene.format=vk_tex_format(scene);
    }
    memcpy(s->camera, F.camera, sizeof s->camera);
    s->prepared = s->transferred = s->replay_depth = 0;
    s->ui_ready = 0;
    s->terminal_ready=0;
    s->owned.used=s->owned.frame=0;
    s->owned.begun=s->owned.failed=s->owned.ready=s->owned.writes=s->owned.copies=0;
    memset(s->owned.sets,0,sizeof s->owned.sets);
    memset(s->owned.layouts,0,sizeof s->owned.layouts);
    memset(s->owned.descriptors,0,sizeof s->owned.descriptors);
    s->generation_template=0;
    s->pair_source = -1;
    if (fg_dump_frame()) {
        s->capture_epoch++;
        s->capture_cb=cb;
        s->capture_ticket_predicted=mtl_last_fence()+1;
        s->captured_real_frame=F.budget_frame+1;
        s->captured_geometry_valid=0;
        memset(s->captured_viewport,0,sizeof s->captured_viewport);
        memset(s->captured_scissor,0,sizeof s->captured_scissor);
    }
    s->predicted = 0;
    s->depth_range_changed=0;
    if (vk_fg_qcom_requested() || vk_fg_lsfg_requested() || fg_generation_test_bridge)
        FQ.copy[index]=FQ.generated[index]=0;
    if (F.previous_valid && F.previous_w == dw && F.previous_h == dh &&
        F.previous_scene_w == w && F.previous_scene_h == h) {
        fg_math_result reason=vk_fg_interpolating() ?
            fg_math_interpolate(F.previous,s->camera,dw,dh,s->transform,s->next_transform) :
            fg_math_predict(s->camera,F.previous,dw,dh,s->transform);
        s->predicted=reason == FG_MATH_OK;
        if (s->predicted && vk_fg_interpolating()) {
            for (int i=10;i<=11;i++)
                if (fabs(s->camera[28+i]-F.previous[28+i]) > .002*(1+fabs(s->camera[28+i])))
                    s->depth_range_changed=1;
            F.depth_range_pairs+=s->depth_range_changed;
        }
        if (reason != FG_MATH_OK && vk_fg_interpolating()) F.cuts++;
        if (F.diag) {
            fg_camera_pair_stats sample=fg_math_camera_pair_stats(F.previous,s->camera,dw,dh,reason,vk_fg_interpolating());
            fg_math_camera_pair_accumulate(&F.camera_pairs,sample);
            if (sample.rejection==FG_CAMERA_REJECT_PROJECTION)
                fg_projection_diag(F.previous,s->camera,vk_fg_interpolating());
        }
        static fg_math_result last_reason = FG_MATH_OK;
        if (reason != FG_MATH_OK && reason != last_reason)
            LOGV("framegen camera prediction rejected: %s",fg_math_reason(reason));
        last_reason = reason;
    }
    s->retire = 0;
    F.current = index;
    vkCmdResetQueryPool(cb, F.queries, (unsigned)index*2, 2);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.queries, (unsigned)index*2);
    if (scene_copy) {
        fg_profile_source_begin(cb,(unsigned)index,FG_PROFILE_SCENE_COPY);
        fg_copy(cb, scene, sl, sa, &s->scene);
        fg_profile_source_end(cb,(unsigned)index,FG_PROFILE_SCENE_COPY);
    } else if (copy_scene) fg_image_placeholder(cb,&s->scene);
    if (vk_fg_qcom_active() && !vk_scene_aa_enabled()) {
        if (!fg_supports(s->scene.format,VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            vk_fg_qcom_refuse("pre-UI scene format cannot blit to AHB");
        else FQ.copy[index]=vk_fg_qcom_capture(cb,(unsigned)index,vk_tex_image(scene),sl,sa,w,h);
    }
    if (vk_fg_lsfg_active() && !vk_scene_aa_enabled())
        FQ.copy[index]=vk_fg_lsfg_capture(cb,(unsigned)index,s->scene.image,s->scene.raw,s->scene.format,w,h);
    if (!lean) {
        fg_profile_source_begin(cb,(unsigned)index,FG_PROFILE_DEPTH_COPY);
        fg_copy(cb, depth, dl, da, &s->depth);
        fg_profile_source_end(cb,(unsigned)index,FG_PROFILE_DEPTH_COPY);
    } else {
        fg_image_placeholder(cb,&s->depth);
        F.lean_counts[VK_FG_LEAN_SOURCES]++;
        F.lean_counts[VK_FG_LEAN_SCENE_COPIES]+=copy_scene && !scene_copy;
        F.lean_counts[VK_FG_LEAN_DEPTH_COPIES]++;
    }
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.queries, (unsigned)index*2+1);
    s->timed = 1;
    fg_profile_source_mark((unsigned)index,FG_PROFILE_PRESERVE);
    if (s->authority_snapshot) fg_authority_compare(cb,depth,dl,da,(unsigned)index);
    return 1;
}
static int fg_real_reject(const char *reason) {
    static const char *last_reason;
    if (last_reason != reason) {
        LOGV("[framegen-real] refusing interpolation: %s",reason);
        last_reason=reason;
    }
    vk_fg_skip();
    return 0;
}
/* A successful real scene seed calls this before its first UI draw. Retain its
   filtered pixels rather than running AA/upscaling again for every midpoint. */
int vk_fg_capture_native_scene(VkCommandBuffer cb, const vk_post_output *native) {
    if (!vk_fg_enabled() || !vk_fg_interpolating() || !vk_scene_post_enabled() ||
        F.current<0 || F.invalid) return 0;
    fg_source *s=&F.source[F.current];
    if (!cb || !native || !native->image || !native->w || !native->h ||
        native->format!=s->scene.format || s->native_scene_ready ||
        !fg_image_make(&s->scene_scaled,native->w,native->h,native->format,VK_IMAGE_ASPECT_COLOR_BIT,0))
        return fg_real_reject("native pre-UI scene unavailable, duplicated or incompatible");
    unsigned query=(FG_SOURCES*7+FG_OUTPUTS+(unsigned)F.current)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_copy_image(cb,native->image,0,0,&s->scene_scaled);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->native_scene_ready=1;
    fg_profile_source_mark((unsigned)F.current,FG_PROFILE_NATIVE_SCENE_COPY);
    if (vk_scene_aa_enabled()) {
        if (vk_fg_qcom_active()) {
            if (!fg_supports(native->format,VK_FORMAT_FEATURE_BLIT_SRC_BIT))
                vk_fg_qcom_refuse("filtered pre-UI scene format cannot blit to AHB");
            else FQ.copy[F.current]=vk_fg_qcom_capture(cb,(unsigned)F.current,native->image,0,0,native->w,native->h);
        }
        if (vk_fg_lsfg_active()) {
            if (!fg_supports(native->format,VK_FORMAT_FEATURE_BLIT_SRC_BIT))
                vk_fg_lsfg_refuse("filtered pre-UI scene format cannot blit to AHB");
            else {
                FQ.copy[F.current]=vk_fg_lsfg_capture(cb,(unsigned)F.current,s->scene_scaled.image,
                    s->scene_scaled.raw,s->scene_scaled.format,native->w,native->h);
                if (fg_dump_frame()) {
                    s->capture_cb=cb;
                    s->capture_ticket_predicted=mtl_last_fence()+1;
                }
            }
        }
    }
    static int once;
    if (!once) {
        once=1;
        LOGV("[scene-scale-framegen] retaining real AA/upscale once: canvas=%ux%u scene=%ux%u depth=%ux%u; native midpoint reprojection+affine UI; no midpoint SMAA/SGSR/FSR; external engines consume filtered real seeds when scene AA is enabled; independent normalized-UV depth mapping",
            s->scene.w,s->scene.h,native->w,native->h,s->depth.w,s->depth.h);
    }
    return 1;
}
int vk_fg_capture_real(VkCommandBuffer cb, vk_tex *real, unsigned level, unsigned layer) {
    if (!vk_fg_enabled() || !vk_fg_interpolating() || F.current < 0 || F.invalid) return 0;
    fg_source *s=&F.source[F.current];
    if (s->real_ready || s->real_skipped) return fg_real_reject("real terminal was captured more than once");
    if (!cb || !fg_subresource(real,level,layer) || vk_tex_aspect(real) != VK_IMAGE_ASPECT_COLOR_BIT)
        return fg_real_reject("real terminal subresource unavailable");
    VkImage image=vk_tex_image(real);
    uint32_t width=vk_tex_width(real,level), height=vk_tex_height(real,level);
    VkFormat format=vk_tex_format(real);
    int native=vk_native_ui_enabled() && vk_tex_native_active(real);
    if (native) {
        /* native_output requires valid contents. Also pin the existing image:
           an extent change may otherwise replace/reseed it from the guest. */
        VkImage expected=vk_tex_native_image(real);
        vk_post_output output;
        if (level || layer || !expected || !vk_tex_native_output(real,&output) || output.image != expected)
            return fg_real_reject("native presented sibling provenance unavailable or changed");
        image=output.image; width=output.w; height=output.h; format=output.format;
    }
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB &&
        format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB) {
        /* A future masked pair cannot substitute a guessed display conversion.
           Unmasked interpolation retains its existing color path. */
        return native ? fg_real_reject("native real terminal encoding unsupported") : 0;
    }
    if (!width || !height ||
        !fg_supports(format,VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        return fg_real_reject("real terminal copy format/extent or allocation unavailable");
    if (s->lean) {
        /* Same provenance/encoding admission; only the masked-pair fallback copy
           is skipped. real_ready stays 0, so binding 10 aliases the final image. */
        s->real_skipped=1; F.lean_counts[VK_FG_LEAN_REAL_COPIES]++;
        return 1;
    }
    if (!fg_image_make(&s->real,width,height,format,VK_IMAGE_ASPECT_COLOR_BIT,2))
        return fg_real_reject("real terminal copy format/extent or allocation unavailable");
    unsigned query=(FG_SOURCES*6+FG_OUTPUTS+(unsigned)F.current)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_copy_image(cb,image,level,layer,&s->real);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->real_ready=1;
    fg_profile_source_mark((unsigned)F.current,FG_PROFILE_REAL_COPY);
    return 1;
}

static void fg_render_record(VkCommandBuffer cb, fg_source *s, fg_image *target, VkPipeline pipeline,
                       VkImageView final, VkImageLayout final_layout, float mode,int immutable) {
    if (immutable || !F.known_ready) {
        const uint32_t known[4]={0};
        vkCmdUpdateBuffer(cb,F.known.buffer,0,sizeof known,known);
        fg_buffer_barrier(cb,&F.known,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        if (!immutable) F.known_ready=1;
    }
    VkDescriptorSet set = vk_alloc_set(F.dsl);
    int interp = mode == 3;
    int output = mode == 4;
    int qcom = mode == 5 || mode == 6;
    fg_source *first=interp || mode == 6 ? &F.source[F.history] :
        output && s->pair_source >= 0 ? &F.source[s->pair_source] : s;
    fg_image *first_scene=first->native_scene_ready ? &first->scene_scaled : &first->scene;
    fg_image *second_scene=s->native_scene_ready ? &s->scene_scaled : &s->scene;
    VkDescriptorImageInfo images[11] = {
        {F.sampler[0],first_scene->raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {F.sampler[1],first->depth.raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {F.sampler[0],final,final_layout},
        {F.sampler[0],second_scene->raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {F.sampler[1],s->depth.raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {F.sampler[0],interp || qcom ? first->ui_black.raw : final,interp || qcom ? VK_IMAGE_LAYOUT_GENERAL : final_layout},
        {F.sampler[0],interp || qcom ? first->ui_white.raw : final,interp || qcom ? VK_IMAGE_LAYOUT_GENERAL : final_layout},
        {0},{0},
        {F.sampler[1],output ? s->fallback.raw : final,output ? VK_IMAGE_LAYOUT_GENERAL : final_layout},
        {F.sampler[1],first->real_ready ? first->real.raw : final,
            first->real_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : final_layout}};
    fg_buffer *first_mask=interp && first->authority_compared ? &s->pair_authority[0] : &F.known;
    fg_buffer *second_mask=interp && s->authority_compared ? &s->pair_authority[1] : &F.known;
    VkDescriptorBufferInfo buffers[2]={{first_mask->buffer,0,first_mask->size},{second_mask->buffer,0,second_mask->size}};
    VkWriteDescriptorSet writes[11];
    for (unsigned i=0; i<11; i++) {
        writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=set,.dstBinding=i,.descriptorCount=1,
            .descriptorType=i == 7 || i == 8 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        if (i == 7 || i == 8) writes[i].pBufferInfo=&buffers[i-7];
        else writes[i].pImageInfo=&images[i];
    }
    vkUpdateDescriptorSets(V.dev,11,writes,0,NULL);
    vk_barrier_all(cb);
    fg_barrier(cb,target->image,(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView=target->raw,
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE}};
    if (interp) {
        fg_barrier(cb,s->fallback.image,(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
            VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        attachment[1]=(VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=s->fallback.raw,
            .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea={{0,0},{target->w,target->h}}, .layerCount=1,
        .colorAttachmentCount=interp ? 2 : 1, .pColorAttachments=attachment};
    vkCmdBeginRendering(cb,&ri);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,F.layout,0,1,&set,0,NULL);
    float push[32];
    memcpy(push,s->transform,sizeof s->transform);
    if (interp) memcpy(push+16,s->next_transform,sizeof s->next_transform);
    else {
        push[16]=mode; push[17]=s->camera[56]; push[18]=1.0f/target->w; push[19]=1.0f/target->h;
    }
    vkCmdPushConstants(cb,F.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,interp ? sizeof push : 80,push);
    VkViewport viewport={0,0,(float)target->w,(float)target->h,0,1};
    VkRect2D scissor={{0,0},{target->w,target->h}};
    vkCmdSetViewport(cb,0,1,&viewport); vkCmdSetScissor(cb,0,1,&scissor);
    vkCmdDraw(cb,3,1,0,0); vkCmdEndRendering(cb);
    fg_barrier(cb,target->image,(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
    if (interp) fg_barrier(cb,s->fallback.image,(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
static void fg_render(VkCommandBuffer cb,fg_source *s,fg_image *target,VkPipeline pipeline,
        VkImageView final,VkImageLayout final_layout,float mode) {
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
    fg_render_record(cb,s,target,pipeline,final,final_layout,mode,0);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
}
static int fg_scale_scene(VkCommandBuffer cb, fg_source *s, fg_image *input, fg_image *target) {
    vk_post_output dst={target->image,target->raw,target->w,target->h,vk_fg_raw_format(target->format)},scratch={0};
    if (vk_scene_scale_fsr()) {
        if (!fg_image_make(&s->scene_scratch,target->w,target->h,target->format,VK_IMAGE_ASPECT_COLOR_BIT,1)) return 0;
        scratch=(vk_post_output){s->scene_scratch.image,s->scene_scratch.raw,target->w,target->h,dst.format};
    }
    return vk_scene_scale_run(cb,input->raw,input->w,input->h,0,0,input->w,input->h,&dst,&scratch);
}
int vk_fg_prepare_canvas(VkCommandBuffer cb, uint32_t width, uint32_t height) {
    if (!vk_fg_enabled() || F.current < 0 || F.invalid) return 0;
    fg_source *s = &F.source[F.current];
    if (!s->predicted && !vk_fg_interpolating()) return 0;
    if (F.canvas_format != s->scene.format) {
        /* Pipelines can still be referenced by earlier source-ring entries. Do
           not destroy one on a resize; the accepted canvas format is fixed. */
        if (F.canvas_format != VK_FORMAT_UNDEFINED) { vk_fg_skip(); return 0; }
        if (!vk_fg_interpolating() && !fg_make_pipeline(s->scene.format, &F.canvas_pipeline,0)) return 0;
        F.canvas_format = s->scene.format;
    }
    if (!width || !height || !fg_image_make(&s->canvas,width,height,s->scene.format,VK_IMAGE_ASPECT_COLOR_BIT,1)) return 0;
    if (!vk_fg_interpolating()) {
        unsigned query = (FG_SOURCES+FG_OUTPUTS+(unsigned)F.current)*2;
        vkCmdResetQueryPool(cb,F.queries,query,2);
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
        if (vk_scene_scale_enabled()) {
            if (!fg_image_make(&s->scene_warp,s->scene.w,s->scene.h,s->scene.format,VK_IMAGE_ASPECT_COLOR_BIT,1)) return 0;
            fg_render(cb,s,&s->scene_warp,F.canvas_pipeline,s->scene.raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,-1);
            if (!fg_scale_scene(cb,s,&s->scene_warp,&s->canvas)) { vk_fg_skip(); return 0; }
        } else fg_render(cb,s,&s->canvas,F.canvas_pipeline,s->scene.raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,-1);
        F.passes[0]++;
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
        fg_profile_source_mark((unsigned)F.current,FG_PROFILE_WARP);
    }
    s->prepared = 1;
    return 1;
}
int vk_fg_interp_compose(VkCommandBuffer cb) {
    /* Adaptive generates only after assigning its actual display target. UI
     * capture still occurs here; no speculative fixed midpoint is submitted. */
    if (vk_fg_adaptive()) return 0;
    if (!vk_fg_interpolating() || !vk_fg_canvas_view() || F.history < 0) {
        F.history_skips++; return 0;
    }
    fg_source *s=&F.source[F.current], *old=&F.source[F.history];
    if (vk_fg_lsfg_active() && s->predicted && old->ui_ready==3 &&
        old->scene.format==s->scene.format && old->ui_black.w==s->canvas.w &&
        old->ui_black.h==s->canvas.h && FQ.copy[F.current] && FQ.copy[F.history]) {
        if (!vk_fg_lsfg_can_generate() || !fg_budget_pre_admit()) return 0;
        if (!FQ.pipeline && !fg_make_pipeline(s->scene.format,&FQ.pipeline,2)) return 0;
        if (!fg_budget_reserve_work()) return 0;
        VkImage image; VkImageView view;
        vk_fg_lsfg_provenance provenance,*probe=NULL;
        uint64_t frame=F.budget_frame+1;
        if (vk_fg_lsfg_probe_requested() && !fg_dump_captured && fg_dump_frame() &&
            frame>=fg_dump_frame()) {
            provenance=(vk_fg_lsfg_provenance){
                .frame={old->captured_real_frame,s->captured_real_frame},
                .epoch={old->capture_epoch,s->capture_epoch},
                .capture_ticket={old->capture_ticket_predicted,s->capture_ticket_predicted},
                .capture_cb={old->capture_cb,s->capture_cb},.retire={old->retire,s->retire},
                .references={(unsigned)atomic_load_explicit(&old->referenced,memory_order_acquire),
                    (unsigned)atomic_load_explicit(&s->referenced,memory_order_acquire)},
                .history=F.history,.pair_source=s->pair_source};
            probe=&provenance;
        }
        if (vk_fg_lsfg_interpolate((unsigned)F.history,(unsigned)F.current,frame,0.5f,probe,&image,&view)) {
            cb=vk_cb();
            /* Same exact affine shader as QCOM; midpoint retains the older UI. */
            fg_render(cb,s,&s->canvas,FQ.pipeline,view,VK_IMAGE_LAYOUT_GENERAL,6);
            vk_fg_lsfg_release(cb,(unsigned)F.current);
            FQ.generated[F.current]=2; s->pair_source=F.history; F.passes[0]++;
            return 1;
        }
        cb=vk_cb(); /* signal may have sealed the old primary, even on refusal */
        return 0;
    }
    if (vk_fg_qcom_active() && s->predicted && s->ui_ready==3 &&
        FQ.copy[F.current] && FQ.copy[F.history]) {
        if (!vk_fg_qcom_can_generate() || !fg_budget_pre_admit()) return 0;
        if (!FQ.pipeline && !fg_make_pipeline(s->scene.format,&FQ.pipeline,2)) return 0;
        if (!fg_budget_reserve_work()) return 0;
        VkImage image; VkImageView view;
        if (vk_fg_qcom_extrapolate((unsigned)F.history,(unsigned)F.current,
                (unsigned)F.current,F.budget_frame+1,0.5f,&image,&view)) {
            cb=vk_cb();
            fg_render(cb,s,&s->canvas,FQ.pipeline,view,VK_IMAGE_LAYOUT_GENERAL,5);
            vk_fg_qcom_release(cb,(unsigned)F.current);
            FQ.generated[F.current]=1; s->pair_source=F.history; F.passes[0]++;
            return 1;
        }
        /* The handoff may have sealed the old primary even on failure. */
        cb=vk_cb();
        return 0; /* Keep this real frame; fallback starts with the next pair. */
    }
    if (!s->predicted || old->ui_ready != 3 || old->scene.format != s->scene.format ||
        old->ui_black.w != s->canvas.w || old->ui_black.h != s->canvas.h) return 0;
    if ((s->authority_snapshot || old->authority_snapshot) && !old->real_ready) { vk_fg_skip(); return 0; }
    if (!fg_budget_pre_admit()) return 0;
    if (vk_scene_post_enabled() &&
        (!s->native_scene_ready || !old->native_scene_ready ||
         s->scene_scaled.w!=s->canvas.w || s->scene_scaled.h!=s->canvas.h ||
         old->scene_scaled.w!=s->canvas.w || old->scene_scaled.h!=s->canvas.h))
        return fg_real_reject("retained native scenes do not match current native UI extent");
    fg_image *target=&s->canvas;
    if (!fg_supports(VK_FORMAT_R8_UNORM,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !fg_image_make(&s->fallback,target->w,target->h,VK_FORMAT_R8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,1)) return 0;
    if (!F.interp_pipeline && !fg_make_pipeline(s->scene.format,&F.interp_pipeline,1)) return 0;
    if (!fg_budget_reserve_work()) return 0;
    if (!fg_pair_dilate(cb,s,old)) { vk_fg_skip(); return 0; }
    fg_profile_source_begin(cb,(unsigned)F.current,FG_PROFILE_INTERP_UI);
    fg_render(cb,s,target,F.interp_pipeline,vk_scene_post_enabled()?s->scene_scaled.raw:s->scene.raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,3);
    fg_profile_source_end(cb,(unsigned)F.current,FG_PROFILE_INTERP_UI);
    s->pair_source=F.history;
    F.passes[0]++;
    return 1;
}
int vk_fg_qcom_picture(void) {
    return F.current>=0 && vk_fg_qcom_requested() && FQ.generated[F.current];
}
int vk_fg_lsfg_picture(void) {
    return F.current>=0 && vk_fg_lsfg_requested() && FQ.generated[F.current]==2;
}
VkImageView vk_fg_canvas_view(void) {
    return F.current >= 0 && !F.invalid && F.source[F.current].prepared ?
        F.source[F.current].canvas.raw : VK_NULL_HANDLE;
}
int vk_fg_replay_depth(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer) {
    if (!vk_fg_canvas_view()) return 0;
    fg_source *s = &F.source[F.current];
    s->replay_depth = 0;
    if (!depth) return 1;
    if (!fg_subresource(depth,level,layer)) { vk_fg_skip(); return 0; }
    unsigned aspect = vk_tex_aspect(depth);
    if (!fg_image_make(&s->ui_depth,vk_tex_width(depth,level),vk_tex_height(depth,level),
        vk_tex_format(depth),aspect,1)) return 0;
    unsigned query = (FG_SOURCES*3+FG_OUTPUTS+(unsigned)F.current)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    fg_copy(cb,depth,level,layer,&s->ui_depth);
    if (vk_fg_interpolating()) {
        if (!fg_image_make(&s->ui_depth_white,vk_tex_width(depth,level),vk_tex_height(depth,level),
            vk_tex_format(depth),aspect,1)) return 0;
        fg_copy(cb,depth,level,layer,&s->ui_depth_white);
    }
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query+1);
    s->replay_depth = 1;
    fg_profile_source_mark((unsigned)F.current,FG_PROFILE_UI_DEPTH);
    return 1;
}
int vk_fg_replay_begin(VkCommandBuffer cb, uint32_t width, uint32_t height, VkFormat format, int final) {
    if (!vk_fg_canvas_view()) return 0;
    fg_source *s = &F.source[F.current];
    if (!final) {
        unsigned query = (FG_SOURCES*2+FG_OUTPUTS+(unsigned)F.current)*2;
        vkCmdResetQueryPool(cb,F.queries,query,2);
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    }
    int terminal=final == 1, capture=vk_fg_interpolating() && !terminal;
    fg_image *target = terminal ? &s->final : capture ? (final == 2 ? &s->ui_white : &s->ui_black) : &s->canvas;
    if (terminal || capture) {
        if (!fg_image_make(target,width,height,format,VK_IMAGE_ASPECT_COLOR_BIT,1)) return 0;
    } else if (target->w != width || target->h != height || target->format != format) {
        vk_fg_skip(); return 0;
    }
    fg_profile_source_begin(cb,(unsigned)F.current,
        terminal ? FG_PROFILE_TERMINAL : final == 2 ? FG_PROFILE_UI_WHITE : FG_PROFILE_UI_BLACK);
    fg_barrier(cb,target->image,(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        terminal || capture ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView=target->display,
        .imageLayout=VK_IMAGE_LAYOUT_GENERAL, .loadOp=capture ? VK_ATTACHMENT_LOAD_OP_CLEAR :
            terminal ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color.float32={final == 2 ? 1.0f : 0,final == 2 ? 1.0f : 0,final == 2 ? 1.0f : 0,final == 2 ? 1.0f : 0}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea={{0,0},{width,height}}, .layerCount=1,
        .colorAttachmentCount=1, .pColorAttachments=&attachment};
    fg_image *ui_depth=final == 2 ? &s->ui_depth_white : &s->ui_depth;
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView=ui_depth->raw,
        .imageLayout=VK_IMAGE_LAYOUT_GENERAL, .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    if (!terminal && s->replay_depth) {
        fg_barrier(cb,ui_depth->image,(VkImageSubresourceRange){ui_depth->aspect,0,1,0,1},
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        if (s->ui_depth.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (s->ui_depth.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &depth;
    }
    vkCmdBeginRendering(cb,&ri);
    F.passes[terminal ? 2 : 1]++;
    return 1;
}
void vk_fg_replay_end(VkCommandBuffer cb, int final) {
    vkCmdEndRendering(cb);
    vk_barrier_all(cb);
    if (F.current >= 0 && !(vk_fg_adaptive() && final==1)) fg_profile_source_end(cb,(unsigned)F.current,
        final == 1 ? FG_PROFILE_TERMINAL : final == 2 ? FG_PROFILE_UI_WHITE : FG_PROFILE_UI_BLACK);
    if (F.current >= 0 && (final == 2 || (final == 1 && !vk_fg_adaptive()))) {
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,
            (FG_SOURCES*2+FG_OUTPUTS+(unsigned)F.current)*2+1);
        if (final == 1) F.source[F.current].transferred = 1;
        fg_profile_source_mark((unsigned)F.current,FG_PROFILE_REPLAY);
    }
    if (F.current>=0 && final==1 && vk_fg_adaptive()) F.source[F.current].transferred=1;
    if (F.current >= 0 && vk_fg_interpolating() && final != 1) {
        F.source[F.current].ui_ready |= final == 2 ? 2 : 1;
    }
}
void vk_fg_replay_draw(VkCommandBuffer cb, const vk_record_draw *d, const VkDescriptorSet sets[2]) {
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,d->pipeline);
    vk_record_dynamic last;
    vk_record_dynamic_emit(cb,&d->dynamic,&last,1);
    for (unsigned i = 0; i < 2; i++) if (sets[i])
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,d->layout,i,1,&sets[i],0,NULL);
    for (unsigned i = 0; i < 17; i++) if (d->vertex_mask >> i & 1) {
        VkDeviceSize size = d->vertex[i].size, stride = d->vertex[i].stride;
        vkCmdBindVertexBuffers2(cb,i,1,&d->vertex[i].buffer,&d->vertex[i].offset,&size,&stride);
    }
    if (d->index) {
        vkCmdBindIndexBuffer(cb,d->index,d->index_offset,d->index_type);
        vkCmdDrawIndexed(cb,d->count,d->instances,d->first,d->vertex_offset,d->base_instance);
    } else vkCmdDraw(cb,d->count,d->instances,d->first,d->base_instance);
}
void vk_fg_adaptive_terminal(const vk_record_draw *draw, const VkDescriptorSet sets[2],
        uint32_t width, uint32_t height, VkFormat format) {
    if ((!vk_fg_adaptive() && !fg_generation_test_bridge) || F.current<0 || !draw || !sets) return;
    fg_source *s=&F.source[F.current];
    if (atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed)) {
        fg_terminal_owned *o=&s->owned;
        s->terminal_ready=0;
        if (!o->begun || o->failed || o->frame!=F.budget_frame+1 ||
            (!!sets[0]!=!!o->sets[0]) || (!!sets[1]!=!!o->sets[1])) {
            (void)fg_terminal_reject(s,"missing-owned-terminal-set"); return;
        }
        vk_record_draw owned=*draw;
        for (unsigned i=0;i<17;i++) if (owned.vertex_mask>>i&1) {
            VkDescriptorBufferInfo copy;
            if (!fg_terminal_copy(s,(VkDescriptorBufferInfo){owned.vertex[i].buffer,
                    owned.vertex[i].offset,owned.vertex[i].size},16,&copy,(vk_fg_terminal_input){
                        .role=VK_FG_TERMINAL_INPUT_VERTEX,.stage=0,.stream=i})) return;
            owned.vertex[i].buffer=copy.buffer; owned.vertex[i].offset=copy.offset;
        }
        if (owned.index) {
            unsigned stride=owned.index_type==VK_INDEX_TYPE_UINT8_EXT?1:
                owned.index_type==VK_INDEX_TYPE_UINT16?2:owned.index_type==VK_INDEX_TYPE_UINT32?4:0;
            VkDescriptorBufferInfo copy;
            if (!stride) { (void)fg_terminal_reject(s,"index-type"); return; }
            if (!fg_terminal_copy(s,(VkDescriptorBufferInfo){owned.index,
                    owned.index_offset,((uint64_t)owned.first+owned.count)*stride},16,&copy,
                    (vk_fg_terminal_input){.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0})) return;
            owned.index=copy.buffer; owned.index_offset=copy.offset;
        }
        if (o->used && !o->bytes.coherent) {
            VkMappedMemoryRange range={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=o->bytes.memory,
                .offset=0,.size=VK_WHOLE_SIZE};
            if (!fg_result(vkFlushMappedMemoryRanges(V.dev,1,&range),"immutable terminal flush")) return;
        }
        s->terminal=owned; memcpy(s->terminal_sets,o->sets,sizeof s->terminal_sets);
        s->terminal_w=width; s->terminal_h=height; s->terminal_format=format;
        o->ready=1; s->terminal_ready=1;
        if (atomic_load_explicit(&F.job_trace,memory_order_relaxed))
            LOGV("[adaptive-slot-owned] event=ready frame=%llu bytes=%llu writes=%u copies=%u sets=%u snapshots=0",
                (unsigned long long)o->frame,(unsigned long long)o->used,o->writes,o->copies,
                (o->sets[0]!=VK_NULL_HANDLE)+(o->sets[1]!=VK_NULL_HANDLE));
        return;
    }
    s->terminal=*draw;
    memcpy(s->terminal_sets,sets,sizeof s->terminal_sets);
    s->terminal_w=width; s->terminal_h=height; s->terminal_format=format;
    s->terminal_ready=1;
}
int vk_fg_generation_state(const vk_fg_generation *g) {
    return g ? atomic_load_explicit(&g->state,memory_order_acquire) : VK_FG_GENERATION_EMPTY;
}
unsigned vk_fg_generation_slot(const vk_fg_generation *g) { return g ? g->slot : FG_OUTPUTS; }
int vk_fg_generation_stamp_get(const vk_fg_generation *g,vk_fg_generation_stamp *stamp) {
    if (!g || !stamp || !vk_fg_generation_state(g)) return 0;
    *stamp=g->stamp; return 1;
}
int vk_fg_generation_output(const vk_fg_generation *g,vk_post_output *out) {
    if (!g || !out || !vk_fg_generation_state(g)) return 0;
    *out=g->output; return 1;
}
VkCommandBuffer vk_fg_generation_prepare_cb(vk_fg_generation *g) {
    return vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARING ? g->cb : VK_NULL_HANDLE;
}
static void fg_generation_release(vk_fg_generation *g) {
    vk_fg_qcom_output_cancel(g->slot);
    if (g->lease) { (void)vk_replay_lease_end(g->lease,g->ticket); g->lease=0; }
    atomic_fetch_sub_explicit(&F.source[g->source].referenced,1,memory_order_release);
    atomic_fetch_sub_explicit(&F.source[g->previous].referenced,1,memory_order_release);
    atomic_store_explicit(&g->state,VK_FG_GENERATION_EMPTY,memory_order_release);
}
void vk_fg_generation_cancel(vk_fg_generation *g) {
    int state=vk_fg_generation_state(g);
    if (!state) return;
    if (atomic_load_explicit(&F.job_trace,memory_order_relaxed) &&
        !atomic_load_explicit(&g->cancel_reason,memory_order_relaxed))
        atomic_store_explicit(&g->cancel_reason,FG_SLOT_CANCEL_OWNER,memory_order_relaxed);
    atomic_store_explicit(&g->canceled,1,memory_order_release);
    if (state==VK_FG_GENERATION_PREPARING || state==VK_FG_GENERATION_PREPARED)
        fg_generation_release(g);
    /* Submitted/recording/quarantined graphs remain owned, including shutdown
     * and surface loss. No pool, source, descriptor or AHB is freed here. */
}
unsigned vk_fg_generation_cancel_prepared(void) {
    unsigned cancelled=0;
    for (unsigned i=0;i<FG_OUTPUTS;i++) {
        int state=vk_fg_generation_state(&fg_generation[i]);
        if (state!=VK_FG_GENERATION_PREPARING && state!=VK_FG_GENERATION_PREPARED) continue;
        if (atomic_load_explicit(&F.job_trace,memory_order_relaxed))
            atomic_store_explicit(&fg_generation[i].cancel_reason,FG_SLOT_CANCEL_SNAPSHOT_PRESSURE,memory_order_relaxed);
        vk_fg_generation_cancel(&fg_generation[i]);
        cancelled++;
    }
    return cancelled;
}
unsigned vk_fg_generation_cancel_reason(const vk_fg_generation *g) {
    return g?atomic_load_explicit(&g->cancel_reason,memory_order_relaxed):FG_SLOT_CANCEL_NONE;
}
void vk_fg_snapshot_pressure_trace(unsigned owners,unsigned states,unsigned cancelled,int append) {
    if (!atomic_load_explicit(&F.job_trace,memory_order_relaxed)) return;
    atomic_fetch_add_explicit(&fg_snapshot_pressure.observations,1,memory_order_relaxed);
    atomic_fetch_add_explicit(&fg_snapshot_pressure.cancellations,cancelled,memory_order_relaxed);
    atomic_fetch_add_explicit(&fg_snapshot_pressure.appends,append!=0,memory_order_relaxed);
    atomic_fetch_add_explicit(&fg_snapshot_pressure.pending,(owners&1)!=0,memory_order_relaxed);
    atomic_fetch_add_explicit(&fg_snapshot_pressure.quarantined,
        (states&(1u<<VK_FG_GENERATION_QUARANTINED))!=0,memory_order_relaxed);
}
vk_fg_snapshot_pressure_counts vk_fg_snapshot_pressure_get(void) {
    return (vk_fg_snapshot_pressure_counts){
        atomic_load_explicit(&fg_snapshot_pressure.observations,memory_order_relaxed),
        atomic_load_explicit(&fg_snapshot_pressure.cancellations,memory_order_relaxed),
        atomic_load_explicit(&fg_snapshot_pressure.appends,memory_order_relaxed),
        atomic_load_explicit(&fg_snapshot_pressure.pending,memory_order_relaxed),
        atomic_load_explicit(&fg_snapshot_pressure.quarantined,memory_order_relaxed)};
}
unsigned vk_fg_generation_state_mask(void) {
    unsigned mask=0;
    for (unsigned i=0;i<FG_OUTPUTS;i++) {
        unsigned state=(unsigned)vk_fg_generation_state(&fg_generation[i]);
        if (state) mask|=1u<<state;
    }
    return mask;
}
static void fg_generation_cancel_all(void) {
    for (unsigned i=0;i<FG_OUTPUTS;i++) vk_fg_generation_cancel(&fg_generation[i]);
}
vk_fg_generation *vk_fg_generation_prepare(unsigned slot,uint32_t fw,uint32_t fh,
        int srgb,const vk_fg_generation_stamp *stamp) {
    if ((!vk_fg_adaptive() && !fg_generation_test_bridge) || !vk_fg_enabled() || !stamp || !stamp->frame ||
        !stamp->epoch || slot>=FG_OUTPUTS || !fw || !fh) return NULL;
    fg_consume_reset();
    if (!F.ready || F.invalid || F.current<0 || F.history<0 || F.current==F.history ||
        stamp->frame!=vk_fg_real_frame_id() || !fg_generation_engine_ready()) return NULL;
    fg_source *s=&F.source[F.current];
    fg_output *o=&F.output[slot];
    vk_fg_generation *g=&fg_generation[slot];
    if (vk_fg_generation_state(g) || atomic_load_explicit(&o->busy,memory_order_acquire) ||
        s->generation_template || FQ.generated[F.current] || !s->predicted || !s->prepared || s->ui_ready!=3 ||
        !s->terminal_ready || (!fg_generation_test_bridge && (!FQ.copy[F.current] || !FQ.copy[F.history])) ||
        fw>V.props.limits.maxImageDimension2D || fh>V.props.limits.maxImageDimension2D ||
        fabs((double)fw/fh-(double)s->scene.w/s->scene.h)>0.01) return NULL;
    VkFormat format=srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!fg_image_make(&o->image,fw,fh,format,VK_IMAGE_ASPECT_COLOR_BIT,1) ||
        !fg_image_make(&s->final,s->terminal_w,s->terminal_h,s->terminal_format,VK_IMAGE_ASPECT_COLOR_BIT,1) ||
        (!FQ.pipeline && !fg_make_pipeline(s->scene.format,&FQ.pipeline,2))) return NULL;
    if (!g->pool) {
        VkCommandPoolCreateInfo pi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=V.qfam};
        if (!fg_result(vkCreateCommandPool(V.dev,&pi,NULL,&g->pool),"template command pool")) return NULL;
        VkCommandBufferAllocateInfo ai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=g->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        if (!fg_result(vkAllocateCommandBuffers(V.dev,&ai,&g->cb),"template command storage")) return NULL;
    } else if (!g->cb || !fg_result(vkResetCommandPool(V.dev,g->pool,0),"template pool reset")) return NULL;
    uint64_t lease=s->owned.ready ? vk_replay_lease_begin_immutable() : vk_replay_lease_begin();
    if (!lease) return NULL;
    atomic_store_explicit(&vk_fg_generation_snapshot_retained,1,memory_order_relaxed);
    VkImageView view=s->native_scene_ready ? s->scene_scaled.raw : s->scene.raw;
    if (!fg_generation_test_bridge &&
        !vk_fg_qcom_output_prepare((unsigned)F.history,(unsigned)F.current,slot,&view)) {
        (void)vk_replay_lease_end(lease,0); return NULL;
    }
#ifdef TOTK_VK_TESTS
    if (fg_generation_test_bridge && !g->test_bridge) {
        VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!fg_result(vkCreateSemaphore(V.dev,&si,NULL,&g->test_bridge),"test bridge semaphore")) {
            (void)vk_replay_lease_end(lease,0); return NULL;
        }
    }
#endif
    g->lease=lease; g->ticket=0; g->first_ticket=0; g->charge_frame=0;
    g->stamp=*stamp; g->slot=slot; g->source=(unsigned)F.current; g->previous=(unsigned)F.history;
    g->immutable_terminal=s->owned.ready;
    g->output=(vk_post_output){o->image.image,o->image.display,fw,fh,format};
    g->profile_mask=0; g->profile_copy_started=0;
    g->trace.enabled=0;
#ifdef TOTK_VK_TESTS
    g->test_phase=0;
#endif
    atomic_store_explicit(&g->cancel_reason,FG_SLOT_CANCEL_NONE,memory_order_relaxed);
    atomic_store_explicit(&g->canceled,0,memory_order_relaxed);
    atomic_fetch_add_explicit(&s->referenced,1,memory_order_release);
    atomic_fetch_add_explicit(&F.source[g->previous].referenced,1,memory_order_release);
    atomic_store_explicit(&g->state,VK_FG_GENERATION_PREPARING,memory_order_release);
    s->generation_template=1;
    VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!fg_result(vkBeginCommandBuffer(g->cb,&bi),"template begin")) {
        vk_fg_generation_cancel(g); return NULL;
    }
    VkCommandBuffer cb=g->cb;
    if (g->immutable_terminal && s->owned.used)
        fg_buffer_barrier(cb,&s->owned.bytes,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
    if (!fg_generation_test_bridge) vk_fg_qcom_acquire(cb,slot);
    fg_render_record(cb,s,&s->canvas,FQ.pipeline,view,
        fg_generation_test_bridge ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,5,1);
    if (!fg_generation_test_bridge) vk_fg_qcom_release(cb,slot);
    /* Exact terminal replay, record-only: no F.current lookup, profile mark,
     * ui_ready/transferred/pass mutation, allocation or callback at dispatch.
     * DIRECT_OUT replays straight into the output only when its pipeline's
     * attachment format and frozen viewport/scissor extent are the output's;
     * the mode-2 copy would then reproduce the same encoded texels. */
    int direct=atomic_load_explicit(&fg_adaptive_direct_config,memory_order_relaxed)==1 && !fg_dump_frame();
    if (direct) {
        direct=s->terminal_format==format && s->terminal_w==fw && s->terminal_h==fh;
        F.lean_counts[direct ? VK_FG_LEAN_DIRECT : VK_FG_LEAN_DIRECT_FALLBACK]++;
    }
    fg_image *terminal=direct ? &o->image : &s->final;
    fg_barrier(cb,terminal->image,(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=terminal->display,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{s->terminal_w,s->terminal_h}},
        .layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&attachment};
    vkCmdBeginRendering(cb,&ri);
    vk_fg_replay_draw(cb,&s->terminal,s->terminal_sets);
    vkCmdEndRendering(cb); vk_barrier_all(cb);
    unsigned query=(FG_SOURCES+slot)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    if (F.profile_queries) fg_profile_begin(cb,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_RENDER));
    if (!direct) fg_render_record(cb,s,&o->image,F.pipeline,s->final.raw,VK_IMAGE_LAYOUT_GENERAL,2,1);
    if (F.profile_queries) {
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
            fg_profile_output_query(slot,FG_PROFILE_OUTPUT_RENDER)+1);
        g->profile_mask=1;
        fg_profile_begin(cb,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_POST));
    }
    atomic_store_explicit(&o->raw,o->image.raw,memory_order_release);
    return g;
}
int vk_fg_generation_prepare_finish(vk_fg_generation *g) {
    if (vk_fg_generation_state(g)!=VK_FG_GENERATION_PREPARING) return 0;
    vkCmdWriteTimestamp2(g->cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,(FG_SOURCES+g->slot)*2+1);
    if (F.profile_queries && g->profile_copy_started) {
        vkCmdWriteTimestamp2(g->cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
            fg_profile_output_query(g->slot,FG_PROFILE_OUTPUT_COPY)+1);
        g->profile_mask|=UINT32_C(1)<<(FG_PROFILE_OUTPUT_COPY-FG_PROFILE_SOURCE_STAGES);
    }
    if (!fg_result(vkEndCommandBuffer(g->cb),"template close")) {
        vk_fg_generation_cancel(g); return 0;
    }
    atomic_store_explicit(&g->state,VK_FG_GENERATION_PREPARED,memory_order_release);
    return 1;
}
static void fg_generation_unreserve(const fg_qcom_reservation *saved,uint64_t cached) {
    pthread_mutex_lock(&fg_stats_mu);
    if (fg_qcom_reservation_cancel(&F.budget,saved)) F.budget_reserved_frame=cached;
    pthread_mutex_unlock(&fg_stats_mu);
}
static int fg_generation_dispatch_result(int result) {
    vk_fg_dispatch_trace_current=NULL;
    return result;
}
int vk_fg_generation_dispatch(vk_fg_generation *g,float phase,uint64_t deadline,vk_post_output *out) {
    fg_dispatch_trace trace={.enabled=vk_fg_job_trace_enabled(),
        .source=g ? g->stamp.frame : 0,.charge=F.budget_frame+1,.output=g ? g->slot : FG_OUTPUTS,
        .previous=g ? g->previous : 0,.current=g ? g->source : 0,.phase=phase,.deadline=deadline,
        .budget_status=-1,.budget_reason=-1,.probe=-1,.cooling=-1,.capacity=-1,
        .qcom_warmed=-1,.qcom_suspended=-1,.qcom_cooling=-1,.qcom_quarantined=-1,.qcom_admitted=-1};
    vk_fg_dispatch_trace_current=&trace;
    if (FG_DISPATCH_GUARD(&trace,"entry","generation-not-prepared",vk_fg_generation_state(g)!=VK_FG_GENERATION_PREPARED) ||
        FG_DISPATCH_GUARD(&trace,"entry","output-null",!out) ||
        FG_DISPATCH_GUARD(&trace,"entry","deadline-zero",!deadline) ||
        FG_DISPATCH_GUARD(&trace,"entry","deadline-entry",host_now_ns()>=deadline) ||
        FG_DISPATCH_GUARD(&trace,"entry","phase-nonfinite",!isfinite(phase)) ||
        FG_DISPATCH_GUARD(&trace,"entry","phase-nonpositive",phase<=0) ||
        FG_DISPATCH_GUARD(&trace,"entry","phase-above-one",phase>1) ||
        FG_DISPATCH_GUARD(&trace,"entry","generation-canceled",atomic_load_explicit(&g->canceled,memory_order_acquire)) ||
        FG_DISPATCH_GUARD(&trace,"entry","reset-requested",atomic_load_explicit(&fg_reset_requested,memory_order_acquire)) ||
        FG_DISPATCH_GUARD(&trace,"entry","framegen-disabled",!vk_fg_enabled()) ||
        FG_DISPATCH_GUARD(&trace,"entry","qcom-cannot-generate",!fg_generation_engine_ready()))
        return fg_generation_dispatch_result(0);
    /* Budget belongs to the still-open accounting interval, not the already
     * published source's possibly finalized interval. No real tickets between
     * that source and this boundary are relabeled synthetic. */
    if (FG_DISPATCH_GUARD(&trace,"admission","budget-pre-admit",!fg_budget_pre_admit()) ||
        FG_DISPATCH_GUARD(&trace,"admission","charge-interval-already-used",fg_generation_charge_frame==F.budget_frame+1) ||
        FG_DISPATCH_GUARD(&trace,"admission","deadline-before-optional",host_now_ns()>=deadline) ||
        FG_DISPATCH_GUARD(&trace,"admission","optional-begin-refused",!(g->immutable_terminal ?
            vk_submit_optional_begin_immutable() : vk_submit_optional_begin())))
        return fg_generation_dispatch_result(0);
    fg_dispatch_trace_event(&trace,"stage","optional-begin","accepted");
    pthread_mutex_lock(&fg_stats_mu);
    fg_qcom_reservation reservation=fg_qcom_reservation_save(&F.budget,F.budget_frame+1);
    uint64_t cached_reservation=F.budget_reserved_frame;
    pthread_mutex_unlock(&fg_stats_mu);
    trace.cooling=trace.capacity=-1;
    if (FG_DISPATCH_GUARD(&trace,"reservation","budget-reserve",!fg_budget_reserve_work()) ||
        FG_DISPATCH_GUARD(&trace,"reservation","deadline-after-reserve",host_now_ns()>=deadline)) {
        fg_generation_unreserve(&reservation,cached_reservation);
        vk_submit_optional_cancel(); return fg_generation_dispatch_result(0);
    }
    g->first_ticket=mtl_last_fence()+1;
    g->charge_frame=F.budget_frame+1;
    fg_dispatch_trace_tickets(&trace,g->first_ticket,0);
    g->external_ms=NAN;
    atomic_store_explicit(&g->state,VK_FG_GENERATION_RECORDING,memory_order_release);
    fg_dispatch_trace_event(&trace,"stage","qcom-dispatch-begin","submit-call");
    int result;
#ifdef TOTK_VK_TESTS
    if (fg_generation_test_bridge) {
        /* Only Android's external bridge is substituted. Commands, binary
         * semaphore split, adoption, FIFO tickets and retirement are real. */
        vk_submit_external_enable();
        result=vk_submit_signal(g->test_bridge)!=0;
        if (result) vk_submit_wait(g->test_bridge);
        g->test_phase=phase;
        g->external_ms=0;
    } else
#endif
        result=vk_fg_qcom_output_dispatch(g->previous,g->source,g->slot,g->charge_frame,phase,deadline);
    fg_dispatch_trace_event(&trace,"stage","qcom-dispatch-end",result==1 ? "accepted" : "refused");
    if (FG_DISPATCH_GUARD(&trace,"handoff","qcom-output-refused",result!=1)) {
        fg_dispatch_trace_event(&trace,"stage","refused-submit-begin","submit-call");
        (void)vk_cb(); g->ticket=vk_submit_now(); vk_submit_optional_end();
        fg_dispatch_trace_tickets(&trace,g->first_ticket,g->ticket);
        fg_dispatch_trace_event(&trace,"stage","refused-submit-end","cpu-return-not-gpu-done");
        /* Even a refused/canceled handoff consumed these optional tickets.
         * Classify only that actual range and retain it for post-hoc auditing. */
        if (!FG_DISPATCH_GUARD(&trace,"handoff","refused-submit-ticket-zero",!g->ticket)) {
            vk_fg_synthetic_range(g->first_ticket,g->ticket);
            pthread_mutex_lock(&fg_stats_mu);
            int charged=fg_qcom_prepared_charge(&F.prepared_budget,g->charge_frame,g->first_ticket,g->ticket);
            fg_prepared_budget_audit();
            pthread_mutex_unlock(&fg_stats_mu);
            if (trace.enabled) trace.charge_status=charged ? 1 : 2;
            fg_dispatch_trace_event(&trace,"stage","refused-range-charge","existing-charge-result");
        }
        if (FG_DISPATCH_GUARD(&trace,"handoff","qcom-output-quarantined",result<0)) {
            atomic_store_explicit(&g->state,VK_FG_GENERATION_QUARANTINED,memory_order_release);
            return fg_generation_dispatch_result(-1);
        }
        fg_generation_unreserve(&reservation,cached_reservation);
        atomic_store_explicit(&g->state,VK_FG_GENERATION_PREPARED,memory_order_release);
        vk_fg_generation_cancel(g); return fg_generation_dispatch_result(0);
    }
    fg_generation_charge_frame=g->charge_frame;
    vk_submit_adopt_primary(g->cb);
    fg_output *o=&F.output[g->slot];
    o->source=g->source; o->previous=g->previous;
    o->profile_mask=g->profile_mask; o->profile_copy_started=g->profile_copy_started;
    F.output_budget_frame[g->slot]=g->charge_frame;
    FQ.generated[g->source]=1;
    atomic_store_explicit(&o->ended,1,memory_order_relaxed);
    atomic_store_explicit(&o->busy,1,memory_order_release);
    pthread_mutex_lock(&fg_stats_mu); F.passes[0]++; F.passes[2]++; F.passes[3]++; F.recorded++; pthread_mutex_unlock(&fg_stats_mu);
    atomic_store_explicit(&g->state,VK_FG_GENERATION_RECORDING,memory_order_release);
    if (trace.enabled) g->trace=trace;
    fg_dispatch_trace_event(&trace,"stage","dispatch-adopted","recording");
    *out=g->output; return fg_generation_dispatch_result(1);
}
uint64_t vk_fg_generation_seal(vk_fg_generation *g) {
    fg_dispatch_trace absent={.enabled=vk_fg_job_trace_enabled(),
        .source=g ? g->stamp.frame : 0,.charge=g ? g->charge_frame : 0,
        .output=g ? g->slot : FG_OUTPUTS,.phase=NAN,
        .budget_status=-1,.budget_reason=-1,.probe=-1,.cooling=-1,.capacity=-1,
        .qcom_warmed=-1,.qcom_suspended=-1,.qcom_cooling=-1,.qcom_quarantined=-1,.qcom_admitted=-1};
    fg_dispatch_trace *trace=g && g->trace.enabled ? &g->trace : &absent;
    if (FG_DISPATCH_GUARD(trace,"seal","generation-not-recording",vk_fg_generation_state(g)!=VK_FG_GENERATION_RECORDING))
        return 0;
    fg_dispatch_trace_event(trace,"stage","seal-submit-begin","submit-call");
    g->ticket=vk_submit_now(); vk_submit_optional_end();
    fg_dispatch_trace_tickets(trace,g->first_ticket,g->ticket);
    fg_dispatch_trace_event(trace,"stage","seal-submit-end","cpu-return-not-gpu-done");
    if (FG_DISPATCH_GUARD(trace,"seal","seal-ticket-zero",!g->ticket) ||
        FG_DISPATCH_GUARD(trace,"seal","replay-lease-end-refused",!vk_replay_lease_end(g->lease,g->ticket))) {
        atomic_store_explicit(&g->state,VK_FG_GENERATION_QUARANTINED,memory_order_release);
        return 0;
    }
    g->lease=0;
    vk_fg_synthetic_range(g->first_ticket,g->ticket);
    pthread_mutex_lock(&fg_stats_mu);
    int charged=fg_qcom_prepared_charge(&F.prepared_budget,g->charge_frame,g->first_ticket,g->ticket);
    fg_prepared_budget_audit();
    pthread_mutex_unlock(&fg_stats_mu);
    if (trace->enabled) trace->charge_status=charged ? 1 : 2;
    if (FG_DISPATCH_GUARD(trace,"seal","prepared-charge-refused",!charged))
        atomic_store_explicit(&g->canceled,1,memory_order_release);
    atomic_store_explicit(&g->state,VK_FG_GENERATION_SUBMITTED,memory_order_release);
    fg_dispatch_trace_event(trace,"stage","seal-submitted","cpu-submit-complete");
    return g->ticket;
}
int vk_fg_generation_retire(vk_fg_generation *g) {
    if (vk_fg_generation_state(g)!=VK_FG_GENERATION_SUBMITTED || !mtl_wait(g->ticket,0)) return 0;
    (void)vk_fg_complete(g->slot);
    return vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY;
}
/* The current REAL cost is normally unknown here: dispatch follows the source
 * seal, while the charging interval ends at the NEXT real. Publication uses a
 * labeled complete-history predictor, never the source REAL as an actual cost.
 * All incremental tickets/GL must already be measured; actual full-interval
 * verification and overshoot backoff happen independently, even after cancel.
 * Caller holds fg_stats_mu. No finalized measurement is modified. */
static fg_budget_decision fg_generation_completed(const vk_fg_generation *g) {
    const fg_budget_external_group *external=&FQ.adaptive_cost[g->charge_frame%FG_BUDGET_FRAMES];
    fg_budget_decision d=fg_qcom_prepared_completed(&F.budget,&F.prepared_budget,
        g->stamp.frame,g->charge_frame,g->first_ticket,g->ticket,g->external_ms,
        external->frame,external->ms,external->pending,external->valid);
    return d;
}
static float fg_adaptive_target_phase; /* recording-owner only */
int vk_fg_generate_phase(VkCommandBuffer cb, unsigned slot, uint32_t fw, uint32_t fh,
        int srgb, float phase, vk_post_output *out) {
    if (!vk_fg_adaptive()) return vk_fg_generate(cb,slot,fw,fh,srgb,out);
    if (!isfinite(phase) || phase<=0 || phase>1) return 0;
    fg_adaptive_target_phase=phase;
    return vk_fg_generate(cb,slot,fw,fh,srgb,out);
}

int vk_fg_generate(VkCommandBuffer cb, unsigned slot, uint32_t fw, uint32_t fh,
                   int srgb, vk_post_output *out) {
    if (!vk_fg_enabled()) return 0;
    fg_consume_reset();
    if (!F.ready || F.current < 0 || F.invalid || (F.generated && !vk_fg_adaptive()) || !cb || !out ||
        !fw || !fh || slot >= FG_OUTPUTS) { if (F.diag) F.admission[0]++; return 0; }
    fg_source *s = &F.source[F.current];
    if (vk_fg_adaptive() && s->generation_template) return 0; /* no mixed/multi-child replay */
    fg_output *o = &F.output[slot];
    if (!s->predicted) { if (F.diag) F.admission[1]++; return 0; }
    if (!s->prepared || (!vk_fg_adaptive() && !s->transferred)) { if (F.diag) F.admission[2]++; return 0; }
    if (atomic_load_explicit(&o->busy,memory_order_acquire)) { if (F.diag) F.admission[3]++; return 0; }
    if (!fg_budget_pre_admit()) return 0;
    int masked=!vk_fg_adaptive() && vk_fg_interpolating() && !((vk_fg_qcom_requested() || vk_fg_lsfg_requested()) &&
        FQ.generated[F.current]) && s->pair_source >= 0 &&
        (s->authority_snapshot || F.source[s->pair_source].authority_snapshot);
    if (masked) {
        const fg_source *old=&F.source[s->pair_source];
        if (!old->real_ready || old->real.w != s->final.w || old->real.h != s->final.h ||
            vk_fg_raw_format(old->real.format) != vk_fg_raw_format(s->final.format))
            return fg_real_reject("earlier real fallback and replayed terminal extent/encoding differ");
    }
    if (fw > V.props.limits.maxImageDimension2D || fh > V.props.limits.maxImageDimension2D ||
        fabs((double)fw/fh - (double)s->scene.w/s->scene.h) > 0.01) {
        if (F.diag) F.admission[4]++;
        return 0;
    }
    VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!fg_image_make(&o->image,fw,fh,format,VK_IMAGE_ASPECT_COLOR_BIT,1)) {
        if (F.diag) F.admission[5]++;
        return 0;
    }
    if (vk_fg_adaptive()) {
        if (!vk_fg_qcom_active() || F.history<0 || !s->terminal_ready ||
            !s->predicted || s->ui_ready!=3 || !FQ.copy[F.current] || !FQ.copy[F.history] ||
            !vk_fg_qcom_can_generate() || !fg_budget_reserve_work() ||
            !fg_image_make(&s->final,s->terminal_w,s->terminal_h,s->terminal_format,VK_IMAGE_ASPECT_COLOR_BIT,1))
            return 0;
        if (!FQ.pipeline && !fg_make_pipeline(s->scene.format,&FQ.pipeline,2)) return 0;
        VkImage image; VkImageView view;
        if (!vk_fg_qcom_extrapolate((unsigned)F.history,(unsigned)F.current,slot,
                F.budget_frame+1,fg_adaptive_target_phase,&image,&view)) return 0;
        cb=vk_cb();
        fg_render(cb,s,&s->canvas,FQ.pipeline,view,VK_IMAGE_LAYOUT_GENERAL,5);
        vk_fg_qcom_release(cb,slot);
        FQ.generated[F.current]=1; s->pair_source=F.history;
        /* Canvas and exact terminal image were validated/allocated before the
         * external job. No allocation/refusal remains after handing it off. */
        (void)vk_fg_replay_begin(cb,s->terminal_w,s->terminal_h,s->terminal_format,1);
        vk_fg_replay_draw(cb,&s->terminal,s->terminal_sets);
        vk_fg_replay_end(cb,1);
    }
    unsigned query = (FG_SOURCES+slot)*2;
    vkCmdResetQueryPool(cb,F.queries,query,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,query);
    o->profile_mask=0;
    o->profile_copy_started=0;
    if (F.profile_queries) fg_profile_begin(cb,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_RENDER));
    fg_render(cb,s,&o->image,F.pipeline,s->final.raw,VK_IMAGE_LAYOUT_GENERAL,masked ? 4 : 2);
    if (F.profile_queries) {
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
            fg_profile_output_query(slot,FG_PROFILE_OUTPUT_RENDER)+1);
        o->profile_mask=1;
        fg_profile_begin(cb,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_POST));
    }
    F.passes[3]++;
    o->source=(unsigned)F.current;
    o->previous=vk_fg_interpolating() && s->pair_source >= 0 ? (unsigned)s->pair_source : FG_SOURCES;
    if (o->previous < FG_SOURCES)
        atomic_fetch_add_explicit(&F.source[o->previous].referenced,1,memory_order_release);
    atomic_store_explicit(&o->ended,0,memory_order_relaxed);
    F.output_budget_frame[slot]=F.budget_frame+1;
    atomic_fetch_add_explicit(&s->referenced,1,memory_order_release);
    atomic_store_explicit(&o->raw,o->image.raw,memory_order_release);
    atomic_store_explicit(&o->busy,1,memory_order_release);
    F.generated=1;
    pthread_mutex_lock(&fg_stats_mu); F.recorded++; pthread_mutex_unlock(&fg_stats_mu);
    if (F.diag) F.admission[6]++;
    *out=(vk_post_output){o->image.image,o->image.display,fw,fh,format};
    return 1;
}
VkImageView vk_fg_raw_view(unsigned slot) {
    return slot < FG_OUTPUTS ? atomic_load_explicit(&F.output[slot].raw,memory_order_acquire) : VK_NULL_HANDLE;
}
void vk_fg_output_copy_begin(VkCommandBuffer cb, unsigned slot) {
    if (!F.profile_queries || !cb || slot>=FG_OUTPUTS) return;
    fg_output *o=&F.output[slot];
    vk_fg_generation *g=&fg_generation[slot];
    if (vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARING && cb==g->cb) {
        if (g->profile_copy_started) return;
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
            fg_profile_output_query(slot,FG_PROFILE_OUTPUT_POST)+1);
        g->profile_mask|=UINT32_C(1)<<(FG_PROFILE_OUTPUT_POST-FG_PROFILE_SOURCE_STAGES);
        fg_profile_begin(cb,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_COPY));
        g->profile_copy_started=1; return;
    }
    if (!atomic_load_explicit(&o->busy,memory_order_acquire) ||
        atomic_load_explicit(&o->ended,memory_order_acquire) || o->profile_copy_started) return;
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
        fg_profile_output_query(slot,FG_PROFILE_OUTPUT_POST)+1);
    o->profile_mask|=UINT32_C(1)<<(FG_PROFILE_OUTPUT_POST-FG_PROFILE_SOURCE_STAGES);
    fg_profile_begin(cb,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_COPY));
    o->profile_copy_started=1;
}
void vk_fg_gpu_end(VkCommandBuffer cb, unsigned slot) {
    if (!cb || slot >= FG_OUTPUTS) return;
    fg_output *o=&F.output[slot];
    if (!atomic_load_explicit(&o->busy,memory_order_acquire) ||
        atomic_load_explicit(&o->ended,memory_order_acquire)) return;
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.queries,(FG_SOURCES+slot)*2+1);
    if (F.profile_queries && o->profile_copy_started) {
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,F.profile_queries,
            fg_profile_output_query(slot,FG_PROFILE_OUTPUT_COPY)+1);
        o->profile_mask|=UINT32_C(1)<<(FG_PROFILE_OUTPUT_COPY-FG_PROFILE_SOURCE_STAGES);
    }
    atomic_store_explicit(&o->ended,1,memory_order_release);
}
void vk_fg_diag_dump(unsigned slot) {
    uint64_t selected=fg_dump_frame();
    if (!selected || fg_dump_captured || slot>=FG_OUTPUTS) return;
    fg_output *o=&F.output[slot];
    uint64_t frame=F.output_budget_frame[slot];
    if (frame<selected || o->source>=FG_SOURCES || o->previous>=FG_SOURCES ||
        !atomic_load_explicit(&o->busy,memory_order_acquire) ||
        !atomic_load_explicit(&o->ended,memory_order_acquire)) return;
    fg_dump_captured=1;
    fg_source *s=&F.source[o->source], *old=&F.source[o->previous];
    fg_image model={.w=s->scene.w,.h=s->scene.h,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
    if (FQ.generated[o->source]==2)
        (void)vk_fg_lsfg_dump_image(o->source,&model.image,&model.format,&model.w,&model.h);
    struct {
        const char *name;
        fg_image *image;
        VkImageLayout layout;
        VkDeviceSize offset, bytes;
    } images[]={
        {"earlier-scene",&old->scene,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {"later-scene",&s->scene,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {"earlier-depth",&old->depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {"later-depth",&s->depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {"earlier-ui-black",&old->ui_black,VK_IMAGE_LAYOUT_GENERAL},
        {"earlier-ui-white",&old->ui_white,VK_IMAGE_LAYOUT_GENERAL},
        {"later-ui-black",&s->ui_black,VK_IMAGE_LAYOUT_GENERAL},
        {"later-ui-white",&s->ui_white,VK_IMAGE_LAYOUT_GENERAL},
        {"composed-canvas",&s->canvas,VK_IMAGE_LAYOUT_GENERAL},
        {"authority-fallback",&s->fallback,VK_IMAGE_LAYOUT_GENERAL},
        {"replayed-terminal",&s->final,VK_IMAGE_LAYOUT_GENERAL},
        {"earlier-real-terminal",&old->real,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {"later-real-terminal",&s->real,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {"generated-output",&o->image,VK_IMAGE_LAYOUT_GENERAL},
        {"lsfg-model-output",&model,VK_IMAGE_LAYOUT_GENERAL}
    };
    VkDeviceSize bytes=0;
    for (unsigned i=0;i<sizeof images/sizeof images[0];i++) {
        fg_image *im=images[i].image;
        unsigned texel=0;
        switch (im->format) {
        case VK_FORMAT_R8_UNORM: texel=1; break;
        case VK_FORMAT_D16_UNORM: texel=2; break;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_D32_SFLOAT: texel=4; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: texel=8; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: texel=16; break;
        default: break;
        }
        if (!im->image || !texel) continue;
        bytes=(bytes+15)&~(VkDeviceSize)15;
        images[i].offset=bytes;
        images[i].bytes=(VkDeviceSize)im->w*im->h*texel;
        bytes+=images[i].bytes;
    }
    fg_buffer readback={0};
    if (!bytes || !fg_buffer_make(&readback,bytes,1)) return;
    VkCommandBuffer cb=vk_cb();
    for (unsigned i=0;i<sizeof images/sizeof images[0];i++) {
        if (!images[i].bytes) continue;
        fg_image *im=images[i].image;
        VkImageSubresourceRange range={im->aspect,0,1,0,1};
        fg_barrier(cb,im->image,range,images[i].layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy={.bufferOffset=images[i].offset,
            .imageSubresource={im->aspect,0,0,1},.imageExtent={im->w,im->h,1}};
        vkCmdCopyImageToBuffer(cb,im->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
        fg_barrier(cb,im->image,range,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[i].layout,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    fg_buffer_barrier(cb,&readback,VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
    vk_flush_wait(); /* Selected diagnostic frame only; its timings are not a benchmark. */
    if (!readback.coherent) {
        VkMappedMemoryRange range={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=readback.memory,.offset=0,.size=VK_WHOLE_SIZE};
        if (vkInvalidateMappedMemoryRanges(V.dev,1,&range)!=VK_SUCCESS) {
            LOGV("[framegen-dump] readback invalidation failed");
            fg_buffer_free(&readback,1); return;
        }
    }
    const char *base=getenv("TOTK_VK_SHOT_DIR");
    if (!base || !*base) base="shots";
    char dir[1024],path[1200];
    mkdir(base,0755);
    snprintf(dir,sizeof dir,"%s/framegen-%06llu",base,(unsigned long long)frame);
    mkdir(dir,0755);
    snprintf(path,sizeof path,"%s/index.txt",dir);
    FILE *index=fopen(path,"w");
    if (index) {
        fprintf(index,"real-frame-id=%llu earlier-source=%u later-source=%u output-slot=%u\n"
            "Generated midpoint is between earlier/later real terminals; names identify parity without guessing video parity.\n"
            "Raw native Vulkan format, tightly packed rows, top-left origin; no normalization or gamma conversion.\n",
            (unsigned long long)frame,o->previous,o->source,slot);
        if (model.image) fprintf(index,"LSFG midpoint=0.5 output-index=0 intermediate-count=1; game VkDevice/queue; native float scene input, RGBA16F output; model and composed canvas are distinct images.\n");
        for (unsigned side=0;side<2;side++) {
            const fg_source *source=side ? s : old;
            fprintf(index,"%s-real-frame=%llu scene-format=%u scene-extent=%ux%u valid-scene-crop=0,0,%u,%u normalized-UV=0,0,1,1 depth-extent=%ux%u depth-geometry-observed=%d accepted-depth-viewport=%.9g,%.9g,%.9g,%.9g accepted-depth-scissor=%d,%d,%d,%d\n",
                side ? "later" : "earlier",(unsigned long long)source->captured_real_frame,
                source->scene.format,source->scene.w,source->scene.h,source->scene.w,source->scene.h,
                source->depth.w,source->depth.h,source->captured_geometry_valid,
                source->captured_viewport[0],source->captured_viewport[1],
                source->captured_viewport[2],source->captured_viewport[3],source->captured_scissor[0],
                source->captured_scissor[1],source->captured_scissor[2],source->captured_scissor[3]);
            fprintf(index,"%s-source-epoch=%llu capture-cb=0x%llx capture-ticket-predicted=%llu "
                "retire-at-dump=%llu references-at-dump=%d\n",side ? "later" : "earlier",
                (unsigned long long)source->capture_epoch,(unsigned long long)(uintptr_t)source->capture_cb,
                (unsigned long long)source->capture_ticket_predicted,(unsigned long long)source->retire,
                atomic_load_explicit(&source->referenced,memory_order_acquire));
            fprintf(index,"%s-camera",side ? "later" : "earlier");
            for (unsigned j=0;j<84;j++) fprintf(index," %.9g",source->camera[j]);
            fprintf(index,"\n%s-transform",side ? "later" : "earlier");
            const float *transform=side ? s->next_transform : s->transform;
            for (unsigned j=0;j<16;j++) fprintf(index," %.9g",transform[j]);
            fputc('\n',index);
        }
    }
    unsigned written=0;
    for (unsigned i=0;i<sizeof images/sizeof images[0];i++) {
        fg_image *im=images[i].image;
        if (!images[i].bytes) {
            if (index) fprintf(index,"%s unavailable format=%u\n",images[i].name,im->format);
            continue;
        }
        snprintf(path,sizeof path,"%s/%s_%u_%ux%u.raw",dir,images[i].name,im->format,im->w,im->h);
        FILE *file=fopen(path,"wb");
        int ok=file && fwrite((char *)readback.mapped+images[i].offset,1,(size_t)images[i].bytes,file)==images[i].bytes;
        if (file && fclose(file)) ok=0;
        if (ok) written++;
        if (index) fprintf(index,"%s_%u_%ux%u.raw bytes=%llu written=%d\n",images[i].name,im->format,im->w,im->h,
            (unsigned long long)images[i].bytes,ok);
    }
    if (vk_fg_lsfg_probe_requested()) {
        int ok=vk_fg_lsfg_probe_dump(o->source,frame,dir);
        if (index) fprintf(index,"lsfg-probe.txt status=%s (probe errors do not discard native raw originals)\n",
            ok ? "ok" : "error");
        LOGV("[framegen-dump] LSFG probe frame=%llu status=%s receipt=%s/lsfg-probe.txt",
            (unsigned long long)frame,ok ? "ok" : "error",dir);
    }
    if (index) fclose(index);
    LOGV("[framegen-dump] real-frame-id=%llu %u images in %s (blocking DIAG capture; discard timing window)",
        (unsigned long long)frame,written,dir);
    fg_buffer_free(&readback,1);
}
int vk_fg_complete(unsigned slot) {
    if (slot >= FG_OUTPUTS) return 0;
    fg_output *o=&F.output[slot];
    vk_fg_generation *g=&fg_generation[slot];
    int state=vk_fg_generation_state(g);
    if (state && (state!=VK_FG_GENERATION_SUBMITTED || !mtl_wait(g->ticket,0))) return 0;
    vk_fg_qcom_harvest();
    vk_fg_lsfg_harvest();
    if (state==VK_FG_GENERATION_SUBMITTED) {
        int expected=1;
        if (!atomic_compare_exchange_strong_explicit(&o->busy,&expected,2,
            memory_order_acq_rel,memory_order_acquire)) return 0;
    } else if (atomic_load_explicit(&o->busy,memory_order_acquire)!=1) return 0;
    fg_source *s=&F.source[o->source];
    double capture_ms=0, generate_ms=0;
    int ended=atomic_load_explicit(&o->ended,memory_order_acquire);
    if (!ended) fg_suspend("generated GPU timing boundary missing");
    int measured=ended && fg_source_timestamp(o->source,&capture_ms) && fg_timestamp((FG_SOURCES+slot)*2,&generate_ms);
    fg_profile_source_publish(o->source);
    if (o->previous<FG_SOURCES) fg_profile_source_publish(o->previous);
    if (ended) fg_profile_output_publish(slot);
    s->timed=0;
    int qcom=state==VK_FG_GENERATION_SUBMITTED ||
        ((vk_fg_qcom_requested() || vk_fg_lsfg_requested()) && FQ.generated[o->source]);
    int status=qcom ? 1 : fg_pair_accept(s), authority_ok=status > 0, over_limit=status == 0;
    if (!qcom) fg_pair_stats(s);
    /* QCOM/scheduler children never consult the authority VALUE; only an
       existing snapshot's recorded timestamps must be readable. A lean source
       has no snapshot, so its authority cost is exactly zero and available. */
    if (o->previous < FG_SOURCES) {
        double ignored;
        if (!fg_authority_timestamp(o->previous,&ignored)) authority_ok=0;
    }
    if (over_limit) {
        pthread_mutex_lock(&fg_stats_mu); F.authority_skips++; pthread_mutex_unlock(&fg_stats_mu);
    }
    int accepted=measured && authority_ok;
    if (measured) {
        double ms=capture_ms+generate_ms;
        fg_budget(ms);
        pthread_mutex_lock(&fg_stats_mu);
        F.sum_ms+=ms; if (ms>F.max_ms) F.max_ms=ms; F.measured++;
        if (vk_fg_interpolating()) {
            int budget_ok;
            if (F.budget_override) {
                budget_ok=fg_budget_fixed(F.budget_ms,ms);
                if (!budget_ok) F.budget_post_skips[1]++;
            } else {
                fg_budget_decision d=state==VK_FG_GENERATION_SUBMITTED ?
                    fg_generation_completed(g) : fg_budget_completed(&F.budget,F.output_budget_frame[slot]);
                budget_ok=d.status == FG_BUDGET_ACCEPT;
                if (state==VK_FG_GENERATION_SUBMITTED) {
                    int completion_accept=authority_ok && budget_ok &&
                        !atomic_load_explicit(&g->canceled,memory_order_acquire);
                    fg_qcom_prepared_decision(&F.prepared_budget,g->charge_frame,&d,completion_accept);
                    LOGV("[framegen-prepared-budget-decision] source=%llu charge=%llu tickets=%llu..%llu predicted=%u predictor=max-latest-two-complete real_samples=%u newest_real=%llu real_ms=%.6f incremental_ms=%.6f total_ms=%.6f ceiling_ms=%.3f status=%u completion_accept=%d cooldown_until=%llu (prediction, NOT current-interval actual-at-publication; SF outcome UNKNOWN)",
                        (unsigned long long)g->stamp.frame,(unsigned long long)g->charge_frame,
                        (unsigned long long)g->first_ticket,(unsigned long long)g->ticket,
                        d.probe,d.real_samples,(unsigned long long)d.newest_real_frame,d.real_ms,
                        d.incremental_ms,d.total_ms,FG_BUDGET_TARGET_MS,d.status,completion_accept,
                        (unsigned long long)F.prepared_budget.cooldown_until);
                }
                if (d.status != FG_BUDGET_MISSING) {
                    F.budget_last=d;
                    vk_overlay_set_gpu(d.real_ms,d.incremental_ms,FG_BUDGET_TICK_MS,1);
                    F.budget_real_sum+=d.real_ms; F.budget_incremental_sum+=d.incremental_ms;
                    F.budget_total_sum+=d.total_ms;
                    if (!F.budget_samples || d.margin_ms < F.budget_margin_min) F.budget_margin_min=d.margin_ms;
                    F.budget_samples++;
                }
                if (!budget_ok) F.budget_post_skips[d.status == FG_BUDGET_OVERLOAD]++;
            }
            accepted=authority_ok && budget_ok;
            if (!budget_ok) F.budget_skips++;
            if (accepted) {
                F.generated_count++;
                F.depth_range_generated+=s->depth_range_changed;
            }
            if (accepted && s->depth_range_changed && F.diag)
                LOGV("[framegen-depth-range] real=%llu near=%.9g far=%.9g generated=1",
                    (unsigned long long)F.output_budget_frame[slot],s->camera[56],s->camera[57]);
        }
        pthread_mutex_unlock(&fg_stats_mu);
    }
    if (!measured && vk_fg_interpolating()) {
        pthread_mutex_lock(&fg_stats_mu);
        F.budget_post_skips[0]++; F.budget_skips++;
        pthread_mutex_unlock(&fg_stats_mu);
    }
    if (vk_fg_lsfg_requested() && FQ.generated[o->source]==2) vk_fg_lsfg_result(accepted);
    if (vk_fg_qcom_requested() && FQ.generated[o->source]==1)
        vk_fg_qcom_complete(vk_fg_adaptive() ? slot : o->source);
    atomic_fetch_sub_explicit(&s->referenced,1,memory_order_release);
    if (o->previous < FG_SOURCES)
        atomic_fetch_sub_explicit(&F.source[o->previous].referenced,1,memory_order_release);
    if (state==VK_FG_GENERATION_SUBMITTED &&
        atomic_load_explicit(&g->canceled,memory_order_acquire)) accepted=0;
    atomic_store_explicit(&o->busy,0,memory_order_release);
    if (state==VK_FG_GENERATION_SUBMITTED)
        atomic_store_explicit(&g->state,VK_FG_GENERATION_EMPTY,memory_order_release);
    return accepted;
}
#ifdef TOTK_VK_TESTS
int vk_fg_test_generation_bridge(int enabled) {
    for (unsigned i=0;i<FG_OUTPUTS;i++) if (vk_fg_generation_state(&fg_generation[i])) return 0;
    if (!enabled) for (unsigned i=0;i<FG_OUTPUTS;i++) {
        vk_fg_generation *g=&fg_generation[i];
        if (g->test_bridge) vkDestroySemaphore(V.dev,g->test_bridge,NULL);
        g->test_bridge=VK_NULL_HANDLE;
    }
    fg_generation_test_bridge=!!enabled;
    return 1;
}
int vk_fg_test_generation_bridge_active(void) { return fg_generation_test_bridge; }
int vk_fg_test_terminal_owned(int enabled) {
    if (!fg_generation_test_bridge) return 0;
    for (unsigned i=0;i<FG_OUTPUTS;i++) if (vk_fg_generation_state(&fg_generation[i])) return 0;
    atomic_store_explicit(&vk_fg_terminal_owned_active,!!enabled,memory_order_relaxed);
    return 1;
}
int vk_fg_test_adaptive_lean(int lean,int direct) {
    for (unsigned i=0;i<FG_OUTPUTS;i++) if (vk_fg_generation_state(&fg_generation[i])) return 0;
    atomic_store_explicit(&fg_adaptive_lean_config,!!lean,memory_order_relaxed);
    atomic_store_explicit(&fg_adaptive_direct_config,!!direct,memory_order_relaxed);
    F.lean_latched=F.lean_unknown=0;
    return 1;
}
void vk_fg_test_lean_counts(uint64_t counts[VK_FG_LEAN_COUNTS]) {
    memcpy(counts,F.lean_counts,sizeof F.lean_counts);
}
void vk_fg_test_generation_refuse(int refused) { fg_generation_test_refused=!!refused; }
int vk_fg_test_terminal_current(uint64_t facts[6],vk_fg_terminal_input *refusal) {
    if (!fg_generation_test_bridge || F.current<0) return 0;
    const fg_terminal_owned *o=&F.source[F.current].owned;
    if (!o->begun || o->frame!=F.budget_frame+1) return 0;
    if (facts) {
        const uint64_t values[6]={(uint64_t)F.current,o->ready,o->failed,o->used,o->writes,o->copies};
        memcpy(facts,values,sizeof values);
    }
    if (refusal) *refusal=o->refusal;
    return 1;
}
int vk_fg_test_terminal_facts(const vk_fg_generation *g,uint64_t facts[6],
        VkDescriptorBufferInfo *buffer,VkDescriptorSet sets[2],vk_record_draw *draw) {
    if (!fg_generation_test_bridge || !g || !vk_fg_generation_state(g) || !g->immutable_terminal) return 0;
    const fg_source *s=&F.source[g->source]; const fg_terminal_owned *o=&s->owned;
    if (facts) {
        const uint64_t values[6]={g->source,g->previous,o->used,o->writes,o->copies,(uint64_t)g->immutable_terminal};
        memcpy(facts,values,sizeof values);
    }
    if (buffer) *buffer=(VkDescriptorBufferInfo){o->bytes.buffer,0,o->used};
    if (sets) memcpy(sets,o->sets,sizeof o->sets);
    if (draw) *draw=s->terminal;
    return 1;
}
int vk_fg_test_terminal_copy(const vk_fg_generation *g,void *destination,size_t capacity) {
    if (!fg_generation_test_bridge || !g || !vk_fg_generation_state(g) || !g->immutable_terminal) return 0;
    const fg_terminal_owned *o=&F.source[g->source].owned;
    if (!destination || capacity<o->used || !o->bytes.mapped) return 0;
    memcpy(destination,o->bytes.mapped,(size_t)o->used); return 1;
}
int vk_fg_test_generation_quarantine(vk_fg_generation *g,int enabled) {
    if (!fg_generation_test_bridge || !g || !g->lease || g->ticket ||
        vk_fg_generation_state(g)!=(enabled?VK_FG_GENERATION_PREPARED:VK_FG_GENERATION_QUARANTINED))
        return 0;
    /* No GL/GPU dispatch took place: inject ownership state, then restore
     * ONLY that owned test graph for cleanup. Production has no such escape. */
    atomic_store_explicit(&g->state,enabled?VK_FG_GENERATION_QUARANTINED:VK_FG_GENERATION_PREPARED,
        memory_order_release);
    return 1;
}
void vk_fg_test_generation_facts(const vk_fg_generation *g,uint64_t facts[6],float *phase) {
    facts[0]=g ? g->lease : 0;
    facts[1]=g ? g->ticket : 0;
    facts[2]=g ? (uint64_t)(uintptr_t)g->pool : 0;
    facts[3]=g ? (uint64_t)(uintptr_t)g->cb : 0;
    facts[4]=g ? g->first_ticket : 0;
    facts[5]=(uint64_t)vk_fg_generation_state(g);
    if (phase) *phase=g ? g->test_phase : 0;
}
int vk_fg_test_budget_ticket(uint64_t ticket,uint64_t facts[4],double *ms) {
    pthread_mutex_lock(&fg_stats_mu);
    const fg_budget_slot *s=&F.budget.slots[ticket%FG_BUDGET_SLOTS];
    int exact=ticket && s->ticket==ticket;
    if (facts) {
        facts[0]=s->ticket; facts[1]=s->measured;
        facts[2]=s->synthetic; facts[3]=F.budget.last_sample;
    }
    if (ms) *ms=s->ms;
    pthread_mutex_unlock(&fg_stats_mu);
    return exact;
}
int vk_fg_test_budget_frame(uint64_t frame,uint64_t facts[6],double costs[2]) {
    pthread_mutex_lock(&fg_stats_mu);
    const fg_budget_frame *f=&F.budget.frames[frame%FG_BUDGET_FRAMES];
    int exact=frame && f->frame==frame;
    if (facts) {
        facts[0]=f->frame; facts[1]=f->first; facts[2]=f->last;
        facts[3]=f->status; facts[4]=f->synthetic; facts[5]=F.budget.last_frame;
    }
    if (costs) { costs[0]=f->real_ms; costs[1]=f->incremental_ms; }
    pthread_mutex_unlock(&fg_stats_mu);
    return exact;
}
#endif
void vk_fg_frame_end(uint64_t fence) {
    if (atomic_load_explicit(&fg_config,memory_order_relaxed) < 1) return;
    vk_fg_qcom_harvest();
    vk_fg_lsfg_harvest();
    fg_authority_harvest();
    fg_profile_harvest();
    fg_writer_harvest();
    if (vk_fg_writer_diag()) vk_fg_writer_poison("frame ended with pending span");
    if (F.ready && F.authority >= 0 && F.source[F.authority].span_open)
        (void)fg_authority_span_fail("frame ended before the unproven after-copy");
    if (vk_fg_interpolating()) {
        pthread_mutex_lock(&fg_stats_mu);
        if (FQ.budget_enabled) {
            uint64_t frame=++F.budget_frame;
            if (vk_fg_adaptive()) fg_budget_external_group_seal(&F.budget,&FQ.budget,
                &FQ.adaptive_cost[frame%FG_BUDGET_FRAMES],frame);
            fg_budget_external_seal(&F.budget,&FQ.budget,frame,fence);
        }
        else fg_budget_seal(&F.budget,++F.budget_frame,fence);
        fg_prepared_budget_audit();
        pthread_mutex_unlock(&fg_stats_mu);
    }
    if (F.ready && F.current >= 0) {
        fg_source *s=&F.source[F.current];
        /* A missing covering ticket must never make the entry look idle. */
        s->retire=fence ? fence : UINT64_MAX;
        if (s->pair_source >= 0) F.source[s->pair_source].retire=s->retire;
        if (!fence) fg_suspend("missing source retirement ticket");
        if (!F.invalid && vk_fg_enabled() && (!vk_fg_interpolating() || s->ui_ready == 3)) {
            memcpy(F.previous,s->camera,sizeof F.previous);
            F.previous_w=s->depth.w; F.previous_h=s->depth.h; F.previous_valid=1;
            F.previous_scene_w=s->scene.w; F.previous_scene_h=s->scene.h;
            if (vk_fg_interpolating()) F.history=F.current;
        } else F.previous_valid=0;
    } else F.previous_valid=0;
    /* A snapshot whose capture/replay was refused still belongs to this real
       ticket. Never let failed capture make its private allocation look idle. */
    if (F.ready && F.authority >= 0 && F.authority != F.current) {
        F.source[F.authority].retire=fence ? fence : UINT64_MAX;
        if (!fence) fg_suspend("missing authority snapshot retirement ticket");
    }
    if (!F.previous_valid && vk_fg_interpolating()) F.history=-1;
    pthread_mutex_lock(&fg_stats_mu);
    if (F.generated && !vk_fg_interpolating()) F.generated_count++;
    pthread_mutex_unlock(&fg_stats_mu);
    if (vk_fg_qcom_requested()) vk_fg_qcom_tick(F.ready && F.current>=0 && !F.invalid);
    if (vk_fg_lsfg_requested()) vk_fg_lsfg_tick(F.ready && F.current>=0 && !F.invalid);
    if (!vk_fg_qcom_requested() && !vk_fg_lsfg_requested())
        vk_overlay_set_framegen(VK_OVERLAY_FG_CAMERA,
            vk_fg_interpolating() ? VK_OVERLAY_FG_INTERP : VK_OVERLAY_FG_EXTRAP,
            atomic_load_explicit(&fg_suspended,memory_order_relaxed) ? VK_OVERLAY_FG_REFUSED :
                F.previous_valid ? VK_OVERLAY_FG_ACTIVE : VK_OVERLAY_FG_WARMUP,0,0);
    fg_drs_tick();
    F.current=F.authority=-1; F.authority_attempted=0;
    F.have_camera=0; F.invalid=0; F.generated=0;
    /* One skipped root replaced its init + final census/readback this frame. */
    F.lean_counts[VK_FG_LEAN_ROOTS]+=F.lean_unknown;
    F.lean_latched=F.lean_unknown=0;
}
void vk_fg_track_result(unsigned result) {
    if (atomic_load_explicit(&fg_config,memory_order_relaxed) < 1 || result >= VK_FG_TRACK_RESULTS) return;
    if (result == VK_FG_ELIGIBLE) F.eligible++;
    else F.refused[result]++;
}
#ifdef TOTK_VK_TESTS
void vk_fg_test_pass_counts(uint64_t counts[4]) { memcpy(counts,F.passes,sizeof F.passes); }
void vk_fg_test_source_lifecycle(uint64_t facts[3]) {
    facts[0]=F.ready && F.history>=0 ? (uint64_t)F.history+1 : 0;
    facts[1]=F.budget_frame;
    facts[2]=F.ready && F.current>=0 ? (uint64_t)F.source[F.current].ui_ready :
        F.ready && F.history>=0 ? (uint64_t)F.source[F.history].ui_ready : 0;
}
void vk_fg_test_job_ownership(uint64_t counts[2]) {
    counts[0]=counts[1]=0;
    for (unsigned i=0;i<FG_SOURCES;i++)
        counts[0]+=(unsigned)atomic_load_explicit(&F.source[i].referenced,memory_order_acquire);
    for (unsigned i=0;i<FG_OUTPUTS;i++)
        counts[1]+=(unsigned)atomic_load_explicit(&F.output[i].busy,memory_order_acquire);
}
int vk_fg_test_scene_cost(unsigned slot, double milliseconds[4]) {
    if (!milliseconds || !F.profile_queries || slot>=FG_OUTPUTS ||
        !atomic_load_explicit(&F.output[slot].busy,memory_order_acquire) ||
        !atomic_load_explicit(&F.output[slot].ended,memory_order_acquire)) return 0;
    unsigned index=F.output[slot].source;
    milliseconds[3]=0;
    return fg_profile_source_read(index,FG_PROFILE_REPLAY,&milliseconds[0]) &&
        fg_profile_source_read(index,FG_PROFILE_INTERP_UI,&milliseconds[1]) &&
        fg_profile_read(F.profile_queries,fg_profile_output_query(slot,FG_PROFILE_OUTPUT_RENDER),&milliseconds[2]) &&
        (!F.source[index].native_scene_ready ||
            fg_profile_source_read(index,FG_PROFILE_NATIVE_SCENE_COPY,&milliseconds[3]));
}
void vk_fg_test_writer_fail(unsigned enabled, unsigned span_limit) {
    FW.test_failure=enabled; FW.test_span_limit=span_limit;
}
int vk_fg_test_writer_result(unsigned slot, unsigned status[8], vk_fg_writer_test_group groups[64]) {
    memset(status,0,8*sizeof *status); memset(groups,0,64*sizeof *groups);
    if (!vk_fg_writer_diag() || slot>=FG_OUTPUTS ||
        !atomic_load_explicit(&F.output[slot].busy,memory_order_acquire)) return 0;
    unsigned index=F.output[slot].source;
    fg_writer_source *w=&FW.source[index];
    status[0]=w->groups; status[1]=w->spans; status[2]=w->closed;
    status[3]=w->final; status[4]=w->failed; status[5]=w->overflow;
    uint32_t result[FG_WRITER_WORDS];
    if (!fg_writer_read(index,result)) return 0;
    status[6]=result[2]; status[7]=result[3];
    for (unsigned i=0;i<w->groups;i++) {
        vk_fg_writer_test_group *g=&groups[i];
        strcpy(g->vs,w->group[i].vs); strcpy(g->fs,w->group[i].fs); strcpy(g->check,w->group[i].check);
        g->classification=w->group[i].classification; g->residual=result[4+FG_WRITER_SPANS+i];
        for (unsigned span=0;span<w->spans;span++) if (w->span_group[span]==i+1) {
            g->gross+=result[4+span]; g->draws+=w->span_draws[span]; g->spans++;
        }
    }
    return 1;
}
int vk_fg_test_authority_result(unsigned slot, uint32_t counts[4]) {
    uint32_t result[2][FG_AUTHORITY_WORDS];
    if (slot >= FG_OUTPUTS || !atomic_load_explicit(&F.output[slot].busy,memory_order_acquire) ||
        !fg_pair_read(&F.source[F.output[slot].source],result)) return 0;
    memcpy(counts,result[1],4*sizeof *counts);
    return 1;
}
int vk_fg_test_authority_pair(unsigned slot, uint32_t counts[12]) {
    uint32_t result[2][FG_AUTHORITY_WORDS];
    if (slot >= FG_OUTPUTS || !atomic_load_explicit(&F.output[slot].busy,memory_order_acquire) ||
        !fg_pair_read(&F.source[F.output[slot].source],result)) return 0;
    for (unsigned side=0;side<2;side++) {
        memcpy(counts+side*6,result[side],4*sizeof *counts);
        counts[side*6+4]=result[side][10]; counts[side*6+5]=result[side][11];
    }
    return 1;
}
void vk_fg_test_authority_counts(uint64_t counts[5]) {
    pthread_mutex_lock(&fg_stats_mu);
    counts[0]=F.reservations; counts[1]=F.authority_copies; counts[2]=F.comparisons;
    counts[3]=F.authority_skips; counts[4]=F.recorded;
    pthread_mutex_unlock(&fg_stats_mu);
}
void vk_fg_test_authority_span_counts(uint64_t counts[5]) {
    pthread_mutex_lock(&fg_stats_mu);
    counts[0]=F.span_before_copies; counts[1]=F.span_after_copies; counts[2]=F.span_count;
    counts[3]=F.span_copied_texels; counts[4]=F.span_failed;
    pthread_mutex_unlock(&fg_stats_mu);
}
/* The test owns the covering fence, just as with the existing pair getters. */
int vk_fg_test_uncertified_result(unsigned slot, uint32_t counts[2]) {
    counts[0]=counts[1]=0;
    if (slot >= FG_OUTPUTS || !atomic_load_explicit(&F.output[slot].busy,memory_order_acquire)) return 0;
    fg_output *o=&F.output[slot];
    unsigned indices[2]={o->previous,o->source};
    for (unsigned side=0;side<2;side++) if (indices[side]<FG_SOURCES) {
        uint32_t result[8];
        if (!fg_authority_read(&F.source[indices[side]],result)) return 0;
        counts[side]=result[7];
    }
    return 1;
}
int vk_fg_test_uncertified_source(uint32_t *count) {
    uint32_t result[8];
    if (!count || F.current < 0 || !fg_authority_read(&F.source[F.current],result)) return 0;
    *count=result[7];
    return 1;
}
int vk_fg_test_profile(unsigned slot, uint32_t counts[2], double limits[2]) {
    if (!counts || !limits) return 0;
    counts[0]=counts[1]=0;
    limits[0]=limits[1]=0;
    if (!F.profile_queries || slot>=FG_OUTPUTS) return 0;
    fg_output *o=&F.output[slot];
    if (!atomic_load_explicit(&o->busy,memory_order_acquire) ||
        !atomic_load_explicit(&o->ended,memory_order_acquire)) return 0;
    fg_source *s=&F.source[o->source];
    for (unsigned stage=0;stage<FG_PROFILE_STAGES;stage++) {
        int source=stage<FG_PROFILE_SOURCE_STAGES;
        uint32_t bit=UINT32_C(1)<<(source ? stage : stage-FG_PROFILE_SOURCE_STAGES);
        if (!((source ? s->profile_mask : o->profile_mask)&bit)) continue;
        double ms;
        if (!(source ? fg_profile_source_read(o->source,stage,&ms) :
            fg_profile_read(F.profile_queries,fg_profile_output_query(slot,stage),&ms))) return 0;
        if (!(counts[0]+counts[1]) || ms<limits[0]) limits[0]=ms;
        if (ms>limits[1]) limits[1]=ms;
        counts[!source]++;
    }
    return counts[0]>0 && counts[1]>0;
}
#endif
void vk_fg_stats(void) {
    if (atomic_load_explicit(&fg_config,memory_order_relaxed) < 1) return;
    int qcom=vk_fg_qcom_active();
    if (F.real==299) vk_fg_qcom_stats();
    int lsfg=vk_fg_lsfg_active();
    if (F.real==299) vk_fg_lsfg_stats();
    pthread_mutex_lock(&fg_stats_mu);
    if (++F.real == 300) {
        fg_profile_stats();
        fg_writer_stats();
        if (vk_fg_interpolating()) LOGV("framegen %s last 300 real: synthesized=%llu skipped budget=%llu camera-cut=%llu history=%llu late=%llu inputs(boundary/chain/depth/capacity/structure/backoff)=%llu/%llu/%llu/%llu/%llu/%llu latency-added=%s%s",
            lsfg ? "lsfg-present" : qcom ? "qcom-present" : "interp",
            (unsigned long long)F.generated_count,(unsigned long long)F.budget_skips,
            (unsigned long long)F.cuts,(unsigned long long)F.history_skips,(unsigned long long)F.late_skips,
            (unsigned long long)F.refused[VK_FG_NO_BOUNDARY],(unsigned long long)F.refused[VK_FG_CHAIN_BROKEN],
            (unsigned long long)F.refused[VK_FG_DEPTH_LOST],(unsigned long long)F.refused[VK_FG_TRACK_CAPACITY],
            (unsigned long long)F.refused[VK_FG_STRUCTURE_CHANGED],(unsigned long long)F.refused[VK_FG_BACKOFF],
            qcom ? "no-real-tick" : "one-real-tick",
            qcom ? (vk_fg_lead_refreshes()==1 ? "+one-physical-refresh (about16.7 ms at60Hz)" :
                "+two-physical-refreshes (about33.3 ms at60Hz)") :
            vk_fg_locked() ? (vk_fg_lead_refreshes()==1 ?
                "+one-physical-refresh (about50 ms total at60Hz)" :
                "+two-physical-refreshes (about66.7 ms total at60Hz)") : " (33.333 ms)");
        if (vk_fg_interpolating()) LOGV("framegen depth-range last 300 real: newly-accepted-pairs=%llu generated=%llu (independent source near/far, bounded smooth FOV, unchanged pose/aspect/extent/off-center cut guards)",
            (unsigned long long)F.depth_range_pairs,(unsigned long long)F.depth_range_generated);
        F.depth_range_pairs=F.depth_range_generated=0;
        LOGV("framegen budget last 300 real: mode=%s ceiling=%.3f ms pre-skips missing/stale=%llu overload=%llu completion-skips missing=%llu overload=%llu",
            F.budget_override ? "fixed-override-incremental" : vk_fg_interpolating() ? "adaptive-total" : "legacy-incremental",
            F.budget_ms,(unsigned long long)F.budget_pre_skips[0],(unsigned long long)F.budget_pre_skips[1],
            (unsigned long long)F.budget_post_skips[0],(unsigned long long)F.budget_post_skips[1]);
        if (vk_fg_adaptive()) LOGV("[framegen-prepared-budget] mode=predictive-real-max-latest-two-complete measured-incremental ceiling_ms=%.3f actual_audits=%llu actual_overshoots=%llu actual_missing=%llu completion_decisions=%llu completion_accepts=%llu backoff_intervals=%u cooldown_until=%llu (lifetime per arm; ordinary immutable actual aggregate includes ALL charged children; not a strict current-actual publication guarantee)",
            FG_BUDGET_TARGET_MS,(unsigned long long)F.prepared_budget.audits,
            (unsigned long long)F.prepared_budget.overshoots,(unsigned long long)F.prepared_budget.missing,
            (unsigned long long)F.prepared_budget.decisions,(unsigned long long)F.prepared_budget.publications,
            FG_QCOM_PREPARED_BACKOFF,(unsigned long long)F.prepared_budget.cooldown_until);
        if (vk_fg_adaptive() && fg_adaptive_budget_selected())
            LOGV("[framegen-adaptive-budget] mode=stale-incremental-estimate ceiling_ms=%.3f window=%u fresh_age=%u stale_admits=%llu stale_refusals=%llu stale_reservations=%llu stale_audits=%llu stale_overshoots=%llu stale_missing=%llu probes=%llu backoff_intervals=%u (lifetime per arm; same ceiling, immutable audits and back-off; probes only without a measured incremental in window)",
                FG_BUDGET_TARGET_MS,(unsigned)FG_BUDGET_WINDOW,(unsigned)FG_BUDGET_MAX_AGE,
                (unsigned long long)F.adaptive_budget_admits,(unsigned long long)F.adaptive_budget_refusals,
                (unsigned long long)F.adaptive_budget_reservations,(unsigned long long)F.adaptive_budget_audits,
                (unsigned long long)F.adaptive_budget_overshoots,(unsigned long long)F.adaptive_budget_missing,
                (unsigned long long)F.budget.probes,FG_QCOM_PREPARED_BACKOFF);
        if (F.budget_samples) LOGV("framegen headroom decision mean real=%.3f ms incremental=%.3f ms total=%.3f ms min-tick-margin=%.3f ms samples=%llu last frame=%llu tickets=%llu..%llu real/incremental/total/margin=%.3f/%.3f/%.3f/%.3f ms (adaptive REAL may be predicted; measured incremental; immutable actual aggregates/overshoots reported separately)",
            F.budget_real_sum/F.budget_samples,F.budget_incremental_sum/F.budget_samples,F.budget_total_sum/F.budget_samples,
            F.budget_margin_min,(unsigned long long)F.budget_samples,(unsigned long long)F.budget_last.frame,
            (unsigned long long)F.budget_last.first,(unsigned long long)F.budget_last.last,
            F.budget_last.real_ms,F.budget_last.incremental_ms,F.budget_last.total_ms,F.budget_last.margin_ms);
        else if (vk_fg_interpolating() && !F.budget_override)
            LOGV("framegen headroom decision real/incremental/total/margin unavailable (no complete measured incremental decision)");
        if (vk_fg_interpolating() && !F.budget_override && F.budget_pre_frame && F.budget_pre.status != FG_BUDGET_MISSING)
            LOGV("framegen headroom latest pre-admission p90 real/incremental/total/tick-margin=%.3f/%.3f/%.3f/%.3f ms real-p90-frame=%llu tickets=%llu..%llu real/incremental-samples=%u/%u incremental-frame=%llu tickets=%llu..%llu probe=%u reason=%u retry-frame=%llu (nan=unmeasured probe cost, not zero)",
                F.budget_pre.real_ms,F.budget_pre.incremental_ms,F.budget_pre.total_ms,F.budget_pre.margin_ms,
                (unsigned long long)F.budget_pre.frame,(unsigned long long)F.budget_pre.first,(unsigned long long)F.budget_pre.last,
                F.budget_pre.real_samples,F.budget_pre.incremental_samples,
                (unsigned long long)F.budget_pre.incremental_frame,
                (unsigned long long)F.budget_pre.incremental_first,(unsigned long long)F.budget_pre.incremental_last,
                F.budget_pre.probe,F.budget_pre.reason,(unsigned long long)F.budget_pre.retry_frame);
        if (vk_fg_interpolating() && !F.budget_override)
            LOGV("framegen headroom bootstrap/feedback: lifetime reservations=%llu probes=%llu last-probe=%llu real-fence samples=%llu missed-target=%llu correlated-pauses=%llu cooldown-until=%llu backoff=%u (framework present fences >=one refresh after real target; NOT hardware scanout)",
                (unsigned long long)F.budget.reservations,(unsigned long long)F.budget.probes,
                (unsigned long long)F.budget.last_probe,(unsigned long long)F.budget_feedback_samples,
                (unsigned long long)F.budget_feedback_late,(unsigned long long)F.budget_feedback_pauses,
                (unsigned long long)F.budget.cooldown_until,F.budget.feedback_backoff);
        F.budget_feedback_samples=F.budget_feedback_late=F.budget_feedback_pauses=0;
        memset(F.budget_pre_skips,0,sizeof F.budget_pre_skips); memset(F.budget_post_skips,0,sizeof F.budget_post_skips);
        F.budget_samples=0; F.budget_real_sum=F.budget_incremental_sum=F.budget_total_sum=F.budget_margin_min=0;
        F.budget_skips=F.cuts=F.history_skips=F.late_skips=0;
        if (F.diag) LOGV("[framegen-diag] last 300 real: eligible=%llu generated=%llu eligible-not-generated=%llu refused boundary=%llu chain=%llu depth=%llu capacity=%llu structure=%llu suspended=%llu backoff=%llu",
            (unsigned long long)F.eligible,(unsigned long long)F.generated_count,
            (unsigned long long)(F.eligible >= F.generated_count ? F.eligible-F.generated_count : 0),
            (unsigned long long)F.refused[VK_FG_NO_BOUNDARY],(unsigned long long)F.refused[VK_FG_CHAIN_BROKEN],
            (unsigned long long)F.refused[VK_FG_DEPTH_LOST],(unsigned long long)F.refused[VK_FG_TRACK_CAPACITY],
            (unsigned long long)F.refused[VK_FG_STRUCTURE_CHANGED],(unsigned long long)F.refused[VK_FG_SUSPENDED],
            (unsigned long long)F.refused[VK_FG_BACKOFF]);
        if (F.diag) LOGV("[framegen-generate-admission] state=%llu prediction=%llu capture=%llu output-busy=%llu extent=%llu allocation=%llu recorded=%llu",
            (unsigned long long)F.admission[0],(unsigned long long)F.admission[1],
            (unsigned long long)F.admission[2],(unsigned long long)F.admission[3],
            (unsigned long long)F.admission[4],(unsigned long long)F.admission[5],
            (unsigned long long)F.admission[6]);
        memset(F.admission,0,sizeof F.admission);
        if (F.job_trace) for (unsigned i=0;i<FG_AUTH_FAILURES;i++) {
            uint64_t total=F.authority_failures[i];
            LOGV("[framegen-authority-begin-counts] real=%llu reason=%s last300=%llu total=%llu; failed-calls-not-probe-skips",
                (unsigned long long)F.budget_frame,fg_authority_failure_names[i],
                (unsigned long long)(total-F.authority_failures_reported[i]),(unsigned long long)total);
            F.authority_failures_reported[i]=total;
        }
        if (F.job_trace && (atomic_load_explicit(&fg_adaptive_lean_config,memory_order_relaxed)==1 ||
                atomic_load_explicit(&fg_adaptive_direct_config,memory_order_relaxed)==1)) {
            uint64_t d[VK_FG_LEAN_COUNTS];
            for (unsigned i=0;i<VK_FG_LEAN_COUNTS;i++) {
                d[i]=F.lean_counts[i]-F.lean_reported[i]; F.lean_reported[i]=F.lean_counts[i];
            }
            LOGV("[framegen-adaptive-lean-summary] real=%llu last300 lean-sources=%llu skipped scene-copies=%llu depth-copies=%llu real-copies=%llu unknown-writer-draws=%llu authority-roots=%llu direct-out=%llu direct-out-fallback=%llu (unknown-writer draws bound the skipped spans/pass splits from above; each root also skipped its init and final census/readback)",
                (unsigned long long)F.budget_frame,(unsigned long long)d[VK_FG_LEAN_SOURCES],
                (unsigned long long)d[VK_FG_LEAN_SCENE_COPIES],(unsigned long long)d[VK_FG_LEAN_DEPTH_COPIES],
                (unsigned long long)d[VK_FG_LEAN_REAL_COPIES],(unsigned long long)d[VK_FG_LEAN_UNKNOWN_DRAWS],
                (unsigned long long)d[VK_FG_LEAN_ROOTS],(unsigned long long)d[VK_FG_LEAN_DIRECT],
                (unsigned long long)d[VK_FG_LEAN_DIRECT_FALLBACK]);
        }
        if (F.diag) {
            fg_camera_pair_window *c=&F.camera_pairs;
            if (c->samples) LOGV("[framegen-camera] last 300 real: reject translation=%llu rotation=%llu projection=%llu validation=%llu relative-rotation-deg mean=%.4f max=%.4f translation-distance mean=%.5f max=%.5f compared-pairs=%llu pose-samples=%llu (first fired predicate; invalid poses excluded; DIAG only)",
                (unsigned long long)c->translation,(unsigned long long)c->rotation,
                (unsigned long long)c->projection,(unsigned long long)c->validation,
                c->rotation_sum/c->samples,c->rotation_max,
                c->translation_sum/c->samples,c->translation_max,
                (unsigned long long)c->comparisons,(unsigned long long)c->samples);
            else LOGV("[framegen-camera] last 300 real: reject translation=%llu rotation=%llu projection=%llu validation=%llu compared-pairs=%llu pose-samples=0; relative pose mean/max unavailable",
                (unsigned long long)c->translation,(unsigned long long)c->rotation,
                (unsigned long long)c->projection,(unsigned long long)c->validation,
                (unsigned long long)c->comparisons);
        }
        memset(&F.camera_pairs,0,sizeof F.camera_pairs);
        F.projection_diag_logged=0;
        F.eligible=0; memset(F.refused,0,sizeof F.refused);
        LOGV("framegen authority last 300 real: state-reservations=%llu final-depth-copies=%llu compares=%llu recorded=%llu accepted=%llu over-limit=%llu dilated-limit=%.3f%% (ordered clipped unproven VALUE spans; no initial depth snapshot; post-fence/pre-handoff rejection; no recorder wait)",
            (unsigned long long)F.reservations,(unsigned long long)F.authority_copies,
            (unsigned long long)F.comparisons,(unsigned long long)F.recorded,
            (unsigned long long)F.generated_count,(unsigned long long)F.authority_skips,F.unknown_limit);
        if (F.authority_samples) LOGV("framegen authority unknown raw mean=%.3f%% max=%.3f%% dilated mean=%.3f%% max=%.3f%% halo+tile-growth mean=%.3f percentage-points samples=%llu (coarse4x4 conservative mask, native-pixel-weighted partial edges; tile-only overhead not separately measured; extra unknown may reject more pairs at unchanged limit)",
            F.raw_unknown_sum/F.authority_samples*100,F.raw_unknown_max*100,
            F.dilated_unknown_sum/F.authority_samples*100,F.dilated_unknown_max*100,
            (F.dilated_unknown_sum-F.raw_unknown_sum)/F.authority_samples*100,
            (unsigned long long)F.authority_samples);
        else LOGV("framegen authority unknown raw/dilated mean/max unavailable (no completed comparisons)");
        if (F.motion_samples) LOGV("framegen authority pair motion UV bound x mean=%.7f max=%.7f y mean=%.7f max=%.7f radius-texels x mean=%.3f max=%.0f y mean=%.3f max=%.0f samples=%llu (native-pixel radii unchanged; coarse4x4 mask; certified all-final-depth range; hard UV cap .04; pair dilation+compose timed once)",
            F.motion_sum[0]/F.motion_samples,F.motion_max[0],F.motion_sum[1]/F.motion_samples,F.motion_max[1],
            F.radius_sum[0]/F.motion_samples,F.radius_max[0],F.radius_sum[1]/F.motion_samples,F.radius_max[1],
            (unsigned long long)F.motion_samples);
        if (F.authority_cost_samples) LOGV("framegen authority total GPU mean=%.3f ms max=%.3f ms samples=%llu (state initialization + clipped before/after copies/VALUE updates + one final raw copy/prefix/range/readback; pair dilation charged once inside replay)",
            F.authority_sum_ms/F.authority_cost_samples,F.authority_max_ms,(unsigned long long)F.authority_cost_samples);
        else LOGV("framegen authority total GPU mean/max unavailable (no completed comparisons)");
        LOGV("framegen authority spans last 300 real: before-copies=%llu after-copies=%llu completed-spans=%llu copied-native-texels=%llu failed-roots=%llu (ordinary and experimental share bounded state; scissor bounds copies, NOT certified coverage)",
            (unsigned long long)F.span_before_copies,(unsigned long long)F.span_after_copies,
            (unsigned long long)F.span_count,(unsigned long long)F.span_copied_texels,(unsigned long long)F.span_failed);
        if (F.span_cost_samples) LOGV("framegen authority clipped-copy+VALUE-update GPU mean=%.3f ms max=%.3f ms samples=%llu (sum of before/after work only; excludes real draws and proven gaps)",
            F.span_sum_ms/F.span_cost_samples,F.span_max_ms,(unsigned long long)F.span_cost_samples);
        else LOGV("framegen authority clipped-copy+VALUE-update GPU unavailable (no completed span work)");
        if (F.init_only_samples) LOGV("framegen authority refused-capture initialization+span-work GPU mean=%.3f ms max=%.3f ms samples=%llu (no final copy/census)",
            F.init_only_sum_ms/F.init_only_samples,F.init_only_max_ms,(unsigned long long)F.init_only_samples);
        if (vk_fg_uncertified_plane()) {
            LOGV("framegen UNCERTIFIED plane last 300 real: final-unknown-pixels-admitted=%llu source-frames=%llu final-compared=%llu (fenced sources counted once including warmup/refused captures; only changed exact-qualified spans may assume VALUE authority; NOT proof/error bound; ordinary span costs reported separately)",
                (unsigned long long)F.uncertified_pixels,(unsigned long long)F.uncertified_sources,
                (unsigned long long)F.uncertified_compared);
            F.uncertified_pixels=F.uncertified_sources=F.uncertified_compared=0;
        }
        F.reservations=F.authority_copies=F.comparisons=F.authority_skips=F.recorded=0;
        F.span_before_copies=F.span_after_copies=F.span_count=F.span_copied_texels=F.span_failed=0;
        F.span_cost_samples=0; F.span_sum_ms=F.span_max_ms=0;
        F.authority_samples=F.authority_cost_samples=0;
        F.raw_unknown_sum=F.raw_unknown_max=F.dilated_unknown_sum=F.dilated_unknown_max=0;
        F.authority_sum_ms=F.authority_max_ms=0;
        F.init_only_samples=0; F.init_only_sum_ms=F.init_only_max_ms=0;
        memset(F.motion_sum,0,sizeof F.motion_sum); memset(F.motion_max,0,sizeof F.motion_max);
        memset(F.radius_sum,0,sizeof F.radius_sum); memset(F.radius_max,0,sizeof F.radius_max);
        F.motion_samples=0;
        if (F.measured) LOGV("framegen last 300 real: generated=%llu skipped=%llu GPU charged-stage-sum mean=%.3f ms max=%.3f ms samples=%llu%s",
            (unsigned long long)F.generated_count,(unsigned long long)(F.real >= F.generated_count ? F.real-F.generated_count : 0),F.sum_ms/F.measured,F.max_ms,
            (unsigned long long)F.measured,atomic_load_explicit(&fg_suspended,memory_order_relaxed)?" suspended":"");
        else LOGV("framegen last 300 real: generated=%llu skipped=%llu GPU mean/max unavailable (no completed samples)%s",
            (unsigned long long)F.generated_count,(unsigned long long)(F.real >= F.generated_count ? F.real-F.generated_count : 0),
            atomic_load_explicit(&fg_suspended,memory_order_relaxed)?" suspended":"");
        F.real=F.generated_count=F.measured=0; F.sum_ms=F.max_ms=0;
    }
    pthread_mutex_unlock(&fg_stats_mu);
}

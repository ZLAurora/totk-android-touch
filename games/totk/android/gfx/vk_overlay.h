/* Native Android presenter panel; 5x7 font derives from d2r_gfx/gfx/nvn/fps_overlay.c (same MIT lineage). */
#pragma once
#include <stdint.h>
#include "vk_load.h"
#include "vk_present_slots.h"

/* Preserve the pre-compact overlay's baselines 4,15,26,37: pitch11 (33px at scale3). */
enum { VK_OVERLAY_PAD = 2, VK_OVERLAY_ROW_H = 11, VK_OVERLAY_CELL_W = 6, VK_OVERLAY_GLYPH_H = 7,
       VK_OVERLAY_PLAIN_COLS = sizeof("9999x9999->9999x9999 BILIN+BILIN") - 1,
       VK_OVERLAY_COLS = sizeof("GPU 999.9+999.9/999.9MS GEN 9999/9999") - 1,
       /* TOTK_OVERLAY=fps: the frame rate only, one row and no graph. */
       VK_OVERLAY_COMPACT_COLS = sizeof("FPS 999.9") - 1, VK_OVERLAY_COMPACT_FG_COLS = sizeof("FPS 999.9->999.9") - 1,
       VK_OVERLAY_W = VK_OVERLAY_COLS * VK_OVERLAY_CELL_W + 2 * VK_OVERLAY_PAD,
       VK_OVERLAY_GRAPH = VK_OVERLAY_W - 2 * VK_OVERLAY_PAD, VK_OVERLAY_GRAPH_H = 12,
       VK_OVERLAY_MAX_ROWS = 8, VK_OVERLAY_MAX_H = 2 * VK_OVERLAY_PAD + VK_OVERLAY_MAX_ROWS * VK_OVERLAY_ROW_H + VK_OVERLAY_GRAPH_H,
       VK_OVERLAY_MAX_SCALE = 6, VK_OVERLAY_SLOTS = VK_PRESENT_SLOT_CAPACITY };
typedef enum {
    VK_OVERLAY_SCALE_NONE, VK_OVERLAY_SCALE_LINEAR, VK_OVERLAY_SCALE_FSR1, VK_OVERLAY_SCALE_SGSR
} vk_overlay_scale_method;
typedef enum { VK_OVERLAY_STAGE_GAME, VK_OVERLAY_STAGE_HOST } vk_overlay_scale_stage;
typedef enum {
    VK_OVERLAY_FG_OFF, VK_OVERLAY_FG_CAMERA, VK_OVERLAY_FG_QCOM, VK_OVERLAY_FG_LSFG, VK_OVERLAY_FG_ME2
} vk_overlay_fg_engine;
typedef enum { VK_OVERLAY_FG_INTERP, VK_OVERLAY_FG_EXTRAP } vk_overlay_fg_method;
typedef enum {
    VK_OVERLAY_FG_ACTIVE, VK_OVERLAY_FG_WARMUP, VK_OVERLAY_FG_COOLDOWN, VK_OVERLAY_FG_REFUSED
} vk_overlay_fg_state;
typedef struct { uint32_t x, y, w, h, scale; } vk_overlay_rect;
typedef struct {
    unsigned width, height, graph_x, graph_y, graph_w, rows, compile_row, power_row, system_row;
    int framegen, power, handheld, compact, system;
    vk_overlay_rect rect;
    unsigned generation, position;
    uint32_t output_w, output_h;
} vk_overlay_layout;
typedef struct {
    uint64_t layout_rebuilds, text_field_updates, graph_appends;
} vk_overlay_counters;
/* Rounded, bounded display fields. Each row has its own formatting gate; sub-display-unit changes
   never run snprintf or disturb trailing glyphs. All members are 32-bit scalar keys. */
typedef struct {
    struct { int real, shown, ms, handheld, framegen, compact; } rate;
    struct { unsigned rw, rh, ow, oh, game, host, arrow; } resolution;
    struct { unsigned engine, method, state, cooldown, fallback, major, minor, performance; } fg;
    struct { int real, incremental, budget, paired; unsigned shown, expected; } gpu;
    struct { int ms; unsigned count; } compile;
    struct { int power, input, temperature; } power;
    struct { unsigned thermal, headroom, cpu, ratio, cap; } system;
    struct { unsigned available, total, rss; } memory;
} vk_overlay_display_key;
/* Zero means unavailable; thermal/headroom/cpu/ratio encode value + 1. Memory is MiB.
   cap: 0 unknown, 1 no policy cap, 2 scaling_max below hardware max. */
typedef struct {
    unsigned thermal, headroom, cpu, ratio, cap, available, total, rss;
} vk_overlay_system;
typedef struct {
    uint64_t timestamps[256], last_ns, text_ns;
    unsigned next, count;
    double target_ms, fps, mean_ms;
    int power_mw; /* positive means displayable power; negative means unavailable */
    int power_input; /* external/input source, labelled IN instead of battery PWR */
    int temperature_mc; /* battery temperature in millidegrees C; INT_MIN means unavailable */
    char text[VK_OVERLAY_COLS + 1], power_text[VK_OVERLAY_COLS + 1];
    uint64_t real_timestamps[256], real_last_ns, last_generated_ns;
    unsigned real_next, real_count;
    double real_fps, real_mean_ms;
    uint32_t render_w, render_h, canvas_w, canvas_h, output_w, output_h;
    int handheld;
    int compact; /* fps mode: rate row only, no graph; configure re-lays out when it changes */
    int system; /* full mode reserves thermal/CPU and memory rows when opted in */
    vk_overlay_system sys;
    char system_text[VK_OVERLAY_COLS + 1], memory_text[VK_OVERLAY_COLS + 1];
    vk_overlay_scale_method scale_stage[2];
    char resolution_text[VK_OVERLAY_COLS + 1], mode_text[3];
    vk_overlay_fg_engine engine;
    vk_overlay_fg_method method;
    vk_overlay_fg_state state;
    unsigned cooldown;
    int fallback;
    unsigned lsfg_major, lsfg_minor;
    int lsfg_performance;
    double gpu_real_ms, gpu_incremental_ms, gpu_budget_ms;
    int gpu_paired; /* only paired real/incremental/budget samples are compared */
    uint64_t generated_window_shown, generated_window_expected;
    uint64_t compile_at_ns, compile_cost_ns;
    unsigned compile_count; /* synchronous events in the same last-second window as compile_cost_ns */
    char fg_text[VK_OVERLAY_COLS + 1], gpu_text[VK_OVERLAY_COLS + 1], compile_text[VK_OVERLAY_COLS + 1];
    vk_overlay_layout layout;
    /* Physical graph ring: one append per valid handoff interval, never shifted/resampled per frame. */
    double graph_ms[VK_OVERLAY_GRAPH];
    unsigned graph_next, graph_count;
    uint64_t graph_total;
    unsigned text_generation;
    vk_overlay_display_key display_key;
    int display_key_valid;
    vk_overlay_counters counters;
} vk_overlay_panel;
/* Pure CPU API. Configure only when output/config changes; width comes from worst-case templates,
   never current label lengths. Returns 1 on a changed configuration. Power reserves a row even
   when samples are unavailable (full layout only). Position is tl/tr/bl/br; default is tr.
   p->compact selects the one-row fps layout. Zero-initialise panels. */
int vk_overlay_configure(vk_overlay_panel *p, uint32_t output_w, uint32_t output_h,
                         int framegen, int power, const char *position);
void vk_overlay_note(vk_overlay_panel *p, uint64_t now_ns);
/* Every handoff counts as displayed; only non-generated handoffs count as real frames. */
void vk_overlay_note_frame(vk_overlay_panel *p, uint64_t now_ns, int generated);
/* All labels refresh at most once per second. GEN counts synthetic/real handoffs in the trailing
   second, independently of refresh stalls, engine state changes and diagnostic-counter resets.
   A window exceeding the retained timestamp capacity is unknown, not a truncated count.
   COMPILE shows synchronous last-second event count and total cost, with its row always reserved.
   Unknown GPU is omitted; unchanged rounded fields do not reformat. */
void vk_overlay_text(vk_overlay_panel *p, uint64_t now_ns);
/* Full CPU snapshot for golden images, with chronological graph; stride is layout.width * scale.
   Production instead retains per-slot pixels and updates only changed cells/new ring columns. */
void vk_overlay_raster(const vk_overlay_panel *p, uint32_t scale, uint32_t *rgba);
/* Recording-thread diagnostics; NULL selects production history, otherwise a pure CPU panel. */
vk_overlay_counters vk_overlay_get_counters(const vk_overlay_panel *panel);
typedef struct { int mw, input; } vk_overlay_power;
vk_overlay_power vk_overlay_read_power(const char *supply_root);
/* battery/temp is defined in tenths of a degree C. No arbitrary thermal-zone guess. */
int vk_overlay_read_temperature(const char *supply_root);
/* Explicit roots for deterministic host fixtures; production never overrides sensor paths.
   cpu is the sampled g-main host CPU, not the sampler CPU; negative means unavailable. */
vk_overlay_system vk_overlay_read_system(const char *cpu_root, const char *proc_root, int cpu);

/* Recording thread owns staging; each slot may only be reused after its present ring slot is idle.
   The transfer restores the supplied image layout; caller retains queue-family ownership.
   vk_overlay_mode() is the live mode (0 off, 1 fps, 2 full): TOTK_OVERLAY at startup, then
   vk_overlay_cycle(). The next recorded present follows it. */
int vk_overlay_mode(void);
/* Whether this session can show the panel at all. Android app builds always can (the in-game
   Minus+R shortcut may turn it on later), so presenter images carry TRANSFER_DST and telemetry
   publications run even while the mode is off; other builds follow the startup mode. */
int vk_overlay_available(void);
/* Advance off -> fps -> full -> off for this session (not persisted); returns the new mode.
   Any thread; a no-op returning 0 when the overlay is unavailable. */
int vk_overlay_cycle(void);
void vk_overlay_target(double period_s);
void vk_overlay_presented(uint64_t now_ns, int generated);
/* Best-effort relaxed publications; no initialisation, allocation, locking, or clock reads.
   Setters are no-ops while the overlay is unavailable. Zero render extent means unknown; canvas is the
   game's active output crop, not allocation padding. GAME NONE uses the mode's built-in scaler
   below canvas size; HOST NONE defaults to linear. A 1:1 stage is always hidden. */
void vk_overlay_set_resolution(uint32_t render_w, uint32_t render_h,
                               uint32_t canvas_w, uint32_t canvas_h);
void vk_overlay_set_scale_stage(vk_overlay_scale_stage stage, vk_overlay_scale_method method);
void vk_overlay_set_framegen(vk_overlay_fg_engine engine, vk_overlay_fg_method method,
                             vk_overlay_fg_state state, unsigned cooldown, int fallback);
void vk_overlay_set_lsfg_model(unsigned major, unsigned minor, int performance);
/* Negative/NaN GPU values are unknown; unpaired samples never imply a comparable budget. */
void vk_overlay_set_gpu(double real_ms, double incremental_ms, double budget_ms, int paired);
void vk_overlay_note_compile(uint64_t at_ns, uint64_t cost_ns);
int vk_overlay_record(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                      uint32_t w, uint32_t h, unsigned slot, unsigned rotation);
/* w/h and panel placement are logical; rotation is clockwise quarter turns
   into the actual destination image, matching the final AHB composite. */
#ifdef TOTK_VK_TESTS
void vk_overlay_test_scene_reset(int enabled);
void vk_overlay_test_scene_size(uint32_t *w, uint32_t *h);
void vk_overlay_test_publications(uint64_t *resolution, unsigned stages[2]);
/* Recording-owner snapshot for deterministic pixel oracles; does not freeze
   telemetry, alter cached staging, or mock GPU execution. */
void vk_overlay_test_snapshot(vk_overlay_panel *panel);
#endif

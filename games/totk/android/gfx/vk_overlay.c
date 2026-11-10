/* 5x7 glyphs derive from d2r_gfx/gfx/nvn/fps_overlay.c (same MIT project lineage). */
#include "vk_int.h"
#include "vk_overlay.h"
#include "vk_present_policy.h"
#include <stddef.h>
#include "build_kind.h"
extern int runtime_handheld_mode(void);
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <float.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#endif
#define MINV(a, b) ((a) < (b) ? (a) : (b))
#define SECOND_NS UINT64_C(1000000000)

/* Rows use five bits, most significant pixel first. Unspecified glyphs are spaces. */
static const uint8_t font[128][7] = {
    ['0'] = {14,17,19,21,25,17,14}, ['1'] = {4,12,4,4,4,4,14},
    ['2'] = {14,17,1,2,4,8,31}, ['3'] = {30,1,1,14,1,1,30},
    ['4'] = {2,6,10,18,31,2,2}, ['5'] = {31,16,30,1,1,17,14},
    ['6'] = {6,8,16,30,17,17,14}, ['7'] = {31,1,2,4,8,8,8},
    ['8'] = {14,17,17,14,17,17,14}, ['9'] = {14,17,17,15,1,2,12},
    ['F'] = {31,16,16,30,16,16,16}, ['P'] = {30,17,17,30,16,16,16},
    ['S'] = {15,16,16,14,1,1,30}, ['M'] = {17,27,21,21,17,17,17},
    ['I'] = {14,4,4,4,4,4,14}, ['N'] = {17,25,25,21,19,19,17},
    ['R'] = {30,17,17,30,20,18,17}, ['W'] = {17,17,17,21,21,21,10},
    ['A'] = {14,17,17,31,17,17,17}, ['B'] = {30,17,17,30,17,17,30},
    ['C'] = {14,17,16,16,16,17,14}, ['D'] = {30,17,17,17,17,17,30},
    ['E'] = {31,16,16,30,16,16,31}, ['G'] = {14,17,16,23,17,17,15},
    ['H'] = {17,17,17,31,17,17,17}, ['K'] = {17,18,20,24,20,18,17},
    ['L'] = {16,16,16,16,16,16,31}, ['O'] = {14,17,17,17,17,17,14},
    ['Q'] = {14,17,17,17,21,18,13}, ['T'] = {31,4,4,4,4,4,4},
    ['U'] = {17,17,17,17,17,17,14}, ['V'] = {17,17,17,17,17,10,4},
    ['X'] = {17,17,10,4,10,17,17}, ['Y'] = {17,17,10,4,4,4,4},
    ['x'] = {0,0,17,10,4,10,17}, ['+'] = {0,4,4,31,4,4,0},
    ['?'] = {14,17,1,2,4,0,4}, ['/'] = {1,1,2,4,8,16,16},
    ['.'] = {0,0,0,0,0,12,12}, ['-'] = {0,0,0,31,0,0,0}, ['>'] = {16,8,4,2,4,8,16}
};
/* Explicit little-endian RGBA8; Android ARM64 and the Linux host fixtures are little-endian. */
#define RGBA(r,g,b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xff000000u)
static const uint32_t bg = RGBA(12,12,16), fg = RGBA(224,240,224);
static uint64_t clock_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * SECOND_NS + (uint64_t)ts.tv_nsec;
}
static void graph_append(vk_overlay_panel *p, double ms) {
    if (!p->layout.graph_w) return;
    p->graph_ms[p->graph_next] = ms;
    p->graph_next = (p->graph_next + 1) % p->layout.graph_w;
    if (p->graph_count < p->layout.graph_w) p->graph_count++;
    p->graph_total++;
}
void vk_overlay_note(vk_overlay_panel *p, uint64_t now_ns) {
    if (p->count && now_ns <= p->last_ns) return;
    if (p->count && p->layout.graph_w) {
        graph_append(p, (now_ns - p->last_ns) / 1e6); p->counters.graph_appends++;
    }
    p->timestamps[p->next] = now_ns; p->next = (p->next + 1) % 256;
    if (p->count < 256) p->count++;
    p->last_ns = now_ns;
}
void vk_overlay_note_frame(vk_overlay_panel *p, uint64_t now_ns, int generated) {
    if (p->count && now_ns <= p->last_ns) return;
    vk_overlay_note(p, now_ns);
    if (generated) { p->last_generated_ns = now_ns; return; }
    if (p->real_count && now_ns <= p->real_last_ns) return;
    p->real_timestamps[p->real_next] = now_ns; p->real_next = (p->real_next + 1) % 256;
    if (p->real_count < 256) p->real_count++;
    p->real_last_ns = now_ns;
}
/* Rate and handoff count in (now - 1 second, now]. A refresh can be delayed by loading;
   its duration is not the window. UINT_MAX means the retained ring cannot cover the window. */
static unsigned window_rate(const uint64_t *ts, unsigned next, unsigned count, uint64_t now_ns,
                            double *fps, double *mean_ms) {
    uint64_t sum = 0; unsigned n = 0, samples = 0;
    for (unsigned k = 0; k < count; k++) {
        uint64_t end = ts[(next + 255 - k) % 256];
        if (end > now_ns) continue;
        if (now_ns - end >= SECOND_NS) break;
        samples++;
        if (k + 1 == count) continue;
        uint64_t start = ts[(next + 254 - k) % 256];
        if (end <= start) continue;
        sum += end - start; n++;
    }
    *fps = sum ? n * 1e9 / (double)sum : 0;
    *mean_ms = n ? sum / (n * 1e6) : 0;
    return samples == 256 ? UINT_MAX : samples;
}
static const char *scale_name(vk_overlay_scale_method method) {
    switch (method) {
        case VK_OVERLAY_SCALE_LINEAR: return "BILIN";
        case VK_OVERLAY_SCALE_FSR1: return "FSR1";
        case VK_OVERLAY_SCALE_SGSR: return "SGSR";
        default: return "";
    }
}
/* Fixed field limits are deliberate: overflow is a visible unknown, never a plausible clamped value. */
static int decimal_key(double value) {
    if (!isfinite(value) || value < 0) return -1;
    if (value >= 999.95) return -2;
    unsigned tenth = (unsigned)(value * 10 + .5);
    return tenth > 9999 ? -2 : (int)tenth;
}
static void decimal(char out[8], int tenth) {
    if (tenth < 0) { memcpy(out, "???.?", 6); return; }
    snprintf(out, 8, "%3u.%u", (unsigned)tenth / 10, (unsigned)tenth % 10);
}
static unsigned count_key(uint64_t value) { return value > 9999 ? 10000 : (unsigned)value; }
static void count4(char out[8], unsigned value) {
    if (value > 9999) memcpy(out, "????", 5);
    else snprintf(out, 8, "%4u", value);
}
static unsigned memory_key(unsigned mib) { return mib > 999999 ? 1000000 : mib; }
static void memory6(char out[8], unsigned mib) {
    if (mib > 999999) memcpy(out, "??????", 7);
    else snprintf(out, 8, "%6u", mib);
}
static void extent(char out[10], uint32_t w, uint32_t h) {
    if (!w || !h) { memcpy(out, "        ?", 10); return; }
    char a[5], b[5], raw[10];
    if (w > 9999) memcpy(a, "????", 5); else snprintf(a, sizeof a, "%u", w);
    if (h > 9999) memcpy(b, "????", 5); else snprintf(b, sizeof b, "%u", h);
    snprintf(raw, sizeof raw, "%sx%s", a, b);
    snprintf(out, 10, "%9s", raw);
}
static void label(vk_overlay_panel *p, char *dst, const char *src) {
    if (!strcmp(dst, src)) return;
    size_t n = strlen(src);
    /* Every producer below is bounded by the configuration's worst-case template. */
    if (n > VK_OVERLAY_COLS) { src = "?"; n = 1; }
    memcpy(dst, src, n + 1); p->text_generation++; p->counters.text_field_updates++;
}
void vk_overlay_text(vk_overlay_panel *p, uint64_t now_ns) {
    if (p->text[0] && (now_ns < p->text_ns || now_ns - p->text_ns < SECOND_NS)) return;
    p->text_ns = now_ns;
    unsigned presented = window_rate(p->timestamps, p->next, p->count, now_ns, &p->fps, &p->mean_ms);
    unsigned real_frames = window_rate(p->real_timestamps, p->real_next, p->real_count, now_ns,
                                      &p->real_fps, &p->real_mean_ms);
    /* Use the same actual handoff window as FPS, not diagnostic counters which reset every300 frames. */
    if (presented == UINT_MAX || real_frames == UINT_MAX || real_frames > presented)
        p->generated_window_shown = p->generated_window_expected = UINT64_MAX;
    else {
        p->generated_window_shown = presented - real_frames;
        p->generated_window_expected = real_frames;
    }

    /* Build only integer display keys first. Each row below formats solely when its rounded,
       bounded fields change; a stable overlay performs no snprintf, including at the 1 Hz tick. */
    vk_overlay_display_key key = {0};
    int generating = p->layout.framegen;
    double real = generating ? p->real_fps : p->fps, ms = generating ? p->real_mean_ms : p->mean_ms;
    key.rate.real = real > 0 ? decimal_key(real) : -1;
    key.rate.shown = generating ? (p->fps > 0 ? decimal_key(p->fps) : -1) : 0;
    key.rate.ms = real > 0 ? decimal_key(ms) : -1;
    key.rate.handheld = p->handheld != 0; key.rate.framegen = generating; key.rate.compact = p->layout.compact;
    int render_known = p->render_w && p->render_h, output_known = p->output_w && p->output_h;
    key.resolution.rw = render_known ? count_key(p->render_w) : 0;
    key.resolution.rh = render_known ? count_key(p->render_h) : 0;
    key.resolution.ow = output_known ? count_key(p->output_w) : 0;
    key.resolution.oh = output_known ? count_key(p->output_h) : 0;
    key.resolution.arrow = render_known ? p->render_w != p->output_w || p->render_h != p->output_h : output_known;
    if (render_known) {
        if (p->canvas_w && p->canvas_h && (p->render_w != p->canvas_w || p->render_h != p->canvas_h))
            key.resolution.game = p->scale_stage[0] ? p->scale_stage[0] :
                p->render_w <= p->canvas_w && p->render_h <= p->canvas_h ?
                (p->handheld ? VK_OVERLAY_SCALE_LINEAR : VK_OVERLAY_SCALE_FSR1) : VK_OVERLAY_SCALE_NONE;
        if (p->canvas_w && p->canvas_h && (p->canvas_w != p->output_w || p->canvas_h != p->output_h))
            key.resolution.host = p->scale_stage[1] ? p->scale_stage[1] : VK_OVERLAY_SCALE_LINEAR;
    }
    key.fg.engine = p->engine;
    if (generating) {
        key.fg.method = p->method; key.fg.state = p->state;
        key.fg.cooldown = p->state == VK_OVERLAY_FG_COOLDOWN ? count_key(p->cooldown) : 0;
        key.fg.fallback = (p->engine == VK_OVERLAY_FG_QCOM || p->engine == VK_OVERLAY_FG_LSFG) && p->fallback;
        if (p->engine == VK_OVERLAY_FG_LSFG && !key.fg.fallback) {
            key.fg.major = MINV(p->lsfg_major, 100); key.fg.minor = MINV(p->lsfg_minor, 100);
            key.fg.performance = p->lsfg_performance != 0;
        }
    }
    key.gpu.real = generating ? decimal_key(p->gpu_real_ms) : -1;
    key.gpu.incremental = key.gpu.budget = -1;
    if (key.gpu.real != -1 && p->gpu_paired && isfinite(p->gpu_incremental_ms) && p->gpu_incremental_ms >= 0 && isfinite(p->gpu_budget_ms) && p->gpu_budget_ms >= 0) {
        key.gpu.paired = 1; key.gpu.incremental = decimal_key(p->gpu_incremental_ms); key.gpu.budget = decimal_key(p->gpu_budget_ms);
    }
    key.gpu.shown = count_key(p->generated_window_shown); key.gpu.expected = count_key(p->generated_window_expected);
    if (p->compile_at_ns && now_ns >= p->compile_at_ns && now_ns - p->compile_at_ns < SECOND_NS && p->compile_count) {
        key.compile.count = count_key(p->compile_count); key.compile.ms = decimal_key(p->compile_cost_ns / 1e6);
    }
    key.power.power = p->power_mw > 0 ? decimal_key(p->power_mw / 1000.0) : -1;
    key.power.input = p->power_mw > 0 && p->power_input;
    key.power.temperature = p->temperature_mc != INT_MIN && p->temperature_mc >= -50000 && p->temperature_mc <= 150000 ? p->temperature_mc / 100 : INT_MIN;
    if (p->system && !p->compact) {
        key.system.thermal = p->sys.thermal <= 7 ? p->sys.thermal : 0;
        key.system.headroom = p->sys.headroom <= 1000 ? p->sys.headroom : 0;
        key.system.cpu = p->sys.cpu <= 1024 ? p->sys.cpu : 0;
        key.system.ratio = p->sys.ratio <= 101 ? p->sys.ratio : 0;
        key.system.cap = p->sys.cap <= 2 ? p->sys.cap : 0;
        key.memory.available = memory_key(p->sys.available);
        key.memory.total = memory_key(p->sys.total);
        key.memory.rss = memory_key(p->sys.rss);
    }
    if (p->display_key_valid && !memcmp(&key, &p->display_key, sizeof key)) return;

    char a[8], b[8], c[8], line[VK_OVERLAY_COLS + 1];
    const vk_overlay_display_key *old = &p->display_key;
    if (!p->display_key_valid || memcmp(&key.rate, &old->rate, sizeof key.rate)) {
        decimal(a, key.rate.real); decimal(b, key.rate.shown); decimal(c, key.rate.ms);
        if (key.rate.real == -1) { memcpy(a, " --.-", 6); memcpy(c, " --.-", 6); }
        if (key.rate.shown == -1) memcpy(b, " --.-", 6);
        memcpy(p->mode_text, p->handheld ? "HH" : "DK", 3);
        if (key.rate.compact && generating) snprintf(line, sizeof line, "FPS %s->%s", a, b);
        else if (key.rate.compact) snprintf(line, sizeof line, "FPS %s", a);
        else if (generating) snprintf(line, sizeof line, "FPS %s->%s %sMS %s", a, b, c, p->mode_text);
        else snprintf(line, sizeof line, "FPS %s %sMS %s", a, c, p->mode_text);
        label(p, p->text, line);
    }
    if (!p->display_key_valid || memcmp(&key.resolution, &old->resolution, sizeof key.resolution)) {
        char render[10], output[10];
        extent(render, key.resolution.rw, key.resolution.rh); extent(output, key.resolution.ow, key.resolution.oh);
        const char *g = scale_name(key.resolution.game), *h = scale_name(key.resolution.host);
        if (key.resolution.arrow) snprintf(line, sizeof line, "%s->%s%s%s%s%s", render, output, *g || *h ? " " : "", g, *g && *h ? "+" : "", h);
        else snprintf(line, sizeof line, "%s%s%s%s%s", render, *g || *h ? " " : "", g, *g && *h ? "+" : "", h);
        label(p, p->resolution_text, line);
    }
    if (!p->display_key_valid || memcmp(&key.fg, &old->fg, sizeof key.fg)) {
        const char *engine = "OFF", *method = key.fg.method == VK_OVERLAY_FG_EXTRAP ? "EXTRAP" : "INTERP";
        char model[16], state[16];
        switch (key.fg.engine) {
            case VK_OVERLAY_FG_CAMERA: engine = "CAMERA"; break;
            case VK_OVERLAY_FG_QCOM: engine = key.fg.fallback ? "QCOM->CAMERA" : "QCOM"; break;
            case VK_OVERLAY_FG_LSFG:
                engine = key.fg.fallback ? "LSFG->CAMERA" : key.fg.performance ? "LSFG PERF" : "LSFG";
                if (!key.fg.fallback && (key.fg.major || key.fg.minor)) {
                    char major[4], minor[4];
                    if (key.fg.major > 99) memcpy(major, "??", 3); else snprintf(major, sizeof major, "%u", key.fg.major);
                    if (key.fg.minor > 99) memcpy(minor, "??", 3); else snprintf(minor, sizeof minor, "%u", key.fg.minor);
                    snprintf(model, sizeof model, "LSFG%s.%s%s", major, minor, key.fg.performance ? " PERF" : ""); engine = model;
                }
                break;
            case VK_OVERLAY_FG_ME2: engine = "ME2"; break;
            default: break;
        }
        switch (key.fg.state) {
            case VK_OVERLAY_FG_WARMUP: memcpy(state, "WARMUP", 7); break;
            case VK_OVERLAY_FG_COOLDOWN:
                count4(a, key.fg.cooldown); snprintf(state, sizeof state, "COOLDOWN%s", a); break;
            case VK_OVERLAY_FG_REFUSED: memcpy(state, "REFUSED", 8); break;
            default: memcpy(state, "ACTIVE", 7); break;
        }
        snprintf(line, sizeof line, "FG %s %s %s", engine, method, state); label(p, p->fg_text, line);
    }
    if (!p->display_key_valid || memcmp(&key.gpu, &old->gpu, sizeof key.gpu)) {
        char shown[8], expected[8]; count4(shown, key.gpu.shown); count4(expected, key.gpu.expected);
        if (key.gpu.real != -1) {
            decimal(a, key.gpu.real);
            if (key.gpu.paired) {
                decimal(b, key.gpu.incremental); decimal(c, key.gpu.budget);
                snprintf(line, sizeof line, "GPU %s+%s/%sMS GEN %s/%s", a, b, c, shown, expected);
            } else snprintf(line, sizeof line, "GPU %sMS GEN %s/%s", a, shown, expected);
        } else snprintf(line, sizeof line, "GEN %s/%s", shown, expected);
        label(p, p->gpu_text, line);
    }
    if (!p->display_key_valid || memcmp(&key.compile, &old->compile, sizeof key.compile)) {
        line[0] = 0;
        if (key.compile.count) {
            count4(b, key.compile.count); decimal(a, key.compile.ms);
            snprintf(line, sizeof line, "COMPILE %s %sMS", b, a);
        }
        label(p, p->compile_text, line);
    }
    if (!p->display_key_valid || memcmp(&key.power, &old->power, sizeof key.power)) {
        line[0] = 0;
        if (key.power.power != -1) { decimal(a, key.power.power); snprintf(line, sizeof line, "%-3s %sW", key.power.input ? "IN" : "PWR", a); }
        if (key.power.temperature != INT_MIN) {
            int tenths = key.power.temperature < 0 ? -key.power.temperature : key.power.temperature;
            snprintf(a, sizeof a, "%s%d.%d", key.power.temperature < 0 ? "-" : "", tenths / 10, tenths % 10);
            size_t n = strlen(line); snprintf(line + n, sizeof line - n, "%sBAT %5sC", n ? " " : "", a);
        }
        label(p, p->power_text, line);
    }
    if (!p->display_key_valid || memcmp(&key.system, &old->system, sizeof key.system)) {
        line[0] = 0;
        if (key.system.thermal) snprintf(line, sizeof line, "THM %u", key.system.thermal - 1);
        if (key.system.headroom) {
            unsigned v = key.system.headroom - 1; size_t n = strlen(line);
            snprintf(line + n, sizeof line - n, "%sH%u.%02u", n ? " " : "", v / 100, v % 100);
        }
        if (key.system.cpu && key.system.ratio) {
            unsigned v = key.system.ratio - 1; size_t n = strlen(line);
            snprintf(line + n, sizeof line - n, "%sCPU%u %u.%02u%s", n ? " " : "",
                key.system.cpu - 1, v / 100, v % 100, key.system.cap == 2 ? " CAP" : "");
        }
        label(p, p->system_text, line);
    }
    if (!p->display_key_valid || memcmp(&key.memory, &old->memory, sizeof key.memory)) {
        line[0] = 0;
        if (key.memory.total) {
            memory6(a, key.memory.available); memory6(b, key.memory.total);
            snprintf(line, sizeof line, "RAM %s/%sM", a, b);
        }
        if (key.memory.rss) {
            memory6(c, key.memory.rss); size_t n = strlen(line);
            snprintf(line + n, sizeof line - n, "%sRSS %sM", n ? " " : "", c);
        }
        label(p, p->memory_text, line);
    }
    p->display_key = key; p->display_key_valid = 1;
}
int vk_overlay_configure(vk_overlay_panel *p, uint32_t w, uint32_t h, int framegen, int power, const char *pos) {
    unsigned corner = !pos || (strcmp(pos, "tl") && strcmp(pos, "bl")) ? 1 : 0;
    if (pos && (!strcmp(pos, "bl") || !strcmp(pos, "br"))) corner |= 2;
    vk_overlay_layout *l = &p->layout;
    framegen = framegen != 0; power = power != 0;
    int compact = p->compact != 0;
    int system = p->system != 0;
    if (l->generation && l->output_w == w && l->output_h == h && l->framegen == framegen && l->power == power && l->position == corner && l->handheld == (p->handheld != 0) && l->compact == compact && l->system == system) return 0;
    l->output_w = w; l->output_h = h; l->framegen = framegen; l->power = power; l->position = corner; l->handheld = p->handheld != 0; l->compact = compact; l->system = system;
    if (compact) {
        /* One rate row, no graph (graph_w 0 skips every graph raster/copy); glyph height plus padding. */
        l->width = (framegen ? VK_OVERLAY_COMPACT_FG_COLS : VK_OVERLAY_COMPACT_COLS) * VK_OVERLAY_CELL_W + 2 * VK_OVERLAY_PAD;
        l->compile_row = l->power_row = l->system_row = UINT_MAX; l->rows = 1;
        l->height = 2 * VK_OVERLAY_PAD + VK_OVERLAY_GLYPH_H;
        l->graph_x = VK_OVERLAY_PAD; l->graph_y = l->height; l->graph_w = 0;
    } else {
        l->width = (framegen || system ? VK_OVERLAY_COLS : VK_OVERLAY_PLAIN_COLS) * VK_OVERLAY_CELL_W + 2 * VK_OVERLAY_PAD;
        l->compile_row = framegen ? 4 : 2; l->power_row = power ? l->compile_row + 1 : UINT_MAX;
        l->rows = l->compile_row + 1 + power;
        l->system_row = system ? l->rows : UINT_MAX;
        l->rows += system ? 2 : 0;
        l->graph_x = VK_OVERLAY_PAD; l->graph_y = VK_OVERLAY_PAD + l->rows * VK_OVERLAY_ROW_H;
        l->graph_w = l->width - 2 * VK_OVERLAY_PAD; l->height = l->graph_y + VK_OVERLAY_GRAPH_H + VK_OVERLAY_PAD;
    }
    uint32_t s = (uint32_t)(((uint64_t)h * 3 + 550) / 1100);
    if (s < 1) s = 1; if (s > VK_OVERLAY_MAX_SCALE) s = VK_OVERLAY_MAX_SCALE;
    while (s > 1 && ((uint64_t)l->width * s > w || (uint64_t)l->height * s > h)) s--;
    uint32_t rw = MINV(l->width * s, w), rh = MINV(l->height * s, h);
    uint32_t mx = MINV(VK_OVERLAY_PAD * s, (w - rw) / 2), my = MINV(VK_OVERLAY_PAD * s, (h - rh) / 2);
    l->rect = (vk_overlay_rect){corner & 1 ? w - rw - mx : mx, corner & 2 ? h - rh - my : my, rw, rh, s};
    l->generation++; p->counters.layout_rebuilds++;
    p->output_w = w; p->output_h = h;
    /* Configuration changes alone rebuild history; normal handoffs append exactly one column. */
    p->graph_next = p->graph_count = 0; p->graph_total = 0;
    unsigned n = MINV(p->count ? p->count - 1 : 0, l->graph_w);
    for (unsigned k = n; k; k--) {
        uint64_t end = p->timestamps[(p->next + 256 - k) % 256];
        uint64_t start = p->timestamps[(p->next + 255 - k) % 256];
        graph_append(p, (end - start) / 1e6);
    }
    return 1;
}
static const char *row_text(const vk_overlay_panel *p, unsigned row) {
    if (row == 0) return p->text;
    if (row == 1) return p->resolution_text;
    if (row == p->layout.compile_row) return p->compile_text;
    if (row == p->layout.power_row) return p->power_text;
    if (row == p->layout.system_row) return p->system_text;
    if (p->layout.system && row == p->layout.system_row + 1) return p->memory_text;
    return row == 2 ? p->fg_text : p->gpu_text;
}
/* Rotate the small cached panel while rasterizing changed cells/columns.
   Signed pixel strides avoid a second staging copy or an extra GPU pass. */
typedef struct { uint32_t *origin; ptrdiff_t dx,dy; } raster_target;
static raster_target raster_target_make(uint32_t *pixels, unsigned w, unsigned h, unsigned rotation) {
    switch (rotation & 3) {
    case 1: return (raster_target){pixels+h-1,(ptrdiff_t)h,-1};
    case 2: return (raster_target){pixels+(size_t)w*h-1,-1,-(ptrdiff_t)w};
    case 3: return (raster_target){pixels+(size_t)(w-1)*h,-(ptrdiff_t)h,1};
    default: return (raster_target){pixels,1,(ptrdiff_t)w};
    }
}
static void raster_cell(const vk_overlay_panel *p, unsigned row, unsigned col, unsigned ch, unsigned scale, raster_target out) {
    unsigned x = (VK_OVERLAY_PAD + col * VK_OVERLAY_CELL_W) * scale;
    unsigned y = (VK_OVERLAY_PAD + row * VK_OVERLAY_ROW_H) * scale;
    for (unsigned r = 0; r < 7; r++) for (unsigned sy = 0; sy < scale; sy++) {
        uint32_t *dst = out.origin + (y + r * scale + sy) * out.dy + x * out.dx;
        for (unsigned c = 0; c < VK_OVERLAY_CELL_W; c++) {
            uint32_t color = c < 5 && (font[ch & 127][r] & (16 >> c)) ? fg : bg;
            for (unsigned sx = 0; sx < scale; sx++) dst[(c * scale + sx) * out.dx] = color;
        }
    }
}
static void raster_column(const vk_overlay_panel *p, unsigned source, unsigned dest, unsigned scale, raster_target out) {
    int valid = p->graph_count == p->layout.graph_w || source < p->graph_count;
    double ms = valid ? p->graph_ms[source] : 0;
    double target = isfinite(p->target_ms) && p->target_ms > 0 ? p->target_ms : 1000.0 / 30;
    unsigned bar = valid ? (unsigned)(fmin(ms, 100.0) * VK_OVERLAY_GRAPH_H / 100.0 + .5) : 0;
    if (valid && !bar) bar = 1;
    unsigned line = (unsigned)(fmin(target, 100.0) * VK_OVERLAY_GRAPH_H / 100.0 + .5);
    if (line >= VK_OVERLAY_GRAPH_H) line = VK_OVERLAY_GRAPH_H - 1;
    uint32_t color = ms <= target + .000001 ? RGBA(80,220,100) : ms <= target * 1.5 + .000001 ? RGBA(255,210,40) : RGBA(240,70,60);
    for (unsigned y = 0; y < VK_OVERLAY_GRAPH_H; y++) {
        uint32_t pixel = y == VK_OVERLAY_GRAPH_H - 1 - line ? RGBA(120,140,160) : y >= VK_OVERLAY_GRAPH_H - bar ? color : bg;
        for (unsigned sy = 0; sy < scale; sy++) {
            uint32_t *dst = out.origin + ((p->layout.graph_y + y) * scale + sy) * out.dy + (p->layout.graph_x + dest) * scale * out.dx;
            for (unsigned sx = 0; sx < scale; sx++) dst[sx * out.dx] = pixel;
        }
    }
}
void vk_overlay_raster(const vk_overlay_panel *p, uint32_t scale, uint32_t *rgba) {
    if (!scale || scale > VK_OVERLAY_MAX_SCALE || !p->layout.generation) return;
    unsigned size = p->layout.width * p->layout.height * scale * scale;
    for (unsigned i = 0; i < size; i++) rgba[i] = bg;
    unsigned cols = (p->layout.width - 2 * VK_OVERLAY_PAD) / VK_OVERLAY_CELL_W;
    raster_target out = raster_target_make(rgba,p->layout.width*scale,p->layout.height*scale,0);
    for (unsigned r = 0; r < p->layout.rows; r++) {
        const char *s = row_text(p, r);
        for (unsigned c = 0; c < cols && s[c]; c++) raster_cell(p, r, c, (unsigned char)s[c], scale, out);
    }
    for (unsigned x = 0; x < p->layout.graph_w; x++) raster_column(p, (p->graph_next + x) % p->layout.graph_w, x, scale, out);
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
/* The recording thread owns accumulated history, layout, labels and mapped staging. Publications
   are best-effort relaxed snapshots; no setter initialises controls, allocates, locks or reads time. */
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "overlay publication must not acquire a runtime lock");
_Static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "overlay GPU publication requires IEEE-754 binary64");
static _Atomic uint64_t handoffs[256], target_ns, resolution_sample, framegen_sample, model_sample;
/* IEEE-754 -1.0, not zero: no snapshot may invent a measured GPU sample before publication. */
static _Atomic uint64_t gpu_sample[3] = {UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000000)};
static _Atomic uint64_t compile_at[256], compile_cost[256];
static _Atomic unsigned handoff_count, scale_sample[2], model_performance, gpu_paired, compile_count;
static _Atomic unsigned power_sample;
static _Atomic int temperature_sample = INT_MIN;
static _Atomic unsigned system_sample[8];
static vk_overlay_panel history = {.power_mw = -1, .temperature_mc = INT_MIN, .gpu_real_ms = -1, .gpu_incremental_ms = -1, .gpu_budget_ms = -1};
vk_overlay_counters vk_overlay_get_counters(const vk_overlay_panel *panel) {
    return (panel ? panel : &history)->counters;
}
/* available: fixed at init (see vk_overlay_available). live_mode: the shown mode, changed by
   vk_overlay_cycle from the input thread and read by the recorder at every present. */
static int available, power_enabled, system_enabled;
static _Atomic int live_mode, power_started;
static const char *position;
void vk_overlay_set_resolution(uint32_t rw, uint32_t rh, uint32_t cw, uint32_t ch) {
    if (!available || rw > 65535 || rh > 65535 || cw > 65535 || ch > 65535) return;
    atomic_store_explicit(&resolution_sample, (uint64_t)rw << 48 | (uint64_t)rh << 32 | (uint64_t)cw << 16 | ch, memory_order_relaxed);
}
void vk_overlay_set_scale_stage(vk_overlay_scale_stage stage, vk_overlay_scale_method method) {
    if (!available || (unsigned)stage > VK_OVERLAY_STAGE_HOST || (unsigned)method > VK_OVERLAY_SCALE_SGSR) return;
    atomic_store_explicit(&scale_sample[stage], method, memory_order_relaxed);
}
void vk_overlay_set_framegen(vk_overlay_fg_engine engine, vk_overlay_fg_method method, vk_overlay_fg_state state, unsigned cooldown, int fallback) {
    if (!available || (unsigned)engine > VK_OVERLAY_FG_ME2 || (unsigned)method > VK_OVERLAY_FG_EXTRAP || (unsigned)state > VK_OVERLAY_FG_REFUSED) return;
    uint64_t sample = (uint64_t)cooldown << 16 | (uint64_t)(fallback != 0) << 8 | (unsigned)state << 5 | (unsigned)method << 4 | (unsigned)engine;
    atomic_store_explicit(&framegen_sample, sample, memory_order_relaxed);
}
void vk_overlay_set_lsfg_model(unsigned major, unsigned minor, int performance) {
    if (!available) return;
    atomic_store_explicit(&model_sample, (uint64_t)major << 32 | minor, memory_order_relaxed);
    atomic_store_explicit(&model_performance, performance != 0, memory_order_relaxed);
}
void vk_overlay_set_gpu(double real_ms, double incremental_ms, double budget_ms, int paired) {
    if (!available) return;
    double values[] = {real_ms, incremental_ms, budget_ms};
    for (unsigned i = 0; i < 3; i++) {
        uint64_t bits; memcpy(&bits, &values[i], sizeof bits);
        atomic_store_explicit(&gpu_sample[i], bits, memory_order_relaxed);
    }
    atomic_store_explicit(&gpu_paired, paired != 0, memory_order_relaxed);
}
void vk_overlay_note_compile(uint64_t at_ns, uint64_t cost_ns) {
    if (!available) return;
    unsigned n = atomic_fetch_add_explicit(&compile_count, 1, memory_order_relaxed) % 256;
    atomic_store_explicit(&compile_cost[n], cost_ns, memory_order_relaxed);
    atomic_store_explicit(&compile_at[n], at_ns, memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
void vk_overlay_test_publications(uint64_t *resolution, unsigned stages[2]) {
    *resolution = atomic_load_explicit(&resolution_sample, memory_order_relaxed);
    for (unsigned i = 0; i < 2; i++) stages[i] = atomic_load_explicit(&scale_sample[i], memory_order_relaxed);
}
void vk_overlay_test_snapshot(vk_overlay_panel *panel) { *panel = history; }
#endif
static int read_number(const char *root, const char *name, double *value) {
    char path[512]; if (snprintf(path, sizeof path, "%s/%s", root, name) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char extra; int ok = fscanf(f, "%lf %c", value, &extra) == 1; fclose(f);
    return ok && isfinite(*value);
}
static int meaningful_mw(double mw) {
    /* Below half a display unit would still print the misleading '0.0 W'. */
    return isfinite(mw) && mw >= 50 && mw <= 100000 ? (int)(mw + .5) : -1;
}
vk_overlay_power vk_overlay_read_power(const char *root) {
    vk_overlay_power unavailable = {-1, 0};
    char path[512], status[32] = {0};
    if (snprintf(path, sizeof path, "%s/battery/status", root) >= (int)sizeof path) return unavailable;
    FILE *f = fopen(path, "r");
    if (f) { if (fgets(status, sizeof status, f)) status[strcspn(status, "\r\n")] = 0; fclose(f); }
    int discharging = !strcmp(status, "Discharging");
    int plugged = !strcmp(status, "Charging") || !strcmp(status, "Full") || !strcmp(status, "Not charging");
    double current, voltage, online, power;
    int mw;
    if (discharging && read_number(root, "battery/current_now", &current) &&
        read_number(root, "battery/voltage_now", &voltage) && voltage > 0 &&
        (mw = meaningful_mw(fabs(current * voltage) / 1e9)) > 0) return (vk_overlay_power){mw, 0};
    /* Never substitute battery net charge current for device input power. Prefer an actual USB supply pair.
       An explicitly offline USB supply may expose stale current/voltage values, so reject it. */
    if ((!read_number(root, "usb/online", &online) || online == 1) &&
        read_number(root, "usb/current_now", &current) && current > 0 &&
        read_number(root, "usb/voltage_now", &voltage) && voltage > 0 &&
        (mw = meaningful_mw(current * voltage / 1e9)) > 0) return (vk_overlay_power){mw, 1};
    /* Some devices expose only this power counter while plugged in. Label the reported source IN,
       not whole-device consumption: battery power_now semantics can differ between devices. */
    if (plugged && read_number(root, "battery/power_now", &power) &&
        (mw = meaningful_mw(fabs(power) / 1000)) > 0) return (vk_overlay_power){mw, 1};
    return unavailable;
}
int vk_overlay_read_temperature(const char *root) {
    double value;
    if (!read_number(root, "battery/temp", &value) || value < -500 || value > 1500) return INT_MIN;
    return (int)lround(value * 100);
}
/* Kernel counters are KiB. Reject malformed/duplicate fields, not partial samples. */
static unsigned memory_mib(const char *root, const char *file, const char *field) {
    char path[512], line[256];
    if (snprintf(path, sizeof path, "%s/%s", root, file) >= (int)sizeof path) return UINT_MAX;
    FILE *f = fopen(path, "r"); if (!f) return UINT_MAX;
    unsigned result = UINT_MAX, found = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(field);
        if (strncmp(line, field, n) || line[n] != ':') continue;
        double kb; char unit[8], extra;
        if (++found != 1 || sscanf(line + n + 1, "%lf %7s %c", &kb, unit, &extra) != 2 ||
            strcmp(unit, "kB") || !isfinite(kb) || kb < 0 || kb >= (double)UINT_MAX * 1024 || floor(kb) != kb) {
            result = UINT_MAX; break;
        }
        result = (unsigned)(kb / 1024);
    }
    if (ferror(f)) result = UINT_MAX;
    fclose(f); return result;
}
vk_overlay_system vk_overlay_read_system(const char *cpu_root, const char *proc_root, int cpu) {
    vk_overlay_system s = {0};
    s.total = memory_mib(proc_root, "meminfo", "MemTotal");
    s.available = memory_mib(proc_root, "meminfo", "MemAvailable");
    if (s.total == UINT_MAX || s.available == UINT_MAX || s.available > s.total) s.available = s.total = 0;
    s.rss = memory_mib(proc_root, "self/status", "VmRSS");
    if (s.rss == UINT_MAX) s.rss = 0;
    if (cpu < 0 || cpu >= 1024) return s;
    char root[512];
    if (snprintf(root, sizeof root, "%s/cpu%d/cpufreq", cpu_root, cpu) >= (int)sizeof root) return s;
    double current, maximum, cap;
    if (read_number(root, "scaling_cur_freq", &current) && current > 0 &&
        read_number(root, "cpuinfo_max_freq", &maximum) && maximum > 0 &&
        current <= maximum && maximum <= INT_MAX) {
        s.cpu = (unsigned)cpu + 1; s.ratio = (unsigned)lround(current * 100 / maximum) + 1;
        if (read_number(root, "scaling_max_freq", &cap) && cap > 0 && cap <= maximum)
            s.cap = cap < maximum ? 2 : 1;
    }
    return s;
}
/* Follow g-main's actual last host CPU, never the sampler's CPU or a guessed cluster. */
static int main_cpu(void) {
    static unsigned tid;
    char path[128], text[1024];
    if (!tid) {
        DIR *dir = opendir("/proc/self/task"); if (!dir) return -1;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            char *end; unsigned long candidate = strtoul(entry->d_name, &end, 10);
            if (*end || !candidate || candidate > INT_MAX) continue;
            snprintf(path, sizeof path, "/proc/self/task/%lu/comm", candidate);
            FILE *f = fopen(path, "r"); if (!f) continue;
            int match = fgets(text, sizeof text, f) && !strcmp(text, "g-main\n"); fclose(f);
            if (match) { tid = (unsigned)candidate; break; }
        }
        closedir(dir);
    }
    if (!tid) return -1;
    snprintf(path, sizeof path, "/proc/self/task/%u/stat", tid);
    FILE *f = fopen(path, "r"); if (!f) { tid = 0; return -1; }
    int ok = fgets(text, sizeof text, f) != NULL; fclose(f);
    char *p = ok ? strrchr(text, ')') : NULL;
    if (!p || p[1] != ' ') return -1;
    p += 2; /* field 3 (state); processor is field 39 */
    for (unsigned field = 3; field < 39; field++) {
        p = strchr(p, ' '); if (!p) return -1;
        while (*p == ' ') p++;
    }
    char *end; long cpu = strtol(p, &end, 10);
    return end != p && (*end == ' ' || *end == '\n') && cpu >= 0 && cpu < 1024 ? (int)cpu : -1;
}
#ifdef __ANDROID__
/* Optional API29/30 symbols: older releases still load. Android headroom requires
   >=10s between calls; keep its last value between those samples. */
static void thermal_sample(vk_overlay_system *s) {
    static int initialized, tick;
    static void *manager;
    static int (*status)(void *);
    static float (*headroom)(void *, int);
    static unsigned cached;
    if (!initialized) {
        initialized = 1;
        void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            void *(*acquire)(void) = (void *(*)(void))dlsym(lib, "AThermal_acquireManager");
            status = (int (*)(void *))dlsym(lib, "AThermal_getCurrentThermalStatus");
            headroom = (float (*)(void *, int))dlsym(lib, "AThermal_getThermalHeadroom");
            if (acquire) manager = acquire();
        }
    }
    if (manager && status) { int v = status(manager); if (v >= 0 && v <= 6) s->thermal = (unsigned)v + 1; }
    if (manager && headroom && !tick) {
        float v = headroom(manager, 0);
        cached = isfinite(v) && v >= 0 && v < 9.995f ? (unsigned)lroundf(v * 100) + 1 : 0;
    }
    tick = (tick + 1) % 10;
    s->headroom = cached;
}
#endif
static void *power_sampler(void *unused) {
    (void)unused;
#ifdef __ANDROID__
    pthread_setname_np(pthread_self(), "totk-power");
#endif
    for (;;) {
        /* Only while the panel is shown: the live toggle may hide it for most of a session. */
        int mode = atomic_load_explicit(&live_mode, memory_order_relaxed);
        if (mode && power_enabled) {
            vk_overlay_power p = vk_overlay_read_power("/sys/class/power_supply");
            unsigned sample = p.mw > 0 ? (unsigned)p.mw << 1 | (unsigned)p.input : 0;
            atomic_store_explicit(&power_sample, sample, memory_order_relaxed);
            atomic_store_explicit(&temperature_sample, vk_overlay_read_temperature("/sys/class/power_supply"), memory_order_relaxed);
        }
        if (mode == 2 && system_enabled) {
            vk_overlay_system s = vk_overlay_read_system("/sys/devices/system/cpu", "/proc", main_cpu());
#ifdef __ANDROID__
            thermal_sample(&s);
#endif
            unsigned values[] = {s.thermal, s.headroom, s.cpu, s.ratio, s.cap, s.available, s.total, s.rss};
            for (unsigned i = 0; i < 8; i++) atomic_store_explicit(&system_sample[i], values[i], memory_order_relaxed);
        }
        /* Delay after sampling: reads can never be more frequent than once per second. */
        struct timespec delay = {.tv_sec = 1};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    return NULL;
}
static void power_start(void) {
    if ((!power_enabled && !system_enabled) || atomic_exchange_explicit(&power_started, 1, memory_order_relaxed)) return;
    pthread_t th; if (!pthread_create(&th, NULL, power_sampler, NULL)) pthread_detach(th);
}
static void init(void) {
    const char *e = getenv("TOTK_OVERLAY");
    int mode = !e ? (TOTK_DEV_BUILD ? 2 : 0) : (!strcmp(e, "1") || !strcmp(e, "fps")) ? 1 : !strcmp(e, "full") ? 2 : 0;
#ifdef TOTK_ANDROID_APP
    /* The in-game Minus+R shortcut can show the panel in any session. Keeping it possible costs
       TRANSFER_DST on the presenter images and a few relaxed stores per present; no staging, raster
       or copy happens until the panel is actually shown (APP.md, live overlay shortcut). */
    available = 1;
#else
    available = mode != 0;
#endif
    position = getenv("TOTK_OVERLAY_POS");
    e = getenv("TOTK_FPS60"); history.target_ms = e && !strcmp(e, "1") ? 1000.0 / 60 : 1000.0 / 30;
    history.handheld = runtime_handheld_mode();
    e = getenv("TOTK_OVERLAY_POWER"); power_enabled = available && e && !strcmp(e, "1");
    e = getenv("TOTK_OVERLAY_SYS"); system_enabled = available && e && !strcmp(e, "1");
    history.system = system_enabled;
    fprintf(stderr, "[totk-overlay] sys=%d (TOTK_OVERLAY_SYS, startup-only)\n", system_enabled);
    atomic_store_explicit(&live_mode, mode, memory_order_relaxed);
    if (mode) power_start();
}
int vk_overlay_mode(void) { pthread_once(&once, init); return atomic_load_explicit(&live_mode, memory_order_relaxed); }
int vk_overlay_available(void) { pthread_once(&once, init); return available; }
int vk_overlay_cycle(void) {
    pthread_once(&once, init);
    if (!available) return 0;
    int mode = atomic_load_explicit(&live_mode, memory_order_relaxed), next;
    do next = (mode + 1) % 3;
    while (!atomic_compare_exchange_weak_explicit(&live_mode, &mode, next, memory_order_relaxed, memory_order_relaxed));
    if (next) power_start();
    return next;
}
void vk_overlay_target(double period_s) {
    if (!available || !isfinite(period_s) || period_s <= 0 || period_s > 1) return;
    atomic_store_explicit(&target_ns, (uint64_t)(period_s * 1e9), memory_order_relaxed);
}
void vk_overlay_presented(uint64_t now_ns, int generated) {
    if (!available) return;
    unsigned n = atomic_load_explicit(&handoff_count, memory_order_relaxed);
    atomic_store_explicit(&handoffs[n % 256], (now_ns & ~UINT64_C(1)) | (generated != 0), memory_order_relaxed);
    atomic_store_explicit(&handoff_count, n + 1, memory_order_relaxed);
}
typedef struct {
    VkBuffer buf; VkDeviceMemory mem; uint32_t *pixels; VkDeviceSize size;
    unsigned layout_generation, text_generation, rotation;
    uint64_t graph_total;
    double target_ms;
    char cells[VK_OVERLAY_MAX_ROWS][VK_OVERLAY_COLS];
} staging;
static staging slots[VK_OVERLAY_SLOTS];
static int allocate(staging *s, VkDeviceSize size) {
    if (s->size >= size) return 1;
    /* The owning present slot is idle, so its prior GPU read has completed. */
    if (s->buf) {
        vkUnmapMemory(V.dev, s->mem); vkDestroyBuffer(V.dev, s->buf, NULL); vk_memory_release(s->mem, VK_MEMORY_OVERLAY);
        memset(s, 0, sizeof *s);
    }
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(V.dev, &ci, NULL, &s->buf)) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, s->buf, &req);
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vk_memory_allocate(&ai, &s->mem, VK_MEMORY_OVERLAY) || vkBindBufferMemory(V.dev, s->buf, s->mem, 0) ||
        vkMapMemory(V.dev, s->mem, 0, VK_WHOLE_SIZE, 0, (void **)&s->pixels)) {
        vkDestroyBuffer(V.dev, s->buf, NULL); vk_memory_release(s->mem, VK_MEMORY_OVERLAY);
        memset(s, 0, sizeof *s); return 0;
    }
    s->size = size;
    return 1;
}
static void sample_labels(vk_overlay_panel *p, uint64_t now, uint64_t fg_sample) {
    p->handheld = runtime_handheld_mode();
    unsigned sample = atomic_load_explicit(&power_sample, memory_order_relaxed);
    p->power_mw = sample ? (int)(sample >> 1) : -1; p->power_input = sample & 1;
    p->temperature_mc = atomic_load_explicit(&temperature_sample, memory_order_relaxed);
    if (system_enabled) {
        unsigned *fields[] = {&p->sys.thermal, &p->sys.headroom, &p->sys.cpu, &p->sys.ratio,
                              &p->sys.cap, &p->sys.available, &p->sys.total, &p->sys.rss};
        for (unsigned i = 0; i < 8; i++) *fields[i] = atomic_load_explicit(&system_sample[i], memory_order_relaxed);
    }
    uint64_t resolution = atomic_load_explicit(&resolution_sample, memory_order_relaxed);
    p->render_w = resolution >> 48; p->render_h = resolution >> 32 & 65535;
    p->canvas_w = resolution >> 16 & 65535; p->canvas_h = resolution & 65535;
    for (unsigned stage = 0; stage < 2; stage++) p->scale_stage[stage] = atomic_load_explicit(&scale_sample[stage], memory_order_relaxed);
    p->engine = fg_sample & 15; p->method = fg_sample >> 4 & 1; p->state = fg_sample >> 5 & 3;
    p->fallback = fg_sample >> 8 & 1; p->cooldown = fg_sample >> 16;
    uint64_t model = atomic_load_explicit(&model_sample, memory_order_relaxed);
    p->lsfg_major = model >> 32; p->lsfg_minor = (unsigned)model;
    p->lsfg_performance = atomic_load_explicit(&model_performance, memory_order_relaxed);
    double *values[] = {&p->gpu_real_ms, &p->gpu_incremental_ms, &p->gpu_budget_ms};
    for (unsigned i = 0; i < 3; i++) {
        uint64_t bits = atomic_load_explicit(&gpu_sample[i], memory_order_relaxed); memcpy(values[i], &bits, sizeof bits);
    }
    p->gpu_paired = atomic_load_explicit(&gpu_paired, memory_order_relaxed);
    unsigned count = atomic_load_explicit(&compile_count, memory_order_relaxed), n = MINV(count, 256);
    p->compile_at_ns = p->compile_cost_ns = 0; p->compile_count = 0;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (count - 1 - k) % 256;
        uint64_t at = atomic_load_explicit(&compile_at[i], memory_order_relaxed);
        if (!at || at > now || now - at >= SECOND_NS) continue;
        uint64_t cost = atomic_load_explicit(&compile_cost[i], memory_order_relaxed);
        p->compile_count++;
        if (at > p->compile_at_ns) p->compile_at_ns = at;
        if (UINT64_MAX - p->compile_cost_ns < cost) p->compile_cost_ns = UINT64_MAX;
        else p->compile_cost_ns += cost;
        /* A complete retained ring inside this window cannot prove that no events were lost. */
        if (count > 256 && k == 255) { p->compile_cost_ns = UINT64_MAX; p->compile_count = UINT_MAX; }
    }
}
static int update_staging(staging *s, const vk_overlay_panel *p, unsigned rotation) {
    unsigned scale = p->layout.rect.scale;
    int fresh = s->layout_generation != p->layout.generation || s->rotation != rotation;
    if (fresh) {
        VkDeviceSize size = (VkDeviceSize)p->layout.width * p->layout.height * scale * scale * sizeof *s->pixels;
        if (!allocate(s, size)) return 0;
        for (VkDeviceSize i = 0; i < size / sizeof *s->pixels; i++) s->pixels[i] = bg;
        memset(s->cells, ' ', sizeof s->cells);
        s->layout_generation = p->layout.generation;
        s->rotation = rotation;
    }
    raster_target out = raster_target_make(s->pixels,p->layout.width*scale,p->layout.height*scale,rotation);
    if (fresh || s->text_generation != p->text_generation) {
        unsigned cols = (p->layout.width - 2 * VK_OVERLAY_PAD) / VK_OVERLAY_CELL_W;
        for (unsigned r = 0; r < p->layout.rows; r++) {
            const char *text = row_text(p, r); int end = 0;
            for (unsigned c = 0; c < cols; c++) {
                unsigned char ch = end ? ' ' : (unsigned char)text[c];
                if (!ch) { end = 1; ch = ' '; }
                if ((unsigned char)s->cells[r][c] == ch) continue;
                raster_cell(p, r, c, ch, scale, out); s->cells[r][c] = ch;
            }
        }
        s->text_generation = p->text_generation;
    }
    uint64_t delta = p->graph_total - s->graph_total;
    unsigned n = fresh || s->target_ms != p->target_ms || delta >= p->layout.graph_w ? p->layout.graph_w : (unsigned)delta;
    unsigned start = n == p->layout.graph_w ? 0 : (p->graph_next + p->layout.graph_w - n) % p->layout.graph_w;
    for (unsigned k = 0; k < n; k++) {
        unsigned x = (start + k) % p->layout.graph_w; raster_column(p, x, x, scale, out);
    }
    s->graph_total = p->graph_total; s->target_ms = p->target_ms;
    return 1;
}
static void copy_region(VkBufferImageCopy copies[6], unsigned *count, const vk_overlay_layout *l,
                        unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w, unsigned h, unsigned rotation) {
    if (dx >= l->rect.w || dy >= l->rect.h || !w || !h) return;
    w = MINV(w, l->rect.w - dx); h = MINV(h, l->rect.h - dy);
    unsigned width = l->width * l->rect.scale, height = l->height * l->rect.scale;
    vk_present_extent panel = vk_present_rotate_extent(width,height,rotation);
    vk_present_rect src = vk_present_rotate_rect((vk_present_rect){(int32_t)sx,(int32_t)sy,w,h},width,height,rotation);
    vk_present_rect dst = vk_present_rotate_rect((vk_present_rect){(int32_t)(l->rect.x+dx),(int32_t)(l->rect.y+dy),w,h},
        l->output_w,l->output_h,rotation);
    copies[(*count)++] = (VkBufferImageCopy){.bufferOffset = ((VkDeviceSize)src.y * panel.w + src.x) * 4,
        .bufferRowLength = panel.w, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {dst.x,dst.y,0}, .imageExtent = {dst.w,dst.h,1}};
}
int vk_overlay_record(VkCommandBuffer cb, VkImage image, VkImageLayout layout, uint32_t w, uint32_t h, unsigned slot, unsigned rotation) {
    int mode = vk_overlay_mode();
    if (!mode || slot >= VK_OVERLAY_SLOTS || !w || !h) return 0;
    static unsigned consumed;
    static int shown_mode;
    vk_overlay_panel *p = &history;
    if (mode != shown_mode) {
        /* A live toggle: relabel at once (not at the next 1 Hz tick) and re-lay out below. */
        shown_mode = mode; p->compact = mode == 1; p->text_ns = 0; p->display_key_valid = 0;
    }
    uint64_t now = clock_ns();
    int refresh = !p->text[0] || (now >= p->text_ns && now - p->text_ns >= SECOND_NS);
    if (refresh) sample_labels(p, now, atomic_load_explicit(&framegen_sample, memory_order_relaxed));
    /* Configuration identity, not text length or telemetry values, owns geometry. */
    if (!p->layout.generation || p->layout.output_w != w || p->layout.output_h != h || p->layout.framegen != (p->engine != VK_OVERLAY_FG_OFF) ||
        p->layout.handheld != (p->handheld != 0) || p->layout.compact != p->compact)
        vk_overlay_configure(p, w, h, p->engine != VK_OVERLAY_FG_OFF, power_enabled, position);
    unsigned produced = atomic_load_explicit(&handoff_count, memory_order_relaxed);
    if (produced - consumed > 256) consumed = produced - 256;
    while (consumed != produced) {
        uint64_t sample = atomic_load_explicit(&handoffs[consumed++ % 256], memory_order_relaxed);
        if (sample) vk_overlay_note_frame(p, sample & ~UINT64_C(1), sample & 1);
    }
    uint64_t target = atomic_load_explicit(&target_ns, memory_order_relaxed);
    if (target) p->target_ms = target / 1e6;
    if (refresh) vk_overlay_text(p, now);
    staging *s = &slots[slot];
    if (!update_staging(s, p, rotation)) return 0;
    const vk_overlay_layout *l = &p->layout;
    unsigned scale = l->rect.scale, stride = l->width * scale, gy = l->graph_y * scale;
    unsigned gx = l->graph_x * scale, gw = l->graph_w * scale, gh = l->graph_w ? VK_OVERLAY_GRAPH_H * scale : 0;
    unsigned next = p->graph_next * scale, count = 0;
    VkBufferImageCopy copies[6];
    copy_region(copies, &count, l, 0, 0, 0, 0, stride, gy, rotation);
    copy_region(copies, &count, l, 0, gy, 0, gy, gx, gh, rotation);
    copy_region(copies, &count, l, gx + gw, gy, gx + gw, gy, stride - gx - gw, gh, rotation);
    copy_region(copies, &count, l, 0, gy + gh, 0, gy + gh, stride, l->height * scale - gy - gh, rotation);
    /* Rotate only the copy regions, not CPU history/pixels: blank prefix then oldest-to-newest. */
    copy_region(copies, &count, l, gx + next, gy, gx, gy, gw - next, gh, rotation);
    copy_region(copies, &count, l, gx, gy, gx + gw - next, gy, next, gh, rotation);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdCopyBufferToImage(cb, s->buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies);
    ib.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = layout;
    vkCmdPipelineBarrier2(cb, &di);
    return 1;
}

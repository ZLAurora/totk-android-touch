/* Fresh-process host fixture: real presenter commands on lavapipe, no game/shader seed required.
   make -C android vktest-overlay TARGET=linux CC=cc OUT=<scratch>/overlay
   TOTK_OVERLAY=off|fps|full|1 TOTK_OVERLAY_POS=tl|tr|bl|br
   TOTK_POST_AA=off|smaa TOTK_POST_SCALE=off|fsr|sgsr <scratch>/overlay/vkoverlaytest [frame.ppm]
   Every presented frame checks exact output pixels, including after present-slot reuse.
   An optional PPM records 125 paced frames instead of seven; --bench measures CPU reraster only.
   TOTK_NATIVE_UI=1 also checks a 1920x1080 single-pixel pattern with stale APP_WIDTH/scale,
   reattachment, and aspect-fit bars on wider/taller windows before restoring 1080p.
   vk_present_policy.h is the shared copy/AHB extent policy; AHB import itself is not mocked.
   This host fixture executes the copy presenter, not Android's AHB import/foreign release path.
   After detach, standalone GPU images exercise all overlay rotations, clipped panels and cached slot reuse. */
#include "vk_int.h"
#include "vk_overlay.h"
#include "vk_present_policy.h"
#include "vk_present_spv.h"
#include "vk_present_rotated_spv.h"
#include "build_kind.h"
#include <android/native_window.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>

enum { TEST_PLAIN_W = 196, TEST_FG_W = 226, TEST_PLAIN_H = 49, TEST_FG_H = 71 };
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)
#define COLOR(r,g,b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xff000000u)
enum { SOURCE_W = 96, SOURCE_H = 32, OUTPUT_W = 960, OUTPUT_H = 320, WINDOW_PAD = 7 };
static uint32_t source_w = SOURCE_W, source_h = SOURCE_H, output_w = OUTPUT_W, output_h = OUTPUT_H;
static uint32_t window_w = OUTPUT_W, window_h = OUTPUT_H;
static int native_ui, native_pattern, guest_output;
static uint32_t guest_canvas_w, guest_canvas_h;
static _Atomic unsigned window_queries;
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

/* Strided window, as in test_window_mock.c, with a publication condition so pixel reads are race-free. */
struct ANativeWindow { int unused; };
static struct ANativeWindow window;
static pthread_mutex_t window_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t posted = PTHREAD_COND_INITIALIZER;
static unsigned posts;
static uint32_t *window_pixels;
static int width, height, stride;
static unsigned geometry_calls, references;
void ANativeWindow_acquire(ANativeWindow *w) {
    (void)w; pthread_mutex_lock(&window_mu); references++; pthread_mutex_unlock(&window_mu);
}
void ANativeWindow_release(ANativeWindow *w) {
    (void)w; pthread_mutex_lock(&window_mu); references--; pthread_cond_signal(&posted); pthread_mutex_unlock(&window_mu);
}
int32_t ANativeWindow_getWidth(ANativeWindow *w) {
    (void)w; atomic_fetch_add_explicit(&window_queries, 1, memory_order_relaxed); return (int32_t)window_w;
}
int32_t ANativeWindow_getHeight(ANativeWindow *w) {
    (void)w; atomic_fetch_add_explicit(&window_queries, 1, memory_order_relaxed); return (int32_t)window_h;
}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t ww, int32_t hh, int32_t format) {
    (void)w; (void)format; width = ww; height = hh; stride = ww + WINDOW_PAD;
    geometry_calls++;
    free(window_pixels); window_pixels = calloc((size_t)stride * height, 4); return window_pixels ? 0 : -1;
}
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *out, ARect *dirty) {
    (void)w; (void)dirty;
    *out = (ANativeWindow_Buffer){.width = width, .height = height, .stride = stride, .format = 2, .bits = window_pixels}; return 0;
}
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) {
    (void)w; pthread_mutex_lock(&window_mu); posts++; pthread_cond_signal(&posted); pthread_mutex_unlock(&window_mu); return 0;
}
void front_frame_presented(void) {}
uint64_t vk_window_set(ANativeWindow *w, ANativeWindow *surface_parent, int display_id);
void vk_window_resized(ANativeWindow *w, uint64_t epoch);
unsigned vk_window_test_fit_rebuilds(void);
uint64_t vk_window_test_cached_extent(void);
static int wait_post(unsigned n) {
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 10;
    pthread_mutex_lock(&window_mu);
    while (posts < n) if (pthread_cond_timedwait(&posted, &window_mu, &until)) break;
    int ok = posts == n; pthread_mutex_unlock(&window_mu); return ok;
}
static int wait_detached(void) {
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 10;
    pthread_mutex_lock(&window_mu);
    while (references) if (pthread_cond_timedwait(&posted, &window_mu, &until)) break;
    int ok = !references; pthread_mutex_unlock(&window_mu); return ok;
}
static void size_policy_tests(void) {
    static const struct { int native, scale; uint32_t app, ww, wh, w, h; } cases[] = {
        {0,0,1280,1920,1080,1280,720}, {0,0,960,1920,1080,960,540},
        {0,1,960,1920,1080,1920,1080}, {0,0,1280,0,0,1280,720},
        {1,0,960,1920,1080,1920,1080}, {1,1,640,1920,1080,1920,1080},
        {1,1,3840,1920,1080,1920,1080}, {1,1,960,2400,1080,2400,1080},
        {1,0,960,1920,1200,1920,1200}, {1,1,960,1280,720,1280,720},
        {1,1,960,0,0,1920,1080}, {1,1,960,1920,0,1920,1080},
        {1,1,960,0,1080,1920,1080},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        vk_present_extent r = vk_present_output_size(cases[i].native, cases[i].app,
            cases[i].scale, cases[i].ww, cases[i].wh);
        CHECK(r.w == cases[i].w && r.h == cases[i].h,
              "shared copy/AHB size case %u: %ux%u expected %ux%u", i, r.w, r.h, cases[i].w, cases[i].h);
    }
}
/* Inspect the actual committed shader declarations, independently of the
 * pixel oracle. Command/layout selection is traced by the native-UI policy
 * fixture; this host path does not claim Android AHB import coverage. */
static unsigned present_push_members(const uint32_t *code, size_t words) {
    uint32_t pointer=0,structure=0;
    for (size_t i=5;i<words;) {
        unsigned count=code[i]>>16,op=code[i]&0xffff;
        if (!count || count>words-i) return 0;
        if (op==59 && count>=4 && code[i+3]==9) pointer=code[i+1]; /* PushConstant variable */
        i+=count;
    }
    for (size_t i=5;i<words;) {
        unsigned count=code[i]>>16,op=code[i]&0xffff;
        if (op==32 && count==4 && code[i+1]==pointer) structure=code[i+3]; /* OpTypePointer */
        i+=count;
    }
    for (size_t i=5;i<words;) {
        unsigned count=code[i]>>16,op=code[i]&0xffff;
        if (op==30 && code[i+1]==structure) return count-2; /* OpTypeStruct */
        i+=count;
    }
    return 0;
}
static unsigned present_dot_count(const uint32_t *code, size_t words) {
    unsigned dots=0;
    for (size_t i=5;i<words;) {
        unsigned count=code[i]>>16;
        if (!count || count>words-i) return UINT_MAX;
        dots+=(code[i]&0xffff)==148; /* OpDot */
        i+=count;
    }
    return dots;
}
static void present_shader_contract_tests(void) {
    CHECK(pres_vert_spv[3]==0x3b &&
        present_push_members(pres_vert_spv,sizeof pres_vert_spv/sizeof *pres_vert_spv)==1,
        "original AHB shader retains its single vec4 / 16-byte crop declaration");
    CHECK(!present_dot_count(pres_vert_spv,sizeof pres_vert_spv/sizeof *pres_vert_spv),
        "legacy AHB vertex program performs no rotation dot products");
    CHECK(present_push_members(pres_rotated_vert_spv,
        sizeof pres_rotated_vert_spv/sizeof *pres_rotated_vert_spv)==2 &&
        present_dot_count(pres_rotated_vert_spv,
        sizeof pres_rotated_vert_spv/sizeof *pres_rotated_vert_spv)==2,
        "separate rotated AHB shader declares crop/matrix and two dot products");
}
static uint32_t source_pixel(unsigned x, unsigned y) {
    if (!native_pattern) return COLOR(0,255,0);
    return COLOR(x & 1 ? 255 : 0, y & 1 ? 255 : 0, (x / 7 + y / 11) & 1 ? 255 : 0);
}
static uint32_t background_pixel(unsigned x, unsigned y) {
    if (guest_output) {
        vk_present_rect fit=vk_present_guest_fit(guest_canvas_w ? guest_canvas_w : source_w,
            guest_canvas_h ? guest_canvas_h : source_h,output_w,output_h);
        return x<(unsigned)fit.x || x>=(unsigned)fit.x+fit.w ||
            y<(unsigned)fit.y || y>=(unsigned)fit.y+fit.h ? COLOR(0,0,0) : COLOR(0,255,0);
    }
    if (!native_ui) return COLOR(0,255,0);
    unsigned dx = (output_w - source_w) / 2, dy = (output_h - source_h) / 2;
    return x < dx || x >= dx + source_w || y < dy || y >= dy + source_h ?
        COLOR(0,0,0) : source_pixel(x - dx, y - dy);
}
static unsigned fixed_glyph_mismatches(const uint32_t *pixels,unsigned width,unsigned x,unsigned y,
                                       const uint8_t rows[][7],unsigned letters) {
    unsigned bad=0;
    for(unsigned i=0;i<letters;i++)for(unsigned dy=0;dy<7;dy++)for(unsigned dx=0;dx<6;dx++){
        uint32_t want=dx<5&&(rows[i][dy]&(16u>>dx))?COLOR(224,240,224):COLOR(12,12,16);
        bad+=pixels[(size_t)(y+dy)*width+x+i*6+dx]!=want;
    }
    return bad;
}
static void check_presented_frame(unsigned frame) {
    CHECK(width == (int)output_w && height == (int)output_h && stride == (int)output_w + WINDOW_PAD,
          "frame %u output is %dx%d with strided rows (%d), expected %ux%u", frame, width, height, stride, output_w, output_h);
    if (width != (int)output_w || height != (int)output_h || stride != (int)output_w + WINDOW_PAD) return;
    int on = vk_overlay_mode();
    const char *pos = getenv("TOTK_OVERLAY_POS");
    int right = !pos || (strcmp(pos,"tl") && strcmp(pos,"bl")), bottom = pos && (!strcmp(pos,"bl") || !strcmp(pos,"br"));
    unsigned s = (output_h * 3 + 550) / 1100;
    if (!s) s=1; if (s>VK_OVERLAY_MAX_SCALE) s=VK_OVERLAY_MAX_SCALE;
    vk_overlay_panel layout = {.compact = on == 1};
    layout.system = getenv("TOTK_OVERLAY_SYS") && !strcmp(getenv("TOTK_OVERLAY_SYS"), "1");
    int fg = getenv("TOTK_OVERLAY_TEST_FG") != NULL;
    int power_enabled = getenv("TOTK_OVERLAY_POWER") && !strcmp(getenv("TOTK_OVERLAY_POWER"), "1");
    vk_overlay_configure(&layout, output_w, output_h, fg, power_enabled, pos);
    vk_overlay_rect r = layout.layout.rect;
    CHECK(r.scale == s && r.w == layout.layout.width * s && r.h == layout.layout.height * s &&
          r.x == (right ? output_w - r.w - 2*s : 2*s) &&
          r.y == (bottom ? output_h - r.h - 2*s : 2*s),
          "frame %u selected corner has independently expected %s geometry", frame, on == 1 ? "one-row fps" : "compact");
    unsigned outside = 0, border = 0, palette = 0, ink = 0, padding = 0;
    for (unsigned y = 0; y < output_h; y++) for (unsigned x = 0; x < output_w + WINDOW_PAD; x++) {
        uint32_t pixel = window_pixels[(size_t)y * stride + x];
        if (x >= output_w) { if (pixel) padding++; continue; }
        int inside = on && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
        if (!inside) { if (pixel != background_pixel(x,y)) outside++; continue; }
        unsigned px = x - r.x, py = y - r.y;
        if ((px < 2*s || px >= r.w - 2*s || py < 2*s || py >= r.h - 2*s) && pixel != COLOR(12,12,16)) border++;
        if (pixel == COLOR(224,240,224)) ink++;
        if (pixel != COLOR(12,12,16) && pixel != COLOR(224,240,224) && pixel != COLOR(120,140,160) &&
            pixel != COLOR(80,220,100) && pixel != COLOR(255,210,40) && pixel != COLOR(240,70,60)) palette++;
    }
    CHECK(!outside, "frame %u preserves every background pixel (%u changed)", frame, outside);
    CHECK(!padding, "frame %u presenter leaves row padding untouched (%u changed)", frame, padding);
    if (!on) return;
    CHECK(!border && !palette && ink > 20*s*s, "frame %u compact panel has exact opaque raster pixels", frame);
}
static void generation_window_tests(void) {
    const uint64_t base=UINT64_C(1000000000),second=UINT64_C(1000000000);
    vk_overlay_panel p={.engine=VK_OVERLAY_FG_QCOM,.state=VK_OVERLAY_FG_WARMUP,
        .fallback=1,.gpu_real_ms=-1,.temperature_mc=INT_MIN};
    vk_overlay_configure(&p,1920,1080,1,0,"tr");
    for(unsigned i=1;i<=300;i++){
        vk_overlay_note_frame(&p,base+i*second/30,0);
        if(i>=271&&i<300)vk_overlay_note_frame(&p,base+(2*i+1)*second/60,1);
    }
    vk_overlay_text(&p,base+10*second);
    CHECK(p.generated_window_shown==29&&p.generated_window_expected==30,
        "GEN counts the first trailing second without a cumulative baseline");
    /* The real diagnostics exchange g_fg_handoffs to0 at300, not g_pres_shown.
       Reusing those mixed-lifetime totals makes the next30 real frames look like59. */
    CHECK((UINT64_C(362)-3)-(UINT64_C(329)-29)==59,
        "negative control reproduces the old reset-counter expected59");
    p.state=VK_OVERLAY_FG_COOLDOWN;p.cooldown=30;
    for(unsigned i=301;i<=330;i++){
        vk_overlay_note_frame(&p,base+i*second/30,0);
        if(i>=327&&i<330)vk_overlay_note_frame(&p,base+(2*i+1)*second/60,1);
    }
    p.state=VK_OVERLAY_FG_ACTIVE;p.fallback=0;p.cooldown=0;
    vk_overlay_text(&p,base+11*second);
    CHECK(p.generated_window_shown==3&&p.generated_window_expected==30,
        "GEN stays3/30 across the300-frame stats reset and recovery transition, never3/59");
    for(unsigned i=331;i<=420;i++){
        vk_overlay_note_frame(&p,base+i*second/30,0);
        if(i<420)vk_overlay_note_frame(&p,base+(2*i+1)*second/60,1);
    }
    vk_overlay_text(&p,base+14*second);
    CHECK(p.generated_window_shown==30&&p.generated_window_expected==30,
        "three-second refresh stall still reports only the trailing30/30, not accumulated90/90");
    CHECK(p.count==256&&p.generated_window_expected==30,
        "GEN remains correct after the timestamp ring wraps");
    for(unsigned i=421;i<=450;i++)vk_overlay_note_frame(&p,base+i*second/30,0);
    vk_overlay_text(&p,base+15*second);
    CHECK(p.generated_window_shown==0&&p.generated_window_expected==30&&
        fabs(p.fps-30)<.001&&fabs(p.real_fps-30)<.001,
        "map/loading real-only output reports0/30 with30->30");
    vk_overlay_text(&p,base+16*second);
    CHECK(!p.generated_window_shown&&!p.generated_window_expected&&!p.fps,
        "empty trailing second excludes the exact old boundary and clears stale GEN/FPS");
    CHECK(p.counters.layout_rebuilds==1,"GEN window/state changes never rebuild layout");
    vk_overlay_panel saturated={.engine=VK_OVERLAY_FG_CAMERA,.gpu_real_ms=-1,.temperature_mc=INT_MIN};
    vk_overlay_configure(&saturated,1920,1080,1,0,"tr");
    for(unsigned i=1;i<=300;i++)vk_overlay_note_frame(&saturated,base+i*UINT64_C(1000000),i&1);
    vk_overlay_text(&saturated,base+300*UINT64_C(1000000));
    CHECK(saturated.generated_window_shown==UINT64_MAX&&saturated.generated_window_expected==UINT64_MAX&&
        strstr(saturated.gpu_text,"????" "/" "????"),"incomplete retained window is unknown, never truncated counts");
}
static void cpu_tests(void) {
    vk_overlay_panel p={.target_ms=1000.0/30,.power_mw=-1};
    uint64_t t=1000000000ull;
    CHECK(vk_overlay_configure(&p,1920,1080,1,0,"tr"),"initial fixed FG configuration");
    vk_overlay_note(&p,t);
    const uint64_t dt[]={16000000,33333333,40000000,50000000,60000000,120000000};
    for(unsigned i=0;i<6;i++){t+=dt[i];vk_overlay_note(&p,t);}
    vk_overlay_text(&p,t);
    CHECK(fabs(p.mean_ms-319.333333/6)<.00001&&fabs(p.fps-6000/319.333333)<.00001,"mean/FPS from handoff intervals");
    CHECK(p.counters.graph_appends==6,"one graph append per valid interval");
    static const uint8_t fps_glyphs[3][7]={{31,16,16,30,16,16,16},{30,17,17,30,16,16,16},{15,16,16,14,1,1,30}};
    uint32_t raster[VK_OVERLAY_W*VK_OVERLAY_MAX_H];
    vk_overlay_raster(&p,1,raster);
    unsigned fps_bad=fixed_glyph_mismatches(raster,p.layout.width,2,2,fps_glyphs,3);
    CHECK(!fps_bad,"FPS glyphs and empty cell are exact (%u mismatches)",fps_bad);
    unsigned gap_bad=0;
    for(unsigned y=9;y<13;y++)for(unsigned x=2;x<p.layout.width-2;x++)
        gap_bad+=raster[(size_t)y*p.layout.width+x]!=COLOR(12,12,16);
    CHECK(!gap_bad,"four blank base-pixel rows preserve the old 11-pixel text pitch (%u mismatches)",gap_bad);
    vk_overlay_counters counters=vk_overlay_get_counters(&p);
    unsigned gen=p.layout.generation;
    CHECK(p.layout.width==TEST_FG_W&&p.layout.height==TEST_FG_H&&p.layout.graph_w==p.layout.width-2*VK_OVERLAY_PAD,"FG fixed layout and full-width graph");
    CHECK(!vk_overlay_configure(&p,1920,1080,1,0,"tr")&&p.layout.generation==gen,"changing telemetry does not reconfigure");
    vk_overlay_panel rate={.target_ms=10};
    vk_overlay_configure(&rate,1920,1080,0,0,"tr");
    uint64_t rt=1000000000ull;vk_overlay_note(&rate,rt);
    for(unsigned i=0;i<99;i++)vk_overlay_note(&rate,rt+=9900000ull);
    vk_overlay_text(&rate,rt);
    CHECK(fabs(rate.mean_ms-9.9)<.001,"9.9ms interval from measured handoffs");
    vk_overlay_counters before_round=vk_overlay_get_counters(&rate);
    for(unsigned i=0;i<100;i++)vk_overlay_note(&rate,rt+=10000000ull);
    vk_overlay_text(&rate,rt);
    CHECK(fabs(rate.mean_ms-10.0)<.001&&vk_overlay_get_counters(&rate).text_field_updates>before_round.text_field_updates,
          "measured 9.9ms to 10.0ms update changes rounded text");
    vk_overlay_panel fps_rate={.target_ms=10};
    vk_overlay_configure(&fps_rate,1920,1080,0,0,"tr");
    uint64_t ft=1000000000ull;vk_overlay_note(&fps_rate,ft);
    for(unsigned i=0;i<99;i++)vk_overlay_note(&fps_rate,ft+=10101010ull);
    vk_overlay_text(&fps_rate,ft);
    CHECK(fabs(fps_rate.fps-99.0)<.1,"99fps measured from handoff history");
    for(unsigned i=0;i<100;i++)vk_overlay_note(&fps_rate,ft+=10000000ull);
    vk_overlay_text(&fps_rate,ft);
    CHECK(fabs(fps_rate.fps-100.0)<.1,"100fps measured after rolling history advances");
    /* TOTK_OVERLAY=fps: the frame rate only (one row, no graph, no MS/mode); a live switch to full re-lays out
       from the same handoff history, so the full graph is complete at once. */
    vk_overlay_panel compact={.target_ms=1000.0/30,.power_mw=-1,.compact=1};
    CHECK(vk_overlay_configure(&compact,1920,1080,0,1,"tr"),"compact fps configuration");
    uint64_t ct=1000000000ull;vk_overlay_note(&compact,ct);
    for(unsigned i=0;i<60;i++)vk_overlay_note(&compact,ct+=33333333ull);
    vk_overlay_text(&compact,ct);
    CHECK(compact.layout.rows==1&&!compact.layout.graph_w&&compact.layout.power_row==UINT_MAX&&
          compact.layout.width==VK_OVERLAY_COMPACT_COLS*VK_OVERLAY_CELL_W+2*VK_OVERLAY_PAD&&
          compact.layout.height==2*VK_OVERLAY_PAD+VK_OVERLAY_GLYPH_H&&!strcmp(compact.text,"FPS  30.0"),
          "fps mode is one row \"%s\" without graph or power row",compact.text);
    uint32_t compact_raster[VK_OVERLAY_W*VK_OVERLAY_MAX_H];
    vk_overlay_raster(&compact,1,compact_raster);
    unsigned compact_bad=fixed_glyph_mismatches(compact_raster,compact.layout.width,2,2,fps_glyphs,3);
    CHECK(!compact_bad,"fps-mode FPS glyphs are exact (%u mismatches)",compact_bad);
    CHECK(vk_overlay_configure(&compact,1920,1080,1,1,"tr")&&compact.layout.width==VK_OVERLAY_COMPACT_FG_COLS*VK_OVERLAY_CELL_W+2*VK_OVERLAY_PAD,
          "fps mode reserves the generated rate field while frame generation is active");
    compact.compact=0;compact.display_key_valid=0;compact.text_ns=0;
    CHECK(vk_overlay_configure(&compact,1920,1080,0,1,"tr")&&compact.layout.graph_w&&compact.graph_count==60,
          "live fps -> full rebuilds the graph from the 60 retained handoff intervals");
    vk_overlay_text(&compact,ct);
    CHECK(strstr(compact.text,"MS DK")!=NULL,"full mode returns the frame-time and mode fields (\"%s\")",compact.text);
    p.output_w=5120;p.output_h=2880;p.render_w=4096;p.render_h=2160;p.canvas_w=4096;p.canvas_h=2160;
    vk_overlay_text(&p,t+3000000000ull);
    CHECK(strstr(p.resolution_text,"4096x2160->5120x2880"),"four-digit render/output extents remain explicit");
    CHECK(vk_overlay_get_counters(&p).layout_rebuilds==counters.layout_rebuilds,"render/canvas extent text update does not reconfigure");
    CHECK(vk_overlay_configure(&p,1920,1080,0,0,"tr"),"FG on/off changes configuration");
    CHECK(vk_overlay_get_counters(&p).layout_rebuilds==counters.layout_rebuilds+1,"configuration rebuild count increments once");
    CHECK(p.layout.width==TEST_PLAIN_W&&p.layout.height==TEST_PLAIN_H,"plain fixed template dimensions");
    CHECK(!vk_overlay_configure(&p,1920,1080,0,0,"tr"),"same config remains cached");
    p.output_w=1920;p.output_h=1080;p.render_w=1280;p.render_h=720;p.canvas_w=1280;p.canvas_h=720;p.handheld=1;
    CHECK(vk_overlay_configure(&p,1920,1080,0,0,"tr"),"HH configuration is reflected in layout");
    vk_overlay_text(&p,t+4000000000ull);
    CHECK(strstr(p.resolution_text,"1280x720->1920x1080 BILIN")&&!strcmp(p.mode_text,"HH"),"HH compact resolution and label");
    static const uint8_t hh[2][7]={{17,17,17,31,17,17,17},{17,17,17,31,17,17,17}};
    vk_overlay_raster(&p,1,raster);
    unsigned hh_bad=fixed_glyph_mismatches(raster,p.layout.width,2+(unsigned)(strlen(p.text)-2)*6,2,hh,2);
    CHECK(!hh_bad,"HH glyphs independently match the 5x7 golden (%u mismatches)",hh_bad);
    static const uint8_t bilin[5][7]={{30,17,17,30,17,17,30},{14,4,4,4,4,4,14},
        {16,16,16,16,16,16,31},{14,4,4,4,4,4,14},{17,25,25,21,19,19,17}};
    CHECK(!fixed_glyph_mismatches(raster,p.layout.width,2+(unsigned)(strlen(p.resolution_text)-5)*6,
        2+VK_OVERLAY_ROW_H,bilin,5),"BILIN resolution glyphs are exact");
    p.scale_stage[VK_OVERLAY_STAGE_GAME]=VK_OVERLAY_SCALE_FSR1;p.scale_stage[VK_OVERLAY_STAGE_HOST]=VK_OVERLAY_SCALE_SGSR;p.canvas_w=1600;p.canvas_h=900;p.handheld=0;
    CHECK(vk_overlay_configure(&p,1920,1080,0,0,"tr"),"DK configuration is reflected in layout");
    vk_overlay_text(&p,t+5000000000ull);
    CHECK(strstr(p.resolution_text,"1280x720->1920x1080 FSR1+SGSR")&&!strcmp(p.mode_text,"DK"),"DK and scaler labels");
    static const uint8_t dk[2][7]={{30,17,17,17,17,17,30},{17,18,20,24,20,18,17}};
    vk_overlay_raster(&p,1,raster);
    unsigned dk_bad=fixed_glyph_mismatches(raster,p.layout.width,2+(unsigned)(strlen(p.text)-2)*6,2,dk,2);
    static const uint8_t methods[9][7]={
        {31,16,16,30,16,16,16},{15,16,16,14,1,1,30},{30,17,17,30,20,18,17},{4,12,4,4,4,4,14},
        {0,4,4,31,4,4,0},{15,16,16,14,1,1,30},{14,17,16,23,17,17,15},
        {15,16,16,14,1,1,30},{30,17,17,30,20,18,17}};
    unsigned method_bad=fixed_glyph_mismatches(raster,p.layout.width,2+(unsigned)(strlen(p.resolution_text)-9)*6,2+VK_OVERLAY_ROW_H,methods,9);
    CHECK(!method_bad,"FSR1+SGSR resolution suffix matches every independent mask");
    CHECK(!dk_bad,"DK glyphs independently match the 5x7 golden (%u mismatches)",dk_bad);
    p.output_w=p.render_w;p.output_h=p.render_h;p.canvas_w=p.render_w;p.canvas_h=p.render_h;
    vk_overlay_text(&p,t+6000000000ull);
    CHECK(strstr(p.resolution_text,"1280x720")&&!strstr(p.resolution_text,"->"),"equal-size output omits scaler arrow");
    static const uint8_t equal_extent[8][7]={{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {14,17,17,14,17,17,14},{14,17,19,21,25,17,14},{0,0,17,10,4,10,17},
        {31,1,2,4,8,8,8},{14,17,1,2,4,8,31},{14,17,19,21,25,17,14}};
    vk_overlay_raster(&p,1,raster);
    CHECK(!fixed_glyph_mismatches(raster,p.layout.width,2+6,2+VK_OVERLAY_ROW_H,equal_extent,8),
        "equal-size resolution pixels retain all digits and the x glyph");
    p.render_w=p.render_h=0;p.output_w=1920;p.output_h=1080;vk_overlay_text(&p,t+7000000000ull);
    CHECK(strstr(p.resolution_text,"?")&&strstr(p.resolution_text,"1920x1080"),"unknown resolution does not invent values");
    vk_overlay_configure(&p,1920,1080,1,0,"tr");
    p.engine=VK_OVERLAY_FG_QCOM;p.method=VK_OVERLAY_FG_INTERP;p.state=VK_OVERLAY_FG_ACTIVE;p.fallback=1;
    p.lsfg_major=2;p.lsfg_minor=1;p.lsfg_performance=1;
    p.gpu_real_ms=-1;p.gpu_incremental_ms=NAN;p.gpu_budget_ms=-1;p.gpu_paired=0;
    p.compile_at_ns=t+7500000000ull;p.compile_cost_ns=45000000;p.compile_count=2;
    vk_overlay_text(&p,t+8000000000ull);
    CHECK(strstr(p.fg_text,"QCOM->CAMERA")&&strstr(p.fg_text,"INTERP"),"QCOM fallback/interpolation glyph text");
    CHECK(!strstr(p.gpu_text,"GPU")&&strstr(p.gpu_text,"GEN"),"unknown GPU omitted while generated totals remain visible");
    CHECK(strstr(p.compile_text,"COMPILE")&&strstr(p.compile_text,"   2")&&strstr(p.compile_text,"45.0MS"),
        "last-second compile count and summed duration visible");
    static const uint8_t fg_glyphs[2][7]={{31,16,16,30,16,16,16},{14,17,16,23,17,17,15}};
    static const uint8_t gen_glyphs[3][7]={{14,17,16,23,17,17,15},{31,16,16,30,16,16,31},{17,25,25,21,19,19,17}};
    static const uint8_t compile_glyphs[7][7]={
        {14,17,16,16,16,17,14},{14,17,17,17,17,17,14},{17,27,21,21,17,17,17},
        {30,17,17,30,16,16,16},{14,4,4,4,4,4,14},{16,16,16,16,16,16,31},{31,16,16,30,16,16,31}};
    vk_overlay_raster(&p,1,raster);
    unsigned fg_bad=fixed_glyph_mismatches(raster,p.layout.width,2,2+2*VK_OVERLAY_ROW_H,fg_glyphs,2);
    unsigned gen_bad=fixed_glyph_mismatches(raster,p.layout.width,2,2+3*VK_OVERLAY_ROW_H,gen_glyphs,3);
    unsigned compile_bad=fixed_glyph_mismatches(raster,p.layout.width,2,2+4*VK_OVERLAY_ROW_H,compile_glyphs,7);
    CHECK(!fg_bad&&!gen_bad&&!compile_bad,"FG, GEN and COMPILE words match independent glyph goldens");
    static const uint8_t qcom[4][7]={{14,17,17,17,21,18,13},{14,17,16,16,16,17,14},
        {14,17,17,17,17,17,14},{17,27,21,21,17,17,17}};
    CHECK(!fixed_glyph_mismatches(raster,p.layout.width,2+3*6,2+2*VK_OVERLAY_ROW_H,qcom,4),
        "QCOM engine glyphs, including Q, match exact masks");
    unsigned compile_row=p.layout.compile_row;
    p.compile_at_ns=0;p.compile_cost_ns=0;p.compile_count=0;vk_overlay_text(&p,t+9000000000ull);
    CHECK(!p.compile_text[0]&&p.layout.compile_row==compile_row,"compile expiry clears glyphs without moving reserved row");
    uint64_t state_rebuilds=p.counters.layout_rebuilds;
    unsigned w=p.layout.width,h=p.layout.height;
    p.method=VK_OVERLAY_FG_EXTRAP;p.state=VK_OVERLAY_FG_WARMUP;
    vk_overlay_text(&p,t+10000000000ull);
    CHECK(strstr(p.fg_text,"EXTRAP")&&strstr(p.fg_text,"WARMUP"),"extrapolation and warmup");
    static const uint8_t extrap[6][7]={{31,16,16,30,16,16,31},{17,17,10,4,10,17,17},
        {31,4,4,4,4,4,4},{30,17,17,30,20,18,17},{14,17,17,31,17,17,17},{30,17,17,30,16,16,16}};
    const char *extrap_at=strstr(p.fg_text,"EXTRAP");
    vk_overlay_raster(&p,1,raster);
    CHECK(extrap_at&&!fixed_glyph_mismatches(raster,p.layout.width,2+(unsigned)(extrap_at-p.fg_text)*6,
        2+2*VK_OVERLAY_ROW_H,extrap,6),"EXTRAP glyphs, including X and T, are exact");
    CHECK(p.layout.width==w&&p.layout.height==h,"text lengths do not change template geometry");
    p.state=VK_OVERLAY_FG_COOLDOWN;p.cooldown=4;vk_overlay_text(&p,t+11000000000ull);
    CHECK(strstr(p.fg_text,"COOLDOWN"),"cooldown state");
    p.state=VK_OVERLAY_FG_REFUSED;vk_overlay_text(&p,t+12000000000ull);
    CHECK(strstr(p.fg_text,"REFUSED"),"refused state");
    p.engine=VK_OVERLAY_FG_LSFG;p.fallback=0;p.state=VK_OVERLAY_FG_ACTIVE;vk_overlay_text(&p,t+13000000000ull);
    CHECK(strstr(p.fg_text,"LSFG")&&strstr(p.fg_text,"PERF"),"LSFG PERF label");
    CHECK(p.counters.layout_rebuilds==state_rebuilds,"engine/state text updates never rebuild layout");
    p.gpu_real_ms=8.25;p.gpu_incremental_ms=2.5;p.gpu_budget_ms=16.67;p.gpu_paired=1;
    vk_overlay_text(&p,t+14000000000ull);
    CHECK(p.gpu_text[0]&&strstr(p.gpu_text,"GPU"),"paired GPU timings");
    static const uint8_t gpu[3][7]={{14,17,16,23,17,17,15},{30,17,17,30,16,16,16},{17,17,17,17,17,17,14}};
    static const uint8_t slash[1][7]={{1,1,2,4,8,16,16}};
    vk_overlay_raster(&p,1,raster);
    const char *slash_at=strchr(p.gpu_text,'/');
    CHECK(!fixed_glyph_mismatches(raster,p.layout.width,2,2+3*VK_OVERLAY_ROW_H,gpu,3)&&
        slash_at&&!fixed_glyph_mismatches(raster,p.layout.width,2+(unsigned)(slash_at-p.gpu_text)*6,
            2+3*VK_OVERLAY_ROW_H,slash,1),"GPU and budget separator glyphs are exact");
    CHECK(vk_overlay_configure(&p,1920,1080,1,1,"tr")&&p.layout.power,"power option reserves row regardless of sample");
    vk_overlay_panel zero={.power_mw=0,.temperature_mc=INT_MIN};vk_overlay_text(&zero,t);
    CHECK(!zero.power_text[0],"zero power has no label");
    vk_overlay_panel ring={.target_ms=1000.0/60,.power_mw=5200};vk_overlay_note(&ring,t);
    for(unsigned i=0;i<300;i++)vk_overlay_note(&ring,t+=(uint64_t)10000000);
    vk_overlay_text(&ring,t);
    CHECK(ring.count==256&&fabs(ring.fps-100)<.0001,"history survives ring wrap");
    const char *positions[]={"tl","tr","bl","br"};
    for(unsigned i=0;i<4;i++){vk_overlay_panel corner={0};vk_overlay_configure(&corner,1920,1080,1,0,positions[i]);vk_overlay_rect r=corner.layout.rect;
        CHECK(r.scale==3&&r.w==corner.layout.width*3&&r.h==corner.layout.height*3&&r.x==(i&1?1920-r.w-6:6)&&r.y==(i&2?1080-r.h-6:6),"corner %s geometry",positions[i]);}
    vk_overlay_panel drs={.handheld=1,.temperature_mc=INT_MIN,.gpu_real_ms=-1};
    vk_overlay_configure(&drs,1920,1080,0,0,"tr");
    uint64_t drs_rebuilds=drs.counters.layout_rebuilds;
    for(unsigned i=0;i<16;i++){
        drs.render_w=i&1?864:960;drs.render_h=i&1?480:536;drs.canvas_w=1280;drs.canvas_h=720;
        CHECK(!vk_overlay_configure(&drs,1920,1080,0,0,"tr"),"DRS flip %u keeps fixed layout",i);
        vk_overlay_text(&drs,(i+1)*UINT64_C(1000000000));
    }
    CHECK(drs.counters.layout_rebuilds==drs_rebuilds,"sixteen aligned DRS flips cause zero rebuilds");
    CHECK(vk_overlay_configure(&drs,2400,1080,0,0,"tr")&&
        drs.counters.layout_rebuilds==drs_rebuilds+1,"one output viewport change causes exactly one rebuild");
    vk_overlay_text(&drs,UINT64_C(18000000000));
    uint64_t fields=drs.counters.text_field_updates;
    vk_overlay_text(&drs,UINT64_C(19000000000));
    CHECK(drs.counters.text_field_updates==fields,"stable fields stay untouched across a one-second refresh");
}
/* Explicit scratch root: no device/sysfs access, and no environment override of production sensor paths.
   Run vkoverlaytest --power <absent scratch directory>; all fixture files are removed afterwards. */
static int power_file(const char *root, const char *name, const char *value) {
    char path[512]; if (snprintf(path, sizeof path, "%s/%s", root, name) >= (int)sizeof path) return 0;
    if (!value) return !unlink(path) || errno == ENOENT;
    FILE *f = fopen(path, "w"); if (!f) return 0;
    int ok = fputs(value, f) >= 0; return !fclose(f) && ok;
}
static void power_tests(const char *root) {
    const char *files[] = {"battery/status","battery/current_now","battery/voltage_now","usb/current_now","usb/voltage_now","usb/online","battery/power_now"};
    static const struct { const char *name, *values[7]; int mw, input; } cases[] = {
        {"discharge", {"Discharging\n","1000000\n","4000000\n"}, 4000, 0},
        {"negative battery current", {"Discharging\n","-1300000\n","4000000\n"}, 5200, 0},
        {"zero bypass current", {"Discharging\n","0\n","4000000\n"}, -1, 0},
        {"charging net current", {"Charging\n","1500000\n","4000000\n"}, -1, 0},
        {"full net current", {"Full\n","1500000\n","4000000\n"}, -1, 0},
        {"not charging net current", {"Not charging\n","1500000\n","4000000\n"}, -1, 0},
        {"unreadable sensors", {NULL}, -1, 0},
        {"missing status", {NULL,"1000000\n","4000000\n"}, -1, 0},
        {"unknown status", {"Unknown\n","1000000\n","4000000\n"}, -1, 0},
        {"rounded zero watts", {"Discharging\n","1000\n","4000000\n"}, -1, 0},
        {"USB charging input", {"Charging\n","0\n","4000000\n","1000000\n","5000000\n","1\n"}, 5000, 1},
        {"USB full without online counter", {"Full\n","0\n","4000000\n","1000000\n","5000000\n"}, 5000, 1},
        {"USB explicitly offline", {"Not charging\n","0\n","4000000\n","1000000\n","5000000\n","0\n"}, -1, 0},
        {"battery power counter fallback", {"Charging\n","0\n","4000000\n","0\n","5000000\n","1\n","5200000\n"}, 5200, 1},
        {"negative power counter", {"Full\n","0\n","4000000\n",NULL,NULL,NULL,"-5200000\n"}, 5200, 1},
        {"zero power counter", {"Not charging\n","0\n","4000000\n",NULL,NULL,NULL,"0\n"}, -1, 0},
        {"discharge counter is not input", {"Discharging\n","0\n","4000000\n",NULL,NULL,NULL,"5200000\n"}, -1, 0},
        {"USB nonfinite", {"Charging\n","0\n","4000000\n","nan\n","5000000\n"}, -1, 0},
        {"USB negative/OTG current", {"Charging\n","0\n","4000000\n","-1000000\n","5000000\n"}, -1, 0},
        {"USB invalid range", {"Charging\n","0\n","4000000\n","1000000000\n","1000000000\n"}, -1, 0},
        {"USB zero voltage", {"Charging\n","0\n","4000000\n","1000000\n","0\n"}, -1, 0},
        {"USB malformed counter", {"Charging\n","0\n","4000000\n","1000000garbage\n","5000000\n"}, -1, 0},
        {"USB with unknown battery status", {NULL,NULL,NULL,"1000000\n","5000000\n","1\n"}, 5000, 1},
        {"prefer USB over fallback", {"Charging\n","0\n","4000000\n","1000000\n","5000000\n","1\n","3000000\n"}, 5000, 1},
        {"discharge prefers battery", {"Discharging\n","1000000\n","4000000\n","1000000\n","5000000\n","1\n"}, 4000, 0},
    };
    if (mkdir(root, 0700)) { CHECK(0, "create fresh power fixture root"); return; }
    char path[512]; snprintf(path, sizeof path, "%s/battery", root); int made = !mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/usb", root); made &= !mkdir(path, 0700);
    CHECK(made, "create fixture supply directories");
    for (unsigned c = 0; made && c < sizeof cases / sizeof *cases; c++) {
        int ready = 1;
        for (unsigned k = 0; k < sizeof files / sizeof *files; k++) ready &= power_file(root, files[k], cases[c].values[k]);
        CHECK(ready, "%s fixture files", cases[c].name);
        vk_overlay_power value = vk_overlay_read_power(root);
        CHECK(value.mw == cases[c].mw && value.input == cases[c].input, "%s: got %d mW input=%d", cases[c].name, value.mw, value.input);
        vk_overlay_panel p = {.power_mw = value.mw, .power_input = value.input,.temperature_mc=INT_MIN};
        vk_overlay_text(&p, 1000000000ull);
        CHECK(value.mw > 0 ? !strncmp(p.power_text, value.input ? "IN " : "PWR ", value.input ? 3 : 4) : !p.power_text[0], "%s label/hiding", cases[c].name);
        if (value.input) {
            vk_overlay_configure(&p,1920,1080,0,1,"tr");
            uint32_t pixels[VK_OVERLAY_W * VK_OVERLAY_MAX_H]; vk_overlay_raster(&p,1,pixels);
            static const uint8_t in_glyphs[2][7]={{14,4,4,4,4,4,14},{17,25,25,21,19,19,17}};
            CHECK(!fixed_glyph_mismatches(pixels,p.layout.width,2,2+p.layout.power_row*VK_OVERLAY_ROW_H,in_glyphs,2),"IN glyphs independently match exact masks");
        }
    }
    static const struct { const char *raw; int expected; } temperatures[]={
        {"387\n",38700},{"-50\n",-5000},{"0\n",0},{"nan\n",INT_MIN},{"9999\n",INT_MIN},{NULL,INT_MIN}};
    for(unsigned i=0;i<sizeof temperatures/sizeof *temperatures;i++){
        CHECK(power_file(root,"battery/temp",temperatures[i].raw),"temperature fixture %u",i);
        CHECK(vk_overlay_read_temperature(root)==temperatures[i].expected,"battery deci-C temperature case %u",i);
    }
    for (unsigned k = 0; k < sizeof files / sizeof *files; k++) power_file(root, files[k], NULL);
    snprintf(path, sizeof path, "%s/battery", root); rmdir(path);
    snprintf(path, sizeof path, "%s/usb", root); rmdir(path);
    CHECK(!rmdir(root), "remove power fixture");
}
static void system_tests(const char *root) {
    char path[512];
    CHECK(!mkdir(root,0700),"create fresh system fixture");
    const char *dirs[]={"self","cpu6","cpu6/cpufreq"};
    for(unsigned i=0;i<3;i++){snprintf(path,sizeof path,"%s/%s",root,dirs[i]);CHECK(!mkdir(path,0700),"system directory %s",dirs[i]);}
    CHECK(power_file(root,"meminfo","MemTotal: 12582912 kB\nMemFree: 7 kB\nMemAvailable: 3145728 kB\n")&&
          power_file(root,"self/status","Name: game\nVmRSS: 6291456 kB\n")&&
          power_file(root,"cpu6/cpufreq/scaling_cur_freq","1500000\n")&&
          power_file(root,"cpu6/cpufreq/cpuinfo_max_freq","2000000\n")&&
          power_file(root,"cpu6/cpufreq/scaling_max_freq","1800000\n"),"write real-format system counters");
    vk_overlay_system s=vk_overlay_read_system(root,root,6);
    CHECK(s.total==12288&&s.available==3072&&s.rss==6144&&s.cpu==7&&s.ratio==76&&s.cap==2,
          "memory uses MemAvailable and MiB; selected CPU ratio and policy cap");
    vk_overlay_panel p={.system=1,.sys=s,.temperature_mc=INT_MIN,.gpu_real_ms=-1};
    p.sys.thermal=1;p.sys.headroom=43;
    vk_overlay_configure(&p,1920,1080,0,0,"tr");vk_overlay_text(&p,1000000000ull);
    CHECK(!strcmp(p.system_text,"THM 0 H0.42 CPU6 0.75 CAP")&&
          !strcmp(p.memory_text,"RAM   3072/ 12288M RSS   6144M"),"system labels: %s / %s",p.system_text,p.memory_text);
    CHECK(p.layout.rows==5&&p.layout.system_row==3&&p.layout.width==TEST_FG_W&&
          p.layout.height==TEST_PLAIN_H+2*VK_OVERLAY_ROW_H,"system layout reserves exactly two rows");
    unsigned w=p.layout.width,h=p.layout.height;
    uint32_t pixels[VK_OVERLAY_W*VK_OVERLAY_MAX_H];vk_overlay_raster(&p,1,pixels);
    static const uint8_t thm[3][7]={{31,4,4,4,4,4,4},{17,17,17,31,17,17,17},{17,27,21,21,17,17,17}};
    CHECK(!fixed_glyph_mismatches(pixels,p.layout.width,2,2+p.layout.system_row*VK_OVERLAY_ROW_H,thm,3),
          "thermal row uses exact existing glyph masks");
    uint64_t updates=p.counters.text_field_updates;
    vk_overlay_text(&p,2000000000ull);
    CHECK(p.counters.text_field_updates==updates,"unchanged system values do not reformat");
    p.sys=(vk_overlay_system){0};vk_overlay_text(&p,3000000000ull);
    CHECK(!p.system_text[0]&&!p.memory_text[0]&&p.layout.width==w&&p.layout.height==h&&
          !vk_overlay_configure(&p,1920,1080,0,0,"tr"),"missing values clear without layout jitter");
    p.sys=(vk_overlay_system){.thermal=8,.headroom=1001,.cpu=1025,.ratio=102,.total=1000000,.available=1000000,.rss=1000000};
    vk_overlay_text(&p,4000000000ull);
    CHECK(!p.system_text[0]&&strstr(p.memory_text,"??????"),"invalid system fields omit; memory overflow visible");
    p.compact=1;vk_overlay_configure(&p,1920,1080,0,1,"tr");
    CHECK(p.layout.rows==1&&p.layout.system_row==UINT_MAX&&p.layout.width==VK_OVERLAY_COMPACT_COLS*6+4,
          "opt-in never expands FPS-only layout");
    p.compact=0;vk_overlay_configure(&p,1920,1080,1,1,"tr");
    CHECK(p.layout.rows==VK_OVERLAY_MAX_ROWS&&p.layout.height==VK_OVERLAY_MAX_H,"FG plus both options fits reserved storage");
    const char *bad[]={"nan\n","0\n","2000001\n","1500000junk\n",NULL};
    for(unsigned i=0;i<5;i++){
        CHECK(power_file(root,"cpu6/cpufreq/scaling_cur_freq",bad[i]),"write bad frequency %u",i);
        s=vk_overlay_read_system(root,root,6);
        CHECK(!s.cpu&&!s.ratio&&s.total==12288,"invalid frequency hides independently %u",i);
    }
    CHECK(power_file(root,"cpu6/cpufreq/scaling_cur_freq","1500000\n")&&
          power_file(root,"cpu6/cpufreq/scaling_max_freq",NULL),"remove policy cap");
    s=vk_overlay_read_system(root,root,6);CHECK(s.ratio==76&&!s.cap,"missing policy cap is unknown, not capped");
    CHECK(power_file(root,"cpu6/cpufreq/scaling_max_freq","2000000\n"),"write uncapped policy");
    s=vk_overlay_read_system(root,root,6);CHECK(s.cap==1,"hardware maximum is not a policy cap");
    CHECK(power_file(root,"meminfo","MemTotal: 1024 kB\nMemAvailable: 2048 kB\n"),"write impossible memory");
    s=vk_overlay_read_system(root,root,6);CHECK(!s.total&&!s.available&&s.rss==6144,"impossible RAM hides independently");
    const char *files[]={"meminfo","self/status","cpu6/cpufreq/scaling_cur_freq","cpu6/cpufreq/scaling_max_freq","cpu6/cpufreq/cpuinfo_max_freq"};
    for(unsigned i=0;i<5;i++)power_file(root,files[i],NULL);
    s=vk_overlay_read_system(root,root,-1);CHECK(!s.cpu&&!s.total&&!s.rss,"unavailable sources omit everything");
    for(unsigned i=3;i;i--){snprintf(path,sizeof path,"%s/%s",root,dirs[i-1]);CHECK(!rmdir(path),"remove system directory");}
    CHECK(!rmdir(root),"remove system fixture");
}
static void benchmark(void) {
    vk_overlay_panel p = {.target_ms = 1000.0 / 30, .power_mw = 5200};
    vk_overlay_configure(&p,1920,1080,1,1,"tr");
    uint64_t t = 1000000000ull; vk_overlay_note(&p, t);
    for (unsigned k = 0; k < 120; k++) vk_overlay_note(&p, t += (20 + k % 70) * 1000000ull);
    vk_overlay_text(&p, t);
    uint32_t *pixels = malloc(VK_OVERLAY_W * VK_OVERLAY_MAX_H * 36 * 4);
    for (unsigned scale = 1; scale <= 6; scale++) {
        for (unsigned k = 0; k < 100; k++) vk_overlay_raster(&p, scale, pixels);
        uint64_t start = now_ns(); unsigned iterations = 10000;
        for (unsigned k = 0; k < iterations; k++) vk_overlay_raster(&p, scale, pixels);
        printf("raster full scale %u (%ux%u): %.6f ms/call (%u iterations; full text+graph+scale, uncached)\n", scale, p.layout.width * scale, p.layout.height * scale, (now_ns() - start) / (double)iterations / 1e6, iterations);
    }
    free(pixels);
}
/* Actual recorder benchmark: one warmed record per submission, cycling reusable slots.
   Query results are read only after completion and never in the CPU interval. */
static int compare_u64(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
static void record_benchmark(VkImage image) {
    enum { SAMPLES=1000, WARMUP=32 };
    VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
    VkQueryPool queries=VK_NULL_HANDLE;
    VKCHECK(vkCreateQueryPool(V.dev,&qi,NULL,&queries));
    uint64_t cpu[SAMPLES],gpu[SAMPLES];
    vk_overlay_target(1.0/60.0);
    for(unsigned n=0;n<SAMPLES+WARMUP;n++) {
        mtl_sub_begin();
        VkCommandBuffer cb=vk_cb();
        vkCmdResetQueryPool(cb,queries,0,2);
        vk_overlay_presented(now_ns(),0);
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,queries,0);
        uint64_t t0=now_ns();
        int recorded=vk_overlay_record(cb,image,VK_IMAGE_LAYOUT_GENERAL,1920,1080,n%VK_OVERLAY_SLOTS,0);
        uint64_t elapsed=now_ns()-t0;
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,queries,1);
        uint64_t fence=mtl_sub_commit();
        CHECK(recorded == (vk_overlay_mode() != 0),"record benchmark sample %u follows selected overlay mode",n);
        CHECK(mtl_wait(fence,5000000000ull),"record benchmark sample %u completes",n);
        uint64_t ticks[2]={0};
        VKCHECK(vkGetQueryPoolResults(V.dev,queries,0,2,sizeof ticks,ticks,sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
        if(n>=WARMUP) { cpu[n-WARMUP]=elapsed; gpu[n-WARMUP]=ticks[1]-ticks[0]; }
    }
    qsort(cpu,SAMPLES,sizeof *cpu,compare_u64);
    qsort(gpu,SAMPLES,sizeof *gpu,compare_u64);
    printf("overlay record benchmark 1920x1080 RGBA (%u warmed records): CPU median %.3f/p95 %.3f us; GPU median %.3f/p95 %.3f us\n",
        SAMPLES,(double)cpu[SAMPLES/2]/1000.0,(double)cpu[(SAMPLES*95)/100]/1000.0,
        (double)gpu[SAMPLES/2]*V.props.limits.timestampPeriod/1000.0,
        (double)gpu[(SAMPLES*95)/100]*V.props.limits.timestampPeriod/1000.0);
    vkDestroyQueryPool(V.dev,queries,NULL);
}
static int render_layout_ppm(const char *path) {
    vk_overlay_panel p={.power_mw=4200,.temperature_mc=38700,.target_ms=1000.0/60,.handheld=1};
    const char *power=getenv("TOTK_OVERLAY_POWER");
    p.system=getenv("TOTK_OVERLAY_SYS")&&!strcmp(getenv("TOTK_OVERLAY_SYS"),"1");
    p.sys=(vk_overlay_system){.thermal=1,.headroom=43,.cpu=7,.ratio=76,.cap=2,.available=3072,.total=12288,.rss=6144};
    if(!vk_overlay_configure(&p,1920,1080,1,power&&!strcmp(power,"1"),"tr"))return 0;
    uint64_t t=1000000000ull;
    for(unsigned i=0;i<240;i++)vk_overlay_note_frame(&p,t+=16666667,i&1);
    p.engine=VK_OVERLAY_FG_QCOM;p.method=VK_OVERLAY_FG_EXTRAP;p.state=VK_OVERLAY_FG_ACTIVE;
    p.gpu_real_ms=23.1;p.gpu_incremental_ms=2.4;p.gpu_budget_ms=33.3;p.gpu_paired=1;
    p.compile_at_ns=t-500000000ull;p.compile_cost_ns=140000000;p.compile_count=3;
    p.output_w=1920;p.output_h=1080;p.render_w=960;p.render_h=540;p.canvas_w=1280;p.canvas_h=720;
    vk_overlay_text(&p,t);
    unsigned w=p.layout.width,h=p.layout.height,scale=3;
    uint32_t *pixels=malloc((size_t)w*h*scale*scale*sizeof *pixels);
    if(!pixels)return 0;
    vk_overlay_raster(&p,scale,pixels);
    FILE *f=fopen(path,"wb");
    if(!f){free(pixels);return 0;}
    fprintf(f,"P6\n%u %u\n255\n",w*scale,h*scale);
    for(unsigned y=0;y<h*scale;y++)for(unsigned x=0;x<w*scale;x++){
        uint32_t c=pixels[(size_t)y*w*scale+x];unsigned char rgb[3]={(unsigned char)c,(unsigned char)(c>>8),(unsigned char)(c>>16)};
        fwrite(rgb,1,sizeof rgb,f);
    }
    int ok=!ferror(f);ok=!fclose(f)&&ok;free(pixels);return ok;
}
static void render_layout_usage(const char *program) {
    fprintf(stderr,"Usage: %s --render-layout <output.ppm>  # pure host compact FG layout fixture\n",program);
}
static int publish_framegen_fixture(const char *name) {
    vk_overlay_fg_engine engine;
    if(!name||!strcmp(name,"camera"))engine=VK_OVERLAY_FG_CAMERA;
    else if(!strcmp(name,"qcom"))engine=VK_OVERLAY_FG_QCOM;
    else if(!strcmp(name,"lsfg"))engine=VK_OVERLAY_FG_LSFG;
    else if(!strcmp(name,"me2"))engine=VK_OVERLAY_FG_ME2;
    else return 0;
    vk_overlay_set_framegen(engine,VK_OVERLAY_FG_INTERP,VK_OVERLAY_FG_ACTIVE,0,engine==VK_OVERLAY_FG_QCOM);
    if(engine==VK_OVERLAY_FG_LSFG)vk_overlay_set_lsfg_model(3,1,1);
    vk_overlay_set_gpu(8.3,3.2,33.3,1);
    vk_overlay_note_compile(now_ns(),12000000);
    return 1;
}
/* Independent source->destination pixel mapping, deliberately not the
   production rectangle helper: compare each GPU image rotated back to a
   CPU identity raster of the exact panel that vk_overlay_record consumed.
   The read-only snapshot avoids assumptions about wall-clock text refresh. */
static size_t rotated_pixel_index(unsigned x, unsigned y, unsigned w, unsigned h, unsigned rotation) {
    switch (rotation) {
    case 1: return (size_t)x*h+h-1-y;
    case 2: return (size_t)(h-1-y)*w+w-1-x;
    case 3: return (size_t)(w-1-x)*h+y;
    default: return (size_t)y*w+x;
    }
}
static uint32_t rotation_background(unsigned x, unsigned y, unsigned w, unsigned h) {
    if (x<8 && y<8) return COLOR(255,0,64);
    if (x>=w-8 && y<8) return COLOR(0,255,128);
    if (x<8 && y>=h-8) return COLOR(64,0,255);
    if (x>=w-8 && y>=h-8) return COLOR(255,192,0);
    return COLOR(32+x%17,64+y%19,96+(x+y)%23);
}
static void rotation_pixel_tests(int expected_mode) {
    static const struct { unsigned w,h; } cases[]={{1280,800},{137,43}};
    static const unsigned turns[]={0,1,2,3,3,2,1,0};
    /* Fill/wrap history using actual monotonically increasing timestamps.
       Both circular graph copy regions must contain data; labels and colors
       remain real recorder outputs, not replacement test rasterization. */
    for (unsigned i=0;i<256;i++) vk_overlay_presented(now_ns(),i&1);
    vk_overlay_set_resolution(123,77,704,320);
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        unsigned w=cases[c].w,h=cases[c].h;
        for (unsigned k=0;k<sizeof turns/sizeof *turns;k++) {
            unsigned rotation=turns[k],nw=rotation&1?h:w,nh=rotation&1?w:h;
            VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
            VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
                .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={nw,nh,1},.mipLevels=1,.arrayLayers=1,
                .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
                .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VKCHECK(vkCreateImage(V.dev,&ci,NULL,&image));
            VkMemoryRequirements requirements; vkGetImageMemoryRequirements(V.dev,image,&requirements);
            int type=vk_pick_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type<0) type=vk_pick_type(requirements.memoryTypeBits,0);
            CHECK(type>=0,"rotation case %u turn %u has an image memory type",c,rotation);
            if (type<0) { vkDestroyImage(V.dev,image,NULL); return; }
            VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize=requirements.size,.memoryTypeIndex=(uint32_t)type};
            VKCHECK(vk_memory_allocate(&ai,&memory,VK_MEMORY_DIAGNOSTIC));
            VKCHECK(vkBindImageMemory(V.dev,image,memory,0));
            mtl_sub_begin(); VkCommandBuffer cb=vk_cb();
            VkImageMemoryBarrier2 barrier={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            VkDependencyInfo dependency={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier};
            vkCmdPipelineBarrier2(cb,&dependency);
            size_t bytes=(size_t)w*h*sizeof(uint32_t);
            VkBuffer upload,readback; VkDeviceSize upload_offset,readback_offset;
            uint32_t *background=vk_transient(bytes,4,&upload,&upload_offset);
            uint32_t *actual=vk_transient(bytes,4,&readback,&readback_offset);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
                background[rotated_pixel_index(x,y,w,h,rotation)]=rotation_background(x,y,w,h);
            VkBufferImageCopy region={.bufferOffset=upload_offset,
                .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={nw,nh,1}};
            vkCmdCopyBufferToImage(cb,upload,image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
            vk_barrier_all(cb);
            vk_overlay_target(k&1?1.0/60:1.0/30);
            vk_overlay_presented(now_ns(),k&1);
            /* Keep the ring write position strictly inside the graph: a whole number of laps since the earlier
               frames (whose count depends on the live-mode steps) would hide one of the two wrapped regions. */
            static vk_overlay_panel before; vk_overlay_test_snapshot(&before);
            if (before.layout.graph_w && before.layout.output_w==w && before.layout.output_h==h &&
                (before.graph_next+1)%before.layout.graph_w==0) vk_overlay_presented(now_ns(),0);
            /* The same fenced staging slot changes orientation, then repeats
               it, before returning to identity: exercise rebuild and reuse. */
            int recorded=vk_overlay_record(cb,image,VK_IMAGE_LAYOUT_GENERAL,w,h,VK_OVERLAY_SLOTS-1,rotation);
            CHECK(recorded==(expected_mode!=0),"rotation case %u turn %u follows overlay mode",c,rotation);
            vk_overlay_panel panel={0};
            if (recorded) vk_overlay_test_snapshot(&panel);
            if (recorded && panel.layout.graph_w) CHECK(panel.graph_count==panel.layout.graph_w && panel.graph_next>0 &&
                panel.graph_next<panel.layout.graph_w,
                "rotation case %u turn %u exercises both wrapped graph copy regions",c,rotation);
            if (recorded && expected_mode==1) CHECK(panel.layout.rows==1 && !panel.layout.graph_w && !strncmp(panel.text,"FPS ",4),
                "rotation case %u turn %u fps mode is the one-row rate panel without a graph",c,rotation);
            vk_barrier_all(cb); region.bufferOffset=readback_offset;
            vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_GENERAL,readback,1,&region);
            uint64_t fence=mtl_sub_commit();
            int complete=mtl_wait(fence,5000000000ull);
            CHECK(complete,"rotation case %u turn %u GPU readback completes",c,rotation);
            if (!complete) return; /* do not destroy potentially in-flight test resources */
            unsigned panel_w=panel.layout.width*panel.layout.rect.scale;
            unsigned panel_h=panel.layout.height*panel.layout.rect.scale;
            uint32_t *identity=recorded?malloc((size_t)panel_w*panel_h*sizeof *identity):NULL;
            CHECK(!recorded || identity,"rotation case %u turn %u identity oracle allocated",c,rotation);
            if (identity) vk_overlay_raster(&panel,panel.layout.rect.scale,identity);
            unsigned ink=0,inside_bad=0,outside_bad=0,first_x=0,first_y=0;
            if (identity) for (size_t i=0;i<(size_t)panel_w*panel_h;i++) ink+=identity[i]==COLOR(224,240,224);
            if (recorded) CHECK(ink>20,"rotation case %u turn %u oracle includes asymmetric text",c,rotation);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
                vk_overlay_rect r=panel.layout.rect;
                int inside=identity && x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
                uint32_t expected=inside?identity[(size_t)(y-r.y)*panel_w+x-r.x]:rotation_background(x,y,w,h);
                if (actual[rotated_pixel_index(x,y,w,h,rotation)]!=expected) {
                    if (!inside_bad && !outside_bad) { first_x=x; first_y=y; }
                    if (inside) inside_bad++; else outside_bad++;
                }
            }
            CHECK(!inside_bad && !outside_bad,
                "rotation case %u pass %u turn %u GPU pixels rotated back match identity (panel=%u background=%u first=%u,%u)",
                c,k,rotation,inside_bad,outside_bad,first_x,first_y);
            free(identity); vkDestroyImage(V.dev,image,NULL); vk_memory_release(memory,VK_MEMORY_DIAGNOSTIC);
        }
    }
}
int main(int argc, char **argv) {
    int bench_record = argc > 1 && !strcmp(argv[1], "--bench-record");
    if (!bench_record) { cpu_tests(); generation_window_tests(); size_policy_tests(); present_shader_contract_tests(); }
    if (argc > 1 && !strcmp(argv[1], "--bench")) { benchmark(); return fails != 0; }
    if (argc == 3 && !strcmp(argv[1], "--power")) {
        power_tests(argv[2]);
        system_tests(argv[2]);
        printf("%s: %d power/CPU checks, %d failed\n",fails ? "FAILED" : "ok",checks,fails);
        return fails != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--render-layout")) {
        if(argc!=3){render_layout_usage(argv[0]);return 2;}
        if(!render_layout_ppm(argv[2])){fprintf(stderr,"Could not write layout image: %s\n",argv[2]);return 2;}
        printf("Wrote compact frame-generation layout fixture: %s\n",argv[2]);return fails != 0;
    }
    if (bench_record) {
        const char *mode=getenv("TOTK_OVERLAY");
        if(!mode)setenv("TOTK_OVERLAY","full",1);
        setenv("TOTK_NATIVE_UI","0",1);
        source_w=output_w=1920; source_h=output_h=1080;
    }
    int extent_diag=argc>1 && !strcmp(argv[1],"--extent-diag");
    guest_output=extent_diag || (argc>1 && !strcmp(argv[1],"--guest-output"));
    if (guest_output) setenv("TOTK_NATIVE_UI","0",1);
    if (extent_diag) setenv("TOTK_FRAMEGEN_DIAG","1",1);
    const char *extent_cache=getenv("TOTK_GUEST_EXTENT_CACHE");
    int cached_extent=!extent_cache || strcmp(extent_cache,"0");
    int capture=argc>1 && !guest_output;
    const char *env_mode = getenv("TOTK_OVERLAY");
    int expected_mode = !env_mode ? (TOTK_DEV_BUILD ? 2 : 0) : (!strcmp(env_mode,"1") || !strcmp(env_mode,"fps")) ? 1 : !strcmp(env_mode,"full") ? 2 : 0;
    CHECK(vk_overlay_mode() == expected_mode, "startup overlay mode matches environment");
    uint64_t published; unsigned stages[2];
    vk_overlay_set_resolution(960, 536, 1280, 720);
    vk_overlay_set_scale_stage(VK_OVERLAY_STAGE_GAME, VK_OVERLAY_SCALE_FSR1);
    vk_overlay_set_scale_stage(VK_OVERLAY_STAGE_HOST, VK_OVERLAY_SCALE_SGSR);
    vk_overlay_test_publications(&published, stages);
    CHECK(vk_overlay_available() && published == (UINT64_C(960) << 48 | UINT64_C(536) << 32 | UINT64_C(1280) << 16 | 720),
          "app builds publish resolution even while off, so a live toggle shows current values at once");
    CHECK(stages[0] == VK_OVERLAY_SCALE_FSR1 && stages[1] == VK_OVERLAY_SCALE_SGSR,
          "both scaling publications follow availability, not the startup mode");
    vk_overlay_set_resolution(0, 0, 0, 0);
    vk_overlay_set_scale_stage(VK_OVERLAY_STAGE_GAME, VK_OVERLAY_SCALE_NONE);
    vk_overlay_set_scale_stage(VK_OVERLAY_STAGE_HOST, VK_OVERLAY_SCALE_NONE);
    const char *fg_fixture=bench_record?getenv("TOTK_OVERLAY_BENCH_FG"):getenv("TOTK_OVERLAY_TEST_FG");
    if(fg_fixture)CHECK(publish_framegen_fixture(fg_fixture),"frame-generation fixture selector is valid");
    CHECK(!strcmp(TOTK_BUILD_KIND, TOTK_DEV_BUILD ? "dev" : "release"), "compiled build-kind label matches policy");
    setenv("TOTK_APP_WIDTH", guest_output ? "guest" : "960", 1); setenv("TOTK_VK_BLACK", "0", 1);
    const char *native = getenv("TOTK_NATIVE_UI");
    native_ui = native && !strcmp(native, "1");
    if (native_ui || guest_output) {
        source_w = output_w = window_w = VK_NATIVE_UI_WIDTH;
        source_h = output_h = window_h = VK_NATIVE_UI_HEIGHT;
    }
    mtl_init();
    const char *aa = getenv("TOTK_POST_AA"), *scale = getenv("TOTK_POST_SCALE");
    int want_aa = aa && !strcmp(aa, "smaa");
    int want_scale = !native_ui && scale && (!strcmp(scale, "fsr") || !strcmp(scale, "sgsr"));
    native_pattern = native_ui && !want_aa; /* SMAA may alter game edges, but never the final overlay. */
    CHECK(vk_native_ui_enabled() == native_ui, "native UI startup state matches exact environment switch");
    CHECK(vk_post_enabled() == (want_aa || want_scale), "startup post chain preserves AA and suppresses native-UI scaling");
    CHECK(vk_post_scale() == want_scale, "startup scaler state ignores stale scaler environment in native UI");
    const size_t bytes = bench_record || native_ui || guest_output ? 16u << 20 : 1u << 20;
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 2;
    mtl_pool_add(memory, bytes);
    pool_t pool = {.mem = memory, .size = bytes, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t texture = {0}; texture.d.target = 1; texture.d.format = 0x25;
    texture.d.width = source_w; texture.d.height = source_h; texture.d.depth = texture.d.levels = 1;
    nvn_tex_layout(&texture.d, &texture.L); texture.mem = memory; texture.pool = &pool; texture.serial = 9001;
    CHECK(texture.L.size <= bytes, "source texture fits fixture memory pool");
    /* Exercise capacity slot 3 directly, even though the default serialized
     * presenter fixture only needs one of its three active buffers at a time.
     * The normal frame loop clears this source again before post/presentation. */
    mtl_sub_begin(); vk_tex *slot_source = vk_tex_get(&texture); VkCommandBuffer slot_cb = vk_cb(); vk_barrier_all(slot_cb);
    VkClearColorValue slot_green = {{0,1,0,1}}; VkImageSubresourceRange slot_range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(slot_cb, vk_tex_image(slot_source), VK_IMAGE_LAYOUT_GENERAL, &slot_green, 1, &slot_range);
    vk_barrier_all(slot_cb);
    vk_overlay_target(1.0 / 30);
    int fourth = vk_overlay_record(slot_cb, vk_tex_image(slot_source), VK_IMAGE_LAYOUT_GENERAL,
                                  source_w, source_h, VK_OVERLAY_SLOTS - 1, 0);
    uint64_t fourth_fence = mtl_sub_commit();
    CHECK(fourth == (expected_mode != 0), "fourth overlay staging slot records work exactly when enabled");
    CHECK(mtl_wait(fourth_fence, 5000000000ull), "fourth overlay staging slot GPU submission completes");
    if (bench_record) {
        record_benchmark(vk_tex_image(slot_source));
        printf("record benchmark complete\n");
        return fails != 0;
    }
    uint64_t window_epoch = vk_window_set(&window, NULL, -1);
    CHECK(vk_window_test_cached_extent() == ((uint64_t)window_w << 32 | window_h),
          "coherent window extent is published before the first frame");
    int posted_all = 1;
    unsigned frames = extent_diag ? 600 : capture ? 125 : guest_output ? 11 : 7, expected_posts=0;
    uint32_t cached_canvas_w = 0, cached_canvas_h = 0;
    unsigned steady_queries = 0;
    for (unsigned n = 1; n <= frames; n++) {
        if (guest_output && n == 2) {
            static struct ANativeWindow retired;
            unsigned queries = atomic_load_explicit(&window_queries, memory_order_relaxed);
            vk_window_resized(&retired, window_epoch);
            vk_window_resized(&window, window_epoch - 1);
            CHECK(atomic_load_explicit(&window_queries, memory_order_relaxed) == queries,
                  "retired pointer/attachment epoch performs no native-window queries");
            unsigned fits = vk_window_test_fit_rebuilds();
            vk_window_resized(&window, window_epoch);
            CHECK(atomic_load_explicit(&window_queries, memory_order_relaxed) == queries + 2 &&
                  vk_window_test_fit_rebuilds() == fits, "same-size lifecycle event queries once but keeps cached fit");
        }
        if ((native_ui || guest_output) && n >= 3 && n <= 7) {
            /* Reattachment must keep the canvas fixed, rebuild only the idle
               output resources, and reset copy geometry even for the same size. */
            int recreate=!guest_output || n!=5;
            if (recreate) {
                vk_window_set(NULL, NULL, -1);
                CHECK(wait_detached(), "frame %u old window references retire before reattachment", n);
            } /* guest frame5 resizes the live window without destroying it */
            window_w = n == 3 ? 2400 : n == 6 ? 0 : VK_NATIVE_UI_WIDTH;
            window_h = n == 4 ? 1200 : n == 6 ? 0 : VK_NATIVE_UI_HEIGHT;
            output_w = n == 3 ? 2400 : VK_NATIVE_UI_WIDTH;
            output_h = n == 4 ? 1200 : VK_NATIVE_UI_HEIGHT;
            if (guest_output) {
                if (n==5) { window_w=1280; window_h=800; }
                output_w=window_w; output_h=window_h;
            }
            if (recreate) {
                uint64_t old_epoch = window_epoch;
                window_epoch = vk_window_set(&window, NULL, n == 3 ? 2 : -1); /* display move/recreate */
                unsigned queries = atomic_load_explicit(&window_queries, memory_order_relaxed);
                vk_window_resized(&window, old_epoch);
                CHECK(atomic_load_explicit(&window_queries, memory_order_relaxed) == queries,
                      "stale callback cannot replace the recreated window's extent");
            } else vk_window_resized(&window, window_epoch);
            CHECK(vk_window_test_cached_extent() == ((uint64_t)window_w << 32 | window_h),
                  "lifecycle publishes a coherent width/height pair");
        }
        if (n >= 5 && n <= 7) {
            /* Minus+R: three live steps visit every mode once and return to the startup mode,
               re-laying out the next present (fps is one row) and checked by the same oracle. */
            int before = vk_overlay_mode(), after = vk_overlay_cycle();
            CHECK(after == (before + 1) % 3 && vk_overlay_mode() == after, "frame %u live overlay step %d -> %d", n, before, after);
        }
        mtl_sub_begin(); vk_tex *x = vk_tex_get(&texture); VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
        if (native_pattern) {
            VkBuffer upload; VkDeviceSize offset;
            uint32_t *pixels = vk_transient((size_t)source_w * source_h * 4, 4, &upload, &offset);
            for (unsigned y = 0; y < source_h; y++) for (unsigned x = 0; x < source_w; x++)
                pixels[(size_t)y * source_w + x] = source_pixel(x,y);
            VkBufferImageCopy copy = {.bufferOffset = offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .imageExtent = {source_w,source_h,1}};
            vkCmdCopyBufferToImage(cb, upload, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        } else {
            VkClearColorValue green = {{0,1,0,1}}; VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &green, 1, &range);
        }
        vk_barrier_all(cb); uint64_t fence = mtl_sub_commit();
        CHECK(mtl_wait(fence, 5000000000ull), "source frame %u completes", n);
        int crop[4] = {0};
        guest_canvas_w = source_w; guest_canvas_h = source_h;
        if (guest_output && n >= 8) {
            guest_canvas_w = n < 10 ? 1280 : 960; guest_canvas_h = n < 10 ? 720 : 536;
            crop[2] = (int)guest_canvas_w; crop[3] = (int)guest_canvas_h;
        }
        unsigned queries_before = atomic_load_explicit(&window_queries, memory_order_relaxed);
        unsigned fits_before = vk_window_test_fit_rebuilds();
        mtl_present(&texture, 1.0 / 30, guest_output && crop[2] ? crop : NULL);
        unsigned fits_after = vk_window_test_fit_rebuilds();
        int ready_guest = guest_output && window_w && window_h;
        unsigned expected_fits = fits_before + (cached_extent && ready_guest &&
            (cached_canvas_w != guest_canvas_w || cached_canvas_h != guest_canvas_h));
        CHECK(fits_after == expected_fits, "frame %u recomputes cached fit only after canvas/crop change (%u -> %u)",
              n, fits_before, fits_after);
        if (ready_guest) { cached_canvas_w = guest_canvas_w; cached_canvas_h = guest_canvas_h; }
        if (guest_output && (!window_w || !window_h)) {
            CHECK(posts==expected_posts,"unready guest window defers without posting");
            unsigned calls = atomic_load_explicit(&window_queries, memory_order_relaxed) - queries_before;
            CHECK(cached_extent ? calls == 0 : calls == 2, "unready frame uses cached extent or legacy producer query (%u calls)", calls);
            continue;
        }
        expected_posts++;
        if (!wait_post(expected_posts)) { posted_all = 0; break; }
        unsigned calls = atomic_load_explicit(&window_queries, memory_order_relaxed) - queries_before;
        steady_queries += calls;
        CHECK(!guest_output || cached_extent ? calls == 0 : calls == 4,
              "frame %u producer + presenter extent queries match selected arm (%u calls; cache=%d)", n, calls, cached_extent);
        check_presented_frame(n);
        if (capture) usleep(33333); /* Fill the screenshot's history and publish measured text. */
    }
    CHECK(posted_all && posts == expected_posts && expected_posts == frames-(guest_output?1u:0u),
        "all ready frames traverse the real presenter (posts %u; capacity %u)",posts,VK_OVERLAY_SLOTS);
    if (posted_all) CHECK(geometry_calls == (native_ui ? 6u : guest_output ? 5u : 1u),
        "front-buffer geometry follows attachments and ignores unready guest windows (%u calls)", geometry_calls);
    CHECK(!guest_output || cached_extent ? steady_queries == 0 : steady_queries == expected_posts * 4,
          "steady-state ANativeWindow queries: %u (cache=%d; posted=%u)", steady_queries, cached_extent, expected_posts);
    vk_window_set(NULL, NULL, -1);
    CHECK(wait_detached(), "all window references retire after final detach");
    if (posted_all) {
        if (capture) {
            FILE *f = fopen(argv[1], "wb"); CHECK(f != NULL, "open rendered image output");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", width, height); for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) fwrite(&window_pixels[(size_t)y * stride + x], 1, 3, f); fclose(f); }
        }
    }
    if (posted_all) rotation_pixel_tests(expected_mode);
    printf("%s: %d checks, %d failed (native_ui=%d mode=%s pos=%s aa=%s scale=%s)\n", fails ? "FAILED" : "ok", checks, fails, native_ui, getenv("TOTK_OVERLAY") ?: "unset", getenv("TOTK_OVERLAY_POS") ?: "tr", aa ?: "unset", scale ?: "unset");
    return fails != 0;
}

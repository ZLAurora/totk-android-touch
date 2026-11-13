/* Host acceptance for the actual backend: deterministic cache miss / held compiler / injected driver failures.
   The dispatch wrappers only control timing/results; successful builds and pixel readback use real Vulkan.
   Run with the vkdraw seed, lavapipe, and LP_NUM_THREADS=0 for ThreadSanitizer.
   --predict also exercises opt-in production lookahead with small command/key/rescan caps.
   --gpl exercises real library recombination and optimized replacement; TOTK_VK_PIPELINE_LIBRARY=1
   without --gpl runs the original failure/backoff/compute/takeover suite with the backend enabled.
   --screen-classify covers aligned scene misses and a persistent 64x64 color/depth capture.
   --screen-classify-pure (or TOTK_SCREEN_CLASSIFY_ONLY) runs just the dependency-free helper cases.
   --target-life checks current-frame and retained 480x268 color/depth contents through real sampled draws.
   TOTK_VK_PIPE_BOOST=1 checks that the bounded graphics join and the compute dispatch join lend their waiter's nice
   to the building worker exactly for that build (through a priority hook: hosts may not lower nice); unset checks
   that joins never touch worker priority.
   TOTK_VK_UNWRITTEN_MASK=1 checks that a fragment shader writing location 0 only leaves the second of two colour
   targets at its clear through colorWriteMask 0; unset checks that the game's write mask is kept. */
#include "vk_screen_classify.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); puts(""); } } while (0)

static void screen_classify_cases(void) {
    static const struct {
        uint32_t w, h, configured_w, configured_h;
        int expected;
        const char *name;
    } cases[] = {
        {1280, 720, 0, 0, 1, "legacy exact"},
        {960, 540, 0, 0, 1, "Duo requested"},
        {864, 486, 960, 540, 1, "Duo reduced requested"},
        {768, 432, 960, 540, 1, "Duo matched-cache extent"},
        {1152, 648, 960, 540, 1, "Thor 90 percent"},
        {64, 36, 0, 0, 1, "legacy small fixture unchanged"},
        {960, 539, 0, 0, 1, "positive legacy delta 16"},
        {960, 541, 0, 0, 1, "negative legacy delta 16"},
        {967, 542, 0, 0, 1, "positive legacy delta 31"},
        {969, 547, 0, 0, 1, "negative legacy delta 31"},
        {960, 538, 0, 0, 0, "strict positive legacy boundary 32"},
        {960, 542, 0, 0, 0, "strict negative legacy boundary 32"},
        {960, 536, 960, 540, 8, "Duo native aligned"},
        {864, 480, 960, 540, 8, "Duo 90 percent aligned"},
        {1032, 576, 960, 540, 8, "Thor 81 percent aligned"},
        {920, 512, 960, 540, 8, "Thor 72 percent aligned"},
        {816, 456, 960, 540, 8, "Thor 64 percent aligned"},
        {728, 408, 960, 540, 1, "Thor 57 percent within legacy tolerance"},
        {960, 528, 0, 0, 16, "16 alignment only"},
        {960, 512, 0, 0, 32, "32 alignment only"},
        {960, 544, 0, 0, 16, "negative delta needs 16"},
        {904, 512, 0, 0, 8, "negative eight-alignment slack"},
        {1008, 560, 0, 0, 8, "inclusive positive eight-alignment boundary"},
        {952, 528, 0, 0, 0, "outside eight slack without 16 divisibility"},
        {960, 504, 0, 0, 0, "not divisible by wider candidates"},
        {960, 480, 0, 0, 0, "outside every true-aspect interval"},
        {959, 536, 960, 540, 0, "width must align"},
        {960, 535, 960, 540, 0, "height must align"},
        {632, 360, 960, 540, 0, "minimum scene width"},
        {640, 352, 960, 540, 0, "minimum scene height"},
        {640, 368, 0, 0, 16, "minimum scene width admitted"},
        {64, 64, 960, 540, 0, "persistent probe"},
        {256, 256, 960, 540, 0, "cube face"},
        {512, 512, 960, 540, 0, "shadow square"},
        {1024, 1024, 1024, 1024, 0, "configured square cannot admit a square"},
        {2048, 2048, 4096, 2048, 0, "large shadow square"},
        {720, 1280, 720, 1280, 0, "portrait target"},
        {0, 360, 960, 540, 0, "zero width"},
        {640, 0, 960, 540, 0, "zero height"},
        {960, 600, 1280, 800, 8, "configured 16:10 aspect"},
        {960, 592, 1280, 800, 16, "configured 16:10 floor-aligned"},
        {1024, 512, 128, 64, 8, "minimum configured height"},
        {1024, 512, 4096, 2048, 8, "maximum configured width"},
        {960, 544, 1280, 728, 8, "configured eight beats true-aspect sixteen"},
        {1024, 512, 0, 0, 0, "configured aspect absent"},
        {1024, 512, 2048, 0, 0, "partial configured height"},
        {1024, 512, 0, 1024, 0, "partial configured width"},
        {1024, 512, 126, 63, 0, "configured height below bound"},
        {1024, 512, 63, 126, 0, "configured width below bound"},
        {1024, 512, 4097, 2048, 0, "configured width above bound"},
        {1024, 512, 4096, 4097, 0, "configured height above bound"},
        {1024, 512, 1024, 1024, 0, "configured square ignored"},
        {1024, 512, 1024, 2048, 0, "configured portrait ignored"},
        {960, 536, UINT32_MAX, UINT32_MAX, 8, "invalid configured aspect preserves true aspect"},
        {UINT32_MAX, UINT32_MAX, 0, 0, 0, "unsigned dimensions do not wrap legacy arithmetic"},
        {UINT32_MAX, 360, UINT32_MAX, 64, 0, "huge invalid configured aspect"},
        {4294967280u, 2415919095u, 0, 0, 1, "large exact aspect uses signed 64-bit products"},
        {4294967280u, 2415919088u, 0, 0, 8, "large true-aspect interval uses signed 64-bit products"},
        {4294967280u, 2147483640u, 4096, 2048, 8, "large configured interval uses signed 64-bit products"}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        int actual = totk_screen_classify(cases[i].w, cases[i].h, cases[i].configured_w, cases[i].configured_h);
        CHECK(actual == cases[i].expected, "%s: %ux%u configured %ux%u gives %d, expected %d",
              cases[i].name, cases[i].w, cases[i].h, cases[i].configured_w, cases[i].configured_h, actual, cases[i].expected);
    }
    /* Sweep exact source rays, then floor both dimensions independently. A smaller reason is
       allowed, but every eligible floor-aligned extent must be recognized without configuration. */
    for (uint32_t alignment = 8; alignment <= 32; alignment *= 2)
        for (uint32_t scale = 40; scale <= 256; scale++) {
            uint32_t w = (16 * scale) & ~(alignment - 1), h = (9 * scale) & ~(alignment - 1);
            if (w < 640 || h < 360) continue;
            int actual = totk_screen_classify(w, h, 0, 0);
            CHECK(actual > 0 && (uint32_t)actual <= alignment, "floor %u of %ux%u gives %d",
                  alignment, 16 * scale, 9 * scale, actual);
        }
    for (uint32_t alignment = 8; alignment <= 32; alignment *= 2)
        for (uint32_t scale = 40; scale <= 256; scale++) {
            uint32_t w = (16 * scale) & ~(alignment - 1), h = (10 * scale) & ~(alignment - 1);
            int actual = totk_screen_classify(w, h, 1280, 800);
            CHECK(actual > 0 && (uint32_t)actual <= alignment, "configured floor %u of %ux%u gives %d",
                  alignment, 16 * scale, 10 * scale, actual);
        }
}

#ifndef TOTK_SCREEN_CLASSIFY_ONLY
#include "nvn_dirty.h"
#include "vk_int.h"
#include "vk_record.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include "dm_wait_stats.h"
#include "vk_upload_pool.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t walk;
static unsigned held_mask = 15, entered, release_build, failures_left, attempts[16], main_builds[16], completed[16], compute_calls;
static unsigned two_target_builds, second_write_mask;   /* monolithic builds with two attachments; attachment 1's mask */
static unsigned compute_hold, compute_entered, compute_recording;
extern void vk_compute_test_counts(uint64_t out[4]);
static unsigned failure_mask = 7;
static PFN_vkCreateGraphicsPipelines real_graphics;
static PFN_vkCreateComputePipelines real_compute;
/* Priority hook: every lend (worker tid, waiter nice) and restore (0 = the worker itself, VK_WORKER_NICE) in order. */
extern void vk_pipe_boost_test_hooks(int (*self_nice)(void), int (*set)(int tid, int nice));
extern void vk_pipe_boost_test_stats(uint64_t out[4]);
enum { WAITER_NICE = -8 };
static struct { int tid, nice, caller; } boost_calls[16];
static unsigned nboost, held_tid, compute_tid;
static int waiter_nice(void) { return WAITER_NICE; }
static int boost_hook(int tid, int nice) {
    pthread_mutex_lock(&mu);
    if (nboost < 16) boost_calls[nboost] = (typeof(boost_calls[0])){tid, nice, (int)syscall(SYS_gettid)};
    nboost++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return 0;
}
/* The loan to worker `tid` and that worker's own restore, both recorded after `from`. */
static int boost_loan_seen(unsigned from, unsigned tid) {
    pthread_mutex_lock(&mu); int lent = -1, restored = 0;
    for (unsigned i = from; i < nboost && i < 16; i++) {
        if (lent < 0 && boost_calls[i].tid == (int)tid && boost_calls[i].nice == WAITER_NICE) lent = (int)i;
        else if (lent >= 0 && !boost_calls[i].tid && boost_calls[i].nice == VK_WORKER_NICE && boost_calls[i].caller == (int)tid) restored = 1;
    }
    pthread_mutex_unlock(&mu); return tid && lent >= 0 && restored;
}
static unsigned graphics_count(const unsigned *counter) {
    pthread_mutex_lock(&mu); unsigned n = *counter; pthread_mutex_unlock(&mu); return n;
}
static int wait_graphics(const unsigned *counter, unsigned minimum) {
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 5;
    pthread_mutex_lock(&mu);
    while (*counter < minimum)
        if (pthread_cond_timedwait(&cv, &mu, &deadline)) break;
    int ready = *counter >= minimum; pthread_mutex_unlock(&mu); return ready;
}
static void hold_graphics(unsigned mask) {
    pthread_mutex_lock(&mu); held_mask = mask; entered = release_build = 0; pthread_mutex_unlock(&mu);
}
static void release_graphics(void) {
    pthread_mutex_lock(&mu); release_build = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
}
static int library_creation(const VkGraphicsPipelineCreateInfo *ci) {
    if (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) return 1;
    for (const VkBaseInStructure *next = ci->pNext; next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) return 1;
    return 0;
}
static VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    /* Parts may omit blend state; executable links need no local state at all. Neither is a monolithic build. */
    if (n != 1 || library_creation(ci)) return real_graphics(dev, cache, n, ci, alloc, out);
    if (ci->flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) { *out = VK_NULL_HANDLE; return VK_PIPELINE_COMPILE_REQUIRED; }
    if (!ci->pColorBlendState || !ci->pColorBlendState->attachmentCount || !ci->pColorBlendState->pAttachments)
        return real_graphics(dev, cache, n, ci, alloc, out);
    unsigned mask = ci->pColorBlendState->pAttachments[0].colorWriteMask;
    if (mask >= 16) return real_graphics(dev, cache, n, ci, alloc, out);
    pthread_mutex_lock(&mu);
    attempts[mask]++;
    main_builds[mask] += pthread_equal(pthread_self(), walk) != 0;
    if (ci->pColorBlendState->attachmentCount >= 2) { two_target_builds++; second_write_mask = ci->pColorBlendState->pAttachments[1].colorWriteMask; }
    if (mask == held_mask && !pthread_equal(pthread_self(), walk)) {
        entered = 1; held_tid = (unsigned)syscall(SYS_gettid); pthread_cond_broadcast(&cv);
        while (!release_build) pthread_cond_wait(&cv, &mu);
    }
    int fail = mask == failure_mask && failures_left;
    if (fail) failures_left--;
    pthread_mutex_unlock(&mu);
    VkResult r;
    if (fail) { *out = VK_NULL_HANDLE; r = VK_ERROR_OUT_OF_HOST_MEMORY; }
    else r = real_graphics(dev, cache, n, ci, alloc, out);
    pthread_mutex_lock(&mu); completed[mask]++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out) {
    pthread_mutex_lock(&mu);
    compute_calls++; compute_recording += pthread_equal(pthread_self(), walk) != 0;
    if (!pthread_equal(pthread_self(), walk)) compute_tid = (unsigned)syscall(SYS_gettid);
    compute_entered = 1; pthread_cond_broadcast(&cv);
    while (compute_hold) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    VkResult r = real_compute(dev, cache, n, ci, alloc, out);
    return r;
}
static void *release_compute_waiter(void *unused) {
    (void)unused; uint64_t counts[4] = {0};
    for (unsigned i = 0; i < 50000; i++) {
        vk_compute_test_counts(counts); if (counts[3]) break; usleep(100);
    }
    pthread_mutex_lock(&mu); compute_hold = 0; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return (void *)(uintptr_t)(counts[3] != 0);
}
static void *release_at_bound(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 50000 && !atomic_load_explicit(&g_hk.pipe_bounds, memory_order_relaxed); i++) usleep(100);
    pthread_mutex_lock(&mu); release_build = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return NULL;
}
static void *release_for_takeover(void *arg) {
    uint64_t before = *(const uint64_t *)arg;
    for (unsigned i = 0; i < 50000 && atomic_load_explicit(&g_hk.pipes_sync, memory_order_relaxed) <= before; i++) usleep(100);
    pthread_mutex_lock(&mu); release_build = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return NULL;
}
static uint8_t ops[1 << 16]; static size_t used;
static void op(uint32_t code, const void *data, size_t n) {
    op_hdr h = {code, (uint32_t)((8 + n + 7) & ~7u)};
    memcpy(ops + used, &h, 8); if (n) memcpy(ops + used + 8, data, n);
    memset(ops + used + 8 + n, 0, h.size - 8 - n); used += h.size;
}
static uint64_t submit(void) { op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(ops); used = 0; return mtl_sub_commit(); }
static void shader_binary(int stage, int salt, uint8_t *control, uint8_t *code) {
    memset(control, 0, 0x880); uint32_t size = 0x230, csize = 0x880, st = (uint32_t)stage;
    memset(code, salt, 0x30); for (unsigned i = 0; i < 0x200; i++) code[0x30 + i] = (uint8_t)(i * 7 + salt);
    memcpy(control + 0x20, &csize, 4); memcpy(control + 0x700, &size, 4); memcpy(control + 0x704, &size, 4); memcpy(control + 0x714, &st, 4);
}
static p_set_rt rt_bind; static p_bind_program program; static p_bind_vattr attrs; static p_bind_vstream streams;
static p_bind_vbuf vertices; static p_bind_buf uniforms[2]; static p_bind_handles texture;
static p_viewports viewport; static p_copy_b2t readback;
static void draw_topology(unsigned mask, unsigned count, unsigned topology) {
    op(OP_SET_RT, &rt_bind, sizeof rt_bind);
    p_clear_color clear = {0, 15, {0, 0, 1, 1}}; op(OP_CLEAR_COLOR, &clear, sizeof clear);
    if (rt_bind.depth) {
        p_clear_ds depth = {0.75f, 1, 0, 0}; op(OP_CLEAR_DS, &depth, sizeof depth);
    }
    op(OP_BIND_PROGRAM, &program, sizeof program); op(OP_BIND_VATTR, &attrs, sizeof attrs); op(OP_BIND_VSTREAM, &streams, sizeof streams);
    op(OP_BIND_VBUF, &vertices, sizeof vertices); op(OP_BIND_UBO, &uniforms[0], sizeof uniforms[0]); op(OP_BIND_UBO, &uniforms[1], sizeof uniforms[1]);
    op(OP_BIND_TEX, &texture, sizeof texture); op(OP_VIEWPORTS, &viewport, sizeof viewport);
    chmask_st channels = {mask}; p_bind_state state = {.kind = 2}; memcpy(state.bytes, &channels, sizeof channels); op(OP_BIND_STATE, &state, sizeof state);
    p_draw d = {topology, 0, 3, 0, 0}; for (unsigned i = 0; i < count; i++) op(OP_DRAW, &d, sizeof d);
    op(OP_COPY_T2B, &readback, sizeof readback);
    CHECK(mtl_wait(submit(), 5000000000ull), "draw submission completed");
}
static void draw(unsigned mask, unsigned count) { draw_topology(mask, count, 4); }
/* Reuse the exact walking state/signatures: this must hit the recorder's program memo while pipe_get
   still observes asynchronous publication. Pipeline handles are intentionally not cached in that memo. */
static void draw_current(unsigned count) {
    p_draw d = {4, 0, 3, 0, 0}; for (unsigned i = 0; i < count; i++) op(OP_DRAW, &d, sizeof d);
    op(OP_COPY_T2B, &readback, sizeof readback);
    CHECK(mtl_wait(submit(), 5000000000ull), "unchanged-state draw submission completed");
}
static void *release_prediction(void *unused) {
    (void)unused; usleep(20000);
    pthread_mutex_lock(&mu); release_build = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return NULL;
}
static void screen_target(tex_t *color, const tex_t *depth, uint8_t *out) {
    rt_bind.color[0] = color; rt_bind.depth = depth;
    viewport.v[0][2] = color->d.width; viewport.v[0][3] = color->d.height;
    readback.tex = color; readback.src = (uintptr_t)out;
    readback.r = (NVNcopyRegion){0, 0, 0, color->d.width, color->d.height, 1};
}
static void screen_depth_state(int enable) {
    ds_st ds = {.dt = enable != 0, .dw = enable != 0, .df = 8};
    p_bind_state state = {.kind = 5}; memcpy(state.bytes, &ds, sizeof ds);
    op(OP_BIND_STATE, &state, sizeof state);
}
static void screen_capture_pixels(const tex_t *color, const tex_t *depth, uint8_t *out,
        unsigned mask, const char *phase) {
    /* Read both real attachments again; no clear, draw, or pipeline lookup is permitted here. */
    memset(out, 0x5a, 64 * 64 * 4); memset(out + 0x10000, 0x5a, 64 * 64 * 4);
    p_copy_b2t cp = {.src = (uintptr_t)out, .tex = color, .r = {0, 0, 0, 64, 64, 1}};
    op(OP_COPY_T2B, &cp, sizeof cp);
    cp.src += 0x10000; cp.tex = depth; op(OP_COPY_T2B, &cp, sizeof cp);
    CHECK(mtl_wait(submit(), 5000000000ull), "%s capture-only readback completes", phase);
    unsigned bad_color = 0, bad_depth = 0;
    for (unsigned i = 0; i < 64 * 64; i++) {
        const uint8_t *p = out + i * 4; float z; memcpy(&z, out + 0x10000 + i * 4, sizeof z);
        bad_color += p[0] != ((mask & 1) ? 255 : 0) || p[1] != ((mask & 2) ? 255 : 0) || p[2] != 255 || p[3] != 255;
        bad_depth += z != 0.0f;   /* the seed's vertex shader writes clip z = 0 */
    }
    CHECK(!bad_color, "%s retains all RGBA8 capture pixels (%u wrong)", phase, bad_color);
    CHECK(!bad_depth, "%s retains real D32 draw depth, not the 0.75 clear (%u wrong)", phase, bad_depth);
}
static void *release_capture_waiter(void *unused) {
    (void)unused; usleep(100000); release_graphics(); return NULL;
}
static void screen_classify_probe(const tex_t *base, uint8_t *mem) {
    /* RGBA8 + D32 is deliberate: this tests the capture policy/lifetime and real depth writes,
       not B10G11R11 packing. Dimensions never enter the PSO key, so masks must be distinct. */
    /* The shared VS selects off[component] / off[component + 1], where
     * component = ((floatBitsToUint(pos.x) * 48) >> 2) & 3 must fold to zero.
     * Consume distinct, nonzero UBO lanes and compensate the geometry so the
     * exact full-target pixels below still apply. A wrong lane cannot hide
     * behind an all-zero offset vector: z/w would move the triangle away. */
    const float offsets[] = {.25f, -.25f, 4, 8};
    const float full[] = {-1.25f, -.75f, 2.75f, -.75f, -1.25f, 3.25f};
    memcpy(mem + 0x40000, offsets, sizeof offsets);
    memcpy(mem + 0x30000, full, sizeof full);
    tex_t capture = *base, capture_depth = *base, scene = *base, scene_depth = *base;
    capture.mem = mem + 0x70000; capture.serial = 200;
    capture_depth.mem = mem + 0x80000; capture_depth.serial = 201; capture_depth.d.format = 0x34;
    nvn_tex_layout(&capture_depth.d, &capture_depth.L);
    scene.mem = mem + 0x100000; scene.serial = 1000;
    scene_depth.mem = mem + 0x500000; scene_depth.serial = 2000; scene_depth.d.format = 0x34;
    uint8_t *capture_out = mem + 0xc00000, *scene_out = mem + 0x800000;
    uint64_t sync = atomic_load(&g_hk.pipes_sync), skipped = atomic_load(&g_hk.pipe_skips);
    screen_target(&capture, &capture_depth, capture_out); screen_depth_state(1);
    draw(15, 1);
    CHECK(graphics_count(&attempts[15]) == 1 && graphics_count(&main_builds[15]) == 1 &&
          atomic_load(&g_hk.pipes_sync) == sync + 1 && atomic_load(&g_hk.pipe_skips) == skipped,
          "cold 64x64 color/depth capture builds once synchronously and never skips first demand");
    screen_capture_pixels(&capture, &capture_depth, capture_out, 15, "first demand");

    static const struct { uint32_t w, h; unsigned mask; int reason; } extents[] = {
        {960, 536, 1, 8}, {864, 480, 2, 8}, {768, 432, 3, 1},
        {1152, 648, 5, 1}, {1032, 576, 6, 8}, {920, 512, 7, 8},
        {816, 456, 9, 8}, {728, 408, 10, 1}, {960, 528, 11, 16}, {960, 512, 13, 32}
    };
    screen_depth_state(0);
    for (unsigned i = 0; i < sizeof extents / sizeof *extents; i++) {
        uint32_t w = extents[i].w, h = extents[i].h; unsigned mask = extents[i].mask;
        scene.d.width = w; scene.d.height = h; scene.serial++;
        nvn_tex_layout(&scene.d, &scene.L); screen_target(&scene, NULL, scene_out);
        CHECK(totk_screen_classify(w, h, 960, 540) == extents[i].reason,
              "%ux%u has the expected classifier reason %d", w, h, extents[i].reason);
        CHECK(!graphics_count(&attempts[mask]), "%ux%u mask %u is a never-demanded complete key", w, h, mask);
        sync = atomic_load(&g_hk.pipes_sync); skipped = atomic_load(&g_hk.pipe_skips);
        uint64_t bounds = atomic_load(&g_hk.pipe_bounds);
        hold_graphics(mask); draw(mask, 1);
        int held = wait_graphics(&entered, 1);
        CHECK(held, "%ux%u returns from cold demand while its worker is held", w, h);
        const uint8_t *pixel = scene_out + (10 * w + 10) * 4;
        CHECK(!pixel[0] && !pixel[1] && pixel[2] == 255, "%ux%u first draw skips, preserving the blue clear", w, h);
        CHECK(graphics_count(&attempts[mask]) == 1 && !graphics_count(&main_builds[mask]) &&
              atomic_load(&g_hk.pipes_sync) == sync && atomic_load(&g_hk.pipe_skips) == skipped + 1 &&
              atomic_load(&g_hk.pipe_bounds) == bounds, "%ux%u cold miss queues instead of capture-sync or a bound join", w, h);
        release_graphics();
        CHECK(wait_graphics(&completed[mask], 1), "%ux%u released worker finishes", w, h);
        /* Completion precedes publication; two later demands also cover the bounded join race,
           after release, so a broken classifier cannot strand this fixture behind its own gate. */
        draw_current(1); draw_current(1);
        CHECK(pixel[0] == ((mask & 1) ? 255 : 0) && pixel[1] == ((mask & 2) ? 255 : 0) &&
              pixel[2] == 255 && pixel[3] == 255, "%ux%u later demands produce the requested real pixels", w, h);
        CHECK(graphics_count(&attempts[mask]) == 1 && !graphics_count(&main_builds[mask]) &&
              atomic_load(&g_hk.pipes_sync) == sync, "%ux%u recovery never duplicates or takes over the worker build", w, h);
    }
    screen_capture_pixels(&capture, &capture_depth, capture_out, 15, "after all scene recoveries without redraw");
    CHECK(graphics_count(&attempts[15]) == 1, "persistent capture readback never redraws or rebuilds its pipeline");

    /* Start one more cold scene key with the SAME formats as the capture, then change only
       attachment dimensions. A memo hit must reclassify the square and join the held worker. */
    scene.d.width = scene_depth.d.width = 960; scene.d.height = scene_depth.d.height = 536;
    scene.serial++; scene_depth.serial++;
    nvn_tex_layout(&scene.d, &scene.L); nvn_tex_layout(&scene_depth.d, &scene_depth.L);
    screen_target(&scene, &scene_depth, scene_out); screen_depth_state(1);
    sync = atomic_load(&g_hk.pipes_sync); skipped = atomic_load(&g_hk.pipe_skips);
    CHECK(!graphics_count(&attempts[14]), "pending screen-to-capture transition uses a fresh complete key");
    hold_graphics(14); draw(14, 1);
    int held = wait_graphics(&entered, 1);
    CHECK(held && !graphics_count(&main_builds[14]) && atomic_load(&g_hk.pipe_skips) == skipped + 1,
          "depth-attached 960x536 demand queues the held worker before capture transition");
    if (!held) { release_graphics(); return; }
    screen_target(&capture, &capture_depth, capture_out);
    pthread_t helper; int started = pthread_create(&helper, NULL, release_capture_waiter, NULL);
    CHECK(!started, "capture join release helper starts");
    if (started) release_graphics();
    skipped = atomic_load(&g_hk.pipe_skips);
    draw(14, 1);
    CHECK(graphics_count(&release_build), "64x64 pending-key demand does not return before worker release");
    if (!started) pthread_join(helper, NULL);
    CHECK(wait_graphics(&completed[14], 1) && graphics_count(&attempts[14]) == 1 &&
          !graphics_count(&main_builds[14]) && atomic_load(&g_hk.pipes_sync) == sync &&
          atomic_load(&g_hk.pipe_skips) == skipped,
          "same-key capture joins exactly one existing worker instead of skipping or compiling a duplicate");
    screen_capture_pixels(&capture, &capture_depth, capture_out, 14, "pending-key first capture demand");
    screen_target(&scene, &scene_depth, scene_out); draw(14, 1);
    const uint8_t *pixel = scene_out + (10 * 960 + 10) * 4;
    CHECK(!pixel[0] && pixel[1] == 255 && pixel[2] == 255, "recovered same-key scene draws after the capture join");
    screen_capture_pixels(&capture, &capture_depth, capture_out, 14, "after same-key scene recovery without redraw");
    puts("screen-classify scope: real RGBA8 + D32 capture; packed B10G11R11 readback is not exercised");
}
static void target_life_pixels(const uint8_t *out, uint32_t w, uint32_t h,
        unsigned mask, unsigned frame, const char *phase) {
    unsigned bad = 0;
    for (uint32_t i = 0; i < w * h; i++) {
        const uint8_t *p = out + i * 4;
        bad += p[0] != ((mask & 1) ? 255 : 0) || p[1] != ((mask & 2) ? 255 : 0) ||
               p[2] != 255 || p[3] != 255;
    }
    CHECK(!bad, "%s frame %u checks all %ux%u RGBA8 pixels (%u wrong)", phase, frame, w, h, bad);
}
static void target_life_capture(const tex_t *root, const tex_t *depth, uint8_t *out,
        uint8_t *depth_out, unsigned mask, unsigned frame, const char *phase) {
    /* Copy-only observations must not silently establish another root writer. */
    size_t bytes = (size_t)root->d.width * root->d.height * 4;
    memset(out, 0x5a, bytes); memset(depth_out, 0x5a, bytes);
    p_copy_b2t cp = {.src = (uintptr_t)out, .tex = root,
                    .r = {0, 0, 0, root->d.width, root->d.height, 1}};
    op(OP_COPY_T2B, &cp, sizeof cp);
    cp.src = (uintptr_t)depth_out; cp.tex = depth; op(OP_COPY_T2B, &cp, sizeof cp);
    CHECK(mtl_wait(submit(), 5000000000ull), "%s frame %u copy-only root readback completes", phase, frame);
    target_life_pixels(out, root->d.width, root->d.height, mask, frame, phase);
    unsigned bad = 0;
    for (uint32_t i = 0; i < root->d.width * root->d.height; i++) {
        float z; memcpy(&z, depth_out + i * 4, sizeof z); bad += z != 0.0f;
    }
    CHECK(!bad, "%s frame %u retains every D32 draw value, not the 0.75 clear (%u wrong)", phase, frame, bad);
}
static void target_life_write(tex_t *root, const tex_t *depth, uint8_t *out,
        unsigned mask, unsigned frame) {
    screen_target(root, depth, out); screen_depth_state(1);
    texture.h[0] = 1 | (1u << 20);   /* producer samples only the original white 64x64 texture */
    vk_record_test_counts before, after; vk_record_test_counts_get(&before);
    draw(mask, 1); vk_record_test_counts_get(&after);
    CHECK(after.draws == before.draws + 1, "root frame %u executes its real color/depth draw", frame);
    target_life_pixels(out, root->d.width, root->d.height, mask, frame, "root write");
}
static void target_life_consume(tex_t *consumer, uint8_t *out, unsigned tid,
        unsigned mask, unsigned frame, unsigned writer_frame, const char *phase) {
    screen_target(consumer, NULL, out); screen_depth_state(0);
    texture.h[0] = tid | (1u << 20);
    vk_record_test_counts before, after; vk_record_test_counts_get(&before);
    /* One submission/pass, three unchanged draws: the later descriptors must reuse the
       recorder freeze memo. Every draw runs lifetime enumeration, while identical sampled
       reads in this physical pass intentionally collapse to one production record. */
    draw(15, 3); vk_record_test_counts_get(&after);
    CHECK(after.draws == before.draws + 3, "%s frame %u executes all three ready consumer draws", phase, frame);
    CHECK(after.freeze_hits > before.freeze_hits, "%s frame %u reuses frozen descriptors", phase, frame);
    CHECK(after.program_hits > before.program_hits, "%s frame %u reuses the program memo", phase, frame);
    target_life_pixels(out, consumer->d.width, consumer->d.height, mask, frame, phase);
    printf("[pso-life-fixture] case=%s frame=%u writer_frame=%u expected_reads=1 accepted_draws=%llu freeze_hits=%llu\n",
           phase, frame, writer_frame, (unsigned long long)(after.draws - before.draws),
           (unsigned long long)(after.freeze_hits - before.freeze_hits));
}
static void target_life_probe(const tex_t *base, uint8_t *mem) {
    tex_t root = *base, depth = *base, consumer = *base;
    root.d.width = depth.d.width = 480; root.d.height = depth.d.height = 268;
    root.mem = mem + 0x100000; root.serial = 4000;
    depth.mem = mem + 0x200000; depth.serial = 4001; depth.d.format = 0x34;
    consumer.d.width = 960; consumer.d.height = 536;
    consumer.mem = mem + 0x400000; consumer.serial = 4002;
    nvn_tex_layout(&root.d, &root.L); nvn_tex_layout(&depth.d, &depth.L);
    nvn_tex_layout(&consumer.d, &consumer.L);
    CHECK(root.L.size <= 0x100000 && depth.L.size <= 0x100000 && consumer.L.size <= 0x400000,
          "lifetime root/depth/consumer fit their separate real texture storage");
    if (root.L.size > 0x100000 || depth.L.size > 0x100000 || consumer.L.size > 0x400000) return;
    uint8_t *consumer_out = mem + 0x800000, *root_out = mem + 0xb00000, *depth_out = mem + 0xc00000;
    CHECK(vk_record_enabled(), "lifetime fixture exercises the real recorder descriptor freeze memo");
    release_graphics();   /* this arm needs real readiness, not the failure suite's held mask-15 compiler */
    mtl_reg_tex(2, &root, NULL);
    puts("[pso-life-fixture] case=warmup frame=1");
    target_life_write(&root, &depth, root_out, 1, 1);
    screen_target(&consumer, NULL, consumer_out); screen_depth_state(0); texture.h[0] = 2 | (1u << 20);
    draw(15, 1);
    CHECK(wait_graphics(&completed[15], 1), "lifetime consumer's asynchronous pipeline creation completes");
    /* Driver completion precedes publication. Demand without rebinding until actual accepted
       draws prove readiness; no frame boundary can turn a warmup miss into missing history. */
    int ready = 0;
    for (unsigned i = 0; i < 8 && !ready; i++) {
        vk_record_test_counts before, after; vk_record_test_counts_get(&before);
        draw_current(3); vk_record_test_counts_get(&after);
        ready = after.draws == before.draws + 3;
    }
    CHECK(ready, "lifetime consumer is published before the known current/history stages");
    target_life_pixels(consumer_out, consumer.d.width, consumer.d.height, 1, 1, "ready warmup consumer");
    target_life_capture(&root, &depth, root_out, depth_out, 1, 1, "warmup root");
    mtl_present(NULL, 1.0 / 60, NULL);

    for (unsigned frame = 2; frame <= 4; frame++) {
        unsigned mask = frame == 3 ? 2 : 1;
        target_life_write(&root, &depth, root_out, mask, frame);
        target_life_consume(&consumer, consumer_out, 2, mask, frame, frame, "current");
        target_life_capture(&root, &depth, root_out, depth_out, mask, frame, "current root");
        mtl_present(NULL, 1.0 / 60, NULL);
    }
    /* Frame 5 consumes frame 4 before any new root write. Neither its color nor depth is
       rebound as an attachment, cleared, redrawn, or replaced by a guest upload. */
    target_life_consume(&consumer, consumer_out, 2, 1, 5, 4, "history");
    target_life_capture(&root, &depth, root_out, depth_out, 1, 5, "history retention");
    mtl_present(NULL, 1.0 / 60, NULL);

    /* A new registry serial is metadata, not a new image lifetime. Prove storage sharing
       and retained pixels, then contrast the pre-write history and post-write producer. */
    tex_t alias = root; alias.serial = 4003; mtl_reg_tex(3, &alias, NULL);
    VkImage image = vk_tex_image(vk_tex_get(&root));
    CHECK(image && vk_tex_image(vk_tex_get(&alias)) == image,
          "new-serial root alias retains the exact underlying Vulkan image");
    target_life_consume(&consumer, consumer_out, 3, 1, 6, 4, "history-alias");
    target_life_capture(&root, &depth, root_out, depth_out, 1, 6, "alias history retention");
    target_life_write(&root, &depth, root_out, 2, 6);
    target_life_consume(&consumer, consumer_out, 3, 2, 6, 6, "current-alias");
    target_life_capture(&root, &depth, root_out, depth_out, 2, 6, "rewritten alias root");
    mtl_present(NULL, 1.0 / 60, NULL);
    /* Explicit empty boundaries close the last observed frame; finish_tests also crosses
       the configured 20-frame diagnostic limit and exercises normal fixture teardown. */
    mtl_present(NULL, 1.0 / 60, NULL); mtl_present(NULL, 1.0 / 60, NULL);
}
static void prediction_probe(const tex_t *texture_object, uint8_t *pixel) {
    mtl_present(NULL, 1.0 / 60, NULL);
    vk_pipeline_test_stats before, first, second, after;
    vk_pipeline_test_stats_get(&before);
    CHECK(!before.used_ns && !before.sync_ns, "present resets both scan spending and synchronous build credit");
    p_draw d = {4, 0, 3, 0, 0};
    chmask_st channels = {15}; p_bind_state state = {.kind = 2}; memcpy(state.bytes, &channels, sizeof channels);
    op(OP_BIND_STATE, &state, sizeof state); op(OP_DRAW, &d, sizeof d); op(OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(ops); used = 0;   /* leave the warm pass open for the explicit prediction cursor */
    vk_state saved = S, shadow = S;
    vk_shader *active[6], *predicted[6];
    CHECK(!vk_prog_resolve(S.prog, active) && vk_prog_predict(&shadow, predicted) &&
          !memcmp(active, predicted, sizeof active), "prediction borrows the exact ready shader stages");
    uint64_t io = vk_shader_io_cum_ns();
    prog_t future = {.serial = UINT64_MAX - 17}; shadow.prog = &future;
    CHECK(!vk_prog_predict(&shadow, predicted), "an unknown future program is not loaded by prediction");
    shadow = saved; shadow.attr[0].fmt = 0x27;
    CHECK(!vk_prog_predict(&shadow, predicted), "an unresolved signature is not selected or loaded speculatively");
    tex_t replacement = *texture_object; replacement.d.format = 0x32; replacement.serial += 100;
    mtl_reg_tex(1, &replacement, NULL); shadow = saved;
    CHECK(!vk_prog_predict(&shadow, predicted), "registry replacement invalidates a previously ready stage signature");
    mtl_reg_tex(1, texture_object, NULL);
    CHECK(vk_prog_predict(&shadow, predicted) && !memcmp(active, predicted, sizeof active),
          "restoring the registry reuses the authoritative ready variants");
    CHECK(vk_shader_io_cum_ns() == io && !memcmp(&S, &saved, sizeof S),
          "prediction misses cause no seed I/O or live binding changes");

    pthread_mutex_lock(&mu); held_mask = 2; entered = release_build = 0; pthread_mutex_unlock(&mu);
    p_scissor scissor = {101, 202, 303, 404}; op(OP_SCISSOR, &scissor, sizeof scissor);
    const unsigned masks[] = {2, 4, 8};
    for (unsigned i = 0; i < 3; i++) {
        channels.mask = masks[i]; memcpy(state.bytes, &channels, sizeof channels);
        op(OP_BIND_STATE, &state, sizeof state); op(OP_DRAW, &d, sizeof d);
    }
    for (unsigned i = 0; i < 32; i++) op(OP_BEGIN, NULL, 0);
    op(OP_END, NULL, 0);
    vk_pipeline_test_predict(ops); vk_pipeline_test_stats_get(&first);
    CHECK(first.keys == 2 && first.stop == 2 && first.ops <= 16, "production prediction stops at its two-key cap");
    if (first.keys != 2) {   /* report a failed prerequisite without leaving a held compiler or open submission */
        pthread_mutex_lock(&mu); release_build = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
        used = 0; CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "failed prediction fixture drains"); return;
    }
    pthread_mutex_lock(&mu); while (!entered) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    vk_pipeline_test_predict(ops); vk_pipeline_test_stats_get(&second);
    CHECK(second.scans == first.scans + 1 && second.keys == 1 && second.ops == 16 && second.stop == 1,
          "a productive rescan deduplicates pending keys and stops at its command cap");
    vk_pipeline_test_predict(ops); vk_pipeline_test_stats_get(&after); used = 0;
    CHECK(after.scans == second.scans && after.used_ns == second.used_ns && after.scans_in_sub == 2,
          "first scan plus one rescan is a hard submission cap");
    CHECK(!after.sync_ns && after.used_ns > 0, "scans and the held background build earn no adaptive credit");
    CHECK(!memcmp(&S, &saved, sizeof S), "lookahead leaves scissor, program and all active draw state untouched");

    uint64_t skipped = atomic_load(&g_hk.pipe_skips);
    pthread_t helper; pthread_create(&helper, NULL, release_prediction, NULL);
    p_clear_color clear = {0, 15, {0, 0, 1, 1}}; op(OP_CLEAR_COLOR, &clear, sizeof clear);
    channels.mask = 2; memcpy(state.bytes, &channels, sizeof channels);
    op(OP_BIND_STATE, &state, sizeof state); op(OP_DRAW, &d, sizeof d); op(OP_DRAW, &d, sizeof d);
    op(OP_COPY_T2B, &readback, sizeof readback); op(OP_END, NULL, 0);
    mtl_exec(ops); used = 0;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "predicted pending/ready draw submission completed");
    pthread_join(helper, NULL); vk_pipeline_test_stats_get(&after);
    CHECK(!after.sync_ns, "joining a predicted worker, including pipeline-cache serialization, earns no build credit");
    CHECK(atomic_load(&g_hk.pipe_skips) == skipped && graphics_count(&attempts[2]) == 1 && !graphics_count(&main_builds[2]),
          "predicted pending key is joined once; READY memo reuse adds no skipped draw or duplicate build");
    CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 255, "prediction's real pending-to-ready draw produces the requested pixels");
}
/* Hold only the monolithic optimization: reaching the wrapper proves that every missing library is ready. */
static int library_warm(unsigned mask, unsigned topology, const unsigned expected[4], uint8_t *pixel) {
    vk_pipeline_test_stats before, after; vk_pipeline_test_stats_get(&before);
    unsigned done = graphics_count(&completed[mask]) + 1;
    hold_graphics(mask); draw_topology(mask, 1, topology);
    int held = wait_graphics(&entered, 1);
    CHECK(held, "warm mask %u topology %u reaches its held monolithic build", mask, topology);
    vk_pipeline_test_stats_get(&after);
    CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255, "unknown library combination skips its first draw");
    CHECK(after.library_skips == before.library_skips + 1 && after.fast_links == before.fast_links &&
          after.fast_draws == before.fast_draws, "missing parts never build or link synchronously on the first draw");
    for (unsigned i = 0; i < 4; i++) {
        CHECK(after.parts[i] == before.parts[i] + expected[i], "warm combination creates exactly %u library part %u", expected[i], i);
        CHECK(expected[i] ? after.part_ns[i] > before.part_ns[i] : after.part_ns[i] == before.part_ns[i],
              "library part %u timing follows actual creations only", i);
    }
    release_graphics();
    int finished = wait_graphics(&completed[mask], done);
    CHECK(finished, "warm monolithic build finishes after release");
    return held && finished;
}
static void pipeline_library_probe(uint8_t *pixel) {
    /* Different topology classes distinguish only vertex input; write masks distinguish only fragment output.
       Their unobserved cross-product (triangle, mask 2) must use all four independently cached libraries. */
    static const unsigned first_parts[4] = {1, 1, 1, 1}, second_parts[4] = {1, 0, 0, 1};
    uint64_t bounds = atomic_load(&g_hk.pipe_bounds), sync = atomic_load(&g_hk.pipes_sync);
    if (!library_warm(15, 4, first_parts, pixel) || !library_warm(2, 1, second_parts, pixel)) return;
    vk_pipeline_test_stats ready, held, published, after; vk_pipeline_test_stats_get(&ready);
    CHECK(graphics_count(&attempts[15]) == 1 && graphics_count(&attempts[2]) == 1 &&
          !graphics_count(&main_builds[15]) && !graphics_count(&main_builds[2]), "warm monolithic builds run once on the worker");
    hold_graphics(2);
    draw(2, 1);   /* exactly one draw and one submission for this previously unseen complete key */
    int entered_optimizer = wait_graphics(&entered, 1);
    CHECK(entered_optimizer && !graphics_count(&release_build), "cold recombination queues its optimized build without waiting for it");
    vk_pipeline_test_stats_get(&held);
    CHECK(held.fast_links == ready.fast_links + 1 && held.fast_draws == ready.fast_draws + 1,
          "the first cold-key draw fast-links and issues real geometry in the same submission");
    CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 255 && pixel[3] == 255,
          "fast-linked geometry uses the recombined output mask while monolithic optimization remains held");
    CHECK(held.library_skips == ready.library_skips && !memcmp(held.parts, ready.parts, sizeof held.parts) &&
          !memcmp(held.part_ns, ready.part_ns, sizeof held.part_ns), "cold recombination neither skips nor creates any library part");
    CHECK(held.optimized_replacements == ready.optimized_replacements && held.optimized_ns == ready.optimized_ns,
          "held optimization cannot be published or replace the fast pipeline");
    CHECK(held.fast_link_ns > ready.fast_link_ns && held.fast_link_max_ns > 0 &&
          held.fast_link_max_ns <= held.fast_link_ns, "successful fast linking has cumulative total and maximum driver timings");
    CHECK(graphics_count(&attempts[2]) == 2 && !graphics_count(&main_builds[2]),
          "fast link is not mistaken for a monolithic build and optimization is worker-only");
    release_graphics();
    int finished = wait_graphics(&completed[2], 2);
    CHECK(finished, "cold-key optimization finishes after release");
    if (!entered_optimizer || !finished) return;

    /* The wrapper's completion signal precedes publication. Observe publication by drawing, not by racing
       a worker's timing fields, and keep these observations within one open submission. */
    uint64_t memo_hits = vk_prog_record_test_hits();
    p_draw d = {4, 0, 3, 0, 0};
    mtl_sub_begin();
    for (unsigned i = 0; i < 5000; i++) {
        op(OP_DRAW, &d, sizeof d); op(OP_END, NULL, 0); mtl_exec(ops); used = 0;
        vk_pipeline_test_stats_get(&published);
        if (published.optimized_replacements > held.optimized_replacements) break;
        usleep(1000);
    }
    CHECK(published.optimized_replacements == held.optimized_replacements + 1 && published.optimized_ns > held.optimized_ns,
          "a later unchanged-state draw publishes exactly one optimized replacement with cumulative build timing");
    CHECK(!vk_record_enabled() || vk_prog_record_test_hits() > memo_hits, "optimized replacement is visible through the recorder program memo");
    p_clear_color clear = {0, 15, {0, 0, 1, 1}}; op(OP_CLEAR_COLOR, &clear, sizeof clear);
    op(OP_DRAW, &d, sizeof d); op(OP_COPY_T2B, &readback, sizeof readback); op(OP_END, NULL, 0);
    mtl_exec(ops); used = 0;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "optimized replacement submission completed");
    vk_pipeline_test_stats_get(&after);
    CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 255 && pixel[3] == 255,
          "optimized replacement redraws real geometry after an explicit clear");
    CHECK(after.fast_draws == published.fast_draws && after.fast_links == held.fast_links &&
          after.optimized_replacements == published.optimized_replacements && after.optimized_ns == published.optimized_ns,
          "later ready draws reuse the optimized pipeline instead of the retained fast handle");
    CHECK(!memcmp(after.parts, ready.parts, sizeof after.parts) && !memcmp(after.part_ns, ready.part_ns, sizeof after.part_ns) &&
          after.library_skips == ready.library_skips, "optimized publication leaves cached-part and skipped-draw snapshots unchanged");
    CHECK(graphics_count(&attempts[2]) == 2 && !graphics_count(&main_builds[2]) &&
          atomic_load(&g_hk.pipe_bounds) == bounds && atomic_load(&g_hk.pipes_sync) == sync,
          "library recombination and replacement require no submission-bound join, takeover or duplicate build");
}
/* A mirrored block-linear texture large enough for the parallel deswizzle (TOTK_VK_UPLOAD_THREADS=1..3) reads back
   through CopyTextureToBuffer exactly as the serial decoder lays it out, at every level, with the switch on or off. */
static void upload_probe(pool_t *pool, uint8_t *mem) {
    tex_t big = {.d = {.target = 1, .format = 0x25, .width = 1024, .height = 256, .depth = 1, .levels = 3}};
    nvn_tex_layout(&big.d, &big.L); big.mem = mem + 0x800000; big.pool = pool; big.serial = 900;
    uint32_t s = 0x9e3779b9; for (uint64_t i = 0; i < big.L.size; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; big.mem[i] = (uint8_t)s; }
    upload_pool_stats stats; upload_pool_stats_take(&stats);
    uint8_t *out = mem + 0xc00000, *expected = malloc(1024 * 256 * 4);
    for (unsigned level = 0; level < big.d.levels; level++) {
        uint32_t w = big.d.width >> level, h = big.d.height >> level; memset(out, 0, (size_t)w * h * 4);
        p_copy_b2t copy = {.src = (uintptr_t)out, .tex = &big, .has_view = 1, .view = {.set_levels = 1, .base_level = (uint8_t)level, .nlevels = 1},
                           .r = {0, 0, 0, (int)w, (int)h, 1}};
        op(OP_COPY_T2B, &copy, sizeof copy);
        CHECK(mtl_wait(submit(), 5000000000ull), "upload readback level %u completed", level);
        CHECK(!nvn_deswizzle(&big.d, level, 0, big.mem, expected, (size_t)w * 4) && !memcmp(out, expected, (size_t)w * h * 4),
              "level %u of a 1.3 MiB block-linear upload reads back byte-identical to the serial decoder", level);
    }
    free(expected);
    upload_pool_stats_take(&stats);
    /* Untracked pool bytes re-upload at every submission: one parallel job per readback. */
    CHECK(upload_pool_helpers() ? stats.jobs == big.d.levels && stats.runs >= 2 * stats.jobs : !stats.jobs,
          "every upload deswizzled %s (jobs %llu, runs %llu)", upload_pool_helpers() ? "in parallel" : "serially",
          (unsigned long long)stats.jobs, (unsigned long long)stats.runs);
}
/* One draw into two RGBA8 colour targets cleared blue and green; returns whether a new two-attachment pipeline was
   built and attachment 1's write mask, with both targets read back to out. */
static int two_target_draw(const p_bind_program *bind, tex_t color[2], uint8_t *out[2], unsigned *mask) {
    p_set_rt rt = {.n = 2, .color = {&color[0], &color[1]}};
    p_viewports square = viewport; square.v[0][2] = square.v[0][3] = 64;
    p_clear_color blue = {0, 15, {0, 0, 1, 1}}, green = {1, 15, {0, 1, 0, 1}};
    chmask_st channels = {0xff}; p_bind_state state = {.kind = 2}; memcpy(state.bytes, &channels, sizeof channels);
    p_draw d = {4, 0, 3, 0, 0};
    unsigned builds = graphics_count(&two_target_builds);
    op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &blue, sizeof blue); op(OP_CLEAR_COLOR, &green, sizeof green);
    op(OP_BIND_PROGRAM, bind, sizeof *bind); op(OP_BIND_VATTR, &attrs, sizeof attrs); op(OP_BIND_VSTREAM, &streams, sizeof streams);
    op(OP_BIND_VBUF, &vertices, sizeof vertices); op(OP_BIND_UBO, &uniforms[0], sizeof uniforms[0]); op(OP_BIND_UBO, &uniforms[1], sizeof uniforms[1]);
    op(OP_BIND_TEX, &texture, sizeof texture); op(OP_VIEWPORTS, &square, sizeof square); op(OP_BIND_STATE, &state, sizeof state);
    op(OP_DRAW, &d, sizeof d);
    for (int i = 0; i < 2; i++) {
        memset(out[i], 0x5c, 64 * 64 * 4);
        p_copy_b2t back = {.src = (uintptr_t)out[i], .tex = &color[i], .r = {0, 0, 0, 64, 64, 1}}; op(OP_COPY_T2B, &back, sizeof back);
    }
    CHECK(mtl_wait(submit(), 5000000000ull), "two-target draw submission completed");
    pthread_mutex_lock(&mu); int built = two_target_builds > builds; *mask = second_write_mask; pthread_mutex_unlock(&mu);
    return built;
}
/* The fragment shader (salt 2) outputs location 0 only. NVN leaves target 1 untouched: TOTK_VK_UNWRITTEN_MASK=1
   builds colorWriteMask 0 for it, so it keeps its clear; unset keeps the game's mask 0xf. An array output covering
   locations 0..1 (salt 46; the seed's fout names location 0 only) is never masked. */
static void unwritten_probe(const tex_t *base, uint8_t *mem) {
    const char *env = getenv("TOTK_VK_UNWRITTEN_MASK"); int on = env && atoi(env) == 1;
    tex_t color[2] = {*base, *base};
    for (int i = 0; i < 2; i++) { color[i].mem = mem + 0xa00000 + i * 0x10000; color[i].serial = 910 + (uint64_t)i; }
    uint8_t *out[2] = {mem + 0xa20000, mem + 0xa30000};
    const uint8_t *p0 = out[0] + (10 * 64 + 10) * 4, *p1 = out[1] + (10 * 64 + 10) * 4;
    unsigned mask;
    int built = two_target_draw(&program, color, out, &mask);
    CHECK(built && p0[0] == 255 && p0[1] == 255 && p0[2] == 255, "location 0 draws the textured result into target 0 (%u,%u,%u)", p0[0], p0[1], p0[2]);
    if (on) CHECK(!mask && p1[0] == 0 && p1[1] == 255 && p1[2] == 0 && p1[3] == 255,
                  "unwritten target 1 gets colorWriteMask 0 (%#x) and keeps its green clear (%u,%u,%u,%u)", mask, p1[0], p1[1], p1[2], p1[3]);
    else CHECK(mask == 0xf, "switch off: target 1 keeps the game's write mask (%#x; Vulkan leaves it undefined, here %u,%u,%u,%u)",
               mask, p1[0], p1[1], p1[2], p1[3]);
    static uint8_t control[0x880]; static prog_t arrays;
    arrays = *program.prog; arrays.serial = 102;
    uint8_t *code = mem + 0xa40000; shader_binary(1, 46, control, code);
    arrays.sd[1].data = (uintptr_t)code; arrays.sd[1].control = control;
    p_bind_program bind = {&arrays, 3};
    built = two_target_draw(&bind, color, out, &mask);
    CHECK(built && mask == 0xf && p0[0] == 255 && p0[1] == 255 && p0[2] == 255 && p1[0] == 255 && !p1[1] && p1[2] == 255,
          "an array output spanning locations 0..1 writes both targets (mask %#x, target 1 %u,%u,%u)", mask, p1[0], p1[1], p1[2]);
}
static int finish_tests(void) {
    uint64_t lib_ns, pso_ns, lib_wait_ns, pso_wait_ns, shader_scan_ns, pso_scan_ns;
    mtl_create_ns(&lib_ns, &pso_ns); mtl_wait_stats(&lib_wait_ns, &pso_wait_ns); mtl_scan_stats(&shader_scan_ns, &pso_scan_ns);
    CHECK(!lib_ns && !lib_wait_ns && !shader_scan_ns, "Android has no runtime shader-library or translation accounting");
    if (dm_wait_stats_enabled()) CHECK(pso_ns > 0, "enabled diagnostics expose actual synchronous graphics/compute driver calls");
    else CHECK(!pso_ns && !pso_wait_ns && !pso_scan_ns, "disabled diagnostic snapshots remain zero");
    for (unsigned i = 0; i < 300; i++) { if (i == 100) usleep(60000); mtl_present(NULL, 1.0 / 60, NULL); }
    puts("hitch histogram exercised (synthetic frame intervals, not a performance measurement)");
    if (V.has_pipeline_library) {
        vk_pipeline_test_stats stats; vk_pipeline_test_stats_get(&stats);
        printf("pipeline-library: fast_links=%llu fast_link_ns=%llu fast_link_max_ns=%llu fast_draws=%llu "
               "optimized_replacements=%llu optimized_ns=%llu library_skips=%llu\n",
               (unsigned long long)stats.fast_links, (unsigned long long)stats.fast_link_ns,
               (unsigned long long)stats.fast_link_max_ns, (unsigned long long)stats.fast_draws,
               (unsigned long long)stats.optimized_replacements, (unsigned long long)stats.optimized_ns,
               (unsigned long long)stats.library_skips);
        for (unsigned i = 0; i < 4; i++)
            printf("pipeline-library: part[%u]=%llu part_ns[%u]=%llu\n", i, (unsigned long long)stats.parts[i],
                   i, (unsigned long long)stats.part_ns[i]);
    }
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
int main(int argc, char **argv) {
    int screen_test = argc == 2 && !strcmp(argv[1], "--screen-classify");
    int screen_pure_test = argc == 2 && !strcmp(argv[1], "--screen-classify-pure");
    int target_life_test = argc == 2 && !strcmp(argv[1], "--target-life");
    if (screen_test || screen_pure_test) screen_classify_cases();
    if (screen_pure_test) {
        printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (screen_test || target_life_test) {
        setenv("TOTK_RENDER_RES", "960x540", 1);
        setenv("TOTK_VK_PIPELINE_LIBRARY", "0", 1);
        setenv("TOTK_PSO_PREDICT", "0", 1);
    }
    if (target_life_test) {
        setenv("TOTK_PSO_LIFE", "480x268", 1);
        setenv("TOTK_PSO_LIFE_FRAMES", "20", 1);
        setenv("TOTK_VK_REC_THREADS", "2", 1);
    }
    extern int vk_pipeline_test_keys(void);
    if (argc == 2 && !strcmp(argv[1], "--pso-keylist")) return vk_pipeline_test_keys();
    if (argc == 2 && !strcmp(argv[1], "--pso-warm-only")) {
        extern int vk_pipeline_test_key_order(void);
        mtl_init();
        uint64_t stats[6] = {0};
        for (unsigned i = 0; i < 30000; i++) {
            vk_pipeline_keys_stats(stats);
            if (stats[0]+stats[3]+stats[4] >= stats[1]) break;
            usleep(1000);
        }
        printf("pso-warm-only: done=%llu total=%llu ns=%llu missing=%llu failed=%llu order=%d\n",
            (unsigned long long)stats[0], (unsigned long long)stats[1], (unsigned long long)stats[2],
            (unsigned long long)stats[3], (unsigned long long)stats[4], vk_pipeline_test_key_order());
        vk_pcache_flush();
        return stats[4] || stats[0]+stats[3] != stats[1] || !vk_pipeline_test_key_order();
    }
    int prediction_test = argc == 2 && !strcmp(argv[1], "--predict");
    int library_test = argc == 2 && !strcmp(argv[1], "--gpl");
    int compute_warm_test = argc == 2 && !strcmp(argv[1], "--compute-warm");
    if (library_test) setenv("TOTK_VK_PIPELINE_LIBRARY", "1", 1);
    const char *library_env = getenv("TOTK_VK_PIPELINE_LIBRARY");
    int library_requested = library_env && atoi(library_env);
    if (prediction_test) {
        setenv("TOTK_PSO_PREDICT", "1", 1); setenv("TOTK_PSO_SCAN_ADAPT", "1", 1);
        setenv("TOTK_PSO_PREDICT_SCAN", "16", 1); setenv("TOTK_PSO_PREDICT_KEYS", "2", 1);
        setenv("TOTK_PSO_PREDICT_US", "1000000", 1); setenv("TOTK_PSO_PREDICT_FRAME_US", "1000000", 1);
        setenv("TOTK_PSO_RESCANS", "1", 1);
    }
    setenv("TOTK_PIPE_WORKERS", "1", 1); setenv("TOTK_VK_PIPE_WAIT_MAX", "2", 1); setenv("TOTK_VK_HITCH", "1", 1);
    setenv("TOTK_PSO_PREWARM", "0", 1); setenv("TOTK_VK_PIPELINE_CACHE", "0", 1); walk = pthread_self();
    setenv("TOTK_COMPUTE_STATS", "1", 1);
    mtl_init();
    if (compute_warm_test) {
        uint64_t counts[4];
        for (unsigned i = 0; i < 50000; i++) {
            vk_compute_test_counts(counts); if (counts[1]) break; usleep(100);
        }
    } /* finish startup driver calls before replacing the test dispatch pointer */
    if (!V.has_cache_control) { puts("pipeline tests require pipelineCreationCacheControl"); return 1; }
    if (library_requested && !V.has_pipeline_library) { puts("pipeline library tests require fully supported VK_EXT_graphics_pipeline_library"); return 1; }
    real_graphics = vkCreateGraphicsPipelines; real_compute = vkCreateComputePipelines;
    vkCreateGraphicsPipelines = graphics; vkCreateComputePipelines = compute;
    uint8_t *mem = mmap(NULL, 16u << 20, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(mem != MAP_FAILED, "test pool mapped"); if (mem == MAP_FAILED) return 1;
    mtl_pool_add(mem, 16u << 20);
    pool_t pool = {.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = mem, .size = 16u << 20};
    tex_t tex = {0}, target = {0}; tex.d.target = 1; tex.d.format = 0x25; tex.d.width = tex.d.height = 64; tex.d.depth = tex.d.levels = 1;
    nvn_tex_layout(&tex.d, &tex.L); tex.mem = mem; tex.pool = &pool; tex.serial = 1;
    uint8_t white[64 * 64 * 4]; memset(white, 255, sizeof white); nvn_swizzle(&tex.d, 0, 0, white, 256, tex.mem);
    target = tex; target.d.height = 36; nvn_tex_layout(&target.d, &target.L); target.mem = mem + 0x10000; target.serial = 2;
    mtl_reg_tex(1, &tex, NULL); samp_par sp = {.lod_max = 1000}; mtl_reg_samp(1, &sp);
    uint8_t control[2][0x880]; prog_t pg = {.nstage = 2, .serial = 100};
    for (int i = 0; i < 2; i++) { uint8_t *code = mem + 0x20000 + i * 0x1000; shader_binary(i, i + 1, control[i], code); pg.sd[i].data = (uintptr_t)code; pg.sd[i].control = control[i]; }
    float full[] = {-1, -1, 3, -1, -1, 3}; memcpy(mem + 0x30000, full, sizeof full);
    float tint[] = {1, 1, 1, 1}; memcpy(mem + 0x40100, tint, sizeof tint);
    rt_bind.n = 1; rt_bind.color[0] = &target; program = (p_bind_program){&pg, 3}; attrs.n = 1;
    va_st a = {0x16, 0, 0}; memcpy(&attrs.st[0], &a, 4); streams.n = 1; streams.st[0].stride = 8;
    vertices.n = 1; vertices.r[0].address = (uintptr_t)(mem + 0x30000); vertices.r[0].size = 64;
    uniforms[0] = (p_bind_buf){0, 0, (uintptr_t)(mem + 0x40000), 256}; uniforms[1] = (p_bind_buf){1, 0, (uintptr_t)(mem + 0x40100), 256};
    texture.stage = 1; texture.n = 1; texture.h[0] = 1 | (1u << 20); viewport.n = 1; viewport.v[0][2] = 64; viewport.v[0][3] = 36; viewport.v[0][5] = 1;
    readback.src = (uintptr_t)(mem + 0x50000); readback.tex = &target; readback.r = (NVNcopyRegion){0, 0, 0, 64, 36, 1};
    uint8_t *pixel = mem + 0x50000 + (10 * 64 + 10) * 4;
    if (screen_test) { screen_classify_probe(&tex, mem); return finish_tests(); }
    if (target_life_test) {
        /* Track only immutable shader/vertex/UBO/white-texture inputs. GPU readback stays outside:
           untracked uniform copies correctly cannot seed the protected descriptor-freeze memo. */
        if (vk_lag_capture_on()) nvn_dirty_add_range(mem, 0x50000);
        target_life_probe(&tex, mem); return finish_tests();
    }
    if (library_test) { pipeline_library_probe(pixel); return finish_tests(); }
    if (compute_warm_test) {
        uint64_t counts[4] = {0};
        for (unsigned i = 0; i < 50000; i++) {
            vk_compute_test_counts(counts); if (counts[1]) break; usleep(100);
        }
        CHECK(counts[1] == 1 && counts[2] == 1 && !counts[0], "persisted exact compute key built at startup on background worker");
        uint8_t ctl[0x880]; uint8_t *code = mem + 0x22000; shader_binary(5, 3, ctl, code);
        prog_t pg = {.nstage = 1, .serial = 101}; pg.sd[0].control = ctl; pg.sd[0].data = (uintptr_t)code;
        vk_shader *sh[6]; CHECK(!vk_prog_resolve(&pg, sh) && sh[5], "learned compute shader resolves");
        VkPipelineLayout layout; uint64_t before = vk_compute_pipe_cum_ns();
        CHECK(vk_compute_pipeline(sh[5], &layout) != VK_NULL_HANDLE, "learned compute handle available before dispatch");
        CHECK(vk_compute_pipe_cum_ns() == before && !graphics_count(&compute_calls), "learned key incurs zero recording creation/blocking");
        return finish_tests();
    }
    const char *boost_env = getenv("TOTK_VK_PIPE_BOOST"); int boost_on = boost_env && atoi(boost_env) == 1;
    vk_pipe_boost_test_hooks(waiter_nice, boost_hook);
    draw(15, 1);
    pthread_mutex_lock(&mu); while (!entered) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    CHECK(pixel[0] == 0 && pixel[2] == 255, "cold miss skipped the draw (clear remains)");
    /* With GPL the held mask-15 build already owns four ready parts. Demand a fresh output part instead,
       so the existing submission bound still exercises an unavailable queued key rather than a fast link. */
    if (V.has_pipeline_library) draw(7, 100);
    else draw_current(100);
    CHECK(!atomic_load(&g_hk.pipe_bounds), "100 draws in one submission do not exhaust the two-submission bound");
    if (V.has_pipeline_library) draw_current(1);
    pthread_t helper; pthread_create(&helper, NULL, release_at_bound, NULL);
    draw_current(1); pthread_join(helper, NULL);
    CHECK(pixel[0] == 255 && pixel[1] == 255, "bounded in-flight build joined; later draw has real pixels");
    CHECK(graphics_count(&attempts[15]) == 1 && atomic_load(&g_hk.pipe_bounds) == 1, "bound never duplicates or overwrites a worker build");
    uint64_t boost[4]; vk_pipe_boost_test_stats(boost);
    CHECK(boost[0] >= 1 && boost[1] > 0, "the bounded join is counted with its wall time");
    if (boost_on) CHECK(wait_graphics(&nboost, 2) && boost_loan_seen(0, held_tid) && !boost[3],
                        "the join lent the waiter's nice to the held worker, which restored nice %d after publishing", VK_WORKER_NICE);
    else CHECK(!graphics_count(&nboost) && !boost[2], "switch off: a join never changes worker priority");
    if (V.has_pipeline_library) CHECK(graphics_count(&attempts[7]) == 1, "bounded unavailable-part key is built exactly once");
    vk_pipeline_test_stats credit;
    vk_pipeline_test_stats_get(&credit);
    if (V.has_pipeline_library && prediction_test && graphics_count(&main_builds[7]))
        CHECK(credit.sync_ns > 0, "only a real walker takeover of the unavailable-part key earns adaptive credit");
    else CHECK(!credit.sync_ns, "background compilation, prediction scans and bounded worker joins earn no scan credit");
    uint64_t memo_hits = vk_prog_record_test_hits();
    draw_current(1);
    CHECK(pixel[0] == 255 && pixel[1] == 255, "published pipeline remains visible to a later unchanged-state draw");
    CHECK(!vk_record_enabled() || vk_prog_record_test_hits() > memo_hits, "publication draw exercises the recorder program memo");

    /* Compute has its own worker: a held graphics compile cannot starve it.
       Bind starts creation before any dispatch; dispatch must join, never skip. */
    uint64_t before_compute_sync = credit.sync_ns;
    pthread_mutex_lock(&mu); held_mask = 3; entered = release_build = 0; pthread_mutex_unlock(&mu);
    draw(3, 1);
    pthread_mutex_lock(&mu); while (!entered) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    uint8_t cs_control[0x880]; uint8_t *cs_code = mem + 0x22000; shader_binary(5, 3, cs_control, cs_code);
    prog_t cs_prog = {.nstage = 1, .serial = 101}; cs_prog.sd[0].data = (uintptr_t)cs_code; cs_prog.sd[0].control = cs_control;
    pthread_mutex_lock(&mu); compute_hold = 1; compute_entered = 0; pthread_mutex_unlock(&mu);
    p_bind_program cbp = {&cs_prog, 1u << 5};
    op(OP_BIND_PROGRAM, &cbp, sizeof cbp); CHECK(mtl_wait(submit(), 5000000000ull), "bind-only compute submission completes without waiting for compilation");
    CHECK(wait_graphics(&compute_entered, 1), "program bind starts background compute creation ahead of dispatch");
    CHECK(!graphics_count(&compute_recording), "compute driver call is not on the recording thread");
    vk_shader *resolved[6]; CHECK(!vk_prog_resolve(&cs_prog, resolved) && resolved[5], "compute seed resolved");
    uint32_t *result = (uint32_t *)(mem + 0x60000); memset(result, 0, 256);
    p_bind_buf output = {5, 0, (uintptr_t)result, 256}; p_dispatch dispatch = {1, 1, 1};
    unsigned boost_before = graphics_count(&nboost);
    pthread_create(&helper, NULL, release_compute_waiter, NULL);
    op(OP_BIND_UBO, &output, sizeof output); op(OP_DISPATCH, &dispatch, sizeof dispatch);
    CHECK(mtl_wait(submit(), 5000000000ull), "dispatch waits for held compute creation and completes");
    void *joined = NULL; pthread_join(helper, &joined);
    CHECK(joined != NULL, "dispatch joined the unfinished compute build instead of skipping");
    int correct = 1;
    for (uint32_t i = 0; i < 64; i++) if (result[i] != (i ^ 0x12345678u)) correct = 0;
    CHECK(correct, "joined dispatch writes every expected output word");
    CHECK(graphics_count(&compute_calls) == 1, "bind/resolve/dispatch share exactly one in-flight compute creation");
    if (boost_on) CHECK(wait_graphics(&nboost, boost_before + 2) && boost_loan_seen(boost_before, compute_tid),
                        "the dispatch join lent the waiter's nice to the compute worker until its build published");
    else CHECK(!graphics_count(&nboost), "switch off: the dispatch join never changes compute worker priority");
    pthread_mutex_lock(&mu); int graphics_still_held = !release_build; release_build = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    CHECK(graphics_still_held, "compute dispatch completes while graphics compilation is still held");
    CHECK(wait_graphics(&completed[3], 1), "released graphics driver call drains before reusing the hold gate");
    vk_pipeline_test_stats_get(&credit);
    CHECK(credit.sync_ns == before_compute_sync, "compute work and its wait behind a graphics worker do not fund graphics lookahead");

    /* Cache probes must queue without blocking on a busy compiler. At the bound the queued key transfers to the walk. */
    unsigned busy_mask = V.has_pipeline_library ? 9 : 11;
    pthread_mutex_lock(&mu); held_mask = busy_mask; entered = release_build = 0; pthread_mutex_unlock(&mu);
    draw(busy_mask, 1);
    pthread_mutex_lock(&mu); while (!entered) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    draw(1, 100); draw(1, 1);
    CHECK(graphics_count(&attempts[1]) == 0, "a second cold key queued while the pipeline cache was busy");
    uint64_t before_takeover = atomic_load(&g_hk.pipes_sync);
    pthread_create(&helper, NULL, release_for_takeover, &before_takeover);
    draw(1, 1); pthread_join(helper, NULL);
    CHECK(graphics_count(&attempts[1]) == 1 && graphics_count(&main_builds[1]) == 1, "bounded queued build taken over synchronously exactly once");
    CHECK(pixel[0] == 255 && pixel[1] == 0, "queued takeover produces real pixels with the requested write mask");
    vk_pipeline_test_stats_get(&credit);
    if (prediction_test) CHECK(credit.sync_ns > 0, "actual synchronous queued takeover earns adaptive scan credit");

    pthread_mutex_lock(&mu); failure_mask = V.has_pipeline_library ? 11 : 7; failures_left = 2; pthread_mutex_unlock(&mu);
    draw(failure_mask, 1);
    for (unsigned i = 0; i < 10000 && !atomic_load(&g_hk.pipes_fail); i++) usleep(100);
    CHECK(atomic_load(&g_hk.pipes_fail) == 1, "worker failure recorded");
    for (unsigned i = 0; i < 4 && atomic_load(&g_hk.pipes_fail) < 2; i++) draw(failure_mask, 1);
    CHECK(atomic_load(&g_hk.pipes_fail) == 2, "failed key retried synchronously rather than permanently hidden");
    draw(failure_mask, 100); /* observe the second failure and establish its 30-submission backoff */
    unsigned before = graphics_count(&attempts[failure_mask]);
    for (unsigned i = 0; i < 20; i++) draw(failure_mask, 1);
    CHECK(graphics_count(&attempts[failure_mask]) == before, "repeated failure backs off instead of retrying per draw/submission");
    for (unsigned i = 0; i < 20 && graphics_count(&attempts[failure_mask]) == before; i++) draw(failure_mask, 1);
    CHECK(graphics_count(&attempts[failure_mask]) == 3 && pixel[0] == 255 && pixel[1] == 255, "retry eventually recovers real geometry");
    upload_probe(&pool, mem);
    if (prediction_test) prediction_probe(&tex, pixel);
    unwritten_probe(&tex, mem);
    return finish_tests();
}
#else
int main(void) {
    screen_classify_cases();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}
#endif

/* GPU profiler integration fixture: real NVN replay, recorder workers and Vulkan
 * timestamps/statistics, no game/device/WSI. Build vkgpuproftest and vkdraw-seed;
 * tests/test_gpuprof.py drives fresh processes for each launch-time switch.
 * Existing fixtures keep an in-buffer frame marker. --sealed-present instead
 * calls the production CPU present hook AFTER the final real submission, as
 * Android does; --last-closed checks its wakeup before any terminal flush. */
/* CPU measurement arm: --scaled [--frames N] [--textures 0..160]
 * [--passes N] [--draws N]. Defaults: 302 frames, 180 real rendering passes
 * and 2000 actual draws per frame, with 16 bound textures. Registry textures
 * need not be consumed by salt37/38: the legacy profiler audits all bound handles.
 * Replay, commit and fence-wait wall times are reported separately; this is
 * a CPU-overhead fixture, not a game/device GPU-performance prediction. */
#include "vk_int.h"
#include "vk_record.h"
#include "vk_gpuprof_time.h"
#include "vk_gpuprof_pipe.h"
#include "vk_gpuprof_compact.h"
int vk_gpuprof_test_compact_run(int argc, char **argv);
int vk_gpuprof_test_record_run(void);
int vk_gpuprof_test_sink_run(void);
int vk_gpuprof_test_report_contract(void);
#include "vk_load.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <errno.h>
#include <glob.h>
#include <sys/mman.h>

static unsigned checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void timestamp_contract(void) {
    vk_gpuprof_time_domain domain = {1, 2, 0, 48, 1};
    vk_gpuprof_timestamp first = {100, 1}, last = {200, 1};
    vk_gpuprof_interval value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && !value.wrapped && fabs(value.ms - 0.0001) < 1e-12,
        "same-domain available timestamp interval uses advertised units");
    first.available = 0; first.ticks = UINT64_MAX;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_UNAVAILABLE && !value.has_value, "unavailable timestamp cannot reuse stale value");
    first = (vk_gpuprof_timestamp){0, 1}; last = first;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.ms == 0, "available zero counter is legal");
    uint64_t mask = vk_gpuprof_tick_mask(48);
    first.ticks = mask - 9; last.ticks = 10;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.wrapped && fabs(value.ms - 0.00002) < 1e-12,
        "real 48-bit wrap remains a bounded valid interval");
    first.ticks = mask - 3; last.ticks = 0;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.wrapped && value.ms == 0.000004,
        "zero end timestamp at a real wrap is not fabricated unavailability");
    first.ticks = (UINT64_C(1) << 60) | 100; last.ticks = (UINT64_C(2) << 60) | 200;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && !value.wrapped && fabs(value.ms - 0.0001) < 1e-12,
        "only timestampValidBits participate in interval math");
    first.ticks = 100000000; last.ticks = 99999900; domain.period_ns = (double)(float)52.0833321;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_IMPLAUSIBLE && value.has_value && value.ms > 1e10,
        "Thor-style backwards endpoint is flagged, not accepted as a 169-day wrap");
    domain.period_ns = 1; domain.valid_bits = 64;
    first.ticks = UINT64_MAX - 3; last.ticks = 5;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.wrapped && value.ms == 0.000009,
        "64-bit wrap avoids undefined shift and preserves exact delta");
    first.ticks = 0; last.ticks = UINT64_C(10000000000);
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.ms == VK_GPUPROF_MAX_FRAME_MS, "sanity limit is inclusive");
    last.ticks++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_IMPLAUSIBLE,
        "positive implausible duration is rejected too");
    vk_gpuprof_time_domain other = domain; other.queue++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "different queues cannot feed one span");
    other = domain; other.queue_family++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "queue families are explicit timestamp domains");
    other = domain; other.device++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "different devices cannot feed one span");
    other = domain; other.period_ns = 2;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "different timestamp units cannot feed one span");
    domain.valid_bits = 0;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_DOMAIN,
        "zero timestamp precision is unavailable");
    domain.valid_bits = 65;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_DOMAIN,
        "invalid timestamp precision never reaches a shift");
    domain.valid_bits = 48; domain.period_ns = NAN;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_DOMAIN,
        "invalid timestamp period is rejected");
}
static void pipeline_contract(void) {
    for (unsigned budget = 1; budget <= VK_GPUPROF_PIPE_BUDGET; budget++) {
        unsigned hits[VK_GPUPROF_PIPE_ORDINALS] = {0};
        for (unsigned phase = 0; phase < VK_GPUPROF_PIPE_ORDINALS; phase++) {
            for (unsigned i = 0; i < budget; i++) {
                unsigned ordinal = (phase + i) % VK_GPUPROF_PIPE_ORDINALS;
                CHECK(vk_gpuprof_pipe_selected(ordinal, phase, budget, VK_GPUPROF_PIPE_ORDINALS), "rotating window selects each bounded group");
                hits[ordinal]++;
            }
            CHECK(!vk_gpuprof_pipe_selected((phase + budget) % VK_GPUPROF_PIPE_ORDINALS, phase, budget, VK_GPUPROF_PIPE_ORDINALS),
                "next ordinal never exceeds the hard frame budget");
            CHECK(!vk_gpuprof_pipe_selected(VK_GPUPROF_PIPE_ORDINALS, phase, budget, VK_GPUPROF_PIPE_ORDINALS),
                "unknown tail is never presented as an equal-probability sample");
        }
        for (unsigned i = 0; i < VK_GPUPROF_PIPE_ORDINALS; i++)
            CHECK(hits[i] == budget, "uniform phase gives every ordinal exactly k/N inclusion");
    }
    CHECK(vk_gpuprof_pipe_population(2048, 4000) == 4096 &&
        vk_gpuprof_pipe_population(4096, 2000) == 4096, "domain grows to cover whole layouts and never shrinks");
    CHECK(vk_gpuprof_pipe_selected(3000, 3000, 16, 4096),
        "newly observed tail groups become eligible after domain growth");
    uint64_t n = UINT64_MAX;
    CHECK(vk_gpuprof_pipe_primitives(4, 9, 3, 0, &n) && n == 9, "triangle input counts include instances");
    CHECK(vk_gpuprof_pipe_primitives(7, 12, 2, 0, &n) && n == 12, "quads count actual emitted triangles");
    CHECK(vk_gpuprof_pipe_primitives(5, 7, 2, 0, &n) && n == 10, "non-indexed strip primitives are known");
    CHECK(!vk_gpuprof_pipe_primitives(5, 7, 2, 1, &n), "indexed restart is explicitly unknown without payload reads");
    CHECK(vk_gpuprof_pipe_primitives(0, UINT32_MAX, UINT32_MAX, 0, &n) &&
        n == (uint64_t)UINT32_MAX * UINT32_MAX, "primitive products do not truncate at 32 bits");
}
static void compact_contract(void) {
    static gp_compact c;
    gp_compact_key key = {.pipeline = 17, .shader = {"full-vs-variant-a", "full-fs-variant-a", NULL}};
    gp_compact_begin(&c, 1, 2048, 4);
    gp_compact_observe(&c, &key, 1, 1, 0, 0, 2, 2048, 4, 3, 0, 9, 0);
    gp_compact_entry *e = gp_compact_get(&c, &key);
    uint64_t id = e->id;
    c.frame_out_of_range = 1; /* later lease/nested scope invalidates the prefix */
    gp_compact_finish(&c, 1);
    CHECK(e->v[GC_VALID].integer == 1 && e->v[GC_ELIGIBLE].real == 0,
        "later-frame domain gap excludes every earlier sample from eligible subtotal");
    gp_compact_begin(&c, 2, 4096, 4);
    gp_compact_observe(&c, &key, 1, 1, 0, 0, 3, 4096, 4, 4, 0, 12, 0);
    gp_compact_finish(&c, 1);
    CHECK(e->v[GC_MS].real == 5 && e->v[GC_SQUARE].real == 13 && e->v[GC_WEIGHTED].real == 4096 &&
        e->v[GC_ELIGIBLE_WEIGHTED].real == 3072 && e->v[GC_ELIGIBLE_SQUARE].real == 3072.0 * 3072,
        "population growth weights each sample by its actual denominator and preserves moments");
    gp_compact_begin(&c, 3, 4096, 4);
    gp_compact_observe(&c, &key, 1, 0, 0, 0, 0, 4096, 4, 1, 0, 3, 0);
    gp_compact_finish(&c, 1);
    CHECK(e->v[GC_SELECTED].integer == 3 && e->v[GC_MISSING].integer == 1 && e->v[GC_ELIGIBLE_MISSING].real == 1,
        "missing selected timing is not zero and retains eligible-domain missing authority");
    CHECK(c.frames == 3 && c.eligible_frames == 2 && c.domain_excluded_frames == 1 &&
        c.nexposure == 2 && c.exposure[0].population == 2048 && c.exposure[1].frames == 2,
        "partial windows retain exact exposure strata and completed-frame counts");
    gp_compact_key variant = key; variant.shader[1] = "full-fs-variant-b";
    CHECK(gp_compact_get(&c, &variant)->id != id, "full shader variant participates in dictionary identity");
    variant = key; variant.compute = 1;
    CHECK(gp_compact_get(&c, &variant)->id != id, "graphics and compute never share a dictionary key");
    static const char alias_vs[] = "full-vs-variant-a", alias_fs[] = "full-fs-variant-a";
    gp_compact_key alias = {.pipeline = 17, .shader = {alias_vs, alias_fs, NULL}};
    CHECK(gp_compact_get(&c, &alias)->id == id && gp_compact_get(&c, &key)->id == id,
        "different immutable source objects with identical full names intern to the same exact identity");
    static char oversized_name[81];
    memset(oversized_name, 'x', 80);
    gp_compact_key oversized = {.pipeline = 18, .shader = {oversized_name, NULL, NULL}};
    CHECK(!gp_compact_get(&c, &oversized) && c.aggregate_overflow && !c.counts_complete,
        "an unrepresentable full variant is rejected, never truncated into an existing identity");
    CHECK(!c.map_dirty, "insert-only dictionary has no tombstones to rebuild");
    gp_compact_reset(&c);
    gp_compact_begin(&c, 301, 4096, 4);
    CHECK(gp_compact_get(&c, &key)->id == id && !e->v[GC_SELECTED].integer,
        "window reset preserves interned identity but never duplicates flushed samples");
    for (unsigned i = 0; i < GP_COMPACT_KEYS; i++) {
        gp_compact_key churn = {.pipeline = UINT64_C(100000) + i};
        gp_compact_get(&c, &churn);
    }
    CHECK(c.key_overflow > 0 && !c.counts_complete && c.map_dirty,
        "current-window overflow is explicit and successful inactive eviction marks the index dirty");
    gp_compact_reset(&c);
    unsigned tombstones = 0;
    for (unsigned i = 0; i < GP_COMPACT_KEYS * 2; i++) tombstones += c.map[i] == UINT32_MAX;
    CHECK(!c.map_dirty && !tombstones, "dirty reset rebuilds exact key slots and clears every eviction tombstone");
    gp_compact_begin(&c, 601, 4096, 4);
    gp_compact_key fresh = {.pipeline = UINT64_C(100000000)};
    gp_compact_entry *next = gp_compact_get(&c, &fresh);
    CHECK(next && next->id > id && !c.key_overflow && c.nkeys == GP_COMPACT_KEYS,
        "fresh-key lifetime churn evicts inactive dictionary entries without permanent exhaustion");
    gp_compact_observe(&c, &fresh, 1, 1, 0, 0, 1, 4096, 4, 1, 0, 1, 0);
    gp_compact_finish(&c, 0);
    CHECK(!next->v[GC_ELIGIBLE].real && c.domain_excluded_frames == 1,
        "unfinished close never grants completed-frame eligibility");
    gp_compact_begin(&c, 602, 4096, 4);
    gp_compact_observe(&c, &fresh, 0, 0, 0, 0, 0, 4096, 4, UINT64_MAX, 0, 1, 0);
    CHECK(c.aggregate_overflow && !c.counts_complete,
        "counter values beyond exact numeric-array range expose overflow instead of rounding authority");
    gp_compact_begin(&c, 602, 8192, 4);
    gp_compact_finish(&c, 1);
    CHECK(c.frame_overflow && !next->v[GC_ELIGIBLE].real,
        "inconsistent per-frame population snapshots cannot grant eligibility");
    CHECK(sizeof c == 4427008, "compact worker memory bound matches published 64-bit ABI documentation");
    gp_compact_begin(&c, 603, 4096, 4);
    fresh.pipeline++;
    gp_compact_observe(&c, &fresh, 1, 0, 1, 0, 0, 4096, 4, 1, 0, 1, 0);
    gp_compact_entry *discarded = gp_compact_get(&c, &fresh);
    gp_compact_finish(&c, 1);
    CHECK(discarded->selected_discarded == 1 && discarded->v[GC_SELECTED].integer == 0 &&
        discarded->v[GC_DISCARDED].integer == 1 && discarded->v[GC_ELIGIBLE].real == 0,
        "selected discarded-only keys retain output authority without becoming eligible samples");
}
static void unavailable_query_contract(void) {
    if (!V.has_host_query_reset) return;
    VkQueryPool pool;
    VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VKCHECK(vkCreateQueryPool(V.dev, &info, NULL, &pool));
    vkResetQueryPool(V.dev, pool, 0, 2);
    vk_gpuprof_timestamp data[2] = {{UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX}};
    VkResult result = vkGetQueryPoolResults(V.dev, pool, 0, 2, sizeof data, data, sizeof data[0],
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    CHECK(result == VK_NOT_READY && !data[0].available && !data[1].available,
        "real reset-but-unwritten Vulkan queries return availability zero without waiting");
    vk_gpuprof_time_domain domain = {(uintptr_t)V.dev, (uintptr_t)V.queue, V.qfam,
        V.timestamp_bits, V.props.limits.timestampPeriod};
    CHECK(vk_gpuprof_interval_check(data[0], data[1], domain, domain).status == GP_TIME_UNAVAILABLE,
        "real unavailable query payload cannot form a valid span");
    vkDestroyQueryPool(V.dev, pool, NULL);
}
static uint8_t recording[2u << 20];
static size_t used;
static void op(uint32_t code, const void *payload, uint32_t bytes) {
    uint32_t size = (8 + bytes + 7) & ~7u;
    if (used + size > sizeof recording) DIE("GPU profiler fixture recording overflow");
    op_hdr header = {code, size}; memcpy(recording + used, &header, sizeof header);
    if (bytes) memcpy(recording + used + 8, payload, bytes);
    memset(recording + used + 8 + bytes, 0, size - 8 - bytes); used += size;
}
static void shader_binary(unsigned stage, unsigned salt, uint8_t control[0x880], uint8_t code[0x230]) {
    /* Existing test_vkdraw_seed.py salts37/38: position-only VS and four
     * genuinely consumed fragment UBOs. No new shader/compiler convention. */
    memset(control, 0, 0x880);
    for (unsigned i = 0; i < 0x30; i++) code[i] = (uint8_t)salt;
    for (unsigned i = 0; i < 0x200; i++) code[0x30 + i] = (uint8_t)(i * 7 + salt);
    uint32_t header = 0x880, bytes = 0x230;
    memcpy(control + 0x20, &header, 4); memcpy(control + 0x700, &bytes, 4);
    memcpy(control + 0x704, &bytes, 4); memcpy(control + 0x714, &stage, 4);
}
/* Correctness fixtures drain the independent profiler after the renderer's
   fence. Scaled CPU measurements and the backlog regression must not do so. */
static int wait_for_profiler = 1;
static void wait_submission(uint64_t ticket) {
    int completed = ticket && mtl_wait(ticket, UINT64_C(5000000000));
    CHECK(completed, "real GPU fence retires before inspecting output or flushing profiler");
    if (!completed) { fflush(stderr); _Exit(1); }
    if (wait_for_profiler) vk_gpuprof_test_wait();
}
static void submit(int frame_end) {
    op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(recording); used = 0;
    if (frame_end) { vk_end_rendering(); vk_gpuprof_test_frame(vk_cb()); }
    wait_submission(mtl_sub_commit());
}
static int pipeline_groups;
static void record_draws(tex_t *target, prog_t *program, uint8_t *memory, unsigned draws) {
    p_set_rt rt = {.n = 1}; rt.color[0] = target; op(OP_SET_RT, &rt, sizeof rt);
    p_bind_program bind = {program, 3}; op(OP_BIND_PROGRAM, &bind, sizeof bind);
    p_bind_vattr attribute = {.n = 1}; va_st position = {0x16, 0, 0};
    memcpy(&attribute.st[0], &position, sizeof position); op(OP_BIND_VATTR, &attribute, sizeof attribute);
    p_bind_vstream stream = {.n = 1}; stream.st[0].stride = 8; op(OP_BIND_VSTREAM, &stream, sizeof stream);
    p_bind_vbuf vertex = {.n = 1};
    vertex.r[0].address = (NVNbufferAddress)(uintptr_t)(memory + 0x20000); vertex.r[0].size = 64;
    op(OP_BIND_VBUF, &vertex, sizeof vertex);
    for (unsigned slot = 0; slot < 4; slot++) {
        p_bind_buf uniform = {1, slot, (uint64_t)(uintptr_t)(memory + 0x10000 + slot * 0x1000), 1024};
        op(OP_BIND_UBO, &uniform, sizeof uniform);
    }
    p_viewports viewport = {.n = 1}; viewport.v[0][2] = (float)target->d.width;
    viewport.v[0][3] = (float)target->d.height; viewport.v[0][5] = 1;
    op(OP_VIEWPORTS, &viewport, sizeof viewport);
    p_scissor scissor = {0, 0, target->d.width, target->d.height}; op(OP_SCISSOR, &scissor, sizeof scissor);
    /* Keep each target's clear full-area so size is the only key difference,
     * rather than alternating LOAD vs CLEAR attachment operations. */
    p_clear_color clear = {0, 15, {0, 0, 0, 1}}; op(OP_CLEAR_COLOR, &clear, sizeof clear);
    p_draw draw = {4, 0, 3, 0, 0};
    for (unsigned i = 0; i < draws; i++) {
        if (pipeline_groups && draws >= 9) {
            chmask_st channels = {pipeline_groups == 2 ? (i & 1 ? 7 : 15) : i < 3 || i >= 5 ? 15 : 7};
            p_bind_state state = {.kind = 2}; memcpy(state.bytes, &channels, sizeof channels);
            op(OP_BIND_STATE, &state, sizeof state);
        }
        op(OP_DRAW, &draw, sizeof draw);
    }
}
static uint8_t *readback_bytes(tex_t *target, uint8_t *memory) {
    return memory + 0x40000 + (target->d.width == 64 ? 0 : 0x10000);
}
static void record_readback(tex_t *target, uint8_t *memory) {
    p_copy_b2t readback = {.src = (uint64_t)(uintptr_t)readback_bytes(target, memory), .tex = target,
        .r = {0, 0, 0, target->d.width, target->d.height, 1}};
    op(OP_COPY_T2B, &readback, sizeof readback);
}
static void check_pixels(tex_t *target, uint8_t *memory) {
    size_t bad = 0, pixels = (size_t)target->d.width * target->d.height * 4;
    uint8_t *readback = readback_bytes(target, memory);
    for (size_t i = 0; i < pixels; i++) bad += readback[i] != 255;
    CHECK(!bad, "profiled draw produces exact all-white real Vulkan readback at both render extents");
}
static int finish_fixture(unsigned frames, unsigned passes, unsigned draws, int supported) {
    const char *requested = getenv("TOTK_VK_GPU_PROFILE");
    int mode = requested && (!strcmp(requested, "1") || !strcmp(requested, "2") || !strcmp(requested, "3")) ?
        atoi(requested) : 0;
    int workers = getenv("TOTK_VK_REC_THREADS") ? atoi(getenv("TOTK_VK_REC_THREADS")) : 0;
    int stats = getenv("TOTK_VK_GPU_PROFILE_STATS") && !strcmp(getenv("TOTK_VK_GPU_PROFILE_STATS"), "1");
    CHECK(vk_gpuprof_on == mode, "launch-time GPU profile mode remains effective");
    CHECK(!!vk_record_enabled() == !!workers, "GPU profiling does not force requested recorder workers inline");
    CHECK(!!vk_gpuprof_stats_mask() == !!(mode && stats && V.has_pipeline_statistics),
        "statistics are available only when requested and supported");
    if (!mode) {
        uint64_t background[3]; vk_gpuprof_test_background(background);
        CHECK(!vk_gpuprof_pipeline_on, "pipeline subflags cannot enable a disabled master profiler");
        CHECK(!background[0] && !background[1] && !background[2],
            "disabled profiler never queues, processes or drops a query lease");
    }
    if (workers && draws) {
        uint64_t leases[4]; vk_record_test_leases(leases);
        CHECK(leases[1], "parallel recorder secondary leases are actually used for real draws");
        /* GPU retirement publishes returns to each recording owner; recycling
         * happens on that owner's next job, not on the fence-waiting thread.
         * The terminal single-frame case deliberately has no such next job. */
        if (frames > 1) CHECK(leases[2], "subsequent frames recycle fenced secondary leases on their recording owners");
    }
    vk_rec_lock(); vk_gpuprof_test_flush(); vk_rec_unlock();
    vk_gpuprof_shutdown();
    vk_gpuprof_shutdown(); /* normal teardown is idempotent */
    vk_rec_lock();
    uint64_t before = mtl_last_fence(), context[8];
    vk_gpuprof_present();
    CHECK(mtl_last_fence() == before, "late CPU present after shutdown never submits sampled marker work");
    vk_record_test_guest_context(context);
    CHECK(!context[6], "late CPU present after shutdown leaves no open command buffer");
    vk_gpuprof_test_flush(); vk_rec_unlock(); /* late rows cannot follow session_end */
    printf("gpuprof-fixture frames=%u passes=%u draws=%u mode=%d workers=%d stats_supported=%d stats_enabled=%d checks=%u failures=%u\n",
        frames, passes, draws, mode, workers, supported,
        !!vk_gpuprof_stats_mask(), checks, failures);
    return failures != 0;
}
static void area_texture(tex_t *target, pool_t *gpu, size_t *offset,
                         unsigned format, unsigned width, unsigned height, uint64_t serial) {
    target->d = (nvn_texdesc){.target = 1, .format = format, .width = width,
        .height = height, .depth = 1, .levels = 1};
    nvn_tex_layout(&target->d, &target->L);
    *offset = (*offset + 0xffff) & ~(size_t)0xffff;
    if (*offset > gpu->size || target->L.size > gpu->size - *offset) DIE("GPU profiler area fixture texture overflow");
    target->mem = gpu->mem + *offset; target->pool = gpu; target->serial = serial;
    *offset += target->L.size;
}
static uint8_t *area_readback(const tex_t *target, uint8_t *memory) {
    return memory + (target->mem - target->pool->mem);
}
static void record_area_clear(tex_t *colors, tex_t *depth, uint8_t *memory) {
    p_set_rt rt = {.n = colors ? 2 : 0, .depth = depth};
    if (colors) { rt.color[0] = &colors[0]; rt.color[1] = &colors[1]; }
    op(OP_SET_RT, &rt, sizeof rt);
    p_scissor scissor = {0, 0, depth->d.width, depth->d.height}; op(OP_SCISSOR, &scissor, sizeof scissor);
    for (int i = 0; i < rt.n; i++) {
        p_clear_color clear = {i, 15, {i == 0, i == 1, 0, 1}};
        op(OP_CLEAR_COLOR, &clear, sizeof clear);
    }
    p_clear_ds clear = {0.25f, 1, 0, 0}; op(OP_CLEAR_DS, &clear, sizeof clear);
    /* A copy forces pending load-clears into the real dynamic-rendering path.
     * It also proves the supplied NVN metadata describes actual GPU images. */
    for (int i = 0; i <= rt.n; i++) {
        tex_t *target = i < rt.n ? &colors[i] : depth;
        memset(area_readback(target, memory), 0xc7, (size_t)target->d.width * target->d.height * 4);
        p_copy_b2t copy = {.src = (uint64_t)(uintptr_t)area_readback(target, memory), .tex = target,
            .r = {0, 0, 0, target->d.width, target->d.height, 1}};
        op(OP_COPY_T2B, &copy, sizeof copy);
    }
}
static void check_area_pixels(const tex_t *target, uint8_t *memory, int color) {
    const uint8_t *pixels = area_readback(target, memory);
    size_t bad = 0, count = (size_t)target->d.width * target->d.height;
    for (size_t i = 0; i < count; i++) {
        if (color < 0) {
            float depth; memcpy(&depth, pixels + i * 4, sizeof depth); bad += depth != 0.25f;
        } else {
            const uint8_t *p = pixels + i * 4;
            bad += p[0] != (color == 0 ? 255 : 0) || p[1] != (color == 1 ? 255 : 0) || p[2] || p[3] != 255;
        }
    }
    CHECK(!bad, "area fixture real Vulkan color/depth clear has exact readback pixels");
}
static int run_areas(uint8_t *memory, pool_t *gpu, int supported) {
    tex_t scene[2][3] = {0}, fixed[2] = {0}; size_t offset = 0;
    for (unsigned extent = 0; extent < 2; extent++) for (unsigned slot = 0; slot < 3; slot++)
        area_texture(&scene[extent][slot], gpu, &offset, slot == 2 ? 0x34 : 0x25,
            64u << extent, 36u << extent, 86000 + extent * 3 + slot);
    area_texture(&fixed[0], gpu, &offset, 0x34, 360, 360, 86006);
    area_texture(&fixed[1], gpu, &offset, 0x34, 128, 128, 86007);
    for (unsigned frame = 0; frame < 3; frame++) {
        unsigned geometry = frame == 0 ? 1 : frame == 1 ? 2 : 0;
        unsigned prepass = frame < 2, transfers = frame == 0 ? 6 : frame == 1 ? 9 : 2;
        unsigned width = frame < 2 ? 64u << frame : 0, height = frame < 2 ? 36u << frame : 0;
        if (prepass) {
            record_area_clear(NULL, &scene[frame][2], memory);
            record_area_clear(scene[frame], &scene[frame][2], memory);
        }
        /* The smaller MRT comes last: scene selection must take the largest
         * render area, not the most recent pass or the larger fixed shadow.
         * This exact key also has two distinct completed-frame references. */
        if (frame == 1) record_area_clear(scene[0], &scene[0][2], memory);
        record_area_clear(NULL, &fixed[0], memory);
        record_area_clear(NULL, &fixed[1], memory);
        submit(1);
        if (frame < 2) for (unsigned extent = 0; extent <= frame; extent++) {
            for (int color = 0; color < 2; color++) check_area_pixels(&scene[extent][color], memory, color);
            check_area_pixels(&scene[extent][2], memory, -1);
        }
        check_area_pixels(&fixed[0], memory, -1); check_area_pixels(&fixed[1], memory, -1);
        printf("gpuprof-areas-frame frame=%u scene_width=%u scene_height=%u passes=%u prepass=%u geometry=%u shadow=1 other_depth=1 transfers=%u\n",
            frame + 1, width, height, prepass + geometry + 2, prepass, geometry, transfers);
    }
    printf("gpuprof-areas frames=3 passes=11 draws=0 exact_keys=6 families=5 transfers=17\n");
    return finish_fixture(3, 11, 0, supported);
}
static uint64_t benchmark_ns(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static int run_scaled(uint8_t *memory, pool_t *pool, tex_t targets[2], prog_t *program,
                      unsigned frames, unsigned passes, unsigned draws, unsigned textures, int supported) {
    tex_t sampled[160] = {0};
    uint8_t white[4 * 4 * 4]; memset(white, 255, sizeof white);
    samp_par sampler = {.lod_max = 1000}; mtl_reg_samp(1, &sampler);
    for (unsigned i = 0; i < textures; i++) {
        tex_t *t = &sampled[i];
        t->d = (nvn_texdesc){.target = 1, .format = 0x25, .width = 4, .height = 4, .depth = 1, .levels = 1};
        nvn_tex_layout(&t->d, &t->L);
        if (t->L.size > 0x1000) DIE("GPU profiler scaled fixture texture overflow");
        t->mem = memory + 0x80000 + i * 0x1000; t->pool = pool; t->serial = 87000 + i;
        CHECK(!nvn_swizzle(&t->d, 0, 0, white, 4 * 4, t->mem), "scaled bound texture has real swizzled backing");
        mtl_reg_tex((int)i + 1, t, NULL);
    }
    /* Both stages are genuinely active. Fill the 128 fragment slots first,
     * then vertex slots, using ordinary NVN replay payloads (32 per bind).
     * Distinct registry entries and backing ranges exercise registry lookup
     * and prior-pass auditing, not invalid handles or repeated dummy IDs. */
    for (unsigned first = 0; first < textures;) {
        unsigned stage_first = first < 128 ? first : first - 128;
        unsigned count = textures - first; if (count > 32) count = 32;
        p_bind_handles bind = {.stage = first < 128 ? 1 : 0, .first = (int)stage_first, .n = (int)count};
        for (unsigned i = 0; i < count; i++) bind.h[i] = (first + i + 1) | (UINT64_C(1) << 20);
        op(OP_BIND_TEX, &bind, sizeof bind); first += count;
    }
    for (unsigned pass = 0; pass < passes; pass++)
        record_draws(&targets[pass & 1], program, memory, draws / passes + (pass < draws % passes));
    record_readback(&targets[0], memory); record_readback(&targets[1], memory);
    op(OP_END, NULL, 0);
    const size_t tape_bytes = used;
    uint64_t replay_ns = 0, commit_ns = 0, wait_ns = 0, elapsed_start = benchmark_ns();
    for (unsigned frame = 0; frame < frames; frame++) {
        memset(readback_bytes(&targets[0], memory), 0xc7, 64 * 64 * 4);
        memset(readback_bytes(&targets[1], memory), 0xc7, 128 * 128 * 4);
        uint64_t begin = benchmark_ns();
        mtl_sub_begin(); mtl_exec(recording);
        vk_end_rendering(); vk_gpuprof_test_frame(vk_cb());
        uint64_t replay_end = benchmark_ns(), ticket = mtl_sub_commit(), commit_end = benchmark_ns();
        wait_submission(ticket);
        uint64_t wait_end = benchmark_ns();
        replay_ns += replay_end - begin; commit_ns += commit_end - replay_end; wait_ns += wait_end - commit_end;
        check_pixels(&targets[0], memory); check_pixels(&targets[1], memory);
    }
    used = 0;
    printf("gpuprof-scaled frames=%u passes_per_frame=%u draws_per_frame=%u bound_textures=%u "
        "tape_bytes=%zu replay_ns=%llu commit_ns=%llu wait_ns=%llu elapsed_ns=%llu\n",
        frames, passes, draws, textures, tape_bytes, (unsigned long long)replay_ns,
        (unsigned long long)commit_ns, (unsigned long long)wait_ns,
        (unsigned long long)(benchmark_ns() - elapsed_start));
    return finish_fixture(frames, frames * passes, frames * draws, supported);
}
/* A host event holds actual GPU work, not query availability or profiler
 * counters. The watchdog releases it even if a broken present hook waits for
 * the GPU, so that regression fails a CHECK rather than hanging teardown. */
typedef struct {
    VkEvent event;
    PFN_vkSetEvent set_event;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int release, timed_out, shutdown_timeout;
} present_gate;
static void *release_present_gate(void *opaque) {
    present_gate *gate = opaque;
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 2;
    pthread_mutex_lock(&gate->mu);
    while (!gate->release) {
        int result = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        if (result == ETIMEDOUT) { gate->timed_out = 1; break; }
        if (result) DIE("GPU profiler present gate wait failed: %d", result);
    }
    pthread_mutex_unlock(&gate->mu);
    VKCHECK(gate->set_event(V.dev, gate->event));
    return NULL;
}
static void *release_shutdown_gate(void *opaque) {
    present_gate *gate = opaque;
    struct timespec delay = gate->shutdown_timeout ? (struct timespec){3, 0} : (struct timespec){0, 100000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
    VKCHECK(gate->set_event(V.dev, gate->event));
    return NULL;
}
static int published_frame(unsigned number) {
    glob_t paths = {0};
    if (glob("gpuprof-*.jsonl", 0, NULL, &paths)) { globfree(&paths); return 0; }
    int found = 0;
    char expected[64]; snprintf(expected, sizeof expected, "\"type\":\"frame\",\"frame\":%u,", number);
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        FILE *file = fopen(paths.gl_pathv[i], "r");
        if (!file) continue;
        char *line = NULL; size_t capacity = 0;
        while (getline(&line, &capacity, file) >= 0) found |= strstr(line, expected) != NULL;
        free(line); fclose(file);
    }
    globfree(&paths);
    return found;
}
static int run_sealed_present(uint8_t *memory, tex_t targets[2], prog_t programs[2],
                              unsigned frames, int supported, int shutdown_inflight) {
    const char *kind, *why, *driver = getenv("TOTK_VK_DRIVER");
    if (driver && (!*driver || !strcmp(driver, "system"))) driver = NULL;
    PFN_vkGetInstanceProcAddr gipa = vk_load_driver(driver, &kind, &why);
    if (!gipa) DIE("Vulkan event fixture driver: %s", why);
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)gipa(V.inst, "vkGetDeviceProcAddr");
    if (!vkGetDeviceProcAddr) DIE("Vulkan event fixture requires device entrypoint lookup");
    PFN_vkCreateEvent create_event = (PFN_vkCreateEvent)vkGetDeviceProcAddr(V.dev, "vkCreateEvent");
    PFN_vkDestroyEvent destroy_event = (PFN_vkDestroyEvent)vkGetDeviceProcAddr(V.dev, "vkDestroyEvent");
    PFN_vkResetEvent reset_event = (PFN_vkResetEvent)vkGetDeviceProcAddr(V.dev, "vkResetEvent");
    PFN_vkCmdWaitEvents wait_events = (PFN_vkCmdWaitEvents)vkGetDeviceProcAddr(V.dev, "vkCmdWaitEvents");
    present_gate gate = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
        .set_event = (PFN_vkSetEvent)vkGetDeviceProcAddr(V.dev, "vkSetEvent")};
    if (!create_event || !destroy_event || !reset_event || !wait_events || !gate.set_event)
        DIE("GPU profiler real present fixture requires Vulkan host event entrypoints");
    VkEventCreateInfo info = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VKCHECK(create_event(V.dev, &info, NULL, &gate.event));
    VkCommandPool cancel_pool;
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qfam};
    VKCHECK(vkCreateCommandPool(V.dev, &pool_info, NULL, &cancel_pool));
    VkCommandBuffer cancel_cb;
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cancel_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &allocate, &cancel_cb));
    const char *sampling = getenv("TOTK_VK_GPU_PROFILE_SAMPLE");
    unsigned completed_closes = 0, pending_closes = 0, pre_uploads = 0;
    uint64_t initial_ticket = mtl_last_fence(), marker_submissions = 0;
    for (unsigned frame = 0; frame < frames; frame++) {
        /* Two independently fenced main submissions, each with genuine pre
         * upload work nested while its main rendering record is still open.
         * The final backend-only submission is deliberately NOT a new frame. */
        for (unsigned part = 0; part < 2; part++) {
            memset(readback_bytes(&targets[part], memory), 0xc7, (64u << part) * (64u << part) * 4);
            record_draws(&targets[part], &programs[frame & 1], memory, part ? 3 : 1);
            op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(recording); used = 0;
            VkBuffer buffer; VkDeviceSize offset;
            uint32_t *upload = vk_transient(256, 256, &buffer, &offset);
            memset(upload, 0, 256);
            VkCommandBuffer pre = vk_pre_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(pre, "upload");
            uint32_t pattern = 0x13570000u + frame * 2 + part;
            vkCmdFillBuffer(pre, buffer, offset, 256, pattern);
            pre_uploads++;
            if (vk_gpuprof_on) vk_gpuprof_scope_end(pre);
            record_readback(&targets[part], memory);
            op(OP_END, NULL, 0); mtl_exec(recording); used = 0;
            wait_submission(mtl_sub_commit());
            unsigned bad = 0;
            for (unsigned i = 0; i < 64; i++) bad += upload[i] != pattern;
            CHECK(!bad, "sealed-present pre upload executes without losing its open main parent");
            check_pixels(&targets[part], memory);
        }
        int pending = shutdown_inflight || (frame & 1);
        vk_rec_lock();
        VkCommandBuffer cb = vk_cb();
        VkBuffer buffer; VkDeviceSize offset;
        uint32_t *present = vk_transient(256, 256, &buffer, &offset);
        memset(present, 0, 256);
        if (pending) {
            VKCHECK(reset_event(V.dev, gate.event));
            wait_events(VK_SECTION_CB(cb), 1, &gate.event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, NULL, 0, NULL, 0, NULL);
        }
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "present");
        vkCmdFillBuffer(cb, buffer, offset, 256, 0x24680000u + frame);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
        uint64_t final_ticket = vk_submit_now(), context[8];
        vk_record_test_guest_context(context);
        CHECK(!context[6], "backend present seals the last main command buffer before the CPU hook");
        vk_rec_unlock();
        pthread_t releaser;
        if (shutdown_inflight) {
            gate.shutdown_timeout = shutdown_inflight == 2;
            vk_rec_lock(); vk_gpuprof_present(); vk_rec_unlock();
            uint64_t last = mtl_last_fence();
            int was_pending = !mtl_wait(final_ticket, 0);
            CHECK(was_pending, "normal shutdown begins with actual event-gated GPU work in flight");
            int result = pthread_create(&releaser, NULL, release_shutdown_gate, &gate);
            if (result) { VKCHECK(gate.set_event(V.dev, gate.event)); DIE("shutdown gate thread: %d", result); }
            vk_gpuprof_shutdown(); /* no prior profiler flush or fence wait */
            int drained = mtl_wait(last, 0);
            CHECK(drained == !!(vk_gpuprof_on && !gate.shutdown_timeout),
                "enabled shutdown drains GPU work, timeout/off shutdown leaves it outstanding");
            printf("gpuprof-shutdown-inflight pending_before=%d drained_after=%d\n", was_pending, drained);
            pthread_join(releaser, NULL);
            CHECK(mtl_wait(last, UINT64_C(5000000000)), "released shutdown fixture work eventually retires");
            unsigned bad = 0;
            for (unsigned i = 0; i < 64; i++) bad += present[i] != 0x24680000u + frame;
            CHECK(!bad, "shutdown fence drain preserves real present GPU results");
            destroy_event(V.dev, gate.event, NULL);
            vkDestroyCommandPool(V.dev, cancel_pool, NULL);
            pthread_cond_destroy(&gate.cv); pthread_mutex_destroy(&gate.mu);
            return finish_fixture(1, 2, 4, supported);
        }
        if (pending) {
            CHECK(!mtl_wait(final_ticket, 0), "CPU close precedes real GPU completion of event-gated final submission");
            gate.release = gate.timed_out = 0;
            int result = pthread_create(&releaser, NULL, release_present_gate, &gate);
            if (result) {
                VKCHECK(gate.set_event(V.dev, gate.event));
                DIE("GPU profiler present gate thread failed: %d", result);
            }
            /* Exercise profiler lease cancellation independently of optional
             * renderer admission: that API deliberately trylocks g_mu and may
             * reject transient submit/completion contention despite free slots.
             * This real CB is never submitted; its canceled lease must reach
             * the reporter before the older GPU-blocked submitted lease. */
            VKCHECK(vkResetCommandPool(V.dev, cancel_pool, 0));
            VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VKCHECK(vkBeginCommandBuffer(cancel_cb, &begin));
            vk_gpuprof_slot *cancelled = NULL;
            vk_rec_lock();
            vk_gpuprof_slot_begin(&cancelled, cancel_cb);
            CHECK(!vk_gpuprof_on || cancelled,
                "pending sealed frame acquires a real independent profiler lease for cancellation");
            VKCHECK(vkEndCommandBuffer(cancel_cb));
            vk_gpuprof_slot_cancel(&cancelled);
            CHECK(!cancelled, "unsubmitted profiler lease cancellation transfers its ownership");
            vk_rec_unlock();
            vk_gpuprof_test_wait();
            CHECK(!mtl_wait(final_ticket, 0), "discarded optional lease cannot complete the older GPU fence");
            pending_closes++;
        } else {
            wait_submission(final_ticket);
            uint64_t counts[3]; vk_gpuprof_test_background(counts);
            CHECK(counts[0] == counts[1] && !counts[2],
                "fast profiler consumes every fenced lease BEFORE CPU frame closure");
            completed_closes++;
        }
        vk_rec_lock();
        uint64_t before = mtl_last_fence();
        vk_gpuprof_present(); /* current == NULL: production Android ordering */
        uint64_t after = mtl_last_fence();
        /* slot_begin initializes the opt-in profiler lazily, so its effective
         * mode is authoritative only after the real submissions above. */
        int sampled = vk_gpuprof_on && sampling && atoi(sampling) > 0;
        CHECK(after == before + sampled,
            "sealed CPU present submits no full-mode work and exactly one sampled end-marker slot");
        marker_submissions += after - before;
        vk_record_test_guest_context(context);
        CHECK(!context[6], "CPU present leaves no unsubmitted marker or open main command buffer");
        vk_rec_unlock();
        if (pending) {
            pthread_mutex_lock(&gate.mu); gate.release = 1;
            pthread_cond_signal(&gate.cv); pthread_mutex_unlock(&gate.mu);
            pthread_join(releaser, NULL);
            CHECK(!gate.timed_out, "CPU frame closure returns without waiting for event-gated GPU work");
        }
        wait_submission(after);
        unsigned bad = 0;
        for (unsigned i = 0; i < 64; i++) bad += present[i] != 0x24680000u + frame;
        CHECK(!bad, "backend present scope measures actual GPU work with fence-visible results");
    }
    /* No new frame, profiler test flush, shutdown, or fabricated marker can
     * make this row appear: the CPU closure itself must wake the worker. */
    if (vk_gpuprof_on) CHECK(published_frame(frames),
        "last CPU-closed frame is published before next-frame recording or terminal flush");
    uint64_t submissions = mtl_last_fence() - initial_ticket;
    printf("gpuprof-sealed-present frames=%u main_submissions=%llu pre_uploads=%u completed_closes=%u "
        "pending_closes=%u marker_submissions=%llu last_closed_before_flush=1\n",
        frames, (unsigned long long)(submissions - marker_submissions), pre_uploads, completed_closes,
        pending_closes, (unsigned long long)marker_submissions);
    destroy_event(V.dev, gate.event, NULL);
    vkDestroyCommandPool(V.dev, cancel_pool, NULL);
    pthread_cond_destroy(&gate.cv); pthread_mutex_destroy(&gate.mu);
    return finish_fixture(frames, frames * 2, frames * 4, supported);
}
static int run_unavailable_span(int supported) {
    /* Both command buffers are real. Only the first is submitted, so the
     * reserved frame/slot end queries must remain unavailable after its fence. */
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = V.qfam};
    VKCHECK(vkCreateCommandPool(V.dev, &pi, NULL, &pool));
    VkCommandBuffer cb[2];
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(cb[0], &bi)); VKCHECK(vkBeginCommandBuffer(cb[1], &bi));
    vk_gpuprof_slot *slot = NULL;
    vk_rec_lock();
    vk_gpuprof_slot_begin(&slot, cb[0]);
    vk_gpuprof_test_frame(cb[1]); vk_gpuprof_slot_end(slot, cb[1]); vk_gpuprof_slot_seal(slot);
    vk_rec_unlock();
    VKCHECK(vkEndCommandBuffer(cb[0])); VKCHECK(vkEndCommandBuffer(cb[1]));
    VkFence fence; VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(V.dev, &fi, NULL, &fence));
    VkCommandBufferSubmitInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb[0]};
    VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
    VKCHECK(vkQueueSubmit2(V.queue, 1, &submit, fence));
    VKCHECK(vkWaitForFences(V.dev, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    vk_rec_lock(); vk_gpuprof_slot_complete(&slot); vk_rec_unlock();
    vkDestroyFence(V.dev, fence, NULL); vkDestroyCommandPool(V.dev, pool, NULL);
    printf("gpuprof-unavailable expected_frames=1 expected_invalid_spans=1\n");
    return finish_fixture(1, 0, 0, supported);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--compact-fixture"))
        return vk_gpuprof_test_compact_run(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--record-fixture"))
        return vk_gpuprof_test_record_run();
    if (argc == 2 && !strcmp(argv[1], "--sink-contract"))
        return vk_gpuprof_test_sink_run();
    timestamp_contract();
    pipeline_contract();
    compact_contract();
    if (argc == 2 && !strcmp(argv[1], "--compact-contract")) {
        CHECK(!vk_gpuprof_test_report_contract(),
            "frozen compact reports preserve snapshot data and bound each completed-frame pump");
        printf("gpuprof-compact-contract checks=%u failures=%u\n", checks, failures);
        return failures != 0;
    }
    unsigned frames = 302, passes = 180, draws = 2000, textures = 16;
    int areas = argc == 2 && !strcmp(argv[1], "--areas");
    int unavailable_span = argc == 2 && !strcmp(argv[1], "--unavailable-span");
    int sampling = argc == 2 && !strcmp(argv[1], "--sampling");
    int scaled = argc > 1 && !strcmp(argv[1], "--scaled");
    int backpressure = argc == 2 && !strcmp(argv[1], "--backpressure");
    int cancel = argc == 2 && !strcmp(argv[1], "--cancel");
    int sealed_present = argc == 2 && !strcmp(argv[1], "--sealed-present");
    int last_closed = argc == 2 && !strcmp(argv[1], "--last-closed");
    int shutdown_inflight = argc == 2 && !strcmp(argv[1], "--shutdown-inflight") ? 1 :
        argc == 2 && !strcmp(argv[1], "--shutdown-timeout") ? 2 : 0;
    pipeline_groups = argc == 2 && !strcmp(argv[1], "--pipeline-groups") ? 1 :
        argc == 2 && !strcmp(argv[1], "--pipeline-overflow") ? 2 : 0;
    if (pipeline_groups == 2) frames = 2;
    if (scaled || (argc == 3 && !strcmp(argv[1], "--frames"))) {
        for (int i = scaled ? 2 : 1; i < argc; i += 2) {
            if (i + 1 == argc) return 2;
            char *end; unsigned long value = strtoul(argv[i + 1], &end, 10);
            if (!*argv[i + 1] || *end || value > 10000) return 2;
            if (!strcmp(argv[i], "--frames") && value) frames = (unsigned)value;
            else if (scaled && !strcmp(argv[i], "--textures") && value <= 160) textures = (unsigned)value;
            else if (scaled && !strcmp(argv[i], "--passes") && value >= 2 && value <= 1000) passes = (unsigned)value;
            else if (scaled && !strcmp(argv[i], "--draws") && value) draws = (unsigned)value;
            else return 2;
        }
        if (scaled && draws < passes) return 2;
    } else if (argc != 1 && !areas && !unavailable_span && !sampling && !backpressure && !cancel &&
               !sealed_present && !last_closed && !shutdown_inflight && !pipeline_groups) return 2;
    /* Force short secondary chunks so workers=2 tests a real parallel pass,
     * rather than merely checking an enabled worker setting on inline draws. */
    setenv("TOTK_VK_REC_CHUNK_DRAWS", scaled ? "16" : "1", 1);
    setenv("TOTK_VK_REC_LEAD_DRAWS", "1", 1);
    setenv("TOTK_VK_REC_INLINE_DRAWS", "0", 1); setenv("TOTK_VK_CHUNK_DRAWS", "0", 1);
    setenv("TOTK_POST_AA", "off", 1);
    const size_t bytes = 4u << 20;
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("gpuprof fixture mmap"); return 1; }
    wait_for_profiler = !scaled && !backpressure;
    mtl_init();
    unavailable_query_contract();
    int supported = V.has_pipeline_statistics;
    if (getenv("GPUPROF_TEST_UNSUPPORTED")) V.has_pipeline_statistics = 0;
    if (unavailable_span) return run_unavailable_span(supported);
    if (backpressure) { frames = 40; vk_gpuprof_test_pause(1); }
    if (cancel) {
        frames = 4;
        for (unsigned i = 0; i < 32; i++) {
            int opened = vk_submit_optional_begin();
            CHECK(opened, "unsubmitted optional slot can be claimed for profiler lease cancellation");
            if (opened) vk_submit_optional_cancel();
            vk_gpuprof_test_wait();
        }
    }
    mtl_pool_add(memory, bytes);
    pool_t pool = {.mem = memory, .size = bytes,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_t gpu = pool; gpu.mem = memory + (2u << 20); gpu.size = 2u << 20;
    gpu.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    if (areas) return run_areas(memory, &gpu, supported);
    tex_t targets[2]; memset(targets, 0, sizeof targets);
    for (unsigned i = 0; i < 2; i++) {
        targets[i].d = (nvn_texdesc){.target = 1, .format = 0x25, .width = 64u << i,
            .height = 64u << i, .depth = 1, .levels = 1};
        nvn_tex_layout(&targets[i].d, &targets[i].L);
        targets[i].mem = gpu.mem + i * 0x40000; targets[i].pool = &gpu; targets[i].serial = 84000 + i;
    }
    static uint8_t control[2][0x880]; prog_t programs[2]; memset(programs, 0, sizeof programs);
    for (unsigned copy = 0; copy < 2; copy++) {
        programs[copy].nstage = 2; programs[copy].serial = 85000 + copy;
        for (unsigned stage = 0; stage < 2; stage++) {
            uint8_t *code = memory + copy * 0x2000 + stage * 0x1000;
            shader_binary(stage, stage ? 38 : 37, control[stage], code);
            programs[copy].sd[stage].control = control[stage];
            programs[copy].sd[stage].data = (NVNbufferAddress)(uintptr_t)code;
        }
    }
    for (unsigned slot = 0; slot < 4; slot++) {
        float *uniform = (float *)(memory + 0x10000 + slot * 0x1000);
        for (unsigned i = 0; i < 256; i++) uniform[i] = 1;
    }
    const float triangle[] = {-1, -1, 3, -1, -1, 3}; memcpy(memory + 0x20000, triangle, sizeof triangle);
    if (scaled) return run_scaled(memory, &pool, targets, &programs[0], frames, passes, draws, textures, supported);
    static uint8_t compute_control[0x880]; prog_t compute = {.nstage = 1, .serial = 85999};
    if (pipeline_groups == 1) {
        shader_binary(5, 3, compute_control, memory + 0x8000);
        compute.sd[0].control = compute_control;
        compute.sd[0].data = (NVNbufferAddress)(uintptr_t)(memory + 0x8000);
    }
    if (sealed_present || last_closed || shutdown_inflight)
        return run_sealed_present(memory, targets, programs, last_closed || shutdown_inflight ? 1 : 302,
                                  supported, shutdown_inflight);
    unsigned expected_draws = 0, expected_passes = 0;
    for (unsigned frame = 0; frame < frames; frame++) {
        /* Sampling needs stable keys at every frame ordinal, not one key on
         * odd frames and another on even frames: that would alias rotation. */
        tex_t *target = NULL; unsigned draws = 0;
        for (unsigned part = 0; part < (sampling ? 2u : 1u); part++) {
            unsigned index = sampling ? part : frame & 1;
            target = &targets[index]; draws = index ? 3 : 1;
            if (pipeline_groups) draws = pipeline_groups == 2 ? 2049 : 9;
            record_draws(target, &programs[(frame / 2) & 1], memory, draws);
            record_readback(target, memory);
            expected_draws += draws; expected_passes++;
        }
        if (frame % 11 == 7) {
            /* A frame spanning two actual submissions must have one frame row,
             * no lost first segment and pass ordinals independent of slot reuse. */
            submit(0); record_draws(target, &programs[(frame / 2) & 1], memory, draws);
            record_readback(target, memory);
            expected_draws += draws; expected_passes++;
        }
        if (pipeline_groups == 1) {
            p_bind_program bind = {&compute, 1u << 5};
            p_bind_buf output = {5, 0, (uint64_t)(uintptr_t)(memory + 0x1c0000), 256};
            p_dispatch dispatch = {1, 1, 1};
            op(OP_BIND_PROGRAM, &bind, sizeof bind); op(OP_BIND_UBO, &output, sizeof output);
            op(OP_DISPATCH, &dispatch, sizeof dispatch);
        }
        submit(1);
        check_pixels(target, memory);
        if (sampling) check_pixels(&targets[0], memory);
        if (pipeline_groups == 1) {
            uint32_t *result = (uint32_t *)(memory + 0x1c0000);
            for (unsigned i = 0; i < 64; i++)
                CHECK(result[i] == (i ^ 0x12345678u), "pipeline compute timing preserves real dispatch output");
        }
    }
    if (backpressure) {
        uint64_t counts[3]; vk_gpuprof_test_background(counts);
        CHECK(counts[0] == 32 && counts[1] == 0 && counts[2] > 0,
            "walker and GPU completion progress while all bounded profiler leases are paused");
        printf("gpuprof-backpressure frames=%u queued=%llu processed=%llu dropped=%llu\n",
            frames, (unsigned long long)counts[0], (unsigned long long)counts[1], (unsigned long long)counts[2]);
        vk_gpuprof_test_pause(0); vk_gpuprof_test_wait(); wait_for_profiler = 1;
    }
    /* Record a genuine pre upload while the main render record is OPEN. The
     * uploader must restore that parent rather than losing its timestamp/key/
     * statistics. Two frame markers then share one unsubmitted main buffer. */
    memset(memory + 0x40000, 0xc7, 0x20000);
    record_draws(&targets[0], &programs[0], memory, 1);
    op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(recording); used = 0;
    VkBuffer upload_buffer; VkDeviceSize upload_offset;
    uint32_t *upload = vk_transient(256, 256, &upload_buffer, &upload_offset);
    memset(upload, 0, 256);
    VkCommandBuffer pre = vk_pre_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(pre, "upload");
    vkCmdFillBuffer(pre, upload_buffer, upload_offset, 256, 0x13579bdf);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(pre);
    if (pipeline_groups == 1) {
        p_draw continuation = {4, 0, 3, 0, 0};
        op(OP_DRAW, &continuation, sizeof continuation);
    }
    record_readback(&targets[0], memory);
    op(OP_END, NULL, 0); mtl_exec(recording); used = 0;
    vk_end_rendering(); vk_gpuprof_test_frame(vk_cb());
    record_draws(&targets[1], &programs[1], memory, 3); record_readback(&targets[1], memory);
    op(OP_END, NULL, 0); mtl_exec(recording); used = 0;
    vk_end_rendering(); vk_gpuprof_test_frame(vk_cb());
    wait_submission(mtl_sub_commit());
    unsigned bad_upload = 0;
    for (unsigned i = 0; i < 64; i++) bad_upload += upload[i] != 0x13579bdf;
    CHECK(!bad_upload, "direct profiled pre scope really fills bytes before the open main rendering executes");
    check_pixels(&targets[0], memory); check_pixels(&targets[1], memory);
    frames += 2; expected_passes += 2; expected_draws += pipeline_groups == 1 ? 5 : 4;
    return finish_fixture(frames, expected_passes, expected_draws, supported);
}

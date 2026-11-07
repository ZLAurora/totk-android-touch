/* Draws through the whole Vulkan backend without the game: shaders found in a seed by their NVN keys, a texture uploaded
   from block-linear guest memory, a render target, a clear, a textured draw with uniforms and a vertex buffer, and a
   GPU copy of the result back into a memory pool, checked pixel by pixel.
     python3 android/gfx/test_vkdraw_seed.py /tmp/vkdraw-seed       (glslangValidator, on the PC)
     TOTK_SHADER_SEED_DIR=/tmp/vkdraw-seed ./vkdrawtest              (lavapipe here; the device with TOTK_VK_DRIVER) */
#include "nvn_dirty.h"
#include "vk_int.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_overlay.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include "test_lag.h"
#include "../../runtime/frame_timeline.h"
#include "../../runtime/guest_clock.h"
#ifdef TOTK_ANDROID_APP
#include <android/native_window.h>
#endif
#ifdef TOTK_VK_TESTS
#include "test_vkcall_counts.h"
int vk_draw_test_xlate_frame(uint64_t frame);
#endif
/* The host frontend has no async replay queue. The optional single-threaded handoff deterministically replays
   a newer producer during drain, before invalidate requests recording ownership, rather than inventing a fake GPU. */
static _Atomic uint64_t test_drain_calls;
static _Atomic(void (*)(void *)) test_drain_hook; static void *test_drain_arg;
void nvn_x_drain(void) {
    atomic_fetch_add(&test_drain_calls, 1);
    void (*hook)(void *) = atomic_exchange(&test_drain_hook, NULL);
    if (hook) hook(test_drain_arg);
}

void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n);
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static uint8_t rec[1 << 20]; static size_t rn;
static void op(uint32_t code, const void *payload, uint32_t n) {
    uint32_t size = (8 + n + 7) & ~7u; op_hdr h = {code, size};
    if (rn + size > sizeof rec) DIE("test recording exceeds its fixed buffer");
    memcpy(rec + rn, &h, 8); if (n) memcpy(rec + rn + 8, payload, n); memset(rec + rn + 8 + n, 0, size - 8 - n); rn += size;
}
static uint64_t run(void) { op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(rec); rn = 0; return mtl_sub_commit(); }
static uint64_t recorder_maps(uint64_t *slabs, uint64_t *anonymous) {
    FILE *file = fopen("/proc/self/maps", "re"); char line[1024]; uint64_t count = 0; *slabs = *anonymous = 0;
    while (file && fgets(line, sizeof line, file)) {
        unsigned long lo, hi; char protection[5]; unsigned long long offset, inode; unsigned major, minor;
        if (sscanf(line, "%lx-%lx %4s %llx %x:%x %llu", &lo, &hi, protection, &offset, &major, &minor, &inode) != 7) continue;
        count++; *slabs += strstr(line, "[anon:totk-rec-") != NULL; *anonymous += !inode;
    }
    if (file) fclose(file); return count;
}
static VKAPI_ATTR VkResult VKAPI_CALL fail_map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
        VkDeviceSize size, VkMemoryMapFlags flags, void **data) { return VK_ERROR_MEMORY_MAP_FAILED; }
static VKAPI_ATTR VkResult VKAPI_CALL fail_allocate(VkDevice device, const VkMemoryAllocateInfo *info,
        const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
static PFN_vkEndCommandBuffer fault_real_end;
static VKAPI_ATTR VkResult VKAPI_CALL fail_recorder_end(VkCommandBuffer cb) {
    const char *role = nvn_dirty_lag_thread_role_get();
    return role && !strcmp(role, "recorder") ? VK_ERROR_MEMORY_MAP_FAILED : fault_real_end(cb);
}
typedef struct { VkEvent gate; _Atomic int returned; int timed_out; } pressure_return;
static void *pressure_return_run(void *arg) {
    pressure_return *p = arg;
    for (int i = 0; i < 1000 && atomic_load_explicit(&p->returned, memory_order_acquire) != 2; i++) usleep(1000);
    p->timed_out = atomic_load_explicit(&p->returned, memory_order_acquire) != 2;
    VKCHECK(vkSetEvent(V.dev, p->gate));
    return NULL;
}
typedef struct { VkEvent gate; _Atomic int release; int timed_out; } lease_gate;
static void *lease_gate_run(void *arg) {
    lease_gate *p = arg;
    for (int i = 0; i < 20000 && !atomic_load_explicit(&p->release, memory_order_acquire); i++) usleep(1000);
    p->timed_out = !atomic_load_explicit(&p->release, memory_order_acquire);
    VKCHECK(vkSetEvent(V.dev, p->gate)); return NULL;
}
typedef struct { uint64_t ticket, active; int held, completed; } lease_owner_probe;
static void *lease_owner_probe_run(void *arg) {
    lease_owner_probe *p = arg;
    for (int i = 0; i < 10000 && vk_record_test_submit_held() != p->ticket; i++) usleep(1000);
    p->held = vk_record_test_submit_held() == p->ticket;
    p->completed = mtl_wait(p->ticket, 5000000000ull);
    uint64_t leases[4]; vk_record_test_leases(leases); p->active = leases[0];
    vk_record_test_hold_submit(0); return NULL;
}

/* the same synthetic shader binaries as test_vkdraw_seed.py */
static void shader_binary(int stage, int salt, uint8_t control[0x880], uint8_t *code, uint32_t *len) {
    memset(control, 0, 0x880);
    *len = 0x230;
    for (int i = 0; i < 0x30; i++) code[i] = (uint8_t)salt;
    for (int i = 0; i < 0x200; i++) code[0x30 + i] = (uint8_t)((i * 7 + salt) & 255);
    uint32_t v = 0x880; memcpy(control + 0x20, &v, 4);
    memcpy(control + 0x700, len, 4); memcpy(control + 0x704, len, 4);
    uint32_t st = (uint32_t)stage; memcpy(control + 0x714, &st, 4);
}

/* Opt-in, fresh-process CPU recording benchmark. It uses the ordinary NVN stream, real Vulkan commands and public
   pool/submission APIs. Test getters observe existing profiling/reuse counters and walking-side lag decisions.
   New lag statistics are header-gated: these same fixture sources also compile against signed 89c0735 without any
   production-baseline edits. No present calls: they reset the profiler's 300-frame window and invalidate deltas. */
enum { WALK_SETS = 24, WALK_POOLS = 13, WALK_CHUNKS = 12, WALK_REPEATS = 20,
       WALK_DRAWS = WALK_SETS * WALK_CHUNKS * WALK_REPEATS, WALK_STREAM_BYTES = 256 * 1024 };
typedef struct {
    uint8_t *cpu, *alias;
    p_bind_buf ubo[2][4], storage[5];
    p_bind_vbuf vertex;
    p_draw_idx index;
} walk_bank;
typedef struct {
    p_set_rt target;
    p_copy_b2t readback;
    p_bind_program program[2];
    p_bind_program pointer_program;
    p_bind_vattr attr;
    p_bind_vstream stream;
    p_viewports viewport;
} walk_fixture;
typedef struct {
    uint64_t ns[VK_REC_N], cpu, wall, seal, wait, draws, freeze_hits, program_hits;
    int frames;
    uint64_t descriptors[8], api[3], wakes[4];
} walk_measure;
static uint64_t walk_clock(clockid_t clock) {
    struct timespec t; if (clock_gettime(clock, &t)) DIE("walking benchmark clock_gettime failed");
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* Driver-level descriptor calls; nonzero only when TOTK_VK_TEST_CALL_COUNTS installed the counting wrappers. */
static void walk_api_counts(uint64_t out[3]) {
    out[0] = atomic_load(&count_vkAllocateDescriptorSets); out[1] = atomic_load(&call_sets);
    out[2] = atomic_load(&count_vkResetDescriptorPool);
}
static void walk_tint(int frame, int set, int binding, int storage, float color[4]) {
    color[0] = (float)((frame + set + binding + storage) % 5 + 1) / 8;
    color[1] = (float)((3 * set + binding + storage) % 5 + 1) / 8;
    color[2] = (float)(binding + 1) / 8; color[3] = 1;
}
static void walk_expected(int frame, int set, float red, int produced, float color[4]) {
    memset(color, 0, 4 * sizeof *color);
    for (int storage = 0; storage < 2; storage++) for (int k = 0; k < 4; k++) {
        float c[4]; walk_tint(frame, set, k, storage, c);
        if (!storage && k == 0) c[0] = red;
        if (!storage && k == 1 && produced) { c[0] = .875f; c[1] = .25f; c[2] = .75f; }
        for (int component = 0; component < 4; component++) color[component] += c[component] * .125f;
    }
}
static int walk_pixel(const uint8_t *pixel, const float color[4]) {
    for (int i = 0; i < 4; i++) if (abs(pixel[i] - (int)(color[i] * 255 + .5f)) > 1) return 0;
    return 1;
}
static void walk_bank_bindings(walk_bank *b, uint8_t *cpu, uint8_t *alias) {
    memset(b, 0, sizeof *b); b->cpu = cpu; b->alias = alias;
    for (int st = 0; st < 2; st++) for (int k = 0; k < 4; k++) {
        uint8_t *base = st == 1 && k == 2 ? alias : cpu;
        b->ubo[st][k] = (p_bind_buf){st, k, (uint64_t)(uintptr_t)(base + st * 0x10000 + k * 0x200), 512};
    }
    for (int k = 0; k < 4; k++) b->storage[k] = (p_bind_buf){
        1, k, (uint64_t)(uintptr_t)((k & 1 ? alias : cpu) + 0x20000 + k * 0x100), 16};
    /* A real four-byte writer lies INSIDE the broad 512-byte UBO, but after its 16-byte logical tint. */
    b->storage[4] = (p_bind_buf){1, 4, (uint64_t)(uintptr_t)(cpu + 0x10100), 4};
    b->vertex.n = 4;
    for (int k = 0; k < 4; k++) b->vertex.r[k] = (NVNbufferRange){(uint64_t)(uintptr_t)(cpu + 0x30000 + k * 0x100), 64};
    b->index = (p_draw_idx){.prim = 4, .itype = 1, .count = 3, .instances = 1,
        .ibuf = (uint64_t)(uintptr_t)(cpu + 0x38000)};
}
static void walk_bank_fill(walk_bank *b, int frame, int set) {
    /* All prior actual tickets have completed before this next-frame CPU rewrite. Host pre-write is the same
       public hook used for emulated guest writes, not a test dirty-generation or copy-classification override. */
    dm_guest_write_prepare(b->cpu, 0x40000); memset(b->cpu, 0, 0x40000);
    const float full[] = {-1, -1, 3, -1, -1, 3};
    const uint16_t indices[] = {0, 1, 2, 0, 1, 2};
    memcpy(b->cpu + 0x30000, full, sizeof full); memcpy(b->cpu + 0x38000, indices, sizeof indices);
    for (int k = 0; k < 4; k++) {
        float color[4]; walk_tint(frame, set, k, 0, color); memcpy(b->cpu + 0x10000 + k * 0x200, color, sizeof color);
        walk_tint(frame, set, k, 1, color); memcpy(b->cpu + 0x20000 + k * 0x100, color, sizeof color);
    }
    float alternate[4]; walk_tint(frame, set, 3, 0, alternate);
    memcpy(b->cpu + 0x14600, alternate, sizeof alternate); /* alternate FS slot3, same logical colour initially */
    const float producer[] = {.875f, .25f, .75f, 1};
    memcpy(b->cpu + 0x25000, producer, sizeof producer);
    /* +0x26000 / +0x27000 are genuinely GPU-copied degenerate vertices / indices, not recorder markers. */
}
static void walk_setup(const walk_fixture *f) {
    p_scissor all = {0, 0, (int32_t)f->viewport.v[0][2], (int32_t)f->viewport.v[0][3]};
    p_clear_color black = {0, 0xf, {0, 0, 0, 1}};
    op(OP_SET_RT, &f->target, sizeof f->target); op(OP_SCISSOR, &all, sizeof all); op(OP_CLEAR_COLOR, &black, sizeof black);
    op(OP_BIND_VATTR, &f->attr, sizeof f->attr); op(OP_BIND_VSTREAM, &f->stream, sizeof f->stream);
    op(OP_VIEWPORTS, &f->viewport, sizeof f->viewport);
}
static void walk_bind(const walk_fixture *f, const walk_bank *b, int writable) {
    op(OP_BIND_PROGRAM, &f->program[writable], sizeof f->program[0]);
    for (int st = 0; st < 2; st++) for (int k = 0; k < 4; k++) op(OP_BIND_UBO, &b->ubo[st][k], sizeof b->ubo[st][k]);
    for (int k = 0; k < 5; k++) op(OP_BIND_SSBO, &b->storage[k], sizeof b->storage[k]);
    op(OP_BIND_VBUF, &b->vertex, offsetof(p_bind_vbuf, r) + 4 * sizeof b->vertex.r[0]);
}
static void walk_draw(const walk_bank *b, int x, int y, int longer) {
    p_scissor pixel = {x, y, 1, 1}; p_draw_idx index = b->index; if (longer) index.count = 6;
    op(OP_SCISSOR, &pixel, sizeof pixel); op(OP_DRAW_IDX, &index, sizeof index);
}
static void walk_exec(void) { op(OP_END, NULL, 0); mtl_exec(rec); rn = 0; }
static void walk_red(const walk_bank *b, float red) {
    struct { uint64_t dst, n; float value; uint32_t pad; } update = {b->ubo[1][0].addr, 4, red, 0};
    op(OP_UPDATE_UB, &update, sizeof update);
}
static void walk_copy(uint64_t src, uint64_t dst, uint64_t size) {
    const uint64_t copy[] = {src, dst, size}; op(OP_COPY_B2B, copy, sizeof copy);
}
static float walk_churn_red(int frame, int set, int group) {
    return (float)((frame + set + group + 2) % 5 + 1) / 8;
}
static void walk_churn(const walk_bank *b, int frame, int set, int group, int repeat) {
    if (repeat < 7) return;
    p_bind_buf vertex = b->ubo[0][0], fragment = b->ubo[1][3];
    if (repeat & 1) { vertex.addr += 0x4000; fragment.addr += 0x4000; }
    op(OP_BIND_UBO, &vertex, sizeof vertex); op(OP_BIND_UBO, &fragment, sizeof fragment);
    if (repeat == 7) {
        struct { uint64_t dst, n; float value[4]; } update = {vertex.addr, 16, {-.015625f * (group % 2 + 1), 0, 0, 0}};
        op(OP_UPDATE_UB, &update, sizeof update); /* consumed VS displacement; tiny scissors remain inside the triangle */
        update.dst = fragment.addr; walk_tint(frame, set, 3, 0, update.value);
        update.value[0] = walk_churn_red(frame, set, group); op(OP_UPDATE_UB, &update, sizeof update);
    }
}
/* A failed guard may legitimately wait inside commit, so neither the GPU gate nor the writer can depend on
   commit returning. Observe the actual QueueSubmit2 watermark, never the earlier walking seal generation. */
typedef struct {
    void *uniform; VkEvent gate; uint64_t ticket, faults, hosts; int host, waited;
} walk_gate_reuse;
static void *walk_gate_reuse_run(void *arg) {
    walk_gate_reuse *r = arg; uint64_t ns;
    nvn_dirty_lag_thread_role("walk-postqueue");
    for (;;) {
        uint64_t reserved, queued; vk_record_test_submit_status(&reserved, &queued);
        if (queued >= r->ticket) break;
        sched_yield();
    }
    nvn_dirty_lag_stats(&r->faults, &r->hosts, &ns);
    r->waited = lag_test_rewrite(r->uniform, 4, 0, r->host, r->gate);
    return NULL;
}
/* Never rescue a stuck fixture by publishing a synthetic ticket or opening an unobserved submission's event. */
typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv; int done; const char *phase;
} walk_liveness;
static void *walk_liveness_timeout(void *arg) {
    walk_liveness *w = arg;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 30;
    pthread_mutex_lock(&w->mu);
    while (!w->done) if (pthread_cond_timedwait(&w->cv, &w->mu, &until) && !w->done) {
        fprintf(stderr, "FAIL: lag-walk strict safety timed out during %s\n", w->phase);
        fflush(stderr); _Exit(1);
    }
    pthread_mutex_unlock(&w->mu); return NULL;
}
static void walk_liveness_phase(walk_liveness *w, const char *phase) {
    pthread_mutex_lock(&w->mu); w->phase = phase; pthread_mutex_unlock(&w->mu);
}
static void walk_safety(const walk_fixture *f, walk_bank *b, uint8_t *readback) {
    walk_liveness watch = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
        .phase = "same-binding physical/alias CPU rewrite"};
    pthread_t watchdog; if (pthread_create(&watchdog, NULL, walk_liveness_timeout, &watch)) abort();
    float before[4], after[4]; walk_bank_fill(b, 0, 0);
    walk_expected(0, 0, .125f, 0, before); walk_expected(0, 0, .875f, 0, after);
    /* No covering submit snapshot here: the second identical binding must recapture changed CPU contents.
       Write through a genuinely shared alias while the descriptor reads the physical VA. */
    mtl_sub_begin(); walk_setup(f); walk_bind(f, b, 0); walk_draw(b, 0, 0, 0); walk_exec();
    if (!vk_lag_uniform_private()) {
        vk_submit_chunk(); CHECK(mtl_wait(mtl_last_fence(), 30000000000ull), "lag-off alias probe first draw completes before CPU reuse");
    }
    uint64_t faults0, hosts0, ns, faults1, hosts1; nvn_dirty_lag_stats(&faults0, &hosts0, &ns);
    nvn_dirty_protect(b->alias + 0x10000, 16); dm_guest_write_prepare(b->alias + 0x10000, 16);
    const float rewritten = .875f; memcpy(b->alias + 0x10000, &rewritten, sizeof rewritten);
    op(OP_BIND_UBO, &b->ubo[1][0], sizeof b->ubo[1][0]); walk_draw(b, 1, 0, 1);
    op(OP_COPY_T2B, &f->readback, sizeof f->readback); walk_exec();
    CHECK(mtl_wait(mtl_sub_commit(), 30000000000ull), "same-binding physical/alias CPU rewrite completes");
    nvn_dirty_lag_stats(&faults1, &hosts1, &ns);
    CHECK(walk_pixel(readback, before) && walk_pixel(readback + 4, after), "same binding captures distinct pre/post alias CPU bytes");
    CHECK(!vk_lag_uniform_private() || (faults0 == faults1 && hosts0 == hosts1), "private UBO alias rewrite adds zero reuse waits");

    /* An eager, owned submit capture must outlive a CPU rewrite before replay as well as gated GPU execution.
       Other shader inputs reside on different dirty units; only this UBO is rewritten before opening the gate. */
    if (vk_lag_uniform_private()) for (int host = 0; host < 2; host++) {
        walk_liveness_phase(&watch, host ? "owned snapshot and gated host UBO reuse" : "owned snapshot and gated fault UBO reuse");
        walk_bank_fill(b, 0, 0); rn = 0; walk_setup(f); walk_bind(f, b, 0); walk_draw(b, 0, 0, 0);
        op(OP_COPY_T2B, &f->readback, sizeof f->readback); op(OP_END, NULL, 0);
        uint64_t handle = (uint64_t)(uintptr_t)rec;
        void *owned = mtl_sub_capture_ubos(&handle, 1);
        CHECK(owned != NULL, "gated UBO probe owns its submit-time snapshot before replay");
        const float newer = .5f;
        dm_guest_write_prepare(b->cpu + 0x10000, sizeof newer); memcpy(b->cpu + 0x10000, &newer, sizeof newer);
        VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, owned);
        vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
        mtl_exec(rec); rn = 0;
        uint64_t ticket = mtl_last_fence() + 1, guard_ticket = vk_record_lag_ticket();
        CHECK(!guard_ticket || guard_ticket == ticket, "wait-mode guard ticket names the upcoming actual fence; copy mode has no guard ticket");
        walk_gate_reuse reuse = {.uniform = b->cpu + 0x10000, .gate = gate, .ticket = ticket, .host = host};
        pthread_t opener; if (pthread_create(&opener, NULL, walk_gate_reuse_run, &reuse)) abort();
        uint64_t fence = mtl_sub_commit();
        pthread_join(opener, NULL);
        uint64_t reserved, queued; vk_record_test_submit_status(&reserved, &queued);
        CHECK(reserved == ticket && queued == ticket && fence == ticket, "gated UBO probe observes its one actual queued ticket");
        CHECK(!reuse.waited, "private UBO %s writer completes before gated GPU execution", host ? "host" : "fault");
        CHECK(mtl_wait(fence, 5000000000ull), "gated private UBO draw completes");
        nvn_dirty_lag_stats(&faults1, &hosts1, &ns);
        CHECK(reuse.faults == faults1 && reuse.hosts == hosts1, "gated private UBO reuse adds zero fault/host waits");
        CHECK(walk_pixel(readback, before), "owned private UBO retains submit-time pixels through pre-replay and postqueue CPU rewrites");
        vkDestroyEvent(V.dev, gate, NULL);
    }

    /* Same writable binding across an overlapping partial inline update and a genuine GPU producer.
       The tiny writable neighbour must be discovered before readonly UBO selection / beginRendering. */
    walk_liveness_phase(&watch, "writable neighbour and GPU-copy producer");
    walk_bank_fill(b, 0, 0); rn = 0; walk_setup(f); walk_bind(f, b, 1);
    walk_red(b, .875f); walk_draw(b, 0, 0, 0);
    walk_copy((uint64_t)(uintptr_t)(b->cpu + 0x25000), b->ubo[1][1].addr, 16);
    walk_red(b, .25f); walk_draw(b, 1, 0, 1);
    op(OP_COPY_T2B, &f->readback, sizeof f->readback);
    CHECK(mtl_wait(run(), 30000000000ull), "broad UBO and unchanged smaller writable neighbour complete");
    walk_expected(0, 0, .875f, 0, before); walk_expected(0, 0, .25f, 1, after);
    CHECK(walk_pixel(readback, before) && walk_pixel(readback + 4, after), "writable alias preserves inline neighbours and newer GPU-produced UBO bytes");
    CHECK(*(uint32_t *)(b->cpu + 0x10100) == 0x89abcde1u, "real fragment storage atomic writes the smaller neighbour");

    /* A mutable untracked parent is NOT made authoritative by its tracked, cold child. No submit snapshot or
       dirty-generation transition may mask this dependency: the second identical binding must reread the pointer. */
    walk_liveness_phase(&watch, "untracked storage-pointer parent and distinct tracked children");
    walk_bank_fill(b, 0, 0);
    uint64_t child0 = (uint64_t)(uintptr_t)(b->cpu + 0x20000), child1 = child0 + 0x100;
    volatile uint64_t parent = child0;
    p_bind_buf pointer = {1, 0, (uint64_t)(uintptr_t)&parent, sizeof parent};
    CHECK(!nvn_dirty_tracked((const void *)&parent, sizeof parent), "storage-pointer parent is genuinely untracked");
    CHECK(!vk_lag_capture_on() || (!nvn_dirty_lag_hot((const void *)(uintptr_t)child0, 16) &&
        !nvn_dirty_lag_hot((const void *)(uintptr_t)child1, 16)), "storage-pointer children are tracked-cold inputs");
    CHECK(nvn_dirty_tracked((const void *)(uintptr_t)child0, 16) && nvn_dirty_tracked((const void *)(uintptr_t)child1, 16),
        "storage-pointer children retain real tracking authority");
    uint64_t serial = vk_submit_serial();
    rn = 0; walk_setup(f); walk_bind(f, b, 0); op(OP_BIND_PROGRAM, &f->pointer_program, sizeof f->pointer_program);
    op(OP_BIND_UBO, &pointer, sizeof pointer); walk_draw(b, 0, 0, 0);
    mtl_sub_begin(); walk_exec();
    uint64_t generation = nvn_dirty_now(); parent = child1;
    CHECK(nvn_dirty_now() == generation, "untracked parent rewrite introduces no synthetic global dirty transition");
    op(OP_BIND_UBO, &pointer, sizeof pointer); walk_draw(b, 1, 0, 1);
    op(OP_COPY_T2B, &f->readback, sizeof f->readback); walk_exec();
    uint64_t fence = mtl_sub_commit();
    CHECK(vk_submit_serial() == serial + 1, "untracked parent rewrite stays inside one actual submission");
    CHECK(mtl_wait(fence, 30000000000ull), "untracked storage-pointer parent draw pair completes");
    walk_tint(0, 0, 0, 1, before); walk_tint(0, 0, 1, 1, after);
    CHECK(walk_pixel(readback, before) && walk_pixel(readback + 4, after),
        "identical bindings reread untracked storage-pointer parent and select the second tracked child");
    pthread_mutex_lock(&watch.mu); watch.done = 1; pthread_cond_signal(&watch.cv); pthread_mutex_unlock(&watch.mu);
    pthread_join(watchdog, NULL); pthread_cond_destroy(&watch.cv); pthread_mutex_destroy(&watch.mu);
}
static void walk_print_profile(const char *label, const uint64_t ns[VK_REC_N], double draws) {
    uint64_t walking = 0;
    for (int k = 0; k < VK_REC_N; k++) if (k != VK_REC_O && (k < VK_REC_O_SLOT)) walking += ns[k];
    printf("lag-walk %s profile ns/draw: walking=%.2f touch=%.2f vertex_index=%.2f resolve=%.2f snapshot=%.2f GPUwrite=%.2f "
           "bind=%.2f uniform=%.2f submit=%.2f queue=%.2f GPUwait=%.2f workerwait=%.2f execute=%.2f assembly=%.2f\n",
        label, walking / draws, ns[VK_REC_S_TOUCH] / draws, ns[VK_REC_VERTEX] / draws, ns[VK_REC_RESOLVE] / draws,
        ns[VK_REC_S_SNAPSHOT] / draws, ns[VK_REC_S_WRITE] / draws, ns[VK_REC_BIND] / draws, ns[VK_REC_S_UNIFORM] / draws,
        ns[VK_REC_O_SUBMIT] / draws, ns[VK_REC_O_QUEUE] / draws, ns[VK_REC_O_GPU_WAIT] / draws,
        ns[VK_REC_O_WORKER_WAIT] / draws, ns[VK_REC_O_EXECUTE] / draws, ns[VK_REC_O_ASSEMBLE] / draws);
}
static void walk_print_hits(const char *label, uint64_t draws, uint64_t freeze_hits, uint64_t program_hits, int recorder) {
    printf("lag-walk %s observed counters: accepted_draws=%llu freeze_hits=%llu program_hits=%llu",
        label, (unsigned long long)draws, (unsigned long long)freeze_hits, (unsigned long long)program_hits);
    if (recorder && draws)
        printf(" nonempty_stage_inputs=%llu input_normalized_freeze_hit=%.2f%% (two stage inputs/draw; direct miss counter unavailable)\n",
            (unsigned long long)(draws * 2), 100.0 * freeze_hits / (double)(draws * 2));
    else printf(" input_normalized_freeze_hit=N/A (recorder disabled or no accepted draws)\n");
}

/* The device-shaped case deliberately separates immutable cold rings from three genuinely CPU-reused hot rings.
   Each 64KiB lane holds many small bindings in ONE pool span; changing a raw address need not change its dirty unit.
   Two alternating hot offsets/group are not reused in later groups, bounding intended hot payload to 288 ranges /
   243.4KiB per frame without relying on the backend's particular memo hash or inventing hot classifications.
   Every draw owns a distinct pixel, unlike the repeated-pixel stable/churn cases above. */
enum { DEVICE_WIDTH = 96, DEVICE_HEIGHT = 64, DEVICE_HOT_SETS = 3, DEVICE_COLD_SLOTS = 48,
       DEVICE_HOT_SLOTS = WALK_CHUNKS * 2, DEVICE_BANK_BYTES = 0x40000, DEVICE_SLOT_BYTES = 1280 };
static size_t walk_device_vertex_offset(int stream, int slot) {
    /* The 16-byte lane skew also avoids making all three streams artificial exact-memo hash synonyms. */
    return (size_t)stream * 0x10000 + (size_t)slot * DEVICE_SLOT_BYTES + (size_t)stream * 16;
}
static size_t walk_device_vertex_size(int stream) { return 1024 + (size_t)stream * 128; }
static size_t walk_device_index_offset(int slot) { return 0x30000 + (size_t)slot * 16 + 8; }
static int walk_device_slot(int set, int chunk, int repeat) {
    return set < DEVICE_HOT_SETS ? chunk * 2 + (repeat & 1) : (chunk * WALK_REPEATS + repeat) % DEVICE_COLD_SLOTS;
}
static void walk_device_color(int version, int set, int slot, int triangle, float color[4]) {
    color[0] = (float)((version + set + slot + triangle) % 7 + 1) / 8;
    color[1] = (float)((set * 3 + slot * 2 + triangle * 3) % 7 + 1) / 8;
    color[2] = (float)((version * 3 + set + slot * 3 + triangle * 5) % 7 + 1) / 8;
    color[3] = 1;
}
static int walk_device_units(uint8_t *banks, size_t *unit_bytes) {
    uintptr_t base, end;
    if (!nvn_dirty_lag_bounds(banks, 1, &base, &end)) return 0;
    *unit_bytes = end - base;
    uint8_t seen[WALK_SETS * DEVICE_BANK_BYTES / 4096] = {0}; int count = 0;
    for (int set = 0; set < WALK_SETS; set++) {
        int slots = set < DEVICE_HOT_SETS ? DEVICE_HOT_SLOTS : DEVICE_COLD_SLOTS;
        for (int slot = 0; slot < slots; slot++) for (int stream = 0; stream < 4; stream++) {
            size_t off = stream < 3 ? walk_device_vertex_offset(stream, slot) : walk_device_index_offset(slot);
            size_t size = stream < 3 ? walk_device_vertex_size(stream) : 3 * sizeof(uint16_t);
            uintptr_t lo, hi;
            if (!nvn_dirty_lag_bounds(banks + (size_t)set * DEVICE_BANK_BYTES + off, size, &lo, &hi)) continue;
            for (; lo < hi; lo += *unit_bytes) {
                size_t unit = (lo - base) / *unit_bytes;
                if (unit >= sizeof seen) DIE("device-shaped dirty units smaller than supported system pages");
                if (!seen[unit]) { seen[unit] = 1; count++; }
            }
        }
    }
    return count;
}
static void walk_device_fill(uint8_t *bank, int set, int version, int tracked_write) {
    const float full[] = {-1, -1, 3, -1, -1, 3};
    int slots = set < DEVICE_HOT_SETS ? DEVICE_HOT_SLOTS : DEVICE_COLD_SLOTS;
    for (int slot = 0; slot < slots; slot++) {
        for (int stream = 0; stream < 3; stream++) {
            uint8_t *dst = bank + walk_device_vertex_offset(stream, slot);
            size_t size = walk_device_vertex_size(stream);
            if (tracked_write) dm_guest_write_prepare(dst, size);
            for (size_t vertex = 0; vertex < size / (2 * sizeof(float)); vertex++) {
                float value[2], color[4]; walk_device_color(version, set, slot, (int)(vertex / 3), color);
                value[0] = stream ? color[stream == 1 ? 0 : 2] : full[(vertex % 3) * 2];
                value[1] = stream ? color[stream == 1 ? 1 : 3] : full[(vertex % 3) * 2 + 1];
                memcpy(dst + vertex * sizeof value, value, sizeof value);
            }
        }
        /* All indices reference actual initialized vertices; changing contents selects a different coloured triangle. */
        uint16_t first = (uint16_t)(((slot * 7 + version) % 40) * 3);
        const uint16_t index[] = {first, (uint16_t)(first + 1), (uint16_t)(first + 2)};
        uint8_t *dst = bank + walk_device_index_offset(slot);
        if (tracked_write) dm_guest_write_prepare(dst, sizeof index);
        memcpy(dst, index, sizeof index);
    }
}
static int walk_device_hot_ranges(uint8_t *banks) {
    int hot = 0;
    for (int set = 0; set < DEVICE_HOT_SETS; set++) for (int slot = 0; slot < DEVICE_HOT_SLOTS; slot++) {
        uint8_t *bank = banks + (size_t)set * DEVICE_BANK_BYTES;
        for (int stream = 0; stream < 3; stream++)
            hot += !!nvn_dirty_lag_hot(bank + walk_device_vertex_offset(stream, slot), walk_device_vertex_size(stream));
        hot += !!nvn_dirty_lag_hot(bank + walk_device_index_offset(slot), 3 * sizeof(uint16_t));
    }
    return hot;
}
#ifdef VK_LAG_TEST_STATS_VERSION
static void walk_device_stats_add(vk_lag_test_stats *out, const vk_lag_test_stats *before, const vk_lag_test_stats *after) {
    for (int kind = 0; kind < VK_LAG_KIND_COUNT; kind++) {
        vk_lag_kind_stats *d = &out->kind[kind];
        const vk_lag_kind_stats *a = &before->kind[kind], *b = &after->kind[kind];
#define ADD(field) d->field += b->field - a->field
        ADD(decisions); ADD(memo_hits); ADD(memo_misses); ADD(cold_guards); ADD(hot_decisions);
        ADD(sample_count); ADD(sample_ns); ADD(hot_copies); ADD(hot_bytes); ADD(hot_ns);
        ADD(snapshot_allocations); ADD(snapshot_bytes);
        ADD(binding_hits); ADD(binding_misses); ADD(key_range); ADD(key_ticket); ADD(key_mapping); ADD(key_snapshot);
        ADD(class_hits); ADD(class_misses);
#if VK_LAG_TEST_STATS_VERSION >= 2
        ADD(metadata_cross_ticket); ADD(metadata_cross_snapshot); ADD(unit_cross_ticket); ADD(hot_refreshes);
        ADD(copy_hits); ADD(copy_misses); ADD(copy_ticket_misses); ADD(copy_snapshot_misses); ADD(copy_content_misses);
        ADD(uniform_snapshot_binds); ADD(uniform_cover_hits); ADD(uniform_cover_misses);
#endif
        for (int bucket = 0; bucket < 5; bucket++) d->size_buckets[bucket] += b->size_buckets[bucket] - a->size_buckets[bucket];
        d->max_size = b->max_size; /* cumulative process maximum cannot be subtracted into a per-frame maximum */
#undef ADD
    }
}
static void walk_device_print_stats(const char *label, const vk_lag_test_stats *stats, int frames) {
    static const char *names[VK_LAG_KIND_COUNT] = {
        [VK_LAG_VERTEX] = "vertex", [VK_LAG_INDEX] = "index", [VK_LAG_STORAGE] = "storage",
        [VK_LAG_UNIFORM] = "uniform", [VK_LAG_POINTER] = "pointer", [VK_LAG_TEXEL] = "texel",
        [VK_LAG_COPY] = "copy", [VK_LAG_OTHER] = "other"};
    for (int kind = 0; kind < VK_LAG_KIND_COUNT; kind++) {
        const vk_lag_kind_stats *s = &stats->kind[kind];
        if (!s->decisions && !s->snapshot_allocations) continue;
        printf("lag-walk %s lag-kind=%s decisions=%llu memo_hits=%llu memo_misses=%llu cold_guards=%llu hot_decisions=%llu "
               "classification_samples=%llu classification_sample_ns=%llu hot_copies=%llu hot_bytes=%llu hot_ns=%llu "
               "snapshot_allocations=%llu snapshot_bytes=%llu frames=%d hot_copies_per_frame=%.2f hot_KiB_per_frame=%.2f\n", label, names[kind],
            (unsigned long long)s->decisions, (unsigned long long)s->memo_hits, (unsigned long long)s->memo_misses,
            (unsigned long long)s->cold_guards, (unsigned long long)s->hot_decisions,
            (unsigned long long)s->sample_count, (unsigned long long)s->sample_ns,
            (unsigned long long)s->hot_copies, (unsigned long long)s->hot_bytes, (unsigned long long)s->hot_ns,
            (unsigned long long)s->snapshot_allocations, (unsigned long long)s->snapshot_bytes, frames,
            s->hot_copies / (double)frames, s->hot_bytes / (1024.0 * frames));
        printf("lag-walk %s lag-kind=%s binding_hits=%llu binding_misses=%llu key_range=%llu key_ticket=%llu key_mapping=%llu "
               "key_snapshot=%llu class_hits=%llu class_misses=%llu size_buckets_256_4K_64K_cap_over=%llu,%llu,%llu,%llu,%llu "
               "process_cumulative_max_bytes=%llu (key reasons nonexclusive)\n", label, names[kind],
            (unsigned long long)s->binding_hits, (unsigned long long)s->binding_misses,
            (unsigned long long)s->key_range, (unsigned long long)s->key_ticket,
            (unsigned long long)s->key_mapping, (unsigned long long)s->key_snapshot,
            (unsigned long long)s->class_hits, (unsigned long long)s->class_misses,
            (unsigned long long)s->size_buckets[0], (unsigned long long)s->size_buckets[1],
            (unsigned long long)s->size_buckets[2], (unsigned long long)s->size_buckets[3],
            (unsigned long long)s->size_buckets[4], (unsigned long long)s->max_size);
#if VK_LAG_TEST_STATS_VERSION >= 2
        printf("lag-walk %s lag-kind=%s metadata_cross_ticket=%llu metadata_cross_snapshot=%llu unit_cross_ticket=%llu "
               "hot_refreshes=%llu copy_hits=%llu copy_misses=%llu copy_ticket_misses=%llu copy_snapshot_misses=%llu "
               "copy_content_misses=%llu uniform_snapshot_binds=%llu uniform_cover_hits=%llu uniform_cover_misses=%llu\n",
            label, names[kind], (unsigned long long)s->metadata_cross_ticket, (unsigned long long)s->metadata_cross_snapshot,
            (unsigned long long)s->unit_cross_ticket, (unsigned long long)s->hot_refreshes,
            (unsigned long long)s->copy_hits, (unsigned long long)s->copy_misses,
            (unsigned long long)s->copy_ticket_misses, (unsigned long long)s->copy_snapshot_misses,
            (unsigned long long)s->copy_content_misses, (unsigned long long)s->uniform_snapshot_binds,
            (unsigned long long)s->uniform_cover_hits, (unsigned long long)s->uniform_cover_misses);
#endif
    }
}
#endif
static void walk_device_print_hits(const char *label, uint64_t draws, uint64_t freeze_hits, uint64_t program_hits) {
    printf("lag-walk %s observed counters: accepted_draws=%llu freeze_hits=%llu program_hits=%llu "
           "(device shaders have no descriptor inputs; no stage-input-normalized hit-rate claim)\n",
        label, (unsigned long long)draws, (unsigned long long)freeze_hits, (unsigned long long)program_hits);
}
static void walk_device_bench(const walk_fixture *original, uint8_t *output, int frames, uint64_t first_frame, int recorder) {
    const size_t alignment = 0x10000, data_size = (size_t)WALK_SETS * DEVICE_BANK_BYTES;
    const size_t pool_size = data_size + alignment, mapping_size = pool_size + alignment;
    uint8_t *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED, "reserve device-shaped walking data");
    if (mapping == MAP_FAILED) return;
    uint8_t *banks = (uint8_t *)(((uintptr_t)mapping + alignment - 1) & ~(uintptr_t)(alignment - 1));
    /* Initialization precedes tracking: the cold phase cannot inherit fabricated write history from setup. */
    for (int set = 0; set < WALK_SETS; set++)
        walk_device_fill(banks + (size_t)set * DEVICE_BANK_BYTES, set, 0, 0);
    mtl_pool_add(banks, pool_size); nvn_dirty_add_range(banks, data_size);
    uint8_t control[2][0x880]; prog_t program = {.nstage = 2, .serial = 3236};
    for (int stage = 0; stage < 2; stage++) {
        uint8_t code[0x230]; uint32_t length; shader_binary(stage, 35 + stage, control[stage], code, &length);
        uint8_t *dst = banks + data_size + stage * 0x1000; memcpy(dst, code, length);
        program.sd[stage].data = (uint64_t)(uintptr_t)dst; program.sd[stage].control = control[stage];
    }
    tex_t target = *original->target.color[0]; target.d.width = DEVICE_WIDTH; target.d.height = DEVICE_HEIGHT;
    target.serial = 3237; nvn_tex_layout(&target.d, &target.L);
    walk_fixture f = *original; f.target.color[0] = &target; f.readback.tex = &target;
    f.readback.r = (NVNcopyRegion){0, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 1};
    f.viewport.v[0][2] = DEVICE_WIDTH; f.viewport.v[0][3] = DEVICE_HEIGHT; f.attr.n = f.stream.n = 3;
    p_bind_program bind_program = {&program, 3};
    /* Size the separate stream from its actual payloads, including aligned opcode headers and generous setup space.
       Neither the old 256KiB allocation nor rec[] is silently overrun when per-draw vertex/index bindings are added. */
    const size_t vertex_payload = offsetof(p_bind_vbuf, r) + 3 * sizeof(NVNbufferRange);
    const size_t draw_bytes = ((8 + vertex_payload + 7) & ~(size_t)7) +
        ((8 + sizeof(p_scissor) + 7) & ~(size_t)7) + ((8 + sizeof(p_draw_idx) + 7) & ~(size_t)7);
    const size_t stream_capacity = WALK_SETS * WALK_REPEATS * draw_bytes + 4096;
    CHECK(stream_capacity <= sizeof rec, "device-shaped stream capacity fits recording scratch");
    if (stream_capacity > sizeof rec) { nvn_dirty_remove_range(banks); mtl_pool_del(banks); munmap(mapping, mapping_size); return; }
    uint8_t *streams[WALK_CHUNKS]; uint64_t handles[WALK_CHUNKS];
    for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) {
        streams[chunk] = malloc(stream_capacity); if (!streams[chunk]) DIE("device-shaped walking stream allocation failed");
        handles[chunk] = (uint64_t)(uintptr_t)streams[chunk];
    }
    const char *names[] = {"device-cold", "device-training", "device-learned-hot", "device-chunk-stress"};
    walk_measure measured[4] = {0};
#ifdef VK_LAG_TEST_STATS_VERSION
    vk_lag_test_stats measured_stats[4] = {0};
#endif
    const char *mode = getenv("TOTK_VK_LAG_MODE"), *hybrid_env = getenv("TOTK_VK_LAG_HYBRID");
    int wait_mode = vk_lag_capture_on() && (!mode || strcmp(mode, "copy"));
    int hybrid = wait_mode && !(hybrid_env && !atoi(hybrid_env));
    const char *cap_env = getenv("TOTK_VK_LAG_HOT_MAX_KB"); unsigned long cap_kb = cap_env ? strtoul(cap_env, NULL, 10) : 256;
    uint64_t hot_cap = cap_kb && cap_kb <= 1048576 ? (uint64_t)cap_kb * 1024 : 256u * 1024;
    int hot_streams = 0;
    for (int stream = 0; stream < 3; stream++) hot_streams += walk_device_vertex_size(stream) <= hot_cap;
    printf("lag-walk device layout: %d indexed draws/frame, THREE consumed vertex selections/draw plus changing U16 index VA; "
           "one fresh 6MiB input pool span / 24 banks, 3 x 64KiB vertex lanes + 64KiB index lane/bank; "
           "vertex ranges 1024/1152/1280B, index range 6B, lane offsets skewed 16B; stream_capacity=%zu bytes/group "
           "effective_recorder=%d hot_cap_bytes=%llu eligible_hot_vertex_streams=%d\n", WALK_DRAWS, stream_capacity, recorder,
        (unsigned long long)hot_cap, hot_streams);
    printf("lag-walk device locality: 21 cold banks rotate 48 distinct offsets (240 draws/bank); 3 hot banks alternate "
           "two offsets/group, 24 distinct offsets/frame: intended hot copies=288 ranges / 249264 bytes (243.4KiB), "
           "ACTUAL copies/memo collisions reported below. Each draw has a unique checked pixel on a 96x64 target.\n");
    printf("lag-walk device history: 2 unmodified cold warmup + %d measured cold frames; TWO real submitted training frames rewrite only initialized "
           "small hot ranges with dm_guest_write_prepare; %d learned-hot frames continue real CPU writes after each completed "
           "prior ticket; two separately reported 12-actual-chunk stress frames. No manual watch/hot toggle or synthetic ticket.\n",
        frames, frames);
    size_t unit_bytes = 0; int units = walk_device_units(banks, &unit_bytes);
    printf("lag-walk device limits: observed_unique_input_units=%d unit_bytes=%zu (bounds unavailable when lag is off); "
           "default64KiB gives 96 units vs device ~2381. Dirty-unit env settings remain unchanged. NO GPU-written hot neighbours "
           "in this isolated small-read case (device had 112/512/4480B neighbours); old stable/churn GPU producers and strict "
           "writable-neighbour probes remain separate. Scissors, geometry, shaders and locality are synthetic, not a captured "
           "full scene. No concurrent CPU/GPU reuse or frame-time prediction; effective_hybrid=%d\n", units, unit_bytes, hybrid);
#ifndef VK_LAG_TEST_STATS_VERSION
    printf("lag-walk device raw lag classification/hot-copy stats unavailable in this baseline header; "
           "the IDENTICAL fixture still reports CPU/profile, hot history, sticky counters and checked pixels.\n");
#endif
    const int cold_warmup = 2, cold_frames = cold_warmup + frames, total_frames = frames * 2 + cold_warmup + 4;
    for (int frame = 0; frame < total_frames; frame++) {
        int phase = frame < cold_frames ? 0 : frame < cold_frames + 2 ? 1 : frame < cold_frames + 2 + frames ? 2 : 3;
        int sample = frame - (phase == 0 ? 0 : phase == 1 ? cold_frames : phase == 2 ? cold_frames + 2 : cold_frames + 2 + frames);
        int warming = phase == 0 && sample < cold_warmup, stress = phase == 3;
        int version = phase ? frame - cold_frames + 1 : 0;
        uint64_t rearm0, update0, promotion0, demotion0, rearm1, update1, promotion1, demotion1;
        nvn_dirty_lag_sticky_stats(&rearm0, &update0, &promotion0, &demotion0);
        nvn_dirty_lag_frame(first_frame + (uint64_t)frame);
        if (phase) for (int set = 0; set < DEVICE_HOT_SETS; set++)
            walk_device_fill(banks + (size_t)set * DEVICE_BANK_BYTES, set, version, 1);
        int observed_hot = walk_device_hot_ranges(banks);
        CHECK(!wait_mode || observed_hot == ((phase == 0 || (phase == 1 && !sample)) ? 0 : DEVICE_HOT_SETS * DEVICE_HOT_SLOTS * 4),
            "device-shaped %s frame %d observes genuine cold/two-writing-frame hot history (%d hot ranges)",
            names[phase], sample, observed_hot);
        for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) {
            rn = 0; if (!chunk) { walk_setup(&f); op(OP_BIND_PROGRAM, &bind_program, sizeof bind_program); }
            for (int set = 0; set < WALK_SETS; set++) for (int repeat = 0; repeat < WALK_REPEATS; repeat++) {
                int slot = walk_device_slot(set, chunk, repeat), draw = (chunk * WALK_SETS + set) * WALK_REPEATS + repeat;
                uint8_t *bank = banks + (size_t)set * DEVICE_BANK_BYTES;
                p_bind_vbuf vertex = {.n = 3};
                for (int stream = 0; stream < 3; stream++) vertex.r[stream] = (NVNbufferRange){
                    (uint64_t)(uintptr_t)(bank + walk_device_vertex_offset(stream, slot)), walk_device_vertex_size(stream)};
                p_scissor pixel = {draw % DEVICE_WIDTH, draw / DEVICE_WIDTH, 1, 1};
                p_draw_idx index = {.prim = 4, .itype = 1, .count = 3, .instances = 1,
                    .ibuf = (uint64_t)(uintptr_t)(bank + walk_device_index_offset(slot))};
                op(OP_BIND_VBUF, &vertex, (uint32_t)vertex_payload); op(OP_SCISSOR, &pixel, sizeof pixel);
                op(OP_DRAW_IDX, &index, sizeof index);
            }
            if (chunk == WALK_CHUNKS - 1) op(OP_COPY_T2B, &f.readback, sizeof f.readback);
            op(OP_END, NULL, 0);
            CHECK(rn <= stream_capacity, "device-shaped stream group fits explicit capacity");
            if (rn > stream_capacity) DIE("device-shaped walking stream exceeds explicit capacity");
            memcpy(streams[chunk], rec, rn); rn = 0;
        }
        uint64_t previous[VK_REC_N], current[VK_REC_N], delta[VK_REC_N];
        vk_record_test_counts count0, count1; vk_record_test_counts_get(&count0);
        vk_record_test_profile_ns(previous, VK_REC_N);
#ifdef VK_LAG_TEST_STATS_VERSION
        vk_lag_test_stats stats0, stats1, stats = {0}; vk_lag_test_stats_get(&stats0);
#endif
        uint64_t serial = vk_submit_serial(), cpu_ns = 0, wall_ns = 0, seal_ns = 0;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(handles, WALK_CHUNKS, NULL);
        for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) {
            uint64_t cpu0 = walk_clock(CLOCK_THREAD_CPUTIME_ID), wall0 = walk_clock(CLOCK_MONOTONIC);
            mtl_exec(streams[chunk]);
            wall_ns += walk_clock(CLOCK_MONOTONIC) - wall0; cpu_ns += walk_clock(CLOCK_THREAD_CPUTIME_ID) - cpu0;
            if (stress && chunk != WALK_CHUNKS - 1) {
                wall0 = walk_clock(CLOCK_MONOTONIC); vk_submit_chunk(); seal_ns += walk_clock(CLOCK_MONOTONIC) - wall0;
            }
        }
        uint64_t seal0 = walk_clock(CLOCK_MONOTONIC), fence = mtl_sub_commit(); seal_ns += walk_clock(CLOCK_MONOTONIC) - seal0;
        vk_record_test_profile_ns(current, VK_REC_N); vk_record_test_counts_get(&count1);
#ifdef VK_LAG_TEST_STATS_VERSION
        vk_lag_test_stats_get(&stats1); walk_device_stats_add(&stats, &stats0, &stats1);
        if (recorder && vk_lag_capture_on())
            CHECK(stats.kind[VK_LAG_VERTEX].decisions == (uint64_t)WALK_DRAWS * 3 &&
                stats.kind[VK_LAG_INDEX].decisions == WALK_DRAWS,
                "device-shaped frame %d observes all 17280 vertex and 5760 index lag decisions", frame);
        if (recorder && hybrid && (phase >= 2 || (phase == 1 && sample == 1)))
            CHECK(stats.kind[VK_LAG_VERTEX].hot_decisions == (uint64_t)DEVICE_HOT_SETS * WALK_CHUNKS * WALK_REPEATS * hot_streams &&
                stats.kind[VK_LAG_INDEX].hot_decisions == (uint64_t)DEVICE_HOT_SETS * WALK_CHUNKS * WALK_REPEATS &&
                stats.kind[VK_LAG_VERTEX].hot_copies && stats.kind[VK_LAG_INDEX].hot_copies,
                "device-shaped frame %d observes real hot vertex/index selection and bounded private copies", frame);
#endif
        uint64_t accepted = count1.draws - count0.draws, freeze_hits = count1.freeze_hits - count0.freeze_hits;
        uint64_t program_hits = count1.program_hits - count0.program_hits;
        CHECK(accepted == WALK_DRAWS && count1.index_skips == count0.index_skips,
            "device-shaped %s frame %d executes all %d initialized indexed draws (%llu accepted)",
            names[phase], sample, WALK_DRAWS, (unsigned long long)accepted);
        for (int k = 0; k < VK_REC_N; k++) delta[k] = current[k] - previous[k];
        uint64_t wait0 = walk_clock(CLOCK_MONOTONIC);
        CHECK(mtl_wait(fence, 30000000000ull), "device-shaped frame %d real Vulkan execution completes", frame);
        uint64_t wait_ns = walk_clock(CLOCK_MONOTONIC) - wait0, chunks = vk_submit_serial() - serial;
        CHECK(chunks == (stress ? WALK_CHUNKS : 1), "device-shaped %s frame %d has exactly %d actual submissions (%llu observed)",
            names[phase], sample, stress ? WALK_CHUNKS : 1, (unsigned long long)chunks);
        nvn_dirty_lag_sticky_stats(&rearm1, &update1, &promotion1, &demotion1);
        int bad = 0; uint64_t hash = 14695981039346656037ull; uint8_t *pixels = output + 0x20000;
        for (int pixel = 0; pixel < DEVICE_WIDTH * DEVICE_HEIGHT; pixel++) {
            float expected[] = {0, 0, 0, 1};
            if (pixel < WALK_DRAWS) {
                int repeat = pixel % WALK_REPEATS, set = pixel / WALK_REPEATS % WALK_SETS;
                int chunk = pixel / (WALK_REPEATS * WALK_SETS), slot = walk_device_slot(set, chunk, repeat);
                int data_version = set < DEVICE_HOT_SETS ? version : 0;
                walk_device_color(data_version, set, slot, (slot * 7 + data_version) % 40, expected);
            }
            const uint8_t *actual = pixels + pixel * 4;
            if (!walk_pixel(actual, expected)) {
                if (bad < 4) printf("lag-walk %s frame %d pixel (%d,%d) = %u,%u,%u,%u; expected %.3f,%.3f,%.3f,%.3f\n",
                    names[phase], sample, pixel % DEVICE_WIDTH, pixel / DEVICE_WIDTH, actual[0], actual[1], actual[2], actual[3],
                    expected[0] * 255, expected[1] * 255, expected[2] * 255, expected[3] * 255);
                bad++;
            }
            for (int c = 0; c < 4; c++) { hash ^= actual[c]; hash *= 1099511628211ull; }
        }
        CHECK(!bad, "device-shaped %s frame %d checks every draw's selected vertex/index pixels plus untouched clear (%d wrong)",
            names[phase], sample, bad);
        char label[64]; snprintf(label, sizeof label, "%s-%s%d", names[phase], warming ? "warmup" : "frame",
            phase == 0 && !warming ? sample - cold_warmup : sample);
        printf("lag-walk %s measured CPU ns/draw: mtl_exec_thread=%.2f mtl_exec_wall=%.2f seals_commit_wall=%.2f final_wait_wall=%.2f; "
               "actual_chunks=%llu pixels_fnv64=%016llx\n", label, cpu_ns / (double)WALK_DRAWS, wall_ns / (double)WALK_DRAWS,
            seal_ns / (double)WALK_DRAWS, wait_ns / (double)WALK_DRAWS, (unsigned long long)chunks, (unsigned long long)hash);
        printf("lag-walk %s observed history: hot_ranges=%d/288 rearm_delta=%llu armed_ticket_update_delta=%llu "
               "promotion_delta=%llu demotion_delta=%llu\n", label, observed_hot, (unsigned long long)(rearm1 - rearm0),
            (unsigned long long)(update1 - update0), (unsigned long long)(promotion1 - promotion0), (unsigned long long)(demotion1 - demotion0));
        walk_print_profile(label, delta, WALK_DRAWS); walk_device_print_hits(label, accepted, freeze_hits, program_hits);
#ifdef VK_LAG_TEST_STATS_VERSION
        walk_device_print_stats(label, &stats, 1);
        if (!warming) walk_device_stats_add(&measured_stats[phase], &stats0, &stats1);
#endif
        if (!warming) {
            walk_measure *m = &measured[phase]; m->frames++;
            m->cpu += cpu_ns; m->wall += wall_ns; m->seal += seal_ns; m->wait += wait_ns;
            m->draws += accepted; m->freeze_hits += freeze_hits; m->program_hits += program_hits;
            for (int k = 0; k < VK_REC_N; k++) m->ns[k] += delta[k];
        }
    }
    for (int phase = 0; phase < 4; phase++) {
        const walk_measure *m = &measured[phase]; double draws = (double)m->frames * WALK_DRAWS;
        char label[64]; snprintf(label, sizeof label, "%s-mean", names[phase]);
        printf("lag-walk %s measured CPU ns/draw: mtl_exec_thread=%.2f mtl_exec_wall=%.2f seals_commit_wall=%.2f final_wait_wall=%.2f; "
               "measured_frames=%d measured_draws=%.0f%s\n", label, m->cpu / draws, m->wall / draws, m->seal / draws, m->wait / draws,
            m->frames, draws, phase == 1 ? " (training only; excluded from cold/learned-hot means)" :
            phase == 3 ? " (separate actual-chunk stress case)" : "");
        walk_print_profile(label, m->ns, draws); walk_device_print_hits(label, m->draws, m->freeze_hits, m->program_hits);
#ifdef VK_LAG_TEST_STATS_VERSION
        walk_device_print_stats(label, &measured_stats[phase], m->frames);
#endif
    }
    for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) free(streams[chunk]);
    mtl_drain(); nvn_dirty_remove_range(banks); mtl_pool_del(banks); munmap(mapping, mapping_size);
}
/* Separate version/call accounting from the large stable/churn benchmark. A 6912-byte owned capture contains
   overlapping AND adjacent 1024-byte bindings. Every first/last vec4 is shader-visible; the two four-byte overlays
   occur only between complete offset sweeps. Ten real game submissions recycle all four snapshot arenas, and
   twelve actual chunks per game submission wrap the ordinary command/descriptor ring. */
enum { SHARE_FRAMES = 10, SHARE_VERSIONS = 3, SHARE_OFFSETS = 16, SHARE_REPEATS = 4,
       SHARE_DRAWS = SHARE_OFFSETS * SHARE_REPEATS, SHARE_STRIDE = 256, SHARE_RANGE = 1024,
       SHARE_SPAN = 23 * SHARE_STRIDE + SHARE_RANGE + 32, SHARE_BASE = 0x12000, SHARE_STREAM_BYTES = 32768,
       SHARE_RING_DRAWS = 12, SHARE_TOTAL_DRAWS = SHARE_VERSIONS * SHARE_DRAWS + 6 + SHARE_RING_DRAWS };
static void walk_share_update(uint64_t address, float value) {
    struct { uint64_t dst, n; float value; uint32_t pad; } update = {address, 4, value, 0};
    op(OP_UPDATE_UB, &update, sizeof update);
}
static void walk_share_draw(const walk_bank *b, const float colors[SHARE_SPAN / 16][4], int offset,
                            int range, int x, int y, float expected[64 * 64][4]) {
    static const int relative[] = {0, 1, 4, 8};
    float *pixel = expected[y * 64 + x]; memset(pixel, 0, 4 * sizeof *pixel);
    for (int slot = 0; slot < 4; slot++) {
        int first = (offset + relative[slot]) * (SHARE_STRIDE / 16);
        p_bind_buf binding = {1, slot, (uint64_t)(uintptr_t)(b->cpu + SHARE_BASE + first * 16), (uint64_t)range};
        op(OP_BIND_UBO, &binding, sizeof binding);
        for (int c = 0; c < 4; c++) pixel[c] += (colors[first][c] + colors[first + 63][c]) * .125f;
    }
    walk_draw(b, x, y, 0);
}
static void walk_share_bench(const walk_fixture *original, walk_bank *b, uint8_t *readback,
                             uint64_t first_frame, int recorder) {
    walk_bank_fill(b, 0, 0);
    uint8_t control[3][0x880]; prog_t programs[2] = {{0}};
    for (int shader = 0; shader < 3; shader++) {
        uint8_t code[0x230]; uint32_t length;
        shader_binary(shader != 0, shader == 2 ? 5 : 37 + shader, control[shader], code, &length);
        memcpy(b->cpu + 0x2c000 + shader * 0x1000, code, length);
    }
    for (int program = 0; program < 2; program++) {
        programs[program].nstage = 2; programs[program].serial = 3240 + program;
        programs[program].sd[0].data = (uint64_t)(uintptr_t)(b->cpu + 0x2c000);
        programs[program].sd[0].control = control[0];
        programs[program].sd[1].data = (uint64_t)(uintptr_t)(b->cpu + 0x2d000 + program * 0x1000);
        programs[program].sd[1].control = control[program + 1];
    }
    const float produced[2][4] = {{.875f, .25f, .75f, 1}, {.25f, .875f, .5f, 1}};
    memcpy(b->cpu + 0x25000, produced, sizeof produced);
    walk_fixture f = *original; f.attr.n = f.stream.n = 1;
    p_bind_program uniform_program = {&programs[0], 3}, alias_program = {&programs[1], 3};
    p_bind_vbuf vertex = b->vertex; vertex.n = 1;
    uint8_t *streams[SHARE_VERSIONS]; uint64_t handles[SHARE_VERSIONS];
    for (int version = 0; version < SHARE_VERSIONS; version++) {
        streams[version] = malloc(SHARE_STREAM_BYTES);
        if (!streams[version]) DIE("owned-sharing stream allocation failed");
        handles[version] = (uint64_t)(uintptr_t)streams[version];
    }
    uint32_t features = 0;
#if defined(VK_LAG_TEST_STATS_VERSION) && VK_LAG_TEST_STATS_VERSION >= 2
    features = vk_lag_test_features();
#endif
#ifdef VK_LAG_TEST_STATS_VERSION
    vk_lag_test_stats total_stats = {0};
#endif
    printf("lag-walk owned-sharing layout: %d successive game submissions, %d actual chunks/frame; "
           "%d distinct measured semantic versions/frame (base + two ordered 4B overlays), %d draws/version, "
           "4 consumed 1024B UBO bindings/draw, %d UBO calls/version; stride=256 offsets={n,n+1,n+4,n+8}, n=0..15 x4; "
           "first five frames keep declared extent=1024, last five use 1024/1040/1056 for base/overlays; "
           "owned_union_max_bytes=%d, requested_base_bytes_per_version=%d, feature_flags=0x%x effective_recorder=%d\n",
        SHARE_FRAMES, SHARE_RING_DRAWS, SHARE_VERSIONS, SHARE_DRAWS, SHARE_DRAWS * 4,
        SHARE_SPAN, SHARE_DRAWS * 4 * SHARE_RANGE, features, recorder);
    printf("lag-walk owned-sharing safety: unchanged vertex/index metadata crosses real tickets/snapshots; same UBO VAs "
           "receive real CPU rewrites after each completed frame. Separate ordered tail has two same-range GPU producers, "
           "two postproducer 4B updates (four additional semantic versions), two real readonly SSBO alias consumers, "
           "then twelve draws of the final version across ring wrap. Every draw owns a checked pixel; no cost/safety bypass.\n");
    if (!features) printf("lag-walk owned-sharing feature-specific allocation/metadata assertions unavailable; "
                         "identical draws, pixels, producer/alias ordering and ring reuse remain mandatory.\n");
    for (int frame = 0; frame < SHARE_FRAMES; frame++) {
        nvn_dirty_lag_frame(first_frame + (uint64_t)frame);
        int range_jitter = frame >= SHARE_FRAMES / 2;
        int owned_span = SHARE_SPAN - (range_jitter ? 0 : 32);
        printf("lag-walk owned-sharing frame=%d extent_case=%s owned_union_bytes=%d\n",
            frame, range_jitter ? "changing" : "fixed", owned_span);
        float colors[SHARE_SPAN / 16][4], expected[64 * 64][4];
        for (int vector = 0; vector < SHARE_SPAN / 16; vector++) {
            for (int c = 0; c < 3; c++) colors[vector][c] = (float)((frame + vector * 3 + c * 5) % 13 + 1) / 16;
            colors[vector][3] = 1;
        }
        dm_guest_write_prepare(b->cpu + SHARE_BASE, SHARE_SPAN); memcpy(b->cpu + SHARE_BASE, colors, sizeof colors);
        for (int pixel = 0; pixel < 64 * 64; pixel++) {
            expected[pixel][0] = expected[pixel][1] = expected[pixel][2] = 0; expected[pixel][3] = 1;
        }
        for (int version = 0; version < SHARE_VERSIONS; version++) {
            rn = 0;
            if (!version) {
                walk_setup(&f); op(OP_BIND_PROGRAM, &uniform_program, sizeof uniform_program);
                op(OP_BIND_VBUF, &vertex, offsetof(p_bind_vbuf, r) + sizeof vertex.r[0]);
            } else if (version == 1) {
                colors[10 * SHARE_STRIDE / 16][0] = .9375f;
                walk_share_update((uint64_t)(uintptr_t)(b->cpu + SHARE_BASE + 10 * SHARE_STRIDE), .9375f);
            } else {
                colors[12 * SHARE_STRIDE / 16 + 63][1] = .875f;
                walk_share_update((uint64_t)(uintptr_t)(b->cpu + SHARE_BASE + 12 * SHARE_STRIDE + 1008 + 4), .875f);
            }
            for (int draw = 0; draw < SHARE_DRAWS; draw++)
                walk_share_draw(b, colors, draw % SHARE_OFFSETS, SHARE_RANGE + (range_jitter ? 16 * version : 0), draw, version, expected);
            op(OP_END, NULL, 0);
            CHECK(rn <= SHARE_STREAM_BYTES, "owned-sharing version %d fits its stream allocation", version);
            if (rn > SHARE_STREAM_BYTES) DIE("owned-sharing stream exceeds explicit capacity");
            memcpy(streams[version], rec, rn); rn = 0;
        }
        vk_record_test_counts count0, count1; vk_record_test_counts_get(&count0);
#ifdef VK_LAG_TEST_STATS_VERSION
        vk_lag_test_stats stats0, phase_before, phase_after, phase_stats[SHARE_VERSIONS] = {0};
        vk_lag_test_stats_get(&stats0); phase_before = stats0;
#endif
        uint64_t serial = vk_submit_serial(), ticket = mtl_last_fence();
        void *owned = mtl_sub_capture_ubos(handles, SHARE_VERSIONS);
        CHECK(!vk_lag_uniform_private() || owned != NULL, "owned-sharing frame %d eagerly captures its immutable base", frame);
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(handles, SHARE_VERSIONS, owned);
        for (int version = 0; version < SHARE_VERSIONS; version++) {
            mtl_exec(streams[version]);
#ifdef VK_LAG_TEST_STATS_VERSION
            vk_lag_test_stats_get(&phase_after);
            walk_device_stats_add(&phase_stats[version], &phase_before, &phase_after);
            const vk_lag_kind_stats *s = &phase_stats[version].kind[VK_LAG_UNIFORM];
            printf("lag-walk owned-sharing frame=%d semantic_version=%d distinct_versions=1 UBO_calls=%d "
                   "requested_bytes=%d owned_union_bytes=%d snapshot_allocations=%llu snapshot_bytes=%llu "
                   "snapshot_bytes_per_semantic_version=%llu (version0 includes one eager arena base)\n",
                frame, version, SHARE_DRAWS * 4, SHARE_DRAWS * 4 * (SHARE_RANGE + (range_jitter ? 16 * version : 0)), owned_span,
                (unsigned long long)s->snapshot_allocations, (unsigned long long)s->snapshot_bytes,
                (unsigned long long)s->snapshot_bytes);
#if VK_LAG_TEST_STATS_VERSION >= 2
            if (vk_lag_uniform_private() && (features & VK_LAG_TEST_UNIFORM_COVER)) {
                CHECK(s->snapshot_allocations < SHARE_DRAWS * 4 && s->snapshot_bytes < SHARE_DRAWS * 4 * SHARE_RANGE,
                    "owned-sharing frame %d version %d allocates/copies less than per-call snapshots", frame, version);
                CHECK(version ? s->snapshot_allocations <= 4 && s->snapshot_bytes <= 4 * 4096 :
                    s->snapshot_allocations == 1 && s->snapshot_bytes == (uint64_t)owned_span,
                    "owned-sharing frame %d version %d bounds actual copy bytes per semantic version, not dirty unit", frame, version);
            }
#endif
            phase_before = phase_after;
#endif
        }
#if defined(VK_LAG_TEST_STATS_VERSION) && VK_LAG_TEST_STATS_VERSION >= 2
        if (vk_lag_uniform_private() && (features & VK_LAG_TEST_UNIFORM_COVER))
            CHECK(phase_stats[1].kind[VK_LAG_UNIFORM].uniform_cover_hits +
                phase_stats[2].kind[VK_LAG_UNIFORM].uniform_cover_hits > 0,
                "owned-sharing frame %d observes actual overlapping-range cover reuse", frame);
#endif
        /* A readonly SSBO is a real non-UBO usage alias of these UBO bytes. Use the same physical owner:
           POOLS_HOST may import a virtual VA as an independent VkBuffer, unlike Android's donor-buffer aliases.
           Virtual writer/mapping coverage remains in the separate shared-alias and span probes. */
        const uint64_t destination = (uint64_t)(uintptr_t)(b->cpu + SHARE_BASE + 10 * SHARE_STRIDE);
        p_bind_buf storage = {1, 0, destination, 16};
        op(OP_BIND_PROGRAM, &alias_program, sizeof alias_program); op(OP_BIND_SSBO, &storage, sizeof storage);
        walk_draw(b, 0, 3, 0); memcpy(expected[3 * 64], colors[10 * SHARE_STRIDE / 16], 16);
        op(OP_BIND_PROGRAM, &uniform_program, sizeof uniform_program);
        for (int producer = 0; producer < 2; producer++) {
            walk_copy((uint64_t)(uintptr_t)(b->cpu + 0x25000 + producer * 16), destination, 16);
            memcpy(colors[10 * SHARE_STRIDE / 16], produced[producer], 16);
            walk_share_draw(b, colors, 10, SHARE_RANGE, 1 + producer * 2, 3, expected);
            int component = producer ? 1 : 2; float value = producer ? .0625f : .9375f;
            walk_share_update(destination + component * 4, value); colors[10 * SHARE_STRIDE / 16][component] = value;
            walk_share_draw(b, colors, 10, SHARE_RANGE, 2 + producer * 2, 3, expected);
        }
        op(OP_BIND_PROGRAM, &alias_program, sizeof alias_program); op(OP_BIND_SSBO, &storage, sizeof storage);
        walk_draw(b, 5, 3, 0); memcpy(expected[3 * 64 + 5], colors[10 * SHARE_STRIDE / 16], 16);
        op(OP_BIND_PROGRAM, &uniform_program, sizeof uniform_program);
        for (int seal = 0; seal < SHARE_RING_DRAWS; seal++) {
            walk_share_draw(b, colors, 10, SHARE_RANGE, seal, 4, expected);
            if (seal == SHARE_RING_DRAWS - 1) op(OP_COPY_T2B, &f.readback, sizeof f.readback);
            walk_exec();
            if (seal != SHARE_RING_DRAWS - 1) vk_submit_chunk();
        }
        uint64_t fence = mtl_sub_commit(); vk_record_test_counts_get(&count1);
#ifdef VK_LAG_TEST_STATS_VERSION
        vk_lag_test_stats stats1, stats = {0}; vk_lag_test_stats_get(&stats1);
        walk_device_stats_add(&stats, &stats0, &stats1); walk_device_stats_add(&total_stats, &stats0, &stats1);
        char label[48]; snprintf(label, sizeof label, "owned-sharing-frame%d", frame);
        walk_device_print_stats(label, &stats, 1);
#endif
        CHECK(vk_submit_serial() == serial + SHARE_RING_DRAWS && fence == ticket + SHARE_RING_DRAWS,
            "owned-sharing frame %d crosses twelve actual queued tickets, never synthetic early arming", frame);
        CHECK(count1.draws - count0.draws == SHARE_TOTAL_DRAWS && count1.index_skips == count0.index_skips,
            "owned-sharing frame %d executes all %d real draws without rejected indices", frame, SHARE_TOTAL_DRAWS);
        CHECK(mtl_wait(fence, 30000000000ull), "owned-sharing frame %d completes before same-address CPU reuse", frame);
        int bad = 0;
        for (int pixel = 0; pixel < 64 * 64; pixel++) if (!walk_pixel(readback + pixel * 4, expected[pixel])) {
            if (bad < 4) printf("lag-walk owned-sharing frame %d pixel (%d,%d) = %u,%u,%u,%u; expected %.3f,%.3f,%.3f,%.3f\n",
                frame, pixel % 64, pixel / 64, readback[pixel * 4], readback[pixel * 4 + 1],
                readback[pixel * 4 + 2], readback[pixel * 4 + 3], expected[pixel][0] * 255,
                expected[pixel][1] * 255, expected[pixel][2] * 255, expected[pixel][3] * 255);
            bad++;
        }
        CHECK(!bad, "owned-sharing frame %d checks each offset/version, ordered GPU/alias consumer and ring-wrap pixel (%d wrong)", frame, bad);
        printf("lag-walk owned-sharing frame=%d measured_distinct_semantic_versions=%d measured_UBO_calls=%d "
               "tail_additional_semantic_versions=4 ring_final_version_draws=%d actual_chunks=%llu checked_pixels=4096 wrong_pixels=%d\n",
            frame, SHARE_VERSIONS, SHARE_VERSIONS * SHARE_DRAWS * 4, SHARE_RING_DRAWS,
            (unsigned long long)(vk_submit_serial() - serial), bad);
    }
#ifdef VK_LAG_TEST_STATS_VERSION
    walk_device_print_stats("owned-sharing-total", &total_stats, SHARE_FRAMES);
#if VK_LAG_TEST_STATS_VERSION >= 2
    if (recorder && vk_lag_capture_on() && (features & VK_LAG_TEST_CROSS_FRAME_METADATA)) {
        const vk_lag_kind_stats *v = &total_stats.kind[VK_LAG_VERTEX], *i = &total_stats.kind[VK_LAG_INDEX];
        CHECK(v->metadata_cross_ticket + i->metadata_cross_ticket + v->unit_cross_ticket + i->unit_cross_ticket > 0 &&
            v->metadata_cross_snapshot + i->metadata_cross_snapshot > 0,
            "unchanged tracked vertex/index metadata survives real successive submission and snapshot identities");
    }
#endif
#endif
    for (int version = 0; version < SHARE_VERSIONS; version++) free(streams[version]);
}

typedef struct { _Atomic int active, stop; _Atomic uint64_t calls; } walk_service_bench;
static int walk_service_noop(void *arg, int boundary) {
    (void)boundary; atomic_fetch_add_explicit((_Atomic uint64_t *)arg, 1, memory_order_relaxed); return 1;
}
static void *walk_service_bench_run(void *arg) {
    walk_service_bench *b = arg;
    while (!atomic_load(&b->stop)) {
        if (!atomic_load(&b->active)) { usleep(50); continue; }
        vk_rec_service_call(walk_service_noop, &b->calls);
    }
    return NULL;
}
static int lag_walk_bench(int argc, char **argv) {
    int frames = 6; const int warmup = 2;
    const char *fg_boundary = getenv("TOTK_FRAMEGEN_HOST_BENCH");
    int framegen_boundary = fg_boundary && !strcmp(fg_boundary,"1");
    if (argc == 3) {
        char *end; long parsed = strtol(argv[2], &end, 10);
        if (!argv[2][0] || *end || parsed < 1 || parsed > 120) { fprintf(stderr, "--lag-walk-bench frames must be 1..120\n"); return 2; }
        frames = (int)parsed;
    } else if (argc != 2) { fprintf(stderr, "usage: %s --lag-walk-bench [measured-frames-per-case=6]\n", argv[0]); return 2; }
    setenv("TOTK_VK_REC_PROFILE", "1", 1); setenv("TOTK_VK_REC_PROFILE_RATE", "1", 1);
    /* Primary frames measure one actual submission; only the separately labeled stress case seals early.
       Otherwise the backend's resolved draw limit also submits at real copy/update pass boundaries. */
    setenv("TOTK_VK_CHUNK_DRAWS", "0", 1);
    setenv("TOTK_VK_WRITEBACK", "1", 0);
    mtl_init();
    call_counts_install();
    CHECK(vk_rec_profile_on, "walking benchmark profiles every opcode/draw");
    const char *rename = getenv("TOTK_VK_UB_RENAME");
    CHECK(!(rename && atoi(rename) && !vk_off("rename")), "walking benchmark keeps opt-in ordinary UBO renaming OFF");
    if (fails) return 1;
    const size_t pool_size = 0x80000, alias_size = 0x80000, alignment = 0x10000;
    size_t physical_size = WALK_POOLS * pool_size + alignment, virtual_size = WALK_POOLS * alias_size + alignment;
    uint8_t *physical = mmap(NULL, physical_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint8_t *virtual = mmap(NULL, virtual_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(physical != MAP_FAILED && virtual != MAP_FAILED, "reserve walking physical/alias pools");
    if (physical == MAP_FAILED || virtual == MAP_FAILED) return 1;
    uint8_t *pools = (uint8_t *)(((uintptr_t)physical + alignment - 1) & ~(uintptr_t)(alignment - 1));
    uint8_t *aliases = (uint8_t *)(((uintptr_t)virtual + alignment - 1) & ~(uintptr_t)(alignment - 1));
    int backend_aliases = 0;
    for (int p = 0; p < WALK_POOLS; p++) {
        uint8_t *cpu = pools + p * pool_size, *alias = aliases + p * alias_size;
        mtl_pool_add(cpu, pool_size); nvn_dirty_add_range(cpu, pool_size);
        if (mtl_pool_alias(alias, cpu, alias_size)) backend_aliases++;
        else {
            if (mremap(cpu, 0, alias_size, MREMAP_MAYMOVE | MREMAP_FIXED, alias) != alias) DIE("walking benchmark shared alias mapping failed");
            /* HOST imports have independent VkBuffers: expose the REAL shared-page relationship to writer projection. */
            vk_record_test_alias(alias, (uint64_t)(uintptr_t)cpu, alias_size);
        }
        mtl_vpool_add(alias, alias_size); nvn_dirty_add_range(alias, alias_size);
    }
    walk_bank banks[WALK_SETS + 1];
    for (int set = 0; set <= WALK_SETS; set++) {
        int p = set / 2; size_t off = (size_t)(set & 1) * 0x40000;
        walk_bank_bindings(&banks[set], pools + p * pool_size + off, aliases + p * alias_size + off);
        mtl_pool_add(banks[set].cpu + 0x10000, 0x1000); /* overlapping pool view, same actual GPU buffer */
    }
    uint8_t control[4][0x880]; prog_t programs[3] = {{0}}; uint32_t length;
    for (int shader = 0; shader < 4; shader++) {
        uint8_t code[0x230]; shader_binary(shader != 0, shader == 3 ? 6 : 32 + shader, control[shader], code, &length);
        memcpy(pools + 12 * pool_size + 0x40000 + shader * 0x1000, code, length);
    }
    for (int program = 0; program < 3; program++) {
        programs[program].nstage = 2; programs[program].serial = 3200 + program;
        programs[program].sd[0].data = (uint64_t)(uintptr_t)(pools + 12 * pool_size + 0x40000); programs[program].sd[0].control = control[0];
        programs[program].sd[1].data = (uint64_t)(uintptr_t)(pools + 12 * pool_size + 0x41000 + program * 0x1000);
        programs[program].sd[1].control = control[1 + program];
    }
    /* Separate real render-target/readback storage from every CPU-rewritten input. */
    uint8_t *output_mapping = mmap(NULL, 0x50000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(output_mapping != MAP_FAILED, "reserve walking render target and readback");
    if (output_mapping == MAP_FAILED) return 1;
    uint8_t *output = (uint8_t *)(((uintptr_t)output_mapping + alignment - 1) & ~(uintptr_t)(alignment - 1));
    mtl_pool_add(output, 0x40000); nvn_dirty_add_range(output, 0x40000);
    pool_t target_pool = {.mem = output, .size = 0x40000,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t target = {0}; target.d.target = 1; target.d.format = 0x25;
    target.d.width = target.d.height = 64; target.d.depth = target.d.levels = 1;
    nvn_tex_layout(&target.d, &target.L); target.mem = output; target.pool = &target_pool; target.serial = 3234;
    walk_fixture fixture = {0}; fixture.target.n = 1; fixture.target.color[0] = &target;
    fixture.readback.src = (uint64_t)(uintptr_t)(output + 0x20000); fixture.readback.tex = &target;
    fixture.readback.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    fixture.attr.n = fixture.stream.n = 4;
    /* va_st is fmt/offset/stream, not fmt/stream/offset: every shader input selects its own vec2 stream. */
    for (int k = 0; k < 4; k++) {
        va_st attr = {.fmt = 0x16, .stream = k};
        memcpy(&fixture.attr.st[k], &attr, sizeof attr); fixture.stream.st[k].stride = 8;
    }
    fixture.viewport.n = 1; fixture.viewport.v[0][2] = fixture.viewport.v[0][3] = 64; fixture.viewport.v[0][5] = 1;
    for (int writable = 0; writable < 2; writable++) fixture.program[writable] = (p_bind_program){&programs[writable], 3};
    fixture.pointer_program = (p_bind_program){&programs[2], 3}; /* pre-existing salt6 RB_SBUF_UBO seed */
    printf("lag-walk configuration: TOTK_VK_REC_PROFILE=1 TOTK_VK_REC_PROFILE_RATE=1 TOTK_VK_CHUNK_DRAWS=0; FRAME_LAG=%d (env=%s) LAG_CAPTURE=%s LAG_MODE=%s LAG_HYBRID=%s REC_THREADS=%s; warmup=%d measured=%d\n",
        runtime_frame_lag_requested(), getenv("TOTK_FRAME_LAG") ? getenv("TOTK_FRAME_LAG") : "unset", getenv("TOTK_VK_LAG_CAPTURE") ? getenv("TOTK_VK_LAG_CAPTURE") : "default",
        getenv("TOTK_VK_LAG_MODE") ? getenv("TOTK_VK_LAG_MODE") : "wait", getenv("TOTK_VK_LAG_HYBRID") ? getenv("TOTK_VK_LAG_HYBRID") : "default-on",
        getenv("TOTK_VK_REC_THREADS") ? getenv("TOTK_VK_REC_THREADS") : "0", warmup, frames);
    if (framegen_boundary) printf("lag-walk framegen tracking=%s: explicit host real-frame census boundary after commit; no presentation/generated output; DIAG=%s\n",
        getenv("TOTK_FRAMEGEN") ? getenv("TOTK_FRAMEGEN") : "0",
        getenv("TOTK_FRAMEGEN_DIAG") ? getenv("TOTK_FRAMEGEN_DIAG") : "0");
    printf("lag-walk layout: %d indexed draws/frame in ONE actual Vulkan/game submission; %d stream groups x %d sets x %d draws; "
           "12 input pools + safety pool, 25 overlapping views, 13 shared aliases (%d backend / %d host-mremap), separate output pool\n",
        WALK_DRAWS, WALK_CHUNKS, WALK_SETS, WALK_REPEATS, backend_aliases, WALK_POOLS - backend_aliases);
    printf("lag-walk bank layout: 256KiB/set; VS UBO +0, FS UBO +64KiB, readonly storage +128KiB, vertex/index +192KiB; "
           "TOTK_DIRTY_UNIT_KB=%s TOTK_DIRTY_UNIT=%s. Two separately labeled stress frames force 12 actual chunks/frame, excluded from primary means.\n",
        getenv("TOTK_DIRTY_UNIT_KB") ? getenv("TOTK_DIRTY_UNIT_KB") : "default64",
        getenv("TOTK_DIRTY_UNIT") ? getenv("TOTK_DIRTY_UNIT") : "default");
    printf("lag-walk bindings: 4 VS + 4 FS UBOs (512 bytes), 4 readonly SSBOs (16 bytes), 1 in-UBO writable neighbour on 1/8 sets; "
           "4 vertex streams (vec2/stride8/range64), U16 indices alternating 3/6, 1x1 scissor on 64x64 RGBA8\n");
    printf("lag-walk stable-binding case: 20 repeated bindings/group; first 960 draws precede any inline update, "
           "then 96 partial UBO updates and 80 unrelated updates/frame. This is a stable-binding case, NOT a Lookout prediction.\n");
    printf("lag-walk churn-binding case: 7 stable + 13 alternating relevant VS0/FS3 range rebinds/group, "
           "288 VS + 288 FS consumed inline updates/frame, plus the same 96 partial + 80 unrelated updates; FS3 changes checked pixels. "
           "Both cases add group6 UBO/vertex/index GPU producers and same-address next-frame CPU rewrites.\n");
    printf("lag-walk interpretation: observed recording-thread CPU time and CLOCK_MONOTONIC exclusive VK_REC scopes only; "
           "vertex/index share one existing category; no GPU-frame-time or device/game equivalence claim. "
           "Host-mremap aliases have independent imported VkBuffers; existing alias metadata records their real shared pages "
           "for physical-source/virtual-writer safety. GPU producer checks use overlapping owner/view pools.\n");
    uint8_t *streams[WALK_CHUNKS]; uint64_t handles[WALK_CHUNKS];
    for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) {
        streams[chunk] = malloc(WALK_STREAM_BYTES); if (!streams[chunk]) DIE("walking benchmark stream allocation failed");
        handles[chunk] = (uint64_t)(uintptr_t)streams[chunk];
    }
    if (vk_lag_uniform_private()) {
        rn = 0; walk_setup(&fixture); walk_bind(&fixture, &banks[0], 0);
        struct { uint64_t dst, n; uint32_t value[4]; } update = {
            (uint64_t)(uintptr_t)(banks[0].cpu + 0x28000), 16, {1, 0, 0, 0}};
        op(OP_UPDATE_UB, &update, sizeof update);
        mtl_sub_begin(); walk_exec();
        CHECK(!vk_draw_test_prepare_bench(), "uniform preparation microfixture completes");
        CHECK(mtl_wait(mtl_sub_commit(), 30000000000ull), "uniform preparation microfixture publishes its private update");
    }
    walk_measure measured[3] = {0};
    const char *case_names[] = {"stable-binding", "churn-binding", "chunk-stress"};
    const int stress_frames = 2, case_frames = warmup + frames, recorder = vk_record_enabled();
    printf("lag-walk cases: %d warmup + %d measured frames EACH for stable-binding and churn-binding; "
           "effective_recorder=%d. Hit counts use existing runtime test counters, not a requested/synthetic hit rate.\n", warmup, frames, recorder);
    walk_service_bench service = {0}; pthread_t service_thread;
    int pending_service = getenv("TOTK_VK_TEST_OWNER_PENDING") && atoi(getenv("TOTK_VK_TEST_OWNER_PENDING"));
    if (pending_service && pthread_create(&service_thread, NULL, walk_service_bench_run, &service)) DIE("walking service benchmark");
    for (int frame = 0; frame < 2 * case_frames + stress_frames; frame++) {
        int phase = frame < case_frames ? 0 : frame < 2 * case_frames ? 1 : 2;
        int sample = frame - phase * case_frames, stress = phase == 2, churn = phase == 1;
        int warming = !stress && sample < warmup;
        nvn_dirty_lag_frame(100 + (uint64_t)frame);
        for (int set = 0; set < WALK_SETS; set++) walk_bank_fill(&banks[set], frame, set);
        for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) {
            rn = 0; if (!chunk) walk_setup(&fixture);
            for (int set = 0; set < WALK_SETS; set++) {
                walk_bank *b = &banks[set]; walk_bind(&fixture, b, !(set % 8));
                /* First 960 draws have no inline updates; then four waves of 24 partial updates retain neighbours. */
                if (chunk % 3 == 2) walk_red(b, (float)((frame + set + (chunk + 1) / 3) % 4 + 1) / 8);
                struct { uint64_t dst, n; uint32_t value[4]; } unrelated = {
                    (uint64_t)(uintptr_t)(b->cpu + 0x28000), 16, {(uint32_t)(frame * WALK_CHUNKS + chunk), 0, 0, 0}};
                if (chunk >= 2 && !(set % 3)) op(OP_UPDATE_UB, &unrelated, sizeof unrelated);
                if (chunk == 6 && set % 6 == 1) walk_copy((uint64_t)(uintptr_t)(b->cpu + 0x25000), b->ubo[1][1].addr, 16);
                for (int repeat = 0; repeat < WALK_REPEATS; repeat++) {
                    if (chunk == 6 && set >= 22 && repeat == WALK_REPEATS / 2)
                        walk_copy((uint64_t)(uintptr_t)(b->cpu + (set == 22 ? 0x26000 : 0x27000)),
                            set == 22 ? b->vertex.r[0].address : b->index.ibuf, set == 22 ? 24 : 12);
                    /* Real redundant bind opcodes, not just a loop around one frozen draw packet. */
                    op(OP_BIND_UBO, &b->ubo[0][0], sizeof b->ubo[0][0]); op(OP_BIND_UBO, &b->ubo[1][0], sizeof b->ubo[1][0]);
                    if (!(repeat % 4)) op(OP_BIND_SSBO, &b->storage[(repeat / 4) % 4], sizeof b->storage[0]);
                    if (churn) walk_churn(b, frame, set, chunk, repeat);
                    int x = set + (chunk == 6 && set >= 22 && repeat >= WALK_REPEATS / 2 ? 24 : 0);
                    walk_draw(b, x, chunk, repeat & 1);
                }
            }
            if (chunk == WALK_CHUNKS - 1) op(OP_COPY_T2B, &fixture.readback, sizeof fixture.readback);
            op(OP_END, NULL, 0); CHECK(rn <= WALK_STREAM_BYTES, "walking stream group fits fixed capacity");
            if (rn > WALK_STREAM_BYTES) return 1;
            memcpy(streams[chunk], rec, rn); rn = 0;
        }
        uint64_t previous[VK_REC_N], current[VK_REC_N], delta[VK_REC_N];
        vk_record_test_counts count0, count1; vk_record_test_counts_get(&count0);
        vk_record_test_profile_ns(previous, VK_REC_N);
        uint64_t desc0[8], desc1[8], api0[3], api1[3], wake0[4], wake1[4]; vk_record_test_descriptors(desc0); walk_api_counts(api0);
        vk_record_test_wakes(wake0);
        uint64_t serial = vk_submit_serial(), cpu_ns = 0, wall_ns = 0, seal_ns = 0;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(handles, WALK_CHUNKS, NULL);
        if (pending_service) atomic_store(&service.active, 1);
        for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) {
            uint64_t cpu0 = walk_clock(CLOCK_THREAD_CPUTIME_ID), wall0 = walk_clock(CLOCK_MONOTONIC);
            mtl_exec(streams[chunk]);
            wall_ns += walk_clock(CLOCK_MONOTONIC) - wall0; cpu_ns += walk_clock(CLOCK_THREAD_CPUTIME_ID) - cpu0;
            if (stress && chunk != WALK_CHUNKS - 1) {
                wall0 = walk_clock(CLOCK_MONOTONIC); vk_submit_chunk(); seal_ns += walk_clock(CLOCK_MONOTONIC) - wall0;
            }
        }
        atomic_store(&service.active, 0);
        uint64_t seal0 = walk_clock(CLOCK_MONOTONIC), fence = mtl_sub_commit(); seal_ns += walk_clock(CLOCK_MONOTONIC) - seal0;
        /* This fixture ordinarily has no presents. Give pass-level tracking
         * actual frame boundaries so its bounded tables do not measure one
         * endless frame, without resetting the production profiler via present. */
        if (framegen_boundary) vk_fg_extract_end();
        /* The getter is deliberately AFTER commit ended the active profile, never inside a timed scope. */
        vk_record_test_profile_ns(current, VK_REC_N);
        vk_record_test_counts_get(&count1);
        uint64_t accepted = count1.draws - count0.draws, freeze_hits = count1.freeze_hits - count0.freeze_hits;
        uint64_t program_hits = count1.program_hits - count0.program_hits;
        CHECK(accepted == WALK_DRAWS && count1.index_skips == count0.index_skips,
            "walking %s frame %d executes all %d indexed draws without rejected indices (%llu accepted)",
            case_names[phase], sample, WALK_DRAWS, (unsigned long long)accepted);
        for (int k = 0; k < VK_REC_N; k++) delta[k] = current[k] - previous[k];
        uint64_t wait0 = walk_clock(CLOCK_MONOTONIC);
        CHECK(mtl_wait(fence, 30000000000ull), "walking frame %d real Vulkan execution completes", frame);
        uint64_t wait_ns = walk_clock(CLOCK_MONOTONIC) - wait0, chunks = vk_submit_serial() - serial;
        vk_record_test_descriptors(desc1); walk_api_counts(api1); vk_record_test_wakes(wake1);
        CHECK(chunks == (stress ? WALK_CHUNKS : 1),
            "walking %s frame %d has %s actual Vulkan submissions (%llu observed)",
            case_names[phase], sample, stress ? "exactly twelve" : "exactly one", (unsigned long long)chunks);
        int bad = 0; uint64_t hash = 14695981039346656037ull; uint8_t *pixels = output + 0x20000;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            float expected[] = {0, 0, 0, 1};
            if (x < WALK_SETS && y < WALK_CHUNKS && !(x >= 22 && y > 6)) {
                float red = y < 2 ? (float)((frame + x) % 5 + 1) / 8 :
                    (float)((frame + x + (y + 1) / 3) % 4 + 1) / 8;
                walk_expected(frame, x, red, y >= 6 && x % 6 == 1, expected);
                if (churn) {
                    float original[4]; walk_tint(frame, x, 3, 0, original);
                    expected[0] += (walk_churn_red(frame, x, y) - original[0]) * .125f;
                }
            }
            const uint8_t *pixel = pixels + (y * 64 + x) * 4;
            if (!walk_pixel(pixel, expected)) {
                if (bad < 4) printf("lag-walk frame %d pixel (%d,%d) = %u,%u,%u,%u; expected %.3f,%.3f,%.3f,%.3f\n",
                    frame, x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                    expected[0] * 255, expected[1] * 255, expected[2] * 255, expected[3] * 255);
                bad++;
            }
            for (int c = 0; c < 4; c++) { hash ^= pixel[c]; hash *= 1099511628211ull; }
        }
        CHECK(!bad, "walking frame %d retains all deterministic pixels, including copy->live vertices/indices (%d wrong)", frame, bad);
        for (int set = 0; set < WALK_SETS; set += 8)
            CHECK(*(uint32_t *)(banks[set].cpu + 0x10100) == 0x89abcde1u, "walking frame %d set %d real storage producer executes", frame, set);
        char label[32];
        snprintf(label, sizeof label, "%s-%s%d", case_names[phase], warming ? "warmup" : "frame",
            warming || stress ? sample : sample - warmup);
        printf("lag-walk %s measured CPU ns/draw: mtl_exec_thread=%.2f mtl_exec_wall=%.2f seals_commit_wall=%.2f final_wait_wall=%.2f; "
               "actual_chunks=%llu pixels_fnv64=%016llx\n", label, cpu_ns / (double)WALK_DRAWS, wall_ns / (double)WALK_DRAWS,
            seal_ns / (double)WALK_DRAWS, wait_ns / (double)WALK_DRAWS, (unsigned long long)chunks, (unsigned long long)hash);
        walk_print_profile(label, delta, WALK_DRAWS);
        walk_print_hits(label, accepted, freeze_hits, program_hits, recorder);
        printf("lag-walk %s descriptors: sets=%llu calls=%llu resets=%llu walker_ns=%llu list_ns=%llu lists=%llu template_ns=%llu templates=%llu\n",
            label, (unsigned long long)(desc1[0] - desc0[0]), (unsigned long long)(desc1[1] - desc0[1]),
            (unsigned long long)(desc1[2] - desc0[2]), (unsigned long long)(desc1[3] - desc0[3]),
            (unsigned long long)(desc1[4] - desc0[4]), (unsigned long long)(desc1[5] - desc0[5]),
            (unsigned long long)(desc1[6] - desc0[6]), (unsigned long long)(desc1[7] - desc0[7]));
        if (call_counts_path)
            printf("lag-walk %s api: vkAllocateDescriptorSets=%llu allocated_descriptor_sets=%llu vkResetDescriptorPool=%llu\n",
                label, (unsigned long long)(api1[0] - api0[0]), (unsigned long long)(api1[1] - api0[1]),
                (unsigned long long)(api1[2] - api0[2]));
        if (!warming) {
            walk_measure *m = &measured[phase]; m->frames++;
            m->cpu += cpu_ns; m->wall += wall_ns; m->seal += seal_ns; m->wait += wait_ns;
            m->draws += accepted; m->freeze_hits += freeze_hits; m->program_hits += program_hits;
            for (int k = 0; k < VK_REC_N; k++) m->ns[k] += delta[k];
            for (int k = 0; k < 8; k++) m->descriptors[k] += desc1[k] - desc0[k];
            for (int k = 0; k < 3; k++) m->api[k] += api1[k] - api0[k];
            for (int k = 0; k < 4; k++) m->wakes[k] += wake1[k] - wake0[k];
        }
    }
    for (int phase = 0; phase < 3; phase++) {
        const walk_measure *m = &measured[phase]; double draws = (double)m->frames * WALK_DRAWS;
        char label[32]; snprintf(label, sizeof label, "%s-mean", case_names[phase]);
        printf("lag-walk %s measured CPU ns/draw: mtl_exec_thread=%.2f mtl_exec_wall=%.2f seals_commit_wall=%.2f final_wait_wall=%.2f; "
               "measured_frames=%d measured_draws=%.0f%s\n", label, m->cpu / draws, m->wall / draws, m->seal / draws, m->wait / draws,
            m->frames, draws, phase == 2 ? " (separate actual-chunk stress case)" : "");
        walk_print_profile(label, m->ns, draws); walk_print_hits(label, m->draws, m->freeze_hits, m->program_hits, recorder);
        double n = m->frames ? m->frames : 1;
        printf("lag-walk %s descriptors/frame: sets=%.2f calls=%.2f resets=%.2f walker_ns=%.0f list_ns=%.0f lists=%.2f template_ns=%.0f templates=%.2f\n",
            label, m->descriptors[0] / n, m->descriptors[1] / n, m->descriptors[2] / n, m->descriptors[3] / n,
            m->descriptors[4] / n, m->descriptors[5] / n, m->descriptors[6] / n, m->descriptors[7] / n);
        printf("lag-walk %s recorder wakes/frame: walker=%.2f worker_chained=%.2f polled_hits=%.2f parks=%.2f\n",
            label, m->wakes[0] / n, m->wakes[1] / n, m->wakes[2] / n, m->wakes[3] / n);
        if (call_counts_path)
            printf("lag-walk %s api/frame: vkAllocateDescriptorSets=%.2f allocated_descriptor_sets=%.2f vkResetDescriptorPool=%.2f\n",
                label, m->api[0] / n, m->api[1] / n, m->api[2] / n);
    }
    {   /* What the measured phases left retained (TOTK_VK_DESC_STOCK arms compare this, not only the call counts). */
        uint64_t kept[4]; vk_record_test_descriptor_retained(kept);
        printf("lag-walk descriptor retention: pools %llu (recorder %llu, primary %llu) ~%.1f MiB at the reported per-pool estimate, stock sets %llu\n",
            (unsigned long long)(kept[0] + kept[1]), (unsigned long long)kept[0], (unsigned long long)kept[1],
            (kept[0] + kept[1]) * kept[3] / 1048576.0, (unsigned long long)kept[2]);
    }
    if (pending_service) {
        atomic_store(&service.stop, 1); pthread_join(service_thread, NULL);
        printf("lag-walk owner-service pending requests: %llu completed across %d primary/stress frames (callbacks do no rendering work)\n",
            (unsigned long long)atomic_load(&service.calls), 2 * case_frames + stress_frames);
        CHECK(atomic_load(&service.calls) > 0, "walking benchmark actually services pending requests");
    }
    walk_device_bench(&fixture, output, frames, 100 + (uint64_t)(2 * case_frames + stress_frames), recorder);
    walk_share_bench(&fixture, &banks[WALK_SETS], output + 0x20000,
        100 + (uint64_t)(2 * case_frames + stress_frames + frames * 2 + 6), recorder);
    /* Keep safety outside all measurement windows. A failing historical build still emits its measured data,
       but every pixel/zero-wait assertion remains mandatory and the process exits nonzero on any failure. */
    fflush(stdout); /* preserve the measured before/after data even if the bounded strict fixture exits */
    walk_safety(&fixture, &banks[WALK_SETS], output + 0x20000);
    uint64_t prepare_calls, prepare_skips, prepare_checks;
    vk_draw_test_prepare_counts(&prepare_calls, &prepare_skips, &prepare_checks);
    printf("lag-prepare totals: %llu calls, %llu skipped, %llu oracle checks\n",
        (unsigned long long)prepare_calls, (unsigned long long)prepare_skips, (unsigned long long)prepare_checks);
    for (int chunk = 0; chunk < WALK_CHUNKS; chunk++) free(streams[chunk]);
    /* Drain before releasing alias/view/owner lifetimes. No fixture mapping is retired with worker packets in flight. */
    mtl_drain(); nvn_dirty_remove_range(output); mtl_pool_del(output); munmap(output_mapping, 0x50000);
    for (int set = WALK_SETS; set >= 0; set--) mtl_pool_del(banks[set].cpu + 0x10000);
    for (int p = WALK_POOLS - 1; p >= 0; p--) {
        nvn_dirty_remove_range(aliases + p * alias_size); mtl_vpool_del(aliases + p * alias_size);
        nvn_dirty_remove_range(pools + p * pool_size); mtl_pool_del(pools + p * pool_size);
    }
    munmap(virtual, virtual_size); munmap(physical, physical_size);
    printf("%s: lag-walk %d checks, %d failed; private UBO gated zero-wait checks %s\n", fails ? "FAILED" : "ok",
        checks, fails, vk_lag_uniform_private() ? "enabled" : "not applicable (lag off)");
    return fails != 0;
}

/* Fresh execs are intentional: the diagnostic and explicit size/format filter are cached. */
enum { TEXCOPY_W = 8, TEXCOPY_H = 8, TEXCOPY_BYTES = TEXCOPY_W * TEXCOPY_H * 2,
       TEXCOPY_ROUNDS = 2, TEXCOPY_FRAME = 170 };
void test_texcopy_initialize(tex_t *t, const nvn_texdesc *desc, const pool_t *pool, ptrdiff_t off);
uint64_t test_texcopy_submit(const tex_t *src, const tex_t *dst, uint8_t *readback,
    const float rgba[4], uint64_t *replay_sub);
void test_texcopy_release(const tex_t *t);
extern uint64_t g_frame_pub;

static int texcopy_child(void) {
    enum { POOL_BYTES = 1 << 20 };
    uint8_t *memory = mmap(NULL, 2 * POOL_BYTES, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("texcopy mmap"); return 1; }
    pool_t source_pool = {.mem = memory, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_t destination_pool = {.mem = memory + POOL_BYTES, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(source_pool.mem, source_pool.size); mtl_pool_add(destination_pool.mem, destination_pool.size);
    nvn_texdesc desc = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd,
        .width = TEXCOPY_W, .height = TEXCOPY_H, .depth = 1, .levels = 1};
    for (int round = 0; round < TEXCOPY_ROUNDS; round++) {
        g_frame_pub = TEXCOPY_FRAME + round;
        tex_t source, destination;
        test_texcopy_initialize(&source, &desc, &source_pool, round * 0x10000);
        test_texcopy_initialize(&destination, &desc, &destination_pool, round * 0x10000);
        uint8_t *pixels = destination_pool.mem + 0x80000 + round * 0x1000;
        memset(pixels, 0, TEXCOPY_BYTES);
        const float rgba[2][4] = {{.25f, .75f, 0, 1}, {.75f, .25f, 0, 1}};
        uint64_t record_sub = vk_submit_serial(), replay_sub = 0;
        uint64_t ticket = test_texcopy_submit(&source, &destination, pixels, rgba[round], &replay_sub);
        CHECK(mtl_wait(ticket, 5000000000ull), "texcopy round %d GPU copy completes", round);
        size_t nonzero = 0, bad = 0, source_cpu_nonzero = 0;
        for (size_t i = 0; i < TEXCOPY_BYTES; i++) {
            nonzero += pixels[i] != 0;
            int expected = (int)(rgba[round][i & 1] * 255 + .5f);
            bad += abs((int)pixels[i] - expected) > 1;
        }
        for (size_t i = 0; i < source.L.size; i++) source_cpu_nonzero += source.mem[i] != 0;
        CHECK(!bad && nonzero == TEXCOPY_BYTES, "texcopy round %d preserves both nonzero RG channels (%zu wrong, %zu nonzero)",
            round, bad, nonzero);
        CHECK(!source_cpu_nonzero, "texcopy round %d source comes from the render target, not prefilled CPU bytes", round);
        fprintf(stderr, "[texcopy-fixture] round=%d fp=%llu src_ser=%llu dst_ser=%llu src_mem=%#llx dst_mem=%#llx "
            "record_sub=%llu replay_sub=%llu bytes=%u nonzero=%zu bad=%zu\n", round,
            (unsigned long long)g_frame_pub, (unsigned long long)source.serial, (unsigned long long)destination.serial,
            (unsigned long long)(uintptr_t)source.mem, (unsigned long long)(uintptr_t)destination.mem,
            (unsigned long long)record_sub, (unsigned long long)replay_sub, TEXCOPY_BYTES, nonzero, bad);
        test_texcopy_release(&source); test_texcopy_release(&destination);
    }
    mtl_drain(); mtl_pool_del(destination_pool.mem); mtl_pool_del(source_pool.mem);
    munmap(memory, 2 * POOL_BYTES);
    return fails != 0;
}

static const char *texcopy_value(const char *line, const char *key) {
    size_t n = strlen(key);
    for (const char *p = line; (p = strstr(p, key)); p++)
        if ((p == line || p[-1] == ' ') && p[n] == '=') return p + n + 1;
    return NULL;
}
static int texcopy_word(const char *line, const char *key, const char *value) {
    const char *p = texcopy_value(line, key); size_t n = strlen(value);
    return p && !strncmp(p, value, n) && (!p[n] || p[n] == ' ');
}
static int texcopy_number(const char *line, const char *key, uint64_t *value) {
    const char *p = texcopy_value(line, key); char *end;
    if (!p || *p == '-') return 0;
    errno = 0; unsigned long long v = strtoull(p, &end, 0);
    if (errno || end == p || (*end && *end != ' ')) return 0;
    *value = v; return 1;
}
static int texcopy_equal(const char *line, const char *key, uint64_t expected) {
    uint64_t value; return texcopy_number(line, key, &value) && value == expected;
}
typedef struct {
    uint64_t src_ser, dst_ser, src_mem, dst_mem, record_sub, replay_sub, image[2], readback_image[2], readback_sub[2];
    int marker, record, attempt, success, images[2], readbacks[2], valid;
} texcopy_observation;

static void texcopy_assert_log(char *log, size_t len, int enabled) {
    texcopy_observation rounds[TEXCOPY_ROUNDS] = {0}; size_t traces = 0;
    for (size_t i = 0; i < len; i++) if (log[i] == '\n') log[i] = 0;
    for (char *line = log; line < log + len; line += strlen(line) + 1) {
        if (!strstr(line, "[texcopy-fixture]")) continue;
        uint64_t round; if (!texcopy_number(line, "round", &round) || round >= TEXCOPY_ROUNDS) continue;
        texcopy_observation *r = &rounds[round]; r->marker++;
        r->valid = texcopy_number(line, "src_ser", &r->src_ser) && texcopy_number(line, "dst_ser", &r->dst_ser) &&
            texcopy_number(line, "src_mem", &r->src_mem) && texcopy_number(line, "dst_mem", &r->dst_mem) &&
            texcopy_number(line, "record_sub", &r->record_sub) && texcopy_number(line, "replay_sub", &r->replay_sub) &&
            texcopy_equal(line, "fp", TEXCOPY_FRAME + round) && texcopy_equal(line, "bytes", TEXCOPY_BYTES) &&
            texcopy_equal(line, "nonzero", TEXCOPY_BYTES) && texcopy_equal(line, "bad", 0) &&
            r->src_ser && r->dst_ser && r->src_ser != r->dst_ser && r->src_mem && r->dst_mem && r->src_mem != r->dst_mem;
    }
    for (char *line = log; line < log + len; line += strlen(line) + 1) {
        if (!strstr(line, "[texcopy]")) continue;
        traces++;
        int frontend = texcopy_word(line, "op", "nvnCommandBufferCopyTextureToTexture");
        if (!frontend && !texcopy_word(line, "op", "CopyTextureToTexture")) continue;
        uint64_t frame; int have_frame = texcopy_number(line, "fp", &frame);
        CHECK(have_frame && frame >= TEXCOPY_FRAME && frame < TEXCOPY_FRAME + TEXCOPY_ROUNDS,
            "texcopy T2T record carries its actual fixture frame");
        if (!have_frame || frame < TEXCOPY_FRAME || frame >= TEXCOPY_FRAME + TEXCOPY_ROUNDS) continue;
        texcopy_observation *r = &rounds[frame - TEXCOPY_FRAME];
        if (frontend) {
            r->record++;
            r->valid &= texcopy_word(line, "phase", "record") && texcopy_word(line, "sub_kind", "backend-at-record") &&
                texcopy_equal(line, "sub", r->record_sub) &&
                texcopy_equal(line, "src_ser", r->src_ser) && texcopy_equal(line, "dst_ser", r->dst_ser) &&
                texcopy_equal(line, "src_mem", r->src_mem) && texcopy_equal(line, "dst_mem", r->dst_mem) &&
                texcopy_word(line, "src_region", "0,0,0,8,8,1") && texcopy_word(line, "dst_region", "0,0,0,8,8,1");
        } else if (texcopy_word(line, "phase", "replay") &&
                (texcopy_word(line, "result", "attempt") || texcopy_word(line, "result", "success"))) {
            int success = texcopy_word(line, "result", "success");
            if (success) r->success++; else r->attempt++;
            r->valid &= texcopy_equal(line, "sub", r->replay_sub) &&
                texcopy_equal(line, "src_serial", r->src_ser) && texcopy_equal(line, "dst_serial", r->dst_ser);
            if (success) r->valid &= texcopy_word(line, "kind", "copy") && texcopy_equal(line, "encoded", 1);
        } else if (texcopy_word(line, "phase", "image") || texcopy_word(line, "phase", "readback")) {
            int side = texcopy_word(line, "side", "src") ? 0 : texcopy_word(line, "side", "dst") ? 1 : -1;
            if (side < 0) { r->valid = 0; continue; }
            uint64_t serial = side ? r->dst_ser : r->src_ser, mem = side ? r->dst_mem : r->src_mem, sub = 0, image = 0;
            r->valid &= texcopy_equal(line, "ser", serial) && texcopy_equal(line, "mem", mem) &&
                texcopy_number(line, "sub", &sub) && sub >= r->replay_sub &&
                texcopy_number(line, "image", &image) && image != 0;
            if (texcopy_word(line, "phase", "image")) {
                r->images[side]++; r->image[side] = image; r->valid &= sub == r->replay_sub;
                r->valid &= texcopy_equal(line, "cpu_pool", side) && texcopy_equal(line, "pin", 1) &&
                    texcopy_equal(line, "live_ser", serial) && texcopy_equal(line, "finalized", 0);
                if (!side) r->valid &= texcopy_equal(line, "gpu_written", 1);
            } else {
                uint64_t sample = 0;
                r->readbacks[side]++; r->readback_image[side] = image; r->readback_sub[side] = sub;
                r->valid &= texcopy_word(line, "result", "complete") &&
                    texcopy_number(line, "sample", &sample) && sample > 0 && sample <= 32;
                r->valid &= texcopy_equal(line, "bytes", TEXCOPY_BYTES) && texcopy_equal(line, "nonzero", TEXCOPY_BYTES) &&
                    texcopy_equal(line, "level", 0) && texcopy_equal(line, "layer", 0) &&
                    texcopy_word(line, "region", "0,0,0:8x8x1");
            }
        }
    }
    CHECK(enabled ? traces != 0 : traces == 0, "texcopy %s run has the expected diagnostic presence (%zu records)",
        enabled ? "selected" : "suppressed", traces);
    for (int round = 0; round < TEXCOPY_ROUNDS; round++) {
        texcopy_observation *r = &rounds[round];
        CHECK(r->marker == 1 && r->valid, "texcopy round %d actual pixels and all observed diagnostic identities/counters agree", round);
        if (!enabled) continue;
        CHECK(r->record == 1 && r->attempt == 1 && r->success == 1,
            "texcopy round %d has exactly one frontend record and replay attempt/success (%d/%d/%d)", round, r->record, r->attempt, r->success);
        for (int side = 0; side < 2; side++) CHECK(r->images[side] == 1 && r->readbacks[side] == 1 &&
            r->image[side] == r->readback_image[side] && r->image[side] != r->image[!side],
            "texcopy round %d %s actual image has one complete nonzero readback", round, side ? "destination" : "source");
        CHECK(r->readback_sub[0] == r->replay_sub && r->readback_sub[1] > r->readback_sub[0],
            "texcopy round %d source readback follows the actual copy submission, destination follows the source wait", round);
    }
    CHECK(rounds[0].src_ser != rounds[1].src_ser && rounds[0].dst_ser != rounds[1].dst_ser,
        "texcopy second recording uses independently initialized texture serials");
}
static int texcopy_suite(void) {
    static const struct { const char *name, *trace, *filter; int enabled; } modes[] = {
        {"default-off", NULL, "8x8:d", 0}, {"requires-filter", "1", NULL, 0},
        {"invalid-filter", "1", "invalid", 0}, {"nonmatching-filter", "1", "9x9:d", 0},
        {"nonmatching-format", "1", "8x8:25", 0},
        {"requires-exact-enable", "2", "8x8:d", 0}, {"selected", "1", "8x8:d", 1}
    };
    for (unsigned mode = 0; mode < sizeof modes / sizeof *modes; mode++) {
        int fds[2]; if (pipe(fds)) { perror("texcopy pipe"); return 1; }
        pid_t child = fork();
        if (child < 0) { perror("texcopy fork"); close(fds[0]); close(fds[1]); return 1; }
        if (!child) {
            close(fds[0]);
            if (dup2(fds[1], STDERR_FILENO) < 0 || dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
            close(fds[1]);
            if (modes[mode].trace) setenv("TOTK_VK_TEXCOPY_TRACE", modes[mode].trace, 1); else unsetenv("TOTK_VK_TEXCOPY_TRACE");
            if (modes[mode].filter) setenv("DM_NVN_LIFELOG", modes[mode].filter, 1); else unsetenv("DM_NVN_LIFELOG");
            setenv("TOTK_VK_WRITEBACK", "0", 1); /* only the explicit T2B readback supplies the expected pixels */
            execl("/proc/self/exe", "vkdrawtest", "--texcopy-child", (char *)NULL); _exit(127);
        }
        close(fds[1]);
        static char log[256 * 1024]; size_t len = 0; int overflow = 0, read_error = 0; char discard[4096];
        for (;;) {
            char *out = len < sizeof log - 1 ? log + len : discard;
            size_t available = len < sizeof log - 1 ? sizeof log - 1 - len : sizeof discard;
            ssize_t n = read(fds[0], out, available);
            if (n < 0) { if (errno == EINTR) continue; read_error = 1; break; }
            if (!n) break;
            if (out == discard) overflow = 1; else len += (size_t)n;
        }
        close(fds[0]); log[len] = 0;
        int status = 0; pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        printf("texcopy mode=%s frame_lag=%s recorder_threads=%s\n", modes[mode].name,
            getenv("TOTK_FRAME_LAG") ? getenv("TOTK_FRAME_LAG") : "default",
            getenv("TOTK_VK_REC_THREADS") ? getenv("TOTK_VK_REC_THREADS") : "default");
        fputs(log, stdout);
        CHECK(waited == child && WIFEXITED(status) && !WEXITSTATUS(status) && !overflow && !read_error,
            "texcopy %s child completes without truncated capture", modes[mode].name);
        texcopy_assert_log(log, len, modes[mode].enabled);
    }
    printf("%s: texcopy diagnostic %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

/* Fresh execs keep the default/on/off policy independent of this file's blocking-invalidate checks. */
static void settled_render(tex_t *t) {
    for (unsigned l = 0; l < t->d.levels; l++) for (unsigned layer = 0; layer < t->L.layers; layer++) {
        p_set_rt rt = {.n = 1}; rt.color[0] = t; rt.has_cview[0] = 1;
        rt.cview[0] = (view_t){.set_levels = 1, .base_level = l, .nlevels = 1, .set_layers = 1, .min_layer = layer, .nlayers = 1};
        p_clear_color clear = {0, 0xf, {!((l + layer) & 1), !((l + 2 * layer) & 2), 0, 1}};
        op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &clear, sizeof clear);
    }
    p_set_rt none = {0}; op(OP_SET_RT, &none, sizeof none);
}
static void settled_drain_rewrite(void *arg) {
    tex_t *t = arg;
    p_set_rt rt = {.n = 1}, none = {0}; rt.color[0] = t; rt.has_cview[0] = 1;
    rt.cview[0] = (view_t){.set_levels = 1, .nlevels = 1};
    p_clear_color zero = {0, 0xf, {0, 0, 0, 1}};
    op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &zero, sizeof zero); op(OP_SET_RT, &none, sizeof none);
    CHECK(mtl_wait(run(), 5000000000ull), "frontend drain replays a genuine newer GPU producer");
}
typedef struct {
    const pool_t *pool; uint64_t off, size;
    const void *expected; size_t compare;
    _Atomic int returned; int bad;
} settled_consumer;
static void *settled_consumer_run(void *arg) {
    settled_consumer *c = arg;
    mtl_pool_invalidate(c->pool, c->off, c->size);
    if (c->expected) c->bad = memcmp(c->pool->mem + c->off, c->expected, c->compare) != 0;
    atomic_store_explicit(&c->returned, 1, memory_order_release); return NULL;
}
static void settled_idle(unsigned count) {
    int complete = 1;
    for (unsigned i = 0; i < count; i++) complete &= mtl_wait(run(), 5000000000ull);
    vk_rec_lock(); vk_res_sweep(); vk_rec_unlock();
    CHECK(complete, "%u ordinary idle submissions complete", count);
}
static void settled_wait_landed(uint64_t goal) {
    uint64_t stats[7]; unsigned n = 0;
    for (; n < 4096; n++) {
        vk_wb_test_stats(stats); if (stats[0] >= goal) break;
        if (!mtl_wait(run(), 5000000000ull)) break;
    }
    CHECK(n < 4096 && stats[0] >= goal, "budgeted landings finish in ordinary commits without invalidate");
}
static void settled_expected(const tex_t *t, uint8_t *expected) {
    memset(expected, 0x39, t->L.size);
    uint8_t *linear = malloc((size_t)t->d.width * t->d.height * 2);
    for (unsigned l = 0; l < t->d.levels; l++) for (unsigned layer = 0; layer < t->L.layers; layer++) {
        unsigned w = t->d.width >> l, h = t->d.height >> l; if (!w) w = 1; if (!h) h = 1;
        for (unsigned p = 0; p < w * h; p++) { linear[2 * p] = (l + layer) & 1 ? 0 : 255; linear[2 * p + 1] = (l + 2 * layer) & 2 ? 0 : 255; }
        CHECK(!nvn_swizzle(&t->d, l, layer, linear, w * 2, expected), "reference NVN swizzle level/layer %u/%u", l, layer);
    }
    free(linear);
}
static int settled_child(int disabled) {
    const char *setting = getenv("TOTK_VK_SETTLED_WB_SUBS");
    unsigned quiet = setting ? (unsigned)strtoul(setting, NULL, 10) : 60;
    CHECK(getenv("TOTK_VK_WRITEBACK") == NULL, "settled fixture never enables synchronous write-back");
    enum { POOL_BYTES = 1 << 20 };
    uint8_t *memory = mmap(NULL, 2 * POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("settled mmap"); return 1; }
    pool_t pool = {.mem = memory, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, 2 * POOL_BYTES); mtl_pool_add(pool.mem, pool.size);
    nvn_dirty_add_range(memory, 2 * POOL_BYTES);
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd,
        .width = 260, .height = 260, .depth = 1, .levels = 9}, .mem = memory, .pool = &pool, .serial = 73001};
    nvn_tex_layout(&t.d, &t.L); memset(memory, 0x39, t.L.size);
    uint8_t *expected = malloc(t.L.size); settled_expected(&t, expected);
    uint64_t before[7], after[7]; vk_wb_test_stats(before);
    settled_render(&t); CHECK(mtl_wait(run(), 5000000000ull), "one nine-level RG8 rendering completes");
    size_t untouched = 0; for (size_t i = 0; i < t.L.size; i++) untouched += memory[i] != 0x39;
    CHECK(!untouched, "GPU rendering initially leaves every guest backing byte untouched");
    mtl_tex_release(&t); /* the write-back reference must keep the image, not its temporary texture object */
    settled_idle(quiet + 2); vk_wb_test_stats(after);
    if (!disabled) { settled_wait_landed(before[0] + 1); vk_wb_test_stats(after); }
    if (disabled) {
        size_t bad = 0; for (size_t i = 0; i < t.L.size; i++) bad += memory[i] != 0x39;
        CHECK(!bad && after[0] == 0 && after[5] == 0 && after[6] == 0,
            "TOTK_VK_OFF=settledwb preserves every uploaded byte and allocates no staging");
    } else {
        CHECK(!memcmp(memory, expected, t.L.size), "all nine levels and padding equal the rendered NVN-layout reference without invalidate");
        CHECK(after[0] == before[0] + 1 && after[1] == before[1] + t.L.size && !after[2] && !after[5],
            "exactly one settled rendering lands and releases staging/image record (%llu bytes)", (unsigned long long)t.L.size);
        uint64_t tile[6], tile_after[6], sync_before[7], sync_after[7]; vk_wb_test_tile(&t, tile);
        CHECK(tile[0] && tile[0] == tile[1] && tile[3] == t.serial,
            "tile evidence retains the exact landed GPU version/serial after the pending record is dropped");
        vk_wb_test_sync(sync_before);
        mtl_pool_invalidate(&pool, 0, t.L.size); vk_wb_test_tile(&t, tile_after); vk_wb_test_sync(sync_after);
        CHECK(tile_after[4] == tile[4] + 1 && tile_after[5] == tile[5] && tile_after[0] == tile_after[1],
            "invalidate with no armed record reports this exact GPU version already landed, not an unknown miss");
        CHECK(sync_after[2] == sync_before[2] && sync_after[3] == sync_before[3],
            "retained tile diagnostics never drain or request recording ownership for an already landed rendering");
        settled_idle(quiet + 2); vk_wb_test_stats(before);
        CHECK(before[0] == after[0] && before[6] == after[6], "settled content is written back at most once");
        /* Device repro: invalidate/read can happen at age24–50, before K60. Fence-gate the actual producer,
           then read every guest-layout byte immediately after invalidate, with full legacy WB still unset. */
        t.serial++; memset(memory, 0x39, t.L.size);
        VkEvent fresh_event; VkEventCreateInfo fresh_ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &fresh_ci, NULL, &fresh_event));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &fresh_event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        settled_render(&t); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        uint64_t fresh_producer = mtl_sub_commit(), fresh_before[3], fresh_after[3], fresh_tile[6];
        vk_wb_test_tile(&t, fresh_tile); vk_wb_test_fresh(fresh_before);
        CHECK(!mtl_wait(fresh_producer, 0) && vk_submit_serial() - fresh_tile[2] < quiet,
            "explicit consumer starts before K with its real producer fence still unsignalled");
        lease_gate fresh_release = {.gate = fresh_event}; atomic_store(&fresh_release.release, 1);
        pthread_t fresh_opener;
        if (pthread_create(&fresh_opener, NULL, lease_gate_run, &fresh_release)) DIE("fresh invalidate gate opener");
        mtl_pool_invalidate(&pool, 0, t.L.size);
        pthread_join(fresh_opener, NULL); vk_wb_test_fresh(fresh_after);
        CHECK(!fresh_release.timed_out && mtl_wait(fresh_producer, 0) && !memcmp(memory, expected, t.L.size),
            "fresh default invalidate waits its ordered copy/producer and exposes all nine NVN-layout levels");
        CHECK(fresh_after[0] == fresh_before[0] + 1 && fresh_after[1] == fresh_before[1] + t.L.size &&
            fresh_after[2] >= fresh_before[2], "fresh landings count exact bytes and relevant copy-fence wait");
        vkDestroyEvent(V.dev, fresh_event, NULL); mtl_tex_release(&t);
        vk_wb_test_stats(before); vk_wb_test_tile(&t, tile);

        /* A live target rewritten every submission never stages, even across many quiet-interval lengths. */
        t.serial++; memset(memory, 0x39, t.L.size);
        int complete = 1; uint64_t serial = vk_submit_serial();
        for (unsigned i = 0; i < quiet * 2 + 3; i++) { settled_render(&t); complete &= mtl_wait(run(), 5000000000ull); }
        vk_wb_test_stats(after);
        CHECK(complete && after[0] == before[0] && after[6] == before[6] && !after[5] &&
            vk_submit_serial() - serial == quiet * 2 + 3, "per-submission rendering performs zero copies/landings/extra submissions");
        CHECK(memory[0] == 0x39 && memory[t.L.size - 1] == 0x39, "per-submission target stays GPU-only");
        vk_wb_test_tile(&t, tile_after);
        CHECK(tile_after[0] > tile[0] && tile_after[1] == tile[1] && tile_after[3] == t.serial,
            "every GPU rewrite re-arms a new diagnostic version even after an earlier landing");
        vk_wb_range_gone((uintptr_t)memory, (uintptr_t)memory + POOL_BYTES); mtl_tex_release(&t);

        t.serial++; memset(memory, 0x39, t.L.size); settled_render(&t);
        CHECK(mtl_wait(run(), 5000000000ull), "CPU-newer producer completes");
        lag_guest_store(memory, 0x6d); /* real tracked CPU write, not an invalidate */
        memcpy(expected, memory, t.L.size); vk_wb_test_stats(before);
        mtl_pool_invalidate(&pool, 0, t.L.size); /* CPU data must also win at a FRESH explicit consumer. */
        settled_idle(quiet + 2); vk_wb_test_stats(after);
        CHECK(!memcmp(memory, expected, t.L.size) && after[0] == before[0] && after[3] == before[3] + 1,
            "CPU write after rendering preserves the entire newer guest range");
        mtl_tex_release(&t);
        vk_wb_test_tile(&t, tile);
        mtl_pool_invalidate(&pool, 0, t.L.size); vk_wb_test_tile(&t, tile_after);
        CHECK(tile_after[0] == tile[0] && tile_after[1] != tile_after[0] && tile_after[5] == tile[5] + 1,
            "CPU disarm retains the missing-landed-version evidence at a later no-record invalidate");
        /* Freshness must also be checked at LANDING, not only when the image copy is encoded. */
        t.serial++; memset(memory, 0x39, t.L.size); settled_render(&t);
        CHECK(mtl_wait(run(), 5000000000ull), "late-CPU-write producer completes");
        settled_idle(quiet - 1); vk_wb_test_stats(before);
        VkEvent late_event; VkEventCreateInfo late_ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &late_ci, NULL, &late_event));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &late_event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t late_fence = mtl_sub_commit(); vk_wb_test_stats(after);
        CHECK(after[6] == before[6] + 1 && !mtl_wait(late_fence, 0), "late CPU write races a real outstanding staging copy");
        lag_guest_store(memory, 0x7e); memcpy(expected, memory, t.L.size);
        VKCHECK(vkSetEvent(V.dev, late_event)); CHECK(mtl_wait(late_fence, 5000000000ull), "late-CPU-write staging completes");
        settled_idle(2); vk_wb_test_stats(after);
        CHECK(!memcmp(memory, expected, t.L.size) && after[0] == before[0] && after[3] == before[3] + 1 && !after[5],
            "a CPU write after staging is not overwritten when its fence signals");
        vkDestroyEvent(V.dev, late_event, NULL); mtl_tex_release(&t);

        if (nvn_dirty_lag_enabled()) {
            /* A completed image copy must not make its CPU landing wait for a NEWER protected-memory reader. */
            t.serial++; memset(memory, 0x39, t.L.size); settled_render(&t);
            CHECK(mtl_wait(run(), 5000000000ull), "deferred-landing producer completes");
            settled_idle(quiet - 1); vk_wb_test_stats(before);
            VkEvent gates[2]; VkEventCreateInfo gate_ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            for (unsigned i = 0; i < 2; i++) VKCHECK(vkCreateEvent(V.dev, &gate_ci, NULL, &gates[i]));
            mtl_sub_begin();
            vkCmdWaitEvents(vk_cb(), 1, &gates[0], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
            uint64_t copied = mtl_sub_commit();
            /* Register the next reader before queueing it, just like normal pre-submit lag arming. Observe the
               copy first: lavapipe may batch an earlier fence's timestamp/post processing behind a later event. */
            CHECK(nvn_dirty_lag_protect(memory, t.L.size, copied + 1), "next reader registers its future protected-lag ticket");
            VKCHECK(vkSetEvent(V.dev, gates[0])); CHECK(mtl_wait(copied, 5000000000ull), "image staging completes before queueing the newer reader");
            vk_rec_lock(); vk_res_sweep(); vk_rec_unlock(); vk_wb_test_stats(after);
            CHECK(mtl_last_fence() == copied && after[0] == before[0] && after[2] == 1 && after[5] > 0,
                "completed staging defers without submitting or waiting for a future protected-memory reader");
            mtl_sub_begin();
            VkCommandBuffer cb = vk_cb();
            vkCmdWaitEvents(cb, 1, &gates[1], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
            VkBuffer source, destination; uint64_t source_off, destination_off;
            int have_source = vk_resolve((uintptr_t)memory, t.L.size, &source, &source_off);
            int have_destination = vk_resolve((uintptr_t)memory + POOL_BYTES + 0x80000, t.L.size, &destination, &destination_off);
            CHECK(have_source && have_destination, "newer reader resolves the real imported guest buffer");
            if (!have_source || !have_destination) DIE("settled reader buffers unavailable");
            VkBufferCopy region = {source_off, destination_off, t.L.size};
            vkCmdCopyBuffer(cb, source, destination, 1, &region);
            uint64_t reader = mtl_sub_commit();
            CHECK(reader == copied + 1 && !mtl_wait(reader, 0), "the armed reader ticket is the actual next gated submission");
            VKCHECK(vkSetEvent(V.dev, gates[1])); CHECK(mtl_wait(reader, 5000000000ull), "newer protected-memory reader completes");
            settled_idle(1); vk_wb_test_stats(after);
            settled_wait_landed(before[0] + 1); vk_wb_test_stats(after);
            CHECK(after[0] == before[0] + 1 && !after[2] && !after[5], "deferred landing drains after its memory reader completes");
            for (unsigned i = 0; i < 2; i++) vkDestroyEvent(V.dev, gates[i], NULL);
            mtl_tex_release(&t);
        }

        /* Independent landing scenarios use a new real pool registration, not a reset of learned storage state. */
        mtl_pool_del(pool.mem); mtl_pool_add(pool.mem, pool.size);
        /* Re-rendering a staged image cancels the old copy and re-arms exactly one newer rendering. */
        t.serial++; memset(memory, 0x39, t.L.size); settled_render(&t);
        CHECK(mtl_wait(run(), 5000000000ull), "superseded-copy producer completes");
        settled_idle(quiet - 1); vk_wb_test_stats(before);
        VkEvent rewrite_event; VkEventCreateInfo rewrite_ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &rewrite_ci, NULL, &rewrite_event));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &rewrite_event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t old_fence = mtl_sub_commit();
        p_set_rt rewrite_rt = {.n = 1}; rewrite_rt.color[0] = &t;
        p_clear_color green = {0, 0xf, {0, 1, 0, 1}};
        p_set_rt none = {0};
        op(OP_SET_RT, &rewrite_rt, sizeof rewrite_rt); op(OP_CLEAR_COLOR, &green, sizeof green); op(OP_SET_RT, &none, sizeof none);
        uint64_t new_fence = run(); vk_wb_test_stats(after);
        CHECK(after[4] == before[4] + 1 && !mtl_wait(old_fence, 0), "new image write supersedes the outstanding old copy");
        VKCHECK(vkSetEvent(V.dev, rewrite_event)); CHECK(mtl_wait(new_fence, 5000000000ull), "newer rendering executes after the old copy");
        settled_idle(quiet + 2); vk_wb_test_stats(after);
        settled_wait_landed(before[0] + 1); vk_wb_test_stats(after);
        uint8_t pixel[260 * 260 * 2]; nvn_deswizzle(&t.d, 0, 0, memory, pixel, 520);
        size_t wrong = 0; for (unsigned i = 0; i < 260 * 260; i++) wrong += pixel[2 * i] != 0 || pixel[2 * i + 1] != 255;
        CHECK(!wrong && after[0] == before[0] + 1 && !after[2] && !after[5], "only the newest rendering lands after staged-copy supersession");
        vkDestroyEvent(V.dev, rewrite_event, NULL); mtl_tex_release(&t);
        mtl_pool_del(pool.mem); mtl_pool_add(pool.mem, pool.size);

        pool_t bounded = pool; bounded.mem = memory + POOL_BYTES; bounded.size = 4096;
        mtl_pool_add(bounded.mem, bounded.size);
        tex_t outside = t; outside.serial = 73100; outside.pool = &bounded; outside.mem = bounded.mem;
        memset(outside.mem, 0x39, outside.L.size); vk_wb_test_stats(before); settled_render(&outside);
        CHECK(mtl_wait(run(), 5000000000ull), "partially out-of-pool rendering completes");
        settled_idle(quiet + 2); vk_wb_test_stats(after);
        CHECK(after[0] == before[0] && after[6] == before[6] && outside.mem[0] == 0x39,
            "full destination range must fit its exact pool, not just the larger imported backing");
        mtl_tex_release(&outside); mtl_pool_del(bounded.mem);

        /* Gate the actual copy, then finalize the nested pool before its fence can signal. Deleting a VIEW does not
           wait for the owner buffer; PROT_NONE makes an accidental landing into the finalized destination fatal. */
        t.serial++; memset(memory, 0x39, t.L.size); settled_render(&t);
        CHECK(mtl_wait(run(), 5000000000ull), "pool-retirement producer completes");
        settled_idle(quiet - 1); vk_wb_test_stats(before);
        VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t fence = mtl_sub_commit(); vk_wb_test_stats(after);
        CHECK(after[6] == before[6] + 1 && after[5] > 0 && !mtl_wait(fence, 0), "settled copy is pending behind the host gate");
        uint64_t old_identity = vk_wb_pool_identity(&pool, memory, t.L.size);
        mtl_pool_del(pool.mem); mtl_tex_release(&t);
        CHECK(!mprotect(memory, POOL_BYTES, PROT_NONE), "retired pool destination becomes inaccessible");
        VKCHECK(vkSetEvent(V.dev, event)); CHECK(mtl_wait(fence, 5000000000ull), "cancelled GPU copy completes normally");
        settled_idle(2); vk_wb_test_stats(after);
        CHECK(after[0] == before[0] && !after[2] && !after[5], "pool freed before landing drops the result and all staging");
        CHECK(!mprotect(memory, POOL_BYTES, PROT_READ | PROT_WRITE), "retired pool destination can be inspected again");
        size_t bad = 0; for (size_t i = 0; i < t.L.size; i++) bad += memory[i] != 0x39;
        CHECK(!bad, "pool retirement wrote no guest bytes");
        vkDestroyEvent(V.dev, event, NULL);
        mtl_pool_add(pool.mem, pool.size);
        int revived = vk_wb_pool_lock(old_identity, memory, t.L.size); if (revived) vk_wb_pool_unlock();
        CHECK(old_identity && !revived, "a same-address pool registration does not revive the retired destination identity");
        mtl_pool_del(pool.mem);
    }
    vk_wb_stats();
    if (disabled) mtl_pool_del(pool.mem);
    nvn_dirty_remove_range(memory); mtl_pool_del(memory); free(expected); munmap(memory, 2 * POOL_BYTES);
    printf("%s: settled write-back %s K=%u: %d checks, %d failed\n", fails ? "FAILED" : "ok",
        disabled ? "off" : "on", quiet, checks, fails);
    return fails != 0;
}
static int settled_learning(void) {
    enum { POOL_BYTES = 32 << 20, TILES = 64, STRIDE = 0x60000, PERIOD = 100, ROUNDS = 40 };
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_OFF"); unsetenv("TOTK_VK_SETTLED_WB_SUBS");
    uint8_t *memory = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("settled learning mmap"); return 1; }
    pool_t pool = {.mem = memory, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, pool.size); nvn_dirty_add_range(memory, pool.size);
    tex_t tiles[TILES];
    for (unsigned i = 0; i < TILES; i++) {
        tiles[i] = (tex_t){.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd,
            .width = 260, .height = 260, .depth = 1, .levels = 9},
            .mem = memory + i * STRIDE, .pool = &pool, .serial = 75000 + i};
        nvn_tex_layout(&tiles[i].d, &tiles[i].L);
        CHECK(tiles[i].L.size <= STRIDE, "normal tile %u fits its storage slot", i);
        memset(tiles[i].mem, 0x39, tiles[i].L.size); settled_render(&tiles[i]);
    }
    CHECK(mtl_wait(run(), 5000000000ull), "64-tile bake completes in one ordinary submission");
    for (unsigned i = 0; i < TILES; i++) mtl_tex_release(&tiles[i]);
    settled_idle(62);
    settled_wait_landed(TILES);
    uint8_t *expected = malloc(tiles[0].L.size); settled_expected(&tiles[0], expected);
    for (unsigned i = 0; i < TILES; i++) CHECK(!memcmp(tiles[i].mem, expected, tiles[i].L.size),
        "first-use tile %u lands every level promptly despite learning", i);
    free(expected);
    uint64_t before[7], after[7], allocation[5], history[2], resources[8];
    vk_wb_test_stats(before); vk_wb_test_memory(allocation);
    CHECK(before[0] == TILES && before[6] == TILES && !before[2] && !before[5], "one prompt landing for each of the 64 baked storages");
    CHECK(!allocation[0] && !allocation[2] && !allocation[3] && allocation[4] == TILES && allocation[1] < 32768,
        "landed tiles retain only small image-free learning metadata, not record capacity or image pins");
    vk_res_mem(resources);
    CHECK(resources[0] && resources[7] && resources[4] == TILES, "landed images stay in the ordinary cache; blocks remain reusable");

    tex_t target = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 8, .height = 8,
        .depth = 1, .levels = 1}, .mem = memory + (26 << 20), .pool = &pool, .serial = 76000};
    nvn_tex_layout(&target.d, &target.L); memset(target.mem, 0x39, target.L.size);
    p_set_rt rt = {.n = 1}, none = {0}; rt.color[0] = &target;
    int complete = 1;
    for (unsigned cycle = 0; cycle < ROUNDS; cycle++) {
        target.serial++; p_clear_color clear = {0, 0xf, {cycle & 1 ? 0 : 1, 1, 0, 1}};
        op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &clear, sizeof clear); op(OP_SET_RT, &none, sizeof none);
        complete &= mtl_wait(run(), 5000000000ull); mtl_tex_release(&target);
        settled_idle(PERIOD - 1); vk_wb_test_stats(after);
        CHECK(after[0] == before[0] + 1 && after[6] == before[6] + 1 && target.mem[0] == 255,
            "100-submission intermittent refresh %u never restages after its first landing", cycle);
    }
    CHECK(complete, "all intermittent producers complete");
    vk_wb_test_history(&target, history); vk_wb_test_memory(allocation);
    CHECK(history[0] == 1 && history[1] == 120, "learning survives new texture serials and raises the interval above the 100-submission period");
    CHECK(allocation[2] == allocation[3] && allocation[2] < (1 << 20) && allocation[0] < 32768 && allocation[1] < 32768,
        "steady state pins only the one still-unlanded small image; staging/history/records have no high-water burst");
    uint64_t invalidated = vk_wb_test_invalidates(), serial = vk_submit_serial();
    mtl_pool_invalidate(&pool, (uintptr_t)target.mem - (uintptr_t)pool.mem, target.L.size);
    vk_wb_test_stats(after); vk_wb_test_history(&target, history); vk_wb_test_memory(allocation);
    uint8_t pixels[8 * 8 * 2]; nvn_deswizzle(&target.d, 0, 0, target.mem, pixels, 16);
    size_t wrong = 0; for (unsigned i = 0; i < 8 * 8; i++) wrong += pixels[2 * i] != 0 || pixels[2 * i + 1] != 255;
    CHECK(!wrong && after[0] == before[0] + 2 && history[0] == 2 && history[1] == 240,
        "invalidate before learned delay lands the newest rendering immediately, in the exact NVN layout");
    CHECK(vk_wb_test_invalidates() == invalidated + 1 && vk_submit_serial() == serial + 1,
        "default invalidate records one readback slot and counts the forced old-rendering landing separately");
    CHECK(!allocation[0] && !allocation[2] && !allocation[3], "invalidate landing releases its last image pin and record array");
    settled_idle(2); vk_res_mem(resources);
    CHECK(resources[0] && resources[7], "invalidate does not force cold image destruction or empty-block release");

    target.serial++; op(OP_SET_RT, &rt, sizeof rt);
    p_clear_color white = {0, 0xf, {1, 1, 0, 1}};
    op(OP_CLEAR_COLOR, &white, sizeof white); op(OP_SET_RT, &none, sizeof none);
    CHECK(mtl_wait(run(), 5000000000ull), "pre-staged consumer producer completes"); mtl_tex_release(&target);
    settled_idle(239);
    VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
    mtl_sub_begin();
    vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
    uint64_t copy = mtl_sub_commit();
    CHECK(!mtl_wait(copy, 0), "the consumer's existing staging copy is genuinely gated");
    vk_wb_test_stats(after); uint64_t copies = after[6]; serial = vk_submit_serial();
    lease_gate release = {.gate = event}; atomic_store(&release.release, 1);
    pthread_t releaser; if (pthread_create(&releaser, NULL, lease_gate_run, &release)) DIE("settled invalidate gate releaser");
    mtl_pool_invalidate(&pool, (uintptr_t)target.mem - (uintptr_t)pool.mem, target.L.size);
    pthread_join(releaser, NULL); vk_wb_test_stats(after); vk_wb_test_history(&target, history);
    CHECK(!release.timed_out && after[6] == copies && vk_submit_serial() == serial &&
        vk_wb_test_invalidates() == invalidated + 2 && history[0] == 3 && history[1] == 480 && target.mem[0] == 255,
        "invalidate consumes already-staged bytes by their exact fence, without another copy or submit");
    vkDestroyEvent(V.dev, event, NULL); settled_idle(2);

    target.serial++; op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &white, sizeof white); op(OP_SET_RT, &none, sizeof none);
    CHECK(mtl_wait(run(), 5000000000ull), "CPU-newer invalidate producer completes"); mtl_tex_release(&target);
    lag_guest_store(target.mem, 0x3c); memset(target.mem, 0x3c, target.L.size);
    settled_idle(61); serial = vk_submit_serial(); vk_wb_test_stats(after); copies = after[6];
    mtl_pool_invalidate(&pool, (uintptr_t)target.mem - (uintptr_t)pool.mem, target.L.size);
    vk_wb_test_history(&target, history); vk_wb_test_stats(after);
    size_t cpu_bad = 0; for (size_t i = 0; i < target.L.size; i++) cpu_bad += target.mem[i] != 0x3c;
    CHECK(!cpu_bad && after[6] == copies && vk_submit_serial() == serial &&
        vk_wb_test_invalidates() == invalidated + 2 && history[0] == 3 && history[1] == 480,
        "newer CPU bytes survive default invalidate with no readback, and do not reset learned intervals");

    target.serial++; op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &white, sizeof white); op(OP_SET_RT, &none, sizeof none);
    CHECK(mtl_wait(run(), 5000000000ull), "fresh rendering completes"); mtl_tex_release(&target);
    serial = vk_submit_serial();
    mtl_pool_invalidate(&pool, (uintptr_t)target.mem - (uintptr_t)pool.mem, target.L.size);
    CHECK(target.mem[0] == 255 && vk_submit_serial() == serial + 1 && vk_wb_test_invalidates() == invalidated + 3,
        "fresh default invalidate bypasses both base and learned quiet intervals");
    target.serial++; op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &white, sizeof white); op(OP_SET_RT, &none, sizeof none);
    CHECK(mtl_wait(run(), 5000000000ull), "old-layout replacement candidate completes"); mtl_tex_release(&target);
    settled_idle(61); /* still pending under its learned interval when a new layout replaces it */
    tex_t reused = target; reused.serial += 10000; reused.d.width = 32; reused.d.height = 16; reused.d.levels = 2; reused.d.format = 0x25;
    nvn_tex_layout(&reused.d, &reused.L);
    size_t extent = reused.L.size > target.L.size ? reused.L.size : target.L.size;
    uint8_t *guest = malloc(extent); memcpy(guest, target.mem, extent);
    vk_wb_test_stats(before); settled_render(&reused);
    CHECK(mtl_wait(run(), 5000000000ull), "different-layout reuse producer completes"); mtl_tex_release(&reused);
    serial = vk_submit_serial();
    mtl_pool_invalidate(&pool, (uintptr_t)reused.mem - (uintptr_t)pool.mem, reused.L.size);
    vk_wb_test_stats(after);
    CHECK(memcmp(guest, reused.mem, extent) && after[4] == before[4] + 1 && after[0] == before[0] + 1 &&
        vk_submit_serial() == serial + 1 && vk_wb_test_invalidates() == invalidated + 4,
        "different-layout GPU reuse supersedes the old rendering and invalidate lands only its fresh replacement");
    vk_wb_stats();
    reused.serial++; settled_render(&reused);
    CHECK(mtl_wait(run(), 5000000000ull), "dead-pool candidate has a new unlanded GPU rendering"); mtl_tex_release(&reused);
    memcpy(guest, reused.mem, extent);
    nvn_dirty_remove_range(memory); mtl_pool_del(memory);
    mtl_pool_invalidate(&pool, (uintptr_t)reused.mem - (uintptr_t)pool.mem, reused.L.size);
    CHECK(!memcmp(guest, reused.mem, extent) && vk_wb_test_invalidates() == invalidated + 4,
        "pool death before landing makes later invalidate write nothing");
    free(guest); settled_idle(2);
    vk_wb_test_memory(allocation); vk_res_mem(resources);
    CHECK(!allocation[0] && !allocation[1] && !allocation[2] && !allocation[3] && !allocation[4],
        "pool destruction releases all record/history metadata and image pins");
    CHECK(resources[0] && !resources[1] && !resources[7], "pool destruction releases images but retains empty blocks for reuse");
    munmap(memory, pool.size);
    printf("%s: settled learning %d checks, %d failed; 64 prompt tiles, %d intermittent refreshes/%d submissions, one first landing then zero periodic copies\n",
        fails ? "FAILED" : "ok", checks, fails, ROUNDS, ROUNDS * PERIOD);
    return fails != 0;
}

static int settled_cpu_writes(void) {
    enum { POOL_BYTES = 4 << 20, CYCLES = 4, FRAMES = 300 };
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_OFF"); unsetenv("TOTK_VK_SETTLED_WB_SUBS");
    uint8_t *memory = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) DIE("settled CPU-write mmap");
    pool_t pool = {.mem = memory, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, pool.size); nvn_dirty_add_range(memory, pool.size);
    tex_t target = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 260, .height = 260,
        .depth = 1, .levels = 9}, .mem = memory, .pool = &pool, .serial = 77000};
    nvn_tex_layout(&target.d, &target.L); memset(memory, 0x39, target.L.size);
    settled_render(&target); CHECK(mtl_wait(run(), 5000000000ull), "CPU-write fixture initial bake completes");
    mtl_tex_release(&target); settled_idle(62);
    settled_wait_landed(1);
    uint64_t history[2], initial[7]; vk_wb_test_history(&target, history); vk_wb_test_stats(initial);
    CHECK(history[0] == 1 && history[1] == 120, "CPU-write fixture starts with a learned interval");
    /* A CPU write between the GPU stamp and arm publication must win even if it faulted on an older baseline
       watch, before the new exact bit existed. The arm operation must not leave its tail protected. */
    uint8_t *early = memory + (2 << 20); uint64_t stale = nvn_dirty_now();
    nvn_dirty_protect(early, target.L.size); lag_guest_store(early, 0x31);
    uint64_t early_faults, early_preps; nvn_dirty_stats(&early_faults, &early_preps);
    void *obsolete = nvn_dirty_wb_watch(early, target.L.size, stale);
    CHECK(obsolete && nvn_dirty_wb_written(obsolete), "CPU generation before arm publication disarms the entire stale watch");
    lag_guest_store(early, 0x32); memset(early, 0x32, target.L.size);
    uint64_t early_after, early_host; nvn_dirty_stats(&early_after, &early_host);
    CHECK(early_after == early_faults && early_host == early_preps, "stale arm leaves no CPU-owned tail pages protected");
    nvn_dirty_wb_unwatch(obsolete);
    uint64_t own_stamp = nvn_dirty_now();
    void *provenance = nvn_dirty_wb_watch(early + 128, 256, own_stamp);
    void *own_token = nvn_dirty_gpu_write_begin(early + 128, 256);
    memset(early + 128, 0x57, 256); nvn_dirty_gpu_write_end(own_token);
    CHECK(provenance && !nvn_dirty_wb_written(provenance) && !nvn_dirty_wb_cpu_address(provenance) &&
        !nvn_dirty_since(early + 128, 256, own_stamp),
        "our landing token/store/restoration publishes no CPU write address or dirty generation");
    lag_guest_store(early + 129, 0x58);
    CHECK(nvn_dirty_wb_written(provenance) && nvn_dirty_wb_cpu_address(provenance) == (uintptr_t)early + 129,
        "a genuine direct CPU write records its exact byte inside the watched range");
    nvn_dirty_wb_unwatch(provenance);
    provenance = nvn_dirty_wb_watch(early + 128, 256, nvn_dirty_now());
    lag_guest_store(early + 400, 0x59);
    CHECK(nvn_dirty_wb_written(provenance) && nvn_dirty_wb_cpu_address(provenance) == (uintptr_t)early + 400,
        "same-page adjacent CPU takeover is identified outside the texture, not misreported as an in-range store");
    nvn_dirty_wb_unwatch(provenance);
    for (unsigned cycle = 0; cycle < CYCLES; cycle++) {
        target.serial++; settled_render(&target);
        CHECK(mtl_wait(run(), 5000000000ull), "CPU-write cycle %u GPU re-arm completes", cycle);
        mtl_tex_release(&target);
        uint64_t faults0, preps0, wb0, wbp0, waits0, host0, ns0;
        nvn_dirty_stats(&faults0, &preps0); nvn_dirty_wb_stats(&wb0, &wbp0);
        nvn_dirty_lag_stats(&waits0, &host0, &ns0);
        for (unsigned frame = 0; frame < FRAMES; frame++) {
            uint8_t value = (uint8_t)(0x40 + cycle + frame % 32);
            lag_guest_store(memory, value); memset(memory, value, target.L.size);
            settled_idle(1);
            uint64_t now[7], faults, preps, wb, wbp; vk_wb_test_stats(now);
            nvn_dirty_stats(&faults, &preps); nvn_dirty_wb_stats(&wb, &wbp);
            CHECK(faults == faults0 + 1 && wb == wb0 + 1 && preps == preps0 && wbp == wbp0,
                "cycle %u CPU frame %u has only its first storage-wide fault, not one per page/frame", cycle, frame);
            CHECK(!now[2] && !now[5] && now[0] == initial[0] && now[6] == initial[6],
                "cycle %u CPU frame %u retains no record/protection/staging and performs no copy", cycle, frame);
            int wrong = 0; for (size_t i = 0; i < target.L.size; i++) wrong |= memory[i] != value;
            CHECK(!wrong, "cycle %u CPU frame %u preserves every newer guest byte", cycle, frame);
        }
        uint64_t waits, host, ns; nvn_dirty_lag_stats(&waits, &host, &ns);
        vk_wb_test_history(&target, history);
        CHECK(waits == waits0 && host == host0 && history[0] == 1 && history[1] == 120,
            "CPU disarm never waits a WB ticket and preserves learned backoff until the next GPU write");
    }
    /* A WB producer is deliberately incomplete. Its image does not read the guest destination: a CPU overwrite
       must return before that source fence is released, not accidentally register it as a memory-reader guard. */
    target.serial++; VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
    lease_gate gate = {.gate = event}; pthread_t opener;
    if (pthread_create(&opener, NULL, lease_gate_run, &gate)) DIE("CPU-write gate thread");
    mtl_sub_begin();
    vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
    settled_render(&target); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
    uint64_t producer = mtl_sub_commit();
    CHECK(!mtl_wait(producer, 0), "WB source producer is genuinely incomplete during the CPU fault");
    uint64_t waits0, host0, ns0; nvn_dirty_lag_stats(&waits0, &host0, &ns0);
    lag_guest_store(memory, 0x79); memset(memory, 0x79, target.L.size);
    atomic_store_explicit(&gate.release, 1, memory_order_release); pthread_join(opener, NULL);
    uint64_t waits, host, ns; nvn_dirty_lag_stats(&waits, &host, &ns);
    CHECK(!gate.timed_out && waits == waits0 && host == host0,
        "CPU write completes without waiting for the gated WB producer ticket");
    CHECK(mtl_wait(producer, 5000000000ull), "gated WB producer completes after CPU ownership");
    vkDestroyEvent(V.dev, event, NULL); mtl_tex_release(&target); settled_idle(2);
    uint64_t end[7]; vk_wb_test_stats(end);
    CHECK(!end[2] && !end[5] && end[0] == initial[0] && end[6] == initial[6] && memory[0] == 0x79,
        "gated producer's obsolete rendering never copies or lands after the CPU overwrite");
    vk_wb_stats(); nvn_dirty_remove_range(memory); mtl_pool_del(memory); settled_idle(2);
    munmap(memory, pool.size);
    printf("%s: settled CPU writes %d checks, %d failed; %d cycles x %d frames, one fault per GPU re-arm, zero WB ticket waits\n",
        fails ? "FAILED" : "ok", checks, fails, CYCLES, FRAMES);
    return fails != 0;
}

static void settled_span_oracle(void) {
    const nvn_texdesc cases[] = {
        {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 260, .height = 260, .depth = 1, .levels = 9},
        {.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = 0x25, .width = 37, .height = 23, .depth = 3, .levels = 3},
        {.target = NVN_TEXTURE_TARGET_3D, .format = 0xd, .width = 17, .height = 19, .depth = 7, .levels = 3},
        {.target = NVN_TEXTURE_TARGET_2D, .format = 0x25, .flags = 0x40, .width = 37, .height = 23, .depth = 1, .levels = 1},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        const nvn_texdesc *d = &cases[c]; nvn_texlayout layout; nvn_tex_layout(d, &layout);
        uint8_t *reference = malloc(layout.size + 16), *full = malloc(layout.size + 16);
        uint8_t *split = malloc(layout.size + 16), *isolated = malloc(layout.size + 16);
        memset(reference, 0xa6, layout.size + 16); memset(full, 0xa6, layout.size + 16); memset(split, 0xa6, layout.size + 16);
        for (unsigned l = 0; l < d->levels; l++) for (unsigned layer = 0; layer < layout.layers; layer++) {
            uint32_t row, rows, slices, bpb; uint64_t base; unsigned th, td;
            int linear = nvn_tex_is_linear(d);
            if (linear) { row = d->width * layout.bpb; rows = d->height; slices = 1; base = 0; th = td = 0; }
            else if (nvn_deswizzle_geom(d, l, layer, &row, &rows, &slices, &base, &th, &td, &bpb)) DIE("span oracle geometry");
            size_t pitch = row + 16, size = pitch * rows * slices;
            uint8_t *source = malloc(size), *decoded = calloc(size, 1);
            for (size_t i = 0; i < size; i++) source[i] = (uint8_t)(i * 29 + i / 37 + l * 17 + layer * 43);
            /* Independent point-address oracle, not another traversal through the new span encoder. */
            for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < row; x++) {
                uint64_t offset = linear ? (uint64_t)y * nvn_tex_linear_stride(d) + x : nvn_bl_offset(x, y, z, row, rows, th, td);
                reference[base + offset] = source[((size_t)z * rows + y) * pitch + x];
            }
            CHECK(!nvn_swizzle(d, l, layer, source, pitch, full), "full encoder case %u level/layer %u/%u", c, l, layer);
            uint64_t level = nvn_tex_level_offset(d, l);
            uint64_t end = l + 1 < d->levels ? nvn_tex_level_offset(d, l + 1) : layout.layer_size;
            for (uint64_t offset = 0; offset < end - level; offset += 16384) {
                uint64_t n = end - level - offset < 16384 ? end - level - offset : 16384;
                CHECK(!nvn_swizzle_span(d, l, layer, source, pitch, split, offset, n), "bounded span case %u level/layer %u/%u", c, l, layer);
            }
            memset(isolated, 0xa6, layout.size + 16);
            uint64_t offset = end - level >= 1024 ? 512 : 0, n = end - level - offset < 512 ? end - level - offset : 512;
            CHECK(!nvn_swizzle_span(d, l, layer, source, pitch, isolated, offset, n), "isolated storage span encodes");
            int untouched = 1;
            for (uint64_t i = 0; i < layout.size + 16; i++)
                if ((i < base + offset || i >= base + offset + n) && isolated[i] != 0xa6) untouched = 0;
            CHECK(untouched, "case %u writes no byte outside its owned contiguous destination span", c);
            CHECK(!nvn_deswizzle(d, l, layer, full, decoded, pitch), "full decode case %u level/layer %u/%u", c, l, layer);
            int same = 1;
            for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < rows; y++)
                same &= !memcmp(source + ((size_t)z * rows + y) * pitch, decoded + ((size_t)z * rows + y) * pitch, row);
            CHECK(same, "case %u decoder retains every meaningful byte", c);
            free(source); free(decoded);
        }
        CHECK(!memcmp(full, reference, layout.size + 16) && !memcmp(split, reference, layout.size + 16),
            "case %u full and chunked encoders equal the point-address oracle, with padding/guards intact", c);
        free(reference); free(full); free(split); free(isolated);
    }
}

static int settled_budget(void) {
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_OFF"); setenv("TOTK_VK_SETTLED_WB_SUBS", "4", 1);
    settled_span_oracle();
    enum { POOL_BYTES = 32 << 20, VIEW_BYTES = 16 << 20 };
    uint8_t *memory = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) DIE("settled budget mmap");
    pool_t pool = {.mem = memory, .size = VIEW_BYTES, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, POOL_BYTES); mtl_pool_add(memory, pool.size); nvn_dirty_add_range(memory, POOL_BYTES);
    tex_t target = {.d = {.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = 0xd, .width = 1024, .height = 1024,
        .depth = 4, .levels = 4}, .mem = memory, .pool = &pool, .serial = 79500};
    nvn_tex_layout(&target.d, &target.L); CHECK(target.L.size > (1 << 20) && target.L.size <= pool.size, "budget target spans many 1-MiB sweeps");
    uint8_t *expected = malloc(target.L.size), *snapshot = malloc(target.L.size); settled_expected(&target, expected);
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        lag_guest_store(memory, 0x39); memset(memory, 0x39, target.L.size); target.serial++;
        settled_render(&target); CHECK(mtl_wait(run(), 5000000000ull), "budget scenario %u producer completes", scenario);
        mtl_tex_release(&target); uint64_t history[2]; vk_wb_test_history(&target, history); settled_idle((unsigned)history[1] - 1);
        VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
        mtl_sub_begin(); vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t fence = mtl_sub_commit(), before[7], after[7], sweep_before[6], sweep_after[6];
        CHECK(!mtl_wait(fence, 0), "budget scenario staging is genuinely gated");
        vk_wb_test_stats(before); VKCHECK(vkSetEvent(V.dev, event)); CHECK(mtl_wait(fence, 5000000000ull), "budget staging completes");
        vk_wb_test_sweep(sweep_before); vk_rec_lock(); vk_res_sweep(); vk_rec_unlock();
        vk_wb_test_stats(after); vk_wb_test_sweep(sweep_after);
        CHECK(after[0] == before[0] && after[2] == 1 && after[5] && sweep_after[5] == sweep_before[5] + 1,
            "one sweep carries a partial large rendering, without counting it as landed or releasing staging");
        CHECK(sweep_after[2] <= (1 << 20) && sweep_after[3] <= 72 && sweep_after[4] > sweep_before[4],
            "per-sweep byte cap holds and a real budget yield is recorded");
        CHECK(memory[0] == 255 && memcmp(memory, expected, target.L.size),
            "one sweep writes a real prefix but never the entire oversized rendering");
        if (!scenario) {
            uint64_t invalidates = vk_wb_test_invalidates();
            mtl_pool_invalidate(&pool, 0, target.L.size); vk_wb_test_stats(after);
            CHECK(!memcmp(memory, expected, target.L.size) && after[0] == before[0] + 1 &&
                !after[2] && !after[5] && vk_wb_test_invalidates() == invalidates + 1,
                "invalidate completes every remaining chunk/mip synchronously despite the sweep budget");
        } else if (scenario == 1) {
            lag_guest_store(memory, 0x6b); memset(memory, 0x6b, target.L.size); settled_idle(2); vk_wb_test_stats(after);
            int same = 1; for (size_t i = 0; i < target.L.size; i++) same &= memory[i] == 0x6b;
            CHECK(same && after[0] == before[0] && !after[2] && !after[5], "CPU overwrite cancels all remaining chunks without overwriting newer data");
        } else if (scenario == 2) {
            memcpy(snapshot, memory, target.L.size);
            tex_t reused = target; reused.serial += 10000; reused.d.depth = 2; reused.d.format = 0x25; nvn_tex_layout(&reused.d, &reused.L);
            settled_render(&reused); CHECK(mtl_wait(run(), 5000000000ull), "different-layout replacement completes");
            mtl_tex_release(&reused); vk_wb_test_stats(after);
            CHECK(!memcmp(snapshot, memory, target.L.size) && after[0] == before[0] && after[4] == before[4] + 1,
                "new GPU layout supersedes the partial rendering without landing its remaining bytes");
            vk_wb_range_gone((uintptr_t)memory, (uintptr_t)memory + pool.size);
        } else if (scenario == 3) {
            uint64_t invalidates = vk_wb_test_invalidates();
            settled_wait_landed(before[0] + 1); vk_wb_test_stats(after);
            CHECK(!memcmp(memory, expected, target.L.size) && !after[2] && !after[5] &&
                vk_wb_test_invalidates() == invalidates,
                "later ordinary sweeps resume every array layer and mip exactly, without any invalidate");
        } else {
            memcpy(snapshot, memory, target.L.size); mtl_pool_del(memory);
            CHECK(!mprotect(memory, pool.size, PROT_NONE), "partial destination is inaccessible after pool death");
            vk_rec_lock(); vk_res_sweep(); vk_rec_unlock();
            CHECK(!mprotect(memory, pool.size, PROT_READ | PROT_WRITE), "dead destination can be inspected again");
            CHECK(!memcmp(snapshot, memory, target.L.size), "pool death prevents all remaining partial stores");
        }
        vkDestroyEvent(V.dev, event, NULL);
    }
    vk_wb_stats(); nvn_dirty_remove_range(memory); mtl_pool_del(memory); settled_idle(2);
    uint64_t ownership[5], resources[8]; vk_wb_test_memory(ownership); vk_res_mem(resources);
    CHECK(!ownership[0] && !ownership[1] && !ownership[2] && !ownership[3] && resources[0] && !resources[7],
        "finalization releases records/history/pins/images while empty blocks remain reusable");
    free(expected); free(snapshot); munmap(memory, POOL_BYTES);
    printf("%s: settled budget %d checks, %d failed; 1-MiB/2-ms cooperative sweeps, partial CPU/GPU/pool cancellation, complete invalidate\n",
        fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

static int settled_preflight(const char *off) {
    enum { POOL_BYTES = 8 << 20, MISSES = 4096 };
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_SETTLED_WB_SUBS");
    if (off) setenv("TOTK_VK_OFF", off, 1); else unsetenv("TOTK_VK_OFF");
    int disabled = off && !strcmp(off, "settledinval"), no_drain = off && !strcmp(off, "settleddrain");
    int no_seal = off && !strcmp(off, "settledseal");
    uint8_t *memory = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) DIE("settled preflight mmap");
    pool_t pool = {.mem = memory, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, pool.size); nvn_dirty_add_range(memory, pool.size);
    tex_t target[2];
    for (unsigned i = 0; i < 2; i++) {
        target[i] = (tex_t){.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 260,
            .height = 260, .depth = 1, .levels = 9}, .pool = &pool, .mem = memory + (i ? 6 << 20 : 0x40000), .serial = 78000 + i};
        nvn_tex_layout(&target[i].d, &target[i].L); memset(target[i].mem, 0x39, target[i].L.size); settled_render(&target[i]);
    }
    CHECK(mtl_wait(run(), 5000000000ull), "preflight warm producers complete");
    for (unsigned i = 0; i < 2; i++) mtl_tex_release(&target[i]);
    settled_idle(62);
    settled_wait_landed(2);
    for (unsigned i = 0; i < 2; i++) {
        lag_guest_store(target[i].mem, 0x39); memset(target[i].mem, 0x39, target[i].L.size);
        target[i].serial += 10; settled_render(&target[i]);
    }
    CHECK(mtl_wait(run(), 5000000000ull), "preflight re-armed producers complete");
    uint64_t noted = vk_submit_serial() - 1;
    for (unsigned i = 0; i < 2; i++) mtl_tex_release(&target[i]);
    uint64_t before[7], after[7], drains = atomic_load(&test_drain_calls);
    vk_wb_test_sync(before);
    settled_idle(61);
    for (unsigned i = 0; i < MISSES; i++) mtl_pool_invalidate(&pool, 0x100000 + (i % 512) * 4096, 32);
    vk_wb_test_sync(after);
    CHECK(after[0] - before[0] == (disabled ? 0 : MISSES) && after[1] == before[1],
        "4096 inside-hull holes have no overlapping rendering");
    CHECK(after[2] == before[2] && after[3] == before[3] && after[4] == before[4] && after[5] == before[5] &&
        atomic_load(&test_drain_calls) == drains, "unrelated invalidates take zero drains, owner calls, seals or submissions");
    uint8_t *expected = malloc(target[0].L.size); settled_expected(&target[0], expected);
    mtl_pool_invalidate(&pool, target[0].mem - memory, target[0].L.size); vk_wb_test_sync(after);
    CHECK(after[2] - before[2] == (!disabled && !no_drain) && after[3] - before[3] == !disabled &&
        atomic_load(&test_drain_calls) - drains == (!disabled && !no_drain),
        "one actual eligible overlap takes exactly one drain/owner call (unless its individual cutout is set)");
    CHECK(disabled ? target[0].mem[0] == 0x39 : !memcmp(target[0].mem, expected, target[0].L.size),
        "eligible overlap lands every NVN level; settledinval alone disables that consumer path");
    if (!disabled) {
        uint64_t age = vk_submit_serial() - noted; CHECK(age < 120, "second target is still before its learned deadline");
        settled_idle((unsigned)(120 - age));
        uint64_t staged_before[7], staged_after[7]; vk_wb_test_stats(staged_before);
        VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
        mtl_sub_begin(); vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        uint64_t copy = mtl_sub_commit(); CHECK(!mtl_wait(copy, 0), "second target's staged copy is genuinely gated");
        vk_wb_test_stats(staged_after);
        CHECK(staged_after[6] == staged_before[6] + 1 && staged_after[5], "the gated slot contains a real staging copy, not just an empty wait");
        mtl_sub_begin(); (void)vk_cb(); /* an unrelated open stream: optional seal only */
        vk_wb_test_sync(before);
        lease_gate release = {.gate = event}; atomic_store(&release.release, 1); pthread_t opener;
        if (pthread_create(&opener, NULL, lease_gate_run, &release)) DIE("preflight staged-copy opener");
        mtl_pool_invalidate(&pool, target[1].mem - memory, target[1].L.size);
        pthread_join(opener, NULL); vk_wb_test_sync(after);
        CHECK(!release.timed_out && !memcmp(target[1].mem, expected, target[1].L.size),
            "already-staged consumer lands correctly with or without optional sealing");
        CHECK(after[4] - before[4] == !no_seal && after[5] - before[5] == !no_seal,
            "settledseal omits the optional submission but never omits a required copy submission");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "unrelated stream commits normally after invalidate");
        vkDestroyEvent(V.dev, event, NULL);
    }
    if (!disabled && !no_drain) {
        /* Real ordering race: preflight sees an OLD note, draining replays a NEW GPU write, and the old
           post-drain age check used to return without landing either version. The input is not a hull hole. */
        target[0].serial++; lag_guest_store(target[0].mem, 0x39); memset(target[0].mem, 0x39, target[0].L.size);
        settled_render(&target[0]); CHECK(mtl_wait(run(), 5000000000ull), "pre-drain old producer completes");
        settled_idle(61);
        uint64_t old_tile[6], new_tile[6], fresh_before[3], fresh_after[3];
        vk_wb_test_tile(&target[0], old_tile); vk_wb_test_fresh(fresh_before); vk_wb_test_sync(before);
        test_drain_arg = &target[0]; test_drain_hook = settled_drain_rewrite;
        mtl_pool_invalidate(&pool, target[0].mem - memory, target[0].L.size);
        vk_wb_test_tile(&target[0], new_tile); vk_wb_test_fresh(fresh_after); vk_wb_test_sync(after);
        uint8_t *zero = calloc((size_t)target[0].d.width * target[0].d.height, 2);
        CHECK(!nvn_swizzle(&target[0].d, 0, 0, zero, target[0].d.width * 2, expected), "new-producer reference keeps other mips/padding");
        CHECK(!test_drain_hook && new_tile[0] > old_tile[0] && new_tile[0] == new_tile[1] &&
            !memcmp(target[0].mem, expected, target[0].L.size),
            "invalidate lands the exact newer GPU version replayed during drain, including all mips and padding");
        CHECK(after[2] == before[2] + 1 && after[3] == before[3] + 1 && fresh_after[0] == fresh_before[0] + 1,
            "a candidate made fresh during drain still takes exactly one drain/owner call and one fresh landing");
        free(zero); mtl_tex_release(&target[0]);
    }
    if (!disabled) {
        /* The first consumer detaches two records and waits a genuinely incomplete GPU copy. An exact second
           consumer must still see those borrowed ranges and serialize; holes between them must remain cheap. */
        settled_expected(&target[0], expected);
        VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
        mtl_sub_begin();
        vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
        for (unsigned i = 0; i < 2; i++) {
            target[i].serial = 78020 + i; lag_guest_store(target[i].mem, 0x39); memset(target[i].mem, 0x39, target[i].L.size);
            settled_render(&target[i]);
        }
        op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        uint64_t producer = mtl_sub_commit();
        CHECK(!mtl_wait(producer, 0), "two-consumer producer is genuinely GPU-gated");
        lease_gate guard = {.gate = event}; pthread_t opener, first_thread, second_thread;
        if (pthread_create(&opener, NULL, lease_gate_run, &guard)) DIE("consumer deadline opener");
        settled_consumer first = {.pool = &pool, .size = pool.size};
        if (pthread_create(&first_thread, NULL, settled_consumer_run, &first)) DIE("first invalidate consumer");
        uint64_t active[7] = {0};
        for (unsigned spin = 0; spin < 5000; spin++) {
            vk_wb_test_stats(active);
            if ((!active[2] && active[5]) || atomic_load_explicit(&first.returned, memory_order_acquire)) break;
            usleep(1000);
        }
        CHECK(!active[2] && active[5] && !atomic_load_explicit(&first.returned, memory_order_acquire),
            "first invalidate has detached/staged the records but has not landed or returned");
        vk_wb_test_sync(before);
        for (unsigned i = 0; i < MISSES; i++) mtl_pool_invalidate(&pool, 0x100000 + (i % 512) * 4096, 32);
        vk_wb_test_sync(after);
        CHECK(after[2] == before[2] && after[3] == before[3] && after[4] == before[4] && after[5] == before[5] &&
            !atomic_load_explicit(&first.returned, memory_order_acquire),
            "4096 holes between borrowed landing ranges add zero drains/locks/seals/submits while the owner waits");
        uint64_t inflight = vk_wb_test_inflight();
        settled_consumer second = {.pool = &pool, .off = target[0].mem - memory, .size = target[0].L.size,
            .expected = expected, .compare = target[0].L.size};
        if (pthread_create(&second_thread, NULL, settled_consumer_run, &second)) DIE("second invalidate consumer");
        for (unsigned spin = 0; spin < 5000; spin++) {
            vk_wb_test_sync(after);
            if (atomic_load_explicit(&second.returned, memory_order_acquire) ||
                (vk_wb_test_inflight() == inflight + 1 && after[3] == before[3] + 1)) break;
            usleep(1000);
        }
        CHECK(vk_wb_test_inflight() == inflight + 1 && after[3] == before[3] + 1 &&
            !atomic_load_explicit(&second.returned, memory_order_acquire),
            "an overlapping second consumer sees the detached record and waits for its serialized landing");
        atomic_store_explicit(&guard.release, 1, memory_order_release);
        pthread_join(opener, NULL); pthread_join(first_thread, NULL); pthread_join(second_thread, NULL);
        CHECK(!guard.timed_out && !second.bad && !memcmp(target[1].mem, expected, target[1].L.size),
            "both consumers return only after complete guest bytes; the second reads every mip/padding immediately");
        CHECK(mtl_wait(producer, 0), "only the relevant producer/copy fences were needed");
        vkDestroyEvent(V.dev, event, NULL);
        for (unsigned i = 0; i < 2; i++) mtl_tex_release(&target[i]);
    }
    free(expected); vk_wb_test_sync(after); CHECK(after[6] > 0, "wall maxima observe actual copy/landing work");
    vk_wb_stats(); nvn_dirty_remove_range(memory); mtl_pool_del(memory); settled_idle(2); munmap(memory, pool.size);
    printf("%s: settled preflight %s %d checks, %d failed; %d hull misses, zero unrelated drains/owner calls\n",
        fails ? "FAILED" : "ok", off ? off : "default", checks, fails, MISSES);
    return fails != 0;
}

typedef struct {
    tex_t *producer, *unrelated; const uint8_t *stream;
    _Atomic int entered;
    uint64_t hold_ns, fence; int complete;
} settled_service_owner;
static void *settled_service_walk(void *arg) {
    settled_service_owner *o = arg;
    mtl_sub_begin();
    if (o->producer) {
        settled_render(o->producer); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
    }
    p_set_rt hold = {.n = 1}, none = {0}; hold.color[0] = o->unrelated;
    p_clear_color red = {0, 0xf, {1, 0, 0, 1}}, resumed = {0, 0xf, {.25f, .75f, 0, 1}};
    op(OP_SET_RT, &hold, sizeof hold); op(OP_CLEAR_COLOR, &red, sizeof red);
    op(OP_END, NULL, 0); mtl_exec(rec); rn = 0; /* keep a genuine unrelated rendering pass open */
    atomic_store_explicit(&o->entered, 1, memory_order_release);
    uint64_t end = host_now_ns() + o->hold_ns;
    do { mtl_exec(o->stream); usleep(500); } while (host_now_ns() < end);
    if (o->producer) {
        /* The final write is after the waiting consumer: it must not receive a partial producer snapshot. */
        p_set_rt rt = {.n = 1}, none = {0}; rt.color[0] = o->producer; rt.has_cview[0] = 1;
        rt.cview[0] = (view_t){.set_levels = 1, .nlevels = 1};
        p_clear_color zero = {0, 0xf, {0, 0, 0, 1}};
        op(OP_SET_RT, &rt, sizeof rt); op(OP_CLEAR_COLOR, &zero, sizeof zero); op(OP_SET_RT, &none, sizeof none);
        op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
    }
    op(OP_SET_RT, &hold, sizeof hold); op(OP_CLEAR_COLOR, &resumed, sizeof resumed); op(OP_SET_RT, &none, sizeof none);
    op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
    o->fence = mtl_sub_commit(); o->complete = mtl_wait(o->fence, 5000000000ull);
    return NULL;
}
typedef struct {
    const uint8_t *stream, *end; uint8_t *jump;
    _Atomic int entered; int complete;
} settled_service_spin;
static void *settled_service_nondraw(void *arg) {
    settled_service_spin *s = arg;
    mtl_sub_begin(); atomic_store_explicit(&s->entered, 1, memory_order_release);
    mtl_exec(s->stream); /* no return, draw, call, submit or driver work until the second service */
    s->complete = mtl_wait(mtl_sub_commit(), 5000000000ull);
    return NULL;
}
static int settled_service_spin_request(void *arg, int boundary) {
    (void)boundary;
    settled_service_spin *s = arg;
    if (s) memcpy(s->jump, &s->end, sizeof s->end); /* only the existing owner edits its live stream */
    return 1;
}
static int settled_service(void) {
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_OFF"); unsetenv("TOTK_VK_SETTLED_WB_SUBS");
    setenv("TOTK_VK_WB_SERVICE_STATS", "1", 1);
    enum { BYTES = 4 << 20 };
    uint8_t *memory = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) DIE("service fixture mapping");
    pool_t pool = {.mem = memory, .size = BYTES, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, BYTES); nvn_dirty_add_range(memory, BYTES);
    pool_t gpu = pool; gpu.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    uint8_t stream[16]; op(OP_BEGIN, NULL, 0); op(OP_END, NULL, 0); memcpy(stream, rec, sizeof stream); rn = 0;
    uint64_t latencies[2] = {0};
    for (unsigned mode = 0; mode < 3; mode++) {
        tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 260, .height = 260,
            .depth = 1, .levels = 9}, .mem = memory + mode * 0x100000, .pool = &pool, .serial = 81000 + mode};
        nvn_tex_layout(&t.d, &t.L); memset(t.mem, 0x39, t.L.size);
        uint8_t *expected = malloc(t.L.size); settled_expected(&t, expected);
        if (mode < 2) {
            settled_render(&t); CHECK(mtl_wait(run(), 5000000000ull), "service source producer completes");
            if (mode == 1) {
                settled_idle(59);
                VkEvent gate; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
                VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &gate));
                mtl_sub_begin();
                vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
                uint64_t copy = mtl_sub_commit(); uint64_t state[7]; vk_wb_test_stats(state);
                CHECK(state[2] == 1 && state[5] && !mtl_wait(copy, 0), "service test has a real gated staging copy");
                VKCHECK(vkSetEvent(V.dev, gate)); CHECK(mtl_wait(copy, 5000000000ull), "staging completes without an idle landing");
                vkDestroyEvent(V.dev, gate, NULL);
            }
        } else {
            uint8_t *zero = calloc((size_t)t.d.width * t.d.height, 2);
            CHECK(!nvn_swizzle(&t.d, 0, 0, zero, t.d.width * 2, expected), "unfinished-producer reference includes its final clear");
            free(zero);
        }
        tex_t other = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 8, .height = 8,
            .depth = 1, .levels = 1}, .mem = memory + 3 * 0x100000, .pool = &gpu, .serial = 82000 + mode};
        nvn_tex_layout(&other.d, &other.L);
        settled_service_owner owner = {.producer = mode == 2 ? &t : NULL, .unrelated = &other, .stream = stream,
            .hold_ns = mode == 2 ? 150000000 : 400000000};
        pthread_t thread; uint64_t before[8], after[8]; vk_rec_service_test_stats(before);
        if (pthread_create(&thread, NULL, settled_service_walk, &owner)) DIE("long-held recording owner");
        while (!atomic_load_explicit(&owner.entered, memory_order_acquire)) usleep(100);
        uint64_t start = host_now_ns();
        mtl_pool_invalidate(&pool, t.mem - memory, t.L.size);
        uint64_t latency = host_now_ns() - start;
        if (mode < 2) {
            latencies[mode] = latency;
            CHECK(latency < 50000000, "%s invalidate completes under50ms during a400ms unrelated rec-lock hold (%.3fms)",
                mode ? "already staged" : "fresh", latency / 1e6);
        } else CHECK(latency >= 100000000, "unfinished same-image producer is not weakened into a partial snapshot (%.3fms)", latency / 1e6);
        CHECK(!memcmp(t.mem, expected, t.L.size), "service lands exact nine-level NVN bytes and padding before return");
        pthread_join(thread, NULL); vk_rec_service_test_stats(after);
        CHECK(owner.complete && after[2] > before[2], "existing owner services the consumer, not a blocking lock acquisition");
        if (mode == 2) CHECK(after[3] == before[3] + 1, "same-producer request defers once until slot seal");
        uint8_t pixel[2] = {0}; NVNcopyRegion region = {0, 0, 0, 1, 1, 1};
        mtl_read_texels(&other, 0, 0, &region, pixel, 2, 2);
        CHECK(pixel[0] == 64 && pixel[1] == 191, "unrelated pass resumes correctly after owner-side readback (%u,%u)", pixel[0], pixel[1]);
        mtl_tex_release(&other); mtl_tex_release(&t); free(expected);
    }
    /* The first call may coincide with stream entry. The second cannot: the
       owner is trapped in this single draw-free interpreter invocation until
       that callback redirects its jump. Bounded chunk polling must serve it. */
    for (unsigned i = 0; i < 65; i++) op(OP_BEGIN, NULL, 0);
    const uint8_t *loop = rec; size_t jump = rn + 8;
    op(OP_JUMP, &loop, sizeof loop); size_t end = rn; op(OP_END, NULL, 0);
    settled_service_spin spin = {.stream = rec, .end = rec + end, .jump = rec + jump};
    pthread_t spinner; uint64_t spin_before[8], spin_after[8]; vk_rec_service_test_stats(spin_before);
    if (pthread_create(&spinner, NULL, settled_service_nondraw, &spin)) DIE("draw-free owner");
    while (!atomic_load_explicit(&spin.entered, memory_order_acquire)) usleep(100);
    vk_rec_service_call(settled_service_spin_request, NULL);
    uint64_t spin_delay = vk_rec_service_call(settled_service_spin_request, &spin);
    pthread_join(spinner, NULL); rn = 0; vk_rec_service_test_stats(spin_after);
    CHECK(spin.complete && spin_after[2] >= spin_before[2] + 2,
        "a single unbounded draw-free jump stream services both requests cooperatively");
    CHECK(spin_delay < 50000000, "draw-free stream cannot starve a pending request (%.3fms)", spin_delay / 1e6);
    uint64_t stats[8]; vk_rec_service_test_stats(stats);
    CHECK(stats[5] >= 400000000, "optional telemetry observes the real long recording-lock hold");
    nvn_dirty_remove_range(memory); mtl_pool_del(memory); settled_idle(2); munmap(memory, BYTES);
    printf("%s: owner service %d checks, %d failed; fresh/staged %.3f/%.3fms under400ms owner hold, unfinished producer preserved; delay p50<=%.3f p99<=%.3f max%.3fms holdmax%.3fms\n",
        fails ? "FAILED" : "ok", checks, fails, latencies[0] / 1e6, latencies[1] / 1e6,
        stats[6] / 1e6, stats[7] / 1e6, stats[4] / 1e6, stats[5] / 1e6);
    return fails != 0;
}

static int settled_cutout(const char *off) {
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_SETTLED_WB_SUBS"); setenv("TOTK_VK_OFF", off, 1);
    enum { POOL_BYTES = 4 << 20 };
    uint8_t *memory = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) DIE("settled cutout mmap");
    pool_t pool = {.mem = memory, .size = POOL_BYTES, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, pool.size); nvn_dirty_add_range(memory, pool.size);
    tex_t target = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd, .width = 260, .height = 260,
        .depth = 1, .levels = 9}, .mem = memory, .pool = &pool, .serial = 79000};
    nvn_tex_layout(&target.d, &target.L); memset(memory, 0x39, target.L.size); settled_render(&target);
    CHECK(mtl_wait(run(), 5000000000ull), "cutout first producer completes"); mtl_tex_release(&target); settled_idle(62);
    settled_wait_landed(1);
    uint64_t history[2], resources[8], before[7], after[7]; vk_wb_test_history(&target, history); vk_res_mem(resources); vk_wb_test_stats(before);
    CHECK(before[0] == 1 && !before[2] && !before[5], "cutout preserves prompt first landing");
    if (!strcmp(off, "settledbackoff")) {
        CHECK(!history[0] && history[1] == 60, "settledbackoff retains no learned history");
        target.serial++; settled_render(&target); CHECK(mtl_wait(run(), 5000000000ull), "fixed-K producer completes");
        mtl_tex_release(&target); settled_idle(62); vk_wb_test_stats(after);
        settled_wait_landed(2); vk_wb_test_stats(after);
        CHECK(after[0] == 2, "settledbackoff restores first-use K on every rendering");
    } else if (!strcmp(off, "settledcpu")) {
        target.serial++; settled_render(&target); CHECK(mtl_wait(run(), 5000000000ull), "legacy CPU-watch producer completes");
        mtl_tex_release(&target); uint64_t faults0, prep0, wb0, wbp0, faults, prep, wb, wbp;
        nvn_dirty_stats(&faults0, &prep0); nvn_dirty_wb_stats(&wb0, &wbp0);
        lag_guest_store(memory, 0x71); memset(memory, 0x71, target.L.size); settled_idle(1);
        nvn_dirty_stats(&faults, &prep); nvn_dirty_wb_stats(&wb, &wbp); vk_wb_test_stats(after);
        CHECK(after[2] == 1 && wb == wb0 && faults > faults0 + 1,
            "settledcpu restores per-page watches and delayed CPU cancellation for diagnosis");
    } else CHECK(0, "unknown settled cutout %s", off);
    vk_wb_stats(); nvn_dirty_remove_range(memory); mtl_pool_del(memory); settled_idle(2); munmap(memory, pool.size);
    printf("%s: settled cutout %s %d checks, %d failed\n", fails ? "FAILED" : "ok", off, checks, fails);
    return fails != 0;
}

typedef struct {
    uint8_t *texture, *prefix, *suffix; uint8_t value, before, after;
    _Atomic int done;
} settled_race_writer;
static void *settled_race_write(void *arg) {
    settled_race_writer *w = arg;
    for (unsigned i = 0; i < 256; i++) {
        /* As in the existing lag fixture, let the first real machine store take any fault before libc/TSan's
           memset interceptor describes the remaining writes. The full ranges still use instrumented memset;
           there are no sanitizer-suppression annotations or disabled backend threads. */
        lag_guest_store(w->texture, w->value); memset(w->texture, w->value, 64);
        lag_guest_store(w->prefix, w->before); memset(w->prefix, w->before, 64);
        lag_guest_store(w->suffix, w->after); memset(w->suffix, w->after, 64);
    }
    atomic_store_explicit(&w->done, 1, memory_order_release); return NULL;
}
static void *settled_race_land(void *arg) {
    vk_rec_lock(); vk_res_sweep(); vk_rec_unlock();
    atomic_store_explicit((_Atomic int *)arg, 1, memory_order_release); return NULL;
}
static void settled_race_wait(_Atomic int *flag, const char *what) {
    uint64_t deadline = host_now_ns() + 5000000000ull;
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        if (host_now_ns() >= deadline) DIE("settled race timed out: %s", what);
        sched_yield();
    }
}
static int settled_race(void) {
    enum { POOL_BYTES = 1 << 20, ITERATIONS = 128 };
    unsetenv("TOTK_VK_WRITEBACK"); unsetenv("TOTK_VK_OFF"); setenv("TOTK_VK_SETTLED_WB_SUBS", "2", 1);
    uint8_t *memory = mmap(NULL, 2 * POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("settled race mmap"); return 1; }
    pool_t pool = {.mem = memory, .size = POOL_BYTES,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_init(); mtl_pool_add(memory, 2 * pool.size); mtl_pool_add(memory, pool.size); nvn_dirty_add_range(memory, 2 * pool.size);
    tex_t t = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = 0xd,
        .width = 8, .height = 8, .depth = 1, .levels = 1}, .mem = memory + 128, .pool = &pool, .serial = 74000};
    nvn_tex_layout(&t.d, &t.L);
    uintptr_t page = (uintptr_t)getpagesize();
    CHECK(((uintptr_t)(t.mem - 64) / page == (uintptr_t)t.mem / page) &&
        ((uintptr_t)(t.mem + t.L.size + 63) / page == (uintptr_t)(t.mem + t.L.size - 1) / page),
        "both adjacent writer ranges share the rendering's protected pages");
    static const char *names[] = {"", "before-ownership", "faulted-while-protected",
        "residual-before-freshness", "residual-after-freshness", "after-restoration"};
    for (int phase = NVN_DIRTY_TEST_WB_BEFORE_OWNERSHIP; phase <= NVN_DIRTY_TEST_WB_RESTORED; phase++) {
        int phase_fails = fails; unsigned residuals = 0;
        for (unsigned iteration = 0; iteration < ITERATIONS; iteration++) {
            t.serial++; memset(memory, 0x42, t.L.size + 256); settled_render(&t);
            CHECK(mtl_wait(run(), 5000000000ull), "race phase %d trial %u producer completes", phase, iteration);
            settled_idle(1);
            uint64_t before[7], after[7]; vk_wb_test_stats(before);
            VkEvent event; VkEventCreateInfo ci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            VKCHECK(vkCreateEvent(V.dev, &ci, NULL, &event));
            mtl_sub_begin();
            vkCmdWaitEvents(vk_cb(), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
            uint64_t fence = mtl_sub_commit();
            nvn_dirty_test_wb_handoff handoff = {.addr = t.mem, .size = t.L.size, .phase = phase};
            nvn_dirty_test_wb_set(&handoff);
            VKCHECK(vkSetEvent(V.dev, event)); CHECK(mtl_wait(fence, 5000000000ull), "race staging completes");
            _Atomic int landed = 0; pthread_t land_thread, write_thread;
            if (pthread_create(&land_thread, NULL, settled_race_land, &landed)) DIE("settled race land thread");
            settled_race_wait(&handoff.arrived, "landing handoff");
            uint64_t generation = nvn_dirty_now();
            settled_race_writer writer = {.texture = t.mem, .prefix = t.mem - 64, .suffix = t.mem + t.L.size,
                .value = (uint8_t)(0x60 + iteration % 31), .before = (uint8_t)(0xa0 + iteration % 31),
                .after = (uint8_t)(0x20 + iteration % 31)};
            if (pthread_create(&write_thread, NULL, settled_race_write, &writer)) DIE("settled race writer thread");
            if (phase == NVN_DIRTY_TEST_WB_PROTECTED) {
                settled_race_wait(&handoff.faults, "writer actually faults on the still-read-only BUSY page");
                CHECK(!atomic_load_explicit(&writer.done, memory_order_acquire), "already-faulted writer cannot pass BUSY ownership");
            } else {
                settled_race_wait(&writer.done, "writer stores finish at the selected landing boundary");
                int residual = phase == NVN_DIRTY_TEST_WB_BEFORE_FRESHNESS || phase == NVN_DIRTY_TEST_WB_AFTER_FRESHNESS;
                CHECK(residual ? (!atomic_load(&handoff.faults) && nvn_dirty_now() == generation) :
                    atomic_load(&handoff.faults) != 0, "phase %d has precisely the protected versus temporary-RW fault behavior", phase);
            }
            atomic_store_explicit(&handoff.release, 1, memory_order_release);
            settled_race_wait(&landed, "landing returns"); settled_race_wait(&writer.done, "writer finishes after restoration");
            pthread_join(land_thread, NULL); pthread_join(write_thread, NULL); nvn_dirty_test_wb_set(NULL);
            vk_wb_test_stats(after);
            size_t adjacent_bad = 0, texture_bad = 0;
            for (unsigned i = 0; i < 64; i++) {
                adjacent_bad += writer.prefix[i] != writer.before || writer.suffix[i] != writer.after;
                texture_bad += t.mem[i] != writer.value;
            }
            for (unsigned i = 0; i < 64; i++) adjacent_bad += memory[i] != 0x42;
            CHECK(!adjacent_bad, "landing never touches either adjacent same-page range or its outer guard");
            int residual = phase == NVN_DIRTY_TEST_WB_BEFORE_FRESHNESS || phase == NVN_DIRTY_TEST_WB_AFTER_FRESHNESS;
            if (residual) {
                /* Stores are deliberately ordered before resuming swizzle: this demonstrates the coherency hole
                   itself, without an undefined C data race or a TSan suppression masking it. */
                CHECK(t.mem[0] == 255 && after[0] == before[0] + 1 && after[3] == before[3],
                    "fresh direct stores inside temporary RW are overwritten with no published CPU generation");
                residuals += t.mem[0] == 255;
            } else {
                CHECK(!texture_bad, "CPU bytes survive the protocol's guaranteed interleaving");
                CHECK(phase == NVN_DIRTY_TEST_WB_BEFORE_OWNERSHIP ?
                    after[0] == before[0] && after[3] == before[3] + 1 :
                    phase == NVN_DIRTY_TEST_WB_PROTECTED ?
                    after[0] - before[0] + after[3] - before[3] == 1 :
                    after[0] == before[0] + 1 && after[3] == before[3],
                    "CPU-before-ownership rejects; fault under RO may cancel before freshness or store last; restored writer stores last");
            }
            CHECK(!after[2] && !after[5], "race trial releases all records and staging");
            vkDestroyEvent(V.dev, event, NULL); mtl_tex_release(&t);
            /* Each forced race is a first-use rendering in a fresh VIEW registration; a prior trial's successful
               landing must not back off the next trial before it can reach the selected handoff. */
            mtl_pool_del(memory); mtl_pool_add(memory, pool.size);
        }
        printf("settled race %s: %u iterations x 256 writer rounds, residual overwrites %u, adjacent corruptions %s, %s\n",
            names[phase], ITERATIONS, residuals, fails == phase_fails ? "0" : "see failures", fails == phase_fails ? "ok" : "FAILED");
    }
    nvn_dirty_remove_range(memory); mtl_pool_del(memory); mtl_pool_del(memory); munmap(memory, 2 * pool.size);
    printf("%s: settled concurrent writer %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

static int settled_suite(void) {
    static const struct { const char *name, *subs; int off; } modes[] = {
        {"default", NULL, 0}, {"tuned", "4", 0}, {"off", "4", 1}};
    fflush(NULL);
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) {
        pid_t child = fork(); if (child < 0) { perror("settled fork"); return 1; }
        if (!child) {
            unsetenv("TOTK_VK_WRITEBACK");
            if (modes[i].subs) setenv("TOTK_VK_SETTLED_WB_SUBS", modes[i].subs, 1); else unsetenv("TOTK_VK_SETTLED_WB_SUBS");
            if (modes[i].off) setenv("TOTK_VK_OFF", "settledwb", 1); else unsetenv("TOTK_VK_OFF");
            execl("/proc/self/exe", "vkdrawtest", "--settled-wb-child", modes[i].off ? "off" : "on", (char *)NULL);
            _exit(127);
        }
        int status; pid_t waited; do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        CHECK(waited == child && WIFEXITED(status) && !WEXITSTATUS(status), "settled %s fresh-process fixture passes", modes[i].name);
    }
    printf("%s: settled policy %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

/* The terminal packet and native UI render must agree on the presented extent,
   not the smaller guest allocation. This opens a real native attachment and
   asks the production packet-target selector while that pass is live. */
static int nativeui_geometry(void) {
#ifndef TOTK_VK_TESTS
    fprintf(stderr, "native UI geometry fixture requires TOTK_VK_TESTS\n");
    return 2;
#else
    setenv("TOTK_NATIVE_UI", "1", 1);
    setenv("TOTK_POST_AA", "off", 1);
    mtl_init();
    CHECK(vk_native_ui_enabled(), "native UI geometry fixture enables production native rendering");
    vk_native_window_size(48, 27); /* 3:2 presentation extent for 32x18 guest */
    size_t size = 1u << 20;
    uint8_t *memory = aligned_alloc(4096, size);
    CHECK(memory != NULL, "allocate synthetic guest target");
    if (!memory) return 1;
    pool_t pool = {.mem=memory,.size=size,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(memory, size);
    tex_t target = {0};
    target.d.target=1; target.d.format=0x25; target.d.width=32; target.d.height=18;
    target.d.depth=target.d.levels=1; target.pool=&pool; target.mem=memory; target.serial=90901;
    nvn_tex_layout(&target.d,&target.L);
    mtl_sub_begin();
    vk_tex *image=vk_tex_get(&target);
    CHECK(image!=NULL, "create production guest target");
    if (!image) { mtl_sub_commit(); free(memory); return 1; }
    vk_tex_native_promote(image);
    vk_tex_native_ui_begin(image);
    p_set_rt set={.n=1,.color={&target}};
    vk_draw_exec(OP_SET_RT,&set);
    p_viewports viewport={.n=1,.v={{0,0,32,18,0,1}}};
    vk_draw_exec(OP_VIEWPORTS,&viewport);
    p_scissor scissor={0,0,31,18}; /* partial clear opens a pass instead of deferring a load-clear */
    vk_draw_exec(OP_SCISSOR,&scissor);
    p_clear_color clear={0,15,{0.25f,0.5f,0.75f,1}};
    vk_draw_exec(OP_CLEAR_COLOR,&clear); /* establishes the real native render pass */
    vk_post_output target_out={0};
    CHECK(vk_fg_test_replay_target(&target_out),
        "production frozen-packet selector observes the active terminal draw target");
    CHECK(target_out.image!=VK_NULL_HANDLE && target_out.w==48 && target_out.h==27 &&
          target_out.format==VK_FORMAT_R8G8B8A8_UNORM && target_out.view==VK_NULL_HANDLE,
        "frozen terminal target is the actual 48x27 native attachment, not its 32x18 guest texture");
    vk_end_rendering();
    uint64_t ticket=mtl_sub_commit();
    CHECK(mtl_wait(ticket,5000000000ull),"native geometry fixture pass completes");
    vk_gpu_idle();
    mtl_tex_release(&target);
    free(memory);
    return fails ? 1 : 0;
#endif
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--nativeui-geometry")) return nativeui_geometry();
    if (argc == 2 && !strcmp(argv[1], "--settled-wb-service")) return settled_service();
    if (argc == 2 && !strcmp(argv[1], "--settled-wb-budget")) return settled_budget();
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--settled-wb-preflight")) return settled_preflight(argc == 3 ? argv[2] : NULL);
    if (argc == 3 && !strcmp(argv[1], "--settled-wb-cutout")) return settled_cutout(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--settled-wb-cpu")) return settled_cpu_writes();
    if (argc == 2 && !strcmp(argv[1], "--settled-wb-learning")) return settled_learning();
    if (argc == 2 && !strcmp(argv[1], "--settled-wb-race")) return settled_race();
    if (argc == 2 && !strcmp(argv[1], "--settled-wb")) return settled_suite();
    if (argc == 3 && !strcmp(argv[1], "--settled-wb-child")) return settled_child(!strcmp(argv[2], "off"));
    if (argc == 1 && settled_suite()) return 1;
    /* TOTK_NVN_CALL_NOCOPY: production command probes, a call/release/EndRecording race (TSan) and the
       CallCommands + EndRecording microbenchmark in both arms; no Vulkan needed. */
    if (argc >= 2 && !strcmp(argv[1], "--nvn-call")) {
        extern int test_nvn_call_nocopy(int nocopy, int rounds);
        extern double test_nvn_call_bench(int nocopy, int frames, size_t *bytes_out);
        int bad = 0; for (int arm = 0; arm < 2; arm++) bad += test_nvn_call_nocopy(arm, 2000);
        int frames = argc >= 3 ? atoi(argv[2]) : 2000; size_t bytes[2] = {0};
        for (int rep = 0; rep < 3 && frames > 0; rep++) for (int arm = 0; arm < 2; arm++) {
            int a = rep % 2 ? 1 - arm : arm;
            double ns = test_nvn_call_bench(a, frames, &bytes[a]);
            printf("bench nvn-call-nocopy%d ns/draw=%.1f (ns per frame: 64 called lists into one caller, %zu bytes)\n", a, ns, bytes[a]);
        }
        if (bytes[0] != bytes[1]) bad++;
        printf("%s: nvn call nocopy %d failed\n", bad ? "FAILED" : "ok", bad); return bad != 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--lag-walk-bench")) return lag_walk_bench(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--texcopy")) return texcopy_suite();
    if (argc == 2 && !strcmp(argv[1], "--texcopy-child")) return texcopy_child();
    int submit_mode = argc >= 2 && !strcmp(argv[1], "--rec-submit");
    int pressure_mode = argc == 2 && !strcmp(argv[1], "--rec-vma-fallback");
    int storage_mode = argc == 2 && !strcmp(argv[1], "--rec-storage");
    int unlimited_leases = argc == 2 && !strcmp(argv[1], "--rec-leases-unlimited");
    int storage_warm_mode = argc == 2 && !strcmp(argv[1], "--rec-storage-warm");
    int lease_mode = (argc == 2 && !strcmp(argv[1], "--rec-leases")) || unlimited_leases || storage_warm_mode;
    int overlay_scene_mode = argc == 2 && !strcmp(argv[1], "--overlay-scene");
    if (overlay_scene_mode) {
        setenv("TOTK_VK_COMPILE_THREADS", "0", 1); unsetenv("TOTK_PIPE_WORKERS");
        if (!getenv("TOTK_OVERLAY")) setenv("TOTK_OVERLAY", "fps", 1);
    }
    if (lease_mode) setenv("TOTK_VK_CHUNK_DRAWS", unlimited_leases ? "0" : "600", unlimited_leases);
    if (storage_mode) { setenv("TOTK_VK_REC_MEMORY_MB", "32", 1); setenv("TOTK_VK_CHUNK_DRAWS", "0", 1); }
    if (storage_warm_mode) setenv("TOTK_VK_REC_MEMORY_MB", "16", 1);
    if (storage_mode || storage_warm_mode) setenv("TOTK_VK_REC_VMA_NAMES", "1", 1); /* these diagnostics inspect named slabs */
    if (submit_mode) setenv("TOTK_VK_CHUNK_DRAWS", "1", 1); /* force a real GPU slot between the gated alias passes */
    const char *rename_env = getenv("TOTK_VK_UB_RENAME");
    int renaming = rename_env && atoi(rename_env) && !vk_off("rename");
    int snapshots = (renaming && !vk_off("snap")) || (vk_lag_capture_on() && !renaming);
    const size_t G = 32u << 20;
    uint8_t *g = mmap(NULL, G, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *cpu_pool = g, *gpu_pool = g + (16u << 20);
    setenv("TOTK_VK_WRITEBACK", "1", 0);   /* GPU write-back into CPU pools is opt-in; the write-back checks below need it */
#ifdef TOTK_ANDROID_APP
    setenv("TOTK_VK_PASSPROBE", "2", 1);
#endif
    mtl_init();
#ifdef TOTK_VK_TESTS
    call_counts_install();
    if (getenv("TOTK_VK_TEST_WIDEN_U8")) V.has_index_u8 = 0;
#endif
    mtl_pool_add(cpu_pool, 16u << 20);
    mtl_pool_add(gpu_pool, 16u << 20);
    if (runtime_frame_lag_requested()) nvn_dirty_add_range(cpu_pool, 16u << 20);
    static pool_t P_cpu = {.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED}, P_gpu = {.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    P_cpu.mem = cpu_pool; P_cpu.size = 16u << 20; P_gpu.mem = gpu_pool; P_gpu.size = 16u << 20;

    /* the sampled texture: 64x64 RGBA8, block-linear in the CPU pool, texel (x, y) = (4x, 4y, 128, 255) */
    tex_t tx; memset(&tx, 0, sizeof tx);
    tx.d.target = 1; tx.d.format = 0x25; tx.d.width = 64; tx.d.height = 64; tx.d.depth = 1; tx.d.levels = 1;
    nvn_tex_layout(&tx.d, &tx.L); tx.mem = cpu_pool + 0x10000; tx.pool = &P_cpu; tx.serial = 101;
    uint8_t lin[64 * 64 * 4];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint8_t *p = lin + (y * 64 + x) * 4; p[0] = (uint8_t)(4 * x); p[1] = (uint8_t)(4 * y); p[2] = 128; p[3] = 255; }
    CHECK(!nvn_swizzle(&tx.d, 0, 0, lin, 256, tx.mem), "swizzle the test texture");
    /* the render target: 64x64 RGBA8 in the GPU-only pool */
    tex_t rt; memset(&rt, 0, sizeof rt);
    rt.d = tx.d; nvn_tex_layout(&rt.d, &rt.L); rt.mem = gpu_pool; rt.pool = &P_gpu; rt.serial = 102;
    mtl_reg_tex(1, &tx, NULL);
    samp_par sp; memset(&sp, 0, sizeof sp); sp.lod_max = 1000; mtl_reg_samp(1, &sp);

    /* program */
    static uint8_t vctl[0x880], fctl[0x880], vcode[0x230], fcode[0x230]; uint32_t vlen, flen;
    shader_binary(0, 1, vctl, vcode, &vlen); shader_binary(1, 2, fctl, fcode, &flen);
    uint8_t *vc = cpu_pool + 0x100000, *fc = cpu_pool + 0x101000; memcpy(vc, vcode, vlen); memcpy(fc, fcode, flen);
    prog_t pg; memset(&pg, 0, sizeof pg); pg.nstage = 2; pg.serial = 7;
    pg.sd[0].data = (NVNbufferAddress)(uintptr_t)vc; pg.sd[0].control = vctl; pg.sd[1].data = (NVNbufferAddress)(uintptr_t)fc; pg.sd[1].control = fctl;

    /* uniforms and vertices */
    float *vub = (float *)(cpu_pool + 0x200000), *fub = (float *)(cpu_pool + 0x200100), *vtx = (float *)(cpu_pool + 0x201000);
    vub[0] = vub[1] = vub[2] = vub[3] = 0;
    fub[0] = 1; fub[1] = 0.5f; fub[2] = 1; fub[3] = 1;
    /* a triangle covering the upper half (clip y >= 0 is the top, as on Metal and NVN) */
    float tri[6] = {-1, 0, 3, 0, -1, 4}; memcpy(vtx, tri, sizeof tri);
    uint8_t *readback = cpu_pool + 0x300000; memset(readback, 0xcd, 64 * 64 * 4);

    p_set_rt srt; memset(&srt, 0, sizeof srt); srt.n = 1; srt.color[0] = &rt; op(OP_SET_RT, &srt, sizeof srt);
    p_clear_color cc = {0, 0xf, {0, 0, 1, 1}}; op(OP_CLEAR_COLOR, &cc, sizeof cc);
    p_bind_program bp = {&pg, 3}; op(OP_BIND_PROGRAM, &bp, sizeof bp);
    p_bind_vattr va; memset(&va, 0, sizeof va); va.n = 1; va_st a0 = {0x16, 0, 0}; memcpy(&va.st[0], &a0, 4); op(OP_BIND_VATTR, &va, sizeof va);
    p_bind_vstream vs; memset(&vs, 0, sizeof vs); vs.n = 1; vs.st[0].stride = 8; op(OP_BIND_VSTREAM, &vs, sizeof vs);
    p_bind_vbuf vb; memset(&vb, 0, sizeof vb); vb.n = 1; vb.r[0].address = (NVNbufferAddress)(uintptr_t)vtx; vb.r[0].size = 64; op(OP_BIND_VBUF, &vb, sizeof vb);
    p_bind_buf u0 = {0, 0, (uint64_t)(uintptr_t)vub, 256}, u1 = {1, 0, (uint64_t)(uintptr_t)fub, 256}; op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
    p_bind_handles th; memset(&th, 0, sizeof th); th.stage = 1; th.first = 0; th.n = 1; th.h[0] = 1 | (1u << 20); op(OP_BIND_TEX, &th, sizeof th);
    p_viewports vp; memset(&vp, 0, sizeof vp); vp.n = 1; vp.v[0][2] = 64; vp.v[0][3] = 64; vp.v[0][5] = 1; op(OP_VIEWPORTS, &vp, sizeof vp);
    p_draw dw = {4, 0, 3, 0, 0}; op(OP_DRAW, &dw, sizeof dw);
    p_copy_b2t cp; memset(&cp, 0, sizeof cp); cp.src = (uint64_t)(uintptr_t)readback; cp.tex = &rt; cp.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1}; op(OP_COPY_T2B, &cp, sizeof cp);
    if (argc >= 2 && !strcmp(argv[1],"--alpha-test")) {
        CHECK(mtl_wait(run(),5000000000ull),"alpha fixture initial draw completes");
        const float full[]={-1,-1,3,-1,-1,3}; memcpy(vtx,full,sizeof full);
        /* Exact binary alphas keep EQUAL independent of the sampler's UNORM
         * conversion precision (which differs between valid host drivers). */
        fub[3]=.5f;
        for (int y=0;y<64;y++) for (int x=0;x<64;x++) lin[4*(y*64+x)+3]=((x+y)&1)?255:0;
        CHECK(!nvn_swizzle(&tx.d,0,0,lin,256,tx.mem),"alpha checkerboard is swizzled");
        mtl_pool_invalidate(&P_cpu,(uint64_t)((uint8_t *)tx.mem-cpu_pool),tx.L.size);
        extern void vk_draw_test_correctness_counts(uint64_t out[2]);
        uint64_t before[2],after[2]; vk_draw_test_correctness_counts(before);
        for (unsigned enabled=0;enabled<2;enabled++) {
            totk_live_lines lines={0}; const char *setting=enabled ?
                "set TOTK_VK_ALPHA_TEST=1\n" : "set TOTK_VK_ALPHA_TEST=0\n";
            totk_live_feed(&lines,setting,strlen(setting)); totk_live_frame(enabled+1);
            for (unsigned func=NVN_ALPHA_FUNC_NEVER;func<=NVN_ALPHA_FUNC_ALWAYS;func++) {
                color_st color={0,NVN_LOGIC_OP_COPY,func};
                p_bind_state state={.kind=1}; memcpy(state.bytes,&color,sizeof color);
                p_scissor all={0,0,64,64};
                op(OP_SET_RT,&srt,sizeof srt); op(OP_SCISSOR,&all,sizeof all); op(OP_CLEAR_COLOR,&cc,sizeof cc);
                op(OP_BIND_STATE,&state,sizeof state); op(OP_BIND_VBUF,&vb,sizeof vb);
                const float references[]={.25f,0,.5f,.25f};
                for (unsigned stripe=0;stripe<4;stripe++) {
                    p_scissor area={(int)stripe*16,0,16,64};
                    p_alpha_ref ref={references[stripe]};
                    op(OP_ALPHA_REF,&ref,sizeof ref); op(OP_SCISSOR,&area,sizeof area); op(OP_DRAW,&dw,sizeof dw);
                }
                op(OP_COPY_T2B,&cp,sizeof cp);
                CHECK(mtl_wait(run(),5000000000ull),"alpha func %u enabled %u completes",func,enabled);
                int bad=0;
                for (int y=0;y<64;y++) for (int x=0;x<64;x++) {
                    float a=((x+y)&1)?.5f:0, ref=references[x/16];
                    int pass=!enabled || func==NVN_ALPHA_FUNC_ALWAYS ||
                        (func==NVN_ALPHA_FUNC_LESS && a<ref) || (func==NVN_ALPHA_FUNC_EQUAL && a==ref) ||
                        (func==NVN_ALPHA_FUNC_LEQUAL && a<=ref) || (func==NVN_ALPHA_FUNC_GREATER && a>ref) ||
                        (func==NVN_ALPHA_FUNC_NOTEQUAL && a!=ref) || (func==NVN_ALPHA_FUNC_GEQUAL && a>=ref);
                    const uint8_t *pixel=readback+4*(y*64+x);
                    bad+=pass ? abs(pixel[0]-4*x)>2 || abs(pixel[1]-2*y)>2 ||
                        pixel[2]!=128 || abs(pixel[3]-(int)(a*255+.5f))>1 :
                        pixel[0]!=0 || pixel[1]!=0 || pixel[2]!=255 || pixel[3]!=255;
                }
                CHECK(!bad,"alpha checkerboard comparison %u live %u: %d incorrect pixels",func,enabled,bad);
            }
        }
        vk_draw_test_correctness_counts(after);
        CHECK(after[0]-before[0]==56 && after[1]-before[1]==28,
            "alpha-use counters include switch-off draws, exclude ALWAYS (%llu enabled, %llu ignored)",
            (unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]));
        printf("%s: alpha-test %d checks, %d failed\n",fails?"FAILED":"ok",checks,fails);
        return fails!=0;
    }
    if (argc >= 2 && !strcmp(argv[1],"--clear-on-read")) {
        CHECK(mtl_wait(run(),5000000000ull),"read-before-attach fixture initial draw completes");
        extern uint64_t vk_draw_test_clear_read_count(void);
        tex_t alias=rt; alias.serial=103;
        mtl_reg_tex(2,&rt,NULL); mtl_reg_tex(3,&alias,NULL);
        for (unsigned fixture=0;fixture<4;fixture++) {
            int enabled=fixture&1;
            totk_live_lines lines={0}; const char *setting=enabled ?
                "set TOTK_VK_CLEAR_ON_READ=1\n" : "set TOTK_VK_CLEAR_ON_READ=0\n";
            totk_live_feed(&lines,setting,strlen(setting)); totk_live_frame(fixture+1);
            p_scissor all={0,0,64,64};
            op(OP_SET_RT,&srt,sizeof srt); op(OP_SCISSOR,&all,sizeof all); op(OP_CLEAR_COLOR,&cc,sizeof cc);
            CHECK(mtl_wait(run(),5000000000ull),"read-before-attach old contents are blue");
            uint64_t before=vk_draw_test_clear_read_count();
            mtl_sub_begin();
            p_clear_color green={0,15,{0,1,0,1}};
            vk_draw_exec(OP_SET_RT,&srt); vk_draw_exec(OP_CLEAR_COLOR,&green);
            int target=1; vk_tex *image=NULL;
            (void)vk_tex_by_id(1,&target,NULL); /* unrelated reader must not resolve the target */
            CHECK(vk_draw_test_clear_read_count()==before,"unrelated texture read preserves pending clear");
            (void)vk_tex_by_id(fixture<2?2:3,&target,&image);
            (void)vk_tex_by_id(fixture<2?2:3,&target,NULL); /* cached first reader must still order the write */
            VkBuffer buffer; uint64_t offset=0;
            CHECK(image && vk_resolve((uint64_t)(uintptr_t)readback,64*64*4,&buffer,&offset),
                "first read resolves image and readback buffer");
            vk_barrier_all(vk_cb());
            VkBufferImageCopy copy={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .imageExtent={64,64,1}};
            /* Deliberately do not call vk_end_rendering here: that would flush
             * the pending clear even in the control arm and hide the ordering. */
            vkCmdCopyImageToBuffer(vk_cb(),vk_tex_image(image),VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
            vk_barrier_all(vk_cb());
            CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"first-read copy completes");
            CHECK(vk_draw_test_clear_read_count()-before==(unsigned)enabled,
                "one first-read resolution, including storage alias (%u)",fixture);
            int bad=0;
            for (int pixel=0;pixel<64*64;pixel++) {
                const uint8_t *p=readback+4*pixel;
                bad+=p[0] || p[1]!=(enabled?255:0) || p[2]!=(enabled?0:255) || p[3]!=255;
            }
            CHECK(!bad,"read before any attaching draw sees %s (%d incorrect pixels)",enabled?"NVN clear":"control's old contents",bad);
        }
        printf("%s: clear-on-read %d checks, %d failed\n",fails?"FAILED":"ok",checks,fails);
        return fails!=0;
    }
    if (argc == 3 && !strcmp(argv[1], "--load-elide")) {
        CHECK(mtl_wait(run(), 5000000000ull), "coverage fixture setup completes");
        shader_binary(0, 37, vctl, vcode, &vlen); memcpy(vc, vcode, vlen);
        pg.serial = 99007;
        const float full[] = {-1,-1,3,-1,-1,3}, quad[] = {-1,-1,1,-1,-1,1,-1,1,1,-1,1,1};
        FILE *output = fopen(argv[2], "wb"); if (!output) { perror(argv[2]); return 1; }
        const char *setting = getenv("TOTK_VK_LOAD_ELIDE"); int enabled = setting && atoi(setting) == 1;
        extern void vk_draw_test_load_counts(uint64_t out[2]);
        for (int fixture = 0; fixture < 9; fixture++) {
            memcpy(vtx, fixture == 1 ? quad : full, fixture == 1 ? sizeof quad : sizeof full);
            p_draw first = dw; if (fixture == 1) first.count = 6;
            p_bind_vattr position = va; va_st element = {fixture == 7 ? 0x11 : 0x16,0,0};
            memcpy(position.st,&element,sizeof element);
            p_bind_vstream stride = vs; if (fixture == 7) stride.st[0].stride = 4;
            if (fixture == 7) {
                const uint16_t half[] = {0xbc00,0xbc00,0x4200,0xbc00,0xbc00,0x4200};
                memcpy(vtx,half,sizeof half);
            }
            p_draw_idx indexed = {4,0,3,0,0,1,(uint64_t)(uintptr_t)(cpu_pool+0x202000)};
            cpu_pool[0x202000]=0; cpu_pool[0x202001]=1; cpu_pool[0x202002]=2;
            p_scissor scissor = {0,0,fixture == 2 ? 32 : 64,64};
            color_st color = {fixture == 3 ? 1 : 0,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS};
            p_bind_state cb = {.kind=1}; memcpy(cb.bytes,&color,sizeof color);
            chmask_st channels = {.mask=fixture == 4 ? 7u : 15u};
            p_bind_state cm = {.kind=2}; memcpy(cm.bytes,&channels,sizeof channels);
            ds_st depth = {.dt=fixture == 5,.df=NVN_DEPTH_FUNC_ALWAYS};
            p_bind_state db = {.kind=5}; memcpy(db.bytes,&depth,sizeof depth);
            if (fixture == 6) { /* Geometry is not merely bounded by a full-screen rectangle. */
                float partial[] = {-1,-1,1,-1,-1,1}; memcpy(vtx,partial,sizeof partial);
            }
            uint64_t before[2], after[2]; vk_draw_test_load_counts(before);
            rn = 0; op(OP_SET_RT,&srt,sizeof srt); op(OP_SCISSOR,&scissor,sizeof scissor);
            op(OP_CLEAR_COLOR,&cc,sizeof cc); op(OP_END,NULL,0);
            CHECK(mtl_wait(run(),5000000000ull),"coverage fixture %d initializes prior pixels",fixture);
            rn = 0; op(OP_BIND_PROGRAM,&bp,sizeof bp); op(OP_BIND_STATE,&cb,sizeof cb);
            op(OP_BIND_STATE,&cm,sizeof cm); op(OP_BIND_STATE,&db,sizeof db);
            op(OP_BIND_VATTR,&position,sizeof position); op(OP_BIND_VSTREAM,&stride,sizeof stride);
            if (fixture == 8) op(OP_DRAW_IDX,&indexed,sizeof indexed); else op(OP_DRAW,&first,sizeof first);
            op(OP_COPY_T2B,&cp,sizeof cp); op(OP_END,NULL,0);
            CHECK(mtl_wait(run(),5000000000ull),"coverage fixture %d draws",fixture);
            vk_draw_test_load_counts(after);
            CHECK(after[1]-before[1] == (uint64_t)(enabled && (fixture < 2 || fixture >= 7)),
                "coverage fixture %d elides only full triangle/quad (%llu proven, %llu elided)",fixture,
                (unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]));
            CHECK(fwrite(readback,64*64*4,1,output)==1,"coverage fixture %d writes all pixels",fixture);
            printf("load-elide fixture=%d proven=%llu elided=%llu\n",fixture,
                (unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]));
        }
        CHECK(!fclose(output),"coverage fixture output closes");
        return fails != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--feedback-narrow")) {
        CHECK(mtl_wait(run(),5000000000ull),"feedback fixture setup completes");
        shader_binary(0,37,vctl,vcode,&vlen); memcpy(vc,vcode,vlen); pg.serial=99107;
        const float full[]={-1,-1,3,-1,-1,3}; memcpy(vtx,full,sizeof full);
        fub[0]=fub[1]=fub[2]=fub[3]=1;
        uint8_t actl[0x880], acode[0x230]; uint32_t alen;
        shader_binary(1,46,actl,acode,&alen); memcpy(cpu_pool+0x102000,acode,alen);
        prog_t array=pg; array.serial=99108; array.sd[1].control=actl;
        array.sd[1].data=(uint64_t)(uintptr_t)(cpu_pool+0x102000);
        p_bind_program ab={&array,3};
        tex_t auxiliary=rt; auxiliary.mem=gpu_pool+0x10000; auxiliary.serial=99109;
        tex_t alias=tx; alias.serial=99110;
        mtl_reg_tex(2,&auxiliary,NULL); mtl_reg_tex(3,&alias,NULL);
        p_set_rt two=srt; two.n=2; two.color[1]=&auxiliary;
        p_scissor all={0,0,64,64};
        FILE *output=fopen(argv[2],"wb"); if (!output) { perror(argv[2]); return 1; }
        const char *setting=getenv("TOTK_VK_FEEDBACK_NARROW"); int enabled=setting && atoi(setting)==1;
        extern void vk_draw_test_feedback_narrow_counts(uint64_t out[3]);
        for (int fixture=0;fixture<7;fixture++) {
            rn=0; op(OP_SET_RT,&two,sizeof two); op(OP_SCISSOR,&all,sizeof all);
            op(OP_CLEAR_COLOR,&cc,sizeof cc);
            p_clear_color red={1,15,{1,0,0,1}}; op(OP_CLEAR_COLOR,&red,sizeof red); op(OP_END,NULL,0);
            CHECK(mtl_wait(run(),5000000000ull),"feedback fixture %d initializes attachments",fixture);
            p_set_rt targets=(fixture==0 || fixture==5) ? srt : two;
            if (fixture==1 || fixture==4) targets.color[1]=&tx;
            p_bind_handles sample=th; sample.h[0]=(fixture==2 || fixture==3 || fixture==6 ? 2 : fixture==4 ? 3 : 1)|(1u<<20);
            chmask_st mask={fixture==2 ? 255u : 15u};
            p_bind_state channels={.kind=2}; memcpy(channels.bytes,&mask,sizeof mask);
            uint64_t before[3], after[3]; vk_draw_test_feedback_narrow_counts(before);
            vk_record_test_counts start,end; vk_record_test_counts_get(&start);
            rn=0; op(OP_SET_RT,&targets,sizeof targets); op(OP_BIND_PROGRAM,fixture==2 ? &ab : &bp,sizeof bp);
            op(OP_BIND_STATE,&channels,sizeof channels); op(OP_BIND_VATTR,&va,sizeof va);
            op(OP_BIND_VSTREAM,&vs,sizeof vs); op(OP_BIND_VBUF,&vb,sizeof vb);
            /* The first draw never simultaneously samples an image it writes. */
            p_bind_handles initial=sample; if (fixture==2) initial.h[0]=1|(1u<<20);
            op(OP_BIND_TEX,&initial,sizeof initial); op(OP_DRAW,&dw,sizeof dw);
            int bits=NVN_BARRIER_INVALIDATE_TEXTURE_BIT;
            if (fixture==5) bits|=NVN_BARRIER_INVALIDATE_ZCULL_BIT;
            op(OP_BARRIER,&bits,sizeof bits);
            mask.mask=fixture==3 ? 255u : 15u; memcpy(channels.bytes,&mask,sizeof mask);
            op(OP_BIND_STATE,&channels,sizeof channels);
            op(OP_BIND_PROGRAM,fixture==3 ? &ab : &bp,sizeof bp); op(OP_BIND_TEX,&sample,sizeof sample);
            op(OP_DRAW,&dw,sizeof dw);
            if (fixture==6) {
                mask.mask=255; memcpy(channels.bytes,&mask,sizeof mask);
                op(OP_BIND_STATE,&channels,sizeof channels); op(OP_BIND_PROGRAM,&ab,sizeof ab);
                op(OP_DRAW,&dw,sizeof dw); /* late write restores the deferred boundary */
            }
            op(OP_COPY_T2B,&cp,sizeof cp); op(OP_END,NULL,0);
            CHECK(mtl_wait(run(),5000000000ull),"feedback fixture %d completes",fixture);
            vk_record_test_counts_get(&end); vk_draw_test_feedback_narrow_counts(after);
            unsigned avoided=enabled && fixture<2;
            CHECK(end.passes-start.passes==2-avoided,"feedback fixture %d retains required pass boundaries (%llu passes)",fixture,
                (unsigned long long)(end.passes-start.passes));
            CHECK(after[1]-before[1]==avoided,"feedback fixture %d reports net avoided ends %u",fixture,avoided);
            CHECK(fwrite(readback,64*64*4,1,output)==1,"feedback fixture %d writes all pixels",fixture);
            printf("feedback-narrow fixture=%d deferred=%llu avoided=%llu hazards=%llu passes=%llu\n",fixture,
                (unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]),
                (unsigned long long)(after[2]-before[2]),(unsigned long long)(end.passes-start.passes));
        }
        CHECK(!fclose(output),"feedback output closes"); return fails!=0;
    }
    if (overlay_scene_mode) {
        CHECK(mtl_wait(run(), 5000000000ull), "overlay scene fixture warms a real draw");
        const size_t bytes = 64u << 20;
        uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(memory != MAP_FAILED, "overlay scene target storage");
        if (memory == MAP_FAILED) return 1;
        mtl_pool_add(memory, bytes);
        pool_t pool = {.mem=memory,.size=bytes,.flags=P_gpu.flags};
        tex_t targets[3]; size_t offset = 0;
        for (unsigned i = 0; i < 3; i++) {
            targets[i] = rt; targets[i].d.width = 1920; targets[i].d.height = 1080;
            targets[i].d.format = i == 2 ? 0x34 : 0x25;
            nvn_tex_layout(&targets[i].d, &targets[i].L);
            targets[i].mem = memory + offset; targets[i].pool = &pool; targets[i].serial = 91000 + i;
            offset += (targets[i].L.size + 65535) & ~(size_t)65535;
        }
        CHECK(offset <= bytes, "overlay scene MRT/depth fit fixture pool");
        p_set_rt mrt = {.n=2,.color={&targets[0],&targets[1]},.depth=&targets[2]};
        static const uint32_t sizes[][2] = {{960,536},{1440,808},{1600,896},{1152,648},{960,540}};
        int enabled = vk_overlay_mode();
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            mtl_sub_begin();
            vk_overlay_test_scene_reset(enabled);
            p_viewports stale = vp; stale.v[0][2] = 1280; stale.v[0][3] = 720;
            p_viewports active = stale; active.v[0][2] = sizes[i][0]; active.v[0][3] = sizes[i][1];
            p_scissor partial = {0,0,8,8}, all = {0,0,1920,1080};
            vk_draw_exec(OP_SET_RT, &mrt);
            vk_draw_exec(OP_VIEWPORTS, &stale);
            vk_draw_exec(OP_SCISSOR, &partial);
            vk_draw_exec(OP_CLEAR_COLOR, &cc);
            CHECK(vk_in_pass(), "partial clear opens MRT pass with prior 1280x720 viewport");
            uint32_t sw, sh; vk_overlay_test_scene_size(&sw, &sh);
            CHECK(!sw && !sh, "clear-only pass never nominates the stale viewport");
            vk_draw_exec(OP_VIEWPORTS, &active);
            vk_draw_exec(OP_SCISSOR, &all);
            vk_draw_exec(OP_BIND_PROGRAM, &bp);
            vk_draw_exec(OP_BIND_VATTR, &va); vk_draw_exec(OP_BIND_VSTREAM, &vs);
            vk_draw_exec(OP_BIND_VBUF, &vb); vk_draw_exec(OP_BIND_UBO, &u0); vk_draw_exec(OP_BIND_UBO, &u1);
            vk_draw_exec(OP_BIND_TEX, &th);
            vk_record_test_counts before, after;
            vk_record_test_counts_get(&before);
            vk_draw_exec(OP_DRAW, &dw);
            vk_record_test_counts_get(&after);
            CHECK(after.draws == before.draws + 1, "scene %ux%u records an actual Vulkan draw", sizes[i][0], sizes[i][1]);
            vk_overlay_test_scene_size(&sw, &sh);
            CHECK(enabled ? sw == sizes[i][0] && sh == sizes[i][1] : !sw && !sh,
                  "aligned DRS draw reports %ux%u rather than stale/canvas extent (got %ux%u; enabled=%d)",
                  sizes[i][0], sizes[i][1], sw, sh, enabled);
            if (i == 4) {
                active.v[0][2] = 1152; active.v[0][3] = 648;
                vk_draw_exec(OP_VIEWPORTS, &active); vk_draw_exec(OP_DRAW, &dw);
                vk_overlay_test_scene_size(&sw, &sh);
                CHECK(enabled ? sw == 1152 && sh == 648 : !sw && !sh,
                      "viewport changes re-evaluate the next real draw in the same open pass");
            }
            vk_end_rendering();
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "overlay scene draw %u completes", i);
        }
        /* Existing landscape/depth/MRT exclusions must survive the alignment tolerance. */
        for (unsigned kind = 0; kind < 3; kind++) {
            mtl_sub_begin(); vk_overlay_test_scene_reset(enabled);
            p_set_rt excluded = mrt;
            if (kind == 0) excluded.n = 1;
            if (kind == 1) excluded.depth = NULL;
            p_viewports active = vp; active.v[0][2] = kind == 2 ? 512 : 960;
            active.v[0][3] = kind == 2 ? 512 : 540;
            vk_draw_exec(OP_SET_RT, &excluded); vk_draw_exec(OP_VIEWPORTS, &active);
            vk_draw_exec(OP_DRAW, &dw);
            uint32_t sw, sh; vk_overlay_test_scene_size(&sw, &sh);
            CHECK(!sw && !sh, "scene exclusion %u rejects UI/depthless/shadow draws", kind);
            vk_end_rendering();
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "excluded draw %u completes", kind);
        }
        printf("%s: overlay scene %d checks, %d failed (enabled=%d)\n", fails ? "FAILED" : "ok", checks, fails, enabled);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--walker-cpu")) {
        /* A sleeping submission proves raw elapsed, not legacy wait-adjusted recording, bounds CPU.
           Then exercise the real 300-present log with one textured draw per submission. */
        struct timespec pause = {.tv_nsec = 20000000};
        mtl_sub_begin(); while (nanosleep(&pause, &pause) && errno == EINTR) {}
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "CPU-clock probe submission completes");
        double recording, wait, gpu, cpu, elapsed; uint64_t submissions;
        vk_rec_lock(); vk_perf_take(&recording, &wait, &gpu, &cpu, &elapsed, &submissions); vk_rec_unlock();
        CHECK(submissions == 1, "CPU-clock probe counts exactly one submission");
        CHECK(isfinite(cpu) && cpu > 0 && cpu <= elapsed && elapsed < 10000,
              "thread CPU %.6f ms is positive and bounded by raw elapsed %.6f ms", cpu, elapsed);
        CHECK(recording >= 0 && recording <= elapsed, "legacy recording %.6f ms is bounded by raw elapsed", recording);
        CHECK(elapsed - cpu >= 10, "raw elapsed minus CPU retains the 20-ms sleep (%.6f ms)", elapsed - cpu);
        vk_rec_lock(); vk_perf_take(&recording, &wait, &gpu, &cpu, &elapsed, &submissions); vk_rec_unlock();
        CHECK(!cpu && !elapsed && !recording && !submissions, "timing take resets the entire submission window");
        op(OP_END, NULL, 0);
        for (int frame = 0; frame < 300; frame++) {
            mtl_sub_begin(); mtl_exec(rec);
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "metric frame %d finishes", frame);
            mtl_present(NULL, 1.0 / 60, NULL);
        }
        vk_record_test_counts counts; vk_record_test_counts_get(&counts);
        CHECK(counts.draws == 300, "metric window records exactly 300 real draws");
        printf("%d checks, %d failed\n", checks, fails);
        return fails ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--xlate-stats")) {
        CHECK(vk_xlate_stats_on(), "translation statistics explicitly enabled by environment");
        size_t bytes = rn; uint8_t *saved = malloc(bytes);
        CHECK(saved != NULL, "retain diagnostic fixture recording");
        if (!saved) return 1;
        memcpy(saved, rec, bytes);
        CHECK(mtl_wait(run(), 5000000000ull), "initial diagnostic recording completes");
        CHECK(vk_draw_test_xlate_frame(1), "initial diagnostic counts conserve requests");
        memcpy(rec, saved, bytes); rn = bytes; op(OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(rec); mtl_exec(rec); rn = 0;
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "same-submission duplicate recordings complete");
        CHECK(vk_draw_test_xlate_frame(2), "duplicate diagnostic counts conserve requests");
        const uint64_t shift = 0x10000;
        memcpy((uint8_t *)vub + shift, vub, 256); memcpy((uint8_t *)fub + shift, fub, 256);
        memcpy((uint8_t *)vtx + shift, vtx, 64);
        for (size_t at = 0; at < bytes; ) {
            op_hdr h; memcpy(&h, saved + at, sizeof h); void *payload = saved + at + sizeof h;
            if (h.op == OP_BIND_UBO) ((p_bind_buf *)payload)->addr += shift;
            if (h.op == OP_BIND_VBUF) ((p_bind_vbuf *)payload)->r[0].address += shift;
            at += h.size;
        }
        memcpy(rec, saved, bytes); rn = bytes;
        CHECK(mtl_wait(run(), 5000000000ull), "address-shifted diagnostic recording completes");
        CHECK(vk_draw_test_xlate_frame(3), "address-shifted diagnostic counts conserve requests");
        CHECK(vk_draw_test_xlate_frame(4), "empty frame rotates previous-recording set");
        memcpy(rec, saved, bytes); rn = bytes;
        CHECK(mtl_wait(run(), 5000000000ull), "recording after empty frame completes");
        for (uint64_t frame = 5; frame <= 300; frame++)
            CHECK(vk_draw_test_xlate_frame(frame), "diagnostic periodic report has sane counts");
        free(saved);
        printf("%s: translation diagnostic %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--atlas-uv-race")) {
        uint8_t actl[0x880], acode[0x230]; uint32_t alen;
        shader_binary(1, 43, actl, acode, &alen);
        memcpy(cpu_pool + 0x104000, acode, alen);
        pg.sd[1].control = actl; pg.sd[1].data = (uint64_t)(uintptr_t)(cpu_pool + 0x104000);
        fub[0] = fub[1] = .75f;
        CHECK(mtl_wait(run(), 5000000000ull), "atlas UV fixture warms its real textured pipeline");
        rn = 0;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_UBO, &u0, sizeof u0);
        op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th);
        op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
        uint64_t handle = (uint64_t)(uintptr_t)rec;
        VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL);
        vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
        mtl_exec(rec); rn = 0;
        int want = 1; vk_tex *object = NULL;
        VkImageView before = vk_tex_by_id(1, &want, &object);
        uint64_t revision = vk_treg_revision();
        uint64_t fence = mtl_sub_commit();
        /* Same descriptor, next frame's UVs: the wrong atlas region fills the
           original quad when the GPU reads unprotected guest data late. */
        int waited = lag_test_rewrite(fub, 8, 0, 0, gate);
        CHECK(mtl_wait(fence, 5000000000ull), "atlas UV gated GPU work completes");
        CHECK(before && vk_tex_by_id(1, &want, NULL) == before && vk_treg_revision() == revision,
            "UV rewrite changes neither texture registry identity nor sampled image view");
        const uint8_t *pixel = readback + 4 * (10 * 64 + 10);
        printf("atlas UV race: capture=%d writer_waited=%d pixel=%u,%u,%u,%u (submit-time UV wants 192,192,128,255; reused UV wants 0,0,128,255)\n",
            vk_lag_capture_on(), waited, pixel[0], pixel[1], pixel[2], pixel[3]);
        CHECK(abs(pixel[0] - 192) <= 2 && abs(pixel[1] - 192) <= 2 && pixel[2] == 128 && pixel[3] == 255,
            "GPU samples submit-time atlas region, not the next frame's region");
        vkDestroyEvent(V.dev, gate, NULL);
        printf("%s: atlas UV race %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--texture-registry")) {
        CHECK(mtl_wait(run(), 5000000000ull), "registry pixel fixture warms the real pipeline");
        tex_t replacement = tx; replacement.serial = 7101; replacement.mem = cpu_pool + 0x18000;
        replacement.d.width = replacement.d.height = 32;
        nvn_tex_layout(&replacement.d, &replacement.L);
        uint8_t green[32 * 32 * 4];
        for (unsigned i = 0; i < sizeof green; i += 4) {
            green[i] = 12; green[i + 1] = 240; green[i + 2] = 60; green[i + 3] = 255;
        }
        CHECK(!nvn_swizzle(&replacement.d, 0, 0, green, 128, replacement.mem), "replacement has different dimensions and pixels");
        /* Keep every guest binding unchanged. Only the registry writer changes
           id 1; worker packets already extracted must retain the earlier view. */
        for (int across = 0; across < 2; across++) for (int pool_switch = 0; pool_switch < 2; pool_switch++) {
            mtl_reg_tex(1, &tx, NULL);
            rn = 0;
            op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
            op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va);
            op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb, sizeof vb);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
            op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
            mtl_sub_begin();
            for (int version = 0; version < 2; version++) {
                if (version) {
                    if (across) mtl_sub_begin();
                    if (pool_switch) mtl_set_tex_pool_n(16);
                    mtl_reg_tex(1, &replacement, NULL);
                }
                for (int stripe = 0; stripe < 32; stripe++) {
                    p_scissor sc = {version * 32 + stripe, 0, 1, 64};
                    op(OP_SCISSOR, &sc, sizeof sc); op(OP_DRAW, &dw, sizeof dw);
                }
                if (version) op(OP_COPY_T2B, &cp, sizeof cp);
                op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
                if (!version && across) (void)mtl_sub_commit();
            }
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "registry replacement completes (across=%d pool=%d)", across, pool_switch);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                const uint8_t *p = readback + 4 * (y * 64 + x);
                int r = y >= 32 ? 0 : x < 32 ? 4 * x : 12;
                int g0 = y >= 32 ? 0 : x < 32 ? 2 * y : 120;
                int b0 = y >= 32 ? 255 : x < 32 ? 128 : 60;
                bad += abs(p[0] - r) > 2 || abs(p[1] - g0) > 2 || abs(p[2] - b0) > 2 || p[3] != 255;
            }
            CHECK(!bad, "same-id replacement retains old draws and samples new pixels (across=%d pool=%d, %d wrong)", across, pool_switch, bad);
        }
        mtl_set_tex_pool_n(0); mtl_reg_tex(1, NULL, NULL);
        mtl_tex_release(&replacement);
        printf("%s: texture registry pixels %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--descriptor-stock")) {
        CHECK(mtl_wait(run(), 5000000000ull), "descriptor stock fixture warmup completes");
        mtl_drain();
        int stock = vk_desc_stock_enabled();
        CHECK(vk_record_test_descriptor_stock(), "layout-isolated stock checkout, retirement, batching and pressure rebuild");
        CHECK(vk_record_test_descriptor_ceiling(), "stock ceiling: set limit, reuse under it, live lowering, pool limit and live off");
        /* The pool shape: a layout with one binding of every declared type comes from a fresh real pool (it used to
           need SAMPLER no more than one per set); a type no pool declares is refused before it could reach one. */
        {
            VkDescriptorSetLayoutBinding every[VK_DESC_POOL_TYPES]; char why[128];
            for (unsigned t = 0; t < VK_DESC_POOL_TYPES; t++)
                every[t] = (VkDescriptorSetLayoutBinding){t, vk_desc_pool_sizes[t].type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
            CHECK(vk_desc_layout_fits(every, VK_DESC_POOL_TYPES, why, sizeof why), "every declared type fits: %s", why);
            VkDescriptorSetLayoutCreateInfo all = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = VK_DESC_POOL_TYPES, .pBindings = every};
            VkDescriptorSetLayout every_dsl; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &all, NULL, &every_dsl));
            mtl_sub_begin();
            for (int i = 0; i < 64; i++) CHECK(vk_alloc_set(every_dsl) != VK_NULL_HANDLE, "an every-type set %d comes from a real pool", i);
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "every-type submission completes");
            vkDestroyDescriptorSetLayout(V.dev, every_dsl, NULL);
            static const VkDescriptorType undeclared[] = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC};
            for (unsigned i = 0; i < 3; i++) {
                VkDescriptorSetLayoutBinding one[2] = {every[6], {1, undeclared[i], 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
                CHECK(!vk_desc_layout_fits(one, 2, why, sizeof why) && strstr(why, "no descriptor pool declares"),
                    "undeclared type %d is refused at layout creation: %s", (int)undeclared[i], why);
            }
            VkDescriptorSetLayoutBinding many = {0, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1025, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
            CHECK(!vk_desc_layout_fits(&many, 1, why, sizeof why) && strstr(why, "exceed"), "a set larger than a pool is refused: %s", why);
        }
        /* Primary slots through real submissions: 100 sets per submission, eight warm-up submissions (one per ring
           slot), then two more passes of the ring. Each set is checked out once per submission. */
        VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
        VkDescriptorSetLayout dsl; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &dsl));
        uint64_t before[8], after[8]; int unique = 1;
        for (int round = 0; round < 24; round++) {
            if (round == 8) vk_record_test_descriptors(before);
            VkDescriptorSet sets[100];
            mtl_sub_begin();
            for (int i = 0; i < 100; i++) { sets[i] = vk_alloc_set(dsl); for (int k = 0; k < i; k++) unique &= sets[k] != sets[i]; }
            uint64_t fence = mtl_sub_commit(); CHECK(mtl_wait(fence, 5000000000ull), "descriptor stock primary submission %d completes", round);
        }
        vk_record_test_descriptors(after);
        uint64_t sets = after[0] - before[0], calls = after[1] - before[1], resets = after[2] - before[2];
        printf("descriptor-stock primary stock=%d 16 warm submissions: sets=%llu calls=%llu resets=%llu\n", stock,
            (unsigned long long)sets, (unsigned long long)calls, (unsigned long long)resets);
        CHECK(unique, "primary slot never issues one set twice in a submission");
        uint32_t ceiling = vk_desc_stock_ceiling();
        if (stock && ceiling && ceiling < 100)   /* every submission needs more than the ceiling: reset-and-allocate */
            CHECK(sets == 1600 && resets >= 16, "a ceiling below each submission's 100 sets falls back to allocating every set and resetting per slot (%llu/%llu/%llu)",
                (unsigned long long)sets, (unsigned long long)calls, (unsigned long long)resets);
        else if (stock) CHECK(!sets && !calls && !resets, "warm primary stock needs no allocations or pool resets (%llu/%llu/%llu)",
            (unsigned long long)sets, (unsigned long long)calls, (unsigned long long)resets);
        else CHECK(sets == 1600 && calls == 1600 && resets >= 16, "default primary path allocates per set and resets per slot (%llu/%llu/%llu)",
            (unsigned long long)sets, (unsigned long long)calls, (unsigned long long)resets);
        vk_record_stats(300);
        printf("%s: descriptor stock %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--descriptor-stock-live")) {
        /* TOTK_VK_DESC_STOCK and its ceiling toggled through the live channel every few frames while up to BACKLOG
           frames are in flight: 64 draws a frame, each into its own 8x8 tile with its own colour UBO (so its own
           descriptor set), the tile -> UBO assignment rotating per frame. Every frame's pixels are checked once its
           fence retires, and both owners count, right before they reset or destroy pools, whether every submission
           that may reference them has retired (vk_test_descriptor_pools_check). */
        enum { TILES = 64, FRAMES = 240, BACKLOG = 6, READBACK = 64 * 64 * 4 };
        CHECK(mtl_wait(run(), 5000000000ull), "descriptor stock live fixture warmup completes");
        uint8_t *colors = cpu_pool + 0x220000, *readbacks = cpu_pool + 0x400000;
        for (int k = 0; k < TILES; k++) {
            float c[4] = {(float)(k % 8 + 1) / 8, (float)(k / 8 + 1) / 8, 1, 1};
            memcpy(colors + k * 256, c, sizeof c);
        }
        const float full_tri[6] = {-1, -1, 3, -1, -1, 3};   /* full-target triangle, written before any frame reads it */
        memcpy(cpu_pool + 0x202000, full_tri, sizeof full_tri);
        p_bind_vbuf full = vb; full.r[0].address = (uint64_t)(uintptr_t)(cpu_pool + 0x202000);
        static const char *const toggles[] = {"set TOTK_VK_DESC_STOCK=1\n", "set TOTK_VK_DESC_STOCK_MAX_SETS=16\n",
            "set TOTK_VK_DESC_STOCK=0\n", "set TOTK_VK_DESC_STOCK=1\nset TOTK_VK_DESC_STOCK_MAX_SETS=1\n",
            "unset TOTK_VK_DESC_STOCK_MAX_SETS\n", "set TOTK_VK_DESC_STOCK_MAX_SETS=48\n", "unset TOTK_VK_DESC_STOCK\n",
            "set TOTK_VK_DESC_STOCK=1\nset TOTK_VK_DESC_STOCK_MAX_SETS=0\n"};
        uint64_t fences[FRAMES], events0[VK_DESC_STOCK_REASONS + 1], events1[VK_DESC_STOCK_REASONS + 1];
        uint64_t pool_checks0, violations0, pool_checks1, violations1;
        vk_record_test_stock_events(events0); vk_test_descriptor_pools_stats(&pool_checks0, &violations0);
        int bad_frames = 0, applied = 0, in_flight = 0;
        for (int frame = 0; frame < FRAMES + BACKLOG; frame++) {
            int done = frame - BACKLOG;
            if (done >= 0) {   /* the frame that used this readback slot: wait for it, then check every tile */
                CHECK(mtl_wait(fences[done], 30000000000ull), "descriptor stock live frame %d completes", done);
                const uint8_t *out = readbacks + (done % BACKLOG) * READBACK; int bad = 0;
                for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                    int k = ((y / 8) * 8 + x / 8 + done) % TILES;
                    float r = (float)(k % 8 + 1) / 8, g = (float)(k / 8 + 1) / 8;
                    const uint8_t *p = out + 4 * (y * 64 + x);
                    bad += abs(p[0] - (int)(4 * x * r + .5f)) > 2 || abs(p[1] - (int)(4 * y * g + .5f)) > 2 ||
                        abs(p[2] - 128) > 2 || p[3] != 255;
                }
                if (bad && bad_frames++ < 4) printf("descriptor stock live frame %d: %d wrong pixels\n", done, bad);
            }
            if (frame >= FRAMES) continue;
            rn = 0;
            p_scissor all = {0, 0, 64, 64};
            op(OP_SET_RT, &srt, sizeof srt); op(OP_SCISSOR, &all, sizeof all); op(OP_CLEAR_COLOR, &cc, sizeof cc);
            op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
            op(OP_BIND_VBUF, &full, sizeof full);
            for (int t = 0; t < TILES; t++) {
                p_bind_buf u = u1; u.addr = (uint64_t)(uintptr_t)(colors + ((t + frame) % TILES) * 256);
                p_scissor tile = {(t % 8) * 8, (t / 8) * 8, 8, 8};
                op(OP_BIND_UBO, &u, sizeof u); op(OP_SCISSOR, &tile, sizeof tile); op(OP_DRAW, &dw, sizeof dw);
            }
            p_copy_b2t copy = cp; copy.src = (uint64_t)(uintptr_t)(readbacks + (frame % BACKLOG) * READBACK);
            op(OP_COPY_T2B, &copy, sizeof copy);
            fences[frame] = run(); in_flight += frame > 0 && !mtl_wait(fences[frame - 1], 0);   /* the previous frame still runs */
            if (frame % 3 == 2) {   /* stage like totk.ctl; applied at this "presented" frame */
                const char *lines = toggles[applied++ % (sizeof toggles / sizeof *toggles)];
                totk_live_lines l = {{0}}; totk_live_feed(&l, lines, strlen(lines));
            }
            totk_live_frame((uint64_t)frame);
        }
        mtl_drain();
        vk_record_test_stock_events(events1); vk_test_descriptor_pools_stats(&pool_checks1, &violations1);
        uint64_t kept[4]; vk_record_test_descriptor_retained(kept);
        printf("descriptor-stock-live: %d frames (%d committed while the previous one ran), %d toggles, pool checks %llu, violations %llu; resets off/pressure/set-limit/pool-limit %llu/%llu/%llu/%llu, pools destroyed %llu; retained pools %llu+%llu, stock sets %llu\n",
            FRAMES, in_flight, applied, (unsigned long long)(pool_checks1 - pool_checks0), (unsigned long long)(violations1 - violations0),
            (unsigned long long)(events1[0] - events0[0]), (unsigned long long)(events1[1] - events0[1]),
            (unsigned long long)(events1[2] - events0[2]), (unsigned long long)(events1[3] - events0[3]),
            (unsigned long long)(events1[4] - events0[4]), (unsigned long long)kept[0], (unsigned long long)kept[1],
            (unsigned long long)kept[2]);
        CHECK(!bad_frames, "every in-flight frame keeps its own descriptors through live stock toggles (%d bad frames)", bad_frames);
        CHECK(pool_checks1 > pool_checks0 && violations1 == violations0,
            "no pool is reset or destroyed while a submission that may reference it is unretired");
        CHECK(events1[VK_DESC_STOCK_OFF] > events0[VK_DESC_STOCK_OFF] && events1[VK_DESC_STOCK_SET_LIMIT] > events0[VK_DESC_STOCK_SET_LIMIT] &&
            events1[VK_DESC_STOCK_REASONS] > events0[VK_DESC_STOCK_REASONS], "the toggles exercised live off, the set limit and pool destruction");
        printf("%s: descriptor stock live %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--program-resolve-bench")) {
        CHECK(mtl_wait(run(), 5000000000ull), "program benchmark warmup completes");
        vk_shader *expected[6]; int expected_result = vk_prog_resolve_record(&pg, expected), valid = !expected_result;
        enum { NDRAWS = 1000000 };
        for (int dirty = 0; dirty < 2; dirty++) {
            uint64_t start = host_now_ns();
            for (int draw = 0; draw < NDRAWS; draw++) {
                if (dirty) vk_stage_sig_invalidate(1u << 1);
                vk_shader *resolved[6]; int result = vk_prog_resolve_record(&pg, resolved);
                valid &= result == expected_result && !memcmp(resolved, expected, sizeof expected);
            }
            printf("program-resolve-bench %s: %.2f ns/draw\n", dirty ? "invalidated" : "stable",
                (double)(host_now_ns() - start) / NDRAWS);
        }
        CHECK(valid, "program benchmark preserves all stage pointers and result");
        printf("%s: program resolution %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--pipeline-resolve-bench")) {
        CHECK(mtl_wait(run(), 5000000000ull), "pipeline benchmark warmup completes");
        vk_shader *shaders[6];
        CHECK(!vk_prog_resolve_record(&pg, shaders), "pipeline benchmark resolves both stages");
        if (fails) return 1;
        return vk_draw_test_pipeline_bench(shaders[0], shaders[1]);
    }
    if (argc == 2 && !strcmp(argv[1], "--chunk-policy")) {
        uint64_t warm = run();
        CHECK(mtl_wait(warm, 5000000000ull), "chunk policy warmup completes");
        const char *limit = getenv("TOTK_VK_CHUNK_DRAWS");
        long expected_limit = limit && *limit ? atol(limit) : runtime_frame_lag_requested() ? 600 : 75;
        long actual_limit = vk_frame_chunk_limit();
        CHECK(actual_limit == expected_limit, "chunk policy resolved limit %ld matches startup expectation %ld",
              actual_limit, expected_limit);
        uint64_t expected_slots = 1, pending_draws = 1; /* warmup draw remains in the backend's draw counter */
        vk_record_test_counts before, after; vk_record_test_counts_get(&before);
        uint64_t serial = vk_submit_serial();
        mtl_sub_begin();
        for (unsigned pass = 0; pass < 200; pass++) {
            if (expected_limit > 0 && pending_draws >= (uint64_t)expected_limit) {
                expected_slots++; pending_draws = 0;
            }
            for (unsigned draw = 0; draw < 4; draw++) op(OP_DRAW, &dw, sizeof dw);
            op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
            vk_end_rendering(); /* a real recorder-aware pass boundary, not a forced chunk */
            pending_draws += 4;
        }
        op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        uint64_t fence = mtl_sub_commit();
        CHECK(mtl_wait(fence, 5000000000ull), "chunk policy actual GPU draws complete");
        vk_record_test_counts_get(&after);
        CHECK(after.draws - before.draws == 800, "chunk policy records every draw");
        CHECK(vk_submit_serial() - serial == expected_slots,
              "chunk policy limit %ld seals %llu slots, expected %llu", expected_limit,
              (unsigned long long)(vk_submit_serial() - serial), (unsigned long long)expected_slots);
        const uint8_t *pixel = readback + (8 * 64 + 16) * 4;
        CHECK(abs(pixel[0] - 64) <= 2 && abs(pixel[1] - 16) <= 2 && abs(pixel[2] - 128) <= 2 && pixel[3] == 255,
              "chunk policy preserves final textured pixels");
        printf("%s: chunk policy limit=%ld actual_slots=%llu draws=800; %d checks, %d failed\n",
               fails ? "FAILED" : "ok", expected_limit, (unsigned long long)(vk_submit_serial() - serial), checks, fails);
        return fails != 0;
    }
    /* vkrectest invokes this same fixture in fresh processes to compare the exact stream across worker counts. */
    if (argc == 2 && !strncmp(argv[1], "--rec-fail-", 11)) {
        if (!strcmp(argv[1], "--rec-fail-info")) vk_test_fail_heap("j->info");
        else if (!strcmp(argv[1], "--rec-fail-calloc")) vk_test_fail_heap("calloc");
        else if (!strcmp(argv[1], "--rec-fail-slab")) vk_test_fail_heap("recorder-slab");
        else if (!strcmp(argv[1], "--rec-fail-end")) {
            fault_real_end = vkEndCommandBuffer; vkEndCommandBuffer = fail_recorder_end;
        } else {
            CHECK(mtl_wait(run(), 5000000000ull), "failure injection setup completes");
            if (!strcmp(argv[1], "--rec-fail-map")) vkMapMemory = fail_map;
            else if (!strcmp(argv[1], "--rec-fail-allocate")) vkAllocateMemory = fail_allocate;
            else return 2;
            VkBuffer buffer; VkDeviceSize offset;
            (void)vk_transient(32u << 20, 256, &buffer, &offset); /* larger than any setup chunk */
        }
        (void)run(); DIE("injected allocation failure unexpectedly continued");
    }
    if (lease_mode) {
        enum { SMALL_PASSES = 32, FRAMES = 84 };
        /* The GPU-slot threshold counts across NVN commits: a whole multiple of600
           holds its phase fixed, so later frames do not introduce new slot-size peaks. */
        const int LARGE_PASSES = storage_warm_mode ? 1200 : 384;
        const int warm_frames = storage_warm_mode ? 32 : 10;
        CHECK(mtl_wait(run(), 5000000000ull), "lease fixture setup completes");
        CHECK(vk_lag_capture_on(), "lease fixture exercises protected frame lag");
        nvn_dirty_remove_range(cpu_pool); nvn_dirty_add_range(cpu_pool, 0x280000);
        nvn_dirty_protect(tx.mem, tx.L.size); vk_record_test_native_guards(1);
        vk_record_test_delay(1000);
        lease_gate gate = {0}; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate.gate));
        pthread_t opener; if (pthread_create(&opener, NULL, lease_gate_run, &gate)) abort();
        uint64_t warm_capacity = 0, warm_peak = 0, warm_cold = 0, accepted = 0;
        vk_record_test_costs warm_cost = {0}, final_cost;
        uint64_t walk_body_ns = 0;
        uint64_t warm_packets[4] = {0}, warm_storage[4] = {0}, maps_warm = 0, anon_warm = 0, named_warm = 0;
        vk_record_test_counts before, after; vk_record_test_counts_get(&before);
        for (int frame = 0; frame < FRAMES; frame++) {
            int passes = storage_warm_mode || frame < 10 || frame >= FRAMES - 2 ? LARGE_PASSES : SMALL_PASSES;
            rn = 0; op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
            for (int i = 0; i < passes; i++) {
                int barrier = NVN_BARRIER_INVALIDATE_ZCULL_BIT; op(OP_BARRIER, &barrier, sizeof barrier);
                op(OP_DRAW, &dw, sizeof dw); accepted++;
            }
            if (frame >= 2) op(OP_COPY_T2B, &cp, sizeof cp);
            op(OP_END, NULL, 0);
            uint64_t walk_start = host_now_ns();
            mtl_sub_begin();
            if (!frame) vkCmdWaitEvents(vk_cb(), 1, &gate.gate, VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
            mtl_exec(rec); rn = 0;
            uint64_t ticket = mtl_sub_commit();
            if (frame >= warm_frames) walk_body_ns += host_now_ns() - walk_start;
            if (!frame) continue; /* both large slots retain distinct leases while the GPU is closed */
            if (frame == 1) {
                uint64_t leases[4], memory[4]; vk_record_test_leases(leases); vk_record_memory(memory);
                CHECK(!mtl_wait(ticket, 0), "two queued large slots remain genuinely GPU-gated");
                CHECK(!vk_record_enabled() || (leases[0] >= 2 * LARGE_PASSES && memory[2] >= 2 * LARGE_PASSES),
                    "GPU-pending slots cannot return/reuse their %d secondary leases (%llu active, %llu handles)",
                    2 * LARGE_PASSES, (unsigned long long)leases[0], (unsigned long long)memory[2]);
                atomic_store_explicit(&gate.release, 1, memory_order_release); pthread_join(opener, NULL);
                CHECK(!gate.timed_out, "two large-slot commits return without waiting on an unsubmitted/GPU-gated slot");
            }
            CHECK(mtl_wait(ticket, 30000000000ull), "lease frame %d completes", frame);
            if (frame == 1) {
                /* A native-policy software-GPU write to untracked readback requires a real fence fallback.
                   Read only AFTER opening the gate; that fallback must not rescue the lease test. */
                op(OP_COPY_T2B, &cp, sizeof cp);
                CHECK(mtl_wait(run(), 5000000000ull), "gated large-slot pixels are read back after genuine completion");
            }
            uint64_t leases[4], memory[4]; vk_record_test_leases(leases); vk_record_memory(memory);
            CHECK(!vk_record_enabled() || memory[2] <= leases[1] + 7 * 5,
                "lease frame %d capacity tracks actual simultaneous worker peaks, not eight slot peaks (%llu/%llu)",
                frame, (unsigned long long)memory[2], (unsigned long long)leases[1]);
            if (frame == warm_frames - 1) {
                warm_capacity = memory[2]; warm_peak = leases[1]; warm_cold = leases[3];
                vk_record_test_costs_get(&warm_cost);
                vk_record_memory(warm_packets); vk_record_test_storage(warm_storage);
                maps_warm = recorder_maps(&named_warm, &anon_warm);
            }
            if (frame >= warm_frames) CHECK(!vk_record_enabled() || (memory[2] >= warm_capacity &&
                (leases[1] != warm_peak || memory[2] == warm_capacity)),
                "lease frame %d keeps warm handles and allocates only for a new worker-demand peak", frame);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                uint8_t *p = readback + 4 * (y * 64 + x);
                bad += y < 32 ? abs(p[0] - 4 * x) > 2 || abs(p[1] - 2 * y) > 2 || p[2] != 128 || p[3] != 255 :
                    p[0] || p[1] || p[2] != 255 || p[3] != 255;
            }
            CHECK(!bad, "cross-slot lease frame %d preserves every pixel (%d wrong)", frame, bad);
        }
        vk_record_test_counts_get(&after);
        CHECK(after.draws - before.draws == accepted, "cross-slot leases retain all %llu draws", (unsigned long long)accepted);
        uint64_t leases[4]; vk_record_test_leases(leases);
        CHECK(!vk_record_enabled() || leases[3] == warm_cold, "ordinary warm capacity incurs no cold-tail resets");
        vk_record_test_costs_get(&final_cost);
        CHECK(!vk_record_enabled() || (final_cost.publications &&
            final_cost.return_batches[0] + final_cost.return_batches[1] <= final_cost.publications * 5 &&
            leases[2] > final_cost.publications * 8),
            "return handoff counts slot publications/owner batches, not individual handles");
        if (storage_warm_mode) {
            uint64_t memory[4], policy[4], named, anonymous;
            uint64_t maps = recorder_maps(&named, &anonymous);
            vk_record_memory(memory); vk_record_test_storage(policy);
            CHECK(!vk_record_enabled() || warm_cost.referenced_bytes > policy[0],
                "warm-storage fixture genuinely exceeds its16MiB allowance with referenced scene arrays");
            CHECK(!vk_record_enabled() || memory[0] + memory[1] <= final_cost.referenced_bytes + policy[0],
                "warm-scene unreferenced slab capacity stays within its idle allowance");
            CHECK(!vk_record_enabled() || (memory[0] == warm_packets[0] && memory[1] == warm_packets[1] &&
                policy[1] == warm_storage[1] && policy[2] == warm_storage[2]),
                "%d warmed over-budget frames retain scene slabs without evict/rebuild cycles (packet/info %llu/%llu -> %llu/%llu bytes, reclaims %llu -> %llu)",
                FRAMES - warm_frames, (unsigned long long)warm_packets[0], (unsigned long long)warm_packets[1],
                (unsigned long long)memory[0], (unsigned long long)memory[1],
                (unsigned long long)warm_storage[1], (unsigned long long)policy[1]);
            CHECK(named == named_warm, "warm scene retains exactly its recorder-owned VMA count (%llu -> %llu)",
                (unsigned long long)named_warm, (unsigned long long)named);
            /* Whole-process totals also include Vulkan's lazy BO mappings and sanitizer runtime
               metadata; use the same bound as the existing many-small-job storage fixture. */
            CHECK(maps && maps <= maps_warm + 1024 && anonymous <= anon_warm + 128,
                "warm over-budget recording keeps VMAs bounded (%llu/%llu -> %llu/%llu total/anonymous)",
                (unsigned long long)maps_warm, (unsigned long long)anon_warm,
                (unsigned long long)maps, (unsigned long long)anonymous);
            printf("recorder warm storage fixture: packet/info %.1f/%.1f MiB, referenced %.1f MiB, budget %llu MiB; warmed slab reclaims %llu\n",
                memory[0] / 1048576.0, memory[1] / 1048576.0, final_cost.referenced_bytes / 1048576.0,
                (unsigned long long)(policy[0] >> 20), (unsigned long long)(policy[1] - warm_storage[1]));
        }
        for (int side = 0; side < 2; side++) {
            double frames = FRAMES - warm_frames;
            printf("recorder lease cost ns/frame %s: acquire %.0f return %.0f cold maintenance %.0f; owner batches/frame %.2f\n",
                side ? "workers" : "walking", (final_cost.lease_ns[side] - warm_cost.lease_ns[side]) / frames,
                (final_cost.return_ns[side] - warm_cost.return_ns[side]) / frames,
                (final_cost.reset_ns[side] - warm_cost.reset_ns[side]) / frames,
                (final_cost.return_batches[side] - warm_cost.return_batches[side]) / frames);
        }
        printf("recorder lease cost ns/frame: background publication %.0f, walking slab prune %.0f arena slot/snapshot %.0f/%.0f; walking body+commit %.0f (includes publication joins)\n",
            (final_cost.publish_ns - warm_cost.publish_ns) / (double)(FRAMES - warm_frames),
            (final_cost.slab_trim_ns - warm_cost.slab_trim_ns) / (double)(FRAMES - warm_frames),
            (final_cost.arena_ns[0] - warm_cost.arena_ns[0]) / (double)(FRAMES - warm_frames),
            (final_cost.arena_ns[1] - warm_cost.arena_ns[1]) / (double)(FRAMES - warm_frames),
            walk_body_ns / (double)(FRAMES - warm_frames));
        vkDestroyEvent(V.dev, gate.gate, NULL);
        if (vk_record_enabled() && vk_record_test_async_submit()) {
            rn = 0; op(OP_SET_RT, &srt, sizeof srt); op(OP_DRAW, &dw, sizeof dw); op(OP_END, NULL, 0);
            lease_owner_probe probe = {.ticket = mtl_last_fence() + 1};
            vk_record_test_hold_submit(probe.ticket);
            pthread_t observer; if (pthread_create(&observer, NULL, lease_owner_probe_run, &probe)) abort();
            mtl_sub_begin(); mtl_exec(rec); rn = 0;
            vk_submit_chunk(); /* no final UBO-publication gate: GPU completion may precede the held owner pin */
            uint64_t ticket = mtl_sub_commit(); pthread_join(observer, NULL);
            CHECK(probe.held && probe.completed && probe.active,
                "GPU-complete secondary leases stay held until the submit owner releases its pin (held=%d complete=%d active=%llu)",
                probe.held, probe.completed, (unsigned long long)probe.active);
            CHECK(mtl_wait(ticket, 5000000000ull), "submit-owner-last lease retirement completes");
        }
        vk_record_test_leases(leases);
        uint64_t final_memory[4]; vk_record_memory(final_memory);
        printf("recorder lease fixture: warm/final handles %llu/%llu, summed worker peak %llu, returned %llu, cold resets %llu\n",
            (unsigned long long)warm_capacity, (unsigned long long)final_memory[2], (unsigned long long)leases[1],
            (unsigned long long)leases[2], (unsigned long long)leases[3]);
        printf("%s: cross-slot leases %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "--rec-job-reuse") || pressure_mode || storage_mode)) {
        CHECK(mtl_wait(run(), 5000000000ull), "job reuse setup completes");
        CHECK(vk_lag_capture_on(), "job reuse fixture requires protected frame lag");
        nvn_dirty_remove_range(cpu_pool);
        nvn_dirty_add_range(cpu_pool, 0x280000); /* Only read-only GPU inputs; lavapipe writes the untracked readback. */
        /* Rebuilding the host-only tracking inventory removed the already-uploaded texture's watch.
           Restore that baseline authority before testing subsequent CPU content changes. */
        nvn_dirty_protect(tx.mem, tx.L.size);
        vk_record_test_native_guards(1);
        const char *async = getenv("TOTK_VK_REC_ASYNC_SUBMIT");
        int expected_async = vk_record_enabled() && (!async || atoi(async) != 0);
        CHECK(vk_record_test_async_submit() == expected_async, "job reuse exercises the requested native submit ownership");
        vk_record_test_delay(2000);
        uint64_t maps_start = 0, maps_warm = 0, anon_start = 0, anon_warm = 0, slab_maps = 0, storage_before[4] = {0}, storage_warm[4] = {0};
        uint64_t primary_warm[2] = {0};
        if (storage_mode) {
            maps_start = recorder_maps(&slab_maps, &anon_start); vk_record_test_storage(storage_before);
            /* Load-like transient: hundreds of one-draw jobs per GPU slot, then many slot reuses
               with smaller passes. This used to retain full128-draw calloc/realloc arrays per job. */
            mtl_sub_begin(); rn = 0;
            op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
            for (int i = 0; i < 4096; i++) {
                int bits = NVN_BARRIER_INVALIDATE_ZCULL_BIT; op(OP_BARRIER, &bits, sizeof bits);
                op(OP_DRAW, &dw, sizeof dw);
            }
            op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
            CHECK(mtl_wait(mtl_sub_commit(), 30000000000ull), "many-small-job storage warmup completes");
        }
        uint64_t serial = vk_submit_serial(), accepted = 0;
        vk_record_test_counts before, after; vk_record_test_counts_get(&before);
        const int frames = storage_mode ? 48 : 24;
        uint64_t cutover_fallbacks = 0;
        for (int frame = 0; frame < frames; frame++) {
            if (pressure_mode) {
                for (int i = 0; i < 64 * 64; i++) lin[4 * i + 1] = frame & 1 ? 50 : 100;
                dm_guest_write_prepare(tx.mem, tx.L.size);
                nvn_swizzle(&tx.d, 0, 0, lin, 256, tx.mem);
            }
            mtl_sub_begin();
            if (pressure_mode && !frame) nvn_dirty_lag_test_vma_limit(1); /* fail guards in an already-started real stream */
            if (pressure_mode && frame) {
                CHECK(!vk_lag_capture_on() && !vk_lag_uniform_private() && !vk_record_lag_ticket(),
                    "pressure frame %d has completed its one-time ordinary-submission cutover", frame);
                const char *chunk_limit = getenv("TOTK_VK_CHUNK_DRAWS");
                long expected_limit = chunk_limit && *chunk_limit ? atol(chunk_limit) : 75;
                long actual_limit = vk_frame_chunk_limit();
                CHECK(actual_limit == expected_limit,
                    "pressure frame %d resolves chunk limit %ld, expected %ld after cutover",
                    frame, actual_limit, expected_limit);
                if (frame == 1) cutover_fallbacks = vk_record_test_lag_fallbacks();
                CHECK(vk_record_test_lag_fallbacks() == cutover_fallbacks,
                    "pressure frame %d does not retain per-slot GPU waits", frame);
            }
            for (int slot = 0; slot < 17; slot++) {
                rn = 0;
                op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
                for (int pass = 0; pass < 7; pass++) {
                    int bits = NVN_BARRIER_INVALIDATE_ZCULL_BIT;
                    op(OP_BARRIER, &bits, sizeof bits);
                    int length = pass == 3 ? 641 : 1 + (frame * 17 + slot * 11 + pass * 5) % 31;
                    for (int draw = 0; draw < length; draw++) {
                        struct { uint64_t dst, n; float value[4]; } update = {
                            u1.addr, 16, {1, 0.5f, 1, 1}};
                        if (!(draw % 13)) op(OP_UPDATE_UB, &update, sizeof update);
                        op(OP_DRAW, &dw, sizeof dw); accepted++;
                    }
                }
                op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
                vk_submit_chunk();
            }
            op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
            CHECK(mtl_wait(mtl_sub_commit(), 30000000000ull), "job reuse frame %d completes", frame);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                const uint8_t *p = readback + 4 * (y * 64 + x);
                int green = pressure_mode ? (frame & 1 ? 25 : 50) : 2 * y;
                bad += y < 32 ? abs(p[0] - 4 * x) > 2 || abs(p[1] - green) > 2 || p[2] != 128 || p[3] != 255 :
                    p[0] || p[1] || p[2] != 255 || p[3] != 255;
            }
            CHECK(!bad, "job reuse frame %d retains all pixels (%d wrong)", frame, bad);
            if (storage_mode && frame == 7) {
                maps_warm = recorder_maps(&slab_maps, &anon_warm);
                vk_record_test_primary_memory(primary_warm);
            }
            if (storage_mode && frame == 31) vk_record_test_storage(storage_warm);
        }
        vk_record_test_counts_get(&after);
        CHECK(after.draws - before.draws == accepted, "job reuse retains all %llu draws", (unsigned long long)accepted);
        CHECK(vk_submit_serial() - serial > (uint64_t)frames * 17, "job reuse wraps eight slots repeatedly within each game submission");
        CHECK(!pressure_mode || (nvn_dirty_lag_vma_pressure() && vk_record_test_lag_fallbacks()),
            "VMA pressure fixture uses the real postqueue fence fallback");
        if (pressure_mode) {
            pressure_return returned = {0};
            VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &returned.gate));
            mtl_sub_begin();
            vkCmdWaitEvents(vk_cb(), 1, &returned.gate, VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, NULL, 0, NULL, 0, NULL);
            pthread_t opener; if (pthread_create(&opener, NULL, pressure_return_run, &returned)) abort();
            vk_submit_chunk();
            atomic_store_explicit(&returned.returned, 1, memory_order_release);
            (void)vk_cb();
            uint64_t gate_ticket = mtl_sub_commit();
            atomic_store_explicit(&returned.returned, 2, memory_order_release);
            pthread_join(opener, NULL);
            CHECK(!returned.timed_out, "ordinary chunk and commit return while the GPU event is still closed after pressure cutover");
            CHECK(mtl_wait(gate_ticket, 5000000000ull), "post-cutover gated ordinary submission completes");
            CHECK(vk_record_test_lag_fallbacks() == cutover_fallbacks,
                "gated ordinary submissions add no pressure fence fallback");
            vkDestroyEvent(V.dev, returned.gate, NULL);
            tex_t cpu_target = rt; cpu_target.mem = cpu_pool + 0x400000; cpu_target.pool = &P_cpu; cpu_target.serial = 20002;
            memset(cpu_target.mem, 0, 65536); nvn_dirty_add_range(cpu_target.mem, 65536);
            p_set_rt target = srt; target.color[0] = &cpu_target;
            p_copy_b2t copy = cp; copy.tex = &cpu_target;
            for (int round = 0; round < 2; round++) {
                op(OP_SET_RT, &target, sizeof target); op(OP_CLEAR_COLOR, &cc, sizeof cc);
                op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &copy, sizeof copy);
                CHECK(mtl_wait(run(), 5000000000ull), "pressure GPU write-back round %d completes", round);
                if (round) lag_guest_store(cpu_target.mem, 0x55);
                mtl_pool_invalidate(&P_cpu, 0x400000, cpu_target.L.size);
                if (!round) {
                    nvn_deswizzle(&cpu_target.d, 0, 0, cpu_target.mem, lin, 256);
                    CHECK(!memcmp(lin, readback, sizeof lin), "exact pending GPU write-back survives pressure and matches all rendered pixels");
                } else CHECK(*(uint8_t *)cpu_target.mem == 0x55, "exact pressure watch preserves a real newer CPU write instead of overwriting it");
            }
        }
        if (storage_mode) {
            uint64_t memory[4], policy[4], named, anonymous, maps = recorder_maps(&named, &anonymous);
            vk_record_memory(memory); vk_record_test_storage(policy);
            uint64_t primary[2]; vk_record_test_primary_memory(primary);
            CHECK(!vk_record_enabled() || (primary[0] < primary_warm[0] && primary[1] > primary_warm[1]),
                "retired load-shaped primary CB high-water is reclaimed (%llu -> %llu, reclaimed %llu)",
                (unsigned long long)primary_warm[0], (unsigned long long)primary[0], (unsigned long long)primary[1]);
            CHECK(maps_warm && maps && maps <= maps_warm + 1024,
                "lag-like job storage keeps steady-state VMAs bounded (%llu -> %llu)", (unsigned long long)maps_warm, (unsigned long long)maps);
            CHECK(anon_warm && anonymous <= anon_warm + 128,
                "many-small-job storage keeps warmed anonymous mappings bounded (%llu -> %llu)", (unsigned long long)anon_warm, (unsigned long long)anonymous);
            vk_record_test_costs capacity; vk_record_test_costs_get(&capacity);
            CHECK(!vk_record_enabled() || memory[0] + memory[1] <= capacity.referenced_bytes + policy[0],
                "small-job idle slab capacity obeys its retention allowance (mapped %llu, referenced %llu, allowance %llu bytes)",
                (unsigned long long)(memory[0] + memory[1]), (unsigned long long)capacity.referenced_bytes,
                (unsigned long long)policy[0]);
            CHECK(!vk_record_enabled() || (policy[1] > storage_before[1] && policy[2] > storage_before[2]),
                "retired load-shaped slot releases over-budget packet/descriptor slabs");
            CHECK(!vk_record_enabled() || policy[3] == storage_before[3], "recent large-slot capacity is not reclaimed before its long-idle grace");
            CHECK(!vk_record_enabled() || storage_warm[1] == policy[1],
                "under-budget warmed capacity is not evicted on every retired slot");
            CHECK(named <= (memory[0] + memory[1]) / (1u << 20),
                "mapped recorder slabs scale by MiB, not job count (%llu named maps)", (unsigned long long)named);
            printf("recorder storage fixture: maps cold/warm/final %llu/%llu/%llu, anon cold/warm/final %llu/%llu/%llu, named slabs %llu, packet/info %.1f/%.1f MiB, budget %llu MiB, reclaimed slots/CBs %llu/%llu\n",
                (unsigned long long)maps_start, (unsigned long long)maps_warm, (unsigned long long)maps,
                (unsigned long long)anon_start, (unsigned long long)anon_warm, (unsigned long long)anonymous, (unsigned long long)named,
                memory[0] / 1048576.0, memory[1] / 1048576.0, (unsigned long long)(policy[0] >> 20),
                (unsigned long long)policy[1], (unsigned long long)policy[3]);
            printf("recorder primary fixture: warm/final %llu/%llu, reclaimed %llu\n",
                (unsigned long long)primary_warm[0], (unsigned long long)primary[0], (unsigned long long)primary[1]);
            /* Real GPU-slot retirements, not a test-adjusted clock. No draws are needed to age a
               formerly large scene; every owner must then run again to maintain its own pool. */
            vk_record_test_delay(0);
            rn = 0; op(OP_END, NULL, 0);
            for (int idle = 0; idle < 4352; idle++) {
                mtl_sub_begin(); mtl_exec(rec);
                CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "cold-tail idle slot %d retires", idle);
            }
            for (int round = 0; round < 32; round++) {
                rn = 0; op(OP_SET_RT, &srt, sizeof srt);
                for (int i = 0; i < 64; i++) {
                    int bits = NVN_BARRIER_INVALIDATE_ZCULL_BIT; op(OP_BARRIER, &bits, sizeof bits);
                    op(OP_DRAW, &dw, sizeof dw);
                }
                op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
                CHECK(mtl_wait(run(), 5000000000ull), "long-idle maintenance round %d preserves GPU completion", round);
            }
            vk_record_test_storage(policy); vk_record_memory(memory);
            CHECK(!vk_record_enabled() || policy[3] > storage_before[3],
                "long-idle excess worker CB capacity is still reclaimed (%llu handles)",
                (unsigned long long)policy[3]);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                uint8_t *p = readback + 4 * (y * 64 + x);
                bad += y < 32 ? abs(p[0] - 4 * x) > 2 || abs(p[1] - 2 * y) > 2 || p[2] != 128 || p[3] != 255 :
                    p[0] || p[1] || p[2] != 255 || p[3] != 255;
            }
            CHECK(!bad, "long-idle pool reclamation preserves every resumed output pixel");
            printf("recorder long-idle fixture: retained %llu secondary handles, reclaimed %llu\n",
                (unsigned long long)memory[2], (unsigned long long)policy[3]);
        }
        { void vk_mem_report(void); vk_mem_report(); }
        printf("%s: job reuse %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    /* TOTK_VK_SPLIT_PASS_DRAWS: one 300-draw pass per submission with overlapping textured stripes (draw order decides
       every column) and a partial in-pass clear between the would-be parts. Fresh processes with the split on and off
       must write identical bytes (vkrectest); pass counts are checked here against the exact split arithmetic. */
    if (argc == 3 && (!strcmp(argv[1], "--split-pass") || !strcmp(argv[1], "--split-depth") ||
            !strcmp(argv[1], "--split-depth-only") || !strcmp(argv[1], "--split-depth-mask"))) {
        int with_depth = strcmp(argv[1], "--split-pass") != 0, depth_only = !strcmp(argv[1], "--split-depth-only");
        int masked = !strcmp(argv[1], "--split-depth-mask");
        const char *safe_env = getenv("TOTK_VK_SPLIT_LRZ_SAFE");
        int safe = !safe_env || !*safe_env || atoi(safe_env) != 0;
        FILE *out = fopen(argv[2], "wb"); if (!out) { perror(argv[2]); return 1; }
        tex_t tx2 = tx; tx2.serial = 10012; tx2.mem = cpu_pool + 0x20000;
        for (int i = 0; i < 64 * 64; i++) { lin[i * 4] = 255 - lin[i * 4]; lin[i * 4 + 1] = 255 - lin[i * 4 + 1]; }
        nvn_swizzle(&tx2.d, 0, 0, lin, 256, tx2.mem); mtl_reg_tex(2, &tx2, NULL); mtl_reg_samp(2, &sp);
        const float full[] = {-1, -1, 3, -1, -1, 3}; memcpy(cpu_pool + 0x202000, full, sizeof full);
        p_bind_vbuf full_vb = vb; full_vb.r[0].address = (uint64_t)(uintptr_t)(cpu_pool + 0x202000);
        fub[0] = 1; fub[1] = 0.5f; fub[2] = 1; fub[3] = 1;
        const char *e = getenv("TOTK_VK_SPLIT_PASS_DRAWS"); long n = e && *e ? atol(e) : 0;
        const int draws = 300, hole_at = 150;
        uint64_t splits = n > 0 ? (uint64_t)(draws - 1) / (uint64_t)n : 0;
        uint64_t candidates = splits;
        if (safe && depth_only) splits = 0;
        if (safe && masked && n > 0) {
            uint64_t first = n < hole_at ? (uint64_t)hole_at : (uint64_t)n;
            splits = first < draws ? 1 + ((uint64_t)draws - 1 - first) / (uint64_t)n : 0;
        }
        tex_t depth = rt; depth.serial = 10013; depth.d.format = 0x34; depth.mem = gpu_pool + 0xf00000;
        nvn_tex_layout(&depth.d, &depth.L);
        p_set_rt target = srt; if (with_depth) target.depth = &depth; if (depth_only) target.n = 0;
        p_copy_b2t depth_copy = cp; depth_copy.tex = &depth; depth_copy.src = (uint64_t)(uintptr_t)(readback + 0x5000);
        p_bind_state depth_state = {.kind = 5};
        ds_st ds = {.dt = with_depth, .dw = with_depth, .df = NVN_DEPTH_FUNC_LESS};
        memcpy(depth_state.bytes, &ds, sizeof ds);
        p_clear_ds depth_clear = {1.0f, 1, 0, 0};
        p_scissor all = {0, 0, 64, 64}, hole = {16, 16, 8, 8};
        p_clear_color red = {0, 0xf, {1, 0, 0, 1}};
        for (int round = 0; round < 12; round++) {
            vk_record_test_counts count0, count1; vk_record_test_counts_get(&count0);
            uint64_t split0[5], split1[5]; vk_draw_test_split_counts(split0);
            float expect_depth[64][64];
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) expect_depth[y][x] = 1;
            uint8_t expect[64][64][4];
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { expect[y][x][0] = expect[y][x][1] = 0; expect[y][x][2] = expect[y][x][3] = 255; }
            rn = 0; op(OP_SCISSOR, &all, sizeof all);
            /* Clear color even for the depth-only refusal arm, then switch before its first draw. */
            if (depth_only) { op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc); }
            op(OP_SET_RT, &target, sizeof target);
            if (!depth_only) op(OP_CLEAR_COLOR, &cc, sizeof cc);
            op(OP_BIND_STATE, &depth_state, sizeof depth_state);
            p_bind_state mask = {.kind = 2}; memset(mask.bytes, masked ? 0 : 255, sizeof(chmask_st));
            op(OP_BIND_STATE, &mask, sizeof mask);
            if (with_depth) op(OP_CLEAR_DS, &depth_clear, sizeof depth_clear);
            op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
            op(OP_BIND_VBUF, &full_vb, sizeof full_vb); op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
            op(OP_VIEWPORTS, &vp, sizeof vp);
            for (int i = 0; i < draws; i++) {
                if (masked && i == hole_at) { memset(mask.bytes, 255, sizeof(chmask_st)); op(OP_BIND_STATE, &mask, sizeof mask); }
                if (i == hole_at && !depth_only) {
                    op(OP_SCISSOR, &hole, sizeof hole); op(OP_CLEAR_COLOR, &red, sizeof red);
                    for (int y = 16; y < 24; y++) for (int x = 16; x < 24; x++) { expect[y][x][0] = 255; expect[y][x][1] = 0; expect[y][x][2] = 0; }
                }
                int which = (i + round) & 1, x0 = (i * 37 + round * 11) & 63;
                p_scissor stripe = {x0, 0, 3, 64};
                p_bind_handles texture = th, sampler = th;
                texture.h[0] = (uint64_t)(1 + which) | ((uint64_t)(1 + which) << 20);
                sampler.h[0] = (uint64_t)(1 + which) << 20;
                op(OP_SCISSOR, &stripe, sizeof stripe);
                op(OP_BIND_TEX, &texture, offsetof(p_bind_handles, h) + sizeof texture.h[0]);
                op(OP_BIND_SAMP, &sampler, offsetof(p_bind_handles, h) + sizeof sampler.h[0]);
                float z = i & 1 ? 0.9f : 0.75f - (float)i / 600.0f;
                if (with_depth) { p_depth_range range = {z, z}; op(OP_DEPTH_RANGE, &range, sizeof range); }
                /* A threshold reached during discard must not end the pass or submit anything. */
                if (n > 0 && i && i % n == 0) {
                    p_flag off = {1}, on = {0};
                    op(OP_RASTER_DISCARD, &off, sizeof off); op(OP_DRAW, &dw, sizeof dw);
                    op(OP_RASTER_DISCARD, &on, sizeof on);
                }
                op(OP_DRAW, &dw, sizeof dw);
                for (int x = x0; x < x0 + 3 && x < 64; x++) for (int y = 0; y < 64; y++) {
                    if (with_depth) {
                        if (z >= expect_depth[y][x]) continue;
                        expect_depth[y][x] = z;
                    }
                    if (depth_only || (masked && i < hole_at)) continue;
                    expect[y][x][0] = (uint8_t)(which ? 255 - 4 * x : 4 * x);
                    expect[y][x][1] = (uint8_t)(which ? (255 - 4 * y) / 2 : 2 * y); expect[y][x][2] = 128;
                }
            }
            op(OP_SCISSOR, &all, sizeof all); op(OP_COPY_T2B, &cp, sizeof cp);
            if (with_depth) op(OP_COPY_T2B, &depth_copy, sizeof depth_copy);
            uint64_t fence0 = mtl_last_fence(), fence = run();
            CHECK(mtl_wait(fence, 5000000000ull), "split round %d completes", round);
            CHECK(fence - fence0 == 1 + splits, "split round %d: %llu GPU submissions, expected one per part (%llu)", round,
                (unsigned long long)(fence - fence0), (unsigned long long)(1 + splits));
            vk_record_test_counts_get(&count1); vk_draw_test_split_counts(split1);
            CHECK(count1.draws - count0.draws == (uint64_t)draws, "split round %d records every draw (%llu)", round,
                (unsigned long long)(count1.draws - count0.draws));
            uint64_t refused = safe && depth_only && candidates ? (uint64_t)(draws - n) : 0;
            if (safe && masked && n > 0 && n < hole_at) refused = (uint64_t)(hole_at - n);
            CHECK(split1[0] - split0[0] == splits && split1[1] - split0[1] == splits && split1[2] - split0[2] == refused,
                "split round %d: %llu splits, %llu reopened parts, %llu refused; expected %llu/%llu", round,
                (unsigned long long)(split1[0] - split0[0]), (unsigned long long)(split1[1] - split0[1]),
                (unsigned long long)(split1[2] - split0[2]), (unsigned long long)splits, (unsigned long long)refused);
            CHECK(split1[3] - split0[3] == (safe && with_depth ? splits : 0) &&
                    split1[4] - split0[4] == refused,
                "split round %d pins LRZ policy: %llu touches, %llu safety refusals", round,
                (unsigned long long)(split1[3] - split0[3]), (unsigned long long)(split1[4] - split0[4]));
            uint64_t passes = 1 + splits + (uint64_t)depth_only;
            CHECK(count1.passes - count0.passes == passes, "split round %d actual passes %llu, expected %llu", round,
                (unsigned long long)(count1.passes - count0.passes), (unsigned long long)passes);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                const uint8_t *p = readback + 4 * (y * 64 + x);
                for (int c = 0; c < 4; c++) bad += abs(p[c] - expect[y][x][c]) > 2;
                if (with_depth) {
                    float z; memcpy(&z, readback + 0x5000 + 4 * (y * 64 + x), 4);
                    bad += z != expect_depth[y][x];
                }
            }
            CHECK(!bad, "split round %d pixels follow draw order across parts and the in-pass clear (%d wrong)", round, bad);
            uint64_t actual = count1.draws - count0.draws;
            CHECK(fwrite(&actual, sizeof actual, 1, out) == 1 && fwrite(readback, 64 * 64 * 4, 1, out) == 1, "write split identity");
            if (with_depth) CHECK(fwrite(readback + 0x5000, 64 * 64 * 4, 1, out) == 1, "write exact split depth identity");
            int crop[] = {0, 0, 0, 0}; mtl_present(&rt, 1.0 / 60, crop);
        }
        const uint64_t footer[] = {0x53504c4954504153ull, 12};
        CHECK(fwrite(footer, sizeof footer, 1, out) == 1 && !fclose(out), "close split identity output");
        printf("%s: %s (TOTK_VK_SPLIT_PASS_DRAWS=%ld LRZ_SAFE=%d) kept %llu/%llu splits per round; %d checks, %d failed\n",
            fails ? "FAILED" : "ok", argv[1], n, safe, (unsigned long long)splits, (unsigned long long)candidates, checks, fails);
        return fails != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--rec-identity")) {
        FILE *out = fopen(argv[2], "wb"); if (!out) { perror(argv[2]); return 1; }
        tex_t tx2 = tx; tx2.serial = 10002; tx2.mem = cpu_pool + 0x20000;
        for (int i = 0; i < 64 * 64; i++) { lin[i * 4] = 255 - lin[i * 4]; lin[i * 4 + 1] = 255 - lin[i * 4 + 1]; }
        nvn_swizzle(&tx2.d, 0, 0, lin, 256, tx2.mem); mtl_reg_tex(2, &tx2, NULL); mtl_reg_samp(2, &sp);
        const float full[] = {-1.25f, -1, 2.75f, -1, -1.25f, 3}, quad[] = {-1.25f, -1, .75f, -1, .75f, 1, -1.25f, 1};
        memcpy(cpu_pool + 0x202000, full, sizeof full); memcpy(cpu_pool + 0x204000, quad, sizeof quad);
        const float delta[] = {.25f, 0, .25f, 0, .25f, 0, .25f, 0};
        memcpy(cpu_pool + 0x205000, delta, sizeof delta); memcpy(cpu_pool + 0x206000, delta, sizeof delta);
        va.n = vs.n = 2; va_st a1 = {0x16, 1, 0}; memcpy(&va.st[1], &a1, sizeof a1); vs.st[1].stride = 8;
        uint16_t indices[] = {0, 1, 2}; memcpy(cpu_pool + 0x203000, indices, sizeof indices);
        const uint8_t indices8[] = {0, 1, 2}; memcpy(cpu_pool + 0x203100, indices8, sizeof indices8);
        p_bind_vbuf triangle_vb = vb, quad_vb = vb;
        triangle_vb.n = quad_vb.n = 2;
        triangle_vb.r[1].size = quad_vb.r[1].size = sizeof delta;
        triangle_vb.r[0].address = (uint64_t)(uintptr_t)(cpu_pool + 0x202000);
        quad_vb.r[0].address = (uint64_t)(uintptr_t)(cpu_pool + 0x204000);
        p_draw_idx indexed = {.prim = 4, .itype = 1, .count = 3, .ibuf = (uint64_t)(uintptr_t)(cpu_pool + 0x203000)};
        p_draw_idx indexed8 = {.prim = 4, .itype = 0, .count = 3, .ibuf = (uint64_t)(uintptr_t)(cpu_pool + 0x203100)};
        p_draw_idx unaligned = indexed; unaligned.ibuf++;
        p_draw quadrilateral = {.prim = 7, .count = 4};
        uint64_t *reports = (uint64_t *)(cpu_pool + 0x320000);
        uint64_t *timestamps = reports + 64;
        const int draws[] = {1, 17, 257, 1793};
        uint64_t walk_wall = 0, body_wall = 0, final_wall = 0, worker_busy = 0, assembly_wait = 0;
        uint64_t walking_wait = 0, body_wait = 0, slot_sum = 0;
        rn = 0;
        for (int round = 0; round < 12; round++) {
#ifdef TOTK_FRAME_TIMELINE_TEST
            totk_ft_record ft_before = {0};
            if (totk_ft_on()) totk_ft_snapshot(totk_ft_current(), &ft_before);
#endif
            uint64_t busy0, busy1, assembly0, assembly1; vk_record_test_timing(&busy0, &assembly0);
            vk_record_test_counts count0, count1; vk_record_test_counts_get(&count0);
            memset(reports, 0, 128 * sizeof *reports);
            for (int pass = 0; pass < 4; pass++) {
                uint64_t samples = NVN_COUNTER_TYPE_SAMPLES_PASSED;
                uint64_t before[] = {samples, (uint64_t)(uintptr_t)(reports + pass * 4)};
                uint64_t after[] = {samples, before[1] + 16};
                uint64_t top[] = {NVN_COUNTER_TYPE_TIMESTAMP_TOP, (uint64_t)(uintptr_t)(timestamps + pass * 4)};
                uint64_t bottom[] = {NVN_COUNTER_TYPE_TIMESTAMP, top[1] + 16};
                p_scissor all = {0, 0, 64, 64}; p_flag enabled = {1};
                op(OP_RENDER_ENABLE, &enabled, sizeof enabled); op(OP_SCISSOR, &all, sizeof all);
                op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
                op(OP_RESET_COUNTER, &samples, sizeof samples); op(OP_REPORT_COUNTER, before, sizeof before);
                op(OP_REPORT_COUNTER, top, sizeof top);
                op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
                op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_VIEWPORTS, &vp, sizeof vp);
                for (int i = 0; i < draws[pass]; i++) {
                    struct { uint64_t dst, n; float tint[4]; } update = {
                        (uint64_t)(uintptr_t)fub, 16, {(float)((i + round) % 7 + 1) / 8, (float)((i + pass) % 5 + 1) / 6, 1, 1}};
                    p_scissor stripe = {(i * 37) & 63, 0, 1, 64};
                    p_bind_handles texture = th, sampler = th;
                    texture.h[0] = (uint64_t)(1 + (i & 1)) | ((uint64_t)(1 + (i & 1)) << 20);
                    sampler.h[0] = (uint64_t)(1 + (i & 1)) << 20;
                    op(OP_UPDATE_UB, &update, sizeof update); op(OP_SCISSOR, &stripe, sizeof stripe);
                    op(OP_BIND_TEX, &texture, offsetof(p_bind_handles, h) + sizeof texture.h[0]);
                    op(OP_BIND_SAMP, &sampler, offsetof(p_bind_handles, h) + sizeof sampler.h[0]);
                    triangle_vb.r[1].address = quad_vb.r[1].address = (uint64_t)(uintptr_t)(cpu_pool + (i % 11 ? 0x205000 : 0x206000));
                    op(OP_BIND_VBUF, i % 17 ? &triangle_vb : &quad_vb, offsetof(p_bind_vbuf, r) + 2 * sizeof vb.r[0]);
                    if (i == 7) op(OP_DRAW_IDX, &unaligned, sizeof unaligned); /* reject without incrementing samples */
                    if (!(i % 17)) op(OP_DRAW, &quadrilateral, sizeof quadrilateral);
                    else if (i & 1) op(OP_DRAW_IDX, i % 3 ? &indexed : &indexed8, sizeof indexed);
                    else op(OP_DRAW, &dw, sizeof dw);
                }
                op(OP_REPORT_COUNTER, after, sizeof after);
                /* Pending counter values, not stale memory, must skip this destructive clear. */
                p_render_cond equal = {0, 0, before[1]};
                op(OP_RENDER_COND, &equal, sizeof equal); op(OP_SCISSOR, &all, sizeof all); op(OP_CLEAR_COLOR, &cc, sizeof cc);
                op(OP_RENDER_ENABLE, &enabled, sizeof enabled);
                op(OP_REPORT_COUNTER, bottom, sizeof bottom);
                p_copy_b2t copy = cp; copy.src += (uint64_t)pass * 64 * 64 * 4; op(OP_COPY_T2B, &copy, sizeof copy);
            }
            op(OP_END, NULL, 0);
            struct { op_hdr call; const uint8_t *target; op_hdr end; } nested = {{OP_CALL, 16}, rec, {OP_END, 8}};
            struct { op_hdr jump; const uint8_t *target; } entry = {{OP_JUMP, 16}, (const uint8_t *)&nested};
            uint64_t recording = (uint64_t)(uintptr_t)&entry; /* snapshot collection and replay follow identical CALL/JUMP paths */
            uint64_t wait0 = vk_record_test_walk_wait(), slot0 = mtl_last_fence();
            uint64_t walk_start = host_now_ns();
            mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&recording, 1, NULL);
            fub[0] = -100; /* packets must use submission snapshots plus each draw's inline update */
            mtl_exec((const uint8_t *)&entry); rn = 0; uint64_t body_end = host_now_ns();
            uint64_t wait_body = vk_record_test_walk_wait();
            uint64_t fence = mtl_sub_commit(), walk_end = host_now_ns();
            vk_record_test_timing(&busy1, &assembly1);
            walk_wall += walk_end - walk_start; body_wall += body_end - walk_start; final_wall += walk_end - body_end;
            worker_busy += busy1 - busy0; assembly_wait += assembly1 - assembly0;
            walking_wait += vk_record_test_walk_wait() - wait0; body_wait += wait_body - wait0; slot_sum += fence - slot0;
            CHECK(mtl_wait(fence, 5000000000ull), "identity round %d completes", round);
            vk_record_test_counts_get(&count1);
#ifdef TOTK_FRAME_TIMELINE_TEST
            if (totk_ft_on()) {
                totk_ft_record record;
                CHECK(totk_ft_snapshot(totk_ft_current(), &record), "timeline retains identity walk frame");
                CHECK(record.work.value[FT_COUNT_DRAWS] - ft_before.work.value[FT_COUNT_DRAWS] == 2068,
                      "timeline actual worker/inline draws match independently checked stream count");
                CHECK(record.work.value[FT_COUNT_REC_SAMPLES] > ft_before.work.value[FT_COUNT_REC_SAMPLES] &&
                      record.work.value[FT_COUNT_REC_P_NS] > ft_before.work.value[FT_COUNT_REC_P_NS],
                      "timeline opcode sampling captures parallelizable work without requiring full profiler");
                printf("frame-tl identity: frame=%llu samples=%llu S/P/O_ns=%llu/%llu/%llu draws=%llu cpu_steps=%llu\n",
                    (unsigned long long)record.frame, (unsigned long long)record.work.value[FT_COUNT_REC_SAMPLES],
                    (unsigned long long)record.work.value[FT_COUNT_REC_S_NS], (unsigned long long)record.work.value[FT_COUNT_REC_P_NS],
                    (unsigned long long)record.work.value[FT_COUNT_REC_O_NS], (unsigned long long)record.work.value[FT_COUNT_DRAWS],
                    (unsigned long long)record.work.value[FT_COUNT_CPU_STEPS]);
            }
#endif
            CHECK(count1.draws - count0.draws == 2068 && count1.index_skips - count0.index_skips == 3,
                "actual draw and rejected-index counts are stream ordered");
            uint64_t totals[] = {count1.draws - count0.draws, count1.passes - count0.passes, count1.index_skips - count0.index_skips};
            CHECK(fwrite(totals, sizeof totals, 1, out) == 1, "write actual identity draw/pass/skip counts");
            CHECK(!getenv("TOTK_VK_TEST_WIDEN_U8") || count1.widened > count0.widened, "forced uint8 widening executes");
            for (int pass = 0; pass < 4; pass++) {
                uint64_t expected = (uint64_t)draws[pass] * 3 + (uint64_t)(draws[pass] + 16) / 17;
                CHECK(reports[pass * 4] == 0 && reports[pass * 4 + 2] == expected,
                    "identity round %d pass %d samples %llu, expected %llu", round, pass,
                    (unsigned long long)reports[pass * 4 + 2], (unsigned long long)expected);
                uint64_t ordered = timestamps[pass * 4 + 1] != 0 && timestamps[pass * 4 + 3] >= timestamps[pass * 4 + 1]
                    && (!pass || timestamps[pass * 4 + 1] >= timestamps[(pass - 1) * 4 + 3]);
                CHECK(ordered, "identity round %d pass %d timestamp values are nonzero and ordered", round, pass);
                CHECK(fwrite(readback + pass * 64 * 64 * 4, 64 * 64 * 4, 1, out) == 1, "write identity pixels");
                uint64_t counts[] = {reports[pass * 4], reports[pass * 4 + 2], ordered};
                CHECK(fwrite(counts, sizeof counts, 1, out) == 1, "write identity counters");
            }
            int crop[] = {0, 0, 0, 0};
            for (int present = 0; present < 25; present++) mtl_present(&rt, 1.0 / 60, crop);
        }
        const uint64_t footer[] = {0x5245434944454e33ull, 12};
        CHECK(fwrite(footer, sizeof footer, 1, out) == 1, "write identity footer");
        CHECK(!fclose(out), "close identity output");
        printf("recorder identity timing ms/NVN-submit: walk wall %.3f body %.3f final %.3f summed worker busy %.3f background assembly wait %.3f walking wait %.3f body wait %.3f slots %.2f\n",
            walk_wall / 12e6, body_wall / 12e6, final_wall / 12e6, worker_busy / 12e6, assembly_wait / 12e6,
            walking_wait / 12e6, body_wait / 12e6, slot_sum / 12.0);
        printf("%s: identity %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "--rec-boundaries") || submit_mode)) {
        FILE *out = fopen(argv[2], "wb"); if (!out) { perror(argv[2]); return 1; }
        const float full[] = {-1, -1, 3, -1, -1, 3}, away[] = {20, 20, 24, 20, 20, 24};
        memcpy(cpu_pool + 0x202000, full, sizeof full); memcpy(cpu_pool + 0x205000, away, sizeof away);
        p_bind_vbuf full_vb = vb; full_vb.r[0].address = (uint64_t)(uintptr_t)(cpu_pool + 0x202000);
        fub[0] = fub[1] = fub[2] = fub[3] = 1;
        mtl_reg_tex(2, &tx, NULL);
        p_scissor all = {0, 0, 64, 64}, left = {0, 0, 32, 64}, top = {32, 0, 32, 32},
            bottom = {32, 32, 32, 32}, corner = {48, 48, 16, 16};
        p_clear_color red = {0, 0xf, {1, 0, 0, 1}}, green = {0, 0xf, {0, 1, 0, 1}};
        p_set_rt none = {0}; uint64_t samples = NVN_COUNTER_TYPE_SAMPLES_PASSED;
        uint64_t *reports = (uint64_t *)(cpu_pool + 0x320000); memset(reports, 0, 128);
        vk_record_test_counts count0, count1; vk_record_test_counts_get(&count0);
        rn = 0; op(OP_SCISSOR, &all, sizeof all); op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
        op(OP_SET_RT, &none, sizeof none); /* commit the clear-only pass */
        op(OP_SET_RT, &srt, sizeof srt); op(OP_SCISSOR, &left, sizeof left);
        op(OP_CLEAR_COLOR, &red, sizeof red); /* partial clear opens a pass, not a postponed loadOp */
        op(OP_RESET_COUNTER, &samples, sizeof samples);
        uint64_t report[] = {samples, (uint64_t)(uintptr_t)reports}; op(OP_REPORT_COUNTER, report, sizeof report);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
        op(OP_BIND_VBUF, &full_vb, sizeof full_vb); op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
        op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp); op(OP_SCISSOR, &top, sizeof top);
        for (int i = 0; i < 33; i++) {
            struct { uint64_t dst, n; uint32_t values[4]; } unrelated = {
                (uint64_t)(uintptr_t)(cpu_pool + 0x210000), 16, {(uint32_t)i, 0, 0, 0}};
            p_bind_buf unused = {1, 15, unrelated.dst + (uint64_t)(i & 1) * 16, 16};
            p_bind_handles unused_texture = th; unused_texture.first = 127; unused_texture.h[0] = (uint64_t)(1 + (i & 1));
            op(OP_UPDATE_UB, &unrelated, sizeof unrelated); op(OP_BIND_UBO, &unused, sizeof unused);
            op(OP_BIND_TEX, &unused_texture, offsetof(p_bind_handles, h) + sizeof unused_texture.h[0]);
            op(OP_DRAW, &dw, sizeof dw);
            if (i == 31) {
                op(OP_END, NULL, 0); if (!submit_mode) vk_record_test_pause_draws(renaming ? 32 : 1);
                mtl_sub_begin(); mtl_exec(rec); rn = 0;
                CHECK(submit_mode || vk_record_test_wait_paused(), "an eligible recorder chunk reaches its worker during the first 32 draws");
                vk_record_test_release();
            }
        }
        report[1] += 16; op(OP_REPORT_COUNTER, report, sizeof report);
        op(OP_SCISSOR, &bottom, sizeof bottom); op(OP_CLEAR_COLOR, &green, sizeof green);
        report[1] += 16; op(OP_REPORT_COUNTER, report, sizeof report);
        op(OP_SCISSOR, &corner, sizeof corner); op(OP_DRAW, &dw, sizeof dw);
        report[1] += 16; op(OP_REPORT_COUNTER, report, sizeof report); op(OP_COPY_T2B, &cp, sizeof cp);
        op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "partial-open and mid-pass clears complete");
        vk_record_test_counts_get(&count1);
        CHECK(!vk_record_enabled() || (( !renaming || count1.freeze_hits - count0.freeze_hits >= 60) &&
            count1.program_hits - count0.program_hits >= 30),
            "semantic program reuse survives unrelated bindings; renaming also retains same-pass frozen descriptors");
        CHECK(reports[0] == 0 && reports[2] == 99 && reports[4] == 99 && reports[6] == 102,
            "executed mid-pass clear stays between ordered sample reports");
        uint64_t expected_passes = renaming || vk_lag_capture_on() ? 2u : 35u;
        CHECK(count1.passes - count0.passes == expected_passes && count1.draws - count0.draws == 34,
            "clear/draw passes preserve direct boundaries or private uniform updates: passes %llu, expected %llu",
            (unsigned long long)(count1.passes - count0.passes), (unsigned long long)expected_passes);
        int bad = 0;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            const uint8_t *p = readback + 4 * (y * 64 + x);
            int textured = x >= 32 && (y < 32 || (x >= 48 && y >= 48));
            int r = textured ? 4 * x : x < 32 ? 255 : 0;
            int g0 = textured ? 4 * y : x < 32 ? 0 : 255, b0 = textured ? 128 : 0;
            bad += abs(p[0] - r) > 2 || abs(p[1] - g0) > 2 || abs(p[2] - b0) > 2 || p[3] != 255;
        }
        CHECK(!bad, "partial-open and mid-pass clears retain exact regions (%d wrong)", bad);
        uint64_t counts[] = {count1.draws - count0.draws, count1.passes - count0.passes, reports[0], reports[2], reports[4], reports[6]};
        CHECK(fwrite(counts, sizeof counts, 1, out) == 1 && fwrite(readback, 64 * 64 * 4, 1, out) == 1, "write clear identity");
        void *alias = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(alias != MAP_FAILED, "reserve delayed-worker virtual address");
        if (alias == MAP_FAILED) return 1;
        uint64_t donor[] = {(uint64_t)(uintptr_t)(cpu_pool + 0x202000), (uint64_t)(uintptr_t)(cpu_pool + 0x205000)};
        VkBuffer buffers[2]; uint64_t offsets[2];
        for (int i = 0; i < 2; i++) CHECK(vk_resolve_for(donor[i], 64, &buffers[i], &offsets[i], NULL, VK_LOOKUP_VERTEX), "resolve donor %d", i);
        p_bind_vbuf aliased_vb = full_vb; aliased_vb.r[0].address = (uint64_t)(uintptr_t)alias;
        for (int round = 0; round < 12; round++) { /* wrap all eight ring slots */
            int overlap = submit_mode && vk_record_test_async_submit();
            uint64_t before_seal = 0;
            vk_record_test_counts_get(&count0); vk_record_test_pause(); alarm(30);
            for (int version = 0; version < 2; version++) {
                if (version && submit_mode && !overlap) vk_record_test_release();
                vk_record_test_alias(alias, donor[version], 64);
                vk_record_test_expect_vertex((uint64_t)(uintptr_t)alias, buffers[version], offsets[version]);
                rn = 0; op(OP_SET_RT, &srt, sizeof srt); op(OP_SCISSOR, &all, sizeof all); op(OP_CLEAR_COLOR, &cc, sizeof cc);
                op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
                op(OP_BIND_VBUF, &aliased_vb, sizeof aliased_vb); op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
                op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp); op(OP_DRAW, &dw, sizeof dw);
                p_copy_b2t copy = cp; copy.src += (uint64_t)version * 64 * 64 * 4; op(OP_COPY_T2B, &copy, sizeof copy); op(OP_END, NULL, 0);
                uint64_t recording = (uint64_t)(uintptr_t)rec;
                if (!version) { mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&recording, 1, NULL); }
                mtl_exec(rec); rn = 0; /* cannot join the deliberately paused worker at a pass boundary */
                if (!version) {
                    vk_record_test_flush_jobs();
                    CHECK(vk_record_test_wait_paused(), "round %d reaches worker gate before span replacement", round);
                    before_seal = mtl_last_fence();
                }
                if (version && overlap) {
                    uint64_t reserved, queued; vk_record_test_submit_status(&reserved, &queued);
                    CHECK(reserved > before_seal && queued < reserved,
                        "walking records next slot while earlier sealed slot is still blocked on its worker");
                }
            }
            vk_record_test_release(); uint64_t fence = mtl_sub_commit(); alarm(0);
            CHECK(mtl_wait(fence, 5000000000ull), "delayed-worker round %d completes", round);
            uint64_t resolved, wrong, resets; vk_record_test_results(&resolved, &wrong, &resets);
            CHECK(!vk_record_enabled() || (resolved == 2 && !wrong && resets >= 1),
                "round %d resolves frozen buffer/offset versions (%llu checks, %llu wrong, %llu resets)",
                round, (unsigned long long)resolved, (unsigned long long)wrong, (unsigned long long)resets);
            vk_record_test_counts_get(&count1);
            /* The forced one-draw GPU boundary materializes the second pass's pending load clear separately
               from its draw pass: one extra clear-only pass in this stream, identically in 0/2/3 modes. */
            uint64_t pair_passes = submit_mode ? 3 : 2;
            CHECK(count1.draws - count0.draws == 2 && count1.passes - count0.passes == pair_passes,
                "delayed-worker actual pass count %llu, expected %llu", (unsigned long long)(count1.passes - count0.passes), (unsigned long long)pair_passes);
            bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                const uint8_t *a = readback + 4 * (y * 64 + x), *b = a + 64 * 64 * 4;
                bad += abs(a[0] - 4 * x) > 2 || abs(a[1] - 4 * y) > 2 || a[2] != 128 || a[3] != 255;
                bad += b[0] || b[1] || b[2] != 255 || b[3] != 255;
            }
            CHECK(!bad, "round %d old alias draws and replacement is offscreen (%d wrong)", round, bad);
            uint64_t actual[] = {count1.draws - count0.draws, count1.passes - count0.passes};
            CHECK(fwrite(actual, sizeof actual, 1, out) == 1 && fwrite(readback, 2 * 64 * 64 * 4, 1, out) == 1, "write frozen span identity");
        }
        const uint64_t footer[] = {0x524543424e445933ull, 12};
        CHECK(fwrite(footer, sizeof footer, 1, out) == 1 && !fclose(out), "close boundary identity output");
        printf("%s: boundaries %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    /* Run this fixture in fresh processes with explicit TOTK_FRAME_LAG=1,
       default-off renaming (also OFF=rename), and REC=0/2/3. Gates prove the CPU reuses the SAME updated UBO
       before its queued draw executes, not after a wait. */
    if (argc == 2 && !strcmp(argv[1], "--lag-uniform")) {
        CHECK(vk_lag_capture_on() && !renaming, "private uniform fixture requires protected lag and rename OFF");
        if (!vk_lag_capture_on() || renaming) return 1;
        rn = 0;
        float *uniform = (float *)(cpu_pool + 0x280000), *vertices = (float *)(cpu_pool + 0x290000);
        const float full[] = {-1, -1, 3, -1, -1, 3}; memcpy(vertices, full, sizeof full);
        p_bind_vbuf full_vb = vb; full_vb.r[0].address = (uint64_t)(uintptr_t)vertices; full_vb.r[0].size = sizeof full;
        p_bind_buf tint = u1; tint.addr = (uint64_t)(uintptr_t)uniform;
        p_scissor all = {0, 0, 64, 64};
        op(OP_SET_RT, &srt, sizeof srt); op(OP_SCISSOR, &all, sizeof all); op(OP_CLEAR_COLOR, &cc, sizeof cc);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
        op(OP_BIND_VBUF, &full_vb, sizeof full_vb); op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &tint, sizeof tint);
        op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp); op(OP_END, NULL, 0);
        uint8_t setup[4096]; CHECK(rn <= sizeof setup, "uniform setup fits its recording");
        if (rn > sizeof setup) return 1;
        memcpy(setup, rec, rn); rn = 0;
        uint64_t handle = (uint64_t)(uintptr_t)setup;
        VkDeviceSize alignment = V.props.limits.minUniformBufferOffsetAlignment;
        if (!alignment) alignment = 256;
        VkBuffer native = VK_NULL_HANDLE; uint64_t native_offset = 0;
        CHECK(vk_resolve(tint.addr, 256, &native, &native_offset), "uniform original range resolves");
        if (!native) return 1;
        uint64_t (*reports)[2] = (uint64_t (*)[2])(cpu_pool + 0x2c0000); memset(reports, 0, 12 * sizeof *reports);
        uint64_t faults0, hosts0, ns, faults1, hosts1;
        nvn_dirty_lag_stats(&faults0, &hosts0, &ns);
        for (int frame = 0; frame < 12; frame++) {
            for (int i = 0; i < 64; i++) uniform[i] = i + 1;
            uniform[0] = uniform[2] = uniform[3] = 1; uniform[1] = 0.5f;
            struct { uint64_t dst, n; float v[4]; } update = {tint.addr, 16, {0.25f * (frame % 3 + 1), 0.5f, 1, 1}};
            struct { uint64_t dst, n; float v; uint32_t pad; } partial = {tint.addr + 4, 4, 0.75f, 0};
            VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
            mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL);
            vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
            mtl_exec(setup);
            op(OP_UPDATE_UB, &update, sizeof update); op(OP_UPDATE_UB, &partial, sizeof partial);
            op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
            mtl_exec(rec); rn = 0;
            mtl_sub_counter((uint64_t)(uintptr_t)reports[frame], NVN_COUNTER_TYPE_TIMESTAMP_TOP);
            CHECK(uniform[0] == 1 && uniform[1] == 0.5f, "frame %d private updates do not mutate original bytes while walking", frame);
            float logical[4]; vk_ub_read(tint.addr, logical, sizeof logical);
            CHECK(logical[0] == update.v[0] && logical[1] == partial.v, "frame %d ordered full/partial CPU overlay", frame);
            VkDescriptorBufferInfo a = {native, native_offset, 256}, b = {0};
            CHECK(vk_lag_uniform_read(tint.addr, 256, alignment, &a), "frame %d captures private UBO", frame);
            CHECK(a.range == 256 && !(a.offset % alignment) && (a.buffer != native || a.offset != native_offset),
                "frame %d private descriptor has exact extent, alignment and non-native destination", frame);
            CHECK(vk_lag_uniform_read(tint.addr, 256, alignment, &b) && a.buffer == b.buffer && a.offset == b.offset && b.range == a.range,
                "frame %d unchanged descriptor memo is immutable", frame);
            alarm(30); uint64_t fence = mtl_sub_commit(); alarm(0);
            CHECK(uniform[0] == update.v[0] && uniform[1] == partial.v && uniform[4] == 5,
                "frame %d CPU-known publication preserves partial-update neighbours before submit returns", frame);
            int waited = lag_test_rewrite(uniform, 4, 0, frame & 1, gate);
            uniform[0] = 0.875f; uniform[32] = 100 + frame; /* exact CPU writes after publication must stay newer */
            CHECK(!waited, "frame %d rewrites its UBO BEFORE gated execution without a reuse wait", frame);
            CHECK(mtl_wait(fence, 5000000000ull), "frame %d gated uniform draw completes", frame);
            CHECK(reports[frame][1] != 0, "frame %d completion report publishes after CPU-known UBO bytes", frame);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                const uint8_t *p = readback + 4 * (y * 64 + x);
                bad += abs(p[0] - (int)(4 * x * update.v[0])) > 2 || abs(p[1] - 3 * y) > 2 || p[2] != 128 || p[3] != 255;
            }
            CHECK(!bad, "frame %d immutable full/partial uniforms retain exact pixels (%d wrong)", frame, bad);
            CHECK(uniform[0] == 0.875f && uniform[1] == partial.v && uniform[4] == 5 && uniform[32] == 100 + frame,
                "frame %d no queued original UBO write clobbers newer CPU bytes or neighbours", frame);
            vkDestroyEvent(V.dev, gate, NULL);
        }
        nvn_dirty_lag_stats(&faults1, &hosts1, &ns);
        CHECK(faults1 == faults0 && hosts1 == hosts0, "pure UBO reuse adds zero fault/host waits");

        /* More than eight ACTUAL Vulkan seals inside one game submission: retain an early immutable descriptor
           until after the complete ring has recycled, then copy its exact extent and compare every byte. */
        for (int i = 0; i < 64; i++) uniform[i] = i + 1;
        uniform[0] = uniform[2] = uniform[3] = 1; uniform[1] = 0.5f;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL); mtl_exec(setup);
        struct { uint64_t dst, n; float v[4]; } first = {tint.addr, 16, {0.25f, 0.5f, 1, 1}};
        op(OP_UPDATE_UB, &first, sizeof first); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        VkDescriptorBufferInfo oldest = {native, native_offset, 256};
        CHECK(vk_lag_uniform_read(tint.addr, 256, alignment, &oldest), "retain first private descriptor across ring wrap");
        uint64_t serial0 = vk_submit_serial();
        for (int seal = 0; seal < 12; seal++) {
            struct { uint64_t dst, n; float v; uint32_t pad; } partial = {tint.addr + 4, 4, (seal & 1) ? 0.75f : 0.25f, 0};
            op(OP_UPDATE_UB, &partial, sizeof partial); op(OP_DRAW, &dw, sizeof dw); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
            VkDescriptorBufferInfo current = {0};
            CHECK(vk_lag_uniform_read(tint.addr, 256, alignment, &current) &&
                (current.buffer != oldest.buffer || current.offset != oldest.offset),
                "seal %d newer partial update gets new immutable backing", seal);
            vk_submit_chunk();
            CHECK(mtl_wait(mtl_last_fence(), 5000000000ull), "actual uniform seal %d completes", seal);
            float logical[4]; vk_ub_read(tint.addr, logical, sizeof logical);
            CHECK(logical[0] == first.v[0] && logical[1] == partial.v, "seal %d retains newest sequence overlay independently of fences", seal);
        }
        CHECK(vk_submit_serial() >= serial0 + 12, "uniform stress seals more than the eight-slot ring");
        uint64_t copy_addr = (uint64_t)(uintptr_t)(readback + 0x10000), copy_offset;
        VkBuffer copy_buffer; CHECK(vk_resolve(copy_addr, oldest.range, &copy_buffer, &copy_offset), "private-copy readback extent resolves");
        vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_B2B); vk_gpu_write(copy_addr, oldest.range);
        VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
        VkBufferCopy bc = {oldest.offset, copy_offset, oldest.range}; vkCmdCopyBuffer(cb, oldest.buffer, copy_buffer, 1, &bc);
        vk_barrier_all(cb);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "old private descriptor copy completes after ring wrap");
        float *copied = (float *)(readback + 0x10000); int bad_copy = 0;
        for (int i = 0; i < 64; i++) bad_copy += copied[i] != (i < 4 ? first.v[i] : (float)(i + 1));
        CHECK(oldest.range == 256 && !bad_copy, "old arena copy stays immutable, exact-sized and byte-correct after twelve seals");

        /* A real virtual-pool non-UBO copy must observe ordered materialization, not stale original guest bytes. */
        void *alias = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(alias != MAP_FAILED, "reserve uniform non-UBO alias");
        if (alias == MAP_FAILED) return 1;
        vk_record_test_alias(alias, tint.addr, 256);
        first.v[0] = 0.5f; first.v[1] = 0.25f;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL); mtl_exec(setup);
        op(OP_UPDATE_UB, &first, sizeof first); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        mtl_sub_copy((uint64_t)(uintptr_t)alias, copy_addr, 16);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "non-UBO alias materialization completes");
        CHECK(!memcmp((void *)(uintptr_t)copy_addr, first.v, 16), "virtual non-UBO alias sees newest logical UBO value");

        /* A genuine GPU-copy producer supersedes older private bytes. Its subsequent UBO alias stays live,
           including partial-copy retirement: untouched red/blue/alpha must survive the green replacement. */
        float *producer = (float *)(cpu_pool + 0x2b0000); producer[0] = 0.75f;
        uint64_t produced = (uint64_t)(uintptr_t)(gpu_pool + 0x40000);
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL); mtl_exec(setup);
        op(OP_UPDATE_UB, &first, sizeof first); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        mtl_sub_copy((uint64_t)(uintptr_t)producer, produced, 4);
        mtl_sub_copy(produced, tint.addr + 4, 4);
        VkDescriptorBufferInfo live;
        CHECK(!vk_lag_uniform_read(tint.addr, 256, alignment, &live), "GPU-copy-produced uniform alias remains live");
        op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU producer then uniform alias completes");
        const uint8_t *pixel = readback + 4 * (10 * 64 + 10);
        CHECK(abs(pixel[0] - 20) <= 2 && abs(pixel[1] - 30) <= 2 && pixel[2] == 128,
            "partial true producer preserves untouched uniform shadow neighbours (%u,%u,%u)", pixel[0], pixel[1], pixel[2]);
        CHECK(uniform[0] == 0.5f && uniform[1] == 0.75f && uniform[2] == 1 && uniform[3] == 1,
            "partial producer retires exactly its bytes and leaves correct materialized neighbours");

        /* Writable storage aliases are real producers, not readonly UBO captures. A writer after byte16 leaves
           tint intact but overlaps the descriptor's reflected range, forcing ordered original-memory binding. */
        uint8_t wctl[0x880], wcode[0x230]; uint32_t wlen;
        shader_binary(1, 4, wctl, wcode, &wlen); memcpy(cpu_pool + 0x103000, wcode, wlen);
        prog_t writable = pg; writable.serial = 1901; writable.sd[1].control = wctl;
        writable.sd[1].data = (uint64_t)(uintptr_t)(cpu_pool + 0x103000);
        p_bind_program wbp = {&writable, 3}; p_bind_buf output = {1, 0, tint.addr + 16, 64 * 64 * 4};
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL); mtl_exec(setup);
        op(OP_UPDATE_UB, &first, sizeof first); op(OP_BIND_PROGRAM, &wbp, sizeof wbp); op(OP_BIND_SSBO, &output, sizeof output);
        op(OP_DRAW, &dw, sizeof dw); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
        CHECK(!vk_lag_uniform_read(tint.addr, 256, alignment, &live), "writable-storage overlapping UBO stays live guarded");
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
        mtl_exec(rec); rn = 0;
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "writable alias then uniform draw completes");
        pixel = readback + 4 * (10 * 64 + 10);
        CHECK(abs(pixel[0] - 20) <= 2 && abs(pixel[1] - 10) <= 2 && pixel[2] == 128,
            "writable alias materializes untouched logical tint before live uniform read (%u,%u,%u)", pixel[0], pixel[1], pixel[2]);
        CHECK(((uint32_t *)(uniform + 4))[10 * 64 + 10] == 0x89abcde1u, "actual writable storage producer executes");
        printf("%s: lag uniform %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
        return fails != 0;
    }
    uint64_t f = run();
    CHECK(mtl_wait(f, 5000000000ull), "the draw submission completes");
    int bad_top = 0, bad_bottom = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const uint8_t *p = readback + (y * 64 + x) * 4;
        if (y < 32) { int er = abs(p[0] - 4 * x), eg = abs(p[1] - 2 * y), eb = abs(p[2] - 128), ea = abs(p[3] - 255); if (er > 2 || eg > 2 || eb > 2 || ea > 2) { if (!bad_top++) printf("  top (%d,%d) = %d %d %d %d, want %d %d 128 255\n", x, y, p[0], p[1], p[2], p[3], 4 * x, 2 * y); } }
        else if (p[0] || p[1] || p[2] != 255 || p[3] != 255) { if (!bad_bottom++) printf("  bottom (%d,%d) = %d %d %d %d, want the clear 0 0 255 255\n", x, y, p[0], p[1], p[2], p[3]); }
    }
    CHECK(!bad_top, "textured upper half (%d wrong pixels): orientation, texture upload, sampler, uniforms", bad_top);
    CHECK(!bad_bottom, "cleared lower half (%d wrong pixels): clear and Y orientation", bad_bottom);
    /* Compare barrier-coalesced rendering to the original exact pixels. Also retain a real
       storage writer and split before a writer introduced after a deferred read-only barrier. */
    {
        uint8_t reference[64 * 64 * 4]; memcpy(reference, readback, sizeof reference);
        uint64_t before[4], after[4]; vk_pass_merge_counters(before);
        int bits = NVN_BARRIER_ORDER_FRAGMENTS_BIT;
        op(OP_CLEAR_COLOR, &cc, sizeof cc); op(OP_DRAW, &dw, sizeof dw);
        op(OP_BARRIER, &bits, sizeof bits); op(OP_DRAW, &dw, sizeof dw);
        bits = NVN_BARRIER_INVALIDATE_TEXTURE_BIT;
        op(OP_BARRIER, &bits, sizeof bits); op(OP_DRAW, &dw, sizeof dw);
        bits = NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT;
        op(OP_BARRIER, &bits, sizeof bits); op(OP_DRAW, &dw, sizeof dw);
        op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        CHECK(!memcmp(reference, readback, sizeof reference), "read-only barrier merging preserves all 16384 output bytes");
        vk_pass_merge_counters(after);
        const char *me = getenv("TOTK_VK_PASS_MERGE");
        const char *se = getenv("TOTK_VK_STORE_CLEAR");
        int merging = me && !strcmp(me, "1") && !vk_off("passbarrier") && !(se && atoi(se) == 1) && !getenv("TOTK_VK_PASSPROBE");
        CHECK(!merging || (after[0] - before[0] == 3 && after[3] - before[3] == 3),
              "three read-only barriers avoid three pass boundaries");

        uint8_t sctl[0x880], scode[0x230]; uint32_t slen;
        shader_binary(1, 4, sctl, scode, &slen); memcpy(cpu_pool + 0x103000, scode, slen);
        prog_t storage = pg; storage.serial = 901; storage.sd[1].control = sctl;
        storage.sd[1].data = (NVNbufferAddress)(uintptr_t)(cpu_pool + 0x103000);
        p_bind_program sbp = {&storage, 3};
        uint32_t *out = (uint32_t *)(cpu_pool + 0x600000); memset(out, 0, 64 * 64 * 4);
        p_bind_buf output = {1, 0, (uint64_t)(uintptr_t)out, 64 * 64 * 4};
        vk_pass_merge_counters(before);
        op(OP_CLEAR_COLOR, &cc, sizeof cc); op(OP_DRAW, &dw, sizeof dw);
        bits = NVN_BARRIER_ORDER_FRAGMENTS_BIT; op(OP_BARRIER, &bits, sizeof bits);
        op(OP_DRAW, &dw, sizeof dw); op(OP_BARRIER, &bits, sizeof bits);
        op(OP_BIND_PROGRAM, &sbp, sizeof sbp); op(OP_BIND_SSBO, &output, sizeof output); op(OP_DRAW, &dw, sizeof dw);
        op(OP_BARRIER, &bits, sizeof bits);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_DRAW, &dw, sizeof dw);
        op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        CHECK(!memcmp(reference, readback, sizeof reference), "storage dependency preservation keeps exact output pixels");
        int written = 1; for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
            if (out[y * 64 + x] != (y < 32 ? 0x89abcde1u : 0)) written = 0;
        CHECK(written, "fragment storage writer executes and writes only covered pixels");
        vk_pass_merge_counters(after);
        CHECK(!merging || (after[2] - before[2] == 1 && after[1] - before[1] == 1 && after[3] - before[3] == 1),
              "late storage writer forces one split and its following barrier is retained");
        op(OP_CLEAR_COLOR, &cc, sizeof cc); op(OP_DRAW, &dw, sizeof dw);
        bits = NVN_BARRIER_INVALIDATE_ZCULL_BIT; op(OP_BARRIER, &bits, sizeof bits);
        op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        CHECK(!memcmp(reference, readback, sizeof reference), "non-deferrable ZCULL barrier preserves exact pixels");
    }

    /* the texture changes in guest memory: re-uploaded at its next use in a later submission */
    for (int i = 0; i < 64 * 64; i++) { lin[4 * i] = 200; lin[4 * i + 1] = 100; }
    nvn_swizzle(&tx.d, 0, 0, lin, 256, tx.mem);
    op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb, sizeof vb);
    op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
    op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp);
    f = run(); mtl_wait(f, 5000000000ull);
    const uint8_t *p = readback + (5 * 64 + 7) * 4;
    CHECK(abs(p[0] - 200) <= 2 && abs(p[1] - 50) <= 2, "re-upload after a guest write: (7,5) = %d %d, want 200 50", p[0], p[1]);

    /* a uniform update between two draws of one pass: the first draw sees the old bytes, the second the new ones,
       and memory has the new ones after the submission (renaming, nvn_vk.c) */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor top = {0, 0, 64, 32}, bottom = {0, 32, 64, 32};
        op(OP_SCISSOR, &top, sizeof top); op(OP_DRAW, &dw, sizeof dw);
        struct { uint64_t dst, n; float v[4]; } upd = {(uint64_t)(uintptr_t)fub, 16, {0.5f, 0.5f, 1, 1}};
        op(OP_UPDATE_UB, &upd, sizeof upd);
        op(OP_SCISSOR, &bottom, sizeof bottom); op(OP_DRAW, &dw, sizeof dw);
        p_scissor all = {0, 0, 64, 64}; op(OP_SCISSOR, &all, sizeof all);
        op(OP_COPY_T2B, &cp, sizeof cp);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4, *b = readback + (50 * 64 + 3) * 4;
        CHECK(abs(a[0] - 200) <= 2 && abs(b[0] - 100) <= 2, "uniform update between two draws: before %d (want 200), after %d (want 100)", a[0], b[0]);
        CHECK(fub[0] == 0.5f, "the update reached memory after the submission (%g)", fub[0]);
    }

    /* uniforms as of the submit: the game rewrites them right after submitting, before the GPU gets to the draw */
    for (int async_capture = 0; async_capture < 2; async_capture++) {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        fub[0] = 1; fub[1] = 0.5f;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
        uint64_t hrec = (uint64_t)(uintptr_t)rec;
        void *captured = async_capture ? mtl_sub_capture_ubos(&hrec, 1) : NULL;
        if (async_capture) fub[0] = 0.125f; /* eager asynchronous capture must already own the earlier value */
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&hrec, 1, captured);
        fub[0] = 0.25f;                                               /* the game's next frame, before the GPU ran this one */
        mtl_exec(rec); rn = 0; f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
        const uint8_t *a = readback + (10 * 64 + 3) * 4;
        int expected = snapshots ? 200 : 50;
        CHECK(abs(a[0] - expected) <= 2, "submit snapshot mode %d: red %d, want %d", snapshots, a[0], expected);
    }

    /* A long probe update adds enough draws to split a game submission into Vulkan chunks. Inline constants still
       overlay the submit-time snapshot after sealing: a fence on an update does not make it part of the snapshot.
       More than eight splits also recycle the entire slot ring while the same snapshot remains live. */
    {
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        p_scissor all = {0, 0, 64, 64};
        fub[0] = 1; fub[1] = 0.5f; fub[2] = fub[3] = 1;
        op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va);
        op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th);
        op(OP_VIEWPORTS, &vp, sizeof vp); op(OP_SCISSOR, &all, sizeof all); op(OP_END, NULL, 0);
        uint64_t hrec = (uint64_t)(uintptr_t)rec;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&hrec, 1, NULL); mtl_exec(rec); rn = 0;
        for (int split = 0; split < 12; split++) {
            struct { uint64_t dst, n; float v[4]; } upd = {(uint64_t)(uintptr_t)fub, 16, {0.25f, 0.5f, 1, 1}};
            /* Alternate overlapping full/partial writes so the newest red survives later updates to green. */
            upd.v[0] = split & 1 ? 0.5f : 0.25f;
            op(OP_UPDATE_UB, &upd, sizeof upd); op(OP_DRAW, &dw, sizeof dw); op(OP_END, NULL, 0);
            mtl_exec(rec); rn = 0;
            float immediate[4]; vk_ub_read((uint64_t)(uintptr_t)fub, immediate, sizeof immediate);
            CHECK(immediate[0] == upd.v[0], "inline mode %d before chunk %d: CPU read %g, want %g", renaming, split, immediate[0], upd.v[0]);
            if (!renaming) {
                VkDescriptorBufferInfo descriptor;
                CHECK(!vk_ub_rename((uint64_t)(uintptr_t)fub, 256, &descriptor), "direct mode keeps GPU-timeline descriptors after update %d", split);
            }
            vk_submit_chunk();
            float seen[4]; vk_ub_read((uint64_t)(uintptr_t)fub, seen, sizeof seen);
            CHECK(seen[0] == upd.v[0], "inline snapshot after chunk %d: CPU read %g, want %g", split, seen[0], upd.v[0]);
            struct { uint64_t dst, n; float v; uint32_t pad; } green = {(uint64_t)(uintptr_t)(fub + 1), 4, 0.25f, 0};
            op(OP_UPDATE_UB, &green, sizeof green); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
            vk_submit_chunk();
            op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0); mtl_exec(rec); rn = 0;
            vk_flush_wait();
            const uint8_t *a = readback + (10 * 64 + 3) * 4;
            CHECK(abs(a[0] - (int)(200 * upd.v[0])) <= 2 && abs(a[1] - 25) <= 2,
                  "inline snapshot after chunk %d: GPU red/green %d/%d, want %d/25", split, a[0], a[1], (int)(200 * upd.v[0]));
        }
        f = mtl_sub_commit(); CHECK(mtl_wait(f, 5000000000ull), "chunked inline snapshot submission completes");
        fub[0] = 1; fub[1] = 0.5f;
    }

    /* Deterministic frame-lag hazards: submit behind a host gate, then reuse read-only storage (direct and
       uniform-pointer-derived), vertices, or indices before the GPU executes. The lit scene must stay red. */
    if (runtime_frame_lag_requested()) {
        static uint8_t sctl[2][0x880], scode[2][0x230];
        prog_t storage_pg[2];
        for (int j = 0; j < 2; j++) {
            uint32_t len; shader_binary(1, 5 + j, sctl[j], scode[j], &len);
            uint8_t *code = cpu_pool + 0x102000 + 0x1000 * j; memcpy(code, scode[j], len);
            storage_pg[j] = pg; storage_pg[j].serial = 8 + j;
            storage_pg[j].sd[1].data = (NVNbufferAddress)(uintptr_t)code; storage_pg[j].sd[1].control = sctl[j];
            vk_shader *sh[6]; int resolved = !vk_prog_resolve(&storage_pg[j], sh) && sh[1];
            CHECK(resolved && sh[1]->nb == 1 && sh[1]->b[0].extent == 16, "static storage seed %d exposes its complete 16-byte reflected extent", j);
        }
        float *light = (float *)(cpu_pool + (16u << 20) - 256);
        uint16_t *indices = (uint16_t *)(cpu_pool + 0x204000);
        uint64_t *pointer = (uint64_t *)(cpu_pool + 0x205000);
        float *vertices = (float *)(cpu_pool + 0x206000), full[6] = {-1, -1, 3, -1, -1, 3};
        p_bind_buf sb = {1, 0, (uint64_t)(uintptr_t)light, 16};
        p_bind_buf ptr = {1, 0, (uint64_t)(uintptr_t)pointer, 256};
        p_bind_vbuf lagvb = vb; lagvb.r[0].address = (uint64_t)(uintptr_t)vertices; lagvb.r[0].size = sizeof full;
        p_draw_idx idx = {4, 1, 3, 0, 0, 1, (uint64_t)(uintptr_t)indices};
        vk_lag_stats(0);   /* reset guard and reuse-wait counters before the four synthetic frames */
        uint64_t bytes, ns, ranges, gpu;
        vk_lag_capture_take(&bytes, &ns, &ranges, &gpu);
        for (int mode = 0; mode < 4; mode++) {
            memcpy(vertices, full, sizeof full); indices[0] = 0; indices[1] = 1; indices[2] = 2;
            light[0] = light[3] = 1; light[1] = light[2] = 0; *pointer = (uint64_t)(uintptr_t)light;
            p_bind_program lagbp = {&storage_pg[mode == 1], 3};
            op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc); op(OP_BIND_PROGRAM, &lagbp, sizeof lagbp);
            op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &lagvb, sizeof lagvb);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &ptr, sizeof ptr); op(OP_BIND_SSBO, &sb, sizeof sb); op(OP_VIEWPORTS, &vp, sizeof vp);
            op(OP_DRAW_IDX, &idx, sizeof idx); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
            VkEvent gate; VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            VKCHECK(vkCreateEvent(V.dev, &ei, NULL, &gate));
            uint64_t handle = (uint64_t)(uintptr_t)rec;
            mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL);
            /* Reuse unchanged bindings across an actual Vulkan chunk, not a new logical game submission.
               A freeze memo must re-register live ranges (or provide a fresh immutable copy) after this fence. */
            mtl_exec(rec); rn = 0; vk_submit_chunk();
            CHECK(mtl_wait(mtl_last_fence(), 5000000000ull), "lag hazard mode %d first chunk completes", mode);
            vkCmdWaitEvents(vk_cb(), 1, &gate, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
            vk_draw_exec(OP_CLEAR_COLOR, &cc);
            vk_draw_exec(OP_DRAW_IDX, &idx); vk_draw_exec(OP_COPY_T2B, &cp);
            f = mtl_sub_commit();
            int waited = mode < 2 ? lag_test_rewrite(light, 4, 0, 0, gate) :
                         mode == 2 ? lag_test_rewrite(vertices, sizeof full, 0, 0, gate) :
                                     lag_test_rewrite(indices, 6, 0, 0, gate);
            const char *protection = getenv("TOTK_VK_LAG_MODE");
            if (vk_lag_capture_on() && (!protection || strcmp(protection, "copy"))) CHECK(waited, "lag hazard mode %d stalls only the reusing writer", mode);
            CHECK(mtl_wait(f, 5000000000ull), "lag hazard mode %d completes", mode);
            const uint8_t *pixel = readback + (10 * 64 + 10) * 4;
            CHECK(pixel[0] == 255 && !pixel[1] && !pixel[2], "lag hazard mode %d: submit-time storage/vertices/indices retain lit red, got %u,%u,%u", mode, pixel[0], pixel[1], pixel[2]);
            vkDestroyEvent(V.dev, gate, NULL);
        }
        vk_lag_capture_take(&bytes, &ns, &ranges, &gpu);
        if (vk_lag_capture_on()) {
            vk_lag_test_stats timing; vk_lag_test_stats_get(&timing);
            uint64_t samples = 0;
            for (unsigned k = 0; k < VK_LAG_KIND_COUNT; k++) samples += timing.kind[k].sample_count;
            const char *option = getenv("TOTK_VK_LAG_TIMING"); int timed = option && atoi(option);
            CHECK(timed ? samples != 0 : !samples && !ns,
                  "lag per-binding clocks follow TOTK_VK_LAG_TIMING without changing captured bytes");
        }
        printf("lag fixture: %.6f MiB/frame, %.6f ms/frame, %.2f captured ranges/frame, %.2f GPU-owned/frame (4 synthetic frames)\n",
               bytes / 1048576.0 / 4, ns / 1e6 / 4, ranges / 4.0, gpu / 4.0);
        vk_lag_stats(4);
        /* Fill every reusable ring packet with a captured vertex override, then reuse those same slots
           with a GPU-produced live vertex range. The latter must not retain the old private-copy handle.
           Eight fresh submissions are not enough: the regression appears only after slot/job reuse. */
        if (vk_lag_capture_on()) {
            light[0] = light[3] = 1; light[1] = light[2] = 0;
            indices[0] = 0; indices[1] = 1; indices[2] = 2;
            for (uint64_t frame = 100; frame < 102; frame++) {
                nvn_dirty_lag_frame(frame); nvn_dirty_lag_watch(vertices, sizeof full);
                lag_writer writer = {.dst = vertices, .n = sizeof full};
                lag_writer_run(&writer); memcpy(vertices, full, sizeof full);
            }
            uint8_t *degenerate = cpu_pool + 0x240000;
            memset(degenerate, 0, sizeof full);
            vk_lag_capture_take(&bytes, &ns, &ranges, &gpu);
#ifdef TOTK_VK_TESTS
            uint64_t checks0, failures0, resets0; vk_record_test_results(&checks0, &failures0, &resets0);
#endif
            for (int round = 0; round < 24; round++) {
                int live = round >= 8;
                if (live) nvn_dirty_lag_frame(1000 + (uint64_t)round);   /* demote old hot history */
#ifdef TOTK_VK_TESTS
                VkBuffer live_buffer; uint64_t live_offset;
                int resolved = vk_resolve((uint64_t)(uintptr_t)vertices, sizeof full, &live_buffer, &live_offset);
                CHECK(resolved, "lag override recycle vertex range resolves");
                vk_record_test_expect_vertex(live ? (uint64_t)(uintptr_t)vertices : 0, live_buffer, live_offset);
#endif
                p_bind_program lagbp = {&storage_pg[0], 3};
                op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cc, sizeof cc);
                op(OP_BIND_PROGRAM, &lagbp, sizeof lagbp);
                op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
                op(OP_BIND_VBUF, &lagvb, sizeof lagvb); op(OP_BIND_SSBO, &sb, sizeof sb);
                op(OP_VIEWPORTS, &vp, sizeof vp);
                op(OP_DRAW_IDX, &idx, sizeof idx); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_END, NULL, 0);
                uint64_t handle = (uint64_t)(uintptr_t)rec;
                mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL);
                if (live) mtl_sub_copy((uint64_t)(uintptr_t)degenerate, (uint64_t)(uintptr_t)vertices, sizeof full);
                mtl_exec(rec); rn = 0;
                CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "lag override recycle round %d completes", round);
                const uint8_t *pixel = readback + (10 * 64 + 10) * 4;
                CHECK(live ? !pixel[0] && !pixel[1] && pixel[2] == 255 : pixel[0] == 255 && !pixel[1] && !pixel[2],
                    "lag override recycle round %d binds %s vertices, got %u,%u,%u", round,
                    live ? "GPU-produced live" : "private-copy", pixel[0], pixel[1], pixel[2]);
            }
#ifdef TOTK_VK_TESTS
            uint64_t checks1, failures1, resets1; vk_record_test_results(&checks1, &failures1, &resets1);
            CHECK(!vk_record_enabled() || checks1 >= checks0 + 16, "recycled live vertex handles checked on real workers");
            CHECK(failures1 == failures0, "recycled live vertex binds never retain private-copy overrides");
            vk_record_test_expect_vertex(0, VK_NULL_HANDLE, 0);
#endif
            vk_lag_capture_take(&bytes, &ns, &ranges, &gpu);
            printf("lag override recycle: %llu copied bytes, %llu captured ranges, %llu GPU-owned ranges\n",
                (unsigned long long)bytes, (unsigned long long)ranges, (unsigned long long)gpu);
        }
        op(OP_BIND_PROGRAM, &bp, sizeof bp);
    }

    /* Full mid-pass clears can become loadOps; a store may be dropped only before an unconditional full overwrite.
       Run in separate processes with CLEAR_LOAD / STORE_CLEAR off and on. Partial/disabled clears and a copy before
       the overwrite must retain the earlier pixels. Depth and color are read back independently. */
    {
        tex_t depth = rt; depth.serial = 10001; depth.d.format = 0x34; depth.mem = gpu_pool + 0xf00000;
        nvn_tex_layout(&depth.d, &depth.L);
        p_set_rt drt = srt; drt.depth = &depth;
        p_copy_b2t dc = cp; dc.tex = &depth; dc.src = (uint64_t)(uintptr_t)(readback + 0x5000);
        p_copy_b2t before = cp; before.src = (uint64_t)(uintptr_t)(readback + 0xa000);
        p_scissor all = {0, 0, 64, 64}, bottom = {0, 32, 64, 32};
        p_clear_color green = {0, 0xf, {0, 1, 0, 1}};
        p_clear_ds initial = {0.25f, 1, 0, 0}, later = {0.75f, 1, 0, 0};
        p_flag off = {0}, on = {1};
        fub[0] = 1; fub[1] = 0.5f;
        for (int mode = 0; mode < 4; mode++) {
            op(OP_SCISSOR, &all, sizeof all); op(OP_SET_RT, &drt, sizeof drt);
            op(OP_CLEAR_COLOR, &cc, sizeof cc); op(OP_CLEAR_DS, &initial, sizeof initial);
            op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
            op(OP_BIND_VBUF, &vb, sizeof vb); op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
            op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp); op(OP_DRAW, &dw, sizeof dw);
            if (mode == 1) op(OP_SCISSOR, &bottom, sizeof bottom);
            if (mode == 2) op(OP_RENDER_ENABLE, &off, sizeof off);
            if (mode == 3) op(OP_COPY_T2B, &before, sizeof before);
            op(OP_CLEAR_COLOR, &green, sizeof green); op(OP_CLEAR_DS, &later, sizeof later);
            op(OP_RENDER_ENABLE, &on, sizeof on); op(OP_SCISSOR, &all, sizeof all);
            op(OP_COPY_T2B, &cp, sizeof cp); op(OP_COPY_T2B, &dc, sizeof dc);
            f = run(); CHECK(mtl_wait(f, 5000000000ull), "mid-pass clear mode %d completes", mode);
            int bad = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                const uint8_t *c = readback + 4 * (y * 64 + x);
                int drawn = y < 32 && (mode == 1 || mode == 2), blue = y >= 32 && mode == 2;
                int r = drawn ? 200 : 0, g0 = drawn ? 50 : blue ? 0 : 255, b0 = blue ? 255 : drawn ? 128 : 0;
                float d; memcpy(&d, readback + 0x5000 + 4 * (y * 64 + x), 4);
                float expected = mode == 2 || (mode == 1 && y < 32) ? 0.25f : 0.75f;
                bad += abs(c[0] - r) > 2 || abs(c[1] - g0) > 2 || abs(c[2] - b0) > 2 || c[3] != 255 || d != expected;
            }
            CHECK(!bad, "mid-pass clear mode %d retains color/depth (%d wrong pixels)", mode, bad);
            if (mode == 3) {
                const uint8_t *c = readback + 0xa000 + 4 * (10 * 64 + 3);
                CHECK(abs(c[0] - 200) <= 2 && abs(c[1] - 50) <= 2, "copy before full clear still sees the draw (%u,%u)", c[0], c[1]);
            }
        }
    }

    /* Empty reports exercise the default path too. With TOTK_VK_OCCLUSION=1, check exact samples and report snapshots,
       including mid-pass resets/reports, pass splits and a later submission without a reset. No new shaders needed. */
    int real_samples = 0;
    {
        uint64_t (*q)[2] = (uint64_t (*)[2])(cpu_pool + 0x310000), type = NVN_COUNTER_TYPE_SAMPLES_PASSED;
        memset(q, 0xee, 8 * sizeof *q);
        uint64_t report[2] = {type, (uint64_t)(uintptr_t)q[0]};
        op(OP_RESET_COUNTER, &type, 8); op(OP_REPORT_COUNTER, report, sizeof report); op(OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(rec); rn = 0;
        uint64_t pending; int real = real_samples = !vk_counter_value(report[1], &pending);
        f = mtl_sub_commit(); CHECK(mtl_wait(f, 5000000000ull), "empty sample report completes");
        CHECK(q[0][0] == 0 && q[0][1] != 0xeeeeeeeeeeeeeeeeull, "empty sample report (%s)", real ? "real" : "synthetic cumulative");
        if (real) {
            float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
            p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
            op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va);
            op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
            for (int i = 1; i <= 2; i++) {
                op(OP_DRAW, &dw, sizeof dw); report[1] = (uint64_t)(uintptr_t)q[i]; op(OP_REPORT_COUNTER, report, sizeof report);
            }
            op(OP_RESET_COUNTER, &type, 8);
            p_viewports outside = vp; outside.v[0][0] = 64; op(OP_VIEWPORTS, &outside, sizeof outside); op(OP_DRAW, &dw, sizeof dw);
            report[1] = (uint64_t)(uintptr_t)q[3]; op(OP_REPORT_COUNTER, report, sizeof report);   /* no covered samples */
            op(OP_RESET_COUNTER, &type, 8); op(OP_VIEWPORTS, &vp, sizeof vp);
            op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp); op(OP_DRAW, &dw, sizeof dw);   /* two pass segments */
            report[1] = (uint64_t)(uintptr_t)q[4]; op(OP_REPORT_COUNTER, report, sizeof report);
            report[1] = (uint64_t)(uintptr_t)q[5]; op(OP_REPORT_COUNTER, report, sizeof report);   /* no intervening draw */
            op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(rec); rn = 0;
            p_render_cond cond = {.addr = (uint64_t)(uintptr_t)q[4], .mode = 0};
            vk_draw_exec(OP_RENDER_COND, &cond); CHECK(!S.render_disabled, "pending sample equality condition stays visible");
            cond.mode = 1; vk_draw_exec(OP_RENDER_COND, &cond); CHECK(!S.render_disabled, "pending sample inequality condition stays visible");
            f = mtl_sub_commit(); CHECK(mtl_wait(f, 5000000000ull), "sample draws complete");
            CHECK(q[1][0] == 4096 && q[2][0] == 8192, "same-pass cumulative snapshots: %llu, %llu", (unsigned long long)q[1][0], (unsigned long long)q[2][0]);
            CHECK(q[3][0] == 0, "reset inside a pass, offscreen draw: %llu", (unsigned long long)q[3][0]);
            CHECK(q[4][0] == 8192 && q[5][0] == 8192, "sum across passes and repeat report: %llu, %llu", (unsigned long long)q[4][0], (unsigned long long)q[5][0]);
            op(OP_DRAW, &dw, sizeof dw); report[1] = (uint64_t)(uintptr_t)q[6]; op(OP_REPORT_COUNTER, report, sizeof report);
            op(OP_RESET_COUNTER, &type, 8); report[1] = (uint64_t)(uintptr_t)q[7]; op(OP_REPORT_COUNTER, report, sizeof report);
            f = run(); CHECK(mtl_wait(f, 5000000000ull), "cross-submission samples complete");
            CHECK(q[6][0] == 12288 && q[7][0] == 0, "counter survives submission; later reset cannot change earlier report: %llu, %llu", (unsigned long long)q[6][0], (unsigned long long)q[7][0]);
        }
    }

    /* two texture objects over one CPU-pool storage: what the GPU renders through one is what the other reads
       (the per-frame render-target object and the persistent sampling object of a G-buffer) */
    {
        tex_t ta, tb; memset(&ta, 0, sizeof ta);
        ta.d = tx.d; nvn_tex_layout(&ta.d, &ta.L); ta.mem = cpu_pool + 0x400000; ta.pool = &P_cpu; ta.serial = 201;
        tb = ta; tb.serial = 202;
        memset(ta.mem, 0, ta.L.size);
        p_copy_b2t cb0; memset(&cb0, 0, sizeof cb0); cb0.src = (uint64_t)(uintptr_t)readback; cb0.tex = &tb; cb0.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
        op(OP_COPY_T2B, &cb0, sizeof cb0);   /* the sampling object exists first (uploaded from the zero pool bytes) */
        p_set_rt art; memset(&art, 0, sizeof art); art.n = 1; art.color[0] = &ta;
        p_clear_color cr = {0, 0xf, {1, 0, 1, 1}};
        p_set_rt no_rt; memset(&no_rt, 0, sizeof no_rt);
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cr, sizeof cr); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        memset(readback, 0xcd, 64 * 64 * 4);
        op(OP_COPY_T2B, &cb0, sizeof cb0);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *q = readback + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && q[2] == 255, "a GPU-written CPU-pool storage read through another object: %d %d %d, want the magenta clear", q[0], q[1], q[2]);
        /* GPU write-back (nvn_mtl.m wb_note): after an invalidate the CPU sees the rendering in the NVN layout
           (this part of the pool is dirty-tracked as the game's pools are: CPU writes since a rendering are seen) */
        nvn_dirty_add_range(cpu_pool + 0x400000, 0x200000);
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cr, sizeof cr); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        uint8_t lin2[64 * 64 * 4];
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        nvn_deswizzle(&ta.d, 0, 0, ta.mem, lin2, 256);
        q = lin2 + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && q[2] == 255 && q[3] == 255, "GPU rendering written back on invalidate: %d %d %d %d, want the magenta clear", q[0], q[1], q[2], q[3]);
        /* a CPU write after the rendering is newer: the next rendering is not written back over it */
        p_clear_color cg = {0, 0xf, {0, 1, 0, 1}};
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cg, sizeof cg); op(OP_SET_RT, &no_rt, sizeof no_rt);
        f = run(); mtl_wait(f, 5000000000ull);
        ta.mem[ta.L.size - 1] = 7;
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        nvn_deswizzle(&ta.d, 0, 0, ta.mem, lin2, 256);
        q = lin2 + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[1] == 0 && ta.mem[ta.L.size - 1] == 7, "no write-back over a CPU write since the rendering: %d %d %d, want magenta kept", q[0], q[1], q[2]);
        /* a GPU copy into the memory after the rendering supersedes it */
        op(OP_SET_RT, &art, sizeof art); op(OP_CLEAR_COLOR, &cg, sizeof cg); op(OP_SET_RT, &no_rt, sizeof no_rt);
        memset(cpu_pool + 0x500000, 0x5a, ta.L.size);
        uint64_t cp[3] = {(uint64_t)(uintptr_t)(cpu_pool + 0x500000), (uint64_t)(uintptr_t)ta.mem, ta.L.size};
        op(OP_COPY_B2B, cp, sizeof cp);
        f = run(); mtl_wait(f, 5000000000ull);
        mtl_pool_invalidate(&P_cpu, 0x400000, ta.L.size);
        CHECK(ta.mem[0] == 0x5a && ta.mem[ta.L.size - 1] == 0x5a, "a GPU copy after the rendering is not overwritten by its write-back: %#x %#x", ta.mem[0], ta.mem[ta.L.size - 1]);
    }

    /* compressible storage changing format (nvn_mtl.m stor_compressed): the linear depth (R32F) moved away, its old memory
       now an RGBA8 target cleared to white; a stale descriptor of the depth must still read depth, not 0xFFFFFFFF = NaN */
    {
        static pool_t P_cz; P_cz = P_gpu; P_cz.flags |= NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE;
        tex_t zd, zc; memset(&zd, 0, sizeof zd);
        zd.d = tx.d; zd.d.format = 0xA; zd.d.flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE; nvn_tex_layout(&zd.d, &zd.L);
        zd.mem = gpu_pool + 0x600000; zd.pool = &P_cz; zd.serial = 211;
        zc = zd; zc.d.format = 0x25; nvn_tex_layout(&zc.d, &zc.L); zc.serial = 212;
        p_set_rt zr; memset(&zr, 0, sizeof zr); zr.n = 1; zr.color[0] = &zd;
        p_set_rt cr2; memset(&cr2, 0, sizeof cr2); cr2.n = 1; cr2.color[0] = &zc;
        p_set_rt no_rt; memset(&no_rt, 0, sizeof no_rt);
        p_clear_color half = {0, 0xf, {0.5f, 0, 0, 0}}, white = {0, 0xf, {1, 1, 1, 1}};
        op(OP_SET_RT, &zr, sizeof zr); op(OP_CLEAR_COLOR, &half, sizeof half); op(OP_SET_RT, &no_rt, sizeof no_rt);
        op(OP_SET_RT, &cr2, sizeof cr2); op(OP_CLEAR_COLOR, &white, sizeof white); op(OP_SET_RT, &no_rt, sizeof no_rt);
        p_copy_b2t zb; memset(&zb, 0, sizeof zb); zb.src = (uint64_t)(uintptr_t)readback; zb.tex = &zd; zb.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
        memset(readback, 0xcd, 64 * 64 * 4);
        op(OP_COPY_T2B, &zb, sizeof zb);
        f = run(); mtl_wait(f, 5000000000ull);
        float z; memcpy(&z, readback + (30 * 64 + 30) * 4, 4);
        CHECK(z == 0.5f, "compressible R32F storage taken by an RGBA8 target: the old depth object reads %g (bits %08x), want 0.5", z, *(uint32_t *)(readback + (30 * 64 + 30) * 4));
        zb.tex = &zc; memset(readback, 0xcd, 64 * 64 * 4);
        op(OP_COPY_T2B, &zb, sizeof zb);
        f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *w = readback + (30 * 64 + 30) * 4;
        CHECK(w[0] == 255 && w[1] == 255 && w[2] == 255 && w[3] == 255, "the RGBA8 target on that storage: %d %d %d %d, want white", w[0], w[1], w[2], w[3]);
    }

    p_set_rt none_rt0; memset(&none_rt0, 0, sizeof none_rt0);
    /* conditional rendering in GPU order: reports recorded in this submission decide a conditional in it (their memory
       is only written when it completes); samples passed counts the draws between reports ("everything visible") */
    if (!real_samples) {   /* precise queries intentionally leave pending sample conditions visible; tested above */
        uint64_t *cnt = (uint64_t *)(cpu_pool + 0x300000); memset(cnt, 0x55, 64); cnt[2] = 0;   /* stale memory says not equal */
        uint64_t rs = NVN_COUNTER_TYPE_SAMPLES_PASSED, rep0[2] = {rs, (uint64_t)(uintptr_t)cnt}, rep1[2] = {rs, (uint64_t)(uintptr_t)(cnt + 2)};
        p_render_cond eq = {0, 0, (uint64_t)(uintptr_t)cnt}; p_flag on = {1};
        p_clear_color red = {0, 0xf, {1, 0, 0, 1}}, blue = {0, 0xf, {0, 0, 1, 1}};
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        /* 1: no draw between the reports -> equal -> the red clear runs */
        op(OP_RESET_COUNTER, &rs, 8); op(OP_REPORT_COUNTER, rep0, 16); op(OP_REPORT_COUNTER, rep1, 16);
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &blue, sizeof blue); op(OP_SET_RT, &none_rt0, sizeof none_rt0);
        op(OP_RENDER_COND, &eq, sizeof eq); op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &red, sizeof red); op(OP_SET_RT, &none_rt0, sizeof none_rt0); op(OP_RENDER_ENABLE, &on, sizeof on);
        op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        const uint8_t *q = readback + (20 * 64 + 20) * 4;
        CHECK(q[0] == 255 && q[2] == 0, "conditional clear after two reports with no draw between (equal): %d %d %d, want red", q[0], q[1], q[2]);
        /* 2: a draw between the reports -> not equal -> the red clear is skipped */
        memset(cnt, 0x55, 64);
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &blue, sizeof blue);
        op(OP_RESET_COUNTER, &rs, 8); op(OP_REPORT_COUNTER, rep0, 16);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
        op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
        p_scissor corner = {0, 0, 4, 4}, all = {0, 0, 64, 64}; op(OP_SCISSOR, &corner, sizeof corner); op(OP_DRAW, &dw, sizeof dw); op(OP_SCISSOR, &all, sizeof all);
        op(OP_REPORT_COUNTER, rep1, 16); op(OP_SET_RT, &none_rt0, sizeof none_rt0);
        op(OP_RENDER_COND, &eq, sizeof eq); op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &red, sizeof red); op(OP_SET_RT, &none_rt0, sizeof none_rt0); op(OP_RENDER_ENABLE, &on, sizeof on);
        op(OP_COPY_T2B, &cp, sizeof cp); f = run(); mtl_wait(f, 5000000000ull);
        CHECK(q[0] == 0 && q[2] == 255, "conditional clear after a draw between the reports (not equal): %d %d %d, want blue", q[0], q[1], q[2]);
        CHECK(cnt[0] == 0 && cnt[2] > 0, "reported samples: %llu then %llu (want 0, then the draw's)", (unsigned long long)cnt[0], (unsigned long long)cnt[2]);
    }
    /* The title-cut / terrain pattern: conditional clear if a != a+16, report a immediately after it.
       All reports are still pending: the first clear runs, later ones see the newest report and skip.
       After completion, the memory fallback must give the very same answer. */
    {
        uint64_t *cnt = (uint64_t *)(cpu_pool + 0x310080); cnt[0] = 99; cnt[2] = 0;
        p_render_cond ne = {1, 0, (uint64_t)(uintptr_t)cnt}; p_flag on = {1};
        mtl_sub_begin();
        for (int i = 0; i < 4; i++) {
            vk_draw_exec(OP_RENDER_COND, &ne);
            CHECK(S.render_disabled == (i != 0), "conditional/report sequence %d: exactly the first clear enabled", i);
            mtl_sub_counter((uint64_t)(uintptr_t)cnt, NVN_COUNTER_TYPE_TIMESTAMP_TOP);
        }
        vk_draw_exec(OP_RENDER_ENABLE, &on); f = mtl_sub_commit();
        mtl_sub_begin(); vk_draw_exec(OP_RENDER_COND, &ne);
        CHECK(S.render_disabled, "next submission sees newest report whether pending or complete");
        vk_draw_exec(OP_RENDER_ENABLE, &on); uint64_t f2 = mtl_sub_commit();
        CHECK(mtl_wait(f2, 5000000000ull) && mtl_wait(f, 0), "conditional/report submissions complete");
        mtl_sub_begin(); vk_draw_exec(OP_RENDER_COND, &ne);
        CHECK(S.render_disabled, "completed report memory matches pending report evaluation");
        vk_draw_exec(OP_RENDER_ENABLE, &on); f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
    }
    /* The test guest clock is eight hours ahead of CLOCK_MONOTONIC. GPU end
       minus guest CPU origin must still be a small positive frame duration. */
    {
        uint64_t *report = (uint64_t *)(cpu_pool + 0x3100c0);
        uint64_t origin_ns = runtime_now_ns(), origin = guest_clock_ns_to_ticks(origin_ns);
        enum { chunks = 12, delay_us = 2000 }; /* exceed slot ring capacity: old query owners must survive reuse */
        memset(report, 0, chunks * 16);
        vk_gpu_time_guest_submit(origin_ns, origin, 177);
        mtl_sub_begin();
        for (unsigned i = 0; i < chunks; i++) {
            usleep(delay_us); /* synthetic host recording, not guest idle time */
            mtl_sub_counter((uint64_t)(uintptr_t)(report + i * 2), NVN_COUNTER_TYPE_TIMESTAMP);
            if (i + 1 < chunks) vk_submit_chunk();
        }
        uint64_t ticket = mtl_sub_commit();
        CHECK(mtl_wait(ticket, 5000000000ull), "offset guest-clock timestamp submission completes");
        int64_t duration = (int64_t)(report[1] - origin);
        CHECK(duration > 0 && duration < 19200000,
              "GPU end minus eight-hour-offset guest CPU origin is a frame duration: %lld ticks",
              (long long)duration);
        int ordered = 1;
        for (unsigned i = 1; i < chunks; i++) ordered &= report[i * 2 + 1] >= report[(i - 1) * 2 + 1];
        CHECK(ordered && report[(chunks - 1) * 2 + 1] > report[1],
              "carried chunk reports stay ordered and retain accumulated GPU execution after slot reuse");
        const char *time_mode = getenv("TOTK_GPU_TIME");
        int hardware_time = time_mode && !strcmp(time_mode, "hardware");
        uint64_t span = report[(chunks - 1) * 2 + 1] - report[1];
        CHECK(hardware_time ? span < chunks * delay_us * 19200 / 2000 : span >= (chunks - 1) * delay_us * 19200 / 1000,
              "chunk query span retains/removes only the selected host-recording latency: %llu ticks (%s)",
              (unsigned long long)span, hardware_time ? "hardware" : "latency");
        CHECK(!hardware_time || duration < delay_us * 19200 / 2000,
              "first report retains its original producer chunk, not final-delivery chunk: %lld ticks",
              (long long)duration);
        if (hardware_time) {
            uint64_t consumer_origin = report[(chunks - 1) * 2 + 1] + 629648;
            int correct = 1;
            for (unsigned i = 0; i < chunks; i++) {
                uint64_t stamp = report[i * 2 + 1];
                uint64_t converted = vk_gpu_time_controller_end((uint64_t)(uintptr_t)(report + i * 2), stamp, consumer_origin);
                uint64_t transported = guest_clock_ns_to_ticks(converted);
                correct &= transported >= consumer_origin && transported - consumer_origin == stamp - origin;
            }
            CHECK(correct, "lagged controller uses each report's producer origin after 12 chunks/ring reuse");
            uint64_t held = guest_clock_ns_to_ticks(vk_gpu_time_controller_end(
                (uint64_t)(uintptr_t)report, report[1] + 1, consumer_origin));
            CHECK(held - consumer_origin == report[(chunks - 1) * 2 + 1] - origin,
                  "mismatched report timestamp holds previous completed input, not zero/latency");
        }
        vk_slot_stats(300); /* exercise the per-300 diagnostics with known producer origins */
    }

    /* a clear with no draw after it, then other targets: the clear still happens */
    p_clear_color cg = {0, 0xf, {0, 1, 0, 1}};
    op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &cg, sizeof cg);
    p_set_rt none; memset(&none, 0, sizeof none); op(OP_SET_RT, &none, sizeof none);
    op(OP_COPY_T2B, &cp, sizeof cp);
    f = run(); mtl_wait(f, 5000000000ull);
    p = readback + (40 * 64 + 9) * 4;
    CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255, "a clear without a draw: (9,40) = %d %d %d %d, want 0 255 0 255", p[0], p[1], p[2], p[3]);

    /* rendering into a BC4 texture through an RGBA16UI view whose texel is one block (TOTK) */
    tex_t bc; memset(&bc, 0, sizeof bc);
    bc.d.target = 1; bc.d.format = 0x49; bc.d.width = 64; bc.d.height = 64; bc.d.depth = 1; bc.d.levels = 1;
    nvn_tex_layout(&bc.d, &bc.L); bc.mem = gpu_pool + 0x100000; bc.pool = &P_gpu; bc.serial = 103;
    p_set_rt brt; memset(&brt, 0, sizeof brt); brt.n = 1; brt.color[0] = &bc; brt.has_cview[0] = 1;
    brt.cview[0].set_format = 1; brt.cview[0].format = 0x2c;
    p_clear_color cu = {0, 0xf, {0x1111, 0x2222, 0x3333, 0x4444}};
    op(OP_SET_RT, &brt, sizeof brt); op(OP_CLEAR_COLOR, &cu, sizeof cu);
    p_copy_b2t cb4; memset(&cb4, 0, sizeof cb4); cb4.src = (uint64_t)(uintptr_t)readback; cb4.tex = &bc; cb4.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    memset(readback, 0xcd, 16 * 16 * 8);
    op(OP_COPY_T2B, &cb4, sizeof cb4);
    f = run(); mtl_wait(f, 5000000000ull);
    const uint16_t *blk = (const uint16_t *)(readback + (7 * 16 + 5) * 8);
    CHECK(blk[0] == 0x1111 && blk[1] == 0x2222 && blk[2] == 0x3333 && blk[3] == 0x4444, "BC4 block (5,7) written through the RGBA16UI view = %04x %04x %04x %04x", blk[0], blk[1], blk[2], blk[3]);
    /* the same in a CPU pool, rendered and invalidated: the blocks reach memory (TOTK's terrain bakes) */
    {
        tex_t bcc = bc; bcc.mem = cpu_pool + 0x600000; bcc.pool = &P_cpu; bcc.serial = 105;
        nvn_dirty_add_range(cpu_pool + 0x600000, 0x100000);
        memset(bcc.mem, 0, bcc.L.size);
        p_set_rt crt = brt; crt.color[0] = &bcc;
        p_set_rt no_rt2; memset(&no_rt2, 0, sizeof no_rt2);
        op(OP_SET_RT, &crt, sizeof crt); op(OP_CLEAR_COLOR, &cu, sizeof cu); op(OP_SET_RT, &no_rt2, sizeof no_rt2);
        f = run(); mtl_wait(f, 5000000000ull);
        mtl_pool_invalidate(&P_cpu, 0x600000, bcc.L.size);
        static uint8_t lin4[16 * 16 * 8];
        nvn_deswizzle(&bcc.d, 0, 0, bcc.mem, lin4, 16 * 8);
        const uint16_t *b2 = (const uint16_t *)(lin4 + (7 * 16 + 5) * 8);
        CHECK(b2[0] == 0x1111 && b2[1] == 0x2222 && b2[2] == 0x3333 && b2[3] == 0x4444, "CPU-pool BC4 rendering written back on invalidate: block (5,7) = %04x %04x %04x %04x", b2[0], b2[1], b2[2], b2[3]);
    }

    /* a render target in one depth slice of a 3D texture */
    tex_t v3; memset(&v3, 0, sizeof v3);
    v3.d.target = 2; v3.d.format = 0x25; v3.d.width = 16; v3.d.height = 16; v3.d.depth = 4; v3.d.levels = 1;
    nvn_tex_layout(&v3.d, &v3.L); v3.mem = gpu_pool + 0x200000; v3.pool = &P_gpu; v3.serial = 104;
    p_set_rt vrt; memset(&vrt, 0, sizeof vrt); vrt.n = 1; vrt.color[0] = &v3; vrt.has_cview[0] = 1;
    vrt.cview[0].set_layers = 1; vrt.cview[0].min_layer = 2; vrt.cview[0].nlayers = 1;
    p_clear_color cz = {0, 0xf, {1, 0, 0, 1}}, cz0 = {0, 0xf, {0, 0, 0, 0}};
    p_set_rt vrt0 = vrt; vrt0.cview[0].set_layers = 0;
    vrt0.has_cview[0] = 1; vrt0.cview[0].set_layers = 1; vrt0.cview[0].min_layer = 0; vrt0.cview[0].nlayers = 4;
    op(OP_SET_RT, &vrt0, sizeof vrt0); op(OP_CLEAR_COLOR, &cz0, sizeof cz0);
    op(OP_SET_RT, &vrt, sizeof vrt); op(OP_CLEAR_COLOR, &cz, sizeof cz);
    p_copy_b2t c3; memset(&c3, 0, sizeof c3); c3.src = (uint64_t)(uintptr_t)readback; c3.tex = &v3; c3.r = (NVNcopyRegion){0, 0, 0, 16, 16, 4};
    memset(readback, 0xcd, 16 * 16 * 4 * 4);
    op(OP_COPY_T2B, &c3, sizeof c3);
    f = run(); mtl_wait(f, 5000000000ull);
    const uint8_t *s1 = readback + 1 * 16 * 16 * 4 + (3 * 16 + 3) * 4, *s2 = readback + 2 * 16 * 16 * 4 + (3 * 16 + 3) * 4;
    CHECK(s2[0] == 255 && s2[1] == 0 && s2[3] == 255 && !s1[0] && !s1[3], "3D slice 2 as a render target: slice 2 = %d %d %d %d, slice 1 = %d %d %d %d", s2[0], s2[1], s2[2], s2[3], s1[0], s1[1], s1[2], s1[3]);

#ifdef TOTK_ANDROID_APP
    /* the app's presenter: the window crop of the presented texture reaches the window, row by row */
    {
        extern ANativeWindow *mock_window(void); extern uint8_t *mock_window_pixels; extern int32_t mock_window_w, mock_window_h, mock_window_stride; extern _Atomic int mock_window_posts;
        uint64_t vk_window_set(ANativeWindow *w, ANativeWindow *surface_parent, int display_id);
        setenv("TOTK_APP_WIDTH", "96", 1);   /* the window picture's width: the mock window's (96x32) */
        vk_window_set(mock_window(), NULL, -1);
        int crop[4] = {8, 4, 32, 16};
        mtl_present(&rt, 1.0 / 60, crop);
        for (int i = 0; i < 200 && !mock_window_posts; i++) usleep(10000);
        CHECK(mock_window_posts == 1 && mock_window_w == 96 && mock_window_h == 32, "a frame posted to the window at TOTK_APP_WIDTH and the window's shape (%d posts, %dx%d)", mock_window_posts, mock_window_w, mock_window_h);
        /* the 32x16 crop scaled 2x into the middle 64x32 of the 96x32 window, black bars left and right; rt was last cleared green */
        const uint8_t *in = mock_window_pixels ? mock_window_pixels + ((size_t)10 * mock_window_stride + 48) * 4 : NULL, *bar = mock_window_pixels ? mock_window_pixels + ((size_t)10 * mock_window_stride + 5) * 4 : NULL;
        CHECK(in && in[0] == 0 && in[1] == 255 && in[2] == 0, "window middle: %d %d %d, want the green clear", in ? in[0] : -1, in ? in[1] : -1, in ? in[2] : -1);
        CHECK(bar && bar[0] == 0 && bar[1] == 0 && bar[2] == 0, "window bar: %d %d %d, want black", bar ? bar[0] : -1, bar ? bar[1] : -1, bar ? bar[2] : -1);
        /* the pass probe (TOTK_VK_PASSPROBE=1, set at the start): a green frame then a black one differ in their one pass */
        int vk_probe_report(uint64_t flash, uint64_t before); uint64_t vk_present_count(void);
        p_clear_color green = {0, 0xf, {0, 1, 0, 1}}, black = {0, 0xf, {0, 0, 0, 1}};
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &green, sizeof green); op(OP_SET_RT, &none, sizeof none); f = run();
        mtl_present(&rt, 1.0 / 60, crop); uint64_t fa = vk_present_count();
        op(OP_SET_RT, &srt, sizeof srt); op(OP_CLEAR_COLOR, &black, sizeof black); op(OP_SET_RT, &none, sizeof none); f = run();
        mtl_present(&rt, 1.0 / 60, crop); uint64_t fb = vk_present_count();
        mtl_wait(mtl_last_fence(), 5000000000ull);
        CHECK(vk_probe_report(fb, fa) == 1, "pass probe: the black frame's one pass differs from the green frame's");
        CHECK(vk_probe_report(fa, fa) == 0, "pass probe: a frame matches itself");
        /* a draw whose uniform differs between two frames: the report names the uniform (look for "uniform 1" ... "differs") */
        float full[6] = {-1, -1, 3, -1, -1, 3}; float *vtx2 = (float *)(cpu_pool + 0x202000); memcpy(vtx2, full, sizeof full);
        p_bind_vbuf vb2 = vb; vb2.r[0].address = (NVNbufferAddress)(uintptr_t)vtx2;
        for (int k = 0; k < 2; k++) {
            fub[0] = k ? 0.1f : 0.9f;
            op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb2, sizeof vb2);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
            op(OP_DRAW, &dw, sizeof dw); op(OP_SET_RT, &none, sizeof none); f = run(); mtl_wait(f, 5000000000ull);
            mtl_present(&rt, 1.0 / 60, crop); if (k) fb = vk_present_count(); else fa = vk_present_count();
        }
        mtl_wait(mtl_last_fence(), 5000000000ull);
        CHECK(vk_probe_report(fb, fa) == 1, "pass probe: a draw whose uniform changed differs");
    }
#endif
    /* "passdump 1": the next frame but one, each pass's targets as PNG with an index */
    {
        void dm_gfx_passdump_next(long n); uint64_t vk_present_count(void);
        int crop0[4] = {0, 0, 0, 0};
        dm_gfx_passdump_next(1); uint64_t target = vk_present_count() + 2;
        mtl_present(&rt, 1.0 / 60, crop0);
        p_set_rt prt; memset(&prt, 0, sizeof prt); prt.n = 1; prt.color[0] = &rt; p_set_rt pnone; memset(&pnone, 0, sizeof pnone);
        p_clear_color pc = {0, 0xf, {0.25f, 0.5f, 1, 1}};
        op(OP_SET_RT, &prt, sizeof prt); op(OP_CLEAR_COLOR, &pc, sizeof pc); op(OP_SET_RT, &pnone, sizeof pnone); f = run();
        mtl_present(&rt, 1.0 / 60, crop0);
        char path[128]; snprintf(path, sizeof path, "passdump/f%llu/p000_c0_64x64.png", (unsigned long long)target);
        FILE *pf = fopen(path, "rb"); uint8_t hdr[8] = {0}; size_t nhdr = 0; if (pf) { nhdr = fread(hdr, 1, 8, pf); fclose(pf); }
        CHECK(nhdr == 8 && hdr[1] == 'P', "passdump wrote %s", path);
        snprintf(path, sizeof path, "passdump/f%llu/index.txt", (unsigned long long)target);
        pf = fopen(path, "r"); char idx[512] = {0}; if (pf) { size_t nidx = fread(idx, 1, sizeof idx - 1, pf); idx[nidx] = 0; fclose(pf); }
        CHECK(strstr(idx, "pass 0") && strstr(idx, "c0: ser 102 64x64"), "passdump index: %.200s", idx);
    }
    /* more descriptor sets in a submission than one pool holds, many times: the slots' pools are reused, not added to */
    {
        int vk_dpool_count(void);
        VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
        VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(V.dev, &li, NULL, &dsl);
        int after_first = 0;
        for (int round = 0; round < 40; round++) {
            mtl_sub_begin(); for (int i = 0; i < 5000; i++) (void)vk_alloc_set(dsl); f = mtl_sub_commit(); mtl_wait(f, 5000000000ull);
            if (round == 15) after_first = vk_dpool_count();
        }
        CHECK(vk_dpool_count() == after_first, "descriptor pools reused across submissions: %d after 16 rounds, %d after 40", after_first, vk_dpool_count());
    }
    /* GPU profiler acceptance: real passes in 301 frames, recycling all fence slots. The first
       pass loads, then fully clears before its first draw; a later copy consumes its store.
       The second clear-only pass has no later read. Keep ordinary test runs unchanged. */
    if (vk_gpuprof_on) {
        p_set_rt none = {0};
        uint8_t cctl[0x880], ccode[0x230]; uint32_t clen;
        shader_binary(5, 3, cctl, ccode, &clen);
        memcpy(cpu_pool + 0x102000, ccode, clen);
        prog_t compute = {0}; compute.nstage = 1; compute.serial = 900;
        compute.sd[0].data = (NVNbufferAddress)(uintptr_t)(cpu_pool + 0x102000); compute.sd[0].control = cctl;
        p_bind_program cbp = {&compute, 1u << 5};
        uint32_t *result = (uint32_t *)(cpu_pool + 0x500000); memset(result, 0, 256);
        p_bind_buf output = {5, 0, (uint64_t)(uintptr_t)result, 256};
        p_dispatch dispatch = {1, 1, 1};
        p_scissor half = {0, 0, 32, 64}, full = {0, 0, 64, 64};
        for (int n = 0; n < 301; n++) {
            op(OP_SET_RT, &srt, sizeof srt); op(OP_BIND_PROGRAM, &bp, sizeof bp);
            op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs); op(OP_BIND_VBUF, &vb, sizeof vb);
            op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1); op(OP_BIND_TEX, &th, sizeof th); op(OP_VIEWPORTS, &vp, sizeof vp);
            op(OP_SCISSOR, &half, sizeof half); op(OP_CLEAR_COLOR, &cc, sizeof cc);
            op(OP_SCISSOR, &full, sizeof full); op(OP_CLEAR_COLOR, &cc, sizeof cc);
            op(OP_DRAW, &dw, sizeof dw); op(OP_COPY_T2B, &cp, sizeof cp);
            op(OP_CLEAR_COLOR, &cc, sizeof cc); op(OP_SET_RT, &none, sizeof none);
            op(OP_BIND_PROGRAM, &cbp, sizeof cbp); op(OP_BIND_UBO, &output, sizeof output);
            op(OP_DISPATCH, &dispatch, sizeof dispatch);
            f = run(); mtl_present(NULL, 1.0 / 60, NULL);
        }
        /* Submit one trailing slot to carry the completed-window watermark, without adding a
           wait to production profiling. Tests explicitly wait for their own final submission. */
        mtl_sub_begin(); (void)vk_cb(); f = mtl_sub_commit();
        CHECK(mtl_wait(f, 5000000000ull), "GPU profile: 301 frames and trailing slot finish");
        int correct = 1; for (uint32_t i = 0; i < 64; i++) if (result[i] != (i ^ 0x12345678u)) correct = 0;
        CHECK(correct, "GPU profile: compute dispatch writes all 64 output words");
    }
    /* NVN ClearTexture: only the region of the selected level changes, and only the masked channels. Before the region
       path these cleared the whole level and every channel. */
    {
        tex_t ct = rt; ct.d.levels = 2; nvn_tex_layout(&ct.d, &ct.L); ct.mem = gpu_pool + 0xa00000; ct.serial = 30101;
        uint8_t *back = cpu_pool + 0x700000; memset(back, 0xcd, 2 * 64 * 64 * 4);
        view_t level1 = {.set_levels = 1, .base_level = 1, .nlevels = 1};
        p_clear_tex all = {.tex = &ct, .r = {0, 0, 0, 64, 64, 1}, .mask = 0xf, .c = {.f = {0, 0, 1, 1}}};
        p_clear_tex all1 = all; all1.view = level1; all1.has_view = 1; all1.r = (NVNcopyRegion){0, 0, 0, 32, 32, 1};
        p_clear_tex box = all; box.r = (NVNcopyRegion){8, 4, 0, 16, 20, 1}; box.c.f[0] = 1; box.c.f[2] = 0;
        p_clear_tex green = all; green.r = (NVNcopyRegion){40, 40, 0, 10, 12, 1}; green.mask = 0x2;
        green.c.f[1] = 1; green.c.f[2] = 0; green.c.f[3] = 0;
        p_clear_tex box1 = all1; box1.r = (NVNcopyRegion){4, 4, 0, 8, 8, 1}; box1.mask = 0x9;
        box1.c.f[0] = 1; box1.c.f[1] = 1; box1.c.f[2] = 0; box1.c.f[3] = 0.5f;
        p_clear_tex none = box; none.mask = 0; none.r = all.r;
        p_copy_b2t c0 = {.src = (uint64_t)(uintptr_t)back, .tex = &ct, .r = {0, 0, 0, 64, 64, 1}};
        p_copy_b2t c1 = {.src = (uint64_t)(uintptr_t)(back + 64 * 64 * 4), .tex = &ct, .view = level1, .has_view = 1, .r = {0, 0, 0, 32, 32, 1}};
        uint64_t clears0[2], clears1[2]; vk_draw_test_clear_counts(clears0);
        op(OP_CLEAR_TEX, &all, sizeof all); op(OP_CLEAR_TEX, &all1, sizeof all1); op(OP_CLEAR_TEX, &box, sizeof box);
        op(OP_CLEAR_TEX, &green, sizeof green); op(OP_CLEAR_TEX, &box1, sizeof box1); op(OP_CLEAR_TEX, &none, sizeof none);
        op(OP_COPY_T2B, &c0, sizeof c0); op(OP_COPY_T2B, &c1, sizeof c1);
        CHECK(mtl_wait(run(), 5000000000ull), "region/masked texture clears complete");
        vk_draw_test_clear_counts(clears1);
        CHECK(clears1[0] - clears0[0] == 3 && clears1[1] - clears0[1] == 2,
            "region/mask clear path taken for 3 regions and 2 masks (got %llu/%llu)",
            (unsigned long long)(clears1[0] - clears0[0]), (unsigned long long)(clears1[1] - clears0[1]));
        int bad0 = 0, bad1 = 0;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            const uint8_t *p = back + 4 * (y * 64 + x);
            int in_box = x >= 8 && x < 24 && y >= 4 && y < 24, in_green = x >= 40 && x < 50 && y >= 40 && y < 52;
            int r = in_box ? 255 : 0, g = in_green ? 255 : 0, b = in_box ? 0 : 255;
            bad0 += p[0] != r || p[1] != g || p[2] != b || p[3] != 255;
        }
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
            const uint8_t *p = back + 64 * 64 * 4 + 4 * (y * 32 + x);
            int in = x >= 4 && x < 12 && y >= 4 && y < 12;
            bad1 += p[0] != (in ? 255 : 0) || p[1] != 0 || p[2] != 255 || abs(p[3] - (in ? 128 : 255)) > 1;
        }
        CHECK(!bad0, "level-0 region and green-only clears keep every other texel and channel (%d wrong)", bad0);
        CHECK(!bad1, "level-1 red+alpha region clear keeps level 0, other texels and green/blue (%d wrong)", bad1);
    }
    /* TOTK_VK_FEEDBACK_CUT: a pass rendering level 1 of a texture while sampling the same texture (one mip-generation
       step) gets a sampled view of level 0 only; a pass rendering level 0 leaves the view whole. The rendered level-1
       texels (LOD 0 reads) are identical with the switch on and off; only the counters differ. */
    {
        const char *cut_env = getenv("TOTK_VK_FEEDBACK_CUT"); int cut_on = cut_env && atoi(cut_env) == 1;
        tex_t mt = rt; mt.d.levels = 2; nvn_tex_layout(&mt.d, &mt.L); mt.mem = gpu_pool + 0xb00000; mt.serial = 30102;
        mtl_reg_tex(3, &mt, NULL);
        uint8_t *back = cpu_pool + 0x780000; memset(back, 0xcd, 32 * 32 * 4);
        view_t level0 = {.set_levels = 1, .base_level = 0, .nlevels = 1}, level1 = {.set_levels = 1, .base_level = 1, .nlevels = 1};
        p_clear_tex fill0 = {.tex = &mt, .view = level0, .has_view = 1, .r = {0, 0, 0, 64, 64, 1}, .mask = 0xf, .c = {.f = {0.25f, 0.5f, 0.75f, 1}}};
        p_clear_tex fill1 = {.tex = &mt, .view = level1, .has_view = 1, .r = {0, 0, 0, 32, 32, 1}, .mask = 0xf, .c = {.f = {0, 0, 0, 0}}};
        const float full[] = {-1, -1, 3, -1, -1, 3}; memcpy(cpu_pool + 0x202000, full, sizeof full);
        p_bind_vbuf full_vb = vb; full_vb.r[0].address = (uint64_t)(uintptr_t)(cpu_pool + 0x202000);
        fub[0] = 1; fub[1] = 0.5f; fub[2] = 1; fub[3] = 1;
        p_set_rt mip1 = {.n = 1, .color = {&mt}, .cview = {level1}, .has_cview = {1}}, mip0 = {.n = 1, .color = {&mt}, .cview = {level0}, .has_cview = {1}};
        p_bind_handles sampled = th; sampled.h[0] = 3 | (1u << 20);
        p_viewports half = vp; half.v[0][2] = 32; half.v[0][3] = 32;
        p_scissor all = {0, 0, 64, 64}, corner = {0, 0, 4, 4};
        p_copy_b2t c1 = {.src = (uint64_t)(uintptr_t)back, .tex = &mt, .view = level1, .has_view = 1, .r = {0, 0, 0, 32, 32, 1}};
        uint64_t fb0[2], fb1[2], fb2[2]; vk_draw_test_feedback_counts(fb0);
        op(OP_CLEAR_TEX, &fill0, sizeof fill0); op(OP_CLEAR_TEX, &fill1, sizeof fill1);
        op(OP_SCISSOR, &all, sizeof all); op(OP_SET_RT, &mip1, sizeof mip1);
        op(OP_BIND_PROGRAM, &bp, sizeof bp); op(OP_BIND_VATTR, &va, sizeof va); op(OP_BIND_VSTREAM, &vs, sizeof vs);
        op(OP_BIND_VBUF, &full_vb, sizeof full_vb); op(OP_BIND_UBO, &u0, sizeof u0); op(OP_BIND_UBO, &u1, sizeof u1);
        op(OP_BIND_TEX, &sampled, sizeof sampled); op(OP_VIEWPORTS, &half, sizeof half); op(OP_DRAW, &dw, sizeof dw);
        op(OP_COPY_T2B, &c1, sizeof c1);
        CHECK(mtl_wait(run(), 5000000000ull), "mip-generation step with same-texture sampling completes");
        vk_draw_test_feedback_counts(fb1);
        int bad = 0;
        for (int i = 0; i < 32 * 32; i++) {
            const uint8_t *p = back + 4 * i;
            bad += abs(p[0] - 64) > 2 || abs(p[1] - 64) > 2 || abs(p[2] - 191) > 2 || p[3] != 255;
        }
        CHECK(!bad, "level 1 renders level 0's texels, switch %s (%d wrong)", cut_on ? "on" : "off", bad);
        CHECK(cut_on ? fb1[0] > fb0[0] && fb1[1] == fb0[1] : fb1[0] == fb0[0] && fb1[1] == fb0[1],
            "feedback cut %s: %llu views cut, %llu left whole", cut_on ? "on" : "off",
            (unsigned long long)(fb1[0] - fb0[0]), (unsigned long long)(fb1[1] - fb0[1]));
        op(OP_SCISSOR, &corner, sizeof corner); op(OP_SET_RT, &mip0, sizeof mip0);
        op(OP_VIEWPORTS, &vp, sizeof vp); op(OP_DRAW, &dw, sizeof dw);
        CHECK(mtl_wait(run(), 5000000000ull), "level-0 draw sampling its own base level completes");
        vk_draw_test_feedback_counts(fb2);
        CHECK(fb2[0] == fb1[0] && (cut_on ? fb2[1] > fb1[1] : fb2[1] == fb1[1]),
            "a view based at the attached level is never cut (%llu cut, %llu left whole)",
            (unsigned long long)(fb2[0] - fb1[0]), (unsigned long long)(fb2[1] - fb1[1]));
    }
    { void vk_mem_report(void); vk_mem_report(); }   /* the memory line runs */
#ifdef TOTK_FRAME_TIMELINE_TEST
    if (totk_ft_on()) {
        totk_ft_work_counts total = {{0}};
        uint64_t last = totk_ft_current(), first = last >= FT_RING ? last - FT_RING + 1 : 1;
        for (uint64_t frame = first; frame <= last; frame++) {
            totk_ft_record record;
            if (!totk_ft_snapshot(frame, &record)) continue;
            for (unsigned c = 0; c < FT_COUNT_N; c++) total.value[c] += record.work.value[c];
        }
        CHECK(!totk_ft_work_frame(), "timeline counters leave no walking owner after commit");
        CHECK(total.value[FT_COUNT_DRAWS] && total.value[FT_COUNT_PASSES] && total.value[FT_COUNT_CHUNKS],
              "timeline counts real recorded draws, passes and Vulkan slots");
        CHECK(total.value[FT_COUNT_PIPE_CREATES] && total.value[FT_COUNT_DESC_CREATES],
              "timeline captures pipeline and descriptor creations, including worker owners");
        CHECK(total.value[FT_COUNT_TEX_UPLOADS] && total.value[FT_COUNT_TEX_REALIZATIONS],
              "timeline captures actual texture uploads and realizations");
        CHECK(total.value[FT_COUNT_LAG_VERTEX_MISS] <= total.value[FT_COUNT_LAG_VERTEX] &&
              total.value[FT_COUNT_LAG_INDEX_MISS] <= total.value[FT_COUNT_LAG_INDEX] &&
              total.value[FT_COUNT_LAG_UNIFORM_MISS] <= total.value[FT_COUNT_LAG_UNIFORM] &&
              total.value[FT_COUNT_LAG_STORAGE_MISS] <= total.value[FT_COUNT_LAG_STORAGE],
              "timeline lag memo misses are subsets of their decision counts");
        printf("frame-tl fixture:");
#define FT_FIXTURE_COUNTER(symbol, label) printf(" " #label "=%llu", (unsigned long long)total.value[FT_COUNT_##symbol]);
        FT_COUNTERS(FT_FIXTURE_COUNTER)
#undef FT_FIXTURE_COUNTER
        printf("\n");
    }
#endif
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails != 0;
}

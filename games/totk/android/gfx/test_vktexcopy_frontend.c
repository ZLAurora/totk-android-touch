/* Exercise the real static NVN recording wrappers, not a second diagnostic implementation.
   Section GC keeps the unrelated game/runtime entry points out of the host draw fixture. */
/* This fixture invokes wrappers directly: it has no guest thunk table or async replay queue.
   Do not root the frontend's application constructor or optional queue-drain bridge. */
#define constructor unused
#define nvn_x_drain test_unused_nvn_x_drain
#include "../../third_party/d2r_gfx/gfx/nvn/hle_nvn.c"
#undef nvn_x_drain
#undef constructor

/* The host fixture has no loaded guest modules. Its caller is therefore explicitly unknown. */
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    (void)pc; (void)name; (void)off; return 0;
}

void test_texcopy_initialize(tex_t *t, const nvn_texdesc *desc, const pool_t *pool, ptrdiff_t off) {
    tex_builder_t builder = {.d = *desc, .pool = (pool_t *)pool, .off = off};
    memset(t, 0, sizeof *t);
    h_nvnTextureInitialize((NVNtexture *)t, (const NVNtextureBuilder *)&builder);
}

uint64_t test_texcopy_submit(const tex_t *src, const tex_t *dst, uint8_t *readback,
        const float rgba[4], uint64_t *replay_sub) {
    extern uint64_t vk_submit_serial(void);
    NVNcommandBuffer commands;
    const NVNtexture *target = (const NVNtexture *)src;
    NVNcopyRegion region = {0, 0, 0, (int)src->d.width, (int)src->d.height, 1};
    h_nvnCommandBufferInitialize(&commands, NULL);
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferSetRenderTargets(&commands, 1, &target, NULL, NULL, NULL);
    h_nvnCommandBufferSetScissor(&commands, 0, 0, region.width, region.height);
    h_nvnCommandBufferClearColor(&commands, 0, rgba, 0xf);
    h_nvnCommandBufferCopyTextureToTexture(&commands, (const NVNtexture *)src, NULL, &region,
        (const NVNtexture *)dst, NULL, &region, 0);
    h_nvnCommandBufferCopyTextureToBuffer(&commands, (const NVNtexture *)dst, NULL, &region,
        (NVNbufferAddress)(uintptr_t)readback, 0);
    h_nvnCommandBufferSetRenderTargets(&commands, 0, NULL, NULL, NULL, NULL);
    /* Submit the recording directly, as the other backend fixtures do. No guest queue, control-memory
       lifetime or async frontend thread is needed to exercise these actual recording entry points. */
    put_hdr(cmd_alloc(CB(&commands), sizeof(op_hdr)), OP_END, sizeof(op_hdr));
    mtl_sub_begin(); *replay_sub = vk_submit_serial(); mtl_exec(CB(&commands)->seg);
    uint64_t ticket = mtl_sub_commit();
    h_nvnCommandBufferFinalize(&commands);
    return ticket;
}

void test_texcopy_release(const tex_t *t) {
    tex_t copy = *t;
    live_del(&copy); mtl_tex_release(&copy);
}

/* TOTK_NVN_CALL_NOCOPY: the production command probes in both arms, a call / release / EndRecording race on two threads
   (run it under ThreadSanitizer) and a CallCommands + EndRecording microbenchmark. Returns the number of failures. */
#include <pthread.h>
typedef struct { NVNcommandBuffer callee, caller; uint8_t ctl_callee[256], ctl_caller[256]; pthread_barrier_t bar; int rounds, bad; } call_race;
static void *call_race_release(void *arg) {
    call_race *r = arg;
    for (int i = 0; i < r->rounds; i++) {
        pthread_barrier_wait(&r->bar);   /* the callee was called: retire it while the caller ends */
        h_nvnCommandBufferAddControlMemory(&r->callee, r->ctl_callee, sizeof r->ctl_callee);
        pthread_barrier_wait(&r->bar);
    }
    return NULL;
}
static int call_body_has(NVNcommandHandle h, int x) {
    for (const uint8_t *p = (const uint8_t *)(uintptr_t)h;;) {
        op_hdr g; memcpy(&g, p, sizeof g);
        if (g.op == OP_END) return 0;
        if (g.op == OP_SCISSOR) { p_scissor s; memcpy(&s, p + 8, sizeof s); if (s.x == x) return 1; }
        p += g.size;
    }
}
void runtime_describe_address(uint64_t a, char *out, size_t n) { snprintf(out, n, "%#llx", (unsigned long long)a); }   /* TOTK_CHAIN_LOG only */
int test_nvn_call_nocopy(int nocopy, int rounds) {
    dm_nvn_call_nocopy_set(nocopy);
    dm_nvn_probe_commands(); dm_nvn_probe_inline_bulk();   /* the deferred-bind probe needs the queue/runtime, not linked here */
    size_t copies0 = g_ncopies; uint64_t bytes0 = g_copy_bytes; int bad = 0;
    static call_race r; memset(&r, 0, sizeof r); r.rounds = rounds;
    uint8_t commands[4096]; pool_t pool = {.mem = commands};
    NVNcommandBuffer *both[] = {&r.callee, &r.caller}; uint8_t *ctl[] = {r.ctl_callee, r.ctl_caller};
    for (int i = 0; i < 2; i++) {
        h_nvnCommandBufferInitialize(both[i], NULL); h_nvnCommandBufferAddControlMemory(both[i], ctl[i], 256);
        h_nvnCommandBufferAddCommandMemory(both[i], (NVNmemoryPool *)&pool, 0, sizeof commands);
    }
    pthread_barrier_init(&r.bar, NULL, 2); pthread_t t; pthread_create(&t, NULL, call_race_release, &r);
    for (int i = 0; i < rounds; i++) {
        h_nvnCommandBufferBeginRecording(&r.callee); h_nvnCommandBufferSetScissor(&r.callee, 1000 + i, 1, 2, 3);
        NVNcommandHandle callee = h_nvnCommandBufferEndRecording(&r.callee);
        h_nvnCommandBufferBeginRecording(&r.caller); h_nvnCommandBufferSetScissor(&r.caller, 7, 7, 7, 7);
        h_nvnCommandBufferCallCommands(&r.caller, 1, &callee);
        pthread_barrier_wait(&r.bar);
        NVNcommandHandle caller = h_nvnCommandBufferEndRecording(&r.caller);   /* races the callee's release */
        pthread_barrier_wait(&r.bar);
        bad += !call_body_has(caller, 1000 + i) || !call_body_has(caller, 7);
        h_nvnCommandBufferAddControlMemory(&r.caller, r.ctl_caller, sizeof r.ctl_caller);   /* the caller goes too */
    }
    pthread_join(t, NULL); pthread_barrier_destroy(&r.bar);
    /* An abandoned recording drops the references its markers hold (BeginRecording again, then Finalize). */
    h_nvnCommandBufferBeginRecording(&r.callee); h_nvnCommandBufferSetScissor(&r.callee, 5, 5, 5, 5);
    NVNcommandHandle callee = h_nvnCommandBufferEndRecording(&r.callee);
    h_nvnCommandBufferBeginRecording(&r.caller); h_nvnCommandBufferCallCommands(&r.caller, 1, &callee);
    h_nvnCommandBufferBeginRecording(&r.caller); h_nvnCommandBufferCallCommands(&r.caller, 1, &callee);
    h_nvnCommandBufferAddControlMemory(&r.callee, r.ctl_callee, sizeof r.ctl_callee);
    for (int i = 0; i < 2; i++) h_nvnCommandBufferFinalize(both[i]);
    if (g_ncopies != copies0 || g_copy_bytes != bytes0) bad++;
    printf("%s: TOTK_NVN_CALL_NOCOPY=%d command probes, %d call/release/EndRecording races, abandoned markers, no copy leak (%d bad)\n",
        bad ? "FAILED" : "ok", nocopy, rounds, bad);
    return bad;
}
/* 64 called lists of ~2 KiB (scissors and uniform updates) inlined into one caller, then its EndRecording. */
double test_nvn_call_bench(int nocopy, int frames, size_t *bytes_out) {
    enum { LISTS = 64 };
    dm_nvn_call_nocopy_set(nocopy);
    static NVNcommandBuffer lists[LISTS], caller; static uint8_t ctl[LISTS + 1][256], commands[4096];
    pool_t pool = {.mem = commands}; NVNcommandHandle h[LISTS]; uint64_t payload[8] = {0};
    for (int i = 0; i <= LISTS; i++) {
        NVNcommandBuffer *c = i < LISTS ? &lists[i] : &caller;
        h_nvnCommandBufferInitialize(c, NULL); h_nvnCommandBufferAddControlMemory(c, ctl[i], 256);
        h_nvnCommandBufferAddCommandMemory(c, (NVNmemoryPool *)&pool, 0, sizeof commands);
    }
    for (int i = 0; i < LISTS; i++) {
        h_nvnCommandBufferBeginRecording(&lists[i]);
        for (int j = 0; j < 40; j++) { h_nvnCommandBufferSetScissor(&lists[i], i, j, 1, 1); if (j % 4 == 0) h_nvnCommandBufferUpdateUniformBuffer(&lists[i], 0x1000, 64, 0, 64, payload); }
        h[i] = h_nvnCommandBufferEndRecording(&lists[i]);
    }
    struct timespec a, b; size_t bytes = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int f = 0; f < frames; f++) {
        h_nvnCommandBufferBeginRecording(&caller);
        for (int i = 0; i < LISTS; i++) h_nvnCommandBufferCallCommands(&caller, 1, &h[i]);
        NVNcommandHandle done = h_nvnCommandBufferEndRecording(&caller);
        bytes = copy_of(done)->body_bytes;
        h_nvnCommandBufferAddControlMemory(&caller, ctl[LISTS], 256);   /* release this frame's copy */
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    for (int i = 0; i <= LISTS; i++) {
        NVNcommandBuffer *c = i < LISTS ? &lists[i] : &caller;
        h_nvnCommandBufferAddControlMemory(c, ctl[i], 256); h_nvnCommandBufferFinalize(c);
    }
    *bytes_out = bytes;
    return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / frames;
}

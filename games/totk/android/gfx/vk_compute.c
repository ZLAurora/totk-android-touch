/* Compute is never skipped for compilation. Only handles cross from this worker
 * to replay; shader/module/layout maps stay startup/recording-thread owned. */
#include "vk_int.h"
#include "vk_compute_store.h"
#include "vk_overlay.h"
#include "nvn_hmap.h"
#include "dm_wait_stats.h"
#include "../../runtime/fps_auto_runtime.h"
#include "../../runtime/fps_adaptive_runtime.h"
#include "../../runtime/frame_timeline.h"
#include <sys/resource.h>
#include <time.h>
#include <sys/stat.h>

enum { CP_IDLE, CP_QUEUED, CP_BUILDING, CP_READY, CP_FAILED };
typedef struct compute_entry {
    vk_shader *shader;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    int state, learned;
    struct compute_entry *next;
} compute_entry;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static nvn_hmap pipes = {.klen = COMPUTE_KEY_BYTES};
static compute_entry *head, *tail;
static pthread_t thread;
static int started, running, stopping, stats_on, dirty;
static uint64_t save_after;
static vk_lend lend;                      /* mu: the worker and any nice a dispatch join lent it */
static compute_entry *lend_for;           /* mu: the joined entry whose publication ends the loan */
static char path[1100];
static char (*history)[COMPUTE_KEY_BYTES];
static uint32_t count;
static compute_header identity;
static _Atomic uint64_t blocking_ns, build_ns;
static struct { uint64_t background, recording, joins, ready, total, maximum, failures; } window;
#ifdef TOTK_VK_TESTS
static unsigned waiters;
void vk_compute_test_counts(uint64_t out[4]) {
    pthread_mutex_lock(&mu);
    out[0] = window.recording; out[1] = window.background; out[2] = count; out[3] = waiters;
    pthread_mutex_unlock(&mu);
}
#endif

uint64_t vk_compute_pipe_cum_ns(void) { return atomic_load_explicit(&blocking_ns, memory_order_relaxed); }
uint64_t vk_compute_pipe_build_ns(void) { return atomic_load_explicit(&build_ns, memory_order_relaxed); }
static void enqueue(compute_entry *e) { /* mu; exactly one builder owns each entry */
    e->state = CP_QUEUED; e->next = NULL;
    if (tail) tail->next = e; else head = e;
    tail = e; pthread_cond_broadcast(&cv);
}
static void promote(compute_entry *e) { /* mu; active dispatch outranks speculative/learned keys */
    if (e->state != CP_QUEUED || head == e) return;
    compute_entry *previous = head;
    while (previous && previous->next != e) previous = previous->next;
    if (!previous) return;
    previous->next = e->next; if (tail == e) tail = previous;
    e->next = head; head = e;
}
static void build(compute_entry *e, int background) {
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = e->shader->mod, .pName = "main"}, .layout = e->layout};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
        .requiredSubgroupSize = e->shader->required_subgroup_size};
    if (subgroup.requiredSubgroupSize) ci.stage.pNext = &subgroup;
    VkPipeline p = VK_NULL_HANDLE; uint64_t began = host_now_ns();
    VkResult result = vk_pcache_compute(&ci, &p);
    uint64_t elapsed = host_now_ns() - began;
    if (!background) vk_overlay_note_compile(began + elapsed, elapsed);
    if (!background && dm_wait_stats_enabled()) atomic_fetch_add_explicit(&build_ns, elapsed, memory_order_relaxed);
    if (!result) {
        if (!background && (runtime_android_fps_auto_active() || runtime_fps_adaptive_selected()))
            runtime_android_fps_auto_pipeline(host_now_ns());
        totk_ft_count(FT_COUNT_PIPE_CREATES, 1);
    } else {
        if (p) vkDestroyPipeline(V.dev, p, NULL);
        p = VK_NULL_HANDLE;
        RED(8, "compute pipeline %s: %d", e->shader->key, result);
    }
    pthread_mutex_lock(&mu);
    e->pipeline = p; e->state = result ? CP_FAILED : CP_READY;
    vk_lend done = {0};
    if (background && lend.lent && lend_for == e) { done = lend; lend.lent = 0; lend_for = NULL; }
    if (stats_on) {
        if (background) window.background++; else window.recording++;
        window.failures += result != VK_SUCCESS;
        LOGV("[compute-stats] create source=%s key=%s spv_bytes=%zu local=%u,%u,%u build_ms=%.3f result=%d",
            background ? "background" : "recording", e->shader->key, e->shader->spv_bytes,
            e->shader->local[0], e->shader->local[1], e->shader->local[2], elapsed / 1e6, result);
    }
    pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    vk_pipe_boost_restore(&done);
}
static void save(void) { /* mu held; payload snapshot and fsync never on recording */
    char (*copy)[COMPUTE_KEY_BYTES] = count ? malloc((size_t)count * COMPUTE_KEY_BYTES) : NULL;
    if (count && !copy) return;
    uint32_t n = count;
    if (n) memcpy(copy, history, (size_t)n * COMPUTE_KEY_BYTES);
    dirty = 0;
    pthread_mutex_unlock(&mu);
    int ok = compute_history_save(path, identity, (const char (*)[COMPUTE_KEY_BYTES])copy, n);
    if (stats_on || !ok) LOGV("[compute-prewarm] %s %u keys: %s", ok ? "saved" : "failed saving", n, path);
    free(copy); pthread_mutex_lock(&mu);
}
static void *worker(void *unused) {
    (void)unused; totk_thread_start("totk-compute", NULL);
    setpriority(PRIO_PROCESS, 0, VK_WORKER_NICE);
    pthread_mutex_lock(&mu);
    lend.tid = vk_thread_tid();
    for (;;) {
        if (head) {
            compute_entry *e = head; head = e->next; if (!head) tail = NULL;
            e->next = NULL; e->state = CP_BUILDING;
            pthread_mutex_unlock(&mu); build(e, 1); pthread_mutex_lock(&mu); continue;
        }
        if (dirty && (stopping || host_now_ns() >= save_after)) save();
        if (stopping) break;
        if (dirty) {
            struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec++;
            pthread_cond_timedwait(&cv, &mu, &until);
        } else pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu); return NULL;
}
void vk_compute_prepare(vk_shader *cs) {
    if (!cs || cs->stage != 5) return;
    /* All callers are startup/replay. Precreate layouts here, never on worker. */
    pthread_mutex_lock(&mu);
    compute_entry *e = nvn_hm_get(&pipes, cs->key);
    if (!e) {
        e = calloc(1, sizeof *e); if (!e) DIE("compute pipeline entry allocation");
        e->shader = cs; e->layout = vk_layout_of(cs, NULL);
        nvn_hm_put(&pipes, cs->key, e);
        if (running) enqueue(e);
    }
    pthread_mutex_unlock(&mu);
}
void vk_compute_stop(void) {
    pthread_mutex_lock(&mu);
    if (!running || stopping) { pthread_mutex_unlock(&mu); return; }
    stopping = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&mu); running = 0; pthread_mutex_unlock(&mu);
}
void vk_compute_start(void) { /* Vulkan device ready, before replay */
    if (started) return;
    started = 1;
    const char *env = getenv("TOTK_COMPUTE_STATS"); stats_on = env && atoi(env);
    vk_shader_cache_path("state/vk-compute-used.bin", path, sizeof path);
    identity.vendor = V.props.vendorID; identity.device = V.props.deviceID;
    memcpy(identity.uuid, V.props.pipelineCacheUUID, sizeof identity.uuid);
    char (*keys)[COMPUTE_KEY_BYTES] = NULL;
    mkdir("state", 0700);
    int loaded = compute_history_load(path, identity, &keys);
    history = calloc(COMPUTE_KEYS_MAX, COMPUTE_KEY_BYTES);
    if (!history) DIE("compute history allocation");
    if (loaded > 0) { count = (uint32_t)loaded; memcpy(history, keys, (size_t)count * COMPUTE_KEY_BYTES); }
    free(keys);
    if (loaded < 0) LOGV("[compute-prewarm] ignored %s: corrupt/incomplete or incompatible version/device/driver", path);
    running = !pthread_create(&thread, NULL, worker, NULL);
    if (running) atexit(vk_compute_stop);
    else LOGV("[compute-prewarm] worker unavailable; dispatch compiles synchronously");
    unsigned missing = 0;
    for (uint32_t i = 0; i < count; i++) {
        vk_shader *cs = vk_shader_seed(history[i], 5, 0);
        if (cs) {
            vk_compute_prepare(cs);
            pthread_mutex_lock(&mu);
            compute_entry *e = nvn_hm_get(&pipes, cs->key); e->learned = 1;
            pthread_mutex_unlock(&mu);
        } else missing++;
    }
    LOGV("[compute-prewarm] loaded %u keys, missing seed %u; worker=%d (independent of graphics key lists)", count, missing, running);
}
VkPipeline vk_compute_pipeline(vk_shader *cs, VkPipelineLayout *layout) {
    vk_compute_prepare(cs);
    pthread_mutex_lock(&mu);
    compute_entry *e = nvn_hm_get(&pipes, cs->key);
    if (!e->learned) {
        e->learned = 1;
        /* Bounded exact-key set. Full histories replace the oldest discovered key. */
        uint32_t i;
        for (i = 0; i < count; i++) if (!memcmp(history[i], cs->key, COMPUTE_KEY_BYTES)) break;
        if (i == count) {
            if (count == COMPUTE_KEYS_MAX) { memmove(history, history + 1, (COMPUTE_KEYS_MAX - 1) * COMPUTE_KEY_BYTES); i--; }
            else count++;
            memcpy(history[i], cs->key, COMPUTE_KEY_BYTES);
            if (!dirty) save_after = host_now_ns() + UINT64_C(5000000000);
            dirty = 1; pthread_cond_signal(&cv);
        }
    }
    *layout = e->layout;
    uint64_t began = 0;
    if (e->state != CP_READY) {
        began = host_now_ns();
        promote(e);
        if (stats_on && (e->state == CP_QUEUED || e->state == CP_BUILDING)) window.joins++;
        int joined = e->state == CP_QUEUED || e->state == CP_BUILDING, refused = 0;
        while (e->state == CP_QUEUED || e->state == CP_BUILDING) {
            /* The single worker may still be finishing an earlier entry; the loan covers it and ends with this one. */
            if (!refused) { int lent = vk_pipe_boost_lend(&lend); if (lent > 0) lend_for = e; else if (lent < 0) refused = 1; }
#ifdef TOTK_VK_TESTS
            waiters++;
#endif
            pthread_cond_wait(&cv, &mu);
#ifdef TOTK_VK_TESTS
            waiters--;
#endif
        }
        if (joined) vk_pipe_boost_waited(host_now_ns() - began);
        if (e->state != CP_READY) {
            /* Worker startup/build failure is not permission to skip a dispatch. */
            e->state = CP_BUILDING;
            pthread_mutex_unlock(&mu); build(e, 0); pthread_mutex_lock(&mu);
        }
        uint64_t elapsed = host_now_ns() - began;
        atomic_fetch_add_explicit(&blocking_ns, elapsed, memory_order_relaxed);
        if (stats_on) { window.total += elapsed; if (elapsed > window.maximum) window.maximum = elapsed; }
    } else if (stats_on) window.ready++;
    VkPipeline pipeline = e->pipeline;
    pthread_mutex_unlock(&mu); return pipeline;
}
void vk_compute_report(void) {
    pthread_mutex_lock(&mu);
    if (stats_on) {
        LOGV("[compute-stats] 300-present created_recording=%llu created_background=%llu ready_hits=%llu joins=%llu block_total=%.3fms block_max=%.3fms failures=%llu learned=%u",
            (unsigned long long)window.recording, (unsigned long long)window.background,
            (unsigned long long)window.ready, (unsigned long long)window.joins,
            window.total / 1e6, window.maximum / 1e6, (unsigned long long)window.failures, count);
        memset(&window, 0, sizeof window);
    }
    pthread_mutex_unlock(&mu);
}

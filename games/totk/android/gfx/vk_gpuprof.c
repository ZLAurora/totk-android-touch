/* Opt-in GPU timestamps. Slot lifetime/publication is supplied by nvn_vk.c's fence
   queue. Deferred pass-end tokens retain immutable query coordinates, so recorder
   assembly never touches the walking thread's current pass. */
#include "vk_int.h"
#include "vk_gpuprof_time.h"
#include "../../runtime/thread_place.h"
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>

#define GP_RECORDS 2048
#define GP_GROUPS 16384
#define GP_WINDOW 300
#define GP_SEGMENTS 64
#define GP_SPAN_BASE (GP_RECORDS * 2)
#define GP_PRE_QUERY (GP_SPAN_BASE + GP_SEGMENTS * 2 + 1)
#define GP_TIMESTAMPS (GP_PRE_QUERY + 1)
#define GP_HISTOGRAM 272
#define GP_BACKLOG 16
#define GP_CLOSURES 256
enum { GP_IA_VERTICES, GP_IA_PRIMITIVES, GP_VS, GP_CLIP_INVOCATIONS, GP_CLIP_PRIMITIVES, GP_FS, GP_STATS };
#define GP_STATS_MASK (VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT)

typedef struct { uint32_t format, w, h, load, store, bytes; } gp_attachment;
typedef struct {
    gp_attachment a[10];
    uint64_t shader;
    uint32_t w, h, samples, colors, compute;
} gp_key;
typedef struct {
    gp_key key; uint64_t frame, counters[GP_STATS];
    uint32_t draws, instance, reason, detail, stats_q1, stats_count, stats_invalid;
    uint32_t timestamp_q1, timestamp_count, selected;
} gp_record;
typedef struct {
    uint64_t frame;
    uint32_t first, last, first_valid, last_valid, timestamp_count;
} gp_segment;
struct vk_gpuprof_slot {
    VkQueryPool pool, stats;
    uint32_t n, dropped, ns, nt, span_writes;
    uint64_t sealed, sequence;
    uint32_t nsegment, has_pre;
    int discard;
    VkCommandBuffer pre_cb;
    gp_segment segment[GP_SEGMENTS];
    vk_gpuprof_timestamp span_ticks[GP_SEGMENTS * 2 + 1], pre_tick;
    vk_gpuprof_time_domain domain;
    uint8_t timestamp_marks[GP_TIMESTAMPS]; /* counted/invalid once per queried endpoint */
    gp_record rec[GP_RECORDS];
    vk_gpuprof_timestamp ticks[GP_RECORDS * 2];
    uint64_t counters[GP_RECORDS][GP_STATS];
    uint32_t stats_owner[GP_RECORDS];
};
typedef struct {
    gp_key key; uint64_t hash, count, draws, counters[GP_STATS], stats_count, stats_draws;
    uint32_t ordinal, reference_w, reference_h, scene_relative;
    double ms, stats_ms;
    uint64_t sampled, observed_frames, sampled_frames, last_observed_frame, last_sampled_frame;
    uint64_t *histogram; /* window aggregates only, allocated only in sampled mode */
} gp_group;

int vk_gpuprof_on;
static int stats_on, secondary_pass;
static uint32_t sample_budget, sample_previous_passes, sample_selected;
static uint64_t sample_cursor;
static int sample_frame_started;
static vk_gpuprof_slot *current;
static gp_record *open_record;
static VkCommandBuffer open_cb;
typedef struct {
    gp_record *parent; VkCommandBuffer parent_cb, cb;
    int secondary, skipped;
} gp_scope;
static gp_scope scopes[16];
static unsigned scope_n, scope_overflow;
static uint64_t frame = 1;
static uint32_t frame_instance;
static uint64_t recording_sequence; /* increments only at actual slot seal, never cancellation */
/* Aggregation/query/output state below belongs ONLY to totk-gpuprof. */
static gp_group groups[GP_GROUPS];
static struct {
    uint64_t frame, dropped, unavailable_timestamps, invalid_timestamps, timestamp_wraps;
    uint64_t timestamp_count, observed_passes, sampled_passes;
    uint64_t closure_sequence;
    const char *closure_source;
    vk_gpuprof_timestamp first, last;
    vk_gpuprof_time_domain first_domain, last_domain;
    vk_gpuprof_time_status status;
    double measured;
    uint32_t scene_w, scene_h;
    int has_ticks, has_end, failed;
} completed;
static uint64_t last_completed, window_frames;
static double window_span, window_gap;
static uint64_t window_unavailable_timestamps, window_invalid_timestamps, window_invalid_spans, window_valid_span_frames;
static int partial_report;
typedef struct {
    uint64_t frame, dropped; uint32_t n, capacity;
    double span_ms, measured_ms, gap_ms;
    gp_group *pass;
} gp_long_frame;
static gp_long_frame *long_ring;
static gp_long_frame frame_pending; /* one reusable completion-owner frame outside long-frame mode */
static uint32_t game_reference_w, game_reference_h; /* immutable fallback when no scene MRT was observed */
static uint32_t long_frames;
static int window_ordinals;
static double long_ms = 50;
static uint64_t window, total_records, dropped_records, failed_slots, loads, clears, stores, discards;
static uint64_t stats_failures;
static uint64_t window_sample_count, window_observed_passes, window_timestamp_count;
static uint64_t load_bytes, store_bytes, unknown_bytes;
static double total_ms, render_ms, compute_ms;
static const char *const end_names[] = {
    "other", "nvn-barrier", "texture-barrier", "attachment-change", "clear",
    "copy-blit", "query-report", "compute-dispatch", "cpu-wait", "submission-end",
    "uniform-flush", "present", "diagnostic", "deferred-barrier-hazard", "early-submit-split"
};
static FILE *output_file;
static char output_path[PATH_MAX];
static uint64_t output_bytes, output_limit = UINT64_C(128) << 20;
static _Atomic uint64_t output_failures;
static pthread_mutex_t output_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t output_rotation, output_last_frame, output_last_window, output_last_window_end;
static int output_closed;
static char output_launch[96], output_commit[80], output_package[256], output_build[256];
static uint64_t output_version;
static uint64_t report_end;
/* A prepared query lease is never recycled until its existing GPU fence has
   retired and the profiler worker has read it. Game completion only hands off
   the lease: slow formatting/IO cannot hold up renderer slot reuse. */
static pthread_mutex_t background_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t background_cv = PTHREAD_COND_INITIALIZER;
static pthread_t background_thread;
static vk_gpuprof_slot *background_free[GP_BACKLOG], *background_queue[GP_BACKLOG];
static unsigned background_nfree;
static uint64_t background_head, background_tail;
typedef struct { uint64_t frame, sequence; } gp_closure;
static gp_closure background_closures[GP_CLOSURES];
static uint64_t background_close_head, background_close_tail, background_processed_sequence;
static int background_started, background_ready, background_active, background_stop, background_joining, background_joined;
static _Atomic int background_closing;
static _Atomic uint64_t background_dropped_slots, background_queued_slots, background_processed_slots;
static _Atomic uint64_t background_dropped_closures;
static _Atomic uint64_t leases_acquired, leases_submitted, leases_retired, leases_cancelled;
static _Atomic uint64_t queries_submitted, queries_retired, queries_cancelled;
static pthread_mutex_t shutdown_mu = PTHREAD_MUTEX_INITIALIZER;
static int query_drain_timed_out;
#ifdef TOTK_VK_TESTS
static int background_paused;
static uint64_t background_flush_requested, background_flush_done;
#endif
static void background_start(void);
static void background_stop_and_join(void);
static void background_enqueue(vk_gpuprof_slot **owner, int discard);
static uint64_t env_u64(const char *name, uint64_t fallback, uint64_t minimum, uint64_t maximum) {
    const char *e = getenv(name); char *end = NULL;
    if (!e || !*e || *e == '-') return fallback;
    unsigned long long value = strtoull(e, &end, 10);
    return end && !*end && value >= minimum && value <= maximum ? value : fallback;
}
typedef struct { char data[8192]; size_t used; int failed; } gp_json;
static void json_add(gp_json *row, const char *format, ...) {
    if (row->failed) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(row->data + row->used, sizeof row->data - row->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof row->data - row->used) { row->failed = 1; return; }
    row->used += (size_t)n;
}
static gp_json output_header;
static void json_string(gp_json *row, const char *text) {
    if (!text) { json_add(row, "null"); return; }
    json_add(row, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') json_add(row, "\\%c", *p);
        else if (*p < 32) json_add(row, "\\u%04x", *p);
        else json_add(row, "%c", *p);
    }
    json_add(row, "\"");
}
/* Frontend-provided APK facts, before Vulkan startup; never accept environment
   text as an authenticated package/build identity. */
void vk_gpuprof_identity(const char *commit, const char *package, uint64_t version, const char *build) {
    pthread_mutex_lock(&output_mu);
    if (!output_launch[0] && !output_closed) {
        snprintf(output_commit, sizeof output_commit, "%s", commit ? commit : "");
        snprintf(output_package, sizeof output_package, "%s", package ? package : "");
        snprintf(output_build, sizeof output_build, "%s", build ? build : "");
        output_version = version;
    }
    pthread_mutex_unlock(&output_mu);
}
static int output_write_locked(const char *text, size_t n) {
    if (fwrite(text, 1, n, output_file) != n) {
        LOGV("[gpu-profile] JSONL write failed path=%s", output_path);
        fclose(output_file); output_file = NULL; output_failures++; return 0;
    }
    output_bytes += n;
    return 1;
}
static int output_header_locked(void) {
    gp_json row = output_header;
    json_add(&row, ",\"rotation\":%llu}\n", (unsigned long long)output_rotation);
    if (row.failed || row.used >= output_limit) { output_failures++; return 0; }
    return output_write_locked(row.data, row.used);
}
static void output_init(void) {
    char cwd[PATH_MAX]; struct timespec now;
    pthread_mutex_lock(&output_mu);
    if (output_closed) { pthread_mutex_unlock(&output_mu); return; }
    if (!getcwd(cwd, sizeof cwd) || clock_gettime(CLOCK_REALTIME, &now)) {
        LOGV("[gpu-profile] cannot resolve output game directory"); output_failures++; goto done;
    }
    snprintf(output_launch, sizeof output_launch, "%llu-%ld-%09ld",
        (unsigned long long)now.tv_sec, (long)getpid(), now.tv_nsec);
    int n = snprintf(output_path, sizeof output_path, "%s/gpuprof-%s.jsonl", cwd, output_launch);
    if (n < 0 || (size_t)n >= sizeof output_path - 2) { output_failures++; goto done; }
    output_limit = env_u64("TOTK_VK_GPU_PROFILE_MAX_BYTES", output_limit, 4096, UINT64_C(1) << 30);
    gp_json *row = &output_header;
    json_add(row, "{\"schema\":3,\"type\":\"header\",\"launch_id\":"); json_string(row, output_launch);
    json_add(row, ",\"build_commit\":"); json_string(row, output_commit[0] ? output_commit : NULL);
    json_add(row, ",\"build_asset\":"); json_string(row, output_build[0] ? output_build : NULL);
    json_add(row, ",\"build_identity_source\":"); json_string(row, output_commit[0] ? "apk:assets/build.txt" : "unavailable");
    json_add(row, ",\"build_identity_reason\":");
    json_string(row, output_commit[0] ? NULL : "APK build.txt commit unavailable (host fixture or missing asset)");
    json_add(row, ",\"package\":"); json_string(row, output_package[0] ? output_package : NULL);
    json_add(row, ",\"version_code\":");
    if (output_version) json_add(row, "%llu", (unsigned long long)output_version); else json_add(row, "null");
    json_add(row, ",\"apk_sha256\":null,\"apk_sha256_reason\":\"not computed in process; join capture APK hash\","
        "\"profile\":%d,\"stats\":%s,\"ordinals\":%s,\"max_bytes\":%llu,\"retained_files\":4,"
        "\"window_frames\":%d,\"span_limit_ms\":%.0f,\"timestamp_bits\":%u,\"timestamp_period_ns\":%.9f,"
        "\"sample_budget\":%u,\"background_slots\":%u,\"background_closures\":%u,\"background_worker\":\"totk-gpuprof\","
        "\"driver\":{\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,\"driver_version\":%u,\"device_name\":",
        vk_gpuprof_on, stats_on ? "true" : "false", window_ordinals ? "true" : "false",
        (unsigned long long)output_limit, GP_WINDOW, VK_GPUPROF_MAX_FRAME_MS, V.timestamp_bits, V.props.limits.timestampPeriod,
        sample_budget, GP_BACKLOG, GP_CLOSURES, V.props.vendorID, V.props.deviceID, V.props.apiVersion, V.props.driverVersion);
    json_string(row, V.props.deviceName);
    json_add(row, ",\"requested_driver\":"); json_string(row, getenv("TOTK_VK_DRIVER"));
    json_add(row, ",\"binary_sha256\":null,\"binary_sha256_reason\":\"join captured driver artifact\"},\"env\":{");
    static const char *const names[] = {
        "TOTK_VK_DRIVER", "TOTK_VK_GPU_PROFILE", "TOTK_VK_GPU_PROFILE_STATS", "TOTK_VK_GPU_PROFILE_ORDINALS",
        "TOTK_VK_GPU_PROFILE_SAMPLE",
        "TOTK_VK_GPU_PROFILE_LONG_MS", "TOTK_VK_GPU_PROFILE_LONG_FRAMES", "TOTK_VK_GPU_PROFILE_MAX_BYTES",
        "TOTK_VK_REC_THREADS", "TOTK_VK_REC_CHUNK_DRAWS", "TOTK_VK_REC_INLINE_DRAWS", "TOTK_VK_REC_LEAD_DRAWS",
        "TOTK_OPERATION_MODE", "TOTK_RENDER_RES", "TOTK_FPS60", "TOTK_SCENE_SCALE", "TOTK_VK_PRESENT",
        "TU_DEBUG", "TU_PERF"
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        json_add(row, "%s", i ? "," : ""); json_string(row, names[i]); json_add(row, ":"); json_string(row, getenv(names[i]));
    }
    json_add(row, "}");
    output_file = fopen(output_path, "wb");
    if (!output_file) output_failures++;
    else if (!output_header_locked()) { if (output_file) fclose(output_file); output_file = NULL; }
    LOGV("[gpu-profile] JSONL %s path=%s max-bytes=%llu rotations=3 (TOTK_VK_GPU_PROFILE_MAX_BYTES)",
        output_file ? "enabled" : "unavailable", output_path, (unsigned long long)output_limit);
done:
    pthread_mutex_unlock(&output_mu);
}
static int output_row_locked(const char *text) {
    if (!output_file || output_closed) return 0;
    size_t n = strlen(text);
    if (n + output_header.used + 64 > output_limit) { output_failures++; return 0; }
    if (output_bytes + n > output_limit) {
        fclose(output_file); output_file = NULL;
        char from[PATH_MAX], to[PATH_MAX];
        for (int i = 3; i > 0; i--) {
            snprintf(to, sizeof to, "%s.%d", output_path, i);
            if (i == 1) snprintf(from, sizeof from, "%s", output_path);
            else snprintf(from, sizeof from, "%s.%d", output_path, i - 1);
            if (rename(from, to) && access(from, F_OK) == 0) {
                LOGV("[gpu-profile] JSONL rotation failed path=%s", from); output_failures++; return 0;
            }
        }
        output_file = fopen(output_path, "wb"); output_bytes = 0; output_rotation++;
        if (!output_file) { output_failures++; return 0; }
        if (!output_header_locked()) return 0;
    }
    return output_write_locked(text, n);
}
static void output_row(const char *text) {
    pthread_mutex_lock(&output_mu);
    output_row_locked(text);
    pthread_mutex_unlock(&output_mu);
}
static void output_frame(const char *text, uint64_t number) {
    pthread_mutex_lock(&output_mu);
    if (output_row_locked(text)) output_last_frame = number;
    if (output_file) fflush(output_file);
    pthread_mutex_unlock(&output_mu);
}
static void output_window(const char *text, uint64_t sequence, uint64_t end) {
    pthread_mutex_lock(&output_mu);
    if (output_row_locked(text)) { output_last_window = sequence; output_last_window_end = end; }
    if (output_file) fflush(output_file);
    pthread_mutex_unlock(&output_mu);
}
/* Normal close seals/waits renderer work before stopping the reporter. Force-stop
   cannot run this path. Result availability remains independent of fence completion. */
void vk_gpuprof_shutdown(void) {
    if (!vk_gpuprof_on) return; /* no profiler admission, locking or wait on the default path */
    pthread_mutex_lock(&shutdown_mu);
    if (output_closed) { pthread_mutex_unlock(&shutdown_mu); return; }
    atomic_store_explicit(&background_closing, 1, memory_order_release);
    query_drain_timed_out = !vk_gpuprof_renderer_drain(UINT64_C(2000000000));
    background_stop_and_join();
    pthread_mutex_lock(&background_mu);
    uint64_t pending_closures = background_close_tail - background_close_head;
    pthread_mutex_unlock(&background_mu);
    pthread_mutex_lock(&output_mu);
    if (!output_closed && output_file) {
        gp_json row = {0};
        json_add(&row, "{\"schema\":3,\"type\":\"session_end\",\"launch_id\":"); json_string(&row, output_launch);
        uint64_t acquired = atomic_load(&leases_acquired), submitted = atomic_load(&leases_submitted);
        uint64_t retired = atomic_load(&leases_retired), cancelled = atomic_load(&leases_cancelled);
        uint64_t outstanding = acquired - retired - cancelled;
        uint64_t qs = atomic_load(&queries_submitted), qr = atomic_load(&queries_retired);
        uint64_t qc = atomic_load(&queries_cancelled), qo = qs - qr;
        json_add(&row, ",\"clean_end\":true,\"all_queries_retired\":%s,\"last_frame\":%llu,"
            "\"last_window_sequence\":%llu,\"last_window_end\":%llu,\"output_failures\":%llu,"
            "\"background_dropped_slots\":%llu,\"background_dropped_closures\":%llu,\"pending_frame_closures\":%llu,"
            "\"query_drain_timed_out\":%s,"
            "\"query_retirement\":{\"leases_acquired\":%llu,\"leases_submitted\":%llu,\"leases_retired\":%llu,"
            "\"leases_cancelled\":%llu,\"leases_outstanding\":%llu,\"queries_submitted\":%llu,"
            "\"queries_retired\":%llu,\"queries_cancelled\":%llu,\"queries_outstanding\":%llu}}\n",
            !outstanding && !qo ? "true" : "false",
            (unsigned long long)output_last_frame, (unsigned long long)output_last_window,
            (unsigned long long)output_last_window_end, (unsigned long long)atomic_load(&output_failures),
            (unsigned long long)atomic_load(&background_dropped_slots),
            (unsigned long long)atomic_load(&background_dropped_closures), (unsigned long long)pending_closures,
            query_drain_timed_out ? "true" : "false",
            (unsigned long long)acquired, (unsigned long long)submitted, (unsigned long long)retired,
            (unsigned long long)cancelled, (unsigned long long)outstanding,
            (unsigned long long)qs, (unsigned long long)qr, (unsigned long long)qc, (unsigned long long)qo);
        output_row_locked(row.data);
        if (output_file) { fflush(output_file); fclose(output_file); output_file = NULL; }
    }
    output_closed = 1;
    pthread_mutex_unlock(&output_mu);
    pthread_mutex_unlock(&shutdown_mu);
}
static const char *kind_name(const gp_key *key) {
    static const char *const kinds[] = {"render", "compute", "upload", "transfer", "post", "upscale", "present", "framegen"};
    return key->compute < sizeof kinds / sizeof *kinds ? kinds[key->compute] : "other";
}
static uint32_t gcd_u32(uint32_t a, uint32_t b);
static unsigned sample_bucket(double ms) {
    double bucket = ms > 0 ? ceil(log2(ms * 1e6) * 8) : 0;
    return bucket <= 0 ? 0 : bucket >= GP_HISTOGRAM - 1 ? GP_HISTOGRAM - 1 : (unsigned)bucket;
}
static void output_pass(const gp_group *g, const char *type, uint64_t pass_frame) {
    const gp_key *k = &g->key;
    gp_json row = {0};
    uint64_t start = pass_frame ? (pass_frame - 1) / GP_WINDOW * GP_WINDOW + 1 : window * GP_WINDOW + 1;
    uint64_t end = pass_frame ? start + GP_WINDOW - 1 : report_end;
    json_add(&row, "{\"schema\":3,\"type\":\"%s\",\"window_start\":%llu,\"window_end\":%llu,"
        "\"key\":\"%016llx\",\"kind\":\"%s\",\"ordinal\":%u,\"samples\":%u,\"shader\":\"%016llx\","
        "\"gpu_ms\":%.9f,\"count\":%llu,\"draws\":%llu,\"stats_valid\":%llu",
        type, (unsigned long long)start, (unsigned long long)end,
        (unsigned long long)g->hash, kind_name(k), g->ordinal, k->samples, (unsigned long long)k->shader,
        g->ms, (unsigned long long)g->count, (unsigned long long)g->draws, (unsigned long long)g->stats_count);
    if (sample_budget) {
        json_add(&row, ",\"sampling\":{\"budget\":%u,\"observed_count\":%llu,\"sample_count\":%llu,"
            "\"observed_frames\":%llu,\"sampled_frames\":%llu,\"histogram\":[",
            sample_budget, (unsigned long long)g->count, (unsigned long long)g->sampled,
            (unsigned long long)g->observed_frames, (unsigned long long)g->sampled_frames);
        unsigned bucket = sample_bucket(g->ms);
        for (unsigned b = 0; b < GP_HISTOGRAM; b++) json_add(&row, "%s%llu", b ? "," : "",
            (unsigned long long)(g->histogram ? g->histogram[b] : g->sampled && b == bucket));
        json_add(&row, "]}");
    }
    json_add(&row, ",\"render_width\":%u,\"render_height\":%u,\"reference_width\":%u,\"reference_height\":%u,"
        "\"scene_relative\":%s", k->w, k->h, g->reference_w, g->reference_h, g->scene_relative ? "true" : "false");
    if (pass_frame) json_add(&row, ",\"frame\":%llu", (unsigned long long)pass_frame);
    json_add(&row, ",\"attachments\":[");
    int comma = 0;
    for (unsigned j = 0; j < 10; j++) if (k->a[j].format) {
        const gp_attachment *a = &k->a[j];
        uint32_t gw = gcd_u32(a->w, g->reference_w), gh = gcd_u32(a->h, g->reference_h);
        json_add(&row, "%s{\"slot\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"width_num\":%u,\"width_den\":%u,"
            "\"height_num\":%u,\"height_den\":%u,\"load\":%u,\"store\":%u}",
            comma++ ? "," : "", j, a->format, a->w, a->h, a->w / gw, g->reference_w / gw,
            a->h / gh, g->reference_h / gh, a->load, a->store);
    }
    json_add(&row, "]");
    if (g->stats_count) {
        static const char *const names[GP_STATS] = {"ia_vertices", "ia_primitives", "vs_invocations",
            "clipping_invocations", "clipping_primitives", "fs_invocations"};
        json_add(&row, ",\"stats_gpu_ms\":%.9f,\"stats_draws\":%llu,\"stats\":{",
            g->stats_ms, (unsigned long long)g->stats_draws);
        for (unsigned st = 0; st < GP_STATS; st++)
            json_add(&row, "%s\"%s\":%llu", st ? "," : "", names[st], (unsigned long long)g->counters[st]);
        json_add(&row, "},\"ratios\":{\"verts_per_draw\":%.9f,\"prims_per_draw\":%.9f,"
            "\"fragments_per_prim\":%.9f,\"ns_per_vertex\":%.9f,\"ns_per_primitive\":%.9f}",
            g->stats_draws ? g->counters[0] / (double)g->stats_draws : 0,
            g->stats_draws ? g->counters[1] / (double)g->stats_draws : 0,
            g->counters[1] ? g->counters[5] / (double)g->counters[1] : 0,
            g->counters[0] ? g->stats_ms * 1e6 / g->counters[0] : 0,
            g->counters[1] ? g->stats_ms * 1e6 / g->counters[1] : 0);
    }
    json_add(&row, "}\n");
    if (row.failed) output_failures++; else output_row(row.data);
}

static uint64_t hash_bytes(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b) { uint32_t r = a % b; a = b; b = r; }
    return a ? a : 1;
}
static uint64_t key_hash(const gp_key *key) {
    uint64_t h = UINT64_C(14695981039346656037);
    h = hash_bytes(h, &key->shader, sizeof key->shader);
    const uint32_t fields[] = {key->w, key->h, key->samples, key->colors, key->compute};
    h = hash_bytes(h, fields, sizeof fields);
    for (unsigned j = 0; j < 10; j++) {
        const gp_attachment *a = &key->a[j];
        const uint32_t values[] = {a->format, a->w, a->h, a->load, a->store};
        h = hash_bytes(h, values, sizeof values);
    }
    return h;
}
static int key_equal(const gp_key *a, const gp_key *b) {
    if (a->w != b->w || a->h != b->h || a->shader != b->shader || a->samples != b->samples ||
        a->colors != b->colors || a->compute != b->compute) return 0;
    for (unsigned j = 0; j < 10; j++) {
        const gp_attachment *x = &a->a[j], *y = &b->a[j];
        if (x->format != y->format || x->w != y->w || x->h != y->h ||
            x->load != y->load || x->store != y->store) return 0;
    }
    return 1;
}
static gp_long_frame *pending_frame(uint64_t number) {
    return long_ring ? &long_ring[number % long_frames] : &frame_pending;
}
static void capture_record(const gp_record *r, double ms, int stats_valid, int sampled) {
    gp_long_frame *f = pending_frame(r->frame);
    if (f->frame != r->frame) { f->frame = r->frame; f->n = 0; f->dropped = 0; }
    if (f->n == GP_GROUPS) { f->dropped++; completed.dropped++; dropped_records++; return; }
    if (f->n == f->capacity) {
        uint32_t capacity = f->capacity ? f->capacity * 2 : 256;
        gp_group *passes = realloc(f->pass, capacity * sizeof *passes);
        if (!passes) { f->dropped++; completed.dropped++; dropped_records++; return; }
        f->pass = passes; f->capacity = capacity;
    }
    gp_group *g = &f->pass[f->n++];
    *g = (gp_group){.key = r->key, .hash = key_hash(&r->key), .ordinal = r->instance,
        .count = 1, .draws = r->draws, .ms = ms, .sampled = sampled,
        .observed_frames = 1, .sampled_frames = sampled,
        .last_observed_frame = r->frame, .last_sampled_frame = sampled ? r->frame : 0};
    if (stats_valid) {
        memcpy(g->counters, r->counters, sizeof g->counters);
        g->stats_count = 1; g->stats_draws = r->draws; g->stats_ms = ms;
    }
}
static uint64_t long_finish(uint64_t number, double span, double measured, double gap) {
    if (!long_ring) return 0;
    gp_long_frame *f = &long_ring[number % long_frames];
    if (f->frame != number) return 0;
    f->span_ms = span; f->measured_ms = measured; f->gap_ms = gap;
    if (span > long_ms) {
        for (uint32_t i = 0; i < f->n; i++) output_pass(&f->pass[i], "long_frame", number);
        LOGV("[gpu-profile long] frame=%llu span=%.3f measured=%.3f gap=%.3f passes=%u dropped=%llu threshold=%.3f ms",
            (unsigned long long)number, span, measured, gap, f->n, (unsigned long long)f->dropped, long_ms);
    }
    return f->dropped;
}
static unsigned active_colors(const gp_key *key) {
    unsigned colors = 0;
    for (unsigned j = 0; j < 8; j++) colors += key->a[j].format != 0;
    return colors;
}
static void aggregate_sample(const gp_group *sample) {
    const gp_key *key = &sample->key;
    total_records += sample->count; total_ms += sample->ms;
    if (key->compute == 1) compute_ms += sample->ms;
    else if (!key->compute) render_ms += sample->ms;
    for (unsigned j = 0; j < 10; j++) {
        const gp_attachment *a = &key->a[j]; if (!a->format) continue;
        uint64_t bytes = (uint64_t)key->w * key->h * a->bytes * key->samples;
        unknown_bytes += !a->bytes;
        if (a->load == VK_ATTACHMENT_LOAD_OP_LOAD) { loads++; load_bytes += bytes; }
        if (a->load == VK_ATTACHMENT_LOAD_OP_CLEAR) clears++;
        if (a->store == VK_ATTACHMENT_STORE_OP_STORE) { stores++; store_bytes += bytes; } else discards++;
    }
    uint64_t reference = ((uint64_t)sample->reference_w << 32) | sample->reference_h;
    uint32_t ordinal = window_ordinals ? sample->ordinal : 0;
    size_t at = (sample->hash ^ ordinal ^ reference ^ sample->scene_relative) % GP_GROUPS, tries = 0;
    while (groups[at].count && (groups[at].hash != sample->hash || groups[at].ordinal != ordinal ||
        groups[at].reference_w != sample->reference_w || groups[at].reference_h != sample->reference_h ||
        groups[at].scene_relative != sample->scene_relative || !key_equal(&groups[at].key, key)) &&
        ++tries < GP_GROUPS) at = (at + 1) % GP_GROUPS;
    if (tries == GP_GROUPS) { dropped_records++; completed.dropped++; return; }
    gp_group *g = &groups[at];
    if (!g->count) { *g = *sample; g->ordinal = ordinal; }
    else {
        g->count += sample->count; g->draws += sample->draws; g->ms += sample->ms;
        for (unsigned st = 0; st < GP_STATS; st++) g->counters[st] += sample->counters[st];
        g->stats_count += sample->stats_count; g->stats_draws += sample->stats_draws; g->stats_ms += sample->stats_ms;
        g->sampled += sample->sampled;
        if (sample->last_observed_frame != g->last_observed_frame) {
            g->observed_frames++; g->last_observed_frame = sample->last_observed_frame;
        }
        if (sample->sampled && sample->last_sampled_frame != g->last_sampled_frame) {
            g->sampled_frames++; g->last_sampled_frame = sample->last_sampled_frame;
        }
    }
    if (sample_budget && sample->sampled) {
        if (!g->histogram) {
            g->histogram = calloc(GP_HISTOGRAM, sizeof *g->histogram);
            if (!g->histogram) DIE("GPU profiler histogram allocation");
        }
        g->histogram[sample_bucket(sample->ms)]++; window_sample_count++;
    }
}
/* A frame can span several retired slots, and its largest scene MRT can be
   recorded after a shadow/prepass. Resolve the common reference only once the
   complete frame is known; query records cannot borrow retired-slot storage. */
static void finalize_passes(void) {
    gp_long_frame *f = pending_frame(completed.frame);
    if (f->frame != completed.frame) return;
    uint32_t w = completed.scene_w ? completed.scene_w : game_reference_w;
    uint32_t h = completed.scene_h ? completed.scene_h : game_reference_h;
    for (uint32_t i = 0; i < f->n; i++) {
        gp_group *sample = &f->pass[i]; const gp_key *key = &sample->key;
        sample->reference_w = w; sample->reference_h = h;
        sample->scene_relative = !key->compute && key->a[8].format &&
            (active_colors(key) >= 2 || (completed.scene_w && key->w == completed.scene_w && key->h == completed.scene_h));
        aggregate_sample(sample);
    }
}
/* Core VkFormat families are contiguous. Use the VIEW format, not guest storage: a BC
   texture can have an uncompressed attachment view and depth formats can be substituted. */
static uint32_t texel_bytes(VkFormat f) {
    if (f >= VK_FORMAT_R4G4B4A4_UNORM_PACK16 && f <= VK_FORMAT_A1R5G5B5_UNORM_PACK16) return 2;
    if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) return 1;
    if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) return 2;
    if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) return 3;
    if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) return 4;
    if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) return 2;
    if (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) return 4;
    if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) return 6;
    if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT) return 4;
    if (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT) return 12;
    if (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) return 16;
    if (f == VK_FORMAT_D16_UNORM) return 2;
    if (f == VK_FORMAT_S8_UINT) return 1;
    if (f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_X8_D24_UNORM_PACK32 ||
        f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) return 4;
    return 0;
}
static int rank_cmp(const void *a, const void *b) {
    const gp_group *x = *(gp_group *const *)a, *y = *(gp_group *const *)b;
    return x->ms < y->ms ? 1 : x->ms > y->ms ? -1 : x->hash < y->hash ? -1 : x->hash > y->hash;
}
static void report(void) {
    gp_group *rank[GP_GROUPS]; size_t n = 0;
    for (size_t i = 0; i < GP_GROUPS; i++) if (groups[i].count) rank[n++] = &groups[i];
    qsort(rank, n, sizeof *rank, rank_cmp);
    if (!partial_report) report_end = (window + 1) * GP_WINDOW;
    for (size_t i = 0; i < n; i++) output_pass(rank[i], "pass", 0);
    gp_json summary = {0};
    json_add(&summary, "{\"schema\":3,\"type\":\"window\",\"window_start\":%llu,\"window_end\":%llu,"
        "\"sequence\":%llu,\"ordinals\":%s,\"frames\":%llu,\"partial\":%s,\"measured_ms\":%.9f,"
        "\"span_valid\":%s,\"valid_span_ms\":%.9f,\"valid_gap_ms\":%.9f,\"span_frames\":%llu,",
        (unsigned long long)(window * GP_WINDOW + 1), (unsigned long long)report_end,
        (unsigned long long)(window + 1), window_ordinals ? "true" : "false",
        (unsigned long long)window_frames, partial_report ? "true" : "false", total_ms,
        window_invalid_spans ? "false" : "true", window_span, window_gap, (unsigned long long)window_valid_span_frames);
    if (sample_budget) json_add(&summary, "\"sample_budget\":%u,\"sample_count\":%llu,"
        "\"observed_passes\":%llu,\"timestamp_count\":%llu,\"timing_scope\":\"main-frame\",",
        sample_budget, (unsigned long long)window_sample_count, (unsigned long long)window_observed_passes,
        (unsigned long long)window_timestamp_count);
    if (window_invalid_spans) json_add(&summary, "\"span_ms\":null,\"gap_ms\":null,");
    else json_add(&summary, "\"span_ms\":%.9f,\"gap_ms\":%.9f,", window_span, window_gap);
    json_add(&summary, "\"dropped\":%llu,\"failed_slots\":%llu,\"stats_failed_slots\":%llu,"
        "\"unavailable_timestamps\":%llu,\"invalid_timestamps\":%llu,\"invalid_spans\":%llu,\"output_failures\":%llu,"
        "\"background_dropped_slots\":%llu,\"background_dropped_closures\":%llu}\n",
        (unsigned long long)dropped_records, (unsigned long long)failed_slots, (unsigned long long)stats_failures,
        (unsigned long long)window_unavailable_timestamps, (unsigned long long)window_invalid_timestamps,
        (unsigned long long)window_invalid_spans, (unsigned long long)atomic_load(&output_failures),
        (unsigned long long)atomic_load(&background_dropped_slots),
        (unsigned long long)atomic_load(&background_dropped_closures));
    output_window(summary.data, window + 1, report_end);
    if (sample_budget) LOGV("[gpu-profile sample] budget=%u sampled=%llu observed=%llu timestamps=%llu frames=%llu; pass sums are sampled only, not frame totals",
        sample_budget, (unsigned long long)window_sample_count, (unsigned long long)window_observed_passes,
        (unsigned long long)window_timestamp_count, (unsigned long long)window_frames);
    LOGV("[gpu-profile] frames %llu-%llu: measured %.3f ms/frame (render %.3f compute %.3f), %.2f passes+dispatches/frame; LOAD %.2f CLEAR %.2f STORE %.2f DONT_CARE-store %.2f ops/frame; logical attachment estimate load %.3f store %.3f MiB/frame (not actual traffic); unknown-byte attachments %llu; dropped %llu failed-slots %llu",
         (unsigned long long)(window * GP_WINDOW + 1), (unsigned long long)((window + 1) * GP_WINDOW),
         total_ms / GP_WINDOW, render_ms / GP_WINDOW, compute_ms / GP_WINDOW, total_records / (double)GP_WINDOW,
         loads / (double)GP_WINDOW, clears / (double)GP_WINDOW, stores / (double)GP_WINDOW, discards / (double)GP_WINDOW,
         load_bytes / (1048576.0 * GP_WINDOW), store_bytes / (1048576.0 * GP_WINDOW),
         (unsigned long long)unknown_bytes, (unsigned long long)dropped_records, (unsigned long long)failed_slots);
    for (size_t i = 0; i < n && i < 5; i++) {
        const gp_group *g = rank[i]; const gp_key *k = &g->key;
        char attachments[800]; size_t used = 0; attachments[0] = 0;
        for (int j = 0; j < 10; j++) if (k->a[j].format) {
            const gp_attachment *a = &k->a[j];
            int nn = snprintf(attachments + used, sizeof attachments - used, " %c%d=%u:%ux%u:%u/%u", j < 8 ? 'c' : 'd', j, a->format, a->w, a->h, a->load, a->store);
            if (nn < 0 || (size_t)nn >= sizeof attachments - used) break;
            used += (size_t)nn;
        }
        LOGV("[gpu-profile] #%zu sig=%016llx %s ordinal=%u area=%ux%u reference=%ux%u samples=%u colors=%u shader=%016llx: %.3f ms/frame %.2f%% %.2f instances/frame %.2f draws/frame; attachments(fmt:absolute-widthxheight:load/store)%s",
             i + 1, (unsigned long long)g->hash, kind_name(k), g->ordinal, k->w, k->h, g->reference_w, g->reference_h, k->samples, k->colors,
             (unsigned long long)k->shader, g->ms / GP_WINDOW, total_ms ? 100 * g->ms / total_ms : 0,
             g->count / (double)GP_WINDOW, g->draws / (double)GP_WINDOW, attachments);
        LOGV("[gpu-profile scale] sig=%016llx: %.2f draws/instance, %.1f ns/draw (whole pass); geometry-stats %s valid=%llu/%llu IA-vertices=%.0f IA-primitives=%.0f VS=%.0f clipping-invocations=%.0f clipping-primitives=%.0f fragments=%.0f/frame; %.2f verts/draw %.2f prims/draw %.3f fragments/prim %.3f ns/vertex %.3f ns/primitive",
             (unsigned long long)g->hash, g->count ? g->draws / (double)g->count : 0, g->draws ? g->ms * 1e6 / g->draws : 0,
             stats_on ? "on" : "off", (unsigned long long)g->stats_count, (unsigned long long)g->count,
             g->counters[0] / (double)GP_WINDOW, g->counters[1] / (double)GP_WINDOW, g->counters[2] / (double)GP_WINDOW,
             g->counters[3] / (double)GP_WINDOW, g->counters[4] / (double)GP_WINDOW, g->counters[5] / (double)GP_WINDOW,
             g->stats_draws ? g->counters[0] / (double)g->stats_draws : 0,
             g->stats_draws ? g->counters[1] / (double)g->stats_draws : 0,
             g->counters[1] ? g->counters[5] / (double)g->counters[1] : 0,
             g->counters[0] ? g->stats_ms * 1e6 / g->counters[0] : 0,
             g->counters[1] ? g->stats_ms * 1e6 / g->counters[1] : 0);
    }
    if (sample_budget) for (size_t i = 0; i < n; i++) free(rank[i]->histogram);
    memset(groups, 0, sizeof groups);
    total_records = dropped_records = failed_slots = loads = clears = stores = discards = 0;
    if (stats_on) LOGV("[gpu-profile scale] pipeline-stat failed slots=%llu; counts are implementation-defined invocations, not covered pixels", (unsigned long long)stats_failures);
    stats_failures = 0;
    load_bytes = store_bytes = unknown_bytes = 0; total_ms = render_ms = compute_ms = 0;
    window_frames = 0; window_span = window_gap = 0;
    window_unavailable_timestamps = window_invalid_timestamps = window_invalid_spans = window_valid_span_frames = 0;
    window_sample_count = window_observed_passes = window_timestamp_count = 0;
}
static void advance(uint64_t target) { while (window < target) { report(); window++; } }
static vk_gpuprof_time_domain current_time_domain(void) {
    return (vk_gpuprof_time_domain){(uintptr_t)V.dev, (uintptr_t)V.queue, V.qfam,
        V.timestamp_bits, V.props.limits.timestampPeriod};
}
static void frame_time_failure(vk_gpuprof_time_status status) {
    completed.failed = 1;
    if (completed.status == GP_TIME_OK) completed.status = status;
}
static int query_readable(VkResult result) { return result == VK_SUCCESS || result == VK_NOT_READY; }
static void invalid_timestamp(vk_gpuprof_slot *slot, uint32_t query, vk_gpuprof_time_status status) {
    if (!(slot->timestamp_marks[query] & 2)) {
        slot->timestamp_marks[query] |= 2;
        completed.invalid_timestamps++; window_invalid_timestamps++;
    }
    frame_time_failure(status);
}
static void count_timestamp(vk_gpuprof_slot *slot, uint32_t query, vk_gpuprof_timestamp stamp, VkResult result) {
    if (!(slot->timestamp_marks[query] & 1)) {
        slot->timestamp_marks[query] |= 1;
        if (!query_readable(result)) invalid_timestamp(slot, query, GP_TIME_QUERY_FAILED);
        else if (!stamp.available) { completed.unavailable_timestamps++; window_unavailable_timestamps++; }
    }
    if (!query_readable(result)) frame_time_failure(GP_TIME_QUERY_FAILED);
    else if (!stamp.available) frame_time_failure(GP_TIME_UNAVAILABLE);
}
static void check_interval(vk_gpuprof_slot *slot, uint32_t last_query, vk_gpuprof_interval interval) {
    if (interval.status == GP_TIME_OK) { completed.timestamp_wraps += interval.wrapped; return; }
    if (interval.status == GP_TIME_UNAVAILABLE) frame_time_failure(interval.status);
    else invalid_timestamp(slot, last_query, interval.status);
}
static void finish_frame(void) {
    if (!completed.frame) return;
    finalize_passes();
    vk_gpuprof_interval interval = {.status = GP_TIME_MISSING};
    if (completed.has_ticks && completed.has_end) interval = vk_gpuprof_interval_check(completed.first, completed.last,
        completed.first_domain, completed.last_domain);
    if (interval.status != GP_TIME_OK && completed.status == GP_TIME_OK) {
        frame_time_failure(interval.status);
        if (interval.status != GP_TIME_UNAVAILABLE && interval.status != GP_TIME_MISSING) {
            completed.invalid_timestamps++; window_invalid_timestamps++;
        }
    }
    double gap = interval.ms - completed.measured;
    double epsilon = V.props.limits.timestampPeriod * (GP_RECORDS * 2.0 + 2) / 1e6;
    if (completed.status == GP_TIME_OK && (completed.failed || completed.dropped)) frame_time_failure(GP_TIME_INCOMPLETE);
    if (!sample_budget && completed.status == GP_TIME_OK && gap < -epsilon) frame_time_failure(GP_TIME_NEGATIVE_GAP);
    int valid = completed.status == GP_TIME_OK;
    char span_text[64] = "null", gap_text[64] = "null", raw_text[64] = "null", first_text[32] = "null", last_text[32] = "null";
    if (interval.has_value) snprintf(raw_text, sizeof raw_text, "%.9f", interval.ms);
    if (valid) {
        snprintf(span_text, sizeof span_text, "%.9f", interval.ms); snprintf(gap_text, sizeof gap_text, "%.9f", gap);
        window_span += interval.ms; window_gap += gap; window_valid_span_frames++;
    } else {
        window_invalid_spans++;
        LOGV("[gpu-profile invalid] frame=%llu reason=%s raw-span-ms=%s first=%llu/%llu last=%llu/%llu queue-family=%u bits=%u",
            (unsigned long long)completed.frame, vk_gpuprof_time_reason(completed.status), raw_text,
            (unsigned long long)completed.first.ticks, (unsigned long long)completed.first.available,
            (unsigned long long)completed.last.ticks, (unsigned long long)completed.last.available,
            completed.first_domain.queue_family, completed.first_domain.valid_bits);
    }
    if (completed.has_ticks && completed.first.available) snprintf(first_text, sizeof first_text, "%llu", (unsigned long long)completed.first.ticks);
    if (completed.has_ticks && completed.last.available) snprintf(last_text, sizeof last_text, "%llu", (unsigned long long)completed.last.ticks);
    uint64_t long_dropped = valid ? long_finish(completed.frame, interval.ms, completed.measured, gap) : 0;
    gp_json row = {0};
    json_add(&row, "{\"schema\":3,\"type\":\"frame\",\"frame\":%llu,\"span_ms\":%s,"
        "\"measured_ms\":%.9f,\"gap_ms\":%s,\"raw_span_ms\":%s,\"span_valid\":%s,\"span_reason\":\"%s\","
        "\"unavailable_timestamps\":%llu,\"invalid_timestamps\":%llu,\"timestamp_wraps\":%llu,"
        "\"first_tick\":%s,\"last_tick\":%s,\"timestamp_bits\":%u,\"timestamp_period\":%.12g,\"queue_family\":%u,"
        "\"dropped\":%llu,\"reconciliation_failed\":%s,\"long_dropped\":%llu,\"scene_width\":%u,\"scene_height\":%u",
        (unsigned long long)completed.frame, span_text, completed.measured, gap_text, raw_text,
        valid ? "true" : "false", vk_gpuprof_time_reason(completed.status),
        (unsigned long long)completed.unavailable_timestamps, (unsigned long long)completed.invalid_timestamps,
        (unsigned long long)completed.timestamp_wraps, first_text, last_text,
        completed.first_domain.valid_bits, completed.first_domain.period_ns, completed.first_domain.queue_family,
        (unsigned long long)completed.dropped, valid ? "false" : "true", (unsigned long long)long_dropped,
        completed.scene_w, completed.scene_h);
    json_add(&row, ",\"closure_source\":\"%s\",\"closure_sequence\":%llu",
        completed.closure_source ? completed.closure_source : "missing-closure",
        (unsigned long long)completed.closure_sequence);
    if (sample_budget) {
        json_add(&row, ",\"sample_budget\":%u,\"sampled_passes\":%llu,\"observed_passes\":%llu,"
            "\"timestamp_count\":%llu,\"timing_scope\":\"main-frame\"",
            sample_budget, (unsigned long long)completed.sampled_passes, (unsigned long long)completed.observed_passes,
            (unsigned long long)completed.timestamp_count);
        window_observed_passes += completed.observed_passes; window_timestamp_count += completed.timestamp_count;
    }
    json_add(&row, "}\n");
    output_frame(row.data, completed.frame);
    window_frames++; last_completed = completed.frame;
    memset(&completed, 0, sizeof completed);
}

/* Unlike full-mode slot envelopes, sampled mode writes just one distinct
   main-frame start/end pair even when the frame spans several submissions.
   Each segment reserves separate start/end coordinates; unused ones stay
   unavailable and are never consumed as endpoints. */
static void sampled_frame_begin(VkCommandBuffer cb) {
    if (!sample_budget || sample_frame_started || !current || cb == current->pre_cb) return;
    gp_segment *segment = &current->segment[current->nsegment - 1];
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, GP_SPAN_BASE + segment->first);
    current->span_writes++;
    segment->first_valid = 1; segment->timestamp_count++; sample_frame_started = 1;
}
void vk_gpuprof_slot_begin(vk_gpuprof_slot **slot, VkCommandBuffer cb) {
    static int initialized;
    if (!initialized) {
        initialized = 1;
        const char *e = getenv("TOTK_VK_GPU_PROFILE");
        if (e && (!strcmp(e, "1") || !strcmp(e, "2") || !strcmp(e, "3"))) {
            vk_gpuprof_on = V.has_host_query_reset && V.timestamp_bits && V.props.limits.timestampComputeAndGraphics && V.props.limits.timestampPeriod > 0 ? atoi(e) : 0;
            LOGV("[gpu-profile] %s (TOTK_VK_GPU_PROFILE=%s); 300-frame windows, exact primary pass parts; recorder workers supported",
                 vk_gpuprof_on ? "enabled" : "unavailable: graphics+compute timestamps or hostQueryReset missing", e);
            const char *se = getenv("TOTK_VK_GPU_PROFILE_STATS");
            int requested = se && !strcmp(se, "1");
            stats_on = vk_gpuprof_on && requested && V.has_pipeline_statistics;
            if (requested) LOGV("[gpu-profile] geometry+fragment statistics %s (pipelineStatisticsQuery=%d; TOTK_VK_GPU_PROFILE_STATS=1)", stats_on ? "on" : "unavailable", V.has_pipeline_statistics);
            const char *oe = getenv("TOTK_VK_GPU_PROFILE_ORDINALS");
            window_ordinals = oe && !strcmp(oe, "1");
            LOGV("[gpu-profile] window ordinals=%d (TOTK_VK_GPU_PROFILE_ORDINALS=%s; default=0; ordinal=0 means all frame ordinals)",
                window_ordinals, oe ? oe : "unset");
            sample_budget = (uint32_t)env_u64("TOTK_VK_GPU_PROFILE_SAMPLE", 0, 0, 16);
            LOGV("[gpu-profile] sample-budget=%u (TOTK_VK_GPU_PROFILE_SAMPLE; 0=full, 1..16=rotating passes; timestamps/frame <=2k+2)",
                sample_budget);
            if (vk_gpuprof_on) {
                const char *mode = getenv("TOTK_OPERATION_MODE"), *resolution = getenv("TOTK_RENDER_RES");
                int handheld = mode && !strcmp(mode, "handheld");
                game_reference_w = handheld ? 1280 : 1600; game_reference_h = handheld ? 720 : 900;
                unsigned w, h; char tail;
                if (resolution && sscanf(resolution, "%ux%u%c", &w, &h, &tail) == 2 &&
                    w >= 64 && h >= 64 && w <= 4096 && h <= 4096) {
                    game_reference_w = w; game_reference_h = h;
                }
                LOGV("[gpu-profile] schema=3 absolute pass keys; frame reference=largest depth+MRT, game fallback=%ux%u",
                    game_reference_w, game_reference_h);
            }
            if (vk_gpuprof_on == 3) {
                long_frames = (uint32_t)env_u64("TOTK_VK_GPU_PROFILE_LONG_FRAMES", 8, 1, 64);
                const char *threshold = getenv("TOTK_VK_GPU_PROFILE_LONG_MS"); char *end = NULL;
                if (threshold && *threshold) {
                    double parsed = strtod(threshold, &end);
                    if (end && !*end && isfinite(parsed) && parsed >= 0 && parsed <= 60000) long_ms = parsed;
                }
                LOGV("[gpu-profile] long-frame mode threshold=%.3f ms retained-frames=%u (TOTK_VK_GPU_PROFILE_LONG_MS/LONG_FRAMES)",
                    long_ms, long_frames);
            }
            if (vk_gpuprof_on) background_start();
        } else LOGV("[gpu-profile] disabled (TOTK_VK_GPU_PROFILE=%s; default=0)", e ? e : "unset");
    }
    current = NULL;
    if (!vk_gpuprof_on || atomic_load_explicit(&background_closing, memory_order_acquire)) { *slot = NULL; return; }
    if (*slot) DIE("GPU profiler lease reused before completion/cancel handoff");
    pthread_mutex_lock(&background_mu);
    if (!background_stop && !atomic_load_explicit(&background_closing, memory_order_relaxed) && background_nfree)
        *slot = background_free[--background_nfree];
    pthread_mutex_unlock(&background_mu);
    if (!*slot) { atomic_fetch_add_explicit(&background_dropped_slots, 1, memory_order_relaxed); return; }
    current = *slot; current->n = current->dropped = 0; current->sealed = 0;
    atomic_fetch_add_explicit(&leases_acquired, 1, memory_order_relaxed);
    current->span_writes = 0;
    current->ns = current->nt = 0; current->has_pre = 0; current->pre_cb = VK_NULL_HANDLE;
    current->discard = 0; current->sequence = 0;
    current->nsegment = 1; current->segment[0] = (gp_segment){frame, 0, 0};
    current->domain = current_time_domain();
    if (sample_budget) sampled_frame_begin(cb);
    else {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, GP_SPAN_BASE);
        current->span_writes++;
    }
}
void vk_gpuprof_slot_pre(vk_gpuprof_slot *slot, VkCommandBuffer cb) {
    if (!slot) return;
    slot->pre_cb = cb;
    if (sample_budget) return;
    slot->has_pre = 1;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot->pool, GP_PRE_QUERY);
    slot->span_writes++;
}
void vk_gpuprof_slot_end(vk_gpuprof_slot *slot, VkCommandBuffer cb) {
    if (!slot || sample_budget) return;
    slot->segment[slot->nsegment - 1].last = slot->nsegment;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot->pool, GP_SPAN_BASE + slot->nsegment);
    slot->span_writes++;
    /* Renderer seals before appending the final envelope command; standalone
       fixtures may append it before seal. Count each ordering exactly once. */
    if (slot->sequence) atomic_fetch_add_explicit(&queries_submitted, 1, memory_order_relaxed);
}
void vk_gpuprof_slot_cancel(vk_gpuprof_slot **slot) {
    if (current == *slot) {
        if (sample_budget && current && current->segment[0].first_valid) sample_frame_started = 0;
        current = NULL; open_record = NULL; scope_n = scope_overflow = 0;
    }
    background_enqueue(slot, 1);
}
static void frame_marker(VkCommandBuffer cb) {
    if (!current) return;
    if (current->nsegment == GP_SEGMENTS) { current->dropped++; return; }
    uint32_t marker = current->nsegment;
    sampled_frame_begin(cb);
    uint32_t query = sample_budget ? marker * 2 - 1 : marker;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, GP_SPAN_BASE + query);
    current->span_writes++;
    current->segment[marker - 1].last = query;
    current->segment[marker - 1].last_valid = 1; current->segment[marker - 1].timestamp_count++;
    current->segment[marker] = (gp_segment){.frame = frame + 1,
        .first = sample_budget ? marker * 2 : marker, .last = sample_budget ? marker * 2 : marker};
    if (sample_budget) sample_frame_started = 0;
    current->nsegment++;
}
/* Activated query instances, not unused pool capacity or individual counters. */
static uint64_t slot_queries(const vk_gpuprof_slot *slot) {
    return (uint64_t)(sample_budget ? slot->nt : slot->n) * 2 + slot->span_writes + slot->ns;
}
void vk_gpuprof_slot_seal(vk_gpuprof_slot *slot) {
    uint64_t sequence = ++recording_sequence;
    if (slot) {
        slot->sequence = sequence; slot->sealed = frame - 1;
        atomic_fetch_add_explicit(&leases_submitted, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&queries_submitted, slot_queries(slot), memory_order_relaxed);
    }
    current = NULL;
}
static void ingest_record(gp_record *r, double ms, int stats_valid, int sampled) {
    completed.measured += ms;
    if (!r->key.compute && r->key.a[8].format && active_colors(&r->key) >= 2 &&
        (uint64_t)r->key.w * r->key.h > (uint64_t)completed.scene_w * completed.scene_h) {
        completed.scene_w = r->key.w; completed.scene_h = r->key.h;
    }
    capture_record(r, ms, stats_valid, sampled);
    if (vk_gpuprof_on == 2) {
        uint64_t hash = key_hash(&r->key);
        const char *reason = r->reason < sizeof end_names / sizeof *end_names ? end_names[r->reason] : "unknown";
        LOGV("[gpu-split] frame=%llu ordinal=%u sig=%016llx kind=%s end=%s detail=%#x draws=%u ms=%.4f",
            (unsigned long long)r->frame, r->instance, (unsigned long long)hash, kind_name(&r->key), reason, r->detail, r->draws, ms);
        if (sample_budget) LOGV("[gpu-split sample] frame=%llu ordinal=%u timed=%d (zero on an unmeasured pass is not a duration)",
            (unsigned long long)r->frame, r->instance, sampled);
    }
}
static void process_slot(vk_gpuprof_slot *slot) {
    const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    uint32_t timed = sample_budget ? slot->nt : slot->n;
    VkResult result = timed ? vkGetQueryPoolResults(V.dev, slot->pool, 0, timed * 2,
        timed * 2 * sizeof slot->ticks[0], slot->ticks, sizeof slot->ticks[0], flags) : VK_SUCCESS;
    uint32_t spans = sample_budget ? slot->nsegment * 2 : slot->nsegment + 1;
    VkResult span_result = vkGetQueryPoolResults(V.dev, slot->pool, GP_SPAN_BASE, spans,
        spans * sizeof slot->span_ticks[0], slot->span_ticks, sizeof slot->span_ticks[0], flags);
    VkResult pre_result = slot->has_pre ? vkGetQueryPoolResults(V.dev, slot->pool, GP_PRE_QUERY, 1,
        sizeof slot->pre_tick, &slot->pre_tick, sizeof slot->pre_tick, flags) : VK_SUCCESS;
    VkResult sr = stats_on && slot->ns ? vkGetQueryPoolResults(V.dev, slot->stats, 0, slot->ns,
        slot->ns * sizeof slot->counters[0], slot->counters, sizeof slot->counters[0], VK_QUERY_RESULT_64_BIT) : VK_SUCCESS;
    for (uint32_t q = 0; sr == VK_SUCCESS && q < slot->ns; q++)
        for (unsigned st = 0; st < GP_STATS; st++)
            slot->rec[slot->stats_owner[q]].counters[st] += slot->counters[q][st];
    uint64_t read_queries = sr == VK_SUCCESS ? slot->ns : 0;
    if (query_readable(result)) for (uint32_t q = 0; q < timed * 2; q++)
        read_queries += !!slot->ticks[q].available;
    if (query_readable(span_result)) for (uint32_t q = 0; q < spans; q++)
        read_queries += !!slot->span_ticks[q].available;
    if (slot->has_pre && query_readable(pre_result)) read_queries += !!slot->pre_tick.available;
    atomic_fetch_add_explicit(&queries_retired, read_queries, memory_order_relaxed);
    atomic_fetch_add_explicit(&leases_retired, 1, memory_order_relaxed);
    int slot_failed = sample_budget ? !query_readable(result) || !query_readable(span_result) :
        result != VK_SUCCESS || span_result != VK_SUCCESS || pre_result != VK_SUCCESS;
    int slot_reported = 0;
    for (uint32_t s = 0; s < slot->nsegment; s++) {
        const gp_segment *segment = &slot->segment[s];
        if (completed.frame && completed.frame != segment->frame) {
            /* Ready CPU closures are consumed before later-frame leases.
               Reaching this transition still open means missing closure/data,
               not authority to manufacture a short whole-frame span. */
            frame_time_failure(GP_TIME_INCOMPLETE); finish_frame();
        }
        advance((segment->frame - 1) / GP_WINDOW);
        completed.frame = segment->frame;
        if (!s) {
            completed.dropped += slot->dropped; dropped_records += slot->dropped;
            stats_failures += sr != VK_SUCCESS;
            if (slot->sequence > background_processed_sequence + 1) frame_time_failure(GP_TIME_INCOMPLETE);
        }
        uint32_t first_query = !s && slot->has_pre ? GP_PRE_QUERY : GP_SPAN_BASE + segment->first;
        VkResult first_result = !s && slot->has_pre ? pre_result : span_result;
        vk_gpuprof_timestamp first = query_readable(first_result) ?
            (!s && slot->has_pre ? slot->pre_tick : slot->span_ticks[segment->first]) : (vk_gpuprof_timestamp){0};
        vk_gpuprof_timestamp last = query_readable(span_result) ? slot->span_ticks[segment->last] : (vk_gpuprof_timestamp){0};
        if (sample_budget) {
            if (completed.has_ticks && !vk_gpuprof_same_domain(completed.first_domain, slot->domain))
                frame_time_failure(GP_TIME_DOMAIN);
            completed.timestamp_count += segment->timestamp_count;
            if (segment->first_valid) {
                count_timestamp(slot, first_query, first, first_result);
                if (!completed.has_ticks) {
                    completed.first = first; completed.first_domain = slot->domain; completed.has_ticks = 1;
                } else invalid_timestamp(slot, first_query, GP_TIME_DOMAIN);
            }
            if (segment->last_valid) {
                count_timestamp(slot, GP_SPAN_BASE + segment->last, last, span_result);
                completed.last = last; completed.last_domain = slot->domain; completed.has_end = 1;
            }
        } else {
            count_timestamp(slot, first_query, first, first_result);
            count_timestamp(slot, GP_SPAN_BASE + segment->last, last, span_result);
            vk_gpuprof_interval segment_time = vk_gpuprof_interval_check(first, last, slot->domain, slot->domain);
            check_interval(slot, GP_SPAN_BASE + segment->last, segment_time);
            if (completed.has_ticks) check_interval(slot, first_query,
                vk_gpuprof_interval_check(completed.last, first, completed.last_domain, slot->domain));
            else { completed.first = first; completed.first_domain = slot->domain; }
            completed.last = last; completed.last_domain = slot->domain; completed.has_ticks = completed.has_end = 1;
            if (segment_time.status != GP_TIME_OK || completed.status != GP_TIME_OK) slot_failed = 1;
        }
        for (uint32_t i = 0; i < slot->n; i++) {
            gp_record *r = &slot->rec[i];
            if (r->frame != segment->frame) continue;
            completed.observed_passes++; completed.sampled_passes += r->selected;
            completed.timestamp_count += r->timestamp_count;
            if (!r->selected) { ingest_record(r, 0, 0, 0); continue; }
            uint32_t query = r->timestamp_q1 - 1;
            vk_gpuprof_timestamp start = query_readable(result) ? slot->ticks[query] : (vk_gpuprof_timestamp){0};
            vk_gpuprof_timestamp end = query_readable(result) ? slot->ticks[query + 1] : (vk_gpuprof_timestamp){0};
            count_timestamp(slot, query, start, result); count_timestamp(slot, query + 1, end, result);
            vk_gpuprof_interval pass_time = vk_gpuprof_interval_check(start, end, slot->domain, slot->domain);
            check_interval(slot, query + 1, pass_time);
            if (pass_time.status != GP_TIME_OK) {
                slot_failed = 1;
                if (sample_budget) ingest_record(r, 0, 0, 0);
                continue;
            }
            ingest_record(r, pass_time.ms, r->stats_count && !r->stats_invalid && sr == VK_SUCCESS, 1);
        }
        if (slot_failed && !slot_reported) { failed_slots++; slot_reported = 1; }
        if (segment->frame <= slot->sealed) {
            completed.closure_source = "gpu-marker"; completed.closure_sequence = slot->sequence; finish_frame();
        }
    }
    if (slot->sealed) advance(slot->sealed / GP_WINDOW);
}
/* CPU present can happen after the final command buffer was already sealed.
   Its immutable end sequence is authority for when the worker may retire that
   frame; a recording-thread watermark alone would race outstanding leases. */
static int background_closure_ready(void) { /* background_mu held */
    return background_close_head != background_close_tail &&
        background_closures[background_close_head % GP_CLOSURES].sequence <= background_processed_sequence;
}
static void complete_closure(gp_closure close) {
    if (close.frame <= last_completed || completed.frame > close.frame) return;
    if (completed.frame && completed.frame < close.frame) {
        frame_time_failure(GP_TIME_INCOMPLETE); finish_frame();
    }
    completed.frame = close.frame;
    completed.closure_source = "cpu-sealed"; completed.closure_sequence = close.sequence;
    finish_frame();
    advance(close.frame / GP_WINDOW);
}
static void background_prepare(vk_gpuprof_slot *slot) {
    vkResetQueryPool(V.dev, slot->pool, 0, GP_TIMESTAMPS);
    if (stats_on) vkResetQueryPool(V.dev, slot->stats, 0, GP_RECORDS);
    memset(slot->timestamp_marks, 0, sizeof slot->timestamp_marks);
}
static void background_flush(void) {
    if (!window_frames) return;
    partial_report = 1; report_end = last_completed; report(); partial_report = 0;
}
static void *background_main(void *unused) {
    (void)unused;
    totk_thread_start("totk-gpuprof", NULL);
#if defined(__linux__)
    /* Same conservative background policy as pipeline-cache/seed IO: never
       widen the inherited CPU mask and never contend at render priority. */
    int nice_error = setpriority(PRIO_PROCESS, 0, 10) ? errno : 0, affinity_error = 0;
    cpu_set_t allowed, preferred, actual; const char *source = "unavailable", *decision = "unchanged";
    if (sched_getaffinity(0, sizeof allowed, &allowed)) affinity_error = errno;
    else {
        unsigned capacity[CPU_SETSIZE];
        totk_thread_background_select("/sys/devices/system/cpu", &allowed, &preferred, capacity, &source, &decision);
        if (!CPU_EQUAL(&preferred, &allowed) && sched_setaffinity(0, sizeof preferred, &preferred)) affinity_error = errno;
    }
    char mask[CPU_SETSIZE * 6];
    if (sched_getaffinity(0, sizeof actual, &actual)) strcpy(mask, "?");
    else totk_thread_mask_text(&actual, mask, sizeof mask);
    errno = 0; int nice_value = getpriority(PRIO_PROCESS, 0), nice_get_error = errno;
    LOGV("[gpu-profile] background placement nice=%d nice_set_errno=%d nice_get_errno=%d affinity=%s source=%s decision=%s affinity_errno=%d",
        nice_value, nice_error, nice_get_error, mask, source, decision, affinity_error);
#endif
    if (long_frames) {
        long_ring = calloc(long_frames, sizeof *long_ring);
        if (!long_ring) DIE("GPU profiler long-frame ring allocation");
    }
    output_init();
    for (unsigned i = 0; i < GP_BACKLOG; i++) {
        vk_gpuprof_slot *slot = calloc(1, sizeof *slot);
        if (!slot) DIE("GPU profiler lease allocation");
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_TIMESTAMPS};
        VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &slot->pool));
        if (stats_on) {
            qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = GP_RECORDS; qi.pipelineStatistics = GP_STATS_MASK;
            VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &slot->stats));
        }
        background_prepare(slot); background_free[background_nfree++] = slot;
    }
    pthread_mutex_lock(&background_mu);
    background_ready = 1;
    pthread_cond_broadcast(&background_cv);
    for (;;) {
#ifdef TOTK_VK_TESTS
        while (!background_stop && (background_paused ||
            (background_head == background_tail && !background_closure_ready() &&
             background_flush_done == background_flush_requested)))
            pthread_cond_wait(&background_cv, &background_mu);
#else
        while (!background_stop && background_head == background_tail && !background_closure_ready())
            pthread_cond_wait(&background_cv, &background_mu);
#endif
        if (background_closure_ready()) {
            gp_closure close = background_closures[background_close_head++ % GP_CLOSURES];
            background_active = 1;
            pthread_mutex_unlock(&background_mu);
            complete_closure(close);
            pthread_mutex_lock(&background_mu);
            background_active = 0; pthread_cond_broadcast(&background_cv);
            continue;
        }
        if (background_head != background_tail) {
            vk_gpuprof_slot *slot = background_queue[background_head++ % GP_BACKLOG];
            background_active = 1;
            pthread_mutex_unlock(&background_mu);
            if (!slot->discard) process_slot(slot);
            background_prepare(slot);
            atomic_fetch_add_explicit(&background_processed_slots, 1, memory_order_relaxed);
            pthread_mutex_lock(&background_mu);
            if (!slot->discard) background_processed_sequence = slot->sequence;
            background_free[background_nfree++] = slot; background_active = 0;
            pthread_cond_broadcast(&background_cv);
            continue;
        }
        if (background_stop) break;
#ifdef TOTK_VK_TESTS
        uint64_t request = background_flush_requested;
        background_active = 1;
        pthread_mutex_unlock(&background_mu);
        background_flush();
        pthread_mutex_lock(&background_mu);
        background_active = 0; background_flush_done = request;
        pthread_cond_broadcast(&background_cv);
#endif
    }
    pthread_mutex_unlock(&background_mu);
    background_flush();
    /* Only idle leases are owned here. GPU work not yet handed off remains
       deliberately outside shutdown authority and must not be destroyed. */
    while (background_nfree) {
        vk_gpuprof_slot *slot = background_free[--background_nfree];
        if (slot->stats) vkDestroyQueryPool(V.dev, slot->stats, NULL);
        vkDestroyQueryPool(V.dev, slot->pool, NULL); free(slot);
    }
    for (uint32_t i = 0; i < long_frames; i++) free(long_ring[i].pass);
    free(long_ring); long_ring = NULL;
    free(frame_pending.pass); frame_pending.pass = NULL;
    return NULL;
}
static void background_start(void) {
    pthread_mutex_lock(&background_mu);
    if (!background_started && !atomic_load_explicit(&background_closing, memory_order_acquire)) {
        background_started = 1;
        int error = pthread_create(&background_thread, NULL, background_main, NULL);
        if (error) DIE("GPU profiler thread creation failed: %d", error);
    }
    while (background_started && !background_ready) pthread_cond_wait(&background_cv, &background_mu);
    pthread_mutex_unlock(&background_mu);
}
static void background_stop_and_join(void) {
    atomic_store_explicit(&background_closing, 1, memory_order_release);
    pthread_mutex_lock(&background_mu);
    while (background_joining && !background_joined) pthread_cond_wait(&background_cv, &background_mu);
    if (!background_started || background_joined) { pthread_mutex_unlock(&background_mu); return; }
    background_joining = background_stop = 1;
    pthread_cond_broadcast(&background_cv);
    pthread_mutex_unlock(&background_mu);
    pthread_join(background_thread, NULL);
    pthread_mutex_lock(&background_mu);
    background_joined = 1;
    pthread_cond_broadcast(&background_cv);
    pthread_mutex_unlock(&background_mu);
}
static void background_enqueue(vk_gpuprof_slot **owner, int discard) {
    vk_gpuprof_slot *slot = *owner; *owner = NULL;
    if (!slot) return;
    if (discard) {
        atomic_fetch_add_explicit(&leases_cancelled, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&queries_cancelled, slot_queries(slot), memory_order_relaxed);
    }
    slot->discard = discard;
    pthread_mutex_lock(&background_mu);
    if (!background_stop) {
        if (background_tail - background_head == GP_BACKLOG) DIE("GPU profiler lease queue overflow");
        background_queue[background_tail++ % GP_BACKLOG] = slot;
        atomic_fetch_add_explicit(&background_queued_slots, 1, memory_order_relaxed);
        pthread_cond_signal(&background_cv);
    }
    pthread_mutex_unlock(&background_mu);
}
void vk_gpuprof_slot_complete(vk_gpuprof_slot **slot) {
    background_enqueue(slot, 0); /* only called AFTER the renderer's existing fence */
}
static void begin_record(VkCommandBuffer cb, int compute) {
    open_record = NULL;
    if (!current || current->n == GP_RECORDS) { if (current) current->dropped++; return; }
    sampled_frame_begin(cb);
    uint32_t index = current->n++;
    open_record = &current->rec[index]; memset(open_record, 0, sizeof *open_record);
    open_record->frame = frame; open_record->key.compute = (uint32_t)compute;
    open_record->instance = ++frame_instance; open_cb = cb;
    int selected = !sample_budget;
    if (sample_budget && sample_selected < sample_budget) {
        /* Prior frame count bounds an online schedule without buffering GPU
           recording. Stable layouts cover all ordinals in ceil(N/k) frames;
           changing layouts retain explicit observed/sample coverage. */
        uint32_t count = sample_previous_passes ? sample_previous_passes : sample_budget;
        uint32_t offset = (uint32_t)(sample_cursor % count);
        uint32_t ordinal = open_record->instance - 1;
        selected = ordinal < count && (ordinal + count - offset) % count < sample_budget;
    }
    open_record->selected = selected;
    if (!selected) return;
    sample_selected++;
    uint32_t query = sample_budget ? current->nt++ * 2 : index * 2;
    open_record->timestamp_q1 = query + 1;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, query);
    open_record->timestamp_count = 1;
    if (!compute && stats_on && !secondary_pass && current->ns < GP_RECORDS) {
        open_record->stats_q1 = ++current->ns; open_record->stats_count++;
        current->stats_owner[current->ns - 1] = index;
        vkCmdBeginQuery(cb, current->stats, open_record->stats_q1 - 1, 0);
    }
}
void vk_gpuprof_begin(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat formats[10],
                     const tex_t *const textures[10], const uint32_t widths[10], const uint32_t heights[10], uint32_t samples) {
    begin_record(cb, 0); if (!open_record) return;
    gp_key *k = &open_record->key; k->w = ri->renderArea.extent.width; k->h = ri->renderArea.extent.height;
    k->samples = samples; k->colors = ri->colorAttachmentCount;
    for (int j = 0; j < 10; j++) {
        const VkRenderingAttachmentInfo *a = j < 8 ? (j < (int)ri->colorAttachmentCount ? &ri->pColorAttachments[j] : NULL) : j == 8 ? ri->pDepthAttachment : ri->pStencilAttachment;
        if (!a || !a->imageView || !textures[j]) continue;
        uint32_t bytes = texel_bytes(formats[j]);
        /* Depth and stencil are distinct load/store operations: do not count a packed DS image twice. */
        if (j == 9) bytes = 1;
        else if (j == 8 && formats[j] == VK_FORMAT_D32_SFLOAT_S8_UINT) bytes = 4;
        else if (j == 8 && formats[j] == VK_FORMAT_D24_UNORM_S8_UINT) bytes = 3;
        k->a[j] = (gp_attachment){formats[j], widths[j], heights[j], a->loadOp, a->storeOp, bytes};
    }
}
void vk_gpuprof_dispatch_begin(VkCommandBuffer cb) { begin_record(cb, 1); }
void vk_gpuprof_draw(vk_shader *const shaders[6]) {
    if (!open_record) return;
    /* Fixed first-draw identity work; never inspect bound resources or earlier passes. */
    if (!open_record->draws) {
        uint64_t h = UINT64_C(14695981039346656037);
        for (int i = 0; i < 6; i++) if (shaders[i]) { h = hash_bytes(h, &i, sizeof i); h = hash_bytes(h, shaders[i]->key, strlen(shaders[i]->key) + 1); }
        open_record->key.shader = h;
    }
    open_record->draws++;
}
void vk_gpuprof_secondary(int enabled) { secondary_pass = enabled; }
int vk_gpuprof_chunk_query(VkQueryPool *pool, uint32_t *query) {
    if (!stats_on || !current || !open_record || !open_record->selected) return 0;
    if (current->ns == GP_RECORDS) { open_record->stats_invalid = 1; return 0; }
    *pool = current->stats; *query = current->ns++;
    current->stats_owner[*query] = (uint32_t)(open_record - current->rec);
    open_record->stats_count++;
    return 1;
}
VkQueryPipelineStatisticFlags vk_gpuprof_stats_mask(void) {
    return stats_on ? GP_STATS_MASK : 0;
}
void vk_gpuprof_end_emit(VkCommandBuffer cb, const vk_gpuprof_end_token *token) {
    if (!token->pool) return;
    if (token->stats_query1) vkCmdEndQuery(cb, token->stats, token->stats_query1 - 1);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, token->pool, token->timestamp);
    if (token->timestamp_count) (*token->timestamp_count)++;
}
void vk_gpuprof_end_deferred(vk_pass_end_kind reason, uint32_t detail, vk_gpuprof_end_token *token) {
    memset(token, 0, sizeof *token);
    if (!open_record) return;
    open_record->reason = (uint32_t)reason; open_record->detail = detail;
    if (open_record->selected) *token = (vk_gpuprof_end_token){current->pool, current->stats,
        open_record->timestamp_q1, open_record->stats_q1, &open_record->timestamp_count};
    open_record = NULL; open_cb = VK_NULL_HANDLE;
}
void vk_gpuprof_end(VkCommandBuffer cb, vk_pass_end_kind reason, uint32_t detail) {
    vk_gpuprof_end_token token;
    vk_gpuprof_end_deferred(reason, detail, &token);
    vk_gpuprof_end_emit(cb, &token);
}
void vk_gpuprof_scope_begin(VkCommandBuffer cb, const char *label) {
    if (scope_n == sizeof scopes / sizeof *scopes) { scope_overflow++; return; }
    gp_scope *scope = &scopes[scope_n++];
    *scope = (gp_scope){.parent = open_record, .parent_cb = open_cb,
        .cb = cb, .secondary = secondary_pass, .skipped = open_record && cb == open_cb};
    if (scope->skipped) return;
    static const char *const names[] = {"upload", "transfer", "post", "upscale", "present", "framegen"};
    unsigned kind = 2;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(label, names[i])) { kind = i + 2; break; }
    begin_record(cb, (int)kind);
}
void vk_gpuprof_scope_end(VkCommandBuffer cb) {
    if (scope_overflow) { scope_overflow--; return; }
    if (!scope_n) return;
    gp_scope *scope = &scopes[--scope_n];
    if (!scope->skipped) vk_gpuprof_end(cb, VK_PASS_END_OTHER, 0);
    open_record = scope->parent;
    open_cb = scope->parent_cb; secondary_pass = scope->secondary;
}
static void present_at(VkCommandBuffer cb) {
    if (current) frame_marker(cb);
    pthread_mutex_lock(&background_mu);
    if (!background_stop) {
        if (background_close_tail - background_close_head == GP_CLOSURES)
            atomic_fetch_add_explicit(&background_dropped_closures, 1, memory_order_relaxed);
        else {
            background_closures[background_close_tail++ % GP_CLOSURES] =
                (gp_closure){frame, recording_sequence + !!cb};
            pthread_cond_signal(&background_cv);
        }
    }
    pthread_mutex_unlock(&background_mu);
    if (sample_budget) {
        sample_previous_passes = frame_instance;
        sample_cursor += sample_budget; sample_selected = 0;
    }
    frame++;
    frame_instance = 0;
}
void vk_gpuprof_present(void) {
    if (!vk_gpuprof_on || atomic_load_explicit(&background_closing, memory_order_acquire)) return;
    if (!current) {
        if (sample_budget) {
            /* A sealed final submission cannot accept the one sampled-frame
               end timestamp. Submit one marker-only buffer, not per-slot ends
               or a CPU-time surrogate. No new pass/stat query is introduced. */
            VkCommandBuffer cb = vk_cb();
            if (current) { present_at(cb); (void)vk_submit_now(); return; }
        }
        /* Also seal any unprofiled open slot after lease exhaustion before
           publishing a CPU closure. Usually Android present already did this. */
        if (atomic_load_explicit(&background_dropped_slots, memory_order_relaxed)) (void)vk_submit_now();
        present_at(VK_NULL_HANDLE);
    } else present_at(vk_cb());
}
#ifdef TOTK_VK_TESTS
void vk_gpuprof_test_frame(VkCommandBuffer cb) {
    if (!vk_gpuprof_on || atomic_load_explicit(&background_closing, memory_order_acquire)) return;
    present_at(cb);
}
void vk_gpuprof_test_wait(void) {
    if (!vk_gpuprof_on) return;
    pthread_mutex_lock(&background_mu);
    while (!background_stop && (background_head != background_tail || background_active || background_closure_ready()))
        pthread_cond_wait(&background_cv, &background_mu);
    pthread_mutex_unlock(&background_mu);
}
void vk_gpuprof_test_pause(int paused) {
    pthread_mutex_lock(&background_mu);
    background_paused = paused;
    pthread_cond_broadcast(&background_cv);
    pthread_mutex_unlock(&background_mu);
}
void vk_gpuprof_test_background(uint64_t counts[3]) {
    counts[0] = atomic_load_explicit(&background_queued_slots, memory_order_relaxed);
    counts[1] = atomic_load_explicit(&background_processed_slots, memory_order_relaxed);
    counts[2] = atomic_load_explicit(&background_dropped_slots, memory_order_relaxed);
}
void vk_gpuprof_test_flush(void) {
    if (!vk_gpuprof_on) return;
    pthread_mutex_lock(&background_mu);
    if (background_started && !background_stop) {
        uint64_t request = ++background_flush_requested;
        pthread_cond_broadcast(&background_cv);
        while (background_flush_done != request) pthread_cond_wait(&background_cv, &background_mu);
    }
    pthread_mutex_unlock(&background_mu);
}
#endif

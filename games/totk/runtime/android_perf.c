/* Android performance hints. dlsym discovery follows nx2android perf_android.c
 * (MIT); no affinity, governor, frequency-cap or thermal policy writes. */
#include "android_perf.h"
#include "android_thermal.h"
#ifdef __ANDROID__
#include "adpf_policy.h"
#include "live_ctl.h"
#include "guest_clock.h"
#include <android/api-level.h>
#include <android/native_window.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t thread_key;
static int mode, rate_on, fps60, framegen, lattice, registry_dirty, key_ready;
static int (*window_rate)(ANativeWindow *, float, int8_t);
static void (*surface_rate)(ASurfaceTransaction *, ASurfaceControl *, float, int8_t, int8_t);
static totk_adpf_api api;
static totk_adpf_state session;
static int64_t legacy_target;
static unsigned logged, frames;
static uint64_t previous_frame, sample_time;
static _Atomic unsigned content_rate = 60;
/* TOTK_RATE_HINT_HOLD (ms; 0 = the configured rate as before): launch value, live cache, the guest present thread's
 * hold state, the value it last logged, and generated outputs the presenter queued (frame generation is running). */
static int hold_launch, hold_logged = -1;
static totk_live_cache hold_live;
static totk_rate_hold rate_hold;
static _Atomic uint64_t generated_presents;
static uint64_t generated_seen;
/* TOTK_ADPF_GPU: launch value, live cache, the value the session last ran with, the frame's start and the pacing
 * waits since the last present (mu; CLOCK_MONOTONIC), the newest completed frame's GPU time and the last reported. */
static int span_launch;
static _Atomic int span_applied;
static totk_live_cache span_live;
static int load_launch, load_applied;
static totk_live_cache load_live;
static totk_adpf_load load_history;
static int cpu_split_launch;
static _Atomic int cpu_split_applied;
static totk_live_cache cpu_split_live;
static int target_pct_launch = 90, target_pct_applied = 90;
static totk_live_cache target_pct_live;
static _Atomic uint64_t frame_start, gpu_published;
static totk_adpf_pacing span_pacing;
static totk_adpf_pacing span_queue_wait;
static totk_adpf_gpu_acc gpu_acc;   /* the completion thread only */
static uint32_t gpu_seen;
static struct { unsigned frames, spans, gpus, errors, hints, hint_errors; uint64_t span_sum, span_max, gpu_sum, gpu_max; } span_stats;
/* Bounded registry; pthread CPU clocks avoid /proc parsing/allocations. */
static struct { int32_t tid; clockid_t clock; uint64_t cpu, used; int critical; } registry[256];
static int is_one(const char *name) { const char *s = getenv(name); return s && !strcmp(s, "1"); }
static uint64_t mono_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static int span_on(void) { return mode && totk_live_cached(&span_live, "TOTK_ADPF_GPU", span_launch) != 0; }
static void log_span(int on, const char *when) {
    int64_t target = !mode ? 0 : on ? totk_adpf_span_target(fps60, framegen, target_pct_applied) : mode == 1 ? legacy_target : session.target;
    fprintf(stderr, "[totk-perf] TOTK_ADPF_GPU=%d (%s): %s; target %.3fms%s\n", on, when,
        !mode ? "no ADPF session (TOTK_ADPF=0)" :
        !on ? (mode == 2 ? "off, wait-stripped present interval" : "off, raw present interval") :
        api.work ? "frame span (first submit to present, less pacing) + GPU duration of the newest completed frame, once" :
        "frame span only (no reportActualWorkDuration2/AWorkDuration below API35)",
        target / 1e6, mode == 2 && !on ? " (follows the effective interval)" : "");
}
static void remove_thread(void *value) {
    int32_t tid = (int32_t)(intptr_t)value;
    pthread_mutex_lock(&mu);
    for (unsigned i = 0; i < 256; i++) if (registry[i].tid == tid) {
        registry[i].tid = 0; registry_dirty = 1; break;
    }
    pthread_mutex_unlock(&mu);
}
static void log_refinements(void) {
    fprintf(stderr, "[totk-perf] TOTK_ADPF_LOAD_HINT=%d (launch, live): %s; needs TOTK_ADPF_GPU=1\n",
        load_applied, !mode ? "no ADPF session, no-op" : !load_applied ? "off" : api.increase ?
        "3 frames >105%, 500ms limit, GPU >70%" : "unavailable (API36 notifyWorkloadIncrease absent), no-op");
    fprintf(stderr, "[totk-perf] TOTK_ADPF_CPU_SPLIT=%d (launch, live): %s; needs TOTK_ADPF_GPU=1\n",
        cpu_split_launch, !mode ? "no ADPF session, no-op" : !cpu_split_launch ? "off" : api.work ?
        "CPU=span minus present/texture queue wait; total unchanged" : "unavailable (no API35 report2), no-op");
    fprintf(stderr, "[totk-perf] TOTK_ADPF_TARGET_PCT=%d (launch, live): configured period margin; needs TOTK_ADPF_GPU=1%s\n",
        target_pct_applied, mode ? "" : "; no ADPF session, no-op");
}
static void configure(void) {
    const char *request = getenv("TOTK_ADPF"), *target = getenv("TOTK_ADPF_TARGET_MS");
    mode = request && !strcmp(request, "0") ? 0 : request && !strcmp(request, "full") ? 2 : 1;
    double ms = target && *target ? atof(target) : 33.3;
    legacy_target = ms > 0 && ms < 1000 ? (int64_t)(ms * 1e6) : 0;
    if (mode == 1 && !legacy_target) mode = 0;
    rate_on = is_one("TOTK_FRAME_RATE_HINT"); fps60 = is_one("TOTK_FPS60");
    span_applied = span_launch = is_one("TOTK_ADPF_GPU");
    load_applied = load_launch = is_one("TOTK_ADPF_LOAD_HINT");
    cpu_split_applied = cpu_split_launch = is_one("TOTK_ADPF_CPU_SPLIT");
    const char *pct = getenv("TOTK_ADPF_TARGET_PCT");
    while (pct && isspace((unsigned char)*pct)) pct++;
    if (pct && *pct && !totk_live_decimal(pct, 50, 100, &target_pct_launch)) {
        fprintf(stderr, "[totk-perf] TOTK_ADPF_TARGET_PCT=%s ignored: not a decimal in 50..100\n", pct);
        target_pct_launch = 90;
    }
    target_pct_applied = target_pct_launch;
    const char *fg = getenv("TOTK_FRAMEGEN");
    framegen = fg && (!strcmp(fg, "1") || !strcmp(fg, "locked") ||
        !strcmp(fg, "interp") || !strcmp(fg, "hybrid") || !strcmp(fg, "adaptive"));
    const char *acquire = getenv("TOTK_ACQUIRE_CLOCK");
    lattice = is_one("TOTK_PRESENT_SLOTS") || (acquire && !strcmp(acquire, "vsync"));
    const char *pace = getenv("TOTK_PACE"), *slack = getenv("TOTK_ACQUIRE_SLACK");
    lattice |= pace && !strcmp(pace, "display");
    lattice |= slack && strcmp(slack, "0");
    atomic_store_explicit(&content_rate, totk_frame_rate(fps60 ? 60 : 30, framegen, lattice), memory_order_relaxed);
    const char *hold = getenv("TOTK_RATE_HINT_HOLD");
    while (hold && isspace((unsigned char)*hold)) hold++;
    if (hold && *hold && !totk_live_decimal(hold, 0, 60000, &hold_launch)) {
        fprintf(stderr, "[totk-perf] TOTK_RATE_HINT_HOLD=%s ignored: not a decimal in 0..60000 ms\n", hold);
        hold_launch = 0;
    }
    if (rate_on || hold_launch) {
        fprintf(stderr, "[totk-perf] TOTK_RATE_HINT_HOLD=%d ms (launch, live): %s\n", hold_launch,
            !rate_on ? "no effect without TOTK_FRAME_RATE_HINT=1" : hold_launch ?
            "the window's output rate from the game's interval and frame generation, highest of the hold window" :
            "off, the configured rate");
        hold_logged = hold_launch;
    }
    if (!mode && !rate_on) { log_refinements(); if (span_launch) log_span(1, "launch"); return; }
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (rate_on) {
        window_rate = lib ? dlsym(lib, "ANativeWindow_setFrameRate") : NULL;
        surface_rate = lib ? dlsym(lib, "ASurfaceTransaction_setFrameRate") : NULL;
        fprintf(stderr, "[totk-perf] rate hint support: window=%s surface=%s; FIXED_SOURCE; %uHz%s\n",
            window_rate ? "yes" : "no", surface_rate ? "yes" : "no", atomic_load(&content_rate),
            lattice && !fps60 && !framegen ? " (30fps content, retaining 60Hz present lattice)" : "");
    }
    if (!mode) { log_refinements(); if (span_launch) log_span(1, "launch"); return; }
    void *(*get_manager)(void) = lib ? dlsym(lib, "APerformanceHint_getManager") : NULL;
    api.create = lib ? dlsym(lib, "APerformanceHint_createSession") : NULL;
    api.update = lib ? dlsym(lib, "APerformanceHint_updateTargetWorkDuration") : NULL;
    api.report = lib ? dlsym(lib, "APerformanceHint_reportActualWorkDuration") : NULL;
    api.set_threads = lib ? dlsym(lib, "APerformanceHint_setThreads") : NULL;
    api.close = lib ? dlsym(lib, "APerformanceHint_closeSession") : NULL;
    if (android_get_device_api_level() >= 33 && get_manager) api.manager = get_manager();
    if (android_get_device_api_level() >= 35 && lib) {
        /* Resolved at run time: the NDK API level we build against has none of them. */
        void *(*work_create)(void) = dlsym(lib, "AWorkDuration_create");
        api.report2 = dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
        api.set_start = dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
        api.set_total = dlsym(lib, "AWorkDuration_setActualTotalDurationNanos");
        api.set_cpu = dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
        api.set_gpu = dlsym(lib, "AWorkDuration_setActualGpuDurationNanos");
        if (work_create && api.report2 && api.set_start && api.set_total && api.set_cpu && api.set_gpu)
            api.work = work_create();   /* one object for the process, reused by every report */
        if (!api.work) api.report2 = NULL;
    }
    if (android_get_device_api_level() >= 36 && lib)
        api.increase = dlsym(lib, "APerformanceHint_notifyWorkloadIncrease");
    log_refinements();
    if (mode == 2 && api.manager) key_ready = !pthread_key_create(&thread_key, remove_thread);
    fprintf(stderr, "[totk-perf] ADPF mode=%s api=%d manager=%s create/report/close=%s setThreads=%s updateTarget=%s report2/AWorkDuration=%s; legacy-target=%.3fms\n",
        mode == 2 ? "full" : "legacy", android_get_device_api_level(), api.manager ? "yes" : "no",
        api.create && api.report && api.close ? "yes" : "no", api.set_threads ? "yes" : "no",
        api.update ? "yes" : "no", api.work ? "yes" : "no", legacy_target / 1e6);
    log_span(span_launch, "launch, live");
}
static uint64_t cpu_time(clockid_t clock) {
    struct timespec t;
    return clock_gettime(clock, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
void totk_perf_thread(const char *name) {
    pthread_once(&config_once, configure);
    if (mode != 2 || !key_ready) return;
    int critical = !strcmp(name, "render") || !strcmp(name, "totk-present") ||
        !strncmp(name, "totk-recw", 9) || !strcmp(name, "totk-submit") ||
        !strcmp(name, "totk-translate") || !strcmp(name, "g-main");
    if (!critical && strncmp(name, "g-", 2)) return;
    int32_t tid = (int32_t)syscall(SYS_gettid);
    clockid_t clock;
    if (pthread_getcpuclockid(pthread_self(), &clock)) return;
    pthread_mutex_lock(&mu);
    unsigned free_slot = 256;
    for (unsigned i = 0; i < 256; i++) {
        if (registry[i].tid == tid) {
            if (critical && !registry[i].critical) { registry[i].critical = 1; registry_dirty = 1; }
            pthread_mutex_unlock(&mu); return;
        }
        if (!registry[i].tid && free_slot == 256) free_slot = i;
    }
    if (free_slot < 256 && !pthread_setspecific(thread_key, (void *)(intptr_t)tid)) {
        registry[free_slot].tid = tid; registry[free_slot].clock = clock;
        registry[free_slot].cpu = cpu_time(clock); registry[free_slot].used = 0;
        registry[free_slot].critical = critical; registry_dirty = 1;
    }
    pthread_mutex_unlock(&mu);
}
static void log_changes(unsigned changes) {
    if (changes & (TOTK_ADPF_CREATED | TOTK_ADPF_TARGET | TOTK_ADPF_THREADS)) {
        fprintf(stderr, "[totk-perf] ADPF session %s target=%.3fms threads=%zu tids=",
            changes & TOTK_ADPF_CREATED ? "created" : "updated", session.target / 1e6, session.count);
        for (size_t i = 0; i < session.count; i++) fprintf(stderr, "%s%d", i ? "," : "", session.tids[i]);
        fprintf(stderr, "; %s\n", span_applied ? "frame span (TOTK_ADPF_GPU)" :
            mode == 2 ? "real-tick work, synthetic midpoint not reported" : "legacy raw present interval");
    }
    unsigned errors = changes & (TOTK_ADPF_REFUSED | TOTK_ADPF_REPORT_ERROR) & ~logged;
    if (errors) fprintf(stderr, "[totk-perf] ADPF %s; no per-frame retry/log storm\n",
        errors & TOTK_ADPF_REFUSED ? "session unavailable/refused" :
        span_applied && api.work ? "reportActualWorkDuration2 failed" : "reportActualWorkDuration failed");
    logged |= errors;
}
/* Existing default: one render thread, raw present interval, target 33.3ms
 * unless TOTK_ADPF_TARGET_MS overrides it. Keep this experiment's control exact.
 * With TOTK_ADPF_GPU on, the same session reports the frame span from totk_perf_frame instead. */
void totk_perf_render(uint64_t interval_ns) {
    pthread_once(&config_once, configure);
    if (mode == 2) {
        static _Thread_local int registered;
        if (!registered) { totk_perf_thread("render"); registered = 1; }
        return;
    }
    if (mode != 1) return;
    pthread_mutex_lock(&mu);
    unsigned changes = 0;
    if (!session.attempted) {
        int32_t tid = (int32_t)syscall(SYS_gettid);
        changes = totk_adpf_sync(&session, &api, &tid, 1, span_applied ? totk_adpf_span_target(fps60, framegen, target_pct_applied) : legacy_target);
    }
    /* Legacy accepted intervals up to one second, including intentional pacing. */
    if (!span_applied && session.session && interval_ns && interval_ns < 1000000000ull && api.report(session.session, (int64_t)interval_ns))
        changes |= TOTK_ADPF_REPORT_ERROR;
    log_changes(changes); pthread_mutex_unlock(&mu);
}
int totk_perf_full(void) {
    pthread_once(&config_once, configure);
    return mode == 2;
}
/* TOTK_ADPF_GPU: the frame's first guest submit after the previous present fixes its start, the game's own frame
 * origin when it is usable (guest ticks, CLOCK_MONOTONIC epoch), else the submit itself. */
void totk_perf_submit(uint64_t origin_ticks) {
    pthread_once(&config_once, configure);
    if (atomic_load_explicit(&frame_start, memory_order_relaxed) || !span_on()) return;
    uint64_t none = 0, start = totk_adpf_frame_start(origin_ticks ? guest_clock_ticks_to_ns(origin_ticks) : 0, mono_ns());
    atomic_compare_exchange_strong_explicit(&frame_start, &none, start, memory_order_relaxed, memory_order_relaxed);
}
int totk_perf_span(void) {
    pthread_once(&config_once, configure);
    return span_on();
}
/* One acquire pacing wait (CLOCK_MONOTONIC); its part inside the span leaves it at the present. */
void totk_perf_pacing(uint64_t begin, uint64_t end) {
    pthread_once(&config_once, configure);
    if (!span_on()) return;
    pthread_mutex_lock(&mu);
    totk_adpf_pacing_add(&span_pacing, begin, end);
    pthread_mutex_unlock(&mu);
}
/* The acquire thread brackets only native present/texture queue waits, not acquire pacing. */
uint64_t totk_perf_wait_begin(void) {
    pthread_once(&config_once, configure);
    return api.work && atomic_load_explicit(&cpu_split_applied, memory_order_relaxed) && span_on() ? mono_ns() : 0;
}
void totk_perf_wait_end(uint64_t begin) {
    if (!begin) return;
    uint64_t end = mono_ns();
    pthread_mutex_lock(&mu);
    totk_adpf_pacing_add(&span_queue_wait, begin, end);
    pthread_mutex_unlock(&mu);
}
/* The completion thread, once per completed slot in submission order. */
void totk_perf_gpu(uint64_t frame, uint64_t gpu_ns) {
    pthread_once(&config_once, configure);
    if (!api.work) return;
    if (!span_on()) { gpu_acc = (totk_adpf_gpu_acc){0}; return; }   /* switched on again: the next whole frame */
    uint64_t closed = totk_adpf_gpu_slot(&gpu_acc, frame, gpu_ns);
    if (closed) atomic_store_explicit(&gpu_published, closed, memory_order_relaxed);
}
static void span_report(uint64_t start, uint64_t span, uint64_t queue_wait) {   /* mu */
    int load = totk_live_cached(&load_live, "TOTK_ADPF_LOAD_HINT", load_launch);
    if (load != load_applied) {
        load_applied = load; load_history.over = 0;
        fprintf(stderr, "[totk-perf] TOTK_ADPF_LOAD_HINT=%d (live): %s\n", load,
            api.increase ? "API available; needs TOTK_ADPF_GPU=1" : "unavailable, no-op");
    }
    span_stats.frames++;
    if (span && session.session) {
        uint64_t gpu = api.work ? totk_adpf_gpu_take(atomic_load_explicit(&gpu_published, memory_order_relaxed), &gpu_seen) : 0;
        if (load && api.increase && totk_adpf_load_step(&load_history, mono_ns(), span, session.target)) {
            int result = api.increase(session.session, true, totk_adpf_load_gpu(gpu, session.target), "totk-frame-load");
            if (result) span_stats.hint_errors++;
            else span_stats.hints++;
        }
        unsigned changes = totk_adpf_report_span(&session, &api, start, span,
            totk_adpf_cpu(span, queue_wait, cpu_split_applied), gpu);
        span_stats.spans++; span_stats.span_sum += span;
        if (span > span_stats.span_max) span_stats.span_max = span;
        if (gpu) {
            span_stats.gpus++; span_stats.gpu_sum += gpu;
            if (gpu > span_stats.gpu_max) span_stats.gpu_max = gpu;
        }
        span_stats.errors += !!changes;
        log_changes(changes);
    }
    else load_history.over = 0;
    if (span_stats.frames < 300) return;
    fprintf(stderr, "[totk-perf] ADPF span: %u frames, %u reported, span avg %.2f max %.2f ms; GPU %u reported, avg %.2f max %.2f ms; target %.3fms; %u report errors; increase hints %u sent, %u errors\n",
        span_stats.frames, span_stats.spans, span_stats.spans ? span_stats.span_sum / 1e6 / span_stats.spans : 0.0,
        span_stats.span_max / 1e6, span_stats.gpus, span_stats.gpus ? span_stats.gpu_sum / 1e6 / span_stats.gpus : 0.0,
        span_stats.gpu_max / 1e6, session.target / 1e6, span_stats.errors, span_stats.hints, span_stats.hint_errors);
    memset(&span_stats, 0, sizeof span_stats);
}
void totk_perf_generated(void) { atomic_fetch_add_explicit(&generated_presents, 1, memory_order_relaxed); }
/* The rate hint, once per guest present: the configured rate, or with TOTK_RATE_HINT_HOLD the output rate held. */
static void rate_frame(int guest_interval, int64_t target) {
    int hold_ms = totk_live_cached(&hold_live, "TOTK_RATE_HINT_HOLD", hold_launch);
    if (hold_ms != hold_logged) {
        fprintf(stderr, "[totk-perf] TOTK_RATE_HINT_HOLD=%d ms (live)\n", hold_ms);
        hold_logged = hold_ms; memset(&rate_hold, 0, sizeof rate_hold);
    }
    unsigned hz = totk_frame_rate(target == 16666667 ? 60 : 30, framegen, lattice);
    if (hold_ms > 0) {
        uint64_t generated = atomic_load_explicit(&generated_presents, memory_order_relaxed);
        int generating = generated != generated_seen;
        generated_seen = generated;
        hz = totk_rate_hold_step(&rate_hold, totk_output_rate(guest_interval, generating, lattice),
            mono_ns(), (uint64_t)hold_ms * 1000000);
    }
    atomic_store_explicit(&content_rate, hz, memory_order_relaxed);
}
void totk_perf_frame(uint64_t now, uint64_t period, uint64_t pacing, int guest_interval) {
    pthread_once(&config_once, configure);
    runtime_thermal_frame(guest_interval > 0 ? (unsigned)guest_interval : 1u);
    int64_t target = totk_adpf_target(period, fps60, framegen);
    if (rate_on) rate_frame(guest_interval, target);
    if (!mode) return;
    int span = span_on();
    if (mode != 2 && !span && !span_applied) return;   /* the legacy session reports from totk_perf_render */
    /* The frame boundary: the next submit starts the next frame. */
    uint64_t end = mono_ns(), start = atomic_exchange_explicit(&frame_start, 0, memory_order_relaxed);
    if (mode == 2) {
        static _Thread_local int registered;
        if (!registered) { totk_perf_thread("render"); registered = 1; }
    }
    pthread_mutex_lock(&mu);
    uint64_t inside = totk_adpf_pacing_in_span(&span_pacing, start, end);
    span_pacing.n = 0;
    uint64_t queue_wait = totk_adpf_pacing_in_span(&span_queue_wait, start, end);
    span_queue_wait.n = 0;
    int split = totk_live_cached(&cpu_split_live, "TOTK_ADPF_CPU_SPLIT", cpu_split_launch);
    if (split != cpu_split_applied) {
        atomic_store_explicit(&cpu_split_applied, split, memory_order_relaxed);
        queue_wait = 0; /* first newly enabled frame has no complete queue-wait history */
        fprintf(stderr, "[totk-perf] TOTK_ADPF_CPU_SPLIT=%d (live): %s\n", split,
            api.work ? "CPU=span minus present/texture queue wait; total unchanged" : "unavailable, no-op");
    }
    int pct = totk_live_cached(&target_pct_live, "TOTK_ADPF_TARGET_PCT", target_pct_launch);
    if (pct != target_pct_applied) {
        target_pct_applied = pct; load_history.over = 0;
        fprintf(stderr, "[totk-perf] TOTK_ADPF_TARGET_PCT=%d (live): target %.3fms; needs TOTK_ADPF_GPU=1\n",
            pct, totk_adpf_span_target(fps60, framegen, pct) / 1e6);
    }
    if (span != span_applied) {
        span_applied = span; memset(&span_stats, 0, sizeof span_stats);
        load_history.over = 0;
        /* A GPU frame published before the switch is not this session's to report. */
        gpu_seen = (uint32_t)(atomic_load_explicit(&gpu_published, memory_order_relaxed) >> 32);
        log_span(span, "live");
        start = 0;   /* this frame's start may predate the switch: the next frame is the first whole one */
    }
    if (span) target = totk_adpf_span_target(fps60, framegen, target_pct_applied);
    else if (mode == 1) target = legacy_target;
    if (mode == 1) {
        if (session.count && target != session.target)
            log_changes(totk_adpf_sync(&session, &api, session.tids, session.count, target));
        if (span) span_report(start, totk_adpf_span(start, end, inside), queue_wait);
        pthread_mutex_unlock(&mu);
        return;
    }
    uint64_t work = totk_adpf_work(previous_frame, now, pacing);
    previous_frame = now;
    if (registry_dirty || !frames || ++frames % 300 == 0) {
        uint64_t elapsed = sample_time && now > sample_time ? now - sample_time : 0;
        for (unsigned i = 0; i < 256; i++) if (registry[i].tid) {
            uint64_t cpu = cpu_time(registry[i].clock);
            registry[i].used = cpu >= registry[i].cpu && registry[i].cpu ? cpu - registry[i].cpu : 0;
            registry[i].cpu = cpu;
        }
        int32_t tids[TOTK_ADPF_MAX_THREADS]; size_t n = 0;
        for (unsigned i = 0; i < 256 && n < TOTK_ADPF_MAX_THREADS; i++)
            if (registry[i].tid && registry[i].critical) tids[n++] = registry[i].tid;
        /* Up to six busy guest/job threads, not idle I/O or background compilers. */
        for (unsigned k = 0; k < 6 && n < TOTK_ADPF_MAX_THREADS && elapsed; k++) {
            int best = -1;
            for (unsigned i = 0; i < 256; i++) if (registry[i].tid && !registry[i].critical &&
                    registry[i].used >= elapsed * 15 / 100 && (best < 0 || registry[i].used > registry[best].used)) best = (int)i;
            if (best < 0) break;
            tids[n++] = registry[best].tid; registry[best].used = 0;
        }
        for (size_t i = 1; i < n; i++) for (size_t j = i; j && tids[j] < tids[j - 1]; j--) {
            int32_t t = tids[j]; tids[j] = tids[j - 1]; tids[j - 1] = t;
        }
        log_changes(totk_adpf_sync(&session, &api, tids, n, target));
        sample_time = now; registry_dirty = 0; frames = 1;
    } else if (target != session.target) {
        log_changes(totk_adpf_sync(&session, &api, session.tids, session.count, target));
    }
    if (span) span_report(start, totk_adpf_span(start, end, inside), queue_wait);
    else log_changes(totk_adpf_report(&session, &api, work));
    pthread_mutex_unlock(&mu);
}
unsigned totk_perf_rate(void) {
    pthread_once(&config_once, configure);
    return rate_on ? atomic_load_explicit(&content_rate, memory_order_relaxed) : 0;
}
void totk_perf_window(ANativeWindow *window) {
    unsigned hz = totk_perf_rate();
    if (!hz || !window) return;
    int result = window_rate ? window_rate(window, (float)hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) : -1;
    fprintf(stderr, "[totk-perf] window frame-rate hint=%u FIXED_SOURCE result=%d (request, not observed mode)\n", hz, result);
}
void totk_perf_surface(ASurfaceTransaction *tx, ASurfaceControl *surface) {
    unsigned hz = totk_perf_rate();
    if (!hz || !surface || !tx) return;
    /* Only seamless switches: a rate request must not black-screen mid-play. */
    if (surface_rate) surface_rate(tx, surface, (float)hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 0);
    fprintf(stderr, "[totk-perf] SurfaceControl frame-rate hint=%u FIXED_SOURCE seamless-only %s (void API; not mode proof)\n",
        hz, surface_rate ? "staged" : "unavailable");
}
#endif

/* The Android app's frontend (NativeActivity subclassed by the Java TotkActivity, which the setup launcher
   com.aure.totk.setup.SetupActivity starts once the game is set up): the game runs inside the app process as on iOS.
   It shares the boot log and nn::vi of frontend_headless.c (included below) and adds:
   - the game folder: the app's folder on shared storage (/sdcard/Android/data/<package>/files), holding what the app's
     setup writes there (install.json, rewritten/, game-data/, extracted/, shaders/, companion/) or build_android.py puts
     there for a developer build, plus the player's optional driver.so and totk.env (TOTK_* options, one NAME=value per
     line, like the variables run.sh takes in front). Without install.json saying "playable" the game does not start;
   - the Vulkan driver: the player's driver.so (copied into private storage), else the Turnip the APK bundles
     (libtotk_turnip.so beside this library, used in place), else the system's;
   - the log: stdout and stderr go to totk.log in that folder, which a low-priority thread copies to logcat (tag "totk",
     log_mirror.h; TOTK_LOGCAT=1 opts into the copy, off by default);
   - live switches: <game folder>/totk.ctl, followed by another low-priority thread and applied at the next presented
     frame (live_ctl.h; tools/bench/live.sh writes it);
   - the window: handed to the Vulkan backend's AHB presenter or automatic copy fallback (vk_draw.c);
   - input: the built-in controller (the AYN Thor's) or any gamepad, mapped to the Switch's buttons by position;
   - a full-screen landscape window kept on while the game runs.
   The game starts once, when the activity is first created, on a thread of its own (totk_host_main is loader.c's
   main, renamed by the build); closing the activity ends the process. */
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include "frontend_headless.c"
#include "autoload.h"
#include "../android/gfx/display_pace.h"
#include "../android/gfx/display_clock.h"
#include "../android/gfx/acquire_clock.h"
#include "../android/gfx/predict_pace.h"
#include "../android/gfx/vk_present_slots.h"
#include "present_predict.h"
#include "game_error.h"
#include "predict_budget.h"
#include "fps_auto_runtime.h"
#include "../android/gfx/fps_auto.h"
#include "../android/gfx/fps_auto_scene.h"
#include <math.h>
#include "../android/gfx/vk_pcache.h"
#include <stdatomic.h>
#include "present_clock.h"
#include "thread_place.h"
#include "log_mirror.h"
#include "live_ctl.h"
#include "thread_sampler.h"
#include "build_kind.h"
#include "android_perf.h"
#include <android/api-level.h>
#include <android/choreographer.h>
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <android/window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "adreno_gmem.h"
#include "overlay_chord.h"
int totk_host_main(int argc, char **argv);
#include "android_pause.h"
static AndroidGamePause game_pause = ANDROID_GAME_PAUSE_INIT;
int runtime_android_game_paused(void) {
    return atomic_load_explicit(&game_pause.paused, memory_order_acquire);
}
AndroidGameSuspension runtime_android_game_suspension(void) {
    return android_game_pause_suspension(&game_pause);
}
void runtime_android_game_pause_wait(void) {
    if (!runtime_android_game_paused() || runtime_fs_save_pending()) return;
    uint64_t begin = runtime_now_ns();
    fprintf(stderr, "[totk-app] paused at guest frame boundary\n");
    android_game_pause_wait(&game_pause, runtime_fs_save_pending);
    fprintf(stderr, "[totk-app] frame boundary released after %.3f s (%s)\n",
            (runtime_now_ns() - begin) / 1e9, runtime_android_game_paused() ? "finishing save" : "resumed");
}
uint64_t vk_window_set(ANativeWindow *window, ANativeWindow *surface_parent, int display_id);   /* android/gfx/vk_draw.c */
void vk_window_resized(ANativeWindow *window, uint64_t epoch);   /* android/gfx/vk_draw.c */
void vk_window_transform_hint(ANativeWindow *window, uint64_t epoch, int hint);   /* android/gfx/vk_draw.c */
void vk_shader_seed_startup(const char *game_dir);   /* android/gfx/vk_shader.c */
int vk_overlay_cycle(void);   /* android/gfx/vk_overlay.c */
/* Runtime policy is linked only by the Vulkan backend, unlike this frontend.
 * Lifecycle may precede env setup; its notification must not select the mode. */
extern void runtime_fps_adaptive_lifecycle(int window, int paused) __attribute__((weak));
extern void runtime_fps_adaptive_pipeline(uint64_t now) __attribute__((weak));
extern void runtime_fps_adaptive_grace(uint64_t now, uint64_t duration) __attribute__((weak));
extern void vk_gpuprof_identity(const char *commit, const char *package, uint64_t version, const char *build) __attribute__((weak));
extern void vk_gpuprof_shutdown(void) __attribute__((weak));

static char g_data[1024];                    /* the game folder (shared storage) */
static ANativeActivity *g_activity;
ANativeActivity *runtime_android_activity(void) { return g_activity; }   /* runtime/feedback_android.c */
void runtime_android_feedback_hardware(ANativeActivity *a);              /* runtime/feedback_android.c */
const char *runtime_android_private_files(void) { return g_activity ? g_activity->internalDataPath : NULL; }

/* One Looper owns the native Choreographer. Clock-only acquisition/presentation
 * must not change the legacy VI source: only TOTK_PACE=display selects real VI
 * signals. The fixed acquire lattice is independent of display_pace_take(). */
static int display_pace_on, display_pace_ready, present_slots_on, acquire_clock_on, predict_requested;
static int present_slots_auto, display_refresh_observed;
static _Atomic int predict_on;
static pthread_mutex_t display_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t display_cv = PTHREAD_COND_INITIALIZER;
static display_pacer display_clock;
static acquire_clock acquisition_clock, slack_budget_clock;
static int64_t display_period_ns = 16666667;
static uint64_t display_nominal_period_ns = 16666667;
static display_period_clock display_callback_observation, display_fence_observation;
static uint64_t display_callback_measured_ns, display_fence_measured_ns, display_clock_report_ns;
static unsigned display_callback_calibrated, display_fence_calibrated;
static uint64_t display_refresh_frame_ns;
static unsigned present_pending;
static pthread_cond_t predict_cv;   /* CLOCK_MONOTONIC, initialized before callbacks start */

/* API33 functions are resolved only for the exact opt-in. The Looper owns the
 * callback variant for its entire lifetime; display_mu owns every snapshot and
 * its refresh/lifecycle epoch. No callback-data pointer escapes the callback. */
static _Atomic int present_timeline_on;
static _Atomic int adaptive_slots_requested, adaptive_slots_on;
static struct {
    void (*post_vsync)(AChoreographer *, AChoreographer_vsyncCallback, void *);
    int64_t (*frame_time)(const AChoreographerFrameCallbackData *);
    size_t (*length)(const AChoreographerFrameCallbackData *);
    size_t (*preferred)(const AChoreographerFrameCallbackData *);
    AVsyncId (*vsync_id)(const AChoreographerFrameCallbackData *, size_t);
    int64_t (*expected)(const AChoreographerFrameCallbackData *, size_t);
    int64_t (*deadline)(const AChoreographerFrameCallbackData *, size_t);
} present_timeline_api;
static runtime_present_timelines present_timeline_snapshot;
static uint64_t present_timeline_generation = 1;
static uint64_t adaptive_slots_epoch_ns;
static int present_timeline_window, present_timeline_paused;
static void present_timeline_invalidate_locked(void) {
    present_timeline_generation++;
    present_timeline_snapshot.count = 0;
    if (runtime_android_adaptive_slots_active())
        adaptive_slots_epoch_ns = runtime_now_ns();
}
/* -1 leaves that lifecycle facet unchanged. Record events even before startup
 * admission, since NativeActivity may publish its first window first. The stall
 * watchdog (threads.c) also pauses without a window or in the background. */
static void present_timeline_lifecycle(int window, int paused) {
    pthread_mutex_lock(&display_mu);
    if (window >= 0) present_timeline_window = window;
    if (paused >= 0) present_timeline_paused = paused;
    present_timeline_invalidate_locked();
    int watch_paused = !present_timeline_window || present_timeline_paused;
    pthread_mutex_unlock(&display_mu);
    runtime_progress_pause(watch_paused);
    if (runtime_fps_adaptive_lifecycle) runtime_fps_adaptive_lifecycle(window, paused);
}

typedef struct {
    uint64_t period, nominal, callback, fence, phase;
    unsigned source;
    int valid;
} display_clock_report;
/* Call under display_mu, print after unlocking. Calibration/source changes are
 * reported once, then at most once/minute; no per-frame arrival-clock reads. */
static display_clock_report display_clock_report_locked(uint64_t stamp, int force) {
    display_clock_report r = {0};
    if (!force && display_clock_report_ns &&
        (stamp < display_clock_report_ns || stamp - display_clock_report_ns < 60000000000ull)) return r;
    display_clock_report_ns = stamp;
    return (display_clock_report){.period = (uint64_t)display_period_ns,
        .nominal = display_nominal_period_ns, .callback = display_callback_measured_ns,
        .fence = display_fence_measured_ns, .phase = display_clock.frame_ns,
        .source = display_fence_calibrated ? 2u : display_callback_calibrated ? 1u : 0u, .valid = 1};
}
static void display_clock_report_print(const display_clock_report *r) {
    if (!r->valid) return;
    fprintf(stderr, "[totk-pace] measured display clock: effective %.6f Hz (%llu ns) source=%s nominal=%llu ns callback=%llu ns present-fence=%llu ns phase-callback=%llu ns; fixed-slots/slack/predict share period and callback phase\n",
            1e9 / r->period, (unsigned long long)r->period,
            r->source == 2 ? "present-fence" : r->source == 1 ? "Choreographer" : "nominal-warmup",
            (unsigned long long)r->nominal, (unsigned long long)r->callback,
            (unsigned long long)r->fence, (unsigned long long)r->phase);
}
void runtime_android_display_presented(uint64_t display_ns) {
    if (!display_ns) return;
    pthread_mutex_lock(&display_mu);
    display_clock_result r = display_clock_observe(&display_fence_observation, display_ns, display_nominal_period_ns);
    display_fence_measured_ns = r.measured_ns;
    int force = r.calibrated && !display_fence_calibrated;
    if (r.calibrated) {
        display_fence_calibrated = 1;
        if ((uint64_t)display_period_ns != r.refresh_ns) {
            display_period_ns = (int64_t)r.refresh_ns;
            display_refresh_frame_ns = display_clock.frame_ns;
            present_timeline_invalidate_locked();
            force = 1; /* Future reservations wait for a fresh callback. */
        }
    }
    display_clock_report report = display_clock_report_locked(display_ns, force);
    pthread_mutex_unlock(&display_mu);
    display_clock_report_print(&report);
}
static struct {
    predict_pacer model[2];
    vk_present_slots slots;
    uint64_t generation, sequence, refresh;
    unsigned interval, path;
    struct { uintptr_t texture; runtime_present_prediction plan; } pending[16];
    uint64_t leads[300], first_id, last_id, excluded_ns;
    uint64_t issued_lead_ns[2];
    int64_t errors[300];
    unsigned reports, valid, misses, wall_misses, cancelled, stale, mismatched, resets;
    unsigned display_samples, display_misses;
} prediction;

/* Shared with presenter completion under display_mu; changes are applied only
 * by the acquire owner, before its next immutable display-slot reservation. */
static _Atomic int fps_auto_on;
typedef struct {
    uint64_t span_sum, span_max, cpu_sum, tail_sum, blocked_sum;
    unsigned n, relaxed_misses;
} fps_auto_trace;
static struct {
    fps_auto_policy policy;
    fps_auto_scene scene;
    fps_auto_trace trace;
    uint64_t epoch, sequence, last_present, last_blocked;
    uint32_t last_owner;
    unsigned guest_interval, suspended, inhibited;
    struct { uintptr_t texture; runtime_fps_auto_frame frame; } pending[16];
} fps_auto;

int runtime_android_fps_auto_active(void) { return atomic_load_explicit(&fps_auto_on, memory_order_relaxed); }
static void fps_auto_reset_locked(uint64_t now, uint64_t refresh) {
    fps_auto_reset(&fps_auto.policy, now, refresh);
    fps_auto.epoch++;
    fps_auto.last_present = 0;
    fps_auto.last_blocked = 0; fps_auto.last_owner = 0;
    fps_auto.trace = (fps_auto_trace){0};
}
static void fps_auto_inhibit_locked(uint64_t now, const char *reason) {
    if (fps_auto.inhibited) return;
    fps_auto_inhibit(&fps_auto.policy, now);
    fps_auto.epoch++;
    fps_auto.inhibited = 1;
    fps_auto.trace = (fps_auto_trace){0};
    fprintf(stderr, "[totk-fps-auto] decisions inhibited: %s; target %u fps retained, stale window/queued feedback discarded\n",
            reason, fps_auto.policy.interval == 1 ? 60 : 30);
}
void runtime_android_fps_auto_pipeline(uint64_t now) {
    if (runtime_fps_adaptive_pipeline) runtime_fps_adaptive_pipeline(now);
    if (!runtime_android_fps_auto_active()) return;
    pthread_mutex_lock(&display_mu);
    if (fps_auto_scene_pipeline(&fps_auto.scene, now))
        fps_auto_inhibit_locked(now, ">=8 pipeline creations within 1s; 10s settle");
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_fps_auto_grace(uint64_t now, uint64_t duration, const char *reason) {
    if (runtime_fps_adaptive_grace) runtime_fps_adaptive_grace(now, duration);
    if (!runtime_android_fps_auto_active() || !duration) return;
    pthread_mutex_lock(&display_mu);
    fps_auto_scene_grace(&fps_auto.scene, now, duration);
    fps_auto_inhibit_locked(now, reason);
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_fps_auto_scene(uintptr_t texture, uint64_t now, uint64_t draws, int loading) {
    if (!runtime_android_fps_auto_active()) return;
    static const char *const reasons[] = {"ready", "before first world (>2000 draws)",
        "game loading screen", "draws below scene floor", "10s scene/pipeline settle"};
    pthread_mutex_lock(&display_mu);
    unsigned reason = fps_auto_scene_observe(&fps_auto.scene, now, draws, loading);
    if (reason != FPS_SCENE_READY) fps_auto_inhibit_locked(now, reasons[reason]);
    else if (fps_auto.inhibited) {
        /* Inhibition may have lasted minutes. Resume starts healthy evidence
         * NOW: loading/idle wall time cannot pay off failed-return debt. */
        fps_auto_inhibit(&fps_auto.policy, now);
        fps_auto.epoch++;
        fps_auto.trace = (fps_auto_trace){0};
        fps_auto.inhibited = 0;
        fprintf(stderr, "[totk-fps-auto] decisions resumed: stable world draws %llu, scene reference %llu; fresh evidence required\n",
                (unsigned long long)draws, (unsigned long long)fps_auto.scene.scene_draws);
    }
    for (unsigned i = 0; i < 16; i++) if (fps_auto.pending[i].texture == texture) {
        fps_auto.pending[i].frame.scene_eligible = reason == FPS_SCENE_READY;
        break;
    }
    pthread_mutex_unlock(&display_mu);
}
static void fps_auto_configure(void) {
    const char *request = getenv("TOTK_FPS_AUTO");
    if (!request || !strcmp(request, "0")) return;
    const char *fps60 = getenv("TOTK_FPS60"), *rt = getenv("TOTK_REALTIME");
    const char *uncapped = getenv("TOTK_UNCAPPED"), *det = getenv("DM_DETCLOCK");
    const char *hz = getenv("DM_PRESENT_HZ"), *interval = getenv("DM_PRESENT_INTERVAL");
    const char *mfx = getenv("TOTK_MFX");
    const char *reason = strcmp(request, "1") ? "expected TOTK_FPS_AUTO=0 or 1" :
        !fps60 || strcmp(fps60, "1") ? "requires TOTK_FPS60=1" :
        rt && !strcmp(rt, "0") ? "requires measured TOTK_REALTIME timing" :
        !present_slots_on ? "requires TOTK_PRESENT_SLOTS=1 for boundary-safe transitions" :
        uncapped && strcmp(uncapped, "0") ? "incompatible with uncapped pacing" :
        det && *det && strcmp(det, "0") ? "incompatible with deterministic timing" :
        hz && strcmp(hz, "60") ? "requires native 60-Hz pacing (DM_PRESENT_HZ unset or 60)" :
        interval && !strcmp(interval, "display") ? "incompatible with DM_PRESENT_INTERVAL=display" :
        mfx && *mfx && strcmp(mfx, "0") ? "incompatible with frame interpolation" : NULL;
    if (reason) {
        fprintf(stderr, "[totk-fps-auto] ignored: %s; existing interval policy retained\n", reason);
        return;
    }
    fps_auto_reset_locked(runtime_now_ns(), (uint64_t)display_period_ns);
    atomic_store_explicit(&fps_auto_on, 1, memory_order_relaxed);
    fprintf(stderr, "[totk-fps-auto] enabled: initial 60fps; drop only raw game-present rate <55fps in two independent 5s eligible windows; recover >=95%% within refresh+2ms over 8s; 10s world/pipeline settle; minimum dwell 5s; measured game time and fixed slots retained\n");
}
unsigned runtime_android_fps_auto_acquire(unsigned guest_interval) {
    if (!runtime_android_fps_auto_active()) return guest_interval;
    /* This host save-I/O signal is not a guessed game/UI state. Query it
     * outside display_mu; real Commit also supplies start/end grace hooks. */
    if (runtime_fs_save_busy())
        runtime_android_fps_auto_grace(runtime_now_ns(), 1000000000ull, "host save writes in progress");
    pthread_mutex_lock(&display_mu);
    uint64_t now = runtime_now_ns(), refresh = (uint64_t)display_period_ns;
    int refresh60 = refresh >= 16000000 && refresh <= 17300000;
    if (guest_interval < 1 || guest_interval > 2 || !refresh60) {
        if (!fps_auto.suspended) {
            fprintf(stderr, "[totk-fps-auto] suspended: guest interval %u, refresh %.3fms; guest request never shortened\n",
                    guest_interval, refresh / 1e6);
            fps_auto_reset_locked(now, refresh);
        }
        fps_auto.suspended = 1;
        fps_auto.guest_interval = 0;
        pthread_mutex_unlock(&display_mu);
        return guest_interval;
    }
    if (!fps_auto.guest_interval || fps_auto.policy.refresh_ns != refresh)
        fps_auto_reset_locked(now, refresh);
    fps_auto.guest_interval = guest_interval;
    fps_auto.suspended = 0;
    if (!fps_auto.scene.world_seen || now < fps_auto.scene.hold_until_ns)
        fps_auto_inhibit_locked(now, "scene not ready or 10s settle");
    unsigned old = fps_auto.policy.interval, samples = fps_auto.policy.count;
    unsigned misses = fps_auto.policy.misses;
    uint64_t generation = fps_auto.policy.generation;
    uint64_t coverage = fps_auto.policy.coverage_ns, dwell = now - fps_auto.policy.last_change_ns;
    double game_fps = fps_auto.policy.last_game_count > 1 ? fps_auto.policy.last_game_fps_milli / 1000.0 : NAN;
    if (fps_auto.policy.desired == 2 && old == 1) {
        samples = fps_auto.policy.last_drop_count;
        misses = fps_auto.policy.last_drop_misses;
        coverage = fps_auto.policy.last_drop_coverage_ns;
    }
    unsigned effective = fps_auto_apply(&fps_auto.policy, now);
    if (fps_auto.policy.generation != generation) fps_auto.epoch++;
    if (old != effective) {
        fps_auto.last_present = 0;
        fps_auto.trace = (fps_auto_trace){0};
        fprintf(stderr, "[totk-fps-auto] target %u -> %u fps at acquire/next-slot boundary: %s; last-5s-game-rate=%.3ffps evidence-events=%u/%u, coverage %.3fs dwell %.3fs refresh %.3fms, epoch %llu; queued slots unchanged, guest interval %u never shortened; recovery-backoff=%ux remaining-min-hold=%.3fs\n",
                old == 1 ? 60 : 30, effective == 1 ? 60 : 30,
                effective == 2 ? "two independent 5s raw game-rate windows <55fps, slow intervals span >=5s" : "8s counterfactual readiness >=95% within refresh+2ms",
                game_fps, misses, samples,
                coverage / 1e9, dwell / 1e9, refresh / 1e6, (unsigned long long)fps_auto.epoch, guest_interval,
                fps_auto.policy.return_backoff, fps_auto_recovery_wait_ns(&fps_auto.policy, now) / 1e9);
    }
    pthread_mutex_unlock(&display_mu);
    return guest_interval > effective ? guest_interval : effective;
}
void runtime_android_fps_auto_begin(uintptr_t texture, unsigned interval,
        uint64_t release_ns, uint64_t pacing_ns) {
    if (!runtime_android_fps_auto_active() || !texture) return;
    pthread_mutex_lock(&display_mu);
    if (fps_auto.guest_interval) {
        unsigned free_slot = 16;
        for (unsigned i = 0; i < 16; i++) {
            if (fps_auto.pending[i].texture == texture) { free_slot = i; break; }
            if (!fps_auto.pending[i].texture) free_slot = i;
        }
        if (free_slot == 16) runtime_fail("fps auto texture observation capacity");
        fps_auto.pending[free_slot].texture = texture;
        fps_auto.pending[free_slot].frame = (runtime_fps_auto_frame){
            .id = ++fps_auto.sequence, .generation = fps_auto.epoch,
            .release_ns = release_ns, .interval = interval, .pacing_ns = pacing_ns,
            .policy_interval = fps_auto.policy.interval,
            .blocked_at_release_ns = runtime_android_fps_auto_blocked_ns(),
            .owner = runtime_thread_current_handle()};
    }
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_fps_auto_present(uintptr_t texture, uint64_t now) {
    if (!runtime_android_fps_auto_active()) return;
    uint64_t blocked = runtime_android_fps_auto_blocked_ns();
    uint32_t owner = runtime_thread_current_handle();
    pthread_mutex_lock(&display_mu);
    for (unsigned i = 0; i < 16; i++) if (fps_auto.pending[i].texture == texture) {
        runtime_fps_auto_frame *f = &fps_auto.pending[i].frame;
        f->present_ns = now;
        f->game_interval_ns = fps_auto.last_present && now > fps_auto.last_present ?
            now - fps_auto.last_present : 0;
        if (owner && owner == f->owner && owner == fps_auto.last_owner &&
            blocked >= fps_auto.last_blocked && blocked >= f->blocked_at_release_ns) {
            f->blocked_ns = blocked - fps_auto.last_blocked;
            f->post_gate_blocked_ns = blocked - f->blocked_at_release_ns;
            f->blocked_valid = 1;
        }
        fps_auto.last_blocked = blocked; fps_auto.last_owner = owner;
        fps_auto.last_present = now;
        if (!fps_auto.inhibited && f->scene_eligible && f->generation == fps_auto.epoch &&
            f->policy_interval == fps_auto.policy.interval) {
            uint64_t generation = fps_auto.policy.generation;
            fps_auto_game_present(&fps_auto.policy, now);
            if (fps_auto.policy.generation != generation) {
                fps_auto.epoch++;
                fps_auto.trace = (fps_auto_trace){0};
            }
        }
        break;
    }
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_fps_auto_take(uintptr_t texture, runtime_fps_auto_frame *out) {
    *out = (runtime_fps_auto_frame){0};
    if (!runtime_android_fps_auto_active()) return;
    pthread_mutex_lock(&display_mu);
    for (unsigned i = 0; i < 16; i++) if (fps_auto.pending[i].texture == texture) {
        *out = fps_auto.pending[i].frame;
        fps_auto.pending[i].texture = 0;
        break;
    }
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_fps_auto_feedback(const runtime_fps_auto_frame *f,
        uint64_t ready, uint64_t preparation, uint64_t allowance) {
    if (!runtime_android_fps_auto_active() || !f->id) return;
    fps_auto_trace report = {0};
    uint64_t epoch = 0, recovery_wait = 0, game_fps_milli = 0;
    unsigned interval = 0, count = 0, misses = 0, streak = 0, backoff = 1;
    pthread_mutex_lock(&display_mu);
    if (f->generation == fps_auto.epoch && fps_auto.guest_interval && !fps_auto.inhibited &&
        f->scene_eligible && f->policy_interval == fps_auto.policy.interval) {
        uint64_t span = f->blocked_valid ? runtime_fps_auto_ready_span(f, ready, preparation, allowance) : 0;
        uint64_t busy = span ? runtime_fps_auto_busy_span(f) : 0;
        uint64_t generation = fps_auto.policy.generation;
        /* These wait-stripped demand samples drive recovery/diagnostics only.
         * Downshift is decided by the independent raw game-present stream. */
        fps_auto_observation o = fps_auto_observe(&fps_auto.policy, ready, span, span ? (busy ? busy : 1) : 0);
        if (fps_auto.policy.generation != generation) {
            fps_auto.epoch++;
            fps_auto.trace = (fps_auto_trace){0};
        } else if (o.valid) {
            fps_auto.trace.n++;
            fps_auto.trace.span_sum += span;
            if (span > fps_auto.trace.span_max) fps_auto.trace.span_max = span;
            fps_auto.trace.cpu_sum += busy;
            fps_auto.trace.blocked_sum += f->blocked_ns;
            fps_auto.trace.tail_sum += ready - f->present_ns;
            fps_auto.trace.relaxed_misses += span > fps_auto.policy.refresh_ns + FPS_AUTO_RECOVER_MARGIN_NS;
            if (fps_auto.trace.n == 300) {
                report = fps_auto.trace;
                fps_auto.trace = (fps_auto_trace){0};
                epoch = fps_auto.epoch; interval = fps_auto.policy.interval;
                count = fps_auto.policy.count; misses = fps_auto.policy.misses;
                streak = fps_auto.policy.drop_streak;
                backoff = fps_auto.policy.return_backoff;
                recovery_wait = fps_auto_recovery_wait_ns(&fps_auto.policy, runtime_now_ns());
                game_fps_milli = fps_auto.policy.last_game_fps_milli;
            }
        }
    }
    pthread_mutex_unlock(&display_mu);
    if (report.n)
        fprintf(stderr, "[totk-fps-auto] readiness epoch=%llu target=%u fps n=%u joined mean/max=%.3f/%.3fms CPU-production-busy mean=%.3fms kernel-blocked mean=%.3fms GPU-tail mean=%.3fms beyond-refresh+2ms=%u/%u; readiness-window misses=%u/%u raw-game-rate-last5s=%.3ffps drop-streak=%u/2 recovery-backoff=%ux remaining-min-hold=%.3fs; eligible-only, no presenter residence\n",
                (unsigned long long)epoch, interval == 1 ? 60 : 30, report.n,
                report.span_sum / report.n / 1e6, report.span_max / 1e6,
                report.cpu_sum / report.n / 1e6, report.blocked_sum / report.n / 1e6, report.tail_sum / report.n / 1e6,
                report.relaxed_misses, report.n, misses, count, game_fps_milli / 1000.0, streak, backoff, recovery_wait / 1e9);
}

int runtime_android_display_pace_active(void) { return display_pace_on; }
int runtime_android_display_hz(void) {
    pthread_mutex_lock(&display_mu);
    int hz = (int)((1000000000ll + display_period_ns / 2) / display_period_ns);
    pthread_mutex_unlock(&display_mu);
    return hz;
}
int runtime_android_display_wait(unsigned interval) {
    if (!display_pace_on) return 0;
    uint64_t start = runtime_now_ns();
    pthread_mutex_lock(&display_mu);
    /* No timeout-to-unpaced fallback: if Android suspends callbacks, rendering
     * pauses too. GPU fences below acquire still protect texture lifetime. */
    while (!display_pace_take(&display_clock, interval))
        pthread_cond_wait(&display_cv, &display_mu);
    static uint64_t frames, waited_ns, max_wait_ns;
    static unsigned last_interval;
    uint64_t waited = runtime_now_ns() - start;
    waited_ns += waited; if (waited > max_wait_ns) max_wait_ns = waited;
    frames++;
    if (interval != last_interval || frames % 300 == 0) {
        fprintf(stderr, "[totk-pace] display frame %llu: interval %u, refresh %.3f Hz, wait %.3f ms (max %.3f ms); vsync %llu\n",
                (unsigned long long)frames, interval, 1e9 / display_period_ns,
                waited_ns / (double)(frames % 300 ? frames % 300 : 300) / 1e6, max_wait_ns / 1e6,
                (unsigned long long)display_clock.tick);
        last_interval = interval;
    }
    if (frames % 300 == 0) waited_ns = max_wait_ns = 0;
    pthread_mutex_unlock(&display_mu);
    return 1;
}
int runtime_android_acquire_clock_active(void) { return acquire_clock_on; }
uint64_t runtime_android_display_period(unsigned interval) {
    if (!display_pace_on && !present_slots_on && !acquire_clock_on) return 0;
    if (!interval) interval = 1;
    pthread_mutex_lock(&display_mu);
    uint64_t refresh = (uint64_t)display_period_ns;
    uint64_t period = display_pace_ready && display_clock.frame_ns &&
        refresh <= UINT64_MAX / interval ? refresh * interval : 0;
    pthread_mutex_unlock(&display_mu);
    return period;
}
int runtime_android_display_period_observe(runtime_display_period_observation *out) {
    if (!out) return 0;
    pthread_mutex_lock(&display_mu);
    *out = (runtime_display_period_observation){.effective_ns = (uint64_t)display_period_ns,
        .nominal_ns = display_nominal_period_ns, .callback_measured_ns = display_callback_measured_ns,
        .fence_measured_ns = display_fence_measured_ns,
        .callback_calibrated = display_callback_calibrated, .fence_calibrated = display_fence_calibrated};
    pthread_mutex_unlock(&display_mu);
    return 1;
}
uint64_t runtime_android_acquire_period(unsigned interval) {
    if (!acquire_clock_on) return 0;
    return runtime_android_display_period(interval);
}
int runtime_android_acquire_wait(unsigned interval) {
    if (!acquire_clock_on) return 0;
    uint64_t arrival = runtime_now_ns();
    pthread_mutex_lock(&display_mu);
    /* After a real mode change, use the first callback on the new lattice,
     * not the prior mode's last timestamp with the new period attached. */
    while (!display_clock.frame_ns || display_clock.frame_ns <= display_refresh_frame_ns)
        pthread_cond_wait(&display_cv, &display_mu);
    acquire_clock_release r = acquire_clock_reserve(&acquisition_clock, 1,
        display_clock.frame_ns, (uint64_t)display_period_ns, interval, arrival);
    uint64_t late = r.late_ns;
    unsigned reanchor = r.reanchor;
    for (;;) {
        if (r.mode != ACQUIRE_CLOCK_ACTIVE) runtime_fail("fixed acquire clock period");
        if (r.reached) break;
        pthread_cond_wait(&display_cv, &display_mu);
        if (display_clock.frame_ns <= display_refresh_frame_ns) continue;
        r = acquire_clock_observe(&acquisition_clock, display_clock.frame_ns,
                                   (uint64_t)display_period_ns, arrival);
        if (r.late_ns > late) late = r.late_ns;
        reanchor |= r.reanchor;
    }
    static uint64_t frames, waited_ns, max_wait_ns, late_frames, max_late_ns;
    static uint64_t starts, stalls, refreshes;
    uint64_t waited = runtime_now_ns() - arrival;
    frames++; waited_ns += waited;
    if (waited > max_wait_ns) max_wait_ns = waited;
    if (late) late_frames++;
    if (late > max_late_ns) max_late_ns = late;
    starts += !!(reanchor & ACQUIRE_CLOCK_START);
    stalls += !!(reanchor & ACQUIRE_CLOCK_STALL);
    refreshes += !!(reanchor & ACQUIRE_CLOCK_REFRESH);
    if (frames % 300 == 0) {
        fprintf(stderr, "[totk-pace] acquire clock frame %llu: interval %u, refresh %.3f Hz, target %llu ns; wait avg %.3f ms max %.3f ms; late %llu/300 max %.3f ms; reanchors total start %llu stall %llu refresh %llu\n",
                (unsigned long long)frames, acquisition_clock.interval, 1e9 / display_period_ns,
                (unsigned long long)r.target_ns, waited_ns / 300.0 / 1e6, max_wait_ns / 1e6,
                (unsigned long long)late_frames, max_late_ns / 1e6,
                (unsigned long long)starts, (unsigned long long)stalls, (unsigned long long)refreshes);
        waited_ns = max_wait_ns = late_frames = max_late_ns = 0;
    }
    pthread_mutex_unlock(&display_mu);
    return 1;
}
int runtime_android_present_clock(uint64_t *frame_ns, uint64_t *refresh_ns) {
    if (!present_slots_on && !present_slots_auto) return 0;
    pthread_mutex_lock(&display_mu);
    int ready = display_clock.frame_ns != 0;
    if (present_slots_auto)
        ready = ready && display_refresh_observed &&
            runtime_present_auto_clock_fresh(display_clock.frame_ns, (uint64_t)display_period_ns,
                display_refresh_frame_ns, runtime_now_ns());
    if (ready) { *frame_ns = display_clock.frame_ns; *refresh_ns = (uint64_t)display_period_ns; }
    pthread_mutex_unlock(&display_mu);
    return ready;
}
int runtime_android_present_timeline_active(void) {
    return atomic_load_explicit(&present_timeline_on, memory_order_relaxed);
}
int runtime_android_present_timeline_display_id(void) {
    return runtime_android_present_timeline_active() ? 0:-1;
}
int runtime_android_present_timelines(runtime_present_timelines *out) {
    if (!out || !runtime_android_present_timeline_active()) return 0;
    pthread_mutex_lock(&display_mu);
    int ready = present_timeline_snapshot.count != 0;
    if (ready) *out = present_timeline_snapshot;
    pthread_mutex_unlock(&display_mu);
    return ready;
}
int runtime_android_adaptive_slots_requested(void) {
    return atomic_load_explicit(&adaptive_slots_requested, memory_order_relaxed);
}
int runtime_android_adaptive_slots_active(void) {
    return atomic_load_explicit(&adaptive_slots_on, memory_order_relaxed);
}
int runtime_android_adaptive_slots_display_id(void) {
    return runtime_android_adaptive_slots_active() ? 0 : -1;
}
int runtime_android_adaptive_slot_timelines(runtime_present_timelines *out) {
    if (!out || !runtime_android_adaptive_slots_active()) return 0;
    pthread_mutex_lock(&display_mu);
    int ready = display_pace_ready && present_timeline_window && !present_timeline_paused &&
        present_timeline_snapshot.generation == present_timeline_generation &&
        present_timeline_snapshot.refresh_ns == (uint64_t)display_period_ns &&
        present_timeline_snapshot.frame_ns > display_refresh_frame_ns &&
        runtime_present_adaptive_slot_timelines_fresh(&present_timeline_snapshot,
            runtime_now_ns(), adaptive_slots_epoch_ns);
    if (ready) *out = present_timeline_snapshot;
    pthread_mutex_unlock(&display_mu);
    return ready;
}
void runtime_android_present_wait(uint64_t target_ns) {
    if (!present_slots_on && !present_slots_auto) return;
    pthread_mutex_lock(&display_mu);
    /* Match the callback lattice, not nanosecond rounding of refresh-period
     * multiplication. A sub-quarter-refresh difference is the same slot. */
    while (!runtime_present_can_submit(display_clock.frame_ns, target_ns, (uint64_t)display_period_ns, runtime_now_ns()))
        pthread_cond_wait(&display_cv, &display_mu);
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_present_credit(int delta) {
    if (!present_slots_on) return;
    pthread_mutex_lock(&display_mu);
    if (!runtime_present_credit_update(&present_pending, delta))
        runtime_fail("present slot credit invariant");
    if (delta < 0) {
        pthread_cond_broadcast(&display_cv);
        if (predict_requested) pthread_cond_broadcast(&predict_cv);
    }
    pthread_mutex_unlock(&display_mu);
}
int runtime_android_acquire_slack_wait(unsigned interval, unsigned lead) {
    if (!present_slots_on) return 0;
    if (lead > 2) lead = 2;   /* Bounded even for malformed callers. */
    uint64_t arrival = runtime_now_ns();
    pthread_mutex_lock(&display_mu);
    static int logged_lead = -1;
    if (logged_lead != (int)lead) {
        fprintf(stderr, "[totk-pace] acquire slack admission: lead %u period, pending limit %u; same virtual clock, retire at slot post/presentation, GPU fences unchanged\n",
                lead, 1u + lead);
        logged_lead = (int)lead;
    }
    while (!display_clock.frame_ns || display_clock.frame_ns <= display_refresh_frame_ns)
        pthread_cond_wait(&display_cv, &display_mu);
    acquire_clock_release r = acquire_clock_reserve(&slack_budget_clock, 1,
        display_clock.frame_ns, (uint64_t)display_period_ns, interval, arrival);
    for (;;) {
        if (r.mode != ACQUIRE_CLOCK_ACTIVE) runtime_fail("acquire slack clock period");
        if (display_clock.frame_ns > display_refresh_frame_ns &&
            acquire_clock_slack_ready(&slack_budget_clock, display_clock.frame_ns, present_pending, lead)) break;
        pthread_cond_wait(&display_cv, &display_mu);
        if (display_clock.frame_ns <= display_refresh_frame_ns) continue;
        r = acquire_clock_observe(&slack_budget_clock, display_clock.frame_ns,
                                   (uint64_t)display_period_ns, arrival);
    }
    pthread_mutex_unlock(&display_mu);
    return 1;
}

int runtime_android_predict_active(void) { return atomic_load_explicit(&predict_on, memory_order_relaxed); }
static void predict_reset_locked(uint64_t refresh, unsigned interval) {
    predict_pacer_reset(&prediction.model[0]); predict_pacer_reset(&prediction.model[1]);
    prediction.issued_lead_ns[0] = prediction.issued_lead_ns[1] = 0;
    prediction.refresh = refresh; prediction.interval = interval;
    prediction.generation++; prediction.resets++;
}
static int predict_u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x > y ? 1 : x < y ? -1 : 0;
}
static int predict_i64_cmp(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x > y ? 1 : x < y ? -1 : 0;
}
typedef struct {
    uint64_t leads[300], first, last, excluded_ns;
    int64_t errors[300];
    unsigned n, valid, misses, wall_misses, cancelled, stale, mismatched, resets;
    unsigned display_samples, display_misses;
    uint64_t generation, period_ns, cost_ns, allowance_ns, margin_ns, issued_lead_ns;
    int64_t display_phase_ns;
    unsigned path, interval, miss_budget_ppm, ready_samples, ready_misses;
    unsigned phase_samples, physical_samples, physical_misses;
} predict_report;
/* Terminal frames only. Snapshot under display_mu; sort/print after unlocking. */
static void predict_record_locked(const runtime_present_prediction *p, const predict_observation *o,
                                  int cancelled, predict_report *r) {
    unsigned i = prediction.reports++;
    prediction.leads[i] = p->lead_ns;
    if (!i || p->id < prediction.first_id) prediction.first_id = p->id;
    if (!i || p->id > prediction.last_id) prediction.last_id = p->id;
    prediction.cancelled += cancelled;
    if (o && o->valid) {
        prediction.errors[prediction.valid++] = o->error_ns;
        prediction.misses += o->miss;
    }
    if (prediction.reports != 300) return;
    memcpy(r->leads, prediction.leads, sizeof r->leads);
    memcpy(r->errors, prediction.errors, prediction.valid * sizeof *r->errors);
    r->n = 300; r->valid = prediction.valid; r->misses = prediction.misses;
    r->wall_misses = prediction.wall_misses; r->excluded_ns = prediction.excluded_ns;
    r->first = prediction.first_id; r->last = prediction.last_id;
    r->cancelled = prediction.cancelled; r->stale = prediction.stale;
    r->mismatched = prediction.mismatched; r->resets = prediction.resets;
    r->display_samples = prediction.display_samples; r->display_misses = prediction.display_misses;
    const predict_pacer *model = &prediction.model[prediction.path - 1];
    r->generation = prediction.generation; r->path = prediction.path; r->interval = prediction.interval;
    r->period_ns = model->period_ns; r->cost_ns = model->cost_ns;
    r->allowance_ns = model->allowance_ns; r->margin_ns = model->margin_ns;
    r->issued_lead_ns = prediction.issued_lead_ns[prediction.path - 1];
    r->miss_budget_ppm = model->miss_budget_ppm;
    r->ready_samples = model->count; r->ready_misses = model->ready_misses;
    r->phase_samples = model->phase_count; r->display_phase_ns = model->display_phase_ns;
    r->physical_samples = model->display_count; r->physical_misses = model->display_misses;
    prediction.display_samples = prediction.display_misses = 0;
    prediction.reports = prediction.valid = prediction.misses = prediction.wall_misses = 0;
    prediction.cancelled = prediction.stale = prediction.mismatched = prediction.resets = 0;
    prediction.excluded_ns = 0;
}
static void predict_report_print(predict_report *r) {
    if (!r->n) return;
    qsort(r->leads, r->n, sizeof *r->leads, predict_u64_cmp);
    qsort(r->errors, r->valid, sizeof *r->errors, predict_i64_cmp);
    fprintf(stderr, "[totk-predict] terminal ids %llu..%llu n=%u lead p50=%.3f p95=%.3f ms work-ready prediction-error n=%u p50=%.3f p95=%.3f ms work-misses=%u wall-ready-misses=%u excluded-queue=%.3f ms cancelled=%u stale-generation=%u path-mismatch=%u resets=%u AHB-display-feedback=%u AHB-display-misses=%u AHB-display-miss-classification=phase-relative; model-path=%s model-epoch=%llu model-interval=%u model-period=%.3f ms readiness-state=%s rolling-work-ready-misses=%u/%u rolling-work-ready-miss-rate=%.4f%% miss-budget=%.4f%% miss-budget-ppm=%u rolling-phase-relative-display-misses=%u/%u rolling-phase-relative-display-miss-rate=%.4f%% physical-state=%s physical-phase-samples=%u physical-phase=%.3f ms cost-basis=wait-stripped-readiness-span-including-admission-delay current-Q-cost=%.3f ms current-Q-allowance=%.3f ms current-margin=%.3f ms last-issued-lead=%.3f ms lead-limit=%.3f ms lead-saturated=%s; input latency=[frame-tl input-dist] association=global-window-observation (not input-to-image causality)\n",
            (unsigned long long)r->first, (unsigned long long)r->last, r->n,
            r->leads[149] / 1e6, r->leads[284] / 1e6, r->valid,
            r->valid ? r->errors[(r->valid - 1) * 50 / 100] / 1e6 : NAN,
            r->valid ? r->errors[(r->valid - 1) * 95 / 100] / 1e6 : NAN,
            r->misses, r->wall_misses, r->excluded_ns / 1e6, r->cancelled,
            r->stale, r->mismatched, r->resets, r->display_samples, r->display_misses,
            r->path == PRESENT_PREDICT_AHB ? "ahb" : "copy", (unsigned long long)r->generation,
            r->interval, r->period_ns ? r->period_ns / 1e6 : NAN,
            !r->period_ns ? "unused" : r->ready_samples < PREDICT_PACE_WARMUP ? "warming" : "ready",
            r->ready_misses, r->ready_samples,
            r->ready_samples ? 100.0 * r->ready_misses / r->ready_samples : NAN,
            r->miss_budget_ppm / 10000.0, r->miss_budget_ppm,
            r->physical_misses, r->physical_samples,
            r->physical_samples ? 100.0 * r->physical_misses / r->physical_samples : NAN,
            !r->phase_samples ? "absent" : r->phase_samples < PREDICT_PACE_WARMUP ? "warming" : "calibrated",
            r->phase_samples, r->phase_samples >= PREDICT_PACE_WARMUP ? r->display_phase_ns / 1e6 : NAN,
            r->ready_samples ? r->cost_ns / 1e6 : NAN,
            r->ready_samples ? r->allowance_ns / 1e6 : NAN,
            r->ready_samples ? r->margin_ns / 1e6 : NAN,
            r->period_ns ? r->issued_lead_ns / 1e6 : NAN, r->period_ns ? r->period_ns / 1e6 : NAN,
            !r->period_ns ? "unavailable" : r->issued_lead_ns >= r->period_ns ? "yes" : "no");
}
int runtime_android_predict_acquire(uintptr_t texture, unsigned interval) {
    if (!predict_requested) return 0;
    if (!texture) runtime_fail("predict acquisition without texture identity");
    if (!interval) interval = 1;
    pthread_mutex_lock(&display_mu);
    while (!display_clock.frame_ns || display_clock.frame_ns <= display_refresh_frame_ns)
        pthread_cond_wait(&predict_cv, &display_mu);
    uint64_t refresh = (uint64_t)display_period_ns;
    if (prediction.refresh != refresh || prediction.interval != interval)
        predict_reset_locked(refresh, interval);
    vk_present_slot slot = vk_present_slots_reserve(&prediction.slots, 1, display_clock.frame_ns,
                                                   refresh, interval, runtime_now_ns());
    if (slot.mode != VK_PRESENT_SLOTS_ACTIVE) runtime_fail("predict slot period");
    if (slot.skipped) predict_reset_locked(refresh, interval);
    unsigned model_path = prediction.path;
    predict_decision d = predict_pacer_choose(&prediction.model[model_path - 1],
                                              slot.desired_ns, refresh * interval);
    if (!d.slot_ns) runtime_fail("predict decision arithmetic");
    uint64_t credit_hold_ns = 0;
    for (;;) {
        uint64_t now = runtime_now_ns();
        refresh = (uint64_t)display_period_ns;
        /* Only unpublished reservations may change after a real mode/stall.
         * Already queued plans remain immutable on their original lattice. */
        if (display_clock.frame_ns > display_refresh_frame_ns &&
            (refresh != slot.refresh_ns ||
             (now > slot.desired_ns && now - slot.desired_ns > d.period_ns))) {
            predict_reset_locked(refresh, interval);
            slot = vk_present_slots_reserve(&prediction.slots, 1, display_clock.frame_ns,
                                             refresh, interval, now);
            if (slot.mode != VK_PRESENT_SLOTS_ACTIVE) runtime_fail("predict recovery period");
            model_path = prediction.path;
            d = predict_pacer_choose(&prediction.model[model_path - 1],
                                     slot.desired_ns, refresh * interval);
            if (!d.slot_ns) runtime_fail("predict recovery decision arithmetic");
            credit_hold_ns = 0;
        }
        int fresh = display_clock.frame_ns > display_refresh_frame_ns &&
                    now >= display_clock.frame_ns && now - display_clock.frame_ns <= 2 * refresh;
        int credit_ready = runtime_present_slack_ready(present_pending);
        if (fresh && now >= d.release_ns && credit_ready) break;
        if (fresh && now < d.release_ns) {
            struct timespec at = {(time_t)(d.release_ns / 1000000000ull),
                                  (long)(d.release_ns % 1000000000ull)};
            int e = pthread_cond_timedwait(&predict_cv, &display_mu, &at);
            if (e && e != ETIMEDOUT) runtime_fail("predict monotonic wait");
        } else {
            /* Do not teach the work model the deliberate two-picture credit
             * hold. Only observed blocked residence after the target counts;
             * ordinary deadline wake jitter stays in the readiness span. */
            int credit_blocked = fresh && now >= d.release_ns && !credit_ready;
            pthread_cond_wait(&predict_cv, &display_mu);
            uint64_t end = runtime_now_ns();
            if (credit_blocked && end > now) credit_hold_ns += end - now;
        }
    }
    unsigned i;
    for (i = 0; i < 16 && prediction.pending[i].texture; i++) {}
    if (i == 16) runtime_fail("predict texture reservation capacity");
    prediction.pending[i].texture = texture;
    prediction.pending[i].plan = (runtime_present_prediction){
        .id = ++prediction.sequence, .generation = prediction.generation,
        .slot_ns = d.slot_ns, .deadline_ns = d.deadline_ns,
        .release_target_ns = d.release_ns, .release_ns = runtime_now_ns(),
        .credit_hold_ns = credit_hold_ns,
        .period_ns = d.period_ns, .refresh_ns = refresh, .predicted_cost_ns = d.cost_ns,
        .allowance_ns = d.allowance_ns, .margin_ns = d.margin_ns, .lead_ns = d.lead_ns,
        .interval = interval, .path = model_path};
    prediction.issued_lead_ns[model_path - 1] = d.lead_ns;
    atomic_store_explicit(&predict_on, 1, memory_order_relaxed);
    pthread_mutex_unlock(&display_mu);
    return 1;
}
int runtime_android_predict_take(uintptr_t texture, unsigned actual_path, runtime_present_prediction *out) {
    *out = (runtime_present_prediction){0};
    if (!runtime_android_predict_active()) return 0;
    pthread_mutex_lock(&display_mu);
    unsigned oldest = 16;
    for (unsigned i = 0; i < 16; i++) if (prediction.pending[i].texture == texture &&
        (oldest == 16 || prediction.pending[i].plan.id < prediction.pending[oldest].plan.id)) oldest = i;
    if (oldest != 16) {
        *out = prediction.pending[oldest].plan;
        prediction.pending[oldest].texture = 0;
    }
    if (actual_path == PRESENT_PREDICT_COPY || actual_path == PRESENT_PREDICT_AHB)
        prediction.path = actual_path;
    pthread_mutex_unlock(&display_mu);
    return out->id != 0;
}
void runtime_android_predict_feedback(const runtime_present_prediction *p, unsigned actual_path,
        uint64_t ready, uint64_t work, uint64_t allowance) {
    if (!runtime_android_predict_active() || !p->id) return;
    predict_report report; report.n = 0;
    predict_observation o = {0};
    uint64_t work_ready = runtime_present_prediction_ready(p, work);
    pthread_mutex_lock(&display_mu);
    if (actual_path == PRESENT_PREDICT_COPY || actual_path == PRESENT_PREDICT_AHB)
        prediction.path = actual_path;
    if (p->generation != prediction.generation) prediction.stale++;
    else if (p->path != actual_path) prediction.mismatched++;
    else if (work_ready) {
        predict_decision d = {.slot_ns = p->slot_ns, .period_ns = p->period_ns,
            .deadline_ns = p->deadline_ns, .release_ns = p->release_target_ns, .lead_ns = p->lead_ns,
            .cost_ns = p->predicted_cost_ns, .allowance_ns = p->allowance_ns, .margin_ns = p->margin_ns};
        o = predict_pacer_observe(&prediction.model[actual_path - 1], &d, work, allowance, work_ready);
        prediction.wall_misses += ready > p->deadline_ns;
        if (ready > work_ready) prediction.excluded_ns += ready - work_ready;
    }
    predict_record_locked(p, &o, 0, &report);
    pthread_mutex_unlock(&display_mu);
    predict_report_print(&report);
}
void runtime_android_predict_presented(const runtime_present_prediction *p, uint64_t display) {
    if (!runtime_android_predict_active() || !p->id || p->path != PRESENT_PREDICT_AHB) return;
    pthread_mutex_lock(&display_mu);
    if (p->generation == prediction.generation) {
        int missed = predict_pacer_presented(&prediction.model[1], p->slot_ns,
                                             p->period_ns, p->refresh_ns, display);
        if (missed >= 0) { prediction.display_samples++; prediction.display_misses += missed; }
    }
    pthread_mutex_unlock(&display_mu);
}
void runtime_android_predict_cancel(const runtime_present_prediction *p) {
    if (!runtime_android_predict_active() || !p->id) return;
    predict_report report; report.n = 0;
    pthread_mutex_lock(&display_mu);
    predict_record_locked(p, NULL, 1, &report);
    pthread_mutex_unlock(&display_mu);
    predict_report_print(&report);
}
static const char *present_timeline_load_api(void) {
    present_timeline_api.post_vsync = (void (*)(AChoreographer *, AChoreographer_vsyncCallback, void *))
        dlsym(RTLD_DEFAULT, "AChoreographer_postVsyncCallback");
    present_timeline_api.frame_time = (int64_t (*)(const AChoreographerFrameCallbackData *))
        dlsym(RTLD_DEFAULT, "AChoreographerFrameCallbackData_getFrameTimeNanos");
    present_timeline_api.length = (size_t (*)(const AChoreographerFrameCallbackData *))
        dlsym(RTLD_DEFAULT, "AChoreographerFrameCallbackData_getFrameTimelinesLength");
    present_timeline_api.preferred = (size_t (*)(const AChoreographerFrameCallbackData *))
        dlsym(RTLD_DEFAULT, "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex");
    present_timeline_api.vsync_id = (AVsyncId (*)(const AChoreographerFrameCallbackData *, size_t))
        dlsym(RTLD_DEFAULT, "AChoreographerFrameCallbackData_getFrameTimelineVsyncId");
    present_timeline_api.expected = (int64_t (*)(const AChoreographerFrameCallbackData *, size_t))
        dlsym(RTLD_DEFAULT, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
    present_timeline_api.deadline = (int64_t (*)(const AChoreographerFrameCallbackData *, size_t))
        dlsym(RTLD_DEFAULT, "AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos");
    return !present_timeline_api.post_vsync ? "AChoreographer_postVsyncCallback unavailable" :
        !present_timeline_api.frame_time ? "frame-time getter unavailable" :
        !present_timeline_api.length ? "timeline-length getter unavailable" :
        !present_timeline_api.preferred ? "preferred-timeline getter unavailable" :
        !present_timeline_api.vsync_id ? "timeline-vsync-ID getter unavailable" :
        !present_timeline_api.expected ? "expected-presentation getter unavailable" :
        !present_timeline_api.deadline ? "timeline-deadline getter unavailable" : NULL;
}
static void present_timeline_configure(void) {
    const char *request = getenv("TOTK_PRESENT_TIMELINE");
    if (!request || !strcmp(request, "0")) return;
    const char *framegen = getenv("TOTK_FRAMEGEN");
    int api = android_get_device_api_level();
    const char *reason = strcmp(request, "1") ? "expected TOTK_PRESENT_TIMELINE=0 or 1" :
        !framegen || strcmp(framegen, "locked") ? "requires TOTK_FRAMEGEN=locked" :
        !present_slots_on ? "requires TOTK_PRESENT_SLOTS=1" :
        api < 33 ? "requires Android API33" : NULL;
    if (!reason) {
        reason = present_timeline_load_api();
        /* Admission only: the presenter independently resolves and calls its
         * own setter. Neither translation unit links directly to API33. */
        void (*set_timeline)(ASurfaceTransaction *, AVsyncId) =
            (void (*)(ASurfaceTransaction *, AVsyncId))dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setFrameTimeline");
        if (!reason && !set_timeline) reason = "ASurfaceTransaction_setFrameTimeline unavailable";
    }
    if (reason) {
        fprintf(stderr, "[totk-present-timeline] ignored: %s (API %d); legacy callback and presenter clock retained\n", reason, api);
        return;
    }
    atomic_store_explicit(&present_timeline_on, 1, memory_order_relaxed);
    fprintf(stderr, "[totk-present-timeline] admitted: API %d, postVsyncCallback and all frame-data getters + transaction setter resolved; at most %u API timelines copied per callback, legacy display observation retained\n",
            api, (unsigned)RUNTIME_PRESENT_TIMELINE_CAPACITY);
}
static void adaptive_slots_configure(void) {
    const char *request = getenv("TOTK_FG_ADAPTIVE_SLOTS");
    int selected = runtime_present_adaptive_slots_request(request);
    atomic_store_explicit(&adaptive_slots_requested, selected == 1, memory_order_relaxed);
    if (!selected) return;
    const char *framegen = getenv("TOTK_FRAMEGEN"), *engine = getenv("TOTK_FRAMEGEN_ENGINE");
    int api = android_get_device_api_level();
    const char *reason = selected < 0 ? "expected TOTK_FG_ADAPTIVE_SLOTS=0 or 1 (exact spelling)" :
        !framegen || strcmp(framegen, "adaptive") ? "requires TOTK_FRAMEGEN=adaptive" :
        !engine || strcmp(engine, "qcom") ? "requires TOTK_FRAMEGEN_ENGINE=qcom" :
        !present_slots_on ? "requires TOTK_PRESENT_SLOTS=1" :
        api < 33 ? "requires Android API33" : NULL;
    if (!reason) reason = present_timeline_load_api();
    if (reason) {
        fprintf(stderr, "[totk-adaptive-slots] refused: %s (API %d); adaptive slot generation unavailable, no legacy FG fallback\n",
                reason, api);
        return;
    }
    pthread_mutex_lock(&display_mu);
    adaptive_slots_epoch_ns = runtime_now_ns();
    atomic_store_explicit(&adaptive_slots_on, 1, memory_order_relaxed);
    pthread_mutex_unlock(&display_mu);
    fprintf(stderr, "[totk-adaptive-slots] clock admitted: API %d default display 0, copied expected-present/deadline/vsync-ID timelines (capacity %u); fresh lifecycle/refresh epoch required, renderer separately qualifies QCOM/window; locked timeline/acquire policy unchanged\n",
            api, (unsigned)RUNTIME_PRESENT_TIMELINE_CAPACITY);
}
/* Called only by the admitted callback, under display_mu. The preferred index
 * is never clamped to a different API timeline if it falls outside our bound. */
static void present_timeline_publish_locked(const AChoreographerFrameCallbackData *data,
                                            int64_t frame_ns, int fresh) {
    static uint64_t callbacks;
    static unsigned published, unavailable, bounded;
    callbacks++;
    present_timeline_snapshot.count = 0;
    present_timeline_snapshot.preferred = 0;
    present_timeline_snapshot.frame_ns = frame_ns > 0 ? (uint64_t)frame_ns : 0;
    present_timeline_snapshot.sampled_ns = runtime_now_ns();
    present_timeline_snapshot.generation = present_timeline_generation;
    present_timeline_snapshot.refresh_ns = (uint64_t)display_period_ns;
    if (fresh && present_timeline_window && !present_timeline_paused) {
        size_t length = present_timeline_api.length(data);
        size_t preferred = present_timeline_api.preferred(data);
        unsigned count = length < RUNTIME_PRESENT_TIMELINE_CAPACITY ? (unsigned)length : RUNTIME_PRESENT_TIMELINE_CAPACITY;
        bounded += length > RUNTIME_PRESENT_TIMELINE_CAPACITY;
        if (preferred < count) {
            int valid = 1;
            present_timeline_snapshot.preferred = (unsigned)preferred;
            for (unsigned i = 0; i < count; i++) {
                AVsyncId id = present_timeline_api.vsync_id(data, i);
                int64_t expected = present_timeline_api.expected(data, i);
                int64_t deadline = present_timeline_api.deadline(data, i);
                /* Reject signed invalid data before converting timestamps.
                 * In particular -1 is invalid, but zero is a valid API ID. */
                if (id < 0 || expected <= 0 || deadline <= 0) { valid = 0; break; }
                present_timeline_snapshot.entries[i] = (runtime_present_timeline_entry){
                    .expected_ns = (uint64_t)expected, .deadline_ns = (uint64_t)deadline, .vsync_id = id};
            }
            if (valid) present_timeline_snapshot.count = count;
        }
    }
    published += present_timeline_snapshot.count != 0;
    unavailable += present_timeline_snapshot.count == 0;
    if (callbacks % 300 == 0) {
        fprintf(stderr, "[totk-present-timeline] callbacks=%llu last300 published=%u unavailable=%u bounded=%u; entries=%u preferred=%u generation=%llu refresh=%llu ns frame=%llu ns sample=%llu ns; immutable API values only\n",
                (unsigned long long)callbacks, published, unavailable, bounded,
                present_timeline_snapshot.count, present_timeline_snapshot.preferred,
                (unsigned long long)present_timeline_snapshot.generation,
                (unsigned long long)present_timeline_snapshot.refresh_ns,
                (unsigned long long)present_timeline_snapshot.frame_ns,
                (unsigned long long)present_timeline_snapshot.sampled_ns);
        published = unavailable = bounded = 0;
    }
}
static void display_refresh(int64_t period_ns, void *arg) {
    (void)arg;
    if (period_ns < 1000000 || period_ns > 1000000000) return;
    pthread_mutex_lock(&display_mu);
    /* Even the first declaration matching the assumed60Hz period needs a
     * subsequent callback before auto may use its phase. No supported-mode
     * list or frame-rate hint can establish this observed clock. */
    if (!display_refresh_observed && present_slots_auto)
        display_refresh_frame_ns = display_clock.frame_ns;
    display_refresh_observed = 1;
    if (display_nominal_period_ns != (uint64_t)period_ns) {
        display_nominal_period_ns = (uint64_t)period_ns;
        display_period_ns = period_ns;
        display_refresh_frame_ns = display_clock.frame_ns;
        display_callback_observation = (display_period_clock){0};
        display_fence_observation = (display_period_clock){0};
        display_callback_calibrated = display_fence_calibrated = 0;
        display_callback_measured_ns = display_fence_measured_ns = display_clock_report_ns = 0;
        present_timeline_invalidate_locked();
    }
    pthread_mutex_unlock(&display_mu);
    fprintf(stderr, "[totk-pace] display refresh declared %.3f Hz (%lld ns); timestamp calibration applies to all physical-clock modes\n", 1e9 / period_ns, (long long)period_ns);
}
static void display_frame_observe(int64_t frame_ns, const AChoreographerFrameCallbackData *timeline_data) {
    display_clock_report report = {0};
    pthread_mutex_lock(&display_mu);
    int fresh = frame_ns > 0 && display_pace_tick(&display_clock, (uint64_t)frame_ns);
    if (fresh) {
        display_clock_result r = display_clock_observe(&display_callback_observation, (uint64_t)frame_ns, display_nominal_period_ns);
        display_callback_measured_ns = r.measured_ns;
        int force = r.calibrated && !display_callback_calibrated && !display_fence_calibrated;
        display_callback_calibrated = r.calibrated;
        if (!display_fence_calibrated && r.calibrated && (uint64_t)display_period_ns != r.refresh_ns) {
            display_period_ns = (int64_t)r.refresh_ns;
            display_refresh_frame_ns = r.previous_frame_ns;
            present_timeline_invalidate_locked();
            force = 1;
        }
        report = display_clock_report_locked((uint64_t)frame_ns, force);
    }
    if (timeline_data) present_timeline_publish_locked(timeline_data, frame_ns, fresh);
    if (fresh) pthread_cond_broadcast(&display_cv);
    if (fresh && predict_requested) pthread_cond_broadcast(&predict_cv);
    pthread_mutex_unlock(&display_mu);
    display_clock_report_print(&report);
    if (fresh && display_pace_on) signal_vsync();
}
static void display_frame(int64_t frame_ns, void *arg) {
    AChoreographer *choreographer = arg;
    display_frame_observe(frame_ns, NULL);
    AChoreographer_postFrameCallback64(choreographer, display_frame, choreographer);
}
static void display_vsync(const AChoreographerFrameCallbackData *data, void *arg) {
    AChoreographer *choreographer = arg;
    totk_live_slack_tick();   /* a live TOTK_TIMER_SLACK_NS: this thread applies it itself */
    display_frame_observe(present_timeline_api.frame_time(data), data);
    present_timeline_api.post_vsync(choreographer, display_vsync, choreographer);
}
static void *display_thread(void *arg) {
    (void)arg;
    totk_thread_start("totk-vsync", NULL);
    if (!ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS)) runtime_fail("display Looper");
    AChoreographer *choreographer = AChoreographer_getInstance();
    if (!choreographer) runtime_fail("display Choreographer");
    AChoreographer_registerRefreshRateCallback(choreographer, display_refresh, NULL);
    if (runtime_android_present_timeline_active() || runtime_android_adaptive_slots_active())
        present_timeline_api.post_vsync(choreographer, display_vsync, choreographer);
    else
        AChoreographer_postFrameCallback64(choreographer, display_frame, choreographer);
    pthread_mutex_lock(&display_mu);
    display_pace_ready = 1; pthread_cond_broadcast(&display_cv);
    pthread_mutex_unlock(&display_mu);
    for (;;) if (ALooper_pollOnce(-1, NULL, NULL, NULL) == ALOOPER_POLL_ERROR) runtime_fail("display Looper poll");
    return NULL;
}
static int android_display_vsync_start(void) {
    const char *mode = getenv("TOTK_PACE"), *slots = getenv("TOTK_PRESENT_SLOTS");
    const char *acquire = getenv("TOTK_ACQUIRE_CLOCK"), *slack = getenv("TOTK_ACQUIRE_SLACK");
    display_pace_on = mode && !strcmp(mode, "display");
    int slot_mode = vk_present_slots_config(slots);
    present_slots_on = slot_mode == VK_PRESENT_SLOTS_CONFIG_FIXED;
    present_slots_auto = slot_mode == VK_PRESENT_SLOTS_CONFIG_AUTO40;
    present_timeline_configure();
    adaptive_slots_configure();
    acquire_clock_on = acquire && !strcmp(acquire, "vsync");
    predict_requested = slack && !strcmp(slack, "predict") && present_slots_on &&
                        !display_pace_on && !acquire_clock_on;
    fps_auto_configure();
    if (predict_requested) {
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) || pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) ||
            pthread_cond_init(&predict_cv, &attr)) runtime_fail("predict condition clock");
        pthread_condattr_destroy(&attr);
        const char *present = getenv("TOTK_VK_PRESENT");
        prediction.path = !present || !*present || !strcmp(present, "ahb") ? PRESENT_PREDICT_AHB : PRESENT_PREDICT_COPY;
        unsigned miss_budget_ppm = 20000;
        const char *budget = getenv("TOTK_PREDICT_MISS_PCT");
        if (budget && !predict_budget_parse_pct(budget, &miss_budget_ppm))
            fprintf(stderr, "[totk-predict] TOTK_PREDICT_MISS_PCT=%s invalid: expected decimal percentage 0..100; using 2%% (20000 ppm)\n", budget);
        predict_pacer_set_budget(&prediction.model[0], miss_budget_ppm);
        predict_pacer_set_budget(&prediction.model[1], miss_budget_ppm);
    }
    if (acquire && *acquire && strcmp(acquire, "0") && !acquire_clock_on)
        fprintf(stderr, "[totk-pace] TOTK_ACQUIRE_CLOCK=%s ignored: expected vsync or 0 (default off)\n", acquire);
    /* Observe every app's real refresh even when pacing is explicitly off:
     * this does not enable display VI, acquisition gates or presentation slots. */
    pthread_t thread;
    if (pthread_create(&thread, NULL, display_thread, NULL)) runtime_fail("display vsync thread");
    pthread_detach(thread);
    pthread_mutex_lock(&display_mu);
    /* Initialize before launching the guest: producer snapshots never wait on
     * a display callback while holding the graphics/recorder lock. */
    while (!display_pace_ready || !display_clock.frame_ns)
        pthread_cond_wait(&display_cv, &display_mu);
    pthread_mutex_unlock(&display_mu);
    if (display_pace_on)
        fprintf(stderr, "[totk-pace] display pacing enabled: Choreographer VI + %s acquire, guest interval\n",
                acquire_clock_on ? "fixed-clock requested" : "display-take (no catch-up)");
    if (present_slots_on)
        fprintf(stderr, "[totk-pace] presenter slot clock enabled: real vsync, acquisition base %s; slack %s (NVN logs effective policy); VI %s\n",
                acquire_clock_on ? "fixed-clock requested" : display_pace_on ? "display-paced" : "legacy",
                slack && !strcmp(slack, "predict") ? "predict requested" :
                slack && !strcmp(slack, "auto") ? "auto requested" :
                slack && !strcmp(slack, "2") ? "two-period requested" :
                slack && !strcmp(slack, "1") ? "requested" : "off",
                display_pace_on ? "display" : "legacy 60 Hz");
    if (present_slots_auto)
        fprintf(stderr, "[totk-pace] presenter slots auto requested: UNCAPPED=1/HZ=40 +25ms frames + observed120Hz use interval3; scripted30 floor uses interval2/4 at60/120Hz; other caps/refresh/unknown/stale clock retain producer pacing; auto never enables acquire lead or frame lag\n");
    if (acquire_clock_on)
        fprintf(stderr, "[totk-pace] acquire clock available (TOTK_ACQUIRE_CLOCK=vsync): persistent real-vsync lattice, guest interval; strict >one-period stall reanchor, bounded catch-up; VI %s\n",
                display_pace_on ? "display" : "legacy 60 Hz");
    /* Clock-only acquisition/presentation leaves the legacy VI thread running. */
    return display_pace_on;
}

/* ---------- log: stdout / stderr -> totk.log, mirrored to logcat ----------
   Straight into the file, not through a pipe and a reader thread: a failure that ends the process with _exit (or a
   signal) would lose whatever the thread had not yet copied, which is exactly the message that says why. */
static int g_log_fd = -1;
static char g_log_path[1100];
static void log_start(void) {
    snprintf(g_log_path, sizeof g_log_path, "%s/totk.log", g_data);
    g_log_fd = open(g_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (g_log_fd < 0) { __android_log_print(ANDROID_LOG_ERROR, "totk", "cannot open %s: %s", g_log_path, strerror(errno)); return; }
    dup2(g_log_fd, 1); dup2(g_log_fd, 2);
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    __android_log_print(ANDROID_LOG_INFO, "totk", "log: %s", g_log_path);
}
/* The logcat copy (log_mirror.h): a thread that sleeps until totk.log grows, then copies its new complete lines with
   tag "totk" (FATAL / CRASH / device lost as errors, red messages as warnings). It runs at nice 10, on the slower CPUs
   when the device has them, and reads the file from its start, so lines written before it started are copied too.
   TOTK_LOGCAT=1 (totk.env) opts in, off by default; started after the environment files, or at a refusal before them. */
static log_mirror_tail g_log_tail;
static void log_mirror_logcat(void *context, int level, const char *line) {
    (void)context;
    __android_log_write(level == LOG_MIRROR_ERROR ? ANDROID_LOG_ERROR : level == LOG_MIRROR_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
                        "totk", line);
}
static void *log_mirror_thread(void *arg) {
    (void)arg; totk_thread_start("totk-logcat", NULL);
    errno = 0; int nice_error = setpriority(PRIO_PROCESS, 0, 10) ? errno : 0;
    cpu_set_t allowed, slow; unsigned capacity[CPU_SETSIZE]; const char *source, *decision = "unchanged";
    if (!sched_getaffinity(0, sizeof allowed, &allowed)) {
        totk_thread_background_select("/sys/devices/system/cpu", &allowed, &slow, capacity, &source, &decision);
        if (!CPU_EQUAL(&slow, &allowed) && sched_setaffinity(0, sizeof slow, &slow)) decision = "unchanged (setaffinity failed)";
    }
    fprintf(stderr, "[totk-logcat] on (TOTK_LOGCAT=1): totk.log -> logcat tag totk, wake=%s nice=%d%s cpus=%s\n",
            g_log_tail.watch >= 0 ? "inotify" : "poll 20 ms", getpriority(PRIO_PROCESS, 0), nice_error ? " (setpriority failed)" : "", decision);
    log_mirror_run(&g_log_tail);
    return NULL;
}
static void log_mirror_start(void) {
    static int started;
    if (started || g_log_fd < 0) return;
    started = 1;
    const char *value = getenv("TOTK_LOGCAT");
    if (!log_mirror_enabled(value)) { fprintf(stderr, "[totk-logcat] off (TOTK_LOGCAT!=1): totk.log only\n"); return; }
    g_log_tail.path = g_log_path; g_log_tail.emit = log_mirror_logcat;
    int error = log_mirror_open(&g_log_tail);
    pthread_t thread;
    if (!error) error = pthread_create(&thread, NULL, log_mirror_thread, NULL);
    if (error) { fprintf(stderr, "[totk-logcat] off: %s\n", strerror(error)); return; }
    pthread_detach(thread);
}
/* Live switches (live_ctl.h): a thread that sleeps until <game>/totk.ctl grows (or is created) and stages its new
   lines; the present path applies them (runtime_live_frame). Nice 10 on the slower CPUs, like the logcat copy. Lines
   written before this start are not replayed. */
static totk_live_reader g_live_reader;
static char g_live_path[1100];
/* Nice 10 on the slower CPUs, when the device has them: the background threads below. Returns the CPU decision. */
static const char *background_place(int *nice_error) {
    errno = 0; *nice_error = setpriority(PRIO_PROCESS, 0, 10) ? errno : 0;
    cpu_set_t allowed, slow; unsigned capacity[CPU_SETSIZE]; const char *source, *decision = "unchanged";
    if (!sched_getaffinity(0, sizeof allowed, &allowed)) {
        totk_thread_background_select("/sys/devices/system/cpu", &allowed, &slow, capacity, &source, &decision);
        if (!CPU_EQUAL(&slow, &allowed) && sched_setaffinity(0, sizeof slow, &slow)) decision = "unchanged (setaffinity failed)";
    }
    return decision;
}
/* Thread sampler (thread_sampler.h): TOTK_THREAD_SAMPLER=1 (default off, live through totk.ctl) names the threads that
   ran most in each long frame and every 300 presents, in totk.log. The present path starts its thread at the first
   frame with the switch on; with it off the frame hook is one compare. */
static totk_sampler g_sampler;
static int g_sampler_launch;
static totk_live_cache g_sampler_live;
static void sampler_thread_init(void) {
    totk_thread_start("totk-tsample", NULL);
    int nice_error; const char *decision = background_place(&nice_error);
    fprintf(stderr, "[totk-threads] sampler thread nice=%d%s cpus=%s\n", getpriority(PRIO_PROCESS, 0),
            nice_error ? " (setpriority failed)" : "", decision);
}
static void thread_sampler_start(void) {   /* after totk.env: the launch value */
    const char *e = getenv("TOTK_THREAD_SAMPLER"); g_sampler_launch = e && atoi(e) == 1;
    g_sampler.thread_init = sampler_thread_init;
    fprintf(stderr, "[totk-threads] TOTK_THREAD_SAMPLER=%d: %s\n", g_sampler_launch, g_sampler_launch ?
            "long frames (over 50 ms) and every 300 presents name the 3 threads that ran most, from /proc/self/task" :
            "off (default; live through totk.ctl)");
}
void runtime_live_frame(uint64_t frame) {   /* hle_nvn.c, once per presented frame */
    totk_live_frame(frame);
    totk_sampler_frame(&g_sampler, frame, totk_live_cached(&g_sampler_live, "TOTK_THREAD_SAMPLER", g_sampler_launch));
}
static void *live_ctl_thread(void *arg) {
    (void)arg; totk_thread_start("totk-ctl", NULL);
    int nice_error; const char *decision = background_place(&nice_error);
    fprintf(stderr, "[totk-ctl] following %s (set KEY=VALUE, unset KEY, mark <text>; applied at the next presented frame), nice=%d%s cpus=%s\n",
            g_live_path, getpriority(PRIO_PROCESS, 0), nice_error ? " (setpriority failed)" : "", decision);
    live_ctl_run(&g_live_reader);
    return NULL;
}
static void live_ctl_start(void) {
    snprintf(g_live_path, sizeof g_live_path, "%s/totk.ctl", g_data);
    g_live_reader.path = g_live_path; g_live_reader.skip_existing = 1;
    pthread_t thread; int error = pthread_create(&thread, NULL, live_ctl_thread, NULL);
    if (error) { fprintf(stderr, "[totk-ctl] off: %s\n", strerror(error)); return; }
    pthread_detach(thread);
}

/* ---------- options ---------- */
#include "android_storage.h"   /* TOTK_DATA_DIR validation and the game lock */
/* The game lock (android_storage.h), taken by start_game and held until the process ends; given back when the start is
   refused, since the process outlives a finished activity and the launcher would otherwise see a game running.
   A refusal also honors an inherited explicit logcat opt-in; environment files are not read yet. */
static int g_game_lock = -1;
static int start_refused(void) { if (g_game_lock >= 0) { close(g_game_lock); g_game_lock = -1; } log_mirror_start(); return -1; }
/* What the launcher menu may write into <private>/settings.env: the option mapping and the frame-generation timing
   block of the settings contract, and the game data folder when it is not on the primary storage (TOTK_DATA_DIR, which
   start_game reads before anything else), plus TOTK_AUTOLOAD for a session that writes settings.env and starts the game
   directly (the launcher rewrites the file without it), nothing else. Any other key is a stale or foreign writer and is
   ignored; a developer's options belong in <game>/totk.env, which is read after it and wins. */
static const char *const settings_keys[] = {
    "TOTK_FPS60", "TOTK_GPU_TIME", "TOTK_FRAMEGEN", "TOTK_RENDER_RES", "TOTK_ACQUIRE_SLACK", "TOTK_POST_SCALE", "TOTK_POST_SHARPNESS",
    "TOTK_POST_SGSR_SHARPNESS", "TOTK_FXAA", "TOTK_POST_AA", "TOTK_SHADOW_RES", "TOTK_OPERATION_MODE", "TOTK_NATIVE_UI",
    "TOTK_OVERLAY", "TOTK_OVERLAY_POS", "TOTK_COMPANION", "TOTK_FACE_BUTTONS", "TOTK_AUDIO_PREFILL_MS", "TOTK_LOCALE",
    "TOTK_LANG", "TOTK_RUMBLE", "TOTK_RUMBLE_GAIN", "TOTK_GYRO", "TOTK_AUTOLOAD",
    "TOTK_VK_SPLIT_PASS_DRAWS", "TOTK_ADPF_GPU",
    "TOTK_FPS60_HYSTERESIS",
    /* frame generation timing block */
    "TOTK_REALTIME", "TOTK_FRAME_LAG", "TOTK_FPS_AUTO", "DM_PRESENT_HZ", "DM_PRESENT_INTERVAL", "TOTK_PRESENT_SLOTS",
    "TOTK_VK_PRESENT", "TOTK_ACQUIRE_CLOCK", "TOTK_PACE", "TOTK_ASYNC_REPLAY",
    /* 40fps producer clock and optional120Hz request (not observed mode). */
    "TOTK_UNCAPPED", "TOTK_UNCAPPED_HZ", "TOTK_UNCAPPED_INTERVAL", "TOTK_FRAME_RATE_HINT", "TOTK_FRAME_RATE_HINT_HZ",
    "TOTK_DATA_DIR",
};
static int settings_key(const char *name) {
    for (size_t i = 0; i < sizeof settings_keys / sizeof *settings_keys; i++) if (!strcmp(name, settings_keys[i])) return 1;
    return 0;
}
/* One line of an env file: 1 and NAME / value for an assignment ("export " and one level of quotes allowed), 0 for a blank
   or comment line, -1 (with *name at the line) for anything else. */
static int env_line(char *line, char **name, char **value) {
    char *s = line; while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
    *name = s;
    if (!*s || *s == '#') return 0;
    if (!strncmp(s, "export ", 7)) s += 7;
    char *eq = strchr(s, '='); if (!eq || eq == s) return -1;
    *eq = 0; char *v = eq + 1;
    size_t vn = strlen(v); if (vn >= 2 && (v[0] == '"' || v[0] == '\'') && v[vn - 1] == v[0]) { v[vn - 1] = 0; v++; }
    *name = s; *value = v;
    return 1;
}
/* settings.env's TOTK_DATA_DIR (the last one, as the parser would apply it), without touching the environment: 1 if set. */
static int settings_data_dir(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[2048], *name, *value; int found = 0;
    while (fgets(line, sizeof line, f)) if (env_line(line, &name, &value) == 1 && !strcmp(name, "TOTK_DATA_DIR")) { snprintf(out, cap, "%s", value); found = 1; }
    fclose(f);
    return found;
}
/* NAME=value per line; # comments; existing variables are replaced. menu: settings.env (allowlist, one summary line). */
static void env_parse(const char *path, int menu) {
    FILE *f = fopen(path, "r"); if (!f) return;
    const char *file = menu ? "settings.env" : "totk.env";
    char line[2048], applied[4096] = ""; size_t used = 0;
    while (fgets(line, sizeof line, f)) {
        char *s, *v = NULL; int kind = env_line(line, &s, &v);
        if (!kind) continue;
        if (kind < 0) { fprintf(stderr, "[totk-app] %s: ignored line \"%s\"\n", file, s); continue; }
        if (menu && !settings_key(s)) { fprintf(stderr, "[totk-app] settings.env: ignored %s (not a launcher menu option)\n", s); continue; }
        setenv(s, v, 1);
        if (!menu) { fprintf(stderr, "[totk-app] totk.env: %s=%s\n", s, v); continue; }
        int w = snprintf(applied + used, sizeof applied - used, " %s=%s", s, v);
        if (w > 0) used = used + (size_t)w < sizeof applied ? used + (size_t)w : sizeof applied - 1;
    }
    fclose(f);
    if (menu) fprintf(stderr, "[totk-app] settings.env (launcher menu; totk.env overrides):%s\n", used ? applied : " no options");
}
static void settings_file(const char *path) { env_parse(path, 1); }
static void env_file(const char *path) { env_parse(path, 0); }
static void env_default(const char *name, const char *fmt, const char *a) {
    if (getenv(name)) return;
    char v[1200]; snprintf(v, sizeof v, fmt, a); setenv(name, v, 0);
}
/* The value after "key": in install.json, past blanks; NULL when absent. The app's setup and build_android.py write the
   file (format 1, one object, keys unique), so a key lookup is enough here. */
static const char *install_value(const char *text, const char *key) {
    char quoted[64]; snprintf(quoted, sizeof quoted, "\"%s\"", key);
    const char *s = strstr(text, quoted); if (!s) return NULL;
    s += strlen(quoted); while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s++ != ':') return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}
/* <game>/install.json is written last, by atomic rename, once every setup stage the game needs is done and matched its
   pinned hashes. The Java launcher starts the game only then; this refuses a direct start (am start, an old shortcut)
   on a missing or unfinished setup instead of failing later on half-written files. */
static int install_playable(const char *dir) {
    char path[1200], text[65536]; snprintf(path, sizeof path, "%s/install.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[totk-app] 【红】%s: %s: the game is not set up; open TotK Native from the launcher to set it up\n", path, strerror(errno)); return 0; }
    size_t n = fread(text, 1, sizeof text - 1, f); int complete = feof(f) && !ferror(f); fclose(f); text[n] = 0;
    const char *format = install_value(text, "format"), *playable = install_value(text, "playable");
    if (!complete || !format || strncmp(format, "1", 1) || (format[1] >= '0' && format[1] <= '9')) { fprintf(stderr, "[totk-app] 【红】%s: unreadable or of another format; open TotK Native from the launcher to repair the setup\n", path); return 0; }
    if (!playable || strncmp(playable, "true", 4)) { fprintf(stderr, "[totk-app] 【红】%s: setup not finished (playable is not true); open TotK Native from the launcher to finish it\n", path); return 0; }
    return 1;
}
/* Optional identifier-only list shipped with this APK, not the player's translated seed/cache.
 * A missing asset never reuses a stale extracted list from another APK. */
static void pipeline_key_asset(const ANativeActivity *a) {
    const char *enabled = getenv("TOTK_PSO_PREWARM");
    if (getenv("TOTK_PSO_KEYS") || (enabled && !atoi(enabled))) return;
    AAsset *asset = AAssetManager_open(a->assetManager, "pso_keys.bin", AASSET_MODE_STREAMING);
    if (!asset) return;
    off_t size = AAsset_getLength(asset);
    char path[1200], tmp[1200];
    snprintf(path, sizeof path, "%s/state/pso_keys.bin", a->internalDataPath);
    snprintf(tmp, sizeof tmp, "%s/state/pso_keys.bin.tmp", a->internalDataPath);
    FILE *f = size >= 64 && size <= (16 << 20) ? fopen(tmp, "wb") : NULL;
    int ok = f != NULL; char bytes[65536]; off_t left = size;
    while (ok && left) {
        int n = AAsset_read(asset, bytes, left > (off_t)sizeof bytes ? sizeof bytes : (size_t)left);
        if (n <= 0 || fwrite(bytes, 1, (size_t)n, f) != (size_t)n) ok = 0; else left -= n;
    }
    AAsset_close(asset);
    if (f) { if (fflush(f) || fsync(fileno(f))) ok = 0; if (fclose(f)) ok = 0; }
    if (ok && !rename(tmp, path)) env_default("TOTK_PSO_KEYS", "%s", path);
    else { unlink(tmp); fprintf(stderr, "[pso-prewarm] cannot stage APK key list\n"); }
}
/* Profile identity comes from this APK, not user-editable launch variables.
   Missing facts stay unavailable; only collect them for an opted-in launch. */
static void gpu_profile_identity(const ANativeActivity *a) {
    const char *mode = getenv("TOTK_VK_GPU_PROFILE");
    if (!vk_gpuprof_identity || !mode || (strcmp(mode, "1") && strcmp(mode, "2") && strcmp(mode, "3"))) return;
    char build[256] = {0}, commit[80] = {0}, package[256] = {0};
    AAsset *asset = AAssetManager_open(a->assetManager, "build.txt", AASSET_MODE_BUFFER);
    if (asset) {
        off_t size = AAsset_getLength(asset);
        if (size > 0 && size < (off_t)sizeof build && AAsset_read(asset, build, (size_t)size) == size) {
            char candidate[80], extra;
            if (sscanf(build, "commit %79s %c", candidate, &extra) == 1) {
                size_t n = strlen(candidate);
                if ((n == 12 || n == 40 || n == 64) && strspn(candidate, "0123456789abcdef") == n)
                    snprintf(commit, sizeof commit, "%s", candidate);
            }
        } else build[0] = 0;
        AAsset_close(asset);
    }
    JNIEnv *env = NULL; int attached = 0; uint64_t version = 0;
    if ((*a->vm)->GetEnv(a->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) == JNI_OK) attached = 1;
        else env = NULL;
    }
    if (env && (*env)->PushLocalFrame(env, 16) == JNI_OK) {
        jclass activity = (*env)->GetObjectClass(env, a->clazz);
        jmethodID name_method = activity ? (*env)->GetMethodID(env, activity, "getPackageName", "()Ljava/lang/String;") : NULL;
        jstring name = name_method ? (*env)->CallObjectMethod(env, a->clazz, name_method) : NULL;
        if (!name || (*env)->ExceptionCheck(env)) goto identity_done;
        const char *text = (*env)->GetStringUTFChars(env, name, NULL);
        if (!text) goto identity_done;
        snprintf(package, sizeof package, "%s", text);
        (*env)->ReleaseStringUTFChars(env, name, text);
        jmethodID manager_method = (*env)->GetMethodID(env, activity, "getPackageManager", "()Landroid/content/pm/PackageManager;");
        jobject manager = manager_method ? (*env)->CallObjectMethod(env, a->clazz, manager_method) : NULL;
        if (!manager || (*env)->ExceptionCheck(env)) goto identity_done;
        jclass manager_class = (*env)->GetObjectClass(env, manager);
        jmethodID info_method = manager_class ? (*env)->GetMethodID(env, manager_class, "getPackageInfo",
            "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;") : NULL;
        jobject info = info_method ? (*env)->CallObjectMethod(env, manager, info_method, name, 0) : NULL;
        if (!info || (*env)->ExceptionCheck(env)) goto identity_done;
        jclass info_class = (*env)->GetObjectClass(env, info);
        jmethodID version_method = info_class ? (*env)->GetMethodID(env, info_class, "getLongVersionCode", "()J") : NULL;
        if (version_method && !(*env)->ExceptionCheck(env)) {
            jlong value = (*env)->CallLongMethod(env, info, version_method);
            if (!(*env)->ExceptionCheck(env) && value > 0) version = (uint64_t)value;
        }
identity_done:
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->PopLocalFrame(env, NULL);
    }
    if (env && (*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*a->vm)->DetachCurrentThread(a->vm);
    vk_gpuprof_identity(commit, package, version, build);
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
/* copy a folder recursively (dst made); returns the number of files copied, -1 on failure */
static int copy_tree(const char *src, const char *dst) {
    if (mkdir(dst, 0700) && errno != EEXIST) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s2[2048], d2[2048]; snprintf(s2, sizeof s2, "%s/%s", src, e->d_name); snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
        if (is_dir(s2)) { int k = copy_tree(s2, d2); if (k > 0) n += k; continue; }
        int in = open(s2, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(d2, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), ok = in >= 0 && out >= 0;
        char buf[1 << 16]; ssize_t r;
        while (ok && (r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)r) != r) ok = 0;
        if (in >= 0) close(in); if (out >= 0) close(out);
        if (ok) n++; else fprintf(stderr, "[totk-app] copying %s: %s\n", s2, strerror(errno));
    }
    closedir(d);
    return n;
}
/* the single folder under rewritten/ (the game version this folder was built for) */
static int game_version(char *out, size_t n) {
    char p[1100]; snprintf(p, sizeof p, "%s/rewritten", g_data);
    DIR *d = opendir(p); if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) if (e->d_name[0] != '.') { char q[1400]; snprintf(q, sizeof q, "%s/%s", p, e->d_name); if (is_dir(q)) { snprintf(out, n, "%s", e->d_name); found++; } }
    closedir(d);
    return found == 1;
}
/* Minimal arm64 KGSL ABI, kept local so this frontend does not depend on Mesa headers. */
typedef struct {
    uint32_t device_id, chip_id, mmu_enabled, _pad0;
    uint64_t gmem_gpubaseaddr;
    uint32_t gpu_id, _pad1;
    uint64_t gmem_sizebytes;
} TotkKgslDeviceInfo;
typedef struct {
    uint32_t type, _pad0;
    uint64_t value;
    uint64_t sizebytes;
} TotkKgslDeviceGetproperty;
_Static_assert(sizeof(void *) == 8, "KGSL chip probe requires the arm64 userspace ABI");

_Static_assert(sizeof(TotkKgslDeviceInfo) == 40 &&
               __builtin_offsetof(TotkKgslDeviceInfo, device_id) == 0 &&
               __builtin_offsetof(TotkKgslDeviceInfo, chip_id) == 4 &&
               __builtin_offsetof(TotkKgslDeviceInfo, mmu_enabled) == 8 &&
               __builtin_offsetof(TotkKgslDeviceInfo, gmem_gpubaseaddr) == 16 &&
               __builtin_offsetof(TotkKgslDeviceInfo, gpu_id) == 24 &&
               __builtin_offsetof(TotkKgslDeviceInfo, gmem_sizebytes) == 32,
               "arm64 KGSL device-info ABI");
_Static_assert(sizeof(TotkKgslDeviceGetproperty) == 24 &&
               __builtin_offsetof(TotkKgslDeviceGetproperty, type) == 0 &&
               __builtin_offsetof(TotkKgslDeviceGetproperty, value) == 8 &&
               __builtin_offsetof(TotkKgslDeviceGetproperty, sizebytes) == 16,
               "arm64 KGSL getproperty ABI");

#define TOTK_KGSL_PROP_DEVICE_INFO 1u
#define TOTK_KGSL_DEVICE_GETPROPERTY _IOWR(0x09, 0x2, TotkKgslDeviceGetproperty)

static bool android_kgsl_chip_probe(void *context, uint32_t *chip_id) {
    (void)context;
    int fd = open("/dev/kgsl-3d0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    TotkKgslDeviceInfo info = {0};
    TotkKgslDeviceGetproperty request = {
        .type = TOTK_KGSL_PROP_DEVICE_INFO,
        .value = (uint64_t)(uintptr_t)&info,
        .sizebytes = sizeof(info),
    };
    int result = ioctl(fd, TOTK_KGSL_DEVICE_GETPROPERTY, &request);
    int saved_errno = errno;
    close(fd);
    if (result != 0) {
        errno = saved_errno;
        return false;
    }
    *chip_id = info.chip_id;
    return true;
}
/* The folder this library was loaded from: the app's nativeLibraryDir, where Android extracts the APK's libraries
   (extractNativeLibs) and lets the app load code from. The APK bundles the pinned Mesa Turnip there as
   libtotk_turnip.so, so it is used in place, without a copy. */
static int android_native_lib_dir(char *out, size_t size) {
    Dl_info info;
    if (!dladdr((void *)android_native_lib_dir, &info) || !info.dli_fname) return 0;
    const char *slash = strrchr(info.dli_fname, '/');
    if (!slash) return 0;
    snprintf(out, size, "%.*s", (int)(slash - info.dli_fname), info.dli_fname);
    return 1;
}
/* Which Vulkan driver, for the early environment decision and the later setup alike (neither loads it):
   TOTK_VK_DRIVER=system the system's; TOTK_VK_DRIVER=<file> (absolute or in the game folder) or <game>/driver.so a
   player's own build, copied into private storage; otherwise the bundled Turnip; otherwise the system's. */
enum { DRIVER_SYSTEM, DRIVER_CUSTOM, DRIVER_BUNDLED };
static int android_driver_source(char *src, size_t size) {
    const char *want = getenv("TOTK_VK_DRIVER");
    if (want && !strcmp(want, "system")) return DRIVER_SYSTEM;
    if (want && *want) {
        if (want[0] == '/') snprintf(src, size, "%s", want); else snprintf(src, size, "%s/%s", g_data, want);
        return is_file(src) ? DRIVER_CUSTOM : DRIVER_SYSTEM;   /* a named file that is missing: the system's, as before */
    }
    snprintf(src, size, "%s/driver.so", g_data);
    if (is_file(src)) return DRIVER_CUSTOM;
    char dir[1024];
    if (android_native_lib_dir(dir, sizeof dir)) { snprintf(src, size, "%s/libtotk_turnip.so", dir); if (is_file(src)) return DRIVER_BUNDLED; }
    return DRIVER_SYSTEM;
}
static void android_driver_env_policy(void) {
    char src[1200];
    uint32_t chip_id;
    if (totk_a722_sysmem_default(android_driver_source(src, sizeof src) != DRIVER_SYSTEM, getenv("TU_DEBUG"),
            android_kgsl_chip_probe, NULL, &chip_id) && setenv("TU_DEBUG", "sysmem", 0) == 0)
        fprintf(stderr, "[totk-app] Adreno GMEM workaround: TU_DEBUG=sysmem (early startup; KGSL chip_id=0x%08x)\n", chip_id);
}

/* Files on shared storage cannot be loaded as code, so a player's driver is copied into the app's private folder; the
   bundled one already sits where code may be loaded from. */
static void driver_setup(const char *private_dir) {
    const char *want = getenv("TOTK_VK_DRIVER");
    if (want && !strcmp(want, "system")) return;
    char src[1200], libs[1200];
    int kind = android_driver_source(src, sizeof src);
    if (kind == DRIVER_SYSTEM) {
        if (want && *want) fprintf(stderr, "[totk-app] TOTK_VK_DRIVER: no file %s; the system driver is used\n", src);
        else fprintf(stderr, "[totk-app] Vulkan driver: the system's (no driver.so, no bundled Turnip)\n");
        unsetenv("TOTK_VK_DRIVER"); return;
    }
    snprintf(libs, sizeof libs, "%s/driver-libs", private_dir);   /* vk_load.c: system libraries the driver needs */
    if (kind == DRIVER_BUNDLED) {
        setenv("TOTK_VK_DRIVER", src, 1); setenv("TOTK_VK_LIBDIR", libs, 1);
        fprintf(stderr, "[totk-app] Vulkan driver: %s (bundled Turnip)\n", src);
        return;
    }
    char dst[1200]; snprintf(dst, sizeof dst, "%s/driver.so", private_dir);
    int in = open(src, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    int ok = in >= 0 && out >= 0;
    char buf[1 << 16]; ssize_t n;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) ok = 0;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    if (!ok) { fprintf(stderr, "[totk-app] copying the driver %s: %s; the system driver is used\n", src, strerror(errno)); unsetenv("TOTK_VK_DRIVER"); return; }
    setenv("TOTK_VK_DRIVER", dst, 1);
    setenv("TOTK_VK_LIBDIR", libs, 1);
    fprintf(stderr, "[totk-app] Vulkan driver: %s (copied from %s)\n", dst, src);
}

/* ---------- the game ---------- */
static char g_rewritten[1200];
/* All files access right now (start_game; defined with the activity callbacks, which own the JNI calls). */
static int android_all_files_access(const ANativeActivity *a);
static void android_close_requests_start(const char *private_files);
static void *game_thread(void *arg) {
    (void)arg;
    totk_thread_start("totk-boot", NULL);
    char *argv[] = {"totk-native", g_rewritten, "boot", NULL};
    int r = totk_host_main(3, argv);
    fprintf(stderr, "[totk-app] the game returned %d\n", r);
    _exit(r);
    return NULL;
}
/* Process-only policy, after the operation-mode default and before output/UI
 * consumers. A docked canvas uses guest extent unless explicitly overridden.
 * Native-UI output policy remains limited to a docked request for native UI. */
static void android_native_ui_mode_policy(void) {
    const char *scene=getenv("TOTK_SCENE_SCALE"), *aa=getenv("TOTK_SCENE_AA");
    if ((scene && (!strcmp(scene,"sgsr") || !strcmp(scene,"fsr"))) || (aa && !strcmp(aa,"smaa"))) {
        /* Scene filtering precedes UI, so use the host composition path even
         * when a stale NATIVE_UI=0 was left in totk.env. This is opt-in only. */
        setenv("TOTK_NATIVE_UI","1",1);
        setenv("TOTK_OUTPUT_RES","0",1);
        setenv("TOTK_APP_WIDTH","1920",1);
        fprintf(stderr,"[totk-app] scene policy: scale=%s AA=%s; native composition effective=1; shipping guest canvas, window-sized output (process only; totk.env unchanged)\n",scene?scene:"off",aa?aa:"off");
        return;
    }
    const char *ui=getenv("TOTK_NATIVE_UI");
    int handheld=runtime_handheld_mode();
    if (!handheld) {
        const char *width=getenv("TOTK_APP_WIDTH");
        if (!width || !*width) setenv("TOTK_APP_WIDTH","guest",1);
    }
    if (!ui || strcmp(ui,"1") || handheld) return;
    const char *output=getenv("TOTK_OUTPUT_RES");
    if (!output || !*output || !strcmp(output,"auto")) setenv("TOTK_OUTPUT_RES","0",1);
    setenv("TOTK_NATIVE_UI","0",1);
    fprintf(stderr,"[totk-app] native UI mode policy: docked requested=1 effective=0; shipping guest UI, OUTPUT_RES=%s APP_WIDTH=%s (process only; totk.env unchanged)\n",
        getenv("TOTK_OUTPUT_RES"),getenv("TOTK_APP_WIDTH"));
}
/* The package name from internalDataPath (/data/user/<n>/<package>/files): the app folders end in it. */
__attribute__((unused)) static void android_package_name(const char *private_files, char *out, size_t n) {
    char copy[1024]; snprintf(copy, sizeof copy, "%s", private_files ? private_files : "");
    char *end = strrchr(copy, '/'); out[0] = 0;
    if (!end) return;
    *end = 0; char *start = strrchr(copy, '/');
    snprintf(out, n, "%s", start ? start + 1 : copy);
}
static int start_game(const ANativeActivity *a) {
    /* The game data folder comes first, before the log that lives in it: the launcher's choice (settings.env
       TOTK_DATA_DIR, always written by this version's launcher), validated for its kind (android_storage.h). Without
       the key (a settings.env from a launcher before private storage): private storage when it holds an install, else
       the internal app folder those versions used. */
    char state[1100], chosen[1024], why[600], p[1200], package[256];
    snprintf(state, sizeof state, "%s/state", a->internalDataPath); mkdir(state, 0700);
    snprintf(p, sizeof p, "%s/settings.env", a->internalDataPath);
    android_package_name(a->internalDataPath, package, sizeof package);
    if (settings_data_dir(p, chosen, sizeof chosen)) {
        AndroidDataDirs dirs = { package, a->internalDataPath, "/storage", android_all_files_access(a) };
        int kind = android_data_dir_check(&dirs, chosen, NULL, why, sizeof why);
        if (kind < 0) {
            /* Never fall back to another folder (an empty one, or the copy the player moved away from). */
            snprintf(g_data, sizeof g_data, "%s", state);
            log_start();
            fprintf(stderr, "[totk-app] 【红】game folder %s %s; not starting. Insert the card, or open TotK Native and set the game up on another storage\n", chosen, why);
            return start_refused();
        }
        snprintf(g_data, sizeof g_data, "%s", chosen);
        snprintf(why, sizeof why, "%s", kind == ANDROID_DATA_PRIVATE ? "private storage" : kind == ANDROID_DATA_APP ? "app folder" : "folder with All files access");
    } else {
        char own[1200]; snprintf(own, sizeof own, "%s/game/install.json", a->internalDataPath);
        if (is_file(own) || !a->externalDataPath) snprintf(g_data, sizeof g_data, "%s/game", a->internalDataPath);
        else snprintf(g_data, sizeof g_data, "%s", a->externalDataPath);
        mkdir(g_data, 0755);
        snprintf(why, sizeof why, "no TOTK_DATA_DIR");
    }
    log_start();
    fprintf(stderr, "[totk-app] build %s %s %s; game folder %s (%s; Android SDK %d)\n", TOTK_BUILD_KIND, __DATE__, __TIME__, g_data, why, a->sdkVersion);
    snprintf(p, sizeof p, "%s/game.lock", state);
    if ((g_game_lock = android_game_lock(p, 4000, why, sizeof why)) < 0) { fprintf(stderr, "[totk-app] 【红】%s; not starting\n", why); return start_refused(); }
    int marker_rc = android_game_start_finished(a->internalDataPath);
    __android_log_print(marker_rc ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "TotkSetup",
            "game start: pid=%d lock=OFD-acquired marker=%s errno=%d", getpid(), marker_rc ? "remove-error" : "removed", marker_rc ? errno : 0);
    if (chdir(g_data)) fprintf(stderr, "[totk-app] chdir %s: %s\n", g_data, strerror(errno));
    if (!install_playable(g_data)) return start_refused();   /* the caller finishes the activity */
    /* The launcher menu's choices first, then the developer's totk.env, which overrides them (both replace). */
    snprintf(p, sizeof p, "%s/settings.env", a->internalDataPath); settings_file(p);
    snprintf(p, sizeof p, "%s/totk.env", g_data); env_file(p);
    android_driver_env_policy(); /* freeze driver options before any app worker or driver call */
    gpu_profile_identity(a);
    log_mirror_start();   /* TOTK_LOGCAT is final now */
    live_ctl_start();     /* after totk.env: its values are the launch values a live "unset" returns to */
    thread_sampler_start();
    totk_thread_game_startup();   /* snapshot the allowed CPUs before any guest narrows its affinity */
    setenv("TOTK_INPUT_FILE_NEW", "1", 0);   /* the command file outlives runs: read newly appended commands only */
    char version[256];
    if (!game_version(version, sizeof version)) {
        fprintf(stderr, "[totk-app] 【红】no game in %s although install.json says playable; open TotK Native from the launcher to repair the setup\n", g_data);
        return start_refused();
    }
    const char *native_ui=getenv("TOTK_NATIVE_UI");
    const char *scene_scale=getenv("TOTK_SCENE_SCALE");
    if (scene_scale && (!strcmp(scene_scale,"sgsr") || !strcmp(scene_scale,"fsr")) && strcmp(version,"v143")) {
        fprintf(stderr,"[totk-app] [scene-scale] refusing: requires 1.4.3 (selected %s); existing renderer retained\n",version);
        setenv("TOTK_SCENE_SCALE","off",1);
    }
    const char *scene_aa=getenv("TOTK_SCENE_AA");
    if (scene_aa && !strcmp(scene_aa,"smaa") && strcmp(version,"v143")) {
        fprintf(stderr,"[totk-app] [scene-aa] refusing: requires 1.4.3 (selected %s); existing renderer retained\n",version);
        setenv("TOTK_SCENE_AA","off",1);
    }
    if(native_ui&&*native_ui&&strcmp(native_ui,"0")&&strcmp(native_ui,"1")){
        fprintf(stderr,"[totk-app] TOTK_NATIVE_UI must be 0 or 1\n");return 0;
    }
    if(native_ui&&!strcmp(native_ui,"1")&&strcmp(version,"v143")){
        fprintf(stderr,"[totk-app] TOTK_NATIVE_UI=1 requires the 1.4.3 Vulkan canvas/transfer profile (selected %s)\n",version);return 0;
    }
    snprintf(g_rewritten, sizeof g_rewritten, "%s/rewritten/%s", g_data, version);
    /* Saves, the account and run state live in the app's private storage: files on shared storage that adb shell
       copied in belong to the shell user, and the app cannot open them for writing (the account file's lock). The
       game folder's user-data/ is imported once, when the private one does not exist yet. */
    const char *priv = a->internalDataPath;
    char pu[1200]; snprintf(pu, sizeof pu, "%s/user-data", priv);
    snprintf(p, sizeof p, "%s/user-data", g_data);
    if (!is_dir(pu) && is_dir(p)) { int n = copy_tree(p, pu); fprintf(stderr, "[totk-app] saves: imported %d files from %s into %s\n", n, p, pu); }
    env_default("TOTK_PROFILE_DIR", "%s", pu);
    env_default("TOTK_ROMFS_PROOF_DIR", "%s/state", priv);
    env_default("TOTK_SAVE_GENERATION", "%s/state/save-generation", priv);   /* fs_host.c: the launcher's save-folder mirror watches it */
    env_default("TOTK_SAVE_BUSY", "%s/state/save-busy", priv);               /* fs_host.c: present while a save is uncommitted */
    unlink(getenv("TOTK_SAVE_BUSY"));   /* a previous process ended (or was killed) mid-save: nothing of it is in progress now */
    env_default("TOTK_GAME_SLOT_DIR", "%s/state/game-slot", priv);
    env_default("TOTK_VK_PIPELINE_CACHE", "%s/state/vk-pipeline-cache.bin", priv);
    env_default("TOTK_VK_PIPELINE_CACHE_IMPORT", "%s/vk-pipeline-cache.bin", g_data);   /* install_app.sh cache-import */
    env_default("TOTK_SHARED_FONTS", "%s/extracted/firmware/shared-fonts", g_data);
    snprintf(p, sizeof p, "%s/game-data/%s", g_data, version); env_default("TOTK_GAME_DATA", "%s", p);   /* runtime/game_data.h */
    env_default("TOTK_OPERATION_MODE", "%s", "handheld");
    android_native_ui_mode_policy();
    env_default("TOTK_ROMFS_MMAP", "%s", "1");
    /* Retain the shipping minimum interval 2 unless FPS60 is requested. Pacing
     * defaults below apply to BOTH controllers; FPS60 must not inherit the Mac
     * uncapped 120-Hz policy on a fresh Android install. */
    env_default("TOTK_FPS60", "%s", "0");
    env_default("TOTK_FPS_AUTO", "%s", "0");
    env_default("TOTK_UNCAPPED", "%s", "0");
    env_default("TOTK_PACE", "%s", "0");
    env_default("TOTK_ACQUIRE_CLOCK", "%s", "0");
    env_default("TOTK_ACQUIRE_SLACK", "%s", "1");
    env_default("TOTK_PRESENT_SLOTS", "%s", "1");
    if (!strcmp(getenv("TOTK_ACQUIRE_SLACK"), "2")) {
        env_default("TOTK_PRESENT_BUFFERS", "%s", "4");
        fprintf(stderr, "[totk-pace] acquire slack 2 requested: PRESENT_BUFFERS=%s (four required; explicit lower override refuses slack 2); up to one extra game period of input latency versus slack 1; TOTK_FRAME_TIMELINE=1 reports [frame-tl reserve/input-dist]\n",
                getenv("TOTK_PRESENT_BUFFERS"));
    }
    /* Slack 2 is a player option (menu Latency "smoothest"); its admission (runtime_android_acquire_slack_wait) never
       reads the timeline, which only adds per-300 [frame-tl reserve] / [frame-tl input-dist] reports and turns on
       recorder timing and fence instrumentation. The predict experiment keeps it on by default for its reports. */
    if (!strcmp(getenv("TOTK_ACQUIRE_SLACK"), "predict"))
        env_default("TOTK_FRAME_TIMELINE", "%s", "1");
    fprintf(stderr, "[totk-pace] Android policy: FPS60=%s FPS_AUTO=%s UNCAPPED=%s PACE=%s ACQUIRE_CLOCK=%s ACQUIRE_SLACK=%s PRESENT_SLOTS=%s (explicit environment overrides defaults)\n",
            getenv("TOTK_FPS60"), getenv("TOTK_FPS_AUTO"), getenv("TOTK_UNCAPPED"), getenv("TOTK_PACE"),
            getenv("TOTK_ACQUIRE_CLOCK"), getenv("TOTK_ACQUIRE_SLACK"), getenv("TOTK_PRESENT_SLOTS"));
    snprintf(p, sizeof p, "%s/shaders/%s", g_data, version); if (is_dir(p)) env_default("TOTK_SHADER_SEED_DIR", "%s", p);
    /* The setup's on-device seed is a pack (scripts/pack_shader_seed.py format): read it through the indexed seed IO. */
    snprintf(p, sizeof p, "%s/shaders/%s/seed.vkpack", g_data, version); if (is_file(p)) env_default("TOTK_VK_SEED_IO", "%s", "1");
    mkdir(pu, 0700);
    snprintf(p, sizeof p, "%s/state", priv); mkdir(p, 0700);
    pipeline_key_asset(a);
    driver_setup(a->internalDataPath);
    runtime_input_diag_start(); /* Android constructors defer until all environment defaults are frozen */
    runtime_autoload_start();   /* TOTK_AUTOLOAD from settings.env / totk.env (no constructor in the app) */
    runtime_thread_diag_start();
    runtime_android_feedback_hardware((ANativeActivity *)a);   /* rumble / motion only where the device can drive them */
    vk_shader_seed_startup(g_data);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    if (pthread_create(&t, &at, game_thread, NULL)) { fprintf(stderr, "[totk-app] game thread: %s\n", strerror(errno)); return 0; }
    pthread_attr_destroy(&at); pthread_detach(t);
    android_close_requests_start(priv);
    return 1;
}

/* ---------- input ----------
   By name by default: Android's BUTTON_A is the Switch's A and so on. The AYN Thor's built-in controller reports its
   buttons by their Nintendo-style labels (A on the right). TOTK_FACE_BUTTONS=position maps by Xbox position instead
   (BUTTON_A at the bottom = Switch B, right = A, left = Y, top = X), for Xbox-layout gamepads. */
enum { SW_A = 0, SW_B, SW_X, SW_Y, SW_LS, SW_RS, SW_L, SW_R, SW_ZL, SW_ZR, SW_PLUS, SW_MINUS, SW_LEFT, SW_UP, SW_RIGHT, SW_DOWN };
static pthread_mutex_t g_pad_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_keys, g_hat, g_trig, g_edges;   /* buttons from key events, from the hat axes, from trigger axes; presses since the last read */
static int32_t g_axes[4];
static int g_pad_seen;
static overlay_chord g_chord;   /* Minus + R live overlay shortcut (overlay_chord.h); under g_pad_mu */
static uint64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
static int64_t game_input_after_ns; /* UI-owned: do not replay queued events or a return-to-game button held across resume */
static void pad_key_locked(int bit, int down) {
    if (down) { if (!(g_keys >> bit & 1)) g_edges |= 1ull << bit; g_keys |= 1ull << bit; }
    else g_keys &= ~(1ull << bit);
}
static void chord_minus_locked(unsigned r) {   /* Minus as overlay_chord.h replays it (an edge survives a same-read release) */
    if (r & OVERLAY_CHORD_MINUS_UP) pad_key_locked(SW_MINUS, 0);
    if (r & OVERLAY_CHORD_MINUS_DOWN) pad_key_locked(SW_MINUS, 1);
}
static int switch_bit(int keycode) {
    static int by_label = -1;
    if (by_label < 0) {
        const char *e = getenv("TOTK_FACE_BUTTONS"); by_label = !(e && !strcmp(e, "position"));
        fprintf(stderr, "[totk-app] face buttons: %s\n", by_label ? "by label (Nintendo layout; TOTK_FACE_BUTTONS=label)" :
                "by position (Xbox layout; TOTK_FACE_BUTTONS=position)");
    }
    switch (keycode) {
        case AKEYCODE_BUTTON_A: return by_label ? SW_A : SW_B;
        case AKEYCODE_BUTTON_B: return by_label ? SW_B : SW_A;
        case AKEYCODE_BUTTON_X: return by_label ? SW_X : SW_Y;
        case AKEYCODE_BUTTON_Y: return by_label ? SW_Y : SW_X;
        case AKEYCODE_BUTTON_L1: return SW_L; case AKEYCODE_BUTTON_R1: return SW_R;
        case AKEYCODE_BUTTON_L2: return SW_ZL; case AKEYCODE_BUTTON_R2: return SW_ZR;
        case AKEYCODE_BUTTON_THUMBL: return SW_LS; case AKEYCODE_BUTTON_THUMBR: return SW_RS;
        case AKEYCODE_BUTTON_START: return SW_PLUS; case AKEYCODE_BUTTON_SELECT: return SW_MINUS;
        case AKEYCODE_DPAD_LEFT: return SW_LEFT; case AKEYCODE_DPAD_UP: return SW_UP;
        case AKEYCODE_DPAD_RIGHT: return SW_RIGHT; case AKEYCODE_DPAD_DOWN: return SW_DOWN;
    }
    return -1;
}
static int32_t stick(float v) {   /* gamepad.m stick_axis: 12 % dead zone, rescaled to the Switch's range */
    const float dz = 0.12f; float m = v < 0 ? -v : v;
    if (m <= dz) return 0; if (m > 1) m = 1;
    int32_t r = (int32_t)((m - dz) / (1 - dz) * 32767.0f);
    return v < 0 ? -r : r;
}
/* TOTK_INPUT_LOG (runtime/input.c's input diagnostics) also traces each Minus/R decision of the overlay shortcut. */
static int chord_log(void) { static int on = -1; if (on < 0) on = getenv("TOTK_INPUT_LOG") != NULL; return on; }
static void chord_trace(unsigned button, unsigned action, unsigned r, unsigned state) {
    static const char *const actions[] = {"release", "press", "repeat", "cancel"};
    static const char *const states[] = {"idle", "held back", "delivered", "tap replay", "chord"};
    const char *what = r & OVERLAY_CHORD_CYCLE ? "chord: overlay cycles, both swallowed" :
        button == OVERLAY_CHORD_MINUS && action == OVERLAY_CHORD_PRESS && state == CHORD_HELD_BACK ? "held back (up to 150 ms)" :
        r & OVERLAY_CHORD_MINUS_DOWN && state == CHORD_TAP ? "deliver late: tap replayed with its length" :
        r & OVERLAY_CHORD_MINUS_DOWN ? "deliver late" : r & OVERLAY_CHORD_PASS ? "pass-through" : "swallowed";
    fprintf(stderr, "[chord] %s %s: %s (minus %s)\n", button == OVERLAY_CHORD_MINUS ? "minus" : "R", actions[action & 3], what,
            states[state < 5 ? state : 0]);
}
static int handle_input(AInputEvent *ev) {
    if (runtime_android_game_paused()) return 1;
    int type = AInputEvent_getType(ev), src = AInputEvent_getSource(ev);
    if (type == AINPUT_EVENT_TYPE_KEY &&
            (AKeyEvent_getEventTime(ev) < game_input_after_ns || AKeyEvent_getDownTime(ev) < game_input_after_ns)) return 1;
    if (type == AINPUT_EVENT_TYPE_MOTION && AMotionEvent_getEventTime(ev) < game_input_after_ns) return 1;
    int pad = (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(ev), act = AKeyEvent_getAction(ev);
        if (code == AKEYCODE_BACK) return 1;   /* not "leave the game" */
        int bit = switch_bit(code); if (bit < 0) return 0;
        if (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP) return 1;
        unsigned button = bit == SW_MINUS ? OVERLAY_CHORD_MINUS : bit == SW_R ? OVERLAY_CHORD_R : OVERLAY_CHORD_OTHER;
        unsigned action = act == AKEY_EVENT_ACTION_UP ?
                (AKeyEvent_getFlags(ev) & AKEY_EVENT_FLAG_CANCELED ? OVERLAY_CHORD_CANCEL : OVERLAY_CHORD_RELEASE) :
            AKeyEvent_getRepeatCount(ev) > 0 ? OVERLAY_CHORD_REPEAT : OVERLAY_CHORD_PRESS;
        pthread_mutex_lock(&g_pad_mu);
        /* Event time, not handling time: a queue delay must not turn a chord into two presses. */
        unsigned r = overlay_chord_key(&g_chord, button, action, (uint64_t)AKeyEvent_getEventTime(ev));
        chord_minus_locked(r);
        if (r & OVERLAY_CHORD_PASS) pad_key_locked(bit, act == AKEY_EVENT_ACTION_DOWN);
        g_pad_seen = 1;
        unsigned state = g_chord.minus;
        pthread_mutex_unlock(&g_pad_mu);
        if (button != OVERLAY_CHORD_OTHER && chord_log()) chord_trace(button, action, r, state);
        if (r & OVERLAY_CHORD_CYCLE) {
            static const char *const names[] = {"off", "fps", "full"};
            int mode = vk_overlay_cycle();
            fprintf(stderr, "[overlay] Minus+R: performance overlay %s (this session; the menu keeps the start value)\n", names[mode % 3]);
        }
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && pad) {
        float lx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0), ly = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
        float rx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0), ry = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0);
        if (!rx && !ry) { rx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RX, 0); ry = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RY, 0); }
        float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float lt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        float br = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_BRAKE, 0), gas = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_GAS, 0);
        if (br > lt) lt = br; if (gas > rt) rt = gas;
        uint64_t hat = (hx < -0.5f ? 1ull << SW_LEFT : 0) | (hx > 0.5f ? 1ull << SW_RIGHT : 0) | (hy < -0.5f ? 1ull << SW_UP : 0) | (hy > 0.5f ? 1ull << SW_DOWN : 0);
        uint64_t trig = (lt > 0.5f ? 1ull << SW_ZL : 0) | (rt > 0.5f ? 1ull << SW_ZR : 0);
        pthread_mutex_lock(&g_pad_mu);
        g_edges |= (hat & ~g_hat) | (trig & ~g_trig);
        g_hat = hat; g_trig = trig;
        g_axes[0] = stick(lx); g_axes[1] = -stick(ly); g_axes[2] = stick(rx); g_axes[3] = -stick(ry);   /* Android: down is +y; Switch: up is + */
        g_pad_seen = 1;
        pthread_mutex_unlock(&g_pad_mu);
        return 1;
    }
    return 0;
}
void runtime_gamepad_start(void) {}
void runtime_gamepad_clear(void) {
    pthread_mutex_lock(&g_pad_mu);
    g_keys = g_hat = g_trig = g_edges = 0; memset(g_axes, 0, sizeof g_axes); memset(&g_chord, 0, sizeof g_chord);
    pthread_mutex_unlock(&g_pad_mu);
}
void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&g_pad_mu);
    unsigned timed = overlay_chord_poll(&g_chord, mono_ns());   /* a lone Minus past its window, a replayed tap's end */
    chord_minus_locked(timed);
    if (timed && chord_log())   /* rare (once per lone Minus); the diagnostic accepts printing under the pad lock */
        fprintf(stderr, "[chord] pad read: %s\n", timed & OVERLAY_CHORD_MINUS_DOWN ? "deliver late: 150 ms passed without R" : "tap replay ends (minus released)");
    out->buttons = g_keys | g_hat | g_trig | g_edges; g_edges = 0;   /* a press shorter than one read still reaches the game */
    out->lx = g_axes[0]; out->ly = g_axes[1]; out->rx = g_axes[2]; out->ry = g_axes[3];
    out->connected = g_pad_seen;
    pthread_mutex_unlock(&g_pad_mu);
}
int runtime_front_request_close(void) {
    if (!runtime_fs_begin_close()) {
        __android_log_print(ANDROID_LOG_WARN, "TotkSetup", "close game: pid=%d save=busy; native exit refused", getpid());
        return 0;
    }
    __android_log_print(ANDROID_LOG_WARN, "TotkSetup", "close game: pid=%d save=idle; native pipeline-cache flush and exit", getpid());
    fprintf(stderr, "[totk] close requested: flushing pipeline cache and exiting\n"); vk_pcache_shutdown();
    if (vk_gpuprof_shutdown) vk_gpuprof_shutdown();
    _exit(0);
}
/* One stat per 500 ms on this non-render thread; no activity launch, task-affinity changes or frame-loop I/O. */
static void *android_close_requests(void *arg) {
    AndroidClosePaths paths;
    if (android_close_paths(&paths, arg)) {
        __android_log_print(ANDROID_LOG_ERROR, "TotkSetup", "close game: path error errno=%d (%s)", errno, strerror(errno));
        return NULL;
    }
    for (;;) {
        int result = android_close_poll(&paths, runtime_front_request_close);
        if (result) __android_log_print(ANDROID_LOG_WARN, "TotkSetup",
                "close game: pid=%d request=%s errno=%d (%s)", getpid(), result < 0 ? "I/O-error" : "saving-refused",
                result < 0 ? errno : 0, result < 0 ? strerror(errno) : "save in progress");
        struct timespec pause = {0, 500 * 1000000L};
        while (nanosleep(&pause, &pause) && errno == EINTR) { }
    }
}
static void android_close_requests_start(const char *private_files) {
    pthread_t thread;
    int error = pthread_create(&thread, NULL, android_close_requests, (void *)private_files);
    if (!error) pthread_detach(thread);
    else __android_log_print(ANDROID_LOG_ERROR, "TotkSetup", "close game: poller unavailable errno=%d (%s)", error, strerror(error));
}

/* ---------- text entry (nn::swkbd, runtime/swkbd_host.c) ----------
   The guest thread that asks waits here (its virtual core given up by swkbd_host.c). A byte on g_text_wake wakes the UI
   thread's looper (text_wake_cb), which hands the request to TotkActivity.showTextInput (TextEntry.java: a dialog with
   the system keyboard); the dialog answers once through nativeTextInputDone, as UTF-8 or null for a cancel. One request
   at a time; the request's strings live on the waiting thread until it is answered. */
enum { TEXT_IDLE, TEXT_ASKED, TEXT_SHOWN, TEXT_DONE };
static pthread_mutex_t g_text_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_text_cv = PTHREAD_COND_INITIALIZER;
static int g_text_state, g_text_ok, g_text_wake[2] = {-1, -1};
static const RuntimeTextRequest *g_text_req;
static char *g_text_out;
static size_t g_text_cap;
static int64_t g_text_serial;
static void text_finish_locked(int ok, const char *utf8, size_t n) {
    if (ok && g_text_out && g_text_cap) {
        size_t k = n < g_text_cap - 1 ? n : g_text_cap - 1;
        while (k && k < n && ((unsigned char)utf8[k] & 0xc0) == 0x80) k--;   /* never half a character */
        memcpy(g_text_out, utf8, k); g_text_out[k] = 0;
    }
    g_text_ok = ok; g_text_state = TEXT_DONE;
    pthread_cond_broadcast(&g_text_cv);
}
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    if (out_size) out[0] = 0;
    pthread_mutex_lock(&g_text_mu);
    while (g_text_state != TEXT_IDLE) pthread_cond_wait(&g_text_cv, &g_text_mu);
    char wake = 1;
    g_text_req = req; g_text_out = out; g_text_cap = out_size; g_text_ok = 0; g_text_serial++; g_text_state = TEXT_ASKED;
    if (g_text_wake[1] < 0 || write(g_text_wake[1], &wake, 1) != 1) {
        fprintf(stderr, "[totk-app] keyboard: the text dialog cannot be shown (%s); cancelled\n", g_text_wake[1] < 0 ? "no activity" : strerror(errno));
        text_finish_locked(0, NULL, 0);
    }
    while (g_text_state != TEXT_DONE) pthread_cond_wait(&g_text_cv, &g_text_mu);
    int ok = g_text_ok;
    g_text_req = NULL; g_text_out = NULL; g_text_cap = 0; g_text_state = TEXT_IDLE;
    pthread_cond_broadcast(&g_text_cv);
    pthread_mutex_unlock(&g_text_mu);
    return ok;
}
/* UTF-8 -> java.lang.String through UTF-16 (NewStringUTF takes modified UTF-8 only); bad bytes become U+FFFD. */
static jstring text_java_string(JNIEnv *env, const char *s) {
    size_t n = strlen(s ? s : ""), k = 0; jchar *u = malloc((n + 1) * sizeof *u);
    if (!u) return NULL;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;) {
        uint32_t c = *p++; int more = c >= 0xf8 ? 0 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3f >> more; else if (c >= 0x80) c = 0xfffd;
        for (; more; more--, p++) { if ((*p & 0xc0) != 0x80) { c = 0xfffd; break; } c = c << 6 | (*p & 0x3f); }
        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        if (c >= 0x10000) { u[k++] = (jchar)(0xd800 + ((c - 0x10000) >> 10)); u[k++] = (jchar)(0xdc00 + ((c - 0x10000) & 0x3ff)); }
        else u[k++] = (jchar)c;
    }
    jstring r = (*env)->NewString(env, u, (jsize)k);
    free(u);
    return r;
}
/* runtime/game_error.h: any thread, just before a fatal abort. TotkActivity.showGameError opens the launcher's menu
   (its own process, which outlives this one) with the reason; the short wait lets the activity manager take the start
   before the abort ends this process. Once: the first fatal error is the one that is shown. */
void runtime_android_game_error(const char *reason) {
    static atomic_int once;
    ANativeActivity *a = g_activity;
    if (atomic_exchange(&once, 1) || !a || !a->vm) return;
    JNIEnv *env = NULL; int attached = 0;
    if ((*a->vm)->GetEnv(a->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    int shown = 0;
    jstring text = text_java_string(env, reason);
    jclass cls = (*env)->GetObjectClass(env, a->clazz);
    jmethodID show = cls ? (*env)->GetMethodID(env, cls, "showGameError", "(Ljava/lang/String;)V") : NULL;
    if (show && text && !(*env)->ExceptionCheck(env)) {
        (*env)->CallVoidMethod(env, a->clazz, show, text);
        shown = !(*env)->ExceptionCheck(env);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (text) (*env)->DeleteLocalRef(env, text);
    if (attached) (*a->vm)->DetachCurrentThread(a->vm);
    fprintf(stderr, "[totk-app] fatal error %s the menu: %s\n", shown ? "handed to" : "could not be shown in", reason);
    if (shown) usleep(500 * 1000);
}
static int text_wake_cb(int fd, int events, void *data) {
    (void)events; (void)data; char drain[16];
    while (read(fd, drain, sizeof drain) > 0) {}
    ANativeActivity *a = g_activity; JNIEnv *env = a ? a->env : NULL;
    pthread_mutex_lock(&g_text_mu);
    if (g_text_state != TEXT_ASKED) { pthread_mutex_unlock(&g_text_mu); return 1; }
    const RuntimeTextRequest *r = g_text_req; int64_t serial = g_text_serial;
    jstring prompt = env ? text_java_string(env, r->prompt) : NULL, initial = env ? text_java_string(env, r->initial) : NULL;
    int max = r->max_units, min = r->min_units, secure = r->secure, digits = r->digits_only; uint32_t disabled = r->disabled;
    g_text_state = TEXT_SHOWN;
    pthread_mutex_unlock(&g_text_mu);
    int shown = 0;
    if (env && prompt && initial) {
        jclass cls = (*env)->GetObjectClass(env, a->clazz);
        jmethodID show = (*env)->GetMethodID(env, cls, "showTextInput", "(JLjava/lang/String;Ljava/lang/String;IIZZI)V");
        if (show && !(*env)->ExceptionCheck(env)) {
            (*env)->CallVoidMethod(env, a->clazz, show, (jlong)serial, prompt, initial, (jint)max, (jint)min,
                                   (jboolean)(secure != 0), (jboolean)(digits != 0), (jint)disabled);
            shown = !(*env)->ExceptionCheck(env);
        }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
        (*env)->DeleteLocalRef(env, cls);
    }
    if (env && prompt) (*env)->DeleteLocalRef(env, prompt);
    if (env && initial) (*env)->DeleteLocalRef(env, initial);
    fprintf(stderr, "[totk-app] keyboard: %s (up to %d characters)\n", shown ? "text dialog shown" : "the text dialog failed; cancelled", max);
    if (!shown) {
        pthread_mutex_lock(&g_text_mu);
        if (g_text_state == TEXT_SHOWN && g_text_serial == serial) text_finish_locked(0, NULL, 0);
        pthread_mutex_unlock(&g_text_mu);
    }
    return 1;
}
/* TotkActivity.nativeTextInputDone (registered in runtime/companion_jni.c): the dialog's one answer. */
void runtime_android_text_done(JNIEnv *env, jclass cls, jlong serial, jbyteArray utf8) {
    (void)cls;
    jsize n = utf8 ? (*env)->GetArrayLength(env, utf8) : 0;
    jbyte *bytes = utf8 ? (*env)->GetByteArrayElements(env, utf8, NULL) : NULL;
    pthread_mutex_lock(&g_text_mu);
    if (g_text_state == TEXT_SHOWN && g_text_serial == serial) text_finish_locked(utf8 && bytes, (const char *)bytes, (size_t)n);
    pthread_mutex_unlock(&g_text_mu);
    if (bytes) (*env)->ReleaseByteArrayElements(env, utf8, bytes, JNI_ABORT);
}
static void text_wake_start(void) {   /* UI thread, once: the looper callback that shows the dialog */
    if (g_text_wake[0] >= 0) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK)) { fprintf(stderr, "[totk-app] keyboard pipe: %s\n", strerror(errno)); return; }
    if (ALooper_addFd(ALooper_forThread(), fds[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, text_wake_cb, NULL) != 1) {
        fprintf(stderr, "[totk-app] keyboard: no UI looper\n"); close(fds[0]); close(fds[1]); return;
    }
    pthread_mutex_lock(&g_text_mu); g_text_wake[0] = fds[0]; g_text_wake[1] = fds[1]; pthread_mutex_unlock(&g_text_mu);
}

static AInputQueue *g_queue;
static int queue_cb(int fd, int events, void *data) {
    (void)fd; (void)events; AInputQueue *q = data; AInputEvent *ev;
    while (AInputQueue_getEvent(q, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(q, ev)) continue;
        AInputQueue_finishEvent(q, ev, handle_input(ev));
    }
    return 1;
}

/* ---------- the activity (callbacks on the UI thread) ---------- */
/* All files access (Environment.isExternalStorageManager, API 30+) right now; 0 when it cannot be asked. Only a game
   folder the player picked on shared storage needs it (android_storage.h, start_game). */
static int android_all_files_access(const ANativeActivity *a) {
    JNIEnv *env = a->env;
    jclass cls = (*env)->FindClass(env, "android/os/Environment");
    jmethodID m = cls && !(*env)->ExceptionCheck(env) ? (*env)->GetStaticMethodID(env, cls, "isExternalStorageManager", "()Z") : NULL;
    jboolean granted = m && !(*env)->ExceptionCheck(env) ? (*env)->CallStaticBooleanMethod(env, cls, m) : JNI_FALSE;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); granted = JNI_FALSE; }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    return granted == JNI_TRUE;
}
static void immersive(ANativeActivity *a) {   /* hide the status and navigation bars (View.setSystemUiVisibility) */
    JNIEnv *env = a->env; jobject act = a->clazz;
    jclass ac = (*env)->GetObjectClass(env, act);
    jmethodID gw = (*env)->GetMethodID(env, ac, "getWindow", "()Landroid/view/Window;");
    jobject w = gw ? (*env)->CallObjectMethod(env, act, gw) : NULL;
    if (w) {
        jclass wc = (*env)->GetObjectClass(env, w);
        jmethodID gd = (*env)->GetMethodID(env, wc, "getDecorView", "()Landroid/view/View;");
        jobject v = gd ? (*env)->CallObjectMethod(env, w, gd) : NULL;
        if (v) {
            jclass vc = (*env)->GetObjectClass(env, v);
            jmethodID su = (*env)->GetMethodID(env, vc, "setSystemUiVisibility", "(I)V");
            if (su) (*env)->CallVoidMethod(env, v, su, 0x1706);   /* HIDE_NAVIGATION | FULLSCREEN | LAYOUT_* | IMMERSIVE_STICKY */
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
/* NativeActivity's API33 buffer producer may have no SurfaceControl parent.
 * Keep an explicitly attached public-Java SurfaceControl/Surface pair for the
 * opt-in AHB backend; copy mode never allocates this bridge. All state here is
 * UI-thread-owned. vk_window_set acquires its own references to both windows. */
static ANativeWindow *g_window, *g_surface_parent_window;
static jobject g_surface_parent, g_surface_parent_surface;
static int g_window_ahb, g_window_display_id = -1;
static uint64_t g_window_epoch;

static void surface_parent_error(JNIEnv *env, const char *stage) {
    fprintf(stderr, "[totk-ahb-ui] failed at %s (JNI exception %s)\n",
            stage, (*env)->ExceptionCheck(env) ? "yes" : "no");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}
static jmethodID surface_parent_method(JNIEnv *env, jobject obj, const char *name, const char *signature) {
    jclass cls = (*env)->GetObjectClass(env, obj);
    if (!cls) return NULL;
    jmethodID method = (*env)->GetMethodID(env, cls, name, signature);
    (*env)->DeleteLocalRef(env, cls);
    return method;
}
/* Validate before publishing a new window, and again on existing window/display lifecycle notifications.
 * A previous window's accepted override must never authorize a rate on a different display. */
static void frame_rate_hint_update(ANativeActivity *a) {
    unsigned requested = totk_perf_rate_request();
    if (!requested) return;   /* Off leaves the existing path unchanged, including no Java query. */
    JNIEnv *env = a->env;
    jmethodID method = surface_parent_method(env, a->clazz, "supportsFrameRateHint", "(I)Z");
    jboolean supported = method && !(*env)->ExceptionCheck(env) ?
        (*env)->CallBooleanMethod(env, a->clazz, method, (jint)requested) : JNI_FALSE;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        supported = JNI_FALSE;
        fprintf(stderr, "[totk-perf] TOTK_FRAME_RATE_HINT_HZ=%u refused: display query JNI error; retaining existing policy\n", requested);
    } else if (!method || !supported) {
        fprintf(stderr, "[totk-perf] TOTK_FRAME_RATE_HINT_HZ=%u refused: unavailable or not advertised by current window display; retaining existing policy\n", requested);
    }
    totk_perf_rate_override(supported ? requested : 0);
    if (supported) fprintf(stderr, "[totk-perf] TOTK_FRAME_RATE_HINT_HZ=%u validated for current window display; FIXED_SOURCE request, not observed panel cadence\n", requested);
}
/* Optional Java32 root hint listener, never a guessed panel orientation or a
 * default-display query. Keep JNI and AttachedSurfaceControl on this UI thread.
 * The successful explicit parent attachment gates the initial hint query.
 * No display_mu here: Java can synchronously call vk_window_transform_hint
 * (g_win_mu), while the presenter copies clocks in g_win_mu -> display_mu order. */
static void present_transform_update(ANativeActivity *a, uint64_t epoch) {
    const char *e = getenv("TOTK_PRESENT_PREROTATE"), *u = getenv("TOTK_PRESENT_UBWC");
    if ((!e || strcmp(e,"1")) && (!u || strcmp(u,"1"))) return;
    if (epoch && (!g_window_ahb || !g_surface_parent_window)) {
        static int warned;
        if (!warned) {
            warned=1;
            fprintf(stderr,"[present-prerotate] no attached SurfaceControl root; identity retained until attachment succeeds\n");
        }
        return;
    }
    JNIEnv *env=a->env;
    jmethodID method=surface_parent_method(env,a->clazz,"updatePresentPrerotate","(J)V");
    if (method && !(*env)->ExceptionCheck(env)) (*env)->CallVoidMethod(env,a->clazz,method,(jlong)epoch);
    if (!method || (*env)->ExceptionCheck(env)) surface_parent_error(env,"pre-rotation hint listener");
}
void runtime_android_present_transform_hint(uint64_t epoch, int hint) {
    /* The listener's attachment epoch rejects late notifications after detach. */
    if (g_window) vk_window_transform_hint(g_window,epoch,hint);
}
static int surface_parent_void(JNIEnv *env, jobject obj, const char *name, const char *stage) {
    jmethodID method = surface_parent_method(env, obj, name, "()V");
    if (!method || (*env)->ExceptionCheck(env)) { surface_parent_error(env, stage); return 0; }
    (*env)->CallVoidMethod(env, obj, method);
    if ((*env)->ExceptionCheck(env)) { surface_parent_error(env, stage); return 0; }
    return 1;
}
static void surface_parent_dispose(JNIEnv *env, jobject parent, jobject surface) {
    /* Release alone does not remove an attached layer from the compositor. */
    if (parent) {
        const char *stage = "cleanup Transaction class";
        jclass cls = (*env)->FindClass(env, "android/view/SurfaceControl$Transaction");
        jobject transaction = NULL, result = NULL;
        if (!cls || (*env)->ExceptionCheck(env)) goto detach_failed;
        stage = "cleanup Transaction constructor";
        jmethodID method = (*env)->GetMethodID(env, cls, "<init>", "()V");
        if (!method || (*env)->ExceptionCheck(env)) goto detach_failed;
        transaction = (*env)->NewObject(env, cls, method);
        if (!transaction || (*env)->ExceptionCheck(env)) goto detach_failed;
        stage = "cleanup Transaction.reparent(parent, null)";
        method = (*env)->GetMethodID(env, cls, "reparent",
            "(Landroid/view/SurfaceControl;Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
        if (!method || (*env)->ExceptionCheck(env)) goto detach_failed;
        result = (*env)->CallObjectMethod(env, transaction, method, parent, NULL);
        if (!result || (*env)->ExceptionCheck(env)) goto detach_failed;
        surface_parent_void(env, transaction, "apply", "cleanup Transaction.apply");
        goto detach_done;
detach_failed:
        surface_parent_error(env, stage);
detach_done:
        if (transaction) surface_parent_void(env, transaction, "close", "cleanup Transaction.close");
        if (result) (*env)->DeleteLocalRef(env, result);
        if (transaction) (*env)->DeleteLocalRef(env, transaction);
        if (cls) (*env)->DeleteLocalRef(env, cls);
    }
    if (surface) surface_parent_void(env, surface, "release", "cleanup Surface.release");
    if (parent) surface_parent_void(env, parent, "release", "cleanup SurfaceControl.release");
}
static int surface_parent_create(ANativeActivity *a, const char *event) {
    JNIEnv *env = a->env;
    int api = android_get_device_api_level();
    int width = ANativeWindow_getWidth(g_window), height = ANativeWindow_getHeight(g_window);
    int attached = -1, display_id = -1, ready = 0;
    const char *stage = "PushLocalFrame";
    jobject parent = NULL, surface = NULL, transaction = NULL;
    jobject parent_global = NULL, surface_global = NULL;
    ANativeWindow *parent_window = NULL;
    fprintf(stderr, "[totk-ahb-ui] %s: window %p %dx%d API %d\n", event, (void *)g_window, width, height, api);
    if ((*env)->PushLocalFrame(env, 32) < 0) { surface_parent_error(env, stage); return 0; }
    stage = "Activity.getWindow";
    jmethodID method = surface_parent_method(env, a->clazz, "getWindow", "()Landroid/view/Window;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    jobject window = (*env)->CallObjectMethod(env, a->clazz, method);
    if (!window || (*env)->ExceptionCheck(env)) goto failed;
    stage = "Window.getDecorView";
    method = surface_parent_method(env, window, "getDecorView", "()Landroid/view/View;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    jobject decor = (*env)->CallObjectMethod(env, window, method);
    if (!decor || (*env)->ExceptionCheck(env)) goto failed;
    stage = "View.isAttachedToWindow";
    method = surface_parent_method(env, decor, "isAttachedToWindow", "()Z");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    attached = (*env)->CallBooleanMethod(env, decor, method);
    if ((*env)->ExceptionCheck(env)) goto failed;
    stage = "View.getDisplay";
    method = surface_parent_method(env, decor, "getDisplay", "()Landroid/view/Display;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    jobject display = (*env)->CallObjectMethod(env, decor, method);
    if ((*env)->ExceptionCheck(env)) goto failed;
    if (display) {
        stage = "Display.getDisplayId";
        method = surface_parent_method(env, display, "getDisplayId", "()I");
        if (!method || (*env)->ExceptionCheck(env)) goto failed;
        display_id = (*env)->CallIntMethod(env, display, method);
        if ((*env)->ExceptionCheck(env)) goto failed;
    }
    g_window_display_id = display_id;   /* The decor's actual display, not the default display. */
    fprintf(stderr, "[totk-ahb-ui] %s: decor attached %d display %d\n", event, attached, display_id);
    stage = "View.isAttachedToWindow";
    if (!attached) goto deferred;
    stage = "View.getDisplay";
    if (!display) goto deferred;
    stage = "native window dimensions";
    if (width <= 0 || height <= 0) goto deferred;
    stage = "View.getRootSurfaceControl (requires API31)";
    if (api < 31) goto failed;
    method = surface_parent_method(env, decor, "getRootSurfaceControl", "()Landroid/view/AttachedSurfaceControl;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    jobject root = (*env)->CallObjectMethod(env, decor, method);
    if ((*env)->ExceptionCheck(env)) goto failed;
    if (!root) goto deferred;
    stage = "SurfaceControl.Builder constructor";
    jclass builder_class = (*env)->FindClass(env, "android/view/SurfaceControl$Builder");
    if (!builder_class || (*env)->ExceptionCheck(env)) goto failed;
    method = (*env)->GetMethodID(env, builder_class, "<init>", "()V");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    jobject builder = (*env)->NewObject(env, builder_class, method);
    if (!builder || (*env)->ExceptionCheck(env)) goto failed;
    stage = "SurfaceControl.Builder.setName";
    jstring name = (*env)->NewStringUTF(env, "TOTK AHB parent");
    if (!name || (*env)->ExceptionCheck(env)) goto failed;
    method = (*env)->GetMethodID(env, builder_class, "setName", "(Ljava/lang/String;)Landroid/view/SurfaceControl$Builder;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    jobject result = (*env)->CallObjectMethod(env, builder, method, name);
    if (!result || (*env)->ExceptionCheck(env)) goto failed;
    stage = "SurfaceControl.Builder.setBufferSize";
    method = (*env)->GetMethodID(env, builder_class, "setBufferSize", "(II)Landroid/view/SurfaceControl$Builder;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    result = (*env)->CallObjectMethod(env, builder, method, width, height);
    if (!result || (*env)->ExceptionCheck(env)) goto failed;
    stage = "SurfaceControl.Builder.build";
    method = (*env)->GetMethodID(env, builder_class, "build", "()Landroid/view/SurfaceControl;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    parent = (*env)->CallObjectMethod(env, builder, method);
    if (!parent || (*env)->ExceptionCheck(env)) goto failed;
    stage = "AttachedSurfaceControl.buildReparentTransaction";
    method = surface_parent_method(env, root, "buildReparentTransaction",
        "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    transaction = (*env)->CallObjectMethod(env, root, method, parent);
    if ((*env)->ExceptionCheck(env)) goto failed;
    if (!transaction) goto deferred;
    stage = "Transaction.show";
    method = surface_parent_method(env, transaction, "show", "(Landroid/view/SurfaceControl;)Landroid/view/SurfaceControl$Transaction;");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    result = (*env)->CallObjectMethod(env, transaction, method, parent);
    if (!result || (*env)->ExceptionCheck(env)) goto failed;
    if (!surface_parent_void(env, transaction, "apply", "Transaction.apply")) goto done;
    int closed = surface_parent_void(env, transaction, "close", "Transaction.close");
    transaction = NULL;
    if (!closed) goto done;
    stage = "Surface(SurfaceControl)";
    jclass surface_class = (*env)->FindClass(env, "android/view/Surface");
    if (!surface_class || (*env)->ExceptionCheck(env)) goto failed;
    method = (*env)->GetMethodID(env, surface_class, "<init>", "(Landroid/view/SurfaceControl;)V");
    if (!method || (*env)->ExceptionCheck(env)) goto failed;
    surface = (*env)->NewObject(env, surface_class, method, parent);
    if (!surface || (*env)->ExceptionCheck(env)) goto failed;
    stage = "ANativeWindow_fromSurface";
    parent_window = ANativeWindow_fromSurface(env, surface);
    if (!parent_window || (*env)->ExceptionCheck(env)) goto failed;
    stage = "NewGlobalRef(SurfaceControl)";
    parent_global = (*env)->NewGlobalRef(env, parent);
    if (!parent_global || (*env)->ExceptionCheck(env)) goto failed;
    stage = "NewGlobalRef(Surface)";
    surface_global = (*env)->NewGlobalRef(env, surface);
    if (!surface_global || (*env)->ExceptionCheck(env)) goto failed;
    g_surface_parent = parent_global;
    g_surface_parent_surface = surface_global;
    g_surface_parent_window = parent_window;
    ready = 1;
    fprintf(stderr, "[totk-ahb-ui] %s: attached parent window %p %dx%d display %d API %d\n",
            event, (void *)parent_window, ANativeWindow_getWidth(parent_window), ANativeWindow_getHeight(parent_window), display_id, api);
    goto done;
deferred:
    fprintf(stderr, "[totk-ahb-ui] %s: deferred at %s (attached %d display %d); retry on window readiness callbacks\n",
            event, stage, attached, display_id);
    goto done;
failed:
    fprintf(stderr, "[totk-ahb-ui] %s: window %p attached %d display %d API %d\n",
            event, (void *)g_window, attached, display_id, api);
    surface_parent_error(env, stage);
done:
    if (transaction) surface_parent_void(env, transaction, "close", "cleanup creation Transaction.close");
    if (!ready) {
        surface_parent_dispose(env, parent, surface);
        if (surface_global) (*env)->DeleteGlobalRef(env, surface_global);
        if (parent_global) (*env)->DeleteGlobalRef(env, parent_global);
        if (parent_window) ANativeWindow_release(parent_window);
    }
    (*env)->PopLocalFrame(env, NULL);
    return ready;
}
static void window_clear(ANativeActivity *a) {
    /* Unpublish before removing the Java layer or dropping our window refs. */
    present_transform_update(a,0);
    present_timeline_lifecycle(0, -1);
    (void)vk_window_set(NULL, NULL, -1);
    g_window_epoch = 0;
    surface_parent_dispose(a->env, g_surface_parent, g_surface_parent_surface);
    if (g_surface_parent_surface) (*a->env)->DeleteGlobalRef(a->env, g_surface_parent_surface);
    if (g_surface_parent) (*a->env)->DeleteGlobalRef(a->env, g_surface_parent);
    if (g_surface_parent_window) ANativeWindow_release(g_surface_parent_window);
    if (g_window) ANativeWindow_release(g_window);
    g_surface_parent_surface = g_surface_parent = NULL;
    g_surface_parent_window = g_window = NULL;
    g_window_ahb = 0;
    g_window_display_id = -1;
}
static void surface_parent_retry(ANativeActivity *a, ANativeWindow *w, const char *event) {
    if (w && w == g_window) frame_rate_hint_update(a);
    if (w && w == g_window && g_window_ahb && !g_surface_parent_window && surface_parent_create(a, event))
        g_window_epoch = vk_window_set(g_window, g_surface_parent_window, g_window_display_id);
    if (w && w == g_window) present_transform_update(a,g_window_epoch);
}
static void on_window_created(ANativeActivity *a, ANativeWindow *w) {
    if (g_window) window_clear(a);
    ANativeWindow_acquire(w);
    g_window = w;
    const char *present = getenv("TOTK_VK_PRESENT");
    g_window_ahb = !present || !*present || !strcmp(present, "ahb");
    frame_rate_hint_update(a);
    if (g_window_ahb) surface_parent_create(a, "created");
    g_window_epoch = vk_window_set(w, g_surface_parent_window, g_window_display_id);
    present_transform_update(a,g_window_epoch);
    present_timeline_lifecycle(1, -1);
}
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)w; window_clear(a); }
static void on_window_resized(ANativeActivity *a, ANativeWindow *w) {
    surface_parent_retry(a, w, "resized");
    vk_window_resized(w, g_window_epoch);
}
static void on_window_redraw(ANativeActivity *a, ANativeWindow *w) {
    surface_parent_retry(a, w, "redraw-needed");
    vk_window_resized(w, g_window_epoch);
}
static void on_content_rect_changed(ANativeActivity *a, const ARect *rect) {
    (void)rect;
    surface_parent_retry(a, g_window, "content-rect");
    vk_window_resized(g_window, g_window_epoch);
}
static void on_configuration_changed(ANativeActivity *a) {
    present_timeline_lifecycle(-1, -1);
    surface_parent_retry(a, g_window, "configuration-changed");
    vk_window_resized(g_window, g_window_epoch);
}
static void on_queue_created(ANativeActivity *a, AInputQueue *q) {
    (void)a; g_queue = q;
    AInputQueue_attachLooper(q, ALooper_forThread(), 1, queue_cb, q);
}
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); if (g_queue == q) g_queue = NULL; }
static void on_focus(ANativeActivity *a, int focused) {
    if (focused) immersive(a);
    if (focused) {
        surface_parent_retry(a, g_window, "focus");
        vk_window_resized(g_window, g_window_epoch);
    } else { runtime_gamepad_clear(); runtime_feedback_quiet(); }   /* releases may not arrive while another window has focus; no rumble meanwhile */
}
/* UI-thread facets: focus cannot undo an actual lifecycle pause. A single
 * display's live recents tile may stay RESUMED; dual-screen focus stays free. */
static int game_activity_paused, game_focus_paused;
static void android_game_pause_update(void) {
    int paused = game_activity_paused || game_focus_paused;
    if (paused == runtime_android_game_paused()) return;
    game_input_after_ns = (int64_t)mono_ns();
    android_game_pause_set(&game_pause, paused);
    runtime_audio_output_pause(paused);
    runtime_input_clear();
    present_timeline_lifecycle(-1, paused);
    vk_pcache_background(paused);
    fprintf(stderr, "[totk-app] game and audio %s: lifecycle=%d single-display-focus=%d\n",
            paused ? "paused (pending saves may finish)" : "resumed", game_activity_paused, game_focus_paused);
}
void runtime_android_game_focus(JNIEnv *env, jclass cls, jboolean focused, jboolean single_display) {
    (void)env; (void)cls;
    game_focus_paused = !focused && single_display;
    android_game_pause_update();
}
/* Top-resumed is global; window focus is per-display. Clear held/queued input
 * when controller routing changes, without altering either background-pause facet. */
void runtime_android_game_input_focus(JNIEnv *env, jclass cls, jboolean top) {
    (void)env; (void)cls;
    game_input_after_ns = (int64_t)mono_ns();
    /* Losing routing clears everything held; regaining it clears only the controller state, so a companion shortcut
     * issued in between (a synthetic key with its pending edge) still reaches the game. */
    if (top) runtime_gamepad_clear();
    else { runtime_input_clear(); runtime_feedback_quiet(); }
}
static void android_game_lifecycle_pause(int paused) {
    game_activity_paused = paused;
    android_game_pause_update();
}
static void on_pause(ANativeActivity *a) { (void)a; android_game_lifecycle_pause(1); }
static void on_stop(ANativeActivity *a) { (void)a; android_game_lifecycle_pause(1); }
static void on_resume(ANativeActivity *a) {
    (void)a;
    game_input_after_ns = (int64_t)mono_ns();
    android_game_lifecycle_pause(0);
    if (g_window) frame_rate_hint_update(a);
    /* The focus facet may still hold the gate until the window regains focus. */
    vk_window_resized(g_window, g_window_epoch);
    present_transform_update(a,g_window_epoch);
}
static void on_destroy(ANativeActivity *a) {
    vk_pcache_shutdown(); window_clear(a); fprintf(stderr, "[totk-app] activity destroyed: exiting\n");
    if (vk_gpuprof_shutdown) vk_gpuprof_shutdown();
    _exit(0);
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    static int started;
    g_activity = a;
    a->callbacks->onNativeWindowCreated = on_window_created;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onNativeWindowResized = on_window_resized;
    a->callbacks->onNativeWindowRedrawNeeded = on_window_redraw;
    a->callbacks->onInputQueueCreated = on_queue_created;
    a->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    a->callbacks->onWindowFocusChanged = on_focus;
    a->callbacks->onContentRectChanged = on_content_rect_changed;
    a->callbacks->onConfigurationChanged = on_configuration_changed;
    a->callbacks->onPause = on_pause;
    a->callbacks->onStop = on_stop;
    a->callbacks->onResume = on_resume;
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    immersive(a);
    text_wake_start();
    /* Not set up (or not finished): nothing to show, so return to the launcher, whose setup screen explains. */
    if (!started) {
        started = 1;
        if (start_game(a) < 0) {
            int rc = android_game_start_finished(a->internalDataPath);
            __android_log_print(ANDROID_LOG_INFO, "TotkSetup", "game start refused: pid=%d marker=%s errno=%d", getpid(), rc ? "remove-error" : "removed", rc ? errno : 0);
            ANativeActivity_finish(a);
            android_refused_exit(1500);
        }
    }
}
#endif

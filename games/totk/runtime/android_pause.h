/* Android suspends the guest at acquire, after giving back its virtual core.
 * No renderer, filesystem, window or UI-thread lock is held by this gate. */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    _Atomic int paused;
    uint64_t suspended_ns, paused_at_ns; /* under lock; CLOCK_MONOTONIC */
} AndroidGamePause;
#define ANDROID_GAME_PAUSE_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }

typedef struct { uint64_t ns; int paused; } AndroidGameSuspension;
static inline uint64_t android_game_pause_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* Include an in-progress suspension: save-completion frames may pass the gate
 * while audio stays frozen. Duration and paused state are one locked snapshot. */
static inline AndroidGameSuspension android_game_pause_suspension(AndroidGamePause *p) {
    pthread_mutex_lock(&p->lock);
    AndroidGameSuspension s = {p->suspended_ns, atomic_load_explicit(&p->paused, memory_order_relaxed)};
    if (s.paused) s.ns += android_game_pause_now_ns() - p->paused_at_ns;
    pthread_mutex_unlock(&p->lock);
    return s;
}

static inline void android_game_pause_set(AndroidGamePause *p, int paused) {
    pthread_mutex_lock(&p->lock);
    paused = !!paused;
    if (paused != atomic_load_explicit(&p->paused, memory_order_relaxed)) {
        uint64_t now = android_game_pause_now_ns();
        if (paused) p->paused_at_ns = now;
        else p->suspended_ns += now - p->paused_at_ns;
    }
    atomic_store_explicit(&p->paused, paused, memory_order_release);
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
}
/* A save may need later guest frames to finish. Let those frames through until
 * Commit, including a save that starts on another thread while we are asleep.
 * This poll does no filesystem I/O and never takes the save/close mutex. */
static inline void android_game_pause_wait(AndroidGamePause *p, int (*save_pending)(void)) {
    if (!atomic_load_explicit(&p->paused, memory_order_acquire)) return;
    pthread_mutex_lock(&p->lock);
    while (atomic_load_explicit(&p->paused, memory_order_acquire) && !save_pending()) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 50000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&p->changed, &p->lock, &until);
    }
    pthread_mutex_unlock(&p->lock);
}

#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
int runtime_android_game_paused(void);
AndroidGameSuspension runtime_android_game_suspension(void);
void runtime_android_game_pause_wait(void);
void runtime_audio_output_pause(int paused); /* atomic notification only; AAudio work belongs to its worker */
#else
static inline int runtime_android_game_paused(void) { return 0; }
static inline AndroidGameSuspension runtime_android_game_suspension(void) { return (AndroidGameSuspension){0}; }
#endif

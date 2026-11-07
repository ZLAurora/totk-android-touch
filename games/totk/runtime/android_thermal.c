/* Thermal reads never run on a guest/render thread. No platform policy writes. */
#include "android_thermal.h"
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include "thermal_step.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t ns; int status; float headroom; } reading;
static _Atomic uint32_t *guest_floor; /* bound before the game starts */
static int launch;
static totk_thermal_policy policy;
static unsigned frames, transitions;
static uint64_t mono_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void *sample_thread(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-thermal");
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    /* Opaque NDK AThermalManager; dlsym keeps API30/31 optional at link time. */
    void *(*acquire)(void) = lib ? dlsym(lib, "AThermal_acquireManager") : NULL;
    void (*release)(void *) = lib ? dlsym(lib, "AThermal_releaseManager") : NULL;
    int (*status)(void *) = lib ? dlsym(lib, "AThermal_getCurrentThermalStatus") : NULL;
    float (*headroom)(void *, int) = lib ? dlsym(lib, "AThermal_getThermalHeadroom") : NULL;
    void *manager = acquire && release && status ? acquire() : NULL;
    fprintf(stderr, "[totk-thermal] API status=%d headroom=%d forecast=10s poll=1Hz nice=10\n",
        manager != NULL, manager && headroom != NULL);
    if (!manager) { if (lib) dlclose(lib); return NULL; }
    totk_thermal_policy poll = {0};
    for (;;) {
        uint64_t now = mono_ns();
        if (totk_thermal_poll(&poll, now)) {
            int s = status(manager);
            float h = headroom ? headroom(manager, 10) : NAN;
            pthread_mutex_lock(&mu);
            reading.ns = now; reading.status = s; reading.headroom = h;
            pthread_mutex_unlock(&mu);
        }
        /* Relative sleep AFTER the API calls: never catch up missed deadlines
         * with a burst of headroom requests. EINTR retains the remaining sleep. */
        struct timespec remaining = {1, 0};
        while (nanosleep(&remaining, &remaining) && errno == EINTR) {}
    }
    /* Process-lifetime thread owns the manager/library until process exit. */
}
static void configure(void) {
    const char *e = getenv("TOTK_THERMAL_STEP");
    launch = e && !strcmp(e, "1");
    fprintf(stderr, "[totk-thermal] TOTK_THERMAL_STEP=%d (startup-only); guest-floor=%s; severe or headroom>=1.0; recovery 30s below moderate and headroom<0.8\n",
        launch, guest_floor ? "available" : "unavailable (requires v143 FPS60)");
    if (!launch) return;
    pthread_t thread;
    int error = pthread_create(&thread, NULL, sample_thread, NULL);
    if (!error) pthread_detach(thread);
    else fprintf(stderr, "[totk-thermal] sampler unavailable: pthread_create=%d\n", error);
}
void runtime_thermal_bind(_Atomic uint32_t *floor) {
    guest_floor = floor;
    atomic_store_explicit(floor, 1, memory_order_release);
}
void runtime_thermal_frame(unsigned interval) {
    pthread_once(&once, configure);
    if (!launch) return;
    static uint64_t sample;
    static int status = -1;
    static float headroom = NAN;
    if (!pthread_mutex_trylock(&mu)) {
        sample = reading.ns;
        if (sample) { status = reading.status; headroom = reading.headroom; }
        pthread_mutex_unlock(&mu);
    }
    uint64_t now = mono_ns();
    int fresh = sample && now >= sample && now - sample <= 2 * TOTK_THERMAL_POLL_NS;
    if (!fresh) policy.cooling = 0;
    if (totk_thermal_step(&policy, guest_floor != NULL, interval,
            fresh ? sample : policy.sample_ns, fresh ? status : -1, fresh ? headroom : NAN)) {
        transitions++;
        fprintf(stderr, "[totk-thermal] transition floor=%u status=%d headroom=%.3f forecast=10s enabled=%d; guest controller decision, no present delay\n",
            policy.capped ? 2u : 1u, status, headroom, launch);
    }
    if (guest_floor) atomic_store_explicit(guest_floor, policy.capped ? 2 : 1, memory_order_release);
    if (++frames == 300) {
        fprintf(stderr, "[totk-thermal] last300 status=%d headroom=%.3f forecast=10s age_ms=%.0f fresh=%d enabled=%d floor=%u guest_interval=%u transitions=%u\n",
            status, headroom, sample && now >= sample ? (now - sample) / 1e6 : -1., fresh,
            launch, policy.capped ? 2u : 1u, interval, transitions);
        frames = transitions = 0;
    }
}
#endif

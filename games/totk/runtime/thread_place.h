/* Optional host scheduling controls, independent of Horizon's virtual-core bookkeeping. */
#pragma once
#include <stdint.h>
#include "android_perf.h"
#include "live_ctl.h"   /* live TOTK_PRIO_RENDER / TOTK_TIMER_SLACK_NS (stubs off Linux) */
#if defined(__linux__)
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Bare decimal means a CPU id/list/range; only 0x-prefixed values mean a bit mask. */
static inline int totk_thread_cpu_number(const char **text, unsigned *cpu) {
    const char *p = *text; unsigned n = 0;
    if (!isdigit((unsigned char)*p)) return 0;
    do { n = n * 10 + (unsigned)(*p++ - '0'); if (n >= CPU_SETSIZE) return 0; } while (isdigit((unsigned char)*p));
    *text = p; *cpu = n; return 1;
}
static inline void totk_thread_spaces(const char **p) { while (isspace((unsigned char)**p)) (*p)++; }
static inline int totk_thread_cpu_parse(const char *text, cpu_set_t *set) {
    CPU_ZERO(set); const char *p = text; totk_thread_spaces(&p);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *first = p + 2, *end = first; while (isxdigit((unsigned char)*end)) end++;
        if (end == first) return 0; p = end; totk_thread_spaces(&p); if (*p) return 0;
        unsigned bit = 0; int any = 0;
        while (end != first) {
            unsigned c = (unsigned char)*--end;
            unsigned digit = c <= '9' ? c - '0' : (c | 32u) - 'a' + 10;
            for (unsigned b = 0; b < 4; b++) if (digit >> b & 1) {
                if (bit + b >= CPU_SETSIZE) return 0;
                CPU_SET(bit + b, set); any = 1;
            }
            if (bit < CPU_SETSIZE) bit += 4; /* Saturate for arbitrarily many leading zeroes. */
        }
        return any;
    }
    for (;;) {
        unsigned first, last; if (!totk_thread_cpu_number(&p, &first)) return 0;
        last = first; totk_thread_spaces(&p);
        if (*p == '-') { p++; totk_thread_spaces(&p); if (!totk_thread_cpu_number(&p, &last) || last < first) return 0; totk_thread_spaces(&p); }
        for (unsigned cpu = first; cpu <= last; cpu++) CPU_SET(cpu, set);
        if (!*p) return 1;
        if (*p++ != ',') return 0; totk_thread_spaces(&p);
    }
}
static inline int totk_thread_integer(const char *text, long lo, long hi, int *value) {
    const char *p = text; totk_thread_spaces(&p);
    if (!isdigit((unsigned char)*p) && !((*p == '-' || *p == '+') && isdigit((unsigned char)p[1]))) return 0;
    char *end; errno = 0; long n = strtol(p, &end, 10);
    if (errno || n < lo || n > hi) return 0;
    p = end; totk_thread_spaces(&p); if (*p) return 0; *value = (int)n; return 1;
}
static inline void totk_thread_mask_text(const cpu_set_t *set, char *text, size_t size) {
    size_t n = 0; text[0] = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, set)) {
        unsigned first = cpu; while (cpu + 1 < CPU_SETSIZE && CPU_ISSET(cpu + 1, set)) cpu++;
        int k = first == cpu ? snprintf(text + n, size - n, "%s%u", n ? "," : "", first) :
            snprintf(text + n, size - n, "%s%u-%u", n ? "," : "", first, cpu);
        if (k < 0 || (size_t)k >= size - n) break; n += (size_t)k;
    }
    if (!n) snprintf(text, size, "none");
}
static inline void totk_thread_setting_log(const char *setting, const char *requested, int result, int error) {
    cpu_set_t set; char mask[CPU_SETSIZE * 6], name[16] = "?";
    if (!sched_getaffinity(0, sizeof set, &set)) totk_thread_mask_text(&set, mask, sizeof mask); else snprintf(mask, sizeof mask, "?");
    pthread_getname_np(pthread_self(), name, sizeof name);
    fprintf(stderr, "[totk-thread] tid=%ld name=%s %s=%s result=%d mask=%s errno=%d%s%s\n",
        (long)syscall(SYS_gettid), name, setting, requested, result, mask, error,
        error ? " " : "", error == EPERM ? "EPERM" : error ? strerror(error) : "");
}
/* Read one complete, positive sysfs integer. Never mix capacity units with kHz:
 * if any allowed CPU lacks capacity, retry the entire allowed set using the
 * hardware maximum of its cpufreq policy (not a user's current frequency cap). */
static inline unsigned totk_thread_capacity_read(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char text[64]; size_t n = fread(text, 1, sizeof text - 1, f);
    int complete = !ferror(f) && feof(f); fclose(f); text[n] = 0;
    int value;
    return complete && totk_thread_integer(text, 1, INT_MAX, &value) ? (unsigned)value : 0;
}
static inline int totk_thread_capacities(const char *root, const cpu_set_t *allowed,
                                       unsigned capacity[CPU_SETSIZE], int frequency) {
    char path[PATH_MAX]; int complete = 1;
    memset(capacity, 0, CPU_SETSIZE * sizeof *capacity);
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, allowed)) {
        snprintf(path, sizeof path, "%s/cpu%u/%s", root, cpu,
                 frequency ? "cpufreq/cpuinfo_max_freq" : "cpu_capacity");
        capacity[cpu] = totk_thread_capacity_read(path);
        if (!capacity[cpu]) complete = 0;
    }
    if (complete || !frequency) return complete;
    /* The per-CPU cpufreq link may be absent; policy membership is authoritative. */
    snprintf(path, sizeof path, "%s/cpufreq", root);
    DIR *dir = opendir(path); if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "policy", 6)) continue;
        snprintf(path, sizeof path, "%s/cpufreq/%s/cpuinfo_max_freq", root, entry->d_name);
        unsigned maximum = totk_thread_capacity_read(path); if (!maximum) continue;
        snprintf(path, sizeof path, "%s/cpufreq/%s/related_cpus", root, entry->d_name);
        FILE *f = fopen(path, "r"); if (!f) continue;
        unsigned cpu;
        while (fscanf(f, "%u", &cpu) == 1)
            if (cpu < CPU_SETSIZE && CPU_ISSET(cpu, allowed) && !capacity[cpu]) capacity[cpu] = maximum;
        fclose(f);
    }
    closedir(dir);
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, allowed) && !capacity[cpu]) return 0;
    return 1;
}
/* Pure policy selection, with an injectable sysfs root for synthetic host tests.
 * Return EINVAL for an explicit invalid override; it must never enable auto. */
static inline int totk_thread_game_select(const char *root, const cpu_set_t *allowed,
        const char *value, cpu_set_t *selected, unsigned capacity[CPU_SETSIZE],
        const char **source, const char **decision) {
    *selected = *allowed; memset(capacity, 0, CPU_SETSIZE * sizeof *capacity);
    *source = "none"; *decision = "unchanged";
    if (value && strcmp(value, "auto")) {
        if (!strcmp(value, "all")) return 0;
        *decision = "explicit";
        if (!totk_thread_cpu_parse(value, selected)) { *selected = *allowed; return EINVAL; }
        CPU_AND(selected, selected, allowed);
        if (!CPU_COUNT(selected)) { *selected = *allowed; return EINVAL; }
        return 0;
    }
    *source = "capacity";
    if (!totk_thread_capacities(root, allowed, capacity, 0)) {
        *source = "max_freq";
        if (!totk_thread_capacities(root, allowed, capacity, 1)) {
            *source = "unavailable"; return 0;
        }
    }
    unsigned low = UINT_MAX, high = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, allowed)) {
        if (capacity[cpu] < low) low = capacity[cpu];
        if (capacity[cpu] > high) high = capacity[cpu];
    }
    if (low == high || (uint64_t)low * 100 >= (uint64_t)high * 60) return 0;
    cpu_set_t big = *allowed;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, allowed) && capacity[cpu] == low) CPU_CLR(cpu, &big);
    if (CPU_COUNT(&big) < 2) return 0; /* Auto never pins guests to one CPU. */
    *selected = big; *decision = "exclude-lowest"; return 0;
}
/* Background IO workers prefer a genuinely slower tier when sysfs can identify
 * one. Missing, single-tier or weakly differentiated topology keeps the inherited
 * allowed mask; never infer cluster membership from logical CPU numbers. */
static inline void totk_thread_background_select(const char *root, const cpu_set_t *allowed,
        cpu_set_t *selected, unsigned capacity[CPU_SETSIZE],
        const char **source, const char **decision) {
    *selected = *allowed; memset(capacity, 0, CPU_SETSIZE * sizeof *capacity);
    *source = "capacity"; *decision = "unchanged";
    if (!totk_thread_capacities(root, allowed, capacity, 0)) {
        *source = "max_freq";
        if (!totk_thread_capacities(root, allowed, capacity, 1)) {
            *source = "unavailable"; return;
        }
    }
    unsigned low = UINT_MAX, high = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, allowed)) {
        if (capacity[cpu] < low) low = capacity[cpu];
        if (capacity[cpu] > high) high = capacity[cpu];
    }
    if (low == high || (uint64_t)low * 100 >= (uint64_t)high * 60) return;
    cpu_set_t slow = *allowed;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, allowed) && capacity[cpu] != low) CPU_CLR(cpu, &slow);
    if (CPU_COUNT(&slow) < 2) return;
    *selected = slow; *decision = "select-lowest";
}


#if defined(__ANDROID__) || defined(TOTK_THREAD_PLACE_TEST)
void totk_thread_game_startup(void);
void totk_thread_game_auto(void);
#ifdef TOTK_THREAD_PLACE_IMPLEMENTATION
/* One process-wide snapshot, shared by Android startup, g-main and every g-N.
 * Startup runs after totk.env and before guest creation. pthread_once also
 * covers the native command-line entry and publishes the immutable snapshot. */
static pthread_once_t totk_game_affinity_once = PTHREAD_ONCE_INIT;
static cpu_set_t totk_game_affinity_mask;
static int totk_game_affinity_valid;
static void totk_thread_game_init(void) {
    cpu_set_t allowed; unsigned capacity[CPU_SETSIZE]; const char *source, *decision;
    if (sched_getaffinity(0, sizeof allowed, &allowed)) {
        fprintf(stderr, "[totk-thread] game affinity auto: source=unavailable cpus=? allowed=? mask=? decision=unchanged errno=%d\n", errno);
        return;
    }
    totk_thread_game_select("/sys/devices/system/cpu", &allowed, NULL,
                           &totk_game_affinity_mask, capacity, &source, &decision);
    char before[CPU_SETSIZE * 6], after[CPU_SETSIZE * 6], cpus[CPU_SETSIZE * 24];
    totk_thread_mask_text(&allowed, before, sizeof before);
    totk_thread_mask_text(&totk_game_affinity_mask, after, sizeof after);
    size_t n = 0; cpus[0] = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, &allowed)) {
        int k = snprintf(cpus + n, sizeof cpus - n, "%s%u:%u", n ? "," : "", cpu, capacity[cpu]);
        if (k < 0 || (size_t)k >= sizeof cpus - n) break; n += (size_t)k;
    }
    fprintf(stderr, "[totk-thread] game affinity auto: source=%s cpus=%s allowed=%s mask=%s decision=%s\n",
            source, cpus, before, after, decision);
    totk_game_affinity_valid = 1;
}
void totk_thread_game_startup(void) {
    int saved = errno; const char *value = getenv("TOTK_AFFINITY_GAME");
    if (!value || !strcmp(value, "auto")) pthread_once(&totk_game_affinity_once, totk_thread_game_init);
    errno = saved;
}
void totk_thread_game_auto(void) {
    totk_thread_game_startup();
    if (!totk_game_affinity_valid) return;
    int error = sched_setaffinity(0, sizeof totk_game_affinity_mask, &totk_game_affinity_mask) ? errno : 0;
    totk_thread_setting_log("TOTK_AFFINITY_GAME", "auto", error ? -1 : 0, error);
}
#endif
#endif
static inline void totk_thread_affinity(const char *key) {
    if (!key) return; const char *value = getenv(key);
    if (!strcmp(key, "TOTK_AFFINITY_GAME")) {
        if (value && !strcmp(value, "all")) return;
#if defined(__ANDROID__) || defined(TOTK_THREAD_PLACE_TEST)
        if (!value || !strcmp(value, "auto")) { totk_thread_game_auto(); return; }
#else
        if (value && !strcmp(value, "auto")) return;
#endif
    }
    if (!value) return;
    cpu_set_t set; int error = 0;
    if (!totk_thread_cpu_parse(value, &set)) error = EINVAL;
    else if (sched_setaffinity(0, sizeof set, &set)) error = errno;
    totk_thread_setting_log(key, value, error ? -1 : 0, error);
}
/* Linux per-tid nice from a decimal -20..19 setting. Unset leaves the inherited/current nice untouched. */
static inline void totk_thread_nice(const char *key) {
    const char *value = getenv(key); if (!value) return;
    int nice, error = 0;
    if (!totk_thread_integer(value, -20, 19, &nice)) error = EINVAL;
    else if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice)) error = errno;
    int result = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    totk_thread_setting_log(key, value, result, error);
}
/* One process-wide flag for every translation unit that includes this header (a weak definition is merged). */
__attribute__((weak)) int totk_thread_slack_logged;
/* TOTK_TIMER_SLACK_NS=1..50000: per-thread timer slack of game and frame-critical threads (Linux defaults to
   50 us, which every timed sleep or timeout of that thread may overshoot by). Unset leaves it untouched. */
static inline void totk_thread_timer_slack(void) {
    const char *value = getenv("TOTK_TIMER_SLACK_NS"); if (!value) return;
    int ns, error = 0;
    if (!totk_thread_integer(value, 1, 50000, &ns)) error = EINVAL;
    else if (prctl(PR_SET_TIMERSLACK, (unsigned long)ns, 0, 0, 0)) error = errno;
    if (error || !__atomic_exchange_n(&totk_thread_slack_logged, 1, __ATOMIC_RELAXED))
        totk_thread_setting_log("TOTK_TIMER_SLACK_NS", value, (int)prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0), error);
}
/* Host threads on the frame path: walker/submission, recorder workers, ordered submit, GPU-fence and completion
   (post) threads, the presenter and the display callback thread. Compile, seed, cache and translator workers are not. */
static inline int totk_thread_frame_critical(const char *name) {
    return !strcmp(name, "totk-walk") || !strncmp(name, "totk-recw", 9) || !strcmp(name, "totk-submit") ||
        !strcmp(name, "totk-gpufence") || !strcmp(name, "totk-post") || !strcmp(name, "totk-present") ||
        !strcmp(name, "totk-vsync");
}
static inline void totk_thread_start(const char *name, const char *affinity_key) {
    int saved = errno; char short_name[16]; snprintf(short_name, sizeof short_name, "%s", name);
    int error = pthread_setname_np(pthread_self(), short_name);
    totk_thread_setting_log("name", short_name, error ? -1 : 0, error);
    if (!strncmp(name, "g-", 2) || !strncmp(name, "totk-recw", 9) || !strcmp(name, "totk-submit") ||
        !strcmp(name, "totk-present"))
        totk_perf_thread(name);
    int frame = totk_thread_frame_critical(name);
    if (frame || !strncmp(name, "g-", 2)) { totk_thread_timer_slack(); totk_live_slack_tick(); }   /* a live value wins */
    /* Render threads register with their inherited nice, so a live TOTK_PRIO_RENDER can reach them by tid. */
    if (frame && !totk_live_register_render()) totk_thread_nice("TOTK_PRIO_RENDER");
    totk_thread_affinity(affinity_key); errno = saved;
}
/* Linux UAPI sched_attr v1. KEEP_POLICY/PARAMS preserve the caller's scheduler and nice value. */
typedef struct {
    uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t priority;
    uint64_t runtime, deadline, period; uint32_t util_min, util_max;
} totk_thread_sched_attr;
static inline int totk_thread_uclamp_set(unsigned minimum) {
#ifdef SYS_sched_setattr
    totk_thread_sched_attr a = {.size = sizeof a, .flags = 0x08 | 0x10 | 0x20, .util_min = minimum};
    if (syscall(SYS_sched_setattr, 0, &a, 0)) return errno;
    return 0;
#else
    (void)minimum; return ENOSYS;
#endif
}
static inline int totk_thread_uclamp_get(void) {
#ifdef SYS_sched_getattr
    totk_thread_sched_attr a = {.size = sizeof a};
    if (!syscall(SYS_sched_getattr, 0, &a, sizeof a, 0)) return (int)a.util_min;
#endif
    return -1;
}
static inline void totk_thread_walk(void) {
    static _Thread_local int identified; if (identified) return; identified = 1;
    int saved = errno; totk_thread_start("totk-walk", "TOTK_AFFINITY_WALK");
    totk_thread_nice("TOTK_PRIO_WALK");   /* after TOTK_PRIO_RENDER: the walker's own setting wins */
    if (getenv("TOTK_PRIO_WALK")) totk_live_pin_render();   /* also over a live TOTK_PRIO_RENDER */
    const char *value = getenv("TOTK_UCLAMP_WALK");
    if (value) {
        int minimum, error;
        if (!totk_thread_integer(value, 0, 1024, &minimum)) error = EINVAL;
        else error = totk_thread_uclamp_set((unsigned)minimum);
        totk_thread_setting_log("TOTK_UCLAMP_WALK", value, totk_thread_uclamp_get(), error);
    }
    errno = saved;
}
static inline void totk_thread_game(uint64_t id) {
    char name[16]; snprintf(name, sizeof name, "g-%llu", (unsigned long long)id);
    totk_thread_start(name, "TOTK_AFFINITY_GAME");
}
#else
static inline void totk_thread_start(const char *name, const char *key) { (void)name; (void)key; }
static inline void totk_thread_walk(void) {}
static inline void totk_thread_game(uint64_t id) { (void)id; }
#endif

/* Pure ADPF session state, shared by Android and the fake-API host tests. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

enum { TOTK_ADPF_MAX_THREADS = 16 };
typedef struct {
    void *manager;
    void *(*create)(void *, const int32_t *, size_t, int64_t);
    int (*update)(void *, int64_t);
    int (*report)(void *, int64_t);
    int (*set_threads)(void *, const int32_t *, size_t);
    void (*close)(void *);
    /* API35 (TOTK_ADPF_GPU): one AWorkDuration, created once and reused for every report. */
    int (*report2)(void *, void *);
    void (*set_start)(void *, int64_t), (*set_total)(void *, int64_t);
    void (*set_cpu)(void *, int64_t), (*set_gpu)(void *, int64_t);
    void *work;
    /* API36; resolved dynamically even when the build targets API33. */
    int (*increase)(void *, bool, bool, const char *);
} totk_adpf_api;
typedef struct {
    void *session;
    int64_t target;
    int32_t tids[TOTK_ADPF_MAX_THREADS];
    size_t count;
    unsigned attempted;
} totk_adpf_state;
enum { TOTK_ADPF_CREATED = 1, TOTK_ADPF_TARGET = 2, TOTK_ADPF_THREADS = 4,
       TOTK_ADPF_REFUSED = 8, TOTK_ADPF_REPORT_ERROR = 16 };

/* The real tick, never the synthetic midpoint or measured display refresh.
 * Shipping/locked 30 stays 33.3ms; native60/auto follows its effective interval. */
static inline int64_t totk_adpf_target(uint64_t period_ns, int fps60, int framegen) {
    return !framegen && fps60 && period_ns && period_ns < 25000000 ? 16666667 : 33333333;
}
static inline unsigned totk_frame_rate(unsigned real_hz, int framegen, int lattice) {
    /* A 30-Hz physical mode would turn the two-slot 30fps lattice into 15fps. */
    return framegen || lattice || real_hz >= 60 ? 60 : 30;
}
/* The opt-in auto40 profile must not select a physical30Hz mode, even while
 * its scripted-camera floor is active. Preserve higher validated overrides. */
static inline unsigned totk_frame_rate_minimum(int auto40, unsigned hz) {
    return auto40 && hz < 60 ? 60 : hz;
}
/* Startup-only integer decimal Hz; 0/unset disables the override. The actual window display must separately
 * advertise the requested rate before it is used. Reject signs, fractions, suffixes and overflow. */
static inline int totk_frame_rate_hint_parse(const char *s, unsigned *hz) {
    *hz = 0;
    if (!s || !*s) return 1;
    unsigned value = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || value > (2147483647u - (unsigned)(*s - '0')) / 10) return 0;
        value = value * 10 + (unsigned)(*s - '0');
    }
    *hz = value;
    return 1;
}
/* TOTK_RATE_HINT_HOLD: the window's output rate from the game's own interval (60-Hz units), never from the measured
 * present period, which follows the panel: a panel that took a 30 Hz request would otherwise read as a 30 fps game
 * forever. Host floors (cutscenes, FPS auto) only lower what is shown, so leaving them out can only ask for more.
 * Frame generation doubles real frames of at most 30fps; the 30fps lattice keeps its 60 Hz clock. */
static inline unsigned totk_output_rate(int guest_interval, int generating, int lattice) {
    unsigned real = 60u / (unsigned)(guest_interval > 0 ? guest_interval : 1);
    if (!real) real = 1;
    if (generating && real > 30) real = 30;
    unsigned out = generating ? real * 2 : real;
    return lattice && out < 60 ? 60 : out;
}
/* The highest rate requested within the last hold_ns: up at once, down only after the lower rate has held that
 * long. A few distinct rates are ever in use; the least recently used entry is replaced. */
enum { TOTK_RATE_HOLD_RATES = 8 };
typedef struct { unsigned hz[TOTK_RATE_HOLD_RATES]; uint64_t last[TOTK_RATE_HOLD_RATES]; } totk_rate_hold;
static inline unsigned totk_rate_hold_step(totk_rate_hold *h, unsigned hz, uint64_t now, uint64_t hold_ns) {
    unsigned slot = TOTK_RATE_HOLD_RATES, victim = 0;
    for (unsigned i = 0; i < TOTK_RATE_HOLD_RATES && slot == TOTK_RATE_HOLD_RATES; i++) {
        if (h->hz[i] == hz) slot = i;
        else if (h->hz[victim] && (!h->hz[i] || h->last[i] < h->last[victim])) victim = i;   /* empty, else LRU */
    }
    if (slot == TOTK_RATE_HOLD_RATES) slot = victim;
    h->hz[slot] = hz; h->last[slot] = now;
    unsigned held = hz;
    for (unsigned i = 0; i < TOTK_RATE_HOLD_RATES; i++)
        if (h->hz[i] > held && now - h->last[i] < hold_ns) held = h->hz[i];
    return held;
}
static inline uint64_t totk_adpf_work(uint64_t previous, uint64_t now, uint64_t pacing) {
    if (!previous || now <= previous || pacing >= now - previous) return 0;
    uint64_t work = now - previous - pacing;
    return work < 250000000 ? work : 0; /* background/loading discontinuity */
}
static inline void totk_adpf_close(totk_adpf_state *s, const totk_adpf_api *api) {
    if (s->session) api->close(s->session);
    s->session = NULL;
}
/* Called with canonical sorted unique positive tids. Failed configurations are
 * attempted once, not allocated/retried every frame. A real change permits retry.
 * API33 has no setThreads: recreate only at membership changes. */
static inline unsigned totk_adpf_sync(totk_adpf_state *s, const totk_adpf_api *api,
        const int32_t *tids, size_t count, int64_t target) {
    if (!count || count > TOTK_ADPF_MAX_THREADS || target <= 0) return 0;
    int threads = count != s->count || memcmp(s->tids, tids, count * sizeof *tids);
    int duration = target != s->target;
    if (s->attempted && !threads && !duration) return 0;
    unsigned result = 0;
    if (s->session && threads) {
        if (!api->set_threads || api->set_threads(s->session, tids, count)) totk_adpf_close(s, api);
        else result |= TOTK_ADPF_THREADS;
    }
    if (s->session && duration) {
        if (!api->update || api->update(s->session, target)) totk_adpf_close(s, api);
        else result |= TOTK_ADPF_TARGET;
    }
    memcpy(s->tids, tids, count * sizeof *tids); s->count = count;
    s->target = target; s->attempted = 1;
    if (!s->session) {
        s->session = api->manager && api->create && api->report && api->close ?
            api->create(api->manager, tids, count, target) : NULL;
        result |= s->session ? TOTK_ADPF_CREATED : TOTK_ADPF_REFUSED;
    }
    return result;
}
static inline unsigned totk_adpf_report(totk_adpf_state *s, const totk_adpf_api *api, uint64_t ns) {
    if (!s->session || !ns || ns >= 250000000) return 0;
    return api->report(s->session, (int64_t)ns) ? TOTK_ADPF_REPORT_ERROR : 0;
}

/* Three consecutive valid spans above 105 % of target, at most one request per 500ms.
 * Record attempts, not just successes: platform refusal must not cause a per-frame retry storm. */
typedef struct { unsigned over; uint64_t last; int attempted; } totk_adpf_load;
static inline int totk_adpf_load_step(totk_adpf_load *s, uint64_t now, uint64_t span, int64_t target) {
    if (target <= 0 || !span || span >= 250000000 || span <= (uint64_t)target * 105 / 100) {
        s->over = 0; return 0;
    }
    if (s->over < 3) s->over++;
    if (s->over < 3 || (s->attempted && (now < s->last || now - s->last < 500000000))) return 0;
    s->last = now; s->attempted = 1; return 1;
}
static inline bool totk_adpf_load_gpu(uint64_t gpu, int64_t target) {
    return target > 0 && gpu > (uint64_t)target * 70 / 100;
}

/* The span already excludes acquire pacing. Only disjoint present/texture queue
 * waits still inside that span leave its CPU field; never subtract pacing twice. */
static inline uint64_t totk_adpf_cpu(uint64_t span, uint64_t queue_wait, int split) {
    return !split ? span : queue_wait < span ? span - queue_wait : 0;
}

/* ---------- TOTK_ADPF_GPU: the frame's span and the GPU duration ----------
 * The target defaults to 90 % of the configured frame period (16.7ms native60, else 33.3ms), capped at 33.3ms. It never
 * follows the controller's current interval: after a drop to 30 or 20fps that period as the target would ask for
 * clocks that keep the frame that slow. */
static inline int64_t totk_adpf_span_target(int fps60, int framegen, int pct) {
    if (pct < 50 || pct > 100) pct = 90;
    int64_t period = !framegen && fps60 ? 16666667 : 33333333, target = period * pct / 100;
    return target < 33333333 ? target : 33333333;
}
/* From the frame's start to its present, less the pacing waits inside it. Never the present interval:
 * that is the period whatever the work took, so every frame would read "on target". */
static inline uint64_t totk_adpf_span(uint64_t start, uint64_t present, uint64_t pacing) {
    if (!start || present <= start || pacing >= present - start) return 0;
    uint64_t span = present - start - pacing;
    return span < 250000000 ? span : 0; /* background/loading discontinuity */
}
/* The frame starts at the game's own CPU frame origin (its controller measures completed work from there), read at
 * the frame's first submit; without a usable origin (other versions, a deterministic clock) at that submit. */
static inline uint64_t totk_adpf_frame_start(uint64_t origin_ns, uint64_t now) {
    return origin_ns && origin_ns <= now && now - origin_ns < 250000000 ? origin_ns : now;
}
/* The acquire pacing waits since the previous present. The start is known only at the frame's first submit, which
 * comes after the acquire, so the waits are kept and clipped to the span at the present. */
enum { TOTK_ADPF_PACING = 4 };
typedef struct { uint64_t begin[TOTK_ADPF_PACING], end[TOTK_ADPF_PACING]; unsigned n; } totk_adpf_pacing;
static inline void totk_adpf_pacing_add(totk_adpf_pacing *p, uint64_t begin, uint64_t end) {
    if (end <= begin) return;
    if (p->n == TOTK_ADPF_PACING) { p->end[p->n - 1] = end; return; }   /* more waits: the last one grows */
    p->begin[p->n] = begin; p->end[p->n] = end; p->n++;
}
static inline uint64_t totk_adpf_pacing_in_span(const totk_adpf_pacing *p, uint64_t start, uint64_t present) {
    uint64_t n = 0;
    for (unsigned i = 0; start && i < p->n; i++) {
        uint64_t begin = p->begin[i] > start ? p->begin[i] : start, end = p->end[i] < present ? p->end[i] : present;
        if (end > begin) n += end - begin;
    }
    return n;
}
/* GPU busy time per guest frame, fed in completion order by the one completion thread. A frame closes when the
 * first slot of a later frame completes; slots of no guest frame (backend work) join the open frame. A frame first
 * seen mid-way (after a reset) is incomplete and never published. Returns 0, or the closed frame packed as
 * (frame id << 32 | GPU ns) for a single atomic publication. */
typedef struct { uint64_t frame, gpu_ns; int whole; } totk_adpf_gpu_acc;
static inline uint64_t totk_adpf_gpu_slot(totk_adpf_gpu_acc *a, uint64_t frame, uint64_t gpu_ns) {
    uint64_t closed = 0;
    if (frame > a->frame) {
        if (a->frame && a->whole && a->gpu_ns && a->gpu_ns < 250000000)
            closed = (uint64_t)((uint32_t)a->frame ? (uint32_t)a->frame : 1u) << 32 | a->gpu_ns;
        a->whole = a->frame != 0; a->frame = frame; a->gpu_ns = 0;
    }
    if (a->frame) a->gpu_ns += gpu_ns;
    return closed;
}
/* The newest completed frame's GPU ns, once: 0 when it was already reported (or none completed). */
static inline uint64_t totk_adpf_gpu_take(uint64_t published, uint32_t *seen) {
    uint32_t id = (uint32_t)(published >> 32);
    if (!id || id == *seen) return 0;
    *seen = id;
    return (uint32_t)published;
}
/* API35 reports start, total, CPU and GPU; older APIs report the span alone.
 * A zero GPU field means "not measured this report". */
static inline unsigned totk_adpf_report_span(totk_adpf_state *s, const totk_adpf_api *api, uint64_t start,
        uint64_t span, uint64_t cpu_ns, uint64_t gpu_ns) {
    if (!s->session || !start || !span || span >= 250000000) return 0;
    if (!api->report2 || !api->work) return api->report(s->session, (int64_t)span) ? TOTK_ADPF_REPORT_ERROR : 0;
    if (!cpu_ns && !gpu_ns) return 0; /* No measured CPU/GPU work: report2 rejects both fields zero. */
    api->set_start(api->work, (int64_t)start); api->set_total(api->work, (int64_t)span);
    api->set_cpu(api->work, (int64_t)cpu_ns); api->set_gpu(api->work, (int64_t)gpu_ns);
    return api->report2(s->session, api->work) ? TOTK_ADPF_REPORT_ERROR : 0;
}

#include "../games/totk/runtime/adpf_policy.h"
#include <assert.h>
#include <stdio.h>

static unsigned creates, closes, updates, sets, reports;
static int refuse_create, refuse_update, refuse_set, refuse_report;
static int64_t actual, target_seen;
static int marker;
static void *create(void *manager, const int32_t *tids, size_t n, int64_t target) {
    assert(manager == &marker && n && tids[0] > 0); creates++; target_seen = target;
    return refuse_create ? NULL : &marker;
}
static void close_session(void *s) { assert(s == &marker); closes++; }
static int update(void *s, int64_t target) { assert(s == &marker); updates++; target_seen = target; return refuse_update; }
static int set_threads(void *s, const int32_t *tids, size_t n) { assert(s == &marker && tids[0] > 0 && n); sets++; return refuse_set; }
static int report(void *s, int64_t ns) { assert(s == &marker); reports++; actual = ns; return refuse_report; }
/* API35 AWorkDuration fake: one object, its four fields, and reportActualWorkDuration2. */
static int work_object;
static int64_t w_start, w_total, w_cpu, w_gpu;
static unsigned reports2;
static void set_start(void *w, int64_t v) { assert(w == &work_object); w_start = v; }
static void set_total(void *w, int64_t v) { assert(w == &work_object); w_total = v; }
static void set_cpu(void *w, int64_t v) { assert(w == &work_object); w_cpu = v; }
static void set_gpu(void *w, int64_t v) { assert(w == &work_object); w_gpu = v; }
static int report2(void *s, void *w) {
    assert(s == &marker && w == &work_object);
    assert(w_start > 0 && w_total > 0 && w_cpu >= 0 && w_gpu >= 0 && (w_cpu || w_gpu)); /* the platform's EINVAL rules */
    reports2++; return refuse_report;
}
static void span_policy(void) {
    /* Target: 90 % of the configured period, capped at 33.3ms; the controller's current interval is not an input,
     * so a drop to 30 or 20fps (period 33.3/50ms) cannot lower the clocks that would bring it back. */
    assert(totk_adpf_span_target(1, 0, 90) == 15000000);
    assert(totk_adpf_span_target(0, 0, 90) == 29999999);
    assert(totk_adpf_span_target(1, 1, 90) == 29999999); /* framegen: real frames at 30 */
    assert(totk_adpf_target(50000000, 0, 0) == 33333333 && totk_adpf_span_target(0, 0, 90) < 33333333);
    /* Span, not period: a 33.3ms present interval whose frame started 20ms before the present, with 5ms of
     * pacing inside, reports 15ms. The old work measure (interval minus pacing) would say 28.3ms. */
    const uint64_t present = 1000000000, start = present - 20000000;
    /* The start: the game's frame origin when it is plausible, else the first submit. */
    assert(totk_adpf_frame_start(start, start + 4000000) == start);
    assert(totk_adpf_frame_start(0, start) == start);
    assert(totk_adpf_frame_start(start + 1, start) == start); /* origin in the future: another clock */
    assert(totk_adpf_frame_start(start - 250000000, start) == start); /* stale origin (paused/loading) */
    /* Pacing: the acquire comes before the first submit, so waits are kept and clipped at the present. */
    totk_adpf_pacing waits = {0};
    totk_adpf_pacing_add(&waits, start - 9000000, start - 1000000); /* before the frame: not in the span */
    totk_adpf_pacing_add(&waits, start + 2000000, start + 2000000); /* empty: dropped */
    totk_adpf_pacing_add(&waits, start + 2000000, start + 6000000);
    assert(waits.n == 2 && totk_adpf_pacing_in_span(&waits, start, present) == 4000000);
    totk_adpf_pacing_add(&waits, start - 3000000, start + 1000000); /* straddles the start: 1ms inside */
    assert(totk_adpf_pacing_in_span(&waits, start, present) == 5000000);
    assert(totk_adpf_pacing_in_span(&waits, 0, present) == 0); /* no frame start: no span */
    totk_adpf_pacing_add(&waits, present - 1000000, present + 4000000); /* clipped at the present */
    assert(waits.n == TOTK_ADPF_PACING && totk_adpf_pacing_in_span(&waits, start, present) == 6000000);
    totk_adpf_pacing_add(&waits, present + 5000000, present + 6000000); /* full: the last wait grows, no overflow */
    assert(waits.n == TOTK_ADPF_PACING && waits.end[TOTK_ADPF_PACING - 1] == present + 6000000);
    uint64_t pacing = 5000000;
    assert(totk_adpf_span(start, present, pacing) == 15000000);
    assert(totk_adpf_work(present - 33333333, present, pacing) == 28333333);
    assert(totk_adpf_span(0, present, 0) == 0 && totk_adpf_span(present, present, 0) == 0);
    assert(totk_adpf_span(start, present, 20000000) == 0); /* all pacing */
    assert(totk_adpf_span(present - 250000000, present, 0) == 0 && totk_adpf_span(present - 249999999, present, 0) == 249999999);
    /* Once-only GPU: frames close in completion order when a later frame's first slot completes. */
    totk_adpf_gpu_acc acc = {0}; uint32_t seen = 0; uint64_t published = 0;
    assert(!totk_adpf_gpu_slot(&acc, 5, 9000000)); /* first seen mid-frame: incomplete */
    assert(!totk_adpf_gpu_slot(&acc, 0, 1000000));
    assert(!totk_adpf_gpu_take(published, &seen));
    assert(!totk_adpf_gpu_slot(&acc, 6, 3000000)); /* frame 5 is never published */
    assert(!totk_adpf_gpu_slot(&acc, 0, 1000000)); /* backend work joins the open frame */
    assert(!totk_adpf_gpu_slot(&acc, 6, 4000000));
    published = totk_adpf_gpu_slot(&acc, 7, 2000000);
    assert(published == ((uint64_t)6 << 32 | 8000000));
    assert(totk_adpf_gpu_take(published, &seen) == 8000000 && seen == 6);
    for (int i = 0; i < 3; i++) assert(!totk_adpf_gpu_take(published, &seen)); /* reported once */
    published = totk_adpf_gpu_slot(&acc, 9, 1000000); /* frame 8 had no slot */
    assert(published == ((uint64_t)7 << 32 | 2000000));
    assert(!totk_adpf_gpu_slot(&acc, 9, 250000000));
    assert(!totk_adpf_gpu_slot(&acc, 10, 0)); /* a >=250ms frame (loading) is not published */
    assert(!totk_adpf_gpu_slot(&acc, 11, 1)); /* nor one without GPU time (no timestamps) */
    assert(totk_adpf_gpu_slot(&acc, 12, 0) == ((uint64_t)11 << 32 | 1));
    assert(totk_adpf_gpu_take(published, &seen) == 2000000 && !totk_adpf_gpu_take(published, &seen));
    acc = (totk_adpf_gpu_acc){0}; /* a reset (switch off/on) */
    assert(!totk_adpf_gpu_slot(&acc, 12, 5000000) && !totk_adpf_gpu_slot(&acc, 13, 5000000));
    assert(totk_adpf_gpu_slot(&acc, 14, 1) == ((uint64_t)13 << 32 | 5000000));
    /* Reporting: API35 gets start, total = CPU = span and the GPU field; GPU 0 means not measured. */
    totk_adpf_api api = {.manager=&marker, .create=create, .update=update, .report=report, .close=close_session,
        .report2=report2, .set_start=set_start, .set_total=set_total, .set_cpu=set_cpu, .set_gpu=set_gpu, .work=&work_object};
    totk_adpf_state state = {0}; int32_t tid = 7; unsigned before = reports;
    assert(totk_adpf_sync(&state, &api, &tid, 1, totk_adpf_span_target(1, 0, 90)) == TOTK_ADPF_CREATED);
    assert(target_seen == 15000000);
    assert(!totk_adpf_report_span(&state, &api, start, 15000000, 15000000, 8000000));
    assert(reports2 == 1 && w_start == (int64_t)start && w_total == 15000000 && w_cpu == 15000000 && w_gpu == 8000000);
    assert(!totk_adpf_report_span(&state, &api, start, 14000000, 14000000, 0));
    assert(reports2 == 2 && w_total == 14000000 && w_gpu == 0);
    assert(!totk_adpf_report_span(&state, &api, 0, 14000000, 14000000, 0) && !totk_adpf_report_span(&state, &api, start, 0, 0, 0));
    assert(!totk_adpf_report_span(&state, &api, start, 250000000, 250000000, 0) && reports2 == 2 && reports == before);
    refuse_report = 1;
    assert(totk_adpf_report_span(&state, &api, start, 14000000, 14000000, 0) == TOTK_ADPF_REPORT_ERROR);
    refuse_report = 0;
    api.report2 = NULL; /* below API35: the span alone through reportActualWorkDuration */
    assert(!totk_adpf_report_span(&state, &api, start, 13000000, 13000000, 8000000));
    assert(reports == before + 1 && actual == 13000000 && reports2 == 3);
    totk_adpf_close(&state, &api);
    assert(!totk_adpf_report_span(&state, &api, start, 13000000, 13000000, 0) && reports == before + 1);
}
static void rate_hold_policy(void) {
    /* Output rate: the game's interval in 60-Hz units, doubled while frame generation runs (real frames <= 30). */
    assert(totk_output_rate(1, 0, 0) == 60 && totk_output_rate(2, 0, 0) == 30 && totk_output_rate(3, 0, 0) == 20);
    assert(totk_output_rate(0, 0, 0) == 60 && totk_output_rate(-1, 0, 0) == 60 && totk_output_rate(61, 0, 0) == 1);
    assert(totk_output_rate(2, 1, 0) == 60);
    assert(totk_output_rate(3, 1, 0) == 40); /* not a forced 60: 20 real + 20 generated */
    assert(totk_output_rate(1, 1, 0) == 60); /* generation runs from at most 30 real frames */
    assert(totk_output_rate(2, 0, 1) == 60 && totk_output_rate(3, 1, 1) == 60); /* the 60 Hz present lattice */
    assert(totk_frame_rate(30, 1, 0) == 60); /* the configured rate (switch off) is unchanged */
    /* Hold: up at once, down only after the lower rate held for the whole window. */
    const uint64_t s = 1000000000, hold = 5 * s;
    totk_rate_hold h = {0};
    assert(totk_rate_hold_step(&h, 60, 10 * s, hold) == 60);
    assert(totk_rate_hold_step(&h, 30, 11 * s, hold) == 60);
    assert(totk_rate_hold_step(&h, 30, 14 * s, hold) == 60);
    assert(totk_rate_hold_step(&h, 30, 15 * s - 1, hold) == 60);
    assert(totk_rate_hold_step(&h, 30, 15 * s, hold) == 30); /* 60 last used 5 s ago */
    assert(totk_rate_hold_step(&h, 60, 16 * s, hold) == 60); /* up at once */
    assert(totk_rate_hold_step(&h, 20, 17 * s, hold) == 60);
    assert(totk_rate_hold_step(&h, 30, 19 * s, hold) == 60);
    assert(totk_rate_hold_step(&h, 20, 21 * s, hold) == 30); /* 60 expired; 30 (19 s) is the highest still held */
    assert(totk_rate_hold_step(&h, 20, 24 * s, hold) == 20);
    /* A 60->30->60 flap within the window never requests 30. */
    totk_rate_hold flap = {0};
    for (uint64_t t = 0; t < 60; t++)
        assert(totk_rate_hold_step(&flap, t % 7 >= 4 ? 30 : 60, 100 * s + t * s / 2, hold) == 60);
    /* hold 0: the requested rate itself. */
    totk_rate_hold none = {0};
    assert(totk_rate_hold_step(&none, 60, s, 0) == 60 && totk_rate_hold_step(&none, 30, s + 1, 0) == 30);
    /* More distinct rates than entries: the least recently used is replaced, the newest are kept. */
    totk_rate_hold many = {0};
    for (unsigned i = 0; i < 3 * TOTK_RATE_HOLD_RATES; i++) totk_rate_hold_step(&many, 10 + i, s + i, hold);
    assert(totk_rate_hold_step(&many, 1, s + 100, hold) == 10 + 3 * TOTK_RATE_HOLD_RATES - 1);
    for (unsigned i = 0; i < TOTK_RATE_HOLD_RATES; i++) assert(many.hz[i] != 10 + 2 * TOTK_RATE_HOLD_RATES);
}
int main(void) {
    assert(totk_adpf_target(16666667, 0, 0) == 33333333);
    assert(totk_adpf_target(33333333, 0, 1) == 33333333);
    assert(totk_adpf_target(16666667, 1, 1) == 33333333);
    assert(totk_adpf_target(16666667, 1, 0) == 16666667);
    assert(totk_adpf_target(33333333, 1, 0) == 33333333); /* auto drop */
    assert(totk_adpf_target(0, 1, 0) == 33333333);
    assert(totk_frame_rate(30, 0, 0) == 30);
    assert(totk_frame_rate(30, 0, 1) == 60);
    assert(totk_frame_rate(30, 1, 0) == 60);
    assert(totk_frame_rate(60, 0, 0) == 60);
    assert(totk_adpf_work(0, 100, 0) == 0);
    assert(totk_adpf_work(100, 99, 0) == 0);
    assert(totk_adpf_work(100, 40000100, 10000000) == 30000000);
    assert(totk_adpf_work(100, 40000100, 40000000) == 0);
    assert(totk_adpf_work(100, 250000100, 0) == 0);
    assert(totk_adpf_work(100, 250000099, 0) == 249999999);
    totk_adpf_api api = {.manager=&marker, .create=create, .update=update,
        .report=report, .set_threads=set_threads, .close=close_session};
    totk_adpf_state state = {0}; int32_t tids[] = {11, 22, 33};
    assert(!totk_adpf_sync(&state, &api, tids, 0, 33333333));
    assert(!totk_adpf_sync(&state, &api, tids, 1, 0));
    assert(totk_adpf_sync(&state, &api, tids, 1, 33333333) == TOTK_ADPF_CREATED);
    assert(creates == 1 && target_seen == 33333333);
    for (int frame=0; frame<1000; frame++) {
        assert(!totk_adpf_sync(&state, &api, tids, 1, 33333333));
        assert(!totk_adpf_report(&state, &api, 23000000));
    }
    assert(creates == 1 && sets == 0 && updates == 0 && reports == 1000 && actual == 23000000);
    assert(!totk_adpf_report(&state, &api, 0));
    assert(!totk_adpf_report(&state, &api, 250000000)); assert(reports == 1000);
    assert(totk_adpf_sync(&state, &api, tids, 2, 16666667) == (TOTK_ADPF_THREADS | TOTK_ADPF_TARGET));
    assert(sets == 1 && updates == 1 && creates == 1 && target_seen == 16666667);
    api.set_threads = NULL; /* Android33 membership change */
    assert(totk_adpf_sync(&state, &api, tids, 3, 16666667) == TOTK_ADPF_CREATED);
    assert(creates == 2 && closes == 1 && state.count == 3);
    refuse_update = 1;
    assert(totk_adpf_sync(&state, &api, tids, 3, 33333333) == TOTK_ADPF_CREATED);
    assert(creates == 3 && closes == 2 && state.target == 33333333);
    refuse_report = 1;
    assert(totk_adpf_report(&state, &api, 22000000) == TOTK_ADPF_REPORT_ERROR);
    totk_adpf_close(&state, &api); assert(!state.session && closes == 3);
    state = (totk_adpf_state){0}; refuse_create = 1;
    assert(totk_adpf_sync(&state, &api, tids, 1, 33333333) == TOTK_ADPF_REFUSED);
    for (int frame=0; frame<1000; frame++) assert(!totk_adpf_sync(&state, &api, tids, 1, 33333333));
    assert(creates == 4); /* no per-frame allocation/retry after refusal */
    refuse_create = refuse_update = refuse_report = 0;
    assert(totk_adpf_sync(&state, &api, tids, 1, 16666667) == TOTK_ADPF_CREATED);
    api.set_threads = set_threads; refuse_set = 1;
    assert(totk_adpf_sync(&state, &api, tids, 2, 16666667) == TOTK_ADPF_CREATED);
    assert(creates == 6 && closes == 4);
    totk_adpf_close(&state, &api);
    state = (totk_adpf_state){0}; api.manager = NULL;
    assert(totk_adpf_sync(&state, &api, tids, 1, 33333333) == TOTK_ADPF_REFUSED);
    assert(!totk_adpf_report(&state, &api, 22000000));
    span_policy();
    rate_hold_policy();
    puts("ADPF policy: durations, lattice, fake API lifecycle, failures and steady-state passed; span target, span vs period, once-only GPU, rate hold");
    return 0;
}

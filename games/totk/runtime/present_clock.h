/* Shared native Android display clock. Snapshot never waits for a callback;
 * presenter and opt-in fixed acquisition waits use the same physical lattice. */
#pragma once
#include <stdint.h>
#include "present_timeline.h"
/* Period multiplication and callback timestamps can round differently. Treat
 * sub-quarter-refresh error as the same physical slot, not a new refresh. */
static inline int runtime_present_slot_reached(uint64_t frame_ns, uint64_t target_ns, uint64_t refresh_ns) {
    return frame_ns >= target_ns || (refresh_ns && target_ns - frame_ns <= refresh_ns / 4);
}
static inline int runtime_present_can_submit(uint64_t frame_ns, uint64_t target_ns,
                                              uint64_t refresh_ns, uint64_t now_ns) {
    return now_ns >= target_ns && runtime_present_slot_reached(frame_ns, target_ns, refresh_ns);
}
/* Auto40 must not use the startup's assumed60Hz period, an old callback with
 * a newly declared period, or a suspended callback stream. Explicit slot1
 * keeps its legacy clock contract. Two refreshes of callback age are allowed. */
static inline int runtime_present_auto_clock_fresh(uint64_t frame_ns, uint64_t refresh_ns,
        uint64_t refresh_frame_ns, uint64_t now_ns) {
    return refresh_ns >= 1000000 && refresh_ns <= 1000000000 &&
        frame_ns > refresh_frame_ns && frame_ns <= now_ns && now_ns <= (uint64_t)INT64_MAX &&
        now_ns - frame_ns <= 2 * refresh_ns;
}
/* Predictor budget: one mandatory picture plus one optional extra. Discrete
 * slack admission uses its own pending < 1+lead gate (lead 0/1/2). */
enum { RUNTIME_PRESENT_SLACK_PENDING = 2 };
static inline int runtime_present_slack_ready(unsigned pending) {
    return pending < RUNTIME_PRESENT_SLACK_PENDING;
}
static inline int runtime_present_credit_update(unsigned *pending, int delta) {
    if (delta == 1) { (*pending)++; return 1; }
    if (delta == -1 && *pending) { (*pending)--; return 1; }
    return 0;
}
/* Exact new scheduler selector: absent/0 is off, 1 requests it, every other
 * spelling is invalid. A refused request must not enable a legacy FG mode. */
static inline int runtime_present_adaptive_slots_request(const char *value) {
    if (!value || (value[0] == '0' && !value[1])) return 0;
    return value[0] == '1' && !value[1] ? 1 : -1;
}
/* Scheduler ONLY: SurfaceFlinger's desired<expected eligibility test is strict
 * (APP.md's present-floor contract). Preserve the exact intended slot in all
 * phase/ownership metadata and encode one ns below it for the transaction.
 * A valid encoding is still strictly after the previous expected-present.
 * Failure is explicit and leaves output untouched; NEVER submit it as zero. */
static inline int runtime_present_adaptive_slot_desired(
        uint64_t expected_ns, uint64_t refresh_ns, uint64_t *desired_ns) {
    if (!desired_ns || refresh_ns<=1 || expected_ns<=refresh_ns ||
            expected_ns>(uint64_t)INT64_MAX) return 0;
    *desired_ns=expected_ns-1;
    return 1;
}
/* The API clock is usable only within its current lifecycle/refresh epoch and
 * two physical refreshes of the callback. Validate copied API values without
 * inventing a deadline, vsync ID or callback-to-SF phase. The owner also checks
 * generation/period against its live clock while holding the snapshot lock. */
static inline int runtime_present_adaptive_slot_timelines_fresh(
        const runtime_present_timelines *snapshot, uint64_t now_ns, uint64_t epoch_ns) {
    const uint64_t limit = (uint64_t)INT64_MAX;
    if (!snapshot || !snapshot->generation || !snapshot->count ||
        snapshot->count > RUNTIME_PRESENT_TIMELINE_CAPACITY ||
        snapshot->preferred >= snapshot->count || !snapshot->refresh_ns ||
        snapshot->refresh_ns > limit / 2 || !snapshot->frame_ns ||
        snapshot->frame_ns < epoch_ns || snapshot->frame_ns > snapshot->sampled_ns ||
        snapshot->sampled_ns > now_ns || now_ns > limit ||
        now_ns - snapshot->frame_ns > 2 * snapshot->refresh_ns) return 0;
    int open = 0;
    for (unsigned i = 0; i < snapshot->count; i++) {
        const runtime_present_timeline_entry *entry = &snapshot->entries[i];
        if (!entry->expected_ns || entry->expected_ns > limit || !entry->deadline_ns ||
            entry->deadline_ns > entry->expected_ns || entry->vsync_id < 0) return 0;
        if (i && (entry->expected_ns <= snapshot->entries[i - 1].expected_ns ||
            entry->deadline_ns <= snapshot->entries[i - 1].deadline_ns)) return 0;
        for (unsigned j = 0; j < i; j++)
            if (entry->vsync_id == snapshot->entries[j].vsync_id) return 0;
        if (i >= snapshot->preferred && entry->deadline_ns > now_ns) open = 1;
    }
    return open;
}
/* Recheck the copied clock at BOTH dispatch and final publication. A refresh
 * or calibration can invalidate an already submitted child without a new real
 * frame/window generation. The snapshot accessor separately proves freshness. */
static inline int runtime_present_adaptive_slot_clock_matches(
        const runtime_present_timelines *snapshot, uint64_t generation,
        uint64_t refresh_ns, int source_display, int window_display) {
    return snapshot && generation && refresh_ns &&
        snapshot->generation==generation && snapshot->refresh_ns==refresh_ns &&
        runtime_present_timeline_display_allowed(source_display,window_display);
}
/* Read-only diagnostics: the effective period every physical-clock mode uses,
 * plus the latest raw callback and present-fence window measurements. A
 * calibrated effective period stays frozen against sub-material drift. */
typedef struct {
    uint64_t effective_ns, nominal_ns, callback_measured_ns, fence_measured_ns;
    unsigned callback_calibrated, fence_calibrated;
} runtime_display_period_observation;
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
int runtime_android_present_clock(uint64_t *frame_ns, uint64_t *refresh_ns);
/* Opt-in API33 admission is independent of snapshot availability. Snapshots are
 * copied by value, never retain callback data, and never wait for a callback. */
int runtime_android_present_timeline_active(void);
int runtime_android_present_timelines(runtime_present_timelines *out);
/* Native getInstance() observes the default internal display (logical ID0).
 * Unknown/unadmitted bindings return -1, never permission to tag other displays. */
int runtime_android_present_timeline_display_id(void);
/* Independent default-off adaptive+QCOM clock admission. requested is exactly
 * TOTK_FG_ADAPTIVE_SLOTS=1, even when API/capability admission fails. active does
 * not imply a current sample or backend/device qualification. A missing/stale
 * sample means REAL-only, never locked30 acquisition or legacy adaptive FG.
 * Snapshot success copies values; failure leaves out untouched and never waits.
 * Source display is 0 only when admitted; the caller must match its actual
 * window display and validate deadlines again before a platform submission. */
int runtime_android_adaptive_slots_requested(void);
int runtime_android_adaptive_slots_active(void);
int runtime_android_adaptive_slots_display_id(void);
int runtime_android_adaptive_slot_timelines(runtime_present_timelines *out);
/* Exact effective period, available to every active physical-clock mode;
 * unlike display_hz it is not rounded to an integer rate. */
uint64_t runtime_android_display_period(unsigned interval);
/* Copies the values above under the display mutex; never waits for a callback. */
int runtime_android_display_period_observe(runtime_display_period_observation *out);
/* Current-transaction AHB present-fence timestamp; zero/missing is not a sample. */
void runtime_android_display_presented(uint64_t display_ns);
void runtime_android_present_wait(uint64_t target_ns);
void runtime_android_present_credit(int delta);
int runtime_android_acquire_slack_wait(unsigned interval, unsigned lead);
int runtime_android_acquire_clock_active(void);
uint64_t runtime_android_acquire_period(unsigned interval);
int runtime_android_acquire_wait(unsigned interval);
#else
static inline int runtime_android_present_clock(uint64_t *frame_ns, uint64_t *refresh_ns) {
    (void)frame_ns; (void)refresh_ns; return 0;
}
static inline int runtime_android_present_timeline_active(void) { return 0; }
static inline int runtime_android_present_timelines(runtime_present_timelines *out) { (void)out; return 0; }
static inline int runtime_android_present_timeline_display_id(void) { return -1; }
static inline int runtime_android_adaptive_slots_requested(void) { return 0; }
static inline int runtime_android_adaptive_slots_active(void) { return 0; }
static inline int runtime_android_adaptive_slots_display_id(void) { return -1; }
static inline int runtime_android_adaptive_slot_timelines(runtime_present_timelines *out) { (void)out; return 0; }
static inline uint64_t runtime_android_display_period(unsigned interval) { (void)interval; return 0; }
static inline int runtime_android_display_period_observe(runtime_display_period_observation *out) { (void)out; return 0; }
static inline void runtime_android_display_presented(uint64_t display_ns) { (void)display_ns; }
static inline void runtime_android_present_wait(uint64_t target_ns) { (void)target_ns; }
static inline void runtime_android_present_credit(int delta) { (void)delta; }
static inline int runtime_android_acquire_slack_wait(unsigned interval, unsigned lead) { (void)interval; (void)lead; return 0; }
static inline int runtime_android_acquire_clock_active(void) { return 0; }
static inline uint64_t runtime_android_acquire_period(unsigned interval) { (void)interval; return 0; }
static inline int runtime_android_acquire_wait(unsigned interval) { (void)interval; return 0; }
#endif

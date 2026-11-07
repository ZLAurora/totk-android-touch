/* Pure 1 Hz thermal policy; Android status values: NONE=0, MODERATE=2, SEVERE=3. */
#pragma once
#include <math.h>
#include <stdint.h>
#define TOTK_THERMAL_POLL_NS UINT64_C(1000000000)
#define TOTK_THERMAL_RECOVER_NS UINT64_C(30000000000)
typedef struct {
    uint64_t poll_ns, sample_ns, cool_ns;
    int polled, sampled, cooling, capped;
} totk_thermal_policy;
/* Reserve BEFORE calling headroom, including failed/NaN calls. Reversed clocks
 * do not permit a second call before the previous deadline. */
static inline int totk_thermal_poll(totk_thermal_policy *s, uint64_t now) {
    if (s->polled && (now < s->poll_ns || now - s->poll_ns < TOTK_THERMAL_POLL_NS)) return 0;
    s->polled = 1; s->poll_ns = now; return 1;
}
/* Each sample is consumed once. Missing/invalid forecasts cannot prove cool
 * recovery; SEVERE still caps without a forecast (API30/status-only devices).
 * A sampling gap >2 s breaks continuous cool evidence. Disabling releases only
 * our floor, never forces the game's current interval back to one. */
static inline int totk_thermal_step(totk_thermal_policy *s, int enabled,
        unsigned interval, uint64_t sample, int status, float headroom) {
    int before = s->capped;
    if (!enabled) { s->capped = s->cooling = 0; return before != s->capped; }
    if (s->sampled && sample <= s->sample_ns) return 0;
    if (s->sampled && sample - s->sample_ns > 2 * TOTK_THERMAL_POLL_NS) s->cooling = 0;
    s->sampled = 1; s->sample_ns = sample;
    int valid_status = status >= 0 && status <= 6;
    int valid_headroom = isfinite(headroom) && headroom >= 0;
    if (!s->capped) {
        s->cooling = 0;
        if (interval == 1 && ((valid_status && status >= 3) || (valid_headroom && headroom >= 1.0f)))
            s->capped = 1;
    } else if (valid_status && status < 2 && valid_headroom && headroom < .8f) {
        if (!s->cooling) { s->cooling = 1; s->cool_ns = sample; }
        if (sample - s->cool_ns >= TOTK_THERMAL_RECOVER_NS) s->capped = s->cooling = 0;
    } else s->cooling = 0;
    return before != s->capped;
}

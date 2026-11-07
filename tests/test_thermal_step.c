#include "../games/totk/runtime/thermal_step.h"
#include "../games/totk/runtime/live_ctl.h"
#include <assert.h>
int main(void) {
    const uint64_t second = TOTK_THERMAL_POLL_NS;
    totk_thermal_policy s = {0};
    assert(totk_thermal_poll(&s, 0));
    assert(!totk_thermal_poll(&s, second - 1));
    assert(totk_thermal_poll(&s, second));
    assert(!totk_thermal_poll(&s, 0));
    assert(!totk_thermal_poll(&s, second));
    assert(totk_thermal_poll(&s, 4 * second));
    assert(!totk_thermal_poll(&s, 4 * second + 1)); /* no catch-up burst */
    s = (totk_thermal_policy){0};
    assert(!totk_thermal_step(&s, 1, 1, 0, 2, .99f));
    assert(totk_thermal_step(&s, 1, 1, second, 3, NAN) && s.capped);
    for (unsigned i = 2; i <= 31; i++)
        assert(!totk_thermal_step(&s, 1, 2, i * second, 1, .79f));
    assert(s.capped);
    assert(totk_thermal_step(&s, 1, 2, 32 * second, 1, .79f) && !s.capped);
    /* Forecast alone can cap. NaN/unknown/moderate/exact .8 break recovery. */
    assert(totk_thermal_step(&s, 1, 1, 33 * second, -1, 1.f) && s.capped);
    assert(!totk_thermal_step(&s, 1, 2, 34 * second, 0, .7f) && s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 35 * second, 0, NAN) && !s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 36 * second, 0, .7f) && s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 37 * second, -1, .7f) && !s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 38 * second, 0, .7f) && s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 39 * second, 2, .7f) && !s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 40 * second, 0, .7f) && s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 41 * second, 1, .8f) && !s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 42 * second, 0, .7f) && s.cooling);
    assert(!totk_thermal_step(&s, 1, 2, 80 * second, 0, .7f) && s.capped);
    assert(s.cool_ns == 80 * second); /* no recovery across missing samples */
    assert(!totk_thermal_step(&s, 1, 2, 80 * second, 0, .7f));
    assert(!totk_thermal_step(&s, 1, 2, 79 * second, 0, .7f));
    assert(s.cool_ns == 80 * second); /* repeated/reversed samples do not train */
    assert(totk_thermal_step(&s, 0, 2, 80 * second, 3, 1.f) && !s.capped);
    assert(!totk_thermal_step(&s, 1, 2, 81 * second, 3, 1.f)); /* already 30 */
    assert(!totk_thermal_step(&s, 1, 1, 82 * second, -1, NAN));
    assert(!totk_thermal_step(&s, 1, 1, 83 * second, 0, INFINITY));
    assert(!totk_thermal_step(&s, 1, 1, 84 * second, 0, -1.f));
    assert(totk_thermal_step(&s, 1, 1, 85 * second, 0, 1.f));
    totk_live_op op; char why[240];
    assert(totk_live_parse("set TOTK_THERMAL_STEP=1", &op, why, sizeof why) == -1);
    assert(strstr(why, "startup-only"));
    assert(totk_live_parse("unset TOTK_THERMAL_STEP", &op, why, sizeof why) == -1);
    return 0;
}

/* Standalone host test: timestamps in/deadlines out; no GPU, sleeping or Android. */
#include "vk_present_slots.h"
#include "framegen_slots.h"
#include "vk_present_fence.h"
#include "../../runtime/present_clock.h"
#include "../../runtime/present_timeline.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t base = 1000000000ull, refresh60 = 16666667;
static void eligibility_floor(void) {
    assert(!vk_present_floor_config(NULL));
    const char *off[]={"", "0", "2", "-1", "1x", " 1", "01", "1000"};
    for (unsigned i=0;i<sizeof off/sizeof *off;i++) assert(!vk_present_floor_config(off[i]));
    assert(vk_present_floor_config("1")==1);
    assert(!vk_present_guard_config(NULL));
    const char *guard_off[]={"", "0", "1", "-125000", "125001", "125000x", " 125000", "0125000"};
    for (unsigned i=0;i<sizeof guard_off/sizeof *guard_off;i++) assert(!vk_present_guard_config(guard_off[i]));
    assert(vk_present_guard_config("125000")==125000);
    assert(vk_present_eligible_time(base,0,125000,refresh60)==base-125000);
    assert(vk_present_eligible_time(base,1,125000,refresh60)==base-125001);
    assert(vk_present_eligible_time(base,1,125001,refresh60)==base-1);
    assert(vk_present_eligible_time(base,1,125000,0)==base-1);
    assert(vk_present_eligible_time(base,1,125000,250002)==base-1);
    assert(vk_present_eligible_time(base,1,125000,250004)==base-125001);
    assert(vk_present_eligible_time(125001,1,125000,refresh60)==125001);
    assert(vk_present_eligible_time(INT64_MAX,1,125000,refresh60)==INT64_MAX-125001);
    assert(vk_present_eligible_time(UINT64_MAX,1,125000,refresh60)==UINT64_MAX);
    const uint64_t targets[]={0,1,2,base,INT64_MAX,UINT64_MAX};
    for (unsigned i=0;i<sizeof targets/sizeof *targets;i++) {
        uint64_t target=targets[i];
        assert(vk_present_eligible_time(target, 0, 0, refresh60)==target);
        assert(vk_present_eligible_time(target, 2, 0, refresh60)==target);
        assert(vk_present_eligible_time(target, 1, 0, refresh60)==
            (target>1 && target<=INT64_MAX ? target-1:target));
    }
    for (unsigned slot=1;slot<=600;slot++) {
        uint64_t target=base+slot*refresh60;
        uint64_t eligible=vk_present_eligible_time(target, 1, 0, refresh60);
        /* SF rejects exact equality; eligibility moves strictly below, not into the prior refresh. */
        assert(eligible<target && eligible>target-refresh60);
        assert(eligible==vk_present_eligible_time(target-refresh60, 1, 0, refresh60)+refresh60);
    }
    runtime_present_timeline_entry entry={.vsync_id=0,.expected_ns=base,.deadline_ns=base-1000000};
    assert(vk_present_eligible_time(entry.expected_ns, 1, 0, refresh60)==base-1);
    assert(entry.vsync_id==0 && entry.expected_ns==base && entry.deadline_ns==base-1000000);
}
static vk_present_slot reserve(vk_present_slots *p, uint64_t callback, uint64_t refresh,
        uint32_t interval, uint64_t now) {
    vk_present_slot r = vk_present_slots_reserve(p, 1, callback, refresh, interval, now);
    assert(r.mode == VK_PRESENT_SLOTS_ACTIVE);
    assert(r.refresh_ns == refresh && r.interval == interval);
    assert(r.copy_open_ns == r.desired_ns);
    assert(p->desired_ns == r.desired_ns && p->refresh_ns == refresh);
    return r;
}
static void cadence(uint32_t interval) {
    vk_present_slots p = {0};
    const uint64_t jitter[] = {1000000, 8000000, 2000000, 14000000, 500000, 4000000};
    for (unsigned frame = 0; frame < 600; frame++) {
        uint64_t callback = base + (uint64_t)frame * interval * refresh60;
        vk_present_slot r = reserve(&p, callback, refresh60, interval,
            callback + jitter[frame % (sizeof jitter / sizeof *jitter)]);
        assert(r.desired_ns == base + (uint64_t)(frame + 1) * interval * refresh60);
        assert(!r.skipped); /* arrival jitter does not become display jitter */
    }
}
static void late_short(void) {
    vk_present_slots p = {0};
    reserve(&p, base, refresh60, 2, base + 1000000);
    /* A late frame retains slot 4; its short successor can still make slot 6. */
    vk_present_slot late = reserve(&p, base + 5 * refresh60, refresh60, 2,
        base + 5 * refresh60 + 1000000);
    assert(late.desired_ns == base + 4 * refresh60 && !late.skipped);
    vk_present_slot short_frame = reserve(&p, base + 5 * refresh60, refresh60, 2,
        base + 5 * refresh60 + 2000000);
    assert(short_frame.desired_ns == base + 6 * refresh60 && !short_frame.skipped);
    /* Exactly one game-period late is allowed; one nanosecond more skips to
     * the first nonpast slot instead of repeatedly submitting stale targets. */
    p = (vk_present_slots){0};
    reserve(&p, base, refresh60, 2, base);
    late = reserve(&p, base + 6 * refresh60, refresh60, 2, base + 6 * refresh60);
    assert(late.desired_ns == base + 4 * refresh60 && !late.skipped);
    p = (vk_present_slots){0};
    reserve(&p, base, refresh60, 2, base);
    late = reserve(&p, base + 6 * refresh60, refresh60, 2, base + 6 * refresh60 + 1);
    assert(late.desired_ns == base + 8 * refresh60 && late.skipped == 2);
}
static void long_stall(void) {
    vk_present_slots p = {0};
    reserve(&p, base, refresh60, 2, base);
    vk_present_slot r = reserve(&p, base + 1001 * refresh60, refresh60, 2,
        base + 1001 * refresh60 + 5000000);
    assert(r.desired_ns == base + 1002 * refresh60 && r.skipped == 499);
    r = reserve(&p, base + 1003 * refresh60, refresh60, 2, base + 1003 * refresh60 + 1);
    assert(r.desired_ns == base + 1004 * refresh60 && !r.skipped);
    uint64_t now = base + 7ull * 24 * 60 * 60 * 1000000000;
    r = reserve(&p, now - (now - base) % refresh60, refresh60, 2, now);
    assert(r.desired_ns >= now && r.desired_ns - now < 2 * refresh60);
    assert((r.desired_ns - base) % (2 * refresh60) == 0 && r.skipped > 1000000);
    uint64_t previous = r.desired_ns;
    r = reserve(&p, now - (now - base) % refresh60, refresh60, 2, now + 1);
    assert(r.desired_ns == previous + 2 * refresh60 && !r.skipped);
}
static void mode_changes(void) {
    vk_present_slots p = {0};
    uint64_t ms = 1000000;
    vk_present_slot r = reserve(&p, base, 20 * ms, 2, base);
    assert(r.desired_ns == base + 40 * ms);
    r = reserve(&p, base + 20 * ms, 20 * ms, 1, base + 21 * ms);
    assert(r.desired_ns == base + 60 * ms); /* interval 2 -> 1 */
    r = reserve(&p, base + 55 * ms, 10 * ms, 1, base + 56 * ms);
    assert(r.desired_ns == base + 75 * ms); /* new lattice, no future regression */
    r = reserve(&p, base + 65 * ms, 10 * ms, 1, base + 66 * ms);
    assert(r.desired_ns == base + 85 * ms);
    r = reserve(&p, base + 75 * ms, 10 * ms, 3, base + 76 * ms);
    assert(r.desired_ns == base + 115 * ms); /* interval 1 -> 3 */
    r = reserve(&p, base + 100 * ms, 25 * ms, 3, base + 101 * ms);
    assert(r.desired_ns == base + 200 * ms); /* slower refresh, same tick count */
    r = reserve(&p, base + 1000 * ms, 25 * ms, 1, base + 1001 * ms);
    assert(r.desired_ns == base + 1025 * ms && r.skipped == 32);
    /* Refresh change after a suspend starts from the current physical lattice,
     * not the long-obsolete old target; it needs no catch-up sequence. */
    r = reserve(&p, base + 10000 * ms, 10 * ms, 2, base + 10001 * ms);
    assert(r.desired_ns == base + 10020 * ms && !r.skipped);
}
static void fractional_refresh(void) {
    vk_present_slots p = {0};
    for (unsigned frame = 0; frame < 600; frame++) {
        uint64_t callback = base + (uint64_t)(2 * frame) * 1000000000 / 60;
        vk_present_slot r = reserve(&p, callback, refresh60, 2, callback + 1000000);
        uint64_t open_callback = base + (uint64_t)(2 * frame + 2) * 1000000000 / 60;
        assert(runtime_present_slot_reached(open_callback, r.copy_open_ns, refresh60));
        assert(runtime_present_slot_reached(open_callback - 1, r.copy_open_ns, refresh60));
        assert(runtime_present_slot_reached(open_callback + 1, r.copy_open_ns, refresh60));
        assert(!runtime_present_slot_reached(open_callback - refresh60, r.copy_open_ns, refresh60));
        assert(!runtime_present_can_submit(open_callback, r.copy_open_ns, refresh60, r.desired_ns - 1));
        assert(runtime_present_can_submit(open_callback, r.copy_open_ns, refresh60, r.desired_ns));
    }
    assert(runtime_present_slot_reached(base - 1, base, refresh60));
    assert(runtime_present_slot_reached(base + 1, base, refresh60));
    assert(!runtime_present_slot_reached(base - refresh60, base, refresh60));
}
static void disabled_unavailable(void) {
    vk_present_slots p = {0};
    vk_present_slot r = vk_present_slots_reserve(&p, 0, base, refresh60, 2, base + 1);
    assert(r.mode == VK_PRESENT_SLOTS_OFF && r.desired_ns == base + 1);
    assert(!r.copy_open_ns && !r.refresh_ns && !r.interval && !p.desired_ns);
    r = vk_present_slots_reserve(&p, 1, 0, 0, 2, base + 2);
    assert(r.mode == VK_PRESENT_SLOTS_NO_CLOCK && r.desired_ns == base + 2);
    assert(!r.copy_open_ns && !r.refresh_ns && !r.interval && !p.desired_ns);
    reserve(&p, base, refresh60, 2, base + 3);
    uint64_t previous = p.desired_ns;
    r = vk_present_slots_reserve(&p, 1, base, 0, 2, base + 4);
    assert(r.mode == VK_PRESENT_SLOTS_NO_CLOCK && p.desired_ns == previous);
    r = vk_present_slots_reserve(&p, 0, base, refresh60, 2, base + 5);
    assert(r.mode == VK_PRESENT_SLOTS_OFF && p.desired_ns == previous);
    r = reserve(&p, base + 2 * refresh60, refresh60, 2, base + 2 * refresh60);
    assert(r.desired_ns == previous + 2 * refresh60);
#if !defined(__ANDROID__) || !defined(TOTK_ANDROID_APP)
    uint64_t callback = 0, refresh = 0;
    assert(!runtime_android_present_clock(&callback, &refresh)); /* enabled host is unavailable, not disabled */
    runtime_android_present_wait(UINT64_MAX); /* host stub cannot sleep */
#endif
}
static void auto40_policy(void) {
    const uint64_t refresh120 = 8333333, producer = 25000000;
    assert(vk_present_slots_config("1") == VK_PRESENT_SLOTS_CONFIG_FIXED);
    assert(vk_present_slots_config("auto") == VK_PRESENT_SLOTS_CONFIG_AUTO40);
    const char *off[] = {NULL, "", "0", "01", "AUTO", "auto40", " auto", "auto "};
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++)
        assert(vk_present_slots_config(off[i]) == VK_PRESENT_SLOTS_CONFIG_OFF);
    vk_present_slots p = {0};
    vk_present_slot r = vk_present_slots_auto40_reserve(&p, 0.025, 0, 0, base);
    assert(r.mode == VK_PRESENT_SLOTS_NO_CLOCK && r.desired_ns == base);
    assert(!r.interval && !r.refresh_ns && !r.copy_open_ns);
    for (unsigned i = 0; i < 600; i++) {
        uint64_t now = base + (uint64_t)(i + 1) * producer;
        uint64_t callback = base + (now - base) / refresh60 * refresh60;
        r = vk_present_slots_auto40_reserve(&p, 0.025, callback, refresh60, now);
        assert(r.mode == VK_PRESENT_SLOTS_OFF && r.desired_ns == now);
        assert(!r.interval && !r.refresh_ns && !r.copy_open_ns && !p.refresh_ns);
    }
    p = (vk_present_slots){0};
    for (unsigned i = 0; i < 600; i++) {
        uint64_t callback = base + (uint64_t)i * 3 * refresh120;
        r = vk_present_slots_auto40_reserve(&p, 0.025, callback, refresh120,
            callback + (i % 3 + 1) * 1000000);
        assert(r.mode == VK_PRESENT_SLOTS_ACTIVE && r.interval == 3);
        assert(r.refresh_ns == refresh120 && !r.skipped);
        assert(r.desired_ns == base + (uint64_t)(i + 1) * 3 * refresh120);
        assert(r.copy_open_ns == r.desired_ns);
    }
    const double other_periods[] = {0, -1, 1.0 / 60, 0.025001, 0.024999};
    for (unsigned i = 0; i < sizeof other_periods / sizeof *other_periods; i++) {
        p = (vk_present_slots){0};
        r = vk_present_slots_auto40_reserve(&p, other_periods[i], base, refresh120, base);
        assert(r.mode == VK_PRESENT_SLOTS_OFF && !r.interval && r.desired_ns == base);
    }
    const uint64_t other_refreshes[] = {refresh60, 11111111, 6944444, 7999999, 8650001};
    for (unsigned i = 0; i < sizeof other_refreshes / sizeof *other_refreshes; i++) {
        p = (vk_present_slots){0};
        r = vk_present_slots_auto40_reserve(&p, 0.025, base, other_refreshes[i], base);
        assert(r.mode == VK_PRESENT_SLOTS_OFF && !r.interval && r.desired_ns == base);
    }
    p = (vk_present_slots){0};
    r = vk_present_slots_auto40_reserve(&p, 0.025, base, 8341675, base);
    assert(r.mode == VK_PRESENT_SLOTS_ACTIVE && r.interval == 3); /*119.88Hz*/
    const uint64_t cutscene_refreshes[] = {refresh60, refresh120};
    for (unsigned i = 0; i < 2; i++) {
        uint64_t refresh = cutscene_refreshes[i], previous = 0;
        p = (vk_present_slots){0};
        for (unsigned frame = 0; frame < 600; frame++) {
            unsigned interval = i ? 4 : 2;
            uint64_t callback = base + (uint64_t)frame * interval * refresh;
            r = vk_present_slots_auto40_reserve(&p, (1000000000ull / 30) / 1e9, callback, refresh, callback + 1);
            assert(r.mode == VK_PRESENT_SLOTS_ACTIVE && r.interval == interval);
            assert(r.desired_ns == callback + interval * refresh);
            if (previous) assert(r.desired_ns - previous == interval * refresh);
            previous = r.desired_ns;
        }
        r = vk_present_slots_auto40_reserve(&p, 0.025, previous, refresh, previous + 1);
        assert(r.interval == (i ? 3 : 0) && r.desired_ns > previous);
    }
}
static void auto40_transitions(void) {
    const uint64_t refresh120 = 8333333;
    vk_present_slots p = {0};
    vk_present_slot queued = vk_present_slots_auto40_reserve(&p, 0.025, base, refresh120, base);
    vk_present_slot saved = queued;
    vk_present_slot fallback = vk_present_slots_auto40_reserve(&p, 0.025,
        base + refresh60, refresh60, base + refresh60);
    assert(fallback.mode == VK_PRESENT_SLOTS_OFF && fallback.desired_ns == queued.desired_ns + 1);
    assert(!fallback.refresh_ns && !fallback.copy_open_ns && !fallback.interval && !p.refresh_ns);
    vk_present_slot unavailable = vk_present_slots_auto40_reserve(&p, 0.025, 0, 0, base + refresh60 + 1);
    assert(unavailable.mode == VK_PRESENT_SLOTS_NO_CLOCK);
    assert(unavailable.desired_ns == fallback.desired_ns + 1 && !p.refresh_ns);
    uint64_t callback = base + 10 * refresh120;
    vk_present_slot resumed = vk_present_slots_auto40_reserve(&p, 0.025,
        callback, refresh120, callback + 1);
    assert(resumed.mode == VK_PRESENT_SLOTS_ACTIVE && resumed.desired_ns == callback + 3 * refresh120);
    assert(resumed.desired_ns > unavailable.desired_ns);
    assert(!memcmp(&queued, &saved, sizeof queued)); /* Owned targets are immutable. */
    /* Calibrated period changes retain ordering on the new physical lattice. */
    vk_present_slot calibrated = vk_present_slots_auto40_reserve(&p, 0.025,
        callback + refresh120, 8341675, callback + refresh120 + 1);
    assert(calibrated.desired_ns >= resumed.desired_ns + 3 * UINT64_C(8341675));
    assert(calibrated.interval == 3 && calibrated.refresh_ns == 8341675);
    assert(runtime_present_auto_clock_fresh(base, refresh120, 0, base));
    assert(runtime_present_auto_clock_fresh(base, refresh120, 0, base + 2 * refresh120));
    assert(!runtime_present_auto_clock_fresh(base, refresh120, 0, base + 2 * refresh120 + 1));
    assert(!runtime_present_auto_clock_fresh(base, refresh120, base, base));
    assert(!runtime_present_auto_clock_fresh(base, refresh120, base + 1, base));
    assert(!runtime_present_auto_clock_fresh(base, refresh120, 0, base - 1));
    assert(!runtime_present_auto_clock_fresh(0, refresh120, 0, base));
    assert(!runtime_present_auto_clock_fresh(base, 0, 0, base));
    assert(!runtime_present_auto_clock_fresh(base, UINT64_MAX, 0, base));
}
static void interpolation_order(void) {
    vk_present_slots slots = {0};
    vk_present_interp interp = {.retained = -1};
    uint64_t last_real = 0, last_midpoint = 0;
    for (unsigned frame = 0; frame < 600; frame++) {
        uint64_t callback = base + (uint64_t)frame * 2 * refresh60;
        vk_present_slot slot = reserve(&slots, callback, refresh60, 2, callback + 1000000);
        vk_present_interp_pair pair = vk_present_interp_push(&interp,(int)(frame % 4),
            frame + 1,7,slot,callback + 1000000);
        assert(interp.retained == (int)(frame % 4) && interp.frame == frame + 1);
        assert(slots.desired_ns == slot.desired_ns); /* no synthetic lattice advancement */
        if (!frame) {
            assert(pair.real == -1 && !pair.midpoint_ns); /* first real is retained, not duplicated */
            assert(pair.reason == VK_INTERP_WARMUP);
            continue;
        }
        assert(pair.real == (int)((frame - 1) % 4));
        assert(pair.reason == VK_INTERP_READY);
        assert(pair.slot.desired_ns == base + (uint64_t)(frame + 1) * 2 * refresh60);
        assert(pair.slot.copy_open_ns == pair.slot.desired_ns && pair.slot.interval == 2);
        assert(pair.midpoint_ns == pair.slot.desired_ns + refresh60);
        if (last_real) {
            assert(pair.slot.desired_ns == last_real + 2 * refresh60);
            assert(pair.slot.desired_ns == last_midpoint + refresh60);
        }
        /* Display order is real N, midpoint(N,N+1), then real N+1.
           Omitting any midpoint does not alter the next real reservation. */
        last_real = pair.slot.desired_ns; last_midpoint = pair.midpoint_ns;
    }
}
static void interpolation_late(void) {
    vk_present_slots slots = {0};
    vk_present_interp interp = {.retained = -1};
    vk_present_slot first = reserve(&slots,base,refresh60,2,base);
    vk_present_interp_pair pair = vk_present_interp_push(&interp,0,1,1,first,base);
    assert(pair.real == -1 && !pair.midpoint_ns);
    assert(pair.reason == VK_INTERP_WARMUP);
    vk_present_slot second = reserve(&slots,base + 2 * refresh60,refresh60,2,base + 2 * refresh60);
    pair = vk_present_interp_push(&interp,1,2,1,second,second.desired_ns + 1);
    assert(pair.real == 0 && pair.slot.desired_ns == second.desired_ns);
    assert(pair.midpoint_ns == second.desired_ns + refresh60 && pair.reason == VK_INTERP_READY);
    /* Odin: reserved-late real N still has almost a refresh for its child.
       The following real keeps the unchanged one-tick-delayed lattice. */
    vk_present_slot third = reserve(&slots,base + 4 * refresh60,refresh60,2,base + 4 * refresh60);
    pair = vk_present_interp_push(&interp,2,3,1,third,third.desired_ns);
    assert(pair.real == 1 && pair.slot.desired_ns == second.desired_ns + 2 * refresh60);
    assert(pair.midpoint_ns == third.desired_ns + refresh60);
    assert(pair.reason == VK_INTERP_READY);
    assert(pair.midpoint_ns > second.desired_ns + refresh60);
    /* Extrapolation retains its old punctual-parent contract. Interpolation
       uses the child's own vsync instead (exercised separately below). */
    assert(framegen_child_ready(third.desired_ns,third.desired_ns,pair.midpoint_ns,pair.midpoint_ns - 1));
    assert(!framegen_child_ready(third.desired_ns + 1,third.desired_ns,pair.midpoint_ns,pair.midpoint_ns - 1));
    assert(!framegen_child_ready(third.desired_ns,third.desired_ns,pair.midpoint_ns,pair.midpoint_ns));
    assert(!framegen_child_ready(0,third.desired_ns,pair.midpoint_ns,pair.midpoint_ns - 1));
    /* A long stall skips on the real30 lattice, then releases the retained
       real once; there is no queue of obsolete midpoint catch-up work. */
    vk_present_slot stalled = reserve(&slots,base + 1001 * refresh60,refresh60,2,base + 1001 * refresh60 + 1);
    pair = vk_present_interp_push(&interp,3,4,1,stalled,stalled.desired_ns);
    assert(pair.real == 2 && pair.slot.skipped && pair.slot.desired_ns == stalled.desired_ns);
    assert(pair.midpoint_ns == stalled.desired_ns + refresh60 && interp.retained == 3);
}
static void interpolation_buffered(void) {
    const uint64_t refresh = 16697278; /* 59.89 Hz physical display */
    vk_present_slots slots = {0};
    vk_present_interp interp = {.retained = -1};
    uint64_t previous_midpoint = 0;
    unsigned ready = 0;
    for (unsigned frame = 0; frame < 300; frame++) {
        uint64_t callback = base + (uint64_t)frame * 2 * refresh +
            (uint64_t)(frame % 3) * 700000;
        vk_present_slot real = reserve(&slots,callback,refresh,2,callback + 1000000);
        uint64_t source_ready = real.desired_ns + 8000000;
        vk_present_interp_pair pair = vk_present_interp_push(&interp,frame%7,frame+1,9,
            real,source_ready);
        vk_present_interp_pair one=vk_present_interp_buffer(pair,1,source_ready);
        pair = vk_present_interp_buffer(pair,2,source_ready);
        assert(one.slot.desired_ns+refresh == pair.slot.desired_ns);
        assert(source_ready < one.slot.desired_ns);
        if (frame) assert(one.reason == VK_INTERP_READY && one.midpoint_ns == pair.slot.desired_ns);
        else assert(one.reason == VK_INTERP_WARMUP && !one.midpoint_ns);
        if (!frame) {
            assert(pair.reason == VK_INTERP_WARMUP && !pair.midpoint_ns);
            assert(pair.slot.desired_ns == real.desired_ns + 2 * refresh);
            continue;
        }
        assert(pair.reason == VK_INTERP_READY && pair.midpoint_ns);
        assert(pair.slot.desired_ns == real.desired_ns + 2 * refresh);
        assert(pair.slot.copy_open_ns == pair.slot.desired_ns);
        assert(pair.midpoint_ns == pair.slot.desired_ns + refresh);
        if (previous_midpoint) assert(pair.slot.desired_ns == previous_midpoint + refresh);
        assert(source_ready < pair.slot.desired_ns); /* GPU ready ~8ms after unbuffered target */
        previous_midpoint = pair.midpoint_ns;
        ready++;
    }
    assert(ready == 299);
    assert(slots.desired_ns == base + (uint64_t)300 * 2 * refresh);

    /* A child late on the old deadline becomes viable while the fixed lead
       remains; callback arrival/jitter cannot change its buffered phase. */
    vk_present_slots p = {0};
    vk_present_interp i = {.retained = -1};
    vk_present_slot first = reserve(&p,base,refresh,2,base);
    vk_present_interp_pair pair = vk_present_interp_push(&i,0,1,1,first,base);
    pair = vk_present_interp_buffer(pair,2,base);
    vk_present_slot next = reserve(&p,base+2*refresh,refresh,2,base+2*refresh);
    uint64_t old_deadline = next.desired_ns + refresh;
    pair = vk_present_interp_push(&i,1,2,1,next,old_deadline);
    assert(pair.reason == VK_INTERP_LATE && !pair.midpoint_ns);
    pair = vk_present_interp_buffer(pair,2,old_deadline);
    assert(pair.reason == VK_INTERP_READY && pair.midpoint_ns > old_deadline);
    assert(pair.slot.desired_ns == next.desired_ns + 2*refresh);
    vk_present_slot third = reserve(&p,base+4*refresh,refresh,2,base+4*refresh);
    pair = vk_present_interp_push(&i,2,3,1,third,third.desired_ns);
    uint64_t delayed_deadline = third.desired_ns + 3*refresh;
    pair = vk_present_interp_buffer(pair,2,delayed_deadline);
    assert(pair.reason == VK_INTERP_LATE && !pair.midpoint_ns);
    assert(pair.slot.desired_ns == third.desired_ns + 2*refresh);

    /* Non-pair failures remain failures; valid real reservations still retain
       the buffered cadence, but no synthetic midpoint is manufactured. */
    pair.reason = VK_INTERP_ADJACENCY;
    pair.midpoint_ns = 0;
    pair.slot = third;
    pair = vk_present_interp_buffer(pair,2,base);
    assert(pair.reason == VK_INTERP_ADJACENCY && !pair.midpoint_ns);
    assert(pair.slot.desired_ns == third.desired_ns + 2*refresh);

    pair = (vk_present_interp_pair){.real=0,.reason=VK_INTERP_READY,
        .slot={.desired_ns=(uint64_t)INT64_MAX-3*refresh,
            .copy_open_ns=(uint64_t)INT64_MAX-3*refresh,.refresh_ns=refresh,
            .interval=2,.mode=VK_PRESENT_SLOTS_ACTIVE}};
    pair = vk_present_interp_buffer(pair,2,base);
    assert(pair.reason == VK_INTERP_READY && pair.midpoint_ns == (uint64_t)INT64_MAX);
    assert(pair.slot.desired_ns == (uint64_t)INT64_MAX-refresh);
    pair = (vk_present_interp_pair){.real=0,.reason=VK_INTERP_READY,
        .slot={.desired_ns=(uint64_t)INT64_MAX-3*refresh+1,
            .copy_open_ns=(uint64_t)INT64_MAX-3*refresh+1,.refresh_ns=refresh,
            .interval=2,.mode=VK_PRESENT_SLOTS_ACTIVE}};
    pair = vk_present_interp_buffer(pair,2,base);
    assert(pair.reason == VK_INTERP_SLOT && !pair.midpoint_ns);
    assert(pair.slot.desired_ns <= (uint64_t)INT64_MAX);
    pair = (vk_present_interp_pair){.real=0,.reason=VK_INTERP_READY,
        .slot={.desired_ns=(uint64_t)INT64_MAX-refresh,
            .copy_open_ns=(uint64_t)INT64_MAX-refresh,.refresh_ns=refresh,
            .interval=2,.mode=VK_PRESENT_SLOTS_ACTIVE}};
    pair = vk_present_interp_buffer(pair,1,base);
    assert(pair.reason == VK_INTERP_SLOT && !pair.midpoint_ns); /* midpoint would exceed int64 */
}
static void interpolation_boundaries(void) {
    vk_present_slot slot = {.desired_ns = base, .copy_open_ns = base,
        .refresh_ns = refresh60, .interval = 2, .mode = VK_PRESENT_SLOTS_ACTIVE};
    vk_present_interp interp = {.retained = -1};
    vk_present_interp_push(&interp,0,1,1,slot,base);
    slot.desired_ns = slot.copy_open_ns += 2 * refresh60;
    vk_present_interp_pair pair = vk_present_interp_push(&interp,1,2,2,slot,base);
    assert(pair.real == 0 && !pair.midpoint_ns); /* old surface retires, never synthesizes */
    assert(pair.reason == VK_INTERP_ADJACENCY);
    slot.desired_ns = slot.copy_open_ns += 2 * refresh60;
    pair = vk_present_interp_push(&interp,2,4,2,slot,base);
    assert(pair.real == 1 && !pair.midpoint_ns); /* dropped real breaks adjacency */
    assert(pair.reason == VK_INTERP_ADJACENCY);
    slot.desired_ns = slot.copy_open_ns += 2 * refresh60;
    pair = vk_present_interp_push(&interp,3,5,2,slot,base);
    assert(pair.real == 2 && pair.midpoint_ns == slot.desired_ns + refresh60);
    assert(pair.reason == VK_INTERP_READY);
    slot.refresh_ns = 16666666; slot.desired_ns = slot.copy_open_ns += 2 * slot.refresh_ns;
    pair = vk_present_interp_push(&interp,0,6,2,slot,base);
    assert(pair.real == 3 && !pair.midpoint_ns); /* refresh transition has no cross-clock child */
    assert(pair.reason == VK_INTERP_REFRESH);
    slot.interval = 1; slot.desired_ns = slot.copy_open_ns += slot.refresh_ns;
    pair = vk_present_interp_push(&interp,1,7,2,slot,base);
    assert(pair.real == 0 && !pair.midpoint_ns);
    assert(pair.reason == VK_INTERP_INTERVAL);
    slot.mode = VK_PRESENT_SLOTS_NO_CLOCK; slot.interval = 0; slot.refresh_ns = 0;
    pair = vk_present_interp_push(&interp,2,8,2,slot,base);
    assert(pair.real == 1 && !pair.midpoint_ns && interp.retained == 2);
    assert(pair.reason == VK_INTERP_CLOCK);
    slot.mode = VK_PRESENT_SLOTS_ACTIVE; slot.interval = 2; slot.refresh_ns = refresh60;
    slot.desired_ns = slot.copy_open_ns = INT64_MAX - 2 * refresh60;
    vk_present_interp_push(&interp,3,9,2,slot,base);
    slot.desired_ns = slot.copy_open_ns = INT64_MAX - refresh60 + 1;
    pair = vk_present_interp_push(&interp,0,10,2,slot,base);
    assert(pair.real == 3 && !pair.midpoint_ns); /* signed desired-time overflow */
    assert(pair.reason == VK_INTERP_SLOT);
    pair = vk_present_interp_push(&interp,1,11,2,slot,base);
    assert(pair.real == 0 && !pair.midpoint_ns); /* no repeated midpoint target */
    assert(pair.reason == VK_INTERP_SLOT);
}
/* Model the disjoint ownerships at production, not just the number of game
 * textures. AHB completion frees PREVIOUS, never CURRENT; a prior child can
 * still be queued while that transaction awaits its release fence. */
static int claim(unsigned char *owned, unsigned count) {
    for (unsigned i = 0; i < count; i++) if (!owned[i]) { owned[i] = 1; return (int)i; }
    return -1;
}
static void interpolation_pool(void) {
    assert(vk_present_slots_buffers(0, 0, 0, 0) == 3);
    assert(vk_present_slots_buffers(0, 0, 4, 0) == 4);
    assert(vk_present_slots_buffers(0, 0, 100, 0) == 4);
    assert(vk_present_slots_buffers(1, 0, 0, 0) == 6);
    assert(vk_present_slots_buffers(1, 1, 0, 0) == 7);
    assert(vk_present_slots_buffers(1, 2, 0, 0) == 8);
    assert(vk_present_slots_buffers(1, 100, 0, 0) == 8);
    /* A larger opt-in maximum must not silently change runahead's
       four-slot requirement or legacy/SGSR interpolation sizing. */
    assert(VK_PRESENT_INTERP_LEGACY_CAPACITY == 6);
    assert(vk_present_slots_buffers(0, 0, vk_present_buffer_count(NULL,NULL), 0) == 3);
    assert(vk_present_slots_buffers(0, 0, vk_present_buffer_count(NULL,"2"), 0) == 4);
    assert(vk_present_slots_buffers(0, 0, vk_present_buffer_count("3","2"), 0) == 3);
    assert(vk_present_slots_buffers(0, 0, vk_present_buffer_count("4",NULL), 0) == 4);
    assert(vk_present_slots_buffers(1, 0, vk_present_buffer_count("3",NULL), 0) == 6);
    assert(vk_present_slots_buffers(1, 1, vk_present_buffer_count("3",NULL), 0) == 7);
    assert(vk_present_slots_buffers(0, 2, 0, 1) == 3);
    assert(vk_present_slots_buffers(0, 2, 4, 1) == 4);
    assert(vk_present_slots_buffers(1, 0, 0, 1) == 7);
    assert(vk_present_slots_buffers(1, 1, 0, 1) == 8);
    assert(vk_present_slots_buffers(1, 2, 0, 1) == 9);
    assert(vk_present_slots_buffers(1, 100, 0, 1) == VK_PRESENT_SLOT_CAPACITY);
    for (unsigned lead=1; lead<=2; lead++) {
        unsigned count=vk_present_slots_buffers(1, lead, 0, 0);
        unsigned char owned[VK_PRESENT_SLOT_CAPACITY] = {0};
        int previous=claim(owned,count), current=claim(owned,count);
        int retained=claim(owned,count), newer=claim(owned,count);
        int queued_child=claim(owned,count), new_child=claim(owned,count);
        assert(previous>=0 && current>=0 && retained>=0 && newer>=0 &&
            queued_child>=0 && new_child>=0);
        for (unsigned n=0; n<lead; n++) assert(claim(owned,count)>=0);
        assert(claim(owned,count)==-1); /* stall cannot exceed the per-lead bound */
        owned[previous]=0; /* only PREVIOUS's observed compositor release */
        assert(owned[current] && owned[retained] && owned[newer] &&
            owned[queued_child] && owned[new_child]);
        assert(claim(owned,count)==previous);
    }
}
/* Timed producer/GPU/presenter model, unlike the static claim-count test above.
 * One refresh is 100 units; a real is produced every 200. Odin FSR's observed
 * real-target lead at producer3000 is 4.87 refreshes: phase387 + lead1*100.
 * GPU completion and ordered PREVIOUS release are separate events. These are
 * explicit synthetic timings, not claims about arbitrary Android fence delay. */
typedef struct { unsigned children, child_drop, real_drop, source_drop, peak, source_peak; } pool_result;
static pool_result interpolation_phase(unsigned count, unsigned lead, unsigned phase) {
    enum { FRAMES=600, WARMUP=20, SOURCES=4, QUEUE=VK_PRESENT_SLOT_CAPACITY*2 };
    struct picture { uint64_t ready, desired; int generated, source, previous; }
        picture[VK_PRESENT_SLOT_CAPACITY]={0};
    struct source { uint64_t retire; unsigned references; } source[SOURCES]={0};
    struct transaction { uint64_t callback, release; int previous; }
        transaction[VK_PRESENT_FLIGHT_CAPACITY]={0};
    unsigned char owned[VK_PRESENT_SLOT_CAPACITY]={0};
    int queue[QUEUE], retained=-1, current=-1, history=-1, presenting=-1;
    unsigned qhead=0,qtail=0,frame=0;
    uint64_t now=0;
    vk_present_flight flight={0};
    pool_result result={0};
    assert(count<=VK_PRESENT_SLOT_CAPACITY);
    while (frame<FRAMES || qhead!=qtail || presenting>=0) {
        uint64_t production=frame<FRAMES ? (uint64_t)frame*200 : UINT64_MAX;
        uint64_t event=UINT64_MAX;
        if (presenting>=0) event=picture[presenting].ready;
        else if (flight.count==VK_PRESENT_FLIGHT_CAPACITY) {
            struct transaction *t=&transaction[flight.head];
            event=t->callback>t->release ? t->callback : t->release;
        } else if (qhead!=qtail) event=picture[queue[qhead%QUEUE]].ready;
        if (event<now) event=now;
        now=production<event ? production : event;
        if (production<=event) {
            int selected=-1;
            for (int i=0;i<SOURCES;i++) if (i!=history &&
                !source[i].references && source[i].retire<=now) { selected=i; break; }
            if (selected<0) { result.source_drop++; break; }
            source[selected].retire=now+40; /* current real GPU fence */
            int real=claim(owned,count);
            if (real<0) { result.real_drop++; break; }
            picture[real]=(struct picture){.ready=now+40,.source=selected};
            if (retained>=0) {
                picture[retained].desired=now+phase+lead*100;
                picture[retained].ready=now+40; /* successor fence covers pair */
                assert(qtail-qhead<QUEUE); queue[qtail++%QUEUE]=retained;
                int child=claim(owned,count);
                if (child<0) result.child_drop+=frame>=WARMUP;
                else {
                    picture[child]=(struct picture){.ready=now+45,
                        .desired=now+phase+(lead+1)*100,.generated=1,
                        .source=selected,.previous=history};
                    source[selected].references++; source[history].references++;
                    assert(qtail-qhead<QUEUE); queue[qtail++%QUEUE]=child;
                    result.children+=frame>=WARMUP;
                }
            }
            retained=real; history=selected; frame++;
            unsigned busy=0,live=0;
            for (unsigned i=0;i<count;i++) busy+=owned[i]!=0;
            for (int i=0;i<SOURCES;i++)
                live+=i==history || source[i].references || source[i].retire>now;
            if (busy>result.peak) result.peak=busy;
            if (live>result.source_peak) result.source_peak=live;
        } else if (presenting<0 && flight.count==VK_PRESENT_FLIGHT_CAPACITY) {
            unsigned index=flight.head;
            struct transaction *t=&transaction[index];
            assert(now>=t->callback && now>=t->release);
            /* Callback alone never frees CURRENT. Only this transaction's
               observed PREVIOUS release makes that slot available. */
            if (t->previous>=0) {
                assert(owned[t->previous] && t->previous!=retained);
                owned[t->previous]=0;
            }
            assert(vk_present_flight_retire(&flight,index));
        } else {
            if (presenting<0) { assert(qhead!=qtail); presenting=queue[qhead++%QUEUE]; }
            struct picture *p=&picture[presenting];
            if (p->ready>now) continue;
            if (p->generated) {
                assert(source[p->source].references && source[p->previous].references);
                source[p->source].references--; source[p->previous].references--;
            }
            int index=vk_present_flight_next(&flight); assert(index>=0);
            uint64_t callback=p->desired>now ? p->desired : now;
            transaction[index]=(struct transaction){.callback=callback,
                .release=callback+5,.previous=current};
            assert(vk_present_flight_submit(&flight,(unsigned)index));
            current=presenting; presenting=-1;
        }
    }
    return result;
}
static void interpolation_fsr_phase(void) {
    for (unsigned lead=0;lead<=VK_PRESENT_INTERP_MAX_LEAD;lead++) {
        unsigned ordinary=vk_present_slots_buffers(1,lead,0,0);
        unsigned fsr=vk_present_slots_buffers(1,lead,0,1);
        pool_result normal=interpolation_phase(ordinary,lead,250);
        assert(normal.children==580 && !normal.child_drop && !normal.real_drop && !normal.source_drop);
        /* The same phase, not a different filter mock: N starves, N+1 passes.
           Sweep readiness/display phase around the observed FSR snapshot. */
        for (unsigned phase=300;phase<=390;phase+=15) {
            pool_result before=interpolation_phase(ordinary,lead,phase);
            pool_result after=interpolation_phase(fsr,lead,phase);
            assert(before.child_drop && !before.real_drop && !before.source_drop);
            assert(after.children==580 && !after.child_drop && !after.real_drop && !after.source_drop);
            assert(after.peak==fsr && after.source_peak<=4);
        }
        pool_result observed=interpolation_phase(fsr,lead,387);
        assert(observed.children==580 && !observed.child_drop &&
            !observed.real_drop && !observed.source_drop);
        /* One extra slot is not unbounded runahead or early-owner reuse. */
        pool_result overload=interpolation_phase(fsr,lead,480);
        assert(overload.child_drop || overload.real_drop || overload.source_drop);
        printf("present FSR phase lead%u: %u starves, %u generates580/580; four sources; ordinary phase unchanged\n",
            lead,ordinary,fsr);
    }
}
static void interpolation_odin(void) {
    vk_present_slots slots = {0};
    vk_present_interp interp = {.retained = -1};
    unsigned ready = 0, generated = 0, handoffs = 0, expired = 0;
    uint64_t previous_midpoint = 0;
    for (unsigned frame = 0; frame <= 300; frame++) {
        uint64_t callback = base + frame * 2 * refresh60;
        vk_present_slot slot = reserve(&slots,callback,refresh60,2,callback + 1000000);
        uint64_t source_ready = slot.desired_ns + 3000000;
        vk_present_interp_pair pair = vk_present_interp_push(&interp,frame%6,frame+1,1,
            slot,source_ready);
        if (!frame) { assert(pair.reason == VK_INTERP_WARMUP); continue; }
        assert(pair.reason == VK_INTERP_READY); ready++;
        assert(pair.real == (int)((frame-1)%6));
        assert(pair.midpoint_ns == pair.slot.desired_ns + refresh60);
        if (previous_midpoint) assert(pair.slot.desired_ns == previous_midpoint + refresh60);
        /* Odin's ~2.1 ms output fits its own deadline, despite missing the
           parent's real slot by 3 ms. Old punctual-parent gate rejected all. */
        assert(!framegen_child_ready(source_ready,slot.desired_ns,pair.midpoint_ns,source_ready+2100000));
        generated++;
        framegen_child_reason result = framegen_interp_child_status(source_ready,pair.midpoint_ns,source_ready+2100000);
        handoffs += result == FRAMEGEN_CHILD_READY;
        expired += result != FRAMEGEN_CHILD_READY;
        previous_midpoint = pair.midpoint_ns;
    }
    assert(ready == 300 && generated == 300 && handoffs == 300 && !expired);
    uint64_t target = previous_midpoint + 2 * refresh60;
    assert(framegen_interp_child_status(target-1,target,target-1) == FRAMEGEN_CHILD_READY);
    assert(framegen_interp_child_status(0,target,target-1) == FRAMEGEN_CHILD_SOURCE_MISSING);
    assert(framegen_interp_child_status(target,target,target) == FRAMEGEN_CHILD_SOURCE_LATE);
    assert(framegen_interp_child_status(target-1,target,target) == FRAMEGEN_CHILD_DEADLINE);
    assert(framegen_interp_child_status(target-1,target,target+1) == FRAMEGEN_CHILD_DEADLINE);
    /* Refuse generation itself at the child's deadline, not its parent's. */
    vk_present_slot slot = reserve(&slots,target,refresh60,2,target);
    vk_present_interp_pair pair = vk_present_interp_push(&interp,0,302,1,slot,slot.desired_ns+refresh60);
    assert(pair.reason == VK_INTERP_LATE && pair.real >= 0 && !pair.midpoint_ns);
    puts("present interpolation regressions: Thor 4-slot starvation -> bounded 6-slot admission; Odin ready/generated/handed-off=300/300/300 expired=0; true child deadline/source failures rejected");
}

/* FrameTimeline policy fixtures use API-owned values with deliberately
 * noncontiguous, nonmonotonic IDs. Neither presentation arm may infer child
 * identity by arithmetic on the real ID or on a rounded physical period. */
static int64_t timeline_id(unsigned tick) {
    return tick & 1 ? (int64_t)tick * 101 : INT64_MAX - (int64_t)tick * 103;
}
static runtime_present_timelines timeline_fixture(uint64_t callback, uint64_t refresh,
        uint64_t generation, unsigned tick) {
    runtime_present_timelines s = {.frame_ns=callback,.sampled_ns=callback,
        .generation=generation,.refresh_ns=refresh,.count=RUNTIME_PRESENT_TIMELINE_CAPACITY};
    for (unsigned i=0;i<s.count;i++) s.entries[i]=(runtime_present_timeline_entry){
        .expected_ns=callback+(i+1)*refresh,.deadline_ns=callback+(i+1)*refresh-refresh/4,
        .vsync_id=timeline_id(tick+i+1)};
    return s;
}
static void timeline_pair_at(const runtime_present_timelines *s, unsigned i,
        runtime_present_timeline_pair pair) {
    assert(i+1<s->count && pair.refresh_ns==s->refresh_ns);
    assert(pair.real.expected_ns==s->entries[i].expected_ns);
    assert(pair.real.deadline_ns==s->entries[i].deadline_ns);
    assert(pair.real.vsync_id==s->entries[i].vsync_id);
    assert(pair.child.expected_ns==s->entries[i+1].expected_ns);
    assert(pair.child.deadline_ns==s->entries[i+1].deadline_ns);
    assert(pair.child.vsync_id==s->entries[i+1].vsync_id);
}
static void timeline_distinct_refreshes(void) {
    const uint64_t periods[]={16666667,16697278};
    for (unsigned lead=1;lead<=2;lead++) for (unsigned p=0;p<sizeof periods/sizeof *periods;p++) {
        uint64_t refresh=periods[p],half=refresh/2+refresh%2;
        const uint64_t shifts[]={1,120674,half-1,half,half+1};
        for (unsigned k=0;k<sizeof shifts/sizeof *shifts;k++) {
            runtime_present_timeline_state state={0};
            runtime_present_timeline_pair pair;
            runtime_present_timelines first=timeline_fixture(base,refresh,1,0);
            assert(runtime_present_timeline_choose(&state,&first,base+refresh+refresh/2,lead,1,1,&pair));
            uint64_t floor=state.last_child_ns;
            runtime_present_timelines next=timeline_fixture(base+2*refresh+shifts[k],refresh,2,100);
            uint64_t now=base+3*refresh;
            runtime_present_timeline_state old=state,control=state;
            assert(runtime_present_timeline_choose(&control,&next,now,lead,1,0,&pair));
            assert(pair.real.expected_ns==floor+shifts[k]);
            assert(runtime_present_timeline_choose(&state,&next,now,lead,1,1,&pair));
            assert(pair.real.expected_ns==floor+shifts[k]+(shifts[k]<half ? refresh:0));
            timeline_pair_at(&next,lead+(shifts[k]<half),pair); /* adjacent copied API entries */
            assert(pair.real.expected_ns-old.last_child_ns>=half);
            if (shifts[k]<half) {
                next.count=lead+2; state=old;
                runtime_present_timeline_pair untouched=pair;
                assert(!runtime_present_timeline_choose(&state,&next,now,lead,1,1,&pair));
                assert(!memcmp(&state,&old,sizeof old) && !memcmp(&pair,&untouched,sizeof pair));
                /* Only a drained new window resets the prior-child floor. */
                assert(runtime_present_timeline_choose(&state,&next,now,lead,2,1,&pair));
                assert(pair.real.expected_ns==floor+shifts[k]);
            }
        }
    }
    const uint64_t limit=INT64_MAX;
    runtime_present_timelines edge={.frame_ns=limit-250,.sampled_ns=limit-250,.refresh_ns=100,.count=2,
        .entries={{.expected_ns=limit-40,.deadline_ns=limit-50,.vsync_id=0},
                  {.expected_ns=limit,.deadline_ns=limit-10,.vsync_id=INT64_MAX}}};
    runtime_present_timeline_state state={.window_generation=1,.last_child_ns=limit-60},old=state;
    runtime_present_timeline_pair pair={0},untouched=pair;
    assert(!runtime_present_timeline_choose(&state,&edge,limit-200,1,1,1,&pair));
    assert(!memcmp(&state,&old,sizeof old) && !memcmp(&pair,&untouched,sizeof pair));
    assert(runtime_present_timeline_choose(&state,&edge,limit-200,1,1,0,&pair));
}
static void timeline_rejected(runtime_present_timeline_state *state,
        const runtime_present_timelines *s, uint64_t now, unsigned lead, uint64_t window) {
    runtime_present_timeline_state before=*state;
    runtime_present_timeline_pair pair={.real={11,12,13},.child={21,22,23},.refresh_ns=31};
    runtime_present_timeline_pair untouched=pair;
    assert(!runtime_present_timeline_choose(state, s, now, lead, window, 0, &pair));
    assert(!memcmp(state,&before,sizeof before));
    assert(!memcmp(&pair,&untouched,sizeof pair));
}
static void timeline_reanchor_rejected(runtime_present_timeline_state *state,
        uint64_t callback, uint64_t refresh, uint64_t now, unsigned lead, uint64_t window) {
    runtime_present_timeline_state before=*state;
    runtime_present_timeline_pair pair={.real={11,12,13},.child={21,22,23},.refresh_ns=31};
    runtime_present_timeline_pair untouched=pair;
    assert(!runtime_present_timeline_reanchor(state,callback,refresh,now,lead,window,&pair));
    assert(!memcmp(state,&before,sizeof before));
    assert(!memcmp(&pair,&untouched,sizeof pair));
}
static void timeline_fallback_pair(runtime_present_timeline_pair pair, uint64_t real,
        uint64_t refresh) {
    assert(pair.real.expected_ns==real && pair.child.expected_ns==real+refresh);
    assert(pair.real.deadline_ns==pair.real.expected_ns && pair.child.deadline_ns==pair.child.expected_ns);
    assert(pair.real.vsync_id==-1 && pair.child.vsync_id==-1 && pair.refresh_ns==refresh);
}
/* Reuse the existing retained-real and direct-real harness policies. Timeline
 * anchoring is independent of which source picture owns the real transaction;
 * frame lag changes source latency, never the real/child display roles. */
static vk_present_interp_pair timeline_legacy_pair(vk_present_slots *slots,
        vk_present_interp *interp, unsigned lag, unsigned lead, unsigned frame,
        uint64_t callback, uint64_t refresh, uint64_t now) {
    vk_present_slot slot=reserve(slots,callback,refresh,2,now);
    vk_present_interp_pair pair;
    if (lag) pair=vk_present_interp_push(interp,frame%7,frame+1,1,slot,now);
    else pair=(vk_present_interp_pair){.real=(int)(frame%7),.slot=slot,.reason=VK_INTERP_READY};
    return vk_present_interp_buffer(pair,lead,now);
}
static void timeline_frame_lag(unsigned lag, unsigned lead, uint64_t refresh) {
    vk_present_slots slots={0};
    vk_present_interp interp={.retained=-1};
    runtime_present_timeline_state state={0};
    uint64_t last_child=0;
    for (unsigned frame=0;frame<300;frame++) {
        uint64_t callback=base+(uint64_t)frame*2*refresh, now=callback+1000000;
        runtime_present_timelines s=timeline_fixture(callback,refresh,1,frame*2);
        s.sampled_ns=now;
        vk_present_interp_pair source=timeline_legacy_pair(&slots,&interp,lag,lead,frame,
            callback,refresh,now);
        if (lag && !frame) {
            assert(source.real<0 && source.reason==VK_INTERP_WARMUP);
            assert(!state.last_real_ns && !state.last_child_ns);
            continue;
        }
        assert(source.real==(int)((frame-lag)%7) && source.reason==VK_INTERP_READY);
        runtime_present_timelines unchanged=s;
        runtime_present_timeline_pair pair;
        assert(runtime_present_timeline_choose(&state, &s, now, lead, 1, 0, &pair));
        assert(!memcmp(&s,&unchanged,sizeof s));
        timeline_pair_at(&s,lead,pair);
        assert(pair.real.expected_ns==callback+(lead+1)*refresh);
        assert(pair.child.expected_ns==callback+(lead+2)*refresh);
        if (last_child) assert(pair.real.expected_ns==last_child+refresh);
        assert(pair.real.expected_ns>=now+lead*refresh && pair.real.deadline_ns>now);
        assert(pair.child.deadline_ns>now && pair.real.vsync_id!=pair.child.vsync_id);
        assert(state.window_generation==1 && state.clock_generation==1);
        assert(state.last_real_ns==pair.real.expected_ns && state.last_child_ns==pair.child.expected_ns);
        last_child=pair.child.expected_ns;
        /* Callback storage can be overwritten as soon as it returns. */
        s.entries[lead]=(runtime_present_timeline_entry){0};
        s.entries[lead+1]=(runtime_present_timeline_entry){0};
        timeline_pair_at(&unchanged,lead,pair);
    }
}
static void timeline_variable(unsigned lead) {
    enum { FRAMES=600, TICKS=2*FRAMES+RUNTIME_PRESENT_TIMELINE_CAPACITY };
    uint64_t panel[TICKS+1];
    panel[0]=base;
    for (unsigned i=1;i<=TICKS;i++) {
        uint64_t period=i<400 ? 16666666 : i<800 ? 11111111 : 20000000;
        panel[i]=panel[i-1]+period+(i%3==0 ? 1 : 0);
    }
    runtime_present_timeline_state state={0};
    uint64_t last_child=0;
    unsigned nonuniform=0;
    for (unsigned frame=0;frame<FRAMES;frame++) {
        unsigned tick=frame*2;
        uint64_t refresh=panel[tick+1]-panel[tick], now=panel[tick]+refresh/5;
        runtime_present_timelines s={.frame_ns=panel[tick],.sampled_ns=now,
            .refresh_ns=refresh,.generation=1,.count=RUNTIME_PRESENT_TIMELINE_CAPACITY};
        for (unsigned i=0;i<s.count;i++) s.entries[i]=(runtime_present_timeline_entry){
            .expected_ns=panel[tick+i+1],.deadline_ns=panel[tick+i+1]-refresh/4,
            .vsync_id=timeline_id(tick+i+1)};
        unsigned expected=0;
        while (expected+1<s.count && (s.entries[expected].expected_ns<now+lead*refresh ||
            s.entries[expected].expected_ns<=last_child)) expected++;
        assert(expected+1<s.count);
        runtime_present_timeline_pair pair;
        assert(runtime_present_timeline_choose(&state, &s, now, lead, 1, 0, &pair));
        timeline_pair_at(&s,expected,pair);
        assert(pair.real.expected_ns>last_child);
        nonuniform+=pair.child.expected_ns-pair.real.expected_ns!=refresh;
        last_child=pair.child.expected_ns;
    }
    assert(nonuniform); /* Exact API times survive fractional and mode changes. */
}
static void timeline_beat(unsigned lead) {
    const uint64_t refresh=16697278; /* Guest30 and panel59.89 cross beats. */
    runtime_present_timeline_state state={0};
    uint64_t last_tick=0;
    unsigned crossings=0, previous_phase=0;
    for (unsigned frame=0;frame<1800;frame++) {
        uint64_t now=base+(uint64_t)frame*1000000000/30+refresh/3;
        uint64_t callback_tick=(now-base)/refresh;
        uint64_t callback=base+callback_tick*refresh;
        runtime_present_timelines s=timeline_fixture(callback,refresh,1,(unsigned)callback_tick);
        s.sampled_ns=now;
        uint64_t delta=now+lead*refresh-base;
        uint64_t real_tick=delta/refresh+(delta%refresh!=0);
        if (real_tick<=last_tick) real_tick=last_tick+1;
        runtime_present_timeline_pair pair;
        assert(runtime_present_timeline_choose(&state, &s, now, lead, 1, 0, &pair));
        assert(pair.real.expected_ns==base+real_tick*refresh);
        assert(pair.child.expected_ns==base+(real_tick+1)*refresh);
        assert(pair.real.vsync_id==timeline_id((unsigned)real_tick));
        assert(pair.child.vsync_id==timeline_id((unsigned)real_tick+1));
        assert((pair.real.expected_ns-base)%refresh==0); /* No accumulated guest-period drift. */
        unsigned phase=(unsigned)((now-base)%refresh);
        crossings+=frame && phase>previous_phase;
        previous_phase=phase;
        last_tick=real_tick+1;
    }
    assert(crossings>=3);
}
static void timeline_invalid(void) {
    const uint64_t now=base+1000000;
    runtime_present_timelines good=timeline_fixture(base,refresh60,9,0), bad;
    runtime_present_timeline_state state={.window_generation=7,.clock_generation=9};
    runtime_present_timeline_pair pair={0}, untouched=pair;
    runtime_present_timeline_state before=state;
    assert(!runtime_present_timeline_choose(NULL, &good, now, 1, 7, 0, &pair));
    assert(!memcmp(&pair,&untouched,sizeof pair));
    assert(!runtime_present_timeline_choose(&state, &good, now, 1, 7, 0, NULL));
    assert(!memcmp(&state,&before,sizeof state));
    timeline_rejected(&state,NULL,now,1,7);
    timeline_rejected(&state,&good,now,0,7);
    timeline_rejected(&state,&good,now,3,7);
    timeline_rejected(&state,&good,now,UINT32_MAX,7);
    bad=good; bad.count=0; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.count=1; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.count=RUNTIME_PRESENT_TIMELINE_CAPACITY+1;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.count=UINT32_MAX; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.preferred=bad.count; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.preferred=bad.count-1; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.refresh_ns=0; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.refresh_ns=(uint64_t)INT64_MAX/2+1; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.frame_ns=0; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.sampled_ns=0; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.frame_ns=bad.sampled_ns+1; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.sampled_ns=now+1; timeline_rejected(&state,&bad,now,1,7);
    timeline_rejected(&state,&good,(uint64_t)INT64_MAX+1,1,7);
    timeline_rejected(&state,&good,UINT64_MAX,1,7);
    bad=good; bad.entries[0].expected_ns=0; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[15].expected_ns=(uint64_t)INT64_MAX+1;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[1].expected_ns=bad.entries[0].expected_ns;
    bad.entries[1].deadline_ns=bad.entries[0].deadline_ns+1;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[1].expected_ns=bad.entries[0].expected_ns-1;
    bad.entries[1].deadline_ns=bad.entries[0].deadline_ns+1;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[0].deadline_ns=0; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[0].deadline_ns=bad.entries[0].expected_ns+1;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[1].deadline_ns=bad.entries[0].deadline_ns;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[1].deadline_ns=bad.entries[0].deadline_ns-1;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[0].vsync_id=-1; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[15].vsync_id=-2; timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[15].vsync_id=bad.entries[0].vsync_id;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.entries[0].vsync_id=bad.entries[1].vsync_id=0;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.count=2; bad.entries[0].deadline_ns=now;
    timeline_rejected(&state,&bad,now,1,7);
    bad=good; bad.count=3; timeline_rejected(&state,&bad,now,2,7);
    bad=good;
    for (unsigned i=0;i<bad.count;i++) bad.entries[i].deadline_ns=now-bad.count+i;
    timeline_rejected(&state,&bad,now,1,7);
    /* Even a malformed tail invalidates a snapshot with usable early entries. */
    bad=good; bad.generation++; bad.entries[15].deadline_ns=0;
    timeline_rejected(&state,&bad,now,1,8);
}
static void timeline_boundaries(void) {
    runtime_present_timelines s=timeline_fixture(base,refresh60,1,0);
    runtime_present_timeline_state state={0};
    runtime_present_timeline_pair pair;
    /* Zero is a valid ID; the second ID need not be adjacent or ordered. */
    s.count=2; s.entries[0].vsync_id=0; s.entries[1].vsync_id=INT64_MAX;
    assert(runtime_present_timeline_choose(&state, &s, base, 1, 0, 0, &pair));
    timeline_pair_at(&s,0,pair);
    timeline_rejected(&state,&s,base,1,0); /* No duplicate reservation. */
    s=timeline_fixture(base,refresh60,2,0);
    s.preferred=3;
    assert(runtime_present_timeline_choose(&state, &s, base, 1, 0, 0, &pair));
    timeline_pair_at(&s,3,pair); /* Do not override the API's preferred floor. */
    s=timeline_fixture(base,refresh60,3,0);
    state=(runtime_present_timeline_state){0}; /* Independent deadline fixture. */
    s.entries[0].deadline_ns=base+999999;
    s.entries[1].deadline_ns=base+1000000;
    s.entries[2].deadline_ns=base+1000001;
    assert(runtime_present_timeline_choose(&state, &s, base+1000000, 1, 0, 0, &pair));
    timeline_pair_at(&s,2,pair); /* A deadline equal to now has closed. */
    s=timeline_fixture(base,refresh60,4,0);
    state=(runtime_present_timeline_state){0}; /* Independent freshness fixture. */
    assert(runtime_present_timeline_choose(&state, &s, base+2*refresh60, 1, 0, 0, &pair));
    timeline_pair_at(&s,2,pair); /* Exactly two refreshes old is still fresh. */
    timeline_rejected(&state,&s,base+2*refresh60+1,1,0);
    s.sampled_ns=base+2*refresh60+1;
    timeline_rejected(&state,&s,s.sampled_ns,1,0); /* Fresh receipt cannot revive an old callback. */
    const uint64_t limit=(uint64_t)INT64_MAX;
    s=(runtime_present_timelines){.frame_ns=limit-400,.sampled_ns=limit-400,
        .refresh_ns=100,.generation=5,.count=4};
    for (unsigned i=0;i<s.count;i++) s.entries[i]=(runtime_present_timeline_entry){
        .expected_ns=limit-300+i*100,.deadline_ns=limit-325+i*100,.vsync_id=timeline_id(i)};
    assert(runtime_present_timeline_choose(&state, &s, limit-300, 2, 0, 0, &pair));
    timeline_pair_at(&s,2,pair); /* The child's signed-int64 maximum is representable. */
    s.frame_ns=s.sampled_ns=limit-199;
    timeline_rejected(&state,&s,limit-199,2,1); /* now+lead must not wrap. */
    s=timeline_fixture(base,refresh60,0,0);
    assert(runtime_present_timeline_choose(&state, &s, base, 1, 1, 0, &pair)); /* New window; epoch zero is an identity. */
    timeline_pair_at(&s,0,pair);
}
static void timeline_resume(void) {
    runtime_present_timeline_state state={0};
    runtime_present_timelines s=timeline_fixture(base,refresh60,1,0);
    runtime_present_timeline_pair pair, old;
    for (unsigned i=0;i<RUNTIME_PRESENT_TIMELINE_CAPACITY/2;i++) {
        assert(runtime_present_timeline_choose(&state, &s, base, 1, 1, 0, &pair));
        timeline_pair_at(&s,i*2,pair);
    }
    timeline_rejected(&state,&s,base,1,1); /* Finite snapshot, no invented continuation. */
    old=pair;
    uint64_t resumed=base+1000*refresh60;
    timeline_rejected(&state,&s,resumed,2,1); /* Producer pause: stale, no catch-up reservations. */
    timeline_rejected(&state,NULL,resumed,2,1);
    s=timeline_fixture(resumed,refresh60,1,1000);
    assert(runtime_present_timeline_choose(&state, &s, resumed, 2, 1, 0, &pair));
    timeline_pair_at(&s,1,pair);
    assert(pair.real.expected_ns==resumed+2*refresh60 && pair.real.expected_ns>old.child.expected_ns);
    old=pair;
    assert(runtime_present_timeline_choose(&state, &s, resumed, 1, 1, 0, &pair));
    timeline_pair_at(&s,3,pair); /* Lead reduction still cannot overtake the previous child. */
    assert(pair.real.expected_ns==old.child.expected_ns+refresh60);
    old=pair;
    s.generation=2; /* Recalibration must not overtake queued old-clock targets. */
    runtime_present_timelines missing=s; missing.count=0;
    timeline_rejected(&state,&missing,resumed,1,1);
    assert(runtime_present_timeline_choose(&state, &s, resumed, 1, 1, 0, &pair));
    timeline_pair_at(&s,5,pair);
    assert(state.clock_generation==2 && state.window_generation==1);
    assert(pair.real.expected_ns==old.child.expected_ns+refresh60);
    runtime_present_timeline_pair clock_pair=pair;
    assert(runtime_present_timeline_choose(&state, &s, resumed, 2, 2, 0, &pair));
    timeline_pair_at(&s,1,pair);
    assert(state.clock_generation==2 && state.window_generation==2);
    assert(clock_pair.real.expected_ns==s.entries[5].expected_ns);
    assert(pair.real.expected_ns<clock_pair.real.expected_ns); /* Only the replaced surface resets the floor. */
    assert(old.real.expected_ns==s.entries[3].expected_ns); /* Queued values remain immutable. */
    s.count=4;
    timeline_rejected(&state,&s,resumed,1,2); /* Same generation keeps the selected floor. */
}
static void timeline_reanchor_transitions(unsigned lead) {
    runtime_present_timeline_state state={0};
    runtime_present_timelines s=timeline_fixture(base,refresh60,1,0);
    runtime_present_timeline_pair pair, api, fallback;
    assert(runtime_present_timeline_choose(&state, &s, base, lead, 1, 0, &api));
    runtime_present_timelines short_snapshot=s; short_snapshot.count=1;
    timeline_rejected(&state,&short_snapshot,base,lead,1);
    assert(runtime_present_timeline_reanchor(&state,base,refresh60,base,lead,1,&pair));
    timeline_fallback_pair(pair,api.child.expected_ns+refresh60,refresh60);
    assert(state.clock_generation==1);
    uint64_t resumed=base+1000*refresh60, now=resumed+1000000;
    timeline_rejected(&state,&s,now,lead,1); /* Old API callback after a producer pause. */
    timeline_rejected(&state,NULL,now,lead,1);
    assert(runtime_present_timeline_reanchor(&state,resumed,refresh60,now,lead,1,&fallback));
    timeline_fallback_pair(fallback,resumed+(lead+1)*refresh60,refresh60);
    assert(fallback.real.expected_ns>pair.child.expected_ns && state.clock_generation==1);
    /* A new calibration cannot reset the fallback floor. Resume exact API
       timestamps/IDs at the earliest adjacent pair beyond the legacy child. */
    s=timeline_fixture(resumed,16697278,2,1000); s.sampled_ns=now;
    assert(runtime_present_timeline_choose(&state, &s, now, lead, 1, 0, &pair));
    timeline_pair_at(&s,lead+1,pair);
    assert(pair.real.expected_ns>fallback.child.expected_ns && state.clock_generation==2);
    api=pair;
    s.generation=3;
    assert(runtime_present_timeline_choose(&state, &s, now, lead, 1, 0, &pair));
    timeline_pair_at(&s,lead+3,pair);
    assert(pair.real.expected_ns>api.child.expected_ns && state.clock_generation==3);
    api=pair;
    assert(runtime_present_timeline_choose(&state, &s, now, lead, 2, 0, &pair));
    timeline_pair_at(&s,lead,pair);
    assert(pair.real.expected_ns<api.real.expected_ns && state.window_generation==2);
    timeline_fallback_pair(fallback,resumed+(lead+1)*refresh60,refresh60);
}
static void timeline_reanchor_boundaries(void) {
    runtime_present_timeline_state state={.clock_generation=42};
    runtime_present_timeline_pair pair={0}, untouched=pair;
    runtime_present_timeline_state before=state;
    assert(!runtime_present_timeline_reanchor(NULL,base,refresh60,base,1,7,&pair));
    assert(!memcmp(&pair,&untouched,sizeof pair));
    assert(!runtime_present_timeline_reanchor(&state,base,refresh60,base,1,7,NULL));
    assert(!memcmp(&state,&before,sizeof state));
    assert(runtime_present_timeline_reanchor(&state,base,refresh60,base+2*refresh60,1,7,&pair));
    timeline_fallback_pair(pair,base+3*refresh60,refresh60); /* Inclusive freshness boundary. */
    assert(state.window_generation==7 && state.clock_generation==42);
    timeline_reanchor_rejected(&state,base,refresh60,base+2*refresh60+1,1,7);
    timeline_reanchor_rejected(&state,0,refresh60,base,1,7);
    timeline_reanchor_rejected(&state,base+1,refresh60,base,1,7);
    timeline_reanchor_rejected(&state,base,0,base,1,7);
    timeline_reanchor_rejected(&state,base,(uint64_t)INT64_MAX/2+1,base,1,7);
    timeline_reanchor_rejected(&state,base,refresh60,base,0,7);
    timeline_reanchor_rejected(&state,base,refresh60,base,3,7);
    timeline_reanchor_rejected(&state,base,refresh60,base,UINT32_MAX,7);
    timeline_reanchor_rejected(&state,base,refresh60,(uint64_t)INT64_MAX+1,1,7);
    timeline_reanchor_rejected(&state,base,refresh60,UINT64_MAX,1,7);
    state.last_real_ns=base+5*refresh60+123; state.last_child_ns=base+6*refresh60+123;
    uint64_t shifted=base+refresh60/3;
    assert(runtime_present_timeline_reanchor(&state,shifted,refresh60,shifted+1000000,2,7,&pair));
    timeline_fallback_pair(pair,shifted+6*refresh60,refresh60); /* Strict off-lattice child floor. */
    assert(runtime_present_timeline_reanchor(&state,shifted,refresh60,shifted+1000000,1,8,&pair));
    timeline_fallback_pair(pair,shifted+2*refresh60,refresh60); /* Surface replacement alone resets. */
    const uint64_t limit=(uint64_t)INT64_MAX;
    assert(runtime_present_timeline_reanchor(&state,limit-300,100,limit-300,2,9,&pair));
    timeline_fallback_pair(pair,limit-100,100); /* Child may equal INT64_MAX. */
    assert(state.clock_generation==42);
    timeline_reanchor_rejected(&state,limit-300,100,limit-300,2,9); /* No room beyond prior child. */
    timeline_reanchor_rejected(&state,limit-100,100,limit-100,1,10); /* Real fits; child would overflow. */
    timeline_reanchor_rejected(&state,limit-99,100,limit-99,1,10); /* Minimum itself would overflow. */
}
static void timeline_legacy_equivalence(void) {
#if !defined(__ANDROID__) || !defined(TOTK_ANDROID_APP)
    runtime_present_timelines unavailable={.count=99}, untouched=unavailable;
    assert(!runtime_android_present_timeline_active());
    assert(runtime_android_present_timeline_display_id()==-1);
    assert(!runtime_android_present_timelines(&unavailable));
    assert(!memcmp(&unavailable,&untouched,sizeof unavailable));
    for (unsigned lag=0;lag<=1;lag++) for (unsigned lead=1;lead<=2;lead++)
    for (unsigned requested=0;requested<=1;requested++) {
        vk_present_slots plain_slots={0}, optional_slots={0};
        vk_present_interp plain_interp={.retained=-1}, optional_interp={.retained=-1};
        runtime_present_timeline_state state={.window_generation=7,.clock_generation=9,
            .last_real_ns=11,.last_child_ns=12}, before=state;
        for (unsigned frame=0;frame<120;frame++) {
            uint64_t refresh=frame<60 ? refresh60 : 16697278;
            uint64_t callback=base+(uint64_t)frame*2*refresh, now=callback+1000000;
            vk_present_interp_pair plain=timeline_legacy_pair(&plain_slots,&plain_interp,
                lag,lead,frame,callback,refresh,now);
            vk_present_interp_pair optional=timeline_legacy_pair(&optional_slots,&optional_interp,
                lag,lead,frame,callback,refresh,now);
            if (requested && runtime_android_present_timeline_active()) {
                runtime_present_timeline_pair selected;
                assert(runtime_android_present_timelines(&unavailable));
                assert(runtime_present_timeline_choose(&state, &unavailable, now, lead, 1, 0, &selected));
                assert(0); /* Unadmitted host path must never enter the selector. */
            }
            assert(plain.real==optional.real && plain.reason==optional.reason);
            assert(plain.midpoint_ns==optional.midpoint_ns);
            assert(plain.slot.desired_ns==optional.slot.desired_ns);
            assert(plain.slot.copy_open_ns==optional.slot.copy_open_ns);
            assert(plain.slot.refresh_ns==optional.slot.refresh_ns && plain.slot.interval==optional.slot.interval);
            assert(plain.slot.mode==optional.slot.mode && plain.slot.skipped==optional.slot.skipped);
        }
        assert(!memcmp(&state,&before,sizeof state));
    }
#endif
}
static void timeline_wait_order(void) {
    assert(runtime_present_timeline_display_allowed(0,0));
    assert(!runtime_present_timeline_display_allowed(0,1));
    assert(!runtime_present_timeline_display_allowed(0,-1));
    assert(!runtime_present_timeline_display_allowed(-1,0));
    assert(runtime_present_timeline_display_allowed(3,3));
    for (unsigned lag=0;lag<=1;lag++) for (int own=0;own<=1;own++)
    for (int next=0;next<=1;next++) {
        /* Own fence is ALWAYS a prerequisite; the record hold has already
         * happened in either recorder arm before this presenter predicate. */
        int legacy=own && (!runtime_present_pair_wait_required(0,0,1) || next);
        int timeline=own && (!runtime_present_pair_wait_required(RUNTIME_PRESENT_TIMELINE_REAL,0,1) || next);
        int child=own && (!runtime_present_pair_wait_required(RUNTIME_PRESENT_TIMELINE_GENERATED,1,1) || next);
        assert(legacy==(own && next));
        assert(timeline==own);
        assert(child==(own && next));
        assert(!runtime_present_pair_wait_required(RUNTIME_PRESENT_TIMELINE_REAL,0,0));
        assert(!runtime_present_pair_wait_required(0,1,1)); /* Legacy child's own queue fence covers sources. */
        assert(runtime_present_pair_wait_required(RUNTIME_PRESENT_TIMELINE_REAL,1,1)); /* Malformed role is conservative. */
        (void)lag;
    }
    /* Regression: with an API deadline at10ms, immutable N is ready at4ms
     * while N+1 renders until15ms. The old forward wait misses the real target;
     * independent N applies at4ms and its generated child still waits to15ms. */
    uint64_t own_ready=4000000,pair_ready=15000000,real_deadline=10000000,child_deadline=26000000;
    assert((runtime_present_pair_wait_required(0,0,1)?pair_ready:own_ready)>real_deadline);
    assert((runtime_present_pair_wait_required(RUNTIME_PRESENT_TIMELINE_REAL,0,1)?pair_ready:own_ready)<real_deadline);
    assert((runtime_present_pair_wait_required(RUNTIME_PRESENT_TIMELINE_GENERATED,1,1)?pair_ready:own_ready)<child_deadline);
}
static void timelines(void) {
    const uint64_t periods[]={16666667,16697278};
    for (unsigned lag=0;lag<=1;lag++) for (unsigned lead=1;lead<=2;lead++)
        for (unsigned i=0;i<sizeof periods/sizeof *periods;i++) timeline_frame_lag(lag,lead,periods[i]);
    for (unsigned lead=1;lead<=2;lead++) {
        timeline_variable(lead); timeline_beat(lead); timeline_reanchor_transitions(lead);
    }
    timeline_invalid(); timeline_boundaries(); timeline_resume(); timeline_reanchor_boundaries();
    timeline_legacy_equivalence();
    timeline_wait_order();
    timeline_distinct_refreshes();
    puts("present timelines: both frame-lag arms, lead1/2, exact API roles/times/IDs, finite deadlines, monotonic recalibration/fallback/resume, surface boundaries and unadmitted legacy equivalence passed");
}

int main(void) {
    eligibility_floor();
    assert(vk_present_slots_interval(1.0 / 30, refresh60) == 2);
    assert(vk_present_slots_interval(1.0 / 60, refresh60) == 1);
    assert(vk_present_slots_interval(1.0 / 30, 8333333) == 4);
    assert(vk_present_slots_interval(1.0 / 60, 8333333) == 2);
    assert(vk_present_slots_interval(0, refresh60) == 1);
    assert(vk_present_slots_interval(1.0 / 30, 0) == 1);
    cadence(2); cadence(1); late_short(); long_stall(); mode_changes(); fractional_refresh(); disabled_unavailable();
    auto40_policy(); auto40_transitions();
    interpolation_order(); interpolation_late(); interpolation_buffered(); interpolation_boundaries();
    interpolation_pool(); interpolation_fsr_phase(); interpolation_odin();
    timelines();
    puts("present slots: real30/60 cadence, interpolation order/latency, late-child drops, phase skips, clock/surface boundaries and legacy fallback passed");
    return 0;
}

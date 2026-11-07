/* Fixed display slots for the opt-in Android presenter. No clocks, waits or
 * allocation: the producer owns one state, and queues each reservation with
 * its pixels. All timestamps are CLOCK_MONOTONIC nanoseconds. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Legacy interpolation owns six disjoint slots: two real pair members,
 * CURRENT/PREVIOUS compositor owners, a queued child and a new child.
 * Backpressured future presentation retains up to one additional buffer per
 * refresh of lead. Two-pass FSR scene filtering exposes one more steady-state
 * queued-ready picture at the measured production/display phase: reserve one
 * extra buffer for that opt-in only. These are admission bounds, never permission
 * to reuse GPU/compositor-owned pixels. All image allocations remain lazy. */
enum {
    VK_PRESENT_SLOT_DEFAULT_BUFFERS = 3, VK_PRESENT_SLOT_OPTIONAL_BUFFERS = 4,
    VK_PRESENT_INTERP_REALS = 2, VK_PRESENT_INTERP_AHB = 2,
    VK_PRESENT_INTERP_QUEUED_CHILD = 1, VK_PRESENT_INTERP_NEW_CHILD = 1,
    VK_PRESENT_INTERP_LEGACY_CAPACITY = VK_PRESENT_INTERP_REALS + VK_PRESENT_INTERP_AHB +
        VK_PRESENT_INTERP_QUEUED_CHILD + VK_PRESENT_INTERP_NEW_CHILD,
    VK_PRESENT_INTERP_MAX_LEAD = 2,
    VK_PRESENT_INTERP_FSR_PHASE = 1,
    VK_PRESENT_SLOT_CAPACITY = VK_PRESENT_INTERP_LEGACY_CAPACITY + VK_PRESENT_INTERP_MAX_LEAD +
        VK_PRESENT_INTERP_FSR_PHASE
};

static inline unsigned vk_present_slots_buffers(int interpolating, unsigned lead,
        unsigned requested, int scene_fsr) {
    if (interpolating) return VK_PRESENT_INTERP_LEGACY_CAPACITY +
        (lead > VK_PRESENT_INTERP_MAX_LEAD ? VK_PRESENT_INTERP_MAX_LEAD : lead) +
        (scene_fsr ? VK_PRESENT_INTERP_FSR_PHASE : 0);
    return requested >= VK_PRESENT_SLOT_OPTIONAL_BUFFERS ? VK_PRESENT_SLOT_OPTIONAL_BUFFERS :
        VK_PRESENT_SLOT_DEFAULT_BUFFERS;
}


/* AHB retains CURRENT independently of the pending-credit budget: two banked
 * frames need CURRENT + three pending buffers. Explicit smaller overrides win
 * here; acquisition refuses lead two rather than silently overriding them. */
static inline unsigned vk_present_buffer_count(const char *buffers, const char *slack) {
    if (buffers) return atoi(buffers) >= VK_PRESENT_SLOT_OPTIONAL_BUFFERS ?
        VK_PRESENT_SLOT_OPTIONAL_BUFFERS : VK_PRESENT_SLOT_DEFAULT_BUFFERS;
    return slack && !strcmp(slack, "2") ? VK_PRESENT_SLOT_OPTIONAL_BUFFERS : VK_PRESENT_SLOT_DEFAULT_BUFFERS;
}

/* Odin rejects desired == predicted present, and resampled predictions can
 * differ by ~0.1 ms. Keep the one-nanosecond floor and measured 125 us guard
 * separately gated; neither changes reservations or API timeline metadata. */
enum { VK_PRESENT_ELIGIBILITY_GUARD_NS = 125000 };
static inline unsigned vk_present_floor_config(const char *value) {
    return value && !strcmp(value, "1");
}
static inline unsigned vk_present_guard_config(const char *value) {
    return value && !strcmp(value, "125000") ? VK_PRESENT_ELIGIBILITY_GUARD_NS : 0;
}
static inline uint64_t vk_present_eligible_time(uint64_t desired_ns, unsigned floor_ns,
        unsigned guard_ns, uint64_t refresh_ns) {
    uint64_t shift = floor_ns == 1;
    if (guard_ns == VK_PRESENT_ELIGIBILITY_GUARD_NS && refresh_ns / 2 > guard_ns + shift)
        shift += guard_ns;
    return shift && desired_ns > shift && desired_ns <= INT64_MAX ? desired_ns - shift : desired_ns;
}

typedef struct {
    uint64_t desired_ns, refresh_ns;
} vk_present_slots;

enum { VK_PRESENT_SLOTS_OFF, VK_PRESENT_SLOTS_NO_CLOCK, VK_PRESENT_SLOTS_ACTIVE };
typedef struct {
    uint64_t desired_ns, copy_open_ns, refresh_ns, skipped;
    uint32_t interval;
    int mode;
} vk_present_slot;

/* The producer supplies seconds, not the guest's nominal 60 Hz tick count.
 * Quantize that requested duration to the nearest physical refresh count. */
static inline uint32_t vk_present_slots_interval(double period, uint64_t refresh_ns) {
    if (!(period > 0) || !refresh_ns) return 1;
    double ticks = period * (1e9 / (double)refresh_ns);
    if (ticks >= (double)UINT32_MAX - 0.5) return UINT32_MAX;
    return ticks < 1 ? 1 : (uint32_t)(ticks + 0.5);
}

/* auto is deliberately the 40fps gameplay policy plus its30fps cutscene
 * floor, not nearest-refresh quantization for arbitrary caps. The producer's
 * 25ms period remains authoritative at60Hz; observed120Hz admits three slots.
 * The cutscene floor uses two60Hz/four120Hz slots. Physical bands match
 * adaptive presentation. Explicit0/1 retain their existing behavior. */
enum { VK_PRESENT_SLOTS_CONFIG_OFF, VK_PRESENT_SLOTS_CONFIG_FIXED, VK_PRESENT_SLOTS_CONFIG_AUTO40 };
static inline int vk_present_slots_config(const char *value) {
    return value && !strcmp(value, "1") ? VK_PRESENT_SLOTS_CONFIG_FIXED :
        value && !strcmp(value, "auto") ? VK_PRESENT_SLOTS_CONFIG_AUTO40 : VK_PRESENT_SLOTS_CONFIG_OFF;
}

/* One full requested game period of initial lead. Thereafter arrival jitter
 * never resets phase. A reservation up to one game period late is retained so
 * a short following frame can catch up; a longer stall skips directly to the
 * first nonpast slot on that SAME game cadence, without a catch-up loop.
 * Interval changes advance by the new period. A refresh change uses the new
 * callback lattice, at least one new period beyond the last reserved target:
 * already queued future targets can never be overtaken by a mode change.
 * Disabled/unavailable snapshots leave state untouched and return legacy now.
 * Deadlines must fit Android's signed int64_t desired-present-time API. */
static inline vk_present_slot vk_present_slots_reserve(vk_present_slots *p, int enabled,
        uint64_t callback_ns, uint64_t refresh_ns, uint32_t interval, uint64_t now_ns) {
    vk_present_slot r = {.desired_ns = now_ns, .mode = enabled ? VK_PRESENT_SLOTS_NO_CLOCK : VK_PRESENT_SLOTS_OFF};
    if (!enabled) return r;
    if (!interval) interval = 1;
    if (!callback_ns || !refresh_ns || now_ns > INT64_MAX ||
        refresh_ns > (uint64_t)INT64_MAX / interval) return r;
    uint64_t period = refresh_ns * interval;
    if (callback_ns > (uint64_t)INT64_MAX - period ||
        p->desired_ns > (uint64_t)INT64_MAX - period) return r;
    uint64_t target = callback_ns + period;
    if (p->desired_ns) {
        uint64_t next = p->desired_ns + period;
        if (p->refresh_ns == refresh_ns) target = next;
        else if (target < next) {
            uint64_t delta = next - target;
            uint64_t steps = delta / refresh_ns + (delta % refresh_ns != 0);
            if (steps > ((uint64_t)INT64_MAX - target) / refresh_ns) return r;
            target += steps * refresh_ns;
        }
    }
    uint64_t skipped = 0;
    if (now_ns > target && now_ns - target > period) {
        uint64_t delta = now_ns - target;
        skipped = delta / period + (delta % period != 0);
        if (skipped > ((uint64_t)INT64_MAX - target) / period) return r;
        target += skipped * period;
    }
    *p = (vk_present_slots){.desired_ns = target, .refresh_ns = refresh_ns};
    return (vk_present_slot){.desired_ns = target, .copy_open_ns = target,
        .refresh_ns = refresh_ns, .skipped = skipped, .interval = interval, .mode = VK_PRESENT_SLOTS_ACTIVE};
}

/* Dropping out of auto slots clears phase, not the queued-target floor. The
 * 40fps gameplay period only slots on120Hz; its scripted-camera30fps floor
 * slots on either120Hz (four refreshes) or60Hz (two). Other pictures retain
 * interval/refresh/copy-open zero without nearest-refresh rounding. A pending
 * future target is crossed monotonically; re-entry reanchors beyond it. */
static inline vk_present_slot vk_present_slots_auto40_reserve(vk_present_slots *p,
        double requested_period, uint64_t callback_ns, uint64_t refresh_ns, uint64_t now_ns) {
    vk_present_slot r;
    unsigned interval = 0;
    int cutscene30 = requested_period == (1000000000ull / 30) / 1e9 || requested_period == 1.0 / 30.0;
    if (refresh_ns >= 8000000 && refresh_ns <= 8650000)
        interval = requested_period == 1.0 / 40.0 ? 3 : cutscene30 ? 4 : 0;
    else if (cutscene30 && refresh_ns >= 16000000 && refresh_ns <= 17300000)
        interval = 2;
    if (interval) {
        r = vk_present_slots_reserve(p, 1, callback_ns, refresh_ns, interval, now_ns);
        if (r.mode == VK_PRESENT_SLOTS_ACTIVE) return r;
    }
    r = (vk_present_slot){.desired_ns = now_ns,
        .mode = callback_ns && refresh_ns ? VK_PRESENT_SLOTS_OFF : VK_PRESENT_SLOTS_NO_CLOCK};
    if (p->desired_ns >= now_ns && p->desired_ns < (uint64_t)INT64_MAX)
        r.desired_ns = p->desired_ns + 1;
    p->desired_ns = r.desired_ns;
    p->refresh_ns = 0;
    return r;
}

/* Interpolation retains the current real picture until its successor exists.
 * Only the real producer reserves slots: releasing N at N+1's reservation
 * adds exactly one real-frame latency without advancing the real lattice.
 * Presenter indices stay owned/busy while retained; each is released once.
 * Initialize retained=-1. A nonadjacent frame or changed surface/clock still
 * releases the old real picture, but must not publish a midpoint for that pair. */
typedef struct {
    int retained;
    uint64_t frame, generation;
    vk_present_slot slot;
} vk_present_interp;
typedef enum {
    VK_INTERP_READY, VK_INTERP_WARMUP, VK_INTERP_ADJACENCY, VK_INTERP_CLOCK,
    VK_INTERP_INTERVAL, VK_INTERP_REFRESH, VK_INTERP_LATE, VK_INTERP_SLOT,
    VK_INTERP_REASONS
} vk_present_interp_reason;
typedef struct {
    int real;
    vk_present_slot slot;
    uint64_t midpoint_ns;
    vk_present_interp_reason reason;
} vk_present_interp_pair;

static inline vk_present_interp_pair vk_present_interp_push(vk_present_interp *p,
        int current, uint64_t frame, uint64_t generation, vk_present_slot slot, uint64_t now_ns) {
    vk_present_interp_pair pair = {.real = p->retained, .slot = slot};
    if (p->retained < 0) pair.reason=VK_INTERP_WARMUP;
    else if (p->generation != generation || frame <= p->frame || frame-p->frame != 1)
        pair.reason=VK_INTERP_ADJACENCY;
    else if (p->slot.mode != VK_PRESENT_SLOTS_ACTIVE || slot.mode != VK_PRESENT_SLOTS_ACTIVE)
        pair.reason=VK_INTERP_CLOCK;
    else if (p->slot.interval != 2 || slot.interval != 2) pair.reason=VK_INTERP_INTERVAL;
    else if (p->slot.refresh_ns != slot.refresh_ns || slot.refresh_ns < 16000000 || slot.refresh_ns > 17300000)
        pair.reason=VK_INTERP_REFRESH;
    else if (slot.desired_ns <= p->slot.desired_ns || slot.desired_ns > (uint64_t)INT64_MAX-slot.refresh_ns)
        pair.reason=VK_INTERP_SLOT;
    /* A late real reservation is not a late child: the delayed older real
     * precedes this midpoint by one refresh, even when N arrives after it. */
    else if (now_ns >= slot.desired_ns + slot.refresh_ns) pair.reason=VK_INTERP_LATE;
    else pair.midpoint_ns=slot.desired_ns+slot.refresh_ns;
    *p = (vk_present_interp){.retained = current, .frame = frame, .generation = generation, .slot = slot};
    return pair;
}
/* Apply a fixed buffered lead after push has made the real-pair decision.
 * Only READY/LATE pairs can produce a child; all other rejection reasons stay
 * rejected, although a valid real reservation still advances with the delay.
 * The delayed real and its copy-open gate move by whole refreshes; its child
 * remains one refresh later. */
static inline vk_present_interp_pair vk_present_interp_buffer(vk_present_interp_pair pair,
        unsigned extra_refreshes, uint64_t now_ns) {
    if (!extra_refreshes) return pair;
    vk_present_slot *slot = &pair.slot;
    if (slot->mode != VK_PRESENT_SLOTS_ACTIVE || !slot->refresh_ns ||
        slot->desired_ns > INT64_MAX || slot->copy_open_ns > INT64_MAX) return pair;
    uint64_t limit = (uint64_t)INT64_MAX;
    if (extra_refreshes > (limit - slot->desired_ns) / slot->refresh_ns ||
        extra_refreshes > (limit - slot->copy_open_ns) / slot->refresh_ns) {
        pair.midpoint_ns = 0;
        pair.reason = VK_INTERP_SLOT;
        return pair;
    }
    uint64_t delay = (uint64_t)extra_refreshes * slot->refresh_ns;
    slot->desired_ns += delay;
    slot->copy_open_ns += delay;
    if (pair.reason != VK_INTERP_READY && pair.reason != VK_INTERP_LATE) {
        pair.midpoint_ns = 0;
        return pair;
    }
    if (slot->refresh_ns > limit - slot->desired_ns) {
        pair.midpoint_ns = 0;
        pair.reason = VK_INTERP_SLOT;
        return pair;
    }
    pair.midpoint_ns = slot->desired_ns + slot->refresh_ns;
    pair.reason = now_ns >= pair.midpoint_ns ? VK_INTERP_LATE : VK_INTERP_READY;
    if (pair.reason == VK_INTERP_LATE) pair.midpoint_ns = 0;
    return pair;
}



/* A native return retires this never-shown real through the presenter FIFO.
 * Its preceding child still owns slot+refresh. Never display the retained
 * original target (which would reverse time after that child). */
static inline uint64_t vk_present_interp_child_floor(const vk_present_interp *p) {
    return p->retained>=0 && p->slot.refresh_ns &&
        p->slot.desired_ns<=(uint64_t)INT64_MAX-p->slot.refresh_ns ?
        p->slot.desired_ns+p->slot.refresh_ns : 0;
}
static inline int vk_present_interp_leave(vk_present_interp *p) {
    int retained=p->retained;
    *p=(vk_present_interp){.retained=-1};
    return retained;
}

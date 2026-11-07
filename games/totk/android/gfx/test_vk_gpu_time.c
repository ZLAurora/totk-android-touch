#include "vk_gpu_time.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static vk_gpu_time_slot begin(vk_gpu_time_queue *queue, uint64_t guest_ns,
                              uint64_t raw_start_ns, uint64_t raw_end_ns) {
    return vk_gpu_time_begin(queue, guest_ns, raw_start_ns, raw_end_ns);
}

static uint64_t report(vk_gpu_time_queue *queue, const vk_gpu_time_slot *slot,
                       uint64_t raw_ns) {
    return vk_gpu_time_report(queue, slot, raw_ns, 0);
}

static void replay_delay_does_not_change_guest_timing(void) {
    vk_gpu_time_queue early = {0}, delayed = {0};
    vk_gpu_time_slot early_a = begin(&early, 1000, 10000, 10030);
    vk_gpu_time_slot early_b = begin(&early, 1040, 10040, 10060);
    vk_gpu_time_slot delayed_a = begin(&delayed, 1000, 900000, 900030);
    vk_gpu_time_slot delayed_b = begin(&delayed, 1040, 930000, 930020);

    assert(early_a.start_ns == 1000 && early_a.end_ns == 1030);
    assert(delayed_a.start_ns == early_a.start_ns && delayed_a.end_ns == early_a.end_ns);
    assert(early_b.start_ns == 1040 && early_b.end_ns == 1060);
    assert(delayed_b.start_ns == early_b.start_ns && delayed_b.end_ns == early_b.end_ns);
    assert(report(&early, &early_a, 10030) == report(&delayed, &delayed_a, 900030));
    assert(report(&early, &early_b, 10060) == report(&delayed, &delayed_b, 930020));
}

static void reports_are_monotonic_and_not_before_submit(void) {
    vk_gpu_time_queue queue = {0};
    vk_gpu_time_slot slot = begin(&queue, 1000, 5000, 5100);

    /* Deliberately publish the end query before the top query. */
    assert(report(&queue, &slot, 5100) == 1100);
    assert(report(&queue, &slot, 5000) == 1100);

    vk_gpu_time_slot later = begin(&queue, 1200, 5200, 5220);
    assert(report(&queue, &later, 5200) == 1200);
    assert(report(&queue, &later, 5210) == 1210);
    assert(report(&queue, &later, 5205) == 1210);
}

static void chunks_accumulate_gpu_work_not_host_gaps(void) {
    vk_gpu_time_queue queue = {0};
    const uint64_t guest_submit_ns = 2000;
    vk_gpu_time_slot chunk0 = begin(&queue, guest_submit_ns, 10000, 10010);
    /* The second chunk executes much later on the host, after a large recording
       or scheduling gap. Its ten nanoseconds are GPU work; the gap is not. */
    vk_gpu_time_slot chunk1 = begin(&queue, guest_submit_ns, 50000, 50030);

    assert(chunk0.start_ns == 2000 && chunk0.end_ns == 2010);
    assert(chunk1.start_ns == 2010 && chunk1.end_ns == 2040);
    assert(report(&queue, &chunk0, 10010) == 2010);
    assert(report(&queue, &chunk1, 50030) == 2040);
}

static void saved_slot_survives_delayed_publication_and_frame_lag(void) {
    vk_gpu_time_queue queue = {0};
    /* Slots are retained by submit/frame owner, not inferred from the frame
       currently being presented when the query result becomes available. */
    vk_gpu_time_slot frame10_owner = begin(&queue, 3000, 7000, 7020);
    vk_gpu_time_slot frame11_owner = begin(&queue, 3040, 9000, 9030);
    vk_gpu_time_slot frame12_owner = begin(&queue, 3100, 12000, 12010);

    /* Frame 10's result arrives while frame 12 is current; use frame 10's saved
       raw origin and guest submit context, not frame 12's context. */
    assert(report(&queue, &frame10_owner, 7010) == 3010);
    assert(report(&queue, &frame11_owner, 9030) == 3070);
    assert(report(&queue, &frame12_owner, 12010) == 3110);
}

static void guest_idle_gaps_and_gpu_backlog_are_preserved(void) {
    vk_gpu_time_queue queue = {0};
    vk_gpu_time_slot first = begin(&queue, 4000, 1000, 1020);
    /* An actual guest idle interval remains idle in the guest clock domain. */
    vk_gpu_time_slot after_idle = begin(&queue, 4100, 2000, 2010);
    /* A later guest submit arrives while this GPU queue is still backlogged. */
    vk_gpu_time_slot backlogged = begin(&queue, 4105, 3000, 3040);

    assert(first.start_ns == 4000 && first.end_ns == 4020);
    assert(after_idle.start_ns == 4100 && after_idle.end_ns == 4110);
    assert(backlogged.start_ns == 4110 && backlogged.end_ns == 4150);
    assert(report(&queue, &backlogged, 3040) == 4150);
}
static void sustained_gpu_bound_submits_keep_accumulated_backlog(void) {
    vk_gpu_time_queue queue = {0};
    for (uint64_t frame = 0; frame < 120; frame++) {
        uint64_t guest = 1000000 + frame * 16000000;
        uint64_t raw_start = 9000000000 + frame * 40000000;
        vk_gpu_time_slot slot = begin(&queue, guest, raw_start, raw_start + 20000000);
        uint64_t end = report(&queue, &slot, raw_start + 20000000);
        assert(end - guest == 20000000 + frame * 4000000);
        assert(end - guest > 16666667); /* GPU-bound cannot be made to look 60-fps sustainable. */
    }
}
static void overlap_is_not_double_counted_and_backlog_still_accumulates(void) {
    vk_gpu_time_queue overlap = {0};
    vk_gpu_time_slot first = begin(&overlap, 1000, 10000, 10040);
    vk_gpu_time_slot second = begin(&overlap, 1000, 10020, 10060);
    assert(first.end_ns == 1040 && second.end_ns == 1060); /* union is 60, not 80 */
    assert(report(&overlap, &first, 10040) == 1040);
    assert(report(&overlap, &second, 10060) == 1060);
    vk_gpu_time_slot backend = begin(&overlap, 0, 900000, 900010);
    assert(backend.start_ns == 1060 && backend.end_ns == 1070); /* no backend host-time floor */
    vk_gpu_time_queue busy = {0};
    for (uint64_t frame = 0; frame < 120; frame++) {
        uint64_t guest = 1000000 + frame * 16000000;
        uint64_t raw_start = 9000000000 + frame * 16000000;
        uint64_t raw_end = 9000000000 + (frame + 1) * 20000000;
        vk_gpu_time_slot slot = begin(&busy, guest, raw_start, raw_end);
        assert(report(&busy, &slot, raw_end) - guest == 20000000 + frame * 4000000);
    }
}

static void interleaved_guest_queues_share_actual_queue_order(void) {
    vk_gpu_time_queue actual_vk_queue = {0};
    /* Logical queues A/B are interleaved, but submissions are serialized on
       this one actual Vulkan queue. B depends on A; A's next submit follows B. */
    vk_gpu_time_slot a0 = begin(&actual_vk_queue, 5000, 10000, 10020);
    vk_gpu_time_slot b0 = begin(&actual_vk_queue, 5005, 20000, 20030);
    vk_gpu_time_slot a1 = begin(&actual_vk_queue, 5010, 30000, 30010);

    assert(a0.start_ns == 5000 && a0.end_ns == 5020);
    assert(b0.start_ns == 5020 && b0.end_ns == 5050);
    assert(a1.start_ns == 5050 && a1.end_ns == 5060);
    assert(b0.start_ns >= a0.end_ns);
    assert(a1.start_ns >= b0.end_ns);
    uint64_t drs_start = report(&actual_vk_queue, &a0, 10000);
    (void)report(&actual_vk_queue, &b0, 20030);
    uint64_t drs_end = report(&actual_vk_queue, &a1, 30010);
    assert(drs_end >= drs_start && drs_end - drs_start == 60); /* unsigned DRS pair cannot wrap */
}

static void latency_mode_returns_raw_query_unchanged(void) {
    vk_gpu_time_queue queue = {0};
    vk_gpu_time_slot slot = begin(&queue, 6000, 1000, 1010);
    assert(vk_gpu_time_report(&queue, &slot, 987654321, 1) == 987654321);
}

static void lagged_controller_subtracts_the_report_owners_origin(void) {
    vk_gpu_time_samples samples = {0};
    const uint64_t addr = 0x100010, producer = 19200000, work = 320000;
    const uint64_t end = producer + work, next_origin = end + 309648;
    int valid, rebased;
    vk_gpu_time_sample_publish(&samples, addr, end, producer);
    /* Thor frame 3289's observed -309648 ticks (-16.128 ms): a completed
       virtual END precedes the next live CPU origin, despite valid owner work.
       Frame lag replaces the origin BEFORE the old report is consumed. */
    assert((int64_t)(end - next_origin) == -309648);
    uint64_t transported = vk_gpu_time_sample_consume(&samples, addr, end, next_origin, &valid, &rebased);
    assert(valid && rebased && transported - next_origin == work);
    assert(transported >= next_origin); /* not a clamp to zero */
    /* Repeated reads and current-frame reads retain exactly the same work. */
    assert(vk_gpu_time_sample_consume(&samples, addr, end, producer, &valid, &rebased) - producer == work);
    assert(valid && !rebased);
    for (uint64_t duration = 100000; duration <= 800000; duration += 100000) {
        vk_gpu_time_sample_publish(&samples, addr, producer + duration, producer);
        transported = vk_gpu_time_sample_consume(&samples, addr, producer + duration, next_origin, &valid, &rebased);
        assert(valid && transported - next_origin == duration); /* both sides of controller thresholds */
    }
}

static void missing_invalid_and_evicted_reports_hold_the_last_completed_input(void) {
    vk_gpu_time_samples samples = {0};
    const uint64_t addr = 0x200010, origin = 19200000, work = 700000, end = origin + work;
    int valid, rebased;
    uint64_t current = origin + 2000000;
    /* Bootstrap retains the game's initialized zero; later holds retain the
       last real measurement, including an overloaded (>33ms) one. */
    assert(vk_gpu_time_sample_consume(&samples, addr, end, current, &valid, &rebased) == current);
    assert(!valid);
    vk_gpu_time_sample_publish(&samples, addr, end, origin);
    assert(vk_gpu_time_sample_consume(&samples, addr, end, current, &valid, &rebased) - current == work);
    assert(valid);
    vk_gpu_time_sample_publish(&samples, addr, end + 1, origin); /* same address reused */
    assert(vk_gpu_time_sample_consume(&samples, addr, end, current, &valid, &rebased) - current == work);
    assert(!valid); /* never pair an old timestamp with a new owner's origin */
    vk_gpu_time_sample_publish(&samples, addr, end, end + 1);
    assert(vk_gpu_time_sample_consume(&samples, addr, end, current, &valid, &rebased) - current == work);
    assert(!valid); /* genuinely negative producer duration is withheld */
    vk_gpu_time_sample_publish(&samples, addr, end, 0);
    assert(vk_gpu_time_sample_consume(&samples, addr, end, current, &valid, &rebased) - current == work);
    assert(!valid); /* unavailable query/unknown producer origin */
    uint64_t colliding[VK_GPU_TIME_SAMPLE_WAYS + 1]; unsigned count = 0;
    for (uint64_t a = 16; count < VK_GPU_TIME_SAMPLE_WAYS + 1; a += 16)
        if (vk_gpu_time_sample_bucket(a) == vk_gpu_time_sample_bucket(addr)) colliding[count++] = a;
    for (unsigned i = 0; i < count; i++) vk_gpu_time_sample_publish(&samples, colliding[i], end + i, origin);
    assert(vk_gpu_time_sample_consume(&samples, addr, end, current, &valid, &rebased) - current == work);
    assert(!valid); /* bounded-cache eviction is observable withholding, not stale provenance */
}

static void fps_median_is_opt_in_and_preserves_sustained_work(void) {
    assert(!vk_gpu_time_fps_median_enabled(NULL));
    const char *off[] = {"", "0", "2", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++)
        assert(!vk_gpu_time_fps_median_enabled(off[i]));
    assert(vk_gpu_time_fps_median_enabled("1"));
    vk_gpu_time_fps_filter f = {0};
    const uint64_t addr = 0x1000, low = 12 * 19200, high = 40 * 19200;
    assert(vk_gpu_time_fps_median(&f, addr, 0, low, 0) == 0);
    assert(vk_gpu_time_fps_median(&f, addr, 1, high, 1) == high);
    assert(vk_gpu_time_fps_median(&f, addr, 2, low, 1) == low);
    assert(vk_gpu_time_fps_median(&f, addr, 3, low, 1) == low);
    assert(vk_gpu_time_fps_median(&f, addr, 4, high, 1) == low); /* isolated spike */
    assert(vk_gpu_time_fps_median(&f, addr, 4, high, 1) == low); /* duplicate END */
    assert(vk_gpu_time_fps_median(&f, addr, 2, low, 1) == low); /* older selected END */
    assert(f.next == 1); /* interleaved rereads cannot fill the window */
    assert(f.count == 3 && f.next == 1);
    assert(vk_gpu_time_fps_median(&f, addr, 5, high, 1) == high); /* second overload */
    assert(vk_gpu_time_fps_median(&f, addr, 6, 0, 0) == high); /* invalid must not zero */
    assert(vk_gpu_time_fps_median(&f, addr, 6, high, 1) == high);
    assert(vk_gpu_time_fps_median(&f, addr, 7, low, 1) == high); /* recovery delay */
    assert(vk_gpu_time_fps_median(&f, addr, 8, low, 1) == low);
    /* Address is part of identity; equal timestamps in distinct reports count. */
    assert(vk_gpu_time_fps_median(&f, addr + 16, 8, high, 1) == low);
    assert(vk_gpu_time_fps_median(&f, addr + 32, 8, high, 1) == high);
    /* Median ordering and full tick range, without sum/overflow. */
    uint64_t perm[6][3] = {{0,1,UINT64_MAX},{0,UINT64_MAX,1},{1,0,UINT64_MAX},
        {1,UINT64_MAX,0},{UINT64_MAX,0,1},{UINT64_MAX,1,0}};
    for (unsigned i = 0; i < 6; i++) {
        vk_gpu_time_fps_filter p = {0};
        for (unsigned j = 0; j < 3; j++)
            vk_gpu_time_fps_median(&p, addr, j + 1, perm[i][j], 1);
        assert(p.output == 1);
    }
}

int main(void) {
    replay_delay_does_not_change_guest_timing();
    reports_are_monotonic_and_not_before_submit();
    chunks_accumulate_gpu_work_not_host_gaps();
    saved_slot_survives_delayed_publication_and_frame_lag();
    guest_idle_gaps_and_gpu_backlog_are_preserved();
    sustained_gpu_bound_submits_keep_accumulated_backlog();
    overlap_is_not_double_counted_and_backlog_still_accumulates();
    interleaved_guest_queues_share_actual_queue_order();
    latency_mode_returns_raw_query_unchanged();
    lagged_controller_subtracts_the_report_owners_origin();
    missing_invalid_and_evicted_reports_hold_the_last_completed_input();
    fps_median_is_opt_in_and_preserves_sustained_work();
    puts("vk_gpu_time tests passed");
    return 0;
}

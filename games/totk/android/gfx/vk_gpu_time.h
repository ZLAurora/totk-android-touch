#pragma once
#include <stdint.h>

/* All values are nanoseconds in the guest clock domain. This queue is owned by
   the ordered completion cursor, never by the walking or submit threads. */
typedef struct { uint64_t end_ns, report_ns, raw_end_ns; } vk_gpu_time_queue;
typedef struct {
    uint64_t guest_ns, raw_start_ns, raw_end_ns, start_ns, end_ns;
} vk_gpu_time_slot;

/* Commands in a guest submit already exist at guest_ns. Host recording gaps
   between its Vulkan chunks are not guest GPU work; actual execution of every
   chunk and backlog from earlier submissions are retained on a virtual queue. */
static inline vk_gpu_time_slot vk_gpu_time_begin(vk_gpu_time_queue *queue,
                                               uint64_t guest_ns,
                                               uint64_t raw_start_ns,
                                               uint64_t raw_end_ns) {
    if (raw_end_ns < raw_start_ns) raw_end_ns = raw_start_ns;
    uint64_t start = queue->end_ns;
    /* TOP/BOTTOM brackets may overlap: queued/pipelined GPU execution must not
       be charged twice merely because it appears in both slot intervals. */
    if (raw_start_ns < queue->raw_end_ns) {
        uint64_t overlap = queue->raw_end_ns - raw_start_ns;
        start = overlap < start ? start - overlap : 0;
    }
    if (start < guest_ns) start = guest_ns;
    vk_gpu_time_slot slot = {guest_ns, raw_start_ns, raw_end_ns, start,
                            start + (raw_end_ns - raw_start_ns)};
    if (queue->end_ns < slot.end_ns) queue->end_ns = slot.end_ns;
    if (queue->raw_end_ns < raw_end_ns) queue->raw_end_ns = raw_end_ns;
    return slot;
}

static inline uint64_t vk_gpu_time_report(vk_gpu_time_queue *queue,
                                         const vk_gpu_time_slot *slot,
                                         uint64_t raw_ns, int latency) {
    uint64_t report = raw_ns;
    if (!latency) {
        report = slot->start_ns + (raw_ns > slot->raw_start_ns ? raw_ns - slot->raw_start_ns : 0);
        if (report < slot->guest_ns) report = slot->guest_ns;
        if (report < queue->report_ns) report = queue->report_ns;
    }
    queue->report_ns = report;
    return report;
}

/* FPS listeners consume a lagged report but subtract the live CPU origin. Keep
   the report's producer origin separately from the guest-visible absolute clock.
   Access is serialized by the backend's existing completion mutex. A bounded
   four-way cache may evict a report: exact address AND timestamp matching makes
   that a withheld sample, never a duration from a different buffer/frame. */
enum { VK_GPU_TIME_SAMPLE_SETS = 256, VK_GPU_TIME_SAMPLE_WAYS = 4 };
typedef struct { uint64_t addr, timestamp, origin; } vk_gpu_time_sample;
typedef struct {
    vk_gpu_time_sample sample[VK_GPU_TIME_SAMPLE_SETS * VK_GPU_TIME_SAMPLE_WAYS];
    uint64_t last_work_ticks; /* game's initial completed GPU input is zero */
} vk_gpu_time_samples;

static inline unsigned vk_gpu_time_sample_bucket(uint64_t addr) {
    return (unsigned)(((addr >> 4) * UINT64_C(11400714819323198485)) >> 56) * VK_GPU_TIME_SAMPLE_WAYS;
}
static inline void vk_gpu_time_sample_publish(vk_gpu_time_samples *samples,
                                              uint64_t addr, uint64_t timestamp,
                                              uint64_t origin) {
    unsigned first = vk_gpu_time_sample_bucket(addr), target = first;
    for (unsigned i = 0; i < VK_GPU_TIME_SAMPLE_WAYS; i++) {
        unsigned n = first + i;
        if (samples->sample[n].addr == addr) { target = n; break; }
        if (!samples->sample[n].addr) target = n;
    }
    samples->sample[target] = (vk_gpu_time_sample){addr, timestamp, origin};
}
/* Return the producer frame's measured work, transported to the origin the
   listener is about to subtract. Missing/invalid samples retain the previous
   completed input, not zero, a latency sample, or a threshold-dependent value.
   Caller origins are ticks from the uint64 nanosecond guest clock, so adding a
   signed duration cannot overflow uint64 ticks. */
static inline uint64_t vk_gpu_time_sample_consume(vk_gpu_time_samples *samples,
                                                 uint64_t addr, uint64_t timestamp,
                                                 uint64_t consumer_origin,
                                                 int *valid, int *rebased) {
    unsigned first = vk_gpu_time_sample_bucket(addr);
    *valid = 0; *rebased = 0;
    for (unsigned i = 0; i < VK_GPU_TIME_SAMPLE_WAYS; i++) {
        const vk_gpu_time_sample *s = &samples->sample[first + i];
        if (s->addr != addr || s->timestamp != timestamp) continue;
        if (s->origin && timestamp >= s->origin && timestamp - s->origin <= INT64_MAX) {
            samples->last_work_ticks = timestamp - s->origin;
            *valid = 1; *rebased = s->origin != consumer_origin;
        }
        break;
    }
    return consumer_origin + samples->last_work_ticks;
}

/* Opt-in FPS-only median: absolute reports and DRS pairs remain untouched.
   Re-reading one completed END against a newer live origin is not a new
   measurement. Invalid reads hold the last filtered input, just as the raw
   consumer holds its last valid producer duration. No synthetic warmup zeros. */
typedef struct {
    uint64_t work[3], addr[3], timestamp[3], output;
    unsigned count, next;
} vk_gpu_time_fps_filter;

static inline int vk_gpu_time_fps_median_enabled(const char *value) {
    return value && value[0] == '1' && value[1] == '\0';
}

static inline uint64_t vk_gpu_time_fps_median(vk_gpu_time_fps_filter *filter,
                                             uint64_t addr, uint64_t timestamp,
                                             uint64_t work, int valid) {
    if (!valid) return filter->output;
    for (unsigned i = 0; i < filter->count; i++)
        if (filter->addr[i] == addr && filter->timestamp[i] == timestamp)
            return filter->output;
    filter->addr[filter->next] = addr; filter->timestamp[filter->next] = timestamp;
    filter->work[filter->next] = work;
    filter->next = (filter->next + 1) % 3;
    if (filter->count < 3) filter->count++;
    filter->output = work;
    if (filter->count == 3) {
        uint64_t a = filter->work[0], b = filter->work[1], c = filter->work[2];
        if (a > b) { uint64_t t = a; a = b; b = t; }
        if (b > c) b = c;
        filter->output = a > b ? a : b;
    }
    return filter->output;
}

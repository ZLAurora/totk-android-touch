#pragma once
/* Profiling timestamps are comparable only within one captured queue/time domain.
 * Availability is authoritative: a zero-valued available counter is legal,
 * especially at a real wrap. A backwards value is not automatically a wrap. */
#include <stdint.h>
#include <math.h>

#define VK_GPUPROF_MAX_FRAME_MS 10000.0

typedef struct { uint64_t ticks, available; } vk_gpuprof_timestamp;
typedef struct {
    uintptr_t device, queue;
    uint32_t queue_family, valid_bits;
    double period_ns;
} vk_gpuprof_time_domain;
typedef enum {
    GP_TIME_OK, GP_TIME_UNAVAILABLE, GP_TIME_QUERY_FAILED, GP_TIME_DOMAIN,
    GP_TIME_IMPLAUSIBLE, GP_TIME_MISSING, GP_TIME_INCOMPLETE, GP_TIME_NEGATIVE_GAP
} vk_gpuprof_time_status;
typedef struct {
    vk_gpuprof_time_status status;
    double ms;
    int has_value, wrapped;
} vk_gpuprof_interval;

static inline int vk_gpuprof_same_domain(vk_gpuprof_time_domain a, vk_gpuprof_time_domain b) {
    return a.device == b.device && a.queue == b.queue && a.queue_family == b.queue_family &&
        a.valid_bits == b.valid_bits && a.period_ns == b.period_ns;
}
static inline uint64_t vk_gpuprof_tick_mask(uint32_t bits) {
    return bits == 64 ? UINT64_MAX : bits ? (UINT64_C(1) << bits) - 1 : 0;
}
static inline vk_gpuprof_interval vk_gpuprof_interval_check(vk_gpuprof_timestamp first,
        vk_gpuprof_timestamp last, vk_gpuprof_time_domain a, vk_gpuprof_time_domain b) {
    vk_gpuprof_interval result = {.status = GP_TIME_UNAVAILABLE};
    if (!first.available || !last.available) return result;
    result.status = GP_TIME_DOMAIN;
    if (!vk_gpuprof_same_domain(a, b) || !a.valid_bits || a.valid_bits > 64 ||
        !isfinite(a.period_ns) || a.period_ns <= 0) return result;
    uint64_t mask = vk_gpuprof_tick_mask(a.valid_bits);
    uint64_t start = first.ticks & mask, end = last.ticks & mask;
    result.ms = (double)((end - start) & mask) * a.period_ns / 1e6;
    result.has_value = 1; result.wrapped = end < start;
    result.status = isfinite(result.ms) && result.ms <= VK_GPUPROF_MAX_FRAME_MS ? GP_TIME_OK : GP_TIME_IMPLAUSIBLE;
    return result;
}
static inline const char *vk_gpuprof_time_reason(vk_gpuprof_time_status status) {
    switch (status) {
    case GP_TIME_OK: return "ok";
    case GP_TIME_UNAVAILABLE: return "unavailable-timestamp";
    case GP_TIME_QUERY_FAILED: return "query-read-failed";
    case GP_TIME_DOMAIN: return "timestamp-domain-mismatch";
    case GP_TIME_IMPLAUSIBLE: return "implausible-timestamp";
    case GP_TIME_MISSING: return "missing-endpoints";
    case GP_TIME_INCOMPLETE: return "incomplete-records";
    case GP_TIME_NEGATIVE_GAP: return "negative-gap";
    }
    return "invalid-timestamp";
}

#pragma once
#include <stdint.h>
#if defined(__ANDROID__)
/* Startup names register candidates; only busy game workers and named critical
 * pipeline threads join the opt-in session. No scheduler settings are changed. */
int totk_perf_full(void);
void totk_perf_thread(const char *name);
void totk_perf_render(uint64_t interval_ns);
/* Once per guest present; guest_interval is the game's swap interval in 60-Hz units (TOTK_RATE_HINT_HOLD). */
void totk_perf_frame(uint64_t now_ns, uint64_t period_ns, uint64_t pacing_ns, int guest_interval);
/* TOTK_ADPF_GPU (live): the frame's first guest submit with the game's frame origin (guest ticks, 0 = none), the
 * acquire pacing waits (CLOCK_MONOTONIC ns), and each completed Vulkan slot's GPU time with its guest frame (the
 * completion thread, in submission order). */
void totk_perf_submit(uint64_t origin_ticks);
int totk_perf_span(void);
void totk_perf_pacing(uint64_t begin_ns, uint64_t end_ns);
void totk_perf_gpu(uint64_t guest_frame, uint64_t gpu_ns);
/* Bracket native present/texture queue waits only; begin returns 0 when CPU splitting is off. */
uint64_t totk_perf_wait_begin(void);
void totk_perf_wait_end(uint64_t begin_ns);
struct ANativeWindow;
struct ASurfaceTransaction;
struct ASurfaceControl;
void totk_perf_window(struct ANativeWindow *window);
void totk_perf_surface(struct ASurfaceTransaction *tx, struct ASurfaceControl *surface);
float totk_perf_rate(void);
/* Startup override: request UINT32_MAX means highest advertised rate; frontend validates before publishing.
 * Fractional display rates remain exact floats. 0 restores the existing content-rate policy. */
unsigned totk_perf_rate_request(void);
void totk_perf_rate_override(float hz);
void totk_perf_generated(void);   /* the presenter queued a generated frame (frame generation is running) */
#else
static inline void totk_perf_thread(const char *name) { (void)name; }
static inline void totk_perf_render(uint64_t ns) { (void)ns; }
static inline int totk_perf_full(void) { return 0; }
static inline void totk_perf_frame(uint64_t now, uint64_t period, uint64_t pacing, int guest_interval) { (void)now; (void)period; (void)pacing; (void)guest_interval; }
static inline void totk_perf_submit(uint64_t origin_ticks) { (void)origin_ticks; }
static inline int totk_perf_span(void) { return 0; }
static inline void totk_perf_pacing(uint64_t begin, uint64_t end) { (void)begin; (void)end; }
static inline void totk_perf_gpu(uint64_t frame, uint64_t gpu_ns) { (void)frame; (void)gpu_ns; }
static inline uint64_t totk_perf_wait_begin(void) { return 0; }
static inline void totk_perf_wait_end(uint64_t begin) { (void)begin; }
#endif

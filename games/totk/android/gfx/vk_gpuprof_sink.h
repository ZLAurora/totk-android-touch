/* Compact-output byte sink. The lease consumer never waits for file writes:
 * a full fixed byte FIFO is explicit output loss, not hidden query-lease loss.
 * Callers serialize writes/rotation/close under output_mu. Rotation markers
 * share the FIFO's byte sequence; only terminal/test drains wait for IO. */
#ifndef VK_GPUPROF_SINK_H
#define VK_GPUPROF_SINK_H
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#if defined(__linux__)
#include <sys/resource.h>
#endif
#define GP_SINK_BYTES (1u << 20)
#define GP_SINK_STACK_BYTES (256u << 10)
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    FILE *file;
    uint64_t head, tail, flush_requested, flush_done, peak, overflows;
    uint64_t rotation[64], rotation_head, rotation_tail;
    int started, stop, failed;
#ifdef TOTK_VK_TESTS
    unsigned stall_ms;
    uint64_t stall_after;
    int stalled, paused;
#endif
    unsigned char bytes[GP_SINK_BYTES];
} gp_sink;
static gp_sink output_sink = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
static void *gp_sink_main(void *unused) {
    (void)unused;
    totk_thread_start("totk-prof-io", NULL);
#if defined(__linux__)
    /* Blocking storage does not own query leases and need not compete with the
       game. Never change the inherited affinity. */
    (void)setpriority(PRIO_PROCESS, 0, 10);
#endif
    gp_sink *s = &output_sink;
    pthread_mutex_lock(&s->mu);
    for (;;) {
#ifdef TOTK_VK_TESTS
        while (s->paused && !s->stop) pthread_cond_wait(&s->cv, &s->mu);
#endif
        while (s->head == s->tail && s->rotation_head == s->rotation_tail &&
               s->flush_done == s->flush_requested && !s->stop)
            pthread_cond_wait(&s->cv, &s->mu);
        if (s->rotation_head != s->rotation_tail &&
            s->head == s->rotation[s->rotation_head % 64]) {
            FILE *file = s->file;
            int failed = s->failed;
            pthread_mutex_unlock(&s->mu);
            if (!failed) file = output_rotate_file(file);
            pthread_mutex_lock(&s->mu);
            s->file = file; s->rotation_head++;
            if (!file && !s->failed) {
                s->failed = 1; atomic_fetch_add_explicit(&output_failures, 1, memory_order_relaxed);
            }
            continue;
        }
        if (s->head != s->tail) {
            size_t at = s->head & (GP_SINK_BYTES - 1);
            size_t n = (size_t)(s->tail - s->head);
            if (n > GP_SINK_BYTES - at) n = GP_SINK_BYTES - at;
            if (s->rotation_head != s->rotation_tail) {
                uint64_t until_rotation = s->rotation[s->rotation_head % 64] - s->head;
                if (n > until_rotation) n = until_rotation;
            }
            int failed = s->failed;
#ifdef TOTK_VK_TESTS
            unsigned stall = 0;
            if (!s->stalled && s->stall_ms && s->head + n >= s->stall_after) {
                s->stalled = 1; stall = s->stall_ms;
            }
#endif
            pthread_mutex_unlock(&s->mu);
#ifdef TOTK_VK_TESTS
            if (stall) {
                struct timespec delay = {stall / 1000, (long)(stall % 1000) * 1000000};
                while (nanosleep(&delay, &delay) && errno == EINTR) {}
            }
#endif
            size_t written = failed ? n : fwrite(s->bytes + at, 1, n, s->file);
            pthread_mutex_lock(&s->mu);
            if (written != n && !s->failed) {
                s->failed = 1; atomic_fetch_add_explicit(&output_failures, 1, memory_order_relaxed);
            }
            s->head += n;
            pthread_cond_broadcast(&s->cv);
            continue;
        }
        if (s->flush_done != s->flush_requested) {
            uint64_t target = s->flush_requested;
            pthread_mutex_unlock(&s->mu);
            int error = s->file ? fflush(s->file) : 0;
            pthread_mutex_lock(&s->mu);
            if (error && !s->failed) {
                s->failed = 1; atomic_fetch_add_explicit(&output_failures, 1, memory_order_relaxed);
            }
            s->flush_done = target;
            pthread_cond_broadcast(&s->cv);
            continue;
        }
        if (s->stop) break;
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}
static void gp_sink_start(FILE *file) {
    gp_sink *s = &output_sink;
    s->file = file;
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    if (error) DIE("GPU profiler output thread attributes failed: %d", error);
    error = pthread_attr_setstacksize(&attr, GP_SINK_STACK_BYTES);
    if (!error) error = pthread_create(&s->thread, &attr, gp_sink_main, NULL);
    pthread_attr_destroy(&attr);
    if (error) DIE("GPU profiler output thread creation failed: %d", error);
    s->started = 1;
}
static int gp_sink_write(const char *text, size_t n) {
    gp_sink *s = &output_sink;
    pthread_mutex_lock(&s->mu);
    if (s->failed || n > GP_SINK_BYTES - (s->tail - s->head)) {
        if (!s->failed) s->overflows++;
        pthread_mutex_unlock(&s->mu);
        return 0;
    }
    size_t at = s->tail & (GP_SINK_BYTES - 1), first = GP_SINK_BYTES - at;
    if (first > n) first = n;
    memcpy(s->bytes + at, text, first);
    memcpy(s->bytes, text + first, n - first);
    int wake = s->tail == s->head;
    s->tail += n;
    if (s->tail - s->head > s->peak) s->peak = s->tail - s->head;
    if (wake) pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return 1;
}
static void gp_sink_flush(int wait) {
    gp_sink *s = &output_sink;
    if (!s->started) return;
    pthread_mutex_lock(&s->mu);
    uint64_t target = s->flush_requested = s->tail;
    pthread_cond_signal(&s->cv);
    while (wait && (s->head != target || s->flush_done != target))
        pthread_cond_wait(&s->cv, &s->mu);
    pthread_mutex_unlock(&s->mu);
}
static int gp_sink_rotate(void) {
    gp_sink *s = &output_sink;
    pthread_mutex_lock(&s->mu);
    if (s->failed || s->rotation_tail - s->rotation_head == 64) {
        if (!s->failed) s->overflows++;
        pthread_mutex_unlock(&s->mu);
        return 0;
    }
    s->rotation[s->rotation_tail++ % 64] = s->tail;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return 1;
}
static void gp_sink_stop(void) {
    gp_sink *s = &output_sink;
    if (!s->started) return;
    gp_sink_flush(1);
    pthread_mutex_lock(&s->mu);
    s->stop = 1; pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    pthread_join(s->thread, NULL);
    s->started = 0;
}
#endif

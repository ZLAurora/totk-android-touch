/* Host probe: how late do short nanosleep() calls wake with the inherited timer slack and with
 * TOTK_TIMER_SLACK_NS=1 applied by the production thread-start helper? Numbers are host measurements only;
 * the checks pin the switch semantics (game/frame threads only, bounds, untouched when unset). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include "thread_place.h"

static int failures;
#define CHECK(c, text) do { if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", text); } } while (0)

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y;
}
enum { ROUNDS = 400, PASSES = 3, SIZES = 3 };
static const uint64_t targets[SIZES] = {10000, 100000, 1000000};
/* Lateness = wall time beyond the request, per arm and request size, over all alternating passes. */
typedef struct { const char *env; long slack; uint64_t late[SIZES][ROUNDS * PASSES]; unsigned n; } arm_t;
static void *arm_thread(void *arg) {
    arm_t *a = arg;
    long inherited = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    totk_thread_start("totk-submit", NULL);
    a->slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    if (!a->env) CHECK(a->slack == inherited, "unset switch leaves the inherited slack");
    for (unsigned k = 0; k < SIZES; k++) {
        struct timespec ts = {0, (long)targets[k]};
        for (unsigned i = 0; i < 20; i++) nanosleep(&ts, NULL);   /* warm-up */
        for (unsigned i = 0; i < ROUNDS; i++) {
            uint64_t start = now_ns(); nanosleep(&ts, NULL); uint64_t elapsed = now_ns() - start;
            a->late[k][a->n + i] = elapsed > targets[k] ? elapsed - targets[k] : 0;
        }
    }
    a->n += ROUNDS;
    return NULL;
}
typedef struct { const char *name, *value; long expected; } semantic_t;
static void *semantic_thread(void *arg) {
    semantic_t *s = arg; totk_thread_start(s->name, NULL);
    long slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    if (slack != s->expected) {
        failures++;
        fprintf(stderr, "FAIL: %s with TOTK_TIMER_SLACK_NS=%s: slack %ld, expected %ld\n", s->name, s->value, slack, s->expected);
    }
    return NULL;
}
static void join(void *(*fn)(void *), void *arg) {
    pthread_t t; CHECK(!pthread_create(&t, NULL, fn, arg), "create probe thread");
    CHECK(!pthread_join(t, NULL), "join probe thread");
}
static arm_t arms[2] = {{.env = NULL}, {.env = "1"}};
int main(void) {
    long base = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    /* Game and frame-critical threads only; out-of-range or malformed values change nothing. */
    semantic_t cases[] = {
        {"totk-submit", "1", 1}, {"totk-recw0", "1", 1}, {"totk-post", "1", 1}, {"totk-gpufence", "1", 1},
        {"totk-present", "1", 1}, {"totk-vsync", "1", 1}, {"totk-walk", "1", 1}, {"g-main", "1", 1},
        {"g-82", "50000", 50000}, {"totk-pipe0", "1", base}, {"totk-pcache", "1", base},
        {"totk-seed-io", "1", base}, {"totk-compute", "1", base}, {"totk-submit", "0", base},
        {"totk-submit", "50001", base}, {"totk-submit", "-1", base}, {"totk-submit", "", base},
        {"totk-submit", "1us", base},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        setenv("TOTK_TIMER_SLACK_NS", cases[i].value, 1); join(semantic_thread, &cases[i]);
    }
    CHECK(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) == base, "per-thread setting does not reach the creator");
    for (unsigned pass = 0; pass < PASSES; pass++)   /* alternate arms to spread host noise */
        for (unsigned i = 0; i < 2; i++) {
            if (arms[i].env) setenv("TOTK_TIMER_SLACK_NS", arms[i].env, 1); else unsetenv("TOTK_TIMER_SLACK_NS");
            join(arm_thread, &arms[i]);
        }
    printf("timer-slack probe: %u nanosleeps per size and arm (%u alternating passes); lateness beyond the request, us\n",
           ROUNDS * PASSES, PASSES);
    for (unsigned i = 0; i < 2; i++)
        for (unsigned k = 0; k < SIZES; k++) {
            uint64_t *l = arms[i].late[k]; unsigned n = arms[i].n; qsort(l, n, sizeof *l, cmp_u64);
            printf("  slack=%-6ld request=%5llu us  p50=%7.1f p90=%7.1f p99=%7.1f max=%8.1f\n", arms[i].slack,
                   (unsigned long long)(targets[k] / 1000), l[n / 2] / 1e3, l[n * 9 / 10] / 1e3, l[n * 99 / 100] / 1e3,
                   l[n - 1] / 1e3);
        }
    CHECK(arms[1].slack == 1, "frame thread measured with slack 1");
    printf("%s: timer slack semantics\n", failures ? "FAIL" : "ok");
    return failures != 0;
}

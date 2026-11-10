/* No game/Vulkan/device needed: exercise the real per-thread Linux scheduling helpers in short-lived threads. */
#define TOTK_THREAD_PLACE_TEST
#define TOTK_THREAD_PLACE_IMPLEMENTATION
#include "thread_place.h"
#include <stdatomic.h>
#include <sys/stat.h>
#ifdef __ANDROID__
/* This standalone scheduling fixture has no Android frontend/ADPF session. */
void totk_perf_thread(const char *name) { (void)name; }
#endif
static _Atomic unsigned checks, failures;
#define CHECK(c, text) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", text); } } while (0)
static cpu_set_t original;
static unsigned first_cpu, walk_cpu;
static int requested_nice;
static void check_name(const char *expected) {
    char name[16]; CHECK(!pthread_getname_np(pthread_self(), name, sizeof name) && !strcmp(name, expected), "kernel thread name");
}
static void check_mask(const cpu_set_t *expected) {
    cpu_set_t actual; CHECK(!sched_getaffinity(0, sizeof actual, &actual) && !memcmp(&actual, expected, sizeof actual), "resulting per-thread affinity");
}
static void *worker(void *arg) {
    unsigned phase = (unsigned)(uintptr_t)arg;
    int nice = getpriority(PRIO_PROCESS, 0), policy = sched_getscheduler(0), clamp = totk_thread_uclamp_get();
    if (!phase) {
        errno = EDOM; totk_thread_start("test-name-longer-than-15", NULL); CHECK(errno == EDOM, "name helper preserves errno");
        check_name("test-name-longe"); check_mask(&original);
        CHECK(getpriority(PRIO_PROCESS, 0) == nice && sched_getscheduler(0) == policy, "default keeps nice/scheduler");
        CHECK(totk_thread_uclamp_get() == clamp, "default keeps uclamp");
    } else if (phase == 1) {
        cpu_set_t expected; CPU_ZERO(&expected); CPU_SET(first_cpu, &expected);
        errno = EDOM; totk_thread_game(81); CHECK(errno == EDOM, "game helper preserves errno"); check_name("g-81"); check_mask(&expected);
        CHECK(getpriority(PRIO_PROCESS, 0) == nice, "game affinity does not change nice");
        CPU_ZERO(&expected); CPU_SET(walk_cpu, &expected);
        errno = ERANGE; totk_thread_walk(); CHECK(errno == ERANGE, "walk helper preserves errno"); check_name("totk-walk"); check_mask(&expected);
        CHECK(getpriority(PRIO_PROCESS, 0) == requested_nice, "walk nice applies to this tid");
        CHECK(sched_getscheduler(0) == policy, "uclamp preserves scheduling policy");
        if (totk_thread_uclamp_get() >= 0) CHECK(totk_thread_uclamp_get() == 0, "uclamp minimum queried after reset");
        totk_thread_start("once-kept", NULL); totk_thread_walk(); check_name("once-kept");
    } else if (phase == 2) {
        totk_thread_walk(); check_name("totk-walk"); check_mask(&original);
        CHECK(getpriority(PRIO_PROCESS, 0) == nice && totk_thread_uclamp_get() == clamp, "invalid options change no scheduling state");
    } else if (phase == 3) {
        totk_thread_start("totk-recw0", "TOTK_AFFINITY_RECW"); check_name("totk-recw0");
        cpu_set_t expected; CPU_ZERO(&expected); CPU_SET(first_cpu, &expected); check_mask(&expected);
    } else if (phase == 5 || phase == 6) {
        cpu_set_t narrow; CPU_ZERO(&narrow); CPU_SET(first_cpu, &narrow);
        CHECK(!sched_setaffinity(0, sizeof narrow, &narrow), "simulate inheritance from a pinned walk thread");
        errno = EDOM;
        if (phase == 5) totk_thread_start("g-main", "TOTK_AFFINITY_GAME"); else totk_thread_game(82);
        CHECK(errno == EDOM, "auto game affinity preserves errno");
        check_name(phase == 5 ? "g-main" : "g-82"); check_mask(&totk_game_affinity_mask);
        CHECK(getpriority(PRIO_PROCESS, 0) == nice && sched_getscheduler(0) == policy && totk_thread_uclamp_get() == clamp,
              "auto changes no nice, scheduler or clamp");
    } else if (phase == 7 || phase == 8) {
        totk_thread_start("g-main", "TOTK_AFFINITY_GAME"); check_mask(&original);
    } else {
        totk_thread_walk();
        int result = getpriority(PRIO_PROCESS, 0);
        CHECK(result == nice || result == -20, "privileged nice request applies or fails without fallback");
        CHECK(sched_getscheduler(0) == policy, "privileged request keeps scheduling class");
    }
    return NULL;
}
static void launch(unsigned phase) {
    pthread_t thread; int error = pthread_create(&thread, NULL, worker, (void *)(uintptr_t)phase);
    CHECK(!error, "create placement test thread"); if (!error) CHECK(!pthread_join(thread, NULL), "join placement test thread");
}
static void options_clear(void) {
    const char *keys[] = {"TOTK_AFFINITY_WALK", "TOTK_AFFINITY_RECW", "TOTK_AFFINITY_GAME", "TOTK_PRIO_WALK", "TOTK_UCLAMP_WALK",
        "TOTK_PRIO_RENDER", "TOTK_TIMER_SLACK_NS"};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) unsetenv(keys[i]);
}
/* TOTK_PRIO_RENDER: frame-critical host threads only; compile/seed/cache workers and guests keep their nice. */
typedef struct { const char *name; int expected_delta; } render_case;
static void *render_worker(void *arg) {
    const render_case *c = arg; int nice = getpriority(PRIO_PROCESS, 0);
    errno = EDOM;
    if (!strcmp(c->name, "totk-walk")) totk_thread_walk(); else totk_thread_start(c->name, NULL);
    CHECK(errno == EDOM, "render priority preserves errno");
    int result = getpriority(PRIO_PROCESS, 0);
    if (result != nice + c->expected_delta) {
        failures++; fprintf(stderr, "FAIL: %s nice %d, expected %d\n", c->name, result, nice + c->expected_delta);
    }
    checks++; return NULL;
}
static void render_launch(const char *name, int expected_delta) {
    render_case c = {name, expected_delta}; pthread_t thread;
    int error = pthread_create(&thread, NULL, render_worker, &c);
    CHECK(!error, "create render test thread"); if (!error) CHECK(!pthread_join(thread, NULL), "join render test thread");
}
static void render_tests(int initial_nice) {
    if (initial_nice > 17) return;   /* needs two unprivileged nice steps */
    char value[16]; options_clear();
    snprintf(value, sizeof value, "%d", initial_nice + 1); setenv("TOTK_PRIO_RENDER", value, 1);
    const char *frame[] = {"totk-walk", "totk-recw0", "totk-recw1", "totk-submit", "totk-gpufence", "totk-post",
        "totk-present", "totk-vsync"};
    for (unsigned i = 0; i < sizeof frame / sizeof *frame; i++) render_launch(frame[i], 1);
    const char *other[] = {"totk-pipe0", "totk-compute", "totk-pcache", "totk-seed-io", "totk-prewarm", "totk-audio",
        "g-main", "g-90"};
    for (unsigned i = 0; i < sizeof other / sizeof *other; i++) render_launch(other[i], 0);
    snprintf(value, sizeof value, "%d", initial_nice + 2); setenv("TOTK_PRIO_WALK", value, 1);
    render_launch("totk-walk", 2);   /* the walker's own setting is applied last */
    render_launch("totk-submit", 1);
    const char *invalid[] = {"20", "-21", "", "+", "1x"};
    unsetenv("TOTK_PRIO_WALK");
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        setenv("TOTK_PRIO_RENDER", invalid[i], 1); render_launch("totk-submit", 0);
    }
    options_clear();
}
static void fixture_file(const char *root, const char *relative, const char *text) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/%s", root, relative);
    FILE *f = fopen(path, "w"); CHECK(f != NULL, "create synthetic sysfs file");
    if (f) { CHECK(fputs(text, f) >= 0, "write synthetic sysfs file"); CHECK(!fclose(f), "close synthetic sysfs file"); }
}
static void fixture_capacity(const char *root, unsigned cpu, unsigned value) {
    char path[64], text[32]; snprintf(path, sizeof path, "cpu%u/cpu_capacity", cpu);
    snprintf(text, sizeof text, "%u\n", value); fixture_file(root, path, text);
}
static void fixture_remove(const char *root, const char *relative, int directory) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/%s", root, relative);
    CHECK(!(directory ? rmdir(path) : unlink(path)), "remove synthetic sysfs fixture");
}
static void selection(const char *root, const char *allowed_text, const char *value,
                      const char *expected_text, const char *expected_source, const char *expected_decision, int error) {
    cpu_set_t allowed, expected, actual; unsigned capacity[CPU_SETSIZE]; const char *source, *decision;
    CHECK(totk_thread_cpu_parse(allowed_text, &allowed) && totk_thread_cpu_parse(expected_text, &expected), "parse policy fixture masks");
    CHECK(totk_thread_game_select(root, &allowed, value, &actual, capacity, &source, &decision) == error, "policy override result");
    CHECK(!memcmp(&actual, &expected, sizeof actual), "capacity-tier selected mask");
    CHECK(!strcmp(source, expected_source) && !strcmp(decision, expected_decision), "capacity source and decision");
}
static void background_selection(const char *root, const char *allowed_text,
                                const char *expected_text, const char *expected_source,
                                const char *expected_decision) {
    cpu_set_t allowed, expected, actual; unsigned capacity[CPU_SETSIZE];
    const char *source, *decision;
    CHECK(totk_thread_cpu_parse(allowed_text, &allowed) &&
          totk_thread_cpu_parse(expected_text, &expected), "parse background policy fixture masks");
    totk_thread_background_select(root, &allowed, &actual, capacity, &source, &decision);
    CHECK(!memcmp(&actual, &expected, sizeof actual), "background capacity-tier selected mask");
    CHECK(!strcmp(source, expected_source) && !strcmp(decision, expected_decision),
          "background capacity source and decision");
}

static void capacity_tests(void) {
    char root[] = ".thread-place-XXXXXX"; CHECK(mkdtemp(root) != NULL, "create scratch sysfs root");
    char path[PATH_MAX];
    for (unsigned cpu = 0; cpu < 8; cpu++) {
        snprintf(path, sizeof path, "%s/cpu%u", root, cpu); CHECK(!mkdir(path, 0700), "create synthetic CPU");
        fixture_capacity(root, cpu, cpu < 3 ? 256 : cpu < 7 ? 768 : 1024);
    }
    selection(root, "0-7", NULL, "3-7", "capacity", "exclude-lowest", 0); /* Thor */
    background_selection(root, "0-7", "0-2", "capacity", "select-lowest"); /* Thor */
    background_selection(root, "1,7", "1,7", "capacity", "unchanged"); /* no single-CPU pin */
    selection(root, "0-7", "auto", "3-7", "capacity", "exclude-lowest", 0);
    selection(root, "1-2,4,7", NULL, "4,7", "capacity", "exclude-lowest", 0);
    background_selection(root, "1-2,4,7", "1-2", "capacity", "select-lowest"); /* honor inherited cpuset */
    selection(root, "3-7", NULL, "3-7", "capacity", "unchanged", 0);
    selection(root, "0-2", NULL, "0-2", "capacity", "unchanged", 0);
    selection(root, "1,7", NULL, "1,7", "capacity", "unchanged", 0); /* never auto single-CPU */
    selection(root, "0-7", "all", "0-7", "none", "unchanged", 0);
    selection(root, "0-7", "0-2", "0-2", "none", "explicit", 0);
    selection(root, "0-7", "0xf8", "3-7", "none", "explicit", 0);
    selection(root, "4-7", "3-6", "4-6", "none", "explicit", 0);
    const char *invalid[] = {"", "AUTO", "all ", "junk", "0x0", "8"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        selection(root, "0-7", invalid[i], "0-7", "none", "explicit", EINVAL);
    for (unsigned cpu = 0; cpu < 8; cpu++) fixture_capacity(root, cpu, cpu < 6 ? 768 : 1024); /* Odin */
    background_selection(root, "0-7", "0-7", "capacity", "unchanged"); /* no supported Odin low-cluster assumption */
    selection(root, "0-7", NULL, "0-7", "capacity", "unchanged", 0);
    for (unsigned cpu = 0; cpu < 8; cpu++) fixture_capacity(root, cpu, 1024);
    selection(root, "0-7", NULL, "0-7", "capacity", "unchanged", 0);
    background_selection(root, "0-7", "0-7", "capacity", "unchanged"); /* homogeneous */
    fixture_capacity(root, 0, 600); fixture_capacity(root, 1, 1000);
    fixture_capacity(root, 2, 1000);
    selection(root, "0-2", NULL, "0-2", "capacity", "unchanged", 0); /* exact 60% boundary */
    fixture_capacity(root, 0, 599);
    selection(root, "0-2", NULL, "1-2", "capacity", "exclude-lowest", 0);
    fixture_remove(root, "cpu7/cpu_capacity", 0);
    selection(root, "0-7", NULL, "0-7", "unavailable", "unchanged", 0);
    background_selection(root, "0-7", "0-7", "unavailable", "unchanged");
    selection(root, "0-7", "all", "0-7", "none", "unchanged", 0);
    snprintf(path, sizeof path, "%s/cpufreq", root); CHECK(!mkdir(path, 0700), "create policy root");
    const unsigned policies[] = {0, 3, 7}; const char *members[] = {"0 1 2\n", "3 4 5 6\n", "7\n"};
    const char *frequencies[] = {"1000000\n", "2800000\n", "3200000\n"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/cpufreq/policy%u", root, policies[i]); CHECK(!mkdir(path, 0700), "create synthetic policy");
        char relative[96]; snprintf(relative, sizeof relative, "cpufreq/policy%u/related_cpus", policies[i]); fixture_file(root, relative, members[i]);
        snprintf(relative, sizeof relative, "cpufreq/policy%u/cpuinfo_max_freq", policies[i]); fixture_file(root, relative, frequencies[i]);
    }
    selection(root, "0-7", NULL, "3-7", "max_freq", "exclude-lowest", 0);
    background_selection(root, "0-7", "0-2", "max_freq", "select-lowest");
    selection(root, "1-2,4,7", NULL, "4,7", "max_freq", "exclude-lowest", 0);
    fixture_file(root, "cpufreq/policy0/cpuinfo_max_freq", "2500000\n"); /* all-big frequency tiers */
    selection(root, "0-7", NULL, "0-7", "max_freq", "unchanged", 0);
    fixture_file(root, "cpufreq/policy7/cpuinfo_max_freq", "0\n");
    selection(root, "0-7", NULL, "0-7", "unavailable", "unchanged", 0);
    snprintf(path, sizeof path, "%s/cpu7/cpufreq", root); CHECK(!mkdir(path, 0700), "create per-CPU policy link fixture");
    fixture_file(root, "cpu7/cpufreq/cpuinfo_max_freq", "3200000\n");
    selection(root, "0-7", NULL, "0-7", "max_freq", "unchanged", 0);
    fixture_remove(root, "cpu7/cpufreq/cpuinfo_max_freq", 0); fixture_remove(root, "cpu7/cpufreq", 1);
    fixture_capacity(root, 7, 1024);
    fixture_file(root, "cpu0/cpu_capacity", "256junk\n");
    selection(root, "0-7", NULL, "0-7", "unavailable", "unchanged", 0);
    for (unsigned cpu = 0; cpu < 8; cpu++) {
        char relative[64]; snprintf(relative, sizeof relative, "cpu%u/cpu_capacity", cpu); fixture_remove(root, relative, 0);
        snprintf(relative, sizeof relative, "cpu%u", cpu); fixture_remove(root, relative, 1);
    }
    for (unsigned i = 0; i < 3; i++) {
        char relative[96]; snprintf(relative, sizeof relative, "cpufreq/policy%u/cpuinfo_max_freq", policies[i]); fixture_remove(root, relative, 0);
        snprintf(relative, sizeof relative, "cpufreq/policy%u/related_cpus", policies[i]); fixture_remove(root, relative, 0);
        snprintf(relative, sizeof relative, "cpufreq/policy%u", policies[i]); fixture_remove(root, relative, 1);
    }
    fixture_remove(root, "cpufreq", 1);
    selection(root, "0-7", NULL, "0-7", "unavailable", "unchanged", 0);
    CHECK(!rmdir(root), "remove scratch sysfs root");
}
int main(void) {
    capacity_tests();
    cpu_set_t a, b;
    CHECK(totk_thread_cpu_parse(" 3-6, 7 ", &a) && totk_thread_cpu_parse("0xf8", &b) && !memcmp(&a, &b, sizeof a), "list/range equals explicit hex mask");
    CHECK(totk_thread_cpu_parse("7", &a) && CPU_ISSET(7, &a) && !CPU_ISSET(0, &a), "bare number is a CPU id, not a mask");
    CHECK(totk_thread_cpu_parse("0x00000000000000000000000000000000000000000000000000000000000000001", &a) && CPU_ISSET(0, &a), "leading-zero mask");
    const char *invalid[] = {"", " ", "-1", "+1", "0x", "0x0", "0x80z", "0x-1", "1,", "1,,2", "2-1", "1 2", "1024", "9999999999999999999999", "0-1024"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) CHECK(!totk_thread_cpu_parse(invalid[i], &a), "reject invalid/empty/out-of-range CPU specification");
    char too_wide[CPU_SETSIZE / 4 + 4]; memcpy(too_wide, "0x1", 3); memset(too_wide + 3, '0', CPU_SETSIZE / 4); too_wide[sizeof too_wide - 1] = 0;
    CHECK(!totk_thread_cpu_parse(too_wide, &a), "reject set bits outside cpu_set_t");
    int n; CHECK(totk_thread_integer(" -20 ", -20, 19, &n) && n == -20, "signed nice bound");
    CHECK(!totk_thread_integer("20", -20, 19, &n) && !totk_thread_integer("-21", -20, 19, &n) && !totk_thread_integer("1junk", -20, 19, &n), "reject invalid nice");
    CHECK(totk_thread_integer("1024", 0, 1024, &n) && n == 1024 && !totk_thread_integer("1025", 0, 1024, &n), "uclamp bounds");
    if (sched_getaffinity(0, sizeof original, &original)) { perror("sched_getaffinity"); return 1; }
    first_cpu = CPU_SETSIZE; walk_cpu = CPU_SETSIZE;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, &original)) { if (first_cpu == CPU_SETSIZE) first_cpu = cpu; else { walk_cpu = cpu; break; } }
    if (first_cpu == CPU_SETSIZE) return 1; if (walk_cpu == CPU_SETSIZE) walk_cpu = first_cpu;
    char list[32], mask[CPU_SETSIZE * 6]; totk_thread_mask_text(&original, mask, sizeof mask);
    CHECK(totk_thread_cpu_parse(mask, &a) && !memcmp(&original, &a, sizeof a), "result mask text round trip");
    int initial_nice = getpriority(PRIO_PROCESS, 0); requested_nice = initial_nice < 19 ? initial_nice + 1 : 19;
    options_clear(); launch(0);
    snprintf(list, sizeof list, "%u", first_cpu); setenv("TOTK_AFFINITY_GAME", list, 1); setenv("TOTK_AFFINITY_RECW", list, 1);
    snprintf(list, sizeof list, "%u", walk_cpu); setenv("TOTK_AFFINITY_WALK", list, 1);
    snprintf(list, sizeof list, "%d", requested_nice); setenv("TOTK_PRIO_WALK", list, 1); setenv("TOTK_UCLAMP_WALK", "0", 1); launch(1);
    options_clear(); setenv("TOTK_AFFINITY_WALK", "0x0", 1); setenv("TOTK_PRIO_WALK", "20", 1); setenv("TOTK_UCLAMP_WALK", "1025", 1); launch(2);
    options_clear(); snprintf(list, sizeof list, "%u", first_cpu); setenv("TOTK_AFFINITY_RECW", list, 1); launch(3);
    options_clear(); setenv("TOTK_PRIO_WALK", "-20", 1); launch(4); options_clear();
    errno = EDOM; totk_thread_game_startup(); CHECK(errno == EDOM, "startup preserves errno");
    CHECK(totk_game_affinity_valid, "startup captures one allowed mask");
    cpu_set_t cached = totk_game_affinity_mask;
    options_clear(); launch(5);
    setenv("TOTK_AFFINITY_GAME", "auto", 1); launch(6);
    CHECK(!memcmp(&cached, &totk_game_affinity_mask, sizeof cached), "main and created guests share immutable startup mask");
    setenv("TOTK_AFFINITY_GAME", "all", 1); launch(7);
    setenv("TOTK_AFFINITY_GAME", "", 1); launch(8); options_clear();
    render_tests(initial_nice);
    check_mask(&original); CHECK(getpriority(PRIO_PROCESS, 0) == initial_nice, "other threads' nice settings do not affect parent");
    printf("ok: thread placement %u checks, %u failed\n", atomic_load(&checks), atomic_load(&failures)); return atomic_load(&failures) != 0;
}

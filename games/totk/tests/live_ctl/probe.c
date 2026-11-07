/* Host probe of the live-switch channel (runtime/live_ctl.h, through thread_place.h), the production headers unchanged:
 *   parse                     one line per stdin line: "ok <kind> <key> <value> <text>", "skip" or "refused <why>"
 *   core <dir>                staging and application at the frame boundary, generation ordering, the accessor's
 *                             cache, concurrent readers against a writer and a frame thread, and the control thread
 *                             on a real file in <dir> (created late, appended, partial lines, replaced, truncated)
 *   switches <dir>            every live switch toggled many times by batches applied on a frame thread, while
 *                             registered render threads (TOTK_PRIO_RENDER by tid, through the nice hook; a walker
 *                             pinned by TOTK_PRIO_WALK), game threads applying TOTK_TIMER_SLACK_NS before their timed
 *                             waits, and readers of every other switch run
 *   missing <dir>             one CPU-pinned control worker: unrelated creates/deletes and moves while totk.ctl
 *                             is absent, interrupted reads, then late creation and repeated deletion/recreation;
 *                             thread CPU and syscall/watch counts catch spins invisible to rd.wakeups
 * Every check prints FAIL: on failure; the last line is "ok: ..." or "FAIL: ...". Build it plain and with
 * -fsanitize=thread: the concurrent sections are the race check. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Observe only the isolated control worker's real syscalls, not the fixture's file operations. */
static int observed_open(const char *path, int flags, ...);
static ssize_t observed_read(int fd, void *data, size_t size);
static ssize_t observed_pread(int fd, void *data, size_t size, off_t at);
static int observed_stat(const char *path, struct stat *st);
static int observed_fstat(int fd, struct stat *st);
static int observed_close(int fd);
static int observed_add_watch(int fd, const char *path, uint32_t mask);
static int observed_rm_watch(int fd, int watch);
#define open(...) observed_open(__VA_ARGS__)
#define read(...) observed_read(__VA_ARGS__)
#define pread(...) observed_pread(__VA_ARGS__)
#define stat(...) observed_stat(__VA_ARGS__)
#define fstat(...) observed_fstat(__VA_ARGS__)
#define close(...) observed_close(__VA_ARGS__)
#define inotify_add_watch(...) observed_add_watch(__VA_ARGS__)
#define inotify_rm_watch(...) observed_rm_watch(__VA_ARGS__)
#include "thread_place.h"   /* includes live_ctl.h */
#undef open
#undef read
#undef pread
#undef stat
#undef fstat
#undef close
#undef inotify_add_watch
#undef inotify_rm_watch

enum { CTL_OPEN, CTL_READ, CTL_PREAD, CTL_STAT, CTL_FSTAT, CTL_CLOSE, CTL_ADD, CTL_REMOVE, CTL_CALLS };
static _Thread_local int observe_control;
static _Atomic unsigned long ctl_calls[CTL_CALLS], ctl_event_reads, ctl_interrupts, ctl_ignored, ctl_directory_watches;
static int trace_open;
static void observed_call(unsigned which) { if (observe_control) atomic_fetch_add(&ctl_calls[which], 1); }
static int observed_open(const char *path, int flags, ...) {
    observed_call(CTL_OPEN);
    if (!(flags & O_CREAT)) {
        int fd = open(path, flags), saved = errno;
        if (observe_control && trace_open) printf("open %s\n", fd >= 0 ? "ok" : saved == EACCES ? "denied" : "missing");
        errno = saved; return fd;
    }
    va_list args; va_start(args, flags); mode_t mode = va_arg(args, mode_t); va_end(args);
    return open(path, flags, mode);
}
static ssize_t observed_read(int fd, void *data, size_t size) {
    observed_call(CTL_READ);
    ssize_t n = read(fd, data, size); int saved = errno;
    if (observe_control) {
        if (n < 0 && saved == EINTR) atomic_fetch_add(&ctl_interrupts, 1);
        if (n > 0) {
            for (ssize_t i = 0; i < n; ) {
                const struct inotify_event *ev = (const struct inotify_event *)((const char *)data + i);
                if (ev->mask & IN_IGNORED) atomic_fetch_add(&ctl_ignored, 1);
                i += (ssize_t)sizeof *ev + ev->len;
            }
            atomic_fetch_add(&ctl_event_reads, 1);
        }
    }
    errno = saved; return n;
}
static ssize_t observed_pread(int fd, void *data, size_t size, off_t at) {
    observed_call(CTL_PREAD); return pread(fd, data, size, at);
}
static int observed_stat(const char *path, struct stat *st) { observed_call(CTL_STAT); return stat(path, st); }
static int observed_fstat(int fd, struct stat *st) { observed_call(CTL_FSTAT); return fstat(fd, st); }
static int observed_close(int fd) { observed_call(CTL_CLOSE); return close(fd); }
static int observed_add_watch(int fd, const char *path, uint32_t mask) {
    observed_call(CTL_ADD); int watch = inotify_add_watch(fd, path, mask);
    if (observe_control && watch >= 0 && (mask & IN_CREATE)) atomic_fetch_add(&ctl_directory_watches, 1);
    return watch;
}
static int observed_rm_watch(int fd, int watch) { observed_call(CTL_REMOVE); return inotify_rm_watch(fd, watch); }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stdout, "FAIL: "); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } } while (0)

static const char *kind_name(int kind) { return kind == TOTK_LIVE_SET ? "set" : kind == TOTK_LIVE_UNSET ? "unset" : "mark"; }
static int parse_mode(void) {
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        totk_live_op op; char why[160]; int r = totk_live_parse(line, &op, why, sizeof why);
        if (r > 0) printf("ok %s %s %d %s\n", kind_name(op.kind), op.kind == TOTK_LIVE_MARK ? "-" : totk_live_switches[op.key].key,
                          op.value, op.kind == TOTK_LIVE_MARK ? op.text : "-");
        else if (!r) printf("skip\n");
        else printf("refused %s\n", why);
    }
    return 0;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const char *log_path;
static long log_at;
/* The [totk-ctl] lines stderr received since the last call, into out. */
static void log_take(char *out, size_t size) {
    fflush(stderr); out[0] = 0;
    FILE *f = fopen(log_path, "r"); if (!f) return;
    fseek(f, log_at, SEEK_SET); size_t n = fread(out, 1, size - 1, f); out[n] = 0; log_at += (long)n; fclose(f);
}
static void feed(const char *text) { static totk_live_lines lines; totk_live_feed(&lines, text, strlen(text)); }
static int value(const char *key) { return totk_live_int(key, -999); }

/* ---- concurrent readers: batch k sets both wide switches to k; generation g implies values >= g ---- */
enum { BATCHES = 400 };
static _Atomic int writer_done, applied_all;
static uint32_t g_first;   /* the generation before the first batch; set before the threads start */
static void *writer(void *arg) {
    (void)arg; char line[128];
    for (int k = (int)totk_live_generation() + 1, end = k + BATCHES; k < end; k++) {
        while (atomic_load(&totk_live_staged)) usleep(50);   /* one batch per generation */
        snprintf(line, sizeof line, "set TOTK_VK_SPLIT_PASS_DRAWS=%d\nset TOTK_TIMER_SLACK_NS=%d\n", k, k); feed(line);
    }
    atomic_store(&writer_done, 1); return NULL;
}
static void *frames(void *arg) {
    (void)arg; uint64_t frame = 1000;
    while (!atomic_load(&writer_done) || atomic_load(&totk_live_staged)) { totk_live_frame(++frame); usleep(20); }
    atomic_store(&applied_all, 1); return NULL;
}
static void *reader(void *arg) {
    int *bad = arg; int last = 0; totk_live_cache cache = {0};
    while (!atomic_load(&applied_all)) {
        /* Values are stored before their generation is published, so a read may lead its generation, never trail it. */
        int g = (int)totk_live_generation(), a = value("TOTK_VK_SPLIT_PASS_DRAWS"), b = value("TOTK_TIMER_SLACK_NS");
        int h = (int)totk_live_generation(), c = totk_live_cached(&cache, "TOTK_VK_SPLIT_PASS_DRAWS", -999);
        if (g > (int)g_first && (a < g || b < g || a < last || c < h)) (*bad)++;
        last = a;
    }
    return NULL;
}

/* ---- the control thread on a real file ---- */
static totk_live_reader rd;
static void *control(void *arg) { (void)arg; live_ctl_run(&rd); return NULL; }
/* Presents frames until the generation passes `after` (5 s at most); returns the new generation or 0. */
static uint32_t frames_until(uint32_t after) {
    static uint64_t frame = 50000; double start = now_ms();
    while (now_ms() - start < 5000) {
        totk_live_frame(++frame);
        uint32_t g = totk_live_generation(); if (g > after) return g;
        usleep(1000);
    }
    return 0;
}
static void append(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600); if (fd < 0) return;
    if (write(fd, text, strlen(text)) < 0) perror("append"); close(fd);
}

static void core(const char *dir) {
    static char log[1 << 18]; char path[1024];
    /* Before anything was applied: generation 0, every read is the fallback, no table lookup. */
    totk_live_cache cache = {0};
    for (int i = 0; i < 1000; i++) CHECK(totk_live_cached(&cache, "TOTK_VK_PIPE_BOOST", 7) == 7, "inactive read returns the fallback");
    CHECK(!totk_live_generation() && !atomic_load(&totk_live_reads), "inactive: generation 0 and no lookups");
    totk_live_frame(1); CHECK(!totk_live_generation(), "a frame with nothing staged changes nothing");

    /* Malformed and refused lines stage nothing; the valid ones wait for the frame and apply together, in file order. */
    feed("set TOTK_VK_PIPE_BOOST=1\nset TOTK_IDEAL_CORE_FIX=1\nfrobnicate\nset TOTK_VK_UPLOAD_THREADS=9\nset TOTK_VK_DESC_STOCK=1\n"
         "set TOTK_VK_DESC_STOCK=0\nunset TOTK_VK_UNWRITTEN_MASK\nset TOTK_NOT_A_SWITCH=1\nmark arm=A block=1\n# note\n\n"
         "set TOTK_VK_FEEDBACK_CUT");   /* partial: waits for its newline */
    log_take(log, sizeof log);
    CHECK(strstr(log, "refused \"set TOTK_IDEAL_CORE_FIX=1\": TOTK_IDEAL_CORE_FIX is startup-only"), "startup-only key refused with its reason: %s", log);
    CHECK(strstr(log, "refused \"frobnicate\": unknown command"), "unknown command refused");
    CHECK(strstr(log, "refused \"set TOTK_VK_UPLOAD_THREADS=9\": TOTK_VK_UPLOAD_THREADS=9: not a decimal in 0..3"), "out of range refused");
    CHECK(strstr(log, "refused \"set TOTK_NOT_A_SWITCH=1\": TOTK_NOT_A_SWITCH is not a live switch"), "unknown key refused");
    CHECK(!totk_live_generation() && value("TOTK_VK_PIPE_BOOST") == -999, "staged lines wait for the frame boundary");
    totk_live_frame(300);
    log_take(log, sizeof log);
    CHECK(totk_live_generation() == 1, "one batch, one generation");
    CHECK(strstr(log, "[totk-ctl] gen=1 frame=300 applied TOTK_VK_PIPE_BOOST=1 TOTK_VK_DESC_STOCK=1 TOTK_VK_DESC_STOCK=0 unset TOTK_VK_UNWRITTEN_MASK\n")
          && strstr(log, "[totk-ctl] gen=1 frame=300 mark arm=A block=1\n"), "application lines: %s", log);
    CHECK(value("TOTK_VK_PIPE_BOOST") == 1 && value("TOTK_VK_DESC_STOCK") == 0 && value("TOTK_VK_UNWRITTEN_MASK") == -999,
          "values in file order; unset is the launch value");
    CHECK(value("TOTK_VK_FEEDBACK_CUT") == -999, "a partial line is not applied");
    feed("=1\n"); totk_live_frame(301);
    CHECK(totk_live_generation() == 2 && value("TOTK_VK_FEEDBACK_CUT") == 1, "its newline completes it: the next generation");
    feed("unset TOTK_VK_PIPE_BOOST\n"); totk_live_frame(302);
    CHECK(totk_live_generation() == 3 && value("TOTK_VK_PIPE_BOOST") == -999, "unset returns the launch value");
    log_take(log, sizeof log);
    CHECK(strstr(log, "gen=2 frame=301 applied TOTK_VK_FEEDBACK_CUT=1\n") && strstr(log, "gen=3 frame=302 applied unset TOTK_VK_PIPE_BOOST\n"),
          "generations in order: %s", log);

    /* The cache: one lookup per generation, however many reads. */
    uint64_t reads = atomic_load(&totk_live_reads);
    for (int i = 0; i < 1000; i++) CHECK(totk_live_cached(&cache, "TOTK_VK_FEEDBACK_CUT", 0) == 1, "cached value");
    CHECK(atomic_load(&totk_live_reads) == reads + 1, "1000 reads in one generation: one lookup (%llu)", (unsigned long long)(atomic_load(&totk_live_reads) - reads));
    feed("set TOTK_VK_FEEDBACK_CUT=0\n"); totk_live_frame(303);
    for (int i = 0; i < 1000; i++) CHECK(totk_live_cached(&cache, "TOTK_VK_FEEDBACK_CUT", 0) == 0, "cached value after a change");
    CHECK(atomic_load(&totk_live_reads) == reads + 2, "a new generation: one more lookup");
    printf("ok: parser, staging, application at the frame boundary, generation order, cache\n");

    /* Concurrent: a writer stages BATCHES batches, a frame thread applies them, readers check the ordering. */
    pthread_t w, f, r[3]; int bad[3] = {0};
    uint32_t g0 = g_first = totk_live_generation();
    pthread_create(&w, NULL, writer, NULL); pthread_create(&f, NULL, frames, NULL);
    for (int i = 0; i < 3; i++) pthread_create(&r[i], NULL, reader, &bad[i]);
    pthread_join(w, NULL); pthread_join(f, NULL);
    for (int i = 0; i < 3; i++) { pthread_join(r[i], NULL); CHECK(!bad[i], "reader %d saw a value older than its generation (%d times)", i, bad[i]); }
    CHECK(totk_live_generation() == g0 + BATCHES, "%d batches, %d generations (%u)", BATCHES, BATCHES, totk_live_generation() - g0);
    log_take(log, sizeof log);
    printf("ok: %d batches applied by a frame thread while 3 readers checked generation >= values\n", BATCHES);

    /* The control thread: the file does not exist yet, then lines are appended (one partial), then it is replaced. */
    snprintf(path, sizeof path, "%s/totk.ctl", dir); unlink(path);
    rd.path = path; rd.skip_existing = 1;
    pthread_t t; pthread_create(&t, NULL, control, NULL); pthread_detach(t);
    usleep(100000);
    uint32_t g = totk_live_generation();
    append(path, "set TOTK_VK_PIPE_BOOST=1\nmark created\n");
    CHECK((g = frames_until(g)) && value("TOTK_VK_PIPE_BOOST") == 1, "a created file is read from its start");
    append(path, "set TOTK_VK_UPLOAD_");
    usleep(100000); totk_live_frame(1); CHECK(totk_live_generation() == g, "a partial line waits");
    append(path, "THREADS=2\n");
    CHECK((g = frames_until(g)) && value("TOTK_VK_UPLOAD_THREADS") == 2, "the rest of the line arrives");
    char fresh[1100]; snprintf(fresh, sizeof fresh, "%s.new", path);
    unlink(fresh); append(fresh, "set TOTK_VK_UPLOAD_THREADS=3\n"); rename(fresh, path);
    CHECK((g = frames_until(g)) && value("TOTK_VK_UPLOAD_THREADS") == 3, "a replaced file is read from its start");
    if (truncate(path, 0)) perror("truncate");
    usleep(200000);   /* the reader sees the shorter file (an equal-size rewrite would be indistinguishable) */
    append(path, "set TOTK_VK_UPLOAD_THREADS=1\n");
    CHECK((g = frames_until(g)) && value("TOTK_VK_UPLOAD_THREADS") == 1, "a truncated file is read again from its start");
    append(path, "garbage line\nset TOTK_VK_DESC_STOCK=1\n");
    CHECK((g = frames_until(g)) && value("TOTK_VK_DESC_STOCK") == 1, "a malformed line does not stop the reader");
    log_take(log, sizeof log);
    CHECK(strstr(log, "mark created") && strstr(log, "refused \"garbage line\""), "control thread lines: %s", log);
    printf("ok: control thread (%s): created late, appended, partial line, replaced, truncated, malformed; %lu wakeups\n",
           "inotify", __atomic_load_n(&rd.wakeups, __ATOMIC_RELAXED));

    /* A file that exists at the start: its old lines are not replayed. */
    char old[1100]; snprintf(old, sizeof old, "%s/old.ctl", dir); unlink(old);
    append(old, "set TOTK_VK_PIPE_BOOST=0\n");
    static totk_live_reader rd2; rd2.path = old; rd2.skip_existing = 1;
    static pthread_t t2; pthread_create(&t2, NULL, (void *(*)(void *))(void *)live_ctl_run, &rd2); pthread_detach(t2);
    usleep(200000); totk_live_frame(2);
    CHECK(value("TOTK_VK_PIPE_BOOST") == 1, "existing lines are skipped at the start");
    append(old, "set TOTK_VK_PIPE_BOOST=0\n");
    CHECK(frames_until(g) && value("TOTK_VK_PIPE_BOOST") == 0, "lines appended later apply");
    printf("ok: an existing file's earlier lines are skipped\n");
}

/* ---- absent-file idle cost: wakeups alone never saw the old remove/IN_IGNORED/rearm loop ---- */
typedef struct { unsigned long calls[CTL_CALLS], events, interrupts, ignored, wakeups; double cpu; } ctl_sample;
static clockid_t ctl_clock;
static ctl_sample control_sample(void) {
    ctl_sample s = {0}; struct timespec t = {0};
    CHECK(!clock_gettime(ctl_clock, &t), "read the control worker's CPU clock");
    s.cpu = t.tv_sec * 1e3 + t.tv_nsec / 1e6;
    for (unsigned i = 0; i < CTL_CALLS; i++) s.calls[i] = atomic_load(&ctl_calls[i]);
    s.events = atomic_load(&ctl_event_reads); s.interrupts = atomic_load(&ctl_interrupts);
    s.ignored = atomic_load(&ctl_ignored); s.wakeups = __atomic_load_n(&rd.wakeups, __ATOMIC_RELAXED);
    return s;
}
static int control_until(_Atomic unsigned long *counter, unsigned long want) {
    double start = now_ms();
    while (now_ms() - start < 5000) {
        if (atomic_load(counter) >= want) return 1;
        usleep(1000);
    }
    return 0;
}
static void *observed_control(void *arg) { observe_control = 1; live_ctl_run(arg); return NULL; }
static void interrupt_control(int sig) { (void)sig; }
static void control_idle(const char *dir, const char *phase, ctl_sample before, unsigned count, int moved,
                         unsigned transitions, int directory) {
    char other[1100], source[1100];
    snprintf(other, sizeof other, "%s/unrelated", dir);
    snprintf(source, sizeof source, "%s/incoming/unrelated", dir);
    for (unsigned i = 0; i < count; i++) {
        int fd = open(moved ? source : other, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0, "%s: create unrelated file %u", phase, i);
        if (fd >= 0) close(fd);
        if (moved) CHECK(!rename(source, other), "%s: move unrelated file %u into watched directory", phase, i);
        CHECK(!unlink(other), "%s: delete unrelated file %u", phase, i);
    }
    if (count && directory) CHECK(control_until(&ctl_event_reads, before.events + 1), "%s: consume directory events", phase);
    usleep(300000);
    ctl_sample after = control_sample(); unsigned long calls = 0;
    for (unsigned i = 0; i < CTL_CALLS; i++) calls += after.calls[i] - before.calls[i];
    unsigned long opens = after.calls[CTL_OPEN] - before.calls[CTL_OPEN];
    unsigned long adds = after.calls[CTL_ADD] - before.calls[CTL_ADD];
    unsigned long removes = after.calls[CTL_REMOVE] - before.calls[CTL_REMOVE];
    /* CPU is actual thread run time, not elapsed scheduling time. A blocked reader needs far less than
       100 ms per 300 ms window; syscall bounds catch a spin even when the pinned CPU is heavily loaded. */
    CHECK(after.cpu - before.cpu < 100, "%s: idle control worker used %.3f ms CPU", phase, after.cpu - before.cpu);
    CHECK(calls <= count + 32, "%s: bounded syscalls: %lu for %u unrelated events", phase, calls, count);
    CHECK(opens <= transitions && adds <= transitions && removes <= transitions,
          "%s: no unrelated reopen/rearm: open=%lu add=%lu remove=%lu (allowed transitions=%u)",
          phase, opens, adds, removes, transitions);
    if (!transitions && directory) CHECK(after.wakeups == before.wakeups, "%s: unrelated events/EINTR do not count as control wakes", phase);
    printf("ok: idle %s cpu=%.3f ms syscalls=%lu open=%lu add=%lu remove=%lu wakeups=%lu ignored=%lu EINTR=%lu\n",
           phase, after.cpu - before.cpu, calls, opens, adds, removes, after.wakeups - before.wakeups,
           after.ignored - before.ignored, after.interrupts - before.interrupts);
}
static void missing(const char *dir) {
    static char path[1024]; char incoming[1100], unrelated_log[1100];
    snprintf(path, sizeof path, "%s/totk.ctl", dir); unlink(path);
    snprintf(incoming, sizeof incoming, "%s/incoming", dir);
    CHECK(!mkdir(incoming, 0700), "create the unrelated move's source directory");
    snprintf(unrelated_log, sizeof unrelated_log, "%s/unrelated.log", dir);
    append(unrelated_log, "created before the watch\n");
    cpu_set_t allowed, one; CPU_ZERO(&one);
    int error = sched_getaffinity(0, sizeof allowed, &allowed), cpu = -1;
    CHECK(!error, "read allowed host CPUs");
    if (error) return;
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &allowed)) { cpu = i; CPU_SET(i, &one); break; }
    CHECK(cpu >= 0, "an allowed host CPU exists"); if (cpu < 0) return;
    struct sigaction action = {.sa_handler = interrupt_control};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1, &action, NULL), "install non-restarting signal handler");
    pthread_attr_t attr; error = pthread_attr_init(&attr);
    CHECK(!error, "initialize control worker attributes"); if (error) return;
    error = pthread_attr_setaffinity_np(&attr, sizeof one, &one);
    CHECK(!error, "pin the isolated worker to allowed CPU %d", cpu);
    if (error) { pthread_attr_destroy(&attr); return; }
    rd.path = path; rd.skip_existing = 1;
    pthread_t thread; error = pthread_create(&thread, &attr, observed_control, &rd); pthread_attr_destroy(&attr);
    CHECK(!error, "start isolated control worker"); if (error) return;
    pthread_detach(thread);
    error = pthread_getcpuclockid(thread, &ctl_clock);
    CHECK(!error, "get isolated control worker CPU clock"); if (error) return;
    CHECK(control_until(&ctl_calls[CTL_READ], 1), "absent target: directory watch armed and reader waiting");
    CHECK(atomic_load(&ctl_directory_watches) == 1 && atomic_load(&ctl_calls[CTL_OPEN]) == 1,
          "absent target starts with one directory watch and one open attempt");
    printf("ok: isolated control worker pinned to allowed CPU %d\n", cpu);

    /* Normal play closes logs/cache files and changes their attributes while totk.ctl never exists.
       rd.wakeups filters unrelated events, so assert zero actual inotify deliveries/read retries as well. */
    ctl_sample quiet_before = control_sample();
    for (int i = 0; i < 64; i++) {
        append(unrelated_log, "unrelated write and close\n");
        CHECK(!chmod(unrelated_log, i & 1 ? 0600 : 0640), "change unrelated log attributes");
    }
    control_idle(dir, "absent unrelated CLOSE_WRITE+ATTRIB", quiet_before, 0, 0, 0, 1);
    ctl_sample quiet_after = control_sample();
    CHECK(quiet_after.events == quiet_before.events && quiet_after.calls[CTL_READ] == quiet_before.calls[CTL_READ],
          "absent target: unrelated closes/attributes cause zero inotify deliveries or read retries");
    CHECK(quiet_after.wakeups == quiet_before.wakeups, "absent target: unrelated closes/attributes cause zero reader wakeups");
    printf("ok: absent unrelated CLOSE_WRITE+ATTRIB: zero inotify deliveries and reader wakeups\n");

    /* Replay the device failure: absent target, many unrelated CREATE+DELETE pairs, then MOVED_TO+DELETE.
       The wake counter stays zero even in the old spinning implementation, so measure CPU and syscalls too. */
    control_idle(dir, "absent CREATE+DELETE", control_sample(), 64, 0, 0, 1);
    control_idle(dir, "absent MOVED_TO+DELETE", control_sample(), 64, 1, 0, 1);
    ctl_sample before = control_sample();
    for (int i = 0; i < 8; i++) { CHECK(!pthread_kill(thread, SIGUSR1), "interrupt the blocked reader"); usleep(1000); }
    CHECK(control_until(&ctl_interrupts, before.interrupts + 1), "blocking inotify read returns EINTR");
    control_idle(dir, "absent EINTR", before, 0, 0, 0, 1);

    uint32_t gen = totk_live_generation();
    append(path, "set TOTK_VK_SPLIT_PASS_DRAWS=1\nmark created after unrelated events\n");
    CHECK((gen = frames_until(gen)) && value("TOTK_VK_SPLIT_PASS_DRAWS") == 1,
          "late-created totk.ctl is read from its beginning after unrelated events and EINTR");
    CHECK(control_until(&ctl_ignored, 1), "consume the removed directory watch's IN_IGNORED");
    control_idle(dir, "created IN_IGNORED", control_sample(), 16, 0, 0, 0);
    for (int i = 0; i < 4; i++) {
        before = control_sample(); unsigned long directories = atomic_load(&ctl_directory_watches);
        CHECK(!unlink(path), "delete totk.ctl before recreation %d", i + 1);
        CHECK(control_until(&ctl_directory_watches, directories + 1), "rearm one directory watch after deletion %d", i + 1);
        char phase[64]; snprintf(phase, sizeof phase, "deleted %d %s+DELETE", i + 1, i & 1 ? "MOVED_TO" : "CREATE");
        control_idle(dir, phase, before, 32, i & 1, 1, 1);
        char line[128]; snprintf(line, sizeof line, "set TOTK_VK_SPLIT_PASS_DRAWS=%d\nmark recreation %d\n", i + 2, i + 1);
        append(path, line);
        CHECK((gen = frames_until(gen)) && value("TOTK_VK_SPLIT_PASS_DRAWS") == i + 2,
              "totk.ctl recreation %d is read from its beginning", i + 1);
    }
    before = control_sample();
    CHECK(control_until(&ctl_ignored, 5), "directory-watch IN_IGNORED events consumed across recreations");
    control_idle(dir, "recreated IN_IGNORED", before, 16, 1, 0, 0);
    printf("ok: absent-file control reader: unrelated creates/deletes/moves, EINTR, late creation, 4 delete/recreate cycles\n");
}

/* The Python fixture supplies deliberately separated creates, writes and permission changes. */
static void late(const char *dir) {
    static char path[1024];
    snprintf(path, sizeof path, "%s/totk.ctl", dir);
    rd.path = path; rd.skip_existing = 1; trace_open = 1;
    pthread_t thread;
    int error = pthread_create(&thread, NULL, observed_control, &rd);
    CHECK(!error, "start late-create control worker"); if (error) return;
    pthread_detach(thread);
    CHECK(control_until(&ctl_calls[CTL_READ], 1), "absent target: reader waiting on directory watch");
    printf("ready\n");
    CHECK(frames_until(0) == 1, "late-created file acknowledged without a rename rescue");
    char log[4096]; log_take(log, sizeof log); printf("%s", log);
}

/* ---------- every live switch toggled ---------- */
enum { RENDER = 4, SLACKERS = 4, TOGGLES = 300 };
static _Atomic int render_tid[RENDER + 1], render_nice[RENDER + 1], render_started, stop_all;
static _Atomic int slack_started;
static _Atomic uint32_t applied_gen;
static int hook_nice(int tid, int nice) {   /* totk_live_set_nice: records instead of needing CAP_SYS_NICE */
    for (int i = 0; i <= RENDER; i++) if (atomic_load(&render_tid[i]) == tid) { atomic_store(&render_nice[i], nice); return 0; }
    return ESRCH;
}
static void *render_thread(void *arg) {
    int i = (int)(intptr_t)arg; atomic_store(&render_tid[i], (int)syscall(SYS_gettid));
    if (i == RENDER) totk_thread_walk();   /* the walker: TOTK_PRIO_WALK pins it */
    else totk_thread_start(i & 1 ? "totk-submit" : "totk-recw0", NULL);
    atomic_fetch_add(&render_started, 1);
    while (!atomic_load(&stop_all)) usleep(500);
    return NULL;
}
static void *slack_thread(void *arg) {
    int *bad = arg; char name[16]; snprintf(name, sizeof name, "g-%d", (int)(intptr_t)bad % 97);
    /* Capture inheritance before startup can apply a live value. Do not publish
     * any toggles until every worker has recorded that same pre-toggle baseline. */
    long base = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    totk_thread_start(name, NULL);
    atomic_fetch_add(&slack_started, 1);
    while (!atomic_load(&stop_all)) {
        /* A value is stored just before its slack generation is published: a mismatch counts only if it outlives a
           few further ticks with the generation unchanged. */
        int mismatch = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            uint32_t before = atomic_load(&totk_live_slack_gen);
            totk_live_slack_tick();   /* as before a timed wait */
            long slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
            int want = totk_live_int("TOTK_TIMER_SLACK_NS", 0);
            mismatch = atomic_load(&totk_live_slack_gen) == before && slack != (want ? want : base);
            if (!mismatch) break;
            usleep(1000);
        }
        *bad += mismatch;
        struct timespec ts = {0, 20000}; nanosleep(&ts, NULL);
    }
    return NULL;
}
static void *switch_reader(void *arg) {
    int *bad = arg; totk_live_cache cache[TOTK_LIVE_MAX] = {0};
    unsigned n = sizeof totk_live_switches / sizeof *totk_live_switches;
    while (!atomic_load(&stop_all))
        for (unsigned k = 0; k < n; k++) {
            int v = totk_live_cached(&cache[k], totk_live_switches[k].key, -999);
            if (v != -999 && (v < totk_live_switches[k].lo || v > totk_live_switches[k].hi)) (*bad)++;
        }
    return NULL;
}
static void *frame_thread(void *arg) {
    (void)arg; uint64_t frame = 0;
    while (!atomic_load(&stop_all)) {
        uint32_t g = totk_live_generation(); totk_live_frame(++frame);
        if (totk_live_generation() != g) atomic_store(&applied_gen, totk_live_generation());   /* after its side effects */
        usleep(100);
    }
    return NULL;
}
static void switches(void) {
    totk_live_set_nice = hook_nice;
    setenv("TOTK_PRIO_WALK", "5", 1); unsetenv("TOTK_PRIO_RENDER"); unsetenv("TOTK_TIMER_SLACK_NS");
    for (int i = 0; i <= RENDER; i++) atomic_store(&render_nice[i], -100);
    errno = 0; int inherited = getpriority(PRIO_PROCESS, 0);   /* render threads inherit it; unset returns them to it */
    pthread_t render[RENDER + 1], slackers[SLACKERS], readers[2], frames;
    int slack_bad[SLACKERS] = {0}, read_bad[2] = {0};
    for (int i = 0; i <= RENDER; i++) pthread_create(&render[i], NULL, render_thread, (void *)(intptr_t)i);
    while (atomic_load(&render_started) < RENDER + 1) usleep(1000);   /* registered, the walker pinned */
    for (int i = 0; i < SLACKERS; i++) pthread_create(&slackers[i], NULL, slack_thread, &slack_bad[i]);
    while (atomic_load(&slack_started) < SLACKERS) usleep(1000);
    for (int i = 0; i < 2; i++) pthread_create(&readers[i], NULL, switch_reader, &read_bad[i]);
    pthread_create(&frames, NULL, frame_thread, NULL);
    uint32_t s = 0x9e3779b9u; unsigned n = sizeof totk_live_switches / sizeof *totk_live_switches, prio_bad = 0, pinned_bad = 0;
    for (int t = 0; t < TOGGLES; t++) {
        char batch[2048]; int k = 0;
        for (unsigned i = 0; i < n; i++) {
            const totk_live_switch *w = &totk_live_switches[i];
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            if (s % 3 == 0) k += snprintf(batch + k, sizeof batch - k, "unset %s\n", w->key);
            else k += snprintf(batch + k, sizeof batch - k, "set %s=%d\n", w->key, w->lo + (int)(s % (uint32_t)(w->hi - w->lo + 1)));
        }
        snprintf(batch + k, sizeof batch - k, "mark toggle %d\n", t);
        uint32_t target = totk_live_generation() + 1;
        feed(batch);
        double start = now_ms();
        while (atomic_load(&applied_gen) < target && now_ms() - start < 5000) usleep(100);
        CHECK(atomic_load(&applied_gen) >= target, "toggle %d applied", t);
        int want = totk_live_int("TOTK_PRIO_RENDER", 1000);
        for (int i = 0; i < RENDER; i++) prio_bad += atomic_load(&render_nice[i]) != (want == 1000 ? inherited : want);
        pinned_bad += atomic_load(&render_nice[RENDER]) != -100;
    }
    atomic_store(&stop_all, 1);
    for (int i = 0; i <= RENDER; i++) pthread_join(render[i], NULL);
    for (int i = 0; i < SLACKERS; i++) { pthread_join(slackers[i], NULL); CHECK(!slack_bad[i], "game thread %d: slack differed from the live value %d times", i, slack_bad[i]); }
    for (int i = 0; i < 2; i++) { pthread_join(readers[i], NULL); CHECK(!read_bad[i], "reader %d: %d values out of range", i, read_bad[i]); }
    pthread_join(frames, NULL);
    CHECK(!prio_bad, "render threads: nice differed from TOTK_PRIO_RENDER (or their inherited nice when unset) %u times", prio_bad);
    CHECK(!pinned_bad, "the walker pinned by TOTK_PRIO_WALK was changed %u times", pinned_bad);
    printf("ok: %d batches toggled all %u live switches; %d render threads by tid, the pinned walker, %d game threads' slack, 2 readers\n",
           TOGGLES, n, RENDER, SLACKERS);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "parse")) return parse_mode();
    if (argc != 3 || (strcmp(argv[1], "core") && strcmp(argv[1], "switches") && strcmp(argv[1], "missing") && strcmp(argv[1], "late"))) {
        fprintf(stderr, "usage: probe parse | probe {core|switches|missing|late} <dir>\n"); return 2;
    }
    static char stderr_path[1100]; snprintf(stderr_path, sizeof stderr_path, "%s/stderr.txt", argv[2]);
    if (!freopen(stderr_path, "w", stderr)) return 3;
    setvbuf(stderr, NULL, _IONBF, 0); setvbuf(stdout, NULL, _IOLBF, 0);
    log_path = stderr_path;
    if (!strcmp(argv[1], "core")) core(argv[2]); else if (!strcmp(argv[1], "missing")) missing(argv[2]);
    else if (!strcmp(argv[1], "late")) late(argv[2]); else switches();
    printf("%s: live switch channel\n", failures ? "FAIL" : "ok");
    return failures != 0;
}

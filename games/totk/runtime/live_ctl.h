/* Live switches: experiment settings changed inside one running game session, at a frame boundary, so an A/B
   measurement can alternate its arms in one loaded scene (tools/bench/live.sh, ab.sh) instead of restarting.

   A low-priority thread (live_ctl_run, Linux; frontend_android.c starts it) follows <game>/totk.ctl, reading only
   what is appended after the game started, and stages every complete line:
     set KEY=VALUE   a whitelisted switch (totk_live_switches) to a decimal value inside its range
     unset KEY       the switch back to its launch value (totk.env, else the switch's default)
     mark <text>     a marker in totk.log, e.g. the A/B arm that starts there
   Empty lines and lines starting with # are ignored. Anything else (a malformed line, a value out of range, a key that
   is not live, a startup-only key) is logged as refused and changes nothing; the game keeps running.

   The present path calls totk_live_frame(frame) once per presented frame. While nothing is staged that is one relaxed
   load. Everything staged is applied there in one step, in file order, and the generation advances once:
     [totk-ctl] gen=N frame=F applied KEY=VALUE unset KEY ...
     [totk-ctl] gen=N frame=F mark <text>
   F is the last frame presented before the change: frames after F run with generation N. The staging lock is only
   tried there, never waited for (a busy lock retries at the next frame).

   Subsystems read a switch with totk_live_cached (or totk_live_int) at their own safe point and keep their launch-time
   parsing as the fallback. Until the first application the generation is 0 and every read returns that fallback
   without a lookup; afterwards a read is one relaxed load until the generation changes again.

   The state is shared by every translation unit that includes this header (weak definitions are merged), so the
   runtime, the Vulkan backend and the host probe (games/totk/tests/live_ctl) use one copy. */
#pragma once
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The whitelist. Each switch is read live at its own safe point; its range is checked here, before staging.
   To make another launch-time switch live: add it here with its range, read it through totk_live_cached at a point
   where a change is safe, and document that point next to the switch (APP.md / VULKAN.md) and in "Live switches"
   (APP.md). Keys not listed are refused; totk_live_startup_only names the ones refused with the reason. */
typedef struct { const char *key; int lo, hi; } totk_live_switch;
static const totk_live_switch totk_live_switches[] = {
    {"TOTK_VK_PIPE_BOOST", 0, 1},            /* the next join on a worker build */
    {"TOTK_VK_UPLOAD_THREADS", 0, 3},        /* the next texture upload; helpers start lazily, idle at 0 */
    {"TOTK_PRIO_RENDER", -20, 19},           /* at the application, by tid on the registered render threads */
    {"TOTK_TIMER_SLACK_NS", 1, 50000},       /* each game / frame thread before its next timed wait */
    {"TOTK_VK_DESC_STOCK", 0, 1},            /* each descriptor owner at its next slot generation */
    {"TOTK_VK_DESC_STOCK_MAX_SETS", 0, 1048576}, /* the same handoff; 0 = no ceiling */
    {"TOTK_VK_UNWRITTEN_MASK", 0, 1},        /* pipelines built afterwards; built ones keep their masks */
    {"TOTK_VK_SPLIT_PASS_DRAWS", 0, 1000000},/* the next render pass opened */
    {"TOTK_VK_SPLIT_LRZ_SAFE", 0, 1},       /* the next split decision / depth pass opened */
    {"TOTK_VK_LOAD_ELIDE", 0, 1},           /* the next render pass opened; never changes an open pass's load ops */
    {"TOTK_VK_FEEDBACK_NARROW", 0, 1},     /* the next render pass opened; outstanding invalidations still drain at its end */
    {"TOTK_VK_ALPHA_TEST", 0, 1},          /* next draw: immutable fragment comparison variant and reference block */
    {"TOTK_VK_CLEAR_ON_READ", 0, 1},       /* next resource read: resolve an outstanding attachment load-clear */
    {"TOTK_VK_STAGING_RETAIN_MB", 8, 4096},  /* each slot at its next open, after its fence */
    {"TOTK_VK_FEEDBACK_CUT", 0, 1},          /* the next descriptor build */
    {"TOTK_VK_PREPARE_MASK", 0, 1},          /* the next draw's uniform preparation; a change dirties every register */
    {"TOTK_THREAD_SAMPLER", 0, 1},           /* the next presented frame (thread_sampler.h); starts its thread once */
    {"TOTK_VK_REC_WAKE_BATCH", 0, 2000},     /* the next chunk enqueue and the next time a recorder worker finds no work */
    {"TOTK_VK_TEX_MEMO", 0, 1},              /* the next texture ID lookup (entries are kept: their key is the registry state) */
    {"TOTK_NVN_CALL_NOCOPY", 0, 1},          /* the next CallCommands/CopyCommands; markers already recorded still expand */
    {"TOTK_ADPF_GPU", 0, 1},                 /* the next presented frame; that frame is not reported */
    {"TOTK_ADPF_LOAD_HINT", 0, 1},           /* the next span report; reset consecutive-frame history, retain the 500ms limit */
    {"TOTK_ADPF_CPU_SPLIT", 0, 1},           /* the next presented frame; only subsequent frames have complete queue-wait history */
    {"TOTK_ADPF_TARGET_PCT", 50, 100},       /* the next presented frame; update target before its duration report */
    {"TOTK_RATE_HINT_HOLD", 0, 60000},       /* the next guest present (ms; 0 = the configured rate); needs TOTK_FRAME_RATE_HINT=1 */
};
enum { TOTK_LIVE_MAX = 32 };
_Static_assert(sizeof totk_live_switches / sizeof *totk_live_switches <= TOTK_LIVE_MAX, "live switch table");
/* Read once at startup by design: changing them inside a session is refused with this reason. */
static const char *const totk_live_startup_only[][2] = {
    {"TOTK_IDEAL_CORE_FIX", "core bookkeeping of every guest thread is chosen when it is created"},
    {"TOTK_CORE_WAIT_EVENT", "virtual-core wait and host-return handoff invariants are chosen at startup"},
    {"TOTK_VK_POS_INVARIANT", "shader modules and cache identity are fixed at startup"},
    {"TOTK_VK_POS_NOCONTRACT", "shader modules and cache identity are fixed at startup"},
    {"TOTK_VK_INT_FOLD", "shader modules and cache identity are fixed at startup"},
    {"TOTK_VK_SUBGROUP_FIX", "device subgroup features and immutable shader modules/pipelines are chosen at startup"},
    {"TOTK_THERMAL_STEP", "guest controller veneers and thermal sampling are installed only at startup"},
};

enum { TOTK_LIVE_SET = 1, TOTK_LIVE_UNSET, TOTK_LIVE_MARK };
enum { TOTK_LIVE_LINE = 240, TOTK_LIVE_STAGE = 128 };
typedef struct { int kind, key, value; char text[TOTK_LIVE_LINE]; } totk_live_op;
typedef struct { pthread_mutex_t mu; unsigned n; totk_live_op op[TOTK_LIVE_STAGE]; } totk_live_stage_t;

__attribute__((weak)) _Atomic uint32_t totk_live_gen;            /* applications so far; 0 = never */
__attribute__((weak)) _Atomic uint32_t totk_live_staged;         /* staged lines wait for the next frame */
__attribute__((weak)) _Atomic uint32_t totk_live_slack_gen;      /* TOTK_TIMER_SLACK_NS changed (totk_live_slack_tick) */
__attribute__((weak)) _Atomic uint64_t totk_live_reads;          /* table lookups (the caching test) */
/* Per switch: 0 = launch value, else 1 << 32 | (uint32_t)value. Written only by totk_live_frame. */
__attribute__((weak)) _Atomic uint64_t totk_live_state[TOTK_LIVE_MAX];
__attribute__((weak)) totk_live_stage_t totk_live_stage = {.mu = PTHREAD_MUTEX_INITIALIZER};

static inline int totk_live_index(const char *key) {
    for (unsigned i = 0; i < sizeof totk_live_switches / sizeof *totk_live_switches; i++)
        if (!strcmp(key, totk_live_switches[i].key)) return (int)i;
    return -1;
}
static inline uint32_t totk_live_generation(void) { return atomic_load_explicit(&totk_live_gen, memory_order_acquire); }
/* The switch's live value, or fallback (the caller's launch value) when it was never set live or was unset. */
static inline int totk_live_int(const char *key, int fallback) {
    int i = totk_live_index(key); if (i < 0) return fallback;
    atomic_fetch_add_explicit(&totk_live_reads, 1, memory_order_relaxed);
    uint64_t s = atomic_load_explicit(&totk_live_state[i], memory_order_acquire);
    return s ? (int)(int32_t)(uint32_t)s : fallback;
}
/* One call site's cache: the generation it last read and that value, in one atomic word, so threads may share it.
   Zero-initialise it; pass the same key and fallback at every call. */
typedef struct { _Atomic uint64_t seen; } totk_live_cache;
static inline int totk_live_cached(totk_live_cache *c, const char *key, int fallback) {
    uint32_t g = atomic_load_explicit(&totk_live_gen, memory_order_relaxed);
    if (__builtin_expect(!g, 1)) return fallback;
    uint64_t s = atomic_load_explicit(&c->seen, memory_order_relaxed);
    if ((uint32_t)(s >> 32) == g) return (int)(int32_t)(uint32_t)s;
    g = atomic_load_explicit(&totk_live_gen, memory_order_acquire);   /* pairs with the generation's release */
    int v = totk_live_int(key, fallback);
    atomic_store_explicit(&c->seen, (uint64_t)g << 32 | (uint32_t)v, memory_order_relaxed);
    return v;
}

/* ---------- parsing (the control thread) ---------- */
static inline int totk_live_decimal(const char *p, long lo, long hi, int *value) {
    if (!isdigit((unsigned char)*p) && !((*p == '-' || *p == '+') && isdigit((unsigned char)p[1]))) return 0;
    char *end; errno = 0; long n = strtol(p, &end, 10);
    if (errno || *end || n < lo || n > hi) return 0;
    *value = (int)n; return 1;
}
/* One line (no newline). 1: *op is a command; 0: empty or a comment; -1: refused, why says so. */
static inline int totk_live_parse(const char *line, totk_live_op *op, char *why, size_t size) {
    char text[TOTK_LIVE_LINE]; size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1])) n--;
    while (n && isspace((unsigned char)*line)) { line++; n--; }
    if (!n || *line == '#') return 0;
    if (n >= sizeof text) { snprintf(why, size, "line longer than %d bytes", TOTK_LIVE_LINE - 1); return -1; }
    memcpy(text, line, n); text[n] = 0;
    memset(op, 0, sizeof *op);
    char *arg = text; while (*arg && !isspace((unsigned char)*arg)) arg++;
    if (*arg) { *arg++ = 0; while (isspace((unsigned char)*arg)) arg++; }
    if (!strcmp(text, "mark")) {
        if (!*arg) { snprintf(why, size, "mark needs a text"); return -1; }
        op->kind = TOTK_LIVE_MARK;
        size_t i = 0; for (; arg[i]; i++) op->text[i] = (unsigned char)arg[i] < 32 ? ' ' : arg[i];
        op->text[i] = 0;
        return 1;
    }
    int set = !strcmp(text, "set");
    if (!set && strcmp(text, "unset")) { snprintf(why, size, "unknown command (set KEY=VALUE, unset KEY, mark <text>)"); return -1; }
    char *value = NULL;
    if (set) {
        value = strchr(arg, '=');
        if (!value) { snprintf(why, size, "set needs KEY=VALUE"); return -1; }
        *value++ = 0;
    }
    if (!*arg) { snprintf(why, size, "missing key"); return -1; }
    for (char *p = arg; *p; p++) if (!isupper((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '_') {
        snprintf(why, size, "malformed key"); return -1;
    }
    int key = totk_live_index(arg);
    if (key < 0) {
        for (unsigned i = 0; i < sizeof totk_live_startup_only / sizeof *totk_live_startup_only; i++)
            if (!strcmp(arg, totk_live_startup_only[i][0])) {
                snprintf(why, size, "%s is startup-only (%s): set it in totk.env and restart", arg, totk_live_startup_only[i][1]);
                return -1;
            }
        snprintf(why, size, "%s is not a live switch", arg); return -1;
    }
    op->key = key;
    if (!set) { op->kind = TOTK_LIVE_UNSET; return 1; }
    const totk_live_switch *s = &totk_live_switches[key];
    if (!totk_live_decimal(value, s->lo, s->hi, &op->value)) {
        snprintf(why, size, "%s=%s: not a decimal in %d..%d", arg, value, s->lo, s->hi); return -1;
    }
    op->kind = TOTK_LIVE_SET; return 1;
}
/* Parses one line and stages it, or logs the refusal; the caller holds the staging lock. 1 staged, 0 ignored, -1 refused. */
static inline int totk_live_line_locked(const char *line) {
    totk_live_op op; char why[160];
    totk_live_stage_t *st = &totk_live_stage;
    int r = totk_live_parse(line, &op, why, sizeof why);
    if (r > 0) {
        if (st->n < TOTK_LIVE_STAGE) st->op[st->n++] = op;
        else { r = -1; snprintf(why, sizeof why, "%d lines already wait for a frame", TOTK_LIVE_STAGE); }
    }
    if (r < 0) {
        char shown[TOTK_LIVE_LINE]; size_t i = 0;
        for (; line[i] && i < sizeof shown - 1; i++) shown[i] = (unsigned char)line[i] < 32 ? ' ' : line[i];
        shown[i] = 0;
        fprintf(stderr, "[totk-ctl] refused \"%s\": %s\n", shown, why);
    }
    return r;
}
/* Complete lines of a byte stream; a partial last line waits for its newline. The complete lines of one chunk (one
   read of the file) are staged under one lock, so the lines of one write apply in the same generation. */
typedef struct { char line[TOTK_LIVE_LINE + 1]; size_t used; int lost; } totk_live_lines;
static inline void totk_live_feed(totk_live_lines *l, const char *data, size_t n) {
    totk_live_stage_t *st = &totk_live_stage;
    pthread_mutex_lock(&st->mu);
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            l->line[l->used] = 0;
            if (l->lost) fprintf(stderr, "[totk-ctl] refused a line longer than %d bytes\n", TOTK_LIVE_LINE - 1);
            else totk_live_line_locked(l->line);
            l->used = 0; l->lost = 0;
        } else if (l->used < TOTK_LIVE_LINE - 1) l->line[l->used++] = c ? c : ' ';
        else l->lost = 1;
    }
    if (st->n) atomic_store_explicit(&totk_live_staged, 1, memory_order_release);
    pthread_mutex_unlock(&st->mu);
}

/* ---------- application (the present path) ---------- */
/* Render-thread nice (TOTK_PRIO_RENDER) and per-thread timer slack (TOTK_TIMER_SLACK_NS); thread_place.h. */
static inline void totk_live_prio_apply(uint32_t gen);
static inline void totk_live_slack_apply(void);
static inline void totk_live_apply(uint64_t frame) {
    totk_live_stage_t *st = &totk_live_stage;
    if (pthread_mutex_trylock(&st->mu)) return;   /* the control thread is staging: the next frame */
    unsigned n = st->n;
    if (!n) { pthread_mutex_unlock(&st->mu); return; }
    int prio = 0, slack = 0;
    for (unsigned i = 0; i < n; i++) {
        const totk_live_op *op = &st->op[i];
        if (op->kind == TOTK_LIVE_MARK) continue;
        uint64_t s = op->kind == TOTK_LIVE_SET ? (uint64_t)1 << 32 | (uint32_t)op->value : 0;
        atomic_store_explicit(&totk_live_state[op->key], s, memory_order_relaxed);
        prio |= !strcmp(totk_live_switches[op->key].key, "TOTK_PRIO_RENDER");
        slack |= !strcmp(totk_live_switches[op->key].key, "TOTK_TIMER_SLACK_NS");
    }
    uint32_t gen = atomic_fetch_add_explicit(&totk_live_gen, 1, memory_order_release) + 1;
    if (slack) atomic_fetch_add_explicit(&totk_live_slack_gen, 1, memory_order_release);
    char line[2048]; int k = snprintf(line, sizeof line, "[totk-ctl] gen=%u frame=%llu applied", gen, (unsigned long long)frame);
    int changes = 0;
    for (unsigned i = 0; i < n; i++) {
        const totk_live_op *op = &st->op[i];
        if (op->kind == TOTK_LIVE_MARK || k < 0 || (size_t)k >= sizeof line - 64) continue;
        changes++;
        k += op->kind == TOTK_LIVE_SET ? snprintf(line + k, sizeof line - k, " %s=%d", totk_live_switches[op->key].key, op->value)
                                      : snprintf(line + k, sizeof line - k, " unset %s", totk_live_switches[op->key].key);
    }
    if (changes) fprintf(stderr, "%s\n", line);
    for (unsigned i = 0; i < n; i++)
        if (st->op[i].kind == TOTK_LIVE_MARK)
            fprintf(stderr, "[totk-ctl] gen=%u frame=%llu mark %s\n", gen, (unsigned long long)frame, st->op[i].text);
    st->n = 0; atomic_store_explicit(&totk_live_staged, 0, memory_order_relaxed);
    pthread_mutex_unlock(&st->mu);
    if (prio) totk_live_prio_apply(gen);
    if (slack) totk_live_slack_apply();   /* the presenting thread itself; the others at their next timed wait */
}
/* Once per presented frame, after the frame's work: one relaxed load while nothing is staged. */
static inline void totk_live_frame(uint64_t frame) {
    if (__builtin_expect(!atomic_load_explicit(&totk_live_staged, memory_order_relaxed), 1)) return;
    /* totk_live_apply takes the staging lock, which orders the staged lines. */
    totk_live_apply(frame);
}

/* ---------- render threads (TOTK_PRIO_RENDER) ---------- */
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
/* The frame-critical host threads (thread_place.h totk_thread_frame_critical) register at their start with the nice
   they inherited, so a live TOTK_PRIO_RENDER can be set and unset on them by tid. They live as long as the process. */
enum { TOTK_LIVE_THREADS = 64 };
typedef struct { int tid, inherited, pinned; } totk_live_thread;
typedef struct { pthread_mutex_t mu; unsigned n; totk_live_thread t[TOTK_LIVE_THREADS]; } totk_live_threads_t;
__attribute__((weak)) totk_live_threads_t totk_live_threads = {.mu = PTHREAD_MUTEX_INITIALIZER};
/* Test hook: the nice setter (tid, nice) -> 0 or errno. */
__attribute__((weak)) int (*totk_live_set_nice)(int tid, int nice);
static inline int totk_live_nice_set(int tid, int nice) {
    if (totk_live_set_nice) return totk_live_set_nice(tid, nice);
    return setpriority(PRIO_PROCESS, (id_t)tid, nice) ? errno : 0;
}
/* The launch value of TOTK_PRIO_RENDER, or 1000 when unset / invalid (each thread keeps its inherited nice). */
static inline int totk_live_prio_launch(void) {
    const char *e = getenv("TOTK_PRIO_RENDER"); int v;
    while (e && isspace((unsigned char)*e)) e++;
    return e && totk_live_decimal(e, -20, 19, &v) ? v : 1000;
}
static inline int totk_live_prio_target(const totk_live_thread *t) {
    int v = totk_live_int("TOTK_PRIO_RENDER", totk_live_prio_launch());
    return v == 1000 ? t->inherited : v;
}
/* Registers the calling render thread (once: a repeat keeps the first inherited nice); returns 1 when a live
   TOTK_PRIO_RENDER was applied to it (the caller then skips its launch-time setting). */
static inline int totk_live_register_render(void) {
    int tid = (int)syscall(SYS_gettid), saved = errno; errno = 0;
    int inherited = getpriority(PRIO_PROCESS, (id_t)tid); if (inherited == -1 && errno) inherited = 0;
    errno = saved;
    totk_live_threads_t *r = &totk_live_threads; int live = 0; totk_live_thread *t = NULL;
    pthread_mutex_lock(&r->mu);
    for (unsigned i = 0; i < r->n && !t; i++) if (r->t[i].tid == tid) t = &r->t[i];
    if (!t && r->n < TOTK_LIVE_THREADS) { t = &r->t[r->n++]; *t = (totk_live_thread){tid, inherited, 0}; }
    if (t && totk_live_generation() && atomic_load_explicit(&totk_live_state[totk_live_index("TOTK_PRIO_RENDER")], memory_order_acquire)) {
        int nice = totk_live_prio_target(t), error = totk_live_nice_set(tid, nice);
        fprintf(stderr, "[totk-ctl] tid=%d TOTK_PRIO_RENDER=%d (live) errno=%d\n", tid, nice, error);
        live = 1;
    }
    pthread_mutex_unlock(&r->mu);
    return live;
}
/* The walker's own TOTK_PRIO_WALK wins over TOTK_PRIO_RENDER, live or not. */
static inline void totk_live_pin_render(void) {
    int tid = (int)syscall(SYS_gettid); totk_live_threads_t *r = &totk_live_threads;
    pthread_mutex_lock(&r->mu);
    for (unsigned i = 0; i < r->n; i++) if (r->t[i].tid == tid) r->t[i].pinned = 1;
    pthread_mutex_unlock(&r->mu);
}
static inline void totk_live_prio_apply(uint32_t gen) {
    totk_live_threads_t *r = &totk_live_threads; unsigned set = 0, kept = 0, gone = 0, refused = 0; int error = 0;
    pthread_mutex_lock(&r->mu);
    for (unsigned i = 0; i < r->n; i++) {
        totk_live_thread *t = &r->t[i];
        if (t->pinned) { kept++; continue; }
        char task[64]; snprintf(task, sizeof task, "/proc/self/task/%d", t->tid);
        if (!totk_live_set_nice && access(task, F_OK)) { gone++; continue; }   /* never another process's tid */
        int e = totk_live_nice_set(t->tid, totk_live_prio_target(t));
        if (e) { refused++; error = e; } else set++;
    }
    pthread_mutex_unlock(&r->mu);
    int v = totk_live_int("TOTK_PRIO_RENDER", totk_live_prio_launch());
    char value[16]; if (v == 1000) snprintf(value, sizeof value, "inherited"); else snprintf(value, sizeof value, "%d", v);
    fprintf(stderr, "[totk-ctl] gen=%u TOTK_PRIO_RENDER=%s: %u render threads set, %u kept (TOTK_PRIO_WALK), %u gone, %u refused%s%s\n",
            gen, value, set, kept, gone, refused, refused ? " " : "", refused ? strerror(error) : "");
}

/* ---------- per-thread timer slack (TOTK_TIMER_SLACK_NS) ---------- */
/* PR_SET_TIMERSLACK changes only the calling thread, so every game / frame thread applies a live change itself before
   its next timed wait (totk_live_slack_tick: one relaxed load while nothing changed). Unset returns to the launch
   value, else 0: the thread's default slack (the kernel's inherited 50 us). */
static inline void totk_live_slack_apply(void) {
    int launch = 0; const char *e = getenv("TOTK_TIMER_SLACK_NS");
    while (e && isspace((unsigned char)*e)) e++;
    if (!e || !totk_live_decimal(e, 1, 50000, &launch)) launch = 0;
    int ns = totk_live_int("TOTK_TIMER_SLACK_NS", launch);
    prctl(PR_SET_TIMERSLACK, (unsigned long)ns, 0, 0, 0);
}
static inline void totk_live_slack_tick(void) {
    static _Thread_local uint32_t seen;   /* per translation unit: a repeat is only a redundant prctl */
    uint32_t g = atomic_load_explicit(&totk_live_slack_gen, memory_order_relaxed);
    if (__builtin_expect(g == seen, 1)) return;
    g = atomic_load_explicit(&totk_live_slack_gen, memory_order_acquire);   /* pairs with its release */
    seen = g; totk_live_slack_apply();
}

/* ---------- the control thread ---------- */
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
/* Follows path: keep a create/move-only directory watch until open succeeds, so unrelated log writes/closes and
   attribute changes do not wake the absent-file reader. Failed opens other than ENOENT and failed reads retry
   every 250 ms, including ownership/context repair after creation. Then watch only the file.
   Without inotify it polls every 250 ms. A truncated or replaced file is read again from its beginning. Never returns. */
typedef struct { const char *path; int skip_existing; unsigned long wakeups; } totk_live_reader;
static inline void live_ctl_run(totk_live_reader *rd) {
    char dir[PATH_MAX], data[4096], events[4096]; const char *name = strrchr(rd->path, '/');
    snprintf(dir, sizeof dir, "%.*s", name ? (int)(name - rd->path) : 1, name ? rd->path : ".");
    name = name ? name + 1 : rd->path;
    int in = inotify_init1(IN_CLOEXEC), watch = -1, directory = 0, fd = -1; ino_t inode = 0; off_t at = 0;
    totk_live_lines lines = {0};
    for (int first = 1;; first = 0) {
        int retry = 0;
        if (fd < 0) {
            /* Keep the folder watch until our file can be opened. Removing it queues IN_IGNORED, so rearming after
               unrelated folder events would wake this thread forever, even with no control-file changes. */
            if (in >= 0 && !directory) {
                if (watch >= 0) inotify_rm_watch(in, watch);
                watch = inotify_add_watch(in, dir, IN_CREATE | IN_MOVED_TO);
                directory = watch >= 0;
            }
            fd = open(rd->path, O_RDONLY | O_CLOEXEC);
            if (fd < 0 && errno != ENOENT) retry = 1;
            struct stat st;
            if (fd >= 0 && fstat(fd, &st)) { close(fd); fd = -1; retry = 1; }
            if (fd >= 0) {
                inode = st.st_ino; at = first && rd->skip_existing ? st.st_size : 0; lines.used = 0; lines.lost = 0;
                if (at) fprintf(stderr, "[totk-ctl] %s: %lld bytes from before this start skipped\n", rd->path, (long long)at);
                if (in >= 0) {
                    if (watch >= 0) inotify_rm_watch(in, watch);
                    watch = inotify_add_watch(in, rd->path, IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
                    directory = 0;
                }
            }
        }
        if (fd >= 0) {
            struct stat st, now;
            if (fstat(fd, &st) || stat(rd->path, &now) || now.st_ino != inode || st.st_size < at) {
                close(fd); fd = -1; continue;   /* replaced, removed or truncated: start over */
            }
            ssize_t r;
            while ((r = pread(fd, data, sizeof data, at)) > 0) { at += r; totk_live_feed(&lines, data, (size_t)r); }
            if (r < 0) retry = 1;   /* e.g. a context change after open: do not wait forever for another write */
            if (in >= 0 && watch < 0)
                watch = inotify_add_watch(in, rd->path, IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
        }
        if (in >= 0 && watch >= 0) {
            struct timespec since;
            if (retry) clock_gettime(CLOCK_MONOTONIC, &since);
            for (;;) {
                if (retry) {
                    struct pollfd pending = {.fd = in, .events = POLLIN};
                    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                    int ms = 250 - (int)((now.tv_sec - since.tv_sec) * 1000 + (now.tv_nsec - since.tv_nsec) / 1000000);
                    if (ms <= 0) break;   /* unrelated folder traffic must not postpone a failed-open retry */
                    int ready = poll(&pending, 1, ms);
                    if (ready < 0 && errno == EINTR) continue;
                    if (!ready) break;   /* bounded recovery even if permission/context recovery emits no event */
                    if (ready < 0) { close(in); in = watch = -1; directory = 0; break; }
                }
                ssize_t r = read(in, events, sizeof events);
                if (r < 0 && errno == EINTR) continue;   /* retry the blocking read, not the open or watch */
                if (r <= 0) { close(in); in = watch = -1; directory = 0; break; }
                int ours = 0, lost = 0;
                for (ssize_t i = 0; i < r; ) {
                    const struct inotify_event *ev = (const struct inotify_event *)(events + i);
                    if (ev->mask & IN_Q_OVERFLOW) ours = 1;   /* our event may have been lost: recheck the path */
                    if (ev->wd == watch) {
                        if (ev->mask & IN_IGNORED) lost = 1;
                        else if (fd >= 0 || (ev->len && !strcmp(ev->name, name)))
                            ours = 1;
                    }
                    i += (ssize_t)sizeof *ev + ev->len;
                }
                if (lost) { watch = -1; directory = 0; }
                /* A removed watch's IN_IGNORED and unrelated folder events do not reopen the file or
                   rearm the directory. Stay blocked until a relevant event (or a failed-open retry deadline). */
                if (ours || lost) break;
            }
        } else usleep(250000);
        __atomic_fetch_add(&rd->wakeups, 1, __ATOMIC_RELAXED);
    }
}
#else
static inline void totk_live_prio_apply(uint32_t gen) { (void)gen; }
static inline void totk_live_slack_apply(void) {}
static inline void totk_live_slack_tick(void) {}
#endif

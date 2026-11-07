/* totk.log -> logcat (frontend_android.c), as code the host test (tests/test_log_mirror.py) runs unchanged.
   stdout and stderr go straight into totk.log (log_start), so nothing is lost when the process ends with _exit or a
   signal; this mirror only copies what is already in the file to logcat, from a thread of its own:
   - log_mirror_feed splits the file's bytes into complete lines (a partial last line waits for its newline), cuts a
     line longer than LOG_MIRROR_LINE at a UTF-8 character boundary and marks it " [...]", and drops empty lines;
   - log_mirror_level: FATAL, CRASH or "device lost" -> error, a red player-facing message (【红】 or [RED]) -> warning,
     anything else -> info;
   - log_mirror_run (Linux / Android) sleeps in read() on an inotify IN_MODIFY watch of the file, so an idle log costs
     no wakeup at all; without inotify it polls every 20 ms. */
#pragma once
#include <stddef.h>
#include <string.h>

#define LOG_MIRROR_LINE 1000   /* bytes of one line kept (logcat's own limit per entry is ~4 KiB) */
enum { LOG_MIRROR_INFO, LOG_MIRROR_WARN, LOG_MIRROR_ERROR };
/* Mirroring is diagnostic work: only an explicit launch opt-in may start its worker. */
static inline int log_mirror_enabled(const char *value) { return value && !strcmp(value, "1"); }
typedef void (*log_mirror_emit)(void *context, int level, const char *line);
typedef struct {
    char line[LOG_MIRROR_LINE + 8];   /* + room for the cut mark and the terminator */
    size_t used;
    int cut;                          /* this line was cut: drop its bytes up to the newline */
} log_mirror;

static inline int log_mirror_level(const char *line) {
    if (strstr(line, "FATAL") || strstr(line, "CRASH") || strstr(line, "device lost")) return LOG_MIRROR_ERROR;
    if (strstr(line, "[RED]") || strstr(line, "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91")) return LOG_MIRROR_WARN;   /* 【红】 */
    return LOG_MIRROR_INFO;
}
static inline void log_mirror_line(log_mirror *m, log_mirror_emit emit, void *context) {
    size_t n = m->used;
    if (n && m->line[n - 1] == '\r') n--;
    m->line[n] = 0;
    if (n) emit(context, log_mirror_level(m->line), m->line);
}
static inline void log_mirror_feed(log_mirror *m, const char *data, size_t n, log_mirror_emit emit, void *context) {
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!m->cut) log_mirror_line(m, emit, context);
            m->used = 0; m->cut = 0;
            continue;
        }
        if (m->cut) continue;
        if (m->used == LOG_MIRROR_LINE) {
            /* c does not fit: end before the character it belongs to (c itself, or the multi-byte character whose
               lead byte is already stored), so the cut line stays valid UTF-8. */
            size_t end = m->used;
            if (((unsigned char)c & 0xc0) == 0x80) {
                while (end && ((unsigned char)m->line[end - 1] & 0xc0) == 0x80) end--;
                if (end) end--;
            }
            memcpy(m->line + end, " [...]", 7);
            emit(context, log_mirror_level(m->line), m->line);
            m->used = 0; m->cut = 1;
            continue;
        }
        m->line[m->used++] = c ? c : ' ';   /* a NUL inside a line would end the logcat message early */
    }
}

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
typedef struct {
    const char *path;
    log_mirror_emit emit;
    void *context;
    int fd, watch;            /* the log (read only) and the inotify descriptor (-1: 20 ms polling) */
    long tid;                 /* the thread that runs log_mirror_run (for /proc/self/task/<tid>/status) */
    unsigned long wakeups;    /* times the loop woke (event or poll); only that thread writes it */
    log_mirror lines;
} log_mirror_tail;
/* 0, or errno when the log cannot be opened. Reading starts at the beginning: lines written before the thread
   started are mirrored too. */
static inline int log_mirror_open(log_mirror_tail *t) {
    memset(&t->lines, 0, sizeof t->lines); t->wakeups = 0; t->watch = -1;
    t->fd = open(t->path, O_RDONLY | O_CLOEXEC);
    if (t->fd < 0) return errno;
    int in = inotify_init1(IN_CLOEXEC);
    if (in >= 0 && inotify_add_watch(in, t->path, IN_MODIFY) < 0) { close(in); in = -1; }
    t->watch = in;
    return 0;
}
static inline void log_mirror_run(log_mirror_tail *t) {
    char data[16384], events[4096];
    __atomic_store_n(&t->tid, (long)syscall(SYS_gettid), __ATOMIC_RELEASE);
    for (;;) {
        ssize_t r;
        while ((r = read(t->fd, data, sizeof data)) > 0) log_mirror_feed(&t->lines, data, (size_t)r, t->emit, t->context);
        if (!r) {   /* truncated by someone else: start over rather than wait past the end forever */
            struct stat st; off_t at = lseek(t->fd, 0, SEEK_CUR);
            if (at > 0 && !fstat(t->fd, &st) && st.st_size < at) { lseek(t->fd, 0, SEEK_SET); t->lines.used = 0; t->lines.cut = 0; continue; }
        }
        if (t->watch >= 0) {
            /* Blocks until the next write; events that piled up meanwhile are drained by the one read. */
            if (read(t->watch, events, sizeof events) < 0 && errno != EINTR) { close(t->watch); t->watch = -1; }
        } else usleep(20000);
        __atomic_fetch_add(&t->wakeups, 1, __ATOMIC_RELAXED);
    }
}
#endif

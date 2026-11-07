/* The progress watchdog's decisions (threads.c runs them on a thread of its own, polling every PROGRESS_WATCH_POLL_NS),
   as a pure state machine for the host test (tests/test_progress_watch.py). Progress is the last presented frame's
   existing monotonic timestamp (hle_nvn.c runtime_progress_mark), not another clock read on the present path:
   - nothing before the first progress (boot), nor while paused (background, no window);
   - a held stage (a loading screen, a host movie, the first-world pipeline warm-up) counts only once it has lasted
     PROGRESS_WATCH_HOLD_NS, so a long load is not a stall but a load that never ends still is;
   - a stall of PROGRESS_WATCH_STALL_NS gives one thread dump; the same stall never gives a second one, and dumps are at
     most PROGRESS_WATCH_DUMPS per process, PROGRESS_WATCH_GAP_NS apart;
   - with exit_ns (TOTK_WATCH_STALL), a stall that long ends the process;
   - a poll that comes much later than due means the whole process was stopped (suspended app, debugger): the stall
     restarts at that poll, as it does after a pause, instead of counting the time nothing ran. */
#pragma once
#include <stdint.h>

#define PROGRESS_WATCH_POLL_NS  UINT64_C(250000000)
#define PROGRESS_WATCH_STALL_NS UINT64_C(1000000000)
#define PROGRESS_WATCH_GAP_NS   UINT64_C(10000000000)
#define PROGRESS_WATCH_HOLD_NS  UINT64_C(60000000000)
#define PROGRESS_WATCH_DUMPS    3u
enum { PROGRESS_WATCH_NONE, PROGRESS_WATCH_DUMP, PROGRESS_WATCH_EXIT };
typedef struct {
    uint64_t exit_ns;          /* 0: never exit */
    uint64_t last_poll;        /* the previous poll */
    uint64_t floor;            /* stalls are measured from no earlier than this (resume, a stopped process) */
    uint64_t last_dump;        /* when the last dump was given */
    uint64_t dumped_from;      /* the start of the stall that was dumped */
    unsigned dumps;
} progress_watch;

/* progress: the last progress time (0: none yet); *stalled: the stall measured, for the report. */
static inline unsigned progress_watch_poll(progress_watch *w, uint64_t now, uint64_t progress, int held, int paused,
                                           uint64_t *stalled) {
    *stalled = 0;
    if (w->last_poll && now > w->last_poll && now - w->last_poll > 4 * PROGRESS_WATCH_POLL_NS) w->floor = now;
    w->last_poll = now;
    if (paused) { w->floor = now; return PROGRESS_WATCH_NONE; }
    if (!progress) return PROGRESS_WATCH_NONE;
    uint64_t from = progress > w->floor ? progress : w->floor;
    if (now <= from) return PROGRESS_WATCH_NONE;
    *stalled = now - from;
    if (held && *stalled < PROGRESS_WATCH_HOLD_NS) return PROGRESS_WATCH_NONE;
    if (w->exit_ns && *stalled >= w->exit_ns) return PROGRESS_WATCH_EXIT;
    if (*stalled < PROGRESS_WATCH_STALL_NS || w->dumped_from == from || w->dumps >= PROGRESS_WATCH_DUMPS ||
        (w->dumps && now - w->last_dump < PROGRESS_WATCH_GAP_NS)) return PROGRESS_WATCH_NONE;
    w->dumps++; w->last_dump = now; w->dumped_from = from;
    return PROGRESS_WATCH_DUMP;
}

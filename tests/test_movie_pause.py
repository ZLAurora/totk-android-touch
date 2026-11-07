"""Run the production pause clock and movie scheduler with simulated monotonic time.

No VP9/ARM dependency: queued decoded frames exercise the actual due/drop/present
functions, while the pause gate's production clock_gettime calls use a fake clock.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which(os.environ.get('CC', 'cc'))

PRELUDE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static uint64_t fake_ns = 1000000000ull;
static int fake_clock_gettime(clockid_t clock, struct timespec *t) {
    assert(clock == CLOCK_MONOTONIC);
    t->tv_sec = fake_ns / 1000000000ull;
    t->tv_nsec = fake_ns % 1000000000ull;
    return 0;
}
#define clock_gettime fake_clock_gettime
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include "android_pause.h"
#undef clock_gettime
static AndroidGamePause game_pause = ANDROID_GAME_PAUSE_INIT;
AndroidGameSuspension runtime_android_game_suspension(void) {
    return android_game_pause_suspension(&game_pause);
}
typedef int vpx_codec_ctx_t; /* decoder state is not used by scheduling */
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
'''
SUPPORT = r'''
static Movie *g_movies[MAX_MOVIES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t fake_clock(void) { return fake_ns; }
static uint64_t (*g_clock)(void) = fake_clock;
typedef struct { void *ev[8]; int n; } Signals;
static void note(Movie *m, Signals *s, int which) { (void)m; (void)which; s->n++; }
static void fire(const Signals *s) { (void)s; }
static void queue(Movie *m, int index, int64_t pts, unsigned flags) {
    m->decoded[m->ndecoded++] = (Ready){index, flags, pts};
    m->out_held[index] = 1;
}
static int take(Movie *m) {
    assert(m->nready == 1);
    Ready r = m->ready[0];
    m->nready = 0;
    m->out_held[r.index] = 0;
    return r.index;
}
'''
MAIN = r'''
int main(void) {
    Movie m = {.started = 1, .mu = PTHREAD_MUTEX_INITIALIZER};
    Movie second = {.started = 1, .mu = PTHREAD_MUTEX_INITIALIZER};
    g_movies[0] = &m;
    queue(&m, 0, 900000, 0);
    queue(&m, 1, 933333, 0);
    queue(&m, 2, 966666, 0);
    queue(&m, 3, 999999, 0);
    movie_host_tick();
    assert(take(&m) == 0 && m.t0 == fake_ns && m.pts0 == 900000);
    uint64_t due = due_at(&m, &m.decoded[0]);
    fake_ns += 10000000ull; /* ten ms into the next movie frame */
    android_game_pause_set(&game_pause, 1);
    fake_ns += 10000000000ull;
    android_game_pause_set(&game_pause, 1); /* duplicate lifecycle/focus pause */
    fake_ns += 20000000000ull;
    android_game_pause_set(&game_pause, 0);
    android_game_pause_set(&game_pause, 0);
    movie_host_tick();
    assert(m.dropped == 0 && m.ndecoded == 3 && m.nready == 0);
    assert(due_at(&m, &m.decoded[0]) == due + 30000000000ull);
    fake_ns = due_at(&m, &m.decoded[0]) - 1;
    movie_host_tick();
    assert(!m.nready);
    fake_ns++;
    movie_host_tick();
    assert(take(&m) == 1 && !m.dropped);

    /* Save completion can admit presents during suspension. Both movies remain
       frozen, including a decoder whose very first frame arrives while paused. */
    fake_ns += 5000000ull;
    android_game_pause_set(&game_pause, 1);
    g_movies[1] = &second;
    queue(&second, 0, 0, 0);
    for (int i = 0; i < 3; i++) {
        fake_ns += 10000000000ull;
        movie_host_tick();
        assert(!m.nready && !second.nready && !second.t0);
    }
    uint64_t after_save_frames = m.t0;
    android_game_pause_set(&game_pause, 0);
    movie_host_tick();
    assert(m.t0 == after_save_frames && !m.dropped && !m.nready);
    assert(take(&second) == 0 && second.t0 == fake_ns);
    assert(m.suspended_ns == 60000000000ull);
    fake_ns = due_at(&m, &m.decoded[0]);
    movie_host_tick();
    assert(take(&m) == 2 && !m.dropped);

    /* A genuine foreground hitch still drops late frames: pause correction
       must not disable the existing A/V catch-up policy. */
    queue(&m, 4, 1033332, 0);
    queue(&m, 5, 1066665, 0);
    fake_ns = m.t0 + 200000000ull;
    movie_host_tick();
    assert(take(&m) == 5 && m.dropped == 2);
    queue(&m, 6, 0, FLAG_EOS);
    movie_host_tick();
    assert(take(&m) == 6);
    puts("movie pause: due times shifted by 60 s; zero pause drops; save frames frozen; foreground catch-up retained");
    return 0;
}
'''


@unittest.skipUnless(CC, 'needs a C compiler')
class MoviePause(unittest.TestCase):
    def test_suspension_rebases_production_movie_scheduler(self):
        assert CC is not None
        runtime = ROOT / 'games/totk/runtime'
        source = (runtime / 'movie_host.c').read_text()
        types = source[source.index('enum { IN_BUFS'):source.index('static MovieGuest G;')]
        scheduling = source[source.index('/* Host time a decoded frame is due'):source.index('/* Decode one input')]
        with tempfile.TemporaryDirectory(prefix='totk-movie-pause-') as directory:
            work = Path(directory)
            fixture = work / 'pause.c'
            fixture.write_text(PRELUDE + types + SUPPORT + scheduling + MAIN)
            binary = work / 'pause'
            subprocess.run([CC, '-D_POSIX_C_SOURCE=200809L', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-pthread', '-I', str(runtime), str(fixture), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('due times shifted by 60 s; zero pause drops', result.stdout)


if __name__ == '__main__':
    unittest.main()

"""Exercise the production pause worker against AAudio's legacy cached-state edge.

On Odin Android 15, requestPause stopped the callback but getState kept returning
PAUSING until waitForStateChange serviced the state machine. Resume must await
pause/flush completion, including when foreground returns before flush completes.
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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
typedef int aaudio_result_t;
typedef int aaudio_stream_state_t;
enum { AAUDIO_OK, AAUDIO_ERROR_TIMEOUT, AAUDIO_STREAM_STATE_STARTED,
       AAUDIO_STREAM_STATE_PAUSING, AAUDIO_STREAM_STATE_PAUSED,
       AAUDIO_STREAM_STATE_FLUSHING, AAUDIO_STREAM_STATE_FLUSHED };
typedef struct {
    void *unit;
    unsigned background;
    _Atomic int lost;
    _Atomic uint64_t wr, flush_until;
    _Atomic unsigned stop_epoch;
} Mixer;
static int paused, cached = AAUDIO_STREAM_STATE_STARTED;
static unsigned waits, starts, flushes;
static int audio_mixer_output_paused(void) { return paused; }
static int AAudioStream_requestPause(void *s) {
    (void)s; assert(cached == AAUDIO_STREAM_STATE_STARTED);
    cached = AAUDIO_STREAM_STATE_PAUSING; return AAUDIO_OK;
}
static int AAudioStream_getState(void *s) { (void)s; return cached; }
static int AAudioStream_waitForStateChange(void *s, int from, int *next, int64_t timeout) {
    (void)s; assert(timeout > 0 && timeout <= 20000000); assert(from == cached);
    if (cached == AAUDIO_STREAM_STATE_PAUSING) cached = AAUDIO_STREAM_STATE_PAUSED;
    else { assert(cached == AAUDIO_STREAM_STATE_FLUSHING); cached = AAUDIO_STREAM_STATE_FLUSHED; }
    *next = cached; waits++; return AAUDIO_OK;
}
static int AAudioStream_requestFlush(void *s) {
    (void)s; assert(cached == AAUDIO_STREAM_STATE_PAUSED);
    cached = AAUDIO_STREAM_STATE_FLUSHING; flushes++; return AAUDIO_OK;
}
static int AAudioStream_requestStart(void *s) {
    (void)s; assert(cached == AAUDIO_STREAM_STATE_FLUSHED);
    cached = AAUDIO_STREAM_STATE_STARTED; starts++; return AAUDIO_OK;
}
'''
MAIN = r'''
int main(void) {
    Mixer m = {.wr = 1234};
    paused = 1;
    output_background(&m);
    assert(m.background == 2 && flushes == 1 && waits == 1 && !starts);
    output_background(&m);
    assert(m.background == 3 && waits == 2 && !starts && !m.lost);
    paused = 0;
    output_background(&m);
    assert(!m.background && starts == 1 && m.flush_until == 1234 && m.stop_epoch == 1);
    paused = 1;
    output_background(&m);
    paused = 0; /* foreground returned while the flush was still pending */
    m.wr = 5678;
    output_background(&m);
    assert(!m.background && starts == 2 && flushes == 2 && waits == 4 && !m.lost);
    assert(m.flush_until == 5678 && m.stop_epoch == 2);
    puts("legacy cached states serviced; pause/flush ordered before fresh resume");
    return 0;
}
'''


@unittest.skipUnless(CC, 'needs a C compiler')
class AudioPause(unittest.TestCase):
    def test_legacy_state_machine_and_early_resume(self):
        assert CC is not None  # narrowed after skipUnless for the compiler invocation
        source = (ROOT / 'games/totk/runtime/audio_mixer.c').read_text()
        begin = source.index('static void output_background(Mixer *m) {')
        end = source.index('/* Reopen streams whose device went away', begin)
        with tempfile.TemporaryDirectory(prefix='totk-audio-pause-') as directory:
            work = Path(directory)
            fixture = work / 'pause.c'
            fixture.write_text(PRELUDE + source[begin:end] + MAIN)
            binary = work / 'pause'
            subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('legacy cached states serviced; pause/flush ordered before fresh resume', result.stdout)


if __name__ == '__main__':
    unittest.main()

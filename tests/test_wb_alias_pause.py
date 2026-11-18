"""Execute the production Android pause transition with optional backend hooks."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WritebackAliasPause(unittest.TestCase):
    def test_report_once_per_pause_edge_and_optional_backend(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('host C compiler required')
        source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        begin = source.index('static int game_activity_paused, game_focus_paused;')
        end = source.index('void runtime_android_game_focus(', begin)
        transition = source[begin:end]
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
unsigned reports;
static int game_pause, audio_edges, input_edges, timeline_edges, cache_edges;
static int64_t game_input_after_ns;
void vk_wb_alias_report(const char *) __attribute__((weak));
static int runtime_android_game_paused(void) { return game_pause; }
static uint64_t mono_ns(void) { return 123; }
static void android_game_pause_set(int *p, int paused) { *p = paused; }
static void runtime_audio_output_pause(int p) { assert(p == game_pause); audio_edges++; }
static void runtime_input_clear(void) { input_edges++; }
static void present_timeline_lifecycle(int d, int p) { assert(d == -1 && p == game_pause); timeline_edges++; }
static void vk_pcache_background(int p) { assert(p == game_pause); cache_edges++; }
'''
        main = r'''
int main(void) {
    android_game_pause_update();
    game_activity_paused = 1; android_game_pause_update();
    android_game_pause_update();
    game_focus_paused = 1; android_game_pause_update();
    game_activity_paused = 0; android_game_pause_update();
    game_focus_paused = 0; android_game_pause_update();
    game_activity_paused = 1; android_game_pause_update();
    android_game_pause_update();
    assert(reports == (WITH_BACKEND ? 2u : 0u));
    assert(audio_edges == 3 && input_edges == 3 && timeline_edges == 3 && cache_edges == 3);
    assert(game_pause && game_input_after_ns == 123);
    puts("pause-report PASS");
    return 0;
}
'''
        hook = r'''
#include <assert.h>
#include <string.h>
extern unsigned reports;
void vk_wb_alias_report(const char *phase) { assert(!strcmp(phase, "pause")); reports++; }
'''
        scratch = next((p / '.scratch' for p in ROOT.parents if (p / '.scratch').is_dir()), ROOT / '.scratch')
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='wb-alias-pause-', dir=scratch) as temporary:
            work = Path(temporary)
            probe = work / 'pause.c'; probe.write_text(prelude + transition + main)
            backend = work / 'backend.c'; backend.write_text(hook)
            for present in (0, 1):
                with self.subTest(backend=present):
                    executable = work / f'pause-{present}'
                    paths = [str(probe), *([str(backend)] if present else [])]
                    subprocess.run([*compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra',
                                    f'-DWITH_BACKEND={present}', *paths, '-o', str(executable)],
                                   check=True, capture_output=True, text=True, timeout=60)
                    result = subprocess.run([str(executable)], check=True, capture_output=True,
                                            text=True, timeout=10)
                    self.assertIn('pause-report PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()

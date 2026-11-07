"""The Android setup JVM's actual shared owner, launch handshake, and native OFD/move interoperability.
No game data: the move stops at missing_input after its production lock acquisition.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
SETUP = GAME / 'android/setup'
JAVA = GAME / 'android/java/com/aure/totk/setup'

PROBE = r'''package com.aure.totk.setup;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.*;
public final class GameLockProbe {
    private static native String move(String request);
    private static void check(boolean value, String why) { if (!value) throw new AssertionError(why); }
    private static void concurrent(File dir) throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(32);
        CountDownLatch start = new CountDownLatch(1);
        List<Future<?>> futures = new ArrayList<>();
        for (int i = 0; i < 64; i++) futures.add(pool.submit(() -> {
            start.await();
            for (int n = 0; n < 200; n++) try (GameLock.Hold held = GameLock.hold(dir)) {
                check(held.result.free(), held.result.reason);
                check(GameLock.check(dir, false).free(), "concurrent probe called setup's own lease GAME");
            }
            return null;
        }));
        start.countDown();
        try { for (Future<?> future : futures) future.get(); }
        finally { pool.shutdownNow(); }
    }
    private static int nativeLock(String exe, File dir) throws Exception {
        Process p = new ProcessBuilder(exe, "lock", new File(dir, "state/game.lock").getPath()).redirectError(ProcessBuilder.Redirect.INHERIT).start();
        p.getInputStream().readAllBytes();
        return p.waitFor();
    }
    public static void main(String[] args) throws Exception {
        GameLock.logger = message -> {};
        File dir = new File(args[1]);
        switch (args[0]) {
            case "state": {
                GameLock.Result r = GameLock.check(dir, Boolean.parseBoolean(args[2]));
                System.out.println(r.state + "|" + r.reason); break;
            }
            case "concurrent": concurrent(dir); System.out.println("FREE"); break;
            case "lease": {
                System.load(args[2]);
                try (GameLock.Hold held = GameLock.hold(dir)) {
                    held.requireFree();
                    concurrent(dir);
                    check(nativeLock(args[3], dir) == 3, "game acquired OFD while shared lease held");
                    String request = "{\"from\":\"" + dir + "/missing\",\"to\":\"" + dir + "/target\",\"privateDir\":\"" + dir + "\",\"gameLockHeld\":true}";
                    String result = move(request);
                    check(result.contains("missing_input"), "move did not reach install validation under lease: " + result);
                    check(nativeLock(args[3], dir) == 3, "native move/probe closed a fd and released the shared POSIX lease");
                    concurrent(dir);
                    check(nativeLock(args[3], dir) == 3, "concurrent close released the outer lease");
                }
                check(nativeLock(args[3], dir) == 0, "game did not acquire OFD after final release");
                System.out.println("lease preserved across concurrent checks and native move"); break;
            }
            case "handshake": {
                File marker = new File(dir, "state/game-starting");
                GameLock.start(dir, 123456789L, 321, () -> {
                    try { check(Files.readString(marker.toPath()).equals("time=123456789 pid=321\n"), "start marker payload"); }
                    catch (IOException failed) { throw new UncheckedIOException(failed); }
                    check(GameLock.check(dir, false).state == GameLock.State.GAME, "launch gap was not covered");
                });
                check(GameLock.check(dir, true).state == GameLock.State.GAME, "live marker not GAME");
                Process finished = new ProcessBuilder(args[2], "finish", dir.getPath()).start();
                check(finished.waitFor() == 0, "native marker removal failed");
                check(!marker.exists() && GameLock.check(dir, true).free(), "cached process or removed marker counted as game");
                System.out.println("marker handshake complete"); break;
            }
            case "refused": {
                try { GameLock.start(dir, 1, 2, () -> { throw new IllegalStateException("refused"); }); }
                catch (IllegalStateException expected) { }
                check(!new File(dir, "state/game-starting").exists(), "refused launch left marker");
                System.out.println("FREE"); break;
            }
            case "close-transport": {
                check(GameLock.check(dir, false).free(), "initial lock");
                File request = new File(dir, "state/close-request");
                GameLock.requestClose(dir, "123:456");
                check(Files.readString(request.toPath()).equals("123:456\n"), "atomic request payload");
                check(!new File(dir, "state/close-request.tmp").exists(), "request temp left behind");
                Process busy = new ProcessBuilder(args[2], "close-busy", dir.getPath()).start();
                check(busy.waitFor() == 0, "busy poll did not run");
                check(!request.exists() && GameLock.closeResponse(dir, "123:456").equals("saving"), "missing correlated saving refusal");
                check(GameLock.closeResponse(dir, "another").isEmpty(), "old refusal applied to another request");
                GameLock.clearClose(dir, "123:456");
                GameLock.requestClose(dir, "123:789");
                Process accepted = new ProcessBuilder(args[2], "close-accepted", dir.getPath()).start();
                check(accepted.waitFor() == 0 && !request.exists(), "accepted close did not consume request before exit");
                check(GameLock.closeResponse(dir, "123:789").isEmpty(), "accepted close left a refusal");
                Files.createDirectory(request.toPath());
                Process failed = new ProcessBuilder(args[2], "close-error", dir.getPath()).start();
                check(failed.waitFor() == 0, "I/O error poll did not run");
                check(GameLock.closeResponse(dir, "123:789").contains("error: read close-request errno="), "I/O error lost its reason/errno");
                System.out.println("atomic marker close transport complete"); break;
            }
        }
    }
}
'''

NATIVE = r'''#define _GNU_SOURCE
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "android_storage.h"
#include "setup_internal.h"
JNIEXPORT jstring JNICALL Java_com_aure_totk_setup_GameLockProbe_move(JNIEnv *env, jclass cls, jstring request) {
    (void)cls;
    const char *text = (*env)->GetStringUTFChars(env, request, NULL);
    if (!text) return NULL;
    char *result = setup_move_json(text, strlen(text), NULL, NULL, NULL, NULL, NULL);
    (*env)->ReleaseStringUTFChars(env, request, text);
    jstring value = result ? (*env)->NewStringUTF(env, result) : NULL;
    free(result); return value;
}
static int close_busy(void) { return 0; }
static int close_accepted(void) { _exit(0); }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "finish")) return android_game_start_finished(argv[2]) ? 4 : 0;
    if (!strncmp(argv[1], "close-", 6)) {
        AndroidClosePaths paths;
        if (android_close_paths(&paths, argv[2])) return 5;
        int result = android_close_poll(&paths, !strcmp(argv[1], "close-accepted") ? close_accepted : close_busy);
        return !strcmp(argv[1], "close-error") ? (result == -1 ? 0 : 6) : (result == 1 ? 0 : 7);
    }
    char why[600];
    int fd = android_game_lock(argv[2], 0, why, sizeof why);
    if (fd < 0) { puts(why); return 3; }
    puts("OFD acquired"); fflush(stdout);
    if (!strcmp(argv[1], "hold")) getchar();
    close(fd); return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux'), 'OFD locks require Linux')
class GameLockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls_java, cc = shutil.which('javac'), shutil.which('java'), shutil.which('cc')
        if not javac or not cls_java or not cc:
            raise unittest.SkipTest('JDK and host C compiler are required')
        cls.java = cls_java
        scratch = Path(os.environ.get('TOTK_SETUP_WORK', ROOT / '.scratch/setup-tests')).resolve() / 'game-lock'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='lock-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        probe, native = cls.work / 'GameLockProbe.java', cls.work / 'native.c'
        probe.write_text(PROBE)
        native.write_text(NATIVE)
        cls.classes = cls.work / 'classes'
        cls.run_command([javac, '--release', '17', '-d', cls.classes, probe,
                         *[JAVA / name for name in ('GameLock.java', 'SaveManager.java', 'Settings.java')]])
        cls.run_command([sys.executable, GAME / 'scripts/make_pins_header.py', GAME / 'assets/expected-v1.4.3.json', cls.work / 'totk_pins.h'])
        jni = Path(javac).resolve().parents[1] / 'include'
        cls.lib, cls.exe = cls.work / 'native.so', cls.work / 'native'
        common = [cc, '-std=gnu11', '-O1', '-fPIC', '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
                  '-Wl,--gc-sections', '-I' + str(jni), '-I' + str(jni / 'linux'), '-I' + str(GAME / 'runtime'),
                  '-I' + str(SETUP), '-I' + str(cls.work), native,
                  *[SETUP / name for name in ('move.c', 'setup.c', 'json.c', 'util.c', 'sha256.c', 'romfs_image.c')], '-pthread', '-lm']
        cls.run_command([*common, '-shared', '-Wl,--no-undefined', '-o', cls.lib])
        cls.run_command([*common, '-o', cls.exe])

    @staticmethod
    def run_command(command):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=120)
        if result.returncode:
            raise AssertionError(f"Command failed ({result.returncode}): {command}\n{result.stdout}\n{result.stderr}")
        return result

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix='case-', dir=self.work))

    def probe(self, mode, *args):
        return self.run_command([self.java, '-cp', self.classes, 'com.aure.totk.setup.GameLockProbe', mode, self.case, *args]).stdout.strip()

    def state(self, alive=False):
        return self.probe('state', str(alive).lower()).split('|', 1)

    def test_same_jvm_concurrent_probes_are_free(self):
        self.assertEqual(self.probe('concurrent'), 'FREE')

    def test_shared_lease_survives_native_move_and_probes(self):
        self.assertIn('lease preserved', self.probe('lease', self.lib, self.exe))

    def test_marker_launch_gap_and_game_removal(self):
        self.assertEqual(self.probe('handshake', self.exe), 'marker handshake complete')
        self.assertEqual(self.probe('refused'), 'FREE')

    def test_atomic_close_marker_busy_error_and_accepted_exit(self):
        self.assertEqual(self.probe('close-transport', self.exe), 'atomic marker close transport complete')

    def test_stale_marker_is_removed_without_default_process(self):
        (self.case / 'state').mkdir()
        marker = self.case / 'state/game-starting'
        marker.write_text('time=1 pid=999999\n')
        self.assertEqual(self.state()[0], 'FREE')
        self.assertFalse(marker.exists())

    def test_live_marker_is_game_but_process_alone_is_free(self):
        self.assertEqual(self.state(True)[0], 'FREE')
        marker = self.case / 'state/game-starting'
        marker.write_text('time=1 pid=23\n')
        status, reason = self.state(True)
        self.assertEqual(status, 'GAME')
        self.assertIn('branch=start-marker', reason)
        self.assertTrue(marker.exists())

    def test_io_failure_is_error_never_free_or_game(self):
        (self.case / 'state').write_text('not a directory')
        status, reason = self.state()
        self.assertEqual(status, 'ERROR')
        self.assertIn('IOException', reason)
        (self.case / 'state').unlink()
        (self.case / 'state/game.lock').mkdir(parents=True)
        status, reason = self.state()
        self.assertEqual(status, 'ERROR')
        self.assertIn('FileNotFoundException', reason)
        (self.case / 'state/game.lock').rmdir()
        (self.case / 'state/game-starting').mkdir()
        self.assertEqual(self.state(True)[0], 'ERROR')

    def test_other_process_ofd_is_game(self):
        self.state()
        process = subprocess.Popen([self.exe, 'hold', self.case / 'state/game.lock'], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True)
        try:
            assert process.stdout is not None
            self.assertEqual(process.stdout.readline().strip(), 'OFD acquired')
            status, reason = self.state()
            self.assertEqual(status, 'GAME')
            self.assertIn('lock=contended', reason)
        finally:
            process.communicate('\n', timeout=10)
        self.assertEqual(self.state()[0], 'FREE')

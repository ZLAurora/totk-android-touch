"""The Android runtime's storage rules, compiled for the host from the production sources:

- TOTK_DATA_DIR validation (runtime/android_storage.h android_data_dir_check): private storage <private>/game, an
  existing, link-free <volume>/Android/data/com.aure.totk/files folder on a mounted volume, or a picked folder below a
  shared volume (<root>/emulated/<user>/..., <root>/<uuid>/...) only while All files access is granted (mount tables and
  the storage root are synthetic here).
- The game lock (android_game_lock): an open-file-description lock that conflicts with the classic POSIX record lock
  Java's FileChannel.tryLock takes (Python's fcntl.lockf is the same F_SETLK), waits for a brief hold, refuses a long one,
  and is released when the process ends.
- The save generation (runtime/fs_host.c commit): <TOTK_SAVE_GENERATION> grows by one on each Commit that completes a
  save, continuing from the file's value, and only then.
- A refused start (android_refused_exit): the process ends by itself shortly after, so the next start is a fresh one.
"""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'
SUFFIX = 'Android/data/com.aure.totk/files'

STORAGE_PROBE = r'''
#define _GNU_SOURCE
#include "android_storage.h"
int main(int argc, char **argv) {
    char why[600];
    if (argc == 7 && !strcmp(argv[1], "dir")) {   /* dir PATH MOUNTS PRIVATE STORAGE_ROOT ALL_FILES */
        AndroidDataDirs d = { "com.aure.totk", argv[4], argv[5], atoi(argv[6]) };
        int kind = android_data_dir_check(&d, argv[2], argv[3], why, sizeof why);
        if (kind < 0) { printf("refused: %s\n", why); return 1; }
        printf("ok %s\n", kind == ANDROID_DATA_PRIVATE ? "private" : kind == ANDROID_DATA_APP ? "app" : "folder"); return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "lock")) {   /* lock PATH WAIT_MS HOLD_MS */
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        int fd = android_game_lock(argv[2], atoi(argv[3]), why, sizeof why);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (fd < 0) { printf("refused after %ld ms: %s\n", ms, why); return 1; }
        printf("locked after %ld ms\n", ms); fflush(stdout);
        usleep((useconds_t)atoi(argv[4]) * 1000);
        return 0;   /* the kernel drops the lock with the process */
    }
    if (argc == 3 && !strcmp(argv[1], "refused")) {   /* refused DELAY_MS: exits by itself, never reaches the end */
        android_refused_exit((unsigned)atoi(argv[2]));
        sleep(30);
        return 3;
    }
    return 2;
}
'''

# fs_host.c with the few runtime functions it calls stubbed; the save probe's own entry points drive it.
RUNTIME_STUB = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static _Noreturn void runtime_fail(const char *m) { fprintf(stderr, "FAIL %s\n", m); exit(7); }
static uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
'''
AUTOLOAD_STUB = r'''
#pragma once
static void runtime_autoload_save_file_opened(const char *p) { (void)p; }
'''
SAVE_MAIN = r'''
#include "fs_host.c"
int runtime_romfs_owns(const char *p) { (void)p; return 0; }
int runtime_romfs_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_romfs_open(uint64_t *o, const char *p, int m) { (void)o; (void)p; (void)m; return 1; }
unsigned runtime_romfs_read(uint64_t *o, uint64_t h, int64_t off, void *b, uint64_t n) { (void)o; (void)h; (void)off; (void)b; (void)n; return 1; }
unsigned runtime_romfs_size(int64_t *o, uint64_t h) { (void)o; (void)h; return 1; }
void runtime_romfs_close(uint64_t h) { (void)h; }
unsigned runtime_romfs_entry_type(int *o, const char *p) { (void)o; (void)p; return 1; }
void runtime_account_uid(uint64_t out[2]) { out[0] = 0x1122334455667788ull; out[1] = 0x99aabbccddeeff00ull; }
void runtime_fs_log(const char *w, const char *p) { (void)w; (void)p; }
static const char *gen_path;
static void show(const char *step) {
    FILE *f = gen_path ? fopen(gen_path, "r") : NULL; char v[64] = "none";
    if (f) { if (!fgets(v, sizeof v, f)) snprintf(v, sizeof v, "empty"); fclose(f); v[strcspn(v, "\n")] = 0; }
    const char *busy = getenv("TOTK_SAVE_BUSY");
    printf("%s %s/%s\n", step, v, busy && !access(busy, F_OK) ? "busy" : "idle");
}
int main(void) {
    gen_path = getenv("TOTK_SAVE_GENERATION");
    uint64_t uid[2], h; runtime_account_uid(uid); static const char data[8] = "slot";
    if (mount_save("save", uid) || mount_temp("temp")) return 3;
    show("start");
    commit("save"); show("commit-without-changes");
    create_file("temp:/t.bin", 4); commit("temp"); show("temporary-storage-commit");
    create_file("save:/progress.sav", 8); open_file(&h, "save:/progress.sav", OPEN_WRITE); write_file(h, 0, data, sizeof data, NULL);
    commit("save"); show("commit-while-a-save-file-is-open");
    close_file(h); commit("save"); show("commit-after-close");
    commit("save"); show("second-commit-nothing-new");
    rename_file("save:/progress.sav", "save:/caption.sav"); commit("save"); show("rename-commit");
    delete_file("save:/caption.sav"); show("delete-not-committed-yet");
    commit("save"); show("delete-commit");
    /* A file whose flush fails (fsync on a pipe: EINVAL) sits in the handle table while a save is committed. */
    int pipefd[2]; if (pipe(pipefd)) return 4;
    create_file("save:/option.sav", 8);
    files[NFILES - 1] = (File){pipefd[0], OPEN_READ, 0};
    commit("save"); show("commit-whose-flush-failed");
    files[NFILES - 1].fd = 0;
    commit("save"); show("next-commit-succeeds");
    return 0;
}
'''

SAVE_CLOSE_MAIN = SAVE_MAIN[:SAVE_MAIN.index('int main(void)')] + r'''
static _Atomic int save_attempted, save_finished;
static void *late_save(void *unused) {
    (void)unused;
    atomic_store(&save_attempted, 1);
    create_file("save:/after-close.sav", 8);
    atomic_store(&save_finished, 1);
    return NULL;
}
int main(void) {
    uint64_t uid[2], h; runtime_account_uid(uid);
    if (mount_save("save", uid) || create_file("save:/before-close.sav", 8)
            || open_file(&h, "save:/before-close.sav", OPEN_WRITE)) return 3;
    if (runtime_fs_begin_close()) return 4;  /* save first: no close, no loss */
    commit("save");
    if (runtime_fs_begin_close()) return 5;  /* an open writer still forbids close */
    close_file(h);
    if (runtime_fs_begin_close()) return 6;  /* between files, before Commit */
    commit("save");
    const char *busy = getenv("TOTK_SAVE_BUSY");
    int fd = open(busy, O_WRONLY | O_CREAT, 0600); if (fd < 0) return 7; close(fd);
    if (runtime_fs_begin_close()) return 8;  /* physical save-busy alone also forbids close */
    unlink(busy);
    if (!runtime_fs_begin_close()) return 9;
    pthread_t thread;
    if (pthread_create(&thread, NULL, late_save, NULL)) return 10;
    while (!atomic_load(&save_attempted)) usleep(1000);
    usleep(100000);
    char path[1100];
    if (host_path("save:/after-close.sav", path, sizeof path)) return 11;
    if (atomic_load(&save_finished) || !access(path, F_OK) || !access(busy, F_OK)) return 12;
    puts("save-first refused; close-first save blocked before any change");
    fflush(stdout);
    _exit(0);   /* the blocked save is held only until the owning process exits */
}
'''

SAVE_PAUSE_MAIN = SAVE_MAIN[:SAVE_MAIN.index('int main(void)')] + r'''
#include "android_pause.h"
static AndroidGamePause pause_gate = ANDROID_GAME_PAUSE_INIT;
static _Atomic unsigned frames;
static void *guest_frames(void *unused) {
    (void)unused;
    for (;;) {
        android_game_pause_wait(&pause_gate, runtime_fs_save_pending);
        atomic_fetch_add(&frames, 1);
        usleep(1000);
    }
    return NULL;
}
static int advancing(void) {
    unsigned before = atomic_load(&frames);
    for (int i = 0; i < 1000; i++) {
        if (atomic_load(&frames) > before) return 1;
        usleep(1000);
    }
    return 0;
}
static int stopped(void) {
    usleep(100000);
    unsigned before = atomic_load(&frames);
    usleep(100000);
    return atomic_load(&frames) == before;
}
int main(void) {
    uint64_t uid[2]; runtime_account_uid(uid);
    if (mount_save("save", uid)) return 3;
    android_game_pause_set(&pause_gate, 1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, guest_frames, NULL) || !stopped()) return 4;
    /* A save beginning on another thread after the frame gate has slept must
       still get guest frames, even after the three-second UI busy heuristic. */
    if (create_file("save:/paused.sav", 8)) return 5;
    atomic_store(&save_changed_ns, runtime_now_ns() - 10000000000ull);
    if (runtime_fs_save_busy() || !runtime_fs_save_pending() || !advancing()) return 6;
    if (runtime_fs_begin_close()) return 7;
    if (commit("save") || runtime_fs_save_pending() || !stopped()) return 8;
    android_game_pause_set(&pause_gate, 0);
    if (!advancing()) return 9;
    android_game_pause_set(&pause_gate, 1);
    if (!stopped() || !runtime_fs_begin_close()) return 10;
    puts("paused frames held; save progresses through Commit; resume wakes; close admitted while paused");
    fflush(stdout);
    _exit(0); /* native close does not need the suspended guest or its gate */
}
'''


def compiler():
    return shutil.which(os.environ.get('CC', 'cc'))


@unittest.skipUnless(compiler(), 'needs a C compiler')
class Storage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='totk-android-storage-')
        cls.work = Path(cls.tmp.name)
        src = cls.work / 'storage_probe.c'; src.write_text(STORAGE_PROBE)
        cls.probe = cls.work / 'storage_probe'
        subprocess.run([compiler(), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(RUNTIME), str(src), '-pthread', '-o', str(cls.probe)], check=True)
        save = cls.work / 'save'; save.mkdir()
        shutil.copy(RUNTIME / 'fs_host.c', save / 'fs_host.c')
        (save / 'runtime.h').write_text(RUNTIME_STUB); (save / 'autoload.h').write_text(AUTOLOAD_STUB)
        (save / 'main.c').write_text(SAVE_MAIN)
        cls.save_probe = cls.work / 'save_probe'
        subprocess.run([compiler(), '-std=gnu11', '-O1', '-w', '-I', str(save), '-I', str(RUNTIME), str(save / 'main.c'), '-pthread', '-o', str(cls.save_probe)], check=True)
        (save / 'close.c').write_text(SAVE_CLOSE_MAIN)
        cls.close_probe = cls.work / 'close_probe'
        subprocess.run([compiler(), '-std=gnu11', '-O1', '-w', '-I', str(save), '-I', str(RUNTIME), str(save / 'close.c'),
                        '-pthread', '-o', str(cls.close_probe)], check=True)
        (save / 'pause.c').write_text(SAVE_PAUSE_MAIN)
        cls.pause_probe = cls.work / 'pause_probe'
        subprocess.run([compiler(), '-std=gnu11', '-O1', '-w', '-I', str(save), '-I', str(RUNTIME), str(save / 'pause.c'),
                        '-pthread', '-o', str(cls.pause_probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(dir=self.work))

    # ---- TOTK_DATA_DIR ----
    # self.dir plays /storage: cards are self.dir/<uuid>, internal shared storage self.dir/emulated/0; private storage is
    # self.dir/user/0/com.aure.totk/files/game (outside any volume).
    def check(self, path, mounts, all_files=1, private=None):
        table = self.dir / 'mounts'
        table.write_text(''.join(f'{dev} {mnt} {kind} rw 0 0\n' for dev, mnt, kind in mounts))
        private = private or self.dir / 'user/0/com.aure.totk/files'
        r = subprocess.run([self.probe, 'dir', str(path), str(table), str(private), str(self.dir), str(all_files)], capture_output=True, text=True)
        return r.returncode, r.stdout.strip()

    def card(self, name='AAAA-AAAA'):
        volume = self.dir / name
        (volume / SUFFIX).mkdir(parents=True)
        return volume, volume / SUFFIX

    def test_refused_start_ends_the_process(self):
        t0 = time.monotonic()
        r = subprocess.run([self.probe, 'refused', '300'], capture_output=True, text=True, timeout=20)
        took = time.monotonic() - t0
        self.assertEqual(r.returncode, 0)
        self.assertIn('this process ends', r.stderr)
        self.assertGreaterEqual(took, 0.25)
        self.assertLess(took, 5)

    def test_card_folder_accepted(self):
        volume, files = self.card()
        mounts = [('/dev/root', '/', 'ext4'), ('tmpfs', str(self.dir), 'tmpfs'), ('/dev/fuse', str(volume), 'fuse')]
        self.assertEqual(self.check(files, mounts), (0, 'ok app'))
        self.assertEqual(self.check(files, mounts, all_files=0), (0, 'ok app'))   # app folders never need the permission

    def test_wrong_suffix_relative_and_not_a_folder(self):
        volume, files = self.card()
        mounts = [('/dev/fuse', str(volume), 'fuse')]
        other = volume / 'Android/data/dev.kitcatkode.totk/files'; other.mkdir(parents=True)
        code, out = self.check(other, mounts)
        self.assertEqual(code, 1); self.assertIn("volume's Android folder", out)
        code, out = self.check(f'{volume.name}/{SUFFIX}', mounts)
        self.assertIn('not an absolute', out)
        code, out = self.check(volume / 'Android/data/com.aure.totk/files2', mounts)
        self.assertIn("volume's Android folder", out)
        plain = self.dir / 'FILE-VOL'; (plain / SUFFIX).parent.mkdir(parents=True); (plain / SUFFIX).write_text('x')
        code, out = self.check(plain / SUFFIX, [('/dev/fuse', str(plain), 'fuse')])
        self.assertIn('not a folder', out)
        code, out = self.check('/opt/elsewhere', mounts)
        self.assertEqual(code, 1); self.assertIn('not private storage, an app data folder', out)

    def test_missing_folder_or_card(self):
        volume, files = self.card()
        gone = self.dir / 'BBBB-BBBB' / SUFFIX
        code, out = self.check(gone, [('/dev/fuse', str(volume), 'fuse')])
        self.assertEqual(code, 1); self.assertIn('not available', out)

    def test_links_refused(self):
        volume, files = self.card()
        link = self.dir / 'LINK-VOL'; link.symlink_to(volume)
        mounts = [('/dev/fuse', str(volume), 'fuse'), ('/dev/fuse', str(link), 'fuse')]
        code, out = self.check(link / SUFFIX, mounts)
        self.assertEqual(code, 1); self.assertIn('link', out)
        inner = self.dir / 'CCCC-CCCC'; (inner / 'Android/data').mkdir(parents=True)
        (inner / 'Android/data/com.aure.totk').symlink_to(volume / 'Android/data/com.aure.totk')
        code, out = self.check(inner / SUFFIX, [('/dev/fuse', str(inner), 'fuse')])
        self.assertEqual(code, 1); self.assertIn('link', out)
        code, out = self.check(f'{volume}/Android/../Android/data/com.aure.totk/files', mounts)
        self.assertEqual(code, 1)
        (volume / 'Games').mkdir(); (volume / 'Linked').symlink_to(volume / 'Games')
        code, out = self.check(volume / 'Linked', mounts)
        self.assertEqual(code, 1); self.assertIn('link', out)

    def test_unmounted_volume_refused(self):
        volume, files = self.card()
        code, out = self.check(files, [('/dev/root', '/', 'ext4')])
        self.assertEqual(code, 1); self.assertIn('not on a mounted storage volume', out)
        # The card's mount point left behind on /storage's tmpfs after an unmount.
        code, out = self.check(files, [('/dev/root', '/', 'ext4'), ('tmpfs', str(self.dir), 'tmpfs')])
        self.assertEqual(code, 1); self.assertIn('tmpfs', out)
        (volume / 'TotK Native').mkdir()
        code, out = self.check(volume / 'TotK Native', [('/dev/root', '/', 'ext4'), ('tmpfs', str(self.dir), 'tmpfs')])
        self.assertEqual(code, 1); self.assertIn('tmpfs', out)

    def test_private_storage(self):
        files = self.dir / 'user/0/com.aure.totk/files'; (files / 'game').mkdir(parents=True)
        self.assertEqual(self.check(files / 'game', [('/dev/root', '/', 'ext4')], all_files=0), (0, 'ok private'))
        # Reached through a link to the files folder (like /data/data -> /data/user/0): the real paths still agree.
        alias = self.dir / 'data'; alias.symlink_to(self.dir / 'user/0')
        self.assertEqual(self.check(alias / 'com.aure.totk/files/game', [], private=alias / 'com.aure.totk/files'), (0, 'ok private'))
        # game itself a link to somewhere else: refused.
        shutil.rmtree(files / 'game'); (self.dir / 'elsewhere').mkdir(); (files / 'game').symlink_to(self.dir / 'elsewhere')
        code, out = self.check(files / 'game', [])
        self.assertEqual(code, 1); self.assertIn('link', out)
        shutil.rmtree(self.dir / 'elsewhere'); (files / 'game').unlink()
        code, out = self.check(files / 'game', [])
        self.assertEqual(code, 1); self.assertIn('not available', out)
        code, out = self.check(f'{files}/game/', [])
        self.assertEqual(code, 1)

    def test_picked_folder_needs_all_files_access(self):
        volume, _ = self.card()
        internal = self.dir / 'emulated/0'
        for folder in (volume / 'TotK Native', internal / 'Games/TotK Native'):
            folder.mkdir(parents=True)
            mounts = [('/dev/root', '/', 'ext4'), ('tmpfs', str(self.dir), 'tmpfs'), ('/dev/fuse', str(volume), 'fuse'),
                      ('/dev/fuse', str(self.dir / 'emulated'), 'fuse')]
            self.assertEqual(self.check(folder, mounts, all_files=1), (0, 'ok folder'))
            code, out = self.check(folder, mounts, all_files=0)
            self.assertEqual(code, 1); self.assertIn('needs All files access', out)
        mounts = [('/dev/fuse', str(volume), 'fuse')]
        code, out = self.check(volume, mounts)
        self.assertEqual(code, 1); self.assertIn('whole storage volume', out)
        (volume / 'Android/media').mkdir(parents=True)
        code, out = self.check(volume / 'Android/media', mounts)
        self.assertEqual(code, 1); self.assertIn("volume's Android folder", out)
        (self.dir / 'emulated/x/Games').mkdir(parents=True)
        code, out = self.check(self.dir / 'emulated/x/Games', mounts)
        self.assertEqual(code, 1); self.assertIn('not inside a storage volume', out)
        (self.dir / 'self/primary/Games').mkdir(parents=True)
        code, out = self.check(self.dir / 'self/primary/Games', mounts)
        self.assertEqual(code, 1); self.assertIn('not inside a storage volume', out)

    # ---- the game lock ----
    def lock_probe(self, path, wait_ms, hold_ms):
        return subprocess.Popen([self.probe, 'lock', str(path), str(wait_ms), str(hold_ms)], stdout=subprocess.PIPE, text=True)

    def test_waits_for_a_brief_launcher_hold(self):
        path = self.dir / 'game.lock'
        with open(path, 'a+') as held:
            fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB)   # FileChannel.tryLock: a classic POSIX record lock
            p = self.lock_probe(path, 4000, 0)
            time.sleep(0.5)
            fcntl.lockf(held, fcntl.LOCK_UN)
        out, _ = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 0, out)
        self.assertGreaterEqual(int(out.split()[2]), 400, out)

    def test_long_hold_refused(self):
        path = self.dir / 'game.lock'
        with open(path, 'a+') as held:
            fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            p = self.lock_probe(path, 300, 0)
            out, _ = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 1, out)
        self.assertIn('held by another TotK Native process', out)

    def test_launcher_sees_the_game_and_its_end(self):
        path = self.dir / 'game.lock'
        p = self.lock_probe(path, 0, 1500)
        self.assertIn('locked', p.stdout.readline())
        with open(path, 'a+') as probe:
            with self.assertRaises(OSError):
                fcntl.lockf(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)      # the game is running
            second = self.lock_probe(path, 0, 0)                       # a second game process
            out, _ = second.communicate(timeout=10)
            self.assertEqual(second.returncode, 1, out)
            p.wait(timeout=10); p.stdout.close()
            fcntl.lockf(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)          # gone with the process
            fcntl.lockf(probe, fcntl.LOCK_UN)

    # ---- save generation ----
    def test_generation_grows_only_on_completed_saves(self):
        state = self.dir / 'state'; state.mkdir()
        gen = state / 'save-generation'; gen.write_text('41\n')
        env = dict(os.environ, TOTK_PROFILE_DIR=str(self.dir / 'profile'), TOTK_SAVE_GENERATION=str(gen),
                   TOTK_SAVE_BUSY=str(state / 'save-busy'))
        r = subprocess.run([self.save_probe], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        steps = dict(line.split() for line in r.stdout.splitlines())
        # generation / save-busy marker after each step
        self.assertEqual(steps, {'start': '41/idle', 'commit-without-changes': '41/idle', 'temporary-storage-commit': '41/idle',
                                 'commit-while-a-save-file-is-open': '41/busy', 'commit-after-close': '42/idle',
                                 'second-commit-nothing-new': '42/idle', 'rename-commit': '43/idle',
                                 'delete-not-committed-yet': '43/busy', 'delete-commit': '44/idle',
                                 'commit-whose-flush-failed': '44/busy', 'next-commit-succeeds': '45/idle'})
        self.assertEqual(gen.read_text(), '45\n')
        self.assertEqual(sorted(p.name for p in state.iterdir()), ['save-generation'])   # no temporary left behind
        self.assertEqual(r.stderr.count('save committed: generation'), 4)
        self.assertIn('flushing a file failed', r.stderr)
        # Without the variable (desktop hosts) nothing is written.
        gen.unlink()
        env.pop('TOTK_SAVE_GENERATION')
        r = subprocess.run([self.save_probe], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(gen.exists())
        self.assertNotIn('save committed', r.stderr)

    def test_save_close_admission_both_orders(self):
        state = self.dir / 'state'; state.mkdir()
        env = dict(os.environ, TOTK_PROFILE_DIR=str(self.dir / 'profile'), TOTK_SAVE_BUSY=str(state / 'save-busy'))
        result = subprocess.run([self.close_probe], capture_output=True, text=True, env=env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('save-first refused; close-first save blocked before any change', result.stdout)

    def test_background_pause_allows_save_commit_and_close(self):
        state = self.dir / 'state'; state.mkdir()
        env = dict(os.environ, TOTK_PROFILE_DIR=str(self.dir / 'profile'), TOTK_SAVE_BUSY=str(state / 'save-busy'))
        result = subprocess.run([self.pause_probe], capture_output=True, text=True, env=env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('paused frames held; save progresses through Commit; resume wakes; close admitted while paused', result.stdout)


if __name__ == '__main__':
    unittest.main()

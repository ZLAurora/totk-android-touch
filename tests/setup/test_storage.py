"""Game data on another volume (android/setup, synthetic bytes only):

- RomFS parts: the romfs stage (decrypted route, and the container route through a FIFO with a stand-in hactool) writes
  equal-size parts romfs.bin.000 ... (the last one shorter) when romfsPartBytes asks for them or the volume is FAT; every
  setup reader (index stage and CLI, the shader seed's RomFS reader) gives the same results over parts as over one file;
  install.json romfs.parts; the runtime (runtime/romfs.c, compiled for the host) reads the parts and reuses setup's
  .romfs-verified-parts-v1 record; changing the layout removes the old one's files and record.
- Move (NativeSetup.move through `totk-setup move`): success both ways with a re-cut RomFS and a fresh record; cancellation
  at every phase, a full destination (preflight and a write failure half-way), a corrupted destination byte, a damaged
  source, a running game: each leaves the old copy byte-for-byte intact and nothing of the move in the destination.
- removeGameData (`totk-setup remove`): only the game data goes.
Run: python -m unittest discover -s tests/setup -p 'test_storage.py' -v"""
import fcntl
import hashlib
import json
import os
import random
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_import import (WORK, RUNTIME, PROBE_PRELUDE, HEADER, Base, DecryptedFixture, encrypt, decrypt, modes, nca, partition,  # noqa: E402
                         ticket, game_image, python_index, write_pins, sha)

ROMFS = 'extracted/v143/romfs'
PROOF_SIZE = 160   # sizeof(RomfsProof), runtime/romfs_proof.h
PARTS_MAIN = r'''
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(7); }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) { (void)stage; (void)done; (void)total; }
int main(int argc, char **argv) {   /* romfs-dir sha256 size: the runtime's own open (romfs.bin, else parts) and verification */
    if (argc != 4) return 2;
    romfs_image_sha256 = argv[2]; romfs_image_size = strtoull(argv[3], NULL, 10);
    char path[4096]; snprintf(path, sizeof path, "%s/romfs.bin", argv[1]);
    image_fd = open(path, O_RDONLY);
    if (image_fd < 0) open_chunks(argv[1]);
    verify_image(image_fd, argv[1]);
    printf("parts %u\n", nchunks);
    return 0;
}
'''

FIFO_SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int mkfifo(const char *path, mode_t mode) {
    const char *root = getenv("TOTK_DENY_FIFO_UNDER");
    if (root && !strncmp(path, root, strlen(root)) && path[strlen(root)] == '/') { errno = EPERM; return -1; }
    return ((int (*)(const char *, mode_t))dlsym(RTLD_NEXT, "mkfifo"))(path, mode);
}
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (getenv("TOTK_DENY_FIFO_OPEN") && strstr(path, "/romfs-") && strstr(path, ".fifo")) { errno = EACCES; return -1; }
    return ((int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open"))(path, flags, mode);
}
ssize_t write(int fd, const void *buf, size_t n) {
    const char *name = getenv("TOTK_DENY_KEY_WRITE");
    if (name) {
        char link[64], path[4096]; snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t len = readlink(link, path, sizeof path - 1);
        if (len >= 0) {
            path[len] = 0; const char *base = strrchr(path, '/');
            if (base && !strcmp(base + 1, name)) { errno = ENOSPC; return -1; }
        }
    }
    return ((ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write"))(fd, buf, n);
}
int close(int fd) {
    int rc = ((int (*)(int))dlsym(RTLD_NEXT, "close"))(fd);
    if (getenv("TOTK_CLOBBER_CLOSE_ERRNO")) errno = ENOENT;
    return rc;
}
int unlink(const char *path) {
    const char *deny = getenv("TOTK_DENY_UNLINK");
    if (deny && !strcmp(path, deny)) { errno = EPERM; return -1; }
    return ((int (*)(const char *))dlsym(RTLD_NEXT, "unlink"))(path);
}
'''


def files_of(root, skip=()):
    """rel path -> sha256 of every regular file under root."""
    out = {}
    for p in sorted(Path(root).rglob('*')):
        rel = str(p.relative_to(root))
        if p.is_file() and not any(rel == s or rel.startswith(s + '/') for s in skip):
            out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def snapshot(root):
    """Everything about a tree that a move must not change: paths, bytes, sizes and modification times."""
    return {str(p.relative_to(root)): (p.is_dir(), p.stat().st_mtime_ns, p.stat().st_size,
                                       hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None)
            for p in sorted(Path(root).rglob('*'))}


def image_files(folder):
    return sorted(p.name for p in Path(folder).iterdir() if p.name.startswith('romfs.bin'))


class Storage(DecryptedFixture):
    """The fixture's RomFS (about 75 kB) in parts of a third of it plus a few bytes: three parts, the last one shorter."""
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        source = (RUNTIME / 'romfs.c').read_text()
        source = source[:source.index('static uint64_t lower(const char *name);')]
        source = '\n'.join(l for l in source.splitlines() if not l.startswith('#include "') or l == '#include "romfs_proof.h"')
        host_os = (RUNTIME / 'host_os.c').read_text()
        sha_part = host_os[host_os.index('/* SHA-256 (FIPS 180-4)'):host_os.rindex('#endif')]
        cls.runtime_probe = WORK / 'romfs-parts-probe'
        c = WORK / 'romfs-parts-probe.c'
        c.write_text(PROBE_PRELUDE + source + sha_part + PARTS_MAIN)
        subprocess.run([shutil.which('cc') or 'gcc', '-std=gnu11', '-O1', '-w', '-I', str(RUNTIME), str(c), '-o', str(cls.runtime_probe)], check=True)

    def setUp(self):
        super().setUp()
        self.part = len(self.image) // 3 + 5
        self.priv = self.dir / 'private'

    def run_parts(self, part_bytes, stages=('romfs', 'index', 'fonts'), game=None, env=None):
        fds = self.open_fds([self.dir / 'romfs.bin'])
        req = self.request(fds, list(stages), romfs=[{'fd': fds['parts'][0], 'size': 1, 'name': 'romfs.bin'}])
        data = json.loads(req.read_text()); data['romfsPartBytes'] = part_bytes
        if game:
            data['gameDir'] = str(game)
        req.write_text(json.dumps(data))
        e = dict(os.environ, **(env or {}))
        p = subprocess.run([str(self.cli), 'run', '--request', str(req), '--pins', str(self.pins)], capture_output=True, text=True,
                           pass_fds=(fds['font'], fds['parts'][0]), env=e)
        self.assertNotIn('Sanitizer', p.stderr, p.stderr)
        return json.loads(p.stdout.strip().splitlines()[-1])

    def runtime(self, folder, **env):
        e = dict(os.environ, TOTK_ROMFS_PROOF_DIR=str(self.priv / 'state')); e.pop('TOTK_ROMFS_VERIFY', None); e.update(env)
        return subprocess.run([str(self.runtime_probe), str(folder), sha(self.image), str(len(self.image))], capture_output=True, text=True, env=e)

    def cli_index(self, romfs, out):
        out.mkdir(parents=True, exist_ok=True)
        return self.cli_run('index', '--romfs', romfs, '--out-index', out / 'runtime-index.bin', '--out-config', out / 'romfs-config.txt', '--pins', self.pins)[1]


class Parts(Storage):
    def test_parts_written_and_every_reader_agrees(self):
        out = self.run_parts(self.part)
        self.assertTrue(out['ok'], out)
        folder = self.game / ROMFS
        self.assertEqual(image_files(folder), ['romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002'])
        sizes = [(folder / n).stat().st_size for n in image_files(folder)]
        self.assertEqual(sizes, [self.part, self.part, len(self.image) - 2 * self.part])
        self.assertLess(sizes[2], self.part)
        self.assertEqual(b''.join((folder / n).read_bytes() for n in image_files(folder)), self.image)
        st = self.install()['stages']['romfs']
        self.assertEqual(st, {'done': True, 'size': len(self.image), 'sha256': sha(self.image), 'parts': 3})
        self.assertEqual(out['install']['stages']['romfs']['parts'], 3)
        # The index stage read the parts: the same index as the Python writer over the single file.
        self.assertEqual((folder / 'runtime-index.bin').read_bytes(), self.index)
        # The index CLI over the parts folder and over the one file.
        a = self.cli_index(folder, self.dir / 'idx-parts'); b = self.cli_index(self.dir / 'romfs.bin', self.dir / 'idx-one')
        self.assertEqual((a['sha256'], a['parts'], b['parts']), (b['sha256'], 3, 0))
        self.assertEqual((self.dir / 'idx-parts/runtime-index.bin').read_bytes(), (self.dir / 'idx-one/runtime-index.bin').read_bytes())
        # The runtime opens the parts and takes setup's record (one entry per part) without hashing.
        record = self.priv / 'state/.romfs-verified-parts-v1'
        self.assertEqual(record.stat().st_size, 3 * PROOF_SIZE)
        self.assertFalse((self.priv / 'state/.romfs-verified-v1').exists())
        r = self.runtime(folder)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('RomFS parts unchanged; reused full SHA256 verification record (3 parts)', r.stderr)
        self.assertIn('parts 3', r.stdout)
        # Its own record for the same files is byte-identical; a forced hash reads every part correctly.
        written = record.read_bytes(); record.unlink()
        r = self.runtime(folder)
        self.assertIn('verifying local RomFS SHA256 over 3 parts', r.stderr)
        self.assertEqual(record.read_bytes(), written)
        self.assertEqual(self.runtime(folder, TOTK_ROMFS_VERIFY='full').returncode, 0)
        # A done stage stays done while all parts are there; a missing part reopens it.
        out = self.run_parts(self.part, stages=['fonts'])
        self.assertTrue(out['install']['stages']['romfs']['done'])
        (folder / 'romfs.bin.001').unlink()
        out = self.run_parts(self.part, stages=['fonts'])
        self.assertFalse(out['install']['stages']['romfs']['done'])

    def test_remounted_card_keeps_the_record(self):
        # exFAT behind Android's FUSE view gives files new device/inode numbers after a remount (measured on the Thor),
        # size and mtime kept. Copies with the same times stand in for that here.
        for part_bytes in (self.part, 0):
            with self.subTest(part_bytes=part_bytes):
                self.assertTrue(self.run_parts(part_bytes, stages=['romfs'])['ok'])
                folder = self.game / ROMFS
                self.assertTrue((self.priv / 'state/.romfs-samples-v1').exists())
                for name in image_files(folder):
                    shutil.copy2(folder / name, folder / (name + '.new')); os.replace(folder / (name + '.new'), folder / name)
                r = self.runtime(folder)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('new device/inode numbers (storage remounted); size, mtime and sampled content unchanged', r.stderr)
                self.assertNotIn('verifying local RomFS', r.stderr)
                r = self.runtime(folder)
                self.assertIn('unchanged; reused full SHA256 verification record', r.stderr)   # the record now names the new files
                # Same size and times but other bytes: the samples differ, the full hash runs and refuses the image.
                victim = folder / image_files(folder)[-1]
                st = victim.stat(); data = bytearray(victim.read_bytes()); data[len(data) // 2] ^= 1
                victim.with_suffix('.tmp').write_bytes(data); os.utime(victim.with_suffix('.tmp'), ns=(st.st_atime_ns, st.st_mtime_ns))
                os.replace(victim.with_suffix('.tmp'), victim)
                r = self.runtime(folder)
                self.assertIn('verifying local RomFS SHA256', r.stderr)
                self.assertEqual(r.returncode, 7)
                self.assertIn('integrity mismatch', r.stderr)
                shutil.rmtree(self.game)

    def test_exact_multiple_and_one_part(self):
        size = len(self.image)
        for part, want in ((size // 2 if size % 2 == 0 else (size + 1) // 2, 2), (size, 1), (size * 4, 1)):
            with self.subTest(part=part):
                out = self.run_parts(part, stages=['romfs'])
                self.assertTrue(out['ok'], out)
                names = image_files(self.game / ROMFS)
                self.assertEqual(len(names), want)
                self.assertEqual(b''.join((self.game / ROMFS / n).read_bytes() for n in names), self.image)
                self.assertEqual(self.install()['stages']['romfs']['parts'], want)

    def test_layout_change_removes_the_old_layout(self):
        folder = self.game / ROMFS
        self.assertTrue(self.run_parts(self.part // 2)['ok'])                  # 6 parts
        self.assertEqual(len(image_files(folder)), 6)
        self.assertTrue(self.run_parts(0)['ok'])                               # one file again
        self.assertEqual(image_files(folder), ['romfs.bin'])
        self.assertTrue((self.priv / 'state/.romfs-verified-v1').exists())
        self.assertFalse((self.priv / 'state/.romfs-verified-parts-v1').exists())
        self.assertEqual(self.install()['stages']['romfs']['parts'], 0)
        self.assertTrue(self.run_parts(self.part // 2)['ok'])
        self.assertTrue(self.run_parts(self.part)['ok'])                       # fewer, larger parts: no stale .003-.005
        self.assertEqual(image_files(folder), ['romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002'])
        self.assertFalse((self.priv / 'state/.romfs-verified-v1').exists())
        r = self.runtime(folder)
        self.assertIn('reused full SHA256 verification record (3 parts)', r.stderr)

    def test_too_many_parts_refused(self):
        out = self.run_parts(len(self.image) // 64 - 1, stages=['romfs'])
        self.assertEqual((out['ok'], out['code']), (False, 'internal'))
        self.assertIn('more than 64 parts', out['message'])
        self.assertFalse((self.game / ROMFS).exists())

    def test_fat_volume_gets_parts_whatever_was_asked(self):
        mounts = self.dir / 'mounts'
        self.game.mkdir()
        mounts.write_text(f'/dev/root / ext4 rw 0 0\n/dev/block/vold/public:179,1 {self.game} vfat rw 0 0\n')
        out = self.run_parts(0, stages=['romfs'], env={'TOTK_SETUP_MOUNTS': str(mounts)})
        self.assertTrue(out['ok'], out)
        self.assertEqual(image_files(self.game / ROMFS), ['romfs.bin.000'])    # 0xFFFF0000-byte parts: one for this image
        # exFAT (and the FUSE /storage view) take the single file.
        mounts.write_text(f'/dev/root / ext4 rw 0 0\n/dev/block/vold/public:179,1 {self.game} exfat rw 0 0\n')
        out = self.run_parts(0, stages=['romfs'], env={'TOTK_SETUP_MOUNTS': str(mounts)})
        self.assertEqual(image_files(self.game / ROMFS), ['romfs.bin'])

    def test_readers_refuse_bad_part_sets(self):
        folder = self.dir / 'bad'; folder.mkdir()
        cuts = [0, self.part, 2 * self.part - 9, len(self.image)]           # middle part shorter than the first
        for i in range(3):
            (folder / f'romfs.bin.{i:03d}').write_bytes(self.image[cuts[i]:cuts[i + 1]])
        out = self.cli_index(folder, self.dir / 'idx')
        self.assertFalse(out['ok'])
        r = self.runtime(folder)
        self.assertEqual(r.returncode, 7)
        self.assertIn('equal sizes', r.stderr)

    def test_windowed_parts_copy_is_byte_identical(self):
        # Parts close (fsync + cache drop) inside the 128 MiB write-back windows: non-zero data must arrive bit for bit.
        size = (300 << 20) + 12345
        path = self.dir / 'noisy.bin'
        with path.open('wb') as f:
            f.write(self.image)
            rng = random.Random(9)
            while f.tell() < size:
                f.write(rng.randbytes(min(4 << 20, size - f.tell())))
        data = path.read_bytes()
        pins = write_pins(self.dir / 'noisy.json', size, sha(data), sha(python_index(path)))
        out_dir = self.dir / 'out'
        part = (101 << 20) + 7   # three parts, seams off every window boundary
        p, out = self.cli_run('romfs', '--part', path, '--out', out_dir, '--part-bytes', part, '--pins', pins)
        self.assertTrue(out['ok'], out)
        self.assertEqual(out['sha256'], sha(data))
        names = image_files(out_dir)
        self.assertEqual(len(names), 3)
        self.assertEqual(hashlib.sha256(b''.join((out_dir / n).read_bytes() for n in names)).hexdigest(), sha(data))


class ContainerParts(Storage):
    """The container route's merged image through the FIFO sink, with a stand-in for hactool that writes the image to its
    --sectionN= output the way hactool does (sequentially) and reports all six IVFC levels."""
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cc = shutil.which('cc') or 'gcc'
        source = WORK / 'fifo-shim.c'
        source.write_text(FIFO_SHIM)
        cls.fifo_shim = WORK / 'fifo-shim.so'
        subprocess.run([cc, '-shared', '-fPIC', str(source), '-o', str(cls.fifo_shim), '-ldl'], check=True)
        # ASan must be first even when the test injects a filesystem failure through LD_PRELOAD.
        preload = ([subprocess.check_output([cc, '-print-file-name=libasan.so'], text=True).strip()] if cls.sanitize else [])
        cls.preload = ':'.join([*preload, str(cls.fifo_shim)])

    def setUp(self):
        super().setUp()
        self.keys.write_text(f'header_key = {HEADER.hex()}\nkey_area_key_application_00 = {bytes.fromhex("00" * 16).hex()}\n')
        from test_import import AREA, TITLEKEK
        self.keys.write_text(f'header_key = {HEADER.hex()}\nkey_area_key_application_00 = {AREA.hex()}\ntitlekek_00 = {TITLEKEK.hex()}\n')
        self.base = self.dir / 'base.nsp'; self.base.write_bytes(game_image('nsp'))
        data = nca(True)
        header = bytearray(b''.join(decrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), data[i * 512:(i + 1) * 512]) for i in range(6)))
        header[0x604] = 4
        header[0x2a0:0x2c0] = hashlib.sha256(header[0x600:0x800]).digest()
        update = b''.join(encrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), header[i * 512:(i + 1) * 512]) for i in range(6)) + data[0xc00:]
        self.update = self.dir / 'update.nsp'
        self.update.write_bytes(partition([('program.nca', update), ('meta.cnmt.nca', nca(content=1, title=0x0100F2C0115B6800)), ('a.tik', ticket())]))
        self.lib = self.dir / 'lib'; self.lib.mkdir()
        fake = self.lib / 'libhactool.so'
        # Do not preload the CLI's ASan runtime / fault shim into the unrelated system shell and cat.
        fake.write_text('#!/usr/bin/env -S -u LD_PRELOAD /bin/sh\nfor a in "$@"; do case "$a" in --section*=*) out="${a#*=}";; esac; done\n'
                        'test -p "$out" || exit 2\nprintf "%s\\n" "$out" > "$TOTK_FIFO_PATH_LOG"\n'
                        'cat "$TOTK_FAKE_IMAGE" > "$out" || exit 1\nfor i in 0 1 2 3 4 5; do echo "Level $i (GOOD)"; done\n')
        fake.chmod(0o755)

    def merge(self, out, part_bytes, nobackup=True, **extra_env):
        work = self.game / '.import'; work.mkdir(parents=True, exist_ok=True)
        self.fifo_log = self.dir / 'fifo-path.txt'
        env = dict(os.environ, TOTK_FAKE_IMAGE=str(self.dir / 'romfs.bin'), TOTK_FIFO_PATH_LOG=str(self.fifo_log),
                   ASAN_OPTIONS='detect_leaks=1:abort_on_error=0', UBSAN_OPTIONS='print_stacktrace=1')
        env.update(extra_env)
        p = subprocess.run([str(self.cli), 'merge', '--base', str(self.base), '--update', str(self.update), '--keys', str(self.keys),
                            '--out', str(out), '--work', str(work), '--libdir', str(self.lib), '--part-bytes', str(part_bytes),
                            '--pins', str(self.pins), *(['--nobackup', str(self.nobackup)] if nobackup else [])],
                           capture_output=True, text=True, env=env, timeout=120)
        self.assertNotIn('Sanitizer', p.stderr, p.stderr)
        self.assertNotIn('runtime error', p.stderr, p.stderr)
        self.no_secret(p.stdout + p.stderr)
        return p, json.loads(p.stdout.strip().splitlines()[-1])

    def test_merge_into_parts_and_one_file(self):
        p, out = self.merge(self.dir / 'parts', self.part)
        self.assertTrue(out['ok'], out | {'stderr': p.stderr[-2000:]})
        self.assertEqual(out['sha256'], sha(self.image))
        names = image_files(self.dir / 'parts')
        self.assertEqual(names, ['romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002'])
        self.assertEqual(b''.join((self.dir / 'parts' / n).read_bytes() for n in names), self.image)
        fifo = Path(self.fifo_log.read_text().strip())
        self.assertEqual(fifo.parent, self.nobackup / 'setup-work')
        self.assertRegex(fifo.name, r'^romfs-\d+\.fifo$')
        self.assertFalse(fifo.exists())
        p, out = self.merge(self.dir / 'one', 0)
        self.assertTrue(out['ok'], out)
        self.assertEqual((self.dir / 'one/romfs.bin').read_bytes(), self.image)

    def test_merge_mismatch_leaves_no_parts(self):
        bad = bytearray(self.image); bad[-3] ^= 1; (self.dir / 'romfs.bin').write_bytes(bad)
        p, out = self.merge(self.dir / 'parts', self.part)
        self.assertEqual(out['code'], 'hash_mismatch', out)
        self.assertEqual(image_files(self.dir / 'parts'), [])

    def test_merge_on_volume_without_fifo_support(self):
        for part_bytes in (0, self.part):
            with self.subTest(part_bytes=part_bytes):
                folder = self.game / ('parts' if part_bytes else 'one')
                p, out = self.merge(folder, part_bytes, LD_PRELOAD=self.preload, TOTK_DENY_FIFO_UNDER=str(self.game))
                self.assertTrue(out['ok'], out | {'stderr': p.stderr[-2000:]})
                self.assertEqual(out['sha256'], sha(self.image))
                self.assertEqual(b''.join((folder / name).read_bytes() for name in image_files(folder)), self.image)
                fifo = Path(self.fifo_log.read_text().strip())
                self.assertEqual(fifo.parent, self.nobackup / 'setup-work')
                self.assertFalse(fifo.exists())

    def test_merge_without_nobackup_uses_temporary_fifo(self):
        p, out = self.merge(self.game / 'one', 0, nobackup=False,
                            LD_PRELOAD=self.preload, TOTK_DENY_FIFO_UNDER=str(self.game))
        self.assertTrue(out['ok'], out | {'stderr': p.stderr[-2000:]})
        self.assertEqual((self.game / 'one/romfs.bin').read_bytes(), self.image)
        fifo = Path(self.fifo_log.read_text().strip())
        self.assertEqual(fifo.parent.parent, Path('/tmp'))
        self.assertTrue(fifo.parent.name.startswith('totk-romfs-'))
        self.assertFalse(fifo.parent.exists())

    def test_next_locked_setup_reclaims_dead_process_fifos(self):
        work = self.nobackup / 'setup-work'; work.mkdir(parents=True)
        stale = [work / f'romfs-{pid}.fifo' for pid in (2147483646, 2147483647)]
        for path in stale:
            os.mkfifo(path, 0o600)
        regular = work / 'romfs-regular.fifo'; regular.write_bytes(b'keep')
        other = work / 'other.fifo'; os.mkfifo(other, 0o600)
        link = work / 'romfs-link.fifo'; link.symlink_to(other)
        scratch = work / 'shaders-resume'; scratch.mkdir()
        nested = scratch / stale[0].name; os.mkfifo(nested, 0o600)
        with (self.nobackup / 'setup.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs'])
            self.assertFalse(out['ok'], out)
            self.assertIn('already running', out['message'])
            self.assertTrue(all(path.exists() for path in stale), 'another setup must not reclaim the active setup’s pipes')
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs'])
        self.assertTrue(out['ok'], out | {'stderr': p.stderr[-2000:]})
        self.assertTrue(all(not path.exists() for path in stale), 'the next locked run removes dead process pipes')
        self.assertEqual(regular.read_bytes(), b'keep')
        self.assertTrue(other.exists())
        self.assertTrue(link.is_symlink())
        self.assertTrue(nested.exists(), 'shader resume scratch is not swept recursively')

    def test_mkfifo_reports_its_own_error_before_cleanup(self):
        p, out = self.merge(self.game / 'parts', self.part, LD_PRELOAD=self.preload,
                            TOTK_DENY_FIFO_UNDER=str(self.nobackup), TOTK_CLOBBER_CLOSE_ERRNO='1')
        self.assertEqual((p.returncode, out['ok'], out['code']), (1, False, 'io'), out)
        self.assertIn('mkfifo romfs-', out['message'])
        self.assertIn(os.strerror(1), out['message'])   # EPERM, not unlink's ENOENT
        self.assertNotIn(os.strerror(2), out['message'])
        self.assertEqual(image_files(self.game / 'parts'), [])
        self.assertEqual(list((self.nobackup / 'setup-work').glob('*.fifo')), [])

    def test_fifo_open_reports_its_own_error_before_cleanup(self):
        p, out = self.merge(self.game / 'parts', self.part, LD_PRELOAD=self.preload,
                            TOTK_DENY_FIFO_OPEN='1', TOTK_CLOBBER_CLOSE_ERRNO='1')
        self.assertEqual((p.returncode, out['ok'], out['code']), (1, False, 'io'), out)
        self.assertIn('open romfs-', out['message'])
        self.assertIn(os.strerror(13), out['message'])   # EACCES
        self.assertNotIn('mkfifo', out['message'])
        self.assertEqual(image_files(self.game / 'parts'), [])
        self.assertEqual(list((self.nobackup / 'setup-work').glob('*.fifo')), [])

    def test_temporary_key_write_error_survives_close(self):
        for name in ('hactool.keys', 'title.keys'):
            with self.subTest(name=name):
                p, out = self.merge(self.game / 'parts', self.part, LD_PRELOAD=self.preload,
                                    TOTK_DENY_KEY_WRITE=name, TOTK_CLOBBER_CLOSE_ERRNO='1')
                self.assertEqual((p.returncode, out['ok'], out['code']), (1, False, 'no_space'), out)
                self.assertIn('write the temporary', out['message'])
                self.assertIn(name, out['message'])
                self.assertIn(os.strerror(28), out['message'])   # ENOSPC
                self.assertFalse((self.game / '.import/import-keys').exists())
                self.assertEqual(list((self.nobackup / 'setup-work').glob('*.fifo')), [])

    def test_remove_keeps_first_error_while_cleaning_other_data(self):
        self.game.mkdir()
        install = self.game / 'install.json'; install.write_text('{}')
        extracted = self.game / 'extracted'; extracted.mkdir(); (extracted / 'stale').write_bytes(b'x')
        env = dict(os.environ, LD_PRELOAD=self.preload, TOTK_DENY_UNLINK=str(install), TOTK_CLOBBER_CLOSE_ERRNO='1',
                   ASAN_OPTIONS='detect_leaks=1:abort_on_error=0', UBSAN_OPTIONS='print_stacktrace=1')
        p = subprocess.run([str(self.cli), 'remove', '--dir', str(self.game)], capture_output=True, text=True, env=env)
        self.assertNotIn('Sanitizer', p.stderr, p.stderr)
        out = json.loads(p.stdout)
        self.assertEqual((p.returncode, out['ok'], out['code']), (1, False, 'io'), out)
        self.assertIn(os.strerror(1), out['message'])
        self.assertTrue(install.exists())
        self.assertFalse(extracted.exists())


class Move(Storage):
    def setUp(self):
        super().setUp()
        out = self.run_parts(0)
        self.assertTrue(out['ok'], out)
        g = self.game
        for rel, size in (('rewritten/v143/main.patched.bin', 70001), ('rewritten/v143/sdk.patched.bin', 3000), ('rewritten/v143/subsdk0.patched.bin', 999),
                          ('game-data/v143/runtime-tables.bin', 5000), ('shaders/v143/seed.vkpack', 40000), ('shaders/v143/seed-id.txt', 65),
                          ('companion/index.json', 1200), ('companion/icons/a.webp', 30000), ('companion/map/G/1/0_0.webp', 9000), ('driver.so', 4096)):
            (g / rel).parent.mkdir(parents=True, exist_ok=True); (g / rel).write_bytes(os.urandom(size))
        (g / 'companion/empty-dir').mkdir()
        for rel in ('totk.log', '.import/stale.log', 'shader-requests/x.txt', 'state/run.txt'):   # not game data
            (g / rel).parent.mkdir(parents=True, exist_ok=True); (g / rel).write_text('keep me')
        self.card = self.dir / 'card/Android/data/com.aure.totk/files'; self.card.mkdir(parents=True)
        self.before = snapshot(g)

    def move(self, src, dst, part_bytes=0, *extra, preexec=None):
        req = self.dir / 'move.json'
        req.write_text(json.dumps({'from': str(src), 'to': str(dst), 'privateDir': str(self.priv), 'romfsPartBytes': part_bytes}))
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0')
        p = subprocess.run([str(self.cli), 'move', '--request', str(req), '--pins', str(self.pins), *map(str, extra)],
                           capture_output=True, text=True, env=env, preexec_fn=preexec, timeout=300)
        self.assertNotIn('Sanitizer', p.stderr, p.stderr[-3000:])
        return p, json.loads(p.stdout.strip().splitlines()[-1])

    def assert_untouched(self):
        self.assertEqual(snapshot(self.game), self.before)

    def assert_nothing_moved(self, dst=None):
        dst = dst or self.card
        self.assertEqual(sorted(p.name for p in dst.iterdir()), [], 'the destination keeps nothing of a failed move')
        self.assert_untouched()

    def test_move_to_parts_and_back(self):
        p, out = self.move(self.game, self.card, self.part)
        self.assertTrue(out['ok'], out | {'stderr': p.stderr[-2000:]})
        self.assert_untouched()
        # Every game file arrived; the RomFS was re-cut; logs, staging and runtime state stayed behind.
        skip = ('totk.log', '.import', 'shader-requests', 'state', ROMFS + '/romfs.bin', 'install.json')
        moved = files_of(self.card, skip=(ROMFS + '/romfs.bin.000', ROMFS + '/romfs.bin.001', ROMFS + '/romfs.bin.002', 'install.json'))
        self.assertEqual(moved, files_of(self.game, skip=skip))
        self.assertTrue((self.card / 'companion/empty-dir').is_dir())
        for gone in ('totk.log', '.import', 'shader-requests', 'state', '.move'):
            self.assertFalse((self.card / gone).exists(), gone)
        names = image_files(self.card / ROMFS)
        self.assertEqual(names, ['romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002'])
        self.assertEqual(b''.join((self.card / ROMFS / n).read_bytes() for n in names), self.image)
        src_install = json.loads((self.game / 'install.json').read_text())
        dst_install = json.loads((self.card / 'install.json').read_text())
        src_install['stages']['romfs']['parts'] = 3
        self.assertEqual(dst_install, src_install)
        self.assertEqual(out['install'], dst_install)
        # A fresh record for the new files: the runtime starts there without hashing.
        self.assertFalse((self.priv / 'state/.romfs-verified-v1').exists())
        r = self.runtime(self.card / ROMFS)
        self.assertIn('reused full SHA256 verification record (3 parts)', r.stderr)
        self.assertNotIn('verifying', r.stderr)
        self.assertFalse((self.priv / 'state/game.lock.held').exists())
        # The app switched; the old copy goes, everything else in its folder stays.
        p = subprocess.run([str(self.cli), 'remove', '--dir', str(self.game)], capture_output=True, text=True)
        self.assertEqual(json.loads(p.stdout), {'ok': True})
        self.assertEqual(sorted(files_of(self.game)), ['shader-requests/x.txt', 'state/run.txt', 'totk.log'])
        # And back again into one file.
        p, out = self.move(self.card, self.game, 0)
        self.assertTrue(out['ok'], out)
        self.assertEqual(image_files(self.game / ROMFS), ['romfs.bin'])
        self.assertEqual((self.game / ROMFS / 'romfs.bin').read_bytes(), self.image)
        self.assertEqual(json.loads((self.game / 'install.json').read_text())['stages']['romfs']['parts'], 0)
        r = self.runtime(self.game / ROMFS)
        self.assertIn('RomFS unchanged; reused full SHA256 verification record', r.stderr)
        self.assertFalse((self.priv / 'state/.romfs-verified-parts-v1').exists())

    def test_cancel_at_every_phase(self):
        for phase in ('copy', 'romfs', 'verify', 'commit'):
            with self.subTest(phase=phase):
                p, out = self.move(self.game, self.card, self.part, '--cancel-at', phase)
                self.assertEqual((p.returncode, out['code']), (3, 'cancelled'), out)
                self.assertIn(f'phase {phase}', p.stderr)
                self.assert_nothing_moved()

    def test_sigterm_while_copying(self):
        big = self.game / 'companion/big.bin'; big.write_bytes(os.urandom(64 << 20))
        self.before = snapshot(self.game)
        req = self.dir / 'move.json'
        req.write_text(json.dumps({'from': str(self.game), 'to': str(self.card), 'privateDir': str(self.priv), 'romfsPartBytes': 0}))
        proc = subprocess.Popen([str(self.cli), 'move', '--request', str(req), '--pins', str(self.pins)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        for line in proc.stderr:
            if 'copying companion/big.bin' in line:
                break
        proc.send_signal(signal.SIGTERM)
        stdout, _ = proc.communicate(timeout=120)
        out = json.loads(stdout.strip().splitlines()[-1])
        self.assertEqual((proc.returncode, out['code']), (3, 'cancelled'), out)
        self.assert_nothing_moved()

    def test_killed_move_is_cleaned_by_the_next_run(self):
        # A killed process (the app swiped away, the device rebooting) cannot clean up: the next move does, and until
        # then the destination is not playable (no install.json) while the source is as it was.
        big = self.game / 'companion/big.bin'; big.write_bytes(os.urandom(64 << 20))
        self.before = snapshot(self.game)
        req = self.dir / 'move.json'
        req.write_text(json.dumps({'from': str(self.game), 'to': str(self.card), 'privateDir': str(self.priv), 'romfsPartBytes': self.part}))
        proc = subprocess.Popen([str(self.cli), 'move', '--request', str(req), '--pins', str(self.pins)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        for line in proc.stderr:
            if 'copying companion/big.bin' in line:
                break
        proc.kill(); proc.communicate(timeout=60)
        self.assertTrue((self.card / '.move').is_dir())
        self.assertFalse((self.card / 'install.json').exists())
        self.assert_untouched()
        # The lock died with the process: the game could start from the source now.
        with open(self.priv / 'state/game.lock', 'a+') as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB); fcntl.lockf(lock, fcntl.LOCK_UN)
        r = self.runtime(self.game / ROMFS)
        self.assertEqual(r.returncode, 0, r.stderr)
        # The next attempt (here cancelled straight away) starts by removing the leftovers.
        p, out = self.move(self.game, self.card, self.part, '--cancel-at', 'copy')
        self.assertEqual(out['code'], 'cancelled')
        self.assert_nothing_moved()
        p, out = self.move(self.game, self.card, self.part)
        self.assertTrue(out['ok'], out)
        self.assertFalse((self.card / '.move').exists())

    def test_destination_full(self):
        p, out = self.move(self.game, self.card, self.part, '--assume-free', 100000)
        self.assertEqual(out['code'], 'no_space', out)
        self.assertIn('GB', out['message'])
        self.assert_nothing_moved()
        # The space runs out half-way (a write fails as on a full or FAT32 volume): the same clean failure.
        def limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (20000, 20000))
        p, out = self.move(self.game, self.card, 0, preexec=limit)
        self.assertEqual((p.returncode, out['code']), (1, 'io'), out)
        self.assertIn('large', out['message'])
        self.assert_nothing_moved()

    def test_corrupted_destination_detected(self):
        for rel in ('companion/icons/a.webp', ROMFS + '/romfs.bin.001', ROMFS + '/runtime-index.bin'):
            with self.subTest(rel=rel):
                p, out = self.move(self.game, self.card, self.part, '--corrupt', rel)
                self.assertEqual(out['code'], 'io', out)
                self.assertIn('reads back differently', out['message'])
                self.assert_nothing_moved()

    def test_damaged_source_moves_nothing(self):
        image = self.game / ROMFS / 'romfs.bin'
        data = bytearray(image.read_bytes()); data[len(data) // 2] ^= 1
        st = image.stat(); image.write_bytes(data); os.utime(image, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.before = snapshot(self.game)
        p, out = self.move(self.game, self.card, self.part)
        self.assertEqual(out['code'], 'hash_mismatch', out)
        self.assert_nothing_moved()

    def test_game_running_refused(self):
        (self.priv / 'state').mkdir(parents=True, exist_ok=True)
        with open(self.priv / 'state/game.lock', 'a+') as held:
            fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            p, out = self.move(self.game, self.card, self.part)
        self.assertEqual(out['code'], 'busy', out)
        self.assert_nothing_moved()

    def test_older_copy_at_destination_is_replaced(self):
        (self.card / 'rewritten/v142').mkdir(parents=True); (self.card / 'rewritten/v142/old.bin').write_bytes(b'old')
        (self.card / ROMFS).mkdir(parents=True); (self.card / ROMFS / 'romfs.bin.007').write_bytes(b'stale part')
        (self.card / 'install.json').write_text('{"format":1}')
        (self.card / 'mine.txt').write_text('the player\'s own file')
        p, out = self.move(self.game, self.card, self.part)
        self.assertTrue(out['ok'], out)
        self.assertFalse((self.card / 'rewritten/v142').exists())
        self.assertEqual(image_files(self.card / ROMFS), ['romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002'])
        self.assertEqual((self.card / 'mine.txt').read_text(), 'the player\'s own file')

    def test_bad_requests(self):
        for req, code in (({'from': str(self.game), 'to': str(self.game / 'sub')}, 'internal'),
                          ({'from': 'relative', 'to': str(self.card)}, 'internal'),
                          ({'from': str(self.card), 'to': str(self.dir / 'other')}, 'missing_input')):
            with self.subTest(req=req):
                path = self.dir / 'bad.json'; path.write_text(json.dumps(req))
                p = subprocess.run([str(self.cli), 'move', '--request', str(path), '--pins', str(self.pins)], capture_output=True, text=True)
                out = json.loads(p.stdout)
                self.assertEqual((out['ok'], out['stage'], out['code']), (False, 'move', code), out)
        self.assert_untouched()
        p = subprocess.run([str(self.cli), 'remove', '--dir', 'relative/dir'], capture_output=True, text=True)
        self.assertFalse(json.loads(p.stdout)['ok'])


class PartsSan(Parts):
    sanitize = True


class ContainerPartsSan(ContainerParts):
    sanitize = True


class MoveSan(Move):
    sanitize = True


if __name__ == '__main__':
    unittest.main()

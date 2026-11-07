"""The importer's pipelined RomFS copy and the side-by-side companion and shader stages, on the host with synthetic bytes.

Pipeline (romfs_image.c RomfsWriter: the caller reads into a ring of buffers, a hash thread and a write thread follow):
ring and part seams anywhere, short reads and short writes (an LD_PRELOAD shim), a write failure mid-ring (RLIMIT_FSIZE),
cancellation while the reader waits for a buffer, while it reads, and while the ring drains, and the digest being that of
the stream (nothing is read back). TOTK_SETUP_RING shrinks the ring so a small image crosses many buffers.

Companion and shaders (setup.c run_companion_and_shaders): they run at the same time (setup_cli --fake stands in for both
generators); a failed companion stage is recorded and keeps the install playable; a failed shader stage fails setup (the
game needs the seed) and the install stays unplayable until a retry builds it; a cancel stops both; a run killed after one
of them finished keeps that one in install.json and the next run only does the other. Also: the ExeFS copy decides on the
pins from the hashes it took while copying. Everything also runs under ASan + UBSan."""
import json
import os
import random
import resource
import shutil
import signal
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_import import WORK, DecryptedFixture, Base, partition, sha, write_pins  # noqa: E402

SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
/* Test shim for the RomFS copy: SHIM_PREAD_MAX / SHIM_WRITE_MAX cap every source read / image write at a pseudo-random
   1..N bytes, SHIM_PREAD_US / SHIM_WRITE_US delay each one, SHIM_FLIP flips the first byte of every image write. */
static long env(const char *k) { const char *v = getenv(k); return v ? atol(v) : 0; }
static int kind(int fd) {   /* 1: an image file the importer writes, 2: anything read outside the game folder */
    char l[64], p[4096]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(l, p, sizeof p - 1); if (n <= 0) return 0; p[n] = 0;
    if (strstr(p, "/.import/romfs/romfs.bin")) return 1;
    return strstr(p, "/game/") ? 0 : 2;
}
static unsigned seed = 12345;
static size_t cap(size_t n, long max) { if (max <= 0 || n <= 1) return n; size_t k = 1 + (size_t)(rand_r(&seed) % max); return k < n ? k : n; }
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    static ssize_t (*real)(int, void *, size_t, off_t); if (!real) real = dlsym(RTLD_NEXT, "pread");
    if (kind(fd) == 2) { if (env("SHIM_PREAD_US")) usleep((useconds_t)env("SHIM_PREAD_US")); n = cap(n, env("SHIM_PREAD_MAX")); }
    return real(fd, buf, n, off);
}
ssize_t pread64(int fd, void *buf, size_t n, off_t off) { return pread(fd, buf, n, off); }
ssize_t write(int fd, const void *buf, size_t n) {
    static ssize_t (*real)(int, const void *, size_t); if (!real) real = dlsym(RTLD_NEXT, "write");
    if (kind(fd) != 1) return real(fd, buf, n);
    if (env("SHIM_WRITE_US")) usleep((useconds_t)env("SHIM_WRITE_US"));
    n = cap(n, env("SHIM_WRITE_MAX"));
    if (env("SHIM_FLIP") && n) { char *copy = malloc(n); memcpy(copy, buf, n); copy[0] ^= 1; ssize_t r = real(fd, copy, n); free(copy); return r; }
    return real(fd, buf, n);
}
'''


def shim():
    lib = WORK / 'io-shim.so'
    if not lib.exists():
        src = WORK / 'io-shim.c'; src.write_text(SHIM)
        subprocess.run([shutil.which('cc') or 'gcc', '-shared', '-fPIC', '-O1', str(src), '-o', str(lib), '-ldl'], check=True)
    return lib


class Pipeline(DecryptedFixture):
    RING = '3x4099'   # odd-sized buffers: every seam (buffer, source part, output part) falls somewhere else

    def setUp(self):
        super().setUp()
        size = (1 << 20) + 54321
        rng = random.Random(11)
        self.data = self.image + rng.randbytes(size - len(self.image))
        self.src = self.dir / 'noisy.bin'; self.src.write_bytes(self.data)
        from test_import import python_index
        self.npins = write_pins(self.dir / 'noisy.json', size, sha(self.data), sha(python_index(self.src)))

    def popen(self, parts, stages=('romfs',), part_bytes=0, env=None, preexec=None):
        fds = self.open_fds(parts)
        req = self.request(fds, list(stages), romfs=[{'fd': f, 'size': 1, 'name': Path(p).name} for f, p in zip(fds['parts'], parts)])
        data = json.loads(req.read_text()); data['romfsPartBytes'] = part_bytes; req.write_text(json.dumps(data))
        e = dict(os.environ, TOTK_SETUP_RING=self.RING, ASAN_OPTIONS='detect_leaks=0:verify_asan_link_order=0', UBSAN_OPTIONS='print_stacktrace=1')
        e.update(env or {})
        return subprocess.Popen([str(self.cli), 'run', '--request', str(req), '--pins', str(self.npins)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, pass_fds=(fds['font'], *fds['parts']), env=e, preexec_fn=preexec)

    def finish(self, proc):
        stdout, stderr = proc.communicate(timeout=300)
        self.assertNotIn('Sanitizer', stderr, stderr); self.assertNotIn('runtime error', stderr, stderr)
        return proc.returncode, json.loads(stdout.strip().splitlines()[-1]), stderr

    def cuts(self, points):
        parts = []
        for i in range(len(points) - 1):
            part = self.dir / f'src.{i}'; part.write_bytes(self.data[points[i]:points[i + 1]]); parts.append(part)
        return parts

    def image_files(self):
        folder = self.game / 'extracted/v143/romfs'
        return sorted(p.name for p in folder.glob('romfs.bin*')) if folder.exists() else []

    def assert_installed(self, out, part_bytes):
        self.assertTrue(out['ok'], out)
        self.assertEqual(out['install']['stages']['romfs']['sha256'], sha(self.data))
        names = self.image_files()
        got = b''.join((self.game / 'extracted/v143/romfs' / n).read_bytes() for n in names)
        self.assertEqual(got, self.data)
        if part_bytes:
            self.assertEqual(len(names), -(-len(self.data) // part_bytes))
            for n in names[:-1]:
                self.assertEqual((self.game / 'extracted/v143/romfs' / n).stat().st_size, part_bytes)

    def assert_nothing_kept(self):
        self.assertEqual(self.image_files(), [])
        staged = self.game / '.import/romfs'
        self.assertEqual(sorted(staged.glob('romfs.bin*')) if staged.exists() else [], [])

    def test_seams_anywhere(self):
        parts = self.cuts([0, 81, 4099, 4100, 300000, len(self.data) - 1, len(self.data)])
        for part_bytes in (0, 4099 * 7 + 3, 300000):
            code, out, _ = self.finish(self.popen(parts, part_bytes=part_bytes))
            self.assert_installed(out, part_bytes)

    def test_default_ring(self):
        self.RING = ''   # 4 x 16 MiB: the whole image in one buffer
        code, out, _ = self.finish(self.popen([self.src], part_bytes=300000))
        self.assert_installed(out, 300000)

    def test_short_reads_and_writes(self):
        env = {'LD_PRELOAD': str(shim()), 'SHIM_PREAD_MAX': '777', 'SHIM_WRITE_MAX': '1000'}
        code, out, _ = self.finish(self.popen(self.cuts([0, 5000, len(self.data)]), part_bytes=250001, env=env))
        self.assert_installed(out, 250001)

    def test_write_failure_mid_ring(self):
        def limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (600000, resource.RLIM_INFINITY))
        code, out, _ = self.finish(self.popen([self.src], preexec=limit))
        self.assertEqual((code, out['stage'], out['code']), (1, 'romfs', 'io'), out)
        self.assertIn('write romfs.bin', out['message'])
        self.assert_nothing_kept()
        self.assertFalse((self.game / 'install.json').exists())

    def cancel_at(self, marker, **env):
        proc = self.popen([self.src], env={'LD_PRELOAD': str(shim()), **env})
        for line in proc.stderr:
            if marker(line):
                break
        proc.send_signal(signal.SIGTERM)
        code, out, _ = self.finish(proc)
        self.assertEqual((code, out['stage'], out['code']), (3, 'romfs', 'cancelled'), out)
        self.assert_nothing_kept()

    def test_cancel_while_waiting_for_a_buffer(self):
        # Slow writes: the ring is full, the reader waits for the write thread when the cancel comes.
        self.RING = '2x4096'
        self.cancel_at(lambda l: l.startswith('[romfs] '), SHIM_WRITE_US='20000')

    def test_cancel_while_reading(self):
        self.RING = '4x4096'
        self.cancel_at(lambda l: l.startswith('[romfs] '), SHIM_PREAD_US='20000', SHIM_PREAD_MAX='4096')

    def test_cancel_while_draining(self):
        # The whole image fits the ring: the reader is done and waits for the slow writes to drain.
        self.RING = f'16x{1 << 17}'
        total = len(self.data)
        self.cancel_at(lambda l: l.startswith(f'[romfs] {total}/{total}'), SHIM_WRITE_US='300000')

    def test_digest_is_the_stream(self):
        # The digest is taken from the bytes handed to the write thread, never from a read-back: bytes damaged on their
        # way to the file do not change it (the pin check is a property of the input stream).
        code, out, _ = self.finish(self.popen([self.src], env={'LD_PRELOAD': str(shim()), 'SHIM_FLIP': '1'}))
        self.assertTrue(out['ok'], out)
        self.assertEqual(out['install']['stages']['romfs']['sha256'], sha(self.data))
        self.assertNotEqual((self.game / 'extracted/v143/romfs/romfs.bin').read_bytes(), self.data)

    def test_mismatch_commits_nothing(self):
        bad = bytearray(self.data); bad[len(bad) // 2] ^= 1; (self.dir / 'bad.bin').write_bytes(bad)
        code, out, _ = self.finish(self.popen([self.dir / 'bad.bin'], part_bytes=300000))
        self.assertEqual((out['stage'], out['code']), ('romfs', 'hash_mismatch'), out)
        self.assert_nothing_kept()

    def test_path_inputs(self):
        # "path" instead of "fd": the library opens (and closes) the file itself.
        fds = self.open_fds([])
        req = self.request(fds, ['romfs'], romfs=[{'path': str(self.src), 'size': 0, 'name': 'noisy.bin'}])
        run = lambda: self.finish(subprocess.Popen([str(self.cli), 'run', '--request', str(req), '--pins', str(self.npins)],
                                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, pass_fds=(fds['font'],)))
        code, out, _ = run()
        self.assert_installed(out, 0)
        data = json.loads(req.read_text()); data['inputs']['romfs'] = [{'path': str(self.dir / 'missing.bin'), 'name': 'missing.bin'}]
        req.write_text(json.dumps(data))
        code, out, _ = run()
        self.assertEqual((out['code'], out['message']), ('internal', 'cannot open missing.bin: No such file or directory'))
        data['inputs']['romfs'] = [{'path': str(self.dir), 'name': 'a folder'}]; req.write_text(json.dumps(data))
        code, out, _ = run()
        self.assertEqual(out['message'], 'cannot open a folder: not a regular file')


class CompanionAndShaders(DecryptedFixture):
    """companion and shaders side by side, with --fake generators."""
    def setUp(self):
        super().setUp()
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs', 'index', 'fonts'])
        self.assertTrue(out['ok'], out)
        # A committed code stage (its outputs present), so the shader stage may run.
        for rel in ('rewritten/v143/main.patched.bin', 'rewritten/v143/sdk.patched.bin', 'rewritten/v143/subsdk0.patched.bin',
                    'game-data/v143/runtime-tables.bin'):
            (self.game / rel).parent.mkdir(parents=True, exist_ok=True); (self.game / rel).write_bytes(b'x')
        install = self.install()
        install['stages']['code'] = {'done': True, 'rewritten_sha256': {f'{m}.patched.bin': '0' * 64 for m in ('main', 'sdk', 'subsdk0')},
                                     'runtime_tables_sha256': '0' * 64}
        (self.game / 'install.json').write_text(json.dumps(install))

    def popen(self, fakes, stages=('companion', 'shaders'), skip_companion=False):
        fds = self.open_fds([self.dir / 'romfs.bin'])
        req = self.request(fds, list(stages) if stages else None, romfs=[{'fd': fds['parts'][0], 'size': 1, 'name': 'romfs.bin'}])
        if skip_companion:
            data = json.loads(req.read_text())
            data['skipCompanion'] = True
            req.write_text(json.dumps(data))
        args = [a for f in fakes for a in ('--fake', f)]
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0', UBSAN_OPTIONS='print_stacktrace=1')
        return subprocess.Popen([str(self.cli), 'run', '--request', str(req), '--pins', str(self.pins), *args], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, pass_fds=(fds['font'], fds['parts'][0]), env=env)

    def finish(self, proc):
        stdout, stderr = proc.communicate(timeout=120)
        self.assertNotIn('Sanitizer', stderr, stderr); self.assertNotIn('runtime error', stderr, stderr)
        return proc.returncode, json.loads(stdout.strip().splitlines()[-1]), stderr

    def test_both_run_at_once(self):
        code, out, err = self.finish(self.popen(['companion=ok:600', 'shaders=ok:600']))
        self.assertTrue(out['ok'], out)
        st = out['install']['stages']
        self.assertTrue(st['companion']['done'] and st['shaders']['done'])
        self.assertEqual(out['install'], self.install())
        self.assertTrue((self.game / 'companion/fake.txt').exists() and (self.game / 'shaders/v143/seed.vkpack').exists())
        lines = [l.split(']')[0][1:] for l in err.splitlines() if l.startswith('[')]
        # Both streams interleave: companion reports between the first and the last shader report.
        first, last = lines.index('shaders'), len(lines) - 1 - lines[::-1].index('shaders')
        self.assertIn('companion', lines[first:last])
        self.assertIn('[companion] 1/1 done', err); self.assertIn('[shaders] 1/1 done', err)
        self.assertFalse((self.game / '.import').exists())

    def test_single_screen_skips_art_but_explicit_build_still_runs(self):
        code, out, err = self.finish(self.popen(['companion=fail:10', 'shaders=ok:100'], stages=None, skip_companion=True))
        self.assertEqual(code, 0, out)
        self.assertTrue(out['ok'] and out['install']['playable'], out)
        self.assertEqual(out['install']['stages']['companion'], {'done': False, 'skipped': True})
        self.assertEqual(out['install'], self.install())
        self.assertFalse((self.game / 'companion').exists())
        self.assertIn('[companion] 0/0 skipped', err)
        # A later automatic setup keeps the distinct skipped state and does not invoke the generator.
        code, out, err = self.finish(self.popen(['companion=fail:10'], stages=None, skip_companion=True))
        self.assertEqual(out['install']['stages']['companion'], {'done': False, 'skipped': True})
        self.assertFalse((self.game / 'companion').exists())
        # Game data overrides the automatic skip policy and clears skipped after a successful build.
        code, out, err = self.finish(self.popen(['companion=ok:100'], stages=('companion',), skip_companion=True))
        self.assertEqual(out['install']['stages']['companion'], {'done': True})
        self.assertTrue((self.game / 'companion/fake.txt').is_file())

    def test_companion_failing_keeps_install_playable(self):
        code, out, err = self.finish(self.popen(['companion=fail:300', 'shaders=ok:500']))
        self.assertTrue(out['ok'], out)
        install = self.install(); st = install['stages']
        self.assertTrue(st['shaders']['done'], st)
        self.assertEqual((st['companion']['done'], st['companion']['code'], st['companion']['error']), (False, 'internal', 'fake companion failure'))
        self.assertTrue(install['playable'])
        self.assertIn('[companion] 0/0 skipped', err)
        # A later failure of a stage that is done keeps its committed output and its done state; the shader stage's failure
        # still fails the run, but the install stays playable with the seed it already had.
        code, out, err = self.finish(self.popen(['companion=ok:100', 'shaders=fail:100']))
        self.assertEqual((out['ok'], out['stage'], out['code'], out['message']), (False, 'shaders', 'internal', 'fake shaders failure'), out)
        install = self.install(); st = install['stages']
        self.assertTrue(st['companion']['done'] and st['shaders']['done'])
        self.assertTrue(install['playable'])
        self.assertTrue((self.game / 'shaders/v143/seed.vkpack').exists())

    def test_shaders_failing_fails_setup_and_is_not_playable(self):
        code, out, err = self.finish(self.popen(['companion=ok:500', 'shaders=fail:200']))
        self.assertNotEqual(code, 0)
        self.assertEqual((out['ok'], out['stage'], out['code'], out['message']), (False, 'shaders', 'internal', 'fake shaders failure'), out)
        install = self.install(); st = install['stages']
        # The companion lane still ran to the end and was committed; the shader failure is recorded for Game data.
        self.assertTrue(st['companion']['done'], st)
        self.assertEqual((st['shaders']['done'], st['shaders']['code'], st['shaders']['error']), (False, 'internal', 'fake shaders failure'))
        self.assertFalse(install['playable'])
        self.assertIn('[shaders] 0/0 failed', err); self.assertNotIn('[shaders] 0/0 skipped', err)
        self.assertIn('[companion] 1/1 done', err)
        self.assertFalse((self.game / 'shaders').exists())
        # Resume (pending stages only) builds just the seed; then the install is playable.
        code, out, err = self.finish(self.popen(['companion=fail:10', 'shaders=ok:100'], stages=None))
        self.assertTrue(out['ok'], out)
        self.assertNotIn('[companion]', err)
        install = self.install()
        self.assertTrue(install['stages']['shaders']['done'] and install['playable'])
        self.assertNotIn('code', install['stages']['shaders'])

    def test_cancel_stops_both(self):
        proc = self.popen(['companion=ok:5000', 'shaders=ok:5000'])
        seen = set()
        for line in proc.stderr:
            if line.startswith('[companion] ') or line.startswith('[shaders] '):
                seen.add(line[1:line.index(']')])
            if len(seen) == 2:
                break
        proc.send_signal(signal.SIGTERM)
        code, out, _ = self.finish(proc)
        self.assertEqual((code, out['code']), (3, 'cancelled'), out)
        self.assertIn(out['stage'], ('companion', 'shaders'))
        st = self.install()['stages']
        self.assertFalse(st['companion']['done'] or st['shaders']['done'])
        self.assertNotIn('code', st['companion']); self.assertNotIn('code', st['shaders'])
        self.assertFalse((self.game / 'companion').exists() or (self.game / 'shaders').exists())
        self.assertEqual(sorted(p.name for p in (self.game / '.import').iterdir()), [])

    def test_killed_with_one_done_resumes_the_other(self):
        proc = self.popen(['companion=ok:100', 'shaders=ok:20000'])
        for line in proc.stderr:
            if line.startswith('[companion] 1/1 done'):
                break
        proc.kill(); proc.communicate(timeout=60)
        st = self.install()['stages']
        self.assertTrue(st['companion']['done']); self.assertFalse(st['shaders']['done'])
        # Pending stages only: the next run does the shaders and leaves the committed companion alone.
        mtime = (self.game / 'companion/fake.txt').stat().st_mtime_ns
        code, out, err = self.finish(self.popen(['companion=fail:10', 'shaders=ok:100'], stages=None))
        self.assertTrue(out['ok'], out)
        self.assertNotIn('[companion]', err)
        st = self.install()['stages']
        self.assertTrue(st['companion']['done'] and st['shaders']['done'])
        self.assertEqual((self.game / 'companion/fake.txt').stat().st_mtime_ns, mtime)


class ExefsCopy(Base):
    """The ExeFS copy hashes each file while copying and decides on those hashes (no second pass over the files)."""
    def setUp(self):
        super().setUp()
        self.files = {n: os.urandom(100 + i) for i, n in enumerate(('main', 'sdk', 'subsdk0', 'main.npdm', 'rtld'))}
        self.pins = write_pins(self.dir / 'pins.json', 100, '0' * 64, '0' * 64, {n: sha(d) for n, d in self.files.items()})

    def run_code(self, inputs):
        fds = []
        for name, data in inputs:
            path = self.dir / name; path.write_bytes(data); fds.append(os.open(path, os.O_RDONLY))
        self.addCleanup(lambda: [os.close(f) for f in fds])
        req = self.dir / 'req.json'
        req.write_text(json.dumps({'gameDir': str(self.dir / 'g'), 'noBackupDir': str(self.dir / 'n'), 'nativeLibDir': '/x', 'stages': ['code'],
                                   'inputs': {'exefs': [{'fd': f, 'size': 0, 'name': n} for f, (n, _) in zip(fds, inputs)]}}))
        return self.cli_run('run', '--request', req, '--pins', self.pins, pass_fds=fds)[1]

    def test_verdicts(self):
        out = self.run_code([('a', self.files['sdk']), ('b', self.files['main'])])
        self.assertEqual((out['stage'], out['code'], out['message']), ('code', 'missing_input', 'The ExeFS has no subsdk0'))
        out = self.run_code([('main', b'other main'), ('sdk', self.files['sdk'])])
        self.assertEqual((out['code'], out['message']), ('unsupported_version', 'main is not the 1.4.3 version; only 1.4.3 is supported'))
        bad = dict(self.files, sdk=b'not the sdk')
        out = self.run_code([('exefs.pfs0', partition(list(bad.items())))])
        self.assertEqual((out['code'], out['message']), ('unsupported_version', 'sdk is not the 1.4.3 version; only 1.4.3 is supported'))
        partial = {k: v for k, v in self.files.items() if k != 'subsdk0'}
        out = self.run_code([('exefs.pfs0', partition(list(partial.items())))])
        self.assertEqual((out['code'], out['message']), ('missing_input', 'The ExeFS has no subsdk0'))
        # A complete ExeFS passes the pin checks and reaches the rewriter (which has no real code to work on here).
        for inputs in ([('x', self.files['subsdk0']), ('y', self.files['main']), ('z', self.files['sdk'])],
                       [('exefs.pfs0', partition(list(self.files.items())))]):
            out = self.run_code(inputs)
            self.assertNotIn(out['code'], ('missing_input', 'unsupported_version'), out)
            self.assertEqual(out['stage'], 'code')


class PipelineSan(Pipeline):
    sanitize = True


class CompanionAndShadersSan(CompanionAndShaders):
    sanitize = True


class ExefsCopySan(ExefsCopy):
    sanitize = True


if __name__ == '__main__':
    unittest.main()

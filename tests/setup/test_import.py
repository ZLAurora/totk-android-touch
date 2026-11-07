"""On-device importer (android/setup) on the host, with synthetic bytes only: prod.keys parsing, XCI / NSP / NCA / ticket
handling against the Python reference builders, malformed containers, the decrypted-route RomFS copy (hash mismatch, wrong
size, split parts, cancellation, resume after a killed run), the RomFS index against the Python writer, the shared-font
slots, inspect, and no key material in any output. Everything also runs under ASan + UBSan.

Builds android/setup/setup.mk host targets into TOTK_SETUP_WORK (default games/totk/build/setup-import); hactool comes from
android/setup/hactool/deps.py and capstone from android/setup/rewrite/deps.py (TOTK_REWRITE_WORK, default
games/totk/build/setup-rewrite), both fetched once. Run: python -m unittest discover -s tests/setup -p 'test_import*.py' -v"""
import hashlib
import json
import os
import random
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / 'games/totk'
SETUP = GAME / 'android/setup'
WORK = Path(os.environ.get('TOTK_SETUP_WORK', GAME / 'build/setup-import'))
REWRITE_WORK = Path(os.environ.get('TOTK_REWRITE_WORK', GAME / 'build/setup-rewrite'))
sys.path[:0] = [str(ROOT / 'tests'), str(GAME / 'scripts')]
from test_game_containers import (HEADER, AREA, TITLEKEK, CONTENT, encrypt, nca, partition, ticket, game_image)  # noqa: E402
from cryptography.hazmat.primitives.ciphers import modes  # noqa: E402
from extract_exefs import decrypt  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402
from extract_shared_fonts import bfttf_size  # noqa: E402
PINS = json.loads((GAME / 'assets/expected-v1.4.3.json').read_text())
SECRETS = [k.hex() for k in (HEADER, AREA, TITLEKEK, CONTENT)]
_BUILT = {}


def _module(name, path):
    """android/setup/hactool/deps.py and android/setup/rewrite/deps.py share a file name; load each by path."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def build(sanitize=False):
    """Host CLI + host hactool; cached per process."""
    if sanitize in _BUILT:
        return _BUILT[sanitize]
    hactool = _module('hactool_deps', SETUP / 'hactool/deps.py').prepare_hactool(WORK, WORK / 'logs/fetch-hactool.log')
    capstone = _module('rewrite_deps', SETUP / 'rewrite/deps.py').prepare_capstone(REWRITE_WORK, REWRITE_WORK / 'capstone-host.log', target='host')
    out = WORK / ('out-san' if sanitize else 'out')
    extra = []
    if (SETUP / 'seed/deps.py').is_file():   # the shader seed stage links zstd
        zstd = _module('seed_deps', SETUP / 'seed/deps.py').prepare_zstd(WORK, WORK / 'logs/zstd-host.log', target='host')
        extra.append(f'ZSTD_PREFIX_HOST={zstd}')
    if (SETUP / 'companion/deps.py').is_file():   # the companion stage encodes WebP
        webp = _module('webp_deps', SETUP / 'companion/deps.py').prepare_libwebp(WORK, WORK / 'logs/libwebp-host.log', target='host')
        extra.append(f'WEBP_PREFIX_HOST={webp}')
    cmd = ['make', '-s', '-f', str(SETUP / 'setup.mk'), f'OUT={out}', 'HOST_CC=' + (shutil.which('cc') or 'gcc'),
           f'CAPSTONE_PREFIX_HOST={capstone}', f'HACTOOL_SRC={hactool}', f'SANITIZE={int(sanitize)}', *extra,
           f'-j{os.cpu_count() or 2}', 'setup-host']
    subprocess.run(cmd, cwd=GAME, check=True, stdout=subprocess.DEVNULL)
    _BUILT[sanitize] = out / 'totk-setup'
    return _BUILT[sanitize]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def romfs_image(files):
    """A RomFS image in the Nintendo layout (directory / file tables, names padded to 4) with nested directories."""
    tree = {'': {'dirs': [], 'files': []}}
    for path in sorted(files):
        parts = path.split('/')
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i]); parent = '/'.join(parts[:i - 1])
            if d not in tree:
                tree[d] = {'dirs': [], 'files': []}; tree[parent]['dirs'].append(d)
        tree['/'.join(parts[:-1])]['files'].append(path)
    order = list(tree)
    pad = lambda n: (n + 3) & ~3
    doff, off = {}, 0
    for d in order:
        doff[d] = off; off += 24 + pad(len(d.rsplit('/', 1)[-1].encode()) if d else 0)
    foff, off = {}, 0
    for d in order:
        for f in tree[d]['files']:
            foff[f] = off; off += 32 + pad(len(f.rsplit('/', 1)[-1].encode()))
    dirs, ftab, data = bytearray(), bytearray(), bytearray()
    for d in order:
        name = d.rsplit('/', 1)[-1].encode() if d else b''
        parent = d.rsplit('/', 1)[0] if '/' in d else ''
        sibs = tree[parent]['dirs'] if d else []
        sib = sibs[sibs.index(d) + 1] if d and sibs.index(d) + 1 < len(sibs) else None
        child = tree[d]['dirs'][0] if tree[d]['dirs'] else None
        first = tree[d]['files'][0] if tree[d]['files'] else None
        dirs += struct.pack('<6I', doff[parent], doff[sib] if sib else 0xffffffff, doff[child] if child else 0xffffffff,
                            foff[first] if first else 0xffffffff, 0xffffffff, len(name)) + name.ljust(pad(len(name)), b'\0')
        fl = tree[d]['files']
        for i, f in enumerate(fl):
            name = f.rsplit('/', 1)[-1].encode()
            nxt = foff[fl[i + 1]] if i + 1 < len(fl) else 0xffffffff
            ftab += struct.pack('<IIQQII', doff[d], nxt, len(data), len(files[f]), 0xffffffff, len(name)) + name.ljust(pad(len(name)), b'\0')
            data += files[f]; data += bytes((-len(data)) % 16)
    dt, ft = 80, 80 + len(dirs)
    dataoff = (ft + len(ftab) + 15) & ~15
    head = struct.pack('<10Q', 80, dt, 0, dt, len(dirs), ft, 0, ft, len(ftab), dataoff)
    return (head + dirs + ftab).ljust(dataoff, b'\0') + bytes(data)


def python_index(path):
    """The TOTKRFS1 bytes scripts/prepare_romfs_backend.py writes for this image."""
    files = index_romfs(Path(path))
    strings, rows = bytearray(), bytearray()
    for item in files:
        name = item['path'].encode() + b'\0'
        rows += struct.pack('<QQII', item['offset'], item['size'], len(strings), len(name)); strings += name
    return struct.pack('<8sQQ', b'TOTKRFS1', len(files), 24 + len(rows)) + rows + strings


def write_pins(path, romfs_bytes_or_size, romfs_sha, index_sha, exefs=None):
    pins = json.loads(json.dumps(PINS))
    pins['romfs'] = {'size': romfs_bytes_or_size, 'sha256': romfs_sha}
    pins['romfs_index_sha256'] = index_sha
    if exefs:
        pins['exefs_sha256'].update(exefs)
    path.write_text(json.dumps(pins))
    return path


class Base(unittest.TestCase):
    sanitize = False

    @classmethod
    def setUpClass(cls):
        try:
            cls.cli = build(cls.sanitize)
        except (subprocess.CalledProcessError, OSError) as e:
            raise unittest.SkipTest(f'cannot build the host setup CLI: {e}')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='totk-import-')
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.keys = self.dir / 'prod.keys'
        self.keys.write_text('# comment line\r\nHEADER_KEY = ' + HEADER.hex().upper() + '  # trailing\r\n'
                             f'key_area_key_application_00 = {AREA.hex()}\ntitlekek_00={TITLEKEK.hex()}\n'
                             'master_kek_source_00 = 0011zz not hex but not needed\n')

    def cli_run(self, *args, pass_fds=(), check_secret=True, **kw):
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0', UBSAN_OPTIONS='print_stacktrace=1')
        p = subprocess.run([str(self.cli), *map(str, args)], capture_output=True, text=True, pass_fds=pass_fds, env=env, **kw)
        self.assertGreaterEqual(p.returncode, 0, p.stderr)
        self.assertNotIn('Sanitizer', p.stderr, p.stderr)
        self.assertNotIn('runtime error', p.stderr, p.stderr)
        if check_secret:
            self.no_secret(p.stdout + p.stderr)
        out = json.loads(p.stdout.strip().splitlines()[-1]) if p.stdout.strip() else None
        return p, out

    def no_secret(self, text, extra=()):
        low = text.lower()
        for s in (*SECRETS, *extra):
            self.assertNotIn(s, low)

    def no_secret_files(self, folder):
        for p in Path(folder).rglob('*'):
            if p.is_file() and p.stat().st_size < (64 << 20):
                self.no_secret(p.read_bytes().decode('latin-1'))


class Keys(Base):
    def test_needed_keys_only(self):
        _, out = self.cli_run('keys', '--keys', self.keys)
        self.assertEqual(out, {'ok': True, 'header_key': True, 'titlekek': [0], 'key_area_key_application': [0]})

    def test_rejects_bad_or_missing_header_key(self):
        bad = self.dir / 'bad.keys'
        bad.write_text(f'header_key = {HEADER.hex()[:-2]}\n')
        p, out = self.cli_run('keys', '--keys', bad)
        self.assertEqual((p.returncode, out['code']), (1, 'bad_keys'))
        self.assertIn('line 1', out['message'])
        bad.write_text(f'titlekek_00 = {TITLEKEK.hex()}\n')
        p, out = self.cli_run('keys', '--keys', bad)
        self.assertEqual(out['code'], 'bad_keys')


class Containers(Base):
    def identify(self, image, update=False, keys=None):
        path = self.dir / ('game.nsp' if image[:4] == b'PFS0' else 'game.xci')
        path.write_bytes(image)
        return self.cli_run('identify', path, '--keys', keys or self.keys, *(['--update'] if update else []))

    def test_hactool_moves_bktr_buckets_with_memmove(self):
        """hactool spreads its BKTR relocation/subsection buckets in place, an overlapping move: bionic's arm64 memcpy
        corrupted the table and the real base + 1.4.3 merge failed on the device ("Failed to read RomFS directory cache!")
        while glibc's survived. The prepared source must use memmove, and the patch must be idempotent."""
        deps = _module('hactool_deps', SETUP / 'hactool/deps.py')
        nca = (WORK / 'src/hactool-setup/nca.c').read_text()
        for table in ('relocation', 'subsection'):
            self.assertEqual(nca.count(f'memmove(bktr_get_{table}_bucket('), 1)
            self.assertNotIn(f'memcpy(bktr_get_{table}_bucket(', nca)
        (self.dir / 'nca.c').write_text(nca.replace('memmove(bktr_get_relocation', 'memcpy(bktr_get_relocation'))
        deps.patch_bucket_memmove(self.dir)
        deps.patch_bucket_memmove(self.dir)
        self.assertEqual((self.dir / 'nca.c').read_text(), nca)

    def test_nsp_and_xci_key_area_and_ticket(self):
        for kind in ('nsp', 'xci'):
            for managed in (False, True):
                with self.subTest(kind=kind, managed=managed):
                    p, out = self.identify(game_image(kind, managed=managed))
                    self.assertEqual(p.returncode, 0, out)
                    self.assertEqual(out['kind'], kind)
                    self.assertEqual(out['program']['title_id'], '0100F2C0115B6000')
                    self.assertEqual(out['tickets'], int(managed))

    def test_wrong_keys_are_reported(self):
        wrong = self.dir / 'wrong.keys'
        wrong.write_text(f'header_key = {HEADER.hex()}\nkey_area_key_application_00 = {bytes(16).hex()}\ntitlekek_00 = {bytes(16).hex()}\n')
        for managed in (False, True):
            p, out = self.identify(game_image('nsp', managed=managed), keys=wrong)
            self.assertEqual(out['code'], 'bad_keys', out)
        wrong.write_text(f'header_key = {bytes(32).hex()}\n')
        p, out = self.identify(game_image('nsp'), keys=wrong)
        self.assertEqual(out['code'], 'bad_keys')
        self.assertIn('NCA3', out['message'])

    def test_personalized_ticket_rejected(self):
        image = partition([('program.nca', nca(True)), ('personal.tik', ticket(personalized=True))])
        p, out = self.identify(image)
        self.assertEqual(p.returncode, 1)
        self.assertIn('personalized', out['message'])

    def test_update_selection(self):
        data = nca(True)
        header = bytearray(b''.join(decrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), data[i * 512:(i + 1) * 512]) for i in range(6)))
        header[0x604] = 4
        header[0x2a0:0x2c0] = hashlib.sha256(header[0x600:0x800]).digest()
        update = b''.join(encrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), header[i * 512:(i + 1) * 512]) for i in range(6)) + data[0xc00:]
        image = partition([('program.nca', update), ('meta.cnmt.nca', nca(content=1, title=0x0100F2C0115B6800)), ('a.tik', ticket())])
        p, out = self.identify(image, update=True)
        self.assertEqual(p.returncode, 0, out)
        self.assertEqual(out['program']['bktr_section'], 1)
        p, out = self.identify(image)                       # an update is not a base game
        self.assertEqual(out['code'], 'unsupported_version')
        self.assertIn('this is the update, not the base game', out['message'])
        p, out = self.identify(game_image('nsp'), update=True)   # a base game is not an update
        self.assertIn('BKTR', out['message'])
        p, out = self.identify(game_image('xci'), update=True)
        self.assertIn('NSP', out['message'])
        other = partition([('program.nca', nca(title=0x0100000000010000))])
        p, out = self.identify(other)
        self.assertEqual(out['code'], 'unsupported_version')
        self.assertIn('another game', out['message'])

    def test_malformed_containers_fail_cleanly(self):
        rng = random.Random(1234)
        samples = [game_image('nsp', managed=True), game_image('xci')]
        cases = []
        for image in samples:
            cases += [image[:n] for n in sorted({0, 3, 15, 16, 40, 0x100, 0x137, 0x200, 0x210, 0x260, 0x400, 0xc00, len(image) // 2, len(image) - 1})]
            for _ in range(40):
                b = bytearray(image)
                for _ in range(rng.randint(1, 4)):
                    b[rng.randrange(0, min(len(b), 0x400))] = rng.randrange(256)
                cases.append(bytes(b))
        nsp = bytearray(samples[0])
        bad = []
        b = bytearray(nsp); struct.pack_into('<I', b, 4, 0xffffffff); bad.append(b)               # huge count
        b = bytearray(nsp); struct.pack_into('<I', b, 8, 0x7fffffff); bad.append(b)               # huge string table
        b = bytearray(nsp); struct.pack_into('<Q', b, 16, 1 << 62); bad.append(b)                 # entry offset out of bounds
        b = bytearray(nsp); struct.pack_into('<Q', b, 24, 1 << 63); bad.append(b)                 # entry size overflow
        b = bytearray(nsp); struct.pack_into('<I', b, 32, 0xfffffff0); bad.append(b)              # name offset out of bounds
        b = bytearray(nsp); struct.pack_into('<Q', b, 16 + 24, 0); bad.append(b)                  # overlapping entries
        count, strsize = struct.unpack_from('<II', nsp, 4)
        b = bytearray(nsp); s = 16 + count * 24; b[s:s + strsize] = b'x' * strsize; bad.append(b)  # unterminated names
        b = bytearray(nsp); b[s:s + 11] = b'program.nca'; b[s + 12:s + 23] = b'program.nca'; bad.append(b)  # duplicate names
        xci = bytearray(samples[1]); struct.pack_into('<Q', xci, 0x130, len(xci) + 5); bad.append(xci)  # root beyond the file
        cases += [bytes(x) for x in bad]
        for i, image in enumerate(cases):
            with self.subTest(case=i):
                p, out = self.identify(image)
                self.assertIn(p.returncode, (0, 1), p.stderr)
                self.assertIsNotNone(out)
                if p.returncode:
                    self.assertFalse(out['ok'])
        for i in range(len(bad)):
            with self.subTest(bad=i):
                p, out = self.identify(bytes(bad[i]))
                self.assertEqual(p.returncode, 1)


class DecryptedFixture(Base):
    """The run() path with real inherited descriptors, a synthetic RomFS and pins for it (helpers only)."""
    FILES = {'Actor/Pack/a.pack.zs': b'A' * 1000, 'Actor/Pack/b.pack.zs': b'B' * 33, 'Mals/USen.Product.100.sarc.zs': os.urandom(4096),
             'System/Resource/ResourceSizeTable.Product.rsizetable.zs': b'rst', 'Model/x.bfres.mc': os.urandom(70000),
             'Model/Sub/deep/y.txt': 'é名前'.encode(), 'top.bin': b'\0' * 17}

    def setUp(self):
        super().setUp()
        self.game = self.dir / 'game'; self.nobackup = self.dir / 'nobackup'
        self.image = romfs_image(self.FILES)
        (self.dir / 'romfs.bin').write_bytes(self.image)
        self.index = python_index(self.dir / 'romfs.bin')
        self.font = b'\0\1\0\0' + os.urandom(1001)
        (self.dir / 'font.ttf').write_bytes(b'APKHEADER' + self.font + b'TRAILER')
        self.pins = write_pins(self.dir / 'pins.json', len(self.image), sha(self.image), sha(self.index))

    def request(self, fds, stages=None, **inputs):
        req = {'gameDir': str(self.game), 'privateDir': str(self.dir / 'private'), 'noBackupDir': str(self.nobackup),
               'nativeLibDir': str(self.cli.parent / 'host'), 'inputs': inputs,
               'fonts': [{'fd': fds['font'], 'offset': 9, 'length': len(self.font), 'name': 'NotoSans-Regular.ttf'}]}
        if stages is not None:
            req['stages'] = stages
        path = self.dir / 'request.json'
        path.write_text(json.dumps(req))
        return path

    def open_fds(self, parts):
        fds = {'font': os.open(self.dir / 'font.ttf', os.O_RDONLY)}
        fds['parts'] = [os.open(p, os.O_RDONLY) for p in parts]
        self.addCleanup(lambda: [os.close(f) for f in [fds['font'], *fds['parts']]])
        return fds

    def run_setup(self, parts, stages=None, pins=None, cmd='run', **popen):
        fds = self.open_fds(parts)
        req = self.request(fds, stages, romfs=[{'fd': f, 'size': 1, 'name': Path(p).name} for f, p in zip(fds['parts'], parts)])
        all_fds = (fds['font'], *fds['parts'])
        if popen:
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
            return subprocess.Popen([str(self.cli), cmd, '--request', str(req), '--pins', str(pins or self.pins)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, pass_fds=all_fds, env=env)
        return self.cli_run(cmd, '--request', req, '--pins', pins or self.pins, pass_fds=all_fds)

    def install(self):
        return json.loads((self.game / 'install.json').read_text())


class Decrypted(DecryptedFixture):
    def test_romfs_index_fonts_and_install_json(self):
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs', 'index', 'fonts'])
        self.assertTrue(out['ok'], out)
        self.assertEqual(out['install'], self.install())
        st = self.install()['stages']
        self.assertEqual(st['romfs'], {'done': True, 'size': len(self.image), 'sha256': sha(self.image), 'parts': 0})
        self.assertEqual(st['index'], {'done': True, 'sha256': sha(self.index)})
        self.assertEqual(st['fonts'], {'done': True, 'source': 'NotoSans-Regular.ttf', 'sha256': sha(self.font)})
        self.assertFalse(st['code']['done']); self.assertFalse(self.install()['playable'])
        self.assertEqual((self.game / 'extracted/v143/romfs/romfs.bin').read_bytes(), self.image)
        self.assertEqual((self.game / 'extracted/v143/romfs/runtime-index.bin').read_bytes(), self.index)
        self.assertEqual((self.game / 'game-data/v143/romfs-config.txt').read_text(),
                         f'ROMFS_IMAGE_SIZE {len(self.image)}\nROMFS_IMAGE_SHA256 {sha(self.image)}\nROMFS_INDEX_SHA256 {sha(self.index)}\n')
        txt = (self.game / 'game-data/v143/shared-fonts.txt').read_text().splitlines()
        names = ['0-standard', '1-chinese-simplified', '2-chinese-simplified-extension', '3-chinese-traditional', '4-korean', '5-nintendo-extension']
        for i, name in enumerate(names):
            data = (self.game / f'extracted/firmware/shared-fonts/{name}.bfttf').read_bytes()
            size = bfttf_size(data)                         # the firmware decoder accepts it
            key = struct.unpack('>I', data[:4])[0] ^ 0x7f9a0218
            plain = b''.join(struct.pack('>I', w ^ key) for w in struct.unpack(f'>{size // 4}I', data[8:]))
            self.assertEqual(plain, self.font.ljust(size, b'\0'))
            self.assertEqual(txt[4 * i:4 * i + 4], [f'FONT{i}_FILE {name}.bfttf', f'FONT{i}_FILE_BYTES {len(data)}',
                                                   f'FONT{i}_FONT_BYTES {size}', f'FONT{i}_SHA256 {sha(data)}'])
        self.assertFalse((self.game / '.import').exists())
        self.assertFalse((self.nobackup / 'import-keys').exists())
        # Everything requested is done: a second run does nothing and changes nothing.
        before = (self.game / 'install.json').stat().st_mtime_ns
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=None)
        self.assertEqual(out['code'], 'missing_input')     # code is still pending and has no input
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs', 'index'])
        self.assertTrue(out['ok'])
        self.assertEqual((self.game / 'install.json').stat().st_mtime_ns != before, True)   # explicit stages re-run

    def test_split_parts(self):
        cut = [0, 81, 4000, len(self.image) - 1, len(self.image)]
        parts = []
        for i in range(len(cut) - 1):
            part = self.dir / f'romfs.{i}'; part.write_bytes(self.image[cut[i]:cut[i + 1]]); parts.append(part)
        p, out = self.run_setup(parts, stages=['romfs'])
        self.assertTrue(out['ok'], out)
        self.assertEqual((self.game / 'extracted/v143/romfs/romfs.bin').read_bytes(), self.image)
        p, out = self.run_setup([parts[0], parts[2], parts[1], parts[3]], stages=['romfs'])   # out of order
        self.assertEqual((out['stage'], out['code']), ('romfs', 'hash_mismatch'))
        p, out = self.run_setup(parts[1:] + parts[:1], stages=['romfs'])
        self.assertEqual(out['code'], 'missing_input')
        self.assertIn('raw RomFS', out['message'])

    def test_hash_mismatch_and_wrong_size_commit_nothing(self):
        bad = bytearray(self.image); bad[-5] ^= 1
        (self.dir / 'bad.bin').write_bytes(bad)
        p, out = self.run_setup([self.dir / 'bad.bin'], stages=['romfs'])
        self.assertEqual((p.returncode, out['stage'], out['code']), (1, 'romfs', 'hash_mismatch'))
        (self.dir / 'short.bin').write_bytes(self.image[:-1])
        p, out = self.run_setup([self.dir / 'short.bin'], stages=['romfs'])
        self.assertEqual(out['code'], 'hash_mismatch')
        self.assertIn('bytes', out['message'])
        self.assertFalse((self.game / 'extracted/v143/romfs/romfs.bin').exists())
        self.assertFalse((self.game / 'install.json').exists())
        self.assertEqual([x for x in (self.game / '.import').rglob('*') if x.is_file()], [])

    def test_no_space(self):
        pins = write_pins(self.dir / 'huge.json', 1 << 55, sha(self.image), sha(self.index))
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs'], pins=pins)
        self.assertEqual(out['code'], 'no_space')
        self.assertIn('GB', out['message'])

    def big_image(self, size=384 << 20):
        path = self.dir / 'big.bin'
        with path.open('wb') as f:
            f.write(self.image); f.truncate(size)
        h = hashlib.sha256()
        with path.open('rb') as f:
            for block in iter(lambda: f.read(8 << 20), b''):
                h.update(block)
        index = python_index(path)
        return path, write_pins(self.dir / 'big.json', size, h.hexdigest(), sha(index))

    def wait_for(self, proc, marker):
        for line in proc.stderr:
            if marker in line:
                return True
        return False

    def test_windowed_copy_is_byte_identical(self):
        # The copy writes back and drops the page cache per 128 MiB window (st_cache_written): non-zero data across
        # several windows, split into parts whose seams fall inside windows, must still arrive bit for bit.
        size = (300 << 20) + 12345
        path = self.dir / 'noisy.bin'
        with path.open('wb') as f:
            f.write(self.image)
            rng = random.Random(7)
            while f.tell() < size:
                f.write(rng.randbytes(min(4 << 20, size - f.tell())))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        pins = write_pins(self.dir / 'noisy.json', size, digest, sha(python_index(path)))
        data = path.read_bytes()
        cut = [0, (100 << 20) + 3, (200 << 20) - 5, size]
        parts = []
        for i in range(len(cut) - 1):
            part = self.dir / f'noisy.{i}'; part.write_bytes(data[cut[i]:cut[i + 1]]); parts.append(part)
        for picks in ([path], parts):
            p, out = self.run_setup(picks, stages=['romfs'], pins=pins)
            self.assertTrue(out['ok'], out)
            got = self.game / 'extracted/v143/romfs/romfs.bin'
            self.assertEqual(got.stat().st_size, size)
            self.assertEqual(hashlib.sha256(got.read_bytes()).hexdigest(), digest)
            self.assertEqual(out['install']['stages']['romfs']['sha256'], digest)

    def test_cancel_and_resume_after_kill(self):
        big, pins = self.big_image()
        # Commit fonts first, so a later killed run has something committed to keep.
        p, out = self.run_setup([big], stages=['fonts'], pins=pins)
        self.assertTrue(out['ok'], out)
        fonts_mtime = (self.game / 'game-data/v143/shared-fonts.txt').stat().st_mtime_ns
        # Cancellation (SIGTERM -> cancel flag): reported as cancelled, nothing of the RomFS committed or left in staging.
        proc = self.run_setup([big], stages=['romfs', 'index'], pins=pins, popen=True)
        self.assertTrue(self.wait_for(proc, '[romfs]'))
        proc.send_signal(signal.SIGTERM)
        stdout, _ = proc.communicate(timeout=60)
        out = json.loads(stdout.strip().splitlines()[-1])
        self.assertEqual((proc.returncode, out['code'], out['stage']), (3, 'cancelled', 'romfs'), out)
        self.assertFalse((self.game / 'extracted/v143/romfs/romfs.bin').exists())
        self.assertFalse((self.game / '.import/romfs/romfs.bin').exists())
        self.assertTrue(self.install()['stages']['fonts']['done'])
        # A killed run leaves partial staging behind; the next run discards it and keeps the committed fonts.
        proc = self.run_setup([big], stages=['romfs', 'index', 'fonts'], pins=pins, popen=True)
        self.assertTrue(self.wait_for(proc, '[romfs]'))
        staging = self.game / '.import/romfs/romfs.bin'
        deadline = time.monotonic() + 10
        try:
            # The stage-start progress precedes file creation; kill only after there is staging to recover.
            while not staging.exists() and proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(staging.exists(), 'RomFS staging was not created before the kill deadline')
        finally:
            proc.kill(); proc.communicate(timeout=60)
        self.assertTrue((self.game / '.import/romfs/romfs.bin').exists())
        self.assertTrue(self.install()['stages']['fonts']['done'])
        fds = self.open_fds([big])
        req = self.request(fds, None, romfs=[{'fd': fds['parts'][0], 'size': 0, 'name': 'big.bin'}])
        data = json.loads(req.read_text()); data['stages'] = ['romfs', 'index', 'fonts']
        # Only pending stages run when "stages" is absent; code stays pending without input, so name the rest
        # explicitly but leave fonts out to prove it is not redone.
        data['stages'] = ['romfs', 'index']; req.write_text(json.dumps(data))
        p, out = self.cli_run('run', '--request', req, '--pins', pins, pass_fds=(fds['font'], fds['parts'][0]))
        self.assertTrue(out['ok'], out)
        self.assertNotIn('[fonts]', p.stderr)
        self.assertEqual((self.game / 'game-data/v143/shared-fonts.txt').stat().st_mtime_ns, fonts_mtime)
        self.assertFalse((self.game / '.import').exists())
        st = self.install()['stages']
        self.assertTrue(st['romfs']['done'] and st['index']['done'] and st['fonts']['done'])
        # Pending-only mode: with code input missing it refuses before touching anything.
        data.pop('stages'); req.write_text(json.dumps(data))
        p, out = self.cli_run('run', '--request', req, '--pins', pins, pass_fds=(fds['font'], fds['parts'][0]))
        self.assertEqual((out['stage'], out['code']), ('code', 'missing_input'))

    def test_shader_stage_failure_fails_setup_companion_failure_does_not(self):
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs', 'index', 'fonts', 'companion', 'shaders'])
        # The shader seed is required (without it the game draws nothing): its failure is the run's failure.
        self.assertEqual((out['ok'], out['stage'], out['code']), (False, 'shaders', 'missing_input'), out)   # needs the code stage first
        install = self.install(); st = install['stages']
        self.assertTrue(st['romfs']['done'] and st['index']['done'] and st['fonts']['done'])
        self.assertFalse(st['companion']['done'])
        self.assertIn(st['companion']['code'], ('not_available', 'missing_input', 'internal', 'io', 'hash_mismatch'))
        self.assertIsInstance(st['companion']['error'], str)
        self.assertEqual(st['shaders']['code'], 'missing_input')
        self.assertEqual(st['shaders']['variants'], 0)
        self.assertFalse(install['playable'])

    def test_missing_outputs_reopen_a_done_stage(self):
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs', 'index', 'fonts'])
        self.assertTrue(out['ok'])
        (self.game / 'extracted/v143/romfs/runtime-index.bin').unlink()
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['fonts'])
        self.assertFalse(out['install']['stages']['index']['done'])
        self.assertFalse(self.install()['stages']['index']['done'])

    def test_inspect(self):
        p, out = self.run_setup([self.dir / 'romfs.bin'], cmd='inspect')
        self.assertTrue(out['ok'], out)
        self.assertEqual(out['route'], 'decrypted')
        self.assertEqual(out['items'][0]['role'], 'romfs'); self.assertTrue(out['items'][0]['ok'])
        self.assertEqual(out['need'], ['exefs'])
        self.assertFalse(self.game.exists())               # inspect writes nothing
        (self.dir / 'short.bin').write_bytes(self.image[:-1])
        p, out = self.run_setup([self.dir / 'short.bin'], cmd='inspect')
        self.assertFalse(out['items'][0]['ok']); self.assertIn('Size', out['items'][0]['problem'])


RUNTIME = GAME / 'runtime'
PROBE_PRELUDE = r'''
#include <stdint.h>
#include <pthread.h>
#define TOTK_UI(zh,en) (en)
_Noreturn void runtime_fail(const char *message);
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total);
#include "host_os.h"
'''
PROBE_MAIN = r'''
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(7); }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) { (void)stage; (void)done; (void)total; }
int main(int argc, char **argv) {   /* image sha256 size */
    if (argc != 4) return 2;
    romfs_image_sha256 = argv[2]; romfs_image_size = strtoull(argv[3], NULL, 10);
    int fd = open(argv[1], O_RDONLY); if (fd < 0) return 3;
    verify_image(fd, "unused"); close(fd); return 0;
}
'''


class RomfsProofHandoff(DecryptedFixture):
    """The record setup writes after the romfs stage is the one runtime/romfs.c verify_image reuses (its production code,
    compiled for the host as scripts/verify_romfs_proof.py does on a Mac)."""
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        source = (RUNTIME / 'romfs.c').read_text()
        source = source[:source.index('static uint64_t lower(const char *name);')]
        source = '\n'.join(l for l in source.splitlines() if not l.startswith('#include "') or l == '#include "romfs_proof.h"')
        # The runtime's own SHA-256 (host_os.c); the rest of that file is arm64 signal-frame code.
        host_os = (RUNTIME / 'host_os.c').read_text()
        sha_part = host_os[host_os.index('/* SHA-256 (FIPS 180-4)'):host_os.rindex('#endif')]
        cls.probe = WORK / 'romfs-proof-probe'
        c = WORK / 'romfs-proof-probe.c'
        c.write_text(PROBE_PRELUDE + source + sha_part + PROBE_MAIN)
        subprocess.run([shutil.which('cc') or 'gcc', '-std=gnu11', '-O1', '-w', '-I', str(RUNTIME), str(c), '-o', str(cls.probe)], check=True)

    def verify(self, image, **env):
        e = dict(os.environ, TOTK_ROMFS_PROOF_DIR=str(self.dir / 'private/state'))
        e.pop('TOTK_ROMFS_VERIFY', None); e.update(env)
        return subprocess.run([str(self.probe), str(image), sha(self.image), str(len(self.image))], capture_output=True, text=True, env=e)

    def test_runtime_reuses_setup_record(self):
        state = self.dir / 'private/state'; state.mkdir(parents=True)
        (state / '.romfs-verified-v1').write_bytes(b'stale')           # removed when the romfs stage starts
        (state / '.romfs-verified-parts-v1').write_bytes(b'stale')
        bad = bytearray(self.image); bad[-1] ^= 1; (self.dir / 'bad.bin').write_bytes(bad)
        p, out = self.run_setup([self.dir / 'bad.bin'], stages=['romfs'])
        self.assertEqual(out['code'], 'hash_mismatch')
        self.assertEqual(sorted(state.iterdir()), [], 'a failed romfs stage leaves no record')
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs', 'index', 'fonts'])
        self.assertTrue(out['ok'], out)
        record = state / '.romfs-verified-v1'
        self.assertEqual(record.stat().st_mode & 0o777, 0o600)
        written = record.read_bytes()
        image = self.game / 'extracted/v143/romfs/romfs.bin'
        r = self.verify(image)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('reused full SHA256 verification record', r.stderr)
        self.assertNotIn('verifying local RomFS', r.stderr)
        self.assertEqual(record.read_bytes(), written)
        # The runtime writes the very same bytes for this unchanged file (one record definition, romfs_proof.h).
        record.unlink()
        r = self.verify(image)
        self.assertIn('verifying local RomFS SHA256', r.stderr)
        self.assertEqual(record.read_bytes(), written)
        # A changed modification time invalidates the record: the runtime hashes again and writes its own.
        st = image.stat(); os.utime(image, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))
        r = self.verify(image)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('verifying local RomFS SHA256', r.stderr)
        self.assertNotEqual(record.read_bytes(), written)
        r = self.verify(image)
        self.assertIn('reused full SHA256 verification record', r.stderr)
        # Re-importing replaces the file (new inode and times): the old record is dropped and a fresh one written.
        p, out = self.run_setup([self.dir / 'romfs.bin'], stages=['romfs'])
        self.assertTrue(out['ok'], out)
        r = self.verify(image)
        self.assertIn('reused full SHA256 verification record', r.stderr)
        self.assertNotIn('verifying local RomFS', r.stderr)
        # TOTK_ROMFS_VERIFY=full still forces a fresh hash over the record.
        r = self.verify(image, TOTK_ROMFS_VERIFY='full')
        self.assertIn('[forced]', r.stderr)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_no_private_dir_no_record(self):
        fds = self.open_fds([self.dir / 'romfs.bin'])
        req = self.request(fds, ['romfs'], romfs=[{'fd': fds['parts'][0], 'size': 1, 'name': 'romfs.bin'}])
        data = json.loads(req.read_text()); data.pop('privateDir'); req.write_text(json.dumps(data))
        p, out = self.cli_run('run', '--request', req, '--pins', self.pins, pass_fds=(fds['font'], fds['parts'][0]))
        self.assertTrue(out['ok'], out)
        self.assertFalse((self.dir / 'private').exists())


class Exefs(Base):
    """ExeFS inputs are recognised by their pinned hash, as files or as one raw PFS0 image."""
    def setUp(self):
        super().setUp()
        self.files = {n: os.urandom(100 + i) for i, n in enumerate(('main', 'sdk', 'subsdk0', 'main.npdm', 'rtld'))}
        self.pins = write_pins(self.dir / 'pins.json', 100, '0' * 64, '0' * 64, {n: sha(d) for n, d in self.files.items()})

    def inspect(self, inputs):
        fds = []
        for name, data in inputs:
            path = self.dir / name; path.write_bytes(data); fds.append(os.open(path, os.O_RDONLY))
        self.addCleanup(lambda: [os.close(f) for f in fds])
        req = self.dir / 'req.json'
        req.write_text(json.dumps({'gameDir': str(self.dir / 'g'), 'noBackupDir': str(self.dir / 'n'), 'nativeLibDir': '/x',
                                   'inputs': {'exefs': [{'fd': f, 'size': 0, 'name': n} for f, (n, _) in zip(fds, inputs)]}}))
        return self.cli_run('inspect', '--request', req, '--pins', self.pins, pass_fds=fds)[1]

    def test_files_by_hash_and_pfs0(self):
        out = self.inspect([('a', self.files['sdk']), ('b', self.files['main']), ('c', self.files['subsdk0'])])
        self.assertTrue(out['items'][0]['ok'], out)
        self.assertEqual(out['need'], ['romfs'])
        out = self.inspect([('exefs.pfs0', partition(list(self.files.items())))])
        self.assertTrue(out['items'][0]['ok'], out)
        out = self.inspect([('main', b'other main'), ('sdk', self.files['sdk'])])
        self.assertFalse(out['items'][0]['ok'])
        self.assertIn('not a 1.4.3', out['items'][0]['problem'])


class KeysSan(Keys):
    sanitize = True


class ContainersSan(Containers):
    sanitize = True


class DecryptedSan(Decrypted):
    sanitize = True


class ExefsSan(Exefs):
    sanitize = True


class RomfsProofHandoffSan(RomfsProofHandoff):
    sanitize = True


if __name__ == '__main__':
    unittest.main()

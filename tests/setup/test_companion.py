"""The on-device companion stage (android/setup/companion, setup_stage_companion) against scripts/make_companion_assets.py.

CompanionErrors (needs cc, cmake and git; no game data): a RomFS folder without the game fails with missing_input.

CompanionOracle (skipped without the reference data): `totk-setup companion` runs on the reference RomFS in place
(TOTK_REF_WORK, default build/TOTK_build/totk in the workspace folder that holds this checkout: extracted/v143/romfs with romfs.bin and
runtime-index.bin) and its tree is compared with the Python generator's output (TOTK_COMPANION_REF, default
<workspace>/build/TOTK_build/companion-v143/companion):
  - the same file list; index.json, text/*.json, map/markers.json and the font byte-identical;
  - every image decoded and compared per category, with the max / mean absolute channel error reported. Pure texture
    decodes (map tiles, the ui textures) must match exactly. Resampled or composited images (icon / ability thumbnails,
    decor, map overviews) go through the libImaging ports and libm's sin / cos / hypot; a last-bit libm difference on
    another host can move one input value by 1, which the lossy WebP re-encode then spreads, so they are allowed
    TOLERANCE. On the reference host (and on the AYN Thor) every file is byte-identical.
  - cancelling (SIGINT) a run exits with the cancelled code;
  - a second run writes a byte-identical tree (the image phase runs in any thread order);
  - every item and Combined fuse attachment in every progress.sav under TOTK_COMPANION_SAVES (os.pathsep-separated
    folders or save ZIPs; skipped when unset) has its game name in every language, so the companion shows no
    humanized actor name ("Weapon Sword 077"); pouch items without an icon are printed.
Pillow (requirements.txt) decodes the images; without it only byte equality is checked. The host CLI is built from
android/setup/setup.mk with capstone, zstd and libwebp from the stage deps helpers into TOTK_COMPANION_WORK (default
games/totk/build/setup-companion). Outputs go to a temporary folder; nothing is written into the reference folders."""
import hashlib
import importlib.util
import json
import os
import shutil
import signal
import struct
import sys
import subprocess
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / 'games/totk'
SETUP = GAME / 'android/setup'
WORK = Path(os.environ.get('TOTK_COMPANION_WORK', GAME / 'build/setup-companion'))
REF = Path(os.environ.get('TOTK_REF_WORK', ROOT.parent / 'build/TOTK_build/totk'))
ROMFS = REF / 'extracted/v143/romfs'
ORACLE = Path(os.environ.get('TOTK_COMPANION_REF', ROOT.parent / 'build/TOTK_build/companion-v143/companion'))
TOLERANCE = (16, 0.25)   # max / mean absolute channel error for resampled and composited images (see above)
SAVES = [Path(p) for p in os.environ.get('TOTK_COMPANION_SAVES', '').split(os.pathsep) if p]
EXACT = (0, 0.0)

try:
    from PIL import Image, ImageChops
except ImportError:
    Image = None


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


_CLI = []


def build():
    """totk-setup (host) with the code, seed and companion stages; cached per process."""
    if _CLI:
        return _CLI[0]
    WORK.mkdir(parents=True, exist_ok=True)
    logs = WORK / 'logs'
    capstone = _module('rewrite_deps', SETUP / 'rewrite/deps.py').prepare_capstone(WORK, logs / 'capstone-host.log', target='host')
    zstd = _module('seed_deps', SETUP / 'seed/deps.py').prepare_zstd(WORK, logs / 'zstd-host.log', target='host')
    webp = _module('webp_deps', SETUP / 'companion/deps.py').prepare_libwebp(WORK, logs / 'libwebp-host.log', target='host')
    out = WORK / 'out'
    subprocess.run(['make', '-s', '-f', str(SETUP / 'setup.mk'), f'OUT={out}', 'HOST_CC=' + (shutil.which('cc') or 'gcc'),
                    f'CAPSTONE_PREFIX_HOST={capstone}', f'ZSTD_PREFIX_HOST={zstd}', f'WEBP_PREFIX_HOST={webp}',
                    f'-j{os.cpu_count() or 2}', str(out / 'totk-setup')], cwd=GAME, check=True, stdout=subprocess.DEVNULL)
    _CLI.append(out / 'totk-setup')
    return _CLI[0]


def category(rel):
    parts = rel.split('/')
    if parts[0] == 'map':
        return ('map overviews', TOLERANCE) if parts[-1] == 'overview.webp' else ('map tiles', EXACT)
    if parts[0] == 'ui':
        return ('ui thumbnails', TOLERANCE) if parts[1].startswith('ability_') else ('ui', EXACT)
    return parts[0], TOLERANCE   # icons (thumbnails), decor (layout renders)


def files(root):
    return sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file())


def mm3(text):
    """MurmurHash3 x86_32, seed 0: the progress.sav key hash."""
    data, h = text.encode(), 0
    def rotl(x, r): return ((x << r) | (x >> (32 - r))) & 0xffffffff
    for i in range(0, len(data) // 4 * 4, 4):
        k = rotl(int.from_bytes(data[i:i + 4], 'little') * 0xcc9e2d51 & 0xffffffff, 15) * 0x1b873593 & 0xffffffff
        h = (rotl(h ^ k, 13) * 5 + 0xe6546b64) & 0xffffffff
    tail = data[len(data) // 4 * 4:]
    if tail:
        h ^= rotl(int.from_bytes(tail, 'little') * 0xcc9e2d51 & 0xffffffff, 15) * 0x1b873593 & 0xffffffff
    h ^= len(data)
    for shift, mul in ((16, 0x85ebca6b), (13, 0xc2b2ae35)):
        h = (h ^ (h >> shift)) * mul & 0xffffffff
    return h ^ (h >> 16)


POUCH = {mm3(f'Pouch.{c}.Content.Name'): c for c in ('Weapon', 'Bow', 'Arrow', 'Shield', 'Armor', 'Material', 'Food',
                                                    'SpecialParts', 'KeyItem', 'SpecialPower')}
POUCH.update({mm3(f'Pouch.{c}.Content.Combined.Name'): c + '.Combined' for c in ('Weapon', 'Bow', 'Arrow', 'Shield')})


def save_pouch(path, data=None):
    """{category: [actor, ...]} from progress.sav: pouch names and Combined attachment names are String64 arrays
    ({u32 count, 64-byte names}) addressed by the header's (key hash, offset) table; 0xa3db7114 ends the table."""
    data, out = path.read_bytes() if data is None else data, {}
    if data[:4] != b'\x04\x03\x02\x01':
        raise ValueError(f'{path} is not a progress.sav')
    for at in range(0x28, len(data) - 8, 8):
        key, value = int.from_bytes(data[at:at + 4], 'little'), int.from_bytes(data[at + 4:at + 8], 'little')
        if key == 0xa3db7114:
            break
        if key in POUCH:
            count = int.from_bytes(data[value:value + 4], 'little')
            names = (data[value + 4 + 64 * i:value + 68 + 64 * i].split(b'\0')[0].decode() for i in range(count))
            out[POUCH[key]] = [name for name in names if name and name != 'None']
    return out


def save_blobs(root):
    if root.is_file() and root.suffix.lower() == '.zip':
        with zipfile.ZipFile(root) as archive:
            for name in sorted(archive.namelist()):
                if name.endswith('/progress.sav') or name == 'progress.sav':
                    yield f'{root}!{name}', archive.read(name)
    else:
        for path in sorted(root.rglob('progress.sav')):
            yield path, path.read_bytes()


class CompanionSaveNames(unittest.TestCase):
    @unittest.skipUnless(sys.version_info >= (3, 14), 'reference MSBT reader requires Python 3.14')
    def test_msbt_keeps_standalone_declension_form(self):
        sys.path.insert(0, str(GAME / 'scripts'))
        try:
            generator = _module('make_companion_assets', GAME / 'scripts/make_companion_assets.py')
        finally:
            sys.path.pop(0)
        for endian, encoding, bom in (('<', 'utf-16-le', b'\xff\xfe'), ('>', 'utf-16-be', b'\xfe\xff')):
            with self.subTest(endian=endian):
                first, other = 'Example имя'.encode(encoding), 'Other form'.encode(encoding)
                payload = struct.pack(endian + 'H', len(first)) + first + struct.pack(endian + 'H', len(other)) + other
                message = struct.pack(endian + '4H', 0xe, 201, 10, len(payload)) + payload + b'\0\0'
                labels = struct.pack(endian + '3I', 1, 1, 12) + b'\x04Name' + struct.pack(endian + 'I', 0)
                texts = struct.pack(endian + '2I', 1, 8) + message
                data = bytearray(32)
                data[:8], data[8:10], data[12] = b'MsgStdBn', bom, 1
                struct.pack_into(endian + 'H', data, 14, 2)
                for name, section in ((b'LBL1', labels), (b'TXT2', texts)):
                    data += name + struct.pack(endian + 'I', len(section)) + b'\0' * 8 + section
                    data += b'\0' * (-len(data) % 16)
                self.assertEqual(generator.msbt(bytes(data)), {'Name': 'Example имя'})

    def test_pouch_and_all_combined_arrays_in_save_zip(self):
        # Synthetic save structure only: no game files or text in the test fixture.
        arrays = {key: ['TestActor', '', 'None'] for key in POUCH}
        table_end = 0x28 + 8 * (len(arrays) + 1)
        data = bytearray(table_end)
        data[:4] = b'\x04\x03\x02\x01'
        for i, (key, names) in enumerate(arrays.items()):
            struct.pack_into('<II', data, 0x28 + 8 * i, key, len(data))
            data += struct.pack('<I', len(names)) + b''.join(name.encode().ljust(64, b'\0') for name in names)
        struct.pack_into('<II', data, table_end - 8, 0xa3db7114, 0)
        with tempfile.TemporaryDirectory() as d:
            archive = Path(d) / 'saves.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('Export/slot_00/progress.sav', data)
            blobs = list(save_blobs(archive))
            self.assertEqual(len(blobs), 1)
            self.assertEqual(save_pouch(*blobs[0]), {cat: ['TestActor'] for cat in POUCH.values()})


tools = shutil.which('cc') and shutil.which('cmake') and shutil.which('git') and shutil.which('make')


@unittest.skipUnless(tools, 'needs cc, make, cmake and git')
class CompanionErrors(unittest.TestCase):
    def test_romfs_without_the_game_is_missing_input(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'romfs').mkdir()
            r = subprocess.run([build(), 'companion', '--romfs', Path(d) / 'romfs', '--out', Path(d) / 'out'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1, r.stderr)
            result = json.loads(r.stdout)
            self.assertFalse(result['ok'])
            self.assertEqual(result['code'], 'missing_input')


@unittest.skipUnless(tools, 'needs cc, make, cmake and git')
@unittest.skipUnless((ROMFS / 'romfs.bin').is_file() and (ROMFS / 'runtime-index.bin').is_file() and (ORACLE / 'index.json').is_file(),
                     f'reference RomFS {ROMFS} or Python companion output {ORACLE} not present')
class CompanionOracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cli = build()
        cls.tmp = Path(tempfile.mkdtemp(prefix='totk-companion-'))
        cls.out = cls.tmp / 'companion'
        t0 = time.monotonic()
        cls.run_ = subprocess.run([cli, 'companion', '--romfs', ROMFS, '--out', cls.out], capture_output=True, text=True)
        cls.seconds = time.monotonic() - t0

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.assertEqual(self.run_.returncode, 0, self.run_.stdout + self.run_.stderr[-2000:])

    def test_file_list(self):
        self.assertEqual(files(self.out), files(ORACLE))

    def test_json_and_font_identical(self):
        names = [f for f in files(ORACLE) if f.endswith('.json') or f.startswith('fonts/')]
        self.assertIn('index.json', names)
        self.assertTrue(any(f.startswith('text/') for f in names))
        self.assertTrue(any(f.startswith('fonts/') for f in names))
        for name in names:
            self.assertEqual((self.out / name).read_bytes(), (ORACLE / name).read_bytes(), name)
        index = json.loads((self.out / 'index.json').read_text())
        self.assertEqual((index['schema'], index['generator']), (1, 5))

    def test_images(self):
        report, failures = {}, []
        for name in (f for f in files(ORACLE) if f.endswith('.webp')):
            cat, (max_ok, mean_ok) = category(name)
            row = report.setdefault(cat, {'files': 0, 'identical': 0, 'max': 0, 'mean': 0.0, 'limit': (max_ok, mean_ok)})
            row['files'] += 1
            ours, ref = (self.out / name).read_bytes(), (ORACLE / name).read_bytes()
            if ours == ref:
                row['identical'] += 1
                continue
            if Image is None:
                failures.append(f'{name}: differs (Pillow not installed to compare pixels)')
                continue
            with Image.open(self.out / name) as a, Image.open(ORACLE / name) as b:
                if a.size != b.size or a.mode != b.mode:
                    failures.append(f'{name}: {a.mode} {a.size} vs {b.mode} {b.size}')
                    continue
                diff = ImageChops.difference(a.convert('RGBA'), b.convert('RGBA'))
                hist = diff.histogram()
                worst = max(hi for _, hi in diff.getextrema())
                mean = sum((i % 256) * n for i, n in enumerate(hist)) / (len(hist) // 256 * a.size[0] * a.size[1])
            row['max'] = max(row['max'], worst)
            row['mean'] = max(row['mean'], mean)
            if worst > max_ok or mean > mean_ok:
                failures.append(f'{name}: max {worst} mean {mean:.4f} (allowed {max_ok} / {mean_ok})')
        print(f'\ncompanion stage: {self.seconds:.1f} s')
        for cat, row in sorted(report.items()):
            print(f"  {cat:14} {row['files']:5} files, {row['identical']:5} byte-identical, max abs {row['max']}, "
                  f"worst mean abs {row['mean']:.4f} (allowed {row['limit'][0]} / {row['limit'][1]})")
        self.assertFalse(failures, '\n'.join(failures[:20]))

    def test_second_run_byte_identical(self):
        again = self.tmp / 'again'
        r = subprocess.run([build(), 'companion', '--romfs', ROMFS, '--out', again], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-2000:])
        self.assertEqual(files(again), files(self.out))
        for name in files(self.out):
            self.assertEqual((again / name).read_bytes(), (self.out / name).read_bytes(), name)

    def test_cancel(self):
        out = self.tmp / 'cancelled'
        p = subprocess.Popen([build(), 'companion', '--romfs', ROMFS, '--out', out], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        for line in p.stderr:   # progress lines; cancel once the image phase runs
            if '[companion]' in line and 'images' in line:
                p.send_signal(signal.SIGINT)
                break
        stdout, _ = p.communicate(timeout=120)
        self.assertEqual(p.returncode, 3, stdout)
        self.assertEqual(json.loads(stdout)['code'], 'cancelled')

    @unittest.skipUnless(sys.version_info >= (3, 14), 'reference BYML reader requires Python 3.14')
    def test_every_fusible_reference_actor_has_its_name(self):
        sys.path.insert(0, str(GAME / 'scripts'))
        try:
            generator = _module('make_companion_assets', GAME / 'scripts/make_companion_assets.py')
        finally:
            sys.path.pop(0)
        reader = generator.RomFS(ROMFS)
        try:
            rows = generator.rsdb(reader, 'AttachmentActorInfo')
        finally:
            os.close(reader.fd)
        # Chemical-only entries also describe non-fusible constructions. Damage/multiplier entries are fuse materials.
        actors = {row['__RowId'] for row in rows if 'AttachmentAdditionalDamage' in row or 'AttachmentMulValueArrow' in row}
        self.assertTrue(actors)
        for lang in json.loads((self.out / 'index.json').read_text())['languages']:
            items = json.loads((self.out / 'text' / f'{lang}.json').read_text())['items']
            missing = [actor for actor in sorted(actors) if not items.get(actor, {}).get('name')
                       or ('_' in actor and items[actor]['name'] == actor)]
            self.assertFalse(missing, f'{lang}: {missing}')
        print(f'\n{len(rows)} attachment metadata rows, {len(actors)} fusible actors named in every language')

    @unittest.skipUnless(SAVES, 'TOTK_COMPANION_SAVES not set')
    def test_every_save_pouch_item_has_its_name(self):
        actors, fused, saves = {}, set(), set()
        for root in SAVES:
            for path, data in save_blobs(root):
                digest = hashlib.sha256(data).digest()
                if digest in saves: continue
                saves.add(digest)
                for cat, names in save_pouch(path, data).items():
                    for name in names:
                        actors.setdefault(name, (cat, path))
                        if cat.endswith('.Combined'): fused.add(name)
        self.assertTrue(actors, f'no progress.sav under {SAVES}')
        index = json.loads((self.out / 'index.json').read_text())
        missing = []
        for lang in index['languages']:
            items = json.loads((self.out / 'text' / f'{lang}.json').read_text())['items']
            missing += [f'{lang} {cat} {actor} ({path})' for actor, (cat, path) in sorted(actors.items())
                        if not items.get(actor, {}).get('name') or ('_' in actor and items[actor]['name'] == actor)]
        icons, aliases = set(index['icons']), index['iconAliases']
        no_icon = [actor for actor in sorted(actors) if actor not in fused and actor not in icons and aliases.get(actor) not in icons]
        print(f'\n{len(saves)} saves, {len(actors)} pouch/fuse actors ({len(fused)} fuse actors), '
              f'{len(missing)} names missing; pouch items without an icon: {no_icon}')
        self.assertFalse(missing, '\n'.join(missing[:20]))


if __name__ == '__main__':
    unittest.main()

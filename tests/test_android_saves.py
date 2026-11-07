"""Save sets of the Android app (android/java/.../setup/SaveManager.java) on a host JVM: save-root detection on real dump
layouts, rejection of unsafe dumps, the export/import round trip, slot dates from caption.sav, backups and their rotation,
the atomic replace under the game lock, the save folder's generation folders (a killed upload never counts), the sync
decision table, and the stable in-game snapshot.
"""
from pathlib import Path
import fcntl
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
SOURCES = [TOTK / 'android/java/com/aure/totk/setup/SaveManager.java', TOTK / 'android/java/com/aure/totk/setup/Settings.java',
           TOTK / 'android/java/com/aure/totk/setup/GameLock.java', TOTK / 'tests/saves/SavesProbe.java']
SLOT_FILES = {'progress.sav': 2307856, 'caption.sav': 21620, 'footprint.sav': 614760, 'direct_file_save_related.sav': 1152}


def jpeg_caption(seed):
    """A caption.sav-like file: a small header, then a JPEG-marked block (SOI ... EOI), then padding."""
    body = b'\xff\xd8\xff\xe0' + bytes([seed]) * 200 + b'\xff\xd9'
    return b'\x04\x03\x02\x01' + b'\0' * 60 + body + b'\0' * 40


# caption.sav's save-time entries (u32 murmur3 key hash -> s32): year, month, day, hour, minute, second.
TIME_KEYS = (0x9811a3f7, 0xdfd840d3, 0xbd46f485, 0x23f3d75e, 0x27853bf7, 0x5ee50bed)
# A real save set (the merged release set); its slots' in-game dates, as the game's load menu shows them.
SAMPLE_SAVES = Path(os.environ.get('TOTK_SAVES_SAMPLE', ROOT.parent / '.scratch/release/ThorWave/merged-saves.zip'))
SAMPLE_DATES = ['0:2026-10-27T09:47:32', '1:2026-10-25T00:42:17', '2:2026-10-25T00:45:40', '3:2026-10-27T09:46:46',
                '4:2024-06-22T14:36:30', '5:2026-11-02T14:00:56']


def timed_caption(when, other=()):
    """A caption.sav hash/value table: magic, version, data offset, then {hash, value} entries (other, then the time)."""
    entries = [*other, *zip(TIME_KEYS, when)]
    end = 0x20 + 8 * len(entries)
    head = struct.pack('<III', 0x01020304, 0x49e946, end) + b'\0' * 20
    return head + b''.join(struct.pack('<Ii', k, v) for k, v in entries) + b'\xff\xd8\xff' + b'\0' * 64 + b'\xff\xd9'


def save_set(base, slots=(0, 1), seed=1, option=False, extra=True):
    """A save root like the game writes it: slot_NN/{progress,caption,footprint,direct_file_save_related}.sav, plus
    storage/ and album/ as on the device."""
    base.mkdir(parents=True, exist_ok=True)
    for n in slots:
        slot = base / f'slot_{n:02d}'
        slot.mkdir(parents=True, exist_ok=True)
        for name, size in SLOT_FILES.items():
            data = jpeg_caption(seed + n) if name == 'caption.sav' else bytes([(seed + n) % 256]) * (size // 64)
            (slot / name).write_bytes(data)
    if option:
        (base / 'option.sav').write_bytes(bytes([seed]) * 560)
    if extra:
        (base / 'storage').mkdir(exist_ok=True)
        (base / 'storage/CacheStorageKey.dat').write_bytes(b'k' * 9)
        (base / 'album').mkdir(exist_ok=True)
        (base / 'album/000_Photo.jpg').write_bytes(b'\xff\xd8' + bytes([seed]) * 6000 + b'\xff\xd9')
    return base


def tree(base):
    return {p.relative_to(base).as_posix(): p.read_bytes() for p in sorted(base.rglob('*')) if p.is_file()}


class SavesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-saves-')
        cls.classes = Path(cls.temp.name) / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), *map(str, SOURCES)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='case-', dir=self.temp.name))

    def probe(self, *commands):
        result = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.SavesProbe'],
                                input='\n'.join('\t'.join(map(str, c)) for c in commands) + '\n',
                                capture_output=True, text=True, timeout=120, check=True)
        return result.stdout.splitlines()

    def one(self, *command):
        return self.probe(command)[0]

    # ---------- save-root detection ----------
    def test_save_root_on_real_layouts(self):
        flat = save_set(self.dir / 'flat', slots=range(6))
        save_set(self.dir / 'wrapper/totk save', slots=(0, 1, 2), option=True, extra=False)
        (self.dir / 'wrapper/readme.txt').write_text('dump notes')
        # JKSV: JKSV/<title>/<user - date>/ holding the save files; two dated dumps side by side, the newer one wins.
        jksv = self.dir / 'jksv/JKSV/The Legend of Zelda Tears of the Kingdom'
        older, newer = save_set(jksv / 'Local - 2023.05.18 @ 17.15.38', slots=(0,)), save_set(jksv / 'Local - 2023.06.01 @ 10.00.00', slots=(0, 1))
        for f in older.rglob('*.sav'):
            os.utime(f, (1_684_000_000, 1_684_000_000))
        for f in newer.rglob('*.sav'):
            os.utime(f, (1_685_600_000, 1_685_600_000))
        # yuzu: nand/user/save/0000000000000000/<uid>/0100F2C0115B6000/
        save_set(self.dir / 'yuzu/nand/user/save/0000000000000000/00000000000000000000000000000001/0100F2C0115B6000', slots=(0,))
        flat_answer, wrapper_answer, jksv_answer, yuzu_answer, empty = self.probe(
            ('select', flat), ('select', self.dir / 'wrapper'), ('select', self.dir / 'jksv'), ('select', self.dir / 'yuzu'),
            ('select', self.dir / 'wrapper/totk save/slot_00'))
        self.assertEqual(flat_answer, f'root= files={6 * 4 + 2} total={sum(p.stat().st_size for p in flat.rglob("*") if p.is_file())} manifest=false')
        self.assertTrue(wrapper_answer.startswith('root=totk save/ files=13 '), wrapper_answer)   # readme.txt outside the root is ignored
        self.assertTrue(jksv_answer.startswith('root=JKSV/The Legend of Zelda Tears of the Kingdom/Local - 2023.06.01 @ 10.00.00/ files=10 '), jksv_answer)
        self.assertTrue(yuzu_answer.startswith('root=nand/user/save/0000000000000000/00000000000000000000000000000001/0100F2C0115B6000/ files=6 '), yuzu_answer)
        self.assertEqual(empty, 'invalid no_saves')   # a single slot folder is not a save set

    def test_unsafe_dumps_are_rejected(self):
        base = save_set(self.dir / 'link', slots=(0,))
        (base / 'slot_00/escape.sav').symlink_to('/etc/passwd')
        big = save_set(self.dir / 'big', slots=(0,))
        (big / 'slot_00/movie.mp4').write_bytes(b'\0' * (2 << 20))   # an unknown file far beyond any save file
        many = save_set(self.dir / 'many', slots=(0,), extra=False)
        (many / 'album').mkdir()
        for i in range(501):
            (many / f'album/{i:03d}_Thumb.jpg').write_bytes(b'x')
        huge = save_set(self.dir / 'huge', slots=(0,), extra=False)
        with open(huge / 'slot_00/progress.sav', 'r+b') as f:
            f.truncate(33 << 20)
        total = save_set(self.dir / 'total', slots=(0,), extra=False)
        (total / 'album').mkdir()
        for i in range(5):
            with open(total / f'album/{i}.bin', 'wb') as f:
                f.truncate(30 << 20)
        self.assertEqual(self.probe(('select', self.dir / 'link'), ('select', big), ('select', many), ('select', huge), ('select', total)),
                         ['invalid symlink', 'invalid huge_file', 'invalid too_many', 'invalid huge_file', 'invalid too_big'])

        def zipped(name, entries, unix_link=None):
            path = self.dir / name
            with zipfile.ZipFile(path, 'w') as z:
                for entry, data in entries:
                    z.writestr(entry, data)
                if unix_link:
                    info = zipfile.ZipInfo(unix_link)
                    info.create_system = 3
                    info.external_attr = (0o120777 << 16)
                    z.writestr(info, '/etc/passwd')
            return path
        ok = [('save/slot_00/progress.sav', b'p' * 100)]
        traversal = zipped('traversal.zip', ok + [('save/../../evil.sav', b'x')])
        absolute = zipped('absolute.zip', ok + [('/abs/option.sav', b'x')])
        link = zipped('link.zip', ok, unix_link='save/slot_00/caption.sav')
        notzip = self.dir / 'not.zip'
        notzip.write_bytes(b'PK not really')
        answers = self.probe(*(('zip', z, self.dir / f'staged-{z.stem}') for z in (traversal, absolute, link, notzip)))
        self.assertEqual(answers, ['invalid traversal', 'invalid absolute', 'invalid symlink', 'invalid not_zip'])
        for z in (traversal, absolute, link):
            self.assertFalse((self.dir / f'staged-{z.stem}').exists(), 'nothing is staged from a refused dump')

    # ---------- export and import ----------
    def test_export_zip_round_trip_and_manifest_check(self):
        live = save_set(self.dir / 'live', slots=range(6), option=True)
        exported = self.dir / 'export.zip'
        self.assertEqual(self.one('export', live, exported), str(6 * 4 + 3))   # six slots, option.sav, storage/, album/
        with zipfile.ZipFile(exported) as z:
            names = z.namelist()
        self.assertEqual(names[-1], 'TotK Native saves/totk-native-saves.json')   # the manifest is written last
        self.assertTrue(all(n.startswith('TotK Native saves/') for n in names))
        self.assertEqual(self.one('zip', exported, self.dir / 'staged'), 'root=TotK Native saves/ files=27')
        self.assertEqual(tree(self.dir / 'staged'), tree(live))
        # an edited export no longer matches its manifest
        edited = self.dir / 'edited.zip'
        with zipfile.ZipFile(exported) as src, zipfile.ZipFile(edited, 'w') as dst:
            for info in src.infolist():
                data = src.read(info)
                if info.filename.endswith('slot_03/progress.sav'):
                    data = b'!' + data[1:]
                dst.writestr(info, data)
        self.assertEqual(self.one('zip', edited, self.dir / 'staged-edited'), 'invalid corrupt')

    def test_caption_picture_is_found(self):
        cap = self.dir / 'caption.sav'
        cap.write_bytes(jpeg_caption(7))
        empty = self.dir / 'empty.sav'
        empty.write_bytes(b'\0' * 100)
        self.assertEqual(self.probe(('jpeg', cap), ('jpeg', empty)), ['64 206', 'none'])

    def test_slot_date_is_the_in_game_save_time(self):
        cap = self.dir / 'caption.sav'
        cap.write_bytes(timed_caption((2024, 6, 22, 14, 36, 30), other=((0xaa3cf025, 1), (0, 2))))
        bad = self.dir / 'bad.sav'
        bad.write_bytes(timed_caption((2024, 13, 22, 14, 36, 30)))
        short = self.dir / 'short.sav'
        short.write_bytes(timed_caption((2024, 6, 22, 14, 36, 30))[:0x30])
        self.assertEqual(self.probe(('captime', cap, 'UTC'), ('captime', cap, 'America/New_York'), ('captime', bad, 'UTC'),
                                    ('captime', short, 'UTC'), ('jpeg', cap)),
                         ['2024-06-22T14:36:30', '2024-06-22T14:36:30', 'none', 'none', f'{0x20 + 8 * 8} 69'])
        # A slot whose caption has no readable time falls back to progress.sav's modification time.
        root = save_set(self.dir / 'live', slots=(0, 1))
        (root / 'slot_01/caption.sav').write_bytes(timed_caption((2023, 5, 18, 7, 5, 9)))
        os.utime(root / 'slot_00/progress.sav', (1_700_000_000, 1_700_000_000))
        self.assertEqual(self.one('slots', root, 'Asia/Tokyo'), '0:2023-11-15T07:13:20 1:2023-05-18T07:05:09')

    def test_slot_dates_of_a_real_save_set(self):
        if not SAMPLE_SAVES.is_file():
            self.skipTest(f'no sample save set at {SAMPLE_SAVES}')
        with zipfile.ZipFile(SAMPLE_SAVES) as z:
            z.extractall(self.dir / 'sample')
        root = next(p.parent for p in (self.dir / 'sample').rglob('slot_00'))
        for slot in root.glob('slot_*'):   # extraction dates must not matter
            os.utime(slot / 'progress.sav', (1_000_000_000, 1_000_000_000))
        self.assertEqual(self.one('slots', root, 'Europe/Berlin').split(), SAMPLE_DATES)

    def test_account_folder_matches_the_runtime(self):
        files = self.dir / 'files'
        (files / 'user-data').mkdir(parents=True)
        lo, hi = 0x2a9432118b4b4ecbb, 0x509dc96a8360a7ea
        lo &= (1 << 64) - 1
        (files / 'user-data/account.uid').write_bytes(struct.pack('<QQ', lo, hi))
        self.assertEqual(self.one('root', files), f'{hi:016x}{lo:016x}')   # fs_host.c: "%016llx%016llx", uid[1], uid[0]
        fresh = self.dir / 'fresh'
        name = self.one('root', fresh)
        self.assertRegex(name, '^[0-9a-f]{32}$')
        self.assertEqual(len((fresh / 'user-data/account.uid').read_bytes()), 16)
        self.assertEqual(self.one('root', fresh), name, 'the identity is created once, then reused')

    # ---------- backups and the swap ----------
    def test_backup_rotation_keeps_the_newest_ten(self):
        live = save_set(self.dir / 'live', slots=(0, 1, 2))
        backups = self.dir / 'backups'
        names = self.probe(*(('backup', live, backups, 'import' if i % 2 else 'mirror', 1_700_000_000_000 + i * 60_000) for i in range(12)))
        listed = self.one('backups', backups).split()
        self.assertEqual(len(listed), 10)
        self.assertEqual([entry.split(':')[0] for entry in listed], list(reversed(names))[:10])   # newest first, the two oldest gone
        self.assertTrue(all(entry.endswith(':3') for entry in listed))
        self.assertEqual(tree(backups / names[-1] / 'save'), tree(live))
        self.assertEqual(self.one('backup', self.dir / 'nothing', backups, 'manual', 1), 'none')

    def test_replace_backs_up_and_refuses_while_the_game_runs(self):
        files = self.dir / 'files'
        root_name = self.one('root', files)
        live = save_set(files / 'user-data/save' / root_name, slots=(0,), seed=1)
        before = tree(live)
        staged = save_set(files / 'save-staging/import', slots=(0, 1, 2), seed=9)
        incoming = tree(staged)
        # The game holds an OFD write lock on state/game.lock (fcntl F_OFD_SETLKW, whole file) while it runs.
        (files / 'state').mkdir()
        fd = os.open(files / 'state/game.lock', os.O_RDWR | os.O_CREAT)
        try:
            fcntl.fcntl(fd, getattr(fcntl, 'F_OFD_SETLK', 37), struct.pack('hhqqi4x', fcntl.F_WRLCK, 0, 0, 0, 0))
            locked, refused = self.probe(('game-state', files), ('replace', files, staged, 'import', 1_700_000_000_000))
        finally:
            os.close(fd)
        self.assertEqual((locked, refused), ('GAME', 'invalid game_running'))
        self.assertEqual(tree(live), before)
        unlocked, answer = self.probe(('game-state', files), ('replace', files, staged, 'import', 1_700_000_000_000))
        self.assertEqual(unlocked, 'FREE')
        self.assertTrue(answer.startswith('ok 2023') or answer.startswith('ok 20'), answer)
        self.assertEqual(tree(live), incoming)
        self.assertEqual(tree(files / 'save-backups' / answer.split()[1] / 'save'), before)
        self.assertFalse(staged.exists())
        self.assertEqual(sorted(p.name for p in (files / 'user-data/save').iterdir()), [root_name], 'no .incoming/.ready/.old left')

    # ---------- the save folder ----------
    def test_killed_uploads_never_count_and_mixed_copies_are_refused(self):
        top = self.dir / 'TotK Native saves'
        top.mkdir()
        first, second = save_set(self.dir / 'first', slots=(0, 1), seed=1), save_set(self.dir / 'second', slots=(0, 1, 2), seed=5)
        self.assertEqual(self.one('read', top), 'none')
        self.assertEqual(self.one('push', top, first, -1), 'sync-000001')
        # killed after 3 files of the next upload, then killed while writing its manifest: the first copy still stands
        crashed = self.probe(('push', top, second, 3), ('read', top), ('push', top, second, 14), ('read', top))
        self.assertEqual(crashed, ['killed', 'gen sync-000001 10', 'killed', 'gen sync-000001 10'])
        self.assertTrue((top / 'sync-000003/totk-native-saves.json').is_file(), 'the torn manifest is really there')
        self.assertEqual(self.one('fetch', top, self.dir / 'got1'), 'ok 10')
        self.assertEqual(tree(self.dir / 'got1'), tree(first))
        # a complete upload replaces every older generation (finished or not)
        self.assertEqual(self.one('push', top, second, -1), 'sync-000004')
        self.assertEqual(sorted(p.name for p in top.iterdir()), ['sync-000004'])
        self.assertEqual(self.one('fetch', top, self.dir / 'got2'), 'ok 14')
        self.assertEqual(tree(self.dir / 'got2'), tree(second))
        # mixed: a file swapped for one of the same size after the manifest was written -> refused on fetch
        target = top / 'sync-000004/slot_02/progress.sav'
        target.write_bytes(b'?' * target.stat().st_size)
        self.assertEqual(self.one('fetch', top, self.dir / 'got3'), 'invalid incomplete')
        self.assertFalse((self.dir / 'got3').exists())
        # mixed: a listed file missing -> the folder reads as incomplete, nothing is taken from it
        (top / 'sync-000004/slot_01/footprint.sav').unlink()
        self.assertEqual(self.one('read', top), 'incomplete sync-000004/slot_01/footprint.sav')

    def test_sync_decision_table(self):
        a, b, c = (save_set(self.dir / n, slots=(0, 1), seed=s) for n, s in (('a', 1), ('b', 2), ('c', 3)))
        empty = self.dir / 'empty'
        empty.mkdir()
        cases = [  # live, folder, last -> decision
            (a, a, a, 'NONE'), (a, a, '-', 'NONE'), ('-', '-', '-', 'NONE'),
            (a, '-', '-', 'PUSH'), (a, empty, b, 'PUSH'),            # empty folder: upload (never deletes the device's saves)
            (empty, a, '-', 'PULL'), (empty, a, b, 'PULL'),          # empty device: download
            (a, b, '-', 'CONFLICT'),                                 # first sync, different saves on both sides
            (a, b, a, 'PULL'),                                       # only the folder changed
            (b, a, a, 'PUSH'),                                       # only the device changed
            (b, c, a, 'CONFLICT'),                                   # both changed
            (b, b, a, 'NONE'),                                       # both changed the same way
        ]
        answers = self.probe(*(('decide', live, folder, last) for live, folder, last, _ in cases))
        self.assertEqual(answers, [want for *_, want in cases])

    def test_stable_snapshot_needs_an_unchanged_generation_and_no_save_in_progress(self):
        root = save_set(self.dir / 'live', slots=(0, 1))
        answers = self.probe(
            ('stable', root, self.dir / 's1', 'false', '7'),                 # quiet: a snapshot
            ('stable', root, self.dir / 's2', 'true', '7'),                  # a save in progress before the copy
            ('stable', root, self.dir / 's3', 'false,true', '7'),            # a save started during the copy
            ('stable', root, self.dir / 's4', 'false', '7,8'),               # a save finished during the copy
        )
        self.assertEqual(answers, ['ok 10', 'unstable', 'unstable', 'unstable'])
        self.assertEqual(tree(self.dir / 's1'), tree(root))


if __name__ == '__main__':
    unittest.main()

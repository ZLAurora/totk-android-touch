"""Direct file access in the Android app (android/java/.../setup/StoragePaths.java and SaveManager.FileStore) on a host
JVM: which kind of game folder a path is, which picked folders may hold the game data, how picked documents become real
paths (Azahar's getNativePath: primary, Documents, Downloads, removable volumes by UUID), where an install made before
private storage starts out, when Home offers the move, when a setup input goes by path instead of a descriptor, the
save folder written through plain files (each file appears whole or not at all; killed uploads never count) and the
game data size shown on Home (DataSize.java). The runtime's half of the rules (TOTK_DATA_DIR) is tests/test_android_storage.py.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test_android_saves import save_set, tree

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk/setup'
SOURCES = [JAVA / 'StoragePaths.java', JAVA / 'SaveManager.java', JAVA / 'GameLock.java', JAVA / 'Settings.java', JAVA / 'DataSize.java',
           ROOT / 'games/totk/tests/storage/DirectAccessProbe.java']
PKG = 'com.aure.totk'
PRIVATE_FILES = f'/data/user/0/{PKG}/files'
PRIVATE_GAME = PRIVATE_FILES + '/game'
EXT = 'com.android.externalstorage.documents'
DL = 'com.android.providers.downloads.documents'


class DirectAccessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-direct-')
        cls.classes = Path(cls.temp.name) / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), *map(str, SOURCES)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='case-', dir=self.temp.name))

    def probe(self, *commands):
        result = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.DirectAccessProbe'],
                                input='\n'.join('\t'.join(map(str, c)) for c in commands) + '\n',
                                capture_output=True, text=True, timeout=120, check=True)
        return result.stdout.splitlines()

    # ---------- game folder kinds and picked folders ----------
    def test_kinds(self):
        self.assertEqual(self.probe(
            ('kind', PRIVATE_GAME, PRIVATE_GAME, PKG),
            ('kind', PRIVATE_GAME + '/', PRIVATE_GAME, PKG),
            ('kind', f'/storage/emulated/0/Android/data/{PKG}/files', PRIVATE_GAME, PKG),
            ('kind', f'/storage/AAAA-AAAA/Android/data/{PKG}/files', PRIVATE_GAME, PKG),
            ('kind', '/storage/AAAA-AAAA/TotK Native', PRIVATE_GAME, PKG),
            ('kind', '/storage/AAAA-AAAA/Android/data/com.bernalr.totk/files', PRIVATE_GAME, PKG),   # earlier test builds' package: not this app's folder
        ), ['PRIVATE', 'PRIVATE', 'APP', 'APP', 'FOLDER', 'FOLDER'])

    def test_picked_folder_rules(self):
        root = '/storage'
        self.assertEqual(self.probe(
            ('problem', '/storage/emulated/0/Games/TotK Native', root, PRIVATE_FILES),
            ('problem', '/storage/AAAA-AAAA/TotK Native', root, PRIVATE_FILES),
            ('problem', '/storage/AAAA-AAAA', root, PRIVATE_FILES),
            ('problem', '/storage/emulated/0', root, PRIVATE_FILES),
            ('problem', '/storage/emulated/0/Android/data/x', root, PRIVATE_FILES),
            ('problem', '/storage/AAAA-AAAA/android/obb', root, PRIVATE_FILES),
            ('problem', '/storage/emulated/0/Games/../Android', root, PRIVATE_FILES),
            ('problem', '/storage/emulated/0/Games//x', root, PRIVATE_FILES),
            ('problem', '/storage/self/primary/Games', root, PRIVATE_FILES),
            ('problem', '/storage/emulated/x/Games', root, PRIVATE_FILES),
            ('problem', '/mnt/media_rw/AAAA-AAAA/TotK Native', root, PRIVATE_FILES),
            ('problem', PRIVATE_GAME, root, PRIVATE_FILES),
            ('problem', 'relative/folder', root, PRIVATE_FILES),
            ('problem', '-', root, PRIVATE_FILES),
        ), ['ok', 'ok', 'volume_root', 'volume_root', 'android_folder', 'android_folder', 'not_a_path', 'not_a_path',
            'not_shared', 'not_shared', 'not_shared', 'private', 'not_a_path', 'not_a_path'])

    def test_folder_target(self):
        self.assertEqual(self.probe(
            ('target', '/storage/AAAA-AAAA', 0),             # the card's top level with other files: a folder of our own
            ('target', '/storage/AAAA-AAAA/Games', 0),
            ('target', '/storage/AAAA-AAAA/Games/', 1),      # empty, or already holding game data: used as it is
            ('target', '/storage/AAAA-AAAA/TotK Native', 0),  # already named for us
        ), ['/storage/AAAA-AAAA/TotK Native', '/storage/AAAA-AAAA/Games/TotK Native', '/storage/AAAA-AAAA/Games',
            '/storage/AAAA-AAAA/TotK Native'])

    # ---------- picked documents -> real paths ----------
    def test_document_paths(self):
        primary = '/storage/emulated/0'
        volumes = 'AAAA-AAAA=/storage/AAAA-AAAA,1A2B-3C4D=/storage/1A2B-3C4D'
        self.assertEqual(self.probe(
            ('doc', EXT, 'primary:Download/totk-dump/romfs.bin', primary, volumes),
            ('doc', EXT, 'primary:', primary, volumes),                       # the volume's top level
            ('doc', EXT, 'AAAA-AAAA:totk/romfs.bin', primary, volumes),
            ('doc', EXT, '1a2b-3c4d:Games', primary, volumes),                # uuid case as SAF gives it
            ('doc', EXT, 'home:saves', primary, volumes),                     # the Documents root
            ('doc', EXT, 'FFFF-0000:x', primary, volumes),                    # unknown volume (USB stick gone)
            ('doc', EXT, 'primary:a/../../etc', primary, volumes),
            ('doc', EXT, 'nocolon', primary, volumes),
            ('doc', DL, 'raw:/storage/emulated/0/Download/keys/prod.keys', primary, volumes),
            ('doc', DL, 'msf:1000000123', primary, volumes),                  # MediaStore lookup (mediaStorePath), never Download/<name>
            ('doc', DL, '42', primary, volumes),
            ('doc', 'com.android.providers.media.documents', 'document:12', primary, volumes),
            ('doc', 'com.google.android.apps.docs.storage', 'acc=1;doc=2', primary, volumes),
            ('ids', 'msf:1000000123'), ('ids', '42'), ('ids', 'msf:'), ('ids', 'msf:12a'), ('ids', 'raw:/x'),
            ('ids', '99999999999999999999'), ('ids', '-'), ('ids', 'downloads'),
        ), ['/storage/emulated/0/Download/totk-dump/romfs.bin', '/storage/emulated/0', '/storage/AAAA-AAAA/totk/romfs.bin',
            '/storage/1A2B-3C4D/Games', '/storage/emulated/0/Documents/saves', '-', '-', '-',
            '/storage/emulated/0/Download/keys/prod.keys', '-', '-', '-', '-',
            '1000000123', '42', '-1', '-1', '-1', '-1', '-1', '-1'])

    def test_downloads_document_through_media_store(self):
        """A file picked under Downloads by its msf:/numeric id: the MediaStore row with that id gives _data (else volume +
        RELATIVE_PATH + DISPLAY_NAME), and the path is used only when it is the very file the picker's descriptor is open
        on ({st_dev, st_ino}). A same-named file of the same size elsewhere, a stale row, a folder, a descriptor that could
        not be opened or a path that can no longer be read all keep the descriptor."""
        primary, card = self.dir / 'emulated/0', self.dir / '1A2B-3C4D'
        real = primary / 'Download/totk-dump/romfs.bin'
        decoy = primary / 'Download/romfs.bin'                  # Download/<display name>: same name, same size
        copy = primary / 'Download/other/romfs.bin'             # same name, same size, same bytes
        sd = card / 'Download/dump/romfs.bin'
        for path in (real, decoy, copy, sd):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'\0' * 100)
        folder = primary / 'Download/folder.bin'
        folder.mkdir()
        volumes = f'1A2B-3C4D={card}'
        p, ep = str(primary), 'external_primary'
        self.assertEqual(self.probe(
            ('media', real, ep, 'Download/totk-dump/', 'romfs.bin', p, volumes, real),
            ('media', '-', ep, 'Download/totk-dump/', 'romfs.bin', p, volumes, real),        # no _data: nested folder
            ('media', primary / 'gone.bin', ep, 'Download/totk-dump', 'romfs.bin', p, volumes, real),   # stale _data
            ('media', '-', '1a2b-3c4d', 'Download/dump/', 'romfs.bin', p, volumes, sd),      # on the SD card
            ('media', decoy, '-', '-', '-', p, volumes, real),                               # duplicate name + size
            ('media', copy, ep, 'Download/other/', 'romfs.bin', p, volumes, real),           # identical copy elsewhere
            ('media', '-', ep, 'Download/', 'romfs.bin', p, volumes, real),                  # the old Download/<name> guess
            ('media', real, ep, 'Download/totk-dump/', 'romfs.bin', p, volumes, '-'),        # descriptor not opened
            ('media', folder, '-', '-', '-', p, volumes, real),                              # not a regular file
            ('media', folder, '-', '-', '-', p, volumes, folder),
            ('media', '-', ep, 'Download/../Download/totk-dump/', 'romfs.bin', p, volumes, real),
            ('media', '-', ep, 'Download/', 'totk-dump/romfs.bin', p, volumes, real),
            ('media', '-', 'ffff-0000', 'Download/dump/', 'romfs.bin', p, volumes, sd),      # volume not mounted
            ('media', 'Download/totk-dump/romfs.bin', '-', '-', '-', p, volumes, real),      # relative _data
            ('media', '-', '-', '-', '-', p, volumes, real),                                 # unknown id: an empty row
        ), [str(real), str(real), str(real), str(sd), '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-'])

    @unittest.skipIf(hasattr(os, 'geteuid') and os.geteuid() == 0, 'root ignores folder permissions')
    def test_downloads_document_access_revoked(self):
        """The real path can no longer be reached (All files access revoked meanwhile): the descriptor is kept."""
        real = self.dir / 'emulated/0/Download/totk-dump/romfs.bin'
        real.parent.mkdir(parents=True)
        real.write_bytes(b'\0' * 100)
        # The picker's descriptor stays open on the file (its identity is taken while the folder can still be reached).
        st = os.stat(real)
        picked = f'{st.st_dev}:{st.st_ino}'
        row = ('media', real, 'external_primary', 'Download/totk-dump/', 'romfs.bin', self.dir / 'emulated/0', '-', picked)
        self.assertEqual(self.probe(row), [str(real)])
        real.parent.chmod(0)
        try:
            out = self.probe(row)
        finally:
            real.parent.chmod(0o755)
        self.assertEqual(out, ['-'])

    # ---------- the game data size on Home ----------
    def test_data_size(self):
        """DataSize walks the data parts (links not followed, other files ignored), walks again when the install
        changes, and a walk that fails or overruns its time reports unknown (-2) instead of measuring (-1) forever."""
        game = self.dir / 'game'
        for rel, size in (('rewritten/main', 10), ('game-data/romfs.bin', 1000), ('companion/a/b/c.webp', 7),
                          ('shaders/seed.pack', 30), ('extracted/x', 3), ('.import/staging', 5000), ('install.json', 2)):
            (game / rel).parent.mkdir(parents=True, exist_ok=True)
            (game / rel).write_bytes(b'\0' * size)
        (game / 'companion/link').symlink_to(game / 'game-data/romfs.bin')
        self.assertEqual(self.probe(('dsize', game), ('dsize', game)), ['1050 1', '1050 1'])   # measured once
        # A companion rebuild replaces its folder and rewrites install.json: walked again, new size.
        shutil.rmtree(game / 'companion')
        (game / 'companion').mkdir()
        (game / 'companion/new.webp').write_bytes(b'\0' * 20)
        (game / 'install.json').write_bytes(b'{"x":1}')
        self.assertEqual(self.probe(('dsize', game), ('dsize', game)), ['1063 1', '1063 1'])
        # In one process: the install changes between two looks.
        out = self.probe(('dsize', game), ('dinvalidate',), ('dsize', game))
        self.assertEqual(out, ['1063 1', 'ok', '1063 2'])
        # Past its time: unknown, never measuring; it stays unknown (no busy retry) until the install changes.
        self.assertEqual(self.probe(('dtimeout', -1), ('dsize', game), ('dtimeout', 30000), ('dsize', game),
                                    ('dinvalidate',), ('dsize', game)), ['ok', '-2 1', 'ok', '-2 1', 'ok', '1063 2'])
        self.assertEqual(self.probe(('dsize', self.dir / 'nothing')), ['0 1'])

    # ---------- the update that introduced private storage ----------
    def test_migration_decision(self):
        legacy = f'/storage/emulated/0/Android/data/{PKG}/files'
        self.assertEqual(self.probe(
            ('initial', 1, 0, legacy),     # an existing install stays where it is, still playable, until it is moved
            ('initial', 0, 0, legacy),     # a fresh start uses private storage
            ('initial', 1, 1, legacy),     # private storage already holds one (an earlier move): it wins
            ('initial', 0, 1, legacy),
            ('initial', 1, 0, '-'),
            ('pending', legacy, legacy, 1, 0),
            ('pending', legacy + '/', legacy, 1, 0),
            ('pending', legacy, legacy, 0, 0),             # nothing playable to move
            ('pending', legacy, legacy, 1, 1),             # "Not now" is remembered
            ('pending', PRIVATE_GAME, legacy, 1, 0),       # already moved
            ('pending', f'/storage/AAAA-AAAA/Android/data/{PKG}/files', legacy, 1, 0),   # on the card by choice
            ('pending', legacy, '-', 1, 0),
        ), [legacy, 'private', 'private', 'private', 'private', 'true', 'true', 'false', 'false', 'false', 'false', 'false'])

    # ---------- setup inputs: path or descriptor ----------
    def test_input_by_path_or_descriptor(self):
        p = '/storage/emulated/0/Download/totk-dump/romfs.bin'
        self.assertEqual(self.probe(
            ('direct', 1, p, 1, 17052625232, 17052625232),
            ('direct', 1, p, 1, 17052625232, -1),     # the picker reported no size
            ('direct', 0, p, 1, 17052625232, 17052625232),   # no All files access: the picker's descriptor
            ('direct', 1, '-', 1, 5, 5),               # no real path (another provider)
            ('direct', 1, p, 0, -1, 5),                # not readable (revoked, card gone)
            ('direct', 1, p, 1, 4, 5),                 # not the file that was picked
            ('direct', 1, 'romfs.bin', 1, 5, 5),
        ), ['true', 'true', 'false', 'false', 'false', 'false', 'false'])

    # ---------- the save folder through plain files ----------
    def test_save_folder_through_files(self):
        live = save_set(self.dir / 'live', slots=(0, 1), option=True)
        top = self.dir / 'folder/TotK Native saves'; top.mkdir(parents=True)
        first, read1 = self.probe(('fspush', top, live), ('fsread', top))
        self.assertEqual(first, 'sync-000001')
        self.assertEqual(read1, f'gen sync-000001 {len(tree(live))}')
        # A killed upload: a later generation with a hidden partial file and no manifest never counts.
        torn = top / 'sync-000002/slot_00'; torn.mkdir(parents=True)
        (torn / '.progress.sav.partial').write_bytes(b'half')
        self.assertEqual(self.probe(('fsread', top))[0], f'gen sync-000001 {len(tree(live))}')
        (live / 'slot_00/progress.sav').write_bytes(b'newer save')
        second, read2, fetched = self.probe(('fspush', top, live), ('fsread', top), ('fsfetch', top, self.dir / 'staged'))
        self.assertEqual(second, 'sync-000003')
        self.assertEqual(read2, f'gen sync-000003 {len(tree(live))}')
        self.assertEqual(fetched, f'ok {len(tree(live))}')
        self.assertEqual(tree(self.dir / 'staged'), tree(live))
        self.assertEqual(sorted(p.name for p in top.iterdir()), ['sync-000003'])   # older generations removed after the push
        self.assertFalse([p for p in top.rglob('*') if p.name.endswith('.partial')])
        # A file changed in place behind the manifest's back: refused on fetch.
        victim = top / 'sync-000003/option.sav'
        victim.write_bytes(bytes(len(victim.read_bytes())))
        self.assertTrue(self.probe(('fsfetch', top, self.dir / 'staged2'))[0].startswith('invalid'))

    def test_import_candidates_from_a_folder(self):
        base = save_set(self.dir / 'dump/totk save', slots=(0,))
        (base / 'slot_00/escape.sav').symlink_to('/etc/passwd')
        listing = self.probe(('candidates', self.dir / 'dump'))[0].split('|')
        self.assertIn('totk save/slot_00/escape.sav 0 true', listing)
        self.assertIn(f'totk save/slot_00/progress.sav {(base / "slot_00/progress.sav").stat().st_size} false', listing)
        self.assertEqual(len(listing), len([p for p in (self.dir / 'dump').rglob('*') if p.is_file() or p.is_symlink()]))


if __name__ == '__main__':
    unittest.main()

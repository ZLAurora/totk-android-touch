"""Production custom-driver import on a host JVM: selection, bounded ELF validation and atomic replacement."""
from pathlib import Path
import io
import json
import shutil
import struct
import subprocess
import tempfile
import unittest
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
JAVA = GAME / 'android/java/com/aure/totk/setup'
SOURCES = [JAVA / f'{name}.java' for name in ('DriverImport', 'SaveManager', 'Settings', 'GameLock')]
SOURCES.append(GAME / 'tests/drivers/DriverProbe.java')
ELF = bytearray(b'\x7fELF\x02\x01' + b'\0' * 58)
ELF[18] = 183
ELF = bytes(ELF) + b'driver payload' * 200


def archive(entries, compression=zipfile.ZIP_DEFLATED):
    output = io.BytesIO()
    with warnings.catch_warnings(), zipfile.ZipFile(output, 'w', compression=compression) as z:
        warnings.simplefilter('ignore', UserWarning)
        for name, value in entries:
            z.writestr(name, value)
    return output.getvalue()


def meta(library='driver.so', **values):
    return json.dumps(dict(libraryName=library, **values))


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-drivers-')
        cls.classes = Path(cls.temp.name) / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), *map(str, SOURCES)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix='case-', dir=self.temp.name))
        self.target = self.directory / 'driver.so'
        self.label = self.directory / 'driverName'
        self.target.write_bytes(b'previous driver')
        self.label.write_text('previous name')

    def pick(self, data, filename='picked.zip', failure=None):
        source = self.directory / filename
        source.write_bytes(data)
        return self.pick_file(source, failure)

    def pick_file(self, source, failure=None):
        command = [self.java, '-Xmx32m', '-cp', str(self.classes), 'com.aure.totk.setup.DriverProbe', str(source), str(self.target)]
        if failure:
            command.append(failure)
        result = subprocess.run(command, capture_output=True, text=True, check=True, timeout=30)
        self.assertFalse((self.directory / 'driver.so.tmp').exists())
        self.assertFalse((self.directory / 'archive.tmp').exists())
        return result.stdout.strip()

    def unchanged(self):
        self.assertEqual(self.target.read_bytes(), b'previous driver')
        self.assertEqual(self.label.read_text(), 'previous name')

    def test_metadata_selects_nested_library_among_several(self):
        data = archive([('other.so', b'not a driver'), ('meta.json', meta('lib/vulkan.so', name='Mesa Turnip', driverVersion='Vulkan 1.4')),
                        ('lib/vulkan.so', ELF), ('../../not-extracted.txt', b'ignore')])
        self.assertEqual(self.pick(data, 'not-a-zip-extension.bin'), 'ok Mesa Turnip · Vulkan 1.4')
        self.assertEqual(self.target.read_bytes(), ELF)
        self.assertEqual(self.label.read_text(), 'Mesa Turnip · Vulkan 1.4')
        self.assertEqual({p.name for p in self.directory.iterdir()}, {'driver.so', 'driverName', 'not-a-zip-extension.bin'})

    def test_single_library_without_metadata(self):
        self.assertEqual(self.pick(archive([('readme.txt', 'text'), ('nested/custom.so', ELF)])), 'ok picked.zip')
        self.assertEqual(self.target.read_bytes(), ELF)

    def test_metadata_name_without_version_and_filename_fallback(self):
        for values, expected in (({'name': 'Turnip'}, 'Turnip'), ({'driverVersion': '1.4'}, 'picked.zip')):
            with self.subTest(values=values):
                self.assertEqual(self.pick(archive([('meta.json', meta(**values)), ('driver.so', ELF)])), 'ok ' + expected)

    def test_ambiguous_missing_and_malformed_metadata_are_refused(self):
        cases = [[], [('a.so', ELF), ('b.so', ELF)], [('text.txt', 'no driver')],
                 [('meta.json', meta('missing.so')), ('driver.so', ELF)],
                 [('meta.json', '{}'), ('driver.so', ELF)], [('meta.json', '{'), ('driver.so', ELF)],
                 [('meta.json', meta('lib/')), ('lib/', '')],
                 [('meta.json', '[]'), ('driver.so', ELF)],
                 [('meta.json', '{"nested":' + '[' * 40 + '0' + ']' * 40 + '}'), ('driver.so', ELF)],
                 [('driver.so', ELF), ('driver.so', ELF)]]
        for entries in cases:
            with self.subTest(entries=[name for name, _ in entries]):
                self.assertEqual(self.pick(archive(entries)), 'invalid zip')
                self.unchanged()

    def test_wrong_elf_architecture_and_class_are_refused(self):
        for offset, value in ((0, 0), (4, 1), (5, 2), (18, 62), (19, 1)):
            bad = bytearray(ELF); bad[offset] = value
            for zipped in (True, False):
                with self.subTest(offset=offset, zipped=zipped):
                    self.assertEqual(self.pick(archive([('driver.so', bad)]) if zipped else bad),
                                     'invalid zip' if zipped else 'invalid elf')
                    self.unchanged()
        self.assertEqual(self.pick(archive([('driver.so', b'not an ELF')])), 'invalid zip')
        self.unchanged()

    def test_limits_on_entries_metadata_and_declared_library_size(self):
        cases = [archive([(f'{n}.txt', '') for n in range(257)]),
                 archive([('meta.json', ' ' * (65536 + 1)), ('driver.so', ELF)])]
        declared = bytearray(archive([('driver.so', ELF)]))
        central = declared.index(b'PK\x01\x02')
        struct.pack_into('<I', declared, central + 24, (128 << 20) + 1)
        cases.append(bytes(declared))
        for data in cases:
            self.assertEqual(self.pick(data), 'invalid zip')
            self.unchanged()

    def test_nested_eocd_is_refused_before_zipfile_allocation(self):
        valid = bytearray(archive([('meta.json', meta(name='New')), ('driver.so', ELF)]))
        footer = valid.rindex(b'PK\x05\x06')
        hidden = bytearray(valid[footer:footer + 22])
        struct.pack_into('<I', hidden, 12, 0x7ffffff0)  # unchecked platform selection could request a huge CEN
        struct.pack_into('<H', hidden, 20, 7)  # deliberately does not end at EOF
        comment = bytes(hidden) + b'padding!'
        struct.pack_into('<H', valid, footer + 20, len(comment))
        crafted = bytes(valid) + comment
        self.assertEqual(self.pick(crafted, failure='directory'), 'invalid zip')
        self.unchanged()
        self.assertEqual(self.pick(crafted), 'invalid zip')
        self.unchanged()

    def test_normal_zip_comment_is_accepted(self):
        valid = bytearray(archive([('driver.so', ELF)]))
        footer = valid.rindex(b'PK\x05\x06')
        comment = b'A normal AdrenoTools archive comment.'
        struct.pack_into('<H', valid, footer + 20, len(comment))
        self.assertEqual(self.pick(bytes(valid) + comment), 'ok picked.zip')
        self.assertEqual(self.target.read_bytes(), ELF)

    def test_actual_bare_library_size_is_bounded(self):
        source = self.directory / 'large.so'
        with source.open('wb') as out:
            out.write(ELF)
            out.truncate((128 << 20) + 1)
        self.assertEqual(self.pick_file(source), 'invalid elf')
        self.unchanged()

    def test_truncated_zip_and_corrupt_selected_entry_leave_previous_driver_and_name(self):
        valid = archive([('meta.json', meta(name='New')), ('driver.so', ELF)], zipfile.ZIP_STORED)
        corrupt = bytearray(valid)
        payload = corrupt.index(ELF)
        corrupt[payload + 100] ^= 1
        for bad in (valid[:10], valid[:-12], corrupt, b'PKnot a zip'):
            with self.subTest(length=len(bad)):
                self.assertEqual(self.pick(bad), 'invalid zip')
                self.unchanged()

    def test_interrupted_source_and_failed_close_leave_previous_driver_and_name(self):
        for data in (ELF, archive([('meta.json', meta(name='New')), ('driver.so', ELF)])):
            for failure in ('interrupt', 'close'):
                with self.subTest(zipped=data.startswith(b'PK'), failure=failure):
                    self.assertTrue(self.pick(data, failure=failure).startswith('io '))
                    self.unchanged()

    def test_bare_library_still_accepted_by_content(self):
        self.assertEqual(self.pick(ELF, 'bare-with-wrong-extension.zip'), 'ok bare-with-wrong-extension.zip')
        self.assertEqual(self.target.read_bytes(), ELF)
        self.assertEqual(self.label.read_text(), 'bare-with-wrong-extension.zip')

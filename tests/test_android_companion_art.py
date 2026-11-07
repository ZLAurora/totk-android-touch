"""Second-screen art from an older generator (android/java/.../setup/CompanionArt.java): the app's generator number equals
the C and Python generators', and art whose index.json names an older generator, or an unreadable index, is not current.
InstallState separately distinguishes missing art; Home updates old-but-present art only on dual-screen devices.
"""
from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
SOURCES = [TOTK / 'android/java/com/aure/totk/setup/CompanionArt.java', TOTK / 'tests/companion/CompanionArtProbe.java']


def generator_number(path, pattern):
    match = re.search(pattern, path.read_text(encoding='utf-8'), re.M)
    if not match:
        raise AssertionError(f'no generator version in {path}')
    return int(match.group(1))


class CompanionArtTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-art-')
        cls.classes = Path(cls.temp.name) / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), *map(str, SOURCES)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, *dirs):
        out = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.CompanionArtProbe', *map(str, dirs)],
                             capture_output=True, text=True, timeout=60, check=True).stdout.splitlines()
        return int(out[0].split()[1]), out[1:]

    def art(self, name, index):
        d = Path(self.temp.name) / name
        d.mkdir()
        if index is not None:
            (d / 'index.json').write_bytes(index if isinstance(index, bytes) else index.encode())
        return d

    def test_app_generator_matches_the_generators(self):
        app, _ = self.probe()
        c = generator_number(TOTK / 'android/setup/companion/stage_companion.c', r'^#define GENERATOR_VERSION (\d+)')
        py = generator_number(TOTK / 'scripts/make_companion_assets.py', r'^GENERATOR_VERSION = (\d+)')
        self.assertEqual((app, app), (c, py))

    def test_older_or_unreadable_art_is_out_of_date(self):
        app, _ = self.probe()
        icons = ['Icon_%04d' % i for i in range(20000)]   # the real index is hundreds of KB; only its head is read
        def c_index(n):   # stage_companion.c's layout
            return '{"schema":1,"generator":%d,"gameVersion":"1.4.3","languages":["USen"],"icons":%s}' % (n, json.dumps(icons))
        def py_index(n):  # make_companion_assets.py's json.dump layout
            return json.dumps(dict(schema=1, generator=n, gameVersion='1.4.3', icons=icons), indent=1)
        cases = [
            (self.art('old', c_index(app - 1)), f'{app - 1} outdated'),
            (self.art('same', c_index(app)), f'{app} current'),
            (self.art('newer', c_index(app + 1)), f'{app + 1} current'),
            (self.art('pc', py_index(app)), f'{app} current'),
            (self.art('pc-old', py_index(app - 1)), f'{app - 1} outdated'),
            (self.art('missing', None), '-1 outdated'),
            (self.art('empty', ''), '-1 outdated'),
            (self.art('cut', '{"schema":1,"generator":'), '-1 outdated'),
            (self.art('garbage', b'\x00\xff' * 3000), '-1 outdated'),
            (self.art('not-json', 'generator: 9'), '-1 outdated'),
            (self.art('late', '{"schema":1,"icons":%s,"generator":%d}' % (json.dumps(icons), app)), '-1 outdated'),
        ]
        _, got = self.probe(*(d for d, _ in cases))
        self.assertEqual(got, [want for _, want in cases])


if __name__ == '__main__':
    unittest.main()

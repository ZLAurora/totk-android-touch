"""The production plain-text setup report on a host JVM: bounded output and allowlisted, record-safe diagnostics."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/android/java/com/aure/totk/setup/SetupDetails.java'
PROBE = ROOT / 'games/totk/tests/setup_details/SetupDetailsProbe.java'


class SetupDetailsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-setup-details-classes-')
        cls.classes = Path(cls.temp.name)
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), str(SOURCE), str(PROBE)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='totk-setup-details-')
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name)
        self.logs = self.game / '.import'
        self.logs.mkdir()

    def report(self, metadata='Result JSON: {"ok":false,"stage":"romfs","code":"internal","message":"failed"}',
               stage='romfs', excluded=()):
        output = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.SetupDetailsProbe',
                                 str(self.game), stage, *excluded], input=metadata.encode(), capture_output=True,
                                timeout=30, check=True).stdout
        self.assertLessEqual(len(output), 64 * 1024)
        return output.decode()

    def test_missing_logs_and_exact_error(self):
        report = self.report()
        self.assertIn('"code":"internal","message":"failed"', report)
        self.assertEqual(report.count('[missing]'), 2)
        self.assertIn('No game files or keys are included; nothing is sent until you choose an app.', report)
        self.assertIn('Share details before Resume to include the logs', report)

    def test_allowlist_never_reads_keys_game_files_or_other_logs(self):
        for name in ['prod.keys', 'title.keys', 'game.nsp', 'other.log', 'shaders.log']:
            (self.logs / name).write_text('PLANTED-SECRET-CONTENT')
        (self.logs / 'hactool-romfs.log').write_text('ROMFS-LOG')
        (self.logs / 'hactool-exefs.log').write_text('EXEFS-LOG')
        report = self.report()
        self.assertIn('ROMFS-LOG', report)
        self.assertIn('EXEFS-LOG', report)
        self.assertNotIn('PLANTED', report)
        for name in ['prod.keys', 'title.keys', 'game.nsp', 'other.log', 'shaders.log']:
            self.assertNotIn(name, report)
        shader = self.report(stage='shaders')
        self.assertIn('--- .import/shaders.log ---', shader)
        self.assertIn('PLANTED-SECRET-CONTENT', shader)

    def test_redacts_whole_text_paths_uris_and_input_names(self):
        secret = '0123456789aBcDeF' * 4
        short = 'a' * 31
        (self.logs / 'hactool-romfs.log').write_text(f'log {secret} {short}\n/data/user/0/pkg/files/key\ncontent://private/item\nSecret Game.nsp')
        report = self.report(f'App: {secret}\nResult: {secret}\n/storage/emulated/0/private',
                             excluded=['Secret Game.nsp'])
        self.assertNotRegex(report, r'[0-9a-fA-F]{32,}')
        self.assertIn(short, report)
        self.assertIn('<hex>', report)
        for value in ['Secret Game.nsp', '/data/user', '/storage/', 'content://', secret]:
            self.assertNotIn(value, report)

    def test_input_names_cannot_erase_structural_metadata(self):
        (self.logs / 'hactool-romfs.log').write_text('Could not open code')
        report = self.report('Failed stage: code\nSource romfs[1]: direct path; extension (none); bytes 16',
                             excluded=['code', 'romfs'])
        self.assertIn('Failed stage: code', report)
        self.assertIn('Source romfs[1]: direct path', report)
        self.assertIn('Could not open <input>', report)
        message = 'Could not read file named romfs at /data/user/0/pkg/files/romfs'
        result = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.SetupDetailsProbe',
                                 'scrub', '/data/user/0/pkg/files/romfs', 'romfs'], input=message,
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, 'Could not read file named <input> at <input>')

    def test_large_logs_keep_each_head_tail_and_omission_count(self):
        for name, token in [('hactool-romfs.log', 'ROMFS'), ('hactool-exefs.log', 'EXEFS'), ('shaders.log', 'SHADER')]:
            data = (token + '-BEGIN\n').encode() + b'!' * 500000 + ('\n' + token + '-END').encode()
            (self.logs / name).write_bytes(data)
        report = self.report(stage='shaders')
        for token in ['ROMFS', 'EXEFS', 'SHADER']:
            self.assertIn(token + '-BEGIN', report)
            self.assertIn(token + '-END', report)
        self.assertEqual(len(re.findall(r'\[… \d+ bytes omitted …\]', report)), 3)
        for name, section in zip(['hactool-romfs.log', 'hactool-exefs.log', 'shaders.log'], report.split('---\n')[1:]):
            marker = re.search(r'\n\[… (\d+) bytes omitted …\]\n', section)
            assert marker is not None
            kept = section[:marker.start()] + section[marker.end():]
            kept = kept.split('\n--- .import/')[0].rstrip('\n')
            self.assertEqual(len(kept.encode()) + int(marker[1]), (self.logs / name).stat().st_size)

    def test_unicode_invalid_bytes_and_large_metadata_remain_bounded(self):
        (self.logs / 'hactool-romfs.log').write_bytes(b'BEGIN\n' + b'\xff' * 200000 + b'\nEND')
        (self.logs / 'hactool-exefs.log').write_text('START\n' + '雪' * 200000 + '\nFINISH')
        report = self.report('HEADER\n' + '界' * 200000 + '\nFOOTER')
        for token in ['HEADER', 'FOOTER', 'BEGIN', 'END', 'START', 'FINISH']:
            self.assertIn(token, report)

    def test_hex_crossing_truncation_boundaries_never_leaks(self):
        (self.logs / 'hactool-romfs.log').write_text('BEGIN\n' + '1234abcdef' * 100000 + '\nEND')
        report = self.report()
        self.assertIn('BEGIN', report)
        self.assertIn('END', report)
        self.assertNotIn('1234', report)
        self.assertIn('bytes omitted', report)

    def test_private_values_crossing_both_byte_cuts_leave_no_fragments(self):
        intro = self.report().split('\n--- .import/', 1)[0]
        budget = (64 * 1024 - len(intro.encode()) - 512) // 2
        keep = (budget - 128) // 3
        size = 60000
        cuts = [keep // 2, size - (keep - keep // 2)]
        secrets = [
            ('SensitivePickedName-PrefixHiddenSuffix.nsp', True),
            ('/data/user/0/SecretAccountDirectory/PrivateLeafLocation', False),
            ('content://SecretDocumentsAuthority/ConfidentialDocumentSegment', False),
        ]
        for secret, exclude in secrets:
            with self.subTest(secret=secret):
                data = bytearray(b'!' * size)
                beginning, ending = b'HEAD-CONTEXT\n', b'\nTAIL-CONTEXT'
                data[:len(beginning)] = beginning
                data[-len(ending):] = ending
                encoded = secret.encode()
                for cut in cuts:
                    start = cut - len(encoded) // 2
                    data[start - 1] = ord('\n')
                    data[start:start + len(encoded)] = encoded
                    data[start + len(encoded)] = ord('\n')
                (self.logs / 'hactool-romfs.log').write_bytes(data)
                report = self.report(excluded=[secret] if exclude else [])
                self.assertIn('HEAD-CONTEXT', report)
                self.assertIn('TAIL-CONTEXT', report)
                self.assertIn('bytes omitted', report)
                for start in range(len(secret) - 7):
                    self.assertNotIn(secret[start:start + 8], report, 'a cut leaked part of a private value')

    def test_oversized_edge_lines_are_scrubbed_before_truncation(self):
        intro = self.report().split('\n--- .import/', 1)[0]
        budget = (64 * 1024 - len(intro.encode()) - 512) // 2
        keep = (budget - 128) // 3
        secrets = [
            'SensitivePickedName-PrefixHiddenSuffix.nsp',
            '/data/user/0/SecretAccountDirectory/PrivateLeafLocation',
            'content://SecretDocumentsAuthority/ConfidentialDocumentSegment',
        ]
        line = bytearray(b'!' * 60000)
        line[:len(b'LINE-BEGIN')] = b'LINE-BEGIN'
        line[-len(b'LINE-END'):] = b'LINE-END'
        # Put complete private values across both former raw cuts, with no newline to drop at either edge.
        for cut in [keep // 2, len(line) - (keep - keep // 2)]:
            text = (' ' + ' '.join(secrets) + ' ').encode()
            start = cut - len(text) // 2
            line[start:start + len(text)] = text
        for data in [bytes(line), bytes(line) + b'\nTAIL-CONTEXT\n', b'HEAD-CONTEXT\n' + bytes(line)]:
            with self.subTest(first_bytes=data[:20]):
                (self.logs / 'hactool-romfs.log').write_bytes(data)
                report = self.report(excluded=[secrets[0]])
                self.assertIn('LINE-BEGIN', report)
                self.assertIn('LINE-END', report)
                self.assertIn('bytes omitted', report)
                for secret in secrets:
                    for start in range(len(secret) - 7):
                        self.assertNotIn(secret[start:start + 8], report, 'an oversized line leaked a private value')

    def test_edge_lines_over_one_mib_are_left_out_whole(self):
        secret = 'SensitivePickedName-PrefixHiddenSuffix.nsp'
        line = b'!' * (2 << 20) + (' ' + secret + ' ').encode() + b'!' * (2 << 20)
        for data in [line, b'HEAD-CONTEXT\n' + line, line + b'\nTAIL-CONTEXT\n']:
            with self.subTest(first_bytes=data[:20]):
                (self.logs / 'hactool-romfs.log').write_bytes(data)
                report = self.report(excluded=[secret])
                self.assertIn('[a line longer than 1 MiB is not included]', report)
                for start in range(len(secret) - 7):
                    self.assertNotIn(secret[start:start + 8], report)
                self.assertLess(len(report.encode()), 64 * 1024 + 1)

    def test_symlinks_and_nonregular_files_are_not_opened(self):
        secret = self.game / 'prod.keys'
        secret.write_text('NEVER-READ-THIS')
        (self.logs / 'hactool-romfs.log').symlink_to(secret)
        (self.logs / 'hactool-exefs.log').mkdir()
        report = self.report()
        self.assertIn('[not included: symbolic link]', report)
        self.assertIn('[not included: not a regular log]', report)
        self.assertNotIn('NEVER-READ', report)
        shutil.rmtree(self.logs)
        elsewhere = self.game / 'elsewhere'
        elsewhere.mkdir()
        (elsewhere / 'hactool-romfs.log').write_text('NEVER-READ-THIS')
        self.logs.symlink_to(elsewhere)
        self.assertNotIn('NEVER-READ', self.report())

    def test_only_extensions_are_exposed(self):
        for name, expected in [('Secret Game.NSP', '.nsp'), ('main', '(none)'), ('private.longextension', '(none)'),
                               ('secret.nsp/path', '(none)')]:
            result = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.SetupDetailsProbe',
                                     'extension', name], capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout, expected)

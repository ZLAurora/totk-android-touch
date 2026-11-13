"""Host-only manifest, resource, signing and APK packaging tests; no game files. The Android SDK (aapt2, apksigner) is used
when installed, as an independent reader of what build_apk.py writes."""
import os
import subprocess
import hashlib
import io
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'games/totk/android/apk'), str(ROOT / 'games/totk/scripts')]
import build_apk


def chunks(data, start, end):
    while start < end:
        kind, header, size = struct.unpack_from('<HHI', data, start)
        if header < 8 or size < header or start + size > end:
            raise AssertionError('invalid resource chunk bounds')
        yield kind, start, header, size
        start += size
    if start != end:
        raise AssertionError('resource chunk alignment')


def strings(data, start):
    count, styles, flags, offset = struct.unpack_from('<4I', data, start + 8)
    assert styles == flags == 0
    result = []
    for i in range(count):
        pos = start + offset + struct.unpack_from('<I', data, start + 28 + 4 * i)[0]
        length = struct.unpack_from('<H', data, pos)[0]
        result.append(data[pos + 2:pos + 2 + 2 * length].decode('utf-16-le'))
    return result


def manifest_elements(data):
    """Decode compiled start elements and typed attributes independently of manifest_tree."""
    assert list(chunks(data, 0, len(data))) == [(3, 0, 8, len(data))]
    pool = strings(data, 8)
    elements = {}
    for kind, start, header, _ in chunks(data, 8, len(data)):
        if kind != 0x0102:
            continue
        assert header == 16
        _, tag, offset, size, count = struct.unpack_from('<IIHHH', data, start + header)
        assert (offset, size) == (20, 20)
        attrs = {}
        for i in range(count):
            ns, name, _, value_size, reserved, dtype, value = struct.unpack_from(
                '<IIIHBBI', data, start + header + offset + i * size)
            assert (value_size, reserved) == (8, 0)
            attrs[(None if ns == 0xffffffff else pool[ns], pool[name])] = (
                dtype, pool[value] if dtype == 3 else value)
        elements.setdefault(pool[tag], attrs)
        elements.setdefault('all', []).append((pool[tag], attrs))
    return elements


def resource_configs(data):
    """Decode the table independently and resolve each density's file-backed resource value."""
    top = list(chunks(data, 0, len(data)))
    assert top == [(2, 0, 12, len(data))]
    assert struct.unpack_from('<I', data, 8)[0] == 1
    children = list(chunks(data, 12, len(data)))
    assert [c[0] for c in children] == [1, 0x0200]
    paths = strings(data, children[0][1])
    _, package, header, size = children[1]
    assert header == 288
    assert struct.unpack_from('<I', data, package + 8)[0] == 0x7f
    typeoff, _, keyoff, _, typeid = struct.unpack_from('<5I', data, package + 268)
    assert typeid == 0
    assert strings(data, package + typeoff) == ['mipmap']
    assert strings(data, package + keyoff) == ['ic_launcher']
    configs = {}
    for kind, start, header, _ in chunks(data, package + header, package + size):
        if kind != 0x0201:
            continue
        tid, flags, _, count, entries = struct.unpack_from('<BBHII', data, start + 8)
        assert (tid, flags, count) == (1, 0, 1)
        assert struct.unpack_from('<I', data, start + 20)[0] == 64
        density = struct.unpack_from('<H', data, start + 34)[0]
        off = struct.unpack_from('<I', data, start + header)[0]
        entry = start + entries + off
        assert struct.unpack_from('<HHI', data, entry) == (8, 0, 0)
        value_size, reserved, dtype, index = struct.unpack_from('<HBBI', data, entry + 8)
        assert (value_size, reserved, dtype) == (8, 0, 3)
        configs[density] = paths[index]
    return configs


def sdk_tool(name):
    sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk'))
    tools = sorted(sdk.glob(f'build-tools/*/{name}'))
    return tools[-1] if tools else None


def fixture_inputs(path):
    (path / 'libtotk.so').write_bytes(b'host packaging fixture, not an Android executable')
    (path / 'classes.dex').write_bytes(b'dex packaging fixture')
    return ['--dex', str(path / 'classes.dex'), '--lib', str(path / 'libtotk.so')]


def by_name(elements, tag, name):
    return next(attrs for t, attrs in elements['all'] if t == tag and attrs.get((build_apk.ANDROID_NS, 'name'), (0, None))[1] == name)


A = build_apk.ANDROID_NS
TRUE = (0x12, 0xffffffff)
FALSE = (0x12, 0)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        source = patch.object(build_apk, 'PSO_SOURCE', Path(scratch.name) / 'pso_keys.tsv')
        source.start()
        self.addCleanup(source.stop)

    def manifest(self, **kw):
        return manifest_elements(build_apk.binary_xml(build_apk.manifest_tree(build_apk.PACKAGE, build_apk.LABEL, 1, '1', 33, 33, **kw)))

    def test_generic_app_manifest(self):
        m = self.manifest(icon=build_apk.LAUNCHER_ICON, round_icon=build_apk.LAUNCHER_ROUND, release=True)
        self.assertEqual(m['manifest'][(None, 'package')], (3, 'com.aure.totk'))
        self.assertEqual(m['uses-sdk'][(A, 'minSdkVersion')], (0x10, 33))
        self.assertEqual(sorted(a[(A, 'name')][1] for t, a in m['all'] if t == 'uses-permission'),
                         sorted(['android.permission.FOREGROUND_SERVICE', 'android.permission.FOREGROUND_SERVICE_DATA_SYNC',
                                 'android.permission.POST_NOTIFICATIONS', 'android.permission.WAKE_LOCK',
                                 'android.permission.MANAGE_EXTERNAL_STORAGE', 'android.permission.VIBRATE',
                                 'android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE',
                                 'android.permission.REQUEST_INSTALL_PACKAGES']))
        app = m['application']
        self.assertEqual(app[(A, 'label')], (3, 'TotK Native'))
        for name, value in (('hasCode', TRUE), ('debuggable', FALSE), ('extractNativeLibs', TRUE), ('allowBackup', FALSE),
                            ('hasFragileUserData', TRUE)):
            self.assertEqual(app[(A, name)], value, name)
        self.assertEqual(app[(A, 'icon')], (1, build_apk.LAUNCHER_ICON))
        setup = by_name(m, 'activity', 'com.aure.totk.setup.SetupActivity')
        self.assertEqual(setup[(A, 'exported')], TRUE)
        self.assertEqual(setup[(A, 'process')], (3, ':setup'))
        self.assertEqual(setup[(A, 'launchMode')], (0x10, 1))
        self.assertEqual(setup[(A, 'relinquishTaskIdentity')], TRUE)
        self.assertNotIn((A, 'taskAffinity'), setup)
        self.assertEqual(setup[(A, 'theme')], (1, 0x01030011))
        self.assertNotIn((A, 'screenOrientation'), setup)
        game = by_name(m, 'activity', 'com.aure.totk.TotkActivity')
        self.assertEqual(game[(A, 'exported')], FALSE)
        self.assertNotIn((A, 'process'), game)
        self.assertEqual(game[(A, 'launchMode')], (0x10, 2))
        self.assertEqual(game[(A, 'label')], app[(A, 'label')])
        self.assertNotIn((A, 'taskAffinity'), game)
        self.assertNotIn((A, 'relinquishTaskIdentity'), game)
        self.assertEqual(game[(A, 'screenOrientation')], (0x10, 6))
        cover = by_name(m, 'activity', build_apk.COVER_ACTIVITY)
        self.assertEqual(cover[(A, 'exported')], FALSE)
        self.assertEqual(cover[(A, 'taskAffinity')], (3, 'com.aure.totk.cover'))
        self.assertNotEqual(cover[(A, 'taskAffinity')][1], build_apk.PACKAGE)
        self.assertEqual(cover[(A, 'excludeFromRecents')], TRUE)
        self.assertEqual(cover[(A, 'launchMode')], (0x10, 2))
        self.assertEqual(cover[(A, 'configChanges')], (0x11, build_apk.CONFIG_CHANGES))
        self.assertNotIn((A, 'theme'), cover)
        self.assertEqual(app[(A, 'theme')], (1, build_apk.THEME_NOTITLEBAR_FULLSCREEN))
        self.assertNotIn((A, 'noHistory'), cover)
        self.assertNotIn((A, 'resizeableActivity'), cover)
        self.assertNotIn((A, 'process'), cover)
        self.assertEqual(build_apk.ATTR['taskAffinity'], 0x01010012)
        self.assertEqual(build_apk.ATTR['excludeFromRecents'], 0x01010017)
        service = by_name(m, 'service', 'com.aure.totk.setup.SetupService')
        self.assertEqual(service[(A, 'foregroundServiceType')], (0x11, 1))
        self.assertEqual(service[(A, 'exported')], FALSE)
        self.assertEqual(service[(A, 'process')], (3, ':setup'))
        control = by_name(m, 'service', 'com.aure.totk.GameControlService')
        self.assertEqual(control[(A, 'exported')], FALSE)
        self.assertNotIn((A, 'process'), control)
        self.assertNotIn((A, 'foregroundServiceType'), control)
        receiver = by_name(m, 'receiver', 'com.aure.totk.setup.UpdateReceiver')
        self.assertEqual(receiver[(A, 'exported')], FALSE)
        self.assertEqual(receiver[(A, 'process')], (3, ':setup'))
        update_entry = by_name(m, 'activity', 'com.aure.totk.setup.UpdateEntryActivity')
        self.assertEqual(update_entry[(A, 'exported')], FALSE)
        self.assertEqual(update_entry[(A, 'process')], (3, ':setup'))
        self.assertEqual(app[(A, 'usesCleartextTraffic')], TRUE)
        self.assertNotIn((A, 'process'), app)
        # One launcher entry: the setup screen; the game is started by it once install.json says playable.
        tags = [t for t, _ in m['all']]
        self.assertEqual(tags.count('intent-filter'), 1)
        self.assertEqual(m['meta-data'][(A, 'value')], (3, 'totk'))
        strings_pool = strings(build_apk.binary_xml(build_apk.manifest_tree(build_apk.PACKAGE, build_apk.LABEL, 1, '1', 33, 33)), 8)
        self.assertFalse(any('kitcatkode' in s or 'bernalr' in s or 'NativeActivity' in s for s in strings_pool))

    def test_backup_rules(self):
        """Private game data, run state and the old external app folder never go to a backup or a new device; the
        manifest points at both rule files, whose resource ids follow the launcher mipmaps."""
        for icons in (True, False):
            ids = build_apk.xml_ids(icons)
            app = self.manifest(backup=ids)['application']
            self.assertEqual(app[(A, 'dataExtractionRules')], (1, ids['data_extraction_rules']))
            self.assertEqual(app[(A, 'fullBackupContent')], (1, ids['backup_rules']))
            self.assertEqual(app[(A, 'allowBackup')], FALSE)
        self.assertEqual(build_apk.xml_ids(True)['data_extraction_rules'], 0x7f020000)
        self.assertEqual(build_apk.xml_ids(False)['backup_rules'], 0x7f010001)
        rules = {name: (path, data) for name, path, data in build_apk.backup_rules()}
        excludes = [('file', 'game'), ('file', 'state'), ('external', '.')]
        extraction = manifest_elements(rules['data_extraction_rules'][1])
        self.assertEqual([t for t, _ in extraction['all']], ['data-extraction-rules', 'cloud-backup', 'exclude', 'exclude', 'exclude',
                                                              'device-transfer', 'exclude', 'exclude', 'exclude'])
        got = [(a[(None, 'domain')][1], a[(None, 'path')][1]) for t, a in extraction['all'] if t == 'exclude']
        self.assertEqual(got, excludes * 2)
        full = manifest_elements(rules['backup_rules'][1])
        self.assertEqual([(a[(None, 'domain')][1], a[(None, 'path')][1]) for t, a in full['all'] if t == 'exclude'], excludes)
        images = [(160, 'res/mipmap-mdpi/ic_launcher.png', b'image')]
        entries = dict(build_apk.icon_resources(build_apk.PACKAGE, images, adaptive=True, xml=build_apk.backup_rules()))
        self.assertIn('res/xml/data_extraction_rules.xml', entries)
        self.assertIn('res/xml/backup_rules.xml', entries)
        alone = dict(build_apk.icon_resources(build_apk.PACKAGE, [], xml=build_apk.backup_rules()))
        self.assertEqual(sorted(alone), ['res/xml/backup_rules.xml', 'res/xml/data_extraction_rules.xml', 'resources.arsc'])

    def test_debuggable_defaults_to_dev(self):
        self.assertEqual(self.manifest()['application'][(A, 'debuggable')], TRUE)
        with patch('build_apk.build') as build:
            build_apk.main(['--dex', 'c.dex', '--lib', 'libtotk.so', '--out', 'game.apk', '--key', 'k.pem'])
            self.assertFalse(build.call_args.args[0].release)
            build_apk.main(['--dex', 'c.dex', '--lib', 'libtotk.so', '--out', 'game.apk', '--key', 'k.pem', '--release'])
            self.assertTrue(build.call_args.args[0].release)

    def test_density_table(self):
        images = [(dpi, f'res/mipmap-{q}/ic_launcher.png', b'image') for dpi, q in
                  [(160, 'mdpi'), (240, 'hdpi'), (320, 'xhdpi'), (480, 'xxhdpi'), (640, 'xxxhdpi')]]
        table = build_apk.icon_resources(build_apk.PACKAGE, images)[0][1]
        self.assertEqual(resource_configs(table), {d: p for d, p, _ in images})

    @unittest.skipUnless(sdk_tool('aapt2'), 'Android SDK aapt2 unavailable')
    def test_aapt2_reads_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            build_apk.main([*fixture_inputs(path), '--out', str(path / 'a.apk'), '--key', str(path / 'k.pem'), '--make-key', '--version-code', '7'])
            xml = subprocess.run([str(sdk_tool('aapt2')), 'dump', 'xmltree', str(path / 'a.apk'), '--file', 'AndroidManifest.xml'],
                                 check=True, capture_output=True, text=True).stdout
            for needle in ('com.aure.totk.setup.SetupActivity', 'com.aure.totk.setup.SetupService', 'com.aure.totk.TotkActivity',
                           'foregroundServiceType(0x01010599)=0x00000001', 'process(0x01010011)=":setup"', 'allowBackup(0x01010280)=false',
                           'hasFragileUserData(0x0101059a)=true', 'android.intent.category.LAUNCHER', 'POST_NOTIFICATIONS'):
                self.assertIn(needle, xml)


class SigningTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.path = Path(scratch.name)
        source = patch.object(build_apk, 'PSO_SOURCE', self.path / 'pso_keys.tsv')
        source.start()
        self.addCleanup(source.stop)
        self.inputs = fixture_inputs(self.path)

    def test_missing_key_is_never_generated_without_make_key(self):
        with self.assertRaisesRegex(SystemExit, 'does not exist'):
            build_apk.main([*self.inputs, '--out', str(self.path / 'a.apk'), '--key', str(self.path / 'release.pem')])
        self.assertFalse((self.path / 'release.pem').exists())
        self.assertFalse((self.path / 'a.apk').exists())

    def test_encrypted_pkcs12_and_ec_keys_with_password_from_environment(self):
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.serialization import pkcs12
        from cryptography.x509.oid import NameOID
        import datetime
        key = ec.generate_private_key(ec.SECP256R1())
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'test')])
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(1)
                .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30)).sign(key, hashes.SHA256()))
        p12 = self.path / 'release.p12'
        p12.write_bytes(pkcs12.serialize_key_and_certificates(b't', key, cert, None, serialization.BestAvailableEncryption(b'secret')))
        with patch.dict(os.environ, {'TEST_KEY_PASS': 'wrong'}), self.assertRaisesRegex(SystemExit, 'cannot read it'):
            build_apk.main([*self.inputs, '--out', str(self.path / 'a.apk'), '--key', str(p12), '--key-pass-env', 'TEST_KEY_PASS'])
        with patch.dict(os.environ, {'TEST_KEY_PASS': 'secret'}):
            build_apk.main([*self.inputs, '--out', str(self.path / 'a.apk'), '--key', str(p12), '--key-pass-env', 'TEST_KEY_PASS'])
        with zipfile.ZipFile(self.path / 'a.apk') as z:
            self.assertIn('META-INF/CERT.EC', z.namelist())
        self.verify(self.path / 'a.apk')

    def verify(self, apk):
        apksigner = sdk_tool('apksigner')
        if not apksigner or not shutil.which('java'):
            return
        r = subprocess.run([str(apksigner), 'verify', '--verbose', '--min-sdk-version', '21', str(apk)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('Verified using v1 scheme (JAR signing): true', r.stdout)
        for scheme in ('v2', 'v3'):
            self.assertIn(f'Verified using {scheme} scheme (APK Signature Scheme {scheme}): true', r.stdout)

    def test_signed_package_alignment_and_schemes(self):
        assets = self.path / 'assets'
        (assets / 'fonts').mkdir(parents=True)
        (assets / 'fonts/Font.ttf').write_bytes(b'\x00\x01\x00\x00' + bytes(61))   # odd length: the next entry must realign
        (assets / 'fonts/Font-OFL.txt').write_text('licence')
        (assets / 'shader_list.tsv').write_text('V\t' + 'a' * 64 + '\t{}\n')
        build_apk.main([*self.inputs, '--out', str(self.path / 'a.apk'), '--key', str(self.path / 'k.pem'), '--make-key',
                        '--assets', str(assets), '--version-code', '1'])
        apk = (self.path / 'a.apk').read_bytes()
        self.assertIn(b'APK Sig Block 42', apk)
        self.assertEqual(apk.count(struct.pack('<I', build_apk.V3_ID)), 1)
        with zipfile.ZipFile(io.BytesIO(apk)) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(z.getinfo('assets/shader_list.tsv').compress_type, zipfile.ZIP_DEFLATED)
            for name in ('assets/fonts/Font.ttf',):
                info = z.getinfo(name)
                self.assertEqual(info.compress_type, zipfile.ZIP_STORED, name)
                n, extra = struct.unpack_from('<HH', apk, info.header_offset + 26)
                self.assertEqual((info.header_offset + 30 + n + extra) % 4, 0, name)
            self.assertEqual(z.getinfo('assets/fonts/Font-OFL.txt').compress_type, zipfile.ZIP_DEFLATED)
            self.assertIn('X-Android-APK-Signed: 2, 3', z.read('META-INF/CERT.SF').decode())
        self.verify(self.path / 'a.apk')

    def test_release_build_verifies_every_scheme_not_only_v3(self):
        """apksigner alone reads only v3 on a minSdk 33 APK; build_release.verify_signatures also makes it verify v2 and v1
        (as Android 7 and 4.3 would), so a broken v2 block cannot hide behind a valid v3 one."""
        if not sdk_tool('apksigner') or not shutil.which('java'):
            self.skipTest('apksigner/java not found')
        import importlib.util
        spec = importlib.util.spec_from_file_location('release_build', ROOT / 'games/totk/android/build_release.py')
        release = importlib.util.module_from_spec(spec); spec.loader.exec_module(release)
        build_apk.main([*self.inputs, '--out', str(self.path / 'a.apk'), '--key', str(self.path / 'k.pem'), '--make-key',
                        '--min-sdk', '33', '--target-sdk', '33'])
        default = subprocess.run([str(sdk_tool('apksigner')), 'verify', '-v', str(self.path / 'a.apk')], capture_output=True, text=True)
        self.assertIn('Verified using v2 scheme (APK Signature Scheme v2): false', default.stdout)   # not read, not broken
        sdk = str(Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk')))
        self.assertEqual(release.verify_signatures(self.path / 'a.apk', self.path, sdk),
                         ['Verified using v3 scheme (APK Signature Scheme v3): true',
                          'Verified using v2 scheme (APK Signature Scheme v2): true',
                          'Verified using v1 scheme (JAR signing): true'])
        apk = bytearray((self.path / 'a.apk').read_bytes())
        v3 = apk.index(struct.pack('<I', build_apk.V3_ID))
        apk[v3 - 8 - 1] ^= 0xff   # the last byte of the v2 signer (its public key); the v3 signer stays valid
        (self.path / 'b.apk').write_bytes(apk)
        with self.assertRaisesRegex(release.BuildError, 'v2 does not verify'):
            release.verify_signatures(self.path / 'b.apk', self.path, sdk)

    def test_executables_and_translator(self):
        hactool = self.path / 'libhactool.so'
        hactool.write_bytes(b'executable fixture')
        translator = self.path / 'totk_xlate'
        translator.write_bytes(b'NativeAOT executable packaging fixture')
        build_apk.main([*self.inputs, '--lib', str(hactool), '--ondevice-xlate', str(translator), '--out', str(self.path / 'a.apk'),
                        '--key', str(self.path / 'k.pem'), '--make-key'])
        with zipfile.ZipFile(self.path / 'a.apk') as z:
            for name in ('lib/arm64-v8a/libtotk_xlate.so', 'lib/arm64-v8a/libhactool.so'):
                self.assertEqual((z.getinfo(name).external_attr >> 16) & 0o777, 0o755)
            self.assertEqual(z.read('lib/arm64-v8a/libtotk_xlate.so'), translator.read_bytes())
        (self.path / 'libtotk_xlate.so').write_bytes(b'duplicate payload')
        with self.assertRaisesRegex(SystemExit, 'duplicates --lib'):
            build_apk.main([*self.inputs, '--lib', str(self.path / 'libtotk_xlate.so'), '--ondevice-xlate', str(translator),
                            '--out', str(self.path / 'b.apk'), '--key', str(self.path / 'k.pem')])
        with self.assertRaisesRegex(SystemExit, 'requires --min-sdk 33'):
            build_apk.main([*self.inputs, '--ondevice-xlate', str(translator), '--min-sdk', '32', '--out', str(self.path / 'b.apk'),
                            '--key', str(self.path / 'k.pem')])
        self.assertFalse((self.path / 'b.apk').exists())

    def test_jar_headers_fold_long_names(self):
        for value in ('assets/licenses/' + 'x' * 90 + '.txt', 'assets/' + 'é' * 80 + '.json'):
            folded = build_apk.jar_header('Name', value)
            self.assertTrue(all(len(line.encode('utf-8')) <= 70 for line in folded.split('\r\n')))
            self.assertEqual(folded.replace('\r\n ', '').rstrip('\r\n'), 'Name: ' + value)

    def test_invalid_asset_directory_is_rejected(self):
        for assets in (self.path / 'missing-assets', self.path / 'libtotk.so'):
            with self.subTest(assets=assets), self.assertRaises(SystemExit) as error:
                build_apk.main([*self.inputs, '--assets', str(assets), '--out', str(self.path / 'invalid.apk'),
                                '--key', str(self.path / 'key.pem'), '--make-key'])
            self.assertEqual(str(error.exception), f'no such asset directory: {assets.resolve()}')
        self.assertFalse((self.path / 'invalid.apk').exists())
        self.assertFalse((self.path / 'key.pem').exists())


class PsoAssetPackagingTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.path = Path(scratch.name)
        self.source = self.path / 'pso_keys.tsv'
        source = patch.object(build_apk, 'PSO_SOURCE', self.source)
        source.start()
        self.addCleanup(source.stop)
        self.output = self.path / 'test.apk'
        self.args = [*fixture_inputs(self.path), '--out', str(self.output),
                     '--key', str(self.path / 'key.pem'), '--make-key', '--version-code', '1']
        self.words = [0] * 156
        self.words[11] = self.words[12] = 1
        vs, fs = 'a'*64 + '-0123456789ab', 'b'*64 + '-fedcba987654'
        import merge_pso_keys
        self.text = (merge_pso_keys.TSV_HEADER + (
            f'{vs}\t{fs}\t0\t' + ','.join(f'{v:x}' for v in self.words[1:]) + '\t1\t7\n').encode())
        payload = vs.encode().ljust(80, b'\0') + fs.encode().ljust(80, b'\0')
        payload += struct.pack('<158I', *self.words, 7, 1)
        # Independent fixed-format encoder, not the production binary encoder/decoder.
        self.binary = struct.pack('<8sIIII32s8s', b'TOTKPS01', 1, 792, 1, 0,
                                  hashlib.sha256(payload).digest(), bytes(8)) + payload
        (self.path / 'shader_list.tsv').write_text('V\t' + 'a'*64 + '\t{}\nV\t' + 'b'*64 + '\t{}\n')

    def test_generates_only_binary_asset_from_text(self):
        self.source.write_bytes(self.text)
        build_apk.main(self.args)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual([n for n in archive.namelist() if n.startswith('assets/')], ['assets/pso_keys.bin'])
            self.assertEqual(archive.read('assets/pso_keys.bin'), self.binary)

    def test_no_text_means_no_asset_even_with_stale_binary(self):
        self.source.with_suffix('.bin').write_bytes(self.binary)
        build_apk.main(self.args)
        with zipfile.ZipFile(self.output) as archive:
            self.assertFalse(any(n.startswith('assets/') for n in archive.namelist()))

    def test_rejects_corruption_and_absent_shader_roots(self):
        for data in (self.text[:-1], self.text.replace(b'a'*64, b'c'*64)):
            self.source.write_bytes(data)
            with self.subTest(data=data[:80]), self.assertRaises(SystemExit):
                build_apk.main(self.args)
        self.assertFalse(self.output.exists())
        self.assertFalse((self.path / 'key.pem').exists())

    def test_rejects_parallel_asset_sources(self):
        assets = self.path / 'assets'
        assets.mkdir()
        self.source.write_bytes(self.text)
        for name in ('pso_keys.bin', 'pso_keys.tsv'):
            extra = assets / name
            extra.write_bytes(self.binary if name.endswith('.bin') else self.text)
            with self.subTest(name=name), self.assertRaises(SystemExit):
                build_apk.main([*self.args, '--assets', str(assets)])
            extra.unlink()
        self.assertFalse(self.output.exists())



if __name__ == '__main__':
    unittest.main()

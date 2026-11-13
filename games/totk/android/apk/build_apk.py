#!/usr/bin/env python3
"""Pack the app (classes.dex, libtotk.so and the other native libraries, assets) into an installable APK, without aapt2
or Gradle.

This writes the binary AndroidManifest.xml and the launcher-icon resource table itself, zips them with the payload and
signs the APK with the APK Signature Schemes v3 and v2 and the v1 (JAR) scheme. `apksigner verify` checks only the
newest scheme the APK's minSdk needs (v3 for minSdk 33, so it prints v1/v2 "false" without reading them);
--min-sdk-version 24 makes it verify v2 and 18 also v1 (build_release.py verify_signatures does both). The same key must sign every later
build, or Android refuses the update (uninstalling removes the app's data, the game folder too). The key is the
caller's: a PEM file holding the private key and its certificate, or a PKCS#12 file; --key-pass-env names the
environment variable holding its password. Only --make-key (developer builds) creates a missing key.

usage: build_apk.py --dex classes.dex --lib libtotk.so [--lib more.so ...] --out totk.apk --key key.pem [--key-pass-env VAR]
       [--make-key] [--min-sdk 33] [--target-sdk 33] [--package com.aure.totk] [--label "TotK Native"] [--version-code N]
       [--version-name X] [--assets DIR] [--ondevice-xlate /path/to/android-arm64/totk_xlate] [--launcher-icon icon.png]
       [--release]
Needs the Python package 'cryptography'; --launcher-icon also requires Pillow.
"""
import argparse, base64, hashlib, io, math, os, struct, sys, time, warnings, zipfile
from pathlib import Path

ANDROID_NS = 'http://schemas.android.com/apk/res/android'
PSO_SOURCE = Path(__file__).resolve().parents[2] / 'assets/pso_keys.tsv'
PACKAGE = 'com.aure.totk'
LABEL = 'TotK Native'
# The Java classes (android/java/com/aure/totk): the launcher is the setup screen, which starts the game activity
# once <game>/install.json says playable; setup and its foreground service run in their own process so the game
# process starts without the setup library.
SETUP_ACTIVITY = 'com.aure.totk.setup.SetupActivity'
SETUP_SERVICE = 'com.aure.totk.setup.SetupService'
GAME_ACTIVITY = 'com.aure.totk.TotkActivity'
COVER_ACTIVITY = 'com.aure.totk.companion.CoverActivity'
GAME_CONTROL_SERVICE = 'com.aure.totk.GameControlService'
SETUP_PROCESS = ':setup'
# MANAGE_EXTERNAL_STORAGE: optional All files access (direct file access, as Azahar's non-Play build); the app works
# without it through the Storage Access Framework. The APK is distributed outside Google Play (see the Azahar notes in
# android/APP.md for what a Play build would have to remove).
PERMISSIONS = ('android.permission.FOREGROUND_SERVICE', 'android.permission.FOREGROUND_SERVICE_DATA_SYNC',
               'android.permission.POST_NOTIFICATIONS', 'android.permission.WAKE_LOCK',
               'android.permission.MANAGE_EXTERNAL_STORAGE', 'android.permission.VIBRATE',
               'android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE',
               'android.permission.REQUEST_INSTALL_PACKAGES')

# android.R.attr resource ids of the manifest attributes used here (checked against aapt2 link with android.jar)
ATTR = {
    'theme': 0x01010000, 'label': 0x01010001, 'icon': 0x01010002, 'name': 0x01010003, 'hasCode': 0x0101000c, 'debuggable': 0x0101000f,
    'exported': 0x01010010, 'process': 0x01010011, 'taskAffinity': 0x01010012, 'excludeFromRecents': 0x01010017,
    'launchMode': 0x0101001d, 'screenOrientation': 0x0101001e, 'configChanges': 0x0101001f,
    'value': 0x01010024, 'minSdkVersion': 0x0101020c, 'versionCode': 0x0101021b, 'versionName': 0x0101021c,
    'targetSdkVersion': 0x01010270, 'allowBackup': 0x01010280, 'isGame': 0x010103f4, 'extractNativeLibs': 0x010104ea,
    'drawable': 0x01010199, 'roundIcon': 0x0101052c, 'foregroundServiceType': 0x01010599, 'hasFragileUserData': 0x0101059a,
    'fullBackupContent': 0x010104eb, 'dataExtractionRules': 0x0101063e,
    'relinquishTaskIdentity': 0x01010476,
    'usesCleartextTraffic': 0x010104ec,
}
THEME_NOTITLEBAR_FULLSCREEN = 0x01030007   # @android:style/Theme.NoTitleBar.Fullscreen
THEME_TRANSLUCENT_FULLSCREEN = 0x01030011  # @android:style/Theme.Translucent.NoTitleBar.Fullscreen (no flash when setup forwards to the game)
SENSOR_LANDSCAPE = 6
# orientation | keyboard | keyboardHidden | navigation | screenLayout | uiMode | screenSize | smallestScreenSize | density
CONFIG_CHANGES = 0x80 | 0x10 | 0x20 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000
SINGLE_TOP, SINGLE_TASK = 1, 2
FOREGROUND_DATA_SYNC = 0x1
LAUNCHER_ICON = 0x7f010000   # @mipmap/ic_launcher, package 0x7f, type 1, entry 0
LAUNCHER_ROUND = 0x7f010001
LAUNCHER_BACKGROUND = 0x7f010002
LAUNCHER_FOREGROUND = 0x7f010003
ICON_DENSITIES = [('mdpi', 160, 48), ('hdpi', 240, 72), ('xhdpi', 320, 96),
                  ('xxhdpi', 480, 144), ('xxxhdpi', 640, 192)]
# Uncompressed in the APK: the resource table (Android 11+), images, and the fonts the app hands to native code through
# AssetManager.openFd (the setup's shared-font stage).
STORED_SUFFIXES = ('.png', '.webp', '.ttf', '.otf')
LAUNCHER_NAMES = ('ic_launcher', 'ic_launcher_round', 'ic_launcher_background', 'ic_launcher_foreground')
# Backup and device-transfer rules (res/xml): the game data in private storage (<filesDir>/game, ~19 GB, rebuilt from
# the player's dump), the run state (<filesDir>/state: locks, RomFS proofs, the Vulkan pipeline cache) and the old
# internal app folder (domain external) never leave the device. Everything else, the saves included, keeps its
# behaviour: no cloud backup (allowBackup=false), Android's device-to-device transfer as before.
BACKUP_EXCLUDES = (('file', 'game'), ('file', 'state'), ('external', '.'))
XML_NAMES = ('data_extraction_rules', 'backup_rules')


def backup_rules():
    """[(name, res path, binary XML)] for android:dataExtractionRules (API 31+) and android:fullBackupContent."""
    excludes = [('exclude', [(False, 'domain', 'str', d), (False, 'path', 'str', p)], []) for d, p in BACKUP_EXCLUDES]
    extraction = ('data-extraction-rules', [], [('cloud-backup', [], excludes), ('device-transfer', [], excludes)])
    return [('data_extraction_rules', 'res/xml/data_extraction_rules.xml', binary_xml(extraction)),
            ('backup_rules', 'res/xml/backup_rules.xml', binary_xml(('full-backup-content', [], excludes)))]


def xml_ids(with_icons):
    """Resource ids of XML_NAMES: the xml type follows mipmap when the APK has launcher icons."""
    type_id = 2 if with_icons else 1
    return {name: 0x7f000000 | type_id << 16 | i for i, name in enumerate(XML_NAMES)}

# ---------- binary XML (the format aapt writes) ----------
T_REFERENCE, T_STRING, T_INT_DEC, T_INT_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12


def manifest_tree(package, label, version_code, version_name, min_sdk, target_sdk, icon=None, *, round_icon=None, release=False,
                  backup=None):
    """(tag, [(ns?, name, kind, value)], children); kind: 'str', 'int', 'hex', 'bool', 'ref'. backup: xml_ids() (the
    dataExtractionRules / fullBackupContent resources), or None for no rules."""
    A = True   # attribute in the android namespace
    return ('manifest', [(False, 'package', 'str', package), (A, 'versionCode', 'int', version_code), (A, 'versionName', 'str', version_name)], [
        ('uses-sdk', [(A, 'minSdkVersion', 'int', min_sdk), (A, 'targetSdkVersion', 'int', target_sdk)], []),
        *[('uses-permission', [(A, 'name', 'str', p)], []) for p in PERMISSIONS],
        ('application', [(A, 'label', 'str', label), (A, 'hasCode', 'bool', True), (A, 'debuggable', 'bool', not release),
                         (A, 'extractNativeLibs', 'bool', True), (A, 'isGame', 'bool', True), (A, 'theme', 'ref', THEME_NOTITLEBAR_FULLSCREEN),
                         # Saves and setup state stay on the device: no cloud/adb backup of the app's private files, and
                         # Android asks before removing the (large, player-supplied) data on uninstall.
                         (A, 'allowBackup', 'bool', False), (A, 'hasFragileUserData', 'bool', True),
                         # Only the private, root-written update-source.txt override can use HTTP; production update
                         # URLs and every redirect are checked in Java before opening a connection.
                         (A, 'usesCleartextTraffic', 'bool', True)]
                         + ([(A, 'dataExtractionRules', 'ref', backup['data_extraction_rules']),
                             (A, 'fullBackupContent', 'ref', backup['backup_rules'])] if backup else [])
                         + ([(A, 'icon', 'ref', icon)] if icon is not None else [])
                         + ([(A, 'roundIcon', 'ref', round_icon)] if round_icon is not None else []), [
            ('activity', [(A, 'name', 'str', SETUP_ACTIVITY), (A, 'label', 'str', label), (A, 'exported', 'bool', True),
                          (A, 'process', 'str', SETUP_PROCESS), (A, 'launchMode', 'int', SINGLE_TOP), (A, 'relinquishTaskIdentity', 'bool', True),
                          (A, 'theme', 'ref', THEME_TRANSLUCENT_FULLSCREEN), (A, 'configChanges', 'hex', CONFIG_CHANGES)], [
                ('intent-filter', [], [
                    ('action', [(A, 'name', 'str', 'android.intent.action.MAIN')], []),
                    ('category', [(A, 'name', 'str', 'android.intent.category.LAUNCHER')], []),
                ]),
            ]),
            ('activity', [(A, 'name', 'str', GAME_ACTIVITY), (A, 'label', 'str', label), (A, 'exported', 'bool', False),
                          (A, 'launchMode', 'int', SINGLE_TASK), (A, 'screenOrientation', 'int', SENSOR_LANDSCAPE),
                          (A, 'configChanges', 'hex', CONFIG_CHANGES)], [
                ('meta-data', [(A, 'name', 'str', 'android.app.lib_name'), (A, 'value', 'str', 'totk')], []),
            ]),
            ('activity', [(A, 'name', 'str', COVER_ACTIVITY), (A, 'exported', 'bool', False),
                          (A, 'taskAffinity', 'str', package + '.cover'), (A, 'excludeFromRecents', 'bool', True),
                          (A, 'launchMode', 'int', SINGLE_TASK), (A, 'configChanges', 'hex', CONFIG_CHANGES)], []),
            ('activity', [(A, 'name', 'str', 'com.aure.totk.setup.UpdateEntryActivity'), (A, 'exported', 'bool', False),
                          (A, 'process', 'str', SETUP_PROCESS), (A, 'theme', 'ref', THEME_TRANSLUCENT_FULLSCREEN)], []),
            ('service', [(A, 'name', 'str', SETUP_SERVICE), (A, 'exported', 'bool', False), (A, 'process', 'str', SETUP_PROCESS),
                         (A, 'foregroundServiceType', 'hex', FOREGROUND_DATA_SYNC)], []),
            ('service', [(A, 'name', 'str', GAME_CONTROL_SERVICE), (A, 'exported', 'bool', False)], []),
            ('receiver', [(A, 'name', 'str', 'com.aure.totk.setup.UpdateReceiver'), (A, 'exported', 'bool', False),
                          (A, 'process', 'str', SETUP_PROCESS)], []),
        ]),
    ])


def binary_xml(tree):
    # strings: android attribute names first (they carry resource ids, in the resource map's order), then the rest
    attr_names, others = [], []
    def collect(node):
        tag, attrs, kids = node
        for ns, name, kind, value in attrs:
            if ns and name not in attr_names: attr_names.append(name)
            elif not ns and name not in others: others.append(name)
            if kind == 'str' and value not in others: others.append(value)
        if tag not in others: others.append(tag)
        for k in kids: collect(k)
    collect(tree)
    attr_names.sort(key=lambda n: ATTR[n])
    for extra in ('android', ANDROID_NS):
        if extra not in others: others.append(extra)
    strings = attr_names + [s for s in others if s not in attr_names]
    idx = {s: i for i, s in enumerate(strings)}

    def chunk(ctype, header, body):   # header: bytes after the 8-byte chunk header
        return struct.pack('<HHI', ctype, 8 + len(header), 8 + len(header) + len(body)) + header + body

    # string pool (UTF-16)
    data, offsets = b'', []
    for s in strings:
        offsets.append(len(data))
        u = s.encode('utf-16-le'); data += struct.pack('<H', len(s)) + u + b'\0\0'
    while len(data) % 4: data += b'\0'
    header_len = 28
    body = b''.join(struct.pack('<I', o) for o in offsets) + data
    pool = chunk(0x0001, struct.pack('<IIIII', len(strings), 0, 0, header_len + 4 * len(strings), 0), body)
    resmap = chunk(0x0180, b'', b''.join(struct.pack('<I', ATTR[n]) for n in attr_names))

    NONE = 0xffffffff
    out = []
    out.append(chunk(0x0100, struct.pack('<II', 1, NONE), struct.pack('<II', idx['android'], idx[ANDROID_NS])))
    def emit(node, line=[1]):
        tag, attrs, kids = node
        line[0] += 1
        recs = []
        for ns, name, kind, value in attrs:
            nsi = idx[ANDROID_NS] if ns else NONE
            raw = NONE
            if kind == 'str': dtype, d = T_STRING, idx[value]; raw = d
            elif kind == 'int': dtype, d = T_INT_DEC, value
            elif kind == 'hex': dtype, d = T_INT_HEX, value
            elif kind == 'bool': dtype, d = T_BOOL, 0xffffffff if value else 0
            elif kind == 'ref': dtype, d = T_REFERENCE, value
            else: raise ValueError(kind)
            key = ATTR[name] if ns else 0x7fffffff   # aapt order: by resource id, plain attributes last
            recs.append((key, struct.pack('<IIIHBBI', nsi, idx[name], raw, 8, 0, dtype, d & 0xffffffff)))
        recs.sort(key=lambda r: r[0])
        ext = struct.pack('<IIHHHHHH', NONE, idx[tag], 20, 20, len(recs), 0, 0, 0)
        out.append(chunk(0x0102, struct.pack('<II', line[0], NONE), ext + b''.join(r[1] for r in recs)))
        for k in kids: emit(k)
        out.append(chunk(0x0103, struct.pack('<II', line[0], NONE), struct.pack('<II', NONE, idx[tag])))
    emit(tree)
    out.append(chunk(0x0101, struct.pack('<II', 1, NONE), struct.pack('<II', idx['android'], idx[ANDROID_NS])))
    xml = pool + resmap + b''.join(out)
    return struct.pack('<HHI', 0x0003, 8, 8 + len(xml)) + xml


# ---------- launcher resources (the small subset of resources.arsc needed by this APK) ----------
def resource_chunk(kind, header, body):
    return struct.pack('<HHI', kind, 8 + len(header), 8 + len(header) + len(body)) + header + body


def resource_strings(strings):
    data, offsets = bytearray(), []
    for text in strings:
        offsets.append(len(data))
        encoded = text.encode('utf-16-le')
        data.extend(struct.pack('<H', len(encoded) // 2) + encoded + b'\0\0')
    data.extend(bytes(-len(data) % 4))
    return resource_chunk(1, struct.pack('<5I', len(strings), 0, 0, 28 + 4 * len(strings), 0),
                          struct.pack(f'<{len(offsets)}I', *offsets) + data)


def icon_resources(package, images, *, adaptive=False, xml=()):
    """File-backed launcher mipmaps; adaptive=True adds the round and layered icons and their anydpi-v26 adaptive XML.
    xml: [(name, res path, data)] of the 'xml' type (default configuration), after mipmap (xml_ids)."""
    names = (['ic_launcher', 'ic_launcher_round', 'ic_launcher_background', 'ic_launcher_foreground'] if adaptive else ['ic_launcher']) if images else []
    files = list(images)
    if adaptive and images:
        xml_icon = binary_xml(('adaptive-icon', [], [
            ('background', [(True, 'drawable', 'ref', LAUNCHER_BACKGROUND)], []),
            ('foreground', [(True, 'drawable', 'ref', LAUNCHER_FOREGROUND)], []),
        ]))
        files += [(0xfffe, f'res/mipmap-anydpi-v26/{name}.xml', xml_icon) for name in names[:2]]
    type_names, chunks = [], []
    if names:
        type_names.append('mipmap')
        type_id = len(type_names)
        flags = [0x500 if adaptive and i < 2 else 0x100 for i in range(len(names))]  # DENSITY | VERSION
        chunks.append(resource_chunk(0x0202, struct.pack('<BBHI', type_id, 0, 0, len(names)), struct.pack(f'<{len(flags)}I', *flags)))
        variants = {}
        for index, (density, path, _) in enumerate(files):
            sdk = 26 if path.endswith('.xml') else 0
            variants.setdefault((density, sdk), {})[names.index(Path(path).stem)] = index
        for (density, sdk), values in variants.items():
            chunks.append(type_chunk(type_id, len(names), values, density=density, sdk=sdk))
    if xml:
        type_names.append('xml')
        type_id = len(type_names)
        chunks.append(resource_chunk(0x0202, struct.pack('<BBHI', type_id, 0, 0, len(xml)), struct.pack(f'<{len(xml)}I', *([0] * len(xml)))))
        chunks.append(type_chunk(type_id, len(xml), {i: len(files) + i for i in range(len(xml))}, key_base=len(names)))
        files += [(0, path, data) for _, path, data in xml]
    keys = resource_strings(names + [name for name, _, _ in xml])
    types = resource_strings(type_names)
    # ResTable_package has a 288-byte header; string-pool offsets are relative to its start.
    name = package.encode('utf-16-le')
    if len(name) > 254:
        raise ValueError('resource package name exceeds 127 UTF-16 code units')
    header = struct.pack('<I', 0x7f) + name.ljust(256, b'\0')
    header += struct.pack('<5I', 288, len(type_names), 288 + len(types), len(names) + len(xml), 0)
    pkg = resource_chunk(0x0200, header, types + keys + b''.join(chunks))
    table = resource_chunk(2, struct.pack('<I', 1), resource_strings([p for _, p, _ in files]) + pkg)
    return [('resources.arsc', table)] + [(p, data) for _, p, data in files]


def type_chunk(type_id, count, values, *, density=0, sdk=0, key_base=0):
    """One ResTable_type: entry index -> global string index of its file path (absent entries have no value). Entry i
    is named by key key_base + i of the package's key pool (types share one pool)."""
    config = bytearray(64)
    struct.pack_into('<I', config, 0, len(config))
    struct.pack_into('<H', config, 14, density)
    struct.pack_into('<H', config, 24, sdk)
    header_size = 20 + len(config)
    header = struct.pack('<BBHII', type_id, 0, 0, count, header_size + 4 * count) + config
    offsets, entries = [], bytearray()
    for key in range(count):
        offsets.append(len(entries) if key in values else 0xffffffff)
        if key in values:
            entries.extend(struct.pack('<HHI', 8, 0, key_base + key) + struct.pack('<HBBI', 8, 0, T_STRING, values[key]))
    return resource_chunk(0x0201, header, struct.pack(f'<{len(offsets)}I', *offsets) + entries)


def launcher_override(path):
    """Decode the launcher artwork (the app's own, android/app-assets/icon); return mipmaps and the 512px store PNG."""
    try:
        from PIL import Image, ImageDraw, ImageFilter, ImageOps, ImageStat
    except ImportError:
        raise SystemExit('--launcher-icon requires Pillow in the APK builder Python environment')
    path = Path(path).expanduser().resolve()
    try:
        data = path.read_bytes()
        with warnings.catch_warnings():
            warnings.simplefilter('error', Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data)) as image:
                if image.format not in ('JPEG', 'PNG'):
                    raise ValueError('expected a JPEG or PNG image')
                if getattr(image, 'n_frames', 1) != 1:
                    raise ValueError('expected a single-frame image')
                image.load()  # Reject truncated files before creating an APK or signing key.
                source = ImageOps.exif_transpose(image).convert('RGBA')
    except (OSError, ValueError, Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
        raise SystemExit(f'--launcher-icon: {path}: {error}')
    alpha_min, alpha_max = source.getchannel('A').getextrema()
    if alpha_max == 0:
        raise SystemExit(f'--launcher-icon: {path}: image is entirely transparent')
    if alpha_min < 255:
        background = (11, 20, 22, 255)  # Dark teal lets transparent artwork/glow read naturally.
    else:
        corners = [ImageStat.Stat(source.crop((x, y, x + 1, y + 1))).mean
                   for x in (0, source.width - 1) for y in (0, source.height - 1)]
        background = tuple(round(sum(c[i] for c in corners) / len(corners)) for i in range(3)) + (255,)
    # Resize premultiplied colour and alpha together, not unassociated RGB (which causes glow fringes).
    premultiplied = source.convert('RGBa')

    def fitted(size, *, circle=False, adaptive=False):
        bound = size * (66 / 108 if adaptive else 0.94)
        scale = bound / (math.hypot(*source.size) if circle or adaptive else max(source.size))
        dimensions = (max(1, int(source.width * scale)), max(1, int(source.height * scale)))
        logo = premultiplied.resize(dimensions, Image.Resampling.LANCZOS).convert('RGBA')
        canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0) if adaptive else background)
        canvas.alpha_composite(logo, ((size - logo.width) // 2, (size - logo.height) // 2))
        if not adaptive and size <= 192:
            canvas = canvas.convert('RGB').filter(ImageFilter.UnsharpMask(radius=0.5, percent=110, threshold=3)).convert('RGBA')
        if circle:
            mask = Image.new('L', (size * 4, size * 4))
            ImageDraw.Draw(mask).ellipse((0, 0, size * 4 - 1, size * 4 - 1), fill=255)
            canvas.putalpha(mask.resize((size, size), Image.Resampling.LANCZOS))
        return canvas

    def png(image):
        out = io.BytesIO()
        image.save(out, format='PNG')
        return out.getvalue()

    images = []
    for qualifier, dpi, size in ICON_DENSITIES:
        root = f'res/mipmap-{qualifier}'
        layer_size = size * 9 // 4  # 108dp rather than the legacy icon's 48dp.
        images += [(dpi, f'{root}/ic_launcher.png', png(fitted(size))),
                   (dpi, f'{root}/ic_launcher_round.png', png(fitted(size, circle=True))),
                   (dpi, f'{root}/ic_launcher_background.png', png(Image.new('RGBA', (layer_size, layer_size), background))),
                   (dpi, f'{root}/ic_launcher_foreground.png', png(fitted(layer_size, adaptive=True)))]
    print(f'launcher-icon: {path} sha256={hashlib.sha256(data).hexdigest()}')
    return images, png(fitted(512))


def launcher_mipmaps(folder):
    """The launcher images apk/render_launcher.py rendered and committed (no Pillow needed): mipmaps and the store PNG."""
    folder = Path(folder).expanduser().resolve()
    images = []
    for qualifier, dpi, _ in ICON_DENSITIES:
        for name in LAUNCHER_NAMES:
            path = folder / f'res/mipmap-{qualifier}/{name}.png'
            if not path.is_file() or path.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
                raise SystemExit(f'--launcher-mipmaps: {path} missing or not a PNG (rerun android/apk/render_launcher.py)')
            images.append((dpi, f'res/mipmap-{qualifier}/{name}.png', path.read_bytes()))
    store = folder / 'launcher-512.png'
    return images, store.read_bytes() if store.is_file() else None


# ---------- signing key ----------
def signature_algorithm(key):
    """APK Signature Scheme algorithm id for the key: RSASSA-PKCS1-v1_5 or ECDSA (P-256), both with SHA2-256."""
    from cryptography.hazmat.primitives.asymmetric import ec, rsa
    if isinstance(key, rsa.RSAPrivateKey):
        return SIG_RSA_PKCS1_SHA256
    if isinstance(key, ec.EllipticCurvePrivateKey) and key.curve.name == 'secp256r1':
        return SIG_ECDSA_SHA256
    raise SystemExit('signing key: only RSA and EC P-256 keys are supported')


def load_key(path, password=None, *, make=False):
    """(private key, certificate) from a PEM file holding both, or a PKCS#12 file (.p12 / .pfx). With make=True a missing
    file becomes a new self-signed RSA key (developer builds only; a release key is never generated here)."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import pkcs12
    from cryptography.x509.oid import NameOID
    import datetime
    path = Path(path).expanduser()
    secret = password.encode() if password else None
    if path.is_file():
        blob = path.read_bytes()
        try:
            if path.suffix.lower() in ('.p12', '.pfx'):
                key, cert, _ = pkcs12.load_key_and_certificates(blob, secret)
            else:
                key = serialization.load_pem_private_key(blob, password=secret)
                cert = x509.load_pem_x509_certificate(blob)
        except (ValueError, TypeError) as error:   # never echo key material, only what went wrong
            raise SystemExit(f'signing key {path}: cannot read it ({type(error).__name__}: wrong password, or not a key + certificate)')
        if key is None or cert is None:
            raise SystemExit(f'signing key {path}: needs both the private key and its certificate')
        signature_algorithm(key)
        return key, cert
    if not make:
        raise SystemExit(f'signing key {path} does not exist (pass your own key; only developer builds use --make-key)')
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'TotK Native (local developer build)')])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=365 * 30)).sign(key, hashes.SHA256()))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
                     + cert.public_bytes(serialization.Encoding.PEM))
    os.chmod(path, 0o600)
    print(f'made a new signing key: {path} (keep it: later builds must be signed with it to update the app)')
    return key, cert


def sign_data(key, data):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, padding
    if signature_algorithm(key) == SIG_RSA_PKCS1_SHA256:
        return key.sign(data, padding.PKCS1v15(), hashes.SHA256())
    return key.sign(data, ec.ECDSA(hashes.SHA256()))


# ---------- v1 (JAR) signature ----------
def b64(d): return base64.b64encode(d).decode()

def jar_header(name, value):
    """Fold UTF-8 JAR headers at 70 payload bytes (72 including CRLF)."""
    lines, line, size = [], '', 0
    for char in f'{name}: {value}':
        width = len(char.encode('utf-8'))
        if size + width > 70:
            lines.append(line)
            line, size = ' ', 1
        line += char
        size += width
    return '\r\n'.join([*lines, line]) + '\r\n'



def v1_files(entries, key, cert):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.serialization import pkcs7
    mf = 'Manifest-Version: 1.0\r\nCreated-By: 1.0 (totk build_apk.py)\r\n\r\n'
    sections = []
    for name, data in entries:
        sec = jar_header('Name', name) + f'SHA-256-Digest: {b64(hashlib.sha256(data).digest())}\r\n\r\n'
        sections.append((name, sec)); mf += sec
    mfb = mf.encode()
    # X-Android-APK-Signed names the newer schemes this APK also carries, so stripping them is detected.
    sf = ('Signature-Version: 1.0\r\nCreated-By: 1.0 (totk build_apk.py)\r\n'
          f'SHA-256-Digest-Manifest: {b64(hashlib.sha256(mfb).digest())}\r\nX-Android-APK-Signed: 2, 3\r\n\r\n')
    for name, sec in sections:
        sf += jar_header('Name', name) + f'SHA-256-Digest: {b64(hashlib.sha256(sec.encode()).digest())}\r\n\r\n'
    sfb = sf.encode()
    sig = (pkcs7.PKCS7SignatureBuilder().set_data(sfb).add_signer(cert, key, hashes.SHA256())
           .sign(serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.NoAttributes]))
    block = 'META-INF/CERT.RSA' if signature_algorithm(key) == SIG_RSA_PKCS1_SHA256 else 'META-INF/CERT.EC'
    return [('META-INF/MANIFEST.MF', mfb), ('META-INF/CERT.SF', sfb), (block, sig)]


# ---------- v2 + v3 signatures (APK Signing Block) ----------
def lp(b): return struct.pack('<I', len(b)) + b
SIG_RSA_PKCS1_SHA256 = 0x0103
SIG_ECDSA_SHA256 = 0x0201
V2_ID = 0x7109871a
V3_ID = 0xf05368c0
STRIPPING_PROTECTION = 0xbeeff00d   # v2 signed attribute: "a v3 signature also exists" (value: scheme id 3)
V3_MIN_SDK, V3_MAX_SDK = 28, 0x7fffffff   # v3 is read from Android 9 (API 28)


def chunk_digest(sections):
    digests = []
    for s in sections:
        for i in range(0, len(s), 1 << 20):
            c = s[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()


def v2_sign(apk, key, cert, *, v3=True):
    """Insert the APK Signing Block with a v2 and (by default) a v3 signer over the same content digest."""
    from cryptography.hazmat.primitives import serialization
    eocd = apk.rfind(b'PK\x05\x06')
    if eocd < 0 or len(apk) - eocd != 22: raise ValueError('unexpected zip end record')
    _, cd_off = struct.unpack_from('<II', apk, eocd + 12)
    entries, cd, end = apk[:cd_off], apk[cd_off:eocd], bytearray(apk[eocd:])
    # the digest covers the end record as if the central directory started where the signing block will be
    struct.pack_into('<I', end, 16, cd_off)
    digest = chunk_digest([entries, cd, bytes(end)])
    algorithm = signature_algorithm(key)
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    spki = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    digests, certs = lp(lp(struct.pack('<I', algorithm) + lp(digest))), lp(lp(cert_der))
    def signature(signed): return lp(lp(struct.pack('<I', algorithm) + lp(sign_data(key, signed))))
    attributes = lp(lp(struct.pack('<II', STRIPPING_PROTECTION, 3))) if v3 else lp(b'')
    signed = digests + certs + attributes
    v2_value = lp(lp(lp(signed) + signature(signed) + lp(spki)))
    pairs = struct.pack('<QI', 4 + len(v2_value), V2_ID) + v2_value
    if v3:
        sdk = struct.pack('<II', V3_MIN_SDK, V3_MAX_SDK)
        signed3 = digests + certs + sdk + lp(b'')
        v3_value = lp(lp(lp(signed3) + sdk + signature(signed3) + lp(spki)))
        pairs += struct.pack('<QI', 4 + len(v3_value), V3_ID) + v3_value
    size = len(pairs) + 8 + 16
    block = struct.pack('<Q', size) + pairs + struct.pack('<Q', size) + b'APK Sig Block 42'
    struct.pack_into('<I', end, 16, cd_off + len(block))
    return entries + block + cd + bytes(end)


def pso_asset():
    """assets/pso_keys.bin from assets/pso_keys.tsv: pipeline identifiers only (shader hashes and state words, no game
    bytes), checked against the shared shader list; None when the list is absent."""
    if not PSO_SOURCE.is_file():
        return None
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
    from merge_pso_keys import encode, shader_ids, name, read_tsv
    try:
        records = read_tsv(PSO_SOURCE)
        allowed = shader_ids(PSO_SOURCE.with_name('shader_list.tsv'))
        for key_bytes, _, _ in records:
            vs, fs = name(key_bytes[:80]), name(key_bytes[80:160], True)
            if vs[:64] not in allowed or fs and fs[:64] not in allowed:
                raise ValueError('references a shader absent from shader_list.tsv')
        data = encode(records)
    except (OSError, ValueError) as error:
        raise SystemExit(f'pso_keys.tsv: {error}') from error
    print(f'pso-keys: generated {len(records)} keys, {len(data)} bytes, sha256={hashlib.sha256(data).hexdigest()}')
    return data


def write_zip(entries, stamp):
    """The APK's zip: stored entries 4-byte aligned (padding in the local extra field, as zipalign does), so the resource
    table and the fonts handed to AssetManager.openFd can be read in place."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, date_time=stamp)
            if name == 'lib/arm64-v8a/libtotk_xlate.so' or name == 'lib/arm64-v8a/libhactool.so':
                zi.external_attr = 0o100755 << 16   # executables the app starts from nativeLibraryDir
            stored = name == 'resources.arsc' or (name.startswith(('assets/', 'res/')) and Path(name).suffix.lower() in STORED_SUFFIXES)
            zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            if stored:   # extra field: 4-byte header + padding, so (offset + header + name + extra) % 4 == 0
                padding = -(buf.tell() + 30 + len(name.encode())) % 4
                if padding:
                    zi.extra = struct.pack('<HH', 0xffff, padding) + bytes(padding)
            z.writestr(zi, data)
    return buf.getvalue()


def build(args):
    libs = [Path(p).expanduser().resolve() for p in args.lib]
    for p in libs:
        if not p.is_file(): raise SystemExit(f'no such file: {p}')
    if len({p.name for p in libs}) != len(libs): raise SystemExit('two --lib files with one name')
    dex = Path(args.dex).expanduser().resolve()
    if not dex.is_file(): raise SystemExit(f'no such classes.dex: {dex}')
    translator = args.ondevice_xlate.expanduser().resolve() if args.ondevice_xlate else None
    if translator:
        if not translator.is_file(): raise SystemExit(f'no such on-device shader translator: {translator}')
        if args.min_sdk < 33: raise SystemExit('--ondevice-xlate requires --min-sdk 33 or newer')
        if any(p.name == 'libtotk_xlate.so' for p in libs):
            raise SystemExit('--ondevice-xlate duplicates --lib libtotk_xlate.so')
    asset_root = None
    if args.assets:
        asset_root = Path(args.assets).expanduser().resolve()
        if not asset_root.is_dir(): raise SystemExit(f'no such asset directory: {asset_root}')
    password = None
    if args.key_pass_env:
        password = os.environ.get(args.key_pass_env)
        if password is None: raise SystemExit(f'--key-pass-env: ${args.key_pass_env} is not set')
    if args.launcher_icon and args.launcher_mipmaps:
        raise SystemExit('--launcher-icon and --launcher-mipmaps are alternatives')
    override = (launcher_override(args.launcher_icon) if args.launcher_icon else
                launcher_mipmaps(args.launcher_mipmaps) if args.launcher_mipmaps else None)
    images = override[0] if override else []
    resources = icon_resources(args.package, images, adaptive=True, xml=backup_rules())
    manifest = binary_xml(manifest_tree(args.package, args.label, args.version_code, args.version_name, args.min_sdk,
                                       args.target_sdk, LAUNCHER_ICON if images else None,
                                       round_icon=LAUNCHER_ROUND if images else None, release=args.release,
                                       backup=xml_ids(bool(images))))
    entries = [('AndroidManifest.xml', manifest), ('classes.dex', dex.read_bytes())] + resources
    entries += [(f'lib/arm64-v8a/{p.name}', p.read_bytes()) for p in libs]
    if translator:
        # Android extracts lib*.so into its executable nativeLibraryDir. This is a NativeAOT PIE,
        # not a dlopen library; the runtime starts it as a child process from that directory.
        entries.append(('lib/arm64-v8a/libtotk_xlate.so', translator.read_bytes()))
    if asset_root:
        entries += [(f'assets/{p.relative_to(asset_root).as_posix()}', p.read_bytes())
                    for p in sorted(asset_root.rglob('*')) if p.is_file()]
    if any(path in ('assets/pso_keys.bin', 'assets/pso_keys.tsv') for path, _ in entries):
        raise SystemExit('duplicate PSO key asset; pso_keys.tsv is the only source')
    pso = pso_asset()
    if pso is not None:
        entries.append(('assets/pso_keys.bin', pso))
    key, cert = load_key(args.key, password, make=args.make_key)   # after every input is checked, before any output
    entries = v1_files(entries, key, cert) + entries   # META-INF first, as jarsigner writes it
    stamp = time.gmtime(args.timestamp)[:6] if args.timestamp is not None else time.localtime()[:6]
    apk = v2_sign(write_zip(entries, stamp), key, cert)
    out = Path(args.out).expanduser(); out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(apk)
    if override and override[1]:
        store = out.with_name(f'{out.stem}-launcher-512.png')
        store.write_bytes(override[1])
        print(f'launcher-icon: store PNG {store}')
    print(f'wrote {out} ({len(apk) / 2**20:.1f} MiB, package {args.package}, version {args.version_code})')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dex', type=Path, required=True, help='classes.dex (android/apk/build_dex.py: setup UI, game activity, companion)')
    ap.add_argument('--lib', action='append', required=True, help='native library for lib/arm64-v8a (libtotk.so first; repeatable)')
    ap.add_argument('--ondevice-xlate', type=Path,
                    help='Android NativeAOT translator executable, packaged as lib/arm64-v8a/libtotk_xlate.so')
    ap.add_argument('--assets', type=Path, help='asset root packed under assets/ (fonts/, licenses/); never game data')
    ap.add_argument('--out', required=True)
    ap.add_argument('--key', required=True, help='signing key + certificate: PEM (both in one file) or PKCS#12 (.p12/.pfx)')
    ap.add_argument('--key-pass-env', metavar='VAR', help='environment variable holding the key password')
    ap.add_argument('--make-key', action='store_true', help='create --key when missing (developer builds only)')
    ap.add_argument('--package', default=PACKAGE)
    ap.add_argument('--label', default=LABEL)
    ap.add_argument('--launcher-icon', type=Path, help='launcher artwork PNG/JPEG to render (needs Pillow)')
    ap.add_argument('--launcher-mipmaps', type=Path, help='pre-rendered launcher images (android/apk/launcher; no Pillow needed)')
    ap.add_argument('--version-code', type=int, default=int(time.time()) // 60, help='default: minutes since 1970, so each build updates the last')
    ap.add_argument('--version-name', default='0.1')
    ap.add_argument('--timestamp', type=int, help='zip entry time (seconds since 1970, UTC) for reproducible output')
    ap.add_argument('--min-sdk', type=int, default=33)
    ap.add_argument('--target-sdk', type=int, default=33)
    ap.add_argument('--release', action='store_true', help='mark the APK as non-debuggable')
    build(ap.parse_args(argv))


if __name__ == '__main__':
    main(sys.argv[1:])

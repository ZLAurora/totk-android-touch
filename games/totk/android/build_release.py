#!/usr/bin/env python3
"""Build the public TotK Native APK from this repository alone: no game files, no keys, nothing derived from them.

    python3 games/totk/android/build_release.py --ndk <android-ndk> --turnip <driver.so> --key <release.pem>
                                                [--key-pass-env VAR] [--work DIR] [-o totk-native.apk]
                                                [--translator <android-arm64 totk_xlate>] [--android-sdk SDK]
                                                [--version-code N] [--version-name X]   (default 1.0.0 / 10000)
    python3 games/totk/android/build_release.py --audit <apk>        (only the content audit)

What goes in (all built here, or fetched at pinned sources, except the two prebuilt inputs named below):
  - libtotk.so (the 1.4.3 runtime, android/Makefile VERSION=v143 DEV_BUILD=0) with libopus and libvpx (pinned sources),
    libdl_android.so, libtotk_lsfg.so and the NDK's libc++_shared.so;
  - libtotk_setup.so and libhactool.so (android/setup/setup.mk: the on-device setup, with capstone and hactool fetched at
    pinned commits by android/setup/*/deps.py);
  - libtotk_xlate.so, the shader translator (NativeAOT: published with tools/totk_xlate/publish_native.sh --android, or
    the prebuilt given by --translator);
  - libtotk_turnip.so: the pinned Mesa Turnip build given by --turnip, refused unless its SHA-256 is the pinned one;
  - classes.dex (android/java via apk/build_dex.py), the launcher icon (android/app-assets/icon), the open fonts
    (android/app-assets/fonts) and the licence texts of everything shipped (assets/licenses/), the identifier-only
    pipeline key list (assets/pso_keys.bin from assets/pso_keys.tsv).
The APK is signed with the caller's key (never made here) and then audited (apk/audit_apk.py): any entry off the allowlist
or anything that looks like game data stops the build. Everything game-specific is made on the player's device by the
app's setup from their own files.
Needs Linux x86-64 (or macOS) with Python 3.12+, git, make, cmake, javac 17+, the Android SDK (platform 33+, build-tools),
the NDK r28, and for the translator the .NET 10 SDK (or --translator).
"""
import argparse, hashlib, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent          # games/totk/android
TOTK = HERE.parent
REPO = TOTK.parents[1]
for p in (REPO, HERE / 'apk', HERE):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))
from build_app import BuildError, say, run, sha256   # noqa: E402
import audit_apk                                    # noqa: E402

VERSION = 'v143'
API = 33
TURNIP_SHA256 = audit_apk.TURNIP_SHA256
LICENSES = HERE / 'apk/licenses'   # licence texts of the prebuilt inputs (their sources are not fetched by this build)
# The app's own semantic version (the game version, 1.4.3, is shown separately in About). versionCode is derived from
# it, so it only ever grows: major * 10000 + minor * 100 + patch (1.0.0 -> 10000, 1.2.3 -> 10203; minor and patch < 100).
APP_VERSION = '1.0.1'


def semver_code(name):
    """The monotonic versionCode of a MAJOR.MINOR.PATCH version name (a pre-release suffix after '-' is ignored)."""
    parts = name.split('-', 1)[0].split('.')
    if len(parts) != 3 or not all(p.isdigit() for p in parts) or int(parts[1]) > 99 or int(parts[2]) > 99:
        raise BuildError(f'version {name!r} is not MAJOR.MINOR.PATCH with minor and patch below 100')
    major, minor, patch = map(int, parts)
    return major * 10000 + minor * 100 + patch


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--audit', type=Path, metavar='APK', help='only audit an existing APK and exit')
    ap.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'))
    ap.add_argument('--turnip', type=Path, help=f'the pinned Mesa Turnip driver.so (sha256 {TURNIP_SHA256})')
    ap.add_argument('--key', type=Path, help='release signing key + certificate (PEM, or PKCS#12 .p12/.pfx); never generated')
    ap.add_argument('--key-pass-env', metavar='VAR', help='environment variable holding the key password')
    ap.add_argument('--work', type=Path, default=TOTK / 'build/release', help='work folder (default games/totk/build/release)')
    ap.add_argument('-o', '--out', type=Path, help='the APK (default <work>/totk-native.apk)')
    ap.add_argument('--translator', type=Path, help='prebuilt Android NativeAOT totk_xlate (default: publish it with the .NET 10 SDK)')
    ap.add_argument('--android-sdk', type=Path, help='Android SDK (default ANDROID_HOME, ANDROID_SDK_ROOT, ~/Android/Sdk)')
    ap.add_argument('--version-code', type=int, help='default: from the version name, MAJOR*10000 + MINOR*100 + PATCH')
    ap.add_argument('--version-name', help=f'default: {APP_VERSION}')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args(argv)
    if not args.audit:
        missing = [n for n in ('ndk', 'turnip', 'key') if not getattr(args, n)]
        if missing:
            ap.error('needs ' + ', '.join('--' + n for n in missing))
    return args


def check_turnip(path):
    path = Path(path).expanduser().resolve()
    if not path.is_file():
        raise BuildError(f'no Turnip driver at {path}')
    got = sha256(path)
    if got != TURNIP_SHA256:
        raise BuildError(f'{path} is not the pinned Turnip build (sha256 {got}, expected {TURNIP_SHA256})')
    return path


def git(*args):
    return subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True, check=True).stdout.strip()


def load_helper(path, name):
    """A setup sub-directory's deps.py (two of them share the file name, so they are loaded by path)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


# ---- native libraries ----
def build_native(work, ndk, logs, *, release, jobs, setup=True):
    """libtotk.so and its companions for VERSION, plus (setup=True) the setup library and hactool. Returns the library
    paths in packing order. Nothing here reads game files: the runtime takes its game data at start."""
    import build_android as dev
    opus = dev.build_opus(work, ndk, API, logs)
    vpx = dev.build_vpx(work, ndk, API, logs)
    # Their sources also supply the licence texts packed into the APK (licence_texts), even when the libraries are cached.
    dev.fetch(*dev.OPUS_SRC, work / 'src/opus', logs / 'fetch-opus.log')
    dev.fetch(*dev.VPX_SRC, work / 'src/libvpx', logs / 'fetch-libvpx.log')
    out = work / f'native-{"release" if release else "dev"}'
    make = ['make', '-C', HERE, 'TARGET=android', 'APP=1', f'NDK={ndk}', f'API={API}', f'VERSION={VERSION}', f'OUT={out}',
            f'OPUS={opus}', f'VPX={vpx}', 'GPU=vk', f'DEV_BUILD={0 if release else 1}', f'PYTHON={sys.executable}', f'-j{jobs}', 'all']
    if setup:
        hactool = load_helper(HERE / 'setup/hactool/deps.py', 'hactool_deps').prepare_hactool(work, logs / 'fetch-hactool-setup.log')
        capstone = load_helper(HERE / 'setup/rewrite/deps.py', 'capstone_deps').prepare_capstone(
            work, logs / 'capstone-android.log', target='android', ndk=ndk, api=API)
        zstd = load_helper(HERE / 'setup/seed/deps.py', 'zstd_deps').prepare_zstd(work, logs / 'zstd-android.log', target='android', ndk=ndk, api=API)
        webp = load_helper(HERE / 'setup/companion/deps.py', 'webp_deps').prepare_libwebp(work, logs / 'libwebp-android.log', target='android', ndk=ndk, api=API)
        make += [f'HACTOOL_SRC={hactool}', f'CAPSTONE_PREFIX_ANDROID={capstone}', f'ZSTD_PREFIX_ANDROID={zstd}', f'WEBP_PREFIX_ANDROID={webp}',
                 'setup-android']
    say(f'native: libtotk.so ({VERSION}, {"release" if release else "dev"}){" + setup library" if setup else ""} with the NDK')
    run(make, logs / f'make-native-{"release" if release else "dev"}.log')
    libs = [out / 'libtotk.so', out / 'libdl_android.so', out / 'libtotk_lsfg.so']
    host = f"{'linux' if sys.platform.startswith('linux') else 'darwin'}-x86_64"
    cxx = Path(ndk) / f'toolchains/llvm/prebuilt/{host}/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
    if not cxx.is_file():
        raise BuildError(f'no libc++_shared.so in the NDK ({cxx})')
    libs.append(cxx)   # libtotk_lsfg.so and Vulkan drivers built against the NDK's C++ library
    if setup:
        libs += [out / 'libtotk_setup.so', out / 'libhactool.so']
    for lib in libs:
        if not lib.is_file():
            raise BuildError(f'the native build did not produce {lib}')
    return libs


def strip_debug(libs, work, ndk, logs):
    """The APK's copies of the libraries built here, without debug info: llvm-strip --strip-debug drops the DWARF (and with
    it every host path) and keeps the symbol tables and the dynamic symbols that JNI, dlopen and crash reports use. The
    full libraries stay in <work>/symbols, outside the APK. The pinned Turnip and the translator are packed as they are."""
    host = f"{'linux' if sys.platform.startswith('linux') else 'darwin'}-x86_64"
    strip = Path(ndk) / f'toolchains/llvm/prebuilt/{host}/bin/llvm-strip'
    if not strip.is_file():
        raise BuildError(f'no llvm-strip in the NDK ({strip})')
    symbols, shipped = Path(work) / 'symbols', Path(work) / 'apk-libs'
    for folder in (symbols, shipped):
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
    out = []
    for lib in libs:
        shutil.copy2(lib, symbols / lib.name)
        run([strip, '--strip-debug', '-o', shipped / lib.name, lib], logs / f'strip-{lib.name}.log')
        out.append(shipped / lib.name)
    say(f'strip: debug info removed from {len(out)} libraries (full copies in {symbols})')
    return out


def release_translator(path, work, ndk, logs):
    """The on-device shader translator: the caller's prebuilt, or a NativeAOT publish kept current in <work>/translator."""
    if path:
        path = Path(path).expanduser().resolve()
        if not path.is_file():
            raise BuildError(f'no translator at {path}')
        with path.open('rb') as f:
            if f.read(4) != b'\x7fELF':
                raise BuildError(f'{path} is not an Android executable')
        return path
    import build_android as dev
    out = work / 'translator'
    binary, stamp = out / 'totk_xlate', out / 'totk_xlate.inputs.sha256'
    want = dev.translator_inputs_hash(TOTK) + f'\napi={API}'
    if binary.is_file() and stamp.is_file() and stamp.read_text().strip() == want + '\n' + sha256(binary):
        return binary
    dotnet = os.environ.get('DOTNET', 'dotnet')
    if not shutil.which(dotnet):
        raise BuildError('the translator needs the .NET 10 SDK (dotnet on PATH or DOTNET=...), or pass --translator')
    say('translator: publishing the Android NativeAOT translator')
    run(['sh', TOTK / 'tools/totk_xlate/publish_native.sh', '--android', '--ndk', ndk, '--api', str(API), '--dotnet', dotnet,
         '--out', out], logs / 'publish-android-translator.log', cwd=TOTK, quiet=False)
    if not binary.is_file():
        raise BuildError(f'publishing did not produce {binary}')
    stamp.write_text(want + '\n' + sha256(binary) + '\n')
    return binary


def build_classes(sdk, out, logs):
    import build_dex
    selected = build_dex.find_sdk(sdk)
    if selected is None:
        raise BuildError('no Android SDK with platform 33+ and build-tools (d8); pass --android-sdk')
    run([sys.executable, HERE / 'apk/build_dex.py', '--android-sdk', selected[0], '--out', out], logs / 'build_dex.log')
    return out


# ---- assets: fonts and licence texts ----
def leading_comment(path):
    """The first C comment of a source file (its licence notice)."""
    text = Path(path).read_text(errors='replace')
    start, end = text.find('/*'), text.find('*/')
    if start < 0 or end < start:
        raise BuildError(f'no licence comment at the top of {path}')
    return text[start:end + 2]


def line_comment(path, after=None):
    """The first run of '//' comment lines of a source file (after the line `after`, when given), without the slashes."""
    lines = Path(path).read_text(errors='replace').splitlines()
    if after is not None:
        if after not in lines:
            raise BuildError(f'no {after!r} in {path}')
        lines = lines[lines.index(after) + 1:]
    start = next((i for i, l in enumerate(lines) if l.startswith('//')), None)
    if start is None:
        raise BuildError(f'no licence comment in {path}')
    block = []
    for l in lines[start:]:
        if not l.startswith('//'):
            break
        block.append(l[2:].removeprefix(' '))
    return '\n'.join(block).strip()


def licence_texts(work, ndk, *, setup=True):
    """{asset file name: text} for every component in the APK. Fetched sources supply their own texts; the two prebuilt
    inputs (Turnip, the .NET runtime inside the translator) use the copies in android/apk/licenses."""
    t = TOTK
    host = f"{'linux' if sys.platform.startswith('linux') else 'darwin'}-x86_64"
    files = {
        'TotK-Native.txt': [REPO / 'LICENSE'],
        'THIRD_PARTY_NOTICES.txt': [REPO / 'THIRD_PARTY_NOTICES.md'],
        'Mesa-Turnip.txt': [LICENSES / 'Mesa-Turnip.txt'],
        'dotnet.txt': [LICENSES / 'dotnet.txt'],
        'Ryujinx.txt': [REPO / 'vendor/ryujinx/LICENSE.txt', REPO / 'vendor/ryujinx/src/Spv.Generator/LICENSE'],
        'totk_xlate.txt': [t / 'tools/totk_xlate/LICENSE'],
        'd2r-mobile.txt': [t / 'third_party/d2r/LICENSE', t / 'third_party/d2r_gfx/LICENSE'],
        'deko3d.txt': [t / 'third_party/d2r_gfx/LICENSE.deko3d'],
        'bcdec.txt': [t / 'third_party/d2r_gfx/third_party/bcdec/LICENSE'],
        'SMAA.txt': [t / 'android/gfx/post/smaa/LICENSE.txt'],
        'FidelityFX-FSR.txt': [t / 'android/gfx/post/fsr/LICENSE.txt'],
        'Snapdragon-GSR.txt': [t / 'android/gfx/post/sgsr/LICENSE'],
        'lsfg-vk-android.txt': [t / 'android/third_party/lsfg/LICENSE.md', t / 'android/third_party/lsfg/third_party/volk/LICENSE.md',
                                t / 'android/third_party/lsfg/third_party/pe-parse/LICENSE', t / 'android/third_party/lsfg/third_party/dxbc/LICENSE.md'],
        **{f'{p.stem}.txt': [p] for p in sorted((t / 'third_party/licenses').glob('*.txt'))},
        'LLVM-libc++.txt': [Path(ndk) / f'toolchains/llvm/prebuilt/{host}/NOTICE'],
        'opus.txt': [work / 'src/opus/COPYING'],
        'libvpx.txt': [work / 'src/libvpx/LICENSE', work / 'src/libvpx/PATENTS'],
    }
    if setup:
        hactool = work / 'src/hactool-setup'
        capstone = work / 'src/capstone'
        files['capstone.txt'] = [next((p for p in (capstone / 'LICENSE.TXT', capstone / 'LICENSE.txt', capstone / 'LICENSE') if p.is_file()),
                                      capstone / 'LICENSE.TXT'), capstone / 'LICENSE_LLVM.TXT']   # LLVM: the TableGen-derived tables
        files['hactool.txt'] = [hactool / 'LICENSE']
        files['mbedtls.txt'] = [hactool / 'mbedtls/LICENSE', hactool / 'mbedtls/apache-2.0.txt']
        files['zstd.txt'] = [work / 'src/zstd/LICENSE']
        files['libwebp.txt'] = [work / 'src/libwebp/COPYING', work / 'src/libwebp/PATENTS']
        # texture decoders and image code ported into the companion stage (texture2ddecoder, AssetStudio, mikunyan, bimg, Pillow)
        files['companion-ports.txt'] = [HERE / 'setup/companion/THIRD_PARTY_LICENSES.txt']
    texts = {}
    for name, sources in files.items():
        for s in sources:
            if not s.is_file():
                raise BuildError(f'licence text {s} missing (for {name})')
        texts[name] = '\n\n'.join(s.read_text(errors='replace').rstrip() for s in sources) + '\n'
    # Notices that live only in source headers.
    texts['Ryujinx.txt'] += ('\n\n==== Spv.Generator SPIR-V grammar (Autogenerated/*Grammar.cs) ====\n'
                             + line_comment(REPO / 'vendor/ryujinx/src/Spv.Generator/Autogenerated/CoreGrammar.cs', '#region Grammar License') + '\n')
    texts['lsfg-vk-android.txt'] += ('\n\n==== SPIR-V headers (dxbc/include/spirv/thirdparty/spirv.hpp) ====\n'
                                     + line_comment(t / 'android/third_party/lsfg/third_party/dxbc/include/spirv/thirdparty/spirv.hpp') + '\n')
    texts['NVN-header.txt'] = leading_comment(t / 'third_party/d2r_gfx/gfx/nvn/include/nvn.h') + '\n'
    if setup:   # hactool's bundled third-party sources carry their notices in their headers
        hactool = work / 'src/hactool-setup'
        texts['hactool.txt'] += ''.join(f'\n\n==== {n} ====\n{leading_comment(hactool / n)}\n' for n in ('cJSON.h', 'lz4.h', 'ConvertUTF.h')
                                        if (hactool / n).is_file())
    return texts


def stage_assets(stage, texts, commit=None):
    """assets/: fonts/ (the open fonts and their licences, android/app-assets/fonts), licenses/ (one text per component),
    shader_list.tsv (the committed shared shader list: the on-device seed's inventory, shader hashes only) and build.txt
    (the source commit, shown in About)."""
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'fonts').mkdir(parents=True)
    fonts = sorted(p for p in (HERE / 'app-assets/fonts').iterdir() if p.is_file() and p.suffix.lower() in ('.ttf', '.otf', '.txt'))
    if not any(p.suffix.lower() == '.ttf' for p in fonts):
        raise BuildError('no fonts in android/app-assets/fonts')
    for p in fonts:
        shutil.copy2(p, stage / 'fonts' / p.name)
    (stage / 'licenses').mkdir()
    for name, text in texts.items():
        (stage / 'licenses' / name).write_text(text)
    shutil.copy2(TOTK / 'assets/shader_list.tsv', stage / 'shader_list.tsv')
    if commit:
        (stage / 'build.txt').write_text(f'commit {commit}\n')
    return stage


def verify_signatures(apk, logs, sdk=None):
    """Every signature scheme in the APK, verified by apksigner. Left to itself apksigner checks only what the APK's minSdk
    needs (v3 for 33) and reports v1/v2 "false" without reading them, so it is also run as an Android 7 (24: v2) and an
    Android 4.3 (18: v1 too) would verify the APK. Returns the scheme lines; a failure raises."""
    import build_dex
    selected = build_dex.find_sdk(sdk)
    apksigner = Path(selected[2]).with_name('apksigner') if selected else None
    if not apksigner or not apksigner.is_file():
        raise BuildError('apksigner not found next to d8 in the Android SDK build-tools; pass --android-sdk')
    want = {None: ('v3', 'APK Signature Scheme v3'), 24: ('v2', 'APK Signature Scheme v2'), 18: ('v1', 'JAR signing')}
    report = []
    with open(Path(logs) / 'apksigner.log', 'w') as log:
        for sdk_level, (scheme, title) in want.items():
            cmd = [str(apksigner), 'verify', '-v', '--print-certs', *(['--min-sdk-version', str(sdk_level)] if sdk_level else []), str(apk)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            log.write('$ ' + ' '.join(cmd[1:]) + '\n' + r.stdout + r.stderr + '\n')
            line = f'Verified using {scheme} scheme ({title}): true'
            if r.returncode or line not in r.stdout:
                raise BuildError(f'apksigner: {scheme} does not verify (see {Path(logs) / "apksigner.log"})')
            report.append(line)
    return report


def package(*, out, dex, libs, translator, turnip, assets, key, key_pass_env=None, make_key=False, release, version_code,
            version_name, timestamp=None, logs, android_sdk=None):
    """Sign the APK with build_apk.py, verify each signature scheme and audit it; a failure removes the APK."""
    libs = list(libs) + ([turnip] if turnip else [])
    if turnip:   # packed under the name the runtime looks for beside libtotk.so
        staged = Path(assets).parent / 'turnip/libtotk_turnip.so'
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(turnip, staged)
        libs[-1] = staged
    # The launcher images are rendered once from the app's artwork and committed (apk/render_launcher.py), so this build
    # needs no Pillow; a stale render (artwork changed since) is refused.
    icons = HERE / 'apk/launcher'
    stamp = icons / 'source.sha256'
    art = HERE / 'app-assets/icon/totk-native-1024.png'
    if not stamp.is_file() or not art.is_file() or stamp.read_text().split()[0] != sha256(art):
        raise BuildError(f'{icons} is missing or older than {art}: rerun python3 games/totk/android/apk/render_launcher.py (needs Pillow)')
    cmd = [sys.executable, HERE / 'apk/build_apk.py', '--dex', dex, *sum((['--lib', str(p)] for p in libs), []),
           '--ondevice-xlate', translator, '--assets', assets, '--launcher-mipmaps', icons, '--out', out, '--key', key,
           '--version-code', str(version_code), '--version-name', version_name, '--min-sdk', str(API), '--target-sdk', str(API)]
    cmd += (['--key-pass-env', key_pass_env] if key_pass_env else []) + (['--make-key'] if make_key else [])
    cmd += (['--release'] if release else []) + (['--timestamp', str(timestamp)] if timestamp is not None else [])
    run(cmd, logs / 'build_apk.log')
    try:
        schemes = verify_signatures(out, logs, android_sdk)
    except BuildError:
        Path(out).unlink()
        raise
    say('signatures: ' + '; '.join(line.split(' scheme ')[0].split()[-1] for line in schemes) + ' verified by apksigner')
    problems = audit_apk.audit(out)
    if problems:
        Path(out).unlink()
        raise BuildError('APK audit failed, APK removed:\n  ' + '\n  '.join(problems))
    say(f'audit: {out} passed ({len(audit_entries(out))} entries)')
    return out


def audit_entries(apk):
    import zipfile
    with zipfile.ZipFile(apk) as z:
        return z.namelist()


def app(*, work, ndk, logs, out, key, key_pass_env=None, make_key=False, release=True, turnip=None, translator=None,
        android_sdk=None, version_code=None, version_name=None, jobs=os.cpu_count() or 4):
    """The whole app from the repository: native libraries, translator, dex, assets, signed and audited APK. Shared by the
    public release (release=True, the caller's key and the pinned Turnip) and build_android.py --apk (a debuggable build
    signed with the work folder's developer key)."""
    work, out = Path(work), Path(out)
    work.mkdir(parents=True, exist_ok=True)
    libs = build_native(work, ndk, logs, release=release, jobs=jobs)
    libs = strip_debug(libs, work, ndk, logs)   # every APK passes the same audit; the full libraries stay in <work>/symbols
    xlate = release_translator(translator, work, ndk, logs)
    dex = build_classes(android_sdk, work / 'classes.dex', logs)
    assets = stage_assets(work / 'apk-stage/assets', licence_texts(work, ndk), commit=source_commit())
    stamp = int(git('log', '-1', '--format=%ct'))
    # Developer builds (make_key) keep a time-based versionCode, so each one updates the previous developer build.
    name = version_name or (f'{APP_VERSION}-dev' if make_key else APP_VERSION)
    return package(out=out, dex=dex, libs=libs, translator=xlate, turnip=check_turnip(turnip) if turnip else None, assets=assets,
                   key=key, key_pass_env=key_pass_env, make_key=make_key, release=release,
                   version_code=version_code or (int(time.time()) // 60 if make_key else semver_code(name)),
                   version_name=name, timestamp=stamp, logs=logs, android_sdk=android_sdk)


def source_commit():
    """The HEAD commit (12 hex digits), with '-dirty' when tracked files differ from it."""
    commit = git('rev-parse', '--short=12', 'HEAD')
    dirty = subprocess.run(['git', '-C', str(REPO), 'diff', '--quiet', 'HEAD', '--'], capture_output=True).returncode != 0
    return commit + ('-dirty' if dirty else '')


def main(argv):
    args = parse_args(argv)
    if args.audit:
        problems = audit_apk.audit(args.audit)
        for p in problems:
            print(f'{args.audit}: {p}')
        if problems:
            raise BuildError('APK audit failed')
        say(f'audit: {args.audit} passed')
        return
    if sys.version_info < (3, 12):
        raise BuildError('needs Python 3.12 or later')
    ndk = Path(args.ndk).expanduser().resolve()
    if not (ndk / 'build/cmake/android.toolchain.cmake').is_file():
        raise BuildError('--ndk must point at an Android NDK (r28)')
    key = args.key.expanduser().resolve()
    if not key.is_file():
        raise BuildError(f'no signing key at {key} (this build never makes one)')
    turnip = check_turnip(args.turnip)
    work = args.work.expanduser().resolve()
    logs = work / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    out = (args.out.expanduser().resolve() if args.out else work / 'totk-native.apk')
    t0 = time.time()
    app(work=work, ndk=ndk, logs=logs, out=out, key=key, key_pass_env=args.key_pass_env, release=True, turnip=turnip,
        translator=args.translator, android_sdk=args.android_sdk, version_code=args.version_code, version_name=args.version_name,
        jobs=args.jobs)
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out} (sha256 {sha256(out)})')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nbuild stopped: {e}', file=sys.stderr)
        sys.exit(1)

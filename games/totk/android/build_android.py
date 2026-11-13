#!/usr/bin/env python3
"""Developer build of the Android (arm64) port of Tears of the Kingdom from your own game files, on a Linux PC.

    python3 games/totk/android/build_android.py <game.xci|game.nsp> -k <prod.keys> --ndk <android-ndk> [--update <update.nsp>]
                                                [--work <folder>] [-o <output folder>] [--equiv] [--apk [--turnip driver.so]]
                                                [--no-packed-shader-seed] [--runtime-only]

Players do not need this: the public APK (android/build_release.py, docs/android-install.md) sets the game up on the
phone. This build does the same work on a PC, for development: a folder (default "<game file folder>/TOTK_android")
holding the game's code rewritten for Android, the game data folder the runtime reads at start (game-data/<version>:
module tables, RomFS and font hashes), the RomFS, the runtime built with the Android NDK, install.json, and run.sh /
push.sh to put it on a device and start it from adb shell. --apk also builds the app (the same as the public one, but
debuggable and signed with a developer key in the work folder) and install_app.sh to fill the app's game folder over USB.

Steps:
  1. The same extraction and checks as build_app.py: ExeFS and the three code modules, rewritten for Apple silicon and
     checked against games/totk/assets/expected-v<version>.json. That rewrite is not used on Android; matching the
     verified hashes proves the dump and the extraction.
  2. The modules rewritten again for Linux / Android (DM_HOST_TP=tpidr_el0, third_party/d2r/hostreg.py). Every rewrite
     site, cut and gate must be the same set as in the verified rewrite; the structural checker (structck.py) checks the
     new images word by word; --equiv also runs the emulator equivalence check of every site (hours, as on the Mac).
  3. RomFS (17 GB), shared fonts and RomFS overlays, as build_app.py; then the game data folder (for 1.4.3 checked
     against the pinned hashes, which the runtime also checks).
  4. libopus and libvpx for Android (pinned sources), then the runtime (android/Makefile, TARGET=android); the runtime
     itself holds nothing of the game.

Needs: Linux (or macOS) with Python 3.12+ (3.14 for NSZ updates), git, rsync, make, cmake, clang or gcc (hactool, for
updates), llvm-objdump / llvm-objcopy (LLVM_BIN, default /usr/bin), the Android NDK r26 or newer, and about 45 GB free.
Nothing from the game is in the repository; key values are never printed.
By default also make shaders/<version>/seed.vkpack from the staged Vulkan seed (loose files are kept on the PC).
Deploy packs with \"sh push.sh [device folder]\" or \"sh install_app.sh data\".
The indexed runtime is on by default. --no-packed-shader-seed / push.sh --loose retain legacy loose deployment.
Existing loose translated overlays are never deleted.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(HERE / 'apk'))
sys.path.insert(0, str(HERE.parent / 'scripts'))
import build_app as mac   # extraction, checks and helpers shared with the Mac build
from build_app import BuildError, say, run, sha256, check
from pack_shader_seed import pack_seed

OPUS_SRC = ('https://github.com/xiph/opus.git', 'v1.5.2')
VPX_SRC = ('https://chromium.googlesource.com/webm/libvpx', 'v1.15.0')
MODULES = ('main', 'subsdk0', 'sdk')


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', type=Path, help='the game file (.xci or .nsp), base game 1.0.0')
    ap.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('--update', type=Path, help='separate supported cumulative update NSP/NSZ (1.1.2, 1.2.1 or 1.4.3)')
    ap.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'),
                    help='Android NDK directory (default $ANDROID_NDK_HOME)')
    ap.add_argument('--api', type=int, default=33, help='Android API level (the AYN Thor runs Android 13 = 33)')
    ap.add_argument('-o', '--out', type=Path, help='output folder (default: TOTK_android next to the game file)')
    ap.add_argument('--work', type=Path, help='work folder (default: TOTK_build next to the game file, shared with the Mac build)')
    ap.add_argument('--equiv', action='store_true', help='also run the emulator equivalence check of every rewrite site (hours)')
    ap.add_argument('--no-vpx', action='store_true', help='leave out libvpx (cutscenes then abort, as on iOS)')
    ap.add_argument('--gpu', choices=('vk', 'null'), default='vk', help='GPU backend: vk (Vulkan, default) or null (no GPU)')
    ap.add_argument('--release', action='store_true', help='build the release runtime and APK (default: dev build)')
    ap.add_argument('--shaders', action='store_true',
                    help='translate the game\'s shaders for Vulkan (about an hour the first time; needs the Linux shader translator, '
                         'games/totk/tools/totk_xlate/publish_native.sh). With --runtime-only: also (re)make the shaders')
    ap.add_argument('--translator', type=Path, help='the shader translator (default: games/totk/prebuilt/linux-x64/totk_xlate)')
    ap.add_argument('--packed-shader-seed', action=argparse.BooleanOptionalAction, default=True,
                    help='pack the staged Vulkan seed by default (also with --runtime-only, without retranslating); '
                         '--no-packed-shader-seed retains loose-only staging')
    ap.add_argument('--ondevice-xlate', action='store_true',
                    help='include the Android shader translator in the output (reuses prebuilt/android-arm64/totk_xlate; otherwise '
                         'needs the .NET 10 SDK and NDK to publish it); --apk always packs a translator')
    ap.add_argument('--apk', action='store_true',
                    help='also build the Android app (totk.apk, debuggable unless --release, developer key work/apk-key.pem) and install_app.sh')
    ap.add_argument('--turnip', type=Path, help='with --apk: the pinned Mesa Turnip driver.so to bundle (default: none; the system or your driver.so)')
    ap.add_argument('--no-companion', action='store_true', help='with --apk: do not make the dual-screen companion\'s drawings (companion/)')
    ap.add_argument('--android-sdk', type=Path, help='SDK for the app\'s Java (otherwise ANDROID_HOME, ANDROID_SDK_ROOT, ~/Android/Sdk)')
    ap.add_argument('--runtime-only', action='store_true',
                    help='after a full build: rebuild only totk-native (and with --apk the app) from the current sources and the '
                         'game data folder from the earlier rewrite (minutes); push it with sh push.sh runtime')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA directory with the shared fonts (NSP input)')
    fonts.add_argument('--shared-fonts', type=Path, help='previously extracted shared-font directory (NSP input)')
    return ap.parse_args(argv)


def prerequisites(args):
    if sys.version_info < (3, 12):
        raise BuildError('needs Python 3.12 or later')
    for tool in ('git', 'rsync', 'make', 'cmake'):
        if not shutil.which(tool):
            raise BuildError(f'{tool} not found')
    llvm = Path(os.environ.get('LLVM_BIN', '/usr/bin'))
    if not (llvm / 'llvm-objdump').exists() or not (llvm / 'llvm-objcopy').exists():
        raise BuildError(f'llvm-objdump / llvm-objcopy not found in {llvm} (install LLVM, or set LLVM_BIN)')
    if not args.ndk or not (Path(args.ndk) / 'build/cmake/android.toolchain.cmake').is_file():
        raise BuildError('--ndk must point at an Android NDK (r26 or newer), or set ANDROID_NDK_HOME')
    if args.ondevice_xlate and args.api < 33:
        raise BuildError('--ondevice-xlate needs Android API 33 or newer')
    return Path(sys.executable), llvm


# ---- step 2: rewrite for Linux / Android ----
def site_lists(folder, name):
    """What must be the same in both rewrites: the sites (text addresses and kinds, window and call extents), cuts,
    parameter gates, observation gates, and each gate's kind / argument / original address (its return point lies in
    the patch area, whose layout differs)."""
    out = {}
    for kind in ('sites', 'cuts', 'params', 'obs'):
        p = folder / f'{name}.{kind}.tsv'
        out[kind] = sorted(p.read_text().splitlines()) if p.exists() else []
    out['gates'] = sorted(' '.join(l.split()[1:]) for l in (folder / f'{name}.gates.tsv').read_text().splitlines())
    return out


def android_rewrite(tree, version, env, logs, llvm, *, equiv=False, darwin=None):
    """Rewrite main / subsdk0 / sdk of `version` for Linux / Android into rewritten-android/<version>; returns that folder.
    darwin: the verified Darwin rewrite to compare the site sets with (default rewritten/<version>)."""
    py = tree / '.venv/bin/python'
    exefs = tree / f'extracted/{version}/exefs'
    darwin = darwin or tree / f'rewritten/{version}'
    out = tree / f'rewritten-android/{version}'
    out.mkdir(parents=True, exist_ok=True)
    renv = dict(env, DM_RULESET='native', DM_PRIVATE_GATES='0', DM_HOST_TP='tpidr_el0', LLVM_BIN=str(llvm))
    stamp = out / 'verified.json'
    rewriter = sha256(tree / 'third_party/d2r/patch_nso.py') + sha256(tree / 'third_party/d2r/hostreg.py')
    known = json.loads(stamp.read_text()) if stamp.exists() else {}
    for name in MODULES:
        image = out / f'{name}.patched.bin'
        k = known.get(name, {})
        if image.exists() and k.get('source') == sha256(exefs / name) and k.get('image') == sha256(image) \
                and k.get('rewriter') == rewriter and (k.get('equiv') or not equiv):
            continue
        say(f'2/4 code: rewriting {name} for Android' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite_android_{version}_{name}.log', cwd=tree, env=renv)
        a, b = site_lists(darwin, name), site_lists(out, name)
        for kind in a:
            if a[kind] != b[kind]:
                raise BuildError(f'{name}: the Android rewrite has different {kind} than the verified rewrite '
                                 f'({len(a[kind])} vs {len(b[kind])}); please report this')
        say(f'2/4 code: structural check of {name} (every rewritten word)')
        run([py, 'third_party/d2r/structck.py', out, name, exefs / name], logs / f'structck_android_{version}_{name}.log', cwd=tree, env=renv)
        if equiv:
            say(f'2/4 code: emulator equivalence check of every {name} site (long)')
            run([py, 'third_party/d2r/equiv.py', out, name, exefs / name], logs / f'equiv_android_{version}_{name}.log', cwd=tree, env=renv)
        known[name] = {'source': sha256(exefs / name), 'image': sha256(image), 'rewriter': rewriter, 'equiv': equiv}
        stamp.write_text(json.dumps(known, indent=2) + '\n')
    report = dict(format=1, status='pass', rom_version=version, host='linux-android (tpidr_el0)',
                  scope='same sites as the verified Darwin rewrite; structck' + (' + equiv' if all(known[n].get('equiv') for n in MODULES) else ''),
                  module_file_sha256={**{f'extracted/{version}/exefs/{n}': sha256(exefs / n) for n in MODULES},
                                      **{str((out / f'{n}.patched.bin').relative_to(tree)): sha256(out / f'{n}.patched.bin') for n in MODULES}})
    (out / 'phase12-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    return out


# ---- step 4: libraries and the runtime ----
def fetch(url, ref, dst, log):
    marker = dst / '.fetched-ref'
    if marker.is_file() and marker.read_text() == ref:
        return dst
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, ref], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)
    marker.write_text(ref)
    return dst


def ndk_tools(ndk, api):
    host = next((p for p in (Path(ndk) / 'toolchains/llvm/prebuilt').iterdir() if p.is_dir()), None)
    if not host:
        raise BuildError('no prebuilt LLVM toolchain in the NDK')
    b = host / 'bin'
    return dict(CC=str(b / f'aarch64-linux-android{api}-clang'), CXX=str(b / f'aarch64-linux-android{api}-clang++'),
                AR=str(b / 'llvm-ar'), NM=str(b / 'llvm-nm'), RANLIB=str(b / 'llvm-ranlib'), STRIP=str(b / 'llvm-strip'), AS=str(b / f'aarch64-linux-android{api}-clang'))


def build_opus(work, ndk, api, logs):
    """libopus for Android. Its hardening keeps assertions, whose messages carry __FILE__: the source folder is mapped to
    opus/ (and the NDK to ndk/), so no host path reaches libtotk.so. A library built with other flags is rebuilt."""
    prefix = work / 'android-libs/opus'
    src = work / 'src/opus'
    build = work / 'android-libs/opus-build'
    flags = ' '.join(f'-ffile-prefix-map={p}/={name}/' for p, name in
                     ((src.resolve(), 'opus'), (build.resolve(), 'opus-build'), (Path(ndk).resolve(), 'ndk')))
    stamp = prefix / 'build-flags.txt'
    if (prefix / 'lib/libopus.a').is_file() and stamp.is_file() and stamp.read_text() == flags + '\n':
        return prefix
    say('4/4 libraries: libopus for Android')
    src = fetch(*OPUS_SRC, src, logs / 'fetch-opus.log')
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(prefix, ignore_errors=True)
    run(['cmake', '-S', src, '-B', build, f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', '-DANDROID_ABI=arm64-v8a',
         f'-DANDROID_PLATFORM=android-{api}', '-DCMAKE_BUILD_TYPE=Release', '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_BUILD_TESTING=OFF',
         '-DBUILD_TESTING=OFF', f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_INSTALL_PREFIX={prefix}'], logs / 'opus-cmake.log')
    run(['cmake', '--build', build, '--target', 'install', '-j', str(os.cpu_count() or 4)], logs / 'opus-build.log')
    stamp.write_text(flags + '\n')
    return prefix


def build_vpx(work, ndk, api, logs):
    """libvpx's VP9 decoder only, configured as generic arm64 Linux with the NDK's compilers (its own android target
    expects the old standalone toolchains)."""
    prefix = work / 'android-libs/vpx'
    if (prefix / 'lib/libvpx.a').is_file():
        return prefix
    say('4/4 libraries: libvpx (VP9 decoder) for Android')
    src = fetch(*VPX_SRC, work / 'src/libvpx', logs / 'fetch-libvpx.log')
    build = work / 'android-libs/vpx-build'
    shutil.rmtree(build, ignore_errors=True); build.mkdir(parents=True)
    env = dict(os.environ, **ndk_tools(ndk, api), LD=ndk_tools(ndk, api)['CC'])
    run([src / 'configure', '--target=arm64-linux-gcc', f'--prefix={prefix}', '--enable-pic', '--disable-examples', '--disable-tools',
         '--disable-docs', '--disable-unit-tests', '--disable-vp8', '--disable-vp9-encoder', '--enable-vp9-decoder',
         '--disable-webm-io', '--disable-libyuv', '--enable-runtime-cpu-detect'], logs / 'vpx-configure.log', cwd=build, env=env)
    run(['make', '-j', str(os.cpu_count() or 4), 'install'], logs / 'vpx-build.log', cwd=build, env=env)
    return prefix


def write_game_data(tree, version, rewritten, logs):
    """The game data folder the runtime reads at start (runtime/game_data.h): runtime-tables.bin (TOTKTAB1, from the Android
    rewrite), romfs-config.txt and shared-fonts.txt, in <tree>/build/game-data/<version>. For 1.4.3 every value must be
    the pinned one (assets/expected-v1.4.3.json), as the pinned runtime refuses anything else. Returns the folder."""
    py = tree / '.venv/bin/python'
    out = tree / f'build/game-data/{version}'
    out.mkdir(parents=True, exist_ok=True)
    run([py, 'scripts/prepare_runtime.py', '--version', version, '--rewritten', rewritten, '--report', rewritten / 'phase12-validation.json',
         '--out', out], logs / f'prepare_runtime_android_{version}.log', cwd=tree)
    run([py, 'scripts/prepare_romfs_backend.py', '--version', version, '--out', out], logs / f'prepare_romfs_{version}.log', cwd=tree)
    run([py, 'scripts/prepare_shared_fonts.py', '--version', version, '--out', out], logs / 'prepare_shared_fonts.log', cwd=tree)
    if version == 'v143':
        check_pins(out, rewritten)
    return out


def config_values(path):
    """The KEY VALUE lines of a game data text file (runtime/game_data.c reads the same)."""
    return dict(line.split(' ', 1) for line in path.read_text().splitlines() if line and not line.startswith('#'))


def check_pins(game_data, rewritten):
    pins = json.loads((HERE.parent / 'assets/expected-v1.4.3.json').read_text())
    for name in MODULES:
        check(f'Android rewrite {name}', sha256(rewritten / f'{name}.patched.bin'), pins['android_rewritten_sha256'][f'{name}.patched.bin'])
    if 'android_runtime_tables_sha256' in pins:
        check('runtime-tables.bin', sha256(game_data / 'runtime-tables.bin'), pins['android_runtime_tables_sha256'])
    romfs = config_values(game_data / 'romfs-config.txt')
    check('romfs-config.txt image size', int(romfs['ROMFS_IMAGE_SIZE']), pins['romfs']['size'])
    check('romfs-config.txt image SHA-256', romfs['ROMFS_IMAGE_SHA256'], pins['romfs']['sha256'])
    check('romfs-config.txt index SHA-256', romfs['ROMFS_INDEX_SHA256'], pins['romfs_index_sha256'])


def install_manifest(game_data, rewritten, version, *, companion=False, shader_variants=0):
    """install.json (format 1, the app's setup writes the same) for a game folder this computer filled: the app starts the
    game only when it says playable. route "containers": this build reads the card image, update and keys."""
    romfs = config_values(game_data / 'romfs-config.txt')
    return {'format': 1, 'game': 'totk', 'version': version, 'route': 'containers',
            'stages': {'code': {'done': True, 'rewritten_sha256': {f'{n}.patched.bin': sha256(rewritten / f'{n}.patched.bin') for n in MODULES},
                                'runtime_tables_sha256': sha256(game_data / 'runtime-tables.bin')},
                       'romfs': {'done': True, 'size': int(romfs['ROMFS_IMAGE_SIZE']), 'sha256': romfs['ROMFS_IMAGE_SHA256']},
                       'index': {'done': True, 'sha256': romfs['ROMFS_INDEX_SHA256']},
                       'fonts': {'done': True, 'source': 'firmware', 'sha256': sha256(game_data / 'shared-fonts.txt')},
                       'companion': {'done': companion}, 'shaders': {'done': shader_variants > 0, 'variants': shader_variants}},
            'playable': True, 'setup_version': 1}


def place_game_data(game_data, rewritten, version, out):
    """out/game-data/<version> and out/install.json (written last, by rename, as the app's setup does)."""
    dst = out / f'game-data/{version}'
    dst.mkdir(parents=True, exist_ok=True)
    for name in ('runtime-tables.bin', 'romfs-config.txt', 'shared-fonts.txt'):
        shutil.copy2(game_data / name, dst / name)
    seed = out / f'shaders/{version}'
    variants = len(list(seed.glob('*.vk'))) if seed.is_dir() else 0
    manifest = install_manifest(game_data, rewritten, version, companion=(out / 'companion/index.json').is_file(), shader_variants=variants)
    tmp = out / 'install.json.tmp'
    tmp.write_text(json.dumps(manifest, indent=1) + '\n'); tmp.replace(out / 'install.json')


def companion_assets(tree, version, work, out, logs, env):
    """The dual-screen companion's drawings, made on this computer from the RomFS (on a phone the app's setup makes them):
    out/companion, pushed into the game folder with the data."""
    generator = tree / 'scripts/make_companion_assets.py'
    cache = work / f'companion-{version}'
    fingerprint = hashlib.sha256(generator.read_bytes()).hexdigest()
    stamp = work / f'companion-{version}.generator-sha256'
    if not (cache / 'companion/index.json').is_file() or not stamp.is_file() or stamp.read_text().strip() != fingerprint:
        run([tree / '.venv/bin/python', generator, '--romfs', tree / f'extracted/{version}/romfs', '--out', cache],
            logs / 'companion_assets.log', env=env)
        stamp.write_text(fingerprint + '\n')
    if (out / 'companion').exists():
        shutil.rmtree(out / 'companion')
    shutil.copytree(cache / 'companion', out / 'companion', copy_function=os.link if os.stat(cache).st_dev == os.stat(out).st_dev else shutil.copy2)


# ---- the app ----
APP_PACKAGE = 'com.aure.totk'
INSTALL_SH = r'''#!/bin/sh
# The Android app TotK Native (android/APP.md), a developer build: its game folder is the app's folder on shared storage
#   /sdcard/Android/data/{package}/files   (install.json, rewritten/, game-data/, extracted/, shaders/, companion/,
#                                            driver.so, totk.env, totk.log)
# The public APK sets this folder up on the phone; these commands fill it from this computer instead.
#   sh install_app.sh              install or update the app (keeps its data)
#   sh install_app.sh from-device  copy the game folder from /data/local/tmp/totk (the adb shell build) on the device (~17 GB, minutes)
#   sh install_app.sh data         copy the game folder and packed shader seeds from this computer (USB)
#   sh install_app.sh data-packed  same as data (explicit packed spelling)
#   sh install_app.sh data-loose   legacy loose seeds, e.g. for pack recovery
# install.json goes last: the app starts the game only once it is there and says playable.
# Packed pushes retain device loose overlays. Use a fresh shader folder to avoid scanning an old huge loose seed.
#   sh install_app.sh log          the app's log (totk.log)
#   sh install_app.sh saves        copy the app's saves (kept in its private storage) to saves-<date>.tar here
#                                  (requires a debuggable dev APK; release APKs deny run-as)
#   sh install_app.sh cache-export copy existing private Vulkan caches into cache-export-<date>/, preserving their names
#                                  (requires a debuggable dev APK; release APKs deny run-as)
#   sh install_app.sh cache-import FILE...   stage exported caches under their unchanged names (also works with release APKs)
set -e
CALLER="$PWD"   # cache-import paths are relative to where the script was called, not the output folder
cd "$(dirname "$0")"
PKG={package}
DIR=/sdcard/Android/data/$PKG/files
require_run_as() {{
  if ! adb shell run-as "$PKG" id >/dev/null 2>&1; then
    echo "error: saves/cache-export require a debuggable dev APK (run-as access was denied)." >&2
    echo "Install a matching-signed dev APK over the release APK without uninstalling to retain data; export, then reinstall the release APK if wanted." >&2
    exit 1
  fi
}}
# The runtime-produced basename carries seed/position/integer-fold identity. Never
# infer the active arm from the host, source totk.env, strip a suffix or relabel bytes.
cache_name() {{
  case "$1" in vk-pipeline-cache.bin*) ;; *) return 1 ;; esac
  suffix=${{1#vk-pipeline-cache.bin}}
  case "$suffix" in *.intfold-v1) suffix=${{suffix%.intfold-v1}} ;; esac
  case "$suffix" in
    *.pos-v1-10) suffix=${{suffix%.pos-v1-10}} ;;
    *.pos-v1-01) suffix=${{suffix%.pos-v1-01}} ;;
    *.pos-v1-11) suffix=${{suffix%.pos-v1-11}} ;;
  esac
  case "$suffix" in "") return 0 ;; .*) suffix=${{suffix#.}} ;; *) return 1 ;; esac
  [ "${{#suffix}}" -eq 64 ] || return 1
  case "$suffix" in *[!0-9a-f]*) return 1 ;; esac
}}
case "$1" in
  "") adb install -r totk.apk; echo "installed; start it from the launcher, or: adb shell monkey -p $PKG -c android.intent.category.LAUNCHER 1" ;;
  from-device)
    adb shell mkdir -p "$DIR"
    for f in rewritten game-data extracted shaders companion user-data driver.so totk.env install.json; do
      adb shell "if [ -e /data/local/tmp/totk/$f ]; then echo copying $f; cp -r /data/local/tmp/totk/$f $DIR/; fi"
    done ;;
  data|data-packed|data-loose)
    adb shell mkdir -p "$DIR"
    adb shell rm -f "$DIR/install.json"
    find rewritten game-data extracted -type d | sed "s|^|$DIR/|" | adb shell xargs mkdir -p
    adb push --sync rewritten game-data extracted "$DIR/"
    if [ -d companion ]; then adb push --sync companion "$DIR/"; fi
    if [ "$1" != data-loose ]; then
      for seed in shaders/*; do
        [ -d "$seed" ] || continue
        if [ -L "$seed" ] || [ -L "$seed/seed.vkpack" ] || [ ! -f "$seed/seed.vkpack" ]; then
          echo "error: pack $seed first with pack_shader_seed.py" >&2; exit 1
        fi
        if [ -L "$seed/seed-id.txt" ]; then echo "error: seed-id.txt must not be a symlink" >&2; exit 1; fi
        adb shell mkdir -p "$DIR/$seed"
        adb push "$seed/seed.vkpack" "$DIR/$seed/seed.vkpack.tmp"
        adb shell mv "$DIR/$seed/seed.vkpack.tmp" "$DIR/$seed/seed.vkpack"
        if [ -f "$seed/seed-id.txt" ]; then adb push --sync "$seed/seed-id.txt" "$DIR/$seed/"; fi
      done
    elif [ -d shaders ]; then
      find shaders -type d | sed "s|^|$DIR/|" | adb shell xargs mkdir -p
      adb push --sync shaders "$DIR/"
    fi
    if [ -f driver.so ]; then adb push driver.so "$DIR/"; fi
    adb push install.json "$DIR/install.json.tmp"
    adb shell mv "$DIR/install.json.tmp" "$DIR/install.json" ;;
  log) adb shell cat "$DIR/totk.log" ;;
  saves)
    require_run_as
    f="saves-$(date +%Y%m%d-%H%M%S).tar"; tmp="$f.tmp"
    if ! adb exec-out run-as "$PKG" tar cf - files/user-data > "$tmp"; then rm -f "$tmp"; exit 1; fi
    if ! mv "$tmp" "$f"; then rm -f "$tmp"; exit 1; fi
    echo "saved $f" ;;
  cache-export)
    require_run_as
    paths=$(adb exec-out run-as "$PKG" find files/state -maxdepth 1 -type f)
    folder="cache-export-$(date +%Y%m%d-%H%M%S)-$$"; found=0
    while IFS= read -r path; do
      name=${{path##*/}}
      cache_name "$name" || continue
      [ "$path" = "files/state/$name" ] || continue
      if [ "$found" = 0 ]; then mkdir "$folder"; found=1; fi
      tmp="$folder/$name.tmp"
      if ! adb exec-out run-as "$PKG" cat "$path" > "$tmp"; then
        rm -f "$tmp"; rmdir "$folder" 2>/dev/null || :; exit 1
      fi
      if ! mv "$tmp" "$folder/$name"; then
        rm -f "$tmp"; rmdir "$folder" 2>/dev/null || :; exit 1
      fi
      echo "saved $folder/$name ($(wc -c < "$folder/$name") bytes; keep this basename)"
    done <<CACHE_PATHS
$paths
CACHE_PATHS
    if [ "$found" = 0 ]; then echo "error: no transferable Vulkan pipeline caches in private files/state" >&2; exit 1; fi ;;
  cache-import)
    shift
    if [ "$#" = 0 ]; then echo "usage: sh install_app.sh cache-import FILE..."; exit 2; fi
    for file do
      case "$file" in /*) ;; *) file="$CALLER/$file" ;; esac
      name=${{file##*/}}
      if [ ! -f "$file" ] || ! cache_name "$name"; then
        echo "error: use an existing cache with its original vk-pipeline-cache.bin[namespace] basename; refusing to guess or relabel its arm: $file" >&2; exit 2
      fi
      adb push "$file" "$DIR/$name"
      echo "staged $name unchanged; only the matching runtime namespace imports it (.imported on merge, .stale on context mismatch)"
    done ;;
  *) echo "usage: sh install_app.sh [from-device|data|data-packed|data-loose|log|saves|cache-export|cache-import FILE...]"; exit 2 ;;
esac
'''


def build_app(version, ndk, work, out, logs, *, release=False, ondevice_xlate=None, turnip=None, android_sdk=None):
    """totk.apk: the same app as the public one (android/build_release.py), debuggable unless --release, signed with
    work/apk-key.pem (made on first use: a developer key, never a release key)."""
    if version != 'v143':
        raise BuildError('the app runs 1.4.3 only (pinned game data); build it with the 1.4.3 update')
    import build_release
    say('app: building totk.apk (runtime, setup library, translator, Java)')
    build_release.app(work=work, ndk=ndk, logs=logs, out=out / 'totk.apk', key=work / 'apk-key.pem', make_key=True,
                      release=release, turnip=turnip, translator=ondevice_xlate, android_sdk=android_sdk)
    (out / 'install_app.sh').write_text(INSTALL_SH.format(package=APP_PACKAGE)); (out / 'install_app.sh').chmod(0o755)


# ---- output folder ----
RUN_SH = '''#!/system/bin/sh
# Start the game from adb shell (headless: no picture or input yet; the log shows how far it boots).
#   adb shell sh /data/local/tmp/totk/run.sh          (TOTK_* variables can be set in front)
cd "$(dirname "$0")" || exit 1
mkdir -p user-data state
export TOTK_PROFILE_DIR="$PWD/user-data" TOTK_ROMFS_PROOF_DIR="$PWD/state" TOTK_SHARED_FONTS="$PWD/extracted/firmware/shared-fonts"
export TOTK_GAME_DATA="${{TOTK_GAME_DATA:-$PWD/game-data/{version}}}"   # module tables, RomFS and font hashes (runtime/game_data.h)
export TOTK_OPERATION_MODE="${{TOTK_OPERATION_MODE:-handheld}}" TOTK_ROMFS_MMAP="${{TOTK_ROMFS_MMAP:-1}}"
# Vulkan driver: driver.so in this folder (e.g. a Turnip build) when present, else the system one.
# TOTK_VK_DRIVER=/path/to/driver.so picks another file, TOTK_VK_DRIVER=system the system driver.
if [ -z "$TOTK_VK_DRIVER" ] && [ -f driver.so ]; then export TOTK_VK_DRIVER="$PWD/driver.so"; fi
if [ -x totk_xlate ]; then
  mkdir -p shaders/{version}
  if [ -z "$TOTK_SHADER_TRANSLATOR" ]; then export TOTK_SHADER_TRANSLATOR="$PWD/totk_xlate"; fi
fi
if [ -d shaders/{version} ]; then export TOTK_SHADER_SEED_DIR="$PWD/shaders/{version}"; fi
exec ./totk-native rewritten/{version} boot 2>&1 | tee totk.log
'''
PUSH_SH = '''#!/bin/sh
# Copy this folder to the device (about {gb:.0f} GB): sh push.sh [--packed|--loose] [device folder]
# After build_android.py --runtime-only: sh push.sh runtime (also run.sh, the game data folder and translator)
# Seeds default to pack-only transfer; --loose is for recovery/legacy installations.
# Pack replacement is atomic; stop the app before deployment and restart to select the new inode.
# Use a fresh destination to avoid scanning old loose files. Device overlays are never deleted.
set -e
cd "$(dirname "$0")"
PACKED=1
if [ "$1" = --packed ]; then shift; elif [ "$1" = --loose ]; then PACKED=0; shift; fi
DEST=/data/local/tmp/totk
push_shaders() {{
  [ -d shaders ] || return 0
  if [ "$PACKED" = 0 ]; then adb push --sync shaders "$DEST/"; return; fi
  for seed in shaders/*; do
    [ -d "$seed" ] || continue
    if [ -L "$seed" ] || [ -L "$seed/seed.vkpack" ] || [ ! -f "$seed/seed.vkpack" ]; then
      echo "error: pack $seed first with pack_shader_seed.py" >&2; exit 1
    fi
    if [ -L "$seed/seed-id.txt" ]; then echo "error: seed-id.txt must not be a symlink" >&2; exit 1; fi
    adb shell mkdir -p "$DEST/$seed"
    adb push "$seed/seed.vkpack" "$DEST/$seed/seed.vkpack.tmp"
    adb shell mv "$DEST/$seed/seed.vkpack.tmp" "$DEST/$seed/seed.vkpack"
    if [ -f "$seed/seed-id.txt" ]; then adb push --sync "$seed/seed-id.txt" "$DEST/$seed/"; fi
  done
}}
if [ "$1" = runtime ]; then adb push totk-native run.sh "$DEST/"; adb push --sync game-data "$DEST/"
elif [ "$1" = shaders ]; then adb push totk-native run.sh "$DEST/"; adb push --sync game-data "$DEST/"; push_shaders; else
  [ -n "$1" ] && DEST="$1"
  adb shell mkdir -p "$DEST"
  adb push --sync totk-native run.sh rewritten game-data extracted "$DEST/"
  if [ -d companion ]; then adb push --sync companion "$DEST/"; fi
  adb push --sync install.json "$DEST/"
  push_shaders
fi
if [ -f totk_xlate ]; then
  adb push totk_xlate "$DEST/"
  adb shell chmod 755 "$DEST/totk_xlate"
fi
adb shell chmod 755 "$DEST/totk-native" "$DEST/run.sh"
echo "now: adb shell sh $DEST/run.sh"
'''


# ---- shaders: the Mac's seed preparation, keeping the Vulkan SPIR-V ----
def build_spirv_cross_linux(tree, logs):
    """SPIRV-Cross with the host C++ compiler (build_app.build_spirv_cross uses Xcode's)."""
    src, out = tree / 'tooling/spirv-cross', tree / 'build/spirv-cross'
    cxx = shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        raise BuildError('SPIRV-Cross needs clang++ or g++')
    flags = list(mac.SPIRV_CROSS_FLAGS)
    if '-arch' in flags:
        i = flags.index('-arch'); del flags[i:i + 2]
    objs = out / 'obj'; objs.mkdir(parents=True, exist_ok=True)
    import concurrent.futures
    def one(name):
        run([cxx, *flags, '-c', src / name, '-o', objs / (name + '.o')], logs / 'spirv-cross' / (name + '.log'), quiet=False)
    (logs / 'spirv-cross').mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        list(pool.map(one, mac.SPIRV_CROSS_SOURCES))
    run([cxx, *[objs / (n + '.o') for n in mac.SPIRV_CROSS_SOURCES], '-o', out / 'spirv-cross'], logs / 'spirv-cross' / 'link.log', quiet=False)


def translator_inputs_hash(source):
    """Hash the inputs actually consumed by publish_native.sh, not the possibly stale prebuilt binary."""
    h = hashlib.sha256()
    src = source / 'tools/totk_xlate'
    inputs = list(src.glob('*.cs')) + list(src.glob('*.csproj')) + list(src.glob('*.sh')) + list(src.glob('*.py'))
    inputs += list((src / 'patches').glob('*.patch'))
    vendor = source.parents[1] / 'vendor/ryujinx'
    inputs += [p for p in vendor.rglob('*') if p.is_file() and p.suffix in ('.cs', '.csproj', '.props', '.targets')
               and not {'bin', 'obj'}.intersection(p.relative_to(vendor).parts)]
    for p in sorted(inputs):
        h.update(str(p.relative_to(source.parents[1])).encode())
        h.update(b'\0'); h.update(p.read_bytes()); h.update(b'\0')
    return h.hexdigest()


def translator_path(args, tree, logs):
    # An explicit translator is caller-owned; never rebuild it or require our input stamp.
    if args.translator:
        t = args.translator.expanduser().resolve()
        if not t.is_file():
            raise BuildError(f'no shader translator at {t}')
        return t
    import platform
    platform_name = f"linux-{'arm64' if platform.machine() in ('aarch64', 'arm64') else 'x64'}"
    t = tree / f'prebuilt/{platform_name}/totk_xlate'
    source = mac.REPO / 'games/totk'
    script = source / 'tools/totk_xlate/publish_native.sh'
    stamp = t.parent / 'totk_xlate.inputs.sha256'
    # Shipped prebuilts have a binary checksum but no local input stamp; they must not require an SDK.
    if t.is_file() and not stamp.is_file():
        return t
    want = translator_inputs_hash(source)
    identity = want + '\n' + (sha256(t) if t.is_file() else '')
    if not stamp.is_file() or stamp.read_text().strip() != identity:
        if not script.is_file() or not shutil.which('dotnet'):
            raise BuildError('shader translator missing or stale; rebuild with '
                             'sh games/totk/tools/totk_xlate/publish_native.sh '
                             '(needs .NET 10 SDK, clang and zlib), or pass --translator explicitly')
        say('shaders: rebuilding stale translator from the source checkout (publish_native.sh)')
        run(['sh', script], logs / 'publish-native-translator.log', cwd=source, quiet=False)
        fresh = source / f'prebuilt/{platform_name}/totk_xlate'
        t.parent.mkdir(parents=True, exist_ok=True)
        if fresh.resolve() != t.resolve():
            shutil.copy2(fresh, t)
            shutil.copy2(fresh.with_suffix('.sha256'), t.with_suffix('.sha256'))
        stamp.write_text(want + '\n' + sha256(t) + '\n')
    return t


def prepare_ondevice_translator(tree, ndk, api, out, logs):
    """Reuse a shipped Android prebuilt, or keep a work-local publish current; stage it for adb/APK."""
    binary = tree / 'prebuilt/android-arm64/totk_xlate'
    stamp = binary.parent / 'totk_xlate.inputs.sha256'
    source = mac.REPO / 'games/totk'
    if not binary.is_file() or stamp.is_file():
        want = translator_inputs_hash(source) + f'\napi={api}'
        identity = want + '\n' + (sha256(binary) if binary.is_file() else '')
        if not stamp.is_file() or stamp.read_text().strip() != identity:
            script = source / 'tools/totk_xlate/publish_native.sh'
            dotnet = os.environ.get('DOTNET', 'dotnet')
            if not script.is_file() or not shutil.which(dotnet):
                raise BuildError('--ondevice-xlate: Android translator missing or stale; publishing needs the .NET 10 SDK '
                                 '(dotnet on PATH or set DOTNET) and the Android NDK. Publish with '
                                 'sh games/totk/tools/totk_xlate/publish_native.sh --android --ndk PATH')
            say('shaders: publishing the on-device Android translator (NativeAOT, no device .NET runtime)')
            run(['sh', script, '--android', '--ndk', ndk, '--api', str(api), '--dotnet', dotnet, '--out', binary.parent],
                logs / 'publish-android-translator.log', cwd=source, quiet=False)
            if not binary.is_file():
                raise BuildError(f'--ondevice-xlate: publishing did not produce {binary}')
            stamp.write_text(want + '\n' + sha256(binary) + '\n')
    out.mkdir(parents=True, exist_ok=True)
    staged = out / 'totk_xlate'
    if binary.resolve() != staged.resolve():
        shutil.copy2(binary, staged)
    staged.chmod(0o755)
    return staged


def step_shaders(args, tree, version, logs, existing_seed=None):
    """Every shader of the game for its default bind state and every bind state of the shared list, as SPIR-V for
    Vulkan with the reflection the runtime uses (scripts/prepare_shader_seed.py --keep-spirv). Reused when unchanged."""
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        say('shaders: building SPIRV-Cross')
        build_spirv_cross_linux(tree, logs)
    translator = translator_path(args, tree, logs)
    seed = tree / f'build/shader-seed-android/{version}'
    say('shaders: translating the game\'s shaders for Vulkan (about an hour the first time, then reused)')
    run([tree / '.venv/bin/python', 'scripts/prepare_shader_seed.py', '--romfs', tree / f'extracted/{version}/romfs',
         '--output', seed, '--translator', translator, '--spirv-cross', tree / 'build/spirv-cross/spirv-cross',
         '--version', version, '--modules', tree / f'rewritten/{version}', '--keep-spirv',
         *(['--retain-vulkan-seed', existing_seed] if existing_seed and existing_seed.is_dir() else [])],
        logs / f'prepare-shader-seed-android-{version}.log', cwd=tree, quiet=False)
    return seed


def place_shaders(seed, out, version):
    """The seed for the device in out/shaders/<version>: each variant's SPIR-V (hard links) with its Vulkan reflection
    (<variant>.vk) and each shader's variant list (<key>.vv), made by android/vk_reflect.py. The MSL and the Mac
    reflection stay in the work folder."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import vk_reflect
    from prepare_shader_seed import retained_vulkan_requests
    dst = out / f'shaders/{version}'
    previous = retained_vulkan_requests(dst)
    manifest = json.loads((seed / 'seed-manifest.json').read_text())
    scope = set(json.loads((seed / 'shader-keys.json').read_text())['keys'])
    staging = dst.with_name(f'{version}.refresh-{os.getpid()}')
    staging.mkdir(parents=True)
    try:
        n, bad, unknown = vk_reflect.convert(seed, staging)
        if bad:
            raise BuildError(f'shaders: {bad} incomplete regenerated variants')
        for f in seed.glob('*.spv'):
            try: os.link(f, staging / f.name)
            except OSError: shutil.copy2(f, staging / f.name)
        retained = 0
        for variant, request in previous.items():
            if request['key'] not in scope:
                continue
            for extension, digest_key in (('.spv', 'spv_sha256'), ('.vk', 'vk_sha256')):
                payload = staging / (variant + extension)
                if not payload.is_file():
                    raise BuildError(f'shaders: retained variant was not regenerated: {variant}')
                if sha256(payload) != request[digest_key]:
                    raise BuildError(f'shaders: retained name has different regenerated content: {payload.name}')
            retained += 1
        final = retained_vulkan_requests(staging)   # Every .vv row has both payloads; no dangling names.
        report = {field: manifest[field] for field in ('schema', 'chain', 'version', 'game', 'translator_sha256',
                                                       'source_romfs_sha256', 'preparation_sha256', 'shader_list_sha256')}
        report.update(variants=len(final), retained_variants=retained,
                      retained_outside_inventory=sorted(set(previous) - set(final)),
                      files={p.name: sha256(p) for p in sorted(staging.iterdir())})
        (staging / 'seed-stage-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        marker = seed / 'seed-id.txt'
        if marker.is_file():
            shutil.copy2(marker, staging / marker.name)
        if dst.exists():
            shutil.rmtree(dst)
        staging.rename(dst)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    say(f'shaders: {n} variants reflected for Vulkan into {dst}' + (f', {bad} incomplete' if bad else ''))
    for k, c in sorted(unknown.items(), key=lambda kv: -kv[1])[:10]:
        say(f'  unclassified descriptor {k}: {c} variants (bound as empty)')


def pack_shaders(out, version):
    """Pack an already staged seed, preserving loose payloads and its optional namespace marker."""
    seed = out / f'shaders/{version}'
    if not seed.is_dir():
        raise BuildError('--packed-shader-seed needs a staged seed; use --shaders or populate ' + str(seed))
    try:
        packed = pack_seed(seed)
    except (OSError, ValueError) as error:
        raise BuildError(f'--packed-shader-seed: {error}') from error
    say(f'shaders: packed {seed} into {packed}; push with --packed (loose files kept)')


def assemble(tree, version, rewritten, binary, game_data, out):
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, out / 'totk-native')
    copy_rewritten(rewritten, version, out)
    def place(src, rel):   # hard links: no second 17 GB copy on the same file system
        d = out / rel; d.parent.mkdir(parents=True, exist_ok=True)
        if d.exists(): d.unlink()
        try: os.link(src, d)
        except OSError: shutil.copy2(src, d)
    romfs = tree / f'extracted/{version}/romfs'
    for f in ('romfs.bin', 'runtime-index.bin'):
        place(romfs / f, f'extracted/{version}/romfs/{f}')
    for folder in (f'extracted/{version}/romfs-patches', 'extracted/firmware/shared-fonts'):
        for f in (tree / folder).rglob('*'):
            if f.is_file(): place(f, f'{folder}/{f.relative_to(tree / folder)}')
    place_game_data(game_data, rewritten, version, out)
    return write_scripts(version, out)


def copy_rewritten(rewritten, version, out):
    dst = out / f'rewritten/{version}'
    dst.mkdir(parents=True, exist_ok=True)
    for n in MODULES:
        shutil.copy2(rewritten / f'{n}.patched.bin', dst / f'{n}.patched.bin')


def write_scripts(version, out):
    gb = sum(f.stat().st_size for f in out.rglob('*') if f.is_file()) / 2**30
    (out / 'run.sh').write_text(RUN_SH.format(version=version)); (out / 'run.sh').chmod(0o755)
    (out / 'push.sh').write_text(PUSH_SH.format(gb=gb)); (out / 'push.sh').chmod(0o755)
    return gb


def build_runtime(tree, version, ndk, work, logs, env, args):
    """totk-native for adb shell (android/Makefile; no game files needed: the game data folder is read at start)."""
    opus = build_opus(work, ndk, args.api, logs)
    vpx = None if args.no_vpx else build_vpx(work, ndk, args.api, logs)
    gen = tree / f'build/android-{version}'
    say(f'runtime: building totk-native ({version}, GPU {args.gpu}) with the NDK')
    make = ['make', '-C', tree / 'android', 'TARGET=android', f'NDK={ndk}', f'API={args.api}', f'VERSION={version}',
            f'OUT={gen}', f'OPUS={opus}', *([f'VPX={vpx}'] if vpx else []), f'GPU={args.gpu}', f'PYTHON={sys.executable}',
            f'DEV_BUILD={0 if args.release else 1}', f'-j{os.cpu_count() or 4}']
    run(make, logs / f'make_android_{version}.log', env=env)
    return gen / 'totk-native'


def main(argv):
    args = parse_args(argv)
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    for p in (game, keys):
        if not p.is_file():
            raise BuildError(f'no such file: {p}')
    if mac.input_kind(game) not in ('xci', 'nsp'):
        raise BuildError('the base game must be an uncompressed XCI or NSP; NSZ is supported for update packages')
    update = args.update.expanduser().resolve() if args.update else None
    work = (args.work.expanduser().resolve() if args.work else game.parent / 'TOTK_build')
    out = (args.out.expanduser().resolve() if args.out else game.parent / 'TOTK_android')
    firmware = args.firmware.expanduser().resolve() if args.firmware else None
    shared_fonts = args.shared_fonts.expanduser().resolve() if args.shared_fonts else None
    turnip = args.turnip.expanduser().resolve() if args.turnip else None
    if turnip and not args.apk:
        raise BuildError('--turnip requires --apk')
    py, llvm = prerequisites(args)
    ndk = Path(args.ndk).expanduser().resolve()
    logs = work / 'logs'
    env = dict(os.environ)
    t0 = time.time()
    tree = mac.prepare_work(work, py, logs)
    ondevice_xlate = prepare_ondevice_translator(tree, ndk, args.api, out, logs) if args.ondevice_xlate else None
    if args.runtime_only:
        return runtime_only(args, tree, work, out, ndk, logs, env, t0, ondevice_xlate=ondevice_xlate, turnip=turnip)
    if mac.identify_game(tree, game, keys, logs) != 'totk':
        raise BuildError('the Android build is for Tears of the Kingdom only')
    version = 'v0'
    if update:
        version, update = mac.identify_update(tree, update, keys, logs)
    say('1/4 code: extracting and checking the code modules (the verified Apple-silicon rewrite proves the dump)')
    mac.step_code(tree, game, keys, env, logs)
    if update:
        mac.step_code(tree, update, keys, env, logs, version=version)
    rewritten = android_rewrite(tree, version, env, logs, llvm, equiv=args.equiv)
    say('3/4 data: RomFS, shared fonts and overlays')
    mac.step_data(tree, game, keys, logs, firmware=firmware, shared_fonts=shared_fonts)
    if update:
        mac.step_update_data(tree, game, update, keys, logs, version=version)
    seed = step_shaders(args, tree, version, logs, out / f'shaders/{version}') if args.shaders else None
    game_data = write_game_data(tree, version, rewritten, logs)
    say('4/4 runtime: building totk-native with the NDK')
    binary = build_runtime(tree, version, ndk, work, logs, env, args)
    if seed:
        place_shaders(seed, out, version)
    if args.packed_shader_seed:
        pack_shaders(out, version)
    if args.apk and not args.no_companion:
        companion_assets(tree, version, work, out, logs, env)
    gb = assemble(tree, version, rewritten, binary, game_data, out)
    if args.apk:
        build_app(version, ndk, work, out, logs, release=args.release, ondevice_xlate=ondevice_xlate, turnip=turnip,
                  android_sdk=args.android_sdk)
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out} ({gb:.0f} GB)')
    say('Connect the device with USB debugging on, then: sh push.sh   and   adb shell sh /data/local/tmp/totk/run.sh')


def runtime_only(args, tree, work, out, ndk, logs, env, t0, *, ondevice_xlate=None, turnip=None):
    """Rebuild totk-native (and with --apk the app) from the current sources, and the game data folder and install.json
    from the Android rewrite of an earlier full build; the large data already in the output folder or on the device stays."""
    reports = sorted((tree / 'rewritten-android').glob('*/phase12-validation.json'), key=lambda p: p.stat().st_mtime)
    if not reports:
        raise BuildError('--runtime-only needs a full build first (no rewritten-android/<version> in the work folder)')
    rewritten = reports[-1].parent; version = rewritten.name
    game_data = write_game_data(tree, version, rewritten, logs)
    if not (out / 'push.sh').is_file():
        raise BuildError(f'--runtime-only: no earlier output folder at {out}')
    if args.shaders:
        place_shaders(step_shaders(args, tree, version, logs, out / f'shaders/{version}'), out, version)
    if args.packed_shader_seed:
        pack_shaders(out, version)
    shutil.copy2(build_runtime(tree, version, ndk, work, logs, env, args), out / 'totk-native')
    copy_rewritten(rewritten, version, out)
    if args.apk and not args.no_companion:
        companion_assets(tree, version, work, out, logs, env)
    place_game_data(game_data, rewritten, version, out)
    if args.apk:
        build_app(version, ndk, work, out, logs, release=args.release, ondevice_xlate=ondevice_xlate, turnip=turnip,
                  android_sdk=args.android_sdk)
    write_scripts(version, out)
    say(f'done in {time.time() - t0:.0f} s: {out / "totk-native"}, {out / "game-data" / version}. Then: sh {out / "push.sh"} {"shaders" if args.shaders else "runtime"}')
    if args.apk:
        say(f'app: {out / "totk.apk"}. Install: sh {out / "install_app.sh"}   (the game folder once: sh {out / "install_app.sh"} data)')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nbuild stopped: {e}', file=sys.stderr)
        sys.exit(1)

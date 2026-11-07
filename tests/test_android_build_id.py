"""Runtime link policy and built ELF identity for native profiler symbol matching."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('build_id_elf', ROOT / 'tools/bench/cpuprof_report.py')
assert SPEC is not None and SPEC.loader is not None
elf = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(elf)
NDK = Path(os.environ.get('ANDROID_NDK_HOME', '/home/agent/Android/Sdk/ndk/28.2.13676358'))
CLANG = NDK / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang'


class BuildIdentity(unittest.TestCase):
    def test_runtime_link_requests_sha1_and_relinks_on_recipe_change(self):
        make = (ROOT / 'games/totk/android/Makefile').read_text().splitlines()
        start = next(i for i, line in enumerate(make) if line.startswith('$(OUT)/libtotk.so:'))
        self.assertIn('$(TOTK)/android/Makefile', make[start])
        link = next(line for line in make[start + 1:] if '$(CC) -shared ' in line)
        self.assertIn('-Wl,--build-id=sha1', link.split())

    @unittest.skipUnless(CLANG.is_file(), 'Android NDK clang not available')
    def test_linked_library_has_twenty_byte_build_id_preserved_by_strip(self):
        with tempfile.TemporaryDirectory() as tmp:
            lib = Path(tmp) / 'libtotk.so'
            subprocess.run([str(CLANG), '-shared', '-nostdlib', '-g', '-Wl,--build-id=sha1',
                            '-x', 'c', '-', '-o', str(lib)], input=b'int identity_fixture(void) { return 7; }',
                           capture_output=True, check=True)
            full = elf.read_elf_identity(lib)
            self.assertRegex(full['build_id'], r'^[0-9a-f]{40}$')
            self.assertTrue(full['has_debug_info'])
            shipped = Path(tmp) / 'stripped.so'
            subprocess.run([str(CLANG.with_name('llvm-strip')), '--strip-debug', '-o', str(shipped), str(lib)],
                           capture_output=True, check=True)
            self.assertEqual(elf.read_elf_identity(shipped)['build_id'], full['build_id'])

    @unittest.skipUnless(os.environ.get('TOTK_RELEASE_APK'), 'set TOTK_RELEASE_APK to check built libtotk.so')
    def test_built_release_libtotk_has_build_id(self):
        with tempfile.TemporaryDirectory() as tmp, zipfile.ZipFile(os.environ['TOTK_RELEASE_APK']) as apk:
            lib = Path(tmp) / 'libtotk.so'
            lib.write_bytes(apk.read('lib/arm64-v8a/libtotk.so'))
            self.assertRegex(elf.read_elf_identity(lib)['build_id'], r'^[0-9a-f]{40}$')


if __name__ == '__main__':
    unittest.main()

"""Behavioral tests of generated install_app.sh using mocked adb and local files.

These exercise transfer/namespacing, not Vulkan context validation or APK builds.
"""
import ast
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'games/totk/android'
PACKAGE = 'com.aure.totk'
EXTERNAL = '/sdcard/Android/data/' + PACKAGE + '/files'
BASE = 'vk-pipeline-cache.bin'
# The app context envelope and opaque driver bytes must be copied without changes.
PAYLOAD = b'TOTK context envelope fixture\x00\r\n\xff' + bytes(range(256))

FAKE_ADB = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

args = sys.argv[1:]
with open(os.environ['ADB_CALLS'], 'a') as log:
    log.write(json.dumps(args) + '\n')
if args == ['shell', 'run-as', 'com.aure.totk', 'id']:
    sys.exit(1 if os.environ.get('REJECT_RUN_AS') else 0)
if args[:3] == ['exec-out', 'run-as', 'com.aure.totk']:
    operation = args[3:]
    if operation == ['find', 'files/state', '-maxdepth', '1', '-type', 'f']:
        result = subprocess.run(operation, cwd=os.environ['DEVICE_PRIVATE'], capture_output=True)
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        if os.environ.get('EXTRA_FIND_LINES'):
            print(os.environ['EXTRA_FIND_LINES'])
        sys.exit(result.returncode)
    if len(operation) == 2 and operation[0] == 'cat':
        if os.environ.get('FAIL_DEVICE_READ') == operation[1]:
            sys.stdout.buffer.write(b'partial bytes')
            print('injected cache read failure', file=sys.stderr)
            sys.exit(7)
        try:
            sys.stdout.buffer.write((Path(os.environ['DEVICE_PRIVATE']) / operation[1]).read_bytes())
        except OSError as error:
            print(error, file=sys.stderr)
            sys.exit(1)
        sys.exit(0)
if len(args) == 3 and args[0] == 'push':
    if os.environ.get('FAIL_PUSH'):
        print('injected push failure', file=sys.stderr)
        sys.exit(8)
    target = Path(os.environ['DEVICE_ROOT']) / args[2].lstrip('/')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args[1], target)
    sys.exit(0)
print('unexpected adb invocation: ' + repr(args), file=sys.stderr)
sys.exit(90)
'''


def generated_installer():
    # Parse the literal, without importing/running the build entrypoint.
    tree = ast.parse((ANDROID / 'build_android.py').read_text())
    install = next(node.value for node in tree.body if isinstance(node, ast.Assign)
                   and any(isinstance(name, ast.Name) and name.id == 'INSTALL_SH'
                           for name in node.targets))
    return ast.literal_eval(install).format(package=PACKAGE)


class AndroidCacheTransferTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='android-cache-transfer-')
        self.addCleanup(temporary.cleanup)
        self.temp = Path(temporary.name)
        self.output = self.temp / 'output'
        self.output.mkdir()
        self.device = self.temp / 'device'
        self.private = self.device / 'private'
        (self.private / 'files/state').mkdir(parents=True)
        self.script = self.output / 'install_app.sh'
        self.script.write_text(generated_installer())
        tools = self.temp / 'tools'
        tools.mkdir()
        adb = tools / 'adb'
        adb.write_text(FAKE_ADB)
        adb.chmod(0o755)
        self.calls = self.temp / 'adb-calls.jsonl'
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                        DEVICE_ROOT=str(self.device), DEVICE_PRIVATE=str(self.private),
                        ADB_CALLS=str(self.calls))
        for key in ('REJECT_RUN_AS', 'FAIL_DEVICE_READ', 'EXTRA_FIND_LINES', 'FAIL_PUSH'):
            self.env.pop(key, None)

    def cached(self, name, data=PAYLOAD):
        target = self.private / 'files/state' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def staged(self, name):
        return self.device / EXTERNAL.lstrip('/') / name

    def invoke(self, command, *args, env=None, cwd=None):
        return subprocess.run(['sh', str(self.script), command, *map(str, args)],
                              cwd=cwd or self.output, env=dict(self.env, **(env or dict())),
                              capture_output=True, text=True)

    def export(self, env=None):
        before = set(self.output.glob('cache-export-*'))
        result = self.invoke('cache-export', env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        created = set(self.output.glob('cache-export-*')) - before
        self.assertEqual(len(created), 1)
        folder = created.pop()
        self.assertTrue(folder.is_dir())
        self.assertFalse(list(folder.glob('*.tmp')))
        return folder, result

    def recorded_calls(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def test_fresh_default_exports_and_imports_intfold_basename(self):
        name = BASE + '.intfold-v1'
        self.cached(name)
        folder, result = self.export()
        self.assertEqual([path.name for path in folder.iterdir()], [name])
        self.assertEqual((folder / name).read_bytes(), PAYLOAD)
        self.assertIn(name, result.stdout)
        imported = self.invoke('cache-import', folder / name)
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        self.assertEqual(self.staged(name).read_bytes(), PAYLOAD)
        self.assertFalse(self.staged(BASE).exists())
        self.assertIn('only the matching runtime namespace imports it', imported.stdout)

    def test_exact_zero_legacy_file_stays_legacy(self):
        self.cached(BASE)
        folder, _ = self.export(env=dict(TOTK_VK_INT_FOLD='0'))
        self.assertEqual((folder / BASE).read_bytes(), PAYLOAD)
        imported = self.invoke('cache-import', folder / BASE, env=dict(TOTK_VK_INT_FOLD='0'))
        self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
        self.assertEqual(self.staged(BASE).read_bytes(), PAYLOAD)
        self.assertFalse(self.staged(BASE + '.intfold-v1').exists())

    def test_both_arms_are_exported_and_imported_without_relabeling(self):
        on, off = BASE + '.intfold-v1', BASE
        self.cached(on, PAYLOAD + b'on')
        self.cached(off, PAYLOAD + b'off')
        folder, _ = self.export()
        self.assertEqual({path.name for path in folder.iterdir()}, {on, off})
        result = self.invoke('cache-import', folder / off, folder / on)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.staged(on).read_bytes(), PAYLOAD + b'on')
        self.assertEqual(self.staged(off).read_bytes(), PAYLOAD + b'off')
        pushes = [call for call in self.recorded_calls() if call[0] == 'push']
        self.assertEqual([call[2] for call in pushes], [EXTERNAL + '/' + off, EXTERNAL + '/' + on])

    def test_opposite_arm_is_never_promoted_to_current_namespace(self):
        # Transfer deliberately does not guess the phone's active arm. The runtime
        # refuses consumption of the opposite basename; staging cannot rename it.
        for source, opposite, host_arm in ((BASE, BASE + '.intfold-v1', '1'),
                                           (BASE + '.intfold-v1', BASE, '0')):
            with self.subTest(source=source):
                artifact = self.output / source
                artifact.write_bytes(PAYLOAD)
                self.staged(opposite).unlink(missing_ok=True)
                result = self.invoke('cache-import', artifact, env=dict(TOTK_VK_INT_FOLD=host_arm))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.staged(source).read_bytes(), PAYLOAD)
                self.assertFalse(self.staged(opposite).exists())

    def test_position_and_seed_namespaces_are_preserved_exactly(self):
        names = set()
        for seed in ('', '.' + '0123456789abcdef' * 4):
            for position in ('', '.pos-v1-10', '.pos-v1-01', '.pos-v1-11'):
                for fold in ('', '.intfold-v1'):
                    name = BASE + seed + position + fold
                    names.add(name)
                    self.cached(name, PAYLOAD + name.encode())
        folder, _ = self.export()
        self.assertEqual({path.name for path in folder.iterdir()}, names)
        result = self.invoke('cache-import', *(folder / name for name in sorted(names)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in names:
            self.assertEqual((folder / name).read_bytes(), PAYLOAD + name.encode())
            self.assertEqual(self.staged(name).read_bytes(), PAYLOAD + name.encode())

    def test_noncache_and_unproven_names_are_not_exported_or_imported(self):
        invalid = [BASE + suffix for suffix in (
            '.tmp', '.stale', '.imported', '.pso-keys', '.pso-state', '.intfold-v1.tmp',
            '.intfold-v1.stale', '.intfold-v1.imported', '.intfold-v1.pso-keys',
            '.intfold-v1.pso-state', '.pos-v1-00', '.pos-v1-10.pos-v1-01',
            '.intfold-v1.pos-v1-10', '.intfold-v1.intfold-v1',
            '.' + 'a' * 63, '.' + 'a' * 65, '.' + 'A' * 64,
            '.' + 'g' * 64, '.unrecognized')]
        invalid += ['vk-pipeline-cache-20260101-000000.bin', 'custom-cache.bin', 'unrelated']
        self.cached(BASE + '.intfold-v1')
        for name in invalid:
            self.cached(name)
        self.cached('nested/' + BASE)
        folder, _ = self.export()
        self.assertEqual({path.name for path in folder.iterdir()}, {BASE + '.intfold-v1'})
        for name in invalid:
            with self.subTest(name=name):
                source = self.output / name
                source.write_bytes(PAYLOAD)
                result = self.invoke('cache-import', source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('refusing to guess or relabel its arm', result.stderr)
                self.assertFalse(self.staged(name).exists())

    def test_import_still_works_for_release_without_run_as(self):
        source = self.output / (BASE + '.intfold-v1')
        source.write_bytes(PAYLOAD)
        result = self.invoke('cache-import', source, env=dict(REJECT_RUN_AS='1'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.staged(source.name).read_bytes(), PAYLOAD)
        self.assertEqual(self.recorded_calls(), [['push', str(source), EXTERNAL + '/' + source.name]])

    def test_import_paths_are_relative_to_callers_directory(self):
        caller = self.temp / 'caller with spaces'
        caller.mkdir()
        source = caller / (BASE + '.intfold-v1')
        source.write_bytes(PAYLOAD)
        result = self.invoke('cache-import', source.name, cwd=caller)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.staged(source.name).read_bytes(), PAYLOAD)

    def test_environment_files_are_never_evaluated_or_used_to_relabel(self):
        marker = self.temp / 'ENV_WAS_EXECUTED'
        poison = 'TOTK_VK_INT_FOLD=0\nUNRELATED=$(touch "%s")\n' % marker
        (self.output / 'totk.env').write_text(poison)
        remote_env = self.staged('totk.env')
        remote_env.parent.mkdir(parents=True)
        remote_env.write_text(poison)
        self.cached(BASE + '.intfold-v1')
        folder, _ = self.export(env=dict(TOTK_VK_INT_FOLD='0', TOTK_VK_POS_INVARIANT='1',
                                        TOTK_SHADER_SEED_DIR='/host-only'))
        result = self.invoke('cache-import', folder / (BASE + '.intfold-v1'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(self.staged(BASE).exists())
        self.assertFalse(any('totk.env' in ' '.join(call) for call in self.recorded_calls()))

    def test_find_output_cannot_export_another_path_or_execute_shell(self):
        self.cached(BASE + '.intfold-v1')
        marker = self.temp / 'FIND_WAS_EXECUTED'
        injected = 'elsewhere/%s\nfiles/state/../%s\n$(touch "%s")' % (BASE, BASE, marker)
        folder, _ = self.export(env=dict(EXTRA_FIND_LINES=injected))
        self.assertEqual({path.name for path in folder.iterdir()}, {BASE + '.intfold-v1'})
        self.assertFalse(marker.exists())
        reads = [call[-1] for call in self.recorded_calls() if call[:4] ==
                 ['exec-out', 'run-as', PACKAGE, 'cat']]
        self.assertEqual(reads, ['files/state/' + BASE + '.intfold-v1'])

    def test_export_run_as_preflight_and_read_failures_leave_no_empty_backup(self):
        self.cached(BASE + '.intfold-v1')
        for extra, reason in ((dict(REJECT_RUN_AS='1'), 'debuggable dev APK'),
                              (dict(FAIL_DEVICE_READ='files/state/' + BASE + '.intfold-v1'),
                               'injected cache read failure')):
            with self.subTest(extra=extra):
                result = self.invoke('cache-export', env=extra)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(reason, result.stderr)
                self.assertFalse(list(self.output.glob('cache-export-*')))
                self.assertFalse(list(self.output.rglob('*.tmp')))

    def test_export_with_no_matching_caches_does_not_create_backup(self):
        self.cached(BASE + '.stale')
        result = self.invoke('cache-export')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no transferable Vulkan pipeline caches', result.stderr)
        self.assertFalse(list(self.output.glob('cache-export-*')))

    def test_import_requires_files_and_propagates_adb_failure(self):
        result = self.invoke('cache-import')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('usage:', result.stdout)
        source = self.output / (BASE + '.intfold-v1')
        result = self.invoke('cache-import', source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('original vk-pipeline-cache.bin', result.stderr)
        source.write_bytes(PAYLOAD)
        result = self.invoke('cache-import', source, env=dict(FAIL_PUSH='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('injected push failure', result.stderr)
        self.assertFalse(self.staged(source.name).exists())
        self.assertNotIn('staged ', result.stdout)


if __name__ == '__main__':
    unittest.main()

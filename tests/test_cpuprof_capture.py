"""CPU capture staging and restore paths without adb or device controls."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('cpuprof_capture', ROOT / 'tools/bench/cpuprof_capture.py')
assert SPEC is not None and SPEC.loader is not None
cap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cap)


class Capture(unittest.TestCase):
    def test_staging_is_shell_accessible_then_root_copied(self):
        args = SimpleNamespace(serial='device', ndk_simpleperf=Path('/ndk'))
        state = {'remote': '/data/local/tmp/totk-cpuprof-' + 'a' * 32}
        with patch.object(cap, 'run') as run, patch.object(cap, 'root') as root:
            cap.deploy(args, state, Path('/host/capture.sh'))
        pushes = [call.args[1] for call in run.call_args_list]
        self.assertEqual(len(pushes), 2)
        for cmd in pushes:
            self.assertEqual(cmd[:4], ['adb', '-s', 'device', 'push'])
            self.assertTrue(cmd[-1].startswith('/data/local/tmp/'))
            self.assertNotEqual(Path(cmd[-1]).parent.as_posix(), state['remote'])
        for call in root.call_args_list:
            self.assertIn('cp /data/local/tmp/', call.args[1])
            self.assertIn('chmod 700', call.args[1])
            self.assertIn('rm -f', call.args[1])

    def exercise_device_trap(self, perf_status):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            proc = base / 'sysctl'
            proc.mkdir()
            remote = base / 'remote'
            remote.mkdir()
            props = base / 'properties'
            props.mkdir()
            values = dict(zip(cap.CONTROLS, ('3', '100000', '25', '516', '2')))
            for key, value in values.items():
                (proc / key).write_text(value + '\n')
            for key in cap.PROPERTIES:
                (props / key).write_text('1\n' if key == 'security.perf_harden' else '\n')
            original = ''.join(f'sysctl {k} {v}\n' for k, v in values.items())
            original += ''.join(f'property {k} ' + (props / k).read_text() for k in cap.PROPERTIES)
            (remote / 'controls.before').write_text(original)
            (remote / 'boot-id').write_text(Path('/proc/sys/kernel/random/boot_id').read_text())
            perf = remote / 'simpleperf'
            perf.write_text(f'''#!/bin/sh
printf '777\\n' > '{proc}/perf_event_max_sample_rate'
printf '0\\n' > '{props}/security.perf_harden'
printf '999\\n' > '{props}/debug.perf_event_mlock_kb'
exit {perf_status}
''')
            perf.chmod(0o700)
            # Device getprop/setprop are mocked only at this process's shell boundary.
            prefix = f'''getprop() {{ cat '{props}/'$1; }}
setprop() {{ printf '%s\\n' "$2" > '{props}/'$1; }}
'''
            script = prefix + cap.device_script(remote, os.getpid(), 1, 750, proc)
            result = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
            self.assertEqual(result.returncode, perf_status, result.stderr)
            self.assertEqual((remote / 'controls.restored').read_text(), original)
            self.assertEqual((remote / 'restore-status').read_text(), '0\n')
            self.assertEqual((remote / 'record-status').read_text(), str(perf_status) + '\n')
            for key, value in values.items():
                self.assertEqual((proc / key).read_text(), value + '\n')
            self.assertEqual((props / 'debug.perf_event_mlock_kb').read_text(), '\n')

    def test_success_restores_original_controls_and_empty_properties(self):
        self.exercise_device_trap(0)

    def test_perf_failure_still_restores_original_controls(self):
        self.exercise_device_trap(7)

    def test_cli_refuses_invalid_rate_before_device_access(self):
        with patch.object(cap, 'capture') as capture:
            with self.assertRaises(SystemExit) as result:
                cap.main(['-s', 'fake', '--out', '/tmp/unused', '--symbols', '/symbols',
                          '--commit', '3421073', '--hz', '499'])
        self.assertEqual(result.exception.code, 2)
        capture.assert_not_called()

    def test_recovery_refuses_other_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            cap.save_json(Path(tmp) / 'capture.json', dict(serial='a', remote='/data/local/tmp/totk-cpuprof-' + 'b' * 32))
            with patch.object(cap, 'root') as root, self.assertRaises(SystemExit):
                cap.main(['-s', 'b', '--out', tmp, '--recover'])
            root.assert_not_called()

    def test_rc8_short_commit_and_missing_build_id_bind_exact_allocated_sections(self):
        with tempfile.TemporaryDirectory() as tmp:
            symbols = Path(tmp) / 'symbols'
            symbols.mkdir()
            (symbols / 'libtotk.so').write_bytes(b'unstripped')
            apk = Path(tmp) / 'installed.apk'
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('assets/build.txt', 'commit 34210734b46f\n')
                archive.writestr('lib/arm64-v8a/libtotk.so', b'stripped')
            identity = dict(build_id=None, has_symtab=True, has_debug_info=True)
            report = SimpleNamespace(read_elf_identity=Mock(return_value=identity),
                                     elf_alloc_sha256=Mock(return_value='a' * 64))
            with patch.dict('sys.modules', {'cpuprof_report': report}):
                state = cap.apk_identity(apk, symbols, '34210734b46ffc340ff61f1304f355bf32271bca')
            self.assertEqual(state['elf_alloc_sha256'], 'a' * 64)
            self.assertEqual(state['commit'], '34210734b46f')
            self.assertIsNone(state['build_id'])
            self.assertEqual(len(state['symbols_sha256']), 64)
            self.assertEqual(len(state['apk_sha256']), 64)

    def test_rc8_same_commit_without_build_ids_still_rejects_different_elf_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            symbols = Path(tmp) / 'symbols'
            symbols.mkdir()
            (symbols / 'libtotk.so').write_bytes(b'unstripped')
            apk = Path(tmp) / 'installed.apk'
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('assets/build.txt', 'commit 34210734b46f\n')
                archive.writestr('lib/arm64-v8a/libtotk.so', b'wrong build')
            identity = dict(build_id=None, has_symtab=True, has_debug_info=True)
            report = SimpleNamespace(read_elf_identity=Mock(return_value=identity),
                                     elf_alloc_sha256=Mock(side_effect=['a' * 64, 'b' * 64]))
            with patch.dict('sys.modules', {'cpuprof_report': report}):
                with self.assertRaisesRegex(ValueError, 'allocated ELF sections'):
                    cap.apk_identity(apk, symbols, '3421073')


if __name__ == '__main__':
    unittest.main()

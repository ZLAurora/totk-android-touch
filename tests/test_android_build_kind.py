"""Build-kind plumbing/invalidation without running build_android.py or an Android compiler."""
import ast
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'games/totk/android'


class BuildKindTests(unittest.TestCase):
    def test_wrapper_release_flag_and_callers(self):
        # Parse only: importing/running the game-build entrypoint is deliberately unnecessary.
        tree = ast.parse((ANDROID / 'build_android.py').read_text())
        functions = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
        release_options = [n for n in ast.walk(functions['parse_args'])
                           if isinstance(n, ast.Call) and any(isinstance(a, ast.Constant) and a.value == '--release' for a in n.args)]
        self.assertEqual(len(release_options), 1)
        self.assertTrue(any(k.arg == 'action' and isinstance(k.value, ast.Constant) and k.value.value == 'store_true'
                            for k in release_options[0].keywords))
        for name in ('main', 'runtime_only'):
            calls = [n for n in ast.walk(functions[name]) if isinstance(n, ast.Call)
                     and isinstance(n.func, ast.Name) and n.func.id == 'build_app']
            self.assertEqual(len(calls), 1, name)
            self.assertTrue(any(k.arg == 'release' and ast.unparse(k.value) == 'args.release' for k in calls[0].keywords), name)
        self.assertIn('DEV_BUILD=', ast.unparse(functions['build_runtime']))
        self.assertIn('release=release', ast.unparse(functions['build_app']))
        # The app itself is built by build_release.app, shared with the public release build.
        release = ast.parse((ANDROID / 'build_release.py').read_text())
        release_functions = {n.name: n for n in release.body if isinstance(n, ast.FunctionDef)}
        self.assertIn('DEV_BUILD=', ast.unparse(release_functions['build_native']))
        self.assertIn("'--release'", ast.unparse(release_functions['package']))


    def test_private_saves_preflight_run_as_before_creating_files(self):
        tree = ast.parse((ANDROID / 'build_android.py').read_text())
        install = next(n.value for n in tree.body if isinstance(n, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == 'INSTALL_SH' for t in n.targets))
        script = install.value.format(package='com.aure.totk')
        with tempfile.TemporaryDirectory(prefix='install-export-') as temp:
            temp = Path(temp)
            generated = temp / 'install_app.sh'
            generated.write_text(script)
            adb = temp / 'adb'
            adb.write_text(
                '#!/usr/bin/env python3\n'
                'import os, sys\n'
                'args = sys.argv[1:]\n'
                'if args[:2] == ["shell", "run-as"]:\n'
                '    sys.exit(1 if os.environ.get("REJECT_RUN_AS") else 0)\n'
                'if args[:2] == ["exec-out", "run-as"]:\n'
                '    sys.stdout.buffer.write(b"development export fixture")\n'
                '    sys.exit(7 if os.environ.get("REJECT_EXPORT") else 0)\n'
                'sys.exit(9)\n')
            adb.chmod(0o755)
            env = dict(os.environ, PATH=f'{temp}{os.pathsep}{os.environ["PATH"]}')
            # Cache transfer has its own namespace-aware adb/filesystem fixture.
            rejected = subprocess.run(['sh', str(generated), 'saves'], cwd=temp,
                                      env=dict(env, REJECT_RUN_AS='1'), capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('debuggable dev APK', rejected.stderr)
            self.assertIn('matching-signed dev APK over the release APK without uninstalling',
                          rejected.stderr)
            self.assertFalse(list(temp.glob('saves-*.tar')))
            self.assertFalse(list(temp.glob('*.tmp')))
            partial = subprocess.run(['sh', str(generated), 'saves'], cwd=temp,
                                     env=dict(env, REJECT_EXPORT='1'), capture_output=True, text=True)
            self.assertNotEqual(partial.returncode, 0)
            self.assertFalse(list(temp.glob('saves-*.tar')))
            self.assertFalse(list(temp.glob('*.tmp')))
            exported = subprocess.run(['sh', str(generated), 'saves'], cwd=temp, env=env,
                                      capture_output=True, text=True)
            self.assertEqual(exported.returncode, 0, exported.stdout + exported.stderr)
            matches = list(temp.glob('saves-*.tar'))
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0].read_bytes(), b'development export fixture')

    def test_same_output_build_kind_transitions(self):
        # Observe the production Makefile's real runtime/GFX object rules. A recording compiler only writes
        # invocation JSON: these are dependency/flag tests, not claims that an Android binary was built.
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='build-kind-', dir=scratch) as temp:
            temp = Path(temp)
            compiler = temp / 'record-cc'
            compiler.write_text('#!/usr/bin/env python3\n'
                                'import json, os, pathlib, sys\n'
                                'args = sys.argv[1:]\n'
                                'out = pathlib.Path(args[args.index("-o") + 1])\n'
                                'out.parent.mkdir(parents=True, exist_ok=True)\n'
                                'out.write_text(json.dumps(args))\n'
                                'with open(os.environ["BUILD_KIND_LOG"], "a") as f: f.write(json.dumps(args) + "\\n")\n')
            compiler.chmod(0o755)
            # The runtime builds without generated game headers (its game data is read at start).
            for app, objname, frontend in ((0, 'obj', 'frontend_headless'), (1, 'obj-app', 'frontend_android')):
                with self.subTest(app=app):
                    output = temp / f'out-{app}'
                    log = temp / f'calls-{app}.jsonl'
                    runtime = output / objname / 'rt' / f'{frontend}.c.o'
                    gfx = output / objname / 'gfx/android/gfx/vk_overlay.c.o'
                    linked = output / 'kind-linked'
                    fixture = temp / f'fixture-{app}.mk'
                    fixture.write_text(f'include {ANDROID / "Makefile"}\n'
                                       f'.PHONY: kind-test\nkind-test: {linked}\n'
                                       f'{linked}: {runtime} {gfx}\n\t$(CC) -o $@ $^\n')
                    environment = dict(os.environ, BUILD_KIND_LOG=str(log))
                    def make(kind):
                        args = ['make', '--no-print-directory', '-f', str(fixture), 'kind-test', 'TARGET=linux',
                                f'APP={app}', f'OUT={output}', f'CC={compiler}']
                        if kind is not None:
                            args.append(f'DEV_BUILD={kind}')
                        return subprocess.run(args, env=environment, capture_output=True, text=True)
                    previous = 0
                    for kind, expected, count in ((None, 1, 3), (1, 1, 0), (0, 0, 3), (0, 0, 0), (1, 1, 3), (1, 1, 0)):
                        run = make(kind)
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        invocations = [json.loads(line) for line in log.read_text().splitlines()]
                        self.assertEqual(len(invocations) - previous, count, f'APP={app}, DEV_BUILD={kind}: {run.stdout}')
                        previous = len(invocations)
                        self.assertIn(f'-DTOTK_DEV_BUILD={expected}', json.loads(runtime.read_text()))
                        self.assertIn(f'-DTOTK_DEV_BUILD={expected}', json.loads(gfx.read_text()))
                    bad = make(2)
                    self.assertNotEqual(bad.returncode, 0)
                    self.assertIn('DEV_BUILD', bad.stderr)


if __name__ == '__main__':
    unittest.main()

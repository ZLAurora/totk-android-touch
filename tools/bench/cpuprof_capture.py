#!/usr/bin/env python3
"""Rooted Android simpleperf capture with durable originals and verified recovery."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import uuid
import zipfile

HERE = Path(__file__).resolve().parent
NDK = Path('/home/agent/Android/Sdk/ndk/28.2.13676358/simpleperf')
LOCK = '/data/local/tmp/totk-cpuprof.lock'
CONTROLS = ('perf_event_paranoid', 'perf_event_max_sample_rate', 'perf_cpu_time_max_percent',
            'perf_event_mlock_kb', 'kptr_restrict')
PROPERTIES = ('debug.perf_event_max_sample_rate', 'debug.perf_cpu_time_max_percent',
              'debug.perf_event_mlock_kb', 'security.perf_harden')


def q(value):
    return shlex.quote(str(value))


def controls_script():
    return '\n'.join([f'printf "sysctl {k} "; cat "$PROC/{k}"' for k in CONTROLS] +
                     [f'printf "property {k} "; getprop {k}' for k in PROPERTIES])


def device_script(remote, pid, seconds, hz, proc='/proc/sys/kernel'):
    """The EXIT trap restores even when perf fails or the controller disconnects."""
    return f'''#!/system/bin/sh
set -eu
D={q(remote)}
PROC={q(proc)}
CHILD=""
controls() {{
{controls_script()}
}}
restore() {{
  trap - EXIT HUP INT TERM
  set +e
  failed=0
  if [ -n "$CHILD" ]; then kill -INT "$CHILD" 2>/dev/null; wait "$CHILD"; CHILD=""; fi
  [ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$D/boot-id")" ] || {{ echo 'boot changed; refusing stale restore' >&2; exit 1; }}
  while read -r kind key value; do
    [ "$kind" != property ] || setprop "$key" "$value" || failed=1
  done < "$D/controls.before"
  sleep 1
  while read -r kind key value; do
    [ "$kind" != sysctl ] || printf '%s\\n' "$value" > "$PROC/$key" || failed=1
  done < "$D/controls.before"
  controls > "$D/controls.restored" || failed=1
  cmp -s "$D/controls.before" "$D/controls.restored" || failed=1
  printf '%s\\n' "$failed" > "$D/restore-status"
  [ "$failed" = 0 ] || {{ echo "RESTORE FAILED: retain $D and run --recover" >&2; exit 1; }}
}}
[ -s "$D/controls.before" ]
[ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$D/boot-id")" ]
if [ "${{1:-}}" = restore ]; then restore; exit; fi
trap 'rc=$?; restore; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
printf '%s\\n' -1 > "$PROC/perf_event_paranoid"
printf '%s\\n' 0 > "$PROC/kptr_restrict"
# simpleperf can change property-backed limits itself; every such property/sysctl
# is snapshotted and restored above, including initially empty debug properties.
PCT=$(cat "$PROC/perf_cpu_time_max_percent")
[ "$PCT" -ge 1 ] && [ "$PCT" -le 100 ]
controls > "$D/controls.pre-record"
cat /proc/{pid}/maps > "$D/maps.before"
for t in /proc/{pid}/task/*; do printf '%s ' "${{t##*/}}"; cat "$t/comm"; done > "$D/threads.before"
"$D/simpleperf" record -p {pid} -e cpu-clock -f {hz} --call-graph fp --cpu-percent "$PCT" --duration {seconds} -o "$D/perf.data" > "$D/simpleperf.log" 2>&1 &
CHILD=$!
printf '%s\\n' "$CHILD" > "$D/simpleperf.pid"
rc=0
wait "$CHILD" || rc=$?
CHILD=""
controls > "$D/controls.after-record"
cat /proc/{pid}/maps > "$D/maps.after" || :
printf '%s\\n' "$rc" > "$D/record-status"
exit "$rc"
'''

def root_env():
    # A long record request must not serialize pan/input requests on their broker.
    runtime = Path(tempfile.gettempdir()) / ('totk-cpuprof-rootsh-' + str(os.getuid()))
    runtime.mkdir(mode=0o700, exist_ok=True)
    return dict(os.environ, XDG_RUNTIME_DIR=str(runtime))



def run(args, cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def root(args, script, timeout=120):
    result = run(args, [sys.executable, str(HERE / 'rootsh.py'), '-s', args.serial,
                       'run', '--timeout', str(timeout), '-c', script], capture_output=True, env=root_env())
    return result.stdout


def save_json(path, doc):
    path.write_text(json.dumps(doc, indent=2) + '\n')


def pull_root(args, remote, destination):
    with destination.open('wb') as target:
        run(args, [sys.executable, str(HERE / 'rootsh.py'), '-s', args.serial, 'run',
                   '--timeout', '120', '-c', 'cat ' + q(remote)], stdout=target, env=root_env())


def deploy(args, state, script):
    """adb's shell user cannot push into the private root directory."""
    for name, source in (('simpleperf', args.ndk_simpleperf / 'bin/android/arm64/simpleperf'),
                         ('capture.sh', script)):
        stage = state['remote'] + '-' + name
        run(args, ['adb', '-s', args.serial, 'push', str(source), stage], capture_output=True)
        root(args, f'set -e; cp {q(stage)} {q(state["remote"] + "/" + name)}; '
             f'chmod 700 {q(state["remote"] + "/" + name)}; rm -f {q(stage)}')


def apk_identity(apk, symbols, commit):
    from cpuprof_report import elf_alloc_sha256, read_elf_identity
    local = read_elf_identity(symbols / 'libtotk.so')
    if not local['has_symtab'] or not local['has_debug_info']:
        raise ValueError('symbols/libtotk.so must have full symbol table and DWARF')
    allocated = elf_alloc_sha256(symbols / 'libtotk.so')
    with zipfile.ZipFile(apk) as archive:
        metadata = archive.read('assets/build.txt').decode().strip()
        match = re.fullmatch(r'commit ([0-9a-f]{12,40})', metadata)
        if not match or not (match[1].startswith(commit) or commit.startswith(match[1])):
            raise ValueError(f'installed APK commit differs: {metadata!r}; expected {commit}')
        with tempfile.TemporaryDirectory() as tmp:
            shipped = Path(tmp) / 'libtotk.so'
            shipped.write_bytes(archive.read('lib/arm64-v8a/libtotk.so'))
            if read_elf_identity(shipped)['build_id'] != local['build_id']:
                raise ValueError('installed APK libtotk build ID does not match supplied exact-build symbols')
            if elf_alloc_sha256(shipped) != allocated:
                raise ValueError('installed APK allocated ELF sections do not match supplied exact-build symbols')
    return dict(commit=match[1], build_id=local['build_id'],
                elf_alloc_sha256=allocated,
                symbols=str(symbols / 'libtotk.so'),
                symbols_sha256=hashlib.sha256((symbols / 'libtotk.so').read_bytes()).hexdigest(),
                apk_sha256=hashlib.sha256(apk.read_bytes()).hexdigest())


def recover(args, state):
    remote = state['remote']
    root(args, f'[ "$(cat {LOCK}/owner)" = {q(remote)} ]')
    script = device_script(remote, state['pid'], state['duration_s'], state['frequency_hz'])
    root(args, 'sh -c ' + q(script) + ' cpuprof restore')
    pull_root(args, remote + '/controls.restored', args.out / 'controls.restored')
    if not (args.out / 'controls.before').exists():
        pull_root(args, remote + '/controls.before', args.out / 'controls.before')
    if (args.out / 'controls.before').read_bytes() != (args.out / 'controls.restored').read_bytes():
        raise RuntimeError('restore readback differs; retained remote directory and lock')
    state['restored'] = True
    save_json(args.out / 'capture.json', state)


def cleanup(args, state):
    # Require the saved ownership token; never remove another profiler's lock.
    root(args, f'set -e; [ "$(cat {LOCK}/owner)" = {q(state["remote"])} ]; '
         f'rm -rf {q(state["remote"])}; rm -f {q(state["remote"] + "-simpleperf")} '
         f'{q(state["remote"] + "-capture.sh")}; rm -f {LOCK}/owner; rmdir {LOCK}')
    state['cleaned'] = True
    save_json(args.out / 'capture.json', state)


def capture(args):
    args.out.mkdir(parents=True, exist_ok=False)
    state: dict = dict(serial=args.serial, remote='/data/local/tmp/totk-cpuprof-' + uuid.uuid4().hex,
                 event='cpu-clock', call_graph='fp', frequency_hz=args.hz, duration_s=args.seconds,
                 restored=False, cleaned=False)
    save_json(args.out / 'capture.json', state)
    apk = args.out / 'installed.apk'
    # Fail exact-build authentication before modifying any profiling controls.
    paths = root(args, 'pm path ' + q(args.package)).decode().splitlines()
    bases = [p.removeprefix('package:').strip() for p in paths if p.strip().endswith('/base.apk')]
    if len(bases) != 1:
        raise ValueError('expected one installed base APK')
    pull_root(args, bases[0], apk)
    state.update(apk_identity(apk, args.symbols, args.commit))
    apk.unlink()
    pids = root(args, 'pidof ' + q(args.package)).decode().split()
    if len(pids) != 1 or not pids[0].isdigit():
        raise ValueError('expected one running game process')
    state['pid'] = int(pids[0])
    save_json(args.out / 'capture.json', state)
    remote = state['remote']
    root(args, f'set -e; mkdir -m 700 {LOCK}; printf "%s\\n" {q(remote)} > {LOCK}/owner; '
         f'mkdir -m 700 {q(remote)}; cat /proc/sys/kernel/random/boot_id > {q(remote + "/boot-id")}; '
         f'PROC=/proc/sys/kernel; {{\n{controls_script()}\n}} > {q(remote + "/controls.before")}')
    try:
        pull_root(args, remote + '/controls.before', args.out / 'controls.before')
        script = args.out / 'capture-device.sh'
        script.write_text(device_script(remote, state['pid'], args.seconds, args.hz))
        # Root broker keeps the bounded device script alive across client disconnects.
        record_error = None
        try:
            deploy(args, state, script)
            (args.out / 'capture.ready').write_text('starting bounded simpleperf record\n')
            print(f'cpuprof: ready; start pan after 5 s ({args.out}/capture.ready)', flush=True)
            root(args, 'sh ' + q(remote + '/capture.sh'), timeout=args.seconds + 120)
        except subprocess.CalledProcessError as error:
            record_error = error
        finally:
            # The device trap normally restored already. This explicit recovery also
            # repairs a lost controller connection; it serializes behind the bounded run.
            recover(args, state)
        for name in ('simpleperf.log', 'controls.pre-record', 'controls.after-record',
                     'restore-status', 'record-status', 'maps.before', 'maps.after', 'threads.before', 'boot-id'):
            pull_root(args, remote + '/' + name, args.out / name)
        if record_error:
            raise record_error
        if (args.out / 'record-status').read_text().strip() != '0':
            raise RuntimeError('simpleperf record failed; see simpleperf.log')
        pull_root(args, remote + '/perf.data', args.out / 'perf.data')
        state['data_sha256'] = hashlib.sha256((args.out / 'perf.data').read_bytes()).hexdigest()
        save_json(args.out / 'capture.json', state)
        cleanup(args, state)
    except BaseException:
        print(f'Capture incomplete. Originals: {args.out}/controls.before. '
              f'Recover with: {HERE}/cpuprof.sh -s {args.serial} --out {args.out} --recover', file=sys.stderr)
        raise
    cmd = [sys.executable, str(HERE / 'cpuprof_report.py'), str(args.out / 'perf.data'),
           '--symbols', str(args.symbols), '--ndk-simpleperf', str(args.ndk_simpleperf),
           '--out', str(args.out / 'report.json'), '--capture-manifest', str(args.out / 'capture.json')]
    run(args, cmd)
    print(f'cpuprof: capture complete, controls restored and verified: {args.out}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-s', '--serial', required=True)
    parser.add_argument('--out', required=True, type=Path, help='new capture directory, or existing for --recover')
    parser.add_argument('--symbols', type=Path, help='exact build_release.py work/symbols directory')
    parser.add_argument('--commit', help='expected installed source commit (7-40 hex digits)')
    parser.add_argument('--package', default='com.aure.totk')
    parser.add_argument('--ndk-simpleperf', type=Path, default=NDK)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--hz', type=int, default=750)
    parser.add_argument('--recover', action='store_true')
    args = parser.parse_args(argv)
    args.out = args.out.resolve()
    if not re.fullmatch(r'[A-Za-z0-9_.]+', args.package):
        parser.error('invalid package name')
    if args.recover:
        state = json.loads((args.out / 'capture.json').read_text())
        if state['serial'] != args.serial or not re.fullmatch('/data/local/tmp/totk-cpuprof-[0-9a-f]{32}', state['remote']):
            parser.error('recovery serial/path mismatch')
        if state['cleaned']:
            print('cpuprof: already restored and cleaned')
            return
        recover(args, state)
        cleanup(args, state)
        return
    if not args.symbols or not args.commit or not re.fullmatch(r'[0-9a-f]{7,40}', args.commit):
        parser.error('--symbols and --commit (7-40 lowercase hex digits) required')
    if not 500 <= args.hz <= 1000 or not 1 <= args.seconds <= 300:
        parser.error('--hz must be 500-1000; --seconds must be 1-300')
    args.symbols = args.symbols.resolve()
    if not (args.ndk_simpleperf / 'bin/android/arm64/simpleperf').is_file():
        parser.error('NDK arm64 simpleperf missing')
    try:
        capture(args)
    finally:
        subprocess.run([sys.executable, str(HERE / 'rootsh.py'), '-s', args.serial, 'stop'],
                       env=root_env(), check=False)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f'cpuprof: {error}', file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors='replace') if isinstance(error.stderr, bytes) else error.stderr, file=sys.stderr)
        sys.exit(1)

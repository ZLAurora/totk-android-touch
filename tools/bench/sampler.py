#!/usr/bin/env python3
"""sampler.py -s <serial> --device <label> --out <dir> [--interval 3] [--duration S] [--package PKG] [--game-dir DIR]

Every --interval seconds, through one persistent root shell on the device (nothing is pushed), print the newest
'计时 至帧 N' frame of totk.log, the max CPU / GPU thermal-zone temperature, the sum of the thermal cooling-device states
(cpufreq / cpu-cluster / kgsl / gpu), the GPU's thermal_pwrlevel, clock and max_gpuclk (its effective cap), each
cpufreq policy's scaling_max_freq/cpuinfo_max_freq, and memory pressure: MemAvailable, the game's VmRSS / VmSwap /
RssShmem (kB), its kgsl memory /sys/class/kgsl/kgsl/proc/<pid>/{kernel,gpumem_mapped,imported} (bytes, whichever
exist), PSI cpu / memory some / full avg10 (/proc/pressure), and the kswapd threads' CPU ticks, from which the host adds
kswapd's CPU % over the interval. Lines are host-timestamped (epoch seconds) and appended to <out>/samples-<label>.txt:

  <epoch> f=<frame> cpu=<mC> gpu=<mC> cool=<sum> tpl=<level> gclk=<Hz> gcap=<Hz> external_power=<0|1> level=<%>
          mav=<kB> rss=<kB> swp=<kB> shm=<kB> kgk=<B> kgm=<B> kgi=<B> pcs=<avg10> pcf=<avg10> pms=<avg10> pmf=<avg10>
          ac=<raw Mains online> usb=<raw USB online> supply.<name>.type=<kernel type> supply.<name>.online=<0|1> ...
          kst=<ticks> ksw=<%> caps= <scaling>/<cpuinfo> ...

A value the device does not expose (no matching thermal zone / cooling device, no kgsl, no PSI, the game not running)
is left empty and becomes null.
external_power is any readable online non-battery supply, not Android BatteryService's AC/USB classification;
raw kernel supply observations are retained. Old rows without named supplies derive it from raw ac/usb.

Runs until --duration elapses, or SIGINT / SIGTERM; then the device loop and its sleep are killed, so no process is left
on the device. As a safety net the device loop also ends by itself after --max-seconds (default 3 h, or --duration + 60).
(The loop sleeps with `sleep`: mksh's `read -t` under su overshoots its timeout by seconds on some devices.) The loop
pins itself, and so every command it starts, to the slowest cpufreq cluster (rootsh.PIN_SH); the cleanup goes through
the serial's persistent root shell (rootsh.py), so sampling costs two su sessions in all.

--gpu-snapshot --scene LABEL --t0 EPOCH --boundary start|end writes one raw cumulative GPU counter snapshot to
<out>/gpu-time-in-state.jsonl through rootsh. parse_gpu_time_in_state, parse_gpu_snapshots and gpu_residency_delta
are importable; counter units are kept from the source header, or unknown, never assumed to be seconds."""
import argparse, hashlib, json, math, os, re, signal, subprocess, sys, threading, time

import rootsh

LOOP = r'''
echo "pid=$$ cpus=$C"
END=$(( $(date +%s) + @MAX@ ))
cd /sys/class/thermal 2>/dev/null
CZ=""; GZ=""; CD=""
for z in thermal_zone*; do t=$(cat $z/type 2>/dev/null); case $t in cpu*) CZ="$CZ $z/temp";; gpuss*) GZ="$GZ $z/temp";; esac; done
for c in cooling_device*; do t=$(cat $c/type 2>/dev/null); case $t in cpufreq-*|cpu-cluster*|thermal-cluster*|kgsl|gpu|devfreq-*kgsl*) CD="$CD $c/cur_state";; esac; done
L=@LOG@
AC=""; USB=""; BAT=""; SUPPLIES=""
for p in /sys/class/power_supply/*; do
  t=$(cat "$p/type" 2>/dev/null)
  case $t in Mains) AC="$AC $p/online" ;; USB|USB_*) USB="$USB $p/online" ;; Battery) [ -n "$BAT" ] || BAT=$p/capacity ;; esac
  case $t in Battery|BMS) ;; *) SUPPLIES="$SUPPLIES $p" ;; esac
done
online() { [ -z "$1" ] || cat $1 2>/dev/null </dev/null | sort -n | tail -n1; }
PSI() { sed -n "s/^$1 avg10=\([0-9.]*\).*/\1/p" $2 2>/dev/null </dev/null; }
while true; do
  f=$(tail -c 30000 $L 2>/dev/null | grep -ao '至帧 [0-9]*' | tail -n1 | grep -o '[0-9]*')
  # </dev/null: with an empty zone list `cat` would otherwise read this script's stdin and block.
  ct=$(cat $CZ 2>/dev/null </dev/null | sort -n | tail -n1); gt=$(cat $GZ 2>/dev/null </dev/null | sort -n | tail -n1)
  cs=""; [ -z "$CD" ] || { cs=0; for s in $(cat $CD 2>/dev/null </dev/null); do cs=$((cs+s)); done; }
  caps=""; for p in /sys/devices/system/cpu/cpufreq/policy*; do caps="$caps $(cat $p/scaling_max_freq 2>/dev/null)/$(cat $p/cpuinfo_max_freq 2>/dev/null)"; done
  P=$(pidof @PKG@ 2>/dev/null | cut -d' ' -f1); S=/proc/${P:-0}/status; K=/sys/class/kgsl/kgsl/proc/${P:-0}
  st() { [ -n "$P" ] && grep -m1 "^$1:" $S 2>/dev/null </dev/null | tr -s ' \t' ' ' | cut -d' ' -f2; }
  kv() { [ -n "$P" ] && cat $K/$1 2>/dev/null </dev/null; }
  ks=""; for k in $(pgrep kswapd 2>/dev/null); do t=$(cut -d' ' -f14,15 /proc/$k/stat 2>/dev/null </dev/null); [ -n "$t" ] && ks=$(( ${ks:-0} + ${t% *} + ${t#* } )); done
  mem="mav=$(grep -m1 '^MemAvailable:' /proc/meminfo </dev/null | tr -s ' ' | cut -d' ' -f2) rss=$(st VmRSS) swp=$(st VmSwap) shm=$(st RssShmem) kgk=$(kv kernel) kgm=$(kv gpumem_mapped) kgi=$(kv imported)"
  psi="pcs=$(PSI some /proc/pressure/cpu) pcf=$(PSI full /proc/pressure/cpu) pms=$(PSI some /proc/pressure/memory) pmf=$(PSI full /proc/pressure/memory) kst=$ks"
  power=""; for p in $SUPPLIES; do n=${p##*/}; power="$power supply.$n.type=$(cat "$p/type" 2>/dev/null) supply.$n.online=$(cat "$p/online" 2>/dev/null)"; done
  echo "f=${f:-0} cpu=$ct gpu=$gt cool=$cs tpl=$(cat /sys/class/kgsl/kgsl-3d0/thermal_pwrlevel 2>/dev/null) gclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) gcap=$(cat /sys/class/kgsl/kgsl-3d0/max_gpuclk 2>/dev/null) ac=$(online "$AC") usb=$(online "$USB") level=$(cat $BAT 2>/dev/null </dev/null) $power $mem $psi caps=$caps"
  [ $(date +%s) -lt $END ] || exit 0
  sleep @INTERVAL@
done
'''


GPU_SNAPSHOT_SCHEMA = 'totk-gpu-time-in-state/1'
GPU_SNAPSHOT_FILE = 'gpu-time-in-state.jsonl'
GPU_SNAPSHOT_SH = r'''
K=/sys/class/kgsl/kgsl-3d0
for f in "$K/devfreq/trans_stat" /sys/class/devfreq/*/trans_stat "$K/gpu_clock_stats" \
    "$K/devfreq/time_in_state" /sys/class/devfreq/*/time_in_state; do
  case $f in *kgsl*|*gpu*|*mali*) ;; *) continue ;; esac
  [ -r "$f" ] || continue
  printf 'source=%s\n' "$f"
  printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
  printf 'reset_count=%s\n' "$(cat "$K/reset_count" 2>/dev/null)"
  printf 'frequencies=%s\n' "$(cat "$K/gpu_available_frequencies" 2>/dev/null)"
  printf 'uptime_start=%s\n' "$(cut -d' ' -f1 /proc/uptime)"
  printf 'raw_begin\n'
  cat "$f"
  rc=$?
  printf '\nraw_end\n'
  printf 'uptime_end=%s\n' "$(cut -d' ' -f1 /proc/uptime)"
  exit "$rc"
done
echo 'no readable GPU cumulative time-in-state source' >&2
exit 3
'''


def power_of_tokens(tokens):
    """Kernel supply observations, not Android BatteryService AC/USB categories; no readable online => unknown."""
    supplies = {}
    for key, value in tokens.items():
        if key.startswith('supply.') and key.endswith(('.type', '.online')):
            name, field = key[7:].rsplit('.', 1)
            supplies.setdefault(name, {})[field] = value if field == 'type' else int(value) if value in ('0', '1') else None
    online = [s['online'] for s in supplies.values() if s.get('type') not in ('Battery', 'BMS')
              and s.get('online') in (0, 1)]
    if not online:
        online = [int(tokens[k]) for k in ('ac', 'usb') if tokens.get(k) in ('0', '1')]
    if not online and tokens.get('external_power') in ('0', '1'):
        online = [int(tokens['external_power'])]
    return {'external_power': any(online) if online else None, 'power_supplies': supplies}


def add_power(line):
    head, sep, tail = line.partition(' caps=')
    tokens = dict(t.split('=', 1) for t in head.split() if '=' in t)
    value = power_of_tokens(tokens)['external_power']
    return f'{head} external_power={"" if value is None else int(value)}{sep}{tail}'


def parse_gpu_time_in_state(text, source, frequencies=None):
    """Parse kernel cumulative counters, retaining units and integer precision.

    trans_stat: the last column is time-in-state, NOT the transition matrix. Its
    time(...) header declares the counter unit. gpu_clock_stats: a KGSL vector in
    gpu_available_frequencies order; this counts active clock time, not idle time.
    Plain devfreq time_in_state pairs have an undocumented counter unit here.
    Raises ValueError for unsupported, incomplete or ambiguous data.
    """
    if not isinstance(text, str) or not isinstance(source, str):
        raise ValueError('missing GPU counter text or source')
    name = source.rsplit('/', 1)[-1]
    counters, unit, kind = {}, 'unknown', 'residency'

    def add(freq, count):
        if freq in counters:
            raise ValueError(f'duplicate GPU frequency {freq}')
        counters[freq] = count

    if name == 'trans_stat':
        header, rows = None, []
        for line in text.splitlines():
            if m := re.search(r'\btime\s*\(([^)]+)\)\s*$', line, re.I):
                prefix = line[:m.start()].strip()
                if header is not None or not re.fullmatch(r':[\d\s]+', prefix):
                    raise ValueError('invalid trans_stat frequency header')
                header = prefix[1:].strip()
                unit = m[1].strip()
            elif m := re.fullmatch(r'\s*\*?\s*(\d+)\s*:([\d \t]+)', line):
                rows.append((int(m[1]), m[2]))
            elif re.match(r'\s*\*?\s*\d', line):
                raise ValueError('malformed trans_stat row')
        if header is None or not rows:
            raise ValueError('missing trans_stat time-in-state rows')
        freqs = [freq for freq, _ in rows]
        # The kernel prints %10lu with no separator: adjacent GHz frequencies
        # occupy all ten columns and must not be treated as one integer.
        if ([int(v) for v in header.split()] != freqs
                and header != ''.join(f'{freq:10d}' for freq in freqs).strip()):
            raise ValueError('mismatched trans_stat header frequencies')
        for freq, values in rows:
            cols = values.split()
            if len(cols) != len(rows) + 1:
                cols = [values[i * 10:(i + 1) * 10].strip() for i in range(len(rows))]
                cols.append(values[len(rows) * 10:].strip())
            if len(cols) != len(rows) + 1 or not all(c.isdigit() for c in cols):
                raise ValueError('incomplete trans_stat row')
            add(freq, int(cols[-1]))
    elif name == 'gpu_clock_stats':
        values = text.split()
        freqs = frequencies.split() if isinstance(frequencies, str) else []
        if (not values or not freqs or len(values) != len(freqs)
                or not all(v.isdigit() for v in values + freqs)):
            raise ValueError('gpu_clock_stats needs a matching gpu_available_frequencies vector')
        for freq, count in zip(freqs, values):
            add(int(freq), int(count))
        kind = 'active'
    elif name == 'time_in_state':
        for line in text.splitlines():
            if not line.strip():
                continue
            if not (m := re.fullmatch(r'\s*(\d+)\s+(\d+)\s*', line)):
                raise ValueError('malformed time_in_state pair')
            add(int(m[1]), int(m[2]))
    else:
        raise ValueError(f'unsupported GPU counter source {source!r}')
    if not counters:
        raise ValueError('empty GPU time-in-state snapshot')
    return {'source': source, 'counter_unit': unit, 'frequency_unit': 'Hz', 'counter_kind': kind,
            'counters': counters}


def parse_gpu_snapshots(path):
    """Read JSONL as {(device, T0): {'start': record, 'end': record}}.

    A missing file returns {}, leaving gpu_residency_delta to reject missing
    boundaries per pan. Failed captures remain records with status/error/raw.
    Invalid records or duplicate boundaries raise ValueError, never overwrite.
    """
    snapshots = {}
    if not os.path.exists(path):
        return snapshots
    with open(path, encoding='utf-8') as fh:
        for n, line in enumerate(fh, 1):
            try:
                record = json.loads(line)
                if (not isinstance(record, dict) or record.get('schema') != GPU_SNAPSHOT_SCHEMA
                        or not isinstance(record.get('device'), str) or not record['device']
                        or not isinstance(record.get('scene'), str) or not record['scene']
                        or type(record.get('T0')) is not int
                        or record.get('boundary') not in ('start', 'end')):
                    raise ValueError('invalid snapshot identity')
                pair = snapshots.setdefault((record['device'], record['T0']), {})
                if record['boundary'] in pair:
                    raise ValueError('duplicate snapshot boundary')
                pair[record['boundary']] = record
            except (ValueError, TypeError) as exc:
                raise ValueError(f'{path}:{n}: {exc}') from exc
    return snapshots


def gpu_residency_delta(start, end):
    """Delta two raw records; reject counter regressions, identity changes and impossible unit-aware total times.

    Returns source/counter_unit/frequency_unit/counter_kind, integer deltas,
    total_delta and fractions keyed by frequency in Hz. Fractions describe only
    the counter's coverage (active time for gpu_clock_stats), not wall seconds.
    KGSL reset_count changes are diagnostic GPU-start counts, not evidence of a devfreq stats reset.
    """
    parsed = []
    for boundary, record in (('start', start), ('end', end)):
        if not isinstance(record, dict):
            raise ValueError(f'missing GPU {boundary} snapshot')
        if record.get('schema') != GPU_SNAPSHOT_SCHEMA or record.get('boundary') != boundary:
            raise ValueError(f'invalid GPU {boundary} snapshot identity')
        if record.get('status') != 'ok':
            raise ValueError(f'GPU {boundary} snapshot unavailable: {record.get("error", "capture failed")}')
        for key in ('device', 'scene', 'serial_sha256', 'boot_id'):
            if not isinstance(record.get(key), str) or not record[key]:
                raise ValueError(f'missing GPU {boundary} snapshot {key}')
        if type(record.get('T0')) is not int:
            raise ValueError(f'missing GPU {boundary} snapshot T0')
        for key in ('uptime_start', 'uptime_end'):
            value = record.get(key)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError(f'missing GPU {boundary} snapshot {key}')
        if record['uptime_end'] < record['uptime_start']:
            raise ValueError(f'GPU {boundary} snapshot uptime reset')
        parsed.append(parse_gpu_time_in_state(record.get('raw'), record.get('source'), record.get('frequencies')))
    for key in ('device', 'scene', 'T0', 'serial_sha256', 'boot_id', 'source'):
        if start.get(key) != end.get(key):
            raise ValueError(f'mismatched GPU snapshot {key}')
    if end['uptime_start'] < start['uptime_end']:
        raise ValueError('GPU snapshot uptime reset or reversed boundaries')
    before, after = parsed
    for key in ('counter_unit', 'frequency_unit', 'counter_kind'):
        if before[key] != after[key]:
            raise ValueError(f'mismatched GPU snapshot {key}')
    if before['counters'].keys() != after['counters'].keys():
        raise ValueError('mismatched GPU snapshot frequencies')
    deltas = {f: after['counters'][f] - count for f, count in before['counters'].items()}
    if any(d < 0 for d in deltas.values()):
        raise ValueError('GPU cumulative counter reset')
    total = sum(deltas.values())
    if not total:
        raise ValueError('GPU cumulative counters did not advance')
    # KGSL reset_count counts GPU starts (including wake from slumber), not devfreq statistics resets.
    # The actual per-frequency counter deltas and read-time bounds establish whether a snapshot pair is usable.
    try:
        reset_delta = int(end.get('reset_count', '')) - int(start.get('reset_count', ''))
    except (TypeError, ValueError):
        reset_delta = None
    elapsed_lower = end['uptime_start'] - start['uptime_end']
    elapsed_upper = end['uptime_end'] - start['uptime_start']
    scale = {'s': 1, 'ms': 1e-3, 'us': 1e-6, 'ns': 1e-9}.get(before['counter_unit'].lower())
    sanity = {'status': 'unavailable', 'reason': 'counter unit has no known seconds conversion',
              'elapsed_lower_s': elapsed_lower, 'elapsed_upper_s': elapsed_upper}
    if scale is not None:
        seconds = total * scale
        # Suspended devfreq / active-clock counters may cover LESS than elapsed time. Never require a lower bound.
        # Permit sysfs accounting granularity and small timestamp/clock differences, but reject impossible totals.
        allowance = max(1.0, elapsed_upper * 0.05)
        if seconds > elapsed_upper + allowance:
            raise ValueError(f'GPU counter total {seconds:.3f}s exceeds elapsed upper bound '
                             f'{elapsed_upper:.3f}s plus {allowance:.3f}s tolerance')
        sanity = {'status': 'passed', 'counter_seconds': seconds, 'elapsed_lower_s': elapsed_lower,
                  'elapsed_upper_s': elapsed_upper, 'allowance_s': allowance}
    return {k: before[k] for k in ('source', 'counter_unit', 'frequency_unit', 'counter_kind')} | {
        'deltas': deltas, 'total_delta': total, 'fractions': {f: d / total for f, d in deltas.items()},
        'reset_count_start': start.get('reset_count'), 'reset_count_end': end.get('reset_count'),
        'reset_count_delta': reset_delta, 'time_sanity': sanity}


def capture_gpu_snapshot(serial, device, scene, t0, boundary):
    """Capture through the persistent root shell; failures are explicit, serializable records."""
    record = {'schema': GPU_SNAPSHOT_SCHEMA, 'device': device, 'scene': scene, 'T0': t0, 'boundary': boundary,
              'serial_sha256': hashlib.sha256(serial.encode()).hexdigest(), 'host_epoch_ns': time.time_ns(),
              'status': 'error', 'raw': ''}
    rc, out, err = rootsh.run(serial, GPU_SNAPSHOT_SH)
    text = out.decode('utf-8', errors='replace').replace('\r\n', '\n')
    head, marker, rest = text.partition('raw_begin\n')
    raw, end_marker, tail = rest.rpartition('\nraw_end\n')
    metadata = dict(line.split('=', 1) for line in (head + tail).splitlines() if '=' in line)
    record.update({k: metadata.get(k, '') for k in ('source', 'boot_id', 'reset_count', 'frequencies')})
    record['raw'] = raw if marker and end_marker else text
    try:
        if rc or not marker or not end_marker:
            raise ValueError(f'rootsh status {rc}: {err.decode(errors="replace").strip() or "incomplete capture"}')
        for key in ('uptime_start', 'uptime_end'):
            record[key] = float(metadata[key])
            if not math.isfinite(record[key]) or record[key] < 0:
                raise ValueError(f'invalid {key}')
        if not record['boot_id'] or record['uptime_end'] < record['uptime_start']:
            raise ValueError('missing boot identity or uptime reset during capture')
        parse_gpu_time_in_state(record['raw'], record['source'], record['frequencies'])
        record['status'] = 'ok'
    except (KeyError, ValueError) as exc:
        record['error'] = str(exc)
    return record


class Stop(Exception):
    pass


USER_HZ = 100   # /proc/<pid>/stat ticks per second on Linux / Android


def add_kswapd(line, now, previous):
    """The device line with ksw=<kswapd CPU % since the previous sample> inserted before caps= (empty for the first
    sample or without kst), and the new (time, ticks) state."""
    ticks = next((t[4:] for t in line.split(' caps=', 1)[0].split() if t.startswith('kst=')), '')
    ksw = ''
    if ticks.isdigit():
        if previous and now > previous[0] and int(ticks) >= previous[1]:
            ksw = f'{(int(ticks) - previous[1]) / USER_HZ / (now - previous[0]) * 100:.1f}'
        state = (now, int(ticks))
    else:
        state = None
    head, sep, tail = line.partition(' caps=')
    return f'{head} ksw={ksw}{sep}{tail}', state


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('-s', dest='serial', required=True)
    ap.add_argument('--device', required=True, help='device label used in file names (e.g. thor)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--interval', type=int, default=3)
    ap.add_argument('--duration', type=float, help='stop after this many seconds (default: until SIGINT/SIGTERM)')
    ap.add_argument('--max-seconds', type=int, help='device-side safety deadline (default 10800, or --duration + 60)')
    ap.add_argument('--package', default='com.aure.totk')
    ap.add_argument('--game-dir', help='default /data/user/0/<package>/files/game')
    ap.add_argument('--gpu-snapshot', action='store_true', help='capture one raw cumulative GPU counter boundary')
    ap.add_argument('--scene', help='scene identity for --gpu-snapshot')
    ap.add_argument('--t0', type=int, help='pan T0 identity for --gpu-snapshot')
    ap.add_argument('--boundary', choices=('start', 'end'), help='pan boundary for --gpu-snapshot')
    a = ap.parse_args()
    game = a.game_dir or f'/data/user/0/{a.package}/files/game'
    os.makedirs(a.out, exist_ok=True)
    if a.gpu_snapshot:
        if not a.scene or a.t0 is None or not a.boundary:
            ap.error('--gpu-snapshot requires --scene, --t0 and --boundary')
        record = capture_gpu_snapshot(a.serial, a.device, a.scene, a.t0, a.boundary)
        with open(os.path.join(a.out, GPU_SNAPSHOT_FILE), 'a', encoding='utf-8') as out:
            out.write(json.dumps(record, ensure_ascii=False) + '\n')
        if record['status'] != 'ok':
            sys.exit(f'sampler: GPU {a.boundary} snapshot unavailable: {record["error"]}')
        return
    path = os.path.join(a.out, f'samples-{a.device}.txt')

    def stop(*_):
        raise Stop
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if a.duration:
        signal.signal(signal.SIGALRM, stop)
        signal.setitimer(signal.ITIMER_REAL, a.duration)

    # Own session: a terminal Ctrl-C must reach only this script, which then stops the device loop in order.
    p = subprocess.Popen(['adb', '-s', a.serial, 'shell', 'su -c sh'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, start_new_session=True)
    max_s = a.max_seconds or (int(a.duration) + 60 if a.duration else 10800)
    p.stdin.write(rootsh.PIN_SH + LOOP.replace('@LOG@', f'{game}/totk.log').replace('@INTERVAL@', str(a.interval))
                  .replace('@PKG@', a.package)
                  .replace('@MAX@', str(max_s)))
    p.stdin.flush()
    remote_pid, n, kswapd = None, 0, None
    try:
        with open(path, 'a', buffering=1) as out:
            for line in p.stdout:
                line = line.rstrip('\r\n')
                if remote_pid is None and line.startswith('pid='):
                    remote_pid, _, cpus = line[4:].partition(' cpus=')
                    print(f'sampler: {a.serial} -> {path} (device loop pid {remote_pid}, cpus {cpus or "unpinned"})',
                          file=sys.stderr)
                    continue
                if not line.startswith('f='):
                    print(f'sampler: device: {line}', file=sys.stderr)
                    continue
                now = time.time()
                line, kswapd = add_kswapd(add_power(line), now, kswapd)
                out.write(f'{now:.1f} {line}\n')
                n += 1
    except Stop:
        pass
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.setitimer(signal.ITIMER_REAL, 0)
        if remote_pid:   # the loop shell and its current sleep / cat children
            rootsh.run(a.serial, f'kill $(pgrep -P {remote_pid}) {remote_pid}')
        try:
            p.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        reader = threading.Thread(target=lambda: p.stdout.read(), daemon=True)   # drain so adb can exit
        reader.start()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        if remote_pid:
            left = rootsh.run(a.serial, f'ps -p {remote_pid} -o PID=')[1].decode(errors='replace').strip()
            if left:
                print(f'sampler: WARNING device loop pid {remote_pid} still running', file=sys.stderr)
    print(f'sampler: stopped, {n} samples in {path}', file=sys.stderr)
    if remote_pid is None:
        sys.exit('sampler: the root shell never started (adb / su failed?)')


if __name__ == '__main__':
    main()

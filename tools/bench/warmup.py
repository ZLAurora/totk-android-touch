#!/usr/bin/env python3
"""Optional, bounded wait for fresh graphics compile reports to be calm; never filters measured windows."""
import argparse
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

from provenance import compile_record


def wait_for_calm(read_log, calm_windows, timeout, interval=3, clock=time.monotonic, sleep=time.sleep):
    started = clock()
    observations, previous, calm = [], None, 0
    result = {'scope': 'graphics-300-present-only', 'required_calm_windows': calm_windows,
              'timeout_s': timeout, 'observations': observations,
              'limitation': 'does not establish shader-translation/compute quiescence or absence of queued work'}
    while clock() - started < timeout:
        try:
            rows = dict(row for line in read_log(timeout - (clock() - started)).splitlines()
                        if (row := compile_record(line)) is not None)
        except (OSError, subprocess.SubprocessError) as exc:
            result.update(status='unavailable', reason=str(exc))
            break
        newest = max(rows, default=None)
        if previous is None:
            previous = newest  # Existing calm history does not satisfy a new wait.
            result['initial_frame'] = newest
        elif newest is not None and newest < previous:
            result.update(status='unavailable', reason='frame counter reset during warm-up')
            break
        else:
            for frame in sorted(f for f in rows if previous is not None and f > previous):
                row = rows[frame]
                if frame != previous + 300:
                    calm = 0
                known_zero = all(row.get(k) == 0 for k in ('pipeline_creations', 'pipeline_sync', 'pipeline_async'))
                calm = calm + 1 if known_zero else 0
                observations.append({'frame': frame, **row, 'calm_streak': calm})
                previous = frame
                if calm >= calm_windows:
                    result['status'] = 'calm'
                    break
            if result.get('status') == 'calm':
                break
        remaining = timeout - (clock() - started)
        if remaining > 0:
            sleep(min(interval, remaining))
    result.setdefault('status', 'timeout')
    result['elapsed_s'] = round(clock() - started, 3)
    result['calm_streak'] = calm
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-s', required=True)
    parser.add_argument('--device', required=True)
    parser.add_argument('--scene', required=True)
    parser.add_argument('--game-dir', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--calm-windows', type=int, required=True)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    if not 1 <= args.calm_windows <= 100 or not math.isfinite(args.timeout) or not 0 < args.timeout <= 3600:
        parser.error('calm-windows must be 1..100 and timeout must be finite in (0, 3600] seconds')
    here = Path(__file__).resolve().parent

    def read_log(remaining):
        # Small bounded tail, not an ever-growing full log pull. Missing reports remain unknown.
        # A byte-offset tail can start inside a multibyte character; replace it rather than abort.
        command = 'tail -c 65536 ' + shlex.quote(args.game_dir + '/totk.log')
        return subprocess.run([sys.executable, str(here / 'rootsh.py'), '-s', args.s, 'run',
                               '--timeout', str(max(0.01, min(10, remaining))), '-c', command],
                              capture_output=True, text=True, encoding='utf-8', errors='replace',
                              check=True, timeout=max(0.01, remaining)).stdout

    record = {'device': args.device, 'scene': args.scene, 'started_epoch': time.time(),
              **wait_for_calm(read_log, args.calm_windows, args.timeout)}
    os.makedirs(args.out, exist_ok=True)
    with open(Path(args.out) / 'warmups.jsonl', 'a', encoding='utf-8') as stream:
        stream.write(json.dumps(record) + '\n')
    print(f'warm-up: {record["status"]}, {record["calm_streak"]}/{args.calm_windows} fresh calm graphics windows, '
          f'{record["elapsed_s"]:.1f}s; shader/compute quiescence unavailable')


if __name__ == '__main__':
    main()

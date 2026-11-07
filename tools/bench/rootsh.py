#!/usr/bin/env python3
"""rootsh.py -s <serial> run [-c SCRIPT] [--timeout S] | stop | status

One persistent root shell per device for the bench tools (bench_input.sh, pan.sh, live.sh, sampler.py's cleanup).
Every `su -c` opens a new su session, and with Magisk each one also starts a su-notify app_process (which crashes on
some devices, then crash_dump runs): about 0.6 s of CPU on any core, the game's included, per command. `run` sends
SCRIPT (default: its stdin) to a host-side broker that holds ONE `adb shell su -c sh` for the serial, prints the
script's stdout and stderr and exits with its status (124: no answer within --timeout, the broker then ends; 125: no
root shell). The broker starts on the first `run` and ends with `stop`, after --idle seconds without a request
(default 1800) or when the root shell ends (device gone).

The root shell first pins itself, and so every command it starts, to the device's slowest cpufreq cluster (lowest
cpuinfo_max_freq) with the device's `taskset`, so the tools' processes stay off the cores the game needs most
(`status` prints the cores). Each script runs in its own `sh -c` with stdin from /dev/null: its `exit`, a syntax error
or a read of stdin cannot touch the persistent shell. Requests run one at a time, in arrival order.
Socket: $XDG_RUNTIME_DIR (else the temp dir)/totk-rootsh-<serial>.sock, with .log (broker errors) and .lock beside."""
import argparse, base64, fcntl, json, os, re, secrets, socket, struct, subprocess, sys, tempfile, threading, time

# Device side: pin this shell to the slowest cpufreq cluster; C = its cores (empty when unpinned).
PIN_SH = r'''
C=$(for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo "$(cat $p/cpuinfo_max_freq 2>/dev/null </dev/null) $(cat $p/related_cpus 2>/dev/null </dev/null)"
done | sort -n | head -n1 | cut -d' ' -f2-)
m=0; for c in $C; do m=$((m | (1 << c))); done
[ "$m" -gt 0 ] && taskset -p "$(printf %x $m)" $$ >/dev/null 2>&1 || C=""
'''
NO_SHELL, TIMED_OUT = 125, 124


def sock_path(serial):
    base = os.environ.get('XDG_RUNTIME_DIR') or tempfile.gettempdir()
    return os.path.join(base, 'totk-rootsh-' + re.sub(r'[^A-Za-z0-9_.-]', '_', serial) + '.sock')


def send(conn, kind, data=b''):
    conn.sendall(kind + struct.pack('>I', len(data)) + data)


def recv_exact(conn, n):
    buf = b''
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('the broker closed the connection')
        buf += chunk
    return buf


# ---- broker ---------------------------------------------------------------------------------------------------------

class Shell:
    """The persistent `adb shell su -c sh`, its stdout / stderr collected by two reader threads."""

    def __init__(self, serial):
        self.p = subprocess.Popen(['adb', '-s', serial, 'shell', 'su -c sh'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.buf = {'out': bytearray(), 'err': bytearray()}
        self.eof = {'out': False, 'err': False}
        self.cv = threading.Condition()
        for name, f in (('out', self.p.stdout), ('err', self.p.stderr)):
            threading.Thread(target=self.reader, args=(name, f), daemon=True).start()

    def reader(self, name, f):
        while True:
            chunk = os.read(f.fileno(), 65536)
            with self.cv:
                if not chunk:
                    self.eof[name] = True
                else:
                    self.buf[name] += chunk
                self.cv.notify_all()
            if not chunk:
                return

    def wait_for(self, pred, deadline):
        """Wait until pred() (under the lock) returns a value; None on timeout or when the shell ended."""
        with self.cv:
            while True:
                v = pred()
                if v is not None:
                    return v
                left = deadline - time.monotonic()
                if left <= 0 or (self.eof['out'] and self.eof['err']):
                    return None
                self.cv.wait(left)

    def start(self, timeout=30):
        self.p.stdin.write((PIN_SH + 'echo "rootsh pid=$$ cpus=$C"\n').encode())
        self.p.stdin.flush()

        def ready():
            m = re.search(rb'rootsh pid=\d+ cpus=[^\n]*\n', self.buf['out'])
            if m:
                line = bytes(m.group(0)).decode().strip()
                del self.buf['out'][:m.end()]
                return line
        return self.wait_for(ready, time.monotonic() + timeout)

    def run(self, script, timeout):
        """(status, stdout, stderr) of one script; status None when the shell did not answer in time or ended."""
        mark = ('rootsh-end-' + secrets.token_hex(8)).encode()
        b64 = base64.b64encode(script).decode()
        # $(...) drops the script's trailing newlines only. The leading \n keeps the marker on its own line.
        cmd = (f'''sh -c "$(printf %s '{b64}' | base64 -d)" rootsh </dev/null; '''
               f'''printf '\\n%s %s\\n' {mark.decode()} $?; printf '\\n%s\\n' {mark.decode()} >&2\n''')
        try:
            self.p.stdin.write(cmd.encode())
            self.p.stdin.flush()
        except (BrokenPipeError, OSError):
            return None, b'', b''
        deadline = time.monotonic() + timeout
        out_re = re.compile(rb'\n' + re.escape(mark) + rb' (\d+)\n')
        err_tag = b'\n' + mark + b'\n'

        def done():
            mo = out_re.search(self.buf['out'])
            ie = self.buf['err'].find(err_tag)
            if mo and ie >= 0:
                out, err = bytes(self.buf['out'][:mo.start()]), bytes(self.buf['err'][:ie])
                rc = int(mo.group(1))
                del self.buf['out'][:mo.end()]
                del self.buf['err'][:ie + len(err_tag)]
                return rc, out, err
        r = self.wait_for(done, deadline)
        return r if r is not None else (None, b'', b'')

    def close(self):
        try:
            self.p.stdin.close()   # the root shell ends at EOF, then su and adb
        except OSError:
            pass
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def serve(serial, idle):
    path = sock_path(serial)
    sh = Shell(serial)
    ready = sh.start()
    if not ready:
        sh.close()
        sys.exit(f'rootsh: no root shell on {serial} (adb / su failed?)')
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if os.path.exists(path):
        os.unlink(path)
    old = os.umask(0o077)
    srv.bind(path)
    os.umask(old)
    srv.listen(16)
    srv.settimeout(idle)
    try:
        while True:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                return
            with conn:
                conn.settimeout(None)
                try:
                    head = json.loads(recv_exact(conn, struct.unpack('>I', recv_exact(conn, 4))[0]))
                    script = recv_exact(conn, struct.unpack('>I', recv_exact(conn, 4))[0])
                except (ConnectionError, ValueError, OSError):
                    continue
                op = head.get('op')
                if op == 'stop':
                    send(conn, b'R', struct.pack('>i', 0))
                    return
                if op == 'status':
                    send(conn, b'O', (ready + '\n').encode())
                    send(conn, b'R', struct.pack('>i', 0))
                    continue
                rc, out, err = sh.run(script, float(head.get('timeout') or 120))
                try:
                    if out:
                        send(conn, b'O', out)
                    if err:
                        send(conn, b'E', err)
                    if rc is None:
                        gone = sh.eof['out'] and sh.eof['err']
                        send(conn, b'E', (b'rootsh: the root shell ended\n' if gone else
                                          b'rootsh: no answer from the root shell in time; the broker ends\n'))
                        send(conn, b'R', struct.pack('>i', NO_SHELL if gone else TIMED_OUT))
                        return
                    send(conn, b'R', struct.pack('>i', rc))
                except OSError:
                    if rc is None:
                        return
    finally:
        srv.close()
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        sh.close()


# ---- client ---------------------------------------------------------------------------------------------------------

def connect(serial, start=True, idle=1800):
    """A connection to the serial's broker, started first when needed (None when not running and start is False)."""
    path = sock_path(serial)

    def attempt():
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            s.close()
            return None
    s = attempt()
    if s or not start:
        return s
    with open(path + '.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        s = attempt()
        if s:
            return s
        if os.path.exists(path):
            os.unlink(path)   # stale: its broker is gone
        with open(path + '.log', 'ab') as log:
            child = subprocess.Popen([sys.executable, os.path.abspath(__file__), '-s', serial, 'serve', '--idle', str(idle)],
                                     stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            s = attempt()
            if s:
                return s
            if child.poll() is not None:
                break
            time.sleep(0.05)
    try:
        with open(path + '.log', encoding='utf-8', errors='replace') as f:
            tail = f.read().strip().splitlines()[-1:]
    except OSError:
        tail = []
    raise ConnectionError('rootsh: the broker did not start' + (f': {tail[0]}' if tail else ''))


def request(serial, op, script=b'', timeout=120, start=True):
    """(status, stdout, stderr) of one request to the broker."""
    s = connect(serial, start=start)
    if s is None:
        return None, b'', b''
    with s:
        head = json.dumps({'op': op, 'timeout': timeout}).encode()
        s.sendall(struct.pack('>I', len(head)) + head + struct.pack('>I', len(script)) + script)
        out, err = bytearray(), bytearray()
        while True:
            kind = recv_exact(s, 1)
            data = recv_exact(s, struct.unpack('>I', recv_exact(s, 4))[0])
            if kind == b'O':
                out += data
            elif kind == b'E':
                err += data
            else:
                return struct.unpack('>i', data)[0], bytes(out), bytes(err)


def run(serial, script, timeout=120):
    """Run a device script (str or bytes) as root through the serial's broker: (status, stdout bytes, stderr bytes)."""
    if isinstance(script, str):
        script = script.encode()
    try:
        return request(serial, 'run', script, timeout)
    except (ConnectionError, OSError) as e:
        return NO_SHELL, b'', f'{e}\n'.encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('-s', dest='serial', required=True)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='run a script as root (stdin, or -c)')
    r.add_argument('-c', dest='script', help='the script (default: read from stdin)')
    r.add_argument('--timeout', type=float, default=120)
    sub.add_parser('stop', help='end the broker and its root shell')
    sub.add_parser('status', help='print the root shell pid and pinned cores, if a broker runs')
    sv = sub.add_parser('serve', help='(internal) run the broker in the foreground')
    sv.add_argument('--idle', type=float, default=1800)
    a = ap.parse_args()
    if a.cmd == 'serve':
        serve(a.serial, a.idle)
        return
    if a.cmd == 'run':
        script = a.script.encode() if a.script is not None else sys.stdin.buffer.read()
        rc, out, err = run(a.serial, script, a.timeout)
    else:
        try:
            rc, out, err = request(a.serial, a.cmd, start=False)
        except (ConnectionError, OSError) as e:
            rc, out, err = NO_SHELL, b'', f'{e}\n'.encode()
        if rc is None:
            rc, out = 0, b'rootsh: not running\n'
    sys.stdout.buffer.write(out)
    sys.stdout.flush()
    sys.stderr.buffer.write(err)
    sys.exit(rc)


if __name__ == '__main__':
    main()

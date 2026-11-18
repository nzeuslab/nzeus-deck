#!/usr/bin/env python3
"""PC test: do rescue-command.py and rescue-upload.py work UNCHANGED against the
SD root's USB shell (overlay/usr/local/sbin/nzeus-usb-shell)?

The USB serial port is replaced by a pseudo-terminal (pty) on the PC:
  - nzeus-usb-shell runs exactly as init starts it: a new session with no
    terminal, NZ_USB_TTY = the pty's slave side (instead of /dev/ttyGS0);
  - the two PC tools are byte-identical copies (sha256 checked) run in this
    process with two stand-in modules: `serial` (reads/writes the pty master)
    and `rescue_port` (returns the port; the real one asks Windows).
Proves the shell protocol (markers, stty -echo / raw, head -n upload, binary
readback, exit status) with the PC's bash/dash - NOT the console's kernel,
USB controller or Windows. The upload lands in /tmp/nzeus-tools/<unique> on
this PC and is deleted afterwards.

  python3 rescue_pty_test.py --wrapper .../nzeus-usb-shell --shell /bin/bash --work DIR
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import random
import runpy
import select
import shutil
import signal
import subprocess
import sys
import time
import types
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
DIAG = REPO / 'linux-port' / 'diagnostics'
RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok)))
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok or not detail else f' ({detail})'), flush=True)


class FakeSerial:
    """The few pyserial calls the tools use, on the pty master."""
    fd = None

    def __init__(self, port, baud, timeout=None, write_timeout=None):
        self.timeout = timeout

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False          # the "COM port" closes; the pty stays (like the cable)

    def read(self, n):
        r, _, _ = select.select([self.fd], [], [], self.timeout or 0)
        if not r:
            return b''
        try:
            return os.read(self.fd, n)
        except OSError:
            return b''

    def write(self, data):
        view = memoryview(data)
        while view:
            _, w, _ = select.select([], [self.fd], [], 5)
            if not w:
                raise TimeoutError('write timeout')
            n = os.write(self.fd, view)
            view = view[n:]
        return len(data)


def run_tool(path, argv):
    """Run a tool copy as __main__ with the stand-in modules; return (exit code, stdout)."""
    serial_mod = types.ModuleType('serial')
    serial_mod.Serial = FakeSerial
    port_mod = types.ModuleType('rescue_port')
    port_mod.find_rescue_port = lambda: ('COM9', 'USB\\VID_1D6B&PID_0104\\NZEUS-RAM-RESCUE-01')
    saved = {k: sys.modules.get(k) for k in ('serial', 'rescue_port')}
    sys.modules['serial'], sys.modules['rescue_port'] = serial_mod, port_mod
    old_argv = sys.argv
    sys.argv = [str(path)] + argv
    out = io.StringIO()
    code = 0
    try:
        with contextlib.redirect_stdout(out):
            runpy.run_path(str(path), run_name='__main__')
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
    except Exception as e:      # an assertion inside the tool = a failed protocol
        code = f'exception: {e!r}'
    finally:
        sys.argv = old_argv
        for k, v in saved.items():
            if v is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = v
    return code, out.getvalue()


def drain(fd, seconds):
    end = time.monotonic() + seconds
    data = b''
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                data += os.read(fd, 65536)
            except OSError:
                break
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--wrapper', required=True)
    ap.add_argument('--shell', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--inputrc', default='')
    ap.add_argument('--kernel', default='7.2.0-rc5-nzeus-gma-native-sdroot+')
    a = ap.parse_args()
    work = Path(a.work)
    shutil.rmtree(work, ignore_errors=True)
    tools, fakebin, home = work / 'tools', work / 'bin', work / 'home'
    for d in (tools, fakebin, home, tools / 'uart-usb-fix'):   # (evidence folder, exists in the repo)
        d.mkdir(parents=True)
    for name in ('rescue-command.py', 'rescue-upload.py'):
        shutil.copy2(DIAG / name, tools / name)
        same = hashlib.sha256((DIAG / name).read_bytes()).digest() == hashlib.sha256((tools / name).read_bytes()).digest()
        check(f'{name}: test copy is byte-identical to the repo tool', same)
    # uname -r answers like the console's kernel (the tools refuse anything else)
    (fakebin / 'uname').write_text('#!/bin/sh\ncase "$1" in -r) echo %s ;; *) /bin/uname "$@" ;; esac\n' % a.kernel)
    (fakebin / 'uname').chmod(0o755)
    # like Debian's /root/.bashrc (prompt + umask)
    (home / '.bashrc').write_text("PS1='\\h:\\w\\$ '\numask 022\n")

    master, slave = os.openpty()
    FakeSerial.fd = master
    env = dict(NZ_USB_TTY=os.ttyname(slave), NZ_USB_SHELL=a.shell,
               NZ_TEST_PATH=f'{fakebin}:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
               NZ_TEST_HOME=str(home), NZ_TEST_INPUTRC=a.inputrc or '/nonexistent')
    # exactly like init: own session, no controlling terminal, stdio on /dev/null
    proc = subprocess.Popen(['/bin/sh', a.wrapper], env=env, start_new_session=True,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    banner = drain(master, 2.0)
    check('banner printed on the port', b'root shell on the USB cable' in banner, banner[-300:])

    # ---- rescue-command.py: a script with its own exit status
    script = work / 'probe.sh'
    script.write_text('echo PROBE_START\n'
                      'test -t 0 && echo STDIN_IS_A_TERMINAL\n'
                      'ps -o tty= -p $$ 2>/dev/null | grep -q pts && echo CONTROLLING_TTY_OK\n'
                      'echo "kernel=$(uname -r)"\n'
                      "printf 'quote \"x\" dollar $HOME back\\\\slash\\n'\n"
                      'exit 0\n')
    code, out = run_tool(tools / 'rescue-command.py', [str(script), '--seconds', '20'])
    res = json.loads(out[:out.index('\n}') + 2]) if '\n}' in out else {}
    check('rescue-command: finished, exit status 0', code == 0 and res.get('complete') and res.get('remote_exit') == 0, (code, res))
    text = Path(res.get('output', work / 'none')).read_text(errors='replace') if res.get('output') else ''
    check('rescue-command: script ran inside the shell', 'PROBE_START' in text and 'kernel=' + a.kernel in text, text[-400:])
    check('rescue-command: stdin is the terminal', 'STDIN_IS_A_TERMINAL' in text)
    check('rescue-command: the port is the controlling terminal', 'CONTROLLING_TTY_OK' in text)
    check('rescue-command: no bracketed-paste escape codes', '\x1b[?2004' not in text)

    script2 = work / 'fail.sh'
    script2.write_text('echo FAILING_ON_PURPOSE\nexit 5\n')
    code, out = run_tool(tools / 'rescue-command.py', [str(script2), '--seconds', '20'])
    res = json.loads(out[:out.index('\n}') + 2]) if '\n}' in out else {}
    check('rescue-command: remote exit 5 is reported (tool exit 1)', code == 1 and res.get('remote_exit') == 5, (code, res))

    # ---- rescue-upload.py: binary with every byte value (incl. CR, LF, Ctrl+C/D/Z, DEL)
    rnd = random.Random(20261009)
    payload = bytes(range(256)) * 64 + bytes(rnd.getrandbits(8) for _ in range(150000))
    src = work / 'upload.bin'
    src.write_bytes(payload)
    name = f'nzeus-rootfs-ptytest-{os.getpid()}'
    target = Path('/tmp/nzeus-tools') / name
    code, out = run_tool(tools / 'rescue-upload.py', [str(src), '--name', name])
    ok = code == 0 and target.is_file() and target.read_bytes() == payload + b'\n'
    check(f'rescue-upload: {len(payload)} bytes arrived exactly (+1 LF), readback verified', ok, (code, out[-300:]))
    check('rescue-upload: file mode 700 after verification', target.is_file() and (target.stat().st_mode & 0o777) == 0o700)
    if target.exists():
        target.unlink()

    # the shell still works after raw mode and the binary transfer
    code, out = run_tool(tools / 'rescue-command.py', [str(script), '--seconds', '20'])
    check('rescue-command again after the upload', code == 0, code)

    # PC closes the port -> hang-up -> the shell ends (init would start a new one)
    os.close(slave)
    os.close(master)
    try:
        proc.wait(timeout=10)
        check('shell ends when the port is closed (init restarts it)', True)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        check('shell ends when the port is closed (init restarts it)', False)
    failed = [n for n, ok in RESULTS if not ok]
    print(f'RESCUE_PTY {"PASSED" if not failed else "FAILED"}: {len(RESULTS) - len(failed)}/{len(RESULTS)} ({a.shell})')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())

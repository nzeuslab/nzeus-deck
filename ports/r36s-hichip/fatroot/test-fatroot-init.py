#!/usr/bin/env python3
"""PC test of ports/r36s-hichip/fatroot/init with the console's MIPS BusyBox under qemu-mipsel.

Same method as sdroot/test-sdroot-init.py: the test runs a COPY of the init in
which ONLY the marked "fixed paths" block is replaced (asserted). mount, umount,
mountpoint, switch_root, setsid, cttyhack, mkdir, sleep and nz-fatroot are
logging stubs; everything else is the real MIPS BusyBox. Proves the script
logic only - not the kernel, the card or USB.

  python3 test-fatroot-init.py --busybox MIPS_BUSYBOX
"""
import argparse
import os
import shutil
import signal
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
QEMU = shutil.which('qemu-mipsel')        # full path: the scenarios run with a bare PATH
ap = argparse.ArgumentParser()
ap.add_argument('--busybox', required=True)
args = ap.parse_args()
BB = str(Path(args.busybox).resolve())
INIT = (HERE / 'init').read_text()
BEGIN = '# --- fixed paths: test-fatroot-init.py replaces ONLY these lines in a copy ---\n'
END = '# --- end of fixed paths ---\n'
head, rest = INIT.split(BEGIN)
block, tail = rest.split(END)
assert block == ('export PATH=/bin\nNEWROOT=/newroot\nFATROOT=/bin/nz-fatroot\nRESCUE=/usb-rescue\n'
                 'CARD=/dev/mmcblk0\nFAT=/fat\nDATA=/data0\nKMSG=/dev/kmsg\nLOG=/run/nzeus-fatroot.log\n'
                 'CARD_WAIT=10\nATTACH_WAIT=300\n'), block
work = Path(tempfile.mkdtemp(prefix='nzpub-fatroot-init-', dir='/tmp'))
failures = []

STUB = r'''#!/bin/sh
# test stub: never touches the PC, only logs (and emulates mount points)
PATH=/usr/bin:/bin
T=%(T)s
name=${0##*/}
echo "$name $*" >> "$T/log"
mp() { echo "$1" | sed 's|/|_|g'; }
case "$name" in
mount)
    if test "$1" = --move; then
        test -e "$T/fail-move$(mp "$2")" && exit 1
        rm -f "$T/mp/$(mp "$2")"; touch "$T/mp/$(mp "$3")"; exit 0
    fi
    eval "target=\${$#}"; touch "$T/mp/$(mp "$target")"; exit 0 ;;
umount) test -e "$T/mp/$(mp "$1")" || exit 1; rm -f "$T/mp/$(mp "$1")"; exit 0 ;;
mountpoint) test -e "$T/mp/$(mp "$2")" ;;
switch_root) touch "$T/switched"; exit 0 ;;
setsid) kill -TERM "$PPID"; exit 0 ;;
fatroot)
    case "$1" in
    attach)
        test -e "$T/attach-hang" && exec /bin/sleep 600
        cat "$T/attach-out" 2>/dev/null
        rc=$(cat "$T/attach-rc" 2>/dev/null || echo 0)
        if test "$rc" = 0; then touch "$T/mp/$(mp "$3")" "$T/mp/$(mp "$T/fat")" "$T/mp/$(mp "$T/data0")"; fi
        exit "$rc" ;;
    *) exit 0 ;;
    esac ;;
rescue) echo "usb-rescue started" >> "$T/log" ;;
sleep) /bin/sleep 0.25 ;;
dmesg) echo "fake kernel message 1"; echo "fake kernel message 2" ;;
ld.so.1)
    if test -e "$T/run-fail"; then echo "Segmentation fault"; exit 139; fi
    echo "nzroot-runs" ;;
*) exit 0 ;;
esac
'''
NAMES = ['mount', 'umount', 'mountpoint', 'setsid', 'cttyhack', 'mkdir', 'sleep', 'switch_root', 'sync', 'dmesg']


def scenario(name, setup=None, attach_wait=300):
    T = work / ''.join(c if c.isalnum() else '-' for c in name)
    (T / 'bin').mkdir(parents=True)
    (T / 'mp').mkdir()
    nr = T / 'newroot'
    for d in ('dev', 'proc', 'sys', 'run', 'sbin', 'sdcard', 'data'):
        (nr / d).mkdir(parents=True)
    (nr / 'sbin' / 'init').write_text('#!/bin/sh\n')
    (nr / 'sbin' / 'init').chmod(0o755)
    (nr / 'lib').mkdir()                              # nzroot.img's program loader (a stub here)
    (nr / 'lib' / 'ld.so.1').write_text(STUB % {'T': T})
    (nr / 'lib' / 'ld.so.1').chmod(0o755)
    (T / 'card').write_text('')                       # the SD card is there
    stub = STUB % {'T': T}
    applets = subprocess.run([QEMU, BB, '--list'], capture_output=True, text=True, check=True).stdout.split()
    for n in applets:
        if n not in NAMES:
            (T / 'bin' / n).write_text('#!/bin/sh\nexec %s %s %s "$@"\n' % (QEMU, BB, n))
            (T / 'bin' / n).chmod(0o755)
    for n in NAMES:
        (T / 'bin' / n).write_text(stub)
        (T / 'bin' / n).chmod(0o755)
    for n in ('fatroot', 'rescue'):
        (T / n).write_text(stub)
        (T / n).chmod(0o755)
    if setup:
        setup(T, nr)
    fixed = ('export PATH=%s/bin\nNEWROOT=%s\nFATROOT=%s/fatroot\nRESCUE=%s/rescue\nCARD=%s/card\n'
             'FAT=%s/fat\nDATA=%s/data0\nKMSG=%s/kmsg\nLOG=%s/fatroot.log\nCARD_WAIT=2\nATTACH_WAIT=%d\n') % (
        T, nr, T, T, T, T, T, T, T, attach_wait)
    # BusyBox runs some applets in-process whatever PATH says: route the stubbed names
    # through shell functions; `exec switch_root` goes through an alias (see sdroot test).
    fixed += ''.join('%s() { %s/bin/%s "$@"; }\n' % (n, T, n) for n in NAMES if n != 'switch_root')
    fixed += "alias exec='exec '\nalias switch_root=%s/bin/switch_root\n" % T
    script = head + BEGIN + fixed + END + tail
    assert script.replace(fixed, block) == INIT
    (T / 'init').write_text(script)
    out = T / 'out.txt'
    with open(out, 'wb') as f:
        proc = subprocess.Popen(['env', '-i', 'PATH=%s/bin' % T, QEMU, BB, 'sh', str(T / 'init')],
                                stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            proc.wait(timeout=90)
        except subprocess.TimeoutExpired:
            pass
        finally:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    log = (T / 'log').read_text().splitlines() if (T / 'log').exists() else []
    return T, nr, log, out.read_text(errors='replace')


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name, flush=True)
    if not ok:
        print(detail)
        failures.append(name)


def rescue_kept(log, out):
    return ('usb-rescue started' in log and 'setsid cttyhack /bin/sh' in log and
            'staying in the RAM rescue system' in out and not any(l.startswith('switch_root') for l in log))


def shown(log, text):
    """the message went to the screen through `nz-fatroot show`"""
    return any(l.startswith('fatroot show') and text in l for l in log)


# 0. syntax and the "no [" rule
r = subprocess.run([QEMU, BB, 'sh', '-n', str(HERE / 'init')], capture_output=True, text=True)
check('init_syntax', r.returncode == 0, r.stderr)
check('init_no_bracket_command', not any(l.strip().startswith('[') or ' [ ' in l
                                         for l in INIT.splitlines() if not l.strip().startswith('#')))

# 1. success: mounts move in the right order, then switch_root
T, nr, log, out = scenario('success')
moves = [l for l in log if l.startswith('mount --move')]
want = ['mount --move %s/fat %s/sdcard' % (T, nr), 'mount --move %s/data0 %s/data' % (T, nr),
        'mount --move /dev %s/dev' % nr, 'mount --move /proc %s/proc' % nr,
        'mount --move /sys %s/sys' % nr, 'mount --move /run %s/run' % nr]
check('init_success_moves_mounts', moves == want and (T / 'switched').exists()
      and 'fatroot attach %s/card %s' % (T, nr) in log, '\n'.join(log) + out)

# 1b. before the switch, the boot log goes onto the card (nzeus/boot-log.txt) and is synced;
#     the log of the start before that is kept as boot-log-previous.txt
def card_has_nzeus(T, nr):
    (T / 'fat' / 'nzeus').mkdir(parents=True)
    (T / 'fat' / 'nzeus' / 'boot-log.txt').write_text('the start before\n')
T, nr, log, out = scenario('boot log', card_has_nzeus)
bl = T / 'fat' / 'nzeus' / 'boot-log.txt'
text = bl.read_text() if bl.exists() else ''
first_move = next((i for i, l in enumerate(log) if l.startswith('mount --move')), len(log))
check('init_boot_log_on_card', 'all checks passed' in text and 'fake kernel message 2' in text
      and any(l.strip() == 'sync' for l in log[:first_move]) and (T / 'switched').exists()
      and (T / 'fat' / 'nzeus' / 'boot-log-previous.txt').read_text() == 'the start before\n',
      text + '\n'.join(log) + out)

# 1d. before the switch, a program is run from nzroot.img; if that crashes (the console's
#     cache bug), NO switch: the message goes to the screen and the boot log, rescue follows
def runs_fail(T, nr):
    card_has_nzeus(T, nr)
    (T / 'run-fail').write_text('')
T, nr, log, out = scenario('test run fails', runs_fail)
text = (T / 'fat' / 'nzeus' / 'boot-log.txt').read_text() if (T / 'fat' / 'nzeus' / 'boot-log.txt').exists() else ''
check('init_test_run_fails_stays_in_rescue', rescue_kept(log, out) and shown(log, "can't run programs")
      and 'exit 139' in text and 'fatroot detach %s' % nr in log and not (T / 'switched').exists(),
      text + '\n'.join(log) + out)
T, nr, log, out = scenario('test run ok', card_has_nzeus)
check('init_test_run_ok_logged', any(l.startswith('ld.so.1 --library-path %s/lib/mipsel-linux-gnu %s/bin/sh'
                                                  % (nr, nr)) for l in log)
      and 'exit 0' in (T / 'fat' / 'nzeus' / 'boot-log.txt').read_text() and (T / 'switched').exists(),
      '\n'.join(log) + out)

# 1c. a card without an nzeus folder for the log: the start goes on anyway
T, nr, log, out = scenario('boot log not writable')
check('init_boot_log_never_blocks', (T / 'switched').exists(), '\n'.join(log) + out)

# 2. no card: rescue, message on screen
T, nr, log, out = scenario('no card', lambda T, nr: (T / 'card').unlink())
check('init_no_card_rescue', rescue_kept(log, out) and shown(log, 'no SD card') and 'fatroot attach' not in
      ' '.join(log), '\n'.join(log) + out)

# 3. attach fails: its fatroot: line reaches the screen, rescue follows
def attach_fails(T, nr):
    (T / 'attach-out').write_text('fatroot: nzroot.img is missing on the SD card (run install.bat again)\n')
    (T / 'attach-rc').write_text('4\n')
T, nr, log, out = scenario('attach fails', attach_fails)
check('init_attach_fails_rescue_and_message', rescue_kept(log, out)
      and shown(log, 'nzroot.img is missing on the SD card') and not (T / 'switched').exists(),
      '\n'.join(log) + out)

# 4. attach never finishes: give up after ATTACH_WAIT, rescue
T, nr, log, out = scenario('attach hangs', lambda T, nr: (T / 'attach-hang').write_text(''), attach_wait=2)
check('init_attach_hangs_rescue', rescue_kept(log, out) and shown(log, 'took too long'), '\n'.join(log) + out)

# 5. the image has no /sbin/init: undo (detach) and rescue
T, nr, log, out = scenario('no sbin init', lambda T, nr: (nr / 'sbin' / 'init').unlink())
check('init_no_sbin_init_detaches', rescue_kept(log, out) and 'fatroot detach %s' % nr in log
      and shown(log, '/sbin/init'), '\n'.join(log) + out)

# 6. a move fails half-way: everything goes back, rescue
T, nr, log, out = scenario('move fails', lambda T, nr: (T / ('fail-move' + '/sys'.replace('/', '_'))).write_text(''))
check('init_move_fails_undoes', rescue_kept(log, out) and 'fatroot detach %s' % nr in log
      and not (T / 'switched').exists(), '\n'.join(log) + out)

# 7. the rescue part is exactly the one in sdroot/init
mark = '# ---- from here on: exactly the RAM rescue system of ports/r36s-hichip/init ----\n'
sd = (HERE.parent / 'sdroot' / 'init').read_text()
check('init_rescue_section_identical', mark in INIT and INIT.split(mark)[1] == sd.split(mark)[1])

print(f'{13 - len(failures)}/13 passed' if not failures else f'FAILED: {failures}')
raise SystemExit(1 if failures else 0)

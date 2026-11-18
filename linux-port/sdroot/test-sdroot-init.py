#!/usr/bin/env python3
"""PC test of the sdroot /init with the console's MIPS BusyBox under qemu-mipsel.

The test runs a COPY of linux-port/sdroot/init in which ONLY the marked
"fixed paths" block is replaced (asserted: the rest is byte-identical).
Commands that would touch the PC (mount, umount, mountpoint, blockdev,
switch_root, setsid, cttyhack, mkdir, sleep) are logging stubs in the test
PATH; everything else is the MIPS BusyBox. Scenarios: every failure path must
end in the unchanged RAM rescue (usb-rescue started, serial shell loop), the
success path must move /dev /proc /sys /run and exec switch_root, and two
runs use the REAL nz-sdboot (fake tree) end to end.
Proves the script logic only - not the kernel, the card or USB.

  python3 test-sdroot-init.py --busybox MIPS_BUSYBOX --nz-sdboot MIPS_NZ_SDBOOT
                              [--old-busybox BOOT_BUSYBOX_WITHOUT_SWITCH_ROOT]
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--busybox', required=True)
ap.add_argument('--nz-sdboot', required=True)
ap.add_argument('--old-busybox')
ap.add_argument('--qemu', default=shutil.which('qemu-mipsel') or 'qemu-mipsel')
ap.add_argument('--report')
args = ap.parse_args()
QEMU = args.qemu
BB = str(Path(args.busybox).resolve())
SDBOOT = str(Path(args.nz_sdboot).resolve())
INIT = (HERE / 'init').read_text()
BEGIN = '# --- fixed paths: test-sdroot-init.sh replaces ONLY these lines in a copy ---\n'
END = '# --- end of fixed paths ---\n'
head, rest = INIT.split(BEGIN)
block, tail = rest.split(END)
assert block == ('export PATH=/bin\nNEWROOT=/newroot\nSDBOOT=/bin/nz-sdboot\nRESCUE=/usb-rescue\n'
                 'MOUNTS=/proc/mounts\nKMSG=/dev/kmsg\nLOG=/run/nzeus-sdboot.log\nMOUNT_WAIT=20\n'), block
work = Path(tempfile.mkdtemp(prefix='nz-sdroot-init-'))
results = []

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
    case " $* " in
    *" -t ext4 "*)
        test -e "$T/mount-hang" && exec /bin/sleep 600
        test -e "$T/mount-fail" && exit 32 ;;
    esac
    eval "target=\${$#}"; touch "$T/mp/$(mp "$target")"; exit 0 ;;
umount) test -e "$T/mp/$(mp "$1")" || exit 1; rm -f "$T/mp/$(mp "$1")"; exit 0 ;;
mountpoint) test -e "$T/mp/$(mp "$2")" ;;
blockdev) n=${2#/dev/mmcblk0p}; if test -e "$T/ro-$n"; then cat "$T/ro-$n"; else echo 1; fi ;;
switch_root) touch "$T/switched"; exit 0 ;;
setsid) kill -TERM "$PPID"; exit 0 ;;
sdboot)
    if test "$1" = --check-init; then exit "$(cat "$T/initcheck-rc" 2>/dev/null || echo 0)"; fi
    exit "$(cat "$T/sdboot-rc" 2>/dev/null || echo 0)" ;;
rescue) echo "usb-rescue started" >> "$T/log" ;;
sleep) /bin/sleep 0.25 ;;
*) exit 0 ;;
esac
'''


def scenario(name, setup=None, sdboot=None, busybox=BB, stubs=('switch_root',)):
    T = work / ''.join(c if c.isalnum() else '-' for c in name)   # plain folder names only
    (T / 'bin').mkdir(parents=True)
    (T / 'mp').mkdir()
    nr = T / 'newroot'
    for d in ('dev', 'proc', 'sys', 'run', 'sbin'):
        (nr / d).mkdir(parents=True)
    stub = STUB % {'T': T}
    names = ['mount', 'umount', 'mountpoint', 'blockdev', 'setsid', 'cttyhack', 'mkdir', 'sleep'] + list(stubs)
    # Like the console's /bin: one entry per applet of THIS BusyBox (run under
    # qemu), except the stubbed ones. An applet the BusyBox lacks is missing.
    applets = subprocess.run([QEMU, busybox, '--list'], capture_output=True, text=True, check=True).stdout.split()
    for n in applets:
        if n not in names:
            (T / 'bin' / n).write_text('#!/bin/sh\nexec %s %s %s "$@"\n' % (QEMU, busybox, n))
            (T / 'bin' / n).chmod(0o755)
    for n in names:
        (T / 'bin' / n).write_text(stub)
        (T / 'bin' / n).chmod(0o755)
    for n in ('sdboot', 'rescue'):
        (T / n).write_text(stub)
        (T / n).chmod(0o755)
    (T / 'mounts').write_text('rootfs / rootfs rw 0 0\n')
    if setup:
        setup(T, nr)
    fixed = ('export PATH=%s/bin\nNEWROOT=%s\nSDBOOT="%s"\nRESCUE=%s/rescue\nMOUNTS=%s/mounts\n'
             'KMSG=%s/kmsg\nLOG=%s/sdboot.log\nMOUNT_WAIT=20\n') % (
        T, nr, sdboot(T) if sdboot else '%s/sdboot' % T, T, T, T, T)
    # BusyBox ash runs NOFORK applets (mountpoint, mkdir, ...) in-process
    # whatever PATH says; shell functions come first, so route the stubbed
    # names through functions (test copy only).
    fixed += ''.join('%s() { %s/bin/%s "$@"; }\n' % (n, T, n) for n in names if n != 'switch_root')
    # `exec switch_root` cannot go through a function. With FEATURE_SH_STANDALONE
    # ash runs its OWN applet via /proc/self/exe when the PC can execute MIPS
    # programs directly (binfmt_misc + qemu-user-static, as after installing the
    # Debian builder's tools); a full path is never an applet lookup, so an alias
    # (expanded after `exec ` thanks to the trailing space) reaches the stub.
    if 'switch_root' in names:
        fixed += "alias exec='exec '\nalias switch_root=%s/bin/switch_root\n" % T
    script = head + BEGIN + fixed + END + tail
    assert script.replace(fixed, block) == INIT      # nothing else changed
    (T / 'init').write_text(script)
    out = T / 'out.txt'
    with open(out, 'wb') as f:
        proc = subprocess.Popen(['env', '-i', 'PATH=%s/bin' % T, QEMU, busybox, 'sh', str(T / 'init')],
                                stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = proc.wait(timeout=90)
        except subprocess.TimeoutExpired:
            code = 'timeout'
        finally:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    log = (T / 'log').read_text().splitlines() if (T / 'log').exists() else []
    return T, nr, log, out.read_text(errors='replace'), code


def check(name, ok, detail=''):
    results.append(dict(case=name, ok=bool(ok), **({} if ok else {'detail': detail})))
    print('%s %s' % ('PASS' if ok else 'FAIL', name))
    if not ok:
        print(detail)


def rescue_kept(log, out):
    """The unchanged RAM rescue ran: usb-rescue in the background, banner, serial shell loop."""
    return ('usb-rescue started' in log and 'setsid cttyhack /bin/sh' in log and
            'No SD card or internal flash filesystem has been mounted.' in out and
            'staying in the RAM rescue system' in out and not any(l.startswith('switch_root') for l in log))


def put(path, text):
    path.write_text(text)


# 0. syntax with the new and (if given) the current boot BusyBox
for label, bb in (('new', BB), ('old', args.old_busybox)):
    if bb:
        r = subprocess.run([QEMU, bb, 'sh', '-n', str(HERE / 'init')], capture_output=True, text=True)
        check('syntax: sh -n with the %s boot BusyBox' % label, r.returncode == 0, r.stderr)
check('no "[" command in /init', not any(l.strip().startswith('[') or ' [ ' in l
                                         for l in INIT.splitlines() if not l.strip().startswith('#')))

def fail_case(name, setup, why):
    T, nr, log, out, code = scenario(name, setup)
    check('fallback: %s' % name, rescue_kept(log, out) and why in out, '\n'.join(log) + '\n' + out)
    return T, nr, log


def mounts_ro(T, nr):
    put(T / 'mounts', 'rootfs / rootfs rw 0 0\n/dev/mmcblk0p3 %s ext4 ro,noatime 0 0\n' % nr)


def success_mounts(fn):
    def setup(T, nr):
        mounts_ro(T, nr)
        fn(T, nr)
    return setup


# 1. success path with stubs (/proc/mounts shows the read-only ext4 mount)
T, nr, log, out, code = scenario('success', success_mounts(lambda T, nr: None))
moves = [l for l in log if l.startswith('mount --move')]
check('success: ro ext4 mount, checks, moves, switch_root',
      'mount -t ext4 -o ro,noatime /dev/mmcblk0p3 %s' % nr in log
      and 'sdboot --log %s/sdboot.log' % T in log
      and 'sdboot --check-init %s --log %s/sdboot.log' % (nr, T) in log
      and [l for l in log if l.startswith('blockdev')] == ['blockdev --getro /dev/mmcblk0p%d' % p for p in (1, 2, 5)]
      and 'umount /tmp' in log
      and moves == ['mount --move /%s %s/%s' % (d, nr, d) for d in ('dev', 'proc', 'sys', 'run')]
      and log[-1] == 'switch_root %s /sbin/init' % nr and (T / 'switched').exists()
      and 'usb-rescue started' not in log and code == 0, '\n'.join(log) + out)
fail_case('nz-sdboot refuses (exit 1)', lambda T, nr: put(T / 'sdboot-rc', '1'), 'staying in the RAM')
fail_case('FN / nzeus.ramonly (exit 4)', lambda T, nr: put(T / 'sdboot-rc', '4'), 'staying in the RAM')
fail_case('nz-sdboot no answer (exit 3)', lambda T, nr: put(T / 'sdboot-rc', '3'), 'staying in the RAM')
fail_case('mount of p3 fails', lambda T, nr: put(T / 'mount-fail', ''), 'mount of mmcblk0p3 failed')
T, nr, log = fail_case('mount of p3 never returns', lambda T, nr: put(T / 'mount-hang', ''),
                       'mount did not finish within 20 s')
T, nr, log = fail_case('p3 mounted but not ro ext4 in /proc/mounts', None, 'not mounted read-only as ext4')
check('  ...and /newroot was unmounted again', 'umount %s' % nr in log)
T, nr, log = fail_case('SD root /sbin/init unusable',
                       success_mounts(lambda T, nr: put(T / 'initcheck-rc', '1')), 'staying in the RAM')
check('  ...and /newroot was unmounted again', 'umount %s' % nr in log)
for p in (1, 2, 5):
    fail_case('p%d not read-only (blockdev --getro)' % p,
              success_mounts(lambda T, nr, p=p: put(T / ('ro-%d' % p), '0')),
              'mmcblk0p%d is not read-only in the kernel' % p)
fail_case('SD root has no /run', success_mounts(lambda T, nr: (nr / 'run').rmdir()), 'has no /run folder')
T, nr, log = fail_case('moving /sys fails half way', success_mounts(lambda T, nr: put(T / 'fail-move_sys', '')),
                       'could not move /sys')
check('  ...and /proc, /dev moved back, /tmp remounted, /newroot unmounted',
      'mount --move %s/proc /proc' % nr in log and 'mount --move %s/dev /dev' % nr in log
      and 'mount -t tmpfs tmpfs /tmp' in log[log.index('umount /tmp'):] and 'umount %s' % nr in log,
      '\n'.join(log))
if args.old_busybox:
    T, nr, log, out, code = scenario('old BusyBox without switch_root', success_mounts(lambda T, nr: None),
                                     busybox=str(Path(args.old_busybox).resolve()), stubs=())
    check('fallback: BusyBox without switch_root', rescue_kept(log, out)
          and 'switch_root is missing' in out, '\n'.join(log) + out)

# 2. end to end with the REAL nz-sdboot (fake tree, MIPS) - pass and refuse
sys.argv = [sys.argv[0], '--binary', SDBOOT]
src = (HERE / 'test-nz-sdboot.py').read_text().split('# ------------------------------------------------------------ main decision')[0]
src = src.replace('Path(__file__).resolve().parent', 'Path(%r)' % str(HERE))
g = {'__name__': 'nz_sdboot_tree'}
exec(compile(src, str(HERE / 'test-nz-sdboot.py'), 'exec'), g)    # only the fake-tree builder


def real(fake):
    def make(T):
        return '%s %s --fake-root %s' % (QEMU, SDBOOT, fake)
    return make


fake_ok = g['fake_tree']('e2e-ok')


def static_init(T, nr):
    mounts_ro(T, nr)
    shutil.copy(BB, nr / 'sbin/init')
    (nr / 'sbin/init').chmod(0o755)


T, nr, log, out, code = scenario('e2e-pass', static_init, sdboot=real(fake_ok))
check('end to end: real nz-sdboot passes -> switch_root', (T / 'switched').exists()
      and 'NZ_SDBOOT_OK' in out and 'NZ_SDBOOT_INIT_OK' in out and 'usb-rescue started' not in log
      and 'NZ_SDBOOT_OK' in (T / 'sdboot.log').read_text(), '\n'.join(log) + out)
fake_bad = g['fake_tree']('e2e-bad')
(fake_bad / 'sys/block/mmcblk0/device/cid').write_text('00000000000000000000000000000000\n')
T, nr, log, out, code = scenario('e2e-wrong-card', static_init, sdboot=real(fake_bad))
check('end to end: real nz-sdboot refuses wrong card -> RAM rescue', rescue_kept(log, out)
      and 'wrong card' in out and not any(l.startswith('mount -t ext4') for l in log), '\n'.join(log) + out)
shutil.rmtree(g['work'], ignore_errors=True)

passed = sum(1 for r in results if r['ok'])
print('SDROOT_INIT_TESTS %d/%d passed' % (passed, len(results)))
if args.report:
    Path(args.report).write_text(json.dumps(dict(
        status='PASS_PC_QEMU_ONLY' if passed == len(results) else 'FAIL', passed=passed, total=len(results),
        cases=results, limits=['stubs replace mount/umount/switch_root/setsid; no kernel, card or USB involved']),
        indent=2) + '\n')
shutil.rmtree(work, ignore_errors=True)
sys.exit(0 if passed == len(results) else 1)

"""fn-hold.py FLAGFILE SECONDS - for usb-host-trial.sh (2026-10-10).

The R36S USB-C data port cannot sense the PC cable being unplugged (hardware:
the controller kept saying "configured" after the cable was pulled), so the
trial cannot wait for "not attached". Instead the owner confirms "the PC cable
is out" by HOLDING FN for 2 s. This watcher writes FLAGFILE as soon as FN
(KEY_MENU on the "r36s-keys" input device) has been held for 2 s, and gives up
after SECONDS. Read-only: it only asks the kernel which keys are down.
"""
import fcntl
import os
import sys
import time

KEY_MENU = 139                       # FN on the r36s-keys device (GPIO L02)
HOLD = 2.0
# EVIOCGKEY(96) with the MIPS ioctl encoding (_IOC_READ = 2 at bit 29); on a PC
# (x86: read = 2 at bit 30) the test sets NZ_EVIOCGKEY.
EVIOCGKEY = int(os.environ.get('NZ_EVIOCGKEY', str((2 << 29) | (96 << 16) | (ord('E') << 8) | 0x18)))
INPUT = os.environ.get('NZ_INPUT', '/sys/class/input')
DEV = os.environ.get('NZ_DEV', '/dev/input')


def keys_device():
    for d in sorted(os.listdir(INPUT)):
        if not d.startswith('event'):
            continue
        try:
            with open(os.path.join(INPUT, d, 'device', 'name')) as f:
                if f.read().strip() == 'r36s-keys':
                    return os.path.join(DEV, d)
        except OSError:
            pass
    return None


def fn_down(fd):
    buf = bytearray(96)
    fcntl.ioctl(fd, EVIOCGKEY, buf)
    return bool(buf[KEY_MENU // 8] >> (KEY_MENU % 8) & 1)


def main(flag, seconds):
    path = keys_device()
    if not path:
        return 2                     # no FN button here: the trial falls back to "not attached"
    fd = os.open(path, os.O_RDONLY)
    t0 = time.time()
    held_since = None
    while time.time() - t0 < seconds:
        if fn_down(fd):
            held_since = held_since or time.time()
            if time.time() - held_since >= HOLD:
                with open(flag, 'w') as f:
                    f.write('FN held %.1f s at %.1f s\n' % (HOLD, time.time() - t0))
                return 0
        else:
            held_since = None
        time.sleep(0.1)
    return 1


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], float(sys.argv[2])))

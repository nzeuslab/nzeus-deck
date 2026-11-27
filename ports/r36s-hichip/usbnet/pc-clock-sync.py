"""NZEUS Deck clock sync, PC side (2026-10-10).

The GREEN console has no clock chip: Debian restarts from the last time it
saved (every 5 min), so after being switched off its clock is behind. This
script gives it the PC's time over the USB network:

  - run once a minute by the Windows task "NZEUS Deck clock sync" (pythonw:
    no window); it does nothing while the console is not reachable;
  - when the console answers on 192.168.77.1 after having been away, or every
    10 minutes while it stays connected, it logs in ONCE with SSH (alias
    nzeus-deck in ~/.ssh/config) and, if the clocks differ by more than 2 s,
    sets the console's clock to the PC's UTC seconds and saves it
    (nzeus-clock save, so the next restart starts from it);
  - one line per sync in %LOCALAPPDATA%/NZEUS/clock-sync.log.

Run by hand to sync now:  python ports/r36s-hichip/usbnet/pc-clock-sync.py --now
NZ_* environment variables are for the PC test only.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HOST = os.environ.get('NZ_HOST', '192.168.77.1')
ALIAS = os.environ.get('NZ_ALIAS', 'nzeus-deck')
SSH = os.environ.get('NZ_SSH', r'C:\Windows\System32\OpenSSH\ssh.exe')
PING = os.environ.get('NZ_PING', 'ping')
EVERY = 600            # seconds between syncs while the console stays connected
TOLERANCE = 2          # seconds of difference that are left alone
STATE = Path(os.environ.get('NZ_STATE') or Path(os.environ.get('LOCALAPPDATA', '.')) / 'NZEUS' / 'clock-sync.json')
LOG = STATE.with_name('clock-sync.log')
NO_WINDOW = 0x08000000 if os.name == 'nt' else 0   # CREATE_NO_WINDOW: no flashing consoles


def run(cmd, timeout):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, creationflags=NO_WINDOW)


def log(line):
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + line + '\n')


def reachable():
    try:
        r = run([PING, '-n', '1', '-w', '700', HOST], 5)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and 'TTL=' in r.stdout


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {'online': False, 'last_sync': 0}


def save_state(state):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(state))


def sync():
    """One SSH login: the console compares and, if needed, sets + saves its clock."""
    pc = int(time.time())
    remote = ('pc=%d; now=$(date +%%s); d=$((pc - now)); '
              'case $d in -*) a=${d#-} ;; *) a=$d ;; esac; '
              'if test "$a" -gt %d; then date -u -s "@$pc" > /dev/null && '
              '/usr/local/sbin/nzeus-clock save > /dev/null; echo "SET was_off_by=${d}s"; '
              'else echo "OK off_by=${d}s"; fi') % (pc, TOLERANCE)
    r = run([SSH, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4', ALIAS, remote], 25)
    out = (r.stdout.strip() or r.stderr.strip() or 'no output').splitlines()[-1]
    return r.returncode == 0, out


def main():
    force = '--now' in sys.argv
    state = load_state()
    if not reachable():
        if state.get('online'):
            state['online'] = False
            save_state(state)
            log('console went away')
        if force:
            print('console not reachable on', HOST)
        return 0
    if not force and state.get('online') and time.time() - state.get('last_sync', 0) < EVERY:
        return 0
    try:
        ok, out = sync()
    except (OSError, subprocess.TimeoutExpired) as e:
        ok, out = False, 'ssh failed: %s' % e
    log(('synced: ' if ok else 'FAILED: ') + out)
    if force:
        print(out)
    if ok:
        state = {'online': True, 'last_sync': time.time()}
        save_state(state)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

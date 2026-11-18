#!/bin/bash
# USB keyboard test (PC): usbkbd.c is built against 8 fake /dev/input devices
# and a fake clock (tests/usbkbd_compare.c), then plays 40 made-up sessions:
# key presses, Shift/Ctrl/Alt, held keys, plugging and unplugging, a full queue.
# Each session's printout must match its fingerprint in usbkbd_expected.sha256
# (made from the version proven on the console).
#   bash tests/test_usbkbd.sh          check
#   bash tests/test_usbkbd.sh update   write new fingerprints (only after a WANTED change)
set -e
G="$(cd "$(dirname "$0")/.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAKE="-Dopen=t_open -Dread=t_read -Dclose=t_close -Dioctl=t_ioctl -Dclock_gettime=t_clock_gettime -U_FORTIFY_SOURCE -O0"
gcc -Wall -Wextra $FAKE -c "$G/usbkbd.c" -o "$T/k.o"
gcc -Wall -O0 -c "$G/tests/usbkbd_compare.c" -o "$T/h.o"
gcc "$T/k.o" "$T/h.o" -o "$T/run"
for seed in $(seq 1 40); do echo "$seed $("$T/run" $seed | sha256sum | cut -c1-64)"; done > "$T/now"
if [ "$1" = update ]; then cp "$T/now" "$G/tests/usbkbd_expected.sha256"; echo "fingerprints written"; exit 0; fi
if cmp -s "$T/now" "$G/tests/usbkbd_expected.sha256"; then echo "USBKBD TEST: ALL PASS (40 sessions)"
else echo "USBKBD TEST: FAIL - sessions that changed:"; diff "$T/now" "$G/tests/usbkbd_expected.sha256" | grep "^<" | cut -d' ' -f2 | tr '\n' ' '; echo; exit 1; fi

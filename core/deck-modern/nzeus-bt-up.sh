#!/bin/sh
# nzeus-bt-up.sh - NZEUS Deck "Bluetooth on" on the SD Debian. Stock: loaded 7
# add-on modules and copied the firmware. Here Bluetooth, btusb/btrtl, HIDP and
# UHID are built into the kernel and the firmware is in /lib/firmware/rtl_bt
# (firmware-realtek), so this only waits for the dongle's hci0, powers it on
# and starts the nzeus-bt service. The dongle is on the only USB port, so
# Settings > USB must be "Devices" first. Last line = what the Deck shows.
HERE=$(cd "$(dirname "$0")" && pwd)
SW=${NZ_ROLEDIR:-/sys/class/usb_role}/18844000.usb-role-switch
LOG=${LOG:-/tmp/nzeus-bt.log}
PIDFILE=${PIDFILE:-/tmp/nzeus-bt-daemon.pid}
BT=$HERE/nzeus-bt

test "$(cat "$SW/role" 2>/dev/null)" = host ||
    { echo "error switch Settings > USB to Devices first (the dongle needs the USB port)"; exit 1; }
test -e /dev/uhid || echo "warning: no /dev/uhid (LE keyboards/mice will not work)"
i=0
while ! test -e /sys/class/bluetooth/hci0 && test "$i" -lt "${WAIT_HCI:-15}"; do sleep 1; i=$((i + 1)); done
test -e /sys/class/bluetooth/hci0 || { echo "error no Bluetooth adapter (hci0): is the dongle plugged in?"; exit 1; }
tries=0
until out=$("$BT" on 2>&1); do
    tries=$((tries + 1))
    if test "$tries" -ge 3; then
        if dmesg 2>/dev/null | grep -qE "RTL: (HCI_OP_READ_LOCAL_VERSION failed|download fw command failed)"; then
            echo "error the Bluetooth chip is stuck: unplug the dongle for 5 seconds, plug it in again, then switch Bluetooth on"
        else
            echo "$out"
        fi
        exit 1
    fi
    sleep 2
done
echo "$out"
pid=$(cat "$PIDFILE" 2>/dev/null)
if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
    echo "service: already running"
else
    "$BT" daemon >> "$LOG" 2>&1 &
    echo "service: started (log: $LOG)"
fi
echo "Bluetooth ready"
exit 0

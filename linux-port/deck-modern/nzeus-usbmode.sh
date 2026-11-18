#!/bin/sh
# nzeus-usbmode.sh status|pc|host - Settings > USB of NZEUS Deck on the SD
# Debian (nzeus-modern). apps/nzeus-gui/usbmode.c reads these answers:
#   status  one fact per line: mode=pc|host, pc=connected|waiting, adb=off,
#           ip=..., devices=N, error=..., event=...
#   host    "Devices" mode: the USB-C port becomes a USB host (OTG adapter:
#           Wi-Fi/Bluetooth dongle, keyboard, mouse, stick). Uses the proven
#           usb-host-trial.sh (linux-port/usbhost): the Deck's own "switch"
#           press is the owner's "the PC cable is out" (the port cannot sense
#           an unplug), then the trial is confirmed at once, so host mode stays
#           until "pc" or power-off. Every start is PC mode again.
#   pc      back to the PC connection (usb-host-off.sh: device mode, rescue
#           serial port, USB network as at power-on).
# The last line printed is what the Deck shows ("ERROR: ..." = a problem).
#
# SAFETY: host mode puts 5 V on the port. Never plug the PC in while in
# Devices mode. Before switching, this refuses while the PC is still sending
# network packets on usb0 (the only sign of a PC this port can give).
# Works with dash and BusyBox ash. NZ_* variables: PC tests only.
LIB=${NZ_USBHOST_LIB:-/usr/local/lib/nzeus/usbhost}
SW=${NZ_ROLEDIR:-/sys/class/usb_role}/18844000.usb-role-switch
STATE=${NZ_USBHOST_STATE:-/run/nzeus-usbhost}
CFG=${NZ_CONFIGFS:-/sys/kernel/config}/usb_gadget
UDCDIR=${NZ_UDCDIR:-/sys/class/udc}
USBDEV=${NZ_USBDEV:-/sys/bus/usb/devices}
NET=${NZ_SYSNET:-/sys/class/net}
HERE=$(cd "$(dirname "$0")" && pwd)
WAIT=${NZ_SWITCH_WAIT:-25}

role() { cat "$SW/role" 2>/dev/null; }

device_count() {
    n=0
    for d in "$USBDEV"/*; do
        case "${d##*/}" in *:*|usb*) continue ;; esac
        test -r "$d/idVendor" && n=$((n + 1))
    done
    echo "$n"
}

ip4() { ip -4 -o addr show dev "$1" 2>/dev/null | awk '{ split($4, a, "/"); print a[1]; exit }'; }

do_status() {
    r=$(role)
    if test -z "$r"; then
        echo "mode=pc"
        echo "error=this kernel has no USB role switch (needs the gma-native-host kernel)"
        return 0
    fi
    if test "$r" = host; then
        echo "mode=host"
        echo "devices=$(device_count)"
        return 0
    fi
    echo "mode=pc"
    pc=waiting
    for g in "$CFG"/*; do
        test -n "$(cat "$g/UDC" 2>/dev/null)" || continue
        udc=$(cat "$g/UDC")
        test "$(cat "$UDCDIR/$udc/state" 2>/dev/null)" = configured && pc=connected
    done
    echo "pc=$pc"
    echo "adb=off"
    a=$(ip4 usb0)
    echo "ip=${a:-none}"
    return 0
}

# Is the PC still there? It sends packets on usb0 (USB network mode) every
# few seconds; a quiet usb0 or no usb0 cannot be told apart from "unplugged".
pc_talking() {
    test -r "$NET/usb0/statistics/rx_packets" || return 1
    a=$(cat "$NET/usb0/statistics/rx_packets")
    sleep "${NZ_PC_LISTEN:-4}"
    b=$(cat "$NET/usb0/statistics/rx_packets" 2>/dev/null)
    test -n "$b" && test "$b" -gt "$a"
}

wait_state() {   # wait_state PATTERN: until the trial's state matches (or WAIT s)
    n=0
    while test "$n" -lt "$WAIT"; do
        st=$(cat "$STATE/state" 2>/dev/null)
        case "$st" in $1) return 0 ;; esac
        case "$st" in failed) return 1 ;; esac
        sleep 1
        n=$((n + 1))
    done
    return 1
}

do_host() {
    test -e "$SW/role" || { echo "ERROR: this kernel has no USB host mode"; return 1; }
    test "$(role)" = host && { echo "Already in Devices mode"; return 0; }
    if pc_talking; then
        echo "ERROR: the PC is still connected (it is sending data). Unplug the PC cable first."
        return 1
    fi
    # the trial waits for "FN held 2 s"; this press in the Deck is that answer
    out=$(NZ_FN_WATCH="sh $HERE/flag-now.sh" NZ_UNPLUG_WAIT=30 sh "$LIB/usb-host-trial.sh" 300 2>&1)
    case "$out" in
        *USBHOST_TRIAL_STARTED*) ;;
        *) echo "ERROR: $(echo "$out" | tail -n 1)"; return 1 ;;
    esac
    if ! wait_state 'host-[0-9]*'; then
        echo "ERROR: the port did not switch (see /data/nzeus/usbhost.log)"
        return 1
    fi
    sh "$LIB/usb-host-confirm.sh" > /dev/null 2>&1 ||
        { echo "ERROR: Devices mode is on but could not be kept: it switches back in 5 min"; return 1; }
    # Wi-Fi: join a saved network by itself once the dongle is there (unless switched off)
    setsid sh "$HERE/wifi-auto.sh" > /tmp/nzeus-wifi-auto.log 2>&1 < /dev/null &
    echo "Devices mode on: plug in the OTG adapter. Never plug the PC in now."
    return 0
}

do_pc() {
    test "$(role)" = host || { echo "Already in PC mode"; return 0; }
    # Hardware 2026-10-11: switching back with the Wi-Fi/BT dongle still plugged
    # in (Bluetooth on, battery ~40 %) cut the console's power at once (twice it
    # had worked with the dongle in). So: the devices come out FIRST.
    if test "$(device_count)" -gt 0; then
        echo "ERROR: unplug the OTG adapter first (USB devices still plugged in: $(device_count)), then choose PC connection"
        return 1
    fi
    sh "$HERE/wifi-join.sh" disconnect > /dev/null 2>&1      # the Deck's own Wi-Fi (wpa + DHCP)
    test -x "$HERE/nzeus-bt" && "$HERE/nzeus-bt" off > /dev/null 2>&1
    out=$(sh "$LIB/usb-host-off.sh" 2>&1)
    case "$out" in
        *USBHOST_OFF*|*USBHOST_ALREADY_DEVICE*) ;;
        *) echo "ERROR: $(echo "$out" | tail -n 1)"; return 1 ;;
    esac
    n=0
    while test "$(role)" != device && test "$n" -lt "$WAIT"; do sleep 1; n=$((n + 1)); done
    test "$(role)" = device || { echo "ERROR: still in Devices mode: hold power 3 s (off), start again"; return 1; }
    wait_state device || true
    echo "PC mode: unplug the OTG adapter, then plug the PC in"
    return 0
}

case "$1" in
    status) do_status ;;
    host)   do_host ;;
    pc)     do_pc ;;
    *)      echo "usage: nzeus-usbmode.sh status|pc|host"; exit 2 ;;
esac

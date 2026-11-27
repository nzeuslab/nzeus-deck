#!/bin/sh
# wifi-on.sh - NZEUS Deck "turn Wi-Fi on" on the SD Debian. The driver (rtw88)
# is built into the kernel, so "on" = the dongle's wlan0 switched up. The
# dongle is on the only USB port, so Settings > USB must be "Devices" first.
# If no network is saved in the Deck yet but /data/nzeus/wifi.conf exists (the
# owner's file for usb-host-trial --wifi), that network is saved once from it;
# its password never leaves the console and is never printed.
# Last line = what the Deck shows. NZ_* variables: PC tests only.
SW=${NZ_ROLEDIR:-/sys/class/usb_role}/18844000.usb-role-switch
NET=${NZ_SYSNET:-/sys/class/net}
USBDEV=${NZ_USBDEV:-/sys/bus/usb/devices}
HERE=$(cd "$(dirname "$0")" && pwd)
DECK=${NZ_DECK_DIR:-/tmp/nzeus-data/nzeus}
OWNERCONF=${NZ_OWNER_WIFI:-/data/nzeus/wifi.conf}
IFACE=wlan0

usb_has() {
    for d in "$USBDEV"/*; do
        test -r "$d/idVendor" || continue
        test "$(cat "$d/idVendor"):$(cat "$d/idProduct" 2>/dev/null)" = "$1" && return 0
    done
    return 1
}

test "$(cat "$SW/role" 2>/dev/null)" = host ||
    { echo "error: switch Settings > USB to Devices first (the dongle needs the USB port)"; exit 1; }
if usb_has 0bda:1a2b; then
    echo "the dongle is in 'driver CD' mode: switching it to Wi-Fi"
    /usr/local/sbin/nz-cdeject > /dev/null 2>&1
fi
n=0
while ! test -e "$NET/$IFACE" && test "$n" -lt "${NZ_WAIT_WLAN:-20}"; do sleep 1; n=$((n + 1)); done
test -e "$NET/$IFACE" || { echo "error: no Wi-Fi interface: is the dongle plugged in?"; exit 1; }
ip link set "$IFACE" up || { echo "error: could not switch $IFACE on"; exit 1; }

# the owner's network from /data/nzeus/wifi.conf, saved once (WPA2/WPA3-mixed routers take the PSK)
if test -r "$OWNERCONF" && ! grep -q '^network=' "$DECK/wifi/networks.conf" 2>/dev/null; then
    s=$(sed -n 's/^ssid=//p' "$OWNERCONF" | head -n 1)
    p=$(sed -n 's/^psk=//p' "$OWNERCONF" | head -n 1)
    if test -n "$s"; then
        sh "$HERE/wifi-join.sh" connect "$s" "$p" > /dev/null 2>&1 &&
            echo "saved and joining your network from /data/nzeus/wifi.conf"
    fi
    s=; p=
fi
echo "Wi-Fi is on"
exit 0

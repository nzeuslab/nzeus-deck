#!/bin/sh
# wifi-auto.sh - started by nzeus-usbmode.sh when Devices mode comes on: waits
# up to 60 s for the Wi-Fi dongle, then switches Wi-Fi on and joins whichever
# SAVED network is in reach - unless Settings > Wi-Fi > "Join saved networks"
# is off (the Deck's flag file wifi/autojoin-off).
HERE=$(cd "$(dirname "$0")" && pwd)
DECK=${NZ_DECK_DIR:-/tmp/nzeus-data/nzeus}
test -e "$DECK/wifi/autojoin-off" && { echo "auto-join is off"; exit 0; }
n=0
while ! test -e /sys/class/net/wlan0 && test "$n" -lt 60; do sleep 2; n=$((n + 2)); done
test -e /sys/class/net/wlan0 || { echo "no Wi-Fi dongle"; exit 0; }
sh "$HERE/wifi-on.sh" || exit 1
# (wifi-on.sh may have just saved and joined the owner's network)
case "$(sh "$HERE/wifi-join.sh" status 2>/dev/null)" in
    state=NOT_RUNNING) sh "$HERE/wifi-join.sh" auto || exit 0 ;;
esac
sh "$HERE/nzeus-net-up.sh"

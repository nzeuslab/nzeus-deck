#!/bin/sh
# wifi-join.sh - NZEUS Deck's Wi-Fi joining on the SD Debian: runs the joiner
# wifi-join-core.sh (next to this file) with Debian's wpa_supplicant and the
# Deck's own saved-networks file. Arguments and answers: see that file.
# Extra on "disconnect": the DHCP client of nzeus-net-up.sh stops too and
# wlan0's address goes away.
HERE=$(cd "$(dirname "$0")" && pwd)
DECK=${NZ_DECK_DIR:-/tmp/nzeus-data/nzeus}
W=/run/nzeus-wpa-bin                      # wpa_supplicant, wpa_cli, wpa_passphrase in one folder
mkdir -p "$W"
for p in /usr/sbin/wpa_supplicant /usr/sbin/wpa_cli /usr/bin/wpa_passphrase /sbin/wpa_supplicant /sbin/wpa_cli; do
    test -x "$p" && ! test -e "$W/${p##*/}" && ln -s "$p" "$W/${p##*/}"
done
NZ_WPA_BIN=$W
NZ_WPA_SAVED=$DECK/wifi/networks.conf
export NZ_WPA_BIN NZ_WPA_SAVED
if test "$1" = disconnect; then
    pid=$(cat /run/nzeus-deck-udhcpc.pid 2>/dev/null)
    case "$pid" in ''|*[!0-9]*) ;; *) kill "$pid" 2>/dev/null ;; esac
    ip -4 addr flush dev wlan0 2>/dev/null
fi
exec sh "$HERE/wifi-join-core.sh" "$@"

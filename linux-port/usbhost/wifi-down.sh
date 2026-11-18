#!/bin/sh
# wifi-down.sh - stop what wifi-up.sh started: DHCP client, wpa_supplicant,
# addresses on the Wi-Fi interface; the made-up config in /run/nzeus-wifi is
# overwritten with nothing. SSH keeps running (it listens on every address).
# Safe to run when Wi-Fi is not up. NZ_* variables: PC tests only.
RUN=${NZ_WIFI_RUN:-/run/nzeus-wifi}
NET=${NZ_NETDIR:-/sys/class/net}
KMSG=${NZ_KMSG:-/dev/kmsg}
IP=${NZ_IP:-ip}
pid_alive() {
    case "$1" in ''|0|*[!0-9]*) return 1 ;; esac
    kill -0 "$1" 2>/dev/null
}
for p in udhcpc wpa; do
    pid=$(cat "$RUN/$p.pid" 2>/dev/null)
    if pid_alive "$pid"; then
        kill "$pid" 2>/dev/null
        echo "wifi: stopped $p ($pid)"
    fi
    test -f "$RUN/$p.pid" && : > "$RUN/$p.pid"
done
for i in "$NET"/*; do
    if test -e "$i/phy80211" || test -d "$i/wireless"; then
        $IP addr flush dev "${i##*/}" 2>/dev/null
        $IP link set "${i##*/}" down 2>/dev/null
    fi
done
test -f "$RUN/wpa.conf" && : > "$RUN/wpa.conf"
echo "wifi: down" >> "$KMSG" 2>/dev/null
echo "WIFI_DOWN"
exit 0

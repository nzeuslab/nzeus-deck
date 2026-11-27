#!/bin/sh
# nzeus-net-up.sh - NZEUS Deck starts this once the Wi-Fi link is joined
# (state=COMPLETED). On the stock system it started nzeus-netd (user-space
# TCP/IP); on the SD Debian the kernel does TCP/IP, so this only gets an
# address: BusyBox udhcpc on wlan0 with the usbhost DHCP script (address,
# default route, DNS). It keeps renewing in the background.
IFACE=wlan0
PIDF=/run/nzeus-deck-udhcpc.pid
SCRIPT=${NZ_DHCP_SCRIPT:-/usr/local/lib/nzeus/usbhost/udhcpc.script}
pid=$(cat "$PIDF" 2>/dev/null)
case "$pid" in
    ''|*[!0-9]*) ;;
    *) if kill -0 "$pid" 2>/dev/null; then echo "DHCP already running"; exit 0; fi ;;
esac
busybox udhcpc -i "$IFACE" -s "$SCRIPT" -p "$PIDF" -t 5 -T 3 -A 10 -b > /dev/null 2>&1 ||
    { echo "error: the DHCP client did not start"; exit 1; }
echo "getting an address on $IFACE"
exit 0

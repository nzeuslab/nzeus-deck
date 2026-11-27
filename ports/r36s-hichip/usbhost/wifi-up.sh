#!/bin/sh
# wifi-up.sh - join your Wi-Fi with the RTL8821CU dongle (USB host mode must be
# on: usb-host-trial.sh), get an address by DHCP, and make sure SSH (Dropbear,
# keys only) is running: the SECOND LIFELINE while the USB port is a host.
#
# Your network name and password live ONLY on the console, in
#   /data/nzeus/wifi.conf     (owner root, mode 600 - this script refuses others)
# with these lines (no quotes, everything after the first '=' is the value):
#   ssid=MyNetwork
#   psk=my wifi password
#   security=auto            optional: auto (WPA2 or WPA3), wpa2, wpa3
# That file is never in git (.gitignore) and never printed or logged here.
# Example: ports/r36s-hichip/usbhost/wifi.conf.example.
#
# Steps: dongle in "driver CD" mode (0bda:1a2b)? -> nz-cdeject; wait for the
# Wi-Fi interface; wpa_supplicant (Debian, nl80211) with a config made in
# /run/nzeus-wifi (RAM, mode 600); Debian BusyBox udhcpc; nzeus-ssh start.
# Undo: wifi-down.sh (usb-host-off.sh does it too).
# Logs: /data/nzeus/wifi.log and the kernel log. For the SD Debian root (dash);
# also BusyBox-ash compatible. NZ_* variables: PC tests only.
CONF=${NZ_WIFI_CONF:-/data/nzeus/wifi.conf}
RUN=${NZ_WIFI_RUN:-/run/nzeus-wifi}
NET=${NZ_NETDIR:-/sys/class/net}
USBDEV=${NZ_USBDEV:-/sys/bus/usb/devices}
ROLEDIR=${NZ_ROLEDIR:-/sys/class/usb_role}
KMSG=${NZ_KMSG:-/dev/kmsg}
LOG=${NZ_WIFI_LOG:-/data/nzeus/wifi.log}
LIB=${NZ_USBHOST_LIB:-/usr/local/lib/nzeus/usbhost}
EJECT=${NZ_CDEJECT:-/usr/local/sbin/nz-cdeject}
WPA=${NZ_WPA_SUPPLICANT:-wpa_supplicant}
WPACLI=${NZ_WPA_CLI:-wpa_cli}
UDHCPC=${NZ_UDHCPC:-busybox udhcpc}
IP=${NZ_IP:-ip}
SSHINIT=${NZ_SSH_INIT:-/etc/init.d/nzeus-ssh}
WAIT_IF=${NZ_WIFI_IF_WAIT:-20}
WAIT_ASSOC=${NZ_WIFI_ASSOC_WAIT:-40}
SW=$ROLEDIR/18844000.usb-role-switch

say() {
    echo "wifi: $*"
    echo "wifi: $*" >> "$KMSG" 2>/dev/null
    if mkdir -p "${LOG%/*}" 2>/dev/null; then
        echo "[$(cut -d' ' -f1 /proc/uptime 2>/dev/null)] $*" >> "$LOG" 2>/dev/null
    fi
}
die() { say "$*"; exit 1; }
pid_alive() {
    case "$1" in ''|0|*[!0-9]*) return 1 ;; esac
    kill -0 "$1" 2>/dev/null
}
has_usb_id() {
    local d
    for d in "$USBDEV"/*; do
        case "${d##*/}" in *:*) continue ;; esac
        test "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = "$1" && return 0
    done
    return 1
}
wifi_if() {
    local i
    for i in "$NET"/*; do
        if test -e "$i/phy80211" || test -d "$i/wireless"; then echo "${i##*/}"; return 0; fi
    done
    return 0
}
# value KEY: the text after "KEY=" on the first such line (literal, no quotes;
# a Windows line end is dropped)
value() {
    awk -v k="$1" 'index($0, k "=") == 1 { v = substr($0, length(k) + 2); sub(/\r$/, "", v); print v; exit }' "$CONF"
}

# options: --no-dhcp (only join), --wait SECONDS (wait that long for the
# dongle's Wi-Fi interface; usb-host-trial.sh --wifi uses 120)
dhcp=1
while test $# -gt 0; do
    case "$1" in
        --no-dhcp) dhcp=0 ;;
        --wait)
            case "$2" in ''|*[!0-9]*) echo "wifi: --wait needs a number of seconds"; exit 1 ;; esac
            WAIT_IF=$2; shift ;;
        --help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "usage: wifi-up.sh [--no-dhcp] [--wait SECONDS]"; exit 1 ;;
    esac
    shift
done
test -e "$SW/role" || die "no USB role switch: needs the gma-native-host kernel"
test "$(cat "$SW/role" 2>/dev/null)" = host || die "the USB port is not in host mode: run usb-host-trial.sh first"

# ---- the settings file: must exist, be a plain file, root only
test -f "$CONF" || die "no $CONF (copy ports/r36s-hichip/usbhost/wifi.conf.example there, then chmod 600)"
perm=$(ls -ln "$CONF" | cut -c1-10)
owner=$(ls -ln "$CONF" | awk '{ print $3 }')
test "$perm" = "-rw-------" || die "$CONF must be readable by root only: chmod 600 $CONF (now $perm)"
test "$owner" = 0 || die "$CONF must belong to root (now uid $owner): chown root $CONF"
ssid=$(value ssid)
psk=$(value psk)
security=$(value security)
security=${security:-auto}
test -n "$ssid" || die "$CONF has no ssid= line"
test "${#ssid}" -le 32 || die "ssid is longer than 32 characters"
case "$security" in auto|wpa2|wpa3) ;; *) die "security must be auto, wpa2 or wpa3 (not '$security')" ;; esac
case "$psk" in
    *[!0-9a-fA-F]*|'') hexpsk=0 ;;
    *) hexpsk=1 ;;
esac
if test "$hexpsk" = 1 && test "${#psk}" = 64; then
    test "$security" = wpa3 && die "WPA3 needs the password itself, not a 64-digit hex key"
else
    hexpsk=0
    test "${#psk}" -ge 8 && test "${#psk}" -le 63 || die "psk must be 8..63 characters (or 64 hex digits)"
fi

# ---- the dongle: eject the driver CD if needed, then wait for the interface
if has_usb_id 0bda:1a2b; then
    say "dongle is in driver-CD mode (0bda:1a2b): ejecting it"
    "$EJECT" || die "nz-cdeject failed (see above)"
fi
n=0
ifname=$(wifi_if)
while test -z "$ifname"; do
    n=$((n + 1))
    test "$n" -gt "$WAIT_IF" && die "no Wi-Fi interface after $WAIT_IF s (dongle plugged in? usb-host-status.sh, dmesg | grep rtw)"
    sleep 1
    ifname=$(wifi_if)
done
say "Wi-Fi interface $ifname"

# ---- wpa_supplicant config in RAM, root only. The SSID is written as hex and
# the password between quotes (wpa_supplicant takes everything up to the LAST
# quote), so no character in them can break the file.
case "$security" in
    auto) keymgmt='WPA-PSK WPA-PSK-SHA256 SAE'; pmf=1 ;;
    wpa2) keymgmt='WPA-PSK WPA-PSK-SHA256'; pmf=1 ;;
    wpa3) keymgmt='SAE'; pmf=2 ;;
esac
# hex straight from the file (no echo/printf in between); drop the line end "0a"
# (od -b: octal bytes, understood by coreutils and the smallest BusyBox od;
# the first field of every od line is the offset)
ssidhex=$(value ssid | od -b -v | awk '{ for (i = 2; i <= NF; i++) h = h sprintf("%02x", substr($i, 1, 1) * 64 + substr($i, 2, 1) * 8 + substr($i, 3, 1)) } END { print h }')
ssidhex=${ssidhex%0a}
test "${#ssidhex}" -ge 2 || die "could not encode the ssid"
old=$(umask)
umask 077
mkdir -p "$RUN" || die "cannot make $RUN"
chmod 700 "$RUN"
# printf '%s', never echo: dash's echo would turn a "\1" in a password into a byte
{
    printf '%s\n' "ctrl_interface=$RUN/ctrl" "update_config=0" "network={" "    ssid=$ssidhex"
    if test "$hexpsk" = 1; then printf '    psk=%s\n' "$psk"; else printf '    psk="%s"\n' "$psk"; fi
    printf '%s\n' "    key_mgmt=$keymgmt" "    ieee80211w=$pmf" "}"
} > "$RUN/wpa.conf"
umask "$old"
psk=

# ---- start (stop an older run first)
pid_alive "$(cat "$RUN/wpa.pid" 2>/dev/null)" && kill "$(cat "$RUN/wpa.pid")"
pid_alive "$(cat "$RUN/udhcpc.pid" 2>/dev/null)" && kill "$(cat "$RUN/udhcpc.pid")"
sleep 1
$IP link set "$ifname" up || die "could not bring $ifname up"
$WPA -B -D nl80211 -i "$ifname" -c "$RUN/wpa.conf" -P "$RUN/wpa.pid" -f "$RUN/wpa.log" ||
    die "wpa_supplicant did not start (log $RUN/wpa.log)"
say "wpa_supplicant started ($security); waiting up to $WAIT_ASSOC s to join"
n=0
while true; do
    st=$($WPACLI -p "$RUN/ctrl" -i "$ifname" status 2>/dev/null | sed -n 's/^wpa_state=//p')
    test "$st" = COMPLETED && break
    n=$((n + 1))
    test "$n" -gt "$WAIT_ASSOC" && die "not joined after $WAIT_ASSOC s (state '$st'; wrong password or name? log $RUN/wpa.log)"
    sleep 1
done
say "joined (wpa_state COMPLETED)"
if test "$dhcp" = 1; then
    $UDHCPC -i "$ifname" -s "$LIB/udhcpc.script" -p "$RUN/udhcpc.pid" -t 5 -T 3 -A 10 -b ||
        say "udhcpc did not start"
    n=0
    addr=
    while test -z "$addr" && test "$n" -lt 20; do
        addr=$($IP -4 addr show dev "$ifname" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -n 1)
        test -n "$addr" && break
        sleep 1
        n=$((n + 1))
    done
    if test -n "$addr"; then say "address $addr on $ifname"; else say "no DHCP address yet (udhcpc keeps trying in the background)"; fi
fi
# ---- the second lifeline: SSH (keys only) on every address
if test -x "$SSHINIT"; then
    if ! "$SSHINIT" status > /dev/null 2>&1; then "$SSHINIT" start; fi
    if "$SSHINIT" status > /dev/null 2>&1; then
        say "SSH is running: ssh root@${addr:-<address>} (keys only)"
    else
        say "WARNING: SSH is NOT running (no /root/.ssh/authorized_keys?): no second lifeline"
    fi
fi
say "WIFI_UP $ifname ${addr:-no-address}"
exit 0

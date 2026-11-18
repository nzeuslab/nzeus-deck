#!/bin/sh
# wifi-scan.sh OUTFILE - NZEUS Deck's Wi-Fi scan on the SD Debian. The stock
# Realtek driver had a scan page (/proc/net/*/wlan0/survey_info); the kernel's
# rtw88 driver has none, so this asks "iw" and writes the SAME table the Deck
# already reads (apps/nzeus-gui/wifi.c, wifi_parse_survey):
#   index  bssid  channel  rssi  signal-dBm  noise  age  [FLAGS]  SSID
# FLAGS always ends in ']' ([WPA2], [WPA], [WEP] or [ESS] for open), so the
# SSID is whatever follows the last ']'. Written to a temporary file first,
# then renamed, so the Deck never reads half a list.
OUT=${1:-/tmp/nzeus-wifi-survey.txt}
IFACE=${NZ_WIFI_IFACE:-wlan0}
IW=${NZ_IW:-iw}
n=0
until res=$($IW dev "$IFACE" scan 2>&1); do
    n=$((n + 1))
    # busy (a join is scanning too): use what the driver found last time
    test "$n" -ge 3 && { res=$($IW dev "$IFACE" scan dump 2>&1) || exit 1; break; }
    sleep 2
done
echo "$res" | awk '
    function flush() {
        if (bssid == "") return
        if (fl == "") fl = priv ? "[WEP]" : "[ESS]"
        ch = 0
        if (freq >= 2412 && freq <= 2472) ch = (freq - 2407) / 5
        else if (freq == 2484) ch = 14
        else if (freq >= 5000) ch = (freq - 5000) / 5
        idx++
        printf "%3d  %s  %3d  %3d  %4d  %5d  %5d  %32s  %32s\n", idx, bssid, ch, 0, sig, 0, 0, fl, ssid
    }
    /^BSS / { flush(); bssid = substr($2, 1, 17); freq = 0; sig = -100; ssid = ""; fl = ""; priv = 0; next }
    /^[ \t]+freq:/      { freq = int($2) }
    /^[ \t]+signal:/    { sig = int($2) }
    /^[ \t]+SSID:/      { ssid = $0; sub(/^[ \t]+SSID: ?/, "", ssid) }
    /^[ \t]+RSN:/       { if (index(fl, "[WPA2]") == 0) fl = fl "[WPA2]" }
    /^[ \t]+WPA:/       { if (index(fl, "[WPA]") == 0) fl = fl "[WPA]" }
    /^[ \t]+capability:.*Privacy/ { priv = 1 }
    END { flush() }
' > "$OUT.tmp" && mv "$OUT.tmp" "$OUT"

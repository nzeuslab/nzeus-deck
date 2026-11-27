#!/bin/sh
# wifi-join.sh - join a Wi-Fi network (WPA2-PSK or open) with wpa_supplicant.
# For NZEUS OS on the R36S console; works in busybox ash (no bash needed).
#
#   wifi-join.sh connect "SSID" "PASSWORD"   empty PASSWORD = open network
#   wifi-join.sh status                      prints ONE line, for example:
#        state=COMPLETED ssid=Home Net bssid=aa:bb:cc:dd:ee:ff freq=2437
#        state=SCANNING   (or ASSOCIATING, 4WAY_HANDSHAKE, DISCONNECTED ...)
#        state=NOT_RUNNING
#   wifi-join.sh disconnect                  stops wpa_supplicant
#   wifi-join.sh auto                        join whichever SAVED network is in reach
#                                            (exit 4 = no saved networks yet)
#   wifi-join.sh use "SSID"                  join one saved network (its saved key)
#   wifi-join.sh saved                       list the saved networks, one per line:
#                                            "<name in hex> <name>" (hex: exact even
#                                            with odd characters; name: for people)
#   wifi-join.sh forget "SSID"               delete a saved network (and its key)
#
# Reading the status line in a program: the SSID may contain spaces, so take
# bssid= and freq= from the END of the line; ssid= is everything between
# "ssid=" and the LAST " bssid=". wpa_supplicant writes odd characters in the
# SSID as \xNN, a " as \" and a \ as \\.
#
# This is only the Wi-Fi link (association + WPA2 4-way handshake). There is
# no IP address yet: the console's kernel has no IPv4.
#
# Where things go:
#   saved networks  /mnt/sdcard/nzeus/wifi/networks.conf  (kept for later;
#                   the password is NOT stored, only the key made from it -
#                   but FAT32 can't protect files, so anyone with the card
#                   can read that key and join that network)
#   running config  /tmp/nzeus-wpa.conf   (RAM only, deleted on disconnect)
#   control socket  /tmp/nzeus-wpa/wlan0  (wpa_cli talks to it)
#   log             /tmp/nzeus-wpa.log
#
# Every setting can be changed with an environment variable (used by tests):
BIN=${NZ_WPA_BIN:-/mnt/sdcard/nzeus/bin}           # wpa_supplicant, wpa_cli, wpa_passphrase
IFACE=${NZ_WIFI_IFACE:-wlan0}
DRIVER=${NZ_WPA_DRIVER:-nl80211}                     # see WPA_README.md: wext can't work on the stock kernel
SAVED=${NZ_WPA_SAVED:-/mnt/sdcard/nzeus/wifi/networks.conf}
RUNCONF=${NZ_WPA_RUNCONF:-/tmp/nzeus-wpa.conf}
CTRL=${NZ_WPA_CTRL:-/tmp/nzeus-wpa}
LOG=${NZ_WPA_LOG:-/tmp/nzeus-wpa.log}
PIDF=${NZ_WPA_PID:-/tmp/nzeus-wpa.pid}
# NZ_WPA_DEBUG=-d writes much more detail to the log. The log is in RAM (/tmp)
# and the console has only ~5 MB free, so use -d only for short tests.
DEBUG=${NZ_WPA_DEBUG:-}
SYSNET=${NZ_SYSNET:-/sys/class/net}

# a newline character (to refuse passwords that contain one)
NL='
'

err() { echo "error: $*" >&2; }

# Is $1 exactly 64 hex digits (a finished WPA key instead of a password)?
is_hex64() {
    [ ${#1} -eq 64 ] || return 1
    case "$1" in *[!0-9a-fA-F]*) return 1 ;; esac
}

# Text -> hex digits, e.g. "Hi" -> 4869. A hex SSID needs no quoting at all,
# so quotes, backslashes and other odd characters in a name can't break the config.
to_hex() { printf '%s' "$1" | od -An -tx1 -v | tr -d ' \n'; }

# Ask wpa_cli; prints its answer, fails if wpa_supplicant isn't answering.
cli() { "$BIN/wpa_cli" -p "$CTRL" -i "$IFACE" "$@" 2>/dev/null; }

# Is the wpa_supplicant from our pid file still running?
running_pid() {
    pid=$(cat "$PIDF" 2>/dev/null)
    [ -n "$pid" ] && [ -d "/proc/$pid" ] || return 1
    # make sure the number wasn't reused by another program
    tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q wpa_supplicant || return 1
    echo "$pid"
}

do_status() {
    out=$(cli status)
    state=$(printf '%s\n' "$out" | sed -n 's/^wpa_state=//p')
    if [ -z "$state" ]; then
        echo "state=NOT_RUNNING"
    elif [ "$state" = "COMPLETED" ]; then
        ssid=$(printf '%s\n' "$out" | sed -n 's/^ssid=//p')
        bssid=$(printf '%s\n' "$out" | sed -n 's/^bssid=//p')
        freq=$(printf '%s\n' "$out" | sed -n 's/^freq=//p')
        printf 'state=COMPLETED ssid=%s bssid=%s freq=%s\n' "$ssid" "$bssid" "$freq"
    else
        echo "state=$state"
    fi
}

do_disconnect() {
    cli terminate > /dev/null              # polite: wpa_supplicant leaves the network itself
    n=0
    while pid=$(running_pid) && [ $n -lt 5 ]; do sleep 1; n=$((n + 1)); done
    if pid=$(running_pid); then            # still there: stop it the hard way
        kill "$pid" 2>/dev/null; sleep 1
        pid=$(running_pid) && kill -9 "$pid" 2>/dev/null
    fi
    rm -f "$PIDF" "$RUNCONF"               # the running config holds the Wi-Fi key
    echo "state=NOT_RUNNING"
}

# Write the network block for $1=SSID $2=password to stdout.
network_block() {
    ssid_hex=$(to_hex "$1")
    # a readable name for people looking at the file (odd characters -> ?)
    # (sed, not tr '[:print:]': the console's busybox tr turned EVERY letter into ?)
    printf '# nzeus ssid: %s\n' "$(printf '%s' "$1" | LC_ALL=C sed 's/[^ -~]/?/g')"
    echo "network={"
    echo "	ssid=$ssid_hex"
    if [ -z "$2" ]; then
        echo "	key_mgmt=NONE"                      # open network, no password
    else
        echo "	key_mgmt=WPA-PSK"
        if is_hex64 "$2"; then
            echo "	psk=$2"                         # already a 64-digit key
        else
            # wpa_passphrase turns the password into the 64-digit key (PBKDF2).
            # The password goes in through a pipe (not the command line, where
            # other programs could see it), and we keep ONLY the key line -
            # wpa_passphrase also prints the password as a comment ("#psk=").
            key=$(printf '%s\n' "$2" | "$BIN/wpa_passphrase" "$1" 2>/dev/null | sed -n 's/^	psk=//p')
            [ ${#key} -eq 64 ] || return 1
            echo "	psk=$key"
        fi
    fi
    echo "}"
}

# Save the block in networks.conf, replacing an older entry for the same SSID.
save_network() {   # $1 = SSID in hex, $2 = file with the new block
    mkdir -p "$(dirname "$SAVED")" || return 1
    touch "$SAVED" || return 1
    awk -v hex="$1" '
        /^# nzeus ssid:/ { pending = $0 "\n"; next }
        /^network=\{/    { inblk = 1; drop = 0; buf = pending $0 "\n"; pending = ""; next }
        inblk {
            buf = buf $0 "\n"
            if ($0 ~ ("^[ \t]*ssid=" hex "$")) drop = 1
            if ($0 ~ /^[ \t]*}/) { inblk = 0; if (!drop) printf "%s", buf }
            next
        }
        { printf "%s", pending; pending = ""; print }
        END { printf "%s", pending }
    ' "$SAVED" > "$SAVED.new" && cat "$2" >> "$SAVED.new" && mv "$SAVED.new" "$SAVED"
}

# Print saved network blocks (with their "# nzeus ssid:" line): all of them,
# or only the one whose hex name is $1.
saved_blocks() {
    [ -f "$SAVED" ] || return 0
    awk -v hex="$1" '
        /^# nzeus ssid:/ { pending = $0 "\n"; next }
        /^network=\{/    { inblk = 1; keep = (hex == ""); buf = pending $0 "\n"; pending = ""; next }
        inblk {
            buf = buf $0 "\n"
            if (hex != "" && $0 ~ ("^[ \t]*ssid=" hex "$")) keep = 1
            if ($0 ~ /^[ \t]*}/) { inblk = 0; if (keep) printf "%s", buf }
            next
        }
        { pending = "" }
    ' "$SAVED"
}

# (Re)start wpa_supplicant with the network block(s) in file $1, then print the status.
start_wpa() {
    do_disconnect > /dev/null              # stop an older one (if any)
    { echo "# made by wifi-join.sh - holds the Wi-Fi key, deleted on disconnect"
      echo "ctrl_interface=$CTRL"
      cat "$1"; } > "$RUNCONF"
    rm -f "$1"
    : > "$LOG"                              # start a fresh log (it lives in RAM)
    # -B background  -t time stamps  -f log file  -P pid file
    if ! "$BIN/wpa_supplicant" -B -t $DEBUG -i "$IFACE" -D "$DRIVER" -c "$RUNCONF" -P "$PIDF" -f "$LOG"; then
        err "wpa_supplicant did not start - last lines of $LOG:"
        tail -n 5 "$LOG" >&2
        rm -f "$RUNCONF"
        echo "state=NOT_RUNNING"
        return 1
    fi
    # wait (max 5 s) until its control socket answers
    n=0
    while [ $n -lt 5 ] && ! cli ping | grep -q PONG; do sleep 1; n=$((n + 1)); done
    do_status
}

# Things every join needs: the programs and the Wi-Fi interface.
check_ready() {
    for p in wpa_supplicant wpa_cli wpa_passphrase; do
        [ -x "$BIN/$p" ] || { err "$BIN/$p not found"; return 3; }
    done
    [ -d "$SYSNET/$IFACE" ] || { err "no $IFACE - load the Wi-Fi driver first (wifitest)"; return 3; }
}

# auto: all saved networks at once - wpa_supplicant joins the one in reach
# (the strongest if several are). use SSID: only that saved network.
do_saved_join() {
    check_ready || return $?
    umask 077
    blk="$RUNCONF.blk"
    saved_blocks "$1" > "$blk"
    if ! grep -q '^network={' "$blk"; then
        rm -f "$blk"
        if [ -n "$1" ]; then err "that network is not saved - join it with its password first"; return 2; fi
        err "no saved networks yet - join one with its password first"; return 4
    fi
    start_wpa "$blk"
}

# saved: "<hex> <readable name>" per line (the readable name is the comment line)
do_list() {
    saved_blocks "" | awk '
        /^# nzeus ssid:/ { name = substr($0, 15); next }
        /^[ \t]*ssid=/   { h = $0; sub(/^[ \t]*ssid=/, "", h); print h " " name; name = "" }'
}

do_forget() {
    hex=$(to_hex "$1")
    if [ -z "$(saved_blocks "$hex")" ]; then err "not saved: $1"; return 2; fi
    empty="$RUNCONF.empty"
    umask 077
    : > "$empty"
    save_network "$hex" "$empty"           # (replaces it with nothing = deletes it)
    rm -f "$empty"
    echo "forgotten: $1"
}

do_connect() {
    ssid=$1 pass=$2
    # --- check what we were given
    n=$(printf '%s' "$ssid" | wc -c)
    if [ "$n" -lt 1 ] || [ "$n" -gt 32 ]; then err "SSID must be 1..32 bytes (got $n)"; return 2; fi
    case "$pass" in *"$NL"*) err "password can't contain a line break"; return 2 ;; esac
    if [ -n "$pass" ] && ! is_hex64 "$pass"; then
        n=$(printf '%s' "$pass" | wc -c)
        if [ "$n" -lt 8 ] || [ "$n" -gt 63 ]; then err "WPA2 password must be 8..63 characters (got $n)"; return 2; fi
    fi
    check_ready || return $?

    # --- make the network block (secrets: files only readable by us)
    umask 077
    blk="$RUNCONF.blk"
    if ! network_block "$ssid" "$pass" > "$blk"; then
        rm -f "$blk"; err "could not make the key (bad password characters?)"; return 2
    fi
    # saved on the card, so the next time (and at every power-on) no password is needed
    save_network "$(to_hex "$ssid")" "$blk" || err "could not save to $SAVED (continuing)"

    # --- (re)start wpa_supplicant with just this network
    start_wpa "$blk"
}

case "$1" in
    connect)    [ $# -eq 3 ] || { err 'usage: wifi-join.sh connect "SSID" "PASSWORD"'; exit 2; }
                do_connect "$2" "$3" ;;
    status)     do_status ;;
    disconnect) do_disconnect ;;
    auto)       do_saved_join "" ;;
    use)        [ $# -eq 2 ] || { err 'usage: wifi-join.sh use "SSID"'; exit 2; }
                do_saved_join "$(to_hex "$2")" ;;
    saved)      do_list ;;
    forget)     [ $# -eq 2 ] || { err 'usage: wifi-join.sh forget "SSID"'; exit 2; }
                do_forget "$2" ;;
    *)          err "usage: wifi-join.sh connect \"SSID\" \"PASSWORD\" | status | disconnect | auto | use \"SSID\" | saved | forget \"SSID\""; exit 2 ;;
esac

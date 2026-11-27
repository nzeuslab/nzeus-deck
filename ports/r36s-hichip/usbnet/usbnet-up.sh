#!/bin/sh
# usbnet-up.sh - real TCP/IP over the USB cable for the MODERN Linux console,
# added NEXT TO the USB serial rescue shell, at run time, with a way back.
#
# How it works (plain words):
#  At boot, /usb-rescue made ONE USB gadget, "nzeus_rescue": a serial port (COM9
#  on the PC) with a root shell. THIS SCRIPT NEVER CHANGES THAT GADGET.
#  It builds a SECOND gadget = the same kind of serial port + a network card, and
#  swaps which gadget is plugged into the USB controller:
#        nzeus_rescue (serial)  ->  nzeus_ncm or nzeus_rndis (serial + network)
#  The PC sees the console unplug and come back as a new device: a new COM port
#  (same root shell) and a network card. The console gets 192.168.77.1/24.
#
#  SAFETY - automatic way back: unless the NEW serial port is proven to work
#  within KEEP seconds (default 180) by sending usbnet-confirm.sh with
#  rescue-command.py, the script swaps back to the untouched nzeus_rescue gadget
#  by itself. A power cycle also always gives the plain rescue gadget back:
#  everything here lives in RAM; nothing is written to the SD card or flash.
#
# Usage, from the rescue shell (or sent as-is with rescue-command.py):
#   sh usbnet-up.sh [auto|ncm|rndis] [KEEP]   switch to serial + network
#                       auto (default) = ncm if this kernel has it, else rndis
#   sh usbnet-up.sh --confirm                 "the new serial port works": stay
#   sh usbnet-up.sh --status                  what is active now
#   sh usbnet-up.sh --down                    back to the plain rescue gadget
# rescue-command.py runs a copy of this file, so the real work is done by a copy
# in /tmp/nzeus-usbnet/ that keeps running after the USB serial port goes away.
#
# Written for the boot BusyBox: no '[' (use test), no rm/tr/dd/od/timeout/printf.
# The NZ_* variables exist only so the PC test can point the script at fake
# folders; on the console they are never set.

CFG=${NZ_CONFIGFS:-/sys/kernel/config}/usb_gadget
UDCDIR=${NZ_UDCDIR:-/sys/class/udc}
NETDIR=${NZ_NETDIR:-/sys/class/net}
DEVDIR=${NZ_DEVDIR:-/dev}
CONSOLE=${NZ_CONSOLE:-/dev/console}
STATE=${NZ_USBNET_STATE:-/tmp/nzeus-usbnet}
IP=${NZ_IP:-ip}
SELF=$STATE/usbnet-up.sh
RESCUE=$CFG/nzeus_rescue        # made by /usb-rescue at boot; never modified here
CONSOLE_ADDR=192.168.77.1/24
PC_ADDR=192.168.77.2

case "$(uname -r)" in
    *-nzeus-gma-native*) ;;
    *) echo 'REFUSED: usbnet is only for the NZEUS modern kernels (gma-native*).'; exit 1 ;;
esac

# say: to the log/terminal AND to the kernel console (UART log + screen), so
# progress is visible even while the USB serial port is away.
say() {
    echo "usbnet: $*"
    # 2>/dev/null FIRST: when the console is gone (pin L12 = Vol-, uart0 unbound)
    # the shell's own "cannot create /dev/console" must not reach the caller.
    echo "usbnet: $*" 2>/dev/null > "$CONSOLE"
}
# w FILE VALUE: write one gadget setting; stop if the kernel refuses it.
w() {
    echo "$2" > "$1" || { say "could not write $2 to $1"; return 1; }
}
bound_to() { cat "$1/UDC" 2>/dev/null; }
# The net gadget that is plugged in now (empty = none).
active_net() {
    local g
    for g in "$CFG/nzeus_ncm" "$CFG/nzeus_rndis"; do
        test -n "$(bound_to "$g")" && { echo "$g"; return 0; }
    done
    return 0
}
set_state() { echo "$1" > "$STATE/state"; }
# pid_alive PID: true only for a real, running process number. (Careful: in
# BusyBox ash "kill -0 ''" means process 0 = OUR OWN process group, and plain
# "kill ''" would signal it - so every pid is checked here first.)
pid_alive() {
    case "$1" in ''|0|*[!0-9]*) return 1 ;; esac
    kill -0 "$1" 2>/dev/null
}

# Build gadget $1 for mode $2 (ncm or rndis). Safe to call again: a gadget
# that is already complete is reused as it is (the kernel locks the settings
# of a function once it is linked). Returns 2 if the kernel lacks this mode.
build_gadget() {
    local g=$1 mode=$2 serial bcd dev_mac host_mac
    if test -L "$g/configs/c.1/acm.serial" && test -L "$g/configs/c.1/$mode.net"; then
        return 0
    fi
    mkdir -p "$g" || return 1
    mkdir -p "$g/functions/$mode.net" 2>/dev/null || {
        echo "this kernel has no USB $mode function"
        return 2
    }
    mkdir -p "$g/functions/acm.serial" || return 1
    if test "$mode" = ncm; then
        serial=NZEUS-RAM-NCM-01; bcd=0x0101; dev_mac=02:4e:5a:01:00:01; host_mac=02:4e:5a:01:00:02
    else
        serial=NZEUS-RAM-RNDIS-01; bcd=0x0102; dev_mac=02:4e:5a:02:00:01; host_mac=02:4e:5a:02:00:02
    fi
    # Same vendor/product as the rescue gadget (Linux Foundation 0104 =
    # "multifunction composite gadget"), but its OWN serial number and bcdDevice:
    # Windows then treats it as a separate device and never changes the drivers
    # it already chose for the plain rescue port (NZEUS-RAM-RESCUE-01).
    w "$g/idVendor" 0x1d6b || return 1
    w "$g/idProduct" 0x0104 || return 1
    w "$g/bcdUSB" 0x0200 || return 1
    w "$g/bcdDevice" "$bcd" || return 1
    # EF/02/01 = "several functions, each described by an IAD" (Windows composite).
    w "$g/bDeviceClass" 0xEF || return 1
    w "$g/bDeviceSubClass" 0x02 || return 1
    w "$g/bDeviceProtocol" 0x01 || return 1
    mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" || return 1
    w "$g/strings/0x409/serialnumber" "$serial" || return 1
    w "$g/strings/0x409/manufacturer" 'NZEUS LAB' || return 1
    w "$g/strings/0x409/product" "NZEUS Experimental Linux Rescue + Network ($mode)" || return 1
    w "$g/configs/c.1/strings/0x409/configuration" "USB rescue terminal + $mode network" || return 1
    w "$g/configs/c.1/MaxPower" 250 || return 1
    if test ! -L "$g/configs/c.1/$mode.net"; then
        # Fixed made-up addresses (02:... = locally made), so the PC sees the
        # same network card every time and keeps its 192.168.77.2 setting.
        w "$g/functions/$mode.net/dev_addr" "$dev_mac" || return 1
        w "$g/functions/$mode.net/host_addr" "$host_mac" || return 1
    fi
    if test "$mode" = rndis; then
        # "Microsoft OS descriptors": tell Windows to use its own RNDIS driver
        # (same values as the stock NZEUS Deck PC-connection mode).
        w "$g/os_desc/use" 1 || return 1
        w "$g/os_desc/b_vendor_code" 0xcd || return 1
        w "$g/os_desc/qw_sign" MSFT100 || return 1
        if test ! -L "$g/configs/c.1/rndis.net"; then
            w "$g/functions/rndis.net/os_desc/interface.rndis/compatible_id" RNDIS || return 1
            w "$g/functions/rndis.net/os_desc/interface.rndis/sub_compatible_id" 5162001 || return 1
        fi
        test -L "$g/os_desc/c.1" || ln -s "$g/configs/c.1" "$g/os_desc/c.1" || return 1
        # Windows expects RNDIS as the FIRST function; the serial port follows.
        test -L "$g/configs/c.1/rndis.net" || ln -s "$g/functions/rndis.net" "$g/configs/c.1/rndis.net" || return 1
        test -L "$g/configs/c.1/acm.serial" || ln -s "$g/functions/acm.serial" "$g/configs/c.1/acm.serial" || return 1
    else
        # NCM: serial port first (interfaces 0-1), network second (2-3).
        test -L "$g/configs/c.1/acm.serial" || ln -s "$g/functions/acm.serial" "$g/configs/c.1/acm.serial" || return 1
        test -L "$g/configs/c.1/ncm.net" || ln -s "$g/functions/ncm.net" "$g/configs/c.1/ncm.net" || return 1
    fi
    return 0
}

# A root shell on the net gadget's serial port, restarted like /usb-rescue does.
start_serial_shell() {
    local port tty n
    port=$(cat "$1/functions/acm.serial/port_num" 2>/dev/null)
    case "$port" in ''|*[!0-9]*) say "no serial port number for $1"; return 1 ;; esac
    tty=$DEVDIR/ttyGS$port
    n=0
    while test ! -e "$tty"; do
        n=$((n + 1))
        test "$n" -gt 10 && { say "$tty did not appear"; return 1; }
        sleep 1
    done
    (
        while true; do
            setsid -c sh -c '
                stty sane
                echo
                echo "NZEUS RAM rescue (USB serial + network mode)."
                uname -a
                echo "Back to the plain rescue port: sh /tmp/nzeus-usbnet/usbnet-up.sh --down"
                exec sh -i
            ' < "$tty" > "$tty" 2>&1
            sleep 1
        done
    ) > /dev/null 2>&1 &
    echo $! > "$STATE/shell.pid"
    say "root shell started on $tty"
}
stop_serial_shell() {
    local pid
    pid=$(cat "$STATE/shell.pid" 2>/dev/null)
    pid_alive "$pid" && kill "$pid" 2>/dev/null
    : > "$STATE/shell.pid"
}

# Plug the untouched rescue gadget back in (tries a few times).
back_to_rescue() {
    local udc g n
    udc=$(cat "$STATE/udc" 2>/dev/null)
    stop_serial_shell
    for g in "$CFG/nzeus_ncm" "$CFG/nzeus_rndis"; do
        test -n "$(bound_to "$g")" && echo "" > "$g/UDC"
    done
    sleep 1
    n=0
    while test -z "$(bound_to "$RESCUE")"; do
        n=$((n + 1))
        if test "$n" -gt 5; then
            say "FAILED to re-plug the rescue gadget on $udc. Power-cycle the console (RAM only, safe)."
            set_state failed
            return 1
        fi
        echo "$udc" > "$RESCUE/UDC" || sleep 1
    done
    set_state rescue
    say "back to the plain rescue gadget on $udc (PC: NZEUS-RAM-RESCUE-01)"
}

setup_ip() {
    local ifname n
    ifname=$(cat "$1/functions/$2.net/ifname" 2>/dev/null)
    n=0
    while test -z "$ifname" || test ! -e "$NETDIR/$ifname"; do
        n=$((n + 1))
        test "$n" -gt 10 && { say "network interface did not appear ($ifname)"; return 1; }
        sleep 1
        ifname=$(cat "$1/functions/$2.net/ifname" 2>/dev/null)
    done
    $IP link set lo up 2>/dev/null
    $IP addr flush dev "$ifname"
    $IP addr add "$CONSOLE_ADDR" dev "$ifname" || { say "could not set $CONSOLE_ADDR on $ifname"; return 1; }
    $IP link set "$ifname" up || return 1
    echo "$ifname" > "$STATE/ifname"
    say "network $ifname = $CONSOLE_ADDR (PC should use $PC_ADDR)"
}

# ---- the part that keeps running after the USB serial port goes away ----
worker() {
    mode=$1
    keep=$2
    run=$NZ_USBNET_RUN
    sleep 3                       # let rescue-command.py read our reply first
    udc=$(bound_to "$RESCUE")
    test -n "$udc" || { say 'rescue gadget is not plugged in: nothing changed'; set_state rescue; exit 1; }
    echo "$udc" > "$STATE/udc"
    g=
    if test "$mode" = auto || test "$mode" = ncm; then
        build_gadget "$CFG/nzeus_ncm" ncm
        result=$?
        if test "$result" = 0; then
            g=$CFG/nzeus_ncm; mode=ncm
        elif test "$result" = 2 && test "$mode" = auto; then
            say 'no NCM in this kernel: using RNDIS'
        else
            say 'could not build the NCM gadget: nothing changed'; set_state rescue; exit 1
        fi
    fi
    if test -z "$g"; then
        build_gadget "$CFG/nzeus_rndis" rndis || { say 'could not build the RNDIS gadget: nothing changed'; set_state rescue; exit 1; }
        g=$CFG/nzeus_rndis; mode=rndis
    fi
    echo "$mode" > "$STATE/mode"
    say "switching USB on $udc: rescue -> rescue + $mode network (automatic way back in $keep s)"
    set_state "switching-$run"
    echo "" > "$RESCUE/UDC" || { say 'could not unplug the rescue gadget: nothing changed'; set_state rescue; exit 1; }
    sleep 1
    if ! echo "$udc" > "$g/UDC"; then
        say "the $mode gadget was refused by $udc"
        back_to_rescue
        exit 1
    fi
    set_state "waiting-$run"
    start_serial_shell "$g"
    setup_ip "$g" "$mode"
    say "waiting up to $keep s for usbnet-confirm.sh on the NEW serial port"
    n=0
    while test "$n" -lt "$keep"; do
        # --down (or a newer run) took over: stop quietly.
        test "$(cat "$STATE/state" 2>/dev/null)" = "waiting-$run" || exit 0
        if test "$(cat "$STATE/confirmed" 2>/dev/null)" = "$run"; then
            set_state "net-$mode"
            say "CONFIRMED: serial + $mode network stay on (PC: NZEUS-RAM-$(echo "$mode" | sed 's/ncm/NCM/; s/rndis/RNDIS/')-01)"
            exit 0
        fi
        # once every 10 s: what the USB controller says (for the log)
        case "$n" in *0) echo "usb state at $n s: $(cat "$UDCDIR/$udc/state" 2>/dev/null)" ;; esac
        sleep 1
        n=$((n + 1))
    done
    say "no confirmation within $keep s"
    back_to_rescue
}

down_worker() {
    sleep 3
    set_state "down"
    back_to_rescue
}

# start "$@" in its own session, from our own copy, logging to $STATE/log
detach() {
    local run
    run="$$-$(cut -d' ' -f1 /proc/uptime)"
    echo "$run" > "$STATE/run"
    NZ_USBNET_RUN=$run setsid sh "$SELF" "$@" > "$STATE/log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/worker.pid"
    echo "$run"
}

status() {
    local g ifname
    echo "kernel: $(uname -r)"
    echo "rescue gadget plugged in: [$(bound_to "$RESCUE")]"
    for g in "$CFG/nzeus_ncm" "$CFG/nzeus_rndis"; do
        test -d "$g" && echo "${g##*/} plugged in: [$(bound_to "$g")]"
    done
    echo "state: $(cat "$STATE/state" 2>/dev/null)"
    ifname=$(cat "$STATE/ifname" 2>/dev/null)
    test -n "$ifname" && $IP addr show dev "$ifname" 2>&1
    test -f "$STATE/log" && { echo '--- last log lines'; tail -n 8 "$STATE/log"; }
    return 0
}

mkdir -p "$STATE" && chmod 700 "$STATE" || { echo "cannot use $STATE"; exit 1; }
case "$1" in
    --worker) worker "$2" "$3"; exit $? ;;
    --down-worker) down_worker; exit $? ;;
    --status) status; exit 0 ;;
    --confirm)
        run=$(cat "$STATE/run" 2>/dev/null)
        case "$(cat "$STATE/state" 2>/dev/null)" in
            "waiting-$run") echo "$run" > "$STATE/confirmed"; echo "USBNET_CONFIRMED run=$run"; exit 0 ;;
            net-*) echo "USBNET_ALREADY_CONFIRMED"; exit 0 ;;
            *) echo "USBNET_NOTHING_TO_CONFIRM state=$(cat "$STATE/state" 2>/dev/null)"; exit 1 ;;
        esac ;;
    --down)
        test -n "$(active_net)" || { echo 'USBNET_ALREADY_DOWN: the plain rescue gadget is active.'; exit 0; }
        test "$0" = "$SELF" || cp "$0" "$SELF" || exit 1
        run=$(detach --down-worker)
        echo "USBNET_DOWN_STARTED run=$run: in 3 s this serial port goes away; use the plain"
        echo "rescue port (NZEUS-RAM-RESCUE-01) again. Log: $STATE/log"
        exit 0 ;;
esac

mode=${1:-auto}
keep=${2:-180}
case "$mode" in auto|ncm|rndis) ;; *) echo "unknown mode '$mode' (auto, ncm or rndis)"; exit 1 ;; esac
case "$keep" in ''|*[!0-9]*) echo "KEEP must be a number of seconds"; exit 1 ;; esac
if test "$keep" -lt 30 || test "$keep" -gt 900; then
    echo 'KEEP must be 30..900 seconds'; exit 1
fi
test -z "$(active_net)" || { echo 'REFUSED: network mode is already on (see --status).'; exit 1; }
test -n "$(bound_to "$RESCUE")" || { echo 'REFUSED: the rescue gadget (nzeus_rescue) is not plugged in.'; exit 1; }
case "$(cat "$STATE/state" 2>/dev/null)" in
    starting|switching-*|waiting-*)
        # only if that switch is really still running (not left over)
        if pid_alive "$(cat "$STATE/worker.pid" 2>/dev/null)"; then
            echo 'REFUSED: a switch is already in progress (see --status).'; exit 1
        fi ;;
esac
test "$0" = "$SELF" || cp "$0" "$SELF" || exit 1
: > "$STATE/confirmed"
set_state starting
run=$(detach --worker "$mode" "$keep")
echo "USBNET_STARTED run=$run mode=$mode keep=$keep"
echo "In 3 s this USB serial port goes away and the console comes back as"
echo "serial + network. Then, within $keep s, on the PC:"
echo "  python ports\\r36s-hichip\\diagnostics\\rescue-command.py ports\\r36s-hichip\\usbnet\\usbnet-confirm.sh"
echo "  powershell -ExecutionPolicy Bypass -File ports\\r36s-hichip\\usbnet\\pc-setup.ps1"
echo "Without the confirm step the console switches back by itself."
exit 0

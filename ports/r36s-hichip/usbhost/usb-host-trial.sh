#!/bin/sh
# usb-host-trial.sh [SECONDS] [--force-vbus] [--wifi] - try USB HOST mode on the console's
# USB-C data port for SECONDS (default 300), then switch back BY ITSELF to the
# normal USB device mode with the USB rescue serial port (NZEUS-RAM-RESCUE-01,
# COM9 on the PC) - unless usb-host-confirm.sh was run in time.
#
#   !!! UNPLUG THE PC CABLE FIRST. Host mode with a PC attached is NEVER done:
#   !!! nobody has measured yet what the console does with VBUS (the 5 V line)
#   !!! in host mode. This script waits until the USB controller reports
#   !!! "not attached" (no PC) and refuses to switch otherwise.
#
# For the gma-native-host kernel on the SD Debian root (ports/r36s-hichip/usbhost/README.md).
# How it works:
#   1. checks: NZEUS kernel, usb0 role switch present and in "device" role,
#      no other trial running, SECONDS 60..3600
#   2. a copy of this script keeps running in the background (it survives the
#      USB serial port going away) and logs to the kernel log (/dev/kmsg -> the
#      UART log via the Uno) and to /data/nzeus/usbhost.log
#   3. waits up to 60 s for the PC to be unplugged ("not attached")
#   4. stops the USB network mode if it is on (nzeus-usbnet / usbnet-up: its
#      serial shell, and its pending automatic way back, which must not touch
#      the port while it is a host), unplugs the gadgets, writes "host" to
#      /sys/class/usb_role/18844000.usb-role-switch/role (the kernel re-creates
#      the USB core as a host)
#   5. after SECONDS (or usb-host-off.sh): Wi-Fi down, "device" back to the
#      role switch, then the SAME state as after power-on: rescue gadget
#      plugged in (nzeus-usb-gadget, the same identity as at boot, COM9) and,
#      only if /etc/nzeus/usbnet-at-boot exists, nzeus-usbnet-boot in the
#      background (serial + network, confirmed by itself once the PC sends
#      packets, else back to COM9 after 180 s - exactly as at boot).
#   usb-host-confirm.sh within SECONDS = stay in host mode (until usb-host-off.sh
#   or power-off; every start is device mode again).
# --force-vbus: ask the PHY to report "VBUS valid" in host mode (UPHY20 bit 15;
#   only if the Phase 0 stock dump shows it set, or devices never connect).
# --wifi: once host mode is on, start wifi-up.sh by itself (it waits up to
#   120 s for the dongle) - the way to reach SSH when the only USB port holds
#   the dongle and no keyboard can be plugged in. Then confirm over SSH.
# Other ways back: power off (hold the power button 3 s); the next start is
# always device mode with the rescue port.
#
# Works with dash (Debian) and BusyBox ash; no '[', printf, rm, date or timeout.
# NZ_* variables: PC tests only.
ROLEDIR=${NZ_ROLEDIR:-/sys/class/usb_role}
UDCDIR=${NZ_UDCDIR:-/sys/class/udc}
CFG=${NZ_CONFIGFS:-/sys/kernel/config}/usb_gadget
USBDEV=${NZ_USBDEV:-/sys/bus/usb/devices}
KMSG=${NZ_KMSG:-/dev/kmsg}
LOG=${NZ_LOG:-/data/nzeus/usbhost.log}
STATE=${NZ_USBHOST_STATE:-/run/nzeus-usbhost}
GADGET=${NZ_GADGET_CMD:-/usr/local/sbin/nzeus-usb-gadget}
WIFIDOWN=${NZ_WIFI_DOWN:-/usr/local/lib/nzeus/usbhost/wifi-down.sh}
WIFIUP=${NZ_WIFI_UP:-/usr/local/lib/nzeus/usbhost/wifi-up.sh}
UNPLUG_WAIT=${NZ_UNPLUG_WAIT:-120}
USBREGS=${NZ_USBREGS:-/usr/local/sbin/usb-regs}   # read-only snapshot tool (missing = skipped)
PROCDIR=${NZ_PROC:-/proc}
DEVDIR=${NZ_DEVDIR:-/dev}
# Hardware 2026-10-10: this port CANNOT sense the PC cable being pulled (the udc
# stayed "configured"), so the owner confirms it by holding FN for 2 s; the
# watcher writes a flag file (read-only key check, ports/r36s-hichip/usbhost/fn-hold.py).
FN_WATCH=${NZ_FN_WATCH:-python3 /usr/local/lib/nzeus/usbhost/fn-hold.py}
# the USB network mode of the SD Debian (ports/r36s-hichip/usbnet, overlay nzeus-usbnet):
# its switch at boot, its state folder (same default as usbnet-up.sh) and its log
USBNET_FLAG=${NZ_USBNET_FLAG:-/etc/nzeus/usbnet-at-boot}
USBNET_BOOT=${NZ_USBNET_BOOT:-/usr/local/sbin/nzeus-usbnet-boot}
USBNET_STATE=${NZ_USBNET_STATE:-/tmp/nzeus-usbnet}
USBNET_BOOTLOG=${NZ_USBNET_BOOTLOG:-/run/nzeus-usbnet-boot.log}
SW=$ROLEDIR/18844000.usb-role-switch      # usb0 = the USB-C data port; never usb1
SELF=$STATE/usb-host-trial.sh

say() {
    local up
    up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    echo "usbhost: $*"
    echo "usbhost: $*" >> "$KMSG" 2>/dev/null
    if mkdir -p "${LOG%/*}" 2>/dev/null; then
        echo "[$up] $*" >> "$LOG" 2>/dev/null
    fi
}
set_state() { echo "$1" > "$STATE/state"; }
# step: say + sync, around the writes that touch the USB core. Hardware
# 2026-10-11: after a start WITHOUT the PC (udc "not attached") the console
# froze somewhere after "unplugging the rescue gadget" and the unsynced log
# lines were lost; each step is now on the card before the next write.
step() { say "step: $*"; sync; }

# net_gadget_remove DIR: take an UNBOUND configfs gadget apart in the order
# configfs needs (os_desc link, function links, config strings, configs,
# functions, strings, the gadget). Removing its function unregisters its
# network interface while the core it was registered under still exists.
# Needs rm or unlink for the configfs links (Debian has both).
# tty_users_stop PORT: end every process that still has /dev/ttyGS<PORT> open.
# Hardware 2026-10-11: after a start WITHOUT the PC the network gadget's serial
# port was never connected, so unplugging it never hangs the line up; the
# usbnet serial shell (sh -i) kept it open and removing the gadget's serial
# function waited for it FOREVER (u_serial gserial_free_port: wait_event until
# closed) - the trial hung at "removing nzeus_rndis". "test -ef" = the same
# file (no readlink in the boot BusyBox).
tty_users_stop() {
    local tty p f pids=""
    case "$1" in ''|*[!0-9]*) return 0 ;; esac
    tty=$DEVDIR/ttyGS$1
    test -e "$tty" || return 0
    for p in "$PROCDIR"/[0-9]*; do
        test "${p##*/}" = "$$" && continue
        for f in "$p"/fd/*; do
            if test "$f" -ef "$tty" 2>/dev/null; then pids="$pids ${p##*/}"; break; fi
        done
    done
    test -n "$pids" || return 0
    say "closing $tty first: still open by process$pids (a port the PC never connected is never hung up)"
    kill $pids 2>/dev/null
    sleep 1
    kill -9 $pids 2>/dev/null
    return 0
}
unlink_cfs() { rm "$1" 2>/dev/null || unlink "$1" 2>/dev/null; }
net_gadget_remove() {
    local g=$1 x c
    test -d "$g" || return 0
    test -z "$(cat "$g/UDC" 2>/dev/null)" || { say "${g##*/} still plugged in: not removed"; return 1; }
    for x in "$g"/os_desc/*; do test -L "$x" && unlink_cfs "$x"; done
    for c in "$g"/configs/*; do
        test -d "$c" || continue
        for x in "$c"/*; do test -L "$x" && unlink_cfs "$x"; done
        for x in "$c"/strings/*; do test -d "$x" && rmdir "$x"; done
        rmdir "$c"
    done
    for x in "$g"/functions/acm.*; do test -d "$x" && tty_users_stop "$(cat "$x/port_num" 2>/dev/null)"; done
    for x in "$g"/functions/*; do test -d "$x" && rmdir "$x"; done
    for x in "$g"/strings/*; do test -d "$x" && rmdir "$x"; done
    if rmdir "$g" 2>/dev/null; then
        say "${g##*/} removed (usbnet-up builds it fresh after host mode)"
    else
        say "${g##*/} could not be removed completely"
    fi
    return 0
}
get_state() { cat "$STATE/state" 2>/dev/null; }
role() { cat "$SW/role" 2>/dev/null; }
first_udc() {
    local u
    for u in "$UDCDIR"/*; do
        test -d "$u" && { echo "${u##*/}"; return 0; }
    done
    return 0
}
# pid_alive PID: true only for a real, running process number (in BusyBox ash
# "kill -0 ''" would mean our own process group).
pid_alive() {
    case "$1" in ''|0|*[!0-9]*) return 1 ;; esac
    kill -0 "$1" 2>/dev/null
}
device_count() {
    local d n=0
    for d in "$USBDEV"/*; do
        case "${d##*/}" in *:*|usb*) continue ;; esac
        test -r "$d/idVendor" && n=$((n + 1))
    done
    echo "$n"
}

# Back to device mode + rescue gadget (+ the USB network if it is on for boot).
# Tries hard; says clearly if it failed.
revert() {
    local n udc
    set_state reverting
    say "switching back to device mode"
    test -f "$WIFIDOWN" && sh "$WIFIDOWN" > /dev/null 2>&1
    sync
    n=0
    while test "$(role)" != device; do
        n=$((n + 1))
        if test "$n" -gt 3; then
            say "FAILED: the role switch stays '$(role)'. Way back: hold the power button 3 s (power off), start again."
            set_state failed
            return 1
        fi
        echo device > "$SW/role" 2>/dev/null || sleep 2
    done
    if ! NZ_UDC_WAIT=${NZ_UDC_WAIT:-30} sh "$GADGET"; then
        say "FAILED: device mode is back but the rescue gadget could not be plugged in. Way back: power off, start again."
        set_state failed
        return 1
    fi
    udc=$(cat "$CFG/nzeus_rescue/UDC" 2>/dev/null)
    if test -z "$udc"; then
        say "FAILED: rescue gadget not plugged in after nzeus-usb-gadget. Way back: power off, start again."
        set_state failed
        return 1
    fi
    set_state device
    say "BACK TO DEVICE MODE: rescue port NZEUS-RAM-RESCUE-01 on $udc (plug the PC in: COM9)"
    usbnet_restore
    return 0
}

# Host mode is coming: stop the USB network mode (ports/r36s-hichip/usbnet) so nothing
# of it touches the port while it is a host. Its waiting worker (state
# waiting-RUN) stops by itself when the state changes; nzeus-usbnet-boot stops
# waiting on "down" (the same value usbnet-up.sh --down uses). Its serial
# shell loop is stopped like usbnet-up.sh does it. Refuses (1) while
# usbnet-up is in the middle of a switch.
usbnet_release() {
    local st pid
    test -d "$USBNET_STATE" || return 0
    st=$(cat "$USBNET_STATE/state" 2>/dev/null)
    case "$st" in
        starting|switching-*)
            if pid_alive "$(cat "$USBNET_STATE/worker.pid" 2>/dev/null)"; then
                say "the USB network mode is switching right now (state '$st'): NOT switching"
                return 1
            fi ;;
    esac
    echo down > "$USBNET_STATE/state" 2>/dev/null
    pid=$(cat "$USBNET_STATE/shell.pid" 2>/dev/null)
    pid_alive "$pid" && kill "$pid" 2>/dev/null
    test -f "$USBNET_STATE/shell.pid" && : > "$USBNET_STATE/shell.pid"
    say "USB network mode stopped for host mode (was '$st')"
    return 0
}

# Back in device mode: the same as at power-on (etc/init.d/nzeus-usbnet). The
# USB network comes back only if it is switched on for boot; nzeus-usbnet-boot
# keeps usbnet-up's own way back (no PC traffic within 180 s = plain COM9 again).
usbnet_restore() {
    test -f "$USBNET_FLAG" || return 0
    if ! test -f "$USBNET_BOOT"; then
        say "USB network: $USBNET_BOOT missing - staying on the plain rescue port (COM9)"
        return 0
    fi
    setsid sh "$USBNET_BOOT" > "$USBNET_BOOTLOG" 2>&1 < /dev/null &
    say "USB network as at power-on: nzeus-usbnet-boot started (plug the PC in within 3 min: serial + network; else COM9 stays)"
    return 0
}

worker() {
    local secs=$1 run=$2 force=$3 wifi=$4 udc n st g fnflag why
    sleep 3                       # let the caller read our first reply
    udc=$(first_udc)
    say "run $run: armed for $secs s; waiting up to $UNPLUG_WAIT s for the PC cable to be unplugged AND FN held 2 s (udc $udc)"
    fnflag=$STATE/fn-$run
    $FN_WATCH "$fnflag" "$UNPLUG_WAIT" > /dev/null 2>&1 &
    n=0
    while true; do
        test "$(cat "$STATE/off" 2>/dev/null)" = "$run" && { say "cancelled before host mode: nothing changed"; set_state device; exit 0; }
        st=$(cat "$UDCDIR/$udc/state" 2>/dev/null)
        if test "$st" = "not attached"; then
            why="no PC attached (udc state '$st')"
            break
        fi
        if test -e "$fnflag"; then
            why="FN held 2 s: the owner says the PC cable is out (udc still says '$st': this port cannot sense an unplug)"
            break
        fi
        if test "$n" -ge "$UNPLUG_WAIT"; then
            say "no unplug seen and FN not held within $UNPLUG_WAIT s (state '$st'): NOT switching. Still device mode, nothing changed."
            set_state device
            exit 1
        fi
        sleep 1
        n=$((n + 1))
    done
    usbnet_release || { say "Still device mode, nothing changed: try again in a minute"; set_state device; exit 1; }
    say "$why: unplugging the rescue gadget"
    # read-only register snapshot (clock gate, reset, POWER, DEVCTL, UTMI, PHY) next to the log
    if test -x "$USBREGS"; then
        "$USBREGS" > "${LOG%/*}/usbregs-$run.txt" 2>&1
        step "USB registers saved: ${LOG%/*}/usbregs-$run.txt ($(grep -o 'suspend_phy=[0-9]' "${LOG%/*}/usbregs-$run.txt"), $(grep -o 'session=[0-9]' "${LOG%/*}/usbregs-$run.txt"))"
    fi
    for g in nzeus_rescue nzeus_ncm nzeus_rndis; do
        if test -n "$(cat "$CFG/$g/UDC" 2>/dev/null)"; then
            step "unplugging $g (udc state '$(cat "$UDCDIR/$udc/state" 2>/dev/null)')"
            echo "" > "$CFG/$g/UDC" || { say "could not unplug $g: NOT switching"; revert; exit 1; }
            step "$g unplugged"
        fi
    done
    # Hardware 2026-10-10/11: a network gadget (RNDIS/NCM) whose network interface
    # (usb0) was registered under the OLD core could never bind to the re-created
    # core ("kobject_add_internal failed for usb0 (error: -2 parent: net)").
    # Remove those gadgets while the old core still exists; usbnet-up builds them
    # fresh after the way back. The rescue gadget (serial only) is kept.
    for g in nzeus_ncm nzeus_rndis; do
        test -d "$CFG/$g" && step "removing $g"
        net_gadget_remove "$CFG/$g"
    done
    if test -e "$SW/device/nzeus_host_force_vbus"; then
        echo "$force" > "$SW/device/nzeus_host_force_vbus" 2>/dev/null
        say "force VBUS valid in host mode: $(cat "$SW/device/nzeus_host_force_vbus" 2>/dev/null)"
    fi
    set_state "host-$run"
    step "writing 'host' to the role switch (the kernel re-creates the USB core)"
    # a refused write fails right away; the kernel's own fall-back to
    # peripheral shows up as role "device" a moment later
    if ! echo host > "$SW/role" 2>/dev/null || ! sleep 1 || test "$(role)" != host; then
        say "the kernel refused host mode (role '$(role)'; see dmesg)"
        revert
        exit 1
    fi
    say "HOST MODE ON (run $run). Automatic way back in $secs s unless usb-host-confirm.sh runs."
    if test "$wifi" = 1; then
        say "starting wifi-up.sh (waits up to 120 s for the dongle; log /data/nzeus/wifi.log)"
        sh "$WIFIUP" --wait 120 > "$STATE/wifi-up.log" 2>&1 < /dev/null &
    fi
    n=0
    while test "$n" -lt "$secs"; do
        test "$(get_state)" = "host-$run" || exit 0
        if test "$(cat "$STATE/confirmed" 2>/dev/null)" = "$run"; then
            set_state host-confirmed
            say "CONFIRMED: host mode stays on (usb-host-off.sh or power-off to leave it)"
            exit 0
        fi
        if test "$(cat "$STATE/off" 2>/dev/null)" = "$run"; then
            say "usb-host-off.sh asked to leave host mode"
            break
        fi
        case "$n" in *0) test $((n % 30)) = 0 && say "host mode $n/$secs s: $(device_count) USB device(s)" ;; esac
        sleep 1
        n=$((n + 1))
    done
    test "$n" -ge "$secs" && say "no confirmation within $secs s"
    revert
}

revert_worker() {
    sleep 3
    revert
}

detach() {
    local run
    run="$$-$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)"
    echo "$run" > "$STATE/run"
    setsid sh "$SELF" "$@" "$run" > "$STATE/worker.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/worker.pid"
    echo "$run"
}

status() {
    echo "role switch: ${SW##*/} role=$(role) force_vbus=$(cat "$SW/device/nzeus_host_force_vbus" 2>/dev/null)"
    echo "trial state: $(get_state)"
    echo "rescue gadget plugged into: [$(cat "$CFG/nzeus_rescue/UDC" 2>/dev/null)]"
    test -f "$STATE/worker.log" && { echo '--- last worker lines'; tail -n 6 "$STATE/worker.log"; }
    return 0
}

mkdir -p "$STATE" && chmod 700 "$STATE" || { echo "cannot use $STATE"; exit 1; }
case "$1" in
    --worker) worker "$2" "$5" "$3" "$4"; exit $? ;;
    --revert-worker) revert_worker; exit $? ;;
    --status) status; exit 0 ;;
    --confirm)
        run=$(cat "$STATE/run" 2>/dev/null)
        case "$(get_state)" in
            "host-$run") echo "$run" > "$STATE/confirmed"; echo "USBHOST_CONFIRMED run=$run"; exit 0 ;;
            host-confirmed) echo "USBHOST_ALREADY_CONFIRMED"; exit 0 ;;
            *) echo "USBHOST_NOTHING_TO_CONFIRM state=$(get_state)"; exit 1 ;;
        esac ;;
    --off)
        run=$(cat "$STATE/run" 2>/dev/null)
        case "$(get_state)" in
            "host-$run"|starting)
                echo "$run" > "$STATE/off"
                echo "USBHOST_OFF_REQUESTED run=$run: the trial switches back within a few seconds"
                exit 0 ;;
            host-confirmed|failed)
                test -f "$SELF" || { echo "USBHOST_NO_COPY: $SELF missing"; exit 1; }
                setsid sh "$SELF" --revert-worker > "$STATE/worker.log" 2>&1 < /dev/null &
                echo "$!" > "$STATE/worker.pid"
                echo "USBHOST_OFF_STARTED: device mode + rescue port in a few seconds (log: $LOG)"
                exit 0 ;;
            *)
                if test "$(role)" = host; then
                    test -f "$SELF" || cp "$0" "$SELF" || exit 1
                    setsid sh "$SELF" --revert-worker > "$STATE/worker.log" 2>&1 < /dev/null &
                    echo "USBHOST_OFF_STARTED (host mode without a trial)"
                    exit 0
                fi
                echo "USBHOST_ALREADY_DEVICE: role=$(role)"; exit 0 ;;
        esac ;;
esac

secs=300
force=0
wifi=0
for a in "$@"; do
    case "$a" in
        --force-vbus) force=1 ;;
        --wifi) wifi=1 ;;
        *[!0-9]*|'') echo "usage: usb-host-trial.sh [SECONDS] [--force-vbus] [--wifi]"; exit 1 ;;
        *) secs=$a ;;
    esac
done
if test "$secs" -lt "${NZ_TRIAL_MIN:-60}" || test "$secs" -gt 3600; then
    echo 'SECONDS must be 60..3600'; exit 1
fi
case "$(uname -r)" in
    *-nzeus-*) ;;
    *) echo 'REFUSED: only for the NZEUS modern kernels (gma-native-host).'; exit 1 ;;
esac
test -e "$SW/role" || { echo "REFUSED: no USB role switch ${SW##*/} (needs the gma-native-host kernel)."; exit 1; }
test "$(role)" = device || { echo "REFUSED: the USB port is not in device mode (role '$(role)'); see usb-host-status.sh"; exit 1; }
case "$(get_state)" in
    starting|host-*|reverting)
        if pid_alive "$(cat "$STATE/worker.pid" 2>/dev/null)"; then
            echo 'REFUSED: a trial is already running (usb-host-trial.sh --status).'; exit 1
        fi ;;
esac
test "$0" = "$SELF" || cp "$0" "$SELF" || exit 1
: > "$STATE/confirmed"
: > "$STATE/off"
set_state starting
echo "=================================================================="
echo " USB HOST MODE TRIAL ($secs s, automatic way back)"
echo " 1. UNPLUG THE PC CABLE NOW, then HOLD FN FOR 2 s (within $UNPLUG_WAIT s):"
echo "    this port cannot sense the unplug, FN is your 'the PC is out'."
echo "    Never use host mode with a PC attached: VBUS behaviour is not known yet."
echo " 2. Then plug in the USB-C OTG adapter with ONE device (stick first)."
echo " 3. Look at it: usb-host-status.sh (keyboard on the console, or SSH"
echo "    over Wi-Fi later). Keep it: usb-host-confirm.sh within $secs s."
echo " 4. Otherwise it switches back by itself: unplug the adapter, plug"
echo "    the PC in, COM9 is back. Stuck? Hold power 3 s, start again."
echo "=================================================================="
run=$(detach --worker "$secs" "$force" "$wifi")
echo "USBHOST_TRIAL_STARTED run=$run seconds=$secs force_vbus=$force wifi=$wifi"
echo "Log: $LOG and the kernel log (UART). This serial port goes away when you unplug."
exit 0

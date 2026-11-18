#!/bin/sh
# usb-phase0.sh [LABEL] - Phase 0 of linux-port/usbhost: READ-ONLY look at the
# USB controllers. Changes nothing: no register write, no sysfs write, no
# gadget change, no file outside /tmp. Safe on the stock kernel (ADB) and on
# every modern NZEUS kernel (USB rescue shell, rescue-command.py).
#
# LABEL says what is plugged in right now, so several runs can be compared:
#   pc        the normal USB cable to the PC (modern rescue shell)
#   nothing   nothing in the data port
#   adapter   only the USB-C OTG adapter
#   dongle    OTG adapter + Wi-Fi dongle (stock: the dongle working)
#   stick     OTG adapter + USB stick
# Needs the reader program uploaded first, as /tmp/nzeus-tools/usb-regs:
#   modern: python linux-port\diagnostics\rescue-upload.py linux-port\usbhost\usb-regs-mipsel --name usb-regs
#   stock:  adb push linux-port/usbhost/usb-regs-mipsel /tmp/nzeus-tools/usb-regs
# Written for the boot BusyBox: no '[' (test only), no printf/rm/tr/od/date.
# NZ_* variables: PC tests only.
REGS=${NZ_USB_REGS:-/tmp/nzeus-tools/usb-regs}
SYS=${NZ_SYS:-/sys}
PROC=${NZ_PROC:-/proc}
LABEL=${1:-unlabelled}
case "$LABEL" in
    pc|nothing|adapter|dongle|stick|unlabelled) ;;
    *) echo "unknown LABEL '$LABEL' (pc, nothing, adapter, dongle, stick)"; exit 1 ;;
esac

echo "USB_PHASE0_BEGIN label=$LABEL"
echo "kernel: $(uname -r)"
echo "uptime: $(cut -d' ' -f1 "$PROC/uptime") s"
if test -x "$REGS"; then
    "$REGS" || echo "usb-regs exit $?"
    if test "${NZ_USB1:-0}" = 1; then "$REGS" --usb1 || echo "usb-regs --usb1 exit $?"; fi
else
    echo "USB_REGS_MISSING: upload usb-regs-mipsel as $REGS (see the top of this script)"
fi

echo "-- usb role switches (modern host kernel only)"
for sw in "$SYS"/class/usb_role/*; do
    test -e "$sw/role" || continue
    echo "${sw##*/}: role=$(cat "$sw/role" 2>/dev/null) force_vbus=$(cat "$sw/device/nzeus_host_force_vbus" 2>/dev/null)"
done
echo "-- device controllers (gadget side)"
for u in "$SYS"/class/udc/*; do
    test -d "$u" || continue
    echo "${u##*/}: state=$(cat "$u/state" 2>/dev/null) speed=$(cat "$u/current_speed" 2>/dev/null)"
done
echo "-- USB devices (host side; the root hubs are 1d6b:0002)"
n=0
for d in "$SYS"/bus/usb/devices/*; do
    case "${d##*/}" in *:*) continue ;; esac
    test -r "$d/idVendor" || continue
    n=$((n + 1))
    echo "${d##*/}: $(cat "$d/idVendor"):$(cat "$d/idProduct") speed=$(cat "$d/speed" 2>/dev/null) power=$(cat "$d/bMaxPower" 2>/dev/null) '$(cat "$d/product" 2>/dev/null)'"
done
test "$n" = 0 && echo "none (no USB host bus in this kernel/mode)"
echo "-- interrupts (stock: per-IRQ counts are real; modern: always 0, only ERR counts)"
grep -E 'musb|usb|ERR' "$PROC/interrupts"
echo "-- charger/battery (modern power kernels)"
for p in "$SYS"/class/power_supply/*; do
    test -d "$p" || continue
    echo "${p##*/}: online=$(cat "$p/online" 2>/dev/null) capacity=$(cat "$p/capacity" 2>/dev/null) status=$(cat "$p/status" 2>/dev/null)"
done
echo "-- recent kernel lines about USB"
dmesg | grep -i -E 'musb|usb|hc16xx|role' | tail -n 15
echo "USB_PHASE0_END label=$LABEL"

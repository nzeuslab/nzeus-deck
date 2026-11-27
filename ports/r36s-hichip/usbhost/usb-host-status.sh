#!/bin/sh
# usb-host-status.sh - what is on the USB-C data port, like "lsusb -t", read
# from /sys only (no usbutils needed). Changes nothing.
#   role switch, trial state, rescue gadget, every USB device with its
#   interfaces and which driver took each one, Wi-Fi/Bluetooth/disk names.
# Works with dash and BusyBox ash. NZ_* variables: PC tests only.
ROLEDIR=${NZ_ROLEDIR:-/sys/class/usb_role}
UDCDIR=${NZ_UDCDIR:-/sys/class/udc}
CFG=${NZ_CONFIGFS:-/sys/kernel/config}/usb_gadget
USBDEV=${NZ_USBDEV:-/sys/bus/usb/devices}
NET=${NZ_NETDIR:-/sys/class/net}
BT=${NZ_BTDIR:-/sys/class/bluetooth}
BLOCK=${NZ_BLOCKDIR:-/sys/class/block}
STATE=${NZ_USBHOST_STATE:-/run/nzeus-usbhost}
SW=$ROLEDIR/18844000.usb-role-switch

val() { cat "$1" 2>/dev/null; }
# a few well-known IDs, so nobody needs usb.ids
known() {
    case "$1" in
        0bda:1a2b) echo 'Realtek RTL8821CU dongle in DRIVER-CD mode (run nz-cdeject)' ;;
        0bda:c820) echo 'Realtek RTL8821CU Wi-Fi + Bluetooth (LB-Link BL-WN650BT)' ;;
        1d6b:0002) echo 'Linux root hub (USB 2.0)' ;;
        *) echo '' ;;
    esac
}
class_name() {
    case "$1" in
        03) echo HID ;; 08) echo storage ;; 09) echo hub ;; e0) echo wireless/BT ;;
        ff) echo vendor ;; 02|0a) echo CDC ;; 01) echo audio ;; 0e) echo video ;; *) echo "class $1" ;;
    esac
}

echo "USB-C data port (usb0):"
if test -e "$SW/role"; then
    echo "  role: $(val "$SW/role")   (role switch ${SW##*/}; force VBUS valid in host: $(val "$SW/device/nzeus_host_force_vbus"))"
else
    echo "  role: no role switch (not the gma-native-host kernel): device mode only"
fi
echo "  trial: $(val "$STATE/state")"
for u in "$UDCDIR"/*; do
    test -d "$u" && echo "  device side: ${u##*/} state=$(val "$u/state")"
done
echo "  rescue gadget plugged into: [$(val "$CFG/nzeus_rescue/UDC")]"
echo "USB devices:"
n=0
for d in "$USBDEV"/*; do
    name=${d##*/}
    case "$name" in *:*) continue ;; esac
    test -r "$d/idVendor" || continue
    n=$((n + 1))
    id="$(val "$d/idVendor"):$(val "$d/idProduct")"
    k=$(known "$id")
    echo "  Bus $(val "$d/busnum") Device $(val "$d/devnum") ID $id ${k:-$(val "$d/manufacturer") $(val "$d/product")} [$name, $(val "$d/speed") Mbit/s, $(val "$d/bMaxPower")]"
    for i in "$d"/"$name":*; do
        test -r "$i/bInterfaceClass" || continue
        drv=
        test -e "$i/driver" && drv=$(ls -l "$i/driver" 2>/dev/null | sed 's|.*/||')
        echo "    interface ${i##*:} $(class_name "$(val "$i/bInterfaceClass")") driver=${drv:-none}"
    done
done
test "$n" = 0 && echo "  none"
echo "Network interfaces from USB:"
m=0
for i in "$NET"/*; do
    test -e "$i/phy80211" || test -d "$i/wireless" || continue
    m=1
    echo "  ${i##*/} (Wi-Fi) state=$(val "$i/operstate") mac=$(val "$i/address")"
done
test "$m" = 0 && echo "  no Wi-Fi interface"
for h in "$BT"/hci*; do
    test -e "$h" && echo "  ${h##*/} (Bluetooth)"
done
for b in "$BLOCK"/sd*; do
    test -e "$b/size" && echo "  /dev/${b##*/} disk $(($(val "$b/size") / 2048)) MiB"
done
exit 0

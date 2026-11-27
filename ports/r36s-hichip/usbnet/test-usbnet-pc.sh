#!/bin/bash
# PC test of usbnet-up.sh / usbnet-confirm.sh / usbnet-down.sh with the console's
# EXACT boot BusyBox (taken from the gma-native-usbfix initramfs) under qemu-mipsel.
# Only the boot applets are on PATH (each one runs the MIPS BusyBox), so a missing
# applet ('[', rm, tr, timeout...) fails here just like on the console.
# The kernel's configfs/sysfs are replaced by plain folders (NZ_* variables).
# This proves the script logic only - NOT the real USB controller, Windows, or
# that the kernel accepts the gadget. Needs: python3, qemu-mipsel. ~2 minutes.
#   bash ports/r36s-hichip/usbnet/test-usbnet-pc.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
QEMU=$(command -v qemu-mipsel)
T=$(mktemp -d /tmp/nzeus-usbnet-test.XXXXXX)
BB=$T/busybox-boot
python3 - "$HERE/../display/bootlog/build-usbfix/initramfs.cpio.gz" "$BB" <<'EOF'
import gzip, sys
d = gzip.decompress(open(sys.argv[1], 'rb').read()); pos = 0
while True:
    f = [int(d[pos+6+i*8:pos+14+i*8], 16) for i in range(13)]; pos += 110
    name = d[pos:pos+f[11]-1].decode(); pos = (pos + f[11] + 3) & ~3
    data = d[pos:pos+f[6]]; pos = (pos + f[6] + 3) & ~3
    if name == 'bin/busybox':
        open(sys.argv[2], 'wb').write(data); break
    assert name != 'TRAILER!!!', 'no bin/busybox in the initramfs'
EOF
chmod 755 "$BB"
mkdir -p "$T/bin"
# the boot applets, exactly (see ports/r36s-hichip/display/STATUS.md)
for a in $(qemu-mipsel "$BB" --list); do
    printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$QEMU" "$BB" "$a" > "$T/bin/$a"
done
chmod 755 "$T/bin/"*
printf '#!/bin/sh\necho "$*" >> %s/ip.log\n' "$T" > "$T/bin/fakeip"; chmod 755 "$T/bin/fakeip"
test -e "$T/bin/[" && echo "NOTE: this BusyBox has '['"
export QEMU_UNAME=7.2.0-rc5-nzeus-gma-native-net+
S=$T/scripts; mkdir -p "$S"; cp "$HERE"/usbnet-*.sh "$S/"
F=$T/fake
pass=0; fail=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
fresh() {   # a fake console: rescue gadget plugged into musb-hdrc.0
    rm -rf "$F"; G=$F/config/usb_gadget
    mkdir -p "$G/nzeus_rescue" "$F/dev" "$F/net/usb0" "$F/udc/musb-hdrc.0"
    echo musb-hdrc.0 > "$G/nzeus_rescue/UDC"
    echo configured > "$F/udc/musb-hdrc.0/state"
    for m in ncm rndis; do   # what the kernel would create inside each new folder
        mkdir -p "$G/nzeus_$m/functions/acm.serial" "$G/nzeus_$m/functions/$m.net" "$G/nzeus_$m/os_desc"
        touch "$G/nzeus_$m/UDC"
        echo usb0 > "$G/nzeus_$m/functions/$m.net/ifname"
    done
    mkdir -p "$G/nzeus_rndis/functions/rndis.net/os_desc/interface.rndis"
    echo 1 > "$G/nzeus_ncm/functions/acm.serial/port_num"; touch "$F/dev/ttyGS1"
    echo 2 > "$G/nzeus_rndis/functions/acm.serial/port_num"; touch "$F/dev/ttyGS2"
    : > "$T/ip.log"
}
con() {   # run a console script with the boot BusyBox, fake folders, only boot applets
    env -i PATH="$T/bin" QEMU_UNAME="$QEMU_UNAME" NZ_CONFIGFS="$F/config" NZ_UDCDIR="$F/udc" \
        NZ_NETDIR="$F/net" NZ_DEVDIR="$F/dev" NZ_CONSOLE="$F/console" NZ_USBNET_STATE="$F/state" \
        NZ_IP=fakeip "$QEMU" "$BB" sh "$@" 2>&1
}
G=$F/config/usb_gadget
ST=$F/state

echo "== syntax (boot BusyBox sh -n)"
for s in usbnet-up usbnet-confirm usbnet-down; do
    ok "sh -n $s.sh" "qemu-mipsel '$BB' sh -n '$S/$s.sh'"
done

echo "== refusals"
fresh
out=$(QEMU_UNAME=4.4.186 con "$S/usbnet-up.sh"); ok "stock kernel refused" "echo \"\$out\" | grep -q REFUSED"
out=$(con "$S/usbnet-up.sh" wifi); ok "bad mode refused" "echo \"\$out\" | grep -q 'unknown mode'"
out=$(con "$S/usbnet-up.sh" ncm 5); ok "KEEP < 30 refused" "echo \"\$out\" | grep -q '30..900'"
out=$(con "$S/usbnet-up.sh" ncm 1x); ok "KEEP not a number refused" "echo \"\$out\" | grep -q 'number'"
: > "$G/nzeus_rescue/UDC"
out=$(con "$S/usbnet-up.sh"); ok "rescue gadget not plugged in: refused" "echo \"\$out\" | grep -q 'not plugged in'"
out=$(con "$S/usbnet-confirm.sh"); ok "confirm before start: refused" "echo \"\$out\" | grep -q USBNET_NOT_STARTED"

echo "== NCM, confirmed, then down"
fresh
out=$(con "$S/usbnet-up.sh" ncm 60); echo "$out" | head -3
ok "started" "echo \"\$out\" | grep -q 'USBNET_STARTED.*mode=ncm keep=60'"
ok "worker copy made" "cmp -s '$S/usbnet-up.sh' '$ST/usbnet-up.sh'"
out=$(con "$S/usbnet-up.sh" ncm 60); ok "second start while switching: refused" "echo \"\$out\" | grep -q REFUSED"
sleep 9
ok "rescue unplugged" "test -z \"\$(cat $G/nzeus_rescue/UDC)\""
ok "ncm gadget plugged into musb-hdrc.0" "test \"\$(cat $G/nzeus_ncm/UDC)\" = musb-hdrc.0"
ok "ncm identity" "test \"\$(cat $G/nzeus_ncm/strings/0x409/serialnumber)\" = NZEUS-RAM-NCM-01 && test \"\$(cat $G/nzeus_ncm/bcdDevice)\" = 0x0101 && test \"\$(cat $G/nzeus_ncm/idProduct)\" = 0x0104"
ok "composite class EF/02/01" "test \"\$(cat $G/nzeus_ncm/bDeviceClass)\$(cat $G/nzeus_ncm/bDeviceSubClass)\$(cat $G/nzeus_ncm/bDeviceProtocol)\" = 0xEF0x020x01"
ok "acm + ncm linked" "test -L $G/nzeus_ncm/configs/c.1/acm.serial && test -L $G/nzeus_ncm/configs/c.1/ncm.net"
ok "fixed MACs" "test \"\$(cat $G/nzeus_ncm/functions/ncm.net/host_addr)\" = 02:4e:5a:01:00:02"
ok "rescue gadget settings untouched" "test \"\$(ls $G/nzeus_rescue)\" = UDC"
ok "IP set on usb0" "grep -q 'addr add 192.168.77.1/24 dev usb0' '$T/ip.log' && grep -q 'link set usb0 up' '$T/ip.log'"
ok "state waiting" "grep -q '^waiting-' '$ST/state'"
ok "shell loop running" "kill -0 \$(cat $ST/shell.pid) 2>/dev/null"
out=$(con "$S/usbnet-confirm.sh"); echo "$out" | head -2
ok "confirm accepted" "echo \"\$out\" | grep -q USBNET_CONFIRMED"
sleep 3
ok "state net-ncm" "test \"\$(cat $ST/state)\" = net-ncm"
ok "console log says CONFIRMED" "grep -q CONFIRMED '$F/console'"
out=$(con "$S/usbnet-up.sh" ncm 60); ok "start while net is on: refused" "echo \"\$out\" | grep -q 'already on'"
out=$(con "$S/usbnet-down.sh"); echo "$out" | head -1
ok "down started" "echo \"\$out\" | grep -q USBNET_DOWN_STARTED"
sleep 7
ok "rescue plugged back in" "test \"\$(cat $G/nzeus_rescue/UDC)\" = musb-hdrc.0"
ok "ncm unplugged" "test -z \"\$(cat $G/nzeus_ncm/UDC)\""
ok "state rescue" "test \"\$(cat $ST/state)\" = rescue"
ok "shell loop stopped" "test -z \"\$(cat $ST/shell.pid)\""
out=$(con "$S/usbnet-down.sh"); ok "down again: nothing to do" "echo \"\$out\" | grep -q USBNET_ALREADY_DOWN"

echo "== auto on a kernel WITHOUT NCM -> RNDIS; nobody confirms -> automatic way back"
fresh
rm -rf "$G/nzeus_ncm/functions/ncm.net"; touch "$G/nzeus_ncm/functions/ncm.net"   # mkdir fails like ENOENT
out=$(con "$S/usbnet-up.sh" auto 30); echo "$out" | head -1
sleep 9
echo '--- worker log so far:'; cat "$ST/log"
ok "rndis chosen" "grep -q 'no NCM in this kernel: using RNDIS' '$ST/log' && test \"\$(cat $G/nzeus_rndis/UDC)\" = musb-hdrc.0"
ok "rndis identity" "test \"\$(cat $G/nzeus_rndis/strings/0x409/serialnumber)\" = NZEUS-RAM-RNDIS-01 && test \"\$(cat $G/nzeus_rndis/bcdDevice)\" = 0x0102"
ok "rndis OS descriptors" "test \"\$(cat $G/nzeus_rndis/functions/rndis.net/os_desc/interface.rndis/compatible_id)\" = RNDIS && test -L $G/nzeus_rndis/os_desc/c.1"
ok "ncm gadget never plugged in" "test -z \"\$(cat $G/nzeus_ncm/UDC)\""
sleep 30
ok "reverted after 30 s" "test \"\$(cat $G/nzeus_rescue/UDC)\" = musb-hdrc.0 && test -z \"\$(cat $G/nzeus_rndis/UDC)\""
ok "log says why" "grep -q 'no confirmation within 30 s' '$ST/log'"
ok "state rescue" "test \"\$(cat $ST/state)\" = rescue"
out=$(con "$S/usbnet-confirm.sh"); ok "late confirm refused" "echo \"\$out\" | grep -q USBNET_NOTHING_TO_CONFIRM"

echo "== kernel refuses the new gadget -> straight back"
fresh
rm -f "$G/nzeus_ncm/UDC"; mkdir "$G/nzeus_ncm/UDC"   # writing UDC fails
out=$(con "$S/usbnet-up.sh" ncm 60)
sleep 8
ok "back to rescue at once" "test \"\$(cat $G/nzeus_rescue/UDC)\" = musb-hdrc.0 && grep -q 'refused by musb-hdrc.0' '$ST/log' && test \"\$(cat $ST/state)\" = rescue"

echo "== a second up re-uses the built gadget"
fresh
out=$(con "$S/usbnet-up.sh" ncm 60); sleep 8; con "$S/usbnet-confirm.sh" > /dev/null; sleep 2
con "$S/usbnet-down.sh" > /dev/null; sleep 6
out=$(con "$S/usbnet-up.sh" ncm 60); sleep 8
ok "second switch works" "test \"\$(cat $G/nzeus_ncm/UDC)\" = musb-hdrc.0 && grep -q '^waiting-' '$ST/state'"
con "$S/usbnet-down.sh" > /dev/null; sleep 6
ok "and back" "test \"\$(cat $G/nzeus_rescue/UDC)\" = musb-hdrc.0"

echo "== a left-over 'starting' state with an empty pid file does not block"
# (an empty pid must never reach 'kill': in ash, kill -0 '' tests our own group)
echo starting > "$ST/state"; : > "$ST/worker.pid"
out=$(con "$S/usbnet-up.sh" ncm 60)
ok "not refused" "echo \"\$out\" | grep -q USBNET_STARTED"
sleep 8; con "$S/usbnet-down.sh" > /dev/null; sleep 6
ok "and back again" "test \"\$(cat $G/nzeus_rescue/UDC)\" = musb-hdrc.0"
pkill -f "$T/busybox-boot" 2>/dev/null
echo "RESULT: $pass passed, $fail failed"
test -n "${KEEP_T:-}" || rm -rf "$T"
test "$fail" = 0

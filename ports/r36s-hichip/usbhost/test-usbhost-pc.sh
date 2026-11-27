#!/bin/bash
# test-usbhost-pc.sh [FULL_BUSYBOX] - PC test of ports/r36s-hichip/usbhost WITHOUT the
# console, a card or a USB controller:
#   1. syntax of every script: dash, the console's boot BusyBox ash (qemu-mipsel)
#      and, if given, a fuller MIPS BusyBox; no '[' used as a command
#   2. usb-regs (MIPS, qemu) on a FAKE /dev/mem file: decoding, clock-gate and
#      reset guards, usb1 only on request; a build with a read-side-effect
#      register in its table refuses to run
#   3. nz-cdeject (MIPS under qemu + a PC test build) on FAKE sysfs and device
#      nodes: finds only 0bda:1a2b, refuses everything else, exact CBW bytes
#   4. usb-host-trial/confirm/off/status on FAKE sysfs/configfs trees with a
#      small "kernel simulator" (role host -> UDC gone, device -> UDC back),
#      with dash (Debian) and the boot BusyBox ash; the REAL nzeus-usb-gadget
#      from the Debian overlay re-plugs the rescue gadget; the USB network mode
#      (nzeus-usbnet) is stopped for host mode and comes back as at boot only
#      when /etc/nzeus/usbnet-at-boot exists (stand-in nzeus-usbnet-boot)
#   5. usb-phase0.sh, wifi-up/down, udhcpc.script with stand-ins
# Proves the PC side only - NOT the kernel, the USB controller, VBUS, the
# dongle or Wi-Fi (lab rule 7). Needs: python3, qemu-mipsel, dash, gcc,
# mipsel-linux-gnu-gcc. ~6-10 minutes. Writes only in a new folder under /tmp.
#   bash ports/r36s-hichip/usbhost/test-usbhost-pc.sh [FULL_BUSYBOX]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
QEMU=$(command -v qemu-mipsel || true)
test -n "$QEMU" || { echo "missing qemu-mipsel"; exit 2; }
command -v dash > /dev/null || { echo "missing dash"; exit 2; }
T=$(mktemp -d /tmp/nzeus-usbhost-test.XXXXXX)
pass=0; fail=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass + 1)); else echo "FAIL $1"; fail=$((fail + 1)); debug_dump; fi; }
# on a failure: what the fake console's logs say (if a fake console exists)
debug_dump() {
    test -n "${F:-}" && test -f "$F/kmsg" || return 0
    echo "  --- kmsg:"; sed 's/^/  | /' "$F/kmsg" | tail -n 12
    test -f "$F/data/nzeus/usbhost.log" && { echo "  --- usbhost.log:"; sed 's/^/  | /' "$F/data/nzeus/usbhost.log" | tail -n 12; }
    test -f "$F/run/usbhost/worker.log" && { echo "  --- worker.log:"; sed 's/^/  | /' "$F/run/usbhost/worker.log" | tail -n 12; }
    test -n "${out:-}" && { echo "  --- last output:"; echo "$out" | sed 's/^/  | /' | tail -n 8; }
    return 0
}

# ---- the console's boot BusyBox, from the gma-native-usbfix initramfs
BB=$T/busybox-boot
python3 - "$REPO/ports/r36s-hichip/display/bootlog/build-usbfix/initramfs.cpio.gz" "$BB" <<'EOF'
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
BBF=${1:-}
test -n "$BBF" && test -x "$BBF" || BBF=
BBBIN=$T/bbbin
mkdir -p "$BBBIN"
for a in $("$QEMU" "$BB" --list); do
    printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$QEMU" "$BB" "$a" > "$BBBIN/$a"
done
chmod 755 "$BBBIN"/*
ok "boot BusyBox has no '[' (scripts must use test)" "! test -e '$BBBIN/['"

SCRIPTS="usb-phase0.sh usb-host-trial.sh usb-host-confirm.sh usb-host-off.sh usb-host-status.sh wifi-up.sh wifi-down.sh udhcpc.script"
echo "== 1. syntax"
for s in $SCRIPTS; do
    ok "dash -n $s" "dash -n '$HERE/$s'"
    ok "boot BusyBox ash -n $s" "'$QEMU' '$BB' sh -n '$HERE/$s'"
    test -n "$BBF" && ok "full BusyBox ash -n $s" "'$QEMU' '$BBF' sh -n '$HERE/$s'"
    ok "$s: no '[' command" "! grep -v '^[[:space:]]*#' '$HERE/$s' | grep -E '(^|[;&|(]|then|do|if|while|!)[[:space:]]*\\[[[:space:]]' > /dev/null"
    ok "$s: no Windows line ends" "! grep -q \$(printf '\\r') '$HERE/$s'"
done
for s in test-usbhost-pc.sh; do ok "bash -n $s" "bash -n '$HERE/$s'"; done

echo "== 2. usb-regs on a fake /dev/mem"
MEM=$T/mem
python3 - "$MEM" <<'EOF'
import sys
f = open(sys.argv[1], 'wb'); f.truncate(0x18860000)
def put(addr, data):
    f.seek(addr); f.write(bytes(data))
put(0x18800064, (0).to_bytes(4, 'little'))                 # GATE1: all clocks on
put(0x1880007c, (1 << 12).to_bytes(4, 'little'))           # usb0 clock select bit
put(0x18800080, (0).to_bytes(4, 'little'))
put(0x18800084, (0).to_bytes(4, 'little'))
u = 0x18844000
put(u + 0x00, [0x05, 0x60])                                # FADDR 5, POWER softconn+hs_enable
put(u + 0x02, [0xee, 0xee, 0xee, 0xee])                    # INTRTX/INTRRX: must never be printed
put(u + 0x0a, [0xee])                                      # INTRUSB
put(u + 0x60, [0x99, 0x11])                                # DEVCTL b_device, vbus=3, session
put(u + 0x380, [0x94, 0x2a, 0x00, 0x00])                   # UTMI: peripheral, otg bits, disable_otg
put(0x18845000, list(range(0x24)))
put(0x18845020, (0x8000).to_bytes(4, 'little'))
put(0x18850060, [0x5d])                                    # usb1 DEVCTL
put(0x18845120, (0x1234).to_bytes(4, 'little'))
EOF
REGS=$HERE/usb-regs-mipsel
out=$(NZ_USB_MEM=$MEM "$QEMU" "$REGS"); rc=$?
ok "usb-regs: runs (rc 0) and ends with USB_REGS_DONE" "test $rc = 0 && echo \"\$out\" | grep -q '^USB_REGS_DONE$'"
ok "usb-regs: DEVCTL decoded (session 1, vbus 3, b_device 1)" "echo \"\$out\" | grep -q 'USB0.DEVCTL(+60)=99' && echo \"\$out\" | grep -q 'USB0.DEVCTL_DECODE session=1 host_req=0 host_mode=0 vbus_level=3 lsdev=0 fsdev=0 b_device=1'"
ok "usb-regs: UTMI 0x380/0x381 decoded" "echo \"\$out\" | grep -q 'USB0.UTMI(+380)=94 disable_phy=0 suspend_phy=0 otg_bits(3:2)=1 peripheral=1 disable_otg=1' && echo \"\$out\" | grep -q 'USB0.VBUS(+381)=2a'"
ok "usb-regs: PHY bytes and UPHY20 force bit" "echo \"\$out\" | grep -q 'PHY0.bytes(+00..+23)=00 01 02 03' && echo \"\$out\" | grep -q 'PHY0.UPHY20(32bit)=00008000 force_vbus_valid(bit15)=1'"
ok "usb-regs: never prints the clear-on-read INTRTX/INTRRX/INTRUSB" "! echo \"\$out\" | grep -q -E 'INTRTX\\(|INTRRX\\(|INTRUSB\\(|=ee'"
ok "usb-regs: usb1 not read without --usb1" "echo \"\$out\" | grep -q 'USB1 not read' && ! echo \"\$out\" | grep -q '^USB1\\.'"
out=$(NZ_USB_MEM=$MEM "$QEMU" "$REGS" --usb1)
ok "usb-regs --usb1: usb1 + PHY1 read when clocked" "echo \"\$out\" | grep -q 'USB1.DEVCTL(+60)=5d' && echo \"\$out\" | grep -q 'PHY1.UPHY20(32bit)=00001234'"
python3 - "$MEM" <<'EOF'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(0x18800064); f.write((3 << 24).to_bytes(4, 'little'))
EOF
out=$(NZ_USB_MEM=$MEM "$QEMU" "$REGS" --usb1)
ok "usb-regs: clock gated -> USB0/PHY0 and USB1/PHY1 NOT read" "echo \"\$out\" | grep -q 'USB0 SKIPPED' && echo \"\$out\" | grep -q 'USB1 SKIPPED' && ! echo \"\$out\" | grep -q -E '^(USB0|USB1|PHY0|PHY1)\\.'"
python3 - "$MEM" <<'EOF'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(0x18800064); f.write((0).to_bytes(4, 'little'))
f.seek(0x18800080); f.write((1 << 28).to_bytes(4, 'little'))
EOF
out=$(NZ_USB_MEM=$MEM "$QEMU" "$REGS")
ok "usb-regs: usb0 in reset -> not read" "echo \"\$out\" | grep -q 'USB0 SKIPPED' && ! echo \"\$out\" | grep -q '^USB0\\.'"
out=$("$QEMU" "$REGS" --bogus 2>&1); rc=$?
ok "usb-regs: bad argument refused (rc 2)" "test $rc = 2"
ok "usb-regs: opens /dev/mem O_RDONLY and maps PROT_READ only" "grep -q 'O_RDONLY | O_SYNC' '$HERE/usb-regs.c' && ! grep -q -E 'O_RDWR|O_WRONLY|PROT_WRITE' '$HERE/usb-regs.c'"
sed 's|{ "FADDR", 0x00, 1 },|{ "INTRUSB", 0x0a, 1 }, { "FADDR", 0x00, 1 },|' "$HERE/usb-regs.c" > "$T/usb-regs-bad.c"
mipsel-linux-gnu-gcc -Os -static -march=mips32r2 -mabi=32 -o "$T/usb-regs-bad" "$T/usb-regs-bad.c"
out=$(NZ_USB_MEM=$MEM "$QEMU" "$T/usb-regs-bad" 2>&1); rc=$?
ok "usb-regs: a table with INTRUSB (clear-on-read) refuses to run (rc 3)" "test $rc = 3 && echo \"\$out\" | grep -q 'read-side-effect'"
sed 's|{ "FADDR", 0x00, 1 },|{ "FIFO1", 0x24, 4 }, { "FADDR", 0x00, 1 },|' "$HERE/usb-regs.c" > "$T/usb-regs-bad2.c"
mipsel-linux-gnu-gcc -Os -static -march=mips32r2 -mabi=32 -o "$T/usb-regs-bad2" "$T/usb-regs-bad2.c"
out=$(NZ_USB_MEM=$MEM "$QEMU" "$T/usb-regs-bad2" 2>&1); rc=$?
ok "usb-regs: a table with a FIFO register refuses to run (rc 3)" "test $rc = 3"
mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32 -Wall -Wextra -Werror -o "$T/usb-regs-rebuilt" "$HERE/usb-regs.c"
ok "usb-regs-mipsel = a fresh build of usb-regs.c" "cmp -s '$T/usb-regs-rebuilt' '$REGS'"

echo "== 3. nz-cdeject on fake sysfs + device nodes"
mkdev() {   # DIR NAME VID PID BUS DEV
    mkdir -p "$1/$2"; echo "$3" > "$1/$2/idVendor"; echo "$4" > "$1/$2/idProduct"
    echo "$5" > "$1/$2/busnum"; echo "$6" > "$1/$2/devnum"
}
mkdesc() {   # FILE VID PID IFCLASS  (device + one config with one interface, bulk 0x81 IN / 0x02 OUT)
    python3 - "$@" <<'EOF'
import struct, sys
path, vid, pid, cls = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
dev = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200, 0, 0, 0, 64, vid, pid, 0x200, 1, 2, 3, 1)
ifc = bytes([9, 4, 0, 0, 2, cls, 6, 0x50, 0])
ep_in = bytes([7, 5, 0x81, 2, 0, 2, 0])
ep_out = bytes([7, 5, 0x02, 2, 0, 2, 0])
body = ifc + ep_in + ep_out
cfg = struct.pack('<BBHBBBBB', 9, 2, 9 + len(body), 1, 1, 0, 0x80, 250) + body
open(path, 'wb').write(dev + cfg)
EOF
}
E=$T/eject; SYSU=$E/sys; DEVB=$E/dev
ejrun() { env NZ_USBDEV="$SYSU" NZ_DEVBUS="$DEVB" "$@"; }
for EX in "$QEMU $HERE/nz-cdeject-mipsel" "$T/nz-cdeject-test"; do
    tag=$(test "${EX#$QEMU}" != "$EX" && echo mips || echo pc-test)
    if test "$tag" = pc-test; then
        gcc -Wall -Wextra -Werror -O2 -DNZ_TEST_BULK -o "$T/nz-cdeject-test" "$HERE/nz-cdeject.c" || echo "FAIL build pc test"
    fi
    rm -rf "$E"; mkdir -p "$SYSU" "$DEVB/001"
    mkdev "$SYSU" usb1 1d6b 0002 1 1; mkdev "$SYSU" 1-1 0bda c820 1 2; mkdir -p "$SYSU/1-1:1.0"
    out=$(ejrun $EX 2>&1); rc=$?
    ok "$tag eject: no CD-mode dongle (only c820 + root hub) -> rc 2, nothing opened" "test $rc = 2 && echo \"\$out\" | grep -q 'no CD-mode dongle'"
    mkdev "$SYSU" 1-1 0bda 1a2b 1 3; mkdesc "$DEVB/001/003" 0bda 1a2b 08; cp "$DEVB/001/003" "$E/desc.orig"
    out=$(ejrun $EX --check 2>&1); rc=$?
    ok "$tag eject --check: finds it, interface 0, IN 81 OUT 02, sends nothing" "test $rc = 0 && echo \"\$out\" | grep -q 'interface 0, bulk IN 81, bulk OUT 02' && echo \"\$out\" | grep -q CHECK_ONLY"
    if test "$tag" = mips; then
        out=$(ejrun $EX 2>&1); rc=$?
        ok "mips eject: a plain file is no usbdevfs node -> claim refused (rc 1), node unchanged" "test $rc = 1 && echo \"\$out\" | grep -q 'cannot claim' && cmp -s '$DEVB/001/003' '$E/desc.orig'"
    else
        rm -f "$E/bulk.out"
        out=$(ejrun env NZ_BULK_OUT="$E/bulk.out" $EX 2>&1); rc=$?
        ok "pc-test eject: rc 0, EJECT_SENT" "test $rc = 0 && echo \"\$out\" | grep -q EJECT_SENT"
        ok "pc-test eject: exactly two CBWs to OUT 02: ALLOW MEDIUM REMOVAL, then START STOP UNIT LoEj=1 Start=0" "python3 - '$E/bulk.out' <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert len(d) == 2 * 32, len(d)
for i, (tag, cdb) in enumerate(((0x4e5a0001, bytes([0x1e, 0, 0, 0, 0, 0])), (0x4e5a0002, bytes([0x1b, 0, 0, 0, 2, 0])))):
    ep, cbw = d[32 * i], d[32 * i + 1:32 * i + 32]
    assert ep == 0x02
    sig, t, length, flags, lun, cblen = struct.unpack('<IIIBBB', cbw[:15])
    assert sig == 0x43425355 and t == tag and length == 0 and flags == 0 and lun == 0 and cblen == 6
    assert cbw[15:21] == cdb and cbw[21:] == bytes(10)
EOF"
        rm -f "$E/bulk.out"
        out=$(ejrun env NZ_BULK_OUT="$E/bulk.out" NZ_BULK_GONE=1 $EX 2>&1); rc=$?
        ok "pc-test eject: dongle leaves before the last CSW -> still success" "test $rc = 0 && echo \"\$out\" | grep -q 'left CD mode already'"
    fi
    mkdesc "$DEVB/001/003" 0bda c820 08
    out=$(ejrun $EX --check 2>&1); rc=$?
    ok "$tag eject: device node is NOT 0bda:1a2b (sysfs says it is) -> REFUSED" "test $rc = 1 && echo \"\$out\" | grep -q 'REFUSED: the device node is 0bda:c820'"
    mkdesc "$DEVB/001/003" 0bda 1a2b ff
    out=$(ejrun $EX --check 2>&1); rc=$?
    ok "$tag eject: no mass-storage interface -> error" "test $rc = 1 && echo \"\$out\" | grep -q 'no bulk-only mass-storage interface'"
    mkdesc "$DEVB/001/003" 0bda 1a2b 08; mkdev "$SYSU" 1-2 0bda 1a2b 1 4
    out=$(ejrun $EX --check 2>&1); rc=$?
    ok "$tag eject: two CD-mode dongles -> REFUSED" "test $rc = 1 && echo \"\$out\" | grep -q 'more than one'"
    out=$(ejrun $EX --bogus 2>&1); rc=$?
    ok "$tag eject: bad argument -> rc 1" "test $rc = 1"
done
ok "nz-cdeject.c: test hooks only in the -DNZ_TEST_BULK build" "! mipsel-linux-gnu-strings '$HERE/nz-cdeject-mipsel' | grep -q NZ_BULK"
mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32 -Wall -Wextra -Werror -o "$T/nz-cdeject-rebuilt" "$HERE/nz-cdeject.c"
ok "nz-cdeject-mipsel = a fresh build of nz-cdeject.c" "cmp -s '$T/nz-cdeject-rebuilt' '$HERE/nz-cdeject-mipsel'"
ok "SHA256SUMS matches both programs" "(cd '$HERE' && sha256sum -c --quiet SHA256SUMS)"

# ------------------------------------------------------------ fake console for the trial
F=$T/fake
SH=$T/shims
mkdir -p "$SH"
mkshim() { printf '#!/bin/sh\n%s\n' "$2" > "$SH/$1"; chmod 755 "$SH/$1"; }
mkshim uname "case \"\$1\" in -r) cat $F/uname ;; *) echo \"Linux nzeus \$(cat $F/uname) mips\" ;; esac"
mkshim fake-wifi-down "echo called >> $F/wifidown.log"
mkshim fake-gadget-fail "exit 1"
mkshim fake-wifi-up "echo \"\$*\" >> $F/wifiup.log"
# stands in for the overlay's nzeus-usbnet-boot: notes whether the rescue gadget
# was already plugged in when it was started
# FN watchers (fn-hold.py stand-ins): never held / held - writes the flag file ($1) after 1 s
mkshim fake-fn-never "exit 1"
mkshim fake-fn-held "sleep 1; echo held > \"\$1\""
mkshim fake-usbnet-boot "echo \"called rescue=\$(cat $F/config/usb_gadget/nzeus_rescue/UDC 2>/dev/null)\" >> $F/usbnetboot.log; echo usbnet-boot-ran"
UDC=musb-hdrc.0.auto
fresh() {
    rm -rf "$F"; mkdir -p "$F/run" "$F/data" "$F/udc/$UDC" "$F/usb" "$F/net" "$F/bt" "$F/block"
    echo 7.2.0-rc5-nzeus-gma-native-host+ > "$F/uname"
    SWD=$F/role/18844000.usb-role-switch
    mkdir -p "$SWD/device"
    echo device > "$SWD/role"; echo 0 > "$SWD/device/nzeus_host_force_vbus"
    echo "not attached" > "$F/udc/$UDC/state"
    G=$F/config/usb_gadget/nzeus_rescue
    mkdir -p "$G/configs/c.1" "$G/functions/acm.usb0" "$F/config/usb_gadget/nzeus_ncm"
    ln -s "$G/functions/acm.usb0" "$G/configs/c.1/acm.usb0"
    echo "$UDC" > "$G/UDC"; : > "$F/config/usb_gadget/nzeus_ncm/UDC"
    mkdev "$F/usb" usb1 1d6b 0002 1 1
    : > "$F/kmsg"; : > "$F/wifidown.log"
}
# The "kernel": role host -> the UDC disappears and unplugs gadgets, a USB stick
# appears; role device -> the UDC comes back after 1 s. REFUSE_HOST=1: the
# kernel answers a host write by staying in device mode.
sim_start() {
    (
        while test -d "$F"; do
            r=$(cat "$F/role/18844000.usb-role-switch/role" 2>/dev/null)
            if test "$r" = host; then
                if test -e "$F/refuse_host"; then echo device > "$F/role/18844000.usb-role-switch/role"; fi
                if test -d "$F/udc/$UDC"; then
                    rm -rf "$F/udc/$UDC"
                    for g in "$F"/config/usb_gadget/*; do test -f "$g/UDC" && : > "$g/UDC"; done
                    mkdev "$F/usb" 1-1 0781 5567 1 2
                fi
            elif test "$r" = device && ! test -d "$F/udc/$UDC"; then
                rm -rf "$F/usb/1-1"
                sleep 1
                mkdir -p "$F/udc/$UDC"; echo "not attached" > "$F/udc/$UDC/state"
            fi
            sleep 0.2
        done
    ) &
    SIM=$!
}
sim_stop() { kill "$SIM" 2>/dev/null; wait "$SIM" 2>/dev/null; }
GADGET_SCRIPT=$REPO/ports/r36s-hichip/rootfs/debian/overlay/usr/local/sbin/nzeus-usb-gadget
con() {   # KIND SCRIPT ARGS: dash (Debian) or the boot BusyBox ash under qemu
    local kind=$1; shift
    local envs=(NZ_ROLEDIR="$F/role" NZ_UDCDIR="$F/udc" NZ_CONFIGFS="$F/config" NZ_USBDEV="$F/usb"
        NZ_KMSG="$F/kmsg" NZ_LOG="$F/data/nzeus/usbhost.log" NZ_USBHOST_STATE="$F/run/usbhost"
        NZ_GADGET_CMD="${GADGET:-$GADGET_SCRIPT}" NZ_WIFI_DOWN="$SH/fake-wifi-down" NZ_WIFI_UP="$SH/fake-wifi-up" NZ_UNPLUG_WAIT="${UNPLUG:-8}" NZ_FN_WATCH="${FNW:-$SH/fake-fn-never}"
        NZ_TRIAL_MIN=4 NZ_UDC_WAIT=8 NZ_USBHOST_LIB="$HERE" NZ_NETDIR="$F/net" NZ_BTDIR="$F/bt"
        NZ_BLOCKDIR="$F/block" NZ_USBNET_FLAG="$F/etc/usbnet-at-boot" NZ_USBNET_BOOT="$SH/fake-usbnet-boot"
        NZ_USBNET_STATE="$F/tmp/nzeus-usbnet" NZ_USBNET_BOOTLOG="$F/run/nzeus-usbnet-boot.log"
        NZ_DEVDIR="$F/dev" NZ_USBREGS=/nonexistent)
    if test "$kind" = dash; then
        env -i "${envs[@]}" PATH="$SH:/usr/sbin:/usr/bin:/sbin:/bin" dash "$@" 2>&1
    else
        env -i "${envs[@]}" PATH="$BBBIN" QEMU_UNAME="$(cat "$F/uname")" "$QEMU" "$BB" sh "$@" 2>&1
    fi
}
waitfor() {   # SECONDS CONDITION
    local n=0
    while ! eval "$2"; do n=$((n + 1)); test "$n" -gt $(($1 * 5)) && return 1; sleep 0.2; done
    return 0
}
ST=$F/run/usbhost
role_is() { test "$(cat "$F/role/18844000.usb-role-switch/role")" = "$1"; }
bound() { test "$(cat "$F/config/usb_gadget/nzeus_rescue/UDC" 2>/dev/null)" = "$UDC"; }

for K in ${NZ_TEST_KINDS:-dash ash}; do
    echo "== 4. trial with $K ($(test $K = dash && echo 'as Debian' || echo 'boot BusyBox ash under qemu'))"
    TRIAL=$HERE/usb-host-trial.sh
    # ---- refusals
    fresh
    echo 4.4.186 > "$F/uname"
    out=$(con $K "$TRIAL" 60); ok "$K trial: stock kernel refused" "echo \"\$out\" | grep -q REFUSED && role_is device"
    fresh
    out=$(con $K "$TRIAL" 3); ok "$K trial: SECONDS below the minimum refused" "echo \"\$out\" | grep -q 'SECONDS must be'"
    out=$(con $K "$TRIAL" 4000); ok "$K trial: SECONDS 4000 refused" "echo \"\$out\" | grep -q 'SECONDS must be'"
    out=$(con $K "$TRIAL" 5x); ok "$K trial: SECONDS not a number refused" "echo \"\$out\" | grep -q usage"
    rm -rf "$F/role"
    out=$(con $K "$TRIAL" 60); ok "$K trial: no role switch (older kernel) refused" "echo \"\$out\" | grep -q 'no USB role switch'"
    fresh; echo host > "$F/role/18844000.usb-role-switch/role"
    out=$(con $K "$TRIAL" 60); ok "$K trial: already host refused" "echo \"\$out\" | grep -q 'not in device mode'"
    out=$(con $K "$HERE/usb-host-confirm.sh"); ok "$K confirm before any trial: refused" "echo \"\$out\" | grep -q USBHOST_NOT_STARTED"

    # ---- PC never unplugged: nothing changes
    fresh; echo configured > "$F/udc/$UDC/state"; sim_start
    out=$(UNPLUG=3 con $K "$TRIAL" 6); echo "$out" | grep USBHOST_TRIAL_STARTED
    ok "$K trial: started, prints the UNPLUG warning" "echo \"\$out\" | grep -q 'UNPLUG THE PC CABLE NOW' && echo \"\$out\" | grep -q 'USBHOST_TRIAL_STARTED.*seconds=6 force_vbus=0'"
    out2=$(con $K "$TRIAL" 6); ok "$K trial: second start while one runs: refused" "echo \"\$out2\" | grep -q 'already running'"
    waitfor 15 "grep -q 'NOT switching' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: PC still attached -> NOT switching, still device, gadget still bound" "grep -q 'FN not held within' '$F/data/nzeus/usbhost.log' && role_is device && bound && test \"\$(cat $ST/state)\" = device"
    sim_stop

    # ---- the port cannot sense the unplug (hardware 2026-10-10): FN held 2 s -> host
    fresh; echo configured > "$F/udc/$UDC/state"; sim_start
    out=$(FNW=$SH/fake-fn-held con $K "$TRIAL" 6); echo "$out" | grep USBHOST_TRIAL_STARTED
    waitfor 20 "grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: udc still 'configured' but FN held -> host mode" "grep -q 'FN held 2 s' '$F/data/nzeus/usbhost.log' && grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    waitfor 25 "grep -q 'BACK TO DEVICE MODE' '$F/data/nzeus/usbhost.log'"
    ok "$K trial (FN path): back to device by itself" "grep -q 'BACK TO DEVICE MODE' '$F/data/nzeus/usbhost.log' && role_is device"
    sim_stop

    # ---- unplugged, host, nobody confirms -> back by itself
    fresh; echo configured > "$F/udc/$UDC/state"; echo "$UDC" > "$F/config/usb_gadget/nzeus_ncm/UDC"; : > "$F/config/usb_gadget/nzeus_rescue/UDC"; sim_start
    out=$(con $K "$TRIAL" 6); echo "$out" | grep USBHOST_TRIAL_STARTED
    sleep 5; echo "not attached" > "$F/udc/$UDC/state"
    waitfor 20 "grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: switched to host after the unplug" "role_is host && grep -q 'no PC attached' '$F/data/nzeus/usbhost.log' && grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: progress also goes to the kernel log (/dev/kmsg, UART)" "grep -q 'HOST MODE ON' '$F/kmsg'"
    ok "$K trial: USB network gadget was unplugged first" "test -z \"\$(cat $F/config/usb_gadget/nzeus_ncm/UDC)\""
    ok "$K trial: force VBUS knob set to 0 (default)" "test \"\$(cat $F/role/18844000.usb-role-switch/device/nzeus_host_force_vbus)\" = 0"
    ok "$K trial: state host-<run>" "grep -q '^host-' '$ST/state'"
    out=$(con $K "$HERE/usb-host-status.sh")
    ok "$K status during host: role host, the stick listed" "echo \"\$out\" | grep -q 'role: host' && echo \"\$out\" | grep -q 'ID 0781:5567'"
    # (the state is set just before the log line: wait for both - slow under qemu)
    waitfor 25 "test \"\$(cat $ST/state)\" = device && grep -q 'BACK TO DEVICE MODE' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: no confirmation -> back to device by itself" "grep -q 'no confirmation within 6 s' '$F/data/nzeus/usbhost.log' && role_is device"
    ok "$K trial: rescue gadget plugged in again (by the real nzeus-usb-gadget)" "bound && grep -q 'BACK TO DEVICE MODE' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: Wi-Fi was taken down before the switch back" "grep -q called '$F/wifidown.log'"
    ok "$K trial: log file on /data written" "grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log' && grep -q 'BACK TO DEVICE MODE' '$F/data/nzeus/usbhost.log'"
    ok "$K trial: USB network gadget NOT re-plugged (only the rescue lifeline)" "test -z \"\$(cat $F/config/usb_gadget/nzeus_ncm/UDC)\""
    ok "$K trial: no usbnet-at-boot file -> nzeus-usbnet-boot NOT started" "! test -e '$F/usbnetboot.log'"
    sim_stop

    # ---- USB network on at boot (Debian nzeus-usbnet): stopped for host mode,
    # back afterwards exactly as at power-on (rescue gadget first, then nzeus-usbnet-boot)
    fresh; U=$F/tmp/nzeus-usbnet; mkdir -p "$F/etc" "$U"; echo rndis > "$F/etc/usbnet-at-boot"
    echo "$UDC" > "$F/config/usb_gadget/nzeus_ncm/UDC"; : > "$F/config/usb_gadget/nzeus_rescue/UDC"
    echo waiting-1-2 > "$U/state"
    (sleep 300 > /dev/null 2>&1 & echo $! > "$U/shell.pid")   # stand-in for usbnet-up's serial shell loop
    SHP=$(cat "$U/shell.pid")
    # its serial port, never connected (start without the PC): a shell still holds it open
    mkdir -p "$F/config/usb_gadget/nzeus_ncm/functions/acm.usb0" "$F/dev"
    echo 1 > "$F/config/usb_gadget/nzeus_ncm/functions/acm.usb0/port_num"; : > "$F/dev/ttyGS1"
    (sleep 300 < "$F/dev/ttyGS1" > /dev/null 2>&1 & echo $! > "$F/ttyholder.pid")
    TTYP=$(cat "$F/ttyholder.pid")
    sim_start
    out=$(con $K "$TRIAL" 6); echo "$out" | grep USBHOST_TRIAL_STARTED
    waitfor 20 "grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    ok "$K usbnet: stopped before host mode (state down, its serial shell loop ended, gadget unplugged)" "role_is host && test \"\$(cat $U/state)\" = down && ! kill -0 $SHP 2>/dev/null && test -z \"\$(cat $F/config/usb_gadget/nzeus_ncm/UDC)\" && grep -q 'USB network mode stopped for host mode' '$F/data/nzeus/usbhost.log'"
    ok "$K usbnet: the process still holding its serial port was ended before the gadget was removed" "! kill -0 $TTYP 2>/dev/null && grep -q 'closing $F/dev/ttyGS1 first: still open by process $TTYP' '$F/data/nzeus/usbhost.log'"
    ok "$K usbnet: NOT restarted while the port is a host" "! test -e '$F/usbnetboot.log'"
    waitfor 25 "test -s '$F/usbnetboot.log' && test -s '$F/run/nzeus-usbnet-boot.log'"
    ok "$K usbnet: back in device mode -> rescue gadget plugged in first, then nzeus-usbnet-boot once" "role_is device && bound && test \"\$(cat $ST/state)\" = device && test \$(grep -c called '$F/usbnetboot.log') = 1 && grep -qx 'called rescue=$UDC' '$F/usbnetboot.log' && grep -q usbnet-boot-ran '$F/run/nzeus-usbnet-boot.log' && grep -q 'nzeus-usbnet-boot started' '$F/data/nzeus/usbhost.log'"
    kill "$SHP" 2>/dev/null
    sim_stop

    # ---- usbnet-up in the middle of a switch: no host mode now
    fresh; U=$F/tmp/nzeus-usbnet; mkdir -p "$U"; echo switching-9 > "$U/state"
    (sleep 300 > /dev/null 2>&1 & echo $! > "$U/worker.pid")
    sim_start
    out=$(con $K "$TRIAL" 6)
    waitfor 20 "test \"\$(cat $ST/state 2>/dev/null)\" = device && grep -q 'switching right now' '$F/data/nzeus/usbhost.log'"
    ok "$K usbnet switching: NOT switching to host, still device, rescue gadget bound, usbnet left alone" "grep -q 'switching right now' '$F/data/nzeus/usbhost.log' && role_is device && bound && test \"\$(cat $U/state)\" = switching-9"
    kill "$(cat "$U/worker.pid")" 2>/dev/null
    sim_stop

    # ---- confirmed: stays host past the timer; usb-host-off.sh brings it back
    fresh; sim_start
    out=$(con $K "$TRIAL" 6 --force-vbus --wifi); echo "$out" | grep USBHOST_TRIAL_STARTED
    waitfor 20 "grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    ok "$K trial --force-vbus: knob set to 1" "test \"\$(cat $F/role/18844000.usb-role-switch/device/nzeus_host_force_vbus)\" = 1"
    waitfor 10 "test -s '$F/wifiup.log'"
    ok "$K trial --wifi: wifi-up started after host mode, waiting 120 s for the dongle" "grep -qx -- '--wait 120' '$F/wifiup.log' && echo \"\$out\" | grep -q 'wifi=1'"
    out=$(con $K "$HERE/usb-host-confirm.sh"); echo "$out" | head -1
    ok "$K confirm accepted" "echo \"\$out\" | grep -q USBHOST_CONFIRMED"
    sleep 10
    ok "$K confirmed: still host after the timer" "role_is host && test \"\$(cat $ST/state)\" = host-confirmed && grep -q CONFIRMED '$F/data/nzeus/usbhost.log'"
    out=$(con $K "$HERE/usb-host-confirm.sh"); ok "$K confirm again: already confirmed" "echo \"\$out\" | grep -q USBHOST_ALREADY_CONFIRMED"
    out=$(con $K "$HERE/usb-host-off.sh"); echo "$out" | head -1
    ok "$K off after confirm: started" "echo \"\$out\" | grep -q USBHOST_OFF_STARTED"
    waitfor 20 "test \"\$(cat $ST/state)\" = device"
    ok "$K off: device + rescue gadget back" "role_is device && bound"
    out=$(con $K "$HERE/usb-host-off.sh"); ok "$K off again: already device" "echo \"\$out\" | grep -q USBHOST_ALREADY_DEVICE"
    sim_stop

    # ---- usb-host-off.sh during the trial
    fresh; sim_start
    out=$(con $K "$TRIAL" 300); waitfor 20 "grep -q 'HOST MODE ON' '$F/data/nzeus/usbhost.log'"
    out=$(con $K "$HERE/usb-host-off.sh")
    ok "$K off during the trial: requested" "echo \"\$out\" | grep -q USBHOST_OFF_REQUESTED"
    waitfor 20 "test \"\$(cat $ST/state)\" = device"
    ok "$K off during the trial: back long before 300 s" "role_is device && bound && grep -q 'usb-host-off.sh asked' '$F/data/nzeus/usbhost.log'"
    sim_stop

    # ---- cancel while still waiting for the unplug
    fresh; echo configured > "$F/udc/$UDC/state"; sim_start
    out=$(UNPLUG=30 con $K "$TRIAL" 60); sleep 5
    out=$(con $K "$HERE/usb-host-off.sh")
    waitfor 15 "grep -q 'cancelled before host mode' '$F/data/nzeus/usbhost.log'"
    ok "$K off before the unplug: cancelled, nothing changed" "grep -q 'cancelled before host mode' '$F/data/nzeus/usbhost.log' && role_is device && bound"
    sim_stop

    # ---- the kernel refuses host mode
    fresh; touch "$F/refuse_host"; sim_start
    out=$(con $K "$TRIAL" 60)
    waitfor 25 "test \"\$(cat $ST/state 2>/dev/null)\" = device && grep -q 'refused host mode' '$F/data/nzeus/usbhost.log'"
    ok "$K kernel refuses host: switched back, rescue gadget plugged in" "grep -q 'refused host mode' '$F/data/nzeus/usbhost.log' && role_is device && bound"
    sim_stop

    # ---- the way back fails: says so clearly (power off)
    fresh; sim_start
    out=$(GADGET=$SH/fake-gadget-fail con $K "$TRIAL" 4)
    waitfor 30 "test \"\$(cat $ST/state 2>/dev/null)\" = failed"
    ok "$K revert failure: state failed + 'power off' instruction in the log" "test \"\$(cat $ST/state)\" = failed && grep -q 'FAILED.*power off' '$F/data/nzeus/usbhost.log'"
    sim_stop
done

echo "== 5. status, phase 0, Wi-Fi"
for K in ${NZ_TEST_KINDS:-dash ash}; do
    fresh
    echo host > "$F/role/18844000.usb-role-switch/role"
    mkdev "$F/usb" 1-1 0bda c820 1 3
    for i in 0 1 2; do mkdir -p "$F/usb/1-1/1-1:1.$i"; echo ff > "$F/usb/1-1/1-1:1.$i/bInterfaceClass"; done
    echo e0 > "$F/usb/1-1/1-1:1.0/bInterfaceClass"
    mkdir -p "$T/drv/btusb" "$T/drv/rtw88_8821cu"
    ln -sfn "$T/drv/btusb" "$F/usb/1-1/1-1:1.0/driver"; ln -sfn "$T/drv/rtw88_8821cu" "$F/usb/1-1/1-1:1.2/driver"
    mkdev "$F/usb" 1-2 0bda 1a2b 1 4
    mkdir -p "$F/net/wlan0/wireless" "$F/net/lo" "$F/bt/hci0" "$F/block/sda"
    echo up > "$F/net/wlan0/operstate"; echo 2048000 > "$F/block/sda/size"
    out=$(con $K "$HERE/usb-host-status.sh"); rc=$?
    ok "$K status: rc 0, role host, root hub, dongle named, drivers per interface" "test $rc = 0 && echo \"\$out\" | grep -q 'role: host' && echo \"\$out\" | grep -q 'ID 1d6b:0002 Linux root hub' && echo \"\$out\" | grep -q 'ID 0bda:c820 Realtek RTL8821CU' && echo \"\$out\" | grep -q 'interface 1.0 wireless/BT driver=btusb' && echo \"\$out\" | grep -q 'interface 1.2 vendor driver=rtw88_8821cu'"
    ok "$K status: CD mode, wlan0, hci0, sda listed" "echo \"\$out\" | grep -q 'DRIVER-CD mode' && echo \"\$out\" | grep -q 'wlan0 (Wi-Fi) state=up' && echo \"\$out\" | grep -q 'hci0 (Bluetooth)' && echo \"\$out\" | grep -q '/dev/sda disk 1000 MiB'"
    # phase 0: read-only (fake usb-regs), LABEL check, nothing written
    printf '#!/bin/sh\necho USB_REGS_V1 read-only\necho USB_REGS_DONE\n' > "$T/fake-usb-regs"; chmod 755 "$T/fake-usb-regs"
    mkdir -p "$F/proc"; echo '53: 0 sys_irq 53 musb-hdrc.0.auto' > "$F/proc/interrupts"; echo '12.3 4.5' > "$F/proc/uptime"
    mkdir -p "$F/sys/class" "$F/sys/bus/usb"; ln -s "$F/role" "$F/sys/class/usb_role"; ln -s "$F/udc" "$F/sys/class/udc"; ln -s "$F/usb" "$F/sys/bus/usb/devices"
    before=$(find "$F" -type f -exec md5sum {} + | sort | md5sum)
    out=$(if test $K = dash; then env -i PATH="$SH:/usr/bin:/bin" NZ_USB_REGS="$T/fake-usb-regs" NZ_SYS="$F/sys" NZ_PROC="$F/proc" dash "$HERE/usb-phase0.sh" dongle; else env -i PATH="$BBBIN" NZ_USB_REGS="$T/fake-usb-regs" NZ_SYS="$F/sys" NZ_PROC="$F/proc" QEMU_UNAME=7.2.0-rc5-nzeus-gma-native-host+ "$QEMU" "$BB" sh "$HERE/usb-phase0.sh" dongle; fi 2>&1)
    after=$(find "$F" -type f -exec md5sum {} + | sort | md5sum)
    ok "$K phase0: runs the reader, lists role/udc/devices/interrupts" "echo \"\$out\" | grep -q 'USB_PHASE0_BEGIN label=dongle' && echo \"\$out\" | grep -q USB_REGS_DONE && echo \"\$out\" | grep -q '18844000.usb-role-switch: role=host' && echo \"\$out\" | grep -q '1-1: 0bda:c820' && echo \"\$out\" | grep -q musb-hdrc.0.auto && echo \"\$out\" | grep -q USB_PHASE0_END"
    ok "$K phase0: wrote nothing in the fake /sys" "test '$before' = '$after'"
    out=$(if test $K = dash; then env -i PATH="$SH:/usr/bin:/bin" dash "$HERE/usb-phase0.sh" wifi; else env -i PATH="$BBBIN" "$QEMU" "$BB" sh "$HERE/usb-phase0.sh" wifi; fi 2>&1)
    ok "$K phase0: unknown label refused" "echo \"\$out\" | grep -q 'unknown LABEL'"
    out=$(if test $K = dash; then env -i PATH="$SH:/usr/bin:/bin" NZ_USB_REGS=/nonexistent NZ_SYS="$F/sys" NZ_PROC="$F/proc" dash "$HERE/usb-phase0.sh" nothing; else env -i PATH="$BBBIN" NZ_USB_REGS=/nonexistent NZ_SYS="$F/sys" NZ_PROC="$F/proc" "$QEMU" "$BB" sh "$HERE/usb-phase0.sh" nothing; fi 2>&1)
    ok "$K phase0: reader missing -> says how to upload it" "echo \"\$out\" | grep -q USB_REGS_MISSING"
done

# ---- Wi-Fi (Debian: dash; plus a fuller BusyBox ash if given - the boot BusyBox has no od)
W=$T/wifi
mkshim fake-wpa "echo \"\$*\" > $W/wpa.args; for a; do case \"\$p\" in -P) echo \$\$ > \"\$a\" ;; esac; p=\$a; done; exit \$(cat $W/wpa_rc 2>/dev/null || echo 0)"
mkshim fake-wpacli "echo \"\$*\" >> $W/wpacli.args; echo bssid=00:11:22:33:44:55; echo wpa_state=\$(cat $W/wpa_state 2>/dev/null || echo COMPLETED)"
mkshim fake-udhcpc "echo \"\$*\" > $W/udhcpc.args; echo 'inet 192.168.1.50/24' > $W/addr"
mkshim fake-ip "echo \"\$*\" >> $W/ip.log; case \"\$*\" in *'addr show'*) cat $W/addr 2>/dev/null | sed 's/^/    /' ;; esac; exit 0"
mkshim fake-eject "echo ejected >> $W/eject.log; rm -rf $F/usb/1-2; mkdir -p $F/net/wlan0/wireless; exit 0"
mkshim fake-ssh "case \"\$1\" in status) test -e $W/ssh_running ;; start) echo 1 > $W/ssh_running; echo started ;; esac"
wfresh() {
    fresh; rm -rf "$W"; mkdir -p "$W" "$F/data/nzeus"
    echo host > "$F/role/18844000.usb-role-switch/role"
    mkdir -p "$F/net/wlan0/wireless"
    printf 'ssid=Caf\303\251 "Net" $HOME\npsk=pa ss"w$ord\\1\nsecurity=auto\n' > "$F/data/nzeus/wifi.conf"
    chmod 600 "$F/data/nzeus/wifi.conf"
}
wcon() {   # KIND SCRIPT ARGS
    local kind=$1; shift
    local envs=(NZ_WIFI_CONF="$F/data/nzeus/wifi.conf" NZ_WIFI_RUN="$F/run/wifi" NZ_NETDIR="$F/net" NZ_USBDEV="$F/usb"
        NZ_ROLEDIR="$F/role" NZ_KMSG="$F/kmsg" NZ_WIFI_LOG="$F/data/nzeus/wifi.log" NZ_USBHOST_LIB="$HERE"
        NZ_CDEJECT="$SH/fake-eject" NZ_WPA_SUPPLICANT="$SH/fake-wpa" NZ_WPA_CLI="$SH/fake-wpacli"
        NZ_UDHCPC="$SH/fake-udhcpc" NZ_IP="$SH/fake-ip" NZ_SSH_INIT="$SH/fake-ssh" NZ_WIFI_IF_WAIT=3 NZ_WIFI_ASSOC_WAIT=3)
    if test "$kind" = dash; then
        env -i "${envs[@]}" PATH="$SH:/usr/bin:/bin" dash "$@" 2>&1
    else
        env -i "${envs[@]}" PATH="$SH:$T/fullbin" "$QEMU" "$BBF" sh "$@" 2>&1
    fi
}
WKINDS=dash
if test -n "$BBF"; then
    WKINDS="dash ash"; mkdir -p "$T/fullbin"
    for a in $("$QEMU" "$BBF" --list); do printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$QEMU" "$BBF" "$a" > "$T/fullbin/$a"; done
    chmod 755 "$T/fullbin"/*
else
    echo "SKIP Wi-Fi with BusyBox ash (give a full MIPS BusyBox as the 1st argument; the boot one has no od)"
fi
for K in $WKINDS; do
    echo "== 5b. Wi-Fi with $K"
    wfresh; echo device > "$F/role/18844000.usb-role-switch/role"
    out=$(wcon $K "$HERE/wifi-up.sh"); ok "$K wifi: refused while the port is in device mode" "echo \"\$out\" | grep -q 'not in host mode'"
    out=$(wcon $K "$HERE/wifi-up.sh" --wait x); ok "$K wifi: --wait needs a number" "echo \"\$out\" | grep -q 'needs a number'"
    out=$(wcon $K "$HERE/wifi-up.sh" --bogus); ok "$K wifi: unknown option -> usage" "echo \"\$out\" | grep -q 'usage: wifi-up.sh'"
    wfresh; rm "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh"); ok "$K wifi: no wifi.conf -> says where it goes" "echo \"\$out\" | grep -q 'no .*wifi.conf'"
    wfresh; chmod 644 "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh"); ok "$K wifi: wifi.conf readable by others -> refused (chmod 600)" "echo \"\$out\" | grep -q 'chmod 600' && ! test -e '$W/wpa.args'"
    wfresh; printf 'ssid=x\npsk=short\n' > "$F/data/nzeus/wifi.conf"; chmod 600 "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh"); ok "$K wifi: password shorter than 8 refused" "echo \"\$out\" | grep -q '8..63'"
    wfresh; printf 'psk=12345678\n' > "$F/data/nzeus/wifi.conf"; chmod 600 "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh"); ok "$K wifi: no ssid refused" "echo \"\$out\" | grep -q 'no ssid'"
    wfresh; printf 'ssid=x\npsk=12345678\nsecurity=wep\n' > "$F/data/nzeus/wifi.conf"; chmod 600 "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh"); ok "$K wifi: unknown security refused" "echo \"\$out\" | grep -q 'security must be'"
    # the full run: dongle in CD mode first
    wfresh; rm -rf "$F/net/wlan0"; mkdev "$F/usb" 1-2 0bda 1a2b 1 4
    out=$(wcon $K "$HERE/wifi-up.sh"); rc=$?
    ok "$K wifi: CD-mode dongle ejected, interface found, WIFI_UP with the DHCP address" "test $rc = 0 && grep -q ejected '$W/eject.log' && echo \"\$out\" | grep -q 'WIFI_UP wlan0 192.168.1.50'"
    ok "$K wifi: wpa_supplicant with nl80211 on wlan0 and the RAM config" "grep -q -- '-B -D nl80211 -i wlan0 -c $F/run/wifi/wpa.conf' '$W/wpa.args'"
    C=$F/run/wifi/wpa.conf
    ok "$K wifi: config is root-only (600) in a 700 folder" "test \"\$(stat -c %a '$C')\" = 600 && test \"\$(stat -c %a '$F/run/wifi')\" = 700"
    ok "$K wifi: SSID written as hex (UTF-8, quotes, \$ survive)" "grep -qx '    ssid=436166c3a920224e6574222024484f4d45' '$C'"
    ok "$K wifi: password quoted exactly, auto = WPA2+WPA3, PMF optional" "grep -qxF '    psk=\"pa ss\"w\$ord\\1\"' '$C' && grep -qx '    key_mgmt=WPA-PSK WPA-PSK-SHA256 SAE' '$C' && grep -qx '    ieee80211w=1' '$C'"
    ok "$K wifi: password never in the output, kernel log or wifi.log" "! echo \"\$out\" | grep -q 'pa ss' && ! grep -q 'pa ss' '$F/kmsg' '$F/data/nzeus/wifi.log'"
    ok "$K wifi: udhcpc with our script, background" "grep -q -- '-i wlan0 -s $HERE/udhcpc.script' '$W/udhcpc.args' && grep -q -- ' -b' '$W/udhcpc.args'"
    ok "$K wifi: SSH started as the second lifeline" "test -e '$W/ssh_running' && echo \"\$out\" | grep -q 'SSH is running: ssh root@192.168.1.50'"
    wfresh; printf 'ssid=Home\npsk=%s\nsecurity=wpa2\n' 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef > "$F/data/nzeus/wifi.conf"; chmod 600 "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh" --no-dhcp)
    ok "$K wifi: 64-hex key unquoted, wpa2 only, --no-dhcp skips udhcpc" "grep -qx '    psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef' '$C' && grep -qx '    key_mgmt=WPA-PSK WPA-PSK-SHA256' '$C' && ! test -e '$W/udhcpc.args'"
    wfresh; printf 'ssid=Home\r\npsk=12345678\r\nsecurity=wpa3\r\n' > "$F/data/nzeus/wifi.conf"; chmod 600 "$F/data/nzeus/wifi.conf"
    out=$(wcon $K "$HERE/wifi-up.sh")
    ok "$K wifi: Windows line ends dropped, wpa3 = SAE + PMF required" "grep -qx '    ssid=486f6d65' '$C' && grep -qx '    psk=\"12345678\"' '$C' && grep -qx '    key_mgmt=SAE' '$C' && grep -qx '    ieee80211w=2' '$C'"
    wfresh; echo SCANNING > "$W/wpa_state"
    out=$(wcon $K "$HERE/wifi-up.sh"); rc=$?
    ok "$K wifi: not joined in time -> clear error, rc 1" "test $rc = 1 && echo \"\$out\" | grep -q 'not joined after'"
    # wifi-down
    sleep 300 & p1=$!; sleep 300 & p2=$!
    echo $p1 > "$F/run/wifi/wpa.pid"; echo $p2 > "$F/run/wifi/udhcpc.pid"
    out=$(wcon $K "$HERE/wifi-down.sh")
    sleep 0.5
    ok "$K wifi-down: both processes stopped, address flushed, config emptied" "! kill -0 $p1 2>/dev/null && ! kill -0 $p2 2>/dev/null && grep -q 'addr flush dev wlan0' '$W/ip.log' && ! test -s '$F/run/wifi/wpa.conf' && echo \"\$out\" | grep -q WIFI_DOWN"
    kill $p1 $p2 2>/dev/null
done
for K in dash ash; do
    rm -rf "$W"; mkdir -p "$W"
    if test $K = dash; then
        out=$(env -i PATH="$SH:/usr/bin:/bin" NZ_IP="$SH/fake-ip" NZ_RESOLV="$W/resolv.conf" NZ_KMSG="$W/kmsg" interface=wlan0 ip=192.168.1.50 mask=24 router=192.168.1.1 dns='1.1.1.1 9.9.9.9' dash "$HERE/udhcpc.script" bound 2>&1)
    else
        out=$(env -i PATH="$BBBIN" NZ_IP="$SH/fake-ip" NZ_RESOLV="$W/resolv.conf" NZ_KMSG="$W/kmsg" interface=wlan0 ip=192.168.1.50 mask=24 router=192.168.1.1 dns='1.1.1.1 9.9.9.9' "$QEMU" "$BB" sh "$HERE/udhcpc.script" bound 2>&1)
    fi
    ok "$K udhcpc.script bound: address, default route, two DNS servers" "grep -q 'addr add 192.168.1.50/24 dev wlan0' '$W/ip.log' && grep -q 'route add default via 192.168.1.1 dev wlan0' '$W/ip.log' && grep -qx 'nameserver 9.9.9.9' '$W/resolv.conf' && ! test -e '$W/resolv.conf.nzeus-new'"
done

echo
echo "RESULT: $pass passed, $fail failed (work folder $T)"
test "$fail" = 0 && echo USBHOST_PC_TESTS_PASSED
test "$fail" = 0

#!/bin/bash
# test-debian-pc.sh [WORK_DIR [BUSYBOX_FULL]] - PC test of the NZEUS SD-root pieces WITHOUT
# Debian, a card or the console:
#   1. syntax of every script: dash, bash, the console's boot BusyBox ash
#      (qemu-mipsel) and, if given, a fuller BusyBox (NZ_TEST_BUSYBOX_FULL)
#   2. the fixed files: fstab, inittab, init.d headers, packages, apt pins...
#   3. behaviour of the scripts on FAKE folders (configfs, sysfs, /proc) with
#      stand-ins for blockdev/e2label/e2fsck/mount/mkswap/swapon/uname, run
#      with dash (as in Debian) and with BusyBox ash (as in Buildroot)
#   4. the USB shell with the UNCHANGED rescue-command.py / rescue-upload.py
#      over a pseudo-terminal (tests/rescue_pty_test.py), bash and dash
#   5. apply-overlay.sh (debian + buildroot), refusals (CRLF, private key)
#   6. a FAKE root folder (BusyBox + overlay) through the SAME make-image.sh,
#      e2fsck, image checks, then nz_region.py selftest --rootfs-ext4 with it
#   7. build-debian.sh refuses a non-WSL PC and Windows folders
# Proves the PC side only - NOT Debian on the console, the kernel, the USB
# controller or the card (lab rule 7).
# Needs: root (mknod, mke2fs -d owners), python3, qemu-mipsel, dash, e2fsprogs,
# dosfstools + mtools (region self-test). ~3-5 minutes. Writes only in WORK_DIR
# (default: a new folder under /tmp) and, for the upload test, one file in
# /tmp/nzeus-tools that it deletes again.
#   sudo bash ports/r36s-hichip/rootfs/debian/test-debian-pc.sh [WORK_DIR]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
OVL=$HERE/overlay
BRO=$REPO/ports/r36s-hichip/rootfs/buildroot/board/nzeus-r36s
T=${1:-$(mktemp -d /tmp/nzeus-debian-test.XXXXXX)}
mkdir -p "$T"
T=$(cd "$T" && pwd)
test "$(id -u)" = 0 || { echo "run as root (sudo): mknod and mke2fs -d need it"; exit 2; }
QEMU=$(command -v qemu-mipsel || true)
test -n "$QEMU" || { echo "missing qemu-mipsel (sudo apt install qemu-user)"; exit 2; }
command -v dash > /dev/null || { echo "missing dash"; exit 2; }
pass=0; fail=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass + 1)); else echo "FAIL $1"; fail=$((fail + 1)); fi; }

# ---- the console's boot BusyBox, taken from the gma-native-usbfix initramfs
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
BBF=${2:-${NZ_TEST_BUSYBOX_FULL:-}}
test -n "$BBF" && test -x "$BBF" || BBF=
# applet links for BusyBox ash runs (each one runs the MIPS BusyBox under qemu)
BBBIN=$T/bbbin
mkdir -p "$BBBIN"
ASHBB=${BBF:-$BB}
for a in $("$QEMU" "$ASHBB" --list); do
    printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$QEMU" "$ASHBB" "$a" > "$BBBIN/$a"
done
chmod 755 "$BBBIN"/*

# every shell script we ship
SCRIPTS=("$OVL"/etc/init.d/* "$OVL"/usr/local/sbin/* "$OVL"/usr/local/bin/* "$OVL"/etc/profile.d/*
         "$HERE/apply-overlay.sh" "$BRO/post-build.sh" "$BRO"/overlay/etc/init.d/S*)

echo "== 1. syntax"
for s in "${SCRIPTS[@]}"; do
    n=${s#$REPO/}
    ok "dash -n $n" "dash -n '$s'"
    ok "boot BusyBox ash -n $n" "'$QEMU' '$BB' sh -n '$s'"
    test -n "$BBF" && ok "full BusyBox ash -n $n" "'$QEMU' '$BBF' sh -n '$s'"
done
for s in "$HERE/build-debian.sh" "$HERE/make-image.sh" "$HERE/test-debian-pc.sh"; do
    ok "bash -n ${s#$REPO/}" "bash -n '$s'"
done
for py in rescue_pty_test.py gadget_compare.py; do
    ok "python3 parses tests/$py" "python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' '$HERE/tests/$py'"
done
crlf=$(grep -rlI "$(printf '\r')" "$OVL" "$HERE"/*.sh "$HERE"/*.txt "$HERE"/*.conf 2>/dev/null)
ok "no Windows line ends in overlay/ and the scripts" "test -z '$crlf'"

echo "== 2. fixed files"
FS=$OVL/etc/fstab
rootline=$(grep -E '^/dev/mmcblk0p3[[:space:]]+/[[:space:]]' "$FS")
ok "fstab: / is mmcblk0p3 ext4" "echo '$rootline' | awk '{exit !(\$3 == \"ext4\")}'"
ok "fstab: / has ro (only nzeus-rootcheck makes it rw)" "echo '$rootline' | awk '{exit !(\",\" \$4 \",\" ~ /,ro,/)}'"
ok "fstab: / has noatime and errors=remount-ro" "echo '$rootline' | grep -q noatime && echo '$rootline' | grep -q errors=remount-ro"
ok "fstab: / pass 0 (Debian's checkroot never checks it)" "echo '$rootline' | awk '{exit !(\$6 == 0)}'"
ok "fstab: /data = mmcblk0p4 noauto,nofail,noatime pass 0" "grep -E '^/dev/mmcblk0p4[[:space:]]+/data[[:space:]]+ext4[[:space:]]+[^ ]*noauto' '$FS' | grep nofail | grep noatime | awk '{exit !(\$6 == 0)}'"
ok "fstab: stock FAT32 p1 at /mnt/stock, ro" "grep -E '^/dev/mmcblk0p1[[:space:]]+/mnt/stock[[:space:]]+vfat[[:space:]]+ro,' '$FS' > /dev/null"
ok "fstab: every p1 line is ro" "! grep -E '^/dev/mmcblk0p1' '$FS' | grep -v -E '[[:space:]](ro|ro,[^ ]*)[[:space:]]'"
ok "fstab: header blocks p2/p5 never mounted" "! grep -E '^/dev/mmcblk0p[25]' '$FS'"
ok "fstab: /tmp tmpfs size=8M" "grep -E '^tmpfs[[:space:]]+/tmp[[:space:]]+tmpfs[[:space:]]+[^ ]*size=8M' '$FS' > /dev/null"
IT=$OVL/etc/inittab
gs=$(grep -n '^GS0:' "$IT" | cut -d: -f1); l0=$(grep -n '^l0:' "$IT" | cut -d: -f1)
ok "inittab: USB shell on ttyGS0 respawns, before the runlevel lines" "test -n '$gs' && test '$gs' -lt '$l0' && grep -q '^GS0:S12345:respawn:/usr/local/sbin/nzeus-usb-shell$' '$IT'"
ok "inittab: no sulogin, no getty (UART is listen-only; RAM)" "! grep -v '^#' '$IT' | grep -E 'sulogin|getty'"
ok "inittab: default runlevel 2, sysinit rcS" "grep -q '^id:2:initdefault:' '$IT' && grep -q '^si::sysinit:/etc/init.d/rcS' '$IT'"
ok "halt: HALT=poweroff (poweroff reaches the kernel power-off)" "grep -q '^HALT=poweroff$' '$OVL/etc/default/halt'"
ok "e2fsck.conf: broken_system_clock (no clock chip)" "grep -q 'broken_system_clock = 1' '$OVL/etc/e2fsck.conf'"
ok "hostname nzeus" "test \"\$(cat '$OVL/etc/hostname')\" = nzeus"
ok "locale C.UTF-8" "grep -q '^LANG=C.UTF-8$' '$OVL/etc/default/locale'"
for s in "$OVL"/etc/init.d/*; do
    n=$(basename "$s")
    ok "init.d/$n: LSB header, Provides: $n, Default-Start" "grep -q '^### BEGIN INIT INFO' '$s' && grep -q '^### END INIT INFO' '$s' && grep -q '^# Provides: *$n\$' '$s' && grep -q '^# Default-Start: ' '$s'"
done
ok "init.d: nzeus-usb and nzeus-zram before checkroot" "grep -q 'X-Start-Before: *checkroot\$' '$OVL/etc/init.d/nzeus-usb' && grep -q 'X-Start-Before: *checkroot\$' '$OVL/etc/init.d/nzeus-zram'"
ok "init.d: nzeus-rootfs after checkroot, before checkroot-bootclean/checkfs/mountall" "grep -q 'Required-Start: *checkroot\$' '$OVL/etc/init.d/nzeus-rootfs' && grep -q 'X-Start-Before: *checkroot-bootclean checkfs mountall\$' '$OVL/etc/init.d/nzeus-rootfs'"
ok "clock: only /data/nzeus/clock.txt (never the boot-files partition)" "! grep -q /mnt/stock '$OVL/usr/local/sbin/nzeus-clock'"
ok "init.d: clock saved at power-off/restart (Default-Stop 0 6, before \$local_fs goes)" "grep -q 'Default-Stop: *0 6\$' '$OVL/etc/init.d/nzeus-clock' && grep -q 'Required-Stop: *\$local_fs' '$OVL/etc/init.d/nzeus-clock'"
PK=$(sed 's/#.*//' "$HERE/packages.txt" | xargs)
for p in sysvinit-core dropbear-bin nano less htop procps iproute2 iputils-ping e2fsprogs ca-certificates python3 wpasupplicant iw evtest fbset curl apt \
         firmware-realtek wireless-regdb usbutils; do
    ok "packages.txt has $p" "echo ' $PK ' | grep -q ' $p '"
done
for p in systemd systemd-sysv udev dbus kmod; do
    ok "packages.txt does NOT have $p" "! echo ' $PK ' | grep -q ' $p '"
done
ok "build-debian: apt sources use main + non-free-firmware (firmware-realtek), every line" "grep -q '^COMPONENTS=\"main non-free-firmware\"\$' '$HERE/build-debian.sh' && test \$(grep -c 'SUITE[a-z-]* \$COMPONENTS' '$HERE/build-debian.sh') = 5 && ! grep -E '^deb .* main\$' '$HERE/build-debian.sh'"
ok "apt pin -1 for systemd systemd-sysv udev dbus" "grep -q '^Package: systemd systemd-sysv udev dbus' '$OVL/etc/apt/preferences.d/90nzeus-no-systemd' && grep -q '^Pin-Priority: -1' '$OVL/etc/apt/preferences.d/90nzeus-no-systemd'"
ok "apt: no recommends" "grep -q 'Install-Recommends \"false\"' '$OVL/etc/apt/apt.conf.d/90nzeus'"
ok "mke2fs.conf: metadata_csum, no csum_seed/orphan_file" "grep -q 'metadata_csum,64bit' '$HERE/mke2fs.conf' && ! grep -E 'metadata_csum_seed|orphan_file' '$HERE/mke2fs.conf' | grep -v '^#' | grep -q features"
ok "motd is plain ASCII" "! LC_ALL=C grep -q '[^[:print:][:space:]]' '$OVL/etc/motd'"

# ------------------------------------------------------------ fake console
# (shims run with the BusyBox PATH too, so they use no other programs but cat)
F=$T/fake
SH=$T/shims
mkdir -p "$SH"
mkshim() { printf '#!/bin/sh\n%s\n' "$2" > "$SH/$1"; chmod 755 "$SH/$1"; }
mkshim uname "case \"\$1\" in -r) cat $F/uname ;; *) echo \"Linux nzeus \$(cat $F/uname) mips\" ;; esac"
mkshim blockdev "test \"\$1\" = --getro || exit 1; cat $F/ro_\${2##*/} 2>/dev/null || echo 0"
mkshim e2label "cat $F/label_\${1##*/} 2>/dev/null"
mkshim e2fsck "echo \"\$*\" >> $F/e2fsck.log; for a; do last=\$a; done; exit \$(cat $F/fsck_rc_\${last##*/} 2>/dev/null || echo 0)"
mkshim mount "echo \"\$*\" >> $F/mount.log; exit \$(cat $F/mount_rc 2>/dev/null || echo 0)"
mkshim mkswap "echo \"mkswap \$*\" >> $F/swap.log"
mkshim swapon "echo \"swapon \$*\" >> $F/swap.log"
mkshim mountpoint "test \"\$1\" = -q && shift; test -e $F/mounted_\${1##*/}"
mkshim rm "/bin/rm \"\$@\""
fresh() {
    rm -rf "$F"; mkdir -p "$F/run" "$F/dev"
    echo 7.2.0-rc5-nzeus-gma-native-sdroot+ > "$F/uname"
    mknod "$F/dev/mmcblk0p3" b 7 250; mknod "$F/dev/mmcblk0p4" b 7 251    # loop250/251: never real
    local q
    for q in 1 2 5; do echo 1 > "$F/ro_mmcblk0p$q"; done
    echo nzroot > "$F/label_mmcblk0p3"; echo nzdata > "$F/label_mmcblk0p4"
    cat > "$F/sdroot.conf" <<EOF
NZ_ROOT_DEV=$F/dev/mmcblk0p3
NZ_ROOT_LABEL=nzroot
NZ_DATA_DEV=$F/dev/mmcblk0p4
NZ_DATA_LABEL=nzdata
NZ_DATA_DIR=$F/data
NZ_NEVER_WRITE="$F/dev/mmcblk0p1 $F/dev/mmcblk0p2 $F/dev/mmcblk0p5"
EOF
    echo "$F/dev/mmcblk0p3 / ext4 ro,noatime 0 0" > "$F/mounts"
    : > "$F/e2fsck.log"; : > "$F/mount.log"
}
# con KIND SCRIPT ARGS: run with dash (Debian) or BusyBox ash under qemu (Buildroot)
con() {
    local kind=$1; shift
    local envs=(PATH="$SH:/usr/sbin:/usr/bin:/sbin:/bin" NZ_CONF="$F/sdroot.conf" NZ_MOUNTS="$F/mounts"
        NZ_RUN="$F/run" NZ_KMSG="$F/kmsg" NZ_CONFIGFS="$F/config" NZ_UDCDIR="$F/udc" NZ_UDC_WAIT=2
        NZ_SYS="$F/sys" NZ_PROC="$F/proc" NZ_ZRAM_DEV="$F/zram0" NZ_MOUNT="$SH/mount" $EXTRA)
    if test "$kind" = dash; then
        env -i "${envs[@]}" dash "$@" 2>&1
    else
        # BusyBox runs its own applets (uname, mount, date...) without PATH search,
        # so the kernel name comes from qemu and mount/date have NZ_* hooks.
        env -i "${envs[@]}" PATH="$SH:$BBBIN" QEMU_UNAME="$(cat "$F/uname")" "$QEMU" "$ASHBB" sh "$@" 2>&1
    fi
}
EXTRA=
S=$OVL/usr/local/sbin

# BusyBox ash behaviour runs need a fuller BusyBox (Buildroot-like: printf, mv,
# ...); the console's boot BusyBox only checks syntax (the shared scripts never
# run under it).
KINDS=dash
if test -n "$BBF"; then KINDS="dash ash"; else echo "SKIP BusyBox ash behaviour runs (give a full MIPS BusyBox as 2nd argument)"; fi
for K in $KINDS; do
    echo "== 3. behaviour with $K ($(test $K = dash && echo 'as Debian' || echo "BusyBox ash under qemu, as Buildroot"))"
    # ---------------- USB gadget: identical to /usb-rescue
    fresh; mkdir -p "$F/config/usb_gadget" "$F/udc/musb-hdrc.0"
    out=$(con $K "$S/nzeus-usb-gadget"); rc=$?
    ok "$K usb-gadget: fresh gadget made and bound (rc 0)" "test $rc = 0 && test \"\$(cat '$F/config/usb_gadget/nzeus_rescue/UDC')\" = musb-hdrc.0"
    ok "$K usb-gadget: identical to ports/r36s-hichip/usb-rescue (every value, link, nothing extra)" \
        "python3 '$HERE/tests/gadget_compare.py' '$REPO/ports/r36s-hichip/usb-rescue' '$F/config/usb_gadget/nzeus_rescue' musb-hdrc.0"
    fresh; G=$F/config/usb_gadget/nzeus_rescue; mkdir -p "$G" "$F/udc/musb-hdrc.0"; echo musb-hdrc.0 > "$G/UDC"; echo 0xdead > "$G/idVendor"
    out=$(con $K "$S/nzeus-usb-gadget"); rc=$?
    ok "$K usb-gadget: already plugged in by the RAM system -> unchanged" "test $rc = 0 && test \"\$(cat '$G/idVendor')\" = 0xdead && echo '$out' | grep -q unchanged"
    fresh; mkdir -p "$F/config/usb_gadget/nzeus_ncm" "$F/udc/musb-hdrc.0"; echo musb-hdrc.0 > "$F/config/usb_gadget/nzeus_ncm/UDC"
    out=$(con $K "$S/nzeus-usb-gadget"); rc=$?
    ok "$K usb-gadget: USB network mode active -> left alone" "test $rc = 0 && ! test -e '$F/config/usb_gadget/nzeus_rescue/UDC'"
    fresh; mkdir -p "$F/config/usb_gadget" "$F/udc/musb-hdrc.0"; echo 4.4.186 > "$F/uname"
    out=$(con $K "$S/nzeus-usb-gadget"); rc=$?
    ok "$K usb-gadget: stock kernel refused, nothing made" "test $rc = 1 && ! test -e '$F/config/usb_gadget/nzeus_rescue'"
    fresh; mkdir -p "$F/config/usb_gadget" "$F/udc"
    out=$(con $K "$S/nzeus-usb-gadget"); rc=$?
    ok "$K usb-gadget: no USB controller -> error, not bound" "test $rc = 1 && echo '$out' | grep -q 'no USB device controller' && ! test -s '$F/config/usb_gadget/nzeus_rescue/UDC'"

    # ---------------- root check
    for frc in 0 1; do
        fresh; echo $frc > "$F/fsck_rc_mmcblk0p3"
        out=$(con $K "$S/nzeus-rootcheck")
        ok "$K rootcheck: e2fsck rc $frc -> remount rw" "grep -qx -- '-o remount,rw,noatime,errors=remount-ro /' '$F/mount.log' && grep -q '^rw e2fsck=$frc' '$F/run/nzroot-state'"
        ok "$K rootcheck: e2fsck called exactly once as 'e2fsck -p DEV' (never -f/-y)" "test \"\$(cat '$F/e2fsck.log')\" = '-p $F/dev/mmcblk0p3'"
    done
    for frc in 2 3 4 8; do
        fresh; echo $frc > "$F/fsck_rc_mmcblk0p3"
        out=$(con $K "$S/nzeus-rootcheck")
        ok "$K rootcheck: e2fsck rc $frc -> stays READ-ONLY, no remount" "! test -s '$F/mount.log' && grep -q '^ro ' '$F/run/nzroot-state' && test \"\$(cat '$F/run/nzroot-fsck-rc')\" = $frc"
    done
    fresh; echo "$F/dev/mmcblk0p3 / ext4 rw,noatime 0 0" > "$F/mounts"; out=$(con $K "$S/nzeus-rootcheck")
    ok "$K rootcheck: already rw -> no e2fsck, no remount" "! test -s '$F/e2fsck.log' && ! test -s '$F/mount.log'"
    fresh; echo "/dev/root / ext4 ro 0 0" > "$F/mounts"; out=$(con $K "$S/nzeus-rootcheck")
    ok "$K rootcheck: another root device -> stays ro, no e2fsck" "! test -s '$F/e2fsck.log' && ! test -s '$F/mount.log' && grep -q '^ro ' '$F/run/nzroot-state'"
    for p in 1 2 5; do
        fresh; echo 0 > "$F/ro_mmcblk0p$p"; out=$(con $K "$S/nzeus-rootcheck")
        ok "$K rootcheck: p$p not read-only in the kernel -> stays ro, no e2fsck" "! test -s '$F/e2fsck.log' && ! test -s '$F/mount.log' && grep -q 'mmcblk0p$p is not read-only' '$F/run/nzroot-state'"
    done
    fresh; echo other > "$F/label_mmcblk0p3"; out=$(con $K "$S/nzeus-rootcheck")
    ok "$K rootcheck: wrong label -> stays ro, no e2fsck" "! test -s '$F/e2fsck.log' && ! test -s '$F/mount.log'"
    fresh; echo 32 > "$F/mount_rc"; out=$(con $K "$S/nzeus-rootcheck")
    ok "$K rootcheck: remount fails -> state says ro" "grep -q '^ro .*FAILED' '$F/run/nzroot-state'"

    # ---------------- /data
    fresh; out=$(con $K "$S/nzeus-datamount")
    ok "$K datamount: label ok + e2fsck 0 -> mount -t ext4 -o noatime,nodev,nosuid" "grep -qx -- '-t ext4 -o noatime,nodev,nosuid $F/dev/mmcblk0p4 $F/data' '$F/mount.log' && test \"\$(cat '$F/e2fsck.log')\" = '-p $F/dev/mmcblk0p4'"
    fresh; echo 4 > "$F/fsck_rc_mmcblk0p4"; out=$(con $K "$S/nzeus-datamount")
    ok "$K datamount: e2fsck 4 -> NOT mounted" "! test -s '$F/mount.log' && echo '$out' | grep -q 'NOT mounted'"
    fresh; echo wrong > "$F/label_mmcblk0p4"; out=$(con $K "$S/nzeus-datamount")
    ok "$K datamount: wrong label -> no e2fsck, not mounted" "! test -s '$F/e2fsck.log' && ! test -s '$F/mount.log'"
    fresh; echo "$F/dev/mmcblk0p4 $F/data ext4 rw 0 0" >> "$F/mounts"; out=$(con $K "$S/nzeus-datamount")
    ok "$K datamount: already mounted -> no e2fsck on a mounted file system" "! test -s '$F/e2fsck.log' && ! test -s '$F/mount.log'"

    # ---------------- clock (fake date: "now" in $F/now; "-u -s X" only notes X)
    fresh
    cat > "$SH/fakedate" <<EOF
#!/bin/bash
n=\$(cat $F/now)
if [ "\$1" = +%s ]; then echo "\$n"
elif [ "\$1" = -u ] && [ "\$2" = -s ]; then echo "\$3" > $F/set
elif [ "\$1" = -u ]; then /bin/date -u -d "@\$n" "\$2"
fi
EOF
    chmod 755 "$SH/fakedate"
    clk() {
        local r
        EXTRA="NZ_CLOCK_DATE=$SH/fakedate NZ_CLOCK_DATA=$F/data NZ_CLOCK_MOUNTED=$1"
        shift; con $K "$S/nzeus-clock" "$@"; r=$?; EXTRA=; return $r
    }
    mkdir -p "$F/data/nzeus"
    echo 1000 > "$F/now"
    out=$(clk true restore)
    ok "$K clock: nothing saved -> clock untouched" "! test -e '$F/set' && echo '$out' | grep -q 'nothing saved'"
    printf '1795000000\n2026-11-18 07:06:40 UTC\n' > "$F/data/nzeus/clock.txt"
    out=$(clk true restore)
    ok "$K clock: /data clock.txt -> set from it" "test \"\$(cat '$F/set')\" = '2026-11-18 07:06:40'"
    rm -f "$F/set"; echo 1800000000 > "$F/now"; out=$(clk true restore)
    ok "$K clock: clock already ahead -> untouched" "! test -e '$F/set'"
    rm -f "$F/set"; echo 1000 > "$F/now"; printf 'junk\nnot a date\n' > "$F/data/nzeus/clock.txt"
    out=$(clk true restore)
    ok "$K clock: junk clock.txt ignored" "! test -e '$F/set'"
    printf '1790000000\nnot-a-date-at-all\n' > "$F/data/nzeus/clock.txt"; out=$(clk true restore)
    ok "$K clock: bad date line not used" "! test -e '$F/set'"
    rm -f "$F/data/nzeus/clock.txt"; echo 1791234567 > "$F/now"
    out=$(clk true save)
    ok "$K clock: save -> 2 lines (seconds, UTC date), no .tmp left" "test \"\$(head -n 1 '$F/data/nzeus/clock.txt')\" = 1791234567 && test \"\$(sed -n 2p '$F/data/nzeus/clock.txt')\" = '2026-10-05 21:09:27 UTC' && ! test -e '$F/data/nzeus/clock.txt.tmp'"
    echo 1000 > "$F/now"; rm -f "$F/set"; out=$(clk true restore)
    ok "$K clock: restore reads back what save wrote" "test \"\$(cat '$F/set')\" = '2026-10-05 21:09:27'"
    rm -f "$F/data/nzeus/clock.txt"; echo 1791234567 > "$F/now"
    out=$(clk false save); rc=$?
    ok "$K clock: /data not mounted -> nothing written, rc 1" "test $rc = 1 && ! test -e '$F/data/nzeus/clock.txt'"
    echo 1000 > "$F/now"; out=$(clk true save); rc=$?
    ok "$K clock: clock still 1970 -> nothing written" "test $rc = 1 && ! test -e '$F/data/nzeus/clock.txt'"

    # ---------------- zram
    fresh; mkdir -p "$F/sys/block/zram0" "$F/proc"; echo 0 > "$F/sys/block/zram0/disksize"
    printf 'MemTotal:          39588 kB\nMemFree: 1000 kB\n' > "$F/proc/meminfo"; printf 'Filename Type Size Used Priority\n' > "$F/proc/swaps"
    out=$(con $K "$S/nzeus-zram")
    ok "$K zram: half of MemTotal (19794K), mkswap + swapon -p 100" "test \"\$(cat '$F/sys/block/zram0/disksize')\" = 19794K && grep -qx 'mkswap $F/zram0' '$F/swap.log' && grep -qx 'swapon -p 100 $F/zram0' '$F/swap.log'"
    rm -f "$F/swap.log"; echo '/dev/zram0 partition 19792 0 100' >> "$F/proc/swaps"; out=$(con $K "$S/nzeus-zram")
    ok "$K zram: already on -> nothing" "! test -e '$F/swap.log' && echo '$out' | grep -q 'already on'"
    fresh; mkdir -p "$F/proc"; printf 'MemTotal: 39588 kB\n' > "$F/proc/meminfo"; : > "$F/proc/swaps"; out=$(con $K "$S/nzeus-zram"); rc=$?
    ok "$K zram: kernel without zram -> message, rc 0" "test $rc = 0 && echo '$out' | grep -q 'no zram0'"

    # ---------------- nzeus-status on a fake console
    fresh; mkdir -p "$F/proc" "$F/sys/class/power_supply/battery" "$F/sys/class/power_supply/charger" "$F/config/usb_gadget/nzeus_rescue"
    printf 'MemTotal: 39588 kB\nMemAvailable: 20000 kB\nSwapTotal: 19792 kB\nSwapFree: 19792 kB\n' > "$F/proc/meminfo"
    printf '%s / ext4 ro,noatime 0 0\n/dev/mmcblk0p1 /mnt/stock vfat ro,noatime 0 0\n' "$F/dev/mmcblk0p3" > "$F/proc/mounts"
    echo '1234.5 1000.0' > "$F/proc/uptime"
    echo 95 > "$F/sys/class/power_supply/battery/capacity"; echo Discharging > "$F/sys/class/power_supply/battery/status"; echo 0 > "$F/sys/class/power_supply/charger/online"
    for p in 1 3 4; do mkdir -p "$F/sys/class/block/mmcblk0p$p"; echo 8388608 > "$F/sys/class/block/mmcblk0p$p/size"; done
    echo 1 > "$F/sys/class/block/mmcblk0p1/ro"; echo 0 > "$F/sys/class/block/mmcblk0p3/ro"
    echo musb-hdrc.0 > "$F/config/usb_gadget/nzeus_rescue/UDC"; echo "ro e2fsck=4" > "$F/run/nzroot-state"
    out=$(con $K "$OVL/usr/local/bin/nzeus-status"); rc=$?
    ok "$K status: runs (rc 0), battery 95%, root READ-ONLY, partitions with ro flag" "test $rc = 0 && echo \"\$out\" | grep -q 'Battery: *95% (Discharging), on battery' && echo \"\$out\" | grep -q 'Root /: *READ-ONLY (ro e2fsck=4)' && echo \"\$out\" | grep -q 'mmcblk0p1 .* 4096 MiB  ro=1' && echo \"\$out\" | grep -q 'boot files (read-only'"
    ok "$K status: memory and USB lines" "echo \"\$out\" | grep -q 'Memory: *38 MiB total, 19 MiB available' && echo \"\$out\" | grep -q 'plugged into musb-hdrc.0'"
    out=$(con $K "$OVL/usr/local/bin/nzeus-status" --short)
    ok "$K status --short: one line" "test \"\$(echo \"\$out\" | wc -l)\" = 1 && echo \"\$out\" | grep -q '^root: READ-ONLY'"
done

echo "== 4. USB shell + unchanged rescue tools over a pty"
PT=$T/pty
for shell in /bin/bash /bin/sh; do
    out=$(timeout 300 python3 "$HERE/tests/rescue_pty_test.py" --wrapper "$OVL/usr/local/sbin/nzeus-usb-shell" --shell "$shell" \
        --inputrc "$OVL/etc/nzeus/inputrc" --work "$PT$(basename $shell)" 2>&1)
    echo "$out" | sed 's/^/    /'
    ok "rescue tools work unchanged with $shell" "echo \"\$out\" | grep -q '^RESCUE_PTY PASSED'"
done
# negative control: without our inputrc, bash 5.2 sends bracketed-paste codes (the check must see them)
out=$(timeout 300 python3 "$HERE/tests/rescue_pty_test.py" --wrapper "$OVL/usr/local/sbin/nzeus-usb-shell" --shell /bin/bash \
    --work "$PT-noinputrc" 2>&1)
ok "control: bash WITHOUT /etc/nzeus/inputrc is caught (bracketed paste)" "echo \"\$out\" | grep -q '^FAIL rescue-command: no bracketed-paste'"
rmdir /tmp/nzeus-tools 2>/dev/null

echo "== 5. apply-overlay.sh"
R=$T/root-debian
mkdir -p "$R"
printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFakeKeyForTheTestOnlyxxxxxxxxxxxxxxxxxxxxxx test@pc\n' > "$T/test.pub"
# a tiny emoji pack (tools/make_emoji.py's layout); a CR byte inside a picture
# proves the pictures are copied as binary files
EMO=$T/emoji-pack; mkdir -p "$EMO"
printf '\005\100\000\002ab\rcd' > "$EMO/1f600.bin"; printf 'xyz' > "$EMO/2764.bin"
printf '2764\n1f600\n' > "$EMO/index.txt"; echo 'Twemoji CC-BY 4.0 (test)' > "$EMO/LICENSE.txt"; echo junk > "$EMO/notes.tmp"
out=$(NZ_AUTHORIZED_KEYS=$T/test.pub NZ_EMOJI_DIR=$EMO sh "$HERE/apply-overlay.sh" "$R" debian 2>&1); rc=$?
echo "$out" | sed 's/^/    /'
ok "apply-overlay debian: rc 0" "test $rc = 0"
DB=$R/usr/local/lib/nzeus/deck-bin
while read -r helper; do
    ok "Deck helper $helper: installed with mode 755" "test -f '$DB/$helper' && test \$(stat -c %a '$DB/$helper') = 755"
done < "$REPO/core/deck-modern/required-files.txt"
ok "Deck helper manifest: every recorded hash matches" "(cd '$DB' && sha256sum -c SHA256SUMS >/dev/null)"
ok "Deck: the Wi-Fi joiner (wifi-join-core.sh) comes from core/deck-modern" "cmp -s '$REPO/core/deck-modern/wifi-join-core.sh' '$DB/wifi-join-core.sh'"
ok "Deck: nzeus-bt.bin = the checked bt/nzeus-bt-mipsel, mode 755" "cmp -s '$REPO/core/deck-modern/bt/nzeus-bt-mipsel' '$DB/nzeus-bt.bin' && test \$(stat -c %a '$DB/nzeus-bt.bin') = 755 && (cd '$REPO/core/deck-modern/bt' && sha256sum -c SHA256SUMS >/dev/null)"
ok "Deck: no Claude-access stub any more" "! test -e '$DB/nzeus-claude-access.sh'"
PX=$R/usr/local/lib/nzeus/python-examples
n_src=$(ls "$REPO"/core/deck-modern/python-examples/*.py | wc -l)
ok "Python examples: all $n_src in /usr/local/lib/nzeus/python-examples, mode 644, unchanged" \
    "test \$(ls '$PX'/*.py | wc -l) = $n_src && test $n_src -ge 7 && ( for f in '$REPO'/core/deck-modern/python-examples/*.py; do cmp -s \"\$f\" '$PX'/\"\${f##*/}\" && test \$(stat -c %a '$PX'/\"\${f##*/}\") = 644 || exit 1; done )"
EI=$R/usr/local/lib/nzeus/emoji
ok "emoji: pictures + index + licence copied byte for byte (a CR byte too), mode 644, nothing else" \
    "cmp -s '$EMO/1f600.bin' '$EI/1f600.bin' && cmp -s '$EMO/2764.bin' '$EI/2764.bin' && cmp -s '$EMO/index.txt' '$EI/index.txt' && test -f '$EI/LICENSE.txt' && ! test -e '$EI/notes.tmp' && test \$(stat -c %a '$EI/1f600.bin') = 644 && test \$(ls '$EI' | wc -l) = 4"
ok "nzeus-deck: links NZ_DIR/emoji to the system's pack only when there is none" "grep -q 'test -d /usr/local/lib/nzeus/emoji && ! test -e /data/nzeus/deck/nzeus/emoji' '$OVL/usr/local/bin/nzeus-deck'"
ok "nzeus-deck: no read-only card link for the Deck (NZ_SDCARD) any more" "! grep -q NZ_SDCARD '$OVL/usr/local/bin/nzeus-deck'"
for f in etc/inittab etc/fstab etc/init.d/nzeus-rootfs usr/local/sbin/nzeus-rootcheck usr/local/sbin/nzeus-usb-shell \
         usr/local/bin/nzeus-status usr/local/bin/nzeus-modern usr/local/sbin/dropbearmulti-modern \
         usr/local/lib/nzeus/usbnet/usbnet-up.sh etc/nzeus-rootfs-release root/.ssh/authorized_keys \
         usr/local/bin/usb-host-trial usr/local/bin/wifi-up usr/local/sbin/nz-cdeject usr/local/sbin/usb-regs \
         usr/local/lib/nzeus/usbhost/udhcpc.script usr/local/lib/nzeus/usbhost/wifi.conf.example; do
    ok "apply-overlay debian: /$f" "test -f '$R/$f'"
done
ok "modes: init.d + sbin 755, fstab 644, root 700, .ssh 700, authorized_keys 600" \
    "test \$(stat -c %a '$R/etc/init.d/nzeus-rootfs') = 755 && test \$(stat -c %a '$R/usr/local/sbin/nzeus-clock') = 755 && test \$(stat -c %a '$R/etc/fstab') = 644 && test \$(stat -c %a '$R/root') = 700 && test \$(stat -c %a '$R/root/.ssh') = 700 && test \$(stat -c %a '$R/root/.ssh/authorized_keys') = 600"
ok "usbnet scripts are unchanged copies" "cmp -s '$REPO/ports/r36s-hichip/usbnet/usbnet-up.sh' '$R/usr/local/lib/nzeus/usbnet/usbnet-up.sh'"
for f in usb-host-trial.sh usb-host-confirm.sh usb-host-off.sh usb-host-status.sh wifi-up.sh wifi-down.sh udhcpc.script; do
    ok "usbhost $f: unchanged copy, mode 755" "cmp -s '$REPO/ports/r36s-hichip/usbhost/$f' '$R/usr/local/lib/nzeus/usbhost/$f' && test \$(stat -c %a '$R/usr/local/lib/nzeus/usbhost/$f') = 755"
done
ok "usbhost programs: the checked static MIPS files, 755" "cmp -s '$REPO/ports/r36s-hichip/usbhost/nz-cdeject-mipsel' '$R/usr/local/sbin/nz-cdeject' && cmp -s '$REPO/ports/r36s-hichip/usbhost/usb-regs-mipsel' '$R/usr/local/sbin/usb-regs' && test \$(stat -c %a '$R/usr/local/sbin/nz-cdeject') = 755"
ok "usbhost wrappers call the installed copies" "grep -q '^exec /bin/sh /usr/local/lib/nzeus/usbhost/usb-host-trial.sh \"\$@\"\$' '$R/usr/local/bin/usb-host-trial' && grep -q '^exec /bin/sh /usr/local/lib/nzeus/usbhost/wifi-up.sh \"\$@\"\$' '$R/usr/local/bin/wifi-up'"
ok "no Wi-Fi settings file in the image (only the example)" "! test -e '$R/data/nzeus/wifi.conf' && ! find '$R' -name wifi.conf | grep -q ."
ok "release file says NZEUS_ROOTFS_LAYOUT=1" "grep -qx NZEUS_ROOTFS_LAYOUT=1 '$R/etc/nzeus-rootfs-release'"
printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAA\n-----END OPENSSH PRIVATE KEY-----\n' > "$T/private_key_test"
mkdir -p "$T/root-x"; out=$(NZ_AUTHORIZED_KEYS=$T/private_key_test sh "$HERE/apply-overlay.sh" "$T/root-x" debian 2>&1); rc=$?
ok "apply-overlay: a PRIVATE key is refused" "test $rc != 0 && echo '$out' | grep -q 'PRIVATE key' && ! test -e '$T/root-x/root/.ssh/authorized_keys'"
printf 'this is not a key\n' > "$T/bad.pub"
out=$(NZ_AUTHORIZED_KEYS=$T/bad.pub sh "$HERE/apply-overlay.sh" "$T/root-x" debian 2>&1); rc=$?
ok "apply-overlay: a non-key line is refused" "test $rc != 0"
C=$T/crlfcopy/ports/r36s-hichip/rootfs/debian
mkdir -p "$C" && cp -a "$HERE/." "$C/" && mkdir -p "$T/crlfcopy/ports/r36s-hichip/usbnet" "$T/crlfcopy/ports/r36s-hichip/usbhost" "$T/crlfcopy/apps" && cp -a "$REPO/ports/r36s-hichip/usbnet/." "$T/crlfcopy/ports/r36s-hichip/usbnet/" && cp -a "$REPO/ports/r36s-hichip/usbhost/." "$T/crlfcopy/ports/r36s-hichip/usbhost/" && cp -a "$REPO/apps/frontend-modern" "$T/crlfcopy/apps/"
printf 'echo hi\r\n' >> "$C/overlay/usr/local/sbin/nzeus-clock"
mkdir -p "$T/root-crlf"; out=$(sh "$C/apply-overlay.sh" "$T/root-crlf" debian 2>&1); rc=$?
ok "apply-overlay: a script with Windows line ends is refused" "test $rc != 0 && echo '$out' | grep -q CRLF"
# Reuse only the PC fixture: a missing required helper must fail the image build.
cp "$OVL/usr/local/sbin/nzeus-clock" "$C/overlay/usr/local/sbin/nzeus-clock"
mkdir -p "$T/crlfcopy/core" && cp -a "$REPO/core/deck-modern" "$T/crlfcopy/core/"
rm -f "$T/crlfcopy/core/deck-modern/python"
mkdir -p "$T/root-missing-helper"
out=$(sh "$C/apply-overlay.sh" "$T/root-missing-helper" debian 2>&1); rc=$?
ok "apply-overlay: missing required Deck helper is refused" "test $rc != 0 && echo '$out' | grep -q 'missing .*deck-modern/python'"
RB=$T/root-buildroot; mkdir -p "$RB"
out=$(NZ_EMOJI_DIR=$T/no-emoji-here sh "$HERE/apply-overlay.sh" "$RB" buildroot 2>&1); rc=$?
ok "apply-overlay: no emoji pack -> a warning, no emoji folder, still rc 0" "test $rc = 0 && echo '$out' | grep -q 'WARNING no emoji pack' && ! test -e '$RB/usr/local/lib/nzeus/emoji'"
ok "apply-overlay buildroot: rc 0, shared scripts in, Debian-only files out" "test $rc = 0 && test -x '$RB/usr/local/sbin/nzeus-rootcheck' && test -x '$RB/usr/local/sbin/nzeus-usb-shell' && ! test -e '$RB/etc/inittab' && ! test -e '$RB/etc/fstab' && ! test -e '$RB/etc/init.d' && ! test -e '$RB/etc/apt'"
ok "buildroot inittab uses the shared USB shell" "grep -q '^ttyGS0::respawn:/usr/local/sbin/nzeus-usb-shell$' '$BRO/overlay/etc/inittab'"
ok "buildroot defconfig: no unconditional remount rw" "! grep -q '^BR2_TARGET_GENERIC_REMOUNT_ROOTFS_RW=y' '$REPO/ports/r36s-hichip/rootfs/buildroot/configs/nzeus_r36s_defconfig'"

echo "== 6. fake root folder -> make-image.sh -> rootfs.ext4 -> nz_region.py"
FR=$T/fakeroot
rm -rf "$FR"; mkdir -p "$FR/bin" "$FR/sbin" "$FR/etc" "$FR/dev" "$FR/proc" "$FR/sys" "$FR/run" "$FR/tmp" "$FR/var/log"
cp "$ASHBB" "$FR/bin/busybox"; ln -s busybox "$FR/bin/sh"; ln -s ../bin/busybox "$FR/sbin/init"
mknod "$FR/dev/console" c 5 1; mknod "$FR/dev/null" c 1 3; chmod 1777 "$FR/tmp"
echo 'root:x:0:0:root:/root:/bin/sh' > "$FR/etc/passwd"; echo 'nzuser:x:1000:1000::/home/nzuser:/bin/sh' >> "$FR/etc/passwd"
mkdir -p "$FR/home/nzuser"; chown 1000:1000 "$FR/home/nzuser"
NZ_AUTHORIZED_KEYS=$T/test.pub sh "$HERE/apply-overlay.sh" "$FR" debian > /dev/null
IMG=$T/out/rootfs.ext4; mkdir -p "$T/out"; rm -f "$IMG" "$IMG.info"
out=$(bash "$HERE/make-image.sh" "$FR" "$IMG" 2>&1); rc=$?
echo "$out" | sed 's/^/    /'
ok "make-image: rc 0" "test $rc = 0"
ok "make-image: exactly 4 GiB, sparse (< 300 MiB on disk)" "test \$(stat -c %s '$IMG') = 4294967296 && test \$((\$(stat -c %b '$IMG') * \$(stat -c %B '$IMG'))) -lt 314572800"
ok "image: e2fsck -fn clean" "e2fsck -fn '$IMG' > /dev/null 2>&1"
feat=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Filesystem features:[[:space:]]*//p')
ok "image: features ok, no metadata_csum_seed/orphan_file" "echo ' $feat ' | grep -q ' metadata_csum ' && echo ' $feat ' | grep -q ' 64bit ' && ! echo ' $feat ' | grep -qE ' (metadata_csum_seed|orphan_file) '"
ok "image: label nzroot, 4K blocks, mount count + interval off, errors=remount-ro" "dumpe2fs -h '$IMG' 2>/dev/null | grep -q '^Filesystem volume name: *nzroot$' && dumpe2fs -h '$IMG' 2>/dev/null | grep -q '^Block size: *4096$' && dumpe2fs -h '$IMG' 2>/dev/null | grep -q '^Maximum mount count: *-1$' && dumpe2fs -h '$IMG' 2>/dev/null | grep -q '^Check interval: *0 ' && dumpe2fs -h '$IMG' 2>/dev/null | grep -q '^Errors behavior: *Remount read-only$'"
ok "image: /etc/inittab content identical" "debugfs -R 'cat /etc/inittab' '$IMG' 2>/dev/null | cmp -s - '$OVL/etc/inittab'"
ok "image: nzeus-rootcheck mode 0755 owner root" "debugfs -R 'stat /usr/local/sbin/nzeus-rootcheck' '$IMG' 2>/dev/null | grep -q 'Mode:  0755' && debugfs -R 'stat /usr/local/sbin/nzeus-rootcheck' '$IMG' 2>/dev/null | grep -q 'User:     0   Group:     0'"
ok "image: owners kept (/home/nzuser = 1000), /root 0700, device node kept" "debugfs -R 'stat /home/nzuser' '$IMG' 2>/dev/null | grep -q 'User:  1000' && debugfs -R 'stat /root' '$IMG' 2>/dev/null | grep -q 'Mode:  0700' && debugfs -R 'stat /dev/console' '$IMG' 2>/dev/null | grep -q 'character special'"
ok "image: /sbin/init symlink and release file" "debugfs -R 'stat /sbin/init' '$IMG' 2>/dev/null | grep -q 'symlink' && debugfs -R 'cat /etc/nzeus-rootfs-release' '$IMG' 2>/dev/null | grep -qx NZEUS_ROOTFS_LAYOUT=1"
ok "image: .info has sha256 that matches" "grep -qx \"sha256=\$(sha256sum '$IMG' | cut -d' ' -f1)\" '$IMG.info'"
out=$(bash "$HERE/make-image.sh" "$FR" "$IMG" 2>&1); rc=$?
ok "make-image: refuses to overwrite an existing image" "test $rc != 0 && echo '$out' | grep -q 'already exists'"
mkdir -p "$T/noroot/sbin"; ln -sf /bin/sh "$T/noroot/sbin/init"
out=$(bash "$HERE/make-image.sh" "$T/noroot" "$T/out/x.ext4" 2>&1); rc=$?
ok "make-image: refuses a folder without etc/nzeus-rootfs-release" "test $rc != 0 && ! test -e '$T/out/x.ext4'"
out=$(bash "$HERE/make-image.sh" "$FR" /dev/null 2>&1); rc=$?
ok "make-image: refuses a device path" "test $rc != 0"
if command -v mkfs.fat > /dev/null && command -v mtype > /dev/null; then
    out=$(python3 "$REPO/ports/r36s-hichip/rootfs/region-tool/nz_region.py" selftest --workdir "$T" --rootfs-ext4 "$IMG" 2>&1); rc=$?
    echo "$out" | tail -n 4 | sed 's/^/    /'
    ok "nz_region.py selftest --rootfs-ext4 <our image> PASSED" "test $rc = 0 && echo \"\$out\" | grep -q 'SELFTEST PASSED'"
    rm -rf "$T"/nz-region-selftest-*
else
    echo "SKIP nz_region.py selftest (needs dosfstools + mtools)"
fi

echo "== 7. build-debian.sh refusals (before it does anything)"
if grep -qiE 'microsoft|wsl' /proc/sys/kernel/osrelease; then
    echo "SKIP not-WSL refusal (this IS WSL)"
else
    out=$(env -u NZ_ALLOW_NON_WSL bash "$HERE/build-debian.sh" "$T/w" 2>&1); rc=$?
    ok "build-debian: refuses a PC that is not WSL" "test $rc != 0 && echo '$out' | grep -q 'not WSL'"
fi
out=$(NZ_ALLOW_NON_WSL=1 bash "$HERE/build-debian.sh" /mnt/c/nzeus-test 2>&1); rc=$?
ok "build-debian: refuses a Windows folder (/mnt/c)" "test $rc != 0 && echo '$out' | grep -q 'Windows folder'"
out=$(NZ_ALLOW_NON_WSL=1 bash "$HERE/build-debian.sh" /proc/nzeus-test 2>&1); rc=$?
ok "build-debian: refuses a non-Linux-disk file system (proc)" "test $rc != 0 && echo '$out' | grep -q 'file system'"

echo
echo "RESULT: $pass passed, $fail failed (work folder $T)"
test "$fail" = 0

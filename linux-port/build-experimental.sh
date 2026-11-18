#!/bin/bash
# Build artifacts on the PC only. Never uploads, boots, flashes or modifies a card.
set -euo pipefail
R=$(cd "$(dirname "$0")" && pwd)
K=${NZEUS_KERNEL_TREE:-/home/nzeus/r36s-linux-port-20261007}
W=${NZEUS_PORT_WORK:-/home/nzeus/r36s-linux-port-work-20261007}
PIN=da5f1a22ef2ff5c46aa3f750fa10825813970b09
JOBS=${NZEUS_BUILD_JOBS:-8}
test "$(git -C "$K" rev-parse HEAD)" = "$PIN"
python3 "$R/apply-port-patches.py" "$K"
for tool in mipsel-linux-gnu-gcc qemu-mipsel make cpio; do command -v "$tool" >/dev/null; done
mkdir -p "$W" "$R/build" "$R/artifacts"
exec > >(tee -a "$R/build/kernel-build.log") 2>&1
export SOURCE_DATE_EPOCH=$(git -C "$K" show -s --format=%ct HEAD)
export KBUILD_BUILD_TIMESTAMP='2026-08-31 07:15:24 UTC'
export KBUILD_BUILD_USER=nzeus KBUILD_BUILD_HOST=nzeus-deck
B="$W/busybox-1.37.0"
O="$W/kernel"

if [ "${1:-}" != --export-only ]; then
echo '1. Verify official BusyBox source and build a static MIPS shell.'
(cd "$R/upstream" && sha256sum -c busybox-1.37.0.tar.bz2.sha256)
B="$W/busybox-1.37.0"
[ -d "$B" ] || tar -xjf "$R/upstream/busybox-1.37.0.tar.bz2" -C "$W"
make -C "$B" allnoconfig > "$R/build/busybox-config.log"
for option in BUSYBOX SHOW_USAGE FEATURE_VERBOSE_USAGE STATIC LFS TIME64 ASH SH_IS_ASH ASH_JOB_CONTROL ASH_ALIAS FEATURE_SH_MATH FEATURE_SH_MATH_64 \
    FEATURE_SH_STANDALONE FEATURE_EDITING MOUNT UMOUNT CAT ECHO LS GREP SED AWK \
    CUT HEAD TAIL DMESG UNAME FREE PS MKDIR MKNOD CHMOD SLEEP CP MV LN SYNC TRUE FALSE \
    CTTYHACK SETSID STTY TTY TEST FEATURE_TEST_64 IP FEATURE_IP_ADDRESS FEATURE_IP_LINK \
    FEATURE_IP_ROUTE PING HOSTNAME VI DF DU IFCONFIG; do
    "$K/scripts/config" --file "$B/.config" --enable "$option"
done
# BusyBox's older Kconfig has oldconfig, not the kernel's olddefconfig target.
(set +o pipefail; yes '' | make -C "$B" oldconfig) >> "$R/build/busybox-config.log" 2>&1
make -C "$B" -j"$JOBS" CROSS_COMPILE=mipsel-linux-gnu- \
    EXTRA_CFLAGS='-march=mips32r2 -mabi=32' > "$R/build/busybox-build.log" 2>&1
if mipsel-linux-gnu-readelf -l "$B/busybox" | grep -q INTERP; then
    echo 'STOP: rescue BusyBox is not static.'; exit 1
fi
qemu-mipsel "$B/busybox" --list > "$R/build/busybox-applets.txt"
qemu-mipsel "$B/busybox" ash -c 'test "$((2 + 3))" = 5 && echo "PASS: MIPS BusyBox shell under QEMU"'
cp "$R/init" "$W/init"
chmod +x "$W/init"
cp "$R/usb-rescue" "$W/usb-rescue"
chmod +x "$W/usb-rescue"
{
    for directory in /bin /dev /proc /sys /tmp /run; do printf 'dir %s 0755 0 0\n' "$directory"; done
    printf 'nod /dev/console 0600 0 0 c 5 1\nnod /dev/null 0666 0 0 c 1 3\n'
    printf 'file /bin/busybox %s 0755 0 0\n' "$B/busybox"
    printf 'file /init %s 0755 0 0\n' "$W/init"
    printf 'file /usb-rescue %s 0755 0 0\n' "$W/usb-rescue"
    while read -r applet; do
        [ "$applet" = busybox ] || printf 'slink /bin/%s busybox 0777 0 0\n' "$applet"
    done < "$R/build/busybox-applets.txt"
} > "$W/initramfs.list"

echo '2. Resolve kernel configuration and compile the experimental image.'
O="$W/kernel"
mkdir -p "$O"
cp "$R/r36s-e3100-bringup.dts" "$K/arch/mips/boot/dts/hichip/"
make -C "$K" O="$O" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- sf3000_defconfig
# The upstream merge script splits filenames on spaces, so use the WSL copy.
cp "$R/bringup.config" "$W/bringup.config"
(cd "$K" && scripts/kconfig/merge_config.sh -m -O "$O" "$O/.config" "$W/bringup.config")
"$K/scripts/config" --file "$O/.config" --set-str INITRAMFS_SOURCE "$W/initramfs.list" \
    --enable INITRAMFS_COMPRESSION_GZIP --disable DEBUG_INFO \
    --set-str LOCALVERSION '-nzeus-experimental' --disable LOCALVERSION_AUTO
make -C "$K" O="$O" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- olddefconfig
for option in INET UNIX98_PTYS BLK_DEV_INITRD DEVTMPFS TMPFS; do
    grep -qx "CONFIG_$option=y" "$O/.config" || { echo "Missing $option"; exit 1; }
done
make -C "$K" O="$O" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j"$JOBS" \
    vmlinux vmlinuz.bin hichip/r36s-e3100-bringup.dtb
fi

echo '3. Save artifacts; do not deploy them.'
cp "$O/arch/mips/boot/vmlinuz.bin" "$R/artifacts/vmlinuz.bin"
cp "$O/vmlinuz" "$R/artifacts/nzeus-compressed.elf"
cp "$O/arch/mips/boot/dts/hichip/r36s-e3100-bringup.dtb" "$R/artifacts/"
cat "$R/artifacts/vmlinuz.bin" "$R/artifacts/r36s-e3100-bringup.dtb" \
    > "$R/artifacts/nzeus-r36s-experimental-with-dtb.bin"
cp "$O/.config" "$R/artifacts/kernel.config"
cp "$B/.config" "$R/artifacts/busybox.config"
cp "$B/busybox" "$R/artifacts/busybox-mipsel"
# This kernel keeps the compressed archive under initramfs_inc_data.
gzip -t "$O/usr/initramfs_inc_data"
cp "$O/usr/initramfs_inc_data" "$R/artifacts/initramfs.cpio.gz"
cp "$O/vmlinux" "$R/artifacts/nzeus-kernel.elf"
mipsel-linux-gnu-readelf -h -l "$O/vmlinux" > "$R/artifacts/kernel-elf.txt"
mipsel-linux-gnu-readelf -h -l "$B/busybox" > "$R/artifacts/busybox-elf.txt"
gzip -dc "$R/artifacts/initramfs.cpio.gz" | cpio -it > "$R/artifacts/initramfs-files.txt"
(cd "$R/artifacts" && sha256sum *.bin *.dtb *.config busybox-mipsel *.gz *.elf > SHA256SUMS)
echo 'PASS: PC build complete. Hardware boot and SD standalone boot remain UNTESTED.'
python3 "$R/verify-artifacts.py" | tee "$R/build/artifact-verification.txt"

#!/bin/bash
# Build the gma-native-sdroot initramfs parts from sources in this repo:
#   - the boot BusyBox (official 1.37.0 tarball, ports/r36s-hichip/artifacts/busybox.config
#     plus SWITCH_ROOT KILL MOUNTPOINT BLOCKDEV FEATURE_MOUNT_FLAGS), static MIPS
#   - nz-sdboot (static MIPS, -Os)
#   - /init (this folder) and /usb-rescue (ports/r36s-hichip/usb-rescue, unchanged)
#   - initramfs.list for the kernel's CONFIG_INITRAMFS_SOURCE
# Usage: bash build-initramfs.sh WORK_DIR
# PC build only: no device access, no card, no upload, no flashing.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$H/.." && pwd)
S=${1:?usage: build-initramfs.sh WORK_DIR}
JOBS=${NZEUS_BUILD_JOBS:-8}
for tool in mipsel-linux-gnu-gcc mipsel-linux-gnu-readelf qemu-mipsel make bunzip2 sha256sum; do
    command -v "$tool" > /dev/null || { echo "STOP: $tool missing"; exit 1; }
done
mkdir -p "$S"
S=$(cd "$S" && pwd)
# BusyBox's version banner date and config header: fixed, not the build day.
export SOURCE_DATE_EPOCH=$(date -u -d '2026-10-10 15:00:00 UTC' +%s) KCONFIG_NOTIMESTAMP=1

echo '1. BusyBox: official source, boot config + switch_root/kill/mountpoint/blockdev.'
(cd "$P/upstream" && sha256sum -c busybox-1.37.0.tar.bz2.sha256)
B="$S/busybox-1.37.0"
test -d "$B" || tar -xjf "$P/upstream/busybox-1.37.0.tar.bz2" -C "$S"
cp "$P/artifacts/busybox.config" "$B/.config"
for option in SWITCH_ROOT KILL MOUNTPOINT BLOCKDEV FEATURE_MOUNT_FLAGS; do
    grep -qx "# CONFIG_$option is not set" "$B/.config"
    sed -i "s/^# CONFIG_$option is not set\$/CONFIG_$option=y/" "$B/.config"
done
(set +o pipefail; yes '' | make -C "$B" oldconfig) > "$S/busybox-config.log" 2>&1
# The resolved config must be exactly the reviewed one (comments/dates ignored).
diff <(grep '^CONFIG_\|^# CONFIG_' "$H/busybox.config") <(grep '^CONFIG_\|^# CONFIG_' "$B/.config") \
    || { echo 'STOP: BusyBox config differs from ports/r36s-hichip/sdroot/busybox.config'; exit 1; }
make -C "$B" -j"$JOBS" CROSS_COMPILE=mipsel-linux-gnu- \
    EXTRA_CFLAGS='-march=mips32r2 -mabi=32' busybox > "$S/busybox-build.log" 2>&1
if mipsel-linux-gnu-readelf -l "$B/busybox" | grep -q INTERP; then
    echo 'STOP: boot BusyBox is not static.'; exit 1
fi
qemu-mipsel "$B/busybox" --list > "$S/busybox-applets.txt"
diff "$H/busybox-applets.txt" "$S/busybox-applets.txt" \
    || { echo 'STOP: applet list differs from ports/r36s-hichip/sdroot/busybox-applets.txt'; exit 1; }
cp "$B/busybox" "$S/busybox"

echo '2. nz-sdboot: static MIPS helper (reads only).'
mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32 -Wall -Wextra -Werror \
    -o "$S/nz-sdboot" "$H/nz-sdboot.c"
if mipsel-linux-gnu-readelf -l "$S/nz-sdboot" | grep -q INTERP; then
    echo 'STOP: nz-sdboot is not static.'; exit 1
fi
set +e
qemu-mipsel "$S/nz-sdboot" --bogus > /dev/null 2>&1
test $? = 2 || { echo 'STOP: nz-sdboot does not run under qemu-mipsel'; exit 1; }
set -e

echo '3. /init, /usb-rescue and the initramfs list.'
qemu-mipsel "$S/busybox" sh -n "$H/init"
qemu-mipsel "$S/busybox" sh -n "$P/usb-rescue"
cp "$H/init" "$S/init"
cp "$P/usb-rescue" "$S/usb-rescue"
# Real file times on purpose: the kernel build only re-packs the initramfs
# when a listed file is newer than its last archive.
chmod 0755 "$S/init" "$S/usb-rescue" "$S/busybox" "$S/nz-sdboot"
{
    for directory in /bin /dev /proc /sys /tmp /run /newroot; do printf 'dir %s 0755 0 0\n' "$directory"; done
    printf 'nod /dev/console 0600 0 0 c 5 1\nnod /dev/null 0666 0 0 c 1 3\n'
    printf 'file /bin/busybox %s 0755 0 0\n' "$S/busybox"
    printf 'file /bin/nz-sdboot %s 0755 0 0\n' "$S/nz-sdboot"
    printf 'file /init %s 0755 0 0\n' "$S/init"
    printf 'file /usb-rescue %s 0755 0 0\n' "$S/usb-rescue"
    while read -r applet; do
        test "$applet" = busybox || printf 'slink /bin/%s busybox 0777 0 0\n' "$applet"
    done < "$S/busybox-applets.txt"
} > "$S/initramfs.list"
sha256sum "$S/busybox" "$S/nz-sdboot" "$S/init" "$S/usb-rescue" > "$S/SHA256SUMS"
cat "$S/SHA256SUMS"
echo "SDROOT_INITRAMFS_LIST_READY: $S/initramfs.list"

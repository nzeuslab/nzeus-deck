#!/bin/bash
# Builds the boot RAM disk of the public NZEUS kernel (copy-and-go install).
#   bash ports/r36s-hichip/fatroot/build-initramfs.sh WORK_DIR
# Writes WORK_DIR/initramfs.list for CONFIG_INITRAMFS_SOURCE. PC only: never
# touches a card or the console.
# Contents: the same verified BusyBox as sdroot, nz-fatroot, a static e2fsck
# (e2fsprogs 1.47.1, kernel.org source, sha256-pinned), /init, /usb-rescue.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$H/.." && pwd)
S=${1:?usage: build-initramfs.sh WORK_DIR}
E2VER=1.47.1
E2TAR=/home/nzeus/r36s/downloads/e2fsprogs-$E2VER.tar.xz
E2URL=https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$E2VER/e2fsprogs-$E2VER.tar.xz
mkdir -p "$S"
S=$(cd "$S" && pwd)
export SOURCE_DATE_EPOCH=$(date -u -d '2026-11-18 00:00:00 UTC' +%s)

echo '1. BusyBox: the same verified build as sdroot (its own checks run).'
BB_TAR="$P/upstream/busybox-1.37.0.tar.bz2"            # not in git: fetched once, sha256-checked by sdroot
if [ ! -f "$BB_TAR" ]; then
    curl -fsSL --proto '=https' --tlsv1.2 -o "$BB_TAR.part" https://busybox.net/downloads/busybox-1.37.0.tar.bz2
    mv "$BB_TAR.part" "$BB_TAR"
fi
bash "$P/sdroot/build-initramfs.sh" "$S/base" > "$S/base.log" 2>&1 || { tail -20 "$S/base.log"; exit 1; }
cp "$S/base/busybox" "$S/base/busybox-applets.txt" "$S/"

echo '2. nz-fatroot: static MIPS helper.'
mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32 -Wall -Wextra -Werror \
    -o "$S/nz-fatroot" "$H/nz-fatroot.c"
if mipsel-linux-gnu-readelf -l "$S/nz-fatroot" | grep -q INTERP; then echo 'STOP: nz-fatroot is not static.'; exit 1; fi

echo '3. e2fsck: static MIPS build of e2fsprogs (download once, always sha256-checked).'
mkdir -p "$(dirname "$E2TAR")"
if [ ! -f "$E2TAR" ]; then
    curl -fsSL --proto '=https' --tlsv1.2 -o "$E2TAR.part" "$E2URL"
    mv "$E2TAR.part" "$E2TAR"
fi
(cd "$(dirname "$E2TAR")" && sha256sum -c "$P/upstream/e2fsprogs-$E2VER.tar.xz.sha256")
E2="$S/e2fsprogs-$E2VER"
[ -d "$E2" ] || tar -xJf "$E2TAR" -C "$S"
if [ ! -f "$E2/e2fsck/e2fsck.static" ]; then
    (cd "$E2" && ./configure --host=mipsel-linux-gnu --disable-nls --disable-fuse2fs --disable-defrag \
        --disable-imager --disable-debugfs --disable-resizer --without-libarchive \
        CFLAGS='-Os -march=mips32r2 -mabi=32' > "$S/e2fsprogs-configure.log" 2>&1)
    make -C "$E2" -j8 libs > "$S/e2fsprogs-build.log" 2>&1
    make -C "$E2/e2fsck" -j8 e2fsck.static >> "$S/e2fsprogs-build.log" 2>&1
fi
cp "$E2/e2fsck/e2fsck.static" "$S/e2fsck"
mipsel-linux-gnu-strip "$S/e2fsck"
if mipsel-linux-gnu-readelf -l "$S/e2fsck" | grep -q INTERP; then echo 'STOP: e2fsck is not static.'; exit 1; fi
qemu-mipsel "$S/e2fsck" -V 2>&1 | sed -n 1p     # (sed reads everything: no pipefail surprise)

echo '4. /init, /usb-rescue and the initramfs list.'
qemu-mipsel "$S/busybox" sh -n "$H/init"
qemu-mipsel "$S/busybox" sh -n "$P/usb-rescue"
cp "$H/init" "$S/init"
cp "$P/usb-rescue" "$S/usb-rescue"
chmod 0755 "$S/init" "$S/usb-rescue" "$S/busybox" "$S/nz-fatroot" "$S/e2fsck"
{
    for directory in /bin /dev /proc /sys /tmp /run /newroot /fat /data0; do printf 'dir %s 0755 0 0\n' "$directory"; done
    printf 'nod /dev/console 0600 0 0 c 5 1\nnod /dev/null 0666 0 0 c 1 3\n'
    printf 'file /bin/busybox %s 0755 0 0\n' "$S/busybox"
    printf 'file /bin/nz-fatroot %s 0755 0 0\n' "$S/nz-fatroot"
    printf 'file /bin/e2fsck %s 0755 0 0\n' "$S/e2fsck"
    printf 'file /init %s 0755 0 0\n' "$S/init"
    printf 'file /usb-rescue %s 0755 0 0\n' "$S/usb-rescue"
    while read -r applet; do
        test "$applet" = busybox || printf 'slink /bin/%s busybox 0777 0 0\n' "$applet"
    done < "$S/busybox-applets.txt"
} > "$S/initramfs.list"
sha256sum "$S/busybox" "$S/nz-fatroot" "$S/e2fsck" "$S/init" "$S/usb-rescue" > "$S/SHA256SUMS"
cat "$S/SHA256SUMS"
echo "FATROOT_INITRAMFS_LIST_READY: $S/initramfs.list"

#!/bin/bash
# make-image.sh ROOT_DIR OUT_IMAGE [SIZE]
# Turn a finished root folder into rootfs.ext4 for the NZEUS SD Linux area:
# ext4, 4 KiB blocks, label "nzroot", SIZE (default 4G = the nzroot partition)
# as a SPARSE file, made with mke2fs -d (no loop mount, no card).
# This is exactly what ports/r36s-hichip/rootfs/region-tool/nz_region.py
# "build --rootfs-ext4" takes (it may not be bigger than 4 GiB; the tool then
# sets the final UUID and checks it again).
#
# Run as root (so mke2fs -d copies the real owners: root, _apt, ...).
# Only writes OUT_IMAGE (+ OUT_IMAGE.info). Never touches a card or a device:
# OUT_IMAGE must be a new plain file path.
# Used by build-debian.sh, and by test-debian-pc.sh with a fake root folder.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${1:?usage: make-image.sh ROOT_DIR OUT_IMAGE [SIZE]}
OUT=${2:?usage: make-image.sh ROOT_DIR OUT_IMAGE [SIZE]}
SIZE=${3:-4G}

die() { echo "make-image: $*" >&2; exit 1; }
test "$(id -u)" = 0 || die "run as root (sudo), so file owners in the image are right"
test -d "$ROOT" || die "$ROOT is not a folder"
test -x "$ROOT/sbin/init" || test -L "$ROOT/sbin/init" || die "$ROOT/sbin/init is missing"
grep -q '^NZEUS_ROOTFS_LAYOUT=1$' "$ROOT/etc/nzeus-rootfs-release" 2>/dev/null ||
    die "$ROOT/etc/nzeus-rootfs-release must say NZEUS_ROOTFS_LAYOUT=1 (the RAM system checks it)"
case "$OUT" in /dev/*|/sys/*|/proc/*) die "refusing a device path: $OUT" ;; esac
test ! -e "$OUT" || die "$OUT already exists (move it away first)"
test ! -e "$OUT.tmp" || die "$OUT.tmp exists (left from a failed run? remove it)"
for t in mke2fs e2fsck tune2fs dumpe2fs sha256sum; do
    command -v "$t" > /dev/null || die "missing $t (sudo apt install e2fsprogs coreutils)"
done

ver=$(mke2fs -V 2>&1 | head -n 1)
echo "make-image: $ver"
truncate -s "$SIZE" "$OUT.tmp"
# lazy_*_init=0: every table is written now, so the console never zeroes the
# card in the background later. nodiscard: no TRIM on an image file.
MKE2FS_CONFIG=$HERE/mke2fs.conf mke2fs -q -F -t ext4 -b 4096 -L nzroot -m 2 \
    -E lazy_itable_init=0,lazy_journal_init=0,nodiscard,root_owner=0:0 \
    -d "$ROOT" "$OUT.tmp"
# no check by mount count or by date (no clock chip: dates are unreliable);
# on errors the kernel makes the root read-only instead of carrying on.
tune2fs -c 0 -i 0 -e remount-ro "$OUT.tmp" > /dev/null

set +e
e2fsck -fn "$OUT.tmp" > "$OUT.tmp.fsck" 2>&1
rc=$?
set -e
if test $rc != 0; then
    cat "$OUT.tmp.fsck" >&2
    die "e2fsck -fn found problems (rc=$rc) - image NOT made"
fi
rm -f "$OUT.tmp.fsck"

info=$(dumpe2fs -h "$OUT.tmp" 2>/dev/null)
field() { echo "$info" | sed -n "s/^$1:[[:space:]]*//p"; }
features=$(field 'Filesystem features')
for f in has_journal extent 64bit flex_bg metadata_csum dir_index; do
    case " $features " in *" $f "*) ;; *) die "feature $f missing ($features)" ;; esac
done
for f in metadata_csum_seed orphan_file orphan_present casefold encrypt verity inline_data; do
    case " $features " in *" $f "*) die "unwanted feature $f ($features)" ;; esac
done
test "$(field 'Filesystem volume name')" = nzroot || die "label is not nzroot"
test "$(field 'Block size')" = 4096 || die "block size is not 4096"
blocks=$(field 'Block count')
free=$(field 'Free blocks')
mv "$OUT.tmp" "$OUT"
sha=$(sha256sum "$OUT" | cut -d' ' -f1)
{
    echo "image=$(basename "$OUT")"
    echo "sha256=$sha"
    echo "bytes=$(stat -c %s "$OUT")"
    echo "allocated_bytes=$(($(stat -c %b "$OUT") * $(stat -c %B "$OUT")))"
    echo "used_bytes=$(((blocks - free) * 4096))"
    echo "label=nzroot"
    echo "uuid=$(field 'Filesystem UUID')"
    echo "features=$features"
    echo "inodes=$(field 'Inode count') free_inodes=$(field 'Free inodes')"
    echo "max_mount_count=$(field 'Maximum mount count') check_interval=$(field 'Check interval')"
    echo "errors=$(field 'Errors behavior')"
    echo "mke2fs=$ver"
} > "$OUT.info"
cat "$OUT.info"
echo "make-image: OK - $OUT (e2fsck -fn clean)"

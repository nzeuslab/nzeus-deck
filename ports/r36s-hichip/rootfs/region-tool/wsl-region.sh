#!/bin/bash
# WSL helper for ports/r36s-hichip/purelinux/session/sdroot-install.ps1. PC only: it
# never opens a disk, never flashes, uploads or reboots anything.
#   check [ROOTFS]  the root file system image and the tools exist (run before
#                   any card step, so a missing image is found early)
#   plan  [ROOTFS]  copy the root image to ~/nzeus-region/<UTC time>/, build the
#                   Linux-area candidate there (a 44 GiB SPARSE file: it must be
#                   on the WSL disk), then write the sector plan (delta.json +
#                   region-after.blocks) into ports/r36s-hichip/rootfs/card-region,
#                   which already holds the fresh backup and survey (D: junction)
# ROOTFS defaults to the Debian build: ~/nzeus-rootfs-debian/out/rootfs.ext4
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
CARD=$(cd "$T/.." && pwd)/card-region
MODE=${1:-}
ROOTFS=${2:-$HOME/nzeus-rootfs-debian/out/rootfs.ext4}
case "$ROOTFS" in "~/"*) ROOTFS="$HOME/${ROOTFS#\~/}" ;; esac
for tool in python3 mke2fs e2fsck resize2fs tune2fs dumpe2fs sha256sum; do
  command -v "$tool" >/dev/null || { echo "MISSING_TOOL $tool (sudo apt install e2fsprogs python3)"; exit 1; }
done
test -f "$ROOTFS" || { echo "NO_ROOTFS $ROOTFS (build the Debian root file system first)"; exit 1; }
case "$MODE" in
check)
  ls -l "$ROOTFS"
  dumpe2fs -h "$ROOTFS" 2>/dev/null | grep -E '^(Filesystem volume name|Block count|Free blocks|Block size):' \
    || { echo "NOT_EXT4 $ROOTFS"; exit 1; }
  echo WSL_REGION_CHECK_OK
  ;;
plan)
  test -f "$CARD/before.img" && test -f "$CARD/snapshot.json" && test -f "$CARD/region-survey.json" \
    || { echo "run the backup and the survey first ($CARD)"; exit 1; }
  test ! -e "$CARD/delta.json" || { echo "delta.json already exists in $CARD (plans are never overwritten)"; exit 1; }
  W=$HOME/nzeus-region/$(date -u +%Y%m%dT%H%M%SZ)
  mkdir -p "$W"
  cp --sparse=always "$ROOTFS" "$W/rootfs.ext4"
  sha256sum "$W/rootfs.ext4" | tee "$W/rootfs.ext4.sha256"
  python3 "$T/nz_region.py" build --snapshot "$CARD/before.img" --rootfs-ext4 "$W/rootfs.ext4" --out "$W/build"
  python3 "$T/nz_region.py" plan --snapshot "$CARD/before.img" --snapshot-json "$CARD/snapshot.json" \
    --survey "$CARD" --build "$W/build" --out "$CARD"
  python3 "$T/nz_region.py" check "$CARD"
  cp "$W/build/build.json" "$W/build/nzroot-initramfs.conf" "$CARD/"
  echo "REGION_PLAN_READY (candidate kept in $W/build)"
  ;;
*)
  echo "usage: $0 check|plan [rootfs.ext4]"
  exit 2
  ;;
esac

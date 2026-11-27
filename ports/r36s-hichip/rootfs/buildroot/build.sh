#!/bin/bash
# Build the NZEUS SD-root userspace with Buildroot (in WSL Ubuntu).
# It only builds files on the PC: it never writes a card, uploads to the
# console or reboots anything (lab rule 8).
#
#   wsl -d Ubuntu -- bash ports/r36s-hichip/rootfs/buildroot/build.sh [WORK_DIR]
#
# WORK_DIR defaults to ~/nzeus-rootfs-br and must be on the WSL (Linux) disk,
# not under /mnt/c or /mnt/d (Windows folders are slow and case-insensitive,
# Buildroot fails there). Needs about 8 GB free and 1-2 hours the first time.
# Host packages (once): sudo apt install build-essential cpio rsync bc unzip file wget python3 git
#
# Output: WORK_DIR/out/images/rootfs.tar, rootfs.ext4 and
# nzeus-rootfs-manifest.txt. The manifest (only) is copied to
# ports/r36s-hichip/rootfs/buildroot/manifest/ so it can be committed.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HOME/nzeus-rootfs-br}
# Buildroot 2025.02.x LTS branch as tested in the cloud on 2026-10-09 (2025.02.18+).
BR_URL=https://gitlab.com/buildroot.org/buildroot.git
BR_COMMIT=393f0196a3d356dad8e736e7db09cf066fb044e9
case "$WORK" in /mnt/*) echo "WORK_DIR must be on the Linux disk, not $WORK"; exit 1 ;; esac
mkdir -p "$WORK"
if ! test -d "$WORK/buildroot/.git"; then
    git clone --depth 1 -b 2025.02.x "$BR_URL" "$WORK/buildroot"
fi
if test "$(git -C "$WORK/buildroot" rev-parse HEAD)" != "$BR_COMMIT"; then
    git -C "$WORK/buildroot" fetch --depth 1 origin "$BR_COMMIT"
    git -C "$WORK/buildroot" checkout --detach "$BR_COMMIT"
fi
make -C "$WORK/buildroot" BR2_EXTERNAL="$HERE" O="$WORK/out" nzeus_r36s_defconfig
# Every line of our defconfig must have survived (a typo would silently vanish).
missing=0
for key in $(grep -E '^BR2_[A-Za-z0-9_]+=' "$HERE/configs/nzeus_r36s_defconfig" | cut -d= -f1); do
    grep -q "^$key=" "$WORK/out/.config" || { echo "MISSING from .config: $key"; missing=1; }
done
test $missing = 0
make -C "$WORK/out"
mkdir -p "$HERE/manifest"
cp "$WORK/out/images/nzeus-rootfs-manifest.txt" "$HERE/manifest/"
ls -la "$WORK/out/images"
echo "BUILD_COMPLETE (nothing was written to any card)"

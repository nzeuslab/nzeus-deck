#!/bin/bash
# Builds the PUBLIC NZEUS kernel (copy-and-go install). PC only: never touches
# a card or the console.
#   bash linux-port/kernel-public/build.sh WORK_DIR
# Result: WORK_DIR/nzeus-kernel.elf (package it with package.py).
#
# Same kernel as my daily one (linux-hichip at the pinned commit + the
# NZEUS port patches and drivers + the FPU/clock patch, the daily kernel's own
# settings), with two differences only:
#   - the boot RAM disk is linux-port/fatroot (starts NZEUS from files on FAT32)
#   - the version name is -nzeus-public
# Env: NZ_KERNEL_TREE = a git clone of linux-hichip holding the pinned commit (only read).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$H/.." && pwd)
W=${1:?usage: build.sh WORK_DIR}
K=${NZ_KERNEL_TREE:-/home/nzeus/r36s-linux-port-20261007}
PIN=da5f1a22ef2ff5c46aa3f750fa10825813970b09
JOBS=${NZEUS_BUILD_JOBS:-16}
git -C "$K" cat-file -e "$PIN^{commit}" || { echo "STOP: $K does not hold linux-hichip $PIN"; exit 1; }
mkdir -p "$W"
W=$(cd "$W" && pwd)
S=$W/linux
O=$W/build
MAKE=(make -C "$S" O="$O" ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- RUSTC=false)

echo "1. pinned kernel source (a local clone: the tree in $K is only read)"
if [ ! -d "$S/.git" ]; then
    git clone -q --shared --no-checkout "$K" "$S"
    git -C "$S" checkout -q --detach "$PIN"
fi
test "$(git -C "$S" rev-parse HEAD)" = "$PIN"
echo '2. NZEUS port patches, drivers and the FPU/clock patch (all from this repo)'
python3 "$P/apply-port-patches.py" "$S"
for d in display avp-rpc power ram usbhost/kernel audio; do python3 "$P/$d/install-source.py" "$S"; done
grep -q nzeus_fpudiag_early "$S/arch/mips/generic/init.c" || (cd "$S" && patch -s -p1 < "$H/0001-nzeus-fpudiag.patch")
cmp "$H/nzeus-fpudiag.c" "$S/arch/mips/generic/nzeus-fpudiag.c"

echo '3. boot RAM disk (linux-port/fatroot)'
bash "$P/fatroot/build-initramfs.sh" "$W/initramfs" > "$W/initramfs.log" 2>&1 || { tail -20 "$W/initramfs.log"; exit 1; }
tail -1 "$W/initramfs.log"

echo '4. settings: the daily kernel.config, only the RAM disk and the version name changed'
mkdir -p "$O"
cp "$H/kernel.config" "$O/.config"
"$S/scripts/config" --file "$O/.config" --set-str LOCALVERSION '-nzeus-public' \
    --set-str INITRAMFS_SOURCE "$W/initramfs/initramfs.list"
"${MAKE[@]}" olddefconfig > "$W/olddefconfig.log" 2>&1
# (diff exits 1 when the files differ - expected here, so it may not stop the script)
changed=$({ diff <(grep -v '^#' "$H/kernel.config" | sort) <(grep -v '^#' "$O/.config" | sort) || true; } \
    | grep '^[<>]' | sed 's/^[<>] //; s/=.*//' | sort -u | tr '\n' ' ')
test "$changed" = "CONFIG_INITRAMFS_SOURCE CONFIG_LOCALVERSION " || { echo "STOP: unexpected settings changed: $changed"; exit 1; }

echo "5. vmlinux ($(date +%T))"
export KBUILD_BUILD_USER=nzeus KBUILD_BUILD_HOST=nzeus-deck
export KBUILD_BUILD_TIMESTAMP='2026-11-18 00:00:00 UTC'
"${MAKE[@]}" -j"$JOBS" vmlinux > "$W/kernel-build.log" 2>&1 || { tail -30 "$W/kernel-build.log"; exit 1; }
cp "$O/vmlinux" "$W/nzeus-kernel.elf"
strings "$W/nzeus-kernel.elf" | grep '^Linux version' | sed -n 1p
sha256sum "$W/nzeus-kernel.elf"
echo "PUBLIC_KERNEL_BUILT: $W/nzeus-kernel.elf (not packaged, not installed)"

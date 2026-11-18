#!/bin/bash
# All PC tests of the SD-root boot pieces (no card, no console, no USB):
#   1. nz-sdboot against fake /proc,/sys,/dev trees (real nz_region.py headers)
#   2. /init with the MIPS boot BusyBox, every failure path -> RAM rescue
#   3. /usb-rescue name guard admits gma-native-sdroot
# Usage (WSL, after build-sdroot.sh or build-initramfs.sh):
#   bash linux-port/sdroot/test-all.sh [WORK_DIR]
# WORK_DIR defaults to the PC build folder; it must hold busybox and nz-sdboot.
set -u
H=$(cd "$(dirname "$0")" && pwd)
S=${1:-/home/nzeus/r36s-linux-port-work-20261007/sdroot}
T=$(mktemp -d /tmp/nzeus-sdroot-test.XXXXXX)
OUT=${NZ_TEST_REPORTS:-$T}
# The CURRENT boot BusyBox (no switch_root), from the gma-native-usbfix initramfs.
(cd "$T" && zcat "$H/../display/bootlog/build-usbfix/initramfs.cpio.gz" | cpio -i -d --quiet bin/busybox)
fail=0
python3 "$H/test-nz-sdboot.py" --binary "$S/nz-sdboot" --busybox "$S/busybox" \
    --report "$OUT/nz-sdboot-test.json" || fail=1
python3 "$H/test-sdroot-init.py" --busybox "$S/busybox" --nz-sdboot "$S/nz-sdboot" \
    --old-busybox "$T/bin/busybox" --report "$OUT/init-test.json" || fail=1
python3 "$H/test-usb-admission.py" --busybox "$S/busybox" --report "$OUT/usb-admission-test.json" || fail=1
test "$OUT" = "$T" || rm -rf "$T"
if test "$fail" = 0; then echo 'SDROOT_PC_TESTS_PASSED (PC/qemu only; nothing on hardware)'; else echo 'SDROOT_PC_TESTS_FAILED'; fi
exit "$fail"

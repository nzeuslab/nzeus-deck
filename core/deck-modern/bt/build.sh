#!/bin/bash
# Builds nzeus-bt, NZEUS Deck's Bluetooth program for the SD Debian, and tests it.
#   nzeus-bt-mipsel         the console program (MIPS32r2, static, stripped),
#                           recorded in SHA256SUMS; the system image and the
#                           updater install it as deck-bin/nzeus-bt.bin
#   $B/nzeus-bt-fake-x86    PC test build: talks to tests/fake_kernel.py instead of the kernel
#   $B/nzeus-bt-fake-mips   the same for MIPS, run under qemu-mipsel in the tests
#   $B/unit-x86, unit-mips  unit tests of the pure parts (addresses, EIR, store, SDP)
# Then runs tests/run_tests.sh (unit + flow tests). Nothing is copied to a console.
# Run from WSL (the R36S folder):  bash core/deck-modern/bt/build.sh
#   NZBT_BUILD=DIR   where the building happens (default ~/r36s/build/nzeus-bt-modern)
#   NZBT_OUT=FILE    write the console program somewhere else (a check build:
#                    SHA256SUMS is then left alone)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
B=${NZBT_BUILD:-~/r36s/build/nzeus-bt-modern}
OUT=${NZBT_OUT:-$HERE/nzeus-bt-mipsel}
T=~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
MCC=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
MSTRIP=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-strip
SYSROOT=$T/mipsel-buildroot-linux-gnu/sysroot
ARCH="-mips32r2 -march=mips32r2 -mtune=74kc -mfp32 -mhard-float -EL --sysroot=$SYSROOT"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-format-truncation"   # (long device names are cut on purpose)
SRC="main.c util.c eir.c store.c mgmt.c sdp.c hid.c hogp.c io.c"
LIBSRC="util.c eir.c store.c sdp.c"
mkdir -p "$B"

# copy sources to the Linux disk first (paths with spaces + faster); the
# compiler always sees the same paths, so the program is the same byte for byte
rm -rf "$B/src" && mkdir -p "$B/src" && cp "$HERE"/src/*.c "$HERE"/src/*.h "$B/src/" && cp "$HERE/tests/unit_tests.c" "$B/src/"
cd "$B/src"

$MCC $ARCH -Os $WARN -Werror -static -o "$B/nzeus-bt-mips" $SRC
$MSTRIP "$B/nzeus-bt-mips"
# test builds (fake kernel); -g so a crash in the tests is easy to find
gcc -O1 -g $WARN -Werror -DNZBT_FAKE -fsanitize=address,undefined -o "$B/nzeus-bt-fake-x86" $SRC
$MCC $ARCH -O1 $WARN -Werror -static -DNZBT_FAKE -o "$B/nzeus-bt-fake-mips" $SRC
gcc -O1 -g $WARN -Werror -fsanitize=address,undefined -I. -o "$B/unit-x86" unit_tests.c $LIBSRC
$MCC $ARCH -O1 $WARN -Werror -static -I. -o "$B/unit-mips" unit_tests.c $LIBSRC

NZBT_BUILD="$B" bash "$HERE/tests/run_tests.sh"

cp "$B/nzeus-bt-mips" "$OUT"
if test -z "${NZBT_OUT:-}"; then
    (cd "$HERE" && sha256sum nzeus-bt-mipsel > SHA256SUMS)
fi
ls -la "$OUT"
$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-readelf -h "$OUT" | grep -E "Machine|Flags" | sed 's/^ */   /'
sha256sum "$OUT"
echo "NZEUS_BT_BUILD_DONE (PC only: needs a console test with the dongle)"

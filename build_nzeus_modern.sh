#!/bin/bash
# Builds nzeus-modern: NZEUS Deck for the console's Linux 7.2 (the SD Debian),
# as ONE static program (apps/frontend-modern/nzeus-modern.c + the NZEUS core
# from apps/nzeus-gui). See apps/frontend-modern/README.md.
#
#   bash build_nzeus_modern.sh         this PC (WSL), the buildroot toolchain the
#                                      other console builds use
#                                      -> apps/frontend-modern/mips/nzeus-modern
#   CC=mipsel-linux-gnu-gcc bash build_nzeus_modern.sh
#                                      Ubuntu's cross compiler (the cloud box): a
#                                      compile check / qemu test build
#                                      -> apps/frontend-modern/mips/nzeus-modern-cloud
#   ... build_nzeus_modern.sh test     also: the hw_modern.c test on the PC (gcc), and the
#                                      self-test under qemu-mipsel, which saves the home
#                                      screen as reports/gui/modern-home.ppm (+ .png when
#                                      python3 has PIL). PC side only: proves nothing
#                                      about the console.
#
# LVGL: official LVGL 8.3.1 in ~/r36s/src/lvgl (setup_lvgl.sh), or LVGL=/path/to/lvgl.
# Sources are copied to ~/r36s/build/nzeus-modern first, because the space in
# "nzeus lab" breaks compiler include paths.
# Like every build script here: it only makes the file. It never uploads,
# flashes or reboots anything.
set -e
WIN="$(cd "$(dirname "$0")" && pwd)"
L=${LVGL:-$HOME/r36s/src/lvgl}
NZ_BUILD=${NZ_BUILD:-$HOME/r36s/build}         # another build folder: NZ_BUILD=/path
S=$NZ_BUILD/nzeus-modern/src
[ -f "$L/lvgl.h" ] || { echo "LVGL not found in $L (set LVGL=/path/to/lvgl-8.3.1)"; exit 1; }

if [ -z "$CC" ]; then
    T=$HOME/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
    CC=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
    # -mfp32 + the SDK's own C library, like the other console builds
    EXTRA="-mfp32 --sysroot=$T/mipsel-buildroot-linux-gnu/sysroot"
    NAME=nzeus-modern
    TAG=buildroot
else
    EXTRA=""
    NAME=nzeus-modern-cloud
    TAG=$(basename "$CC")
fi
STRIP=${CC%gcc}strip                                  # (the same tool family as the compiler)
READELF=${CC%gcc}readelf
SIZE=${CC%gcc}size
B=$NZ_BUILD/nzeus-modern/$TAG
OUT="$WIN/apps/frontend-modern/mips/$NAME"
# The console's CPU: MIPS 74Kc, little-endian, MIPS32r2 with a hardware FPU.
# Per-function sections: unused LVGL code is dropped at link time.
CFLAGS="-O2 -march=mips32r2 -mtune=74kc -mhard-float -EL -ffunction-sections -fdata-sections $EXTRA"
# NZ_SDCARD: NZEUS's own folder (nzeus-deck links it to /data/nzeus/deck).
DEFS='-DNZ_SDCARD="/tmp/nzeus-data"'

mkdir -p $S/gui $S/front $B/lvgl $B/third "$WIN/apps/frontend-modern/mips"
rsync -a --delete "$WIN/apps/nzeus-gui/" $S/gui/
rsync -a --delete "$WIN/apps/frontend-modern/" $S/front/
INC="-I$S/gui -I$S/gui/vterm/include -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"
echo "compiler: $CC ($($CC -dumpversion))"

# 1) LVGL itself (only files that changed since the last build are recompiled)
compile_one() {
    f=$1
    o=$B/lvgl/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$f" ] && [ "$o" -nt "$S/gui/lv_conf.h" ] && return 0
    $CC $CFLAGS -w $INC -c "$f" -o "$o"
}
export -f compile_one; export CC CFLAGS INC B L S
find $L/src -name '*.c' | xargs -P "$(nproc)" -n 1 bash -c 'compile_one "$0"'
echo "LVGL: $(ls $B/lvgl/*.o | wc -l) objects"

# 2) other people's libraries (libvterm): compiled once, without our warning checks
. $S/gui/sources.sh                                   # UI_FILES, CONSOLE_FILES, THIRD_FILES
for f in $THIRD_FILES; do
    o=$B/third/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$S/gui/$f" ] || $CC $CFLAGS -w $INC -c "$S/gui/$f" -o "$o"
done

# 3) the NZEUS core + the frontend, linked into one static program
CORE_SRC="$(for f in $CONSOLE_FILES $UI_FILES; do printf '%s ' "$S/gui/$f"; done)"
$CC $CFLAGS $DEFS -Wall $INC -static -Wl,--gc-sections -o $B/$NAME \
    $S/front/nzeus-modern.c $CORE_SRC $B/third/*.o $B/lvgl/*.o -lm
$STRIP $B/$NAME
cp $B/$NAME "$OUT"
ls -la "$OUT"
$READELF -h "$OUT" | grep -E "Class|Machine|Flags|Type"
if $READELF -l "$OUT" | grep -q interpreter; then
    echo "NOT STATIC (it asks for a program interpreter) - stop"; exit 1
fi
echo "static: yes (no shared libraries needed)"
# memory: code + data + zeroed data (bss) when it runs (the new kernel has ~39 MB in all)
$SIZE "$OUT" | awk 'NR==2 { printf "memory: code %d KB, data %d KB, bss %d KB, total %d KB\n", $1/1024, $2/1024, $3/1024, $4/1024 }'
# linux-port/diagnostics/rescue-upload.py takes at most 4 MiB
[ $(stat -c %s "$OUT") -le $((4 * 1024 * 1024)) ] || { echo "BIGGER THAN 4 MiB (the rescue upload limit) - stop"; exit 1; }

# 4) optional: the self-test under qemu (PC side only - it proves nothing about the console)
if [ "$1" = test ]; then
    bash "$WIN/apps/nzeus-gui/tests/test_obd_identity.sh"
    echo "== hw_modern.c on the PC (fake battery / charger / backlight files)"
    gcc -O1 -Wall -Wextra -DNZ_TESTING '-DNZ_SDCARD="/tmp/nzhwtest"' -I$S/gui \
        -o $NZ_BUILD/nzeus-modern/test_hw_modern $S/front/tests/test_hw_modern.c $S/gui/hw.c $S/gui/hw_modern.c
    $NZ_BUILD/nzeus-modern/test_hw_modern
    echo "== ALSA partial writes and recovery (fake PCM device)"
    gcc -O1 -Wall -Wextra -DNZ_TESTING '-DNZ_SDCARD="/tmp/nzhwtest"' -I$S/gui \
        -Wl,--wrap=open,--wrap=close,--wrap=ioctl -o $NZ_BUILD/nzeus-modern/test_pcm \
        $S/front/tests/test_pcm.c $S/gui/hw.c $S/gui/hw_modern.c
    $NZ_BUILD/nzeus-modern/test_pcm
    PPM="$WIN/reports/gui/modern-home.ppm"
    mkdir -p "$WIN/reports/gui"
    echo "== self-test under qemu-mipsel (no screen, no buttons: NZEUS in memory)"
    qemu-mipsel "$OUT" --selftest "$PPM"
    python3 -c "
import sys
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
Image.open(sys.argv[1]).save(sys.argv[1][:-4] + '.png')
print('picture:', sys.argv[1][:-4] + '.png')" "$PPM"
fi

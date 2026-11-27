#!/bin/bash
# The NZEUS GUI's PC tests (the console program itself is build_nzeus_modern.sh).
#   bash build_nzeus_gui.sh pc               -> the theme previews (reports/gui/t*_*.ppm)
#                                               + the full regression (fake frontend)
#   bash build_nzeus_gui.sh pc tests         -> the full regression only (no theme previews)
#   bash build_nzeus_gui.sh pc pdf-reader    -> only the PDF reader's tests
#   bash build_nzeus_gui.sh pc car-recovery  -> only the Car app's recovery tests
# The same core as on the console (core.c, hw.c, hw_modern.c ...), built with gcc:
# the console's files and helper scripts are fakes in /tmp (see tests/fake_frontend.c).
# Sources are copied to ~/r36s/build/nzeus-gui/src first, because the space
# in "nzeus lab" breaks compiler include paths.
set -e
MODE=${1:-pc}
[ "$MODE" = pc ] || { echo "usage: bash build_nzeus_gui.sh pc [tests|pdf-reader|car-recovery]"
                      echo "(the console build is: bash build_nzeus_modern.sh)"; exit 2; }
WIN="$(cd "$(dirname "$0")" && pwd)"
L=~/r36s/src/lvgl
# NZ_BUILD: another build folder (e.g. when two people build at the same time)
NZ_BUILD=${NZ_BUILD:-$HOME/r36s/build}
S=$NZ_BUILD/nzeus-gui/src
B=$NZ_BUILD/nzeus-gui/$MODE
mkdir -p $S $B/lvgl
rsync -a --delete "$WIN/apps/nzeus-gui/" $S/

bash "$WIN/apps/nzeus-gui/tests/test_obd_identity.sh"
CC=gcc
CFLAGS="-O2 -g -Wall -DNZ_TESTING"           # NZ_TESTING: never reboot/power off the PC
# LV_KCONFIG_IGNORE: HiChip's copy of LVGL reads its settings from their own
# build system; this official LVGL switch makes it use our lv_conf.h instead.
INC="-I$S -I$S/vterm/include -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"

# 1) LVGL itself (only files that changed since the last build are recompiled)
compile_one() {
    f=$1
    o=$B/lvgl/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$f" ] && [ "$o" -nt "$S/lv_conf.h" ] && return 0
    $CC $CFLAGS -w $INC -c "$f" -o "$o"
}
export -f compile_one; export CC CFLAGS INC B L S
find $L/src -name '*.c' | xargs -P 32 -n 1 bash -c 'compile_one "$0"'
echo "LVGL: $(ls $B/lvgl/*.o | wc -l) objects"

# 2) our files
. $S/sources.sh                                   # UI_FILES, CONSOLE_FILES, THIRD_FILES
# the built-in pictures must match their art/ files (after changing one: python3 apps/nzeus-gui/art/make_*_data.py)
while read -r art data; do
    want=$(sha256sum "$WIN/apps/nzeus-gui/$art" | cut -d' ' -f1)
    grep -q "sha256 $want" "$WIN/apps/nzeus-gui/$data" || { echo "FAIL  $data does not match $art: run its art/make_*_data.py"; exit 1; }
done <<'ART'
art/hill.jpg wallpaper_hill_data.c
art/retro-icons.png retroicons_data.c
ART
echo "PASS  built-in pictures match art/hill.jpg and art/retro-icons.png"
# other people's libraries (libvterm, libmad): compiled once, without our warning checks
mkdir -p $B/third
for f in $THIRD_FILES; do
    o=$B/third/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$S/$f" ] || $CC $CFLAGS -w $INC -c "$S/$f" -o "$o"
done
UI_SRC=$(for f in $UI_FILES; do printf '%s ' "$S/$f"; done)
CORE_SRC="$(for f in $CONSOLE_FILES; do printf '%s ' "$S/$f"; done)$UI_SRC"
OUT="$WIN/reports/gui"
mkdir -p "$OUT"

if [ "${2:-}" = claude-chain ]; then
    $CC $CFLAGS $INC -DNZ_SDCARD='"/tmp/nzchain_sd"' -o $B/claude_chain \
        $S/tests/claude_chain.c $UI_SRC $B/third/*.o $B/lvgl/*.o -lm
    exit 0
fi

# 3) the theme previews: every screen in every theme (pc_preview.c)
$CC $CFLAGS $INC -DNZ_SDCARD='"/tmp/nz_preview_sd"' -o $B/pc_preview $S/pc_preview.c $UI_SRC $B/third/*.o $B/lvgl/*.o -lm
# a small fake NZEUS folder for the pictures
P=/tmp/nz_preview_sd
rm -rf $P && mkdir -p $P/nzeus $P/docs $P/music $P/pictures $P/projects
cp $S/tests/sample.py $P/projects/hello.py
mkdir -p $P/nzeus/python
cp $S/tests/sample.py $P/nzeus/python/my_robot.py
rm -rf /tmp/nz_preview_pyex && mkdir -p /tmp/nz_preview_pyex   # (the system's examples on the console)
PYEX="$WIN/core/deck-modern/python-examples"            # (their home since 2026-10-13;
[ -d "$PYEX" ] || PYEX="$WIN/apps/python/examples"           #  before that: apps/python/examples)
cp "$PYEX"/*.py /tmp/nz_preview_pyex/
mkdir -p $P/web $P/nzeus/notes && cp $S/tests/pics/tb.png $S/tests/pics/big.jpg $P/pictures/
printf 'Shopping list\nresistors 220R, 10k\nLEDs\n' > $P/nzeus/notes/shopping-list-2026-10-04.txt
printf 'Project ideas\nweather station, new case\n' > $P/nzeus/notes/project-ideas-2026-10-03.txt
python3 -c "
import wave, math
w = wave.open('$P/music/sunrise.wav', 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b''.join(int(8000*math.sin(i*440*2*math.pi/44100)).to_bytes(2,'little',signed=True)*2 for i in range(44100*8)))
w.close()"
# a pretend Wi-Fi dongle (switched on) and a scan result for the Wi-Fi app's picture
Y=/tmp/nz_preview_sys
rm -rf $Y && mkdir -p $Y/sys/bus/usb/devices/1-1 $Y/sys/class/net/wlan0
echo 0bda > $Y/sys/bus/usb/devices/1-1/idVendor; echo c820 > $Y/sys/bus/usb/devices/1-1/idProduct
echo 0x1003 > $Y/sys/class/net/wlan0/flags
cp $S/tests/preview_survey.txt /tmp/nz_preview_survey.txt
echo "Shopping: resistors, LEDs, solder" > $P/docs/notes.txt
cp $S/tests/sample.py /tmp/sample.py            # (a copy: the preview must not change the original)
if [ -z "${2:-}" ]; then $B/pc_preview "$OUT" /tmp/sample.py; fi

# 4) the core, driven by a fake frontend with fake buttons and fake devices
$CC $CFLAGS $INC -DNZ_SDCARD='"/tmp/fake_sdcard"' "-DNZ_REPO=\"$WIN\"" -o $B/fake_frontend \
    $S/tests/fake_frontend.c $CORE_SRC $B/third/*.o $B/lvgl/*.o -lm
if [ "${2:-}" = pdf-reader ]; then
    NZ_PDF_READER_ONLY=1 $B/fake_frontend "$OUT" $S/tests/pics
    exit 0
fi
if [ "${2:-}" = car-recovery ]; then
    NZ_CAR_RECOVERY_ONLY=1 $B/fake_frontend "$OUT" $S/tests/pics
    exit 0
fi
$B/fake_frontend "$OUT" $S/tests/pics
# the Claude app on the real SSH chain (run by R36S/brain/test_modern_chain.sh)
$CC $CFLAGS $INC -DNZ_SDCARD='"/tmp/nzchain_sd"' -o $B/claude_chain \
    $S/tests/claude_chain.c $UI_SRC $B/third/*.o $B/lvgl/*.o -lm

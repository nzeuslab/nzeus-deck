#!/bin/bash
# Builds and tests the Electronics app (apps/nzeus-gui/app_electronics.c + elec_calc.c).
#   1. unit checks of every elec_calc function + the app driven with fake buttons
#   2. pictures of every tool in 3 themes -> reports/gui/elec_*.png
#   3. mutants: deliberate mistakes in elec_calc.c must make checks FAIL
#   4. the console (MIPS) build of our two files, with zero warnings
# Run it from Windows like this (WSL mangles inline commands):
#   cd "<R36S folder>" && wsl -d Ubuntu -- bash build_electronics_test.sh
# Everything is built in ~/r36s/build/elec-test, with its own copy of LVGL
# (~/r36s/build/nzeus-gui is rebuilt by others all the time).
set -e
WIN="$(cd "$(dirname "$0")" && pwd)"
A="$WIN/apps/nzeus-gui"
L=~/r36s/src/lvgl
B=~/r36s/build/elec-test
S=$B/src
OURS="app_electronics.c elec_calc.c"
mkdir -p $S $B/lvgl/obj $B/pc $B/third $B/mips $B/shots $B/mut

# 1) copy the sources (the space in "nzeus lab" breaks include paths; nothing there is changed)
rsync -a --delete --exclude build "$A/" $S/
cmp -s $S/lv_conf.h $B/lvgl/lv_conf.h || cp $S/lv_conf.h $B/lvgl/lv_conf.h
# -isystem for LVGL: its own headers' warnings are not ours to fix
INC="-I$B/lvgl -I$S -I$S/vterm/include -isystem $L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"
DEFS="-DNZ_TESTING -DNZ_SDCARD=\"/tmp/elec_sd\""

# 2) LVGL for the PC, our own objects (only files that changed are recompiled)
compile_lvgl() {
    f=$1
    o=$B/lvgl/obj/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$f" ] && [ "$o" -nt "$B/lvgl/lv_conf.h" ] && return 0
    gcc -O2 -g -w $INC -c "$f" -o "$o"
}
export -f compile_lvgl; export INC B L S
find $L/src -name '*.c' | xargs -P 32 -n 1 bash -c 'compile_lvgl "$0"'
echo "LVGL (PC): $(ls $B/lvgl/obj/*.o | wc -l) objects"

# 3) the other NZEUS screens (not ours: compiled quietly), from sources.sh
. $S/sources.sh                                   # UI_FILES, THIRD_FILES
for f in $THIRD_FILES; do                         # other people's libraries (libvterm)
    o=$B/third/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$S/$f" ] || gcc -O2 -g -w $INC -c "$S/$f" -o "$o"
done
OTHERS=""
for f in $UI_FILES; do case " $OURS " in *" $f "*) ;; *) OTHERS="$OTHERS $f";; esac; done
rm -f $B/pc/*.o
compile_other() { gcc -O2 -g -w $INC $DEFS -c $S/$1 -o $B/pc/${1%.c}.o; }
export -f compile_other; export DEFS
echo $OTHERS | xargs -P 32 -n 1 bash -c 'compile_other "$0"'

# 4) our files and the test, with all warnings on (ours must have none)
build_test() {   # $1 = the elec_calc.c to use, $2 = the program to make
    : > $B/warnings.txt
    gcc -O2 -g -Wall -Wextra $INC $DEFS -c $1 -o $B/elec_calc_tmp.o 2>>$B/warnings.txt || { cat $B/warnings.txt; return 1; }
    gcc -O2 -g -Wall -Wextra $INC $DEFS -c $S/app_electronics.c -o $B/app_electronics.o 2>>$B/warnings.txt || { cat $B/warnings.txt; return 1; }
    gcc -O2 -g -Wall -Wextra $INC $DEFS -c $S/tests/test_electronics.c -o $B/test_electronics.o 2>>$B/warnings.txt || { cat $B/warnings.txt; return 1; }
    gcc -o $2 $B/test_electronics.o $B/app_electronics.o $B/elec_calc_tmp.o $B/pc/*.o $(ls $B/third/*.o 2>/dev/null) $B/lvgl/obj/*.o -lm
}
build_test $S/elec_calc.c $B/test_electronics
if [ -s $B/warnings.txt ]; then echo "WARNINGS in our code (PC):"; cat $B/warnings.txt; exit 1; fi
echo "PC build: no warnings in app_electronics.c / elec_calc.c / test_electronics.c"

# 5) run: checks + pictures (a small fake SD card for settings.ini)
rm -rf /tmp/elec_sd && mkdir -p /tmp/elec_sd/nzeus
rm -f $B/shots/*.ppm
set +e
$B/test_electronics $B/shots > $B/result.txt 2>&1
RESULT=$?
set -e
cat $B/result.txt
OUT="$WIN/reports/gui"
mkdir -p "$OUT"
rm -f "$OUT"/elec_*.png                          # (only our own old pictures)
python3 - "$B/shots" "$OUT" <<'PY'
# PPM -> PNG with only the Python standard library
import sys, os, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
n = 0
for name in sorted(os.listdir(src)):
    if not name.endswith('.ppm'): continue
    data = open(os.path.join(src, name), 'rb').read()
    head, pix = data.split(b'\n', 1)
    _, w, h, _ = head.split()
    w, h = int(w), int(h)
    raw = b''.join(b'\0' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
          chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(os.path.join(dst, name[:-4] + '.png'), 'wb').write(png)
    n += 1
print('pictures:', n, 'PNG files in reports/gui/elec_*.png')
PY
echo "test_electronics exit code: $RESULT (= number of failed checks)"

# 6) mutants: deliberate mistakes in a copy of elec_calc.c; each must make a check FAIL
mutant() {   # $1 = name, $2 = sed expression
    cp $S/elec_calc.c $B/mut/elec_calc.c
    sed -i "$2" $B/mut/elec_calc.c
    if cmp -s $S/elec_calc.c $B/mut/elec_calc.c; then echo "MUTANT $1: sed did not change anything (BROKEN)"; return; fi
    build_test $B/mut/elec_calc.c $B/mut/test_electronics || { echo "MUTANT $1: does not compile"; return; }
    out=$($B/mut/test_electronics --checks-only 2>&1) || true
    n=$(echo "$out" | grep -c '^FAIL') || true
    if [ "$n" -gt 0 ]; then echo "MUTANT $1: caught ($n checks FAIL)"; echo "$out" | grep '^FAIL' | head -4 | sed 's/^/      /'
    else echo "MUTANT $1: NOT caught"; fi
}
echo "--- mutants"
mutant "555: 0.693 typed as 0.639"           's/o->t_high = 0.693 \* (r1 + r2) \* c;/o->t_high = 0.639 * (r1 + r2) * c;/'
mutant "gold tolerance 10 % instead of 5 %"  's/case EC_GOLD:   return 5;/case EC_GOLD:   return 10;/'
mutant "LED: supply + LED voltage"           's/double drop = supply - vf;/double drop = supply + vf;/'
mutant "E12 series: 4.7 written as 4.3"      's/3.3, 3.9, 4.7, 5.6, 6.8, 8.2 };/3.3, 3.9, 4.3, 5.6, 6.8, 8.2 };/'
mutant "Blue Pill: PB5 marked 5 V tolerant"  's/{ "B5",  "NOT 5V tol.", 0 },/{ "B5",  "NOT 5V tol.", FT },/'
mutant "format: 2 digits instead of 3"       's/int dec = m >= 100 ? 0 : m >= 10 ? 1 : 2;/int dec = m >= 100 ? 0 : m >= 10 ? 0 : 1;/'
# put the real test program back (build_test overwrote the shared objects)
build_test $S/elec_calc.c $B/test_electronics >/dev/null

# 7) the console (MIPS) build: our two files must compile with zero warnings
T=~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
MCC=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
# the console program's flags (build_nzeus_modern.sh: MIPS 74Kc, hardware FPU, one static program)
MFLAGS="-O2 -march=mips32r2 -mtune=74kc -mhard-float -EL -ffunction-sections -fdata-sections -mfp32 \
        --sysroot=$T/mipsel-buildroot-linux-gnu/sysroot"
echo "--- MIPS build"
: > $B/mips/warnings.txt
for f in $OURS; do
    $MCC $MFLAGS -Wall -Wextra $INC -DNZ_SDCARD=\"/tmp/nzeus-data\" -c $S/$f -o $B/mips/${f%.c}.o 2>>$B/mips/warnings.txt
done
if [ -s $B/mips/warnings.txt ]; then echo "WARNINGS in our code (MIPS):"; cat $B/mips/warnings.txt; exit 1; fi
echo "MIPS build: app_electronics.c and elec_calc.c have no warnings"
$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-size $B/mips/*.o
exit $RESULT

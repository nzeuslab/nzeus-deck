#!/bin/bash
# Builds and runs the tests for NZEUS's four tool apps (Task Manager,
# Calculator, Notes, Clock): apps/nzeus-gui/tests/test_tools.c
#   bash build_tools_test.sh          everything: checks, memory, pictures, MIPS build, mutants
#   bash build_tools_test.sh quick    only the checks (no pictures, no mutants)
# Run it from Windows like this (WSL):
#   cd C:\path\to\nzeus\R36S && wsl -d Ubuntu -- bash build_tools_test.sh
#
# It uses its OWN build folder, ~/r36s/build/tools-test, with its own copy of
# the sources and its own LVGL objects, so it never disturbs the main NZEUS
# build (~/r36s/build/nzeus-gui). Pictures go to reports/gui/tools_*.png.
set -e
MODE=${1:-full}
WIN="$(cd "$(dirname "$0")" && pwd)"
L=~/r36s/src/lvgl
B=~/r36s/build/tools-test
S=$B/src
mkdir -p $S $B/lvgl $B/obj $B/shots $B/mips $B/mut
# a copy of the sources: the space in "nzeus lab" breaks compiler include paths
rsync -a --delete --exclude build "$WIN/apps/nzeus-gui/" $S/

CC=gcc
CFLAGS="-O2 -g -DNZ_TESTING -DNZ_SDCARD=\"/tmp/tools_sd\""
INC="-I$S -I$S/vterm/include -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"

# 1) LVGL, with our copy of lv_conf.h (only files that changed are recompiled)
compile_lvgl() {
    f=$1
    o=$B/lvgl/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$f" ] && [ "$o" -nt "$S/lv_conf.h" ] && return 0
    $CC -O2 -w $INC -c "$f" -o "$o"
}
export -f compile_lvgl; export CC INC B L S
find $L/src -name '*.c' | xargs -P 32 -n 1 bash -c 'compile_lvgl "$0"'
echo "LVGL: $(ls $B/lvgl/*.o | wc -l) objects"

# 2) NZEUS's files (warnings hidden: not ours) + our files (every warning shown)
MINE="app_taskmgr.c procs.c app_calculator.c calc_eval.c app_notes.c app_clock.c devtools.c app_devtools.c health.c app_health.c wifiinfo.c btinfo.c"
. $S/sources.sh                                    # UI_FILES (other NZEUS files), THIRD_FILES (libvterm)
OTHERS=""
for f in $UI_FILES $THIRD_FILES; do case " $MINE " in *" $f "*) ;; *) OTHERS="$OTHERS $f";; esac; done
obj_of() { echo $B/obj/$(echo ${1%.c} | tr '/' '_').o; }   # vterm/src/screen.c -> obj/vterm_src_screen.o
compile_other() { $CC $CFLAGS -w $INC -c $S/$1 -o $(obj_of $1); }
export -f compile_other obj_of; export CFLAGS
echo $OTHERS | tr ' ' '\n' | xargs -P 32 -n 1 bash -c 'compile_other "$0"'
rm -f $B/warnings.txt
for f in $MINE tests/test_tools.c; do
    $CC $CFLAGS -Wall -Wextra $INC -c $S/$f -o $(obj_of $f) 2>>$B/warnings.txt ||
        { cat $B/warnings.txt; echo "COMPILE FAILED: $f"; exit 1; }
done
echo "warnings in our files (gcc -Wall -Wextra): $(grep -c 'warning:' $B/warnings.txt || true)"
cat $B/warnings.txt
OBJS=""
for f in $OTHERS $MINE tests/test_tools.c; do OBJS="$OBJS $(obj_of $f)"; done
$CC -o $B/test_tools $OBJS $B/lvgl/*.o -lm

# 3) run it: checks, memory, pictures
rm -f $B/shots/*.ppm
set +e
if [ "$MODE" = quick ]; then NO_PICTURES=1 $B/test_tools $B/shots; else $B/test_tools $B/shots; fi
RESULT=$?
set -e
echo "test_tools exit code: $RESULT (= number of failed checks)"
[ "$MODE" = quick ] && exit $RESULT

# 4) pictures: PPM -> PNG with Windows Python + Pillow, into reports/gui
cat > $B/ppm2png.py <<'PY'
import sys, os
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for name in sorted(os.listdir(src)):
    if name.endswith('.ppm'):
        Image.open(os.path.join(src, name)).save(os.path.join(dst, name[:-4] + '.png'))
print('pictures converted')
PY
mkdir -p "$WIN/reports/gui"
"/mnt/c/Program Files/Python313/python.exe" "$(wslpath -w $B/ppm2png.py)" "$(wslpath -w $B/shots)" "$(wslpath -w "$WIN/reports/gui")"
ls "$WIN/reports/gui" | grep -c '^tools_' | sed 's/^/tools pictures: /'

# 5) the console build of our files: the MIPS cross compiler, same flags as build_nzeus_modern.sh
T=~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
MCC=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
# the console program's flags (build_nzeus_modern.sh: MIPS 74Kc, hardware FPU, one static program)
MFLAGS="-O2 -march=mips32r2 -mtune=74kc -mhard-float -EL -ffunction-sections -fdata-sections -mfp32 \
        --sysroot=$T/mipsel-buildroot-linux-gnu/sysroot"
rm -f $B/mips/warnings.txt
for f in $MINE; do
    $MCC $MFLAGS -Wall -Wextra $INC -c $S/$f -o $B/mips/${f%.c}.o 2>>$B/mips/warnings.txt ||
        { cat $B/mips/warnings.txt; echo "MIPS COMPILE FAILED: $f"; exit 1; }
done
echo "MIPS warnings in our files: $(grep -c 'warning:' $B/mips/warnings.txt || true)"
cat $B/mips/warnings.txt
$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-size $B/mips/*.o

# 6) mutants: a deliberate mistake in a copy of one file; the checks must FAIL
mutant() {   # $1 = file, $2 = description, $3 = exact text, $4 = replacement
    d=$B/mut/$(echo "$2" | tr -c 'a-zA-Z0-9' '_')
    rm -rf $d && mkdir -p $d
    cp $S/$1 $d/$1
    python3 -c 'import sys; p,a,b=sys.argv[1:4]; s=open(p).read(); n=s.count(a); open(p,"w").write(s.replace(a,b,1)); sys.exit(0 if n==1 else 1)' \
        $d/$1 "$3" "$4" || { echo "MUTANT [$2]: text not found exactly once (BROKEN MUTANT)"; return; }
    $CC $CFLAGS -w $INC -c $d/$1 -o $d/${1%.c}.o || { echo "MUTANT [$2]: does not compile"; return; }
    objs=$(echo $OBJS | sed "s#$B/obj/${1%.c}.o#$d/${1%.c}.o#")
    $CC -o $d/test_tools $objs $B/lvgl/*.o -lm
    n=$($d/test_tools --unit 2>&1 | tee $d/out.txt | grep -c '^FAIL' || true)
    if [ "$n" -gt 0 ]; then echo "MUTANT [$2]: caught by $n failing check(s), e.g. $(grep -m1 '^FAIL' $d/out.txt)"
    else echo "MUTANT [$2]: SURVIVED (the tests missed it)"; fi
}
echo "--- mutants ---"
mutant calc_eval.c "calc: no divide-by-zero check" \
    'if (r == 0) { fail(p, "Can'"'"'t divide by zero"); return 0; }' ''
mutant calc_eval.c "calc: unary minus ignored" \
    "return c == '-' ? -v : v;" "return v;"
mutant procs.c "procs: NZEUS's parents not protected" \
    'if (in_my_family(p->pid, self_pid)) return' 'if (0) return'
mutant app_notes.c "notes: dots and slashes kept in file names" \
    "if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {" \
    "if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '/') {"
# the Dev toolbox (devtools.c, 2026-10-07)
mutant devtools.c "devtools: Base64 without its = padding" \
    "i + 2 < len ? b64[v & 63] : '='" "b64[v & 63]"
mutant devtools.c "devtools: CRC-32 with the wrong polynomial" "0xEDB88320u" "0x04C11DB7u"
mutant devtools.c "devtools: binary not grouped in fours" \
    'if (i && i % 4 == 0) put(out, n, &k, " ");' ';'
mutant devtools.c "devtools: + in a URL stays a +" "*p == '+' ? ' ' : *p" "*p"
mutant devtools.c "devtools: JSON indented by 4" "j->depth * 2" "j->depth * 4"
mutant devtools.c "devtools: milliseconds read as seconds" "v /= 1000; ms = 1;" "ms = 1;"
mutant devtools.c "devtools: a missing hex digit is not noticed" \
    "if (half >= 0) return bad(" "if (0) return bad("
# the Health dashboard (health.c, 2026-10-07)
mutant health.c "health: 2 MB free counts as only getting low" \
    "h->mem_avail_kb < 3 * 1024" "h->mem_avail_kb < 1 * 1024"
mutant health.c "health: an almost-empty battery worries even while charging" \
    "!h->charging && h->battery <= 10" "h->battery <= 10"
mutant health.c "health: zram worry only above 95%" \
    "h->swap_used_kb * 100 > h->swap_total_kb * 85" "h->swap_used_kb * 100 > h->swap_total_kb * 95"
mutant health.c "health: millidegrees not turned into degrees" \
    "milli >= 1000 ? milli / 1000 : milli" "milli"
# Wi-Fi ID cards (wifiinfo.c, 2026-10-07; checked on the console's real scans)
mutant wifiinfo.c "wifi: [WEP] counted before WPA2 (real WPA2 networks look unsafe)" \
    'else if (strstr(flags, "WPA2") || strstr(flags, "RSN"))' 'else if (strstr(flags, "WEP")) { g = 1; snprintf(out, n, "WEP - unsafe, broken in minutes"); } else if (strstr(flags, "WPA2") || strstr(flags, "RSN"))'
mutant wifiinfo.c "wifi: one router on two bands taken for two routers" \
    "int last_ok = abs((int)x[5] - (int)y[5]) <= 16;" "int last_ok = x[5] == y[5];"
mutant wifiinfo.c "wifi: the local bit makes a different router" \
    "int first_ok = (x[0] | 0x02) == (y[0] | 0x02);" "int first_ok = x[0] == y[0];"
mutant wifiinfo.c "wifi: your own network counted for the best channel" \
    "if (mine && nets[i].ssid[0] && !strcmp(nets[i].ssid, mine)) continue;" ";"
mutant wifiinfo.c "wifi: a strong network weighs the same as a weak one" \
    "score[c] += loud > 0 ? loud : 1;" "score[c] += 1;"
# Bluetooth ID cards (btinfo.c, 2026-10-07)
mutant btinfo.c "bt: Apple's model code read from the wrong bytes" \
    "a->model = data[1] << 8 | data[2];" "a->model = data[0] << 8 | data[1];"
mutant btinfo.c "bt: Find My adverts not seen as trackers" "else if (type == 0x12) a->findmy = 1;" "else if (0) a->findmy = 1;"
mutant btinfo.c "bt: Tile not seen as a tracker" \
    "if (has_uuid(d, 0xFEED) || has_uuid(d, 0xFEEC) || has_uuid(d, 0xFD5A)) return 1;" "if (has_uuid(d, 0xFD5A)) return 1;"
mutant btinfo.c "bt: private and fixed random addresses mixed up" "if (top == 3) return" "if (top == 1) return"
mutant btinfo.c "bt: the device class read from the wrong bits" "unsigned m = (cod >> 8) & 0x1f;" "unsigned m = (cod >> 10) & 0x1f;"
mutant btinfo.c "bt: unknown companies get a made-up name" \
    'else if (d->has_mfr) ADD("Maker     company #%04X (not in the built-in list)\n", d->company);' \
    'else if (d->has_mfr) ADD("Maker     Apple (from its maker code)\n");'
exit $RESULT

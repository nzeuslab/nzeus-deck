#!/bin/bash
# Builds and tests the terminal widget (apps/nzeus-gui/vtwidget.c + libvterm).
#   bash build_vt_test.sh                  -> PC test: checks + screenshots, mutants, MIPS build
#   bash build_vt_test.sh --install-claude -> also installs the official Claude Code npm
#                                             package into ~/r36s/tools/claude-cli (if npm exists)
# Run it from Windows like this (WSL mangles inline commands):
#   cd "<R36S folder>" && wsl -d Ubuntu -- bash build_vt_test.sh
# Everything is built in ~/r36s/build/vt-test (the space in "nzeus lab" breaks
# include paths). Screenshots go to reports/gui/vt_*.png.
set -e
WIN="$(cd "$(dirname "$0")" && pwd)"
A="$WIN/apps/nzeus-gui"
L=~/r36s/src/lvgl
B=~/r36s/build/vt-test
S=$B/src
CLAUDE_DIR=~/r36s/tools/claude-cli
mkdir -p $S $B/lvgl $B/pc $B/mips $B/shots $B/mut

# 1) copy only the files we need (nothing in apps/nzeus-gui is changed)
rm -rf $S/vterm
cp -r "$A/vterm" $S/vterm
cp "$A/vtwidget.c" "$A/vtwidget.h" "$A/font_mono16.c" "$A/usbkbd.h" "$A/lv_conf.h" "$A/nzeus_tick.h" $S/
cp "$A/tests/vt_test.c" $S/
VT_SRC=$(ls $S/vterm/src/*.c)

# 2) LVGL for the PC: our own objects (only changed files are recompiled)
INC="-I$S -I$S/vterm/include -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"
compile_lvgl() {
    f=$1
    o=$B/lvgl/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/')
    [ "$o" -nt "$f" ] && [ "$o" -nt "$S/lv_conf.h" ] && return 0
    gcc -O2 -g -w $INC -c "$f" -o "$o"
}
export -f compile_lvgl; export INC B L S
find $L/src -name '*.c' | xargs -P 32 -n 1 bash -c 'compile_lvgl "$0"'
echo "LVGL (PC): $(ls $B/lvgl/*.o | wc -l) objects"

# 3) libvterm + font for the PC
for f in $VT_SRC; do gcc -O2 -g -std=c99 -Wall -I$S/vterm/include -c $f -o $B/pc/vterm_$(basename ${f%.c}).o; done
gcc -O2 -g -w $INC -c $S/font_mono16.c -o $B/pc/font_mono16.o

# 4) the widget and the test, with all warnings on (ours must have none)
build_test() {   # $1 = vtwidget.c to use, $2 = output program
    gcc -O2 -g -std=gnu99 -Wall -Wextra $INC -c $1 -o $B/pc/vtwidget_tmp.o 2>$B/pc/warnings.txt || { cat $B/pc/warnings.txt; return 1; }
    gcc -O2 -g -std=gnu99 -Wall -Wextra $INC -c $S/vt_test.c -o $B/pc/vt_test.o 2>>$B/pc/warnings.txt
    gcc -o $2 $B/pc/vt_test.o $B/pc/vtwidget_tmp.o $B/pc/vterm_*.o $B/pc/font_mono16.o $B/lvgl/*.o -lm
}
build_test $S/vtwidget.c $B/vt_test
if [ -s $B/pc/warnings.txt ]; then echo "WARNINGS in our code (PC):"; cat $B/pc/warnings.txt; exit 1; fi
echo "PC build: no warnings in vtwidget.c / vt_test.c"

# 5) optional: the official Claude Code package, locally (no sudo, no global install)
if [ "$1" = "--install-claude" ] && command -v npm >/dev/null && [ ! -e $CLAUDE_DIR/node_modules/.bin/claude ]; then
    mkdir -p $CLAUDE_DIR
    npm install --prefix $CLAUDE_DIR @anthropic-ai/claude-code
fi
CLAUDE_ARG=""
if [ -e $CLAUDE_DIR/node_modules/.bin/claude ]; then
    CLAUDE_ARG="--claude $CLAUDE_DIR/node_modules/.bin/claude"
    rm -rf /tmp/claude-fake-home && mkdir -p /tmp/claude-fake-home     # empty: no login can exist
    echo "Claude Code: $(cat $CLAUDE_DIR/node_modules/@anthropic-ai/claude-code/package.json | grep '"version"')"
else
    echo "Claude Code not installed: test 4 skipped"
fi

# 6) run: checks + screenshots
rm -f $B/shots/*.ppm
set +e
$B/vt_test $B/shots $CLAUDE_ARG
RESULT=$?
rm -rf /tmp/claude-fake-home                     # what Claude Code wrote there (no login)
set -e
mkdir -p "$WIN/reports/gui"
python3 - "$B/shots" "$WIN/reports/gui" <<'PY'
# PPM -> PNG with only the Python standard library
import sys, os, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
for name in sorted(os.listdir(src)):
    if not name.endswith('.ppm'): continue
    data = open(os.path.join(src, name), 'rb').read()
    head, pix = data.split(b'\n', 1)
    _, w, h, _ = head.split()
    w, h = int(w), int(h)
    raw = b''.join(b'\0' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
          chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    out = os.path.join(dst, name[:-4] + '.png')
    open(out, 'wb').write(png)
    print('png', out)
PY
echo "vt_test exit code: $RESULT (= number of failed checks)"

# 7) mutants: deliberate mistakes in a copy of vtwidget.c; each must make a check FAIL
mutant() {   # $1 = name, $2 = sed expression
    cp $S/vtwidget.c $B/mut/vtwidget.c
    sed -i "$2" $B/mut/vtwidget.c
    if cmp -s $S/vtwidget.c $B/mut/vtwidget.c; then echo "MUTANT $1: sed did not change anything (BROKEN)"; return; fi
    build_test $B/mut/vtwidget.c $B/mut/vt_test || { echo "MUTANT $1: does not compile"; return; }
    out=$($B/mut/vt_test $B/mut --checks-only 2>&1) || true
    n=$(echo "$out" | grep -c '^FAIL') || true
    if [ "$n" -gt 0 ]; then echo "MUTANT $1: caught ($n checks FAIL)"; echo "$out" | grep '^FAIL' | sed 's/^/      /'
    else echo "MUTANT $1: NOT caught"; fi
}
echo "--- mutants"
mutant "colour cube red/blue swapped" 's/level\[i \/ 36\], level\[(i \/ 6) % 6\], level\[i % 6\]/level[i % 6], level[(i \/ 6) % 6], level[i \/ 36]/'
mutant "box drawing: down arm missing" 's/if (D == 1 || D == 2) {/if (0) {/'
mutant "damage: whole widget redrawn" 's/if (too_many) invalidate(t, &all);/if (1) lv_obj_invalidate(t->obj);/'
mutant "rows: font not shifted up 2 px" 's/#define FONT_TOP 2 /#define FONT_TOP 0 /'
mutant "rows: no air around the letters" 's/#define PAD ((CH - GH) \/ 2)/#define PAD 0/'
mutant "box lines: down arm stops at the letters" 's/fill(p, x + 3, g + start, x + ((D == 2) ? 4 : 3), y + CH - 1, fg);/fill(p, x + 3, g + start, x + ((D == 2) ? 4 : 3), g + 15, fg);/'
mutant "replies to the program lost" 's/t->outq_len += len;/t->outq_len += 0;/'
mutant "pipe mode: Backspace ignored" 's/if (t->line_len > 0) { t->line_len--;/if (0) { t->line_len--;/'
mutant "UTF-8 split characters not kept back" 's/if (c >= 0xc0 && utf8_len(c) > k) keep = k;/if (0) keep = k;/'
# scrollback (kept lines, 2026-10-06)
mutant "scrollback: lines that go off the top are not kept" 's/\.sb_pushline = sb_pushline,/.sb_pushline = NULL,/'
mutant "scrollback: the view ignores how far back it is" 's/int line = r - t->sb_offset; /int line = r; /'
mutant "scrollback: no line limit (5000)" 's/#define SB_MAX_LINES 500$/#define SB_MAX_LINES 5000/'
mutant "scrollback: no byte limit" 's/t->sb_bytes + sb_size(l) > SB_MAX_BYTES/0/'
mutant "scrollback: blanks at the end are kept too" 's/if (!sb_blank(t, &v)) len = c + 1;/len = c + 1;/'
mutant "scrollback: background colours not kept" 's/runs\[k\]\.bg = v\.bg;/runs[k].bg = t->bg;/'
mutant "scrollback: new output moves the view while reading back" 's/if (t->sb_offset > 0 \&\& t->sb_offset < t->sb_count) t->sb_offset++;/;/'
mutant "scrollback: new output redraws the view while reading back" 's/    if (t->sb_offset) return;                     \/\* scrolled back/    if (0) return;  \/\* scrolled back/'
mutant "scrollback: the alternate screen scrolls" 's/if (!t || t->altscreen) return 0; /if (!t) return 0; /'
mutant "scrollback: a bigger screen does not take lines back" 's/\.sb_popline = sb_popline,/.sb_popline = NULL,/'

# 8) the console (MIPS) build: libvterm + widget must compile; our code with zero warnings
T=~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot
MCC=$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-gcc
# the console program's flags (build_nzeus_modern.sh: MIPS 74Kc, hardware FPU, one static program)
MFLAGS="-O2 -march=mips32r2 -mtune=74kc -mhard-float -EL -ffunction-sections -fdata-sections -mfp32 \
        --sysroot=$T/mipsel-buildroot-linux-gnu/sysroot"
echo "--- MIPS build"
# (libvterm is other people's code: compiled without our warning checks, as in build_nzeus_modern.sh)
for f in $VT_SRC; do $MCC $MFLAGS -std=c99 -w -I$S/vterm/include -c $f -o $B/mips/vterm_$(basename ${f%.c}).o; done
$MCC $MFLAGS -std=gnu99 -Wall -Wextra $INC -c $S/vtwidget.c -o $B/mips/vtwidget.o 2>$B/mips/warnings.txt
if [ -s $B/mips/warnings.txt ]; then echo "WARNINGS in vtwidget.c (MIPS):"; cat $B/mips/warnings.txt; exit 1; fi
echo "MIPS build: vtwidget.c has no warnings"
$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-size $B/mips/*.o
$T/opt/ext-toolchain/bin/mips-mti-linux-gnu-size -t $B/mips/*.o | tail -1
exit $RESULT

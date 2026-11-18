#!/bin/bash
# Debug build of the PC preview: LVGL and our code both compiled with -O0 -g,
# frame pointers and AddressSanitizer, so a crash shows the whole call chain.
S=~/r36s/build/nzeus-gui/src
B=~/r36s/build/nzeus-gui/pcdbg
L=~/r36s/src/lvgl
mkdir -p $B/lvgl
INC="-I$S -I$L -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE"
CF="-O0 -g -fno-omit-frame-pointer -fsanitize=address"
compile_one() { f=$1; o=$B/lvgl/$(echo "${f#$L/}" | tr '/' '_' | sed 's/\.c$/.o/'); [ "$o" -nt "$f" ] && return 0; gcc $CF -w $INC -c "$f" -o "$o"; }
export -f compile_one; export CF INC B L S
find $L/src -name '*.c' | xargs -P 32 -n 1 bash -c 'compile_one "$0"'
gcc $CF $INC -o $B/pc_preview $S/pc_preview.c $S/home.c $S/display.c $B/lvgl/*.o
mkdir -p /tmp/gui
$B/pc_preview /tmp/gui 2>&1 | grep -E "^ +#[0-9]+|ERROR|saved" | head -20

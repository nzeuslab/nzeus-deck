#!/bin/bash
# Builds and runs the BMP safety test (apps/nzeus-gui/tests/test_picture_bmp.c)
# on the PC, with AddressSanitizer: any read outside a picture file stops it.
# Run it from Windows like this (WSL):
#   cd C:\path\to\nzeus-deck && wsl -d Ubuntu -- bash build_picture_test.sh
# Needs: gcc and the LVGL source from setup_lvgl.sh (~/r36s/src/lvgl).
set -e
WIN="$(cd "$(dirname "$0")" && pwd)"
L=~/r36s/src/lvgl
B=~/r36s/build/picture-test
mkdir -p "$B"
# a copy of the sources: a space in the folder name breaks compiler include paths
rsync -a --delete --exclude build "$WIN/apps/nzeus-gui/" "$B/src/"
S=$B/src
gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$S" -I"$L" -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE \
    -o "$B/test_picture_bmp" \
    "$S/tests/test_picture_bmp.c" "$S/picture.c" \
    "$L/src/extra/libs/png/lodepng.c" "$L/src/extra/libs/sjpg/tjpgd.c"
"$B/test_picture_bmp"

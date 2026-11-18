#!/bin/bash
# Gets LVGL 8.3.1 (MIT licence) into the build area ~/r36s/src/lvgl.
# Downloads the official release from github.com/lvgl/lvgl (tag v8.3.1).
# Only the library's own sources are kept (no demos or examples).
set -e
DST=~/r36s/src/lvgl
TMP=$(mktemp -d)
git clone --quiet --depth 1 --branch v8.3.1 https://github.com/lvgl/lvgl.git "$TMP/lvgl"
rm -rf "$DST" && mkdir -p "$DST"
cp -a "$TMP/lvgl/src" "$TMP/lvgl/lvgl.h" "$TMP/lvgl/LICENCE.txt" "$DST/"
[ -f "$TMP/lvgl/lv_conf_template.h" ] && cp "$TMP/lvgl/lv_conf_template.h" "$DST/"
rm -rf "$TMP"
grep -E "define LVGL_VERSION_(MAJOR|MINOR|PATCH)" "$DST/lvgl.h"
echo "C files: $(find "$DST/src" -name '*.c' | wc -l)"
du -sh "$DST"

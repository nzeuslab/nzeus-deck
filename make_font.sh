#!/bin/bash
# Makes NZEUS's pixel fonts from LVGL's unscii 8x8 font (see tools/make_mono_font.py)
cd "$(dirname "$0")/apps/nzeus-gui"   # works from any folder
U=~/r36s/src/lvgl/src/font/lv_font_unscii_8.c
python3 tools/make_mono_font.py $U font_mono16.c 1 2 lv_font_nzeus_mono_16 18 lv_font_montserrat_16
python3 tools/make_mono_font.py $U font_mono24.c 3 3 lv_font_nzeus_mono_24 28 lv_font_montserrat_20

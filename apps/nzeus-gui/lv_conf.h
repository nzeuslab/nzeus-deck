/* LVGL settings for NZEUS Linux.
 * Only the settings that differ from LVGL's defaults are listed here;
 * LVGL fills in everything else by itself (lv_conf_internal.h). */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16                 /* the console's screen is 16-bit (RGB565) */
#define LV_COLOR_16_SWAP 0
/* LVGL takes memory from the normal C allocator (malloc), not a fixed pool:
 * the picture decoders need a few MB for a moment, the rest of the time the
 * memory goes back to Linux. */
#define LV_MEM_CUSTOM 1
#define LV_DISP_DEF_REFR_PERIOD 16        /* redraw at most ~60 times a second */
#define LV_INDEV_DEF_READ_PERIOD 10       /* read buttons/pointer every frame, so short clicks aren't missed */

/* time for animations and timers comes from our own millisecond clock */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "nzeus_tick.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (nzeus_tick_ms())

#define LV_DPI_DEF 130

/* fonts (they also contain LVGL's built-in symbols: Wi-Fi, battery, folder...)
 * Only the sizes theme.c uses: 12 small, 16 body, 20 headings, 40 big.
 * 14 is on by LVGL's default, so it is switched off here; 28 was never used.
 * (2026-10-06: dropped to make room in the console's 3 MB, see the memory check) */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16
/* (the Terminal's monospace font is our own font_mono16.c) */

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_LOG 0

/* LVGL widgets NZEUS never uses (LVGL switches them all on by default; no
 * source or test creates them - checked with grep 2026-10-06). Off = less code
 * in the console's 3 MB. Still on: label, img, btn, bar, arc, spinner, chart,
 * canvas, textarea, keyboard (+ its btnmatrix), win, imgfont, flex, grid. */
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_LINE       0
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TABLE      0
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
/* only LVGL's default theme is used (theme.c builds on it) */
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO  0

/* pictures: files are read with normal C file functions ("S:/tmp/nzeus-data/x.png"),
 * PNG, BMP and JPG decoders are built in, and the last picture stays decoded */
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 0
#define LV_USE_PNG 1
#define LV_USE_BMP 1
#define LV_USE_SJPG 1
/* emoji: a font whose letters are small pictures on the SD card (emoji.c) */
#define LV_USE_IMGFONT 1

#endif

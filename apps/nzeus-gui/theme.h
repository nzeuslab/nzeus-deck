#ifndef NZEUS_THEME_H
#define NZEUS_THEME_H
#include <stdint.h>
#include "lvgl.h"

/* NZEUS themes.
 *
 * Every colour, font and corner radius on every NZEUS screen comes from the
 * theme in use (its "tokens"). Screens don't pick colours themselves: they
 * use the shared styles below (th_add, th_label...). Changing the theme
 * changes those styles, and LVGL redraws everything with the new look, live,
 * without closing any app. The choice is saved in settings.ini. */

enum { TH_ORIGINAL, TH_GREEN_CRT, TH_AMBER_CRT, TH_DAYLIGHT, TH_MIDNIGHT, TH_RETRO_BLUE, TH_COUNT };

typedef struct {
    const char *id;              /* saved in settings.ini, e.g. "green_crt" */
    const char *name;            /* shown in Settings, e.g. "Green CRT" */
    int dark;                    /* light text on a dark background */
    /* colours, 0xRRGGBB */
    uint32_t bg;                 /* screen background */
    uint32_t bar;                /* status bar, taskbar, title bars */
    uint32_t surface;            /* tiles, windows, pop-ups */
    uint32_t surface_hi;         /* selected / pressed surface */
    uint32_t border;             /* thin lines around boxes */
    uint32_t accent;             /* NZEUS green: selection, icons, titles */
    uint32_t on_accent;          /* text on an accent background */
    uint32_t text;
    uint32_t muted;              /* hints, secondary text */
    uint32_t danger;             /* delete / close buttons */
    uint32_t on_danger;
    uint32_t watermark;          /* the quiet "NZEUS" on the desktop */
    uint32_t term_bg, term_fg;   /* Terminal */
    uint32_t ed_bg, ed_line, ed_gutter, ed_cursor;   /* Editor */
    uint32_t syntax[12];         /* Editor code colours, by HL_* (highlight.h) */
    int radius;                  /* corner roundness of tiles, windows, pop-ups */
    int crt;                     /* 1 = old monitor look: scanlines + glow */
    int retro;                   /* 1 = the classic blue desktop look (desktop.c: blue taskbar,
                                    green start button, blue window title bars) */
    const lv_font_t *font_small; /* hints */
    const lv_font_t *font_body;  /* most text */
    const lv_font_t *font_heading;
    const lv_font_t *font_big;   /* clock, calculator display, wallpaper */
} NzTheme;

/* the 8 x 16 monospace font (Terminal, Editor, and the CRT themes' text) */
LV_FONT_DECLARE(lv_font_nzeus_mono_16);
LV_FONT_DECLARE(lv_font_nzeus_mono_24);

void theme_init(void);                   /* after lv_init(): loads the saved theme */
const NzTheme *theme(void);              /* the theme in use */
const NzTheme *theme_get(int index);     /* any theme (for the picker's previews) */
int  theme_index(void);
void theme_set(int index);               /* switch live, and save the choice */
void theme_on_change(void (*fn)(void));  /* fn runs after every theme change */
lv_color_t th_color(uint32_t rgb);       /* 0xRRGGBB -> LVGL colour */

/* Shared styles. Add them to objects instead of setting colours/fonts directly. */
enum {
    TS_SCREEN,       /* screen background + default text */
    TS_BAR,          /* status bar / title bar / taskbar background */
    TS_PANEL,        /* box with border and rounded corners (windows, pop-ups) */
    TS_TILE,         /* home-screen tile */
    TS_TILE_SEL,     /* the selected tile: accent frame (and glow on CRT) */
    TS_ROW_SEL,      /* the selected row in a list */
    TS_BTN,          /* normal button */
    TS_BTN_ACCENT,   /* main button (accent colour) */
    TS_BTN_DANGER,   /* delete / close */
    TS_TEXT,         /* body text */
    TS_SMALL,        /* small muted text (hints) */
    TS_MUTED,        /* body-size muted text */
    TS_TITLE,        /* titles: accent colour, body font */
    TS_HEADING,      /* section headings: bigger font */
    TS_BIG,          /* very big text in the accent colour */
    TS_ON_ACCENT,    /* text on an accent background */
    TS_ICON,         /* icon images: drawn in the accent colour */
    TS_ICON_TEXT,    /* icon images: drawn in the text colour */
    TS_WATERMARK,    /* quiet big text on the desktop */
    TS_MONO,         /* monospace text in the Terminal colours */
    TS_TERM,         /* Terminal background */
    TS_INPUT,        /* text boxes */
    TS_KEYCAP,       /* a button name in a hint, like a small key cap */
    TS_MONO_ACCENT,  /* monospace text in the accent colour (Terminal prompt) */
    TS_ARC,          /* a gauge's background ring */
    TS_ARC_IND,      /* a gauge's value ring */
    TS_COUNT
};
lv_style_t *th_style(int s);
void th_add(lv_obj_t *o, int s);                           /* to the main part */
void th_add_to(lv_obj_t *o, int s, lv_style_selector_t sel); /* e.g. LV_STATE_CHECKED */
lv_obj_t *th_label(lv_obj_t *parent, const char *text, int s);
lv_obj_t *th_box(lv_obj_t *parent, int s);                 /* a plain object with one style */

#endif

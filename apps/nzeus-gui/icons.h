/* icons.h - the NZEUS OS icon set. MADE BY tools/make_icons.py - do not edit by hand.
 *
 * Clean outline icons in three sizes: 48 px (home tiles), 32 px (desktop icons)
 * and 20 px (start menu, lists, file rows). They are alpha-only pictures
 * (LV_IMG_CF_ALPHA_8BIT), so they take the colour of the style that draws them:
 *
 *     lv_obj_t *img = lv_img_create(parent);
 *     lv_img_set_src(img, icon_get(ICON_WIFI, 20));
 *     lv_obj_set_style_img_recolor(img, accent_colour, 0);
 *     lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
 */
#ifndef NZEUS_ICONS_H
#define NZEUS_ICONS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* the 12 home-screen apps, in screen order */
    ICON_TERMINAL, ICON_FILES, ICON_EDITOR, ICON_PYTHON,
    ICON_CAR, ICON_WEB, ICON_ELECTRONICS, ICON_ARDUINO,
    ICON_CLAUDE, ICON_MEDIA, ICON_WIFI, ICON_SETTINGS,
    /* everything else */
    ICON_TASKMGR, ICON_CALCULATOR, ICON_NOTES, ICON_CLOCK,
    ICON_SCREENSHOT, ICON_SYSTEM, ICON_ABOUT, ICON_HANDHELD,
    ICON_DESKTOP, ICON_FOLDER, ICON_FILE, ICON_FILE_TEXT,
    ICON_FILE_CODE, ICON_FILE_IMAGE, ICON_FILE_AUDIO, ICON_USB,
    ICON_IMAGE, ICON_MUSIC, ICON_THEME, ICON_POWER,
    ICON_KEYBOARD, ICON_SERIAL, ICON_GAUGE, ICON_LOCK,
    ICON_CHAT,
    ICON_COUNT
} NzIcon;

/* The icon `id` at `size` px: 48, 32 or 20 (any other size gives the nearest one).
 * Returns NULL for an unknown id. */
const lv_img_dsc_t *icon_get(NzIcon id, int size);

#ifdef __cplusplus
}
#endif

#endif

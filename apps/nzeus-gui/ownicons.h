#ifndef NZEUS_OWNICONS_H
#define NZEUS_OWNICONS_H
#include "lvgl.h"

/* ownicons.c - the owner's own small pictures for the desktop: the start
 * button's logo (nzeus/start-icon.bmp) and app icons (nzeus/icons/NAME.bmp).
 * See CUSTOM-ICONS.md. A .bmp or .png file; pure green (0x00FF00) pixels are
 * see-through (LVGL's "chroma key"). Pictures bigger than the space are made
 * smaller (never bigger). Missing or broken files: the built-in icon. */

typedef struct {
    lv_img_dsc_t dsc;        /* give &dsc to lv_img_set_src */
    uint16_t *px;            /* its pixels (NULL = nothing loaded) */
} OwnIcon;

/* Load NZ_DIR "/<base>.bmp" (or .png), made to fit max x max. base is e.g.
 * "start-icon" or "icons/files". 1 = loaded, 0 = no file / not a picture. */
int  own_icon_load(OwnIcon *ic, const char *base, int max);
/* Free it (nothing may still show it). Harmless on an empty one. */
void own_icon_free(OwnIcon *ic);

#endif

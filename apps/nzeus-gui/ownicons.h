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
    int builtin;             /* 1 = one of the Retro Blue theme's built-in pictures (px stays NULL) */
} OwnIcon;

/* Load NZ_DIR "/<base>.bmp" (or .png), made to fit max x max. base is e.g.
 * "start-icon" or "icons/files". 1 = loaded, 0 = no file / not a picture. */
int  own_icon_load(OwnIcon *ic, const char *base, int max);
/* The Retro Blue theme's own picture for the app called name ("files", ...;
 * art/retro-icons.png, built into the program), size 32 (desktop) or 20
 * (menus). 1 = there is one. Used when the owner has no file of their own. */
int  own_icon_builtin(OwnIcon *ic, const char *name, int size);
/* 1 = something is loaded (the owner's file or a built-in picture) */
static inline int own_icon_shown(const OwnIcon *ic) { return ic->px != NULL || ic->builtin; }
/* Free it (nothing may still show it). Harmless on an empty one. */
void own_icon_free(OwnIcon *ic);

#endif

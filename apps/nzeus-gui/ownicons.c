/* ownicons.c - the owner's own icons (see ownicons.h). */
#include <stdio.h>
#include <string.h>
#include "ownicons.h"
#include "picture.h"
#include "settings.h"

int own_icon_load(OwnIcon *ic, const char *base, int max)
{
    static const char *const kinds[] = { ".bmp", ".png" };
    memset(ic, 0, sizeof *ic);
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        char path[160], err[80];
        Picture p;
        snprintf(path, sizeof path, NZ_DIR "/%s%s", base, kinds[k]);
        if (picture_load(path, max, max, &p, err, sizeof err) != 0) continue;   /* none / broken: try the next */
        ic->px = p.px;                                       /* (the pixels are kept: freed by own_icon_free) */
        ic->dsc.header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;   /* pure green = see-through */
        ic->dsc.header.w = (uint32_t)p.w;
        ic->dsc.header.h = (uint32_t)p.h;
        ic->dsc.data_size = (uint32_t)p.w * p.h * sizeof(uint16_t);
        ic->dsc.data = (const uint8_t *)p.px;
        return 1;
    }
    return 0;
}

/* the built-in pictures, in retroicons_data.c */
extern const int nz_retro_icon_count;
extern const char *const nz_retro_icon_names[];
extern const uint16_t nz_retro_icons32[][32 * 32];
extern const uint16_t nz_retro_icons20[][20 * 20];

int own_icon_builtin(OwnIcon *ic, const char *name, int size)
{
    memset(ic, 0, sizeof *ic);
    if (!name || (size != 32 && size != 20)) return 0;
    for (int i = 0; i < nz_retro_icon_count; i++) {
        if (strcmp(name, nz_retro_icon_names[i])) continue;
        ic->builtin = 1;                                     /* (static pixels: nothing to free) */
        ic->dsc.header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;
        ic->dsc.header.w = (uint32_t)size;
        ic->dsc.header.h = (uint32_t)size;
        ic->dsc.data_size = (uint32_t)size * size * sizeof(uint16_t);
        ic->dsc.data = (const uint8_t *)(size == 32 ? nz_retro_icons32[i] : nz_retro_icons20[i]);
        return 1;
    }
    return 0;
}

void own_icon_free(OwnIcon *ic)
{
    if (ic->builtin) { lv_img_cache_invalidate_src(&ic->dsc); memset(ic, 0, sizeof *ic); return; }
    if (!ic->px) return;
    lv_img_cache_invalidate_src(&ic->dsc);                 /* (LVGL must forget it before it goes) */
    Picture p = { 0 };
    p.px = ic->px;
    picture_free(&p);
    memset(ic, 0, sizeof *ic);
}

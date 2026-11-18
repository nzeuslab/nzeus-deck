/* emoji.c - emoji pictures inside text (see emoji.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "emoji.h"
#include "carreport_int.h"        /* rp_utf8_next: one letter of a UTF-8 text */
#include "settings.h"             /* NZ_DIR: the nzeus folder on the SD card */

#define EMOJI_DIR  NZ_DIR "/emoji"
#define EMOJI_DROP 3              /* an emoji reaches this many pixels below the text's baseline */

static uint32_t *codes;           /* the code points with a picture, sorted (malloc'd once) */
static int ncodes, loaded;
static uint32_t tried_at;         /* when the list was last looked for (no pack on the card) */
static lv_font_t *font;           /* the image font (made once, kept) */

static int cmp_code(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* could this letter be an emoji? (most letters are not: no need to look at the list) */
static int maybe_emoji(uint32_t c)
{
    return c >= 0x2100 || c == 0xA9 || c == 0xAE || c == 0x203C || c == 0x2049;
}

/* read nzeus/emoji/index.txt (one hex code point per line) into RAM, once.
 * No pack on the card: looked for again at most every 5 seconds. */
static int load_index(void)
{
    if (loaded) return ncodes > 0;
    if (tried_at && lv_tick_elaps(tried_at) < 5000) return 0;
    tried_at = lv_tick_get() | 1;                          /* (| 1: never 0, "not tried yet") */
    FILE *f = fopen(EMOJI_DIR "/index.txt", "r");
    if (!f) return 0;
    loaded = 1;
    int cap = 0;
    char line[32];
    while (fgets(line, sizeof line, f)) {
        char *end;
        unsigned long c = strtoul(line, &end, 16);
        if (end == line || c < 0x80 || c > 0x10FFFF) continue;   /* (not a code point) */
        if (ncodes == cap) {                               /* grow the list */
            int ncap = cap ? cap * 2 : 1024;
            uint32_t *more = realloc(codes, sizeof(uint32_t) * (size_t)ncap);
            if (!more) break;
            codes = more;
            cap = ncap;
        }
        codes[ncodes++] = (uint32_t)c;
    }
    fclose(f);
    qsort(codes, (size_t)ncodes, sizeof(uint32_t), cmp_code);   /* (it should already be sorted) */
    return ncodes > 0;
}

void emoji_look_again(void)
{
    if (!loaded) tried_at = 0;                             /* (the next emoji looks for the pack at once) */
}

int emoji_has(uint32_t c)
{
    if (!loaded || !ncodes || !maybe_emoji(c)) return 0;
    return bsearch(&c, codes, (size_t)ncodes, sizeof(uint32_t), cmp_code) != NULL;
}

int emoji_in_text(const char *s)
{
    uint32_t c;
    if (!s) return 0;
    while ((c = rp_utf8_next(&s)) != 0)
        if (maybe_emoji(c) && load_index() && emoji_has(c)) return 1;
    return 0;
}

/* the picture's file for a code point: "S:" NZ_DIR "/emoji/1f600.bin"
 * (S: is LVGL's normal-file driver) */
static bool emoji_path(const lv_font_t *f, void *img_src, uint16_t len, uint32_t c, uint32_t next)
{
    (void)f; (void)next;
    if (!emoji_has(c)) return false;                       /* no picture: no letter */
    return snprintf(img_src, len, "S:" EMOJI_DIR "/%x.bin", (unsigned)c) < (int)len;
}

/* A letter's size and place. LVGL's own imgfont would open the picture's file
 * to read its size, for every letter, every time text is measured or drawn.
 * All our pictures are EMOJI_SIZE x EMOJI_SIZE, so the answer comes from the
 * list in RAM instead: no file is opened here (only when it is drawn). */
static bool emoji_glyph(const lv_font_t *f, lv_font_glyph_dsc_t *g, uint32_t c, uint32_t next)
{
    (void)f; (void)next;
    if (!emoji_has(c)) return false;
    g->box_w = g->box_h = EMOJI_SIZE;
    g->adv_w = EMOJI_SIZE + 1;                             /* (a pixel of room after it) */
    g->ofs_x = 0;
    g->ofs_y = -EMOJI_DROP;                                /* sit on the line like a letter */
    g->bpp = LV_IMGFONT_BPP;                               /* "this letter is a picture": LVGL draws the file */
    g->is_placeholder = 0;
    return true;
}

/* what LVGL draws for an emoji letter: its picture's file name */
static const uint8_t *emoji_bitmap(const lv_font_t *f, uint32_t c)
{
    static char path[64];
    if (!emoji_path(f, path, sizeof path, c, 0)) return NULL;
    return (const uint8_t *)path;
}

const lv_font_t *emoji_font(void)
{
    if (font) return font;
    if (!load_index()) return NULL;
    font = lv_imgfont_create(EMOJI_SIZE, emoji_path);      /* an LVGL image font ... */
    if (!font) return NULL;
    font->base_line = EMOJI_DROP;                          /* (the line's height: 13 above, 3 below) */
    font->get_glyph_dsc = emoji_glyph;                     /* ... that answers from the list in RAM */
    font->get_glyph_bitmap = emoji_bitmap;
    return font;
}

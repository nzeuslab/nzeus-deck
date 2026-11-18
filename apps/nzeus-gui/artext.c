/* artext.c - Arabic text on LVGL labels (see artext.h).
 * (Moved here from app_web.c on 2026-10-06 so WhatsApp and the lists can use
 * it too; the Web app behaves exactly as before.)
 * The letters, the joining and the right-to-left order come from the Car app's
 * report code (carreport_text.c + font_report.c).
 * Emoji (2026-10-06): a label whose text has an emoji gets a font whose chain
 * ends with the emoji pictures (emoji.c); text without Arabic is not changed. */
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "artext.h"
#include "carreport_int.h"        /* rp_shape_arabic, rp_visual_order, rp_font_* */
#include "emoji.h"                 /* emoji pictures (a font at the end of the chain) */

#define AR_FONTS 32                 /* font copies: (6 theme fonts + their fallbacks) x Arabic / emoji / both */
/* While a block is joined and turned, it needs about 5x its size for a moment
 * (the joined copy, a list of its letters, the result). A longer block (a big
 * text file with no empty lines) is shown unjoined instead, to keep RAM safe. */
#define AR_MAX_TEXT (64 * 1024)
/* Each copy is made once and kept (a few dozen bytes each, in a fixed table):
 * nothing to free, nothing can leak however often labels change. */
static struct { const lv_font_t *base, *ar, *emo; lv_font_t copy; } ar_fonts[AR_FONTS];

int artext_has_arabic(const char *s) { return rp_has_arabic(s); }

/* a copy of `base` that also finds Arabic letters in `ar` and emoji in `emo`
 * (either may be NULL) */
static const lv_font_t *with_extra(const lv_font_t *base, const lv_font_t *ar, const lv_font_t *emo)
{
    for (int i = 0; i < AR_FONTS; i++)
        if (ar_fonts[i].base == base && ar_fonts[i].ar == ar && ar_fonts[i].emo == emo) return &ar_fonts[i].copy;
    /* where missing letters are looked up: base's own fallback (with the extras
     * too, e.g. mono 16 -> Montserrat 16 -> Arabic -> emoji), or else the extras */
    const lv_font_t *next;
    if (base->fallback) next = with_extra(base->fallback, ar, emo);
    else if (ar && emo) next = with_extra(ar, NULL, emo);  /* the Arabic font, then the emoji */
    else next = ar ? ar : emo;
    for (int i = 0; i < AR_FONTS; i++) {
        if (ar_fonts[i].base) continue;
        lv_font_t *f = &ar_fonts[i].copy;
        *f = *base;
        f->fallback = next;
        /* lines tall enough for all the fonts: Arabic letters and emoji reach higher and lower */
        int up = LV_MAX(base->line_height - base->base_line, next->line_height - next->base_line);
        int down = LV_MAX(base->base_line, next->base_line);
        f->line_height = (lv_coord_t)(up + down);
        f->base_line = (lv_coord_t)down;
        ar_fonts[i].base = base;
        ar_fonts[i].ar = ar;
        ar_fonts[i].emo = emo;
        return f;
    }
    return base;                                           /* (all used: no extra letters) */
}

/* the Arabic font of about the size of `base` */
static const lv_font_t *arabic_for(const lv_font_t *base)
{
    return base->line_height >= 28 ? &rp_font_semi27      /* big headings (CRT themes) */
         : base->line_height > 20 ? &rp_font_semi20       /* headings */
         : &rp_font_reg19;                                /* text, small text */
}

/* the font for a label with Arabic: `base` + Arabic letters of about its size */
const lv_font_t *artext_font(const lv_font_t *base)
{
    return with_extra(base, arabic_for(base), NULL);
}

const lv_font_t *artext_font_for(const lv_font_t *base, const char *text)
{
    int ar = rp_has_arabic(text);
    const lv_font_t *emo = emoji_in_text(text) ? emoji_font() : NULL;
    if (!ar && !emo) return base;                          /* plain text: the font as it is */
    return with_extra(base, ar ? arabic_for(base) : NULL, emo);
}

/* 1 = the text's first real letter is Arabic: a right-to-left paragraph */
int artext_starts_arabic(const char *s)
{
    uint32_t c;
    while ((c = rp_utf8_next(&s)) != 0) {
        if ((c >= 0x0621 && c <= 0x064A) || (c >= 0xFE70 && c <= 0xFEFC)) return 1;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x600)) return 0;
    }
    return 0;
}

char *artext_layout(const char *text, const lv_font_t *font, lv_coord_t space, int width, int rtl)
{
    int n_in = (int)strlen(text);
    if (n_in > AR_MAX_TEXT) return NULL;                   /* (a huge block: shown as it is, see below) */
    int cap = n_in + n_in / 2 + 16;                        /* joined letters: 3 bytes instead of 2 */
    char *joined = malloc((size_t)cap);
    if (!joined || rp_shape_arabic(text, joined, cap) < 0) { free(joined); return NULL; }
    int jn = (int)strlen(joined);
    int out_n = jn + jn / 4 + 16;                          /* (+ a line break for each wrapped line) */
    char *out = malloc((size_t)out_n);
    if (!out) { free(joined); return NULL; }
    int o = 0;
    for (const char *p = joined; *p;) {
        /* where this line ends: LVGL's own word wrapping, or the next line break */
        size_t nl = strcspn(p, "\n");
        uint32_t k = width > 0 ? _lv_txt_get_next_line(p, font, space, (lv_coord_t)width, NULL, LV_TEXT_FLAG_NONE)
                               : (uint32_t)(nl + (p[nl] == '\n'));
        if (k == 0) k = (uint32_t)strlen(p);               /* (never stuck) */
        if (o + 1 >= out_n) { free(joined); free(out); return NULL; }
        if (o) out[o++] = '\n';
        int n = rp_visual_order(p, (int)k, rtl, out + o, out_n - o - 1);
        if (n < 0) { free(joined); free(out); return NULL; }
        o += n;
        p += k;
    }
    out[o] = 0;
    free(joined);
    return out;
}

void artext_set(lv_obj_t *l, const char *text, int width)
{
    if (!l) return;
    lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_FONT, 0);
    lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_ALIGN, 0);
    const lv_font_t *f = artext_font_for(lv_obj_get_style_text_font(l, 0), text);
    if (!rp_has_arabic(text)) {                            /* (emoji only: just the font; left to right) */
        if (f != lv_obj_get_style_text_font(l, 0)) lv_obj_set_style_text_font(l, f, 0);
        lv_label_set_text(l, text);
        return;
    }
    int rtl = artext_starts_arabic(text);
    lv_obj_set_style_text_font(l, f, 0);
    if (rtl) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);   /* Arabic starts at the right */
    char *t = artext_layout(text, f, lv_obj_get_style_text_letter_space(l, 0), width, rtl);
    lv_label_set_text(l, t ? t : text);
    free(t);
}

void artext_set_line(lv_obj_t *l, const char *text, int width)
{
    if (!l) return;
    lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_FONT, 0);
    lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_ALIGN, 0);
    size_t n = strlen(text);
    if (n > AR_MAX_TEXT) n = AR_MAX_TEXT;
    char *flat = malloc(n + 1);                            /* one line: breaks and tabs become spaces */
    if (!flat) { lv_label_set_text(l, ""); return; }
    for (size_t i = 0; i < n; i++) flat[i] = (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') ? ' ' : text[i];
    flat[n] = 0;
    int ar = rp_has_arabic(flat), rtl = ar && artext_starts_arabic(flat);
    const lv_font_t *f = artext_font_for(lv_obj_get_style_text_font(l, 0), flat);   /* (+ Arabic / emoji) */
    if (f != lv_obj_get_style_text_font(l, 0)) lv_obj_set_style_text_font(l, f, 0);
    char *src = flat, *joined = NULL;                      /* the text to cut (Arabic: joined letters) */
    if (ar) {
        if (rtl) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        int cap = (int)n + (int)n / 2 + 16;
        joined = malloc((size_t)cap);
        if (joined && rp_shape_arabic(flat, joined, cap) >= 0) src = joined;
    }
    lv_coord_t space = lv_obj_get_style_text_letter_space(l, 0);
    uint32_t len = (uint32_t)strlen(src), k = len;
    if (width > 0 && lv_txt_get_width(src, len, f, space, LV_TEXT_FLAG_NONE) > width) {
        /* too long: keep the words that fit, leaving room for "..." */
        lv_coord_t room = (lv_coord_t)(width - lv_txt_get_width("...", 3, f, space, LV_TEXT_FLAG_NONE));
        k = _lv_txt_get_next_line(src, f, space, room > 8 ? room : 8, NULL, LV_TEXT_FLAG_NONE);
        if (k == 0 || k > len) k = len;
        while (k > 0 && src[k - 1] == ' ') k--;            /* (no space before the dots) */
    }
    char *cut = malloc(k + 4);
    if (!cut) { free(flat); free(joined); lv_label_set_text(l, ""); return; }
    memcpy(cut, src, k);
    strcpy(cut + k, k < len ? "..." : "");
    char *out = NULL;
    if (ar) {                                              /* Arabic: in drawing order */
        int out_n = (int)k + (int)k / 4 + 24;
        out = malloc((size_t)out_n);
        int m = out ? rp_visual_order(cut, (int)strlen(cut), rtl, out, out_n - 1) : -1;
        if (m < 0) { free(out); out = NULL; }
        else out[m] = 0;
    }
    lv_label_set_text(l, out ? out : cut);
    free(out);
    free(cut);
    free(joined);
    free(flat);
}

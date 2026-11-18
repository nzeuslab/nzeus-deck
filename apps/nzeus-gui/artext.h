#ifndef NZEUS_ARTEXT_H
#define NZEUS_ARTEXT_H
#include "lvgl.h"

/* Arabic text on LVGL labels, shared by the Web app, the WhatsApp app and the
 * list rows (nzlist). The theme fonts have no Arabic letters, and LVGL draws
 * text left to right letter by letter, so Arabic needs three things first:
 *   1. a font: a copy of the label's font whose "fallback" is the car report's
 *      Arabic font (LVGL looks each letter up in the main font, then the fallback)
 *   2. joining: every Arabic letter gets its joined shape (start/middle/end)
 *   3. direction: each line is turned into drawing order (Arabic right to left;
 *      English words and numbers inside it stay left to right)
 * Text without Arabic is always left exactly as it is.
 * Emoji: a text with an emoji gets a font whose chain ends with the emoji
 * pictures (emoji.h); its letters and direction are not changed for that. */

int  artext_has_arabic(const char *s);              /* 1 = there is an Arabic letter in s */
int  artext_starts_arabic(const char *s);           /* 1 = the first real letter is Arabic (right to left) */
const lv_font_t *artext_font(const lv_font_t *base);   /* base + Arabic letters of about its size */
/* the font a label needs for this text: base itself for plain text, else base
 * + Arabic letters (if it has any) + emoji pictures (if it has any). The fonts
 * are made once and kept: nothing to free. */
const lv_font_t *artext_font_for(const lv_font_t *base, const char *text);

/* Arabic text, ready for LVGL: letters joined, cut into lines that fit `width`
 * pixels (0 = only at line breaks), each line in drawing order.
 * Returns a malloc'd text (free it), NULL = not enough memory / too long. */
char *artext_layout(const char *text, const lv_font_t *font, lv_coord_t space, int width, int rtl);

/* Puts `text` on label l. Text with Arabic gets the Arabic font, joined letters
 * and right-to-left lines (width: the label's text width to wrap long text,
 * 0 = only at line breaks). Other text is set as it is, and a label that showed
 * Arabic before gets its normal font and alignment back. */
void artext_set(lv_obj_t *l, const char *text, int width);

/* Like artext_set, but always ONE line that fits `width` pixels: line breaks
 * become spaces, and a longer text is cut after a word, with "..." (for an
 * Arabic text the cut keeps its START, which is drawn on the right). */
void artext_set_line(lv_obj_t *l, const char *text, int width);

#endif

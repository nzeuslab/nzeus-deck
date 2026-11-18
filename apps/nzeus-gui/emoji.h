#ifndef NZEUS_EMOJI_H
#define NZEUS_EMOJI_H
#include <stdint.h>
#include "lvgl.h"

/* Emoji inside text. The theme fonts have no emoji, so each emoji is a small
 * picture (16 x 16, Twemoji) on the SD card: nzeus/emoji/1f600.bin ...
 * (made by R36S/tools/make_emoji.py, with nzeus/emoji/index.txt listing them).
 *
 * emoji_font() is an LVGL "image font": a font whose letters are pictures.
 * artext.c puts it at the end of a label's font chain (the "fallback"), so
 * LVGL draws an emoji's picture when the normal font has no such letter.
 * The list of emoji is read once into RAM: a letter that is not an emoji is
 * answered from that list, no file is opened for it. */

#define EMOJI_SIZE 16                     /* the pictures' size in pixels */

int  emoji_has(uint32_t c);               /* 1 = there is a picture for code point c */
int  emoji_in_text(const char *s);        /* 1 = the text has an emoji there is a picture for */
const lv_font_t *emoji_font(void);        /* the image font, NULL = no emoji pictures on the card */
/* No pack on the card: it is looked for again at most every 5 seconds; this
 * makes the next emoji look at once (an app that shows emoji calls it when it opens) */
void emoji_look_again(void);

#endif

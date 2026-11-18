#ifndef NZEUS_WALLPAPER_H
#define NZEUS_WALLPAPER_H
#include <stddef.h>
#include <stdint.h>

/* wallpaper.c - the desktop's picture wallpapers. Each one is drawn ONCE into
 * a buffer of 16-bit RGB565 pixels the size of the desktop (640 x 446: about
 * 570 KB), which the desktop then shows as an image: nothing is drawn again
 * frame by frame. Free the buffer with free() when another wallpaper is chosen. */

/* "Hills": our own drawing of a sunny scene - a blue sky (deep blue at the top,
 * light at the horizon), a few soft white clouds and one rolling green hill.
 * NULL = not enough memory. */
uint16_t *wallpaper_hills(int w, int h);

/* "Picture": the user's own picture file (JPG / PNG / BMP), made to cover
 * w x h (cut at the sides or the top and bottom if its shape is different). A
 * picture that is already w x h is used as it is. NULL = missing or not a
 * picture (err says why). */
uint16_t *wallpaper_picture(const char *path, int w, int h, char *err, size_t errn);

#endif

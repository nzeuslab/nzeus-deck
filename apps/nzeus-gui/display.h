#ifndef NZEUS_DISPLAY_H
#define NZEUS_DISPLAY_H
#include <stdint.h>

#define NZ_W 640   /* the console's screen size */
#define NZ_H 480

/* The finished picture, one 16-bit RGB565 value per pixel, top row first.
 * On the console it is handed to the frontend (nzeus-modern) every frame; on
 * the PC it is saved as a picture file so we can look at it. */
extern uint16_t nzeus_fb[NZ_W * NZ_H];

/* Where LVGL's finished rows really go: nzeus_fb, or (nzeus-modern with the
 * 16-bit screen) the screen memory itself - then nothing has to be copied
 * afterwards. Screenshots read from here. */
extern uint16_t *nzeus_picture;

/* Draw straight into SCREEN (640x480 RGB565) from now on, or back into
 * nzeus_fb with NULL. Switching to a screen redraws everything into it. */
void nzeus_display_target(uint16_t *screen);

/* Connects LVGL to nzeus_fb. Call once, after lv_init(). */
void nzeus_display_init(void);

/* The CRT themes' old-monitor look (scanlines + glow), applied as the
 * picture is copied into nzeus_fb. */
void nzeus_display_set_crt(int on);

/* The rows of nzeus_fb that changed since the last call (top..bottom), or
 * -1..-1 if none did. NZEUS's own frontend (apps/frontend) asks every frame
 * and copies only those rows to the screen - nothing at all when idle. */
void nzeus_dirty_rows(int *top, int *bottom);

#endif

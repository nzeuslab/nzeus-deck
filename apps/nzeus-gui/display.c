/* display.c - connects LVGL to a plain 640x480 picture in memory.
 *
 * LVGL draws a few rows at a time into a small "draw buffer", then calls
 * flush() to copy those rows into the full picture (nzeus_fb). */
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "display.h"
#include "nzeus_tick.h"

uint16_t nzeus_fb[NZ_W * NZ_H];
uint16_t *nzeus_picture = nzeus_fb;                   /* see display.h */

#define DRAW_ROWS 60                                  /* rows LVGL draws at once */
static lv_color_t draw_rows[NZ_W * DRAW_ROWS];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;

uint32_t nzeus_tick_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int crt;                                       /* old-monitor look on/off */
void nzeus_display_set_crt(int on) { crt = on; }

/* The CRT look, applied while copying (so it costs nothing extra when the
 * picture doesn't change, and only touches the rows LVGL just redrew):
 *  - glow: each pixel picks up half of its left and right neighbours' light,
 *    so bright text gets a soft phosphor halo
 *  - scanlines: every second row is a quarter darker
 * RGB565 trick: (p >> 1) & 0x7BEF halves all three colours at once (the mask
 * stops bits sliding from one colour into the next), and OR-ing two pixels
 * keeps the brighter bits of each colour. */
static void crt_row(uint16_t *dst, const uint16_t *src, int w, int dark_row)
{
    uint16_t left = 0;
    for (int x = 0; x < w; x++) {
        uint16_t p = src[x], right = x + 1 < w ? src[x + 1] : 0;
        uint16_t g = p | ((left >> 1) & 0x7BEF) | ((right >> 1) & 0x7BEF);
        if (dark_row) g = g - ((g >> 2) & 0x39E7);          /* 75 % brightness */
        dst[x] = g;
        left = p;
    }
}

/* The rows that changed since the frontend last asked (NZEUS's own frontend
 * copies only these to the screen; -1 = nothing changed). Starts as "all". */
static int dirty_top = 0, dirty_bottom = NZ_H - 1;

void nzeus_dirty_rows(int *top, int *bottom)
{
    *top = dirty_top;
    *bottom = dirty_bottom;
    dirty_top = dirty_bottom = -1;
}

/* Copy one finished rectangle from LVGL into the full picture. */
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    int w = area->x2 - area->x1 + 1;
    if (dirty_top < 0 || area->y1 < dirty_top) dirty_top = area->y1;
    if (area->y2 > dirty_bottom) dirty_bottom = area->y2;
    for (int y = area->y1; y <= area->y2; y++) {
        uint16_t *dst = &nzeus_picture[y * NZ_W + area->x1];
        if (crt) {
            /* the effect in fast (cached) memory first, then one copy of the row:
             * pixel-by-pixel writes straight into the (uncached) screen were slower */
            static uint16_t row[NZ_W];
            crt_row(row, (const uint16_t *)colors, w, y & 1);
            memcpy(dst, row, (size_t)w * sizeof(uint16_t));
        } else {
            memcpy(dst, colors, (size_t)w * sizeof(uint16_t));
        }
        colors += w;
    }
    lv_disp_flush_ready(drv);       /* tell LVGL it may reuse its draw buffer */
}

void nzeus_display_target(uint16_t *screen)
{
    nzeus_picture = screen ? screen : nzeus_fb;
    if (screen && lv_is_initialized()) {             /* the new place starts empty: draw it all */
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_obj_invalidate(lv_layer_sys());
    }
}

void nzeus_display_init(void)
{
    lv_disp_draw_buf_init(&draw_buf, draw_rows, NULL, NZ_W * DRAW_ROWS);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = NZ_W;
    disp_drv.ver_res = NZ_H;
    disp_drv.flush_cb = flush;
    disp_drv.draw_buf = &draw_buf;
    /* Pick LVGL's normal software renderer. HiChip's copy of LVGL lost the
     * line that does this by default (their own apps set it themselves),
     * which left an empty function pointer and crashed on start. */
    disp_drv.draw_ctx_init = lv_draw_sw_init_ctx;
    disp_drv.draw_ctx_deinit = lv_draw_sw_deinit_ctx;
    disp_drv.draw_ctx_size = sizeof(lv_draw_sw_ctx_t);
    lv_disp_drv_register(&disp_drv);
}

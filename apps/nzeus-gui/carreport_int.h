/* carreport_int.h - the parts of the car report PDF maker that talk to each other.
 * (Only carreport*.c, font_report.c and the test include this; the Car app
 * only needs carscan.h.)
 *
 *   carreport.c       what the report says and where it goes on the page
 *   carreport_text.c  Arabic letter joining + right-to-left ordering
 *   carreport_pdf.c   squeezing the page pictures (deflate) + writing the .pdf file
 *   font_report.c     the letters (made by tools/make_report_fonts.py) */
#ifndef NZEUS_CARREPORT_INT_H
#define NZEUS_CARREPORT_INT_H
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

/* ---- fonts (font_report.c): name = weight + size in pixels at 150 dpi ---- */
extern const lv_font_t rp_font_reg19, rp_font_med19, rp_font_semi20, rp_font_reg22,
                       rp_font_reg23, rp_font_med25, rp_font_semi27, rp_font_bold31,
                       rp_font_bold44, rp_font_mono19, rp_font_mono22, rp_font_monomed25;

/* ---- text helpers (carreport_text.c) ---- */
uint32_t rp_utf8_next(const char **p);          /* next character, 0 at the end */
int rp_utf8_put(uint32_t c, char *out);         /* writes 1-4 bytes, returns how many */
int rp_is_mark(uint32_t c);                     /* Arabic vowel mark (sits on the letter before) */
int rp_has_arabic(const char *s);
/* Arabic joining: every Arabic letter becomes its joined shape (start /
 * middle / end / alone, Unicode "presentation forms"). Other text is copied.
 * Returns the length written, or -1 if out is too small. */
int rp_shape_arabic(const char *in, char *out, int out_n);
/* One line of text (len bytes, in reading order) -> the order to draw it
 * left to right. rtl = 1 for an Arabic paragraph. Returns length or -1. */
int rp_visual_order(const char *in, int len, int rtl, char *out, int out_n);

/* ---- PDF file (carreport_pdf.c): one picture per page ---- */
typedef struct RpPdf RpPdf;
RpPdf *rp_pdf_open(const char *path, int w_px, int h_px, int dpi);
int rp_pdf_page_begin(RpPdf *p);
int rp_pdf_rows(RpPdf *p, const lv_color_t *rows, int nrows);   /* next rows of the page, top to bottom */
int rp_pdf_page_end(RpPdf *p);
int rp_pdf_close(RpPdf *p, const char *title);  /* finishes the file; 0 = ok */
void rp_pdf_abort(RpPdf *p);                    /* closes and deletes a half-written file */

/* ---- memory bookkeeping, so the tests can check the peak ---- */
void *rp_malloc(size_t n);
void *rp_realloc(void *p, size_t n);
void rp_free(void *p);
size_t carreport_mem_peak(void);                /* most bytes in use at once (last report) */
void carreport_mem_reset(void);
/* how many pages the last report had (tests) */
int carreport_last_pages(void);

#endif

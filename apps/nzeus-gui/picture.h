#ifndef NZEUS_PICTURE_H
#define NZEUS_PICTURE_H
#include <stddef.h>
#include <stdint.h>

/* Loads a PNG, JPG or BMP picture, already shrunk to fit a box (never made
 * bigger), as 16-bit RGB565 pixels ready for the screen.
 *
 * Memory matters on the console (about 42 MB for everything), so pictures are
 * shrunk WHILE they are decoded where possible:
 *   JPG  - the decoder itself skips detail (1/2, 1/4, 1/8), then we pick
 *          pixels: even a 12-megapixel photo needs only a few MB
 *   BMP  - read row by row, only the rows we need are kept
 *   PNG  - has to be decoded whole, so PNGs are limited to 2.5 megapixels */
typedef struct {
    int w, h;              /* the shrunk picture */
    int orig_w, orig_h;    /* the size in the file */
    uint16_t *px;          /* w * h pixels, row by row (free with picture_free) */
} Picture;

/* 0 = ok; otherwise -1 and a short reason in err ("Not a picture", ...) */
int  picture_load(const char *path, int max_w, int max_h, Picture *out, char *err, size_t errn);
/* A near-native report page. Exact 1754-edge rendering was slow on the console. */
#define PICTURE_PDF_EDGE 1750
/* PDF renderer output only: preserve pixels within this bounded square. */
int  picture_load_pdf(const char *path, Picture *out, char *err, size_t errn);
void picture_free(Picture *p);

#endif

/* picture.c - PNG / JPG / BMP loading for the Images app (see picture.h).
 * Uses the PNG decoder (lodepng) and JPG decoder (TJpgDec) that come with LVGL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "picture.h"
#include "src/extra/libs/png/lodepng.h"
#include "src/extra/libs/sjpg/tjpgd.h"

#define PNG_MAX_PIXELS (2500 * 1000)
#define FILE_MAX_BYTES (40 * 1024 * 1024)

/* --- shrinking: which source pixel lands on which screen pixel ---
 * For every source column/row: the destination column/row it fills, or -1. */
typedef struct {
    Picture *out;
    int src_w, src_h;
    int *col_map, *row_map;
} Shrink;

static int shrink_begin(Shrink *s, Picture *out, int src_w, int src_h, int max_w, int max_h)
{
    memset(s, 0, sizeof *s);
    s->out = out;
    s->src_w = src_w;
    s->src_h = src_h;
    double k = 1.0;                                     /* fit inside the box, never enlarge */
    if (src_w > max_w) k = (double)max_w / src_w;
    if (src_h * k > max_h) k = (double)max_h / src_h;
    out->w = (int)(src_w * k + 0.5);
    out->h = (int)(src_h * k + 0.5);
    if (out->w < 1) out->w = 1;
    if (out->h < 1) out->h = 1;
    out->px = calloc((size_t)out->w * out->h, sizeof(uint16_t));
    s->col_map = malloc(sizeof(int) * src_w);
    s->row_map = malloc(sizeof(int) * src_h);
    if (!out->px || !s->col_map || !s->row_map) return -1;
    for (int x = 0; x < src_w; x++) s->col_map[x] = -1;
    for (int y = 0; y < src_h; y++) s->row_map[y] = -1;
    for (int dx = 0; dx < out->w; dx++) s->col_map[(int)((dx + 0.5) * src_w / out->w)] = dx;   /* the source pixel */
    for (int dy = 0; dy < out->h; dy++) s->row_map[(int)((dy + 0.5) * src_h / out->h)] = dy;   /* in each cell's middle */
    return 0;
}

static void shrink_end(Shrink *s) { free(s->col_map); free(s->row_map); }

static inline void put(Shrink *s, int x, int y, unsigned r, unsigned g, unsigned b)
{
    if (x < 0 || y < 0 || x >= s->src_w || y >= s->src_h) return;
    int dx = s->col_map[x], dy = s->row_map[y];
    if (dx < 0 || dy < 0) return;
    s->out->px[dy * s->out->w + dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int fail(char *err, size_t n, const char *why) { snprintf(err, n, "%s", why); return -1; }

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > FILE_MAX_BYTES) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const unsigned char *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---------- PNG ---------- */
static int load_png(const unsigned char *data, size_t size, int max_w, int max_h, unsigned max_pixels, Picture *out, char *err, size_t n)
{
    if (size < 24) return fail(err, n, "The PNG file is damaged");
    uint32_t w = be32(data + 16), h = be32(data + 20);   /* the IHDR chunk: size first, decode later */
    out->orig_w = (int)w;
    out->orig_h = (int)h;
    if (!w || !h || (uint64_t)w * h > max_pixels)
        return fail(err, n, max_pixels == PNG_MAX_PIXELS ? "This PNG is too big for the console (2.5 megapixels max)" : "The rendered PDF page is too large");
    unsigned char *rgba = NULL;
    unsigned dw, dh;
    if (lodepng_decode32(&rgba, &dw, &dh, data, size) || !rgba) { free(rgba); return fail(err, n, "The PNG file is damaged"); }
    Shrink s;
    int rc = shrink_begin(&s, out, (int)dw, (int)dh, max_w, max_h);
    if (rc == 0)
        for (unsigned y = 0; y < dh; y++) {
            if (s.row_map[y] < 0) continue;
            const unsigned char *p = rgba + (size_t)y * dw * 4;
            for (unsigned x = 0; x < dw; x++, p += 4) {
                unsigned a = p[3];                        /* see-through parts: drawn over black */
                put(&s, (int)x, (int)y, p[0] * a / 255, p[1] * a / 255, p[2] * a / 255);
            }
        }
    shrink_end(&s);
    lv_mem_free(rgba);                                    /* (lodepng allocates through LVGL) */
    return rc == 0 ? 0 : fail(err, n, "Not enough memory for this picture");
}

/* ---------- BMP (uncompressed 8, 16, 24 and 32 bit) ---------- */
static int load_bmp(const unsigned char *d, size_t size, int max_w, int max_h, Picture *out, char *err, size_t n)
{
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return fail(err, n, "The BMP file is damaged");
    uint32_t off = le32(d + 10), hdr = le32(d + 14), comp = le32(d + 30);
    int w = (int)le32(d + 18), h = (int)le32(d + 22), bpp = le16(d + 28);
    int top_down = h < 0;
    if (top_down) h = -h;
    out->orig_w = w;
    out->orig_h = h;
    if (w <= 0 || h <= 0 || w > 20000 || h > 20000) return fail(err, n, "The BMP file is damaged");
    if (!(comp == 0 || (comp == 3 && (bpp == 16 || bpp == 32))) || !(bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32))
        return fail(err, n, "This kind of BMP is not supported (compressed)");
    size_t row = ((size_t)bpp * w + 31) / 32 * 4;
    if (off + row * h > size) return fail(err, n, "The BMP file is damaged");
    const unsigned char *pal = d + 14 + hdr;            /* 8-bit: the colour table */
    Shrink s;
    if (shrink_begin(&s, out, w, h, max_w, max_h) != 0) { shrink_end(&s); return fail(err, n, "Not enough memory for this picture"); }
    for (int y = 0; y < h; y++) {
        int sy = top_down ? y : h - 1 - y;              /* BMP rows are usually stored bottom first */
        if (s.row_map[sy] < 0) continue;
        const unsigned char *p = d + off + row * y;
        for (int x = 0; x < w; x++) {
            unsigned r, g, b;
            if (bpp == 24)      { b = p[x * 3]; g = p[x * 3 + 1]; r = p[x * 3 + 2]; }
            else if (bpp == 32) { b = p[x * 4]; g = p[x * 4 + 1]; r = p[x * 4 + 2]; }
            else if (bpp == 16) { uint16_t v = le16(p + x * 2);    /* 5-6-5 (BITFIELDS) or 5-5-5 */
                                  if (comp == 3) { r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; }
                                  else           { r = ((v >> 10) & 31) << 3; g = ((v >> 5) & 31) << 3; b = (v & 31) << 3; } }
            else                { const unsigned char *c = pal + p[x] * 4; b = c[0]; g = c[1]; r = c[2]; }
            put(&s, x, sy, r, g, b);
        }
    }
    shrink_end(&s);
    return 0;
}

/* ---------- JPG (TJpgDec, shrinking while decoding) ---------- */
typedef struct { const unsigned char *data; size_t size, pos; Shrink s; } JpgJob;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    JpgJob *j = jd->device;
    if (n > j->size - j->pos) n = j->size - j->pos;
    if (buf) memcpy(buf, j->data + j->pos, n);
    j->pos += n;
    return n;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    JpgJob *j = jd->device;
    const uint8_t *p = bitmap;                         /* RGB888, the rectangle row by row */
    for (int y = r->top; y <= r->bottom; y++)
        for (int x = r->left; x <= r->right; x++, p += 3) put(&j->s, x, y, p[0], p[1], p[2]);
    return 1;
}

static int load_jpg(const unsigned char *data, size_t size, int max_w, int max_h, Picture *out, char *err, size_t n)
{
    static uint8_t work[4096];                         /* the decoder's scratch space */
    JDEC jd;
    JpgJob j = { data, size, 0, { 0 } };
    JRESULT rc = jd_prepare(&jd, jpg_in, work, sizeof work, &j);
    if (rc == JDR_FMT3) return fail(err, n, "This JPG kind (progressive) is not supported");
    if (rc != JDR_OK) return fail(err, n, "The JPG file is damaged");
    out->orig_w = jd.width;
    out->orig_h = jd.height;
    uint8_t scale = 0;                                 /* 0..3 = 1/1 .. 1/8 */
    while (scale < 3 && ((jd.width >> scale) > 2 * max_w || (jd.height >> scale) > 2 * max_h)) scale++;
    int sw = (jd.width + (1 << scale) - 1) >> scale, sh = (jd.height + (1 << scale) - 1) >> scale;
    if (shrink_begin(&j.s, out, sw, sh, max_w, max_h) != 0) { shrink_end(&j.s); return fail(err, n, "Not enough memory for this picture"); }
    rc = jd_decomp(&jd, jpg_out, scale);
    shrink_end(&j.s);
    if (rc != JDR_OK) return fail(err, n, "The JPG file is damaged");
    return 0;
}

/* the picture's bytes (a file's, or ones built into the program) -> out */
static int decode(const unsigned char *data, size_t size, int max_w, int max_h, unsigned max_pixels, Picture *out, char *err, size_t errn)
{
    int rc;
    if (size > 8 && !memcmp(data, "\x89PNG", 4)) rc = load_png(data, size, max_w, max_h, max_pixels, out, err, errn);
    else if (size > 3 && data[0] == 0xFF && data[1] == 0xD8) rc = load_jpg(data, size, max_w, max_h, out, err, errn);
    else if (size > 2 && data[0] == 'B' && data[1] == 'M') rc = load_bmp(data, size, max_w, max_h, out, err, errn);
    else rc = fail(err, errn, "Not a PNG, JPG or BMP picture");
    if (rc != 0) picture_free(out);
    return rc;
}

static int load(const char *path, int max_w, int max_h, unsigned max_pixels, Picture *out, char *err, size_t errn)
{
    memset(out, 0, sizeof *out);
    size_t size = 0;
    unsigned char *data = read_file(path, &size);
    if (!data) return fail(err, errn, size > FILE_MAX_BYTES ? "The file is too big" : "Could not read the file");
    int rc = decode(data, size, max_w, max_h, max_pixels, out, err, errn);
    free(data);
    return rc;
}

int picture_load_mem(const unsigned char *data, size_t size, int max_w, int max_h, Picture *out, char *err, size_t errn)
{
    memset(out, 0, sizeof *out);
    return decode(data, size, max_w, max_h, PNG_MAX_PIXELS, out, err, errn);
}

int picture_load(const char *path, int max_w, int max_h, Picture *out, char *err, size_t errn)
{
    return load(path, max_w, max_h, PNG_MAX_PIXELS, out, err, errn);
}

int picture_load_pdf(const char *path, Picture *out, char *err, size_t errn)
{
    /* Keep all pixels of the bounded renderer output, including square pages. */
    return load(path, PICTURE_PDF_EDGE, PICTURE_PDF_EDGE, PICTURE_PDF_EDGE * PICTURE_PDF_EDGE, out, err, errn);
}

void picture_free(Picture *p)
{
    free(p->px);
    p->px = NULL;
}

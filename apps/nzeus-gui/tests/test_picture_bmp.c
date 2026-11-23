/* test_picture_bmp.c - PC test: damaged or hostile BMP files must be refused,
 * never read outside the file. Built by build_picture_test.sh with
 * AddressSanitizer, which stops the program at the first byte read outside
 * the file's memory (that would be a FAIL even if the picture "looked" fine). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picture.h"

/* lodepng (PNG) allocates through LVGL; this test needs plain malloc only */
void *lv_mem_alloc(size_t n) { return malloc(n); }
void *lv_mem_realloc(void *p, size_t n) { return realloc(p, n); }
void lv_mem_free(void *p) { free(p); }
void *lv_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void lv_memset(void *d, unsigned char v, size_t n) { memset(d, v, n); }
/* lodepng's file functions: never used here (pictures come from memory) */
int lv_fs_open() { return 1; }
int lv_fs_close() { return 1; }
int lv_fs_read() { return 1; }
int lv_fs_write() { return 1; }
int lv_fs_seek() { return 1; }
int lv_fs_tell() { return 1; }

static int failures;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

/* An 8-bit BMP, w x h, with a colour table of ncol entries; every pixel uses
 * colour 'index'. Colour 1 is pure red, the others black. Returns its size.
 * The header fields can be overwritten afterwards to make it hostile. */
static size_t make_bmp8(unsigned char *d, int w, int h, unsigned ncol, unsigned index)
{
    unsigned row = (8 * w + 31) / 32 * 4;
    unsigned off = 14 + 40 + ncol * 4;
    size_t size = off + row * h;
    memset(d, 0, size);
    d[0] = 'B'; d[1] = 'M';
    put32(d + 2, size);
    put32(d + 10, off);          /* where the pixels start */
    put32(d + 14, 40);           /* info header size */
    put32(d + 18, w);
    put32(d + 22, h);
    put16(d + 26, 1);
    put16(d + 28, 8);            /* bits per pixel */
    put32(d + 30, 0);            /* not compressed */
    put32(d + 46, ncol);         /* colours used */
    if (ncol > 1) { d[54 + 4 + 2] = 255; }   /* colour 1 = red (stored B, G, R, 0) */
    for (int y = 0; y < h; y++) memset(d + off + y * row, index, w);
    return size;
}

/* load from a heap copy of exactly 'size' bytes, so AddressSanitizer sees the end */
static int load(const unsigned char *src, size_t size, Picture *p)
{
    char err[100];
    unsigned char *copy = malloc(size);
    memcpy(copy, src, size);
    int rc = picture_load_mem(copy, size, 64, 64, p, err, sizeof err);
    free(copy);
    return rc;
}

int main(int argc, char **argv)
{
    static unsigned char d[4096];
    Picture p;
    int only = argc > 1 ? atoi(argv[1]) : 0;      /* "test_picture_bmp 3" = only case 3 */
#define CASE(k) if (!only || only == (k))
    setvbuf(stdout, NULL, _IONBF, 0);            /* each line out at once, even if a case crashes */
    size_t n;

    CASE(1) { /* 1. a normal 8-bit BMP still works: colour 1 = red = 0xF800 in RGB565 */
    n = make_bmp8(d, 4, 4, 2, 1);
    check(load(d, n, &p) == 0 && p.w == 4 && p.h == 4 && p.px[0] == 0xF800, "a normal 8-bit BMP decodes to the right colour");
    picture_free(&p);
    }

    CASE(2) { /* 2. info-header size pointing far past the file: the colour table would be outside */
    n = make_bmp8(d, 4, 4, 2, 1);
    put32(d + 14, 0x7FFFFFF0);
    check(load(d, n, &p) != 0, "a header size past the end of the file is refused");
    }

    CASE(3) { /* 3. pixel start so large that 'start + rows' wraps round on a 32-bit system */
    n = make_bmp8(d, 4, 4, 2, 1);
    put32(d + 10, 0xFFFFFFF0);
    check(load(d, n, &p) != 0, "a pixel start that wraps round is refused");
    }

    CASE(4) { /* 4. colour table cut short: says 256 colours, the file holds far fewer */
    n = make_bmp8(d, 4, 4, 2, 1);
    put32(d + 46, 256);
    check(load(d, n, &p) != 0, "a colour table longer than the file is refused");
    }

    CASE(5) { /* 5. a pixel that uses a colour the table does not have: drawn black, not read from outside */
    n = make_bmp8(d, 4, 4, 2, 200);
    check(load(d, n, &p) == 0 && p.px[0] == 0, "a colour number past the table is drawn black");
    picture_free(&p);
    }

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}

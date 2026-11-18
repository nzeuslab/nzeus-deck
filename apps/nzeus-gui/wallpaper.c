/* wallpaper.c - the desktop's picture wallpapers (see wallpaper.h). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picture.h"
#include "wallpaper.h"

/* ---------- colours ---------- */
typedef struct { float r, g, b; } Rgb;          /* 0..255 each */

static Rgb rgb(uint32_t hex) { Rgb c = { (float)(hex >> 16 & 255), (float)(hex >> 8 & 255), (float)(hex & 255) }; return c; }

static Rgb mix(Rgb a, Rgb b, float t)            /* t = 0: a, t = 1: b */
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    Rgb c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
    return c;
}

/* 8-bit colour -> RGB565, with a little "ordered dither" (a fixed 4 x 4
 * pattern) so a smooth sky does not turn into visible stripes on a 16-bit screen */
static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static uint16_t to565(Rgb c, int x, int y)
{
    int d = bayer[y & 3][x & 3];                     /* 0..15 */
    int r = ((int)c.r + d / 2) >> 3, g = ((int)c.g + d / 4) >> 2, b = ((int)c.b + d / 2) >> 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* ---------- Hills ---------- */
/* each cloud is a few overlapping soft ovals: centre x, y and half width,
 * half height, for a 640 x 446 picture (other sizes are scaled) */
typedef struct { float cx, cy, rx, ry; } Blob;
static const Blob blobs[] = {
    { 110, 70, 70, 22 }, { 152, 56, 50, 26 }, { 196, 72, 58, 20 },          /* cloud 1 */
    { 428, 96, 78, 24 }, { 474, 80, 54, 28 }, { 522, 98, 64, 20 },          /* cloud 2 */
    { 300, 150, 56, 14 }, { 334, 141, 40, 16 },                             /* cloud 3: small, low */
    { 588, 40, 50, 15 }, { 614, 31, 34, 16 },                               /* cloud 4: top right */
    { 36, 176, 46, 12 }, { 72, 168, 34, 14 },                               /* cloud 5: left, low */
};
#define BLOB_COUNT (int)(sizeof blobs / sizeof blobs[0])

uint16_t *wallpaper_hills(int w, int h)
{
    uint16_t *px = malloc((size_t)w * h * sizeof *px);
    float *crest = malloc((size_t)w * sizeof *crest);
    if (!px || !crest) { free(px); free(crest); return NULL; }
    float sx = w / 640.0f, sy = h / 446.0f;
    const Rgb sky_top = rgb(0x1C55C9), sky_low = rgb(0xB8DCF6), cloud = rgb(0xFBFCFF), cloud_shade = rgb(0xD4E1F0);
    const Rgb hill_crest = rgb(0xA6DC63), hill_light = rgb(0x6DBB3C), hill_dark = rgb(0x2B761C);
    /* the hill's top edge: highest a little left of the middle, a gentle wave on it */
    for (int x = 0; x < w; x++) {
        float u = x / sx;                                  /* (in 640-wide units) */
        crest[x] = (222 + 0.00045f * (u - 270) * (u - 270) + 6 * sinf(u / 70.0f)) * sy;
    }
    float horizon = h * 0.64f;
    for (int y = 0; y < h; y++) {
        float t = y / horizon;                             /* the sky: deep blue -> light near the horizon */
        Rgb sky = mix(sky_top, sky_low, t * t * 0.35f + t * 0.65f);
        for (int x = 0; x < w; x++) {
            Rgb c = sky;
            /* clouds: the strongest of the soft ovals over this point */
            float a = 0, shade = 0;
            for (int i = 0; i < BLOB_COUNT; i++) {
                float dx = (x - blobs[i].cx * sx) / (blobs[i].rx * sx), dy = (y - blobs[i].cy * sy) / (blobs[i].ry * sy);
                float d = dx * dx + dy * dy;
                if (d >= 1) continue;
                float k = (1 - d) * (1 - d) * 1.6f;          /* soft edge, solid middle */
                if (k > 1) k = 1;
                if (k > a) { a = k; shade = dy > 0 ? dy : 0; }   /* the lower half a little greyer */
            }
            if (a > 0) c = mix(c, mix(cloud, cloud_shade, shade * 0.8f), a * 0.92f);
            /* the hill (with a smooth edge: the pixel the edge crosses is mixed) */
            float below = y + 1 - crest[x];
            if (below > 0) {
                float depth = (y - crest[x]) / (h - crest[x]);   /* 0 at the crest, 1 at the bottom */
                Rgb g = depth < 0.22f ? mix(hill_crest, hill_light, depth / 0.22f)
                                      : mix(hill_light, hill_dark, (depth - 0.22f) / 0.78f);
                g = mix(g, hill_dark, 0.18f * x / w);           /* the sun is on the left */
                c = mix(c, g, below > 1 ? 1 : below);
            }
            px[y * w + x] = to565(c, x, y);
        }
    }
    free(crest);
    return px;
}

/* ---------- Picture ---------- */
uint16_t *wallpaper_picture(const char *path, int w, int h, char *err, size_t errn)
{
    Picture p;
    /* load it at most 1.5 x the screen (big photos are shrunk while they are read) */
    if (picture_load(path, w * 3 / 2, h * 3 / 2, &p, err, errn) != 0) return NULL;
    if (p.w == w && p.h == h) return p.px;           /* already the right size: no copy, no scaling */
    uint16_t *px = malloc((size_t)w * h * sizeof *px);
    if (!px) { picture_free(&p); snprintf(err, errn, "Not enough memory"); return NULL; }
    /* cover: the bigger of the two scales, the middle of the picture */
    float s = (float)w / p.w > (float)h / p.h ? (float)w / p.w : (float)h / p.h;
    float ox = (p.w * s - w) / 2, oy = (p.h * s - h) / 2;
    for (int y = 0; y < h; y++) {
        int sy = (int)((y + oy) / s);
        if (sy >= p.h) sy = p.h - 1;
        for (int x = 0; x < w; x++) {
            int sx = (int)((x + ox) / s);
            if (sx >= p.w) sx = p.w - 1;
            px[y * w + x] = p.px[sy * p.w + sx];
        }
    }
    picture_free(&p);
    return px;
}

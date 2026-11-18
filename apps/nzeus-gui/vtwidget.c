/* vtwidget.c - a real terminal emulator as an LVGL widget (see vtwidget.h).
 *
 * How it fits together:
 *   program output --> vtw_feed() --> libvterm (understands the escape
 *   sequences, keeps the screen: one cell per character with its colours)
 *   libvterm says "these cells changed" (damage) --> we mark those rows
 *   dirty --> LVGL redraws only those rectangles --> draw_event() paints the
 *   cells: background rectangles first, then text with our 8 x 16 font.
 *   Box-drawing and block characters are painted as rectangles here, because
 *   the font only has ASCII.
 *
 *   key presses --> vtw_key() --> libvterm turns them into bytes such as
 *   ESC [ A (arrow up) --> output_cb() queues them --> vtw_poll() writes them
 *   to the program (or the caller fetches them with vtw_take_input()).
 *
 *   lines that scroll off the top --> sb_pushline() keeps them, packed small
 *   ("kept lines", about 500) --> vtw_scroll() shows them again. */
#define _GNU_SOURCE             /* posix_openpt, ptsname */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "vterm.h"
#include "vtwidget.h"
#include "usbkbd.h"             /* the KB_* key codes */

LV_FONT_DECLARE(lv_font_nzeus_mono_16);   /* font_mono16.c: 8 x 16, ASCII only */

#define CW VTW_CELL_W
#define CH VTW_CELL_H
#define FONT_TOP 2              /* the font draws a cell's top pixel 2 rows below the label's top */
#define GH 16                   /* the letters are 16 px tall; a row is CH = 20: */
#define PAD ((CH - GH) / 2)     /* 2 px of air above and below them, so lines don't touch */
#define POLL_BUDGET 16384       /* most bytes read from the program per vtw_poll (keeps frames short) */
#define OUTQ_MAX (1024 * 1024)  /* most bytes waiting to go to the program */

/* one cell, ready to draw */
typedef struct {
    uint32_t cp;                /* character drawn by our own code (box, block, symbol) */
    lv_color_t fg, bg;
    char ascii;                 /* ASCII character drawn with the font (' ' = nothing) */
    uint8_t flags;
} vcell_t;

enum { CF_BOLD = 1, CF_UL = 2, CF_UL2 = 4, CF_STRIKE = 8, CF_WIDE = 16, CF_SPECIAL = 32 };

/* A kept line (one that scrolled off the top), packed small:
 *   sbline_t        how many cells, how many colour runs
 *   sbrun_t runs[]  where each run of the same colours / style starts
 *   uint16_t ch[]   one code per cell: ASCII, or a box / block / symbol
 * (see "kept lines" below) */
typedef struct {
    uint16_t start;             /* the run's first cell */
    lv_color_t fg, bg;
    uint8_t flags;              /* CF_* */
} sbrun_t;

typedef struct {
    uint16_t len, nruns;        /* cells kept (blanks at the end are left out), colour runs */
} sbline_t;                     /* followed by sbrun_t runs[nruns], then uint16_t ch[len] */

typedef struct {
    VTerm *vt;
    VTermScreen *screen;
    lv_obj_t *obj;
    int cols, rows;

    lv_color_t fg, bg;          /* default colours */
    lv_color_t pal[256];        /* the xterm 256-colour palette */

    uint16_t *dirty_lo, *dirty_hi;  /* per row: changed columns lo..hi (lo > hi: clean) */
    vcell_t *cells;             /* one row being drawn */
    char *text;                 /* one text run being drawn */

    int cur_row, cur_col, cur_visible, cur_shape;
    char title[64];

    char *outq;                 /* bytes waiting to go to the program */
    size_t outq_len, outq_cap;

    pid_t pid;                  /* the spawned program, or -1 */
    int fd_in, fd_out;          /* its input / output (the same fd with a PTY) */
    int pty, exit_code;
    int raw_pipe;               /* pipes, but the far end has a real terminal (ssh -t): no local editing */
    char line[256];             /* pipe mode: the line being typed */
    int line_len;
    char carry[4];              /* an unfinished UTF-8 character from the last chunk */
    int carry_len;

    sbline_t **sb;              /* kept lines: a ring of SB_MAX_LINES (made at the first one) */
    int sb_first, sb_count;     /* where the oldest is in the ring, how many there are */
    size_t sb_bytes;            /* their memory */
    int sb_offset;              /* how many lines the view is scrolled back (0 = live) */
    int altscreen;              /* a full-screen program's own screen is up (no scrolling) */
    unsigned long changes;      /* +1 for every bit of output (the chat view: "anything new?") */
} vtw_t;

static vtw_t *get(lv_obj_t *obj) { return obj ? (vtw_t *)lv_obj_get_user_data(obj) : NULL; }

/* ------------------------------------------------------------------ colours */

/* The 16 basic colours: xterm's defaults, except a lighter blue, which is
 * easier to read on a dark background. */
static const uint32_t ansi16[16] = {
    0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x3b6cff, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
    0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x6c8cff, 0xff00ff, 0x00ffff, 0xffffff };

static lv_color_t rgb_to_color(uint32_t rgb)
{
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

/* xterm's 256 colours: 16 basic, a 6x6x6 colour cube, 24 greys */
static void make_palette(vtw_t *t)
{
    static const uint8_t level[6] = { 0, 95, 135, 175, 215, 255 };
    for (int i = 0; i < 16; i++) t->pal[i] = rgb_to_color(ansi16[i]);
    for (int i = 0; i < 216; i++)
        t->pal[16 + i] = lv_color_make(level[i / 36], level[(i / 6) % 6], level[i % 6]);
    for (int i = 0; i < 24; i++) {
        uint8_t g = (uint8_t)(8 + 10 * i);
        t->pal[232 + i] = lv_color_make(g, g, g);
    }
}

static lv_color_t cell_color(const vtw_t *t, const VTermColor *c, int is_fg)
{
    if (is_fg && VTERM_COLOR_IS_DEFAULT_FG(c)) return t->fg;
    if (!is_fg && VTERM_COLOR_IS_DEFAULT_BG(c)) return t->bg;
    if (VTERM_COLOR_IS_INDEXED(c)) return t->pal[c->indexed.idx];
    return lv_color_make(c->rgb.red, c->rgb.green, c->rgb.blue);
}

/* ------------------------------------------------------- damage -> redraw */

static void mark(vtw_t *t, int row, int c0, int c1)    /* c1 inclusive */
{
    if (row < 0 || row >= t->rows) return;
    if (c0 < 0) c0 = 0;
    if (c1 >= t->cols) c1 = t->cols - 1;
    if (c0 < t->dirty_lo[row]) t->dirty_lo[row] = (uint16_t)c0;
    if (c1 > t->dirty_hi[row] || t->dirty_lo[row] > t->dirty_hi[row]) t->dirty_hi[row] = (uint16_t)c1;
}

static void mark_all(vtw_t *t)
{
    for (int r = 0; r < t->rows; r++) mark(t, r, 0, t->cols - 1);
}

/* Mark one rectangle for redrawing. lv_obj_invalidate_area() would make it
 * 5 px bigger on every side (LVGL 8.3 adds that margin for transformed
 * objects, even when nothing is transformed): a 16 px row would cost 26 px.
 * So: ask LVGL whether (and where) the area is visible, cut its margin off
 * again, and register exactly that. */
static void invalidate(vtw_t *t, const lv_area_t *a)
{
    lv_area_t vis = *a, exact;
    if (!lv_obj_area_is_visible(t->obj, &vis)) return;     /* hidden, off screen... */
    if (!_lv_area_intersect(&exact, &vis, a)) return;
    _lv_inv_area(lv_obj_get_disp(t->obj), &exact);
}

/* Tell LVGL which rectangles must be redrawn. Neighbouring dirty rows are
 * joined into one rectangle; with many separate bands everything dirty is
 * joined into one, since LVGL only keeps a short list of areas. */
static void flush_damage(vtw_t *t)
{
    lv_area_t band[8];
    int nb = 0, too_many = 0;
    lv_area_t all = { 0, 0, -1, -1 };
    lv_area_t o;
    lv_obj_get_coords(t->obj, &o);
    for (int r = 0; r < t->rows; r++) {
        if (t->dirty_lo[r] > t->dirty_hi[r]) continue;
        lv_area_t a = { (lv_coord_t)(o.x1 + t->dirty_lo[r] * CW), (lv_coord_t)(o.y1 + r * CH),
                        (lv_coord_t)(o.x1 + (t->dirty_hi[r] + 1) * CW - 1), (lv_coord_t)(o.y1 + (r + 1) * CH - 1) };
        t->dirty_lo[r] = 0xffff;
        t->dirty_hi[r] = 0;
        if (all.x2 < all.x1) all = a; else _lv_area_join(&all, &all, &a);
        if (nb > 0 && band[nb - 1].y2 + 1 == a.y1) _lv_area_join(&band[nb - 1], &band[nb - 1], &a);
        else if (nb < 8) band[nb++] = a;
        else too_many = 1;
    }
    if (all.x2 < all.x1) return;                  /* nothing changed */
    if (t->sb_offset) return;                     /* scrolled back: the view stays still while new output
                                                   * arrives (Claude's spinner redraws all the time) */
    if (too_many) invalidate(t, &all);
    else for (int i = 0; i < nb; i++) invalidate(t, &band[i]);
}

/* Scrolled back to older lines? Go back to the live screen (new output, a
 * typed key...). The whole widget is redrawn once. */
static void scroll_live(vtw_t *t)
{
    if (!t->sb_offset) return;
    t->sb_offset = 0;
    lv_obj_invalidate(t->obj);
}

/* ------------------------------------------------------ libvterm callbacks */

static int cb_damage(VTermRect r, void *user)
{
    vtw_t *t = user;
    for (int row = r.start_row; row < r.end_row; row++) mark(t, row, r.start_col, r.end_col - 1);
    return 1;
}

static void mark_cursor(vtw_t *t)       /* the cursor cell (2 wide: it may sit on a wide character) */
{
    mark(t, t->cur_row, t->cur_col, t->cur_col + 1);
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    vtw_t *t = user;
    (void)oldpos;
    mark_cursor(t);                     /* where it was */
    t->cur_row = pos.row;
    t->cur_col = pos.col;
    t->cur_visible = visible;
    mark_cursor(t);                     /* where it is now */
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    vtw_t *t = user;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: t->cur_visible = val->boolean; mark_cursor(t); break;
    case VTERM_PROP_CURSORSHAPE:   t->cur_shape = val->number;    mark_cursor(t); break;
    case VTERM_PROP_REVERSE:       mark_all(t); break;
    case VTERM_PROP_ALTSCREEN:                    /* a full-screen program starts / ends */
        t->altscreen = val->boolean;
        scroll_live(t);
        mark_all(t);
        break;
    case VTERM_PROP_TITLE: {       /* the title may arrive in several pieces */
        size_t have = val->string.initial ? 0 : strlen(t->title);
        size_t n = val->string.len;
        if (n > sizeof t->title - 1 - have) n = sizeof t->title - 1 - have;
        memcpy(t->title + have, val->string.str, n);
        t->title[have + n] = 0;
        break;
    }
    default: break;
    }
    return 1;
}

static int cb_bell(void *user) { (void)user; return 1; }

/* (kept lines: further down) */
static int sb_pushline(int cols, const VTermScreenCell *cells, void *user);
static int sb_popline(int cols, VTermScreenCell *cells, void *user);
static int sb_clear(void *user);

static const VTermScreenCallbacks screen_cbs = {
    .damage = cb_damage,
    .movecursor = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell = cb_bell,
    /* no moverect: on scrolling libvterm reports the moved rows as damaged */
    .sb_pushline = sb_pushline,     /* a line scrolled off the top: keep it */
    .sb_popline = sb_popline,       /* the screen grew taller: give the newest kept line back */
    .sb_clear = sb_clear,           /* ESC [ 3 J: forget the kept lines */
};

/* libvterm wants to send bytes to the program (keys, answers to queries) */
static void output_cb(const char *s, size_t len, void *user)
{
    vtw_t *t = user;
    if (t->outq_len + len > t->outq_cap) {
        size_t cap = t->outq_cap ? t->outq_cap : 1024;
        while (cap < t->outq_len + len) cap *= 2;
        if (cap > OUTQ_MAX) return;                 /* program not reading: drop */
        char *q = realloc(t->outq, cap);
        if (!q) return;
        t->outq = q;
        t->outq_cap = cap;
    }
    memcpy(t->outq + t->outq_len, s, len);
    t->outq_len += len;
}

/* ---------------------------------------------------- bytes -> libvterm */

static size_t utf8_len(unsigned char lead) { return lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4; }

/* Give bytes to libvterm, but never half a UTF-8 character: libvterm 0.3.3
 * turns a character split between two writes into U+FFFD when the first
 * write started with plain ASCII (it then decodes with a different decoder
 * state). Program output arrives in arbitrary chunks, so the unfinished end
 * of a chunk is kept back and sent together with the next one. */
static void vt_write(vtw_t *t, const char *b, size_t n)
{
    /* (new output does NOT end scrolling back: only a key does - see flush_damage) */
    t->changes++;
    while (t->carry_len && n) {                   /* finish the kept-back character */
        unsigned char c = (unsigned char)*b;
        if ((c & 0xc0) != 0x80) {                 /* not a continuation byte: pass it on broken */
            vterm_input_write(t->vt, t->carry, (size_t)t->carry_len);
            t->carry_len = 0;
            break;
        }
        t->carry[t->carry_len++] = (char)c;
        b++; n--;
        if ((size_t)t->carry_len == utf8_len((unsigned char)t->carry[0])) {
            vterm_input_write(t->vt, t->carry, (size_t)t->carry_len);
            t->carry_len = 0;
        }
    }
    if (!n) return;
    size_t keep = 0;                              /* does the chunk end inside a character? */
    for (size_t k = 1; k <= 3 && k <= n; k++) {
        unsigned char c = (unsigned char)b[n - k];
        if ((c & 0xc0) == 0x80) continue;         /* continuation byte: look further back */
        if (c >= 0xc0 && utf8_len(c) > k) keep = k;
        break;
    }
    vterm_input_write(t->vt, b, n - keep);
    memcpy(t->carry, b + n - keep, keep);
    t->carry_len = (int)keep;
}

/* ------------------------------------------------------------- drawing */

typedef struct {
    lv_draw_ctx_t *ctx;
    lv_draw_rect_dsc_t rect;
} painter_t;

static void fill(painter_t *p, int x1, int y1, int x2, int y2, lv_color_t c)
{
    lv_area_t a = { (lv_coord_t)x1, (lv_coord_t)y1, (lv_coord_t)x2, (lv_coord_t)y2 };
    p->rect.bg_color = c;
    lv_draw_rect(p->ctx, &p->rect, &a);
}

/* Box drawing U+2500..U+257F: each character is described by its four arms
 * (left, right, up, down), each 0 = none, 1 = light, 2 = heavy, 3 = double. */
#define B(l, r, u, d) ((l) | (r) << 2 | (u) << 4 | (d) << 6)
static const uint8_t box_arms[128] = {
    B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2), B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2), /* 2500 */
    B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2), B(0,1,0,1), B(0,2,0,1), B(0,1,0,2), B(0,2,0,2), /* 2508 */
    B(1,0,0,1), B(2,0,0,1), B(1,0,0,2), B(2,0,0,2), B(0,1,1,0), B(0,2,1,0), B(0,1,2,0), B(0,2,2,0), /* 2510 */
    B(1,0,1,0), B(2,0,1,0), B(1,0,2,0), B(2,0,2,0), B(0,1,1,1), B(0,2,1,1), B(0,1,2,1), B(0,1,1,2), /* 2518 */
    B(0,1,2,2), B(0,2,2,1), B(0,2,1,2), B(0,2,2,2), B(1,0,1,1), B(2,0,1,1), B(1,0,2,1), B(1,0,1,2), /* 2520 */
    B(1,0,2,2), B(2,0,2,1), B(2,0,1,2), B(2,0,2,2), B(1,1,0,1), B(2,1,0,1), B(1,2,0,1), B(2,2,0,1), /* 2528 */
    B(1,1,0,2), B(2,1,0,2), B(1,2,0,2), B(2,2,0,2), B(1,1,1,0), B(2,1,1,0), B(1,2,1,0), B(2,2,1,0), /* 2530 */
    B(1,1,2,0), B(2,1,2,0), B(1,2,2,0), B(2,2,2,0), B(1,1,1,1), B(2,1,1,1), B(1,2,1,1), B(2,2,1,1), /* 2538 */
    B(1,1,2,1), B(1,1,1,2), B(1,1,2,2), B(2,1,2,1), B(1,2,2,1), B(2,1,1,2), B(1,2,1,2), B(2,2,2,1), /* 2540 */
    B(2,2,1,2), B(2,1,2,2), B(1,2,2,2), B(2,2,2,2), B(1,1,0,0), B(2,2,0,0), B(0,0,1,1), B(0,0,2,2), /* 2548 */
    B(3,3,0,0), B(0,0,3,3), B(0,3,0,1), B(0,1,0,3), B(0,3,0,3), B(3,0,0,1), B(1,0,0,3), B(3,0,0,3), /* 2550 */
    B(0,3,1,0), B(0,1,3,0), B(0,3,3,0), B(3,0,1,0), B(1,0,3,0), B(3,0,3,0), B(0,3,1,1), B(0,1,3,3), /* 2558 */
    B(0,3,3,3), B(3,0,1,1), B(1,0,3,3), B(3,0,3,3), B(3,3,0,1), B(1,1,0,3), B(3,3,0,3), B(3,3,1,0), /* 2560 */
    B(1,1,3,0), B(3,3,3,0), B(3,3,1,1), B(1,1,3,3), B(3,3,3,3), B(0,1,0,1), B(1,0,0,1), B(1,0,1,0), /* 2568 */
    B(0,1,1,0), 0,          0,          0,          B(1,0,0,0), B(0,0,1,0), B(0,1,0,0), B(0,0,0,1), /* 2570 */
    B(2,0,0,0), B(0,0,2,0), B(0,2,0,0), B(0,0,0,2), B(1,2,0,0), B(0,0,1,2), B(2,1,0,0), B(0,0,2,1), /* 2578 */
};

/* Box geometry inside the 8 x 16 cell (to match the font's 1 px wide,
 * 2 px tall strokes):
 *   vertical light line x=3, heavy x=3..4, double x=2 and x=5
 *   horizontal light line y=7..8, heavy y=6..9, double y=5..6 and y=9..10 */
static void draw_box(painter_t *p, uint32_t cp, int x, int y, lv_color_t fg)
{
    int a = box_arms[cp - 0x2500];
    int g = y + PAD;                              /* (the letter area; lines still span the whole row) */
    int L = a & 3, R = (a >> 2) & 3, U = (a >> 4) & 3, D = (a >> 6) & 3;
    int dbl_h = (L == 3 || R == 3), dbl_v = (U == 3 || D == 3);
    int dashes = 0;
    if (cp >= 0x2504 && cp <= 0x2507) dashes = 3;
    else if (cp >= 0x2508 && cp <= 0x250b) dashes = 4;
    else if (cp >= 0x254c && cp <= 0x254f) dashes = 2;
    int round = (cp >= 0x256d && cp <= 0x2570);   /* rounded corners: arms stop short and meet diagonally */

    if (cp >= 0x2571 && cp <= 0x2573) {           /* diagonals */
        for (int i = 0; i < 8; i++) {
            if (cp != 0x2572) fill(p, x + 7 - i, g + 2 * i, x + 7 - i, g + 2 * i + 1, fg);   /* / */
            if (cp != 0x2571) fill(p, x + i, g + 2 * i, x + i, g + 2 * i + 1, fg);           /* \ */
        }
        return;
    }
    if (dashes) {                                 /* dashed straight lines */
        int heavy = (L == 2 || U == 2);
        for (int i = 0; i < dashes; i++) {
            if (L) {
                int s = i * CW / dashes, e = (i + 1) * CW / dashes - (dashes == 4 ? 1 : 2);
                fill(p, x + s, g + (heavy ? 6 : 7), x + e, g + (heavy ? 9 : 8), fg);
            } else {
                int s = i * CH / dashes, e = (i + 1) * CH / dashes - 2;
                fill(p, x + 3, y + s, x + (heavy ? 4 : 3), y + e, fg);
            }
        }
        return;
    }

    /* single (light/heavy) arms */
    int hy1 = 7, hy2 = 8;                         /* where the horizontal line is */
    if (L == 2 || R == 2) { hy1 = 6; hy2 = 9; }
    if (L == 1 || L == 2) {
        int y1 = (L == 2) ? 6 : 7, y2 = (L == 2) ? 9 : 8;
        int end = (U == 2 || D == 2) ? 4 : 3;     /* up to the far edge of the vertical line */
        if (dbl_v && !R) end = (U && D) ? 2 : 5;  /* stop at the near / far double line */
        if (round) end = 2;
        fill(p, x, g + y1, x + end, g + y2, fg);
    }
    if (R == 1 || R == 2) {
        int y1 = (R == 2) ? 6 : 7, y2 = (R == 2) ? 9 : 8;
        int start = 3;
        if (dbl_v && !L) start = (U && D) ? 5 : 2;
        if (round) start = 4;
        fill(p, x + start, g + y1, x + 7, g + y2, fg);
    }
    if (U == 1 || U == 2) {
        int end = (hy2 > 8) ? hy2 : 8;
        if (dbl_h && !D) end = (L && R) ? 6 : 10;
        if (round) end = 6;
        fill(p, x + 3, y, x + ((U == 2) ? 4 : 3), g + end, fg);
    }
    if (D == 1 || D == 2) {
        int start = (hy1 < 7) ? hy1 : 7;
        if (dbl_h && !U) start = (L && R) ? 9 : 5;
        if (round) start = 9;
        fill(p, x + 3, g + start, x + ((D == 2) ? 4 : 3), y + CH - 1, fg);
    }

    /* double arms: two parallel lines, each ending where it meets a line
     * of the neighbouring arm, so corners and joints come out clean */
    if (L == 3) {
        int top = U == 3 ? 2 : U ? 3 : R ? 7 : D == 3 ? 5 : D ? 3 : 4;
        int bot = D == 3 ? 2 : D ? 3 : R ? 7 : U == 3 ? 5 : U ? 3 : 4;
        fill(p, x, g + 5, x + top, g + 6, fg);
        fill(p, x, g + 9, x + bot, g + 10, fg);
    }
    if (R == 3) {
        int top = U == 3 ? 5 : U ? 3 : L ? 0 : D == 3 ? 2 : 3;
        int bot = D == 3 ? 5 : D ? 3 : L ? 0 : U == 3 ? 2 : 3;
        fill(p, x + top, g + 5, x + 7, g + 6, fg);
        fill(p, x + bot, g + 9, x + 7, g + 10, fg);
    }
    if (U == 3) {
        int left = L == 3 ? 6 : L ? hy2 : D ? 15 : R == 3 ? 10 : 8;
        int right = R == 3 ? 6 : R ? hy2 : D ? 15 : L == 3 ? 10 : 8;
        fill(p, x + 2, y, x + 2, g + left, fg);
        fill(p, x + 5, y, x + 5, g + right, fg);
    }
    if (D == 3) {
        int left = L == 3 ? 9 : L ? hy1 : U ? 0 : R == 3 ? 5 : 7;
        int right = R == 3 ? 9 : R ? hy1 : U ? 0 : L == 3 ? 5 : 7;
        fill(p, x + 2, g + left, x + 2, y + CH - 1, fg);
        fill(p, x + 5, g + right, x + 5, y + CH - 1, fg);
    }
}

/* Block elements U+2580..U+259F as rectangles */
static void draw_block(painter_t *p, uint32_t cp, int x, int y, lv_color_t fg, lv_color_t bg)
{
    if (cp == 0x2580) { fill(p, x, y, x + 7, y + CH / 2 - 1, fg); return; }  /* upper half */
    if (cp <= 0x2588) {                                                     /* lower 1/8 .. full */
        int h = (int)(cp - 0x2580) * CH / 8;
        fill(p, x, y + CH - h, x + 7, y + CH - 1, fg);
        return;
    }
    if (cp <= 0x258f) { fill(p, x, y, x + (int)(0x2590 - cp) - 1, y + CH - 1, fg); return; }  /* left 7/8 .. 1/8 */
    if (cp == 0x2590) { fill(p, x + 4, y, x + 7, y + CH - 1, fg); return; } /* right half */
    if (cp <= 0x2593) {                                                     /* shades: 25 / 50 / 75 % */
        static const lv_opa_t mix[3] = { 64, 128, 192 };
        fill(p, x, y, x + 7, y + CH - 1, lv_color_mix(fg, bg, mix[cp - 0x2591]));
        return;
    }
    if (cp == 0x2594) { fill(p, x, y, x + 7, y + 1, fg); return; }          /* upper 1/8 */
    if (cp == 0x2595) { fill(p, x + 7, y, x + 7, y + CH - 1, fg); return; } /* right 1/8 */
    /* quadrants: bit 1 upper left, 2 upper right, 4 lower left, 8 lower right */
    static const uint8_t quad[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };    /* U+2596..U+259F */
    int q = quad[cp - 0x2596];
    if (q & 1) fill(p, x,     y,          x + 3, y + CH / 2 - 1, fg);
    if (q & 2) fill(p, x + 4, y,          x + 7, y + CH / 2 - 1, fg);
    if (q & 4) fill(p, x,     y + CH / 2, x + 3, y + CH - 1,     fg);
    if (q & 8) fill(p, x + 4, y + CH / 2, x + 7, y + CH - 1,     fg);
}

/* Our own little symbols (private-use code points, never sent by programs) */
enum { G_DOT = 0xf0001, G_BULLET, G_ELLIPSIS, G_SQUARE, G_CHECK, G_UNKNOWN };

/* What to draw for a character the font does not have: an ASCII look-alike
 * (returned as < 0x80), a box/block character, one of our symbols, or the
 * "unknown" box. */
static uint32_t map_char(uint32_t c)
{
    /* accented Latin-1 letters -> the plain letter */
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (c >= 0x2500 && c <= 0x259f) return c;
    if (c >= 0xc0 && c <= 0xff) return (uint32_t)latin1[c - 0xc0];
    switch (c) {
    case 0xa0: return ' ';
    case 0xa6: return '|';
    case 0xab: case 0x2039: case 0x2190: case 0x25c0: case 0x25c2: case 0x25c4: case 0x2264: return '<';
    case 0xbb: case 0x203a: case 0x2192: case 0x25b6: case 0x25b8: case 0x25ba: case 0x276f:
    case 0x23f5: case 0x2265: return '>';
    case 0xb0: return 'o';
    case 0xb1: return '+';
    case 0x2018: case 0x2019: case 0x201b: case 0x2032: case 0xb4: return '\'';
    case 0x201c: case 0x201d: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2191: case 0x25b2: case 0x25b3: return '^';
    case 0x2193: case 0x25bc: case 0x25bd: return 'v';
    case 0xd7: case 0x2715: case 0x2716: case 0x2717: case 0x2718: return 'x';
    case 0x2260: return '#';
    case 0x2217: case 0x22c6: case 0x2722: case 0x2723: case 0x2724: case 0x2725: case 0x2731:
    case 0x2732: case 0x2733: case 0x2734: case 0x2735: case 0x2736: case 0x2737: case 0x2738:
    case 0x2739: case 0x273a: case 0x273b: case 0x273c: case 0x273d: return '*';
    case 0x2605: case 0x2606: return '*';
    case 0x26a0: case 0x2757: case 0x203c: return '!';   /* ⚠ warning signs */
    case 0x2139: return 'i';                    /* ℹ */
    case 0x25cb: case 0x25ef: case 0x25e6: return 'o';
    case 0x21b5: return '<';
    case 0x23bf: return 0x2514;                 /* ⎿ (Claude Code's reply marker) -> └ */
    case 0xb7: case 0x2219: case 0x22c5: return G_DOT;
    case 0x2022: case 0x25cf: case 0x23fa: case 0x25c9: case 0x2b24: case 0x25c6: case 0x25c7:
    case 0x25ca: case 0x2666: return G_BULLET;
    case 0x2026: return G_ELLIPSIS;
    case 0x25a0: case 0x25aa: case 0x25fc: case 0x25fe: return G_SQUARE;
    case 0x2713: case 0x2714: case 0x221a: return G_CHECK;
    default: return G_UNKNOWN;
    }
}

static void draw_special(painter_t *p, uint32_t cp, int x, int y, int w, lv_color_t fg, lv_color_t bg)
{
    if (cp >= 0x2500 && cp <= 0x257f) { draw_box(p, cp, x, y, fg); return; }
    if (cp >= 0x2580 && cp <= 0x259f) { draw_block(p, cp, x, y, fg, bg); return; }
    y += PAD;                                     /* the small symbols sit where the letters sit */
    switch (cp) {
    case G_DOT:      fill(p, x + 3, y + 7, x + 4, y + 8, fg); break;
    case G_BULLET:   fill(p, x + 3, y + 4, x + 4, y + 11, fg); fill(p, x + 2, y + 5, x + 5, y + 10, fg); break;
    case G_SQUARE:   fill(p, x + 2, y + 5, x + 5, y + 10, fg); break;
    case G_ELLIPSIS: for (int i = 0; i < 3; i++) fill(p, x + 1 + 3 * i, y + 12, x + 1 + 3 * i, y + 13, fg); break;
    case G_CHECK:                                 /* a small tick */
        fill(p, x + 1, y + 8, x + 1, y + 9, fg);  fill(p, x + 2, y + 10, x + 2, y + 11, fg);
        fill(p, x + 3, y + 12, x + 3, y + 13, fg); fill(p, x + 4, y + 10, x + 4, y + 11, fg);
        fill(p, x + 5, y + 7, x + 5, y + 9, fg);  fill(p, x + 6, y + 4, x + 6, y + 6, fg);
        break;
    default:                                      /* unknown: a hollow box */
        fill(p, x + 1, y + 3, x + w - 2, y + 3, fg);
        fill(p, x + 1, y + 14, x + w - 2, y + 14, fg);
        fill(p, x + 1, y + 3, x + 1, y + 14, fg);
        fill(p, x + w - 2, y + 3, x + w - 2, y + 14, fg);
        break;
    }
}

/* One libvterm cell -> ready to draw (colours worked out, odd characters mapped) */
static void resolve_cell(const vtw_t *t, const VTermScreenCell *cell, vcell_t *v)
{
    memset(v, 0, sizeof *v);
    v->ascii = ' ';
    v->fg = cell_color(t, &cell->fg, 1);
    v->bg = cell_color(t, &cell->bg, 0);
    if (cell->attrs.reverse) { lv_color_t s = v->fg; v->fg = v->bg; v->bg = s; }
    uint32_t ch = cell->chars[0];
    if (ch == (uint32_t)-1) ch = 0;               /* right half of a wide character */
    if (cell->attrs.conceal) ch = 0;
    if (ch > 0x20 && ch < 0x7f) v->ascii = (char)ch;
    else if (ch > 0x7f) {
        uint32_t m = map_char(ch);
        if (m < 0x80) v->ascii = (char)m;
        else { v->cp = m; v->flags |= CF_SPECIAL; }
    }
    if (cell->width == 2) v->flags |= CF_WIDE;
    if (cell->attrs.bold) v->flags |= CF_BOLD;
    if (cell->attrs.underline == VTERM_UNDERLINE_DOUBLE) v->flags |= CF_UL2;
    else if (cell->attrs.underline) v->flags |= CF_UL;
    if (cell->attrs.strike) v->flags |= CF_STRIKE;
}

/* Read row r, columns c0..c1, from libvterm into t->cells, ready to draw. */
static void resolve_row(vtw_t *t, int r, int c0, int c1)
{
    VTermScreenCell cell;
    VTermPos pos = { r, 0 };
    /* the block cursor is on this row (not shown while scrolled back) */
    int cursor = t->cur_visible && !t->sb_offset && t->cur_shape <= VTERM_PROP_CURSORSHAPE_BLOCK && r == t->cur_row;
    for (int c = c0; c <= c1; c++) {
        vcell_t *v = &t->cells[c];
        pos.col = c;
        if (!vterm_screen_get_cell(t->screen, pos, &cell)) {
            memset(v, 0, sizeof *v);
            v->ascii = ' ';
            v->fg = t->fg;
            v->bg = t->bg;
            continue;
        }
        resolve_cell(t, &cell, v);
        /* block cursor: the cell in reverse colours */
        if (cursor && (c == t->cur_col || (c == t->cur_col + 1 && c > c0 && (t->cells[c - 1].flags & CF_WIDE)))) {
            lv_color_t s = v->fg; v->fg = v->bg; v->bg = s;
        }
    }
}

/* ---------------------------------------------- kept lines (scrollback)
 * When a line scrolls off the top of the screen, libvterm hands it to
 * sb_pushline() and we keep it, so it can be scrolled back to. Each line is
 * packed (sbline_t, see the top): 2 bytes per letter + 8 per colour run, and
 * blanks at the end are left out - a line of plain text costs about 2 bytes
 * per letter + 20. At most SB_MAX_LINES lines and SB_MAX_BYTES bytes: when
 * full, the oldest line goes. Full-screen programs (the alternate screen)
 * keep nothing - libvterm only hands over lines of the normal screen. */
#define SB_MAX_LINES 500
#define SB_MAX_BYTES (100 * 1024)
#define SB_MALLOC_EXTRA 8         /* what malloc itself uses per block (counted too) */
#define SB_SYMBOL 0xe000          /* codes from 0xe000 on are our symbols G_DOT, G_BULLET... */

/* our symbols as Unicode again (G_DOT .. G_UNKNOWN) */
static const uint32_t symbol_char[] = { 0xb7, 0x2022, 0x2026, 0x25a0, 0x2713, 0xfffd };

static sbrun_t *sb_runs(sbline_t *l) { return (sbrun_t *)(l + 1); }
static uint16_t *sb_chars(sbline_t *l) { return (uint16_t *)(sb_runs(l) + l->nruns); }
static size_t sb_size(const sbline_t *l)
{
    return sizeof *l + l->nruns * sizeof(sbrun_t) + l->len * sizeof(uint16_t) + SB_MALLOC_EXTRA;
}
static sbline_t *sb_line(const vtw_t *t, int i) { return t->sb[(t->sb_first + i) % SB_MAX_LINES]; }   /* 0 = oldest */

/* a drawn cell -> its 16-bit code, and back */
static uint16_t sb_code(const vcell_t *v)
{
    if (!(v->flags & CF_SPECIAL)) return (uint8_t)v->ascii;
    if (v->cp >= G_DOT) return (uint16_t)(SB_SYMBOL + (v->cp - G_DOT));
    return (uint16_t)v->cp;                       /* box drawing / blocks: U+2500..U+259F */
}
static uint32_t sb_unicode(uint16_t ch) { return ch >= SB_SYMBOL ? symbol_char[ch - SB_SYMBOL] : ch; }

/* The code kept for one cell. A letter our font lacks (Arabic, Claude Code's
 * reply markers...) keeps its own Unicode code, so the Claude app's chat view
 * can still read it later; it is mapped again when drawn (resolve_kept). */
static uint16_t sb_keep_code(const VTermScreenCell *cell, const vcell_t *v)
{
    uint32_t u = cell->chars[0];
    if (u > 0x7f && u < SB_SYMBOL && !cell->attrs.conceal) return (uint16_t)u;
    return sb_code(v);
}

/* a cell that shows nothing: a space on the default background */
static int sb_blank(const vtw_t *t, const vcell_t *v)
{
    return v->ascii == ' ' && !(v->flags & (CF_SPECIAL | CF_UL | CF_UL2 | CF_STRIKE)) && v->bg.full == t->bg.full;
}

/* do two cells have the same colours and style (= the same run)? */
static int same_look(const vcell_t *a, const vcell_t *b)
{
    return a->fg.full == b->fg.full && a->bg.full == b->bg.full && ((a->flags ^ b->flags) & ~CF_SPECIAL) == 0;
}

static void sb_drop_oldest(vtw_t *t)
{
    sbline_t *l = t->sb[t->sb_first];
    t->sb_bytes -= sb_size(l);
    free(l);
    t->sb[t->sb_first] = NULL;
    t->sb_first = (t->sb_first + 1) % SB_MAX_LINES;
    t->sb_count--;
    if (t->sb_offset > t->sb_count) t->sb_offset = t->sb_count;
}

static int sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    vtw_t *t = user;
    vcell_t v, prev;
    int len = 0, nruns = 0, k = -1;
    memset(&prev, 0, sizeof prev);
    for (int c = 0; c < cols; c++) {              /* 1) its length without the blanks at the end */
        resolve_cell(t, &cells[c], &v);
        if (!sb_blank(t, &v)) len = c + 1;
    }
    for (int c = 0; c < len; c++) {               /* 2) how many colour runs */
        resolve_cell(t, &cells[c], &v);
        if (c == 0 || !same_look(&v, &prev)) nruns++;
        prev = v;
    }
    sbline_t *l = malloc(sizeof *l + (size_t)nruns * sizeof(sbrun_t) + (size_t)len * sizeof(uint16_t));
    if (!l) return 0;                             /* out of memory: this line is lost */
    l->len = (uint16_t)len;
    l->nruns = (uint16_t)nruns;
    sbrun_t *runs = sb_runs(l);
    uint16_t *ch = sb_chars(l);
    for (int c = 0; c < len; c++) {               /* 3) fill in the runs and the letters */
        resolve_cell(t, &cells[c], &v);
        if (c == 0 || !same_look(&v, &prev)) {
            k++;
            runs[k].start = (uint16_t)c;
            runs[k].fg = v.fg;
            runs[k].bg = v.bg;
            runs[k].flags = (uint8_t)(v.flags & ~CF_SPECIAL);
        }
        ch[c] = sb_keep_code(&cells[c], &v);
        prev = v;
    }

    if (!t->sb) t->sb = calloc(SB_MAX_LINES, sizeof *t->sb);    /* the ring, at the first kept line */
    if (!t->sb) { free(l); return 0; }
    while (t->sb_count && (t->sb_count == SB_MAX_LINES || t->sb_bytes + sb_size(l) > SB_MAX_BYTES))
        sb_drop_oldest(t);                        /* full: the oldest line makes room */
    t->sb[(t->sb_first + t->sb_count) % SB_MAX_LINES] = l;
    t->sb_count++;
    t->sb_bytes += sb_size(l);
    /* scrolled back: one more line went up, so look one more line back - the
     * reader keeps seeing the same lines */
    if (t->sb_offset > 0 && t->sb_offset < t->sb_count) t->sb_offset++;
    return 1;
}

/* our colour -> libvterm's (the default colours stay "default") */
static void sb_color(const vtw_t *t, lv_color_t c, VTermColor *out, int is_fg)
{
    lv_color32_t c32;
    c32.full = lv_color_to32(c);
    vterm_color_rgb(out, c32.ch.red, c32.ch.green, c32.ch.blue);
    if (is_fg && c.full == t->fg.full) out->type |= VTERM_COLOR_DEFAULT_FG;
    if (!is_fg && c.full == t->bg.full) out->type |= VTERM_COLOR_DEFAULT_BG;
}

static int sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    vtw_t *t = user;
    if (!t->sb_count) return 0;
    sbline_t *l = sb_line(t, t->sb_count - 1);    /* the newest */
    sbrun_t *runs = sb_runs(l);
    uint16_t *ch = sb_chars(l);
    int k = 0;
    for (int c = 0; c < cols; c++) {
        VTermScreenCell *cell = &cells[c];
        memset(cell, 0, sizeof *cell);
        cell->width = 1;                          /* (libvterm steps by the width: never 0) */
        if (c >= l->len) {                        /* past the end: blank */
            sb_color(t, t->fg, &cell->fg, 1);
            sb_color(t, t->bg, &cell->bg, 0);
            continue;
        }
        while (k + 1 < l->nruns && runs[k + 1].start <= c) k++;
        sb_color(t, runs[k].fg, &cell->fg, 1);
        sb_color(t, runs[k].bg, &cell->bg, 0);
        uint8_t f = runs[k].flags;
        cell->attrs.bold = (f & CF_BOLD) != 0;
        cell->attrs.underline = (f & CF_UL2) ? VTERM_UNDERLINE_DOUBLE : (f & CF_UL) ? VTERM_UNDERLINE_SINGLE : 0;
        cell->attrs.strike = (f & CF_STRIKE) != 0;
        cell->chars[0] = sb_unicode(ch[c]);
        if ((f & CF_WIDE) && c + 1 < cols) cell->width = 2;
    }
    t->sb_count--;                                /* it is on the screen again */
    t->sb_bytes -= sb_size(l);
    t->sb[(t->sb_first + t->sb_count) % SB_MAX_LINES] = NULL;
    free(l);
    if (t->sb_offset > t->sb_count) t->sb_offset = t->sb_count;
    return 1;
}

static int sb_clear(void *user)
{
    vtw_t *t = user;
    scroll_live(t);
    while (t->sb_count) sb_drop_oldest(t);
    return 1;
}

/* Kept line l, columns c0..c1, into t->cells, ready to draw */
static void resolve_kept(vtw_t *t, sbline_t *l, int c0, int c1)
{
    sbrun_t *runs = sb_runs(l);
    uint16_t *ch = sb_chars(l);
    int k = 0;
    for (int c = c0; c <= c1; c++) {
        vcell_t *v = &t->cells[c];
        memset(v, 0, sizeof *v);
        v->ascii = ' ';
        v->fg = t->fg;
        v->bg = t->bg;
        if (c >= l->len) continue;                /* past the end: blank */
        while (k + 1 < l->nruns && runs[k + 1].start <= c) k++;
        v->fg = runs[k].fg;
        v->bg = runs[k].bg;
        v->flags = runs[k].flags;
        if (ch[c] < 0x80) v->ascii = (char)ch[c];
        else if (ch[c] >= SB_SYMBOL) {                    /* one of our symbols */
            v->cp = G_DOT + (uint32_t)(ch[c] - SB_SYMBOL);
            v->flags |= CF_SPECIAL;
        } else {                                          /* a kept Unicode letter: drawn as on the live screen */
            uint32_t m = map_char(ch[c]);
            if (m < 0x80) v->ascii = (char)m;
            else { v->cp = m; v->flags |= CF_SPECIAL; }
        }
    }
}

/* Draw one row from t->cells (filled by resolve_row or resolve_kept).
 * r = the screen row (for the cursor), -1 for a kept line. */
static void draw_row(vtw_t *t, painter_t *p, lv_draw_label_dsc_t *ld, int r, int c0, int c1, int x0, int y)
{
    vcell_t *cl = t->cells;

    /* 1) backgrounds that differ from the default (already filled), in runs */
    for (int c = c0; c <= c1;) {
        int e = c;
        while (e < c1 && cl[e + 1].bg.full == cl[c].bg.full) e++;
        if (cl[c].bg.full != t->bg.full) fill(p, x0 + c * CW, y, x0 + (e + 1) * CW - 1, y + CH - 1, cl[c].bg);
        c = e + 1;
    }

    /* 2) characters we draw ourselves */
    for (int c = c0; c <= c1; c++)
        if (cl[c].flags & CF_SPECIAL)
            draw_special(p, cl[c].cp, x0 + c * CW, y, (cl[c].flags & CF_WIDE) ? 2 * CW : CW, cl[c].fg, cl[c].bg);

    /* 3) text, in runs of the same colour and boldness; blanks in between
     *    don't break a run (they are drawn as spaces, which draw nothing) */
    for (int c = c0; c <= c1;) {
        if (cl[c].ascii == ' ') { c++; continue; }
        int last = c, n = 0;
        for (int k = c + 1; k <= c1; k++) {
            if (cl[k].ascii == ' ') continue;
            if (cl[k].fg.full != cl[c].fg.full || (cl[k].flags & CF_BOLD) != (cl[c].flags & CF_BOLD)) break;
            last = k;
        }
        for (int k = c; k <= last; k++) t->text[n++] = cl[k].ascii;
        t->text[n] = 0;
        lv_area_t a = { (lv_coord_t)(x0 + c * CW), (lv_coord_t)(y + PAD - FONT_TOP),
                        (lv_coord_t)(x0 + (last + 2) * CW - 1), (lv_coord_t)(y + CH - 1) };
        ld->color = cl[c].fg;
        lv_draw_label(p->ctx, ld, &a, t->text, NULL);
        if (cl[c].flags & CF_BOLD) {          /* fake bold: the same text 1 pixel to the right */
            a.x1++; a.x2++;
            lv_draw_label(p->ctx, ld, &a, t->text, NULL);
        }
        c = last + 1;
    }

    /* 4) underline and strike-through */
    for (int c = c0; c <= c1; c++) {
        uint8_t f = cl[c].flags;
        int x = x0 + c * CW;
        int g = y + PAD;                      /* the letters' top */
        if (f & (CF_UL | CF_UL2)) fill(p, x, g + 15, x + CW - 1, g + 15, cl[c].fg);
        if (f & CF_UL2) fill(p, x, g + 13, x + CW - 1, g + 13, cl[c].fg);
        if (f & CF_STRIKE) fill(p, x, g + 7, x + CW - 1, g + 8, cl[c].fg);
    }

    /* 5) underline / bar cursor (the block cursor was done by reversing colours) */
    if (t->cur_visible && !t->sb_offset && r == t->cur_row && t->cur_col >= c0 && t->cur_col <= c1 &&
        t->cur_shape > VTERM_PROP_CURSORSHAPE_BLOCK) {
        int x = x0 + t->cur_col * CW;
        lv_color_t fg = cl[t->cur_col].fg;
        if (t->cur_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE) fill(p, x, y + PAD + 14, x + CW - 1, y + PAD + 15, fg);
        else fill(p, x, y, x + 1, y + CH - 1, fg);
    }
}

static void draw_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    vtw_t *t = get(obj);
    if (!t) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t o, clip;
    lv_obj_get_coords(obj, &o);
    if (!_lv_area_intersect(&clip, ctx->clip_area, &o)) return;

    painter_t p;
    p.ctx = ctx;
    lv_draw_rect_dsc_init(&p.rect);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_nzeus_mono_16;

    fill(&p, clip.x1, clip.y1, clip.x2, clip.y2, t->bg);    /* default background, once */

    int c0 = (clip.x1 - o.x1) / CW, c1 = (clip.x2 - o.x1) / CW;
    int r0 = (clip.y1 - o.y1) / CH, r1 = (clip.y2 - o.y1) / CH;
    if (c0 > 0) c0--;                     /* a wide character may start one cell to the left */
    if (c1 >= t->cols) c1 = t->cols - 1;
    if (r1 >= t->rows) r1 = t->rows - 1;

    const lv_area_t *saved = ctx->clip_area;
    for (int r = r0; r <= r1; r++) {
        /* draw each row clipped to itself: the font's label box is 18 px tall and starts above the row */
        lv_area_t row = { clip.x1, (lv_coord_t)(o.y1 + r * CH), clip.x2, (lv_coord_t)(o.y1 + r * CH + CH - 1) };
        lv_area_t rc;
        if (!_lv_area_intersect(&rc, &row, &clip)) continue;
        ctx->clip_area = &rc;
        /* scrolled back by sb_offset lines: the top rows show kept lines */
        int line = r - t->sb_offset;              /* the screen row shown here (< 0: a kept line) */
        if (line < 0) resolve_kept(t, sb_line(t, t->sb_count + line), c0, c1);
        else resolve_row(t, line, c0, c1);
        draw_row(t, &p, &ld, line < 0 ? -1 : line, c0, c1, o.x1, o.y1 + r * CH);
    }
    ctx->clip_area = saved;
}

static void cover_event(lv_event_t *e)     /* we paint every pixel: LVGL can skip what is behind us */
{
    lv_cover_check_info_t *info = lv_event_get_param(e);
    if (info->res == LV_COVER_RES_MASKED) return;
    lv_area_t o;
    lv_obj_get_coords(lv_event_get_target(e), &o);
    info->res = _lv_area_is_in(info->area, &o, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
}

/* ------------------------------------------------------- the program */

static void close_child_fds(vtw_t *t)
{
    if (t->fd_in >= 0 && t->fd_in != t->fd_out) close(t->fd_in);
    if (t->fd_out >= 0) close(t->fd_out);
    t->fd_in = t->fd_out = -1;
}

static void stop_child(vtw_t *t)
{
    if (t->pid > 0) {
        kill(-t->pid, SIGHUP);
        kill(t->pid, SIGHUP);
        usleep(20000);
        if (waitpid(t->pid, NULL, WNOHANG) == 0) {   /* still there: force it */
            kill(-t->pid, SIGKILL);
            kill(t->pid, SIGKILL);
            waitpid(t->pid, NULL, 0);
        }
        t->pid = -1;
    }
    close_child_fds(t);
}

static void delete_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    vtw_t *t = get(obj);
    if (!t) return;
    stop_child(t);
    vterm_free(t->vt);
    while (t->sb_count) sb_drop_oldest(t);        /* the kept lines */
    free(t->sb);
    free(t->dirty_lo); free(t->dirty_hi); free(t->cells); free(t->text); free(t->outq);
    free(t);
    lv_obj_set_user_data(obj, NULL);
}

/* (re)allocate the per-row and per-column arrays */
static int alloc_rows(vtw_t *t, int cols, int rows)
{
    uint16_t *lo = realloc(t->dirty_lo, (size_t)rows * sizeof *lo);
    if (lo) t->dirty_lo = lo;
    uint16_t *hi = realloc(t->dirty_hi, (size_t)rows * sizeof *hi);
    if (hi) t->dirty_hi = hi;
    vcell_t *cells = realloc(t->cells, (size_t)cols * sizeof *cells);
    if (cells) t->cells = cells;
    char *text = realloc(t->text, (size_t)cols + 1);
    if (text) t->text = text;
    if (!lo || !hi || !cells || !text) return -1;
    t->cols = cols;
    t->rows = rows;
    for (int r = 0; r < rows; r++) { t->dirty_lo[r] = 0xffff; t->dirty_hi[r] = 0; }
    return 0;
}

lv_obj_t *vtw_create(lv_obj_t *parent, int cols, int rows)
{
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000) return NULL;
    vtw_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->pid = -1;
    t->fd_in = t->fd_out = -1;
    t->exit_code = -1;
    t->cur_visible = 1;
    t->cur_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
    t->fg = rgb_to_color(0xe5e5e5);
    t->bg = rgb_to_color(0x000000);
    make_palette(t);
    t->vt = vterm_new(rows, cols);
    if (!t->vt || alloc_rows(t, cols, rows) != 0) {
        if (t->vt) vterm_free(t->vt);
        free(t->dirty_lo); free(t->dirty_hi); free(t->cells); free(t->text); free(t);
        return NULL;
    }
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, output_cb, t);
    t->screen = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->screen, &screen_cbs, t);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_screen_reset(t->screen, 1);

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);                 /* no theme background/border/padding */
    lv_obj_set_size(obj, cols * CW, rows * CH);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(obj, t);
    t->obj = obj;
    lv_obj_add_event_cb(obj, draw_event, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, cover_event, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(obj, delete_event, LV_EVENT_DELETE, NULL);
    return obj;
}

void vtw_feed(lv_obj_t *vt, const char *bytes, size_t len)
{
    vtw_t *t = get(vt);
    if (!t || !len) return;
    vt_write(t, bytes, len);
    flush_damage(t);
}

size_t vtw_take_input(lv_obj_t *vt, char *buf, size_t max)
{
    vtw_t *t = get(vt);
    if (!t || t->fd_out >= 0) return 0;           /* a spawned program gets it via vtw_poll */
    size_t n = t->outq_len < max ? t->outq_len : max;
    memcpy(buf, t->outq, n);
    memmove(t->outq, t->outq + n, t->outq_len - n);
    t->outq_len -= n;
    return n;
}

/* pipe mode: show typed text ourselves (no terminal driver does it) */
static void local_echo(vtw_t *t, const char *s, size_t n)
{
    vt_write(t, s, n);
    flush_damage(t);
}

/* pipe mode: a tiny line editor, because without a PTY nobody else
 * handles Backspace, echo, Ctrl+C or Ctrl+D */
static void pipe_key(vtw_t *t, int key, int ch, int mods)
{
    if (key == KB_CHAR && (mods & VTW_MOD_CTRL)) {
        if (ch == 'c' || ch == 'C') key = KB_CTRL_C;
        else if (ch == 'd' || ch == 'D') key = KB_CTRL_D;
        else return;
    }
    switch (key) {
    case KB_CHAR:
    case KB_TAB:
        if (key == KB_TAB) ch = '\t';
        if (ch < 32 && ch != '\t') return;
        if (t->line_len < (int)sizeof t->line - 1) {
            char c = (char)ch;
            t->line[t->line_len++] = c;
            local_echo(t, &c, 1);
        }
        break;
    case KB_BACKSPACE:
        if (t->line_len > 0) { t->line_len--; local_echo(t, "\b \b", 3); }
        break;
    case KB_ENTER:
        output_cb(t->line, (size_t)t->line_len, t);
        output_cb("\n", 1, t);
        t->line_len = 0;
        local_echo(t, "\r\n", 2);
        break;
    case KB_CTRL_C:
        kill(-t->pid, SIGINT);
        t->line_len = 0;
        local_echo(t, "^C\r\n", 4);
        break;
    case KB_CTRL_D:
        if (t->line_len) { output_cb(t->line, (size_t)t->line_len, t); t->line_len = 0; }
        else if (t->fd_in >= 0 && t->fd_in != t->fd_out) { close(t->fd_in); t->fd_in = -1; }  /* end of input */
        break;
    default: break;
    }
}

void vtw_key(lv_obj_t *vt, int key, int ch, int mods)
{
    vtw_t *t = get(vt);
    if (!t) return;
    scroll_live(t);                               /* a typed key: back to the live screen */
    if (t->pid > 0 && !t->pty && !t->raw_pipe) { pipe_key(t, key, ch, mods); return; }
    VTermModifier m = (VTermModifier)(mods & VTERM_ALL_MODS_MASK);
    switch (key) {
    case KB_CHAR:
        if (ch >= 32 && ch != 127) vterm_keyboard_unichar(t->vt, (uint32_t)ch, m);
        else if (ch > 0 && ch < 32) {             /* a raw control character */
            char c = (char)ch;
            output_cb(&c, 1, t);
        }
        break;
    case KB_ENTER:     vterm_keyboard_key(t->vt, VTERM_KEY_ENTER, m); break;
    case KB_BACKSPACE: vterm_keyboard_key(t->vt, VTERM_KEY_BACKSPACE, m); break;
    case KB_DELETE:    vterm_keyboard_key(t->vt, VTERM_KEY_DEL, m); break;
    case KB_TAB:       vterm_keyboard_key(t->vt, VTERM_KEY_TAB, m); break;
    case KB_BACKTAB:   vterm_keyboard_key(t->vt, VTERM_KEY_TAB, (VTermModifier)(m | VTERM_MOD_SHIFT)); break;
    case KB_ESC:       vterm_keyboard_key(t->vt, VTERM_KEY_ESCAPE, m); break;
    case KB_UP:        vterm_keyboard_key(t->vt, VTERM_KEY_UP, m); break;
    case KB_DOWN:      vterm_keyboard_key(t->vt, VTERM_KEY_DOWN, m); break;
    case KB_LEFT:      vterm_keyboard_key(t->vt, VTERM_KEY_LEFT, m); break;
    case KB_RIGHT:     vterm_keyboard_key(t->vt, VTERM_KEY_RIGHT, m); break;
    case KB_HOME:      vterm_keyboard_key(t->vt, VTERM_KEY_HOME, m); break;
    case KB_END:       vterm_keyboard_key(t->vt, VTERM_KEY_END, m); break;
    case KB_PGUP:      vterm_keyboard_key(t->vt, VTERM_KEY_PAGEUP, m); break;
    case KB_PGDN:      vterm_keyboard_key(t->vt, VTERM_KEY_PAGEDOWN, m); break;
    case KB_CTRL_C:    vterm_keyboard_unichar(t->vt, 'c', VTERM_MOD_CTRL); break;
    case KB_CTRL_D:    vterm_keyboard_unichar(t->vt, 'd', VTERM_MOD_CTRL); break;
    case KB_CTRL_S:    vterm_keyboard_unichar(t->vt, 's', VTERM_MOD_CTRL); break;
    case VTW_KEY_INSERT: vterm_keyboard_key(t->vt, VTERM_KEY_INS, m); break;
    default:
        if (key >= VTW_KEY_F(1) && key <= VTW_KEY_F(12))
            vterm_keyboard_key(t->vt, (VTermKey)VTERM_KEY_FUNCTION(key - VTW_KEY_F(0)), m);
        break;
    }
}

void vtw_paste(lv_obj_t *vt, const char *text, size_t len)
{
    vtw_t *t = get(vt);
    if (!t) return;
    scroll_live(t);
    if (t->pid > 0 && !t->pty && !t->raw_pipe) {  /* pipe mode: type it */
        for (size_t i = 0; i < len; i++) pipe_key(t, text[i] == '\n' ? KB_ENTER : KB_CHAR, (unsigned char)text[i], 0);
        return;
    }
    vterm_keyboard_start_paste(t->vt);            /* only sends markers if the program asked */
    output_cb(text, len, t);
    vterm_keyboard_end_paste(t->vt);
}

void vtw_set_colors(lv_obj_t *vt, uint32_t fg, uint32_t bg)
{
    vtw_t *t = get(vt);
    if (!t) return;
    t->fg = rgb_to_color(fg);
    t->bg = rgb_to_color(bg);
    lv_obj_invalidate(vt);
}

void vtw_set_palette(lv_obj_t *vt, int index, uint32_t rgb)
{
    vtw_t *t = get(vt);
    if (!t || index < 0 || index > 15) return;
    t->pal[index] = rgb_to_color(rgb);
    lv_obj_invalidate(vt);
}

static void set_winsize(vtw_t *t)
{
    if (!t->pty || t->fd_out < 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)t->rows;
    ws.ws_col = (unsigned short)t->cols;
    ws.ws_xpixel = (unsigned short)(t->cols * CW);
    ws.ws_ypixel = (unsigned short)(t->rows * CH);
    ioctl(t->fd_out, TIOCSWINSZ, &ws);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* in the child, just before exec: everything above fd 2 is not for it */
static void child_exec(char *const argv[], int pipes, int cols, int rows)
{
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd <= 0 || maxfd > 4096) maxfd = 4096;
    for (int fd = 3; fd < maxfd; fd++) close(fd);
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    if (pipes) {                                  /* no PTY to ask: tell it the size */
        char n[16];
        snprintf(n, sizeof n, "%d", cols);
        setenv("COLUMNS", n, 1);
        snprintf(n, sizeof n, "%d", rows);
        setenv("LINES", n, 1);
    } else {
        unsetenv("COLUMNS");
        unsetenv("LINES");
    }
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    execvp(argv[0], argv);
    _exit(127);
}

static int spawn_pty(vtw_t *t, char *const argv[])
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0 || !ptsname(m)) { close(m); return -1; }
    char slave[128];
    strncpy(slave, ptsname(m), sizeof slave - 1);
    slave[sizeof slave - 1] = 0;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)t->rows;
    ws.ws_col = (unsigned short)t->cols;
    pid_t pid = fork();
    if (pid < 0) { close(m); return -1; }
    if (pid == 0) {                               /* child: the PTY becomes its terminal */
        setsid();
        int s = open(slave, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        child_exec(argv, 0, t->cols, t->rows);
    }
    set_nonblock(m);
    t->pid = pid;
    t->fd_in = t->fd_out = m;
    t->pty = 1;
    return 0;
}

static int spawn_pipes(vtw_t *t, char *const argv[])
{
    int in[2], out[2];
    if (pipe(in) != 0) return -1;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return -1; }
    if (pid == 0) {
        setpgid(0, 0);                            /* own group, so Ctrl+C reaches all of it */
        dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
        child_exec(argv, 1, t->cols, t->rows);
    }
    setpgid(pid, pid);
    close(in[0]); close(out[1]);
    set_nonblock(in[1]);
    set_nonblock(out[0]);
    t->pid = pid;
    t->fd_in = in[1];
    t->fd_out = out[0];
    t->pty = 0;
    t->line_len = 0;
    return 0;
}

int vtw_spawn(lv_obj_t *vt, char *const argv[], int use_pty)
{
    vtw_t *t = get(vt);
    if (!t || !argv || !argv[0] || t->pid > 0) return -1;
    close_child_fds(t);
    /* a program that exits while we write to it must not kill us (SIGPIPE) */
    signal(SIGPIPE, SIG_IGN);
    t->exit_code = -1;
    t->outq_len = 0;
    if (use_pty && spawn_pty(t, argv) == 0) return 0;
    return spawn_pipes(t, argv);                  /* no PTYs (the console): plain pipes */
}

/* pipe mode: the program prints "\n" but the screen needs "\r\n" (a PTY
 * would do this translation) */
static void feed_translated(vtw_t *t, const char *buf, size_t n)
{
    char tmp[512];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') tmp[k++] = '\r';
        tmp[k++] = buf[i];
        if (k >= sizeof tmp - 2) { vt_write(t, tmp, k); k = 0; }
    }
    if (k) vt_write(t, tmp, k);
}

/* Pipes, but the program on the far end has a real terminal (an SSH session
 * with a remote pty, as the console has no PTYs itself): keys go out as
 * terminal bytes and the output is shown as it is. */
void vtw_set_raw_pipe(lv_obj_t *vt, int on)
{
    vtw_t *t = get(vt);
    if (t) t->raw_pipe = on;
}

void vtw_poll(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    if (!t || t->fd_out < 0) return;

    /* 1) pending input -> program */
    if (t->fd_in >= 0 && t->outq_len) {
        ssize_t w = write(t->fd_in, t->outq, t->outq_len);
        if (w > 0) {
            memmove(t->outq, t->outq + w, t->outq_len - (size_t)w);
            t->outq_len -= (size_t)w;
        } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
            t->outq_len = 0;                      /* program gone: drop it */
        }
    }

    /* 2) has it ended? (checked before reading: then everything it printed
     *    is already waiting in the pipe and gets read below) */
    if (t->pid > 0) {
        int st;
        if (waitpid(t->pid, &st, WNOHANG) == t->pid) {
            t->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
            t->pid = -1;
        }
    }

    /* 3) program output -> screen */
    char buf[4096];
    size_t total = 0;
    int eof = 0;
    while (total < POLL_BUDGET) {
        ssize_t n = read(t->fd_out, buf, sizeof buf);
        if (n > 0) {
            if (t->pty || t->raw_pipe) vt_write(t, buf, (size_t)n);
            else feed_translated(t, buf, (size_t)n);
            total += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        eof = 1;                                  /* 0 = end, EIO = PTY closed */
        break;
    }
    if (total) flush_damage(t);

    /* it ended and all its output is shown: close (a program that is still
     * running but closed its output is kept until it ends) */
    if (t->pid < 0 && (eof || total < POLL_BUDGET)) close_child_fds(t);
}

int vtw_child_running(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t && t->pid > 0;
}

int vtw_child_exit_code(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->exit_code : -1;
}

int vtw_uses_pty(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t && t->pty && t->fd_out >= 0;
}

void vtw_kill(lv_obj_t *vt, int sig)
{
    vtw_t *t = get(vt);
    if (!t || t->pid <= 0) return;
    kill(-t->pid, sig);                           /* the whole group (its own children too) */
    kill(t->pid, sig);
}

void vtw_resize(lv_obj_t *vt, int cols, int rows)
{
    vtw_t *t = get(vt);
    if (!t || cols < 1 || rows < 1 || cols > 1000 || rows > 1000) return;
    if (alloc_rows(t, cols, rows) != 0) return;
    t->sb_offset = 0;                             /* the live screen (lines may move to / from the kept ones) */
    t->changes++;
    vterm_set_size(t->vt, rows, cols);
    lv_obj_set_size(vt, cols * CW, rows * CH);
    lv_obj_update_layout(vt);                     /* new coordinates now, not at the next redraw */
    set_winsize(t);                              /* the program gets SIGWINCH */
    lv_obj_invalidate(vt);
}

void vtw_get_cursor(lv_obj_t *vt, int *row, int *col)
{
    vtw_t *t = get(vt);
    VTermPos p = { 0, 0 };
    if (t) vterm_state_get_cursorpos(vterm_obtain_state(t->vt), &p);
    if (row) *row = p.row;
    if (col) *col = p.col;
}

const char *vtw_title(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->title : "";
}

const char *vtw_row_text(lv_obj_t *vt, int row, char *buf, size_t n)
{
    vtw_t *t = get(vt);
    if (!buf || n == 0) return "";
    buf[0] = 0;
    if (!t || row < 0 || row >= t->rows) return buf;
    VTermRect r = { row, row + 1, 0, t->cols };
    size_t len = vterm_screen_get_text(t->screen, buf, n - 1, r);
    while (len > 0 && buf[len - 1] == ' ') len--;
    buf[len] = 0;
    return buf;
}

/* ---------------------------------------------- scrolling back */
int vtw_scroll(lv_obj_t *vt, int lines)
{
    vtw_t *t = get(vt);
    if (!t || t->altscreen) return 0;             /* a full-screen program: nothing to scroll */
    int s = t->sb_offset + lines;
    if (s > t->sb_count) s = t->sb_count;         /* the oldest kept line at the top: as far as it goes */
    if (s < 0) s = 0;
    if (s != t->sb_offset) {
        t->sb_offset = s;
        lv_obj_invalidate(vt);                    /* every row shows something else now */
    }
    return s;
}

int vtw_scroll_offset(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->sb_offset : 0;
}

int vtw_kept_lines(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->sb_count : 0;
}

size_t vtw_kept_bytes(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->sb_bytes : 0;
}

/* one character as UTF-8 (our codes are all below U+10000); returns its length */
static size_t utf8_put(char *o, uint32_t u)
{
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) { o[0] = (char)(0xc0 | u >> 6); o[1] = (char)(0x80 | (u & 0x3f)); return 2; }
    o[0] = (char)(0xe0 | u >> 12);
    o[1] = (char)(0x80 | ((u >> 6) & 0x3f));
    o[2] = (char)(0x80 | (u & 0x3f));
    return 3;
}

/* kept line i (0 = the oldest) as UTF-8 text, trailing blanks removed */
static const char *kept_text(vtw_t *t, int i, char *buf, size_t n)
{
    sbline_t *l = sb_line(t, i);
    uint16_t *ch = sb_chars(l);
    size_t k = 0;
    for (int c = 0; c < l->len && k + 4 < n; c++) k += utf8_put(buf + k, sb_unicode(ch[c]));
    while (k > 0 && buf[k - 1] == ' ') k--;
    buf[k] = 0;
    return buf;
}

const char *vtw_view_text(lv_obj_t *vt, int row, char *buf, size_t n)
{
    vtw_t *t = get(vt);
    if (!buf || n == 0) return "";
    buf[0] = 0;
    if (!t || row < 0 || row >= t->rows) return buf;
    int line = row - t->sb_offset;
    if (line >= 0) return vtw_row_text(vt, line, buf, n);   /* a screen row */
    return kept_text(t, t->sb_count + line, buf, n);         /* a kept line */
}

/* ---------------------------------------------- all the text (the chat view) */
int vtw_line_count(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->sb_count + t->rows : 0;
}

const char *vtw_line_text(lv_obj_t *vt, int i, char *buf, size_t n)
{
    vtw_t *t = get(vt);
    if (!buf || n == 0) return "";
    buf[0] = 0;
    if (!t || i < 0) return buf;
    if (i < t->sb_count) return kept_text(t, i, buf, n);    /* a kept line */
    return vtw_row_text(vt, i - t->sb_count, buf, n);        /* a screen row (the live screen) */
}

unsigned long vtw_changes(lv_obj_t *vt)
{
    vtw_t *t = get(vt);
    return t ? t->changes : 0;
}

/* vt_test.c - PC test for the terminal widget (vtwidget.c). Not part of the
 * console build.
 *
 * 1) automated checks: feeds known escape sequences into an 80 x 30 widget
 *    on a 640 x 480 LVGL display and checks the text, cursor, replies, keys
 *    and the drawn pixels. Prints PASS/FAIL per check; the exit code is the
 *    number of failures.
 * 2) screenshots (unless --checks-only): runs real programs through a PTY
 *    and saves pictures as .ppm files: a colour test, vim, top, and (when
 *    --claude PATH is given) the Claude Code CLI's first screen.
 *
 * Usage: vt_test OUTDIR [--checks-only] [--claude PATH_TO_CLAUDE] */
#define _GNU_SOURCE
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "vtwidget.h"
#include "usbkbd.h"

#define W 640
#define H 480
#define C VTW_CELL_H            /* one text row: 20 px */
#define P ((C - 16) / 2)        /* the air above the letters inside a row */

/* ---------------------------------------------------------- display */
static uint16_t fb[W * H];                   /* the finished picture, RGB565 */
static lv_color_t draw_rows[W * 60];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static long flushed_pixels;                  /* how much LVGL redrew (damage check) */
static int log_areas;                        /* print every redrawn rectangle */

uint32_t nzeus_tick_ms(void)                 /* LVGL's clock (lv_conf.h) */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(&fb[y * W + area->x1], colors, (size_t)w * 2);
        colors += w;
    }
    flushed_pixels += (long)w * (area->y2 - area->y1 + 1);
    if (log_areas) printf("      redrawn: x %d..%d, y %d..%d\n", area->x1, area->x2, area->y1, area->y2);
    lv_disp_flush_ready(drv);
}

static void display_init(void)
{
    lv_disp_draw_buf_init(&draw_buf, draw_rows, NULL, W * 60);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W;
    disp_drv.ver_res = H;
    disp_drv.flush_cb = flush;
    disp_drv.draw_buf = &draw_buf;
    /* HiChip's LVGL lost the software-renderer default: set it ourselves */
    disp_drv.draw_ctx_init = lv_draw_sw_init_ctx;
    disp_drv.draw_ctx_deinit = lv_draw_sw_deinit_ctx;
    disp_drv.draw_ctx_size = sizeof(lv_draw_sw_ctx_t);
    lv_disp_drv_register(&disp_drv);
}

static const char *outdir = ".";

static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = fb[i];
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("saved %s\n", path);
}

/* ---------------------------------------------------------- helpers */
static lv_obj_t *vt;

static void render(void) { lv_refr_now(NULL); }

/* let the program run for ms milliseconds */
static void pump(int ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)ms;
    do {
        vtw_poll(vt);
        lv_timer_handler();
        usleep(5000);
    } while ((int32_t)(end - nzeus_tick_ms()) > 0);
    render();
}

static int screen_has(const char *needle)
{
    char row[1024];
    for (int r = 0; r < 30; r++)
        if (strstr(vtw_row_text(vt, r, row, sizeof row), needle)) return 1;
    return 0;
}

/* wait until the screen shows needle (or the timeout) */
static int wait_text(const char *needle, int timeout_ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)timeout_ms;
    while ((int32_t)(end - nzeus_tick_ms()) > 0) {
        pump(50);
        if (screen_has(needle)) return 1;
    }
    return 0;
}

static int wait_exit(int timeout_ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)timeout_ms;
    while ((int32_t)(end - nzeus_tick_ms()) > 0) {
        pump(50);
        if (!vtw_child_running(vt)) { pump(100); return 1; }
    }
    return 0;
}

static void type(const char *s)
{
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') vtw_key(vt, KB_ENTER, 0, 0);
        else if (*s == 27) vtw_key(vt, KB_ESC, 0, 0);
        else if (*s == 8) vtw_key(vt, KB_BACKSPACE, 0, 0);
        else vtw_key(vt, KB_CHAR, (unsigned char)*s, 0);
        pump(15);
    }
}

static void feed(const char *s) { vtw_feed(vt, s, strlen(s)); }

static void reset(void)                       /* RIS: a fresh terminal, ESC[3J: no kept lines */
{
    char junk[256];
    feed("\033c\033[3J");
    while (vtw_take_input(vt, junk, sizeof junk)) {}
}

/* what row r of the view shows (a kept line while scrolled back) */
static int view_is(int r, const char *want)
{
    char buf[1024];
    vtw_view_text(vt, r, buf, sizeof buf);
    if (strcmp(buf, want) != 0) { printf("      view row %d is \"%s\", wanted \"%s\"\n", r, buf, want); return 0; }
    return 1;
}

static int fails, passes;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passes++; else fails++;
}

static int row_is(int r, const char *want)
{
    char buf[1024];
    vtw_row_text(vt, r, buf, sizeof buf);
    if (strcmp(buf, want) != 0) { printf("      row %d is \"%s\", wanted \"%s\"\n", r, buf, want); return 0; }
    return 1;
}

/* bytes the terminal wants to send to the program */
static int reply_is(const char *want)
{
    char buf[256];
    size_t n = vtw_take_input(vt, buf, sizeof buf - 1);
    buf[n] = 0;
    if (strcmp(buf, want) != 0) {
        printf("      sent \"");
        for (size_t i = 0; i < n; i++) printf(buf[i] == 27 ? "\\e" : "%c", buf[i]);
        printf("\", wanted something else\n");
        return 0;
    }
    return 1;
}

static uint16_t px(int x, int y) { return fb[y * W + x]; }
static uint16_t rgb565(int r, int g, int b) { return lv_color_make(r, g, b).full; }

static int px_is(int x, int y, uint16_t want)
{
    if (px(x, y) != want) { printf("      pixel (%d,%d) is 0x%04x, wanted 0x%04x\n", x, y, px(x, y), want); return 0; }
    return 1;
}

/* ---------------------------------------------------------- checks */
static void run_checks(void)
{
    const uint16_t FG = rgb565(0xe5, 0xe5, 0xe5), BG = rgb565(0, 0, 0);
    int r, c;

    reset();
    feed("Hello, world");
    check(row_is(0, "Hello, world"), "plain text appears on row 0");

    reset();
    feed("\033[5;10HX");
    vtw_get_cursor(vt, &r, &c);
    check(row_is(4, "         X") && r == 4 && c == 10, "CUP ESC[5;10H moves the cursor to row 4, column 9");

    reset();
    feed("h\303");                            /* a UTF-8 character split over two calls */
    feed("\251llo \342\224\214\342\224\200\342\224\220");
    check(row_is(0, "h\303\251llo \342\224\214\342\224\200\342\224\220"), "UTF-8 split between feeds (héllo ┌─┐)");

    reset();
    feed("\346\274\242x");                    /* 漢 is 2 cells wide */
    vtw_get_cursor(vt, &r, &c);
    check(row_is(0, "\346\274\242x") && c == 3, "wide character takes 2 cells");

    reset();
    feed("MAIN SCREEN");
    feed("\033[?1049h\033[2J\033[HALT SCREEN");
    int alt_ok = row_is(0, "ALT SCREEN");
    feed("\033[?1049l");
    check(alt_ok && row_is(0, "MAIN SCREEN"), "alternate screen switch, main screen restored after");

    reset();
    feed("A\r\nB\r\nC\r\nD\r\nE");
    feed("\033[2;4r\033[4;1H\n");             /* scroll region rows 2-4, LF at its bottom */
    check(row_is(0, "A") && row_is(1, "C") && row_is(2, "D") && row_is(3, "") && row_is(4, "E"),
          "scroll region: only rows 2-4 scroll");

    reset();
    feed("\033[3;7H\033[6n");
    check(reply_is("\033[3;7R"), "DSR ESC[6n answers the cursor position ESC[3;7R");
    feed("\033[c");
    check(reply_is("\033[?1;2c"), "DA ESC[c answers ESC[?1;2c");

    reset();
    vtw_key(vt, KB_UP, 0, 0);           check(reply_is("\033[A"), "key Up -> ESC[A");
    feed("\033[?1h");
    vtw_key(vt, KB_UP, 0, 0);           check(reply_is("\033OA"), "key Up in application cursor mode -> ESC O A");
    vtw_key(vt, KB_BACKTAB, 0, 0);      check(reply_is("\033[Z"), "Shift+Tab -> ESC[Z");
    vtw_key(vt, KB_CTRL_C, 0, 0);       check(reply_is("\003"), "Ctrl+C -> 0x03");
    vtw_key(vt, KB_CHAR, 'r', VTW_MOD_CTRL); check(reply_is("\022"), "Ctrl+R -> 0x12");
    vtw_key(vt, KB_ENTER, 0, 0);        check(reply_is("\r"), "Enter -> CR");
    vtw_key(vt, KB_BACKSPACE, 0, 0);    check(reply_is("\177"), "Backspace -> DEL (0x7f)");
    vtw_key(vt, KB_DELETE, 0, 0);       check(reply_is("\033[3~"), "Delete -> ESC[3~");
    vtw_key(vt, KB_PGDN, 0, 0);         check(reply_is("\033[6~"), "PgDn -> ESC[6~");
    vtw_key(vt, VTW_KEY_F(1), 0, 0);    check(reply_is("\033OP"), "F1 -> ESC O P");
    vtw_key(vt, VTW_KEY_F(5), 0, 0);    check(reply_is("\033[15~"), "F5 -> ESC[15~");
    vtw_key(vt, KB_ESC, 0, 0);          check(reply_is("\033"), "Esc -> ESC");
    feed("\033[?2004h");
    vtw_paste(vt, "hi", 2);             check(reply_is("\033[200~hi\033[201~"), "bracketed paste markers");

    reset();
    vtw_resize(vt, 40, 10);
    feed("0123456789012345678901234567890123456789wrapped");
    int rs_ok = lv_obj_get_width(vt) == 320 && lv_obj_get_height(vt) == 10 * C && row_is(1, "wrapped");
    vtw_resize(vt, 80, 30);
    check(rs_ok && lv_obj_get_width(vt) == 640, "vtw_resize: 40 x 10 cells = 320 x 200 px, text wraps at 40");

    reset();
    feed("\033]0;My Title\007");
    check(strcmp(vtw_title(vt), "My Title") == 0, "window title (OSC 0)");

    /* ---- pixels */
    reset();
    feed("\033[1;1H\033[48;5;67m  \033[48;5;245m  \033[41m  \033[0m");
    feed("\033[2;1H\033[38;2;10;200;30m\342\226\210\033[0m");       /* █ in truecolour */
    feed("\033[3;1H\033[7m \033[0m");                               /* reverse */
    feed("\033[4;1H\033[4mU\033[0m");                               /* underline */
    feed("\033[5;1H\342\224\214\342\224\200\342\224\220");          /* ┌─┐ */
    feed("\033[6;1H\342\225\224\342\225\220\342\225\227");          /* ╔═╗ */
    feed("\033[7;1H\342\226\200\342\226\204\342\226\210\342\226\221"); /* ▀▄█░ */
    feed("\033[8;1HT_");
    feed("\033[20;1H");
    render();
    check(px_is(4, 8, rgb565(95, 135, 175)), "256-colour 67 = rgb(95,135,175) as RGB565");
    check(px_is(20, 8, rgb565(138, 138, 138)), "256-colour 245 (grey) = rgb(138,138,138)");
    check(px_is(36, 8, rgb565(0xcd, 0, 0)), "16-colour red background");
    check(px_is(4, C + 8, rgb565(10, 200, 30)), "truecolour foreground (full block)");
    check(px_is(4, 2 * C + 8, FG), "reverse video: background = default foreground");
    check(px_is(3, 3 * C + P + 15, FG) && px_is(3, 3 * C + P + 1, BG), "underline drawn at the bottom of the letters");
    /* ┌ in row 4: arm down and arm right, nothing left or up (g = the letters' top) */
    int y = 4 * C, g = y + P;
    check(px_is(3, g + 12, FG) && px_is(6, g + 7, FG) && px_is(1, g + 7, BG) && px_is(3, g + 2, BG) &&
          px_is(8 + 0, g + 7, FG) && px_is(8 + 7, g + 8, FG) && px_is(8 + 3, g + 2, BG) &&
          px_is(16 + 1, g + 7, FG) && px_is(16 + 3, g + 14, FG) && px_is(16 + 6, g + 7, BG) &&
          px_is(3, y + C - 1, FG) && px_is(16 + 3, y + C - 1, FG),     /* down arms reach the next row */
          "box drawing ┌─┐ drawn as joined lines");
    y = 5 * C; g = y + P;
    check(px_is(2, g + 12, FG) && px_is(5, g + 12, FG) && px_is(4, g + 5, FG) && px_is(0, g + 5, BG) &&
          px_is(8, g + 5, FG) && px_is(8, g + 9, FG) && px_is(8, g + 7, BG) && px_is(16 + 5, g + 12, FG),
          "double box drawing ╔═╗");
    y = 6 * C;
    check(px_is(3, y + 3, FG) && px_is(3, y + 12, BG) && px_is(8 + 3, y + 3, BG) && px_is(8 + 3, y + 12, FG) &&
          px_is(16 + 0, y + 0, FG) && px_is(16 + 7, y + C - 1, FG) &&
          px(24 + 3, y + 8) != FG && px(24 + 3, y + 8) != BG,
          "block elements ▀▄█ and shade ░ fill the whole row");
    y = 7 * C; g = y + P;     /* 'T' starts at the letters' top, '_' fills their bottom 2 rows */
    check(px_is(3, g, FG) && px_is(3, y, BG) && px_is(8 + 3, g + 15, FG) && px_is(8 + 3, y + C - 1, BG) &&
          px_is(8 + 3, g + 13, BG),
          "text rows are 20 px apart: 2 px of air above and below the letters");

    /* ---- damage: one character must not redraw the whole screen */
    render();
    flushed_pixels = 0;
    log_areas = 1;
    feed("\033[10;10HZ");
    render();
    log_areas = 0;
    printf("      one character redrew %ld pixels\n", flushed_pixels);
    check(flushed_pixels > 0 && flushed_pixels <= 4 * 16 * C, "damage: one character redraws only a few cells");
    flushed_pixels = 0;
    feed("\033[15;1Hline of text that changes a row");
    render();
    printf("      one line redrew %ld pixels\n", flushed_pixels);
    check(flushed_pixels > 0 && flushed_pixels <= 2 * 640 * C, "damage: one line redraws at most its rows");

    /* ---- scrollback: lines that scroll off the top are kept and can be shown again */
    {
        static uint16_t first_row[W * C];         /* row 0's pixels while it was on the live screen */
        char line[1024];
        reset();
        feed("\033[41mRED\033[0m first \033[1mline\033[0m \342\224\200\342\224\200\r\n");   /* red, bold, a box line */
        render();
        memcpy(first_row, fb, sizeof first_row);
        for (int i = 2; i <= 60; i++) { snprintf(line, sizeof line, "line %d\r\n", i); feed(line); }
        /* 60 lines + the cursor's empty row = 61 rows on a 30-row screen: 31 went off the top */
        check(vtw_kept_lines(vt) == 31 && vtw_scroll_offset(vt) == 0 && row_is(0, "line 32") && view_is(0, "line 32"),
              "scrollback: the 31 lines that went off the top are kept, the view is live");
        printf("      31 kept lines use %zu bytes\n", vtw_kept_bytes(vt));
        check(vtw_kept_bytes(vt) > 0 && vtw_kept_bytes(vt) < 31 * 64, "scrollback: kept lines are packed small (no blanks, no 40-byte cells)");
        check(vtw_scroll(vt, 1) == 1 && view_is(0, "line 31") && view_is(1, "line 32") && view_is(29, "line 60"),
              "scrollback: one line back shows the newest kept line on top");
        check(vtw_scroll(vt, 1000) == 31 && view_is(0, "RED first line \342\224\200\342\224\200") && view_is(29, "line 30"),
              "scrollback: scrolling back stops at the oldest kept line");
        render();
        int same = 1;
        for (int i = 0; i < W * C; i++) if (fb[i] != first_row[i]) { same = 0; printf("      pixel (%d,%d) differs\n", i % W, i / W); break; }
        check(same && px_is(0, 1, rgb565(0xcd, 0, 0)), "scrollback: a kept line is drawn exactly as it was (colours, bold, box line, 20 px row)");
        check(vtw_scroll(vt, -10) == 21 && view_is(0, "line 11"), "scrollback: scrolling forward again");
        /* while reading back, new output (e.g. Claude's busy spinner) must not move the view */
        render();
        static uint16_t before[W * H];
        memcpy(before, fb, sizeof before);
        feed("x\r\nnew A\r\nnew B");                    /* 2 more lines go off the top */
        render();
        check(vtw_scroll_offset(vt) == 23 && view_is(0, "line 11") && !memcmp(before, fb, sizeof before),
              "scrollback: new output while scrolled back leaves the view still (same lines, same pixels)");
        feed("\033[H\033[2Jall new\033[30;1Hnew C");  /* the whole live screen changes (like a redraw) */
        render();
        check(vtw_scroll_offset(vt) == 23 && !memcmp(before, fb, sizeof before),
              "scrollback: even a whole new live screen doesn't disturb the reader (no redraw while back)");
        vtw_key(vt, KB_CHAR, 'q', 0);
        render();
        check(vtw_scroll_offset(vt) == 0 && view_is(0, "all new") && view_is(29, "new C") && reply_is("q"),
              "scrollback: then a key returns to the live screen, with the new output");
        vtw_scroll(vt, 5);
        vtw_key(vt, KB_CHAR, 'a', 0);
        check(vtw_scroll_offset(vt) == 0 && reply_is("a"), "scrollback: a typed key jumps back to the live screen (and is sent)");

        /* full-screen programs (the alternate screen) keep nothing and don't scroll */
        int kept_before = vtw_kept_lines(vt);         /* 33: RED line, line 2 .. line 33 */
        feed("\033[?1049h");
        for (int i = 0; i < 40; i++) feed("alt line\r\n");
        int alt_kept = vtw_kept_lines(vt), alt_scroll = vtw_scroll(vt, 5);
        feed("\033[?1049l");
        check(kept_before == 33 && alt_kept == kept_before && alt_scroll == 0 && vtw_scroll(vt, 5) == 5 && view_is(0, "line 29"),
              "scrollback: the alternate screen keeps no lines and can't scroll; afterwards it works again");
        vtw_scroll(vt, -5);
        feed("\033[3J");
        check(vtw_kept_lines(vt) == 0 && vtw_kept_bytes(vt) == 0, "scrollback: ESC[3J forgets the kept lines");

        /* the limits: about 500 lines, about 100 KB */
        reset();
        for (int i = 1; i <= 600; i++) { snprintf(line, sizeof line, "n %d\r\n", i); feed(line); }
        int kept = vtw_kept_lines(vt);            /* 571 went off the top: the newest 500 stay */
        check(kept == 500 && vtw_scroll(vt, 1) == 1 && view_is(0, "n 571") && vtw_scroll(vt, 1000) == 500 && view_is(0, "n 72"),
              "scrollback: at most 500 lines, the oldest go first");
        reset();
        size_t heap0 = mallinfo2().uordblks;
        for (int i = 0; i < 300; i++) {           /* 80 colours per line: about 800 bytes each */
            int k = 0;
            for (int c = 0; c < 80; c++) k += snprintf(line + k, sizeof line - (size_t)k, "\033[38;5;%dm#", 16 + (c + i) % 200);
            snprintf(line + k, sizeof line - (size_t)k, "\r\n");
            feed(line);
        }
        long heap = (long)(mallinfo2().uordblks - heap0);
        printf("      colourful lines: %d kept, %zu bytes counted, heap grew %ld bytes\n", vtw_kept_lines(vt), vtw_kept_bytes(vt), heap);
        check(vtw_kept_lines(vt) > 100 && vtw_kept_lines(vt) < 200 && vtw_kept_bytes(vt) <= 100 * 1024 &&
              vtw_kept_bytes(vt) > 90 * 1024 && heap < 112 * 1024,
              "scrollback: at most 100 KB (colourful lines: fewer of them are kept)");

        /* resizing: lines pushed off by a smaller screen come back when it grows again */
        reset();
        for (int i = 0; i < 25; i++) { snprintf(line, sizeof line, "r %d\r\n", i); feed(line); }
        vtw_resize(vt, 80, 10);
        int small_kept = vtw_kept_lines(vt);
        vtw_resize(vt, 80, 30);
        check(small_kept > 0 && vtw_kept_lines(vt) == 0 && row_is(0, "r 0") && row_is(24, "r 24"),
              "scrollback: resize smaller keeps the top lines, bigger again gives them back");
        reset();
    }

    /* ---- running programs */
    reset();
    char *sh_pipe[] = { "/bin/sh", "-c", "echo ready; read x; echo got:$x", NULL };
    check(vtw_spawn(vt, sh_pipe, 0) == 0 && !vtw_uses_pty(vt), "spawn with pipes");
    int ready = wait_text("ready", 3000);
    type("abc\b\bxy\n");                      /* Backspace edits the line before it is sent */
    check(ready && wait_text("got:axy", 3000) && wait_exit(3000) && vtw_child_exit_code(vt) == 0,
          "pipe mode: local echo, Backspace, Enter sends the line");

    reset();
    char *sh_pty[] = { "/bin/sh", "-c", "stty size; tput colors; printf 'tty:'; tty", NULL };
    check(vtw_spawn(vt, sh_pty, 1) == 0 && vtw_uses_pty(vt), "spawn with a PTY");
    check(wait_text("30 80", 3000) && wait_text("256", 3000) && wait_text("tty:/dev/pts/", 3000),
          "PTY: program sees 30 x 80, 256 colours, a real tty");
    wait_exit(3000);

    reset();
    char *sleeper[] = { "/bin/sleep", "30", NULL };
    vtw_spawn(vt, sleeper, 1);
    pump(100);
    vtw_kill(vt, SIGTERM);
    check(wait_exit(3000) && vtw_child_exit_code(vt) == 128 + SIGTERM, "vtw_kill stops the program");

    printf("checks: %d passed, %d failed\n", passes, fails);
}

/* ---------------------------------------------------------- screenshots */
static const char colour_script[] =
    "printf '\\033[1mNZEUS terminal colour test\\033[0m   TERM=%s COLORTERM=%s\\n' \"$TERM\" \"$COLORTERM\"\n"
    "for i in $(seq 0 15); do printf '\\033[48;5;%dm\\033[38;5;%dm %2d  \\033[0m' $i $(( i < 7 || i == 8 ? 15 : 0 )) $i; [ $i = 7 ] && echo; done; echo\n"
    "printf '\\033[30;47mblk\\033[0m \\033[31mred \\033[32mgrn \\033[33myel \\033[34mblu \\033[35mmag \\033[36mcyn \\033[37mwht "
    "\\033[90mBLK \\033[91mRED \\033[92mGRN \\033[93mYEL \\033[94mBLU \\033[95mMAG \\033[96mCYN \\033[97mWHT\\033[0m\\n'\n"
    "for r in 0 1 2 3 4 5; do for c in $(seq 0 35); do printf '\\033[48;5;%dm  ' $((16 + r * 36 + c)); done; printf '\\033[0m\\n'; done\n"
    "for i in $(seq 232 255); do printf '\\033[48;5;%dm   ' $i; done; printf '\\033[0m\\n'\n"
    "for x in $(seq 0 79); do printf '\\033[48;2;%d;%d;%dm ' $((x * 255 / 79)) $((255 - (x < 40 ? 40 - x : x - 40) * 6)) $((255 - x * 255 / 79)); done; printf '\\033[0m\\n'\n"
    "printf 'normal \\033[1mbold\\033[0m \\033[4munderline\\033[0m \\033[21mdouble\\033[0m \\033[7mreverse\\033[0m "
    "\\033[9mstrike\\033[0m \\033[1;4;31mbold+ul+red\\033[0m \\033[38;2;255;160;0mtruecolour\\033[0m\\n'\n"
    "cat <<'EOF'\n"
    "\342\224\214\342\224\200\342\224\200\342\224\200\342\224\254\342\224\200\342\224\200\342\224\200\342\224\220 "
    "\342\225\224\342\225\220\342\225\220\342\225\220\342\225\246\342\225\220\342\225\220\342\225\220\342\225\227 "
    "\342\224\217\342\224\201\342\224\201\342\224\201\342\224\263\342\224\201\342\224\201\342\224\201\342\224\223 "
    "\342\225\255\342\224\200\342\224\200\342\224\200\342\225\256 "
    "\342\225\222\342\225\220\342\225\244\342\225\220\342\225\225 \342\225\223\342\224\200\342\225\245\342\224\200\342\225\226\n"
    "\342\224\202 a \342\224\202 b \342\224\202 \342\225\221 c \342\225\221 d \342\225\221 \342\224\203 e \342\224\203 f \342\224\203 "
    "\342\224\202 g \342\224\202 "
    "\342\225\236\342\225\220\342\225\252\342\225\220\342\225\241 \342\225\237\342\224\200\342\225\253\342\224\200\342\225\242\n"
    "\342\224\234\342\224\200\342\224\200\342\224\200\342\224\274\342\224\200\342\224\200\342\224\200\342\224\244 "
    "\342\225\240\342\225\220\342\225\220\342\225\220\342\225\254\342\225\220\342\225\220\342\225\220\342\225\243 "
    "\342\224\243\342\224\201\342\224\201\342\224\201\342\225\213\342\224\201\342\224\201\342\224\201\342\224\253 "
    "\342\225\260\342\224\200\342\224\200\342\224\200\342\225\257 "
    "\342\225\230\342\225\220\342\225\247\342\225\220\342\225\233 \342\225\231\342\224\200\342\225\250\342\224\200\342\225\234\n"
    "\342\224\224\342\224\200\342\224\200\342\224\200\342\224\264\342\224\200\342\224\200\342\224\200\342\224\230 "
    "\342\225\232\342\225\220\342\225\220\342\225\220\342\225\251\342\225\220\342\225\220\342\225\220\342\225\235 "
    "\342\224\227\342\224\201\342\224\201\342\224\201\342\224\273\342\224\201\342\224\201\342\224\201\342\224\233 "
    "\342\224\204\342\224\204\342\224\210\342\224\210\342\225\214\342\225\214 \342\225\261\342\225\262\342\225\263 "
    "\342\225\264\342\225\266 \342\225\275\342\225\277\n"
    "blocks \342\226\221\342\226\222\342\226\223\342\226\210 \342\226\200\342\226\204\342\226\214\342\226\220 "
    "\342\226\201\342\226\202\342\226\203\342\226\204\342\226\205\342\226\206\342\226\207\342\226\210 "
    "\342\226\217\342\226\216\342\226\215\342\226\214\342\226\213\342\226\212\342\226\211\342\226\210 "
    "\342\226\226\342\226\227\342\226\230\342\226\231\342\226\232\342\226\233\342\226\234\342\226\235\342\226\236\342\226\237\n"
    "symbols: h\303\251llo \303\274ber \342\200\242 \342\200\246 \342\234\223 \342\234\227 \342\206\222 \342\206\220 "
    "\342\235\257 \342\217\272 \342\216\277 \302\267 \346\274\242\345\255\227 \342\230\203 (unknown -> box)\n"
    "EOF\n"
    "o='\\033[38;2;215;119;87m'; z='\\033[0m'\n"
    "printf \" ${o}\342\226\220\342\226\233\342\226\210\342\226\210\342\226\210\342\226\234\342\226\214${z}   Claude logo in blocks\\n\"\n"
    "printf \"${o}\342\226\235\342\226\234\342\226\210\342\226\210\342\226\210\342\226\210\342\226\210\342\226\233\342\226\230${z}\\n\"\n"
    "printf \"  ${o}\342\226\230\342\226\230 \342\226\235\342\226\235${z}\\n\"\n"
    "printf '\\033[48;5;236m\\033[38;5;214m on a 236 grey background: \\033[1mbold\\033[22m and \\033[4munderlined\\033[24m \\033[0m'\n";

static const char demo_c[] =
    "/* demo.c - a small C file for the vim screenshot */\n"
    "#include <stdio.h>\n"
    "#include <stdint.h>\n"
    "\n"
    "#define LED_PIN 13      /* the board's LED */\n"
    "\n"
    "typedef struct {\n"
    "    const char *name;\n"
    "    uint32_t hz;\n"
    "} band_t;\n"
    "\n"
    "static const band_t bands[] = {\n"
    "    { \"2m\",  144000000 },\n"
    "    { \"70cm\", 430000000 },\n"
    "};\n"
    "\n"
    "int main(int argc, char **argv)\n"
    "{\n"
    "    for (int i = 0; i < 2; i++) {\n"
    "        printf(\"%-5s %10u Hz\\n\", bands[i].name, bands[i].hz);\n"
    "    }\n"
    "    if (argc > 1)\n"
    "        return 1;   // error\n"
    "    return 0;\n"
    "}\n";

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void shot_colours(void)
{
    reset();
    char script[600];                          /* the test files live in OUTDIR */
    snprintf(script, sizeof script, "%s/vt_colours.sh", outdir);
    write_file(script, colour_script);
    char *argv[] = { "/bin/bash", script, NULL };
    vtw_spawn(vt, argv, 1);
    wait_exit(5000);
    pump(100);
    save("vt_1_colours");
    /* how long a full redraw of this (busy) screen takes on this PC */
    flushed_pixels = 0;
    uint32_t t0 = nzeus_tick_ms();
    for (int i = 0; i < 50; i++) { lv_obj_invalidate(vt); render(); }
    printf("info: full redraw of the colour screen: %.2f ms on this PC (%ld pixels each)\n",
           (nzeus_tick_ms() - t0) / 50.0, flushed_pixels / 50);
}

static void shot_vim(void)
{
    if (access("/usr/bin/vim", X_OK) != 0) { printf("no vim: skipped\n"); return; }
    reset();
    char demo[600];
    snprintf(demo, sizeof demo, "%s/demo.c", outdir);
    write_file(demo, demo_c);
    char *argv[] = { "vim", "-n", "-i", "NONE", "-u", "DEFAULTS", "-N",
                     "-c", "syntax on", "-c", "set number background=dark cursorline",
                     demo, NULL };
    vtw_spawn(vt, argv, 1);
    check(wait_text("#include <stdio.h>", 5000), "vim shows the file");
    pump(300);
    type("19G");                               /* go to the for loop line */
    type("A  /* edited on R36S */");
    vtw_key(vt, KB_ESC, 0, 0);
    pump(300);
    save("vt_2_vim");
    type(":q!\n");
    check(wait_exit(3000), "vim quits with :q!");
}

static void shot_top(void)
{
    reset();
    int have_htop = access("/usr/bin/htop", X_OK) == 0;
    char *htop[] = { "htop", NULL };
    char *top[] = { "top", "-d", "1", NULL };
    vtw_spawn(vt, have_htop ? htop : top, 1);
    wait_text(have_htop ? "Tasks" : "load average", 5000);
    pump(2500);
    save(have_htop ? "vt_3_htop" : "vt_3_top");
    type("q");
    if (!wait_exit(3000)) vtw_kill(vt, SIGKILL);
    wait_exit(2000);
}

/* Claude Code's first screen, with an empty fake home: no login exists and
 * nothing is typed. The program is stopped right after the screenshot. */
static void shot_claude(const char *claude)
{
    reset();
    char *argv[] = { "/usr/bin/env", "-i", "HOME=/tmp/claude-fake-home", "PATH=/usr/local/bin:/usr/bin:/bin",
                     "LANG=C.UTF-8", "TERM=xterm-256color", "COLORTERM=truecolor", "DISABLE_AUTOUPDATER=1", "DISABLE_TELEMETRY=1", "DISABLE_ERROR_REPORTING=1",
                     "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1", (char *)claude, NULL };
    vtw_spawn(vt, argv, 1);
    int seen = 0;
    for (int i = 0; i < 60 && !seen; i++) {        /* up to 30 s for the first screen */
        pump(500);
        seen = screen_has("Welcome") || screen_has("theme") || screen_has("Claude Code");
    }
    pump(3000);                                     /* let it finish drawing */
    save("vt_4_claude");
    char row[1024];
    for (int r = 0; r < 30; r++) printf("  claude %2d|%s\n", r, vtw_row_text(vt, r, row, sizeof row));
    vtw_kill(vt, SIGTERM);
    if (!wait_exit(3000)) { vtw_kill(vt, SIGKILL); wait_exit(2000); }
    printf("claude first screen %s\n", seen ? "seen" : "NOT seen");
}

int main(int argc, char **argv)
{
    int checks_only = 0;
    const char *claude = NULL;
    if (argc > 1) outdir = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--checks-only") == 0) checks_only = 1;
        else if (strcmp(argv[i], "--claude") == 0 && i + 1 < argc) claude = argv[++i];
    }
    lv_init();
    display_init();
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(255, 0, 255), 0);   /* magenta: shows gaps */
    size_t heap_before = mallinfo2().uordblks;
    vt = vtw_create(lv_scr_act(), 80, 30);
    if (!vt) { printf("FAIL  vtw_create\n"); return 1; }
    printf("info: an 80 x 30 terminal uses %zu bytes of heap (libvterm screens + widget)\n",
           mallinfo2().uordblks - heap_before);
    render();

    run_checks();
    if (!checks_only) {
        shot_colours();
        shot_vim();
        shot_top();
        if (claude) shot_claude(claude);
        printf("after screenshots: %d passed, %d failed\n", passes, fails);
    }
    lv_obj_del(vt);
    return fails;
}

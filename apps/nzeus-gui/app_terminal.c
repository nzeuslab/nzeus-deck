/* app_terminal.c - the NZEUS Terminal app.
 *
 * The terminal engine (terminal.c + process.c) runs
 * /bin/sh commands through pipes and keeps the scrollback. This file draws
 * it with LVGL and feeds it what you type.
 *
 * Buttons, keyboard closed:
 *   A or START   open the keyboard       X        menu (Commands list, copy +
 *   Y            stop the program                  paste, sessions)
 *   UP / DOWN    command history         L1 / R1  scroll the old output
 *   LEFT / RIGHT other session (when there are several)      B  leave
 * Keyboard open:
 *   D-pad + A    pick and type a key     START    Enter (run the line)
 *   X            delete a letter         B        hide the keyboard
 *   Its layers: abc / ABC, #+= (symbols and numbers), Edit (Esc, Home, End,
 *   history, Stop, Mark / Copy / Cut / Paste for the typed line, Select for
 *   the output, Copy last, Commands). Ctrl is "sticky": press Ctrl, then a
 *   letter (C = stop the program, D = end of input, L = clean the screen,
 *   A/E = start/end of the line, U = clear it, V = paste, X = cut).
 *   Tab completes a file or folder name.
 * Select text (menu, the Edit layer's Select key, F4, or drag with the mouse):
 *   a cursor you move over the output with the D-pad (L1/R1 a page, Y = the
 *   word under it). A = start here, A = end here (the part in between is
 *   highlighted, it can cover several lines), A = copy. X = more, B = cancel.
 * The typed line: Mark (Edit layer), move with the arrows, then Copy or Cut;
 *   Paste puts the copied text where the cursor is.
 * USB keyboard: type normally; F1 menu, F2 Commands list, F3 next session,
 *   F4 select text, Shift+arrows mark letters on the typed line,
 *   Ctrl+C copies what is marked (else stops the program), Ctrl+X cut,
 *   Ctrl+V paste, Ctrl+D end of input, Ctrl+L clean the screen. */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "terminal.h"
#include "term_cmds.h"
#include "nzeus_tick.h"
#include "icons.h"
#include "theme.h"
#include "nzui.h"
#include "ui.h"

LV_FONT_DECLARE(lv_font_nzeus_mono_16);   /* font_mono16.c: 8 x 16, 80 columns */
int app_editor_open_file(const char *path); /* app_editor.c */

#define LINE_H     18                      /* our 8 x 16 font, with 2 pixels between lines */
#define CELL_W     8                       /* ... and every letter is 8 pixels wide */
#define MAX_ROWS   40                      /* output rows on screen, at most */
#define KB_H       200                     /* the on-screen keyboard's height */

/* ---------- the on-screen keyboard's layers ----------
 * Key names that do something special (the rest type themselves). */
#define K_BKSP   LV_SYMBOL_BACKSPACE
#define K_ENTER  LV_SYMBOL_NEW_LINE
#define K_LEFT   LV_SYMBOL_LEFT
#define K_RIGHT  LV_SYMBOL_RIGHT
#define K_UP     LV_SYMBOL_UP
#define K_DOWN   LV_SYMBOL_DOWN
#define K_SPACE  "space"
static const char K_CTRL[] = "Ctrl", K_CTRL_ON[] = "CTRL ON";   /* the Ctrl key shows when it is on */
static const char K_MARK[] = "Mark", K_MARK_ON[] = "MARK ON";   /* ... and so does Mark */

enum { LAYER_LOWER, LAYER_UPPER, LAYER_SYMBOLS, LAYER_EDIT };

/* widths (1-7) are relative inside a row; C = a control key (drawn darker) */
#define C (LV_BTNMATRIX_CTRL_CHECKED | LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG)
#define N (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG)

static const char *map_lower[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", K_BKSP, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "/", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ".", K_ENTER, "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", "_", "~", "|", "\n",
    "#+=", K_CTRL, "Tab", K_SPACE, K_LEFT, K_RIGHT, "Edit", "" };
static const char *map_upper[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", K_BKSP, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "/", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", ".", K_ENTER, "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", "_", "~", "|", "\n",
    "#+=", K_CTRL, "Tab", K_SPACE, K_LEFT, K_RIGHT, "Edit", "" };
static const lv_btnmatrix_ctrl_t ctrl_letters[] = {
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 3 | C,
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N,
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 4 | C,
    3 | C, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N,
    3 | C, 3 | C, 3 | C, 7 | N, 2 | C, 2 | C, 3 | C };
/* the symbols a command line needs most: the first row */
static const char *map_symbols[] = {
    "|", "&", ";", ">", "<", "~", "/", "\\", "-", "_", K_BKSP, "\n",
    "$", "\"", "'", "`", "{", "}", "[", "]", "(", ")", "=", "\n",
    "*", "?", "!", "#", "%", "^", ":", ".", ",", "@", K_ENTER, "\n",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "+", "\n",
    "abc", K_CTRL, "Tab", K_SPACE, K_LEFT, K_RIGHT, "Edit", "" };
static const lv_btnmatrix_ctrl_t ctrl_symbols[] = {
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 3 | C,
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N,
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 4 | C,
    2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N, 2 | N,
    3 | C, 3 | C, 3 | C, 7 | N, 2 | C, 2 | C, 3 | C };
/* moving around, history, marking + copy and paste */
static const char *map_edit[] = {
    "Esc", "Home", K_UP, "End", K_BKSP, "Commands", "\n",
    "Stop", K_LEFT, K_DOWN, K_RIGHT, K_ENTER, "Copy last", "\n",
    K_MARK, "Copy", "Cut", "Paste", "Clear", "Select", "\n",
    "abc", K_CTRL, "Tab", K_SPACE, "#+=", "" };
static const lv_btnmatrix_ctrl_t ctrl_edit[] = {
    2 | C, 2 | C, 2 | C, 2 | C, 2 | C, 2 | C,
    2 | C, 2 | C, 2 | C, 2 | C, 2 | C, 2 | C,
    2 | C, 2 | C, 2 | C, 2 | C, 2 | C, 2 | C,
    3 | C, 3 | C, 3 | C, 7 | N, 3 | C };
#undef C
#undef N

static const char **maps[] = { map_lower, map_upper, map_symbols, map_edit };

typedef struct { int line, col; } Pos;     /* a letter of the output: its line, its column */

static struct {
    NzApp *app;
    lv_obj_t *root;
    lv_obj_t *out;                         /* the output text */
    lv_obj_t *prompt;                      /* folder + "$" before what you type */
    lv_obj_t *ta;                          /* what you type */
    lv_obj_t *kb;                          /* on-screen keyboard (hidden until needed) */
    lv_obj_t *hint;                        /* button help (hidden while the keyboard is up) */
    int last_count, last_scroll, hint_kind, last_theme;
    uint32_t last_refresh;
    /* what is on screen: row k shows output line row_line[k] from letter row_col[k]
     * (a line longer than the screen is wide goes on over several rows) */
    int nrows, max_rows, cols;
    short row_line[MAX_ROWS], row_col[MAX_ROWS];
    int layer;                             /* LAYER_* */
    int ctrl;                              /* Ctrl pressed: the next letter is Ctrl+letter */
    /* Select mode: a cursor on the output; the anchor is where A started (line -1 = not yet) */
    int selecting, sel_done, dragging;     /* sel_done: the end is set too (A copies) */
    Pos cur, anc;
    int want_col;                          /* UP/DOWN try to stay in this column */
    /* the typed line: Mark is on, and where it started (-1 = nothing marked) */
    int marking, ta_anchor;
    int ph_pos, ph_len;                    /* a CAPITALS part that typing replaces (-1 = none) */
    char ph_text[48];
    char title[40];
} T;
static const char *pending_cmd;            /* run this as soon as the app is open */
static char pending_cd[1024];              /* ... and start in this folder (Files: "Terminal here") */

static void refresh(int force);
static void set_hints(void);
static void show_keyboard(int show);

/* ---------- small helpers ---------- */
static int kb_open(void) { return T.kb && !lv_obj_has_flag(T.kb, LV_OBJ_FLAG_HIDDEN); }
static int ta_cursor(void) { return (int)lv_textarea_get_cursor_pos(T.ta); }

static void update_title(void)
{
    int n = terminal_session_count();
    if (n <= 1) snprintf(T.title, sizeof T.title, "Terminal");
    else snprintf(T.title, sizeof T.title, "Terminal %d of %d", terminal_session_current() + 1, n);
    apps_set_title(T.app, T.title);
}

/* A key that shows whether it is on (Ctrl, Mark): its label on every layer. */
static void key_label(const char *off, const char *on, int state)
{
    for (int m = 0; m < 4; m++)
        for (int i = 0; maps[m][i][0]; i++)
            if (maps[m][i] == off || maps[m][i] == on) maps[m][i] = state ? on : off;
    if (T.kb) lv_obj_invalidate(T.kb);
}

static void forget_placeholder(void) { T.ph_pos = -1; }

/* ---------- the typed line: marking letters ----------
 * The marked letters are kept by LVGL (the text box's label "selection"),
 * so a mouse drag over the typed line marks them too. */
static int ta_marked(int *lo, int *hi)
{
    lv_obj_t *l = lv_textarea_get_label(T.ta);
    uint32_t s = lv_label_get_text_selection_start(l), e = lv_label_get_text_selection_end(l);
    int len = (int)strlen(lv_textarea_get_text(T.ta));
    if (s == LV_DRAW_LABEL_NO_TXT_SEL || e == LV_DRAW_LABEL_NO_TXT_SEL || s == e) return 0;
    if (s > e) { uint32_t t = s; s = e; e = t; }
    *lo = (int)s;
    *hi = (int)e > len ? len : (int)e;
    return *hi > *lo;
}

/* show the letters between the anchor and the cursor as marked */
static void ta_show_mark(void)
{
    int cur = ta_cursor();
    if (T.ta_anchor < 0 || T.ta_anchor == cur) { lv_textarea_clear_selection(T.ta); return; }
    lv_obj_t *l = lv_textarea_get_label(T.ta);
    lv_label_set_text_sel_start(l, (uint32_t)(T.ta_anchor < cur ? T.ta_anchor : cur));
    lv_label_set_text_sel_end(l, (uint32_t)(T.ta_anchor < cur ? cur : T.ta_anchor));
}

static void ta_unmark(void)
{
    T.ta_anchor = -1;
    lv_textarea_clear_selection(T.ta);
    if (T.marking) { T.marking = 0; key_label(K_MARK, K_MARK_ON, 0); }
}

/* the marked colours follow the theme */
static void ta_colors(void)
{
    const NzTheme *th = theme();
    lv_obj_t *l = lv_textarea_get_label(T.ta);
    /* the typed line in the Terminal's own colours, in every theme: a light
     * theme's box colour with the Terminal's light text was unreadable (Retro Blue) */
    lv_obj_set_style_bg_color(T.ta, th_color(th->term_bg), 0);
    lv_obj_set_style_text_color(T.ta, th_color(th->term_fg), 0);
    lv_obj_set_style_border_color(T.ta, th_color(th->term_fg), LV_PART_CURSOR);
    lv_obj_set_style_bg_color(l, th_color(theme()->accent), LV_PART_SELECTED);
    lv_obj_set_style_text_color(l, th_color(theme()->on_accent), LV_PART_SELECTED);
}

/* take letters lo..hi-1 out of the typed line */
static void ta_delete(int lo, int hi)
{
    char line[TERM_INPUT];
    snprintf(line, sizeof line, "%s", lv_textarea_get_text(T.ta));
    int len = (int)strlen(line);
    if (hi > len) hi = len;
    if (lo < 0 || hi <= lo) return;
    memmove(line + lo, line + hi, (size_t)(len - hi + 1));
    ta_unmark();
    forget_placeholder();
    lv_textarea_set_text(T.ta, line);
    lv_textarea_set_cursor_pos(T.ta, lo);
}

/* typing (or Backspace, or Paste) over marked letters replaces them: 1 = there were some */
static int ta_delete_marked(void)
{
    int lo, hi;
    if (!ta_marked(&lo, &hi)) return 0;
    ta_delete(lo, hi);
    return 1;
}

/* move the text cursor: -1 left, 1 right, -2 start, 2 end. With Shift (or
 * Mark on) the letters it passes are marked. */
static void ta_move(int how, int shift)
{
    forget_placeholder();
    int extend = shift || T.marking;
    if (extend && T.ta_anchor < 0) T.ta_anchor = ta_cursor();
    if (!extend) T.ta_anchor = -1;
    if (how == -1) lv_textarea_cursor_left(T.ta);
    else if (how == 1) lv_textarea_cursor_right(T.ta);
    else lv_textarea_set_cursor_pos(T.ta, how < 0 ? 0 : LV_TEXTAREA_CURSOR_LAST);
    ta_show_mark();
}

static void mark_toggle(void)
{
    if (T.marking) { ta_unmark(); return; }
    T.marking = 1;
    T.ta_anchor = ta_cursor();
    key_label(K_MARK, K_MARK_ON, 1);                 /* (the key now says MARK ON) */
}

/* Copy / Cut on the typed line: the marked letters (nothing marked = the whole line) */
static void ta_copy(int cut)
{
    const char *txt = lv_textarea_get_text(T.ta);
    int lo = 0, hi = (int)strlen(txt);
    ta_marked(&lo, &hi);
    if (hi <= lo) { ui_toast(cut ? "Nothing typed to cut" : "Nothing typed to copy"); return; }
    char part[TERM_INPUT], msg[64];
    snprintf(part, sizeof part, "%.*s", hi - lo, txt + lo);
    nz_clip_set(part);
    snprintf(msg, sizeof msg, "%s: %.40s", cut ? "Cut" : "Copied", part);
    ui_toast(msg);
    if (cut) ta_delete(lo, hi);
    else ta_unmark();
}

/* ---------- typing ---------- */
/* Typing over a CAPITALS part (from the Commands list) replaces it. */
static void replace_placeholder(void)
{
    if (T.ph_pos < 0) return;
    const char *txt = lv_textarea_get_text(T.ta);
    if ((int)strlen(txt) >= T.ph_pos + T.ph_len && !strncmp(txt + T.ph_pos, T.ph_text, (size_t)T.ph_len)) {
        lv_textarea_set_cursor_pos(T.ta, T.ph_pos);
        for (int i = 0; i < T.ph_len; i++) lv_textarea_del_char_forward(T.ta);
    }
    forget_placeholder();
}

static void type_text(const char *s)
{
    if (!ta_delete_marked()) replace_placeholder();
    ta_unmark();
    lv_textarea_add_text(T.ta, s);         /* (at the cursor) */
}

static void backspace(void)
{
    if (ta_delete_marked()) return;                         /* deletes the marked letters */
    ta_unmark();
    if (T.ph_pos >= 0) { replace_placeholder(); return; }   /* deletes the whole CAPITALS part */
    lv_textarea_del_char(T.ta);
}

static void set_line(const char *s)
{
    ta_unmark();
    lv_textarea_set_text(T.ta, s);
    forget_placeholder();
}

static void submit(void)
{
    forget_placeholder();
    ta_unmark();
    terminal_submit(lv_textarea_get_text(T.ta));
    lv_textarea_set_text(T.ta, "");
    terminal_scroll_set(0);                /* jump back to the newest output */
    refresh(1);
}

static void history(int delta)
{
    set_line(terminal_history_step(lv_textarea_get_text(T.ta), delta));
}

/* Tab: finish a file or folder name (or 4 spaces for a running program, e.g. Python) */
static void tab(void)
{
    if (terminal_running()) { type_text("    "); return; }
    char line[TERM_INPUT];
    snprintf(line, sizeof line, "%s", lv_textarea_get_text(T.ta));
    int cur = ta_cursor();
    if (cur > (int)strlen(line)) cur = (int)strlen(line);
    forget_placeholder();
    ta_unmark();
    if (terminal_complete(line, sizeof line, &cur) == 0) { ui_toast("Tab: no file or folder starts like that"); return; }
    lv_textarea_set_text(T.ta, line);
    lv_textarea_set_cursor_pos(T.ta, cur);
    refresh(1);
}

/* Paste where the cursor is (over marked letters). One line, so line breaks become spaces. */
static void paste(void)
{
    const char *c = nz_clip_get();
    if (!c[0]) { ui_toast("Nothing copied yet"); return; }
    char line[TERM_INPUT];
    size_t n = 0;
    for (; *c && n < sizeof line - 1; c++) line[n++] = (*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c;
    while (n && line[n - 1] == ' ') n--;
    line[n] = 0;
    type_text(line);
}

/* ---------- copying from the output (the clipboard is shared with the other apps) ---------- */
static int line_len(int i) { return (int)strlen(terminal_line(i)); }
static int last_col(int i) { int n = line_len(i); return n > 0 ? n - 1 : 0; }
static int pos_before(Pos a, Pos b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }

/* The output from a to b, both letters included; the lines are joined with
 * line breaks. Returns how many lines it covers. */
static int range_text(Pos a, Pos b, char *buf, size_t size)
{
    size_t n = 0;
    int lines = 0;
    for (int i = a.line; i <= b.line; i++) {
        const char *s = terminal_line(i);
        int len = (int)strlen(s), from = i == a.line ? a.col : 0, to = i == b.line ? b.col : len - 1;
        if (to > len - 1) to = len - 1;
        if (i > a.line) { if (n + 1 >= size) break; buf[n++] = '\n'; }
        for (int c = from; c <= to && n + 1 < size; c++) buf[n++] = s[c];
        lines++;
    }
    buf[n] = 0;
    return lines;
}

static void copy_range(Pos a, Pos b, int say_lines)
{
    static char buf[NZ_CLIP_MAX + 1];
    int lines = range_text(a, b, buf, sizeof buf);
    if (!buf[0] && lines <= 1) { ui_toast("Nothing to copy there: the line is empty"); return; }
    nz_clip_set(buf);
    char msg[64];
    if (lines > 1 || say_lines) snprintf(msg, sizeof msg, "Copied %d line%s", lines, lines == 1 ? "" : "s");
    else snprintf(msg, sizeof msg, "Copied: %.40s", buf);
    ui_toast(msg);
}

static void copy_last_output(void)
{
    int first, count = terminal_last_output(&first);
    if (count <= 0) { ui_toast("No output to copy yet"); return; }
    Pos a = { first, 0 }, b = { first + count - 1, TERM_COLS };
    copy_range(a, b, 1);
}

/* ---------- what the output rows show ---------- */
/* Fill the screen rows from the bottom up: the newest line (or an older one
 * while scrolled back) at the bottom. Returns how many rows are used. */
static int layout(int scroll, short *line_of, short *col_of)
{
    int count = terminal_line_count(), end = count - scroll, n = 0, cols = T.cols > 0 ? T.cols : 1;
    if (end < 0) end = 0;
    for (int i = end - 1; i >= 0 && n < T.max_rows; i--) {
        int len = line_len(i), parts = len > 0 ? (len + cols - 1) / cols : 1;
        for (int p = parts - 1; p >= 0 && n < T.max_rows; p--) { line_of[n] = (short)i; col_of[n] = (short)(p * cols); n++; }
    }
    for (int k = 0; k < n / 2; k++) {                  /* (filled bottom-up: turn it round) */
        short l = line_of[k], c = col_of[k];
        line_of[k] = line_of[n - 1 - k]; col_of[k] = col_of[n - 1 - k];
        line_of[n - 1 - k] = l; col_of[n - 1 - k] = c;
    }
    return n;
}

/* the screen row that shows letter p (-1 = not on screen) */
static int row_of(Pos p, int n, const short *line_of, const short *col_of)
{
    for (int k = 0; k < n; k++)
        if (line_of[k] == p.line && p.col >= col_of[k] && p.col < col_of[k] + T.cols) return k;
    return -1;
}

/* ---------- Select mode: a cursor on the output ---------- */
static void sel_clamp(void)
{
    int count = terminal_line_count();
    if (T.cur.line > count - 1) T.cur.line = count - 1;
    if (T.cur.line < 0) T.cur.line = 0;
    if (T.cur.col > last_col(T.cur.line)) T.cur.col = last_col(T.cur.line);
    if (T.cur.col < 0) T.cur.col = 0;
    if (T.anc.line > count - 1) T.anc.line = -1;      /* (the output was cleaned meanwhile) */
}

/* scroll the output so the cursor is on screen (one line at a time, so a
 * move off the top or bottom edge scrolls by one line) */
static void scroll_to_cursor(void)
{
    short L[MAX_ROWS], Cc[MAX_ROWS];
    int s = terminal_scroll_get(), count = terminal_line_count();
    for (int guard = 0; guard <= TERM_LINES + 1; guard++) {
        int n = layout(s, L, Cc);
        if (row_of(T.cur, n, L, Cc) >= 0) break;
        Pos bottom = { L[n > 0 ? n - 1 : 0], Cc[n > 0 ? n - 1 : 0] + T.cols - 1 };
        if (n == 0 || pos_before(bottom, T.cur)) { if (s == 0) break; s--; }    /* below: show newer */
        else { if (s >= count - 1) break; s++; }                                 /* above: show older */
    }
    terminal_scroll_set(s);
}

static void select_changed(void)
{
    set_hints();
    refresh(1);
}

static void select_start(void)
{
    if (!terminal_line_count()) { ui_toast("There is no output to copy yet"); return; }
    if (kb_open()) show_keyboard(0);
    T.selecting = 1;
    T.sel_done = T.dragging = 0;
    T.anc.line = -1;
    T.cur.line = T.nrows > 0 ? T.row_line[T.nrows - 1] : terminal_line_count() - 1;   /* the lowest line on screen */
    T.cur.col = T.want_col = 0;
    select_changed();
}

static void select_end(void)
{
    T.selecting = T.sel_done = T.dragging = 0;
    select_changed();
}

/* the selected part, in order (no anchor yet: just the letter under the cursor) */
static void sel_range(Pos *a, Pos *b)
{
    if (T.anc.line < 0) { *a = *b = T.cur; return; }
    if (pos_before(T.cur, T.anc)) { *a = T.cur; *b = T.anc; }
    else { *a = T.anc; *b = T.cur; }
}

static void select_copy(void)
{
    Pos a, b;
    sel_range(&a, &b);
    copy_range(a, b, 0);
    select_end();
}

static void sel_move_line(int d)
{
    T.sel_done = 0;                                  /* moving again changes the end */
    T.cur.line += d;
    if (T.cur.line < 0) T.cur.line = 0;
    if (T.cur.line > terminal_line_count() - 1) T.cur.line = terminal_line_count() - 1;
    T.cur.col = T.want_col < last_col(T.cur.line) ? T.want_col : last_col(T.cur.line);
    select_changed();
}

/* one letter left / right; at the end of a line, on to the next one */
static void sel_move_col(int d)
{
    T.sel_done = 0;
    if (d < 0 && T.cur.col > 0) T.cur.col--;
    else if (d < 0 && T.cur.line > 0) { T.cur.line--; T.cur.col = last_col(T.cur.line); }
    else if (d > 0 && T.cur.col < last_col(T.cur.line)) T.cur.col++;
    else if (d > 0 && T.cur.line < terminal_line_count() - 1) { T.cur.line++; T.cur.col = 0; }
    T.want_col = T.cur.col;
    select_changed();
}

static void sel_move_to(int col)                    /* Home / End */
{
    T.sel_done = 0;
    T.cur.col = T.want_col = col < 0 ? 0 : last_col(T.cur.line);
    select_changed();
}

/* Y: the word under the cursor (letters up to the spaces around it) */
static void select_word(void)
{
    const char *s = terminal_line(T.cur.line);
    if (!s[T.cur.col] || s[T.cur.col] == ' ') { ui_toast("Put the cursor on a word first"); return; }
    int a = T.cur.col, b = T.cur.col;
    while (a > 0 && s[a - 1] != ' ') a--;
    while (s[b + 1] && s[b + 1] != ' ') b++;
    T.anc.line = T.cur.line; T.anc.col = a;
    T.cur.col = T.want_col = b;
    T.sel_done = 1;
    select_changed();
}

/* A: start here, end here, then copy */
static void select_press_a(void)
{
    if (T.anc.line < 0) T.anc = T.cur;
    else if (!T.sel_done) T.sel_done = 1;
    else { select_copy(); return; }
    select_changed();
}

/* X: more choices */
static void select_menu_answer(int choice, void *ctx)
{
    (void)ctx;
    if (!T.selecting) return;
    Pos a, b;
    sel_range(&a, &b);
    switch (choice) {
    case 0: select_copy(); break;
    case 1: select_copy(); paste(); break;                  /* ... and put it on the typed line */
    case 2:                                                 /* whole lines */
        T.anc.line = a.line; T.anc.col = 0;
        T.cur.line = b.line; T.cur.col = T.want_col = last_col(b.line);
        T.sel_done = 1;
        select_changed();
        break;
    case 3:                                                 /* everything */
        T.anc.line = 0; T.anc.col = 0;
        T.cur.line = terminal_line_count() - 1; T.cur.col = T.want_col = last_col(T.cur.line);
        T.sel_done = 1;
        select_changed();
        break;
    case 4: select_end(); break;
    default: break;
    }
}

static void select_menu(void)
{
    static const char *const items[] = { "Copy", "Copy and paste below", "Select whole lines", "Select all",
                                          "Stop selecting" };
    nz_menu("Selected text", items, 5, select_menu_answer, NULL);
}

static int select_key(int k)
{
    int page = T.max_rows > 1 ? T.max_rows - 1 : 1;
    switch (k) {
    case NZ_KEY_UP:    sel_move_line(-1); break;
    case NZ_KEY_DOWN:  sel_move_line(1); break;
    case NZ_KEY_LEFT:  sel_move_col(-1); break;
    case NZ_KEY_RIGHT: sel_move_col(1); break;
    case NZ_KEY_L:     sel_move_line(-page); break;
    case NZ_KEY_R:     sel_move_line(page); break;
    case NZ_KEY_Y:     select_word(); break;
    case NZ_KEY_A:     select_press_a(); break;
    case NZ_KEY_X:     select_menu(); break;
    case NZ_KEY_B:     select_end(); break;
    default: break;
    }
    return 1;
}

/* The selection is drawn on top of the output text: the theme's accent colour
 * behind the letters, the letters in the colour made for it (on_accent).
 * The cursor alone is one such letter; at the moving end of a selection it
 * is a frame in the accent colour instead (so you see which end moves). */
enum { CELLS_SELECTED, CELLS_CURSOR, CELLS_CURSOR_FRAME };
static void draw_cells(lv_draw_ctx_t *dc, const lv_area_t *box, int k, int lo, int hi, int how)
{
    const NzTheme *th = theme();
    const char *s = terminal_line(T.row_line[k]);
    int len = (int)strlen(s), c0 = T.row_col[k];
    lv_area_t r = { (lv_coord_t)(box->x1 + (lo - c0) * CELL_W), (lv_coord_t)(box->y1 + k * LINE_H),
                    (lv_coord_t)(box->x1 + (hi - c0 + 1) * CELL_W - 1), (lv_coord_t)(box->y1 + (k + 1) * LINE_H - 1) };
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = th_color(how == CELLS_CURSOR_FRAME ? th->term_bg : th->accent);
    rd.bg_opa = LV_OPA_COVER;
    if (how == CELLS_CURSOR_FRAME) {
        rd.border_color = th_color(th->accent);
        rd.border_width = th->crt ? 1 : 2;           /* (thin on the CRT looks: the glow widens it) */
        rd.border_opa = LV_OPA_COVER;
    }
    lv_draw_rect(dc, &rd, &r);
    if (lo < len) {                                  /* the letters again, on top, in the matching colour */
        char part[TERM_COLS + 1];
        int n = (hi < len ? hi : len - 1) - lo + 1;
        memcpy(part, s + lo, (size_t)n);
        part[n] = 0;
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.font = &lv_font_nzeus_mono_16;
        ld.color = th_color(how == CELLS_CURSOR_FRAME ? th->term_fg : th->on_accent);
        ld.flag = LV_TEXT_FLAG_EXPAND;
        lv_draw_label(dc, &ld, &r, part, NULL);
    }
}

static void out_draw(lv_event_t *e)
{
    if (!T.selecting || T.nrows <= 0) return;
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_content_coords(T.out, &box);
    Pos a, b;
    sel_range(&a, &b);
    for (int k = 0; k < T.nrows; k++) {
        int line = T.row_line[k], c0 = T.row_col[k], end = c0 + T.cols - 1;
        if (T.anc.line >= 0 && line >= a.line && line <= b.line) {
            int lo = line == a.line ? a.col : 0;
            int hi = line == b.line ? b.col : line_len(line);   /* (on to the next line: the line break too) */
            if (lo < c0) lo = c0;
            if (hi > end) hi = end;
            if (lo <= hi) draw_cells(dc, &box, k, lo, hi, CELLS_SELECTED);
        }
        if (line == T.cur.line && T.cur.col >= c0 && T.cur.col <= end)
            draw_cells(dc, &box, k, T.cur.col, T.cur.col, T.anc.line >= 0 ? CELLS_CURSOR_FRAME : CELLS_CURSOR);
    }
}

/* the output letter under the mouse pointer */
static int pointer_pos(Pos *p)
{
    lv_indev_t *in = lv_indev_get_act();
    if (!in || T.nrows <= 0) return 0;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    lv_area_t box;
    lv_obj_get_content_coords(T.out, &box);
    int k = pt.y < box.y1 ? 0 : (pt.y - box.y1) / LINE_H;
    if (k > T.nrows - 1) k = T.nrows - 1;
    int c = pt.x < box.x1 ? 0 : (pt.x - box.x1) / CELL_W;
    if (c > T.cols - 1) c = T.cols - 1;
    p->line = T.row_line[k];
    p->col = T.row_col[k] + c;
    if (p->col > last_col(p->line)) p->col = last_col(p->line);
    return 1;
}

/* the mouse: press, drag over the output, let go = copied */
static void out_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    Pos p;
    if (code == LV_EVENT_PRESSED) {
        if (kb_open()) { show_keyboard(0); return; }    /* (the first click hides the keyboard) */
        if (!pointer_pos(&p)) return;
        T.selecting = T.dragging = 1;
        T.sel_done = 0;
        T.anc = T.cur = p;
        T.want_col = p.col;
        select_changed();
    } else if (code == LV_EVENT_PRESSING && T.dragging && pointer_pos(&p)) {
        if (p.line != T.cur.line || p.col != T.cur.col) {
            T.cur = p;
            T.want_col = p.col;
            lv_obj_invalidate(T.out);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && T.dragging) {
        T.dragging = 0;
        if (T.anc.line == T.cur.line && T.anc.col == T.cur.col) { select_end(); return; }   /* a click: nothing */
        T.sel_done = 1;                                 /* dragged: copied at once, still shown */
        Pos a, b;
        sel_range(&a, &b);
        copy_range(a, b, 0);
        select_changed();
    }
}

/* ---------- drawing ---------- */
static int hint_kind_now(void)
{
    int sel = T.selecting ? 1 + (T.anc.line >= 0) + T.sel_done : 0;
    return (T.app->in_window ? 100 : 0) + (terminal_session_count() > 1) * 10 + sel;
}

/* Show the newest lines that fit (or older ones while scrolled back). */
static void refresh(int force)
{
    if (theme_index() != T.last_theme) { T.last_theme = theme_index(); ta_colors(); force = 1; }
    int count = terminal_line_count(), scroll = terminal_scroll_get();
    uint32_t now = nzeus_tick_ms();
    if (!force && count == T.last_count && scroll == T.last_scroll && now - T.last_refresh < 250) return;

    T.max_rows = lv_obj_get_content_height(T.out) / LINE_H;
    if (T.max_rows < 1) T.max_rows = 1;
    if (T.max_rows > MAX_ROWS) T.max_rows = MAX_ROWS;
    T.cols = lv_obj_get_content_width(T.out) / CELL_W;
    if (T.cols < 1) T.cols = 1;
    if (T.cols > TERM_COLS) T.cols = TERM_COLS;
    if (T.selecting && !count) T.selecting = T.sel_done = T.dragging = 0;   /* the output was cleaned */
    if (T.selecting) {                                 /* keep the cursor on screen */
        sel_clamp();
        scroll_to_cursor();
        scroll = terminal_scroll_get();
    }
    T.last_count = count; T.last_scroll = scroll; T.last_refresh = now;
    T.nrows = layout(scroll, T.row_line, T.row_col);

    static char buf[MAX_ROWS * (TERM_COLS + 1)];
    size_t n = 0;
    for (int k = 0; k < T.nrows; k++) {
        const char *line = terminal_line(T.row_line[k]) + T.row_col[k];
        size_t len = strlen(line);
        if (len > (size_t)T.cols) len = (size_t)T.cols;   /* the rest is on the next row */
        if (k) buf[n++] = '\n';
        memcpy(buf + n, line, len);
        n += len;
    }
    buf[n] = 0;
    if (strcmp(lv_label_get_text(T.out), buf) != 0) lv_label_set_text(T.out, buf);
    if (T.selecting || force) lv_obj_invalidate(T.out);   /* (the selection is drawn on top) */

    const char *cwd = terminal_get_cwd(), *base = strrchr(cwd, '/');
    char p[64];
    snprintf(p, sizeof p, "%s $", (base && base[1]) ? base + 1 : cwd);
    if (strcmp(lv_label_get_text(T.prompt), p) != 0) lv_label_set_text(T.prompt, p);
    if (T.hint_kind != hint_kind_now()) set_hints();   /* window <-> full screen, or select mode changed */
}

/* The button help at the bottom: depends on what you are doing. */
static void set_hints(void)
{
    int sessions = terminal_session_count() > 1;
    T.hint_kind = hint_kind_now();
    static const char *const normal[] = { "A", "Type", "X", "Menu", "Y", "Stop", "UP", "History", "L1/R1", "Scroll" };
    static const char *const multi[] = { "A", "Type", "X", "Menu", "Y", "Stop", "UP", "History", "LEFT/RIGHT", "Session", "L1/R1", "Scroll" };
    static const char *const sel0[] = { "D-pad", "Move", "A", "Start here", "Y", "Word", "L1/R1", "Page", "B", "Cancel" };
    static const char *const sel1[] = { "D-pad", "Move", "A", "End here", "X", "More", "L1/R1", "Page", "B", "Cancel" };
    static const char *const sel2[] = { "A", "Copy", "X", "More", "D-pad", "Change the end", "B", "Cancel" };
    static const char *const keys[] = { "F1", "Menu", "F2", "Commands", "F4", "Select", "Ctrl+C", "Stop" };
    static const char *const keys_multi[] = { "F1", "Menu", "F2", "Commands", "F3", "Session", "F4", "Select", "Ctrl+C", "Stop" };
    static const char *const selkeys[] = { "Arrows", "Move", "Enter", "Start / end / copy", "Ctrl+C", "Copy", "Esc", "Cancel" };
    lv_obj_clean(T.hint);
    lv_obj_t *h;
    if (T.selecting && T.app->in_window) h = nz_hints(T.hint, selkeys, 4);
    else if (T.selecting) h = T.sel_done ? nz_hints(T.hint, sel2, 4) : nz_hints(T.hint, T.anc.line < 0 ? sel0 : sel1, 5);
    else if (T.app->in_window) h = sessions ? nz_hints(T.hint, keys_multi, 5) : nz_hints(T.hint, keys, 4);   /* in a window: a USB keyboard */
    else h = sessions ? nz_hints(T.hint, multi, 6) : nz_hints(T.hint, normal, 5);   /* (L1/R1 scroll in both) */
    lv_obj_center(h);
}

static void show_keyboard(int show)
{
    if (show) {
        if (T.selecting) T.selecting = T.sel_done = T.dragging = 0;
        lv_obj_clear_flag(T.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(T.hint, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(T.kb);
    } else {
        lv_obj_add_flag(T.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(T.hint, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_update_layout(T.root);
    refresh(1);
}

static void scroll_by(int lines)
{
    int s = terminal_scroll_get() + lines;
    if (s < 0) s = 0;
    terminal_scroll_set(s);
    refresh(1);
}

/* ---------- sessions ---------- */
static void keep_typed(void) { terminal_set_input(lv_textarea_get_text(T.ta)); }   /* each session keeps its line */

static void session_shown(void)
{
    set_line(terminal_input_text());
    T.selecting = T.sel_done = T.dragging = 0;
    T.last_count = -1;
    update_title();
    set_hints();
    refresh(1);
}

static void switch_session(int index)
{
    keep_typed();
    terminal_session_switch(index);
    session_shown();
    char msg[40];
    snprintf(msg, sizeof msg, "Session %d of %d", index + 1, terminal_session_count());
    ui_toast(msg);
}

static void new_session(void)
{
    keep_typed();
    if (terminal_session_new() < 0) { ui_toast("4 sessions is the most. Close one first."); return; }
    session_shown();
}

static void close_session(void)
{
    if (terminal_session_count() <= 1) { apps_close(T.app); return; }   /* the last one: leave */
    terminal_session_close();
    session_shown();
}

static void switch_answer(int choice, void *ctx) { (void)ctx; if (choice >= 0) switch_session(choice); }

static void switch_menu(void)
{
    static char names[TERM_SESSIONS][48];
    static const char *items[TERM_SESSIONS];
    int n = terminal_session_count();
    for (int i = 0; i < n; i++) {
        const char *cwd = terminal_session_cwd(i), *base = strrchr(cwd, '/');
        snprintf(names[i], sizeof names[i], "%d: %.24s%s%s", i + 1, (base && base[1]) ? base + 1 : cwd,
                 terminal_session_running(i) ? " (running)" : "", i == terminal_session_current() ? "  <- here" : "");
        items[i] = names[i];
    }
    nz_menu("Switch to session", items, n, switch_answer, NULL);
}

/* ---------- the Commands list (term_cmds.c) ---------- */
static void command_picked(const char *cmd)
{
    set_line(cmd);
    int len, pos = term_cmds_placeholder(cmd, &len);
    if (pos >= 0) {                                    /* the cursor goes to the first CAPITALS part */
        lv_textarea_set_cursor_pos(T.ta, pos);
        T.ph_pos = pos;
        T.ph_len = len < (int)sizeof T.ph_text ? len : (int)sizeof T.ph_text - 1;
        snprintf(T.ph_text, sizeof T.ph_text, "%.*s", T.ph_len, cmd + pos);
        char msg[80];
        snprintf(msg, sizeof msg, "Type to fill in %s", T.ph_text);
        ui_toast(msg);
    }
    if (!T.app->in_window && !kb_open()) show_keyboard(1);   /* ready to change it */
    refresh(1);
}

static void open_commands(void)
{
    if (kb_open()) show_keyboard(0);
    T.selecting = T.sel_done = T.dragging = 0;
    term_cmds_open(T.root, command_picked);
}

/* ---------- the menu (X / F1) ---------- */
static void menu_answer(int choice, void *ctx)
{
    (void)ctx;
    switch (choice) {
    case 0: open_commands(); break;
    case 1: paste(); break;
    case 2: select_start(); break;
    case 3: copy_last_output(); break;
    case 4: new_session(); break;
    case 5: switch_menu(); break;
    case 6: close_session(); break;
    default: break;
    }
}

static void open_menu(void)
{
    static const char *const items[] = { "Commands...", "Paste", "Select text to copy", "Copy last output",
                                         "New session", "Switch session...", "Close this session" };
    nz_menu("Terminal", items, 7, menu_answer, NULL);
}

/* ---------- Ctrl + a letter ---------- */
static void set_ctrl(int on)
{
    T.ctrl = on;
    key_label(K_CTRL, K_CTRL_ON, on);
}

static void ctrl_letter(char c)
{
    set_ctrl(0);
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    switch (c) {
    case 'c': terminal_interrupt(); break;                    /* stop the program */
    case 'd': terminal_send_eof(); break;                     /* end of input */
    case 'l': terminal_clear(); refresh(1); break;            /* clean the screen */
    case 'a': ta_move(-2, 0); break;
    case 'e': ta_move(2, 0); break;
    case 'u': set_line(""); break;                            /* clear the typed line */
    case 'v': paste(); break;
    case 'x': ta_copy(1); break;                              /* cut */
    default: {
        char msg[48];
        snprintf(msg, sizeof msg, "Ctrl+%c does nothing here", c);
        ui_toast(msg);
    }
    }
}

static void set_layer(int layer)
{
    T.layer = layer;
    lv_keyboard_set_mode(T.kb, layer == LAYER_LOWER ? LV_KEYBOARD_MODE_TEXT_LOWER : layer == LAYER_UPPER ? LV_KEYBOARD_MODE_TEXT_UPPER
                               : layer == LAYER_SYMBOLS ? LV_KEYBOARD_MODE_SPECIAL : LV_KEYBOARD_MODE_USER_1);
}

/* A key on the on-screen keyboard (by its name). */
static void press_key(const char *t)
{
    if (!strcmp(t, "abc")) { set_layer(LAYER_LOWER); return; }
    if (!strcmp(t, "ABC")) { set_layer(LAYER_UPPER); return; }
    if (!strcmp(t, "#+=")) { set_layer(LAYER_SYMBOLS); return; }
    if (!strcmp(t, "Edit")) { set_layer(LAYER_EDIT); return; }
    if (!strcmp(t, K_CTRL) || !strcmp(t, K_CTRL_ON)) {
        set_ctrl(!T.ctrl);
        if (T.ctrl) ui_toast("Ctrl: now press a letter (C = stop the program)");
        return;
    }
    if (T.ctrl && t[0] && !t[1] && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z'))) { ctrl_letter(t[0]); return; }
    if (T.ctrl) set_ctrl(0);                          /* any other key: Ctrl is off again */
    if (!strcmp(t, K_BKSP)) backspace();
    else if (!strcmp(t, K_ENTER)) submit();
    else if (!strcmp(t, K_LEFT)) ta_move(-1, 0);
    else if (!strcmp(t, K_RIGHT)) ta_move(1, 0);
    else if (!strcmp(t, K_UP)) history(-1);
    else if (!strcmp(t, K_DOWN)) history(1);
    else if (!strcmp(t, "Tab")) tab();
    else if (!strcmp(t, "Esc")) { forget_placeholder(); ta_unmark(); show_keyboard(0); }
    else if (!strcmp(t, "Home")) ta_move(-2, 0);
    else if (!strcmp(t, "End")) ta_move(2, 0);
    else if (!strcmp(t, "Stop")) terminal_interrupt();
    else if (!strcmp(t, K_MARK) || !strcmp(t, K_MARK_ON)) mark_toggle();
    else if (!strcmp(t, "Copy")) ta_copy(0);
    else if (!strcmp(t, "Cut")) ta_copy(1);
    else if (!strcmp(t, "Select")) select_start();
    else if (!strcmp(t, "Copy last")) copy_last_output();
    else if (!strcmp(t, "Paste")) paste();
    else if (!strcmp(t, "Clear")) set_line("");
    else if (!strcmp(t, "Commands")) open_commands();
    else if (!strcmp(t, K_SPACE)) type_text(" ");
    else type_text(t);                               /* a letter, number, symbol or space */
}

static void kb_event(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(kb);
    if (id == LV_BTNMATRIX_BTN_NONE) return;
    const char *t = lv_btnmatrix_get_btn_text(kb, id);
    if (t) press_key(t);
}

static void ta_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) submit();
    else if (lv_event_get_code(e) == LV_EVENT_CLICKED && !kb_open()) show_keyboard(1);   /* mouse: click the line */
}

/* ---------- the app ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    terminal_init();
    memset(&T, 0, sizeof T);
    T.app = a;
    T.root = root;
    T.ph_pos = -1;
    T.hint_kind = -1;
    T.ta_anchor = -1;
    T.anc.line = -1;
    set_ctrl(0);
    key_label(K_MARK, K_MARK_ON, 0);
    th_add(root, TS_TERM);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 4, 0);

    T.out = lv_label_create(root);         /* output: fills the free space */
    lv_obj_set_width(T.out, LV_PCT(100));
    lv_obj_set_flex_grow(T.out, 1);
    lv_label_set_long_mode(T.out, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_hor(T.out, 8, 0);     /* a little air between the text and the edges */
    lv_obj_set_style_pad_top(T.out, 6, 0);
    th_add(T.out, TS_MONO);
    lv_label_set_text(T.out, "");
    lv_obj_add_flag(T.out, LV_OBJ_FLAG_CLICKABLE);          /* the mouse selects text in it */
    lv_obj_clear_flag(T.out, LV_OBJ_FLAG_SCROLL_CHAIN);     /* (dragging must not move the window) */
    lv_obj_add_event_cb(T.out, out_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(T.out, out_draw, LV_EVENT_DRAW_POST, NULL);

    lv_obj_t *row = lv_obj_create(root);   /* input line: prompt + text box */
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    T.prompt = th_label(row, "$", TS_MONO_ACCENT);
    T.ta = lv_textarea_create(row);
    lv_textarea_set_one_line(T.ta, true);
    lv_textarea_set_text_selection(T.ta, true);             /* the mouse can mark letters too */
    lv_obj_set_flex_grow(T.ta, 1);
    lv_obj_set_height(T.ta, 34);
    th_add(T.ta, TS_MONO);
    lv_obj_set_style_pad_ver(T.ta, 6, 0);
    lv_obj_add_state(T.ta, LV_STATE_FOCUSED);     /* show the text cursor */
    lv_obj_add_event_cb(T.ta, ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(T.ta, ta_event, LV_EVENT_CLICKED, NULL);
    T.last_theme = theme_index();
    ta_colors();

    T.kb = lv_keyboard_create(root);        /* on-screen keyboard: our layers, our key handler */
    lv_obj_set_size(T.kb, LV_PCT(100), KB_H);
    lv_keyboard_set_map(T.kb, LV_KEYBOARD_MODE_TEXT_LOWER, map_lower, ctrl_letters);
    lv_keyboard_set_map(T.kb, LV_KEYBOARD_MODE_TEXT_UPPER, map_upper, ctrl_letters);
    lv_keyboard_set_map(T.kb, LV_KEYBOARD_MODE_SPECIAL, map_symbols, ctrl_symbols);
    lv_keyboard_set_map(T.kb, LV_KEYBOARD_MODE_USER_1, map_edit, ctrl_edit);
    lv_obj_remove_event_cb(T.kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(T.kb, kb_event, LV_EVENT_VALUE_CHANGED, NULL);
    set_layer(LAYER_LOWER);
    lv_group_add_obj(apps_keypad_group(), T.kb);
    lv_obj_add_flag(T.kb, LV_OBJ_FLAG_HIDDEN);

    T.hint = th_box(root, TS_BAR);          /* button help */
    lv_obj_set_size(T.hint, LV_PCT(100), 24);
    lv_obj_clear_flag(T.hint, LV_OBJ_FLAG_SCROLLABLE);
    set_hints();
    update_title();

    lv_obj_update_layout(root);
    refresh(1);
}

static void tick(NzApp *a)
{
    terminal_update();                      /* read output from every session's program */
    if (pending_cd[0]) { terminal_set_cwd(pending_cd); pending_cd[0] = 0; refresh(1); }
    if (pending_cmd) {
        if (terminal_running() && terminal_session_count() < TERM_SESSIONS) new_session();   /* busy: use a new session */
        terminal_submit(pending_cmd);
        pending_cmd = NULL;
    }
    if (terminal_exit_pending()) {          /* the `exit` command */
        if (terminal_session_count() > 1) close_session();
        else { apps_close(a); return; }
    }
    const char *edit = terminal_edit_request();               /* the `edit FILE` command */
    if (edit) app_editor_open_file(edit);
    refresh(0);
}

static void leave_answer(int yes, void *ctx) { (void)ctx; if (yes) apps_close(T.app); }

static int key(NzApp *a, int k)
{
    (void)a;
    if (term_cmds_is_open()) return term_cmds_key(k);          /* the Commands list is on top */
    if (T.selecting) return select_key(k);
    switch (k) {
    case NZ_KEY_Y: terminal_interrupt(); return 1;            /* Ctrl+C */
    case NZ_KEY_L: scroll_by(5); return 1;
    case NZ_KEY_R: scroll_by(-5); return 1;
    case NZ_KEY_START: if (kb_open()) submit(); else show_keyboard(1); return 1;
    default: break;
    }
    if (!kb_open()) {
        int n = terminal_session_count();
        switch (k) {
        case NZ_KEY_A: show_keyboard(1); return 1;
        case NZ_KEY_X: open_menu(); return 1;
        case NZ_KEY_UP: history(-1); return 1;                  /* command history */
        case NZ_KEY_DOWN: history(1); return 1;
        case NZ_KEY_LEFT:
        case NZ_KEY_RIGHT:                                     /* the other sessions */
            if (n > 1) switch_session((terminal_session_current() + (k == NZ_KEY_RIGHT ? 1 : n - 1)) % n);
            return 1;
        case NZ_KEY_B:
            if (n > 1) {                                       /* don't lose several sessions by accident */
                static char q[64];
                snprintf(q, sizeof q, "Close the Terminal and its %d sessions?", n);
                nz_confirm(q, leave_answer, NULL);
                return 1;
            }
            return 0;                                          /* B: leave the app */
        default: return 0;
        }
    }
    switch (k) {                                               /* keyboard open: drive it */
    case NZ_KEY_UP:    apps_keypad_send(LV_KEY_UP);    return 1;
    case NZ_KEY_DOWN:  apps_keypad_send(LV_KEY_DOWN);  return 1;
    case NZ_KEY_LEFT:  apps_keypad_send(LV_KEY_LEFT);  return 1;
    case NZ_KEY_RIGHT: apps_keypad_send(LV_KEY_RIGHT); return 1;
    case NZ_KEY_A:     apps_keypad_send(LV_KEY_ENTER); return 1;
    case NZ_KEY_X:     backspace();                    return 1;
    case NZ_KEY_B:     if (T.ctrl) set_ctrl(0); show_keyboard(0); return 1;
    default:           return 0;
    }
}

/* the USB keyboard in Select mode */
static void select_kbd(const KbEvent *ev)
{
    int page = T.max_rows > 1 ? T.max_rows - 1 : 1;
    if (ev->ch == KB_SHIFTED && T.anc.line < 0 && ev->key >= KB_UP && ev->key <= KB_PGDN) T.anc = T.cur;  /* Shift: start here */
    switch (ev->key) {
    case KB_UP:     sel_move_line(-1); break;
    case KB_DOWN:   sel_move_line(1); break;
    case KB_LEFT:   sel_move_col(-1); break;
    case KB_RIGHT:  sel_move_col(1); break;
    case KB_HOME:   sel_move_to(-1); break;
    case KB_END:    sel_move_to(1); break;
    case KB_PGUP:   sel_move_line(-page); break;
    case KB_PGDN:   sel_move_line(page); break;
    case KB_ENTER:  select_press_a(); break;
    case KB_CTRL_C: select_copy(); break;                      /* Ctrl+C copies while selecting */
    case KB_ESC:    select_end(); break;
    case KB_F1:     select_menu(); break;
    default: break;
    }
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    (void)a;
    if (term_cmds_is_open()) { term_cmds_kbd(ev); return; }
    if (T.selecting) { select_kbd(ev); return; }
    if (kb_open()) show_keyboard(0);                           /* a real keyboard: hide ours */
    int shift = ev->ch == KB_SHIFTED, lo, hi;
    switch ((int)ev->key) {                                    /* (int: F2/F3 are KB_F1 + n) */
    case KB_CHAR:      { char s[2] = { ev->ch, 0 }; type_text(s); break; }
    case KB_ENTER:     submit(); break;
    case KB_BACKSPACE: backspace(); break;
    case KB_DELETE:
        if (!ta_delete_marked()) { forget_placeholder(); ta_unmark(); lv_textarea_del_char_forward(T.ta); }
        break;
    case KB_LEFT:      ta_move(-1, shift); break;              /* Shift + arrows mark letters */
    case KB_RIGHT:     ta_move(1, shift); break;
    case KB_HOME:      ta_move(-2, shift); break;
    case KB_END:       ta_move(2, shift); break;
    case KB_UP:        history(-1); break;
    case KB_DOWN:      history(1); break;
    case KB_PGUP:      scroll_by(10); break;
    case KB_PGDN:      scroll_by(-10); break;
    case KB_CTRL_C:                                            /* letters marked (or Shift): copy them */
        if (shift || ta_marked(&lo, &hi)) ta_copy(0);
        else terminal_interrupt();
        break;
    case KB_CTRL_D:    terminal_send_eof(); break;
    case KB_CTRL_OTHER: ctrl_letter(ev->ch); break;           /* Ctrl+V, Ctrl+X, Ctrl+L, ... */
    case KB_TAB:       tab(); break;
    case KB_ESC:       forget_placeholder(); ta_unmark(); break;
    case KB_F1:        open_menu(); break;
    case KB_F1 + 1:    open_commands(); break;                 /* F2 */
    case KB_F1 + 2:                                            /* F3: the next session */
        if (terminal_session_count() > 1) switch_session((terminal_session_current() + 1) % terminal_session_count());
        else ui_toast("Only one session (F1 > New session)");
        break;
    case KB_F1 + 3:    select_start(); break;                  /* F4: select text in the output */
    default: break;
    }
}

static void destroy(NzApp *a)
{
    term_cmds_drop();                       /* (its objects are deleted with the root) */
    terminal_free();                        /* stops every session's program */
    a->title = "Terminal";
    set_ctrl(0);
    key_label(K_MARK, K_MARK_ON, 0);
    memset(&T, 0, sizeof T);
}

/* ---------- for tests ---------- */
const char *app_terminal_input(void) { return T.ta ? lv_textarea_get_text(T.ta) : ""; }
lv_obj_t *app_terminal_textbox(void) { return T.ta; }      /* (tests: its colours) */
int app_terminal_cursor(void) { return T.ta ? ta_cursor() : -1; }
int app_terminal_selecting(void) { return T.selecting; }
int app_terminal_kb_open(void) { return kb_open(); }
int app_terminal_layer(void) { return T.layer; }
int app_terminal_ctrl(void) { return T.ctrl; }
int app_terminal_marking(void) { return T.marking; }
const char *app_terminal_title(void) { return T.title; }
const char *app_terminal_screen(void) { return T.out ? lv_label_get_text(T.out) : ""; }
/* the typed line's marked letters (lo..hi-1); 0 = none */
int app_terminal_marked(int *lo, int *hi) { return T.ta && ta_marked(lo, hi); }
/* Select mode: where the start and the cursor are; returns 1 when the end is set too */
int app_terminal_sel(int *anc_line, int *anc_col, int *cur_line, int *cur_col)
{
    *anc_line = T.anc.line; *anc_col = T.anc.col; *cur_line = T.cur.line; *cur_col = T.cur.col;
    return T.sel_done;
}
/* the screen position (top-left pixel) of an output letter; 0 = not on screen */
int app_terminal_cell_xy(int line, int col, int *x, int *y)
{
    Pos p = { line, col };
    int k = T.out ? row_of(p, T.nrows, T.row_line, T.row_col) : -1;
    if (k < 0) return 0;
    lv_area_t box;
    lv_obj_get_content_coords(T.out, &box);
    *x = box.x1 + (col - T.row_col[k]) * CELL_W;
    *y = box.y1 + k * LINE_H;
    return 1;
}
int app_terminal_row_line(int k) { return k >= 0 && k < T.nrows ? T.row_line[k] : -1; }   /* -1 = no such row */
/* Put the D-pad selection on the key called `name` (the next A presses it). 1 = found. */
int app_terminal_osk_select(const char *name)
{
    if (!kb_open()) return 0;
    for (uint16_t i = 0; i < 80; i++) {
        const char *t = lv_btnmatrix_get_btn_text(T.kb, i);
        if (!t || !t[0]) return 0;                               /* past the last key */
        if (!strcmp(t, name)) { lv_btnmatrix_set_selected_btn(T.kb, i); return 1; }
    }
    return 0;
}

/* Open the Terminal and run a command in it (the Python tile uses this). */
void app_terminal_run(const char *cmd)
{
    pending_cmd = cmd;
    apps_open(APP_TERMINAL);
}

/* Open the Terminal in a folder (Files: "Terminal here"). */
void app_terminal_cd(const char *dir)
{
    snprintf(pending_cd, sizeof pending_cd, "%s", dir);
    apps_open(APP_TERMINAL);
}

NzApp app_terminal = {
    .icon = ICON_TERMINAL,
    .id = APP_TERMINAL, .title = "Terminal",
    .create = create, .key = key, .kbd = kbd, .tick = tick, .destroy = destroy,
};

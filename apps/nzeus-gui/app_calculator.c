/* app_calculator.c - the NZEUS Calculator.
 *
 * Top: the display - the sum you are typing in big numbers, and above it
 * the answer so far (or the sum you just worked out). Below: the keys, and
 * on the right the last 5 answers.
 *   D-pad  move between the keys      A      press the key
 *   START  =  (work it out)           X      clear everything
 *   B      delete the last character (when there is nothing left: leave)
 * A mouse can click the keys. A USB keyboard types straight in:
 *   0-9 + - * / ( ) . %   Enter or =  work it out   Backspace  delete   Esc  clear
 * The maths itself is in calc_eval.c. */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "calc_eval.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

#define ROWS 5
#define COLS 5
#define HISTORY 5

/* The keys. act: what the key does ('0'-'9' . + - * / ( ) % as typed,
 * 'n' = +/-, 'C' = clear, 'D' = delete, '=' = work it out).
 * kind: 0 number, 1 operator, 2 the = key */
typedef struct { const char *label; char act; int row, col, span, kind; } KeyDef;
static const KeyDef keys[] = {
    { "C", 'C', 0, 0, 1, 1 }, { "DEL", 'D', 0, 1, 1, 1 }, { "%", '%', 0, 2, 1, 1 }, { "+/-", 'n', 0, 3, 1, 1 }, { "/", '/', 0, 4, 1, 1 },
    { "7", '7', 1, 0, 1, 0 }, { "8", '8', 1, 1, 1, 0 }, { "9", '9', 1, 2, 1, 0 }, { "(", '(', 1, 3, 1, 1 }, { "x", '*', 1, 4, 1, 1 },
    { "4", '4', 2, 0, 1, 0 }, { "5", '5', 2, 1, 1, 0 }, { "6", '6', 2, 2, 1, 0 }, { ")", ')', 2, 3, 1, 1 }, { "-", '-', 2, 4, 1, 1 },
    { "1", '1', 3, 0, 1, 0 }, { "2", '2', 3, 1, 1, 0 }, { "3", '3', 3, 2, 1, 0 }, { ".", '.', 3, 3, 1, 0 }, { "+", '+', 3, 4, 1, 1 },
    { "0", '0', 4, 0, 3, 0 }, { "=", '=', 4, 3, 2, 2 },
};
#define KEY_COUNT ((int)(sizeof keys / sizeof keys[0]))

static struct {
    lv_obj_t *display, *top, *big, *hist_title;
    lv_obj_t *btn[KEY_COUNT];
    lv_obj_t *hist_sum[HISTORY], *hist_ans[HISTORY];
    int focus_r, focus_c;            /* the focused cell of the 5 x 5 grid */
    char expr[CALC_MAX_LEN + 1];     /* what is typed, e.g. "12+3*4" */
    char top_text[CALC_MAX_LEN + 8]; /* the small line above */
    int top_is_error, top_shows_error;
    int just_answered;               /* expr holds an answer: a new number starts a new sum */
    char hist[HISTORY][2][CALC_MAX_LEN + 1];   /* newest first: the sum, the answer */
    int hist_count;
    int open;
} C;

/* which key covers cell (r, c) */
static int key_at(int r, int c)
{
    for (int i = 0; i < KEY_COUNT; i++)
        if (keys[i].row == r && c >= keys[i].col && c < keys[i].col + keys[i].span) return i;
    return 0;
}

static int is_op(char c) { return c == '+' || c == '-' || c == '*' || c == '/'; }

/* ---------- showing ---------- */

/* "12+-3*4" -> "12 + -3 x 4": spaces around the operators, x for times */
static void pretty(const char *e, char *out, size_t n)
{
    size_t o = 0;
    for (int i = 0; e[i] && o + 4 < n; i++) {
        char c = e[i];
        int unary = c == '-' && (i == 0 || is_op(e[i - 1]) || e[i - 1] == '(');
        if (is_op(c) && !unary) {
            out[o++] = ' ';
            out[o++] = c == '*' ? 'x' : c;
            out[o++] = ' ';
        } else out[o++] = c;
    }
    out[o] = 0;
}

/* set a label's text, cutting it from the left ("...345+6") so the end stays visible */
static void set_fitted(lv_obj_t *label, const char *text, lv_coord_t width)
{
    const lv_font_t *f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const char *s = text;
    char buf[CALC_MAX_LEN * 3 + 8];
    while (*s && lv_txt_get_width(s, (uint32_t)strlen(s), f, 0, LV_TEXT_FLAG_NONE) > width - lv_txt_get_width("...", 3, f, 0, LV_TEXT_FLAG_NONE)) s++;
    if (s != text) { snprintf(buf, sizeof buf, "...%s", s); lv_label_set_text(label, buf); }
    else lv_label_set_text(label, text);
}

static void show(void)
{
    char p[CALC_MAX_LEN * 3 + 1];
    lv_obj_update_layout(C.display);
    lv_coord_t w = lv_obj_get_content_width(C.display);
    if (w < 40) w = 400;
    pretty(C.expr, p, sizeof p);
    set_fitted(C.big, C.expr[0] ? p : "0", w);

    /* the small line: the answer so far while typing */
    if (!C.just_answered && !C.top_is_error) {
        double v;
        char err[64], num[48];
        int has_op = 0;
        for (int i = 1; C.expr[i]; i++) if (is_op(C.expr[i]) || C.expr[i] == '%') has_op = 1;
        if (has_op && calc_eval(C.expr, &v, err, sizeof err) == 0) {
            calc_format(v, num, sizeof num);
            snprintf(C.top_text, sizeof C.top_text, "= %s", num);
        } else C.top_text[0] = 0;
    }
    if (C.top_is_error != C.top_shows_error) {        /* an error is shown in the accent colour */
        lv_obj_remove_style(C.top, th_style(C.top_is_error ? TS_MUTED : TS_TITLE), 0);
        th_add(C.top, C.top_is_error ? TS_TITLE : TS_MUTED);
        C.top_shows_error = C.top_is_error;
    }
    set_fitted(C.top, C.top_text[0] ? C.top_text : " ", w);
}

static void show_history(void)
{
    char p[CALC_MAX_LEN * 3 + 1];
    for (int i = 0; i < HISTORY; i++) {
        if (i < C.hist_count) {
            pretty(C.hist[i][0], p, sizeof p);
            lv_label_set_text(C.hist_sum[i], p);
            lv_label_set_text_fmt(C.hist_ans[i], "= %s", C.hist[i][1]);
            lv_obj_clear_flag(C.hist_sum[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(C.hist_ans[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(C.hist_sum[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(C.hist_ans[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(C.hist_title, C.hist_count ? "History" : "History\n\nYour answers\nshow up here");
}

/* the focused key gets the selection look */
static void show_focus(void)
{
    int f = key_at(C.focus_r, C.focus_c);
    for (int i = 0; i < KEY_COUNT; i++) {
        if (i == f) lv_obj_add_state(C.btn[i], LV_STATE_CHECKED);
        else lv_obj_clear_state(C.btn[i], LV_STATE_CHECKED);
    }
}

/* colours and fonts that come from the theme but are set on single keys */
static void style_keys(void)
{
    const NzTheme *t = theme();
    for (int i = 0; i < KEY_COUNT; i++) {
        lv_obj_t *b = C.btn[i];
        /* (CRT: the heading font is 24 px wide per letter, too wide for "DEL" and "+/-") */
        lv_obj_set_style_text_font(b, t->crt && strlen(keys[i].label) > 1 ? t->font_body : t->font_heading, 0);
        if (keys[i].kind == 1) lv_obj_set_style_text_color(b, th_color(t->accent), 0);   /* operators */
        if (keys[i].kind == 2) {                       /* "=": a ring in the text colour when focused */
            lv_obj_set_style_outline_color(b, th_color(t->text), LV_STATE_CHECKED);
            lv_obj_set_style_outline_width(b, 3, LV_STATE_CHECKED);
            lv_obj_set_style_outline_pad(b, 2, LV_STATE_CHECKED);
        }
    }
}

static void theme_changed(void) { if (C.open) { style_keys(); show(); } }

/* ---------- typing ---------- */

/* the start of the last number in expr (where +/- puts its sign) */
static int last_number_start(void)
{
    int i = (int)strlen(C.expr);
    while (i > 0 && ((C.expr[i - 1] >= '0' && C.expr[i - 1] <= '9') || C.expr[i - 1] == '.')) i--;
    return i;
}

static void add_char(char c)
{
    size_t n = strlen(C.expr);
    if (n >= CALC_MAX_LEN) { ui_toast("That sum is long enough"); return; }
    C.expr[n] = c;
    C.expr[n + 1] = 0;
}

static void press(char act)
{
    size_t n = strlen(C.expr);
    char last = n ? C.expr[n - 1] : 0;
    int was_answer = C.just_answered;
    C.top_is_error = 0;
    if (act != '=' ) C.just_answered = 0;

    if ((act >= '0' && act <= '9') || act == '.' || act == '(') {
        if (was_answer) { C.expr[0] = 0; n = 0; last = 0; }              /* a new sum */
        if (act == '.') {
            int s = last_number_start();
            if (strchr(C.expr + s, '.')) return;                         /* one dot per number */
            if (s == (int)n) add_char('0');                              /* ".5" -> "0.5" */
        }
        add_char(act);
    } else if (is_op(act)) {
        if (!n && act != '-') add_char('0');                             /* "+3" -> "0+3" */
        else if (is_op(last) && n > 1) C.expr[--n] = 0;                  /* change the operator */
        else if (is_op(last)) return;                                    /* just "-": keep it */
        add_char(act);
    } else if (act == ')' || act == '%') {
        if (n) add_char(act);
    } else if (act == 'n') {                                             /* +/- */
        int s = last_number_start();
        char before = s > 0 ? C.expr[s - 1] : 0;
        int unary = before == '-' && (s == 1 || is_op(C.expr[s - 2]) || C.expr[s - 2] == '(');
        if (unary) memmove(C.expr + s - 1, C.expr + s, n - (size_t)s + 1);   /* -3 -> 3 */
        else if (before == '+' && s > 1) C.expr[s - 1] = '-';               /* 2+3 -> 2-3 */
        else if (before == '-' && s > 1) C.expr[s - 1] = '+';               /* 2-3 -> 2+3 */
        else if (n < CALC_MAX_LEN) {                                     /* 3 -> -3 */
            memmove(C.expr + s + 1, C.expr + s, n - (size_t)s + 1);
            C.expr[s] = '-';
        }
    } else if (act == 'C') {
        C.expr[0] = 0;
        C.top_text[0] = 0;
    } else if (act == 'D') {
        if (n) C.expr[n - 1] = 0;
        C.top_text[0] = 0;
    } else if (act == '=') {
        double v;
        char err[64], num[48];
        if (!n || C.just_answered) return;
        if (calc_eval(C.expr, &v, err, sizeof err) != 0) {
            snprintf(C.top_text, sizeof C.top_text, "%s", err);
            C.top_is_error = 1;
        } else {
            calc_format(v, num, sizeof num);
            memmove(C.hist[1], C.hist[0], sizeof C.hist[0] * (HISTORY - 1));   /* newest first */
            snprintf(C.hist[0][0], sizeof C.hist[0][0], "%s", C.expr);
            snprintf(C.hist[0][1], sizeof C.hist[0][1], "%s", num);
            if (C.hist_count < HISTORY) C.hist_count++;
            char p[CALC_MAX_LEN * 3 + 1];
            pretty(C.expr, p, sizeof p);
            snprintf(C.top_text, sizeof C.top_text, "%.*s =", CALC_MAX_LEN, p);
            snprintf(C.expr, sizeof C.expr, "%s", num);
            C.just_answered = 1;
            show_history();
        }
    }
    show();
}

static void key_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    C.focus_r = keys[i].row;
    C.focus_c = keys[i].col;
    show_focus();
    press(keys[i].act);
}

/* ---------- building ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&C, 0, sizeof C);
    C.focus_r = 2;                                   /* start on "5", in the middle */
    C.focus_c = 1;
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    /* the display */
    C.display = th_box(root, TS_PANEL);
    lv_obj_set_size(C.display, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(C.display, 0, 0);
    lv_obj_set_style_border_side(C.display, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(C.display, 0, 0);
    lv_obj_set_style_pad_hor(C.display, 16, 0);
    lv_obj_set_style_pad_ver(C.display, 8, 0);
    lv_obj_set_flex_flow(C.display, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(C.display, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    C.top = th_label(C.display, " ", TS_MUTED);
    C.big = th_label(C.display, "0", TS_BIG);
    lv_obj_set_width(C.top, LV_PCT(100));
    lv_obj_set_width(C.big, LV_PCT(100));
    lv_label_set_long_mode(C.top, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(C.big, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(C.top, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(C.big, LV_TEXT_ALIGN_RIGHT, 0);

    /* keys (left) + history (right) */
    lv_obj_t *mid = th_box(root, -1);
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(mid, 8, 0);
    lv_obj_set_style_pad_column(mid, 10, 0);

    static lv_coord_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t rows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *pad = th_box(mid, -1);
    lv_obj_set_height(pad, LV_PCT(100));
    lv_obj_set_flex_grow(pad, 3);
    lv_obj_set_grid_dsc_array(pad, cols, rows);
    lv_obj_set_style_pad_row(pad, 6, 0);
    lv_obj_set_style_pad_column(pad, 6, 0);
    lv_obj_set_style_pad_all(pad, 3, 0);                 /* room for the focus ring */
    for (int i = 0; i < KEY_COUNT; i++) {
        lv_obj_t *b = th_box(pad, keys[i].kind == 2 ? TS_BTN_ACCENT : TS_BTN);
        if (keys[i].kind == 2) th_add_to(b, TS_BTN_ACCENT, LV_STATE_CHECKED);
        else th_add_to(b, TS_TILE_SEL, LV_STATE_CHECKED);
        th_add_to(b, TS_BTN_ACCENT, LV_STATE_PRESSED);    /* a click flashes the accent colour */
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, keys[i].col, keys[i].span, LV_GRID_ALIGN_STRETCH, keys[i].row, 1);
        lv_obj_t *l = lv_label_create(b);                /* (takes the key's colour and font) */
        lv_label_set_text(l, keys[i].label);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, key_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        C.btn[i] = b;
    }

    lv_obj_t *hist = th_box(mid, TS_PANEL);
    lv_obj_set_height(hist, LV_PCT(100));
    lv_obj_set_flex_grow(hist, 2);
    lv_obj_set_style_pad_all(hist, 10, 0);
    lv_obj_set_style_pad_row(hist, 1, 0);
    lv_obj_set_flex_flow(hist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hist, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    C.hist_title = th_label(hist, "History", TS_SMALL);
    lv_obj_set_width(C.hist_title, LV_PCT(100));
    for (int i = 0; i < HISTORY; i++) {
        C.hist_sum[i] = th_label(hist, "", TS_SMALL);
        C.hist_ans[i] = th_label(hist, "", TS_TEXT);
        lv_obj_set_width(C.hist_sum[i], LV_PCT(100));
        lv_obj_set_width(C.hist_ans[i], LV_PCT(100));
        lv_label_set_long_mode(C.hist_sum[i], LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(C.hist_ans[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(C.hist_sum[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_align(C.hist_ans[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_pad_bottom(C.hist_ans[i], 4, 0);
    }

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Press", "START", "=", "X", "Clear", "B", "Delete" };
    lv_obj_center(nz_hints(hint, hints, 4));

    static int listening;                               /* follow live theme changes */
    if (!listening) { theme_on_change(theme_changed); listening = 1; }
    C.open = 1;
    style_keys();
    show_focus();
    show_history();
    show();
}

/* move the focus one key in a direction (keys can be wider than one cell) */
static void move(int dr, int dc)
{
    int k = key_at(C.focus_r, C.focus_c);
    if (dc > 0) { int c = keys[k].col + keys[k].span; if (c < COLS) C.focus_c = c; }
    if (dc < 0) { int c = keys[k].col - 1; if (c >= 0) C.focus_c = c; }
    if (dr) { int r = C.focus_r + dr; if (r >= 0 && r < ROWS) C.focus_r = r; }
    show_focus();
}

static int key(NzApp *a, int k)
{
    (void)a;
    switch (k) {
    case NZ_KEY_UP:    move(-1, 0); return 1;
    case NZ_KEY_DOWN:  move(1, 0); return 1;
    case NZ_KEY_LEFT:  move(0, -1); return 1;
    case NZ_KEY_RIGHT: move(0, 1); return 1;
    case NZ_KEY_A:     press(keys[key_at(C.focus_r, C.focus_c)].act); return 1;
    case NZ_KEY_START: press('='); return 1;
    case NZ_KEY_X:     press('C'); return 1;
    case NZ_KEY_B:
        if (!C.expr[0]) return 0;                         /* nothing typed: B leaves */
        press('D');
        return 1;
    default: return 0;
    }
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    (void)a;
    switch (ev->key) {
    case KB_CHAR:
        if (strchr("0123456789.+-*/()%", ev->ch)) press(ev->ch);
        else if (ev->ch == 'x' || ev->ch == 'X') press('*');
        else if (ev->ch == '=') press('=');
        else if (ev->ch == 'c' || ev->ch == 'C') press('C');
        break;
    case KB_ENTER:     press('='); break;
    case KB_BACKSPACE: case KB_DELETE: press('D'); break;
    case KB_ESC:       press('C'); break;
    case KB_UP:        move(-1, 0); break;
    case KB_DOWN:      move(1, 0); break;
    case KB_LEFT:      move(0, -1); break;
    case KB_RIGHT:     move(0, 1); break;
    default: break;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    memset(&C, 0, sizeof C);                            /* (C.open = 0: theme changes are ignored) */
}

/* for tests */
const char *app_calculator_expr(void) { return C.expr; }
const char *app_calculator_top(void) { return C.top_text; }
int app_calculator_history_count(void) { return C.hist_count; }
const char *app_calculator_focused(void) { return keys[key_at(C.focus_r, C.focus_c)].label; }
lv_obj_t *app_calculator_key_obj(const char *label)
{
    for (int i = 0; i < KEY_COUNT; i++) if (!strcmp(keys[i].label, label)) return C.btn[i];
    return NULL;
}

NzApp app_calculator = {
    .id = APP_CALCULATOR, .title = "Calculator", .icon = ICON_CALCULATOR,
    .create = create, .key = key, .kbd = kbd, .destroy = destroy,
};

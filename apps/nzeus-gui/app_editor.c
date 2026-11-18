/* app_editor.c - the NZEUS text Editor (replaces FrogShell's editor).
 *
 * The text engine (editor.c: smart indents, auto-closing brackets, CRLF kept)
 * and the colouring (highlight.c: VS Code "Dark+" colours for Python, shell
 * and C/Arduino) were first written for our FrogShell add-ons; this file draws them with LVGL.
 *
 * Buttons (handheld mode):
 *   D-pad    move the cursor        L1 / R1   page up / down
 *   A        on-screen keyboard (then: D-pad + A type, B hide it, X backspace)
 *   START    save                   X         menu (save, close, go to top/end,
 *                                              copy this line, paste)
 *   B        close (asks first if there are unsaved changes)
 * USB keyboard: type normally; Ctrl+S saves, Ctrl+V pastes, Esc closes,
 * Tab / Shift+Tab indent. Copy/paste use the clipboard shared with the Terminal.
 * Mouse: click to place the cursor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "editor.h"
#include "fsops.h"
#include "highlight.h"
#include "home.h"
#include "nzui.h"
#include "ui.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"

LV_FONT_DECLARE(lv_font_nzeus_mono_16);

#define CHAR_W      8
#define LINE_H      18
#define GUTTER_COLS 5          /* "1234 " */


static struct {
    lv_obj_t *status, *view, *kb, *hint;
    int scroll_row, scroll_col;            /* first line / column on screen */
    HlLang lang;
    int *states;                           /* colouring state at the start of each line */
    int states_cap, states_valid;          /* states[0 .. states_valid-1] are up to date */
    int file_open;
} E;

static char open_path[FS_PATH_MAX];
#define NOTES_FILE NZ_SDCARD "/nzeus/notes.txt"

/* ---------- colouring ---------- */
static void colour_from(int row) { if (row < E.states_valid) E.states_valid = row + 1; }

/* make sure states[] is valid up to line `upto` */
static void colour_upto(int upto)
{
    static unsigned char scratch[4096];
    if (upto >= E.states_cap) {
        int cap = upto + 256;
        int *s = realloc(E.states, (size_t)cap * sizeof *s);
        if (!s) return;
        E.states = s;
        E.states_cap = cap;
    }
    if (E.states_valid < 1) { E.states[0] = 0; E.states_valid = 1; }
    for (int r = E.states_valid; r <= upto; r++) {
        int len = editor_line_len(r - 1);
        if (len > (int)sizeof scratch) len = sizeof scratch;
        E.states[r] = hl_line(E.lang, editor_line(r - 1), len, E.states[r - 1], scratch);
    }
    if (upto + 1 > E.states_valid) E.states_valid = upto + 1;
}

/* ---------- drawing ---------- */
static void status_update(void)
{
    const char *p = editor_path(), *base = p ? strrchr(p, '/') : NULL;
    lv_label_set_text_fmt(E.status, "%s%s     line %d, col %d", base ? base + 1 : (p ? p : "?"),
                          editor_modified() ? "  (not saved)" : "",
                          editor_cursor_row() + 1, editor_cursor_col() + 1);
}

static void changed(void)
{
    colour_from(editor_take_dirty_row());     /* re-colour from the first changed line on */
    status_update();
    lv_obj_invalidate(E.view);
}

static void draw_view(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int rows = (a.y2 - a.y1 + 1) / LINE_H, cols = (a.x2 - a.x1 + 1) / CHAR_W - GUTTER_COLS;
    if (rows < 1 || cols < 1) return;

    int cr = editor_cursor_row(), cc = editor_cursor_col();       /* keep the cursor on screen */
    if (cr < E.scroll_row) E.scroll_row = cr;
    if (cr >= E.scroll_row + rows) E.scroll_row = cr - rows + 1;
    if (cc < E.scroll_col) E.scroll_col = cc;
    if (cc >= E.scroll_col + cols) E.scroll_col = cc - cols + 1;

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_nzeus_mono_16;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);

    int last = E.scroll_row + rows - 1;
    if (last >= editor_line_count()) last = editor_line_count() - 1;
    colour_upto(last);
    static unsigned char kinds[4096];
    char piece[256];

    for (int r = E.scroll_row; r <= last; r++) {
        int y = a.y1 + (r - E.scroll_row) * LINE_H;
        int x0 = a.x1 + GUTTER_COLS * CHAR_W;
        if (r == cr) {                                            /* the cursor's line */
            lv_area_t hl = { x0, y, a.x2, y + LINE_H - 1 };
            rd.bg_color = th_color(theme()->ed_line);
            lv_draw_rect(ctx, &rd, &hl);
        }
        char num[12];                                             /* line number */
        snprintf(num, sizeof num, "%4d", r + 1);
        lv_area_t na = { a.x1, y + 1, x0 - 1, y + LINE_H };
        ld.color = th_color(theme()->ed_gutter);
        lv_draw_label(ctx, &ld, &na, num, NULL);

        const char *s = editor_line(r);                           /* the text, in coloured runs */
        int len = editor_line_len(r);
        if (len > (int)sizeof kinds) len = sizeof kinds;
        hl_line(E.lang, s, len, E.states[r], kinds);
        int end = E.scroll_col + cols < len ? E.scroll_col + cols : len;
        for (int i = E.scroll_col; i < end;) {
            int j = i;
            while (j < end && kinds[j] == kinds[i] && j - i < (int)sizeof piece - 1) j++;
            snprintf(piece, sizeof piece, "%.*s", j - i, s + i);
            lv_area_t pa = { x0 + (i - E.scroll_col) * CHAR_W, y + 1, a.x2, y + LINE_H };
            ld.color = th_color(theme()->syntax[kinds[i]]);
            lv_draw_label(ctx, &ld, &pa, piece, NULL);
            i = j;
        }
    }
    int cx = a.x1 + (GUTTER_COLS + cc - E.scroll_col) * CHAR_W;  /* the cursor: a thin bar */
    int cy = a.y1 + (cr - E.scroll_row) * LINE_H;
    lv_area_t ca = { cx, cy + 1, cx + 1, cy + LINE_H - 2 };
    rd.bg_color = th_color(theme()->ed_cursor);
    lv_draw_rect(ctx, &rd, &ca);
}

/* mouse: click to put the cursor there */
static void view_clicked(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    int row = E.scroll_row + (p.y - a.y1) / LINE_H;
    int col = E.scroll_col + (p.x - a.x1) / CHAR_W - GUTTER_COLS;
    editor_set_cursor(row, col < 0 ? 0 : col);
    changed();
}

/* ---------- saving and closing ---------- */
static void save(void)
{
    if (editor_save()) { sync(); ui_toast("Saved"); }
    else { char m[96]; snprintf(m, sizeof m, "Not saved: %s", editor_error()); ui_toast(m); }
    status_update();
}

static NzApp *self;
static void close_now(void) { if (self) apps_close(self); }

static void close_answer(int choice, void *ctx)
{
    (void)ctx;
    if (choice == 0) { save(); if (!editor_modified()) close_now(); }   /* Save and close */
    else if (choice == 1) close_now();                                  /* Close without saving */
}

static void ask_close(void)
{
    if (!editor_modified()) { close_now(); return; }
    static const char *const items[] = { "Save and close", "Close without saving", "Keep editing" };
    nz_menu("This file has changes", items, 3, close_answer, NULL);
}

/* ---------- search ---------- */
static char find_text[128];

/* case doesn't matter: "print" finds "Print" */
static const char *find_in(const char *line, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = line; *p; p++) if (!strncasecmp(p, word, n)) return p;
    return NULL;
}

/* The next place after the cursor where find_text is, going round to the
 * top at the end. 1 = found (the cursor is put there). */
int app_editor_find_next(void)
{
    if (!find_text[0] || !E.file_open) return 0;
    int rows = editor_line_count(), r0 = editor_cursor_row(), c0 = editor_cursor_col() + 1;
    for (int k = 0; k <= rows; k++) {
        int r = (r0 + k) % rows;
        const char *line = editor_line(r);
        int from = k == 0 ? c0 : 0;
        if (from > editor_line_len(r)) continue;
        const char *hit = find_in(line + from, find_text);
        if (hit) { editor_set_cursor(r, (int)(hit - line)); changed(); return 1; }
    }
    ui_toast("Not found");
    return 0;
}

static void find_answer(const char *text, void *ctx)
{
    (void)ctx;
    if (!text || !text[0]) return;
    snprintf(find_text, sizeof find_text, "%s", text);
    app_editor_find_next();
}

/* New file in the same folder as this one */
int app_editor_open_file(const char *path);
static void new_answer(const char *name, void *ctx)
{
    (void)ctx;
    if (!name || !name[0] || strchr(name, '/')) return;
    char dir[FS_PATH_MAX], p[FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", open_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    fs_join(p, sizeof p, dir, name);
    if (access(p, F_OK) == 0) { ui_toast("That name already exists"); return; }
    FILE *f = fopen(p, "w");
    if (!f) { ui_toast("Could not create the file"); return; }
    fclose(f);
    if (editor_modified()) save();                     /* don't lose this file's changes */
    app_editor_open_file(p);
}

static void menu_answer(int choice, void *ctx)
{
    (void)ctx;
    switch (choice) {
    case 0: save(); break;
    case 1: nz_prompt("Find", find_text, find_answer, NULL); break;
    case 2: if (!find_text[0]) nz_prompt("Find", "", find_answer, NULL); else app_editor_find_next(); break;
    case 3: editor_set_cursor(0, 0); changed(); break;
    case 4: editor_set_cursor(editor_line_count() - 1, 1 << 30); changed(); break;
    case 5: nz_prompt("New file name (e.g. robot.py)", "", new_answer, NULL); break;
    case 6:                                            /* the shared clipboard (nzui.h) */
        nz_clip_set(editor_line(editor_cursor_row()));
        ui_toast(editor_line_len(editor_cursor_row()) ? "Line copied" : "That line is empty");
        break;
    case 7:
        if (!nz_clip_get()[0]) { ui_toast("Nothing copied yet"); break; }
        editor_insert_text(nz_clip_get());             /* as it is: no auto brackets */
        changed();
        break;
    case 8: ask_close(); break;
    default: break;
    }
}

/* ---------- the on-screen keyboard ---------- */
static void show_keyboard(int show)
{
    if (show) { lv_obj_clear_flag(E.kb, LV_OBJ_FLAG_HIDDEN); lv_group_focus_obj(E.kb); }
    else lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(E.hint, LV_OBJ_FLAG_HIDDEN);
    if (!show) lv_obj_clear_flag(E.hint, LV_OBJ_FLAG_HIDDEN);
}

/* A key on the on-screen keyboard was pressed: type it into the file.
 * This replaces LVGL's own keyboard handler: we must read the pressed key
 * BEFORE a layout key ("1#", "ABC", "abc") swaps the layout, then let LVGL
 * do only the layout switch. */
static void kb_pressed(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(kb);
    if (id == LV_BTNMATRIX_BTN_NONE) return;
    const char *t = lv_btnmatrix_get_btn_text(kb, id);
    if (!t) return;
    if (!strcmp(t, "ABC") || !strcmp(t, "abc") || !strcmp(t, "1#")) {          /* layout keys */
        lv_keyboard_def_event_cb(e);
        return;
    }
    if (!strcmp(t, LV_SYMBOL_BACKSPACE)) editor_backspace();
    else if (!strcmp(t, LV_SYMBOL_NEW_LINE)) editor_newline();
    else if (!strcmp(t, LV_SYMBOL_LEFT)) editor_move(0, -1);
    else if (!strcmp(t, LV_SYMBOL_RIGHT)) editor_move(0, 1);
    else if (!strcmp(t, LV_SYMBOL_OK) || !strcmp(t, LV_SYMBOL_KEYBOARD)) { show_keyboard(0); return; }
    else for (; *t; t++) editor_type_char(*t);
    changed();
}

/* ---------- opening files ---------- */
int app_editor_open_file(const char *path)
{
    if (E.file_open) {                              /* another file is open: keep its changes safe */
        if (editor_modified() && strcmp(path, open_path) != 0) {
            ui_toast("Save or close the open file first");
            apps_open(APP_EDITOR);
            return 1;
        }
    }
    if (!editor_open(path)) {
        char m[160];
        snprintf(m, sizeof m, "Can't open: %s", editor_error());
        ui_toast(m);
        return 1;
    }
    snprintf(open_path, sizeof open_path, "%s", path);
    E.file_open = 1;
    E.lang = hl_language_for(path);
    editor_set_smart(E.lang == HL_LANG_PYTHON ? (EDIT_SMART_PAIRS | EDIT_PYTHON)
                   : E.lang == HL_LANG_NONE   ? 0 : (EDIT_SMART_PAIRS | EDIT_BRACES));
    E.states_valid = 0;
    E.scroll_row = E.scroll_col = 0;
    editor_take_dirty_row();
    apps_open(APP_EDITOR);
    if (E.view) { status_update(); lv_obj_invalidate(E.view); }
    return 1;
}

/* The text area is drawn by hand (draw_view), so a theme change must recolour
 * its background and redraw it. */
static void editor_theme_changed(void)
{
    if (!E.view) return;
    lv_obj_set_style_bg_color(E.view, th_color(theme()->ed_bg), 0);
    lv_obj_invalidate(E.view);
}

static void create(NzApp *a, lv_obj_t *root)
{
    static int listening;
    if (!listening) { theme_on_change(editor_theme_changed); listening = 1; }
    self = a;
    if (!E.file_open) {                             /* opened from its tile: your notes */
        FILE *f = fopen(NOTES_FILE, "a");
        if (f) fclose(f);
        if (editor_open(NOTES_FILE)) {
            snprintf(open_path, sizeof open_path, "%s", NOTES_FILE);
            E.file_open = 1;
            E.lang = HL_LANG_NONE;
            editor_set_smart(0);
        }
    }
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    E.status = th_label(root, "", TS_TEXT);         /* file name, changes, line / column */
    th_add(E.status, TS_BAR);
    lv_obj_set_width(E.status, LV_PCT(100));
    lv_obj_set_style_pad_all(E.status, 6, 0);
    lv_label_set_long_mode(E.status, LV_LABEL_LONG_DOT);

    E.view = th_box(root, -1);                      /* the text (drawn by draw_view) */
    lv_obj_set_style_bg_opa(E.view, LV_OPA_COVER, 0);
    lv_obj_set_width(E.view, LV_PCT(100));
    lv_obj_set_flex_grow(E.view, 1);
    lv_obj_add_flag(E.view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(E.view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(E.view, draw_view, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(E.view, view_clicked, LV_EVENT_CLICKED, NULL);

    E.kb = lv_keyboard_create(root);                /* on-screen keyboard, not attached to a box */
    lv_obj_set_size(E.kb, LV_PCT(100), 170);
    lv_obj_remove_event_cb(E.kb, lv_keyboard_def_event_cb);   /* ours replaces it (see kb_pressed) */
    lv_obj_add_event_cb(E.kb, kb_pressed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(apps_keypad_group(), E.kb);
    lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);

    E.hint = th_box(root, TS_BAR);                  /* button help */
    lv_obj_set_size(E.hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Keyboard", "START", "Save", "X", "Menu", "B", "Close", "L1/R1", "Page" };
    lv_obj_center(nz_hints(E.hint, hints, 5));
    editor_theme_changed();

    status_update();
}

static int key(NzApp *a, int k)
{
    (void)a;
    int kb_open = !lv_obj_has_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    if (kb_open) {
        switch (k) {
        case NZ_KEY_UP:    apps_keypad_send(LV_KEY_UP); return 1;
        case NZ_KEY_DOWN:  apps_keypad_send(LV_KEY_DOWN); return 1;
        case NZ_KEY_LEFT:  apps_keypad_send(LV_KEY_LEFT); return 1;
        case NZ_KEY_RIGHT: apps_keypad_send(LV_KEY_RIGHT); return 1;
        case NZ_KEY_A:     apps_keypad_send(LV_KEY_ENTER); return 1;
        case NZ_KEY_X:     editor_backspace(); changed(); return 1;
        case NZ_KEY_B:     show_keyboard(0); return 1;
        case NZ_KEY_START: save(); return 1;
        default: return 1;
        }
    }
    int page = lv_obj_get_height(E.view) / LINE_H - 1;
    switch (k) {
    case NZ_KEY_UP:    editor_move(-1, 0); break;
    case NZ_KEY_DOWN:  editor_move(1, 0); break;
    case NZ_KEY_LEFT:  editor_move(0, -1); break;
    case NZ_KEY_RIGHT: editor_move(0, 1); break;
    case NZ_KEY_L:     editor_move(-(page > 1 ? page : 1), 0); break;
    case NZ_KEY_R:     editor_move(page > 1 ? page : 1, 0); break;
    case NZ_KEY_A:     show_keyboard(1); return 1;
    case NZ_KEY_START: save(); return 1;
    case NZ_KEY_X: {
        static const char *const items[] = { "Save", "Find...", "Find next", "Go to the top",
                                             "Go to the end", "New file...", "Copy this line", "Paste", "Close" };
        nz_menu("Editor", items, 9, menu_answer, NULL);
        return 1;
    }
    case NZ_KEY_B:     ask_close(); return 1;      /* never close silently with unsaved changes */
    default:           return 0;
    }
    changed();
    return 1;
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    if (!lv_obj_has_flag(E.kb, LV_OBJ_FLAG_HIDDEN)) show_keyboard(0);   /* a real keyboard: hide ours */
    int page = lv_obj_get_height(E.view) / LINE_H - 1;
    switch (ev->key) {
    case KB_CHAR:      editor_type_char(ev->ch); break;
    case KB_ENTER:     editor_newline(); break;
    case KB_BACKSPACE: editor_backspace(); break;
    case KB_DELETE:    editor_delete(); break;
    case KB_TAB:       editor_tab(); break;
    case KB_BACKTAB:   editor_unindent(); break;
    case KB_UP:        editor_move(-1, 0); break;
    case KB_DOWN:      editor_move(1, 0); break;
    case KB_LEFT:      editor_move(0, -1); break;
    case KB_RIGHT:     editor_move(0, 1); break;
    case KB_HOME:      editor_home(); break;
    case KB_END:       editor_end(); break;
    case KB_PGUP:      editor_move(-(page > 1 ? page : 1), 0); break;
    case KB_PGDN:      editor_move(page > 1 ? page : 1, 0); break;
    case KB_CTRL_S:    save(); return;
    case KB_CTRL_OTHER:                                /* Ctrl+V: paste (the shared clipboard) */
        if (ev->ch != 'v' && ev->ch != 'V') return;
        editor_insert_text(nz_clip_get());
        break;
    case KB_ESC:       ask_close(); return;
    default:           return;
    }
    (void)a;
    changed();
}

static void destroy(NzApp *a)
{
    (void)a;
    editor_close();
    E.file_open = 0;
    open_path[0] = 0;
    free(E.states);
    E.states = NULL;
    E.states_cap = E.states_valid = 0;
    E.view = E.kb = E.status = E.hint = NULL;
}

/* for tests */
const char *app_editor_path(void) { return E.file_open ? open_path : ""; }

NzApp app_editor = {
    .icon = ICON_EDITOR,
    .id = APP_EDITOR, .title = "Editor",
    .create = create, .key = key, .kbd = kbd, .destroy = destroy,
};

/* nzui.c - pop-ups shared by the NZEUS apps: menu, text prompt, yes/no;
 * and the shared text clipboard (copy / paste).
 * They live on LVGL's top layer, above every screen, behind a dark backdrop
 * that also stops clicks from reaching what is underneath. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "nzui.h"
#include "apps.h"
#include "home.h"
#include "theme.h"

#define MAX_ITEMS  16

enum { NONE, MENU, PROMPT, CONFIRM };
static int kind = NONE;
static lv_obj_t *backdrop, *box, *rows[MAX_ITEMS], *ta, *kb;
static int row_count, selected;
static void *cb_ctx;
static nz_menu_cb menu_cb;
static nz_prompt_cb prompt_cb;
static nz_confirm_cb confirm_cb;

int nzui_active(void) { return kind != NONE; }
int nzui_selected(void) { return selected; }

/* ---------- the shared clipboard ---------- */
static char *clip;                       /* malloc'd copy, NULL = empty */

void nz_clip_set(const char *text)
{
    free(clip);
    clip = NULL;
    if (!text || !text[0]) return;
    size_t n = strlen(text);
    if (n > NZ_CLIP_MAX) n = NZ_CLIP_MAX;
    clip = malloc(n + 1);
    if (!clip) return;
    memcpy(clip, text, n);
    clip[n] = 0;
}

const char *nz_clip_get(void) { return clip ? clip : ""; }

/* ---------- button hints: [A] Open  [B] Back ---------- */
lv_obj_t *nz_hints(lv_obj_t *parent, const char *const pairs[], int count)
{
    lv_obj_t *row = th_box(parent, -1);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    for (int i = 0; i < count; i++) {
        th_label(row, pairs[2 * i], TS_KEYCAP);                       /* the button's name, in a key cap */
        lv_obj_t *what = th_label(row, pairs[2 * i + 1], TS_SMALL); /* what it does */
        if (i < count - 1) lv_obj_set_style_pad_right(what, 12, 0);
    }
    return row;
}

/* the dark backdrop and the box in the middle */
static void open_box(const char *title, int width)
{
    backdrop = th_box(lv_layer_top(), -1);
    lv_obj_set_size(backdrop, 640, 480);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);       /* swallow clicks behind it */
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);

    box = th_box(backdrop, TS_PANEL);
    lv_obj_set_width(box, width);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);
    lv_obj_t *t = th_label(box, title, TS_TITLE);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_style_pad_bottom(t, 6, 0);
}

static void close_box(void)
{
    /* async: we are often inside an event of one of the pop-up's own buttons */
    if (backdrop) lv_obj_del_async(backdrop);
    backdrop = box = ta = kb = NULL;
    row_count = 0;
    kind = NONE;
}

/* ---------- menu ---------- */
static void show_selection(void)
{
    for (int i = 0; i < row_count; i++) {
        if (i == selected) lv_obj_add_state(rows[i], LV_STATE_CHECKED);
        else lv_obj_clear_state(rows[i], LV_STATE_CHECKED);
    }
}

static void finish_menu(int choice)
{
    nz_menu_cb cb = menu_cb;
    void *ctx = cb_ctx;
    close_box();                         /* close first: the callback may open another pop-up */
    if (cb) cb(choice, ctx);
}

static void row_clicked(lv_event_t *e) { finish_menu((int)(intptr_t)lv_event_get_user_data(e)); }

void nz_menu(const char *title, const char *const items[], int count, nz_menu_cb cb, void *ctx)
{
    if (kind != NONE) close_box();
    kind = MENU;
    menu_cb = cb;
    cb_ctx = ctx;
    open_box(title, 280);
    row_count = count > MAX_ITEMS ? MAX_ITEMS : count;
    for (int i = 0; i < row_count; i++) {
        rows[i] = th_box(box, TS_TEXT);
        th_add_to(rows[i], TS_ROW_SEL, LV_STATE_CHECKED);
        lv_obj_set_size(rows[i], LV_PCT(100), 30);
        lv_obj_add_flag(rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(rows[i]);          /* (inherits the row's text colour) */
        lv_label_set_text(l, items[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(rows[i], row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    selected = 0;
    show_selection();
}

/* ---------- text prompt ---------- */
static void finish_prompt(int ok)
{
    char text[2048] = "";                       /* (long enough for a WhatsApp message) */
    if (ok && ta) snprintf(text, sizeof text, "%s", lv_textarea_get_text(ta));
    nz_prompt_cb cb = prompt_cb;
    void *ctx = cb_ctx;
    close_box();
    if (cb) cb(ok ? text : NULL, ctx);
}

static void prompt_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) finish_prompt(1);          /* keyboard OK / Enter */
    else if (code == LV_EVENT_CANCEL) finish_prompt(0);    /* keyboard's close key */
}

void nz_prompt(const char *title, const char *initial, nz_prompt_cb cb, void *ctx)
{
    if (kind != NONE) close_box();
    kind = PROMPT;
    prompt_cb = cb;
    cb_ctx = ctx;
    open_box(title, 600);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 16);
    ta = lv_textarea_create(box);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_text(ta, initial ? initial : "");
    lv_obj_add_state(ta, LV_STATE_FOCUSED);                /* show the text cursor */
    lv_obj_add_event_cb(ta, prompt_event, LV_EVENT_READY, NULL);
    kb = lv_keyboard_create(box);
    lv_obj_set_size(kb, LV_PCT(100), 190);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, prompt_event, LV_EVENT_CANCEL, NULL);
    lv_group_add_obj(apps_keypad_group(), kb);
    lv_group_focus_obj(kb);
    static const char *const hints[] = { "D-pad+A", "Type", "X", "Delete", "START", "Done", "B", "Cancel" };
    nz_hints(box, hints, 4);
}

/* ---------- yes / no ---------- */
static void finish_confirm(int yes)
{
    nz_confirm_cb cb = confirm_cb;
    void *ctx = cb_ctx;
    close_box();
    if (cb) cb(yes, ctx);
}

static void yes_clicked(lv_event_t *e) { (void)e; finish_confirm(1); }
static void no_clicked(lv_event_t *e) { (void)e; finish_confirm(0); }

void nz_confirm(const char *question, nz_confirm_cb cb, void *ctx)
{
    if (kind != NONE) close_box();
    kind = CONFIRM;
    confirm_cb = cb;
    cb_ctx = ctx;
    open_box(question, 360);
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_t *no = lv_btn_create(row);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, "B  No");
    lv_obj_t *yes = lv_btn_create(row);
    th_add(yes, TS_BTN_DANGER);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, "A  Yes");
    lv_obj_add_event_cb(no, no_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(yes, yes_clicked, LV_EVENT_CLICKED, NULL);
}

/* ---------- buttons and keys ---------- */
int nzui_key(int key)
{
    switch (kind) {
    case MENU:
        if (key == NZ_KEY_UP && selected > 0) { selected--; show_selection(); }
        else if (key == NZ_KEY_DOWN && selected < row_count - 1) { selected++; show_selection(); }
        else if (key == NZ_KEY_A) finish_menu(selected);
        else if (key == NZ_KEY_B) finish_menu(-1);
        return 1;
    case PROMPT:
        switch (key) {                   /* the D-pad drives the on-screen keyboard */
        case NZ_KEY_UP:    apps_keypad_send(LV_KEY_UP); break;
        case NZ_KEY_DOWN:  apps_keypad_send(LV_KEY_DOWN); break;
        case NZ_KEY_LEFT:  apps_keypad_send(LV_KEY_LEFT); break;
        case NZ_KEY_RIGHT: apps_keypad_send(LV_KEY_RIGHT); break;
        case NZ_KEY_A:     apps_keypad_send(LV_KEY_ENTER); break;
        case NZ_KEY_X:     lv_textarea_del_char(ta); break;
        case NZ_KEY_START: finish_prompt(1); break;
        case NZ_KEY_B:     finish_prompt(0); break;
        default: break;
        }
        return 1;
    case CONFIRM:
        if (key == NZ_KEY_A) finish_confirm(1);
        else if (key == NZ_KEY_B) finish_confirm(0);
        return 1;
    default:
        return 0;
    }
}

void nzui_kbd(const KbEvent *ev)
{
    if (kind == PROMPT) {
        if (ev->key == KB_CHAR) lv_textarea_add_char(ta, (uint32_t)(unsigned char)ev->ch);
        else if (ev->key == KB_BACKSPACE) lv_textarea_del_char(ta);
        else if (ev->key == KB_LEFT) lv_textarea_cursor_left(ta);
        else if (ev->key == KB_RIGHT) lv_textarea_cursor_right(ta);
        else if (ev->key == KB_ENTER) finish_prompt(1);
        else if (ev->key == KB_ESC) finish_prompt(0);
    } else if (kind == MENU) {
        if (ev->key == KB_UP) nzui_key(NZ_KEY_UP);
        else if (ev->key == KB_DOWN) nzui_key(NZ_KEY_DOWN);
        else if (ev->key == KB_ENTER) nzui_key(NZ_KEY_A);
        else if (ev->key == KB_ESC) nzui_key(NZ_KEY_B);
    } else if (kind == CONFIRM) {
        if (ev->key == KB_ENTER || (ev->key == KB_CHAR && (ev->ch == 'y' || ev->ch == 'Y'))) finish_confirm(1);
        else if (ev->key == KB_ESC || (ev->key == KB_CHAR && (ev->ch == 'n' || ev->ch == 'N'))) finish_confirm(0);
    }
}

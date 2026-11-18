/* nzlist.c - the shared NZEUS list (see nzlist.h). */
#include <stdlib.h>
#include "lvgl.h"
#include "artext.h"
#include "home.h"
#include "icons.h"
#include "nzlist.h"
#include "theme.h"

#define MAX_ITEMS 256

struct NzList {
    lv_obj_t *box;                 /* the scrolling container */
    lv_obj_t *rows[MAX_ITEMS];
    int count, selected, row_h;
    nzlist_cb cb;
    void *ctx;
};

lv_obj_t *nzlist_obj(NzList *l) { return l->box; }
int nzlist_count(NzList *l) { return l->count; }
int nzlist_selected(NzList *l) { return l->count ? l->selected : -1; }
void nzlist_on_activate(NzList *l, nzlist_cb cb, void *ctx) { l->cb = cb; l->ctx = ctx; }

NzList *nzlist_create(lv_obj_t *parent, int row_h)
{
    NzList *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->row_h = row_h;
    l->box = th_box(parent, -1);
    lv_obj_add_flag(l->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(l->box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(l->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(l->box, 4, 0);
    lv_obj_set_style_pad_row(l->box, 2, 0);
    return l;
}

void nzlist_clear(NzList *l)
{
    lv_obj_clean(l->box);
    l->count = 0;
    l->selected = 0;
}

void nzlist_select(NzList *l, int i)
{
    if (!l->count) return;
    if (i < 0) i = 0;
    if (i >= l->count) i = l->count - 1;
    lv_obj_clear_state(l->rows[l->selected], LV_STATE_CHECKED);
    l->selected = i;
    lv_obj_add_state(l->rows[i], LV_STATE_CHECKED);
    lv_obj_scroll_to_view(l->rows[i], LV_ANIM_OFF);
}

static void row_clicked(lv_event_t *e)
{
    NzList *l = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_target(e);
    for (int i = 0; i < l->count; i++)
        if (l->rows[i] == row) {
            nzlist_select(l, i);
            if (l->cb) l->cb(i, l->ctx);
            return;
        }
}

/* child 0 = icon (or nothing), then the text column, then the right text */
static lv_obj_t *row_part(NzList *l, int i, int part)
{
    if (i < 0 || i >= l->count) return NULL;
    return lv_obj_get_child(l->rows[i], part);
}

int nzlist_add(NzList *l, int icon, const char *title, const char *sub, const char *right)
{
    if (l->count >= MAX_ITEMS) return -1;
    lv_obj_t *r = th_box(l->box, TS_TEXT);
    th_add_to(r, TS_ROW_SEL, LV_STATE_CHECKED);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, l->row_h ? l->row_h : (sub ? 46 : 34));
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_style_pad_column(r, 10, 0);

    lv_obj_t *ic = lv_img_create(r);                       /* part 0: icon */
    th_add(ic, TS_ICON);
    if (icon >= 0) lv_img_set_src(ic, icon_get((NzIcon)icon, 20));
    else lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *col = th_box(r, -1);                         /* part 1: title + line under it */
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    /* each line stays one line (width = its text); the column clips what doesn't fit */
    lv_obj_t *t = lv_label_create(col);
    artext_set(t, title ? title : "", 0);                /* (Arabic: joined, right to left) */
    lv_obj_t *s = th_label(col, sub ? sub : "", TS_SMALL);
    if (!sub) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);

    th_label(r, right ? right : "", TS_SMALL);             /* part 2: right text */
    lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, l);
    l->rows[l->count] = r;
    if (l->count == 0) lv_obj_add_state(r, LV_STATE_CHECKED);
    return l->count++;
}

void nzlist_set_right(NzList *l, int i, const char *right)
{
    lv_obj_t *o = row_part(l, i, 2);
    if (o) lv_label_set_text(o, right);
}

void nzlist_set_title(NzList *l, int i, const char *title)
{
    lv_obj_t *col = row_part(l, i, 1);
    if (col) artext_set(lv_obj_get_child(col, 0), title, 0);
}

lv_obj_t *nzlist_title_obj(NzList *l, int i)
{
    lv_obj_t *col = row_part(l, i, 1);
    return col ? lv_obj_get_child(col, 0) : NULL;
}

lv_obj_t *nzlist_right_obj(NzList *l, int i) { return row_part(l, i, 2); }

lv_obj_t *nzlist_sub_obj(NzList *l, int i)
{
    lv_obj_t *col = row_part(l, i, 1);
    return col ? lv_obj_get_child(col, 1) : NULL;
}

void nzlist_set_sub(NzList *l, int i, const char *sub)
{
    lv_obj_t *col = row_part(l, i, 1);
    if (!col) return;
    lv_obj_t *s = lv_obj_get_child(col, 1);
    lv_label_set_text(s, sub);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_HIDDEN);
}

int nzlist_key(NzList *l, int key)
{
    if (!l->count) return 0;
    switch (key) {
    case NZ_KEY_UP:   nzlist_select(l, l->selected - 1); return 1;
    case NZ_KEY_DOWN: nzlist_select(l, l->selected + 1); return 1;
    case NZ_KEY_L:    nzlist_select(l, l->selected - 8); return 1;
    case NZ_KEY_R:    nzlist_select(l, l->selected + 8); return 1;
    case NZ_KEY_A:    if (l->cb) l->cb(l->selected, l->ctx); return 1;
    default:          return 0;
    }
}

void nzlist_free(NzList *l) { free(l); }

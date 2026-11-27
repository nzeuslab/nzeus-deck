#ifndef NZEUS_NZLIST_H
#define NZEUS_NZLIST_H
#include "lvgl.h"

/* A themed list that every NZEUS app can use (Python examples, Wi-Fi networks,
 * songs, processes...). Each row: an icon, a title, a smaller line under it,
 * and a short text on the right. The selected row is highlighted.
 *   D-pad up/down: move     L1/R1: a page up/down     A: activate
 *   mouse: click a row to activate it
 * Titles with Arabic letters are shown joined and right to left (artext.h). */
typedef struct NzList NzList;
typedef void (*nzlist_cb)(int index, void *ctx);

NzList   *nzlist_create(lv_obj_t *parent, int row_h);    /* row_h: 0 = automatic */
lv_obj_t *nzlist_obj(NzList *l);                         /* to size/place it */
void nzlist_clear(NzList *l);
int  nzlist_add(NzList *l, int icon, const char *title, const char *sub, const char *right);  /* icon -1 = none */
void nzlist_set_right(NzList *l, int i, const char *right);
void nzlist_set_title(NzList *l, int i, const char *title);
void nzlist_set_sub(NzList *l, int i, const char *sub);
lv_obj_t *nzlist_title_obj(NzList *l, int i);           /* row i's title label, NULL = no row */
lv_obj_t *nzlist_sub_obj(NzList *l, int i);             /* row i's line under the title, NULL = no row */
lv_obj_t *nzlist_right_obj(NzList *l, int i);           /* row i's text on the right, NULL = no row */
void nzlist_on_activate(NzList *l, nzlist_cb cb, void *ctx);
int  nzlist_count(NzList *l);
int  nzlist_selected(NzList *l);
void nzlist_select(NzList *l, int i);
int  nzlist_key(NzList *l, int key);                     /* 1 = used the button */
void nzlist_free(NzList *l);                             /* (the objects go with their parent) */

/* nzlist_add with sub as a button hint template ({accept}/{back}, see nzui.h);
 * title and right stay literal */
int nzlist_add_hint(NzList *l, int icon, const char *title, const char *sub, const char *right);

#endif

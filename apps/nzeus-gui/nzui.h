#ifndef NZEUS_NZUI_H
#define NZEUS_NZUI_H
#include "lvgl.h"
#include "usbkbd.h"

/* Pop-ups that any NZEUS app can use. One is open at a time; while it is open
 * it gets all buttons and keyboard keys. They work with the D-pad, a mouse
 * and a USB keyboard. Each one calls back once, after it has closed. */

/* A list of choices.  D-pad up/down, A choose, B cancel (choice -1). */
typedef void (*nz_menu_cb)(int choice, void *ctx);
void nz_menu(const char *title, const char *const items[], int count, nz_menu_cb cb, void *ctx);

/* Ask for a line of text (with the on-screen keyboard).
 * START or the keyboard's OK = done; B = cancel (text NULL). */
typedef void (*nz_prompt_cb)(const char *text, void *ctx);
void nz_prompt(const char *title, const char *initial, nz_prompt_cb cb, void *ctx);

/* A yes/no question.  A = yes, B = no. */
typedef void (*nz_confirm_cb)(int yes, void *ctx);
void nz_confirm(const char *question, nz_confirm_cb cb, void *ctx);

/* A row of button hints, e.g. [A] Open  [B] Back. pairs = { "A", "Open", "B", "Back" },
 * count = number of pairs. Returns the row (place it with lv_obj_align). */
lv_obj_t *nz_hints(lv_obj_t *parent, const char *const pairs[], int count);

/* Button-only labels retain their logical A/B names, and redraw live when
 * the Controls setting changes. Do not use this for user-provided text. */
void nz_button_label(lv_obj_t *label, const char *logical_text);
/* Explicit templates for inline help: {accept} and {back}. Setting the label's
 * text any other way later (lv_label_set_text) unbinds it at the next refresh. */
void nz_button_text(lv_obj_t *label, const char *text);
void nz_button_text_fmt(lv_obj_t *label, const char *fmt, ...);
lv_obj_t *nz_text_label(lv_obj_t *parent, const char *text, int style);
void nz_hints_refresh(void);

/* The clipboard shared by all apps (Terminal, Editor): plain text, up to
 * NZ_CLIP_MAX bytes (longer text is cut). nz_clip_get() is "" when empty. */
#define NZ_CLIP_MAX (16 * 1024)
void nz_clip_set(const char *text);
const char *nz_clip_get(void);

int  nzui_active(void);                 /* a pop-up is open */
int  nzui_key(int key);                 /* 1 = the pop-up used the button */
void nzui_kbd(const KbEvent *ev);
int  nzui_selected(void);               /* highlighted menu row (for tests) */

#endif

#ifndef NZEUS_UI_H
#define NZEUS_UI_H
#include <stdint.h>
#include "home.h"
#include "usbkbd.h"

/* NZEUS has two looks, and switches between them by itself:
 *   handheld mode - big tiles, D-pad and buttons      (home.c)
 *   desktop mode  - windows, taskbar, mouse pointer    (desktop.c)
 * Moving or clicking a USB mouse switches to desktop mode; SELECT toggles
 * (when you let go of it). Hold SELECT and press L1: screenshot (core.c).
 * In desktop mode without a mouse, the D-pad moves the pointer, A clicks and
 * X is the right button (on the empty desktop: the desktop menu). A USB
 * keyboard's window shortcuts (Windows key, Alt+Tab...) work in desktop mode
 * only (desktop.c). */
enum { UI_HANDHELD, UI_DESKTOP };

void ui_init(void);                      /* builds both screens, starts in handheld mode */
int  ui_mode(void);
void ui_set_mode(int mode);

void ui_key(int key);                    /* physical NZ_KEY_* press; mapped to accept/back here */
void ui_buttons(uint32_t held);          /* every frame: buttons held now (bit per NZ_KEY_*) */

/* every frame while a USB mouse is plugged in: its position, whether the left
 * button is held, whether it was clicked, and whether anything happened */
void ui_mouse(int x, int y, int left_down, int left_click, int activity);
/* the remote-control channel (shot.c): pointer to x,y (screen pixels), then as ui_mouse */
void ui_mouse_at(int x, int y, int left_down, int left_click);
/* the right button went down (on the empty desktop: the desktop menu), and
 * the wheel turned (+ = up): call them after ui_mouse in the same frame */
void ui_mouse_right_click(void);
void ui_mouse_wheel(int steps);

void ui_set_status(const char *clock, int battery_percent, int wifi_on);
void ui_set_charging(int on);              /* 1 = charger plugged in (a sign next to the battery) */
void ui_set_open_fn(home_open_fn fn);

/* Things outside NZEUS, done by the console glue (core.c) through the
 * same open function, with these numbers. 1 = done. */
enum { OUTSIDE_RESTART = 1001 };       /* quit NZEUS: the frontend starts it again */
int  ui_run_outside(int what);
int  ui_open_app(int app);               /* both modes use this; 1 = started */
void ui_toast(const char *message);      /* short message, shown in both modes */
void ui_set_badge(const char *text);
/* a small sign on the status bars ("CLAUDE"), NULL = none: ui_set_badge above */
void ui_screenshot(void);                /* the screen -> nzeus/shots/ (in 0.3 s: menus close first) */
void ui_screenshot_now(void);            /* the screen right now (SELECT + L1, see core.c) */
int  ui_flash_visible(void);             /* the screenshot flash is showing (for tests) */
void ui_pointer_pos(int *x, int *y);     /* where the pointer is (for tests) */
const char *ui_toast_text(void);         /* the last message (for tests) */

void ui_kbd(const KbEvent *ev);          /* a USB keyboard key: goes to the app in front */
void ui_tick(void);                      /* every frame: lets open apps do their work */
void ui_show_handheld_home(void);        /* back to the home screen (handheld mode) */

#endif

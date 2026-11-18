#ifndef NZEUS_DESKTOP_H
#define NZEUS_DESKTOP_H
#include <stddef.h>
#include "lvgl.h"
#include "usbkbd.h"

/* Windows that NZEUS Home can open itself (more apps become windows later). */
enum { WIN_SYSTEM, WIN_ABOUT, WIN_SHORTCUTS, WIN_KIND_COUNT };

#define DESKTOP_MAX_WINDOWS 6                   /* open windows at once (each one costs RAM) */

lv_obj_t *desktop_create(void);                 /* builds the desktop screen, returns it */
void desktop_set_status(const char *clock, int battery_percent, int wifi_on);
void desktop_set_charging(int on);
void desktop_set_badge(const char *text);       /* NULL hides it */
void desktop_toggle_start_menu(void);
void desktop_close_menus(void);                 /* the start menu and the desktop menu */
void desktop_open_window(int kind);

/* A USB keyboard key in desktop mode (ui.c asks before the app in front gets
 * it): the shortcuts (Windows key, Alt+Tab, Alt+F4...) and an open menu's
 * arrows / Enter / Esc. 1 = the desktop used the key. */
int  desktop_kbd(const KbEvent *ev);
/* A right-click (or the X button) at x, y: on the empty desktop it opens the
 * desktop menu. 1 = a menu opened. */
int  desktop_right_click(int x, int y);
/* The next wallpaper (the desktop menu's "Wallpaper style"), saved: Hills (our
 * own drawn scene), Picture (NZ_DIR/wallpaper.jpg, .png or .bmp), Glow, Sky, Plain. */
void desktop_cycle_wallpaper(void);
/* The owner's own pictures (see CUSTOM-ICONS.md): the start button's logo
 * nzeus/start-icon.bmp and app icons nzeus/icons/NAME.bmp (or .png; pure
 * green = see-through). Loaded when the desktop is built and at every theme
 * change; this loads them again now (the old ones are freed). */
void desktop_reload_own_icons(void);

/* windows that host a built-in app (apps.c); the window is the app's host */
struct NzApp;
lv_obj_t *desktop_open_app_window(struct NzApp *a);   /* returns where the app goes, or NULL */
void desktop_raise_app_window(struct NzApp *a);
void desktop_close_app_window(struct NzApp *a);
void desktop_set_app_title(struct NzApp *a, const char *title);
void desktop_focus_at(int x, int y);                  /* a click there brings its window forward */

/* where the System window gets its text (tests and the PC preview swap it) */
extern void (*desktop_sysinfo_fn)(char *buf, size_t size);
extern void (*desktop_screenshot_fn)(void);         /* the start menu's Screenshot item */

/* for tests */
enum { TRAY_WIFI, TRAY_BT, TRAY_VOLUME, TRAY_BATTERY, TRAY_CLOCK, TRAY_COUNT };
enum { WS_NORMAL, WS_MAX, WS_LEFT, WS_RIGHT };       /* a window's size: its own, full, left / right half */
int desktop_start_menu_is_open(void);
int desktop_desk_menu_is_open(void);                 /* the right-click menu */
int desktop_window_count(void);
lv_obj_t *desktop_start_menu_obj(void);
lv_obj_t *desktop_front_window(void);                /* the window in front, NULL = none */
int  desktop_window_state(lv_obj_t *win);            /* WS_*, -1 = not a window */
int  desktop_window_minimized(lv_obj_t *win);
lv_obj_t *desktop_task_button(lv_obj_t *win);        /* its button on the taskbar */
lv_obj_t *desktop_tray_item(int which);              /* TRAY_* */
const char *desktop_menu_selected_text(void);
const char *desktop_wallpaper_shown(void);           /* "hills", "picture", "glow", "sky", "plain" */
int desktop_wallpaper_has_image(void);               /* Hills / Picture: an image buffer is in use */
int desktop_own_icon_count(void);                    /* the owner's icons loaded now (start logo, desktop, menus) */
lv_obj_t *desktop_start_icon(void);                  /* the start button's logo (hidden = none) */
lv_obj_t *desktop_icon_part(int i, int part);        /* desktop icon i: 0 built-in picture, 1 own picture, 2 tile */
int desktop_menu_own_icon_w(int i);                  /* start menu: width of desktop app i's own icon, 0 = built-in */

#endif

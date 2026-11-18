#ifndef NZEUS_APPS_H
#define NZEUS_APPS_H
#include "lvgl.h"
#include "usbkbd.h"

/* A built-in NZEUS app (Terminal, Files, ...).
 *
 * Every app builds its screen inside one container, `root`. In handheld mode
 * the root fills the screen under a title bar; in desktop mode it sits inside
 * a window. So an app is written once and works in both modes. */
typedef struct NzApp NzApp;
struct NzApp {
    int id;                                       /* APP_TERMINAL, ... (home.h), or EXTRA_* */
    const char *title;
    int icon;                                     /* ICON_* (icons.h) */
    void (*create)(NzApp *a, lv_obj_t *root);     /* build the app inside root */
    int  (*key)(NzApp *a, int key);               /* a button (NZ_KEY_*); 1 = used it */
    void (*kbd)(NzApp *a, const KbEvent *ev);     /* a USB keyboard key */
    void (*tick)(NzApp *a);                       /* every frame, even when not in front */
    void (*destroy)(NzApp *a);                    /* free what create() made (root is deleted for you) */
    /* filled in by apps.c */
    lv_obj_t *root;                               /* NULL = not open */
    lv_obj_t *host;                               /* the handheld view screen, or the window */
    int in_window;
};

void   apps_register(NzApp *a);
NzApp *apps_get(int id);
int    apps_open(int id);              /* 1 = it's a built-in app and it's now open and in front */
void   apps_close(NzApp *a);
NzApp *apps_focused(void);             /* the app in front (handheld view or focused window) */
void   apps_set_focused(NzApp *a);
void   apps_set_title(NzApp *a, const char *title);   /* change the title bar / window title */

/* routing, called by ui.c */
int    apps_key(int key);              /* 1 = the app in front used the button */
void   apps_kbd(const KbEvent *ev);
void   apps_tick(void);
void   apps_mode_changed(void);        /* handheld <-> desktop */
void   apps_init(void);                /* registers all built-in apps */

/* the on-screen keyboard's D-pad driver: apps send LVGL keys here */
void   apps_keypad_send(uint32_t lv_key);
lv_group_t *apps_keypad_group(void);

#endif

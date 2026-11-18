#ifndef NZEUS_HOME_H
#define NZEUS_HOME_H
#include <stddef.h>

/* Buttons, after the console's raw button bits are mapped. */
enum {
    NZ_KEY_UP, NZ_KEY_DOWN, NZ_KEY_LEFT, NZ_KEY_RIGHT,
    NZ_KEY_A, NZ_KEY_B, NZ_KEY_X, NZ_KEY_Y,
    NZ_KEY_L, NZ_KEY_R, NZ_KEY_START, NZ_KEY_SELECT,
    NZ_KEY_COUNT
};

/* The apps on the home screen, in screen order (left to right, top to bottom). */
enum {
    APP_TERMINAL, APP_FILES, APP_EDITOR, APP_PYTHON,
    APP_CAR, APP_WEB, APP_ELECTRONICS, APP_ARDUINO,
    APP_CLAUDE, APP_MEDIA, APP_WIFI, APP_SETTINGS,
    APP_COUNT,
    /* the extra tools (desktop start menu; not on the home grid) */
    APP_TASKMGR = APP_COUNT, APP_CALCULATOR, APP_NOTES, APP_CLOCK, APP_IMAGES, APP_WHATSAPP,
    APP_PROBE, APP_DEVTOOLS, APP_HEALTH, APP_BTSCAN, APP_UPDATE,
    APP_ALL_COUNT
};

/* Called when an app is opened. Return 1 if the app was started, 0 if it
 * isn't available yet (NZEUS then says "coming soon"). */
typedef int (*home_open_fn)(int app);

void home_create(void);                         /* build the home screen on the current screen */
void home_key(int key);                         /* one button press (handheld mode) */
void home_set_status(const char *clock, int battery_percent, int wifi_on);
void home_set_charging(int on);
void home_battery_text(char *out, size_t n, int battery_percent, int is_charging);   /* (also the desktop) */
const char *home_status_text(void);             /* the status bar's text (for tests) */
void home_set_badge(const char *text);           /* NULL hides it */
const char *home_badge_text(void);               /* its text, "" when hidden (tests) */
const char *home_app_name(int app);
int  home_selected(void);
int  home_app_icon(int app);                     /* ICON_* for an app (icons.h) */                        /* the selected tile (for tests) */

#endif

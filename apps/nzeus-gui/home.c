/* home.c - the NZEUS Linux home screen (handheld mode).
 *
 * Layout on the 640x480 screen:
 *   top:    status bar  - "NZEUS", Wi-Fi, battery, clock
 *   middle: 4 x 3 grid of app tiles; the selected one has an accent frame
 *           (Wi-Fi is in Settings; its tile is now "Tools": the extra tools)
 *   bottom: hint bar    - which buttons do what
 * The D-pad moves the selection, A opens the app.
 * Colours and fonts come from the theme (theme.h), icons from icons.c. */
#include <stdio.h>
#include "lvgl.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

#define BAR_H   30
#define HINT_H  28
#define COLS    4
#define ROWS    3
#define GAP     10

static const char *const names[APP_ALL_COUNT] = {
    "Terminal", "Files", "Editor", "Python", "Car", "Web",
    "Electronics", "Arduino", "Claude", "Media", "Wi-Fi", "Settings",
    "Task Manager", "Calculator", "Notes", "Clock", "Images", "WhatsApp",
    "Probe", "Dev toolbox", "Health", "Bluetooth scanner", "Update NZEUS Deck",
};

/* What the 12 tiles open, in screen order. TILE_TOOLS = the Tools list (the
 * same as the Y button). Wi-Fi is not here: it lives in Settings > Wi-Fi. */
#define TILE_TOOLS (-1)
#define TILE_COUNT 12
static const int grid[TILE_COUNT] = {
    APP_TERMINAL, APP_FILES, APP_EDITOR, APP_PYTHON,
    APP_CAR, APP_WEB, APP_ELECTRONICS, APP_ARDUINO,
    APP_CLAUDE, APP_MEDIA, TILE_TOOLS, APP_SETTINGS,
};

static lv_obj_t *tiles[TILE_COUNT];
static lv_obj_t *status_right;     /* Wi-Fi, battery, clock */
static lv_obj_t *badge;            /* e.g. "CLAUDE" while Claude can reach the console */
static int selected;               /* index of the selected tile */

const char *home_app_name(int app) { return (app >= 0 && app < APP_ALL_COUNT) ? names[app] : "?"; }

/* Move the selection frame to tile i. */
static void select_tile(int i)
{
    lv_obj_clear_state(tiles[selected], LV_STATE_CHECKED);
    selected = i;
    lv_obj_add_state(tiles[selected], LV_STATE_CHECKED);
}

int home_selected(void) { return selected; }

/* the tile that opens `app` (-1: no tile) */
static int tile_of(int app)
{
    for (int i = 0; i < TILE_COUNT; i++) if (grid[i] == app) return i;
    return -1;
}

int home_app_icon(int app)
{
    static const int extra[] = { ICON_TASKMGR, ICON_CALCULATOR, ICON_NOTES, ICON_CLOCK, ICON_IMAGE, ICON_CHAT, ICON_GAUGE, ICON_FILE_CODE, ICON_SYSTEM, ICON_KEYBOARD, ICON_POWER };
    if (app >= 0 && app < APP_COUNT) return app;          /* the 12 apps: same order as the icons */
    if (app >= APP_COUNT && app < APP_ALL_COUNT) return extra[app - APP_COUNT];
    return ICON_FILE;
}

static int charging;                /* the charger is plugged in */
void home_set_charging(int on) { charging = on; }

/* "<battery sign> 48%" (+ a lightning sign while charging); "" when unknown.
 * Shared with the desktop's tray. */
void home_battery_text(char *out, size_t n, int battery_percent, int is_charging)
{
    /* battery symbol: full / 3 quarters / half / 1 quarter / empty */
    const char *bat = battery_percent > 80 ? LV_SYMBOL_BATTERY_FULL :
                      battery_percent > 55 ? LV_SYMBOL_BATTERY_3 :
                      battery_percent > 30 ? LV_SYMBOL_BATTERY_2 :
                      battery_percent > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    if (battery_percent < 0) { out[0] = 0; return; }
    snprintf(out, n, "%s%s %d%%", is_charging ? LV_SYMBOL_CHARGE " " : "", bat, battery_percent);
}

void home_set_status(const char *clock, int battery_percent, int wifi_on)
{
    char text[80], batt[32];
    home_battery_text(batt, sizeof batt, battery_percent, charging);
    if (batt[0])
        snprintf(text, sizeof text, "%s   %s   %s", wifi_on ? LV_SYMBOL_WIFI : "", batt, clock);
    else
        snprintf(text, sizeof text, "%s   %s", wifi_on ? LV_SYMBOL_WIFI : "", clock);
    lv_label_set_text(status_right, text);
}
const char *home_status_text(void) { return status_right ? lv_label_get_text(status_right) : ""; }

void home_set_badge(const char *text)
{
    if (!badge) return;
    if (text) { lv_label_set_text(badge, text); lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
}

const char *home_badge_text(void)                    /* the badge's text, "" when hidden (tests) */
{
    return badge && !lv_obj_has_flag(badge, LV_OBJ_FLAG_HIDDEN) ? lv_label_get_text(badge) : "";
}

static void open_tools(void);

/* open what tile i stands for */
static void open_tile(int i)
{
    if (grid[i] == TILE_TOOLS) open_tools();
    else ui_open_app(grid[i]);
}

static void tile_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    select_tile(i);
    open_tile(i);
}

void home_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    th_add(scr, TS_SCREEN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* status bar */
    lv_obj_t *bar = th_box(scr, TS_BAR);
    lv_obj_set_size(bar, 640, BAR_H);
    lv_obj_t *name = th_label(bar, "NZEUS Deck", TS_TITLE);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
    status_right = th_label(bar, "", TS_TEXT);
    lv_obj_align(status_right, LV_ALIGN_RIGHT_MID, -12, 0);
    home_set_status("--:--", -1, 0);
    badge = th_label(bar, "", TS_KEYCAP);
    th_add(badge, TS_BTN_ACCENT);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 80, 0);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);

    /* 4 x 3 grid between the two bars */
    int top = BAR_H + GAP, bottom = 480 - HINT_H - GAP;
    int tw = (640 - GAP * (COLS + 1)) / COLS;
    int th = (bottom - top - GAP * (ROWS - 1)) / ROWS;
    for (int i = 0; i < TILE_COUNT; i++) {
        int col = i % COLS, row = i / COLS;
        lv_obj_t *t = th_box(scr, TS_TILE);
        th_add_to(t, TS_TILE_SEL, LV_STATE_CHECKED);
        lv_obj_set_pos(t, GAP + col * (tw + GAP), top + row * (th + GAP));
        lv_obj_set_size(t, tw, th);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *icon = lv_img_create(t);
        lv_img_set_src(icon, icon_get((NzIcon)(grid[i] == TILE_TOOLS ? ICON_SYSTEM : home_app_icon(grid[i])), 48));
        th_add(icon, TS_ICON);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -14);
        lv_obj_t *nm = th_label(t, grid[i] == TILE_TOOLS ? "Tools" : names[grid[i]], TS_TEXT);
        lv_obj_align(nm, LV_ALIGN_CENTER, 0, 30);
        tiles[i] = t;
    }
    selected = 0;
    lv_obj_add_state(tiles[0], LV_STATE_CHECKED);

    /* hint bar */
    lv_obj_t *hint = th_box(scr, TS_BAR);
    lv_obj_set_size(hint, 640, HINT_H);
    lv_obj_set_pos(hint, 0, 480 - HINT_H);
    static const char *const hints[] = { "D-pad", "Move", "A", "Open", "Y", "Tools", "START", "Settings", "SELECT", "Desktop" };
    lv_obj_t *h = nz_hints(hint, hints, 5);
    lv_obj_center(h);
}

static void tools_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c >= 0 && c < APP_ALL_COUNT - APP_COUNT) ui_open_app(APP_COUNT + c);
    else if (c == APP_ALL_COUNT - APP_COUNT) ui_screenshot();
}

/* the extra tools (the Tools tile and the Y button) */
static void open_tools(void)
{
    static const char *const items[] = { "Task Manager", "Calculator", "Notes", "Clock", "Images", "WhatsApp", "Probe", "Dev toolbox", "Health", "Bluetooth scanner", "Update NZEUS Deck", "Screenshot" };
    nz_menu("Tools", items, 12, tools_chosen, NULL);
}

void home_key(int key)
{
    int col = selected % COLS, row = selected / COLS;
    switch (key) {
    case NZ_KEY_LEFT:  if (col > 0) select_tile(selected - 1); break;
    case NZ_KEY_RIGHT: if (col < COLS - 1) select_tile(selected + 1); break;
    case NZ_KEY_UP:    if (row > 0) select_tile(selected - COLS); break;
    case NZ_KEY_DOWN:  if (row < ROWS - 1) select_tile(selected + COLS); break;
    case NZ_KEY_A: open_tile(selected); break;
    case NZ_KEY_START: select_tile(tile_of(APP_SETTINGS)); ui_open_app(APP_SETTINGS); break;
    case NZ_KEY_Y: open_tools(); break;                /* the extra tools (also the Tools tile) */
    default: break;
    }
}

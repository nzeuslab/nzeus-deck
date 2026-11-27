/* ui.c - switches between handheld mode and desktop mode, and owns the
 * mouse pointer (a real USB mouse, or the D-pad acting as one). */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ui.h"
#include "apps.h"
#include "home.h"
#include "desktop.h"
#include "display.h"
#include "nzui.h"
#include "theme.h"
#include "settings.h"
#include "shot.h"

static int mode = UI_HANDHELD;
static lv_obj_t *home_scr, *desk_scr;
static home_open_fn open_fn;

/* --- the pointer, as LVGL sees it --- */
static lv_indev_t *ptr_indev;
static lv_obj_t *cursor;
static int ptr_x = NZ_W / 2, ptr_y = NZ_H / 2;
static int mouse_down, pad_down;       /* left button held: real mouse / A button */
static int click_reads;                /* keeps a very short click visible to LVGL */
static int last_mx = -1, last_my = -1; /* last real mouse position (for movement) */
static int pad_hold_frames;            /* how long the D-pad has been held (speeds up) */

static void ptr_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = (lv_coord_t)ptr_x;
    data->point.y = (lv_coord_t)ptr_y;
    data->state = (mouse_down || pad_down || click_reads > 0) ? LV_INDEV_STATE_PRESSED
                                                               : LV_INDEV_STATE_RELEASED;
    if (click_reads > 0) click_reads--;
}

/* The arrow pointer (NZEUS's own):
 * '#' black outline, '.' white inside, ' ' see-through. */
static const char *arrow[] = {
    "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#",
    "#.......#", "#........#", "#.........#", "#......#####", "#...#..#",
    "#..##..#", "#.#  #..#", "##   #..#", "#     #..#", "      #..#", "       ##",
};
#define ARROW_W 12
#define ARROW_H 19
static lv_color_t arrow_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(ARROW_W, ARROW_H)];

static lv_obj_t *make_cursor(void)
{
    lv_obj_t *c = lv_canvas_create(lv_layer_sys());
    lv_canvas_set_buffer(c, arrow_buf, ARROW_W, ARROW_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    for (int y = 0; y < ARROW_H; y++)
        for (int x = 0; arrow[y][x]; x++) {
            char p = arrow[y][x];
            if (p == ' ') continue;
            lv_canvas_set_px_color(c, x, y, p == '#' ? lv_color_black() : lv_color_white());
            lv_canvas_set_px_opa(c, x, y, LV_OPA_COVER);
        }
    return c;
}

/* --- toast: a short message near the bottom, above everything --- */
static lv_obj_t *toast;
static lv_timer_t *toast_timer;

static void hide_toast(lv_timer_t *t) { (void)t; lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN); toast_timer = NULL; }

void ui_toast(const char *message)
{
    lv_label_set_text(toast, message);
    /* a long message wraps onto more lines instead of running off the screen */
    lv_obj_set_width(toast, LV_SIZE_CONTENT);
    lv_obj_update_layout(toast);
    if (lv_obj_get_width(toast) > NZ_W - 40) {
        lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(toast, NZ_W - 40);
    }
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -44);
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(hide_toast, 1800, NULL);   /* hide after 1.8 s */
    lv_timer_set_repeat_count(toast_timer, 1);
}

void ui_pointer_pos(int *x, int *y) { *x = ptr_x; *y = ptr_y; }
const char *ui_toast_text(void) { return toast ? lv_label_get_text(toast) : ""; }   /* (for tests) */

/* --- modes --- */
int ui_mode(void) { return mode; }

void ui_show_handheld_home(void) { lv_scr_load(home_scr); }

void ui_set_mode(int m)
{
    if (m == mode) return;
    mode = m;
    desktop_close_menus();
    if (mode == UI_DESKTOP) {
        lv_scr_load(desk_scr);
        lv_obj_clear_flag(cursor, LV_OBJ_FLAG_HIDDEN);
        ui_toast("Desktop mode");
    } else {
        lv_scr_load(home_scr);
        apps_mode_changed();                       /* an app that was open full-screen comes back */
        lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
        pad_down = 0;
        ui_toast("Handheld mode");
    }
}

/* --- apps --- */
void ui_set_open_fn(home_open_fn fn) { open_fn = fn; }
int ui_run_outside(int what) { return open_fn ? open_fn(what) : 0; }

int ui_open_app(int app)
{
    if (apps_open(app)) return 1;                  /* a built-in NZEUS app */
    if (open_fn && open_fn(app)) return 1;         /* an outside program (core.c) */
    char msg[64];
    snprintf(msg, sizeof msg, "%s is coming soon", home_app_name(app));
    ui_toast(msg);
    return 0;
}

/* --- buttons --- */
/* the physical button that confirms (Settings > Controls) */
static int accept_key(void) { return settings_accept_is_b() ? NZ_KEY_B : NZ_KEY_A; }

void ui_key(int key)
{
    /* physical A/B -> logical accept/back; keyboard events already carry
     * actions and do not come through here */
    if (settings_accept_is_b() && (key == NZ_KEY_A || key == NZ_KEY_B))
        key = key == NZ_KEY_A ? NZ_KEY_B : NZ_KEY_A;
    if (nzui_key(key)) return;                        /* an open pop-up gets every button */
    if (key == NZ_KEY_SELECT) {                       /* SELECT: switch look */
        ui_set_mode(mode == UI_DESKTOP ? UI_HANDHELD : UI_DESKTOP);
        return;
    }
    if (mode == UI_HANDHELD) {
        if (apps_key(key)) return;                 /* the open app used it */
        NzApp *a = apps_focused();
        if (key == NZ_KEY_B && a && !a->in_window) { apps_close(a); return; }   /* B: back home */
        if (!a || a->in_window) home_key(key);
        return;
    }
    if (key == NZ_KEY_START) desktop_toggle_start_menu();   /* START: NZEUS menu */
    if (key == NZ_KEY_B) desktop_close_menus();             /* B: close the menu */
    if (key == NZ_KEY_X) desktop_right_click(ptr_x, ptr_y); /* X: like a right-click at the pointer */
}

/* the desktop's window shortcuts (usbkbd.h): in handheld mode they do nothing */
static int desktop_only_key(KbKey k)
{
    return k == KB_SUPER || k == KB_SUPER_UP || k == KB_SUPER_DOWN || k == KB_SUPER_LEFT ||
           k == KB_SUPER_RIGHT || k == KB_CTRL_ESC || k == KB_ALT_TAB || k == KB_ALT_F4 || k == KB_ALT_ENTER;
}

void ui_kbd(const KbEvent *ev)
{
    if (nzui_active()) { nzui_kbd(ev); return; }      /* an open pop-up first */
    if (mode == UI_DESKTOP) {
        if (!desktop_kbd(ev)) apps_kbd(ev);           /* the desktop's shortcuts, then the app in front */
        return;
    }
    if (desktop_only_key(ev->key)) return;
    if (ev->key == KB_CTRL_ALT_T) {                   /* handheld: as before, Ctrl+T for the app */
        KbEvent t = { KB_CTRL_OTHER, 't' };
        apps_kbd(&t);
        return;
    }
    apps_kbd(ev);
}
void ui_tick(void) { apps_tick(); }

/* a new press of the pointer brings the window under it to the front */
static void pointer_edge(void)
{
    static int was_pressed;
    int now = mouse_down || pad_down || click_reads > 0;
    if (now && !was_pressed && mode == UI_DESKTOP) desktop_focus_at(ptr_x, ptr_y);
    was_pressed = now;
}

/* In desktop mode the D-pad moves the pointer (faster the longer it's held)
 * and A works as the left mouse button. */
void ui_buttons(uint32_t held)
{
    /* the D-pad is the pointer only in desktop mode, and not while a pop-up
     * is open (then the D-pad moves through the pop-up instead) */
    if (mode != UI_DESKTOP || nzui_active()) { pad_hold_frames = 0; pad_down = 0; return; }
    int dx = 0, dy = 0;
    if (held & (1u << NZ_KEY_LEFT))  dx--;
    if (held & (1u << NZ_KEY_RIGHT)) dx++;
    if (held & (1u << NZ_KEY_UP))    dy--;
    if (held & (1u << NZ_KEY_DOWN))  dy++;
    if (dx || dy) {
        pad_hold_frames++;
        int speed = pad_hold_frames < 10 ? 2 : pad_hold_frames < 30 ? 5 : 9;   /* pixels per frame */
        ptr_x += dx * speed;
        ptr_y += dy * speed;
        if (ptr_x < 0) ptr_x = 0;
        if (ptr_y < 0) ptr_y = 0;
        if (ptr_x > NZ_W - 1) ptr_x = NZ_W - 1;
        if (ptr_y > NZ_H - 1) ptr_y = NZ_H - 1;
    } else {
        pad_hold_frames = 0;
    }
    pad_down = (held & (1u << accept_key())) != 0;
    pointer_edge();
}

void ui_mouse(int x, int y, int left_down, int left_click, int activity)
{
    if (activity && mode != UI_DESKTOP) ui_set_mode(UI_DESKTOP);     /* mouse used: desktop */
    if (last_mx >= 0) {                     /* follow the mouse's movement, not its */
        ptr_x += x - last_mx;               /* absolute spot, so D-pad moves aren't undone */
        ptr_y += y - last_my;
        if (ptr_x < 0) ptr_x = 0;
        if (ptr_y < 0) ptr_y = 0;
        if (ptr_x > NZ_W - 1) ptr_x = NZ_W - 1;
        if (ptr_y > NZ_H - 1) ptr_y = NZ_H - 1;
    }
    last_mx = x;
    last_my = y;
    mouse_down = left_down;
    if (left_click && !left_down) click_reads = 2;   /* pressed and released between frames */
    pointer_edge();
}

/* The mouse's right button: on the empty desktop it opens the desktop menu. */
void ui_mouse_right_click(void)
{
    if (mode != UI_DESKTOP) ui_set_mode(UI_DESKTOP);   /* (any mouse use: desktop mode) */
    if (nzui_active()) return;                          /* (a pop-up is open: it decides) */
    desktop_right_click(ptr_x, ptr_y);
}

/* The mouse wheel: scrolls whatever can scroll under the pointer (a list, a
 * window's page, a long menu). steps: + = wheel up (towards the top). */
#define WHEEL_STEP_PX 40
void ui_mouse_wheel(int steps)
{
    if (!steps) return;
    if (mode != UI_DESKTOP) ui_set_mode(UI_DESKTOP);
    lv_point_t p = { (lv_coord_t)ptr_x, (lv_coord_t)ptr_y };
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);   /* pop-ups first */
    if (!o) o = lv_indev_search_obj(lv_scr_act(), &p);
    for (; o; o = lv_obj_get_parent(o))                      /* the nearest thing that can scroll */
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
            (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0)) {
            lv_obj_scroll_by_bounded(o, 0, (lv_coord_t)(steps * WHEEL_STEP_PX), LV_ANIM_OFF);
            return;
        }
}

/* The remote-control channel (shot.c: nzctl mouse / click / drag): the pointer
 * jumps to x,y, then the same as a real mouse (desktop mode, pointer shown,
 * button state, the window under it comes to the front). A real mouse keeps
 * working afterwards (it moves the pointer relative to where it is). */
void ui_mouse_at(int x, int y, int left_down, int left_click)
{
    if (mode != UI_DESKTOP) ui_set_mode(UI_DESKTOP);
    ptr_x = x < 0 ? 0 : x > NZ_W - 1 ? NZ_W - 1 : x;
    ptr_y = y < 0 ? 0 : y > NZ_H - 1 ? NZ_H - 1 : y;
    mouse_down = left_down;
    if (left_click && !left_down) click_reads = 2;
    pointer_edge();
}

/* --- screenshots: save, a short white flash, and a message --- */
static lv_obj_t *flash;

static void flash_end(lv_timer_t *t) { (void)t; if (flash) lv_obj_del(flash); flash = NULL; }

static void shot_now(void)
{
    char name[64], msg[96];
    if (shot_take(name, sizeof name) != 0) { ui_toast("Could not save the screenshot"); return; }
    if (!flash) {                                    /* (the picture is saved first: no flash in it) */
        flash = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(flash);
        lv_obj_set_size(flash, NZ_W, NZ_H);
        lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(flash, LV_OPA_70, 0);
        lv_obj_clear_flag(flash, LV_OBJ_FLAG_CLICKABLE);
        lv_timer_t *t = lv_timer_create(flash_end, 150, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
    snprintf(msg, sizeof msg, "Screenshot saved: shots/%s", name);
    ui_toast(msg);
}

int ui_flash_visible(void) { return flash != NULL; }

/* SELECT + L1 (core.c): the screen as it is right now */
void ui_screenshot_now(void) { shot_now(); }

/* the Screenshot menu item: a moment later, so the menu has gone from the picture */
static void shot_later(lv_timer_t *t) { (void)t; shot_now(); }
void ui_screenshot(void)
{
    lv_timer_t *t = lv_timer_create(shot_later, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void ui_set_badge(const char *text)
{
    home_set_badge(text);
    desktop_set_badge(text);
}

void ui_set_status(const char *clock, int battery_percent, int wifi_on)
{
    home_set_status(clock, battery_percent, wifi_on);
    desktop_set_status(clock, battery_percent, wifi_on);
}

/* the charger is plugged in: a lightning sign next to the battery */
void ui_set_charging(int on)
{
    home_set_charging(on);
    desktop_set_charging(on);
}

void ui_init(void)
{
    theme_init();                                /* colours and fonts first: every screen uses them */
    desktop_screenshot_fn = ui_screenshot;
    home_scr = lv_scr_act();
    home_create();
    desk_scr = desktop_create();
    apps_init();                                 /* the built-in apps + the D-pad keyboard driver */

    static lv_indev_drv_t ptr_drv;              /* the pointer for desktop mode */
    lv_indev_drv_init(&ptr_drv);
    ptr_drv.type = LV_INDEV_TYPE_POINTER;
    ptr_drv.read_cb = ptr_read;
    ptr_indev = lv_indev_drv_register(&ptr_drv);
    cursor = make_cursor();
    lv_indev_set_cursor(ptr_indev, cursor);
    lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);

    toast = th_label(lv_layer_top(), "", TS_TEXT);
    th_add(toast, TS_PANEL);
    lv_obj_set_style_pad_hor(toast, 16, 0);
    lv_obj_set_style_pad_ver(toast, 8, 0);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    mode = UI_HANDHELD;

    /* what opens first (Settings > Startup) */
    const char *start = settings_get("start", "home");
    if (!strcmp(start, "desktop")) ui_set_mode(UI_DESKTOP);
    else if (!strcmp(start, "terminal")) apps_open(APP_TERMINAL);
    else if (!strcmp(start, "files")) apps_open(APP_FILES);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);    /* (no "Desktop mode" message at start) */
}

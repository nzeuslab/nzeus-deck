/* desktop.c - the NZEUS Linux desktop (desktop mode).
 *
 * Layout on the 640x480 screen:
 *   left:   desktop icons in two columns (click to open)
 *   middle: windows - drag them by the title bar; buttons: minimize, maximize, close
 *   bottom: taskbar - NZEUS start button, one button per open window, and the
 *           "tray": Wi-Fi, Bluetooth, volume, battery, clock (click them)
 * The start button (or START, or the Windows key) opens the NZEUS menu: apps,
 * tools and system in three columns. Right-click on the empty desktop (or X
 * on the controller) opens a small desktop menu.
 *
 * Windows, the way a PC desktop does it:
 *   - double-click the title bar (or Alt+Enter) = maximize / restore
 *   - drag the title bar to the left / right edge = that half of the screen,
 *     to the top edge = maximized; pulling a maximized window gives it its
 *     own size back
 *   - minimize hides it; its taskbar button brings it back
 *   - keyboard: see the "Keyboard shortcuts" window (shortcut_rows below)
 * Windows hold either a small info page (System, About, Keyboard shortcuts)
 * or a whole app (Terminal, ...: see apps.c). Colours and fonts come from the
 * theme. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "apps.h"
#include "bt.h"
#include "desktop.h"
#include "home.h"
#include "hw.h"
#include "icons.h"
#include "ownicons.h"
#include "settings.h"
#include "sysinfo.h"
#include "theme.h"
#include "ui.h"
#include "wallpaper.h"
#include "wifi.h"
#include <unistd.h>

#define SCR_W       640
#define SCR_H       480
#define TASKBAR_H   34
#define WORK_H      (SCR_H - TASKBAR_H)     /* the space above the taskbar (446) */
#define TITLE_H     30                      /* a window's title bar */
#define MAX_WINDOWS DESKTOP_MAX_WINDOWS     /* 6: every window (and its app) costs RAM */
#define DOUBLE_CLICK_MS 450                 /* two clicks on a title bar within this = maximize */
#define DRAG_START_PX   4                   /* the pointer must move this far before a title bar drag starts */
#define SNAP_EDGE       3                   /* let go this close to the screen's edge = snap */
#define BT_LOOK_EVERY   20                  /* seconds between Bluetooth checks for the tray */

void (*desktop_sysinfo_fn)(char *buf, size_t size) = sysinfo_text;
void (*desktop_screenshot_fn)(void);           /* set by ui.c */

static lv_obj_t *scr, *taskbar, *start_btn, *task_buttons, *tray_box, *badge;
static lv_obj_t *tray[TRAY_COUNT];
static lv_obj_t *watermark[2];                  /* the quiet "NZEUS" / "DECK" (plain wallpapers only) */
typedef struct {                                /* one desktop icon: */
    lv_obj_t *box;                              /*   the clickable square */
    lv_obj_t *tile;                             /*   Retro Blue: a soft rounded tile behind the picture */
    lv_obj_t *glyph;                            /*   the built-in icon */
    lv_obj_t *own;                              /*   the owner's own icon (nzeus/icons/NAME.bmp), hidden if none */
    lv_obj_t *name;
} DeskIconObj;
static DeskIconObj dicons[8];
static OwnIcon own_desk[8];                     /* the owner's desktop icons (px NULL = none) */
static OwnIcon own_start;                       /* the owner's start button logo (nzeus/start-icon.bmp) */
static lv_obj_t *start_icon;                    /* ... shown on the start button */
static void start_layout(void);
static int desk_icon_icon(int i);               /* desktop icon i: its built-in icon ... */
static int desk_icon_what(int i);               /* ... and what it opens */
static lv_obj_t *start_label, *start_bold;      /* "NZEUS"; Retro Blue: "start" (+ a copy 1 px right = bold) */

typedef struct {
    lv_obj_t *win;          /* the window (NULL = slot free) */
    lv_obj_t *task_btn;     /* its button on the taskbar */
    lv_obj_t *max_btn;      /* its maximize / restore button */
    lv_obj_t *body;         /* the text inside (System, About) */
    lv_timer_t *timer;      /* live refresh (System window) */
    int kind;               /* WIN_SYSTEM / WIN_ABOUT / WIN_SHORTCUTS, or -1 for an app window */
    NzApp *app;             /* the app inside (app windows) */
    int state;              /* WS_NORMAL / WS_MAX / WS_LEFT / WS_RIGHT (desktop.h) */
    int minimized;          /* hidden; only its taskbar button shows */
    lv_coord_t nx, ny, nw, nh;          /* its own place and size (back to it after maximize / snap) */
    lv_point_t press_pt;                /* title bar drag: where the pointer went down ... */
    lv_coord_t press_x, press_y;        /* ... and where the window was then */
    int dragged;                        /* the title bar was dragged (not just clicked) */
    uint32_t last_click;                /* when the title bar was last clicked (double-click) */
} Window;
static Window windows[MAX_WINDOWS];
static Window *front;                   /* the window in front (gets the keyboard), NULL = none */
static void look_window(Window *w);     /* (Retro Blue or the normal look: see "the Retro Blue look") */

static void do_action(int what);
static void close_window(Window *w);
static void close_menus_outside(int x, int y);

int desktop_window_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].win) n++;
    return n;
}

static lv_obj_t *icon_img(lv_obj_t *parent, int icon, int size, int style)
{
    lv_obj_t *im = lv_img_create(parent);
    lv_img_set_src(im, icon_get((NzIcon)icon, size));
    th_add(im, style);
    return im;
}

/* is the screen point x, y on this (visible) object? */
static int inside(lv_obj_t *o, int x, int y)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

/* change a label's text only when it is different (each change redraws it) */
static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

/* ================= the taskbar ================= */

/* Share the taskbar's middle between the window buttons: each one gets the
 * same width (at most 150); when they get narrow only the icons show. */
static void layout_taskbar(void)
{
    lv_obj_update_layout(tray_box);
    int area = SCR_W - 104 - lv_obj_get_width(tray_box) - 14;
    lv_obj_set_width(task_buttons, area);
    int n = desktop_window_count();
    int each = n ? (area - (n - 1) * 6) / n : area;
    if (each > 150) each = 150;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &windows[i];
        if (!w->win) continue;
        lv_obj_set_width(w->task_btn, each);
        lv_obj_t *name = lv_obj_get_child(w->task_btn, 1);
        if (each < 84) lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);       /* too narrow: icon only */
        else {
            lv_obj_clear_flag(name, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(name, each - 16 - 20 - 6);                 /* (padding, icon, gap) */
        }
    }
}

/* the front window's taskbar button is highlighted; minimized ones are dimmed */
static void show_focus(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &windows[i];
        if (!w->win) continue;
        if (w == front && !w->minimized) lv_obj_add_state(w->task_btn, LV_STATE_CHECKED);
        else lv_obj_clear_state(w->task_btn, LV_STATE_CHECKED);
        /* (text_opa / img_opa / bg_opa: the whole-object "opa" style draws nothing on this screen) */
        lv_opa_t o = w->minimized ? LV_OPA_50 : LV_OPA_COVER;
        lv_obj_set_style_text_opa(w->task_btn, o, 0);                       /* its name (inherited) */
        lv_obj_set_style_img_opa(lv_obj_get_child(w->task_btn, 0), o, 0);   /* its icon */
        lv_obj_set_style_bg_opa(w->task_btn, w->minimized ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
}

/* ================= windows ================= */
static Window *find_app_window(NzApp *a)
{
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].win && windows[i].app == a) return &windows[i];
    return NULL;
}

static Window *window_of(lv_obj_t *win)
{
    for (int i = 0; i < MAX_WINDOWS; i++) if (win && windows[i].win == win) return &windows[i];
    return NULL;
}

/* the highest window on the screen that is not minimized (NULL = none) */
static Window *top_window(void)
{
    for (int c = (int)lv_obj_get_child_cnt(scr) - 1; c >= 0; c--) {
        Window *w = window_of(lv_obj_get_child(scr, c));
        if (w && !w->minimized) return w;
    }
    return NULL;
}

static void focus_window(Window *w)
{
    if (!w || !w->win) return;
    if (w->minimized) { w->minimized = 0; lv_obj_clear_flag(w->win, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_move_foreground(w->win);
    front = w;
    apps_set_focused(w->app);           /* the keyboard goes to the app inside (info windows: none) */
    show_focus();
}

/* the window that is now on top gets the focus (after a close or minimize) */
static void focus_top(void)
{
    Window *t = top_window();
    if (t) { focus_window(t); return; }
    front = NULL;
    apps_set_focused(NULL);
    show_focus();
}

static void free_window(Window *w)
{
    int was_front = front == w;
    if (w->timer) lv_timer_del(w->timer);
    if (w->task_btn) lv_obj_del(w->task_btn);
    if (w->win) lv_obj_del_async(w->win);   /* async: we may be inside one of its own events */
    memset(w, 0, sizeof *w);
    if (was_front) { front = NULL; focus_top(); }
    else show_focus();
    layout_taskbar();
}

static void close_window(Window *w)
{
    if (!w || !w->win) return;
    if (w->app) apps_close(w->app);          /* apps.c then calls desktop_close_app_window */
    else free_window(w);
}

static void minimize(Window *w)
{
    if (!w || !w->win || w->minimized) return;
    w->minimized = 1;
    lv_obj_add_flag(w->win, LV_OBJ_FLAG_HIDDEN);
    if (front == w) focus_top();             /* the next window comes to the front */
    else show_focus();
}

/* the maximize button: one box = "make it big", two boxes = "back to its own size" */
static void box_icon(lv_obj_t *parent, int x, int y, int filled)
{
    const NzTheme *t = theme();
    lv_obj_t *b = th_box(parent, -1);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, t->retro ? 10 : 11, t->retro ? 8 : 9);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, t->retro ? lv_color_white() : th_color(t->text), 0);
    /* (the button's colour: hides the box behind) */
    lv_obj_set_style_bg_color(b, t->retro ? lv_color_hex(0x2C64DC) : th_color(t->surface_hi), 0);
    lv_obj_set_style_bg_opa(b, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
}

static void draw_max_icon(Window *w)
{
    lv_obj_clean(w->max_btn);
    if (w->state == WS_NORMAL) box_icon(w->max_btn, 0, 0, 0);
    else { box_icon(w->max_btn, 2, -2, 0); box_icon(w->max_btn, -1, 1, 1); }
}

/* WS_NORMAL: its own place and size; WS_MAX: the whole space above the
 * taskbar; WS_LEFT / WS_RIGHT: that half of it */
static void set_state(Window *w, int st)
{
    if (st == w->state && st == WS_NORMAL) return;     /* (already its own size: nothing to do) */
    if (w->state == WS_NORMAL && st != WS_NORMAL) {     /* remember where it was */
        lv_obj_update_layout(w->win);
        w->nx = lv_obj_get_x(w->win);
        w->ny = lv_obj_get_y(w->win);
    }
    w->state = st;
    lv_coord_t x = 0, y = 0, wd = SCR_W, ht = WORK_H;
    if (st == WS_NORMAL) { x = w->nx; y = w->ny; wd = w->nw; ht = w->nh; }
    else if (st == WS_LEFT) wd = SCR_W / 2;
    else if (st == WS_RIGHT) { x = SCR_W / 2; wd = SCR_W / 2; }
    lv_obj_set_pos(w->win, x, y);
    lv_obj_set_size(w->win, wd, ht);
    /* square corners when it fills the screen or a half of it */
    if (st == WS_NORMAL) lv_obj_remove_local_style_prop(w->win, LV_STYLE_RADIUS, 0);
    else lv_obj_set_style_radius(w->win, 0, 0);
    draw_max_icon(w);
}

static void toggle_max(Window *w)
{
    if (!w || !w->win) return;
    set_state(w, w->state == WS_MAX ? WS_NORMAL : WS_MAX);
    focus_window(w);
}

/* --- window events --- */
static void raise_window(lv_event_t *e) { focus_window((Window *)lv_event_get_user_data(e)); }
static void close_clicked(lv_event_t *e) { close_window((Window *)lv_event_get_user_data(e)); }
static void min_clicked(lv_event_t *e) { minimize((Window *)lv_event_get_user_data(e)); }
static void max_clicked(lv_event_t *e) { toggle_max((Window *)lv_event_get_user_data(e)); }

/* the taskbar button: brings its window to the front, or minimizes it when
 * it already is in front (like a PC) */
static void task_clicked(lv_event_t *e)
{
    Window *w = (Window *)lv_event_get_user_data(e);
    if (!w->win) return;
    if (w == front && !w->minimized) minimize(w);
    else focus_window(w);
}

/* the title bar: press = to the front; then drag, click or double-click */
static void title_pressed(lv_event_t *e)
{
    Window *w = (Window *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !w->win) return;
    focus_window(w);
    lv_indev_get_point(indev, &w->press_pt);
    lv_obj_update_layout(w->win);
    w->press_x = lv_obj_get_x(w->win);
    w->press_y = lv_obj_get_y(w->win);
    w->dragged = 0;
}

static void title_pressing(lv_event_t *e)
{
    Window *w = (Window *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !w->win) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - w->press_pt.x, dy = p.y - w->press_pt.y;
    if (!w->dragged) {
        if (abs(dx) + abs(dy) <= DRAG_START_PX) return;    /* a little shake while clicking is no drag */
        if (w->state != WS_NORMAL) {
            /* pulling a big window: its own size comes back, under the pointer */
            set_state(w, WS_NORMAL);
            w->press_pt = p;
            w->press_x = p.x - w->nw / 2;
            w->press_y = p.y - TITLE_H / 2;
            dx = dy = 0;
        }
        w->dragged = 1;
    }
    lv_coord_t x = w->press_x + dx, y = w->press_y + dy;
    lv_coord_t maxx = SCR_W - lv_obj_get_width(w->win), maxy = WORK_H - TITLE_H;
    if (x > maxx) x = maxx;                    /* stays on the screen ... */
    if (y > maxy) y = maxy;                    /* ... at least its title bar */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    lv_obj_set_pos(w->win, x, y);
}

static void title_released(lv_event_t *e)
{
    Window *w = (Window *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !w->win || !w->dragged) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (p.x <= SNAP_EDGE) set_state(w, WS_LEFT);                  /* let go at the left edge */
    else if (p.x >= SCR_W - 1 - SNAP_EDGE) set_state(w, WS_RIGHT);  /* ... at the right edge */
    else if (p.y <= 1) set_state(w, WS_MAX);                       /* ... at the top */
}

static void title_clicked(lv_event_t *e)
{
    Window *w = (Window *)lv_event_get_user_data(e);
    if (w->dragged) { w->last_click = 0; return; }
    uint32_t now = lv_tick_get();
    if (w->last_click && now - w->last_click < DOUBLE_CLICK_MS) { w->last_click = 0; toggle_max(w); }
    else w->last_click = now;
}

/* a small button on the title bar (minimize / maximize) */
static lv_obj_t *title_button(Window *w, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(lv_win_get_header(w->win));
    th_add(b, TS_BTN);
    lv_obj_set_size(b, 30, LV_PCT(100));
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, w);
    return b;
}

/* A new window with an icon, title, minimize / maximize / X buttons and a
 * taskbar button (NULL when all MAX_WINDOWS are open). */
static Window *make_window(const char *title, int icon, int width, int height, int x, int y)
{
    Window *w = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!windows[i].win) { w = &windows[i]; break; }
    if (!w) {
        char msg[80];
        snprintf(msg, sizeof msg, "All %d windows are in use: close one first", MAX_WINDOWS);
        ui_toast(msg);
        return NULL;
    }
    int n = desktop_window_count();
    memset(w, 0, sizeof *w);

    w->win = lv_win_create(scr, TITLE_H);
    th_add(w->win, TS_PANEL);
    lv_obj_set_style_clip_corner(w->win, true, 0);
    lv_obj_set_style_pad_all(w->win, 0, 0);
    lv_obj_set_size(w->win, width, height);
    x += (n % 4) * 24;                                     /* cascade new windows ... */
    y += (n % 4) * 24;
    if (x > SCR_W - width) x = SCR_W - width;              /* ... but keep them on the screen */
    if (y > WORK_H - height) y = WORK_H - height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    lv_obj_set_pos(w->win, x, y);
    w->nx = x; w->ny = y; w->nw = width; w->nh = height;
    lv_obj_add_event_cb(w->win, raise_window, LV_EVENT_PRESSED, w);

    lv_obj_t *header = lv_win_get_header(w->win);          /* children: icon, title, -, box, X */
    th_add(header, TS_BAR);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_pad_column(header, 8, 0);
    icon_img(header, icon, 20, TS_ICON);
    lv_obj_t *t = lv_win_add_title(w->win, title);
    th_add(t, TS_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(header, title_pressed, LV_EVENT_PRESSED, w);
    lv_obj_add_event_cb(header, title_pressing, LV_EVENT_PRESSING, w);
    lv_obj_add_event_cb(header, title_released, LV_EVENT_RELEASED, w);
    lv_obj_add_event_cb(header, title_clicked, LV_EVENT_CLICKED, w);
    lv_obj_set_style_pad_column(header, 0, 0);            /* (the three buttons sit side by side, */
    lv_obj_set_style_pad_hor(t, 8, 0);                    /*  the title keeps a gap on both sides) */
    lv_obj_t *minb = title_button(w, min_clicked);
    lv_obj_t *ml = th_label(minb, LV_SYMBOL_MINUS, TS_TEXT);
    lv_obj_center(ml);
    w->max_btn = title_button(w, max_clicked);
    draw_max_icon(w);
    lv_obj_t *close = lv_win_add_btn(w->win, LV_SYMBOL_CLOSE, 30);
    th_add(close, TS_BTN_DANGER);
    lv_obj_set_style_radius(close, 0, 0);
    lv_obj_add_event_cb(close, close_clicked, LV_EVENT_CLICKED, w);

    lv_obj_t *content = lv_win_get_content(w->win);
    th_add(content, TS_PANEL);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_shadow_width(content, 0, 0);
    lv_obj_add_event_cb(content, raise_window, LV_EVENT_PRESSED, w);

    w->task_btn = lv_btn_create(task_buttons);             /* taskbar button: icon, name */
    th_add_to(w->task_btn, TS_TILE_SEL, LV_STATE_CHECKED);
    lv_obj_set_height(w->task_btn, 26);
    lv_obj_set_style_border_width(w->task_btn, 0, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(w->task_btn, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_side(w->task_btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(w->task_btn, 0, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(w->task_btn, 8, 0);
    lv_obj_set_style_pad_ver(w->task_btn, 0, 0);
    lv_obj_set_flex_flow(w->task_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w->task_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(w->task_btn, 6, 0);
    icon_img(w->task_btn, icon, 20, TS_ICON);
    lv_obj_t *tl = th_label(w->task_btn, title, TS_SMALL);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(w->task_btn, task_clicked, LV_EVENT_CLICKED, w);
    look_window(w);
    front = w;
    show_focus();
    layout_taskbar();
    return w;
}

/* Clicking anywhere inside a window brings it (and its app) to the front; a
 * press outside an open menu closes the menu. Called by ui.c on every new
 * pointer press in desktop mode. */
void desktop_focus_at(int x, int y)
{
    close_menus_outside(x, y);
    lv_point_t p = { (lv_coord_t)x, (lv_coord_t)y };
    lv_obj_t *o = lv_indev_search_obj(scr, &p);
    for (; o; o = lv_obj_get_parent(o)) {
        Window *w = window_of(o);
        if (w) { focus_window(w); return; }
    }
}

/* --- info windows: System, About, Keyboard shortcuts --- */
static const char *window_title(int kind)
{
    return kind == WIN_SYSTEM ? "System" : kind == WIN_SHORTCUTS ? "Keyboard shortcuts" : "About NZEUS Deck";
}

static void fill_window_text(Window *w)
{
    char buf[512];
    if (w->kind == WIN_SYSTEM) desktop_sysinfo_fn(buf, sizeof buf);
    else snprintf(buf, sizeof buf,
                  "NZEUS Deck 0.9\n"
                  "Made by NZEUS LAB\n\n"
                  "Linux 4.4 + LVGL 8.3\n"
                  "Your pocket Linux computer.\n\n"
                  "Your own icons: nzeus/icons/NAME.bmp,\n"
                  "start logo: nzeus/start-icon.bmp\n"
                  "(pure green = see-through)");
    lv_label_set_text(w->body, buf);
}

static void refresh_window(lv_timer_t *t) { fill_window_text((Window *)t->user_data); }

/* The keyboard (and mouse) shortcuts, as shown in their window. */
static const char *const shortcut_rows[][2] = {
    { "Windows key",          "Start menu (open / close)" },
    { "Ctrl + Esc",           "Start menu (open / close)" },
    { "Alt + Tab",            "Next window (+ Shift: back)" },
    { "Alt + F4",             "Close the front window" },
    { "Ctrl + W",             "Close it (not Terminal / Claude)" },
    { "Alt + Enter",          "Maximize / restore" },
    { "Windows + Up",         "Maximize" },
    { "Windows + Down",       "Restore, then minimize" },
    { "Windows + Left/Right", "Fill the left / right half" },
    { "Ctrl + Alt + T",       "Open the Terminal" },
    { "Arrows, Enter, Esc",   "Move in a menu, open, close" },
    { "Right-click desktop",  "Desktop menu" },
    { "Double-click title",   "Maximize / restore" },
    { "Drag title to edge",   "Left / right half, top: full" },
    { "Mouse wheel",          "Scroll a list" },
    { "Controller",           "START menu, X desktop menu" },
};

static void fill_shortcuts(lv_obj_t *content)
{
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 3, 0);
    for (size_t i = 0; i < sizeof shortcut_rows / sizeof shortcut_rows[0]; i++) {
        lv_obj_t *row = th_box(content, -1);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_t *k = th_label(row, shortcut_rows[i][0], TS_TITLE);   /* the keys, in the accent colour */
        lv_obj_set_width(k, 186);
        lv_obj_t *d = th_label(row, shortcut_rows[i][1], TS_TEXT);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);                /* (a long one wraps) */
        lv_obj_set_width(d, 480 - 2 * 14 - 192);
        lv_obj_set_pos(d, 192, 0);
    }
}

void desktop_open_window(int kind)
{
    for (int i = 0; i < MAX_WINDOWS; i++)            /* already open: bring it forward */
        if (windows[i].win && !windows[i].app && windows[i].kind == kind) { focus_window(&windows[i]); return; }
    Window *w = kind == WIN_SHORTCUTS
        ? make_window(window_title(kind), ICON_KEYBOARD, 480, 410, 100, 14)
        : kind == WIN_ABOUT ? make_window(window_title(kind), ICON_ABOUT, 380, 330, 130, 30)
        : make_window(window_title(kind), ICON_SYSTEM, 360, 230, 130, 40);
    if (!w) return;
    w->kind = kind;
    w->app = NULL;
    apps_set_focused(NULL);                           /* (the keyboard is not for an app now) */
    lv_obj_t *content = lv_win_get_content(w->win);
    lv_obj_set_style_pad_all(content, 14, 0);
    if (kind == WIN_SHORTCUTS) { fill_shortcuts(content); return; }
    w->body = th_label(content, "", TS_TEXT);
    lv_obj_set_style_text_line_space(w->body, 6, 0);
    fill_window_text(w);
    w->timer = kind == WIN_SYSTEM ? lv_timer_create(refresh_window, 2000, w) : NULL;  /* live every 2 s */
}

/* --- app windows --- */
lv_obj_t *desktop_open_app_window(NzApp *a)
{
    Window *w = make_window(a->title, a->icon, 480, 370, 92, 12);
    if (!w) return NULL;
    w->kind = -1;
    w->app = a;
    a->host = w->win;
    lv_obj_t *content = lv_win_get_content(w->win);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    apps_set_focused(a);
    return content;
}

void desktop_raise_app_window(NzApp *a) { Window *w = find_app_window(a); if (w) focus_window(w); }

/* the window title (header: icon, title, ...) and its taskbar button (icon, name) */
void desktop_set_app_title(NzApp *a, const char *title)
{
    Window *w = find_app_window(a);
    if (!w) return;
    lv_obj_t *t = lv_obj_get_child(lv_win_get_header(w->win), 1);
    if (t && lv_obj_check_type(t, &lv_label_class)) lv_label_set_text(t, title);
    lv_obj_t *b = lv_obj_get_child(w->task_btn, 1);
    if (b && lv_obj_check_type(b, &lv_label_class)) lv_label_set_text(b, title);
}
void desktop_close_app_window(NzApp *a) { Window *w = find_app_window(a); if (w) free_window(w); }

/* the window in front: next one (Alt+Tab), in the order they were opened */
static void next_window(int dir)
{
    int start = front ? (int)(front - windows) : (dir > 0 ? MAX_WINDOWS - 1 : 0);
    for (int k = 1; k <= MAX_WINDOWS; k++) {
        int i = ((start + dir * k) % MAX_WINDOWS + MAX_WINDOWS) % MAX_WINDOWS;
        if (windows[i].win && &windows[i] != front) { focus_window(&windows[i]); return; }
    }
    if (front && front->minimized) focus_window(front);   /* (only one window: it comes back) */
}

/* ================= menus: the start menu and the right-click desktop menu =================
 * Both are lists of rows (icon + name) in up to three columns. The mouse
 * clicks a row; the keyboard moves a highlight with the arrows, Enter opens
 * it, Esc closes the menu, and a letter jumps to the next row with that letter. */
#define MENU_COLS 3
#define MENU_ROWS 16
typedef struct {
    lv_obj_t *panel;
    lv_obj_t *item[MENU_COLS][MENU_ROWS];
    int what[MENU_COLS][MENU_ROWS];
    int icon[MENU_COLS][MENU_ROWS];               /* the built-in icon of each row */
    OwnIcon own[MENU_COLS][MENU_ROWS];            /* the owner's own icon for it (px NULL = none) */
    int count[MENU_COLS];
    int sel_col, sel_row;                /* the keyboard's highlight (sel_col -1 = none) */
} Menu;
static Menu start, desk;

/* what a menu row does: an APP_* number, or one of these */
#define MENU_SYSTEM     100
#define MENU_ABOUT      101
#define MENU_HANDHELD   102
#define MENU_SCREENSHOT 103
#define MENU_SHORTCUTS  104
#define MENU_THEME      105
#define MENU_WALLPAPER  106

/* the file name of an app's own icon (nzeus/icons/NAME.bmp), NULL = none */
static const char *own_icon_name(int what)
{
    static const char *const names[APP_ALL_COUNT] = {
        [APP_TERMINAL] = "terminal", [APP_FILES] = "files", [APP_EDITOR] = "editor", [APP_PYTHON] = "python",
        [APP_CAR] = "car", [APP_WEB] = "web", [APP_ELECTRONICS] = "electronics", [APP_ARDUINO] = "arduino",
        [APP_CLAUDE] = "claude", [APP_MEDIA] = "media", [APP_WIFI] = "wifi", [APP_SETTINGS] = "settings",
        [APP_TASKMGR] = "taskmanager", [APP_CALCULATOR] = "calculator", [APP_NOTES] = "notes",
        [APP_CLOCK] = "clock", [APP_IMAGES] = "images", [APP_WHATSAPP] = "whatsapp", [APP_PROBE] = "probe",
        [APP_DEVTOOLS] = "devtools", [APP_HEALTH] = "health", [APP_BTSCAN] = "bluetooth", [APP_UPDATE] = "update" };
    if (what == MENU_SYSTEM) return "system";
    return what >= 0 && what < APP_ALL_COUNT ? names[what] : NULL;
}

static int menu_open(Menu *m) { return m->panel && !lv_obj_has_flag(m->panel, LV_OBJ_FLAG_HIDDEN); }
int desktop_start_menu_is_open(void) { return menu_open(&start); }
int desktop_desk_menu_is_open(void) { return menu_open(&desk); }
lv_obj_t *desktop_start_menu_obj(void) { return start.panel; }

/* a press outside an open menu closes it (the start button opens / closes
 * the start menu itself) */
static void close_menus_outside(int x, int y)
{
    if (menu_open(&start) && !inside(start.panel, x, y) && !inside(start_btn, x, y)) desktop_close_menus();
    if (menu_open(&desk) && !inside(desk.panel, x, y)) desktop_close_menus();
}

static void menu_select(Menu *m, int col, int row)
{
    if (m->sel_col >= 0) lv_obj_clear_state(m->item[m->sel_col][m->sel_row], LV_STATE_CHECKED);
    m->sel_col = col;
    m->sel_row = row;
    if (col < 0) return;
    lv_obj_add_state(m->item[col][row], LV_STATE_CHECKED);
    lv_obj_scroll_to_view_recursive(m->item[col][row], LV_ANIM_OFF);   /* (a menu that scrolls) */
}

static void menu_hide(Menu *m)
{
    if (!m->panel) return;
    menu_select(m, -1, 0);
    lv_obj_add_flag(m->panel, LV_OBJ_FLAG_HIDDEN);
}

void desktop_close_menus(void) { menu_hide(&start); menu_hide(&desk); }

void desktop_toggle_start_menu(void)
{
    if (desktop_start_menu_is_open()) { desktop_close_menus(); return; }
    menu_hide(&desk);
    lv_obj_clear_flag(start.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(start.panel, 0, LV_ANIM_OFF);
    lv_obj_move_foreground(start.panel);
}

static void menu_clicked(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    desktop_close_menus();
    do_action(what);
}

/* one row of a menu: icon + name */
static void menu_item(Menu *m, int col, lv_obj_t *parent, int icon, const char *text, int what)
{
    lv_obj_t *b = th_box(parent, TS_TEXT);
    th_add_to(b, TS_ROW_SEL, LV_STATE_PRESSED);
    th_add_to(b, TS_ROW_SEL, LV_STATE_CHECKED);      /* (the keyboard's highlight) */
    lv_obj_set_size(b, LV_PCT(100), 26);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(b, 8, 0);
    lv_obj_set_style_pad_column(b, 10, 0);
    icon_img(b, icon, 20, TS_ICON);
    lv_obj_t *l = lv_label_create(b);                 /* (text colour from the row) */
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    if (m->count[col] < MENU_ROWS) {
        m->item[col][m->count[col]] = b;
        m->what[col][m->count[col]] = what;
        m->icon[col][m->count[col]] = icon;
        m->count[col]++;
    }
}

static lv_obj_t *menu_heading(lv_obj_t *col, const char *text)
{
    lv_obj_t *h = th_label(col, text, TS_SMALL);
    lv_obj_set_style_pad_left(h, 8, 0);
    lv_obj_set_style_pad_bottom(h, 3, 0);
    return h;
}

static lv_obj_t *menu_column(lv_obj_t *parent, const char *heading, int width)
{
    lv_obj_t *col = th_box(parent, -1);
    lv_obj_set_size(col, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    menu_heading(col, heading);
    return col;
}

static const char *item_text(lv_obj_t *item) { return lv_label_get_text(lv_obj_get_child(item, 1)); }

const char *desktop_menu_selected_text(void)
{
    Menu *m = menu_open(&start) ? &start : menu_open(&desk) ? &desk : NULL;
    if (!m || m->sel_col < 0) return "";
    return item_text(m->item[m->sel_col][m->sel_row]);
}

/* move the highlight: dc columns, dr rows (wrapping around) */
static void menu_move(Menu *m, int dc, int dr)
{
    if (m->sel_col < 0) { menu_select(m, 0, 0); return; }    /* the first arrow: the first row */
    int c = m->sel_col, r = m->sel_row;
    if (dc) {
        do c = (c + dc + MENU_COLS) % MENU_COLS; while (!m->count[c]);
        if (r >= m->count[c]) r = m->count[c] - 1;
    }
    if (dr) r = (r + dr + m->count[c]) % m->count[c];
    menu_select(m, c, r);
}

/* a letter: the next row (after the highlight) whose name starts with it */
static void menu_letter(Menu *m, char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    int total = 0, at = 0;
    for (int c = 0; c < MENU_COLS; c++) total += m->count[c];
    if (m->sel_col >= 0) for (int c = 0; c < m->sel_col; c++) at += m->count[c];
    if (m->sel_col >= 0) at += m->sel_row;
    for (int k = 1; k <= total; k++) {
        int n = (at + k) % total, c = 0;
        while (n >= m->count[c]) n -= m->count[c++];
        char first = item_text(m->item[c][n])[0];
        if (first >= 'A' && first <= 'Z') first = (char)(first + 32);
        if (first == ch) { menu_select(m, c, n); return; }
    }
}

/* the keyboard while a menu is open: it takes every key */
static int menu_kbd(Menu *m, const KbEvent *ev)
{
    switch (ev->key) {
    case KB_ESC:     desktop_close_menus(); break;
    case KB_UP:      menu_move(m, 0, -1); break;
    case KB_DOWN:    menu_move(m, 0, 1); break;
    case KB_LEFT:
    case KB_BACKTAB: menu_move(m, -1, 0); break;
    case KB_RIGHT:
    case KB_TAB:     menu_move(m, 1, 0); break;
    case KB_HOME:    if (m->sel_col >= 0) menu_select(m, m->sel_col, 0); else menu_move(m, 0, 0); break;
    case KB_END:     if (m->sel_col < 0) menu_move(m, 0, 0);
                     menu_select(m, m->sel_col, m->count[m->sel_col] - 1); break;
    case KB_CHAR:    menu_letter(m, ev->ch); break;
    case KB_ENTER:
        if (m->sel_col < 0) { menu_move(m, 0, 0); break; }
        { int what = m->what[m->sel_col][m->sel_row]; desktop_close_menus(); do_action(what); }
        break;
    case KB_SUPER:
    case KB_CTRL_ESC:                             /* the start menu key: start menu open / closed */
        if (m == &start) desktop_close_menus();
        else { desktop_close_menus(); desktop_toggle_start_menu(); menu_select(&start, 0, 0); }
        break;
    default: break;                               /* (anything else does nothing while a menu is open) */
    }
    return 1;
}

/* The start menu always fits above the taskbar: if it is ever taller (a
 * theme with bigger letters), it gets the most height there is and scrolls. */
static void fit_start_menu(void)
{
    int hidden = lv_obj_has_flag(start.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(start.panel, LV_OBJ_FLAG_HIDDEN);          /* (hidden objects are not measured) */
    lv_obj_clear_flag(start.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(start.panel, LV_SIZE_CONTENT);
    lv_obj_update_layout(start.panel);
    int most = WORK_H - 8;
    if (lv_obj_get_height(start.panel) > most) {
        lv_obj_set_height(start.panel, most);
        lv_obj_add_flag(start.panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(start.panel, LV_DIR_VER);
    }
    lv_obj_align(start.panel, LV_ALIGN_BOTTOM_LEFT, 4, -(TASKBAR_H + 4));   /* sits on the taskbar */
    if (hidden) lv_obj_add_flag(start.panel, LV_OBJ_FLAG_HIDDEN);
}

static void build_start_menu(void)
{
    start.panel = th_box(scr, TS_PANEL);
    start.sel_col = -1;
    lv_obj_set_size(start.panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(start.panel, 8, 0);
    lv_obj_set_style_pad_column(start.panel, 6, 0);
    lv_obj_set_flex_flow(start.panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(start.panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *apps = menu_column(start.panel, "APPS", 160);
    for (int i = 0; i < APP_COUNT; i++)
        if (i != APP_WIFI) menu_item(&start, 0, apps, home_app_icon(i), home_app_name(i), i);   /* (Wi-Fi: in Settings) */

    lv_obj_t *tools = menu_column(start.panel, "TOOLS", 214);   /* (wide enough for "Update NZEUS Deck") */
    for (int i = APP_COUNT; i < APP_ALL_COUNT; i++) menu_item(&start, 1, tools, home_app_icon(i), home_app_name(i), i);
    menu_item(&start, 1, tools, ICON_SCREENSHOT, "Screenshot", MENU_SCREENSHOT);

    lv_obj_t *sys = menu_column(start.panel, "SYSTEM", 200);
    menu_item(&start, 2, sys, ICON_SYSTEM, "System info", MENU_SYSTEM);
    menu_item(&start, 2, sys, ICON_KEYBOARD, "Keyboard shortcuts", MENU_SHORTCUTS);
    menu_item(&start, 2, sys, ICON_ABOUT, "About NZEUS Deck", MENU_ABOUT);
    menu_item(&start, 2, sys, ICON_HANDHELD, "Handheld mode", MENU_HANDHELD);
    lv_obj_t *hint = th_label(sys, "Keyboard: arrows move,\nEnter opens, Esc closes", TS_SMALL);
    lv_obj_set_style_pad_left(hint, 8, 0);
    lv_obj_set_style_pad_top(hint, 12, 0);

    fit_start_menu();
    lv_obj_add_flag(start.panel, LV_OBJ_FLAG_HIDDEN);
}

/* the right-click menu on the empty desktop */
static void build_desk_menu(void)
{
    desk.panel = th_box(scr, TS_PANEL);
    desk.sel_col = -1;
    lv_obj_set_size(desk.panel, 214, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(desk.panel, 6, 0);
    lv_obj_set_flex_flow(desk.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(desk.panel, 1, 0);
    menu_item(&desk, 0, desk.panel, ICON_TERMINAL, "Open Terminal", APP_TERMINAL);
    menu_item(&desk, 0, desk.panel, ICON_FILES, "Files", APP_FILES);
    menu_item(&desk, 0, desk.panel, ICON_SETTINGS, "Settings", APP_SETTINGS);
    menu_item(&desk, 0, desk.panel, ICON_THEME, "Next theme", MENU_THEME);
    menu_item(&desk, 0, desk.panel, ICON_IMAGE, "Wallpaper style", MENU_WALLPAPER);
    menu_item(&desk, 0, desk.panel, ICON_KEYBOARD, "Keyboard shortcuts", MENU_SHORTCUTS);
    lv_obj_add_flag(desk.panel, LV_OBJ_FLAG_HIDDEN);
}

/* the desktop menu at x, y (moved in so it stays on the screen) */
static void open_desk_menu(int x, int y)
{
    menu_hide(&start);
    lv_obj_clear_flag(desk.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(desk.panel);
    lv_obj_update_layout(desk.panel);
    int w = lv_obj_get_width(desk.panel), h = lv_obj_get_height(desk.panel);
    if (x + w > SCR_W - 2) x = SCR_W - 2 - w;
    if (y + h > WORK_H - 2) y -= h;            /* no room below the pointer: open upwards */
    if (y < 2) y = 2;
    lv_obj_set_pos(desk.panel, x, y);
}

int desktop_right_click(int x, int y)
{
    desktop_close_menus();
    lv_point_t p = { (lv_coord_t)x, (lv_coord_t)y };
    lv_obj_t *o = lv_indev_search_obj(scr, &p);
    if (o != scr) { desktop_focus_at(x, y); return 0; }   /* on a window, an icon or the taskbar: no menu */
    open_desk_menu(x, y);
    return 1;
}

/* ================= the wallpaper =================
 * Hills and Picture are images drawn ONCE into a 640 x 446 buffer (about
 * 570 KB, wallpaper.c), freed when another wallpaper is chosen. Glow, Sky and
 * Plain cost nothing: the theme's own colours, a gradient from top to bottom.
 * The choice is saved as a name in settings.ini (desk_wall). */
enum { WALL_HILLS, WALL_PICTURE, WALL_GLOW, WALL_SKY, WALL_PLAIN, WALL_COUNT };
static const char *const wall_ids[WALL_COUNT] = { "hills", "picture", "glow", "sky", "plain" };
static const char *const wall_names[WALL_COUNT] = { "Hills", "Picture", "Glow", "Sky", "Plain" };
static const char *const wall_files[] = { NZ_DIR "/wallpaper.jpg", NZ_DIR "/wallpaper.png", NZ_DIR "/wallpaper.bmp" };
static lv_obj_t *wall_img;              /* the image on the desktop (Hills / Picture), NULL = none */
static uint16_t *wall_px;               /* its pixels */
static lv_img_dsc_t wall_dsc;
static int wall_shown = -1;             /* the wallpaper on the screen now */
static void desktop_icons_look(void);

/* the user's own picture file (the first that is there), NULL = none */
static const char *wall_file(void)
{
    for (size_t i = 0; i < sizeof wall_files / sizeof wall_files[0]; i++)
        if (access(wall_files[i], R_OK) == 0) return wall_files[i];
    return NULL;
}

/* the wallpaper a theme starts with: Retro Blue = the user's picture if there
 * is one, else Hills; the other themes = Glow */
static int wall_default(void) { return theme()->retro ? (wall_file() ? WALL_PICTURE : WALL_HILLS) : WALL_GLOW; }

static int wall_saved(void)
{
    const char *v = settings_get("desk_wall", "");
    for (int i = 0; i < WALL_COUNT; i++) if (!strcmp(v, wall_ids[i])) return i;
    if (v[0] >= '0' && v[0] <= '2' && !v[1]) return WALL_GLOW + (v[0] - '0');   /* (older numbers: 0 Glow, 1 Sky, 2 Plain) */
    return wall_default();
}

static void free_wall_image(void)
{
    if (wall_img) { lv_obj_del(wall_img); wall_img = NULL; }
    if (wall_px) { lv_img_cache_invalidate_src(&wall_dsc); free(wall_px); wall_px = NULL; }
}

/* Show a wallpaper. `say`: a message when the picture is missing (when the
 * user picks it; not at start). The picture and Hills are only drawn again
 * when they are not on the screen already. */
static void show_wallpaper(int look, int say)
{
    const NzTheme *t = theme();
    if (look == WALL_PICTURE || look == WALL_HILLS) {
        /* (drawn again only when it changes - but a picture the user picks is
         *  always read again: the file may be a new one) */
        if (look != wall_shown || !wall_px || (look == WALL_PICTURE && say)) {
            free_wall_image();                     /* (the old buffer first: never two at once) */
            if (look == WALL_PICTURE) {
                char err[80] = "";
                const char *f = wall_file();
                wall_px = f ? wallpaper_picture(f, SCR_W, WORK_H, err, sizeof err) : NULL;
                if (!wall_px) {                    /* missing or not a picture: Hills instead */
                    if (say) ui_toast("Put a picture named wallpaper.jpg in the nzeus folder");
                    look = WALL_HILLS;
                }
            }
            if (!wall_px) wall_px = wallpaper_hills(SCR_W, WORK_H);
            if (wall_px) {
                memset(&wall_dsc, 0, sizeof wall_dsc);
                wall_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                wall_dsc.header.w = SCR_W;
                wall_dsc.header.h = WORK_H;
                wall_dsc.data_size = (uint32_t)SCR_W * WORK_H * sizeof(uint16_t);
                wall_dsc.data = (const uint8_t *)wall_px;
                wall_img = lv_img_create(scr);
                lv_obj_clear_flag(wall_img, LV_OBJ_FLAG_CLICKABLE);   /* (a click on it = on the desktop) */
                lv_img_set_src(wall_img, &wall_dsc);
                lv_obj_set_pos(wall_img, 0, 0);
                lv_obj_move_background(wall_img);  /* behind the icons and windows */
            } else look = WALL_GLOW;               /* (no memory for it: the gradient) */
        }
    } else free_wall_image();
    wall_shown = look;
    int image = wall_img != NULL;
    /* the screen's own background: a gradient, or plain under an image (cheapest) */
    lv_color_t bg = th_color(t->bg);
    lv_color_t tint = lv_color_mix(th_color(t->accent), bg, t->dark ? 56 : 40);   /* a little of the accent */
    lv_obj_set_style_bg_color(scr, look == WALL_SKY ? tint : bg, 0);
    lv_obj_set_style_bg_grad_color(scr, look == WALL_SKY ? bg : tint, 0);
    lv_obj_set_style_bg_grad_dir(scr, look == WALL_PLAIN || image ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, 0);
    for (int i = 0; i < 2; i++) {
        if (!watermark[i]) continue;
        if (image) lv_obj_add_flag(watermark[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(watermark[i], LV_OBJ_FLAG_HIDDEN);
    }
    desktop_icons_look();
}

/* the desktop menu's "Wallpaper style": the next one, saved */
void desktop_cycle_wallpaper(void)
{
    int look = (wall_saved() + 1) % WALL_COUNT;
    char msg[64];
    settings_set("desk_wall", wall_ids[look]);
    settings_set_int("desk_wall_auto", 0);                 /* (picked by the user: kept) */
    show_wallpaper(look, 1);
    if (look == WALL_PICTURE && wall_shown != WALL_PICTURE) return;     /* (the message said why) */
    snprintf(msg, sizeof msg, "Wallpaper: %s", wall_names[look]);
    ui_toast(msg);
}

const char *desktop_wallpaper_shown(void) { return wall_shown >= 0 ? wall_ids[wall_shown] : ""; }
int desktop_wallpaper_has_image(void) { return wall_img != NULL && wall_px != NULL; }

/* ================= what the menus and icons do ================= */
static void do_action(int what)
{
    char msg[64];
    switch (what) {
    case MENU_SYSTEM:     desktop_open_window(WIN_SYSTEM); break;
    case MENU_ABOUT:      desktop_open_window(WIN_ABOUT); break;
    case MENU_SHORTCUTS:  desktop_open_window(WIN_SHORTCUTS); break;
    case MENU_HANDHELD:   ui_set_mode(UI_HANDHELD); break;
    case MENU_SCREENSHOT: if (desktop_screenshot_fn) desktop_screenshot_fn(); break;
    case MENU_THEME:
        theme_set((theme_index() + 1) % TH_COUNT);          /* (saved; Settings > Theme has them all) */
        snprintf(msg, sizeof msg, "Theme: %s", theme()->name);
        ui_toast(msg);
        break;
    case MENU_WALLPAPER: desktop_cycle_wallpaper(); break;
    default: ui_open_app(what); break;
    }
}

/* ================= the keyboard (desktop mode) ================= */
/* apps that pass Ctrl + letters on to the program inside (Ctrl+W there = delete a word) */
static int takes_ctrl_letters(Window *w) { return w && w->app && (w->app->id == APP_TERMINAL || w->app->id == APP_CLAUDE); }

static int is_window_key(KbKey k)
{
    return k == KB_ALT_TAB || k == KB_ALT_F4 || k == KB_ALT_ENTER || k == KB_CTRL_ALT_T ||
           k == KB_SUPER_UP || k == KB_SUPER_DOWN || k == KB_SUPER_LEFT || k == KB_SUPER_RIGHT;
}

int desktop_kbd(const KbEvent *ev)
{
    Menu *m = menu_open(&start) ? &start : menu_open(&desk) ? &desk : NULL;
    if (m) {
        if (!is_window_key(ev->key)) return menu_kbd(m, ev);
        desktop_close_menus();                    /* a window shortcut closes the menu first */
    }
    Window *w = front && front->win && !front->minimized ? front : NULL;
    switch (ev->key) {
    case KB_SUPER:
    case KB_CTRL_ESC:
        desktop_toggle_start_menu();
        if (desktop_start_menu_is_open()) menu_select(&start, 0, 0);   /* keyboard: the first app is highlighted */
        return 1;
    case KB_ALT_TAB:     next_window(ev->ch == KB_SHIFTED ? -1 : 1); return 1;
    case KB_ALT_F4:      close_window(w); return 1;
    case KB_ALT_ENTER:   toggle_max(w); return 1;
    case KB_CTRL_ALT_T:  do_action(APP_TERMINAL); return 1;
    case KB_SUPER_UP:    if (w) { set_state(w, WS_MAX); focus_window(w); } return 1;
    case KB_SUPER_DOWN:
        if (w && w->state != WS_NORMAL) { set_state(w, WS_NORMAL); focus_window(w); }
        else minimize(w);
        return 1;
    case KB_SUPER_LEFT:
        if (w) { set_state(w, w->state == WS_RIGHT ? WS_NORMAL : WS_LEFT); focus_window(w); }
        return 1;
    case KB_SUPER_RIGHT:
        if (w) { set_state(w, w->state == WS_LEFT ? WS_NORMAL : WS_RIGHT); focus_window(w); }
        return 1;
    case KB_CTRL_OTHER:
        if (ev->ch == 'w' && w && !takes_ctrl_letters(w)) { close_window(w); return 1; }
        return 0;
    default:
        return 0;                                 /* everything else: the app in front */
    }
}

/* ================= the tray: Wi-Fi, Bluetooth, volume, battery, clock ================= */
static int tray_charging, tray_battery = -1, tray_wifi;
static int vol_muted, vol_muted_at;               /* muted from the tray (at this Settings volume) */
static int status_calls;

/* an item that is off is drawn faint */
static void faint(lv_obj_t *o, int off) { lv_obj_set_style_text_opa(o, off ? LV_OPA_40 : LV_OPA_COVER, 0); }

static void refresh_bt_volume(void)
{
    faint(tray[TRAY_BT], bt_state() != BT_ON);
    int v = settings_get_int("volume", 70);
    if (vol_muted && v != vol_muted_at) vol_muted = 0;     /* the volume was changed elsewhere: sound is on again */
    set_text(tray[TRAY_VOLUME], vol_muted || v == 0 ? LV_SYMBOL_MUTE : v < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
}

static void tray_clicked(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    char msg[96];
    switch (which) {
    case TRAY_WIFI: {
        const char *ip = wifi_ip();
        if (tray_wifi && ip[0]) snprintf(msg, sizeof msg, "Wi-Fi: connected, address %s", ip);
        else if (tray_wifi) snprintf(msg, sizeof msg, "Wi-Fi: on, no internet address yet");
        else snprintf(msg, sizeof msg, "Wi-Fi: off (Settings > Wi-Fi switches it on)");
        break;
    }
    case TRAY_BT: {
        static const char *const words[] = {
            [BT_CHECKING] = "checking...", [BT_MISSING] = "not installed on this card",
            [BT_OFF] = "off", [BT_NO_DONGLE] = "on, but no dongle plugged in",
            [BT_STARTING] = "switching on...", [BT_STOPPING] = "switching off...", [BT_ON] = "on" };
        int s = bt_state();
        bt_refresh();                              /* (look again soon) */
        snprintf(msg, sizeof msg, "Bluetooth: %s (Settings > Bluetooth)", s >= 0 && s <= BT_ON ? words[s] : "?");
        break;
    }
    case TRAY_VOLUME: {
        int v = settings_get_int("volume", 70);
        if (hw_set_volume(vol_muted ? v : 0) != 0) { snprintf(msg, sizeof msg, "Volume: no sound device here"); break; }
        vol_muted = !vol_muted;
        vol_muted_at = v;
        if (vol_muted) snprintf(msg, sizeof msg, "Sound off (click again to turn it on)");
        else snprintf(msg, sizeof msg, "Sound on: %d %%", v);
        refresh_bt_volume();
        break;
    }
    case TRAY_BATTERY:
        if (tray_battery < 0) snprintf(msg, sizeof msg, "Battery: not known");
        else snprintf(msg, sizeof msg, "Battery: %d %%%s", tray_battery, tray_charging ? ", charging" : "");
        break;
    default: {                                     /* the clock: today's date */
        time_t now = hw_now();
        struct tm tm = *localtime(&now);
        strftime(msg, sizeof msg, "%A %d %B %Y, %H:%M", &tm);
        break;
    }
    }
    ui_toast(msg);
}

void desktop_set_charging(int on) { tray_charging = on; }

void desktop_set_status(const char *clock, int battery_percent, int wifi_on)
{
    char batt[32];
    home_battery_text(batt, sizeof batt, battery_percent, tray_charging);
    tray_battery = battery_percent;
    tray_wifi = wifi_on;
    set_text(tray[TRAY_CLOCK], clock);
    set_text(tray[TRAY_BATTERY], batt);
    if (batt[0]) lv_obj_clear_flag(tray[TRAY_BATTERY], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(tray[TRAY_BATTERY], LV_OBJ_FLAG_HIDDEN);
    faint(tray[TRAY_WIFI], !wifi_on);
    /* Bluetooth's state comes from a small background program: ask it now
     * and then, and only while the desktop is on the screen */
    if (lv_scr_act() == scr && status_calls++ % BT_LOOK_EVERY == 0) bt_poll();
    refresh_bt_volume();
    static lv_coord_t last_w = -1;               /* the tray got wider / narrower: share the taskbar again */
    lv_obj_update_layout(tray_box);
    if (lv_obj_get_width(tray_box) != last_w) { last_w = lv_obj_get_width(tray_box); layout_taskbar(); }
}

void desktop_set_badge(const char *text)
{
    if (!badge) return;
    if (text) { lv_label_set_text(badge, text); lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    layout_taskbar();
}

/* ================= for tests ================= */
lv_obj_t *desktop_front_window(void) { return front && front->win && !front->minimized ? front->win : NULL; }
int desktop_window_state(lv_obj_t *win) { Window *w = window_of(win); return w ? w->state : -1; }
int desktop_window_minimized(lv_obj_t *win) { Window *w = window_of(win); return w ? w->minimized : 0; }
lv_obj_t *desktop_task_button(lv_obj_t *win) { Window *w = window_of(win); return w ? w->task_btn : NULL; }
lv_obj_t *desktop_tray_item(int which) { return which >= 0 && which < TRAY_COUNT ? tray[which] : NULL; }

/* ================= building the desktop ================= */
typedef struct { int icon; const char *name; int what; } DeskIcon;
static const DeskIcon desk_icons[] = {          /* two columns of four, top to bottom */
    { ICON_FILES,    "Files",    APP_FILES    },
    { ICON_TERMINAL, "Terminal", APP_TERMINAL },
    { ICON_WEB,      "Web",      APP_WEB      },
    { ICON_CAR,      "Car",      APP_CAR      },
    { ICON_SETTINGS, "Settings", APP_SETTINGS },
    { ICON_MEDIA,    "Media",    APP_MEDIA    },
    { ICON_EDITOR,   "Editor",   APP_EDITOR   },
    { ICON_SYSTEM,   "System",   MENU_SYSTEM  },
};
#define ICON_ROWS 4
static int desk_icon_icon(int i) { return desk_icons[i].icon; }
static int desk_icon_what(int i) { return desk_icons[i].what; }

static void desk_icon_clicked(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    desktop_close_menus();
    do_action(what);
}

static void start_clicked(lv_event_t *e) { (void)e; desktop_toggle_start_menu(); }

/* ================= the Retro Blue look =================
 * With the Retro Blue theme (theme()->retro) the desktop looks like a classic
 * early-2000s PC desktop (our own colours and drawing): a blue taskbar, a
 * green "start" button, a light-blue tray, blue window title bars with blue
 * minimize / maximize and an orange-red close button, beige window bodies.
 * These are extra styles added on top of the theme's styles, and taken off
 * again for the other themes. Gradients are LVGL's own (one colour per line). */
static lv_style_t xp_bar, xp_start, xp_tray, xp_task, xp_task_on, xp_white, xp_title, xp_btn, xp_close,
                  xp_body, xp_win, xp_menu, xp_icon_box, xp_icon_img, xp_tile, xp_name;

/* a top-to-bottom gradient from `top` to `bottom` */
static void grad(lv_style_t *s, uint32_t top, uint32_t bottom)
{
    lv_style_set_bg_color(s, lv_color_hex(top));
    lv_style_set_bg_grad_color(s, lv_color_hex(bottom));
    lv_style_set_bg_grad_dir(s, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(s, LV_OPA_COVER);
}

/* a small button on a title bar: a gradient, rounded, with a white edge and white sign */
static void title_btn_style(lv_style_t *s, uint32_t top, uint32_t bottom)
{
    grad(s, top, bottom);
    lv_style_set_radius(s, 3);
    lv_style_set_border_width(s, 1);
    lv_style_set_border_color(s, lv_color_white());
    lv_style_set_text_color(s, lv_color_white());
    lv_style_set_shadow_width(s, 0);
}

static void init_retro_styles(void)
{
    lv_style_t *all[] = { &xp_bar, &xp_start, &xp_tray, &xp_task, &xp_task_on, &xp_white, &xp_title, &xp_btn,
                          &xp_close, &xp_body, &xp_win, &xp_menu, &xp_icon_box, &xp_icon_img, &xp_tile, &xp_name };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lv_style_init(all[i]);
    grad(&xp_bar, 0x3C8AF6, 0x1E4EC4);                    /* taskbar: blue, a light line on top */
    lv_style_set_border_side(&xp_bar, LV_BORDER_SIDE_TOP);
    lv_style_set_border_width(&xp_bar, 1);
    lv_style_set_border_color(&xp_bar, lv_color_hex(0x7FB2FA));
    grad(&xp_start, 0x52C552, 0x2B8A2B);                  /* the green start button */
    lv_style_set_radius(&xp_start, 15);
    lv_style_set_shadow_width(&xp_start, 0);
    lv_style_set_border_width(&xp_start, 0);
    lv_style_set_text_color(&xp_start, lv_color_white());
    lv_style_set_text_font(&xp_start, &lv_font_montserrat_20);
    grad(&xp_tray, 0x18A6EF, 0x0C84D9);                   /* the tray: lighter blue, a dark line on its left */
    lv_style_set_border_side(&xp_tray, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&xp_tray, 1);
    lv_style_set_border_color(&xp_tray, lv_color_hex(0x0A4CA6));
    lv_style_set_pad_hor(&xp_tray, 10);
    grad(&xp_task, 0x4A95F8, 0x2C6FE2);                   /* a window's taskbar button */
    lv_style_set_radius(&xp_task, 3);
    lv_style_set_border_width(&xp_task, 0);
    lv_style_set_shadow_width(&xp_task, 0);
    grad(&xp_task_on, 0x1A4AB6, 0x2058C8);                /* ... the front window's: pressed in */
    lv_style_set_text_color(&xp_white, lv_color_white());
    grad(&xp_title, 0x3A86F7, 0x0846D0);                  /* window title bars */
    lv_style_set_text_color(&xp_title, lv_color_white());
    title_btn_style(&xp_btn, 0x4C8EF7, 0x2357D2);         /* minimize / maximize: blue */
    title_btn_style(&xp_close, 0xEE7A4C, 0xC73A18);       /* close: orange-red */
    lv_style_set_bg_color(&xp_body, lv_color_hex(0xECE9D8));     /* window bodies: beige */
    lv_style_set_border_color(&xp_win, lv_color_hex(0x0A44C8));  /* the window's blue frame */
    lv_style_set_border_width(&xp_win, 2);
    lv_style_set_border_color(&xp_menu, lv_color_hex(0x245EDB));
    lv_style_set_border_width(&xp_menu, 2);
    lv_style_set_bg_color(&xp_icon_box, lv_color_black());       /* icons on a picture: a dark see-through square */
    lv_style_set_bg_opa(&xp_icon_box, LV_OPA_30);
    lv_style_set_radius(&xp_icon_box, 6);
    lv_style_set_img_recolor(&xp_icon_img, lv_color_white());    /* ... white icon, white name */
    lv_style_set_img_recolor_opa(&xp_icon_img, LV_OPA_COVER);
    lv_style_set_text_color(&xp_icon_img, lv_color_white());
    grad(&xp_tile, 0xFFFFFF, 0xDCE6F4);                          /* Retro Blue icons: a soft light tile */
    lv_style_set_radius(&xp_tile, 10);
    lv_style_set_border_width(&xp_tile, 1);
    lv_style_set_border_color(&xp_tile, lv_color_hex(0xA9BEDD));
    lv_style_set_text_color(&xp_name, lv_color_white());         /* ... and the name: white on a dark strip */
    lv_style_set_bg_color(&xp_name, lv_color_black());
    lv_style_set_bg_opa(&xp_name, LV_OPA_40);
    lv_style_set_radius(&xp_name, 4);
    lv_style_set_pad_hor(&xp_name, 4);
}

/* add a style (on = 1) or take it off (on = 0); doing it twice is harmless */
static void look(lv_obj_t *o, lv_style_t *s, lv_style_selector_t sel, int on)
{
    if (!o) return;
    lv_obj_remove_style(o, s, sel);
    if (on) lv_obj_add_style(o, s, sel);
}

/* a window: its title bar, buttons, body and taskbar button */
static void look_window(Window *w)
{
    int r = theme()->retro;
    lv_obj_t *hd = lv_win_get_header(w->win);             /* children: icon, title, -, box, X */
    lv_obj_t *minb = lv_obj_get_child(hd, 2), *maxb = lv_obj_get_child(hd, 3), *cls = lv_obj_get_child(hd, 4);
    look(w->win, &xp_win, 0, r);
    look(hd, &xp_title, 0, r);
    look(lv_obj_get_child(hd, 0), &xp_icon_img, 0, r);    /* its icon: white on blue */
    look(lv_obj_get_child(hd, 1), &xp_white, 0, r);       /* the title */
    look(minb, &xp_btn, 0, r);
    look(lv_obj_get_child(minb, 0), &xp_white, 0, r);     /* its "-" */
    look(maxb, &xp_btn, 0, r);
    look(cls, &xp_close, 0, r);
    lv_obj_t *bs[3] = { minb, maxb, cls };                /* Retro Blue: small square buttons */
    for (int i = 0; i < 3; i++) lv_obj_set_size(bs[i], r ? 22 : 30, r ? 22 : LV_PCT(100));
    lv_obj_set_style_pad_column(hd, r ? 3 : 0, 0);
    lv_obj_set_style_pad_right(hd, r ? 5 : 0, 0);
    look(lv_win_get_content(w->win), &xp_body, 0, r);
    look(w->task_btn, &xp_task, 0, r);
    look(w->task_btn, &xp_task_on, LV_STATE_CHECKED, r);
    look(lv_obj_get_child(w->task_btn, 0), &xp_icon_img, 0, r);
    look(lv_obj_get_child(w->task_btn, 1), &xp_white, 0, r);
    draw_max_icon(w);
}

/* The desktop icons.
 *   Retro Blue: each built-in icon in its own colour on a soft rounded tile,
 *     the name in white on a dark see-through strip (easy to read on a picture)
 *   other themes on a picture wallpaper: a dark see-through square, white icon
 *   the owner's own icon (if there is one) is shown as it is, without a tile */
static const uint32_t retro_icon_colors[8] = {
    0xE0A010,      /* Files: folder yellow */
    0x2B2B2B,      /* Terminal: black */
    0x1E6FD9,      /* Web: blue */
    0xD32F2F,      /* Car: red */
    0x52657F,      /* Settings: grey-blue */
    0xF07800,      /* Media: orange */
    0x1565C0,      /* Editor: blue */
    0x2E9D3A,      /* System: green */
};

static void desktop_icons_look(void)
{
    int r = theme()->retro, pic = wall_img != NULL;
    for (int i = 0; i < 8; i++) {
        DeskIconObj *d = &dicons[i];
        if (!d->box) continue;
        int own = own_desk[i].px != NULL;
        if (r && !own) lv_obj_clear_flag(d->tile, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(d->tile, LV_OBJ_FLAG_HIDDEN);
        if (own) { lv_obj_add_flag(d->glyph, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(d->own, LV_OBJ_FLAG_HIDDEN); }
        else { lv_obj_clear_flag(d->glyph, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(d->own, LV_OBJ_FLAG_HIDDEN); }
        look(d->box, &xp_icon_box, 0, pic && !r);
        look(d->glyph, &xp_icon_img, 0, pic && !r);
        if (r) lv_obj_set_style_img_recolor(d->glyph, lv_color_hex(retro_icon_colors[i]), 0);
        else lv_obj_remove_local_style_prop(d->glyph, LV_STYLE_IMG_RECOLOR, 0);
        look(d->name, &xp_icon_img, 0, pic && !r);
        look(d->name, &xp_name, 0, r);
    }
}

/* Load the owner's own icons (again): the start button's logo, the desktop
 * icons and the menus' icons. Each one is shown only if its file is there
 * and is a picture; the old ones are freed first (nothing is kept twice). */
void desktop_reload_own_icons(void)
{
    /* the start button's logo (at most 24 x 24) */
    lv_obj_add_flag(start_icon, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(start_icon, icon_get(ICON_ABOUT, 20));       /* (let go of the old picture first) */
    own_icon_free(&own_start);
    if (own_icon_load(&own_start, "start-icon", 24)) {
        lv_img_set_src(start_icon, &own_start.dsc);
        lv_obj_clear_flag(start_icon, LV_OBJ_FLAG_HIDDEN);
    }
    /* the desktop icons (at most 32 x 32, like the built-in ones) */
    char base[48];
    for (int i = 0; i < 8; i++) {
        DeskIconObj *d = &dicons[i];
        if (!d->box) continue;
        lv_img_set_src(d->own, icon_get(desk_icon_icon(i), 32));
        own_icon_free(&own_desk[i]);
        const char *nm = own_icon_name(desk_icon_what(i));
        snprintf(base, sizeof base, "icons/%s", nm ? nm : "-");
        if (nm && own_icon_load(&own_desk[i], base, 32)) {
            lv_img_set_src(d->own, &own_desk[i].dsc);
            lv_obj_align(d->own, LV_ALIGN_TOP_MID, 0, 8 + (32 - (int)own_desk[i].dsc.header.h) / 2);
        }
    }
    /* the menus' rows (at most 20 x 20) */
    Menu *menus[2] = { &start, &desk };
    for (int k = 0; k < 2; k++)
        for (int c = 0; c < MENU_COLS; c++)
            for (int r = 0; r < menus[k]->count[c]; r++) {
                lv_obj_t *img = lv_obj_get_child(menus[k]->item[c][r], 0);
                OwnIcon *oi = &menus[k]->own[c][r];
                lv_img_set_src(img, icon_get((NzIcon)menus[k]->icon[c][r], 20));
                lv_obj_remove_local_style_prop(img, LV_STYLE_IMG_RECOLOR_OPA, 0);   /* (built-in: theme colour) */
                own_icon_free(oi);
                const char *nm = own_icon_name(menus[k]->what[c][r]);
                /* a ready-made small copy first (icons/small/: smoothly made smaller on the
                 * PC), else the desktop-size picture (here shrunk by skipping pixels) */
                char small[56];
                snprintf(small, sizeof small, "icons/small/%s", nm ? nm : "-");
                snprintf(base, sizeof base, "icons/%s", nm ? nm : "-");
                if (nm && (own_icon_load(oi, small, 20) || own_icon_load(oi, base, 20))) {
                    lv_img_set_src(img, &oi->dsc);
                    lv_obj_set_style_img_recolor_opa(img, LV_OPA_TRANSP, 0);      /* (own: its own colours) */
                }
            }
    desktop_icons_look();
    start_layout();
}

/* for tests: how many of the owner's icons are loaded now (start logo, desktop, menus) */
int desktop_own_icon_count(void)
{
    int n = own_start.px != NULL;
    for (int i = 0; i < 8; i++) n += own_desk[i].px != NULL;
    Menu *menus[2] = { &start, &desk };
    for (int k = 0; k < 2; k++)
        for (int c = 0; c < MENU_COLS; c++)
            for (int r = 0; r < menus[k]->count[c]; r++) n += menus[k]->own[c][r].px != NULL;
    return n;
}
int desktop_menu_own_icon_w(int i)
{
    if (i < 0 || i >= 8) return 0;
    for (int c = 0; c < MENU_COLS; c++)
        for (int r = 0; r < start.count[c]; r++)
            if (start.what[c][r] == desk_icon_what(i) && start.own[c][r].px) return (int)start.own[c][r].dsc.header.w;
    return 0;
}
lv_obj_t *desktop_start_icon(void) { return start_icon; }
lv_obj_t *desktop_icon_part(int i, int part) { return i < 0 || i >= 8 ? NULL : part == 0 ? dicons[i].glyph : part == 1 ? dicons[i].own : dicons[i].tile; }

/* the start button's inside: the owner's logo (if any) on the left and the
 * text after it; else the text in the middle */
static void start_layout(void)
{
    int r = theme()->retro;
    lv_obj_set_style_pad_all(start_btn, 0, 0);     /* (positions below are from the button's own edge) */
    int logo = !lv_obj_has_flag(start_icon, LV_OBJ_FLAG_HIDDEN);
    int lw = logo ? (int)own_start.dsc.header.w : 0;
    int left = r ? 16 + 7 : 6;                      /* (Retro Blue: 16 px of the button are past the edge) */
    lv_obj_align(start_icon, LV_ALIGN_LEFT_MID, left, 0);
    if (logo) {
        lv_obj_align(start_label, LV_ALIGN_LEFT_MID, left + lw + 4, r ? -1 : 0);
        lv_obj_align(start_bold, LV_ALIGN_LEFT_MID, left + lw + 5, -1);
    } else {
        lv_obj_align(start_label, LV_ALIGN_CENTER, r ? 5 : 0, r ? -1 : 0);
        lv_obj_align(start_bold, LV_ALIGN_CENTER, 6, -1);
    }
}

/* everything on the desktop, for the theme in use */
static void desktop_look(void)
{
    int r = theme()->retro;
    look(taskbar, &xp_bar, 0, r);
    look(start_btn, &xp_start, 0, r);
    if (r) {                       /* rounded only on the right: its left end is past the screen's edge */
        lv_obj_set_size(start_btn, 110, TASKBAR_H);
        lv_obj_set_pos(start_btn, -16, 0);
        lv_label_set_text(start_label, "start");
        lv_obj_clear_flag(start_bold, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_size(start_btn, 92, 26);
        lv_obj_align(start_btn, LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_text(start_label, "NZEUS");
        lv_obj_add_flag(start_bold, LV_OBJ_FLAG_HIDDEN);
    }
    start_layout();
    look(tray_box, &xp_tray, 0, r);
    lv_obj_align(tray_box, LV_ALIGN_RIGHT_MID, r ? 0 : -10, 0);
    for (int i = 0; i < TRAY_COUNT; i++) look(tray[i], &xp_white, 0, r);
    look(start.panel, &xp_menu, 0, r);
    look(desk.panel, &xp_menu, 0, r);
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].win) look_window(&windows[i]);
}

/* a new theme: the looks that are set here (not by the shared styles) again */
static int last_theme = -1;
static void desktop_theme_changed(void)
{
    int was_retro = last_theme >= 0 && theme_get(last_theme)->retro;
    if (theme()->retro && !was_retro && last_theme >= 0) {
        /* Retro Blue starts with its own wallpaper (the one before is kept for later) */
        char before[16];
        snprintf(before, sizeof before, "%s", settings_get("desk_wall", "glow"));
        settings_set("desk_wall_before", before);
        settings_set("desk_wall", wall_ids[wall_default()]);
        settings_set_int("desk_wall_auto", 1);
    } else if (!theme()->retro && was_retro && settings_get_int("desk_wall_auto", 0)) {
        /* leaving Retro Blue without having picked a wallpaper: the one from before */
        char before[16];
        snprintf(before, sizeof before, "%s", settings_get("desk_wall_before", "glow"));
        settings_set("desk_wall", before);
        settings_set_int("desk_wall_auto", 0);
    }
    last_theme = theme_index();
    desktop_reload_own_icons();
    desktop_look();
    show_wallpaper(wall_saved(), 0);
    fit_start_menu();                            /* (other letters: another height) */
    layout_taskbar();
}

static lv_obj_t *tray_label(int which, const char *text)
{
    lv_obj_t *l = th_label(tray_box, text, TS_TEXT);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, 5);
    lv_obj_add_event_cb(l, tray_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    tray[which] = l;
    return l;
}

lv_obj_t *desktop_create(void)
{
    scr = lv_obj_create(NULL);
    th_add(scr, TS_SCREEN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    init_retro_styles();

    /* a quiet NZEUS mark in the middle of the free space */
    lv_obj_t *wm = th_label(scr, "NZEUS", TS_WATERMARK);
    lv_obj_align(wm, LV_ALIGN_CENTER, 84, -30);
    lv_obj_t *wm2 = th_label(scr, "DECK", TS_WATERMARK);
    lv_obj_align_to(wm2, wm, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    watermark[0] = wm;
    watermark[1] = wm2;

    /* desktop icons: two columns of four */
    for (size_t i = 0; i < sizeof desk_icons / sizeof desk_icons[0]; i++) {
        lv_obj_t *b = th_box(scr, -1);
        th_add_to(b, TS_ROW_SEL, LV_STATE_PRESSED);
        lv_obj_set_size(b, 76, 72);
        lv_obj_set_pos(b, 8 + (int)(i / ICON_ROWS) * 82, 8 + (int)(i % ICON_ROWS) * 82);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        DeskIconObj *d = &dicons[i < 8 ? i : 7];
        d->box = b;
        d->tile = th_box(b, -1);                     /* (Retro Blue: behind the icon) */
        lv_obj_add_style(d->tile, &xp_tile, 0);
        lv_obj_clear_flag(d->tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(d->tile, 46, 46);
        lv_obj_align(d->tile, LV_ALIGN_TOP_MID, 0, 1);
        lv_obj_add_flag(d->tile, LV_OBJ_FLAG_HIDDEN);
        d->glyph = icon_img(b, desk_icons[i].icon, 32, TS_ICON);
        lv_obj_align(d->glyph, LV_ALIGN_TOP_MID, 0, 8);
        d->own = lv_img_create(b);                   /* (the owner's own icon, if there is one) */
        lv_obj_clear_flag(d->own, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_src(d->own, icon_get((NzIcon)desk_icons[i].icon, 32));
        lv_obj_align(d->own, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_add_flag(d->own, LV_OBJ_FLAG_HIDDEN);
        d->name = th_label(b, desk_icons[i].name, TS_SMALL);
        th_add(d->name, TS_ICON_TEXT);
        lv_obj_align(d->name, LV_ALIGN_BOTTOM_MID, 0, -5);
        lv_obj_add_event_cb(b, desk_icon_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)desk_icons[i].what);
    }

    /* taskbar */
    taskbar = th_box(scr, TS_BAR);
    lv_obj_add_flag(taskbar, LV_OBJ_FLAG_CLICKABLE);   /* (a click on it is not a click on the desktop) */
    lv_obj_set_size(taskbar, SCR_W, TASKBAR_H);
    lv_obj_set_pos(taskbar, 0, SCR_H - TASKBAR_H);

    start_btn = lv_btn_create(taskbar);
    th_add(start_btn, TS_BTN_ACCENT);
    lv_obj_set_size(start_btn, 92, 26);
    lv_obj_align(start_btn, LV_ALIGN_LEFT_MID, 4, 0);
    start_bold = lv_label_create(start_btn);         /* (Retro Blue only: makes "start" look bold) */
    lv_label_set_text(start_bold, "start");
    lv_obj_add_flag(start_bold, LV_OBJ_FLAG_HIDDEN);
    start_label = lv_label_create(start_btn);
    lv_label_set_text(start_label, "NZEUS");
    lv_obj_center(start_label);
    start_icon = lv_img_create(start_btn);           /* the owner's logo (nzeus/start-icon.bmp), if any */
    lv_obj_clear_flag(start_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(start_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(start_btn, start_clicked, LV_EVENT_CLICKED, NULL);

    task_buttons = th_box(taskbar, -1);              /* one button per open window */
    lv_obj_set_size(task_buttons, 300, TASKBAR_H);
    lv_obj_set_pos(task_buttons, 104, 0);
    lv_obj_set_flex_flow(task_buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(task_buttons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(task_buttons, 6, 0);

    tray_box = th_box(taskbar, -1);                  /* the tray, on the right */
    lv_obj_set_size(tray_box, LV_SIZE_CONTENT, TASKBAR_H);
    lv_obj_set_flex_flow(tray_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tray_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tray_box, 12, 0);
    lv_obj_align(tray_box, LV_ALIGN_RIGHT_MID, -10, 0);
    badge = th_label(tray_box, "", TS_KEYCAP);       /* e.g. "CLAUDE" */
    th_add(badge, TS_BTN_ACCENT);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    tray_label(TRAY_WIFI, LV_SYMBOL_WIFI);
    tray_label(TRAY_BT, LV_SYMBOL_BLUETOOTH);
    tray_label(TRAY_VOLUME, LV_SYMBOL_VOLUME_MAX);
    tray_label(TRAY_BATTERY, "");
    tray_label(TRAY_CLOCK, "--:--");

    build_start_menu();
    build_desk_menu();
    desktop_set_status("--:--", -1, 0);
    last_theme = theme_index();
    desktop_reload_own_icons();
    desktop_look();
    show_wallpaper(wall_saved(), 0);
    theme_on_change(desktop_theme_changed);
    return scr;
}

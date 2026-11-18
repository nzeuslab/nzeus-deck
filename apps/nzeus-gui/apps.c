/* apps.c - opens, closes and switches NZEUS's built-in apps.
 *
 * Handheld mode: an app gets the whole screen under a title bar. Apps form a
 * stack: opening a file in Files puts the Editor on top; closing the Editor
 * goes back to Files (and closing Files goes back to the home screen).
 * Desktop mode:  an app lives in a window (desktop.c).
 * An app's container (root) can move between the two: switching mode never
 * loses what you were doing. */
#include <stdio.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "ui.h"
#include "desktop.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"

#define HEADER_H  30
#define MAX_APPS  24
#define MAX_STACK 8

static NzApp *registry[MAX_APPS];
static int app_count;
static NzApp *focused;                 /* the app in front */
static NzApp *stack[MAX_STACK];        /* handheld apps, the last one is on screen */
static int depth;

void apps_register(NzApp *a) { if (app_count < MAX_APPS) registry[app_count++] = a; }

NzApp *apps_get(int id)
{
    for (int i = 0; i < app_count; i++) if (registry[i]->id == id) return registry[i];
    return NULL;
}

NzApp *apps_focused(void) { return focused; }
void apps_set_focused(NzApp *a) { focused = a; }
static NzApp *top(void) { return depth ? stack[depth - 1] : NULL; }

static void stack_remove(NzApp *a)
{
    for (int i = 0; i < depth; i++)
        if (stack[i] == a) { for (int j = i; j < depth - 1; j++) stack[j] = stack[j + 1]; depth--; return; }
}

static void stack_push(NzApp *a)
{
    stack_remove(a);
    if (depth == MAX_STACK) {          /* full: close the oldest one */
        NzApp *old = stack[0];
        apps_close(old);
    }
    stack[depth++] = a;
}

/* ---------- the D-pad driver for on-screen keyboards ----------
 * LVGL's keyboard widget understands "keypad" keys (arrows to move, ENTER to
 * press). Apps queue those here; LVGL reads them as a press then a release. */
static lv_group_t *kp_group;
static uint32_t kp_queue[16];
static int kp_head, kp_tail, kp_down;
static uint32_t kp_key;

static void kp_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (kp_down) {                                 /* second read: let go of the key */
        kp_down = 0;
        data->key = kp_key;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (kp_head != kp_tail) {                      /* first read: press the next key */
        kp_key = kp_queue[kp_tail];
        kp_tail = (kp_tail + 1) % 16;
        kp_down = 1;
        data->key = kp_key;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    data->key = kp_key;
    data->state = LV_INDEV_STATE_RELEASED;
}

void apps_keypad_send(uint32_t lv_key)
{
    int next = (kp_head + 1) % 16;
    if (next == kp_tail) return;                   /* queue full: drop it */
    kp_queue[kp_head] = lv_key;
    kp_head = next;
}

lv_group_t *apps_keypad_group(void) { return kp_group; }

/* ---------- hosts: a full-screen view, or a window ---------- */
static lv_obj_t *make_handheld_view(NzApp *a, lv_obj_t **body)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    th_add(scr, TS_SCREEN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = th_box(scr, TS_BAR);           /* title bar: icon, name, B = back */
    lv_obj_set_size(bar, 640, HEADER_H);
    lv_obj_t *ic = lv_img_create(bar);
    lv_img_set_src(ic, icon_get(a->icon, 20));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *t = th_label(bar, a->title, TS_TITLE);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 38, 0);
    static const char *const back[] = { "B", "Back" };
    lv_obj_t *h = nz_hints(bar, back, 1);
    lv_obj_align(h, LV_ALIGN_RIGHT_MID, -10, 0);

    *body = th_box(scr, -1);                       /* the app goes here */
    lv_obj_set_pos(*body, 0, HEADER_H);
    lv_obj_set_size(*body, 640, 480 - HEADER_H);
    lv_obj_clear_flag(*body, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

/* Put the app's root inside `parent`, creating the app the first time. */
static void place_root(NzApp *a, lv_obj_t *parent)
{
    if (!a->root) {
        a->root = lv_obj_create(parent);
        lv_obj_remove_style_all(a->root);
        lv_obj_set_size(a->root, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);
        a->create(a, a->root);
    } else {
        lv_obj_set_parent(a->root, parent);        /* move it, keeping its contents */
    }
}

/* ---------- open / close ---------- */
void app_terminal_run(const char *cmd);           /* app_terminal.c */

int apps_open(int id)
{
    NzApp *a = apps_get(id);
    if (!a) return 0;
    if (ui_mode() == UI_HANDHELD) {
        if (a->root && !a->in_window && a->host) {         /* already open full-screen: to the top */
            stack_push(a);
            lv_scr_load(a->host);
        } else {
            lv_obj_t *body, *scr = make_handheld_view(a, &body);
            int from_window = a->root && a->in_window;
            place_root(a, body);
            if (from_window) desktop_close_app_window(a);  /* it moved out of a window */
            a->host = scr;
            a->in_window = 0;
            stack_push(a);
            lv_scr_load(scr);
        }
    } else {
        if (a->root && a->in_window && a->host) { desktop_raise_app_window(a); focused = a; return 1; }
        lv_obj_t *old_view = (a->root && !a->in_window) ? a->host : NULL;
        lv_obj_t *content = desktop_open_app_window(a);    /* sets a->host to the window */
        if (!content) return 1;                            /* no room: desktop already said so */
        place_root(a, content);
        if (old_view) { stack_remove(a); lv_obj_del(old_view); }   /* it moved out of a full-screen view */
        a->in_window = 1;
    }
    focused = a;
    return 1;
}

void apps_close(NzApp *a)
{
    if (!a || !a->root) return;
    if (a->destroy) a->destroy(a);
    lv_obj_t *root = a->root;
    a->root = NULL;
    if (a->in_window) {
        lv_obj_del(root);
        desktop_close_app_window(a);
        a->host = NULL;
    } else {
        int was_top = top() == a;
        stack_remove(a);
        if (was_top && ui_mode() == UI_HANDHELD) {         /* leave its screen before deleting it */
            if (top()) lv_scr_load(top()->host);           /* back to the app below */
            else ui_show_handheld_home();                  /* or to the home screen */
        }
        lv_obj_del(root);
        lv_obj_del_async(a->host);
        a->host = NULL;
    }
    if (focused == a) focused = ui_mode() == UI_HANDHELD ? top() : NULL;
}

/* A new title for an open app (the Terminal: "Terminal 2 of 3"). `title` must
 * stay valid (a static buffer): a new view or window made later uses it too. */
void apps_set_title(NzApp *a, const char *title)
{
    a->title = title;
    if (!a->host) return;
    if (a->in_window) { desktop_set_app_title(a, title); return; }
    lv_obj_t *bar = lv_obj_get_child(a->host, 0);             /* make_handheld_view: bar = child 0, */
    lv_obj_t *t = bar ? lv_obj_get_child(bar, 1) : NULL;      /* its title = child 1 (after the icon) */
    if (t && lv_obj_check_type(t, &lv_label_class)) lv_label_set_text(t, title);
}

/* ---------- routing ---------- */
int apps_key(int key)
{
    NzApp *a = ui_mode() == UI_HANDHELD ? top() : focused;
    if (!a || !a->key) return 0;
    return a->key(a, key);
}

void apps_kbd(const KbEvent *ev)
{
    NzApp *a = ui_mode() == UI_HANDHELD ? top() : focused;
    if (a && a->kbd) a->kbd(a, ev);
}

void apps_tick(void)
{
    for (int i = 0; i < app_count; i++)
        if (registry[i]->root && registry[i]->tick) registry[i]->tick(registry[i]);
}

void apps_mode_changed(void)
{
    if (ui_mode() == UI_HANDHELD && top()) { lv_scr_load(top()->host); focused = top(); }
}

/* the built-in apps (one file each) */
extern NzApp app_terminal, app_files, app_editor, app_settings, app_python, app_images, app_wifi, app_media,
             app_arduino, app_car, app_claude, app_electronics, app_web,
             app_taskmgr, app_calculator, app_notes, app_clock, app_whatsapp, app_probe, app_devtools, app_health, app_btscan, app_update;
extern int (*files_open_audio_fn)(const char *path);   /* app_files.c */
int app_media_open(const char *path);                  /* app_media.c */
extern int (*files_open_image_fn)(const char *path);   /* app_files.c */
int app_images_open(const char *path);                 /* app_images.c */
extern int (*files_open_text_fn)(const char *path);   /* app_files.c */
int app_editor_open_file(const char *path);            /* app_editor.c */

void apps_init(void)
{
    kp_group = lv_group_create();
    static lv_indev_drv_t kd;
    lv_indev_drv_init(&kd);
    kd.type = LV_INDEV_TYPE_KEYPAD;
    kd.read_cb = kp_read;
    lv_indev_t *kp = lv_indev_drv_register(&kd);
    lv_indev_set_group(kp, kp_group);

    apps_register(&app_terminal);
    apps_register(&app_files);
    apps_register(&app_editor);
    apps_register(&app_settings);
    apps_register(&app_python);
    apps_register(&app_images);
    apps_register(&app_wifi);
    apps_register(&app_media);
    apps_register(&app_arduino);
    apps_register(&app_car);
    apps_register(&app_claude);
    apps_register(&app_electronics);
    apps_register(&app_web);
    apps_register(&app_taskmgr);
    apps_register(&app_calculator);
    apps_register(&app_notes);
    apps_register(&app_clock);
    apps_register(&app_whatsapp);
    apps_register(&app_probe);
    apps_register(&app_devtools);
    apps_register(&app_health);
    apps_register(&app_btscan);
    apps_register(&app_update);
    files_open_audio_fn = app_media_open;
    files_open_image_fn = app_images_open;
    files_open_text_fn = app_editor_open_file;       /* text files open in the Editor */
}

/* app_files.c - the NZEUS Files app (replaces FrogShell's file manager).
 *
 * Buttons (handheld mode):
 *   D-pad up/down  move          L1 / R1  page up / down
 *   A              open (folder, text file, script)
 *   B              up one folder (at the top: back to the home screen)
 *   X or START     the actions menu: copy, cut, paste, rename, delete,
 *                  new folder, new file, info, USB drive, Terminal here
 *   Y              mark / unmark (copy, cut and delete work on all marked)
 * Mouse: click a row to select it, click it again to open it; the buttons in
 * the path bar go up a folder and open the actions menu.
 * Only the SD card and USB drives are shown, so system folders stay safe. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "fsops.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

#define ROW_H        30
#define MAX_ROWS     16
#define MAX_ENTRIES 512
#define MAX_MARKED   64

typedef struct { char name[256]; unsigned char dir; long long size; time_t mtime; } Entry;

static struct {
    char cwd[FS_PATH_MAX];
    Entry *e;
    int count, selected, top, rows_visible, last_height;
    lv_obj_t *path, *list, *row[MAX_ROWS], *icon[MAX_ROWS], *name[MAX_ROWS], *info[MAX_ROWS], *empty;
} F;

/* The marked items and the copy/cut "clipboard": each path is kept at its own
 * length (strdup), not in a 1 KB slot - 64 slots each would cost 128 KB of RAM
 * all the time. They stay while NZEUS runs (copy, close Files, open it, paste). */
static char *marked[MAX_MARKED];
static int marked_count;
static char *clip[MAX_MARKED];
static int clip_count, clip_cut;

static void list_clear(char **list, int *count)
{
    for (int i = 0; i < *count; i++) { free(list[i]); list[i] = NULL; }
    *count = 0;
}

/* The Editor, Images and Media apps set these so their files open in them. */
int (*files_open_text_fn)(const char *path);
int (*files_open_image_fn)(const char *path);
int (*files_open_audio_fn)(const char *path);

void app_terminal_run(const char *cmd);          /* app_terminal.c */
void app_terminal_cd(const char *dir);
int app_images_open_pdf(const char *path);

/* ---------- reading folders ---------- */
static int entry_cmp(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (!strcmp(x->name, "..")) return -1;       /* ".." always first */
    if (!strcmp(y->name, "..")) return 1;
    if (x->dir != y->dir) return y->dir - x->dir; /* then folders */
    return strcasecmp(x->name, y->name);
}

static int at_top(void) { return !strcmp(F.cwd, NZ_SDCARD); }

static void scan(void)
{
    F.count = 0;
    if (!at_top()) { strcpy(F.e[0].name, ".."); F.e[0].dir = 1; F.e[0].size = 0; F.count = 1; }
    DIR *d = opendir(F.cwd);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && F.count < MAX_ENTRIES) {
            if (de->d_name[0] == '.') continue;  /* hidden files stay hidden */
            char p[FS_PATH_MAX];
            struct stat st;
            fs_join(p, sizeof p, F.cwd, de->d_name);
            if (stat(p, &st) != 0) continue;
            Entry *e = &F.e[F.count++];
            snprintf(e->name, sizeof e->name, "%s", de->d_name);
            e->dir = S_ISDIR(st.st_mode);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        }
        closedir(d);
    }
    qsort(F.e, F.count, sizeof *F.e, entry_cmp);
    if (F.selected >= F.count) F.selected = F.count ? F.count - 1 : 0;
    if (F.selected < 0) F.selected = 0;
}

static void selected_path(char *out, size_t n) { fs_join(out, n, F.cwd, F.e[F.selected].name); }
static int is_marked(const char *p) { for (int i = 0; i < marked_count; i++) if (!strcmp(marked[i], p)) return 1; return 0; }

/* ---------- drawing ---------- */
static void render(void)
{
    char shown[FS_PATH_MAX * 2];                 /* friendlier folder names */
    if (fs_path_contains(NZ_SDCARD, F.cwd)) snprintf(shown, sizeof shown, "SD card%s", F.cwd + strlen(NZ_SDCARD));
    else if (fs_is_usb(F.cwd)) snprintf(shown, sizeof shown, "USB drive%s", strchr(F.cwd + 7, '/') ? strchr(F.cwd + 7, '/') : "");
    else snprintf(shown, sizeof shown, "%s", F.cwd);
    lv_label_set_text(F.path, shown);

    F.last_height = lv_obj_get_content_height(F.list);
    F.rows_visible = F.last_height / ROW_H;
    if (F.rows_visible > MAX_ROWS) F.rows_visible = MAX_ROWS;
    if (F.rows_visible < 1) F.rows_visible = 1;
    if (F.selected < F.top) F.top = F.selected;                         /* keep the selection visible */
    if (F.selected >= F.top + F.rows_visible) F.top = F.selected - F.rows_visible + 1;
    if (F.top < 0) F.top = 0;

    for (int i = 0; i < MAX_ROWS; i++) {
        int idx = F.top + i;
        if (i >= F.rows_visible || idx >= F.count) { lv_obj_add_flag(F.row[i], LV_OBJ_FLAG_HIDDEN); continue; }
        Entry *e = &F.e[idx];
        char p[FS_PATH_MAX], info[48] = "";
        fs_join(p, sizeof p, F.cwd, e->name);
        lv_obj_clear_flag(F.row[i], LV_OBJ_FLAG_HIDDEN);
        if (idx == F.selected) lv_obj_add_state(F.row[i], LV_STATE_CHECKED);
        else lv_obj_clear_state(F.row[i], LV_STATE_CHECKED);
        lv_img_set_src(F.icon[i], icon_get((NzIcon)fs_file_icon(e->name, e->dir), 20));
        lv_label_set_text_fmt(F.name[i], "%s%s", is_marked(p) ? "* " : "", !strcmp(e->name, "..") ? ".. (up)" : e->name);
        if (is_marked(p)) lv_obj_add_state(F.name[i], LV_STATE_USER_1);   /* marked: accent colour */
        else lv_obj_clear_state(F.name[i], LV_STATE_USER_1);
        if (!e->dir) fs_size_label((unsigned long long)e->size, info, sizeof info);
        lv_label_set_text(F.info[i], info);
    }
    if (F.count == 0) lv_obj_clear_flag(F.empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(F.empty, LV_OBJ_FLAG_HIDDEN);
}

static void refresh(void) { scan(); render(); }

/* ---------- moving around ---------- */
static void go_up(void)
{
    if (at_top()) return;
    char *slash = strrchr(F.cwd, '/');
    if (fs_is_usb(F.cwd) && slash == F.cwd + 6) snprintf(F.cwd, sizeof F.cwd, "%s", NZ_SDCARD);  /* USB top -> SD card */
    else if (slash && slash != F.cwd) *slash = 0;
    if (!fs_is_allowed(F.cwd)) snprintf(F.cwd, sizeof F.cwd, "%s", NZ_SDCARD);
    F.selected = 0;
    F.top = 0;
    refresh();
}

static void enter(const char *dir)
{
    snprintf(F.cwd, sizeof F.cwd, "%s", dir);
    F.selected = 0;
    F.top = 0;
    refresh();
}

static void open_selected(void)
{
    if (F.count == 0) return;
    Entry *e = &F.e[F.selected];
    if (!strcmp(e->name, "..")) { go_up(); return; }
    char p[FS_PATH_MAX];
    selected_path(p, sizeof p);
    if (e->dir) { enter(p); return; }
    const char *dot = strrchr(e->name, '.');
    if (dot && !strcasecmp(dot, ".pdf")) { app_images_open_pdf(p); return; }
    static char cmd[FS_PATH_MAX + 32];
    if (dot && !strcasecmp(dot, ".sh")) { snprintf(cmd, sizeof cmd, "sh \"%s\"", p); app_terminal_run(cmd); return; }
    if (dot && !strcasecmp(dot, ".py")) { snprintf(cmd, sizeof cmd, "python \"%s\"", p); app_terminal_run(cmd); return; }
    if (fs_is_image_file(e->name) && files_open_image_fn) { files_open_image_fn(p); return; }
    if (fs_is_audio_file(e->name) && files_open_audio_fn) { files_open_audio_fn(p); return; }
    if (fs_is_text_file(e->name)) {
        if (files_open_text_fn && files_open_text_fn(p)) return;
        ui_toast("Could not open the Editor");
        return;
    }
    char size[32];
    fs_size_label((unsigned long long)e->size, size, sizeof size);
    char msg[320];
    snprintf(msg, sizeof msg, "%.200s  (%s)", e->name, size);
    ui_toast(msg);
}

/* ---------- marking, copy, paste, delete ---------- */
static void toggle_mark(void)
{
    if (F.count == 0 || !strcmp(F.e[F.selected].name, "..")) return;
    char p[FS_PATH_MAX];
    selected_path(p, sizeof p);
    for (int i = 0; i < marked_count; i++)
        if (!strcmp(marked[i], p)) {                         /* marked already: unmark it */
            free(marked[i]);
            memmove(&marked[i], &marked[i + 1], (size_t)(marked_count - i - 1) * sizeof marked[0]);
            marked[--marked_count] = NULL;
            render();
            return;
        }
    if (marked_count < MAX_MARKED && (marked[marked_count] = strdup(p)) != NULL) marked_count++;
    render();
}

/* the marked items, or else the selected one: copies (strdup) the caller frees */
static int targets(char **out, int cap)
{
    int n = 0;
    if (marked_count) {
        for (int i = 0; i < marked_count && n < cap; i++) if ((out[n] = strdup(marked[i])) != NULL) n++;
        return n;
    }
    if (F.count == 0 || !strcmp(F.e[F.selected].name, "..")) return 0;
    char p[FS_PATH_MAX];
    selected_path(p, sizeof p);
    return (out[0] = strdup(p)) != NULL;
}

static void copy_or_cut(int cut)
{
    list_clear(clip, &clip_count);
    clip_count = targets(clip, MAX_MARKED);
    clip_cut = cut;
    list_clear(marked, &marked_count);
    char msg[64];
    snprintf(msg, sizeof msg, clip_count ? "%s %d item%s" : "Nothing selected", cut ? "Cut" : "Copied", clip_count, clip_count == 1 ? "" : "s");
    ui_toast(msg);
    render();
}

static void paste(void)
{
    if (!clip_count) { ui_toast("Copy or cut something first"); return; }
    int ok = 0, failed = 0;
    for (int i = 0; i < clip_count; i++) {
        const char *src = clip[i], *base = strrchr(src, '/');
        char dst[FS_PATH_MAX], final_dst[FS_PATH_MAX];
        fs_join(dst, sizeof dst, F.cwd, base ? base + 1 : src);
        if (fs_path_contains(src, F.cwd)) { failed++; continue; }     /* a folder into itself */
        if (clip_cut && !strcmp(src, dst)) { ok++; continue; }        /* already here */
        fs_unique_name(dst, final_dst, sizeof final_dst);
        int rc;
        if (clip_cut) {
            rc = rename(src, final_dst);                               /* fast on the same drive */
            if (rc != 0) { rc = fs_copy_tree(src, final_dst); if (rc == 0) rc = fs_remove_tree(src); }
        } else {
            rc = fs_copy_tree(src, final_dst);
        }
        if (rc == 0) ok++; else failed++;
    }
    sync();
    if (clip_cut) list_clear(clip, &clip_count);   /* a cut can only be pasted once */
    char msg[64];
    if (failed) snprintf(msg, sizeof msg, "Pasted %d, %d failed", ok, failed);
    else snprintf(msg, sizeof msg, "Pasted %d item%s", ok, ok == 1 ? "" : "s");
    ui_toast(msg);
    refresh();
}

static void delete_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!yes) return;
    char *list[MAX_MARKED];
    int n = targets(list, MAX_MARKED), failed = 0;
    for (int i = 0; i < n; i++) if (!fs_is_allowed(list[i]) || fs_remove_tree(list[i]) != 0) failed++;
    sync();
    list_clear(list, &n);
    list_clear(marked, &marked_count);
    ui_toast(failed ? "Some items could not be deleted" : "Deleted");
    refresh();
}

static void ask_delete(void)
{
    char *list[MAX_MARKED];
    int n = targets(list, MAX_MARKED);
    if (!n) { ui_toast("Nothing selected"); return; }
    static char q[96];
    if (n == 1) { const char *b = strrchr(list[0], '/'); snprintf(q, sizeof q, "Delete \"%.60s\"?", b ? b + 1 : list[0]); }
    else snprintf(q, sizeof q, "Delete %d items?", n);
    list_clear(list, &n);
    nz_confirm(q, delete_answer, NULL);
}

/* ---------- names: rename, new folder, new file ---------- */
static int good_name(const char *name) { return name && name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, ".."); }

static void rename_done(const char *name, void *ctx)
{
    (void)ctx;
    if (!name) return;
    if (!good_name(name)) { ui_toast("That name can't be used"); return; }
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    selected_path(from, sizeof from);
    fs_join(to, sizeof to, F.cwd, name);
    if (access(to, F_OK) == 0) { ui_toast("That name already exists"); return; }
    if (rename(from, to) != 0) ui_toast("Rename failed");
    sync();
    refresh();
}

static void new_folder_done(const char *name, void *ctx)
{
    (void)ctx;
    if (!name) return;
    if (!good_name(name)) { ui_toast("That name can't be used"); return; }
    char p[FS_PATH_MAX];
    fs_join(p, sizeof p, F.cwd, name);
    if (mkdir(p, 0777) != 0) ui_toast("Could not create the folder");
    sync();
    refresh();
}

static void new_file_done(const char *name, void *ctx)
{
    (void)ctx;
    if (!name) return;
    if (!good_name(name)) { ui_toast("That name can't be used"); return; }
    char p[FS_PATH_MAX];
    fs_join(p, sizeof p, F.cwd, name);
    if (access(p, F_OK) == 0) { ui_toast("That name already exists"); return; }
    FILE *f = fopen(p, "w");
    if (!f) { ui_toast("Could not create the file"); return; }
    fclose(f);
    sync();
    refresh();
    if (files_open_text_fn) files_open_text_fn(p);
}

/* ---------- the actions menu ---------- */
enum { ACT_OPEN, ACT_COPY, ACT_CUT, ACT_PASTE, ACT_RENAME, ACT_DELETE, ACT_NEW_FOLDER,
       ACT_NEW_FILE, ACT_INFO, ACT_USB, ACT_TERMINAL, ACT_EDIT, ACT_COUNT };
static const char *const act_names[ACT_COUNT] = {
    "Open", "Copy", "Cut", "Paste here", "Rename", "Delete", "New folder",
    "New file", "Info", "USB drive", "Terminal here", "Open in the Editor",
};

static void info_closed(int choice, void *ctx) { (void)choice; (void)ctx; }

static void action(int choice, void *ctx)
{
    (void)ctx;
    int has_item = F.count && strcmp(F.e[F.selected].name, "..");
    switch (choice) {
    case ACT_OPEN:       open_selected(); break;
    case ACT_COPY:       copy_or_cut(0); break;
    case ACT_CUT:        copy_or_cut(1); break;
    case ACT_PASTE:      paste(); break;
    case ACT_RENAME:     if (has_item) nz_prompt("Rename to", F.e[F.selected].name, rename_done, NULL); break;
    case ACT_DELETE:     ask_delete(); break;
    case ACT_NEW_FOLDER: nz_prompt("New folder name", "", new_folder_done, NULL); break;
    case ACT_NEW_FILE:   nz_prompt("New file name (e.g. hello.py)", "", new_file_done, NULL); break;
    case ACT_INFO:
        if (has_item) {
            char p[FS_PATH_MAX], size[32], when[32];
            static char text[400];
            selected_path(p, sizeof p);
            fs_size_label(fs_tree_size(p), size, sizeof size);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&F.e[F.selected].mtime));
            snprintf(text, sizeof text, "%.200s\n%s, %s\nchanged %s", F.e[F.selected].name,
                     F.e[F.selected].dir ? "folder" : "file", size, when);
            static const char *const ok[] = { "OK" };
            nz_menu(text, ok, 1, info_closed, NULL);
        }
        break;
    case ACT_USB: {
        char mp[FS_PATH_MAX];
        if (fs_usb_mount_point(mp, sizeof mp)) enter(mp);
        else ui_toast("No USB drive found");
        break;
    }
    case ACT_TERMINAL: app_terminal_cd(F.cwd); break;
    case ACT_EDIT:                                     /* any file as text (e.g. a .py to change it) */
        if (has_item && !F.e[F.selected].dir && files_open_text_fn) {
            char p[FS_PATH_MAX];
            selected_path(p, sizeof p);
            files_open_text_fn(p);
        }
        break;
    default: break;
    }
}

static void open_actions(void) { nz_menu("Files", act_names, ACT_COUNT, action, NULL); }

/* ---------- mouse ---------- */
static void row_clicked(lv_event_t *e)
{
    int idx = F.top + (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= F.count) return;
    if (idx == F.selected) open_selected();      /* second click opens */
    else { F.selected = idx; render(); }
}
static void up_clicked(lv_event_t *e) { (void)e; go_up(); }
static void menu_clicked(lv_event_t *e) { (void)e; open_actions(); }

/* ---------- the app ---------- */
static lv_obj_t *small_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 36, 26);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    if (!F.e) F.e = calloc(MAX_ENTRIES, sizeof *F.e);
    if (!F.cwd[0]) snprintf(F.cwd, sizeof F.cwd, "%s", NZ_SDCARD);
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bar = th_box(root, TS_PANEL);        /* path bar: up, folder name, menu */
    lv_obj_set_size(bar, LV_PCT(100), 36);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    small_button(bar, LV_SYMBOL_UP, up_clicked);
    lv_obj_t *drive = lv_img_create(bar);
    lv_img_set_src(drive, icon_get(ICON_FOLDER, 20));
    th_add(drive, TS_ICON);
    F.path = th_label(bar, "", TS_TEXT);
    lv_obj_set_flex_grow(F.path, 1);
    lv_label_set_long_mode(F.path, LV_LABEL_LONG_DOT);
    small_button(bar, LV_SYMBOL_LIST, menu_clicked);

    F.list = th_box(root, -1);                     /* the rows */
    lv_obj_set_width(F.list, LV_PCT(100));
    lv_obj_set_flex_grow(F.list, 1);
    lv_obj_set_style_pad_hor(F.list, 4, 0);
    lv_obj_set_style_pad_top(F.list, 4, 0);
    for (int i = 0; i < MAX_ROWS; i++) {
        lv_obj_t *r = F.row[i] = th_box(F.list, TS_TEXT);
        th_add_to(r, TS_ROW_SEL, LV_STATE_CHECKED);
        lv_obj_set_size(r, LV_PCT(100), ROW_H);
        lv_obj_set_pos(r, 0, i * ROW_H);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        F.icon[i] = lv_img_create(r);
        th_add(F.icon[i], TS_ICON);
        lv_obj_align(F.icon[i], LV_ALIGN_LEFT_MID, 10, 0);
        F.name[i] = lv_label_create(r);                 /* (text colour from the row) */
        th_add_to(F.name[i], TS_TITLE, LV_STATE_USER_1);
        lv_obj_set_width(F.name[i], LV_PCT(62));
        lv_label_set_long_mode(F.name[i], LV_LABEL_LONG_DOT);
        lv_obj_align(F.name[i], LV_ALIGN_LEFT_MID, 40, 0);
        F.info[i] = th_label(r, "", TS_SMALL);
        lv_obj_align(F.info[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
    F.empty = th_label(F.list, "This folder is empty.  X opens the menu.", TS_MUTED);
    lv_obj_center(F.empty);

    lv_obj_t *hint = th_box(root, TS_BAR);         /* button help */
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Open", "B", "Up", "X", "Menu", "Y", "Mark", "L1/R1", "Page" };
    lv_obj_center(nz_hints(hint, hints, 5));

    lv_obj_update_layout(root);
    refresh();
}

static void tick(NzApp *a)
{
    (void)a;
    if (lv_obj_get_content_height(F.list) != F.last_height) render();   /* moved to/from a window */
}

static int key(NzApp *a, int k)
{
    (void)a;
    int page = F.rows_visible > 1 ? F.rows_visible - 1 : 1;
    switch (k) {
    case NZ_KEY_UP:    if (F.selected > 0) { F.selected--; render(); } return 1;
    case NZ_KEY_DOWN:  if (F.selected < F.count - 1) { F.selected++; render(); } return 1;
    case NZ_KEY_L:     F.selected = F.selected > page ? F.selected - page : 0; render(); return 1;
    case NZ_KEY_R:     F.selected = F.selected + page < F.count ? F.selected + page : (F.count ? F.count - 1 : 0); render(); return 1;
    case NZ_KEY_A:     open_selected(); return 1;
    case NZ_KEY_B:     if (at_top()) return 0; go_up(); return 1;     /* at the top: leave the app */
    case NZ_KEY_X:
    case NZ_KEY_START: open_actions(); return 1;
    case NZ_KEY_Y:     toggle_mark(); return 1;
    default:           return 0;
    }
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    switch (ev->key) {
    case KB_UP:        key(a, NZ_KEY_UP); break;
    case KB_DOWN:      key(a, NZ_KEY_DOWN); break;
    case KB_PGUP:      key(a, NZ_KEY_L); break;
    case KB_PGDN:      key(a, NZ_KEY_R); break;
    case KB_ENTER:     open_selected(); break;
    case KB_BACKSPACE: go_up(); break;
    case KB_DELETE:    ask_delete(); break;
    case KB_TAB:       open_actions(); break;
    default: break;
    }
}

static void destroy(NzApp *a) { (void)a; }   /* keep the folder and clipboard for next time */

/* for tests */
const char *app_files_cwd(void) { return F.cwd; }
const char *app_files_selected_name(void) { return F.count ? F.e[F.selected].name : ""; }

NzApp app_files = {
    .icon = ICON_FILES,
    .id = APP_FILES, .title = "Files",
    .create = create, .key = key, .kbd = kbd, .tick = tick, .destroy = destroy,
};

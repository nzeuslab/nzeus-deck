/* app_python.c - the NZEUS Python app.
 *
 * A start screen for Python 3 (Debian's python3): start the >>> prompt, run
 * an example, or run one of your own scripts. Everything runs in the Terminal
 * app (the `python` helper in nzeus/bin, linux-port/deck-modern/python).
 *   A  run            X  open in the Editor            B  back
 * Examples: /usr/local/lib/nzeus/python-examples    Your scripts: nzeus/python/ */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

/* the examples are part of the system (the SD Debian image); tests change it */
const char *python_examples_dir = "/usr/local/lib/nzeus/python-examples";
#define PY_TITLE     "Python 3"
#define PY_WHERE     "Your own scripts go in  nzeus / python  (Files app)"
#define SCRIPTS_DIR  NZ_DIR "/python"
#define MAX_FILES    64

void app_terminal_run(const char *cmd);        /* app_terminal.c */
int  app_editor_open_file(const char *path);   /* app_editor.c */

static struct {
    NzList *list;
    char (*paths)[256];            /* row i -> file (row 0 = the >>> prompt); borrowed while open (16 KB) */
    int cap;                       /* how many fit (0 = no memory for the list) */
    int count;
} P;
static char command[300];          /* the command handed to the Terminal */

/* "# hello.py - your first program" -> "your first program" */
static void describe(const char *path, char *out, size_t n)
{
    char line[160] = "";
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(line, sizeof line, f) && line[0] == '#') {
        line[strcspn(line, "\r\n")] = 0;
        const char *d = strstr(line, " - ");
        snprintf(out, n, "%s", d ? d + 3 : line + 1);
    }
    fclose(f);
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* every .py file in a folder, A-Z */
static void add_folder(const char *dir, int icon)
{
    char names[MAX_FILES][128];
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_FILES) {
        size_t len = strlen(e->d_name);
        if (len > 3 && !strcmp(e->d_name + len - 3, ".py")) snprintf(names[n++], 128, "%s", e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], name_cmp);
    for (int i = 0; i < n && P.count < P.cap; i++) {
        char desc[160];
        if (snprintf(P.paths[P.count], sizeof P.paths[0], "%s/%s", dir, names[i]) >= (int)sizeof P.paths[0]) continue;
        describe(P.paths[P.count], desc, sizeof desc);
        nzlist_add(P.list, icon, names[i], desc[0] ? desc : NULL, NULL);
        P.count++;
    }
}

static void run_row(int i, void *ctx)
{
    (void)ctx;
    if (i == 0) { app_terminal_run("python"); return; }
    snprintf(command, sizeof command, "python %s", P.paths[i]);
    app_terminal_run(command);
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&P, 0, sizeof P);
    P.paths = calloc(MAX_FILES, sizeof *P.paths);
    P.cap = P.paths ? MAX_FILES : 0;
    if (!P.paths) ui_toast("Not enough memory to list the scripts");
    mkdir(SCRIPTS_DIR, 0755);                        /* a home for your own scripts */
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = th_box(root, TS_PANEL);         /* what this is */
    lv_obj_set_size(card, LV_PCT(100), 64);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_PYTHON, 48));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = th_label(card, PY_TITLE, TS_HEADING);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 72, 8);
    lv_obj_t *s = th_label(card, PY_WHERE, TS_SMALL);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 72, -9);

    P.list = nzlist_create(root, 0);
    lv_obj_set_width(nzlist_obj(P.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(P.list), 1);
    nzlist_on_activate(P.list, run_row, NULL);
    nzlist_add(P.list, ICON_TERMINAL, "Start Python", "the >>> prompt: type Python, see the answer", NULL);
    P.count = 1;
    add_folder(SCRIPTS_DIR, ICON_FILE_CODE);
    add_folder(python_examples_dir, ICON_PYTHON);

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Run", "X", "Edit", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 3));
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (k == NZ_KEY_X) {                             /* open the script in the Editor */
        int i = nzlist_selected(P.list);
        if (i > 0) app_editor_open_file(P.paths[i]);
        else ui_toast("Pick a script first");
        return 1;
    }
    return nzlist_key(P.list, k);
}

static void destroy(NzApp *a)
{
    (void)a;
    nzlist_free(P.list);
    free(P.paths);                 /* the list's memory goes back */
    memset(&P, 0, sizeof P);
}

/* for tests */
int app_python_count(void) { return P.count; }

NzApp app_python = {
    .id = APP_PYTHON, .title = "Python", .icon = ICON_PYTHON,
    .create = create, .key = key, .destroy = destroy,
};

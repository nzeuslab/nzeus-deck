/* app_notes.c - the NZEUS Notes app.
 *
 * Each note is a small text file in  SD card / nzeus / notes. Its first line
 * is its title. The list shows the newest notes first.
 *   A   open the note in the Editor        X   new note (asks for a title)
 *   Y   delete the note (asks first)       B   back
 * Saving happens in the Editor. When you come back, the list is up to date:
 * a timer looks at the folder every second and re-reads it if a file changed. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define NOTES_DIR  NZ_DIR "/notes"
#define MAX_NOTES  128
#define FIRST_NOTE 1                       /* row 0 is "New note" */

int app_editor_open_file(const char *path);   /* app_editor.c */

typedef struct {
    char path[256];
    char title[64];
    char words[96];                       /* the start of the text after the title */
    time_t mtime;
} Note;

static struct {
    lv_obj_t *count_label;
    NzList *list;
    Note *notes;                          /* MAX_NOTES of them (malloc) */
    int count;
    long long signature;                  /* changes when any note file changes */
    int theme_shown, width_shown;         /* what the rows were fitted to */
    lv_timer_t *timer;
} N;

/* ---------- file names (used by tests too) ---------- */

/* "Shopping list!" written on 4 Oct 2026 -> "shopping-list-2026-10-04"
 * Only a-z, 0-9 and "-": safe on every SD card, and no "../" tricks. */
void notes_safe_name(const char *title, time_t when, char *out, size_t n)
{
    char base[40];
    size_t o = 0;
    int dash = 0;
    for (const char *s = title ? title : ""; *s && o < 32; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && o) base[o++] = '-';      /* one "-" for any run of other characters */
            base[o++] = c;
            dash = 0;
        } else dash = 1;
    }
    base[o] = 0;
    char date[16];
    struct tm tm;
    localtime_r(&when, &tm);
    strftime(date, sizeof date, "%Y-%m-%d", &tm);
    snprintf(out, n, "%s-%s", o ? base : "note", date);
}

/* a full path for a new note that doesn't exist yet: .../name.txt, .../name-2.txt ... */
int notes_new_path(const char *dir, const char *title, time_t when, char *out, size_t n)
{
    char name[64];
    notes_safe_name(title, when, name, sizeof name);
    for (int i = 1; i < 100; i++) {
        if (i == 1) snprintf(out, n, "%s/%s.txt", dir, name);
        else snprintf(out, n, "%s/%s-%d.txt", dir, name, i);
        if (access(out, F_OK) != 0) return 0;
    }
    return -1;
}

/* ---------- reading the folder ---------- */
static int is_note_file(const char *name)
{
    size_t len = strlen(name);
    if (name[0] == '.') return 0;
    return (len > 4 && !strcmp(name + len - 4, ".txt")) || (len > 3 && !strcmp(name + len - 3, ".md"));
}

/* a number that changes whenever a note is added, removed or saved */
static long long folder_signature(void)
{
    long long sig = 0;
    DIR *d = opendir(NOTES_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_note_file(e->d_name)) continue;
        char p[512];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", NOTES_DIR, e->d_name);
        if (stat(p, &st) != 0) continue;
        sig += 1000003LL + (long long)st.st_mtime * 31 + (long long)st.st_size;
        for (const char *c = e->d_name; *c; c++) sig += *c;     /* a rename changes it too */
    }
    closedir(d);
    return sig;
}

static int newest_first(const void *a, const void *b)
{
    const Note *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(x->path, y->path);
}

/* the title (first line) and the start of the rest, from the top of the file */
static void peek_note(const char *path, char *title, size_t tn, char *words, size_t wn)
{
    char buf[400];
    title[0] = words[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    char *nl = strchr(buf, '\n');
    char *rest = nl ? nl + 1 : buf + len;
    if (nl) *nl = 0;
    char *t = buf;
    while (*t == ' ' || *t == '\t' || *t == '#') t++;            /* "# Title" (Markdown) -> "Title" */
    t[strcspn(t, "\r")] = 0;
    size_t tl = strlen(t);                                       /* (cut to fit the title's space) */
    if (tl >= tn) tl = tn - 1;
    memcpy(title, t, tl);
    title[tl] = 0;
    size_t o = 0;                                                /* the rest on one line */
    for (char *s = rest; *s && o + 1 < wn; s++) {
        char c = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        if (c == ' ' && (o == 0 || words[o - 1] == ' ')) continue;  /* no double spaces */
        words[o++] = c;
    }
    words[o] = 0;
}

/* Cut `text` so it fits in `width` pixels with this font, ending in "...".
 * (A list row has room for one line; a longer one would spill over the next row.) */
static void fit(char *text, const lv_font_t *font, lv_coord_t width)
{
    size_t len = strlen(text);
    if (lv_txt_get_width(text, (uint32_t)len, font, 0, LV_TEXT_FLAG_NONE) <= width) return;
    lv_coord_t dots = lv_txt_get_width("...", 3, font, 0, LV_TEXT_FLAG_NONE);
    while (len > 0 && lv_txt_get_width(text, (uint32_t)len, font, 0, LV_TEXT_FLAG_NONE) + dots > width) {
        len--;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) len--;   /* not in the middle of a letter */
    }
    while (len > 0 && text[len - 1] == ' ') len--;
    memcpy(text + len, "...", 4);
}

/* "Today 14:32", "Yesterday 09:10", "3 Oct 18:05", "25 Aug 2025" */
static void when_text(time_t t, char *b, size_t n)
{
    time_t now = time(NULL), yest = now - 86400;
    struct tm tm, today, yesterday;
    localtime_r(&t, &tm);
    localtime_r(&now, &today);
    localtime_r(&yest, &yesterday);
    char hm[8];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday) snprintf(b, n, "Today %s", hm);
    else if (tm.tm_year == yesterday.tm_year && tm.tm_yday == yesterday.tm_yday) snprintf(b, n, "Yesterday %s", hm);
    else {
        char mon[8];
        strftime(mon, sizeof mon, "%b", &tm);
        if (tm.tm_year == today.tm_year) snprintf(b, n, "%d %s %s", tm.tm_mday, mon, hm);
        else snprintf(b, n, "%d %s %d", tm.tm_mday, mon, tm.tm_year + 1900);
    }
}

/* the list rows, fitted to the list's width and the theme's fonts.
 * keep: the note to select (NULL = the one selected now) */
static void fill_rows(const char *keep_path)
{
    char keep[256] = "";
    int sel = nzlist_selected(N.list);
    if (keep_path) snprintf(keep, sizeof keep, "%s", keep_path);
    else if (sel >= FIRST_NOTE && sel - FIRST_NOTE < N.count) snprintf(keep, sizeof keep, "%s", N.notes[sel - FIRST_NOTE].path);

    lv_obj_update_layout(nzlist_obj(N.list));
    lv_coord_t w = lv_obj_get_content_width(nzlist_obj(N.list)) - 72;   /* minus padding, icon, gaps */
    if (w < 80) w = 80;
    const NzTheme *t = theme();
    nzlist_clear(N.list);
    nzlist_add(N.list, ICON_NOTES, "New note", "X also makes one", NULL);
    int keep_row = -1;
    for (int i = 0; i < N.count; i++) {
        Note *nt = &N.notes[i];
        char title[64], sub[160], when[32];
        when_text(nt->mtime, when, sizeof when);
        snprintf(title, sizeof title, "%s", nt->title);
        snprintf(sub, sizeof sub, "%s   %s", when, nt->words[0] ? nt->words : "(empty)");
        fit(title, t->font_body, w);
        fit(sub, t->font_small, w);
        nzlist_add(N.list, ICON_FILE_TEXT, title, sub, NULL);
        if (keep[0] && !strcmp(keep, nt->path)) keep_row = i + FIRST_NOTE;
    }
    if (keep_row >= 0) nzlist_select(N.list, keep_row);
    else if (sel > 0) nzlist_select(N.list, sel);
    else if (N.count) nzlist_select(N.list, FIRST_NOTE);       /* the newest note */
    N.theme_shown = theme_index();
    N.width_shown = lv_obj_get_width(nzlist_obj(N.list));
}

/* read the folder again, then show it */
static void reload(void)
{
    char keep[256] = "";
    int sel = nzlist_selected(N.list);
    if (sel >= FIRST_NOTE && sel - FIRST_NOTE < N.count) snprintf(keep, sizeof keep, "%s", N.notes[sel - FIRST_NOTE].path);
    N.count = 0;
    DIR *d = opendir(NOTES_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && N.count < MAX_NOTES) {
            if (!is_note_file(e->d_name)) continue;
            Note *nt = &N.notes[N.count];
            struct stat st;
            if (snprintf(nt->path, sizeof nt->path, "%s/%s", NOTES_DIR, e->d_name) >= (int)sizeof nt->path) continue;
            if (stat(nt->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            nt->mtime = st.st_mtime;
            peek_note(nt->path, nt->title, sizeof nt->title, nt->words, sizeof nt->words);
            if (!nt->title[0]) snprintf(nt->title, sizeof nt->title, "(no title)");
            N.count++;
        }
        closedir(d);
    }
    qsort(N.notes, (size_t)N.count, sizeof N.notes[0], newest_first);
    fill_rows(keep);                                           /* the same note stays selected */

    if (N.count) lv_label_set_text_fmt(N.count_label, "%d note%s", N.count, N.count == 1 ? "" : "s");
    else lv_label_set_text(N.count_label, "No notes yet");
    N.signature = folder_signature();
}

static void watch(lv_timer_t *t)
{
    (void)t;
    if (folder_signature() != N.signature) reload();             /* saved in the Editor, or deleted */
    else if (theme_index() != N.theme_shown || lv_obj_get_width(nzlist_obj(N.list)) != N.width_shown)
        fill_rows(NULL);                                         /* new fonts, or moved into a window */
}

/* ---------- new / open / delete ---------- */
static void open_note(const char *path)
{
    app_editor_open_file(path);
}

static void title_answer(const char *text, void *ctx)
{
    (void)ctx;
    if (!text || !N.list) return;                                /* cancelled (or the app closed) */
    while (*text == ' ') text++;
    const char *title = text[0] ? text : "Note";
    char path[256];
    mkdir(NZ_DIR, 0755);
    mkdir(NOTES_DIR, 0755);
    if (notes_new_path(NOTES_DIR, title, time(NULL), path, sizeof path) != 0) { ui_toast("Too many notes with that title"); return; }
    FILE *f = fopen(path, "w");
    if (!f) { ui_toast("Could not save the note: is the SD card full?"); return; }
    fprintf(f, "%s\n\n", title);                                 /* the first line is the title */
    fclose(f);
    reload();
    open_note(path);
}

static void new_note(void) { nz_prompt("Title of the new note", "", title_answer, NULL); }

static void row_activated(int row, void *ctx)
{
    (void)ctx;
    if (row < FIRST_NOTE) { new_note(); return; }
    if (row - FIRST_NOTE < N.count) open_note(N.notes[row - FIRST_NOTE].path);
}

static char delete_path[256];
static void delete_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!yes || !N.list || !delete_path[0]) return;
    if (unlink(delete_path) == 0) ui_toast("Note deleted");
    else ui_toast("Could not delete the note");
    delete_path[0] = 0;
    reload();
}

static void ask_delete(void)
{
    int i = nzlist_selected(N.list) - FIRST_NOTE;
    if (i < 0 || i >= N.count) { ui_toast("Pick a note first"); return; }
    char q[120];
    snprintf(delete_path, sizeof delete_path, "%s", N.notes[i].path);
    snprintf(q, sizeof q, "Delete \"%s\"?\nIt can't be brought back.", N.notes[i].title);
    nz_confirm(q, delete_answer, NULL);
}

/* ---------- the app ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&N, 0, sizeof N);
    N.notes = calloc(MAX_NOTES, sizeof(Note));
    mkdir(NZ_DIR, 0755);
    mkdir(NOTES_DIR, 0755);
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = th_box(root, TS_PANEL);
    lv_obj_set_size(card, LV_PCT(100), 64);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_NOTES, 48));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    N.count_label = th_label(card, "", TS_HEADING);
    lv_obj_align(N.count_label, LV_ALIGN_TOP_LEFT, 72, 8);
    lv_obj_t *s = th_label(card, "Saved on the SD card in  nzeus / notes", TS_SMALL);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, LV_PCT(80));
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 72, -9);

    N.list = nzlist_create(root, 0);
    lv_obj_set_width(nzlist_obj(N.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(N.list), 1);
    nzlist_on_activate(N.list, row_activated, NULL);

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Open", "X", "New", "Y", "Delete", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 4));

    if (!N.notes) { ui_toast("Not enough memory for Notes"); return; }
    reload();
    N.timer = lv_timer_create(watch, 1000, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (!N.notes) return 0;
    if (k == NZ_KEY_X) { new_note(); return 1; }
    if (k == NZ_KEY_Y) { ask_delete(); return 1; }
    return nzlist_key(N.list, k);
}

static void destroy(NzApp *a)
{
    (void)a;
    if (N.timer) lv_timer_del(N.timer);
    nzlist_free(N.list);
    free(N.notes);
    memset(&N, 0, sizeof N);
}

/* for tests */
int app_notes_count(void) { return N.count; }
const char *app_notes_path(int i) { return i >= 0 && i < N.count ? N.notes[i].path : ""; }
const char *app_notes_title(int i) { return i >= 0 && i < N.count ? N.notes[i].title : ""; }

NzApp app_notes = {
    .id = APP_NOTES, .title = "Notes", .icon = ICON_NOTES,
    .create = create, .key = key, .destroy = destroy,
};

/* app_taskmgr.c - the NZEUS Task Manager.
 *
 * Top: how busy the console is - memory, CPU, swap, programs and up time.
 * List: the running programs, the ones using the most memory first. It is
 * read again every 2 seconds; the selected program stays selected.
 *   A or X   end the selected program (asks first)
 *   B        back
 * Ending a program: first it is asked politely (SIGTERM, "please stop").
 * If it is still there 2 seconds later, NZEUS offers "Force stop" (SIGKILL,
 * which a program can't ignore). Programs NZEUS needs can't be ended:
 * they show a lock, and NZEUS says why. The reading of /proc is in procs.c. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzeus_tick.h"
#include "nzlist.h"
#include "nzui.h"
#include "procs.h"
#include "theme.h"
#include "ui.h"

#define MAX_PROCS   192            /* programs read from /proc (kernel threads included) */
#define REFRESH_MS  2000           /* how often the list is read again */
#define FORCE_MS    2000           /* how long a program gets to stop before "Force stop" */

/* one row of the list, and the text it shows (kept so unchanged rows aren't redrawn) */
typedef struct {
    ProcInfo p;
    int locked;                    /* 1 = NZEUS won't end it */
    char sub[48], right[16];
} Row;

/* one box in the top card: MEMORY / 24.6 MB / [bar] / of 42 MB */
typedef struct { lv_obj_t *value, *detail, *bar; } Stat;

static struct {
    NzList *list;
    Stat mem, cpu, swap, run;
    lv_timer_t *timer;
    int theme_shown;               /* the theme the fonts were picked for */
    ProcInfo *all;                 /* scratch space for procs_list (malloc) */
    Row *rows;                     /* the rows shown (malloc) */
    Row *fresh;                    /* the rows being made (malloc) */
    int count;
    uint32_t last_refresh;
    unsigned long long cpu_busy, cpu_total;   /* the previous CPU reading */
    int cpu_percent;               /* -1 = not measured yet */
    /* the program being ended */
    int end_pid, end_forced, end_asked;
    uint32_t end_since;
    char end_name[PROCS_NAME_LEN];
} T;

/* ---------- small text helpers ---------- */
static void mb_text(char *b, size_t n, long kb)
{
    if (kb < 1024) snprintf(b, n, "%ld KB", kb);
    else if (kb < 100 * 1024) snprintf(b, n, "%.1f MB", kb / 1024.0);
    else snprintf(b, n, "%ld MB", kb / 1024);
}

static void uptime_text(char *b, size_t n, double s)
{
    long t = (long)s;
    if (t >= 86400) snprintf(b, n, "up %ldd %ldh", t / 86400, t / 3600 % 24);
    else if (t >= 3600) snprintf(b, n, "up %ldh %02ldm", t / 3600, t / 60 % 60);
    else snprintf(b, n, "up %ldm %02lds", t / 60, t % 60);
}

static void set_text(lv_obj_t *l, const char *s)     /* only redraw when it changed */
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

/* ---------- the top card ---------- */

/* The CRT themes' heading font is 24 pixels wide per letter: "24.4 MB"
 * wouldn't fit a quarter of a window. There the numbers use the body font. */
static void fit_fonts(void)
{
    const NzTheme *t = theme();
    Stat *all[4] = { &T.mem, &T.cpu, &T.swap, &T.run };
    for (int i = 0; i < 4; i++) lv_obj_set_style_text_font(all[i]->value, t->crt ? t->font_body : t->font_heading, 0);
    T.theme_shown = theme_index();
}
static Stat make_stat(lv_obj_t *card, const char *title, int with_bar)
{
    Stat s;
    lv_obj_t *box = th_box(card, -1);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 2, 0);
    th_label(box, title, TS_SMALL);
    s.value = th_label(box, "", TS_HEADING);
    lv_label_set_long_mode(s.value, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s.value, LV_PCT(100));
    s.bar = NULL;
    if (with_bar) {
        s.bar = lv_bar_create(box);
        lv_bar_set_range(s.bar, 0, 100);
        lv_obj_set_size(s.bar, LV_PCT(90), 6);
        th_add(s.bar, TS_BTN);
        th_add_to(s.bar, TS_BTN_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_pad_ver(s.bar, 2, 0);
    }
    s.detail = th_label(box, "", TS_SMALL);
    lv_label_set_long_mode(s.detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s.detail, LV_PCT(100));
    return s;
}

static void show_summary(int programs)
{
    ProcSummary s;
    char a[32], b[48];
    if (procs_summary(&s) != 0) { set_text(T.mem.value, "?"); return; }

    long used = s.mem_total_kb - s.mem_avail_kb;
    mb_text(a, sizeof a, used);
    set_text(T.mem.value, a);
    mb_text(a, sizeof a, s.mem_total_kb);
    snprintf(b, sizeof b, "of %s", a);
    set_text(T.mem.detail, b);
    lv_bar_set_value(T.mem.bar, s.mem_total_kb > 0 ? (int)(100 * used / s.mem_total_kb) : 0, LV_ANIM_OFF);

    unsigned long long busy, total;                  /* CPU: how busy since the last reading */
    if (procs_cpu_times(&busy, &total) == 0) {
        if (T.cpu_total && total > T.cpu_total && busy >= T.cpu_busy)
            T.cpu_percent = (int)(100 * (busy - T.cpu_busy) / (total - T.cpu_total));
        T.cpu_busy = busy;
        T.cpu_total = total;
    }
    if (T.cpu_percent >= 0) snprintf(a, sizeof a, "%d %%", T.cpu_percent);
    else snprintf(a, sizeof a, "...");
    set_text(T.cpu.value, a);
    lv_bar_set_value(T.cpu.bar, T.cpu_percent > 0 ? T.cpu_percent : 0, LV_ANIM_OFF);
    snprintf(b, sizeof b, "load %.2f", s.load1);
    set_text(T.cpu.detail, b);

    if (s.swap_total_kb > 0) {
        mb_text(a, sizeof a, s.swap_total_kb - s.swap_free_kb);
        set_text(T.swap.value, a);
        mb_text(a, sizeof a, s.swap_total_kb);
        snprintf(b, sizeof b, "of %s", a);
        set_text(T.swap.detail, b);
    } else {
        set_text(T.swap.value, "none");
        set_text(T.swap.detail, "no swap file");
    }

    snprintf(a, sizeof a, "%d", programs);
    set_text(T.run.value, a);
    uptime_text(b, sizeof b, s.uptime_s);
    set_text(T.run.detail, b);
}

/* ---------- the list ---------- */
static int row_of_pid(int pid)
{
    for (int i = 0; i < T.count; i++) if (T.rows[i].p.pid == pid) return i;
    return -1;
}

static void refresh(void)
{
    if (!T.all || !T.rows || !T.fresh) return;
    int sel = nzlist_selected(T.list);
    int sel_pid = sel >= 0 && sel < T.count ? T.rows[sel].p.pid : -1;
    int n = procs_list(T.all, MAX_PROCS);
    Row *fresh = T.fresh;

    /* the new rows: running programs only (kernel threads are part of Linux;
     * a finished "zombie" just waits for its parent to notice) */
    int count = 0, rebuild = 0;
    for (int i = 0; i < n; i++) {
        if (T.all[i].kernel || T.all[i].state == 'Z') continue;
        Row *r = &fresh[count];
        r->p = T.all[i];
        int old = row_of_pid(r->p.pid);           /* known program: its lock can't change */
        r->locked = old >= 0 ? T.rows[old].locked : procs_protected(&r->p, procs_self()) != NULL;
        snprintf(r->sub, sizeof r->sub, "PID %d   %s", r->p.pid, procs_state_word(r->p.state));
        mb_text(r->right, sizeof r->right, r->p.rss_kb);
        if (count >= T.count || T.rows[count].locked != r->locked) rebuild = 1;   /* icon changes */
        count++;
    }
    if (count != T.count) rebuild = 1;

    if (rebuild) {                                 /* new rows from scratch */
        nzlist_clear(T.list);
        for (int i = 0; i < count; i++)
            nzlist_add(T.list, fresh[i].locked ? ICON_LOCK : ICON_GAUGE, fresh[i].p.name, fresh[i].sub, fresh[i].right);
    } else {                                       /* same rows: change only the text that changed */
        for (int i = 0; i < count; i++) {
            if (strcmp(T.rows[i].p.name, fresh[i].p.name)) nzlist_set_title(T.list, i, fresh[i].p.name);
            if (strcmp(T.rows[i].sub, fresh[i].sub)) nzlist_set_sub(T.list, i, fresh[i].sub);
            if (strcmp(T.rows[i].right, fresh[i].right)) nzlist_set_right(T.list, i, fresh[i].right);
        }
    }
    memcpy(T.rows, fresh, sizeof(Row) * (size_t)count);
    T.count = count;

    int keep = row_of_pid(sel_pid);                /* the same program stays selected */
    if (keep >= 0) nzlist_select(T.list, keep);
    else if (sel >= 0) nzlist_select(T.list, sel < count ? sel : count - 1);
    show_summary(count);
    T.last_refresh = nzeus_tick_ms();
}

/* ---------- ending a program ---------- */
static void force_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!T.list || !T.end_pid) return;             /* the app was closed meanwhile */
    if (!yes) { T.end_pid = 0; return; }
    if (procs_end(T.end_pid, 1) != 0) {
        char m[96];
        snprintf(m, sizeof m, "Could not stop %s: %s", T.end_name, strerror(errno));
        ui_toast(m);
        T.end_pid = 0;
        return;
    }
    T.end_forced = 1;
    T.end_since = nzeus_tick_ms();
}

static void end_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!T.list || !yes || !T.end_pid) { T.end_pid = 0; return; }
    if (procs_end(T.end_pid, 0) != 0) {
        char m[96];
        snprintf(m, sizeof m, "Could not end %s: %s", T.end_name, strerror(errno));
        ui_toast(m);
        T.end_pid = 0;
        return;
    }
    T.end_since = nzeus_tick_ms();
    T.end_forced = T.end_asked = 0;
    char m[64];
    snprintf(m, sizeof m, "Asking %s to stop...", T.end_name);
    ui_toast(m);
}

static void ask_end(int i, void *ctx)
{
    (void)ctx;
    if (i < 0 || i >= T.count) return;
    ProcInfo p;
    if (procs_read(T.rows[i].p.pid, &p) != 0 || !procs_alive(p.pid)) { ui_toast("That program has already ended"); refresh(); return; }
    char m[160];
    const char *why = procs_protected(&p, procs_self());
    if (why) {
        snprintf(m, sizeof m, "Can't end %s: %s", p.name, why);
        ui_toast(m);
        return;
    }
    T.end_pid = p.pid;
    snprintf(T.end_name, sizeof T.end_name, "%s", p.name);
    char size[16];
    mb_text(size, sizeof size, p.rss_kb);
    snprintf(m, sizeof m, "End %s?\nPID %d, using %s. Unsaved work in it is lost.", p.name, p.pid, size);
    nz_confirm(m, end_answer, NULL);
}

/* is the program we asked to stop gone yet? */
static void check_ending(void)
{
    if (!T.end_pid || nzui_active()) return;
    char m[96];
    if (!procs_alive(T.end_pid)) {
        snprintf(m, sizeof m, "%s has ended", T.end_name);
        ui_toast(m);
        T.end_pid = 0;
        refresh();
        return;
    }
    if (!T.end_since || nzeus_tick_ms() - T.end_since < FORCE_MS) return;
    if (T.end_forced) {                            /* even SIGKILL didn't work (stuck in the kernel) */
        snprintf(m, sizeof m, "%s won't stop", T.end_name);
        ui_toast(m);
        T.end_pid = 0;
    } else if (!T.end_asked) {
        T.end_asked = 1;
        snprintf(m, sizeof m, "%s is not stopping.\nForce stop? (it can't save anything)", T.end_name);
        nz_confirm(m, force_answer, NULL);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    check_ending();
    if (theme_index() != T.theme_shown) fit_fonts();       /* the theme was changed */
    if (nzeus_tick_ms() - T.last_refresh >= REFRESH_MS) refresh();
}

/* ---------- the app ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&T, 0, sizeof T);
    T.cpu_percent = -1;
    T.all = malloc(sizeof(ProcInfo) * MAX_PROCS);
    T.rows = malloc(sizeof(Row) * MAX_PROCS);
    T.fresh = malloc(sizeof(Row) * MAX_PROCS);
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = th_box(root, TS_PANEL);       /* the top card: 4 numbers side by side */
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_pad_column(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    T.mem = make_stat(card, "MEMORY", 1);
    T.cpu = make_stat(card, "CPU", 1);
    T.swap = make_stat(card, "SWAP", 0);
    T.run = make_stat(card, "PROGRAMS", 0);
    fit_fonts();

    T.list = nzlist_create(root, 0);
    lv_obj_set_width(nzlist_obj(T.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(T.list), 1);
    nzlist_on_activate(T.list, ask_end, NULL);

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "End program", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 2));

    if (T.all && T.rows && T.fresh) refresh();
    else ui_toast("Not enough memory for the Task Manager");
    T.last_refresh -= REFRESH_MS - 500;            /* the 2nd reading (CPU %) after 0.5 s, not 2 s */
    T.timer = lv_timer_create(timer_cb, 250, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (k == NZ_KEY_A || k == NZ_KEY_X) { ask_end(nzlist_selected(T.list), NULL); return 1; }
    return nzlist_key(T.list, k);
}

static void destroy(NzApp *a)
{
    (void)a;
    if (T.timer) lv_timer_del(T.timer);
    nzlist_free(T.list);
    free(T.all);
    free(T.rows);
    free(T.fresh);
    memset(&T, 0, sizeof T);
}

/* for tests */
int app_taskmgr_count(void) { return T.count; }
int app_taskmgr_selected_pid(void) { int i = T.list ? nzlist_selected(T.list) : -1; return i >= 0 && i < T.count ? T.rows[i].p.pid : -1; }
int app_taskmgr_select_pid(int pid) { int i = row_of_pid(pid); if (i >= 0) nzlist_select(T.list, i); return i; }
void app_taskmgr_refresh(void) { if (T.list) refresh(); }

NzApp app_taskmgr = {
    .id = APP_TASKMGR, .title = "Task Manager", .icon = ICON_TASKMGR,
    .create = create, .key = key, .destroy = destroy,
};

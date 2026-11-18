/* app_clock.c - the NZEUS Clock: four pages, switched with L1 / R1 (or by
 * clicking the tabs at the top).
 *   Clock      the time in big numbers, the date in words, the time zone
 *   Calendar   this month; LEFT/RIGHT another month, UP/DOWN another year, A today
 *   Stopwatch  START start/stop, X lap, Y reset
 *   Timer      UP/DOWN minutes, START start/pause, Y reset; when it reaches
 *              zero the screen flashes "Time's up!" until a button is pressed
 * The time zone is the one set in Settings > Date & time (NZEUS applies it
 * when it starts, so the normal clock functions already use it).
 * To keep the slow CPU free, only the labels that change are redrawn: the
 * clock once a second, the stopwatch 20 times a second only while it runs. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzeus_tick.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

enum { PG_CLOCK, PG_CALENDAR, PG_STOPWATCH, PG_TIMER, PG_COUNT };
static const char *const page_names[PG_COUNT] = { "Clock", "Calendar", "Stopwatch", "Timer" };
#define MAX_LAPS 99

static struct {
    lv_obj_t *tabs[PG_COUNT], *pages[PG_COUNT], *hint_bar, *hints;
    int page;
    lv_timer_t *timer;
    int theme_shown;                  /* the theme the fonts were picked for */
    /* clock */
    lv_obj_t *time, *date, *zone;
    time_t shown_sec;
    /* calendar */
    lv_obj_t *month_title, *chips[42], *days[42];
    int cal_year, cal_month;          /* the month shown (month 0 = January) */
    int today_cell;                   /* the highlighted cell, -1 = none */
    int shown_yday;                   /* the day the calendar was drawn on */
    /* stopwatch */
    lv_obj_t *sw_time, *sw_state, *sw_start_label;
    NzList *laps;
    int sw_running, lap_count;
    uint32_t sw_started, sw_before;   /* tick when started; time counted before that */
    uint32_t lap_total[MAX_LAPS];
    /* timer */
    lv_obj_t *tm_time, *tm_state, *tm_start_label, *overlay;
    int tm_minutes, tm_running, tm_paused, tm_flash;
    uint32_t tm_end, tm_left;         /* tick when it reaches zero; ms left while paused */
    uint32_t flash_start;
} K;

static const char *const month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December" };
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static struct tm now_tm(void) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); return tm; }

/* a clickable button with a text, for the mouse */
static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *b = th_box(parent, TS_BTN);
    th_add_to(b, TS_BTN_ACCENT, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label) *label = l;
    return b;
}

/* an empty page: a column, centred */
static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *p = th_box(parent, -1);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(p, 8, 0);
    lv_obj_set_style_pad_row(p, 8, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* ---------- Clock ---------- */

/* The CRT themes' heading font is 24 pixels wide per letter: a long date
 * wouldn't fit a window. There the date uses the body font instead. */
static void fit_fonts(void)
{
    const NzTheme *t = theme();
    lv_obj_set_style_text_font(K.date, t->crt ? t->font_body : t->font_heading, 0);
    K.theme_shown = theme_index();
}
static void show_clock(void)
{
    struct tm tm = now_tm();
    char b[64];
    strftime(b, sizeof b, "%H:%M:%S", &tm);
    lv_label_set_text(K.time, b);
    lv_label_set_text_fmt(K.date, "%s %d %s %d", day_names[tm.tm_wday], tm.tm_mday, month_names[tm.tm_mon], tm.tm_year + 1900);
    int tz = settings_get_int("tz", 0);
    if (tz) lv_label_set_text_fmt(K.zone, "Time zone  UTC %+d", tz);
    else lv_label_set_text(K.zone, "Time zone  UTC");
}

/* ---------- Calendar ---------- */
static int days_in_month(int year, int month)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 1 && leap ? 29 : d[month];
}

/* 0 = Sunday ... 6 = Saturday (Zeller-style formula, no time functions needed) */
static int weekday(int year, int month, int day)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 2) year--;
    return (year + year / 4 - year / 100 + year / 400 + t[month] + day) % 7;
}

static void show_calendar(void)
{
    struct tm tm = now_tm();
    int first = weekday(K.cal_year, K.cal_month, 1), n = days_in_month(K.cal_year, K.cal_month);
    lv_label_set_text_fmt(K.month_title, "%s %d", month_names[K.cal_month], K.cal_year);
    int today = (tm.tm_year + 1900 == K.cal_year && tm.tm_mon == K.cal_month) ? first + tm.tm_mday - 1 : -1;
    for (int i = 0; i < 42; i++) {
        int day = i - first + 1;
        if (day >= 1 && day <= n) lv_label_set_text_fmt(K.days[i], "%d", day);
        else lv_label_set_text(K.days[i], "");
    }
    if (K.today_cell != today) {                       /* move the highlight */
        if (K.today_cell >= 0) lv_obj_remove_style(K.chips[K.today_cell], th_style(TS_BTN_ACCENT), 0);
        if (today >= 0) th_add(K.chips[today], TS_BTN_ACCENT);
        K.today_cell = today;
    }
    K.shown_yday = tm.tm_yday;
}

static void change_month(int d)
{
    K.cal_month += d;
    while (K.cal_month < 0) { K.cal_month += 12; K.cal_year--; }
    while (K.cal_month > 11) { K.cal_month -= 12; K.cal_year++; }
    if (K.cal_year < 1900) K.cal_year = 1900;
    if (K.cal_year > 9999) K.cal_year = 9999;
    show_calendar();
}

static void go_today(void)
{
    struct tm tm = now_tm();
    K.cal_year = tm.tm_year + 1900;
    K.cal_month = tm.tm_mon;
    show_calendar();
}

static void prev_clicked(lv_event_t *e) { (void)e; change_month(-1); }
static void next_clicked(lv_event_t *e) { (void)e; change_month(1); }

/* ---------- Stopwatch ---------- */
static uint32_t sw_elapsed(void) { return K.sw_before + (K.sw_running ? nzeus_tick_ms() - K.sw_started : 0); }

/* 83456 ms -> "01:23.45"; over an hour: "1:02:03" */
static void sw_format(uint32_t ms, char *b, size_t n)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(b, n, "%02u:%02u.%02u", (unsigned)(s / 60), (unsigned)(s % 60), (unsigned)(ms % 1000 / 10));
}

static void show_stopwatch(void)
{
    char b[24];
    sw_format(sw_elapsed(), b, sizeof b);
    lv_label_set_text(K.sw_time, b);
}

static void show_sw_state(void)
{
    lv_label_set_text(K.sw_state, K.sw_running ? "Running" : sw_elapsed() ? "Stopped" : "Press START to start");
    lv_label_set_text(K.sw_start_label, K.sw_running ? "Stop" : "Start");
}

static void sw_start_stop(void)
{
    if (K.sw_running) { K.sw_before = sw_elapsed(); K.sw_running = 0; }
    else { K.sw_started = nzeus_tick_ms(); K.sw_running = 1; }
    show_stopwatch();
    show_sw_state();
}

static void show_laps(void)
{
    nzlist_clear(K.laps);
    for (int i = K.lap_count - 1; i >= 0; i--) {          /* newest first */
        char title[16], lap[24], total[24], right[64];
        snprintf(title, sizeof title, "Lap %d", i + 1);
        sw_format(K.lap_total[i] - (i ? K.lap_total[i - 1] : 0), lap, sizeof lap);
        sw_format(K.lap_total[i], total, sizeof total);
        snprintf(right, sizeof right, "%s     total %s", lap, total);
        nzlist_add(K.laps, -1, title, NULL, right);
    }
}

static void sw_lap(void)
{
    if (!K.sw_running) { ui_toast("Start the stopwatch first"); return; }
    if (K.lap_count >= MAX_LAPS) { ui_toast("That's the most laps it can keep"); return; }
    K.lap_total[K.lap_count++] = sw_elapsed();
    show_laps();
}

static void sw_reset(void)
{
    K.sw_running = 0;
    K.sw_before = 0;
    K.lap_count = 0;
    nzlist_clear(K.laps);
    show_stopwatch();
    show_sw_state();
}

static void sw_start_clicked(lv_event_t *e) { (void)e; sw_start_stop(); }
static void sw_lap_clicked(lv_event_t *e) { (void)e; sw_lap(); }
static void sw_reset_clicked(lv_event_t *e) { (void)e; sw_reset(); }

/* ---------- Timer ---------- */
static uint32_t tm_remaining(void)
{
    if (K.tm_paused) return K.tm_left;
    if (!K.tm_running) return (uint32_t)K.tm_minutes * 60000u;
    int32_t left = (int32_t)(K.tm_end - nzeus_tick_ms());
    return left > 0 ? (uint32_t)left : 0;
}

static void show_timer(void)
{
    uint32_t s = (tm_remaining() + 999) / 1000;            /* round up: 0.4 s left still shows 00:01 */
    if (s >= 3600) lv_label_set_text_fmt(K.tm_time, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else lv_label_set_text_fmt(K.tm_time, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    lv_label_set_text(K.tm_state, K.tm_running ? (K.tm_paused ? "Paused" : "Counting down") : "UP/DOWN sets the minutes");
    lv_label_set_text(K.tm_start_label, K.tm_running && !K.tm_paused ? "Pause" : "Start");
}

static void tm_set(int d)
{
    if (K.tm_running) { ui_toast("Reset the timer to change it"); return; }
    K.tm_minutes += d;
    if (K.tm_minutes < 1) K.tm_minutes = 1;
    if (K.tm_minutes > 180) K.tm_minutes = 180;
    show_timer();
}

static void tm_start_pause(void)
{
    if (!K.tm_running) { K.tm_running = 1; K.tm_paused = 0; K.tm_end = nzeus_tick_ms() + (uint32_t)K.tm_minutes * 60000u; }
    else if (K.tm_paused) { K.tm_paused = 0; K.tm_end = nzeus_tick_ms() + K.tm_left; }
    else { K.tm_left = tm_remaining(); K.tm_paused = 1; }
    show_timer();
}

static void tm_reset(void) { K.tm_running = K.tm_paused = 0; show_timer(); }

static void hide_overlay(void)
{
    if (!K.overlay) return;
    lv_obj_del(K.overlay);
    K.overlay = NULL;
}

static void overlay_clicked(lv_event_t *e) { (void)e; hide_overlay(); }

/* the timer reached zero: cover the app with a big flashing message */
static void tm_finished(lv_obj_t *root)
{
    K.tm_running = K.tm_paused = 0;
    show_timer();
    ui_toast("Clock: time's up!");                         /* seen even when another app is in front */
    hide_overlay();
    K.overlay = th_box(root, TS_BTN_ACCENT);
    lv_obj_add_flag(K.overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);  /* on top of the pages, not in the column */
    lv_obj_add_flag(K.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(K.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(K.overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(K.overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(K.overlay, 12, 0);
    lv_obj_t *big = lv_label_create(K.overlay);              /* (takes the overlay's text colour) */
    lv_label_set_text(big, "Time's up!");
    lv_obj_set_style_text_font(big, theme()->font_big, 0);
    lv_obj_t *small = lv_label_create(K.overlay);
    lv_label_set_text_fmt(small, "%d minute%s - press any button", K.tm_minutes, K.tm_minutes == 1 ? "" : "s");
    lv_obj_add_event_cb(K.overlay, overlay_clicked, LV_EVENT_CLICKED, NULL);
    K.tm_flash = 1;
    K.flash_start = nzeus_tick_ms();
}

/* swap the overlay between the accent colour and the normal one twice a second, for 10 s */
static void flash(void)
{
    if (!K.overlay) return;
    uint32_t t = nzeus_tick_ms() - K.flash_start;
    int accent = t > 10000 || (t / 500) % 2 == 0;
    if (accent == K.tm_flash) return;
    K.tm_flash = accent;
    lv_obj_remove_style(K.overlay, th_style(accent ? TS_PANEL : TS_BTN_ACCENT), 0);
    th_add(K.overlay, accent ? TS_BTN_ACCENT : TS_PANEL);
}

static void tm_start_clicked(lv_event_t *e) { (void)e; tm_start_pause(); }
static void tm_minus_clicked(lv_event_t *e) { (void)e; tm_set(-1); }
static void tm_plus_clicked(lv_event_t *e) { (void)e; tm_set(1); }
static void tm_reset_clicked(lv_event_t *e) { (void)e; tm_reset(); }

/* ---------- pages ---------- */
static void set_hints(void)
{
    static const char *const h_clock[] = { "L1/R1", "Pages", "B", "Back" };
    static const char *const h_cal[] = { "< >", "Month", "UP/DN", "Year", "A", "Today", "B", "Back" };
    static const char *const h_sw[] = { "START", "Start/Stop", "X", "Lap", "Y", "Reset", "B", "Back" };
    static const char *const h_tm[] = { "UP/DN", "Minutes", "START", "Go/Pause", "Y", "Reset", "B", "Back" };
    if (K.hints) lv_obj_del(K.hints);
    switch (K.page) {
    case PG_CLOCK:     K.hints = nz_hints(K.hint_bar, h_clock, 2); break;
    case PG_CALENDAR:  K.hints = nz_hints(K.hint_bar, h_cal, 4); break;
    case PG_STOPWATCH: K.hints = nz_hints(K.hint_bar, h_sw, 4); break;
    default:           K.hints = nz_hints(K.hint_bar, h_tm, 4); break;
    }
    lv_obj_center(K.hints);
}

static void show_page(int p)
{
    K.page = (p + PG_COUNT) % PG_COUNT;
    for (int i = 0; i < PG_COUNT; i++) {
        if (i == K.page) { lv_obj_clear_flag(K.pages[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_state(K.tabs[i], LV_STATE_CHECKED); }
        else { lv_obj_add_flag(K.pages[i], LV_OBJ_FLAG_HIDDEN); lv_obj_clear_state(K.tabs[i], LV_STATE_CHECKED); }
    }
    set_hints();
}

static void tab_clicked(lv_event_t *e) { show_page((int)(intptr_t)lv_event_get_user_data(e)); }

static void tick_cb(lv_timer_t *t)
{
    lv_obj_t *root = t->user_data;
    time_t sec = time(NULL);
    if (sec != K.shown_sec) {                              /* a new second */
        K.shown_sec = sec;
        if (K.page == PG_CLOCK) show_clock();
        struct tm tm = now_tm();
        if (tm.tm_yday != K.shown_yday) show_calendar();    /* midnight: move "today" */
    }
    if (K.sw_running && K.page == PG_STOPWATCH) show_stopwatch();
    if (K.tm_running && !K.tm_paused) {
        if (tm_remaining() == 0) tm_finished(root);
        else if (K.page == PG_TIMER) show_timer();
    }
    flash();
    if (theme_index() != K.theme_shown) fit_fonts();       /* the theme was changed */
}

/* ---------- building ---------- */
static void build_clock(lv_obj_t *p)
{
    K.time = th_label(p, "", TS_BIG);
    K.date = th_label(p, "", TS_HEADING);
    K.zone = th_label(p, "", TS_MUTED);
    lv_obj_t *s = th_label(p, "Set the time and zone in Settings > Date & time", TS_SMALL);
    lv_obj_set_style_pad_top(s, 18, 0);
    lv_obj_set_style_pad_top(K.date, 6, 0);
    fit_fonts();
    show_clock();
}

static void build_calendar(lv_obj_t *p)
{
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_t *head = th_box(p, -1);                        /* <  October 2026  > */
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    small_button(head, "<", prev_clicked, NULL);
    K.month_title = th_label(head, "", TS_HEADING);
    small_button(head, ">", next_clicked, NULL);

    static lv_coord_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t rows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *grid = th_box(p, -1);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_grid_dsc_array(grid, cols, rows);
    static const char *const wd[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    for (int c = 0; c < 7; c++) {                          /* Friday + Saturday (the weekend) in the accent colour */
        lv_obj_t *l = th_label(grid, wd[c], c >= 5 ? TS_TITLE : TS_MUTED);
        lv_obj_set_grid_cell(l, LV_GRID_ALIGN_CENTER, c, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }
    for (int i = 0; i < 42; i++) {
        lv_obj_t *chip = th_box(grid, -1);                 /* the day number in a small box (accent = today) */
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(chip, 34, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(chip, 3, 0);
        lv_obj_set_style_pad_hor(chip, 4, 0);
        lv_obj_set_grid_cell(chip, LV_GRID_ALIGN_CENTER, i % 7, 1, LV_GRID_ALIGN_CENTER, 1 + i / 7, 1);
        K.days[i] = lv_label_create(chip);
        K.chips[i] = chip;
    }
    K.today_cell = -1;
    go_today();
}

static void build_stopwatch(lv_obj_t *p)
{
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    K.sw_time = th_label(p, "", TS_BIG);
    lv_obj_set_style_pad_top(K.sw_time, 4, 0);
    K.sw_state = th_label(p, "", TS_SMALL);
    lv_obj_t *row = th_box(p, -1);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    small_button(row, "Start", sw_start_clicked, &K.sw_start_label);
    small_button(row, "Lap", sw_lap_clicked, NULL);
    small_button(row, "Reset", sw_reset_clicked, NULL);
    K.laps = nzlist_create(p, 30);
    lv_obj_set_width(nzlist_obj(K.laps), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(K.laps), 1);
    show_stopwatch();
    show_sw_state();
}

static void build_timer(lv_obj_t *p)
{
    K.tm_time = th_label(p, "", TS_BIG);
    K.tm_state = th_label(p, "", TS_SMALL);
    lv_obj_t *row = th_box(p, -1);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_top(row, 10, 0);
    small_button(row, "-1 min", tm_minus_clicked, NULL);
    small_button(row, "+1 min", tm_plus_clicked, NULL);
    small_button(row, "Start", tm_start_clicked, &K.tm_start_label);
    small_button(row, "Reset", tm_reset_clicked, NULL);
    show_timer();
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&K, 0, sizeof K);
    K.tm_minutes = 5;
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    /* tabs:  [L1]  Clock  Calendar  Stopwatch  Timer  [R1] */
    lv_obj_t *bar = th_box(root, TS_PANEL);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    th_label(bar, "L1", TS_KEYCAP);
    for (int i = 0; i < PG_COUNT; i++) {
        lv_obj_t *t = th_box(bar, -1);
        th_add_to(t, TS_BTN_ACCENT, LV_STATE_CHECKED);     /* the page shown: accent */
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_height(t, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(t, 5, 0);
        lv_obj_t *l = lv_label_create(t);
        lv_label_set_text(l, page_names[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(t, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        K.tabs[i] = t;
    }
    th_label(bar, "R1", TS_KEYCAP);

    lv_obj_t *body = th_box(root, -1);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    for (int i = 0; i < PG_COUNT; i++) K.pages[i] = make_page(body);
    build_clock(K.pages[PG_CLOCK]);
    build_calendar(K.pages[PG_CALENDAR]);
    build_stopwatch(K.pages[PG_STOPWATCH]);
    build_timer(K.pages[PG_TIMER]);

    K.hint_bar = th_box(root, TS_BAR);
    lv_obj_set_size(K.hint_bar, LV_PCT(100), 26);
    show_page(PG_CLOCK);
    K.shown_sec = time(NULL);
    K.timer = lv_timer_create(tick_cb, 50, root);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (K.overlay) { hide_overlay(); return 1; }           /* any button closes "Time's up!" */
    if (k == NZ_KEY_L) { show_page(K.page - 1); return 1; }
    if (k == NZ_KEY_R) { show_page(K.page + 1); return 1; }
    switch (K.page) {
    case PG_CALENDAR:
        if (k == NZ_KEY_LEFT) { change_month(-1); return 1; }
        if (k == NZ_KEY_RIGHT) { change_month(1); return 1; }
        if (k == NZ_KEY_UP) { change_month(-12); return 1; }
        if (k == NZ_KEY_DOWN) { change_month(12); return 1; }
        if (k == NZ_KEY_A) { go_today(); return 1; }
        break;
    case PG_STOPWATCH:
        if (k == NZ_KEY_START || k == NZ_KEY_A) { sw_start_stop(); return 1; }
        if (k == NZ_KEY_X) { sw_lap(); return 1; }
        if (k == NZ_KEY_Y) { sw_reset(); return 1; }
        if (k == NZ_KEY_UP || k == NZ_KEY_DOWN) return nzlist_key(K.laps, k);
        break;
    case PG_TIMER:
        if (k == NZ_KEY_UP) { tm_set(1); return 1; }
        if (k == NZ_KEY_DOWN) { tm_set(-1); return 1; }
        if (k == NZ_KEY_START || k == NZ_KEY_A) { tm_start_pause(); return 1; }
        if (k == NZ_KEY_Y) { tm_reset(); return 1; }
        break;
    default: break;
    }
    return 0;
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    switch (ev->key) {                                     /* the keyboard works like the buttons */
    case KB_TAB:     key(a, NZ_KEY_R); break;
    case KB_BACKTAB: key(a, NZ_KEY_L); break;
    case KB_LEFT:    key(a, NZ_KEY_LEFT); break;
    case KB_RIGHT:   key(a, NZ_KEY_RIGHT); break;
    case KB_UP:      key(a, NZ_KEY_UP); break;
    case KB_DOWN:    key(a, NZ_KEY_DOWN); break;
    case KB_ENTER:   key(a, NZ_KEY_START); break;
    case KB_ESC:     hide_overlay(); break;
    case KB_CHAR:
        if (ev->ch == ' ') key(a, NZ_KEY_START);
        else if (ev->ch == 'l' || ev->ch == 'L') key(a, NZ_KEY_X);
        else if (ev->ch == 'r' || ev->ch == 'R') key(a, NZ_KEY_Y);
        else if (ev->ch == 't' || ev->ch == 'T') key(a, NZ_KEY_A);
        break;
    default: break;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    if (K.timer) lv_timer_del(K.timer);
    nzlist_free(K.laps);
    memset(&K, 0, sizeof K);
}

/* for tests */
int app_clock_page(void) { return K.page; }
const char *app_clock_text(int which)       /* 0 time, 1 date, 2 stopwatch, 3 timer, 4 month */
{
    lv_obj_t *l = which == 0 ? K.time : which == 1 ? K.date : which == 2 ? K.sw_time : which == 3 ? K.tm_time : K.month_title;
    return l ? lv_label_get_text(l) : "";
}
int app_clock_lap_count(void) { return K.lap_count; }
int app_clock_overlay_shown(void) { return K.overlay != NULL; }
void app_clock_timer_skip(uint32_t ms) { if (K.tm_running && !K.tm_paused) K.tm_end -= ms; }   /* fast-forward */
void app_clock_stopwatch_skip(uint32_t ms) { K.sw_before += ms; }

NzApp app_clock = {
    .id = APP_CLOCK, .title = "Clock", .icon = ICON_CLOCK,
    .create = create, .key = key, .kbd = kbd, .destroy = destroy,
};

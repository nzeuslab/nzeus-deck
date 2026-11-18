/* app_health.c - the NZEUS Health dashboard: one screen that says how the
 * console is doing, so a freeze or a flat battery is never a surprise.
 * Top: "All good", or the worries (worst first). Below: memory, zram,
 * battery, SD card, temperature, how long it's been on and the biggest programs.
 * The readings and the verdict are in health.c. Refreshes every 2 s.
 *   A  refresh now     B  back */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "health.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

enum { R_MEMORY, R_ZRAM, R_BATTERY, R_SD, R_TEMP, R_UP, R_BIGGEST, R_COUNT };
static const char *const row_name[R_COUNT] = {
    "Memory", "zram", "Battery", "SD card", "Temperature", "Switched on", "Biggest"
};

static struct {
    HealthInfo h;
    char verdict[512];
    int worries;
    lv_obj_t *verdict_lbl, *val[R_COUNT], *bar[R_COUNT];
    lv_timer_t *timer;
} H;

/* a bar's fill in % (hidden when the value isn't known) */
static void set_bar(int row, long part, long whole)
{
    if (!H.bar[row]) return;
    if (whole <= 0 || part < 0) { lv_obj_add_flag(H.bar[row], LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(H.bar[row], LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(H.bar[row], (int32_t)(part * 100 / whole), LV_ANIM_OFF);
}

/* 13312 MB -> "13.0 GB", 900 -> "900 MB" */
static void size_mb(long mb, char *out, size_t n)
{
    if (mb >= 1024) snprintf(out, n, "%ld.%ld GB", mb / 1024, mb % 1024 * 10 / 1024);
    else snprintf(out, n, "%ld MB", mb);
}

static void refresh(void)
{
    char s[160], a[24], b[24], d[24];
    HealthInfo *h = &H.h;
    health_read(h);
    H.worries = health_verdict(h, H.verdict, sizeof H.verdict);
    lv_label_set_text(H.verdict_lbl, H.verdict);

    if (h->mem_total_kb > 0)
        snprintf(s, sizeof s, "%ld MB free of %ld MB", h->mem_avail_kb / 1024, h->mem_total_kb / 1024);
    else snprintf(s, sizeof s, "unknown");
    lv_label_set_text(H.val[R_MEMORY], s);
    set_bar(R_MEMORY, h->mem_total_kb - h->mem_avail_kb, h->mem_total_kb);   /* the bar shows what is used */

    if (h->swap_total_kb > 0) snprintf(s, sizeof s, "%ld of %ld MB used", h->swap_used_kb / 1024, h->swap_total_kb / 1024);
    else snprintf(s, sizeof s, "off");
    lv_label_set_text(H.val[R_ZRAM], s);
    set_bar(R_ZRAM, h->swap_used_kb, h->swap_total_kb);

    if (h->battery >= 0) snprintf(s, sizeof s, "%d%%%s", h->battery, h->charging ? "  (charging)" : "");
    else snprintf(s, sizeof s, h->charging ? "unknown (charging)" : "unknown");
    lv_label_set_text(H.val[R_BATTERY], s);
    set_bar(R_BATTERY, h->battery, 100);

    if (h->sd_total_mb > 0) {
        size_mb(h->sd_free_mb, a, sizeof a);
        size_mb(h->sd_total_mb, b, sizeof b);
        snprintf(s, sizeof s, "%s free of %s", a, b);
    } else snprintf(s, sizeof s, "unknown");
    lv_label_set_text(H.val[R_SD], s);
    set_bar(R_SD, h->sd_total_mb - h->sd_free_mb, h->sd_total_mb);

    if (h->temp_c == HEALTH_NO_TEMP) snprintf(s, sizeof s, "not measured on this console");
    else snprintf(s, sizeof s, "%d C", h->temp_c);
    lv_label_set_text(H.val[R_TEMP], s);

    health_duration((long)h->uptime_s, d, sizeof d);
    snprintf(s, sizeof s, "%s ago   (load %.2f)", d, h->load1);
    lv_label_set_text(H.val[R_UP], s);

    size_t k = 0;
    s[0] = 0;
    for (int i = 0; i < h->ntop && k < sizeof s; i++)
        k += (size_t)snprintf(s + k, sizeof s - k, "%s%s %ld.%ld MB", i ? ",  " : "", h->top_name[i],
                              h->top_kb[i] / 1024, h->top_kb[i] % 1024 * 10 / 1024);
    lv_label_set_text(H.val[R_BIGGEST], h->ntop ? s : "unknown");
}

static void tick(lv_timer_t *t) { (void)t; refresh(); }

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&H, 0, sizeof H);
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 4, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = th_box(body, TS_PANEL);                    /* the verdict */
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, 8, 0);
    H.verdict_lbl = th_label(box, "", TS_HEADING);
    lv_obj_set_width(H.verdict_lbl, LV_PCT(100));
    lv_label_set_long_mode(H.verdict_lbl, LV_LABEL_LONG_WRAP);

    for (int r = 0; r < R_COUNT; r++) {                       /* name | value | bar */
        lv_obj_t *row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_pad_top(row, 4, 0);
        lv_obj_t *name = th_label(row, row_name[r], TS_MUTED);
        lv_obj_set_width(name, 132);
        H.val[r] = th_label(row, "", TS_TEXT);
        lv_obj_set_flex_grow(H.val[r], 1);
        lv_label_set_long_mode(H.val[r], LV_LABEL_LONG_DOT);
        if (r == R_MEMORY || r == R_ZRAM || r == R_BATTERY || r == R_SD) {
            H.bar[r] = lv_bar_create(row);
            lv_bar_set_range(H.bar[r], 0, 100);
            lv_obj_set_size(H.bar[r], 120, 8);
            th_add(H.bar[r], TS_BTN);
            th_add_to(H.bar[r], TS_BTN_ACCENT, LV_PART_INDICATOR);
        }
    }

    lv_obj_t *hint_bar = th_box(root, TS_BAR);
    lv_obj_set_size(hint_bar, LV_PCT(100), 24);
    static const char *const hints[] = { "A", "Refresh", "B", "Back" };
    lv_obj_center(nz_hints(hint_bar, hints, 2));

    refresh();
    H.timer = lv_timer_create(tick, 2000, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (k == NZ_KEY_A) { refresh(); ui_toast("Refreshed"); return 1; }
    return 0;                                                 /* B: the app closes */
}

static void destroy(NzApp *a)
{
    (void)a;
    if (H.timer) lv_timer_del(H.timer);
    memset(&H, 0, sizeof H);
}

/* for tests */
const char *app_health_verdict(void) { return H.verdict; }
const char *app_health_value(int row) { return row >= 0 && row < R_COUNT && H.val[row] ? lv_label_get_text(H.val[row]) : ""; }

NzApp app_health = {
    .id = APP_HEALTH, .title = "Health", .icon = ICON_SYSTEM,
    .create = create, .key = key, .destroy = destroy,
};

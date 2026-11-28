/* app_car.c - the NZEUS Car app: live engine data, fault codes, self-tests,
 * graphs, logging, and read-only scans of supported OBD-II cars (reachable
 * systems only; Ford's names and extras on a Ford) with a PDF report, through
 * a USB OBD adapter (OBDLink EX, ELM327). There is no demo: without an
 * adapter it says so (and how to plug one in) and shows no made-up values.
 *
 * Safety (the car project's rules): read-only. Every message to the car goes
 * through obd.c's whitelist. The only change it can make is "Clear fault
 * codes" (OBD mode 04): asked twice, and the codes are saved to the SD card
 * first (nzeus/car/). No programming, no odometer, no emissions settings, no
 * module settings.
 *
 * Pages: gauges (start) - a list page (codes, self-tests, freeze frame,
 * vehicle info) - a graph - the scan's progress - the scan's report.
 *   X  menu        B  back (from any page to the gauges, from the gauges out) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "lvgl.h"
#include "apps.h"
#include "carlog.h"
#include "carscan.h"
#include "carsupport.h"
#include "bgjob.h"
#include "home.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "nzeus_tick.h"
#include "obd.h"
#include "serial.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define CAR_DIR NZ_DIR "/car"
/* the vehicle-support helper (core/deck-modern/nzeus-car-support);
 * the PC tests use the project's own copy instead */
#ifndef NZ_TESTING
#define SUPPORT_HELPER "/usr/local/lib/nzeus/deck-bin/nzeus-car-support"
#elif defined(NZ_REPO)
#define SUPPORT_HELPER NZ_REPO "/core/deck-modern/nzeus-car-support"
#else
#define SUPPORT_HELPER "./core/deck-modern/nzeus-car-support"
#endif

typedef struct { int pid; const char *caption; int lo, hi; const char *fmt; } Gauge;
static const Gauge gauges[] = {
    { PID_RPM, "RPM", 0, 7000, "%.0f" }, { PID_SPEED, "km/h", 0, 200, "%.0f" },
    { PID_COOLANT, "coolant C", 40, 120, "%.0f" }, { PID_LOAD, "engine load %", 0, 100, "%.0f" },
};
static const struct { int pid; const char *caption, *fmt; } stats[] = {
    { PID_THROTTLE, "Throttle", "%.0f %%" }, { PID_FUEL, "Fuel", "%.0f %%" }, { PID_VOLTS, "Battery", "%.1f V" },
};

/* more live values, in small tiles under the gauges (only the ones the car has) */
static const struct { int pid; const char *caption; } extras[] = {
    { PID_IAT, "Intake air" }, { PID_MAF, "Air flow" }, { PID_TIMING, "Timing" },
    { PID_OILTEMP, "Oil temp" }, { PID_STFT1, "Short trim 1" }, { PID_LTFT1, "Long trim 1" },
    { PID_STFT2, "Short trim 2" }, { PID_LTFT2, "Long trim 2" }, { PID_AMBIENT, "Outside air" },
    { PID_RUNTIME, "Running for" }, { PID_FUELRATE, "Fuel use" },   /* (trim = fuel the computer adds or takes away) */
};
#define EXTRAS ((int)(sizeof extras / sizeof extras[0]))

enum { PG_GAUGES, PG_LIST, PG_GRAPH, PG_SCAN, PG_REPORT, PG_HELP, PG_FEATURES, PG_COUNT };
/* what the list page shows (the first three = CODES_STORED / PENDING / PERMANENT) */
enum { V_STORED, V_PENDING, V_PERMANENT, V_READINESS, V_FREEZE, V_INFO };
#define CHART_POINTS 120              /* 30 seconds at 4 points a second */

static CarScan *scan;                 /* the last full health scan (68 KB: made at the first scan, then kept for its report) */
static int scan_have;                 /* 1 = scan holds a finished scan */

static struct {
    lv_obj_t *status, *rec, *hint_bar, *pages[PG_COUNT];
    lv_obj_t *no_adapter, *no_adapter_text;   /* the "No OBD adapter" box on the gauges page */
    lv_obj_t *arcs[4], *vals[4], *stat_vals[3];
    lv_obj_t *list_head;
    NzList *list;
    int page, view, list_filled;
    lv_obj_t *chart, *graph_title, *graph_vals, *axis_top, *axis_bottom;
    lv_obj_t *extra_tiles[PID_COUNT], *extra_vals[PID_COUNT];
    lv_chart_series_t *ser;
    int graph_pid, graph_n, chart_theme, scale;
    double gmin, gmax;
    lv_obj_t *scan_line, *scan_bar, *scan_count;
    lv_obj_t *report, *help, *features;
    int pdf_after_scan;               /* -1 = no automatic export; 0 English, 1 Arabic */
    int waiting_clear, scan_running;
    lv_timer_t *timer;
    char port[48];
    uint32_t reconnect_at;           /* bound retries when a port stays present but fails */
    BgJob support_job;
    int support_vehicle, support_year;
} C;

/* ---------- small helpers ---------- */
static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, int style)
{
    lv_obj_t *l = th_label(parent, text, style);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

/* the screen font has no "·" (the report's separator): show " - " instead */
static const char *screen_text(const char *s, char *out, size_t n)
{
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xB7) { out[k++] = '-'; s++; continue; }
        out[k++] = *s;
    }
    out[k] = 0;
    return out;
}

static lv_obj_t *page_box(lv_obj_t *root)
{
    lv_obj_t *p = th_box(root, -1);
    lv_obj_set_width(p, LV_PCT(100));
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static void set_hints(void)
{
    static const char *const h_gauges[] = { "A", "Report", "X", "Tools", "Y", "Guide", "B", "Back" };
    static const char *const h_list[] = { "UP/DOWN", "Move", "X", "Menu", "B", "Back" };
    static const char *const h_graph[] = { "LEFT/RIGHT", "Other value", "X", "Menu", "B", "Back" };
    static const char *const h_scan[] = { "B", "Stop the scan" };
    static const char *const h_report[] = { "UP/DOWN", "Scroll", "X", "Menu (PDF)", "B", "Back" };
    static const char *const h_help[] = { "UP/DOWN", "Scroll", "B", "Back" };
    static const char *const h_features[] = { "A", "Support actions", "UP/DOWN", "Scroll", "B", "Back" };
    lv_obj_clean(C.hint_bar);
    const char *const *h = h_gauges;
    int n = 4;
    switch (C.page) {
    case PG_LIST:   h = h_list; n = 3; break;
    case PG_GRAPH:  h = h_graph; n = 3; break;
    case PG_SCAN:   h = h_scan; n = 1; break;
    case PG_REPORT: h = h_report; n = 3; break;
    case PG_HELP: h = h_help; n = 2; break;
    case PG_FEATURES: h = h_features; n = 3; break;
    default: break;
    }
    lv_obj_center(nz_hints(C.hint_bar, h, n));
}

static void show_page(int p)
{
    C.page = p;
    for (int i = 0; i < PG_COUNT; i++) {
        if (i == p) lv_obj_clear_flag(C.pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(C.pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    obd_focus_pid(p == PG_GRAPH ? C.graph_pid : -1);       /* the graph's value: asked twice as often */
    set_hints();
}

/* ---------- the gauges ---------- */
/* one gauge: a 3/4 ring, the number inside it, what it is under it.
 * The ring is a square sized from the box's width, so it never stretches. */
static lv_obj_t *gauge(lv_obj_t *parent, int i, int size)
{
    lv_obj_t *box = th_box(parent, TS_PANEL);
    lv_obj_set_size(box, LV_PCT(24), size + 34);
    lv_obj_t *arc = C.arcs[i] = lv_arc_create(box);
    lv_obj_remove_style_all(arc);                          /* a plain arc: no knob, not clickable */
    th_add(arc, TS_ARC);
    th_add_to(arc, TS_ARC_IND, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, gauges[i].lo, gauges[i].hi);
    lv_obj_set_size(arc, size, size);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 8);
    C.vals[i] = th_label(arc, "--", TS_HEADING);          /* inside the ring: stays centred */
    lv_obj_center(C.vals[i]);
    lv_obj_t *cap = th_label(box, gauges[i].caption, TS_SMALL);   /* in the ring's open bottom */
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -8);
    return box;
}

static void show_values(void)
{
    const ObdLive *L = obd_live();
    char b[24];
    for (int i = 0; i < 4; i++) {
        int p = gauges[i].pid;
        if (L->have[p]) { snprintf(b, sizeof b, gauges[i].fmt, L->value[p]); lv_arc_set_value(C.arcs[i], (int16_t)L->value[p]); }
        else { snprintf(b, sizeof b, "--"); lv_arc_set_value(C.arcs[i], (int16_t)gauges[i].lo); }
        lv_label_set_text(C.vals[i], b);
    }
    for (int i = 0; i < 3; i++) {
        int p = stats[i].pid;
        if (L->have[p]) snprintf(b, sizeof b, stats[i].fmt, L->value[p]); else snprintf(b, sizeof b, "--");
        lv_label_set_text_fmt(C.stat_vals[i], "%s  %s", stats[i].caption, b);
    }
    for (int i = 0; i < EXTRAS; i++) {
        int p = extras[i].pid;
        if (!obd_pid_supported(p) || obd_state() != OBD_READY) { lv_obj_add_flag(C.extra_tiles[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(C.extra_tiles[i], LV_OBJ_FLAG_HIDDEN);
        if (!L->have[p]) snprintf(b, sizeof b, "--");
        else if (p == PID_RUNTIME) snprintf(b, sizeof b, "%d:%02d min", (int)L->value[p] / 60, (int)L->value[p] % 60);
        else {
            char v[16];
            double x = L->value[p] > -0.05 && L->value[p] < 0.05 ? 0 : L->value[p];   /* no "-0.0" */
            snprintf(v, sizeof v, obd_pid_format(p), x);
            snprintf(b, sizeof b, "%s %s", v, obd_pid_unit(p));
        }
        lv_label_set_text(C.extra_vals[i], b);
    }
}

static void show_status(void)
{
    lv_label_set_text(C.status, obd_describe());
    /* no adapter (or it stopped answering): say so in big letters, with how to fix it */
    if (obd_state() == OBD_OFF) {
        if (strcmp(lv_label_get_text(C.no_adapter_text), obd_describe())) lv_label_set_text(C.no_adapter_text, obd_describe());
        lv_obj_clear_flag(C.no_adapter, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(C.no_adapter, LV_OBJ_FLAG_HIDDEN);
    if (carlog_active()) lv_obj_clear_flag(C.rec, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(C.rec, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- connecting ---------- */
static void connect(void)
{
    C.reconnect_at = nzeus_tick_ms() + 5000;
    char ports[SERIAL_MAX_PORTS][48];
    int n = serial_list(ports, SERIAL_MAX_PORTS);
    if (n == 0) { obd_stop(); C.port[0] = 0; return; }    /* no adapter: "No OBD adapter - plug ..." (no demo) */
    snprintf(C.port, sizeof C.port, "%s", ports[0]);
    obd_start(C.port);
}

/* ---------- the list page: codes, self-tests, freeze frame, vehicle info ---------- */
static void fill_list(void)
{
    static const char *const heads[] = { "Fault codes (stored)", "Pending codes (seen once, not confirmed yet)",
        "Permanent codes (they clear themselves later)", "Self-tests (readiness)", "Freeze frame", "Vehicle info" };
    static const char *const none[] = { "No fault codes stored", "No pending codes", "No permanent codes" };
    char t[96], r[48];
    lv_label_set_text(C.list_head, heads[C.view]);
    nzlist_clear(C.list);
    C.list_filled = 0;
    if (C.view <= V_PERMANENT) {
        int k = C.view;
        if (!obd_codes_ready(k)) { nzlist_add(C.list, ICON_GAUGE, "Reading fault codes...", NULL, NULL); return; }
        C.list_filled = 1;
        if (obd_codes_count(k) == 0) nzlist_add(C.list, ICON_GAUGE, none[k], "No codes returned in this category; other systems may differ", NULL);
        for (int i = 0; i < obd_codes_count(k); i++) {
            const char *m = obd_dtc_meaning(obd_code(k, i));
            nzlist_add(C.list, ICON_GAUGE, obd_code(k, i), m ? m : "look this code up for your car's model", NULL);
        }
        return;
    }
    if (!obd_info_ready()) { nzlist_add(C.list, ICON_GAUGE, "Asking the car...", NULL, NULL); return; }
    C.list_filled = 1;
    const ObdInfo *I = obd_info();
    if (C.view == V_READINESS) {
        if (!I->have_status) { nzlist_add(C.list, ICON_GAUGE, "No answer from the engine computer", "ignition on?", NULL); return; }
        int done = 0;
        for (int i = 0; i < I->mon_count; i++) done += I->mon[i].complete;
        snprintf(t, sizeof t, "Check-engine light %s", I->mil_on ? "ON" : "off");
        snprintf(r, sizeof r, "%d code%s stored", I->dtc_count, I->dtc_count == 1 ? "" : "s");
        nzlist_add(C.list, ICON_GAUGE, t, r, NULL);
        snprintf(t, sizeof t, "%d of %d self-tests done", done, I->mon_count);
        nzlist_add(C.list, ICON_GAUGE, t, I->compression ? "diesel engine tests" : "petrol engine tests", NULL);
        for (int i = 0; i < I->mon_count; i++)
            nzlist_add(C.list, -1, I->mon[i].name, NULL, I->mon[i].complete ? "Done" : "Not yet");
    } else if (C.view == V_FREEZE) {
        if (!I->have_ff) { nzlist_add(C.list, ICON_GAUGE, "No freeze frame stored", "the engine saves one when it confirms a fault", NULL); return; }
        const char *m = obd_dtc_meaning(I->ff_dtc);
        snprintf(t, sizeof t, "Frozen by %s", I->ff_dtc);
        nzlist_add(C.list, ICON_GAUGE, t, m ? m : "the values at the moment the fault was stored", NULL);
        static const int pid_of[FF_COUNT] = { PID_RPM, PID_SPEED, PID_COOLANT, PID_LOAD };
        for (int i = 0; i < FF_COUNT; i++) {
            if (I->ff_have[i]) { snprintf(r, sizeof r, obd_pid_format(pid_of[i]), I->ff_value[i]); strncat(r, " ", sizeof r - strlen(r) - 1); strncat(r, obd_pid_unit(pid_of[i]), sizeof r - strlen(r) - 1); }
            else snprintf(r, sizeof r, "--");
            nzlist_add(C.list, -1, obd_pid_name(pid_of[i]), NULL, r);
        }
    } else {
        char sub[96];
        snprintf(sub, sizeof sub, "VIN (chassis number)");
        nzlist_add(C.list, ICON_CAR, I->vin[0] ? I->vin : "not given", sub, NULL);
        nzlist_add(C.list, -1, I->calid[0] ? I->calid : "not given", "Calibration ID (engine software)", NULL);
        nzlist_add(C.list, -1, I->ecu_name[0] ? I->ecu_name : "not given", "Engine computer's name", NULL);
        nzlist_add(C.list, -1, obd_adapter(), "Adapter", NULL);
        nzlist_add(C.list, -1, obd_protocol_name(), "Protocol", NULL);
    }
}

static void open_view(int v)
{
    C.view = v;
    if (v <= V_PERMANENT) obd_read_codes(v);
    else obd_read_info();
    fill_list();
    show_page(PG_LIST);
}

/* ---------- clearing codes (the only change NZEUS can make to a car) ---------- */
static int save_codes(void)
{
    char path[128], when[32];
    time_t now = time(NULL);
    mkdir(NZ_DIR, 0755);
    mkdir(CAR_DIR, 0755);
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof path, CAR_DIR "/codes-%s.txt", when);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "Fault codes read %s (%s)\n", when, obd_describe());
    for (int i = 0; i < obd_dtc_count(); i++) {
        const char *m = obd_dtc_meaning(obd_dtc(i));
        fprintf(f, "%s  %s\n", obd_dtc(i), m ? m : "");
    }
    fclose(f);
    return 0;
}

static void clear_sure(int yes, void *ctx)
{
    (void)ctx;
    if (!yes) return;
    if (save_codes() != 0) { ui_toast("Could not save a copy of the codes - not clearing"); return; }
    obd_clear_dtcs();
    C.waiting_clear = 1;
    ui_toast("Copy saved in nzeus/car. Clearing...");
}

static void clear_first(int yes, void *ctx)
{
    (void)ctx;
    if (yes) nz_confirm("Really clear them? A copy is saved first.", clear_sure, NULL);
}

static void ask_clear(void)
{
    if (!obd_dtcs_ready()) { ui_toast("Read the fault codes first"); return; }
    if (obd_dtc_count() == 0) { ui_toast("There are no fault codes to clear"); return; }
    nz_confirm("Clear the fault codes? The engine light goes off and the car's self-tests start "
               "again (an inspection may need a drive first). Engine OFF, ignition ON.", clear_first, NULL);
}

/* ---------- the graph ---------- */
static void graph_colours(void)
{
    const NzTheme *t = theme();
    lv_chart_set_series_color(C.chart, C.ser, th_color(t->accent));
    lv_obj_set_style_line_color(C.chart, th_color(t->border), LV_PART_MAIN);   /* the grid */
    C.chart_theme = theme_index();
}

/* the top and bottom of the scale, small, inside the chart's corners */
static void graph_scale_labels(double lo, double hi)
{
    char t[24];
    const char *f = obd_pid_format(C.graph_pid);
    snprintf(t, sizeof t, f, hi);
    lv_label_set_text(C.axis_top, t);
    snprintf(t, sizeof t, f, lo);
    lv_label_set_text(C.axis_bottom, t);
}

static void graph_set(int pid)
{
    double lo, hi;
    C.graph_pid = pid;
    obd_pid_range(pid, &lo, &hi);
    C.scale = hi - lo <= 60 ? 10 : 1;                       /* small ranges: tenths, so the line is smooth */
    C.gmin = 1e9; C.gmax = -1e9;
    C.graph_n = 0;
    lv_chart_set_range(C.chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(lo * C.scale), (lv_coord_t)(hi * C.scale));
    lv_chart_set_all_value(C.chart, C.ser, LV_CHART_POINT_NONE);
    lv_label_set_text_fmt(C.graph_title, "%s (%s)", obd_pid_name(pid), obd_pid_unit(pid));
    lv_label_set_text(C.graph_vals, "waiting for the first value...");
    graph_scale_labels(lo, hi);
    obd_focus_pid(C.page == PG_GRAPH ? pid : -1);
}

static void graph_step(int dir)                             /* the next value the car has */
{
    int p = C.graph_pid;
    for (int k = 0; k < PID_COUNT; k++) {
        p = (p + dir + PID_COUNT) % PID_COUNT;
        if (obd_pid_supported(p)) { graph_set(p); return; }
    }
}

static void graph_tick(void)
{
    if (C.chart_theme != theme_index()) graph_colours();
    const ObdLive *L = obd_live();
    int p = C.graph_pid;
    if (!L->have[p]) return;
    double v = L->value[p], lo, hi;
    if (v < C.gmin) C.gmin = v;
    if (v > C.gmax) C.gmax = v;
    C.graph_n++;
    obd_pid_range(p, &lo, &hi);                             /* the usual range, wider if the car goes past it */
    if (C.gmin < lo) lo = C.gmin;
    if (C.gmax > hi) hi = C.gmax;
    double top = hi * C.scale, bottom = lo * C.scale;
    if (top > 32000) top = 32000;
    if (bottom < -32000) bottom = -32000;
    lv_chart_set_range(C.chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)bottom, (lv_coord_t)top);
    graph_scale_labels(lo, hi);
    double sv = v * C.scale;
    lv_chart_set_next_value(C.chart, C.ser, (lv_coord_t)(sv > 32000 ? 32000 : sv < -32000 ? -32000 : sv));
    char now[24], mn[24], mx[24];
    const char *f = obd_pid_format(p);
    snprintf(now, sizeof now, f, v);
    snprintf(mn, sizeof mn, f, C.gmin);
    snprintf(mx, sizeof mx, f, C.gmax);
    lv_label_set_text_fmt(C.graph_vals, "now %s    min %s    max %s   %s", now, mn, mx, obd_pid_unit(p));
}

static void build_graph(lv_obj_t *page)
{
    lv_obj_set_style_pad_all(page, 8, 0);
    lv_obj_set_style_pad_row(page, 4, 0);
    C.graph_title = th_label(page, "", TS_HEADING);
    C.graph_vals = th_label(page, "", TS_TEXT);
    C.chart = lv_chart_create(page);
    lv_obj_remove_style_all(C.chart);                       /* our theme, not LVGL's */
    th_add(C.chart, TS_PANEL);
    lv_obj_set_width(C.chart, LV_PCT(100));
    lv_obj_set_flex_grow(C.chart, 1);
    lv_obj_set_style_pad_all(C.chart, 6, 0);
    lv_obj_set_style_line_width(C.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(C.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(C.chart, 0, LV_PART_INDICATOR);  /* a line, no dots: cheap to draw */
    lv_chart_set_type(C.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(C.chart, CHART_POINTS);
    lv_chart_set_update_mode(C.chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(C.chart, 5, 0);
    C.ser = lv_chart_add_series(C.chart, th_color(theme()->accent), LV_CHART_AXIS_PRIMARY_Y);
    C.axis_top = th_label(C.chart, "", TS_SMALL);
    lv_obj_align(C.axis_top, LV_ALIGN_TOP_LEFT, 2, 0);
    C.axis_bottom = th_label(C.chart, "", TS_SMALL);
    lv_obj_align(C.axis_bottom, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    graph_colours();
    graph_set(PID_RPM);
}

/* ---------- logging ---------- */
static void log_chosen(int c, void *ctx)
{
    (void)ctx;
    static const int basics[] = { PID_RPM, PID_SPEED, PID_COOLANT, PID_LOAD, PID_VOLTS };
    int list[PID_COUNT], n = 0;
    if (c < 0) return;
    if (c == 0) { for (int i = 0; i < PID_COUNT; i++) if (obd_pid_supported(i)) list[n++] = i; }
    else for (int i = 0; i < 5; i++) list[n++] = basics[i];
    if (carlog_start(CAR_DIR, list, n) != 0) { ui_toast("Could not create the log file on the SD card"); return; }
    char msg[200];
    const char *name = strrchr(carlog_path(), '/');
    snprintf(msg, sizeof msg, "Logging to nzeus/car/%s", name ? name + 1 : carlog_path());
    ui_toast(msg);
    show_status();
}

static void toggle_logging(void)
{
    if (carlog_active()) {
        char msg[96];
        int rows = carlog_rows();
        carlog_stop();
        snprintf(msg, sizeof msg, "Log saved: %d rows", rows);
        ui_toast(msg);
        show_status();
        return;
    }
    static const char *const items[] = { "Everything the car reports", "Engine basics (RPM, speed, coolant, load, battery)" };
    nz_menu("Log which values?", items, 2, log_chosen, NULL);
}

/* ---------- the full health scan ---------- */
static void report_heading(const char *text)
{
    lv_obj_t *h = th_label(C.report, text, TS_HEADING);
    lv_obj_set_style_pad_top(h, 8, 0);
}

static void report_fact(lv_obj_t *grid, const char *caption, const char *value)
{
    lv_obj_t *b = th_box(grid, TS_PANEL);
    lv_obj_set_size(b, LV_PCT(32), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, 6, 0);
    th_label(b, caption, TS_SMALL);
    wrap_label(b, value, TS_TEXT);
}

static const char *state_word(int st)
{
    switch (st) {
    case DTC_ACTIVE:    return "ACTIVE now";
    case DTC_PENDING:   return "Pending";
    case DTC_PERMANENT: return "Permanent";
    case DTC_STORED:    return "Stored";
    default:            return "Not present";
    }
}

static void build_report(void)
{
    CarScan *s = scan;
    char b[200];
    lv_obj_clean(C.report);
    lv_obj_scroll_to_y(C.report, 0, LV_ANIM_OFF);

    lv_obj_t *head = th_box(C.report, s->healthy ? TS_BTN_ACCENT : TS_BTN_DANGER);   /* the verdict */
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(head, 8, 0);
    lv_obj_t *hl = lv_label_create(head);                  /* (text colour from the box: on-accent / on-danger) */
    lv_label_set_text(hl, s->headline_en);
    lv_label_set_long_mode(hl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hl, LV_PCT(100));
    lv_obj_set_style_text_font(hl, theme()->font_heading, 0);
    wrap_label(C.report, s->summary_en, TS_TEXT);

    lv_obj_t *grid = th_box(C.report, -1);                  /* the facts, three to a row */
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    snprintf(b, sizeof b, "%.63s%s%.47s", s->make_model_en[0] ? s->make_model_en : "unknown",
             s->engine_en[0] ? ", " : "", s->engine_en);
    report_fact(grid, "Vehicle", b);
    if (s->model_year) snprintf(b, sizeof b, "%d", s->model_year); else snprintf(b, sizeof b, "--");
    report_fact(grid, "Model year (from VIN)", b);
    report_fact(grid, "VIN", s->vin[0] ? s->vin : "--");
    if (s->odometer_km >= 0) snprintf(b, sizeof b, "%.0f km", s->odometer_km); else snprintf(b, sizeof b, "--");
    report_fact(grid, "Odometer", b);
    snprintf(b, sizeof b, "%d", s->modules_answered);
    report_fact(grid, "Modules found", b);
    if (s->battery_v > 0) snprintf(b, sizeof b, "%.1f V", s->battery_v); else snprintf(b, sizeof b, "--");
    report_fact(grid, s->engine_running ? "Battery (engine running)" : "Battery (ignition on)", b);

    /* the check-engine light and the engine's self-tests */
    if (s->mil_known) {
        report_heading("Check-engine light and self-tests");
        snprintf(b, sizeof b, "Check-engine light %s.%s%.150s", s->mil_on ? "ON" : "off",
                 s->readiness_en[0] ? " " : "", s->readiness_en);
        lv_obj_t *l = wrap_label(C.report, b, TS_TEXT);
        if (s->mil_on) th_add(l, TS_TITLE);
    }

    if (s->cause_en[0]) {
        lv_obj_t *c = th_box(C.report, TS_PANEL);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 8, 0);
        lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(c, 4, 0);
        lv_obj_set_style_border_color(c, th_color(theme()->accent), 0);
        wrap_label(c, s->cause_en, TS_TEXT);
    }
    if (s->step_count) {
        report_heading("What to do");
        for (int i = 0; i < s->step_count; i++) {
            snprintf(b, sizeof b, "%d. ", i + 1);
            lv_obj_t *l = wrap_label(C.report, b, TS_TEXT);
            lv_label_set_text_fmt(l, "%d. %s", i + 1, s->steps_en[i]);
        }
    }

    snprintf(b, sizeof b, "Fault codes (%d)", s->dtc_count);
    report_heading(b);
    if (!s->dtc_count) wrap_label(C.report, "No fault codes were returned. Unread systems remain unknown.", TS_MUTED);
    /* grouped like the report: each group's heading, then its codes */
    for (int i = 0; i < s->dtc_count; i++) {
        int first = 1;
        for (int k = 0; k < i; k++) if (!strcmp(s->dtcs[k].group_en, s->dtcs[i].group_en)) first = 0;
        if (!first) continue;
        char gname[64];
        lv_obj_t *g = wrap_label(C.report, screen_text(s->dtcs[i].group_en, gname, sizeof gname), TS_TITLE);
        lv_obj_set_style_pad_top(g, 4, 0);
        for (int k = i; k < s->dtc_count; k++) {
            const ScanDtc *d = &s->dtcs[k];
            if (strcmp(d->group_en, s->dtcs[i].group_en)) continue;
            lv_obj_t *row = th_box(C.report, -1);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_set_width(th_label(row, d->module, TS_SMALL), 52);
            lv_obj_set_width(th_label(row, d->code, TS_TEXT), 60);
            lv_obj_t *m = th_label(row, d->meaning_en[0] ? d->meaning_en : "(no description yet - look this code up)", TS_TEXT);
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
            lv_obj_set_flex_grow(m, 1);
            lv_obj_t *st = th_label(row, state_word(d->state), TS_KEYCAP);
            if (d->state == DTC_ACTIVE) th_add(st, TS_BTN_DANGER);
        }
    }

    report_heading("Mileage check");
    if (s->odo_agree == 1) {
        b[0] = 0;
        for (int i = 0; i < s->odo_count; i++)
            snprintf(b + strlen(b), sizeof b - strlen(b), "%s%s", i == 0 ? "" : i == s->odo_count - 1 ? " and " : ", ", s->odo[i].module);
        lv_label_set_text_fmt(wrap_label(C.report, "", TS_TEXT), "%s each store %.0f km. These readings agree; this alone does not prove the mileage has never been changed.", b, s->odometer_km);
    } else if (s->odo_agree == 0) {
        lv_obj_t *l = wrap_label(C.report, "", TS_TEXT);
        char t[300] = "The modules do NOT agree:";
        for (int i = 0; i < s->odo_count; i++) snprintf(t + strlen(t), sizeof t - strlen(t), " %s %.0f km%s", s->odo[i].module, s->odo[i].km, i < s->odo_count - 1 ? "," : ".");
        lv_label_set_text(l, t);
        th_add(l, TS_TITLE);
    } else if (s->odo_count == 1) {
        lv_label_set_text_fmt(wrap_label(C.report, "", TS_TEXT), "Only %s gave the odometer: %.0f km. Not enough to compare.",
                              s->odo[0].module, s->odo[0].km);
    } else wrap_label(C.report, "No module gave its odometer to compare.", TS_MUTED);

    /* the live values at the time of the scan, two to a row */
    if (s->live_count) {
        snprintf(b, sizeof b, "Live values at the scan (%d)", s->live_count);
        report_heading(b);
        lv_obj_t *lv = th_box(C.report, -1);
        lv_obj_set_size(lv, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(lv, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(lv, 2, 0);
        for (int i = 0; i < s->live_count && i < SCAN_MAX_LIVE; i++) {
            lv_obj_t *cell = th_box(lv, -1);
            lv_obj_set_size(cell, LV_PCT(50), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(cell, 8, 0);
            lv_obj_t *n = th_label(cell, s->live[i].name_en, TS_TEXT);
            lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
            lv_obj_set_width(n, LV_PCT(62));
            th_label(cell, s->live[i].value, TS_TITLE);
        }
    }

    snprintf(b, sizeof b, "Modules found (%d)", s->modules_answered);
    report_heading(b);
    lv_obj_t *mods = th_box(C.report, -1);
    lv_obj_set_size(mods, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mods, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(mods, 2, 0);
    for (int i = 0; i < s->module_count; i++) {
        if (!s->modules[i].answered) continue;
        lv_obj_t *cell = th_box(mods, -1);
        lv_obj_set_size(cell, LV_PCT(50), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(cell, 8, 0);
        lv_obj_set_width(th_label(cell, s->modules[i].code, TS_TITLE), 66);    /* "BdyCM" fits on one line */
        th_label(cell, s->modules[i].name_en, TS_TEXT);
    }
    if (s->wish_count) {
        report_heading("Wishlist: first look");
        wrap_label(C.report, "Based only on which modules the car has. Each item still needs checking in FORScan on the laptop.", TS_SMALL);
        static const char *const outlook[] = { "To verify", "Commonly possible", "Possible, safety feature", "Unknown" };
        for (int i = 0; i < s->wish_count; i++) {
            lv_obj_t *row = th_box(C.report, -1);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_t *f = th_label(row, s->wish[i].feature_en, TS_TEXT);
            lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
            lv_obj_set_flex_grow(f, 1);
            lv_obj_t *m = th_label(row, s->wish[i].module_en, TS_SMALL);
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(m, LV_PCT(36));
            th_label(row, outlook[s->wish[i].outlook & 3], TS_KEYCAP);
        }
    }
    if (s->note_count) {                                    /* how the scan went, what it could not reach */
        report_heading("Notes");
        for (int i = 0; i < s->note_count && i < SCAN_MAX_NOTES; i++)
            lv_label_set_text_fmt(wrap_label(C.report, "", TS_SMALL), "- %s", s->notes_en[i]);
    }
    char when[40];
    strftime(when, sizeof when, "%d %b %Y, %H:%M", localtime(&s->when));
    lv_label_set_text_fmt(wrap_label(C.report, "", TS_SMALL), "Scanned %s - %s - %s - read only, no changes made.",
                          when, s->adapter, s->engine_running ? "engine running" : "ignition on");
}

static void pdf_chosen(int c, void *ctx);

static int start_scan(void)
{
    if (!scan) scan = calloc(1, sizeof *scan);              /* (only once a scan is wanted) */
    if (!scan) { ui_toast("Not enough memory for the scan - close other apps"); return 0; }
    if (obd_state() == OBD_OFF) { ui_toast(obd_describe()); return 0; }   /* no adapter: nothing to scan (no demo) */
    if (obd_state() != OBD_READY) { ui_toast("Connect to the car first (ignition on)"); return 0; }
    if (carlog_active()) { carlog_stop(); ui_toast("Logging stopped for the scan"); }
    scan_have = 0;
    carscan_start(scan);
    C.scan_running = 1;
    lv_bar_set_value(C.scan_bar, 0, LV_ANIM_OFF);
    show_page(PG_SCAN);
    return 1;
}

static void scan_tick(void)
{
    int done, total;
    char line[120];
    carscan_progress(&done, &total, line, sizeof line);
    lv_label_set_text(C.scan_line, line);
    lv_bar_set_range(C.scan_bar, 0, total);
    lv_bar_set_value(C.scan_bar, done, LV_ANIM_OFF);
    int answered = 0;
    for (int i = 0; i < scan->module_count; i++) answered += scan->modules[i].answered;
    lv_label_set_text_fmt(C.scan_count, "Modules found so far: %d    Fault codes so far: %d", answered, scan->dtc_count);
    if (carscan_state() == CS_DONE) {
        C.scan_running = 0;
        scan_have = 1;
        if (carsupport_save(scan, CAR_DIR))
            ui_toast("Scan complete; could not save vehicle discovery. Check SD space.");
        build_report();
        show_page(PG_REPORT);
        if (C.pdf_after_scan >= 0) {
            int language = C.pdf_after_scan;
            C.pdf_after_scan = -1;
            pdf_chosen(language, NULL);
        }
    } else if (carscan_state() == CS_CANCELLED) {
        C.scan_running = 0;
        C.pdf_after_scan = -1;
        ui_toast("The scan stopped (the adapter or the car stopped answering)");
        show_page(PG_GAUGES);
    }
}

static void build_scan_page(lv_obj_t *page)
{
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 12, 0);
    th_label(page, "Scan available systems", TS_HEADING);
    C.scan_line = wrap_label(page, "", TS_TEXT);
    C.scan_bar = lv_bar_create(page);
    lv_obj_remove_style_all(C.scan_bar);
    th_add(C.scan_bar, TS_PANEL);
    th_add_to(C.scan_bar, TS_BTN_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_size(C.scan_bar, LV_PCT(100), 18);
    C.scan_count = wrap_label(page, "", TS_MUTED);
    wrap_label(page, "Keep the ignition on. This reads standard OBD-II results and searches for "
                     "other systems where supported. Some systems may be unavailable or locked. "
                     "The report explains missing results. No car settings are changed.", TS_SMALL);
}

/* ---------- the PDF report ---------- */
static void pdf_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c < 0 || !scan_have) return;
    char out[256];
    mkdir(NZ_DIR, 0755);
    mkdir(CAR_DIR, 0755);
    ui_toast("Making the PDF report...");           /* it takes a few seconds: show this first */
    lv_refr_now(NULL);
    if (carreport_pdf(scan, c == 0 ? "en" : "ar", CAR_DIR, out, sizeof out) == 0) {
        char msg[300];
        const char *name = strrchr(out, '/');
        snprintf(msg, sizeof msg, "Saved nzeus/car/%s", name ? name + 1 : out);
        ui_toast(msg);
    } else ui_toast("Could not make the PDF (is the SD card full?)");
}

static void scan_pdf_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c < 0) return;
    C.pdf_after_scan = -1;
    if (start_scan()) C.pdf_after_scan = c;
}

static void ask_new_pdf(void)
{
    static const char *const items[] = { "English", "Arabic" };
    if (obd_state() == OBD_OFF) { ui_toast(obd_describe()); return; }
    if (obd_state() != OBD_READY) { ui_toast("Connect to the car first (ignition on)"); return; }
    nz_menu("Scan and save PDF - language", items, 2, scan_pdf_chosen, NULL);
}

static void report_action_chosen(int c, void *ctx)
{
    (void)ctx;
    static const char *const languages[] = { "English", "Arabic" };
    if (c == 0) ask_new_pdf();
    else if (c == 1 && scan_have) nz_menu("Save last scan as PDF", languages, 2, pdf_chosen, NULL);
}

static void ask_pdf(void)
{
    /* Never silently export an older car's scan as if it were a fresh scan. */
    static const char *const items[] = { "New scan and PDF...", "Save last scan..." };
    if (scan_have) nz_menu("Make report", items, 2, report_action_chosen, NULL);
    else ask_new_pdf();
}

/* Small, readable cards: no jargon without its meaning, no pretend switches. */
static void help_card(lv_obj_t *page, const char *title, const char *text)
{
    lv_obj_t *card = th_box(page, TS_PANEL);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 5, 0);
    wrap_label(card, title, TS_TITLE);
    nz_button_text(wrap_label(card, "", TS_TEXT), text);
}

static lv_obj_t *help_page(lv_obj_t *root, const char *title)
{
    lv_obj_t *page = page_box(root);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(page, 10, 0);
    lv_obj_set_style_pad_row(page, 8, 0);
    wrap_label(page, title, TS_HEADING);
    return page;
}

static void support_text(const char *text)
{
    lv_obj_clean(C.features);
    wrap_label(C.features, "Car features and support", TS_HEADING);
    help_card(C.features, "Feature changes: not available yet",
              "There are no verified profiles for changing settings in this version. Owner guides explain the car's own menus.");
    help_card(C.features, "Vehicle support", text);
    help_card(C.features, "One visit: save vehicle details", "Every completed scan saves a private vehicle-details file in nzeus/car, "
              "including responding computers and available software identifiers. Keep the English and Arabic PDFs too. "
              "Missing or locked computers remain unknown. This does not guarantee we can enable a feature afterward.");
    lv_obj_scroll_to_y(C.features, 0, LV_ANIM_OFF);
}

static void support_run(const char *action)
{
    if (bgjob_running(&C.support_job)) { ui_toast("Vehicle support is still working"); return; }
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "timeout -k 2 45 python3 '%s' --root '%s/support' %s", SUPPORT_HELPER, CAR_DIR, action);
    support_text("Reading verified support data... {back} returns to gauges; {accept} opens support actions.");
    if (bgjob_start(&C.support_job, cmd, "/tmp/nzeus-car-support.log")) support_text("Could not start the support manager.");
    show_page(PG_FEATURES);
}

static void support_market(int choice, void *ctx)
{
    (void)ctx;
    static const char *const makes[] = { "Chevrolet", "Ford", "Land Rover" };
    static const char *const models[] = { "Silverado", "Ranger", "Evoque" };
    static const char *const markets[] = { "GCC", "US", "EU", "Other" };
    if (choice < 0 || choice > 3) return;
    char action[160];
    snprintf(action, sizeof action, "select '%s' '%s' %d %s", makes[C.support_vehicle],
             models[C.support_vehicle], C.support_year, markets[choice]);
    support_run(action);
}

static void support_year(int choice, void *ctx)
{
    (void)ctx;
    if (choice < 0) return;
    C.support_year = 2026 - choice;
    static const char *const markets[] = { "GCC / Gulf", "United States", "Europe", "Other / unknown" };
    nz_menu("Vehicle market", markets, 4, support_market, NULL);
}

static void support_vehicle(int choice, void *ctx)
{
    (void)ctx;
    static const char *const years[] = { "2026", "2025", "2024", "2023", "2022", "2021", "2020", "2019", "2018",
                                        "2017", "2016", "2015", "2014", "2013", "2012", "2011", "2010" };
    if (choice < 0 || choice > 2) return;
    C.support_vehicle = choice;
    nz_menu("Model year", years, choice == 0 ? 17 : 7, support_year, NULL);
}

static void support_action(int choice, void *ctx)
{
    (void)ctx;
    static const char *const vehicles[] = { "Chevrolet Silverado", "Ford Ranger", "Land Rover Evoque" };
    if (choice == 0) nz_menu("Choose vehicle", vehicles, 3, support_vehicle, NULL);
    else if (choice == 1) support_run("refresh");
    else if (choice == 2) support_run("view");
    else if (choice == 3) {
        if (!scan_have) ui_toast("Make a new scan first; it saves discovery automatically");
        else ui_toast(carsupport_save(scan, CAR_DIR) ? "Could not save discovery" : "Vehicle discovery saved in nzeus/car");
    }
}

static void support_menu(void)
{
    static const char *const actions[] = { "Choose vehicle...", "Download guide packs", "View installed support", "Save vehicle details" };
    nz_menu("Vehicle support actions", actions, 4, support_action, NULL);
}

static void build_help(lv_obj_t *root)
{
    C.help = C.pages[PG_HELP] = help_page(root, "Car guide");
    help_card(C.help, "1. Connect", "Plug OBDLink EX into the car's diagnostic socket and the console's USB/OTG port. "
              "Select Settings > USB > Devices, then turn the car's ignition on. Park before using the console.");
    help_card(C.help, "2. Make a report", "Press {accept} on the live screen. Choose a language to scan and save a PDF. "
              "If a previous scan exists, choose New scan and PDF or Save last scan. Files are saved in nzeus/car on the SD card.");
    help_card(C.help, "3. Understand the results", "A module is a car computer, such as the engine or ABS computer. "
              "A fault code is a clue reported by that computer, not proof that a particular part must be replaced. "
              "Unavailable or unread systems remain unknown. A report cannot guarantee the whole car is healthy.");
    help_card(C.help, "Live readings", "RPM is engine speed. Coolant is the temperature of the engine's cooling fluid. "
              "Engine load is how hard the engine is working. Battery shows voltage reported by the adapter. "
              "Fuel trim is the fuel adjustment made by the engine computer. -- means no reading has arrived.");
    help_card(C.help, "Codes and self-tests", "Stored: a confirmed code; it may still be present. Pending: a possible fault not yet confirmed. "
              "Permanent: an emissions code the car clears after its tests pass. Readiness: whether emissions self-tests have finished. "
              "Freeze frame: readings saved when a fault was recorded. VIN: the vehicle identification number.");
    help_card(C.help, "Logging: on or off", "X > Start logging records readings to a CSV spreadsheet on the SD card. "
              "X > Stop logging ends the recording. REC means recording is on. Logging does not change any car feature.");
    help_card(C.help, "Car feature changes", "Actual on/off settings, such as auto-lock or lighting behavior, require a verified support package "
              "for the exact vehicle and its computers. Open X > Car features for the current support status.");
    help_card(C.help, "Clearing codes", "This erases diagnostic evidence and can reset emissions self-tests. It does not repair the fault. "
              "Keep a report first. NZEUS asks twice and saves a code backup before sending the clear request.");
    C.features = C.pages[PG_FEATURES] = help_page(root, "Car features");
    help_card(C.features, "Feature changes: not available yet", "NZEUS currently scans and makes reports. It cannot yet turn actual car features on or off. "
              "The feature wishlist in a report is research guidance, not confirmation that a setting is supported or enabled.");
    help_card(C.features, "Why a setting may be unavailable", "The car's model, year, market and computer software must match a verified support package. "
              "Some cars also need different diagnostic equipment or licensed access. Finding a car computer does not prove a feature can be changed.");
    help_card(C.features, "What the adapter provides", "OBDLink EX carries diagnostic messages. It does not supply the vehicle-specific "
              "meaning of configuration bytes. A report can still be made when feature changes are unavailable.");
}

/* ---------- the X menu ---------- */
static void more_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c == 0) open_view(V_PENDING);
    else if (c == 1) open_view(V_PERMANENT);
    else if (c == 2) open_view(V_READINESS);
    else if (c == 3) open_view(V_FREEZE);
}

static void menu_chosen(int c, void *ctx)
{
    (void)ctx;
    static const char *const more[] = { "Pending codes", "Permanent codes", "Self-tests (readiness)", "Freeze frame" };
    switch (c) {
    case 0: open_view(V_STORED); break;
    case 1: nz_menu("More codes and tests", more, 4, more_chosen, NULL); break;
    case 2: open_view(V_INFO); break;
    case 3: graph_set(C.graph_pid); show_page(PG_GRAPH); break;
    case 4: toggle_logging(); break;
    case 5: C.pdf_after_scan = -1; start_scan(); break;
    case 6: ask_pdf(); break;
    case 7: ask_clear(); break;
    case 8: connect(); break;
    case 9: show_page(PG_HELP); break;
    case 10: support_run("view"); break;
    default: break;
    }
}

static void open_menu(void)
{
    const char *items[] = { "Read fault codes", "More codes and tests...", "Vehicle info (VIN)", "Graph a live value",
                            carlog_active() ? "Stop logging" : "Start logging to SD card", "Scan available systems",
                            "Make PDF report...", "Clear fault codes...", "Connect again", "Guide: what things mean", "Features / vehicle packs" };
    nz_menu("Car tools", items, 11, menu_chosen, NULL);
}

/* ---------- every 250 ms ---------- */
static void tick_timer(lv_timer_t *t)
{
    (void)t;
    if (obd_state() == OBD_OFF) {
        char ports[SERIAL_MAX_PORTS][48];
        int n = serial_list(ports, SERIAL_MAX_PORTS);
        if (n == 0) C.port[0] = 0;                           /* unplugged: plugging it in again connects by itself */
        else if (!C.port[0] || (int32_t)(nzeus_tick_ms() - C.reconnect_at) >= 0) {
            fprintf(stderr, "CAR_RECONNECT port-present\n");
            connect();                                      /* includes a transient failure on the same port */
        }
    }
    if (C.waiting_clear && obd_clear_result()) {
        C.waiting_clear = 0;
        ui_toast(obd_clear_result() > 0 ? "Fault codes cleared" : "The car refused to clear the codes");
        C.view = V_STORED;
        obd_read_codes(CODES_STORED);
        fill_list();
        show_page(PG_LIST);
    }
    show_status();
    switch (C.page) {
    case PG_GAUGES: show_values(); break;
    case PG_LIST:   if (!C.list_filled) fill_list(); break;     /* the answer came in */
    case PG_GRAPH:  graph_tick(); break;
    case PG_SCAN:   scan_tick(); break;
    default: break;
    }
}

/* obd_poll and the scan run every frame (the adapter answers quickly) */
static void tick(NzApp *a)
{
    (void)a;
    obd_poll();
    carscan_poll();
    carlog_tick();
    if (bgjob_running(&C.support_job) && !bgjob_poll(&C.support_job)) {
        char text[12000];
        bgjob_read(&C.support_job, text, sizeof text);
        support_text(text[0] ? text : "No support manager response. Previous packs were kept.");
    }
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&C, 0, sizeof C);
    C.pdf_after_scan = -1;
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = th_box(root, TS_BAR);                  /* what we're connected to */
    lv_obj_set_size(card, LV_PCT(100), 30);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_CAR, 20));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    C.status = th_label(card, "", TS_SMALL);
    lv_label_set_long_mode(C.status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(C.status, LV_PCT(70));
    lv_obj_align(C.status, LV_ALIGN_LEFT_MID, 36, 0);
    C.rec = th_label(card, "REC", TS_KEYCAP);              /* logging to the SD card */
    th_add(C.rec, TS_BTN_DANGER);
    lv_obj_align(C.rec, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_t *gp = C.pages[PG_GAUGES] = th_box(root, -1);    /* gauges + small values */
    lv_obj_set_width(gp, LV_PCT(100));
    lv_obj_set_flex_grow(gp, 1);
    lv_obj_set_flex_flow(gp, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(gp, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(gp, 8, 0);
    lv_obj_set_style_pad_row(gp, 8, 0);
    /* no adapter: a clear box above the gauges (hidden once an adapter answers) */
    C.no_adapter = th_box(gp, TS_PANEL);
    lv_obj_set_size(C.no_adapter, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(C.no_adapter, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(C.no_adapter, 10, 0);
    lv_obj_set_style_pad_row(C.no_adapter, 4, 0);
    lv_obj_set_style_border_side(C.no_adapter, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(C.no_adapter, 4, 0);
    lv_obj_set_style_border_color(C.no_adapter, th_color(theme()->accent), 0);
    C.no_adapter_text = wrap_label(C.no_adapter, OBD_NO_ADAPTER, TS_TITLE);
    wrap_label(C.no_adapter, "Then turn the ignition on. The values below stay empty until a car answers.", TS_SMALL);
    lv_obj_update_layout(root);
    int size = lv_obj_get_content_width(gp) / 4 - 34;      /* rings as big as the width allows */
    if (size > 118) size = 118;
    if (size < 64) size = 64;
    for (int i = 0; i < 4; i++) gauge(gp, i, size);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *s = th_box(gp, TS_PANEL);
        lv_obj_set_size(s, LV_PCT(32), 44);
        C.stat_vals[i] = th_label(s, "", TS_TEXT);
        lv_obj_center(C.stat_vals[i]);
    }
    lv_obj_t *more = th_box(gp, -1);                         /* the extra values, 4 to a row */
    lv_obj_set_size(more, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(more, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(more, 6, 0);
    lv_obj_set_style_pad_row(more, 6, 0);
    for (int i = 0; i < EXTRAS; i++) {
        lv_obj_t *t = C.extra_tiles[i] = th_box(more, TS_PANEL);
        lv_obj_set_size(t, LV_PCT(24), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(t, 8, 0);
        lv_obj_set_style_pad_ver(t, 4, 0);
        lv_obj_t *cap = th_label(t, extras[i].caption, TS_SMALL);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);     /* a narrow window: "Short tr..." */
        lv_obj_set_width(cap, LV_PCT(100));
        C.extra_vals[i] = th_label(t, "--", TS_TEXT);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);              /* shown once the car says it has it */
    }
    lv_obj_add_flag(gp, LV_OBJ_FLAG_SCROLLABLE);            /* (a small window: scroll with the mouse) */

    lv_obj_t *lp = C.pages[PG_LIST] = page_box(root);        /* codes, self-tests, info */
    C.list_head = th_label(lp, "", TS_HEADING);
    lv_obj_set_style_pad_all(C.list_head, 8, 0);
    C.list = nzlist_create(lp, 0);
    lv_obj_set_width(nzlist_obj(C.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(C.list), 1);

    build_graph(C.pages[PG_GRAPH] = page_box(root));
    build_scan_page(C.pages[PG_SCAN] = page_box(root));
    lv_obj_t *rp = C.pages[PG_REPORT] = page_box(root);      /* the scan's report: one long scrolling page */
    C.report = th_box(rp, -1);
    lv_obj_set_width(C.report, LV_PCT(100));
    lv_obj_set_flex_grow(C.report, 1);
    lv_obj_set_flex_flow(C.report, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(C.report, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(C.report, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(C.report, 10, 0);
    lv_obj_set_style_pad_row(C.report, 6, 0);
    if (scan_have) build_report();                          /* the last scan stays until a new one */
    build_help(root);

    C.hint_bar = th_box(root, TS_BAR);
    lv_obj_set_size(C.hint_bar, LV_PCT(100), 26);

    connect();
    show_page(PG_GAUGES);
    show_values();
    show_status();
    C.timer = lv_timer_create(tick_timer, 250, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (C.page == PG_SCAN) {                                /* while scanning only B (stop) */
        if (k == NZ_KEY_B) { carscan_cancel(); C.scan_running = 0; C.pdf_after_scan = -1; ui_toast("Scan stopped - no new PDF saved"); show_page(PG_GAUGES); }
        return 1;
    }
    if (k == NZ_KEY_X) { open_menu(); return 1; }
    if (C.page == PG_FEATURES && k == NZ_KEY_A) { support_menu(); return 1; }
    if (C.page == PG_GAUGES) {
        if (k == NZ_KEY_A) { ask_pdf(); return 1; }
        if (k == NZ_KEY_Y) { show_page(PG_HELP); return 1; }
        if (k == NZ_KEY_UP || k == NZ_KEY_DOWN || k == NZ_KEY_L || k == NZ_KEY_R) {
            lv_obj_scroll_by(C.pages[PG_GAUGES], 0, k == NZ_KEY_UP ? 60 : k == NZ_KEY_DOWN ? -60 : k == NZ_KEY_L ? 260 : -260, LV_ANIM_OFF);
            return 1;
        }
        return 0;                                        /* B: out of the app */
    }
    if (k == NZ_KEY_B) { show_page(PG_GAUGES); return 1; }
    switch (C.page) {
    case PG_LIST:
        return nzlist_key(C.list, k);
    case PG_GRAPH:
        if (k == NZ_KEY_LEFT || k == NZ_KEY_RIGHT) { graph_step(k == NZ_KEY_RIGHT ? 1 : -1); return 1; }
        return 0;
    case PG_REPORT: case PG_HELP: case PG_FEATURES: {
        lv_obj_t *body = C.page == PG_REPORT ? C.report : C.page == PG_HELP ? C.help : C.features;
        int step = k == NZ_KEY_UP ? -60 : k == NZ_KEY_DOWN ? 60 : k == NZ_KEY_L ? -300 : k == NZ_KEY_R ? 300 : 0;
        if (!step) return 0;
        lv_coord_t y = lv_obj_get_scroll_y(body) + step, max = y + lv_obj_get_scroll_bottom(body) - step;
        if (y > max) y = max;
        if (y < 0) y = 0;
        lv_obj_scroll_to_y(body, y, LV_ANIM_OFF);
        return 1;
    }
    default: return 0;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    if (C.timer) lv_timer_del(C.timer);
    bgjob_stop(&C.support_job);
    carscan_cancel();
    carlog_stop();
    obd_stop();
    nzlist_free(C.list);
    memset(&C, 0, sizeof C);
}

NzApp app_car = {
    .id = APP_CAR, .title = "Car", .icon = ICON_CAR,
    .create = create, .key = key, .tick = tick, .destroy = destroy,
};

/* for tests */
int app_car_page(void) { return C.page; }
const CarScan *app_car_scan(void) { return scan_have ? scan : NULL; }
int app_car_graph_points(void) { return C.graph_n; }
int app_car_scroll_y(void)
{
    lv_obj_t *body = C.page == PG_HELP ? C.help : C.page == PG_FEATURES ? C.features : C.pages[PG_GAUGES];
    return body ? lv_obj_get_scroll_y(body) : 0;
}
/* the "No OBD adapter" box's text while it is shown, else "" */
const char *app_car_message(void)
{
    if (!C.no_adapter || lv_obj_has_flag(C.no_adapter, LV_OBJ_FLAG_HIDDEN)) return "";
    return lv_label_get_text(C.no_adapter_text);
}

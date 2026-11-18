/* app_probe.c - the NZEUS Probe screen. Talks to a Probe board (a Pico or
 * STM32 running R36S/apps/probe/firmware) over USB serial and shows its
 * measurements: voltmeter, logic probe, a simple scope, a signal generator
 * and an I2C scanner. The reading of the board's answers is in probe.c; this
 * file is only the screen. No board plugged in -> it says so and keeps looking.
 *
 * UP/DOWN pick a tool.  A read / set.  LEFT/RIGHT change the channel (volt/scope)
 * or the signal-generator frequency.  X start/stop the scope's live sweep.  B back.
 *
 * Finding the board never blocks the screen: a timer tries one port at a time,
 * asks "ID" and gives the board a few seconds to answer (boards with an
 * Arduino-style bootloader restart for ~2 s when their port is opened). */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "probe.h"
#include "serial.h"
#include "theme.h"
#include "ui.h"

#define POLL_MS      50     /* the timer */
#define ID_EVERY_MS  500    /* ask "ID" again this often while trying a port */
#define ID_GIVE_UP   3000   /* ...and move on to the next port after this */
#define LIVE_MS      250    /* the scope's live sweep: a new trace this often */

enum { T_VOLT, T_LOGIC, T_SCOPE, T_SIGGEN, T_I2C, T_COUNT };
static const char *const tool_name[T_COUNT] = {
    "Voltmeter", "Logic probe", "Scope", "Signal generator", "I2C scan"
};

static const int siggen_steps[] = { 0, 10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000 };
#define SIGGEN_STEPS (int)(sizeof siggen_steps / sizeof siggen_steps[0])

static struct {
    int fd;                   /* the Probe's port, -1 = none yet */
    char port[48];
    int try_fd;               /* the port being asked "are you a Probe?" (-1 = none) */
    int try_idx;              /* which port in the list we're on */
    uint32_t try_start, try_asked;
    ProbeInfo info;
    ProbeLines lines;         /* bytes from the board, collected into lines */
    int tool;
    int chan;                 /* voltmeter / scope channel (1..info.adc) */
    int siggen_step;          /* index into siggen_steps */
    int scope_live;
    uint32_t live_at;
    char reading[64];         /* the big line */
    char detail[256];         /* under it (logic pins, I2C list, messages) */
    lv_timer_t *timer;
    lv_obj_t *status, *tool_lbl, *read_lbl, *detail_lbl, *chart, *hint_bar, *hint;
    lv_chart_series_t *ser;
} P;

/* ---------- talking to the board ---------- */

/* send one command line (adds the newline) */
static void send_cmd(const char *cmd)
{
    if (P.fd < 0) return;
    serial_write(P.fd, cmd, (int)strlen(cmd));
    serial_write(P.fd, "\n", 1);
}

static int adc_count(void) { return P.info.adc > 0 ? P.info.adc : 2; }   /* (before INFO: the usual 2) */

/* forget the board (unplugged, or the app is closing) */
static void lose_board(void)
{
    if (P.fd >= 0) serial_close(P.fd);
    if (P.try_fd >= 0) serial_close(P.try_fd);
    P.fd = P.try_fd = -1;
    P.port[0] = 0;
    memset(&P.info, 0, sizeof P.info);
    P.lines.len = 0;
    P.scope_live = 0;
}

/* one step of the search: try the next port, or keep waiting for the one we asked */
static void search_step(void)
{
    char ports[SERIAL_MAX_PORTS][48];
    uint32_t now = lv_tick_get();
    if (P.try_fd < 0) {                                     /* open the next port in the list */
        int n = serial_list(ports, SERIAL_MAX_PORTS);
        if (n <= 0) return;
        if (P.try_idx >= n) P.try_idx = 0;
        P.try_fd = serial_open(ports[P.try_idx], 115200);
        if (P.try_fd < 0) { P.try_idx++; return; }
        snprintf(P.port, sizeof P.port, "%s", ports[P.try_idx]);
        P.lines.len = 0;
        P.try_start = P.try_asked = now;
        serial_write(P.try_fd, "ID\n", 3);
        return;
    }
    char buf[256], line[PROBE_LINE_MAX];
    int got;
    while ((got = serial_read(P.try_fd, buf, sizeof buf)) > 0) {
        int off = 0, used = 0;
        while (off < got) {
            int whole = probe_take_line(&P.lines, buf + off, got - off, &used, line, sizeof line);
            off += used;
            if (whole && probe_parse_id(line, &P.info)) {   /* it's a Probe: keep it */
                P.fd = P.try_fd;
                P.try_fd = -1;
                P.lines.len = 0;
                send_cmd("INFO");
                return;
            }
        }
    }
    if (got < 0 || lv_tick_elaps(P.try_start) > ID_GIVE_UP) {   /* not a Probe (or gone): next port */
        serial_close(P.try_fd);
        P.try_fd = -1;
        P.port[0] = 0;
        P.try_idx++;
    } else if (lv_tick_elaps(P.try_asked) > ID_EVERY_MS) {      /* still restarting? ask again */
        serial_write(P.try_fd, "ID\n", 3);
        P.try_asked = now;
    }
}

/* ask the current tool for a fresh reading (or set the signal generator) */
static void run_tool(void)
{
    char cmd[32];
    if (P.fd < 0) return;
    switch (P.tool) {
    case T_VOLT:   snprintf(cmd, sizeof cmd, "V %d", P.chan); break;
    case T_LOGIC:  snprintf(cmd, sizeof cmd, "L"); break;
    case T_SCOPE:  snprintf(cmd, sizeof cmd, "S %d 100 200", P.chan); break;   /* 100 samples, 200 us apart = 20 ms */
    case T_SIGGEN: snprintf(cmd, sizeof cmd, "P %d 50", siggen_steps[P.siggen_step]); break;   /* 0 = off */
    default:       snprintf(cmd, sizeof cmd, "I"); break;
    }
    send_cmd(cmd);
}

/* put the scope's samples on the chart, 0 V at the bottom, the board's top voltage at the top */
static void draw_scope(const int *v, int n)
{
    lv_chart_set_range(P.chart, LV_CHART_AXIS_PRIMARY_Y, 0, P.info.vref > 0 ? P.info.vref : 3300);
    lv_chart_set_point_count(P.chart, (uint16_t)n);
    for (int i = 0; i < n; i++) lv_chart_set_value_by_id(P.chart, P.ser, (uint16_t)i, (lv_coord_t)v[i]);
    lv_chart_refresh(P.chart);
}

/* read one of the board's answer lines and keep what it says */
static void handle_line(const char *line)
{
    int ch, mv, lv[16], v[PROBE_MAX_SAMPLES], us, ad[16];
    if (probe_parse_info(line, &P.info)) {
        if (P.chan > adc_count()) P.chan = 1;
        return;
    }
    if (!strncmp(line, "ERR", 3)) { snprintf(P.detail, sizeof P.detail, "The board said: %.200s", line); return; }
    switch (P.tool) {
    case T_VOLT:
        if (probe_parse_volt(line, &ch, &mv) && ch == P.chan)
            snprintf(P.reading, sizeof P.reading, "%d.%03d V", mv / 1000, mv % 1000);
        break;
    case T_LOGIC: {
        int nl = probe_parse_logic(line, lv, 16);
        if (nl > 0) {
            size_t k = 0;
            for (int i = 0; i < nl && k + 12 < sizeof P.reading; i++)
                k += (size_t)snprintf(P.reading + k, sizeof P.reading - k, "%d:%s  ", i + 1, lv[i] ? "HIGH" : "low");
        }
        break;
    }
    case T_SCOPE: {
        int nn = probe_parse_scope(line, v, PROBE_MAX_SAMPLES, &us);
        if (nn > 0) {
            int lo = v[0], hi = v[0];
            for (int i = 1; i < nn; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
            snprintf(P.reading, sizeof P.reading, "%d.%03d to %d.%03d V", lo / 1000, lo % 1000, hi / 1000, hi % 1000);
            snprintf(P.detail, sizeof P.detail, "%d samples, %d.%d ms across", nn, nn * us / 1000, nn * us / 100 % 10);
            draw_scope(v, nn);
        }
        break;
    }
    case T_I2C: {
        int na = probe_parse_i2c(line, ad, 16);
        if (na == 0) { snprintf(P.reading, sizeof P.reading, "Nothing on the bus"); P.detail[0] = 0; }
        else if (na > 0) {
            snprintf(P.reading, sizeof P.reading, "%d chip%s found", na, na == 1 ? "" : "s");
            size_t k = 0;
            P.detail[0] = 0;
            for (int i = 0; i < na && k + 40 < sizeof P.detail; i++) {
                const char *nm = probe_i2c_name(ad[i]);
                k += (size_t)snprintf(P.detail + k, sizeof P.detail - k, "0x%02X  %s\n", ad[i], nm ? nm : "unknown");
            }
        }
        break;
    }
    default: break;                                          /* the signal generator's answer: nothing to show */
    }
}

/* ---------- drawing ---------- */

static void show(void)
{
    char s[96], r[96];
    if (P.fd < 0) snprintf(s, sizeof s, P.port[0] ? "Looking for a Probe board... (asking %s)" : "No Probe board - plug a Pico or STM32 into USB", P.port);
    else if (P.info.adc) snprintf(s, sizeof s, LV_SYMBOL_OK "  %s on %s   %d analog, %d logic", P.info.board, P.port, P.info.adc, P.info.logic);
    else snprintf(s, sizeof s, LV_SYMBOL_OK "  %s on %s", P.info.board, P.port);
    lv_label_set_text(P.status, s);

    snprintf(s, sizeof s, "%s   (%d of %d)", tool_name[P.tool], P.tool + 1, T_COUNT);
    lv_label_set_text(P.tool_lbl, s);

    if (P.tool == T_SCOPE) lv_obj_clear_flag(P.chart, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(P.chart, LV_OBJ_FLAG_HIDDEN);

    if (P.fd < 0) {
        lv_label_set_text(P.read_lbl, "Waiting for a board...");
        lv_label_set_text(P.detail_lbl, "The board runs R36S/apps/probe/firmware.\nIts inputs take 0 to 3.3 V only.");
        return;
    }
    const char *rd = P.reading[0] ? P.reading : "(press A)";
    switch (P.tool) {
    case T_VOLT:   snprintf(r, sizeof r, "Channel %d:  %s", P.chan, rd); break;
    case T_SCOPE:  snprintf(r, sizeof r, "Channel %d%s:  %s", P.chan, P.scope_live ? " (live)" : "", rd); break;
    case T_SIGGEN:
        if (siggen_steps[P.siggen_step]) snprintf(r, sizeof r, "%d Hz, 50%%   (A sets it)", siggen_steps[P.siggen_step]);
        else snprintf(r, sizeof r, "Off   (A sets it)");
        break;
    default:       snprintf(r, sizeof r, "%s", rd); break;
    }
    lv_label_set_text(P.read_lbl, r);
    lv_label_set_text(P.detail_lbl, P.detail);
}

/* the button hints change with the tool */
static void set_hint(void)
{
    static const char *const h_scope[] = { LV_SYMBOL_UP LV_SYMBOL_DOWN, "Tool", LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "Channel", "A", "Once", "X", "Live", "B", "Back" };
    static const char *const h_volt[]  = { LV_SYMBOL_UP LV_SYMBOL_DOWN, "Tool", LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "Channel", "A", "Read", "B", "Back" };
    static const char *const h_sig[]   = { LV_SYMBOL_UP LV_SYMBOL_DOWN, "Tool", LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "Hz", "A", "Set", "B", "Back" };
    static const char *const h_plain[] = { LV_SYMBOL_UP LV_SYMBOL_DOWN, "Tool", "A", "Read", "B", "Back" };
    if (P.hint) lv_obj_del(P.hint);
    if (P.tool == T_SCOPE)       P.hint = nz_hints(P.hint_bar, h_scope, 5);
    else if (P.tool == T_VOLT)   P.hint = nz_hints(P.hint_bar, h_volt, 4);
    else if (P.tool == T_SIGGEN) P.hint = nz_hints(P.hint_bar, h_sig, 4);
    else                         P.hint = nz_hints(P.hint_bar, h_plain, 3);
    lv_obj_center(P.hint);
}

/* ---------- the timer ---------- */

static void poll(lv_timer_t *t)
{
    (void)t;
    if (P.fd < 0) { search_step(); show(); return; }
    char buf[512], line[PROBE_LINE_MAX];
    int got;
    while ((got = serial_read(P.fd, buf, sizeof buf)) > 0) {
        int off = 0, used = 0;
        while (off < got) {
            int whole = probe_take_line(&P.lines, buf + off, got - off, &used, line, sizeof line);
            off += used;
            if (whole) handle_line(line);
        }
    }
    if (got < 0) { lose_board(); ui_toast("The Probe board was unplugged"); }
    else if (P.scope_live && P.tool == T_SCOPE && lv_tick_elaps(P.live_at) > LIVE_MS) {
        run_tool();
        P.live_at = lv_tick_get();
    }
    show();
}

/* ---------- the app ---------- */

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&P, 0, sizeof P);
    P.fd = P.try_fd = -1;
    P.chan = 1;
    P.siggen_step = 5;                                       /* 1000 Hz */
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bar = th_box(root, TS_BAR);
    lv_obj_set_size(bar, LV_PCT(100), 28);
    P.status = th_label(bar, "", TS_SMALL);              /* (the title bar above already has the icon) */
    lv_obj_align(P.status, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *body = lv_obj_create(root);                   /* everything between the bars */
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    P.tool_lbl = th_label(body, "", TS_HEADING);
    P.read_lbl = th_label(body, "", TS_BIG);
    P.detail_lbl = th_label(body, "", TS_MONO);
    lv_obj_set_width(P.detail_lbl, LV_PCT(100));
    lv_label_set_long_mode(P.detail_lbl, LV_LABEL_LONG_CLIP);

    P.chart = lv_chart_create(body);                        /* the scope's trace (hidden for the other tools) */
    lv_obj_remove_style_all(P.chart);                       /* our theme, not LVGL's */
    th_add(P.chart, TS_PANEL);
    lv_obj_set_width(P.chart, LV_PCT(100));
    lv_obj_set_flex_grow(P.chart, 1);
    lv_obj_set_style_pad_all(P.chart, 6, 0);
    lv_obj_set_style_line_width(P.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(P.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(P.chart, 0, LV_PART_INDICATOR);  /* a line, no dots */
    lv_obj_set_style_line_color(P.chart, th_color(theme()->border), LV_PART_MAIN);   /* the grid */
    lv_chart_set_type(P.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(P.chart, 5, 0);
    lv_chart_set_point_count(P.chart, 100);
    P.ser = lv_chart_add_series(P.chart, th_color(theme()->accent), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(P.chart, P.ser, LV_CHART_POINT_NONE);

    P.hint_bar = th_box(root, TS_BAR);
    lv_obj_set_size(P.hint_bar, LV_PCT(100), 24);
    set_hint();

    show();
    P.timer = lv_timer_create(poll, POLL_MS, NULL);
}

/* a new tool: clear the old one's reading */
static void pick_tool(int t)
{
    P.tool = (t + T_COUNT) % T_COUNT;
    P.reading[0] = P.detail[0] = 0;
    P.scope_live = 0;
    lv_chart_set_all_value(P.chart, P.ser, LV_CHART_POINT_NONE);
    set_hint();
    show();
}

static int key(NzApp *a, int k)
{
    (void)a;
    switch (k) {
    case NZ_KEY_UP:   pick_tool(P.tool - 1); return 1;
    case NZ_KEY_DOWN: pick_tool(P.tool + 1); return 1;
    case NZ_KEY_LEFT:
    case NZ_KEY_RIGHT: {
        int d = k == NZ_KEY_RIGHT ? 1 : -1;
        if (P.tool == T_VOLT || P.tool == T_SCOPE) {
            P.chan += d;
            if (P.chan < 1) P.chan = adc_count();
            if (P.chan > adc_count()) P.chan = 1;
            P.reading[0] = 0;
        } else if (P.tool == T_SIGGEN) {
            P.siggen_step += d;
            if (P.siggen_step < 0) P.siggen_step = 0;
            if (P.siggen_step >= SIGGEN_STEPS) P.siggen_step = SIGGEN_STEPS - 1;
        } else return 0;
        show();
        return 1;
    }
    case NZ_KEY_A:
        if (P.fd < 0) { ui_toast("No Probe board yet"); return 1; }
        run_tool();
        return 1;
    case NZ_KEY_X:
        if (P.tool != T_SCOPE) return 0;
        if (P.fd < 0) { ui_toast("No Probe board yet"); return 1; }
        P.scope_live = !P.scope_live;
        P.live_at = 0;                                       /* (the first trace right away) */
        show();
        return 1;
    default: return 0;                                       /* B: the app closes */
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    if (P.timer) lv_timer_del(P.timer);
    send_cmd("P 0 50");                                      /* leaving: the signal generator stops too */
    lose_board();
    memset(&P, 0, sizeof P);
    P.fd = P.try_fd = -1;
}

/* for tests */
int app_probe_connected(void) { return P.fd >= 0; }
const char *app_probe_reading(void) { return P.reading; }
const char *app_probe_detail(void) { return P.detail; }

NzApp app_probe = {
    .id = APP_PROBE, .title = "Probe", .icon = ICON_GAUGE,
    .create = create, .key = key, .destroy = destroy,
};

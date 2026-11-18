/* app_electronics.c - the NZEUS Electronics app: bench tools that work offline.
 *
 * First a list of tools. A opens one; B goes back to the list (B on the list
 * leaves the app). A calculator tool is a small form:
 *   top:    the answer, big, with one line in plain words (and a drawing of
 *           the resistor, in its real colours, for the resistor tools)
 *   below:  rows of values.  UP/DOWN pick a row, LEFT/RIGHT change it,
 *           L1/R1 change it in big steps, A types a number (or opens a list)
 * Every change is worked out again at once (the maths is in elec_calc.c).
 * A pinout tool shows a board: pin names down both sides, as on the board.
 * Mouse (desktop mode): click a row to pick it, click again to type a value,
 * click the left/right half of a value to change it, "< Tools" goes back. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "elec_calc.h"
#include "home.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

/* ---------- the tools ---------- */
enum { T_COLOUR, T_FINDER, T_OHM, T_LED, T_DIVIDER, T_555, T_CONVERT,
       T_BLUEPILL, T_UNO, T_NANO, T_ESP32, T_COUNT };
#define IS_PINOUT(t) ((t) >= T_BLUEPILL)

static const struct { const char *title, *sub; int icon; } tool_info[T_COUNT] = {
    { "Resistor colour code", "colour bands -> value, with a picture", ICON_ELECTRONICS },
    { "Resistor finder",      "a value -> the nearest real resistor", ICON_ELECTRONICS },
    { "Ohm's law",            "know two of V, I, R, P? get the rest", ICON_CALCULATOR },
    { "LED resistor",         "stops an LED from burning out", ICON_CALCULATOR },
    { "Voltage divider",      "two resistors make a lower voltage", ICON_CALCULATOR },
    { "555 timer",            "blink speed, or one pulse length", ICON_CLOCK },
    { "Unit converter",       "mA, kOhm, uF, MHz, inch, C/F, AWG", ICON_GAUGE },
    { "Blue Pill pinout",     "STM32F103C8T6: 3.3 V and 5 V pins", ICON_SERIAL },
    { "Arduino Uno pinout",   "PWM, analog, I2C and SPI pins", ICON_ARDUINO },
    { "Arduino Nano pinout",  "the small Uno, for breadboards", ICON_ARDUINO },
    { "ESP32 DevKit pinout",  "30 pins: input-only and boot pins", ICON_WIFI },
};

/* ---------- the rows of a form ("fields") ---------- */
enum { F_NUM, F_CHOICE };                       /* a number, or one of a list */
enum { S_E12, S_E24, S_NICE, S_LIN };           /* how LEFT/RIGHT step a number */
enum { ROLE_NONE, ROLE_DIGIT, ROLE_MULT, ROLE_TOL };   /* colour rows: what the band means */

typedef struct {
    const char *label;
    int kind;
    /* numbers (always in volts, amps, ohms, farads...) */
    double val, min, max, lin;                  /* lin: the step for S_LIN */
    int step;
    const char *unit;                           /* "V", "A", "Ohm", "F"; NULL = a plain number */
    /* choices */
    int choice, n;
    const char *const *names;                   /* the choices as text, or NULL for colours */
    const int *colours;                         /* colour rows: choice -> EC_* colour */
    int role;
    int relayout;                               /* changing it shows/hides other rows */
    int hidden;
} Field;

#define MAX_FIELDS 6
static Field F[T_COUNT][MAX_FIELDS];
static int NF[T_COUNT];

/* field numbers, per tool */
enum { C_BANDS, C_B1, C_B2, C_B3, C_MULT, C_TOL };                /* colour code */
enum { FD_VALUE, FD_SERIES, FD_BANDS };                           /* finder */
enum { O_KNOW, O_V, O_I, O_R, O_P };                              /* Ohm's law */
enum { L_SUPPLY, L_COLOUR, L_VF, L_I };                           /* LED */
enum { D_MODE, D_VIN, D_R1, D_R2, D_VOUT };                       /* divider */
enum { N_MODE, N_R1, N_R2, N_C };                                 /* 555 */
enum { U_KIND, U_VALUE, U_FROM, U_TO };                           /* converter */

static const char *const band_count_names[] = { "4 bands", "5 bands" };
static const char *const series_names[] = { "E12 (most kits)", "E24" };
static const char *const know_names[] = { "V and I", "V and R", "I and R", "P and V", "P and I", "P and R" };
static const int know_pairs[6][2] = { { EC_V, EC_I }, { EC_V, EC_R }, { EC_I, EC_R },
                                      { EC_P, EC_V }, { EC_P, EC_I }, { EC_P, EC_R } };
static const char *const led_names[] = { "red", "orange", "yellow", "green (classic)", "green (bright)",
                                         "blue", "white", "infrared" };
static const double led_vf[] = { 2.0, 2.0, 2.1, 2.2, 3.0, 3.2, 3.2, 1.2 };   /* typical LED voltages */
static const char *const div_names[] = { "find Vout", "find R2 for a Vout" };
static const char *const t555_names[] = { "astable (blinks)", "monostable (1 pulse)" };
static const char *const kind_names[] = { "Current", "Voltage", "Resistance", "Capacitance",
                                          "Frequency", "Length", "Temperature", "Wire AWG" };
#define KIND_AWG EC_Q_COUNT                     /* the 8th kind: wire gauge */
static const char *unit_names[5];               /* the units of the chosen kind */

static const int digit_map[10] = { EC_BLACK, EC_BROWN, EC_RED, EC_ORANGE, EC_YELLOW,
                                   EC_GREEN, EC_BLUE, EC_VIOLET, EC_GREY, EC_WHITE };
static const int mult_map[12] = { EC_BLACK, EC_BROWN, EC_RED, EC_ORANGE, EC_YELLOW, EC_GREEN,
                                  EC_BLUE, EC_VIOLET, EC_GREY, EC_WHITE, EC_GOLD, EC_SILVER };
/* every tolerance colour of IEC 60062:2016 (orange, yellow, grey: rare precision ones) */
static const int tol_map[11] = { EC_BROWN, EC_RED, EC_ORANGE, EC_YELLOW, EC_GREEN, EC_BLUE, EC_VIOLET,
                                 EC_GREY, EC_GOLD, EC_SILVER, EC_NONE };

/* The real colours of resistor bands (NOT theme colours: a red band is red). */
static const uint32_t real_rgb[EC_COLOUR_COUNT] = {
    0x141414, 0x7A4520, 0xD62828, 0xF77F00, 0xF5CE2A, 0x2E9E44, 0x1D5FD6, 0x8A44C8, 0x8C8C8C, 0xF8F8F8,
    0xC9A23A, 0xC2C6CA, 0x000000,
};

/* ---------- setting up the forms ---------- */
static Field *add_num(int t, const char *label, double v, const char *unit, int step,
                      double min, double max)
{
    Field *f = &F[t][NF[t]++];
    memset(f, 0, sizeof *f);
    f->label = label; f->kind = F_NUM; f->val = v; f->unit = unit; f->step = step;
    f->min = min; f->max = max; f->lin = 1;
    return f;
}

static Field *add_choice(int t, const char *label, int choice, const char *const *names, int n)
{
    Field *f = &F[t][NF[t]++];
    memset(f, 0, sizeof *f);
    f->label = label; f->kind = F_CHOICE; f->choice = choice; f->names = names; f->n = n;
    return f;
}

static Field *add_colour(int t, const char *label, int colour, const int *map, int n, int role)
{
    Field *f = add_choice(t, label, 0, NULL, n);
    f->colours = map; f->role = role;
    for (int i = 0; i < n; i++) if (map[i] == colour) f->choice = i;
    return f;
}

/* the converter's starting value and units for each kind */
static void convert_kind_defaults(void)
{
    static const struct { double v; int from, to; } d[EC_Q_COUNT] = {
        { 20, 1, 2 }, { 3300, 0, 1 }, { 4.7, 1, 0 }, { 100, 1, 2 }, { 16, 2, 0 }, { 1, 3, 0 }, { 25, 0, 1 },
    };
    Field *f = F[T_CONVERT];
    int k = f[U_KIND].choice;
    if (k == KIND_AWG) {
        f[U_VALUE].label = "AWG number"; f[U_VALUE].val = 22; f[U_VALUE].step = S_LIN;
        f[U_VALUE].lin = 1; f[U_VALUE].min = 0; f[U_VALUE].max = 40;
        return;
    }
    for (int u = 0; u < ec_unit_count(k); u++) unit_names[u] = ec_unit_name(k, u);
    f[U_FROM].n = f[U_TO].n = ec_unit_count(k);
    f[U_FROM].choice = d[k].from;
    f[U_TO].choice = d[k].to;
    f[U_VALUE].label = "Value";
    f[U_VALUE].val = d[k].v;
    if (k == EC_Q_TEMPERATURE) {                     /* temperatures can be 0 or below */
        f[U_VALUE].step = S_LIN; f[U_VALUE].lin = 1; f[U_VALUE].min = -460; f[U_VALUE].max = 10000;
    } else {
        f[U_VALUE].step = S_NICE; f[U_VALUE].min = 1e-12; f[U_VALUE].max = 1e12;
    }
}

static void set_defaults(void)
{
    memset(NF, 0, sizeof NF);
    int t = T_COLOUR;                                /* brown-black-red-gold = 1 kOhm */
    add_choice(t, "Bands", 0, band_count_names, 2)->relayout = 1;
    add_colour(t, "1st band", EC_BROWN, digit_map, 10, ROLE_DIGIT);
    add_colour(t, "2nd band", EC_BLACK, digit_map, 10, ROLE_DIGIT);
    add_colour(t, "3rd band", EC_BLACK, digit_map, 10, ROLE_DIGIT);
    add_colour(t, "Multiplier", EC_RED, mult_map, 12, ROLE_MULT);
    add_colour(t, "Tolerance", EC_GOLD, tol_map, 11, ROLE_TOL);

    t = T_FINDER;
    add_num(t, "Wanted value", 5000, "Ohm", S_E24, 0.1, 1e9);
    add_choice(t, "Series", 0, series_names, 2);
    add_choice(t, "Bands", 0, band_count_names, 2);

    t = T_OHM;
    add_choice(t, "I know", 0, know_names, 6)->relayout = 1;
    add_num(t, "Voltage V", 5, "V", S_NICE, 1e-6, 1e6);
    add_num(t, "Current I", 0.02, "A", S_NICE, 1e-9, 1e4);
    add_num(t, "Resistance R", 1000, "Ohm", S_E12, 1e-3, 1e10);
    add_num(t, "Power P", 0.25, "W", S_NICE, 1e-9, 1e6);

    t = T_LED;
    Field *f = add_num(t, "Supply voltage", 5, "V", S_LIN, 1, 48);
    f->lin = 0.1;
    add_choice(t, "LED colour", 0, led_names, 8);
    f = add_num(t, "LED voltage", 2.0, "V", S_LIN, 0.5, 5);
    f->lin = 0.1;
    f = add_num(t, "LED current", 0.02, "A", S_LIN, 0.001, 1);
    f->lin = 0.001;

    t = T_DIVIDER;
    add_choice(t, "Find", 0, div_names, 2)->relayout = 1;
    f = add_num(t, "Vin (input)", 5, "V", S_LIN, 0.1, 1000);
    f->lin = 0.1;
    add_num(t, "R1 (top)", 10000, "Ohm", S_E12, 1, 1e9);
    add_num(t, "R2 (bottom)", 20000, "Ohm", S_E12, 1, 1e9);
    f = add_num(t, "Wanted Vout", 3.3, "V", S_LIN, 0.1, 1000);
    f->lin = 0.1;

    t = T_555;
    add_choice(t, "Mode", 0, t555_names, 2)->relayout = 1;
    add_num(t, "R1 (+ to pin 7)", 1000, "Ohm", S_E12, 100, 1e8);
    add_num(t, "R2 (pin 7 to 6)", 10000, "Ohm", S_E12, 100, 1e8);
    add_num(t, "C (pin 6 to GND)", 10e-6, "F", S_E12, 1e-12, 1);

    t = T_CONVERT;
    add_choice(t, "Convert", EC_Q_RESISTANCE, kind_names, 8)->relayout = 1;
    add_num(t, "Value", 4.7, NULL, S_NICE, 1e-12, 1e12);
    add_choice(t, "From", 1, unit_names, 3);
    add_choice(t, "To", 0, unit_names, 3);
    convert_kind_defaults();
}

/* which rows are shown depends on some choices */
static void set_visibility(int t)
{
    Field *f = F[t];
    for (int i = 0; i < NF[t]; i++) f[i].hidden = 0;
    if (t == T_COLOUR) f[C_B3].hidden = f[C_BANDS].choice == 0;
    if (t == T_OHM) {
        const int *k = know_pairs[f[O_KNOW].choice];
        for (int q = 0; q < 4; q++) f[O_V + q].hidden = (q != k[0] && q != k[1]);
    }
    if (t == T_DIVIDER) { f[D_R2].hidden = f[D_MODE].choice == 1; f[D_VOUT].hidden = f[D_MODE].choice == 0; }
    if (t == T_555) f[N_R2].hidden = f[N_MODE].choice == 1;
    if (t == T_CONVERT) f[U_FROM].hidden = f[U_TO].hidden = f[U_KIND].choice == KIND_AWG;
}

/* ---------- the screen ---------- */
#define ROW_H  30
#define PIN_H  20
#define DRAW_W 300

static struct {
    lv_obj_t *list_page, *tool_page, *scroll, *rows_box;
    lv_obj_t *big, *sub, *explain, *draw, *body;
    lv_obj_t *card, *top, *head, *back, *title;
    int stacked;                                /* drawing above the answer (narrow window) */
    int small_title;                            /* tool name in the smaller font (narrow window) */
    int refit;                                  /* 1 = a tool just opened: fit() measures again */
    lv_obj_t *band[5], *band_lbl[5];
    int band_col[5];                            /* (for tests) the colour drawn, -1 = none */
    lv_obj_t *row[MAX_FIELDS], *val[MAX_FIELDS], *swatch[MAX_FIELDS];
    int row_field[MAX_FIELDS], nrows, sel;
    NzList *list;
    int tool;                                   /* -1 = the tool list is showing */
} E;

static void update(void);
static void build_rows(void);

/* ---------- small text helpers ---------- */
static void fmt(double v, const char *unit, char *buf, size_t n) { ec_format(v, unit, buf, n); }

/* a plain number with up to 6 digits: 4700, 25.4, 0.0000254, -40 */
static void fmt_plain(double v, char *buf, size_t n)
{
    if (v == 0 || v != v) { snprintf(buf, n, "0"); return; }
    int digits = (int)floor(log10(fabs(v))) + 1;
    int dec = 6 - digits;
    if (dec < 0) dec = 0;
    if (dec > 12) dec = 12;
    snprintf(buf, n, "%.*f", dec, v);
    if (strchr(buf, '.')) {                          /* "25.4000" -> "25.4" */
        size_t len = strlen(buf);
        while (len && buf[len - 1] == '0') buf[--len] = 0;
        if (len && buf[len - 1] == '.') buf[--len] = 0;
    }
}

/* "x100", "x4.7k" ... the multiplier band's meaning */
static void mult_text(double m, char *buf, size_t n)
{
    char t[24];
    if (m < 1) snprintf(buf, n, "x%g", m);
    else {
        fmt(m, "", t, sizeof t);
        char *sp = strchr(t, ' ');                   /* "1 k" -> "1k" */
        if (sp) memmove(sp, sp + 1, strlen(sp));
        snprintf(buf, n, "x%s", t);
    }
}

static const char *rating_text(double w)
{
    if (w == 0.25) return "1/4 W";
    if (w == 0.5) return "1/2 W";
    if (w == 1) return "1 W";
    if (w == 2) return "2 W";
    if (w == 5) return "5 W";
    return "?";
}

/* the colour a colour row stands for */
static int field_colour(const Field *f) { return f->colours[f->choice]; }

/* what a row shows on the right */
static void field_text(const Field *f, char *buf, size_t n)
{
    char t[48];
    if (f->kind == F_NUM) {
        if (f->unit) fmt(f->val, f->unit, buf, n);
        else fmt_plain(f->val, buf, n);
        return;
    }
    if (!f->colours) { snprintf(buf, n, "%s", f->names[f->choice]); return; }
    int c = field_colour(f);
    if (f->role == ROLE_DIGIT) snprintf(buf, n, "%s  %d", ec_colour_name(c), c);
    else if (f->role == ROLE_MULT) { mult_text(ec_multiplier(c), t, sizeof t); snprintf(buf, n, "%s  %s", ec_colour_name(c), t); }
    else snprintf(buf, n, "%s  %g%%", ec_colour_name(c), ec_tolerance(c));
}

/* ---------- the resistor drawing (real colours) ---------- */
static lv_obj_t *solid(lv_obj_t *parent, uint32_t rgb)
{
    lv_obj_t *o = th_box(parent, -1);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void draw_create(lv_obj_t *parent)
{
    E.draw = th_box(parent, -1);
    lv_obj_set_size(E.draw, DRAW_W, 74);
    lv_obj_t *wire = solid(E.draw, 0xA7A9AC);        /* the two legs, as one line behind the body */
    lv_obj_set_size(wire, DRAW_W, 4);
    lv_obj_set_pos(wire, 0, 26);
    E.body = solid(E.draw, 0xDCC08F);
    lv_obj_set_size(E.body, 220, 48);
    lv_obj_set_pos(E.body, 40, 4);
    lv_obj_set_style_radius(E.body, 20, 0);
    lv_obj_set_style_border_width(E.body, 1, 0);
    lv_obj_set_style_border_color(E.body, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_opa(E.body, LV_OPA_40, 0);
    for (int i = 0; i < 5; i++) {
        E.band[i] = solid(E.body, 0);
        lv_obj_set_style_border_width(E.band[i], 1, 0);       /* a thin edge: white/yellow bands show too */
        lv_obj_set_style_border_color(E.band[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_opa(E.band[i], LV_OPA_30, 0);
        lv_obj_set_style_border_side(E.band[i], LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
        E.band_lbl[i] = th_label(E.draw, "", TS_SMALL);
        lv_obj_set_width(E.band_lbl[i], 48);
        lv_obj_set_style_text_align(E.band_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(E.band_lbl[i], LV_LABEL_LONG_CLIP);
    }
}

/* Show bands b[0..n-1] (n = 4 or 5). The tolerance band sits apart, on the right,
 * as on a real resistor: that is how you know which end to start reading from. */
static void draw_show(const int *b, int n)
{
    static const int x4[4] = { 44, 76, 108, 168 }, x5[5] = { 36, 64, 92, 120, 172 };
    const int *x = n == 5 ? x5 : x4;
    int bw = n == 5 ? 12 : 14;
    lv_obj_set_style_bg_color(E.body, lv_color_hex(n == 5 ? 0x8EC3E6 : 0xDCC08F), 0);  /* precision ones are blue */
    for (int i = 0; i < 5; i++) {
        E.band_col[i] = -1;
        if (i >= n) {
            lv_obj_add_flag(E.band[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(E.band_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char t[16];
        if (i < n - 2) snprintf(t, sizeof t, "%d", b[i]);                 /* a digit */
        else if (i == n - 2) mult_text(ec_multiplier(b[i]), t, sizeof t);  /* the multiplier */
        else snprintf(t, sizeof t, "%g%%", ec_tolerance(b[i]));            /* the tolerance */
        lv_label_set_text(E.band_lbl[i], t);
        lv_obj_clear_flag(E.band_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(E.band_lbl[i], 40 + x[i] + bw / 2 - 24, 56);
        if (b[i] == EC_NONE) { lv_obj_add_flag(E.band[i], LV_OBJ_FLAG_HIDDEN); continue; }   /* no band */
        E.band_col[i] = b[i];
        lv_obj_clear_flag(E.band[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(E.band[i], lv_color_hex(real_rgb[b[i]]), 0);
        lv_obj_set_size(E.band[i], bw, 46);
        lv_obj_set_pos(E.band[i], x[i], 0);
    }
}

/* ---------- the answers ----------
 * Each tool fills: big (the answer), sub (more numbers), ex (plain words),
 * and bands[] if a resistor is drawn. */
typedef struct { char big[64], sub[256], ex[512]; int bands[5], nbands; } Answer;

static void tool_colour(Answer *A)
{
    Field *f = F[T_COLOUR];
    int n = f[C_BANDS].choice ? 5 : 4, b[5], k = 0;
    b[k++] = field_colour(&f[C_B1]);
    b[k++] = field_colour(&f[C_B2]);
    if (n == 5) b[k++] = field_colour(&f[C_B3]);
    b[k++] = field_colour(&f[C_MULT]);
    b[k++] = field_colour(&f[C_TOL]);
    memcpy(A->bands, b, sizeof b);
    A->nbands = n;
    double tol, ohms = ec_bands_to_ohms(b, n, &tol);
    char v[32], lo[32], hi[32], names[96], m[16], digits[8];
    fmt(ohms, "Ohm", v, sizeof v);
    snprintf(A->big, sizeof A->big, "%s", v);
    fmt(ohms * (1 - tol / 100), "Ohm", lo, sizeof lo);
    fmt(ohms * (1 + tol / 100), "Ohm", hi, sizeof hi);
    snprintf(A->sub, sizeof A->sub, "%g%% tolerance: the real value is %s to %s", tol, lo, hi);
    ec_bands_text(b, n, names, sizeof names);
    mult_text(ec_multiplier(b[n - 2]), m, sizeof m);
    for (int i = 0; i < n - 2; i++) digits[i] = (char)('0' + b[i]);
    digits[n - 2] = 0;
    snprintf(A->ex, sizeof A->ex, "%s: the digits %s, then %s = %s. "
             "Read from the end away from the lone band.", names, digits, m, v);
}

static void tool_finder(Answer *A)
{
    Field *f = F[T_FINDER];
    int series = f[FD_SERIES].choice ? 24 : 12, n = f[FD_BANDS].choice ? 5 : 4;
    double want = f[FD_VALUE].val, std = ec_e_nearest(want, series);
    char w[32], s[32], names[96];
    fmt(want, "Ohm", w, sizeof w);
    fmt(std, "Ohm", s, sizeof s);
    snprintf(A->big, sizeof A->big, "%s", s);
    if (ec_ohms_to_bands(std, n, A->bands) != 0) {
        snprintf(A->sub, sizeof A->sub, "too small or too big for colour bands");
        snprintf(A->ex, sizeof A->ex, "%s", n == 4 ? "4 colour bands go from 0.1 Ohm to 99 GOhm."
                                                    : "5 colour bands go from 1 Ohm to 999 GOhm.");
        return;
    }
    A->bands[n - 1] = n == 4 ? EC_GOLD : EC_BROWN;   /* the usual kinds: 5% (gold) or 1% (brown) */
    A->nbands = n;
    ec_bands_text(A->bands, n, names, sizeof names);
    snprintf(A->sub, sizeof A->sub, "%s", names);
    double diff = (std - want) / want * 100;
    if (fabs(diff) < 0.05)
        snprintf(A->ex, sizeof A->ex, "%s is a standard E%d value: you can buy it. %s",
                 s, series, n == 4 ? "Gold band = 5%, the common kind." : "Brown band = 1%, a precision resistor.");
    else
        snprintf(A->ex, sizeof A->ex, "%s is not a standard value. The nearest E%d resistor is %s "
                 "(%.0f%% %s). %s", w, series, s, fabs(diff), diff > 0 ? "more" : "less",
                 n == 4 ? "Gold band = 5%." : "Brown band = 1%.");
}

static void tool_ohm(Answer *A)
{
    static const char *const sym[4] = { "V", "I", "R", "P" }, *const unit[4] = { "V", "A", "Ohm", "W" };
    /* how each missing value is worked out, for each pair you know */
    static const char *const how[6][2] = {
        { "R = V / I", "P = V x I" },       { "I = V / R", "P = V x V / R" },
        { "V = I x R", "P = I x I x R" },   { "I = P / V", "R = V x V / P" },
        { "V = P / I", "R = P / (I x I)" }, { "I = root(P / R)", "V = root(P x R)" },
    };
    Field *f = F[T_OHM];
    int k = f[O_KNOW].choice, qa = know_pairs[k][0], qb = know_pairs[k][1];
    EcOhm o;
    if (ec_ohms_law(qa, f[O_V + qa].val, qb, f[O_V + qb].val, &o) != 0) {
        snprintf(A->big, sizeof A->big, "--");
        snprintf(A->ex, sizeof A->ex, "Both values must be more than 0.");
        return;
    }
    double all[4] = { o.v, o.i, o.r, o.p };
    int miss[2], m = 0;
    for (int q = 0; q < 4; q++) if (q != qa && q != qb) miss[m++] = q;
    char a[32], b[32];
    fmt(all[miss[0]], unit[miss[0]], a, sizeof a);
    fmt(all[miss[1]], unit[miss[1]], b, sizeof b);
    /* the formulas list the missing values in a fixed order: find which comes first */
    int first = how[k][0][0] == sym[miss[0]][0] ? 0 : 1;
    snprintf(A->big, sizeof A->big, "%s = %s", sym[miss[0]], a);
    snprintf(A->sub, sizeof A->sub, "%s = %s", sym[miss[1]], b);
    snprintf(A->ex, sizeof A->ex, "%s  and  %s.  V = volts (push), I = amps (flow), "
             "R = ohms (resists), P = watts (heat).", how[k][first], how[k][1 - first]);
}

static void tool_led(Answer *A)
{
    Field *f = F[T_LED];
    double vs = f[L_SUPPLY].val, vf = f[L_VF].val, i = f[L_I].val;
    EcLed L;
    if (ec_led_resistor(vs, vf, i, 12, &L) != 0) {
        char v[24];
        fmt(vf, "V", v, sizeof v);
        snprintf(A->big, sizeof A->big, "--");
        snprintf(A->sub, sizeof A->sub, "the supply is too low");
        snprintf(A->ex, sizeof A->ex, "The supply must be more than the LED voltage (%s), "
                 "or the LED stays dark.", v);
        return;
    }
    char s[24], d[24], c[24], r[24], rs[24], is[24], pr[24], names[96];
    fmt(vs, "V", s, sizeof s);
    fmt(vf, "V", d, sizeof d);
    fmt(i, "A", c, sizeof c);
    fmt(L.r, "Ohm", r, sizeof r);
    fmt(L.r_std, "Ohm", rs, sizeof rs);
    fmt(L.i_std, "A", is, sizeof is);
    fmt(L.p_r, "W", pr, sizeof pr);
    if (ec_ohms_to_bands(L.r_std, 4, A->bands) == 0) {
        A->bands[3] = EC_GOLD;
        A->nbands = 4;
        ec_bands_text(A->bands, 4, names, sizeof names);
    } else snprintf(names, sizeof names, "?");
    snprintf(A->big, sizeof A->big, "%s", rs);
    if (L.rating > 0)
        snprintf(A->sub, sizeof A->sub, "LED current %s, resistor heat %s: a %s resistor is fine",
                 is, pr, rating_text(L.rating));
    else
        snprintf(A->sub, sizeof A->sub, "resistor heat %s: too much! Use less current.", pr);
    snprintf(A->ex, sizeof A->ex, "The LED needs (%s - %s) / %s = %s. The nearest bigger standard "
             "resistor is %s (%s).", s, d, c, r, rs, names);
}

static void tool_divider(Answer *A)
{
    Field *f = F[T_DIVIDER];
    double vin = f[D_VIN].val, r1 = f[D_R1].val;
    char a[24], b[24], c[24], d[24];
    if (f[D_MODE].choice == 0) {                     /* find Vout */
        double r2 = f[D_R2].val, vout = ec_divider_vout(vin, r1, r2);
        fmt(vout, "V", a, sizeof a);
        fmt(vin / (r1 + r2), "A", b, sizeof b);
        fmt(vin, "V", c, sizeof c);
        snprintf(A->big, sizeof A->big, "Vout = %s", a);
        snprintf(A->sub, sizeof A->sub, "%s flows through R1 and R2", b);
        fmt(r2, "Ohm", a, sizeof a);
        fmt(r1 + r2, "Ohm", d, sizeof d);
        snprintf(A->ex, sizeof A->ex, "Vout = Vin x R2 / (R1 + R2) = %s x %s / %s. True only if what "
                 "you connect to Vout takes very little current.", c, a, d);
        return;
    }
    double want = f[D_VOUT].val, r2 = ec_divider_r2(vin, want, r1);   /* find R2 */
    if (r2 < 0) {
        snprintf(A->big, sizeof A->big, "--");
        snprintf(A->sub, sizeof A->sub, "Vout must be less than Vin");
        snprintf(A->ex, sizeof A->ex, "A divider only makes a voltage smaller: pick a Vout below Vin.");
        return;
    }
    double std = ec_e_nearest(r2, 12), got = ec_divider_vout(vin, r1, std);
    fmt(r2, "Ohm", a, sizeof a);
    fmt(std, "Ohm", b, sizeof b);
    fmt(got, "V", c, sizeof c);
    snprintf(A->big, sizeof A->big, "R2 = %s", a);
    snprintf(A->sub, sizeof A->sub, "nearest E12 resistor: %s, which gives %s", b, c);
    snprintf(A->ex, sizeof A->ex, "R2 = R1 x Vout / (Vin - Vout). Then Vout = Vin x R2 / (R1 + R2).");
}

static void tool_555(Answer *A)
{
    Field *f = F[T_555];
    char a[24], b[24], c[80];
    if (f[N_MODE].choice == 1) {                     /* monostable: one pulse */
        double t = ec_555_mono(f[N_R1].val, f[N_C].val);
        fmt(t, "s", a, sizeof a);
        snprintf(A->big, sizeof A->big, "Pulse %s", a);
        snprintf(A->sub, sizeof A->sub, "t = 1.1 x R1 x C");
        snprintf(A->ex, sizeof A->ex, "Pull pin 2 (trigger) LOW for a moment: pin 3 (output) goes HIGH "
                 "for %s, then LOW again.", a);
        return;
    }
    Ec555 o;
    if (ec_555_astable(f[N_R1].val, f[N_R2].val, f[N_C].val, &o) != 0) { snprintf(A->big, sizeof A->big, "--"); return; }
    fmt(o.f, "Hz", a, sizeof a);
    snprintf(A->big, sizeof A->big, "%s", a);
    fmt(o.t_high, "s", a, sizeof a);
    fmt(o.t_low, "s", b, sizeof b);
    snprintf(A->sub, sizeof A->sub, "HIGH %s, LOW %s, duty %.0f%%", a, b, o.duty);
    if (o.f > 25) snprintf(c, sizeof c, "too fast to see: a tone or a clock");
    else if (o.f >= 1) snprintf(c, sizeof c, "an LED blinks %.0f times a second", o.f);
    else { fmt(o.period, "s", b, sizeof b); snprintf(c, sizeof c, "an LED blinks once every %s", b); }
    snprintf(A->ex, sizeof A->ex, "f = 1.44 / ((R1 + 2 x R2) x C): %s. HIGH is always longer than LOW "
             "with this circuit.", c);
}

static void tool_convert(Answer *A)
{
    static const char *const tips[EC_Q_COUNT] = {
        "1 A = 1000 mA = 1 000 000 uA. An LED wants about 20 mA.",
        "1 V = 1000 mV. USB gives 5 V; the Blue Pill and ESP32 use 3.3 V.",
        "1 kOhm = 1000 Ohm, 1 MOhm = 1000 kOhm. On a schematic 4k7 means 4.7 kOhm.",
        "1 uF = 1000 nF = 1 000 000 pF. \"104\" on a capacitor = 10 + four zeros pF = 100 nF.",
        "1 MHz = 1000 kHz = 1 000 000 Hz. An Arduino Uno runs at 16 MHz.",
        "1 inch = 25.4 mm. 1 mil = 1/1000 inch (PCB tracks). Breadboard holes are 0.1 inch apart.",
        "F = C x 9 / 5 + 32, K = C + 273.15. Water freezes at 0 C = 32 F, boils at 100 C = 212 F.",
    };
    Field *f = F[T_CONVERT];
    int k = f[U_KIND].choice;
    char a[32], b[32];
    if (k == KIND_AWG) {
        int awg = (int)lround(f[U_VALUE].val);
        snprintf(a, sizeof a, "%.3g", ec_awg_mm(awg));     /* 3 digits: 0.644 */
        snprintf(b, sizeof b, "%.3g", ec_awg_mm2(awg));
        snprintf(A->big, sizeof A->big, "%s mm", a);
        snprintf(A->sub, sizeof A->sub, "AWG %d: %s mm across, %s mm2 of copper", awg, a, b);
        snprintf(A->ex, sizeof A->ex, "A bigger AWG number is a thinner wire. Breadboard jumper wires "
                 "are usually AWG 22.");
        return;
    }
    double out;
    if (ec_convert(k, f[U_FROM].choice, f[U_TO].choice, f[U_VALUE].val, &out) != 0) {
        snprintf(A->big, sizeof A->big, "--");
        snprintf(A->sub, sizeof A->sub, "colder than absolute zero (-273.15 C)");
        snprintf(A->ex, sizeof A->ex, "Nothing can be colder than 0 K = -273.15 C = -459.67 F.");
        return;
    }
    fmt_plain(out, a, sizeof a);
    fmt_plain(f[U_VALUE].val, b, sizeof b);
    snprintf(A->big, sizeof A->big, "%s %s", a, ec_unit_name(k, f[U_TO].choice));
    snprintf(A->sub, sizeof A->sub, "%s %s = %s %s", b, ec_unit_name(k, f[U_FROM].choice),
             a, ec_unit_name(k, f[U_TO].choice));
    snprintf(A->ex, sizeof A->ex, "%s", tips[k]);
}

/* work the answer out again and show it, and the row values */
static void update(void)
{
    if (E.tool < 0 || IS_PINOUT(E.tool)) return;
    Answer A;
    memset(&A, 0, sizeof A);
    switch (E.tool) {
    case T_COLOUR:  tool_colour(&A); break;
    case T_FINDER:  tool_finder(&A); break;
    case T_OHM:     tool_ohm(&A); break;
    case T_LED:     tool_led(&A); break;
    case T_DIVIDER: tool_divider(&A); break;
    case T_555:     tool_555(&A); break;
    default:        tool_convert(&A); break;
    }
    lv_label_set_text(E.big, A.big);
    lv_label_set_text(E.sub, A.sub);
    if (A.sub[0]) lv_obj_clear_flag(E.sub, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(E.sub, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(E.explain, A.ex);
    if (E.draw) {
        if (A.nbands) { lv_obj_clear_flag(E.draw, LV_OBJ_FLAG_HIDDEN); draw_show(A.bands, A.nbands); }
        else lv_obj_add_flag(E.draw, LV_OBJ_FLAG_HIDDEN);
    }
    for (int r = 0; r < E.nrows; r++) {              /* the values on the right of each row */
        const Field *fd = &F[E.tool][E.row_field[r]];
        char t[64], v[96];
        field_text(fd, t, sizeof t);
        snprintf(v, sizeof v, LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT, t);
        lv_label_set_text(E.val[r], v);
        if (fd->colours) {                           /* the colour swatch */
            int c = field_colour(fd);
            lv_obj_clear_flag(E.swatch[r], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(E.swatch[r], lv_color_hex(real_rgb[c]), 0);
            lv_obj_set_style_bg_opa(E.swatch[r], c == EC_NONE ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        }
    }
}

/* ---------- picking and changing rows ---------- */
static void show_selection(void)
{
    for (int r = 0; r < E.nrows; r++) {
        if (r == E.sel) lv_obj_add_state(E.row[r], LV_STATE_CHECKED);
        else lv_obj_clear_state(E.row[r], LV_STATE_CHECKED);
    }
    if (E.nrows) lv_obj_scroll_to_view(E.row[E.sel], LV_ANIM_OFF);
}

/* after a change: extra work some fields need, then show it */
static void changed(int field)
{
    Field *f = F[E.tool];
    if (E.tool == T_LED && field == L_COLOUR) f[L_VF].val = led_vf[f[L_COLOUR].choice];
    if (E.tool == T_CONVERT && field == U_KIND) convert_kind_defaults();
    if (f[field].relayout) { set_visibility(E.tool); build_rows(); }
    else update();
}

static void change(int dir, int big)
{
    if (E.tool < 0 || !E.nrows) return;
    int fi = E.row_field[E.sel];
    Field *f = &F[E.tool][fi];
    if (f->kind == F_CHOICE) {
        f->choice = (f->choice + dir + f->n) % f->n;     /* goes round: after the last comes the first */
    } else {
        double v = f->val;
        if (f->step == S_LIN) {
            v += dir * f->lin * (big ? 10 : 1);
            v = round(v / f->lin) * f->lin;              /* no 3.2999999 after many steps */
        } else if (big) {
            v = dir > 0 ? v * 10 : v / 10;
        } else if (f->step == S_NICE) {
            v = ec_step_nice(v, dir);
        } else {
            v = ec_step_e(v, f->step == S_E24 ? 24 : 12, dir);
        }
        if (v < f->min) v = f->min;
        if (v > f->max) v = f->max;
        f->val = v;
    }
    changed(fi);
}

/* A on a number: type it, in the unit the row shows ("20 mA": type 15 for 15 mA)
 * on the number pad. A size letter typed on a USB keyboard works too: 4k7, 10u. */
static int prompt_field;
static double prompt_scale;                      /* 0.001 when the row shows mA, 1000 for kOhm ... */

/* split what a row shows, "20 mA", into "20" and "mA"; returns what 1 "mA" is in A */
static double shown_as(const Field *f, char *num, size_t nn, char *unit, size_t un)
{
    char t[48];
    unit[0] = 0;
    if (!f->unit) { fmt_plain(f->val, num, nn); return 1; }
    fmt(f->val, f->unit, t, sizeof t);
    char *sp = strchr(t, ' ');
    if (!sp) { snprintf(num, nn, "%s", t); snprintf(unit, un, "%s", f->unit); return 1; }
    *sp = 0;
    snprintf(num, nn, "%s", t);
    snprintf(unit, un, "%s", sp + 1);
    if (!strcmp(sp + 1, f->unit)) return 1;           /* no size letter: "5 V" */
    switch (sp[1]) {                                  /* the size letter in front of the unit */
    case 'p': return 1e-12;
    case 'n': return 1e-9;
    case 'u': return 1e-6;
    case 'm': return 1e-3;
    case 'k': return 1e3;
    case 'M': return 1e6;
    case 'G': return 1e9;
    case 'T': return 1e12;
    default:  return 1;
    }
}

static void typed(const char *text, void *ctx)
{
    (void)ctx;
    if (!text || E.tool < 0) return;                /* cancelled */
    Field *f = &F[E.tool][prompt_field];
    double v;
    if (ec_parse(text, &v) != 0) { ui_toast("That is not a number"); return; }
    int letters = 0;                                 /* "4k7" says its own size; "15" is in the unit shown */
    for (const char *c = text; *c; c++) if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) letters = 1;
    if (!letters) v *= prompt_scale;
    if (v < f->min || v > f->max) {
        char a[24], b[24], m[80];
        if (f->unit) { fmt(f->min, f->unit, a, sizeof a); fmt(f->max, f->unit, b, sizeof b); }
        else { fmt_plain(f->min, a, sizeof a); fmt_plain(f->max, b, sizeof b); }
        snprintf(m, sizeof m, "Pick between %s and %s", a, b);
        ui_toast(m);
        v = v < f->min ? f->min : f->max;
    }
    f->val = v;
    changed(prompt_field);
}

/* the prompt's on-screen keyboard (nzui.c), found on the top layer */
static lv_obj_t *find_keyboard(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_keyboard_class)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
        lv_obj_t *k = find_keyboard(lv_obj_get_child(o, i));
        if (k) return k;
    }
    return NULL;
}

/* A on a choice: a list to pick from */
static int menu_field;
static void picked(int choice, void *ctx)
{
    (void)ctx;
    if (choice < 0 || E.tool < 0) return;
    F[E.tool][menu_field].choice = choice;
    changed(menu_field);
}

static void edit(void)
{
    if (E.tool < 0 || !E.nrows) return;
    int fi = E.row_field[E.sel];
    Field *f = &F[E.tool][fi];
    if (f->kind == F_NUM) {
        char title[128], now[48], unit[48];
        prompt_scale = shown_as(f, now, sizeof now, unit, sizeof unit);
        if (unit[0]) snprintf(title, sizeof title, "%s, in %s", f->label, unit);
        else snprintf(title, sizeof title, "%s", f->label);
        prompt_field = fi;
        nz_prompt(title, now, typed, NULL);
        lv_obj_t *kb = find_keyboard(lv_layer_top());
        if (kb) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);   /* digits, point, +/- */
        return;
    }
    static char text[16][32];                         /* the choices as text, e.g. "red  x100" */
    const char *items[16] = { 0 };
    int n = f->n > 16 ? 16 : f->n;
    for (int i = 0; i < n; i++) {
        Field tmp = *f;
        tmp.choice = i;
        field_text(&tmp, text[i], sizeof text[i]);
        items[i] = text[i];
    }
    menu_field = fi;
    nz_menu(f->label, items, n, picked, NULL);
    for (int i = 0; i < f->choice && i < n - 1; i++) nzui_key(NZ_KEY_DOWN);   /* start on the current one */
}

/* mouse: a click on a row picks it (a second click edits it);
 * a click on the left / right half of the value changes it */
static void row_clicked(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    if (r == E.sel) { edit(); return; }
    E.sel = r;
    show_selection();
}

static void value_clicked(lv_event_t *e)
{
    lv_obj_t *v = lv_event_get_target(e);
    lv_point_t p;
    lv_area_t a;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_obj_get_coords(v, &a);
    E.sel = (int)(intptr_t)lv_event_get_user_data(e);
    show_selection();
    change(p.x < (a.x1 + a.x2) / 2 ? -1 : +1, 0);
}

/* the rows of the open tool (again after a choice that shows/hides rows) */
static void build_rows(void)
{
    int keep = E.nrows ? E.row_field[E.sel] : 0;
    lv_obj_clean(E.rows_box);
    E.nrows = 0;
    E.sel = 0;
    for (int i = 0; i < NF[E.tool]; i++) {
        const Field *f = &F[E.tool][i];
        if (f->hidden) continue;
        int r = E.nrows++;
        E.row_field[r] = i;
        if (i == keep) E.sel = r;
        lv_obj_t *row = E.row[r] = th_box(E.rows_box, TS_TEXT);
        th_add_to(row, TS_ROW_SEL, LV_STATE_CHECKED);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_t *l = lv_label_create(row);          /* the name, on the left */
        lv_label_set_text(l, f->label);
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        E.swatch[r] = th_box(row, TS_INPUT);         /* a little square of the band colour */
        lv_obj_set_size(E.swatch[r], 16, 16);
        lv_obj_set_style_radius(E.swatch[r], 3, 0);
        lv_obj_add_flag(E.swatch[r], LV_OBJ_FLAG_HIDDEN);
        E.val[r] = lv_label_create(row);             /* the value, on the right */
        lv_obj_add_flag(E.val[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(E.val[r], value_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    }
    update();
    show_selection();
}

/* ---------- pinouts ---------- */
/* one pin's name + note; the left side reads "note  NAME", the right "NAME  note" */
static void pin_row(lv_obj_t *col, const EcPin *p, int y, int right)
{
    lv_obj_t *row = th_box(col, -1);
    lv_obj_set_size(row, LV_PCT(100), PIN_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    char name[16];
    snprintf(name, sizeof name, "%s%s%s", (p->flags & EC_PIN_PWM) ? "~" : "", p->name,
             (p->flags & EC_PIN_5VT) ? "*" : "");
    int st = (p->flags & EC_PIN_POWER) ? TS_TITLE : (p->flags & EC_PIN_GND) ? TS_MUTED : TS_TEXT;
    lv_obj_t *note = th_label(row, p->note, TS_SMALL);
    lv_obj_t *nm = th_label(row, name, st);
    lv_obj_set_width(nm, 58);
    lv_obj_set_flex_grow(note, 1);
    lv_obj_set_height(note, 16);                     /* one line: "..." if it is too long */
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
    if (right) {                                     /* name first, both to the left */
        lv_obj_move_to_index(nm, 0);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    } else {                                         /* both to the right, next to the board */
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* the y position of each pin (a gap in the header = half a row) */
static int pin_ys(const EcPin *pins, int n, int *ys)
{
    int y = 0;
    for (int i = 0; i < n; i++) {
        if (pins[i].flags & EC_PIN_GAP) y += PIN_H / 2;
        ys[i] = y;
        y += PIN_H;
    }
    return y;
}

static void build_pinout(lv_obj_t *parent, const EcBoard *b)
{
    int yl[24], yr[24];
    int hl = pin_ys(b->left, b->n_left, yl), hr = pin_ys(b->right, b->n_right, yr);
    int h = (hl > hr ? hl : hr) + 2 * PIN_H;         /* a free row above and below for the end labels */

    char t[96];
    snprintf(t, sizeof t, "%s, %s. Seen from the top (chip side), %s end up.", b->chip, b->logic, b->top);
    lv_obj_t *info = th_label(parent, t, TS_SMALL);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    lv_obj_t *stage = th_box(parent, -1);            /* left pins | board | right pins */
    lv_obj_set_size(stage, LV_PCT(100), h);
    lv_obj_set_flex_flow(stage, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stage, 6, 0);
    lv_obj_t *left = th_box(stage, -1);
    lv_obj_set_height(left, h);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_t *board = th_box(stage, TS_PANEL);
    lv_obj_set_size(board, 140, h);
    lv_obj_set_style_shadow_width(board, 0, 0);
    lv_obj_t *right = th_box(stage, -1);
    lv_obj_set_height(right, h);
    lv_obj_set_flex_grow(right, 1);

    for (int side = 0; side < 2; side++) {
        const EcPin *pins = side ? b->right : b->left;
        int n = side ? b->n_right : b->n_left, *ys = side ? yr : yl;
        for (int i = 0; i < n; i++) {
            int y = ys[i] + PIN_H;
            pin_row(side ? right : left, &pins[i], y, side);
            lv_obj_t *pad = th_box(board, (pins[i].flags & EC_PIN_GND) ? TS_INPUT : TS_BTN_ACCENT);  /* GND: a ring */
            lv_obj_set_size(pad, 8, 8);
            lv_obj_set_style_radius(pad, 4, 0);      /* a round solder pad */
            lv_obj_align(pad, side ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, side ? -5 : 5, y + (PIN_H - 8) / 2 - 1);
        }
    }
    lv_obj_t *top = th_label(board, b->top, TS_KEYCAP);         /* what is at each end */
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 2);
    if (b->bottom && b->bottom[0]) {
        lv_obj_t *bot = th_label(board, b->bottom, TS_KEYCAP);
        lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
    lv_obj_t *mid = th_box(board, -1);               /* the board's name in the middle */
    lv_obj_set_size(mid, 116, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(mid, 6, 0);
    lv_obj_center(mid);
    lv_obj_t *nm = th_label(mid, b->title, TS_TITLE);
    lv_obj_set_width(nm, LV_PCT(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *chip = th_label(mid, b->chip, TS_SMALL);
    lv_obj_set_width(chip, LV_PCT(100));
    lv_label_set_long_mode(chip, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chip, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *legend = th_label(parent, b->legend, TS_TEXT);   /* what the marks mean */
    lv_obj_set_width(legend, LV_PCT(100));
    lv_label_set_long_mode(legend, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < 5 && b->notes[i]; i++) {
        snprintf(t, sizeof t, "-  %s", b->notes[i]);
        lv_obj_t *nt = th_label(parent, t, TS_MUTED);
        lv_obj_set_width(nt, LV_PCT(100));
        lv_label_set_long_mode(nt, LV_LABEL_LONG_WRAP);
    }
}

/* ---------- fitting a desktop window ----------
 * In a window (or with the CRT themes' big font) there is less room:
 *   - the resistor drawing goes above the big answer instead of beside it
 *   - the tool's name uses the smaller title font if the big one doesn't fit
 * Checked when a tool opens and every frame (the window, theme or mode can change). */
static void fit(void)
{
    if (E.tool < 0 || !E.head) return;
    static lv_coord_t last_room = -1;
    static int last_theme = -1;
    lv_coord_t room = lv_obj_get_content_width(E.head) - lv_obj_get_width(E.back) - 12;
    if (E.refit) { last_room = -1; E.refit = 0; }   /* a tool just opened: measure again */
    if (room > 0 && (room != last_room || theme_index() != last_theme)) {   /* only when something changed */
        last_room = room;
        last_theme = theme_index();
        lv_point_t size;                             /* how wide the name is in the heading font */
        lv_txt_get_size(&size, lv_label_get_text(E.title), theme()->font_heading, 0, 0,
                        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int small = size.x > room;
        if (small != E.small_title) {
            lv_obj_remove_style(E.title, th_style(small ? TS_HEADING : TS_TITLE), 0);
            th_add(E.title, small ? TS_TITLE : TS_HEADING);
            E.small_title = small;
        }
    }
    if (!E.draw || !E.card) return;
    lv_coord_t w = lv_obj_get_content_width(E.card);
    if (w <= 0) return;
    int stacked = w < DRAW_W + 16 + 200;             /* not enough room for the answer beside it */
    if (stacked == E.stacked) return;
    E.stacked = stacked;
    lv_obj_set_flex_flow(E.top, stacked ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(E.big, stacked ? 0 : 1);    /* (in a column, growing would mean taller) */
    lv_obj_set_width(E.big, stacked ? LV_PCT(100) : 0);
}

static void tick(NzApp *a) { (void)a; fit(); }

/* ---------- opening and closing a tool ---------- */
static void close_tool(void);
static void back_clicked(lv_event_t *e) { (void)e; close_tool(); }

static void open_tool(int t, void *ctx)
{
    (void)ctx;
    if (t < 0 || t >= T_COUNT) return;
    lv_obj_t *root = lv_obj_get_parent(E.list_page);
    E.tool = t;
    E.nrows = 0;
    E.draw = NULL;
    lv_obj_add_flag(E.list_page, LV_OBJ_FLAG_HIDDEN);

    E.tool_page = th_box(root, -1);
    lv_obj_set_width(E.tool_page, LV_PCT(100));
    lv_obj_set_flex_grow(E.tool_page, 1);
    lv_obj_set_flex_flow(E.tool_page, LV_FLEX_FLOW_COLUMN);

    E.scroll = th_box(E.tool_page, -1);              /* everything above the hint bar; it scrolls */
    lv_obj_set_width(E.scroll, LV_PCT(100));
    lv_obj_set_flex_grow(E.scroll, 1);
    lv_obj_add_flag(E.scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(E.scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(E.scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(E.scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(E.scroll, 10, 0);
    lv_obj_set_style_pad_row(E.scroll, 8, 0);

    lv_obj_t *head = E.head = th_box(E.scroll, -1);  /* "< Tools"  Tool name */
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    lv_obj_t *back = E.back = th_box(head, TS_BTN);
    lv_obj_set_size(back, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(back, 8, 0);
    lv_obj_set_style_pad_ver(back, 3, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    th_label(back, LV_SYMBOL_LEFT " Tools", TS_SMALL);
    E.title = th_label(head, tool_info[t].title, TS_HEADING);
    E.small_title = 0;

    if (IS_PINOUT(t)) {
        build_pinout(E.scroll, ec_board(t - T_BLUEPILL));
    } else {
        lv_obj_t *card = E.card = th_box(E.scroll, TS_PANEL);  /* the answer */
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        E.top = th_box(card, -1);                    /* drawing + big answer, side by side */
        lv_obj_set_size(E.top, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(E.top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(E.top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(E.top, 16, 0);
        lv_obj_set_style_pad_row(E.top, 4, 0);
        if (t == T_COLOUR || t == T_FINDER || t == T_LED) draw_create(E.top);
        E.big = th_label(E.top, "", TS_BIG);
        lv_obj_set_flex_grow(E.big, 1);
        lv_label_set_long_mode(E.big, LV_LABEL_LONG_WRAP);
        E.sub = th_label(card, "", TS_TEXT);         /* full width: numbers must not break in two */
        lv_obj_set_width(E.sub, LV_PCT(100));
        lv_label_set_long_mode(E.sub, LV_LABEL_LONG_WRAP);
        E.explain = th_label(card, "", TS_MUTED);
        lv_obj_set_width(E.explain, LV_PCT(100));
        lv_label_set_long_mode(E.explain, LV_LABEL_LONG_WRAP);

        E.rows_box = th_box(E.scroll, -1);           /* the values you can change */
        lv_obj_set_size(E.rows_box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(E.rows_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(E.rows_box, 2, 0);
        set_visibility(t);
        build_rows();
        lv_obj_scroll_to_y(E.scroll, 0, LV_ANIM_OFF);
    }

    lv_obj_t *hint = th_box(E.tool_page, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const calc_hints[] = { LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "Change", "L1/R1", "Big step",
                                              "A", "Edit", "B", "Tools" };
    static const char *const pin_hints[] = { "UP/DOWN", "Scroll", "B", "Tools" };
    if (IS_PINOUT(t)) lv_obj_center(nz_hints(hint, pin_hints, 2));
    else lv_obj_center(nz_hints(hint, calc_hints, 4));
    E.stacked = -1;
    E.refit = 1;
    lv_obj_update_layout(E.tool_page);               /* sizes are known now: fit to them */
    fit();
}

static void close_tool(void)
{
    if (E.tool < 0) return;
    lv_obj_del_async(E.tool_page);                   /* async: we may be inside its own click */
    E.tool_page = E.scroll = E.rows_box = E.big = E.sub = E.explain = E.draw = E.body = NULL;
    E.card = E.top = E.head = E.back = E.title = NULL;
    E.nrows = 0;
    E.tool = -1;
    lv_obj_clear_flag(E.list_page, LV_OBJ_FLAG_HIDDEN);
}

/* UP/DOWN on a pinout: scroll, but not past the ends */
static void scroll_pinout(int dy)
{
    lv_obj_update_layout(E.scroll);
    if (dy > 0) {                                    /* down: show more of the bottom */
        lv_coord_t room = lv_obj_get_scroll_bottom(E.scroll);
        if (dy > room) dy = room;
        if (dy > 0) lv_obj_scroll_by(E.scroll, 0, -dy, LV_ANIM_OFF);
    } else {
        lv_coord_t room = lv_obj_get_scroll_top(E.scroll);
        if (-dy > room) dy = -room;
        if (dy < 0) lv_obj_scroll_by(E.scroll, 0, -dy, LV_ANIM_OFF);
    }
}

/* ---------- the app ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&E, 0, sizeof E);
    E.tool = -1;
    set_defaults();
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    E.list_page = th_box(root, -1);
    lv_obj_set_width(E.list_page, LV_PCT(100));
    lv_obj_set_flex_grow(E.list_page, 1);
    lv_obj_set_flex_flow(E.list_page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = th_box(E.list_page, TS_PANEL);  /* what this is */
    lv_obj_set_size(card, LV_PCT(100), 64);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_ELECTRONICS, 48));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = th_label(card, "Electronics", TS_HEADING);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 72, 8);
    lv_obj_t *s = th_label(card, "Calculators and pinouts, offline", TS_SMALL);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, LV_PCT(80));
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 72, -9);

    E.list = nzlist_create(E.list_page, 0);
    lv_obj_set_width(nzlist_obj(E.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(E.list), 1);
    nzlist_on_activate(E.list, open_tool, NULL);
    for (int i = 0; i < T_COUNT; i++)
        nzlist_add(E.list, tool_info[i].icon, tool_info[i].title, tool_info[i].sub, NULL);

    lv_obj_t *hint = th_box(E.list_page, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Open", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 2));
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (E.tool < 0) {                                /* the tool list */
        if (k == NZ_KEY_B) return 0;                 /* not ours: NZEUS closes the app */
        return nzlist_key(E.list, k);
    }
    if (k == NZ_KEY_B) { close_tool(); return 1; }
    if (IS_PINOUT(E.tool)) {
        if (k == NZ_KEY_DOWN) scroll_pinout(3 * PIN_H);
        else if (k == NZ_KEY_UP) scroll_pinout(-3 * PIN_H);
        else if (k == NZ_KEY_R) scroll_pinout(lv_obj_get_height(E.scroll) - 40);
        else if (k == NZ_KEY_L) scroll_pinout(-(lv_obj_get_height(E.scroll) - 40));
        return 1;
    }
    switch (k) {
    case NZ_KEY_UP:    if (E.sel > 0) { E.sel--; show_selection(); } break;
    case NZ_KEY_DOWN:  if (E.sel < E.nrows - 1) { E.sel++; show_selection(); } break;
    case NZ_KEY_LEFT:  change(-1, 0); break;
    case NZ_KEY_RIGHT: change(+1, 0); break;
    case NZ_KEY_L:     change(-1, 1); break;
    case NZ_KEY_R:     change(+1, 1); break;
    case NZ_KEY_A:     edit(); break;
    default:           break;
    }
    return 1;
}

/* a USB keyboard: arrows, Enter and Esc work like the buttons */
static void kbd(NzApp *a, const KbEvent *ev)
{
    switch (ev->key) {
    case KB_UP:    key(a, NZ_KEY_UP); break;
    case KB_DOWN:  key(a, NZ_KEY_DOWN); break;
    case KB_LEFT:  key(a, NZ_KEY_LEFT); break;
    case KB_RIGHT: key(a, NZ_KEY_RIGHT); break;
    case KB_PGUP:  key(a, NZ_KEY_L); break;
    case KB_PGDN:  key(a, NZ_KEY_R); break;
    case KB_ENTER: key(a, NZ_KEY_A); break;
    case KB_ESC:   if (E.tool >= 0) close_tool(); break;
    default: break;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    nzlist_free(E.list);
    memset(&E, 0, sizeof E);
    E.tool = -1;
}

/* ---------- for tests ---------- */
int app_electronics_tool(void) { return E.tool; }
const char *app_electronics_answer(void) { return (E.tool >= 0 && E.big) ? lv_label_get_text(E.big) : ""; }
const char *app_electronics_sub(void) { return (E.tool >= 0 && E.sub) ? lv_label_get_text(E.sub) : ""; }
const char *app_electronics_explain(void) { return (E.tool >= 0 && E.explain) ? lv_label_get_text(E.explain) : ""; }
int app_electronics_band(int i) { return (E.draw && i >= 0 && i < 5) ? E.band_col[i] : -1; }
int app_electronics_rows(void) { return E.nrows; }
int app_electronics_selected_row(void) { return E.sel; }
void app_electronics_test_open(int t) { if (t < 0) close_tool(); else open_tool(t, NULL); }

NzApp app_electronics = {
    .id = APP_ELECTRONICS, .title = "Electronics", .icon = ICON_ELECTRONICS,
    .create = create, .key = key, .kbd = kbd, .tick = tick, .destroy = destroy,
};

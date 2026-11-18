/* elec_calc.c - the maths and reference data behind the Electronics app.
 * See elec_calc.h. Everything here is plain C: no screen, no files, so the
 * PC test (tests/test_electronics.c) can check every answer. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elec_calc.h"

/* ---------- resistor colours ---------- */
static const char *const colour_names[EC_COLOUR_COUNT] = {
    "black", "brown", "red", "orange", "yellow", "green", "blue", "violet", "grey", "white",
    "gold", "silver", "none",
};

const char *ec_colour_name(int c) { return (c >= 0 && c < EC_COLOUR_COUNT) ? colour_names[c] : "?"; }

/* the third band (fourth on 5-band resistors): how many zeros to add */
double ec_multiplier(int c)
{
    if (c >= EC_BLACK && c <= EC_WHITE) return pow(10, c);   /* black x1, brown x10, red x100 ... */
    if (c == EC_GOLD) return 0.1;
    if (c == EC_SILVER) return 0.01;
    return -1;
}

/* the last band: how far off the real value may be, in %
 * (IEC 60062:2016; orange, yellow and grey are rare precision resistors) */
double ec_tolerance(int c)
{
    switch (c) {
    case EC_BROWN:  return 1;
    case EC_RED:    return 2;
    case EC_ORANGE: return 0.05;
    case EC_YELLOW: return 0.02;
    case EC_GREY:   return 0.01;
    case EC_GREEN:  return 0.5;
    case EC_BLUE:   return 0.25;
    case EC_VIOLET: return 0.1;
    case EC_GOLD:   return 5;
    case EC_SILVER: return 10;
    case EC_NONE:   return 20;          /* no band at all */
    default:        return -1;
    }
}

double ec_bands_to_ohms(const int *b, int n, double *tol)
{
    if (n != 4 && n != 5) return -1;
    int digits = n - 2;                              /* 2 digit bands (4-band) or 3 (5-band) */
    double value = 0;
    for (int i = 0; i < digits; i++) {
        if (b[i] < EC_BLACK || b[i] > EC_WHITE) return -1;   /* only black..white are digits */
        value = value * 10 + b[i];                   /* e.g. brown, black -> 1, then 10 */
    }
    double m = ec_multiplier(b[digits]), t = ec_tolerance(b[digits + 1]);
    if (m < 0 || t < 0) return -1;
    if (tol) *tol = t;
    return value * m;
}

int ec_ohms_to_bands(double ohms, int n, int *b)
{
    if ((n != 4 && n != 5) || !(ohms > 0)) return -1;
    int digits = n - 2;
    /* e = the multiplier's power of ten, so ohms / 10^e has `digits` digits:
     * 4700 with 2 digits -> e = 2 (47 x 100) */
    int e = (int)floor(log10(ohms) + 1e-9) - (digits - 1);
    double top = pow(10, digits);                    /* 100 (2 digits) or 1000 (3 digits) */
    long v = lround(ohms / pow(10, e));
    if (v >= (long)top) { v /= 10; e++; }            /* 99.6 rounded up to 100: one more zero */
    if (e < -2 || e > 9) return -1;                  /* silver (x0.01) .. white (x1 000 000 000) */
    for (int i = digits - 1; i >= 0; i--) { b[i] = (int)(v % 10); v /= 10; }
    b[digits] = e >= 0 ? e : (e == -1 ? EC_GOLD : EC_SILVER);
    return 0;
}

void ec_bands_text(const int *b, int n, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < n && len < size; i++) {
        if (b[i] == EC_NONE) continue;
        len += snprintf(buf + len, size - len, "%s%s", len ? "-" : "", ec_colour_name(b[i]));
    }
}

/* ---------- standard values ---------- */
static const double E12[12] = { 1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2 };
static const double E24[24] = { 1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1 };
/* "round" values for volts, amps, watts: the E12 values plus 2, 2.5, 3, 3.6, 4, 4.5, 5, 6, 7.5, 8, 9 */
static const double NICE[23] = { 1, 1.2, 1.5, 1.8, 2, 2.2, 2.5, 2.7, 3, 3.3, 3.6, 3.9,
                                 4, 4.5, 4.7, 5, 5.6, 6, 6.8, 7.5, 8, 8.2, 9 };

/* A series is the same list in every decade: ... 0.47, 4.7, 47, 470 ...
 * value number k = list[k mod n] x 10^(k div n). */
static double series_value(const double *list, int n, int k)
{
    int d = k >= 0 ? k / n : -((-k + n - 1) / n);    /* division that rounds down, also below 0 */
    return list[k - d * n] * pow(10, d);
}

/* the first value number to look at, one decade below v */
static int series_start(int n, double v) { return n * ((int)floor(log10(v)) - 1); }

#define CLOSE 1e-9                                   /* "equal" for float values */

double ec_e_up(double v, int series)
{
    const double *list = series == 24 ? E24 : E12;
    int n = series == 24 ? 24 : 12;
    if (!(v > 0)) return list[0];
    for (int k = series_start(n, v); ; k++) {
        double s = series_value(list, n, k);
        if (s >= v * (1 - CLOSE)) return s;
    }
}

double ec_e_nearest(double v, int series)
{
    const double *list = series == 24 ? E24 : E12;
    int n = series == 24 ? 24 : 12;
    if (!(v > 0)) return list[0];
    double up = ec_e_up(v, series), down = up;
    for (int k = series_start(n, v) + 3 * n; k >= series_start(n, v) - n; k--) {
        double s = series_value(list, n, k);
        if (s <= v * (1 + CLOSE)) { down = s; break; }
    }
    return (up / v < v / down) ? up : down;          /* closest by ratio (4.7k vs 5.6k for 5k: 4.7k) */
}

static double series_step(const double *list, int n, double v, int dir)
{
    if (!(v > 0)) return list[0];
    int k0 = series_start(n, v);
    if (dir > 0) {
        for (int k = k0; k < k0 + 3 * n; k++) {
            double s = series_value(list, n, k);
            if (s > v * (1 + CLOSE)) return s;
        }
    } else {
        for (int k = k0 + 3 * n; k >= k0 - n; k--) {
            double s = series_value(list, n, k);
            if (s < v * (1 - CLOSE)) return s;
        }
    }
    return v;
}

double ec_step_e(double v, int series, int dir)
{
    return series == 24 ? series_step(E24, 24, v, dir) : series_step(E12, 12, v, dir);
}

double ec_step_nice(double v, int dir) { return series_step(NICE, 23, v, dir); }

/* ---------- Ohm's law ---------- */
int ec_ohms_law(int qa, double a, int qb, double b, EcOhm *o)
{
    if (qa == qb || !(a > 0) || !(b > 0)) return -1;
    double v = -1, i = -1, r = -1, p = -1;
    if (qa == EC_V) v = a; else if (qa == EC_I) i = a; else if (qa == EC_R) r = a; else if (qa == EC_P) p = a; else return -1;
    if (qb == EC_V) v = b; else if (qb == EC_I) i = b; else if (qb == EC_R) r = b; else if (qb == EC_P) p = b; else return -1;

    if (v > 0 && i > 0)      { r = v / i;          p = v * i; }      /* R = V / I,  P = V x I */
    else if (v > 0 && r > 0) { i = v / r;          p = v * v / r; }  /* I = V / R,  P = V^2 / R */
    else if (i > 0 && r > 0) { v = i * r;          p = i * i * r; }  /* V = I x R,  P = I^2 x R */
    else if (p > 0 && v > 0) { i = p / v;          r = v * v / p; }  /* I = P / V,  R = V^2 / P */
    else if (p > 0 && i > 0) { v = p / i;          r = p / (i * i); }/* V = P / I,  R = P / I^2 */
    else if (p > 0 && r > 0) { i = sqrt(p / r);    v = sqrt(p * r); }/* I = root(P / R), V = root(P x R) */
    o->v = v; o->i = i; o->r = r; o->p = p;
    return 0;
}

/* ---------- LED series resistor ---------- */
int ec_led_resistor(double supply, double vf, double current, int series, EcLed *o)
{
    if (!(current > 0) || !(vf >= 0) || !(supply > vf)) return -1;
    double drop = supply - vf;                       /* the volts the resistor must "eat" */
    static const double ratings[] = { 0.25, 0.5, 1, 2, 5 };
    o->r = drop / current;
    o->r_std = ec_e_up(o->r, series);                /* bigger = a bit less current = safe */
    o->i_std = drop / o->r_std;
    o->p_r = drop * o->i_std;
    o->p_led = vf * o->i_std;
    o->rating = -1;                                  /* -1: over 5 W, not a job for one resistor */
    for (int k = 0; k < 5; k++)
        if (ratings[k] >= 2 * o->p_r) { o->rating = ratings[k]; break; }   /* twice the heat: stays cool */
    return 0;
}

/* ---------- voltage divider ---------- */
double ec_divider_vout(double vin, double r1, double r2)
{
    if (!(vin >= 0) || !(r1 >= 0) || !(r2 >= 0) || !(r1 + r2 > 0)) return -1;
    return vin * r2 / (r1 + r2);
}

double ec_divider_r2(double vin, double vout, double r1)
{
    if (!(vout > 0) || !(vin > vout) || !(r1 > 0)) return -1;
    return r1 * vout / (vin - vout);                 /* from Vout = Vin x R2 / (R1 + R2) */
}

/* ---------- 555 timer ----------
 * The capacitor charges through R1 + R2 (output HIGH) and empties through
 * R2 only (output LOW). Each half takes ln(2) x R x C = 0.693 x R x C. */
int ec_555_astable(double r1, double r2, double c, Ec555 *o)
{
    if (!(r1 > 0) || !(r2 > 0) || !(c > 0)) return -1;
    o->t_high = 0.693 * (r1 + r2) * c;
    o->t_low  = 0.693 * r2 * c;
    o->period = o->t_high + o->t_low;
    o->f = 1 / o->period;
    o->duty = 100 * o->t_high / o->period;
    return 0;
}

double ec_555_mono(double r1, double c)
{
    if (!(r1 > 0) || !(c > 0)) return -1;
    return 1.1 * r1 * c;                             /* charging to 2/3 of the supply: ln(3) = 1.1 */
}

/* ---------- unit conversion ---------- */
typedef struct { const char *name; int n; const char *units[5]; double factor[5]; } Quantity;
static const Quantity quantities[EC_Q_COUNT] = {
    /* factor: how many base units (A, V, ohm, F, Hz, m) one of these is */
    { "Current",     3, { "uA", "mA", "A" },                  { 1e-6, 1e-3, 1 } },
    { "Voltage",     3, { "mV", "V", "kV" },                  { 1e-3, 1, 1e3 } },
    { "Resistance",  3, { "Ohm", "kOhm", "MOhm" },            { 1, 1e3, 1e6 } },
    { "Capacitance", 4, { "pF", "nF", "uF", "F" },            { 1e-12, 1e-9, 1e-6, 1 } },
    { "Frequency",   4, { "Hz", "kHz", "MHz", "GHz" },        { 1, 1e3, 1e6, 1e9 } },
    { "Length",      5, { "mm", "cm", "m", "inch", "mil" },   { 1e-3, 1e-2, 1, 0.0254, 0.0000254 } },
    { "Temperature", 3, { "C", "F", "K" },                    { 1, 1, 1 } },   /* (special: offsets) */
};

const char *ec_quantity_name(int q) { return (q >= 0 && q < EC_Q_COUNT) ? quantities[q].name : "?"; }
int ec_unit_count(int q) { return (q >= 0 && q < EC_Q_COUNT) ? quantities[q].n : 0; }
const char *ec_unit_name(int q, int u) { return (u >= 0 && u < ec_unit_count(q)) ? quantities[q].units[u] : "?"; }

int ec_convert(int q, int from, int to, double v, double *out)
{
    if (from < 0 || to < 0 || from >= ec_unit_count(q) || to >= ec_unit_count(q)) return -1;
    if (q == EC_Q_TEMPERATURE) {                     /* via Celsius */
        double c = from == 0 ? v : from == 1 ? (v - 32) * 5 / 9 : v - 273.15;
        if (c < -273.15 - 1e-9) return -1;           /* colder than absolute zero: impossible */
        *out = to == 0 ? c : to == 1 ? c * 9 / 5 + 32 : c + 273.15;
        return 0;
    }
    *out = v * quantities[q].factor[from] / quantities[q].factor[to];
    return 0;
}

/* American Wire Gauge: AWG 36 is 0.127 mm, and every 39 steps the diameter
 * changes 92 times. A bigger number is a thinner wire. */
double ec_awg_mm(int awg)
{
    if (awg < 0 || awg > 40) return -1;
    return 0.127 * pow(92, (36 - awg) / 39.0);
}

double ec_awg_mm2(int awg)
{
    double d = ec_awg_mm(awg);
    return d < 0 ? -1 : 3.14159265358979 / 4 * d * d;   /* circle area: pi / 4 x d^2 */
}

/* ---------- numbers as text ---------- */
static void strip_zeros(char *s)                     /* "4.70" -> "4.7", "10.0" -> "10" */
{
    if (!strchr(s, '.')) return;
    size_t n = strlen(s);
    while (n && s[n - 1] == '0') s[--n] = 0;
    if (n && s[n - 1] == '.') s[--n] = 0;
}

void ec_format(double v, const char *unit, char *buf, size_t size)
{
    static const char *const prefix[9] = { "p", "n", "u", "m", "", "k", "M", "G", "T" };  /* 10^-12 .. 10^12 */
    char num[32];
    if (!unit) unit = "";
    if (v != v || isinf(v)) { snprintf(buf, size, "?"); return; }
    if (v == 0) { snprintf(buf, size, unit[0] ? "0 %s" : "0", unit); return; }
    double a = fabs(v);
    int g = (int)floor(log10(a) / 3);                /* the group of 3 zeros: k = 1, M = 2, m = -1 ... */
    if (g < -4) g = -4;
    if (g > 4) g = 4;
    for (int pass = 0; pass < 2; pass++) {
        double m = a / pow(10, 3 * g);
        int dec = m >= 100 ? 0 : m >= 10 ? 1 : 2;    /* 3 significant digits: 150, 4.70, 6.86 */
        snprintf(num, sizeof num, "%.*f", dec, m);
        if (atof(num) < 1000 || g == 4) break;
        g++;                                         /* 999.7 became "1000": show it as 1 k */
    }
    strip_zeros(num);
    if (!prefix[g + 4][0] && !unit[0]) snprintf(buf, size, "%s%s", v < 0 ? "-" : "", num);
    else snprintf(buf, size, "%s%s %s%s", v < 0 ? "-" : "", num, prefix[g + 4], unit);
}

int ec_parse(const char *s, double *out)
{
    if (!s) return -1;
    while (isspace((unsigned char)*s)) s++;
    if (!(isdigit((unsigned char)*s) || *s == '.' || *s == '-' || *s == '+')) return -1;
    char *end;
    double x = strtod(s, &end);
    if (end == s || x != x || isinf(x)) return -1;
    int had_point = 0;                               /* "4.7k" has one; "4k7" has not */
    for (const char *c = s; c < end; c++) if (*c == '.' || *c == 'e' || *c == 'E') had_point = 1;
    const char *p = end;
    while (*p == ' ') p++;
    double mult = 0;
    switch (*p) {                                    /* the size letter after the number */
    case 'p': case 'P': mult = 1e-12; break;
    case 'n': case 'N': mult = 1e-9; break;
    case 'u': case 'U': mult = 1e-6; break;
    case 'm':           mult = 1e-3; break;          /* small m = milli */
    case 'k': case 'K': mult = 1e3; break;
    case 'M':           mult = 1e6; break;           /* big M = mega */
    case 'G': case 'g': mult = 1e9; break;
    case 'R': case 'r': mult = 1; break;             /* 4R7 = 4.7 ohm (R used as the point) */
    default: break;
    }
    if (mult != 0) {
        p++;
        if (!had_point && p == end + 1) {            /* "4k7": the digits after the letter are decimals */
            double scale = 0.1;
            for (; isdigit((unsigned char)*p); p++, scale /= 10) x += (*p - '0') * scale * (x < 0 ? -1 : 1);
        }
        x *= mult;
    }
    *out = x;
    return 0;
}

/* ---------- board pinouts ----------
 * Each board is seen from the top (the side with the chip), with the end
 * named in .top at the top of the screen. Pins are listed top to bottom. */
#define PWR  EC_PIN_POWER
#define GND  EC_PIN_GND
#define FT   EC_PIN_5VT
#define PWM  EC_PIN_PWM
#define GAP  EC_PIN_GAP

/* STM32 "Blue Pill" (STM32F103C8T6). 5 V tolerant (FT) pins from the
 * STM32F103x8 datasheet pin table: all except PA0-PA7, PB0, PB1, PB5, PC13-PC15.
 * Header order checked on a photo of the board (stm32-base.org): with the USB
 * end up, B12 is top LEFT (next to G, G, 3.3, R on the right) and VB is at the
 * bottom right, by the SWD header. */
static const EcPin bp_left[] = {
    { "B12", "NSS2", FT },
    { "B13", "SCK2", FT },
    { "B14", "MISO2", FT },
    { "B15", "MOSI2", FT },
    { "A8",  "PWM", FT },
    { "A9",  "TX1", FT },
    { "A10", "RX1", FT },
    { "A11", "USB D-", FT },
    { "A12", "USB D+", FT },
    { "A15", "JTAG JTDI", FT },
    { "B3",  "JTAG JTDO", FT },
    { "B4",  "JTAG TRST", FT },
    { "B5",  "NOT 5V tol.", 0 },
    { "B6",  "SCL1", FT },
    { "B7",  "SDA1", FT },
    { "B8",  "SCL1 alt", FT },
    { "B9",  "SDA1 alt", FT },
    { "5V",  "5 V in/out", PWR },
    { "G",   "GND", GND },
    { "3.3", "3.3 V", PWR },
};
static const EcPin bp_right[] = {
    { "G",   "GND", GND },
    { "G",   "GND", GND },
    { "3.3", "3.3 V", PWR },
    { "R",   "reset", 0 },
    { "B11", "RX3 SDA2", FT },
    { "B10", "TX3 SCL2", FT },
    { "B1",  "ADC9 PWM", 0 },
    { "B0",  "ADC8 PWM", 0 },
    { "A7",  "ADC7 MOSI1", 0 },
    { "A6",  "ADC6 MISO1", 0 },
    { "A5",  "ADC5 SCK1", 0 },
    { "A4",  "ADC4 NSS1", 0 },
    { "A3",  "ADC3 RX2", 0 },
    { "A2",  "ADC2 TX2", 0 },
    { "A1",  "ADC1 PWM", 0 },
    { "A0",  "ADC0 PWM", 0 },
    { "C15", "32kHz xtal", 0 },
    { "C14", "32kHz xtal", 0 },
    { "C13", "LED, on=LOW", 0 },
    { "VB",  "RTC battery", PWR },
};

/* Arduino Uno R3: left = power + analog headers, right = digital header */
static const EcPin uno_left[] = {
    { "NC",    "not used", 0 },
    { "IOREF", "5 V ref", PWR },
    { "RESET", "LOW=reset", 0 },
    { "3.3V",  "50 mA max", PWR },
    { "5V",    "5 V", PWR },
    { "GND",   "", GND },
    { "GND",   "", GND },
    { "VIN",   "7-12 V in", PWR },
    { "A0",    "analog in", GAP },
    { "A1",    "analog in", 0 },
    { "A2",    "analog in", 0 },
    { "A3",    "analog in", 0 },
    { "A4",    "SDA (I2C)", 0 },
    { "A5",    "SCL (I2C)", 0 },
};
static const EcPin uno_right[] = {
    { "SCL",  "same as A5", 0 },
    { "SDA",  "same as A4", 0 },
    { "AREF", "analog ref", 0 },
    { "GND",  "", GND },
    { "D13",  "SCK, LED", 0 },
    { "D12",  "MISO", 0 },
    { "D11",  "MOSI", PWM },
    { "D10",  "SS", PWM },
    { "D9",   "", PWM },
    { "D8",   "", 0 },
    { "D7",   "", GAP },
    { "D6",   "", PWM },
    { "D5",   "", PWM },
    { "D4",   "", 0 },
    { "D3",   "INT1", PWM },
    { "D2",   "INT0", 0 },
    { "D1",   "TX", 0 },
    { "D0",   "RX", 0 },
};

/* Arduino Nano (ATmega328P): pin 16 (D13) top left ... pin 30 (VIN) bottom left,
 * pin 15 (D12) top right ... pin 1 (D1/TX) bottom right */
static const EcPin nano_left[] = {
    { "D13",  "SCK, LED", 0 },
    { "3V3",  "3.3 V out", PWR },
    { "AREF", "analog ref", 0 },
    { "A0",   "analog in", 0 },
    { "A1",   "analog in", 0 },
    { "A2",   "analog in", 0 },
    { "A3",   "analog in", 0 },
    { "A4",   "SDA (I2C)", 0 },
    { "A5",   "SCL (I2C)", 0 },
    { "A6",   "analog only", 0 },
    { "A7",   "analog only", 0 },
    { "5V",   "5 V", PWR },
    { "RST",  "LOW=reset", 0 },
    { "GND",  "", GND },
    { "VIN",  "7-12 V in", PWR },
};
static const EcPin nano_right[] = {
    { "D12", "MISO", 0 },
    { "D11", "MOSI", PWM },
    { "D10", "SS", PWM },
    { "D9",  "", PWM },
    { "D8",  "", 0 },
    { "D7",  "", 0 },
    { "D6",  "", PWM },
    { "D5",  "", PWM },
    { "D4",  "", 0 },
    { "D3",  "INT1", PWM },
    { "D2",  "INT0", 0 },
    { "GND", "", GND },
    { "RST", "LOW=reset", 0 },
    { "D0",  "RX", 0 },
    { "D1",  "TX", 0 },
};

/* ESP32 DevKit (the 30-pin DOIT ESP32 DEVKIT V1), antenna at the top */
static const EcPin esp_left[] = {
    { "EN",  "reset (LOW)", 0 },
    { "VP",  "IO36 in only", 0 },
    { "VN",  "IO39 in only", 0 },
    { "D34", "input only", 0 },
    { "D35", "input only", 0 },
    { "D32", "ADC1 touch", 0 },
    { "D33", "ADC1 touch", 0 },
    { "D25", "DAC1 ADC2", 0 },
    { "D26", "DAC2 ADC2", 0 },
    { "D27", "ADC2 touch", 0 },
    { "D14", "ADC2 touch", 0 },
    { "D12", "LOW at boot!", 0 },
    { "D13", "ADC2 touch", 0 },
    { "GND", "", GND },
    { "VIN", "5 V in", PWR },
};
static const EcPin esp_right[] = {
    { "D23", "MOSI", 0 },
    { "D22", "SCL (I2C)", 0 },
    { "TX0", "IO1 USB TX", 0 },
    { "RX0", "IO3 USB RX", 0 },
    { "D21", "SDA (I2C)", 0 },
    { "D19", "MISO", 0 },
    { "D18", "SCK", 0 },
    { "D5",  "SS, boot pin", 0 },
    { "TX2", "IO17 TX2", 0 },
    { "RX2", "IO16 RX2", 0 },
    { "D4",  "ADC2 touch", 0 },
    { "D2",  "LED, boot pin", 0 },
    { "D15", "boot pin", 0 },
    { "GND", "", GND },
    { "3V3", "3.3 V", PWR },
};

#define COUNT(a) (int)(sizeof a / sizeof a[0])
static const EcBoard boards[EC_BOARD_COUNT] = {
    [EC_BOARD_BLUEPILL] = {
        "STM32 Blue Pill", "STM32F103C8T6", "3.3 V logic, 72 MHz", "USB", "SWD",
        bp_left, COUNT(bp_left), bp_right, COUNT(bp_right),
        "* = 5 V tolerant (as a digital pin). The others: 3.3 V only!",
        /* datasheet: +-8 mA per pin keeps the voltages right, 25 mA is the absolute max;
         * PC13-PC15 sink only 3 mA and must not source current (e.g. drive an LED) */
        { "Up to 8 mA per pin (25 mA = absolute max). C13, C14, C15: 3 mA, only to GND.",
          "C13 drives the board's LED: LOW = on.",
          "5V pins go straight to the USB 5 V (no protection).",
          "SWD header (far end): 3.3, DIO (A13), CLK (A14), GND.",
          "BOOT0 jumper: 0 = run your program, 1 = serial loader on A9/A10." },
    },
    [EC_BOARD_UNO] = {
        "Arduino Uno R3", "ATmega328P", "5 V logic, 16 MHz", "USB / jack", "",
        uno_left, COUNT(uno_left), uno_right, COUNT(uno_right),
        "~ = PWM (analogWrite): D3, D5, D6, D9, D10, D11.",
        { "5 V logic. 20 mA per pin is safe; 40 mA damages the chip.",
          "D0/D1 are the USB serial: keep them free while uploading.",
          "SDA/SCL next to AREF are the same wires as A4/A5.",
          "VIN / power jack: 7-12 V in (5 V there is too little).",
          NULL },
    },
    [EC_BOARD_NANO] = {
        "Arduino Nano", "ATmega328P", "5 V logic, 16 MHz", "USB", "ICSP",
        nano_left, COUNT(nano_left), nano_right, COUNT(nano_right),
        "~ = PWM (analogWrite): D3, D5, D6, D9, D10, D11.",
        { "5 V logic. 20 mA per pin is safe; 40 mA damages the chip.",
          "A6 and A7 are analog inputs only (no digitalRead/Write).",
          "D0/D1 are the USB serial: keep them free while uploading.",
          "3V3 out is weak (50 mA or less): only for small sensors.",
          NULL },
    },
    [EC_BOARD_ESP32] = {
        "ESP32 DevKit", "ESP32-WROOM-32", "3.3 V logic, 240 MHz", "antenna", "USB",
        esp_left, COUNT(esp_left), esp_right, COUNT(esp_right),
        "30-pin DOIT DevKit V1. 38-pin boards differ: check the labels.",
        { "3.3 V logic and NOT 5 V tolerant: 5 V on a pin kills it.",
          "34, 35, VP, VN: input only, no internal pull-up resistors.",
          "ADC2 pins can't read analog while Wi-Fi is on: use 32-39.",
          "Boot pins 0, 2, 5, 12, 15: D12 must be LOW at start-up.",
          NULL },
    },
};

const EcBoard *ec_board(int b) { return (b >= 0 && b < EC_BOARD_COUNT) ? &boards[b] : NULL; }

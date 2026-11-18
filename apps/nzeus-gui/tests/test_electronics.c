/* test_electronics.c - checks the Electronics app on the PC.
 *
 * Part 1: every elec_calc.c function against answers worked out by hand.
 * Part 2: the real app (LVGL + NZEUS screens), driven with simulated buttons:
 *         checks that the answers update live, and saves a picture of every
 *         tool in several themes, to look at.
 * Usage:  test_electronics OUTPUT_FOLDER        (checks + pictures)
 *         test_electronics --checks-only        (part 1 only, for the mutants)
 * Every check prints PASS or FAIL; the exit code is the number of FAILs. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "elec_calc.h"

static int fails, passes;

static void check(int ok, const char *what, ...)
{
    va_list ap;
    va_start(ap, what);
    printf("%s  ", ok ? "PASS" : "FAIL");
    vprintf(what, ap);
    printf("\n");
    va_end(ap);
    if (ok) passes++; else fails++;
}

/* equal within 0.1 % (or exactly, for 0) */
static int near(double a, double b) { return b == 0 ? fabs(a) < 1e-12 : fabs(a - b) <= fabs(b) * 1e-3; }

/* ---------- part 1: the maths ---------- */
static void check_bands(int b0, int b1, int b2, int b3, int b4, int n, double ohms, double tol)
{
    int b[5] = { b0, b1, b2, b3, b4 };
    double t = -1, v = ec_bands_to_ohms(b, n, &t);
    char names[96];
    ec_bands_text(b, n, names, sizeof names);
    check(near(v, ohms) && t == tol, "%s = %g ohm %g %%   (got %g ohm %g %%)", names, ohms, tol, v, t);
}

static void check_to_bands(double ohms, int n, int b0, int b1, int b2, int b3)
{
    int b[5] = { -1, -1, -1, -1, -1 };
    int r = ec_ohms_to_bands(ohms, n, b);
    char got[96];
    b[n - 1] = EC_NONE;
    ec_bands_text(b, n, got, sizeof got);
    int ok = r == 0 && b[0] == b0 && b[1] == b1 && b[2] == b2 && (n == 4 || b[3] == b3);
    check(ok, "%g ohm as %d bands -> %s", ohms, n, got);
}

static void check_format(double v, const char *unit, const char *want)
{
    char buf[48];
    ec_format(v, unit, buf, sizeof buf);
    check(!strcmp(buf, want), "format %g %s -> \"%s\"   (got \"%s\")", v, unit, want, buf);
}

static void check_parse(const char *text, int ok, double want)
{
    double v = 0;
    int r = ec_parse(text, &v);
    if (ok) check(r == 0 && near(v, want), "typed \"%s\" -> %g   (got %g)", text, want, v);
    else check(r != 0, "typed \"%s\" -> not a number", text);
}

static void check_convert(int q, int from, int to, double v, double want)
{
    double out = 0;
    int r = ec_convert(q, from, to, v, &out);
    check(r == 0 && near(out, want), "%g %s = %g %s   (got %g)", v, ec_unit_name(q, from), want,
          ec_unit_name(q, to), out);
}

/* is pin `name` on a board flagged with `flag`? -1 = no such pin */
static int pin_flag(const EcBoard *b, const char *name, int flag)
{
    for (int s = 0; s < 2; s++) {
        const EcPin *p = s ? b->right : b->left;
        int n = s ? b->n_right : b->n_left;
        for (int i = 0; i < n; i++) if (!strcmp(p[i].name, name)) return (p[i].flags & flag) != 0;
    }
    return -1;
}

/* does one side of a board list exactly these pin names, top to bottom? */
static void check_side(const EcBoard *b, int right, const char *const *want, int n, const char *what)
{
    const EcPin *p = right ? b->right : b->left;
    int have = right ? b->n_right : b->n_left, bad = have != n;
    for (int i = 0; i < n && i < have; i++)
        if (strcmp(p[i].name, want[i])) { bad++; printf("      pin %d: %s, should be %s\n", i + 1, p[i].name, want[i]); }
    check(!bad, "%s: %s side, top to bottom, as on the real board (%d pins, got %d)", b->title, what, n, have);
}

/* how many pins on a board have this flag */
static int count_flag(const EcBoard *b, int flag)
{
    int c = 0;
    for (int i = 0; i < b->n_left; i++) c += (b->left[i].flags & flag) != 0;
    for (int i = 0; i < b->n_right; i++) c += (b->right[i].flags & flag) != 0;
    return c;
}

/* the note of the first pin called `name` ("" if there is none) */
static const char *pin_note(const EcBoard *b, const char *name)
{
    for (int s = 0; s < 2; s++) {
        const EcPin *p = s ? b->right : b->left;
        int n = s ? b->n_right : b->n_left;
        for (int i = 0; i < n; i++) if (!strcmp(p[i].name, name)) return p[i].note;
    }
    return "";
}

static void math_checks(void)
{
    printf("--- resistor colour code\n");
    check_bands(EC_BROWN, EC_BLACK, EC_RED, EC_GOLD, 0, 4, 1000, 5);            /* brown-black-red-gold */
    check_bands(EC_YELLOW, EC_VIOLET, EC_ORANGE, EC_GOLD, 0, 4, 47000, 5);
    check_bands(EC_RED, EC_RED, EC_BROWN, EC_SILVER, 0, 4, 220, 10);
    check_bands(EC_YELLOW, EC_VIOLET, EC_GOLD, EC_GOLD, 0, 4, 4.7, 5);
    check_bands(EC_GREEN, EC_BLUE, EC_SILVER, EC_RED, 0, 4, 0.56, 2);
    check_bands(EC_BROWN, EC_BLACK, EC_BLUE, EC_NONE, 0, 4, 10e6, 20);
    check_bands(EC_BROWN, EC_BLACK, EC_BLACK, EC_BROWN, EC_BROWN, 5, 1000, 1);    /* 5 bands */
    check_bands(EC_ORANGE, EC_ORANGE, EC_BLACK, EC_BLACK, EC_GREEN, 5, 330, 0.5);
    check_bands(EC_RED, EC_VIOLET, EC_BLACK, EC_ORANGE, EC_BLUE, 5, 270000, 0.25);
    /* IEC 60062:2016 precision tolerances: orange 0.05 %, yellow 0.02 %, grey 0.01 % */
    check_bands(EC_BROWN, EC_BLACK, EC_BLACK, EC_RED, EC_ORANGE, 5, 10000, 0.05);
    check_bands(EC_BROWN, EC_BLACK, EC_BLACK, EC_RED, EC_YELLOW, 5, 10000, 0.02);
    check_bands(EC_BROWN, EC_BLACK, EC_BLACK, EC_RED, EC_GREY, 5, 10000, 0.01);
    {
        int bad1[4] = { EC_GOLD, EC_BLACK, EC_RED, EC_GOLD }, bad2[4] = { EC_BROWN, EC_BLACK, EC_RED, EC_BLACK },
            bad3[4] = { EC_BROWN, EC_BLACK, EC_RED, EC_WHITE };
        check(ec_bands_to_ohms(bad1, 4, NULL) < 0, "gold can't be a digit band");
        check(ec_bands_to_ohms(bad2, 4, NULL) < 0, "black is not a tolerance colour");
        check(ec_bands_to_ohms(bad3, 4, NULL) < 0, "white is not a tolerance colour");
    }
    {
        int b[4] = { EC_BROWN, EC_BLACK, EC_RED, EC_GOLD };
        char t[96];
        ec_bands_text(b, 4, t, sizeof t);
        check(!strcmp(t, "brown-black-red-gold"), "colour names: %s", t);
    }

    printf("--- value -> colour bands\n");
    check_to_bands(4700, 4, EC_YELLOW, EC_VIOLET, EC_RED, 0);
    check_to_bands(150, 4, EC_BROWN, EC_GREEN, EC_BROWN, 0);
    check_to_bands(1, 4, EC_BROWN, EC_BLACK, EC_GOLD, 0);
    check_to_bands(0.47, 4, EC_YELLOW, EC_VIOLET, EC_SILVER, 0);
    check_to_bands(10e6, 4, EC_BROWN, EC_BLACK, EC_BLUE, 0);
    check_to_bands(99.6, 4, EC_BROWN, EC_BLACK, EC_BROWN, 0);          /* rounds to 100 */
    check_to_bands(330, 5, EC_ORANGE, EC_ORANGE, EC_BLACK, EC_BLACK);
    check_to_bands(4700, 5, EC_YELLOW, EC_VIOLET, EC_BLACK, EC_BROWN);
    {
        int b[5];
        check(ec_ohms_to_bands(0.01, 4, b) != 0, "0.01 ohm can't be shown with 4 bands");
        /* every E24 value from 1 ohm to 9.1 MOhm: bands -> value -> same value */
        int bad = 0, count = 0;
        for (double v = 1; v < 1e7; v = ec_step_e(v, 24, +1), count++) {
            if (ec_ohms_to_bands(v, 4, b) != 0) { bad++; continue; }
            b[3] = EC_GOLD;
            if (!near(ec_bands_to_ohms(b, 4, NULL), v)) bad++;
        }
        check(bad == 0 && count == 7 * 24, "all %d E24 values 1 ohm .. 9.1 MOhm survive value -> bands -> value", count);
    }

    printf("--- standard values\n");
    check(near(ec_e_nearest(5000, 12), 4700), "nearest E12 to 5 kOhm = 4.7 kOhm (got %g)", ec_e_nearest(5000, 12));
    check(near(ec_e_nearest(5000, 24), 5100), "nearest E24 to 5 kOhm = 5.1 kOhm (got %g)", ec_e_nearest(5000, 24));
    check(near(ec_e_nearest(250, 12), 270), "nearest E12 to 250 ohm = 270 ohm (got %g)", ec_e_nearest(250, 12));
    check(near(ec_e_nearest(0.3, 12), 0.33), "nearest E12 to 0.3 ohm = 0.33 ohm (got %g)", ec_e_nearest(0.3, 12));
    check(near(ec_e_up(150, 12), 150), "E12 at or above 150 = 150 (got %g)", ec_e_up(150, 12));
    check(near(ec_e_up(151, 12), 180), "E12 at or above 151 = 180 (got %g)", ec_e_up(151, 12));
    check(near(ec_e_up(440, 12), 470), "E12 at or above 440 = 470 (got %g)", ec_e_up(440, 12));
    check(near(ec_e_up(9000, 12), 10000), "E12 at or above 9 k = 10 k (got %g)", ec_e_up(9000, 12));
    check(near(ec_step_e(4700, 12, +1), 5600), "E12 after 4.7 k = 5.6 k (got %g)", ec_step_e(4700, 12, +1));
    check(near(ec_step_e(4700, 12, -1), 3900), "E12 before 4.7 k = 3.9 k (got %g)", ec_step_e(4700, 12, -1));
    check(near(ec_step_e(1000, 12, -1), 820), "E12 before 1 k = 820 (got %g)", ec_step_e(1000, 12, -1));
    check(near(ec_step_e(8200, 12, +1), 10000), "E12 after 8.2 k = 10 k (got %g)", ec_step_e(8200, 12, +1));
    check(near(ec_step_e(5000, 24, +1), 5100), "E24 after 5 k = 5.1 k (got %g)", ec_step_e(5000, 24, +1));
    check(near(ec_step_e(10e-6, 12, -1), 8.2e-6), "E12 before 10 uF = 8.2 uF (got %g)", ec_step_e(10e-6, 12, -1));
    check(near(ec_step_nice(3.3, +1), 3.6), "round value after 3.3 = 3.6 (got %g)", ec_step_nice(3.3, +1));
    check(near(ec_step_nice(4.7, +1), 5), "round value after 4.7 = 5 (got %g)", ec_step_nice(4.7, +1));
    check(near(ec_step_nice(9, +1), 10), "round value after 9 = 10 (got %g)", ec_step_nice(9, +1));
    check(near(ec_step_nice(0.02, -1), 0.018), "round value before 20 m = 18 m (got %g)", ec_step_nice(0.02, -1));

    printf("--- Ohm's law\n");
    EcOhm o;
    int r;
    r = ec_ohms_law(EC_V, 5, EC_I, 0.02, &o);
    check(r == 0 && near(o.r, 250) && near(o.p, 0.1),
          "5 V, 20 mA -> 250 ohm, 0.1 W (got %g, %g)", o.r, o.p);
    r = ec_ohms_law(EC_V, 12, EC_R, 1000, &o);
    check(r == 0 && near(o.i, 0.012) && near(o.p, 0.144),
          "12 V, 1 kOhm -> 12 mA, 0.144 W (got %g, %g)", o.i, o.p);
    r = ec_ohms_law(EC_I, 0.5, EC_R, 10, &o);
    check(r == 0 && near(o.v, 5) && near(o.p, 2.5),
          "0.5 A, 10 ohm -> 5 V, 2.5 W (got %g, %g)", o.v, o.p);
    r = ec_ohms_law(EC_P, 1, EC_V, 5, &o);
    check(r == 0 && near(o.i, 0.2) && near(o.r, 25),
          "1 W at 5 V -> 0.2 A, 25 ohm (got %g, %g)", o.i, o.r);
    r = ec_ohms_law(EC_P, 2.5, EC_I, 0.5, &o);
    check(r == 0 && near(o.v, 5) && near(o.r, 10),
          "2.5 W at 0.5 A -> 5 V, 10 ohm (got %g, %g)", o.v, o.r);
    r = ec_ohms_law(EC_P, 0.25, EC_R, 100, &o);
    check(r == 0 && near(o.i, 0.05) && near(o.v, 5),
          "0.25 W in 100 ohm -> 50 mA, 5 V (got %g, %g)", o.i, o.v);
    r = ec_ohms_law(EC_R, 100, EC_V, 5, &o);
    check(r == 0 && near(o.i, 0.05), "R then V (any order) -> 50 mA (got %g)", o.i);
    check(ec_ohms_law(EC_V, 5, EC_V, 3, &o) != 0, "V and V: not enough to know -> error");
    check(ec_ohms_law(EC_V, 5, EC_R, 0, &o) != 0, "0 ohm -> error");

    printf("--- LED resistor\n");
    EcLed L;
    r = ec_led_resistor(5, 2.0, 0.020, 12, &L);
    check(r == 0 && near(L.r, 150) && near(L.r_std, 150),
          "LED 5V red 2.0V 20mA -> 150 ohm (got %g, standard %g)", L.r, L.r_std);
    check(near(L.p_r, 0.06) && L.rating == 0.25, "  ... resistor heat 60 mW, a 1/4 W resistor (got %g W, %g W)", L.p_r, L.rating);
    r = ec_led_resistor(12, 3.2, 0.020, 12, &L);
    check(r == 0 && near(L.r, 440) && near(L.r_std, 470) && near(L.i_std, 8.8 / 470),
          "LED 12V blue 3.2V 20mA -> 440 ohm, next bigger standard 470 ohm, 18.7 mA (got %g, %g, %g)", L.r, L.r_std, L.i_std);
    r = ec_led_resistor(3.3, 3.2, 0.020, 12, &L);
    check(r == 0 && near(L.r, 5) && near(L.r_std, 5.6),
          "LED 3.3V white 3.2V 20mA -> 5 ohm, standard 5.6 ohm (got %g, %g)", L.r, L.r_std);
    r = ec_led_resistor(12, 2, 0.15, 12, &L);
    check(r == 0 && near(L.r_std, 68) && L.rating == 5, "12 V 150 mA LED -> 68 ohm, 1.5 W of heat: a 5 W resistor (got %g, %g)", L.r_std, L.rating);
    r = ec_led_resistor(24, 2, 0.2, 12, &L);
    check(r == 0 && L.rating < 0, "24 V 200 mA LED -> over 5 W of heat: \"too much\" (got %g)", L.rating);
    check(ec_led_resistor(2, 2.0, 0.020, 12, &L) != 0, "supply not above the LED voltage -> error");

    printf("--- voltage divider\n");
    check(near(ec_divider_vout(5, 10000, 20000), 3.3333), "5 V, R1 10k, R2 20k -> 3.33 V (got %g)", ec_divider_vout(5, 10000, 20000));
    check(near(ec_divider_vout(12, 10000, 10000), 6), "12 V, two equal resistors -> 6 V (got %g)", ec_divider_vout(12, 10000, 10000));
    check(near(ec_divider_r2(5, 3.3, 10000), 19411.76), "5 V -> 3.3 V with R1 10k needs R2 = 19.4 kOhm (got %g)", ec_divider_r2(5, 3.3, 10000));
    check(ec_divider_r2(5, 6, 10000) < 0, "a divider can't make 6 V from 5 V -> error");

    printf("--- 555 timer\n");
    Ec555 t;
    r = ec_555_astable(1000, 10000, 10e-6, &t);
    check(r == 0 && fabs(t.f - 6.87) < 0.01,
          "555 R1=1k R2=10k C=10uF -> about 6.9 Hz (got %.3f Hz)", t.f);
    check(near(t.t_high, 0.07623) && near(t.t_low, 0.0693) && fabs(t.duty - 52.4) < 0.1,
          "  ... HIGH 76.2 ms, LOW 69.3 ms, duty 52.4 %% (got %g, %g, %.1f)", t.t_high, t.t_low, t.duty);
    r = ec_555_astable(1000, 1000, 100e-9, &t);
    check(r == 0 && fabs(t.f - 4810) < 5 && fabs(t.duty - 66.7) < 0.1,
          "555 R1=1k R2=1k C=100nF -> about 4.81 kHz, duty 66.7 %% (got %.0f Hz, %.1f)", t.f, t.duty);
    check(near(ec_555_mono(100000, 10e-6), 1.1), "555 one pulse R=100k C=10uF -> 1.1 s (got %g)", ec_555_mono(100000, 10e-6));
    check(ec_555_astable(0, 1000, 1e-6, &t) != 0, "555 with R1 = 0 -> error");

    printf("--- unit conversion\n");
    check_convert(EC_Q_CURRENT, 1, 2, 20, 0.02);        /* mA -> A */
    check_convert(EC_Q_CURRENT, 2, 0, 0.5, 500000);     /* A -> uA */
    check_convert(EC_Q_VOLTAGE, 0, 1, 3300, 3.3);       /* mV -> V */
    check_convert(EC_Q_RESISTANCE, 1, 0, 4.7, 4700);    /* kOhm -> Ohm */
    check_convert(EC_Q_RESISTANCE, 0, 2, 2200000, 2.2); /* Ohm -> MOhm */
    check_convert(EC_Q_CAPACITANCE, 1, 2, 100, 0.1);    /* nF -> uF */
    check_convert(EC_Q_CAPACITANCE, 2, 0, 1, 1e6);      /* uF -> pF */
    check_convert(EC_Q_FREQUENCY, 2, 1, 16, 16000);     /* MHz -> kHz */
    check_convert(EC_Q_LENGTH, 3, 0, 1, 25.4);          /* inch -> mm */
    check_convert(EC_Q_LENGTH, 4, 0, 1, 0.0254);        /* mil -> mm */
    check_convert(EC_Q_LENGTH, 0, 3, 2.54, 0.1);        /* mm -> inch */
    check_convert(EC_Q_TEMPERATURE, 0, 1, 25, 77);      /* C -> F */
    check_convert(EC_Q_TEMPERATURE, 1, 0, 212, 100);    /* F -> C */
    check_convert(EC_Q_TEMPERATURE, 0, 2, 0, 273.15);   /* C -> K */
    check_convert(EC_Q_TEMPERATURE, 1, 0, -40, -40);    /* -40 is the same in C and F */
    {
        double out;
        check(ec_convert(EC_Q_TEMPERATURE, 0, 1, -300, &out) != 0, "-300 C is colder than absolute zero -> error");
        check(ec_convert(EC_Q_CURRENT, 0, 7, 1, &out) != 0, "a unit that doesn't exist -> error");
    }
    check(fabs(ec_awg_mm(22) - 0.644) < 0.001, "AWG 22 = 0.644 mm (got %.4f)", ec_awg_mm(22));
    check(fabs(ec_awg_mm(24) - 0.511) < 0.001, "AWG 24 = 0.511 mm (got %.4f)", ec_awg_mm(24));
    check(fabs(ec_awg_mm(10) - 2.588) < 0.001, "AWG 10 = 2.588 mm (got %.4f)", ec_awg_mm(10));
    check(fabs(ec_awg_mm(36) - 0.127) < 0.0001, "AWG 36 = 0.127 mm (got %.4f)", ec_awg_mm(36));
    check(fabs(ec_awg_mm2(22) - 0.326) < 0.001, "AWG 22 = 0.326 mm2 (got %.4f)", ec_awg_mm2(22));
    check(ec_awg_mm(41) < 0, "AWG 41 -> error");

    printf("--- numbers as text\n");
    check_format(4700, "Ohm", "4.7 kOhm");
    check_format(150, "Ohm", "150 Ohm");
    check_format(2.2e6, "Ohm", "2.2 MOhm");
    check_format(0.02, "A", "20 mA");
    check_format(6.8713, "Hz", "6.87 Hz");
    check_format(10e-6, "F", "10 uF");
    check_format(100e-9, "F", "100 nF");
    check_format(0.07623, "s", "76.2 ms");
    check_format(999.7, "V", "1 kV");
    check_format(0, "V", "0 V");
    check_format(3.3, "V", "3.3 V");
    check_format(-0.5, "V", "-500 mV");
    check_format(4700, "", "4.7 k");
    check_parse("4k7", 1, 4700);
    check_parse("4.7k", 1, 4700);
    check_parse("4700", 1, 4700);
    check_parse("10u", 1, 10e-6);
    check_parse("100 nF", 1, 100e-9);
    check_parse("20mA", 1, 0.02);
    check_parse("2.2M", 1, 2.2e6);
    check_parse("1M5", 1, 1.5e6);
    check_parse("4R7", 1, 4.7);
    check_parse("3.3", 1, 3.3);
    check_parse("5 V", 1, 5);
    check_parse("-40", 1, -40);
    check_parse("1e3", 1, 1000);
    check_parse("4.7 kOhm", 1, 4700);
    check_parse("abc", 0, 0);
    check_parse("", 0, 0);

    printf("--- pinouts\n");
    const EcBoard *bp = ec_board(EC_BOARD_BLUEPILL), *uno = ec_board(EC_BOARD_UNO),
                  *nano = ec_board(EC_BOARD_NANO), *esp = ec_board(EC_BOARD_ESP32);
    check(bp->n_left == 20 && bp->n_right == 20, "Blue Pill: 2 x 20 pins (got %d + %d)", bp->n_left, bp->n_right);
    {   /* STM32F103C8 datasheet: every I/O is 5 V tolerant except PA0-PA7, PB0, PB1, PB5, PC13-PC15 */
        static const char *const not_ft[] = { "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "B0", "B1", "B5", "C13", "C14", "C15" };
        static const char *const ft[] = { "B10", "B11", "B12", "B13", "B14", "B15", "A8", "A9", "A10", "A11", "A12",
                                          "A15", "B3", "B4", "B6", "B7", "B8", "B9" };
        int bad = 0;
        for (size_t i = 0; i < sizeof not_ft / sizeof *not_ft; i++) if (pin_flag(bp, not_ft[i], EC_PIN_5VT) != 0) { bad++; printf("      wrong: %s\n", not_ft[i]); }
        for (size_t i = 0; i < sizeof ft / sizeof *ft; i++) if (pin_flag(bp, ft[i], EC_PIN_5VT) != 1) { bad++; printf("      wrong: %s\n", ft[i]); }
        check(bad == 0, "Blue Pill: 18 pins marked 5 V tolerant, PA0-7 / PB0 / PB1 / PB5 / PC13-15 not");
        check(count_flag(bp, EC_PIN_5VT) == 18, "Blue Pill: exactly 18 pins marked * (got %d)", count_flag(bp, EC_PIN_5VT));
    }
    {   /* a photo of the board (stm32-base.org), USB end up, seen from the chip side */
        static const char *const l[] = { "B12", "B13", "B14", "B15", "A8", "A9", "A10", "A11", "A12", "A15",
                                         "B3", "B4", "B5", "B6", "B7", "B8", "B9", "5V", "G", "3.3" };
        static const char *const r[] = { "G", "G", "3.3", "R", "B11", "B10", "B1", "B0", "A7", "A6",
                                         "A5", "A4", "A3", "A2", "A1", "A0", "C15", "C14", "C13", "VB" };
        check_side(bp, 0, l, 20, "left");
        check_side(bp, 1, r, 20, "right");
        check(!strcmp(bp->top, "USB") && !strcmp(bp->bottom, "SWD"), "Blue Pill: USB at the top, SWD header at the bottom");
    }
    {
        static const char *const pwm[] = { "D3", "D5", "D6", "D9", "D10", "D11" };
        static const char *const no_pwm[] = { "D0", "D1", "D2", "D4", "D7", "D8", "D12", "D13" };
        int bad = 0;
        for (int b = 0; b < 2; b++) {
            const EcBoard *bd = b ? nano : uno;
            for (int i = 0; i < 6; i++) if (pin_flag(bd, pwm[i], EC_PIN_PWM) != 1) bad++;
            for (int i = 0; i < 8; i++) if (pin_flag(bd, no_pwm[i], EC_PIN_PWM) != 0) bad++;
        }
        check(bad == 0, "Uno and Nano: PWM (~) on D3, D5, D6, D9, D10, D11 only");
    }
    check(count_flag(uno, EC_PIN_PWM) == 6 && count_flag(nano, EC_PIN_PWM) == 6, "Uno and Nano: exactly 6 pins marked ~");
    check(uno->n_left + uno->n_right == 32, "Uno: 8 power + 6 analog + 18 digital header pins (got %d)", uno->n_left + uno->n_right);
    {   /* Arduino's own pinout (A000066-full-pinout.pdf, TOP VIEW, USB and jack up) */
        static const char *const l[] = { "NC", "IOREF", "RESET", "3.3V", "5V", "GND", "GND", "VIN",
                                         "A0", "A1", "A2", "A3", "A4", "A5" };
        static const char *const r[] = { "SCL", "SDA", "AREF", "GND", "D13", "D12", "D11", "D10", "D9", "D8",
                                         "D7", "D6", "D5", "D4", "D3", "D2", "D1", "D0" };
        check_side(uno, 0, l, 14, "left");
        check_side(uno, 1, r, 18, "right");
        check((uno->left[8].flags & EC_PIN_GAP) && (uno->right[10].flags & EC_PIN_GAP),
              "Uno: header gaps before A0 and before D7");
    }
    check(nano->n_left == 15 && nano->n_right == 15, "Nano: 2 x 15 pins (got %d + %d)", nano->n_left, nano->n_right);
    {   /* Arduino's own pinout (A000005-full-pinout.pdf, USB up) */
        static const char *const l[] = { "D13", "3V3", "AREF", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
                                         "5V", "RST", "GND", "VIN" };
        static const char *const r[] = { "D12", "D11", "D10", "D9", "D8", "D7", "D6", "D5", "D4", "D3", "D2",
                                         "GND", "RST", "D0", "D1" };
        check_side(nano, 0, l, 15, "left");
        check_side(nano, 1, r, 15, "right");
    }
    check(pin_flag(nano, "A6", 0) == 0 && pin_flag(nano, "A7", 0) == 0, "Nano: has A6 and A7");
    check(esp->n_left == 15 && esp->n_right == 15, "ESP32 DevKit: 2 x 15 pins (got %d + %d)", esp->n_left, esp->n_right);
    {   /* the 30-pin DOIT DevKit V1 (Random Nerd Tutorials pinout), antenna up */
        static const char *const l[] = { "EN", "VP", "VN", "D34", "D35", "D32", "D33", "D25", "D26", "D27",
                                         "D14", "D12", "D13", "GND", "VIN" };
        static const char *const r[] = { "D23", "D22", "TX0", "RX0", "D21", "D19", "D18", "D5", "TX2", "RX2",
                                         "D4", "D2", "D15", "GND", "3V3" };
        check_side(esp, 0, l, 15, "left");
        check_side(esp, 1, r, 15, "right");
    }
    check(count_flag(esp, EC_PIN_5VT) == 0 && count_flag(esp, EC_PIN_PWM) == 0, "ESP32: no pin marked 5 V tolerant (or ~)");
    {   /* Espressif: GPIO34-39 are input only (VP = GPIO36, VN = GPIO39); the strapping pins */
        int ok = strstr(pin_note(esp, "VP"), "IO36") && strstr(pin_note(esp, "VN"), "IO39");
        static const char *const in_only[] = { "VP", "VN", "D34", "D35" };
        for (int i = 0; i < 4; i++) ok = ok && strstr(pin_note(esp, in_only[i]), "only") != NULL;
        check(ok, "ESP32: VP (IO36), VN (IO39), D34, D35 say input only");
        static const char *const outs[] = { "D32", "D33", "D25", "D26", "D27", "D14", "D12", "D13", "D23", "D22",
                                            "D21", "D19", "D18", "D5", "D4", "D2", "D15" };
        int bad = 0;
        for (size_t i = 0; i < sizeof outs / sizeof *outs; i++) if (strstr(pin_note(esp, outs[i]), "only")) bad++;
        check(bad == 0, "ESP32: no other pin is called input only");
        static const char *const boot[] = { "D2", "D5", "D12", "D15" };   /* GPIO0 is the BOOT button, not a pin here */
        bad = 0;
        for (int i = 0; i < 4; i++) if (!strstr(pin_note(esp, boot[i]), "boot")) bad++;
        check(bad == 0, "ESP32: strapping pins D2, D5, D12, D15 say boot");
    }
    check(strstr(esp->left[3].note, "input") && strstr(esp->left[4].note, "input") &&
          !strcmp(esp->left[3].name, "D34") && !strcmp(esp->left[4].name, "D35"), "ESP32: D34 and D35 are input only");
    check(!strcmp(esp->right[2].name, "TX0") && !strcmp(esp->right[3].name, "RX0"), "ESP32: TX0 / RX0 third and fourth on the right");
    {   /* every board: has ground and power, and notes short enough for the narrow window */
        int bad = 0;
        for (int b = 0; b < EC_BOARD_COUNT; b++) {
            const EcBoard *bd = ec_board(b);
            int gnd = 0, pwr = 0;
            for (int s = 0; s < 2; s++) {
                const EcPin *p = s ? bd->right : bd->left;
                int n = s ? bd->n_right : bd->n_left;
                for (int i = 0; i < n; i++) {
                    gnd += (p[i].flags & EC_PIN_GND) != 0;
                    pwr += (p[i].flags & EC_PIN_POWER) != 0;
                    if (strlen(p[i].note) > 13) { bad++; printf("      long note: %s %s\n", bd->title, p[i].note); }
                }
            }
            if (!gnd || !pwr) bad++;
        }
        check(bad == 0, "every board has GND and power pins, and notes of 13 letters or less");
    }
}

/* ---------- part 2: the app on screen ---------- */
#include "lvgl.h"
#include "apps.h"
#include "display.h"
#include "home.h"
#include "nzeus_tick.h"
#include "nzui.h"
#include "theme.h"
#include "ui.h"

extern NzApp app_electronics;
int app_electronics_tool(void);
const char *app_electronics_answer(void);
const char *app_electronics_sub(void);
const char *app_electronics_explain(void);
int app_electronics_band(int i);
int app_electronics_rows(void);
void app_electronics_test_open(int t);

static const char *dir = ".";
static int theme_now;

/* save the screen as a PPM picture (the build script makes PNGs of them) */
static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/elec_t%d_%s.ppm", dir, theme_now, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6 %d %d 255\n", NZ_W, NZ_H);
    for (int i = 0; i < NZ_W * NZ_H; i++) {
        uint16_t p = nzeus_fb[i];                       /* RGB565 -> 3 bytes */
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run(int ms)
{
    uint32_t end = nzeus_tick_ms() + (uint32_t)ms;
    while ((int32_t)(end - nzeus_tick_ms()) > 0) { ui_tick(); lv_timer_handler(); usleep(3000); }
    lv_refr_now(NULL);
}

static void key(int k) { ui_key(k); run(30); }
static void keys(int k, int times) { for (int i = 0; i < times; i++) ui_key(k); run(30); }
static void type(const char *s)
{
    for (; *s; s++) { KbEvent ev = { KB_CHAR, *s }; ui_kbd(&ev); }
    run(30);
}
static void kb(KbKey k) { KbEvent ev = { k, 0 }; ui_kbd(&ev); run(30); }

static void answer_is(const char *want, const char *what)
{
    check(!strcmp(app_electronics_answer(), want), "screen: %s shows \"%s\"   (got \"%s\")", what, want, app_electronics_answer());
}

/* open tool number t from the list (the list is at the top: A opens row 0) */
static void open_tool(int t)
{
    keys(NZ_KEY_UP, 12);
    keys(NZ_KEY_DOWN, t);
    key(NZ_KEY_A);
}

static void ui_walk(int checks)
{
    ui_set_mode(UI_HANDHELD);
    run(1900);                                        /* let the "Handheld mode" message go */
    apps_open(APP_ELECTRONICS);
    run(200);
    if (checks) check(app_electronics_tool() == -1, "screen: the app opens on the tool list");
    save("list");

    open_tool(0);                                     /* resistor colour code */
    if (checks) {
        check(app_electronics_tool() == 0, "screen: A opens the first tool");
        answer_is("1 kOhm", "brown-black-red-gold");
        check(app_electronics_band(0) == EC_BROWN && app_electronics_band(1) == EC_BLACK &&
              app_electronics_band(2) == EC_RED && app_electronics_band(3) == EC_GOLD,
              "screen: the drawing has brown, black, red, gold bands");
    }
    keys(NZ_KEY_DOWN, 1); keys(NZ_KEY_RIGHT, 3);      /* 1st band: brown -> red -> orange -> yellow */
    keys(NZ_KEY_DOWN, 1); keys(NZ_KEY_RIGHT, 7);      /* 2nd band: black -> violet */
    if (checks) {
        answer_is("4.7 kOhm", "yellow-violet-red-gold");
        check(app_electronics_band(0) == EC_YELLOW && app_electronics_band(1) == EC_VIOLET,
              "screen: the drawing changed live to yellow, violet");
        check(app_electronics_rows() == 5, "screen: 4-band form has 5 rows (got %d)", app_electronics_rows());
    }
    save("colour");
    key(NZ_KEY_A);                                    /* A on a colour row: a list of colours */
    save("colour_menu");
    keys(NZ_KEY_UP, 6);                               /* the list opens on violet: up to brown */
    key(NZ_KEY_A);                                    /* pick "brown" */
    if (checks) answer_is("4.1 kOhm", "picking brown for the 2nd band from the list");
    keys(NZ_KEY_RIGHT, 6);                            /* back to violet */
    keys(NZ_KEY_UP, 5); key(NZ_KEY_RIGHT);            /* bands: 4 -> 5 */
    if (checks) {
        answer_is("47 kOhm", "5 bands yellow-violet-black-red-gold");
        check(app_electronics_rows() == 6 && app_electronics_band(4) == EC_GOLD,
              "screen: 5-band form has 6 rows and a 5th band");
    }
    keys(NZ_KEY_DOWN, 5); keys(NZ_KEY_LEFT, 4);        /* tolerance gold -> grey -> violet -> blue -> green */
    if (checks) check(app_electronics_band(4) == EC_GREEN, "screen: tolerance list has grey and violet between gold and green");
    save("colour5");
    key(NZ_KEY_B);
    if (checks) check(app_electronics_tool() == -1, "screen: B goes back to the tool list");

    open_tool(1);                                     /* finder */
    if (checks) {
        answer_is("4.7 kOhm", "the finder for 5 kOhm (E12)");
        check(!strcmp(app_electronics_sub(), "yellow-violet-red-gold"), "screen: finder colours %s", app_electronics_sub());
    }
    save("finder");
    key(NZ_KEY_A);                                    /* type a value: the number pad, in kOhm */
    for (int i = 0; i < 6; i++) kb(KB_BACKSPACE);
    type("33");
    save("finder_typing");
    kb(KB_ENTER);
    if (checks) answer_is("33 kOhm", "finder after typing 33 (in kOhm)");
    key(NZ_KEY_A);
    for (int i = 0; i < 6; i++) kb(KB_BACKSPACE);
    type("2k");                                       /* a size letter: 2 kOhm -> nearest 2.2 kOhm */
    kb(KB_ENTER);
    if (checks) answer_is("2.2 kOhm", "finder after typing 2k");
    key(NZ_KEY_B);

    open_tool(2);                                     /* Ohm's law */
    if (checks) {
        answer_is("R = 250 Ohm", "Ohm's law 5 V, 20 mA");
        check(!strcmp(app_electronics_sub(), "P = 100 mW"), "screen: Ohm's law power %s", app_electronics_sub());
    }
    save("ohm");
    keys(NZ_KEY_DOWN, 1); key(NZ_KEY_A);              /* type a new voltage */
    for (int i = 0; i < 6; i++) kb(KB_BACKSPACE);
    type("12");
    save("ohm_typing");
    kb(KB_ENTER);
    if (checks) answer_is("R = 600 Ohm", "Ohm's law after typing 12 (V)");
    keys(NZ_KEY_UP, 1); keys(NZ_KEY_RIGHT, 5);        /* I know: P and R */
    if (checks) answer_is("V = 15.8 V", "Ohm's law with P = 0.25 W, R = 1 kOhm");
    save("ohm_pr");
    key(NZ_KEY_B);

    open_tool(3);                                     /* LED */
    if (checks) {
        answer_is("150 Ohm", "LED 5 V, red 2.0 V, 20 mA");
        check(strstr(app_electronics_explain(), "nearest bigger standard resistor is 150 Ohm (brown-green-brown-gold)") != NULL,
              "screen: LED explanation: %s", app_electronics_explain());
        check(app_electronics_band(0) == EC_BROWN && app_electronics_band(1) == EC_GREEN && app_electronics_band(2) == EC_BROWN,
              "screen: LED tool draws brown-green-brown");
    }
    save("led");
    keys(NZ_KEY_DOWN, 3); key(NZ_KEY_A);              /* LED current: type 15 (mA) */
    for (int i = 0; i < 6; i++) kb(KB_BACKSPACE);
    type("15");
    kb(KB_ENTER);
    if (checks) answer_is("220 Ohm", "LED 5 V, red, 15 mA (200 -> 220)");
    key(NZ_KEY_A);
    for (int i = 0; i < 6; i++) kb(KB_BACKSPACE);
    type("20");
    kb(KB_ENTER);
    keys(NZ_KEY_UP, 2); keys(NZ_KEY_RIGHT, 5);        /* LED colour: blue 3.2 V */
    if (checks) answer_is("100 Ohm", "LED 5 V, blue 3.2 V, 20 mA (90 -> 100)");
    keys(NZ_KEY_UP, 1); keys(NZ_KEY_LEFT, 20);        /* supply 5 V -> 3 V: below the LED */
    if (checks) answer_is("--", "LED supply below the LED voltage");
    save("led_error");
    key(NZ_KEY_B);

    open_tool(4);                                     /* divider */
    if (checks) answer_is("Vout = 3.33 V", "divider 5 V, 10k, 20k");
    save("divider");
    key(NZ_KEY_RIGHT);
    if (checks) answer_is("R2 = 19.4 kOhm", "divider: R2 for 3.3 V");
    save("divider_r2");
    key(NZ_KEY_B);

    open_tool(5);                                     /* 555 */
    if (checks) answer_is("6.87 Hz", "555 R1=1k R2=10k C=10uF");
    save("555");
    key(NZ_KEY_RIGHT);
    if (checks) answer_is("Pulse 11 ms", "555 one pulse R1=1k C=10uF");
    save("555_mono");
    key(NZ_KEY_B);

    open_tool(6);                                     /* converter */
    if (checks) answer_is("4700 Ohm", "converter 4.7 kOhm -> Ohm");
    save("convert");
    keys(NZ_KEY_RIGHT, 1);                            /* Capacitance: 100 nF -> uF */
    if (checks) answer_is("0.1 uF", "converter 100 nF -> uF");
    keys(NZ_KEY_RIGHT, 4);                            /* Wire AWG */
    if (checks) answer_is("0.644 mm", "converter AWG 22");
    save("convert_awg");
    keys(NZ_KEY_LEFT, 1);                             /* Temperature: 25 C -> 77 F */
    if (checks) answer_is("77 F", "converter 25 C -> F");
    key(NZ_KEY_B);

    static const char *const pins[4] = { "bluepill", "uno", "nano", "esp32" };
    for (int i = 0; i < 4; i++) {
        open_tool(7 + i);
        if (checks) check(app_electronics_tool() == 7 + i, "screen: pinout %s opens", pins[i]);
        save(pins[i]);
        keys(NZ_KEY_DOWN, 20);                        /* scroll to the end: the notes */
        char n[32];
        snprintf(n, sizeof n, "%s_end", pins[i]);
        save(n);
        key(NZ_KEY_B);
    }

    key(NZ_KEY_B);                                    /* B on the list: the app closes */
    if (checks) check(apps_get(APP_ELECTRONICS)->root == NULL, "screen: B on the tool list closes the app");

    /* in a desktop window (smaller): the list and two tools */
    ui_set_mode(UI_DESKTOP);
    run(1900);
    apps_open(APP_ELECTRONICS);
    run(300);
    save("window_list");
    app_electronics_test_open(0);
    run(200);
    save("window_colour");
    app_electronics_test_open(-1);
    app_electronics_test_open(7);
    run(200);
    save("window_bluepill");
    app_electronics_test_open(-1);
    app_electronics_test_open(2);
    run(200);
    save("window_ohm");
    apps_close(apps_get(APP_ELECTRONICS));
    ui_set_mode(UI_HANDHELD);
    run(100);
}

int main(int argc, char **argv)
{
    int checks_only = argc > 1 && !strcmp(argv[1], "--checks-only");
    printf("=== part 1: the maths\n");
    math_checks();
    if (checks_only) {
        printf("=== %d passed, %d failed\n", passes, fails);
        return fails;
    }
    if (argc > 1) dir = argv[1];
    printf("=== part 2: the app on screen\n");
    lv_init();
    nzeus_display_init();
    ui_init();
    if (!apps_get(APP_ELECTRONICS)) apps_register(&app_electronics);   /* (until apps.c registers it) */
    ui_set_status("14:32", 78, 1);
    static const int themes[] = { TH_ORIGINAL, TH_GREEN_CRT, TH_DAYLIGHT };
    for (int i = 0; i < 3; i++) {
        theme_now = themes[i];
        theme_set(theme_now);
        run(100);
        ui_walk(i == 0);                              /* the checks once; pictures in every theme */
        printf("theme %s: pictures saved\n", theme()->name);
    }
    theme_set(TH_ORIGINAL);
    printf("=== %d passed, %d failed\n", passes, fails);
    return fails;
}

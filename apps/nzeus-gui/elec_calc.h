#ifndef NZEUS_ELEC_CALC_H
#define NZEUS_ELEC_CALC_H
#include <stddef.h>

/* elec_calc - the maths (and reference data) behind the Electronics app.
 *
 * Small "pure" functions: numbers in, numbers out, no screen, no files.
 * That makes them easy to check on the PC (tests/test_electronics.c).
 * Every value is in its base unit: volts, amps, ohms, farads, seconds, hertz.
 * Functions that can fail return 0 = ok, -1 = these numbers make no sense. */

/* ---------- resistor colours ---------- */
/* The 10 digit colours are in order, so EC_RED == 2 means "the digit 2". */
enum {
    EC_BLACK, EC_BROWN, EC_RED, EC_ORANGE, EC_YELLOW,
    EC_GREEN, EC_BLUE, EC_VIOLET, EC_GREY, EC_WHITE,
    EC_GOLD, EC_SILVER, EC_NONE,            /* EC_NONE: no tolerance band at all */
    EC_COLOUR_COUNT
};
const char *ec_colour_name(int c);              /* "brown" */
double ec_multiplier(int c);                    /* red -> 100, gold -> 0.1; -1 = not a multiplier colour */
double ec_tolerance(int c);                     /* gold -> 5 (%); -1 = not a tolerance colour */

/* bands[] = 4 bands (digit, digit, multiplier, tolerance)
 *        or 5 bands (digit, digit, digit, multiplier, tolerance).
 * Returns the resistance in ohms (and the tolerance in % if tol != NULL),
 * or -1 if a band has a colour that can't be there. */
double ec_bands_to_ohms(const int *bands, int n, double *tol);

/* The other way: the digit + multiplier bands for `ohms` (rounded to 2 or 3
 * digits). Fills bands[0..n-2]; the caller picks the tolerance band (bands[n-1]).
 * n = 4 or 5. Returns -1 if it can't be shown (too small / too big). */
int ec_ohms_to_bands(double ohms, int n, int *bands);

/* "brown-black-red-gold" (n bands; EC_NONE is left out) */
void ec_bands_text(const int *bands, int n, char *buf, size_t size);

/* ---------- standard values (E12 / E24 series) ---------- */
/* series = 12 or 24 */
double ec_e_nearest(double v, int series);      /* the closest standard value */
double ec_e_up(double v, int series);           /* the smallest standard value >= v */
/* Step to the next value (dir = +1) or the one before (dir = -1):
 *   ec_step_e:    in the E12 / E24 series (resistors, capacitors)
 *   ec_step_nice: in a list of "round" values (1, 1.2, 1.5, 2, 2.2, 2.5, 3, 3.3 ...) */
double ec_step_e(double v, int series, int dir);
double ec_step_nice(double v, int dir);

/* ---------- Ohm's law ---------- */
enum { EC_V, EC_I, EC_R, EC_P };                /* volts, amps, ohms, watts */
typedef struct { double v, i, r, p; } EcOhm;
/* Know two of V, I, R, P (qa = a, qb = b)? Get all four. */
int ec_ohms_law(int qa, double a, int qb, double b, EcOhm *out);

/* ---------- LED series resistor ---------- */
typedef struct {
    double r;          /* the exact resistor: (supply - LED voltage) / current */
    double r_std;      /* the next bigger standard value (never smaller: protects the LED) */
    double i_std;      /* the current you really get with r_std */
    double p_r;        /* heat in the resistor (watts) with r_std */
    double p_led;      /* power in the LED (watts) with r_std */
    double rating;     /* a resistor power rating that is safe: 0.25, 0.5, 1, 2 or 5 W */
} EcLed;
int ec_led_resistor(double supply, double vf, double current, int series, EcLed *out);

/* ---------- voltage divider ---------- */
double ec_divider_vout(double vin, double r1, double r2);       /* -1 = bad numbers */
double ec_divider_r2(double vin, double vout, double r1);       /* the R2 for a wanted Vout; -1 = impossible */

/* ---------- 555 timer ---------- */
typedef struct { double f, period, t_high, t_low, duty; } Ec555;  /* duty in % */
int    ec_555_astable(double r1, double r2, double c, Ec555 *out);
double ec_555_mono(double r1, double c);                           /* pulse length (s); -1 = bad */

/* ---------- unit conversion ---------- */
enum { EC_Q_CURRENT, EC_Q_VOLTAGE, EC_Q_RESISTANCE, EC_Q_CAPACITANCE,
       EC_Q_FREQUENCY, EC_Q_LENGTH, EC_Q_TEMPERATURE, EC_Q_COUNT };
const char *ec_quantity_name(int q);            /* "Current" */
int  ec_unit_count(int q);
const char *ec_unit_name(int q, int u);         /* "mA" */
int  ec_convert(int q, int from, int to, double v, double *out);
double ec_awg_mm(int awg);                      /* wire diameter in mm (AWG 0..40); -1 = bad */
double ec_awg_mm2(int awg);                     /* copper area in mm2 */

/* ---------- numbers as text ---------- */
/* 4700, "Ohm" -> "4.7 kOhm";  0.02, "A" -> "20 mA";  6.857, "Hz" -> "6.86 Hz" (3 digits) */
void ec_format(double v, const char *unit, char *buf, size_t size);
/* typed text -> number: "4k7" "4.7k" "4700" -> 4700;  "10u" "10 uF" -> 0.00001;
 * "20mA" -> 0.02;  "2.2M" -> 2200000;  "4R7" -> 4.7.   Returns 0 = ok, -1 = not a number. */
int  ec_parse(const char *text, double *out);

/* ---------- board pinouts (reference data) ---------- */
enum {
    EC_PIN_POWER = 1,     /* a supply pin (3.3 V, 5 V, VIN ...) */
    EC_PIN_GND   = 2,     /* ground */
    EC_PIN_5VT   = 4,     /* (STM32) 5 V tolerant */
    EC_PIN_PWM   = 8,     /* (Arduino) can do PWM, marked ~ on the board */
    EC_PIN_GAP   = 16,    /* a gap in the header before this pin */
};
typedef struct { const char *name, *note; int flags; } EcPin;
typedef struct {
    const char *title;           /* "STM32 Blue Pill" */
    const char *chip;            /* "STM32F103C8T6" */
    const char *logic;           /* "3.3 V logic" */
    const char *top, *bottom;    /* what is at each end of the board, e.g. "USB" */
    const EcPin *left;  int n_left;
    const EcPin *right; int n_right;
    const char *legend;          /* what the marks mean */
    const char *notes[5];        /* good to know (NULL-terminated) */
} EcBoard;
enum { EC_BOARD_BLUEPILL, EC_BOARD_UNO, EC_BOARD_NANO, EC_BOARD_ESP32, EC_BOARD_COUNT };
const EcBoard *ec_board(int b);

#endif

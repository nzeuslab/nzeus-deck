#ifndef NZEUS_CALC_EVAL_H
#define NZEUS_CALC_EVAL_H
#include <stddef.h>

/* The Calculator's maths: turns a sum typed as text, like "2+3*(4-1)", into
 * a number. It reads the text one character at a time and never runs it as
 * code, so any text is safe to give it.
 *
 * What it understands:
 *   numbers       12   3.5   .5   7.   1.5e+20 (= 1.5 x 10^20, how big answers are shown)
 *   + - * /       usual order: * and / before + and -   (x also means *)
 *   ( )           brackets, also "2(3+1)" = 2 * (3+1)
 *   -5  +5        a sign in front of a number or bracket
 *   %             50% = 0.5;  200+10% = 220;  200-10% = 180 (like a phone calculator)
 * Limits: at most CALC_MAX_LEN characters and CALC_MAX_DEPTH brackets/signs
 * inside each other (so a huge or nasty input can't crash NZEUS). */

#define CALC_MAX_LEN   200
#define CALC_MAX_DEPTH 32

/* Work out `expr`. Returns 0 and puts the answer in *out, or returns -1 and
 * puts a short message for people in err, e.g. "Can't divide by zero". */
int calc_eval(const char *expr, double *out, char *err, size_t errlen);

/* A number as the Calculator shows it: "3", "0.5", "-12.25", "1.5e+20". */
void calc_format(double v, char *buf, size_t n);

#endif

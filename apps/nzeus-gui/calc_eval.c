/* calc_eval.c - the Calculator's maths (see calc_eval.h).
 *
 * How it works: a "recursive-descent parser". Each kind of piece in a sum has
 * its own small function, and the functions call each other in the order of
 * the maths rules:
 *
 *   sum     = part  { + part  or  - part }        (lowest priority: done last)
 *   part    = sign  { * sign  or  / sign }
 *   sign    = - sign  or  + sign  or  percent
 *   percent = piece { % }
 *   piece   = number  or  ( sum )                 (highest priority: done first)
 *
 * Because sum() asks part() for its pieces, 2+3*4 becomes 2 + (3*4) = 14 by
 * itself. Brackets call sum() again from inside piece(); a depth counter stops
 * "((((((..." from going so deep that the program would run out of stack. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "calc_eval.h"

typedef struct {
    const char *s;          /* the text */
    int pos;                /* where we are reading */
    int depth;              /* brackets / signs inside each other right now */
    int pct;                /* the last part() was just "N%" (for 200+10%) */
    const char *error;      /* the first problem found (NULL = none so far) */
    char bad[2];            /* the character we did not understand */
} Parser;

static double sum(Parser *p);

static void fail(Parser *p, const char *msg) { if (!p->error) p->error = msg; }

static void skip_spaces(Parser *p) { while (p->s[p->pos] == ' ') p->pos++; }

static char peek(Parser *p) { skip_spaces(p); return p->s[p->pos]; }

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* 12   3.5   .5   7. */
static double number(Parser *p)
{
    double v = 0, scale = 1;
    int digits = 0, dot = 0;
    for (;;) {
        char c = p->s[p->pos];
        if (is_digit(c)) {
            if (dot) { scale /= 10; v += (c - '0') * scale; }
            else v = v * 10 + (c - '0');
            digits++;
        } else if (c == '.') {
            if (dot) { fail(p, "A number has two dots"); return 0; }
            dot = 1;
        } else break;
        p->pos++;
    }
    if (!digits) { fail(p, "Something is missing"); return 0; }   /* just "." */
    char e = p->s[p->pos];                                  /* 1.5e+20 = 1.5 times 10 to the 20th */
    if (e == 'e' || e == 'E') {
        int at = p->pos + 1, neg = 0, ex = 0, exdigits = 0;
        if (p->s[at] == '+' || p->s[at] == '-') neg = p->s[at++] == '-';
        while (is_digit(p->s[at])) { if (ex < 10000) ex = ex * 10 + (p->s[at] - '0'); at++; exdigits++; }
        if (exdigits) { p->pos = at; v *= pow(10, neg ? -ex : ex); }   /* (else the 'e' is junk: reported later) */
    }
    return v;
}

/* a number, or a sum in brackets */
static double piece(Parser *p)
{
    char c = peek(p);
    if (is_digit(c) || c == '.') return number(p);
    if (c == '(') {
        if (++p->depth > CALC_MAX_DEPTH) { fail(p, "Too many brackets inside each other"); return 0; }
        p->pos++;
        double v = sum(p);
        if (p->error) return 0;
        if (peek(p) != ')') { fail(p, "Something is missing"); return 0; }   /* no closing bracket */
        p->pos++;
        p->depth--;
        return v;
    }
    if (c == 0 || c == ')' || c == '*' || c == '/' || c == 'x' || c == 'X' || c == '%') {
        fail(p, "Something is missing");                   /* e.g. "2+", "()", "*3" */
        return 0;
    }
    p->bad[0] = c;                                          /* a letter or other junk */
    fail(p, "unknown");
    return 0;
}

/* 50% = 0.5 */
static double percent(Parser *p, int *had_pct)
{
    double v = piece(p);
    *had_pct = 0;
    while (!p->error && peek(p) == '%') { p->pos++; v /= 100; *had_pct = 1; }
    return v;
}

/* -5, +5, --5 */
static double sign(Parser *p, int *had_pct)
{
    char c = peek(p);
    if (c == '-' || c == '+') {
        if (++p->depth > CALC_MAX_DEPTH) { fail(p, "Too many signs in a row"); return 0; }
        p->pos++;
        double v = sign(p, had_pct);
        p->depth--;
        return c == '-' ? -v : v;
    }
    return percent(p, had_pct);
}

/* 3*4/2, and 2(3+1) = 2*(3+1) */
static double part(Parser *p)
{
    int had_pct, factors = 1;
    double v = sign(p, &had_pct);
    while (!p->error) {
        char c = peek(p);
        if (c == '*' || c == 'x' || c == 'X' || c == '/') p->pos++;
        else if (c == '(') c = '*';                         /* a bracket right after a number: times */
        else break;
        int dummy;
        double r = sign(p, &dummy);
        if (p->error) return 0;
        if (c == '/') {
            if (r == 0) { fail(p, "Can't divide by zero"); return 0; }
            v /= r;
        } else v *= r;
        factors++;
    }
    p->pct = had_pct && factors == 1;                        /* the whole part was just "N%" */
    return v;
}

static double sum(Parser *p)
{
    double v = part(p);
    while (!p->error) {
        char c = peek(p);
        if (c != '+' && c != '-') break;
        p->pos++;
        double r = part(p);
        if (p->error) return 0;
        if (p->pct) r = v * r;                              /* 200+10% = 200 + 10% of 200 */
        v = c == '+' ? v + r : v - r;
    }
    return v;
}

int calc_eval(const char *expr, double *out, char *err, size_t errlen)
{
    Parser p;
    memset(&p, 0, sizeof p);
    p.s = expr ? expr : "";
    if (strlen(p.s) > CALC_MAX_LEN) p.error = "That sum is too long";
    double v = p.error ? 0 : sum(&p);
    if (!p.error) {                                         /* something left over? */
        char c = peek(&p);
        if (c == ')') p.error = "A ( is missing";
        else if (is_digit(c) || c == '.') p.error = "A sign is missing between numbers";
        else if (c) { p.bad[0] = c; p.error = "unknown"; }
    }
    if (!p.error && !isfinite(v)) p.error = "That number is too big";
    if (p.error) {
        if (!strcmp(p.error, "unknown")) snprintf(err, errlen, "I don't understand \"%s\"", p.bad);
        else snprintf(err, errlen, "%s", p.error);
        return -1;
    }
    if (v == 0) v = 0;                                      /* turn -0 into 0 */
    *out = v;
    if (errlen) err[0] = 0;
    return 0;
}

void calc_format(double v, char *buf, size_t n)
{
    if (v == 0) { snprintf(buf, n, "0"); return; }
    if (fabs(v) < 1e15 && v == floor(v)) snprintf(buf, n, "%.0f", v);   /* whole numbers: no dot */
    else snprintf(buf, n, "%.10g", v);                      /* 10 digits: 1/3 = 0.3333333333 */
}

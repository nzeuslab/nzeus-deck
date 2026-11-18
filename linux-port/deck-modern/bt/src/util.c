/* util.c - small helpers: addresses, hex, names, time, debug printing. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "bt.h"

int g_verbose = 0;

/* Debug line to stderr, only with -v (so normal output stays clean for the GUI). */
void dbg(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose) return;
    va_start(ap, fmt);
    fputs("  [debug] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "AA:BB:CC:DD:EE:FF" -> address. Also accepts '-' (our file names) and
 * lower case. Exactly 6 pairs, nothing after. */
int addr_parse(const char *s, bdaddr_t *a)
{
    int i;
    if (!s) return -1;
    for (i = 0; i < 6; i++) {
        int hi = hexval(s[0]), lo = hi < 0 ? -1 : hexval(s[1]);
        if (hi < 0 || lo < 0) return -1;
        a->b[5 - i] = (uint8_t)(hi << 4 | lo);   /* written backwards, see bt.h */
        s += 2;
        if (i < 5) {
            if (*s != ':' && *s != '-') return -1;
            s++;
        }
    }
    return *s ? -1 : 0;
}

void addr_str(const bdaddr_t *a, char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->b[5], a->b[4], a->b[3], a->b[2], a->b[1], a->b[0]);
}

void addr_file(const bdaddr_t *a, char out[18])
{
    snprintf(out, 18, "%02X-%02X-%02X-%02X-%02X-%02X",
             a->b[5], a->b[4], a->b[3], a->b[2], a->b[1], a->b[0]);
}

int addr_eq(const bdaddr_t *a, const bdaddr_t *b)
{
    return memcmp(a->b, b->b, 6) == 0;
}

const char *addr_type_str(uint8_t type)
{
    switch (type) {
    case ADDR_BREDR:     return "classic";
    case ADDR_LE_PUBLIC: return "le-public";
    case ADDR_LE_RANDOM: return "le-random";
    }
    return "unknown";
}

int addr_type_parse(const char *s)
{
    if (!strcmp(s, "classic"))   return ADDR_BREDR;
    if (!strcmp(s, "le-public")) return ADDR_LE_PUBLIC;
    if (!strcmp(s, "le-random")) return ADDR_LE_RANDOM;
    return -1;
}

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* bytes -> lower-case hex text. Returns characters written (without the 0). */
int hex_encode(const uint8_t *in, size_t n, char *out, size_t outsz)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (outsz < n * 2 + 1) return -1;
    for (i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
    return (int)(2 * n);
}

/* hex text -> bytes. Stops at the end of the text (or a space/newline).
 * Returns the number of bytes, or -1 if the text is not valid hex. */
int hex_decode(const char *in, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (*in && !isspace((unsigned char)*in)) {
        int hi = hexval(in[0]), lo;
        if (hi < 0) return -1;
        lo = hexval(in[1]);
        if (lo < 0) return -1;
        if (n >= max) return -1;
        out[n++] = (uint8_t)(hi << 4 | lo);
        in += 2;
    }
    return (int)n;
}

/* Device names come from the air: make them safe to print in one line.
 * Control characters (tabs, newlines...) become spaces; UTF-8 letters stay.
 * Leading/trailing spaces are removed. */
void clean_name(const char *in, size_t n, char *out, size_t outsz)
{
    size_t i, o = 0;
    for (i = 0; i < n && in[i] && o + 1 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    out[o] = 0;
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    for (i = 0; out[i] == ' '; i++) ;
    if (i) memmove(out, out + i, strlen(out + i) + 1);
}

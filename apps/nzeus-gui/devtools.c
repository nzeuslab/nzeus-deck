/* devtools.c - the Dev toolbox's workers: number bases, Base64, hex, URL
 * encoding, hashes, Unix time, colours, JSON and regular expressions.
 * Only text in, text out: no screen code here (that is app_devtools.c),
 * so every tool can be tested on its own (tests/test_tools.c). */
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devtools.h"
#include "sha256.h"

/* ---------- small helpers ---------- */

/* add formatted text at out+*k, never past n (a full buffer just stops growing) */
static void put(char *out, size_t n, size_t *k, const char *fmt, ...)
{
    if (*k + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *k, n - *k, fmt, ap);
    va_end(ap);
    if (w > 0) *k += (size_t)w < n - *k ? (size_t)w : n - *k - 1;
}

/* say what is wrong (the return value for "bad input") */
static int bad(char *out, size_t n, const char *why)
{
    snprintf(out, n, "%s", why);
    return -1;
}

/* the input without spaces at both ends, copied into buf */
static const char *trim(const char *in, char *buf, size_t n)
{
    while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r') in++;
    snprintf(buf, n, "%s", in);
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    return buf;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decoded bytes: as text if they look like text, otherwise as hex */
static void show_bytes(const unsigned char *b, size_t len, char *out, size_t n)
{
    size_t k = 0;
    int text = 1;
    for (size_t i = 0; i < len; i++)
        if (b[i] < 32 && b[i] != '\n' && b[i] != '\t' && b[i] != '\r') text = 0;   /* (bytes >= 128: UTF-8 letters) */
    if (text) { put(out, n, &k, "%.*s", (int)len, (const char *)b); if (!len) out[0] = 0; return; }
    put(out, n, &k, "(not text: %u bytes)\n", (unsigned)len);
    for (size_t i = 0; i < len; i++) put(out, n, &k, "%02X%s", b[i], i + 1 < len ? " " : "");
}

/* ---------- number bases ---------- */

/* binary with a space every 4 digits ("1111 1111"), at least 4 digits */
static void binary(unsigned long long v, char *out, size_t n)
{
    char digits[65];
    int d = 0;
    do { digits[d++] = (char)('0' + (v & 1)); v >>= 1; } while (v);
    while (d % 4) digits[d++] = '0';
    size_t k = 0;
    for (int i = d - 1; i >= 0; i--) {
        put(out, n, &k, "%c", digits[i]);
        if (i && i % 4 == 0) put(out, n, &k, " ");
    }
}

int dt_number(const char *in, char *out, size_t n)
{
    char buf[128], clean[128];
    const char *s = trim(in, buf, sizeof buf);
    unsigned long long v;
    int neg = 0;
    if (!*s) return bad(out, n, "Type a number: 255, 0xFF, 0b1111, 0o377 or 'A'");
    if (s[0] == '\'' && s[1] && s[2] == '\'' && !s[3]) v = (unsigned char)s[1];   /* a character: 'A' */
    else {
        size_t c = 0;                                       /* drop the _ and spaces people put in long numbers */
        for (const char *p = s; *p && c + 1 < sizeof clean; p++) if (*p != '_' && *p != ' ') clean[c++] = *p;
        clean[c] = 0;
        const char *p = clean;
        if (*p == '-') { neg = 1; p++; }
        int base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
        else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) { base = 8; p += 2; }
        if (!*p || *p == '-' || *p == '+') return bad(out, n, "That is not a number");
        char *end;
        errno = 0;
        v = strtoull(p, &end, base);
        if (*end) return bad(out, n, base == 2 ? "Binary uses only 0 and 1" : base == 8 ? "Octal uses only 0 to 7" :
                                     base == 16 ? "Hex uses only 0-9 and A-F" : "That is not a number");
        if (errno == ERANGE) return bad(out, n, "Too big (more than 64 bits)");
        if (neg) {                                          /* negative: shown as a 32-bit two's complement */
            if (v > 2147483648ULL) return bad(out, n, "Too small (below -2147483648)");
            v = (unsigned long long)(uint32_t)(-(long long)v);
        }
    }
    size_t k = 0;
    char bin[100];
    binary(v, bin, sizeof bin);
    if (neg) put(out, n, &k, "Decimal   -%llu\n", (unsigned long long)((uint32_t)-(uint32_t)v));
    else     put(out, n, &k, "Decimal   %llu\n", v);
    put(out, n, &k, "Hex       0x%llX%s\n", v, neg ? "  (32-bit)" : "");
    put(out, n, &k, "Binary    %s\n", bin);
    put(out, n, &k, "Octal     0%llo", v);
    if (v >= 32 && v <= 126) put(out, n, &k, "\nCharacter '%c'", (char)v);
    return 0;
}

/* ---------- Base64, hex, URL ---------- */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int dt_b64_encode(const char *in, char *out, size_t n)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t len = strlen(in), k = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)s[i] << 16 | (i + 1 < len ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < len ? s[i + 2] : 0);
        put(out, n, &k, "%c%c%c%c", b64[v >> 18 & 63], b64[v >> 12 & 63],
            i + 1 < len ? b64[v >> 6 & 63] : '=', i + 2 < len ? b64[v & 63] : '=');
    }
    if (!len) out[0] = 0;
    return 0;
}

int dt_b64_decode(const char *in, char *out, size_t n)
{
    unsigned char bytes[1024];
    size_t len = 0;
    unsigned v = 0;
    int bits = 0, pad = 0;
    for (const char *p = in; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') continue;
        if (*p == '=') { pad++; continue; }
        const char *at = *p ? strchr(b64, *p) : NULL;
        if (*p == '-') at = b64 + 62;                       /* (the URL-safe alphabet too) */
        if (*p == '_') at = b64 + 63;
        if (!at) return bad(out, n, "Not Base64: it uses only A-Z a-z 0-9 + / and = at the end");
        if (pad) return bad(out, n, "Not Base64: = can only be at the end");
        v = v << 6 | (unsigned)(at - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (len < sizeof bytes) bytes[len++] = (unsigned char)(v >> bits);
        }
    }
    if (pad > 2 || bits >= 6) return bad(out, n, "Not Base64: the length is wrong (a piece is missing)");
    show_bytes(bytes, len, out, n);
    return 0;
}

int dt_hex_encode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    out[0] = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) put(out, n, &k, "%02X%s", *p, p[1] ? " " : "");
    return 0;
}

int dt_hex_decode(const char *in, char *out, size_t n)
{
    unsigned char bytes[1024];
    size_t len = 0;
    int half = -1;
    for (const char *p = in; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == ',' || *p == '\n' || *p == '-') continue;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && half < 0) { p++; continue; }   /* 0x41 0x42 */
        int h = hexval(*p);
        if (h < 0) return bad(out, n, "Not hex: use 0-9 and A-F (spaces, : and 0x are fine)");
        if (half < 0) half = h;
        else { if (len < sizeof bytes) bytes[len++] = (unsigned char)(half << 4 | h); half = -1; }
    }
    if (half >= 0) return bad(out, n, "Not hex: one digit is missing (each byte is 2 digits)");
    show_bytes(bytes, len, out, n);
    return 0;
}

int dt_url_encode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    out[0] = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') put(out, n, &k, "%c", *p);
        else put(out, n, &k, "%%%02X", *p);
    }
    return 0;
}

int dt_url_decode(const char *in, char *out, size_t n)
{
    unsigned char bytes[1024];
    size_t len = 0;
    for (const char *p = in; *p && len < sizeof bytes; p++) {
        if (*p == '%') {
            int h = hexval(p[1]), l = h >= 0 ? hexval(p[2]) : -1;
            if (h < 0 || l < 0) return bad(out, n, "A % must be followed by 2 hex digits (like %20)");
            bytes[len++] = (unsigned char)(h << 4 | l);
            p += 2;
        } else bytes[len++] = (unsigned char)(*p == '+' ? ' ' : *p);
    }
    show_bytes(bytes, len, out, n);
    return 0;
}

/* ---------- hashes ---------- */

/* CRC-32 (the one in ZIP files, Ethernet and PNG pictures), one bit at a time */
uint32_t dt_crc32(const void *data, size_t len)
{
    const unsigned char *b = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= b[i];
        for (int j = 0; j < 8; j++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

int dt_hash(const char *in, char *out, size_t n)
{
    char hex[65];
    size_t len = strlen(in), k = 0;
    sha256_hex(in, len, hex);
    put(out, n, &k, "SHA-256\n%s\n", hex);
    put(out, n, &k, "CRC-32   0x%08X\n", (unsigned)dt_crc32(in, len));
    put(out, n, &k, "Length   %u bytes", (unsigned)len);
    return 0;
}

/* ---------- Unix time ---------- */

/* "3 days ago" / "in 2 hours" */
static void how_long(long long d, char *out, size_t n)
{
    long long a = d < 0 ? -d : d;
    const char *unit = "second";
    long long v = a;
    if (a >= 86400 * 365LL) { v = a / (86400 * 365LL); unit = "year"; }
    else if (a >= 86400) { v = a / 86400; unit = "day"; }
    else if (a >= 3600) { v = a / 3600; unit = "hour"; }
    else if (a >= 60) { v = a / 60; unit = "minute"; }
    if (a < 5) { snprintf(out, n, "now"); return; }
    snprintf(out, n, d < 0 ? "%lld %s%s ago" : "in %lld %s%s", v, unit, v == 1 ? "" : "s");
}

int dt_time(const char *in, time_t now, char *out, size_t n)
{
    char buf[64], when[48], rel[48];
    const char *s = trim(in, buf, sizeof buf);
    time_t t;
    int ms = 0;
    if (!*s) t = now;                                       /* nothing typed: now */
    else if (strspn(s + (s[0] == '-'), "0123456789") == strlen(s + (s[0] == '-'))) {
        long long v = atoll(s);
        if (v > 100000000000LL || v < -100000000000LL) { v /= 1000; ms = 1; }   /* 13 digits: milliseconds */
        t = (time_t)v;
    } else {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        int y, mo, d, h = 0, mi = 0, se = 0;
        char sep;
        int got = sscanf(s, "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &se);
        if (got != 3 && got != 6 && got != 7) return bad(out, n, "Type an epoch (1759800000) or a date: 2026-10-07 or 2026-10-07 14:30");
        if (got > 3 && sep != ' ' && sep != 'T') return bad(out, n, "Put a space (or T) between the date and the time");
        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60 || h < 0 || mi < 0 || se < 0)
            return bad(out, n, "That date or time doesn't exist");
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
        tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
        t = timegm(&tm);
    }
    size_t k = 0;
    struct tm u;
    if (!gmtime_r(&t, &u)) return bad(out, n, "Out of range");
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S (%A)", &u);
    how_long((long long)(t - now), rel, sizeof rel);
    put(out, n, &k, "Epoch     %lld%s\n", (long long)t, ms ? "  (you typed milliseconds)" : "");
    put(out, n, &k, "UTC       %s\n", when);
    struct tm l;
    if (localtime_r(&t, &l)) { strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &l); put(out, n, &k, "Local     %s\n", when); }
    put(out, n, &k, "From now  %s", rel);
    return 0;
}

/* ---------- colours ---------- */

int dt_colour(const char *in, char *out, size_t n, uint32_t *rgb)
{
    char buf[64];
    const char *s = trim(in, buf, sizeof buf);
    int r, g, b;
    unsigned v;
    char extra;
    size_t len = strlen(s);
    if ((!strncmp(s, "565:", 4) || ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) && len == 6)) &&
        sscanf(s + (s[0] == '5' ? 4 : 2), "%4x%c", &v, &extra) == 1) {   /* RGB565 (small LCD screens) */
        r = (int)((v >> 11 & 31) * 255 + 15) / 31;
        g = (int)((v >> 5 & 63) * 255 + 31) / 63;
        b = (int)((v & 31) * 255 + 15) / 31;
    } else if (strchr(s, ',') || (strchr(s, ' ') && s[0] != '#')) {   /* 255, 128, 0 */
        if (sscanf(s, "%d%*[ ,]%d%*[ ,]%d%c", &r, &g, &b, &extra) != 3) return bad(out, n, "Type 3 numbers: 255, 128, 0");
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return bad(out, n, "Each number is 0 to 255");
    } else {                                                /* #FF8000, FF8000 or #F80 */
        if (*s == '#') { s++; len--; }
        if ((len != 6 && len != 3) || strspn(s, "0123456789abcdefABCDEF") != len)
            return bad(out, n, "Type #FF8000, #F80, 255,128,0 or 565:FC00");
        if (len == 3) { r = hexval(s[0]) * 17; g = hexval(s[1]) * 17; b = hexval(s[2]) * 17; }
        else { sscanf(s, "%6x", &v); r = (int)(v >> 16 & 255); g = (int)(v >> 8 & 255); b = (int)(v & 255); }
    }
    size_t k = 0;
    unsigned c565 = (unsigned)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
    put(out, n, &k, "Hex      #%02X%02X%02X\n", r, g, b);
    put(out, n, &k, "RGB      %d, %d, %d\n", r, g, b);
    put(out, n, &k, "RGB565   0x%04X  (for small LCD screens)\n", c565);
    put(out, n, &k, "In C     lv_color_hex(0x%02X%02X%02X)", r, g, b);
    if (rgb) *rgb = (uint32_t)(r << 16 | g << 8 | b);
    return 0;
}

/* ---------- JSON ---------- */

/* A small JSON reader that writes the same value back out, neatly indented.
 * It stops at the first mistake and says where it is. */
typedef struct {
    const char *s, *start;
    char *out;
    size_t n, k;
    int depth;
    const char *err;
} Js;

static void js_ws(Js *j) { while (*j->s == ' ' || *j->s == '\t' || *j->s == '\n' || *j->s == '\r') j->s++; }
static void js_indent(Js *j) { put(j->out, j->n, &j->k, "\n%*s", j->depth * 2, ""); }
static int js_value(Js *j);

static int js_string(Js *j)
{
    const char *b = j->s++;                                 /* (the opening ") */
    while (*j->s != '"') {
        if (!*j->s || *j->s == '\n') { j->err = "a text in \" \" is not closed"; return 0; }
        if (*j->s == '\\') {
            j->s++;
            if (*j->s == 'u') {
                for (int i = 1; i <= 4; i++) if (hexval(j->s[i]) < 0) { j->err = "\\u needs 4 hex digits"; return 0; }
                j->s += 4;
            } else if (!strchr("\"\\/bfnrt", *j->s) || !*j->s) { j->err = "unknown \\ escape"; return 0; }
        }
        j->s++;
    }
    j->s++;
    put(j->out, j->n, &j->k, "%.*s", (int)(j->s - b), b);
    return 1;
}

static int js_number(Js *j)
{
    const char *b = j->s;
    if (*j->s == '-') j->s++;
    if (!isdigit((unsigned char)*j->s)) { j->err = "a value is missing or wrong"; return 0; }
    if (*j->s == '0' && isdigit((unsigned char)j->s[1])) { j->err = "numbers can't start with 0"; return 0; }
    while (isdigit((unsigned char)*j->s)) j->s++;
    if (*j->s == '.') {
        j->s++;
        if (!isdigit((unsigned char)*j->s)) { j->err = "a digit is missing after the ."; return 0; }
        while (isdigit((unsigned char)*j->s)) j->s++;
    }
    if (*j->s == 'e' || *j->s == 'E') {
        j->s++;
        if (*j->s == '+' || *j->s == '-') j->s++;
        if (!isdigit((unsigned char)*j->s)) { j->err = "a digit is missing after the e"; return 0; }
        while (isdigit((unsigned char)*j->s)) j->s++;
    }
    put(j->out, j->n, &j->k, "%.*s", (int)(j->s - b), b);
    return 1;
}

/* an object { } or a list [ ] */
static int js_group(Js *j, char close)
{
    if (++j->depth > 32) { j->err = "nested too deep (over 32)"; return 0; }
    put(j->out, j->n, &j->k, "%c", close == '}' ? '{' : '[');
    j->s++;
    js_ws(j);
    if (*j->s == close) { j->s++; j->depth--; put(j->out, j->n, &j->k, "%c", close); return 1; }   /* {} or [] */
    for (;;) {
        js_indent(j);
        js_ws(j);
        if (close == '}') {
            if (*j->s != '"') { j->err = "a name in \" \" was expected"; return 0; }
            if (!js_string(j)) return 0;
            js_ws(j);
            if (*j->s != ':') { j->err = "a : was expected after the name"; return 0; }
            j->s++;
            put(j->out, j->n, &j->k, ": ");
        }
        if (!js_value(j)) return 0;
        js_ws(j);
        if (*j->s == ',') { j->s++; put(j->out, j->n, &j->k, ","); continue; }
        if (*j->s == close) break;
        j->err = close == '}' ? "a , or } was expected" : "a , or ] was expected";
        return 0;
    }
    j->s++;
    j->depth--;
    js_indent(j);
    put(j->out, j->n, &j->k, "%c", close);
    return 1;
}

static int js_value(Js *j)
{
    js_ws(j);
    switch (*j->s) {
    case '{': return js_group(j, '}');
    case '[': return js_group(j, ']');
    case '"': return js_string(j);
    case 't': if (!strncmp(j->s, "true", 4))  { j->s += 4; put(j->out, j->n, &j->k, "true");  return 1; } break;
    case 'f': if (!strncmp(j->s, "false", 5)) { j->s += 5; put(j->out, j->n, &j->k, "false"); return 1; } break;
    case 'n': if (!strncmp(j->s, "null", 4))  { j->s += 4; put(j->out, j->n, &j->k, "null");  return 1; } break;
    case '\'': j->err = "JSON uses \" for text, not '"; return 0;
    default: return js_number(j);
    }
    j->err = "unknown word (only true, false and null)";
    return 0;
}

int dt_json(const char *in, char *out, size_t n)
{
    Js j = { in, in, out, n, 0, 0, NULL };
    out[0] = 0;
    int ok = js_value(&j);
    if (ok) { js_ws(&j); if (*j.s) { ok = 0; j.err = "there is more after the end"; } }
    if (ok) return 0;
    int line = 1;
    for (const char *p = in; p < j.s; p++) if (*p == '\n') line++;
    snprintf(out, n, "Not valid JSON: %s\n(at character %d, line %d)", j.err ? j.err : "?", (int)(j.s - in) + 1, line);
    return -1;
}

/* ---------- regular expressions ---------- */

int dt_regex(const char *pattern, const char *text, char *out, size_t n)
{
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc) {
        char why[128];
        regerror(rc, &re, why, sizeof why);
        snprintf(out, n, "The pattern has a mistake: %s", why);
        return -1;
    }
    regmatch_t m[4];
    size_t k = 0;
    int found = 0, off = 0, len = (int)strlen(text);
    while (off <= len && found < 10 && regexec(&re, text + off, 4, m, off ? REG_NOTBOL : 0) == 0) {
        int a = off + (int)m[0].rm_so, b = off + (int)m[0].rm_eo;
        found++;
        put(out, n, &k, "%sMatch %d at %d-%d: \"%.*s\"", found > 1 ? "\n" : "", found, a, b, b - a, text + a);
        for (size_t g = 1; g < 4 && g <= re.re_nsub; g++)
            if (m[g].rm_so >= 0) put(out, n, &k, "\n   group %u: \"%.*s\"", (unsigned)g, (int)(m[g].rm_eo - m[g].rm_so), text + off + m[g].rm_so);
        off = b > a ? b : b + 1;                            /* (an empty match: step on, or it loops forever) */
    }
    regfree(&re);
    if (!found) snprintf(out, n, "No match");
    return 0;
}

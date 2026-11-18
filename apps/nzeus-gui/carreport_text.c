/* carreport_text.c - Arabic for the car report: joining letters and
 * right-to-left order.
 *
 * Why this is needed: a computer stores Arabic as plain letters in reading
 * order ("ل", "ا", "ن"...). To show it correctly two things must happen:
 *   1. joining ("shaping"): each letter gets the shape that fits its place in
 *      the word - alone, at the start, in the middle or at the end (like
 *      handwriting). Unicode has a ready-made code for every one of those
 *      shapes (the "presentation forms", U+FE80-FEFC), and our report font
 *      contains exactly those shapes. So shaping = swapping codes.
 *   2. ordering ("bidi"): Arabic is read right to left, but numbers and
 *      English words inside it (B10F3, OBDLink, 12.2) still read left to
 *      right. The drawing code always draws left to right, so each line is
 *      turned into "drawing order" first.
 * LVGL 8.3 has both features (LV_USE_ARABIC_PERSIAN_CHARS, LV_USE_BIDI), but
 * its joining table misses "آ" (so "الآن" - "now", a column title of the
 * report - comes out broken) and turning them on would change every label of
 * the GUI. Doing it here keeps the GUI exactly as it is. */
#include <string.h>
#include "carreport_int.h"

/* ---------- UTF-8 (how the text is stored: 1 to 4 bytes per character) ---------- */

uint32_t rp_utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int n = 0;
    if (c == 0) return 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *p += 1; return '?'; }                   /* broken byte: show a ? */
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return '?'; }   /* cut-off character */
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p += n + 1;
    return c;
}

int rp_utf8_put(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

int rp_is_mark(uint32_t c)
{
    return (c >= 0x064B && c <= 0x0652) || c == 0x0670 || (c >= 0x0300 && c <= 0x036F);
}

static int is_arabic(uint32_t c)
{
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

int rp_has_arabic(const char *s)
{
    uint32_t c;
    while ((c = rp_utf8_next(&s)) != 0)
        if (is_arabic(c)) return 1;
    return 0;
}

/* ---------- 1. joining ---------- */

/* For each Arabic letter U+0621..U+064A: its "alone" shape and how it joins.
 *   J_NONE  never joins (hamza ء)
 *   J_RIGHT joins only to the letter before it (ا د ذ ر ز و ة ...):
 *           shapes alone, end            = iso, iso+1
 *   J_DUAL  joins on both sides (ب ت س ل م ن ي ...):
 *           shapes alone, end, start, middle = iso, iso+1, iso+2, iso+3 */
enum { J_NO = 0, J_NONE, J_RIGHT, J_DUAL };
static const struct { uint16_t iso; uint8_t type; } LETTERS[0x064A - 0x0621 + 1] = {
    {0xFE80, J_NONE},  {0xFE81, J_RIGHT}, {0xFE83, J_RIGHT}, {0xFE85, J_RIGHT},   /* ء آ أ ؤ */
    {0xFE87, J_RIGHT}, {0xFE89, J_DUAL},  {0xFE8D, J_RIGHT}, {0xFE8F, J_DUAL},    /* إ ئ ا ب */
    {0xFE93, J_RIGHT}, {0xFE95, J_DUAL},  {0xFE99, J_DUAL},  {0xFE9D, J_DUAL},    /* ة ت ث ج */
    {0xFEA1, J_DUAL},  {0xFEA5, J_DUAL},  {0xFEA9, J_RIGHT}, {0xFEAB, J_RIGHT},   /* ح خ د ذ */
    {0xFEAD, J_RIGHT}, {0xFEAF, J_RIGHT}, {0xFEB1, J_DUAL},  {0xFEB5, J_DUAL},    /* ر ز س ش */
    {0xFEB9, J_DUAL},  {0xFEBD, J_DUAL},  {0xFEC1, J_DUAL},  {0xFEC5, J_DUAL},    /* ص ض ط ظ */
    {0xFEC9, J_DUAL},  {0xFECD, J_DUAL},                                          /* ع غ */
    {0, J_NO}, {0, J_NO}, {0, J_NO}, {0, J_NO}, {0, J_NO},                        /* (rare letters) */
    {0x0640, J_DUAL},                                                             /* ـ tatweel */
    {0xFED1, J_DUAL},  {0xFED5, J_DUAL},  {0xFED9, J_DUAL},  {0xFEDD, J_DUAL},    /* ف ق ك ل */
    {0xFEE1, J_DUAL},  {0xFEE5, J_DUAL},  {0xFEE9, J_DUAL},  {0xFEED, J_RIGHT},   /* م ن ه و */
    {0xFEEF, J_RIGHT}, {0xFEF1, J_DUAL},                                          /* ى ي */
};

static int join_type(uint32_t c)
{
    if (c >= 0x0621 && c <= 0x064A) return LETTERS[c - 0x0621].type;
    return J_NO;
}

/* the shape code: form 0 alone, 1 end, 2 start, 3 middle */
static uint32_t shape_of(uint32_t c, int form)
{
    if (c == 0x0640) return 0x0640;                       /* tatweel looks the same everywhere */
    return LETTERS[c - 0x0621].iso + form;
}

/* lam + alef are always written as one combined sign (لا) */
static uint32_t lam_alef(uint32_t alef)
{
    switch (alef) {
    case 0x0622: return 0xFEF5;      /* لآ */
    case 0x0623: return 0xFEF7;      /* لأ */
    case 0x0625: return 0xFEF9;      /* لإ */
    case 0x0627: return 0xFEFB;      /* لا */
    default:     return 0;
    }
}

int rp_shape_arabic(const char *in, char *out, int out_n)
{
    /* read all characters first (we need to look at the neighbours) */
    int n = 0, cap = (int)strlen(in) + 1;
    uint32_t *cp = rp_malloc(sizeof(uint32_t) * (size_t)cap);
    if (!cp) return -1;
    const char *p = in;
    uint32_t c;
    while ((c = rp_utf8_next(&p)) != 0) cp[n++] = c;

    int o = 0;
    for (int i = 0; i < n; i++) {
        c = cp[i];
        int t = join_type(c);
        uint32_t res = c;
        if (c == 0x200C || c == 0x200D) continue;          /* invisible joining hints: drop */
        if (t != J_NO && t != J_NONE) {
            /* the letters before and after, skipping vowel marks */
            int a = i - 1, b = i + 1;
            while (a >= 0 && rp_is_mark(cp[a])) a--;
            while (b < n && rp_is_mark(cp[b])) b++;
            int before = (a >= 0 && join_type(cp[a]) == J_DUAL);    /* it reaches out to us */
            int after = (t == J_DUAL && b < n && join_type(cp[b]) >= J_RIGHT);
            uint32_t la = (c == 0x0644 && i + 1 < n) ? lam_alef(cp[i + 1]) : 0;
            if (la) {
                res = la + (before ? 1 : 0);                 /* the joined-on-the-right version is +1 */
                i++;                                         /* the alef is used up */
            } else if (before && after) res = shape_of(c, 3);
            else if (after) res = shape_of(c, 2);
            else if (before) res = shape_of(c, 1);
            else res = shape_of(c, 0);
        } else if (t == J_NONE) {
            res = shape_of(c, 0);
        }
        if (o + 5 > out_n) { rp_free(cp); return -1; }
        o += rp_utf8_put(res, out + o);
    }
    out[o] = 0;
    rp_free(cp);
    return o;
}

/* ---------- 2. right-to-left order ---------- */

enum { D_L, D_R, D_NUM, D_NEUTRAL, D_MARK };

static int dir_class(uint32_t c)
{
    if (rp_is_mark(c)) return D_MARK;
    if ((c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669)) return D_NUM;
    if (is_arabic(c)) return D_R;
    if (c < 0x80) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return D_L;
        return D_NEUTRAL;                                 /* space, . , : ( ) / + - ... */
    }
    if (c == 0xA0 || c == 0xB0 || c == 0xB1 || c == 0xB7 || c == 0xD7 || c == 0x2212 ||
        (c >= 0x2000 && c <= 0x206F))
        return D_NEUTRAL;                                 /* · ° × – “ ” … */
    if ((c >= 0x2100 && c <= 0x2BFF) || (c >= 0x1F000 && c <= 0x1FAFF))
        return D_NEUTRAL;                                 /* symbols, arrows, emoji: follow the text around them */
    return D_L;                                           /* other letters (é, Ω ...) */
}

static uint32_t mirror(uint32_t c)
{
    switch (c) {
    case '(': return ')';  case ')': return '(';
    case '[': return ']';  case ']': return '[';
    case '{': return '}';  case '}': return '{';
    case '<': return '>';  case '>': return '<';
    case 0xAB: return 0xBB; case 0xBB: return 0xAB;       /* « » */
    default: return c;
    }
}

/* A simplified version of the Unicode bidi rules, enough for this report:
 *  - Arabic letters go right to left; English words and numbers left to right
 *  - spaces and signs between two left-to-right pieces stay with them, so
 *    "FORScan 2.3.71", "12.2 V" and "BdyCM / HCM" keep their order
 *  - any other sign follows the paragraph direction
 *  - brackets are mirrored inside right-to-left pieces
 *  - vowel marks stay right after their letter */
int rp_visual_order(const char *in, int len, int rtl, char *out, int out_n)
{
    /* characters of this line, without spaces / line breaks at the ends */
    uint32_t *cp = rp_malloc(sizeof(uint32_t) * (size_t)(len + 1));
    uint8_t *d = rp_malloc((size_t)len + 1);
    if (!cp || !d) { rp_free(cp); rp_free(d); return -1; }
    int n = 0;
    const char *p = in, *end = in + len;
    while (p < end && *p) {
        uint32_t c = rp_utf8_next(&p);
        if (c == '\n' || c == '\r') continue;
        cp[n++] = c;
    }
    while (n > 0 && (cp[n - 1] == ' ' || cp[n - 1] == 0xA0)) n--;
    int s0 = 0;
    while (s0 < n && cp[s0] == ' ') s0++;

    /* 1) the direction of every character: L (left-to-right) or R */
    int base = rtl ? D_R : D_L;
    for (int i = s0; i < n; i++) {
        int k = dir_class(cp[i]);
        d[i] = (uint8_t)(k == D_NUM ? D_L : k);           /* numbers read left to right */
    }
    for (int i = s0; i < n; i++) {
        if (d[i] == D_NEUTRAL) {
            int a = i - 1, b = i + 1;
            while (a >= s0 && (d[a] == D_NEUTRAL || d[a] == D_MARK)) a--;
            while (b < n && (d[b] == D_NEUTRAL || d[b] == D_MARK)) b++;
            int da = a >= s0 ? d[a] : base, db = b < n ? d[b] : base;
            if (da == D_NEUTRAL || da == D_MARK) da = base;
            if (db == D_NEUTRAL || db == D_MARK) db = base;
            d[i] = (uint8_t)(da == db ? da : base);
            /* (already decided neutrals are L/R now, the next ones see them as strong:
             *  that is fine, the result is the same for a run of signs) */
        }
    }
    for (int i = s0; i < n; i++)                          /* a mark goes with its letter */
        if (d[i] == D_MARK) d[i] = (uint8_t)(i > s0 ? d[i - 1] : base);

    /* 2) cut into runs of the same direction and write them in drawing order */
    int o = 0;
    int ok = 1;
#define PUT(ch) do { if (o + 5 > out_n) { ok = 0; break; } o += rp_utf8_put((ch), out + o); } while (0)
    /* run list, in reading order */
    int nruns = 0;
    int *rs = rp_malloc(sizeof(int) * (size_t)(n - s0 + 1) * 2);
    if (!rs) { rp_free(cp); rp_free(d); return -1; }
    for (int i = s0; i < n;) {
        int j = i;
        while (j < n && d[j] == d[i]) j++;
        rs[nruns * 2] = i; rs[nruns * 2 + 1] = j;
        nruns++;
        i = j;
    }
    for (int r = 0; r < nruns && ok; r++) {
        int ri = rtl ? nruns - 1 - r : r;                 /* right-to-left paragraph: last run first */
        int a = rs[ri * 2], b = rs[ri * 2 + 1];
        if (d[a] == D_R) {
            /* backwards, but each letter keeps its marks right after it */
            int j = b;
            while (j > a && ok) {
                int k = j - 1;
                while (k > a && rp_is_mark(cp[k])) k--;   /* k = the letter, k+1..j-1 = its marks */
                for (int m = k; m < j && ok; m++) PUT(m == k ? mirror(cp[m]) : cp[m]);
                j = k;
            }
        } else {
            for (int m = a; m < b && ok; m++) PUT(cp[m]);
        }
    }
#undef PUT
    out[o] = 0;
    rp_free(rs); rp_free(cp); rp_free(d);
    return ok ? o : -1;
}

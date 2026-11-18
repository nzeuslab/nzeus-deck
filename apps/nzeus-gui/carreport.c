/* carreport.c - turns a car scan (CarScan, carscan.h) into a PDF report,
 * in English or Arabic, that looks like a real FORScan "Raptor Health Scan"
 * report - for any make (Ford's wishlist only appears for a Ford).
 *
 * Sections: title (year + make), summary box, vehicle facts (make, year,
 * masked VIN, odometer...), check-engine light + self-tests, mileage check,
 * fault codes (with their module and meaning), most likely cause, what to do,
 * live values, (Ford) wishlist, modules found, notes, footer. A section with
 * nothing to say is left out.
 *
 * How it works:
 *   1. LAYOUT: we walk through the report (title, summary box, info grid,
 *      tables...) and write down WHAT to draw WHERE on the current page:
 *      boxes and lines of text ("ops"). When the next piece does not fit,
 *      the page is finished and a new one starts (tables repeat their
 *      header row on the new page).
 *   2. DRAWING: a finished page (A4 = 1240 x 1754 pixels at 150 dpi) is drawn
 *      by LVGL's own drawing functions, 96 rows at a time, into a small
 *      off-screen buffer. Each strip goes straight into the PDF writer
 *      (carreport_pdf.c), so a whole page never sits in memory.
 * The GUI is not touched: no screens, no objects, no theme - just LVGL's
 * drawing functions on our own buffer. The report always has the same light
 * look, whatever the screen theme is.
 *
 * Sizes are in pixels at 150 dpi (1 point = 2.08 px). All x positions are
 * written for English (left to right); for Arabic every box and line is
 * mirrored (the start side is the right side). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "carscan.h"
#include "carreport_int.h"
#include "src/draw/sw/lv_draw_sw.h"    /* LVGL's software drawing (lvgl.h leaves it out) */

/* ---------- the page ---------- */
#define DPI    150
#define PW     1240               /* A4 width */
#define PH     1754               /* A4 height */
#define ML     83                 /* side margins (40 pt) */
#define CW     (PW - 2 * ML)      /* content width */
#define TOP    83                 /* first line of a page */
#define BOTTOM 1672               /* nothing goes below this */
#define STRIP  96                 /* rows drawn at once (memory: 1240 x 96 x 2 bytes) */

/* ---------- colours of the original reports (0xRRGGBB) ---------- */
#define NONE      0xFFFFFFFFu     /* "don't draw" */
#define C_INK     0x1A1F29u       /* text */
#define C_GREY    0x5B6474u       /* quieter text */
#define C_ORANGE  0xC8501Cu       /* NZEUS orange */
#define C_LINE    0xDADDE3u       /* table and grid lines */
#define C_HEAD    0xE8ECF3u       /* table header row, grey pills */
#define C_GROUP   0xF3F4F6u       /* table group rows */
#define C_FOOT    0xE3E6EAu       /* footer line */
#define C_GREEN   0x1F7A4Du
#define C_GREEN_BG 0xE4F3EAu
#define C_AMBER   0x93600Au
#define C_AMBER_BG 0xFBF0D9u
#define C_RED     0xB3261Eu
#define C_RED_BG  0xFBE5E3u

/* ---------- memory bookkeeping (so the tests can check the peak) ---------- */
static size_t mem_now, mem_peak;
#define HDR 16                    /* room in front of each block for its size (keeps alignment) */

void *rp_malloc(size_t n)
{
    unsigned char *p = malloc(n + HDR);
    if (!p) return NULL;
    *(size_t *)p = n;
    mem_now += n;
    if (mem_now > mem_peak) mem_peak = mem_now;
    return p + HDR;
}

void rp_free(void *q)
{
    if (!q) return;
    unsigned char *p = (unsigned char *)q - HDR;
    mem_now -= *(size_t *)p;
    free(p);
}

void *rp_realloc(void *q, size_t n)
{
    if (!q) return rp_malloc(n);
    unsigned char *p = (unsigned char *)q - HDR;
    size_t old = *(size_t *)p;
    unsigned char *np = realloc(p, n + HDR);
    if (!np) return NULL;
    *(size_t *)np = n;
    mem_now = mem_now - old + n;
    if (mem_now > mem_peak) mem_peak = mem_now;
    return np + HDR;
}

size_t carreport_mem_peak(void) { return mem_peak; }
void carreport_mem_reset(void) { mem_peak = mem_now; }

/* ---------- what to draw: "ops" ---------- */
enum { OP_BOX, OP_TEXT };
typedef struct {
    uint8_t kind;
    uint8_t radius, border_w;
    int8_t letter_space;
    int16_t x1, y1, x2, y2;        /* box corners; text: x1 = left end, y1 = baseline */
    uint32_t fill, border;         /* colours, NONE = not drawn */
    const lv_font_t *font;
    int text;                      /* where the text starts in R.txt */
} Op;

/* everything about the report being made */
static struct {
    const CarScan *s;
    int ar;                        /* 1 = Arabic */
    RpPdf *pdf;
    Op *ops; int nops, cap;        /* this page's drawing list */
    char *txt; int tlen, tcap;     /* this page's texts (drawing order) */
    int y;                         /* layout: the next free row on this page */
    int pages;
    int err;
    lv_color_t *strip;
    char *work, *work2; int work_n; /* scratch space for joining / ordering text */
} R;

static int last_pages;
int carreport_last_pages(void) { return last_pages; }

static Op *new_op(void)
{
    if (R.nops == R.cap) {
        int nc = R.cap ? R.cap * 2 : 256;
        Op *n = rp_realloc(R.ops, sizeof(Op) * (size_t)nc);
        if (!n) { R.err = 1; return NULL; }
        R.ops = n; R.cap = nc;
    }
    Op *o = &R.ops[R.nops++];
    memset(o, 0, sizeof *o);
    return o;
}

#ifdef NZ_TESTING
/* PC tests only: every line of text the last report drew, one per line, so a
 * test can read what the PDF says (the PDF itself holds only pictures) */
static char test_text[65536];
static size_t test_len;
const char *carreport_last_text(void) { return test_text; }
static void test_keep(const char *t)
{
    size_t n = strlen(t);
    if (test_len + n + 2 >= sizeof test_text) return;
    memcpy(test_text + test_len, t, n);
    test_len += n;
    test_text[test_len++] = '\n';
    test_text[test_len] = 0;
}
#endif

static int keep_text(const char *t)
{
#ifdef NZ_TESTING
    test_keep(t);
#endif
    int n = (int)strlen(t) + 1;
    if (R.tlen + n > R.tcap) {
        int nc = R.tcap ? R.tcap * 2 : 8192;
        while (nc < R.tlen + n) nc *= 2;
        char *x = rp_realloc(R.txt, (size_t)nc);
        if (!x) { R.err = 1; return -1; }
        R.txt = x; R.tcap = nc;
    }
    memcpy(R.txt + R.tlen, t, (size_t)n);
    R.tlen += n;
    return R.tlen - n;
}

/* content x (from the start side of the text column) -> x on the paper;
 * for Arabic the start side is the right margin */
static int mirror_x(int x, int w) { return R.ar ? PW - ML - x - w : ML + x; }

/* a box: x, w from the start side; fill / border may be NONE */
static void box(int x, int y, int w, int h, uint32_t fill, uint32_t border, int border_w, int radius)
{
    if (w <= 0 || h <= 0) return;
    Op *o = new_op();
    if (!o) return;
    o->kind = OP_BOX;
    o->x1 = (int16_t)mirror_x(x, w); o->x2 = (int16_t)(o->x1 + w - 1);
    o->y1 = (int16_t)y; o->y2 = (int16_t)(y + h - 1);
    o->fill = fill; o->border = border;
    o->border_w = (uint8_t)border_w; o->radius = (uint8_t)radius;
}

/* ---------- text: measuring and placing one line ---------- */

/* width of a line of text as LVGL will draw it */
static int text_width(const lv_font_t *f, const char *t, int ls)
{
    int w = 0, n = 0;
    const char *p = t;
    uint32_t c = rp_utf8_next(&p);
    while (c) {
        const char *q = p;
        uint32_t next = rp_utf8_next(&q);
        if (!rp_is_mark(c)) { w += lv_font_get_glyph_width(f, c, next) + ls; n++; }
        c = next; p = q;
    }
    return n ? w - ls : 0;
}

enum { AL_START, AL_END, AL_LEFT, AL_RIGHT };

/* one line, already in drawing order, inside the column [x, x+w) (start-side x) */
static void line_at(int x, int w, int baseline, const char *visual, const lv_font_t *f, uint32_t color,
                    int align, int ls)
{
    int tw = text_width(f, visual, ls);
    int left = mirror_x(x, w);                   /* the column on the paper */
    int right_side = (align == AL_RIGHT) || (align == AL_START && R.ar) || (align == AL_END && !R.ar);
    int px = right_side ? left + w - tw : left;
    int t = keep_text(visual);
    Op *o = new_op();
    if (!o || t < 0) return;
    o->kind = OP_TEXT;
    o->x1 = (int16_t)px; o->x2 = (int16_t)(px + tw);
    o->y1 = (int16_t)baseline;
    o->font = f; o->fill = color; o->text = t;
    o->letter_space = (int8_t)ls;
}

/* text style: font, colour, line pitch (distance between lines) */
typedef struct { const lv_font_t *font; uint32_t color; int pitch; int ls; } St;

/* where the baseline sits inside a line of height `pitch` (like a web browser does it) */
static int base_in_line(const lv_font_t *f, int pitch)
{
    return (pitch + f->line_height - 2 * f->base_line) / 2;
}

/* make sure the scratch buffers can hold text of n bytes (joined Arabic is up to 1.5x longer) */
static int work_room(int n)
{
    int need = n * 3 + 64;              /* room for joined Arabic and glued numbers */
    if (need <= R.work_n) return 0;
    rp_free(R.work); rp_free(R.work2);
    R.work = rp_malloc((size_t)need);
    R.work2 = rp_malloc((size_t)need);
    R.work_n = need;
    if (!R.work || !R.work2) { R.err = 1; R.work_n = 0; return -1; }
    return 0;
}

#define MAX_LINES 64
typedef struct { int n; int start[MAX_LINES]; int len[MAX_LINES]; } Lines;

/* joins the Arabic letters of `text` into R.work and breaks it into lines no
 * wider than w, with LVGL's own line breaking */
static void wrap(const char *text, const lv_font_t *f, int ls, int w, Lines *L)
{
    L->n = 1;                            /* (on any trouble: one empty line) */
    L->start[0] = 0;
    L->len[0] = 0;
    if (!text) text = "";
    if (work_room((int)strlen(text)) < 0) return;
    if (rp_shape_arabic(text, R.work2, R.work_n) < 0) { R.err = 1; R.work[0] = 0; return; }
    /* numbers must not be split at their . , or : (LVGL breaks lines there):
     * between two digits use the font's look-alike "glue" signs U+E000-E002 */
    int o = 0;
    for (const char *p = R.work2; *p; p++) {
        int glue = (p[0] == '.' || p[0] == ',' || p[0] == ':') && p > R.work2 &&
                   p[-1] >= '0' && p[-1] <= '9' && p[1] >= '0' && p[1] <= '9';
        if (glue && o + 4 < R.work_n) o += rp_utf8_put(p[0] == '.' ? 0xE000 : p[0] == ',' ? 0xE001 : 0xE002, R.work + o);
        else if (o + 1 < R.work_n) R.work[o++] = *p;
    }
    R.work[o] = 0;
    L->n = 0;
    const char *t = R.work;
    int pos = 0, total = (int)strlen(t);
    while (pos < total && L->n < MAX_LINES) {
        uint32_t n = _lv_txt_get_next_line(t + pos, f, (lv_coord_t)ls, (lv_coord_t)w, NULL, LV_TEXT_FLAG_NONE);
        if (n == 0) n = (uint32_t)(total - pos);   /* (safety: always move on) */
        L->start[L->n] = pos; L->len[L->n] = (int)n;
        L->n++;
        pos += (int)n;
    }
    if (L->n == 0) { L->start[0] = 0; L->len[0] = 0; L->n = 1; }
}

/* line `i` of a wrapped text, in drawing order (in R.work2) */
static const char *line_visual(const Lines *L, int i)
{
    if (!R.work || !R.work2) return "";
    if (rp_visual_order(R.work + L->start[i], L->len[i], R.ar, R.work2, R.work_n) < 0) {
        R.work2[0] = 0;
        R.err = 1;
    }
    return R.work2;
}

static int text_lines(const char *text, const St *st, int w)
{
    Lines L;
    wrap(text, st->font, st->ls, w, &L);
    return L.n;
}

static void new_page(void);

/* a paragraph in the column [x, x+w) starting at R.y; pages break between lines */
static void para(int x, int w, const char *text, const St *st, int align)
{
    Lines L;
    wrap(text, st->font, st->ls, w, &L);
    int b = base_in_line(st->font, st->pitch);
    for (int i = 0; i < L.n; i++) {
        if (R.y + st->pitch > BOTTOM) new_page();
        line_at(x, w, R.y + b, line_visual(&L, i), st->font, st->color, align, st->ls);
        R.y += st->pitch;
    }
}

/* the same, at a fixed place (inside boxes and table cells): returns the height used */
static int para_at(int x, int y, int w, const char *text, const St *st, int align)
{
    Lines L;
    wrap(text, st->font, st->ls, w, &L);
    int b = base_in_line(st->font, st->pitch);
    for (int i = 0; i < L.n; i++)
        line_at(x, w, y + i * st->pitch + b, line_visual(&L, i), st->font, st->color, align, st->ls);
    return L.n * st->pitch;
}

/* ---------- drawing a finished page with LVGL ---------- */

static void draw_op(lv_draw_ctx_t *dc, const Op *o)
{
    if (o->kind == OP_BOX) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = o->radius;
        d.bg_opa = o->fill == NONE ? LV_OPA_TRANSP : LV_OPA_COVER;
        d.bg_color = lv_color_hex(o->fill);
        d.border_width = o->border == NONE ? 0 : o->border_w;
        d.border_opa = LV_OPA_COVER;
        d.border_color = lv_color_hex(o->border);
        d.shadow_width = 0;
        d.outline_width = 0;
        lv_area_t a = {o->x1, o->y1, o->x2, o->y2};
        lv_draw_rect(dc, &d, &a);
        return;
    }
    /* text: letter by letter, left to right (the text is already in drawing order) */
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = o->font;
    d.color = lv_color_hex(o->fill);
    d.opa = LV_OPA_COVER;
    const lv_font_t *f = o->font;
    int top = o->y1 - (f->line_height - f->base_line);     /* LVGL places letters from the line top */
    int x = o->x1, prev_x = x, prev_w = 0;
    const char *p = R.txt + o->text;
    uint32_t c = rp_utf8_next(&p);
    while (c) {
        const char *q = p;
        uint32_t next = rp_utf8_next(&q);
        if (rp_is_mark(c)) {
            /* a vowel mark: centred over the letter just drawn */
            lv_font_glyph_dsc_t g;
            if (lv_font_get_glyph_dsc(f, &g, c, 0)) {
                lv_point_t pos = {(lv_coord_t)(prev_x + prev_w / 2 - g.ofs_x - g.box_w / 2), (lv_coord_t)top};
                lv_draw_letter(dc, &d, &pos, c);
            }
        } else {
            int gw = lv_font_get_glyph_width(f, c, next);
            if (c != ' ') {
                lv_point_t pos = {(lv_coord_t)x, (lv_coord_t)top};
                lv_draw_letter(dc, &d, &pos, c);
            }
            prev_x = x; prev_w = gw;
            x += gw + o->letter_space;
        }
        c = next; p = q;
    }
}

static void render_page(void)
{
    if (R.err) return;
    if (rp_pdf_page_begin(R.pdf) < 0) { R.err = 1; return; }

    /* a pretend display, only so LVGL's drawing code has somewhere to look
     * (the same trick LVGL's canvas widget uses); the real display is untouched */
    lv_disp_t fake;
    lv_disp_drv_t drv;
    lv_memset_00(&fake, sizeof fake);
    lv_disp_drv_init(&drv);
    drv.hor_res = PW;
    drv.ver_res = STRIP;
    fake.driver = &drv;
    lv_draw_ctx_t *dc = rp_malloc(sizeof(lv_draw_sw_ctx_t));
    if (!dc) { R.err = 1; return; }
    lv_draw_sw_init_ctx(&drv, dc);
    drv.draw_ctx = dc;
    lv_disp_t *was = _lv_refr_get_disp_refreshing();
    _lv_refr_set_disp_refreshing(&fake);

    for (int y0 = 0; y0 < PH && !R.err; y0 += STRIP) {
        int h = PH - y0 < STRIP ? PH - y0 : STRIP;
        lv_color_t white = lv_color_white();
        for (int i = 0; i < PW * h; i++) R.strip[i] = white;
        lv_area_t area = {0, (lv_coord_t)y0, PW - 1, (lv_coord_t)(y0 + h - 1)};
        dc->buf = R.strip;
        dc->buf_area = &area;
        dc->clip_area = &area;
        for (int i = 0; i < R.nops; i++) {
            const Op *o = &R.ops[i];
            int a, b;
            if (o->kind == OP_BOX) { a = o->y1; b = o->y2; }
            else { a = o->y1 - o->font->line_height; b = o->y1 + o->font->line_height / 2; }  /* room for tall letters */
            if (b < y0 || a > y0 + h - 1) continue;           /* not in this strip */
            draw_op(dc, o);
        }
        if (rp_pdf_rows(R.pdf, R.strip, h) < 0) R.err = 1;
    }

    _lv_refr_set_disp_refreshing(was);
    lv_draw_sw_deinit_ctx(&drv, dc);
    rp_free(dc);
    if (rp_pdf_page_end(R.pdf) < 0) R.err = 1;
    R.pages++;
}

/* ---------- the words of the report, English / Arabic ---------- */
enum {
    W_SCAN, W_TITLE_AR, W_SUB, W_SUB_NOADAPTER, W_VEHICLE, W_YEAR, W_VIN, W_ODO, W_MODULES, W_BATT_ON,
    W_BATT_RUN, W_BATT_OFF, W_UNKNOWN, W_KM, W_VOLT, W_MILEAGE, W_FAULTS, W_FAULTS_OLD, W_FAULT_NOTE,
    W_NO_FAULTS, W_COL_MOD, W_COL_CODE, W_COL_MEANS, W_COL_NOW, W_NOT_PRESENT, W_PENDING, W_ACTIVE,
    W_PERMANENT, W_OTHER, W_TODO, W_WISH, W_WISH_NOTE, W_COL_FEATURE, W_COL_INVOLVED, W_COL_OUTLOOK,
    W_O_VERIFY, W_O_COMMON, W_O_SAFETY, W_O_UNKNOWN, W_FOUND, W_NONE_FOUND, W_SCANNED,
    W_ENG_RUN, W_ENG_OFF, W_IGN_OFF, W_READONLY, W_DEMO, W_DEMO_NOTE, W_DEMO_SHORT, W_CAR,
    W_STORED, W_MIL, W_MIL_ON, W_MIL_OFF, W_LIVE, W_LIVE_NOTE, W_COL_VALUE, W_COL_READING, W_NOTES,
    W_COUNT
};
static const char *const WORDS[W_COUNT][2] = {
    [W_SCAN]        = {"VEHICLE SCAN", "فحص السيارة"},
    [W_TITLE_AR]    = {"", "تقرير فحص"},
    [W_SUB]         = {"Read-only scan with NZEUS Deck and an %s. Nothing on the car was changed.",
                       "فحص قراءة فقط باستخدام NZEUS Deck وجهاز %s. لم يتم تغيير أي شيء في السيارة."},
    [W_SUB_NOADAPTER] = {"Read-only scan with NZEUS Deck. Nothing on the car was changed.",
                       "فحص قراءة فقط باستخدام NZEUS Deck. لم يتم تغيير أي شيء في السيارة."},
    [W_VEHICLE]     = {"Vehicle", "السيارة"},
    [W_YEAR]        = {"Model year (from VIN)", "سنة الصنع (من رقم الشاصي)"},
    [W_VIN]         = {"VIN", "رقم الشاصي"},
    [W_ODO]         = {"Odometer", "عداد المسافة"},
    [W_MODULES]     = {"Modules found", "عدد الوحدات"},
    [W_BATT_ON]     = {"Battery (ignition on)", "البطارية (السويتش على ON)"},
    [W_BATT_RUN]    = {"Battery (engine running)", "البطارية (المحرك يعمل)"},
    [W_BATT_OFF]    = {"Battery", "البطارية"},
    [W_UNKNOWN]     = {"Unknown", "غير معروف"},
    [W_KM]          = {"%.0f km", "%.0f كم"},
    [W_VOLT]        = {"%.1f V", "%.1f فولت"},
    [W_MILEAGE]     = {"Mileage check", "فحص الممشى"},
    [W_FAULTS]      = {"Stored fault codes", "رسائل الأعطال المخزنة"},
    [W_FAULTS_OLD]  = {"Stored fault codes (history only)", "رسائل الأعطال المخزنة (قديمة فقط)"},
    [W_FAULT_NOTE]  = {"\"Previously set\" means the fault happened once in the past and is not there now.",
                       "\"مسجل سابقًا\" يعني أن العطل حدث مرة في الماضي وغير موجود الآن."},
    [W_NO_FAULTS]   = {"No fault codes were returned. Unread results remain unknown.", "لم تصل رسائل أعطال. تبقى النتائج التي لم تُقرأ غير معروفة."},
    [W_COL_MOD]     = {"Module", "الوحدة"},
    [W_COL_CODE]    = {"Code", "الكود"},
    [W_COL_MEANS]   = {"What it means", "المعنى"},
    [W_COL_NOW]     = {"Now", "الآن"},
    [W_NOT_PRESENT] = {"Not present", "غير موجود"},
    [W_PENDING]     = {"Pending", "بانتظار التأكيد"},
    [W_ACTIVE]      = {"Active", "موجود الآن"},
    [W_PERMANENT]   = {"Permanent", "دائم"},
    [W_OTHER]       = {"Other", "أخرى"},
    [W_TODO]        = {"What to do", "الخطوات المقترحة"},
    [W_WISH]        = {"Wishlist: first look", "قائمة المزايا المطلوبة: نظرة أولى"},
    [W_WISH_NOTE]   = {"Based only on which modules the car has. Each item still needs checking in the module "
                       "settings, which requires the FORScan Extended license.",
                       "مبنية فقط على الوحدات الموجودة في السيارة. كل ميزة تحتاج تأكيد من داخل إعدادات "
                       "الوحدات، وهذا يتطلب رخصة FORScan الموسعة."},
    [W_COL_FEATURE] = {"Feature", "الميزة"},
    [W_COL_INVOLVED] = {"Module involved", "الوحدة المعنية"},
    [W_COL_OUTLOOK] = {"Outlook", "التوقع"},
    [W_O_VERIFY]    = {"To verify", "يحتاج تأكيد"},
    [W_O_COMMON]    = {"Commonly possible", "ممكن عادة"},
    [W_O_SAFETY]    = {"Possible, safety feature", "ممكن، ميزة أمان"},
    [W_O_UNKNOWN]   = {"Unknown", "غير معروف"},
    [W_FOUND]       = {"Modules found (%d)", "الوحدات المكتشفة (%d)"},
    [W_NONE_FOUND]  = {"No module answered.", "لم تستجب أي وحدة."},
    [W_SCANNED]     = {"Scanned", "تم الفحص بتاريخ"},
    [W_ENG_RUN]     = {"Engine running", "المحرك يعمل"},
    [W_ENG_OFF]     = {"Engine off, ignition on", "المحرك مطفأ، السويتش على ON"},
    [W_IGN_OFF]     = {"Ignition off", "السويتش مطفأ"},
    [W_READONLY]    = {"Read only, no changes made.", "قراءة فقط، لم يتم تغيير أي شيء في السيارة."},
    [W_DEMO]        = {"DEMO DATA - not a real car", "بيانات تجريبية - ليست سيارة حقيقية"},
    [W_DEMO_NOTE]   = {"Everything in this report is made up, for trying out the app. Do not use it for a real car.",
                       "كل ما في هذا التقرير مختلق لتجربة البرنامج. لا تعتمد عليه لسيارة حقيقية."},
    [W_DEMO_SHORT]  = {"DEMO DATA", "بيانات تجريبية"},
    [W_CAR]         = {"Car", "سيارة"},
    [W_STORED]      = {"Stored", "مخزّن"},
    [W_MIL]         = {"Check-engine light and self-tests", "لمبة فحص المحرك والاختبارات الذاتية"},
    [W_MIL_ON]      = {"The check-engine light is ON.", "لمبة فحص المحرك مضاءة."},
    [W_MIL_OFF]     = {"The check-engine light is off.", "لمبة فحص المحرك مطفأة."},
    [W_LIVE]        = {"Live values at the scan", "القراءات المباشرة وقت الفحص"},
    [W_LIVE_NOTE]   = {"Read from the engine computer while the scan ran.", "قُرئت من كمبيوتر المحرك أثناء الفحص."},
    [W_COL_VALUE]   = {"Value", "القراءة"},
    [W_COL_READING] = {"At the scan", "وقت الفحص"},
    [W_NOTES]       = {"Notes", "ملاحظات"},
};
#define W(id) WORDS[id][R.ar]

static const char *const MONTH_EN[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const MONTH_AR[12] = {"يناير", "فبراير", "مارس", "أبريل", "مايو", "يونيو",
                                         "يوليو", "أغسطس", "سبتمبر", "أكتوبر", "نوفمبر", "ديسمبر"};

/* text fields of the scan, safely ended even if the scanner forgot the 0 */
static char fbuf[8][1024];
static int fnext;
static const char *fld(const char *f, size_t n)
{
    char *b = fbuf[fnext++ & 7];
    size_t k = 0;
    while (k < n && k < sizeof fbuf[0] - 1 && f[k]) k++;
    for (size_t i = 0; i < k; i++)                /* tabs, line breaks... -> spaces */
        b[i] = (unsigned char)f[i] < 0x20 ? ' ' : f[i];
    b[k] = 0;
    return b;
}
#define F(x)  fld((x), sizeof(x))
#define FL(e_, a_) (R.ar ? F(a_) : F(e_))       /* the field in the report's language */

/* ---------- styles, per language (Arabic letters need taller lines) ---------- */
typedef struct {
    int body_pitch, body_w;          /* normal paragraphs: line pitch, widest line */
    int sub_pitch;                   /* subtitle */
    int head_pitch;                  /* summary headline */
    int value_pitch;                 /* info grid values */
    int step_pitch, step_gap;        /* "What to do" */
    int row_h, row_base, row_pitch;  /* table data row: height, first baseline, extra lines */
    int thead_h, thead_base;         /* table header row */
    int group_h, group_base;         /* table group row */
    int mod_pitch;                   /* "Modules found" list */
    int foot_pitch;
    int mod_cols[4];                 /* fault table: column starts (+ end = CW) */
    int wish_cols[3];                /* wishlist table */
} Look;
static const Look LOOKS[2] = {
    { 37, 960, 36, 35, 39, 37, 8, 64, 37, 32, 58, 34, 61, 35, 43, 31,
      {0, 111, 217, 903}, {0, 381, 806} },
    { 43, 1010, 37, 40, 45, 44, 9, 70, 41, 36, 62, 37, 66, 38, 44, 33,
      {0, 131, 258, 905}, {0, 436, 828} },
};
#define LK (LOOKS[R.ar])

static St style(const lv_font_t *f, uint32_t color, int pitch) { St s = {f, color, pitch, 0}; return s; }

/* ---------- pages ---------- */

static void demo_strip(void);

static void new_page(void)
{
    render_page();
    R.nops = 0;
    R.tlen = 0;
    R.y = TOP;
    if (R.s->demo) demo_strip();
}

/* is there room for h more pixels? if not, start a new page */
static void need(int h)
{
    if (R.y + h > BOTTOM && R.y > TOP) new_page();
}

/* "DEMO DATA" on top of every page after the first */
static void demo_strip(void)
{
    St st = style(&rp_font_semi20, C_RED, 40);
    box(0, R.y, CW, 40, C_RED_BG, NONE, 0, 9);
    para_at(20, R.y, CW - 40, W(W_DEMO_SHORT), &st, AL_START);
    R.y += 40 + 24;
}

/* section title; keeps `after` pixels of the next part on the same page */
static void heading(const char *t, int gap_after, int after)
{
    St st = style(&rp_font_bold31, C_INK, 41);
    if (R.y > TOP) R.y += 40;
    need(41 + gap_after + after);
    para(0, CW, t, &st, AL_START);
    R.y += gap_after;
}

/* the scan time as calendar fields; a missing (0) or impossible time
 * becomes "now", so the report never shows a silly date */
static void scan_tm(struct tm *tm, int utc)
{
    time_t t = R.s->when > 0 ? R.s->when : time(NULL);
    struct tm *ok = utc ? gmtime_r(&t, tm) : localtime_r(&t, tm);
    if (!ok || tm->tm_year + 1900 < 1990 || tm->tm_year + 1900 > 9999) {
        t = time(NULL);
        if (!(utc ? gmtime_r(&t, tm) : localtime_r(&t, tm))) memset(tm, 0, sizeof *tm);
    }
}

/* ---------- 1. top of the report ---------- */

static void header(void)
{
    const CarScan *s = R.s;
    struct tm tm;
    scan_tm(&tm, 0);
    int mon = tm.tm_mon >= 0 && tm.tm_mon < 12 ? tm.tm_mon : 0;
    char t[600];

    /* small orange line: NZEUS LAB · VEHICLE SCAN · 29 SEP 2026 */
    if (R.ar) {
        snprintf(t, sizeof t, "NZEUS LAB · %s · %d %s %d", W(W_SCAN), tm.tm_mday, MONTH_AR[mon], tm.tm_year + 1900);
        St st = style(&rp_font_reg19, C_ORANGE, 24);
        R.y = TOP + 13;
        para(0, CW, t, &st, AL_START);
    } else {
        char m[4];
        for (int i = 0; i < 3; i++) m[i] = (char)(MONTH_EN[mon][i] & ~0x20);   /* SEP */
        m[3] = 0;
        snprintf(t, sizeof t, "NZEUS LAB · %s · %d %s %d", W(W_SCAN), tm.tm_mday, m, tm.tm_year + 1900);
        St st = {&rp_font_mono19, C_ORANGE, 24, 1};
        R.y = TOP + 13;
        para(0, CW, t, &st, AL_START);
    }

    /* DEMO banner: impossible to miss */
    if (s->demo) {
        St big = style(&rp_font_bold31, C_RED, 44), small = style(&rp_font_reg23, C_RED, 36);
        int h = 22 + 44 + text_lines(W(W_DEMO_NOTE), &small, CW - 56) * 36 + 22;
        R.y += 18;
        box(0, R.y, CW, h, C_RED_BG, C_RED, 3, 9);
        para_at(28, R.y + 22, CW - 56, W(W_DEMO), &big, AL_START);
        para_at(28, R.y + 22 + 44, CW - 56, W(W_DEMO_NOTE), &small, AL_START);
        R.y += h;
    }

    /* title: 2025 Ford Ranger Raptor */
    const char *mm = FL(s->make_model_en, s->make_model_ar);
    if (!mm[0]) mm = W(W_CAR);
    char year[16] = "";
    if (s->model_year > 0) snprintf(year, sizeof year, "%d", s->model_year);
    if (R.ar) snprintf(t, sizeof t, "%s %s%s%s", W(W_TITLE_AR), mm, year[0] ? " " : "", year);
    else snprintf(t, sizeof t, "%s%s%s", year, year[0] ? " " : "", mm);
    St title = style(&rp_font_bold44, C_INK, 52);
    R.y += 20;
    para(0, CW, t, &title, AL_START);

    /* subtitle */
    const char *ad = F(s->adapter);
    if (ad[0]) snprintf(t, sizeof t, W(W_SUB), ad);
    else snprintf(t, sizeof t, "%s", W(W_SUB_NOADAPTER));
    St sub = style(&rp_font_reg23, C_GREY, LK.sub_pitch);
    R.y += 17;
    para(0, LK.body_w - 40, t, &sub, AL_START);

    /* dark rule */
    R.y += 29;
    box(0, R.y, CW, 3, C_INK, NONE, 0, 0);
    R.y += 3;
}

/* worst state of the stored codes: 0 fine, 1 pending, 2 active */
static int worst_state(void)
{
    const CarScan *s = R.s;
    int n = s->dtc_count < 0 ? 0 : s->dtc_count > SCAN_MAX_DTCS ? SCAN_MAX_DTCS : s->dtc_count;
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (s->dtcs[i].state == DTC_ACTIVE || s->dtcs[i].state == DTC_PERMANENT) w = 2;
        else if ((s->dtcs[i].state == DTC_PENDING || s->dtcs[i].state == DTC_STORED) && w < 1) w = 1;
    }
    if (s->mil_on) w = 2;
    if (!s->healthy && w == 0) w = 1;
    return w;
}

static int count_dtcs(int state)
{
    const CarScan *s = R.s;
    int n = s->dtc_count < 0 ? 0 : s->dtc_count > SCAN_MAX_DTCS ? SCAN_MAX_DTCS : s->dtc_count, k = 0;
    for (int i = 0; i < n; i++)
        if (s->dtcs[i].state == state || (state == DTC_ACTIVE && s->dtcs[i].state == DTC_PERMANENT)) k++;
    return k;
}

/* ---------- 2. the coloured summary box ---------- */

static void summary(void)
{
    const CarScan *s = R.s;
    int w = worst_state();
    uint32_t bg = w == 2 ? C_RED_BG : w == 1 ? C_AMBER_BG : C_GREEN_BG;
    uint32_t fg = w == 2 ? C_RED : w == 1 ? C_AMBER : C_GREEN;
    char head[300], body[900];
    snprintf(head, sizeof head, "%s", FL(s->headline_en, s->headline_ar));
    snprintf(body, sizeof body, "%s", FL(s->summary_en, s->summary_ar));
    int act = count_dtcs(DTC_ACTIVE), pen = count_dtcs(DTC_PENDING), old = count_dtcs(DTC_HISTORY);
    if (!head[0]) {                      /* the scanner left it empty: say it plainly */
        if (w == 2) snprintf(head, sizeof head, R.ar ? "يحتاج انتباه. توجد %d رسائل أعطال حالية."
                                                     : "Needs attention. %d active fault codes.", act);
        else if (w == 1) snprintf(head, sizeof head, R.ar ? "يحتاج متابعة. توجد %d رسائل أعطال معلقة."
                                                          : "Check soon. %d pending fault codes.", pen);
        else snprintf(head, sizeof head, "%s", R.ar ? "السيارة سليمة. لا توجد أعطال حالية." : "Healthy. No active faults.");
    }
    if (!body[0])
        snprintf(body, sizeof body, R.ar ? "استجابت %d من %d وحدة. توجد %d رسائل أعطال مخزنة (%d حالية، %d معلقة، %d قديمة)."
                                         : "%d of %d modules answered. %d fault codes stored (%d active, %d pending, %d old).",
                 s->modules_answered, s->module_count, act + pen + old, act, pen, old);
    St hs = style(&rp_font_semi27, fg, LK.head_pitch), bs = style(&rp_font_reg23, C_INK, LK.body_pitch);
    int inner = CW - 54, bw = LK.body_w < inner ? LK.body_w : inner;
    int h = 27 + text_lines(head, &hs, inner) * hs.pitch + 13 + text_lines(body, &bs, bw) * bs.pitch + 27;
    R.y += 40;
    need(h);
    box(0, R.y, CW, h, bg, NONE, 0, 9);
    int y = R.y + 27;
    y += para_at(27, y, inner, head, &hs, AL_START) + 13;
    para_at(27, y, bw, body, &bs, AL_START);
    R.y += h;
}

/* ---------- 3. the info grid (2 rows x 3 cells) ---------- */

static void vin_masked(char *o, size_t n)
{
    const char *v = F(R.s->vin);
    size_t L = strlen(v);
    if (L == 17) {                       /* 6FP·········00001: first 3, 9 dots, last 5 */
        char mid[40] = "";
        for (int i = 0; i < 9; i++) strcat(mid, "·");
        snprintf(o, n, "%.3s%s%s", v, mid, v + 12);
    } else snprintf(o, n, "%s", L ? v : W(W_UNKNOWN));
}

static void info_grid(void)
{
    const CarScan *s = R.s;
    char veh[300], year[32], vin[64], odo[48], mods[16], bat[48];
    const char *mm = FL(s->make_model_en, s->make_model_ar), *eng = FL(s->engine_en, s->engine_ar);
    const char *mk = FL(s->make_en, s->make_ar);
    size_t mkn = strlen(mk);
    /* "Ford Ranger Raptor" -> "Ranger Raptor" (the title already says Ford);
     * just the make ("Toyota", "Land Rover") stays as it is */
    const char *model = mkn && !strncmp(mm, mk, mkn) && mm[mkn] == ' ' && mm[mkn + 1] ? mm + mkn + 1 : mm;
    if (model[0] && eng[0]) snprintf(veh, sizeof veh, "%s%s %s", model, R.ar ? "،" : ",", eng);
    else snprintf(veh, sizeof veh, "%s", model[0] ? model : eng[0] ? eng : W(W_UNKNOWN));
    if (s->model_year > 0) snprintf(year, sizeof year, "%d", s->model_year);
    else snprintf(year, sizeof year, "%s", W(W_UNKNOWN));
    vin_masked(vin, sizeof vin);
    if (s->odometer_km >= 0 && s->odometer_km < 1e8) snprintf(odo, sizeof odo, W(W_KM), s->odometer_km);
    else snprintf(odo, sizeof odo, "%s", W(W_UNKNOWN));
    snprintf(mods, sizeof mods, "%d", s->modules_answered < 0 ? 0 : s->modules_answered);
    if (s->battery_v > 0 && s->battery_v < 100) snprintf(bat, sizeof bat, W(W_VOLT), s->battery_v);
    else snprintf(bat, sizeof bat, "%s", W(W_UNKNOWN));
    const char *batl = s->engine_running ? W(W_BATT_RUN) : s->ignition_on ? W(W_BATT_ON) : W(W_BATT_OFF);

    const char *label[6] = {W(W_VEHICLE), W(W_YEAR), W(W_VIN), W(W_ODO), W(W_MODULES), batl};
    const char *value[6] = {veh, year, vin, odo, mods, bat};
    /* English: numbers in the mono font. Arabic: like the original, the pure
     * "computer" values (VIN, module count) are mono and read left to right */
    int mono[6] = {0, 0, 1, !R.ar, 1, !R.ar};
    St ls = style(&rp_font_reg19, C_GREY, 24);
    int cw = CW / 3, pad = 22, inner = cw - 2 * pad;
    int top = R.y + 41, rowh[2];
    for (int r = 0; r < 2; r++) {
        int lines = 1;
        for (int c = 0; c < 3; c++) {
            int i = r * 3 + c;
            St vs = style(mono[i] ? &rp_font_monomed25 : &rp_font_med25, C_INK, LK.value_pitch);
            int n = text_lines(value[i], &vs, inner);
            if (n > lines) lines = n;
        }
        rowh[r] = 20 + 24 + 6 + lines * LK.value_pitch + 19;
    }
    need(41 + rowh[0] + rowh[1]);
    top = R.y + 41;
    int y = top;
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            int i = r * 3 + c, x = c * cw;
            St vs = style(mono[i] ? &rp_font_monomed25 : &rp_font_med25, C_INK, LK.value_pitch);
            para_at(x + pad, y + 20, inner, label[i], &ls, AL_START);
            para_at(x + pad, y + 20 + 24 + 6, inner, value[i], &vs, (mono[i] && R.ar) ? AL_LEFT : AL_START);
        }
        if (r == 0) box(0, y + rowh[0] - 1, CW, 2, C_LINE, NONE, 0, 0);    /* line between the rows */
        y += rowh[r];
    }
    box(cw - 1, top, 2, y - top, C_LINE, NONE, 0, 0);                       /* lines between the cells */
    box(2 * cw - 1, top, 2, y - top, C_LINE, NONE, 0, 0);
    box(0, top, CW, y - top, NONE, C_LINE, 2, 9);                           /* rounded frame */
    R.y = y;
}

/* ---------- 3b. the check-engine light and the self-tests (when an engine computer said) ---------- */

static void engine_light(void)
{
    const CarScan *s = R.s;
    if (s->mil_known != 1) return;
    char t[900];
    const char *ready = FL(s->readiness_en, s->readiness_ar);
    /* "The check-engine light is off. 7 of 8 self-tests done; not yet: Fuel vapour (EVAP)." */
    snprintf(t, sizeof t, "%s%s%s%s", W(s->mil_on ? W_MIL_ON : W_MIL_OFF), ready[0] ? " " : "", ready, ready[0] ? "." : "");
    St st = style(&rp_font_reg23, s->mil_on ? C_RED : C_INK, LK.body_pitch);
    heading(W(W_MIL), 18, st.pitch * 2);
    para(0, LK.body_w, t, &st, AL_START);
}

/* ---------- 4. mileage check ---------- */

static const char *module_name(const char *code)
{
    const CarScan *s = R.s;
    int n = s->module_count < 0 ? 0 : s->module_count > SCAN_MAX_MODULES ? SCAN_MAX_MODULES : s->module_count;
    for (int i = 0; i < n; i++)
        if (!strcmp(F(s->modules[i].code), code)) {
            const char *nm = FL(s->modules[i].name_en, s->modules[i].name_ar);
            if (nm[0]) return nm;
        }
    return code;
}

static void mileage(void)
{
    const CarScan *s = R.s;
    static const char *const NUM_EN[] = {"", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten"};
    static const char *const NUM_AR[] = {"", "", "وحدتان منفصلتان", "ثلاث", "أربع", "خمس", "ست", "سبع", "ثماني", "تسع", "عشر"};
    int n0 = s->odo_count < 0 ? 0 : s->odo_count > SCAN_MAX_ODO ? SCAN_MAX_ODO : s->odo_count;
    ScanOdometer odo[SCAN_MAX_ODO];             /* only the entries that name a module */
    int n = 0;
    for (int i = 0; i < n0; i++)
        if (s->odo[i].module[0] && s->odo[i].km >= 0 && s->odo[i].km < 1e8) odo[n++] = s->odo[i];
    char names[600] = "", t[1200];
    for (int i = 0; i < n; i++) {
        char code[16];
        snprintf(code, sizeof code, "%s", F(odo[i].module));
        size_t L = strlen(names);
        snprintf(names + L, sizeof names - L, "%s%s", i ? (R.ar ? "، " : ", ") : "", module_name(code));
    }
    if (n >= 2 && s->odo_agree == 1) {
        double km = odo[0].km;
        if (R.ar) {
            if (n == 2) snprintf(t, sizeof t, "%s (%s) تسجلان نفس القراءة: %.1f km. القراءات متطابقة، لكن هذا لا يثبت أن العداد لم يُعدّل.", NUM_AR[2], names, km);
            else if (n <= 10) snprintf(t, sizeof t, "%s وحدات منفصلة (%s) تسجل نفس القراءة: %.1f km. القراءات متطابقة، لكن هذا لا يثبت أن العداد لم يُعدّل.", NUM_AR[n], names, km);
            else snprintf(t, sizeof t, "%d وحدة منفصلة (%s) تسجل نفس القراءة: %.1f km. القراءات متطابقة، لكن هذا لا يثبت أن العداد لم يُعدّل.", n, names, km);
        } else {
            char num[16];
            if (n <= 10) snprintf(num, sizeof num, "%s", NUM_EN[n]); else snprintf(num, sizeof num, "%d", n);
            snprintf(t, sizeof t, "%s separate modules (%s) each store %.1f km. These readings agree; this alone does not prove the mileage has never been changed.", num, names, km);
        }
    } else if (n >= 2) {
        char list[800] = "";
        for (int i = 0; i < n; i++) {
            char code[16];
            snprintf(code, sizeof code, "%s", F(odo[i].module));
            size_t L = strlen(list);
            snprintf(list + L, sizeof list - L, "%s%s %.1f km", i ? (R.ar ? "، " : ", ") : "", module_name(code), odo[i].km);
        }
        snprintf(t, sizeof t, R.ar ? "الوحدات تسجل قراءات مختلفة: %s. الاختلاف قد يعني تبديل وحدة أو تغيير العداد، ويستحق فحصًا أدق."
                                   : "The modules store different values: %s. A difference can mean a module was swapped or the odometer was changed; worth a closer look.", list);
    } else if (n == 1) {
        snprintf(t, sizeof t, R.ar ? "وحدة واحدة فقط (%s) سجلت قراءة العداد: %.1f km. لا يكفي للمقارنة."
                                   : "Only one module (%s) reported the odometer: %.1f km. Not enough to compare.", names, odo[0].km);
    } else {
        snprintf(t, sizeof t, "%s", R.ar ? "لم تسجل أي وحدة قراءة العداد، لذلك لم يمكن مقارنة الممشى."
                                         : "No module reported its stored odometer, so the mileage could not be cross-checked.");
    }
    St st = style(&rp_font_reg23, C_INK, LK.body_pitch);
    heading(W(W_MILEAGE), 18, st.pitch * 2);
    para(0, LK.body_w, t, &st, AL_START);
}

/* ---------- tables (fault codes, wishlist) ---------- */

typedef struct {
    int ncol;
    int x[4], w[4];                  /* columns, start-side x */
    const char *head[4];
    int seg_top;                     /* top of this page's part of the table */
    int open;
    int last_line;                   /* the op of the line under the last row (-1 = none) */
} Table;

static void table_head(Table *t)
{
    St hs = style(&rp_font_med19, C_GREY, 24);
    int h = LK.thead_h;
    t->seg_top = R.y;
    t->last_line = -1;
    /* grey header with round top corners: a rounded box + a square one over its lower half */
    box(0, R.y, CW, h, C_HEAD, NONE, 0, 9);
    box(0, R.y + h / 2, CW, h - h / 2, C_HEAD, NONE, 0, 0);
    for (int c = 0; c < t->ncol; c++) {
        Lines L;
        wrap(t->head[c], hs.font, 0, t->w[c] - 38, &L);      /* (joins the Arabic letters) */
        line_at(t->x[c] + 19, t->w[c] - 38, R.y + LK.thead_base, line_visual(&L, 0), hs.font, hs.color, AL_START, 0);
    }
    box(0, R.y + h - 2, CW, 2, C_LINE, NONE, 0, 0);
    R.y += h;
    t->open = 1;
}

static void table_close(Table *t)
{
    if (!t->open) return;
    /* the frame draws the bottom edge itself: drop the square line under the
     * last row, it would poke out of the round corners */
    if (t->last_line >= 0 && t->last_line < R.nops) R.ops[t->last_line].fill = NONE;
    box(0, t->seg_top, CW, R.y - t->seg_top, NONE, C_LINE, 2, 9);         /* rounded frame */
    t->open = 0;
}

/* make room for a row of height h (+ keep_next): else close this part and go on the next page */
static void table_room(Table *t, int h)
{
    if (R.y + h <= BOTTOM) return;
    table_close(t);
    new_page();
    table_head(t);
}

static void table_line_under(Table *t, int y)
{
    t->last_line = R.nops;
    box(0, y - 2, CW, 2, C_LINE, NONE, 0, 0);
}

static void table_group(Table *t, const char *text, int keep_next)
{
    St gs = style(&rp_font_semi20, C_INK, 30);
    int h = LK.group_h;
    table_room(t, h + keep_next);                  /* a group title never ends a page alone */
    box(0, R.y, CW, h, C_GROUP, NONE, 0, 0);
    Lines L;
    wrap(text, gs.font, 0, CW - 38, &L);
    line_at(19, CW - 38, R.y + LK.group_base, line_visual(&L, 0), gs.font, gs.color, AL_START, 0);
    R.y += h;
    table_line_under(t, R.y);
}

/* a small rounded label ("Not present", "Commonly possible") */
static void pill(int x, int y, int maxw, const char *text, uint32_t fg, uint32_t bg)
{
    const lv_font_t *f = &rp_font_med19;
    Lines L;
    wrap(text, f, 0, maxw - 24, &L);
    const char *v = line_visual(&L, 0);
    int tw = text_width(f, v, 0), w = tw + 24, h = 33;
    box(x, y, w, h, bg, NONE, 0, h / 2);                  /* round ends */
    line_at(x + 12, tw, y + 23, v, f, fg, AL_START, 0);
}

typedef struct { const char *text; const lv_font_t *font; } Cell;

/* a data row; the last column may be a pill instead of text */
static void table_row(Table *t, const Cell *cells, int ncells, const char *pill_text, uint32_t pill_fg, uint32_t pill_bg)
{
    int lines = 1;
    for (int c = 0; c < ncells; c++) {
        St cs = style(cells[c].font, C_INK, LK.row_pitch);
        int n = text_lines(cells[c].text, &cs, t->w[c] - 38);
        if (n > lines) lines = n;
    }
    int h = LK.row_h + (lines - 1) * LK.row_pitch;
    table_room(t, h);
    for (int c = 0; c < ncells; c++) {
        St cs = style(cells[c].font, C_INK, LK.row_pitch);
        int b = base_in_line(cs.font, cs.pitch);
        /* first baseline at row_base, like the original */
        para_at(t->x[c] + 19, R.y + LK.row_base - b, t->w[c] - 38, cells[c].text, &cs, AL_START);
    }
    if (pill_text) {
        int c = t->ncol - 1;
        pill(t->x[c] + 19, R.y + (LK.row_h - 33) / 2, t->w[c] - 30, pill_text, pill_fg, pill_bg);
    }
    R.y += h;
    table_line_under(t, R.y);
}

/* ---------- 5. stored fault codes ---------- */

static void faults(void)
{
    const CarScan *s = R.s;
    int n = s->dtc_count < 0 ? 0 : s->dtc_count > SCAN_MAX_DTCS ? SCAN_MAX_DTCS : s->dtc_count;
    int old_only = n > 0 && count_dtcs(DTC_HISTORY) == n;
    St note = style(&rp_font_reg23, C_GREY, LK.body_pitch);
    heading(old_only ? W(W_FAULTS_OLD) : W(W_FAULTS), 18, note.pitch + 21 + LK.thead_h + LK.group_h + LK.row_h);
    if (n == 0) {
        St st = style(&rp_font_reg23, C_INK, LK.body_pitch);
        para(0, LK.body_w, W(W_NO_FAULTS), &st, AL_START);
        return;
    }
    para(0, LK.body_w, W(W_FAULT_NOTE), &note, AL_START);
    R.y += 21;
    Table t = {4, {0}, {0}, {W(W_COL_MOD), W(W_COL_CODE), W(W_COL_MEANS), W(W_COL_NOW)}, 0, 0, -1};
    for (int c = 0; c < 4; c++) {
        t.x[c] = LK.mod_cols[c];
        t.w[c] = (c < 3 ? LK.mod_cols[c + 1] : CW) - t.x[c];
    }
    need(LK.thead_h + LK.group_h + LK.row_h);
    table_head(&t);
    /* groups in the order they first appear; codes keep their order inside a group */
    uint8_t done[SCAN_MAX_DTCS] = {0};
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        char group[200];
        snprintf(group, sizeof group, "%s", FL(s->dtcs[i].group_en, s->dtcs[i].group_ar));
        int first = 1;
        for (int j = i; j < n; j++) {
            if (done[j]) continue;
            if (strcmp(FL(s->dtcs[j].group_en, s->dtcs[j].group_ar), group)) continue;
            done[j] = 1;
            if (first) { table_group(&t, group[0] ? group : W(W_OTHER), LK.row_h); first = 0; }
            const ScanDtc *d = &s->dtcs[j];
            char mod[16], code[16], mean[400];
            snprintf(mod, sizeof mod, "%s", F(d->module));
            snprintf(code, sizeof code, "%s", F(d->code));
            snprintf(mean, sizeof mean, "%s", FL(d->meaning_en, d->meaning_ar));
            if (!mean[0]) snprintf(mean, sizeof mean, "%s", W(W_UNKNOWN));
            Cell cells[3] = {{mod, &rp_font_reg22}, {code, &rp_font_mono22}, {mean, &rp_font_reg22}};
            const char *pt; uint32_t fg, bg;
            switch (d->state) {
            case DTC_ACTIVE:    pt = W(W_ACTIVE);    fg = C_RED;   bg = C_RED_BG;   break;
            case DTC_PERMANENT: pt = W(W_PERMANENT); fg = C_RED;   bg = C_RED_BG;   break;
            case DTC_PENDING:   pt = W(W_PENDING);   fg = C_AMBER; bg = C_AMBER_BG; break;
            case DTC_STORED:    pt = W(W_STORED);    fg = C_AMBER; bg = C_AMBER_BG; break;
            default:            pt = W(W_NOT_PRESENT); fg = C_GREEN; bg = C_GREEN_BG; break;
            }
            table_row(&t, cells, 3, pt, fg, bg);
        }
    }
    table_close(&t);
}

/* ---------- 6. the cause box (orange bar on the start side) ---------- */

static void cause(void)
{
    const char *c = FL(R.s->cause_en, R.s->cause_ar);
    if (!c[0]) return;
    char t[700];
    snprintf(t, sizeof t, "%s", c);
    St st = style(&rp_font_reg23, C_INK, LK.body_pitch);
    int lines = text_lines(t, &st, LK.body_w - 24);
    R.y += 19;
    need(lines * st.pitch);
    box(0, R.y, 5, lines * st.pitch, C_ORANGE, NONE, 0, 0);
    para_at(24, R.y, LK.body_w - 24, t, &st, AL_START);
    R.y += lines * st.pitch;
}

/* ---------- 7. what to do ---------- */

static void steps(void)
{
    const CarScan *s = R.s;
    int n = s->step_count < 0 ? 0 : s->step_count > SCAN_MAX_STEPS ? SCAN_MAX_STEPS : s->step_count;
    if (!n) return;
    St st = style(&rp_font_reg23, C_INK, LK.step_pitch);
    heading(W(W_TODO), 0, st.pitch * 2);
    for (int i = 0; i < n; i++) {
        char t[400], num[8];
        snprintf(t, sizeof t, "%s", FL(s->steps_en[i], s->steps_ar[i]));
        snprintf(num, sizeof num, "%d.", i + 1);
        int lines = text_lines(t, &st, CW - 34);
        if (i) R.y += LK.step_gap;
        need(lines * st.pitch > 3 * st.pitch ? 2 * st.pitch : lines * st.pitch);   /* keep short steps whole */
        line_at(8, 30, R.y + base_in_line(st.font, st.pitch), num, st.font, C_INK, AL_START, 0);
        para(34, CW - 34, t, &st, AL_START);
    }
}

/* ---------- 7b. live values at the time of the scan ---------- */

static void live_values(void)
{
    const CarScan *s = R.s;
    int n = s->live_count < 0 ? 0 : s->live_count > SCAN_MAX_LIVE ? SCAN_MAX_LIVE : s->live_count;
    if (!n) return;
    St note = style(&rp_font_reg23, C_GREY, LK.body_pitch);
    heading(W(W_LIVE), 18, note.pitch + 20 + LK.thead_h + LK.row_h);
    para(0, LK.body_w, W(W_LIVE_NOTE), &note, AL_START);
    R.y += 20;
    /* two columns: what it is, and its value (a number + unit: left to right in both languages) */
    Table t = {2, {0, 620}, {620, CW - 620}, {W(W_COL_VALUE), W(W_COL_READING), NULL, NULL}, 0, 0, -1};
    need(LK.thead_h + LK.row_h);
    table_head(&t);
    for (int i = 0; i < n; i++) {
        char name[200], val[48];
        snprintf(name, sizeof name, "%s", FL(s->live[i].name_en, s->live[i].name_ar));
        if (!name[0]) snprintf(name, sizeof name, "%s", F(s->live[i].name_en));
        snprintf(val, sizeof val, "%s", F(s->live[i].value));
        Cell cells[2] = {{name, &rp_font_reg22}, {val, &rp_font_mono22}};
        table_row(&t, cells, 2, NULL, 0, 0);
    }
    table_close(&t);
}

/* ---------- 8. wishlist ---------- */

static void wishlist(void)
{
    const CarScan *s = R.s;
    int n = s->wish_count < 0 ? 0 : s->wish_count > SCAN_MAX_WISH ? SCAN_MAX_WISH : s->wish_count;
    if (!n) return;
    St note = style(&rp_font_reg23, C_GREY, LK.body_pitch);
    heading(W(W_WISH), 18, note.pitch * 2 + 20 + LK.thead_h + LK.row_h);
    para(0, LK.body_w, W(W_WISH_NOTE), &note, AL_START);
    R.y += 20;
    Table t = {3, {0}, {0}, {W(W_COL_FEATURE), W(W_COL_INVOLVED), W(W_COL_OUTLOOK), NULL}, 0, 0, -1};
    for (int c = 0; c < 3; c++) {
        t.x[c] = LK.wish_cols[c];
        t.w[c] = (c < 2 ? LK.wish_cols[c + 1] : CW) - t.x[c];
    }
    need(LK.thead_h + LK.row_h);
    table_head(&t);
    for (int i = 0; i < n; i++) {
        const ScanWish *w = &s->wish[i];
        char fe[200], mo[200];
        snprintf(fe, sizeof fe, "%s", FL(w->feature_en, w->feature_ar));
        snprintf(mo, sizeof mo, "%s", FL(w->module_en, w->module_ar));
        Cell cells[2] = {{fe, &rp_font_reg22}, {mo, &rp_font_reg22}};
        const char *pt; uint32_t fg, bg;
        switch (w->outlook) {
        case 1:  pt = W(W_O_COMMON);  fg = C_GREEN; bg = C_GREEN_BG; break;
        case 2:  pt = W(W_O_SAFETY);  fg = C_AMBER; bg = C_AMBER_BG; break;
        case 0:  pt = W(W_O_VERIFY);  fg = C_GREY;  bg = C_HEAD;     break;
        default: pt = W(W_O_UNKNOWN); fg = C_GREY;  bg = C_HEAD;     break;
        }
        table_row(&t, cells, 2, pt, fg, bg);
    }
    table_close(&t);
}

/* ---------- 9. modules found (two columns, orange codes) ---------- */

static void modules(void)
{
    const CarScan *s = R.s;
    int n = s->module_count < 0 ? 0 : s->module_count > SCAN_MAX_MODULES ? SCAN_MAX_MODULES : s->module_count;
    int idx[SCAN_MAX_MODULES], k = 0;
    for (int i = 0; i < n; i++) if (s->modules[i].answered) idx[k++] = i;
    char t[64];
    snprintf(t, sizeof t, W(W_FOUND), k);
    St cs = style(&rp_font_mono22, C_ORANGE, LK.mod_pitch), ns = style(&rp_font_reg22, C_INK, LK.mod_pitch);
    heading(t, 12, cs.pitch * 2);
    if (!k) { para(0, LK.body_w, W(W_NONE_FOUND), &ns, AL_START); return; }
    const int cx[2] = {0, 551}, nx = 115, nw = 551 - 115 - 20;
    for (int r = 0; r < (k + 1) / 2; r++) {
        int lines = 1;
        for (int c = 0; c < 2 && r * 2 + c < k; c++) {
            const ScanModule *m = &s->modules[idx[r * 2 + c]];
            int nl = text_lines(FL(m->name_en, m->name_ar), &ns, nw);
            if (nl > lines) lines = nl;
        }
        need(lines * ns.pitch);
        for (int c = 0; c < 2 && r * 2 + c < k; c++) {
            const ScanModule *m = &s->modules[idx[r * 2 + c]];
            char code[16], name[200];
            snprintf(code, sizeof code, "%s", F(m->code));
            snprintf(name, sizeof name, "%s", FL(m->name_en, m->name_ar));
            para_at(cx[c], R.y, nx - 8, code, &cs, AL_START);
            para_at(cx[c] + nx, R.y, nw, name, &ns, AL_START);
        }
        R.y += lines * ns.pitch;
    }
}

/* ---------- 9b. notes: how the scan went, what it could not reach ---------- */

static void notes(void)
{
    const CarScan *s = R.s;
    int n = s->note_count < 0 ? 0 : s->note_count > SCAN_MAX_NOTES ? SCAN_MAX_NOTES : s->note_count;
    if (!n) return;
    St st = style(&rp_font_reg23, C_GREY, LK.body_pitch);
    heading(W(W_NOTES), 12, st.pitch * 2);
    for (int i = 0; i < n; i++) {
        char t[700];
        snprintf(t, sizeof t, "%s", FL(s->notes_en[i], s->notes_ar[i]));
        if (!t[0]) continue;
        if (i) R.y += 8;
        need(st.pitch * 2);
        line_at(8, 20, R.y + base_in_line(st.font, st.pitch), "•", st.font, C_ORANGE, AL_START, 0);
        para(34, CW - 34, t, &st, AL_START);
    }
}

/* ---------- 10. footer ---------- */

static void footer(void)
{
    const CarScan *s = R.s;
    struct tm tm, g;
    scan_tm(&tm, 0);
    scan_tm(&g, 1);
    /* how far the console clock is from UTC (Settings > Date & time) */
    int days = tm.tm_year != g.tm_year ? (tm.tm_year > g.tm_year ? 1 : -1) : tm.tm_yday - g.tm_yday;
    int ofs = days * 1440 + (tm.tm_hour - g.tm_hour) * 60 + (tm.tm_min - g.tm_min);
    char zone[48];
    if (ofs % 60) snprintf(zone, sizeof zone, "(UTC%+d:%02d)", ofs / 60, abs(ofs % 60));
    else snprintf(zone, sizeof zone, "(UTC%+d)", ofs / 60);
    int mon = tm.tm_mon >= 0 && tm.tm_mon < 12 ? tm.tm_mon : 0;
    const char *eng = s->engine_running ? W(W_ENG_RUN) : s->ignition_on ? W(W_ENG_OFF) : W(W_IGN_OFF);
    char t[800], tool[120];
    const char *ad = F(s->adapter), *pr = F(s->protocol);
    snprintf(tool, sizeof tool, "NZEUS Deck%s%s%s%s", ad[0] ? " · " : "", ad, pr[0] ? " · " : "", pr);
    snprintf(t, sizeof t, "%s %d %s %d%s %02d:%02d %s · %s · %s · %s", W(W_SCANNED), tm.tm_mday,
             R.ar ? MONTH_AR[mon] : MONTH_EN[mon], tm.tm_year + 1900, R.ar ? "،" : ",",
             tm.tm_hour, tm.tm_min, zone, tool, eng, W(W_READONLY));
    St st = style(&rp_font_reg19, C_GREY, LK.foot_pitch);
    int h = 37 + 2 + 20 + text_lines(t, &st, CW) * st.pitch;
    need(h);
    R.y += 37;
    box(0, R.y, CW, 2, C_FOOT, NONE, 0, 0);
    R.y += 2 + 20;
    para(0, CW, t, &st, AL_START);
}

/* ---------- the file name and folder ---------- */

static int make_dirs(const char *path)
{
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        if (mkdir(p, 0755) < 0 && errno != EEXIST) return -1;
        *c = '/';
    }
    if (mkdir(p, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int pick_name(const CarScan *s, const char *lang_tag, const char *folder, char *out, int out_n)
{
    struct tm tm;
    time_t when = s->when > 0 ? s->when : time(NULL);
    if (!localtime_r(&when, &tm) || tm.tm_year + 1900 < 1990 || tm.tm_year + 1900 > 9999) {
        when = time(NULL);                       /* (same rule as scan_tm) */
        if (!localtime_r(&when, &tm)) memset(&tm, 0, sizeof tm);
    }
    char base[64];
    snprintf(base, sizeof base, "Health-Scan-%04d-%02d-%02d-%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, lang_tag);
    for (int k = 1; k < 1000; k++) {
        int n = k == 1 ? snprintf(out, (size_t)out_n, "%s/%s.pdf", folder, base)
                       : snprintf(out, (size_t)out_n, "%s/%s-%d.pdf", folder, base, k);
        if (n < 0 || n >= out_n) return -1;      /* the name does not fit */
        struct stat st;
        if (stat(out, &st) != 0) return 0;      /* free name */
    }
    return -1;
}

/* ---------- the whole report ---------- */

int carreport_pdf(const CarScan *s, const char *lang, const char *folder, char *out_path, int out_n)
{
    if (out_path && out_n > 0) out_path[0] = 0;
    if (!s || !folder || !folder[0] || !out_path || out_n < 16) return -1;
    int ar = lang && (lang[0] == 'a' || lang[0] == 'A');
    char path[512];
    if (make_dirs(folder) < 0) return -1;
    if (pick_name(s, ar ? "AR" : "EN", folder, path, sizeof path) < 0) return -1;
    if ((int)strlen(path) >= out_n) return -1;

    memset(&R, 0, sizeof R);
#ifdef NZ_TESTING
    test_len = 0;
    test_text[0] = 0;
#endif
    mem_peak = mem_now;
    R.s = s;
    R.ar = ar;
    R.strip = rp_malloc(sizeof(lv_color_t) * PW * STRIP);
    R.pdf = rp_pdf_open(path, PW, PH, DPI);
    if (!R.strip || !R.pdf) {
        if (R.pdf) rp_pdf_abort(R.pdf);
        rp_free(R.strip);
        return -1;
    }
    R.y = TOP;

    header();
    summary();
    info_grid();
    engine_light();
    mileage();
    faults();
    cause();
    steps();
    live_values();
    wishlist();
    modules();
    notes();
    footer();
    render_page();                       /* the last page */

    int ok = !R.err;
    char title[96];
    snprintf(title, sizeof title, "Health Scan %s", strrchr(path, '/') ? strrchr(path, '/') + 13 : path);
    if (ok) { if (rp_pdf_close(R.pdf, title) < 0) ok = 0; }
    else rp_pdf_abort(R.pdf);
    last_pages = R.pages;
    rp_free(R.strip); rp_free(R.ops); rp_free(R.txt); rp_free(R.work); rp_free(R.work2);
    memset(&R, 0, sizeof R);
    if (!ok) return -1;
    snprintf(out_path, (size_t)out_n, "%s", path);
    return 0;
}

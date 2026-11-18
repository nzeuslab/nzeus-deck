/* carreport_pdf.c - writes the report as a PDF file: one picture per page.
 *
 * A PDF is a text-like file with numbered "objects" (the catalog, the page
 * list, each page, each picture) and, at the end, a table of where each
 * object starts in the file (the "xref"). Here every page is one picture of
 * 1240 x 1754 pixels (A4 at 150 dots per inch), so Arabic always looks right
 * in every PDF reader - no fonts inside the PDF.
 *
 * The pictures are squeezed with "deflate" (the same method as ZIP and PNG),
 * which PDF readers unpack themselves (/FlateDecode). The squeezer below is a
 * small one written for this: it works on a stream of rows, so a whole page
 * never has to be in memory at once (the page arrives in strips).
 *   - "PNG predictor": each row is stored as its difference to the row above
 *     (or to the pixel on the left). A white page becomes almost all zeros.
 *   - LZ77: "the next 200 bytes are the same as 3720 bytes ago" is stored as
 *     a short (length, distance) pair instead of the bytes.
 *   - Huffman: common symbols get short bit codes, rare ones long codes. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "carreport_int.h"

/* ================= deflate (RFC 1951) inside a zlib wrapper (RFC 1950) ================= */

#define WSIZE    32768            /* how far back a repeat may be (deflate's limit) */
#define BUFSZ    (WSIZE * 2)      /* sliding window: old half + new half */
#define HBITS    15
#define HSIZE    (1 << HBITS)
#define MINM     3                /* shortest / longest repeat deflate can store */
#define MAXM     258
#define NSYM     16384            /* symbols per Huffman block */
#define CHAIN    24               /* how many earlier places to try (speed vs size) */

typedef struct {
    FILE *f;
    long out_bytes;               /* bytes written to the file (for the stream /Length) */
    uint8_t *win;                 /* window: [0, filled) holds data, `pos` = next byte to squeeze */
    int filled, pos;
    int32_t *head, *prev;         /* hash chains: places where 3 bytes looked the same */
    uint16_t *sym_len;            /* the symbols of the current block: literal byte, or */
    uint16_t *sym_dist;           /*   (length, distance) when dist > 0 */
    int nsym;
    uint32_t bits; int nbits;     /* bits waiting to be written */
    uint32_t adler_a, adler_b;    /* checksum of the raw data (zlib needs it) */
    int err;
} Zip;

static void put_byte(Zip *z, int b)
{
    if (fputc(b, z->f) == EOF) z->err = 1;
    z->out_bytes++;
}

static void put_bits(Zip *z, uint32_t v, int n)      /* deflate writes bits lowest first */
{
    z->bits |= v << z->nbits;
    z->nbits += n;
    while (z->nbits >= 8) { put_byte(z, (int)(z->bits & 0xFF)); z->bits >>= 8; z->nbits -= 8; }
}

/* length 3..258 -> code 257..285 + extra bits; distance 1..32768 -> code 0..29 + extra */
static const uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int len_code(int len)  { int i = 28; while (LEN_BASE[i] > len) i--; return i; }
static int dist_code(int d)   { int i = 29; while (DIST_BASE[i] > d) i--; return i; }

/* Huffman code lengths for `n` symbols with counts `freq`, no code longer than
 * `maxbits`. Plain Huffman (always join the two rarest); if a code gets too
 * long, halve the counts and try again. */
static void huff_lengths(const uint32_t *freq_in, int n, int maxbits, uint8_t *len)
{
    uint32_t freq[320];
    int parent[640];
    uint32_t w[640];
    for (int i = 0; i < n; i++) freq[i] = freq_in[i];
    for (;;) {
        int nodes = 0, used = 0;
        int alive[640];
        for (int i = 0; i < n; i++) {
            len[i] = 0;
            if (freq[i]) { w[i] = freq[i]; alive[nodes++] = i; used++; }
        }
        if (used == 0) return;
        if (used == 1) { len[alive[0]] = 1; return; }
        int next = n;
        while (nodes > 1) {                               /* join the two rarest */
            int a = 0, b = 1;
            if (w[alive[b]] < w[alive[a]]) { a = 1; b = 0; }
            for (int i = 2; i < nodes; i++) {
                if (w[alive[i]] < w[alive[a]]) { b = a; a = i; }
                else if (w[alive[i]] < w[alive[b]]) b = i;
            }
            int na = alive[a], nb = alive[b];
            w[next] = w[na] + w[nb];
            parent[na] = next; parent[nb] = next;
            alive[a] = next;                              /* the new node takes a's place */
            alive[b] = alive[--nodes];                    /* b's place gets the last one */
            next++;
        }
        int root = next - 1, maxl = 0;
        for (int i = 0; i < n; i++) {
            if (!freq[i]) continue;
            int l = 0;
            for (int k = i; k != root; k = parent[k]) l++;
            len[i] = (uint8_t)l;
            if (l > maxl) maxl = l;
        }
        if (maxl <= maxbits) return;
        for (int i = 0; i < n; i++) if (freq[i]) freq[i] = (freq[i] >> 1) | 1;
    }
}

/* code lengths -> the actual bit codes (reversed, because deflate is lowest-bit-first) */
static void huff_codes(const uint8_t *len, int n, uint16_t *code)
{
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    int c = 0;
    for (int b = 1; b < 16; b++) { c = (c + count[b - 1]) << 1; next[b] = c; }
    for (int i = 0; i < n; i++) {
        if (!len[i]) continue;
        int v = next[len[i]]++, r = 0;
        for (int k = 0; k < len[i]; k++) { r = (r << 1) | (v & 1); v >>= 1; }
        code[i] = (uint16_t)r;
    }
}

/* writes the collected symbols as one deflate block with its own Huffman codes */
static void flush_block(Zip *z, int last)
{
    uint32_t lf[286] = {0}, df[30] = {0};
    for (int i = 0; i < z->nsym; i++) {
        if (z->sym_dist[i]) { lf[257 + len_code(z->sym_len[i])]++; df[dist_code(z->sym_dist[i])]++; }
        else lf[z->sym_len[i]]++;
    }
    lf[256] = 1;                                         /* end of block */
    int nd = 0;
    for (int i = 0; i < 30; i++) nd += df[i] != 0;
    if (nd < 2) { if (!df[0]) df[0] = 1; else df[1] = 1; }   /* readers want 2+ distance codes */
    uint8_t ll[286], dl[30];
    uint16_t lc[286], dc[30];
    huff_lengths(lf, 286, 15, ll);
    huff_lengths(df, 30, 15, dl);
    huff_codes(ll, 286, lc);
    huff_codes(dl, 30, dc);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    while (hdist > 1 && !dl[hdist - 1]) hdist--;

    /* the code lengths themselves are stored run-length coded (16 = repeat, 17/18 = zeros) */
    uint8_t all[316], rl[316], rx[316];
    int na = 0, nr = 0;
    for (int i = 0; i < hlit; i++) all[na++] = ll[i];
    for (int i = 0; i < hdist; i++) all[na++] = dl[i];
    for (int i = 0; i < na;) {
        int j = i;
        while (j < na && all[j] == all[i]) j++;
        int run = j - i;
        if (all[i] == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r >= 11) { rl[nr] = 18; rx[nr++] = (uint8_t)(r - 11); }
            else { rl[nr] = 17; rx[nr++] = (uint8_t)(r - 3); }
            i += r;
        } else if (all[i] != 0 && run >= 4) {
            rl[nr] = all[i]; rx[nr++] = 0;
            int r = run - 1 > 6 ? 6 : run - 1;
            rl[nr] = 16; rx[nr++] = (uint8_t)(r - 3);
            i += 1 + r;
        } else {
            rl[nr] = all[i]; rx[nr++] = 0;
            i++;
        }
    }
    uint32_t cf[19] = {0};
    for (int i = 0; i < nr; i++) cf[rl[i]]++;
    uint8_t cl[19];
    uint16_t cc[19];
    huff_lengths(cf, 19, 7, cl);
    huff_codes(cl, 19, cc);
    static const uint8_t ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hclen = 19;
    while (hclen > 4 && !cl[ORDER[hclen - 1]]) hclen--;

    put_bits(z, last ? 1 : 0, 1);
    put_bits(z, 2, 2);                                   /* block type 2: our own Huffman codes */
    put_bits(z, (uint32_t)(hlit - 257), 5);
    put_bits(z, (uint32_t)(hdist - 1), 5);
    put_bits(z, (uint32_t)(hclen - 4), 4);
    for (int i = 0; i < hclen; i++) put_bits(z, cl[ORDER[i]], 3);
    for (int i = 0; i < nr; i++) {
        put_bits(z, cc[rl[i]], cl[rl[i]]);
        if (rl[i] == 16) put_bits(z, rx[i], 2);
        else if (rl[i] == 17) put_bits(z, rx[i], 3);
        else if (rl[i] == 18) put_bits(z, rx[i], 7);
    }
    for (int i = 0; i < z->nsym; i++) {
        int d = z->sym_dist[i];
        if (!d) { put_bits(z, lc[z->sym_len[i]], ll[z->sym_len[i]]); continue; }
        int len = z->sym_len[i], k = len_code(len), q = dist_code(d);
        put_bits(z, lc[257 + k], ll[257 + k]);
        if (LEN_EXTRA[k]) put_bits(z, (uint32_t)(len - LEN_BASE[k]), LEN_EXTRA[k]);
        put_bits(z, dc[q], dl[q]);
        if (DIST_EXTRA[q]) put_bits(z, (uint32_t)(d - DIST_BASE[q]), DIST_EXTRA[q]);
    }
    put_bits(z, lc[256], ll[256]);
    z->nsym = 0;
}

static void add_sym(Zip *z, int len_or_byte, int dist)
{
    z->sym_len[z->nsym] = (uint16_t)len_or_byte;
    z->sym_dist[z->nsym] = (uint16_t)dist;
    if (++z->nsym == NSYM) flush_block(z, 0);
}

static uint32_t hash3(const uint8_t *p) { return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (HSIZE - 1); }

static void insert(Zip *z, int at)
{
    uint32_t h = hash3(z->win + at);
    z->prev[at & (WSIZE - 1)] = z->head[h];
    z->head[h] = at;
}

/* squeeze what is in the window; keep MAXM bytes of look-ahead unless `final` */
static void squeeze(Zip *z, int final)
{
    int limit = final ? z->filled : z->filled - MAXM;
    while (z->pos < limit) {
        int best = 0, bestd = 0;
        int avail = z->filled - z->pos;
        if (avail >= MINM) {
            int cand = z->head[hash3(z->win + z->pos)];
            int maxlen = avail < MAXM ? avail : MAXM;
            for (int tries = 0; cand >= 0 && z->pos - cand <= WSIZE - 1 && tries < CHAIN; tries++) {
                const uint8_t *a = z->win + cand, *b = z->win + z->pos;
                if (a[best] == b[best]) {                 /* quick check before counting */
                    int l = 0;
                    while (l < maxlen && a[l] == b[l]) l++;
                    if (l > best) { best = l; bestd = z->pos - cand; if (l == maxlen) break; }
                }
                cand = z->prev[cand & (WSIZE - 1)];
            }
        }
        if (best >= MINM) {
            add_sym(z, best, bestd);
            /* remember the places inside short repeats too (long ones: skip, it's slow) */
            int stop = z->pos + best;
            if (best <= 32) { for (int k = z->pos; k < stop && k + MINM <= z->filled; k++) insert(z, k); }
            else if (z->pos + MINM <= z->filled) insert(z, z->pos);
            z->pos = stop;
        } else {
            add_sym(z, z->win[z->pos], 0);
            if (z->pos + MINM <= z->filled) insert(z, z->pos);
            z->pos++;
        }
    }
}

/* move the window: forget the oldest half */
static void slide(Zip *z)
{
    memmove(z->win, z->win + WSIZE, (size_t)(z->filled - WSIZE));
    z->filled -= WSIZE;
    z->pos -= WSIZE;
    for (int i = 0; i < HSIZE; i++) z->head[i] = z->head[i] >= WSIZE ? z->head[i] - WSIZE : -1;
    for (int i = 0; i < WSIZE; i++) z->prev[i] = z->prev[i] >= WSIZE ? z->prev[i] - WSIZE : -1;
}

static int zip_start(Zip *z, FILE *f)
{
    memset(z, 0, sizeof *z);
    z->f = f;
    z->win = rp_malloc(BUFSZ);
    z->head = rp_malloc(sizeof(int32_t) * HSIZE);
    z->prev = rp_malloc(sizeof(int32_t) * WSIZE);
    z->sym_len = rp_malloc(sizeof(uint16_t) * NSYM);
    z->sym_dist = rp_malloc(sizeof(uint16_t) * NSYM);
    if (!z->win || !z->head || !z->prev || !z->sym_len || !z->sym_dist) return -1;
    for (int i = 0; i < HSIZE; i++) z->head[i] = -1;
    for (int i = 0; i < WSIZE; i++) z->prev[i] = -1;
    z->adler_a = 1;
    put_byte(z, 0x78); put_byte(z, 0x9C);                /* zlib header: deflate, 32K window */
    return 0;
}

static void zip_write(Zip *z, const uint8_t *data, int n)
{
    /* checksum (Adler-32), in pieces so the sums never overflow */
    for (int i = 0; i < n;) {
        int k = n - i < 4096 ? n - i : 4096;
        for (int j = 0; j < k; j++) { z->adler_a += data[i + j]; z->adler_b += z->adler_a; }
        z->adler_a %= 65521; z->adler_b %= 65521;
        i += k;
    }
    while (n > 0) {
        if (z->filled == BUFSZ) {
            squeeze(z, 0);
            if (z->pos >= WSIZE + MAXM || z->pos > BUFSZ - MAXM) slide(z);
        }
        int room = BUFSZ - z->filled;
        int k = n < room ? n : room;
        memcpy(z->win + z->filled, data, (size_t)k);
        z->filled += k; data += k; n -= k;
    }
}

static void zip_finish(Zip *z)
{
    squeeze(z, 1);
    flush_block(z, 1);
    if (z->nbits) put_bits(z, 0, 8 - z->nbits);          /* fill up the last byte */
    uint32_t ad = (z->adler_b << 16) | z->adler_a;
    put_byte(z, (int)(ad >> 24)); put_byte(z, (int)((ad >> 16) & 0xFF));
    put_byte(z, (int)((ad >> 8) & 0xFF)); put_byte(z, (int)(ad & 0xFF));
}

static void zip_free(Zip *z)
{
    rp_free(z->win); rp_free(z->head); rp_free(z->prev); rp_free(z->sym_len); rp_free(z->sym_dist);
    z->win = NULL; z->head = z->prev = NULL; z->sym_len = z->sym_dist = NULL;
}

/* ================= the PDF file ================= */

struct RpPdf {
    FILE *f;
    char path[512], tmp[520];
    int w, h, dpi;
    long *ofs;                     /* where each object starts in the file */
    int nobj, cap;
    int *pages;                    /* object number of each page */
    int npages, pcap;
    Zip z;
    int img_obj, len_obj, row_y;
    long stream_start;
    uint8_t *row, *prev_row, *cand; /* one row of RGB bytes (+1 predictor byte) */
};

static int new_obj(RpPdf *p)                           /* reserve the next object number */
{
    if (p->nobj + 1 >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        long *n = rp_realloc(p->ofs, sizeof(long) * (size_t)nc);
        if (!n) return -1;
        p->ofs = n; p->cap = nc;
    }
    p->nobj++;
    p->ofs[p->nobj] = 0;
    return p->nobj;
}

static void begin_obj(RpPdf *p, int num)
{
    p->ofs[num] = ftell(p->f);
    fprintf(p->f, "%d 0 obj\n", num);
}

RpPdf *rp_pdf_open(const char *path, int w_px, int h_px, int dpi)
{
    RpPdf *p = rp_malloc(sizeof *p);
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%s", path);
    snprintf(p->tmp, sizeof p->tmp, "%s.part", path);    /* the real name only when complete */
    p->w = w_px; p->h = h_px; p->dpi = dpi;
    p->row = rp_malloc((size_t)w_px * 3 + 1);
    p->prev_row = rp_malloc((size_t)w_px * 3 + 1);
    p->cand = rp_malloc((size_t)w_px * 3 + 1);
    p->f = fopen(p->tmp, "wb");
    if (!p->f || !p->row || !p->prev_row || !p->cand) { rp_pdf_abort(p); return NULL; }
    /* objects 1 = catalog, 2 = page list, 3 = info: written at the end */
    for (int i = 0; i < 3; i++) if (new_obj(p) < 0) { rp_pdf_abort(p); return NULL; }
    fputs("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n", p->f);         /* the odd bytes say "binary file" */
    return p;
}

int rp_pdf_page_begin(RpPdf *p)
{
    p->img_obj = new_obj(p);
    p->len_obj = new_obj(p);
    if (p->img_obj < 0 || p->len_obj < 0) return -1;
    begin_obj(p, p->img_obj);
    fprintf(p->f, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB\n"
                  "   /BitsPerComponent 8 /Filter /FlateDecode\n"
                  "   /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns %d >>\n"
                  "   /Length %d 0 R >>\nstream\n", p->w, p->h, p->w, p->len_obj);
    p->stream_start = ftell(p->f);
    p->row_y = 0;
    memset(p->prev_row, 0, (size_t)p->w * 3 + 1);
    return zip_start(&p->z, p->f);
}

/* one row: pick the PNG predictor that gives the smallest numbers */
static void add_row(RpPdf *p)
{
    int n = p->w * 3;
    uint8_t *raw = p->row + 1, *up = p->prev_row + 1, *c = p->cand + 1;
    long s_none = 0, s_sub = 0, s_up = 0;
    for (int i = 0; i < n; i++) {
        s_none += raw[i] < 128 ? raw[i] : 256 - raw[i];
        uint8_t u = (uint8_t)(raw[i] - up[i]);
        s_up += u < 128 ? u : 256 - u;
        uint8_t s = (uint8_t)(raw[i] - (i >= 3 ? raw[i - 3] : 0));
        s_sub += s < 128 ? s : 256 - s;
    }
    if (s_up <= s_sub && s_up <= s_none) {
        c[-1] = 2;                                        /* "Up": difference to the row above */
        for (int i = 0; i < n; i++) c[i] = (uint8_t)(raw[i] - up[i]);
    } else if (s_sub <= s_none) {
        c[-1] = 1;                                        /* "Sub": difference to the left pixel */
        for (int i = 0; i < n; i++) c[i] = (uint8_t)(raw[i] - (i >= 3 ? raw[i - 3] : 0));
    } else {
        c[-1] = 0;                                        /* "None": as it is */
        memcpy(c, raw, (size_t)n);
    }
    zip_write(&p->z, p->cand, n + 1);
    uint8_t *t = p->prev_row; p->prev_row = p->row; p->row = t;
}

int rp_pdf_rows(RpPdf *p, const lv_color_t *rows, int nrows)
{
    for (int y = 0; y < nrows && p->row_y < p->h; y++, p->row_y++) {
        const lv_color_t *src = rows + (size_t)y * p->w;
        uint8_t *o = p->row + 1;
        for (int x = 0; x < p->w; x++) {
            uint32_t c = lv_color_to32(src[x]);         /* any LVGL colour depth -> 8 bits each */
            *o++ = (uint8_t)(c >> 16); *o++ = (uint8_t)(c >> 8); *o++ = (uint8_t)c;
        }
        add_row(p);
    }
    return p->z.err ? -1 : 0;
}

int rp_pdf_page_end(RpPdf *p)
{
    while (p->row_y < p->h) {                            /* (never happens: the page is complete) */
        memset(p->row + 1, 255, (size_t)p->w * 3);
        add_row(p);
        p->row_y++;
    }
    zip_finish(&p->z);
    zip_free(&p->z);
    long len = ftell(p->f) - p->stream_start;
    fputs("\nendstream\nendobj\n", p->f);
    begin_obj(p, p->len_obj);
    fprintf(p->f, "%ld\nendobj\n", len);

    /* the page: draw the picture over the whole A4 sheet */
    double pw = p->w * 72.0 / p->dpi, ph = p->h * 72.0 / p->dpi;
    char content[96];
    int clen = snprintf(content, sizeof content, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pw, ph);
    int cobj = new_obj(p), pobj = new_obj(p);
    if (cobj < 0 || pobj < 0) return -1;
    begin_obj(p, cobj);
    fprintf(p->f, "<< /Length %d >>\nstream\n%sendstream\nendobj\n", clen, content);
    begin_obj(p, pobj);
    fprintf(p->f, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f]\n"
                  "   /Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n",
            pw, ph, p->img_obj, cobj);
    if (p->npages == p->pcap) {
        int nc = p->pcap ? p->pcap * 2 : 8;
        int *n = rp_realloc(p->pages, sizeof(int) * (size_t)nc);
        if (!n) return -1;
        p->pages = n; p->pcap = nc;
    }
    p->pages[p->npages++] = pobj;
    return ferror(p->f) ? -1 : 0;
}

static void free_pdf(RpPdf *p)
{
    zip_free(&p->z);
    rp_free(p->ofs); rp_free(p->pages); rp_free(p->row); rp_free(p->prev_row); rp_free(p->cand);
    rp_free(p);
}

int rp_pdf_close(RpPdf *p, const char *title)
{
    begin_obj(p, 1);
    fputs("<< /Type /Catalog /Pages 2 0 R >>\nendobj\n", p->f);
    begin_obj(p, 2);
    fprintf(p->f, "<< /Type /Pages /Count %d /Kids [", p->npages);
    for (int i = 0; i < p->npages; i++) fprintf(p->f, "%s%d 0 R", i ? " " : "", p->pages[i]);
    fputs("] >>\nendobj\n", p->f);
    begin_obj(p, 3);
    fputs("<< /Title (", p->f);
    for (const char *t = title; *t; t++)                 /* plain ASCII title, () and \ escaped */
        if ((unsigned char)*t >= 32 && (unsigned char)*t < 127) {
            if (*t == '(' || *t == ')' || *t == '\\') fputc('\\', p->f);
            fputc(*t, p->f);
        }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char when[32];
    strftime(when, sizeof when, "%Y%m%d%H%M%S", &tm);
    fprintf(p->f, ") /Producer (NZEUS Deck car report) /Creator (NZEUS LAB) /CreationDate (D:%s) >>\nendobj\n", when);
    long xref = ftell(p->f);
    fprintf(p->f, "xref\n0 %d\n0000000000 65535 f \n", p->nobj + 1);
    for (int i = 1; i <= p->nobj; i++) fprintf(p->f, "%010ld 00000 n \n", p->ofs[i]);
    fprintf(p->f, "trailer\n<< /Size %d /Root 1 0 R /Info 3 0 R >>\nstartxref\n%ld\n%%%%EOF\n", p->nobj + 1, xref);
    int bad = ferror(p->f) != 0;
    if (fclose(p->f) != 0) bad = 1;
    p->f = NULL;
    if (!bad && rename(p->tmp, p->path) != 0) bad = 1;
    if (bad) remove(p->tmp);
    free_pdf(p);
    return bad ? -1 : 0;
}

void rp_pdf_abort(RpPdf *p)
{
    if (!p) return;
    if (p->f) fclose(p->f);
    remove(p->tmp);
    free_pdf(p);
}

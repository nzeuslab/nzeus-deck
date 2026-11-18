/* sdp.c - "Service Discovery Protocol": asking a classic Bluetooth device
 * what it offers. For a keyboard/mouse we need its HID record, above all the
 * "report descriptor" - a small table that says what each byte the device
 * sends means (which byte is the key code, which is the mouse X movement...).
 * The kernel needs that table to turn the device's messages into key presses.
 *
 * SDP talks in "data elements": a header byte (type + size) and a value.
 * Types: 1 = unsigned number, 3 = UUID, 4 = text, 6 = list ("sequence").
 * SDP numbers are big-endian (highest byte first), unlike the rest of Bluetooth. */
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "bt.h"

#define SDP_ERROR_RSP     0x01
#define SDP_SSA_REQ       0x06    /* "service search attribute" request */
#define SDP_SSA_RSP       0x07

#define DE_UINT 1
#define DE_UUID 3
#define DE_TEXT 4
#define DE_SEQ  6
#define DE_ALT  7

/* Reads one data element at p (n bytes available).
 * Out: its type, where its value starts, and the value's length.
 * Returns the total size (header + value), or -1 if damaged. */
static int de_read(const uint8_t *p, size_t n, int *type, const uint8_t **val, uint32_t *vlen)
{
    size_t hdr = 1;
    uint32_t len;
    if (n < 1) return -1;
    *type = p[0] >> 3;
    switch (p[0] & 7) {
    case 0: len = (*type == 0) ? 0 : 1; break;   /* type 0 = "nil": no value */
    case 1: len = 2; break;
    case 2: len = 4; break;
    case 3: len = 8; break;
    case 4: len = 16; break;
    case 5: if (n < 2) return -1; len = p[1]; hdr = 2; break;
    case 6: if (n < 3) return -1; len = get_be16(p + 1); hdr = 3; break;
    default: if (n < 5) return -1; len = get_be32(p + 1); hdr = 5; break;
    }
    if (len > n - hdr) return -1;
    *val = p + hdr;
    *vlen = len;
    return (int)(hdr + len);
}

/* unsigned number of 1, 2 or 4 bytes */
static int de_uint(int type, const uint8_t *v, uint32_t len, uint32_t *out)
{
    if (type != DE_UINT) return -1;
    if (len == 1) *out = v[0];
    else if (len == 2) *out = get_be16(v);
    else if (len == 4) *out = get_be32(v);
    else return -1;
    return 0;
}

/* Builds a "service search attribute" request: "give me ALL attributes of
 * every record with this 16-bit UUID". Returns its length. */
int sdp_build_request(uint8_t *buf, size_t bufsz, uint16_t tid, uint16_t uuid16,
                      const uint8_t *cont, int cont_len)
{
    uint8_t *p = buf + 5;
    if (cont_len < 0 || cont_len > 16 || bufsz < 5 + 15 + 1 + (size_t)cont_len) return -1;
    *p++ = 0x35; *p++ = 3;                         /* list of 3 bytes:     */
    *p++ = 0x19; put_be16(p, uuid16); p += 2;      /*   UUID (16-bit)      */
    put_be16(p, 0xFFFF); p += 2;                   /* reply up to 64 KB    */
    *p++ = 0x35; *p++ = 5;                         /* list of 5 bytes:     */
    *p++ = 0x0A; *p++ = 0; *p++ = 0; *p++ = 0xFF; *p++ = 0xFF;  /* attribute range 0x0000-0xFFFF */
    *p++ = (uint8_t)cont_len;                      /* "continue from here" */
    if (cont_len) { memcpy(p, cont, cont_len); p += cont_len; }
    buf[0] = SDP_SSA_REQ;
    put_be16(buf + 1, tid);
    put_be16(buf + 3, (uint16_t)(p - buf - 5));
    return (int)(p - buf);
}

/* Asks over an open SDP channel (fd). A long answer comes in pieces; each
 * piece ends with a "continuation" token we send back to get the next one.
 * The pieces glued together are written to out. Returns their length, -1 error. */
int sdp_query(int fd, uint16_t uuid16, uint8_t *out, size_t max, int timeout_ms)
{
    uint8_t req[64], rsp[4096], cont[17];
    int cont_len = 0, rounds;
    size_t total = 0;
    uint16_t tid = 1;
    for (rounds = 0; rounds < 64; rounds++, tid++) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int n, len = sdp_build_request(req, sizeof(req), tid, uuid16, cont, cont_len);
        uint16_t bytes;
        if (len < 0 || write(fd, req, len) != len) return -1;
        if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
        n = read(fd, rsp, sizeof(rsp));
        if (n < 5 || get_be16(rsp + 1) != tid) return -1;
        if (rsp[0] == SDP_ERROR_RSP) { dbg("sdp error 0x%04x", n >= 7 ? get_be16(rsp + 5) : 0); return -1; }
        if (rsp[0] != SDP_SSA_RSP || n < 7 || get_be16(rsp + 3) != n - 5) return -1;
        bytes = get_be16(rsp + 5);
        if (7 + bytes + 1 > n) return -1;
        if (total + bytes > max) return -1;
        memcpy(out + total, rsp + 7, bytes);
        total += bytes;
        cont_len = rsp[7 + bytes];
        if (cont_len > 16 || 7 + bytes + 1 + cont_len > n) return -1;
        if (cont_len == 0) return (int)total;          /* that was the last piece */
        memcpy(cont, rsp + 8 + bytes, cont_len);
    }
    return -1;
}

/* Calls fn(attribute id, its value element) for every attribute of every
 * record in the answer. The answer is a list of records; each record is a
 * list of (attribute id, value) pairs. fn returns nonzero to stop. */
typedef int (*attr_fn)(void *ctx, int record, uint16_t id, int type, const uint8_t *v, uint32_t len);
static int walk(const uint8_t *lists, size_t n, attr_fn fn, void *ctx)
{
    int type, rec = 0, used;
    const uint8_t *v;
    uint32_t len;
    size_t pos = 0;
    used = de_read(lists, n, &type, &v, &len);
    if (used < 0 || type != DE_SEQ) return -1;
    while (pos < len) {                              /* each record */
        int rtype, u;
        const uint8_t *rv; uint32_t rlen; size_t rpos = 0;
        u = de_read(v + pos, len - pos, &rtype, &rv, &rlen);
        if (u < 0 || rtype != DE_SEQ) return -1;
        while (rpos < rlen) {                        /* each (id, value) pair */
            int it, vt, a, b;
            const uint8_t *iv, *vv; uint32_t il, vl, id;
            a = de_read(rv + rpos, rlen - rpos, &it, &iv, &il);
            if (a < 0 || de_uint(it, iv, il, &id) < 0) return -1;
            b = de_read(rv + rpos + a, rlen - rpos - a, &vt, &vv, &vl);
            if (b < 0) return -1;
            if (fn(ctx, rec, (uint16_t)id, vt, vv, vl)) return 0;
            rpos += a + b;
        }
        pos += u;
        rec++;
    }
    return 0;
}

static int hid_attr(void *ctx, int record, uint16_t id, int type, const uint8_t *v, uint32_t len)
{
    struct hid_info *hi = ctx;
    uint32_t x;
    (void)record;
    switch (id) {
    case 0x0100:                                  /* service name */
        if (type == DE_TEXT) clean_name((const char *)v, len, hi->name, sizeof(hi->name));
        break;
    case 0x0201: if (de_uint(type, v, len, &x) == 0) hi->parser = x; break;
    case 0x0202: if (de_uint(type, v, len, &x) == 0) hi->subclass = x; break;
    case 0x0203: if (de_uint(type, v, len, &x) == 0) hi->country = x; break;
    case 0x0206: {                                /* HID descriptor list */
        /* list of lists: { { 0x22 (= report descriptor), <bytes as text> } ... } */
        uint32_t pos = 0;
        if (type != DE_SEQ) break;
        while (pos < len && !hi->found) {
            int t, u, a, kt, dt;
            const uint8_t *iv, *kv, *dv; uint32_t il, kl, dl, kind;
            u = de_read(v + pos, len - pos, &t, &iv, &il);
            if (u < 0) break;
            if (t == DE_SEQ) {
                a = de_read(iv, il, &kt, &kv, &kl);
                if (a > 0 && de_uint(kt, kv, kl, &kind) == 0 && kind == 0x22 &&
                    de_read(iv + a, il - a, &dt, &dv, &dl) > 0 && dt == DE_TEXT &&
                    dl > 0 && dl <= HID_DESC_MAX) {
                    memcpy(hi->rd, dv, dl);
                    hi->rd_size = (uint16_t)dl;
                    hi->found = 1;
                }
            }
            pos += u;
        }
        break;
    }
    }
    return 0;
}

/* Fills hi from the answer to an SDP query for UUID 0x1124 (HID).
 * Returns 0 if a report descriptor was found. */
int sdp_parse_hid(const uint8_t *lists, size_t n, struct hid_info *hi)
{
    memset(hi, 0, sizeof(*hi));
    hi->parser = 0x0111;               /* the usual value if the device omits it */
    if (walk(lists, n, hid_attr, hi) < 0 && !hi->found) return -1;
    return hi->found ? 0 : -1;
}

static int pnp_attr(void *ctx, int record, uint16_t id, int type, const uint8_t *v, uint32_t len)
{
    struct pnp_info *pi = ctx;
    uint32_t x;
    if (record > 0) return 1;          /* only the first PnP record */
    if (de_uint(type, v, len, &x) != 0) return 0;
    switch (id) {
    case 0x0201: pi->vendor = x; pi->found = 1; break;
    case 0x0202: pi->product = x; break;
    case 0x0203: pi->version = x; break;
    case 0x0205: pi->source = x; break;
    }
    return 0;
}

/* Fills pi from the answer to an SDP query for UUID 0x1200 (PnP information:
 * the maker's vendor/product numbers). Returns 0 if found. */
int sdp_parse_pnp(const uint8_t *lists, size_t n, struct pnp_info *pi)
{
    memset(pi, 0, sizeof(*pi));
    walk(lists, n, pnp_attr, pi);
    return pi->found ? 0 : -1;
}

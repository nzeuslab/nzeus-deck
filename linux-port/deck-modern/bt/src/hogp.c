/* hogp.c - Bluetooth LOW ENERGY keyboards and mice ("HID over GATT").
 *
 * LE devices don't use the kernel's hidp. Instead they offer a small database
 * ("GATT") that we read with the ATT protocol over L2CAP channel 4:
 *   - a HID service (number 0x1812) with
 *       * the Report Map  (0x2A4B) = the report descriptor (what the bytes mean)
 *       * Report values   (0x2A4D) = one per kind of message, each with a
 *         "Report Reference" note (0x2908) saying its report ID and if it is
 *         input (device -> us) or output (us -> device, e.g. Caps Lock light)
 *   - we switch on "notifications" for the input reports (write 1 to their
 *     0x2902 descriptor); from then on every key press arrives as a message.
 * We create a virtual HID device through /dev/uhid (uhid.ko) with the report
 * map, and copy every input message into it. The kernel's normal HID code
 * then makes /dev/input/eventN, like for any USB keyboard.
 *
 * This runs as its own small process per LE device (the daemon starts it),
 * until the device disconnects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include "bt.h"

#define ATT_ERROR_RSP        0x01
#define ATT_MTU_REQ          0x02
#define ATT_MTU_RSP          0x03
#define ATT_FIND_INFO_REQ    0x04
#define ATT_FIND_INFO_RSP    0x05
#define ATT_READ_BY_TYPE_REQ 0x08
#define ATT_READ_BY_TYPE_RSP 0x09
#define ATT_READ_REQ         0x0A
#define ATT_READ_RSP         0x0B
#define ATT_READ_BLOB_REQ    0x0C
#define ATT_READ_BLOB_RSP    0x0D
#define ATT_READ_GROUP_REQ   0x10
#define ATT_READ_GROUP_RSP   0x11
#define ATT_WRITE_REQ        0x12
#define ATT_WRITE_RSP        0x13
#define ATT_NOTIFY           0x1B
#define ATT_INDICATE         0x1D
#define ATT_CONFIRM          0x1E
#define ATT_WRITE_CMD        0x52
#define ATT_ERR_NOT_FOUND    0x0A
#define ATT_ERR_NOT_SUPPORTED 0x06

#define OUR_MTU  185
#define MAX_REPORTS 16
#define MAX_SERVICES 4

#define RTYPE_INPUT   1
#define RTYPE_OUTPUT  2
#define RTYPE_FEATURE 3

struct report {
    uint16_t handle;          /* where its value is */
    uint16_t cccd;            /* where to switch notifications on (0 = none) */
    uint8_t  id, type;        /* report ID, RTYPE_* */
};
struct hid_service {
    uint16_t start, end, map_handle;
    struct report rep[MAX_REPORTS];
    int n_rep;
    uint8_t rd[HID_DESC_MAX];
    uint16_t rd_size;
    uint8_t country;
    int uhid;                 /* our /dev/uhid device for this service */
};
struct att {
    int fd;
    uint16_t mtu;
    struct hid_service svc[MAX_SERVICES];
    int n_svc;
};

static int att_send(struct att *a, const uint8_t *p, int len)
{
    return write(a->fd, p, len) == len ? 0 : -1;
}

/* The device may ask US things too (we are a GATT server for it). We have no
 * database, so we answer "not found" - but we must answer, or it gives up. */
static void answer_device_request(struct att *a, const uint8_t *p, int n)
{
    uint8_t rsp[5];
    if (n < 1) return;
    if (p[0] == ATT_MTU_REQ && n >= 3) {
        rsp[0] = ATT_MTU_RSP;
        put_le16(rsp + 1, OUR_MTU);
        att_send(a, rsp, 3);
        return;
    }
    if (p[0] == ATT_INDICATE) {            /* indications must be confirmed */
        rsp[0] = ATT_CONFIRM;
        att_send(a, rsp, 1);
        return;
    }
    if ((p[0] & 0x40) || (p[0] & 1))       /* commands and responses: no answer */
        return;
    rsp[0] = ATT_ERROR_RSP;
    rsp[1] = p[0];
    put_le16(rsp + 2, n >= 3 ? get_le16(p + 1) : 0);
    rsp[4] = (p[0] == ATT_FIND_INFO_REQ || p[0] == 0x06 || p[0] == ATT_READ_BY_TYPE_REQ ||
              p[0] == ATT_READ_GROUP_REQ) ? ATT_ERR_NOT_FOUND : ATT_ERR_NOT_SUPPORTED;
    att_send(a, rsp, 5);
}

static void handle_notify(struct att *a, const uint8_t *p, int n);

/* Sends a request and waits for its answer (want = answer opcode).
 * Returns the answer length, -2 if the device answered with an ATT error
 * (code in *err), -1 if the link failed. */
static int att_request(struct att *a, const uint8_t *req, int len, uint8_t want,
                       uint8_t *rsp, int max, uint8_t *err)
{
    long long end = now_ms() + 10000;
    if (att_send(a, req, len) < 0) return -1;
    for (;;) {
        struct pollfd pfd = { a->fd, POLLIN, 0 };
        long long left = end - now_ms();
        int n;
        if (left <= 0) return -1;
        if (poll(&pfd, 1, (int)left) <= 0) return -1;
        n = read(a->fd, rsp, max);
        if (n <= 0) return -1;
        if (rsp[0] == want) return n;
        if (rsp[0] == ATT_ERROR_RSP && n >= 5 && rsp[1] == req[0]) {
            if (err) *err = rsp[4];
            return -2;
        }
        if (rsp[0] == ATT_NOTIFY) handle_notify(a, rsp, n);
        else answer_device_request(a, rsp, n);
    }
}

/* ---- discovery ---- */
static int find_services(struct att *a, uint16_t *dis_start, uint16_t *dis_end)
{
    uint8_t req[7], rsp[512], err = 0;
    uint16_t start = 1;
    *dis_start = *dis_end = 0;
    while (start != 0) {
        int n, i, elen;
        req[0] = ATT_READ_GROUP_REQ;
        put_le16(req + 1, start);
        put_le16(req + 3, 0xFFFF);
        put_le16(req + 5, 0x2800);                 /* "primary service" */
        n = att_request(a, req, 7, ATT_READ_GROUP_RSP, rsp, sizeof(rsp), &err);
        if (n == -2 && err == ATT_ERR_NOT_FOUND) break;   /* no more */
        if (n < 2) return -1;
        elen = rsp[1];
        if (elen < 6) return -1;
        for (i = 2; i + elen <= n; i += elen) {
            uint16_t s = get_le16(rsp + i), e = get_le16(rsp + i + 2);
            uint16_t uuid = (elen == 6) ? get_le16(rsp + i + 4) : 0;
            dbg("service 0x%04x handles %u-%u", uuid, s, e);
            if (uuid == 0x1812 && a->n_svc < MAX_SERVICES) {
                struct hid_service *h = &a->svc[a->n_svc++];
                memset(h, 0, sizeof(*h));
                h->start = s; h->end = e; h->uhid = -1;
            }
            if (uuid == 0x180A) { *dis_start = s; *dis_end = e; }
            start = (e == 0xFFFF) ? 0 : e + 1;
            if (e < s) return -1;
        }
    }
    return 0;
}

struct chr { uint16_t decl, value; uint16_t uuid; };

static int find_chars(struct att *a, uint16_t s, uint16_t e, struct chr *out, int max)
{
    uint8_t req[7], rsp[512], err = 0;
    int count = 0;
    while (s <= e && s != 0) {
        int n, i, elen;
        req[0] = ATT_READ_BY_TYPE_REQ;
        put_le16(req + 1, s);
        put_le16(req + 3, e);
        put_le16(req + 5, 0x2803);                 /* "characteristic" */
        n = att_request(a, req, 7, ATT_READ_BY_TYPE_RSP, rsp, sizeof(rsp), &err);
        if (n == -2 && err == ATT_ERR_NOT_FOUND) break;
        if (n < 2) return -1;
        elen = rsp[1];
        if (elen < 7) return -1;
        for (i = 2; i + elen <= n; i += elen) {
            uint16_t decl = get_le16(rsp + i);
            if (count < max) {
                out[count].decl = decl;
                out[count].value = get_le16(rsp + i + 3);
                out[count].uuid = (elen == 7) ? get_le16(rsp + i + 5) : 0;
                count++;
            }
            if (decl < s) return -1;
            s = decl + 1;
        }
    }
    return count;
}

/* descriptors between two handles: fills cccd / report reference handles */
static int find_descs(struct att *a, uint16_t s, uint16_t e, uint16_t *cccd, uint16_t *rref)
{
    uint8_t req[5], rsp[512], err = 0;
    *cccd = *rref = 0;
    while (s <= e && s != 0) {
        int n, i;
        req[0] = ATT_FIND_INFO_REQ;
        put_le16(req + 1, s);
        put_le16(req + 3, e);
        n = att_request(a, req, 5, ATT_FIND_INFO_RSP, rsp, sizeof(rsp), &err);
        if (n == -2 && err == ATT_ERR_NOT_FOUND) break;
        if (n < 2) return -1;
        if (rsp[1] != 1) break;                    /* 128-bit UUIDs: not ours */
        for (i = 2; i + 4 <= n; i += 4) {
            uint16_t h = get_le16(rsp + i), u = get_le16(rsp + i + 2);
            if (u == 0x2902) *cccd = h;
            if (u == 0x2908) *rref = h;
            if (h < s) return -1;
            s = h + 1;
        }
    }
    return 0;
}

/* reads a value of any length (Read, then Read Blob for the rest) */
static int read_long(struct att *a, uint16_t handle, uint8_t *out, int max)
{
    uint8_t req[5], rsp[512], err = 0;
    int total = 0, n;
    req[0] = ATT_READ_REQ;
    put_le16(req + 1, handle);
    n = att_request(a, req, 3, ATT_READ_RSP, rsp, sizeof(rsp), &err);
    if (n < 1) return -1;
    for (;;) {
        int part = n - 1;
        if (total + part > max) part = max - total;
        memcpy(out + total, rsp + 1, part);
        total += part;
        if (n - 1 < a->mtu - 1 || total >= max) return total;   /* short piece = the end */
        req[0] = ATT_READ_BLOB_REQ;
        put_le16(req + 1, handle);
        put_le16(req + 3, (uint16_t)total);
        n = att_request(a, req, 5, ATT_READ_BLOB_RSP, rsp, sizeof(rsp), &err);
        if (n == -2) return total;                 /* "offset too big" = done */
        if (n < 1) return -1;
        if (n == 1) return total;
    }
}

static int write_req(struct att *a, uint16_t handle, const uint8_t *v, int len)
{
    uint8_t req[64], rsp[64], err = 0;
    if (len > (int)sizeof(req) - 3) return -1;
    req[0] = ATT_WRITE_REQ;
    put_le16(req + 1, handle);
    memcpy(req + 3, v, len);
    return att_request(a, req, 3 + len, ATT_WRITE_RSP, rsp, sizeof(rsp), &err) >= 1 ? 0 : -1;
}

/* ---- uhid side ---- */
static int uhid_write(int fd, struct uhid_event *ev)
{
    return write(fd, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev) ? 0 : -1;
}

static int uhid_create(struct hid_service *h, const struct bt_device *d, uint16_t vendor,
                       uint16_t product, uint16_t version)
{
    static struct uhid_event ev;
    char a[18];
    h->uhid = io_uhid_open();
    if (h->uhid < 0) return -1;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%s",
             d->name[0] ? d->name : "Bluetooth LE HID");
    addr_str(&d->addr, a);
    snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "%s", a);
    snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "nzeus-bt");
    ev.u.create2.rd_size = h->rd_size;
    ev.u.create2.bus = BUS_BLUETOOTH;
    ev.u.create2.vendor = vendor;
    ev.u.create2.product = product;
    ev.u.create2.version = version;
    ev.u.create2.country = h->country;
    memcpy(ev.u.create2.rd_data, h->rd, h->rd_size);
    if (uhid_write(h->uhid, &ev) < 0) { close(h->uhid); h->uhid = -1; return -1; }
    return 0;
}

/* A key press arrived (notification): hand it to the right uhid device. */
static void handle_notify(struct att *a, const uint8_t *p, int n)
{
    static struct uhid_event ev;
    uint16_t handle;
    int s, r;
    if (n < 3) return;
    handle = get_le16(p + 1);
    for (s = 0; s < a->n_svc; s++) {
        struct hid_service *h = &a->svc[s];
        for (r = 0; r < h->n_rep; r++) {
            struct report *rp = &h->rep[r];
            int len = n - 3, off = 0;
            if (rp->handle != handle || rp->type != RTYPE_INPUT || h->uhid < 0) continue;
            memset(&ev, 0, sizeof(ev.type) + sizeof(ev.u.input2.size));
            ev.type = UHID_INPUT2;
            if (rp->id) ev.u.input2.data[off++] = rp->id;   /* report ID goes first */
            if (len + off > UHID_DATA_MAX) len = UHID_DATA_MAX - off;
            memcpy(ev.u.input2.data + off, p + 3, len);
            ev.u.input2.size = (uint16_t)(len + off);
            uhid_write(h->uhid, &ev);
            return;
        }
    }
}

/* Messages from the kernel's HID side: LED changes, report requests. */
static void handle_uhid(struct att *a, struct hid_service *h)
{
    static struct uhid_event ev, rsp;
    int n = read(h->uhid, &ev, sizeof(ev)), r;
    if (n < 4) return;
    switch (ev.type) {
    case UHID_OUTPUT: {     /* e.g. Caps Lock light: send to the output report */
        uint8_t *data = ev.u.output.data, cmd[OUR_MTU];
        int size = ev.u.output.size, id = 0, has_ids = 0;
        for (r = 0; r < h->n_rep; r++) if (h->rep[r].id) has_ids = 1;
        if (has_ids && size > 0) { id = data[0]; data++; size--; }
        for (r = 0; r < h->n_rep; r++) {
            if (h->rep[r].type != RTYPE_OUTPUT || h->rep[r].id != id) continue;
            if (size > a->mtu - 3) size = a->mtu - 3;
            cmd[0] = ATT_WRITE_CMD;
            put_le16(cmd + 1, h->rep[r].handle);
            memcpy(cmd + 3, data, size);
            att_send(a, cmd, 3 + size);
            break;
        }
        break;
    }
    case UHID_GET_REPORT:   /* not supported: answer with an error at once */
        memset(&rsp, 0, sizeof(rsp));
        rsp.type = UHID_GET_REPORT_REPLY;
        rsp.u.get_report_reply.id = ev.u.get_report.id;
        rsp.u.get_report_reply.err = EIO;
        uhid_write(h->uhid, &rsp);
        break;
    case UHID_SET_REPORT:
        memset(&rsp, 0, sizeof(rsp));
        rsp.type = UHID_SET_REPORT_REPLY;
        rsp.u.set_report_reply.id = ev.u.set_report.id;
        rsp.u.set_report_reply.err = EIO;
        uhid_write(h->uhid, &rsp);
        break;
    }
}

static void destroy_all(struct att *a)
{
    static struct uhid_event ev;
    int s;
    for (s = 0; s < a->n_svc; s++) {
        if (a->svc[s].uhid < 0) continue;
        memset(&ev, 0, sizeof(ev));
        ev.type = UHID_DESTROY;
        uhid_write(a->svc[s].uhid, &ev);
        close(a->svc[s].uhid);
        a->svc[s].uhid = -1;
    }
}

int hogp_run(const struct bt_device *d)
{
    static struct att a;
    static struct chr chars[64];
    uint8_t buf[512], err = 0;
    uint16_t dis_s, dis_e, vendor = 0, product = 0, version = 0;
    char addr[18];
    int s, r, n, ok = 0;

    memset(&a, 0, sizeof(a));
    addr_str(&d->addr, addr);
    a.mtu = 23;                                  /* ATT's starting size */
    a.fd = io_l2cap_connect(&d->addr, d->addr_type, 0, L2CAP_CID_ATT, BT_SEC_MEDIUM, 20000);
    if (a.fd < 0) {
        printf("%s: LE connection failed: %s\n", addr, strerror(errno));
        return 1;
    }
    /* ask for bigger messages (some reports are longer than 20 bytes) */
    buf[0] = ATT_MTU_REQ;
    put_le16(buf + 1, OUR_MTU);
    n = att_request(&a, buf, 3, ATT_MTU_RSP, buf, sizeof(buf), &err);
    if (n >= 3) {
        uint16_t m = get_le16(buf + 1);
        a.mtu = m < OUR_MTU ? (m < 23 ? 23 : m) : OUR_MTU;
    }
    if (find_services(&a, &dis_s, &dis_e) < 0 || a.n_svc == 0) {
        printf("%s: no HID service found (not a keyboard/mouse?)\n", addr);
        close(a.fd);
        return 1;
    }
    /* maker numbers from the "Device Information" service (PnP ID 0x2A50) */
    if (dis_s) {
        uint8_t req[7];
        req[0] = ATT_READ_BY_TYPE_REQ;
        put_le16(req + 1, dis_s);
        put_le16(req + 3, dis_e);
        put_le16(req + 5, 0x2A50);
        n = att_request(&a, req, 7, ATT_READ_BY_TYPE_RSP, buf, sizeof(buf), &err);
        if (n >= 2 + 2 + 7 && buf[1] >= 9) {
            vendor = get_le16(buf + 5); product = get_le16(buf + 7); version = get_le16(buf + 9);
        }
    }
    for (s = 0; s < a.n_svc; s++) {
        struct hid_service *h = &a.svc[s];
        int nc = find_chars(&a, h->start, h->end, chars, 64), c;
        if (nc < 0) goto fail;
        for (c = 0; c < nc; c++) {
            uint16_t last = (c + 1 < nc) ? chars[c + 1].decl - 1 : h->end;
            if (chars[c].uuid == 0x2A4B) h->map_handle = chars[c].value;
            if (chars[c].uuid == 0x2A4A) {               /* HID information */
                n = read_long(&a, chars[c].value, buf, sizeof(buf));
                if (n >= 3) h->country = buf[2];
            }
            if (chars[c].uuid == 0x2A4D && h->n_rep < MAX_REPORTS) {
                struct report *rp = &h->rep[h->n_rep];
                uint16_t rref;
                memset(rp, 0, sizeof(*rp));
                rp->handle = chars[c].value;
                rp->type = RTYPE_INPUT;
                if (find_descs(&a, chars[c].value + 1, last, &rp->cccd, &rref) < 0) goto fail;
                if (rref && read_long(&a, rref, buf, 2) == 2) { rp->id = buf[0]; rp->type = buf[1]; }
                h->n_rep++;
                dbg("report handle %u id %u type %u cccd %u", rp->handle, rp->id, rp->type, rp->cccd);
            }
        }
        if (!h->map_handle) continue;
        n = read_long(&a, h->map_handle, h->rd, sizeof(h->rd));
        if (n <= 0) goto fail;
        h->rd_size = (uint16_t)n;
        if (uhid_create(h, d, vendor, product, version) < 0) {
            printf("%s: cannot create the input device (is uhid.ko loaded?): %s\n", addr, strerror(errno));
            goto fail;
        }
        /* switch notifications on for every input report */
        for (r = 0; r < h->n_rep; r++) {
            static const uint8_t on[2] = { 1, 0 };
            if (h->rep[r].type == RTYPE_INPUT && h->rep[r].cccd)
                write_req(&a, h->rep[r].cccd, on, 2);
        }
        ok++;
    }
    if (!ok) { printf("%s: HID service has no report map\n", addr); goto fail; }
    printf("%s: connected (LE keyboard/mouse, %d HID service%s)\n", addr, ok, ok > 1 ? "s" : "");
    fflush(stdout);

    for (;;) {                                   /* forward messages until disconnect */
        struct pollfd p[1 + MAX_SERVICES];
        int np = 1;
        p[0].fd = a.fd; p[0].events = POLLIN; p[0].revents = 0;
        for (s = 0; s < a.n_svc; s++) {
            p[np].fd = a.svc[s].uhid; p[np].events = POLLIN; p[np].revents = 0; np++;
        }
        if (poll(p, np, -1) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents & (POLLHUP | POLLERR)) break;
        if (p[0].revents & POLLIN) {
            n = read(a.fd, buf, sizeof(buf));
            if (n <= 0) break;
            if (buf[0] == ATT_NOTIFY) handle_notify(&a, buf, n);
            else answer_device_request(&a, buf, n);
        }
        for (s = 0; s < a.n_svc; s++)
            if (a.svc[s].uhid >= 0 && (p[1 + s].revents & POLLIN)) handle_uhid(&a, &a.svc[s]);
    }
    printf("%s: disconnected\n", addr);
    destroy_all(&a);
    close(a.fd);
    return 0;
fail:
    printf("%s: reading the device's HID information failed\n", addr);
    destroy_all(&a);
    close(a.fd);
    return 1;
}

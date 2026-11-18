/* io.c - the only file that talks to the real kernel (sockets, ioctls,
 * /dev/uhid). Built with -DNZBT_FAKE (PC tests only), the same functions
 * talk to a fake kernel program over Unix sockets in $NZBT_FAKE_DIR instead,
 * so all the rest of nzeus-bt can be tested on a PC without Bluetooth. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include "bt.h"

/* our controller's address (all zeros = "any") - set once we know it */
static bdaddr_t g_local;
void io_set_local_addr(const bdaddr_t *a) { g_local = *a; }

#ifndef NZBT_FAKE
/* ======================= the real thing (console) ======================= */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
#define BTPROTO_HCI   1
#define BTPROTO_HIDP  6
#define SOL_BLUETOOTH 274
#define BT_SECURITY   4
#define HCI_DEV_NONE  0xffff
#define HCI_CHANNEL_CONTROL 3
#define HIDPCONNADD     _IOW('H', 200, int)
#define HIDPCONNDEL     _IOW('H', 201, int)
#define HIDPGETCONNLIST _IOR('H', 210, int)

struct sockaddr_hci { sa_family_t hci_family; unsigned short hci_dev, hci_channel; };
struct sockaddr_l2 {
    sa_family_t l2_family;
    uint16_t    l2_psm;          /* little-endian */
    bdaddr_t    l2_bdaddr;
    uint16_t    l2_cid;          /* little-endian */
    uint8_t     l2_bdaddr_type;
};
struct bt_security { uint8_t level, key_size; };

int io_mgmt_socket(void)
{
    struct sockaddr_hci a;
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) return -1;      /* EAFNOSUPPORT = bluetooth.ko is not loaded */
    memset(&a, 0, sizeof(a));
    a.hci_family = AF_BLUETOOTH;
    a.hci_dev = HCI_DEV_NONE;
    a.hci_channel = HCI_CHANNEL_CONTROL;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

static int set_security(int fd, int level)
{
    struct bt_security s = { (uint8_t)level, 0 };
    return setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &s, sizeof(s));
}

/* Opens a data channel to a device: classic = by PSM ("port"), LE = the ATT
 * channel (cid 4). Waits up to timeout_ms. Returns the socket or -1 (errno).
 * Our side is bound to OUR controller's address (io_set_local_addr): with no
 * address the kernel picks the first switched-on controller, which may be
 * the wrong one when there are several (found with the UML test). */
int io_l2cap_connect(const bdaddr_t *addr, uint8_t addr_type, uint16_t psm, uint16_t cid,
                     int sec_level, int timeout_ms)
{
    struct sockaddr_l2 a;
    struct pollfd pfd;
    int fd, err = 0, flags;
    socklen_t elen = sizeof(err);
    fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.l2_family = AF_BLUETOOTH;
    a.l2_bdaddr = g_local;
    if (addr_type != ADDR_BREDR) {
        /* LE: our side is "LE, ATT channel", like BlueZ does */
        put_le16((uint8_t *)&a.l2_cid, L2CAP_CID_ATT);
        a.l2_bdaddr_type = ADDR_LE_PUBLIC;
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) goto fail;
    if (sec_level > 0 && set_security(fd, sec_level) < 0) goto fail;
    memset(&a, 0, sizeof(a));
    a.l2_family = AF_BLUETOOTH;
    put_le16((uint8_t *)&a.l2_psm, psm);
    put_le16((uint8_t *)&a.l2_cid, cid);
    a.l2_bdaddr = *addr;
    a.l2_bdaddr_type = addr_type;
    flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);       /* so we can give up after timeout_ms */
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) goto fail;
        pfd.fd = fd; pfd.events = POLLOUT; pfd.revents = 0;
        if (poll(&pfd, 1, timeout_ms) <= 0) { errno = ETIMEDOUT; goto fail; }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) goto fail;
        if (err) { errno = err; goto fail; }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
fail:
    err = errno;
    close(fd);
    errno = err;
    return -1;
}

/* Waits for devices to connect to us on a PSM (keyboards reconnect this way). */
int io_l2cap_listen(uint16_t psm, int sec_level)
{
    struct sockaddr_l2 a;
    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP), e;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.l2_family = AF_BLUETOOTH;
    a.l2_bdaddr = g_local;                       /* only calls to OUR controller */
    put_le16((uint8_t *)&a.l2_psm, psm);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || set_security(fd, sec_level) < 0 ||
        listen(fd, 5) < 0) {
        e = errno; close(fd); errno = e;
        return -1;
    }
    return fd;
}

int io_l2cap_accept(int lfd, bdaddr_t *peer)
{
    struct sockaddr_l2 a;
    socklen_t len = sizeof(a);
    int fd = accept(lfd, (struct sockaddr *)&a, &len);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    *peer = a.l2_bdaddr;
    return fd;
}

static int hidp_ctl(void)
{
    return socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HIDP);
}

int io_hidp_add(struct hidp_connadd_req *req)
{
    int fd = hidp_ctl(), r, e;
    if (fd < 0) return -1;    /* EPROTONOSUPPORT = hidp.ko is not loaded */
    r = ioctl(fd, HIDPCONNADD, req);
    e = errno; close(fd); errno = e;
    return r;
}

int io_hidp_del(const bdaddr_t *a)
{
    struct hidp_conndel_req req;
    int fd = hidp_ctl(), r, e;
    if (fd < 0) return -1;
    memset(&req, 0, sizeof(req));
    req.bdaddr = *a;
    r = ioctl(fd, HIDPCONNDEL, &req);
    e = errno; close(fd); errno = e;
    return r;
}

int io_hidp_list(struct hidp_conninfo *ci, int max)
{
    struct hidp_connlist_req req;
    int fd = hidp_ctl(), r;
    if (fd < 0) return -1;
    req.cnum = max;
    req.ci = ci;
    r = ioctl(fd, HIDPGETCONNLIST, &req);
    close(fd);
    return r < 0 ? -1 : (int)req.cnum;
}

int io_uhid_open(void)
{
    return open("/dev/uhid", O_RDWR | O_CLOEXEC);
}

/* small run-time files (daemon pid, last scan) live in /tmp (RAM): "/tmp/nzeus-bt-<name>" */
const char *io_runfile(const char *name)
{
    static char p[128];
    snprintf(p, sizeof(p), "/tmp/nzeus-bt-%s", name);
    return p;
}

#else
/* ======================= fake kernel (PC tests) ======================= */
/* Every "kernel" socket is a Unix SEQPACKET socket (it keeps message
 * boundaries, just like Bluetooth sockets) to tests/fake_kernel.py. */
static const char *fake_dir(void)
{
    const char *d = getenv("NZBT_FAKE_DIR");
    return d ? d : "/tmp/nzbt-fake";
}

static int fake_connect(const char *name)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), e;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", fake_dir(), name);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { e = errno; close(fd); errno = e; return -1; }
    return fd;
}

/* sends a text line, reads the one-line answer: "OK ..." or "ERR <errno>" */
static int fake_call(int fd, const char *msg, char *answer, size_t n)
{
    struct pollfd p = { fd, POLLIN, 0 };
    int r;
    if (write(fd, msg, strlen(msg)) < 0) return -1;
    if (poll(&p, 1, 5000) <= 0) { errno = ETIMEDOUT; return -1; }
    r = read(fd, answer, n - 1);
    if (r <= 0) { errno = ECONNRESET; return -1; }
    answer[r] = 0;
    if (!strncmp(answer, "ERR ", 4)) { errno = atoi(answer + 4); return -1; }
    return 0;
}

int io_mgmt_socket(void) { return fake_connect("mgmt"); }

int io_l2cap_connect(const bdaddr_t *addr, uint8_t addr_type, uint16_t psm, uint16_t cid,
                     int sec_level, int timeout_ms)
{
    char msg[128], ans[256], s[18], l[18];
    int fd = fake_connect("l2cap"), e;
    (void)timeout_ms;
    if (fd < 0) return -1;
    addr_str(addr, s);
    addr_str(&g_local, l);          /* the fake refuses calls not made from its controller */
    snprintf(msg, sizeof(msg), "CONNECT %s %u %u %u %d %s", s, addr_type, psm, cid, sec_level, l);
    if (fake_call(fd, msg, ans, sizeof(ans)) < 0) { e = errno; close(fd); errno = e; return -1; }
    return fd;
}

int io_l2cap_listen(uint16_t psm, int sec_level)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    (void)sec_level;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/listen-%u", fake_dir(), psm);
    unlink(a.sun_path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 5) < 0) { close(fd); return -1; }
    return fd;
}

int io_l2cap_accept(int lfd, bdaddr_t *peer)
{
    char buf[64];
    struct pollfd p;
    int fd = accept(lfd, NULL, NULL), n;
    if (fd < 0) return -1;
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    /* the fake device first says who it is: "FROM AA:BB:CC:DD:EE:FF" */
    if (poll(&p, 1, 2000) <= 0 || (n = read(fd, buf, sizeof(buf) - 1)) <= 5) { close(fd); return -1; }
    buf[n] = 0;
    if (strncmp(buf, "FROM ", 5) || addr_parse(buf + 5, peer)) { close(fd); return -1; }
    return fd;
}

int io_hidp_add(struct hidp_connadd_req *req)
{
    static char msg[2 * HID_DESC_MAX + 512];
    char ans[64], hex[2 * HID_DESC_MAX + 1];
    int n;
    hex_encode(req->rd_data, req->rd_size, hex, sizeof(hex));
    n = snprintf(msg, sizeof(msg), "HIDPCONNADD vendor=0x%04x product=0x%04x version=0x%04x "
                 "parser=0x%04x country=%u subclass=0x%02x flags=%u rd=%s name=%s",
                 req->vendor, req->product, req->version, req->parser, req->country,
                 req->subclass, req->flags, req->rd_size ? hex : "-", req->name);
    (void)n;
    /* the kernel takes both channels; the fake checks them via the control socket */
    if (write(req->intr_sock, "HIDP-INTR", 9) < 0) return -1;
    return fake_call(req->ctrl_sock, msg, ans, sizeof(ans));
}

static int fake_hidp(const char *msg, char *ans, size_t n)
{
    int fd = fake_connect("hidp"), r, e;
    if (fd < 0) return -1;
    r = fake_call(fd, msg, ans, n);
    e = errno; close(fd); errno = e;
    return r;
}

int io_hidp_del(const bdaddr_t *a)
{
    char msg[64], ans[64], s[18];
    addr_str(a, s);
    snprintf(msg, sizeof(msg), "DEL %s", s);
    return fake_hidp(msg, ans, sizeof(ans));
}

int io_hidp_list(struct hidp_conninfo *ci, int max)
{
    char ans[1024], *p;
    int n = 0;
    if (fake_hidp("LIST", ans, sizeof(ans)) < 0) return -1;
    /* "OK AA:BB:...,AA:BB:..." */
    p = ans + 2;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (strlen(p) < 17) break;
        memset(&ci[n], 0, sizeof(ci[n]));
        {
            char one[18];
            memcpy(one, p, 17); one[17] = 0;
            if (addr_parse(one, &ci[n].bdaddr) == 0) n++;
        }
        p += 17;
    }
    return n;
}

int io_uhid_open(void) { return fake_connect("uhid"); }

const char *io_runfile(const char *name)
{
    static char p[256];
    snprintf(p, sizeof(p), "%s/run-%s", fake_dir(), name);
    return p;
}
#endif

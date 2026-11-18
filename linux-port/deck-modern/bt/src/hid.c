/* hid.c - classic Bluetooth keyboards and mice ("HID profile").
 *
 * A classic keyboard/mouse uses two data channels:
 *     PSM 17 = "control" (commands, rarely used)
 *     PSM 19 = "interrupt" (the key presses / mouse moves)
 * Once both are open, we hand them to the kernel (the HIDPCONNADD ioctl of
 * hidp.ko) together with the device's report descriptor (from SDP, see sdp.c).
 * The kernel then creates a normal input device (/dev/input/eventN) - the
 * NZEUS GUI sees it like a USB keyboard. This is how the old BlueZ "hidd"
 * tool worked, without needing the big bluetoothd daemon. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "bt.h"

#define HIDP_BLUETOOTH_VENDOR_ID 9

/* Reads the device's HID record (+ maker IDs) over SDP into d.
 * Returns 0 if a report descriptor was found. */
int hid_fetch_sdp(const bdaddr_t *a, struct bt_device *d, int timeout_ms)
{
    static uint8_t buf[16384];
    static struct hid_info hi;
    struct pnp_info pi;
    int fd, n, ok = -1;
    fd = io_l2cap_connect(a, ADDR_BREDR, PSM_SDP, 0, 0, timeout_ms);
    if (fd < 0) {
        dbg("sdp connect failed: %s", strerror(errno));
        return -1;
    }
    n = sdp_query(fd, UUID_HID, buf, sizeof(buf), timeout_ms);
    if (n > 0 && sdp_parse_hid(buf, n, &hi) == 0) {
        d->has_hid = 1;
        d->hid_parser = hi.parser;
        d->hid_subclass = hi.subclass;
        d->hid_country = hi.country;
        d->rd_size = hi.rd_size;
        memcpy(d->rd, hi.rd, hi.rd_size);
        if (!d->name[0] && hi.name[0]) snprintf(d->name, sizeof(d->name), "%s", hi.name);
        ok = 0;
    } else {
        char hex[2 * 160 + 1];
        hex_encode(buf, n > 0 ? (n < 160 ? n : 160) : 0, hex, sizeof(hex));
        dbg("no HID record (sdp answer %d bytes: %s)", n, hex);
    }
    n = sdp_query(fd, UUID_PNP, buf, sizeof(buf), timeout_ms);
    if (n > 0 && sdp_parse_pnp(buf, n, &pi) == 0) {
        d->hid_vendor = pi.vendor;
        d->hid_product = pi.product;
        d->hid_version = pi.version;
        d->hid_vendor_src = (uint8_t)pi.source;
    }
    close(fd);
    return ok;
}

/* Gives the two open channels to the kernel (HIDPCONNADD). */
int hid_attach(int ctrl, int intr, const struct bt_device *d)
{
    static struct hidp_connadd_req req;
    memset(&req, 0, sizeof(req));
    req.ctrl_sock = ctrl;
    req.intr_sock = intr;
    req.parser = d->hid_parser ? d->hid_parser : 0x0111;
    /* rd_size 0 = no descriptor known: the kernel then falls back to the
     * simple "boot protocol" keyboard/mouse format */
    req.rd_size = d->rd_size;
    req.rd_data = (uint8_t *)d->rd;
    req.country = d->hid_country;
    req.subclass = d->hid_subclass;
    req.vendor = d->hid_vendor;
    req.product = d->hid_product;
    req.version = d->hid_version;
    if (d->hid_vendor_src == 1)          /* vendor number from the Bluetooth SIG list */
        req.flags = 1u << HIDP_BLUETOOTH_VENDOR_ID;
    req.idle_to = 0;
    snprintf(req.name, sizeof(req.name), "%s", d->name[0] ? d->name : "Bluetooth HID");
    return io_hidp_add(&req);
}

/* We call the device (host-initiated connection). Returns 0 or -1 (errno). */
int hid_connect(const struct bt_device *d, int timeout_ms)
{
    int ctrl, intr, r, e;
    ctrl = io_l2cap_connect(&d->addr, ADDR_BREDR, PSM_HID_CTRL, 0, BT_SEC_MEDIUM, timeout_ms);
    if (ctrl < 0) return -1;
    intr = io_l2cap_connect(&d->addr, ADDR_BREDR, PSM_HID_INTR, 0, BT_SEC_MEDIUM, timeout_ms);
    if (intr < 0) { e = errno; close(ctrl); errno = e; return -1; }
    r = hid_attach(ctrl, intr, d);
    e = errno;
    /* the kernel keeps its own reference to both channels: we can let go */
    close(ctrl);
    close(intr);
    errno = e;
    return r;
}

int hid_detach(const bdaddr_t *a)
{
    return io_hidp_del(a);
}

int hid_is_attached(const bdaddr_t *a)
{
    struct hidp_conninfo ci[16];
    int n = io_hidp_list(ci, 16), i;
    for (i = 0; i < n; i++)
        if (addr_eq(&ci[i].bdaddr, a)) return 1;
    return 0;
}

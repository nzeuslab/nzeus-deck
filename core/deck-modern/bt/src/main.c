/* nzeus-bt - Bluetooth keyboards and mice for the NZEUS R36S console.
 *
 *   nzeus-bt on | off | status
 *   nzeus-bt scan [seconds]
 *   nzeus-bt pair ADDR [--pin NNNN]
 *   nzeus-bt connect ADDR | disconnect ADDR | remove ADDR
 *   nzeus-bt list
 *   nzeus-bt daemon            (background helper, started by nzeus-bt-up.sh)
 *
 * Output is meant for both people and the NZEUS Settings app: one result per
 * line, fields separated by TABs where a line describes a device.
 * See README.md for the exact formats. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "bt.h"

static struct mgmt M;
static int g_index = -1;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* mgmt "address info" = 6 address bytes + 1 type byte */
static void put_addr(uint8_t *p, const bdaddr_t *a, uint8_t type)
{
    memcpy(p, a->b, 6);
    p[6] = type;
}

static int req(uint16_t op, const void *p, uint16_t len, uint8_t *rsp, uint16_t *rsp_len, int ms)
{
    return mgmt_request(&M, op, (uint16_t)g_index, p, len, rsp, rsp_len, ms);
}

static int set_mode(uint16_t op, uint8_t value)
{
    return req(op, &value, 1, NULL, NULL, 5000);
}

struct ctl_info {
    bdaddr_t addr;
    uint32_t supported, current;
    char name[249];
};

static int read_info(struct ctl_info *ci)
{
    uint8_t rsp[300];
    uint16_t len = sizeof(rsp);
    int st = req(MGMT_OP_READ_INFO, NULL, 0, rsp, &len, 3000);
    if (st != 0 || len < 280) return -1;
    memcpy(ci->addr.b, rsp, 6);
    ci->supported = get_le32(rsp + 9);
    ci->current = get_le32(rsp + 13);
    clean_name((const char *)rsp + 20, 249, ci->name, sizeof(ci->name));
    return 0;
}

/* Opens the management socket and finds the controller. With need_power,
 * also insists that Bluetooth is switched on. Exits with a clear message. */
static void open_ready(int need_power, struct ctl_info *ci_out)
{
    struct ctl_info ci;
    if (mgmt_open(&M) < 0) {
        if (errno == EAFNOSUPPORT || errno == ENOENT || errno == ECONNREFUSED)
            printf("error Bluetooth is not loaded (run nzeus-bt-up.sh)\n");
        else
            printf("error cannot open Bluetooth: %s\n", strerror(errno));
        exit(1);
    }
    g_index = mgmt_pick_index(&M);
    if (g_index < 0) {
        printf("error no Bluetooth controller found (is the dongle plugged in and btusb loaded?)\n");
        exit(1);
    }
    if (read_info(&ci) < 0) {
        printf("error cannot read the controller's information\n");
        exit(1);
    }
    io_set_local_addr(&ci.addr);          /* our data channels start from this controller */
    if (need_power && !(ci.current & MGMT_SETTING_POWERED)) {
        printf("error Bluetooth is off (run: nzeus-bt on)\n");
        exit(1);
    }
    if (ci_out) *ci_out = ci;
}

static int parse_addr_arg(const char *s, bdaddr_t *a)
{
    if (!s || addr_parse(s, a) != 0) {
        printf("error not a Bluetooth address: %s (expected AA:BB:CC:DD:EE:FF)\n", s ? s : "(none)");
        return -1;
    }
    return 0;
}

static int daemon_pid(void)
{
    FILE *f = fopen(io_runfile("daemon.pid"), "r");
    int pid = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid > 0 && kill(pid, 0) == 0) return pid;
    return 0;
}

/* Ask the background daemon to look at the connections again (LE devices). */
static int poke_daemon(void)
{
    int pid = daemon_pid();
    if (pid > 0 && kill(pid, SIGUSR1) == 0) return 0;
    return -1;
}

/* Which connections are up right now (fills addrs, returns count). */
static int get_connections(bdaddr_t *addrs, uint8_t *types, int max)
{
    uint8_t rsp[2 + 7 * 64];
    uint16_t len = sizeof(rsp);
    int n, i;
    if (req(MGMT_OP_GET_CONNECTIONS, NULL, 0, rsp, &len, 3000) != 0 || len < 2) return 0;
    n = get_le16(rsp);
    if (n > max) n = max;
    for (i = 0; i < n && 2 + 7 * (i + 1) <= len; i++) {
        memcpy(addrs[i].b, rsp + 2 + 7 * i, 6);
        if (types) types[i] = rsp[2 + 7 * i + 6];
    }
    return i;
}

static int is_le(const struct bt_device *d) { return d->addr_type != ADDR_BREDR; }

/* ------------------------------------------------------------------ */
/* the last scan, kept in /tmp so "pair" knows the device's type/name  */
/* ------------------------------------------------------------------ */
struct found {
    bdaddr_t addr;
    uint8_t type;            /* ADDR_* */
    int both;                /* seen as classic AND LE (same address) */
    int8_t rssi;
    uint32_t cod; int has_cod;
    uint16_t appearance; int has_app;
    int hid_uuid;
    char name[249]; int name_complete;
    /* for "survey" (the deck's "what is this device?") */
    int has_mfr; uint16_t company; uint8_t mfr[31]; uint8_t mfr_len;
    int n_uuid; uint16_t uuid[8];
    int has_tx; int8_t tx_power;
};
#define MAX_FOUND 64
static struct found g_found[MAX_FOUND];
static int g_nfound;

static const char *found_kind(const struct found *f)
{
    if (f->has_cod) return kind_from_class(f->cod);
    if (f->has_app) return kind_from_appearance(f->appearance);
    return f->hid_uuid ? "input" : "other";
}

static int found_is_hid(const struct found *f)
{
    if (f->has_cod && class_is_hid(f->cod)) return 1;
    if (f->has_app && appearance_is_hid(f->appearance)) return 1;
    return f->hid_uuid;
}

static void save_scan_cache(void)
{
    FILE *f = fopen(io_runfile("scan"), "w");
    int i;
    char a[18];
    if (!f) return;
    for (i = 0; i < g_nfound; i++) {
        addr_str(&g_found[i].addr, a);
        fprintf(f, "%s %u %d 0x%06x %d 0x%04x %d %s\n", a, g_found[i].type,
                g_found[i].has_cod, (unsigned)g_found[i].cod, g_found[i].has_app,
                g_found[i].appearance, g_found[i].hid_uuid, g_found[i].name);
    }
    fclose(f);
}

static int load_scan_cache(const bdaddr_t *want, struct found *out)
{
    FILE *f = fopen(io_runfile("scan"), "r");
    char line[400];
    int ok = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char a[18], name[300] = "";
        unsigned type, cod, app;
        int hc, ha, hu, n = 0;
        bdaddr_t ad;
        if (sscanf(line, "%17s %u %d %x %d %x %d %n", a, &type, &hc, &cod, &ha, &app, &hu, &n) < 7)
            continue;
        if (addr_parse(a, &ad) != 0 || !addr_eq(&ad, want)) continue;
        memset(out, 0, sizeof(*out));
        out->addr = ad; out->type = type; out->has_cod = hc; out->cod = cod;
        out->has_app = ha; out->appearance = app; out->hid_uuid = hu;
        if (n > 0) {
            snprintf(name, sizeof(name), "%s", line + n);
            name[strcspn(name, "\r\n")] = 0;
        }
        snprintf(out->name, sizeof(out->name), "%s", name);
        ok = 0;
        break;
    }
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ */
/* saving new pairing keys (both "pair" and the daemon receive them)   */
/* ------------------------------------------------------------------ */
static struct found g_pair_info;     /* what "pair" knows about the device */
static int g_pair_info_valid;

/* loads (or starts) the device's record, lets fn change it, saves it */
static void update_device(const bdaddr_t *a, uint8_t type, void (*fn)(struct bt_device *, const void *),
                          const void *arg)
{
    static struct bt_device d;
    int lock = store_lock();
    if (store_load(a, &d) != 0) {
        memset(&d, 0, sizeof(d));
        d.addr = *a;
        d.addr_type = type;
        if (g_pair_info_valid && addr_eq(&g_pair_info.addr, a)) {
            snprintf(d.name, sizeof(d.name), "%s", g_pair_info.name);
            snprintf(d.kind, sizeof(d.kind), "%s", found_kind(&g_pair_info));
            d.cod = g_pair_info.cod;
            d.appearance = g_pair_info.appearance;
        }
    }
    fn(&d, arg);
    if (store_save(&d) != 0) printf("error cannot save the pairing key in %s\n", store_dir());
    if (lock >= 0) close(lock);
}

static void set_linkkey(struct bt_device *d, const void *arg)
{
    const uint8_t *k = arg;              /* type(1) val(16) pin_len(1) */
    d->has_linkkey = 1;
    d->linkkey_type = k[0];
    memcpy(d->linkkey, k + 1, 16);
    d->pin_len = k[17];
}

static void set_ltk(struct bt_device *d, const void *arg)
{
    const uint8_t *k = arg;              /* type master enc_size ediv(2) rand(8) val(16) */
    struct bt_ltk n;
    int i, slot = -1;
    n.type = k[0]; n.master = k[1] ? 1 : 0; n.enc_size = k[2];
    n.ediv = get_le16(k + 3);
    n.rand = (uint64_t)get_le32(k + 5) | (uint64_t)get_le32(k + 9) << 32;
    memcpy(n.val, k + 13, 16);
    /* one key per role (master/slave): replace the old one of the same role */
    for (i = 0; i < d->n_ltk; i++) if (d->ltk[i].master == n.master) slot = i;
    if (slot < 0) slot = d->n_ltk < 2 ? d->n_ltk++ : 0;
    d->ltk[slot] = n;
}

static void set_irk(struct bt_device *d, const void *arg)
{
    d->has_irk = 1;
    memcpy(d->irk, arg, 16);
}

/* Events every command handles the same way. Returns 1 if it handled it. */
static int common_event(uint16_t ev, const uint8_t *p, uint16_t len)
{
    bdaddr_t a;
    char s[18];
    switch (ev) {
    case MGMT_EV_NEW_LINK_KEY:          /* store_hint(1) addr(7) type(1) val(16) pin_len(1) */
        if (len < 26) return 1;
        memcpy(a.b, p + 1, 6);
        if (!p[0]) { dbg("link key not meant to be stored"); return 1; }
        update_device(&a, p[7], set_linkkey, p + 8);
        addr_str(&a, s);
        dbg("saved link key for %s", s);
        return 1;
    case MGMT_EV_NEW_LONG_TERM_KEY:     /* store_hint(1) addr(7) type master enc ediv(2) rand(8) val(16) */
        if (len < 37) return 1;
        memcpy(a.b, p + 1, 6);
        if (!p[0]) return 1;
        update_device(&a, p[7], set_ltk, p + 8);
        return 1;
    case MGMT_EV_NEW_IRK:               /* store_hint(1) rpa(6) addr(7) val(16) */
        if (len < 30) return 1;
        memcpy(a.b, p + 7, 6);
        if (!p[0]) return 1;
        update_device(&a, p[13], set_irk, p + 14);
        return 1;
    }
    return 0;
}

static void quiet_event(struct mgmt *m, uint16_t ev, uint16_t index, const uint8_t *p, uint16_t len)
{
    (void)m; (void)index;
    common_event(ev, p, len);
}

/* ------------------------------------------------------------------ */
/* on / off                                                           */
/* ------------------------------------------------------------------ */
static void load_keys(struct bt_device *devs, int n)
{
    static uint8_t buf[3 + 25 * 64];
    int i, count = 0, st;
    char a[18];
    /* classic link keys (all at once; this replaces the kernel's list) */
    for (i = 0; i < n && count < 64; i++) {
        uint8_t *k = buf + 3 + 25 * count;
        if (!devs[i].has_linkkey) continue;
        put_addr(k, &devs[i].addr, ADDR_BREDR);
        k[7] = devs[i].linkkey_type;
        memcpy(k + 8, devs[i].linkkey, 16);
        k[24] = devs[i].pin_len;
        count++;
    }
    buf[0] = 0;                              /* no debug keys */
    put_le16(buf + 1, (uint16_t)count);
    st = req(MGMT_OP_LOAD_LINK_KEYS, buf, (uint16_t)(3 + 25 * count), NULL, NULL, 5000);
    if (st) printf("warning loading classic keys failed: %s\n", mgmt_status_str(st));
    /* LE long term keys */
    count = 0;
    for (i = 0; i < n; i++) {
        int j;
        for (j = 0; j < devs[i].n_ltk && count < 64; j++) {
            uint8_t *k = buf + 2 + 36 * count;
            const struct bt_ltk *l = &devs[i].ltk[j];
            if (!is_le(&devs[i])) continue;
            put_addr(k, &devs[i].addr, devs[i].addr_type);
            k[7] = l->type; k[8] = l->master; k[9] = l->enc_size;
            put_le16(k + 10, l->ediv);
            put_le32(k + 12, (uint32_t)l->rand);
            put_le32(k + 16, (uint32_t)(l->rand >> 32));
            memcpy(k + 20, l->val, 16);
            count++;
        }
    }
    put_le16(buf, (uint16_t)count);
    st = req(MGMT_OP_LOAD_LONG_TERM_KEYS, buf, (uint16_t)(2 + 36 * count), NULL, NULL, 5000);
    if (st && st != 0x0c) printf("warning loading LE keys failed: %s\n", mgmt_status_str(st));
    /* LE identity keys (for devices that change their address) */
    count = 0;
    for (i = 0; i < n && count < 64; i++) {
        uint8_t *k = buf + 2 + 23 * count;
        if (!devs[i].has_irk || !is_le(&devs[i])) continue;
        put_addr(k, &devs[i].addr, devs[i].addr_type);
        memcpy(k + 7, devs[i].irk, 16);
        count++;
    }
    put_le16(buf, (uint16_t)count);
    st = req(MGMT_OP_LOAD_IRKS, buf, (uint16_t)(2 + 23 * count), NULL, NULL, 5000);
    if (st && st != 0x0c) printf("warning loading LE identity keys failed: %s\n", mgmt_status_str(st));
    for (i = 0; i < n; i++) {
        addr_str(&devs[i].addr, a);
        dbg("loaded %s (%s)", a, devs[i].name);
    }
}

/* Lets a paired device reconnect by itself: classic = may connect to us,
 * LE = the kernel connects whenever it sees the device advertise. */
static int allow_device(const struct bt_device *d)
{
    uint8_t p[8];
    put_addr(p, &d->addr, d->addr_type);
    p[7] = is_le(d) ? 0x02 : 0x01;
    return req(MGMT_OP_ADD_DEVICE, p, 8, NULL, NULL, 5000);
}

static int cmd_on(void)
{
    struct ctl_info ci;
    struct bt_device *devs;
    uint8_t name[260];
    int n, i, st, waited;
    char a[18];

    if (mgmt_open(&M) < 0) {
        printf("error Bluetooth is not loaded (run nzeus-bt-up.sh)\n");
        return 1;
    }
    M.on_event = quiet_event;
    /* the controller may need a moment after btusb loads */
    for (waited = 0; (g_index = mgmt_pick_index(&M)) < 0 && waited < 10; waited++)
        usleep(500 * 1000);
    if (g_index < 0) {
        printf("error no Bluetooth controller found (is the dongle plugged in and btusb loaded?)\n");
        return 1;
    }
    if (read_info(&ci) < 0) { printf("error cannot read the controller's information\n"); return 1; }
    io_set_local_addr(&ci.addr);

    /* settings (all allowed while off; most also while on) */
    set_mode(MGMT_OP_SET_SSP, 1);                 /* modern "secure simple pairing" */
    set_mode(MGMT_OP_SET_BONDABLE, 1);            /* remember pairings */
    if (ci.supported & MGMT_SETTING_LE) set_mode(MGMT_OP_SET_LE, 1);
    if (ci.supported & MGMT_SETTING_SECURE_CONN) set_mode(MGMT_OP_SET_SECURE_CONN, 1);
    set_mode(MGMT_OP_SET_IO_CAPABILITY, IO_CAP_DISPLAY_YESNO);
    memset(name, 0, sizeof(name));
    snprintf((char *)name, 249, "NZEUS R36S");
    snprintf((char *)name + 249, 11, "NZEUS");
    req(MGMT_OP_SET_LOCAL_NAME, name, 260, NULL, NULL, 5000);
    {
        uint8_t cls[2] = { 0x01, 0x10 };          /* computer / handheld */
        req(MGMT_OP_SET_DEV_CLASS, cls, 2, NULL, NULL, 5000);
    }
    n = store_list(&devs);
    load_keys(devs, n);

    if (!(ci.current & MGMT_SETTING_POWERED)) {
        st = set_mode(MGMT_OP_SET_POWERED, 1);
        if (st) {
            printf("error switching on failed: %s (see dmesg: firmware rtl_bt/rtl8821c_fw.bin loaded?)\n",
                   mgmt_status_str(st));
            free(devs);
            return 1;
        }
    }
    for (i = 0; i < n; i++) {
        st = allow_device(&devs[i]);
        addr_str(&devs[i].addr, a);
        if (st) printf("warning %s: cannot allow reconnecting: %s\n", a, mgmt_status_str(st));
    }
    read_info(&ci);
    addr_str(&ci.addr, a);
    printf("on hci%d %s, %d paired device%s\n", g_index, a, n, n == 1 ? "" : "s");
    free(devs);
    return 0;
}

static int cmd_off(void)
{
    int st;
    open_ready(0, NULL);
    st = set_mode(MGMT_OP_SET_POWERED, 0);
    if (st) { printf("error switching off failed: %s\n", mgmt_status_str(st)); return 1; }
    printf("off\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* scan                                                               */
/* ------------------------------------------------------------------ */
static int g_discovering;

static struct found *found_slot(const bdaddr_t *a, uint8_t type)
{
    int i;
    for (i = 0; i < g_nfound; i++) {
        if (!addr_eq(&g_found[i].addr, a)) continue;
        /* same address seen as classic and as LE public = a "dual" device;
         * keep the classic entry (classic HID is what we connect) */
        if ((g_found[i].type == ADDR_BREDR) != (type == ADDR_BREDR)) {
            g_found[i].both = 1;
            if (type == ADDR_BREDR) g_found[i].type = ADDR_BREDR;
        }
        return &g_found[i];
    }
    if (g_nfound >= MAX_FOUND) return NULL;
    memset(&g_found[g_nfound], 0, sizeof(g_found[0]));
    g_found[g_nfound].addr = *a;
    g_found[g_nfound].type = type;
    g_found[g_nfound].rssi = -127;
    return &g_found[g_nfound++];
}

static void scan_event(struct mgmt *m, uint16_t ev, uint16_t index, const uint8_t *p, uint16_t len)
{
    (void)m; (void)index;
    if (common_event(ev, p, len)) return;
    if (ev == MGMT_EV_DISCOVERING && len >= 2) {
        g_discovering = p[1];
        return;
    }
    if (ev == MGMT_EV_DEVICE_FOUND && len >= 14) {
        /* addr(6) type(1) rssi(1) flags(4) eir_len(2) eir */
        bdaddr_t a;
        struct eir_info e;
        struct found *f;
        int i;
        uint32_t flags = get_le32(p + 8);
        uint16_t elen = get_le16(p + 12);
        memcpy(a.b, p, 6);
        if (14 + elen > len) elen = len - 14;
        eir_parse(p + 14, elen, &e);       /* a damaged tail still gives the good part */
        f = found_slot(&a, p[6]);
        if (f) {
            if ((int8_t)p[7] > f->rssi && (int8_t)p[7] != 0) f->rssi = (int8_t)p[7];
            if (e.has_name && (e.name_complete || !f->name_complete)) {
                snprintf(f->name, sizeof(f->name), "%s", e.name);
                f->name_complete = e.name_complete;
            }
            if (e.has_class) { f->has_cod = 1; f->cod = e.cod; }
            if (e.has_appearance) { f->has_app = 1; f->appearance = e.appearance; }
            if (e.has_hid_uuid) f->hid_uuid = 1;
            if (e.has_mfr && !f->has_mfr) {             /* the first maker data seen is kept */
                f->has_mfr = 1;
                f->company = e.company;
                f->mfr_len = e.mfr_len;
                memcpy(f->mfr, e.mfr, e.mfr_len);
            }
            for (i = 0; i < e.n_uuid; i++) {             /* services: each one once */
                int k, seen = 0;
                for (k = 0; k < f->n_uuid; k++) if (f->uuid[k] == e.uuid[i]) seen = 1;
                if (!seen && f->n_uuid < 8) f->uuid[f->n_uuid++] = e.uuid[i];
            }
            if (e.has_tx) { f->has_tx = 1; f->tx_power = e.tx_power; }
        }
        if (flags & MGMT_DEV_FOUND_CONFIRM_NAME) {
            /* tell the kernel whether to ask the device for its name */
            uint8_t cp[8];
            put_addr(cp, &a, p[6]);
            cp[7] = (f && f->name_complete) ? 1 : 0;    /* 1 = name known */
            mgmt_send(&M, MGMT_OP_CONFIRM_NAME, (uint16_t)g_index, cp, 8);
        }
    }
}

static int cmp_rssi(const void *x, const void *y)
{
    return ((const struct found *)y)->rssi - ((const struct found *)x)->rssi;
}

/* "survey": one line per device with everything it told us, for the deck's
 * ID cards (nzeus-gui/btinfo.c reads it):
 *   address  address-type  signal  tx-power  class  appearance  services  maker-data  name
 * (tab-separated; "-" = not sent; maker-data = "004c:0719010e20" = company:bytes) */
static void print_survey_line(const struct found *f)
{
    char a[18], uu[8 * 5 + 1] = "", md[4 + 1 + 31 * 2 + 1] = "";
    int i, k = 0;
    addr_str(&f->addr, a);
    for (i = 0; i < f->n_uuid; i++) k += snprintf(uu + k, sizeof(uu) - k, "%s%04x", i ? "," : "", f->uuid[i]);
    if (f->has_mfr) {
        k = snprintf(md, sizeof(md), "%04x:", f->company);
        for (i = 0; i < f->mfr_len; i++) k += snprintf(md + k, sizeof(md) - k, "%02x", f->mfr[i]);
    }
    printf("%s\t%s\t%d\t", a, f->both ? "dual" : addr_type_str(f->type), f->rssi);
    if (f->has_tx) printf("%d\t", f->tx_power); else printf("-\t");
    if (f->has_cod) printf("%06x\t", (unsigned)f->cod); else printf("-\t");
    if (f->has_app) printf("%04x\t", f->appearance); else printf("-\t");
    printf("%s\t%s\t%s\n", uu[0] ? uu : "-", md[0] ? md : "-", f->name[0] ? f->name : "(no name)");
}

static int cmd_scan(int seconds, int survey)
{
    struct ctl_info ci;
    uint8_t type;
    long long end;
    int st, i;
    open_ready(1, &ci);
    M.on_event = scan_event;
    /* 0x01 = classic, 0x06 = LE (public + random addresses) */
    type = (ci.current & MGMT_SETTING_LE) ? 0x07 : 0x01;
    end = now_ms() + seconds * 1000LL;
    st = req(MGMT_OP_START_DISCOVERY, &type, 1, NULL, NULL, 5000);
    if (st) { printf("error scan failed: %s\n", mgmt_status_str(st)); return 1; }
    g_discovering = 1;
    while (now_ms() < end) {
        if (mgmt_read_one(&M, (int)(end - now_ms())) < 0) break;
        /* one round of the kernel's scan lasts ~10 s; start another if time is left */
        if (!g_discovering && end - now_ms() > 1500) {
            if (req(MGMT_OP_START_DISCOVERY, &type, 1, NULL, NULL, 5000) == 0) g_discovering = 1;
            else usleep(200 * 1000);
        }
    }
    dbg("scan time over (discovering=%d)", g_discovering);
    if (g_discovering) req(MGMT_OP_STOP_DISCOVERY, &type, 1, NULL, NULL, 3000);
    qsort(g_found, g_nfound, sizeof(g_found[0]), cmp_rssi);
    save_scan_cache();
    dbg("scan: %d device(s)", g_nfound);
    for (i = 0; i < g_nfound; i++) {
        const struct found *f = &g_found[i];
        char a[18];
        const char *t = f->both ? "dual" : (f->type == ADDR_BREDR ? "classic" : "le");
        if (survey) { print_survey_line(f); continue; }
        addr_str(&f->addr, a);
        /* address, name, kind, type, signal (dBm), usable as keyboard/mouse */
        printf("%s\t%s\t%s\t%s\t%d\t%s\n", a, f->name[0] ? f->name : "(no name)", found_kind(f), t,
               f->rssi, found_is_hid(f) ? "yes" : "no");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* pair                                                               */
/* ------------------------------------------------------------------ */
static const char *g_pin_override;
static int g_last_entered = -1;

static int random_digits(char *out, int n)
{
    uint8_t r[16];
    int fd = open("/dev/urandom", O_RDONLY), i;
    if (fd < 0 || read(fd, r, n) != n) {
        unsigned seed = (unsigned)now_ms() ^ (unsigned)getpid();
        for (i = 0; i < n; i++) r[i] = (uint8_t)rand_r(&seed);
    }
    if (fd >= 0) close(fd);
    for (i = 0; i < n; i++) out[i] = '0' + r[i] % 10;
    out[n] = 0;
    return 0;
}

/* read a number typed by the user (or written by the Settings app) on stdin */
static int read_number(long *out, int timeout_ms)
{
    struct pollfd p = { 0, POLLIN, 0 };
    char line[64];
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
    if (!fgets(line, sizeof(line), stdin)) return -1;
    *out = strtol(line, NULL, 10);
    return 0;
}

static void pair_event(struct mgmt *m, uint16_t ev, uint16_t index, const uint8_t *p, uint16_t len)
{
    uint8_t cp[32];
    (void)m; (void)index;
    if (common_event(ev, p, len)) return;
    switch (ev) {
    case MGMT_EV_PASSKEY_NOTIFY:        /* addr(7) passkey(4) entered(1) */
        if (len < 12) break;
        if (g_last_entered < 0)
            printf("passkey %06u - type it on the keyboard, then press Enter\n", get_le32(p + 7));
        else if (p[11] != g_last_entered)
            printf("typed %u\n", p[11]);
        g_last_entered = p[11];
        fflush(stdout);
        break;
    case MGMT_EV_USER_CONFIRM_REQUEST:  /* addr(7) confirm_hint(1) value(4) */
        if (len < 12) break;
        if (p[7]) printf("confirm - accepting the device (it has no display)\n");
        else printf("confirm %06u - accepted (the device should show the same number)\n", get_le32(p + 8));
        fflush(stdout);
        memcpy(cp, p, 7);
        mgmt_send(&M, MGMT_OP_USER_CONFIRM_REPLY, (uint16_t)g_index, cp, 7);
        break;
    case MGMT_EV_USER_PASSKEY_REQUEST: {/* addr(7): the DEVICE shows a number, we type it */
        long v;
        if (len < 7) break;
        printf("enter-passkey - type the number the device shows, then Enter\n");
        fflush(stdout);
        memcpy(cp, p, 7);
        if (read_number(&v, 60000) == 0 && v >= 0 && v <= 999999) {
            put_le32(cp + 7, (uint32_t)v);
            mgmt_send(&M, MGMT_OP_USER_PASSKEY_REPLY, (uint16_t)g_index, cp, 11);
        } else {
            mgmt_send(&M, MGMT_OP_USER_PASSKEY_NEG_REPLY, (uint16_t)g_index, cp, 7);
        }
        break;
    }
    case MGMT_EV_PIN_CODE_REQUEST: {    /* addr(7) secure(1): old (pre-2007) devices */
        char pin[17];
        int is_kb = g_pair_info_valid && strstr(found_kind(&g_pair_info), "keyboard") != NULL;
        if (len < 8) break;
        if (g_pin_override) snprintf(pin, sizeof(pin), "%s", g_pin_override);
        else if (p[7]) random_digits(pin, 16);           /* device wants a 16-digit PIN */
        else if (is_kb) random_digits(pin, 6);
        else snprintf(pin, sizeof(pin), "0000");         /* usual fixed PIN of mice */
        if (is_kb || g_pin_override || p[7])
            printf("pin %s - type it on the keyboard, then press Enter\n", pin);
        else
            printf("pin %s - using the usual PIN for devices without keys\n", pin);
        fflush(stdout);
        memset(cp, 0, sizeof(cp));
        memcpy(cp, p, 7);
        cp[7] = (uint8_t)strlen(pin);
        memcpy(cp + 8, pin, strlen(pin));
        mgmt_send(&M, MGMT_OP_PIN_CODE_REPLY, (uint16_t)g_index, cp, 24);
        break;
    }
    case MGMT_EV_AUTH_FAILED:           /* addr(7) status(1) */
        if (len >= 8) dbg("authentication failed: %s", mgmt_status_str(p[7]));
        break;
    }
}

static void set_hid_info(struct bt_device *d, const void *arg)
{
    const struct bt_device *src = arg;
    if (!d->name[0] && src->name[0]) snprintf(d->name, sizeof(d->name), "%s", src->name);
    if (src->kind[0]) snprintf(d->kind, sizeof(d->kind), "%s", src->kind);
    if (src->cod) d->cod = src->cod;
    if (src->appearance) d->appearance = src->appearance;
    if (src->has_hid) {
        d->has_hid = 1;
        d->hid_vendor = src->hid_vendor; d->hid_product = src->hid_product;
        d->hid_version = src->hid_version; d->hid_vendor_src = src->hid_vendor_src;
        d->hid_parser = src->hid_parser; d->hid_country = src->hid_country;
        d->hid_subclass = src->hid_subclass;
        d->rd_size = src->rd_size;
        memcpy(d->rd, src->rd, src->rd_size);
    }
}

static void unpair_kernel(const bdaddr_t *a, uint8_t type)
{
    uint8_t cp[8];
    put_addr(cp, a, type);
    cp[7] = 1;                                     /* also disconnect */
    req(MGMT_OP_UNPAIR_DEVICE, cp, 8, NULL, NULL, 5000);
    put_addr(cp, a, type);
    req(MGMT_OP_REMOVE_DEVICE, cp, 7, NULL, NULL, 5000);
}

static int cmd_pair(const char *addr_s, const char *pin)
{
    static struct bt_device d, old;
    bdaddr_t a;
    uint8_t cp[8], rsp[16];
    uint16_t rlen = sizeof(rsp);
    char s[18];
    int st;
    if (parse_addr_arg(addr_s, &a) < 0) return 2;
    g_pin_override = pin;
    open_ready(1, NULL);
    M.on_event = pair_event;
    addr_str(&a, s);
    memset(&g_pair_info, 0, sizeof(g_pair_info));
    if (load_scan_cache(&a, &g_pair_info) == 0) g_pair_info_valid = 1;
    else { g_pair_info.addr = a; g_pair_info.type = ADDR_BREDR; g_pair_info_valid = 1; }

    /* "pair" always pairs fresh: forget an old pairing first */
    if (store_load(&a, &old) == 0) {
        unpair_kernel(&a, old.addr_type);
        hid_detach(&a);
        store_remove(&a);
    }
    set_mode(MGMT_OP_SET_IO_CAPABILITY, IO_CAP_DISPLAY_YESNO);
    printf("pairing %s (%s)...\n", s, g_pair_info.type == ADDR_BREDR ? "classic" : "LE");
    fflush(stdout);
    put_addr(cp, &a, g_pair_info.type);
    cp[7] = IO_CAP_DISPLAY_YESNO;
    st = req(MGMT_OP_PAIR_DEVICE, cp, 8, rsp, &rlen, 90000);
    if (st == 0x13) st = 0;                        /* "already paired" (kernel kept a key) */
    if (st) {
        printf("error pairing failed: %s\n", mgmt_status_str(st));
        return 1;
    }
    printf("paired\n");
    fflush(stdout);

    /* collect what we know and save it next to the key */
    memset(&d, 0, sizeof(d));
    d.addr = a;
    d.addr_type = g_pair_info.type;
    snprintf(d.name, sizeof(d.name), "%s", g_pair_info.name);
    snprintf(d.kind, sizeof(d.kind), "%s", found_kind(&g_pair_info));
    d.cod = g_pair_info.cod;
    d.appearance = g_pair_info.appearance;
    if (d.addr_type == ADDR_BREDR) {
        if (hid_fetch_sdp(&a, &d, 10000) != 0)
            printf("warning the device sent no keyboard/mouse description (connect will try the simple 'boot' mode)\n");
        else if (!strcmp(d.kind, "other") || !strcmp(d.kind, "input")) {
            /* SDP subclass: bit 6 keyboard, bit 7 mouse */
            if ((d.hid_subclass & 0xc0) == 0xc0) snprintf(d.kind, sizeof(d.kind), "keyboard+mouse");
            else if (d.hid_subclass & 0x40) snprintf(d.kind, sizeof(d.kind), "keyboard");
            else if (d.hid_subclass & 0x80) snprintf(d.kind, sizeof(d.kind), "mouse");
        }
    }
    update_device(&a, d.addr_type, set_hid_info, &d);
    if (store_load(&a, &d) != 0 || (!d.has_linkkey && d.n_ltk == 0))
        printf("warning the kernel gave no pairing key to store: the device must pair again after a restart\n");
    allow_device(&d);

    if (d.addr_type == ADDR_BREDR) {
        if (hid_connect(&d, 10000) == 0 || errno == EEXIST) printf("connected\n");
        else { printf("error connecting failed: %s\n", strerror(errno)); return 1; }
    } else {
        if (poke_daemon() == 0) printf("connecting - the Bluetooth service attaches the device\n");
        else printf("warning the Bluetooth service is not running (run nzeus-bt-up.sh) - needed for LE devices\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* connect / disconnect / remove / list / status                      */
/* ------------------------------------------------------------------ */
static int load_paired(const char *addr_s, struct bt_device *d)
{
    bdaddr_t a;
    if (parse_addr_arg(addr_s, &a) < 0) return 2;
    if (store_load(&a, d) != 0) {
        printf("error %s is not paired (run: nzeus-bt pair %s)\n", addr_s, addr_s);
        return 1;
    }
    return 0;
}

static int cmd_connect(const char *addr_s)
{
    static struct bt_device d;
    int r = load_paired(addr_s, &d);
    if (r) return r;
    open_ready(1, NULL);
    M.on_event = quiet_event;
    if (is_le(&d)) {
        allow_device(&d);
        if (poke_daemon() == 0) printf("connecting - the device is attached as soon as it wakes up\n");
        else { printf("error the Bluetooth service is not running (run nzeus-bt-up.sh)\n"); return 1; }
        return 0;
    }
    if (hid_is_attached(&d.addr)) { printf("connected (already)\n"); return 0; }
    if (!d.rd_size && hid_fetch_sdp(&d.addr, &d, 10000) == 0) {
        int lock = store_lock();
        store_save(&d);
        if (lock >= 0) close(lock);
    }
    if (hid_connect(&d, 10000) == 0 || errno == EEXIST) { printf("connected\n"); return 0; }
    printf("error connecting failed: %s (is the device switched on and awake?)\n", strerror(errno));
    return 1;
}

static int cmd_disconnect(const char *addr_s)
{
    static struct bt_device d;
    uint8_t cp[7];
    int r = load_paired(addr_s, &d);
    if (r) return r;
    open_ready(0, NULL);
    M.on_event = quiet_event;
    if (is_le(&d)) {          /* stop the kernel from reconnecting it right away */
        put_addr(cp, &d.addr, d.addr_type);
        req(MGMT_OP_REMOVE_DEVICE, cp, 7, NULL, NULL, 5000);
    } else {
        hid_detach(&d.addr);
    }
    put_addr(cp, &d.addr, d.addr_type);
    req(MGMT_OP_DISCONNECT, cp, 7, NULL, NULL, 5000);
    printf("disconnected\n");
    return 0;
}

static int cmd_remove(const char *addr_s)
{
    static struct bt_device d;
    bdaddr_t a;
    int known;
    if (parse_addr_arg(addr_s, &a) < 0) return 2;
    known = store_load(&a, &d) == 0;
    open_ready(0, NULL);
    M.on_event = quiet_event;
    unpair_kernel(&a, known ? d.addr_type : ADDR_BREDR);
    if (!known || !is_le(&d)) hid_detach(&a);
    if (store_remove(&a) == 0 || known) printf("removed\n");
    else printf("removed (was not paired)\n");
    return 0;
}

static int cmd_list(void)
{
    struct bt_device *devs;
    struct mgmt probe;
    bdaddr_t conns[64];
    int n = store_list(&devs), nc = 0, i, j;
    /* connection states, if Bluetooth is up (list also works when it isn't) */
    if (mgmt_open(&probe) == 0) {
        M = probe;
        M.on_event = quiet_event;
        g_index = mgmt_pick_index(&M);
        if (g_index >= 0) nc = get_connections(conns, NULL, 64);
    }
    for (i = 0; i < n; i++) {
        char a[18];
        int c = 0;
        for (j = 0; j < nc; j++) if (addr_eq(&conns[j], &devs[i].addr)) c = 1;
        addr_str(&devs[i].addr, a);
        printf("%s\t%s\t%s\t%s\t%s\n", a, devs[i].name[0] ? devs[i].name : "(no name)",
               devs[i].kind[0] ? devs[i].kind : "other", is_le(&devs[i]) ? "le" : "classic",
               c ? "connected" : "not-connected");
    }
    free(devs);
    return 0;
}

static int cmd_status(void)
{
    struct ctl_info ci;
    struct bt_device *devs;
    bdaddr_t conns[64];
    char a[18];
    int n = store_list(&devs);
    free(devs);
    if (mgmt_open(&M) < 0) {
        printf("loaded=no\npowered=no\npaired=%d\n", n);
        return 0;
    }
    g_index = mgmt_pick_index(&M);
    if (g_index < 0 || read_info(&ci) < 0) {
        printf("loaded=yes\ncontroller=none\npowered=no\npaired=%d\n", n);
        return 0;
    }
    addr_str(&ci.addr, a);
    printf("loaded=yes\ncontroller=hci%d\naddress=%s\nname=%s\n", g_index, a, ci.name);
    printf("powered=%s\n", ci.current & MGMT_SETTING_POWERED ? "yes" : "no");
    printf("le=%s\n", ci.current & MGMT_SETTING_LE ? "yes" : "no");
    printf("paired=%d\n", n);
    printf("connected=%d\n", ci.current & MGMT_SETTING_POWERED ? get_connections(conns, NULL, 64) : 0);
    printf("service=%s\n", daemon_pid() ? "running" : "stopped");
    return 0;
}

/* ------------------------------------------------------------------ */
/* daemon: lets paired keyboards/mice come back by themselves          */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_poke, g_stop;
static void on_signal(int sig)
{
    if (sig == SIGUSR1) g_poke = 1;
    else g_stop = 1;
}

struct pend { int fd; bdaddr_t addr; long long since; };
struct child { pid_t pid; bdaddr_t addr; };
#define MAX_PEND 8
#define MAX_CHILD 8
static struct pend g_pend[MAX_PEND];
static struct child g_child[MAX_CHILD];
static int g_lfd_ctrl = -1, g_lfd_intr = -1;

static void log_line(const char *fmt, const char *addr, const char *text)
{
    printf(fmt, addr, text);
    fflush(stdout);
}

static long long g_recheck_at;   /* look at LE connections again at this time (0 = no) */

/* start the LE keyboard/mouse helper process for d (if not running yet).
 * new_link = the kernel just reported a NEW connection: an old helper for
 * the same device may still be finishing; then look again in a second. */
static void start_hogp(const struct bt_device *d, int new_link)
{
    int i, slot = -1;
    pid_t pid;
    char a[18];
    for (i = 0; i < MAX_CHILD; i++) {
        if (g_child[i].pid > 0 && addr_eq(&g_child[i].addr, &d->addr)) {
            if (waitpid(g_child[i].pid, NULL, WNOHANG) == 0) {     /* still running */
                if (new_link) g_recheck_at = now_ms() + 1000;
                return;
            }
            g_child[i].pid = 0;                                    /* it just ended */
        }
    }
    for (i = 0; i < MAX_CHILD; i++)
        if (g_child[i].pid <= 0 && slot < 0) slot = i;
    if (slot < 0) return;
    addr_str(&d->addr, a);
    log_line("%s: %s\n", a, "LE device connected, attaching it");
    pid = fork();
    if (pid == 0) {
        mgmt_close(&M);
        if (g_lfd_ctrl >= 0) close(g_lfd_ctrl);
        if (g_lfd_intr >= 0) close(g_lfd_intr);
        signal(SIGTERM, SIG_DFL);
        _exit(hogp_run(d));
    }
    if (pid > 0) { g_child[slot].pid = pid; g_child[slot].addr = d->addr; }
}

static void check_le_connections(void)
{
    bdaddr_t conns[64];
    uint8_t types[64];
    int n = get_connections(conns, types, 64), i;
    static struct bt_device d;
    for (i = 0; i < n; i++)
        if (types[i] != ADDR_BREDR && store_load(&conns[i], &d) == 0 && is_le(&d))
            start_hogp(&d, 0);
}

static void daemon_event(struct mgmt *m, uint16_t ev, uint16_t index, const uint8_t *p, uint16_t len)
{
    static struct bt_device d;
    bdaddr_t a;
    (void)m; (void)index;
    if (common_event(ev, p, len)) return;
    if (ev == MGMT_EV_INDEX_REMOVED) {
        /* the dongle was unplugged: our controller is gone. Stop, so that
         * nzeus-bt-up.sh can start a fresh service for the new controller. */
        printf("controller removed (dongle unplugged?) - stopping\n");
        g_stop = 1;
        return;
    }
    if (ev == MGMT_EV_DEVICE_CONNECTED && len >= 7 && p[6] != ADDR_BREDR) {
        memcpy(a.b, p, 6);
        if (store_load(&a, &d) == 0 && is_le(&d)) start_hogp(&d, 1);
    }
}

static void handle_accept(int lfd, int is_intr)
{
    static struct bt_device d;
    bdaddr_t peer;
    char a[18];
    int fd = io_l2cap_accept(lfd, &peer), i;
    if (fd < 0) return;
    addr_str(&peer, a);
    if (store_load(&peer, &d) != 0 || is_le(&d)) {     /* only devices we paired */
        log_line("%s: %s\n", a, "refused (not paired)");
        close(fd);
        return;
    }
    if (!is_intr) {                    /* control channel comes first: keep it */
        for (i = 0; i < MAX_PEND; i++)
            if (g_pend[i].fd >= 0 && addr_eq(&g_pend[i].addr, &peer)) { close(g_pend[i].fd); g_pend[i].fd = -1; }
        for (i = 0; i < MAX_PEND; i++)
            if (g_pend[i].fd < 0) {
                g_pend[i].fd = fd; g_pend[i].addr = peer; g_pend[i].since = now_ms();
                return;
            }
        close(fd);
        return;
    }
    for (i = 0; i < MAX_PEND; i++) {   /* interrupt channel: pair it with its control channel */
        if (g_pend[i].fd < 0 || !addr_eq(&g_pend[i].addr, &peer)) continue;
        if (hid_attach(g_pend[i].fd, fd, &d) == 0) log_line("%s: %s\n", a, "reconnected");
        else { char e[96]; snprintf(e, sizeof(e), "attach failed: %s", strerror(errno)); log_line("%s: %s\n", a, e); }
        close(g_pend[i].fd);
        g_pend[i].fd = -1;
        close(fd);
        return;
    }
    close(fd);
}

/* at start: call every paired classic keyboard/mouse once (in a child
 * process, so the daemon can already accept devices that call us) */
static void reconnect_all_classic(void)
{
    struct bt_device *devs;
    int n, i;
    pid_t pid = fork();
    if (pid != 0) return;
    mgmt_close(&M);
    if (g_lfd_ctrl >= 0) close(g_lfd_ctrl);
    if (g_lfd_intr >= 0) close(g_lfd_intr);
    n = store_list(&devs);
    for (i = 0; i < n; i++) {
        char a[18];
        if (is_le(&devs[i]) || !devs[i].has_linkkey || hid_is_attached(&devs[i].addr)) continue;
        addr_str(&devs[i].addr, a);
        if (hid_connect(&devs[i], 5000) == 0) log_line("%s: %s\n", a, "connected");
        else log_line("%s: %s\n", a, "not reachable now (it will connect when you press a key)");
    }
    _exit(0);
}

static int cmd_daemon(void)
{
    struct sigaction sa;
    FILE *pf;
    int i, other = daemon_pid();
    if (other) { printf("service already running (pid %d)\n", other); return 0; }
    open_ready(0, NULL);
    M.on_event = daemon_event;
    pf = fopen(io_runfile("daemon.pid"), "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;               /* no SA_RESTART: poll() wakes up */
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < MAX_PEND; i++) g_pend[i].fd = -1;
    g_lfd_ctrl = io_l2cap_listen(PSM_HID_CTRL, BT_SEC_MEDIUM);
    g_lfd_intr = io_l2cap_listen(PSM_HID_INTR, BT_SEC_MEDIUM);
    if (g_lfd_ctrl < 0 || g_lfd_intr < 0)
        printf("warning cannot wait for keyboards to reconnect: %s\n", strerror(errno));
    /* every log line goes to the END of the log file, even if the file is
     * shared with helper processes or someone empties it meanwhile */
    fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_APPEND);
    printf("service started (pid %d)\n", (int)getpid());
    fflush(stdout);
    reconnect_all_classic();
    check_le_connections();

    while (!g_stop) {
        struct pollfd p[3 + MAX_PEND];
        int np = 0, r;
        pid_t pid;
        p[np].fd = M.fd; p[np].events = POLLIN; p[np].revents = 0; np++;
        p[np].fd = g_lfd_ctrl; p[np].events = POLLIN; p[np].revents = 0; np++;
        p[np].fd = g_lfd_intr; p[np].events = POLLIN; p[np].revents = 0; np++;
        r = poll(p, np, 1000);
        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)      /* finished helpers */
            for (i = 0; i < MAX_CHILD; i++) if (g_child[i].pid == pid) g_child[i].pid = 0;
        for (i = 0; i < MAX_PEND; i++)                         /* half-open, too old */
            if (g_pend[i].fd >= 0 && now_ms() - g_pend[i].since > 10000) { close(g_pend[i].fd); g_pend[i].fd = -1; }
        if (g_recheck_at && now_ms() >= g_recheck_at) { g_recheck_at = 0; g_poke = 1; }
        if (g_poke) { g_poke = 0; check_le_connections(); }
        if (r <= 0) continue;
        if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { printf("management socket closed\n"); break; }
        if (p[0].revents & POLLIN) mgmt_read_one(&M, 0);
        if (p[1].revents & POLLIN) handle_accept(g_lfd_ctrl, 0);
        if (p[2].revents & POLLIN) handle_accept(g_lfd_intr, 1);
    }
    for (i = 0; i < MAX_CHILD; i++) if (g_child[i].pid > 0) kill(g_child[i].pid, SIGTERM);
    unlink(io_runfile("daemon.pid"));
    printf("service stopped\n");
    return 0;
}

static int cmd_hogp(const char *addr_s)
{
    static struct bt_device d;
    int r = load_paired(addr_s, &d);
    if (r) return r;
    if (!is_le(&d)) { printf("error %s is a classic device (use connect)\n", addr_s); return 1; }
    return hogp_run(&d);
}

static void usage(void)
{
    printf("nzeus-bt %s - Bluetooth keyboards and mice for NZEUS\n"
           "usage: nzeus-bt [-v] COMMAND\n"
           "  on                  switch Bluetooth on (loads saved pairings)\n"
           "  off                 switch Bluetooth off\n"
           "  status              key=value lines: powered, address, paired, connected...\n"
           "  scan [SECONDS]      look for devices (default 10 s); one line per device:\n"
           "                      ADDRESS<tab>NAME<tab>KIND<tab>TYPE<tab>RSSI<tab>SUPPORTED\n"
           "  survey [SECONDS]    like scan, with everything each device told (for the deck's ID cards):\n"
           "                      ADDRESS TYPE RSSI TXPOWER CLASS APPEARANCE SERVICES MAKERDATA NAME (tabs, - = none)\n"
           "  pair ADDR [--pin N] pair + connect; prints 'passkey NNNNNN' to type on the keyboard\n"
           "  connect ADDR        connect a paired device\n"
           "  disconnect ADDR     disconnect it (stays paired)\n"
           "  list                paired devices: ADDRESS<tab>NAME<tab>KIND<tab>TYPE<tab>STATE\n"
           "  remove ADDR         forget a paired device\n"
           "  daemon              background service: reconnects paired devices, runs LE ones\n"
           "Pairings are kept in %s (set NZEUS_BT_DIR to change).\n",
           NZBT_VERSION, store_dir());
}

int main(int argc, char **argv)
{
    const char *cmd;
    int ai = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);       /* one line at a time, also into pipes */
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] && strcmp(argv[ai], "--pin")) {
        if (!strcmp(argv[ai], "-v")) g_verbose = 1;
        else if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) { usage(); return 0; }
        else { usage(); return 2; }
        ai++;
    }
    if (ai >= argc) { usage(); return 2; }
    cmd = argv[ai++];
    signal(SIGPIPE, SIG_IGN);
    if (!strcmp(cmd, "on")) return cmd_on();
    if (!strcmp(cmd, "off")) return cmd_off();
    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "list")) return cmd_list();
    if (!strcmp(cmd, "daemon")) return cmd_daemon();
    if (!strcmp(cmd, "scan") || !strcmp(cmd, "survey")) {
        int secs = ai < argc ? atoi(argv[ai]) : 10;
        if (secs < 1 || secs > 120) { printf("error scan time must be 1..120 seconds\n"); return 2; }
        return cmd_scan(secs, !strcmp(cmd, "survey"));
    }
    if (!strcmp(cmd, "pair")) {
        const char *pin = NULL;
        if (ai + 2 < argc && !strcmp(argv[ai + 1], "--pin")) pin = argv[ai + 2];
        if (pin && (strlen(pin) < 1 || strlen(pin) > 16 || strspn(pin, "0123456789") != strlen(pin))) {
            printf("error the PIN must be 1-16 digits\n");
            return 2;
        }
        return cmd_pair(ai < argc ? argv[ai] : NULL, pin);
    }
    if (!strcmp(cmd, "connect")) return cmd_connect(ai < argc ? argv[ai] : NULL);
    if (!strcmp(cmd, "disconnect")) return cmd_disconnect(ai < argc ? argv[ai] : NULL);
    if (!strcmp(cmd, "remove")) return cmd_remove(ai < argc ? argv[ai] : NULL);
    if (!strcmp(cmd, "hogp")) return cmd_hogp(ai < argc ? argv[ai] : NULL);
    usage();
    return 2;
}

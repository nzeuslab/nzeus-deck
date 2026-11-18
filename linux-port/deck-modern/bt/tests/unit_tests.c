/* unit_tests.c - tests of the parts of nzeus-bt that need no kernel:
 * addresses, scan data (EIR) parsing, device kinds, the key store, SDP.
 * Built for the PC (with address/undefined-behaviour checkers) and for MIPS
 * (run under qemu-mipsel). Prints one line per failure; exit 1 if any. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "bt.h"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---------- addresses ---------- */
static void test_addr(void)
{
    bdaddr_t a, b;
    char s[18];
    CHECK(addr_parse("00:1A:7D:DA:71:13", &a) == 0);
    CHECK(a.b[0] == 0x13 && a.b[5] == 0x00);           /* stored backwards */
    addr_str(&a, s);
    CHECK(!strcmp(s, "00:1A:7D:DA:71:13"));
    addr_file(&a, s);
    CHECK(!strcmp(s, "00-1A-7D-DA-71-13"));
    CHECK(addr_parse("00-1a-7d-da-71-13", &b) == 0 && addr_eq(&a, &b));
    CHECK(addr_parse("00:1A:7D:DA:71", &b) != 0);       /* too short */
    CHECK(addr_parse("00:1A:7D:DA:71:13:00", &b) != 0); /* too long */
    CHECK(addr_parse("00:1A:7D:DA:71:1G", &b) != 0);    /* not hex */
    CHECK(addr_parse("001A7DDA7113", &b) != 0);
    CHECK(addr_parse("", &b) != 0);
    CHECK(addr_type_parse("le-random") == ADDR_LE_RANDOM);
    CHECK(!strcmp(addr_type_str(ADDR_BREDR), "classic"));
}

/* ---------- scan data ---------- */
static void test_eir(void)
{
    struct eir_info e;
    /* classic keyboard: complete name + class of device (the kernel adds the class) */
    static const uint8_t kb[] = {
        0x0d, 0x09, 'K','e','y','s','-','T','o','-','G','o',' ','2',
        0x04, 0x0d, 0x40, 0x25, 0x00,
        0x00, 0x00, 0x00 };                       /* padding after a 0 length */
    /* LE mouse: flags, appearance 0x03C2 (mouse), HID service 0x1812, short name */
    static const uint8_t mouse[] = {
        0x02, 0x01, 0x06,
        0x03, 0x19, 0xc2, 0x03,
        0x05, 0x03, 0x0f, 0x18, 0x12, 0x18,
        0x04, 0x08, 'M','X','3' };
    /* short name first, complete name later: complete wins */
    static const uint8_t names[] = { 0x03, 0x08, 'A','B', 0x05, 0x09, 'A','B','C','D', 0x03, 0x08, 'Z','Z' };
    /* damaged: the second record claims 9 bytes but only 3 follow */
    static const uint8_t broken[] = { 0x02, 0x01, 0x05, 0x09, 0x09, 'a', 'b' };
    /* a name with a tab and a newline must come out printable */
    static const uint8_t ugly[] = { 0x07, 0x09, ' ', 'a', '\t', 'b', '\n', 'c' };

    CHECK(eir_parse(kb, sizeof(kb), &e) == 0);
    CHECK(e.has_name && e.name_complete && !strcmp(e.name, "Keys-To-Go 2"));
    CHECK(e.has_class && e.cod == 0x002540);
    CHECK(!e.has_appearance && !e.has_hid_uuid);

    CHECK(eir_parse(mouse, sizeof(mouse), &e) == 0);
    CHECK(e.has_flags && e.flags == 0x06);
    CHECK(e.has_appearance && e.appearance == 0x03c2);
    CHECK(e.has_hid_uuid);
    CHECK(e.has_name && !e.name_complete && !strcmp(e.name, "MX3"));

    CHECK(eir_parse(names, sizeof(names), &e) == 0);
    CHECK(!strcmp(e.name, "ABCD") && e.name_complete);

    CHECK(eir_parse(broken, sizeof(broken), &e) == -1);
    CHECK(e.has_flags && e.flags == 0x05);           /* the good part before the damage */
    CHECK(!e.has_name);

    CHECK(eir_parse(ugly, sizeof(ugly), &e) == 0);
    CHECK(!strcmp(e.name, "a b c"));

    CHECK(eir_parse(NULL, 0, &e) == 0 && !e.has_name);

    /* the deck's survey: maker data, services, transmit power */
    {
        /* Apple (company 0x004C) "proximity pairing" bytes, Fast Pair service data
         * (0xFE2C + a 3-byte model), TX power -12 dBm, the battery service twice */
        static const uint8_t adv[] = {
            0x02, 0x01, 0x1a,
            0x08, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x01, 0x0e, 0x20,
            0x06, 0x16, 0x2c, 0xfe, 0x12, 0x34, 0x56,
            0x02, 0x0a, 0xf4,
            0x05, 0x03, 0x0f, 0x18, 0x0f, 0x18 };
        /* a maker record longer than we keep (40 bytes of data): cut, not overflowing */
        uint8_t big[44];
        memset(big, 0x55, sizeof(big));
        big[0] = 43; big[1] = 0xff; big[2] = 0x06; big[3] = 0x00;
        CHECK(eir_parse(adv, sizeof(adv), &e) == 0);
        CHECK(e.has_mfr && e.company == 0x004c && e.mfr_len == 5);
        CHECK(e.mfr[0] == 0x07 && e.mfr[1] == 0x19 && e.mfr[3] == 0x0e && e.mfr[4] == 0x20);
        CHECK(e.has_tx && e.tx_power == -12);
        CHECK(e.n_uuid == 2 && e.uuid[0] == 0xfe2c && e.uuid[1] == 0x180f);   /* 0x180F only once */
        CHECK(eir_parse(big, sizeof(big), &e) == 0);
        CHECK(e.has_mfr && e.company == 0x0006 && e.mfr_len == 31 && e.mfr[30] == 0x55);
    }
}

static void test_kinds(void)
{
    CHECK(!strcmp(kind_from_class(0x002540), "keyboard"));        /* peripheral, keyboard */
    CHECK(!strcmp(kind_from_class(0x002580), "mouse"));
    CHECK(!strcmp(kind_from_class(0x0025c0), "keyboard+mouse"));
    CHECK(!strcmp(kind_from_class(0x002508), "gamepad"));
    CHECK(!strcmp(kind_from_class(0x002504), "joystick"));
    CHECK(!strcmp(kind_from_class(0x5a020c), "phone"));           /* smartphone */
    CHECK(!strcmp(kind_from_class(0x240404), "audio"));           /* headset */
    CHECK(!strcmp(kind_from_class(0x00010c), "computer"));        /* laptop */
    CHECK(!strcmp(kind_from_class(0x000000), "other"));
    CHECK(class_is_hid(0x002540) && class_is_hid(0x002508) && !class_is_hid(0x5a020c));
    CHECK(!strcmp(kind_from_appearance(0x03c1), "keyboard"));
    CHECK(!strcmp(kind_from_appearance(0x03c2), "mouse"));
    CHECK(!strcmp(kind_from_appearance(0x03c4), "gamepad"));
    CHECK(!strcmp(kind_from_appearance(0x03c0), "input"));
    CHECK(!strcmp(kind_from_appearance(0x0040), "phone"));
    CHECK(!strcmp(kind_from_appearance(0x00c1), "watch"));
    CHECK(appearance_is_hid(0x03c2) && !appearance_is_hid(0x0040));
}

/* ---------- key store ---------- */
static void fill_device(struct bt_device *d)
{
    int i;
    memset(d, 0, sizeof(*d));
    addr_parse("DC:2C:26:01:02:03", &d->addr);
    d->addr_type = ADDR_BREDR;
    snprintf(d->name, sizeof(d->name), "Keyboard = with equals");
    snprintf(d->kind, sizeof(d->kind), "keyboard");
    d->cod = 0x002540;
    d->has_linkkey = 1;
    for (i = 0; i < 16; i++) d->linkkey[i] = (uint8_t)(0xf0 + i);
    d->linkkey_type = 5; d->pin_len = 0;
    d->has_hid = 1;
    d->hid_vendor = 0x05ac; d->hid_product = 0x0255; d->hid_version = 0x0050;
    d->hid_vendor_src = 1; d->hid_parser = 0x0111; d->hid_country = 0x21; d->hid_subclass = 0x40;
    d->rd_size = 300;
    for (i = 0; i < 300; i++) d->rd[i] = (uint8_t)(i * 7);
}

static void test_store(void)
{
    static struct bt_device d, e;
    static char text[16384];
    struct bt_device *list;
    char dir[] = "/tmp/nzbt-unit-XXXXXX";
    int n, i;
    bdaddr_t other;

    fill_device(&d);
    CHECK(store_format(&d, text, sizeof(text)) > 0);
    CHECK(store_parse(text, &e) == 0);
    CHECK(addr_eq(&d.addr, &e.addr) && e.addr_type == ADDR_BREDR);
    CHECK(!strcmp(e.name, d.name) && !strcmp(e.kind, "keyboard") && e.cod == 0x002540);
    CHECK(e.has_linkkey && !memcmp(e.linkkey, d.linkkey, 16) && e.linkkey_type == 5);
    CHECK(e.has_hid && e.hid_vendor == 0x05ac && e.hid_product == 0x0255 && e.hid_version == 0x50);
    CHECK(e.hid_vendor_src == 1 && e.hid_parser == 0x111 && e.hid_country == 0x21 && e.hid_subclass == 0x40);
    CHECK(e.rd_size == 300 && !memcmp(e.rd, d.rd, 300));
    CHECK(e.n_ltk == 0 && !e.has_irk);

    /* LE device with two keys and an identity key */
    memset(&d, 0, sizeof(d));
    addr_parse("C0:11:22:33:44:55", &d.addr);
    d.addr_type = ADDR_LE_RANDOM;
    d.n_ltk = 2;
    d.ltk[0].master = 1; d.ltk[0].type = 1; d.ltk[0].enc_size = 16; d.ltk[0].ediv = 0xbeef;
    d.ltk[0].rand = 0x0123456789abcdefULL; memset(d.ltk[0].val, 0xaa, 16);
    d.ltk[1].master = 0; d.ltk[1].type = 0; d.ltk[1].enc_size = 7; d.ltk[1].ediv = 1;
    d.ltk[1].rand = 0xffffffffffffffffULL; memset(d.ltk[1].val, 0x55, 16);
    d.has_irk = 1; memset(d.irk, 0x11, 16);
    CHECK(store_format(&d, text, sizeof(text)) > 0);
    CHECK(store_parse(text, &e) == 0);
    CHECK(e.addr_type == ADDR_LE_RANDOM && e.n_ltk == 2);
    CHECK(e.ltk[0].master == 1 && e.ltk[0].ediv == 0xbeef && e.ltk[0].rand == 0x0123456789abcdefULL);
    CHECK(e.ltk[0].enc_size == 16 && e.ltk[0].val[15] == 0xaa);
    CHECK(e.ltk[1].master == 0 && e.ltk[1].rand == 0xffffffffffffffffULL && e.ltk[1].val[0] == 0x55);
    CHECK(e.has_irk && e.irk[7] == 0x11);

    /* Windows line endings + unknown lines + a bad key are tolerated */
    CHECK(store_parse("address=00:11:22:33:44:55\r\nname=PC edited\r\nfuture=1\r\nlinkkey=zz\r\n", &e) == 0);
    CHECK(!strcmp(e.name, "PC edited") && !e.has_linkkey);
    CHECK(store_parse("name=no address\n", &e) != 0);
    /* a too-short key is refused, not half-used */
    CHECK(store_parse("address=00:11:22:33:44:55\nlinkkey=00112233\n", &e) == 0 && !e.has_linkkey);

    /* save / load / list / remove in a temporary folder */
    CHECK(mkdtemp(dir) != NULL);
    setenv("NZEUS_BT_DIR", dir, 1);
    fill_device(&d);
    CHECK(store_save(&d) == 0);
    CHECK(store_load(&d.addr, &e) == 0 && !strcmp(e.name, d.name) && e.rd_size == 300);
    addr_parse("00:00:00:00:00:01", &other);
    CHECK(store_load(&other, &e) != 0);
    memset(&e, 0, sizeof(e));
    e.addr = other;
    snprintf(e.name, sizeof(e.name), "second");
    CHECK(store_save(&e) == 0);
    n = store_list(&list);
    CHECK(n == 2);
    for (i = 0; i < n; i++) CHECK(addr_eq(&list[i].addr, &d.addr) || addr_eq(&list[i].addr, &other));
    free(list);
    /* a file whose name and contents disagree is not trusted */
    {
        char path[256];
        FILE *f;
        snprintf(path, sizeof(path), "%s/devices/00-00-00-00-00-02.conf", dir);
        f = fopen(path, "w");
        fprintf(f, "address=00:00:00:00:00:03\n");
        fclose(f);
        n = store_list(&list);
        CHECK(n == 2);
        free(list);
        unlink(path);
    }
    CHECK(store_remove(&other) == 0);
    CHECK(store_remove(&other) != 0);
    n = store_list(&list);
    CHECK(n == 1);
    free(list);
    /* no temporary file is left behind */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/devices/DC-2C-26-01-02-03.conf.tmp", dir);
        CHECK(access(path, F_OK) != 0);
    }
    CHECK(store_lock() >= 0);
    store_remove(&d.addr);
    {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) printf("note: could not remove %s\n", dir);
    }
}

/* ---------- SDP ---------- */
/* A boot keyboard report descriptor (63 bytes, from the HID specification). */
static const uint8_t KB_DESC[63] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,
    0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,
    0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xc0 };

/* An SDP answer as a real keyboard sends it, written out byte by byte from the
 * specification (NOT made by our own code, so it checks the parser honestly). */
static uint8_t HID_REC[127];
static void make_hid_record(void)
{
    static const uint8_t head[] = {
        0x35, 0x7d,                                   /* list of records, 125 bytes */
        0x35, 0x7b,                                   /*  record, 123 bytes */
        0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x01,     /* 0x0000 handle */
        0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x24,     /* 0x0001 class: HID */
        0x09, 0x01, 0x00, 0x25, 0x0c, 'M','a','g','i','c',' ','K','e','y','b','r','d',
        0x09, 0x02, 0x01, 0x09, 0x01, 0x11,           /* 0x0201 parser 0x0111 */
        0x09, 0x02, 0x02, 0x08, 0x40,                 /* 0x0202 subclass: keyboard */
        0x09, 0x02, 0x03, 0x08, 0x21,                 /* 0x0203 country 0x21 */
        0x09, 0x02, 0x06, 0x35, 0x45,                 /* 0x0206 descriptor list, 69 bytes */
        0x35, 0x43, 0x08, 0x22, 0x25, 0x3f };         /*  { 0x22, text of 63 bytes } */
    memcpy(HID_REC, head, sizeof(head));
    memcpy(HID_REC + sizeof(head), KB_DESC, 63);
}

/* a record whose descriptor list starts with a NON-report descriptor (type 0x23) */
static const uint8_t HID_REC2_HEAD[] = {
    0x35, 0x5c, 0x35, 0x5a,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x24,
    0x09, 0x02, 0x06, 0x35, 0x4d,
    0x35, 0x06, 0x08, 0x23, 0x25, 0x02, 'x', 'y',      /* physical descriptor: must be skipped */
    0x35, 0x43, 0x08, 0x22, 0x25, 0x3f };              /* the report descriptor */

static const uint8_t PNP_REC[] = {
    0x35, 0x1a, 0x35, 0x18,
    0x09, 0x02, 0x01, 0x09, 0x05, 0xac,
    0x09, 0x02, 0x02, 0x09, 0x02, 0x55,
    0x09, 0x02, 0x03, 0x09, 0x00, 0x50,
    0x09, 0x02, 0x05, 0x09, 0x00, 0x01 };

static void test_sdp(void)
{
    static struct hid_info hi;
    struct pnp_info pi;
    uint8_t buf[64];
    static const uint8_t want_req[] = { 0x06, 0x00, 0x01, 0x00, 0x0f, 0x35, 0x03, 0x19, 0x11, 0x24,
                                        0xff, 0xff, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00 };
    static const uint8_t cont[] = { 0xaa, 0xbb };
    int n, cut, i;

    make_hid_record();
    n = sdp_build_request(buf, sizeof(buf), 1, UUID_HID, NULL, 0);
    CHECK(n == (int)sizeof(want_req) && !memcmp(buf, want_req, n));
    n = sdp_build_request(buf, sizeof(buf), 7, UUID_HID, cont, 2);
    CHECK(n == 22 && buf[4] == 17 && buf[19] == 2 && buf[20] == 0xaa && buf[21] == 0xbb && buf[2] == 7);
    CHECK(sdp_build_request(buf, 10, 1, UUID_HID, NULL, 0) < 0);   /* buffer too small */

    CHECK(sdp_parse_hid(HID_REC, sizeof(HID_REC), &hi) == 0);
    CHECK(hi.found && hi.rd_size == 63 && !memcmp(hi.rd, KB_DESC, 63));
    CHECK(hi.parser == 0x0111 && hi.subclass == 0x40 && hi.country == 0x21);
    CHECK(!strcmp(hi.name, "Magic Keybrd"));

    CHECK(sdp_parse_pnp(PNP_REC, sizeof(PNP_REC), &pi) == 0);
    CHECK(pi.vendor == 0x05ac && pi.product == 0x0255 && pi.version == 0x0050 && pi.source == 1);
    CHECK(sdp_parse_pnp(HID_REC, sizeof(HID_REC), &pi) == 0 && pi.vendor == 0x0111); /* (attr ids are per service) */
    CHECK(sdp_parse_hid(PNP_REC, sizeof(PNP_REC), &hi) != 0);                       /* no descriptor */
    {
        uint8_t rec2[sizeof(HID_REC2_HEAD) + 63];
        memcpy(rec2, HID_REC2_HEAD, sizeof(HID_REC2_HEAD));
        memcpy(rec2 + sizeof(HID_REC2_HEAD), KB_DESC, 63);
        CHECK(sizeof(rec2) == 94);
        CHECK(sdp_parse_hid(rec2, sizeof(rec2), &hi) == 0 && hi.rd_size == 63 && !memcmp(hi.rd, KB_DESC, 63));
    }

    /* every cut-off version must be refused without crashing */
    for (cut = 0; cut < (int)sizeof(HID_REC); cut++)
        CHECK(sdp_parse_hid(HID_REC, cut, &hi) != 0);
    /* random garbage must not crash (the checker builds catch bad reads) */
    srand(1);
    for (i = 0; i < 2000; i++) {
        uint8_t junk[200];
        int k, len = rand() % 200;
        for (k = 0; k < len; k++) junk[k] = (uint8_t)rand();
        if (i % 3 == 0 && len > 2) { junk[0] = 0x35; junk[1] = (uint8_t)(len - 2); }
        sdp_parse_hid(junk, len, &hi);
        sdp_parse_pnp(junk, len, &pi);
    }
}

/* sdp_query must glue an answer that comes in two pieces ("continuation") */
static void test_sdp_query(void)
{
    static uint8_t out[1024];
    int sv[2], n, status = 0;
    pid_t pid;
    make_hid_record();
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    pid = fork();
    if (pid == 0) {                   /* the "device": answers in two parts */
        uint8_t req[64], rsp[256];
        int r, part1 = 60, ok = 1;
        close(sv[0]);
        r = read(sv[1], req, sizeof(req));
        if (r != 20 || req[0] != 0x06 || req[19] != 0) ok = 0;
        rsp[0] = 0x07; rsp[1] = req[1]; rsp[2] = req[2];
        put_be16(rsp + 5, part1);
        memcpy(rsp + 7, HID_REC, part1);
        rsp[7 + part1] = 2; rsp[8 + part1] = 0x12; rsp[9 + part1] = 0x34;
        put_be16(rsp + 3, 2 + part1 + 3);
        if (write(sv[1], rsp, 7 + part1 + 3) < 0) ok = 0;
        r = read(sv[1], req, sizeof(req));
        /* the second request must carry the continuation bytes back */
        if (r != 22 || req[19] != 2 || req[20] != 0x12 || req[21] != 0x34) ok = 0;
        rsp[0] = 0x07; rsp[1] = req[1]; rsp[2] = req[2];
        put_be16(rsp + 5, sizeof(HID_REC) - part1);
        memcpy(rsp + 7, HID_REC + part1, sizeof(HID_REC) - part1);
        rsp[7 + sizeof(HID_REC) - part1] = 0;
        put_be16(rsp + 3, 2 + sizeof(HID_REC) - part1 + 1);
        if (write(sv[1], rsp, 7 + sizeof(HID_REC) - part1 + 1) < 0) ok = 0;
        _exit(ok ? 0 : 1);
    }
    close(sv[1]);
    n = sdp_query(sv[0], UUID_HID, out, sizeof(out), 2000);
    CHECK(n == (int)sizeof(HID_REC) && !memcmp(out, HID_REC, n));
    close(sv[0]);          /* first: a "device" still waiting for a request then gives up */
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    test_addr();
    test_eir();
    test_kinds();
    test_store();
    test_sdp();
    test_sdp_query();
    printf("unit tests: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

/*
 * bt.h - shared definitions for nzeus-bt, the small Bluetooth tool of the
 * NZEUS R36S console.
 *
 * How Bluetooth works on the console (no BlueZ daemon, no D-Bus):
 *   - The kernel modules (bluetooth.ko, btusb.ko, btrtl.ko, hidp.ko, uhid.ko)
 *     do the radio work. Programs talk to them through "sockets":
 *       * the MANAGEMENT socket ("mgmt"): switch on/off, scan, pair, keys
 *       * L2CAP sockets: data channels to one device (like TCP ports;
 *         a "PSM" is the port number: 1 = SDP, 17/19 = keyboard/mouse)
 *       * the HIDP socket: hands two L2CAP channels of a classic keyboard
 *         or mouse to the kernel, which then makes /dev/input/eventN
 *       * /dev/uhid: lets us create a keyboard/mouse device ourselves
 *         (used for Bluetooth Low Energy keyboards/mice)
 *   - This header has the numbers and message layouts the kernel expects.
 *     They are copied from the console's kernel (Linux 4.4.186) headers.
 *
 * All multi-byte numbers in Bluetooth messages are little-endian (lowest
 * byte first), except SDP, which is big-endian. Helpers below read/write them.
 */
#ifndef NZEUS_BT_H
#define NZEUS_BT_H

#include <stdint.h>
#include <stddef.h>

#define NZBT_VERSION "1.0"

/* ---------- Bluetooth addresses ---------- */
/* 6 bytes. The kernel stores them "backwards": b[0] is the LAST pair of
 * "AA:BB:CC:DD:EE:FF". addr_parse()/addr_str() do the turning around. */
typedef struct { uint8_t b[6]; } bdaddr_t;

#define ADDR_BREDR      0   /* classic Bluetooth ("BR/EDR")              */
#define ADDR_LE_PUBLIC  1   /* Low Energy, fixed address from the maker   */
#define ADDR_LE_RANDOM  2   /* Low Energy, address the device made up     */

int  addr_parse(const char *s, bdaddr_t *a);       /* "AA:BB:CC:DD:EE:FF" (or AA-BB-...) -> 0 ok */
void addr_str(const bdaddr_t *a, char out[18]);    /* -> "AA:BB:CC:DD:EE:FF" */
void addr_file(const bdaddr_t *a, char out[18]);   /* -> "AA-BB-CC-DD-EE-FF" (FAT32 forbids ':') */
int  addr_eq(const bdaddr_t *a, const bdaddr_t *b);
const char *addr_type_str(uint8_t type);           /* "classic", "le-public", "le-random" */
int  addr_type_parse(const char *s);                /* reverse; -1 if unknown */

/* ---------- little/big-endian helpers ---------- */
static inline uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }

/* ---------- small helpers (util.c) ---------- */
long long now_ms(void);                              /* milliseconds since boot */
int  hex_encode(const uint8_t *in, size_t n, char *out, size_t outsz);  /* -> chars written */
int  hex_decode(const char *in, uint8_t *out, size_t max);              /* -> bytes, -1 bad */
void clean_name(const char *in, size_t n, char *out, size_t outsz);    /* printable, no tabs */
extern int g_verbose;                                /* -v: print protocol details to stderr */
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- what a device is (eir.c) ---------- */
/* "EIR"/advertising data = a list of [length][type][data] records a device
 * sends while we scan: its name, what kind of device it is, its services. */
struct eir_info {
    char     name[249];
    int      has_name, name_complete;
    int      has_class;   uint32_t cod;          /* classic "class of device" */
    int      has_appearance; uint16_t appearance;/* LE "appearance"            */
    int      has_hid_uuid;                       /* advertises a HID service   */
    int      has_flags;   uint8_t flags;
    /* for the deck's "what is this device?" (nzeus-bt survey) */
    int      has_mfr;     uint16_t company;      /* maker data: the Bluetooth SIG company number */
    uint8_t  mfr[31];     uint8_t mfr_len;       /* ...and the maker's own bytes after it */
    int      n_uuid;      uint16_t uuid[8];      /* 16-bit services it announces (or sends data for) */
    int      has_tx;      int8_t tx_power;       /* how loud it sends (dBm): helps guess the distance */
};
int eir_parse(const uint8_t *eir, size_t len, struct eir_info *out);   /* 0 ok, -1 damaged */
const char *kind_from_class(uint32_t cod);       /* "keyboard", "mouse", ... */
const char *kind_from_appearance(uint16_t ap);
int class_is_hid(uint32_t cod);                  /* a keyboard/mouse/gamepad... */
int appearance_is_hid(uint16_t ap);

/* ---------- the key store on the SD card (store.c) ---------- */
#define HID_DESC_MAX 4096
struct bt_ltk {                       /* LE "long term key" (from LE pairing) */
    uint8_t type, master, enc_size;
    uint16_t ediv;
    uint64_t rand;
    uint8_t val[16];
};
struct bt_device {
    bdaddr_t addr;
    uint8_t  addr_type;              /* ADDR_BREDR / ADDR_LE_PUBLIC / ADDR_LE_RANDOM */
    char     name[249];
    char     kind[24];               /* keyboard, mouse, ... */
    uint32_t cod;  uint16_t appearance;
    /* classic pairing key */
    int      has_linkkey; uint8_t linkkey[16]; uint8_t linkkey_type, pin_len;
    /* LE pairing keys */
    int      n_ltk; struct bt_ltk ltk[2];
    int      has_irk; uint8_t irk[16];
    /* classic HID information (from the device's SDP record) */
    int      has_hid;
    uint16_t hid_vendor, hid_product, hid_version, hid_parser;
    uint8_t  hid_country, hid_subclass, hid_vendor_src;
    uint16_t rd_size; uint8_t rd[HID_DESC_MAX];   /* the HID "report descriptor" */
};
const char *store_dir(void);                     /* $NZEUS_BT_DIR or /mnt/sdcard/nzeus/bluetooth */
int  store_load(const bdaddr_t *a, struct bt_device *d);           /* 0 ok, -1 not paired */
int  store_save(const struct bt_device *d);                        /* 0 ok (atomic) */
int  store_remove(const bdaddr_t *a);                              /* 0 removed, -1 none */
int  store_list(struct bt_device **out);                           /* malloc'd array -> count */
int  store_parse(const char *text, struct bt_device *d);           /* for tests */
int  store_format(const struct bt_device *d, char *out, size_t outsz);
int  store_lock(void);                                             /* fd; close() unlocks */

/* ---------- management socket (mgmt.c) ---------- */
#define MGMT_INDEX_NONE 0xFFFF
#define MGMT_OP_READ_VERSION        0x0001
#define MGMT_OP_READ_INDEX_LIST     0x0003
#define MGMT_OP_READ_INFO           0x0004
#define MGMT_OP_SET_POWERED         0x0005
#define MGMT_OP_SET_CONNECTABLE     0x0007
#define MGMT_OP_SET_BONDABLE        0x0009
#define MGMT_OP_SET_SSP             0x000B
#define MGMT_OP_SET_LE              0x000D
#define MGMT_OP_SET_DEV_CLASS       0x000E
#define MGMT_OP_SET_LOCAL_NAME      0x000F
#define MGMT_OP_LOAD_LINK_KEYS      0x0012
#define MGMT_OP_LOAD_LONG_TERM_KEYS 0x0013
#define MGMT_OP_DISCONNECT          0x0014
#define MGMT_OP_GET_CONNECTIONS     0x0015
#define MGMT_OP_PIN_CODE_REPLY      0x0016
#define MGMT_OP_PIN_CODE_NEG_REPLY  0x0017
#define MGMT_OP_SET_IO_CAPABILITY   0x0018
#define MGMT_OP_PAIR_DEVICE         0x0019
#define MGMT_OP_UNPAIR_DEVICE       0x001B
#define MGMT_OP_USER_CONFIRM_REPLY  0x001C
#define MGMT_OP_USER_CONFIRM_NEG_REPLY 0x001D
#define MGMT_OP_USER_PASSKEY_REPLY  0x001E
#define MGMT_OP_USER_PASSKEY_NEG_REPLY 0x001F
#define MGMT_OP_START_DISCOVERY     0x0023
#define MGMT_OP_STOP_DISCOVERY      0x0024
#define MGMT_OP_CONFIRM_NAME        0x0025
#define MGMT_OP_SET_SECURE_CONN     0x002D
#define MGMT_OP_LOAD_IRKS           0x0030
#define MGMT_OP_ADD_DEVICE          0x0033
#define MGMT_OP_REMOVE_DEVICE       0x0034

#define MGMT_EV_CMD_COMPLETE        0x0001
#define MGMT_EV_CMD_STATUS          0x0002
#define MGMT_EV_INDEX_ADDED         0x0004
#define MGMT_EV_INDEX_REMOVED       0x0005
#define MGMT_EV_NEW_SETTINGS        0x0006
#define MGMT_EV_NEW_LINK_KEY        0x0009
#define MGMT_EV_NEW_LONG_TERM_KEY   0x000A
#define MGMT_EV_DEVICE_CONNECTED    0x000B
#define MGMT_EV_DEVICE_DISCONNECTED 0x000C
#define MGMT_EV_CONNECT_FAILED      0x000D
#define MGMT_EV_PIN_CODE_REQUEST    0x000E
#define MGMT_EV_USER_CONFIRM_REQUEST 0x000F
#define MGMT_EV_USER_PASSKEY_REQUEST 0x0010
#define MGMT_EV_AUTH_FAILED         0x0011
#define MGMT_EV_DEVICE_FOUND        0x0012
#define MGMT_EV_DISCOVERING         0x0013
#define MGMT_EV_DEVICE_UNPAIRED     0x0016
#define MGMT_EV_PASSKEY_NOTIFY      0x0017
#define MGMT_EV_NEW_IRK             0x0018

#define MGMT_SETTING_POWERED     0x00000001
#define MGMT_SETTING_CONNECTABLE 0x00000002
#define MGMT_SETTING_DISCOVERABLE 0x00000008
#define MGMT_SETTING_BONDABLE    0x00000010
#define MGMT_SETTING_SSP         0x00000040
#define MGMT_SETTING_BREDR       0x00000080
#define MGMT_SETTING_LE          0x00000200
#define MGMT_SETTING_SECURE_CONN 0x00000800

#define MGMT_DEV_FOUND_CONFIRM_NAME 0x01

/* "I/O capability": what the console can do during pairing.
 * DisplayYesNo = it has a screen and yes/no buttons. With a keyboard
 * (KeyboardOnly) this makes the console SHOW a 6-digit passkey that the
 * user types on the keyboard; with a mouse (no buttons) it is "just works". */
#define IO_CAP_DISPLAY_YESNO 0x01

#define MGMT_ERR_TIMEOUT  0x100     /* our own codes above the kernel's 0x00..0x14 */
#define MGMT_ERR_IO       0x101

struct mgmt;
typedef void (*mgmt_event_fn)(struct mgmt *m, uint16_t ev, uint16_t index,
                              const uint8_t *p, uint16_t len);
struct mgmt {
    int fd;
    uint16_t index;          /* which controller: 0 = hci0 */
    int filter;              /* 1 = ignore events of OTHER controllers (set by mgmt_pick_index) */
    mgmt_event_fn on_event;  /* called for every event that is not our reply */
    void *user;
};
int  mgmt_open(struct mgmt *m);
void mgmt_close(struct mgmt *m);
int  mgmt_send(struct mgmt *m, uint16_t op, uint16_t index, const void *p, uint16_t len);
/* sends a command and waits for its reply; returns the kernel's status
 * (0 = success) or MGMT_ERR_*. rsp/rsp_len may be NULL. */
int  mgmt_request(struct mgmt *m, uint16_t op, uint16_t index, const void *p, uint16_t len,
                  uint8_t *rsp, uint16_t *rsp_len, int timeout_ms);
int  mgmt_wait(struct mgmt *m, int timeout_ms);  /* handle events for a while; -1 on error */
int  mgmt_read_one(struct mgmt *m, int timeout_ms);  /* one message; 0 timeout, 1 got one, -1 error */
const char *mgmt_status_str(int status);
int  mgmt_pick_index(struct mgmt *m);            /* first controller, or $NZEUS_BT_INDEX */

/* ---------- SDP: asking a classic device what it is (sdp.c) ---------- */
struct hid_info {
    int found;
    uint16_t parser; uint8_t subclass, country;
    uint16_t rd_size; uint8_t rd[HID_DESC_MAX];
    char name[128];
};
struct pnp_info { int found; uint16_t vendor, product, version, source; };
int sdp_build_request(uint8_t *buf, size_t bufsz, uint16_t tid, uint16_t uuid16,
                      const uint8_t *cont, int cont_len);
int sdp_query(int fd, uint16_t uuid16, uint8_t *out, size_t max, int timeout_ms);  /* -> bytes */
int sdp_parse_hid(const uint8_t *lists, size_t n, struct hid_info *hi);  /* 0 ok */
int sdp_parse_pnp(const uint8_t *lists, size_t n, struct pnp_info *pi);
#define UUID_HID  0x1124
#define UUID_PNP  0x1200

/* ---------- classic HID (hid.c) ---------- */
#define PSM_SDP      0x0001
#define PSM_HID_CTRL 0x0011
#define PSM_HID_INTR 0x0013
int hid_fetch_sdp(const bdaddr_t *a, struct bt_device *d, int timeout_ms);
int hid_connect(const struct bt_device *d, int timeout_ms);
int hid_attach(int ctrl, int intr, const struct bt_device *d);  /* HIDPCONNADD */
int hid_detach(const bdaddr_t *a);                              /* HIDPCONNDEL */
int hid_is_attached(const bdaddr_t *a);

/* kernel's HIDP ioctl messages (net/bluetooth/hidp/hidp.h) */
struct hidp_connadd_req {
    int      ctrl_sock, intr_sock;
    uint16_t parser, rd_size;
    uint8_t *rd_data;
    uint8_t  country, subclass;
    uint16_t vendor, product, version;
    uint32_t flags, idle_to;
    char     name[128];
};
struct hidp_conndel_req { bdaddr_t bdaddr; uint32_t flags; };
struct hidp_conninfo {
    bdaddr_t bdaddr; uint32_t flags; uint16_t state, vendor, product, version; char name[128];
};
struct hidp_connlist_req { uint32_t cnum; struct hidp_conninfo *ci; };

/* ---------- LE keyboards/mice: HID over GATT (hogp.c) ---------- */
int hogp_run(const struct bt_device *d);   /* runs until the device disconnects */

/* ---------- operating system layer (io.c) ---------- */
/* Everything that touches the real kernel goes through these, so the PC
 * tests can swap them for fakes (built with -DNZBT_FAKE). */
int io_mgmt_socket(void);
int io_l2cap_connect(const bdaddr_t *a, uint8_t addr_type, uint16_t psm, uint16_t cid,
                     int sec_level, int timeout_ms);
int io_l2cap_listen(uint16_t psm, int sec_level);
void io_set_local_addr(const bdaddr_t *a);           /* bind channels to our controller */
int io_l2cap_accept(int lfd, bdaddr_t *peer);
int io_hidp_add(struct hidp_connadd_req *req);
int io_hidp_del(const bdaddr_t *a);
int io_hidp_list(struct hidp_conninfo *ci, int max);
int io_uhid_open(void);
const char *io_runfile(const char *name);   /* "daemon.pid", "scan" */
#define BT_SEC_LOW    1
#define BT_SEC_MEDIUM 2     /* paired + encrypted */
#define L2CAP_CID_ATT 0x0004

/* ---------- uhid (/dev/uhid) messages (include/uapi/linux/uhid.h) ---------- */
#define UHID_DESTROY 1
#define UHID_START   2
#define UHID_STOP    3
#define UHID_OPEN    4
#define UHID_CLOSE   5
#define UHID_OUTPUT  6
#define UHID_GET_REPORT 9
#define UHID_GET_REPORT_REPLY 10
#define UHID_CREATE2 11
#define UHID_INPUT2  12
#define UHID_SET_REPORT 13
#define UHID_SET_REPORT_REPLY 14
#define UHID_DATA_MAX 4096
#define BUS_BLUETOOTH 0x05
struct uhid_create2_req {
    uint8_t name[128], phys[64], uniq[64];
    uint16_t rd_size, bus;
    uint32_t vendor, product, version, country;
    uint8_t rd_data[HID_DESC_MAX];
} __attribute__((packed));
struct uhid_input2_req { uint16_t size; uint8_t data[UHID_DATA_MAX]; } __attribute__((packed));
struct uhid_output_req { uint8_t data[UHID_DATA_MAX]; uint16_t size; uint8_t rtype; } __attribute__((packed));
struct uhid_get_report_req { uint32_t id; uint8_t rnum, rtype; } __attribute__((packed));
struct uhid_get_report_reply_req { uint32_t id; uint16_t err, size; uint8_t data[UHID_DATA_MAX]; } __attribute__((packed));
struct uhid_set_report_req { uint32_t id; uint8_t rnum, rtype; uint16_t size; uint8_t data[UHID_DATA_MAX]; } __attribute__((packed));
struct uhid_set_report_reply_req { uint32_t id; uint16_t err; } __attribute__((packed));
struct uhid_event {
    uint32_t type;
    union {
        struct uhid_create2_req create2;
        struct uhid_input2_req input2;
        struct uhid_output_req output;
        struct uhid_get_report_req get_report;
        struct uhid_get_report_reply_req get_report_reply;
        struct uhid_set_report_req set_report;
        struct uhid_set_report_reply_req set_report_reply;
    } u;
} __attribute__((packed));

#endif

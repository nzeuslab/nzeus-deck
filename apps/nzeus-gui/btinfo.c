/* btinfo.c - Bluetooth "ID cards": what a nearby device is, from what it
 * says while we scan (`nzeus-bt survey`). The clues, best first:
 *   - maker data: who made it (Bluetooth SIG company number) + the maker's
 *     own bytes (Apple's tell the AirPods model, Find My trackers, iBeacons)
 *   - services it announces (Fast Pair, Tile, SmartTag, heart rate...)
 *   - its class (classic) or appearance (Low Energy): phone, keyboard, watch...
 *   - its address: fixed from the maker, or a private one that changes
 * No screen code here (app_btscan.c shows the cards). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btinfo.h"
#include "wifiinfo.h"          /* wi_maker(): the IEEE maker list (same file for Wi-Fi and Bluetooth) */

/* ---------- reading a survey line ---------- */

/* "a,b,c" of hex numbers -> list; returns how many */
static int hex_list(const char *s, unsigned *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) break;
        out[n++] = (unsigned)v;
        s = *end == ',' ? end + 1 : end;
    }
    return n;
}

int bt_parse_survey(const char *line, BtSurvey *d)
{
    char buf[400], *f[9];
    int nf = 0;
    memset(d, 0, sizeof *d);
    snprintf(buf, sizeof buf, "%s", line);
    buf[strcspn(buf, "\r\n")] = 0;
    for (char *p = buf; nf < 9; ) {                         /* split at the tabs (the name is last) */
        f[nf++] = p;
        char *t = nf < 9 ? strchr(p, '\t') : NULL;
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    if (nf != 9 || strlen(f[0]) != 17) return -1;
    snprintf(d->addr, sizeof d->addr, "%s", f[0]);
    snprintf(d->type, sizeof d->type, "%s", f[1]);
    d->rssi = atoi(f[2]);
    if (strcmp(f[3], "-")) { d->has_tx = 1; d->tx_power = atoi(f[3]); }
    if (strcmp(f[4], "-")) { d->has_cod = 1; d->cod = (unsigned)strtoul(f[4], NULL, 16); }
    if (strcmp(f[5], "-")) { d->has_app = 1; d->appearance = (unsigned)strtoul(f[5], NULL, 16); }
    if (strcmp(f[6], "-")) d->n_uuid = hex_list(f[6], d->uuid, 8);
    if (strcmp(f[7], "-")) {                                /* "004c:0719010e20": company:bytes */
        char *colon = strchr(f[7], ':');
        d->has_mfr = 1;
        d->company = (unsigned)strtoul(f[7], NULL, 16);
        for (const char *p = colon ? colon + 1 : ""; p[0] && p[1] && d->mfr_len < (int)sizeof d->mfr; p += 2) {
            char two[3] = { p[0], p[1], 0 };
            d->mfr[d->mfr_len++] = (unsigned char)strtoul(two, NULL, 16);
        }
    }
    snprintf(d->name, sizeof d->name, "%s", strcmp(f[8], "(no name)") ? f[8] : "");
    return 0;
}

/* ---------- the built-in lists (only codes we are sure of) ---------- */

const char *bt_company_name(unsigned id)
{
    static const struct { unsigned id; const char *name; } list[] = {
        { 0x0002, "Intel" }, { 0x0006, "Microsoft" }, { 0x000A, "Qualcomm (CSR)" }, { 0x000F, "Broadcom" },
        { 0x001D, "Qualcomm" }, { 0x0046, "MediaTek" }, { 0x004C, "Apple" }, { 0x0059, "Nordic Semiconductor" },
        { 0x005D, "Realtek" }, { 0x0067, "GN Netcom (Jabra)" }, { 0x0075, "Samsung" }, { 0x0087, "Garmin" },
        { 0x009E, "Bose" }, { 0x00C4, "LG Electronics" }, { 0x00E0, "Google" }, { 0x012D, "Sony" },
        { 0x0171, "Amazon" }, { 0x01DA, "Logitech" }, { 0x027D, "Huawei" }, { 0x02E5, "Espressif" },
        { 0x038F, "Xiaomi" },
    };
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) if (list[i].id == id) return list[i].name;
    return NULL;
}

const char *bt_uuid_name(unsigned u)
{
    static const struct { unsigned id; const char *name; } list[] = {
        { 0x1101, "serial port" }, { 0x1105, "file sending" }, { 0x1108, "headset" }, { 0x110A, "music source" },
        { 0x110B, "music (speaker/headphones)" }, { 0x111E, "hands-free calls" }, { 0x1124, "keyboard/mouse" },
        { 0x1802, "find-me alert" }, { 0x1803, "link-loss alert" }, { 0x1809, "thermometer" },
        { 0x180A, "device information" }, { 0x180D, "heart rate" }, { 0x180F, "battery" },
        { 0x1810, "blood pressure" }, { 0x1812, "keyboard/mouse (BLE)" }, { 0x1816, "cycling speed" },
        { 0x1818, "cycling power" }, { 0x181A, "environment sensor" }, { 0x1826, "fitness machine" },
        { 0xFD5A, "Samsung SmartTag" }, { 0xFD6F, "Covid exposure notification" }, { 0xFE2C, "Google Fast Pair" },
        { 0xFE95, "Xiaomi MiBeacon" }, { 0xFE9F, "Google" }, { 0xFEAA, "Eddystone beacon" },
        { 0xFEEC, "Tile" }, { 0xFEED, "Tile" },
    };
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) if (list[i].id == u) return list[i].name;
    return NULL;
}

/* Apple "proximity pairing" model codes (AirPods / Beats), from public research
 * into Apple's adverts (e.g. the furiousMAC continuity notes) */
const char *bt_apple_model(unsigned m)
{
    static const struct { unsigned id; const char *name; } list[] = {
        { 0x0220, "AirPods (1st gen)" }, { 0x0F20, "AirPods (2nd gen)" }, { 0x1320, "AirPods (3rd gen)" },
        { 0x0E20, "AirPods Pro" }, { 0x1420, "AirPods Pro (2nd gen)" }, { 0x2420, "AirPods Pro (2nd gen, USB-C)" },
        { 0x0A20, "AirPods Max" }, { 0x0320, "Powerbeats3" }, { 0x0B20, "Powerbeats Pro" },
        { 0x0C20, "Beats Solo Pro" }, { 0x0520, "BeatsX" }, { 0x0620, "Beats Solo3" }, { 0x0920, "Beats Studio3" },
        { 0x1020, "Beats Flex" }, { 0x1120, "Beats Studio Buds" }, { 0x1220, "Beats Fit Pro" },
        { 0x1620, "Beats Studio Buds+" }, { 0x1720, "Beats Studio Pro" },
    };
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) if (list[i].id == m) return list[i].name;
    return NULL;
}

const char *bt_address_kind(const BtSurvey *d)
{
    if (strcmp(d->type, "le-random")) return "fixed (given by the maker)";
    unsigned top = (unsigned)strtoul(d->addr, NULL, 16) >> 6;   /* the first number's top 2 bits */
    if (top == 3) return "made up by the device, but it keeps it";
    if (top == 1) return "private, changes every few minutes";
    if (top == 0) return "private - a throw-away one";
    return "random (unusual kind)";
}

static int has_uuid(const BtSurvey *d, unsigned u)
{
    for (int i = 0; i < d->n_uuid; i++) if (d->uuid[i] == u) return 1;
    return 0;
}

/* ---------- Apple's maker bytes: a chain of [type][length][data] ---------- */

typedef struct { int model, findmy, ibeacon, nearby, other; unsigned char beacon[21]; } AppleClues;

static void apple_clues(const BtSurvey *d, AppleClues *a)
{
    memset(a, 0, sizeof *a);
    a->model = -1;
    for (int i = 0; i + 2 <= d->mfr_len; ) {
        int type = d->mfr[i], len = d->mfr[i + 1];
        const unsigned char *data = d->mfr + i + 2;
        if (i + 2 + len > d->mfr_len) len = d->mfr_len - i - 2;   /* (cut short: use what is there) */
        if (type == 0x07 && len >= 3) a->model = data[1] << 8 | data[2];   /* proximity pairing: [01][model] */
        else if (type == 0x12) a->findmy = 1;                             /* Find My network */
        else if (type == 0x02 && len >= 21) { a->ibeacon = 1; memcpy(a->beacon, data, 21); }
        else if (type == 0x10) a->nearby = 1;                             /* "nearby info": an iPhone/Mac/Watch */
        else a->other = 1;
        i += 2 + len;
    }
}

static const char *class_kind(unsigned cod)
{
    static const char *const major[] = { "device", "computer", "phone", "network device", "audio device",
                                         "keyboard/mouse/gamepad", "camera/printer", "wearable", "toy", "health device" };
    unsigned m = (cod >> 8) & 0x1f;
    if (m == 5) {                                           /* peripherals: which kind */
        unsigned minor = (cod >> 2) & 0x3f;
        if ((minor & 0x30) == 0x30) return "keyboard + mouse";
        if (minor & 0x10) return "keyboard";
        if (minor & 0x20) return "mouse";
        if ((minor & 0x0f) == 2) return "gamepad";
    }
    return m < sizeof major / sizeof major[0] ? major[m] : "device";
}

static const char *appearance_kind(unsigned ap)
{
    switch (ap >> 6) {
    case 1: return "phone";            case 2: return "computer";        case 3: return "watch";
    case 4: return "clock";            case 5: return "display";         case 6: return "remote control";
    case 7: return "glasses";          case 8: return "tag";             case 9: return "keyring";
    case 10: return "media player";    case 11: return "barcode scanner"; case 12: return "thermometer";
    case 13: return "heart-rate sensor"; case 14: return "blood-pressure meter";
    case 15: return (ap & 0x3f) == 1 ? "keyboard" : (ap & 0x3f) == 2 ? "mouse" : (ap & 0x3f) == 4 ? "gamepad" : "keyboard/mouse/gamepad";
    case 16: return "glucose meter";   case 17: return "running sensor";  case 18: return "cycling sensor";
    case 33: return "speaker";         case 34: return "audio source";
    case 37: return (ap & 0x3f) == 1 ? "earbuds" : (ap & 0x3f) == 3 ? "headphones" : "headset/earbuds";
    }
    return NULL;
}

int bt_is_tracker(const BtSurvey *d)
{
    AppleClues a;
    if (has_uuid(d, 0xFEED) || has_uuid(d, 0xFEEC) || has_uuid(d, 0xFD5A)) return 1;
    if (d->has_mfr && d->company == 0x004C) { apple_clues(d, &a); return a.findmy; }
    return 0;
}

void bt_guess(const BtSurvey *d, char *out, size_t n)
{
    AppleClues a;
    if (d->has_mfr && d->company == 0x004C) {
        apple_clues(d, &a);
        const char *m = a.model >= 0 ? bt_apple_model((unsigned)a.model) : NULL;
        if (m) { snprintf(out, n, "Likely: Apple %s - from Apple's model code in its advert (high)", m); return; }
        if (a.model >= 0) { snprintf(out, n, "Likely: Apple earbuds/headphones (model code %04X, not in our list) (medium)", (unsigned)a.model); return; }
        if (a.findmy) { snprintf(out, n, "Likely: an AirTag or other Find My item away from its owner - from Apple's Find My advert (medium)"); return; }
        if (a.ibeacon) { snprintf(out, n, "An iBeacon (a location beacon in a shop/building) - from its advert (high)"); return; }
        if (a.nearby) { snprintf(out, n, "Likely: an iPhone, iPad, Mac or Apple Watch - from Apple's \"nearby\" advert (medium)"); return; }
        snprintf(out, n, "Likely: an Apple device - from Apple's maker code (medium)");
        return;
    }
    if (has_uuid(d, 0xFEED) || has_uuid(d, 0xFEEC)) { snprintf(out, n, "Likely: a Tile tracker - from its Tile service (high)"); return; }
    if (has_uuid(d, 0xFD5A)) { snprintf(out, n, "Likely: a Samsung SmartTag - from its SmartTag service (high)"); return; }
    if (has_uuid(d, 0xFE2C)) { snprintf(out, n, "Likely: earbuds/speaker with Google Fast Pair - from its Fast Pair service (high)"); return; }
    if (d->has_mfr && d->company == 0x0006 && d->mfr_len >= 3 && d->mfr[0] == 0x03 && d->mfr[1] == 0x00 && d->mfr[2] == 0x80) {
        snprintf(out, n, "Likely: a device ready to pair with Windows (Microsoft Swift Pair) (high)"); return;
    }
    if (d->has_mfr && d->company == 0x0006 && d->mfr_len >= 1 && d->mfr[0] == 0x01) {
        snprintf(out, n, "Likely: a Windows computer - from Microsoft's \"connected devices\" advert (medium)"); return;
    }
    if (has_uuid(d, 0xFEAA)) { snprintf(out, n, "An Eddystone beacon (a Google location beacon) (high)"); return; }
    const char *k = d->has_app ? appearance_kind(d->appearance) : NULL;
    if (k) { snprintf(out, n, "Likely: a %s - from the kind it announces (medium)", k); return; }
    if (d->has_cod) { snprintf(out, n, "Likely: a %s - from its device class (medium)", class_kind(d->cod)); return; }
    if (has_uuid(d, 0x180D)) { snprintf(out, n, "Likely: a heart-rate sensor (chest belt/watch) - from its services (medium)"); return; }
    if (has_uuid(d, 0x1812) || has_uuid(d, 0x1124)) { snprintf(out, n, "Likely: a keyboard, mouse or gamepad - from its services (medium)"); return; }
    const char *c = d->has_mfr ? bt_company_name(d->company) : NULL;
    if (c) { snprintf(out, n, "Made by %s - nothing more in its advert (low)", c); return; }
    snprintf(out, n, "Not enough clues - it only sends its address (low)");
}

void bt_card(const BtSurvey *d, const char *list_file, char *out, size_t n)
{
    char guess[200], maker[96];
    size_t k = 0;
#define ADD(...) do { if (k < n) k += (size_t)snprintf(out + k, n - k, __VA_ARGS__); } while (0)
    bt_guess(d, guess, sizeof guess);
    ADD("%s\n\n", guess);
    ADD("Name      %s\n", d->name[0] ? d->name : "(it doesn't send a name)");
    ADD("Address   %s - %s\n", d->addr, bt_address_kind(d));
    ADD("Type      %s\n", !strcmp(d->type, "classic") ? "classic Bluetooth" : !strcmp(d->type, "dual") ? "classic + Low Energy" : "Bluetooth Low Energy");
    const char *company = d->has_mfr ? bt_company_name(d->company) : NULL;
    if (company) ADD("Maker     %s (from its maker code)\n", company);
    else if (d->has_mfr) ADD("Maker     company #%04X (not in the built-in list)\n", d->company);
    else if (strcmp(d->type, "le-random") && wi_maker(d->addr, list_file, maker, sizeof maker) == 0)
        ADD("Maker     %s (from its address)\n", maker);
    else ADD("Maker     unknown\n");
    if (d->has_cod) ADD("Class     %s\n", class_kind(d->cod));
    else if (d->has_app && appearance_kind(d->appearance)) ADD("Kind      %s\n", appearance_kind(d->appearance));
    if (d->n_uuid) {
        ADD("Services  ");
        for (int i = 0; i < d->n_uuid; i++) {
            const char *u = bt_uuid_name(d->uuid[i]);
            if (u) ADD("%s%s", i ? ", " : "", u);
            else ADD("%s#%04X", i ? ", " : "", d->uuid[i]);
        }
        ADD("\n");
    }
    /* rough distance: about -59 dBm at 1 m, losing 25 dB for every 10x further. Walls change it a lot. */
    double m = pow(10.0, (-59.0 - d->rssi) / 25.0);
    if (m < 1) ADD("Signal    %d dBm, very close (under 1 m)", d->rssi);
    else ADD("Signal    %d dBm, about %.0f m away", d->rssi, m);
    if (d->has_tx) ADD("  (sends at %d dBm)", d->tx_power);
    if (bt_is_tracker(d)) ADD("\nTracker   yes - if it stays near you for a long time, check who it belongs to");
    if (d->has_mfr) {                                       /* the raw maker bytes, for tinkering */
        ADD("\nRaw data  %04X:", d->company);
        for (int i = 0; i < d->mfr_len; i++) ADD("%02X", d->mfr[i]);
    }
#undef ADD
}

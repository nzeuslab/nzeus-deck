/* carscan.c - the READ-ONLY health scan of any car (see carscan.h).
 *
 * What it asks, in order (every question goes through obd.c's whitelist):
 *  1. ATRV                  the battery voltage (the adapter measures it)
 *  2. which engine / gearbox computers are there (11-bit CAN cars):
 *     ATH1 + 0100 to everyone (7DF) with headers on: each answer starts with
 *     the computer's address (7E8 = engine, 7E9 = gearbox ...), then ATH0.
 *     0902 = the VIN (make + model year).
 *  3. each of those computers on its own (ATSH 7E0 + ATCRA 7E8 ...):
 *     0101 check-engine light + self-tests, 03 stored codes, 07 pending codes,
 *     0A permanent codes, 01A6 odometer (newer cars only); the engine
 *     computer also gives a snapshot of the live values.
 *     Older cars (not 11-bit CAN): the same questions to everyone at once.
 *  4. every other module (11-bit CAN only): each diagnostic address 0x700-0x7F7
 *     (not 7DF = "everyone", not 7E8-7EF = the engine computers' answers) is
 *     asked UDS 19 02 AF "your fault codes?" with a short timeout (ATST19 =
 *     100 ms). Whoever answers is listed with its codes. A module's answer
 *     address (its address + 8) is never asked as if it were a module.
 *  5. Ford only (the VIN says Ford): Ford's module names, each module's
 *     odometer (22 DD01: the mileage check) and, with an OBDLink (STN chip),
 *     Ford's second bus (MS-CAN) for the body modules older Fords keep there.
 *  6. GM only (the VIN says Chevrolet / GMC / Cadillac): GM's body modules speak
 *     GM's own diagnostic language (GMLAN, GMW3110), not UDS, on other addresses:
 *     request 0x241-0x24F, answer on request + 0x400 (BCM 0x241 -> 0x641). Each
 *     address is asked "1A 90" (GM's ReadDataByIdentifier, identifier 90 =
 *     the VIN); whoever answers (or politely refuses: 7F 1A xx) is listed, and
 *     its identifiers B0, C1, CB are read and kept as exact bytes. Because the
 *     answer is not on address + 8, the adapter is told where its "send the rest"
 *     reply goes (ATFCSH = the module's address, ATFCSM1); obd.c puts that back.
 *     Their fault codes are NOT read yet (GM sends them as a stream of separate
 *     frames on another address, not tested with this adapter): they stay
 *     unknown, never healthy.
 *     Source for the addresses and service bytes: docs/feature-coding-research-
 *     20261012.md section 2 (community GMW3110 implementations; confirm live).
 * Nothing here writes, codes, resets or clears anything: there is no such
 * question in this file, and obd.c would refuse it anyway. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "carsum.h"
#include "fordscan.h"
#include "obd.h"

/* the scan's phases */
enum { P_CLEAN, P_FIND, P_OBD, P_SHORT, P_MODS, P_MS_SWITCH, P_MS, P_GM_SETUP, P_GM, P_FINISH };
#define MAX_ECUS  8                   /* OBD-II allows 8 engine/gearbox computers (7E8-7EF) */
#define MAX_ADDR  260                 /* 0x700-0x7F7 (minus 9) + Ford's few others */
#define GM_FIRST  0x241               /* GM (GMLAN) physical request addresses searched */
#define GM_LAST   0x24F
#define GM_REPLY  0x400               /* a GM module answers on its address + 0x400 */

static struct {
    int state, phase, sub, asked;
    CarScan *s;
    int ford, can11;
    int ecu[MAX_ECUS], ecu_n, ecu_i;  /* the computers' REQUEST addresses (7E0...); 0 = "everyone" (older cars) */
    int live_i;                       /* the next live value to ask the engine computer */
    int ecu_codes;                    /* valid stored/pending/permanent replies for this ECU */
    int ready_done;                   /* the self-test line is filled (from the first computer) */
    short addr[MAX_ADDR];             /* module addresses to ask */
    int addr_n, addr_i;
    int ms_list[48], ms_n, ms_i;      /* Ford: silent MS-CAN modules (fordscan table index) */
    int gm, gm_addr, gm_searched;     /* GM car; the GM address being asked; 1 = the GM search ran,
                                         -1 = the adapter refused the flow-control setting */
    int mod;                          /* s->modules index of the module being asked, -1 = none */
    int done, total;
} F;

static const int identity_dids[4] = { 0xF187, 0xF188, 0xF189, 0xF191 };
/* GM's identifiers (one byte), as named by community GMW3110 tools - their meaning
 * is not confirmed here, so only the exact bytes are kept: 90 VIN, B0 the module's
 * diagnostic address, C1 software module 1 number, CB end-model part number */
static const int gm_dids[4] = { 0x90, 0xB0, 0xC1, 0xCB };

const char *carscan_identity_name(const ScanModule *m, int k)
{
    static const char *const uds[4] = { "F187", "F188", "F189", "F191" };
    static const char *const gm[4] = { "GM90", "GMB0", "GMC1", "GMCB" };
    if (k < 0 || k >= 4) return "";
    return m && m->protocol == SCAN_PROTO_GMLAN ? gm[k] : uds[k];
}

int gm_parse_identity(const char *answer, int did, char *hex, int size)
{
    unsigned char start[2] = { 0x5A, (unsigned char)did };
    unsigned char data[68];
    if (!hex || size < 1) return -1;
    hex[0] = 0;
    int len = obd_find_message(answer, start, 2, data, sizeof data);
    if (len <= 2 || len > 66 || (len - 2) * 2 + 1 > size) return -1;
    for (int i = 2; i < len; i++) snprintf(hex + (i - 2) * 2, 3, "%02X", data[i]);
    return len - 2;
}

int gm_answered(const char *answer, int did)
{
    unsigned char yes[2] = { 0x5A, (unsigned char)did }, no[2] = { 0x7F, 0x1A }, m[8];
    if (obd_find_message(answer, yes, 2, m, sizeof m) > 0) return 1;
    return obd_find_message(answer, no, 2, m, sizeof m) >= 3 && m[2] != 0x78;
}

/* Preserve exact bytes, never turn an unknown or truncated identifier into a match. */
int uds_parse_identity(const char *answer, int did, char *hex, int size)
{
    unsigned char start[3] = { 0x62, (unsigned char)(did >> 8), (unsigned char)did };
    unsigned char data[68];
    if (!hex || size < 1) return -1;
    hex[0] = 0;
    int len = obd_find_message(answer, start, 3, data, sizeof data);
    if (len <= 3 || len > 67 || (len - 3) * 2 + 1 > size) return -1;
    for (int i = 3; i < len; i++) snprintf(hex + (i - 3) * 2, 3, "%02X", data[i]);
    return len - 3;
}

static void identify(const char *answer, int index)
{
    if (F.mod < 0 || index < 0 || index >= 4) return;
    ScanModule *m = &F.s->modules[F.mod];
    int n = m->protocol == SCAN_PROTO_GMLAN
        ? gm_parse_identity(answer, gm_dids[index], m->identity[index], sizeof m->identity[index])
        : uds_parse_identity(answer, identity_dids[index], m->identity[index], sizeof m->identity[index]);
    m->identity_status[index] = n > 0 ? 1 : -1;
}

int carscan_state(void) { return F.state; }
int carscan_is_ford(const CarScan *s) { return s && !strcmp(s->make_en, "Ford"); }
int carscan_is_gm(const CarScan *s)
{
    return s && (!strcmp(s->make_en, "Chevrolet") || !strcmp(s->make_en, "GMC") || !strcmp(s->make_en, "Cadillac"));
}

/* ---------- reading the answers ---------- */
int uds_dtc_state(unsigned char st)
{
    /* the UDS status byte (ISO 14229): bit 0 testFailed = failing right now,
     * bit 2 pendingDTC, bit 3 confirmedDTC. "Not present" = stored but not
     * failing now = history. */
    if (st & 0x01) return DTC_ACTIVE;
    if (st & 0x08) return DTC_HISTORY;
    if (st & 0x04) return DTC_PENDING;
    return DTC_HISTORY;
}

void uds_dtc_text(const unsigned char *b, char out[8])
{
    static const char letter[] = "PCBU";                  /* the same first 2 bytes as OBD codes */
    snprintf(out, 8, "%c%u%X%X%X", letter[b[0] >> 6], (unsigned)(b[0] >> 4) & 3, b[0] & 15, b[1] >> 4, b[1] & 15);
}

int uds_parse_dtcs(const char *answer, CarScan *s, const char *module)
{
    /* 59 02 <mask the module supports> then 4 bytes per code: 2 code bytes,
     * failure type, status */
    static const unsigned char start[2] = { 0x59, 0x02 };
    unsigned char m[600];
    int len = obd_find_message(answer, start, 2, m, sizeof m), added = 0;
    if (len < 3 || (len - 3) % 4) return -1;              /* missing mask or an incomplete code record */
    for (int i = 3; i + 3 < len; i += 4) {
        if (!m[i] && !m[i + 1] && !m[i + 2]) continue;   /* (empty record) */
        char code[8];
        uds_dtc_text(m + i, code);
        int st = uds_dtc_state(m[i + 3]);
        ScanDtc *d = carscan_find_dtc(s, module, code);
        if (d) {                                          /* already there (from OBD mode 03/07/0A): the exact status wins */
            d->failure_type = m[i + 2];
            d->status = m[i + 3];
            if (d->state != DTC_PERMANENT || st == DTC_ACTIVE) d->state = st;
            added++;
        } else if (carscan_add_dtc(s, module, code, m[i + 2], m[i + 3], st)) added++;
        else return -1;                                  /* the report cannot hold the whole list */
    }
    return added;
}

int uds_parse_odometer(const char *answer, double *km)
{
    static const unsigned char start[3] = { 0x62, 0xDD, 0x01 };
    unsigned char m[16];
    if (obd_find_message(answer, start, 3, m, sizeof m) < 6) return 0;
    long v = (long)m[3] << 16 | (long)m[4] << 8 | m[5];
    if (v == 0xFFFFFF) return 0;                          /* "not available" */
    *km = (double)v;
    return 1;
}

/* the odometer from OBD-II PID A6 (newer cars): 4 bytes, 0.1 km each */
static int obd_odometer(const char *answer, double *km)
{
    static const unsigned char start[2] = { 0x41, 0xA6 };
    unsigned char m[16];
    if (obd_find_message(answer, start, 2, m, sizeof m) < 6) return 0;
    unsigned long v = (unsigned long)m[2] << 24 | (unsigned long)m[3] << 16 | (unsigned long)m[4] << 8 | m[5];
    if (v == 0xFFFFFFFFul) return 0;
    *km = v / 10.0;
    return 1;
}

static int hexdigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

int obd_parse_responders(const char *answer, int *ids, int max)
{
    /* with headers on, every answer line starts with who sent it:
     * "7E8 06 41 00 BE 3F A8 13" (or without spaces, ATS0: "7E8064100BE3FA813") */
    int n = 0;
    const char *p = answer;
    while (*p) {
        char line[80];
        int k = 0;
        while (*p && *p != '\r' && *p != '\n') { if (*p != ' ' && k < 79) line[k++] = *p; p++; }
        while (*p == '\r' || *p == '\n') p++;
        line[k] = 0;
        if (k < 7) continue;                              /* "SEARCHING...", "NO DATA" ... are not answers */
        int id = 0, ok = 1;
        for (int i = 0; i < k; i++) if (hexdigit(line[i]) < 0) ok = 0;
        if (!ok) continue;
        for (int i = 0; i < 3; i++) id = id * 16 + hexdigit(line[i]);
        if (id < 0x7E8 || id > 0x7EF) continue;           /* only the OBD answer addresses */
        int seen = 0;
        for (int i = 0; i < n; i++) if (ids[i] == id) seen = 1;
        if (!seen && n < max) ids[n++] = id;
    }
    for (int i = 1; i < n; i++)                           /* lowest first: 7E8 (engine) leads */
        for (int k = i; k > 0 && ids[k] < ids[k - 1]; k--) { int t = ids[k]; ids[k] = ids[k - 1]; ids[k - 1] = t; }
    return n;
}

/* ---------- the modules ---------- */
/* the module at this request address: found, or added and named.
 * Names only where certain: 7E0 = engine, 7E1 = gearbox (the OBD-II rule),
 * Ford's own names on a Ford; anything else is "Module 0x760". */
static int module_at(int id)
{
    if (id < 0 || id > 0x7FF) return -1;                  /* this scanner uses 11-bit CAN addresses */
    CarScan *s = F.s;
    int bus = F.phase == P_MS;
    for (int i = 0; i < s->module_count; i++)
        if (s->modules[i].can_id == id && s->modules[i].bus == bus) return i;
    if (s->module_count >= SCAN_MAX_MODULES) return -1;
    ScanModule *m = &s->modules[s->module_count];
    memset(m, 0, sizeof *m);
    m->can_id = id;
    m->bus = bus;
    const FordModule *fm = F.ford ? fordscan_by_id(id) : NULL;
    if (F.phase == P_GM) {                                /* GM's own language: no names (not confirmed) */
        m->protocol = SCAN_PROTO_GMLAN;
        snprintf(m->code, sizeof m->code, "0x%03X", (unsigned)id);
        snprintf(m->name_en, sizeof m->name_en, "GM module 0x%03X", (unsigned)id);
        snprintf(m->name_ar, sizeof m->name_ar, "وحدة جي إم 0x%03X", (unsigned)id);
    } else if (fm) {
        snprintf(m->code, sizeof m->code, "%s", fm->code);
        snprintf(m->name_en, sizeof m->name_en, "%s", fm->en);
        snprintf(m->name_ar, sizeof m->name_ar, "%s", fm->ar);
    } else if (id == 0) {                                 /* older cars: everyone answers together */
        snprintf(m->code, sizeof m->code, "OBD");
        snprintf(m->name_en, sizeof m->name_en, "Engine computer (standard OBD-II)");
        snprintf(m->name_ar, sizeof m->name_ar, "كمبيوتر المحرك (OBD-II القياسي)");
    } else if (id == 0x7E0) {
        snprintf(m->code, sizeof m->code, "ECM");
        snprintf(m->name_en, sizeof m->name_en, "Engine computer");
        snprintf(m->name_ar, sizeof m->name_ar, "كمبيوتر المحرك");
    } else if (id == 0x7E1) {
        snprintf(m->code, sizeof m->code, "TCM");
        snprintf(m->name_en, sizeof m->name_en, "Gearbox computer");
        snprintf(m->name_ar, sizeof m->name_ar, "كمبيوتر ناقل الحركة");
    } else {
        snprintf(m->code, sizeof m->code, "0x%03X", (unsigned)id);
        snprintf(m->name_en, sizeof m->name_en, "Module 0x%03X", (unsigned)id);
        snprintf(m->name_ar, sizeof m->name_ar, "وحدة 0x%03X", (unsigned)id);
    }
    return s->module_count++;
}

/* is this address the answer address of a module that already answered? */
static int is_answer_address(int a)
{
    for (int i = 0; i < F.s->module_count; i++)
        if (F.s->modules[i].answered && F.s->modules[i].can_id && F.s->modules[i].can_id + 8 == a) return 1;
    return 0;
}

/* the addresses to ask in the module search */
static void make_address_list(void)
{
    F.addr_n = 0;
    for (int i = 0; F.ford && i < fordscan_module_count(); i++)       /* Ford's few modules below 0x700 */
        if (fordscan_module(i)->id < 0x700 && F.addr_n < MAX_ADDR) F.addr[F.addr_n++] = (short)fordscan_module(i)->id;
    for (int a = 0x700; a <= 0x7F7 && F.addr_n < MAX_ADDR; a++) {
        if (a == 0x7DF || (a >= 0x7E8 && a <= 0x7EF)) continue;      /* "everyone" and the OBD answer addresses */
        F.addr[F.addr_n++] = (short)a;
    }
}

static void add_odometer(const char *module, double km)
{
    CarScan *s = F.s;
    if (s->odo_count >= SCAN_MAX_ODO) return;
    snprintf(s->odo[s->odo_count].module, sizeof s->odo[0].module, "%s", module);
    s->odo[s->odo_count++].km = km;
}

static void add_note(const char *en, const char *ar)
{
    CarScan *s = F.s;
    if (s->note_count >= SCAN_MAX_NOTES) return;
    snprintf(s->notes_en[s->note_count], sizeof s->notes_en[0], "%s", en);
    snprintf(s->notes_ar[s->note_count], sizeof s->notes_ar[0], "%s", ar);
    s->note_count++;
}

/* one live value for the snapshot: "Engine speed" = "1726 rpm" */
static void add_live(int pid, double v)
{
    CarScan *s = F.s;
    if (s->live_count >= SCAN_MAX_LIVE) return;
    ScanLive *L = &s->live[s->live_count++];
    snprintf(L->name_en, sizeof L->name_en, "%s", obd_pid_name(pid));
    snprintf(L->name_ar, sizeof L->name_ar, "%s", obd_pid_name_ar(pid));
    if (pid == PID_RUNTIME) snprintf(L->value, sizeof L->value, "%d:%02d min", (int)v / 60, (int)v % 60);
    else {
        char num[16];
        snprintf(num, sizeof num, obd_pid_format(pid), v > -0.05 && v < 0.05 ? 0.0 : v);   /* no "-0.0" */
        snprintf(L->value, sizeof L->value, "%s %s", num, obd_pid_unit(pid));
    }
}

/* ---------- the scan ---------- */
void carscan_start(CarScan *s)
{
    memset(s, 0, sizeof *s);
    memset(&F, 0, sizeof F);
    F.s = s;
    F.mod = -1;
    s->when = time(NULL);
    s->odometer_km = -1;
    s->odo_agree = -1;
    F.can11 = obd_can11();
    F.total = 1;
    F.state = CS_RUNNING;
    F.phase = P_CLEAN;
    obd_session(0);                                       /* first: the adapter back to normal (if a scan was stopped) */
}

void carscan_cancel(void)
{
    if (F.state != CS_RUNNING) return;
    F.state = CS_CANCELLED;
    obd_session(0);                                       /* obd.c puts the header, headers, timeout and bus back */
}

void carscan_progress(int *done, int *total, char *line, int line_n)
{
    *done = F.done;
    *total = F.total > F.done ? F.total : F.done + 1;
    if (F.phase == P_CLEAN || F.phase == P_FIND)
        snprintf(line, (size_t)line_n, "Asking which engine and gearbox computers are there...");
    else if (F.phase == P_OBD)
        snprintf(line, (size_t)line_n, "Standard OBD-II: %s", F.mod >= 0 ? F.s->modules[F.mod].name_en : "the engine computer");
    else if (F.phase == P_SHORT || F.phase == P_MODS)
        snprintf(line, (size_t)line_n, "Looking for modules: address 0x%03X (%d of %d)",
                 F.addr_i < F.addr_n ? (unsigned)F.addr[F.addr_i] : 0x7F7u, F.addr_i < F.addr_n ? F.addr_i + 1 : F.addr_n, F.addr_n);
    else if (F.phase == P_MS_SWITCH || F.phase == P_MS) {
        const FordModule *fm = fordscan_module(F.ms_i < F.ms_n ? F.ms_list[F.ms_i] : 0);
        snprintf(line, (size_t)line_n, "Ford's second bus (MS-CAN): %s (%s)", fm ? fm->code : "", fm ? fm->en : "");
    } else if (F.phase == P_GM_SETUP || F.phase == P_GM)
        snprintf(line, (size_t)line_n, "GM modules (GMLAN): address 0x%03X (%d of %d)",
                 (unsigned)F.gm_addr, F.gm_addr - GM_FIRST + 1, GM_LAST - GM_FIRST + 1);
    else snprintf(line, (size_t)line_n, "Writing the report...");
}

static void finish(void)
{
    CarScan *s = F.s;
    obd_session(0);
    snprintf(s->adapter, sizeof s->adapter, "%s", obd_adapter());
    snprintf(s->protocol, sizeof s->protocol, "%s", obd_protocol_name());
    int unreached = 0, silent_lists = 0, unknown = 0;
    if (F.ford && F.can11) {
        /* every Ford has a body module and a cluster: if a Ford answered but those
         * didn't, they are on a bus this adapter could not reach */
        int bcm = 0, ipc = 0, any = 0;
        for (int i = 0; i < s->module_count; i++) {
            any |= s->modules[i].answered && s->modules[i].can_id;
            if (!strcmp(s->modules[i].code, "BdyCM")) bcm = s->modules[i].answered;
            if (!strcmp(s->modules[i].code, "IPC")) ipc = s->modules[i].answered;
        }
        unreached = any && (!bcm || !ipc);
    }
    int gm_found = 0;
    for (int i = 0; i < s->module_count; i++) {
        if (s->modules[i].protocol == SCAN_PROTO_GMLAN) gm_found += s->modules[i].answered;
        else silent_lists += s->modules[i].answered && !s->modules[i].codes_read;
    }
    for (int i = 0; i < s->dtc_count; i++) unknown += !s->dtcs[i].meaning_en[0];

    /* the notes at the end of the report */
    char en[200], ar[320];
    if (!s->vin[0])
        add_note("The car did not give its VIN, so the make and model year are unknown.",
                 "لم تعطِ السيارة رقم الشاصي، لذلك الشركة المصنعة وسنة الصنع غير معروفتين.");
    else if (!s->make_en[0]) {
        snprintf(en, sizeof en, "The make could not be told from the VIN (it starts %.3s).", s->vin);
        snprintf(ar, sizeof ar, "تعذّر معرفة الشركة المصنعة من رقم الشاصي (يبدأ بـ %.3s).", s->vin);
        add_note(en, ar);
    }
    if (F.can11)
        add_note("Modules were found by asking every diagnostic address on the main bus (0x700-0x7F7, HS-CAN). "
                 "Modules on another bus, or behind a gateway that does not pass the question on, are not listed.",
                 "تم العثور على الوحدات بسؤال كل عناوين الفحص على الشبكة الرئيسية (0x700-0x7F7، HS-CAN). "
                 "الوحدات على شبكة أخرى، أو خلف بوابة لا تمرر السؤال، غير مذكورة.");
    else
        add_note("This car does not use 11-bit CAN, so only the standard OBD-II data of its engine computers was read (no module search).",
                 "هذه السيارة لا تستخدم شبكة CAN بعناوين 11 بت، لذلك قُرئت بيانات OBD-II القياسية لكمبيوترات المحرك فقط (بدون البحث عن الوحدات).");
    if (F.gm_searched < 0)
        add_note("GM: the adapter refused the flow-control setting GM modules need, so GM's own modules (GMLAN) were not searched.",
                 "جي إم: رفض المحوّل إعداد التحكم بالتدفق الذي تحتاجه وحدات جي إم، لذلك لم يتم البحث عن وحدات جي إم (GMLAN).");
    else if (F.gm_searched && gm_found) {
        snprintf(en, sizeof en, "GM: %d module%s answered GM's own language (GMLAN, 0x241-0x24F). Only identifiers were read; "
                 "%s fault codes were not read yet, so %s unknown.", gm_found, gm_found == 1 ? "" : "s",
                 gm_found == 1 ? "its" : "their", gm_found == 1 ? "it is" : "they are");
        snprintf(ar, sizeof ar, "جي إم: استجابت %d وحدة بلغة جي إم (GMLAN، 0x241-0x24F). قُرئت المعرّفات فقط؛ "
                 "لم تُقرأ رسائل الأعطال بعد، فحالتها غير معروفة.", gm_found);
        add_note(en, ar);
    } else if (F.gm_searched)
        add_note("GM: no module answered GM's own language (GMLAN) at 0x241-0x24F on the main bus. GM's single-wire bus is out of this adapter's reach.",
                 "جي إم: لم تستجب أي وحدة بلغة جي إم (GMLAN) على 0x241-0x24F في الشبكة الرئيسية. شبكة جي إم أحادية السلك خارج قدرة هذا المحوّل.");
    if (!F.ford && F.can11 && s->module_count > 0)
        add_note("Only the engine (0x7E0) and gearbox (0x7E1) computers are named; other modules are shown by their address, "
                 "because their names differ between car makers.",
                 "سُمّي كمبيوتر المحرك (0x7E0) وكمبيوتر ناقل الحركة (0x7E1) فقط؛ الوحدات الأخرى تظهر بعناوينها "
                 "لأن أسماءها تختلف بين الشركات المصنعة.");
    if (unknown)
        add_note("Codes marked Unknown are not in NZEUS's list. Codes such as P1..., B1..., C1... or U1... are the car maker's own "
                 "and mean different things on different makes: look them up for this make.",
                 "الأكواد المكتوب عليها غير معروف ليست في قائمة NZEUS. الأكواد مثل P1 و B1 و C1 و U1 خاصة بالشركة المصنعة "
                 "ومعناها يختلف من ماركة لأخرى: ابحث عن معناها لهذه الماركة.");
    if (silent_lists) {
        snprintf(en, sizeof en, "%d module%s answered but would not list %s fault codes (it may use an older diagnostic language).",
                 silent_lists, silent_lists == 1 ? "" : "s", silent_lists == 1 ? "its" : "their");
        snprintf(ar, sizeof ar, "استجابت %d وحدة لكنها لم تعرض رسائل أعطالها (قد تستخدم لغة فحص أقدم).", silent_lists);
        add_note(en, ar);
    }
    if (F.ford && s->odo_count)
        add_note("Ford: the mileage check reads the odometer each module keeps (Ford's DD01); its unit (1 km per count) "
                 "comes from owner forums, not from a Ford document.",
                 "فورد: فحص الممشى يقرأ العداد المخزن في كل وحدة (DD01)؛ وحدته (1 كم لكل عدّة) مأخوذة من منتديات الملاك "
                 "وليست من وثيقة من فورد.");
    carscan_summarize(s, unreached);
    F.state = CS_DONE;
}

/* the next phase after the engine computers */
static void after_obd(void)
{
    F.sub = 0;
    F.mod = -1;
    if (!F.can11) { F.phase = P_FINISH; return; }
    make_address_list();
    F.addr_i = 0;
    F.total += F.addr_n;
    F.phase = P_SHORT;                                    /* first: a short answer timeout (ATST19) */
}

/* the module search is over: Ford's MS-CAN (OBDLink only), else the end */
static void after_mods(void)
{
    F.sub = 0;
    F.mod = -1;
    F.phase = P_FINISH;
    if (F.gm) {                                           /* GM: its own modules next (11-bit CAN only, as here) */
        F.phase = P_GM_SETUP;
        F.gm_addr = GM_FIRST;
        F.total += GM_LAST - GM_FIRST + 1;
        return;
    }
    if (!F.ford) return;
    F.ms_n = 0;
    for (int i = 0; i < fordscan_module_count() && F.ms_n < 48; i++) {
        const FordModule *fm = fordscan_module(i);
        int there = 0;
        for (int k = 0; k < F.s->module_count; k++) if (F.s->modules[k].can_id == fm->id && F.s->modules[k].answered) there = 1;
        if (fm->ms && !there) F.ms_list[F.ms_n++] = i;
    }
    if (F.ms_n && obd_is_stn()) { F.phase = P_MS_SWITCH; F.total += F.ms_n; F.ms_i = 0; }
}

/* one engine computer done: the next one, or on to the module search */
static void next_ecu(void)
{
    F.done++;
    F.sub = 0;
    F.live_i = 0;
    F.ecu_codes = 0;
    if (++F.ecu_i < F.ecu_n) { F.mod = module_at(F.ecu[F.ecu_i]); return; }
    after_obd();
}

/* the OBD steps for each engine computer (sub) */
enum { E_SH, E_CRA, E_STATUS, E_STORED, E_PENDING, E_PERM, E_ODO, E_LIVE,
       E_ID0, E_ID1, E_ID2, E_ID3, E_DONE };
/* the steps for each module in the search (sub) */
enum { M_SH, M_CRA, M_DTC, M_ODO, M_ID0, M_ID1, M_ID2, M_ID3 };
/* the GM search: once (flow control), then each address (G_ID0 = the "anyone there?" question) */
enum { GS_FCSH, GS_FCSD, GS_FCSM };
enum { G_SH, G_CRA, G_FCSH, G_ID0, G_ID1, G_ID2, G_ID3 };

static int said_ok(const char *a) { return strstr(a, "OK") != NULL; }

/* one GM address done: the next one, or the end */
static void next_gm(void)
{
    F.done++;
    F.sub = G_SH;
    F.mod = -1;
    if (++F.gm_addr > GM_LAST) F.phase = P_FINISH;
}

static void handle_gm(const char *a)
{
    if (F.phase == P_GM_SETUP) {
        if (!said_ok(a)) {                                /* an adapter without these settings: no GM search */
            F.gm_searched = -1;
            F.done += GM_LAST - GM_FIRST + 1;
            F.phase = P_FINISH;
            return;
        }
        if (++F.sub > GS_FCSM) { F.gm_searched = 1; F.phase = P_GM; F.sub = G_SH; F.mod = -1; }
        return;
    }
    switch (F.sub) {
    case G_SH: case G_CRA: case G_FCSH: F.sub++; return;
    case G_ID0:
        F.mod = gm_answered(a, gm_dids[0]) ? module_at(F.gm_addr) : -1;
        if (F.mod < 0) break;
        F.s->modules[F.mod].answered = 1;                 /* codes_read stays 0: not asked (unknown) */
        identify(a, 0);
        F.sub++;
        return;
    default:
        identify(a, F.sub - G_ID0);
        if (F.sub < G_ID3) { F.sub++; return; }
        break;
    }
    next_gm();
}

static int obd_codes(const char *a, int kind)
{
    static const int resp[3] = { 0x43, 0x47, 0x4A };
    static const int state[3] = { DTC_STORED, DTC_PENDING, DTC_PERMANENT };
    unsigned char start = (unsigned char)resp[kind], data[600];
    int len = obd_find_message(a, &start, 1, data, sizeof data);
    if (obd_is_can()) {
        if (len < 2 || len < 2 + 2 * data[1] || data[1] > OBD_MAX_DTCS) return 0;
    } else if (len < 1 || (len - 1) % 2) return 0;
    char codes[OBD_MAX_DTCS][6];
    const char *mod = F.mod >= 0 ? F.s->modules[F.mod].code : "OBD";
    int n = obd_parse_codes(a, obd_is_can(), resp[kind], codes, OBD_MAX_DTCS);
    for (int i = 0; i < n; i++) {
        ScanDtc *d = carscan_find_dtc(F.s, mod, codes[i]);
        if (!d) {
            if (!carscan_add_dtc(F.s, mod, codes[i], 0, 0, state[kind])) return 0;
        }
        else if (kind == 2 && d->state != DTC_ACTIVE) d->state = DTC_PERMANENT;   /* stored AND permanent: say permanent */
    }
    return 1;
}

static void handle(const char *a)
{
    CarScan *s = F.s;
    double v;
    if (F.phase == P_FIND) {
        if (F.sub == 0 && obd_parse_pid(a, PID_VOLTS, &v)) s->battery_v = v;
        if (F.sub == 2) F.ecu_n = obd_parse_responders(a, F.ecu, MAX_ECUS);   /* answer addresses for now */
        if (F.sub == 4 && obd_parse_vin(a, s->vin)) {
            s->model_year = vin_model_year(s->vin);
            vin_make(s->vin, s);
            F.ford = carscan_is_ford(s);
            F.gm = carscan_is_gm(s);
        }
        if (++F.sub < 5) {
            if (!F.can11 && F.sub >= 1 && F.sub <= 3) F.sub = 4;          /* older cars: no headers question */
            return;
        }
        /* on to the computers one by one: their request address = answer - 8 */
        for (int i = 0; i < F.ecu_n; i++) F.ecu[i] -= 8;
        if (!F.can11 || !F.ecu_n) { F.ecu_n = 1; F.ecu[0] = 0; }            /* everyone at once */
        F.total += F.ecu_n;
        F.ecu_i = 0;
        F.sub = F.ecu[0] ? E_SH : E_STATUS;
        F.mod = module_at(F.ecu[0]);
        F.phase = P_OBD;
        F.done++;
        return;
    }
    if (F.phase == P_OBD) {
        ScanModule *m = F.mod >= 0 ? &s->modules[F.mod] : NULL;
        switch (F.sub) {
        case E_STATUS: {
            ObdInfo info;
            memset(&info, 0, sizeof info);
            if (obd_parse_readiness(a, &info)) {
                int mil = s->mil_on | info.mil_on;
                if (!F.ready_done) { carscan_set_readiness(s, &info); F.ready_done = 1; }
                s->mil_on = mil;
                s->mil_known = 1;
                s->ignition_on = 1;
                if (m) m->answered = 1;                    /* status alone does not prove its code lists were read */
            }
            break;
        }
        case E_STORED: case E_PENDING: case E_PERM: {
            int kind = F.sub - E_STORED;
            if (obd_codes(a, kind)) {
                F.ecu_codes |= 1 << kind;
                if (m) m->answered = 1;
            }
            if (m) m->codes_read = F.ecu_codes == 7;
            break;
        }
        case E_ODO:     if (obd_odometer(a, &v)) add_odometer(m ? m->code : "OBD", v); break;
        case E_LIVE:
            if (obd_parse_pid(a, F.live_i, &v)) {
                add_live(F.live_i, v);
                if (F.live_i == PID_RPM) { s->engine_running = v > 0; s->ignition_on = 1; }
                if (F.live_i == PID_VOLTS) s->battery_v = v;
            }
            F.live_i++;
            return;                                       /* (the next value is picked in carscan_poll) */
        case E_ID0: case E_ID1: case E_ID2: case E_ID3:
            identify(a, F.sub - E_ID0);
            break;
        default: break;
        }
        F.sub++;
        return;
    }
    if (F.phase == P_GM_SETUP || F.phase == P_GM) { handle_gm(a); return; }
    if (F.phase == P_SHORT) { F.phase = P_MODS; F.sub = M_SH; F.mod = -1; return; }
    if (F.phase == P_MS_SWITCH) { F.phase = P_MS; F.sub = M_SH; F.mod = -1; return; }
    /* the module search (P_MODS) and Ford's MS-CAN (P_MS) */
    int id = F.phase == P_MS ? fordscan_module(F.ms_list[F.ms_i])->id : F.addr[F.addr_i];
    int go_on = 0;
    switch (F.sub) {
    case M_SH: case M_CRA: F.sub++; return;
    case M_DTC: {
        /* someone is there if it answers with its list (59 02 ...) or with a
         * polite "I can't do that" (7F 19 xx; 7F 19 78 only means "wait") */
        static const unsigned char yes[2] = { 0x59, 0x02 }, no[2] = { 0x7F, 0x19 };
        unsigned char m8[8];
        int listed = obd_find_message(a, yes, 2, m8, sizeof m8) > 0;
        int neg = !listed && obd_find_message(a, no, 2, m8, sizeof m8) >= 3 && m8[2] != 0x78;
        F.mod = listed || neg ? module_at(id) : -1;
        if (F.mod >= 0) {
            ScanModule *sm = &s->modules[F.mod];
            sm->answered = 1;
            if (listed && uds_parse_dtcs(a, s, sm->code) >= 0) sm->codes_read = 1;
            go_on = 1;
        }
        break;
    }
    case M_ODO:
        if (uds_parse_odometer(a, &v) && F.mod >= 0) add_odometer(s->modules[F.mod].code, v);
        F.sub = M_ID0;
        return;
    case M_ID0: case M_ID1: case M_ID2: case M_ID3:
        identify(a, F.sub - M_ID0);
        if (F.sub < M_ID3) { F.sub++; return; }
        break;
    }
    if (go_on) { F.sub = F.ford ? M_ODO : M_ID0; return; }
    /* this address is done: the next one (never a found module's answer address) */
    F.done++;
    F.sub = M_SH;
    F.mod = -1;
    if (F.phase == P_MS) { if (++F.ms_i >= F.ms_n) F.phase = P_FINISH; return; }
    F.addr_i++;
    while (F.addr_i < F.addr_n && is_answer_address(F.addr[F.addr_i])) { F.addr_i++; F.done++; }
    if (F.addr_i >= F.addr_n) after_mods();
}

void carscan_poll(void)
{
    if (F.state != CS_RUNNING) return;
    if (obd_state() != OBD_READY) { carscan_cancel(); return; }  /* adapter unplugged / car gone */
    if (F.asked) {
        if (!obd_ask_done()) return;
        F.asked = 0;
        handle(obd_ask_answer());
        return;
    }
    if (F.phase == P_CLEAN) {                             /* wait until obd.c has put the adapter back */
        if (!obd_session_clean()) return;
        obd_session(1);
        F.phase = P_FIND;
        F.sub = 0;
    }
    char cmd[24] = "";
    int timeout = 3000;
    if (F.phase == P_FIND) {
        static const char *const find[5] = { "ATRV", "ATH1", "0100", "ATH0", "0902" };
        snprintf(cmd, sizeof cmd, "%s", find[F.sub]);
        if (F.sub == 2) timeout = 9000;                   /* the first question may search for the protocol */
    } else if (F.phase == P_OBD) {
        int id = F.ecu[F.ecu_i];
        if (F.sub == E_LIVE) {                            /* the engine computer's live values, only those the car has */
            while (F.live_i < PID_COUNT && (F.ecu_i != 0 || !obd_pid_supported(F.live_i))) F.live_i++;
            if (F.live_i >= PID_COUNT) F.sub = id && F.mod >= 0 && F.s->modules[F.mod].answered ? E_ID0 : E_DONE;
            else snprintf(cmd, sizeof cmd, "%s", obd_pid_cmd(F.live_i));
        }
        if (F.sub == E_DONE) { next_ecu(); return; }
        switch (F.sub) {
        case E_SH:      snprintf(cmd, sizeof cmd, "ATSH%03X", (unsigned)id); timeout = 1500; break;
        case E_CRA:     snprintf(cmd, sizeof cmd, "ATCRA%03X", (unsigned)id + 8); timeout = 1500; break;
        case E_STATUS:  snprintf(cmd, sizeof cmd, "0101"); break;
        case E_STORED:  snprintf(cmd, sizeof cmd, "03"); timeout = 5000; break;
        case E_PENDING: snprintf(cmd, sizeof cmd, "07"); timeout = 5000; break;
        case E_PERM:    snprintf(cmd, sizeof cmd, "0A"); timeout = 5000; break;
        case E_ODO:     snprintf(cmd, sizeof cmd, "01A6"); break;
        case E_ID0: case E_ID1: case E_ID2: case E_ID3:
            snprintf(cmd, sizeof cmd, "22%04X", identity_dids[F.sub - E_ID0]); timeout = 1200; break;
        default: break;                                   /* E_LIVE: cmd set above */
        }
    }
    if (F.phase == P_SHORT) snprintf(cmd, sizeof cmd, "ATST19");   /* 0x19 x 4 ms = 100 ms: silent addresses go fast */
    if (F.phase == P_MODS || F.phase == P_MS) {
        int id = F.phase == P_MS ? fordscan_module(F.ms_list[F.ms_i])->id : F.addr[F.addr_i];
        switch (F.sub) {
        case M_SH:  snprintf(cmd, sizeof cmd, "ATSH%03X", (unsigned)id); timeout = 1500; break;
        case M_CRA: snprintf(cmd, sizeof cmd, "ATCRA%03X", (unsigned)id + 8); timeout = 1500; break;
        case M_DTC: snprintf(cmd, sizeof cmd, "1902AF"); timeout = 5000; break;   /* a long list can take a while */
        case M_ODO: snprintf(cmd, sizeof cmd, "22DD01"); break;
        default:    snprintf(cmd, sizeof cmd, "22%04X", identity_dids[F.sub - M_ID0]); timeout = 1200; break;
        }
    }
    if (F.phase == P_MS_SWITCH) snprintf(cmd, sizeof cmd, "STP53");   /* OBDLink: MS-CAN, ISO 15765, 125 kbps */
    if (F.phase == P_GM_SETUP) {                          /* "send the rest" replies: to the module, "30 00 00" */
        if (F.sub == GS_FCSH) snprintf(cmd, sizeof cmd, "ATFCSH%03X", (unsigned)GM_FIRST);
        else if (F.sub == GS_FCSD) snprintf(cmd, sizeof cmd, "ATFCSD300000");
        else snprintf(cmd, sizeof cmd, "ATFCSM1");
        timeout = 1500;
    }
    if (F.phase == P_GM) {
        unsigned id = (unsigned)F.gm_addr;
        switch (F.sub) {
        case G_SH:   snprintf(cmd, sizeof cmd, "ATSH%03X", id); timeout = 1500; break;
        case G_CRA:  snprintf(cmd, sizeof cmd, "ATCRA%03X", id + GM_REPLY); timeout = 1500; break;
        case G_FCSH: snprintf(cmd, sizeof cmd, "ATFCSH%03X", id); timeout = 1500; break;
        default:     snprintf(cmd, sizeof cmd, "1A%02X", (unsigned)gm_dids[F.sub - G_ID0]); timeout = 1200; break;
        }
    }
    if (F.phase == P_FINISH) { finish(); return; }
    int r = obd_ask(cmd, timeout);
    if (r == 0) F.asked = 1;
    else if (r == -1) handle("");                         /* refused by the whitelist: as if nobody answered */
}

/* obd.c - OBD-II through an ELM327-type adapter (see obd.h).
 * The ELM327 command set is public (ELM Electronics' ELM327 data sheet), the
 * STN commands come from the "OBDLink Family Reference and Programming Manual"
 * (OBD Solutions), and the answers' meaning from SAE J1979 (OBD-II) and
 * ISO 14229 (UDS, the "language" most modern car modules speak). */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtc_db.h"
#include "nzeus_tick.h"
#include "obd.h"
#include "serial.h"

static const int bauds[] = { 38400, 115200, 9600 };     /* ELM327 clones, OBDLink/STN, old units */

/* every live value: the question, its names, how many data bytes, its usual range */
static const struct { const char *cmd, *name, *shortname, *unit, *fmt; int pid, bytes; double lo, hi; } pids[PID_COUNT] = {
    [PID_RPM]      = { "010C", "Engine speed", "RPM", "rpm", "%.0f", 0x0C, 2, 0, 7000 },
    [PID_SPEED]    = { "010D", "Speed", "Speed", "km/h", "%.0f", 0x0D, 1, 0, 200 },
    [PID_COOLANT]  = { "0105", "Coolant temperature", "Coolant", "C", "%.0f", 0x05, 1, 40, 120 },
    [PID_LOAD]     = { "0104", "Engine load", "Load", "%", "%.0f", 0x04, 1, 0, 100 },
    [PID_THROTTLE] = { "0111", "Throttle", "Throttle", "%", "%.0f", 0x11, 1, 0, 100 },
    [PID_FUEL]     = { "012F", "Fuel level", "Fuel", "%", "%.0f", 0x2F, 1, 0, 100 },
    [PID_VOLTS]    = { "ATRV", "Battery", "Battery", "V", "%.1f", 0, 0, 10, 16 },
    [PID_IAT]      = { "010F", "Intake air temperature", "Intake air", "C", "%.0f", 0x0F, 1, 0, 80 },
    [PID_MAF]      = { "0110", "Air flow (MAF)", "MAF", "g/s", "%.1f", 0x10, 2, 0, 200 },
    [PID_STFT1]    = { "0106", "Short fuel trim, bank 1", "STFT1", "%", "%.1f", 0x06, 1, -25, 25 },
    [PID_LTFT1]    = { "0107", "Long fuel trim, bank 1", "LTFT1", "%", "%.1f", 0x07, 1, -25, 25 },
    [PID_STFT2]    = { "0108", "Short fuel trim, bank 2", "STFT2", "%", "%.1f", 0x08, 1, -25, 25 },
    [PID_LTFT2]    = { "0109", "Long fuel trim, bank 2", "LTFT2", "%", "%.1f", 0x09, 1, -25, 25 },
    [PID_TIMING]   = { "010E", "Timing advance", "Timing", "deg", "%.1f", 0x0E, 1, -10, 40 },
    [PID_AMBIENT]  = { "0146", "Outside air temperature", "Outside", "C", "%.0f", 0x46, 1, 0, 55 },
    [PID_OILTEMP]  = { "015C", "Engine oil temperature", "Oil temp", "C", "%.0f", 0x5C, 1, 40, 140 },
    [PID_RUNTIME]  = { "011F", "Time since engine start", "Run time", "s", "%.0f", 0x1F, 2, 0, 3600 },
    [PID_FUELRATE] = { "015E", "Fuel rate", "Fuel rate", "L/h", "%.1f", 0x5E, 2, 0, 40 },
};
/* the same names in Arabic, for the report's live-data table */
static const char *const pid_ar[PID_COUNT] = {
    [PID_RPM] = "سرعة دوران المحرك", [PID_SPEED] = "سرعة السيارة", [PID_COOLANT] = "حرارة سائل التبريد",
    [PID_LOAD] = "حمل المحرك", [PID_THROTTLE] = "فتحة الخانق (الثروتل)", [PID_FUEL] = "مستوى الوقود",
    [PID_VOLTS] = "البطارية", [PID_IAT] = "حرارة الهواء الداخل", [PID_MAF] = "تدفق الهواء (MAF)",
    [PID_STFT1] = "تعديل الوقود القصير، الصف 1", [PID_LTFT1] = "تعديل الوقود الطويل، الصف 1",
    [PID_STFT2] = "تعديل الوقود القصير، الصف 2", [PID_LTFT2] = "تعديل الوقود الطويل، الصف 2",
    [PID_TIMING] = "تقديم الشرارة", [PID_AMBIENT] = "حرارة الجو الخارجي", [PID_OILTEMP] = "حرارة زيت المحرك",
    [PID_RUNTIME] = "الوقت منذ تشغيل المحرك", [PID_FUELRATE] = "استهلاك الوقود",
};
static const char *const init_cmds[] = { "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0", "0100", "ATDPN" };
#define INIT_STEPS 7
/* fault codes: the question and the first byte of the answer */
static const char *const code_cmds[CODES_KINDS] = { "03", "07", "0A" };
static const int code_resp[CODES_KINDS] = { 0x43, 0x47, 0x4A };
/* readiness + freeze frame + vehicle info, asked one after the other */
enum { I_STATUS, I_FFDTC, I_FF0, I_VIN = I_FF0 + FF_COUNT, I_CALID, I_ECU, I_COUNT };
static const char *const info_cmds[I_COUNT] = { "0101", "020200", "020C00", "020D00", "020500", "020400", "0902", "0904", "090A" };
static const int ff_pid[FF_COUNT] = { PID_RPM, PID_SPEED, PID_COOLANT, PID_LOAD };

/* putting the adapter back after a module scan: ATSP, ATSH7DF, ATCRA, ATH0, ATST32,
 * and ATFCSM0 (automatic flow control again) only if the GM search changed it */
#define RESTORE_STEPS 6

/* what the question waiting for its answer was */
enum { K_NONE, K_RESET, K_INIT, K_ID, K_SUP, K_LIVE, K_CODES, K_CLEAR, K_INFO, K_ASK, K_RESTORE };

static struct {
    int state, fd, baud_i, step, poll_i, can;
    char port[48], elm[48], adapter[48], describe[160];
    char proto;                      /* protocol number from ATDPN ('6' = CAN 11 bit 500k) */
    char rx[4096];
    int rxlen;
    char cmd[32];                    /* the question waiting for its answer */
    int kind, arg;                   /* K_NONE = nothing waiting */
    uint32_t sent_at, timeout, retry_at;
    ObdLive live;
    unsigned sup[4];                 /* which PIDs the car has (01-20, 21-40, 41-60, 61-80) */
    int sup_known, sup_next;
    int focus, focus_turn;
    int codes_wanted[CODES_KINDS], codes_ready[CODES_KINDS], codes_n[CODES_KINDS];
    char codes[CODES_KINDS][OBD_MAX_DTCS][6];
    int clear_wanted, clear_result, clear_armed;
    int info_step, info_ready;
    ObdInfo info;
    char ask_cmd[32];
    int ask_pending, ask_done, ask_timeout;
    char answer[4096];
    int session, hdr_dirty, bus_dirty, fc_dirty, restore_step;
    int id_step, stn, refused;
} O = { .fd = -1, .focus = -1, .info_step = -1 };

int obd_state(void) { return O.state; }
const ObdLive *obd_live(void) { return &O.live; }
int obd_codes_ready(int k) { return k >= 0 && k < CODES_KINDS && O.codes_ready[k]; }
int obd_codes_count(int k) { return k >= 0 && k < CODES_KINDS ? O.codes_n[k] : 0; }
const char *obd_code(int k, int i) { return k >= 0 && k < CODES_KINDS && i >= 0 && i < O.codes_n[k] ? O.codes[k][i] : ""; }
int obd_dtcs_ready(void) { return obd_codes_ready(CODES_STORED); }
int obd_dtc_count(void) { return obd_codes_count(CODES_STORED); }
const char *obd_dtc(int i) { return obd_code(CODES_STORED, i); }
int obd_clear_result(void) { return O.clear_result; }
int obd_info_ready(void) { return O.info_ready; }
const ObdInfo *obd_info(void) { return &O.info; }
int obd_is_stn(void) { return O.stn; }
int obd_refused_count(void) { return O.refused; }
int obd_ask_done(void) { return O.ask_done; }
const char *obd_ask_answer(void) { return O.answer; }
int obd_session_clean(void) { return !O.hdr_dirty && !O.bus_dirty && !O.fc_dirty; }
void obd_session(int on) { O.session = on; }
void obd_focus_pid(int i) { O.focus = i; }

const char *obd_pid_name(int i) { return i >= 0 && i < PID_COUNT ? pids[i].name : ""; }
const char *obd_pid_cmd(int i) { return i >= 0 && i < PID_COUNT ? pids[i].cmd : ""; }
const char *obd_pid_name_ar(int i) { return i >= 0 && i < PID_COUNT && pid_ar[i] ? pid_ar[i] : ""; }
const char *obd_pid_short(int i) { return i >= 0 && i < PID_COUNT ? pids[i].shortname : ""; }
const char *obd_pid_unit(int i) { return i >= 0 && i < PID_COUNT ? pids[i].unit : ""; }
const char *obd_pid_format(int i) { return i >= 0 && i < PID_COUNT ? pids[i].fmt : "%g"; }
void obd_pid_range(int i, double *lo, double *hi) { *lo = pids[i].lo; *hi = pids[i].hi; }

/* is PID number p (1..0x80) in the car's "supported" lists? */
static int sup_bit(int p) { return p >= 1 && p <= 0x80 && (O.sup[(p - 1) / 32] >> (31 - (p - 1) % 32)) & 1; }

int obd_pid_supported(int i)
{
    if (i < 0 || i >= PID_COUNT) return 0;
    if (i == PID_VOLTS) return 1;                                /* the battery: the adapter measures it */
    if (!O.sup_known) return i <= PID_FUEL;                      /* not told yet: the basic ones */
    return sup_bit(pids[i].pid);
}

const char *obd_adapter(void) { return O.adapter[0] ? O.adapter : O.elm; }

int obd_is_can(void) { return O.can; }

/* protocol 6 = CAN 11-bit 500 kbps, 8 = CAN 11-bit 250 kbps (ATDPN) */
int obd_can11(void) { return O.can && (O.proto == '6' || O.proto == '8'); }

const char *obd_protocol_name(void)
{
    static const char *const names[] = { "automatic", "SAE J1850 PWM", "SAE J1850 VPW", "ISO 9141-2",
        "ISO 14230-4 KWP (slow init)", "ISO 14230-4 KWP (fast init)", "ISO 15765-4 CAN 11/500",
        "ISO 15765-4 CAN 29/500", "ISO 15765-4 CAN 11/250", "ISO 15765-4 CAN 29/250", "SAE J1939 CAN" };
    int p =isdigit((unsigned char)O.proto) ? O.proto - '0' : O.proto == 'A' ? 10 : -1;
    return p >= 0 && p <= 10 ? names[p] : "unknown";
}

const char *obd_describe(void)
{
    switch (O.state) {
    case OBD_OFF:       return O.describe[0] ? O.describe : OBD_NO_ADAPTER;
    case OBD_SEARCHING: snprintf(O.describe, sizeof O.describe, "Talking to the adapter on %s (%d baud)...", O.port, bauds[O.baud_i]); break;
    case OBD_INIT:      snprintf(O.describe, sizeof O.describe, "%s found - asking the car...", obd_adapter()); break;
    case OBD_NO_CAR:    snprintf(O.describe, sizeof O.describe, "%s found - no answer from the car. Ignition on?", obd_adapter()); break;
    case OBD_READY:     snprintf(O.describe, sizeof O.describe, "%s - %s", obd_adapter(), O.can ? "CAN" : "older protocol"); break;
    }
    return O.describe;
}

/* ---------- THE WHITELIST: the only things NZEUS may ever send ----------
 * Adapter settings (they change the adapter, never the car):
 *   ATZ ATE0 ATL0 ATS0 ATH0 ATH1 ATI ATDPN ATRV ATSP<n> ATSH<3 hex> ATCRA ATCRA<3 hex>
 *   ATST<2 hex> ATAT0-2 ATCAF1, STN: STI STDI STP33 (HS-CAN) STP53 (Ford MS-CAN)
 *   ATFCSH<3 hex> ATFCSD300000 ATFCSM0 ATFCSM1: who the adapter's "go on, send the
 *   rest" reply (ISO 15765 flow control) goes to - needed for GM modules, which
 *   answer on their address + 0x400 instead of + 8 (the adapter's own guess)
 * Messages to the car (hex):
 *   OBD modes 01 02 09 (read data), 03 07 0A (read codes)
 *   04 (clear codes) ONLY when the Car app's clear flow armed it (clear_ok)
 *   UDS 22 (ReadDataByIdentifier), 19 01 / 19 02 (ReadDTCInformation by status mask),
 *   3E 00 / 3E 80 (TesterPresent)
 *   GM (GMLAN, GMW3110) 1A <one byte> (ReadDataByIdentifier: GM's read, like UDS 22)
 * Everything else is refused: 10 session change, 11 reset, 14 clear, 27 security,
 * 2E write, 2F output control, 31 routines, 34-37 download/upload, 85 DTC setting,
 * GM's 3B write, A5 programming mode, AE device control, 28 bus silence... (GM's A9
 * code list and AA live packets are not on the list either: not tested yet) */
static int hexval(int c) { return isdigit(c) ? c - '0' : (toupper(c) >= 'A' && toupper(c) <= 'F') ? toupper(c) - 'A' + 10 : -1; }

static int all_hex(const char *s, int n)
{
    if ((int)strlen(s) != n) return 0;
    for (int i = 0; i < n; i++) if (hexval((unsigned char)s[i]) < 0) return 0;
    return 1;
}

static int at_allowed(const char *a)                  /* after "AT" */
{
    static const char *const exact[] = { "Z", "E0", "L0", "S0", "H0", "H1", "I", "DPN", "RV", "CRA",
                                         "AT0", "AT1", "AT2", "CAF1" };
    for (size_t i = 0; i < sizeof exact / sizeof exact[0]; i++) if (!strcmp(a, exact[i])) return 1;
    if (!strncmp(a, "SP", 2)) return strlen(a) == 3 && (isdigit((unsigned char)a[2]) || (a[2] >= 'A' && a[2] <= 'C'));
    if (!strncmp(a, "SH", 2)) return all_hex(a + 2, 3);   /* which module we talk to (11-bit) */
    if (!strncmp(a, "CRA", 3)) return all_hex(a + 3, 3);  /* which module's answers we listen to */
    if (!strncmp(a, "ST", 2)) return all_hex(a + 2, 2);   /* how long to wait for answers */
    /* flow control (adapter only): the reply to a module's long answer goes to <3 hex>,
     * always "30 00 00" = "send the rest, no pauses"; mode 1 = use these, mode 0 = automatic */
    if (!strncmp(a, "FCSH", 4)) return all_hex(a + 4, 3);
    return !strcmp(a, "FCSD300000") || !strcmp(a, "FCSM0") || !strcmp(a, "FCSM1");
}

static int st_allowed(const char *a)                  /* after "ST" (OBDLink's own commands) */
{
    return !strcmp(a, "I") || !strcmp(a, "DI") || !strcmp(a, "P33") || !strcmp(a, "P53");
}

int obd_allowed(const char *cmd, int clear_ok)
{
    char c[32];
    int n = 0;
    for (const char *p = cmd; *p; p++) {              /* spaces don't matter; anything odd = refused */
        if (*p == ' ') continue;
        if (!isalnum((unsigned char)*p) || n >= 24) return 0;
        c[n++] = (char)toupper((unsigned char)*p);
    }
    c[n] = 0;
    if (n == 0) return 0;
    if (!strncmp(c, "AT", 2)) return at_allowed(c + 2);
    if (!strncmp(c, "ST", 2)) return st_allowed(c + 2);
    if (n % 2 || !all_hex(c, n)) return 0;            /* a message to the car: whole hex bytes only */
    int len = n / 2, b0 = hexval(c[0]) * 16 + hexval(c[1]), b1 = len > 1 ? hexval(c[2]) * 16 + hexval(c[3]) : -1;
    switch (b0) {
    case 0x01: case 0x02: case 0x09: return len >= 2 && len <= 7;   /* read live data / freeze frame / vehicle info */
    case 0x03: case 0x07: case 0x0A: return len == 1;               /* read fault codes */
    case 0x04: return len == 1 && clear_ok;                         /* clear codes: only when armed */
    case 0x22: return len >= 3 && len % 2 == 1;                     /* UDS ReadDataByIdentifier */
    case 0x19: return len == 3 && (b1 == 0x01 || b1 == 0x02);      /* UDS ReadDTCInformation by status mask */
    case 0x3E: return len == 2 && (b1 == 0x00 || b1 == 0x80);      /* UDS TesterPresent */
    case 0x1A: return len == 2;                                     /* GM ReadDataByIdentifier (one-byte DID) */
    default:   return 0;                                            /* anything else: never */
    }
}

/* Every message to the adapter goes through here: refused = never written. */
static int send(const char *cmd, int kind, int arg, uint32_t timeout_ms)
{
    int clear_ok = O.clear_armed && !O.hdr_dirty && !O.bus_dirty && !O.fc_dirty;   /* clear only goes to the whole car, on HS-CAN */
    if (!obd_allowed(cmd, clear_ok)) { O.refused++; return -1; }
    /* talking to one module, headers on, a shorter timeout (or the MS-CAN bus)
     * now: all put back after the scan */
    if ((!strncmp(cmd, "ATSH", 4) && strcmp(cmd + 4, "7DF")) || (!strncmp(cmd, "ATCRA", 5) && cmd[5]) ||
        !strcmp(cmd, "ATH1") || (!strncmp(cmd, "ATST", 4) && strcmp(cmd + 4, "32"))) { O.hdr_dirty = 1; O.restore_step = 0; }
    if (!strcmp(cmd, "STP53")) { O.bus_dirty = 1; O.restore_step = 0; }
    if (!strcmp(cmd, "ATFCSM1")) { O.fc_dirty = 1; O.restore_step = 0; }   /* (cleared again if the adapter says "?") */
    snprintf(O.cmd, sizeof O.cmd, "%s", cmd);
    O.kind = kind;
    O.arg = arg;
    O.rxlen = 0;
    O.sent_at = nzeus_tick_ms();
    O.timeout = timeout_ms;
    serial_write(O.fd, cmd, (int)strlen(cmd));
    serial_write(O.fd, "\r", 1);
    return 0;
}

static void try_baud(int i)
{
    serial_close(O.fd);
    O.baud_i = i;
    O.fd = serial_open(O.port, bauds[i]);
    if (O.fd < 0) { O.state = OBD_OFF; snprintf(O.describe, sizeof O.describe, "Could not open %s", O.port); return; }
    O.state = OBD_SEARCHING;
    send("ATZ", K_RESET, 0, 2500);                         /* reset: answers "ELM327 v1.x" */
}

void obd_start(const char *port)
{
    obd_stop();
    snprintf(O.port, sizeof O.port, "%s", port);
    try_baud(0);
}

void obd_stop(void)
{
    serial_close(O.fd);
    memset(&O, 0, sizeof O);
    O.fd = -1;
    O.focus = -1;
    O.info_step = -1;
}

/* ---------- reading the answers ---------- */
/* One answer line -> bytes. "41 0C 1A F8" / "410C1AF8" / "0: 43 02 ..." (CAN frame
 * numbers) all work. Lines that aren't data ("SEARCHING...", "00A") give 0. */
static int line_bytes(const char *line, unsigned char *out, int max)
{
    const char *colon = strchr(line, ':');
    if (colon && colon - line <= 2) line = colon + 1;     /* "0:" "1:" multi-frame numbers */
    int digits = 0, n = 0, hi = -1;
    for (const char *p = line; *p; p++) {
        if (*p == ' ') continue;
        int v = hexval(*p);
        if (v < 0) return 0;                              /* words: not data */
        digits++;
        if (hi < 0) hi = v; else { if (n < max) out[n++] = (unsigned char)(hi * 16 + v); hi = -1; }
    }
    return digits <= 3 ? 0 : n;                           /* "00A": the length of what follows */
}

/* Walk the answer line by line. */
static int next_line(const char **p, char *line, int n)
{
    while (**p == '\r' || **p == '\n') (*p)++;
    if (!**p) return 0;
    int k = 0;
    while (**p && **p != '\r' && **p != '\n') { if (k < n - 1) line[k++] = **p; (*p)++; }
    line[k] = 0;
    return 1;
}

/* Every whole message in an answer. A CAN answer longer than 7 bytes comes as
 * "014" (its length in hex) and numbered pieces "0:", "1:", ... "F:", "0:"...;
 * they are joined and the padding after the length is cut off. */
typedef void (*msg_fn)(const unsigned char *m, int len, void *ctx);
static void each_message(const char *answer, msg_fn fn, void *ctx)
{
    const char *p = answer;
    char line[160];
    unsigned char b[96], msg[600];
    int mlen = 0, want = -1;
    while (next_line(&p, line, sizeof line)) {
        int digits = 0, other = 0;
        for (const char *q = line; *q; q++) { if (hexval((unsigned char)*q) >= 0) digits++; else if (*q != ' ') other++; }
        if (digits == 3 && !other) {                      /* a length line: a long message starts */
            if (mlen && (want <= 0 || mlen >= want)) fn(msg, want > 0 ? want : mlen, ctx);
            mlen = 0;
            want = (int)strtol(line, NULL, 16);
            continue;
        }
        int k = line_bytes(line, b, sizeof b);
        if (!k) continue;
        const char *colon = strchr(line, ':');
        int numbered = colon && colon - line <= 2;
        if (!numbered) {                                  /* a short message: one line */
            if (mlen && (want <= 0 || mlen >= want)) fn(msg, want > 0 ? want : mlen, ctx);
            mlen = 0; want = -1;
            fn(b, k, ctx);
            continue;
        }
        /* length unknown: piece "0:" starts a new message. Length known: the pieces
         * count 0..F and then 0 again, so only the length says when it ends. */
        if (want <= 0 && colon - line == 1 && line[0] == '0' && mlen) { fn(msg, mlen, ctx); mlen = 0; }
        for (int i = 0; i < k && mlen < (int)sizeof msg; i++) msg[mlen++] = b[i];
        if (want > 0 && mlen >= want) { fn(msg, want, ctx); mlen = 0; want = -1; }
    }
    /* A partial long response is not a complete diagnostic result. */
    if (mlen && (want <= 0 || mlen >= want)) fn(msg, want > 0 ? want : mlen, ctx);
}

typedef struct { const unsigned char *start; int start_n; unsigned char *out; int max, len; } FindCtx;
static void find_cb(const unsigned char *m, int len, void *ctx)
{
    FindCtx *f = ctx;
    if (f->len || len < f->start_n || memcmp(m, f->start, (size_t)f->start_n)) return;
    f->len = len < f->max ? len : f->max;
    memcpy(f->out, m, (size_t)f->len);
}

int obd_find_message(const char *answer, const unsigned char *start, int start_n, unsigned char *out, int max)
{
    FindCtx f = { start, start_n, out, max, 0 };
    each_message(answer, find_cb, &f);
    return f.len;
}

/* the J1979 formulas: A, B = the data bytes */
static void formula(int idx, double A, double B, double *out)
{
    switch (idx) {
    case PID_RPM:      *out = (A * 256 + B) / 4; break;
    case PID_SPEED:    *out = A; break;           /* km/h */
    case PID_COOLANT: case PID_IAT: case PID_AMBIENT: case PID_OILTEMP:
                       *out = A - 40; break;      /* degrees C */
    case PID_MAF:      *out = (A * 256 + B) / 100; break;               /* grams per second */
    case PID_STFT1: case PID_LTFT1: case PID_STFT2: case PID_LTFT2:
                       *out = (A - 128) * 100 / 128; break;             /* percent, + = adding fuel */
    case PID_TIMING:   *out = A / 2 - 64; break;  /* degrees before top dead centre */
    case PID_RUNTIME:  *out = A * 256 + B; break; /* seconds */
    case PID_FUELRATE: *out = (A * 256 + B) / 20; break;                /* litres per hour */
    default:           *out = A * 100 / 255; break;   /* load, throttle, fuel: percent */
    }
}

/* a value from mode 01 (resp 0x41) or a freeze frame (0x42: one more byte, the frame number) */
static int parse_value(const char *answer, int resp, int idx, double *out)
{
    const char *p = answer;
    char line[128];
    unsigned char b[64];
    int skip = resp == 0x42 ? 3 : 2;
    while (next_line(&p, line, sizeof line)) {
        int n = line_bytes(line, b, sizeof b);
        for (int i = 0; i + skip + pids[idx].bytes <= n; i++) {     /* "41", the PID, then A (and B) */
            if (b[i] != resp || b[i + 1] != pids[idx].pid) continue;
            formula(idx, b[i + skip], pids[idx].bytes > 1 ? b[i + skip + 1] : 0, out);
            return 1;
        }
    }
    return 0;
}

int obd_parse_pid(const char *answer, int idx, double *out)
{
    if (idx == PID_VOLTS) {                               /* ATRV: "12.6V" */
        const char *v = strchr(answer, 'V');
        if (!v || v == answer) return 0;
        const char *s = v;
        while (s > answer && (isdigit((unsigned char)s[-1]) || s[-1] == '.')) s--;
        if (s == v) return 0;
        *out = atof(s);
        return 1;
    }
    if (idx < 0 || idx >= PID_COUNT) return 0;
    return parse_value(answer, 0x41, idx, out);
}

static void dtc_text(unsigned a, unsigned b, char *out)
{
    static const char letter[] = "PCBU";                  /* powertrain, chassis, body, network */
    snprintf(out, 6, "%c%u%X%X%X", letter[a >> 6], (a >> 4) & 3, a & 15, b >> 4, b & 15);
}

/* One CAN answer: 43 (or 47, 4A), how many codes, then the codes (anything
 * after them is padding and is ignored). */
static int can_message(const unsigned char *m, int len, int resp, char codes[][6], int n, int max)
{
    if (len < 2 || m[0] != resp) return n;
    int count = m[1], found = 0;
    for (int i = 2; i + 1 < len && n < max && found < count; i += 2, found++)
        if (m[i] || m[i + 1]) dtc_text(m[i], m[i + 1], codes[n++]);
    return n;
}

int obd_parse_codes(const char *answer, int can, int resp, char codes[][6], int max)
{
    const char *p = answer;
    char line[128];
    unsigned char msg[256], b[64];
    int mlen = 0, n = 0;
    while (next_line(&p, line, sizeof line)) {
        int k = line_bytes(line, b, sizeof b);
        if (!k) continue;
        if (!can) {                                       /* older protocols: "43" + 3 codes per line */
            if (b[0] != resp) continue;
            for (int i = 1; i + 1 < k && n < max; i += 2)
                if (b[i] || b[i + 1]) dtc_text(b[i], b[i + 1], codes[n++]);
            continue;
        }
        /* CAN: a long answer comes in numbered pieces "0:", "1:", ...; a short one
         * is one line. Every control unit (engine, gearbox...) answers separately. */
        const char *colon = strchr(line, ':');
        int numbered = colon && colon - line <= 2;
        int first_piece = numbered && hexval(line[0]) == 0;
        if (!numbered || first_piece) {                   /* a new answer starts: finish the last one */
            n = can_message(msg, mlen, resp, codes, n, max);
            mlen = 0;
        }
        for (int i = 0; i < k && mlen < (int)sizeof msg; i++) msg[mlen++] = b[i];
        if (!numbered) { n = can_message(msg, mlen, resp, codes, n, max); mlen = 0; }
    }
    return can_message(msg, mlen, resp, codes, n, max);
}

int obd_parse_dtcs(const char *answer, int can, char codes[][6], int max) { return obd_parse_codes(answer, can, 0x43, codes, max); }

/* printable characters of a mode 09 answer (VIN, calibration ID, ECU name) */
static void text_of(const unsigned char *m, int len, char *out, int n)
{
    int k = 0;
    for (int i = 0; i < len && k < n - 1; i++) if (m[i] > 0x20 && m[i] < 0x7F) out[k++] = (char)m[i];
    out[k] = 0;
}

typedef struct { unsigned char data[64]; int n, can; } VinCtx;
static void vin_cb(const unsigned char *m, int len, void *ctx)
{
    VinCtx *v = ctx;
    if (len < 4 || m[0] != 0x49 || m[1] != 0x02) return;
    if (len > 7) {                                        /* CAN: one long message, 49 02 01 + 17 letters */
        v->n = 0;
        for (int i = 3; i < len && v->n < 64; i++) v->data[v->n++] = m[i];
        v->can = 1;
    } else if (!v->can) {                                 /* older protocols: 49 02 <n> + 4 letters, 5 lines */
        for (int i = 3; i < len && v->n < 64; i++) v->data[v->n++] = m[i];
    }
}

int obd_parse_vin(const char *answer, char vin[18])
{
    VinCtx v = { .n = 0 };
    each_message(answer, vin_cb, &v);
    char t[65];
    text_of(v.data, v.n, t, sizeof t);
    int len = (int)strlen(t);
    if (len < 17) { vin[0] = 0; return 0; }
    memcpy(vin, t + len - 17, 17);                        /* (older answers start with zeros) */
    vin[17] = 0;
    return 1;
}

/* mode 01 PID 01: check-engine light, number of codes, and the self-tests
 * ("monitors"): which ones the car has and which have finished since the
 * codes were last cleared (an inspection wants them finished). */
static const char *const cont_mons[3] = { "Misfire", "Fuel system", "Components" };
static const char *const spark_mons[8] = { "Catalyst", "Heated catalyst", "Fuel vapour (EVAP)", "Secondary air",
                                           "A/C refrigerant", "Oxygen sensors", "Oxygen sensor heaters", "EGR / VVT" };
static const char *const diesel_mons[8] = { "NMHC catalyst", "NOx / SCR", NULL, "Boost pressure",
                                            NULL, "Exhaust gas sensor", "Particulate filter", "EGR / VVT" };

static void ready_cb(const unsigned char *m, int len, void *ctx)
{
    ObdInfo *I = ctx;
    if (len < 6 || m[0] != 0x41 || m[1] != 0x01) return;
    unsigned A = m[2], B = m[3], C = m[4], D = m[5];
    I->mil_on |= (A >> 7) & 1;                            /* any unit that wants the light on */
    I->dtc_count += A & 0x7F;
    if (I->have_status) return;                           /* the monitors: from the first (engine) unit */
    I->have_status = 1;
    I->compression = (B >> 3) & 1;
    I->mon_count = 0;
    for (int i = 0; i < 3; i++)
        if ((B >> i) & 1) { I->mon[I->mon_count].name = cont_mons[i]; I->mon[I->mon_count++].complete = !((B >> (i + 4)) & 1); }
    const char *const *names = I->compression ? diesel_mons : spark_mons;
    for (int i = 0; i < 8 && I->mon_count < OBD_MAX_MONITORS; i++)
        if (((C >> i) & 1) && names[i]) { I->mon[I->mon_count].name = names[i]; I->mon[I->mon_count++].complete = !((D >> i) & 1); }
}

int obd_parse_readiness(const char *answer, ObdInfo *info)
{
    info->have_status = info->mil_on = info->dtc_count = 0;
    each_message(answer, ready_cb, info);
    return info->have_status;
}

/* which PIDs the car has: 41 <base> + 4 bytes = 32 yes/no bits (all units together) */
typedef struct { int base; unsigned bits; int found; } SupCtx;
static void sup_cb(const unsigned char *m, int len, void *ctx)
{
    SupCtx *s = ctx;
    if (len < 6 || m[0] != 0x41 || m[1] != s->base) return;
    s->bits |= (unsigned)m[2] << 24 | (unsigned)m[3] << 16 | (unsigned)m[4] << 8 | m[5];
    s->found = 1;
}

static int parse_sup(const char *answer, int base, unsigned *bits)
{
    SupCtx s = { base, 0, 0 };
    each_message(answer, sup_cb, &s);
    *bits = s.bits;
    return s.found;
}

static int has(const char *s, const char *word) { return strstr(s, word) != NULL; }

static int answer_ok(const char *answer)
{
    char line[64];
    while (next_line(&answer, line, sizeof line)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        size_t n = strlen(p);
        while (n && isspace((unsigned char)p[n - 1])) n--;
        if (n == 2 && !strncmp(p, "OK", 2)) return 1;
    }
    return 0;
}

/* ATZ reinitializes the adapter, not the car. Keep the completed scan in the UI. */
static void recover_adapter(const char *why)
{
    char port[sizeof O.port];
    snprintf(port, sizeof port, "%s", O.port);             /* obd_start clears O first */
    fprintf(stderr, "OBD_RECONNECT reason=%s restore-step=%d\n", why, O.restore_step);
    obd_start(port);
}

/* mode 09 text answers: 49 <pid> <count> + text */
static void info_text(const char *a, int pid, char *out, int n)
{
    unsigned char start[2] = { 0x49, (unsigned char)pid }, m[128];
    int len = obd_find_message(a, start, 2, m, sizeof m);
    out[0] = 0;
    if (len > 3) text_of(m + 3, len - 3, out, n);
}

static void info_next(int from)
{
    O.info_step = from;
    if (O.info_step == I_FF0 && !O.info.have_ff) O.info_step = I_VIN;    /* no freeze frame: skip its values */
    if (O.info_step >= I_COUNT) { O.info_step = -1; O.info_ready = 1; }
}

static void handle_info(const char *a, int step)
{
    ObdInfo *I = &O.info;
    if (step == I_STATUS) obd_parse_readiness(a, I);
    else if (step == I_FFDTC) {                           /* 42 02 00 <code>: the code that froze the frame */
        unsigned char start[2] = { 0x42, 0x02 }, m[16];
        int len = obd_find_message(a, start, 2, m, sizeof m);
        I->have_ff = len >= 5 && (m[3] || m[4]);
        if (I->have_ff) dtc_text(m[3], m[4], I->ff_dtc);
    } else if (step >= I_FF0 && step < I_FF0 + FF_COUNT) {
        int k = step - I_FF0;
        I->ff_have[k] = parse_value(a, 0x42, ff_pid[k], &I->ff_value[k]);
    } else if (step == I_VIN) obd_parse_vin(a, I->vin);
    else if (step == I_CALID) info_text(a, 0x04, I->calid, sizeof I->calid);
    else if (step == I_ECU) info_text(a, 0x0A, I->ecu_name, sizeof I->ecu_name);
    info_next(step + 1);
}

static void handle_answer(const char *a)
{
    int kind = O.kind, arg = O.arg;
    O.kind = K_NONE;
    if (kind == K_RESET) {                                /* the adapter is alive */
        const char *e = strstr(a, "ELM327");
        if (!e) e = strstr(a, "STN");
        snprintf(O.elm, sizeof O.elm, "%.*s", e ? (int)strcspn(e, "\r\n") : 7, e ? e : "Adapter");
        O.state = OBD_INIT;
        O.id_step = 1;                                    /* identify even if no car answers */
        O.step = 0;
        send(init_cmds[0], K_INIT, 0, 1500);
        return;
    }
    if (kind == K_INIT) {
        const char *c = init_cmds[O.step];
        if (!strcmp(c, "0100")) {                         /* the first question to the car itself */
            if (has(a, "UNABLE") || has(a, "NO DATA") || has(a, "ERROR") || has(a, "STOPPED")) {
                O.state = OBD_NO_CAR;
                O.retry_at = nzeus_tick_ms() + 5000;      /* ask again in 5 s */
                return;
            }
            O.state = OBD_INIT;
            O.sup_known = parse_sup(a, 0x00, &O.sup[0]);  /* PIDs 01-20 the car has */
        }
        if (!strcmp(c, "ATDPN")) {                        /* protocol number: 6-9 (and A-C) are CAN */
            const char *q = a;
            while (*q && !isalnum((unsigned char)*q)) q++;
            if (*q == 'A') q++;                           /* "A6" = automatic, found 6 */
            O.proto = *q;
            O.can = (*q >= '6' && *q <= '9') || (*q >= 'A' && *q <= 'C');
        }
        if (++O.step < INIT_STEPS) { send(init_cmds[O.step], K_INIT, 0, !strcmp(init_cmds[O.step], "0100") ? 9000 : 1500); return; }
        O.state = OBD_READY;
        O.sup_next = O.sup_known && (O.sup[0] & 1) ? 1 : 0;
        return;
    }
    switch (kind) {
    case K_ID:
        if (arg == 1) {                                   /* STI: "STN2232 v5.10.3" = an OBDLink */
            O.stn = has(a, "STN");
            O.id_step = O.stn ? 2 : 0;
        } else {                                          /* STDI: "OBDLink EX r2.7.1" */
            const char *p = a;
            char line[64];
            while (next_line(&p, line, sizeof line)) if (isalpha((unsigned char)line[0])) { snprintf(O.adapter, sizeof O.adapter, "%.47s", line); break; }
            O.id_step = 0;
        }
        break;
    case K_SUP: {
        unsigned bits;
        int found = parse_sup(a, arg * 0x20, &bits);
        if (found) O.sup[arg] = bits;
        O.sup_next = found && (bits & 1) && arg < 3 ? arg + 1 : 0;   /* last bit: "the next list exists" */
        break;
    }
    case K_LIVE:  O.live.have[arg] = obd_parse_pid(a, arg, &O.live.value[arg]); break;
    case K_CODES:
        O.codes_n[arg] = obd_parse_codes(a, O.can, code_resp[arg], O.codes[arg], OBD_MAX_DTCS);
        O.codes_ready[arg] = 1;
        break;
    case K_CLEAR: O.clear_result = has(a, "44") ? 1 : -1; break;     /* 44 = done */
    case K_INFO:  handle_info(a, arg); break;
    case K_ASK:
        snprintf(O.answer, sizeof O.answer, "%s", a);
        O.ask_done = 1;
        if (!strcmp(O.cmd, "ATFCSM1") && !answer_ok(a)) O.fc_dirty = 0;   /* refused: flow control is still automatic */
        break;
    case K_RESTORE:
        if (!answer_ok(a)) { recover_adapter("restore-rejected"); break; }
        if (++O.restore_step >= RESTORE_STEPS || (O.restore_step == RESTORE_STEPS - 1 && !O.fc_dirty)) {
            O.restore_step = 0;
            O.hdr_dirty = O.bus_dirty = O.fc_dirty = 0;
        }
        break;
    default: break;
    }
}

/* no answer in time */
static void handle_timeout(void)
{
    int kind = O.kind, arg = O.arg;
    O.kind = K_NONE;
    switch (kind) {
    case K_RESET:
        if (O.baud_i + 1 < (int)(sizeof bauds / sizeof bauds[0])) try_baud(O.baud_i + 1);
        else { obd_stop(); snprintf(O.describe, sizeof O.describe, "No answer from an OBD adapter"); }
        break;
    case K_INIT:
        if (O.state == OBD_NO_CAR) { O.retry_at = nzeus_tick_ms() + 5000; break; }
        if (++O.step < INIT_STEPS) send(init_cmds[O.step], K_INIT, 0, 1500);
        else O.state = OBD_READY;
        break;
    case K_ID:      O.id_step = 0; break;
    case K_SUP:     O.sup_next = 0; break;
    case K_LIVE:    O.live.have[arg] = 0; break;
    case K_CODES:   O.codes_n[arg] = 0; O.codes_ready[arg] = 1; break;
    case K_CLEAR:   O.clear_result = -1; break;
    case K_INFO:    info_next(arg + 1); break;
    case K_ASK:     O.answer[0] = 0; O.ask_done = 1; break;
    case K_RESTORE: recover_adapter("restore-timeout"); break;
    default: break;
    }
}

/* ---------- every frame ---------- */
/* the next live value to ask (only ones the car has); the graph's value every other time */
static int next_live(void)
{
    if (O.focus >= 0 && O.focus < PID_COUNT && obd_pid_supported(O.focus) && (O.focus_turn ^= 1)) return O.focus;
    for (int k = 0; k < PID_COUNT; k++) {
        int i = (O.poll_i + k) % PID_COUNT;
        if (obd_pid_supported(i)) { O.poll_i = (i + 1) % PID_COUNT; return i; }
    }
    return PID_VOLTS;
}

void obd_poll(void)
{
    if (O.state == OBD_OFF || O.fd < 0) return;
    char buf[512];
    int n = serial_read(O.fd, buf, sizeof buf);
    if (n < 0) {
        fprintf(stderr, "OBD_READ_FAILED errno=%d\n", errno);
        obd_stop(); snprintf(O.describe, sizeof O.describe, "Adapter connection lost - checking the USB port"); return;
    }
    if (n > 0 && O.rxlen + n < (int)sizeof O.rx - 1) { memcpy(O.rx + O.rxlen, buf, (size_t)n); O.rxlen += n; }
    O.rx[O.rxlen] = 0;
    uint32_t now = nzeus_tick_ms();
    if (O.kind != K_NONE) {
        char *prompt = strchr(O.rx, '>');                 /* the adapter is ready again */
        if (prompt) { *prompt = 0; handle_answer(O.rx); O.rxlen = 0; return; }
        if (now - O.sent_at > O.timeout) handle_timeout();
        return;
    }
    if (O.state == OBD_NO_CAR) {                          /* ask the car again from time to time */
        /* STI/STDI query the adapter itself, so they do not need a vehicle. */
        if (O.id_step) { send(O.id_step == 1 ? "STI" : "STDI", K_ID, O.id_step, 1500); return; }
        if ((int32_t)(now - O.retry_at) >= 0) { O.step = 5; send("0100", K_INIT, 0, 9000); }
        return;
    }
    if (O.state != OBD_READY) return;
    char cmd[16];
    /* 1. after a module scan: back to talking to the whole car on HS-CAN:
     *    the bus (only if it was changed), header 7DF, no filter, headers off,
     *    the normal answer timeout (32 hex x 4 ms = 200 ms, the ELM327's own default) */
    if (!O.session && (O.hdr_dirty || O.bus_dirty || O.fc_dirty)) {
        static const char *const restore[RESTORE_STEPS] = { NULL, "ATSH7DF", "ATCRA", "ATH0", "ATST32", "ATFCSM0" };
        if (O.restore_step == 0 && !O.bus_dirty) O.restore_step = 1;
        if (O.restore_step == 0) snprintf(cmd, sizeof cmd, "ATSP%c", O.proto ? O.proto : '6');
        else snprintf(cmd, sizeof cmd, "%s", restore[O.restore_step]);
        send(cmd, K_RESTORE, 0, 1500);
        return;
    }
    /* 2. a question from the module scan */
    if (O.ask_pending) {
        O.ask_pending = 0;
        if (send(O.ask_cmd, K_ASK, 0, (uint32_t)O.ask_timeout) < 0) { O.answer[0] = 0; O.ask_done = 1; }
        return;
    }
    if (O.session) return;                                /* the scan owns the adapter */
    /* 3. which adapter is it, which values does the car have */
    if (O.id_step) { send(O.id_step == 1 ? "STI" : "STDI", K_ID, O.id_step, 1500); return; }
    if (O.sup_next) { snprintf(cmd, sizeof cmd, "01%02X", O.sup_next * 0x20); send(cmd, K_SUP, O.sup_next, 1500); return; }
    /* 4. what the driver asked for */
    if (O.clear_wanted) {
        O.clear_wanted = 0;
        O.clear_armed = 1;                                /* armed for exactly this one message */
        if (send("04", K_CLEAR, 0, 5000) < 0) O.clear_result = -1;
        O.clear_armed = 0;
        return;
    }
    for (int k = 0; k < CODES_KINDS; k++)
        if (O.codes_wanted[k]) { O.codes_wanted[k] = 0; send(code_cmds[k], K_CODES, k, 5000); return; }
    if (O.info_step >= 0) { send(info_cmds[O.info_step], K_INFO, O.info_step, O.info_step >= I_VIN ? 3000 : 1500); return; }
    /* 5. live values, in turn */
    int i = next_live();
    send(pids[i].cmd, K_LIVE, i, 1500);
}

void obd_read_codes(int k)
{
    if (k < 0 || k >= CODES_KINDS) return;
    O.codes_ready[k] = 0;
    O.codes_n[k] = 0;
    O.codes_wanted[k] = 1;
}
void obd_read_dtcs(void) { obd_read_codes(CODES_STORED); }
void obd_clear_dtcs(void) { O.clear_result = 0; O.clear_wanted = 1; }

void obd_read_info(void)
{
    memset(&O.info, 0, sizeof O.info);
    O.info_ready = 0;
    O.info_step = I_STATUS;
}

int obd_ask(const char *cmd, int timeout_ms)
{
    if (!obd_allowed(cmd, 0)) { O.refused++; return -1; }   /* checked here AND in send() */
    if (O.state != OBD_READY || O.ask_pending || O.kind == K_ASK) return -2;
    snprintf(O.ask_cmd, sizeof O.ask_cmd, "%s", cmd);
    O.ask_timeout = timeout_ms;
    O.ask_pending = 1;
    O.ask_done = 0;
    O.answer[0] = 0;
    return 0;
}

/* ---------- plain words for common codes (dtc_db.c has them, in English and Arabic) ----------
 * Only codes that mean the same on every car: the list page does not know
 * the car's make, and a maker's own codes (P1..., B1...) mean different
 * things on different makes. */
const char *obd_dtc_meaning(const char *code)
{
    const DtcInfo *d = dtc_is_generic(code) ? dtc_lookup(NULL, code) : NULL;
    return d ? d->en : NULL;
}

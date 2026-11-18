#ifndef NZEUS_OBD_H
#define NZEUS_OBD_H

/* OBD-II (the car's diagnostic port) through an ELM327-type USB adapter, such
 * as the OBDLink EX. It reads: live data, fault codes, self-test results,
 * freeze frames, the VIN - and (carscan.c) the fault codes of every module
 * that answers, on any car. It never changes the car's programming.
 *
 * SAFETY: every message to the adapter goes through ONE function that checks
 * it against a whitelist (obd_allowed). Anything not on the list is refused
 * and never reaches the serial port. The only thing on the list that changes
 * anything in the car is "clear fault codes" (OBD mode 04), and only while
 * the Car app's clear flow (two questions + a saved copy) has armed it.
 *
 * Everything is non-blocking: obd_poll() is called every frame, sends the next
 * question when the adapter is ready (its ">" prompt) and reads answers. */

enum { OBD_OFF, OBD_SEARCHING, OBD_INIT, OBD_NO_CAR, OBD_READY };

/* what the Car app says when no adapter is plugged in (there is no demo:
 * without an adapter nothing is shown as if it came from a car) */
#define OBD_NO_ADAPTER "No OBD adapter - plug the ELM327 into the USB port (Settings > USB > Devices)"

/* live values (the first 7 are the original gauges; the rest only if the car has them) */
enum { PID_RPM, PID_SPEED, PID_COOLANT, PID_LOAD, PID_THROTTLE, PID_FUEL, PID_VOLTS,
       PID_IAT, PID_MAF, PID_STFT1, PID_LTFT1, PID_STFT2, PID_LTFT2, PID_TIMING,
       PID_AMBIENT, PID_OILTEMP, PID_RUNTIME, PID_FUELRATE, PID_COUNT };

typedef struct {
    double value[PID_COUNT];
    int have[PID_COUNT];          /* 1 = the car answered this one */
} ObdLive;

#define OBD_MAX_DTCS 32

void obd_start(const char *port);     /* connect (tries 38400, 115200, 9600 baud) */
void obd_stop(void);
void obd_poll(void);
int  obd_state(void);
const char *obd_describe(void);       /* one line for the screen: adapter, protocol... */
const ObdLive *obd_live(void);

/* about each live value */
const char *obd_pid_name(int i);      /* "Engine speed" */
const char *obd_pid_name_ar(int i);   /* the same in Arabic (for the report) */
const char *obd_pid_cmd(int i);       /* the question for it: "010C" (the battery: "ATRV") */
const char *obd_pid_short(int i);     /* "RPM" (log file column) */
const char *obd_pid_unit(int i);      /* "rpm" */
const char *obd_pid_format(int i);    /* "%.0f" */
void obd_pid_range(int i, double *lo, double *hi);   /* the usual range (for graphs) */
int  obd_pid_supported(int i);        /* 1 = the car has it (before the car told us: the basic ones) */
void obd_focus_pid(int i);            /* ask this one every other time (graphs); -1 = none */

/* fault codes: stored (mode 03), pending (07: seen once, not confirmed yet),
 * permanent (0A: stay until the car's own self-test passes) */
enum { CODES_STORED, CODES_PENDING, CODES_PERMANENT, CODES_KINDS };
void obd_read_codes(int kind);
int  obd_codes_ready(int kind);
int  obd_codes_count(int kind);
const char *obd_code(int kind, int i);

void obd_read_dtcs(void);             /* = obd_read_codes(CODES_STORED) */
int  obd_dtcs_ready(void);
int  obd_dtc_count(void);
const char *obd_dtc(int i);           /* "P0133" */
void obd_clear_dtcs(void);            /* mode 04 - only after the user confirmed */
int  obd_clear_result(void);          /* 0 waiting, 1 cleared, -1 refused / failed */

const char *obd_dtc_meaning(const char *code);   /* plain words for common codes, or NULL
                                                    (only codes that mean the same on every car) */

/* self-tests (readiness), freeze frame, vehicle info - read together */
#define OBD_MAX_MONITORS 12
enum { FF_RPM, FF_SPEED, FF_COOLANT, FF_LOAD, FF_COUNT };
typedef struct {
    int have_status;                  /* mode 01 PID 01 answered */
    int mil_on, dtc_count, compression;   /* check-engine light, codes stored, diesel */
    struct { const char *name; int complete; } mon[OBD_MAX_MONITORS];
    int mon_count;                    /* the monitors this car supports */
    int have_ff;                      /* a freeze frame is stored */
    char ff_dtc[6];                   /* the code that froze it */
    double ff_value[FF_COUNT];
    int ff_have[FF_COUNT];
    char vin[18], calid[20], ecu_name[24];
} ObdInfo;
void obd_read_info(void);
int  obd_info_ready(void);
const ObdInfo *obd_info(void);

/* the adapter */
const char *obd_adapter(void);        /* "OBDLink EX r2.7.1" or "ELM327 v1.5" */
int  obd_is_stn(void);                /* an OBDLink (STN chip): it can also reach Ford's MS-CAN */
const char *obd_protocol_name(void);  /* "ISO 15765-4 CAN 11/500" */
int  obd_is_can(void);                /* 1 = the car talks CAN (any kind) */
int  obd_can11(void);                 /* 1 = the car talks CAN with 11-bit addresses (7E0, 760...):
                                         the only kind where single modules can be asked */

/* questions from the module scan (carscan.c). Each one goes through the
 * whitelist like everything else. */
int  obd_ask(const char *cmd, int timeout_ms);   /* 0 = will be sent, -1 = refused (not allowed), -2 = not connected / busy */
int  obd_ask_done(void);              /* 1 = the answer (or a timeout) is in */
const char *obd_ask_answer(void);     /* what the adapter said ("" after a timeout) */
void obd_session(int on);             /* 1: a module scan uses the adapter (no live data meanwhile);
                                         0: done - the adapter is put back (header 7DF, filter off,
                                         headers off, normal timeout, HS-CAN) */
int  obd_session_clean(void);         /* 1 = no module header / headers / timeout / bus change left over */

/* the whitelist (see obd.c). clear_ok = the clear flow armed mode 04 */
int  obd_allowed(const char *cmd, int clear_ok);
int  obd_refused_count(void);         /* how many requests were refused so far */

/* for tests: parsing the adapter's answers */
int  obd_parse_pid(const char *answer, int pid_index, double *out);
int  obd_parse_dtcs(const char *answer, int can, char codes[][6], int max);
int  obd_parse_codes(const char *answer, int can, int response_byte, char codes[][6], int max);
int  obd_parse_vin(const char *answer, char vin[18]);
int  obd_parse_readiness(const char *answer, ObdInfo *info);
/* the first message in an answer that starts with these bytes (CAN pieces joined,
 * padding cut off): its length, or 0 */
int  obd_find_message(const char *answer, const unsigned char *start, int start_n, unsigned char *out, int max);

#endif

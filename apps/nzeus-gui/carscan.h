#ifndef NZEUS_CARSCAN_H
#define NZEUS_CARSCAN_H
#include <time.h>

/* One complete, READ-ONLY health scan of a car - any make - as gathered by
 * the Car app (obd.c / carscan.c) and turned into a report (carreport.c: PDF
 * in English and Arabic).
 *
 * Shared by two parts:
 *   - the scanner FILLS it (carscan.c: standard OBD-II from every engine /
 *     gearbox computer, then every module that answers on the main bus;
 *     on a Ford also Ford's module names, its second bus and odometers)
 *   - the report generator READS it (never talks to the car)
 * Every text that appears in the report exists in both languages (_en, _ar);
 * the scanner fills both (the code meanings table has both). */

#define SCAN_MAX_MODULES 48
#define SCAN_MAX_DTCS    96
#define SCAN_MAX_STEPS   6
#define SCAN_MAX_WISH    16
#define SCAN_MAX_ODO     8
#define SCAN_MAX_LIVE    20
#define SCAN_MAX_NOTES   6

typedef struct {
    char code[8];              /* short name: "ECM", "TCM", "0x760"; on a Ford "BdyCM", "PCM", "ABS"... */
    char name_en[48];          /* "Engine computer", "Module 0x760" / "Body control" */
    char name_ar[96];          /* UTF-8 */
    int  can_id;               /* request address, e.g. 0x760 (0 = generic OBD only) */
    int  answered;             /* 1 = it replied during the scan */
    int  codes_read;           /* 1 = it listed its fault codes (0 = it answered "can't do that") */
    char part_no[32];          /* (not read any more: always "") */
    int bus;                  /* 0 = primary CAN; 1 = Ford MS-CAN */
    char identity[4][129];     /* exact positive bytes, hexadecimal: UDS F187/F188/F189/F191,
                                  or on a GM module (protocol 1) GM's identifiers 90/B0/C1/CB */
    int identity_status[4];    /* 0 not asked; 1 valid; -1 unavailable/invalid (never OFF) */
    int protocol;              /* SCAN_PROTO_UDS (OBD-II / UDS) or SCAN_PROTO_GMLAN (GM's own, GMW3110) */
} ScanModule;

enum { SCAN_PROTO_UDS, SCAN_PROTO_GMLAN };

/* "Now" column. STORED = an engine computer's confirmed code (OBD mode 03):
 * it does not say whether the fault is still happening. */
enum { DTC_HISTORY, DTC_PENDING, DTC_ACTIVE, DTC_PERMANENT, DTC_STORED };

typedef struct {
    char module[8];            /* ScanModule.code it came from ("PCM" for generic OBD) */
    char code[8];              /* "B10F3", "U0122", "P0133" */
    unsigned char failure_type;/* UDS 3rd byte (0 = none) */
    unsigned char status;      /* UDS status byte (bit0 testFailed, bit3 confirmed, bit2 pending...) */
    int  state;                /* DTC_* */
    char group_en[48];         /* e.g. "Exterior lights", "Mirrors", "Communication between modules" */
    char group_ar[96];
    char meaning_en[120];      /* plain words; "" if unknown */
    char meaning_ar[200];
} ScanDtc;

typedef struct {
    char module[8];            /* which module reported it */
    double km;
} ScanOdometer;

/* one live value at the time of the scan: "Engine speed" = "1726 rpm" */
typedef struct {
    char name_en[40], name_ar[80];
    char value[24];            /* number + unit (the same in both languages) */
} ScanLive;

typedef struct {
    char feature_en[64], feature_ar[120];
    char module_en[64],  module_ar[120];
    int  outlook;              /* 0 to verify, 1 commonly possible, 2 possible but a safety feature, 3 unknown */
} ScanWish;

typedef struct {
    /* when, with what */
    time_t when;               /* scan time (console clock; time zone from Settings) */
    char adapter[48];          /* "OBDLink EX r2.7.1" / "ELM327 v1.5" */
    char protocol[32];         /* "ISO 15765-4 CAN 11/500" */
    int  ignition_on, engine_running;   /* from RPM > 0 */
    int  demo;                 /* 1 = made-up data: the report says so in big letters. The Car app
                                  never sets it (there is no demo); only the report tests do */

    /* the car */
    char vin[18];              /* full VIN (the report masks the middle: 6FP·········00001) */
    int  model_year;           /* from VIN position 10, 0 = unknown */
    char make_en[24], make_ar[48];   /* from the VIN's first 3 letters: "Toyota", "Ford"; "" = unknown */
    char make_model_en[64];    /* "Ford Ranger Raptor" if it can be told, else just the make / "" */
    char make_model_ar[96];
    char engine_en[48], engine_ar[96];  /* "3.0L V6 EcoBoost" if known */
    double battery_v;          /* ATRV, 0 = unknown */
    double odometer_km;        /* agreed value, <0 = unknown */
    ScanOdometer odo[SCAN_MAX_ODO];      /* odometer as stored in each module (mileage check) */
    int odo_count;
    int odo_agree;             /* 1 = all the same (within 1 km), 0 = they differ, -1 = not enough data */

    /* engine computer (generic OBD-II) */
    int mil_known;             /* 1 = an engine computer said whether the light is on */
    int mil_on;                /* check-engine light (mode 01 PID 01) */
    int readiness_total, readiness_ready;   /* monitors supported / complete */
    char readiness_en[200], readiness_ar[300];   /* one line: "7 of 8 tests done; EVAP not yet" */

    /* every module */
    ScanModule modules[SCAN_MAX_MODULES];
    int module_count;          /* known modules probed */
    int modules_answered;

    /* fault codes */
    ScanDtc dtcs[SCAN_MAX_DTCS];
    int dtc_count;

    /* live values at the time of the scan (from the engine computer) */
    ScanLive live[SCAN_MAX_LIVE];
    int live_count;

    /* notes at the end of the report: how the scan went, what it could not reach */
    char notes_en[SCAN_MAX_NOTES][200], notes_ar[SCAN_MAX_NOTES][320];
    int note_count;

    /* the summary at the top (filled by the scanner from the data) */
    int healthy;               /* available results complete, no active/pending/stored codes or MIL */
    char headline_en[96], headline_ar[160];     /* "No active faults reported." */
    char summary_en[400], summary_ar[700];
    char cause_en[300], cause_ar[500];          /* "Most likely cause: ..." ("" = no box) */
    char steps_en[SCAN_MAX_STEPS][200], steps_ar[SCAN_MAX_STEPS][320];   /* "What to do" */
    int step_count;
    ScanWish wish[SCAN_MAX_WISH];               /* "Wishlist: first look" (module-based guesses) */
    int wish_count;
} CarScan;

/* Report generator (carreport.c): writes a PDF; lang "en" or "ar".
 * Returns 0 and the file name in out_path (e.g. nzeus/car/Health-Scan-2026-10-04-EN.pdf). */
int carreport_pdf(const CarScan *s, const char *lang, const char *folder, char *out_path, int out_n);

/* a ready-made scan for the report's PC tests only (the 29 Sep 2026 Raptor
 * report's data). The Car app never shows it: no adapter = no report. */
void carscan_sample(CarScan *s);

/* ---- the scanner (carscan.c), READ ONLY ----
 * Every question goes through obd.c's whitelist: OBD-II modes 01 03 07 0A 09,
 * UDS 0x19 02 (read fault codes) / 0x22 (read data) and, on a GM car only,
 * GM's 0x1A (read data). Nothing is written. */
enum { CS_IDLE, CS_RUNNING, CS_DONE, CS_CANCELLED };
void carscan_start(CarScan *s);       /* the adapter must be connected (OBD_READY) */
void carscan_poll(void);              /* every frame */
void carscan_cancel(void);            /* stop; the adapter is put back to normal */
int  carscan_state(void);
/* how far: steps done, steps in all, and one line saying what is asked now */
void carscan_progress(int *done, int *total, char *line, int line_n);
int  carscan_is_ford(const CarScan *s);   /* the VIN says Ford */
int  carscan_is_gm(const CarScan *s);     /* the VIN says Chevrolet, GMC or Cadillac */
/* the name of identifier k (0-3) of this module in the discovery file:
 * "F187".. for UDS, "GM90".. for a GM module */
const char *carscan_identity_name(const ScanModule *m, int k);

/* reading UDS answers (also used by the tests) */
int  uds_dtc_state(unsigned char status);     /* status byte -> DTC_ACTIVE / HISTORY / PENDING */
void uds_dtc_text(const unsigned char *b, char out[8]);   /* first 2 bytes -> "B10F3" */
int  uds_parse_dtcs(const char *answer, CarScan *s, const char *module);   /* adds them; count, or -1 if absent/incomplete/too large */
int  uds_parse_odometer(const char *answer, double *km);  /* Ford DID DD01 */
int  uds_parse_identity(const char *answer, int did, char *hex, int size);
/* GM (GMLAN) ReadDataByIdentifier: "5A <did> <bytes>" -> exact bytes as hex (count, or -1) */
int  gm_parse_identity(const char *answer, int did, char *hex, int size);
/* did a GM module answer "1A <did>" at all? (yes: 5A <did> ..., or a refusal 7F 1A xx,
 * but not 7F 1A 78 = "wait") */
int  gm_answered(const char *answer, int did);
/* the OBD ECUs that answered a functional question with headers on ("7E8...", "7E9..."):
 * their answer addresses, in order; returns how many */
int  obd_parse_responders(const char *answer, int *ids, int max);

#endif

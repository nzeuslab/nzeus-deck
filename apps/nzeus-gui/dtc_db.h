#ifndef NZEUS_DTC_DB_H
#define NZEUS_DTC_DB_H

/* Plain-word meanings of fault codes, in English and Arabic (UTF-8).
 *
 * Some codes mean something more exact when we know which module stored
 * them (U0122 in the headlamp module = "the headlamp module lost contact
 * with the stability-control module"), so a lookup gives the module too:
 * a module-specific entry wins, then the general one.
 *
 * Every code also belongs to a group ("Exterior lights", "Communication
 * between modules"...), the headings of the report's code table. */

typedef struct {
    const char *module;        /* "HCM", or NULL = any module */
    const char *code;          /* "U0122" */
    int group;                 /* DG_* */
    const char *en, *ar;       /* the meaning */
    const char *pair;          /* the same fault on the other side (left <-> right), or NULL */
    const char *check_en, *check_ar;   /* what to look at on the car ("brake lights"), or NULL */
} DtcInfo;

enum { DG_LIGHTS, DG_MIRRORS, DG_NETWORK, DG_ENGINE, DG_GEARBOX, DG_BRAKES, DG_STEERING,
       DG_AIRBAGS, DG_BODY, DG_POWER, DG_AUDIO, DG_CLIMATE, DG_COUNT };

/* the best entry for this code from this module (module may be NULL), or NULL */
const DtcInfo *dtc_lookup(const char *module, const char *code);

/* 1 = a code that means the same on every car (P0..., P2..., B0..., C0..., U0...) */
int dtc_is_generic(const char *code);

/* the same for a car of a known make: ford = 1 is dtc_lookup(); for any other
 * make only the generic meanings (no Ford wording, no Ford-only codes) */
const DtcInfo *dtc_lookup_car(const char *module, const char *code, int ford);

/* the group's name ("Exterior lights · Body Control Module"); lang "en" or "ar" */
const char *dtc_group_name(int group, const char *lang);

/* the group for any code: from the table (dtc_lookup_car), else from its letter
 * (P engine, C brakes, B body, U network) */
int dtc_group_of(const char *module, const char *code, int ford);

/* the 3rd byte of a UDS code ("failure type", e.g. 0x13 = circuit open) in words, or "" */
const char *dtc_failure_text(unsigned char type, const char *lang);

#endif

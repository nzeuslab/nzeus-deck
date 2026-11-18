#ifndef NZEUS_BTINFO_H
#define NZEUS_BTINFO_H
#include <stddef.h>
#include <stdint.h>

/* Bluetooth "ID cards" for the deck: what a nearby device is, worked out
 * from what it says while we scan (`nzeus-bt survey`, apps/bluetooth).
 * Every guess says where it came from and how sure it is. Only codes we are
 * sure of are in the built-in lists; others are shown as numbers. */

typedef struct {
    char addr[18];
    char type[12];            /* "classic", "le-public", "le-random", "dual" */
    int  rssi;
    int  has_tx, tx_power;
    int  has_cod;  unsigned cod;
    int  has_app;  unsigned appearance;
    int  n_uuid;   unsigned uuid[8];
    int  has_mfr;  unsigned company;
    unsigned char mfr[31]; int mfr_len;
    char name[64];
} BtSurvey;

/* one `nzeus-bt survey` line -> dev; 0 = ok, -1 = not a survey line */
int  bt_parse_survey(const char *line, BtSurvey *dev);

const char *bt_company_name(unsigned id);          /* NULL = not in the built-in list */
const char *bt_uuid_name(unsigned uuid);           /* NULL = not in the built-in list */
const char *bt_apple_model(unsigned model);        /* AirPods/Beats model code -> name, or NULL */
const char *bt_address_kind(const BtSurvey *d);       /* "fixed (from the maker)", "private, changes...", ... */

/* the "Likely: ..." line: what it is, how we know, how sure (high/medium/low) */
void bt_guess(const BtSurvey *d, char *out, size_t n);
/* is it a tracker (AirTag / Find My item, Tile, SmartTag)? */
int  bt_is_tracker(const BtSurvey *d);

/* the whole ID card; list_file = the IEEE maker list (for classic/public addresses) */
void bt_card(const BtSurvey *d, const char *list_file, char *out, size_t n);

#endif

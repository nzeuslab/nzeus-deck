#ifndef NZEUS_CARSUM_H
#define NZEUS_CARSUM_H
#include "carscan.h"
#include "obd.h"

/* Turning the raw scan data into the words of the report, in English and
 * Arabic: the headline ("Healthy. No active faults."), the summary, the most
 * likely cause, what to do, and (Ford only) the wishlist. Any make: Ford's
 * own wording and code meanings are used only when the VIN says Ford. */

/* add one fault code (module + code + UDS bytes): fills its group and meaning
 * from dtc_db. Returns the new entry, or NULL when the list is full.
 * The make must be known first (vin_make): a Ford gets Ford's meanings,
 * other makes only the meanings that are the same on every car, and their
 * codes are grouped by module. */
ScanDtc *carscan_add_dtc(CarScan *s, const char *module, const char *code,
                         unsigned char failure_type, unsigned char status, int state);
/* the code already in the list from this module, or NULL */
ScanDtc *carscan_find_dtc(CarScan *s, const char *module, const char *code);

/* the self-test line ("8 of 9 self-tests done; not yet: ...") and the check-engine light */
void carscan_set_readiness(CarScan *s, const ObdInfo *info);

/* model year from the VIN's 10th letter (S = 2025), 0 = unknown */
int  vin_model_year(const char *vin);
/* the make from the VIN's first 3 letters (make_en "Toyota", make_ar "تويوتا";
 * also make_model_en/ar = the make), "" = unknown */
void vin_make(const char *vin, CarScan *s);

/* the mileage check: do the modules agree on the odometer? (sets odo_agree, odometer_km) */
void carscan_check_odometer(CarScan *s);

/* everything at the top of the report, from the data. second_bus_unreached = the
 * adapter could not reach Ford's MS-CAN bus and modules there may be missing
 * (Ford only; always 0 for other makes). */
void carscan_summarize(CarScan *s, int second_bus_unreached);

#endif

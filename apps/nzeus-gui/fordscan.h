#ifndef NZEUS_FORDSCAN_H
#define NZEUS_FORDSCAN_H

/* Ford's module names and addresses, used by the scanner (carscan.c) ONLY
 * when the VIN says the car is a Ford. On every other make a module is just
 * "Module 0x760": its name is not known for sure.
 *
 * Ford modules answer on their request address + 8 (PCM asks 7E0, answers
 * 7E8). The OBD port's main bus (HS-CAN, pins 6 and 14) reaches every module
 * on newer Fords through the gateway; older Fords (about 2011-2019) put the
 * body/comfort modules on a second bus (MS-CAN, pins 3 and 11) that only some
 * adapters (OBDLink EX/MX: STN chip) can switch to. */

typedef struct {
    const char *code;          /* "BdyCM" */
    int id;                    /* request address, e.g. 0x726 */
    int ms;                    /* 1 = on MS-CAN in older Fords (try there too when silent) */
    int sure;                  /* 1 = address confirmed in the sources, 0 = less certain */
    const char *en, *ar;       /* "Body control" */
} FordModule;

int fordscan_module_count(void);
const FordModule *fordscan_module(int i);
const FordModule *fordscan_by_id(int id);      /* the Ford module at this address, or NULL */

#endif

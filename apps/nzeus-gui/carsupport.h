#ifndef NZEUS_CARSUPPORT_H
#define NZEUS_CARSUPPORT_H
#include "carscan.h"
/* Private, complete snapshots on the SD card; never uploads a VIN. */
int carsupport_save(const CarScan *scan, const char *folder);
#endif

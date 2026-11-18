#ifndef NZEUS_CARLOG_H
#define NZEUS_CARLOG_H

/* Logging live car values to a CSV file on the SD card (open it in a
 * spreadsheet later): nzeus/car/log-YYYYMMDD-HHMMSS.csv, one row every half
 * second: the time, then each chosen value. The file is pushed to the card
 * every few seconds, so a power cut loses at most those few seconds. */

int  carlog_start(const char *folder, const int *pids, int count);   /* 0 = started */
void carlog_tick(void);               /* call often: writes a row when it is time */
void carlog_stop(void);
int  carlog_active(void);
int  carlog_rows(void);               /* rows written so far */
const char *carlog_path(void);        /* the file being (or last) written */

#endif

#ifndef NZEUS_HEALTH_H
#define NZEUS_HEALTH_H
#include <stddef.h>

/* The Health dashboard's readings (app_health.c is the screen).
 * Everything is read live; anything the console can't tell is marked unknown
 * (-1, or HEALTH_NO_TEMP), never guessed. */

#define HEALTH_NO_TEMP (-1000)
#define HEALTH_TOP 3

typedef struct {
    long mem_total_kb, mem_avail_kb;     /* RAM (-1 = unknown) */
    long swap_total_kb, swap_used_kb;    /* zram / swap (total 0 = none) */
    int  battery, charging;              /* % (-1 = unknown), charger in */
    long sd_total_mb, sd_free_mb;        /* the SD card (-1 = unknown) */
    int  temp_c;                         /* CPU temperature, HEALTH_NO_TEMP = this console can't tell */
    double uptime_s, load1;
    int  ntop;                           /* the biggest programs by memory */
    char top_name[HEALTH_TOP][32];
    long top_kb[HEALTH_TOP];
} HealthInfo;

/* where things are read from (tests point these at fake files) */
extern const char *health_thermal_path, *health_sd_path;

void health_read(HealthInfo *h);

/* "All good" (returns 0), or one worry per line, worst first (returns how many) */
int  health_verdict(const HealthInfo *h, char *out, size_t n);

/* 3725 -> "1 h 2 min" */
void health_duration(long s, char *out, size_t n);

#endif

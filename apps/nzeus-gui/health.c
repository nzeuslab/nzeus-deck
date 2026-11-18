/* health.c - the Health dashboard's readings and its "all good / here's the
 * worry" verdict. No screen code here (app_health.c is the screen), so the
 * tests can feed it made-up numbers and fake files. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "health.h"
#include "hw.h"
#include "procs.h"
#include "settings.h"

const char *health_thermal_path = "/sys/class/thermal/thermal_zone0/temp";   /* (none on this console yet) */
const char *health_sd_path = NZ_SDCARD;

/* ---------- reading ---------- */

void health_duration(long s, char *out, size_t n)
{
    if (s < 0) snprintf(out, n, "?");
    else if (s < 60) snprintf(out, n, "%ld s", s);
    else if (s < 3600) snprintf(out, n, "%ld min", s / 60);
    else if (s < 86400) snprintf(out, n, "%ld h %ld min", s / 3600, s / 60 % 60);
    else snprintf(out, n, "%ld day%s %ld h", s / 86400, s / 86400 == 1 ? "" : "s", s / 3600 % 24);
}

void health_read(HealthInfo *h)
{
    memset(h, 0, sizeof *h);
    ProcSummary s;
    if (procs_summary(&s) == 0) {
        h->mem_total_kb = s.mem_total_kb;
        h->mem_avail_kb = s.mem_avail_kb;
        h->swap_total_kb = s.swap_total_kb;
        h->swap_used_kb = s.swap_total_kb - s.swap_free_kb;
        h->uptime_s = s.uptime_s;
        h->load1 = s.load1;
    } else h->mem_total_kb = h->mem_avail_kb = -1;

    /* the program list is borrowed only while reading (13 KB: too much to keep, or for the stack) */
    ProcInfo *list = malloc(256 * sizeof *list);
    int np = list ? procs_list(list, 256) : 0;
    for (int i = 0; i < np && h->ntop < HEALTH_TOP; i++) {  /* biggest first; kernel threads use no RAM of their own */
        if (list[i].kernel || list[i].rss_kb <= 0) continue;
        snprintf(h->top_name[h->ntop], sizeof h->top_name[0], "%s", list[i].name);
        h->top_kb[h->ntop++] = list[i].rss_kb;
    }
    free(list);

    h->battery = hw_battery_percent();
    h->charging = hw_battery_charging();

    struct statvfs v;
    if (statvfs(health_sd_path, &v) == 0 && v.f_blocks) {
        h->sd_total_mb = (long)((unsigned long long)v.f_blocks * v.f_frsize / (1024 * 1024));
        h->sd_free_mb = (long)((unsigned long long)v.f_bavail * v.f_frsize / (1024 * 1024));
    } else h->sd_total_mb = h->sd_free_mb = -1;

    h->temp_c = HEALTH_NO_TEMP;
    FILE *t = fopen(health_thermal_path, "r");
    if (t) {
        long milli;
        if (fscanf(t, "%ld", &milli) == 1) h->temp_c = (int)(milli >= 1000 ? milli / 1000 : milli);   /* (some give degrees) */
        fclose(t);
    }

}

/* ---------- the verdict ---------- */

/* add one worry as a new line of out (k = where the text ends now) */
static void worry(char *out, size_t n, size_t *k, int *worries, const char *fmt, ...)
{
    if (*worries && *k + 1 < n) out[(*k)++] = '\n', out[*k] = 0;
    if (*k + 1 < n) {
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(out + *k, n - *k, fmt, ap);
        va_end(ap);
        if (w > 0) *k += (size_t)w < n - *k ? (size_t)w : n - *k - 1;
    }
    (*worries)++;
}

int health_verdict(const HealthInfo *h, char *out, size_t n)
{
    int w = 0;          /* how many worries so far */
    size_t k = 0;
    out[0] = 0;
    if (h->mem_avail_kb >= 0 && h->mem_avail_kb < 3 * 1024)
        worry(out, n, &k, &w, "Memory is very low (%ld MB free) - close an app", h->mem_avail_kb / 1024);
    else if (h->mem_avail_kb >= 0 && h->mem_avail_kb < 6 * 1024)
        worry(out, n, &k, &w, "Memory is getting low (%ld MB free)", h->mem_avail_kb / 1024);
    if (h->battery >= 0 && !h->charging && h->battery <= 10)
        worry(out, n, &k, &w, "Battery almost empty (%d%%) - plug in the charger", h->battery);
    else if (h->battery >= 0 && !h->charging && h->battery <= 20)
        worry(out, n, &k, &w, "Battery low (%d%%)", h->battery);
    if (h->sd_free_mb >= 0 && h->sd_free_mb < 100)
        worry(out, n, &k, &w, "SD card almost full (%ld MB free)", h->sd_free_mb);
    if (h->swap_total_kb > 0 && h->swap_used_kb * 100 > h->swap_total_kb * 85)
        worry(out, n, &k, &w, "zram is nearly full (%ld of %ld MB)", h->swap_used_kb / 1024, h->swap_total_kb / 1024);
    if (h->temp_c != HEALTH_NO_TEMP && h->temp_c >= 85)
        worry(out, n, &k, &w, "Running hot (%d C)", h->temp_c);
    if (h->load1 >= 4)
        worry(out, n, &k, &w, "The processor is very busy (load %.1f)", h->load1);
    if (!w) snprintf(out, n, "All good");
    return w;
}

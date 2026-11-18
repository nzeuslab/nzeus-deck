/* sysinfo.c - live system facts for the System window. Everything comes
 * from Linux's own files: /proc/meminfo, /proc/uptime, /sys/class/net. */
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include "sysinfo.h"

#ifndef NZ_SDCARD                       /* NZEUS's own folder (see settings.h) */
#define NZ_SDCARD "/tmp/nzeus-data"
#endif

/* One value from /proc/meminfo, in kB (-1 if missing). */
long sysinfo_meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long value = -1;
    size_t n = strlen(key);
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, n) && line[n] == ':') { sscanf(line + n + 1, "%ld", &value); break; }
    fclose(f);
    return value;
}

/* "connected", "on, not connected", "off" or "no dongle" */
static const char *wifi_state(void)
{
    char state[32] = "";
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return "no dongle";
    if (!fgets(state, sizeof state, f)) state[0] = 0;
    fclose(f);
    if (!strncmp(state, "up", 2)) return "connected";
    f = fopen("/sys/class/net/wlan0/flags", "r");
    unsigned flags = 0;
    if (f) { if (fscanf(f, "%x", &flags) != 1) flags = 0; fclose(f); }
    return (flags & 1) ? "on, not connected" : "off";
}

void sysinfo_kernel(char *buf, size_t size)
{
    struct utsname u;
    snprintf(buf, size, "%s", uname(&u) == 0 ? u.release : "?");
}

void sysinfo_text(char *buf, size_t size)
{
    struct utsname u;
    char kernel[65] = "?";
    if (uname(&u) == 0) snprintf(kernel, sizeof kernel, "%s", u.release);

    long total = sysinfo_meminfo_kb("MemTotal"), avail = sysinfo_meminfo_kb("MemAvailable");
    if (avail < 0) avail = sysinfo_meminfo_kb("MemFree");
    long swap_used = sysinfo_meminfo_kb("SwapTotal") - sysinfo_meminfo_kb("SwapFree");

    double sd_free = 0, sd_total = 0;
    struct statvfs s;
    if (statvfs(NZ_SDCARD, &s) == 0) {
        sd_free = (double)s.f_bavail * s.f_frsize / 1e9;
        sd_total = (double)s.f_blocks * s.f_frsize / 1e9;
    }

    double up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }

    snprintf(buf, size,
             "Kernel:  %s\n"
             "Memory:  %ld MB free of %ld MB\n"
             "Swap:  %ld MB in use\n"
             "SD card:  %.1f GB free of %.1f GB\n"
             "Wi-Fi:  %s\n"
             "Uptime:  %dh %02dm",
             kernel, avail / 1024, total / 1024, swap_used / 1024,
             sd_free, sd_total, wifi_state(), (int)(up / 3600), ((int)up / 60) % 60);
}

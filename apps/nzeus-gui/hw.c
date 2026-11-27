/* hw.c - the hardware rules that are not one device: the brightness curve,
 * the battery line, restart / power off, and the clock keeper (see hw.h).
 * The devices themselves (backlight, battery, sound...) are in hw_modern.c. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "hw.h"
#include "settings.h"

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 0..100 % -> the backlight driver's level 23..255 (a curve: equal steps look equal) */
int hw_backlight_value(int percent)
{
    int p = clamp(percent, 0, 100);
    return p < 43 ? p + 23 : p * p / 43 + 23;
}

/* battery reading (one byte, the power driver's "battery=" level) -> %: a
 * straight line from HW_BATT_EMPTY (0 %) to HW_BATT_FULL (100 %). (The
 * factory menu's formula said ~35 % when this console died at 128.) */
int hw_battery_from_adc(int b)
{
    if (b < 0) return -1;
    if (b <= HW_BATT_EMPTY) return 0;
    if (b >= HW_BATT_FULL) return 100;
    return (b - HW_BATT_EMPTY) * 100 / (HW_BATT_FULL - HW_BATT_EMPTY);
}

#ifdef NZ_TESTING                   /* PC tests: never touch the PC's own power or clock */
void hw_sync(void) {}
int hw_reboot(void) { return -1; }
int hw_test_power_offs;             /* how often a power-off was asked for (tests check it) */
int hw_power_off(void) { hw_test_power_offs++; return -1; }

/* The pretend clock: it starts at fake_base and ticks with the PC clock. */
static time_t fake_base, fake_set_at;
void hw_test_set_now(time_t t) { fake_base = t; fake_set_at = time(NULL); }
time_t hw_now(void) { return fake_base ? fake_base + (time(NULL) - fake_set_at) : time(NULL); }
static int set_system_time(time_t t) { hw_test_set_now(t); return 0; }   /* only the pretend clock moves */
#else
void hw_sync(void) { sync(); }
/* The SD Debian: restart and power off go through Debian's own shutdown.
 * Restart only with a kernel whose power driver has the watchdog restart
 * (nzeus,restart-watchdog, ports/r36s-hichip/power/nzeus-r36s-power.c): without it
 * "reboot" just stops with the screen on. The Settings app then says
 * "Reboot is not possible here". */
static int going_down;      /* switching off: no more clock saves */
const char *hw_restart_prop = "/sys/bus/platform/devices/188184d0.power/of_node/nzeus,restart-watchdog";
int hw_reboot(void)
{
    if (access(hw_restart_prop, F_OK) != 0 || access("/etc/debian_version", F_OK) != 0) return -1;
    hw_clock_save();
    going_down = 1;
    if (system("/sbin/reboot") == 0) return 0;     /* Debian's own shutdown, then the watchdog restart */
    going_down = 0;
    return -1;
}
int hw_power_off(void)
{
    hw_clock_save();
    going_down = 1;
    /* the SD Debian: its own shutdown (saves the clock, stops services, unmounts
     * /data), then the kernel switches off through the power driver (pin L01) */
    if (access("/etc/debian_version", F_OK) == 0 && access(hw_power_driver, F_OK) == 0 &&
        system("/sbin/poweroff") == 0) return 0;
    if (hw_modern_power_off() == 0) return 0;       /* (does not come back when it works) */
    going_down = 0;                                 /* it did not happen: NZEUS goes on */
    return -1;
}

time_t hw_now(void) { return time(NULL); }
static int set_system_time(time_t t)
{
    struct timeval tv = { t, 0 };
    return settimeofday(&tv, NULL);                 /* NZEUS runs as root on the console */
}
#endif

/* ---------- the clock keeper (see hw.h) ---------- */
#define CLOCK_FILE NZ_DIR "/" HW_CLOCK_FILE_NAME

#ifdef NZ_TESTING
static int going_down;      /* (the PC tests never switch off) */
#endif

int hw_clock_save(void)
{
    if (going_down) return 0;                       /* saved a moment ago; a write now could be cut off */
    time_t now = hw_now();
    if (now < HW_CLOCK_MIN) return -1;              /* not set yet (1970): keep the saved time */
    char tmp[] = CLOCK_FILE ".tmp", words[40];
    strftime(words, sizeof words, "%Y-%m-%d %H:%M:%S UTC", gmtime(&now));
    mkdir(NZ_DIR, 0755);
    FILE *f = fopen(tmp, "w");                      /* a .tmp file first, then rename it: */
    if (!f) return -1;                              /* a power cut never leaves half a file */
    fprintf(f, "%lld\n%s\n", (long long)now, words);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    return rename(tmp, CLOCK_FILE) == 0 ? 0 : -1;
}

/* the time saved in clock.txt, or 0 */
static time_t saved_time(void)
{
    long long t = 0;
    FILE *f = fopen(CLOCK_FILE, "r");
    if (!f) return 0;
    if (fscanf(f, "%lld", &t) != 1) t = 0;
    fclose(f);
    return (t >= HW_CLOCK_MIN && t < HW_CLOCK_MAX) ? (time_t)t : 0;
}

int hw_clock_restore(void)
{
    time_t saved = saved_time();
    if (!saved) return 0;                           /* nothing saved yet */
    if (hw_now() >= saved) return 0;                /* the clock is already right (or newer) */
    return set_system_time(saved) == 0;
}

int hw_set_time(time_t t)
{
    if (t < HW_CLOCK_MIN || t >= HW_CLOCK_MAX) return -1;
    if (set_system_time(t) != 0) return -1;
    hw_clock_save();                                /* saved at once: survives switching off */
    return 0;
}

void hw_clock_keep(uint32_t now_ms)
{
    static uint32_t last;
    static int started;
    if (!started) { started = 1; last = now_ms; return; }
    if ((int32_t)(now_ms - last) >= 60000) {        /* once a minute */
        last = now_ms;
        hw_clock_save();
    }
}

void hw_apply_timezone(int hours)
{
    /* POSIX writes the offset the other way round: UTC+3 is "<+03>-3" */
    char tz[32];
    hours = clamp(hours, -12, 14);
    snprintf(tz, sizeof tz, "<%+03d>%d", hours, -hours);
    setenv("TZ", tz, 1);
    tzset();
}

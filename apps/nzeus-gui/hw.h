#ifndef NZEUS_HW_H
#define NZEUS_HW_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* The console's hardware on the SD Debian (Linux 7.2): standard Linux files
 * and devices (hw_modern.c), plus the rules around them (hw.c).
 * Every function returns 0 when it worked, -1 when the device isn't there
 * (for example a kernel without that driver, or the PC). */

int hw_set_brightness(int percent);   /* screen backlight, 0..100 (sysfs backlight) */
int hw_set_volume(int percent);       /* volume, 0..100 (the ALSA card's "Master Playback Volume") */
int hw_get_volume(void);              /* 0..100, or -1 = no sound card */
/* The built-in speaker on (1) or off (0) - off while listening on headphones.
 * The R36S cannot sense headphones (no pin changes with a plug in, tested
 * 2026-10-12), so this is switched by hand. The jack plays either way.
 * hw_get_speaker: 1 on, 0 off, -1 = no such switch here (older kernel, PC). */
int hw_set_speaker(int on);
int hw_get_speaker(void);
size_t hw_pcm_write(const int16_t *frames, size_t count);   /* stereo frames to the ALSA card */
void hw_pcm_set_rate(unsigned rate);                        /* 44100 (default) or 48000: reopens the card if it changes */
int hw_battery_percent(void);         /* 0..100, or -1 if unknown */
int hw_battery_charging(void);        /* 1 = the charger is plugged in */
int hw_backlight_value(int percent);  /* 0..100 % -> the backlight level 23..255 */
int hw_battery_from_adc(int b);       /* the battery reading (1 byte) -> 0..100 %, -1 if b < 0 */
int hw_battery_raw(void);             /* the battery reading itself (the power driver's "battery="), -1 if none */
int hw_modern_power_off(void);        /* sync + switch off; -1 if this kernel can't (comes back) */
int hw_modern_online(void);           /* 1 = a network with a default route is up */

/* Where hw_modern.c finds things (the PC tests point these at fake files). */
extern const char *hw_backlight_file;   /* /sys/class/backlight/r36s-avp/brightness (0..255) */
extern const char *hw_backlight_glob;   /* /sys/class/backlight/ * : any other one, scaled to its max_brightness */
extern const char *hw_capacity_file;    /* /sys/class/power_supply/battery/capacity (0..100) */
extern const char *hw_charger_file;     /* /sys/class/power_supply/charger/online (1 = plugged in) */
extern const char *hw_levels_file;      /* .../188184d0.power/stock_levels ("ref=.. battery=.. charger=..") */
extern const char *hw_power_driver;     /* the power driver's "driver" link: there = power-off works */
extern const char *hw_ctl_dev;          /* /dev/snd/controlC0 (the "Master Playback Volume" control) */
extern const char *hw_pcm_dev;          /* /dev/snd/pcmC0D0p (NZEUS's sound out) */
extern const char *hw_headphones_file;  /* the sound driver's sysfs "headphones" (speaker switch) */
extern const char *hw_route_file;       /* /proc/net/route (hw_modern_online) */
/* Run another program full screen (the Web app's NetSurf): the frontend stops
 * drawing and reading the buttons until it ends, then takes the screen back.
 * Returns its exit code, -1 = it could not run. NULL = not possible here
 * (set by nzeus-modern; the self-test leaves it NULL, the PC tests set a fake). */
extern int (*hw_fullscreen_run)(const char *const argv[]);

/* The battery reading for 0 % and 100 %. Provisional, from measurements
 * 2026-10-06 (this console died at 128-129; empty and rested ~140; ~166-168
 * seen unplugged in use, the top not measured yet); refine after a full
 * charge-to-empty run (the blackbox logs the raw byte every 30 s). */
#define HW_BATT_EMPTY 128
#define HW_BATT_FULL  175

/* Things that leave NZEUS. They write nothing on the PC (tests). */
void hw_sync(void);
int  hw_reboot(void);
int  hw_power_off(void);

/* Time: the time zone is kept as hours from UTC (e.g. +3 = UTC+3). */
void hw_apply_timezone(int hours);

/* The clock. The console has no clock battery: switched off, it forgets the
 * time and starts again in 1970. The "clock keeper" remembers it on the SD
 * card (nzeus/clock.txt: seconds since 1970, then the date in words), every
 * minute and when NZEUS closes. At the next start, a clock that is older
 * than the saved time is set to the saved time, so it goes on from where it
 * stopped (it can't know how long the console was off).
 * On the PC tests the system clock is NEVER changed: there all of this moves
 * a pretend clock instead (hw_test_set_now). */
time_t hw_now(void);                        /* the time now (use this, not time()) */
int  hw_set_time(time_t t);                 /* set the clock (needs root) + save clock.txt; 0 = done */
int  hw_clock_save(void);                   /* write clock.txt now; 0 = done */
int  hw_clock_restore(void);                /* at start: 1 = the clock was set from clock.txt */
void hw_clock_keep(uint32_t now_ms);        /* every frame: saves once a minute */
#define HW_CLOCK_FILE_NAME "clock.txt"      /* (inside NZ_DIR) */
#define HW_CLOCK_MIN 1704067200             /* 2024-01-01: anything older is "not set" */
#define HW_CLOCK_MAX 2145916800             /* 2038-01-01: the console's 32-bit clock ends there */
#ifdef NZ_TESTING
void hw_test_set_now(time_t t);             /* PC tests: the pretend clock (0 = follow the PC clock) */
extern int hw_test_power_offs;              /* PC tests: how often hw_power_off() was called (it does nothing) */
#endif

#endif

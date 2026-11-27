/* hw_modern.c - the console's devices on the SD Debian (Linux 7.2): screen
 * brightness, battery, sound, the speaker switch, the network check and the
 * power-off call. Everything is a standard Linux file (see
 * ports/r36s-hichip/power/README.md and ports/r36s-hichip/audio):
 *
 *   brightness  /sys/class/backlight/r36s-avp/brightness   0..255, NZEUS's
 *               percent -> level curve (hw_backlight_value); another
 *               backlight device (no AVP) is scaled to its max_brightness
 *   battery     /sys/class/power_supply/battery/capacity   0..100 %
 *   charger     /sys/class/power_supply/charger/online     1 = plugged in
 *   raw levels  /sys/bus/platform/devices/188184d0.power/stock_levels
 *               "ref=.. battery=.. charger=..": the battery and charger
 *               readings on the factory scale (~128 empty .. ~175 full)
 *   power off   sync() + reboot(RB_POWER_OFF): the kernel's power driver pulls
 *               the power pin (L14) low, like holding the power button
 *   sound       the ALSA card "R36S": volume = its "Master Playback Volume"
 *               control (/dev/snd/controlC0), NZEUS's sound = 44.1 kHz stereo
 *               written to /dev/snd/pcmC0D0p with the kernel's own ALSA
 *               ioctls (no alsa-lib: one static program). No card = volume
 *               says "not possible", sound dropped.
 *   speaker     the sound driver's sysfs "headphones" setting
 *   network     /proc/net/route: online = a default route is up
 *
 * A kernel without one of these drivers is fine: every function then
 * quietly says "unknown" (-1 / 0). */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>
#include "hw.h"

const char *hw_backlight_file = "/sys/class/backlight/r36s-avp/brightness";
const char *hw_backlight_glob = "/sys/class/backlight/*";     /* any other backlight (scaled) */
const char *hw_capacity_file = "/sys/class/power_supply/battery/capacity";
const char *hw_charger_file = "/sys/class/power_supply/charger/online";
const char *hw_levels_file = "/sys/bus/platform/devices/188184d0.power/stock_levels";
const char *hw_power_driver = "/sys/bus/platform/devices/188184d0.power/driver";   /* there only once the driver runs */

/* a small number from a sysfs file (e.g. "87\n"), or -1 */
static int read_number(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;
    if (!f) return -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* one value from stock_levels ("ref=247 battery=172 charger=0"), or -1 */
static int stock_level(const char *name)
{
    char text[128], key[32];
    FILE *f = fopen(hw_levels_file, "r");
    if (!f) return -1;
    if (!fgets(text, sizeof text, f)) text[0] = 0;
    fclose(f);
    snprintf(key, sizeof key, "%s=", name);
    size_t k = strlen(key);
    for (const char *p = text; *p; p++)                 /* "name=" at the start or after a space */
        if ((p == text || p[-1] == ' ') && !strncmp(p, key, k)) {
            int v;
            return sscanf(p + k, "%d", &v) == 1 ? v : -1;
        }
    return -1;
}

/* Brightness. r36s-avp takes the stock 0..255 level as it is. A kernel
 * without it (the planned "pure Linux" one has a native PWM backlight) may
 * have another standard backlight device: then the same level is scaled to
 * that device's max_brightness. */
/* write LEVEL (0..max) to one backlight's brightness file; 0 = done */
static int write_level(const char *path, int level)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    char text[16];
    int n = snprintf(text, sizeof text, "%d\n", level);
    int ok = write(fd, text, (size_t)n) == n;
    close(fd);
    return ok ? 0 : -1;
}

int hw_set_brightness(int percent)
{
    int level = hw_backlight_value(percent);           /* 23..255, the stock scale */
    /* r36s-avp first (the stock 0..255 scale as it is). In pure Linux it is there
     * but refuses (EPERM: the AVP is not running) - then the other backlights,
     * e.g. r36s-pwm, scaled to their own max_brightness. */
    if (access(hw_backlight_file, F_OK) == 0 && write_level(hw_backlight_file, level) == 0) return 0;
    glob_t g;
    if (glob(hw_backlight_glob, 0, NULL, &g) != 0) return -1;   /* no backlight driver at all */
    int done = -1;
    for (size_t i = 0; i < g.gl_pathc && done != 0; i++) {
        char path[160], max_path[160];
        snprintf(path, sizeof path, "%s/brightness", g.gl_pathv[i]);
        if (!strcmp(path, hw_backlight_file)) continue;          /* (refused above) */
        snprintf(max_path, sizeof max_path, "%s/max_brightness", g.gl_pathv[i]);
        int max = read_number(max_path);
        if (max > 0) done = write_level(path, level * max / 255);
    }
    globfree(&g);
    return done;
}

/* The battery reading on the stock scale (~128 empty .. ~175 full), or -1.
 * A value under 100 has never been seen on this console (it switches off at
 * ~128), so it is taken as a bad reading rather than an empty battery: core.c
 * switches the console off after a minute of "empty" readings, and a new,
 * unproven driver must not be able to cause that by mistake. */
int hw_battery_raw(void)
{
    int raw = stock_level("battery");
    return raw >= 100 && raw <= 255 ? raw : -1;
}

int hw_battery_charging(void)
{
    int online = read_number(hw_charger_file);
    if (online >= 0) return online == 1;
    return stock_level("charger") >= 64;                /* the stock rule: 64 or more = plugged in */
}

/* Linux's battery % if the driver gives one, else NZEUS's own line from the raw reading */
int hw_battery_percent(void)
{
    int pct = read_number(hw_capacity_file);
    if (pct >= 0 && pct <= 100) return pct;
    return hw_battery_from_adc(hw_battery_raw());
}

/* ---- Sound: the ALSA card "R36S" ----
 * Volume: NZEUS's 0..100 % is SQUARED onto the card's 0..255 volume byte, so
 * the steps sound even (50 % = 64 = a quarter of the byte, 100 % = 255). */
const char *hw_ctl_dev = "/dev/snd/controlC0";            /* (tests use fake paths) */
const char *hw_pcm_dev = "/dev/snd/pcmC0D0p";
#define VOLUME_CONTROL "Master Playback Volume"

/* read (set = 0) or write the volume byte; returns it (0..255) or -1 */
static int ctl_volume(int set, int value)
{
    struct snd_ctl_elem_value v;
    int fd = open(hw_ctl_dev, O_RDWR | O_CLOEXEC), r;
    if (fd < 0) return -1;
    memset(&v, 0, sizeof v);
    v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)v.id.name, sizeof v.id.name, "%s", VOLUME_CONTROL);
    if (set) {
        v.value.integer.value[0] = value;
        r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
    } else {
        r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v);
    }
    close(fd);
    return r < 0 ? -1 : (int)v.value.integer.value[0];
}

int hw_set_volume(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return ctl_volume(1, (percent * percent * 255 + 5000) / 10000) < 0 ? -1 : 0;
}

int hw_get_volume(void)
{
    int byte = ctl_volume(0, 0), p = 0;
    if (byte < 0) return -1;
    while (p < 100 && ((p + 1) * (p + 1) * 255 + 5000) / 10000 <= byte) p++;   /* (back from the square) */
    return p;
}

/* The speaker switch: the sound driver's "headphones" setting (root only).
 * Reading it gives "level=L plugged_level=P in=I amp=A". Writing the pin's
 * present level L makes the driver treat headphones as plugged in, so the
 * speaker amp stays off (the jack still plays); "off" = speaker as usual. */
const char *hw_headphones_file = "/sys/bus/platform/devices/1880a000.audio/headphones";
int (*hw_fullscreen_run)(const char *const argv[]);      /* (see hw.h: set by nzeus-modern) */

static int headphones_read(int *level, int *in)
{
    char line[96];
    int plugged;
    FILE *f = fopen(hw_headphones_file, "r");
    if (!f) return -1;
    int ok = fgets(line, sizeof line, f) && sscanf(line, "level=%d plugged_level=%d in=%d", level, &plugged, in) == 3;
    fclose(f);
    return ok ? 0 : -1;                                   /* ("no headphone pin described": no switch) */
}

int hw_set_speaker(int on)
{
    int level, in;
    if (headphones_read(&level, &in) != 0) return -1;
    FILE *f = fopen(hw_headphones_file, "w");
    if (!f) return -1;
    int ok = fputs(on ? "off\n" : level ? "1\n" : "0\n", f) >= 0;
    if (fclose(f) != 0) ok = 0;                            /* (sysfs says no only at the close) */
    return ok ? 0 : -1;
}

int hw_get_speaker(void)
{
    int level, in;
    if (headphones_read(&level, &in) != 0) return -1;
    return in ? 0 : 1;
}

/* Sound out: NZEUS's stereo at 44.1 kHz (48 kHz for 48 kHz songs, played as
 * they are), 1024-frame periods x 4. The device is opened at the first sound;
 * writes never block. The caller keeps frames that do not fit yet.
 * After half a second of pure silence the card is stopped AND closed: the amp
 * goes off (no hiss between songs or while paused) and other programs (aplay,
 * a terminal's sound test) can use the one sound card meanwhile. The next
 * sound opens it again. */
#define PCM_PERIOD  1024
#define PCM_PERIODS 4
static int pcm_fd = -1;
static long pcm_retry_at;                                  /* seconds (no card / busy: try again later) */
static unsigned pcm_rate = 44100;
static size_t pcm_silent;                                  /* silent frames in a row */

void hw_pcm_set_rate(unsigned rate)
{
    if (rate == pcm_rate) return;
    pcm_rate = rate;
    if (pcm_fd >= 0) { close(pcm_fd); pcm_fd = -1; }       /* (the card's PLL changes at the next open) */
    pcm_retry_at = 0;
}

static void hp_mask(struct snd_pcm_hw_params *p, int param, unsigned bit)
{
    struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[bit >> 5] = 1u << (bit & 31);
}
static void hp_value(struct snd_pcm_hw_params *p, int param, unsigned value)
{
    struct snd_interval *i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    memset(i, 0, sizeof *i);
    i->min = i->max = value;
    i->integer = 1;
}

static int pcm_open(void)
{
    struct snd_pcm_hw_params hp;
    struct snd_pcm_sw_params sp;
    int fd, k;

    fd = open(hw_pcm_dev, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    memset(&hp, 0, sizeof hp);
    for (k = 0; k <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; k++)
        memset(&hp.masks[k], 0xff, sizeof hp.masks[k]);
    for (k = 0; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k++)
        hp.intervals[k].max = ~0u;
    hp.rmask = ~0u;
    hp.info = ~0u;
    hp_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hp_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    hp_mask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hp_value(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    hp_value(&hp, SNDRV_PCM_HW_PARAM_RATE, pcm_rate);
    hp_value(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PCM_PERIOD);
    hp_value(&hp, SNDRV_PCM_HW_PARAM_PERIODS, PCM_PERIODS);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) { close(fd); return -1; }
    memset(&sp, 0, sizeof sp);
    sp.period_step = 1;
    sp.avail_min = PCM_PERIOD;
    sp.xfer_align = 1;
    sp.start_threshold = 2 * PCM_PERIOD;                   /* start with 46 ms queued */
    sp.stop_threshold = PCM_PERIOD * PCM_PERIODS;          /* empty = stop (the amp goes off) */
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Return frames accepted by ALSA; the caller retries the remaining suffix.
 * With no sound card, consume the sound deliberately so the UI keeps going. */
size_t hw_pcm_write(const int16_t *frames, size_t count)
{
    struct snd_xferi x;
    size_t i, taken = 0;
    int recoveries = 0, interrupts = 0;

    if (!count) return 0;
    for (i = 0; i < count * 2 && !frames[i]; i++) {}
    if (i < count * 2) {
        pcm_silent = 0;
    } else if (pcm_fd < 0 && pcm_silent > pcm_rate / 2) {
        return count;                                              /* still quiet, card already let go: nothing to do */
    } else if ((pcm_silent += count) > pcm_rate / 2) {             /* long silence: stop the card now */
        if (pcm_fd >= 0) {                                         /* (once: stopped = amp off at once) */
            ioctl(pcm_fd, SNDRV_PCM_IOCTL_DROP);
            close(pcm_fd);                                         /* let other programs have the card */
            pcm_fd = -1;
        }
        return count;
    }
    if (pcm_fd < 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec < pcm_retry_at) return count;
        pcm_fd = pcm_open();
        if (pcm_fd < 0) { pcm_retry_at = ts.tv_sec + 2; return count; }
    }
    while (taken < count) {
        memset(&x, 0, sizeof x);
        x.buf = (void *)(frames + taken * 2);
        x.frames = count - taken;
        if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
            if (x.result <= 0 || (size_t)x.result > count - taken) break;
            taken += (size_t)x.result;
            continue;
        }
        if (errno == EINTR && interrupts++ < 4) continue;
        if ((errno != EPIPE && errno != ESTRPIPE && errno != EBADFD) || recoveries++ >= 2) break;
        if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) break;
    }
    return taken;
}

/* Network: the kernel always has TCP/IP. Online = some network is up with a
 * way out (a default route in /proc/net/route: destination 00000000, flag
 * 1 = up). Used by the Claude app, which then says "No network" instead of
 * trying. */
const char *hw_route_file = "/proc/net/route";            /* (tests use a fake one) */
int hw_modern_online(void)
{
    char line[256], iface[32];
    unsigned long dest, gateway;
    unsigned flags;
    int online = 0;
    FILE *f = fopen(hw_route_file, "r");
    if (!f) return 0;
    while (!online && fgets(line, sizeof line, f))     /* (the first line is the header: skipped by sscanf) */
        if (sscanf(line, "%31s %lx %lx %x", iface, &dest, &gateway, &flags) == 4 &&
            dest == 0 && (flags & 1) && strcmp(iface, "lo"))
            online = 1;
    fclose(f);
    return online;
}

/* Switch off. Only when the kernel's power driver (nzeus-r36s-power.c, which
 * adds the power-off handler) is running: without it, RB_POWER_OFF only halts
 * Linux and leaves the console on, frozen. */
int hw_modern_power_off(void)
{
    if (access(hw_power_driver, F_OK) != 0) return -1;
#ifdef NZ_TESTING
    return -1;                                          /* PC tests: never switch the PC off */
#endif
    sync();
    reboot(RB_POWER_OFF);                               /* does not come back when it works */
    return -1;
}

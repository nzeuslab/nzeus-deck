/* nzeus-modern.c - NZEUS Deck on the NEW Linux 7.2 kernel, as ONE static program.
 *
 *   nzeus-modern [SECONDS]            run NZEUS on the screen; stop after SECONDS
 *                                     (0 or nothing = until stopped)
 *   nzeus-modern --selftest OUT.ppm   no screen, no buttons: run NZEUS in memory,
 *                                     press Right once, save the home screen as OUT.ppm
 *
 * The NZEUS core (apps/nzeus-gui) is a libretro-style "core"; this program
 * is its frontend, both in one: the core is linked straight in and its
 * retro_* functions are called directly - no dlopen, no shared libraries,
 * no maker code.
 *
 * Only standard Linux interfaces:
 *   screen   /dev/fb0 ("hc16xx-gma-argb", 640x480, 32-bit ARGB8888), checked
 *            exactly like ports/r36s-hichip/input/box-demo.c. The driver only shows our
 *            picture after FBIOBLANK UNBLANK; at the end FBIOBLANK POWERDOWN
 *            hands the screen back to the firmware (also on Ctrl+C, SIGTERM and
 *            on a crash).
 *   buttons  /dev/input/event* named "r36s-keys", "serializer-keys" (and
 *            "r36s-power-button", only logged). Read when the core asks
 *            (input_poll), never in a busy loop. USB keyboards are left to the
 *            core (usbkbd.c).
 *   sound    the ALSA card "R36S" (host kernel): hw_pcm_write() in hw_modern.c;
 *            without the card the core's sound is dropped.
 *   files    NZEUS's folder is /tmp/nzeus-data (compiled in as NZ_SDCARD).
 *            NZ_DATA=/folder puts it elsewhere (nzeus-deck: /data/nzeus/deck;
 *            never on a FAT file system). The factory FAT partition only holds
 *            boot files: it is not shown inside NZEUS.
 *
 * 60 pictures a second with a sleep between them. Only the rows of the
 * picture that changed are converted and copied (nzeus_dirty_rows), so an
 * idle screen costs almost nothing.
 *
 * Log lines (stdout): NZEUS_MODERN_ACTIVE, KEY <name>, ALIVE (every minute),
 * NZEUS_MODERN_END, FIRMWARE_LAYER_RESTORE_REQUESTED.
 * Exit codes: 0 fine, 1 usage, 3 no usable screen, 5 no usable data folder. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/magic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "libretro.h"
#include "display.h"            /* NZ_W, NZ_H, nzeus_dirty_rows */
#include "home.h"               /* home_selected (the self-test) */
#include "hw.h"                 /* hw_get_volume / hw_set_volume (Vol+) */
#include "settings.h"           /* the volume is kept (settings "volume", as Settings > Sound does) */
#include "ui.h"                 /* ui_toast (Vol+ message) */
#include "settings.h"           /* NZ_SDCARD, NZ_DIR (the data folder) */
#include "shot.h"               /* shot_cmd_dir (the self-test uses its own) */

#define W NZ_W
#define H NZ_H
#define FB_NAME "hc16xx-gma-argb"

extern int nzeus_restart_wanted;            /* core.c: "Restart NZEUS" was asked for */

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }

static struct timespec started;
static long ms_since_start(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - started.tv_sec) * 1000L + (now.tv_nsec - started.tv_nsec) / 1000000L;
}

/* ---------------- the screen ---------------- */
static uint32_t *screen;                    /* the picture the viewer sees (fb0, or memory in the self-test) */
static int fb_fd = -1;
static size_t fb_size;
static pid_t main_pid;
static long frames_run, rows_copied;
static int first_picture = 1;
/* Fast screen (2026-10-12): the display driver can also show RGB565, the
 * format NZEUS draws in. Then each changed row is copied as it is - half the
 * bytes of ARGB8888 and no per-pixel conversion (this console's memory is
 * slow: a full 32-bit frame took 17.9 ms to write). The NORMAL mode since
 * 2026-10-12 (I checked the colours on the console: perfect; a full-screen
 * change went from 26 to 20 ms of CPU). Any doubt (driver refuses, odd answer)
 * = the 32-bit screen; the file below forces it (emergency way back). */
static int fb16;
#define NORMAL_SCREEN_FLAG NZ_DIR "/normal-screen"
/* Direct drawing (2026-10-12): with the 16-bit screen, LVGL writes its
 * finished rows straight into the screen memory (nzeus_display_target), so
 * video() has nothing to copy. Switched back to the internal picture BEFORE
 * the screen is given back (nothing may draw into unmapped memory). */
static int direct;

/* ask the driver for 16 or 32 bits a pixel; 0 = it took it and says so */
static int set_depth(int fd, unsigned bpp)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var)) return -1;
    var.bits_per_pixel = bpp;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var)) return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) return -1;
    if (var.bits_per_pixel != bpp || fix.line_length != W * (bpp / 8)) return -1;
    if (bpp == 16 && (var.red.offset != 11 || var.green.offset != 5 || var.blue.offset != 0)) return -1;
    return 0;
}

/* Hand the screen back to the firmware. Safe to call twice; only the main
 * process does it (a child the core started must never). */
static void restore_screen(void)
{
    if (fb_fd < 0 || getpid() != main_pid) return;
    if (direct) { nzeus_display_target(NULL); direct = 0; }  /* stop drawing into the screen first */
    if (fb16) { set_depth(fb_fd, 32); fb16 = 0; }            /* other programs expect the normal 32 bits */
    if (ioctl(fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN)) perror("nzeus-modern: restore firmware display");
    else puts("FIRMWARE_LAYER_RESTORE_REQUESTED");
    fflush(stdout);
    munmap(screen, fb_size);
    close(fb_fd);
    fb_fd = -1;
    screen = NULL;
}

/* A crash: give the screen back first (a frozen picture would stay up), then
 * crash as usual. */
static void crashed(int sig)
{
    if (fb_fd >= 0 && getpid() == main_pid) {
        if (fb16) set_depth(fb_fd, 32);
        ioctl(fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Open /dev/fb0 with the same checks as box-demo (refuse anything unexpected). */
static int open_screen(void)
{
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("nzeus-modern: open framebuffer"); return -1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
        perror("nzeus-modern: framebuffer info"); close(fd); return -1;
    }
    if (strncmp(fix.id, FB_NAME, sizeof(fix.id)) ||
        var.xres != W || var.yres != H || var.bits_per_pixel != 32 ||
        var.red.offset != 16 || var.green.offset != 8 || var.blue.offset != 0 ||
        var.transp.offset != 24 || fix.line_length != W * 4 || fix.smem_len != W * H * 4) {
        fputs("nzeus-modern: refusing unexpected framebuffer\n", stderr); close(fd); return -1;
    }
    void *p = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("nzeus-modern: mmap framebuffer"); close(fd); return -1; }
    screen = p;
    fb_size = fix.smem_len;
    fb_fd = fd;
    if (access(NORMAL_SCREEN_FLAG, F_OK) != 0) {                   /* the fast 16-bit screen (unless forced off) */
        if (set_depth(fd, 16) == 0) fb16 = 1;
        else { set_depth(fd, 32); fputs("nzeus-modern: fast screen refused: normal 32-bit screen\n", stderr); }
    }
    printf("SCREEN %s\n", fb16 ? "RGB565 (fast)" : "ARGB8888");
    if (fb16) memset(screen, 0, (size_t)W * H * 2);                 /* black, before it is shown */
    else for (int i = 0; i < W * H; i++) screen[i] = 0xff000000u;
    if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK)) {                 /* the GMA layer on: our picture shows */
        perror("nzeus-modern: activate GMA");
        munmap(p, fb_size); close(fd); fb_fd = -1; screen = NULL;
        return -1;
    }
    return 0;
}

/* The core draws RGB565 (16 bits a pixel); the screen wants ARGB8888 with the
 * alpha byte 0xff. Each 5- or 6-bit colour is stretched to 8 bits by repeating
 * its top bits (so white stays 0xff, not 0xf8). */
static inline uint32_t argb(uint16_t p)
{
    uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    return 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || !screen || w != W || h != H || pitch != W * 2) return;
    int top = 0, bottom = H - 1;
    nzeus_dirty_rows(&top, &bottom);                 /* the rows that changed since last time */
    if (first_picture) { top = 0; bottom = H - 1; first_picture = 0; }
    if (top < 0) return;                             /* nothing changed: nothing to copy */
    const uint16_t *src = data;
    if (direct) {                                    /* LVGL already drew into the screen: nothing to copy */
        rows_copied += bottom - top + 1;
        return;
    }
    if (fb16) {                                      /* same format: one straight copy of the changed rows */
        memcpy((uint16_t *)screen + top * W, src + top * W, (size_t)(bottom - top + 1) * W * 2);
        rows_copied += bottom - top + 1;
        return;
    }
    for (int y = top; y <= bottom; y++) {
        const uint16_t *s = src + y * W;
        uint32_t *d = screen + y * W;
        for (int x = 0; x < W; x++) d[x] = argb(s[x]);
    }
    rows_copied += bottom - top + 1;
}

/* ---------------- the buttons ---------------- */
/* Linux key code -> libretro button (-1 = only logged). Names as printed on the console. */
static const struct { int code, id; const char *name; } keymap[] = {
    { KEY_UP, RETRO_DEVICE_ID_JOYPAD_UP, "UP" },          { KEY_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN, "DOWN" },
    { KEY_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT, "LEFT" },    { KEY_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT, "RIGHT" },
    { KEY_LEFTCTRL, RETRO_DEVICE_ID_JOYPAD_A, "A" },      { KEY_LEFTALT, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { KEY_SPACE, RETRO_DEVICE_ID_JOYPAD_X, "X" },         { KEY_LEFTSHIFT, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { KEY_TAB, RETRO_DEVICE_ID_JOYPAD_L, "L1" },          { KEY_BACKSPACE, RETRO_DEVICE_ID_JOYPAD_R, "R1" },
    { KEY_ESC, RETRO_DEVICE_ID_JOYPAD_SELECT, "SELECT" }, { KEY_ENTER, RETRO_DEVICE_ID_JOYPAD_START, "START" },
    { KEY_PAGEUP, RETRO_DEVICE_ID_JOYPAD_L2, "L2" },      { KEY_PAGEDOWN, RETRO_DEVICE_ID_JOYPAD_R2, "R2" },
    { KEY_VOLUMEUP, -1, "VOL+" },                         { KEY_MENU, -1, "FN" },
    { KEY_VOLUMEDOWN, -1, "VOL-" },
    { KEY_POWER, -1, "POWER (hold 3 s: the kernel switches off)" },
};
#define KEYMAP_N ((int)(sizeof keymap / sizeof keymap[0]))

/* only the console's own button devices (a USB keyboard's Enter must not be START) */
static const char *const button_devices[] = { "r36s-keys", "serializer-keys", "r36s-power-button" };

#define MAX_DEVICES 8
static struct { int fd; uint32_t ids; } dev[MAX_DEVICES];   /* ids: the buttons this device has */
static int dev_count;
static uint32_t held;                        /* one bit per RETRO_DEVICE_ID_JOYPAD_* held now */
static unsigned presses;

static int has_bit(const unsigned char *bits, int n) { return bits[n / 8] >> (n % 8) & 1; }

static void open_buttons(void)
{
    for (int n = 0; n < 16 && dev_count < MAX_DEVICES; n++) {
        char path[32], name[64] = "";
        unsigned char caps[KEY_MAX / 8 + 1] = { 0 };
        snprintf(path, sizeof path, "/dev/input/event%d", n);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        int ours = 0;
        for (size_t i = 0; i < sizeof button_devices / sizeof button_devices[0]; i++)
            if (!strcmp(name, button_devices[i])) ours = 1;
        if (!ours) { close(fd); continue; }
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof caps), caps);
        dev[dev_count].fd = fd;
        dev[dev_count].ids = 0;
        for (int k = 0; k < KEYMAP_N; k++)
            if (keymap[k].id >= 0 && has_bit(caps, keymap[k].code)) dev[dev_count].ids |= 1u << keymap[k].id;
        dev_count++;
        printf("BUTTONS %s = %s\n", path, name);
    }
    if (!dev_count) puts("NO_BUTTON_DEVICES (NZEUS shows, but can't be used)");
    fflush(stdout);
}

/* Vol+ (L13) and Vol- (L12, only while "nzeus-voldown key" has taken the pin
 * from the debug serial line): one step, or say why nothing changes. Runs in
 * input_poll, i.e. on the core's own thread. */
static void volume_step(int step)
{
    char msg[64];
    int v = hw_get_volume();
    if (v < 0) {
        ui_toast("No sound on this system yet");
        return;
    }
    v += step;
    v = v > 100 ? 100 : v < 0 ? 0 : v;
    if (hw_set_volume(v) < 0) {
        ui_toast("Volume: not possible here");
        return;
    }
    settings_set_int("volume", v);
    snprintf(msg, sizeof msg, "Volume %d %%", v);
    ui_toast(msg);
}

/* FN + Vol+ or Vol-: the speaker on/off (for headphones - this console cannot
 * sense them). Saved like Settings > Sound > Speaker. */
static void speaker_toggle(void)
{
    int on = hw_get_speaker();
    if (on < 0 || hw_set_speaker(!on) != 0) {
        ui_toast("Speaker: no switch on this system");
        return;
    }
    settings_set_int("speaker", !on);
    ui_toast(on ? "Speaker off: headphones only (FN + Vol = on)" : "Speaker on");
}

/* One key went down (value 1) or up (0). Auto-repeat (2) is ignored: the core
 * repeats a held D-pad direction itself. */
static void key_event(int code, int value)
{
    static int fn_down;                               /* FN is held (for FN + Vol) */
    if (value == 2) return;
    if (code == KEY_MENU) fn_down = value != 0;
    if ((code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN) && value == 1 && fn_down) speaker_toggle();
    else if (code == KEY_VOLUMEUP && value == 1) volume_step(10);
    else if (code == KEY_VOLUMEDOWN && value == 1) volume_step(-10);
    for (int k = 0; k < KEYMAP_N; k++) {
        if (keymap[k].code != code) continue;
        if (keymap[k].id >= 0) {
            if (value) held |= 1u << keymap[k].id;
            else held &= ~(1u << keymap[k].id);
        }
        if (value == 1) {
            long ms = ms_since_start();
            presses++;
            printf("KEY %s t=%ld.%03ld\n", keymap[k].name, ms / 1000, ms % 1000);
            fflush(stdout);
        }
        return;
    }
}

/* The kernel lost some events (its buffer overflowed): ask it which keys of
 * this device are down now. */
static void resync(int i)
{
    unsigned char down[KEY_MAX / 8 + 1] = { 0 };
    uint32_t now = 0;
    if (ioctl(dev[i].fd, EVIOCGKEY(sizeof down), down) < 0) return;
    for (int k = 0; k < KEYMAP_N; k++)
        if (keymap[k].id >= 0 && has_bit(down, keymap[k].code)) now |= 1u << keymap[k].id;
    held = (held & ~dev[i].ids) | (now & dev[i].ids);
}

/* The core asks at the start of every picture: collect what happened since. */
static void input_poll(void)
{
    for (int i = 0; i < dev_count; i++) {
        struct input_event ev;
        int dropped = 0;
        while (read(dev[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type == EV_SYN && ev.code == SYN_DROPPED) dropped = 1;
            else if (ev.type == EV_KEY) key_event(ev.code, ev.value);
        }
        if (dropped) resync(i);
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id > 15) return 0;
    return (held >> id) & 1;
}

static void close_buttons(void)
{
    while (dev_count) close(dev[--dev_count].fd);
}

/* ---------------- another program full screen (hw_fullscreen_run) ---------------- */
/* The Web app's NetSurf: it draws into the screen itself. Our picture layer
 * stays switched on (no blank: the driver then shows whatever is in the screen
 * memory, in the format the program sets), we only stop drawing; this blocks
 * the Deck until the program ends. Afterwards: our colour depth again, a full
 * redraw, and the presses made meanwhile thrown away (they were the
 * program's), the buttons' state read fresh. A stop request (SIGTERM) ends the
 * program too. */
static int run_fullscreen(const char *const argv[])
{
    if (fb_fd < 0 || !argv || !argv[0]) return -1;
    if (direct) nzeus_display_target(NULL);          /* nothing of ours draws into the screen now */
    pid_t pid = fork();
    if (pid < 0) {
        if (direct) nzeus_display_target((uint16_t *)screen);
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    printf("FULLSCREEN %s (pid %d)\n", argv[0], (int)pid);
    int status = 0, stopped = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { status = -1; break; }
        if (!running && !stopped) { kill(pid, SIGTERM); stopped = 1; }   /* the Deck is being stopped */
    }
    set_depth(fb_fd, fb16 ? 16 : 32);                /* (the program may have changed it) */
    if (direct) nzeus_display_target((uint16_t *)screen);   /* (redraws everything into it) */
    else first_picture = 1;
    for (int i = 0; i < dev_count; i++) {            /* the program's presses are not ours */
        struct input_event ev;
        while (read(dev[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {}
        resync(i);
    }
    int rc = status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    printf("FULLSCREEN_END rc=%d\n", rc);
    return rc == 127 ? -1 : rc;
}

/* ---------------- NZEUS's folder ---------------- */
/* Settings, notes, screenshots... go to NZ_SDCARD (/tmp/nzeus-data, RAM:
 * forgotten at power-off). NZ_DATA=/folder: a link /tmp/nzeus-data -> there.
 * Refused on a FAT file system (that would be the SD card) or a read-only one. */
static int prepare_data(void)
{
    const char *want = getenv("NZ_DATA");
    struct stat st;
    struct statfs fs;
    if (lstat(NZ_SDCARD, &st) == 0 && S_ISLNK(st.st_mode)) unlink(NZ_SDCARD);   /* (the last start's link) */
    if (want && *want && strcmp(want, NZ_SDCARD)) {
        if (lstat(NZ_SDCARD, &st) == 0)
            printf("NZ_DATA ignored: %s is already a real folder\n", NZ_SDCARD);
        else if (symlink(want, NZ_SDCARD) != 0) {
            perror("nzeus-modern: link to NZ_DATA");
            return -1;
        }
    }
    mkdir(NZ_SDCARD, 0755);
    mkdir(NZ_DIR, 0755);
    const char *why = NULL;
    if (statfs(NZ_DIR, &fs) != 0 || access(NZ_DIR, W_OK) != 0) why = "can't write there";
    else if (fs.f_type == MSDOS_SUPER_MAGIC || fs.f_type == 0x2011BAB0 /* exFAT */)
        why = "it is on a FAT file system (the SD card?)";
    if (why) {
        fprintf(stderr, "nzeus-modern: refusing data folder %s: %s\n", NZ_DIR, why);
        if (lstat(NZ_SDCARD, &st) == 0 && S_ISLNK(st.st_mode)) unlink(NZ_SDCARD);
        return -1;
    }
    int linked = lstat(NZ_SDCARD, &st) == 0 && S_ISLNK(st.st_mode);
    printf("DATA %s%s%s\n", NZ_SDCARD, linked ? " -> " : "", linked ? want : "");
    return 0;
}

/* Older versions showed the factory FAT card inside NZEUS's folder as a link
 * "sdcard-ro". That partition now only holds boot files: a link left over
 * from such a start is removed (only the link, never what it points at). */
static void remove_old_card_link(void)
{
    const char link[] = NZ_SDCARD "/sdcard-ro";
    struct stat st;
    if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode) && unlink(link) == 0) printf("OLD_CARD_LINK_REMOVED %s\n", link);
}

/* ---------------- the core ---------------- */
static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:   return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SHUTDOWN:           running = 0; return true;   /* NZEUS closes (Restart NZEUS) */
    default:                                   return false;
    }
}

/* the core's 44.1 kHz stereo sound -> the ALSA card (dropped without one) */
static size_t audio_batch(const int16_t *data, size_t frames) { return hw_pcm_write(data, frames); }
static void audio_one(int16_t l, int16_t r) { int16_t f[2] = { l, r }; hw_pcm_write(f, 1); }

static void start_core(void)
{
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio_one);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();
    retro_load_game(NULL);
}

static void stop_core(void)
{
    retro_unload_game();
    retro_deinit();
}

/* Wait for the next picture's turn (60 a second). Far behind (a long task in
 * an app): start counting again instead of racing to catch up. */
static void next_frame(struct timespec *due)
{
    due->tv_nsec += 1000000000L / 60;
    if (due->tv_nsec >= 1000000000L) { due->tv_nsec -= 1000000000L; due->tv_sec++; }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long behind_ms = (now.tv_sec - due->tv_sec) * 1000 + (now.tv_nsec - due->tv_nsec) / 1000000;
    if (behind_ms > 100) *due = now;
    else while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, due, NULL) == EINTR && running) {}
}

static void run_frames(long n)
{
    struct timespec due;
    clock_gettime(CLOCK_MONOTONIC, &due);
    while (n-- > 0 && running) { retro_run(); frames_run++; next_frame(&due); }
}

/* ---------------- --selftest ---------------- */
static int failures;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = { screen[i] >> 16 & 0xff, screen[i] >> 8 & 0xff, screen[i] & 0xff };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

static int selftest(const char *out)
{
    static unsigned char seen[65536 / 8];             /* which colours appear (as RGB565) */
    char what[160];
    int colours = 0, opaque = 1;
    screen = calloc(W * H, sizeof *screen);
    if (!screen || prepare_data() != 0) return 5;
    shot_cmd_dir = "/tmp/nzeus-modern-selftest-cmd";   /* (not the real nzctl folder: no outside requests) */
    remove_old_card_link();
    start_core();
    run_frames(120);                                  /* 2 seconds: the home screen settles */
    for (int i = 0; i < W * H; i++) {
        uint32_t p = screen[i];
        unsigned k = (p >> 8 & 0xf800) | (p >> 5 & 0x07e0) | (p >> 3 & 0x001f);
        if (!(seen[k / 8] >> (k % 8) & 1)) { seen[k / 8] |= 1 << (k % 8); colours++; }
        if (p >> 24 != 0xff) opaque = 0;
    }
    snprintf(what, sizeof what, "the home screen is drawn (%d different colours)", colours);
    check(colours > 20, what);
    check(opaque, "every pixel has alpha 0xff (ARGB8888 for the GMA layer)");
    check(write_ppm(out) == 0, "screenshot saved");
    printf("      %s\n", out);

    int before = home_selected();                     /* Right, through the same path as a real button */
    key_event(KEY_RIGHT, 1); run_frames(2); key_event(KEY_RIGHT, 0); run_frames(20);
    snprintf(what, sizeof what, "Right (KEY_RIGHT) moves the home selection (tile %d -> %d)", before, home_selected());
    check(home_selected() == before + 1, what);
    key_event(KEY_LEFT, 1); run_frames(2); key_event(KEY_LEFT, 0); run_frames(20);
    check(home_selected() == before, "Left (KEY_LEFT) moves it back");

    long rows_before = rows_copied, frames_before = frames_run;
    run_frames(60);                                    /* one idle second */
    double avg = (double)(rows_copied - rows_before) / (double)(frames_run - frames_before);
    snprintf(what, sizeof what, "idle: %.1f rows copied a picture (of %d) - unchanged rows are skipped", avg, H);
    check(avg < 60, what);

    stop_core();
    printf("%s (%ld pictures)\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED", frames_run);
    free(screen);
    screen = NULL;
    return failures ? 1 : 0;
}

/* ---------------- running on the console ---------------- */
int main(int argc, char **argv)
{
    main_pid = getpid();
    clock_gettime(CLOCK_MONOTONIC, &started);
    setvbuf(stdout, NULL, _IOLBF, 0);                 /* each log line out at once */
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest(argc > 2 ? argv[2] : "/tmp/nzeus-modern-selftest.ppm");
    char *end = NULL;
    long seconds = argc > 1 ? strtol(argv[1], &end, 10) : 0;
    if (argc > 2 || (argc > 1 && (*end || seconds < 0))) {
        fputs("usage: nzeus-modern [SECONDS] | --selftest OUT.ppm\n", stderr);
        return 1;
    }
    if (prepare_data() != 0) return 5;
    remove_old_card_link();
    open_buttons();

    /* (before the screen is switched on, so a stop at any moment gives it back) */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop;                             /* (no SA_RESTART: a sleep ends at once) */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGSEGV, crashed); signal(SIGBUS, crashed); signal(SIGILL, crashed);
    signal(SIGFPE, crashed); signal(SIGABRT, crashed);
    atexit(restore_screen);                           /* (if anything calls exit()) */
    if (open_screen() != 0) { close_buttons(); return 3; }

    hw_fullscreen_run = run_fullscreen;               /* (the Web app's NetSurf) */
    printf("NZEUS_MODERN_ACTIVE limit=%ld s, %d button devices\n", seconds, dev_count);
    start_core();
    if (fb16 && access(NZ_DIR "/no-direct", F_OK) != 0) {   /* draw straight into the 16-bit screen (debug opt-out) */
        nzeus_display_target((uint16_t *)screen);
        direct = 1;
        puts("SCREEN direct drawing");
    }
    struct timespec due;
    clock_gettime(CLOCK_MONOTONIC, &due);
    long next_alive = 60;
    while (running) {
        long s = ms_since_start() / 1000;
        if (seconds && s >= seconds) break;
        if (s >= next_alive) {
            printf("ALIVE t=%ld frames=%ld rows=%ld presses=%u\n", s, frames_run, rows_copied, presses);
            next_alive += 60;
        }
        retro_run();
        frames_run++;
        next_frame(&due);
    }
    stop_core();
    long s = ms_since_start() / 1000;
    printf("NZEUS_MODERN_END t=%ld frames=%ld rows=%ld (%.1f a picture) presses=%u restart=%d\n", s, frames_run,
           rows_copied, frames_run ? (double)rows_copied / frames_run : 0.0, presses, nzeus_restart_wanted);
    close_buttons();
    restore_screen();

    if (nzeus_restart_wanted && (!seconds || s < seconds)) {   /* Settings > Restart NZEUS: start again */
        char left[24];
        snprintf(left, sizeof left, "%ld", seconds ? seconds - s : 0L);
        char *again[] = { argv[0], left, NULL };
        puts("NZEUS_MODERN_RESTART");
        fflush(stdout);
        execv("/proc/self/exe", again);
        perror("nzeus-modern: restart");
    }
    return 0;
}

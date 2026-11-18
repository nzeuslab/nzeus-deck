/* core.c - NZEUS Home as a libretro "core".
 *
 * nzeus-modern (apps/frontend-modern/nzeus-modern.c, the SD Debian on Linux
 * 7.2) links this file straight in and calls the retro_* functions below,
 * the standard libretro way:
 *   - every frame we hand it our finished 640x480 picture (nzeus_fb)
 *   - the buttons: the frontend reads Linux input events and answers
 *     input_state (one libretro joypad button per NZEUS button)
 *   - sound: audio_batch (the frontend writes it to the ALSA card)
 * "Restart NZEUS" closes the core; the frontend then starts it again. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "libretro.h"
#include "lvgl.h"
#include "display.h"
#include "home.h"
#include "nzeus_tick.h"
#include "ui.h"
#include "usbkbd.h"
#include "usbmouse.h"
#include "hw.h"
#include "settings.h"
#include "audio.h"
#include "shot.h"
#include "theme.h"

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_state_t input_state_cb;
int nzeus_restart_wanted;               /* "Restart NZEUS" was asked for: the frontend starts itself again */

/* --- buttons --- */
/* The libretro button for each of our buttons (RETRO_DEVICE_ID_JOYPAD_*). */
static const unsigned joypad_ids[NZ_KEY_COUNT] = {
    [NZ_KEY_UP] = RETRO_DEVICE_ID_JOYPAD_UP, [NZ_KEY_DOWN] = RETRO_DEVICE_ID_JOYPAD_DOWN,
    [NZ_KEY_LEFT] = RETRO_DEVICE_ID_JOYPAD_LEFT, [NZ_KEY_RIGHT] = RETRO_DEVICE_ID_JOYPAD_RIGHT,
    [NZ_KEY_A] = RETRO_DEVICE_ID_JOYPAD_A, [NZ_KEY_B] = RETRO_DEVICE_ID_JOYPAD_B,
    [NZ_KEY_X] = RETRO_DEVICE_ID_JOYPAD_X, [NZ_KEY_Y] = RETRO_DEVICE_ID_JOYPAD_Y,
    [NZ_KEY_L] = RETRO_DEVICE_ID_JOYPAD_L, [NZ_KEY_R] = RETRO_DEVICE_ID_JOYPAD_R,
    [NZ_KEY_START] = RETRO_DEVICE_ID_JOYPAD_START, [NZ_KEY_SELECT] = RETRO_DEVICE_ID_JOYPAD_SELECT,
};
static uint32_t prev_keys;              /* our buttons in the previous frame */
static uint32_t held_since[NZ_KEY_COUNT];
static int quit_requested;

/* Our buttons right now, one bit per NZ_KEY_ value (asked from the frontend). */
static uint32_t keys_now(void)
{
    uint32_t out = 0;
    if (!input_state_cb) return 0;
    for (int i = 0; i < NZ_KEY_COUNT; i++)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, joypad_ids[i])) out |= 1u << i;
    return out;
}

/* Send new presses to the UI. A held D-pad direction repeats (after 400 ms,
 * then every 120 ms, or the speed picked in Settings) so moving across the
 * grid is quick. The UI also gets the held buttons every frame (in desktop
 * mode the D-pad moves the pointer).
 *
 * Screenshot: hold SELECT and press L1 - in every app, with the keyboard
 * open, in window mode. That is why SELECT (handheld / desktop) acts when
 * you LET GO of it, and only if L1 was not pressed meanwhile. */
static int select_used;                 /* L1 was pressed while SELECT was held */

static void handle_keys(void)
{
    uint32_t now = keys_now(), t = nzeus_tick_ms();
    uint32_t rate = (uint32_t)settings_get_int("repeat_ms", 120);
    uint32_t sel = 1u << NZ_KEY_SELECT, l1 = 1u << NZ_KEY_L;
    int shot = (now & sel) && (now & l1) && !(prev_keys & l1);   /* L1 went down while SELECT is held */
    if ((now & sel) && !(prev_keys & sel)) select_used = 0;        /* a fresh SELECT press */
    if (shot) { select_used = 1; ui_screenshot_now(); }
    for (int i = 0; i < NZ_KEY_COUNT; i++) {
        uint32_t bit = 1u << i;
        int down = (now & bit) != 0, was = (prev_keys & bit) != 0;
        if (down && !was && (i == NZ_KEY_SELECT || (i == NZ_KEY_L && shot))) held_since[i] = t;   /* (see above) */
        else if (down && !was) { held_since[i] = t; ui_key(i); }
        else if (!down && was && i == NZ_KEY_SELECT) { if (!select_used) ui_key(i); }    /* SELECT let go */
        else if (down && i <= NZ_KEY_RIGHT && t - held_since[i] > 400) {
            held_since[i] = t - 400 + rate;          /* next repeat in `rate` ms */
            ui_key(i);
        }
    }
    ui_buttons(now);
    prev_keys = now;
}

/* A USB mouse: any use of it (or plugging it in) switches to desktop mode. */
static void handle_mouse(void)
{
    static int was_connected;
    UsbMouse m;
    usbmouse_poll(&m);
    if (m.connected) {
        int activity = m.moved || m.left_click || m.right_click || m.wheel || !was_connected;
        ui_mouse(m.x, m.y, m.left_down, m.left_click, activity);
        if (m.right_click) ui_mouse_right_click();   /* right button: e.g. the desktop menu */
        if (m.wheel) ui_mouse_wheel(m.wheel);        /* wheel (+ = up): scrolls the list under the pointer */
    }
    was_connected = m.connected;
}

/* A USB keyboard: every key goes to the app in front (e.g. the Terminal). */
static void handle_keyboard(void)
{
    KbEvent ev;
    while (usbkbd_next(&ev)) ui_kbd(&ev);
}

/* --- status bar: clock and Wi-Fi --- */
static int wifi_is_up(void)
{
    FILE *f = fopen("/sys/class/net/wlan0/flags", "r");
    unsigned flags = 0;
    if (!f) return 0;
    if (fscanf(f, "%x", &flags) != 1) flags = 0;
    fclose(f);
    return flags & 1;                                /* IFF_UP */
}

/* --- the battery: the % on the status bar, a low warning, a safe power-off ---
 * Read every 10 s. The last 6 readings (one minute) are averaged, so the
 * number doesn't jump around and one odd reading changes nothing. */
#define BATT_READINGS   6
#define BATT_LOW        15      /* warn at this % or lower (not while the charger is in) */
#define BATT_WARN_EVERY 300     /* seconds between two warnings */
#define BATT_EMPTY_RAW  130     /* switch off when the minute's average reading is this or lower:
                                   this console died at 128, even with the charger plugged in */
int battery_off_delay_ms = 10000;    /* how long "Battery empty" shows before switching off (tests: shorter) */

static struct {
    time_t last_read, last_warn;
    int warned;                      /* the low warning was shown (then again every 5 minutes) */
    int pct[BATT_READINGS], raw[BATT_READINGS];   /* the last readings: % and the raw byte */
    int n_pct, n_raw, next_pct, next_raw;
    int shown;                       /* the % shown (-1 = unknown) */
    uint32_t off_at;                 /* switch off at this time (ms); 0 = not */
    lv_obj_t *off_screen;            /* the full-screen "Battery empty" message */
} B = { .shown = -1 };

static int average(const int *v, int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    return n ? sum / n : -1;
}

/* forget the readings (tests, and after a power-off that did not happen) */
void battery_reset(void)
{
    if (B.off_screen) lv_obj_del(B.off_screen);
    memset(&B, 0, sizeof B);
    B.shown = -1;
}
lv_obj_t *battery_off_screen(void) { return B.off_screen; }   /* (tests) */

/* Empty: a message over everything, and a safe power-off a few seconds later */
static void battery_empty(void)
{
    B.off_screen = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(B.off_screen);
    th_add(B.off_screen, TS_SCREEN);
    lv_obj_set_size(B.off_screen, NZ_W, NZ_H);
    lv_obj_t *big = th_label(B.off_screen, "Battery empty", TS_BIG);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -40);
    lv_obj_t *small = th_label(B.off_screen, "NZEUS Deck switches off safely in 10 seconds.\n"
                                             "Charge the console before you switch it on again.", TS_TEXT);
    lv_obj_set_style_text_align(small, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(small, LV_ALIGN_CENTER, 0, 30);
    B.off_at = nzeus_tick_ms() + (uint32_t)battery_off_delay_ms;
    if (!B.off_at) B.off_at = 1;                     /* (0 means "not switching off") */
}

/* every frame: is it time to switch off? */
static void battery_off_check(void)
{
    if (!B.off_at || (int32_t)(nzeus_tick_ms() - B.off_at) < 0) return;
    B.off_at = 0;
    if (hw_power_off() == 0) return;                 /* switching off (sync, card read-only, power cut) */
    battery_reset();                                 /* it did not happen (on the PC tests it never does): */
    ui_toast("Could not switch off - plug in the charger now!");   /* tell, try again after a minute */
}

/* The battery % shown (-1 = unknown); also warns and switches off */
static int battery_now(time_t now)
{
    if (B.last_read && now - B.last_read < 10) return B.shown;
    B.last_read = now;
    int charging = hw_battery_charging();
    int raw = hw_battery_raw();
    if (raw >= 0) {
        B.raw[B.next_raw] = raw;
        B.next_raw = (B.next_raw + 1) % BATT_READINGS;
        if (B.n_raw < BATT_READINGS) B.n_raw++;
        /* a whole minute at "empty" or below, charger or not: switch off before the battery dies */
        if (B.n_raw == BATT_READINGS && average(B.raw, B.n_raw) <= BATT_EMPTY_RAW && !B.off_screen) battery_empty();
    }
    int p = hw_battery_percent();
    if (p < 0) return B.shown;
    B.pct[B.next_pct] = p;
    B.next_pct = (B.next_pct + 1) % BATT_READINGS;
    if (B.n_pct < BATT_READINGS) B.n_pct++;
    B.shown = average(B.pct, B.n_pct);
    ui_set_charging(charging);
    /* low and not charging: a warning, again every 5 minutes */
    if (B.shown <= BATT_LOW && !charging && (!B.warned || now - B.last_warn >= BATT_WARN_EVERY)) {
        char msg[64];
        snprintf(msg, sizeof msg, "Battery low (%d%%) - plug in the charger", B.shown);
        ui_toast(msg);
        B.warned = 1;
        B.last_warn = now;
    }
    return B.shown;
}

static void update_status(void)
{
    static time_t last;
    time_t now = hw_now();
    if (now == last) return;                         /* once per second is enough */
    last = now;
    char clock[8];
    strftime(clock, sizeof clock, "%H:%M", localtime(&now));
    ui_set_status(clock, battery_now(now), wifi_is_up());
}

/* --- leaving NZEUS --- */
/* "Restart NZEUS" closes the core; the frontend sees nzeus_restart_wanted
 * and starts itself again. */
static int open_app(int app)
{
    if (app != OUTSIDE_RESTART) return 0;
    nzeus_restart_wanted = 1;
    quit_requested = 1;
    return 1;
}

/* --- libretro interface --- */
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }   /* (our buttons) */

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "NZEUS Deck";
    info->library_version = "0.1";
    info->valid_extensions = "";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof *info);
    info->geometry.base_width = info->geometry.max_width = NZ_W;
    info->geometry.base_height = info->geometry.max_height = NZ_H;
    info->geometry.aspect_ratio = (float)NZ_W / NZ_H;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
}

void retro_init(void)
{
    quit_requested = 0;
    prev_keys = 0;
    settings_reload();
    hw_clock_restore();                              /* no clock battery: go on from the saved time */
    if (!settings_get("tz", NULL)) {                 /* the SD Debian has a zone (set when it was built): start from it */
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        settings_set_int("tz", (int)(lt.tm_gmtoff / 3600));
    }
    hw_apply_timezone(settings_get_int("tz", 0));    /* UTC until set in Settings > Date & time */
    if (settings_get("brightness", NULL)) hw_set_brightness(settings_get_int("brightness", 80));
    if (settings_get("volume", NULL)) hw_set_volume(settings_get_int("volume", 70));
    if (settings_get_int("speaker", 1) == 0) hw_set_speaker(0);   /* off for headphones, as left last time */
    lv_init();
    nzeus_display_init();
    ui_init();
    ui_set_open_fn(open_app);
    usbmouse_init(NZ_W, NZ_H);
    usbkbd_init();
    update_status();
}

void retro_deinit(void)
{
    hw_clock_save();                                 /* NZEUS closes: remember the time */
    usbkbd_close();
    usbmouse_close();
}

bool retro_load_game(const struct retro_game_info *info) { (void)info; return true; }
void retro_unload_game(void) {}

void retro_run(void)
{
    if (input_poll_cb) input_poll_cb();
    if (!quit_requested) { handle_keys(); handle_mouse(); handle_keyboard(); }
    update_status();
    battery_off_check();                             /* "Battery empty": switch off when it's time */
    hw_clock_keep(nzeus_tick_ms());                  /* saves the time once a minute */
    shot_poll();                                     /* nzshot / nzpress / nztype requests */
    ui_tick();                                        /* open apps do their work */
    if (audio_batch_cb && audio_loaded()) {           /* music: 44100 / 60 = 735 frames per picture */
        static int16_t sound[800 * 2];
        static size_t pending, offset;
        static unsigned revision;
        if (revision != audio_revision()) {
            pending = offset = 0;
            revision = audio_revision();
        }
        /* the ALSA card plays 48 kHz songs as they are (no conversion), everything else at 44.1 kHz */
        int rate = 0, channels = 0;
        audio_info(&rate, &channels);
        int next_rate = rate == 48000 ? 48000 : AUDIO_RATE;
        if (next_rate != audio_out_rate) pending = offset = 0;
        audio_out_rate = next_rate;
        int frames = audio_out_rate / 60;             /* 735 or 800 per picture */
        hw_pcm_set_rate(audio_out_rate);
        if (!pending) {
            audio_render(sound, frames);
            pending = (size_t)frames;
            offset = 0;
        }
        /* Keep unsent samples; decoding again would silently skip them. */
        size_t taken = audio_batch_cb(sound + offset * 2, pending);
        if (taken > pending) taken = pending;
        pending -= taken;
        offset += taken;
    } else if (audio_batch_cb) {                      /* nothing loaded (a song closed mid-play): silence, */
        static const int16_t quiet[735 * 2];          /* so hw_pcm_write lets go of the sound card */
        audio_batch_cb(quiet, 735);
    }
    lv_timer_handler();                               /* LVGL redraws what changed */
    if (video_cb) video_cb(nzeus_fb, NZ_W, NZ_H, NZ_W * sizeof(uint16_t));
    if (quit_requested && environ_cb) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

void retro_reset(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { (void)t; (void)i; (void)n; return false; }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }

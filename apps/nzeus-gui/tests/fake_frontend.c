/* fake_frontend.c - tests the NZEUS core on the PC by pretending to be its
 * frontend (apps/frontend-modern/nzeus-modern.c).
 *
 * Like nzeus-modern it calls the core's retro_* functions every frame and
 * answers input_state for the buttons, takes the pictures (video) and the
 * sound (audio_batch). The console's devices are fake files: hw_modern.c's
 * paths (battery, backlight, sound card, speaker switch, /proc/net/route) are
 * pointed at /tmp, and the helper scripts in nzeus/bin are small fake scripts.
 * It presses buttons frame by frame and checks what happens on screen and in
 * the files. Build with -DNZ_SDCARD=\"/tmp/fake_sdcard\" (NZEUS's folder).
 * Usage: fake_frontend OUTPUT_FOLDER PICTURES_FOLDER   (saves frames as .ppm) */
#define _GNU_SOURCE                    /* posix_openpt, ptsname (the fake serial port) */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <malloc.h>
#include <unistd.h>
#include <linux/input.h>              /* KEY_* (the USB keyboard driver's tests) */
#include "libretro.h"
#include "display.h"
#include "ui.h"
#include "desktop.h"
#include "apps.h"
#include "home.h"
#include "terminal.h"
#include "editor.h"

const char *app_terminal_input(void);    /* app_terminal.c: what is typed so far */
lv_obj_t *app_terminal_textbox(void);    /* app_terminal.c: the typed line's box */
int app_terminal_cursor(void);           /* ... where the text cursor is */
int app_terminal_selecting(void);        /* Select mode is on */
int app_terminal_kb_open(void);          /* the on-screen keyboard is up */
int app_terminal_layer(void);            /* 0 abc, 1 ABC, 2 symbols, 3 Edit */
int app_terminal_ctrl(void);             /* the sticky Ctrl is on */
const char *app_terminal_title(void);
int app_terminal_osk_select(const char *name);   /* put the keyboard's selection on a key */
int app_terminal_marking(void);          /* Mark is on (the typed line) */
int app_terminal_marked(int *lo, int *hi);   /* the typed line's marked letters */
const char *app_terminal_screen(void);   /* the output rows as drawn */
int app_terminal_sel(int *anc_line, int *anc_col, int *cur_line, int *cur_col);   /* Select mode */
int app_terminal_cell_xy(int line, int col, int *x, int *y);   /* where an output letter is on screen */
int app_terminal_row_line(int k);        /* the output line on screen row k */
#include "term_cmds.h"
#include "hw.h"
#include "nzeus_tick.h"
#include <time.h>
const char *app_files_cwd(void);         /* app_files.c: the folder shown */
const char *app_files_selected_name(void);
const char *app_editor_path(void);       /* app_editor.c: the file open in the Editor */
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "wifi.h"
#include "audio.h"
#include "obd.h"
#include "carsupport.h"
#include "carsum.h"
#include "serial.h"
#include "shot.h"
#include "picture.h"
#include "fsops.h"
#include "sha256.h"
#include "keyvault.h"
#include "vtwidget.h"
int app_settings_section(void);           /* app_settings.c */
const char *app_settings_row_value(int i);
int app_python_count(void);               /* app_python.c */
int app_images_open(const char *path);    /* app_images.c */
int app_images_shown_w(void);
int app_images_count(void);
const char *app_images_pdf_error(void);
int app_images_open_pdf(const char *path);
int app_images_pdf_page(void), app_images_pdf_pages(void), app_images_pdf_zoom(void), app_images_pdf_pan_y(void);
int app_wifi_network_count(void);         /* app_wifi.c */
lv_obj_t *app_wifi_spinner(void);         /* the spinner while joining (NULL = none) */
const char *app_wifi_detail(void);        /* the status card's second line */
extern int wifi_scan_wait_ms;
int app_claude_marker_shown(void);        /* app_claude.c: the SCROLLED BACK sign is up */
void battery_reset(void);                 /* core.c: the battery watch */
lv_obj_t *battery_off_screen(void);
extern int battery_off_delay_ms;
int app_media_song_count(void);           /* app_media.c */
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
int app_arduino_connected(void);          /* app_arduino.c */
lv_obj_t *app_claude_terminal(void);      /* app_claude.c */
int app_claude_chat_shown(void);          /* app_claude.c: the chat view (bubbles) is up */
lv_obj_t *app_claude_chat_list(void);     /* its scrolling list */
int app_claude_chat_count(void);          /* bubbles shown */
int app_claude_chat_mine(int i);          /* 1 = bubble i is what I typed */
lv_obj_t *app_claude_chat_label(int i);   /* bubble i's text label */
lv_obj_t *app_web_scroller(void);         /* app_web.c */
extern const char *claude_connect_script;
int app_web_open(const char *path);       /* app_web.c: NetSurf at an address */
const char *app_web_current(void);        /* the address NetSurf was last opened at */
int app_web_loading(void);                /* 1 = NetSurf is about to start */
const char *app_web_result(void);         /* "ok: ..." / "error: ..." (nzctl web) */
const char *app_web_message(void);        /* the line on the Web app's screen ("" = none) */
extern const char *web_netsurf_path, *web_launcher_path;
const char *app_arduino_last_line(void);
int app_arduino_uploading(void);
int app_arduino_firmware_count(void);
int app_probe_connected(void);            /* app_probe.c */
const char *app_probe_reading(void);
const char *app_probe_detail(void);
int app_settings_in_panel(void);
#include "bt.h"                           /* Settings > Bluetooth (app_settings.c + bt.c) */
int app_settings_bt_page(void);
const char *app_settings_bt_message(void);
const char *app_settings_bt_big(void);
const char *app_settings_bt_note(void);
int app_settings_bt_rows(void);
int app_settings_bt_row_greyed(int i);
#include "usbmode.h"                      /* Settings > USB (app_settings.c + usbmode.c) */
#include "nzlist.h"
#ifndef NZ_REPO
#define NZ_REPO "."                               /* the project folder (build_nzeus_gui.sh passes it) */
#endif                       /* WhatsApp (app_whatsapp.c): its chat list */
int app_whatsapp_state(void);             /* 0 no PIN, 1 locked, 2 making the key, 3 setup page, 4 ready */
int app_whatsapp_page(void);              /* 0 the chats, 1 a chat, 2 photos, 3 a photo, 4 pictures to send, 5 voice messages */
int app_whatsapp_busy(void);
int app_whatsapp_chat_count(void);
const char *app_whatsapp_chat_name(int i);
const char *app_whatsapp_chat_last(int i);
int app_whatsapp_chat_unread(int i);
int app_whatsapp_msg_count(void);
const char *app_whatsapp_msg_text(int i);
const char *app_whatsapp_msg_author(int i);
int app_whatsapp_msg_mine(int i);
const char *app_whatsapp_status(void);
lv_obj_t *app_whatsapp_scroller(void);
NzList *app_whatsapp_list(void);
int app_whatsapp_msg_kind(int i);         /* 0 text, 1 photo, 2 video, 3 voice ... */
const char *app_whatsapp_msg_id(int i);
int app_whatsapp_photo_count(void);       /* rows of "Photos in this chat" */
int app_whatsapp_photo_w(void);           /* the photo on screen (0 = none / freed) */
int app_whatsapp_pick_count(void);        /* rows of "Send a picture" */
int app_whatsapp_voice_count(void);       /* rows of "Voice messages" (page 5) */
int app_whatsapp_voice_playing(void);     /* a voice message is in the music engine */
NzList *app_whatsapp_sub_list(void);
lv_obj_t *app_whatsapp_chat_title(void);
lv_obj_t *app_whatsapp_view_img(void);
#include "emoji.h"                        /* emoji pictures (emoji.c) */
void wa_base64(const char *in, char *out, size_t n);
void wa_unescape(char *s);
int whatsapp_relock(const char *old_pin, const char *new_pin);
extern int wa_reload_chat_ms, wa_reload_list_ms;

/* Does the file begin with `text`? */
static int file_starts_with(const char *path, const char *text)
{
    char buf[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return !strncmp(buf, text, strlen(text));
}

/* RGB565 colour checks */
static int is_red(uint16_t p) { return ((p >> 11) & 31) > 20 && ((p >> 5) & 63) < 16 && (p & 31) < 8; }
static int is_blue(uint16_t p) { return ((p >> 11) & 31) < 8 && ((p >> 5) & 63) < 16 && (p & 31) > 20; }

/* Does the file contain `text` somewhere? */
static int file_contains(const char *path, const char *text)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, text) != NULL;
}

/* A small file's bytes (exactly, binary); returns how many, -1 = no file */
static long file_read(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long got = (long)fread(buf, 1, n, f);
    fclose(f);
    return got;
}

/* How many lines of a file start with `prefix` (the fake WhatsApp script's log) */
static int lines_starting(const char *path, const char *prefix)
{
    char line[1024];
    int n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, prefix, strlen(prefix))) n++;
    fclose(f);
    return n;
}

/* Arabic on a label: letters left unjoined (0x621-0x64A), and joined ones (0xFE70+) */
static int arabic_count(const char *t, int joined)
{
    uint32_t k = 0, c;
    int n = 0;
    while ((c = _lv_txt_encoded_next(t, &k)) != 0) {
        if (!joined && c >= 0x621 && c <= 0x64A) n++;
        if (joined && c >= 0xFE70 && c <= 0xFEFC) n++;
    }
    return n;
}

/* Terminal "noise" in a text: box drawing / block characters, Claude Code's
 * reply signs (round dots, the corner, the prompt sign) or an escape code */
static int has_tui_noise(const char *t)
{
    uint32_t k = 0, c;
    while ((c = _lv_txt_encoded_next(t, &k)) != 0)
        if ((c >= 0x2500 && c <= 0x259F) || c == 0x23FA || c == 0x25CF || c == 0x23BF || c == 0x276F || c == 0x1B) return 1;
    return 0;
}

/* Is there an output line that is exactly `text`? */
static int terminal_has_line(const char *text)
{
    for (int i = 0; i < terminal_line_count(); i++) if (!strcmp(terminal_line(i), text)) return 1;
    return 0;
}

/* How many lines a text file has, and line number n (0 = first) in `out` */
static int file_lines(const char *path, int n, char *out, size_t size)
{
    char line[512];
    int count = 0;
    if (out) out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (count == n && out && size > 0) {          /* copy, cut to fit */
            size_t k = strlen(line);
            if (k >= (size_t)size) k = (size_t)size - 1;
            memcpy(out, line, k);
            out[k] = 0;
        }
        count++;
    }
    fclose(f);
    return count;
}

/* How many .png files a folder has */
static int count_pngs(const char *dir)
{
    char cmd[300], buf[32] = "";
    snprintf(cmd, sizeof cmd, "ls %s 2>/dev/null | grep -c '\\.png$'", dir);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (!fgets(buf, sizeof buf, p)) buf[0] = 0;
    pclose(p);
    return atoi(buf);
}

/* Type a line on a (simulated) USB keyboard, then Enter. */
static void type_line(const char *s)
{
    for (; *s; s++) { KbEvent ev = { KB_CHAR, *s }; ui_kbd(&ev); }
    KbEvent enter = { KB_ENTER, 0 };
    ui_kbd(&enter);
}

/* the core's functions (core.c) */
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_init(void);
void retro_run(void);
bool retro_load_game(const struct retro_game_info *);

/* The buttons held right now: one bit per button, numbered like the old
 * stock button word (the tests below press them by these numbers):
 *   0 SELECT  1 START  2 UP  3 DOWN  5 RIGHT  7 LEFT  10 L1  11 R1  12 X  13 A  14 B  15 Y
 * input_state() answers the core's questions from it, the libretro way, the
 * same as nzeus-modern.c does from the console's input events. */
static uint32_t held;
static uint32_t *keys = &held;
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    int bit = -1;
    if (port || device != RETRO_DEVICE_JOYPAD || index) return 0;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_SELECT: bit = 0; break;
    case RETRO_DEVICE_ID_JOYPAD_START:  bit = 1; break;
    case RETRO_DEVICE_ID_JOYPAD_UP:     bit = 2; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   bit = 3; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  bit = 5; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   bit = 7; break;
    case RETRO_DEVICE_ID_JOYPAD_L:      bit = 10; break;
    case RETRO_DEVICE_ID_JOYPAD_R:      bit = 11; break;
    case RETRO_DEVICE_ID_JOYPAD_X:      bit = 12; break;
    case RETRO_DEVICE_ID_JOYPAD_A:      bit = 13; break;
    case RETRO_DEVICE_ID_JOYPAD_B:      bit = 14; break;
    case RETRO_DEVICE_ID_JOYPAD_Y:      bit = 15; break;
    }
    return bit >= 0 ? (int16_t)((held >> bit) & 1) : 0;
}
void retro_set_input_state(retro_input_state_t);
static uint16_t frame[NZ_W * NZ_H];    /* last picture the core gave us */
static int shutdown_asked, rgb565_asked, failures;
static const char *outdir = ".";

static bool env(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_SHUTDOWN) shutdown_asked = 1;
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT && *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565)
        rgb565_asked = 1;
    return true;
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    for (unsigned y = 0; y < h; y++) memcpy(&frame[y * NZ_W], (const char *)data + y * pitch, w * 2);
}

/* sound the core hands to the frontend (nzeus-modern writes it to the ALSA card) */
static long sound_frames;
static int sound_peak;
static int sound_partial, sound_suffix_ok;
static int16_t sound_saved[800 * 2];
static size_t audio_batch(const int16_t *data, size_t frames)
{
    if (sound_partial) {
        if (sound_partial == 1) {
            memcpy(sound_saved, data, frames * 2 * sizeof *data);
            sound_partial = 2;
            return 100;
        }
        sound_suffix_ok &= frames == 635 && !memcmp(data, sound_saved + 200, frames * 2 * sizeof *data);
        if (sound_partial++ == 2) return 0;       /* a busy device on the next frame */
        sound_partial = 0;
    }
    sound_frames += (long)frames;
    for (size_t i = 0; i < frames * 2; i++) { int v = data[i] < 0 ? -data[i] : data[i]; if (v > sound_peak) sound_peak = v; }
    return frames;
}

/* The frontend's "run another program full screen" (nzeus-modern.c's
 * run_fullscreen, used by the Web app for NetSurf): here it just runs the
 * program and waits for it, and counts how often it was asked. */
static int fullscreen_runs;
static int fake_fullscreen(const char *const argv[])
{
    fullscreen_runs++;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return rc == 127 ? -1 : rc;
}

/* a 440 Hz tone, mono 16-bit WAV: `secs` seconds at `rate` (a rate the player must convert) */
static void make_tone_at(const char *path, int rate, int secs)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int n = rate * secs;
    unsigned char h[44] = "RIFF....WAVEfmt ";
    uint32_t v;
    v = 36 + n * 2; memcpy(h + 4, &v, 4);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);          /* PCM */
    s = 1; memcpy(h + 22, &s, 2);                   /* mono */
    v = (uint32_t)rate; memcpy(h + 24, &v, 4);
    v = (uint32_t)rate * 2; memcpy(h + 28, &v, 4);
    s = 2; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    v = (uint32_t)n * 2; memcpy(h + 40, &v, 4);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < n; i++) {                   /* a triangle wave is close enough to a tone */
        int ph = (i * 440 * 4 / rate) % 4, fr = (i * 440 * 4) % rate;
        int x = ph == 0 ? fr : ph == 1 ? rate - fr : ph == 2 ? -fr : fr - rate;
        int16_t sample = (int16_t)(x * 12000 / rate);
        fwrite(&sample, 2, 1, f);
    }
    fclose(f);
}

/* the Media tests' tone: 2 seconds at 22050 Hz */
static void make_tone(const char *path) { make_tone_at(path, 22050, 2); }


/* heap memory in use right now (LVGL uses malloc here, so this counts the GUI too) */
static long heap_used(void) { struct mallinfo2 m = mallinfo2(); return (long)m.uordblks; }

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* the newest output line that is exactly `text`, or -1 */
static int line_index(const char *text)
{
    for (int i = terminal_line_count() - 1; i >= 0; i--) if (!strcmp(terminal_line(i), text)) return i;
    return -1;
}

/* is this screen pixel the theme's accent colour? */
static int is_accent_at(int x, int y)
{
    return x >= 0 && y >= 0 && x < NZ_W && y < NZ_H && frame[y * NZ_W + x] == th_color(theme()->accent).full;
}

/* the first number in nzeus/clock.txt (-1 = no file) */
#define CLOCK_TXT "/tmp/fake_sdcard/nzeus/clock.txt"
static long long clock_file(void)
{
    long long v = -1;
    FILE *f = fopen(CLOCK_TXT, "r");
    if (!f) return -1;
    if (fscanf(f, "%lld", &v) != 1) v = -2;
    fclose(f);
    return v;
}

/* something that runs every frame next to the core (the fake OBD adapter) */
static void (*frame_hook)(void);

/* run n frames at ~60 per second (real time, because key repeat uses time) */
static void frames(int n) { while (n--) { retro_run(); if (frame_hook) frame_hook(); usleep(16000); } }
/* frames without waiting (for the long stability test) */
static void fast_frames(int n) { while (n--) { retro_run(); if (frame_hook) frame_hook(); } }
/* the power driver's levels file (fake): the battery and charger readings */
#define FAKE_LEVELS "/tmp/fake_power/stock_levels"
static void write_levels(int raw, int charger)
{
    FILE *f = fopen(FAKE_LEVELS, "w");
    if (f) { fprintf(f, "ref=247 battery=%d charger=%d\n", raw, charger); fclose(f); }
}
/* One reading for core.c's battery watch: the fake battery and charger levels,
 * then 10 s later on the pretend clock (it reads every 10 s), one frame. */
static time_t batt_clock;
static void battery_reading(int raw, int charger)
{
    write_levels(raw, charger);
    batt_clock += 10;
    hw_test_set_now(batt_clock);
    frames(1);
}

/* run frames until the Terminal shows this line (max ~3 s); 1 = it came */
static int wait_for_line(const char *text)
{
    for (int i = 0; i < 200; i++) { if (terminal_has_line(text)) return 1; frames(1); }
    return terminal_has_line(text);
}

/* ---- the Car app's parts (the Car tests use them) ---- */
#include "carscan.h"
#include "carlog.h"
#include "fordscan.h"
int app_car_page(void);                   /* app_car.c: 0 gauges 1 list 2 graph 3 scanning 4 report */
const CarScan *app_car_scan(void);
int app_car_graph_points(void);
int app_car_scroll_y(void);
const char *app_car_message(void);        /* the "No OBD adapter" box's text while shown */
const char *carreport_last_text(void);    /* carreport.c (NZ_TESTING): every text line the last PDF drew */

/* the Car app with no adapter: no demo, nothing made up, a clear message (1 = all true) */
static int car_no_demo(void)
{
    const ObdLive *L = obd_live();
    int values = 0;
    for (int i = 0; i < PID_COUNT; i++) values += L->have[i];
    return obd_state() == OBD_OFF && values == 0 &&
           !strcmp(app_car_message(), "No OBD adapter - plug the ELM327 into the USB port (Settings > USB > Devices)") &&
           !strcmp(obd_describe(), app_car_message());
}

/* a word anywhere in a scan's texts (English and Arabic), for the "no Ford words" checks */
static int scan_mentions(const CarScan *S, const char *word)
{
    if (strstr(S->make_model_en, word) || strstr(S->make_model_ar, word) || strstr(S->headline_en, word) ||
        strstr(S->headline_ar, word) || strstr(S->summary_en, word) || strstr(S->summary_ar, word) ||
        strstr(S->cause_en, word) || strstr(S->cause_ar, word) || strstr(S->adapter, word)) return 1;
    for (int i = 0; i < S->step_count; i++) if (strstr(S->steps_en[i], word) || strstr(S->steps_ar[i], word)) return 1;
    for (int i = 0; i < S->note_count; i++) if (strstr(S->notes_en[i], word) || strstr(S->notes_ar[i], word)) return 1;
    for (int i = 0; i < S->module_count; i++)
        if (strstr(S->modules[i].code, word) || strstr(S->modules[i].name_en, word) || strstr(S->modules[i].name_ar, word)) return 1;
    for (int i = 0; i < S->dtc_count; i++)
        if (strstr(S->dtcs[i].meaning_en, word) || strstr(S->dtcs[i].meaning_ar, word) ||
            strstr(S->dtcs[i].group_en, word) || strstr(S->dtcs[i].group_ar, word)) return 1;
    return S->wish_count > 0 && strstr(word, "Ford") != NULL;
}

/* ---- a fake serial device: the "board" end of a pseudo-terminal ---- */
static int pty_master = -1;
static char elm_line[64];
static int elm_len, elm_got_clear, elm_questions;
/* the pretend car: a 2025 Ranger Raptor (CAR_RAPTOR), a 2019 Toyota (CAR_TOYOTA) or a
 * 2016 Chevrolet truck (CAR_GM: like my field scan - UDS answers only at 7E0,
 * 7E2 and 7F1 (7F1 can't list codes), plus GM-language (GMLAN) modules at 0x241/0x244) */
enum { CAR_RAPTOR, CAR_TOYOTA, CAR_GM };
static int elm_fc_mode, elm_fc_header = -1, elm_fc_data, elm_fc_refuse;   /* flow control (ATFCSM/SH/SD) */
static int elm_gm_queries, elm_gm_wrong;                 /* "1A" questions; ones NOT to a GM address 0x241-0x24F */
static int elm_gm_long_ok;                                /* GM VIN (long answer) sent with the right flow control */
static int elm_resets;                                    /* ATZ count (an adapter reset) */
static int elm_car = CAR_RAPTOR;
static int elm_header = 0x7DF, elm_cra = -1, elm_ms;     /* who we talk to, who we listen to, MS-CAN bus */
static int elm_h1, elm_st = 0x32;                         /* headers on (ATH1), answer timeout (ATST, x 4 ms) */
static int elm_engine_codes = 1;                          /* the Raptor's engine computer has its 4 codes */
static int elm_forbidden, elm_saw_stp53, elm_odo_disagree;
static int elm_identity_queries;
static char elm_forbidden_last[32];
static unsigned char elm_probed[0x800];                   /* 1 = "1902AF" was sent with this header */

static void elm_say(const char *s) { if (write(pty_master, s, strlen(s)) < 0) {} }

/* run frames until the console has written `want` to the fake port (1), or max frames pass (0) */
static int pty_heard(const char *want, int max)
{
    static char acc[2048];
    size_t k = 0;
    acc[0] = 0;
    for (int f = 0; f < max; f++) {
        frames(1);
        int n = (int)read(pty_master, acc + k, sizeof acc - 1 - k);
        if (n > 0) { k += (size_t)n; acc[k] = 0; }
        if (strstr(acc, want)) return 1;
        if (k > sizeof acc - 64) k = 0;              /* (full of other lines: start again) */
    }
    return 0;
}

/* answer bytes like an ELM327 with headers off and CAN auto-formatting on:
 * up to 7 bytes on one line, longer = "LLL" (length) + numbered 8-byte pieces */
static void elm_bytes(const unsigned char *b, int n)
{
    char out[2048];
    int k = 0;
    if (n <= 7) {
        for (int i = 0; i < n; i++) k += sprintf(out + k, "%02X", b[i]);
        k += sprintf(out + k, "\r");
    } else {
        k += sprintf(out + k, "%03X\r", n);
        int i = 0, seq = 0;
        while (i < n) {
            int room = seq == 0 ? 6 : 7;                     /* the first piece carries the length too */
            k += sprintf(out + k, "%X:", seq & 15);
            for (int j = 0; j < room; j++, i++) k += sprintf(out + k, "%02X", i < n ? b[i] : 0xAA);
            k += sprintf(out + k, "\r");
            seq++;
        }
    }
    sprintf(out + k, "\r>");
    elm_say(out);
}

/* a mode 09 text answer: 49 <pid> 01 + text (padded with zeros to `len`) */
static void elm_text(int pid, const char *text, int text_len, int len)
{
    unsigned char b[64] = { 0x49, (unsigned char)pid, 0x01 };
    for (int i = 0; i < len; i++) b[3 + i] = i < text_len ? (unsigned char)text[i] : 0;
    elm_bytes(b, 3 + len);
}

/* the modules that answer UDS 19 02 (Raptor: the 28 in his report), their fault
 * codes (2 bytes, failure type, status 0x28 = stored, not failing now),
 * odometers, and neg = 1: it answers "7F 19 11" (can't list its codes) */
typedef struct { int id; const char *dtcs; int odo; int neg; } FakeMod;
static const FakeMod raptor_mods[] = {
    { 0x7E0, "", 0, 0 }, { 0x760, "", 0, 0 }, { 0x730, "", 0, 0 }, { 0x721, "", 764, 0 }, { 0x703, "", 764, 0 }, { 0x792, "", 0, 0 },
    { 0x732, "", 0, 0 }, { 0x764, "", 0, 0 }, { 0x706, "", 764, 0 }, { 0x7C4, "", 0, 0 }, { 0x7C6, "", 0, 0 }, { 0x737, "", 0, 0 },
    { 0x726, "90F3002890F40028944400289448 0028", 0, 0 }, { 0x6F0, "", 0, 0 }, { 0x734, "C1220028", 0, 0 },
    { 0x740, "9163002891650028", 0, 0 }, { 0x741, "9164002891660028", 0, 0 }, { 0x733, "C4010028", 0, 0 }, { 0x720, "", 0, 0 },
    { 0x724, "", 0, 0 }, { 0x7A7, "", 0, 0 }, { 0x7D0, "", 0, 0 }, { 0x727, "950C8728", 0, 0 }, { 0x783, "", 0, 0 },
    { 0x754, "", 0, 0 }, { 0x751, "", 0, 0 }, { 0x725, "", 0, 0 }, { 0x716, "", 0, 0 },
};
/* the Toyota: engine (P0442 failing NOW: status 2F), gearbox (no UDS codes), a module at
 * 0x7B0 with C0035 failing now (09) and Ford's own B1318 as an old code (28), and one at
 * 0x7C0 that only says "can't do that" */
static const FakeMod toyota_mods[] = {
    { 0x7E0, "0442002F", 0, 0 }, { 0x7E1, "", 0, 0 }, { 0x7B0, "4035000993181128", 0, 0 }, { 0x7C0, "", 0, 1 },
};

/* the Chevrolet's UDS answers, as in my field scan: engine 7E0 and 7E2 list
 * (no) codes, 7F1 answers but not to UDS 19 */
static const FakeMod gm_mods[] = {
    { 0x7E0, "", 0, 0 }, { 0x7E2, "", 0, 0 }, { 0x7F1, "", 0, 1 },
};
#define GM_TEST_VIN "1GCUKREC5GF123456"           /* made up; G = 2016 */

static int hexbyte(const char *s) { unsigned v; return sscanf(s, "%2x", &v) == 1 ? (int)v : -1; }

/* a GM-language module (GMLAN): asked on 0x24x, answers on 0x64x (address + 0x400).
 * 0x241: its VIN (a long answer: only with flow control set to 0x241) and identifiers;
 * 0x244: no VIN (7F 1A 31 = "not supported"), only B0; 0x246 only ever says "wait"
 * (7F 1A 78) - that is not an answer. Everyone else: silent. */
static void fake_gm_module(const char *q)
{
    int id = elm_header;
    if (elm_ms || elm_cra != id + 0x400 || (id != 0x241 && id != 0x244 && id != 0x246)) { elm_say("NO DATA\r\r>"); return; }
    int did = hexbyte(q + 2);
    unsigned char b[64];
    int n = 0;
    if (id == 0x246) { elm_say("7F1A78\r\r>"); return; }
    if (id == 0x241 && did == 0x90) {
        /* 19 bytes = a long answer: the module waits for the "send the rest" reply */
        if (elm_fc_mode != 1 || elm_fc_header != 0x241 || !elm_fc_data) { elm_say("CAN ERROR\r\r>"); return; }
        elm_gm_long_ok = 1;
        b[n++] = 0x5A; b[n++] = 0x90;
        for (const char *p = GM_TEST_VIN; *p; p++) b[n++] = (unsigned char)*p;
    } else if (did == 0xB0) { b[n++] = 0x5A; b[n++] = 0xB0; b[n++] = id == 0x241 ? 0x40 : 0x44; }
    else if (id == 0x241 && did == 0xC1) { b[n++] = 0x5A; b[n++] = 0xC1; b[n++] = 0x01; b[n++] = 0x23; b[n++] = 0x45; b[n++] = 0x67; }
    else if (id == 0x241 && did == 0xCB) { b[n++] = 0x5A; b[n++] = 0xCB; b[n++] = 0x89; b[n++] = 0xAB; b[n++] = 0xCD; b[n++] = 0xEF; }
    else { b[n++] = 0x7F; b[n++] = 0x1A; b[n++] = 0x31; }
    elm_bytes(b, n);
}

/* one module's answer to a UDS question (header = its address, filter = address + 8) */
static void fake_module(const char *q)
{
    if (!strncmp(q, "1A", 2)) { fake_gm_module(q); return; }
    const FakeMod *mods = elm_car == CAR_TOYOTA ? toyota_mods : elm_car == CAR_GM ? gm_mods : raptor_mods;
    int n_mods = elm_car == CAR_TOYOTA ? (int)(sizeof toyota_mods / sizeof toyota_mods[0])
               : elm_car == CAR_GM ? (int)(sizeof gm_mods / sizeof gm_mods[0]) : (int)(sizeof raptor_mods / sizeof raptor_mods[0]);
    int m = -1;
    if (!strcmp(q, "1902AF") && elm_header >= 0 && elm_header < 0x800) elm_probed[elm_header] = 1;
    for (int i = 0; i < n_mods; i++) if (mods[i].id == elm_header) m = i;
    if (m < 0 || elm_ms || elm_cra != elm_header + 8) { elm_say("NO DATA\r\r>"); return; }   /* not there / not listening */
    unsigned char b[256];
    int n = 0;
    if (!strcmp(q, "22F187") || !strcmp(q, "22F188") || !strcmp(q, "22F189") || !strcmp(q, "22F191")) {
        /* Synthetic identifiers let the test prove the scanner asked the module it records. */
        elm_identity_queries++;
        b[n++] = 0x62; b[n++] = 0xF1; b[n++] = (unsigned char)hexbyte(q + 4);
        b[n++] = (unsigned char)(elm_header >> 8); b[n++] = (unsigned char)elm_header;
    } else if (!strcmp(q, "22F111")) {                              /* part number */
        char part[24];
        snprintf(part, sizeof part, "NB3T-%03X-AB", elm_header);
        b[n++] = 0x62; b[n++] = 0xF1; b[n++] = 0x11;
        for (const char *p = part; *p; p++) b[n++] = (unsigned char)*p;
    } else if (!strcmp(q, "1902AF") && mods[m].neg) {      /* "I can't list my codes" */
        b[n++] = 0x7F; b[n++] = 0x19; b[n++] = 0x11;
    } else if (!strcmp(q, "1902AF")) {                       /* fault codes */
        b[n++] = 0x59; b[n++] = 0x02; b[n++] = 0xFF;
        for (const char *p = mods[m].dtcs; *p; ) { if (*p == ' ') { p++; continue; } b[n++] = (unsigned char)hexbyte(p); p += 2; }
    } else if (!strcmp(q, "22DD01")) {                       /* odometer, km */
        int km = mods[m].odo;
        if (elm_odo_disagree && elm_header == 0x703) km = 801;     /* the variant: one module was "corrected" */
        if (km) { b[n++] = 0x62; b[n++] = 0xDD; b[n++] = 0x01; b[n++] = 0; b[n++] = (unsigned char)(km >> 8); b[n++] = (unsigned char)km; }
        else { b[n++] = 0x7F; b[n++] = 0x22; b[n++] = 0x31; }
    } else if (!strncmp(q, "3E", 2)) { b[n++] = 0x7E; b[n++] = 0x00; }
    else { b[n++] = 0x7F; b[n++] = (unsigned char)hexbyte(q); b[n++] = 0x11; }   /* "service not supported" */
    elm_bytes(b, n);
}

/* the Toyota's gearbox computer (7E1 -> 7E9): one stored code P0700, nothing else */
static void fake_gearbox(const char *q)
{
    if (!strcmp(q, "0101"))      elm_say("410101000000\r\r>");   /* light off, 1 code, no self-tests */
    else if (!strcmp(q, "03"))   elm_say("43010700\r\r>");       /* P0700 */
    else if (!strcmp(q, "07"))   elm_say("4700\r\r>");
    else if (!strcmp(q, "0A"))   elm_say("4A00\r\r>");
    else elm_say("NO DATA\r\r>");
}

/* the engine computer's OBD-II answers (asked as 7DF "everyone", or 7E0 on its own) */
static void fake_engine(const char *q)
{
    int toyota = elm_car == CAR_TOYOTA;
    if (!strcmp(q, "0100") && elm_h1)                        /* headers on: who answers? */
        elm_say(toyota ? "7E8064100BFBFA813\r7E906410080000001\r\r>" : "7E8064100BFBFA813\r\r>");
    else if (!strcmp(q, "0100"))  elm_say("SEARCHING...\r4100BFBFA813\r\r>");   /* PIDs 01-20 it has */
    else if (!strcmp(q, "0120"))  elm_say("41208002A001\r\r>");                  /* 21-40 */
    else if (!strcmp(q, "0140"))  elm_say("414044000010\r\r>");                  /* 41-60: no fuel rate (5E) */
    else if (!strcmp(q, "010C"))  elm_say("410C1AF8\r\r>");     /* (0x1A*256+0xF8)/4 = 1726 rpm */
    else if (!strcmp(q, "010D"))  elm_say("410D3C\r\r>");       /* 60 km/h */
    else if (!strcmp(q, "0105"))  elm_say("41055A\r\r>");       /* 90-40 = 50 C */
    else if (!strcmp(q, "0104"))  elm_say("410480\r\r>");
    else if (!strcmp(q, "0111"))  elm_say("411140\r\r>");
    else if (!strcmp(q, "012F"))  elm_say("412F80\r\r>");
    else if (!strcmp(q, "010F"))  elm_say("410F46\r\r>");       /* intake air 30 C */
    else if (!strcmp(q, "0110"))  elm_say("41100190\r\r>");     /* MAF 4.00 g/s */
    else if (!strcmp(q, "0106"))  elm_say("410680\r\r>");       /* fuel trims */
    else if (!strcmp(q, "0107"))  elm_say("410784\r\r>");
    else if (!strcmp(q, "0108"))  elm_say("41087C\r\r>");
    else if (!strcmp(q, "0109"))  elm_say("410980\r\r>");
    else if (!strcmp(q, "010E"))  elm_say("410E90\r\r>");       /* timing 0x90/2-64 = 8 degrees */
    else if (!strcmp(q, "0146"))  elm_say("414651\r\r>");       /* outside 41 C */
    else if (!strcmp(q, "015C"))  elm_say("415C82\r\r>");       /* oil 90 C */
    else if (!strcmp(q, "011F"))  elm_say("411F012C\r\r>");     /* running 300 s */
    else if (!strcmp(q, "0101") && toyota) elm_say("41018207E50C\r\r>");   /* light ON, 2 codes, EVAP not finished */
    else if (!strcmp(q, "0101"))  elm_say(elm_engine_codes ? "41010407E504\r\r>" : "41010007E504\r\r>");   /* light off, 4 (or 0) codes */
    else if (!strcmp(q, "020200")) elm_say("4202000133\r\r>");  /* freeze frame made by P0133 */
    else if (!strcmp(q, "020C00")) elm_say("420C001CE8\r\r>");  /* 1850 rpm */
    else if (!strcmp(q, "020D00")) elm_say("420D0048\r\r>");    /* 72 km/h */
    else if (!strcmp(q, "020500")) elm_say("4205007B\r\r>");    /* 83 C */
    else if (!strcmp(q, "020400")) elm_say("42040061\r\r>");
    else if (!strcmp(q, "0902") && toyota) elm_text(0x02, "JTDKN3DU5K0123456", 17, 17);   /* a 2019 (K) Toyota */
    else if (!strcmp(q, "0902") && elm_car == CAR_GM) elm_text(0x02, GM_TEST_VIN, 17, 17);  /* a 2016 (G) Chevrolet */
    else if (!strcmp(q, "0902"))  elm_text(0x02, "6FPTESTXXS1200001", 17, 17);   /* VIN: 3 pieces */
    else if (!strcmp(q, "0904"))  elm_text(0x04, "JB3A-14C204-AB", 14, 16);
    else if (!strcmp(q, "090A"))  elm_text(0x0A, "ECM\0-EngineControl", 18, 20);
    else if (!strcmp(q, "03") && toyota) elm_say("430201330442\r\r>");    /* P0133, P0442 */
    else if (!strcmp(q, "03") && elm_engine_codes) elm_say("00A\r0:430401330442\r1:03000562AAAAAA\r\r>");  /* CAN, 4 codes in 2 pieces + padding */
    else if (!strcmp(q, "03"))    elm_say("4300\r\r>");
    else if (!strcmp(q, "07") && (toyota || elm_engine_codes)) elm_say("47010171\r\r>");     /* pending: P0171 */
    else if (!strcmp(q, "07"))    elm_say("4700\r\r>");
    else if (!strcmp(q, "0A") && toyota) elm_say("4A010133\r\r>");       /* P0133 is also permanent */
    else if (!strcmp(q, "0A"))    elm_say("4A00\r\r>");         /* no permanent codes */
    else if (!strcmp(q, "01A6") && toyota) elm_say("41A60001E240\r\r>");  /* odometer 0x1E240 = 123456 -> 12345.6 km */
    else if (!strcmp(q, "04"))    { elm_got_clear = 1; elm_say("44\r\r>"); }
    else elm_say("NO DATA\r\r>");
}

/* a pretend OBDLink EX (STN chip, ELM327 commands) in the pretend car */
static void fake_elm(void)
{
    char c;
    while (read(pty_master, &c, 1) == 1) {
        if (c != '\r') { if (elm_len < 63) elm_line[elm_len++] = c; continue; }
        elm_line[elm_len] = 0;
        elm_len = 0;
        elm_questions++;
        const char *q = elm_line;
        /* anything that could change the car must never arrive here */
        static const char *const bad[] = { "10", "11", "14", "27", "2E", "2F", "31", "34", "35", "36", "37", "3D", "85", "28",
                                           "3B", "A5", "AE", "2C", "20", "A2", "A9", "AA", "23" };   /* + GM's (GMLAN) */
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            if (!strncmp(q, bad[i], 2)) { elm_forbidden++; snprintf(elm_forbidden_last, sizeof elm_forbidden_last, "%.31s", q); }
        if (!strcmp(q, "04") && elm_header != 0x7DF) elm_forbidden++;
        if (!strncmp(q, "1A", 2)) { elm_gm_queries++; if (elm_header < 0x241 || elm_header > 0x24F) elm_gm_wrong++; }
        if (!strcmp(q, "ATZ"))        { elm_resets++; elm_h1 = 0; elm_st = 0x32; elm_fc_mode = 0; elm_fc_header = -1; elm_fc_data = 0;
                                        elm_say("\r\rELM327 v1.4b\r\r>"); }
        /* flow control: mode 1 needs its header (FCSH) and data (FCSD) first, like a real ELM327 */
        else if (!strncmp(q, "ATFCSH", 6)) { elm_fc_header = (int)strtol(q + 6, NULL, 16); elm_say("OK\r\r>"); }
        else if (!strcmp(q, "ATFCSD300000")) { elm_fc_data = 1; elm_say("OK\r\r>"); }
        else if (!strncmp(q, "ATFCSM", 6) && elm_fc_refuse) elm_say("?\r\r>");    /* a clone without these settings */
        else if (!strcmp(q, "ATFCSM1")) { elm_fc_mode = elm_fc_header >= 0 && elm_fc_data; elm_say(elm_fc_mode ? "OK\r\r>" : "?\r\r>"); }
        else if (!strcmp(q, "ATFCSM0")) { elm_fc_mode = 0; elm_say("OK\r\r>"); }
        else if (!strcmp(q, "STI"))   elm_say("STN2232 v5.10.3\r\r>");
        else if (!strcmp(q, "STDI"))  elm_say("OBDLink EX r2.7.1\r\r>");
        else if (!strcmp(q, "STP53")) { elm_ms = 1; elm_saw_stp53 = 1; elm_say("OK\r\r>"); }
        else if (!strncmp(q, "ATSP", 4)) { elm_ms = 0; elm_say("OK\r\r>"); }
        else if (!strncmp(q, "ATSH", 4)) { elm_header = (int)strtol(q + 4, NULL, 16); elm_say("OK\r\r>"); }
        else if (!strncmp(q, "ATCRA", 5)) { elm_cra = q[5] ? (int)strtol(q + 5, NULL, 16) : -1; elm_say("OK\r\r>"); }
        else if (!strcmp(q, "ATH1"))  { elm_h1 = 1; elm_say("OK\r\r>"); }
        else if (!strcmp(q, "ATH0"))  { elm_h1 = 0; elm_say("OK\r\r>"); }
        else if (!strncmp(q, "ATST", 4)) { elm_st = (int)strtol(q + 4, NULL, 16); elm_say("OK\r\r>"); }
        else if (!strcmp(q, "ATDPN")) elm_say("A6\r\r>");
        else if (!strcmp(q, "ATRV"))  elm_say("12.6V\r\r>");
        else if (!strncmp(q, "AT", 2)) elm_say("OK\r\r>");
        else if (q[0] == '0' && elm_header != 0x7DF) {      /* an OBD-II question to ONE computer */
            if (elm_header == 0x7E0 && elm_cra == 0x7E8 && !elm_ms) fake_engine(q);
            else if (elm_car == CAR_TOYOTA && elm_header == 0x7E1 && elm_cra == 0x7E9 && !elm_ms) fake_gearbox(q);
            else elm_say("NO DATA\r\r>");
        }
        else if (elm_header != 0x7DF) fake_module(q);       /* UDS to one module */
        else if (q[0] == '0')         fake_engine(q);       /* everyone (7DF): the engine computer answers */
        else elm_say("NO DATA\r\r>");                       /* UDS to "everyone": nothing */
    }
}

/* press and release a button (console bit numbers) */
static void tap(int bit) { *keys |= 1u << bit; frames(2); *keys &= ~(1u << bit); frames(2); }

/* the Terminal's on-screen keyboard: put the selection on a key, A presses it */
static void osk(const char *name)
{
    char what[80];
    snprintf(what, sizeof what, "Keyboard: the key \"%s\" is there", name);
    if (!app_terminal_osk_select(name)) { check(0, what); return; }
    tap(13);
}

/* a USB mouse: move the pointer to x, y on the screen (button held or not) */
static int mouse_x = 320, mouse_y = 240, mouse_synced;
static void mouse_at(int x, int y, int down)
{
    if (!mouse_synced) { ui_mouse(mouse_x, mouse_y, 0, 0, 1); mouse_synced = 1; }
    int px, py;
    ui_pointer_pos(&px, &py);
    mouse_x += x - px;
    mouse_y += y - py;
    ui_mouse(mouse_x, mouse_y, down, 0, 1);
    frames(3);
}

/* X opens the app's menu; DOWN `item` times; A picks it */
static void menu(int item)
{
    tap(12);                                         /* X */
    for (int i = 0; i < item; i++) tap(3);           /* DOWN */
    tap(13);                                         /* A */
    frames(3);
}

/* Is tile i framed in green? Looks at a pixel on the tile's left border. */
static int tile_selected(int i)
{
    int gap = 10, top = 30 + gap, bottom = 480 - 26 - gap;
    int tw = (640 - gap * 5) / 4, th = (bottom - top - gap * 2) / 3;
    int x = gap + (i % 4) * (tw + gap) + 1, y = top + (i / 4) * (th + gap) + th / 2;
    uint16_t p = frame[y * NZ_W + x];
    int r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63;
    return g > 150 && r < 100;                      /* NZEUS green */
}

static int selected_tile(void)
{
    int found = -1;
    for (int i = 0; i < 12; i++) if (tile_selected(i)) { if (found >= 0) return -2; found = i; }
    return found;                                    /* -1 none, -2 more than one */
}

static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", NZ_W, NZ_H);
    for (int i = 0; i < NZ_W * NZ_H; i++) {
        uint16_t p = frame[i];
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ---- emoji on screen (WhatsApp tests) ---- */
/* how many pixels in a box are "warm" (yellow / orange / red: emoji colours,
 * never the Original theme's text) */
static int warm_pixels(int x1, int y1, int x2, int y2)
{
    int n = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) {
            if (x < 0 || y < 0 || x >= NZ_W || y >= NZ_H) continue;
            uint16_t p = frame[y * NZ_W + x];
            int r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
            if (r > 200 && g > 100 && b < 110) n++;
        }
    return n;
}

/* the letter number of code point cp in a label's text (-1: not there) */
static int letter_index(const char *t, uint32_t cp)
{
    uint32_t k = 0, c;
    int i = 0;
    while ((c = _lv_txt_encoded_next(t, &k)) != 0) { if (c == cp) return i; i++; }
    return -1;
}

/* is emoji cp on label l drawn as a picture? Its glyph must come from the
 * emoji font (not a placeholder box), and the screen must show emoji colours
 * where the letter is. */
static int emoji_drawn(lv_obj_t *l, uint32_t cp)
{
    if (!l || !lv_obj_check_type(l, &lv_label_class)) return 0;   /* (no such label: a FAIL, never a crash) */
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    lv_font_glyph_dsc_t g;
    int idx = letter_index(lv_label_get_text(l), cp);
    if (idx < 0 || !emoji_font() || !lv_font_get_glyph_dsc(f, &g, cp, 0) || g.is_placeholder ||
        g.resolved_font != emoji_font() || g.box_w != EMOJI_SIZE) return 0;
    lv_point_t pos;
    lv_area_t a;
    lv_label_get_letter_pos(l, (uint32_t)idx, &pos);
    lv_obj_get_coords(l, &a);
    int x = a.x1 + pos.x, y = a.y1 + pos.y;
    return warm_pixels(x - 1, y - 1, x + EMOJI_SIZE + 1, y + lv_font_get_line_height(f) + 1) > 20;
}

/* a label's text, "" when there is no such label (a missing object must FAIL, not crash) */
static const char *ltext(lv_obj_t *o)
{
    return o && lv_obj_check_type(o, &lv_label_class) ? lv_label_get_text(o) : "";
}

/* child i of o, NULL when o is NULL or has no such child */
static lv_obj_t *kid(lv_obj_t *o, int i)
{
    return o && i >= 0 && (uint32_t)i < lv_obj_get_child_cnt(o) ? lv_obj_get_child(o, i) : NULL;
}

/* a label somewhere on the pop-up layer whose text starts with `prefix` (NULL = none) */
static lv_obj_t *popup_label(lv_obj_t *o, const char *prefix, int depth)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strncmp(lv_label_get_text(o), prefix, strlen(prefix))) return o;
    for (uint32_t i = 0; depth > 0 && i < lv_obj_get_child_cnt(o); i++) {
        lv_obj_t *f = popup_label(lv_obj_get_child(o, (int32_t)i), prefix, depth - 1);
        if (f) return f;
    }
    return NULL;
}

/* A test picture as a 24-bit BMP file. check = 1: a 1-pixel pattern of pure
 * red and pure blue ((x + y) odd = red); 0: all pure green. (Pure colours stay
 * exactly the same in RGB565: F800, 001F, 07E0.) */
static void write_test_bmp(const char *path, int w, int h, int check)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int row = (w * 3 + 3) & ~3;
    unsigned char hd[54] = { 'B', 'M' };
    uint32_t v;
    v = 54 + (uint32_t)row * h; memcpy(hd + 2, &v, 4);
    v = 54; memcpy(hd + 10, &v, 4);
    v = 40; memcpy(hd + 14, &v, 4);
    v = (uint32_t)w; memcpy(hd + 18, &v, 4);
    v = (uint32_t)h; memcpy(hd + 22, &v, 4);          /* (positive: rows from the bottom up) */
    hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    unsigned char *line = calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = line + x * 3;               /* B, G, R */
            p[0] = p[1] = p[2] = 0;
            if (!check) p[1] = 255;
            else if ((x + y) & 1) p[2] = 255;
            else p[0] = 255;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

/* A test icon as a 24-bit BMP: size x size, pure green (= see-through) all
 * round, a square of colour `rgb` (0xRRGGBB) in the middle half. */
static void write_icon_bmp(const char *path, int size, uint32_t rgb)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int row = (size * 3 + 3) & ~3;
    unsigned char hd[54] = { 'B', 'M' };
    uint32_t v;
    v = 54 + (uint32_t)row * size; memcpy(hd + 2, &v, 4);
    v = 54; memcpy(hd + 10, &v, 4);
    v = 40; memcpy(hd + 14, &v, 4);
    v = (uint32_t)size; memcpy(hd + 18, &v, 4);
    v = (uint32_t)size; memcpy(hd + 22, &v, 4);
    hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    unsigned char *line = calloc(1, (size_t)row);
    for (int y = size - 1; y >= 0; y--) {
        for (int x = 0; x < size; x++) {
            int in = x >= size / 4 && x < size * 3 / 4 && y >= size / 4 && y < size * 3 / 4;
            uint32_t c = in ? rgb : 0x00FF00;
            line[x * 3] = c & 255; line[x * 3 + 1] = c >> 8 & 255; line[x * 3 + 2] = c >> 16 & 255;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

/* the first thing inside `o` that can scroll down (a long list), NULL = none */
static lv_obj_t *scrollable_in(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(o) > 0) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
        lv_obj_t *f = scrollable_in(lv_obj_get_child(o, (int32_t)i));
        if (f) return f;
    }
    return NULL;
}

/* how many files a folder has */
static int count_files(const char *dir)
{
    char cmd[300], buf[32] = "";
    snprintf(cmd, sizeof cmd, "ls %s 2>/dev/null | wc -l", dir);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (!fgets(buf, sizeof buf, p)) buf[0] = 0;
    pclose(p);
    return atoi(buf);
}

/* are two files the same bytes? */
static int same_file(const char *a, const char *b)
{
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s'", a, b);
    return system(cmd) == 0;
}

static void wait_pdf(void)
{
    for (int i = 0; i < 300 && !app_images_shown_w() && !app_images_pdf_error()[0]; i++) frames(1);
}

static int pdf_caches(void)
{
    int n = 0;
    DIR *dir = opendir("/tmp/fake_sdcard/nzeus");
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) if (!strncmp(e->d_name, "pdf-", 4)) n++;
        closedir(dir);
    }
    return n;
}

/* A real saved PDF must open from Files, not fall through to a filename toast. */
static void pdf_reader_test(void)
{
    const char *folder = "/tmp/fake_sdcard/!pdf-reader";
    const char *english = "/tmp/fake_sdcard/!pdf-reader/English' $(printf BAD).PDF";
    mkdir(folder, 0700);
    check(fs_copy_tree(NZ_REPO "/reports/car/Health-Scan-2026-09-29-EN.pdf", english) == 0,
          "PDF: real report fixture copied");
    ui_open_app(APP_FILES); frames(2);
    while (strcmp(app_files_cwd(), "/tmp/fake_sdcard")) tap(14);
    for (int i = 0; i < 600 && strcmp(app_files_selected_name(), "!pdf-reader"); i++) tap(2);
    tap(13); tap(3); tap(13); wait_pdf();
    check(apps_focused() && apps_focused()->id == APP_IMAGES && app_images_shown_w() > 800,
          "PDF: Files opens a real report, including quotes, spaces and shell characters in its name");
    check(app_images_pdf_page() == 1 && app_images_pdf_pages() == 3, "PDF: correct page count and first page");
    check(app_images_shown_w() >= 1200, "PDF: keeps the report's full source resolution for sharp zoom");
    tap(12);
    check(frame[240 * NZ_W + 50] == 0xFFFF, "PDF: X rotates the page to landscape and fits it on screen");
    save("core_pdf_rotated");
    tap(12); tap(12); tap(12);
    check(frame[240 * NZ_W + 50] == 0, "PDF: four rotations return to the original orientation");
    save("core_pdf_english_fit");
    tap(13); int y = app_images_pdf_pan_y();
    tap(3);
    check(app_images_pdf_zoom() == 1 && app_images_pdf_pan_y() < y, "PDF: A zooms to width and D-pad scrolls");
    save("core_pdf_english_zoom");
    tap(13);
    check(app_images_pdf_zoom() == 2, "PDF: second zoom enlarges the text further");
    tap(11); wait_pdf();
    check(app_images_pdf_page() == 2 && app_images_shown_w() > 800 && !app_images_pdf_zoom(),
          "PDF: R loads the next page and resets zoom");
    tap(10); wait_pdf(); tap(10);
    check(app_images_pdf_page() == 1, "PDF: L goes back and stops at the first page");
    tap(14); frames(2);
    check(apps_focused() && apps_focused()->id == APP_FILES && !pdf_caches(),
          "PDF: B returns to Files and removes the temporary page cache");
    const char *arabic = "/tmp/fake_sdcard/!pdf-reader/Arabic.pdf";
    check(fs_copy_tree(NZ_REPO "/reports/car/Health-Scan-2026-09-29-AR.pdf", arabic) == 0,
          "PDF: real Arabic report fixture copied");
    app_images_open_pdf(arabic); wait_pdf();
    check(app_images_shown_w() > 800 && app_images_pdf_pages() == 3, "PDF: Arabic report renders as a complete page");
    tap(13); save("core_pdf_arabic_zoom");
    tap(11); wait_pdf(); tap(11); wait_pdf(); tap(11);
    check(app_images_pdf_page() == 3 && app_images_shown_w() > 800, "PDF: R stops at the final Arabic page");
    tap(14);
    FILE *bad = fopen("/tmp/fake_sdcard/!pdf-reader/bad.pdf", "w");
    if (bad) { fputs("not a PDF", bad); fclose(bad); }
    app_images_open_pdf("/tmp/fake_sdcard/!pdf-reader/bad.pdf"); wait_pdf();
    check(app_images_pdf_error()[0] && !app_images_shown_w(), "PDF: damaged file explains the failure without stale pixels");
    tap(14);
    app_images_open_pdf(english); tap(14); frames(2);
    check(apps_focused() && apps_focused()->id == APP_FILES && !pdf_caches(),
          "PDF: B can cancel page loading without leaving a renderer or cache behind");
    tap(14); tap(14);
    unlink(english); unlink(arabic); unlink("/tmp/fake_sdcard/!pdf-reader/bad.pdf"); rmdir(folder);
}

/* Real Car UI + PTY transport: a read can fail without USB disappearing. */
static void car_recovery_test(void)
{
    void serial_test_fail_next_read(void);
    int own_port = pty_master < 0;
    if (own_port) {
        pty_master = posix_openpt(O_RDWR | O_NOCTTY);
        if (pty_master < 0 || grantpt(pty_master) || unlockpt(pty_master)) {
            check(0, "Car recovery: test PTY opens");
            return;
        }
        fcntl(pty_master, F_SETFL, O_NONBLOCK);
        serial_extra_port = ptsname(pty_master);
    }
    frame_hook = fake_elm;
    ui_open_app(APP_CAR);
    for (int i = 0; i < 250 && !obd_live()->have[PID_RPM]; i++) frames(1);
    check(obd_state() == OBD_READY && obd_live()->have[PID_RPM], "Car recovery: vehicle initially connected");
    const CarScan *saved = app_car_scan();
    serial_test_fail_next_read();
    frames(1);
    check(obd_state() != OBD_READY, "Car recovery: injected read failure interrupts transport while port stays present");
    for (int i = 0; i < 400 && !(obd_state() == OBD_READY && obd_live()->have[PID_RPM]); i++) frames(1);
    check(obd_state() == OBD_READY && obd_live()->have[PID_RPM], "Car recovery: automatically reconnects the still-present adapter");
    check(app_car_scan() == saved, "Car recovery: reconnect preserves the previous scan");
    if (own_port) {
        tap(14); frames(2);
        frame_hook = NULL; serial_extra_port = NULL;
        close(pty_master); pty_master = -1;
    }
}

int main(int argc, char **argv)
{
    /* glibc keeps a few freed blocks in a per-thread cache that still counts
     * as "in use"; switch it off (by starting again) so leak checks are exact */
    if (!getenv("NZ_REEXEC")) {
        setenv("NZ_REEXEC", "1", 1);
        setenv("GLIBC_TUNABLES", "glibc.malloc.tcache_count=0", 1);
        execv("/proc/self/exe", argv);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);                /* each line out at once: a crash keeps what was said */
    if (argc > 1) outdir = argv[1];
    const char *pics = argc > 2 ? argv[2] : "tests/pics";   /* test pictures (tests/pics) */
    /* NZEUS's folder (on the console /tmp/nzeus-data -> /data/nzeus/deck), and the
     * system's Python examples (on the console /usr/local/lib/nzeus/python-examples) */
    extern const char *python_examples_dir;
    python_examples_dir = "/tmp/fake_pyexamples";
    if (system("rm -rf /tmp/fake_sdcard /tmp/fake_pyexamples && mkdir -p /tmp/fake_sdcard/docs /tmp/fake_sdcard/music && "
               "echo hi > /tmp/fake_sdcard/docs/a.txt && mkdir -p /tmp/fake_sdcard/nzeus /tmp/fake_sdcard/pictures /tmp/fake_pyexamples && "
               "echo 'print(2)' > /tmp/fake_sdcard/docs/b.py && "
               "printf '# hello.py - says hello\\nprint(1)\\n' > /tmp/fake_pyexamples/hello.py") != 0) {
        printf("FAIL  could not create the fake NZEUS folder\n");
        return 1;
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "cp %s/* /tmp/fake_sdcard/pictures/", pics);
    if (system(cmd) != 0) printf("FAIL  could not copy the test pictures from %s\n", pics);


    /* the clock keeper: the console has no clock battery and starts in 1970;
     * NZEUS sets the clock from nzeus/clock.txt (here: a pretend clock, the
     * PC's own clock is never changed) */
    struct tm saved_tm = { .tm_year = 2026 - 1900, .tm_mon = 2, .tm_mday = 14, .tm_hour = 9 };   /* 14 March 2026 09:00 UTC */
    time_t saved_t = timegm(&saved_tm), real_start = time(NULL);
    FILE *ck = fopen(CLOCK_TXT, "w");
    if (ck) { fprintf(ck, "%lld\n2026-03-14 09:00:00 UTC\n", (long long)saved_t); fclose(ck); }
    hw_test_set_now(3 * 24 * 3600);                  /* switched on: 4 January 1970 */

    /* The console's devices (hw_modern.c), all fake: no battery, no backlight,
     * no sound card, no speaker switch and no network until a test makes them
     * (the PC's own files - a laptop battery seen through WSL, the PC's network -
     * must never change what the tests see). */
    if (system("rm -rf /tmp/fake_power /tmp/fake_backlight && mkdir -p /tmp/fake_power /tmp/fake_backlight") != 0) {}
    hw_capacity_file = "/tmp/fake_power/capacity";
    hw_charger_file = "/tmp/fake_power/online";
    hw_levels_file = FAKE_LEVELS;
    hw_power_driver = "/tmp/fake_power/driver";
    hw_backlight_file = "/tmp/fake_backlight/r36s-avp/brightness";
    hw_backlight_glob = "/tmp/fake_backlight/*";
    hw_ctl_dev = "/tmp/no_such_controlC0";
    hw_pcm_dev = "/tmp/no_such_pcmC0D0p";
    hw_headphones_file = "/tmp/no_such_headphones";
    hw_route_file = "/tmp/fake_route";
    unlink(hw_route_file);
    /* the Web app: no NetSurf here until the Web tests make a fake one */
    web_netsurf_path = "/tmp/no_such_netsurf";
    hw_fullscreen_run = NULL;

    retro_set_environment(env);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_state(input_state);
    retro_init();
    retro_load_game(NULL);
    frames(5);
    if (getenv("NZ_PDF_READER_ONLY")) {
        pdf_reader_test();
        void retro_deinit(void);
        retro_deinit();
        return failures ? 1 : 0;
    }
    if (getenv("NZ_CAR_RECOVERY_ONLY")) {
        car_recovery_test();
        void retro_deinit(void);
        retro_deinit();
        return failures ? 1 : 0;
    }
    time_t restored = hw_now();
    check(restored >= saved_t && restored <= saved_t + 5, "Clock: it started in 1970 and was set from nzeus/clock.txt (14 March 2026)");
    hw_test_set_now(saved_t + 7200);                 /* a clock that is already later ... */
    check(hw_clock_restore() == 0 && hw_now() >= saved_t + 7200, "Clock: a clock already later than clock.txt is left alone");
    unlink(CLOCK_TXT);
    hw_clock_keep(nzeus_tick_ms() + 30000);          /* half a minute later: not saved yet */
    int not_yet = clock_file() == -1;
    hw_clock_keep(nzeus_tick_ms() + 61000);          /* a minute later: saved */
    long long kept = clock_file();
    check(not_yet && kept >= saved_t + 7200 && kept <= saved_t + 7210, "Clock: the time is saved to nzeus/clock.txt once a minute");
    hw_test_set_now(100000);                         /* a 1970 clock is never saved over a good one */
    int refused = hw_clock_save() != 0 && clock_file() == kept;
    hw_test_set_now(saved_t + 7200);
    check(refused, "Clock: a clock still in 1970 is not saved");
    check(time(NULL) >= real_start && time(NULL) - real_start < 900, "Clock: the PC's own clock was not changed");
    save("core_1_start");
    check(rgb565_asked, "core asks for 16-bit (RGB565) pictures");
    check(selected_tile() == 0, "starts with Terminal selected");

    tap(3);                                          /* DOWN (bit 3) */
    check(selected_tile() == 4, "DOWN moves to Car");
    tap(5);                                          /* RIGHT (bit 5) */
    check(selected_tile() == 5, "RIGHT moves to Web");
    tap(7); tap(2);                                  /* LEFT, UP */
    check(selected_tile() == 0, "LEFT then UP goes back to Terminal");

    /* the rows that changed (nzeus_dirty_rows): NZEUS's own frontend copies only those */
    {
        int top, bottom;
        frames(3); nzeus_dirty_rows(&top, &bottom);       /* (take what is pending) */
        frames(3); nzeus_dirty_rows(&top, &bottom);
        check(top < 0 || bottom < 40, "Changed rows: an idle home screen changes nothing (at most the status bar)");
        tap(3); nzeus_dirty_rows(&top, &bottom);         /* DOWN: Terminal -> Car */
        check(top >= 0 && top <= 60 && bottom >= 300 && bottom < NZ_H,
              "Changed rows: moving the selection reports the rows of both tiles");
        nzeus_dirty_rows(&top, &bottom);
        check(top < 0, "Changed rows: asking again right away reports nothing new");
        tap(2);                                          /* UP: back to Terminal */
    }

    *keys |= 1u << 5; frames(45); *keys &= ~(1u << 5); frames(2);   /* hold RIGHT ~0.7 s */
    check(selected_tile() == 3, "holding RIGHT repeats to the end of the row");
    tap(7); tap(7); tap(7);

    tap(3); tap(13);                                 /* Car, then A */
    frames(3);
    save("core_2_car_no_adapter");
    check(apps_focused() && apps_focused()->id == APP_CAR && car_no_demo() && !shutdown_asked,
          "A on Car opens the Car app: no adapter = no demo, it says how to plug the ELM327 in");
    tap(14); frames(3);                              /* B: back home */

    /* ---- the built-in Terminal ---- */
    tap(2); tap(13);                                 /* back to Terminal, A */
    frames(3);
    NzApp *term = apps_focused();
    check(term && term->id == APP_TERMINAL && !shutdown_asked, "A on Terminal opens the NZEUS Terminal (no FrogShell)");
    save("core_3_terminal");
    type_line("echo nzeus-ok");                      /* typed on a (simulated) USB keyboard */
    int found = 0;
    for (int i = 0; i < 200 && !found; i++) { frames(1); found = terminal_has_line("nzeus-ok"); }
    check(found, "a command typed on the keyboard runs, and its output appears");
    tap(13);                                         /* A: on-screen keyboard */
    tap(5); tap(5); tap(5); tap(13);                 /* D-pad to a key, A types it */
    frames(3);
    check(strlen(app_terminal_input()) == 1, "the on-screen keyboard types with the D-pad and A");
    save("core_4_terminal_keyboard");
    {   /* the typed line must be readable in every theme (WCAG contrast >= 4.5:1) */
        int th_before = theme_index(), bad = -1;
        double worst = 99;
        for (int t = 0; t < TH_COUNT; t++) {
            theme_set(t); frames(2);
            lv_obj_t *ta = app_terminal_textbox();
            uint32_t fg = lv_color_to32(lv_obj_get_style_text_color(ta, LV_PART_MAIN)) & 0xFFFFFF;
            uint32_t bg = lv_color_to32(lv_obj_get_style_bg_color(ta, LV_PART_MAIN)) & 0xFFFFFF;
            double lf = 0, lb = 0;
            for (int c = 0; c < 3; c++) {                    /* relative luminance of each colour */
                double w[3] = { 0.2126, 0.7152, 0.0722 };
                double a = ((fg >> (16 - 8 * c)) & 0xFF) / 255.0, b = ((bg >> (16 - 8 * c)) & 0xFF) / 255.0;
                a = a <= 0.03928 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4);
                b = b <= 0.03928 ? b / 12.92 : pow((b + 0.055) / 1.055, 2.4);
                lf += w[c] * a; lb += w[c] * b;
            }
            double ratio = (fmax(lf, lb) + 0.05) / (fmin(lf, lb) + 0.05);
            if (ratio < worst) { worst = ratio; bad = t; }
            if (t == TH_RETRO_BLUE) {
                lv_textarea_set_text(ta, "echo readable in Retro Blue");
                frames(2); save("core_4b_terminal_retro_input");
                lv_textarea_set_text(ta, "");
            }
        }
        printf("INFO  Terminal typed line: worst contrast %.1f:1 (theme %d)\n", worst, bad);
        check(worst >= 4.5, "Terminal: the typed line is readable in every theme (contrast >= 4.5:1)");
        theme_set(th_before); frames(2);
    }
    tap(14);                                         /* B: hide the keyboard */
    tap(14);                                         /* B: leave the Terminal */
    frames(3);
    check(apps_focused() == NULL && selected_tile() == 0, "B, B: keyboard away, back on the home screen");

    /* ---- Terminal: history kept on the SD card, sessions, the Commands list,
     *      copy + paste, the coding keyboard, and the screenshot buttons ---- */
    {
        const char *hist = "/tmp/fake_sdcard/nzeus/terminal_history";
        char line[300];
        KbEvent ctrl_u = { KB_CTRL_OTHER, 'u' };
        check(file_contains(hist, "echo nzeus-ok\n"), "Terminal history: a command is saved to nzeus/terminal_history at once");
        tap(13); frames(3);                              /* A: the Terminal again */
        type_line("echo one"); wait_for_line("one");
        type_line("echo one"); frames(5);                /* the same command again */
        type_line("echo two"); wait_for_line("two");
        type_line(""); type_line("   ");                 /* empty lines */
        int n = file_lines(hist, 0, NULL, 0), ones = 0, empty = 0;
        for (int i = 0; i < n; i++) {
            file_lines(hist, i, line, sizeof line);
            if (!strcmp(line, "echo one")) ones++;
            if (!line[0] || line[0] == ' ') empty++;
        }
        check(n == 3 && ones == 1 && !empty, "Terminal history: a repeat and empty lines are not saved");
        tap(14); frames(3);                              /* B: leave (the Terminal closes) ... */
        int closed = apps_get(APP_TERMINAL)->root == NULL;
        tap(13); frames(3);                              /* ... and open it again: a "restart" */
        tap(2); int up1 = !strcmp(app_terminal_input(), "echo two");
        tap(2); int up2 = !strcmp(app_terminal_input(), "echo one");
        tap(2); tap(2); int up3 = !strcmp(app_terminal_input(), "echo nzeus-ok");   /* (stops at the oldest) */
        tap(3); tap(3); tap(3);                          /* DOWN x3: back to the empty line */
        check(closed && up1 && up2 && up3 && !app_terminal_input()[0],
              "Terminal history: still there after a restart (UP x3, then DOWN back to the empty line)");
        for (int i = 1; i <= 210; i++) { char c[16]; snprintf(c, sizeof c, "clear %d", i); type_line(c); }
        frames(2);
        char first[64], last[64];
        n = file_lines(hist, 0, first, sizeof first);
        file_lines(hist, n - 1, last, sizeof last);
        check(n == 200 && !strcmp(first, "clear 11") && !strcmp(last, "clear 210"), "Terminal history: only the last 200 commands are kept");
        tap(14); frames(3); tap(13); frames(3);          /* restart once more */
        check(terminal_history_count() == 200, "Terminal history: all 200 are loaded at the next start");

        /* sessions */
        type_line("cd /tmp/fake_sdcard");
        menu(4);                                         /* X menu: New session */
        check(terminal_session_count() == 2 && terminal_session_current() == 1 && !strcmp(app_terminal_title(), "Terminal 2 of 2"),
              "Terminal: New session (title: Terminal 2 of 2)");
        lv_obj_t *bar = lv_obj_get_child(apps_focused()->host, 0);
        check(!strcmp(lv_label_get_text(lv_obj_get_child(bar, 1)), "Terminal 2 of 2"), "Terminal: the title bar shows which session");
        check(!strcmp(terminal_get_cwd(), "/tmp/fake_sdcard"), "Terminal: a new session starts in the same folder");
        type_line("cd docs");
        type_line("echo only-in-two"); wait_for_line("only-in-two");
        {   /* L1 / R1 scroll the old output (and the hints say so, with several sessions too) */
            type_line("seq 1 60"); wait_for_line("60"); frames(10);
            int live = -1, back = -1, again = -1, scrolled;
            for (int k = 0; app_terminal_row_line(k) >= 0; k++) live = app_terminal_row_line(k);   /* the bottom row's line */
            tap(10); frames(2);                          /* L1 */
            scrolled = terminal_scroll_get();
            for (int k = 0; app_terminal_row_line(k) >= 0; k++) back = app_terminal_row_line(k);
            tap(11); frames(2);                          /* R1 */
            for (int k = 0; app_terminal_row_line(k) >= 0; k++) again = app_terminal_row_line(k);
            check(scrolled == 5 && back == live - 5 && terminal_scroll_get() == 0 && again == live,
                  "Terminal: L1 shows older output (5 lines back), R1 comes forward again");
            check(popup_label(apps_focused()->root, "L1/R1", 8) != NULL, "Terminal: with 2 sessions the hints still show L1/R1 Scroll");
        }
        KbEvent q = { KB_CHAR, 'q' }; ui_kbd(&q);        /* typed, not run */
        frames(3);
        save("core_term_sessions");
        theme_set(TH_GREEN_CRT); frames(3); save("core_term_sessions_crt"); theme_set(TH_ORIGINAL); frames(2);
        tap(12); frames(3); save("core_term_menu"); tap(14); frames(2);   /* X: the menu (does it fit?) */
        tap(7); frames(2);                               /* LEFT: session 1 */
        check(terminal_session_current() == 0 && !strcmp(terminal_get_cwd(), "/tmp/fake_sdcard") && !terminal_has_line("only-in-two") &&
              !app_terminal_input()[0], "Terminal: sessions are separate (cd and output in session 2 don't touch 1)");
        tap(5); frames(2);                               /* RIGHT: back to 2 */
        check(terminal_session_current() == 1 && !strcmp(terminal_get_cwd(), "/tmp/fake_sdcard/docs") && terminal_has_line("only-in-two") &&
              !strcmp(app_terminal_input(), "q"), "Terminal: switching back keeps its folder, its output and its typed line");
        ui_kbd(&ctrl_u);
        tap(14); frames(2);                              /* B with 2 sessions: asks first */
        int asked = nzui_active();
        tap(14); frames(2);                              /* B again: "no" */
        check(asked && apps_focused() && apps_focused()->id == APP_TERMINAL, "Terminal: B with several sessions asks before closing them");
        type_line("exit"); frames(3);                    /* exit closes only this session */
        check(terminal_session_count() == 1 && apps_focused() && apps_focused()->id == APP_TERMINAL && !strcmp(app_terminal_title(), "Terminal"),
              "Terminal: exit closes just that session");
        menu(4); menu(6);                                /* New session, then X menu: Close this session */
        check(terminal_session_count() == 1 && apps_focused() && apps_focused()->id == APP_TERMINAL, "Terminal: Close this session");

        /* the Commands list */
        menu(0);                                         /* X menu: Commands... */
        check(term_cmds_is_open() && !strcmp(term_cmds_group_name(term_cmds_group()), "Files and folders") && term_cmds_total() >= 60,
              "Commands list opens (Files and folders first, 60+ commands)");
        save("core_term_commands");
        for (int i = 0; i < 3; i++) tap(5);              /* RIGHT x3: Wi-Fi and internet (pictures of the changed groups) */
        int wifi_group = !strcmp(term_cmds_group_name(term_cmds_group()), "Wi-Fi and internet");
        save("core_term_commands_wifi");
        tap(5); save("core_term_commands_hw");
        tap(5); save("core_term_commands_tools");
        tap(5); save("core_term_commands_python");
        for (int i = 0; i < 6; i++) tap(7);              /* LEFT x6: back to Files and folders */
        check(wifi_group && !strcmp(term_cmds_group_name(term_cmds_group()), "Files and folders"),
              "Commands list: RIGHT / LEFT go through the groups (Wi-Fi and internet is the 4th)");
        tap(3); tap(3);                                  /* DOWN x2: cd FOLDER */
        check(!strcmp(term_cmds_row_text(0), "cd FOLDER") && strstr(term_cmds_row_text(1), "FOLDER = its name"),
              "Commands list: each command shows its plain explanation");
        theme_set(TH_GREEN_CRT); frames(3); save("core_term_commands_crt"); theme_set(TH_ORIGINAL); frames(2);
        tap(13); frames(3);                              /* A: put it on the typed line */
        check(!term_cmds_is_open() && !strcmp(app_terminal_input(), "cd FOLDER") && app_terminal_cursor() == 3 && !terminal_has_line("$ cd FOLDER"),
              "Commands list: A puts it on the typed line (not run), the cursor on FOLDER");
        const char *word = "docs";
        for (const char *c = word; *c; c++) { KbEvent ev = { KB_CHAR, *c }; ui_kbd(&ev); }
        check(!strcmp(app_terminal_input(), "cd docs"), "Commands list: typing replaces the CAPITALS part");
        ui_kbd(&ctrl_u);
        menu(0);                                         /* again: opens on the same group, on "ls" */
        tap(15); frames(2);                              /* Y: star it */
        check(file_contains("/tmp/fake_sdcard/nzeus/terminal_favourites", "ls\n") && term_fav_has("ls"),
              "Commands list: Y stars a command (saved in nzeus/terminal_favourites)");
        tap(7); frames(2);                               /* LEFT from the first group: Favourites */
        check(!strcmp(term_cmds_group_name(term_cmds_group()), "Favourites") && !strcmp(term_cmds_row_text(0), "ls"),
              "Commands list: the starred command is in Favourites");
        tap(7); frames(2);                               /* LEFT: Recent */
        check(!strcmp(term_cmds_group_name(term_cmds_group()), "Recent") &&
              !strcmp(term_cmds_row_text(0), terminal_history_get(terminal_history_count() - 1)),
              "Commands list: Recent shows the newest command first");
        tap(5); tap(5); frames(2);                       /* RIGHT x2: round to the first group */
        tap(14); frames(2);                              /* B: close */
        check(!term_cmds_is_open() && apps_focused() && apps_focused()->id == APP_TERMINAL, "Commands list: B closes it");

        /* copy and paste */
        type_line("echo copy-me-123"); wait_for_line("[exit code 0]");
        menu(2);                                         /* X menu: Select text to copy */
        int sel_on = app_terminal_selecting();
        tap(2);                                          /* UP: from [exit 0] to the output line */
        tap(15);                                         /* Y: the word under the cursor */
        tap(13); frames(2);                              /* A: copy it */
        check(sel_on && !app_terminal_selecting() && !strcmp(nz_clip_get(), "copy-me-123"),
              "Select: Y takes the word under the cursor, A copies it");
        menu(1);                                         /* X menu: Paste */
        check(!strcmp(app_terminal_input(), "copy-me-123"), "Paste: the copied text comes back on the typed line");
        ui_kbd(&ctrl_u);
        check(!app_terminal_input()[0], "Ctrl+U clears the typed line");
        type_line("printf 'a1\\nb2\\n'"); wait_for_line("b2"); frames(5);
        menu(3);                                         /* X menu: Copy last output */
        check(!strcmp(nz_clip_get(), "a1\nb2"), "Copy last output: all lines of the last command");
        menu(1);
        check(!strcmp(app_terminal_input(), "a1 b2"), "Paste: several lines become one typed line");
        ui_kbd(&ctrl_u);

        /* Select text: any letters of the output, also over several lines */
        type_line("printf 'alpha beta gamma\\none111\\ntwo222\\nthree333\\n'");
        wait_for_line("three333");
        for (int i = 0; i < 200 && terminal_running(); i++) frames(1);
        frames(5);                                       /* (and its [exit 0] line) */
        int la = line_index("alpha beta gamma"), al, ac, cl, cc;
        menu(2);                                         /* the cursor starts on the lowest line */
        app_terminal_sel(&al, &ac, &cl, &cc);
        int start_ok = cl == terminal_line_count() - 1 && cc == 0 && al < 0;
        for (int i = 0; i < 4; i++) tap(2);              /* UP x4: "alpha beta gamma" */
        for (int i = 0; i < 6; i++) tap(5);              /* RIGHT x6: the b of beta */
        save("core_term_select_cursor");
        tap(13);                                         /* A: start here */
        for (int i = 0; i < 3; i++) tap(5);              /* RIGHT x3: its last a */
        tap(13); frames(3);                              /* A: end here */
        int done = app_terminal_sel(&al, &ac, &cl, &cc), x, y, x2, y2;
        int shown = app_terminal_cell_xy(la, 7, &x, &y) && app_terminal_cell_xy(la, 2, &x2, &y2);
        check(start_ok && done && al == la && ac == 6 && cl == la && cc == 9 && shown && is_accent_at(x, y) && !is_accent_at(x2, y2),
              "Select: A, move, A highlights just those letters, in the theme's accent colour");
        save("core_term_select");
        theme_set(TH_GREEN_CRT); frames(3); save("core_term_select_crt"); theme_set(TH_ORIGINAL); frames(2);
        tap(13); frames(2);                              /* A: copy */
        check(!app_terminal_selecting() && !strcmp(nz_clip_get(), "beta"), "Select: part of one line is copied exactly (beta)");
        menu(2);
        for (int i = 0; i < 3; i++) tap(2);              /* UP x3: "one111" */
        for (int i = 0; i < 3; i++) tap(5);              /* RIGHT x3: its first 1 */
        tap(13);                                         /* A: start here */
        tap(3); tap(3);                                  /* DOWN x2: "three333", same column */
        tap(5);                                          /* RIGHT: the last e of three */
        tap(13); frames(3);                              /* A: end here */
        save("core_term_select_lines");
        tap(13); frames(2);                              /* A: copy */
        check(!strcmp(nz_clip_get(), "111\ntwo222\nthree"), "Select: over 3 lines, part of the first and the last (line breaks kept)");
        menu(1);
        check(!strcmp(app_terminal_input(), "111 two222 three"), "Paste: the 3 lines become one typed line");
        ui_kbd(&ctrl_u);
        menu(2); tap(2); tap(13); tap(7); tap(14); frames(2);   /* start, move, B */
        check(!app_terminal_selecting() && !strcmp(nz_clip_get(), "111\ntwo222\nthree") && apps_focused() &&
              apps_focused()->id == APP_TERMINAL, "Select: B cancels (nothing copied, still in the Terminal)");
        menu(2); tap(2); tap(13); tap(2);                /* "three333": start; UP: "two222" */
        tap(12); frames(2); save("core_term_select_menu");
        tap(3); tap(3); tap(13); frames(2);              /* X: More > Select whole lines */
        tap(13); frames(2);                              /* A: copy */
        check(!strcmp(nz_clip_get(), "two222\nthree333"), "Select: X > Select whole lines");
        char longcmd[160] = "echo ";                     /* a line longer than the screen is wide */
        for (int i = 0; i < 100; i++) strcat(longcmd, "x");
        strcat(longcmd, "END");
        type_line(longcmd);
        for (int i = 0; i < 200 && !terminal_has_line("[exit code 0]"); i++) frames(1);
        for (int i = 0; i < 200 && terminal_running(); i++) frames(1);
        frames(5);
        const char *scr = app_terminal_screen(), *tail = scr + strlen(scr) - 19;
        check(strlen(scr) > 19 && !strcmp(tail, "xxEND\n[exit code 0]"), "Terminal: a long line goes on on the next row, the newest line stays in view");
        menu(2); tap(2); tap(7);                         /* the long line; LEFT: back to the end of "$ echo xxx..." */
        app_terminal_sel(&al, &ac, &cl, &cc);
        int wrapped = cl == terminal_line_count() - 3 && cc == 109;
        tap(5); tap(13); tap(5); tap(5); tap(13); tap(13); frames(2);   /* "xxx": start, RIGHT x2, end, copy */
        check(wrapped && !strcmp(nz_clip_get(), "xxx"), "Select: LEFT / RIGHT go on over the line ends");

        /* every session */
        menu(4);                                         /* New session */
        type_line("echo second-session"); wait_for_line("second-session");
        for (int i = 0; i < 200 && terminal_running(); i++) frames(1);
        frames(5);
        menu(2); tap(2); tap(15); tap(13); frames(2);
        check(terminal_session_count() == 2 && !strcmp(nz_clip_get(), "second-session"), "Select: works in another session too");
        menu(6); frames(2);                              /* Close this session */

        /* the typed line: Mark, Copy, Cut, and Paste where the cursor is */
        for (const char *c = "hello world"; *c; c++) { KbEvent ev = { KB_CHAR, *c }; ui_kbd(&ev); }
        tap(13); frames(2);                              /* A: the keyboard */
        if (app_terminal_layer() != 3) osk("Edit");
        osk("Home");
        for (int i = 0; i < 6; i++) osk(LV_SYMBOL_RIGHT);
        osk("Mark");
        int marking = app_terminal_marking(), lo = -1, hi = -1;
        for (int i = 0; i < 5; i++) osk(LV_SYMBOL_RIGHT);
        int marked = app_terminal_marked(&lo, &hi) && lo == 6 && hi == 11;
        frames(2); save("core_term_mark");
        osk("Copy");
        check(marking && marked && !strcmp(nz_clip_get(), "world") && !strcmp(app_terminal_input(), "hello world") && !app_terminal_marking(),
              "Typed line: Mark + arrows mark letters, Copy copies just them");
        osk("Home"); osk("Mark");
        for (int i = 0; i < 5; i++) osk(LV_SYMBOL_RIGHT);
        osk("Cut");
        check(!strcmp(app_terminal_input(), " world") && !strcmp(nz_clip_get(), "hello") && app_terminal_cursor() == 0,
              "Typed line: Cut takes the marked letters out (to the clipboard)");
        osk(LV_SYMBOL_RIGHT); osk("Paste");
        check(!strcmp(app_terminal_input(), " helloworld") && app_terminal_cursor() == 6, "Paste: goes where the cursor is, not at the end");
        tap(14); frames(2);                              /* B: keyboard away */
        ui_kbd(&ctrl_u);
        for (const char *c = "abc def"; *c; c++) { KbEvent ev = { KB_CHAR, *c }; ui_kbd(&ev); }
        KbEvent sh_left = { KB_LEFT, KB_SHIFTED }, ctrl_c = { KB_CTRL_C, 0 }, ctrl_v = { KB_CTRL_OTHER, 'v' };
        KbEvent big_x = { KB_CHAR, 'X' }, home_key = { KB_HOME, 0 };
        for (int i = 0; i < 3; i++) ui_kbd(&sh_left);    /* Shift + LEFT x3: "def" */
        ui_kbd(&ctrl_c);                                 /* Ctrl+C: copies the marked letters */
        int usb_copied = !strcmp(nz_clip_get(), "def") && !strcmp(app_terminal_input(), "abc def");
        KbEvent end_key = { KB_END, 0 };
        ui_kbd(&end_key);                                /* End, Shift + LEFT x3: "def" again */
        for (int i = 0; i < 3; i++) ui_kbd(&sh_left);
        ui_kbd(&big_x);                                  /* typing replaces the marked letters */
        int replaced = !strcmp(app_terminal_input(), "abc X");
        ui_kbd(&home_key); ui_kbd(&ctrl_v);              /* Home, Ctrl+V */
        check(usb_copied && replaced && !strcmp(app_terminal_input(), "defabc X"),
              "USB keyboard: Shift+arrows mark, Ctrl+C copies them, typing replaces them, Ctrl+V pastes at the cursor");
        ui_kbd(&ctrl_u);

        /* the keyboard for coding */
        tap(13); frames(2);                              /* A: the keyboard */
        app_terminal_osk_select("#+="); tap(13);
        check(app_terminal_layer() == 2, "Keyboard: #+= shows the symbols");
        save("core_term_kb_symbols");
        theme_set(TH_AMBER_CRT); frames(3); save("core_term_kb_symbols_crt"); theme_set(TH_ORIGINAL); frames(2);
        static const char *const syms[] = { "|", ">", "~", "$", "\\", "&", "`" };
        for (int i = 0; i < 7; i++) { app_terminal_osk_select(syms[i]); tap(13); }
        check(!strcmp(app_terminal_input(), "|>~$\\&`"), "Keyboard: symbols type ( | > ~ $ \\ & ` )");
        app_terminal_osk_select(LV_SYMBOL_LEFT); tap(13);
        check(app_terminal_cursor() == 6, "Keyboard: the left arrow moves the cursor");
        app_terminal_osk_select("Edit"); tap(13);
        check(app_terminal_layer() == 3, "Keyboard: Edit shows Esc / Home / End / Copy / Paste");
        save("core_term_kb_edit");
        app_terminal_osk_select("Home"); tap(13);
        int home_ok = app_terminal_cursor() == 0;
        app_terminal_osk_select("Clear"); tap(13);
        int cleared = !app_terminal_input()[0];
        app_terminal_osk_select("space"); tap(13);
        int spaced = !strcmp(app_terminal_input(), " ");
        app_terminal_osk_select("Clear"); tap(13);
        check(home_ok && cleared && spaced, "Keyboard: Home, Clear, space");
        app_terminal_osk_select("abc"); tap(13);
        const char *part = "cd /tmp/fake_sdcard/do";
        for (const char *c = part; *c; c++) { KbEvent ev = { KB_CHAR, *c }; ui_kbd(&ev); }
        tap(13); app_terminal_osk_select("Tab"); tap(13);   /* keyboard again, Tab */
        check(!strcmp(app_terminal_input(), "cd /tmp/fake_sdcard/docs/"), "Keyboard: Tab finishes a folder name");
        tap(1); frames(2);                               /* START: Enter */
        check(!strcmp(terminal_get_cwd(), "/tmp/fake_sdcard/docs"), "Keyboard: ... and START runs it");
        part = "cat a";
        for (const char *c = part; *c; c++) { KbEvent ev = { KB_CHAR, *c }; ui_kbd(&ev); }
        tap(13); app_terminal_osk_select("Tab"); tap(13);
        check(!strcmp(app_terminal_input(), "cat a.txt "), "Keyboard: Tab finishes a file name");
        ui_kbd(&ctrl_u);
        type_line("sleep 30"); frames(5);
        int long_one = terminal_running();
        tap(13); app_terminal_osk_select("Ctrl"); tap(13);
        int armed = app_terminal_ctrl();
        app_terminal_osk_select("c"); tap(13);
        for (int i = 0; i < 100 && terminal_running(); i++) frames(1);
        check(long_one && armed && !terminal_running() && !app_terminal_ctrl() && !app_terminal_input()[0],
              "Keyboard: Ctrl then C stops the running program (Ctrl+C)");

        /* screenshot: hold SELECT, press L1 (in every app, keyboard open, windows) */
        const char *shots = "/tmp/fake_sdcard/nzeus/shots";
        app_terminal_osk_select("z"); tap(13);           /* (the keyboard is still open) */
        int before = count_pngs(shots), scroll0 = terminal_scroll_get();
        *keys |= 1u << 0; frames(2);                     /* hold SELECT ... */
        *keys |= 1u << 10; frames(1);                    /* ... and press L1 */
        int flashed = ui_flash_visible();
        *keys &= ~(1u << 10); frames(2);
        *keys &= ~(1u << 0); frames(3);                  /* let go of SELECT */
        check(count_pngs(shots) == before + 1 && flashed, "Screenshot: SELECT + L1 saves a PNG in nzeus/shots, with a white flash");
        check(!strncmp(ui_toast_text(), "Screenshot saved: shots/shot-", 29), "Screenshot: the message says where it went");
        char png[256];
        snprintf(png, sizeof png, "/tmp/fake_sdcard/nzeus/%s", ui_toast_text() + 18);
        check(file_starts_with(png, "\x89PNG"), "Screenshot: the file is a PNG");
        check(ui_mode() == UI_HANDHELD && app_terminal_kb_open() && !strcmp(app_terminal_input(), "z") && terminal_scroll_get() == scroll0,
              "Screenshot: works with the keyboard open, and SELECT / L1 do nothing else");
        before = count_pngs(shots);
        tap(10); frames(2);                              /* L1 alone: scrolls, no picture */
        int l1_alone = count_pngs(shots) == before;
        tap(0); frames(3);                               /* SELECT alone: desktop mode, no picture */
        check(l1_alone && count_pngs(shots) == before && ui_mode() == UI_DESKTOP,
              "Screenshot: L1 alone or SELECT alone take no picture (SELECT still switches the look)");
        ui_open_app(APP_TERMINAL); frames(5);            /* the Terminal in a window */
        save("core_term_window");
        KbEvent esc = { KB_ESC, 0 }; ui_kbd(&esc); frames(3);   /* a USB key hides the on-screen keyboard */
        save("core_term_window_keys");
        {                                                /* the mouse: drag over the output = select + copy */
            int row = -1, line = -1, mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;
            for (int k = 0; app_terminal_row_line(k) >= 0; k++)
                if ((int)strlen(terminal_line(app_terminal_row_line(k))) >= 6 && app_terminal_cell_xy(app_terminal_row_line(k), 0, &mx0, &my0))
                    row = k;                             /* the lowest row that starts a line of 6+ letters */
            line = app_terminal_row_line(row);
            int found = row >= 0 && app_terminal_cell_xy(line, 0, &mx0, &my0) && app_terminal_cell_xy(line, 4, &mx1, &my1);
            if (found) {
                mouse_at(mx0 + 3, my0 + 8, 0); mouse_at(mx0 + 3, my0 + 8, 1);       /* press on its first letter */
                mouse_at(mx1 + 3, my1 + 8, 1); mouse_at(mx1 + 3, my1 + 8, 0);       /* drag to the 5th, let go */
            }
            char want[8];
            snprintf(want, sizeof want, "%.5s", found ? terminal_line(line) : "?");
            check(found && app_terminal_selecting() && !strcmp(nz_clip_get(), want),
                  "Terminal window: dragging the mouse over the output selects and copies");
            save("core_term_window_select");
            ui_kbd(&esc); frames(2);
            check(!app_terminal_selecting(), "Terminal window: Esc ends the selection");
        }
        theme_set(TH_GREEN_CRT); frames(3); save("core_term_window_crt"); theme_set(TH_ORIGINAL); frames(2);
        KbEvent f2 = { KB_F(2), 0 }; ui_kbd(&f2); frames(3);    /* F2: the Commands list, in the window */
        check(term_cmds_is_open(), "Terminal window: F2 opens the Commands list");
        save("core_term_window_commands");               /* (left open: the window is closed with it below) */
        before = count_pngs(shots);
        *keys |= 1u << 0; frames(2); *keys |= 1u << 10; frames(2);
        *keys &= ~(1u << 10); frames(2); *keys &= ~(1u << 0); frames(3);
        check(apps_get(APP_TERMINAL)->in_window && count_pngs(shots) == before + 1 && ui_mode() == UI_DESKTOP,
              "Screenshot: SELECT + L1 also works in window mode");
        apps_close(apps_get(APP_TERMINAL)); frames(5);   /* closed with the Commands list still open */
        check(!term_cmds_is_open() && desktop_window_count() == 0, "Terminal window: closing it with the Commands list open is safe");
        tap(0); frames(3);                               /* SELECT: back to handheld mode */
        check(ui_mode() == UI_HANDHELD && apps_focused() == NULL && selected_tile() == 0, "Terminal tests: back on the home screen");
    }

    /* ---- the Python app ---- */
    tap(5); tap(5); tap(5); tap(13);                 /* RIGHT x3 to Python, A */
    frames(3);
    check(apps_focused() && apps_focused()->id == APP_PYTHON && app_python_count() == 2,
          "A on Python opens the Python app: Start Python + the example");
    check(popup_label(apps_focused()->root, "Python 3", 12) != NULL, "the Python app is Debian's Python 3 (not MicroPython)");
    save("core_9_python");
    tap(3); tap(13); frames(5);                      /* DOWN to hello.py, A: run it */
    int ran = 0;
    for (int i = 0; i < terminal_line_count(); i++)
        if (strstr(terminal_line(i), "$ python /tmp/fake_pyexamples/hello.py")) ran = 1;
    check(apps_focused() && apps_focused()->id == APP_TERMINAL && ran, "A on an example runs it with python in the Terminal");
    tap(14); frames(3);                              /* B: leave the Terminal */
    check(apps_focused() && apps_focused()->id == APP_PYTHON, "B in the Terminal goes back to the Python app");
    tap(14); frames(3);                              /* B: home */
    check(apps_focused() == NULL && selected_tile() == 3, "B in Python goes back home");

    /* ---- the built-in Files app ----
     * NZEUS's (fake) folder: docs/ (a.txt) music/ nzeus/ pictures/  (folders first, A-Z) */
    tap(7); tap(7); tap(13);                         /* LEFT x2 to Files, A */
    frames(3);
    NzApp *files = apps_focused();
    check(files && files->id == APP_FILES && !shutdown_asked, "A on Files opens the NZEUS Files app (no FrogShell)");
    check(!strcmp(app_files_cwd(), "/tmp/fake_sdcard") && !strcmp(app_files_selected_name(), "docs"),
          "it starts in NZEUS's folder, folders first");
    tap(13);                                         /* A on docs */
    check(!strcmp(app_files_cwd(), "/tmp/fake_sdcard/docs"), "A opens a folder");
    tap(3);                                          /* DOWN to a.txt (after "..") */
    check(!strcmp(app_files_selected_name(), "a.txt"), "the folder lists its file");

    /* ---- the built-in Editor (opened from Files) ---- */
    tap(13);                                         /* A on a.txt */
    frames(3);
    NzApp *ed = apps_focused();
    check(ed && ed->id == APP_EDITOR && strstr(app_editor_path(), "/docs/a.txt"), "A on a text file opens the Editor");
    save("core_6_editor");
    KbEvent x = { KB_CHAR, 'X' }, ctrl_s = { KB_CTRL_S, 0 };
    ui_kbd(&x); ui_kbd(&ctrl_s); frames(2);
    check(file_starts_with("/tmp/fake_sdcard/docs/a.txt", "Xhi"), "typing, then Ctrl+S saves the file");
    editor_set_cursor(0, 0);
    menu(1);                                         /* X menu: Find... */
    type_line("HI");                                 /* (case doesn't matter) */
    frames(2);
    check(editor_cursor_row() == 0 && editor_cursor_col() == 1, "Find puts the cursor on the word");
    menu(6);                                         /* X menu: Copy this line */
    check(!strcmp(nz_clip_get(), "Xhi"), "Editor: Copy this line uses the clipboard the Terminal uses too");
    tap(14); frames(3);                              /* B: close (nothing unsaved) */
    check(apps_focused() && apps_focused()->id == APP_FILES && lv_scr_act() == apps_focused()->host,
          "closing the Editor goes back to Files (on screen)");
    tap(13); frames(3);                              /* open it again, change it, try to close */
    KbEvent y = { KB_CHAR, 'Y' };
    ui_kbd(&y); tap(14); frames(2);
    check(nzui_active(), "closing with unsaved changes asks first");
    tap(3); tap(13); frames(3);                      /* "Close without saving" */
    check(file_starts_with("/tmp/fake_sdcard/docs/a.txt", "Xhi") && apps_focused()->id == APP_FILES,
          "\"close without saving\" leaves the file as it was");

    tap(3); tap(13); frames(5);                      /* DOWN to b.py, A: runs it */
    int py_ran = 0;
    for (int i = 0; i < terminal_line_count(); i++)
        if (strstr(terminal_line(i), "$ python \"/tmp/fake_sdcard/docs/b.py\"")) py_ran = 1;
    check(apps_focused() && apps_focused()->id == APP_TERMINAL && py_ran, "A on a .py file runs it with python");
    tap(14); frames(3);                              /* B: back to Files */
    tap(2);                                          /* UP to a.txt again */
    check(!strcmp(app_files_selected_name(), "a.txt"), "B in the Terminal goes back to Files");

    menu(1);                                         /* X menu: Copy */
    tap(14);                                         /* B: up a folder */
    check(!strcmp(app_files_cwd(), "/tmp/fake_sdcard"), "B goes up a folder");
    tap(3); tap(13);                                 /* DOWN to music, A */
    menu(3);                                         /* X menu: Paste here */
    check(access("/tmp/fake_sdcard/music/a.txt", F_OK) == 0, "copy, then paste in another folder");
    save("core_5_files");
    menu(6);                                         /* X menu: New folder */
    type_line("newdir");                             /* the name, then Enter */
    frames(3);
    check(access("/tmp/fake_sdcard/music/newdir", F_OK) == 0, "new folder (named with the keyboard)");
    tap(3); tap(3);                                  /* .., newdir, a.txt: select a.txt */
    menu(4);                                         /* X menu: Rename (the box starts with "a.txt") */
    for (int i = 0; i < 5; i++) { KbEvent bs = { KB_BACKSPACE, 0 }; ui_kbd(&bs); }
    type_line("b.txt");
    frames(3);
    check(access("/tmp/fake_sdcard/music/b.txt", F_OK) == 0 && access("/tmp/fake_sdcard/music/a.txt", F_OK) != 0,
          "rename");
    menu(5);                                         /* X menu: Delete -> "Delete b.txt?" */
    frames(2);
    check(nzui_active() && access("/tmp/fake_sdcard/music/b.txt", F_OK) == 0, "delete asks first");
    tap(13);                                         /* A: yes */
    frames(3);
    check(access("/tmp/fake_sdcard/music/b.txt", F_OK) != 0, "and deletes after A (yes)");
    tap(14); tap(14);                                /* B: up to the SD card, B: leave */
    frames(3);
    check(apps_focused() == NULL && selected_tile() == 1, "B at the top of the SD card goes back home");

    pdf_reader_test();

    /* ---- the Images app ---- */
    static const char *const pic_names[] = { "tb.png", "tb.jpg", "tb.bmp" };
    for (int i = 0; i < 3; i++) {
        char p[256], what[120];
        snprintf(p, sizeof p, "/tmp/fake_sdcard/pictures/%s", pic_names[i]);
        app_images_open(p);
        frames(3);
        snprintf(what, sizeof what, "Images shows %s the right way up (red top, blue bottom)", pic_names[i]);
        check(app_images_shown_w() == 64 && is_red(frame[232 * NZ_W + 320]) && is_blue(frame[252 * NZ_W + 320]), what);
    }
    save("core_10_image");
    app_images_open("/tmp/fake_sdcard/pictures/big.jpg");
    frames(3);
    check(app_images_shown_w() > 500 && app_images_shown_w() <= 640 && is_red(frame[120 * NZ_W + 320]) &&
          is_blue(frame[360 * NZ_W + 320]), "a 12-megapixel camera JPG is shrunk to fit the screen");
    tap(14); frames(3);                              /* B: leave (it was opened on a file) */
    check(apps_focused() == NULL, "B leaves the picture viewer");
    ui_open_app(APP_IMAGES); frames(3);
    check(count_pngs("/tmp/fake_sdcard/nzeus/shots") == 2 && app_images_count() == 4 + 2,
          "the Images gallery lists the 4 pictures on the SD card + the 2 screenshots (nzeus/shots)");
    tap(14); frames(3);

    /* ---- Wi-Fi (a fake dongle and wlan0 in /tmp/fake_sys, a fake wifi-scan.sh) ---- */
    static const char survey[] =
        "index  bssid              ch  RSSI  SdBm  Noise    age                              flag                              ssid\n"
        "    1  aa:bb:cc:dd:ee:01    6    60   -61     0      120                  [WPA2][WPS][ESS]                      My Home WiFi\n"
        "    2  aa:bb:cc:dd:ee:02   11    80   -48     0      120                       [WPA2][ESS]                           Office\n"
        "    3  aa:bb:cc:dd:ee:03    1    30   -82     0      120                             [ESS]                        CafeFree\n"
        "    4  aa:bb:cc:dd:ee:04   36    70   -55     0      120                  [WPA2][WPS][ESS]                      My Home WiFi\n";
    WifiNet nets[8];
    int nn = wifi_parse_survey(survey, nets, 8);
    check(nn == 3 && !strcmp(nets[0].ssid, "Office") && !strcmp(nets[1].ssid, "My Home WiFi") && nets[1].dbm == -55 &&
          nets[1].secure && !nets[2].secure, "Wi-Fi scan list: names with spaces, strongest first, duplicates merged");
    if (system("rm -rf /tmp/fake_sys && mkdir -p /tmp/fake_sys/sys/bus/usb/devices/1-1 /tmp/fake_sys/sys/class/net "
               "/tmp/fake_sdcard/nzeus/bin && "
               "echo 0bda > /tmp/fake_sys/sys/bus/usb/devices/1-1/idVendor && "
               "echo c820 > /tmp/fake_sys/sys/bus/usb/devices/1-1/idProduct") != 0) printf("FAIL  could not make the fake Wi-Fi\n");
    FILE *js = fopen("/tmp/fake_sdcard/nzeus/bin/wifi-join.sh", "w");   /* a fake wifi-join.sh: notes its arguments */
    if (js) { fputs("if [ \"$1\" = connect ]; then printf '%s|%s|%s' \"$1\" \"$2\" \"$3\" > /tmp/fake_join_args; fi\n", js); fclose(js); }
    wifi_sys_root = "/tmp/fake_sys";
    /* the driver (rtw88) is in the kernel: "loaded" = wlan0 is there */
    check(wifi_dongle() == DONGLE_WIFI && !wifi_driver_loaded() && wifi_state() == WIFI_OFF,
          "Wi-Fi: the dongle is seen (USB 0bda:c820); no wlan0 yet = no driver, Wi-Fi off");
    if (system("mkdir -p /tmp/fake_sys/sys/class/net/wlan0 && echo 0x1002 > /tmp/fake_sys/sys/class/net/wlan0/flags") != 0) {}
    check(wifi_driver_loaded() && wifi_state() == WIFI_OFF, "Wi-Fi: wlan0 there (switched off) = the driver is loaded, Wi-Fi off");
    if (system("echo 0x1003 > /tmp/fake_sys/sys/class/net/wlan0/flags") != 0) {}
    {   /* Settings > Storage > Card health: a read-only Linux root or /data (after a file system error) is noticed */
        extern const char *settings_mounts_file;
        int settings_card_readonly(void);
        FILE *mf = fopen("/tmp/fake_mounts", "w");
        if (mf) { fputs("/dev/mmcblk0p3 / ext4 rw,relatime 0 0\n/dev/mmcblk0p4 /data ext4 ro,relatime 0 0\n", mf); fclose(mf); }
        settings_mounts_file = "/tmp/fake_mounts";
        int ro = settings_card_readonly();
        mf = fopen("/tmp/fake_mounts", "w");
        if (mf) { fputs("rootfs / rootfs ro 0 0\n/dev/mmcblk0p3 / ext4 rw,relatime 0 0\n/dev/mmcblk0p4 /data ext4 rw 0 0\n"
                        "/dev/mmcblk0p1 /mnt/stock vfat ro,relatime 0 0\n/dev/sda1 /media/ro vfat ro 0 0\n", mf); fclose(mf); }
        int rw = !settings_card_readonly();
        mf = fopen("/tmp/fake_mounts", "w");
        if (mf) { fputs("/dev/mmcblk0p3 / ext4 ro,relatime 0 0\n", mf); fclose(mf); }
        check(ro && rw && settings_card_readonly(),
              "Settings: a read-only / or /data is noticed (the read-only stock FAT, USB sticks and initramfs don't count)");
        settings_mounts_file = "/proc/mounts";
    }
    /* the scan: nzeus/bin/wifi-scan.sh FILE (iw on the console) writes the list, then renames it into place */
    FILE *sc = fopen("/tmp/fake_survey.txt", "w");
    if (sc) { fputs(survey, sc); fclose(sc); }
    js = fopen("/tmp/fake_sdcard/nzeus/bin/wifi-scan.sh", "w");
    if (js) { fputs("echo \"$1\" > /tmp/fake_scan_args; sleep 0.4; cp /tmp/fake_survey.txt \"$1.tmp\" && mv \"$1.tmp\" \"$1\"\n", js); fclose(js); }
    unlink("/tmp/fake_scan_args");
    wifi_scan_wait_ms = 25000;                       /* (the console's: at most 25 s - it is read as soon as it is there) */
    ui_open_app(APP_WIFI); frames(3);
    check(apps_focused() && apps_focused()->id == APP_WIFI && wifi_state() == WIFI_ON, "the Wi-Fi app sees the dongle and wlan0 on");
    tap(13); frames(2);                              /* A: Scan for networks */
    for (int i = 0; i < 20 && access("/tmp/fake_scan_args", F_OK) != 0; i++) frames(1);
    check(file_contains("/tmp/fake_scan_args", "/tmp/nzeus-wifi-survey.txt") && app_wifi_network_count() == 0,
          "Scan starts wifi-scan.sh with its list's file (/tmp/nzeus-wifi-survey.txt)");
    for (int i = 0; i < 150 && app_wifi_network_count() == 0; i++) frames(1);
    check(app_wifi_network_count() == 3, "as soon as wifi-scan.sh's list is there the networks are listed (no 25 s wait)");
    save("core_11_wifi");
    /* X: a network's ID card (wifiinfo.c); any button closes it */
    {
        const char *app_wifi_card_text(void);
        tap(3); tap(12); frames(2);                      /* DOWN: "Office", X */
        check(strstr(app_wifi_card_text(), "Name      Office") && strstr(app_wifi_card_text(), "Security  WPA2 - good") &&
              strstr(app_wifi_card_text(), "WPS       off") && strstr(app_wifi_card_text(), "quietest 2.4 GHz channel"),
              "Wi-Fi: X shows the network's ID card (security in words, WPS, best channel)");
        tap(14); frames(2);                              /* B: closes the card, not the app */
        check(!app_wifi_card_text()[0] && apps_focused() && apps_focused()->id == APP_WIFI, "Wi-Fi: B closes the ID card (the app stays)");
        tap(3); tap(12); frames(2);                      /* DOWN: "My Home WiFi" (on 2 bands), X */
        check(strstr(app_wifi_card_text(), "Band      5 GHz, channel 36") && strstr(app_wifi_card_text(), "Same router  My Home WiFi (ch 6)") &&
              strstr(app_wifi_card_text(), "WPS       on"),
              "Wi-Fi: the card shows the stronger band and the same router's other band");
        save("core_11b_wifi_card");
        tap(13); frames(2);                              /* any button closes it */
        tap(2); tap(2);                                  /* back up to "Scan for networks" */
        check(!app_wifi_card_text()[0], "Wi-Fi: any button closes the ID card");
    }
    /* a join that fails: the spinner turns while it runs, then goes, with a message */
    js = fopen("/tmp/fake_sdcard/nzeus/bin/wifi-join.sh", "w");
    if (js) { fputs("if [ \"$1\" = connect ]; then sleep 0.5; exit 1; fi\n", js); fclose(js); }
    tap(3); tap(3); tap(3); tap(13); frames(2);      /* DOWN x3: "CafeFree" (open: no password), A */
    int spun = app_wifi_spinner() != NULL && !strcmp(app_wifi_detail(), "Joining CafeFree...");
    for (int i = 0; i < 150 && app_wifi_spinner(); i++) frames(1);
    check(spun && !app_wifi_spinner() && !strcmp(ui_toast_text(), "Could not join CafeFree"),
          "Wi-Fi: a spinner turns while joining; a failed join removes it and says so");
    tap(2); tap(2); tap(2);                          /* back up to "Scan for networks" */
    js = fopen("/tmp/fake_sdcard/nzeus/bin/wifi-join.sh", "w");   /* the fake that notes its arguments again */
    if (js) { fputs("if [ \"$1\" = connect ]; then printf '%s|%s|%s' \"$1\" \"$2\" \"$3\" > /tmp/fake_join_args; fi\n", js); fclose(js); }
    tap(3); tap(3); tap(13); frames(2);              /* DOWN x2: "My Home WiFi" (needs a password), A */
    check(nzui_active(), "a network with a password asks for it");
    type_line("it's-secret"); frames(10);
    check(file_contains("/tmp/fake_join_args", "connect|My Home WiFi|it's-secret"),
          "joining passes the name and password safely (spaces, quote) to wifi-join.sh");
    check(app_wifi_spinner() != NULL && !strcmp(app_wifi_detail(), "Joining My Home WiFi..."),
          "Wi-Fi: while joining, a small spinner turns next to \"Joining My Home WiFi...\"");
    save("core_11c_wifi_joining");
    theme_set(TH_GREEN_CRT); frames(3); save("core_11c_wifi_joining_crt"); theme_set(TH_ORIGINAL); frames(2);
    /* joined: nzeus-net-up.sh (DHCP) starts by itself and the app shows the address */
    js = fopen("/tmp/fake_sdcard/nzeus/bin/wifi-join.sh", "w");
    if (js) { fputs("[ \"$1\" = status ] && echo 'state=COMPLETED ssid=My Home WiFi bssid=aa:bb freq=2437'\n", js); fclose(js); }
    js = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-net-up.sh", "w");
    if (js) { fputs("touch /tmp/fake_netup\n", js); fclose(js); }
    js = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-netctl", "w");
    if (js) { fputs("#!/bin/sh\n[ -e /tmp/fake_netup ] && echo 'iface=wlan0 link=up ip=192.168.1.57' || echo 'daemon=none link=down ip=none'\n", js); fclose(js); }   /* exactly what the real nzeus-netctl prints */
    if (system("rm -f /tmp/fake_netup; chmod +x /tmp/fake_sdcard/nzeus/bin/nzeus-netctl") != 0) {}
    for (int i = 0; i < 400 && !wifi_ip()[0]; i++) frames(1);
    check(access("/tmp/fake_netup", F_OK) == 0 && !strcmp(wifi_ip(), "192.168.1.57"),
          "after joining, nzeus-net-up.sh (DHCP) starts by itself and the address is shown");
    frames(20);
    check(!app_wifi_spinner() && !strncmp(app_wifi_detail(), "Joined My Home WiFi", 19),
          "Wi-Fi: once joined, the spinner is gone (deleted) and the card says Joined");
    unlink("/tmp/fake_netup");
    for (int i = 0; i < 300 && wifi_ip()[0]; i++) frames(1);
    check(!wifi_ip()[0], "Wi-Fi: when nzeus-netctl says ip=none the address goes away");
    if (system("touch /tmp/fake_netup") != 0) {}         /* (the address back for the tests below) */
    for (int i = 0; i < 300 && !wifi_ip()[0]; i++) frames(1);
    tap(14); frames(3);

    /* ---- saved Wi-Fi networks: joined without a password, Y forgets (2026-10-05) ---- */
    {
        /* networks.conf as wifi-join.sh writes it: names in hex ("My Home WiFi", "Far Away") */
        FILE *nc = fopen("/tmp/fake_sdcard/nzeus/wifi/networks.conf", "w");
        if (!nc && system("mkdir -p /tmp/fake_sdcard/nzeus/wifi") == 0) nc = fopen("/tmp/fake_sdcard/nzeus/wifi/networks.conf", "w");
        if (nc) {
            fputs("# nzeus ssid: My Home WiFi\nnetwork={\n\tssid=4d7920486f6d652057694669\n\tkey_mgmt=WPA-PSK\n\tpsk=00\n}\n"
                  "# nzeus ssid: Far Away\nnetwork={\n\tssid=4661722041776179\n\tkey_mgmt=WPA-PSK\n\tpsk=11\n}\n", nc);
            fclose(nc);
        }
        char sv[8][33];
        int ns = wifi_saved_list(sv, 8);
        check(ns == 2 && !strcmp(sv[0], "My Home WiFi") && !strcmp(sv[1], "Far Away") && wifi_is_saved("Far Away") && !wifi_is_saved("Office"),
              "Saved Wi-Fi: the names are read back from networks.conf (hex)");
        /* the fake wifi-join.sh: still joined; notes use/forget */
        js = fopen("/tmp/fake_sdcard/nzeus/bin/wifi-join.sh", "w");
        if (js) {
            fputs("[ \"$1\" = status ] && echo 'state=COMPLETED ssid=My Home WiFi bssid=aa:bb freq=2437'\n"
                  "case \"$1\" in use|forget|connect) printf '%s|%s\\n' \"$1\" \"$2\" >> /tmp/fake_join_calls ;; esac\n", js);
            fclose(js);
        }
        if (system("rm -f /tmp/fake_join_calls") != 0) {}
        ui_open_app(APP_WIFI); frames(3);
        tap(13); frames(2);                           /* A: Scan (the fake wifi-scan.sh answers again) */
        for (int i = 0; i < 150 && app_wifi_network_count() == 0; i++) frames(1);
        frames(5);
        save("core_11b_wifi_saved");
        /* rows: Scan, Disconnect, Office, My Home WiFi (saved), CafeFree, Far Away (saved, not seen) */
        tap(3); tap(3); tap(3); tap(13); frames(10);  /* DOWN x3: My Home WiFi, A */
        check(!nzui_active() && file_contains("/tmp/fake_join_calls", "use|My Home WiFi"),
              "Saved Wi-Fi: a saved network joins with its saved key (no password question)");
        tap(3); tap(3); tap(15); frames(2);           /* DOWN x2: Far Away, Y: forget */
        check(nzui_active(), "Saved Wi-Fi: Y on a saved network asks first");
        tap(13); frames(10);                          /* A: yes */
        check(file_contains("/tmp/fake_join_calls", "forget|Far Away"), "Saved Wi-Fi: yes -> wifi-join.sh forget");
        for (int i = 0; i < 8; i++) tap(2);           /* UP to the top (Scan), */
        tap(3); tap(3); tap(15); frames(2);           /* DOWN x2: Office (not saved), Y */
        check(!nzui_active() && !strcmp(ui_toast_text(), "Pick a saved network to forget it"),
              "Saved Wi-Fi: Y on a network that isn't saved only explains");
        tap(14); frames(3);

        /* Settings > Wi-Fi: status, join by itself, saved count; "Wi-Fi networks" opens the app */
        extern const char *wifi_autooff_file;
        wifi_autooff_file = "/tmp/fake_sdcard/nzeus/wifi/autojoin-off";
        unlink(wifi_autooff_file);
        ui_open_app(APP_SETTINGS); frames(3);
        for (int i = 0; i < 7; i++) tap(3);          /* DOWN x7: Wi-Fi */
        for (int i = 0; i < 200 && !strstr(app_settings_row_value(1), "192.168"); i++) frames(1);
        check(app_settings_section() == 7 && !strcmp(app_settings_row_value(1), "My Home WiFi - 192.168.1.57") &&
              strstr(app_settings_row_value(2), "ON") && !strcmp(app_settings_row_value(3), "2 saved"),
              "Settings > Wi-Fi: joined network + address, 'Join saved networks by itself' ON, '2 saved'");
        save("core_wifi_settings");
        tap(13); tap(3); tap(3); tap(5); frames(2);   /* A: rows, DOWN x2: join by itself, RIGHT */
        check(access(wifi_autooff_file, F_OK) == 0 && strstr(app_settings_row_value(2), "off"),
              "Settings > Wi-Fi: switching 'join by itself' off writes nzeus/wifi/autojoin-off (wifi-auto.sh reads it)");
        tap(5); frames(2);
        check(access(wifi_autooff_file, F_OK) != 0 && strstr(app_settings_row_value(2), "ON"), "Settings > Wi-Fi: ...and ON again deletes it");
        tap(2); tap(2); tap(13); frames(3);           /* UP x2: Wi-Fi networks, A */
        check(apps_focused() && apps_focused()->id == APP_WIFI, "Settings > Wi-Fi > Wi-Fi networks opens the network list");
        tap(14); frames(3);
        check(apps_focused() && apps_focused()->id == APP_SETTINGS, "B in the network list goes back to Settings");
        /* Settings > Storage: the card's Linux data, memory, card health (no FAT repair, no
         * "Fast SD card" and no "More space" on this system), compressed memory */
        {
            tap(14); for (int i = 0; i < 2; i++) tap(2);   /* B: sections, UP x2: Storage */
            extern const char *settings_mounts_file;
            settings_mounts_file = "/tmp/fake_mounts";
            FILE *mf = fopen("/tmp/fake_mounts", "w");
            if (mf) { fputs("/dev/mmcblk0p3 / ext4 rw,relatime 0 0\n/dev/mmcblk0p4 /data ext4 rw,relatime 0 0\n", mf); fclose(mf); }
            /* Compressed memory (zram): a switch (nzeus/zram-off there = off) + a note row */
            extern const char *zram_off_file, *zram_swaps_file, *zram_stat_file;
            zram_off_file = "/tmp/fake_sdcard/nzeus/zram-off";
            zram_swaps_file = "/tmp/fake_swaps"; zram_stat_file = "/tmp/fake_zram_stat";
            unlink(zram_off_file);
            FILE *zf = fopen("/tmp/fake_swaps", "w");
            if (zf) { fputs("Filename Type Size Used Priority\n", zf); fclose(zf); }
            tap(13); frames(2);                            /* A: rows */
            check(app_settings_section() == 5 && popup_label(lv_scr_act(), "Card health", 12) &&
                  popup_label(lv_scr_act(), "SD card (Linux data)", 12) && !popup_label(lv_scr_act(), "Repair", 12) &&
                  !popup_label(lv_scr_act(), "Check the card", 12) && !popup_label(lv_scr_act(), "More space", 12) &&
                  !popup_label(lv_scr_act(), "Fast SD", 12),
                  "Settings > Storage: the card's Linux data + card health; no FAT check/repair, 'More space' or 'Fast SD card'");
            for (int i = 0; i < 150 && strcmp(app_settings_row_value(2), "OK (writable)"); i++) frames(1);
            check(!strcmp(app_settings_row_value(2), "OK (writable)"), "Settings > Storage: card health OK when / and /data are writable");
            mf = fopen("/tmp/fake_mounts", "w");
            if (mf) { fputs("/dev/mmcblk0p3 / ext4 rw,relatime 0 0\n/dev/mmcblk0p4 /data ext4 ro,relatime 0 0\n", mf); fclose(mf); }
            for (int i = 0; i < 150 && strncmp(app_settings_row_value(2), "READ-ONLY", 9); i++) frames(1);
            check(!strcmp(app_settings_row_value(2), "READ-ONLY - restart to repair"),
                  "Settings > Storage: a read-only /data says a restart repairs it (Debian checks the card at start)");
            settings_mounts_file = "/proc/mounts";
            for (int i = 0; i < 150 && strcmp(app_settings_row_value(4), "Starts at the next power-on"); i++) frames(1);
            check(strstr(app_settings_row_value(3), " ON ") && !strcmp(app_settings_row_value(4), "Starts at the next power-on"),
                  "Settings > Storage: 'Compressed memory (zram)' ON by default; not running yet = starts at the next power-on");
            zf = fopen("/tmp/fake_swaps", "a");
            if (zf) { fputs("/dev/zram0 partition 16380 4096 100\n", zf); fclose(zf); }
            zf = fopen("/tmp/fake_zram_stat", "w");
            if (zf) { fputs("4194304 1258291 1400832 0 1400832 0 0\n", zf); fclose(zf); }
            for (int i = 0; i < 150 && strncmp(app_settings_row_value(4), "On:", 3); i++) frames(1);   /* (the note refreshes every second) */
            check(!strcmp(app_settings_row_value(4), "On: 4.0 MB squeezed into 1.3 MB of RAM"),
                  "Settings > Storage: zram running -> how much it holds and the RAM it costs");
            save("core_settings_storage");
            for (int i = 0; i < 3; i++) tap(3);            /* DOWN x3: the zram switch */
            tap(5); frames(2);                             /* RIGHT: off */
            check(access(zram_off_file, F_OK) == 0 && strstr(app_settings_row_value(3), " off ") &&
                  strstr(app_settings_row_value(4), "Squeezes memory so more fits"),
                  "Settings > Storage: zram off writes nzeus/zram-off (nzeus-deck switches it off), the note says what it does");
            tap(5); frames(2);                             /* RIGHT: on again */
            check(access(zram_off_file, F_OK) != 0 && strstr(app_settings_row_value(3), " ON "),
                  "Settings > Storage: zram ON again deletes nzeus/zram-off");
            zram_swaps_file = "/proc/swaps";
            tap(14);                                       /* B: the sections */
        }
        /* all 13 sections: the last one (Power) must be fully on the screen when chosen */
        for (int i = 0; i < 7; i++) tap(3);            /* DOWN x7: Power (the last) */
        frames(2);
        { int app_settings_section_on_screen(int i);
          check(app_settings_section() == 12 && app_settings_section_on_screen(12) && app_settings_section_on_screen(0),
                "Settings: with 13 sections the last one (Power) is on the screen too"); }
        save("core_settings_power_visible");
        /* Power: restart NZEUS, restart the console, power off - no Safe mode on this system */
        {
            tap(13); frames(2);                            /* A: rows */
            check(popup_label(lv_scr_act(), "Restart NZEUS Deck", 12) && popup_label(lv_scr_act(), "Power off", 12) &&
                  !popup_label(lv_scr_act(), "Safe mode", 12), "Settings > Power: Restart NZEUS Deck, Restart the console, Power off (no Safe mode)");
            tap(3); tap(13); frames(2);                    /* DOWN: Restart the console, A */
            check(nzui_active(), "Settings > Power > Restart the console: asks first");
            tap(13); frames(3);                            /* A: yes */
            check(!nzui_active() && !strcmp(ui_toast_text(), "Reboot is not possible here"),
                  "Settings > Power > Restart the console: yes -> says so when it can't (the PC; a kernel without the watchdog restart)");
            int offs = hw_test_power_offs;
            tap(3); tap(13); frames(2);                    /* DOWN: Power off, A */
            int asked = nzui_active() && hw_test_power_offs == offs;
            tap(13); frames(3);                            /* A: yes */
            check(asked && hw_test_power_offs == offs + 1 && !strcmp(ui_toast_text(), "Power off is not possible here"),
                  "Settings > Power > Power off: asks first, then switches off (a stub here, so it says it could not)");
            save("core_settings_power");
            tap(14); frames(2);                            /* B: back to the sections */
        }
        /* Sound: the volume (the ALSA card) and the speaker switch (the sound driver's "headphones") */
        {
            for (int i = 0; i < 11; i++) tap(2);           /* UP x11: Sound */
            tap(13); frames(2);                            /* A: rows */
            for (int i = 0; i < 100 && strcmp(app_settings_row_value(0), "< no sound card >"); i++) frames(1);
            check(app_settings_section() == 1 && strstr(app_settings_row_value(0), "no sound card") &&
                  strstr(app_settings_row_value(1), "always on"),
                  "Settings > Sound: no sound card = says so; no speaker switch = 'always on'");
            tap(5); frames(2);                             /* RIGHT on Volume */
            check(!strcmp(ui_toast_text(), "Volume: no sound device here"), "Settings > Sound: Volume without a card says so");
            tap(3); tap(5); frames(2);                     /* DOWN: Speaker, RIGHT */
            check(!strcmp(ui_toast_text(), "Speaker: no switch on this system"), "Settings > Sound: Speaker without the driver's switch says so");
            hw_headphones_file = "/tmp/fake_headphones";
            FILE *hp = fopen(hw_headphones_file, "w");
            if (hp) { fputs("level=1 plugged_level=0 in=0 amp=1\n", hp); fclose(hp); }
            for (int i = 0; i < 100 && !strstr(app_settings_row_value(1), "On"); i++) frames(1);
            check(strstr(app_settings_row_value(1), "On") != NULL, "Settings > Sound: Speaker On (the driver: no headphones, in=0)");
            tap(5); frames(2);                             /* RIGHT: speaker off */
            char hpv[64] = "";
            file_lines(hw_headphones_file, 0, hpv, sizeof hpv);
            check(!strcmp(hpv, "1") && file_contains(NZ_SDCARD "/nzeus/settings.ini", "speaker=0"),
                  "Settings > Sound: Speaker off writes the pin's level (1) to the driver = 'headphones in', and is saved");
            hp = fopen(hw_headphones_file, "w");           /* (what the driver then says) */
            if (hp) { fputs("level=1 plugged_level=1 in=1 amp=0\n", hp); fclose(hp); }
            for (int i = 0; i < 100 && !strstr(app_settings_row_value(1), "Off (headphones only)"); i++) frames(1);
            check(strstr(app_settings_row_value(1), "Off (headphones only)") != NULL, "Settings > Sound: then it shows 'Off (headphones only)'");
            save("core_settings_speaker_off");
            tap(5); frames(2);                             /* RIGHT: speaker on again */
            file_lines(hw_headphones_file, 0, hpv, sizeof hpv);
            check(!strcmp(hpv, "off") && file_contains(NZ_SDCARD "/nzeus/settings.ini", "speaker=1"),
                  "Settings > Sound: Speaker on writes 'off' (the speaker as usual), and is saved");
            hw_headphones_file = "/tmp/no_such_headphones";
            tap(14);                                       /* B: the sections */
        }
        /* Controls: pin L12 = the Vol- button or the debug serial line (nzeus-voldown) */
        {
            extern const char *voldown_sys_file, *voldown_cmd;
            voldown_sys_file = "/tmp/fake_voldown";
            voldown_cmd = "/tmp/fake_nzeus-voldown";
            unlink(voldown_sys_file); unlink("/tmp/fake_voldown_calls");
            tap(3); tap(3); tap(3); tap(13); frames(2);    /* DOWN x3: Controls, A: rows */
            for (int i = 0; i < 3; i++) tap(3);            /* DOWN x3: Vol- / serial pin */
            frames(2);
            check(app_settings_section() == 4 && strstr(app_settings_row_value(3), "Not here"),
                  "Settings > Controls: 'Vol- / serial pin' says 'Not here' without the power driver's voldown file");
            tap(5); frames(2);
            check(!strcmp(ui_toast_text(), "Not possible on this system"), "Settings > Controls: switching it there is refused plainly");
            FILE *vf = fopen(voldown_sys_file, "w");
            if (vf) { fputs("key\n", vf); fclose(vf); }
            vf = fopen(voldown_cmd, "w");                  /* the fake nzeus-voldown: notes the call, then switches */
            if (vf) { fputs("#!/bin/sh\necho \"$1\" >> /tmp/fake_voldown_calls\necho \"$1\" > /tmp/fake_voldown\n", vf); fclose(vf); }
            chmod(voldown_cmd, 0755);
            for (int i = 0; i < 100 && !strstr(app_settings_row_value(3), "Vol- button"); i++) frames(1);
            check(strstr(app_settings_row_value(3), "Vol- button") != NULL, "Settings > Controls: pin L12 = 'Vol- button' (the driver says key)");
            tap(5); frames(5);                             /* RIGHT: the serial line */
            for (int i = 0; i < 100 && !strstr(app_settings_row_value(3), "Debug serial"); i++) frames(1);
            check(file_contains("/tmp/fake_voldown_calls", "uart") && !strcmp(ui_toast_text(), "Pin L12 is the debug serial line now") &&
                  strstr(app_settings_row_value(3), "Debug serial line"),
                  "Settings > Controls: switching runs 'nzeus-voldown uart' and says what the pin is now");
            save("core_settings_voldown");
            tap(5); frames(5);                             /* RIGHT: the button again */
            check(file_contains("/tmp/fake_voldown_calls", "key") && !strcmp(ui_toast_text(), "The Vol- button works now"),
                  "Settings > Controls: ...and 'nzeus-voldown key' gives the Vol- button back");
            voldown_sys_file = "/tmp/no_such_voldown";
            tap(14); frames(2);                            /* B: the sections */
        }
        tap(14); frames(3);
        /* home: no Wi-Fi tile any more; its place is "Tools" (the same list as Y) */
        int home_was = home_selected();               /* (put back at the end: later tests count from it) */
        for (int i = 0; i < 3; i++) { home_key(NZ_KEY_UP); home_key(NZ_KEY_LEFT); }
        home_key(NZ_KEY_DOWN); home_key(NZ_KEY_DOWN); home_key(NZ_KEY_RIGHT); home_key(NZ_KEY_RIGHT);
        home_key(NZ_KEY_A); frames(3);
        check(home_selected() == 10 && nzui_active() && !apps_focused(), "Home: the tile where Wi-Fi was opens the Tools list");
        save("core_home_tools_menu");
        tap(14); frames(3);
        home_key(NZ_KEY_START); frames(3);
        check(home_selected() == 11 && apps_focused() && apps_focused()->id == APP_SETTINGS, "Home: START still opens Settings (its tile is selected)");
        tap(14); tap(14); frames(3);
        for (int i = 0; i < 3; i++) { home_key(NZ_KEY_UP); home_key(NZ_KEY_LEFT); }   /* back to where it was */
        for (int i = 0; i < home_was / 4; i++) home_key(NZ_KEY_DOWN);
        for (int i = 0; i < home_was % 4; i++) home_key(NZ_KEY_RIGHT);
        check(home_selected() == home_was, "Home: (test set-up) the selection is back where it was");
    }

    /* ---- battery % and brightness: the console's own devices (TreeFrogUI's formulas, 2026-10-06) ---- */
    {
        check(hw_backlight_value(0) == 23 && hw_backlight_value(10) == 33 && hw_backlight_value(42) == 65 &&
              hw_backlight_value(43) == 66 && hw_backlight_value(100) == 255 && hw_backlight_value(150) == 255,
              "Brightness: 0..100 % becomes the driver's level 23..255 (10 % is dim, not black)");
        /* measured 2026-10-06: it died at 128 (the stock formula said ~35 %) */
        check(hw_battery_from_adc(-1) == -1 && hw_battery_from_adc(60) == 0 && hw_battery_from_adc(128) == 0 &&
              hw_battery_from_adc(129) == 2 && hw_battery_from_adc(140) == 25 && hw_battery_from_adc(151) == 48 &&
              hw_battery_from_adc(166) == 80 && hw_battery_from_adc(174) == 97 && hw_battery_from_adc(175) == 100 &&
              hw_battery_from_adc(255) == 100,
              "Battery: 128 is 0 %, 175 is 100 %, a straight line between (140 -> 25 %, 166 -> 80 %)");
        /* the power driver's files (fake): levels "ref=.. battery=.. charger=..", Linux's capacity + online */
        write_levels(151, 128);
        check(hw_battery_raw() == 151 && hw_battery_percent() == 48 && hw_battery_charging() == 1,
              "Battery: the power driver's stock_levels (battery=151 -> 48 %, charger=128 -> charging)");
        write_levels(151, 10);
        check(hw_battery_charging() == 0, "Battery: a low charger reading = not charging");
        write_levels(60, 10);
        check(hw_battery_raw() == -1 && hw_battery_percent() == -1,
              "Battery: a reading under 100 is taken as a bad reading (unknown), never as 'empty'");
        FILE *bf = fopen(hw_capacity_file, "w"); if (bf) { fputs("87\n", bf); fclose(bf); }
        bf = fopen(hw_charger_file, "w"); if (bf) { fputs("1\n", bf); fclose(bf); }
        check(hw_battery_percent() == 87 && hw_battery_charging() == 1,
              "Battery: Linux's own capacity (87 %) and charger/online win when the driver gives them");
        unlink(hw_capacity_file); unlink(hw_charger_file); unlink(FAKE_LEVELS);
        check(hw_battery_percent() == -1 && hw_battery_charging() == 0, "Battery: no driver = unknown, not charging");
        /* brightness: r36s-avp takes the 0..255 level as it is; otherwise another backlight, scaled */
        check(hw_set_brightness(100) == -1, "Brightness: no backlight device = -1 (Settings says so)");
        if (system("mkdir -p /tmp/fake_backlight/r36s-avp /tmp/fake_backlight/r36s-pwm && "
                   "touch /tmp/fake_backlight/r36s-avp/brightness /tmp/fake_backlight/r36s-pwm/brightness && "
                   "echo 100 > /tmp/fake_backlight/r36s-pwm/max_brightness") != 0) {}
        char lv[32] = "";
        check(hw_set_brightness(100) == 0 && file_lines(hw_backlight_file, 0, lv, sizeof lv) == 1 && !strcmp(lv, "255"),
              "Brightness: 100 % writes 255 to /sys/class/backlight/r36s-avp/brightness");
        if (system("rm -rf /tmp/fake_backlight/r36s-avp") != 0) {}
        check(hw_set_brightness(50) == 0 && file_lines("/tmp/fake_backlight/r36s-pwm/brightness", 0, lv, sizeof lv) == 1 &&
              !strcmp(lv, "31"), "Brightness: without r36s-avp the native backlight gets the same level scaled (50 % = 81/255 -> 31/100)");
        if (system("rm -rf /tmp/fake_backlight/r36s-pwm") != 0) {}

        /* the core's battery watch (core.c): a reading every 10 s (the pretend clock), a minute averaged */
        {
            time_t clock0 = hw_now(), real0 = time(NULL);
            battery_reset();
            batt_clock = clock0;
            ui_toast("-");
            battery_reading(133, 0);                     /* 133 = 10 %, unplugged */
            check(!strcmp(ui_toast_text(), "Battery low (10%) - plug in the charger"),
                  "Battery low: at 15 % or less (here 10 %) a message: plug in the charger");
            frames(2); save("core_battery_low");
            theme_set(TH_GREEN_CRT); frames(3); save("core_battery_low_crt"); theme_set(TH_ORIGINAL); frames(2);
            ui_toast("-");
            for (int i = 0; i < 5; i++) battery_reading(133, 0);   /* 50 s later: not again yet */
            int quiet = !strcmp(ui_toast_text(), "-");
            batt_clock += 300;
            battery_reading(133, 0);                     /* 5 minutes later: again */
            check(quiet && !strcmp(ui_toast_text(), "Battery low (10%) - plug in the charger"),
                  "Battery low: said again at most every 5 minutes");
            battery_reset();
            ui_toast("-");
            for (int i = 0; i < 3; i++) battery_reading(133, 130);  /* the same, charger plugged in */
            check(!strcmp(ui_toast_text(), "-"), "Battery low: no message while the charger is plugged in");

            /* empty: a minute at 130 or below -> the message, then the safe power-off (it does nothing here) */
            battery_reset();
            int offs = hw_test_power_offs;
            battery_off_delay_ms = 1500;
            for (int i = 0; i < 5; i++) battery_reading(130, 130);   /* 50 s: not a whole minute yet */
            int early = battery_off_screen() != NULL;
            battery_reading(130, 130);                   /* the 6th reading: a minute at 130, charger or not */
            int shown = battery_off_screen() != NULL, not_yet = hw_test_power_offs == offs;
            frames(2); save("core_battery_empty");
            theme_set(TH_AMBER_CRT); frames(3); save("core_battery_empty_crt"); theme_set(TH_ORIGINAL); frames(2);
            for (int i = 0; i < 200 && hw_test_power_offs == offs; i++) frames(1);
            check(!early && shown && not_yet && hw_test_power_offs == offs + 1,
                  "Battery empty: a minute at 130 shows \"Battery empty\", then the safe power-off is called (a stub here)");
            check(!battery_off_screen() && !strncmp(ui_toast_text(), "Could not switch off", 20),
                  "Battery empty: if it did not switch off (the PC), the message goes and says so");
            battery_reset();
            offs = hw_test_power_offs;
            for (int i = 0; i < 8; i++) battery_reading(131, 0);     /* 131: not empty yet */
            frames(10);
            check(!battery_off_screen() && hw_test_power_offs == offs, "Battery empty: NOT at 131");
            battery_reset();
            for (int i = 0; i < 5; i++) battery_reading(140, 0);
            battery_reading(100, 0);                     /* one odd low reading as the 6th */
            int dip_last = battery_off_screen() != NULL;
            battery_reset();
            battery_reading(100, 0);                     /* ... or as the first */
            for (int i = 0; i < 5; i++) battery_reading(140, 0);
            frames(10);
            check(!dip_last && !battery_off_screen() && hw_test_power_offs == offs,
                  "Battery empty: one low reading does not switch off (the minute's average counts)");
            unlink(FAKE_LEVELS);                         /* (no battery again: nothing left for the status bar to read) */
            battery_reset();
            battery_off_delay_ms = 10000;
            hw_test_set_now(clock0 + (time(NULL) - real0));   /* the pretend clock as if nothing happened */
            frames(2);                                   /* (the status bar catches up with the clock) */
        }
        unlink(FAKE_LEVELS);                             /* (no battery again for the other tests) */
        ui_set_charging(1); ui_set_status("12:00", 48, 1); frames(1);
        check(strstr(home_status_text(), "48%") && strstr(home_status_text(), LV_SYMBOL_CHARGE),
              "Status bar: battery 48 % with the charging sign");
        ui_set_charging(0); ui_set_status("12:00", -1, 1); frames(1);
        check(!strstr(home_status_text(), "%"), "Status bar: unknown battery = no number");
    }

    /* ---- Settings > Bluetooth (fake nzeus-bt and nzeus-bt-up.sh; their state in /tmp/fake_bt) ---- */
    {
        #define WAIT_FOR(cond) for (int w_ = 0; w_ < 400 && !(cond); w_++) frames(1)
        enum { P_ROWS, P_MINE, P_FIND, P_PAIR };     /* (app_settings.c's pages) */
        if (system("rm -rf /tmp/fake_bt && mkdir -p /tmp/fake_bt /tmp/fake_sdcard/nzeus/bin && "
                   "printf 'AA:AA:AA:00:00:01\\tOld Mouse\\tmouse\\tclassic\\tnot-connected\\n' > /tmp/fake_bt/list") != 0)
            printf("FAIL  could not make the fake Bluetooth folder\n");
        FILE *bf = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-bt", "w");
        if (bf) {
            fputs("#!/bin/sh\n"
                  "D=/tmp/fake_bt\n"
                  "case \"$1\" in\n"
                  "status) if [ -e $D/on ]; then echo loaded=yes; echo controller=hci0; echo powered=yes;\n"
                  "        else echo loaded=no; echo controller=none; echo powered=no; fi ;;\n"
                  "list) [ -e $D/on ] || { echo 'error Bluetooth is not loaded (run nzeus-bt-up.sh)'; exit 1; }; cat $D/list ;;\n"
                  "off) rm -f $D/on; echo off ;;\n"
                  "scan) echo \"$*\" > $D/scan_args; [ -e $D/slow ] && sleep 1.5\n"
                  "  printf 'DC:2C:26:0A:0B:0C\\tMX Keys\\tkeyboard\\tle\\t-48\\tyes\\n'\n"
                  "  printf 'DC:2C:26:01:02:03\\tKeys-To-Go\\tkeyboard\\tclassic\\t-52\\tyes\\n'\n"
                  "  printf 'AA:AA:AA:00:00:01\\tOld Mouse\\tmouse\\tclassic\\t-60\\tyes\\n'\n"
                  "  printf '11:22:33:44:55:66\\tPixel 7\\tphone\\tdual\\t-70\\tno\\n'\n"
                  "  printf '22:33:44:55:66:77\\tCheap Mouse\\tmouse\\tclassic\\t-75\\tyes\\n' ;;\n"
                  "pair) echo \"$2\" > $D/pair_addr; echo \"pairing $2 (classic)...\"\n"
                  "  case \"$(cat $D/mode)\" in\n"
                  "  passkey) echo 'passkey 123456 - type it on the keyboard, then press Enter'\n"
                  "     i=0; while [ ! -e $D/go ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done\n"
                  "     echo 'typed 6'; echo paired; printf '%s\\tKeys-To-Go\\tkeyboard\\tclassic\\tconnected\\n' \"$2\" >> $D/list; echo connected ;;\n"
                  "  enter) echo 'enter-passkey - type the number the device shows, then Enter'\n"
                  "     read n; echo \"$n\" > $D/got\n"
                  "     if [ \"$n\" = 654321 ]; then echo paired; printf '%s\\tMX Keys\\tkeyboard\\tle\\tconnected\\n' \"$2\" >> $D/list; echo connected\n"
                  "     else echo 'error pairing failed: authentication failed'; exit 1; fi ;;\n"
                  "  *) echo 'error pairing failed: authentication failed'; exit 1 ;;\n"
                  "  esac ;;\n"
                  "survey) echo \"$*\" > $D/survey_args\n"     /* the deck's Bluetooth scanner */
                  "  printf '7A:11:22:33:44:55\\tle-random\\t-50\\t-\\t-\\t-\\t-\\t004c:0719010e20\\t(no name)\\n'\n"
                  "  printf '4F:11:22:33:44:56\\tle-random\\t-70\\t-\\t-\\t-\\t-\\t004c:1219100000000000\\t(no name)\\n'\n"
                  "  printf 'C4:00:00:00:00:05\\tle-random\\t-55\\t-8\\t-\\t03c2\\t1812\\t01da:0102\\tLE Mouse\\n' ;;\n"
                  "connect) echo connected ;;\n"
                  "disconnect) echo disconnected ;;\n"
                  "remove) grep -v \"^$2\" $D/list > $D/l2; mv $D/l2 $D/list; echo removed ;;\n"
                  "esac\n", bf);
            fclose(bf);
        }
        bf = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-bt-up.sh", "w");
        if (bf) {
            fputs("echo 'module ecb: loaded'\nsleep 1\n"
                  "if [ -e /tmp/fake_bt/up_fail ]; then echo 'no hci0: is the BL-WN650BT dongle plugged in? (dmesg shows what btusb saw)'; exit 1; fi\n"
                  "echo \"$NZEUS_DIR\" > /tmp/fake_bt/up_dir; touch /tmp/fake_bt/on; echo 'Bluetooth ready'\n", bf);
            fclose(bf);
        }
        if (system("chmod +x /tmp/fake_sdcard/nzeus/bin/nzeus-bt && touch /tmp/fake_bt/up_fail /tmp/fake_bt/slow") != 0) {}
        BtDev bd;
        check(bt_parse_line("DC:2C:26:01:02:03\t(no name)\tkeyboard\tclassic\t127\tyes", &bd, 1) == 0 &&
              !strcmp(bd.name, "DC:2C:26:01:02:03") && bd.rssi == 0 && bd.supported &&
              bt_parse_line("rm -rf /\tx\tkeyboard\tclassic\t-50\tyes", &bd, 1) != 0,
              "Bluetooth: scan lines are read (no name -> address), bad addresses refused");

        ui_open_app(APP_SETTINGS); frames(3);
        for (int i = 0; i < 8; i++) tap(3);              /* DOWN x8: Bluetooth */
        WAIT_FOR(bt_state() == BT_OFF);
        check(app_settings_section() == 8 && bt_state() == BT_OFF, "Bluetooth: Settings has a Bluetooth section, it says off");
        save("core_bt_1_off");
        tap(13); tap(13); frames(2);                     /* A: into the rows, A: switch on (the dongle is "missing") */
        frames(12);
        check(bt_state() == BT_STARTING && strstr(app_settings_bt_note(), "Starting Bluetooth...  module ecb: loaded"),
              "Bluetooth: On runs nzeus-bt-up.sh in the background (Starting... + its last line)");
        check(access(NZ_SDCARD "/nzeus/bluetooth/on-at-start", F_OK) == 0,
              "Bluetooth: On is remembered (nzeus/bluetooth/on-at-start)");
        save("core_bt_2_starting");
        WAIT_FOR(strstr(app_settings_bt_note(), "Could not switch on") && bt_state() == BT_OFF);
        check(bt_state() == BT_OFF && strstr(app_settings_bt_note(), "Could not switch on: no hci0: is the BL-WN650BT dongle plugged in?"),
              "Bluetooth: switching on failed -> the last line of its log is shown");
    check(file_contains(NZ_SDCARD "/nzeus/tests/bt-up.log", "no hci0") && access(NZ_SDCARD "/nzeus/tests/bt-dmesg.log", F_OK) == 0,
          "Bluetooth: its log and the kernel messages are kept on the SD card (readable after a restart)");
        if (system("rm -f /tmp/fake_bt/up_fail") != 0) {}
        tap(13);                                          /* A: switch on again */
        WAIT_FOR(bt_state() == BT_ON && bt_device_count() == 1);
        check(bt_state() == BT_ON && file_contains("/tmp/fake_bt/up_dir", NZ_SDCARD "/nzeus") && bt_device_count() == 1,
              "Bluetooth: switched on (nzeus-bt-up.sh finds the SD card), the paired device is listed");
        save("core_bt_3_on");
        theme_set(TH_GREEN_CRT); frames(3); save("core_bt_3b_on_crt");
        theme_set(TH_ORIGINAL); frames(2);

        tap(3); tap(3); tap(3); tap(13);                  /* DOWN x3: Find devices..., A */
        check(app_settings_bt_page() == P_FIND && bt_scanning(), "Bluetooth: Find devices starts a search");
        frames(30);
        save("core_bt_4_searching");
        WAIT_FOR(!bt_scanning() && app_settings_bt_rows() == 5);
        check(file_contains("/tmp/fake_bt/scan_args", "scan 10") && bt_found_count() == 4 && app_settings_bt_rows() == 5 &&
              !strcmp(bt_found(0)->name, "MX Keys") && bt_found(2)->supported == 0,
              "Bluetooth: the search (10 s) lists the devices found, strongest first, without the paired one");
        check(app_settings_bt_row_greyed(3) && !app_settings_bt_row_greyed(2) && !app_settings_bt_row_greyed(4),
              "Bluetooth: a device that is not a keyboard/mouse is greyed out");
        save("core_bt_4_found");
        theme_set(TH_AMBER_CRT); frames(3); save("core_bt_4b_found_crt");
        theme_set(TH_ORIGINAL); frames(2);
        tap(3); tap(3); tap(3); tap(13); frames(2);       /* the phone: A */
        check(app_settings_bt_page() == P_FIND && strstr(app_settings_bt_message(), "not a keyboard or mouse"),
              "Bluetooth: A on a greyed device explains it and does not pair");

        /* pairing a keyboard that shows a passkey */
        if (system("echo passkey > /tmp/fake_bt/mode") != 0) {}
        tap(2); tap(13);                                  /* UP: Keys-To-Go, A */
        WAIT_FOR(!strcmp(app_settings_bt_big(), "123456"));
        check(app_settings_bt_page() == P_PAIR && !strcmp(app_settings_bt_big(), "123456") &&
              file_contains("/tmp/fake_bt/pair_addr", "DC:2C:26:01:02:03"),
              "Bluetooth: pairing shows the passkey big (Type this on the keyboard...)");
        save("core_bt_5_passkey");
        theme_set(TH_GREEN_CRT); frames(3);
        save("core_bt_6_passkey_crt");
        theme_set(TH_ORIGINAL); frames(2);
        if (system("touch /tmp/fake_bt/go") != 0) {}
        WAIT_FOR(app_settings_bt_page() == P_MINE && bt_device_count() == 2);
        check(app_settings_bt_page() == P_MINE && !strcmp(app_settings_bt_message(), "Connected - try typing!") &&
              bt_device_count() == 2 && bt_device(1)->connected,
              "Bluetooth: paired + connected -> message, then My devices shows it connected");
        save("core_bt_7_mine");

        /* pairing a keyboard that asks for the number the device shows */
        if (system("echo enter > /tmp/fake_bt/mode; rm -f /tmp/fake_bt/slow") != 0) {}
        tap(14); tap(3); tap(13);                         /* B: rows, DOWN: Find devices..., A */
        WAIT_FOR(!bt_scanning() && app_settings_bt_rows() == 4);
        tap(3); tap(13);                                  /* DOWN: MX Keys, A */
        WAIT_FOR(nzui_active());
        check(app_settings_bt_page() == P_PAIR && nzui_active(), "Bluetooth: enter-passkey asks for the number");
        save("core_bt_8_enter");
        type_line("12x"); frames(3);
        check(nzui_active() && strstr(app_settings_bt_message(), "Only the digits"), "Bluetooth: the number must be digits");
        type_line("654321");
        WAIT_FOR(app_settings_bt_page() == P_MINE && bt_device_count() == 3);
        check(file_contains("/tmp/fake_bt/got", "654321") && app_settings_bt_page() == P_MINE && bt_device_count() == 3,
              "Bluetooth: the number reaches nzeus-bt's input, the device is paired");

        /* a pairing that fails */
        if (system("echo error > /tmp/fake_bt/mode") != 0) {}
        tap(14); tap(3); tap(13);                         /* B: rows, DOWN: Find devices..., A */
        WAIT_FOR(!bt_scanning() && app_settings_bt_rows() == 3);
        tap(3); tap(3); tap(13);                          /* DOWN x2: Cheap Mouse, A */
        WAIT_FOR(strstr(app_settings_bt_note(), "Pairing failed"));
        check(app_settings_bt_page() == P_PAIR && !strcmp(app_settings_bt_note(), "Pairing failed: authentication failed") &&
              !app_settings_bt_big()[0], "Bluetooth: an error line is shown in plain words");
        save("core_bt_9_error");
        tap(14); frames(2);
        check(app_settings_bt_page() == P_FIND, "Bluetooth: B goes back from the pairing page");

        /* my devices: forget asks first; disconnect */
        tap(14); tap(2); tap(13);                         /* B: rows, UP: My devices, A */
        check(app_settings_bt_page() == P_MINE && app_settings_bt_rows() == 3, "Bluetooth: My devices lists the 3 devices");
        tap(13); tap(3); tap(13); frames(2);              /* A on Old Mouse: menu, DOWN: Forget, A */
        check(nzui_active() && file_contains("/tmp/fake_bt/list", "Old Mouse"), "Bluetooth: Forget asks to confirm first");
        save("core_bt_10_forget");
        tap(14); frames(20);                              /* B: no */
        check(!nzui_active() && file_contains("/tmp/fake_bt/list", "Old Mouse"), "Bluetooth: answering No keeps the device");
        tap(13); tap(3); tap(13); tap(13);                /* again, and A: yes */
        WAIT_FOR(bt_device_count() == 2);
        check(!file_contains("/tmp/fake_bt/list", "Old Mouse") && bt_device_count() == 2 &&
              !strcmp(app_settings_bt_message(), "Forgotten"), "Bluetooth: Forget (yes) removes the pairing");
        tap(13); tap(13);                                 /* Keys-To-Go (connected): menu, A: Disconnect */
        WAIT_FOR(!strcmp(app_settings_bt_message(), "Disconnected"));
        check(!strcmp(app_settings_bt_message(), "Disconnected"), "Bluetooth: Disconnect runs nzeus-bt disconnect");

        /* the same page in a desktop window (narrower) */
        ui_set_mode(UI_DESKTOP); ui_open_app(APP_SETTINGS); frames(6);
        save("core_bt_11_window");
        ui_set_mode(UI_HANDHELD); ui_open_app(APP_SETTINGS); frames(4);

        /* off, then B leads all the way out */
        tap(14); tap(2); tap(2); tap(13);                 /* B: rows, UP x2: Bluetooth, A: off */
        WAIT_FOR(bt_state() == BT_OFF);
        check(bt_state() == BT_OFF && access("/tmp/fake_bt/on", F_OK) != 0, "Bluetooth: Off runs nzeus-bt off");
        check(access(NZ_SDCARD "/nzeus/bluetooth/on-at-start", F_OK) != 0, "Bluetooth: Off is remembered too (the marker is gone)");
        save("core_bt_12_off");
        tap(14); tap(14); frames(3);
        check(apps_focused() == NULL, "Bluetooth: B always goes back (out of Settings)");

        /* the deck's Bluetooth scanner (app_btscan.c): nzeus-bt survey -> what each device is */
        {
            int app_btscan_count(void), app_btscan_trackers(void);
            const char *app_btscan_card(void), *app_btscan_status(void);
            ui_open_app(APP_BTSCAN);
            WAIT_FOR(strstr(app_btscan_status(), "Bluetooth is off") != NULL);   /* (after "Checking...") */
            check(strstr(app_btscan_status(), "Bluetooth is off") != NULL, "BT scanner: says when Bluetooth is off");
            if (system("touch /tmp/fake_bt/on") != 0) {}
            apps_close(apps_get(APP_BTSCAN)); frames(2);
            ui_open_app(APP_BTSCAN);
            WAIT_FOR(app_btscan_count() == 3);
            check(app_btscan_count() == 3 && file_contains("/tmp/fake_bt/survey_args", "survey 10"),
                  "BT scanner: scans by itself (nzeus-bt survey 10) and lists 3 devices");
            check(app_btscan_trackers() == 1 && strstr(app_btscan_status(), "3 devices nearby  -  1 tracker"),
                  "BT scanner: counts the tracker (Find My) at the top");
            frames(125);                                      /* let the old "Switching Bluetooth off" fade (1.8 s) */
            save("core_bt_13_scanner");
            tap(13); frames(2);                               /* A on the first (AirPods): its ID card */
            check(strstr(app_btscan_card(), "Likely: Apple AirPods Pro") && strstr(app_btscan_card(), "private, changes"),
                  "BT scanner: A shows the ID card (AirPods Pro, private address)");
            save("core_bt_14_card");
            tap(14); frames(2);
            check(!app_btscan_card()[0] && apps_focused() && apps_focused()->id == APP_BTSCAN, "BT scanner: B closes the card");
            tap(3); tap(13); frames(2);                       /* DOWN: the tracker, A */
            check(strstr(app_btscan_card(), "Tracker   yes") && strstr(app_btscan_card(), "while this scanner was open"),
                  "BT scanner: the tracker's card says how long it has been around");
            tap(14); tap(14); frames(3);
            check(apps_focused() == NULL, "BT scanner: B leaves");
            if (system("rm -f /tmp/fake_bt/on") != 0) {}
        }

        /* Update NZEUS (app_update.c) with a pretend nzeus-update.sh (the real one: linux-port/deck-modern/tests) */
        {
            int app_update_state(void);
            const char *app_update_text(void), *app_update_title(void);
            enum { U_IDLE, U_CHECKING, U_READY, U_NOTHING, U_INSTALLING, U_DONE, U_ERROR };
            FILE *uf = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-update.sh", "w");
            if (uf) {
                fputs("case \"$1\" in\n"
                      "check) [ -e /tmp/fake_upd_fail ] && { echo 'error cannot reach the PC (http://pc) - is publish_update.py running, and Wi-Fi on?'; exit 1; }\n"
                      "  echo 'VERSION 2026-10-08 13:00'; echo 'NOTES memory work + updates over Wi-Fi'; echo 'FILES 3'; echo 'BYTES 2000000' ;;\n"
                      "install) echo 'PROGRESS 1 3 nzeus-modern'; sleep 0.5; echo 'PROGRESS 2 3 deck-bin/nzeus-bt'\n"
                      "  echo 'PROGRESS 3 3 deck-bin/nzeus-browser'; echo 'DONE 2026-10-08 13:00 installed - restart NZEUS to start it'; touch /tmp/fake_upd_did ;;\n"
                      "restart) echo restart > /tmp/fake_upd_restart ;;\n"
                      "esac\n", uf);
                fclose(uf);
            }
            if (system("rm -f /tmp/fake_upd_fail /tmp/fake_upd_did /tmp/fake_upd_restart") != 0) {}
            ui_open_app(APP_UPDATE); frames(3);
            check(app_update_state() == U_IDLE && !strcmp(app_update_title(), "Update NZEUS Deck"), "Update: opens with what it does (A checks)");
            tap(13);                                          /* A: check */
            WAIT_FOR(app_update_state() != U_CHECKING);
            check(app_update_state() == U_READY && strstr(app_update_title(), "2026-10-08 13:00") &&
                  strstr(app_update_text(), "memory work + updates over Wi-Fi") && strstr(app_update_text(), "3 files change (1.9 MB)"),
                  "Update: the check shows the version, what's new and how much changes");
            save("core_update_1_ready");
            tap(13); frames(2);                               /* A: install -> asks first */
            check(nzui_active() && access("/tmp/fake_upd_did", F_OK) != 0, "Update: asks before installing (nothing done yet)");
            tap(13); frames(3);                               /* A: yes */
            check(app_update_state() == U_INSTALLING, "Update: installing");
            tap(14); frames(2);                               /* B: not while installing */
            check(apps_focused() && apps_focused()->id == APP_UPDATE, "Update: B can't leave half-way through an install");
            WAIT_FOR(app_update_state() != U_INSTALLING);
            check(app_update_state() == U_DONE && access("/tmp/fake_upd_did", F_OK) == 0 && strstr(app_update_title(), "Installed: 2026-10-08 13:00"),
                  "Update: installed, and it says to restart");
            save("core_update_2_done");
            tap(13); frames(2);                               /* A: restart? */
            int asked_first = nzui_active() && access("/tmp/fake_upd_restart", F_OK) != 0;
            tap(13); frames(3);                               /* A: yes */
            check(asked_first && file_contains("/tmp/fake_upd_restart", "restart"),
                  "Update: A restarts NZEUS Deck (asks first; nzeus-update.sh restart does it, not the console)");
            tap(14); frames(3);                               /* B: leave */
            if (system("touch /tmp/fake_upd_fail") != 0) {}
            ui_open_app(APP_UPDATE); frames(3);
            tap(13);
            WAIT_FOR(app_update_state() != U_CHECKING);
            check(app_update_state() == U_ERROR && strstr(app_update_text(), "Cannot reach the PC"),
                  "Update: says plainly when the PC can't be reached");
            save("core_update_3_error");
            tap(14); frames(3);
            check(apps_focused() == NULL, "Update: B leaves");
            if (system("rm -f /tmp/fake_upd_fail /tmp/fake_upd_did") != 0) {}
        }
        #undef WAIT_FOR
    }

    /* ---- Settings > USB (a fake nzeus-usbmode.sh; its status text in /tmp/fake_usb/status, as the
     *      SD Debian's linux-port/deck-modern/nzeus-usbmode.sh prints it) ---- */
    {
        #define WAIT_FOR(cond) for (int w_ = 0; w_ < 400 && !(cond); w_++) frames(1)
        if (system("rm -rf /tmp/fake_usb && mkdir -p /tmp/fake_usb /tmp/fake_sdcard/nzeus/bin && "
                   "printf 'mode=host\\ndevices=0\\n' > /tmp/fake_usb/status") != 0)
            printf("FAIL  could not make the fake USB folder\n");
        FILE *uf = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-usbmode.sh", "w");
        if (uf) {
            fputs("D=/tmp/fake_usb\necho \"$1\" >> $D/calls\n"
                  "case \"$1\" in\n"
                  "status) cat $D/status ;;\n"
                  "pc) sleep 0.5\n"
                  "  if [ -e $D/pc_fail ]; then echo 'ERROR: unplug the OTG adapter first (USB devices still plugged in: 1), then choose PC connection'\n"
                  "     exit 1; fi\n"
                  "  printf 'mode=pc\\npc=waiting\\nadb=off\\nip=none\\n' > $D/status\n"
                  "  echo 'PC mode: unplug the OTG adapter, then plug the PC in' ;;\n"
                  "host) sleep 0.5; printf 'mode=host\\ndevices=0\\n' > $D/status\n"
                  "  echo 'Devices mode on: plug in the OTG adapter. Never plug the PC in now.' ;;\n"
                  "esac\n", uf);
            fclose(uf);
        }
        ui_open_app(APP_SETTINGS); frames(3);
        for (int i = 0; i < 9; i++) tap(3);              /* DOWN x9: USB */
        WAIT_FOR(usbmode_state() == USBM_HOST);
        check(app_settings_section() == 9 && usbmode_state() == USBM_HOST && !strcmp(usbmode_line(), "Devices: 0 plugged in"),
              "USB: Settings has a USB section; it says Devices, nothing plugged in");
        frames(20);
        check(strstr(app_settings_bt_note(), "plug in the OTG adapter") && strstr(app_settings_bt_note(), "NEVER plug the PC in now") &&
              strstr(app_settings_bt_note(), "Every power-on starts in PC mode"),
              "USB: the help says how (OTG adapter, never the PC in Devices mode, PC mode after every power-on)");
        save("core_usb_1_devices");
        theme_set(TH_GREEN_CRT); frames(3); save("core_usb_1b_devices_crt"); theme_set(TH_ORIGINAL); frames(2);
        tap(13); tap(5);                                 /* A: the rows, RIGHT on "USB mode": PC connection */
        check(usbmode_state() == USBM_SWITCHING && !strcmp(app_settings_bt_message(), "Switching to PC connection..."),
              "USB: RIGHT switches in the background (switching... + a message)");
        save("core_usb_2_switching");
        WAIT_FOR(usbmode_state() == USBM_PC);
        frames(5);
        check(usbmode_state() == USBM_PC && !strcmp(usbmode_line(), "PC: waiting for the cable, network no address yet") &&
              file_contains("/tmp/fake_usb/calls", "pc"),
              "USB: nzeus-usbmode.sh pc ran; the status line says what is going on (no ADB on this system)");
        WAIT_FOR(strstr(app_settings_bt_note(), "PC mode: unplug the OTG adapter") && strstr(app_settings_bt_note(), "ssh nzeus-deck"));
        check(strstr(app_settings_bt_note(), "PC mode: unplug the OTG adapter, then plug the PC in") &&
              strstr(app_settings_bt_note(), "On the PC: ssh nzeus-deck (USB network) or the serial port"),
              "USB: its last line + what to do on the PC (ssh nzeus-deck) are shown");
        if (system("printf 'mode=pc\\npc=connected\\nadb=off\\nip=192.168.77.1\\n' > /tmp/fake_usb/status") != 0) {}
        usbmode_refresh();
        WAIT_FOR(strstr(usbmode_line(), "connected"));
        frames(5);
        check(!strcmp(usbmode_line(), "PC: connected, network 192.168.77.1"), "USB: connected to the PC: the USB network's address");
        save("core_usb_3_pc_connected");
        theme_set(TH_AMBER_CRT); frames(3); save("core_usb_3b_pc_connected_crt"); theme_set(TH_ORIGINAL); frames(2);
        /* PC -> Devices: the port can't sense the PC, so the owner is asked to unplug it first */
        tap(5); frames(2);                               /* RIGHT: Devices */
        check(nzui_active() && usbmode_state() == USBM_PC, "USB: PC -> Devices asks 'Is the PC cable UNPLUGGED?' first");
        save("core_usb_3c_unplug_question");
        tap(14); frames(10);                             /* B: no */
        check(!nzui_active() && usbmode_state() == USBM_PC && !file_contains("/tmp/fake_usb/calls", "host"),
              "USB: answering no switches nothing");
        tap(5); frames(2); tap(13); frames(2);           /* RIGHT, A: yes */
        check(!strcmp(app_settings_bt_message(), "Switching to Devices..."), "USB: answering yes switches to Devices");
        WAIT_FOR(usbmode_state() == USBM_HOST);
        frames(5);
        check(usbmode_state() == USBM_HOST && file_contains("/tmp/fake_usb/calls", "host") &&
              strstr(app_settings_bt_note(), "Devices mode on: plug in the OTG adapter"), "USB: switching back to Devices works");
        if (system("touch /tmp/fake_usb/pc_fail") != 0) {}
        tap(5);
        WAIT_FOR(usbmode_state() == USBM_HOST && strstr(usbmode_note(), "Problem"));
        frames(20);                                      /* (the help text is redrawn every 250 ms) */
        check(usbmode_state() == USBM_HOST && strstr(app_settings_bt_note(), "Problem: unplug the OTG adapter first"),
              "USB: a refused switch is explained in plain words, the port stays Devices");
        save("core_usb_4_problem");
        if (system("rm -f /tmp/fake_usb/pc_fail; printf 'mode=pc\\nerror=this kernel has no USB role switch (needs the gma-native-host kernel)\\n'"
                   " > /tmp/fake_usb/status") != 0) {}
        usbmode_refresh();
        WAIT_FOR(strstr(usbmode_note(), "role switch"));
        check(usbmode_state() == USBM_PC && !strcmp(usbmode_note(), "Problem: this kernel has no USB role switch (needs the gma-native-host kernel)"),
              "USB: the helper's error= line (a kernel without host mode) is shown as the problem");
        if (system("printf 'mode=host\\ndevices=2\\n' > /tmp/fake_usb/status") != 0) {}
        usbmode_refresh();
        WAIT_FOR(strstr(usbmode_line(), "2 plugged"));
        check(!strcmp(usbmode_line(), "Devices: 2 plugged in"), "USB: Devices mode counts what is plugged in");
        if (system("mv /tmp/fake_sdcard/nzeus/bin/nzeus-usbmode.sh /tmp/fake_usb/away.sh") != 0) {}
        usbmode_refresh();
        WAIT_FOR(usbmode_state() == USBM_MISSING);
        tap(5);
        check(usbmode_state() == USBM_MISSING && !strcmp(app_settings_bt_message(), "The USB add-on is not on the SD card"),
              "USB: without the helper: said plainly, nothing runs");
        if (system("mv /tmp/fake_usb/away.sh /tmp/fake_sdcard/nzeus/bin/nzeus-usbmode.sh") != 0) {}
        tap(14); tap(14); frames(3);
        check(apps_focused() == NULL, "USB: B goes back (out of Settings)");
        #undef WAIT_FOR
    }

    /* ---- Settings > Remote access: the SD Debian's own SSH server (Dropbear, port 22, keys
     *      only) - shown, not switched. Its "listening" comes from /proc/net/tcp (a fake here). ---- */
    {
        extern const char *settings_tcp_file;
        settings_tcp_file = "/tmp/fake_tcp";
        FILE *tf = fopen(settings_tcp_file, "w");
        if (tf) { fputs("  sl  local_address rem_address   st tx_queue rx_queue\n"
                        "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000\n", tf); fclose(tf); }   /* (port 8080 only) */
        ui_open_app(APP_SETTINGS); frames(3);
        for (int i = 0; i < 10; i++) tap(3);             /* DOWN x10: Remote access */
        frames(3);
        check(app_settings_section() == 10 && !strcmp(app_settings_row_value(0), "off") &&
              !strcmp(app_settings_row_value(1), "Cable: root@192.168.77.1   Wi-Fi: root@192.168.1.57") &&
              !strcmp(app_settings_row_value(2), "/root/.ssh/authorized_keys"),
              "Remote access: SSH server off (nothing on port 22), where to log in (cable + the Wi-Fi address), the keys file");
        check(strstr(app_settings_bt_note(), "Only keys listed in /root/.ssh/authorized_keys can log in") &&
              strstr(app_settings_bt_note(), "ssh nzeus-deck"), "Remote access: the note says keys only + how to log in from the PC");
        check(!popup_label(lv_scr_act(), "Let Claude control", 12) && !popup_label(lv_scr_act(), "Remote access over Wi-Fi", 12),
              "Remote access: no 'Claude access' or 'Remote access over Wi-Fi' switches (not on this system)");
        tf = fopen(settings_tcp_file, "a");
        if (tf) { fputs("   1: 00000000:0016 00000000:0000 0A 00000000:00000000\n", tf); fclose(tf); }   /* 0.0.0.0:22 listening */
        for (int i = 0; i < 150 && strcmp(app_settings_row_value(0), "ON (port 22, keys only)"); i++) frames(1);
        check(!strcmp(app_settings_row_value(0), "ON (port 22, keys only)"), "Remote access: Dropbear listening on port 22 = 'ON (port 22, keys only)'");
        save("core_remote_ssh");
        theme_set(TH_GREEN_CRT); frames(3); save("core_remote_ssh_crt"); theme_set(TH_ORIGINAL); frames(2);
        settings_tcp_file = "/proc/net/tcp";
        tap(14); frames(3);
        check(apps_focused() == NULL, "Remote access: B goes back (out of Settings)");
    }

    /* ---- the MP3 decoder itself (libmad): a 320 kbps joint-stereo MP3 with a
     * 1000 Hz tone on the LEFT only must come back at the right pitch, loud on
     * the left and silent on the right (channels kept apart, not swapped) ---- */
    {
        FILE *w = fopen("/tmp/fake_lr.wav", "wb");
        int n = 44100 * 2;                                   /* 2 s, 44.1 kHz stereo */
        uint8_t h[44] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data____";
        uint32_t data = (uint32_t)n * 4, riff = data + 36;
        memcpy(h + 4, &riff, 4); memcpy(h + 40, &data, 4);
        if (w) fwrite(h, 1, 44, w);
        for (int i = 0; w && i < n; i++) {
            int16_t lr[2] = { (int16_t)(16000 * sin(2 * 3.14159265358979 * 1000 * i / 44100.0)), 0 };
            fwrite(lr, 2, 2, w);
        }
        if (w) fclose(w);
        int made = system("lame --quiet -b 320 -m j /tmp/fake_lr.wav /tmp/fake_lr.mp3 2>/dev/null") == 0;
        char aerr[96];
        if (made && audio_open("/tmp/fake_lr.mp3", aerr, sizeof aerr) == 0) {
            static int16_t buf[735 * 2];
            int r, c, crossings = 0, right_peak = 0, prev = 0;
            long long left_sq = 0, count = 0;
            audio_info(&r, &c);
            audio_play(1);
            for (int k = 0; k < 90; k++) {                   /* 1.5 s; the first 0.3 s is skipped (encoder delay) */
                audio_render(buf, 735);
                if (k < 18) continue;
                for (int i = 0; i < 735; i++) {
                    int L = buf[i * 2], R = buf[i * 2 + 1];
                    if (abs(R) > right_peak) right_peak = abs(R);
                    if ((L >= 0) != (prev >= 0)) crossings++;
                    prev = L;
                    left_sq += (long long)L * L; count++;
                }
            }
            double secs = count / 44100.0, hz = crossings / 2.0 / secs, rms = sqrt((double)left_sq / count);
            printf("INFO  MP3 L/R test: %.1f Hz on the left (rms %.0f), right peak %d\n", hz, rms, right_peak);
            check(r == 44100 && c == 2 && hz > 980 && hz < 1020 && rms > 8000 && right_peak < 400,
                  "MP3 (libmad, 320 kbps joint stereo): left-only 1000 Hz tone decodes at the right pitch, right stays silent");
            audio_close();
        } else printf("NOTE  lame not installed: MP3 L/R test skipped\n");
        unlink("/tmp/fake_lr.wav"); unlink("/tmp/fake_lr.mp3");
    }

    /* ---- Media: the music player ---- */
    make_tone("/tmp/fake_sdcard/music/tone.wav");
    int have_mp3 = system("lame --quiet -b 128 /tmp/fake_sdcard/music/tone.wav /tmp/fake_sdcard/music/tone.mp3 2>/dev/null") == 0;
    ui_open_app(APP_MEDIA); frames(3);
    check(apps_focused() && apps_focused()->id == APP_MEDIA && app_media_song_count() == 1 + have_mp3,
          "the Media app lists the songs in the music folder");
    if (have_mp3) tap(3);                            /* to tone.wav (row 1 = tone.mp3, row 2 = tone.wav) */
    long f0 = sound_frames; sound_peak = 0;
    tap(13); frames(60);                             /* A: play, about 1 second */
    double pos = audio_position();
    check(audio_playing() && sound_frames - f0 > 60 * 735 && sound_peak > 5000,
          "playing a WAV sends sound to the frontend (44100 Hz stereo)");
    check(pos > 0.8 && pos < 1.4, "a 22050 Hz WAV plays at the right speed (converted to 44100)");
    save("core_12_media");
    sound_peak = 0;
    tap(1); frames(10);                              /* START: pause */
    check(!audio_playing() && sound_peak == 0, "START pauses (silence)");
    tap(1);
    if (have_mp3) {
        tap(10); frames(40);                         /* L1: the previous song (tone.mp3) */
        int r, c; audio_info(&r, &c);
        check(audio_playing() && r == 22050 && sound_peak > 5000 && audio_length() > 1.8 && audio_length() < 2.4,
              "an MP3 plays too (decoded with libmad)");
    } else printf("NOTE  lame not installed: MP3 test skipped\n");
    /* one speaker: another app (WhatsApp's voice messages) takes it while Media plays.
     * Media lets go of its song; when that other sound ends, Media must NOT start its next song */
    {
        char aerr[96];
        make_tone_at("/tmp/fake_other_sound.wav", 11025, 1);
        int took = audio_open("/tmp/fake_other_sound.wav", aerr, sizeof aerr) == 0;
        audio_play(1);
        for (int i = 0; i < 300 && audio_playing(); i++) frames(1);
        frames(20);                                  /* (Media's timer runs a few times) */
        check(took && audio_finished() && !strcmp(audio_path(), "/tmp/fake_other_sound.wav"),
              "Media: another app's sound took the speaker - when it ends, Media doesn't start its next song");
        audio_close();
    }
    tap(14); frames(3);                              /* B: leave */
    check(!audio_loaded() && apps_focused() == NULL, "B leaves Media and the music stops");
    {
        char err[96];
        check(audio_open("/tmp/fake_sdcard/music/tone.wav", err, sizeof err) == 0, "open backpressure test song");
        audio_play(1);
        sound_partial = sound_suffix_ok = 1;
        frames(1);
        double held = audio_position();
        frames(2);
        check(sound_suffix_ok && !sound_partial && audio_position() == held,
              "audio: partial and busy output retain every sample without decoding ahead");
        frames(1);
        check(audio_position() > held, "audio: decoding resumes after the pending samples are accepted");
        sound_partial = 1; frames(1);
        audio_play(0); sound_partial = 0; sound_peak = 0; frames(1);
        check(sound_peak == 0, "audio: pause discards pending sound instead of replaying it");
        audio_close();
    }

    /* ---- Arduino: serial monitor on a fake board (a pseudo-terminal) ---- */
    pty_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty_master >= 0 && grantpt(pty_master) == 0 && unlockpt(pty_master) == 0) {
        fcntl(pty_master, F_SETFL, O_NONBLOCK);
        static char slave[64];
        snprintf(slave, sizeof slave, "%s", ptsname(pty_master));
        serial_extra_port = slave;
    } else printf("FAIL  could not make a fake serial port\n");
    ui_open_app(APP_ARDUINO); frames(10);
    check(app_arduino_connected(), "Arduino: finds the board's port and connects");
    elm_say("hello from arduino\r\n"); frames(10);
    check(!strcmp(app_arduino_last_line(), "hello from arduino"), "Arduino: shows what the board sends");
    { char junk[256]; while (read(pty_master, junk, sizeof junk) > 0) {} }
    type_line("ping"); frames(5);
    char got[32] = "";
    int gn = (int)read(pty_master, got, sizeof got - 1);
    if (gn > 0) got[gn] = 0;
    check(!strcmp(got, "ping\n"), "Arduino: sends a typed line (+ newline) to the board");
    save("core_13_arduino");
    /* upload a program: a fake nzeus-upload.sh that prints like the real one */
    {
        FILE *us = fopen("/tmp/fake_sdcard/nzeus/bin/nzeus-upload.sh", "w");
        if (us) { fputs("echo \"STEP board=$1 port=$2\"; echo 'PROGRESS write 50'; sleep 0.2; echo 'PROGRESS verify 100'; echo \"FILE $3\"; echo 'Upload OK'\n", us); fclose(us); }
        if (system("mkdir -p /tmp/fake_sdcard/nzeus/firmware && echo ':00000001FF' > /tmp/fake_sdcard/nzeus/firmware/blink.ino.hex"
                   " && echo x > /tmp/fake_sdcard/nzeus/firmware/blink.ino.with_bootloader.hex") != 0) printf("FAIL  no firmware folder\n");
        menu(5); frames(2);                          /* X menu: Upload a program */
        tap(13); frames(2);                          /* board: Arduino Uno */
        check(nzui_active() && app_arduino_firmware_count() == 1, "Arduino upload: lists the .hex (not the with_bootloader one)");
        tap(13); frames(3);                          /* the file: blink.ino.hex */
        check(app_arduino_uploading(), "Arduino upload: runs nzeus-upload.sh in the background");
        for (int i = 0; i < 120 && app_arduino_uploading(); i++) frames(1);
        check(!app_arduino_uploading() && file_contains("/tmp/nzeus-upload.log", "STEP board=uno port=/dev/pts/") &&
              file_contains("/tmp/nzeus-upload.log", "FILE /tmp/fake_sdcard/nzeus/firmware/blink.ino.hex") && app_arduino_connected(),
              "Arduino upload: right board, port and file; the monitor reconnects after");
    }
    tap(14); frames(3);

    /* ---- Probe: a pretend NZEUS Probe board (R36S/apps/probe) on the same fake port ---- */
    {
        char junk[256];
        while (read(pty_master, junk, sizeof junk) > 0) {}
        ui_open_app(APP_PROBE); frames(3);
        save("core_13b_probe_search");
        check(pty_heard("ID\n", 40), "Probe: asks the port who it is (ID)");
        check(!app_probe_connected(), "Probe: a port that hasn't answered is not taken for a Probe");
        elm_say("NZPROBE 1 pico\r\n");
        check(pty_heard("INFO\n", 40) && app_probe_connected(), "Probe: knows the board by its ID answer, then asks INFO");
        elm_say("INFO adc=2 logic=4 vref=3300 pwm=1\n"); frames(3);
        tap(13);                                     /* A: read the voltmeter */
        check(pty_heard("V 1\n", 20), "Probe: A asks for channel 1's voltage");
        elm_say("V 1 1650\n"); frames(5);
        check(!strcmp(app_probe_reading(), "1.650 V"), "Probe: shows the voltage the board measured");
        save("core_13c_probe_volt");
        tap(7); tap(13);                             /* LEFT (channel 1 -> 2: wraps), A */
        check(pty_heard("V 2\n", 20), "Probe: LEFT/RIGHT change the channel, wrapping at the board's 2");
        tap(3); tap(3); tap(13);                     /* DOWN x2: Scope, A */
        check(pty_heard("S 2 100 200\n", 20), "Probe: the scope asks for 100 samples on that channel");
        {
            char s[1024] = "S 100 200";              /* a pretend triangle wave, 0.3 to 3.0 V */
            for (int i = 0; i < 100; i++) {
                int ph = i % 25, mv = 300 + (ph < 13 ? ph : 25 - ph) * 225;
                snprintf(s + strlen(s), sizeof s - strlen(s), " %d", mv);
            }
            strcat(s, "\n");
            elm_say(s); frames(5);
        }
        check(!strcmp(app_probe_reading(), "0.300 to 3.000 V"), "Probe: the scope shows the lowest and highest voltage");
        save("core_13d_probe_scope");
        tap(3); tap(13);                             /* DOWN: Signal generator (1000 Hz), A */
        check(pty_heard("P 1000 50\n", 20), "Probe: the signal generator sets 1000 Hz, 50%");
        tap(3); tap(13);                             /* DOWN: I2C scan, A */
        check(pty_heard("I\n", 20), "Probe: A runs the I2C scan");
        elm_say("I 3C 68\n"); frames(5);
        check(!strcmp(app_probe_reading(), "2 chips found") && strstr(app_probe_detail(), "0x3C") &&
              strstr(app_probe_detail(), "0x68"), "Probe: lists the chips on the I2C bus");
        save("core_13e_probe_i2c");
        elm_say("ERR bus stuck\n"); frames(5);
        check(strstr(app_probe_detail(), "ERR bus stuck") != NULL, "Probe: a board's ERR answer is shown, not dropped");
        tap(14);                                     /* B: leave */
        check(pty_heard("P 0 50\n", 10) && apps_focused() == NULL, "Probe: leaving stops the signal generator");
    }

    /* Missing diagnostic data must never become a clean bill of health. */
    {
        static CarScan incomplete;
        memset(&incomplete, 0, sizeof incomplete);
        carscan_summarize(&incomplete, 0);
        check(!incomplete.healthy && strstr(incomplete.headline_en, "Scan incomplete") &&
              strstr(incomplete.summary_en, "No fault-code results were received") &&
              !strstr(incomplete.summary_en, "light is off"),
              "Car: no responses cannot claim a healthy car or a known engine-light state");
        incomplete.module_count = 1;
        incomplete.modules[0].answered = 1;
        incomplete.mil_known = 1;
        carscan_summarize(&incomplete, 0);
        check(!incomplete.healthy && strstr(incomplete.summary_en, "remain unknown"),
              "Car: a responding module without its code list leaves the scan incomplete");
        incomplete.modules[0].codes_read = 1;
        carscan_summarize(&incomplete, 0);
        check(incomplete.healthy, "Car: confirmed clean results can still report no faults");
        carscan_summarize(&incomplete, 1);
        check(!incomplete.healthy && strstr(incomplete.headline_en, "Scan incomplete"),
              "Car: an unreachable diagnostic bus prevents a healthy headline");
        incomplete.mil_known = 0;
        carscan_add_dtc(&incomplete, "OBD", "P0133", 0, 0, DTC_HISTORY);
        carscan_summarize(&incomplete, 0);
        check(!strstr(incomplete.summary_en, "light is off"),
              "Car: history codes cannot invent an unknown engine-light state");
    }

    /* ---- Car: OBD-II with a pretend OBDLink EX in a pretend 2025 Ranger Raptor ---- */
    frame_hook = fake_elm;
    ui_open_app(APP_CAR);
    for (int i = 0; i < 200 && obd_live()->have[PID_VOLTS] == 0; i++) frames(1);
    const ObdLive *L = obd_live();
    check(obd_state() == OBD_READY && L->have[PID_RPM] && L->value[PID_RPM] == 1726 && L->value[PID_SPEED] == 60 &&
          L->value[PID_COOLANT] == 50 && L->value[PID_VOLTS] > 12.5, "Car: connects to the adapter and reads RPM, speed, coolant, battery");
    for (int i = 0; i < 300 && !(L->have[PID_OILTEMP] && L->have[PID_TIMING] && L->have[PID_MAF]); i++) frames(1);
    check(obd_pid_supported(PID_OILTEMP) && obd_pid_supported(PID_AMBIENT) && !obd_pid_supported(PID_FUELRATE) &&
          L->value[PID_OILTEMP] == 90 && L->value[PID_TIMING] == 8 && L->value[PID_MAF] == 4 && L->value[PID_AMBIENT] == 41 &&
          !L->have[PID_FUELRATE], "Car: finds which values the car has (0100/0120/0140) and reads only those");
    check(!strcmp(obd_adapter(), "OBDLink EX r2.7.1") && obd_is_stn(), "Car: knows the adapter is an OBDLink (STI / STDI)");
    save("core_14_car");
    tap(15); frames(3);                              /* Y: plain-language guide */
    check(app_car_page() == 5 && popup_label(apps_focused()->root, "Car guide", 12) &&
          popup_label(apps_focused()->root, "Codes and self-tests", 12),
          "Car: Y opens a guide explaining readings, reports and diagnostic terms");
    save("core_14c_car_guide");
    tap(3); tap(3); frames(3);
    check(app_car_scroll_y() > 0, "Car: D-pad actually scrolls the long guide on the handheld");
    tap(14); frames(3);
    check(app_car_page() == 0, "Car: B returns from the guide to live readings");
    menu(10); frames(3);
    check(app_car_page() == 6 && popup_label(apps_focused()->root, "Feature changes: not available yet", 12),
          "Car: feature page states the real support status, without pretend on/off controls");
    save("core_14d_car_features");
    tap(13); frames(3);
    check(nzui_active() && popup_label(lv_layer_top(), "Vehicle support actions", 12),
          "Car: feature page opens real vehicle selection and pack actions");
    if (nzui_active()) { tap(14); frames(3); }
    tap(13); tap(13); frames(3);
    check(nzui_active() && popup_label(lv_layer_top(), "Choose vehicle", 12),
          "Car: manual vehicle selection is separate from discovered compatibility");
    if (nzui_active()) { tap(14); frames(3); }
    tap(14); frames(3);
    tap(13); frames(3);                              /* A: report without an earlier scan */
    check(nzui_active() && popup_label(lv_layer_top(), "Scan and save PDF - language", 12),
          "Car: Make report offers a scan and export instead of a manual prerequisite");
    tap(13); frames(3); tap(14); frames(3);          /* begin, then cancel */
    check(app_car_page() == 0 && !app_car_scan() && strstr(ui_toast_text(), "no new PDF saved"),
          "Car: cancelling scan-and-save leaves no finished scan or promised PDF");
    tap(12); frames(3); save("core_14b_car_menu"); tap(14); frames(3);   /* X: the menu (does it fit?), B: closed */
    menu(0); for (int i = 0; i < 60 && !obd_dtcs_ready(); i++) frames(1);
    check(obd_dtc_count() == 4 && !strcmp(obd_dtc(0), "P0133") && !strcmp(obd_dtc(1), "P0442") &&
          !strcmp(obd_dtc(2), "P0300") && !strcmp(obd_dtc(3), "P0562"), "Car: reads the fault codes (a long answer, padding ignored)");
    char two[4][6];
    check(obd_parse_dtcs("43010133\r43010442\r", 1, two, 4) == 2 && !strcmp(two[1], "P0442"),
          "Car: answers from two control units are read separately");
    check(obd_parse_dtcs("43 01 33 04 42 00 00\r", 0, two, 4) == 2 && !strcmp(two[0], "P0133"),
          "Car: older (non-CAN) answers work too");
    save("core_15_car_codes");
    menu(1); tap(13); for (int i = 0; i < 60 && !obd_codes_ready(CODES_PENDING); i++) frames(1);   /* More: pending */
    check(obd_codes_count(CODES_PENDING) == 1 && !strcmp(obd_code(CODES_PENDING, 0), "P0171"), "Car: reads pending codes (mode 07)");
    menu(1); tap(3); tap(13); for (int i = 0; i < 60 && !obd_codes_ready(CODES_PERMANENT); i++) frames(1);
    check(obd_codes_ready(CODES_PERMANENT) && obd_codes_count(CODES_PERMANENT) == 0, "Car: reads permanent codes (mode 0A)");
    menu(1); tap(3); tap(3); tap(13); for (int i = 0; i < 120 && !obd_info_ready(); i++) frames(1);
    const ObdInfo *OI = obd_info();
    int done_tests = 0, evap_done = 1;
    for (int i = 0; i < OI->mon_count; i++) { done_tests += OI->mon[i].complete; if (strstr(OI->mon[i].name, "EVAP")) evap_done = OI->mon[i].complete; }
    check(obd_info_ready() && OI->have_status && !OI->mil_on && OI->dtc_count == 4 && OI->mon_count == 8 && done_tests == 7 && !evap_done,
          "Car: self-tests: light off, 4 codes, 7 of 8 done, EVAP not yet");
    frames(20); save("core_16_car_readiness");
    check(OI->have_ff && !strcmp(OI->ff_dtc, "P0133") && OI->ff_value[FF_RPM] == 1850 && OI->ff_value[FF_SPEED] == 72 &&
          OI->ff_value[FF_COOLANT] == 83, "Car: freeze frame: the code that froze it and the frozen RPM / speed / coolant");
    menu(1); tap(3); tap(3); tap(3); tap(13); for (int i = 0; i < 120 && !obd_info_ready(); i++) frames(1);
    frames(20); save("core_17_car_freeze");
    check(!strcmp(OI->vin, "6FPTESTXXS1200001") && !strcmp(OI->calid, "JB3A-14C204-AB") && !strcmp(OI->ecu_name, "ECM-EngineControl"),
          "Car: VIN (3 pieces joined), calibration ID and ECU name (mode 09)");
    char vin[18];
    check(obd_parse_vin("490201000000 36\r49020246504445\r4902034D4F5858\r49020453313230\r49020530303031\r", vin) &&
          !strcmp(vin, "6FPDEMOXXS1200001"), "Car: VIN from an older (non-CAN) car's 5 lines");
    menu(2); for (int i = 0; i < 120 && !obd_info_ready(); i++) frames(1);
    frames(20); save("core_18_car_info");
    /* the graph */
    menu(3); frames(150);
    check(app_car_page() == 2 && app_car_graph_points() >= 3, "Car: Graph shows a live value, several points a second");
    save("core_19_car_graph");
    tap(5); frames(5);                               /* RIGHT: the next value */
    check(app_car_page() == 2, "Car: LEFT/RIGHT picks another value to graph");
    tap(14); frames(3);
    /* logging to the SD card */
    menu(4); tap(13); frames(100);                   /* Start logging, everything */
    check(carlog_active() && strstr(carlog_path(), "/tmp/fake_sdcard/nzeus/car/log-") && strstr(carlog_path(), ".csv"),
          "Car: logging writes nzeus/car/log-YYYYMMDD-HHMMSS.csv");
    save("core_20_car_rec");
    char logpath[200];
    snprintf(logpath, sizeof logpath, "%s", carlog_path());
    menu(4); frames(3);                              /* Stop logging */
    int log_lines = 0;
    { FILE *lf = fopen(logpath, "r"); char ln[512]; while (lf && fgets(ln, sizeof ln, lf)) log_lines++; if (lf) fclose(lf); }
    check(!carlog_active() && file_starts_with(logpath, "time_s,RPM (rpm),Speed (km/h),Coolant (C)") && log_lines >= 3 &&
          file_contains(logpath, ",1726.00,60.00,50.00"), "Car: the log has a header row and time + values rows");
    /* the whitelist: forbidden requests never reach the adapter */
    {
        static const char *const forbidden[] = { "2EF19001", "3101FF00", "1003", "1001", "1101", "14FFFFFF", "2701", "2F123403",
                                                 "3400", "3601", "8502", "190AFF", "1904FF", "3E01", "04", "ATMA", "ATPP0CSV", "STSBR9600",
                                                 "22", "0100 1", "01\r04", "ATSH7E0\r04" };
        int refused = 1, before = obd_refused_count();
        for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++)
            if (obd_allowed(forbidden[i], 0) || obd_ask(forbidden[i], 1000) != -1) { refused = 0; printf("      NOT refused: %s\n", forbidden[i]); }
        frames(20);
        check(refused && obd_refused_count() == before + (int)(sizeof forbidden / sizeof forbidden[0]) && elm_forbidden == 0,
              "Car: the whitelist refuses writes, resets, sessions, routines, security, clear-by-UDS (never sent)");
        check(obd_allowed("22F190", 0) && obd_allowed("1902AF", 0) && obd_allowed("1901FF", 0) && obd_allowed("3E00", 0) &&
              obd_allowed("0902", 0) && obd_allowed("ATSH726", 0) && obd_allowed("STP53", 0) && !obd_allowed("04", 0) && obd_allowed("04", 1),
              "Car: the whitelist lets reads through; mode 04 only when the clear flow armed it");
        /* GM (GMLAN): only 1A + one identifier byte (a read); GM's write, programming,
         * device control, code list and bus-silence bytes never pass. Flow control:
         * only the exact settings the GM search uses. */
        static const char *const gm_forbidden[] = { "3B9001", "3B90", "A50101", "A503", "AE1001", "A98112", "AA0101", "2C01",
                                                    "2801", "20", "A2", "2701", "1A", "1A9000", "ATFCSM2", "ATFCSD300001",
                                                    "ATFCSD", "ATFCSH24", "ATFCSH2410", "ATFCSHZ41" };
        int gm_refused = 1;
        before = obd_refused_count();
        for (size_t i = 0; i < sizeof gm_forbidden / sizeof gm_forbidden[0]; i++)
            if (obd_allowed(gm_forbidden[i], 0) || obd_ask(gm_forbidden[i], 1000) != -1) { gm_refused = 0; printf("      NOT refused: %s\n", gm_forbidden[i]); }
        frames(20);
        check(gm_refused && obd_refused_count() == before + (int)(sizeof gm_forbidden / sizeof gm_forbidden[0]) && elm_forbidden == 0,
              "Car, GM: the whitelist refuses GM's write / programming / device control / code-list / silence requests (never sent)");
        check(obd_allowed("1A90", 0) && obd_allowed("1ACB", 0) && obd_allowed("ATFCSH241", 0) && obd_allowed("ATFCSD300000", 0) &&
              obd_allowed("ATFCSM1", 0) && obd_allowed("ATFCSM0", 0),
              "Car, GM: GM's read (1A + one identifier) and the flow-control settings pass the whitelist");
        static char gm_hex[129];
        check(gm_parse_identity("5A904142\r", 0x90, gm_hex, sizeof gm_hex) == 2 && !strcmp(gm_hex, "4142") &&
              gm_parse_identity("7F1A31\r", 0x90, gm_hex, sizeof gm_hex) < 0 && !gm_hex[0] &&
              gm_parse_identity("5AB040\r", 0x90, gm_hex, sizeof gm_hex) < 0 &&
              gm_parse_identity("5A90\r", 0x90, gm_hex, sizeof gm_hex) < 0 &&
              gm_parse_identity("5A904142\r", 0x90, gm_hex, 4) < 0,
              "Car, GM: identifier replies keep exact bytes; refused, wrong, empty and oversized replies stay unknown");
        check(gm_answered("5A904142\r", 0x90) && gm_answered("7F1A31\r", 0x90) && !gm_answered("7F1A78\r", 0x90) &&
              !gm_answered("NO DATA\r", 0x90) && !gm_answered("CAN ERROR\r", 0x90) && !gm_answered("5AB040\r", 0x90) &&
              !gm_answered("7F1931\r", 0x90),
              "Car, GM: a module is there if it answers or refuses 1A; 'wait' (7F 1A 78), silence or other replies are not answers");
    }
    /* the full health scan of the Raptor (the engine's 4 codes were "fixed" first:
     * like his real report, only the body modules have old codes) */
    elm_forbidden = 0;
    elm_engine_codes = 0;
    memset(elm_probed, 0, sizeof elm_probed);
    tap(13); tap(13); frames(16);                   /* A report, English: scan and save */
    check(app_car_page() == 3 && carscan_state() == CS_RUNNING, "Car: Full health scan shows its progress");
    save("core_21_car_scanning");
    for (int i = 0; i < 30000 && carscan_state() == CS_RUNNING; i++) fast_frames(1);
    frames(25);
    const CarScan *S = app_car_scan();
    int history = 0, b10f3 = 0, pam_silent = 1, ford_names = 0, unnamed = 0;
    if (S) {
        for (int i = 0; i < S->dtc_count; i++) {
            history += S->dtcs[i].state == DTC_HISTORY;
            if (!strcmp(S->dtcs[i].code, "B10F3") && !strcmp(S->dtcs[i].module, "BdyCM") &&
                !strcmp(S->dtcs[i].meaning_ar, "إضاءة الموقع الأمامية اليسرى: انقطاع أو تماس في الدائرة") &&
                !strcmp(S->dtcs[i].group_en, "Exterior lights · Body Control Module")) b10f3 = 1;
        }
        for (int i = 0; i < S->module_count; i++) {
            if (!strcmp(S->modules[i].code, "PAM")) pam_silent = !S->modules[i].answered;
            if ((S->modules[i].can_id == 0x726 && !strcmp(S->modules[i].code, "BdyCM") && !strcmp(S->modules[i].name_en, "Body control")) ||
                (S->modules[i].can_id == 0x7E0 && !strcmp(S->modules[i].code, "PCM"))) ford_names++;
            if (!strncmp(S->modules[i].name_en, "Module 0x", 9)) unnamed++;
        }
    }
    check(S && carscan_state() == CS_DONE && app_car_page() == 4, "Car: the scan finishes and shows its report");
    check(strstr(ui_toast_text(), "Saved nzeus/car/Health-Scan-") &&
          strstr(carreport_last_text(), "28 modules answered"),
          "Car: report shortcut exports the completed scan automatically");
    check(S && S->modules_answered == 28 && S->module_count == 28 && pam_silent,
          "Car: scan: the 28 modules of his report answer (found by asking every address), missing ones are not listed");
    check(S && ford_names == 2 && !unnamed, "Car: scan: a Ford VIN still gets Ford's module names (BdyCM 'Body control', PCM)");
    check(elm_probed[0x6F0] && elm_probed[0x700] && elm_probed[0x7F7] && !elm_probed[0x7DF] && !elm_probed[0x7E8] &&
          !elm_probed[0x72E] && !elm_probed[0x7F8], "Car: scan: every address 0x700-0x7F7 (+ Ford's 0x6F0) asked; never 7DF, 7E8, or a found module's answer address");
    check(S && S->dtc_count == 11 && history == 11 && b10f3, "Car: scan: his 11 codes, all history (Not present), with English + Arabic meanings");
    check(S && S->healthy && !strcmp(S->headline_en, "No active faults reported.") &&
          !strcmp(S->headline_ar, "لم تُبلّغ أعطال حالية.") && strstr(S->summary_en, "28 modules answered") &&
          strstr(S->summary_ar, "توجد 11 رسالة خطأ قديمة"), "Car: scan: headline qualifies the available results and the summary");
    check(S && S->odo_count == 3 && S->odo_agree == 1 && S->odometer_km == 764, "Car: scan: mileage check - 3 modules agree on 764 km");
    check(S && !strcmp(S->vin, "6FPTESTXXS1200001") && S->model_year == 2025 && !strcmp(S->make_model_en, "Ford") &&
          S->battery_v > 12.5 && !strcmp(S->adapter, "OBDLink EX r2.7.1"), "Car: scan: VIN, model year 2025, battery, adapter");
    check(S && strstr(S->cause_en, "power event") && strstr(S->cause_ar, "مشكلة كهرباء مؤقتة") && S->step_count >= 3 &&
          strstr(S->steps_en[0], "brake lights") && S->wish_count == 10, "Car: scan: the power-event cause, what to do, the wishlist");
    check(elm_saw_stp53 && elm_forbidden == 0, "Car: scan: tries Ford's MS-CAN bus for silent modules (OBDLink), nothing forbidden sent");
    check(elm_gm_queries == 0, "Car: scan: a Ford never gets GM's own questions (1A)");
    check(S && elm_identity_queries >= 4 * 28 && S->modules[0].identity_status[0] == 1 &&
          S->modules[0].identity_status[3] == 1 && !strcmp(S->modules[0].identity[0], "07E0"),
          "Car: scanner reads identification from each responding module with its correct header");
    {
        FILE *discovery = fopen("/tmp/fake_sdcard/nzeus/car/Last-vehicle-discovery.json", "r");
        char data[65536] = "";
        if (discovery) { size_t n = fread(data, 1, sizeof data - 1, discovery); data[n] = 0; fclose(discovery); }
        check(discovery && strstr(data, "6FPTESTXXS1200001") && strstr(data, "\"read_only\":true") &&
              strstr(data, "\"F189\"") && strstr(data, "\"bus\":0"),
              "Car: completed scan automatically saves private vehicle/module discovery JSON");
    }
    check(S && S->mil_known && !S->mil_on && strstr(S->readiness_en, "7 of 8 self-tests done") && S->live_count >= 5 &&
          !strcmp(S->live[0].name_en, "Engine speed") && !strcmp(S->live[0].value, "1726 rpm") && S->note_count >= 2,
          "Car: scan: check-engine light + self-tests, a live-value snapshot and notes from the engine computer");
    frames(20);
    check(elm_header == 0x7DF && elm_cra == -1 && !elm_ms && !elm_h1 && elm_st == 0x32 && obd_session_clean(),
          "Car: after the scan the adapter talks to the whole car again (header 7DF, no filter, headers off, normal timeout)");
    car_recovery_test();
    save("core_22_car_report");
    for (int i = 0; i < 8; i++) tap(3);
    save("core_23_car_report_codes");
    for (int i = 0; i < 12; i++) tap(3);
    save("core_24_car_report_end");
    theme_set(TH_GREEN_CRT); frames(5);
    for (int i = 0; i < 20; i++) tap(2);             /* UP: back to the top */
    save("core_25_car_report_crt");
    tap(14); menu(3); frames(60); save("core_26_car_graph_crt"); tap(14);
    theme_set(TH_ORIGINAL); frames(3);
    /* the variant: one module's odometer was changed */
    elm_odo_disagree = 1;
    menu(5); frames(3);
    for (int i = 0; i < 30000 && carscan_state() == CS_RUNNING; i++) fast_frames(1);
    frames(25);
    S = app_car_scan();
    int odo_step = 0;
    for (int i = 0; S && i < S->step_count; i++) if (strstr(S->steps_en[i], "do not agree on the mileage")) odo_step = 1;
    check(S && S->odo_agree == 0 && S->odometer_km == 801 && odo_step, "Car: scan: a module with a different odometer is noticed");
    elm_odo_disagree = 0;
    tap(14); frames(3);
    /* reading a module's answer: a code failing NOW, and one only pending */
    {
        char identity[129];
        check(uds_parse_identity("62F187414243\r", 0xF187, identity, sizeof identity) == 3 && !strcmp(identity, "414243"),
              "Car: identifier capture preserves exact positive bytes");
        check(uds_parse_identity("7F2231\r", 0xF187, identity, sizeof identity) < 0 && !identity[0] &&
              uds_parse_identity("62F188414243\r", 0xF187, identity, sizeof identity) < 0 &&
              uds_parse_identity("62F187\r", 0xF187, identity, sizeof identity) < 0 &&
              uds_parse_identity("62F187414243\r", 0xF187, identity, 3) < 0,
              "Car: unsupported, wrong, empty and oversized identifier replies stay unknown");
        static CarScan one;
        memset(&one, 0, sizeof one);
        check(uds_parse_dtcs("5902\r", &one, "ABS") < 0 &&
              uds_parse_dtcs("5902FFC1\r", &one, "ABS") < 0 && one.dtc_count == 0 &&
              uds_parse_dtcs("00B\r0:5902FFC100092F\r", &one, "ABS") < 0 &&
              uds_parse_dtcs("5902FF\r", &one, "ABS") == 0,
              "Car: incomplete UDS lists stay unknown; a complete empty list is valid");
        uds_parse_dtcs("00F\r0:5902FFC10009\r1:2F944800049110\r2:13280000AAAAAA\r", &one, "ABS");
        check(one.dtc_count == 3 && !strcmp(one.dtcs[0].code, "U0100") && one.dtcs[0].state == DTC_ACTIVE &&
              !strcmp(one.dtcs[1].code, "B1448") && one.dtcs[1].state == DTC_PENDING &&
              !strcmp(one.dtcs[2].code, "B1110") && one.dtcs[2].failure_type == 0x13 && one.dtcs[2].state == DTC_HISTORY,
              "Car: UDS codes: 3-byte code + status (failing now / pending / history), padding ignored");
    }
    /* the PDF report from this scan: English, then Arabic (DOWN, A) */
    if (system("rm -f /tmp/fake_sdcard/nzeus/car/Health-Scan-*.pdf") != 0) {}
    menu(6); tap(3); tap(13); tap(13); frames(5);
    check(!nzui_active() && apps_focused() && apps_focused()->id == APP_CAR, "Car: Make PDF report asks the language and comes back");
    menu(6); tap(3); tap(13); tap(3); tap(13); frames(5);
    check(system("head -c 5 /tmp/fake_sdcard/nzeus/car/Health-Scan-*-EN.pdf | grep -q '%PDF-'") == 0 &&
          system("head -c 5 /tmp/fake_sdcard/nzeus/car/Health-Scan-*-AR.pdf | grep -q '%PDF-'") == 0 &&
          system("ls /tmp/fake_sdcard/nzeus/car/*.part >/dev/null 2>&1") != 0,
          "Car: Make PDF report writes the English and the Arabic PDF to nzeus/car (no half-written files left)");
    {                                                 /* keep copies next to the screenshots, to look at */
        char cp[700];
        snprintf(cp, sizeof cp, "cp /tmp/fake_sdcard/nzeus/car/Health-Scan-*.pdf '%s'/ 2>/dev/null", outdir);
        if (system(cp) != 0) {}
    }
    /* clearing: the only change it can make */
    menu(7); frames(2);
    check(!nzui_active() && !elm_got_clear && strstr(ui_toast_text(), "Read the fault codes first"),
          "Car: reconnect invalidates old codes and cannot clear without a fresh read");
    elm_engine_codes = 1;                           /* this new read has codes to test the confirmation flow */
    menu(0);
    for (int i = 0; i < 200 && !obd_dtcs_ready(); i++) frames(1);
    check(obd_dtcs_ready() && obd_dtc_count() == 4, "Car: fresh codes can be read after reconnecting");
    menu(7); frames(2); tap(14); frames(20);         /* Clear... then B (no) */
    check(!elm_got_clear, "Car: saying no never clears the codes");
    menu(7); frames(2); tap(13); frames(2);          /* yes ... */
    check(!elm_got_clear && nzui_active(), "Car: clearing asks a second time");
    tap(13); for (int i = 0; i < 60 && !elm_got_clear; i++) frames(1);
    check(elm_got_clear && elm_forbidden == 0 && system("ls /tmp/fake_sdcard/nzeus/car/codes-*.txt >/dev/null 2>&1") == 0,
          "Car: clears only after 2 yes, with a copy of the codes saved first (to the whole car)");
    elm_engine_codes = 0;
    tap(14); tap(14); frames(3);                     /* codes page, then leave */

    /* ---- the same scan on a car that is NOT a Ford: a 2019 Toyota with an engine (7E8)
     *      and a gearbox (7E9) computer, a module at 0x7B0 with codes, one at 0x7C0 that
     *      can't list its codes ---- */
    elm_car = CAR_TOYOTA;
    elm_header = 0x7DF; elm_cra = -1; elm_ms = 0; elm_h1 = 0;
    elm_forbidden = 0; elm_saw_stp53 = 0; elm_gm_queries = 0;
    memset(elm_probed, 0, sizeof elm_probed);
    ui_open_app(APP_CAR);
    for (int i = 0; i < 300 && !(obd_state() == OBD_READY && obd_live()->have[PID_VOLTS]); i++) frames(1);
    menu(5); frames(3);
    for (int i = 0; i < 30000 && carscan_state() == CS_RUNNING; i++) fast_frames(1);
    frames(25);
    S = app_car_scan();
    {
        const ScanModule *ecm = NULL, *tcm = NULL, *m7b0 = NULL, *m7c0 = NULL;
        const ScanDtc *p0133 = NULL, *p0442 = NULL, *p0171 = NULL, *p0700 = NULL, *c0035 = NULL, *b1318 = NULL;
        for (int i = 0; S && i < S->module_count; i++) {
            const ScanModule *m = &S->modules[i];
            if (m->can_id == 0x7E0) ecm = m;
            if (m->can_id == 0x7E1) tcm = m;
            if (m->can_id == 0x7B0) m7b0 = m;
            if (m->can_id == 0x7C0) m7c0 = m;
        }
        for (int i = 0; S && i < S->dtc_count; i++) {
            const ScanDtc *d = &S->dtcs[i];
            if (!strcmp(d->code, "P0133")) p0133 = d;
            if (!strcmp(d->code, "P0442")) p0442 = d;
            if (!strcmp(d->code, "P0171")) p0171 = d;
            if (!strcmp(d->code, "P0700")) p0700 = d;
            if (!strcmp(d->code, "C0035")) c0035 = d;
            if (!strcmp(d->code, "B1318")) b1318 = d;
        }
        check(S && carscan_state() == CS_DONE && app_car_page() == 4 && !strcmp(S->make_en, "Toyota") &&
              !strcmp(S->make_model_en, "Toyota") && S->model_year == 2019 && !strcmp(S->vin, "JTDKN3DU5K0123456"),
              "Car, any make: a Toyota VIN gives make 'Toyota' and model year 2019");
        check(ecm && !strcmp(ecm->code, "ECM") && !strcmp(ecm->name_en, "Engine computer") && tcm && !strcmp(tcm->code, "TCM") &&
              !strcmp(tcm->name_en, "Gearbox computer") && m7b0 && !strcmp(m7b0->code, "0x7B0") && !strcmp(m7b0->name_en, "Module 0x7B0") &&
              m7b0->codes_read && m7c0 && m7c0->answered && !m7c0->codes_read && S->modules_answered == 4 && S->module_count == 4,
              "Car, any make: both engine computers (7E8, 7E9) + every module that answers; unknown ones are 'Module 0x7B0'");
        check(S && S->dtc_count == 6 && p0133 && p0133->state == DTC_PERMANENT && !strcmp(p0133->module, "ECM") && p0442 &&
              p0442->state == DTC_ACTIVE && p0171 && p0171->state == DTC_PENDING && p0700 && p0700->state == DTC_STORED &&
              !strcmp(p0700->module, "TCM") && !strcmp(p0700->group_en, "Gearbox computer"),
              "Car, any make: stored / pending / permanent codes per computer; the module's own UDS status wins (P0442 failing now)");
        check(c0035 && c0035->state == DTC_ACTIVE && !strcmp(c0035->meaning_en, "Left front wheel speed sensor") &&
              !strcmp(c0035->group_en, "Module 0x7B0") && b1318 && b1318->state == DTC_HISTORY && !b1318->meaning_en[0] && !b1318->meaning_ar[0],
              "Car, any make: generic codes get their meaning; a maker's own code (B1318) gets no Ford meaning");
        check(S && S->mil_known && S->mil_on && strstr(S->readiness_en, "not yet: Fuel vapour (EVAP)") && S->odo_count == 1 &&
              S->odometer_km > 12345.5 && S->odometer_km < 12345.7 && S->live_count >= 5 && !strcmp(S->live[0].name_en, "Engine speed") &&
              !strcmp(S->live[0].value, "1726 rpm") && !S->healthy && strstr(S->headline_en, "Needs a check: 2 active faults"),
              "Car, any make: check-engine light ON, self-tests, odometer from PID A6, a live-value snapshot, 'needs a check'");
        check(S && !scan_mentions(S, "Ford") && !scan_mentions(S, "FORScan") && !scan_mentions(S, "MS-CAN") && !scan_mentions(S, "SYNC") &&
              !scan_mentions(S, "فورد") && S->wish_count == 0 && !elm_saw_stp53,
              "Car, any make: no Ford words anywhere in the Toyota's scan, no wishlist, no switch to Ford's MS-CAN");
        check(elm_forbidden == 0 && elm_probed[0x700] && elm_probed[0x7B0] && elm_probed[0x7F7] && !elm_probed[0x7B8] &&
              !elm_probed[0x7E8] && !elm_probed[0x7E9] && !elm_probed[0x7DF] && !elm_probed[0x6F0],
              "Car, any make: every address 0x700-0x7F7 asked (read only), never 7DF, 7E8/7E9 or 0x7B8 (0x7B0's answer address)");
        check(elm_gm_queries == 0, "Car, any make: a Toyota never gets GM's own questions (1A)");
    }
    frames(20);
    check(elm_header == 0x7DF && elm_cra == -1 && !elm_h1 && elm_st == 0x32 && obd_session_clean(),
          "Car, any make: after the scan the adapter is back to normal");
    save("core_22b_car_report_toyota");
    menu(6); tap(13); frames(3);                    /* New scan and PDF is the default */
    check(nzui_active() && popup_label(lv_layer_top(), "Scan and save PDF - language", 12),
          "Car: with an older scan, Make report offers a fresh scan before reusing the old one");
    tap(14); frames(3);
    check(app_car_scan() == S && app_car_page() == 4,
          "Car: cancelling a new report's language leaves the previous scan available");
    {   /* its PDF, English then Arabic: the right title, the new sections, no Ford words */
        static char en[65536];
        if (system("rm -f /tmp/fake_sdcard/nzeus/car/Health-Scan-*.pdf") != 0) {}
        menu(6); tap(3); tap(13); tap(13); frames(5);
        snprintf(en, sizeof en, "%s", carreport_last_text());
        menu(6); tap(3); tap(13); tap(3); tap(13); frames(5);
        const char *ar = carreport_last_text();
        check(system("head -c 5 /tmp/fake_sdcard/nzeus/car/Health-Scan-*-EN.pdf | grep -q '%PDF-'") == 0 &&
              system("head -c 5 /tmp/fake_sdcard/nzeus/car/Health-Scan-*-AR.pdf | grep -q '%PDF-'") == 0 &&
              strstr(ui_toast_text(), "Saved nzeus/car/Health-Scan-"),
              "Car, any make: the Toyota's PDF report is written to nzeus/car in English and Arabic");
        check(strstr(en, "2019 Toyota\n") && strstr(en, "Check-engine light and self-tests") && strstr(en, "The check-engine light is ON.") &&
              strstr(en, "Live values at the scan") && strstr(en, "Engine speed") && strstr(en, "Notes") && strstr(en, "Module 0x7B0") &&
              strstr(en, "Stored") && strstr(en, "JTD") && !strstr(en, "0123456"),
              "Car, any make: the PDF says '2019 Toyota', has the light + self-tests, codes per module, live values, notes; the VIN is masked");
        check(!strstr(en, "Ford") && !strstr(en, "FORScan") && !strstr(en, "Raptor") && !strstr(en, "MS-CAN") && !strstr(en, "SYNC") &&
              !strstr(en, "DEMO") && ar[0] && strstr(ar, "2019") && !strstr(ar, "Ford") && !strstr(ar, "FORScan"),
              "Car, any make: no Ford words (and no DEMO) in the Toyota's PDF, English or Arabic");
        char cp[700];                                 /* keep copies next to the screenshots, to look at */
        snprintf(cp, sizeof cp, "for f in /tmp/fake_sdcard/nzeus/car/Health-Scan-*.pdf; do cp \"$f\" '%s'/Toyota-$(basename \"$f\"); done 2>/dev/null", outdir);
        if (system(cp) != 0) {}
    }
    tap(14); tap(14); frames(3);                     /* report -> gauges, then leave */

    /* ---- GM: a 2016 Chevrolet truck like my field scan. UDS finds only 7E0, 7E2
     *      and 7F1; GM's own language (GMLAN) also finds 0x241 (VIN + identifiers; the VIN
     *      is a long answer that needs flow control to 0x241) and 0x244 (no VIN). 0x246
     *      only says "wait" and is not listed. READ ONLY: no GM write / programming byte
     *      reaches the adapter, and the adapter is put back afterwards ---- */
    elm_car = CAR_GM;
    elm_header = 0x7DF; elm_cra = -1; elm_ms = 0; elm_h1 = 0;
    elm_forbidden = 0; elm_saw_stp53 = 0; elm_gm_queries = elm_gm_wrong = elm_gm_long_ok = 0;
    memset(elm_probed, 0, sizeof elm_probed);
    if (system("rm -f /tmp/fake_sdcard/nzeus/car/Last-vehicle-discovery.json") != 0) {}
    ui_open_app(APP_CAR);
    for (int i = 0; i < 300 && !(obd_state() == OBD_READY && obd_live()->have[PID_VOLTS]); i++) frames(1);
    int gm_resets = elm_resets;
    menu(5); frames(3);
    {
        int saw_gm_progress = 0, d, t;
        char line[160];
        for (int i = 0; i < 30000 && carscan_state() == CS_RUNNING; i++) {
            fast_frames(1);
            carscan_progress(&d, &t, line, sizeof line);
            if (strstr(line, "GM modules (GMLAN): address 0x24")) saw_gm_progress = 1;
        }
        check(saw_gm_progress, "Car, GM: the progress line says it is asking GM's own modules");
    }
    frames(25);
    S = app_car_scan();
    {
        const ScanModule *g241 = NULL, *g244 = NULL, *g246 = NULL, *u7e0 = NULL, *u7e2 = NULL, *u7f1 = NULL;
        int gm_note = -1, silent_note = 0;
        for (int i = 0; S && i < S->module_count; i++) {
            const ScanModule *m = &S->modules[i];
            if (m->can_id == 0x241) g241 = m;
            if (m->can_id == 0x244) g244 = m;
            if (m->can_id == 0x246) g246 = m;
            if (m->can_id == 0x7E0) u7e0 = m;
            if (m->can_id == 0x7E2) u7e2 = m;
            if (m->can_id == 0x7F1) u7f1 = m;
        }
        for (int i = 0; S && i < S->note_count; i++) {
            if (strstr(S->notes_en[i], "GM: 2 modules answered GM's own language (GMLAN, 0x241-0x24F)")) gm_note = i;
            if (strstr(S->notes_en[i], "1 module answered but would not list its fault codes")) silent_note = 1;
        }
        check(S && carscan_state() == CS_DONE && !strcmp(S->make_en, "Chevrolet") && S->model_year == 2016 &&
              !strcmp(S->vin, GM_TEST_VIN), "Car, GM: a Chevrolet VIN (model year 2016) turns on the GM module search");
        check(u7e0 && u7e0->codes_read && u7e0->protocol == SCAN_PROTO_UDS && u7e2 && u7e2->codes_read && u7f1 &&
              u7f1->answered && !u7f1->codes_read && silent_note,
              "Car, GM: the UDS search still finds 7E0 and 7E2 (codes read) and 7F1 (answers, cannot list codes) as in the field");
        check(g241 && g241->answered && g241->protocol == SCAN_PROTO_GMLAN && !g241->codes_read &&
              !strcmp(g241->code, "0x241") && !strcmp(g241->name_en, "GM module 0x241") && g244 && g244->answered &&
              g244->protocol == SCAN_PROTO_GMLAN && !g246 && S->modules_answered == 5 && S->module_count == 5,
              "Car, GM: GM-language modules 0x241 and 0x244 are listed (unnamed); 'wait' alone (0x246) is not an answer");
        check(elm_gm_long_ok && g241 && g241->identity_status[0] == 1 &&
              !strcmp(g241->identity[0], "314743554B524543354746313233343536") &&
              g241->identity_status[1] == 1 && !strcmp(g241->identity[1], "40") &&
              g241->identity_status[2] == 1 && !strcmp(g241->identity[2], "01234567") &&
              g241->identity_status[3] == 1 && !strcmp(g241->identity[3], "89ABCDEF") &&
              g244 && g244->identity_status[0] == -1 && !g244->identity[0][0] && g244->identity_status[1] == 1 &&
              !strcmp(g244->identity[1], "44") && g244->identity_status[2] == -1 && g244->identity_status[3] == -1,
              "Car, GM: exact identifier bytes are kept (the VIN through flow control to 0x241); refused ones stay unknown");
        check(elm_forbidden == 0 && elm_gm_wrong == 0 && elm_gm_queries == 15 + 3 + 3 && !elm_saw_stp53,
              "Car, GM: only GM's read (1A) to 0x241-0x24F, 15 addresses + identifiers; nothing forbidden, no Ford bus switch");
        check(S && !S->healthy && !strcmp(S->headline_en, "Scan incomplete: some results are unknown.") && gm_note >= 0 &&
              strstr(S->notes_en[gm_note], "fault codes were not read yet, so they are unknown.") &&
              strstr(S->notes_ar[gm_note], "GMLAN") && strstr(S->notes_ar[gm_note], "فحالتها غير معروفة.") &&
              strlen(S->notes_en[gm_note]) < sizeof S->notes_en[0] - 1 && strlen(S->notes_ar[gm_note]) < sizeof S->notes_ar[0] - 1,
              "Car, GM: GM modules' fault codes are not read yet - unknown, never healthy; the note is whole in English and Arabic");
        FILE *discovery = fopen("/tmp/fake_sdcard/nzeus/car/Last-vehicle-discovery.json", "r");
        static char data[65536];
        data[0] = 0;
        if (discovery) { size_t n = fread(data, 1, sizeof data - 1, discovery); data[n] = 0; fclose(discovery); }
        check(discovery && strstr(data, GM_TEST_VIN) && strstr(data, "\"read_only\":true") &&
              strstr(data, "{\"address\":577,\"bus\":0,\"answered\":true,\"codes_read\":false,\"name\":\"GM module 0x241\",\"protocol\":\"GMLAN\"") &&
              strstr(data, "\"GM90\":{\"status\":1,\"hex\":\"314743554B524543354746313233343536\"}") &&
              strstr(data, "\"GMCB\":{\"status\":1,\"hex\":\"89ABCDEF\"}") && strstr(data, "\"GM90\":{\"status\":-1,\"hex\":\"\"}") &&
              strstr(data, "\"protocol\":\"UDS\",\"identifiers\":{\"F187\""),
              "Car, GM: the discovery file records GM modules (GMLAN), their GM identifiers as exact bytes, UDS ones as before");
    }
    frames(20);
    check(elm_header == 0x7DF && elm_cra == -1 && !elm_h1 && elm_st == 0x32 && elm_fc_mode == 0 && obd_session_clean() &&
          obd_state() == OBD_READY && elm_resets == gm_resets,
          "Car, GM: after the scan the adapter is back to normal (flow control automatic again, ATFCSM0), no reset");
    {   /* the PDF lists the GM modules and the note (English) */
        if (system("rm -f /tmp/fake_sdcard/nzeus/car/Health-Scan-*.pdf") != 0) {}
        menu(6); tap(3); tap(13); tap(13); frames(5);
        const char *en = carreport_last_text();
        check(strstr(en, "GM module 0x241") && strstr(en, "GM module 0x244") && strstr(en, "GMLAN") &&
              system("head -c 5 /tmp/fake_sdcard/nzeus/car/Health-Scan-*-EN.pdf | grep -q '%PDF-'") == 0,
              "Car, GM: the PDF lists GM's modules and says their codes were not read");
        menu(6); tap(3); tap(13); tap(3); tap(13); frames(5);     /* the same scan in Arabic */
        const char *ar = carreport_last_text();
        int ar_ok = strstr(ar, "0x241") && strstr(ar, "0x244") && strstr(ar, "GMLAN") &&
                    system("head -c 5 /tmp/fake_sdcard/nzeus/car/Health-Scan-*-AR.pdf | grep -q '%PDF-'") == 0;
        if (!ar_ok) {                                 /* show what the Arabic report drew around the GM lines */
            const char *p = ar;
            while ((p = strstr(p, "0x24")) != NULL) { printf("      AR line: %.80s\n", p > ar + 40 ? p - 40 : ar); p += 4; }
        }
        check(ar_ok, "Car, GM: the Arabic PDF lists GM's modules and has the GM note");
        char cp[700];
        snprintf(cp, sizeof cp, "for f in /tmp/fake_sdcard/nzeus/car/Health-Scan-*.pdf; do cp \"$f\" '%s'/GM-$(basename \"$f\"); done 2>/dev/null", outdir);
        if (system(cp) != 0) {}
    }
    save("core_22c_car_report_gm");
    /* an adapter that refuses the flow-control setting: no GM questions at all, said in the notes */
    elm_fc_refuse = 1; elm_gm_queries = 0; elm_forbidden = 0;
    int resets_before = elm_resets;
    menu(5); frames(3);
    for (int i = 0; i < 30000 && carscan_state() == CS_RUNNING; i++) fast_frames(1);
    frames(25);
    S = app_car_scan();
    {
        int refused_note = 0, gm_mods = 0;
        for (int i = 0; S && i < S->note_count; i++)
            if (strstr(S->notes_en[i], "GM: the adapter refused the flow-control setting") && strstr(S->notes_ar[i], "GMLAN")) refused_note = 1;
        for (int i = 0; S && i < S->module_count; i++) gm_mods += S->modules[i].protocol == SCAN_PROTO_GMLAN;
        check(S && carscan_state() == CS_DONE && refused_note && !gm_mods && elm_gm_queries == 0 && elm_forbidden == 0 &&
              S->modules_answered == 3 && !S->healthy,
              "Car, GM: an adapter without flow-control settings: no GM questions, the notes say so, still not 'healthy'");
    }
    frames(20);
    check(elm_header == 0x7DF && elm_cra == -1 && elm_fc_mode == 0 && obd_session_clean() && obd_state() == OBD_READY &&
          elm_resets == resets_before, "Car, GM: a refused flow-control setting leaves the adapter connected and normal (no reset)");
    elm_fc_refuse = 0;
    tap(14); tap(14); frames(3);                     /* report -> gauges, then leave */
    elm_car = CAR_RAPTOR;

    /* ---- no adapter: no demo, nothing made up, and it says what to do ---- */
    frame_hook = NULL;
    serial_extra_port = NULL;
    ui_open_app(APP_CAR); frames(5);
    check(car_no_demo(), "Car: without an adapter there is no demo: no values, 'No OBD adapter - plug the ELM327 into the USB port ...'");
    save("core_27a_car_no_adapter");
    tap(12); frames(3);                              /* X: the menu */
    check(nzui_active() && popup_label(lv_layer_top(), "Scan available systems", 12) && !popup_label(lv_layer_top(), "Demo", 12),
          "Car: the menu has no 'Demo on / off' item");
    tap(14); frames(3);
    S = app_car_scan();
    menu(5); frames(10);
    check(app_car_page() == 0 && app_car_scan() == S && S && !strcmp(S->make_en, "Chevrolet") && car_no_demo() &&
          strstr(ui_toast_text(), "No OBD adapter"),
          "Car: without an adapter the health scan says 'No OBD adapter' - no made-up report");
    tap(14); frames(3);
    ui_set_mode(UI_DESKTOP);                          /* the Car app in a window */
    ui_open_app(APP_CAR); frames(30);
    save("core_27_car_window");
    { NzApp *cw = apps_get(APP_CAR); if (cw && cw->root) apps_close(cw); }
    frames(3);
    ui_set_mode(UI_HANDHELD); frames(3);

    /* ---- nzshot / nzpress / nztype: the helper commands' channel ---- */
    {
        if (system("mkdir -p /tmp/nzeus-cmd && rm -f /tmp/nzeus-cmd/*") != 0) printf("FAIL  no /tmp/nzeus-cmd\n");
        FILE *rq = fopen("/tmp/nzeus-cmd/req.4242", "w");
        if (rq) { fputs("shot /tmp/fake_sdcard/shot.png\n", rq); fclose(rq); }
        frames(10);
        Picture sp;
        char perr[64];
        int same = 0;
        if (picture_load("/tmp/fake_sdcard/shot.png", 640, 480, &sp, perr, sizeof perr) == 0) {
            same = sp.w == 640 && sp.h == 480;
            for (int i = 0; same && i < 640 * 480; i += 997) if (sp.px[i] != frame[i]) same = 0;
            picture_free(&sp);
        }
        check(file_contains("/tmp/nzeus-cmd/done.4242", "ok") && same, "nzshot: NZEUS saves the screen as a PNG (same pixels)");
        int before = home_selected();
        rq = fopen("/tmp/nzeus-cmd/req.4243", "w");
        if (rq) { fputs("press RIGHT\n", rq); fclose(rq); }
        frames(10);
        check(file_contains("/tmp/nzeus-cmd/done.4243", "ok") && home_selected() == before + 1, "nzpress: presses a button");
        rq = fopen("/tmp/nzeus-cmd/req.4244", "w");
        if (rq) { fputs("press JUMP\n", rq); fclose(rq); }
        frames(10);
        check(file_contains("/tmp/nzeus-cmd/done.4244", "error"), "nzpress: an unknown button is an error, not a crash");
        tap(7); frames(2);
    }

    /* ---- nzctl: mouse, click, drag, open APP, home (the same channel; "adb shell nzctl ...") ---- */
    {
        char rq_text[96], ans[300];
        #define CTL(id, ...) do { snprintf(rq_text, sizeof rq_text, __VA_ARGS__); \
            FILE *q_ = fopen("/tmp/nzeus-cmd/req." id, "w"); if (q_) { fprintf(q_, "%s\n", rq_text); fclose(q_); } } while (0)
        #define ANSWER(id) (ans[0] = 0, file_lines("/tmp/nzeus-cmd/done." id, 0, ans, sizeof ans), ans)
        int px, py;
        ui_set_mode(UI_HANDHELD); frames(2);
        CTL("5001", "open car"); frames(10);
        check(!strcmp(ANSWER("5001"), "ok") && apps_focused() && apps_focused()->id == APP_CAR, "nzctl open car: the Car app opens");
        CTL("5002", "open Wi-Fi"); frames(10);
        check(!strcmp(ANSWER("5002"), "ok") && apps_focused() && apps_focused()->id == APP_WIFI, "nzctl open Wi-Fi: by name, any case, dash or not");
        CTL("5003", "open TASK manager"); frames(10);
        check(!strcmp(ANSWER("5003"), "ok") && apps_focused() && apps_focused()->id == APP_TASKMGR, "nzctl open TASK manager: two-word names too");
        CTL("5004", "open toaster"); frames(10);
        check(!strncmp(ANSWER("5004"), "error: no app called 'toaster' - the apps are: terminal files", 61) && strstr(ans, " car ") && strstr(ans, " wifi "),
              "nzctl open: an unknown app is an error that lists the names");
        CTL("5005", "home"); frames(10);
        check(!strcmp(ANSWER("5005"), "ok") && apps_focused() == NULL && ui_mode() == UI_HANDHELD, "nzctl home: back to the home screen (apps closed like B)");
        CTL("5006", "mouse 320 200"); frames(10);
        ui_pointer_pos(&px, &py);
        check(!strcmp(ANSWER("5006"), "ok") && ui_mode() == UI_DESKTOP && px == 320 && py == 200,
              "nzctl mouse: desktop mode, the pointer is shown at that spot (like a real mouse)");
        printf("INFO  pointer after mouse 320 200: %d,%d mode %d\n", px, py, ui_mode());
        save("core_ctl_1_pointer");
        CTL("5007", "click 46 290"); frames(15);          /* the desktop's Car icon (4th, at 8,254 76x72) */
        { NzApp *cw = apps_get(APP_CAR);
          check(!strcmp(ANSWER("5007"), "ok") && cw && cw->root && cw->in_window, "nzctl click: clicking the Car icon opens the Car window"); }
        save("core_ctl_2_click_opens_car");
        { NzApp *cw = apps_get(APP_CAR);
          lv_area_t a0 = { 0 }, a1 = { 0 };
          if (cw && cw->host) lv_obj_get_coords(cw->host, &a0);
          CTL("5008", "drag %d %d %d %d", a0.x1 + 60, a0.y1 + 10, a0.x1 + 110, a0.y1 + 70);
          frames(8);
          check(!file_contains("/tmp/nzeus-cmd/done.5008", "ok"), "nzctl drag: the answer waits until the drag has ended");
          frames(30);
          if (cw && cw->host) lv_obj_get_coords(cw->host, &a1);
          check(!strcmp(ANSWER("5008"), "ok") && a1.x1 - a0.x1 > 35 && a1.x1 - a0.x1 < 65 && a1.y1 - a0.y1 > 45 && a1.y1 - a0.y1 < 75,
                "nzctl drag: dragging the title bar moves the window (about 50 right, 60 down)");
          printf("INFO  drag: window %d,%d %dx%d -> %d,%d\n", (int)a0.x1, (int)a0.y1, (int)(a0.x2 - a0.x1 + 1), (int)(a0.y2 - a0.y1 + 1), (int)a1.x1, (int)a1.y1);
          save("core_ctl_3_dragged");
          if (cw && cw->root) apps_close(cw); }
        CTL("5009", "click 700 10"); frames(10);
        check(!strcmp(ANSWER("5009"), "error: outside the screen (x 0-639, y 0-479)"), "nzctl click: outside the screen is refused");
        CTL("5010", "click 10"); frames(10);
        check(!strcmp(ANSWER("5010"), "error: click needs 2 whole numbers"), "nzctl click: needs two numbers");
        CTL("5011", "drag 1 2 3"); frames(10);
        check(!strcmp(ANSWER("5011"), "error: drag needs 4 whole numbers"), "nzctl drag: needs four numbers");
        CTL("5012", "rclick 100 120"); frames(10);
        ui_pointer_pos(&px, &py);
        check(!strcmp(ANSWER("5012"), "ok") && px == 100 && py == 120 && !desktop_desk_menu_is_open(),
              "nzctl rclick: the pointer moves; on a desktop icon no menu opens");
        CTL("5013", "wheel -3"); frames(10);
        check(!strcmp(ANSWER("5013"), "ok"), "nzctl wheel: accepted (scrolls what is under the pointer)");
        CTL("5014", "wheel lots"); frames(10);
        check(!strcmp(ANSWER("5014"), "error: wheel needs 1 whole number"), "nzctl wheel: needs a number");
        CTL("5015", "rclick 420 300"); frames(10);
        check(!strcmp(ANSWER("5015"), "ok") && desktop_desk_menu_is_open(), "nzctl rclick: on the empty desktop the desktop menu opens");
        desktop_close_menus();
        ui_set_mode(UI_HANDHELD); frames(3);
        #undef CTL
        #undef ANSWER
    }

    /* ---- the desktop with a keyboard + mouse: the keyboard driver's shortcuts,
     *      a start menu that fits, Alt+Tab, maximize, snap, minimize, the
     *      desktop menu, the tray, the wheel, the window limit ---- */
    {
        #define KEY(k) do { KbEvent e_ = { (k), 0 }; ui_kbd(&e_); frames(2); } while (0)
        #define CLICK_ON(o) do { lv_area_t c_; lv_obj_get_coords((o), &c_); \
            ui_mouse_at((c_.x1 + c_.x2) / 2, (c_.y1 + c_.y2) / 2, 0, 1); frames(4); } while (0)
        lv_area_t a, n0;
        KbEvent ev;
        /* the keyboard driver: kernel key codes -> the desktop's shortcuts */
        usbkbd_test_event(KEY_LEFTMETA, 1); usbkbd_test_event(KEY_LEFTMETA, 0);
        int k_super = usbkbd_next(&ev) && ev.key == KB_SUPER;
        usbkbd_test_event(KEY_LEFTMETA, 1); usbkbd_test_event(KEY_LEFT, 1); usbkbd_test_event(KEY_LEFT, 0);
        usbkbd_test_event(KEY_LEFTMETA, 0);
        int k_snap = usbkbd_next(&ev) && ev.key == KB_SUPER_LEFT && !usbkbd_next(&ev);   /* (no start menu after it) */
        usbkbd_test_event(KEY_LEFTALT, 1); usbkbd_test_event(KEY_LEFTSHIFT, 1);
        usbkbd_test_event(KEY_TAB, 1); usbkbd_test_event(KEY_TAB, 0); usbkbd_test_event(KEY_LEFTSHIFT, 0);
        int k_tab = usbkbd_next(&ev) && ev.key == KB_ALT_TAB && ev.ch == KB_SHIFTED;
        usbkbd_test_event(KEY_F4, 1); usbkbd_test_event(KEY_F4, 0);
        int k_f4 = usbkbd_next(&ev) && ev.key == KB_ALT_F4;
        usbkbd_test_event(KEY_ENTER, 1); usbkbd_test_event(KEY_ENTER, 0); usbkbd_test_event(KEY_LEFTALT, 0);
        int k_enter = usbkbd_next(&ev) && ev.key == KB_ALT_ENTER;
        usbkbd_test_event(KEY_LEFTCTRL, 1); usbkbd_test_event(KEY_LEFTALT, 1);
        usbkbd_test_event(KEY_T, 1); usbkbd_test_event(KEY_T, 0); usbkbd_test_event(KEY_LEFTALT, 0);
        int k_term = usbkbd_next(&ev) && ev.key == KB_CTRL_ALT_T;
        usbkbd_test_event(KEY_ESC, 1); usbkbd_test_event(KEY_ESC, 0);
        usbkbd_test_event(KEY_W, 1); usbkbd_test_event(KEY_W, 0); usbkbd_test_event(KEY_LEFTCTRL, 0);
        int k_esc = usbkbd_next(&ev) && ev.key == KB_CTRL_ESC;
        int k_w = usbkbd_next(&ev) && ev.key == KB_CTRL_OTHER && ev.ch == 'w';
        usbkbd_test_event(KEY_A, 1); usbkbd_test_event(KEY_A, 0);
        int k_a = usbkbd_next(&ev) && ev.key == KB_CHAR && ev.ch == 'a';
        check(k_super && k_snap && k_tab && k_f4 && k_enter && k_term && k_esc && k_w && k_a,
              "USB keyboard: Windows key, Windows+Left, Alt+Shift+Tab, Alt+F4, Alt+Enter, Ctrl+Alt+T, Ctrl+Esc, Ctrl+W (and plain letters still type)");

        /* handheld mode: the shortcuts do nothing there */
        ui_set_mode(UI_HANDHELD); frames(2);
        KEY(KB_SUPER); KEY(KB_ALT_TAB); KEY(KB_ALT_F4);
        check(ui_mode() == UI_HANDHELD && !desktop_start_menu_is_open() && desktop_window_count() == 0,
              "Handheld mode: the desktop's keyboard shortcuts change nothing there");

        /* the start menu: fits above the taskbar in every theme (nothing cut off) */
        ui_set_mode(UI_DESKTOP); frames(2);
        int theme_was = theme_index(), fits = 1;
        for (int t = 0; t < TH_COUNT; t++) {
            theme_set(t); frames(2);
            KEY(KB_SUPER);
            lv_obj_t *sm = desktop_start_menu_obj();
            lv_obj_get_coords(sm, &a);
            int ok = desktop_start_menu_is_open() && a.y1 >= 0 && a.x1 >= 0 && a.x2 < NZ_W && a.y2 < NZ_H - 34;
            if (!ok) printf("NOTE  start menu in theme %d: %d,%d - %d,%d\n", t, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
            fits &= ok;
            if (t == TH_GREEN_CRT) save("core_desk_1b_start_menu_crt");
            KEY(KB_ESC);
        }
        theme_set(theme_was); frames(2);
        check(fits, "Start menu: in every theme it is all on the screen, above the taskbar");
        KEY(KB_SUPER);
        lv_obj_get_coords(desktop_start_menu_obj(), &a);
        check(desktop_start_menu_is_open() && !strcmp(desktop_menu_selected_text(), "Terminal") &&
              lv_obj_get_scroll_bottom(desktop_start_menu_obj()) <= 0,
              "Start menu: the Windows key opens it, the first app highlighted, every row visible");
        save("core_desk_1_start_menu_keys");
        KEY(KB_DOWN); KEY(KB_RIGHT);
        check(!strcmp(desktop_menu_selected_text(), "Calculator"), "Start menu: DOWN then RIGHT moves the highlight (Files -> Calculator)");
        { KbEvent n = { KB_CHAR, 'n' }; ui_kbd(&n); frames(2); }
        check(!strcmp(desktop_menu_selected_text(), "Notes"), "Start menu: a letter jumps to the next row with it (n: Notes)");
        KEY(KB_ESC);
        check(!desktop_start_menu_is_open(), "Start menu: Esc closes it");
        KEY(KB_CTRL_ESC); KEY(KB_DOWN); KEY(KB_ENTER); frames(10);
        NzApp *files = apps_get(APP_FILES), *term = apps_get(APP_TERMINAL);
        check(!desktop_start_menu_is_open() && files->root && files->in_window && apps_focused() == files,
              "Start menu: Ctrl+Esc, DOWN, Enter opens Files in a window");

        /* Ctrl+Alt+T, Alt+Tab */
        KEY(KB_CTRL_ALT_T); frames(10);
        check(term->root && term->in_window && apps_focused() == term && desktop_front_window() == term->host,
              "Ctrl+Alt+T opens the Terminal in a window, in front");
        KEY(KB_ALT_TAB);
        check(apps_focused() == files && desktop_front_window() == files->host,
              "Alt+Tab: the Files window comes to the front and gets the keyboard");
        KEY(KB_ALT_TAB);
        check(apps_focused() == term && desktop_front_window() == term->host, "Alt+Tab again: back to the Terminal");

        /* maximize / restore: Alt+Enter and a double-click on the title bar */
        lv_obj_get_coords(term->host, &n0);
        KEY(KB_ALT_ENTER);
        lv_obj_get_coords(term->host, &a);
        check(desktop_window_state(term->host) == WS_MAX && a.x1 == 0 && a.y1 == 0 && a.x2 == NZ_W - 1 && a.y2 == NZ_H - 34 - 1,
              "Alt+Enter maximizes the window (the whole screen above the taskbar)");
        save("core_desk_2_maximized");
        KEY(KB_ALT_ENTER);
        lv_obj_get_coords(term->host, &a);
        check(desktop_window_state(term->host) == WS_NORMAL && a.x1 == n0.x1 && a.y1 == n0.y1 && a.x2 == n0.x2 && a.y2 == n0.y2,
              "Alt+Enter again: back to its own size and place");
        int tx = n0.x1 + 150, ty = n0.y1 + 12;             /* on the title bar, away from its buttons */
        ui_mouse_at(tx, ty, 0, 1); frames(3); ui_mouse_at(tx, ty, 0, 1); frames(3);
        int dbl_max = desktop_window_state(term->host) == WS_MAX;
        ui_mouse_at(150, 12, 0, 1); frames(3); ui_mouse_at(150, 12, 0, 1); frames(3);
        check(dbl_max && desktop_window_state(term->host) == WS_NORMAL, "Double-click on the title bar: maximize, and again: restore");

        /* snap: drag the title bar to the left edge, then to the right edge */
        ui_mouse_at(tx, ty, 1, 0); frames(2);
        for (int s = 1; s <= 10; s++) { ui_mouse_at(tx - tx * s / 10, ty, 1, 0); frames(1); }
        ui_mouse_at(0, ty, 0, 0); frames(3);
        lv_obj_get_coords(term->host, &a);
        check(desktop_window_state(term->host) == WS_LEFT && a.x1 == 0 && a.y1 == 0 && a.x2 == NZ_W / 2 - 1 && a.y2 == NZ_H - 34 - 1,
              "Dragging a window to the left edge: it fills the left half");
        save("core_desk_3_snapped_left");
        ui_mouse_at(100, 12, 1, 0); frames(2);
        for (int s = 1; s <= 10; s++) { ui_mouse_at(100 + (NZ_W - 1 - 100) * s / 10, 12, 1, 0); frames(1); }
        ui_mouse_at(NZ_W - 1, 12, 0, 0); frames(3);
        lv_obj_get_coords(term->host, &a);
        check(desktop_window_state(term->host) == WS_RIGHT && a.x1 == NZ_W / 2 && a.x2 == NZ_W - 1,
              "... and pulled to the right edge: the right half");
        KEY(KB_SUPER_LEFT);
        int back_normal = desktop_window_state(term->host) == WS_NORMAL;
        KEY(KB_SUPER_LEFT);
        check(back_normal && desktop_window_state(term->host) == WS_LEFT, "Windows+Left: from the right half back to normal, then the left half");
        KEY(KB_SUPER_DOWN);
        check(desktop_window_state(term->host) == WS_NORMAL && !desktop_window_minimized(term->host), "Windows+Down: back to its own size");

        /* minimize, and the taskbar button brings it back */
        CLICK_ON(lv_obj_get_child(lv_win_get_header(term->host), 2));      /* (icon, title, minimize, ...) */
        check(desktop_window_minimized(term->host) && lv_obj_has_flag(term->host, LV_OBJ_FLAG_HIDDEN) && apps_focused() == files,
              "The minimize button hides the window; the Files window is now in front");
        save("core_desk_4_minimized");
        CLICK_ON(desktop_task_button(term->host));
        check(!desktop_window_minimized(term->host) && !lv_obj_has_flag(term->host, LV_OBJ_FLAG_HIDDEN) && apps_focused() == term,
              "Its taskbar button brings it back, in front");
        CLICK_ON(desktop_task_button(term->host));
        check(desktop_window_minimized(term->host), "Clicking the front window's taskbar button minimizes it");
        CLICK_ON(desktop_task_button(term->host));

        /* closing: Ctrl+W (not in the Terminal: there it deletes a word), Alt+F4 */
        { KbEvent cw = { KB_CTRL_OTHER, 'w' }; ui_kbd(&cw); frames(2);
          check(term->root && desktop_window_count() == 2, "Ctrl+W in the Terminal stays the shell's: the window stays open");
          KEY(KB_ALT_TAB); ui_kbd(&cw); frames(4); }
        check(!files->root && desktop_window_count() == 1 && apps_focused() == term,
              "Ctrl+W closes the Files window; the Terminal is in front again");
        KEY(KB_ALT_F4); frames(2);
        check(!term->root && desktop_window_count() == 0, "Alt+F4 closes the window in front");

        /* at most 6 windows, said clearly */
        desktop_open_window(WIN_SYSTEM); desktop_open_window(WIN_ABOUT); desktop_open_window(WIN_SHORTCUTS);
        ui_open_app(APP_CALCULATOR); ui_open_app(APP_NOTES); ui_open_app(APP_CLOCK); frames(4);
        ui_open_app(APP_TASKMGR); frames(2);
        check(desktop_window_count() == DESKTOP_MAX_WINDOWS && !apps_get(APP_TASKMGR)->root &&
              !strcmp(ui_toast_text(), "All 6 windows are in use: close one first"),
              "Six windows open: a seventh is refused with a clear message");
        save("core_desk_5_six_windows");
        for (int i = 0; i < 8 && desktop_window_count(); i++) KEY(KB_ALT_F4);
        check(desktop_window_count() == 0, "Alt+F4 six times closes them all (info windows too)");

        /* the desktop menu: right-click, X on the controller, keyboard, a click outside */
        ui_mouse_at(400, 300, 0, 0); frames(1); ui_mouse_right_click(); frames(2);
        check(desktop_desk_menu_is_open(), "Right-click on the empty desktop opens the desktop menu");
        save("core_desk_6_desktop_menu");
        KEY(KB_UP); KEY(KB_UP);
        check(!strcmp(desktop_menu_selected_text(), "Keyboard shortcuts"), "Desktop menu: the arrows move (UP wraps to the last row)");
        KEY(KB_ENTER); frames(2);
        lv_obj_t *fw = desktop_front_window();
        check(!desktop_desk_menu_is_open() && desktop_window_count() == 1 && fw &&
              !strcmp(lv_label_get_text(lv_obj_get_child(lv_win_get_header(fw), 1)), "Keyboard shortcuts"),
              "Desktop menu: Enter on Keyboard shortcuts opens their window");
        save("core_desk_7_shortcuts");
        KEY(KB_ALT_F4);
        ui_mouse_at(400, 300, 0, 0); frames(1); ui_key(NZ_KEY_X); frames(2);
        int x_opens = desktop_desk_menu_is_open();
        ui_key(NZ_KEY_B); frames(2);
        check(x_opens && !desktop_desk_menu_is_open(), "Controller: X at the pointer opens the desktop menu, B closes it");
        ui_mouse_right_click(); frames(2);
        ui_mouse_at(600, 60, 0, 1); frames(3);
        check(!desktop_desk_menu_is_open() && desktop_window_count() == 0, "A click outside the desktop menu closes it");
        ui_mouse_at(46, 44, 0, 0); frames(1); ui_mouse_right_click(); frames(2);
        check(!desktop_desk_menu_is_open(), "Right-click on a desktop icon: no desktop menu");
        char wall_was[16];
        int wall_ok = 1;
        snprintf(wall_was, sizeof wall_was, "%s", desktop_wallpaper_shown());
        for (int i = 0; i < 5; i++) {                      /* Wallpaper style, five times: all looks, then back */
            ui_mouse_at(400, 300, 0, 0); frames(1); ui_mouse_right_click(); frames(2);
            { KbEvent w = { KB_CHAR, 'w' }; ui_kbd(&w); frames(1); }
            KEY(KB_ENTER);
            wall_ok &= !strncmp(ui_toast_text(), "Wallpaper: ", 11) || !strncmp(ui_toast_text(), "Put a picture", 13);
            if (!strcmp(desktop_wallpaper_shown(), "sky")) save("core_desk_8_wallpaper_sky");
        }
        check(wall_ok && !strcmp(desktop_wallpaper_shown(), wall_was) && !strcmp(settings_get("desk_wall", ""), wall_was),
              "Desktop menu: Wallpaper style goes through the 5 looks and back (saved by name)");

        /* the tray: Wi-Fi, Bluetooth, volume, battery, clock */
        static const char *const tray_says[TRAY_COUNT] = { "Wi-Fi: ", "Bluetooth: ", "Volume: no sound device here", "Battery: ", "" };
        int tray_ok = 1;
        batt_clock = hw_now(); battery_reading(151, 0);     /* a battery (48 %): its tray item is shown only when known */
        frames(70);                                         /* (the status bar is redrawn once a second) */
        lv_area_t tb;
        lv_obj_get_coords(desktop_tray_item(TRAY_WIFI), &tb);
        for (int i = 0; i < TRAY_COUNT; i++) {
            lv_obj_get_coords(desktop_tray_item(i), &a);
            int on_bar = a.y1 >= NZ_H - 34 && a.y2 < NZ_H && a.x1 >= tb.x1 && a.x2 < NZ_W;
            CLICK_ON(desktop_tray_item(i));
            int said = i == TRAY_CLOCK ? strstr(ui_toast_text(), "day ") != NULL : !strncmp(ui_toast_text(), tray_says[i], strlen(tray_says[i]));
            if (!on_bar || !said) printf("NOTE  tray item %d: on the bar %d, said '%s'\n", i, on_bar, ui_toast_text());
            tray_ok &= on_bar && said;
        }
        check(tray_ok, "Taskbar tray: Wi-Fi, Bluetooth, volume, battery and clock (the date) each answer a click");
        unlink(FAKE_LEVELS); battery_reset();               /* (no battery again) */
        save("core_desk_9_tray_date");

        /* the mouse wheel scrolls the list under the pointer (Settings in a window) */
        ui_open_app(APP_SETTINGS); frames(6);
        lv_obj_t *sc = scrollable_in(apps_get(APP_SETTINGS)->host);
        int y0 = sc ? lv_obj_get_scroll_y(sc) : -1, y1 = -1, y2 = -1;
        if (sc) {
            lv_obj_get_coords(sc, &a);
            ui_mouse_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, 0, 0); frames(1);
            ui_mouse_wheel(-2); frames(2); y1 = lv_obj_get_scroll_y(sc);
            ui_mouse_wheel(2); frames(2); y2 = lv_obj_get_scroll_y(sc);
        }
        check(sc && y1 > y0 && y2 == y0, "Mouse wheel: down scrolls the list under the pointer, up scrolls it back");
        apps_close(apps_get(APP_SETTINGS)); frames(3);

        /* ---- the Retro Blue theme (theme 6) and the Hills / Picture wallpapers ---- */
        #define WALL_BMP "/tmp/fake_sdcard/nzeus/wallpaper.bmp"
        unlink(WALL_BMP);
        int th_before = theme_index();
        char wall_before[16];
        snprintf(wall_before, sizeof wall_before, "%s", settings_get("desk_wall", "glow"));
        theme_set(TH_RETRO_BLUE); frames(3);
        check(TH_COUNT == 6 && theme()->retro && !strcmp(theme()->name, "Retro Blue") && !strcmp(settings_get("theme", ""), "retro_blue"),
              "Retro Blue: theme 6 exists, is chosen and saved");
        check(!strcmp(desktop_wallpaper_shown(), "hills") && desktop_wallpaper_has_image(),
              "Retro Blue (no wallpaper.jpg): the desktop gets the Hills wallpaper");
        uint16_t sky = frame[20 * NZ_W + 600], hill = frame[420 * NZ_W + 630], bar = frame[470 * NZ_W + 320], go = frame[452 * NZ_W + 8];
        #define R5(p) ((p) >> 11)
        #define G6(p) (((p) >> 5) & 63)
        #define B5(p) ((p) & 31)
        check(B5(sky) > R5(sky) + 8 && G6(hill) / 2 > R5(hill) + 4 && G6(hill) / 2 > B5(hill) + 4 &&
              B5(bar) > R5(bar) + 8 && G6(go) / 2 > R5(go) + 4,
              "Retro Blue desktop: blue sky, a green hill, the blue taskbar, the green start button");
        save("core_retro_1_desktop");
        ui_open_app(APP_FILES); frames(6);
        save("core_retro_2_window");
        lv_obj_t *rw = apps_get(APP_FILES)->host;
        lv_area_t ra;
        lv_obj_get_coords(rw, &ra);
        uint16_t tbar = frame[(ra.y1 + 6) * NZ_W + ra.x1 + 100];
        check(B5(tbar) > R5(tbar) + 8, "Retro Blue window: a blue title bar");
        KEY(KB_SUPER); save("core_retro_3_start_menu"); KEY(KB_ESC);
        apps_close(apps_get(APP_FILES)); frames(3);
        ui_set_mode(UI_HANDHELD); frames(3);
        save("core_retro_4_home");
        ui_open_app(APP_SETTINGS); frames(6);
        save("core_retro_5_settings");
        apps_close(apps_get(APP_SETTINGS)); frames(3);
        ui_set_mode(UI_DESKTOP); frames(2);

        /* a test picture (made here: 640 x 446, a 1-pixel check pattern) is shown 1:1 */
        write_test_bmp(WALL_BMP, NZ_W, NZ_H - 34, 1);
        settings_set("desk_wall", "hills");
        desktop_cycle_wallpaper(); frames(3);              /* Hills -> Picture */
        int exact = 1;
        for (int y = 180; y < 184; y++)
            for (int x = 400; x < 404; x++)
                exact &= frame[y * NZ_W + x] == (((x + y) & 1) ? 0xF800 : 0x001F);   /* red / blue, as in the file */
        check(!strcmp(desktop_wallpaper_shown(), "picture") && desktop_wallpaper_has_image() && exact,
              "Wallpaper Picture: nzeus/wallpaper.bmp (640 x 446) is shown pixel for pixel");
        save("core_retro_6_picture");
        /* a picture of another shape covers the whole desktop */
        write_test_bmp(WALL_BMP, 200, 50, 0);
        settings_set("desk_wall", "hills");
        desktop_cycle_wallpaper(); frames(3);
        check(!strcmp(desktop_wallpaper_shown(), "picture") && frame[440 * NZ_W + 635] == 0x07E0 && frame[2 * NZ_W + 300] == 0x07E0,
              "Wallpaper Picture: a small, wide picture is made to cover the whole desktop");
        /* leaving and choosing Retro Blue again: the picture is its wallpaper now */
        theme_set(TH_ORIGINAL); frames(2);
        settings_set("desk_wall", "glow");
        theme_set(TH_RETRO_BLUE); frames(3);
        check(!strcmp(desktop_wallpaper_shown(), "picture"), "Retro Blue with a wallpaper picture on the card: the picture is the wallpaper");
        /* the picture missing: a clear message, Hills instead */
        unlink(WALL_BMP);
        settings_set("desk_wall", "hills");
        desktop_cycle_wallpaper(); frames(3);
        check(!strcmp(ui_toast_text(), "Put a picture named wallpaper.jpg in the nzeus folder") &&
              !strcmp(desktop_wallpaper_shown(), "hills") && desktop_wallpaper_has_image(),
              "Wallpaper Picture without a file: says where to put one, shows Hills");
        /* memory: 20 wallpaper changes (with a picture on the card) keep nothing */
        write_test_bmp(WALL_BMP, 320, 223, 0);
        settings_set("desk_wall", "plain");
        desktop_cycle_wallpaper(); frames(2);              /* (warm-up: Plain -> Hills) */
        for (int i = 0; i < 5; i++) { desktop_cycle_wallpaper(); frames(1); }   /* (back to Hills, every look once) */
        long wall_heap = heap_used();
        for (int i = 0; i < 20; i++) { desktop_cycle_wallpaper(); frames(1); }   /* 4 x all 5 looks: back to Hills */
        long wall_grew = heap_used() - wall_heap;
        printf("INFO  20 wallpaper changes: memory %+ld bytes\n", wall_grew);
        check(!strcmp(desktop_wallpaper_shown(), "hills") && wall_grew < 8192 && wall_grew > -8192,
              "Wallpapers: 20 changes (Hills, Picture: 570 KB each) leave no memory behind");
        unlink(WALL_BMP);

        /* ---- the owner's own icons: start logo, desktop and menu icons ---- */
        #define OWN "/tmp/fake_sdcard/nzeus"
        mkdir(OWN "/icons", 0755);
        unlink(OWN "/start-icon.bmp"); unlink(OWN "/icons/files.bmp"); unlink(OWN "/icons/web.bmp");
        settings_set("desk_wall", "plain"); desktop_cycle_wallpaper(); frames(2);   /* (Plain -> Hills) */
        desktop_reload_own_icons(); frames(2);
        check(desktop_own_icon_count() == 0 && lv_obj_has_flag(desktop_start_icon(), LV_OBJ_FLAG_HIDDEN) &&
              !lv_obj_has_flag(desktop_icon_part(0, 0), LV_OBJ_FLAG_HIDDEN),
              "Own icons: no files on the card: no start logo, the built-in icons");
        check(!lv_obj_has_flag(desktop_icon_part(0, 2), LV_OBJ_FLAG_HIDDEN) &&
              lv_color_to32(lv_obj_get_style_img_recolor(desktop_icon_part(0, 0), 0)) == lv_color_to32(lv_color_hex(0xE0A010)) &&
              lv_color_to32(lv_obj_get_style_img_recolor(desktop_icon_part(3, 0), 0)) == lv_color_to32(lv_color_hex(0xD32F2F)),
              "Retro Blue desktop icons: each on a tile, in its own colour (Files yellow, Car red)");
        save("core_icons_0_builtin_retro");
        write_icon_bmp(OWN "/start-icon.bmp", 20, 0xFF0000);       /* a red square, green all round */
        write_icon_bmp(OWN "/icons/files.bmp", 48, 0x0000FF);      /* bigger than 32: made smaller */
        { FILE *bad = fopen(OWN "/icons/web.bmp", "w"); if (bad) { fputs("not a picture", bad); fclose(bad); } }
        desktop_reload_own_icons(); frames(3);
        lv_obj_t *si = desktop_start_icon();
        lv_area_t ia;
        lv_obj_get_coords(si, &ia);
        uint16_t mid = frame[((ia.y1 + ia.y2) / 2) * NZ_W + (ia.x1 + ia.x2) / 2], edge = frame[(ia.y1 + 1) * NZ_W + ia.x1 + 1];
        check(!lv_obj_has_flag(si, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_width(si) == 20 && mid == 0xF800 &&
              edge != 0x07E0 && G6(edge) / 2 > R5(edge) + 4,
              "Own start logo: nzeus/start-icon.bmp is shown; its pure green is see-through (the button shows)");
        lv_obj_t *fo = desktop_icon_part(0, 1);
        lv_obj_get_coords(fo, &ia);
        mid = frame[((ia.y1 + ia.y2) / 2) * NZ_W + (ia.x1 + ia.x2) / 2];
        edge = frame[(ia.y1 + 1) * NZ_W + ia.x1 + 1];
        check(!lv_obj_has_flag(fo, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(desktop_icon_part(0, 0), LV_OBJ_FLAG_HIDDEN) &&
              lv_obj_get_width(fo) == 32 && mid == 0x001F && edge != 0x07E0,
              "Own desktop icon: icons/files.bmp (48 x 48) replaces Files' icon at 32 x 32, green see-through");
        check(!lv_obj_has_flag(desktop_icon_part(2, 0), LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(desktop_icon_part(2, 1), LV_OBJ_FLAG_HIDDEN),
              "Own desktop icon: a broken icons/web.bmp is ignored (the built-in Web icon)");
        check(desktop_own_icon_count() == 4, "Own icons: start logo + Files on the desktop, in the start menu and the desktop menu");
        mkdir(OWN "/icons/small", 0755);
        write_icon_bmp(OWN "/icons/small/files.bmp", 16, 0xFF0000); /* a ready-made small copy */
        desktop_reload_own_icons(); frames(2);
        check(desktop_menu_own_icon_w(0) == 16 && lv_obj_get_width(desktop_icon_part(0, 1)) == 32,
              "Own icons: the start menu uses icons/small/files.bmp first; the desktop keeps icons/files.bmp");
        unlink(OWN "/icons/small/files.bmp");
        desktop_reload_own_icons(); frames(2);
        check(desktop_menu_own_icon_w(0) == 20, "Own icons: no small copy: the menu shrinks icons/files.bmp to 20 x 20");
        save("core_icons_1_own");
        KEY(KB_SUPER); save("core_icons_2_menu"); KEY(KB_ESC);
        long icons_heap = heap_used();
        for (int i = 0; i < 20; i++) desktop_reload_own_icons();
        frames(2);
        long icons_grew = heap_used() - icons_heap;
        printf("INFO  20 own-icon reloads: memory %+ld bytes\n", icons_grew);
        check(desktop_own_icon_count() == 4 && icons_grew < 4096 && icons_grew > -4096, "Own icons: loading them 20 times keeps no memory");
        theme_set(TH_ORIGINAL); frames(3);
        check(!lv_obj_has_flag(desktop_start_icon(), LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(desktop_icon_part(0, 2), LV_OBJ_FLAG_HIDDEN),
              "Own start logo in the other themes too (on the NZEUS button); no Retro tiles there");
        save("core_icons_3_original");
        theme_set(TH_RETRO_BLUE); frames(2);
        unlink(OWN "/start-icon.bmp"); unlink(OWN "/icons/files.bmp"); unlink(OWN "/icons/web.bmp");
        desktop_reload_own_icons(); frames(2);
        check(desktop_own_icon_count() == 0 && lv_obj_has_flag(desktop_start_icon(), LV_OBJ_FLAG_HIDDEN) &&
              !lv_obj_has_flag(desktop_icon_part(0, 0), LV_OBJ_FLAG_HIDDEN),
              "Own icons: files removed: back to the built-in icons");
        #undef OWN
        /* back to the theme and wallpaper from before */
        settings_set("desk_wall_auto", "0");
        theme_set(th_before); frames(2);
        settings_set("desk_wall", wall_before);
        desktop_cycle_wallpaper(); for (int i = 0; i < 4; i++) desktop_cycle_wallpaper();
        frames(2);
        check(theme_index() == th_before && !strcmp(settings_get("desk_wall", ""), wall_before), "Retro Blue tests: theme and wallpaper back as before");
        #undef R5
        #undef G6
        #undef B5
        #undef WALL_BMP
        ui_set_mode(UI_HANDHELD); frames(2);
        #undef KEY
        #undef CLICK_ON
    }

    /* ---- Claude: PIN, access switch, the terminal (a fake brain script) ---- */
    {
        char hx[65];
        sha256_hex("abc", 3, hx);
        check(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 is right (test vector \"abc\")");
        /* the key vault's building blocks against their published test vectors */
        uint8_t mac[32], k0b[20], dk[32], ck[32], cn[12] = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
        char hexs[65];
        memset(k0b, 0x0b, sizeof k0b);
        hmac_sha256(k0b, 20, (const uint8_t *)"Hi There", 8, mac);
        for (int i = 0; i < 32; i++) sprintf(hexs + i * 2, "%02x", mac[i]);
        check(!strcmp(hexs, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC-SHA256 is right (RFC 4231 test 1)");
        pbkdf2_sha256("password", (const uint8_t *)"salt", 4, 1, dk, 32);
        for (int i = 0; i < 32; i++) sprintf(hexs + i * 2, "%02x", dk[i]);
        check(!strcmp(hexs, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"), "PBKDF2-HMAC-SHA256 is right (known vector)");
        for (int i = 0; i < 32; i++) ck[i] = (uint8_t)i;
        uint8_t msg[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        chacha20_xor(ck, cn, 1, msg, 114);
        check(msg[0] == 0x6e && msg[1] == 0x2e && msg[2] == 0x35 && msg[3] == 0x9a && msg[113] == 0x4d, "ChaCha20 is right (RFC 8439 2.4.2)");
        /* the console's key is made with Dropbear: the PC (x86) build stands in */
        if (system("cp '" NZ_REPO "/apps/dropbear/x86/dropbearmulti' /tmp/fake_sdcard/nzeus/bin/ 2>/dev/null") != 0)
            printf("NOTE  x86 dropbearmulti not found: key tests will fail\n");
        FILE *fs = fopen("/tmp/fake_claude.sh", "w");     /* stands in for nzeus-claude-connect.sh */
        /* after "got:..." it waits for one more line, prints 60 lines (for scrolling back), then
         * switches its terminal to raw mode and writes every byte it gets to /tmp/fake_claude_in */
        if (fs) { fputs("echo \"FAKE CLAUDE $1 $2 $3\"; [ -s \"$NZ_CONSOLE_KEY\" ] && echo KEY-UNLOCKED; read line; echo \"got:$line\"; "
                        "read go; i=1; while [ $i -le 60 ]; do echo \"old message $i\"; i=$((i+1)); done; "
                        "stty raw -echo; echo RAW-READY; dd bs=1 count=200 of=/tmp/fake_claude_in 2>/dev/null; sleep 30\n", fs); fclose(fs); }
        unlink("/tmp/fake_claude_in");
        claude_connect_script = "/tmp/fake_claude.sh";
        settings_set("claude_pc_host", "");                  /* no PC yet: Connect says so */
        /* the network: hw_modern_online() reads /proc/net/route (a fake here) - no default route yet */
        FILE *rt = fopen(hw_route_file, "w");
        if (rt) { fputs("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
                        "lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\n"
                        "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\n", rt); fclose(rt); }
        ui_open_app(APP_CLAUDE); frames(3);
        check(apps_focused() && apps_focused()->id == APP_CLAUDE && !app_claude_terminal(), "Claude: the Connect page opens");
        check(popup_label(apps_focused()->root, "no internet yet", 12) && !popup_label(apps_focused()->root, "Claude access", 12),
              "Claude: no default route (only lo, a local net) = 'no internet yet'; no 'Claude access' row on this system");
        save("core_16_claude");
        tap(13); frames(3);                               /* A on Connect (selected) without a PIN */
        check(!app_claude_terminal() && !nzui_active(), "Claude: no connection before a PIN is chosen");
        tap(2); tap(13); frames(2);                       /* UP to "Choose a PIN", A */
        type_line("12x4"); frames(2);
        check(!settings_get("claude_pin", NULL), "Claude: a PIN must be digits");
        tap(13); type_line("2468"); frames(2); type_line("2468"); frames(2);
        check(settings_get("claude_pin", NULL) && !file_contains(NZ_SDCARD "/nzeus/settings.ini", "claude_pin=2468"),
              "Claude: the PIN is saved only as a hash (never the digits)");
        check(file_starts_with(NZ_SDCARD "/nzeus/claude/console_key.enc", "NZK1") &&
              file_starts_with(NZ_SDCARD "/nzeus/claude/console_key.pub", "ssh-ed25519") &&
              access(NZ_SDCARD "/nzeus/claude/console_key", F_OK) != 0 && access("/tmp/nzeus-run/console_key", F_OK) != 0,
              "Claude: the console's key is made and kept only LOCKED with the PIN (+ its public half)");
        check(vault_decrypt_file(NZ_SDCARD "/nzeus/claude/console_key.enc", "/tmp/vault_try", "1111") == -1 &&
              access("/tmp/vault_try", F_OK) != 0, "Claude: a wrong PIN can't unlock the key");
        tap(3); tap(13); frames(3);                       /* A on Connect without a network */
        check(!app_claude_terminal() && strstr(ui_toast_text(), "No network yet") && !nzui_active(),
              "Claude: Connect without a network says so (and asks nothing)");
        tap(2);                                           /* (back up to the PIN row) */
        rt = fopen(hw_route_file, "a");                   /* Wi-Fi joined: a default route via the router */
        if (rt) { fputs("wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\n", rt); fclose(rt); }
        for (int i = 0; i < 200 && !popup_label(apps_focused()->root, "ready", 12); i++) frames(1);   /* (rows refresh every 2 s) */
        check(popup_label(apps_focused()->root, "ready", 12) != NULL, "Claude: a default route up = the network row says 'ready'");
        tap(3); tap(13); type_line("2468"); frames(5);    /* Connect, right PIN, but no PC set yet */
        check(!app_claude_terminal() && strstr(ui_toast_text(), "Set the brain's address and user first"),
              "Claude: a fresh console (no PC set) says to set the address first, connects nowhere");
        settings_set("claude_pc_host", "192.168.1.10");     /* now a PC (what A on the Address row saves) */
        settings_set("claude_pc_user", "user");
        tap(13); type_line("1111"); frames(5);            /* Connect, wrong PIN */
        check(!app_claude_terminal(), "Claude: a wrong PIN does not connect");
        tap(13); type_line("2468");                       /* right PIN */
        char row[128] = "";
        for (int i = 0; i < 100 && !strstr(row, "FAKE CLAUDE"); i++) { frames(1); if (app_claude_terminal()) vtw_row_text(app_claude_terminal(), 0, row, sizeof row); }
        check(strstr(row, "FAKE CLAUDE 192.168.1.10 22 user") != NULL, "Claude: the right PIN connects to the brain (Home PC, port, user)");
        KbEvent hk = { KB_CHAR, 'h' }, ik = { KB_CHAR, 'i' };
        ui_kbd(&hk); ui_kbd(&ik); tap(13);               /* typed keys + A (= Enter) go to Claude */
        for (int i = 0; i < 100 && !strstr(row, "got:hi"); i++) {
            frames(1);
            for (int r = 0; r < 4 && app_claude_terminal(); r++) { vtw_row_text(app_claude_terminal(), r, row, sizeof row); if (strstr(row, "got:hi")) break; }
        }
        check(strstr(row, "got:hi") != NULL, "Claude: keys and A (Enter) reach Claude Code");
        int unlocked = 0;
        for (int r = 0; r < 4 && app_claude_terminal(); r++) { vtw_row_text(app_claude_terminal(), r, row, sizeof row); if (strstr(row, "KEY-UNLOCKED")) unlocked = 1; }
        check(unlocked, "Claude: for the connection the key is unlocked into RAM (NZ_CONSOLE_KEY)");
        /* connected: the chat view comes first; Y shows the full terminal (the tests below use it) */
        check(app_claude_chat_shown(), "Claude chat: once connected, the chat view (bubbles) is shown first");
        tap(15); frames(2);                               /* Y */
        check(!app_claude_chat_shown() && app_claude_terminal() && !lv_obj_has_flag(lv_obj_get_parent(app_claude_terminal()), LV_OBJ_FLAG_HIDDEN),
              "Claude chat: Y shows the full terminal instead");
        if (app_claude_terminal()) {                       /* air at the sides: text not on the screen edge */
            lv_area_t va;
            lv_obj_get_coords(app_claude_terminal(), &va);
            check(va.x1 >= 10 && va.x2 <= NZ_W - 11, "Claude: the terminal keeps a margin at both screen edges");
        }
        save("core_17_claude_terminal");

        /* scrolling back (L1 / R1): 60 more lines push the first ones off the top */
        {
            lv_obj_t *cv = app_claude_terminal();
            KbEvent mk = { KB_CHAR, 'm' };
            ui_kbd(&mk); tap(13);                         /* "m" + A: the fake Claude prints 60 lines */
            int ready = 0;
            for (int i = 0; i < 300 && cv && !ready; i++) {
                frames(1);
                for (int r = 0; r < 30; r++) if (strstr(vtw_row_text(cv, r, row, sizeof row), "RAW-READY")) ready = 1;
            }
            frames(5);
            int kept = cv ? vtw_kept_lines(cv) : 0;
            char in[64];
            check(ready && kept > 20 && vtw_scroll_offset(cv) == 0 && !app_claude_marker_shown(),
                  "Claude: lines that scroll off the top are kept (the view stays live, no sign)");
            tap(10); frames(2);                           /* L1: a page back */
            int page1 = cv ? vtw_scroll_offset(cv) : 0;
            vtw_view_text(cv, 0, row, sizeof row);
            check(page1 >= 10 && app_claude_marker_shown() && !strncmp(row, "old message", 11),
                  "Claude: L1 scrolls back a page (earlier messages) and shows the SCROLLED BACK sign");
            for (int i = 0; i < 6; i++) tap(10);          /* L1 to the very first line */
            vtw_view_text(cv, 0, row, sizeof row);
            check(cv && vtw_scroll_offset(cv) == kept && strstr(row, "FAKE CLAUDE 192.168.1.10") != NULL,
                  "Claude: L1 again goes back to the very first message (it was kept)");
            save("core_claude_scrolled");
            theme_set(TH_GREEN_CRT); frames(3); save("core_claude_scrolled_crt"); theme_set(TH_ORIGINAL); frames(2);
            tap(3);                                       /* DOWN: one line forward */
            int down1 = vtw_scroll_offset(cv) == kept - 1;
            tap(2);                                       /* UP: one line back */
            check(down1 && vtw_scroll_offset(cv) == kept && file_read("/tmp/fake_claude_in", in, sizeof in) == 0,
                  "Claude: while scrolled back UP / DOWN move one line (nothing goes to Claude)");
            for (int i = 0; i < 10 && vtw_scroll_offset(cv) > 0; i++) tap(11);   /* R1 until the live screen */
            check(vtw_scroll_offset(cv) == 0 && !app_claude_marker_shown(), "Claude: R1 pages forward to the live screen (the sign goes)");
            tap(10); tap(14); frames(3);                  /* L1, then B */
            check(vtw_scroll_offset(cv) == 0 && !app_claude_marker_shown() && file_read("/tmp/fake_claude_in", in, sizeof in) == 0,
                  "Claude: while scrolled back any other button (B) returns to the live screen and is not sent");
            tap(1); frames(2); tap(3); frames(2);         /* START: the menu, DOWN: Old chats */
            save("core_claude_menu");
            theme_set(TH_AMBER_CRT); frames(3); save("core_claude_menu_crt"); theme_set(TH_ORIGINAL); frames(2);
            tap(13);                                      /* A: Old chats */
            long n = 0;
            for (int i = 0; i < 100 && n < 8; i++) { frames(1); n = file_read("/tmp/fake_claude_in", in, sizeof in); }
            frames(10);
            n = file_read("/tmp/fake_claude_in", in, sizeof in);
            check(n == 8 && !memcmp(in, "/resume\r", 8), "Claude: START > Old chats types exactly /resume + Enter");
            tap(10); frames(2);                           /* L1, then a key on the USB keyboard */
            int was_back = vtw_scroll_offset(cv) > 0;
            KbEvent zk = { KB_CHAR, 'z' };
            ui_kbd(&zk);
            for (int i = 0; i < 50 && n < 9; i++) { frames(1); n = file_read("/tmp/fake_claude_in", in, sizeof in); }
            check(was_back && vtw_scroll_offset(cv) == 0 && !app_claude_marker_shown() && n == 9 && in[8] == 'z',
                  "Claude: a typed key while scrolled back returns to the live screen (and is sent)");
            tap(11); frames(3);                           /* R1 on the live screen: the chat view */
            int r1_chat = app_claude_chat_shown();
            tap(1); frames(2); tap(3); frames(1); tap(3); frames(1); tap(13); frames(3);   /* START, DOWN x2: Show the terminal */
            n = file_read("/tmp/fake_claude_in", in, sizeof in);
            check(r1_chat && !app_claude_chat_shown() && app_claude_terminal() == cv && n == 9,
                  "Claude chat: R1 on the live terminal shows the chat; START > Show the terminal goes back (nothing sent)");
        }
        tap(1); frames(2); tap(13); frames(3);            /* START: menu, A: Disconnect */
        check(!app_claude_terminal(), "Claude: START > Disconnect closes the connection");
        check(access("/tmp/nzeus-run/console_key", F_OK) != 0, "Claude: after the connection the unlocked key is gone");
        tap(2); tap(13); type_line("0000"); frames(2);           /* UP: Change PIN, a WRONG current PIN */
        check(!nzui_active(), "Claude: changing the PIN needs the current PIN");
        if (nzui_active()) { nzui_key(NZ_KEY_B); frames(2); }
        tap(13); type_line("2468"); frames(2);                   /* the right current PIN */
        type_line("1357"); frames(2); type_line("1357"); frames(5);
        check(vault_decrypt_file(NZ_SDCARD "/nzeus/claude/console_key.enc", "/tmp/vault_try", "1357") == 0 &&
              vault_decrypt_file(NZ_SDCARD "/nzeus/claude/console_key.enc", "/tmp/vault_try2", "2468") == -1,
              "Claude: changing the PIN re-locks the key with the new PIN");
        unlink("/tmp/vault_try");
        tap(14); frames(3);
    }

    /* ==== Claude chat view (added 2026-10-07): the conversation as bubbles ====
     * A FAKE brain prints a screen like Claude Code's: the welcome box, "> " lines (mine),
     * reply dots (Claude's), a tool's output, an Arabic answer, a line Claude Code wrapped,
     * a list, then the input box + hints at the bottom. For every line it gets it answers
     * like Claude Code ("> line", then a reply dot) and draws the input box again ("many":
     * 30 questions and answers). (The Claude PIN is 1357 here: set by the tests above.) */
    {
        #define DOT "\xe2\x8f\xba"                                /* the reply dot (U+23FA) */
        #define CORNER "\xe2\x8e\xbf"                             /* the tool output sign (U+23BF) */
        #define HL "\xe2\x94\x80"                                 /* a piece of box line */
        #define HL10 HL HL HL HL HL HL HL HL HL HL
        #define VL "\xe2\x94\x82"
        #define AR "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"     /* "marhaba" */
        #define AR3 "\xd8\xb5\xd8\xaf\xd9\x8a\xd9\x82\xd9\x8a"    /* "sadiqi" */
        #define BT(i) (app_claude_chat_label(i) ? lv_label_get_text(app_claude_chat_label(i)) : "")
        FILE *fs = fopen("/tmp/fake_chat_screen.txt", "w");
        if (fs) {
            fputs("\xe2\x95\xad" HL10 HL10 HL10 HL10 "\xe2\x95\xae\n"
                  VL " \xe2\x9c\xbb Welcome to Claude Code!                \xe2\x94\x82\n"
                  VL "                                          " VL "\n"
                  VL "   cwd: /home/user/nzeus-claude          " VL "\n"
                  "\xe2\x95\xb0" HL10 HL10 HL10 HL10 "\xe2\x95\xaf\n\n"
                  "> hello claude\n\n"
                  DOT " Hello! How can I help you today?\n\n"
                  "> say hi in arabic\n\n"
                  DOT " " AR " " AR3 " - hello in Arabic\n\n"
                  "> list the files\n\n"
                  DOT " Bash(ls)\n"
                  "  " CORNER "  notes.txt\n"
                  "     rocket.py\n\n"
                  DOT " There are two files here: notes.txt and rocket.py. This sentence is long\n"
                  "  enough that Claude Code wraps it onto a second line at the edge.\n\n"
                  "> count to twenty\n\n"
                  DOT " Counting:\n", fs);
            for (int i = 1; i <= 20; i++) fprintf(fs, "  - item %d\n", i);
            fputs("\n" DOT " Done counting.\n\n", fs);
            fclose(fs);
        }
        fs = fopen("/tmp/fake_chat_box.txt", "w");                /* the input box and the hints under it */
        if (fs) { fputs(HL10 HL10 HL10 HL10 HL10 HL10 "\n> \n" HL10 HL10 HL10 HL10 HL10 HL10 "\n  ? for shortcuts\n", fs); fclose(fs); }
        fs = fopen("/tmp/fake_chat.sh", "w");
        if (fs) { fputs("stty -echo; cat /tmp/fake_chat_screen.txt /tmp/fake_chat_box.txt\n"
                        "while read line; do\n"
                        "  echo \"$line\" >> /tmp/fake_chat_in\n"
                        "  printf '\\033[4A\\033[J'\n"                    /* up to the input box, clear it */
                        "  if [ \"$line\" = many ]; then\n"
                        "    printf '> many\\n\\n'; i=1\n"
                        "    while [ $i -le 30 ]; do printf '> question %d\\n\\n" DOT " answer %d\\n\\n' $i $i; i=$((i+1)); done\n"
                        "  else printf '> %s\\n\\n" DOT " You said: %s\\n\\n' \"$line\" \"$line\"; fi\n"
                        "  cat /tmp/fake_chat_box.txt\n"
                        "done; sleep 30\n", fs); fclose(fs); }
        unlink("/tmp/fake_chat_in");
        claude_connect_script = "/tmp/fake_chat.sh";
        ui_open_app(APP_CLAUDE); frames(3);
        tap(13); type_line("1357");                               /* Connect (selected), the PIN */
        for (int i = 0; i < 300 && app_claude_chat_count() < 11; i++) frames(1);
        frames(30);
        lv_obj_t *cl = app_claude_chat_list(), *cvt = app_claude_terminal();
        int nb = app_claude_chat_count();
        check(app_claude_chat_shown() && nb == 11, "Claude chat: the conversation shows as 11 bubbles (input box + hints left out)");
        check(nb == 11 && app_claude_chat_mine(1) && app_claude_chat_mine(3) && app_claude_chat_mine(5) && app_claude_chat_mine(8) &&
              !app_claude_chat_mine(0) && !app_claude_chat_mine(2) && !app_claude_chat_mine(4) && !app_claude_chat_mine(6) &&
              !app_claude_chat_mine(10) && !strcmp(BT(1), "hello claude") && !strcmp(BT(2), "Hello! How can I help you today?"),
              "Claude chat: my \"> \" lines are my bubbles, Claude's replies (round dot) are Claude's");
        int noise = 0;
        for (int i = 0; i < nb; i++) noise += has_tui_noise(BT(i)) || strstr(BT(i), "for shortcuts") != NULL;
        check(noise == 0 && !strcmp(BT(0), "* Welcome to Claude Code!\n\ncwd: /home/user/nzeus-claude") &&
              !strcmp(BT(6), "Bash(ls)\nnotes.txt\nrocket.py"),
              "Claude chat: box lines, reply signs and the tool output sign are stripped (plain words)");
        check(strstr(BT(7), "This sentence is long enough that Claude Code wraps") != NULL &&
              strstr(BT(9), "Counting:\n- item 1\n- item 2\n") != NULL && !strcmp(BT(10), "Done counting."),
              "Claude chat: a line Claude Code wrapped is joined again; list lines stay lines");
        const char *at = BT(4);
        check(arabic_count(at, 0) == 0 && arabic_count(at, 1) >= 8 && app_claude_chat_label(4) &&
              lv_obj_get_style_text_align(app_claude_chat_label(4), 0) == LV_TEXT_ALIGN_RIGHT,
              "Claude chat: an Arabic answer is joined and right to left");
        int kept_ar = 0;
        char ln[256];
        for (int i = 0; cvt && i < vtw_kept_lines(cvt); i++) if (strstr(vtw_line_text(cvt, i, ln, sizeof ln), AR)) kept_ar = 1;
        check(kept_ar, "Claude chat: Arabic that went off the terminal's top is kept as Arabic (the chat reads it back)");
        check(cl && lv_obj_get_scroll_bottom(cl) <= 4, "Claude chat: it starts at the newest message (bottom)");
        save("core_claude_chat");
        theme_set(TH_GREEN_CRT); frames(20); save("core_claude_chat_crt"); theme_set(TH_ORIGINAL); frames(2);

        /* scrolling: L1 / UP to older messages, R1 / DOWN back to the newest */
        lv_coord_t y0 = cl ? lv_obj_get_scroll_y(cl) : 0;
        tap(10); frames(2);
        lv_coord_t y1 = cl ? lv_obj_get_scroll_y(cl) : 0;
        tap(2); frames(2);
        lv_coord_t y2 = cl ? lv_obj_get_scroll_y(cl) : 0;
        check(y1 < y0 - 200 && y2 < y1 && y2 >= 0 && app_claude_chat_shown(), "Claude chat: L1 scrolls up a page, UP a little (to older messages)");
        tap(10); frames(2); tap(10); frames(2);
        save("core_claude_chat_arabic");
        theme_set(TH_DAYLIGHT); frames(3); save("core_claude_chat_arabic_daylight"); theme_set(TH_ORIGINAL); frames(2);
        tap(3); frames(2);
        for (int i = 0; i < 10 && cl && lv_obj_get_scroll_bottom(cl) > 4; i++) tap(11);
        check(cl && lv_obj_get_scroll_bottom(cl) <= 4, "Claude chat: DOWN / R1 scroll back to the newest message");

        /* typing: X, the message, Enter */
        tap(12); frames(2);
        for (const char *c = "what is 2+2"; *c; c++) { KbEvent ev = { KB_CHAR, *c }; ui_kbd(&ev); }
        frames(2); save("core_claude_chat_typing");
        KbEvent en = { KB_ENTER, 0 };
        ui_kbd(&en);
        for (int i = 0; i < 300 && app_claude_chat_count() < 13; i++) frames(1);
        frames(20);
        check(app_claude_chat_count() == 13 && app_claude_chat_mine(11) && !strcmp(BT(11), "what is 2+2") && !app_claude_chat_mine(12) &&
              !strcmp(BT(12), "You said: what is 2+2") && file_contains("/tmp/fake_chat_in", "what is 2+2"),
              "Claude chat: X types a message, it is sent with Enter and shows as my bubble (+ the answer)");
        if (app_claude_chat_label(11) && app_claude_chat_label(12)) {
            lv_obj_t *mb = lv_obj_get_parent(app_claude_chat_label(11)), *cb = lv_obj_get_parent(app_claude_chat_label(12));
            lv_area_t as, am, ac;
            lv_obj_get_coords(cl, &as); lv_obj_get_coords(mb, &am); lv_obj_get_coords(cb, &ac);
            check(am.x2 > as.x2 - 30 && am.x1 > as.x1 + 100 && ac.x1 < as.x1 + 30 &&
                  is_accent_at(am.x1 + 3, (am.y1 + am.y2) / 2) && !is_accent_at(ac.x1 + 3, (ac.y1 + ac.y2) / 2),
                  "Claude chat: my message is on the right in the accent colour, Claude's on the left");
        } else check(0, "Claude chat: the typed message's bubbles");
        save("core_claude_chat_typed");
        theme_set(TH_AMBER_CRT); frames(20); save("core_claude_chat_typed_amber");
        check(cl && lv_obj_get_scroll_bottom(cl) <= 4, "Claude chat: after a theme change (bigger letters) it still shows the newest message whole");
        theme_set(TH_ORIGINAL); frames(20);

        /* the terminal: Y, typing there, R1 back to the chat */
        tap(15); frames(3);
        check(!app_claude_chat_shown() && app_claude_chat_count() == 0 && cvt && !lv_obj_has_flag(lv_obj_get_parent(cvt), LV_OBJ_FLAG_HIDDEN),
              "Claude chat: Y shows the full terminal (the bubbles are let go)");
        save("core_claude_chat_terminal");
        KbEvent o1 = { KB_CHAR, 'o' }, o2 = { KB_CHAR, 'k' };
        ui_kbd(&o1); ui_kbd(&o2); tap(13);                       /* typed on the terminal + A (Enter) */
        int said = 0;
        for (int i = 0; i < 300 && !said; i++) { frames(1); for (int r = 0; r < 20 && cvt; r++) if (strstr(vtw_row_text(cvt, r, ln, sizeof ln), "You said: ok")) said = 1; }
        check(said, "Claude chat: the terminal still works as before (keys + A reach Claude Code)");
        tap(11); frames(3);                                       /* R1 (on the live screen): the chat again */
        check(app_claude_chat_shown() && app_claude_chat_count() == 15 && app_claude_chat_mine(13) && !strcmp(BT(13), "ok"),
              "Claude chat: R1 on the terminal shows the chat again, with what was typed there");

        /* the START menu switches too */
        tap(1); frames(2); tap(3); frames(1); tap(3); frames(1); /* START, DOWN x2: Show the terminal */
        save("core_claude_chat_menu");
        tap(13); frames(3);
        int menu_term = !app_claude_chat_shown();
        tap(1); frames(2); tap(3); frames(1); tap(3); frames(1); tap(13); frames(3);   /* START > Show the chat */
        check(menu_term && app_claude_chat_shown() && app_claude_chat_count() == 15,
              "Claude chat: START > Show the terminal / Show the chat switch between them");

        /* a long conversation: only the newest 40 bubbles are made */
        tap(12); frames(2); type_line("many");
        int last_ok = 0;
        for (int i = 0; i < 400 && !last_ok; i++) { frames(1); int c = app_claude_chat_count(); last_ok = c > 0 && !strcmp(BT(c - 1), "answer 30"); }
        frames(5);
        int cn = app_claude_chat_count();
        check(last_ok && cn == 40 && app_claude_chat_mine(cn - 2) && !strcmp(BT(cn - 2), "question 30") && cl && lv_obj_get_scroll_bottom(cl) <= 4,
              "Claude chat: at most the newest 40 bubbles are made (the newest at the bottom)");

        /* START > Old chats: Claude Code's list is a menu, so the terminal shows it */
        tap(1); frames(2); tap(3); frames(1); tap(13); frames(3);
        int oc = 0;
        for (int i = 0; i < 100 && !oc; i++) { frames(1); oc = file_contains("/tmp/fake_chat_in", "/resume"); }
        check(oc && !app_claude_chat_shown() && app_claude_terminal(), "Claude chat: START > Old chats shows the terminal and types /resume");
        tap(1); frames(2); tap(13); frames(3);                   /* START > Disconnect */
        check(!app_claude_terminal() && !app_claude_chat_shown(), "Claude chat: Disconnect closes the connection (and the chat view)");
        tap(14); frames(3);
        #undef DOT
        #undef CORNER
        #undef HL
        #undef HL10
        #undef VL
        #undef AR
        #undef AR3
        #undef BT
    }

    /* ==== WhatsApp (added 2026-10-06): the PIN-locked key, the chat list, a chat, replies ====
     * A FAKE nzeus-wa.sh on the fake SD card answers with canned text (an Arabic chat, a
     * group with authors, a message with \n inside) and writes every command it got to
     * /tmp/fake_wa_log. Nothing ever reaches a real PC or WhatsApp.
     * (The Claude PIN is 1357 here: set by the Claude tests above.) */
    {
        #define WA_KEYDIR NZ_SDCARD "/nzeus/whatsapp"
        #define WA_LOG "/tmp/fake_wa_log"
        #define WA_WAIT(cond) for (int w_ = 0; w_ < 500 && !(cond); w_++) frames(1)
        FILE *ws = fopen(NZ_SDCARD "/nzeus/bin/nzeus-wa.sh", "w");
        if (ws) {
            fputs("echo \"$*\" >> " WA_LOG "\n"
                  "echo \"KEY=$NZ_WA_KEY HOST=$NZ_WA_HOST PORT=$NZ_WA_PORT USER=$NZ_WA_USER\" > /tmp/fake_wa_env\n"
                  "mode=$(cat /tmp/fake_wa_mode 2>/dev/null)\n"
                  "[ \"$mode\" = error ] && { echo 'error: WhatsApp is not linked on the PC'; exit 1; }\n"
                  "[ \"$mode\" = silent ] && exit 255\n"
                  "case \"$1\" in\n"
                  "--make-key) mkdir -p " WA_KEYDIR "; echo FAKE-WA-PRIVATE-KEY > " WA_KEYDIR "/wa_key\n"
                  "  echo 'ssh-ed25519 AAAAFAKE nzeus-console-wa' > " WA_KEYDIR "/wa_key.pub; echo ok ;;\n"
                  "chats) cat /tmp/fake_wa_chats ;;\n"
                  "read) cat \"/tmp/fake_wa_read_$2\" ;;\n"
                  "send) echo ok ;;\n"
                  /* photo CHAT MSG OUTFILE: a moment, then the photo is saved as OUTFILE (like the real script) */
                  "photo) sleep 0.3; [ -f /tmp/fake_wa_photo_fail ] && { echo 'error: the photo is gone'; exit 1; }\n"
                  "  cp /tmp/fake_wa_photo.jpg \"$4\" && echo ok ;;\n"
                  /* voice CHAT MSG OUTFILE: the same, with a WAV */
                  "voice) sleep 0.3; [ -f /tmp/fake_wa_voice_fail ] && { echo 'error: the voice message is gone'; exit 1; }\n"
                  "  cp /tmp/fake_wa_voice.wav \"$4\" && echo ok ;;\n"
                  "sendphoto) [ -f /tmp/fake_wa_send_fail ] && { echo 'error: please wait 5 s between messages'; exit 1; }; echo ok ;;\n"
                  "*) echo 'error: unknown command'; exit 2 ;;\n"
                  "esac\n", ws);
            fclose(ws);
        }
        long wnow = (long)hw_now();
        FILE *wc = fopen("/tmp/fake_wa_chats", "w");
        if (wc) {
            fprintf(wc, "15550000001@s.whatsapp.net\t2\t%ld\tأحمد\tمرحبا، كيف حالك؟ 😍\n", wnow - 60);
            fprintf(wc, "120363000000000001@g.us\t0\t%ld\tNZEUS Lab group\tSara: see you at 5\\nbring the board\n", wnow - 86400 * 2);
            fprintf(wc, "15550000002@s.whatsapp.net\t0\t%ld\tOmar 😀\tHello 😀 how are you?\n", wnow - 86400 * 10);
            fclose(wc);
        }
        /* Omar's chat: the new 6 fields (MSG_ID, KIND), photos, emoji, a 4-field line, a bad ID */
        wc = fopen("/tmp/fake_wa_read_15550000002@s.whatsapp.net", "w");
        if (wc) {
            fprintf(wc, "%ld\t0\t\tOld style line\n", wnow - 900);
            fprintf(wc, "%ld\t0\tOmar 😀\t[photo] The new board 🔥\t3EB0AAAA0001\tphoto\n", wnow - 800);
            fprintf(wc, "%ld\t1\t\t[photo]\t3EB0AAAA0002\tphoto\n", wnow - 700);
            fprintf(wc, "%ld\t0\tOmar 😀\t[voice]\t3EB0AAAA0003\tvoice\n", wnow - 600);
            fprintf(wc, "%ld\t0\tOmar 😀\tMissing emoji 🧿 here\tbad;id\ttext\n", wnow - 500);
            fprintf(wc, "%ld\t0\tOmar 😀\tشكرا 😍\t3EB0AAAA0005\ttext\n", wnow - 400);
            fprintf(wc, "%ld\t0\tOmar 😀\tHello 😀 how are you?\twamid.AAA111\ttext\n", wnow - 300);
            fclose(wc);
        }
        /* a handful of real emoji pictures (made by tools/make_emoji.py) on the fake card,
         * and the photo the fake PC sends */
        snprintf(cmd, sizeof cmd, "rm -rf " NZ_SDCARD "/nzeus/emoji " NZ_SDCARD "/nzeus/whatsapp/photos && mkdir -p " NZ_SDCARD "/nzeus/emoji && "
                 "cp %s/../emoji/* " NZ_SDCARD "/nzeus/emoji/ && cp %s/../wa_photo.jpg /tmp/fake_wa_photo.jpg", pics, pics);
        check(system(cmd) == 0, "WhatsApp: the test emoji pictures and photo are ready (tests/emoji, tests/wa_photo.jpg)");
        unlink("/tmp/fake_wa_photo_fail");
        unlink("/tmp/fake_wa_send_fail");
        unlink("/tmp/fake_wa_voice_fail");
        make_tone_at("/tmp/fake_wa_voice.wav", 11025, 1);  /* the voice message the fake PC sends (as the real one: 11025 Hz mono) */
        if (system("rm -rf " NZ_SDCARD "/nzeus/whatsapp/voice") != 0) {}
        wc = fopen("/tmp/fake_wa_read_15550000001@s.whatsapp.net", "w");
        if (wc) {
            fprintf(wc, "%ld\t0\t\tالسلام عليكم يا صديقي\n", wnow - 600);
            fprintf(wc, "%ld\t1\t\tHi! On my way\n", wnow - 300);
            fprintf(wc, "%ld\t0\t\tLine one\\nLine two with a \\\\ backslash\n", wnow - 60);
            fclose(wc);
        }
        wc = fopen("/tmp/fake_wa_read_120363000000000001@g.us", "w");
        if (wc) {
            fprintf(wc, "%ld\t0\tSara\tHello everyone\n", wnow - 900);
            fprintf(wc, "%ld\t0\tعلي\tمرحبا بالجميع\n", wnow - 800);
            fprintf(wc, "%ld\t1\t\tThanks!\n", wnow - 700);
            fclose(wc);
        }

        /* the small helpers */
        char u[] = "a\\\\nb\\nc", b64[64];
        wa_unescape(u);
        check(!strcmp(u, "a\\nb\nc"), "WhatsApp: \\n in a text becomes a line break, \\\\ one backslash");
        wa_base64("Hi", b64, sizeof b64);
        int b64_ok = !strcmp(b64, "SGk=");
        wa_base64("مرحبا", b64, sizeof b64);
        check(b64_ok && !strcmp(b64, "2YXYsdit2KjYpw=="), "WhatsApp: base64 is right (English and Arabic text)");

        /* no Claude PIN yet: refused */
        if (system("rm -rf " WA_KEYDIR " " WA_LOG " /tmp/fake_wa_mode /tmp/nzeus-wa-key && cp " NZ_SDCARD "/nzeus/settings.ini /tmp/fake_wa_settings && "
                   "grep -v -e '^claude_pin=' -e '^claude_salt=' /tmp/fake_wa_settings > " NZ_SDCARD "/nzeus/settings.ini") != 0) {}
        settings_reload();
        ui_open_app(APP_WHATSAPP); frames(3);
        check(apps_focused() && apps_focused()->id == APP_WHATSAPP && app_whatsapp_state() == 0 && !nzui_active() &&
              !strcmp(ui_toast_text(), "Choose a PIN in the Claude app first") && access(WA_KEYDIR "/wa_key.enc", F_OK) != 0,
              "WhatsApp: without a Claude PIN it refuses (\"Choose a PIN in the Claude app first\")");
        save("core_wa_1_no_pin");
        tap(14); frames(3);
        check(!apps_focused(), "WhatsApp: B closes it");
        if (system("cp /tmp/fake_wa_settings " NZ_SDCARD "/nzeus/settings.ini") != 0) {}
        settings_reload();

        /* first use: the PIN, then the key is made and locked */
        ui_open_app(APP_WHATSAPP); frames(3);
        check(nzui_active() && app_whatsapp_state() == 1, "WhatsApp: first use asks the PIN");
        type_line("9999"); frames(3);
        check(!strcmp(ui_toast_text(), "Wrong PIN") && app_whatsapp_state() == 1 && access(WA_KEYDIR "/wa_key.enc", F_OK) != 0 &&
              lines_starting(WA_LOG, "--make-key") == 0, "WhatsApp: a wrong PIN makes no key");
        tap(13); frames(2);                                /* A: type the PIN again */
        type_line("1357");
        WA_WAIT(app_whatsapp_state() == 3);
        struct stat kst;
        int key_in_ram = stat("/tmp/nzeus-wa-key", &kst) == 0 && (kst.st_mode & 0777) == 0600 &&
                         file_starts_with("/tmp/nzeus-wa-key", "FAKE-WA-PRIVATE-KEY");
        check(app_whatsapp_state() == 3 && lines_starting(WA_LOG, "--make-key") == 1 && file_starts_with(WA_KEYDIR "/wa_key.enc", "NZK1") &&
              access(WA_KEYDIR "/wa_key", F_OK) != 0 && file_starts_with(WA_KEYDIR "/wa_key.pub", "ssh-ed25519"),
              "WhatsApp: first use makes the key, keeps it only LOCKED (wa_key.enc), deletes the plain wa_key");
        check(vault_decrypt_file(WA_KEYDIR "/wa_key.enc", "/tmp/wa_try", "1357") == 0 && file_starts_with("/tmp/wa_try", "FAKE-WA-PRIVATE-KEY") &&
              vault_decrypt_file(WA_KEYDIR "/wa_key.enc", "/tmp/wa_try2", "2468") == -1, "WhatsApp: wa_key.enc opens only with the PIN");
        unlink("/tmp/wa_try");
        check(key_in_ram, "WhatsApp: the right PIN unlocks the key into RAM (/tmp/nzeus-wa-key, mode 600)");
        frames(3);
        save("core_wa_2_setup");

        /* the chat list */
        tap(13);                                           /* A: continue */
        WA_WAIT(app_whatsapp_chat_count() == 3 && !app_whatsapp_busy());
        check(app_whatsapp_state() == 4 && app_whatsapp_chat_count() == 3 && !strcmp(app_whatsapp_chat_name(0), "أحمد") &&
              app_whatsapp_chat_unread(0) == 2 && !strcmp(app_whatsapp_chat_name(1), "NZEUS Lab group") &&
              !strcmp(app_whatsapp_chat_last(1), "Sara: see you at 5\nbring the board"),
              "WhatsApp: the chat list is read (names, unread counts, \\n inside a message)");
        check(file_contains("/tmp/fake_wa_env", "KEY=/tmp/nzeus-wa-key HOST=192.168.1.10 PORT=22 USER=user"),
              "WhatsApp: it asks the Claude app's PC brain, with the unlocked key");
        NzList *wl = app_whatsapp_list();
        if (nzlist_count(wl) == 3) {
            const char *t0 = lv_label_get_text(nzlist_title_obj(wl, 0)), *s0 = lv_label_get_text(nzlist_sub_obj(wl, 0));
            const char *s1 = lv_label_get_text(nzlist_sub_obj(wl, 1)), *r0 = lv_label_get_text(nzlist_right_obj(wl, 0));
            const char *r2 = lv_label_get_text(nzlist_right_obj(wl, 2));
            check(arabic_count(t0, 0) == 0 && arabic_count(t0, 1) == 4 && arabic_count(s0, 0) == 0 && arabic_count(s0, 1) > 5,
                  "WhatsApp: an Arabic name and message are shown joined (right to left) in the list");
            check(!strchr(s1, '\n') && !strncmp(s1, "Sara: see you at 5 bring", 24), "WhatsApp: the message preview is one line");
            check(strstr(r0, "2 new") && r0[2] == ':' && !strstr(r2, "new") && strchr(r2, ' '),
                  "WhatsApp: on the right: the time (HH:MM today, a date when older) and the unread count");
        } else check(0, "WhatsApp: the list shows 3 chats");
        frames(3);
        save("core_wa_3_chats");

        /* a chat: mine on the right, theirs on the left, newest at the bottom */
        tap(13);                                           /* A: open the Arabic chat */
        WA_WAIT(app_whatsapp_msg_count() == 3 && !app_whatsapp_busy());
        check(app_whatsapp_page() == 1 && app_whatsapp_msg_count() == 3 && app_whatsapp_msg_mine(0) == 0 && app_whatsapp_msg_mine(1) == 1 &&
              app_whatsapp_msg_mine(2) == 0 && !strcmp(app_whatsapp_msg_text(2), "Line one\nLine two with a \\ backslash") &&
              lines_starting(WA_LOG, "read 15550000001@s.whatsapp.net 60") == 1,
              "WhatsApp: A opens a chat: its messages are read (mine / theirs, \\n and \\\\ unescaped)");
        frames(3);
        lv_obj_t *wsc = app_whatsapp_scroller();
        if (lv_obj_get_child_cnt(wsc) == 3) {
            lv_obj_t *b0 = lv_obj_get_child(lv_obj_get_child(wsc, 0), 0), *b1 = lv_obj_get_child(lv_obj_get_child(wsc, 1), 0);
            lv_area_t as, a0, a1;
            lv_obj_get_coords(wsc, &as); lv_obj_get_coords(b0, &a0); lv_obj_get_coords(b1, &a1);
            check(a1.x2 > as.x2 - 30 && a1.x1 > as.x1 + 100 && a0.x1 < as.x1 + 30 && a0.x2 < as.x2 - 100 &&
                  is_accent_at(a1.x1 + 3, (a1.y1 + a1.y2) / 2) && !is_accent_at(a0.x1 + 3, (a0.y1 + a0.y2) / 2),
                  "WhatsApp: my message is on the right in the accent colour, theirs on the left");
            lv_obj_t *ar = lv_obj_get_child(b0, 0);
            const char *at = lv_label_get_text(ar);
            check(arabic_count(at, 0) == 0 && arabic_count(at, 1) > 10 && lv_obj_get_style_text_align(ar, 0) == LV_TEXT_ALIGN_RIGHT,
                  "WhatsApp: an Arabic message is joined and right to left");
        } else check(0, "WhatsApp: 3 message bubbles");
        check(lv_obj_get_scroll_bottom(wsc) <= 4, "WhatsApp: the chat is scrolled to the newest message (bottom)");
        save("core_wa_4_chat");

        /* reply: the exact base64 of what was typed, then the chat is read again */
        int reads = lines_starting(WA_LOG, "read ");
        tap(13); frames(2);                                /* A: reply */
        check(nzui_active(), "WhatsApp: A in a chat opens the keyboard to write a reply");
        type_line("On my way!");
        WA_WAIT(lines_starting(WA_LOG, "read ") > reads && !app_whatsapp_busy());
        check(lines_starting(WA_LOG, "send 15550000001@s.whatsapp.net T24gbXkgd2F5IQ==") == 1,
              "WhatsApp: the reply is sent as the exact base64 of the typed text");
        check(lines_starting(WA_LOG, "read ") > reads, "WhatsApp: after sending, the chat is reloaded");

        /* problems: an "error: ..." answer, and no answer at all */
        if (system("echo error > /tmp/fake_wa_mode") != 0) {}
        tap(14);                                           /* B: back to the chats (they reload) */
        WA_WAIT(!strcmp(ui_toast_text(), "WhatsApp is not linked on the PC"));
        check(app_whatsapp_page() == 0 && !strcmp(ui_toast_text(), "WhatsApp is not linked on the PC") &&
              !strcmp(app_whatsapp_status(), "WhatsApp is not linked on the PC"),
              "WhatsApp: an \"error: ...\" answer becomes a message (toast + status line)");
        frames(3);
        save("core_wa_5_error");
        if (system("echo silent > /tmp/fake_wa_mode") != 0) {}
        tap(12);                                           /* X: refresh */
        WA_WAIT(!strcmp(ui_toast_text(), "Could not reach the PC (is it on? is the bridge running?)"));
        check(!strcmp(ui_toast_text(), "Could not reach the PC (is it on? is the bridge running?)") && app_whatsapp_chat_count() == 3,
              "WhatsApp: no answer at all = \"Could not reach the PC\" (the list stays)");
        unlink("/tmp/fake_wa_mode");
        tap(12);
        WA_WAIT(!app_whatsapp_busy() && strstr(app_whatsapp_status(), "updated"));

        /* a group: the author's name above their messages */
        tap(3); tap(13);                                   /* DOWN, A: the group */
        WA_WAIT(app_whatsapp_msg_count() == 3 && !app_whatsapp_busy() && !strcmp(app_whatsapp_msg_text(0), "Hello everyone"));
        check(!strcmp(app_whatsapp_msg_author(0), "Sara") && !strcmp(app_whatsapp_msg_author(1), "علي") && app_whatsapp_msg_mine(2) == 1 &&
              lv_obj_get_child_cnt(lv_obj_get_child(lv_obj_get_child(wsc, 0), 0)) == 3 &&
              lv_obj_get_child_cnt(lv_obj_get_child(lv_obj_get_child(wsc, 2), 0)) == 2,
              "WhatsApp: in a group, the author's name is shown above their messages");
        frames(3);
        save("core_wa_6_group");

        /* an open chat reloads by itself (every 30 s on the console; 1 s here), not more often */
        wa_reload_chat_ms = 1000;
        int r0 = lines_starting(WA_LOG, "read ");
        frames(100);
        int r1 = lines_starting(WA_LOG, "read ");
        check(r1 - r0 >= 1 && r1 - r0 <= 2, "WhatsApp: an open chat reloads by itself, not more often than set");
        wa_reload_chat_ms = 30000;

        /* ==== WhatsApp emoji + photos (added 2026-10-06) ==== */
        /* (only when the app is ready: a broken key must give a quick FAIL, not minutes of waiting) */
        if (app_whatsapp_state() == 4 && app_whatsapp_chat_count() == 3) {
            /* the 6 fields: MSG_ID and KIND are kept; a 4-field line and a bad ID are tolerated */
            tap(14);                                           /* B: the chats */
            WA_WAIT(!app_whatsapp_busy() && app_whatsapp_page() == 0 && strstr(app_whatsapp_status(), "updated"));
            tap(3); tap(13);                                   /* DOWN, A: Omar's chat */
            WA_WAIT(app_whatsapp_msg_count() == 7 && !app_whatsapp_busy());
            frames(3);
            check(app_whatsapp_msg_count() == 7 && app_whatsapp_msg_kind(0) == 0 && !strcmp(app_whatsapp_msg_id(0), "") &&
                  app_whatsapp_msg_kind(1) == 1 && !strcmp(app_whatsapp_msg_id(1), "3EB0AAAA0001") &&
                  app_whatsapp_msg_kind(2) == 1 && app_whatsapp_msg_mine(2) == 1 && app_whatsapp_msg_kind(3) == 3 &&
                  !strcmp(app_whatsapp_msg_id(4), "") && !strcmp(app_whatsapp_msg_id(6), "wamid.AAA111") &&
                  !strcmp(app_whatsapp_msg_text(1), "[photo] The new board 🔥") && !strcmp(app_whatsapp_msg_author(6), "Omar 😀"),
                  "WhatsApp: a chat's 6 fields are read (MSG_ID, KIND; a 4-field line = text, a bad ID is dropped)");
            save("core_wa_7_emoji_chat");

            /* emoji on screen: in the bubbles, the author line, the chat title and the list */
            int omar_ok = app_whatsapp_msg_count() == 7 && lv_obj_get_child_cnt(wsc) == 7;   /* (else: FAILs below, no crash) */
            check(omar_ok, "WhatsApp: Omar's chat shows its 7 messages");
            lv_obj_t *jb = kid(kid(wsc, 6), 0);   /* the last bubble: author, text, time */
            if (jb) lv_obj_scroll_to_view_recursive(jb, LV_ANIM_OFF);
            frames(3);
            lv_obj_t *jt = kid(jb, 1), *ja = kid(jb, 0);
            check(jt && emoji_drawn(jt, 0x1F600) && !strcmp(ltext(jt), "Hello 😀 how are you?") &&
                  lv_obj_get_style_text_align(jt, 0) != LV_TEXT_ALIGN_RIGHT,
                  "Emoji: an emoji in a bubble is drawn as its picture; the text stays left to right, unchanged");
            check(emoji_drawn(ja, 0x1F600), "Emoji: an emoji in a group author's name is drawn");
            check(emoji_drawn(app_whatsapp_chat_title(), 0x1F600), "Emoji: an emoji in the chat's title is drawn");
            lv_obj_t *ab = kid(kid(kid(wsc, 5), 0), 1);   /* "شكرا 😍" */
            if (ab) lv_obj_scroll_to_view_recursive(ab, LV_ANIM_OFF);
            frames(3);
            const char *abt = ltext(ab);
            lv_font_glyph_dsc_t ag;
            uint32_t ak = 0, first_ar = 0, ac;
            while ((ac = _lv_txt_encoded_next(abt, &ak)) != 0) if (ac >= 0xFE70 && ac <= 0xFEFC && !first_ar) first_ar = ac;
            check(ab && emoji_drawn(ab, 0x1F60D) && arabic_count(abt, 0) == 0 && arabic_count(abt, 1) >= 3 && first_ar &&
                  lv_font_get_glyph_dsc(lv_obj_get_style_text_font(ab, 0), &ag, first_ar, 0) && !ag.is_placeholder &&
                  lv_obj_get_style_text_align(ab, 0) == LV_TEXT_ALIGN_RIGHT,
                  "Emoji: Arabic + emoji: the Arabic is joined and right to left, the emoji is drawn too");
            lv_obj_t *mb = kid(kid(kid(wsc, 4), 0), 1);   /* "Missing emoji 🧿 here" */
            lv_font_glyph_dsc_t mg;
            int miss_ok = !emoji_has(0x1F9FF) && emoji_has(0x1F600) && !emoji_in_text("no emoji here, only a 🧿") &&
                          emoji_font() && !emoji_font()->get_glyph_dsc(emoji_font(), &mg, 0x1F9FF, 0) &&
                          !strcmp(ltext(mb), "Missing emoji 🧿 here");
            if (mb) lv_obj_scroll_to_view_recursive(mb, LV_ANIM_OFF);
            frames(3);
            check(miss_ok && app_whatsapp_page() == 1, "Emoji: an emoji without a picture is no glyph, and nothing crashes");
            lv_obj_t *ob = kid(kid(kid(wsc, 0), 0), 0);   /* "Old style line" */
            check(ob && lv_obj_get_style_text_font(ob, 0) == theme()->font_body, "Emoji: a text without emoji keeps the theme's own font");
            lv_obj_t *pb = kid(kid(wsc, 1), 0);   /* their photo: author, "Photo", caption, time */
            if (pb) lv_obj_scroll_to_view_recursive(pb, LV_ANIM_OFF);
            frames(3);
            lv_area_t pa = { 0, 0, 0, 0 };
            if (kid(pb, 1)) lv_obj_get_coords(kid(pb, 1), &pa);
            int accent_px = 0;
            for (int x = pa.x1; x < pa.x2; x++) for (int y = pa.y1; y < pa.y2; y++) accent_px += is_accent_at(x, y);
            check((pb ? (int)lv_obj_get_child_cnt(pb) : 0) == 4 && !strncmp(ltext(kid(pb, 1)), LV_SYMBOL_IMAGE "  Photo", 5) &&
                  accent_px > 10 && emoji_drawn(kid(pb, 2), 0x1F525) &&
                  !strcmp(ltext(kid(pb, 2)), "The new board 🔥"),
                  "WhatsApp: a photo message is shown as a picture sign + \"Photo\" (accent colour), then its caption");
            save("core_wa_7b_photo_bubbles");
            /* the chat list: an emoji in a name (+ Arabic and emoji in a last message) */
            tap(14);
            WA_WAIT(!app_whatsapp_busy() && app_whatsapp_page() == 0 && strstr(app_whatsapp_status(), "updated"));
            frames(3);
            check((wl ? nzlist_count(wl) : 0) == 3 && emoji_drawn(nzlist_title_obj(wl, 2), 0x1F600) && emoji_drawn(nzlist_sub_obj(wl, 2), 0x1F600) &&
                  emoji_drawn(nzlist_sub_obj(wl, 0), 0x1F60D) && arabic_count(ltext(nzlist_sub_obj(wl, 0)), 1) > 5,
                  "Emoji: the chat list shows emoji in names and last messages (Arabic + emoji too)");
            save("core_wa_7c_emoji_list");
            /* opening and closing a chat with emoji again and again: no memory lost (the fonts are made once) */
            long eh0 = 0;
            for (int round = 0; round < 6; round++) {
                if (round == 1) eh0 = heap_used();
                tap(13);
                WA_WAIT(app_whatsapp_msg_count() == 7 && !app_whatsapp_busy());
                frames(2);
                tap(14);
                WA_WAIT(!app_whatsapp_busy() && app_whatsapp_page() == 0 && strstr(app_whatsapp_status(), "updated"));
                frames(2);
            }
            long egrew = heap_used() - eh0;
            char emsg[120];
            snprintf(emsg, sizeof emsg, "Emoji: a chat with emoji opened 5 times loses no memory (%+ld bytes)", egrew);
            check(egrew < 4096, emsg);
            tap(13);
            WA_WAIT(app_whatsapp_msg_count() == 7 && !app_whatsapp_busy());
            frames(2);

            /* Y: the photos menu */
            tap(15); frames(2);
            lv_obj_t *mi = popup_label(lv_layer_top(), "Photos in this chat (", 6);
            check(nzui_active() && mi && !strcmp(ltext(mi), "Photos in this chat (2)") && popup_label(lv_layer_top(), "Send a picture", 6),
                  "WhatsApp: Y in a chat opens a menu: \"Photos in this chat (2)\", \"Send a picture\"");
            save("core_wa_8_photos_menu");
            tap(13); frames(3);                                /* A: the photos */
            NzList *sl = app_whatsapp_sub_list();
            check(app_whatsapp_page() == 2 && app_whatsapp_photo_count() == 2 && (sl ? nzlist_count(sl) : 0) == 2 &&
                  !strcmp(ltext(nzlist_title_obj(sl, 0)), "Photo") && !strcmp(ltext(nzlist_sub_obj(sl, 0)), "You") &&
                  !strcmp(ltext(nzlist_title_obj(sl, 1)), "The new board 🔥") &&
                  emoji_drawn(nzlist_title_obj(sl, 1), 0x1F525) && emoji_drawn(nzlist_sub_obj(sl, 1), 0x1F600),
                  "WhatsApp: \"Photos in this chat\" lists the photos, newest first (caption, who sent it)");
            save("core_wa_9_photo_list");

            /* A: the photo is downloaded (in the background), then shown full screen */
            int photos0 = lines_starting(WA_LOG, "photo ");
            tap(13); frames(2);
            check(app_whatsapp_page() == 3 && app_whatsapp_busy() && !strcmp(app_whatsapp_status(), "Loading photo..."),
                  "WhatsApp: A on a photo: \"Loading photo...\" while it downloads in the background");
            save("core_wa_10_loading");
            WA_WAIT(!app_whatsapp_busy() && app_whatsapp_photo_w() > 0);
            frames(3);
            check(lines_starting(WA_LOG, "photo 15550000002@s.whatsapp.net 3EB0AAAA0002 " NZ_SDCARD "/nzeus/whatsapp/photos/3EB0AAAA0002_") == 1 &&
                  lines_starting(WA_LOG, "photo ") == photos0 + 1,
                  "WhatsApp: the photo is asked with the exact command (chat, MSG_ID, a file in nzeus/whatsapp/photos)");
            /* fitted: the 480 x 360 photo is made bigger until it touches the page's sides or top and bottom */
            lv_obj_t *vimg = app_whatsapp_view_img(), *vpg = vimg ? lv_obj_get_parent(vimg) : lv_scr_act();
            int vz = vimg ? lv_img_get_zoom(vimg) : 256, vdw = 480 * vz / 256, vdh = 360 * vz / 256;
            int vcw = lv_obj_get_content_width(vpg), vch = lv_obj_get_content_height(vpg);
            lv_area_t vpa;
            lv_obj_get_coords(vpg, &vpa);
            int vcx = (vpa.x1 + vpa.x2) / 2, vcy = vpa.y1 + vch / 2;
            int vex = vcx - vdw / 2 + 6;
            if (vex < 0) vex = 0;
            if (vcy < 0 || vcy >= NZ_H) vcy = NZ_H / 2;
            uint16_t mid = frame[vcy * NZ_W + vcx], edge = frame[vcy * NZ_W + vex];   /* (near the photo's left side) */
            check(app_whatsapp_page() == 3 && app_whatsapp_photo_w() == 480 && count_files(NZ_SDCARD "/nzeus/whatsapp/photos") == 1 &&
                  (vdw >= vcw - 2 || vdh >= vch - 2) && vdw <= vcw + 1 && vdh <= vch + 1 &&
                  ((mid >> 11) & 31) + ((mid >> 5) & 63) / 2 + (mid & 31) > 10 && ((edge >> 11) & 31) + ((edge >> 5) & 63) / 2 + (edge & 31) > 10,
                  "WhatsApp: the photo is decoded and shown fitted to the screen (aspect kept)");
            save("core_wa_11_viewer");
            tap(14); frames(3);                                /* B: back to the list; the pixels are freed */
            check(app_whatsapp_page() == 2 && app_whatsapp_photo_w() == 0, "WhatsApp: B closes the photo (its memory is freed)");
            long ph0 = heap_used();
            tap(13);                                           /* A again: already saved, no download */
            WA_WAIT(app_whatsapp_photo_w() > 0);
            int shown_again = app_whatsapp_photo_w() == 480 && !app_whatsapp_busy();
            frames(3);
            tap(14); frames(3);
            long ph_grew = heap_used() - ph0;
            check(shown_again && lines_starting(WA_LOG, "photo ") == photos0 + 1,
                  "WhatsApp: a photo already saved is shown at once, not downloaded again");
            char pmsg[120];
            snprintf(pmsg, sizeof pmsg, "WhatsApp: opening + closing a photo loses no memory (%+ld bytes)", ph_grew);
            check(app_whatsapp_photo_w() == 0 && ph_grew < 2048, pmsg);

            /* a download that fails: the PC's words as a toast; the folder keeps only about 60 photos */
            if (system("touch /tmp/fake_wa_photo_fail") != 0) {}
            tap(3); tap(13);                                   /* DOWN, A: the other photo */
            WA_WAIT(!app_whatsapp_busy() && !strcmp(ui_toast_text(), "the photo is gone"));
            check(!strcmp(ui_toast_text(), "the photo is gone") && app_whatsapp_page() == 3 && app_whatsapp_photo_w() == 0 &&
                  !strcmp(app_whatsapp_status(), "the photo is gone") && count_files(NZ_SDCARD "/nzeus/whatsapp/photos") == 1,
                  "WhatsApp: a photo that can't be downloaded: the error as a toast, nothing saved");
            tap(14); frames(2);
            unlink("/tmp/fake_wa_photo_fail");
            if (system("cd " NZ_SDCARD "/nzeus/whatsapp/photos && for i in $(seq 10 74); do echo x > old_$i.jpg; done && "
                       "touch -d '2020-01-01' old_*.jpg") != 0) {}
            tap(13);
            WA_WAIT(!app_whatsapp_busy() && app_whatsapp_photo_w() > 0);
            check(count_files(NZ_SDCARD "/nzeus/whatsapp/photos") == 60 && count_files(NZ_SDCARD "/nzeus/whatsapp/photos/3EB0AAAA000*") == 2,
                  "WhatsApp: the photos folder keeps the newest 60 files (the oldest are deleted)");
            if (system("rm -f " NZ_SDCARD "/nzeus/whatsapp/photos/old_*") != 0) {}
            tap(14); frames(2);
            tap(14); frames(2);                                /* B: back to the chat */
            check(app_whatsapp_page() == 1, "WhatsApp: B, B goes back from a photo to the chat");

            /* "Send a picture": the pictures on the card, newest first, none over 4 MB */
            if (system("mkdir -p " NZ_SDCARD "/DCIM/Camera && cp /tmp/fake_wa_photo.jpg " NZ_SDCARD "/DCIM/Camera/cat.jpg && "
                       "touch -d '1 hour' " NZ_SDCARD "/DCIM/Camera/cat.jpg && truncate -s 4194305 " NZ_SDCARD "/DCIM/huge.jpg && "
                       "echo hi > " NZ_SDCARD "/DCIM/notes.txt") != 0) {}
            tap(15); frames(2); tap(3); tap(13); frames(3);    /* Y, DOWN, A: Send a picture */
            int huge_listed = 0;
            for (int i = 0; i < (sl ? nzlist_count(sl) : 0); i++) if (!strcmp(ltext(nzlist_title_obj(sl, i)), "huge.jpg")) huge_listed = 1;
            check(app_whatsapp_page() == 4 && app_whatsapp_pick_count() >= 4 && (sl ? nzlist_count(sl) : 0) == app_whatsapp_pick_count() &&
                  !strcmp(ltext(nzlist_title_obj(sl, 0)), "cat.jpg") && !huge_listed &&
                  strstr(app_whatsapp_status(), "1 skipped (over 4 MB)"),
                  "WhatsApp: \"Send a picture\" lists the card's JPG/PNG files, newest first; files over 4 MB are skipped (and it says so)");
            save("core_wa_12_pick");
            tap(13); frames(2);                                /* A: the question */
            lv_obj_t *q = popup_label(lv_layer_top(), "Send ", 6);
            check(nzui_active() && q && !strcmp(ltext(q), "Send cat.jpg to this chat?"),
                  "WhatsApp: picking a picture asks \"Send cat.jpg to ...?\" first");
            save("core_wa_13_confirm");
            tap(13);                                           /* A: yes */
            WA_WAIT(!app_whatsapp_busy() && !strcmp(ui_toast_text(), "Photo sent"));
            check(lines_starting(WA_LOG, "sendphoto 15550000002@s.whatsapp.net " NZ_SDCARD "/DCIM/Camera/cat.jpg\n") == 1 &&
                  !strcmp(ui_toast_text(), "Photo sent") && app_whatsapp_page() == 1 && app_whatsapp_pick_count() == 0,
                  "WhatsApp: yes sends it with the exact command, then \"Photo sent\" (back in the chat)");
            if (system("touch /tmp/fake_wa_send_fail") != 0) {}
            tap(15); frames(2); tap(3); tap(13); frames(3); tap(13); frames(2); tap(13);
            WA_WAIT(!app_whatsapp_busy() && !strcmp(ui_toast_text(), "please wait 5 s between messages"));
            check(!strcmp(ui_toast_text(), "please wait 5 s between messages"), "WhatsApp: a send that the PC refuses: its words as a toast");
            unlink("/tmp/fake_wa_send_fail");
            tap(15); frames(2); tap(3); tap(13); frames(3);    /* the list again, then B */
            tap(13); frames(2); tap(14); frames(2);            /* (A, then B = no: nothing is sent) */
            int sends = lines_starting(WA_LOG, "sendphoto ");
            tap(14); frames(2);
            check(app_whatsapp_page() == 1 && app_whatsapp_pick_count() == 0 && sends == 2 && !nzui_active(),
                  "WhatsApp: \"no\" sends nothing; B leaves the picture list (its memory is freed)");

            /* ==== voice messages (added 2026-10-06): "Voice messages (N)" in the Y menu; one is
             * downloaded once into nzeus/whatsapp/voice and played by the music engine ==== */
            {
                #define WA_VDIR NZ_SDCARD "/nzeus/whatsapp/voice"
                /* the bubble: a sound sign + "Voice message" + its time (its text "[voice]" isn't shown) */
                lv_obj_t *vb = kid(kid(wsc, 3), 0);           /* Omar's voice message: author, "Voice message", time */
                if (vb) lv_obj_scroll_to_view_recursive(vb, LV_ANIM_OFF);
                frames(3);
                check(vb && lv_obj_get_child_cnt(vb) == 3 && !strcmp(ltext(kid(vb, 1)), LV_SYMBOL_AUDIO "  Voice message") &&
                      ltext(kid(vb, 2))[0] && !strstr(ltext(kid(vb, 1)), "[voice]"),
                      "WhatsApp: a voice message is shown as a sound sign + \"Voice message\" and its time");
                save("core_wa_14_voice_bubble");
                /* Y: the menu's 3rd line */
                tap(15); frames(2);
                check(nzui_active() && popup_label(lv_layer_top(), "Voice messages (1)", 6) != NULL,
                      "WhatsApp: Y in a chat: the menu has \"Voice messages (1)\"");
                save("core_wa_15_voice_menu");
                tap(3); tap(3); tap(13); frames(3);           /* DOWN x2, A */
                NzList *vl = app_whatsapp_sub_list();
                check(app_whatsapp_page() == 5 && app_whatsapp_voice_count() == 1 && (vl ? nzlist_count(vl) : 0) == 1 &&
                      !strcmp(ltext(nzlist_title_obj(vl, 0)), "Voice message") && !strcmp(ltext(nzlist_sub_obj(vl, 0)), "Omar 😀") &&
                      !strcmp(app_whatsapp_status(), "1 voice message"),
                      "WhatsApp: \"Voice messages\" lists them (who sent it, when)");
                save("core_wa_16_voice_list");
                /* A: downloaded in the background with the exact command, then played */
                int v0 = lines_starting(WA_LOG, "voice ");
                tap(13); frames(2);
                check(app_whatsapp_busy() && !strcmp(app_whatsapp_status(), "Loading voice message..."),
                      "WhatsApp: A on a voice message: \"Loading voice message...\" while it downloads");
                WA_WAIT(!app_whatsapp_busy() && app_whatsapp_voice_playing());
                sound_peak = 0;
                frames(10);
                check(lines_starting(WA_LOG, "voice 15550000002@s.whatsapp.net 3EB0AAAA0003 " WA_VDIR "/3EB0AAAA0003_") == 1 &&
                      lines_starting(WA_LOG, "voice ") == v0 + 1 && count_files(WA_VDIR) == 1,
                      "WhatsApp: the voice message is asked with the exact command (chat, MSG_ID, a file in nzeus/whatsapp/voice)");
                check(app_whatsapp_voice_playing() && audio_playing() && sound_peak > 3000 && !strncmp(app_whatsapp_status(), "Playing...", 10),
                      "WhatsApp: ...then it plays (the music engine, from the file): \"Playing...\"");
                save("core_wa_17_voice_playing");
                /* A pauses, A goes on, B stops (and stays on the list) */
                tap(13); frames(3);
                sound_peak = 0;
                frames(5);
                check(app_whatsapp_voice_playing() && !audio_playing() && sound_peak == 0 && !strncmp(app_whatsapp_status(), "Paused", 6),
                      "WhatsApp: A on the one playing pauses it (silence, \"Paused\")");
                save("core_wa_18_voice_paused");
                tap(13); frames(3);
                check(app_whatsapp_voice_playing() && audio_playing(), "WhatsApp: ...and A again goes on");
                tap(14); frames(3);
                check(!audio_loaded() && !app_whatsapp_voice_playing() && app_whatsapp_page() == 5 &&
                      !strcmp(app_whatsapp_status(), "1 voice message"),
                      "WhatsApp: B stops it (the music engine lets go of the file) and stays on the list");
                /* A again: already saved, no second download; it plays to its end and stops by itself */
                tap(13);
                WA_WAIT(app_whatsapp_voice_playing());
                check(lines_starting(WA_LOG, "voice ") == v0 + 1 && audio_playing(),
                      "WhatsApp: a voice message already saved plays at once (no second download)");
                for (int i = 0; i < 300 && audio_loaded(); i++) frames(1);   /* (it is 1 second long) */
                frames(3);
                check(!audio_loaded() && !app_whatsapp_voice_playing() && !strcmp(app_whatsapp_status(), "1 voice message") &&
                      app_whatsapp_page() == 5, "WhatsApp: at its end it stops by itself (back to \"1 voice message\")");
                /* a download that fails: the PC's words as a toast, nothing saved, nothing plays */
                if (system("rm -f " WA_VDIR "/* && touch /tmp/fake_wa_voice_fail") != 0) {}
                tap(13);
                WA_WAIT(!app_whatsapp_busy() && !strcmp(ui_toast_text(), "the voice message is gone"));
                check(!strcmp(ui_toast_text(), "the voice message is gone") && !strcmp(app_whatsapp_status(), "the voice message is gone") &&
                      count_files(WA_VDIR) == 0 && !audio_loaded(),
                      "WhatsApp: a voice message that can't be downloaded: the error as a toast, nothing saved or played");
                unlink("/tmp/fake_wa_voice_fail");
                /* the voice folder keeps only the newest 40 files */
                if (system("cd " WA_VDIR " && for i in $(seq 10 54); do echo x > old_$i.wav; done && touch -d '2020-01-01' old_*.wav") != 0) {}
                tap(13);
                WA_WAIT(!app_whatsapp_busy() && app_whatsapp_voice_playing());
                check(count_files(WA_VDIR) == 40 && count_files(WA_VDIR "/3EB0AAAA0003_*") == 1,
                      "WhatsApp: the voice folder keeps the newest 40 files (the oldest are deleted)");
                if (system("rm -f " WA_VDIR "/old_*") != 0) {}
                /* B back to the chat while it plays: it stops */
                tap(14); frames(2); tap(14); frames(2);
                check(!audio_loaded() && app_whatsapp_page() == 1, "WhatsApp: B, B goes back to the chat (the voice message stopped)");
                /* the app is closed while one plays: it stops (then the app again, with the PIN, back to Omar's chat) */
                tap(15); frames(2); tap(3); tap(3); tap(13); frames(3); tap(13);
                WA_WAIT(app_whatsapp_voice_playing());
                int was_playing = app_whatsapp_voice_playing();
                apps_close(apps_get(APP_WHATSAPP)); frames(3);
                check(was_playing && !audio_loaded() && !apps_focused(), "WhatsApp: closing the app while a voice message plays stops it");
                ui_open_app(APP_WHATSAPP); frames(3);
                type_line("1357");
                WA_WAIT(app_whatsapp_chat_count() == 3 && !app_whatsapp_busy());
                tap(3); tap(3); tap(13);                      /* DOWN x2, A: Omar's chat */
                WA_WAIT(app_whatsapp_msg_count() == 7 && !app_whatsapp_busy() && app_whatsapp_page() == 1);
                frames(3);
                check(app_whatsapp_page() == 1 && app_whatsapp_msg_count() == 7, "WhatsApp: (open again, back in Omar's chat)");
                #undef WA_VDIR
            }

            /* the same screens in two more themes (screenshots to look at) */
            static const int shot_themes[] = { TH_DAYLIGHT, TH_AMBER_CRT };
            for (int s = 0; s < 2; s++) {
                char nm[48];
                theme_set(shot_themes[s]);
                tap(14);                                       /* B, A: the chat again (in the new theme's fonts) */
                WA_WAIT(!app_whatsapp_busy() && app_whatsapp_page() == 0 && strstr(app_whatsapp_status(), "updated"));
                frames(3);
                snprintf(nm, sizeof nm, "core_wa_t%d_emoji_list", shot_themes[s]); save(nm);
                tap(13);
                WA_WAIT(app_whatsapp_msg_count() == 7 && !app_whatsapp_busy() && app_whatsapp_page() == 1);
                frames(3);
                snprintf(nm, sizeof nm, "core_wa_t%d_emoji_chat", shot_themes[s]); save(nm);
                tap(15); frames(2);
                snprintf(nm, sizeof nm, "core_wa_t%d_photos_menu", shot_themes[s]); save(nm);
                tap(13); frames(3);
                snprintf(nm, sizeof nm, "core_wa_t%d_photo_list", shot_themes[s]); save(nm);
                tap(13);
                WA_WAIT(app_whatsapp_photo_w() > 0);
                frames(3);
                snprintf(nm, sizeof nm, "core_wa_t%d_viewer", shot_themes[s]); save(nm);
                tap(14); frames(2); tap(14); frames(2);
                tap(15); frames(2); tap(3); tap(13); frames(3);
                snprintf(nm, sizeof nm, "core_wa_t%d_pick", shot_themes[s]); save(nm);
                tap(13); frames(2);
                snprintf(nm, sizeof nm, "core_wa_t%d_confirm", shot_themes[s]); save(nm);
                tap(14); frames(2); tap(14); frames(2);        /* no, back to the chat */
                tap(15); frames(2); tap(3); tap(3); tap(13); frames(3);   /* Y, DOWN x2, A: the voice messages */
                snprintf(nm, sizeof nm, "core_wa_t%d_voice_list", shot_themes[s]); save(nm);
                tap(13);                                       /* A: play */
                WA_WAIT(app_whatsapp_voice_playing());
                frames(3);
                snprintf(nm, sizeof nm, "core_wa_t%d_voice_playing", shot_themes[s]); save(nm);
                tap(14); frames(2); tap(14); frames(2);        /* B: stop, B: back to the chat */
            }
            theme_set(TH_ORIGINAL); frames(2);

            /* the REAL nzeus-wa.sh (linux-port/deck-modern, as installed on the console): photo + sendphoto, with a FAKE dbclient
             * (it logs its arguments and stdin and answers from a file: nothing leaves the PC) */
            {
                #define RW "/tmp/fake_realwa"
                #define REAL_WA "\"" NZ_REPO "/linux-port/deck-modern/nzeus-wa.sh\""
                #define RUN_WA "NZ_DBMULTI=" RW "/db NZ_USE_NETD=0 NZ_WA=" RW "/wa NZ_WA_KEY=" RW "/key " \
                               "NZ_WA_HOST=192.168.1.10 NZ_WA_USER=user sh " REAL_WA " "
                FILE *db = NULL;
                if (system("rm -rf " RW " && mkdir -p " RW "/wa && echo key > " RW "/key") == 0) db = fopen(RW "/db", "w");
                if (db) {
                    fputs("#!/bin/sh\nlast=''; for a in \"$@\"; do last=\"$a\"; done\necho \"$last\" >> " RW "/args\n"
                          "cat > " RW "/stdin\ncat " RW "/answer\nexit $(cat " RW "/code 2>/dev/null || echo 0)\n", db);
                    fclose(db);
                }
                int set_ok = system("chmod 755 " RW "/db && { echo ok; base64 -w 76 /tmp/fake_wa_photo.jpg; } > " RW "/answer") == 0;
                int rc1 = system(RUN_WA "photo 15550000002@s.whatsapp.net 3EB0AAAA0002 " RW "/out.jpg > " RW "/res");
                check(set_ok && rc1 == 0 && file_starts_with(RW "/res", "ok") && same_file(RW "/out.jpg", "/tmp/fake_wa_photo.jpg") &&
                      access(RW "/out.jpg.tmp", F_OK) != 0 && lines_starting(RW "/args", "photo 15550000002@s.whatsapp.net 3EB0AAAA0002\n") == 1,
                      "nzeus-wa.sh photo: asks the PC (chat + MSG_ID only), decodes the base64 into OUTFILE, prints ok");
                int rc2 = system("echo 'error: the photo is gone' > " RW "/answer && echo 1 > " RW "/code && "
                                 RUN_WA "photo 15550000002@s.whatsapp.net 3EB0AAAA0002 " RW "/out2.jpg > " RW "/res");
                check(rc2 != 0 && file_starts_with(RW "/res", "error: the photo is gone") && access(RW "/out2.jpg", F_OK) != 0 &&
                      access(RW "/out2.jpg.tmp", F_OK) != 0, "nzeus-wa.sh photo: the PC's error is printed, no file is left");
                int rc3 = system("printf 'ok\\n!!!not base64!!!\\n' > " RW "/answer && echo 0 > " RW "/code && "
                                 RUN_WA "photo 15550000002@s.whatsapp.net 3EB0AAAA0002 " RW "/out3.jpg > " RW "/res");
                check(rc3 != 0 && file_starts_with(RW "/res", "error: the photo could not be decoded") && access(RW "/out3.jpg", F_OK) != 0 &&
                      access(RW "/out3.jpg.tmp", F_OK) != 0, "nzeus-wa.sh photo: a broken answer saves nothing");
                /* the answer cut off after whole base64 lines: it decodes fine, but the JPEG has no end mark */
                int rc3b = system("{ echo ok; base64 -w 76 /tmp/fake_wa_photo.jpg | head -n 3; } > " RW "/answer && "
                                  RUN_WA "photo 15550000002@s.whatsapp.net 3EB0AAAA0002 " RW "/out3b.jpg > " RW "/res");
                check(rc3b != 0 && file_starts_with(RW "/res", "error: the photo came only partly") && access(RW "/out3b.jpg", F_OK) != 0 &&
                      access(RW "/out3b.jpg.tmp", F_OK) != 0, "nzeus-wa.sh photo: a cut-off photo (no JPEG end mark) saves nothing");
                int calls = lines_starting(RW "/args", "");
                int rc4 = system(RUN_WA "photo 15550000002@s.whatsapp.net 'a;b' " RW "/out4.jpg > " RW "/res");
                int rc5 = system(RUN_WA "photo 15550000002@s.whatsapp.net abc " RW "/out4.jpg >> " RW "/res");
                check(rc4 != 0 && rc5 != 0 && file_starts_with(RW "/res", "error: bad message ID") && lines_starting(RW "/args", "") == calls,
                      "nzeus-wa.sh photo: a bad MSG_ID (wrong letters / too short) is refused before the PC is asked");
                /* voice: the same way, with a WAV ("RIFF" + size + "WAVE", and as long as that size says) */
                int vset = system("{ echo ok; base64 -w 76 /tmp/fake_wa_voice.wav; } > " RW "/answer && echo 0 > " RW "/code") == 0;
                int rv1 = system(RUN_WA "voice 15550000002@s.whatsapp.net 3EB0AAAA0003 " RW "/v.wav > " RW "/res");
                check(vset && rv1 == 0 && file_starts_with(RW "/res", "ok") && same_file(RW "/v.wav", "/tmp/fake_wa_voice.wav") &&
                      access(RW "/v.wav.tmp", F_OK) != 0 && lines_starting(RW "/args", "voice 15550000002@s.whatsapp.net 3EB0AAAA0003\n") == 1,
                      "nzeus-wa.sh voice: asks the PC (chat + MSG_ID only), decodes the base64 WAV into OUTFILE, prints ok");
                int rv2 = system("{ echo ok; base64 -w 76 /tmp/fake_wa_voice.wav | head -n 3; } > " RW "/answer && "
                                 RUN_WA "voice 15550000002@s.whatsapp.net 3EB0AAAA0003 " RW "/v2.wav > " RW "/res");
                check(rv2 != 0 && file_starts_with(RW "/res", "error: the voice message came only partly") && access(RW "/v2.wav", F_OK) != 0 &&
                      access(RW "/v2.wav.tmp", F_OK) != 0, "nzeus-wa.sh voice: a cut-off WAV (shorter than its header says) saves nothing");
                int rv3 = system("{ echo ok; base64 -w 76 /tmp/fake_wa_photo.jpg; } > " RW "/answer && "
                                 RUN_WA "voice 15550000002@s.whatsapp.net 3EB0AAAA0003 " RW "/v3.wav > " RW "/res");
                check(rv3 != 0 && file_starts_with(RW "/res", "error: the voice message is not a WAV file") && access(RW "/v3.wav", F_OK) != 0 &&
                      access(RW "/v3.wav.tmp", F_OK) != 0, "nzeus-wa.sh voice: an answer that isn't a WAV (no RIFF/WAVE) saves nothing");
                int rv4 = system("echo 'error: no voice' > " RW "/answer && echo 1 > " RW "/code && "
                                 RUN_WA "voice 15550000002@s.whatsapp.net 3EB0AAAA0003 " RW "/v4.wav > " RW "/res");
                check(rv4 != 0 && file_starts_with(RW "/res", "error: no voice") && access(RW "/v4.wav", F_OK) != 0 &&
                      access(RW "/v4.wav.tmp", F_OK) != 0, "nzeus-wa.sh voice: the PC's error is printed, no file is left");
                calls = lines_starting(RW "/args", "");
                int rv5 = system("echo 0 > " RW "/code && " RUN_WA "voice 15550000002@s.whatsapp.net '3EB0;AAAA' " RW "/v5.wav > " RW "/res");
                int rv6 = system(RUN_WA "voice 15550000002@s.whatsapp.net 3EB0AAAA0003 relative.wav >> " RW "/res");
                check(rv5 != 0 && rv6 != 0 && file_starts_with(RW "/res", "error: bad message ID") &&
                      file_contains(RW "/res", "error: OUTFILE must be a full path") && lines_starting(RW "/args", "") == calls,
                      "nzeus-wa.sh voice: a bad MSG_ID or a relative OUTFILE is refused before the PC is asked");
                int rc6 = system("echo ok > " RW "/answer && cp /tmp/fake_wa_photo.jpg '" RW "/my pic.jpg' && "
                                 RUN_WA "sendphoto 15550000002@s.whatsapp.net '" RW "/my pic.jpg' > " RW "/res");
                check(rc6 == 0 && file_starts_with(RW "/res", "ok") && same_file(RW "/stdin", "/tmp/fake_wa_photo.jpg") &&
                      lines_starting(RW "/args", "sendphoto 15550000002@s.whatsapp.net\n") == 1,
                      "nzeus-wa.sh sendphoto: the picture's bytes go on stdin (the file name stays on the console)");
                int rc7 = system(RUN_WA "sendphoto 15550000002@s.whatsapp.net '" RW "/my pic.jpg' SGk= > " RW "/res");
                check(rc7 == 0 && lines_starting(RW "/args", "sendphoto 15550000002@s.whatsapp.net SGk=\n") == 1,
                      "nzeus-wa.sh sendphoto: a base64 caption is passed on");
                calls = lines_starting(RW "/args", "");
                int rc8 = system("truncate -s 4194305 " RW "/big.jpg && " RUN_WA "sendphoto 15550000002@s.whatsapp.net " RW "/big.jpg > " RW "/res");
                int rc9 = system(RUN_WA "sendphoto 15550000002@s.whatsapp.net " RW "/none.jpg >> " RW "/res");
                check(rc8 != 0 && rc9 != 0 && file_starts_with(RW "/res", "error: the picture is too big (4 MB at most)") &&
                      file_contains(RW "/res", "error: no such picture") && lines_starting(RW "/args", "") == calls,
                      "nzeus-wa.sh sendphoto: a missing picture or one over 4 MB is refused before the PC is asked");
                int rc10 = system(RUN_WA "read 15550000002@s.whatsapp.net 60 < /dev/null > " RW "/res");
                int rc11 = system(RUN_WA "read 'a b' 60 < /dev/null > " RW "/res");
                check(rc10 == 0 && lines_starting(RW "/args", "read 15550000002@s.whatsapp.net 60\n") == 1 && rc11 != 0 &&
                      file_starts_with(RW "/res", "error: bad request"),
                      "nzeus-wa.sh: the other commands work as before (and keep their strict check)");
                if (system("rm -rf " RW) != 0) {}
                #undef RW
                #undef REAL_WA
                #undef RUN_WA
            }

            /* back to the group (the theme pictures below start from it) */
            tap(14);
            WA_WAIT(!app_whatsapp_busy() && app_whatsapp_page() == 0 && strstr(app_whatsapp_status(), "updated"));
            tap(2); tap(13);                                   /* UP, A: the group */
            WA_WAIT(app_whatsapp_msg_count() == 3 && !app_whatsapp_busy() && !strcmp(app_whatsapp_msg_text(0), "Hello everyone"));
        } else check(0, "WhatsApp: emoji + photos: the app is ready (unlocked, 3 chats)");
        /* ==== end of WhatsApp emoji + photos ==== */

        /* every theme: the list and the Arabic chat */
        for (int th = 0; th < TH_COUNT; th++) {
            char nm[40];
            theme_set(th);
            tap(14);                                       /* B: the chats (reloaded) */
            WA_WAIT(!app_whatsapp_busy() && app_whatsapp_page() == 0 && strstr(app_whatsapp_status(), "updated"));
            frames(3);
            snprintf(nm, sizeof nm, "core_wa_t%d_chats", th);
            save(nm);
            tap(2); tap(13);                               /* UP, A: the Arabic chat */
            WA_WAIT(app_whatsapp_page() == 1 && app_whatsapp_msg_count() == 3 && !app_whatsapp_busy() &&
                    !strcmp(app_whatsapp_msg_text(1), "Hi! On my way"));
            frames(3);
            snprintf(nm, sizeof nm, "core_wa_t%d_chat", th);
            save(nm);
        }
        theme_set(TH_ORIGINAL); frames(2);

        /* closing deletes the unlocked key; the next time the PIN is asked again */
        tap(14); frames(2); tap(14); frames(3);
        check(!apps_focused() && access("/tmp/nzeus-wa-key", F_OK) != 0, "WhatsApp: closing the app deletes the unlocked key (RAM)");
        wc = fopen("/tmp/fake_wa_chats", "w");             /* 35 chats: only 30 are kept */
        for (int i = 0; wc && i < 35; i++) fprintf(wc, "1555%07d@s.whatsapp.net\t0\t%ld\tFriend %d\thello\n", i, wnow - i * 60, i);
        if (wc) fclose(wc);
        ui_open_app(APP_WHATSAPP); frames(3);
        check(nzui_active() && app_whatsapp_state() == 1, "WhatsApp: the PIN is asked every time it opens");
        type_line("2468"); frames(3);
        check(!strcmp(ui_toast_text(), "Wrong PIN") && app_whatsapp_state() == 1 && access("/tmp/nzeus-wa-key", F_OK) != 0,
              "WhatsApp: a wrong PIN unlocks nothing");
        tap(13); type_line("1357");
        WA_WAIT(app_whatsapp_chat_count() > 0 && !app_whatsapp_busy());
        check(app_whatsapp_state() == 4 && file_starts_with("/tmp/nzeus-wa-key", "FAKE-WA-PRIVATE-KEY"), "WhatsApp: the right PIN unlocks the key again");
        check(app_whatsapp_chat_count() == 30, "WhatsApp: at most 30 chats are kept in RAM");
        tap(14); frames(3);
        check(!apps_focused() && access("/tmp/nzeus-wa-key", F_OK) != 0, "WhatsApp: and closing deletes it again");

        /* the Claude app's PIN changes: the WhatsApp key follows */
        check(whatsapp_relock("1357", "2468") == 0 && vault_decrypt_file(WA_KEYDIR "/wa_key.enc", "/tmp/wa_try", "2468") == 0 &&
              vault_decrypt_file(WA_KEYDIR "/wa_key.enc", "/tmp/wa_try", "1357") == -1 && whatsapp_relock("2468", "1357") == 0,
              "WhatsApp: when the Claude PIN changes, the WhatsApp key is re-locked with the new one");
        unlink("/tmp/wa_try");

        /* how it is opened: the Tools list (Y on the home screen), and "nzctl open whatsapp" */
        tap(15); frames(2);                                /* Y: Tools */
        for (int i = 0; i < 5; i++) tap(3);                /* DOWN x5: WhatsApp (after Images) */
        tap(13); frames(3);
        check(apps_focused() && apps_focused()->id == APP_WHATSAPP && nzui_active(), "WhatsApp: it is in the Tools list (home screen, Y)");
        tap(14); tap(14); frames(3);                       /* B: no PIN, B: close */
        check(!apps_focused(), "WhatsApp: B, B closes it from the PIN question");
        FILE *rq = fopen("/tmp/nzeus-cmd/req.5101", "w");
        if (rq) { fputs("open whatsapp\n", rq); fclose(rq); }
        frames(10);
        char wans[64] = "";
        file_lines("/tmp/nzeus-cmd/done.5101", 0, wans, sizeof wans);
        check(!strcmp(wans, "ok") && apps_focused() && apps_focused()->id == APP_WHATSAPP, "WhatsApp: nzctl open whatsapp opens it");
        tap(14); tap(14); frames(3);
        #undef WA_KEYDIR
        #undef WA_LOG
        #undef WA_WAIT
    }
    /* ==== end of WhatsApp ==== */

    /* ==== Web = NetSurf (2026-10-13) ====
     * The Web app hands the screen to nzeus/bin/nzeus-browser (NetSurf) through
     * hw_fullscreen_run. Here a FAKE frontend function runs a FAKE launcher
     * script that writes its argument to a log and exits with the code the test
     * wants (0 = quit, 10 = Y: type an address, 3 = could not start). */
    {
        #define BR_LOG "/tmp/fake_browser_log"
        #define BR_RC  "/tmp/fake_browser_rc"
        #define FAVS   NZ_SDCARD "/nzeus/web/favourites.txt"
        char last[600];
        /* no NetSurf: a clear message, nothing else */
        unlink(FAVS); unlink(BR_LOG);
        if (system("mkdir -p " NZ_SDCARD "/nzeus/bin") != 0) {}
        FILE *bs = fopen(NZ_SDCARD "/nzeus/bin/nzeus-browser", "w");
        if (bs) {
            fputs("#!/bin/sh\n"
                  "echo \"$*\" >> " BR_LOG "\n"
                  "rc=$(head -n 1 " BR_RC " 2>/dev/null); sed -i 1d " BR_RC " 2>/dev/null\n"
                  "exit ${rc:-0}\n", bs);
            fclose(bs);
        }
        chmod(NZ_SDCARD "/nzeus/bin/nzeus-browser", 0755);
        apps_open(APP_WEB); frames(15);
        check(apps_focused() && apps_focused()->id == APP_WEB &&
              !strcmp(app_web_message(), "The browser is not installed: apt install netsurf-fb") && access(BR_LOG, F_OK) != 0,
              "Web: without NetSurf it says how to install it (apt install netsurf-fb), and starts nothing");
        check(!strcmp(app_web_result(), "error: The browser is not installed: apt install netsurf-fb"),
              "Web: nzctl web would answer the same message as an error");
        save("core_web_not_installed");
        check(file_contains(FAVS, "# NZEUS Deck Web favourites") && file_contains(FAVS, "DuckDuckGo (search)|https://lite.duckduckgo.com/lite/"),
              "Web favourites: nzeus/web/favourites.txt is made on first use (Name|https://address lines)");
        tap(14); frames(3);
        check(apps_get(APP_WEB)->root == NULL, "Web: B goes home from the message");
        FILE *fv = fopen(FAVS, "w");
        if (fv) { fputs("# mine\nMy page|https://example.org/\n", fv); fclose(fv); }
        apps_open(APP_WEB); frames(3); tap(14); frames(3);
        check(file_contains(FAVS, "My page|https://example.org/") && !file_contains(FAVS, "DuckDuckGo"),
              "Web favourites: the owner's own file is never overwritten");

        /* NetSurf installed, but the launcher missing, or a frontend that can't hand over the screen */
        FILE *ns = fopen("/tmp/fake_netsurf", "w");
        if (ns) { fputs("#!/bin/sh\nexit 0\n", ns); fclose(ns); }
        chmod("/tmp/fake_netsurf", 0755);
        web_netsurf_path = "/tmp/fake_netsurf";
        hw_fullscreen_run = fake_fullscreen;
        web_launcher_path = "/tmp/no_such_launcher";
        apps_open(APP_WEB); frames(15);
        check(!strcmp(app_web_message(), "The browser is not installed: apt install netsurf-fb") && fullscreen_runs == 0,
              "Web: NetSurf there but no nzeus-browser: the same message, nothing started");
        tap(14); frames(3);
        web_launcher_path = NZ_SDCARD "/nzeus/bin/nzeus-browser";
        hw_fullscreen_run = NULL;
        apps_open(APP_WEB); frames(15);
        check(!strcmp(app_web_message(), "The browser is not installed: apt install netsurf-fb") && access(BR_LOG, F_OK) != 0,
              "Web: a frontend that can't hand over the screen (the self-test): the same message");
        tap(14); frames(3);

        /* NetSurf: "Opening the browser..." first, then nzeus-browser home; quit = back home */
        hw_fullscreen_run = fake_fullscreen;
        fullscreen_runs = 0;
        apps_open(APP_WEB); frames(1);
        check(!strcmp(app_web_message(), "Opening the browser...") && app_web_loading() && fullscreen_runs == 0,
              "Web: opening it first shows 'Opening the browser...' (NetSurf not started yet)");
        save("core_web_opening");
        for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
        frames(3);
        check(fullscreen_runs == 1 && file_lines(BR_LOG, 0, last, sizeof last) == 1 && !strcmp(last, "home"),
              "Web: then the screen goes to nzeus-browser, at NetSurf's start page (home)");
        check(apps_get(APP_WEB)->root == NULL && apps_focused() == NULL, "Web: quitting NetSurf (SELECT+START, exit 0) goes back home");

        /* Y in NetSurf (exit 10): the address box, then NetSurf at what was typed */
        unlink(BR_LOG);
        FILE *rcf = fopen(BR_RC, "w");
        if (rcf) { fputs("10\n0\n", rcf); fclose(rcf); }
        apps_open(APP_WEB);
        for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
        frames(3);
        check(nzui_active() && apps_focused() && apps_focused()->id == APP_WEB && !app_web_message()[0],
              "Web: Y in NetSurf (exit 10) asks for a web address or words to search for");
        save("core_web_address");
        type_line("example.com"); frames(3);
        int n = file_lines(BR_LOG, 1, last, sizeof last);
        check(n == 2 && !strcmp(last, "https://example.com/") && apps_get(APP_WEB)->root == NULL,
              "Web: an address typed there (example.com) opens NetSurf at https://example.com/");
        /* words = a search; a path = a file; http:// stays as it is; B = the same page again */
        static const struct { const char *typed, *want, *what; } addr[] = {
            { "led lamp", "https://lite.duckduckgo.com/lite/?q=led%20lamp", "Web: words typed there are searched for (DuckDuckGo Lite)" },
            { "/tmp/fake_sdcard/docs/a.txt", "file:///tmp/fake_sdcard/docs/a.txt", "Web: a path typed there opens that file in NetSurf" },
            { "http://68k.news/x", "http://68k.news/x", "Web: an http:// address is kept as it is" },
        };
        for (size_t k = 0; k < sizeof addr / sizeof addr[0]; k++) {
            unlink(BR_LOG);
            rcf = fopen(BR_RC, "w");
            if (rcf) { fputs("10\n0\n", rcf); fclose(rcf); }
            apps_open(APP_WEB);
            for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
            frames(2);
            type_line(addr[k].typed); frames(3);
            check(file_lines(BR_LOG, 1, last, sizeof last) == 2 && !strcmp(last, addr[k].want), addr[k].what);
        }
        unlink(BR_LOG);
        rcf = fopen(BR_RC, "w");
        if (rcf) { fputs("10\n10\n0\n", rcf); fclose(rcf); }
        app_web_open("https://example.com/page");
        for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
        frames(2);
        tap(14); frames(3);                              /* B in the address box */
        char second[600] = "";
        file_lines(BR_LOG, 1, second, sizeof second);
        check(!strcmp(second, "https://example.com/page") && nzui_active(),
              "Web: B in the address box goes back to NetSurf at the same page");
        tap(14); frames(3);                              /* (B again: the same page, it quits this time) */
        check(file_lines(BR_LOG, 2, last, sizeof last) == 3 && !strcmp(last, "https://example.com/page") &&
              apps_get(APP_WEB)->root == NULL, "Web: ... and every time again (exit 0 then goes home)");

        /* NetSurf could not start (exit 3): it says so; B goes home */
        rcf = fopen(BR_RC, "w");
        if (rcf) { fputs("3\n", rcf); fclose(rcf); }
        apps_open(APP_WEB);
        for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
        frames(3);
        check(apps_focused() && apps_focused()->id == APP_WEB && strstr(app_web_message(), "The browser could not start") &&
              !strncmp(app_web_result(), "error: The browser could not start", 34),
              "Web: NetSurf failing to start says so (and where its log is)");
        save("core_web_failed");
        tap(14); frames(3);

        /* app_web_open (nzctl web URL): NetSurf at that address; a path becomes file:// */
        unlink(BR_LOG);
        app_web_open("https://example.com/");
        for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
        frames(3);
        check(file_lines(BR_LOG, 0, last, sizeof last) == 1 && !strcmp(last, "https://example.com/") &&
              !strcmp(app_web_result(), "ok: opened in the browser: https://example.com/") && !strcmp(app_web_current(), "https://example.com/"),
              "Web: app_web_open(URL) opens NetSurf there and remembers how it went");
        unlink(BR_LOG);
        app_web_open("/tmp/fake_sdcard/docs/a.txt");
        for (int i = 0; i < 40 && app_web_loading(); i++) frames(1);
        frames(3);
        check(file_lines(BR_LOG, 0, last, sizeof last) == 1 && !strcmp(last, "file:///tmp/fake_sdcard/docs/a.txt"),
              "Web: app_web_open(a path) opens the file in NetSurf (file://)");
        /* the remote-control channel: "web URL" answers once NetSurf has ended */
        {
            char ans[300] = "";
            if (system("mkdir -p /tmp/nzeus-cmd && rm -f /tmp/nzeus-cmd/*") != 0) {}
            FILE *q = fopen("/tmp/nzeus-cmd/req.6001", "w");
            if (q) { fputs("web https://example.com/news\n", q); fclose(q); }
            for (int i = 0; i < 200 && access("/tmp/nzeus-cmd/done.6001", F_OK) != 0; i++) frames(1);
            file_lines("/tmp/nzeus-cmd/done.6001", 0, ans, sizeof ans);
            check(!strcmp(ans, "ok: opened in the browser: https://example.com/news"),
                  "nzctl web URL: NetSurf opens it, the answer comes when it has ended");
            web_netsurf_path = "/tmp/no_such_netsurf";
            q = fopen("/tmp/nzeus-cmd/req.6002", "w");
            if (q) { fputs("web https://example.com/\n", q); fclose(q); }
            for (int i = 0; i < 200 && access("/tmp/nzeus-cmd/done.6002", F_OK) != 0; i++) frames(1);
            file_lines("/tmp/nzeus-cmd/done.6002", 0, ans, sizeof ans);
            check(!strcmp(ans, "error: The browser is not installed: apt install netsurf-fb"),
                  "nzctl web: without NetSurf it answers with the reason");
            tap(14); frames(3);
        }
        hw_fullscreen_run = NULL;                        /* (the other tests: no NetSurf) */
        web_netsurf_path = "/tmp/no_such_netsurf";
        unlink(BR_RC);
        #undef BR_LOG
        #undef BR_RC
        #undef FAVS
    }
    {   /* a long message pop-up wraps instead of running off the screen (seen on the console) */
        ui_toast("nzeus-fetch: TLS handshake with lite.duckduckgo.com failed: SSL - The connection indicated an EOF");
        frames(2);
        int fits = 1;
        for (uint32_t ci = 0; ci < lv_obj_get_child_cnt(lv_layer_top()); ci++) {
            lv_obj_t *o = lv_obj_get_child(lv_layer_top(), ci);
            if (lv_obj_check_type(o, &lv_label_class) && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) &&
                (lv_obj_get_x(o) < 0 || lv_obj_get_x(o) + lv_obj_get_width(o) > NZ_W)) fits = 0;
        }
        check(fits && strstr(ui_toast_text(), "duckduckgo"), "a long pop-up message wraps and stays on the screen");
    }
    /* ==== end of Web ==== */

    /* ---- Arabic on a label (artext.c: WhatsApp messages, list rows): a long paragraph is cut
     *      into lines first (each fits), then each line is turned right to left ---- */
    {
        void artext_set(lv_obj_t *l, const char *text, int width);
        lv_obj_t *p1 = lv_label_create(lv_scr_act());
        lv_obj_set_width(p1, 400);
        artext_set(p1, "هذه صفحة تجريبية لمتصفح NZEUS على جهاز R36S. الحروف العربية تظهر متصلة ومن اليمين إلى اليسار، "
                       "والأرقام مثل 2026 و 12.5 تبقى كما هي.", 400);
        lv_obj_update_layout(p1);
        const char *t1 = lv_label_get_text(p1);
        const lv_font_t *f1 = lv_obj_get_style_text_font(p1, 0);
        int lines = 0, too_wide = 0, cw = lv_obj_get_content_width(p1);
        const char *nl1 = strchr(t1, '\n');
        for (const char *ln = t1; ln;) {
            const char *e = strchr(ln, '\n');
            uint32_t n = e ? (uint32_t)(e - ln) : (uint32_t)strlen(ln);
            if (lv_txt_get_width(ln, n, f1, 0, LV_TEXT_FLAG_NONE) > cw) too_wide++;
            lines++;
            ln = e ? e + 1 : NULL;
        }
        const char *first_word = "ﻩﺬﻫ";                 /* "هذه", drawn right to left */
        int top_ok = nl1 && nl1 - t1 >= (long)strlen(first_word) && !strncmp(nl1 - strlen(first_word), first_word, strlen(first_word));
        check(lines >= 2 && !too_wide && top_ok && lv_obj_get_style_text_align(p1, 0) == LV_TEXT_ALIGN_RIGHT,
              "Arabic: a long paragraph wraps into right-to-left lines that each fit (first words at the top right)");
        lv_obj_del(p1);
    }

    /* ---- the Terminal's Commands list only names what the SD Debian really has ---- */
    {
        int mnt = 0, stock = 0, missing = 0, examples = 0;
        char why[300] = "";
        static const char *const gone[] = { "nzinfo", "nzshot", "nzpress", "nzlog", "wifi-diag.sh", "nzeus-usbserial.sh",
                                             "nzeus-netctl ping", "nzeus-netctl resolve", "micropython" };
        const TermCmd *c;
        for (int i = 0; (c = term_cmds_entry(i)); i++) {
            if (!c->cmd) continue;
            const char *parts[3] = { c->cmd, c->what, c->example ? c->example : "" };
            for (int k = 0; k < 3; k++) {
                if (strstr(parts[k], "/mnt/sdcard")) mnt++;
                if (strstr(parts[k], "/usr/local/lib/nzeus/python-examples")) examples++;
            }
            for (size_t g = 0; g < sizeof gone / sizeof gone[0]; g++) if (strstr(c->cmd, gone[g])) stock++;
            /* a NZEUS helper (nzeus-..., wifi-....sh): it must be one of linux-port/deck-modern's */
            char first[64];
            sscanf(c->cmd, "%63s", first);
            if (!strncmp(first, "nzeus-", 6) || !strncmp(first, "wifi-", 5)) {
                char path[300];
                snprintf(path, sizeof path, "%s/linux-port/deck-modern/%s", NZ_REPO, first);
                if (access(path, F_OK) != 0) { missing++; snprintf(why, sizeof why, "%s", first); }
            }
        }
        check(!mnt, "Terminal help: no /mnt/sdcard paths (that is the factory system's card)");
        check(!stock, "Terminal help: no factory-system tools (nzinfo, nzshot, nzpress, nzlog, wifi-diag.sh, netctl ping/resolve...)");
        if (missing) printf("INFO  Terminal help names a helper that linux-port/deck-modern lacks: %s\n", why);
        check(!missing, "Terminal help: every NZEUS helper it names is one the SD Debian has (linux-port/deck-modern)");
        check(examples >= 3, "Terminal help: the Python examples are where the Python app finds them (/usr/local/lib/nzeus/python-examples)");
    }
    /* ---- Wi-Fi: no scan while wlan0 is switched off ---- */
    if (system("echo 0x1002 > /tmp/fake_sys/sys/class/net/wlan0/flags") != 0) {}
    check(wifi_scan_start() == -1, "Wi-Fi: a scan is refused while wlan0 is switched off");
    if (system("echo 0x1003 > /tmp/fake_sys/sys/class/net/wlan0/flags") != 0) {}

    /* ---- the built-in Settings app ---- */
    tap(3); tap(3); tap(5); tap(5);                  /* Files -> DOWN, DOWN, RIGHT, RIGHT: Settings */
    tap(13); frames(3);
    check(apps_focused() && apps_focused()->id == APP_SETTINGS && !shutdown_asked,
          "A on Settings opens the NZEUS Settings app (no TreeFrogUI)");
    save("core_7_settings");
    uint16_t px_before = frame[300 * NZ_W + 600];    /* a background pixel on the right */
    tap(5);                                          /* RIGHT: into the rows (Theme) */
    tap(5);                                          /* RIGHT: the next theme */
    frames(3);
    check(theme_index() == TH_GREEN_CRT, "RIGHT on Theme switches to the next theme (Green CRT)");
    check(file_contains(NZ_SDCARD "/nzeus/settings.ini", "theme=green_crt"), "the theme is saved to settings.ini");
    check(frame[300 * NZ_W + 600] != px_before, "every screen is redrawn in the new colours at once");
    check(frame[100 * NZ_W + 400] != frame[101 * NZ_W + 400], "Green CRT draws scanlines (every 2nd row darker)");
    save("core_8_settings_green_crt");
    settings_reload(); theme_init(); frames(2);      /* like a restart: read the file again */
    check(theme_index() == TH_GREEN_CRT, "the theme is kept after a restart");
    tap(7); frames(3);                               /* LEFT: back to Original */
    check(theme_index() == TH_ORIGINAL && frame[300 * NZ_W + 600] == px_before, "LEFT goes back to Original");
    check(frame[100 * NZ_W + 400] == frame[101 * NZ_W + 400], "Original has no scanlines");
    tap(14);                                         /* B: back to the sections */
    tap(3); tap(3); tap(3);                          /* DOWN x3: Date & time */
    tap(13); frames(2);                              /* A: its rows (Now) */
    {
        time_t t0 = hw_now();
        tap(3); tap(5); frames(2);                   /* Day: RIGHT = the next day */
        time_t t1 = hw_now();
        struct tm d1 = *localtime(&t1);
        long long saved1 = clock_file();
        tap(3); tap(5); frames(2);                   /* Month: RIGHT */
        time_t t2 = hw_now();
        struct tm d2 = *localtime(&t2);
        tap(3); tap(5); frames(2);                   /* Year: RIGHT */
        time_t t3 = hw_now();
        struct tm d3 = *localtime(&t3);
        save("core_settings_date");
        theme_set(TH_AMBER_CRT); frames(3); save("core_settings_date_crt"); theme_set(TH_ORIGINAL); frames(2);
        check(app_settings_section() == 3 && t1 - t0 >= 86400 && t1 - t0 <= 86405, "Settings > Date & time: RIGHT on Day = the next day");
        check(saved1 >= t1 - 2 && saved1 <= t1, "Settings > Date & time: a new date is saved to clock.txt at once");
        check(d2.tm_mon == (d1.tm_mon + 1) % 12 && d2.tm_mday == d1.tm_mday && d3.tm_year == d2.tm_year + 1 && d3.tm_mon == d2.tm_mon,
              "Settings > Date & time: Month and Year rows change the date");
        tap(7); frames(2);                           /* Year: LEFT = back again */
        time_t t4 = hw_now();
        struct tm d4 = *localtime(&t4);
        check(d4.tm_year == d2.tm_year && clock_file() >= t4 - 2, "Settings > Date & time: LEFT goes back");
        check(time(NULL) >= real_start && time(NULL) - real_start < 900, "Settings > Date & time: the PC's own clock was not changed");
    }
    tap(14);                                         /* B: back to the sections */
    tap(3); tap(3); tap(3);                          /* DOWN x3: Startup */
    check(app_settings_section() == 6 && !app_settings_in_panel(), "the D-pad moves through the sections");
    tap(13); frames(2);                              /* A: rows ("Start in"; no game menu on this system) */
    check(!popup_label(lv_scr_act(), "Open the game menu", 12) && strstr(app_settings_row_value(0), "Home screen"),
          "Settings > Startup: only 'Start in' (Home screen) - no game menu on this system");
    tap(5); frames(2);                               /* RIGHT: Desktop */
    check(file_contains(NZ_SDCARD "/nzeus/settings.ini", "start=desktop") && strstr(app_settings_row_value(0), "Desktop"),
          "Settings > Startup: 'Start in' Desktop is saved");
    tap(7); frames(2);                               /* LEFT: back to the home screen */
    check(file_contains(NZ_SDCARD "/nzeus/settings.ini", "start=home"), "Settings > Startup: ...and back to the home screen");
    /* Power > Restart NZEUS Deck: the core asks the frontend to quit and to start it again */
    tap(14); for (int i = 0; i < 6; i++) tap(3);     /* B: sections, DOWN x6: Power */
    tap(13); tap(13); frames(2);                     /* A: rows, A: Restart NZEUS Deck */
    int restart_asked = nzui_active() && !shutdown_asked;
    tap(13); frames(3);                              /* A: yes */
    extern int nzeus_restart_wanted;
    check(restart_asked && shutdown_asked && nzeus_restart_wanted,
          "Settings > Power > Restart NZEUS Deck: asks, then the core quits and asks nzeus-modern to start it again");

    /* ---- desktop mode ----
     * (the core stops reading buttons once it has asked to quit, so the
     *  rest talks to the UI layer directly, the same calls the core makes) */
    ui_key(NZ_KEY_SELECT); frames(2);
    check(ui_mode() == UI_DESKTOP, "SELECT switches to desktop mode");
    ui_key(NZ_KEY_START); frames(2);
    check(desktop_start_menu_is_open(), "START opens the NZEUS menu");
    ui_key(NZ_KEY_B); frames(2);
    check(!desktop_start_menu_is_open(), "B closes it");
    ui_key(NZ_KEY_SELECT); frames(2);
    check(ui_mode() == UI_HANDHELD, "SELECT switches back to handheld mode");

    ui_mouse(320, 240, 0, 0, 1); frames(3);       /* someone uses a USB mouse */
    check(ui_mode() == UI_DESKTOP, "using a mouse switches to desktop mode by itself");

    int x0, y0, x1, y1;                           /* the D-pad as a pointer */
    ui_pointer_pos(&x0, &y0);
    for (int i = 0; i < 20; i++) { ui_buttons(1u << NZ_KEY_RIGHT); frames(1); }
    ui_buttons(0); frames(1);
    ui_pointer_pos(&x1, &y1);
    check(x1 > x0 + 20 && y1 == y0, "holding RIGHT moves the pointer right");

    /* D-pad to the System icon (2nd column, 4th row: x 128, y 290), then A to click it */
    ui_mouse(320, 240, 0, 0, 0);                  /* (no real mouse movement) */
    for (int guard = 0; guard < 600; guard++) {  /* (bounded: a broken pointer must not hang the test) */
        ui_pointer_pos(&x1, &y1);
        uint32_t held = 0;
        if (x1 > 138) held |= 1u << NZ_KEY_LEFT; else if (x1 < 118) held |= 1u << NZ_KEY_RIGHT;
        if (y1 < 280) held |= 1u << NZ_KEY_DOWN; else if (y1 > 300) held |= 1u << NZ_KEY_UP;
        if (!held) break;
        ui_buttons(held); frames(1);
    }
    ui_buttons(0); frames(1);
    ui_buttons(1u << NZ_KEY_A); frames(3);       /* press A ... */
    ui_buttons(0); frames(5);                    /* ... and let go: a click */
    check(desktop_window_count() == 1, "D-pad + A on the System icon opens a window");
    save("core_3_desktop_window");

    /* click the window's close button (top right of the window, x 475, y 55) */
    ui_mouse(320, 240, 0, 0, 1); frames(1);      /* sync the fake mouse with the pointer */
    ui_pointer_pos(&x1, &y1);
    ui_mouse(320 + (475 - x1), 240 + (55 - y1), 0, 0, 1); frames(2);
    ui_mouse(320 + (475 - x1), 240 + (55 - y1), 1, 1, 1); frames(2);
    ui_mouse(320 + (475 - x1), 240 + (55 - y1), 0, 0, 1); frames(5);
    check(desktop_window_count() == 0, "clicking X closes the window");

    /* ---- stability: every app opened and closed 50 times, buttons pressed,
     *      B always leads back home, and no memory is lost ---- */
    {
        static const int ids[] = { APP_TERMINAL, APP_FILES, APP_EDITOR, APP_PYTHON, APP_CAR, APP_WEB, APP_ELECTRONICS,
                                   APP_ARDUINO, APP_CLAUDE, APP_MEDIA, APP_WIFI, APP_SETTINGS, APP_TASKMGR,
                                   APP_CALCULATOR, APP_NOTES, APP_CLOCK, APP_IMAGES, APP_WHATSAPP, APP_PROBE, APP_DEVTOOLS, APP_HEALTH, APP_BTSCAN, APP_UPDATE };
        static const int presses[] = { NZ_KEY_DOWN, NZ_KEY_UP, NZ_KEY_RIGHT, NZ_KEY_LEFT, NZ_KEY_L, NZ_KEY_R, NZ_KEY_DOWN };
        ui_set_mode(UI_HANDHELD);
        frame_hook = NULL;
        int stuck = 0, worst_app = -1;
        long worst = 0;
        for (size_t a = 0; a < sizeof ids / sizeof ids[0]; a++) {
            for (int warm = 0; warm < 2; warm++) {          /* first opens fill caches: not a leak */
                ui_open_app(ids[a]); fast_frames(3);
                for (int b = 0; b < 6 && apps_focused(); b++) { ui_key(NZ_KEY_B); fast_frames(2); }
            }
            long before = heap_used();
            int rounds = getenv("NZ_SOAK") ? atoi(getenv("NZ_SOAK")) : 50;   /* NZ_SOAK=200: a longer run */
            for (int round = 0; round < rounds; round++) {
                ui_open_app(ids[a]); fast_frames(2);
                for (size_t p = 0; p < sizeof presses / sizeof presses[0]; p++) { ui_key(presses[p]); fast_frames(1); }
                for (int b = 0; b < 6 && apps_focused(); b++) { ui_key(NZ_KEY_B); fast_frames(2); }
                if (apps_focused() || nzui_active()) { stuck++; printf("NOTE  B did not lead home from %s\n", home_app_name(ids[a]));
                    if (nzui_active()) nzui_key(NZ_KEY_B);
                    NzApp *f = apps_focused(); if (f) apps_close(f); fast_frames(2); }
            }
            fast_frames(5);
            long grew = heap_used() - before;
            printf("      %-14s 50 x open/close: memory %+ld bytes\n", home_app_name(ids[a]), grew);
            if (grew > worst) { worst = grew; worst_app = ids[a]; }
        }
        check(stuck == 0, "every app: B always leads back to the home screen");
        /* the same in desktop mode: apps in windows, opened and closed 20 times */
        ui_set_mode(UI_DESKTOP);
        long wbefore = heap_used(), wgrew;
        int wleft = 0;
        for (int round = 0; round < 21; round++) {
            if (round == 1) wbefore = heap_used();         /* (round 0 warms up) */
            for (size_t a = 0; a < sizeof ids / sizeof ids[0]; a += 4) {
                ui_open_app(ids[a]); fast_frames(2);
                NzApp *w = apps_get(ids[a]);
                if (w && w->root) apps_close(w);
                fast_frames(2);
            }
        }
        fast_frames(5);
        wleft = desktop_window_count();
        wgrew = heap_used() - wbefore;
        printf("      windows: 20 rounds of 5 apps: memory %+ld bytes, windows left %d\n", wgrew, wleft);
        check(wleft == 0 && wgrew < 4096, "desktop mode: app windows open and close 20 times, nothing left behind");
        ui_set_mode(UI_HANDHELD);
        char what[120];
        snprintf(what, sizeof what, "no app loses memory when opened and closed 50 times (worst: %s %+ld bytes)",
                 worst_app >= 0 ? home_app_name(worst_app) : "-", worst);
        check(worst < 4096, what);
    }

    /* the clock is saved when NZEUS closes */
    unlink(CLOCK_TXT);
    hw_test_set_now(saved_t + 86400 * 3);
    void retro_deinit(void);
    retro_deinit();
    long long at_close = clock_file();
    check(at_close >= saved_t + 86400 * 3 && at_close <= saved_t + 86400 * 3 + 5, "Clock: saved to nzeus/clock.txt when NZEUS closes");

    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

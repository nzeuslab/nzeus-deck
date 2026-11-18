/* app_arduino.c - the NZEUS Arduino app: a serial monitor, like the one in
 * the Arduino IDE. Talks to Arduino, STM32, ESP32 boards (and any other
 * USB-serial device) through /dev/ttyACM* or /dev/ttyUSB*.
 *
 *   A       on-screen keyboard (type a line)     START  send the line
 *   X       menu: port, speed (baud), line ending, clear, save to the SD card
 *   Y       connect / disconnect                 L1/R1  scroll back / forward
 *   B       hide the keyboard / leave
 * X > "Upload a program" sends a ready-made firmware file (.hex exported from
 * the Arduino IDE on the PC, or .bin/.hex for STM32) to the board, with
 * nzeus/bin/nzeus-upload.sh (avrdude / stm32flash, R36S/apps/upload). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "serial.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "bgjob.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#define LINES     200
#define LINE_LEN  100
#define LINE_H    18

static struct {
    lv_obj_t *status, *out, *ta, *kb, *hint;
    int fd, scroll, dirty;
    char port[48];
    char (*lines)[LINE_LEN];       /* received text: a ring of lines (20 KB, borrowed while the app is open) */
    int first, count;              /* oldest line, how many */
    char partial[LINE_LEN];        /* the line still arriving */
    int plen;
    lv_timer_t *timer;
    BgJob upload;                  /* a firmware upload running in the background */
    long upload_seen;              /* how much of its output we've shown */
    lv_obj_t *bar;                 /* upload progress */
} R;

static const char *const endings[] = { "\n", "\r\n", "" };
static const char *const ending_names[] = { "newline", "CR + newline", "nothing" };

static int baud(void) { return settings_get_int("arduino_baud", 9600); }
static int ending(void) { int e = settings_get_int("arduino_ending", 0); return e >= 0 && e < 3 ? e : 0; }

static void push_line(const char *s)
{
    if (!R.lines) return;                                  /* (no memory for the log) */
    int slot = (R.first + R.count) % LINES;
    if (R.count == LINES) R.first = (R.first + 1) % LINES; else R.count++;
    snprintf(R.lines[slot], LINE_LEN, "%s", s);
    R.dirty = 1;
}

/* bytes from the board: split into lines (\n), ignore \r, show other
 * control characters as nothing */
static void received(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || R.plen == LINE_LEN - 1) { R.partial[R.plen] = 0; push_line(R.partial); R.plen = 0; if (c == '\n') continue; }
        if (c == '\r' || (unsigned char)c < 32) continue;
        R.partial[R.plen++] = c;
    }
    R.dirty = 1;
}

static void show_status(void)
{
    if (R.fd >= 0) lv_label_set_text_fmt(R.status, LV_SYMBOL_OK "  %s   %d baud   ends with %s", R.port, baud(), ending_names[ending()]);
    else if (R.port[0]) lv_label_set_text_fmt(R.status, "Not connected   %s   %d baud   (Y connects)", R.port, baud());
    else lv_label_set_text(R.status, "No board found - plug it in with a USB cable (OTG)");
}

static void render(void)
{
    int rows = lv_obj_get_content_height(R.out) / LINE_H;
    if (rows < 1) rows = 1;
    int total = R.count + (R.plen ? 1 : 0);
    if (R.scroll > total - rows) R.scroll = total - rows > 0 ? total - rows : 0;
    int end = total - R.scroll, start = end - rows < 0 ? 0 : end - rows;
    static char text[64 * (LINE_LEN + 1)];
    size_t k = 0;
    for (int i = start; i < end && k < sizeof text - LINE_LEN - 2; i++) {
        const char *l = i < R.count ? R.lines[(R.first + i) % LINES] : R.partial;
        size_t len = i < R.count ? strlen(l) : (size_t)R.plen;
        memcpy(text + k, l, len);
        k += len;
        text[k++] = '\n';
    }
    if (k) k--;
    text[k] = 0;
    if (!total) snprintf(text, sizeof text, "(what the board sends appears here)");
    lv_label_set_text(R.out, text);
    R.dirty = 0;
}

static void connect(int on)
{
    if (R.fd >= 0) { serial_close(R.fd); R.fd = -1; }
    if (on && R.port[0]) {
        R.fd = serial_open(R.port, baud());
        if (R.fd < 0) ui_toast("Could not open the port");
        else ui_toast("Connected");
    }
    show_status();
}

static void pick_port(void)                 /* the first port there, if none chosen */
{
    char ports[SERIAL_MAX_PORTS][48];
    int n = serial_list(ports, SERIAL_MAX_PORTS);
    if (!R.port[0] && n) { snprintf(R.port, sizeof R.port, "%s", ports[0]); connect(1); }
}

static void send_line(void)
{
    const char *t = lv_textarea_get_text(R.ta);
    if (R.fd < 0) { ui_toast("Not connected (Y connects)"); return; }
    serial_write(R.fd, t, (int)strlen(t));
    serial_write(R.fd, endings[ending()], (int)strlen(endings[ending()]));
    char echo[LINE_LEN];
    snprintf(echo, sizeof echo, "> %s", t);              /* show what we sent */
    push_line(echo);
    lv_textarea_set_text(R.ta, "");
    R.scroll = 0;
}

/* ---------- uploading a program ---------- */
static const char *const boards[] = { "uno", "nano", "nano-old", "mega", "stm32" };
static const char *const board_names[] = { "Arduino Uno", "Arduino Nano", "Arduino Nano (old bootloader)",
                                           "Arduino Mega 2560", "STM32 (serial bootloader, BOOT0 = 1)" };
static int up_board;
static char up_files[16][256];
static int up_count;

static int firmware_ok(const char *name, int stm32)
{
    const char *dot = strrchr(name, '.');
    if (!dot || strstr(name, "with_bootloader")) return 0;     /* that one would overwrite the bootloader */
    return !strcasecmp(dot, ".hex") || (stm32 && !strcasecmp(dot, ".bin"));
}

static void find_firmware(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && up_count < 16) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof up_files[0]) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) find_firmware(p, depth - 1); continue; }
        if (firmware_ok(e->d_name, up_board == 4)) memcpy(up_files[up_count++], p, strlen(p) + 1);
    }
    closedir(d);
}

static void upload_start(int f)
{
    char cmd[700];
    if (f < 0 || f >= up_count) return;
    if (!R.port[0]) { ui_toast("No board found - plug it in first"); return; }
    serial_close(R.fd);                                        /* the uploader needs the port to itself */
    R.fd = -1;
    snprintf(cmd, sizeof cmd, "sh %s/bin/nzeus-upload.sh %s %s '%s'", NZ_DIR, boards[up_board], R.port, up_files[f]);
    if (bgjob_start(&R.upload, cmd, "/tmp/nzeus-upload.log") != 0) { ui_toast("Could not start the upload"); return; }
    R.upload_seen = 0;
    push_line("--- upload started ---");
    lv_obj_clear_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(R.bar, 0, LV_ANIM_OFF);
    show_status();
}

static void file_chosen(int c, void *ctx) { (void)ctx; if (c >= 0) upload_start(c); }

static void board_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c < 0) return;
    up_board = c;
    up_count = 0;
    find_firmware(NZ_DIR "/firmware", 2);                     /* nzeus/firmware/ first */
    find_firmware(NZ_SDCARD, 1);
    if (!up_count) { ui_toast(c == 4 ? "No .bin/.hex file: put it in nzeus/firmware" : "No .hex file: put it in nzeus/firmware"); return; }
    static const char *items[16];
    for (int i = 0; i < up_count; i++) { const char *s = strrchr(up_files[i], '/'); items[i] = s ? s + 1 : up_files[i]; }
    nz_menu("Which program?", items, up_count, file_chosen, NULL);
}

/* show the uploader's lines as they come; STEP/PROGRESS drive the status + bar */
static void upload_poll(void)
{
    if (!R.upload.out_path[0]) return;
    int running = bgjob_poll(&R.upload);
    FILE *f = fopen(R.upload.out_path, "r");
    if (f) {
        char line[160];
        fseek(f, R.upload_seen, SEEK_SET);
        while (fgets(line, sizeof line, f)) {
            if (!strchr(line, '\n') && running) break;           /* half a line: next time */
            R.upload_seen = ftell(f);
            line[strcspn(line, "\r\n")] = 0;
            int pct;
            char what[16];
            if (sscanf(line, "PROGRESS %15s %d", what, &pct) == 2) {
                lv_bar_set_value(R.bar, pct, LV_ANIM_OFF);
                lv_label_set_text_fmt(R.status, "%s  %d %%", !strcmp(what, "verify") ? "Checking" : "Writing", pct);
            } else if (!strncmp(line, "STEP ", 5)) {
                lv_label_set_text(R.status, line + 5);
                push_line(line + 5);
            } else if (line[0]) push_line(line);
        }
        fclose(f);
    }
    if (!running) {                                            /* finished: the last line says how */
        ui_toast(R.upload.exit_code == 0 ? "Upload OK" : "Upload failed - see the messages");
        lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        R.upload.out_path[0] = 0;
        connect(1);                                            /* back to the serial monitor */
    }
}

int app_arduino_uploading(void) { return R.upload.out_path[0] != 0; }
int app_arduino_firmware_count(void) { return up_count; }

static void poll(lv_timer_t *t)
{
    (void)t;
    if (R.upload.out_path[0]) { upload_poll(); if (R.dirty) render(); return; }   /* (the port is busy) */
    if (R.fd < 0) pick_port();
    if (R.fd >= 0) {
        char buf[512];
        int n;
        while ((n = serial_read(R.fd, buf, sizeof buf)) > 0) received(buf, n);
        if (n < 0) { serial_close(R.fd); R.fd = -1; R.port[0] = 0; ui_toast("The board was unplugged"); show_status(); }
    }
    if (R.dirty) render();
}

/* ---------- menu ---------- */
static char port_names[SERIAL_MAX_PORTS][48];
static void port_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c < 0) return;
    snprintf(R.port, sizeof R.port, "%s", port_names[c]);
    connect(1);
}
static void baud_chosen(int c, void *ctx)
{
    (void)ctx;
    if (c < 0) return;
    settings_set_int("arduino_baud", serial_bauds[c]);
    if (R.fd >= 0) serial_set_baud(R.fd, serial_bauds[c]);
    show_status();
}
static void ending_chosen(int c, void *ctx) { (void)ctx; if (c >= 0) settings_set_int("arduino_ending", c); show_status(); }

static void save_log(void)
{
    char path[128];
    time_t now = time(NULL);
    snprintf(path, sizeof path, NZ_DIR "/serial-log-%ld.txt", (long)now);
    FILE *f = fopen(path, "w");
    if (!f) { ui_toast("Could not save"); return; }
    for (int i = 0; i < R.count; i++) fprintf(f, "%s\n", R.lines[(R.first + i) % LINES]);
    fclose(f);
    ui_toast("Saved in nzeus/ (serial-log-....txt)");
}

static void menu_chosen(int c, void *ctx)
{
    (void)ctx;
    static const char *bnames[] = { "9600", "19200", "38400", "57600", "115200" };
    switch (c) {
    case 0: {
        int n = serial_list(port_names, SERIAL_MAX_PORTS);
        static const char *items[SERIAL_MAX_PORTS];
        for (int i = 0; i < n; i++) items[i] = port_names[i];
        if (!n) ui_toast("No port: plug a board in");
        else nz_menu("Port", items, n, port_chosen, NULL);
        break;
    }
    case 1: nz_menu("Speed (baud) - must match Serial.begin()", bnames, 5, baud_chosen, NULL); break;
    case 2: nz_menu("End each sent line with", ending_names, 3, ending_chosen, NULL); break;
    case 3: R.count = 0; R.first = 0; R.plen = 0; R.dirty = 1; break;
    case 4: save_log(); break;
    case 5: nz_menu("Which board?", board_names, 5, board_chosen, NULL); break;
    default: break;
    }
}

/* ---------- the app ---------- */
static void show_keyboard(int on)
{
    if (on) { lv_obj_clear_flag(R.kb, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(R.hint, LV_OBJ_FLAG_HIDDEN); lv_group_focus_obj(R.kb); }
    else { lv_obj_add_flag(R.kb, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(R.hint, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_update_layout(R.out);
    R.dirty = 1;
}

static void ta_event(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_READY) send_line(); }

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&R, 0, sizeof R);
    R.lines = calloc(LINES, sizeof *R.lines);
    if (!R.lines) ui_toast("Not enough memory for the serial log");
    R.fd = -1;
    th_add(root, TS_TERM);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bar = th_box(root, TS_BAR);
    lv_obj_set_size(bar, LV_PCT(100), 28);
    lv_obj_t *ic = lv_img_create(bar);
    lv_img_set_src(ic, icon_get(ICON_SERIAL, 20));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    R.status = th_label(bar, "", TS_SMALL);
    lv_obj_align(R.status, LV_ALIGN_LEFT_MID, 36, 0);

    R.out = th_label(root, "", TS_MONO);
    lv_obj_set_width(R.out, LV_PCT(100));
    lv_obj_set_flex_grow(R.out, 1);
    lv_label_set_long_mode(R.out, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_hor(R.out, 6, 0);
    lv_obj_set_style_pad_top(R.out, 4, 0);

    R.ta = lv_textarea_create(root);
    lv_textarea_set_one_line(R.ta, true);
    lv_textarea_set_placeholder_text(R.ta, "type here, START sends");
    th_add(R.ta, TS_MONO);
    lv_obj_set_width(R.ta, LV_PCT(100));
    lv_obj_add_state(R.ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(R.ta, ta_event, LV_EVENT_READY, NULL);

    R.kb = lv_keyboard_create(root);
    lv_obj_set_size(R.kb, LV_PCT(100), 170);
    lv_keyboard_set_textarea(R.kb, R.ta);
    lv_group_add_obj(apps_keypad_group(), R.kb);
    lv_obj_add_flag(R.kb, LV_OBJ_FLAG_HIDDEN);

    R.bar = lv_bar_create(root);                    /* upload progress (hidden until then) */
    lv_bar_set_range(R.bar, 0, 100);
    lv_obj_set_size(R.bar, LV_PCT(100), 6);
    th_add(R.bar, TS_BTN);
    th_add_to(R.bar, TS_BTN_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);

    R.hint = th_box(root, TS_BAR);
    lv_obj_set_size(R.hint, LV_PCT(100), 24);
    static const char *const hints[] = { "A", "Type", "START", "Send", "X", "Menu", "Y", "Connect", "L1/R1", "Scroll" };
    lv_obj_center(nz_hints(R.hint, hints, 5));

    lv_obj_update_layout(root);
    show_status();
    poll(NULL);
    render();
    R.timer = lv_timer_create(poll, 50, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    int kb_open = !lv_obj_has_flag(R.kb, LV_OBJ_FLAG_HIDDEN);
    switch (k) {
    case NZ_KEY_START: send_line(); return 1;
    case NZ_KEY_L: R.scroll += 5; R.dirty = 1; return 1;
    case NZ_KEY_R: R.scroll = R.scroll > 5 ? R.scroll - 5 : 0; R.dirty = 1; return 1;
    case NZ_KEY_Y: connect(R.fd < 0); return 1;
    case NZ_KEY_X: {
        static const char *const items[] = { "Port...", "Speed (baud)...", "Line ending...", "Clear", "Save to the SD card",
                                             "Upload a program (.hex)..." };
        if (R.upload.out_path[0]) { ui_toast("An upload is running"); return 1; }
        nz_menu("Serial monitor", items, 6, menu_chosen, NULL);
        return 1;
    }
    default: break;
    }
    if (!kb_open) {
        if (k == NZ_KEY_A) { show_keyboard(1); return 1; }
        return 0;
    }
    switch (k) {
    case NZ_KEY_UP:    apps_keypad_send(LV_KEY_UP);    return 1;
    case NZ_KEY_DOWN:  apps_keypad_send(LV_KEY_DOWN);  return 1;
    case NZ_KEY_LEFT:  apps_keypad_send(LV_KEY_LEFT);  return 1;
    case NZ_KEY_RIGHT: apps_keypad_send(LV_KEY_RIGHT); return 1;
    case NZ_KEY_A:     apps_keypad_send(LV_KEY_ENTER); return 1;
    case NZ_KEY_B:     show_keyboard(0);               return 1;
    default:           return 0;
    }
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    (void)a;
    switch (ev->key) {
    case KB_CHAR:      lv_textarea_add_char(R.ta, (uint32_t)(unsigned char)ev->ch); break;
    case KB_ENTER:     send_line(); break;
    case KB_BACKSPACE: lv_textarea_del_char(R.ta); break;
    case KB_PGUP:      R.scroll += 10; R.dirty = 1; break;
    case KB_PGDN:      R.scroll = R.scroll > 10 ? R.scroll - 10 : 0; R.dirty = 1; break;
    default: break;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    bgjob_stop(&R.upload);                         /* (closing the app stops an upload) */
    if (R.timer) lv_timer_del(R.timer);
    serial_close(R.fd);
    free(R.lines);                                 /* the log's memory goes back */
    memset(&R, 0, sizeof R);
    R.fd = -1;
}

/* for tests */
int app_arduino_connected(void) { return R.fd >= 0; }
const char *app_arduino_last_line(void) { return R.count && R.lines ? R.lines[(R.first + R.count - 1) % LINES] : ""; }

NzApp app_arduino = {
    .id = APP_ARDUINO, .title = "Arduino", .icon = ICON_ARDUINO,
    .create = create, .key = key, .kbd = kbd, .destroy = destroy,
};

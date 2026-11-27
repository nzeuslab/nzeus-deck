/* app_settings.c - the NZEUS Settings app.
 *
 * Left: the sections (Appearance, Sound, Display, ...). Right: the rows of
 * the chosen section. Every change happens at once and is saved to
 * nzeus/settings.ini straight away.
 *
 * Buttons:  UP/DOWN      move               A       open a section / press a button
 *           LEFT/RIGHT   change a value     B       back to the sections / leave
 * Mouse:    click a section, click a row; click the < > of a value.
 *
 * Bluetooth has pages of its own: My devices, Find devices, Pairing.
 * There B (or clicking the "<" title) goes back one page.
 * USB: the USB-C port's mode, Devices or PC connection (usbmode.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "bgjob.h"
#include "bt.h"
#include "home.h"
#include "hw.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "sysinfo.h"
#include "wifi.h"
#include "theme.h"
#include "ui.h"
#include "usbmode.h"

enum { SEC_APPEARANCE, SEC_SOUND, SEC_DISPLAY, SEC_TIME, SEC_CONTROLS, SEC_STORAGE,
       SEC_STARTUP, SEC_WIFI, SEC_BLUETOOTH, SEC_USB, SEC_CLAUDE, SEC_ABOUT, SEC_POWER, SEC_COUNT };
static const char *const sec_names[SEC_COUNT] = {
    "Appearance", "Sound", "Display", "Date & time", "Controls", "Storage",
    "Startup", "Wi-Fi", "Bluetooth", "USB", "Remote access", "About", "Power",
};
static const int sec_icons[SEC_COUNT] = {           /* -1, -2: the Bluetooth / USB signs (font symbols) */
    ICON_THEME, ICON_MUSIC, ICON_DESKTOP, ICON_CLOCK, ICON_KEYBOARD, ICON_FOLDER,
    ICON_HANDHELD, ICON_WIFI, -1, -2, ICON_LOCK, ICON_ABOUT, ICON_POWER,
};

/* One row on the right: a value you change with LEFT/RIGHT, or a button. */
enum { ROW_VALUE, ROW_BUTTON, ROW_TEXT };
typedef struct {
    int type;
    const char *label;
    void (*change)(int dir);          /* ROW_VALUE: dir = -1 or +1 */
    void (*value)(char *buf, size_t n);  /* ROW_VALUE / ROW_TEXT: what to show */
    void (*press)(void);              /* ROW_BUTTON */
} Row;

#define MAX_ROWS 8
/* the Bluetooth section has pages of its own (B goes back one page) */
enum { PAGE_ROWS, PAGE_MINE, PAGE_FIND, PAGE_PAIR };
static struct {
    lv_obj_t *secs[SEC_COUNT], *panel, *rows[MAX_ROWS], *vals[MAX_ROWS], *extra;
    const Row *row_defs;
    int row_count, sec, row, in_panel;
    lv_timer_t *timer;
    /* Bluetooth */
    int page, list_ver, scan_ver, dev_connected;
    NzList *list;                                 /* my devices / devices found */
    lv_obj_t *big, *info, *bar, *hint_word;       /* the pairing page; the search bar */
    char dev_addr[18], dev_name[40];              /* the device being paired / changed */
    lv_timer_t *bt_timer;
} S;
static char bt_msg[100];                          /* the last Bluetooth message (for tests) */

/* ---------- the values ---------- */
static const char *const repeat_names[] = { "Slow", "Normal", "Fast" };
static const int repeat_ms[] = { 200, 120, 70 };
static const char *const start_names[] = { "Home screen", "Desktop", "Terminal", "Files" };
static const char *const start_ids[] = { "home", "desktop", "terminal", "files" };

static int find_index(const char *const ids[], int n, const char *id)
{
    for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) return i;
    return 0;
}
static int wrap(int v, int n) { return (v % n + n) % n; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Appearance */
static void theme_change(int dir) { theme_set(wrap(theme_index() + dir, TH_COUNT)); }
static void theme_value(char *b, size_t n) { snprintf(b, n, "%s", theme()->name); }

/* Sound */
static void volume_change(int dir)
{
    int v = clampi(settings_get_int("volume", 70) + dir * 10, 0, 100);
    settings_set_int("volume", v);
    if (hw_set_volume(v) != 0) ui_toast("Volume: no sound device here");
}
static void volume_value(char *b, size_t n)
{
    if (hw_get_volume() < 0) snprintf(b, n, "no sound card");
    else snprintf(b, n, "%d %%", settings_get_int("volume", 70));
}
/* the speaker on/off (for headphones: this console cannot sense them); FN + Vol does the same */
static void speaker_change(int dir)
{
    (void)dir;
    int on = hw_get_speaker();
    if (on < 0 || hw_set_speaker(!on) != 0) { ui_toast("Speaker: no switch on this system"); return; }
    settings_set_int("speaker", !on);
}
static void speaker_value(char *b, size_t n)
{
    int on = hw_get_speaker();
    snprintf(b, n, "%s", on < 0 ? "always on" : on ? "On" : "Off (headphones only)");
}

/* Display */
static void bright_change(int dir)
{
    int v = clampi(settings_get_int("brightness", 80) + dir * 10, 10, 100);   /* never fully dark */
    settings_set_int("brightness", v);
    if (hw_set_brightness(v) != 0) ui_toast("Brightness: no backlight device here");
}
static void bright_value(char *b, size_t n) { snprintf(b, n, "%d %%", settings_get_int("brightness", 80)); }

/* Date & time */
static void tz_change(int dir)
{
    int tz = clampi(settings_get_int("tz", 0) + dir, -12, 14);
    settings_set_int("tz", tz);
    hw_apply_timezone(tz);
}
static void tz_value(char *b, size_t n) { snprintf(b, n, "UTC %+d", settings_get_int("tz", 0)); }
static void time_text(char *b, size_t n, const char *fmt) { time_t t = hw_now(); strftime(b, n, fmt, localtime(&t)); }
static void now_value(char *b, size_t n) { time_text(b, n, "%a %d %b %Y  %H:%M:%S"); }
/* set the clock (it is saved on the SD card at once, see hw.h) */
static void set_time(time_t t)
{
    if (hw_set_time(t) != 0) ui_toast("Could not set the clock");
}
static void clock_add(int minutes)
{
    time_t t = hw_now() + minutes * 60;
    set_time(t - t % 60);                             /* (the seconds start again at 0) */
}
static void hour_change(int dir) { clock_add(dir * 60); }
static void hour_value(char *b, size_t n) { time_text(b, n, "%H"); }
static void min_change(int dir) { clock_add(dir); }
static void min_value(char *b, size_t n) { time_text(b, n, "%M"); }
/* the date: Day goes on into the next month; Month and Year keep the day
 * (31 March - 1 month = 28/29 February) */
static int days_in_month(int year, int mon)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return mon == 1 && leap ? 29 : days[mon];
}
static void date_add(int months)
{
    time_t t = hw_now();
    struct tm tm = *localtime(&t);
    int mon = tm.tm_year * 12 + tm.tm_mon + months;  /* months since 1900 */
    int year = clampi(mon / 12 + 1900, 2024, 2037);   /* the console's clock works up to 2037 */
    tm.tm_year = year - 1900;
    tm.tm_mon = mon % 12;
    if (tm.tm_mday > days_in_month(year, tm.tm_mon)) tm.tm_mday = days_in_month(year, tm.tm_mon);
    set_time(mktime(&tm));
}
static void day_change(int dir) { set_time(hw_now() + dir * 24 * 3600); }
static void day_value(char *b, size_t n) { time_text(b, n, "%a %d"); }
static void month_change(int dir) { date_add(dir); }
static void month_value(char *b, size_t n) { time_text(b, n, "%B"); }
static void year_change(int dir) { date_add(dir * 12); }
static void year_value(char *b, size_t n) { time_text(b, n, "%Y"); }

/* Controls */
static void accept_change(int dir)
{
    if (!dir) return;
    settings_set("accept_button", settings_accept_is_b() ? "a" : "b");
    nz_hints_refresh();
}
static void accept_value(char *b, size_t n)
{
    snprintf(b, n, "%s to accept", settings_accept_is_b() ? "B" : "A");
}

static int repeat_index(void)
{
    int ms = settings_get_int("repeat_ms", 120);
    for (int i = 0; i < 3; i++) if (repeat_ms[i] == ms) return i;
    return 1;
}
static void repeat_change(int dir) { settings_set_int("repeat_ms", repeat_ms[clampi(repeat_index() + dir, 0, 2)]); }
static void repeat_value(char *b, size_t n) { snprintf(b, n, "%s", repeat_names[repeat_index()]); }

/* Pin L12 = the Vol- button OR the debug serial line (one pin, one job). The
 * switch is nzeus-voldown on Debian (ports/r36s-hichip/rootfs/debian/overlay/usr/local/
 * sbin/nzeus-voldown); the kernel shows the state in the power driver's sysfs. */
const char *voldown_sys_file = "/sys/bus/platform/devices/188184d0.power/voldown";   /* (tests use fakes) */
const char *voldown_cmd = "/usr/local/sbin/nzeus-voldown";
static int voldown_is_key(void)                   /* 1 = Vol- button, 0 = serial line, -1 = can't */
{
    char buf[8] = "";
    FILE *f = fopen(voldown_sys_file, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    return !strncmp(buf, "key", 3);
}
static void voldown_change(int dir)
{
    (void)dir;                                    /* two choices: LEFT or RIGHT both switch */
    int key = voldown_is_key();
    char cmd[200];
    if (key < 0 || access(voldown_cmd, X_OK) != 0) { ui_toast("Not possible on this system"); return; }
    snprintf(cmd, sizeof cmd, "%s %s > /dev/null 2>&1", voldown_cmd, key ? "uart" : "key");
    if (system(cmd) != 0)
        ui_toast("Could not switch pin L12");
    else
        ui_toast(key ? "Pin L12 is the debug serial line now" : "The Vol- button works now");
}
static void voldown_value(char *b, size_t n)
{
    int key = voldown_is_key();
    snprintf(b, n, "%s", key < 0 ? "Not here" : key ? "Vol- button" : "Debug serial line");
}
static void keys_value(char *b, size_t n) { snprintf(b, n, "SELECT: handheld / desktop"); }
static void shot_keys_value(char *b, size_t n) { snprintf(b, n, "hold SELECT, press L1"); }

/* Storage */
static void sd_value(char *b, size_t n)
{
    struct statvfs s;
    if (statvfs(NZ_SDCARD, &s) != 0) { snprintf(b, n, "not found"); return; }
    double free_gb = (double)s.f_bavail * s.f_frsize / 1e9, total = (double)s.f_blocks * s.f_frsize / 1e9;
    snprintf(b, n, "%.1f GB free of %.1f GB", free_gb, total);
}
static void ram_value(char *b, size_t n)
{
    long total = sysinfo_meminfo_kb("MemTotal"), avail = sysinfo_meminfo_kb("MemAvailable");
    if (avail < 0) avail = sysinfo_meminfo_kb("MemFree");
    if (total <= 0) { snprintf(b, n, "?"); return; }
    snprintf(b, n, "%ld MB free of %ld MB", avail / 1024, total / 1024);
}

/* Card health: after a forced power-off the card's file system can get a small
 * error; Linux then makes it READ-ONLY. The SD Debian checks (fsck) its root
 * and its /data partition at every start, so a restart repairs it. */
const char *settings_mounts_file = "/proc/mounts";       /* (tests use a fake one) */
int settings_card_readonly(void)
{
    char line[256];
    int ro = 0;
    FILE *f = fopen(settings_mounts_file, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char dev[64], mnt[128], type[16], opts[96];
        if (sscanf(line, "%63s %127s %15s %95s", dev, mnt, type, opts) == 4 &&
            (!strcmp(mnt, "/") || !strcmp(mnt, "/data")) && strcmp(dev, "rootfs") &&
            (!strcmp(opts, "ro") || !strncmp(opts, "ro,", 3))) ro = 1;
    }
    fclose(f);
    return ro;
}
static void health_value(char *b, size_t n)
{
    snprintf(b, n, "%s", settings_card_readonly() ? "READ-ONLY - restart to repair" : "OK (writable)");
}

/* Startup */
static int start_index(void) { return find_index(start_ids, 4, settings_get("start", "home")); }
static void start_change(int dir) { settings_set("start", start_ids[wrap(start_index() + dir, 4)]); }
static void start_value(char *b, size_t n) { snprintf(b, n, "%s", start_names[start_index()]); }

/* The SD Debian runs Dropbear (etc/init.d/nzeus-ssh) on port 22, keys only (-s):
 * reachable over the USB cable (192.168.77.1) and over Wi-Fi. Shown, not switched. */
const char *settings_tcp_file = "/proc/net/tcp";          /* (tests use a fake one) */
static int ssh_listening(void)
{
    char line[200];
    int on = 0;
    FILE *f = fopen(settings_tcp_file, "r");
    if (!f) return 0;
    while (!on && fgets(line, sizeof line, f)) {   /* local address :0016 (= 22) in state 0A (listen) */
        unsigned port, st;
        if (sscanf(line, " %*d: %*8x:%x %*8x:%*x %x", &port, &st) == 2 && port == 22 && st == 0x0A) on = 1;
    }
    fclose(f);
    return on;
}
static void ssh_value(char *b, size_t n) { snprintf(b, n, "%s", ssh_listening() ? "ON (port 22, keys only)" : "off"); }
static void ssh_where_value(char *b, size_t n)
{
    char ip[20] = "";
    snprintf(ip, sizeof ip, "%s", wifi_ip());
    if (ip[0]) snprintf(b, n, "Cable: root@192.168.77.1   Wi-Fi: root@%s", ip);
    else snprintf(b, n, "Cable: root@192.168.77.1   Wi-Fi: not connected");
}
static void ssh_keys_value(char *b, size_t n) { snprintf(b, n, "/root/.ssh/authorized_keys"); }

/* Switches kept as flag files: there = ON. nzeus/bin/wifi-auto.sh (started
 * when Devices mode comes on) reads wifi/autojoin-off. */
const char *wifi_autooff_file = NZ_DIR "/wifi/autojoin-off";       /* (tests use a fake path) */
static int flag_on(const char *path) { return access(path, F_OK) == 0; }
static void flag_set(const char *path, int on)
{
    if (!on) { remove(path); return; }
    char dir[160];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }      /* (the folder may not exist yet) */
    FILE *f = fopen(path, "w");
    if (f) fclose(f); else ui_toast("Could not save that (is the SD card read-only?)");
}

/* Wi-Fi: the network list is the Wi-Fi app (app_wifi.c); here are the
 * status, the "join at power-on" switch and how many networks are saved */
static void wifi_open_press(void) { ui_open_app(APP_WIFI); }
static void wifi_auto_change(int dir) { (void)dir; flag_set(wifi_autooff_file, !flag_on(wifi_autooff_file)); }
static void wifi_auto_value(char *b, size_t n) { snprintf(b, n, "%s", flag_on(wifi_autooff_file) ? "off" : "ON"); }
static void wifi_saved_value(char *b, size_t n)
{
    char names[16][33];
    int c = wifi_saved_list(names, 16);
    if (c) snprintf(b, n, "%d saved", c);
    else snprintf(b, n, "none yet");
}
static void wifi_status_value(char *b, size_t n)
{
    const char *link = wifi_link_status();
    char name[40] = "";
    const char *s = strstr(link, "ssid="), *e = strstr(link, " bssid=");
    if (s && e && e > s) snprintf(name, sizeof name, "%.*s", (int)(e - s - 5) < 39 ? (int)(e - s - 5) : 39, s + 5);
    int state = wifi_state();
    if (wifi_dongle() == DONGLE_NONE) snprintf(b, n, "no dongle plugged in");
    else if (state == WIFI_STARTING) snprintf(b, n, "switching on...");
    else if (!strncmp(link, "state=COMPLETED", 15) && wifi_ip()[0]) snprintf(b, n, "%s - %s", name, wifi_ip());
    else if (!strncmp(link, "state=COMPLETED", 15)) snprintf(b, n, "%s - getting an address...", name);
    else if (state == WIFI_ON) snprintf(b, n, "on, not joined");
    else snprintf(b, n, "off");
}

/* About */
static void ver_value(char *b, size_t n) { snprintf(b, n, "NZEUS Deck 0.9"); }
static void kernel_value(char *b, size_t n) { sysinfo_kernel(b, n); }
static void cpu_value(char *b, size_t n) { snprintf(b, n, "HiChip HC16xx, MIPS 74Kc"); }
static void credits_value(char *b, size_t n) { snprintf(b, n, "LVGL (MIT), unscii (public domain)"); }

/* Power */
static void restart_answer(int yes, void *ctx) { (void)ctx; if (yes) ui_run_outside(OUTSIDE_RESTART); }
static void reboot_answer(int yes, void *ctx) { (void)ctx; if (yes && hw_reboot() != 0) ui_toast("Reboot is not possible here"); }
static void off_answer(int yes, void *ctx) { (void)ctx; if (yes && hw_power_off() != 0) ui_toast("Power off is not possible here"); }
static void restart_press(void) { nz_confirm("Restart NZEUS Deck?", restart_answer, NULL); }
static void reboot_press(void) { nz_confirm("Restart the console?", reboot_answer, NULL); }
static void off_press(void) { nz_confirm("Turn the console off?", off_answer, NULL); }

/* Bluetooth (bt.c runs nzeus-bt in the background; the pages are further down) */
static void bt_toast(const char *m) { snprintf(bt_msg, sizeof bt_msg, "%.99s", m); ui_toast(m); }
static void bt_onoff_change(int dir)
{
    (void)dir;
    int st = bt_state();
    if (st == BT_STARTING || st == BT_STOPPING) bt_toast("Please wait a moment...");
    else if (st == BT_MISSING) bt_toast("The Bluetooth add-on is not on the SD card");
    else if (st == BT_ON) { if (bt_turn_off() == 0) bt_toast("Switching Bluetooth off..."); }
    else if (bt_turn_on() == 0) bt_toast("Starting Bluetooth... (about 10 seconds)");
}
static void bt_onoff_value(char *b, size_t n)
{
    int st = bt_state();
    snprintf(b, n, "%s", st == BT_ON ? "ON" : st == BT_STARTING ? "starting..." : st == BT_STOPPING ? "stopping..." : "off");
}
static void bt_status_value(char *b, size_t n)
{
    int count = bt_device_count(), conn = 0;
    for (int i = 0; i < count; i++) conn += bt_device(i)->connected;
    switch (bt_state()) {
    case BT_CHECKING:  snprintf(b, n, "checking..."); break;
    case BT_MISSING:   snprintf(b, n, "not installed"); break;
    case BT_NO_DONGLE: snprintf(b, n, "no dongle"); break;
    case BT_STARTING:  snprintf(b, n, "starting..."); break;
    case BT_STOPPING:  snprintf(b, n, "switching off..."); break;
    case BT_ON:        snprintf(b, n, "%d device%s, %d connected", count, count == 1 ? "" : "s", conn); break;
    default:           snprintf(b, n, "off"); break;
    }
}
static void bt_open_page(int page);

/* USB (usbmode.c runs nzeus-usbmode.sh in the background) */
static void usb_host_answer(int yes, void *ctx)
{
    (void)ctx;
    if (yes && usbmode_switch(0) == 0) bt_toast("Switching to Devices...");
}
static void usb_change(int dir)
{
    (void)dir;
    int st = usbmode_state();
    if (st == USBM_SWITCHING || st == USBM_CHECKING) bt_toast("Please wait a moment...");
    else if (st == USBM_MISSING) bt_toast("The USB add-on is not on the SD card");
    else if (st == USBM_PC)
        nz_confirm("Is the PC cable UNPLUGGED? Devices mode puts power on the port: never connect the PC in Devices mode.",
                   usb_host_answer, NULL);
    else if (usbmode_switch(st == USBM_HOST) == 0)
        bt_toast(st == USBM_HOST ? "Switching to PC connection..." : "Switching to Devices...");
}
static void usb_value(char *b, size_t n)
{
    int st = usbmode_state();
    snprintf(b, n, "%s", st == USBM_PC ? "PC connection" : st == USBM_HOST ? "Devices" :
                         st == USBM_SWITCHING ? "switching..." : st == USBM_MISSING ? "not installed" : "checking...");
}
static void usb_status_value(char *b, size_t n) { snprintf(b, n, "%s", usbmode_line()); }
static void bt_mine_press(void) { bt_open_page(PAGE_MINE); }
static void bt_find_press(void) { bt_open_page(PAGE_FIND); }

/* Compressed memory (zram): squeezes memory that isn't being used, so more
 * fits in the console's small RAM. ON unless nzeus/zram-off exists: the SD
 * Debian switches zram on at every power-on (/etc/init.d/nzeus-zram), and
 * nzeus-deck switches it off again when it starts NZEUS while the flag is
 * there (ports/r36s-hichip/rootfs/debian/overlay/usr/local/bin/nzeus-deck). */
const char *zram_off_file = NZ_DIR "/zram-off";                    /* (tests use fake paths) */
const char *zram_swaps_file = "/proc/swaps";
const char *zram_stat_file = "/sys/block/zram0/mm_stat";
static int zram_running(void)
{
    char line[160];
    int on = 0;
    FILE *f = fopen(zram_swaps_file, "r");
    if (!f) return 0;
    while (!on && fgets(line, sizeof line, f)) on = !strncmp(line, "/dev/zram0 ", 11);
    fclose(f);
    return on;
}
static void zram_change(int dir) { (void)dir; flag_set(zram_off_file, !flag_on(zram_off_file)); }
static void zram_value(char *b, size_t n) { snprintf(b, n, "%s", flag_on(zram_off_file) ? "off" : "ON"); }
static void zram_note(char *b, size_t n)
{
    double stored = 0, ram = 0, compr = 0;
    if (flag_on(zram_off_file)) { snprintf(b, n, "Squeezes memory so more fits in RAM"); return; }
    if (!zram_running()) { snprintf(b, n, "Starts at the next power-on"); return; }
    FILE *f = fopen(zram_stat_file, "r");          /* mm_stat: stored, compressed, RAM used, ... (bytes) */
    if (!f || fscanf(f, "%lf %lf %lf", &stored, &compr, &ram) != 3) { snprintf(b, n, "On"); if (f) fclose(f); return; }
    fclose(f);
    snprintf(b, n, "On: %.1f MB squeezed into %.1f MB of RAM", stored / 1048576, ram / 1048576);
}

static const Row rows_appearance[] = {
    { ROW_VALUE, "Theme", theme_change, theme_value, NULL },
};
static const Row rows_sound[] = {
    { ROW_VALUE, "Volume", volume_change, volume_value, NULL },
    { ROW_VALUE, "Speaker", speaker_change, speaker_value, NULL },
};
static const Row rows_display[] = {
    { ROW_VALUE, "Brightness", bright_change, bright_value, NULL },
};
static const Row rows_time[] = {
    { ROW_TEXT,  "Now", NULL, now_value, NULL },
    { ROW_VALUE, "Day", day_change, day_value, NULL },
    { ROW_VALUE, "Month", month_change, month_value, NULL },
    { ROW_VALUE, "Year", year_change, year_value, NULL },
    { ROW_VALUE, "Hour", hour_change, hour_value, NULL },
    { ROW_VALUE, "Minute", min_change, min_value, NULL },
    { ROW_VALUE, "Time zone", tz_change, tz_value, NULL },
};
static const Row rows_controls[] = {
    { ROW_VALUE, "Button repeat", repeat_change, repeat_value, NULL },
    { ROW_TEXT,  "Switch look", NULL, keys_value, NULL },
    { ROW_TEXT,  "Screenshot", NULL, shot_keys_value, NULL },
    { ROW_VALUE, "Vol- / serial pin", voldown_change, voldown_value, NULL },
    { ROW_VALUE, "Confirm button", accept_change, accept_value, NULL },
};
static const Row rows_storage[] = {
    { ROW_TEXT, "SD card (Linux data)", NULL, sd_value, NULL },
    { ROW_TEXT, "Memory", NULL, ram_value, NULL },
    { ROW_TEXT, "Card health", NULL, health_value, NULL },
    { ROW_VALUE, "Compressed memory (zram)", zram_change, zram_value, NULL },
    { ROW_TEXT,  "", NULL, zram_note, NULL },                     /* (what it does / how it is doing: the whole row) */
};
static const Row rows_startup[] = {
    { ROW_VALUE,  "Start in", start_change, start_value, NULL },
};
static const Row rows_claude[] = {
    { ROW_TEXT, "SSH server", NULL, ssh_value, NULL },
    { ROW_TEXT, "", NULL, ssh_where_value, NULL },
    { ROW_TEXT, "Allowed keys", NULL, ssh_keys_value, NULL },
};
static const Row rows_wifi[] = {
    { ROW_BUTTON, "Wi-Fi networks", NULL, NULL, wifi_open_press },
    { ROW_TEXT,   "Status", NULL, wifi_status_value, NULL },
    { ROW_VALUE,  "Join saved networks by itself", wifi_auto_change, wifi_auto_value, NULL },
    { ROW_TEXT,   "Saved networks", NULL, wifi_saved_value, NULL },
};
static const Row rows_about[] = {
    { ROW_TEXT, "Version", NULL, ver_value, NULL },
    { ROW_TEXT, "Kernel", NULL, kernel_value, NULL },
    { ROW_TEXT, "Processor", NULL, cpu_value, NULL },
    { ROW_TEXT, "Built with", NULL, credits_value, NULL },
};
static const Row rows_power[] = {
    { ROW_BUTTON, "Restart NZEUS Deck", NULL, NULL, restart_press },
    { ROW_BUTTON, "Restart the console", NULL, NULL, reboot_press },
    { ROW_BUTTON, "Power off", NULL, NULL, off_press },
};
static const Row rows_bt[] = {
    { ROW_VALUE,  "Bluetooth", bt_onoff_change, bt_onoff_value, NULL },
    { ROW_TEXT,   "Status", NULL, bt_status_value, NULL },
    { ROW_BUTTON, "My devices", NULL, NULL, bt_mine_press },
    { ROW_BUTTON, "Find devices...", NULL, NULL, bt_find_press },
};
static const Row rows_usb[] = {
    { ROW_VALUE, "USB mode", usb_change, usb_value, NULL },
    { ROW_TEXT,  "", NULL, usb_status_value, NULL },              /* (the status line: the whole row) */
};
static const struct { const Row *rows; int count; } sections[SEC_COUNT] = {
#define ROWS(a) { a, (int)(sizeof a / sizeof a[0]) }
    ROWS(rows_appearance), ROWS(rows_sound), ROWS(rows_display), ROWS(rows_time),
    ROWS(rows_controls), ROWS(rows_storage), ROWS(rows_startup), ROWS(rows_wifi), ROWS(rows_bt), ROWS(rows_usb),
    ROWS(rows_claude), ROWS(rows_about), ROWS(rows_power),
#undef ROWS
};

/* ---------- drawing ---------- */
/* change a label only when its text is different (no needless redraw) */
static void set_text(lv_obj_t *l, const char *text)
{
    if (l && strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void show_values(void)
{
    char buf[96];
    for (int i = 0; i < S.row_count; i++) {
        const Row *r = &S.row_defs[i];
        if (!r->value) continue;
        r->value(buf, sizeof buf);
        if (r->type == ROW_VALUE) {
            char v[112];
            snprintf(v, sizeof v, LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT, buf);
            set_text(S.vals[i], v);
        } else {
            set_text(S.vals[i], buf);
        }
    }
}

static void show_selection(void)
{
    for (int i = 0; i < SEC_COUNT; i++) {
        if (i == S.sec) {
            lv_obj_add_state(S.secs[i], LV_STATE_CHECKED);
            lv_obj_scroll_to_view(S.secs[i], LV_ANIM_OFF);   /* (13 sections: the list may scroll) */
        } else lv_obj_clear_state(S.secs[i], LV_STATE_CHECKED);
    }
    for (int i = 0; i < S.row_count; i++) {
        if (S.in_panel && i == S.row) {
            lv_obj_add_state(S.rows[i], LV_STATE_CHECKED);
            lv_obj_scroll_to_view(S.rows[i], LV_ANIM_OFF);   /* (a small window shows only some rows) */
        } else lv_obj_clear_state(S.rows[i], LV_STATE_CHECKED);
    }
}

/* The theme cards: one small picture of each theme, made from its own colours
 * (not the shared styles, which always show the theme in use). */
static void build_theme_cards(lv_obj_t *parent)
{
    lv_obj_t *wrapbox = th_box(parent, -1);
    lv_obj_set_size(wrapbox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrapbox, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(wrapbox, 8, 0);
    lv_obj_set_style_pad_top(wrapbox, 10, 0);
    for (int i = 0; i < TH_COUNT; i++) {
        const NzTheme *t = theme_get(i);
        lv_obj_t *c = lv_obj_create(wrapbox);
        lv_obj_remove_style_all(c);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(c, 120, 74);
        lv_obj_set_style_bg_color(c, th_color(t->bg), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, t->radius ? 8 : 0, 0);
        lv_obj_set_style_border_width(c, i == theme_index() ? 3 : 1, 0);
        lv_obj_set_style_border_color(c, th_color(i == theme_index() ? theme()->accent : t->border), 0);
        lv_obj_t *bar = lv_obj_create(c);                     /* a tiny status bar */
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), 12);
        lv_obj_set_style_bg_color(bar, th_color(t->bar), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        for (int k = 0; k < 3; k++) {                          /* three tiny tiles */
            lv_obj_t *tile = lv_obj_create(c);
            lv_obj_remove_style_all(tile);
            lv_obj_set_size(tile, 28, 22);
            lv_obj_set_pos(tile, 8 + k * 36, 18);
            lv_obj_set_style_bg_color(tile, th_color(t->surface), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(tile, t->radius ? 4 : 0, 0);
            lv_obj_set_style_border_width(tile, k == 0 ? 2 : 0, 0);
            lv_obj_set_style_border_color(tile, th_color(t->accent), 0);
        }
        lv_obj_t *nm = lv_label_create(c);
        lv_label_set_text(nm, t->name);
        lv_obj_set_style_text_font(nm, t->font_small, 0);
        lv_obj_set_style_text_color(nm, th_color(t->text), 0);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -6);
        if (t->retro) {               /* Retro Blue: sky, a green hill, the blue taskbar + green start */
            lv_obj_set_style_bg_color(c, lv_color_hex(0x2A68D4), 0);
            lv_obj_set_style_bg_grad_color(c, lv_color_hex(0xA3CDF2), 0);
            lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
            lv_obj_t *hill = lv_obj_create(c);
            lv_obj_remove_style_all(hill);
            lv_obj_set_size(hill, 150, 60);
            lv_obj_set_pos(hill, -15, 40);
            lv_obj_set_style_radius(hill, 40, 0);
            lv_obj_set_style_bg_color(hill, lv_color_hex(0x56AC34), 0);
            lv_obj_set_style_bg_grad_color(hill, lv_color_hex(0x2F7A1F), 0);
            lv_obj_set_style_bg_grad_dir(hill, LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_bg_opa(hill, LV_OPA_COVER, 0);
            lv_obj_move_to_index(hill, 1);                     /* (behind the name) */
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x245EDB), 0);
            lv_obj_set_height(bar, 10);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_t *go = lv_obj_create(bar);                 /* the little green start button */
            lv_obj_remove_style_all(go);
            lv_obj_set_size(go, 26, 10);
            lv_obj_set_style_radius(go, 5, 0);
            lv_obj_set_style_bg_color(go, lv_color_hex(0x3FA83F), 0);
            lv_obj_set_style_bg_opa(go, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(nm, lv_color_white(), 0);
            lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -13);
        }
    }
}

static void row_clicked(lv_event_t *e);
static void arrow_clicked(lv_event_t *e);
static void bt_clear_page(void);
static void bt_add_hints(const char *const pairs[], int count);

static void build_panel(void)
{
    bt_clear_page();
    lv_obj_clean(S.panel);
    S.row_defs = sections[S.sec].rows;
    S.row_count = sections[S.sec].count;
    if (S.row >= S.row_count) S.row = 0;
    lv_obj_t *h = th_label(S.panel, sec_names[S.sec], TS_HEADING);
    lv_obj_set_style_pad_bottom(h, 8, 0);
    for (int i = 0; i < S.row_count; i++) {
        const Row *r = &S.row_defs[i];
        lv_obj_t *row = S.rows[i] = th_box(S.panel, TS_TEXT);
        th_add_to(row, TS_ROW_SEL, LV_STATE_CHECKED);
        lv_obj_set_size(row, LV_PCT(100), 38);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, r->label);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        if (r->type == ROW_BUTTON) {
            lv_label_set_text_fmt(l, "%s  " LV_SYMBOL_RIGHT, r->label);
            S.vals[i] = NULL;
            continue;
        }
        S.vals[i] = lv_label_create(row);               /* the value (text colour from the row) */
        if (r->label[0]) lv_obj_align(S.vals[i], LV_ALIGN_RIGHT_MID, -10, 0);
        else lv_obj_align(S.vals[i], LV_ALIGN_LEFT_MID, 10, 0);   /* no label: a long line has the whole row */
        if (r->type == ROW_VALUE) {
            lv_obj_add_flag(S.vals[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(S.vals[i], arrow_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
    if (S.sec == SEC_APPEARANCE) build_theme_cards(S.panel);
    if (S.sec == SEC_TIME || S.sec == SEC_CLAUDE) {    /* the time / the remote warning */
        lv_obj_t *note = th_label(S.panel, S.sec == SEC_TIME ?
                                  "The console has no clock battery: after switching off, the time "
                                  "continues from where it stopped. Set it here." :
                                  "Only keys listed in /root/.ssh/authorized_keys can log in (no passwords). "
                                  "From the PC: ssh nzeus-deck over the USB cable.", TS_SMALL);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, LV_PCT(100));
        lv_obj_set_style_pad_top(note, 6, 0);
        if (S.sec == SEC_CLAUDE) S.extra = note;       /* (so tests can read it) */
    }
    if (S.sec == SEC_BLUETOOTH || S.sec == SEC_USB) {   /* a line of help under the rows, then the hints */
        S.extra = th_label(S.panel, "", TS_SMALL);
        lv_label_set_long_mode(S.extra, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(S.extra, LV_PCT(100));
        lv_obj_set_style_pad_top(S.extra, 8, 0);
        static const char *const hints[] = { "A", "Choose", LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "On/off", "B", "Back" };
        static const char *const usb_hints[] = { LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "Switch mode", "B", "Back" };
        if (S.sec == SEC_USB) bt_add_hints(usb_hints, 2);
        else bt_add_hints(hints, 3);
    }
    show_values();
    show_selection();
}

/* ---------- actions ---------- */
static void change(int dir)
{
    const Row *r = &S.row_defs[S.row];
    if (r->type != ROW_VALUE || !r->change) return;
    r->change(dir);
    if (S.sec == SEC_APPEARANCE) build_panel();       /* the cards show the new choice */
    else show_values();
}

static void press(void)
{
    const Row *r = &S.row_defs[S.row];
    if (r->type == ROW_BUTTON && r->press) r->press();
    else if (r->type == ROW_VALUE) change(+1);
}

static void open_section(int sec)
{
    S.sec = sec;
    S.row = 0;
    build_panel();
    if (sec == SEC_BLUETOOTH) bt_refresh();         /* ask for the state now, not in 3 s */
    if (sec == SEC_USB) usbmode_refresh();
}

static void sec_clicked(lv_event_t *e)
{
    S.in_panel = 0;
    open_section((int)(intptr_t)lv_event_get_user_data(e));
}

static void row_clicked(lv_event_t *e)
{
    S.in_panel = 1;
    S.row = (int)(intptr_t)lv_event_get_user_data(e);
    show_selection();
    if (S.row_defs[S.row].type == ROW_BUTTON) press();
}

/* clicking the value: left half = smaller, right half = bigger */
static void arrow_clicked(lv_event_t *e)
{
    lv_obj_t *v = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_area_t a;
    lv_obj_get_coords(v, &a);
    S.in_panel = 1;
    S.row = (int)(intptr_t)lv_event_get_user_data(e);
    change(p.x < (a.x1 + a.x2) / 2 ? -1 : +1);
}

static void refresh_timer(lv_timer_t *t)              /* clock, memory, Wi-Fi status */
{
    (void)t;
    if (S.sec == SEC_WIFI) wifi_poll();              /* (asks the Wi-Fi status in the background) */
    show_values();
}

/* ---------- Bluetooth pages: My devices, Find devices, Pairing ---------- */
#define BT_SCAN_SECONDS 10

/* the hint row at the bottom of the panel (a stretchy gap pushes it down) */
static void bt_add_hints(const char *const pairs[], int count)
{
    lv_obj_t *gap = th_box(S.panel, -1);
    lv_obj_set_size(gap, LV_PCT(100), 0);
    lv_obj_set_flex_grow(gap, 1);
    lv_obj_t *h = nz_hints(S.panel, pairs, count);
    S.hint_word = lv_obj_get_child(h, 1);            /* the first "what it does" word */
}

/* forget the objects of a Bluetooth page (they are about to be deleted) */
static void bt_clear_page(void)
{
    if (S.page == PAGE_PAIR && bt_pairing()) bt_pair_cancel();   /* leaving: stop pairing */
    if (S.list) nzlist_free(S.list);
    S.list = NULL;
    S.extra = S.big = S.info = S.bar = S.hint_word = NULL;
    S.page = PAGE_ROWS;
}

static const char *signal_word(int dbm)
{
    return dbm == 0 ? "" : dbm > -55 ? "strong" : dbm > -70 ? "good" : dbm > -80 ? "weak" : "very weak";
}
static int kind_icon(const char *kind)
{
    if (strstr(kind, "keyboard")) return ICON_KEYBOARD;
    if (strstr(kind, "mouse") || strstr(kind, "tablet") || !strcmp(kind, "computer")) return ICON_DESKTOP;
    if (strstr(kind, "pad") || strstr(kind, "joystick") || !strcmp(kind, "phone")) return ICON_HANDHELD;
    if (!strcmp(kind, "audio")) return ICON_MUSIC;
    return ICON_ABOUT;
}
/* "keyboard+mouse" -> "Keyboard + mouse", "mouse" -> "Mouse" */
static void kind_words(const char *kind, char *b, size_t n)
{
    if (!strcmp(kind, "keyboard+mouse")) snprintf(b, n, "Keyboard + mouse");
    else snprintf(b, n, "%s", kind);
    b[0] = (char)(b[0] >= 'a' && b[0] <= 'z' ? b[0] - 32 : b[0]);
}

static void bt_fill_mine(void)
{
    int sel = nzlist_selected(S.list);
    nzlist_clear(S.list);
    for (int i = 0; i < bt_device_count(); i++) {
        const BtDev *d = bt_device(i);
        char kind[24], sub[64];
        kind_words(d->kind, kind, sizeof kind);
        snprintf(sub, sizeof sub, "%s  -  %s", kind, d->connected ? "Connected" : "Not connected");
        nzlist_add(S.list, kind_icon(d->kind), d->name, sub, strcmp(d->type, "le") ? NULL : "LE");
    }
    if (!bt_device_count())
        nzlist_add_hint(S.list, ICON_ABOUT, "No devices yet", "Press {accept} to find a keyboard or mouse", NULL);
    if (sel > 0) nzlist_select(S.list, sel);
    S.list_ver = bt_list_version();
}

static void bt_fill_find(void)
{
    int sel = nzlist_selected(S.list);
    nzlist_clear(S.list);
    nzlist_add(S.list, ICON_WIFI, "Search again", "Put the device in pairing mode first",
               bt_scanning() ? "searching..." : NULL);
    for (int i = 0; i < bt_found_count(); i++) {
        const BtDev *d = bt_found(i);
        char kind[24], sub[64];
        kind_words(d->kind, kind, sizeof kind);
        if (!d->supported) snprintf(sub, sizeof sub, "not a keyboard/mouse (%s)", d->kind);
        else snprintf(sub, sizeof sub, "%s%s", kind, strcmp(d->type, "le") ? "" : "  -  LE, experimental");
        int r = nzlist_add(S.list, kind_icon(d->kind), d->name, sub, signal_word(d->rssi));
        if (!d->supported && r >= 0) {               /* greyed out: muted text, faded icon */
            lv_obj_t *row = lv_obj_get_child(nzlist_obj(S.list), r);
            th_add(row, TS_MUTED);
            lv_obj_set_style_img_opa(lv_obj_get_child(row, 0), LV_OPA_50, 0);
        }
    }
    if (sel > 0) nzlist_select(S.list, sel);
    S.scan_ver = bt_scan_version();
}

static void bt_start_search(void)
{
    if (bt_state() != BT_ON) { bt_toast("Switch Bluetooth on first"); return; }
    if (bt_scan_start(BT_SCAN_SECONDS) != 0) { bt_toast("Could not start the search"); return; }
    bt_fill_find();
}

/* ---- My devices: A -> Connect / Disconnect / Forget ---- */
static void bt_forget_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!yes || !S.panel || S.page != PAGE_MINE) return;
    if (bt_command("remove", S.dev_addr) == 0) bt_toast("Forgetting...");
    else bt_toast("Busy - try again in a moment");
}

static void bt_menu_done(int choice, void *ctx)
{
    (void)ctx;
    if (choice < 0 || !S.panel || S.page != PAGE_MINE) return;
    if (choice == 1) {
        char q[80];
        snprintf(q, sizeof q, "Forget %s?", S.dev_name);
        nz_confirm(q, bt_forget_answer, NULL);
    } else if (bt_command(S.dev_connected ? "disconnect" : "connect", S.dev_addr) == 0) {
        bt_toast(S.dev_connected ? "Disconnecting..." : "Connecting... (wake the device up)");
    } else bt_toast("Busy - try again in a moment");
}

static void bt_mine_activate(int i, void *ctx)
{
    (void)ctx;
    const BtDev *d = bt_device(i);
    if (!d) { bt_open_page(PAGE_FIND); return; }    /* the "No devices yet" row */
    snprintf(S.dev_addr, sizeof S.dev_addr, "%s", d->addr);
    snprintf(S.dev_name, sizeof S.dev_name, "%s", d->name);
    S.dev_connected = d->connected;
    const char *items[] = { d->connected ? "Disconnect" : "Connect", "Forget" };
    nz_menu(d->name, items, 2, bt_menu_done, NULL);
}

/* ---- Find devices: A on a keyboard/mouse -> pair ---- */
static void bt_find_activate(int i, void *ctx)
{
    (void)ctx;
    if (i == 0) { bt_start_search(); return; }
    const BtDev *d = bt_found(i - 1);
    if (!d) return;
    if (bt_scanning()) { bt_toast("Still searching - wait a moment"); return; }
    if (!d->supported) { bt_toast("That is not a keyboard or mouse"); return; }
    snprintf(S.dev_addr, sizeof S.dev_addr, "%s", d->addr);
    snprintf(S.dev_name, sizeof S.dev_name, "%s", d->name);
    if (bt_pair(d->addr) != 0) { bt_toast("Could not start pairing"); return; }
    bt_open_page(PAGE_PAIR);
}

/* ---- Pairing: the number typed for "enter-passkey" ---- */
static void bt_number_done(const char *text, void *ctx)
{
    (void)ctx;
    if (!S.panel || S.page != PAGE_PAIR) return;
    if (!text) {                                     /* B: cancel pairing */
        bt_pair_cancel();
        bt_toast("Pairing cancelled");
        bt_open_page(PAGE_FIND);
        return;
    }
    size_t len = strlen(text);
    if (len < 1 || len > 6 || strspn(text, "0123456789") != len) {
        bt_toast("Only the digits, please (up to 6)");
        nz_prompt("Type the number the device shows", "", bt_number_done, NULL);
        return;
    }
    if (bt_pair_answer(atol(text)) == 0) set_text(S.info, "Checking the number...");
}

/* the big passkey digits ("" = hide them) */
static void set_big(const char *digits)
{
    set_text(S.big, digits);
    if (digits[0]) lv_obj_clear_flag(S.big, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.big, LV_OBJ_FLAG_HIDDEN);
}

static void bt_pair_events(void)
{
    char text[120], line[64];
    int ev;
    while ((ev = bt_pair_next(text, sizeof text)) != BT_EV_NONE) {
        switch (ev) {
        case BT_EV_PASSKEY:
            set_big(text);
            set_text(S.info, "Type this on the keyboard, then press Enter");
            set_text(S.extra, "");
            break;
        case BT_EV_TYPED:
            snprintf(line, sizeof line, "%.20s typed so far", text);
            set_text(S.extra, line);
            break;
        case BT_EV_ENTER:
            set_big("");
            set_text(S.info, "Type the number the device shows");
            nz_prompt("Type the number the device shows", "", bt_number_done, NULL);
            break;
        case BT_EV_INFO:
            set_text(S.extra, text);
            break;
        case BT_EV_PAIRED:
            set_text(S.info, "Paired. Connecting...");
            break;
        case BT_EV_DONE:
            bt_toast(strcmp(text, "Connected") ? "Paired - connecting..." : "Connected - try typing!");
            bt_refresh();
            bt_open_page(PAGE_MINE);
            return;
        case BT_EV_ERROR:
            set_big("");
            set_text(S.info, "Pairing did not work");
            set_text(S.extra, text);
            set_text(S.hint_word, "Back");
            break;
        }
    }
}

static void bt_back(void)
{
    if (S.page == PAGE_PAIR) {
        if (bt_pairing()) { bt_pair_cancel(); bt_toast("Pairing cancelled"); }
        bt_open_page(PAGE_FIND);
        return;
    }
    int from = S.page;
    build_panel();                                   /* back to the Bluetooth rows */
    S.row = from == PAGE_MINE ? 2 : 3;
    show_selection();
}
static void bt_back_clicked(lv_event_t *e) { (void)e; bt_back(); }

static void bt_open_page(int page)
{
    char title[64];
    int from = S.page;
    bt_clear_page();
    lv_obj_clean(S.panel);
    S.page = page;
    S.row_count = 0;                                 /* (the rows are gone) */
    S.in_panel = 1;
    snprintf(title, sizeof title, LV_SYMBOL_LEFT "  %s",
             page == PAGE_PAIR ? "Pairing" : page == PAGE_MINE ? "My devices" : "Find devices");
    lv_obj_t *h = th_label(S.panel, title, TS_HEADING);       /* click the title = back */
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h, bt_back_clicked, LV_EVENT_CLICKED, NULL);

    if (page == PAGE_PAIR) {
        lv_obj_t *box = th_box(S.panel, TS_PANEL);  /* the card: name, passkey, what to do */
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(box, 14, 0);
        lv_obj_set_style_pad_row(box, 10, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *name = th_label(box, S.dev_name, TS_TITLE);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(name, LV_PCT(100), 0);
        S.big = th_label(box, "", TS_BIG);          /* (hidden while it has no digits) */
        lv_obj_set_style_text_letter_space(S.big, 6, 0);
        lv_obj_add_flag(S.big, LV_OBJ_FLAG_HIDDEN);
        S.info = th_label(box, "Waiting for the device...", TS_TEXT);
        S.extra = th_label(box, "", TS_SMALL);
        for (int i = 0; i < 2; i++) {
            lv_obj_t *l = i ? S.extra : S.info;
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, LV_PCT(100));
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        }
        static const char *const hints[] = { "B", "Cancel" };
        bt_add_hints(hints, 1);
        return;
    }
    S.extra = th_label(S.panel, "", TS_SMALL);
    lv_label_set_long_mode(S.extra, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(S.extra, LV_PCT(100));
    if (page == PAGE_FIND) {                         /* the search's progress */
        S.bar = lv_bar_create(S.panel);
        lv_obj_set_size(S.bar, LV_PCT(100), 6);
        lv_bar_set_range(S.bar, 0, BT_SCAN_SECONDS * 1000);
        lv_obj_set_style_radius(S.bar, theme()->radius ? LV_RADIUS_CIRCLE : 0, 0);
        lv_obj_set_style_radius(S.bar, theme()->radius ? LV_RADIUS_CIRCLE : 0, LV_PART_INDICATOR);
    }
    S.list = nzlist_create(S.panel, 0);
    lv_obj_set_width(nzlist_obj(S.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(S.list), 1);
    lv_obj_set_style_pad_hor(nzlist_obj(S.list), 0, 0);
    static const char *const hints_mine[] = { "A", "Choose", "B", "Back" };
    static const char *const hints_find[] = { "A", "Pair", "B", "Back" };
    nz_hints(S.panel, page == PAGE_MINE ? hints_mine : hints_find, 2);
    if (page == PAGE_MINE) {
        nzlist_on_activate(S.list, bt_mine_activate, NULL);
        bt_fill_mine();
        bt_refresh();
    } else {
        nzlist_on_activate(S.list, bt_find_activate, NULL);
        bt_fill_find();
        if (from != PAGE_PAIR && !bt_scanning()) bt_start_search();   /* opening the page starts a search */
    }
}

/* the USB section's help: what the mode means, what to do (+ the last message) */
static void usb_tick(void)
{
    char msg[400];
    usbmode_poll();
    show_values();
    int st = usbmode_state();
    snprintf(msg, sizeof msg, "%s%s%s", usbmode_note(), usbmode_note()[0] ? "\n" : "",
             st == USBM_PC ? "On the PC: ssh nzeus-deck (USB network) or the serial port. "
                             "For a Wi-Fi/Bluetooth dongle, keyboard or stick: UNPLUG the PC cable, then choose Devices."
           : st == USBM_MISSING ? "The USB files are missing (/usr/local/lib/nzeus/deck-bin)."
           : "Devices: plug in the OTG adapter. NEVER plug the PC in now. Back to the PC: unplug all USB devices, "
             "choose PC connection, then connect the cable. Every power-on starts in PC mode.");
    set_text(S.extra, msg);
}

/* every 250 ms while the Bluetooth (or USB) section is shown */
static void bt_timer(lv_timer_t *t)
{
    (void)t;
    char msg[120];
    if (S.sec == SEC_USB && S.panel && S.page == PAGE_ROWS) { usb_tick(); return; }
    if (S.sec != SEC_BLUETOOTH || !S.panel) return;
    bt_poll();
    if (bt_command_result(msg, sizeof msg)) bt_toast(msg);
    int st = bt_state();
    switch (S.page) {
    case PAGE_ROWS:
        show_values();
        if (st == BT_STARTING) snprintf(msg, sizeof msg, "Starting Bluetooth...  %.80s", bt_start_line());
        else if (bt_problem()[0]) snprintf(msg, sizeof msg, "%s", bt_problem());
        else if (st == BT_NO_DONGLE) snprintf(msg, sizeof msg, "Plug the Bluetooth dongle into the USB port, then switch on again.");
        else if (st == BT_MISSING) snprintf(msg, sizeof msg, "The Bluetooth files are missing from nzeus/bin on the SD card.");
        else if (st == BT_ON) snprintf(msg, sizeof msg, "For keyboards and mice. LE (Bluetooth Smart) devices: experimental.");
        else snprintf(msg, sizeof msg, "Switch on to use a Bluetooth keyboard or mouse (starting takes about 10 seconds).");
        set_text(S.extra, msg);
        break;
    case PAGE_MINE:
        if (S.list_ver != bt_list_version()) bt_fill_mine();
        set_text(S.extra, bt_command_busy() ? "Working..." : st != BT_ON ? "Bluetooth is off: switch it on to connect." : "");
        break;
    case PAGE_FIND:
        if (S.scan_ver != bt_scan_version()) bt_fill_find();
        if (bt_scanning()) {
            int ms = bt_scan_elapsed_ms();
            lv_obj_clear_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(S.bar, ms, LV_ANIM_OFF);
            snprintf(msg, sizeof msg, "Searching... %d of %d s  (put your keyboard in pairing mode)",
                     ms / 1000 < BT_SCAN_SECONDS ? ms / 1000 : BT_SCAN_SECONDS, BT_SCAN_SECONDS);
        } else {
            lv_obj_add_flag(S.bar, LV_OBJ_FLAG_HIDDEN);    /* the bar only while searching */
            if (bt_problem()[0]) snprintf(msg, sizeof msg, "%s", bt_problem());
            else if (!bt_found_count()) snprintf(msg, sizeof msg, "Nothing found yet. Is the device in pairing mode?");
            else snprintf(msg, sizeof msg, "Found %d. Grey ones are not keyboards or mice.", bt_found_count());
        }
        set_text(S.extra, msg);
        break;
    case PAGE_PAIR:
        bt_pair_events();
        break;
    }
}

static int bt_page_key(int k)
{
    if (k == NZ_KEY_B) bt_back();
    else if (S.list) nzlist_key(S.list, k);
    return 1;                                        /* a page keeps every button */
}

/* ---------- the app ---------- */
static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&S, 0, sizeof S);
    th_add(root, TS_SCREEN);

    lv_obj_t *left = th_box(root, TS_PANEL);          /* the sections */
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_shadow_width(left, 0, 0);
    lv_obj_set_size(left, 190, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left, 6, 0);
    lv_obj_set_style_pad_row(left, 1, 0);
    lv_obj_add_flag(left, LV_OBJ_FLAG_SCROLLABLE);   /* (a desktop window is shorter: it scrolls) */
    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_t *b = S.secs[i] = th_box(left, TS_TEXT);
        th_add_to(b, TS_ROW_SEL, LV_STATE_CHECKED);
        lv_obj_set_size(b, LV_PCT(100), 32);           /* 13 sections fit on the 480-pixel screen */
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(b, 8, 0);
        lv_obj_set_style_pad_column(b, 10, 0);
        if (sec_icons[i] >= 0) {
            lv_obj_t *ic = lv_img_create(b);
            lv_img_set_src(ic, icon_get((NzIcon)sec_icons[i], 20));
            th_add(ic, TS_ICON);
        } else {                                      /* Bluetooth / USB: the font's sign */
            lv_obj_t *ic = th_label(b, sec_icons[i] == -1 ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_USB, TS_ICON);
            lv_obj_set_width(ic, 20);
            lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, sec_names[i]);
        lv_obj_add_event_cb(b, sec_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    S.panel = th_box(root, -1);                       /* the rows of the chosen section */
    lv_obj_set_size(S.panel, 640 - 190, LV_PCT(100));
    lv_obj_set_pos(S.panel, 190, 0);
    lv_obj_set_style_pad_all(S.panel, 14, 0);
    lv_obj_set_style_pad_row(S.panel, 4, 0);
    lv_obj_set_flex_flow(S.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(S.panel, LV_OBJ_FLAG_SCROLLABLE);

    open_section(SEC_APPEARANCE);
    S.timer = lv_timer_create(refresh_timer, 1000, NULL);
    S.bt_timer = lv_timer_create(bt_timer, 250, NULL);
}

/* the panel takes what the section list leaves (a window is narrower than the screen) */
static void tick(NzApp *a)
{
    lv_coord_t w = lv_obj_get_content_width(a->root) - 190;
    if (S.panel && w > 0 && lv_obj_get_width(S.panel) != w) lv_obj_set_width(S.panel, w);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (!S.in_panel) {
        switch (k) {
        case NZ_KEY_UP:    if (S.sec > 0) open_section(S.sec - 1); return 1;
        case NZ_KEY_DOWN:  if (S.sec < SEC_COUNT - 1) open_section(S.sec + 1); return 1;
        case NZ_KEY_A:
        case NZ_KEY_RIGHT: S.in_panel = 1; S.row = 0; show_selection(); return 1;
        default: return 0;                            /* B: leave Settings */
        }
    }
    if (S.page != PAGE_ROWS) return bt_page_key(k);   /* a Bluetooth page */
    switch (k) {
    case NZ_KEY_UP:    if (S.row > 0) S.row--; show_selection(); return 1;
    case NZ_KEY_DOWN:  if (S.row < S.row_count - 1) S.row++; show_selection(); return 1;
    case NZ_KEY_LEFT:  change(-1); return 1;
    case NZ_KEY_RIGHT: change(+1); return 1;
    case NZ_KEY_A:     press(); return 1;
    case NZ_KEY_B:     S.in_panel = 0; show_selection(); return 1;
    default:           return 0;
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    if (S.timer) lv_timer_del(S.timer);
    if (S.bt_timer) lv_timer_del(S.bt_timer);
    bt_clear_page();                                 /* (stops a pairing that is still running) */
    memset(&S, 0, sizeof S);
}

/* for tests */
int app_settings_section(void) { return S.sec; }
int app_settings_section_on_screen(int i)        /* is section i's row fully on the screen? (tests) */
{
    if (i < 0 || i >= SEC_COUNT || !S.secs[i]) return 0;
    lv_area_t a;
    lv_obj_update_layout(S.secs[i]);
    lv_obj_get_coords(S.secs[i], &a);
    return a.y1 >= 0 && a.y2 < 480;
}
int app_settings_in_panel(void) { return S.in_panel; }
int app_settings_bt_page(void) { return S.page; }
const char *app_settings_bt_message(void) { return bt_msg; }
const char *app_settings_bt_big(void) { return S.big ? lv_label_get_text(S.big) : ""; }
const char *app_settings_bt_note(void) { return S.extra ? lv_label_get_text(S.extra) : ""; }
const char *app_settings_row_value(int i) { return i >= 0 && i < S.row_count && S.vals[i] ? lv_label_get_text(S.vals[i]) : ""; }
int app_settings_bt_rows(void) { return S.list ? nzlist_count(S.list) : 0; }
int app_settings_bt_row_greyed(int i)              /* drawn in the theme's muted colour? */
{
    lv_obj_t *row = S.list && i < nzlist_count(S.list) ? lv_obj_get_child(nzlist_obj(S.list), i) : NULL;
    return row && lv_obj_get_style_text_color(row, LV_PART_MAIN).full == th_color(theme()->muted).full;
}

NzApp app_settings = {
    .id = APP_SETTINGS, .title = "Settings", .icon = ICON_SETTINGS,
    .create = create, .key = key, .tick = tick, .destroy = destroy,
};

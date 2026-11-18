/* app_wifi.c - the NZEUS Wi-Fi app.
 *
 * Top: what is there (dongle, driver, Wi-Fi on/off, connection).
 * List: "Turn Wi-Fi on" / "Scan for networks", then the networks found, then
 * the saved networks that were not seen in the scan.
 *   A on a network: join it (asks for the password the first time; a saved
 *                   network joins with its saved key, no questions). A small
 *                   spinner turns next to "Joining ..." until it worked or failed.
 *   Y on a saved network: forget it (its key is deleted)
 *   X on a network: its ID card (security in words, WPS, maker, same router,
 *                   best channel... from wifiinfo.c); any button closes it
 *   B back (to Settings, where this app is opened from)
 * Every network joined with a password is saved (nzeus/wifi/networks.conf);
 * when Devices mode comes on, the console joins a saved one by itself
 * (wifi-auto.sh, unless Settings > Wi-Fi says not to). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "icons.h"
#include "nzeus_tick.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "wifi.h"
#include "wifiinfo.h"

#define MAKER_LIST NZ_DIR "/lists/oui.txt"   /* the IEEE maker list (copied from the PC; optional) */

#define MAX_NETS 24
#define MAX_SAVED 16
#define MAX_ROWS (2 + MAX_NETS + MAX_SAVED)

static struct {
    lv_obj_t *status, *detail;
    NzList *list;
    WifiNet nets[MAX_NETS];
    int net_count, scanning, last_state, last_dongle, action_rows;
    char saved[MAX_SAVED][33];
    int saved_count;
    /* what each network row stands for (index = row - action_rows) */
    char row_ssid[MAX_ROWS][33];
    char row_secure[MAX_ROWS], row_saved[MAX_ROWS];
    uint32_t scan_started;
    char last_link[96];
    lv_timer_t *timer;
    lv_obj_t *card, *spinner;             /* spinner: turns while a join runs (NULL otherwise) */
    int joining;
    uint32_t join_started;
    lv_obj_t *info;                       /* a network's ID card, over everything (NULL = closed) */
    char info_text[1400];
} W;
static char join_ssid[33];
int wifi_scan_wait_ms = 25000;            /* at most: iw scans 2.4 + 5 GHz (~13 s), read when done */
int wifi_join_wait_ms = 30000;            /* no connection after this long: the join failed */

static const char *signal_word(int dbm) { return dbm > -55 ? "strong" : dbm > -70 ? "good" : dbm > -80 ? "weak" : "very weak"; }

/* the top card: what is there right now */
static void show_status(void)
{
    int dongle = wifi_dongle(), state = wifi_state();
    const char *link = wifi_link_status();
    char line1[96], line2[200];
    if (dongle == DONGLE_NONE)       snprintf(line1, sizeof line1, "No Wi-Fi dongle found");
    else if (dongle == DONGLE_CDROM) snprintf(line1, sizeof line1, "Dongle found (in its \"driver CD\" mode)");
    else snprintf(line1, sizeof line1, "%s", state == WIFI_ON ? "Wi-Fi is on" : state == WIFI_STARTING ? "Switching Wi-Fi on..." : "Wi-Fi is off");
    if (dongle == DONGLE_NONE)
        snprintf(line2, sizeof line2, "Plug the LB-Link dongle into the USB-C port (OTG adapter or hub).");
    else if (W.joining)
        snprintf(line2, sizeof line2, "Joining %s...", join_ssid);
    else if (state == WIFI_STARTING) {
        const char *log = wifi_job_log(), *last = log + strlen(log);
        while (last > log && (last[-1] == '\n')) last--;          /* the last line it printed */
        const char *start = last;
        while (start > log && start[-1] != '\n') start--;
        snprintf(line2, sizeof line2, "%.*s", (int)(last - start), start);
    } else if (!strncmp(link, "state=COMPLETED", 15) && wifi_ip()[0])
        snprintf(line2, sizeof line2, "Joined %s.  Internet on: %s", join_ssid[0] ? join_ssid : "the network", wifi_ip());
    else if (!strncmp(link, "state=COMPLETED", 15))
        snprintf(line2, sizeof line2, "Joined %s.  Starting the internet (getting an address)...", join_ssid[0] ? join_ssid : "the network");
    else if (state == WIFI_ON)
        snprintf(line2, sizeof line2, "Driver loaded.  Scan, then pick a network to join.");
    else
        snprintf(line2, sizeof line2, "%s", wifi_driver_loaded() ? "Driver loaded." : "Turn it on to load the driver.");
    lv_label_set_text(W.status, line1);
    lv_label_set_text(W.detail, line2);
    /* while joining, the text moves right to make room for the spinner */
    lv_obj_align(W.detail, LV_ALIGN_BOTTOM_LEFT, W.spinner ? 98 : 72, -10);
}

/* ---- joining: a small spinner next to "Joining <name>..." until it worked or failed ---- */
static void join_started(void)
{
    W.joining = 1;
    W.join_started = nzeus_tick_ms();
    if (!W.spinner) {
        W.spinner = lv_spinner_create(W.card, 1000, 60);      /* one turn a second, a 60 degree arc */
        lv_obj_remove_style_all(W.spinner);                  /* a plain ring in the theme's colours */
        th_add(W.spinner, TS_ARC);
        th_add_to(W.spinner, TS_ARC_IND, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(W.spinner, 3, 0);         /* thin: it is small */
        lv_obj_set_style_arc_width(W.spinner, 3, LV_PART_INDICATOR);
        lv_obj_clear_flag(W.spinner, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(W.spinner, 18, 18);
        lv_obj_align(W.spinner, LV_ALIGN_BOTTOM_LEFT, 72, -9);
    }
    show_status();
}

/* message: what went wrong (NULL = it worked: the card says "Joined ...") */
static void join_ended(const char *message)
{
    W.joining = 0;
    if (W.spinner) { lv_obj_del(W.spinner); W.spinner = NULL; }   /* (it costs nothing while it's gone) */
    if (message) ui_toast(message);
    show_status();
}

/* every refresh while joining: connected? failed? taking too long? */
static void check_join(void)
{
    char msg[128];
    if (!W.joining) return;
    int running = wifi_join_running();
    if (!running && !strncmp(wifi_link_status(), "state=COMPLETED", 15)) {
        join_ended(NULL);
    } else if (!running && wifi_join_failed()) {
        snprintf(msg, sizeof msg, "Could not join %s", join_ssid);
        join_ended(msg);
    } else if (nzeus_tick_ms() - W.join_started > (uint32_t)wifi_join_wait_ms) {
        snprintf(msg, sizeof msg, "Could not join %s (wrong password, or too far away?)", join_ssid);
        join_ended(msg);
    }
}

static void fill_list(void);

static void list_activate(int i, void *ctx);

static void start_scan(void)
{
    if (wifi_scan_start() != 0) { ui_toast("Scan failed: is Wi-Fi on?"); return; }
    W.scanning = 1;
    W.scan_started = nzeus_tick_ms();
    nzlist_set_right(W.list, 0, "scanning...");
}

static void password_done(const char *pw, void *ctx)
{
    (void)ctx;
    if (!pw) return;
    if (wifi_join(join_ssid, pw) == 0) join_started();
}

static int is_saved(const char *ssid)
{
    for (int k = 0; k < W.saved_count; k++) if (!strcmp(W.saved[k], ssid)) return 1;
    return 0;
}

static void list_activate(int i, void *ctx)
{
    (void)ctx;
    int state = wifi_state();
    if (i == 0) {                                     /* the action row */
        if (wifi_dongle() == DONGLE_NONE) { ui_toast("Plug in the Wi-Fi dongle first"); return; }
        if (state == WIFI_OFF) { wifi_turn_on(); ui_toast("Switching Wi-Fi on..."); }
        else if (state == WIFI_ON) start_scan();
        return;
    }
    if (i == 1 && W.action_rows == 2) { wifi_disconnect(); ui_toast("Disconnected"); fill_list(); return; }
    int r = i - W.action_rows;
    if (r < 0 || r >= MAX_ROWS || !W.row_ssid[r][0]) return;
    if (state != WIFI_ON) { ui_toast("Turn Wi-Fi on first"); return; }
    snprintf(join_ssid, sizeof join_ssid, "%s", W.row_ssid[r]);
    if (W.row_saved[r]) {                               /* saved: its key is on the card */
        if (wifi_join_saved(join_ssid) == 0) join_started();
    } else if (W.row_secure[r]) {
        char title[80];
        snprintf(title, sizeof title, "Password for %s", join_ssid);
        nz_prompt(title, "", password_done, NULL);
    } else if (wifi_join(join_ssid, "") == 0) join_started();
}

/* Y: forget the selected saved network */
static char forget_ssid[33];
static void forget_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!yes) return;
    if (wifi_forget(forget_ssid) == 0) ui_toast("Forgotten: its password is deleted");
    else ui_toast("Could not forget it (is the SD card read-only?)");
    fill_list();
}
static void forget_selected(void)
{
    int r = nzlist_selected(W.list) - W.action_rows;
    if (r < 0 || r >= MAX_ROWS || !W.row_ssid[r][0] || !W.row_saved[r]) { ui_toast("Pick a saved network to forget it"); return; }
    char q[80];
    snprintf(forget_ssid, sizeof forget_ssid, "%s", W.row_ssid[r]);
    snprintf(q, sizeof q, "Forget %s? (its password is deleted)", forget_ssid);
    nz_confirm(q, forget_answer, NULL);
}

/* X: the selected network's ID card */
static void close_info(void)
{
    if (W.info) { lv_obj_del(W.info); W.info = NULL; }
}

static void show_info(void)
{
    int r = nzlist_selected(W.list) - W.action_rows;
    if (r < 0 || r >= W.net_count || !W.row_ssid[r][0]) { ui_toast("Pick a network from the scan"); return; }
    WiNet *raw = malloc(32 * sizeof *raw);                   /* (borrowed only while the card is made) */
    if (!raw) return;
    int n = wi_parse(wifi_last_survey(), raw, 32), best = -1;
    const char *want = strcmp(W.row_ssid[r], "(hidden network)") ? W.row_ssid[r] : "";
    for (int i = 0; i < n; i++)                              /* its strongest row (one router: one row per band) */
        if (!strcmp(raw[i].ssid, want) && (best < 0 || raw[i].dbm > raw[best].dbm)) best = i;
    if (best < 0) { free(raw); ui_toast("Scan again to see its details"); return; }
    char why[96];
    size_t k;
    wi_card(raw, n, best, MAKER_LIST, W.info_text, sizeof W.info_text);
    int ch = wi_best_channel(raw, n, raw[best].ssid[0] ? raw[best].ssid : NULL, why, sizeof why);
    k = strlen(W.info_text);
    snprintf(W.info_text + k, sizeof W.info_text - k, "\n\nIf this is your router, its quietest 2.4 GHz channel\nis %d.  (%s)", ch, why);
    free(raw);

    W.info = lv_obj_create(lv_layer_top());                  /* over the whole screen */
    lv_obj_remove_style_all(W.info);
    th_add(W.info, TS_PANEL);
    lv_obj_set_size(W.info, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(W.info, 14, 0);
    lv_obj_set_flex_flow(W.info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(W.info, 8, 0);
    th_label(W.info, "Network ID card", TS_HEADING);
    lv_obj_t *t = th_label(W.info, W.info_text, TS_MONO);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    th_label(W.info, "Any button closes this card", TS_SMALL);
}

static void fill_list(void)
{
    int state = wifi_state(), sel = nzlist_selected(W.list);
    nzlist_clear(W.list);
    if (state == WIFI_ON) nzlist_add(W.list, ICON_WIFI, "Scan for networks", "takes about 15 seconds", NULL);
    else if (state == WIFI_STARTING) nzlist_add(W.list, ICON_WIFI, "Switching on...", "waiting for the dongle", NULL);
    else nzlist_add(W.list, ICON_POWER, "Turn Wi-Fi on", "needs Settings > USB on Devices", NULL);
    W.action_rows = 1;
    if (!strncmp(wifi_link_status(), "state=COMPLETED", 15)) {
        nzlist_add(W.list, ICON_LOCK, "Disconnect", join_ssid, NULL);
        W.action_rows = 2;
    }
    W.saved_count = wifi_saved_list(W.saved, MAX_SAVED);
    memset(W.row_ssid, 0, sizeof W.row_ssid);
    int r = 0;
    for (int i = 0; i < W.net_count && r < MAX_ROWS; i++, r++) {      /* the networks found */
        char sub[64], right[24];
        int saved = is_saved(W.nets[i].ssid);
        snprintf(sub, sizeof sub, "%schannel %d  -  %s", saved ? "saved  -  " : "", W.nets[i].channel,
                 W.nets[i].secure ? "password" : "open");
        snprintf(right, sizeof right, "%s  %d dBm", signal_word(W.nets[i].dbm), W.nets[i].dbm);
        nzlist_add(W.list, W.nets[i].secure ? ICON_LOCK : ICON_WIFI, W.nets[i].ssid, sub, right);
        snprintf(W.row_ssid[r], sizeof W.row_ssid[r], "%s", W.nets[i].ssid);
        W.row_secure[r] = (char)W.nets[i].secure;
        W.row_saved[r] = (char)saved;
    }
    for (int k = 0; k < W.saved_count && r < MAX_ROWS; k++) {          /* saved, but not seen now */
        int seen = 0;
        for (int i = 0; i < W.net_count; i++) if (!strcmp(W.nets[i].ssid, W.saved[k])) seen = 1;
        if (seen) continue;
        nzlist_add(W.list, ICON_LOCK, W.saved[k], W.net_count ? "saved  -  not in reach now" : "saved", NULL);
        snprintf(W.row_ssid[r], sizeof W.row_ssid[r], "%s", W.saved[k]);
        W.row_secure[r] = 1;
        W.row_saved[r] = 1;
        r++;
    }
    if (sel > 0) nzlist_select(W.list, sel);
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    wifi_poll();
    int state = wifi_state(), dongle = wifi_dongle();
    if (W.scanning && (wifi_scan_done() || nzeus_tick_ms() - W.scan_started > (uint32_t)wifi_scan_wait_ms)) {   /* the scan is done: read it */
        W.scanning = 0;
        W.net_count = wifi_scan_read(W.nets, MAX_NETS);
        fill_list();
        if (!W.net_count) ui_toast("No networks found");
    }
    if (state != W.last_state || dongle != W.last_dongle || strcmp(W.last_link, wifi_link_status())) {
        W.last_state = state;
        W.last_dongle = dongle;
        snprintf(W.last_link, sizeof W.last_link, "%s", wifi_link_status());
        if (!W.scanning) fill_list();
    }
    check_join();
    show_status();
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&W, 0, sizeof W);
    W.last_state = -1;
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card = W.card = th_box(root, TS_PANEL);
    lv_obj_set_size(card, LV_PCT(100), 72);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_WIFI, 48));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    W.status = th_label(card, "", TS_HEADING);
    lv_obj_align(W.status, LV_ALIGN_TOP_LEFT, 72, 8);
    W.detail = th_label(card, "", TS_SMALL);
    lv_label_set_long_mode(W.detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(W.detail, LV_PCT(85));
    lv_obj_align(W.detail, LV_ALIGN_BOTTOM_LEFT, 72, -10);

    W.list = nzlist_create(root, 0);
    lv_obj_set_width(nzlist_obj(W.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(W.list), 1);
    nzlist_on_activate(W.list, list_activate, NULL);

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Choose", "X", "Info", "Y", "Forget", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 4));

    refresh(NULL);
    W.timer = lv_timer_create(refresh, 250, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (W.info) { close_info(); return 1; }                  /* the ID card is open: any button closes it */
    if (k == NZ_KEY_X) { show_info(); return 1; }
    if (k == NZ_KEY_Y) { forget_selected(); return 1; }
    return nzlist_key(W.list, k);
}

static void destroy(NzApp *a)
{
    (void)a;
    if (W.timer) lv_timer_del(W.timer);
    close_info();
    nzlist_free(W.list);
    memset(&W, 0, sizeof W);
}

/* for tests */
int app_wifi_network_count(void) { return W.net_count; }
lv_obj_t *app_wifi_spinner(void) { return W.spinner; }
const char *app_wifi_detail(void) { return W.detail ? lv_label_get_text(W.detail) : ""; }
const char *app_wifi_card_text(void) { return W.info ? W.info_text : ""; }

NzApp app_wifi = {
    .id = APP_WIFI, .title = "Wi-Fi", .icon = ICON_WIFI,
    .create = create, .key = key, .destroy = destroy,
};

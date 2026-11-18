/* app_btscan.c - the deck's Bluetooth scanner: "what is this device?"
 * Lists every Bluetooth device nearby with a best guess of what it is
 * (AirPods model, tracker, keyboard, phone...) and shows its ID card.
 * The work is done by `nzeus-bt survey` (apps/bluetooth) in the background;
 * btinfo.c turns its lines into the guesses and cards.
 *
 *   A  the device's ID card     X  scan again     B  back (or close the card)
 * Trackers (AirTag / Find My, Tile, SmartTag) are counted at the top, and the
 * scanner remembers how long each one has been around while it is open:
 * one that stays near you for a long time is worth a look. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "apps.h"
#include "bgjob.h"
#include "bt.h"
#include "btinfo.h"
#include "home.h"
#include "icons.h"
#include "nzeus_tick.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define BT_TOOL     NZ_DIR "/bin/nzeus-bt"
#define MAKER_LIST  NZ_DIR "/lists/oui.txt"
#define SCAN_S      10                 /* seconds per scan */
#define MAX_DEVS    24
#define MAX_TRACK   16

static struct {
    lv_obj_t *status, *info;
    NzList *list;
    lv_timer_t *timer;
    BgJob job;
    BtSurvey *dev;                     /* the devices found (4.5 KB, borrowed while the app is open) */
    int ndev, scanned, trackers;
    char problem[96];
    struct { char addr[18]; uint32_t first_ms; int scans; } seen[MAX_TRACK];   /* trackers seen so far */
    int nseen;
    char card[1600];
} S;

/* ---------- scanning ---------- */

static void start_scan(void)
{
    char cmd[160];
    if (bgjob_running(&S.job)) return;
    if (bt_state() != BT_ON) return;
    snprintf(cmd, sizeof cmd, BT_TOOL " survey %d", SCAN_S);
    if (bgjob_start(&S.job, cmd, "/tmp/nzeus-bt-survey.log") != 0) snprintf(S.problem, sizeof S.problem, "Could not start the scan");
}

/* a tracker seen again: how long has it been around? (in minutes) */
static int tracker_minutes(const char *addr, int add)
{
    uint32_t now = nzeus_tick_ms();
    for (int i = 0; i < S.nseen; i++)
        if (!strcmp(S.seen[i].addr, addr)) { S.seen[i].scans += add; return (int)((now - S.seen[i].first_ms) / 60000); }
    if (add && S.nseen < MAX_TRACK) {
        snprintf(S.seen[S.nseen].addr, sizeof S.seen[0].addr, "%s", addr);
        S.seen[S.nseen].first_ms = now;
        S.seen[S.nseen++].scans = 1;
    }
    return 0;
}

static void fill_list(void);

/* the scan finished: read its lines */
static void read_answer(void)
{
    char line[400];
    FILE *f = fopen(S.job.out_path, "r");
    S.ndev = S.trackers = 0;
    S.problem[0] = 0;
    if (f) {
        while (S.dev && fgets(line, sizeof line, f) && S.ndev < MAX_DEVS) {
            if (!strncmp(line, "error ", 6)) { snprintf(S.problem, sizeof S.problem, "%.80s", line + 6); continue; }
            if (bt_parse_survey(line, &S.dev[S.ndev]) != 0) continue;
            if (bt_is_tracker(&S.dev[S.ndev])) { S.trackers++; tracker_minutes(S.dev[S.ndev].addr, 1); }
            S.ndev++;
        }
        fclose(f);
    }
    S.problem[strcspn(S.problem, "\r\n")] = 0;
    S.scanned++;
    fill_list();
}

/* ---------- showing ---------- */

/* "Likely: Apple AirPods Pro - from ... (high)" -> "Apple AirPods Pro" (for the list) */
static void short_guess(const BtSurvey *d, char *out, size_t n)
{
    char g[200];
    bt_guess(d, g, sizeof g);
    const char *s = !strncmp(g, "Likely: ", 8) ? g + 8 : g;
    const char *dash = strstr(s, " - ");
    snprintf(out, n, "%.*s", dash ? (int)(dash - s) : (int)strlen(s), s);
}

static void fill_list(void)
{
    int sel = nzlist_selected(S.list);
    nzlist_clear(S.list);
    for (int i = 0; i < S.ndev; i++) {
        char sub[120], right[24];
        const BtSurvey *d = &S.dev[i];
        char guess[100];
        short_guess(d, guess, sizeof guess);
        snprintf(sub, sizeof sub, "%s%s", bt_is_tracker(d) ? "Tracker: " : "", guess);   /* trackers say so first */
        snprintf(right, sizeof right, "%d dBm", d->rssi);
        nzlist_add(S.list, -1, d->name[0] ? d->name : d->addr, sub, right);
    }
    if (sel >= 0 && sel < S.ndev) nzlist_select(S.list, sel);
}

static void show_status(void)
{
    char s[200];
    int st = bt_state();
    if (st == BT_MISSING) snprintf(s, sizeof s, "The Bluetooth add-on is not on the SD card");
    else if (st == BT_NO_DONGLE) snprintf(s, sizeof s, "No Bluetooth dongle - plug it in");
    else if (st == BT_CHECKING) snprintf(s, sizeof s, "Checking Bluetooth...");
    else if (st != BT_ON) snprintf(s, sizeof s, "Bluetooth is off - switch it on in Settings > Bluetooth");
    else if (bgjob_running(&S.job)) snprintf(s, sizeof s, "Scanning for %d seconds...  %s", SCAN_S, S.ndev ? "(the last scan is below)" : "");
    else if (S.problem[0]) snprintf(s, sizeof s, "Scan problem: %s", S.problem);
    else if (!S.scanned) snprintf(s, sizeof s, "Starting a scan...");
    else if (S.trackers) snprintf(s, sizeof s, "%d device%s nearby  -  %d tracker%s", S.ndev, S.ndev == 1 ? "" : "s",
                                  S.trackers, S.trackers == 1 ? "" : "s");
    else snprintf(s, sizeof s, "%d device%s nearby, no trackers", S.ndev, S.ndev == 1 ? "" : "s");
    lv_label_set_text(S.status, s);
}

static void close_card(void)
{
    if (S.info) { lv_obj_del(S.info); S.info = NULL; }
}

static void show_card(int i)
{
    if (i < 0 || i >= S.ndev) return;
    size_t k;
    bt_card(&S.dev[i], MAKER_LIST, S.card, sizeof S.card);
    if (bt_is_tracker(&S.dev[i])) {
        int min = tracker_minutes(S.dev[i].addr, 0);
        k = strlen(S.card);
        snprintf(S.card + k, sizeof S.card - k, "\nAround     %d min while this scanner was open", min);
    }
    S.info = lv_obj_create(lv_layer_top());                 /* over the whole screen */
    lv_obj_remove_style_all(S.info);
    th_add(S.info, TS_PANEL);
    lv_obj_set_size(S.info, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(S.info, 14, 0);
    lv_obj_set_flex_flow(S.info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.info, 8, 0);
    th_label(S.info, "Bluetooth ID card", TS_HEADING);
    lv_obj_t *t = th_label(S.info, S.card, TS_MONO);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    th_label(S.info, "Any button closes this card", TS_SMALL);
}

static void activate(int i, void *ctx) { (void)ctx; show_card(i); }

static void tick(lv_timer_t *t)
{
    (void)t;
    bt_poll();                                               /* (keeps the Bluetooth state fresh) */
    if (bgjob_running(&S.job) && bgjob_poll(&S.job) == 0) read_answer();
    else if (!S.scanned && !bgjob_running(&S.job) && bt_state() == BT_ON) start_scan();   /* the first scan by itself */
    show_status();
}

/* ---------- the app ---------- */

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&S, 0, sizeof S);
    S.dev = calloc(MAX_DEVS, sizeof *S.dev);
    if (!S.dev) ui_toast("Not enough memory for the device list");
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bar = th_box(root, TS_BAR);
    lv_obj_set_size(bar, LV_PCT(100), 30);
    S.status = th_label(bar, "", TS_TEXT);
    lv_obj_align(S.status, LV_ALIGN_LEFT_MID, 12, 0);

    S.list = nzlist_create(root, 0);
    lv_obj_set_width(nzlist_obj(S.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(S.list), 1);
    nzlist_on_activate(S.list, activate, NULL);

    lv_obj_t *hint = th_box(root, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "ID card", "X", "Scan again", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 3));

    bt_refresh();                                            /* ask whether Bluetooth is on */
    show_status();
    S.timer = lv_timer_create(tick, 250, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (S.info) { close_card(); return 1; }                  /* the card is open: any button closes it */
    if (k == NZ_KEY_X) {
        if (bt_state() != BT_ON) { ui_toast("Switch Bluetooth on first (Settings > Bluetooth)"); return 1; }
        start_scan();
        return 1;
    }
    return nzlist_key(S.list, k);
}

static void destroy(NzApp *a)
{
    (void)a;
    if (S.timer) lv_timer_del(S.timer);
    bgjob_stop(&S.job);
    close_card();
    nzlist_free(S.list);
    free(S.dev);                                             /* the list's memory goes back */
    memset(&S, 0, sizeof S);
}

/* for tests */
int app_btscan_count(void) { return S.ndev; }
int app_btscan_trackers(void) { return S.trackers; }
const char *app_btscan_card(void) { return S.info ? S.card : ""; }
const char *app_btscan_status(void) { return S.status ? lv_label_get_text(S.status) : ""; }

NzApp app_btscan = {
    .id = APP_BTSCAN, .title = "Bluetooth scanner", .icon = ICON_KEYBOARD,
    .create = create, .key = key, .destroy = destroy,
};

/* app_update.c - "Update NZEUS" (Tools list): installs a new NZEUS from the PC
 * over Wi-Fi, with no cable and no flashing.
 *
 * On the PC: python tools/publish_update.py --modern --notes "what is new"
 * Here:      A = check -> it shows the version, what's new and how many files
 *            change -> A = install (asks first) -> restart NZEUS Deck.
 * The work is done by nzeus/bin/nzeus-update.sh (core/deck-modern): it
 * installs nothing that isn't signed, checks every file before changing
 * anything and keeps the old copies (nzeus-modern.prev). "Restart now"
 * restarts the Deck, not the console: if the new Deck does not keep running
 * for a minute, the old one comes back by itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "apps.h"
#include "bgjob.h"
#include "home.h"
#include "icons.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define UPDATE_SH NZ_DIR "/bin/nzeus-update.sh"
#define PUBLISH_CMD   "python tools/publish_update.py --modern"
#define KEPT_TEXT     "The old version is kept. If the new one doesn't keep\nrunning for a minute, the old one comes back by itself."
#define DONE_TEXT     "Restart NZEUS Deck to start the new version ({accept}).\nIf it keeps running for a minute it is kept;\nif not, the old one comes back by itself."
#define RESTART_ASK   "Restart NZEUS Deck now?"
#define LOG_CHECK   "/tmp/nzeus-update-check.log"
#define LOG_INSTALL NZ_DIR "/update/install.log"      /* (on the card: it tells what happened, even after a restart) */

enum { ST_IDLE, ST_CHECKING, ST_READY, ST_NOTHING, ST_INSTALLING, ST_DONE, ST_ERROR };

/* the install keeps running even if the app is closed (stopping it half-way
 * would leave a half-installed update), so its job lives outside the app */
static BgJob install_job;

static struct {
    int state;
    char version[48], notes[200], error[200], progress[160];
    int files;
    long bytes;
    BgJob check_job;
    lv_timer_t *timer;
    lv_obj_t *title, *body, *hint_bar, *hint;
} U;

/* ---------- reading the script's answers ---------- */

static void read_check(void)
{
    char line[300];
    FILE *f = fopen(LOG_CHECK, "r");
    U.version[0] = U.notes[0] = U.error[0] = 0;
    U.files = -1;
    U.bytes = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            /* (%.Ns: a long line is cut to fit, on purpose) */
            if (!strncmp(line, "VERSION ", 8)) snprintf(U.version, sizeof U.version, "%.47s", line + 8);
            else if (!strncmp(line, "NOTES ", 6)) snprintf(U.notes, sizeof U.notes, "%.199s", line + 6);
            else if (!strncmp(line, "FILES ", 6)) U.files = atoi(line + 6);
            else if (!strncmp(line, "BYTES ", 6)) U.bytes = atol(line + 6);
            else if (!strncmp(line, "error ", 6)) snprintf(U.error, sizeof U.error, "%.199s", line + 6);
        }
        fclose(f);
    }
    if (U.error[0] || U.files < 0) {
        if (!U.error[0]) snprintf(U.error, sizeof U.error, "No answer from the update script");
        U.state = ST_ERROR;
    } else U.state = U.files ? ST_READY : ST_NOTHING;
}

/* the install's last PROGRESS line (and how it ended) */
static void read_install(int finished)
{
    char line[300];
    FILE *f = fopen(LOG_INSTALL, "r");
    int done = 0;
    U.error[0] = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            int i, n;
            char path[200];
            if (sscanf(line, "PROGRESS %d %d %199s", &i, &n, path) == 3)
                snprintf(U.progress, sizeof U.progress, "Downloading and checking %d of %d\n%.100s", i, n, path);
            else if (!strncmp(line, "DONE", 4)) done = 1;
            else if (!strncmp(line, "error", 5)) snprintf(U.error, sizeof U.error, "%.199s", line + (line[5] == ' ' ? 6 : 5));
        }
        fclose(f);
    }
    if (finished) U.state = done && !U.error[0] ? ST_DONE : ST_ERROR;
    if (finished && U.state == ST_ERROR && !U.error[0]) snprintf(U.error, sizeof U.error, "The install stopped (see nzeus/update/install.log)");
}

/* ---------- the screen ---------- */

static void set_hint(void)
{
    static const char *const h_check[] = { "A", "Check for an update", "B", "Back" };
    static const char *const h_install[] = { "A", "Install", "X", "Check again", "B", "Back" };
    static const char *const h_restart[] = { "A", "Restart now", "B", "Later" };
    static const char *const h_wait[] = { "", "Please wait..." };
    if (U.hint) lv_obj_del(U.hint);
    if (U.state == ST_READY) U.hint = nz_hints(U.hint_bar, h_install, 3);
    else if (U.state == ST_DONE) U.hint = nz_hints(U.hint_bar, h_restart, 2);
    else if (U.state == ST_CHECKING || U.state == ST_INSTALLING) U.hint = nz_hints(U.hint_bar, h_wait, 1);
    else U.hint = nz_hints(U.hint_bar, h_check, 2);
    lv_obj_center(U.hint);
}

static void show(void)
{
    char s[600], size[32];
    if (U.bytes >= 1024 * 1024) snprintf(size, sizeof size, "%.1f MB", U.bytes / 1048576.0);
    else snprintf(size, sizeof size, "%ld KB", (U.bytes + 1023) / 1024);
    switch (U.state) {
    case ST_CHECKING:
        lv_label_set_text(U.title, "Asking the PC...");
        lv_label_set_text(U.body, "The PC must be running " PUBLISH_CMD ", and Wi-Fi must be on.");
        break;
    case ST_READY:
        lv_label_set_text_fmt(U.title, "Update ready: %s", U.version);
        snprintf(s, sizeof s, "What's new: %s\n\n%d file%s change (%s).\n\n" KEPT_TEXT,
                 U.notes[0] ? U.notes : "(no notes)", U.files, U.files == 1 ? "" : "s", size);
        lv_label_set_text(U.body, s);
        break;
    case ST_NOTHING:
        lv_label_set_text(U.title, "NZEUS Deck is up to date");
        lv_label_set_text_fmt(U.body, "The PC offers %s, and every file here is the same.", U.version[0] ? U.version : "nothing new");
        break;
    case ST_INSTALLING:
        lv_label_set_text(U.title, "Installing...");
        lv_label_set_text(U.body, U.progress[0] ? U.progress : "Starting...");
        break;
    case ST_DONE:
        lv_label_set_text_fmt(U.title, "Installed: %s", U.version);
        nz_button_text(U.body, DONE_TEXT);
        break;
    case ST_ERROR:
        lv_label_set_text(U.title, "No update");
        snprintf(s, sizeof s, "%s", U.error);
        if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');   /* (the script's messages start small) */
        lv_label_set_text(U.body, s);
        break;
    default:
        lv_label_set_text(U.title, "Update NZEUS Deck");
        nz_button_text(U.body, "Gets the newest NZEUS Deck from your PC over Wi-Fi.\n\nOn the PC: " PUBLISH_CMD "\nThen press {accept} here.");
        break;
    }
    set_hint();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (U.state == ST_CHECKING && bgjob_poll(&U.check_job) == 0) { read_check(); show(); }
    if (U.state == ST_INSTALLING) {
        int running = bgjob_poll(&install_job);
        read_install(!running);
        show();
    }
}

static void start_check(void)
{
    if (bgjob_start(&U.check_job, "sh " UPDATE_SH " check", LOG_CHECK) != 0) {
        snprintf(U.error, sizeof U.error, "Could not start the update script (is nzeus-update.sh on the card?)");
        U.state = ST_ERROR;
    } else U.state = ST_CHECKING;
    show();
}

static void install_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!yes) return;
    U.progress[0] = 0;
    mkdir(NZ_DIR "/update", 0755);                           /* (the log goes there) */
    if (bgjob_start(&install_job, "sh " UPDATE_SH " install", LOG_INSTALL) != 0) {
        snprintf(U.error, sizeof U.error, "Could not start the install");
        U.state = ST_ERROR;
    } else U.state = ST_INSTALLING;
    show();
}

static void restart_answer(int yes, void *ctx)
{
    (void)ctx;
    /* the script restarts the Deck from a session of its own (it outlives this one) */
    if (yes && system("sh " UPDATE_SH " restart > /dev/null 2>&1 < /dev/null") != 0)
        ui_toast("Could not restart: run  nzeus-deck stop; nzeus-deck start  over SSH");
}

/* ---------- the app ---------- */

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&U, 0, sizeof U);
    th_add(root, TS_SCREEN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    U.title = th_label(body, "", TS_HEADING);
    U.body = th_label(body, "", TS_TEXT);
    lv_obj_set_width(U.body, LV_PCT(100));
    lv_label_set_long_mode(U.body, LV_LABEL_LONG_WRAP);
    U.hint_bar = th_box(root, TS_BAR);
    lv_obj_set_size(U.hint_bar, LV_PCT(100), 26);
    if (bgjob_running(&install_job)) U.state = ST_INSTALLING;   /* (opened again while installing) */
    show();
    U.timer = lv_timer_create(tick, 300, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (U.state == ST_INSTALLING) {                          /* never half-way: wait for it */
        if (k == NZ_KEY_B) ui_toast("Installing - please wait until it is done");
        return 1;
    }
    switch (k) {
    case NZ_KEY_A:
        if (U.state == ST_CHECKING) return 1;
        if (U.state == ST_READY) {
            static char q[160];
            snprintf(q, sizeof q, "Install %s? %d file%s change. Keep the charger in.", U.version, U.files, U.files == 1 ? "" : "s");
            nz_confirm(q, install_answer, NULL);
        } else if (U.state == ST_DONE) nz_confirm(RESTART_ASK, restart_answer, NULL);
        else start_check();
        return 1;
    case NZ_KEY_X:
        if (U.state == ST_READY) start_check();
        return 1;
    default: return 0;                                       /* B: the app closes */
    }
}

static void destroy(NzApp *a)
{
    (void)a;
    if (U.timer) lv_timer_del(U.timer);
    bgjob_stop(&U.check_job);                                /* (a check can stop; an install can't) */
    memset(&U, 0, sizeof U);
}

/* for tests */
int app_update_state(void) { return U.state; }
const char *app_update_text(void) { return U.body ? lv_label_get_text(U.body) : ""; }
const char *app_update_title(void) { return U.title ? lv_label_get_text(U.title) : ""; }

NzApp app_update = {
    .id = APP_UPDATE, .title = "Update NZEUS Deck", .icon = ICON_POWER,
    .create = create, .key = key, .destroy = destroy,
};

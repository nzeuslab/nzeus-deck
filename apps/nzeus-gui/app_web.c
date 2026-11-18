/* app_web.c - the NZEUS Web app: NetSurf, a real web browser (Debian's
 * netsurf-fb), full screen.
 *
 * Opening the app shows "Opening the browser..." and then hands the whole
 * screen to nzeus/bin/nzeus-browser (linux-port/deck-modern) through
 * hw_fullscreen_run: the Deck stops drawing until NetSurf ends. NetSurf's
 * buttons are explained on its start page (D-pad = pointer, A = click,
 * B = back, Y = type an address, SELECT+START = back to the Deck).
 * When it ends, nzeus-browser's exit code says why:
 *   10  Y was pressed: this app asks for a web address or words to search
 *       for, then opens NetSurf there (B in the box: NetSurf again at the
 *       same page)
 *    0  the owner quit: back home
 *   any other: "The browser could not start" (B goes home)
 * Without NetSurf (or without the launcher, or a frontend that can't hand
 * over the screen) the app says how to install it instead of pretending.
 *
 * Favourites: NZ_DIR/web/favourites.txt, one "Name|https://address" per line.
 * nzeus-browser lists them on NetSurf's start page; this app only makes the
 * file (a starter list) the first time, so the owner can edit it (Editor). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "devtools.h"                            /* dt_url_encode (search words) */
#include "home.h"
#include "hw.h"                                  /* hw_fullscreen_run */
#include "icons.h"
#include "nzui.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define FAV_FILE    NZ_DIR "/web/favourites.txt"
#define SEARCH_URL  "https://lite.duckduckgo.com/lite/?q="
#define NOT_INSTALLED "The browser is not installed: apt install netsurf-fb"

/* where NetSurf and its launcher are (the PC tests point these at fakes) */
const char *web_netsurf_path = "/usr/bin/netsurf-fb";
const char *web_launcher_path = NZ_DIR "/bin/nzeus-browser";

/* the starter favourites (made once; NetSurf's start page shows them) */
static const char default_favs[] =
    "# NZEUS Deck Web favourites: one website per line,  Name|https://address\n"
    "DuckDuckGo (search)|https://lite.duckduckgo.com/lite/\n"
    "Wikipedia|https://en.m.wikipedia.org/wiki/Main_Page\n"
    "Weather (where you are)|https://wttr.in/?n&A&T\n"
    "Al Jazeera English|https://www.aljazeera.com/\n"
    "BBC News|https://www.bbc.com/news\n"
    "CNN Lite|https://lite.cnn.com/\n"
    "NPR Text|https://text.npr.org/\n"
    "68k.news (world headlines)|http://68k.news/\n"
    "Hacker News (tech)|https://news.ycombinator.com/\n"
    "Electronics Tutorials|https://www.electronics-tutorials.ws/\n"
    "Project Gutenberg (free books)|https://www.gutenberg.org/\n"
    "Test page (example.com)|https://example.com/\n";

static struct {
    lv_obj_t *msg;                    /* the one line on the app's screen (NULL = the app is closed) */
    lv_timer_t *start;                /* NetSurf starts on the next loop (the screen is drawn first) */
} B;
/* (kept after the app closes: nzctl's "web URL" reads the answer afterwards) */
static char url[512];                 /* the page NetSurf was last opened at ("home" = its start page) */
static char result[200];              /* how the last start went ("ok: ..." / "error: ...") */
static char pending[512];             /* app_web_open: the address to open first */

/* NetSurf can run here: installed, the launcher is there, the frontend hands over the screen */
static int netsurf_ready(void)
{
    return hw_fullscreen_run && access(web_netsurf_path, X_OK) == 0 && access(web_launcher_path, X_OK) == 0;
}

/* a line on the app's screen (also remembered for nzctl's "web URL") */
static void say(const char *text, int problem)
{
    if (problem) snprintf(result, sizeof result, "error: %.180s", text);
    if (!B.msg) return;
    lv_label_set_text(B.msg, text);
    lv_obj_clear_flag(B.msg, LV_OBJ_FLAG_HIDDEN);
}

/* the favourites file, made once */
static void make_favourites(void)
{
    if (access(FAV_FILE, F_OK) == 0) return;
    mkdir(NZ_DIR, 0755);
    mkdir(NZ_DIR "/web", 0755);
    FILE *f = fopen(FAV_FILE, "w");
    if (f) { fputs(default_favs, f); fclose(f); }
}

static void close_later(lv_timer_t *t)                  /* (not from inside this app's own code) */
{
    (void)t;
    NzApp *a = apps_get(APP_WEB);
    if (a && a->root) apps_close(a);
}

static void netsurf_run(const char *url);

/* What was typed in the address box: an address, a file, or words to search for */
static void address_typed(const char *t, void *ctx)
{
    (void)ctx;
    char u[sizeof url];
    if (!t || !t[0]) { netsurf_run(url[0] ? url : "home"); return; }       /* B: back to the page */
    if (!strncmp(t, "http://", 7) || !strncmp(t, "https://", 8) || !strncmp(t, "file://", 7))
        snprintf(u, sizeof u, "%s", t);
    else if (t[0] == '/')                                                     /* a file on the console */
        snprintf(u, sizeof u, "file://%s", t);
    else if (strchr(t, '.') && !strchr(t, ' '))                               /* "example.com" */
        snprintf(u, sizeof u, "https://%s%s", t, strchr(t, '/') ? "" : "/");
    else {                                                                    /* plain words: a search */
        char words[400];
        dt_url_encode(t, words, sizeof words);
        snprintf(u, sizeof u, SEARCH_URL "%s", words);
    }
    netsurf_run(u);
}

/* NetSurf at URL ("home" = its start page), full screen until it ends */
static void netsurf_run(const char *where)
{
    if (!B.msg) return;                                  /* (the app was closed meanwhile) */
    if (!netsurf_ready()) { say(NOT_INSTALLED, 1); return; }
    if (where != url) snprintf(url, sizeof url, "%s", where);
    const char *argv[] = { web_launcher_path, url, NULL };
    int rc = hw_fullscreen_run(argv);
    if (rc == 10) {                                      /* Y: the address box (the screen behind it: empty) */
        lv_obj_add_flag(B.msg, LV_OBJ_FLAG_HIDDEN);
        snprintf(result, sizeof result, "ok: opened in the browser: %.150s", url);
        nz_prompt("A web address (or words to search for)", "", address_typed, NULL);
        return;
    }
    if (rc != 0) { say("The browser could not start (see /tmp/nzeus-browser.log)", 1); return; }
    snprintf(result, sizeof result, "ok: opened in the browser: %.150s", url);
    lv_timer_t *t = lv_timer_create(close_later, 0, NULL);   /* quit in NetSurf: back home */
    lv_timer_set_repeat_count(t, 1);
}

static void start_now(lv_timer_t *t)
{
    (void)t;
    B.start = NULL;                                      /* (a one-shot timer: LVGL deletes it) */
    netsurf_run(url);
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&B, 0, sizeof B);
    th_add(root, TS_SCREEN);
    B.msg = th_label(root, "", TS_MUTED);
    lv_label_set_long_mode(B.msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(B.msg, LV_PCT(90));
    lv_obj_set_style_text_align(B.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(B.msg, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_center(B.msg);
    make_favourites();
    snprintf(url, sizeof url, "%s", pending[0] ? pending : "home");
    pending[0] = 0;
    result[0] = 0;
    if (!netsurf_ready()) { say(NOT_INSTALLED, 1); return; }
    say("Opening the browser...", 0);
    /* (150 ms: this screen is drawn first; NetSurf then takes a few seconds,
     * and it stays on the screen meanwhile) */
    B.start = lv_timer_create(start_now, 150, NULL);
    lv_timer_set_repeat_count(B.start, 1);
}

static int key(NzApp *a, int k)
{
    (void)a;
    (void)k;
    return 0;                                            /* (B: home, as in every app) */
}

static void destroy(NzApp *a)
{
    (void)a;
    if (B.start) lv_timer_del(B.start);
    memset(&B, 0, sizeof B);
}

/* Open NetSurf at an address, or a file (a path) - nzctl's "web URL" and the tests.
 * The answer (app_web_result) is there once NetSurf has ended. */
int app_web_open(const char *path)
{
    if (path[0] == '/') snprintf(pending, sizeof pending, "file://%.490s", path);
    else snprintf(pending, sizeof pending, "%.511s", path);
    NzApp *a = apps_get(APP_WEB);
    if (a && a->root) apps_close(a);
    return apps_open(APP_WEB);
}

/* for tests and nzctl */
const char *app_web_current(void) { return url; }
int app_web_loading(void) { return B.start != NULL; }
const char *app_web_result(void) { return result[0] ? result : "error: no page was opened"; }
const char *app_web_message(void) { return B.msg && !lv_obj_has_flag(B.msg, LV_OBJ_FLAG_HIDDEN) ? lv_label_get_text(B.msg) : ""; }

NzApp app_web = {
    .id = APP_WEB, .title = "Web", .icon = ICON_WEB,
    .create = create, .key = key, .destroy = destroy,
};

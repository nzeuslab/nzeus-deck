/* app_whatsapp.c - the NZEUS WhatsApp app.
 *
 * WhatsApp itself runs on the PC (the "brain", R36S/brain/whatsapp). This app
 * asks it questions over SSH with nzeus/bin/nzeus-wa.sh, in the background
 * (each question takes a few seconds on this slow CPU; the screen never waits):
 *   chats            -> ID <TAB> UNREAD <TAB> TIME <TAB> NAME <TAB> LAST MESSAGE
 *   read ID 60       -> TIME <TAB> MINE <TAB> AUTHOR <TAB> TEXT <TAB> MSG_ID <TAB> KIND   (oldest first)
 *                       (KIND: text photo video voice file sticker other; an older
 *                        PC sends only the first 4: then no MSG_ID, KIND text)
 *   send ID BASE64   -> ok
 *   photo ID MSG_ID FILE   -> ok  (the script saved the photo, a JPEG, as FILE)
 *   voice ID MSG_ID FILE   -> ok  (the script saved the voice message, a WAV, as FILE)
 *   sendphoto ID FILE      -> ok  (the script sent the picture's bytes)
 *   any problem      -> error: plain words     (nothing at all: the PC is not there)
 * Inside a text, a line break arrives as the two letters \n, a backslash as \\.
 * Emoji arrive as one code point each and are drawn as small pictures (emoji.c,
 * through artext.c).
 *
 * The SSH key: nzeus/whatsapp/wa_key.enc, locked with the Claude app's PIN
 * (keyvault.h). Every time the app opens it asks the PIN and unlocks the key
 * into RAM (/tmp/nzeus-wa-key); closing the app deletes that copy.
 *
 * Page 1, chats:  A open   X refresh   B back      (refreshes every 60 s)
 * Page 2, a chat: UP/DOWN scroll   L1/R1 page   A or X reply   Y photos, voice   B back to the chats
 *                 (mine on the right in the accent colour; reloads every 30 s)
 * Y in a chat: a small menu
 *   "Photos in this chat (N)": the photo messages, newest first; A shows one
 *       full screen (downloaded once into nzeus/whatsapp/photos/, at most
 *       about 60 kept there), B back
 *   "Send a picture": the JPG/PNG files on the SD card, newest first; A asks
 *       "Send ... to ...?", then sends it in the background
 *   "Voice messages (N)": the voice messages, newest first; A plays one
 *       (downloaded once into nzeus/whatsapp/voice/, at most about 40 kept;
 *       played by the music engine, audio.c, straight from the file), A again
 *       pauses, B stops; B when nothing plays goes back. Leaving the app stops it. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "lvgl.h"
#include "apps.h"
#include "artext.h"
#include "audio.h"
#include "bgjob.h"
#include "emoji.h"
#include "fsops.h"
#include "home.h"
#include "hw.h"
#include "icons.h"
#include "keyvault.h"
#include "nzlist.h"
#include "nzui.h"
#include "picture.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#define WA_DIR       NZ_DIR "/whatsapp"
#define WA_KEY_ENC   WA_DIR "/wa_key.enc"       /* the key, locked with the PIN */
#define WA_KEY_PLAIN WA_DIR "/wa_key"           /* (only for a moment, while it is made) */
#define WA_RUN_KEY   "/tmp/nzeus-wa-key"        /* unlocked, in RAM, while the app is open */
#define WA_SCRIPT    NZ_DIR "/bin/nzeus-wa.sh"
#define WA_OUT       "/tmp/nzeus-wa.out"        /* what the script printed */
#define MAX_CHATS    30
#define MAX_MSGS     60
#define WA_LINE     8192                       /* one line of the answer (a long message) */
#define NO_PC        "Could not reach the PC (is it on? is the bridge running?)"
#define WA_PHOTOS    WA_DIR "/photos"           /* photos seen on the console (a few, the newest) */
#define WA_KEEP_PHOTOS 60                       /* at most this many are kept there */
#define WA_VOICE     WA_DIR "/voice"            /* voice messages heard on the console (WAV) */
#define WA_KEEP_VOICE 40                        /* at most this many are kept there (up to ~2.6 MB each) */
#define MAX_PICKS    200                        /* pictures listed for sending */
#define SEND_MAX     (4L * 1024 * 1024)         /* the biggest picture the PC accepts: 4 MB */

int wa_reload_chat_ms = 30000;                  /* an open chat: reload every 30 s (tests: shorter) */
int wa_reload_list_ms = 60000;                  /* the chat list: every 60 s */

int claude_pin_ok(const char *pin);             /* app_claude.c: the PIN is shared */

typedef struct { char id[64], name[80], last[160]; int unread; long when; } WaChat;
/* text and id share one malloc'd block (freeing text frees both) */
typedef struct { long when; int mine, kind; char author[48]; char *text, *id; } WaMsg;
typedef struct { char path[256]; long when; } WaPick;   /* a picture that can be sent */

enum { ST_NO_PIN, ST_LOCKED, ST_MAKING_KEY, ST_SETUP, ST_READY };   /* the key's state */
/* the page on screen: the chats, a chat, its photos, one photo, pictures to send, its voice messages */
enum { PG_CHATS, PG_CHAT, PG_PHOTOS, PG_VIEW, PG_PICK, PG_VOICE };
enum { JOB_NONE, JOB_MAKE_KEY, JOB_CHATS, JOB_READ, JOB_SEND, JOB_PHOTO, JOB_SENDPHOTO, JOB_VOICE };   /* the background job */
/* a message's KIND (photos can be looked at, voice messages listened to) */
enum { KIND_TEXT, KIND_PHOTO, KIND_VIDEO, KIND_VOICE, KIND_FILE, KIND_STICKER, KIND_OTHER };
static const char *const kind_names[] = { "text", "photo", "video", "voice", "file", "sticker", "other" };

static struct {
    lv_obj_t *chats_page, *chat_page, *status, *info, *hint_bar;
    lv_obj_t *chat_name, *chat_status, *scroller;
    lv_obj_t *sub_page, *sub_title, *sub_status, *sub_hints;   /* page 3: the photos / the pictures to send */
    lv_obj_t *view_page, *view_img, *view_msg, *view_caption;  /* page 4: one photo, full screen */
    NzList *list, *sub_list;
    int photo_msg[MAX_MSGS], nphotos;   /* the photos list: row -> message (newest first) */
    char photo_path[200];               /* the photo being loaded / shown */
    Picture pic;                        /* its pixels (up to 640 x 480 x 2 = 600 KB; freed when it closes) */
    lv_img_dsc_t dsc;
    WaPick *picks;                      /* the pictures to send (malloc'd while that list is open) */
    int npicks, nbig;                   /* (nbig: skipped, over 4 MB) */
    char send_path[256];
    int voice_msg[MAX_MSGS], nvoice;    /* the voice list: row -> message (newest first) */
    char voice_file[200];               /* the voice message being loaded / played */
    int voice_want;                     /* the row being downloaded */
    int voice_row;                      /* the row in the music engine (-1 none) */
    int voice_state;                    /* what the hints say: 0 nothing playing, 1 playing, 2 paused */
    WaChat *chats;                  /* MAX_CHATS of them (malloc) */
    int nchats;
    WaMsg msgs[MAX_MSGS];           /* the open chat's messages (texts malloc'd) */
    int nmsgs;
    int state, page;
    char chat_id[64], chat_name_text[80];   /* the open chat */
    BgJob job;
    int job_kind;
    int need_chats, need_read;      /* waiting to be loaded when the job is free */
    uint32_t chats_at, read_at;     /* when they were last loaded (ms) */
    unsigned long read_hash;        /* the last chat answer (the same again: nothing to redraw) */
    int chat_shown;                 /* the messages were drawn at least once */
    char pin[16];                   /* only while the key is being made */
    char last_status[200];
    lv_timer_t *timer;
} W;

/* ---------- small text helpers (tests use the first two) ---------- */

/* "\n" (2 letters) -> a line break, "\\" -> one backslash; in place */
void wa_unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
        else if (p[0] == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
        else *o++ = *p;
    }
    *o = 0;
}

/* base64 (the PC wants the message text this way: no shell trouble with any letter) */
void wa_base64(const char *in, char *out, size_t n)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *s = (const unsigned char *)in;
    size_t len = strlen(in), o = 0;
    for (size_t i = 0; i < len && o + 5 < n; i += 3) {
        unsigned v = s[i] << 16 | (i + 1 < len ? s[i + 1] << 8 : 0) | (i + 2 < len ? s[i + 2] : 0);
        out[o++] = abc[(v >> 18) & 63];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = i + 1 < len ? abc[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? abc[v & 63] : '=';
    }
    out[o] = 0;
}

/* a chat ID is only plain characters (it goes into a command line) */
static int safe_id(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && n < 64 && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@._+/=-") == n;
}

/* a message ID from the PC: 5 to 160 plain characters (it goes into a command line) */
static int safe_msg_id(const char *s)
{
    size_t n = strlen(s);
    return n >= 5 && n <= 160 && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@._-") == n;
}

static int kind_of(const char *s)
{
    for (int i = 0; i < (int)(sizeof kind_names / sizeof kind_names[0]); i++)
        if (!strcmp(s, kind_names[i])) return i;
    return KIND_OTHER;
}

/* 'text' for the shell (a ' inside becomes '\'') */
static void sh_quote(const char *s, char *out, size_t n)
{
    size_t k = 0;
    out[k++] = '\'';
    for (; *s && k + 6 < n; s++) {
        if (*s == '\'') { memcpy(out + k, "'\\''", 4); k += 4; }
        else out[k++] = *s;
    }
    out[k++] = '\'';
    out[k] = 0;
}

/* cut a line into TAB-separated fields (in place); returns how many */
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    return n;
}

/* copy that never cuts a letter in half (UTF-8) */
static void copy_text(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* back to a letter's start */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* "14:05" today, "Yesterday", "Mon" this week, else "04 Oct" */
static void when_text(long t, char *out, size_t n)
{
    out[0] = 0;
    if (t <= 0) return;
    time_t now = hw_now(), tt = (time_t)t;
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(out, n, "%H:%M", &a);
    else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1) snprintf(out, n, "Yesterday");
    else if (now > tt && now - tt < 6 * 86400) strftime(out, n, "%a", &a);
    else strftime(out, n, "%d %b", &a);
}

/* one line of the answer file: 1 = got one (a too-long line is cut, the rest skipped) */
static int next_line(FILE *f, char *buf, int size)
{
    if (!fgets(buf, size, f)) return 0;
    size_t n = strlen(buf);
    if (n && buf[n - 1] != '\n') {                         /* longer than the buffer: skip the rest */
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n') {}
    }
    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

/* the first "error: ..." line of the answer (its words in out); 1 = there is one */
static int answer_error(char *out, size_t n)
{
    char line[300];
    int found = 0;
    FILE *f = fopen(WA_OUT, "r");
    if (!f) return 0;
    while (!found && next_line(f, line, sizeof line))
        if (!strncmp(line, "error: ", 7)) { copy_text(out, n, line + 7); found = 1; }
    fclose(f);
    return found;
}

/* ---------- what the screen says ---------- */
static void set_status(const char *text)
{
    snprintf(W.last_status, sizeof W.last_status, "%s", text);
    lv_obj_t *l = W.page == PG_CHAT ? W.chat_status                        /* each page has its own */
                : W.page == PG_PHOTOS || W.page == PG_PICK || W.page == PG_VOICE ? W.sub_status
                : W.page == PG_VIEW ? W.view_msg                           /* (the photo page: in the middle) */
                : W.status;
    lv_label_set_text(l, text);
}

/* show one page, hide the others */
static void show_page(int pg)
{
    lv_obj_t *pages[] = { W.chats_page, W.chat_page, W.sub_page, W.view_page };
    lv_obj_t *show = pg == PG_PICK || pg == PG_VOICE ? W.sub_page : pages[pg];   /* (photos, pictures, voice share a page) */
    for (int i = 0; i < 4; i++) if (pages[i] != show) lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(show, LV_OBJ_FLAG_HIDDEN);
    W.page = pg;
}

static void problem(const char *text)                     /* a toast + the status line */
{
    ui_toast(text);
    set_status(text);
}

static void hints_on(lv_obj_t *bar, const char *const *pairs, int n)
{
    lv_obj_clean(bar);
    lv_obj_center(nz_hints(bar, pairs, n));
}

static void set_hints(const char *const *pairs, int n) { hints_on(W.hint_bar, pairs, n); }

/* the chats page: the list when ready, else a message in the middle */
static void set_state(int st)
{
    static const char *const h_back[] = { "B", "Back" };
    static const char *const h_pin[] = { "A", "Type the PIN", "B", "Back" };
    static const char *const h_setup[] = { "A", "Continue", "B", "Back" };
    static const char *const h_ready[] = { "A", "Open", "X", "Refresh", "B", "Back" };
    W.state = st;
    int ready = st == ST_READY;
    if (ready) { lv_obj_clear_flag(nzlist_obj(W.list), LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(W.info, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(nzlist_obj(W.list), LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(W.info, LV_OBJ_FLAG_HIDDEN); }
    switch (st) {
    case ST_NO_PIN:
        lv_label_set_text(W.info, "Choose a PIN in the Claude app first.\n\nWhatsApp locks its key with the same PIN.");
        set_hints(h_back, 1);
        set_status("No PIN yet");
        break;
    case ST_LOCKED:
        lv_label_set_text(W.info, "Locked.\n\nA: type your PIN (the Claude app's PIN)");
        set_hints(h_pin, 2);
        set_status("Locked");
        break;
    case ST_MAKING_KEY:
        lv_label_set_text(W.info, "Making the WhatsApp key...");
        set_hints(h_back, 1);
        set_status("Setting up");
        break;
    case ST_SETUP:
        lv_label_set_text(W.info, "Key made and locked with your PIN.\n\n"
                                  "Send this to the PC:\nnzeus/whatsapp/wa_key.pub\n\n"
                                  "(the PC's helper copies it)");
        set_hints(h_setup, 2);
        set_status("Setup");
        break;
    default:
        set_hints(h_ready, 3);
        set_status(W.nchats ? "" : "Loading chats...");
        break;
    }
}

/* ---------- the background job ---------- */
static int start_job(int kind, const char *args)
{
    static char cmd[3600];
    char host[80], port[24], user[80];
    sh_quote(settings_get("claude_pc_host", ""), host, sizeof host);   /* the Claude app's PC brain */
    sh_quote(settings_get("claude_pc_port", "22"), port, sizeof port);
    sh_quote(settings_get("claude_pc_user", ""), user, sizeof user);
    snprintf(cmd, sizeof cmd, "NZ_WA_KEY=%s NZ_WA_HOST=%s NZ_WA_PORT=%s NZ_WA_USER=%s sh %s %s",
             WA_RUN_KEY, host, port, user, WA_SCRIPT, args);
    unlink(WA_OUT);
    if (bgjob_start(&W.job, cmd, WA_OUT) != 0) { problem("Could not start nzeus-wa.sh"); return -1; }
    W.job_kind = kind;
    return 0;
}

/* stop a load that is no longer wanted (a message being sent is never stopped) */
static void stop_loading(void)
{
    if (W.job_kind == JOB_CHATS || W.job_kind == JOB_READ) {
        bgjob_stop(&W.job);
        W.job_kind = JOB_NONE;
    }
}

/* start what is waiting, if the job is free */
static void pump(void)
{
    if (W.job_kind != JOB_NONE || W.state != ST_READY) return;
    char args[120];
    if (W.page == PG_CHAT && W.need_read) {
        W.need_read = 0;
        snprintf(args, sizeof args, "read '%s' %d", W.chat_id, MAX_MSGS);   /* (safe_id checked it) */
        if (start_job(JOB_READ, args) == 0 && !W.chat_shown) set_status("Loading...");
    } else if (W.page == PG_CHATS && W.need_chats) {
        W.need_chats = 0;
        if (start_job(JOB_CHATS, "chats") == 0) set_status(W.nchats ? "Updating..." : "Loading chats...");
    }
}

/* ---------- page 1: the chats ---------- */
static void fill_list(void)
{
    char keep[64] = "";                                    /* stay on the same chat */
    int sel = nzlist_selected(W.list);
    if (sel >= 0 && sel < W.nchats) snprintf(keep, sizeof keep, "%s", W.chats[sel].id);
    nzlist_clear(W.list);
    for (int i = 0; i < W.nchats; i++) {
        WaChat *c = &W.chats[i];
        char right[40], when[24];
        when_text(c->when, when, sizeof when);
        if (c->unread > 0) snprintf(right, sizeof right, "%s\n%d new", when, c->unread);
        else snprintf(right, sizeof right, "%s", when);
        int r = nzlist_add(W.list, ICON_CHAT, "", "", right);
        lv_obj_t *rt = nzlist_right_obj(W.list, r);
        if (rt) lv_obj_set_style_text_align(rt, LV_TEXT_ALIGN_RIGHT, 0);
        if (rt && c->unread > 0) th_add(rt, TS_ICON);      /* unread: in the accent colour */
    }
    /* the name and the last message: one line each, cut to the room the row has */
    lv_obj_update_layout(nzlist_obj(W.list));
    for (int i = 0; i < W.nchats; i++) {
        lv_obj_t *t = nzlist_title_obj(W.list, i), *s = nzlist_sub_obj(W.list, i);
        if (!t || !s) continue;
        int w = lv_obj_get_content_width(lv_obj_get_parent(t));
        if (w < 40) w = 40;
        artext_set_line(t, W.chats[i].name, w);
        artext_set_line(s, W.chats[i].last, w);
    }
    int pick = 0;
    for (int i = 0; i < W.nchats; i++) if (keep[0] && !strcmp(W.chats[i].id, keep)) pick = i;
    if (W.nchats) nzlist_select(W.list, pick);
}

static void chats_loaded(void)
{
    FILE *f = fopen(WA_OUT, "r");
    char *line = malloc(WA_LINE);
    W.nchats = 0;
    while (f && line && W.nchats < MAX_CHATS && next_line(f, line, WA_LINE)) {
        char *fl[5];
        if (split_tabs(line, fl, 5) < 5 || !safe_id(fl[0])) continue;   /* (not a chat line) */
        WaChat *c = &W.chats[W.nchats++];
        snprintf(c->id, sizeof c->id, "%s", fl[0]);
        c->unread = atoi(fl[1]);
        c->when = atol(fl[2]);
        wa_unescape(fl[3]);
        wa_unescape(fl[4]);
        copy_text(c->name, sizeof c->name, fl[3][0] ? fl[3] : fl[0]);
        copy_text(c->last, sizeof c->last, fl[4]);
    }
    free(line);
    if (f) fclose(f);
    fill_list();
    char s[60], now[16];
    when_text((long)hw_now(), now, sizeof now);
    if (W.nchats) snprintf(s, sizeof s, "%d chat%s  -  updated %s", W.nchats, W.nchats == 1 ? "" : "s", now);
    else snprintf(s, sizeof s, "No chats yet");
    set_status(s);
}

/* ---------- page 2: a chat ---------- */
static void free_msgs(void)
{
    for (int i = 0; i < W.nmsgs; i++) free(W.msgs[i].text);
    memset(W.msgs, 0, sizeof W.msgs);
    W.nmsgs = 0;
}

/* a message's text on a label, at most maxw pixels wide (Arabic: joined, right to left) */
static void bubble_text(lv_obj_t *l, const char *text, int maxw)
{
    if (artext_has_arabic(text)) { artext_set(l, text, maxw); return; }   /* (its lines already fit) */
    artext_set(l, text, 0);                                /* (emoji: the font gets the emoji pictures) */
    lv_point_t sz;
    lv_txt_get_size(&sz, text, lv_obj_get_style_text_font(l, 0), lv_obj_get_style_text_letter_space(l, 0),
                    lv_obj_get_style_text_line_space(l, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > maxw) {                                     /* too wide: wrap it */
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, maxw);
    }
}

/* a photo's caption: its text after "[photo]" ("" = none) */
static const char *photo_caption(const char *text)
{
    if (!strncmp(text, "[photo]", 7)) text += 7;
    while (*text == ' ') text++;
    return text;
}

/* one message: a bubble, on the right (mine, accent colour) or the left */
static void add_bubble(const WaMsg *m, int maxw)
{
    lv_obj_t *row = th_box(W.scroller, -1);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m->mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *b = th_box(row, m->mine ? TS_BTN_ACCENT : TS_BTN);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 5, 0);
    lv_obj_set_style_pad_row(b, 2, 0);
    if (!m->mine && m->author[0]) {                        /* a group: who wrote it, small, accent colour */
        lv_obj_t *a = th_label(b, "", TS_SMALL);
        th_add(a, TS_ICON);                                /* (TS_ICON: the accent text colour) */
        artext_set_line(a, m->author, maxw - 20);
    }
    if (m->kind == KIND_PHOTO) {                           /* a photo: a picture sign + "Photo", then its caption */
        lv_obj_t *ph = lv_label_create(b);
        lv_label_set_text(ph, LV_SYMBOL_IMAGE "  Photo");
        if (!m->mine) th_add(ph, TS_ICON);                 /* (theirs: in the accent colour) */
        const char *cap = photo_caption(m->text);
        if (cap[0]) bubble_text(lv_label_create(b), cap, maxw - 20);
    } else if (m->kind == KIND_VOICE) {                    /* a voice message: a sound sign + "Voice message" */
        lv_obj_t *vo = lv_label_create(b);                 /* (its text, "[voice]", isn't shown) */
        lv_label_set_text(vo, LV_SYMBOL_AUDIO "  Voice message");
        if (!m->mine) th_add(vo, TS_ICON);                 /* (theirs: in the accent colour) */
    } else {
        lv_obj_t *t = lv_label_create(b);                  /* (colour from the bubble) */
        bubble_text(t, m->text, maxw - 20);
    }
    char when[24];
    when_text(m->when, when, sizeof when);
    lv_obj_t *tm = lv_label_create(b);                     /* the time, small */
    lv_label_set_text(tm, when);
    lv_obj_set_style_text_font(tm, theme()->font_small, 0);
    if (!m->mine) lv_obj_set_style_text_opa(tm, LV_OPA_70, 0);   /* (quieter; on the accent colour it stays solid to be readable) */
}

static void show_messages(void)
{
    int at_bottom = !W.chat_shown || lv_obj_get_scroll_bottom(W.scroller) <= 4;
    lv_coord_t y = lv_obj_get_scroll_y(W.scroller);
    lv_obj_clean(W.scroller);
    lv_obj_update_layout(W.scroller);
    int w = lv_obj_get_content_width(W.scroller);
    if (w <= 0) w = 600;
    int maxw = w * 3 / 4;                                  /* a bubble: at most 3/4 of the width */
    for (int i = 0; i < W.nmsgs; i++) add_bubble(&W.msgs[i], maxw);
    if (!W.nmsgs) th_label(W.scroller, "No messages yet. A: write one", TS_MUTED);
    lv_obj_update_layout(W.scroller);
    if (at_bottom) lv_obj_scroll_by(W.scroller, 0, -lv_obj_get_scroll_bottom(W.scroller), LV_ANIM_OFF);   /* newest at the bottom */
    else lv_obj_scroll_to_y(W.scroller, y, LV_ANIM_OFF);   /* (reading older ones: stay there) */
    W.chat_shown = 1;
}

static void chat_loaded(void)
{
    FILE *f = fopen(WA_OUT, "r");
    char *line = malloc(WA_LINE);
    unsigned long h = 5381;                                /* a fingerprint of the answer */
    if (f) { int c; while ((c = fgetc(f)) != EOF) h = h * 33 + (unsigned char)c; rewind(f); }
    if (W.chat_shown && h == W.read_hash) { free(line); if (f) fclose(f); set_status(""); return; }   /* nothing new */
    W.read_hash = h;
    free_msgs();
    while (f && line && next_line(f, line, WA_LINE)) {
        char *fl[6];
        int nf = split_tabs(line, fl, 6);
        if (nf < 4) continue;
        /* fields 5 and 6: the message's ID (checked: it goes into a command
         * line later) and its kind; an older PC sends neither */
        const char *id = nf >= 5 && safe_msg_id(fl[4]) ? fl[4] : "";
        int kind = nf >= 6 ? kind_of(fl[5]) : KIND_TEXT;
        if (W.nmsgs == MAX_MSGS) {                         /* full: the oldest one goes */
            free(W.msgs[0].text);
            memmove(&W.msgs[0], &W.msgs[1], (MAX_MSGS - 1) * sizeof W.msgs[0]);
            W.nmsgs--;
        }
        wa_unescape(fl[2]);
        wa_unescape(fl[3]);
        WaMsg *m = &W.msgs[W.nmsgs];
        size_t tl = strlen(fl[3]), il = strlen(id);
        m->text = malloc(tl + il + 2);                     /* the text, then the ID, in one block */
        if (!m->text) break;
        memcpy(m->text, fl[3], tl + 1);
        m->id = m->text + tl + 1;
        memcpy(m->id, id, il + 1);
        m->kind = kind;
        m->when = atol(fl[0]);
        m->mine = atoi(fl[1]) == 1;
        copy_text(m->author, sizeof m->author, fl[2]);
        W.nmsgs++;
    }
    free(line);
    if (f) fclose(f);
    show_messages();
    set_status("");
}

static void open_chat(int i, void *ctx)
{
    (void)ctx;
    if (i < 0 || i >= W.nchats || !safe_id(W.chats[i].id)) return;
    snprintf(W.chat_id, sizeof W.chat_id, "%s", W.chats[i].id);
    snprintf(W.chat_name_text, sizeof W.chat_name_text, "%s", W.chats[i].name);
    show_page(PG_CHAT);
    lv_obj_update_layout(W.chat_page);
    artext_set_line(W.chat_name, W.chat_name_text, lv_obj_get_content_width(W.chat_page) * 2 / 3);
    free_msgs();
    lv_obj_clean(W.scroller);
    W.chat_shown = 0;
    W.read_hash = 0;
    stop_loading();
    W.need_read = 1;
    set_status("Loading...");
    pump();
}

static void voice_stop(void);                              /* (further down) */

static void back_to_list(void)
{
    voice_stop();                                          /* leaving the chat: its voice message stops */
    if (W.job_kind == JOB_READ) stop_loading();
    free_msgs();                                           /* (only one chat's messages in RAM) */
    lv_obj_clean(W.scroller);
    W.chat_shown = 0;
    show_page(PG_CHATS);
    W.need_chats = 1;                                     /* (reading it changed its unread count) */
    set_status("");
    pump();
}

/* the reply was typed: send it (base64), then the chat is reloaded */
static void reply_typed(const char *text, void *ctx)
{
    (void)ctx;
    if (!text || !text[strspn(text, " ")] || W.page != PG_CHAT) return;
    if (W.job_kind == JOB_SEND) { ui_toast("Still sending the last message - try again in a moment"); return; }
    if (!safe_id(W.chat_id)) return;
    static char args[3000];
    char *b64 = malloc(strlen(text) * 4 / 3 + 8);
    if (!b64) return;
    wa_base64(text, b64, strlen(text) * 4 / 3 + 8);
    snprintf(args, sizeof args, "send '%s' %s", W.chat_id, b64);
    free(b64);
    stop_loading();                                        /* (a reload can wait) */
    if (start_job(JOB_SEND, args) == 0) set_status("Sending...");
}

static void ask_reply(void)
{
    char title[120];
    if (artext_has_arabic(W.chat_name_text)) snprintf(title, sizeof title, "Your message");   /* (pop-ups: no Arabic font) */
    else snprintf(title, sizeof title, "Message to %.60s", W.chat_name_text);
    nz_prompt(title, "", reply_typed, NULL);
}

/* ---------- Y in a chat: its photos, sending a picture ---------- */

/* a job that must not be stopped or doubled is running (sending, a photo loading) */
static int busy_sending(void)
{
    return W.job_kind == JOB_SEND || W.job_kind == JOB_SENDPHOTO || W.job_kind == JOB_PHOTO || W.job_kind == JOB_VOICE;
}

/* the name a chat's text gets in a pop-up (pop-ups have no Arabic font or emoji) */
static const char *popup_name(void)
{
    return artext_has_arabic(W.chat_name_text) || emoji_in_text(W.chat_name_text) ? "this chat" : W.chat_name_text;
}

/* page 3 (photos or pictures): its title, hints and what the rows are */
static void open_sub(int pg, const char *title, const char *const *hints, int nhints)
{
    lv_label_set_text(W.sub_title, title);
    hints_on(W.sub_hints, hints, nhints);
    nzlist_clear(W.sub_list);
    show_page(pg);
}

/* where a photo / voice message is kept: DIR/<the ID's start>_<a fingerprint>.EXT,
 * e.g. nzeus/whatsapp/photos/3EB0..._1a2b3c4d.jpg (only plain letters in the
 * name; the fingerprint of chat + ID keeps it unique) */
static void media_file(const WaMsg *m, const char *dir, const char *ext, char *out, size_t n)
{
    unsigned long h = 5381;
    for (const char *p = W.chat_id; *p; p++) h = h * 33 + (unsigned char)*p;
    for (const char *p = m->id; *p; p++) h = h * 33 + (unsigned char)*p;
    char safe[41];
    int k = 0;
    for (const char *p = m->id; *p && k < 40; p++) safe[k++] = isalnum((unsigned char)*p) ? *p : '_';
    safe[k] = 0;
    snprintf(out, n, "%s/%s_%08lx.%s", dir, safe, h & 0xFFFFFFFFUL, ext);
}

/* a folder of downloads: keep only its newest `keep` files (by date) */
typedef struct { long when; char name[96]; } Kept;
static int kept_cmp(const void *a, const void *b)
{
    long x = ((const Kept *)a)->when, y = ((const Kept *)b)->when;
    return x < y ? -1 : x > y;                             /* oldest first */
}

static void tidy_dir(const char *dir, int keep)
{
    DIR *d = opendir(dir);
    if (!d) return;
    Kept *k = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[300];
        struct stat st;
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof k->name) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {                                    /* grow the list */
            Kept *more = realloc(k, sizeof(Kept) * (size_t)(cap ? cap * 2 : 64));
            if (!more) break;
            k = more;
            cap = cap ? cap * 2 : 64;
        }
        k[n].when = (long)st.st_mtime;
        snprintf(k[n].name, sizeof k[n].name, "%s", e->d_name);
        n++;
    }
    closedir(d);
    if (n > keep) {
        qsort(k, (size_t)n, sizeof(Kept), kept_cmp);
        for (int i = 0; i < n - keep; i++) {               /* the oldest ones go */
            char p[300];
            snprintf(p, sizeof p, "%s/%s", dir, k[i].name);
            unlink(p);
        }
    }
    free(k);
}

/* the photo on screen goes (its 600 KB go back) */
static void free_photo(void)
{
    lv_img_set_src(W.view_img, NULL);
    lv_img_cache_invalidate_src(&W.dsc);
    picture_free(&W.pic);
    lv_obj_add_flag(W.view_img, LV_OBJ_FLAG_HIDDEN);
}

/* W.photo_path is on the card: decode it, fitted to the screen (aspect kept) */
static void show_photo(void)
{
    char err[96];
    free_photo();
    lv_obj_update_layout(W.view_page);
    int bw = lv_obj_get_content_width(W.view_page), bh = lv_obj_get_content_height(W.view_page);
    if (bw <= 0) bw = 640;
    if (bh <= 0) bh = 454;
    if (picture_load(W.photo_path, bw, bh, &W.pic, err, sizeof err) != 0) {   /* (picture.c, as the Images app) */
        unlink(W.photo_path);                              /* (a broken file: loaded again next time) */
        problem(err);
        return;
    }
    utime(W.photo_path, NULL);                             /* just looked at: kept longest by tidy_dir */
    memset(&W.dsc, 0, sizeof W.dsc);
    W.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    W.dsc.header.w = (uint32_t)W.pic.w;
    W.dsc.header.h = (uint32_t)W.pic.h;
    W.dsc.data_size = (uint32_t)(W.pic.w * W.pic.h * 2);
    W.dsc.data = (const uint8_t *)W.pic.px;
    lv_img_cache_invalidate_src(&W.dsc);
    lv_img_set_src(W.view_img, &W.dsc);
    /* a small photo is made bigger to fill the screen (a big one was already shrunk) */
    int zw = bw * 256 / W.pic.w, zh = bh * 256 / W.pic.h, z = zw < zh ? zw : zh;
    lv_img_set_zoom(W.view_img, (uint16_t)(z > 256 ? z : 256));
    lv_obj_center(W.view_img);
    lv_obj_clear_flag(W.view_img, LV_OBJ_FLAG_HIDDEN);
    set_status("");
}

/* A on a photo: show it (downloaded first if it isn't on the card yet) */
static void photo_chosen(int r)
{
    if (r < 0 || r >= W.nphotos) return;
    if (busy_sending()) { ui_toast("Busy - try again in a moment"); return; }
    const WaMsg *m = &W.msgs[W.photo_msg[r]];
    media_file(m, WA_PHOTOS, "jpg", W.photo_path, sizeof W.photo_path);
    char who[120], when[24];
    when_text(m->when, when, sizeof when);
    snprintf(who, sizeof who, "%s   %s", m->mine ? "You" : m->author[0] ? m->author : W.chat_name_text, when);
    show_page(PG_VIEW);
    artext_set_line(W.view_caption, who, 420);
    struct stat st;
    if (stat(W.photo_path, &st) == 0 && st.st_size > 0) { show_photo(); return; }   /* already saved: no download */
    mkdir(NZ_DIR, 0755);
    mkdir(WA_DIR, 0755);
    mkdir(WA_PHOTOS, 0755);
    char out[240], args[480];
    sh_quote(W.photo_path, out, sizeof out);
    snprintf(args, sizeof args, "photo '%s' '%s' %s", W.chat_id, m->id, out);   /* (both IDs checked) */
    stop_loading();
    if (start_job(JOB_PHOTO, args) == 0) set_status("Loading photo...");
}

/* "Photos in this chat": the photo messages, newest first */
static int count_photos(void)
{
    W.nphotos = 0;
    for (int i = W.nmsgs - 1; i >= 0; i--)
        if (W.msgs[i].kind == KIND_PHOTO && W.msgs[i].id[0]) W.photo_msg[W.nphotos++] = i;
    return W.nphotos;
}

static void open_photos(void)
{
    static const char *const h[] = { "A", "Show", "B", "Back" };
    if (!count_photos()) { ui_toast("No photos in this chat yet"); return; }
    open_sub(PG_PHOTOS, "Photos in this chat", h, 2);
    for (int r = 0; r < W.nphotos; r++) {
        const WaMsg *m = &W.msgs[W.photo_msg[r]];
        char when[24];
        when_text(m->when, when, sizeof when);
        nzlist_add(W.sub_list, ICON_IMAGE, "", "", when);
    }
    lv_obj_update_layout(nzlist_obj(W.sub_list));         /* the caption and who sent it: one line each */
    for (int r = 0; r < W.nphotos; r++) {
        const WaMsg *m = &W.msgs[W.photo_msg[r]];
        lv_obj_t *t = nzlist_title_obj(W.sub_list, r), *s = nzlist_sub_obj(W.sub_list, r);
        if (!t || !s) continue;
        int w = lv_obj_get_content_width(lv_obj_get_parent(t));
        const char *cap = photo_caption(m->text);
        artext_set_line(t, cap[0] ? cap : "Photo", w < 40 ? 40 : w);
        artext_set_line(s, m->mine ? "You" : m->author[0] ? m->author : W.chat_name_text, w < 40 ? 40 : w);
    }
    nzlist_select(W.sub_list, 0);
    char s[60];
    snprintf(s, sizeof s, "%d photo%s", W.nphotos, W.nphotos == 1 ? "" : "s");
    set_status(s);
}

/* "Send a picture": the JPG/PNG files on the card */
static int is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png"));
}

static void pick_scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && W.npicks < MAX_PICKS) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "emoji")) continue;   /* (1400 tiny emoji pictures: never) */
        int pic = is_picture(e->d_name);
        if (!pic && e->d_type == DT_REG) continue;         /* other files: not even looked at */
        char p[512];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof W.picks[0].path) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) pick_scan(p, depth - 1); continue; }
        if (!pic || !S_ISREG(st.st_mode)) continue;
        if (st.st_size > SEND_MAX) { W.nbig++; continue; } /* too big to send (the status line says so) */
        int dup = 0;                                       /* (the card ignores upper/lower case: Pictures = pictures) */
        for (int i = 0; i < W.npicks && !dup; i++) dup = !strcasecmp(W.picks[i].path, p);
        if (dup) continue;
        memcpy(W.picks[W.npicks].path, p, strlen(p) + 1);   /* (its length was checked above) */
        W.picks[W.npicks].when = (long)st.st_mtime;
        W.npicks++;
    }
    closedir(d);
}

static int pick_cmp(const void *a, const void *b)
{
    long x = ((const WaPick *)a)->when, y = ((const WaPick *)b)->when;
    return x < y ? 1 : x > y ? -1 : strcmp(((const WaPick *)a)->path, ((const WaPick *)b)->path);   /* newest first */
}

static void close_pick(void)
{
    free(W.picks);                                         /* (50 KB: only while the list is open) */
    W.picks = NULL;
    W.npicks = W.nbig = 0;
}

static void open_pick(void)
{
    static const char *const h[] = { "A", "Send", "B", "Back" };
    close_pick();
    W.picks = calloc(MAX_PICKS, sizeof W.picks[0]);
    if (!W.picks) { ui_toast("Not enough memory"); return; }
    /* the usual picture folders, 3 folders deep (nzeus/ has shots/ and the WhatsApp
     * photos); the card's root only by itself (not the games' folders) */
    pick_scan(NZ_SDCARD "/DCIM", 3);
    pick_scan(NZ_SDCARD "/Pictures", 3);
    pick_scan(NZ_SDCARD "/pictures", 3);
    pick_scan(NZ_DIR, 3);
    pick_scan(NZ_SDCARD, 0);
    qsort(W.picks, (size_t)W.npicks, sizeof W.picks[0], pick_cmp);
    open_sub(PG_PICK, "Send a picture", h, 2);
    for (int i = 0; i < W.npicks; i++) {
        const char *path = W.picks[i].path, *name = strrchr(path, '/');
        char folder[256], size[32] = "";
        snprintf(folder, sizeof folder, "%s", path);
        char *slash = strrchr(folder, '/');
        if (slash) *slash = 0;
        struct stat st;
        if (stat(path, &st) == 0) fs_size_label((unsigned long long)st.st_size, size, sizeof size);
        const char *shown = fs_path_contains(NZ_SDCARD, folder) ? folder + strlen(NZ_SDCARD) : folder;
        nzlist_add(W.sub_list, ICON_FILE_IMAGE, name ? name + 1 : path, shown[0] ? shown : "/", size);
    }
    if (!W.npicks) nzlist_add(W.sub_list, ICON_IMAGE, "No pictures found", "Put JPG or PNG files in DCIM or Pictures", NULL);
    nzlist_select(W.sub_list, 0);
    char s[80];
    if (W.nbig) snprintf(s, sizeof s, "%d picture%s  -  %d skipped (over 4 MB)", W.npicks, W.npicks == 1 ? "" : "s", W.nbig);
    else snprintf(s, sizeof s, "%d picture%s", W.npicks, W.npicks == 1 ? "" : "s");
    set_status(s);
}

/* "Send <file> to <chat>?" answered: send it in the background, back to the chat */
static void send_answer(int yes, void *ctx)
{
    (void)ctx;
    if (!yes || W.page != PG_PICK) return;
    if (busy_sending()) { ui_toast("Still sending - try again in a moment"); return; }
    if (!safe_id(W.chat_id)) return;
    char file[600], args[700];
    sh_quote(W.send_path, file, sizeof file);
    snprintf(args, sizeof args, "sendphoto '%s' %s", W.chat_id, file);
    close_pick();
    show_page(PG_CHAT);
    stop_loading();                                        /* (a reload can wait) */
    if (start_job(JOB_SENDPHOTO, args) == 0) set_status("Sending the photo...");
}

static void pick_chosen(int r)
{
    if (r < 0 || r >= W.npicks) return;                    /* ("No pictures found") */
    snprintf(W.send_path, sizeof W.send_path, "%s", W.picks[r].path);
    const char *name = strrchr(W.send_path, '/');
    char q[200];
    snprintf(q, sizeof q, "Send %.60s to %.60s?", name ? name + 1 : W.send_path, popup_name());
    nz_confirm(q, send_answer, NULL);
}

/* ---------- Y in a chat: its voice messages ----------
 * Played by the music engine (audio.c): it reads the WAV from the card a
 * little at a time, so a 2-minute message needs no more RAM than a short one. */

/* is the sound in the music engine our voice message? (Media may have taken it) */
static int voice_mine(void)
{
    return W.voice_row >= 0 && audio_loaded() && !strcmp(audio_path(), W.voice_file);
}

/* "1:05" */
static void mmss(double s, char *out, size_t n)
{
    int t = (int)(s + 0.5);
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

/* the voice page's status line and hints: playing / paused / nothing playing */
static void voice_show(void)
{
    static const char *const h_idle[] = { "A", "Play", "B", "Back" };
    static const char *const h_play[] = { "A", "Pause", "B", "Stop" };
    static const char *const h_pause[] = { "A", "Play", "B", "Stop" };
    int st = !voice_mine() ? 0 : audio_playing() ? 1 : 2;
    if (st != W.voice_state) {                             /* (the hints change only with the state) */
        hints_on(W.sub_hints, st == 1 ? h_play : st == 2 ? h_pause : h_idle, 2);
        W.voice_state = st;
    }
    char s[80], a[16], b[16];
    if (st) {
        mmss(audio_position(), a, sizeof a);
        mmss(audio_length(), b, sizeof b);
        snprintf(s, sizeof s, "%s  %s / %s", st == 1 ? "Playing..." : "Paused", a, b);
    } else snprintf(s, sizeof s, "%d voice message%s", W.nvoice, W.nvoice == 1 ? "" : "s");
    if (strcmp(s, W.last_status)) set_status(s);           /* (no redraw when nothing changed) */
}

/* stop our voice message (the music engine lets go of the file) */
static void voice_stop(void)
{
    if (voice_mine()) audio_close();
    W.voice_row = -1;
    if (W.page == PG_VOICE) voice_show();
}

/* W.voice_file is on the card: play it */
static void voice_play(void)
{
    char err[96];
    if (audio_open(W.voice_file, err, sizeof err) != 0) {  /* (audio.c, as the Media app) */
        unlink(W.voice_file);                              /* (a broken file: loaded again next time) */
        W.voice_row = -1;
        problem(err);
        return;
    }
    utime(W.voice_file, NULL);                             /* just played: kept longest by tidy_dir */
    W.voice_row = W.voice_want;
    audio_play(1);
    voice_show();
}

/* A on a voice message: play it (downloaded first if it isn't on the card yet);
 * A on the one playing: pause / go on */
static void voice_chosen(int r)
{
    if (r < 0 || r >= W.nvoice) return;
    if (r == W.voice_row && voice_mine()) {
        audio_play(!audio_playing());
        voice_show();
        return;
    }
    if (busy_sending()) { ui_toast("Busy - try again in a moment"); return; }
    voice_stop();
    const WaMsg *m = &W.msgs[W.voice_msg[r]];
    media_file(m, WA_VOICE, "wav", W.voice_file, sizeof W.voice_file);
    W.voice_want = r;
    struct stat st;
    if (stat(W.voice_file, &st) == 0 && st.st_size > 0) { voice_play(); return; }   /* already saved: no download */
    mkdir(NZ_DIR, 0755);
    mkdir(WA_DIR, 0755);
    mkdir(WA_VOICE, 0755);
    char out[240], args[480];
    sh_quote(W.voice_file, out, sizeof out);
    snprintf(args, sizeof args, "voice '%s' '%s' %s", W.chat_id, m->id, out);   /* (both IDs checked) */
    stop_loading();
    if (start_job(JOB_VOICE, args) == 0) set_status("Loading voice message...");
}

/* "Voice messages in this chat": newest first */
static int count_voice(void)
{
    W.nvoice = 0;
    for (int i = W.nmsgs - 1; i >= 0; i--)
        if (W.msgs[i].kind == KIND_VOICE && W.msgs[i].id[0]) W.voice_msg[W.nvoice++] = i;
    return W.nvoice;
}

static void open_voice(void)
{
    static const char *const h[] = { "A", "Play", "B", "Back" };
    if (!count_voice()) { ui_toast("No voice messages in this chat yet"); return; }
    open_sub(PG_VOICE, "Voice messages", h, 2);
    W.voice_state = 0;                                     /* (the hints just set: nothing playing) */
    W.voice_row = -1;
    for (int r = 0; r < W.nvoice; r++) {
        const WaMsg *m = &W.msgs[W.voice_msg[r]];
        char when[24];
        when_text(m->when, when, sizeof when);
        nzlist_add(W.sub_list, ICON_FILE_AUDIO, "", "", when);
    }
    lv_obj_update_layout(nzlist_obj(W.sub_list));         /* who sent it: one line */
    for (int r = 0; r < W.nvoice; r++) {
        const WaMsg *m = &W.msgs[W.voice_msg[r]];
        lv_obj_t *t = nzlist_title_obj(W.sub_list, r), *s = nzlist_sub_obj(W.sub_list, r);
        if (!t || !s) continue;
        int w = lv_obj_get_content_width(lv_obj_get_parent(t));
        lv_label_set_text(t, "Voice message");
        artext_set_line(s, m->mine ? "You" : m->author[0] ? m->author : W.chat_name_text, w < 40 ? 40 : w);
    }
    nzlist_select(W.sub_list, 0);
    voice_show();
}

/* A on a row of page 3 */
static void sub_activate(int i, void *ctx)
{
    (void)ctx;
    if (W.page == PG_PHOTOS) photo_chosen(i);
    else if (W.page == PG_PICK) pick_chosen(i);
    else if (W.page == PG_VOICE) voice_chosen(i);
}

/* B on page 3 or 4: one step back (the voice page: first B stops what plays) */
static void sub_back(void)
{
    if (W.page == PG_VOICE && W.voice_row >= 0) { voice_stop(); return; }
    if (W.job_kind == JOB_VOICE) { bgjob_stop(&W.job); W.job_kind = JOB_NONE; }   /* (stop loading it) */
    if (W.page == PG_VIEW) {
        if (W.job_kind == JOB_PHOTO) { bgjob_stop(&W.job); W.job_kind = JOB_NONE; }   /* (stop loading it) */
        free_photo();
        show_page(PG_PHOTOS);
        char s[60];
        snprintf(s, sizeof s, "%d photo%s", W.nphotos, W.nphotos == 1 ? "" : "s");
        set_status(s);
        return;
    }
    close_pick();
    show_page(PG_CHAT);
    set_status("");
}

static void photos_menu_chosen(int choice, void *ctx)
{
    (void)ctx;
    if (W.page != PG_CHAT) return;
    if (choice == 0) open_photos();
    else if (choice == 1) open_pick();
    else if (choice == 2) open_voice();
}

static void photos_menu(void)
{
    char first[48], third[48];
    snprintf(first, sizeof first, "Photos in this chat (%d)", count_photos());
    snprintf(third, sizeof third, "Voice messages (%d)", count_voice());
    const char *items[] = { first, "Send a picture", third };   /* (nz_menu copies the texts) */
    nz_menu("Photos and voice", items, 3, photos_menu_chosen, NULL);
}

/* ---------- the key + PIN ---------- */
static void begin(void);

static void forget_run_key(void) { unlink(WA_RUN_KEY); }

/* the right PIN: unlock the key into RAM */
static int unlock(const char *pin)
{
    forget_run_key();
    if (vault_decrypt_file(WA_KEY_ENC, WA_RUN_KEY, pin) != 0) { forget_run_key(); return -1; }
    chmod(WA_RUN_KEY, 0600);                               /* only NZEUS may read it */
    unlink(WA_KEY_PLAIN);                                  /* (never an unlocked key on the card) */
    return 0;
}

static void pin_for_open(const char *p, void *ctx)
{
    (void)ctx;
    if (!p) return;                                        /* cancelled: stays locked */
    if (!claude_pin_ok(p)) { ui_toast("Wrong PIN"); return; }
    if (unlock(p) != 0) { problem("Could not unlock the WhatsApp key"); return; }
    set_state(ST_READY);
    W.need_chats = 1;
    pump();
}

static void pin_for_setup(const char *p, void *ctx)
{
    (void)ctx;
    if (!p) return;
    if (!claude_pin_ok(p)) { ui_toast("Wrong PIN"); return; }
    snprintf(W.pin, sizeof W.pin, "%s", p);                /* kept until the key is made */
    mkdir(NZ_DIR, 0755);
    mkdir(WA_DIR, 0755);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "sh %s --make-key", WA_SCRIPT);
    unlink(WA_OUT);
    if (bgjob_start(&W.job, cmd, WA_OUT) != 0) { memset(W.pin, 0, sizeof W.pin); problem("Could not start nzeus-wa.sh"); return; }
    W.job_kind = JOB_MAKE_KEY;
    set_state(ST_MAKING_KEY);
}

/* --make-key finished: lock the new key with the PIN, delete the plain one */
static void key_made(void)
{
    char err[200];
    int ok = W.job.exit_code == 0 && access(WA_KEY_PLAIN, R_OK) == 0 &&
             vault_encrypt_file(WA_KEY_PLAIN, WA_KEY_ENC, W.pin) == 0;
    unlink(WA_KEY_PLAIN);                                  /* the card keeps only the locked copy */
    if (ok) ok = unlock(W.pin) == 0;
    memset(W.pin, 0, sizeof W.pin);
    if (!ok) {
        set_state(ST_LOCKED);
        problem(answer_error(err, sizeof err) ? err : "Could not make the WhatsApp key");
        return;
    }
    set_state(ST_SETUP);
}

/* what the app does first (and A on "Locked") */
static void begin(void)
{
    if (access(WA_KEY_ENC, R_OK) != 0) {                   /* first use: make the key */
        if (!settings_get("claude_pin", NULL)) {
            set_state(ST_NO_PIN);
            ui_toast("Choose a PIN in the Claude app first");
            return;
        }
        set_state(ST_LOCKED);
        nz_prompt("WhatsApp setup: your PIN (the Claude app's PIN)", "", pin_for_setup, NULL);
        return;
    }
    set_state(ST_LOCKED);
    nz_prompt("WhatsApp: your PIN (the Claude app's PIN)", "", pin_for_open, NULL);
}

/* The Claude app's PIN changed: lock the WhatsApp key with the new one too.
 * 0 = done (or there is no key yet). */
int whatsapp_relock(const char *old_pin, const char *new_pin)
{
    const char *tmp = "/tmp/nzeus-wa-relock";
    if (access(WA_KEY_ENC, R_OK) != 0) return 0;
    unlink(tmp);
    int rc = vault_decrypt_file(WA_KEY_ENC, tmp, old_pin) == 0 ? vault_encrypt_file(tmp, WA_KEY_ENC, new_pin) : -1;
    unlink(tmp);
    return rc;
}

/* ---------- the job finished ---------- */
static void job_done(void)
{
    int kind = W.job_kind, code = W.job.exit_code;
    char err[200];
    W.job_kind = JOB_NONE;
    if (kind == JOB_MAKE_KEY) { key_made(); return; }
    if (kind == JOB_CHATS) W.chats_at = lv_tick_get();
    if (kind == JOB_READ) W.read_at = lv_tick_get();
    /* the page the answer belongs to may be gone (B was pressed meanwhile) */
    if ((kind == JOB_READ && W.page != PG_CHAT) || (kind == JOB_CHATS && W.page != PG_CHATS) ||
        (kind == JOB_PHOTO && W.page != PG_VIEW) || (kind == JOB_VOICE && W.page != PG_VOICE)) return;
    if (answer_error(err, sizeof err)) { problem(err); return; }      /* the PC said what is wrong */
    if (kind == JOB_SEND || kind == JOB_SENDPHOTO || kind == JOB_PHOTO || kind == JOB_VOICE) {
        char line[16] = "";
        FILE *f = fopen(WA_OUT, "r");
        if (f) { next_line(f, line, sizeof line); fclose(f); }
        if (code != 0 || strcmp(line, "ok")) { problem(NO_PC); return; }
        if (kind == JOB_PHOTO) {                           /* the photo is on the card now */
            tidy_dir(WA_PHOTOS, WA_KEEP_PHOTOS);
            show_photo();
            return;
        }
        if (kind == JOB_VOICE) {                           /* the voice message is on the card now */
            tidy_dir(WA_VOICE, WA_KEEP_VOICE);
            voice_play();
            return;
        }
        if (kind == JOB_SENDPHOTO) ui_toast("Photo sent");
        set_status(kind == JOB_SENDPHOTO ? "Photo sent" : "Sent");
        W.need_read = W.need_chats = 1;                    /* show it in the chat (and the list later) */
        return;
    }
    if (code != 0) { problem(NO_PC); return; }             /* no answer at all */
    if (kind == JOB_CHATS) chats_loaded();
    else chat_loaded();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (W.job_kind != JOB_NONE && !bgjob_poll(&W.job)) job_done();
    if (W.voice_row >= 0) {                                /* a voice message is loaded: */
        if (!voice_mine() || audio_finished()) voice_stop();   /* at its end (or Media took the speaker): stop */
        else if (W.page == PG_VOICE) voice_show();         /* playing: the time goes on */
    }
    if (W.state != ST_READY) return;
    if (W.page == PG_CHAT && lv_tick_elaps(W.read_at) >= (uint32_t)wa_reload_chat_ms) W.need_read = 1;
    if (W.page == PG_CHATS && lv_tick_elaps(W.chats_at) >= (uint32_t)wa_reload_list_ms) W.need_chats = 1;
    pump();
}

/* ---------- the app ---------- */
static lv_obj_t *page_box(lv_obj_t *root)
{
    lv_obj_t *p = th_box(root, -1);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    return p;
}

static lv_obj_t *top_bar(lv_obj_t *page)                   /* a thin bar at the top */
{
    lv_obj_t *bar = th_box(page, TS_PANEL);
    lv_obj_set_size(bar, LV_PCT(100), 30);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    return bar;
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&W, 0, sizeof W);
    W.voice_row = -1;                                      /* (no voice message playing) */
    th_add(root, TS_SCREEN);
    emoji_look_again();                                    /* (the emoji pack may be new on the card) */
    W.chats = calloc(MAX_CHATS, sizeof W.chats[0]);

    W.chats_page = page_box(root);                         /* page 1: the chats */
    lv_obj_t *bar = top_bar(W.chats_page);
    W.status = th_label(bar, "", TS_SMALL);
    lv_label_set_long_mode(W.status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(W.status, LV_PCT(96));
    lv_obj_align(W.status, LV_ALIGN_LEFT_MID, 10, 0);
    W.list = nzlist_create(W.chats_page, 0);
    lv_obj_set_width(nzlist_obj(W.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(W.list), 1);
    nzlist_on_activate(W.list, open_chat, NULL);
    W.info = th_label(W.chats_page, "", TS_TEXT);          /* (instead of the list until the key is open) */
    lv_label_set_long_mode(W.info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(W.info, LV_PCT(100));
    lv_obj_set_flex_grow(W.info, 1);
    lv_obj_set_style_text_align(W.info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(W.info, 90, 0);
    lv_obj_set_style_pad_hor(W.info, 30, 0);
    W.hint_bar = th_box(W.chats_page, TS_BAR);
    lv_obj_set_size(W.hint_bar, LV_PCT(100), 26);

    W.chat_page = page_box(root);                          /* page 2: one chat */
    bar = top_bar(W.chat_page);
    W.chat_name = th_label(bar, "", TS_TITLE);
    lv_obj_align(W.chat_name, LV_ALIGN_LEFT_MID, 10, 0);
    W.chat_status = th_label(bar, "", TS_SMALL);
    lv_obj_align(W.chat_status, LV_ALIGN_RIGHT_MID, -10, 0);
    W.scroller = th_box(W.chat_page, -1);
    lv_obj_add_flag(W.scroller, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(W.scroller, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_width(W.scroller, LV_PCT(100));
    lv_obj_set_flex_grow(W.scroller, 1);
    lv_obj_set_flex_flow(W.scroller, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(W.scroller, 10, 0);
    lv_obj_set_style_pad_row(W.scroller, 6, 0);
    lv_obj_t *h = th_box(W.chat_page, TS_BAR);
    lv_obj_set_size(h, LV_PCT(100), 26);
    static const char *const chat_hints[] = { "UP/DOWN", "Scroll", "A", "Reply", "Y", "Photos, voice", "B", "Chats" };
    lv_obj_center(nz_hints(h, chat_hints, 4));
    lv_obj_add_flag(W.chat_page, LV_OBJ_FLAG_HIDDEN);

    W.sub_page = page_box(root);                           /* page 3: a chat's photos / pictures to send */
    bar = top_bar(W.sub_page);
    W.sub_title = th_label(bar, "", TS_TITLE);
    lv_obj_align(W.sub_title, LV_ALIGN_LEFT_MID, 10, 0);
    W.sub_status = th_label(bar, "", TS_SMALL);
    lv_obj_align(W.sub_status, LV_ALIGN_RIGHT_MID, -10, 0);
    W.sub_list = nzlist_create(W.sub_page, 0);
    lv_obj_set_width(nzlist_obj(W.sub_list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(W.sub_list), 1);
    nzlist_on_activate(W.sub_list, sub_activate, NULL);
    W.sub_hints = th_box(W.sub_page, TS_BAR);
    lv_obj_set_size(W.sub_hints, LV_PCT(100), 26);
    lv_obj_add_flag(W.sub_page, LV_OBJ_FLAG_HIDDEN);

    W.view_page = th_box(root, -1);                        /* page 4: one photo, full screen (as the Images app) */
    lv_obj_set_size(W.view_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(W.view_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(W.view_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_bottom(W.view_page, 26, 0);       /* room for the caption */
    W.view_img = lv_img_create(W.view_page);
    lv_img_set_antialias(W.view_img, false);               /* (zoom by whole pixels: fast) */
    lv_obj_add_flag(W.view_img, LV_OBJ_FLAG_HIDDEN);
    W.view_msg = th_label(W.view_page, "", TS_MUTED);      /* "Loading photo...", or what went wrong */
    lv_obj_set_style_text_align(W.view_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(W.view_msg);
    lv_obj_t *cap = th_box(W.view_page, TS_BAR);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(cap, LV_PCT(100), 26);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 26);
    W.view_caption = th_label(cap, "", TS_SMALL);          /* who sent it, when */
    lv_obj_align(W.view_caption, LV_ALIGN_LEFT_MID, 10, 0);
    static const char *const view_hints[] = { "B", "Back" };
    lv_obj_align(nz_hints(cap, view_hints, 1), LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(W.view_page, LV_OBJ_FLAG_HIDDEN);

    W.timer = lv_timer_create(tick, 200, NULL);
    if (!W.chats) { set_state(ST_LOCKED); problem("Not enough memory"); return; }
    begin();
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (W.page == PG_VIEW) { if (k == NZ_KEY_B) sub_back(); return 1; }
    if (W.page == PG_PHOTOS || W.page == PG_PICK || W.page == PG_VOICE) {
        if (k == NZ_KEY_B) sub_back();
        else nzlist_key(W.sub_list, k);
        return 1;
    }
    if (W.page == PG_CHAT) {
        int page = lv_obj_get_height(W.scroller) - 40;
        switch (k) {
        case NZ_KEY_UP:   lv_obj_scroll_by(W.scroller, 0, 60, LV_ANIM_OFF); return 1;
        case NZ_KEY_DOWN: lv_obj_scroll_by(W.scroller, 0, -60, LV_ANIM_OFF); return 1;
        case NZ_KEY_L:    lv_obj_scroll_by(W.scroller, 0, page, LV_ANIM_OFF); return 1;
        case NZ_KEY_R:    lv_obj_scroll_by(W.scroller, 0, -page, LV_ANIM_OFF); return 1;
        case NZ_KEY_A: case NZ_KEY_X: ask_reply(); return 1;
        case NZ_KEY_Y:    photos_menu(); return 1;
        case NZ_KEY_B:    back_to_list(); return 1;
        default: return 1;
        }
    }
    if (k == NZ_KEY_B) return 0;                           /* B on the chats: the app closes */
    if (W.state == ST_LOCKED && k == NZ_KEY_A) { begin(); return 1; }
    if (W.state == ST_SETUP && k == NZ_KEY_A) { set_state(ST_READY); W.need_chats = 1; pump(); return 1; }
    if (W.state != ST_READY) return 1;
    if (k == NZ_KEY_X) {                                   /* refresh now */
        if (W.job_kind == JOB_NONE) { W.need_chats = 1; pump(); }
        return 1;
    }
    return nzlist_key(W.list, k);
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    (void)a;
    if (W.page == PG_CHAT && ev->key == KB_ENTER) ask_reply();   /* a USB keyboard: Enter = write */
}

static void destroy(NzApp *a)
{
    (void)a;
    bgjob_stop(&W.job);
    if (W.timer) lv_timer_del(W.timer);
    if (voice_mine()) audio_close();                       /* leaving the app: the voice message stops */
    forget_run_key();                                      /* the unlocked key leaves RAM */
    unlink(WA_KEY_PLAIN);
    free_msgs();
    free(W.chats);
    if (W.view_img) free_photo();                          /* (the photo's pixels) */
    close_pick();
    nzlist_free(W.list);
    nzlist_free(W.sub_list);
    memset(&W, 0, sizeof W);                               /* (the PIN too) */
}

/* for tests */
int app_whatsapp_state(void) { return W.state; }
int app_whatsapp_page(void) { return W.page; }
int app_whatsapp_busy(void) { return W.job_kind != JOB_NONE; }
int app_whatsapp_chat_count(void) { return W.nchats; }
const char *app_whatsapp_chat_name(int i) { return i >= 0 && i < W.nchats ? W.chats[i].name : ""; }
const char *app_whatsapp_chat_last(int i) { return i >= 0 && i < W.nchats ? W.chats[i].last : ""; }
int app_whatsapp_chat_unread(int i) { return i >= 0 && i < W.nchats ? W.chats[i].unread : -1; }
int app_whatsapp_msg_count(void) { return W.nmsgs; }
const char *app_whatsapp_msg_text(int i) { return i >= 0 && i < W.nmsgs ? W.msgs[i].text : ""; }
const char *app_whatsapp_msg_author(int i) { return i >= 0 && i < W.nmsgs ? W.msgs[i].author : ""; }
int app_whatsapp_msg_mine(int i) { return i >= 0 && i < W.nmsgs ? W.msgs[i].mine : -1; }
int app_whatsapp_msg_kind(int i) { return i >= 0 && i < W.nmsgs ? W.msgs[i].kind : -1; }   /* 0 text, 1 photo, 2 video, 3 voice ... */
const char *app_whatsapp_msg_id(int i) { return i >= 0 && i < W.nmsgs ? W.msgs[i].id : ""; }
int app_whatsapp_photo_count(void) { return W.nphotos; }
int app_whatsapp_photo_w(void) { return W.pic.px ? W.pic.w : 0; }   /* the photo on screen (0 = none / freed) */
int app_whatsapp_pick_count(void) { return W.npicks; }
int app_whatsapp_voice_count(void) { return W.nvoice; }
int app_whatsapp_voice_playing(void) { return voice_mine(); }   /* our voice message is in the music engine */
NzList *app_whatsapp_sub_list(void) { return W.sub_list; }
lv_obj_t *app_whatsapp_chat_title(void) { return W.chat_name; }
lv_obj_t *app_whatsapp_view_img(void) { return W.view_img; }
const char *app_whatsapp_status(void) { return W.last_status; }
lv_obj_t *app_whatsapp_scroller(void) { return W.scroller; }
NzList *app_whatsapp_list(void) { return W.list; }

NzApp app_whatsapp = {
    .id = APP_WHATSAPP, .title = "WhatsApp", .icon = ICON_CHAT,
    .create = create, .key = key, .kbd = kbd, .destroy = destroy,
};

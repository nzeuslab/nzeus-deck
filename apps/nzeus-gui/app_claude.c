/* app_claude.c - the NZEUS Claude app.
 *
 * Claude Code itself can't run on this console (MIPS CPU, little RAM). It runs
 * on a "brain" - the owner's home PC, or their phone - logged in with their own Claude
 * subscription, and this app shows it, full screen, over SSH
 * (R36S/brain/README.md has the whole picture):
 *   console --ssh -t--> brain: runs `claude` in its nzeus-claude folder
 * (nzeus/bin/nzeus-claude-connect.sh, core/deck-modern). There is no
 * tunnel back: Claude chats, but does not reach the console this way.
 *
 * Page 1, Connect: which brain, the network state, keys, PIN, Connect.
 * Page 2, connected: two views of the SAME connection (only the drawing differs):
 *  a) the chat view (shown first): the conversation as bubbles, like a
 *     messaging app - Claude's text on the left, what you typed on the right.
 *     It is read from the terminal's text (screen + kept lines), so it is a
 *     best-effort tidy copy of Claude Code's screen, not a real chat: menus
 *     (e.g. /resume) look odd there - Y shows the full terminal.
 *       X type a message (sent with Enter)   A Enter   B Esc   Y the terminal
 *       UP / DOWN, L1 / R1   scroll (it starts at the newest message)
 *  b) the terminal (80 columns): Claude Code's own screen.
 *       A Enter   B Esc   Y Ctrl+C   D-pad arrows   X on-screen keyboard
 *       L1 / R1   scroll back a page / forward a page through earlier messages;
 *                 while scrolled back UP / DOWN move one line, any other button
 *                 goes back to the live screen (and is not sent).
 *                 R1 on the live screen: back to the chat view.
 *  START     menu: Disconnect, Old chats (/resume), Show the chat / terminal, Tab...
 *  A USB keyboard works fully (Ctrl+letters, F-keys). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "artext.h"
#include "bgjob.h"
#include "home.h"
#include "hw.h"
#include "icons.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "sha256.h"
#include "theme.h"
#include "ui.h"
#include "vtwidget.h"
#include "wifi.h"
#include "keyvault.h"
#include <sys/stat.h>

#define CLAUDE_DIR NZ_DIR "/claude"
#define KEY_VAULT  CLAUDE_DIR "/console_key.enc"    /* the console's SSH key, locked with the PIN */
#define KEY_PUB    CLAUDE_DIR "/console_key.pub"    /* its public half: goes to the brain */
#define KEY_PLAIN  CLAUDE_DIR "/console_key"        /* (an old unlocked key: locked away, then deleted) */
#define RUN_DIR    "/tmp/nzeus-run"                 /* RAM only, gone at power off */
#define RUN_KEY    RUN_DIR "/console_key"
enum { ROW_BRAIN, ROW_ADDRESS, ROW_NETWORK, ROW_KEYS, ROW_PIN, ROW_CONNECT, ROW_COUNT };
static const char *const brain_names[] = { "Home PC", "Phone" };

/* the chat view (see "the chat view" below) */
#define CHAT_MAX   40                      /* bubbles at most (the newest): few LVGL objects */
#define CHAT_LINES 24                      /* a longer message goes on in a new bubble */
#define CHAT_TEXT  4096                    /* one bubble's text at most (bytes) */
#define CHAT_EVERY 12                      /* redrawn at most every 12 timer ticks (~0.4 s) */
enum { WHO_CLAUDE, WHO_ME };
typedef struct { int first, last, who; } Bubble;   /* terminal lines first..last, who wrote them */

static struct {
    lv_obj_t *root, *connect_page, *term_page, *vt, *keys_row, *kb, *kb_ta;
    lv_obj_t *marker;                      /* "SCROLLED BACK ..." while showing earlier lines */
    NzList *list;
    lv_timer_t *timer;
    int connected, rows, cols;             /* rows, cols: the terminal's size in text rows / columns */
    /* the chat view: the same connection, drawn as bubbles */
    lv_obj_t *chat_page, *chat_list;       /* the page, its scrolling list of bubbles */
    lv_obj_t *chat_empty;                  /* "Waiting for Claude Code..." while there are none */
    lv_obj_t *chat_label[CHAT_MAX];        /* the text label of each bubble shown */
    uint32_t chat_hash[CHAT_MAX];          /* a fingerprint of each one's text */
    int chat_who[CHAT_MAX], nshown;        /* who wrote each one, how many are shown */
    int chat_mode;                         /* 1 = the chat view is shown, 0 = the terminal */
    int chat_follow;                       /* 1 = stay at the newest message (bottom) */
    int chat_tick, enter_in;               /* ticks since the last redraw; ticks until a delayed Enter */
    unsigned long chat_seen;               /* vtw_changes() at the last redraw */
} K;
const char *claude_connect_script = NZ_DIR "/bin/nzeus-claude-connect.sh";   /* tests change it */

/* ---------- settings ---------- */
static int brain(void) { return settings_get_int("claude_brain", 0) ? 1 : 0; }
static const char *host(void) { return brain() ? settings_get("claude_phone_host", "192.168.43.1") : settings_get("claude_pc_host", ""); }
static const char *port(void) { return brain() ? settings_get("claude_phone_port", "8022") : settings_get("claude_pc_port", "22"); }
static const char *user(void) { return brain() ? settings_get("claude_phone_user", "nzeus") : settings_get("claude_pc_user", ""); }

/* The kernel always has TCP/IP: what matters is whether a network with a
 * way out (a default route) is up. */
static int have_tcp(void) { return hw_modern_online(); }
#define NO_INTERNET_TEXT "No network yet: Settings > USB > Devices, plug in the Wi-Fi dongle, join Wi-Fi"

/* ---------- PIN (kept only as a salted hash) ---------- */
static void pin_hash(const char *pin, const char *salt, char out[65])
{
    char buf[96];
    snprintf(buf, sizeof buf, "%s:%s", salt, pin);
    sha256_hex(buf, strlen(buf), out);
}

int claude_pin_ok(const char *pin)
{
    char h[65];
    pin_hash(pin, settings_get("claude_salt", ""), h);
    return !strcmp(h, settings_get("claude_pin", "-"));
}

void claude_pin_set(const char *pin)
{
    char salt[17], h[65];
    snprintf(salt, sizeof salt, "%08lx%08lx", (unsigned long)time(NULL), (unsigned long)rand() ^ (unsigned long)getpid());
    pin_hash(pin, salt, h);
    settings_set("claude_salt", salt);
    settings_set("claude_pin", h);
}

/* ---------- the console's SSH key, kept locked with the PIN ---------- */
const char *claude_dropbear = NZ_DIR "/bin/dropbearmulti";   /* tests change it */

static void forget_run_key(void) { unlink(RUN_KEY); }

/* Lock the key in RUN_KEY with the PIN, write its public half, delete RUN_KEY. */
static int lock_run_key(const char *pin)
{
    char cmd[400];
    mkdir(NZ_DIR, 0755);
    mkdir(CLAUDE_DIR, 0755);
    snprintf(cmd, sizeof cmd, "%s dropbearkey -y -f %s 2>/dev/null | awk '/^(ssh-|ecdsa-)/ { print $1 \" \" $2 \" nzeus-console\" }' > %s",
             claude_dropbear, RUN_KEY, KEY_PUB);
    if (system(cmd) != 0) { forget_run_key(); return -1; }
    int rc = vault_encrypt_file(RUN_KEY, KEY_VAULT, pin);
    forget_run_key();
    return rc;
}

/* First PIN: make the key (or lock away an old unlocked one). */
static int make_key(const char *pin)
{
    char cmd[300];
    mkdir(RUN_DIR, 0700);
    forget_run_key();
    if (access(KEY_PLAIN, R_OK) == 0) {                    /* made earlier with --make-key */
        snprintf(cmd, sizeof cmd, "cp %s %s", KEY_PLAIN, RUN_KEY);
        if (system(cmd) != 0) return -1;
        chmod(RUN_KEY, 0600);
        if (lock_run_key(pin) != 0) return -1;
        unlink(KEY_PLAIN);                                 /* the card keeps only the locked copy */
        return 0;
    }
    snprintf(cmd, sizeof cmd, "%s dropbearkey -t ed25519 -f %s >/dev/null 2>&1", claude_dropbear, RUN_KEY);
    if (system(cmd) != 0 || access(RUN_KEY, R_OK) != 0) return -1;
    return lock_run_key(pin);
}

static int pin_valid(const char *p)
{
    size_t n = strlen(p);
    if (n < 4 || n > 8) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

/* ---------- the connect page ---------- */
static void fill_rows(void)
{
    char v[96];
    int sel = nzlist_selected(K.list);
    nzlist_clear(K.list);
    nzlist_add(K.list, ICON_CLAUDE, "Brain", "where Claude Code runs (A changes it)", brain_names[brain()]);
    if (!host()[0] || !user()[0]) snprintf(v, sizeof v, "not set yet (A sets it)");
    else snprintf(v, sizeof v, "%s : %s  (user %s)", host(), port(), user());
    nzlist_add(K.list, ICON_DESKTOP, "Address", "A changes it", v);
    nzlist_add(K.list, ICON_WIFI, "Network", have_tcp() ? "Wi-Fi + TCP/IP" : "join Wi-Fi first (Wi-Fi app), then the internet starts",
               have_tcp() ? "ready" : "no internet yet");
    nzlist_add(K.list, ICON_KEYBOARD, "Keys and setup", "what to copy to the brain, once", NULL);
    nzlist_add(K.list, ICON_LOCK, settings_get("claude_pin", NULL) ? "Change PIN" : "Choose a PIN", "asked before every connection", NULL);
    nzlist_add(K.list, ICON_TERMINAL, "Connect", "opens Claude Code from the brain", NULL);
    if (sel >= 0) nzlist_select(K.list, sel);
}

static char new_pin[16], old_pin[16];
int whatsapp_relock(const char *old_pin, const char *new_pin);   /* app_whatsapp.c */
static void pin_again(const char *p, void *ctx)
{
    (void)ctx;
    if (!p) return;
    if (strcmp(p, new_pin)) { ui_toast("The two PINs were different - nothing changed"); return; }
    /* the WhatsApp app's key is locked with this PIN too (app_whatsapp.c) */
    if (old_pin[0] && whatsapp_relock(old_pin, p) != 0) { ui_toast("Could not re-lock the WhatsApp key - PIN not changed"); return; }
    if (old_pin[0] && access(KEY_VAULT, R_OK) == 0) {      /* changing the PIN: lock the key with the new one */
        mkdir(RUN_DIR, 0700);
        if (vault_decrypt_file(KEY_VAULT, RUN_KEY, old_pin) != 0 || vault_encrypt_file(RUN_KEY, KEY_VAULT, p) != 0) {
            forget_run_key();
            whatsapp_relock(p, old_pin);                   /* (the WhatsApp key back to the old PIN) */
            ui_toast("Could not re-lock the key - PIN not changed");
            return;
        }
        forget_run_key();
    }
    claude_pin_set(p);
    memset(old_pin, 0, sizeof old_pin);
    if (access(KEY_VAULT, R_OK) != 0)                      /* first PIN: make the console's key now */
        ui_toast(make_key(p) == 0 ? "PIN saved, key made (copy console_key.pub to the brain)" : "PIN saved (the key is made later: dropbear is missing)");
    else ui_toast("PIN saved");
    fill_rows();
}
static void pin_first(const char *p, void *ctx)
{
    (void)ctx;
    if (!p) return;
    if (!pin_valid(p)) { ui_toast("Use 4 to 8 digits"); return; }
    snprintf(new_pin, sizeof new_pin, "%s", p);
    nz_prompt("Type the new PIN again", "", pin_again, NULL);
}
static void pin_old_checked(const char *p, void *ctx)
{
    (void)ctx;
    if (!p) return;
    if (!claude_pin_ok(p)) { ui_toast("Wrong PIN"); return; }
    snprintf(old_pin, sizeof old_pin, "%s", p);
    nz_prompt("New PIN (4-8 digits)", "", pin_first, NULL);
}

static void open_terminal(void);
static void pin_for_connect(const char *p, void *ctx)
{
    (void)ctx;
    if (!p) return;
    if (!claude_pin_ok(p)) { ui_toast("Wrong PIN"); return; }
    mkdir(RUN_DIR, 0700);
    if (access(KEY_VAULT, R_OK) != 0 && make_key(p) != 0) { ui_toast("Could not make the console's key"); return; }
    if (vault_decrypt_file(KEY_VAULT, RUN_KEY, p) != 0) { ui_toast("Could not unlock the key"); return; }
    setenv("NZ_CONSOLE_KEY", RUN_KEY, 1);                  /* nzeus-claude-connect.sh uses it */
    open_terminal();
}

static char edit_buf[64];
static void address_done(const char *t, void *ctx)
{
    int what = (int)(intptr_t)ctx;                 /* 0 host, 1 port, 2 user */
    if (!t || !t[0]) return;
    static const char *const pc_keys[] = { "claude_pc_host", "claude_pc_port", "claude_pc_user" };
    static const char *const ph_keys[] = { "claude_phone_host", "claude_phone_port", "claude_phone_user" };
    settings_set(brain() ? ph_keys[what] : pc_keys[what], t);
    fill_rows();
}
static void address_menu(int c, void *ctx)
{
    (void)ctx;
    if (c < 0) return;
    const char *now = c == 0 ? host() : c == 1 ? port() : user();
    snprintf(edit_buf, sizeof edit_buf, "%s", now);
    static const char *const titles[] = { "Address (IP or name)", "Port", "User name" };
    nz_prompt(titles[c], edit_buf, address_done, (void *)(intptr_t)c);
}

static void show_keys(void)
{
    static char text[700];
    char pub[400] = "";
    FILE *f = fopen(CLAUDE_DIR "/console_key.pub", "r");
    if (f) { if (!fgets(pub, sizeof pub, f)) pub[0] = 0; fclose(f); }
    pub[strcspn(pub, "\r\n")] = 0;
    /* SD Debian: no tunnel back, so no brain key here (CLAUDE-SSH.md) */
    snprintf(text, sizeof text,
             "1. On the brain, run its setup once (R36S/brain/%s).\n"
             "2. Choose a PIN here: it makes this console's key\n   /data/nzeus/deck/nzeus/claude/console_key.pub %s\n"
             "3. Copy that key to the brain, ending its line in\n   nzeus-console (R36S/linux-port/deck-modern/CLAUDE-SSH.md).\n"
             "4. Connect.",
             brain() ? "android/setup.sh" : "pc/setup_pc_ssh.ps1",
             pub[0] ? "(made)" : "(not made yet)");
    static const char *const ok[] = { "OK" };
    nz_menu(text, ok, 1, NULL, NULL);
}

static void row_activated(int i, void *ctx)
{
    (void)ctx;
    switch (i) {
    case ROW_BRAIN: settings_set_int("claude_brain", !brain()); fill_rows(); break;
    case ROW_ADDRESS: {
        static const char *const items[] = { "Address", "Port", "User name" };
        nz_menu("Brain address", items, 3, address_menu, NULL);
        break;
    }
    case ROW_KEYS: show_keys(); break;
    case ROW_PIN:
        if (settings_get("claude_pin", NULL)) nz_prompt("Current PIN", "", pin_old_checked, NULL);
        else nz_prompt("Choose a PIN (4-8 digits)", "", pin_first, NULL);
        break;
    case ROW_CONNECT:
        if (!settings_get("claude_pin", NULL)) { ui_toast("Choose a PIN first"); break; }
        if (!have_tcp()) { ui_toast(NO_INTERNET_TEXT); break; }
        nz_prompt("PIN", "", pin_for_connect, NULL);
        break;
    default: break;
    }
}

/* ---------- the terminal page ---------- */
static void show_view(int chat);                          /* (the chat view: further down) */
static void chat_forget(void);

static void send_key(int key, int ch, int mods) { if (K.vt) vtw_key(K.vt, key, ch, mods); }

typedef struct { const char *label; int key, ch, mods; } Shortcut;
/* (Tab and Shift+Tab are in the START menu: the row has no room for them) */
static const Shortcut shortcuts[] = {
    { "Enter", KB_ENTER, 0, 0 }, { "Esc", KB_ESC, 0, 0 },
    { LV_SYMBOL_UP, KB_UP, 0, 0 }, { LV_SYMBOL_DOWN, KB_DOWN, 0, 0 }, { LV_SYMBOL_LEFT, KB_LEFT, 0, 0 },
    { LV_SYMBOL_RIGHT, KB_RIGHT, 0, 0 }, { "Ctrl+C", KB_CHAR, 'c', VTW_MOD_CTRL },
};

static void shortcut_clicked(lv_event_t *e)
{
    const Shortcut *s = lv_event_get_user_data(e);
    send_key(s->key, s->ch, s->mods);
}

static void close_terminal(void)
{
    if (K.vt) { lv_obj_del(K.vt); K.vt = NULL; }             /* (stops dbclient) */
    if (K.marker) lv_obj_add_flag(K.marker, LV_OBJ_FLAG_HIDDEN);
    forget_run_key();                                         /* the unlocked key leaves RAM too */
    K.connected = 0;
    ui_set_badge(NULL);
    chat_forget();                                            /* (the bubbles go too) */
    K.chat_mode = 0;
    K.enter_in = 0;
    lv_obj_add_flag(K.chat_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(K.term_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(K.connect_page, LV_OBJ_FLAG_HIDDEN);
    fill_rows();
}

static void open_terminal(void)
{
    if (!host()[0] || !user()[0]) { ui_toast("Set the brain's address and user first (the Address row)"); return; }
    lv_obj_add_flag(K.connect_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(K.term_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(K.term_page);
    int rows = (lv_obj_get_content_height(K.term_page) - 30) / VTW_CELL_H;
    int cols = lv_obj_get_content_width(K.term_page) / VTW_CELL_W;
    if (rows > 30) rows = 30;
    if (cols > 80) cols = 80;
    K.rows = rows;
    K.cols = cols;
    K.vt = vtw_create(K.term_page, cols, rows);
    if (!K.vt) { ui_toast("Not enough memory for the terminal"); close_terminal(); return; }
    lv_obj_move_to_index(K.vt, 0);
    vtw_set_colors(K.vt, theme()->term_fg, theme()->term_bg);
    static char a_script[200], a_host[64], a_port[16], a_user[64];
    snprintf(a_script, sizeof a_script, "%s", claude_connect_script);
    snprintf(a_host, sizeof a_host, "%s", host());
    snprintf(a_port, sizeof a_port, "%s", port());
    snprintf(a_user, sizeof a_user, "%s", user());
    char *argv[] = { "sh", a_script, a_host, a_port, a_user, NULL };
    char num[16];
    snprintf(num, sizeof num, "%d", cols);                /* the brain's terminal gets our size */
    setenv("COLUMNS", num, 1);
    snprintf(num, sizeof num, "%d", rows);
    setenv("LINES", num, 1);
    int pty = getenv("NZ_FORCE_PIPES") ? 0 : 1;           /* tests: behave like the console (no PTYs) */
    if (vtw_spawn(K.vt, argv, pty) != 0) { ui_toast("Could not start the connection"); close_terminal(); return; }
    vtw_set_raw_pipe(K.vt, !vtw_uses_pty(K.vt));          /* console: pipes, the brain has the terminal */
    K.connected = 1;
    ui_set_badge("CLAUDE");
    show_view(1);                                         /* the chat view first (Y: the terminal) */
}

/* X: type a line on the on-screen keyboard; it goes to Claude as typed keys */
static void typed(const char *text, void *ctx)
{
    (void)ctx;
    if (!text) return;
    for (const char *c = text; *c; c++) send_key(KB_CHAR, (unsigned char)*c, 0);
}

/* START > Old chats: types /resume and Enter, as if typed, so Claude Code
 * shows its list of earlier conversations (pick one with UP / DOWN and A) */
static void old_chats(void)
{
    if (!K.vt) return;
    if (K.chat_mode) show_view(0);                            /* (its list is a menu: the terminal shows it properly) */
    typed("/resume", NULL);
    send_key(KB_ENTER, 0, 0);
}

/* the START menu; item 2 switches between the chat view and the terminal */
static const char *const term_items[] = {
    "Disconnect", "Old chats", "Show the chat", "Tab", "Shift+Tab (change mode)", "Ctrl+C", "Stop the connection program" };
static const char *const chat_items[] = {
    "Disconnect", "Old chats", "Show the terminal", "Tab", "Shift+Tab (change mode)", "Ctrl+C", "Stop the connection program" };

static void term_menu(int c, void *ctx)
{
    (void)ctx;
    switch (c) {
    case 0: close_terminal(); break;
    case 1: old_chats(); break;
    case 2: show_view(!K.chat_mode); break;                   /* chat view <-> terminal */
    case 3: send_key(KB_TAB, 0, 0); break;                    /* (L1 / R1 scroll now) */
    case 4: send_key(KB_BACKTAB, 0, 0); break;
    case 5: send_key(KB_CHAR, 'c', VTW_MOD_CTRL); break;      /* (in the chat view Y shows the terminal) */
    case 6: if (K.vt) vtw_kill(K.vt, 2); break;               /* SIGINT to the local ssh: rarely needed */
    default: break;
    }
}

static void open_menu(void)
{
    nz_menu("Claude", K.chat_mode ? chat_items : term_items, (int)(sizeof term_items / sizeof term_items[0]), term_menu, NULL);
}

/* ---------- scrolling back through earlier messages ---------- */
/* show the "SCROLLED BACK" sign only while earlier lines are shown */
static void update_marker(void)
{
    if (!K.marker) return;
    int back = K.vt && vtw_scroll_offset(K.vt) > 0;
    if (back) lv_obj_clear_flag(K.marker, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(K.marker, LV_OBJ_FLAG_HIDDEN);
}

static int page(void) { return K.rows > 2 ? K.rows - 1 : 1; }   /* a page: one line stays for context */

/* lines > 0: back to earlier lines, < 0: forward again */
static void scroll_by(int lines)
{
    if (!K.vt) return;
    int before = vtw_scroll_offset(K.vt);
    vtw_scroll(K.vt, lines);
    if (lines > 0 && before == 0 && vtw_scroll_offset(K.vt) == 0) ui_toast("Nothing earlier to show yet");
    update_marker();
}

/* ---------- the chat view: the same conversation as bubbles ----------
 * Claude Code is a terminal program: there is no chat to download, so the
 * bubbles are worked out from the terminal's own text (its kept lines + the
 * live screen) whenever something new was printed, at most ~2 times a second.
 * Nothing of the conversation is copied and kept here - only which lines
 * make each bubble, and the labels of the newest CHAT_MAX bubbles.
 * What Claude Code prints, and what becomes of it:
 *     "> hello"               (at the left edge)  -> your bubble, on the right
 *     a round dot + text      (Claude's answer, or a tool it used) -> Claude's
 *                             bubble, on the left; the indented lines under
 *                             it (also a tool's output, after a corner sign)
 *                             belong to it
 *     box lines (----, corners) are dropped and end a bubble
 *     the input box at the bottom (its "> " prompt) and the hints under it:
 *                             left out - they are not messages
 *     anything else (the welcome box, "Thinking...") -> Claude's bubble
 * A menu or question box from Claude Code becomes a Claude bubble too, which
 * may look odd: the terminal (Y) shows it as it really is. */
enum { MARK_NONE, MARK_CLAUDE, MARK_ME };

/* the round dots Claude Code puts before its answers (U+23FA, U+25CF; U+2022
 * is how older kept lines stored them) */
static int claude_dot(uint32_t c) { return c == 0x23FA || c == 0x25CF || c == 0x2022; }

/* add len bytes of s to out (size n, k bytes used); a piece that doesn't fit is left out */
static size_t put(char *out, size_t n, size_t k, const char *s, size_t len)
{
    if (k + len >= n) return k;
    memcpy(out + k, s, len);
    return k + len;
}

/* One terminal line, cleaned up for a bubble (into out, size n):
 *  - box-drawing and block characters become spaces,
 *  - a few look-alikes become plain letters (the bubble font has only plain
 *    letters; Arabic and emoji are drawn by artext.c),
 *  - a reply sign at the left edge ("> " = you, a round dot = Claude) is
 *    taken off and told in *mark,
 *  - spaces at both ends go.
 * *rule = 1 for a line that is just a box line. Returns the indent (spaces). */
static int clean_line(const char *in, char *out, size_t n, int *rule, int *mark)
{
    size_t k = 0;
    uint32_t i = 0;
    int boxes = 0, others = 0;
    while (in[i]) {
        uint32_t s = i, c = _lv_txt_encoded_next(in, &i);
        const char *rep = NULL;                               /* what to write instead (NULL = the letter itself) */
        if (c >= 0x2500 && c <= 0x259F) { boxes++; rep = " "; }   /* box drawing, blocks */
        else if (c == 0x23BF || c == 0xA0) rep = " ";        /* the tool output sign (a corner), no-break space */
        else if (c == 0xFFFD) rep = "";                       /* a letter that got lost */
        else if (c == 0x2018 || c == 0x2019) rep = "'";
        else if (c == 0x201C || c == 0x201D) rep = "\"";
        else if (c == 0x2013 || c == 0x2014 || c == 0x2212) rep = "-";
        else if (c == 0x2026) rep = "...";
        else if (c == 0x2192) rep = "->";
        else if (c == 0x2190) rep = "<-";
        else if (c == 0x276F) rep = ">";                      /* the prompt sign of newer Claude Codes */
        else if (c == 0x2713 || c == 0x2714) rep = LV_SYMBOL_OK;
        else if (c == 0x2717 || c == 0x2718) rep = "x";
        else if (c == 0xB7 || c == 0x2722 || c == 0x2733 || c == 0x2736 || c == 0x273B || c == 0x273D) rep = "*";   /* the busy star */
        if (c != ' ' && !(c >= 0x2500 && c <= 0x259F)) others++;
        k = rep ? put(out, n, k, rep, strlen(rep)) : put(out, n, k, in + s, i - s);
    }
    out[k] = 0;
    *rule = boxes >= 8 && others * 4 <= boxes;                /* (nearly) only box characters */
    int indent = 0;
    while (out[indent] == ' ') indent++;
    char *p = out + indent;
    uint32_t j = 0, c = _lv_txt_encoded_next(p, &j);
    *mark = MARK_NONE;
    if (indent == 0 && claude_dot(c) && (p[j] == ' ' || !p[j])) { *mark = MARK_CLAUDE; p += j; }
    else if (indent == 0 && c == '>' && (p[1] == ' ' || !p[1])) { *mark = MARK_ME; p++; }
    while (*p == ' ') p++;
    memmove(out, p, strlen(p) + 1);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == ' ') out[--len] = 0;
    return indent;
}

/* the "> " prompt of the input box (not "> 1. Yes": a choice in a question box) */
static int is_prompt(int mark, const char *txt)
{
    if (mark != MARK_ME) {                                    /* inside a box: "> " is still in the text */
        if (txt[0] != '>') return 0;
        txt++;
        while (*txt == ' ') txt++;
    }
    return !(txt[0] >= '0' && txt[0] <= '9');
}

/* Where Claude Code's input box starts: the lowest two box lines at most 8
 * lines apart with the "> " prompt between them, near the bottom of the
 * screen. It and the hints under it are not part of the conversation.
 * No input box (e.g. a question box is up instead): total. */
static int input_box_start(int total)
{
    char raw[400], txt[400];
    int top = total - K.rows, last = total - 1, below = -1, prompt = 0;
    if (top < 0) top = 0;
    while (last >= top && !vtw_line_text(K.vt, last, raw, sizeof raw)[0]) last--;   /* (empty rows at the bottom) */
    for (int i = last; i >= top && i > last - 16; i--) {
        int rule, mark;
        clean_line(vtw_line_text(K.vt, i, raw, sizeof raw), txt, sizeof txt, &rule, &mark);
        if (rule) {
            if (below >= 0 && prompt && below - i <= 8) return i;   /* the box's top line */
            below = i;                                        /* a box's bottom line (maybe) */
            prompt = 0;
        } else if (below >= 0 && is_prompt(mark, txt)) prompt = 1;
    }
    return total;
}

/* Which lines make which bubble: the newest bubbles (at most CHAT_MAX),
 * oldest first, into b[]. Returns how many. */
static int chat_scan(Bubble *b)
{
    char raw[400], txt[400];
    Bubble ring[CHAT_MAX];                                    /* the newest CHAT_MAX, round and round */
    memset(ring, 0, sizeof ring);
    int total = vtw_line_count(K.vt), end = input_box_start(total);
    int n = 0, who = -1, cur = 0;                             /* n: bubbles found; who: whose bubble is open (-1 none) */
    for (int i = 0; i < end; i++) {
        int rule, mark, ind = clean_line(vtw_line_text(K.vt, i, raw, sizeof raw), txt, sizeof txt, &rule, &mark);
        if (rule) { who = -1; continue; }                     /* a box line ends a bubble */
        if (!txt[0]) { if (who == WHO_ME) who = -1; continue; }   /* an empty line ends yours (not Claude's: paragraphs) */
        int start = -1;                                       /* >= 0: a new bubble starts here, by WHO_* */
        if (mark == MARK_ME) start = WHO_ME;
        else if (mark == MARK_CLAUDE) start = WHO_CLAUDE;
        else if (who < 0 || (who == WHO_ME && ind == 0)) start = WHO_CLAUDE;   /* Claude Code's own text */
        else if (i - ring[cur].first >= CHAT_LINES) start = who;              /* long: goes on in a new bubble */
        if (start >= 0) {
            cur = n++ % CHAT_MAX;
            ring[cur].first = ring[cur].last = i;
            ring[cur].who = who = start;
        } else ring[cur].last = i;
    }
    int count = n < CHAT_MAX ? n : CHAT_MAX, oldest = n < CHAT_MAX ? 0 : n % CHAT_MAX;
    for (int j = 0; j < count; j++) b[j] = ring[(oldest + j) % CHAT_MAX];
    return count;
}

/* a numbered or dashed list line ("- ", "* ", "1. ") */
static int list_item(const char *s)
{
    if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') return 1;
    int d = 0;
    while (s[d] >= '0' && s[d] <= '9') d++;
    return d > 0 && (s[d] == '.' || s[d] == ')') && s[d + 1] == ' ';
}

/* A bubble's text (into out, size n): its lines cleaned; lines that Claude
 * Code had to wrap at the terminal's edge are joined again, so the bubble can
 * wrap them to its own width; an empty line starts a new paragraph. */
static void chat_text(const Bubble *b, char *out, size_t n)
{
    char raw[400], txt[400];
    size_t k = 0;
    int blank = 0, wrapped = 0;
    for (int i = b->first; i <= b->last; i++) {
        int rule, mark, ind = clean_line(vtw_line_text(K.vt, i, raw, sizeof raw), txt, sizeof txt, &rule, &mark);
        if (!txt[0]) { blank = 1; continue; }
        if (k) {
            const char *sep = blank ? "\n\n" : wrapped && !list_item(txt) ? " " : "\n";
            k = put(out, n, k, sep, strlen(sep));
        }
        k = put(out, n, k, txt, strlen(txt));
        /* did this line reach (nearly) the right edge? then Claude Code wrapped it there */
        wrapped = ind + (mark ? 2 : 0) + (int)_lv_txt_get_encoded_length(txt) >= K.cols - 15;
        blank = 0;
    }
    out[k] = 0;
}

/* a quick fingerprint of a text (has a bubble's text changed?) */
static uint32_t text_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* a bubble's text on its label, at most maxw pixels wide (Arabic: joined, right to left) */
static void bubble_text(lv_obj_t *l, const char *text, int maxw)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_SIZE_CONTENT);                     /* (a label used before: its own width again) */
    if (artext_has_arabic(text)) { artext_set(l, text, maxw); return; }   /* (its lines already fit) */
    artext_set(l, text, 0);                                   /* (emoji: the font gets the emoji pictures) */
    lv_point_t sz;
    lv_txt_get_size(&sz, text, lv_obj_get_style_text_font(l, 0), lv_obj_get_style_text_letter_space(l, 0),
                    lv_obj_get_style_text_line_space(l, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > maxw) lv_obj_set_width(l, maxw);               /* too wide: wrapped */
}

/* a new, empty bubble: on the right in the accent colour (yours) or on the
 * left in the surface colour (Claude's). Returns its text label. */
static lv_obj_t *add_bubble(int who)
{
    lv_obj_t *row = th_box(K.chat_list, -1);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, who == WHO_ME ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *b = th_box(row, who == WHO_ME ? TS_BTN_ACCENT : TS_BTN);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 5, 0);
    return lv_label_create(b);                                /* (its colour comes from the bubble) */
}

/* all bubbles gone (their memory too) */
static void chat_forget(void)
{
    if (K.chat_list) lv_obj_clean(K.chat_list);
    K.nshown = 0;
}

/* (re)draw the bubbles from the terminal's text */
static void chat_show(void)
{
    if (!K.vt || !K.chat_list) return;
    Bubble b[CHAT_MAX];
    int nb = chat_scan(b);
    char *text = malloc(CHAT_TEXT);                           /* (only while drawing) */
    if (!text) return;
    /* the same bubbles as shown (as many, by the same people)? then only changed texts are set */
    int same = nb == K.nshown;
    for (int i = 0; same && i < nb; i++) if (b[i].who != K.chat_who[i]) same = 0;
    lv_coord_t y = lv_obj_get_scroll_y(K.chat_list);
    if (!same) chat_forget();
    lv_obj_update_layout(K.chat_list);
    int w = lv_obj_get_content_width(K.chat_list);
    if (w <= 0) w = 600;
    for (int i = 0; i < nb; i++) {
        chat_text(&b[i], text, CHAT_TEXT);
        uint32_t h = text_hash(text);
        if (!same) { K.chat_label[i] = add_bubble(b[i].who); K.chat_who[i] = b[i].who; }
        else if (h == K.chat_hash[i]) continue;               /* (unchanged) */
        K.chat_hash[i] = h;
        /* a bubble: yours at most 3/4 of the width, Claude's (often long) 7/8; minus its padding */
        bubble_text(K.chat_label[i], text, (b[i].who == WHO_ME ? w * 3 / 4 : w * 7 / 8) - 20);
    }
    free(text);
    K.nshown = nb;
    if (nb) lv_obj_add_flag(K.chat_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(K.chat_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(K.chat_list);
    if (K.chat_follow) lv_obj_scroll_by(K.chat_list, 0, -lv_obj_get_scroll_bottom(K.chat_list), LV_ANIM_OFF);   /* the newest */
    else if (!same) lv_obj_scroll_to_y(K.chat_list, y, LV_ANIM_OFF);   /* (reading older ones: stay there) */
}

/* 1: the chat view, 0: the terminal (the connection stays as it is) */
static void show_view(int chat)
{
    K.chat_mode = chat && K.vt;
    if (K.chat_mode) {
        lv_obj_add_flag(K.term_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(K.chat_page, LV_OBJ_FLAG_HIDDEN);
        K.chat_follow = 1;                                    /* it starts at the newest message */
        K.chat_seen = vtw_changes(K.vt);
        K.chat_tick = 0;
        chat_show();
    } else {
        lv_obj_add_flag(K.chat_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(K.term_page, LV_OBJ_FLAG_HIDDEN);
        chat_forget();                                        /* (the terminal shows it all: bubbles not needed) */
    }
}

/* scroll the bubbles (dy > 0: to older ones); at the bottom it follows new ones again */
static void chat_scroll(int dy)
{
    lv_obj_scroll_by_bounded(K.chat_list, 0, dy, LV_ANIM_OFF);
    K.chat_follow = lv_obj_get_scroll_bottom(K.chat_list) <= 4;
}

/* chat view, X: the message is typed to Claude and Enter follows a moment
 * later (sent together, Claude Code could take the Enter as part of a paste) */
static void chat_typed(const char *text, void *ctx)
{
    (void)ctx;
    if (!text || !text[strspn(text, " ")] || !K.vt) return;
    typed(text, NULL);
    K.enter_in = 8;                                           /* 8 ticks: ~0.25 s (see refresh) */
    K.chat_follow = 1;                                        /* show the newest: your message */
}

static int chat_key(int k)
{
    int page = lv_obj_get_height(K.chat_list) - 40;           /* a page: a little stays for context */
    switch (k) {
    case NZ_KEY_UP:    chat_scroll(60); return 1;
    case NZ_KEY_DOWN:  chat_scroll(-60); return 1;
    case NZ_KEY_L:     chat_scroll(page); return 1;
    case NZ_KEY_R:     chat_scroll(-page); return 1;
    case NZ_KEY_A:     send_key(KB_ENTER, 0, 0); return 1;
    case NZ_KEY_B:     send_key(KB_ESC, 0, 0); return 1;
    case NZ_KEY_Y:     show_view(0); return 1;                /* the full terminal */
    case NZ_KEY_X:     nz_prompt("Message to Claude (sent with Enter)", "", chat_typed, NULL); return 1;
    case NZ_KEY_START: open_menu(); return 1;
    default:           return 1;                              /* (LEFT / RIGHT: nothing here) */
    }
}

/* ---------- the app ---------- */
static void refresh(lv_timer_t *t)
{
    (void)t;
    if (K.vt) {
        vtw_poll(K.vt);
        update_marker();                                      /* (new output goes back to the live screen) */
        if (K.enter_in > 0 && --K.enter_in == 0) send_key(KB_ENTER, 0, 0);   /* (the chat view's delayed Enter) */
        if (K.chat_mode && ++K.chat_tick >= CHAT_EVERY) {     /* the chat view: redrawn when something new came */
            K.chat_tick = 0;
            if (vtw_changes(K.vt) != K.chat_seen) { K.chat_seen = vtw_changes(K.vt); chat_show(); }
            else if (K.chat_follow && lv_obj_get_scroll_bottom(K.chat_list) > 4)   /* (e.g. a new theme made the text taller) */
                lv_obj_scroll_by(K.chat_list, 0, -lv_obj_get_scroll_bottom(K.chat_list), LV_ANIM_OFF);
        }
        if (!vtw_child_running(K.vt)) { ui_toast("The connection ended"); close_terminal(); }
    } else if (!K.connected) {
        static uint32_t n;
        if (++n % 20 == 0) {                                  /* every 2 s: network state, access */
            int sel = nzlist_selected(K.list);
            fill_rows();
            nzlist_select(K.list, sel);
        }
    }
}

static void create(NzApp *a, lv_obj_t *root)
{
    (void)a;
    memset(&K, 0, sizeof K);
    K.root = root;
    th_add(root, TS_SCREEN);

    K.connect_page = th_box(root, -1);
    lv_obj_set_size(K.connect_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(K.connect_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *card = th_box(K.connect_page, TS_PANEL);
    lv_obj_set_size(card, LV_PCT(100), 66);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *ic = lv_img_create(card);
    lv_img_set_src(ic, icon_get(ICON_CLAUDE, 48));
    th_add(ic, TS_ICON);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = th_label(card, "Claude Code", TS_HEADING);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 72, 8);
    lv_obj_t *s = th_label(card, "runs on your brain (home PC or phone) with your subscription", TS_SMALL);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 72, -10);
    K.list = nzlist_create(K.connect_page, 0);
    lv_obj_set_width(nzlist_obj(K.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(K.list), 1);
    nzlist_on_activate(K.list, row_activated, NULL);
    lv_obj_t *hint = th_box(K.connect_page, TS_BAR);
    lv_obj_set_size(hint, LV_PCT(100), 26);
    static const char *const hints[] = { "A", "Choose", "B", "Back" };
    lv_obj_center(nz_hints(hint, hints, 2));
    fill_rows();
    nzlist_select(K.list, ROW_CONNECT);

    K.term_page = th_box(root, TS_TERM);                       /* the terminal + shortcut row */
    lv_obj_set_size(K.term_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(K.term_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(K.term_page, 12, 0);               /* text not glued to the screen edge */
    lv_obj_set_style_pad_top(K.term_page, 4, 0);
    K.keys_row = th_box(K.term_page, TS_BAR);
    lv_obj_set_size(K.keys_row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(K.keys_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(K.keys_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    th_label(K.keys_row, "START menu  X type  L1 scroll  R1 chat", TS_SMALL);
    for (size_t i = 0; i < sizeof shortcuts / sizeof shortcuts[0]; i++) {
        lv_obj_t *b = th_label(K.keys_row, shortcuts[i].label, TS_KEYCAP);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, shortcut_clicked, LV_EVENT_CLICKED, (void *)&shortcuts[i]);
    }
    /* the sign shown while scrolled back: on top of the terminal, top right */
    K.marker = th_label(K.term_page, "SCROLLED BACK - R1 / any key returns", TS_SMALL);
    th_add(K.marker, TS_BTN_ACCENT);                           /* accent background: easy to see */
    lv_obj_set_style_pad_hor(K.marker, 8, 0);
    lv_obj_set_style_pad_ver(K.marker, 3, 0);
    lv_obj_add_flag(K.marker, LV_OBJ_FLAG_FLOATING);           /* not part of the column */
    lv_obj_align(K.marker, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(K.marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(K.term_page, LV_OBJ_FLAG_HIDDEN);

    /* the chat view: the bubbles, and the buttons at the bottom */
    K.chat_page = th_box(root, -1);
    lv_obj_set_size(K.chat_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(K.chat_page, LV_FLEX_FLOW_COLUMN);
    K.chat_list = th_box(K.chat_page, -1);
    lv_obj_add_flag(K.chat_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(K.chat_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_width(K.chat_list, LV_PCT(100));
    lv_obj_set_flex_grow(K.chat_list, 1);
    lv_obj_set_flex_flow(K.chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(K.chat_list, 10, 0);
    lv_obj_set_style_pad_row(K.chat_list, 6, 0);
    K.chat_empty = th_label(K.chat_page, "Waiting for Claude Code...   (Y shows the full terminal)", TS_MUTED);
    lv_obj_add_flag(K.chat_empty, LV_OBJ_FLAG_FLOATING);      /* (not part of the column) */
    lv_obj_center(K.chat_empty);
    lv_obj_t *ch = th_box(K.chat_page, TS_BAR);
    lv_obj_set_size(ch, LV_PCT(100), 26);
    static const char *const chat_hints[] = { "X", "Type", "A", "Enter", "B", "Esc", "L1/R1", "Scroll", "Y", "Full terminal", "START", "Menu" };
    lv_obj_center(nz_hints(ch, chat_hints, 6));
    lv_obj_add_flag(K.chat_page, LV_OBJ_FLAG_HIDDEN);
    K.timer = lv_timer_create(refresh, 33, NULL);
}

static int key(NzApp *a, int k)
{
    (void)a;
    if (!K.vt) {
        if (k == NZ_KEY_LEFT || k == NZ_KEY_RIGHT) {          /* change the brain right here */
            if (nzlist_selected(K.list) == ROW_BRAIN) { row_activated(ROW_BRAIN, NULL); return 1; }
        }
        return nzlist_key(K.list, k);
    }
    if (K.chat_mode) return chat_key(k);                     /* the chat view has its own buttons */
    if (vtw_scroll_offset(K.vt) > 0) {                        /* scrolled back to earlier lines */
        switch (k) {
        case NZ_KEY_L:    scroll_by(page()); return 1;        /* a page further back */
        case NZ_KEY_R:    scroll_by(-page()); return 1;       /* a page forward (to the live screen at the end) */
        case NZ_KEY_UP:   scroll_by(1); return 1;             /* one line */
        case NZ_KEY_DOWN: scroll_by(-1); return 1;
        default:          scroll_by(-vtw_scroll_offset(K.vt)); return 1;   /* any other: back to live, not sent */
        }
    }
    switch (k) {                                              /* the buttons drive Claude Code */
    case NZ_KEY_A:     send_key(KB_ENTER, 0, 0); return 1;
    case NZ_KEY_B:     send_key(KB_ESC, 0, 0); return 1;
    case NZ_KEY_Y:     send_key(KB_CHAR, 'c', VTW_MOD_CTRL); return 1;
    case NZ_KEY_L:     scroll_by(page()); return 1;           /* back to earlier messages */
    case NZ_KEY_R:     show_view(1); return 1;                /* (the live screen is the newest): the chat view */
    case NZ_KEY_UP:    send_key(KB_UP, 0, 0); return 1;
    case NZ_KEY_DOWN:  send_key(KB_DOWN, 0, 0); return 1;
    case NZ_KEY_LEFT:  send_key(KB_LEFT, 0, 0); return 1;
    case NZ_KEY_RIGHT: send_key(KB_RIGHT, 0, 0); return 1;
    case NZ_KEY_X:     nz_prompt("Type to Claude (then A = Enter)", "", typed, NULL); return 1;
    case NZ_KEY_START: open_menu(); return 1;
    default: return 1;
    }
}

static void kbd(NzApp *a, const KbEvent *ev)
{
    (void)a;
    if (!K.vt) return;
    if (ev->key == KB_CTRL_C) send_key(KB_CHAR, 'c', VTW_MOD_CTRL);
    else if (ev->key == KB_CTRL_D) send_key(KB_CHAR, 'd', VTW_MOD_CTRL);
    else if (ev->key == KB_CTRL_S) send_key(KB_CHAR, 's', VTW_MOD_CTRL);
    else if (ev->key == KB_CTRL_OTHER) send_key(KB_CHAR, ev->ch, VTW_MOD_CTRL);
    else if (ev->key >= KB_F1) send_key(VTW_KEY_F(ev->key - KB_F1 + 1), 0, 0);
    else send_key(ev->key, ev->ch, 0);
    update_marker();                                          /* (a typed key went back to the live screen) */
}

static void destroy(NzApp *a)
{
    (void)a;
    if (K.timer) lv_timer_del(K.timer);
    if (K.vt) lv_obj_del(K.vt);
    forget_run_key();
    nzlist_free(K.list);
    memset(&K, 0, sizeof K);
    ui_set_badge(NULL);
}

/* for tests */
lv_obj_t *app_claude_terminal(void) { return K.vt; }
int app_claude_marker_shown(void) { return K.marker && !lv_obj_has_flag(K.marker, LV_OBJ_FLAG_HIDDEN); }
int app_claude_chat_shown(void) { return K.chat_page && K.chat_mode && !lv_obj_has_flag(K.chat_page, LV_OBJ_FLAG_HIDDEN); }
lv_obj_t *app_claude_chat_list(void) { return K.chat_list; }
int app_claude_chat_count(void) { return K.nshown; }
int app_claude_chat_mine(int i) { return i >= 0 && i < K.nshown && K.chat_who[i] == WHO_ME; }
/* bubble i's label (its text is in drawing order: Arabic joined, right to left) */
lv_obj_t *app_claude_chat_label(int i) { return i >= 0 && i < K.nshown ? K.chat_label[i] : NULL; }

NzApp app_claude = {
    .id = APP_CLAUDE, .title = "Claude", .icon = ICON_CLAUDE,
    .create = create, .key = key, .kbd = kbd, .destroy = destroy,
};

/* term_cmds.c - the Terminal's "Commands" list (see term_cmds.h):
 * the table of commands, and the page that shows it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lvgl.h"
#include "apps.h"
#include "home.h"
#include "nzlist.h"
#include "nzui.h"
#include "settings.h"
#include "term_cmds.h"
#include "terminal.h"
#include "theme.h"
#include "ui.h"

/* ---------- the table ----------
 * { command, what it does (one line, plain words), example or NULL }
 * A line with command NULL starts a group (its name is in the 2nd field).
 * Only commands the console really has: Debian's own, and NZEUS Deck's helpers
 * (nzeus/bin, core/deck-modern). CAPITALS = the part you fill in. */
static const TermCmd table[] = {
    { NULL, "Files and folders", NULL },
    { "ls", "List what is in this folder", NULL },
    { "ls -la", "List everything here, with sizes, dates and hidden files", NULL },
    { "cd FOLDER", "Go into a folder. FOLDER = its name, or .. for one folder up", "cd docs" },
    { "cd", "Go back to your home folder (NZEUS's own files)", NULL },
    { "pwd", "Show the full name of the folder you are in", NULL },
    { "mkdir NAME", "Make a new folder called NAME", "mkdir projects" },
    { "touch FILE", "Make a new, empty file called FILE", "touch notes.txt" },
    { "cp FILE COPY", "Copy a file. FILE = the original, COPY = the new name", "cp robot.py robot-old.py" },
    { "cp -r FOLDER COPY", "Copy a whole folder, with everything in it", "cp -r projects projects-backup" },
    { "mv OLD NEW", "Rename or move. OLD = the name now, NEW = new name or folder", "mv test.py robot.py" },
    { "rm FILE", "Delete a file. There is no undo!", "rm old.txt" },
    { "rm -r FOLDER", "Delete a folder and everything in it. No undo!", NULL },
    { "find . -name \"WORD*\"", "Find files whose names start with WORD (here and below)", "find . -name \"*.py\"" },
    { "du -sh FOLDER", "How much space a folder uses", "du -sh music" },
    { "edit FILE", "Open FILE in the Editor (a new name makes a new file)", "edit hello.py" },

    { NULL, "Look inside files", NULL },
    { "cat FILE", "Show everything in a file", "cat notes.txt" },
    { "head FILE", "Show the first 10 lines of a file", NULL },
    { "tail FILE", "Show the last 10 lines of a file", NULL },
    { "tail -n 50 FILE", "Show the last 50 lines (change 50 to any number)", NULL },
    { "grep WORD FILE", "Show the lines of FILE that have WORD in them", "grep error log.txt" },
    { "grep -rn WORD .", "Search all files here and below for WORD (with line numbers)", "grep -rn wifi ." },
    { "wc -l FILE", "Count the lines in a file", NULL },
    { "sort FILE", "Show the lines of a file in A-Z order", NULL },
    { "echo \"TEXT\" > FILE", "Write TEXT into FILE (what was in it is lost!)", "echo \"hello\" > hi.txt" },
    { "echo \"TEXT\" >> FILE", "Add TEXT as a new line at the end of FILE", "echo \"LEDs\" >> shopping.txt" },
    { "od -c FILE | head", "Show every character, also hidden ones like \\r", NULL },

    { NULL, "How is the console doing", NULL },
    { "free", "Memory (RAM): total, used and free, in KB", NULL },
    { "df -h", "Space used and free on the SD card", NULL },
    { "top -b -n 1", "Which programs use the most processor and memory, once", NULL },
    { "ps", "List the running programs, each with its number (PID)", NULL },
    { "kill NUMBER", "Stop a program. NUMBER = its PID (from ps)", "kill 1234" },
    { "uptime", "How long it has been on, and how busy it is", NULL },
    { "dmesg | tail", "The newest messages from Linux itself (drivers, USB, errors)", NULL },
    { "cat /proc/meminfo", "Every memory number Linux knows", NULL },
    { "uname -a", "Which Linux is running", NULL },
    { "date", "The date and time", NULL },

    { NULL, "Wi-Fi and internet", NULL },
    { "wifi-on.sh", "Switch Wi-Fi on (plug the Wi-Fi dongle in first)", NULL },
    { "wifi-join.sh connect \"NAME\" \"PASSWORD\"", "Join a network. NAME = its name. Not saved in the history", "wifi-join.sh connect \"Home\" \"secret123\"" },
    { "wifi-join.sh status", "Joined or not, and to which network", NULL },
    { "wifi-join.sh disconnect", "Leave the Wi-Fi network (saves memory)", NULL },
    { "nzeus-net-up.sh", "Get an address after joining (DHCP)", NULL },
    { "nzeus-netctl status", "The Wi-Fi address now (ip=none: no address yet)", NULL },
    { "ip route", "The way out to the internet (\"default via ...\")", NULL },
    { "ping -c 3 HOST", "Does HOST answer? HOST = an address or a name", "ping -c 3 8.8.8.8" },
    { "nzeus-fetch https://ADDRESS", "Download a web page and show it", "nzeus-fetch https://example.com" },
    { "nzeus-fetch https://ADDRESS FILE", "Download a page or file and save it as FILE", "nzeus-fetch https://example.com page.html" },

    { NULL, "Hardware and USB", NULL },
    { "lsusb", "List the USB devices that are plugged in", NULL },
    { "ls /dev/tty*", "Serial ports: ttyUSB0 or ttyACM0 = an Arduino or USB adapter", NULL },
    { "ls /dev/input", "Keyboards, mice and game pads that Linux found", NULL },
    { "dmesg | grep -i usb", "Linux messages about USB (what got plugged in)", NULL },
    { "cat /proc/cpuinfo", "About the processor", NULL },
    { "cat /proc/partitions", "The SD card and its parts", NULL },

    { NULL, "NZEUS Deck tools", NULL },
    { "nzeus-bt-up.sh", "Switch Bluetooth on (Bluetooth dongle plugged in)", NULL },
    { "nzeus-bt status", "Bluetooth: on or off, how many devices paired", NULL },
    { "nzeus-bt scan 15", "Look for Bluetooth devices for 15 seconds", NULL },
    { "nzeus-bt pair ADDRESS", "Pair a device. ADDRESS = its address from the scan", "nzeus-bt pair DC:2C:26:01:02:03" },
    { "nzeus-bt list", "Your paired Bluetooth devices", NULL },
    { "nzeus-upload.sh BOARD PORT FILE", "Put firmware on a board. BOARD = uno, nano, nano-old, mega, stm32",
      "nzeus-upload.sh uno /dev/ttyACM0 nzeus/firmware/Blink.ino.hex" },
    { "clear", "Clean the screen (the old output goes away)", NULL },
    { "exit", "Close this session (or the Terminal, if it is the last one)", NULL },

    { NULL, "Python", NULL },
    { "python", "Start Python. Type Python lines; exit() or Ctrl+D stops it", NULL },
    { "python FILE.py", "Run a Python program. FILE = its name", "python hello.py" },
    { "ls /usr/local/lib/nzeus/python-examples", "The example Python programs", NULL },
    { "python /usr/local/lib/nzeus/python-examples/hello.py", "Run the hello example", NULL },
    { "cp /usr/local/lib/nzeus/python-examples/FILE.py .", "Copy an example here, so you can change it",
      "cp /usr/local/lib/nzeus/python-examples/guess.py ." },
    { "edit FILE.py", "Write a Python program in the Editor", "edit robot.py" },
};
#define TABLE_N ((int)(sizeof table / sizeof table[0]))

static int group_start[16], table_groups;      /* where each group starts in the table */

static void count_groups(void)
{
    if (table_groups) return;
    for (int i = 0; i < TABLE_N && table_groups < 16; i++) if (!table[i].cmd) group_start[table_groups++] = i;
}

int term_cmds_group_count(void) { count_groups(); return table_groups + 2; }   /* + Recent, Favourites */

const char *term_cmds_group_name(int g)
{
    count_groups();
    if (g >= 0 && g < table_groups) return table[group_start[g]].what;
    return g == table_groups ? "Recent" : "Favourites";
}

/* entry i of the whole table (a group name has cmd NULL), NULL past the end (tests) */
const TermCmd *term_cmds_entry(int i) { return i >= 0 && i < TABLE_N ? &table[i] : NULL; }

int term_cmds_total(void)
{
    count_groups();
    return TABLE_N - table_groups;
}

static const TermCmd *find_cmd(const char *cmd)
{
    for (int i = 0; i < TABLE_N; i++) if (table[i].cmd && !strcmp(table[i].cmd, cmd)) return &table[i];
    return NULL;
}

/* First CAPITALS part: 2+ capital letters (digits and _ allowed after the
 * first), not glued to small letters, e.g. FILE in "python FILE.py". */
int term_cmds_placeholder(const char *cmd, int *len)
{
    for (int i = 0; cmd[i]; i++) {
        if (cmd[i] < 'A' || cmd[i] > 'Z') continue;
        if (i > 0 && ((cmd[i - 1] >= 'a' && cmd[i - 1] <= 'z') || (cmd[i - 1] >= 'A' && cmd[i - 1] <= 'Z') ||
                      (cmd[i - 1] >= '0' && cmd[i - 1] <= '9'))) continue;
        int j = i, caps = 0;
        while ((cmd[j] >= 'A' && cmd[j] <= 'Z') || (cmd[j] >= '0' && cmd[j] <= '9') || cmd[j] == '_') {
            if (cmd[j] >= 'A' && cmd[j] <= 'Z') caps++;
            j++;
        }
        if (caps >= 2 && !(cmd[j] >= 'a' && cmd[j] <= 'z')) { *len = j - i; return i; }
        i = j > i ? j - 1 : i;
    }
    *len = 0;
    return -1;
}

/* ---------- favourites ---------- */
#define FAV_FILE NZ_DIR "/terminal_favourites"
#define FAV_MAX  50
static char *favs[FAV_MAX];
static int fav_n, fav_loaded;

static void fav_free(void)
{
    for (int i = 0; i < fav_n; i++) free(favs[i]);
    fav_n = 0;
    fav_loaded = 0;
}

static void fav_load(void)
{
    fav_free();
    fav_loaded = 1;
    FILE *f = fopen(FAV_FILE, "r");
    if (!f) return;
    char line[TERM_INPUT + 2];
    while (fgets(line, sizeof line, f) && fav_n < FAV_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && (favs[fav_n] = strdup(line))) fav_n++;
    }
    fclose(f);
}

/* to a .tmp file, then renamed over the old one (safe if the power goes) */
static void fav_save(void)
{
    const char *tmp = FAV_FILE ".tmp";
    mkdir(NZ_DIR, 0755);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < fav_n; i++) fprintf(f, "%s\n", favs[i]);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0) { unlink(tmp); return; }
    rename(tmp, FAV_FILE);
}

static int fav_index(const char *cmd)
{
    for (int i = 0; i < fav_n; i++) if (!strcmp(favs[i], cmd)) return i;
    return -1;
}

int term_fav_has(const char *cmd)
{
    int was = fav_loaded;
    if (!was) fav_load();
    int r = fav_index(cmd) >= 0;
    if (!was) fav_free();
    return r;
}

int term_fav_toggle(const char *cmd)
{
    int was = fav_loaded, now;
    if (!was) fav_load();
    int i = fav_index(cmd);
    if (i >= 0) {                                    /* starred: remove it */
        free(favs[i]);
        memmove(favs + i, favs + i + 1, (size_t)(fav_n - i - 1) * sizeof favs[0]);
        fav_n--;
        now = 0;
    } else if (fav_n < FAV_MAX && (favs[fav_n] = strdup(cmd))) {
        fav_n++;
        now = 1;
    } else now = 0;
    fav_save();
    if (!was) fav_free();
    return now;
}

/* ---------- the page ---------- */
#define MAX_ROWS 64
static struct {
    lv_obj_t *box, *title, *count, *detail, *hint;
    NzList *list;
    int group, rows, in_window;
    const char *cmd[MAX_ROWS];     /* each row's command (NULL = an information row) */
    term_cmds_pick_fn pick;
} P;
static int last_group;             /* the list opens where you were last time */

int term_cmds_is_open(void) { return P.box != NULL; }
int term_cmds_group(void) { return P.group; }
int term_cmds_rows(void) { return P.rows; }

/* the selected row's text, as it is on the screen (for tests) */
const char *term_cmds_row_text(int part)
{
    if (!P.box || !P.rows) return "";
    lv_obj_t *row = lv_obj_get_child(nzlist_obj(P.list), nzlist_selected(P.list));
    lv_obj_t *col = row ? lv_obj_get_child(row, 1) : NULL;           /* nzlist: icon, text column, right */
    lv_obj_t *l = col ? lv_obj_get_child(col, part ? 1 : 0) : NULL;
    return l ? lv_label_get_text(l) : "";
}

static void update_detail(void)
{
    if (!P.box) return;
    int i = nzlist_selected(P.list);
    const char *c = (i >= 0 && i < P.rows) ? P.cmd[i] : NULL;
    const TermCmd *t = c ? find_cmd(c) : NULL;
    if (t && t->example) lv_label_set_text_fmt(P.detail, "Example:  %s", t->example);
    else if (c) lv_label_set_text(P.detail, "It goes on the typed line. Change it, then Enter runs it.");
    else lv_label_set_text(P.detail, "");
}

static void add_row(const char *cmd, const char *what)
{
    if (P.rows >= MAX_ROWS) return;
    int i = nzlist_add(P.list, -1, cmd, what, cmd && fav_index(cmd) >= 0 ? "starred" : NULL);
    if (i < 0) return;
    lv_obj_t *row = lv_obj_get_child(nzlist_obj(P.list), i);
    if (cmd) th_add(lv_obj_get_child(lv_obj_get_child(row, 1), 0), TS_MONO_ACCENT);   /* the command: monospace */
    P.cmd[i] = cmd;
    P.rows = i + 1;
}

static void fill(int keep)
{
    nzlist_clear(P.list);
    P.rows = 0;
    if (P.group < table_groups) {                    /* a group from the table */
        for (int i = group_start[P.group] + 1; i < TABLE_N && table[i].cmd; i++) add_row(table[i].cmd, table[i].what);
    } else if (P.group == table_groups) {            /* Recent: newest first, each once */
        for (int i = terminal_history_count() - 1; i >= 0 && P.rows < 30; i--) {
            const char *h = terminal_history_get(i);
            int seen = 0;
            for (int k = 0; k < P.rows; k++) if (!strcmp(P.cmd[k], h)) seen = 1;
            if (seen) continue;
            const TermCmd *t = find_cmd(h);
            add_row(h, t ? t->what : "You ran this before");
        }
        if (!P.rows) add_row(NULL, "Commands you run will show up here");
    } else {                                         /* Favourites */
        for (int i = 0; i < fav_n; i++) {
            const TermCmd *t = find_cmd(favs[i]);
            add_row(favs[i], t ? t->what : "Your starred command");
        }
        if (!P.rows) add_row(NULL, "Press Y on a command to star it");
    }
    if (P.rows && !P.cmd[0]) nzlist_set_title(P.list, 0, "Nothing here yet");
    lv_label_set_text(P.title, term_cmds_group_name(P.group));
    lv_label_set_text_fmt(P.count, "%d of %d", P.group + 1, term_cmds_group_count());
    nzlist_select(P.list, keep < P.rows ? keep : P.rows - 1);
    update_detail();
}

static void set_group(int g)
{
    int n = term_cmds_group_count();
    P.group = (g + n) % n;                           /* past the end: round to the start */
    last_group = P.group;
    fill(0);
}

static void picked(int index, void *ctx)
{
    (void)ctx;
    if (index < 0 || index >= P.rows || !P.cmd[index]) return;
    static char chosen[TERM_INPUT];
    snprintf(chosen, sizeof chosen, "%s", P.cmd[index]);
    term_cmds_pick_fn fn = P.pick;
    term_cmds_close();                               /* close first: the pick may open the keyboard */
    if (fn) fn(chosen);
}

static void star_selected(void)
{
    int i = nzlist_selected(P.list);
    if (i < 0 || i >= P.rows || !P.cmd[i]) return;
    char cmd[TERM_INPUT];
    snprintf(cmd, sizeof cmd, "%s", P.cmd[i]);
    int on = term_fav_toggle(cmd);
    ui_toast(on ? "Starred: it is in Favourites now" : "Removed from Favourites");
    if (P.group == table_groups + 1) fill(i);        /* the Favourites list itself changed */
    else nzlist_set_right(P.list, i, on ? "starred" : "");
}

static void prev_clicked(lv_event_t *e) { (void)e; set_group(P.group - 1); }
static void next_clicked(lv_event_t *e) { (void)e; set_group(P.group + 1); }
static void close_clicked(lv_event_t *e) { (void)e; term_cmds_close(); }

static void show_hints(void)
{
    static const char *const pad[] = { "A", "Put on line", "Y", "Star", "LEFT/RIGHT", "Group", "L1/R1", "Page", "B", "Close" };
    static const char *const keys[] = { "Enter", "Put on line", "S", "Star", "Left/Right", "Group", "Esc", "Close" };
    lv_obj_clean(P.hint);
    if (P.in_window) lv_obj_center(nz_hints(P.hint, keys, 4));    /* window: a USB keyboard and mouse */
    else lv_obj_center(nz_hints(P.hint, pad, 5));
}

static lv_obj_t *arrow(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = th_label(parent, sym, TS_TITLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void term_cmds_open(lv_obj_t *parent, term_cmds_pick_fn pick)
{
    if (P.box) term_cmds_close();
    count_groups();
    fav_load();
    P.pick = pick;
    NzApp *a = apps_get(APP_TERMINAL);
    P.in_window = a && a->in_window;

    P.box = th_box(parent, TS_SCREEN);               /* covers the whole Terminal */
    lv_obj_add_flag(P.box, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(P.box, 0, 0);
    lv_obj_set_size(P.box, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(P.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(P.box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = th_box(P.box, TS_BAR);          /* < group name   3 of 9 >  X */
    lv_obj_set_size(head, LV_PCT(100), 34);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(head, 6, 0);
    lv_obj_set_style_pad_column(head, 6, 0);
    arrow(head, LV_SYMBOL_LEFT, prev_clicked);
    P.title = th_label(head, "", TS_TITLE);
    lv_obj_set_flex_grow(P.title, 1);
    lv_label_set_long_mode(P.title, LV_LABEL_LONG_DOT);
    P.count = th_label(head, "", TS_SMALL);
    arrow(head, LV_SYMBOL_RIGHT, next_clicked);
    lv_obj_t *x = lv_btn_create(head);               /* close (for the mouse) */
    lv_obj_set_size(x, 28, 24);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, close_clicked, LV_EVENT_CLICKED, NULL);

    P.list = nzlist_create(P.box, 0);
    lv_obj_set_width(nzlist_obj(P.list), LV_PCT(100));
    lv_obj_set_flex_grow(nzlist_obj(P.list), 1);
    nzlist_on_activate(P.list, picked, NULL);

    P.detail = th_label(P.box, "", TS_MONO);         /* the example for the selected command */
    lv_obj_set_width(P.detail, LV_PCT(100));
    lv_label_set_long_mode(P.detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(P.detail, 10, 0);
    lv_obj_set_style_pad_ver(P.detail, 4, 0);

    P.hint = th_box(P.box, TS_BAR);
    lv_obj_set_size(P.hint, LV_PCT(100), 24);
    lv_obj_clear_flag(P.hint, LV_OBJ_FLAG_SCROLLABLE);
    show_hints();

    P.group = last_group < term_cmds_group_count() ? last_group : 0;
    fill(0);
}

void term_cmds_close(void)
{
    if (!P.box) return;
    lv_obj_del_async(P.box);                         /* async: we may be inside a click on one of its rows */
    term_cmds_drop();
}

/* The Terminal is closing: forget the list (LVGL deletes its objects together
 * with the Terminal's, so they must not be deleted again later). */
void term_cmds_drop(void)
{
    if (!P.box) return;
    nzlist_free(P.list);
    fav_free();
    memset(&P, 0, sizeof P);
}

int term_cmds_key(int key)
{
    if (!P.box) return 0;
    switch (key) {
    case NZ_KEY_LEFT:  set_group(P.group - 1); break;
    case NZ_KEY_RIGHT: set_group(P.group + 1); break;
    case NZ_KEY_Y:     star_selected(); break;
    case NZ_KEY_B:     term_cmds_close(); break;
    case NZ_KEY_START: nzlist_key(P.list, NZ_KEY_A); break;          /* same as A */
    case NZ_KEY_X:     break;
    default:           nzlist_key(P.list, key); update_detail(); break;   /* up/down, L1/R1, A */
    }
    return 1;
}

void term_cmds_kbd(const KbEvent *ev)
{
    if (!P.box) return;
    switch (ev->key) {
    case KB_UP:    term_cmds_key(NZ_KEY_UP); break;
    case KB_DOWN:  term_cmds_key(NZ_KEY_DOWN); break;
    case KB_LEFT:  term_cmds_key(NZ_KEY_LEFT); break;
    case KB_RIGHT: term_cmds_key(NZ_KEY_RIGHT); break;
    case KB_PGUP:  term_cmds_key(NZ_KEY_L); break;
    case KB_PGDN:  term_cmds_key(NZ_KEY_R); break;
    case KB_ENTER: term_cmds_key(NZ_KEY_A); break;
    case KB_ESC:   term_cmds_close(); break;
    case KB_CHAR:  if (ev->ch == 's' || ev->ch == 'S' || ev->ch == '*') star_selected(); break;
    default: break;
    }
}

/* terminal.c - the NZEUS Terminal's engine (see terminal.h).
 * Sessions: each has its own scrollback, folder, typed line and program.
 * History: shared, saved to the SD card after every command. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "process.h"
#include "settings.h"
#include "terminal.h"

#define NZ_PATH      1024
#define HOME_FOLDER  NZ_SDCARD                  /* NZEUS's own folder (settings.h) */
#define HISTORY_FILE NZ_DIR "/terminal_history"

/* where the output reader is inside an escape code (colours, cursor moves...) */
enum { TEXT, AFTER_ESC, IN_CSI };

typedef struct {
    char (*ring)[TERM_COLS];   /* scrollback: TERM_LINES lines (malloc'd) */
    int  ring_head;            /* next write slot */
    int  ring_count;           /* lines currently stored */
    int  scroll_off;           /* 0 = bottom (live view) */
    /* the program's output, on its way to becoming lines */
    char partial[TERM_COLS];   /* the unfinished last line (e.g. a "Name: " prompt) */
    int  partial_len;
    int  escape;               /* TEXT, AFTER_ESC or IN_CSI */
    int  cr_waiting;           /* '\r' seen: "\r\n" ends the line, '\r' + text starts it again */
    /* typing */
    int  hist_browse;          /* -1 = live input, >=0 = showing history entry */
    char input_buf[TERM_INPUT];
    char saved_input[TERM_INPUT];
    char cwd[NZ_PATH];
    char edit_request[NZ_PATH];   /* from `edit FILE`, picked up by the app */
    bool have_reported;        /* the "[exit code N]" line was written */
    bool exit_requested;       /* `exit` was typed: the app should close */
    Proc proc;                 /* this session's program */
} Session;

static Session *sessions[TERM_SESSIONS];
static int session_count;
static Session *S;             /* the current session (NULL before terminal_init) */
static char prompt_line[TERM_COLS + TERM_INPUT];

/* ---------- history (shared by all sessions) ---------- */
static char *hist[TERM_HIST];          /* oldest first */
static unsigned char hist_keep[TERM_HIST];   /* 1 = written to the SD card */
static int hist_count;

static void hist_clear(void) {
    for (int i = 0; i < hist_count; i++) free(hist[i]);
    hist_count = 0;
}

/* Add a command; returns 1 if it was added. Empty lines and a repeat of the
 * newest one are skipped; past TERM_HIST the oldest is dropped. */
static int hist_add(const char *cmd, int keep) {
    const char *p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    if (hist_count && !strcmp(hist[hist_count - 1], cmd)) return 0;
    char *copy = strdup(cmd);
    if (!copy) return 0;
    if (hist_count == TERM_HIST) {
        free(hist[0]);
        memmove(hist, hist + 1, (TERM_HIST - 1) * sizeof hist[0]);
        memmove(hist_keep, hist_keep + 1, TERM_HIST - 1);
        hist_count--;
    }
    hist[hist_count] = copy;
    hist_keep[hist_count] = (unsigned char)keep;
    hist_count++;
    return 1;
}

/* Write the history: to a .tmp file first, then rename it over the old one,
 * so a power cut never leaves a half-written file. */
static void hist_save(void) {
    const char *tmp = HISTORY_FILE ".tmp";
    mkdir(NZ_DIR, 0755);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < hist_count; i++) if (hist_keep[i]) fprintf(f, "%s\n", hist[i]);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0) { unlink(tmp); return; }
    rename(tmp, HISTORY_FILE);
}

static void hist_load(void) {
    hist_clear();
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) return;
    char line[TERM_INPUT + 2];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        int whole = n && line[n - 1] == '\n';
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n > TERM_INPUT - 1) line[TERM_INPUT - 1] = 0;
        if (!whole && !feof(f)) {               /* a very long line: skip the rest of it */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        }
        hist_add(line, 1);
    }
    fclose(f);
}

/* Lines that should not be written to the card: a Wi-Fi password. */
static int keep_on_card(const char *cmd) {
    return !(strstr(cmd, "wifi-join") && strstr(cmd, "connect"));
}

int terminal_history_count(void) { return hist_count; }
const char *terminal_history_get(int i) { return (i >= 0 && i < hist_count) ? hist[i] : ""; }

/* ---------- scrollback ---------- */
static void line_push(Session *s, const char *text) {
    strncpy(s->ring[s->ring_head], text, TERM_COLS - 1);
    s->ring[s->ring_head][TERM_COLS - 1] = 0;
    s->ring_head = (s->ring_head + 1) % TERM_LINES;
    if (s->ring_count < TERM_LINES) s->ring_count++;
    s->scroll_off = 0;  /* new output returns to live view */
}

/* The unfinished line becomes a finished one (even when it is empty). */
static void end_line(Session *s) {
    s->partial[s->partial_len] = 0;
    line_push(s, s->partial);
    s->partial_len = 0;
}

static void reader_reset(Session *s) { s->partial_len = 0; s->escape = TEXT; s->cr_waiting = 0; }

/* One byte of a program's output. Escape codes (colours, cursor moves) and
 * other control bytes are left out: the screen shows plain text only. */
static void output_byte(Session *s, unsigned char c) {
    if (s->escape == AFTER_ESC) {               /* ESC [ starts a longer code, ESC + anything else is all */
        s->escape = (c == '[') ? IN_CSI : TEXT;
        return;
    }
    if (s->escape == IN_CSI) {                  /* ESC [ ... ends with a byte from '@' to '~' */
        if (c >= '@' && c <= '~') s->escape = TEXT;
        return;
    }
    if (s->cr_waiting) {                        /* a '\r' on its own: the line is written again */
        s->cr_waiting = 0;
        if (c != '\n') s->partial_len = 0;
    }
    switch (c) {
    case 0x1B: s->escape = AFTER_ESC; return;
    case '\r': s->cr_waiting = 1; return;      /* (what it means depends on the next byte) */
    case '\n': end_line(s); return;
    case '\t': c = ' '; break;
    }
    if (c < ' ' || c == 0x7F) return;
    if (s->partial_len == TERM_COLS - 1) end_line(s);   /* too long for one line: go on in the next */
    s->partial[s->partial_len++] = (char)c;
}

static void output_bytes(Session *s, const char *buf, int len) {
    for (int i = 0; i < len; i++) output_byte(s, (unsigned char)buf[i]);
}

static void on_output(void *ctx, const char *buf, int len) { output_bytes((Session *)ctx, buf, len); }

void terminal_output(const char *buf, int len) { if (S) output_bytes(S, buf, len); }

static void clear_session(Session *s) {
    s->ring_count = s->ring_head = s->scroll_off = 0;
    reader_reset(s);
}

void terminal_clear(void) { if (S) clear_session(S); }

/* The line under a finished program: how it ended. */
static void show_result(Session *s, int code) {
    char text[48];
    if (s->partial_len) end_line(s);            /* its last words, if they had no '\n' */
    if (code > 128 && code < 160) snprintf(text, sizeof text, "[stopped by signal %d]", code - 128);
    else snprintf(text, sizeof text, "[exit code %d]", code);
    line_push(s, text);
}

/* "$ command" above a command's output */
static void show_command(Session *s, const char *cmd) {
    char text[TERM_COLS];
    snprintf(text, sizeof text, "$ %s", cmd);
    line_push(s, text);
}

/* ---------- commands the Terminal runs itself ---------- */
static const char *skip_blanks(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool is_folder(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* cd             -> the SD card
 * cd /a/b        -> that folder
 * cd ..          -> one folder up
 * cd name        -> a folder inside the current one */
static void cmd_cd(Session *s, const char *arg) {
    char dest[NZ_PATH];
    arg = skip_blanks(arg);
    if (!*arg) {
        snprintf(dest, sizeof dest, "%s", HOME_FOLDER);
    } else if (!strcmp(arg, "..")) {
        snprintf(dest, sizeof dest, "%s", s->cwd);
        char *last = strrchr(dest, '/');
        if (last && last != dest) *last = 0;
        else snprintf(dest, sizeof dest, "/");
    } else if (*arg == '/') {
        snprintf(dest, sizeof dest, "%s", arg);
    } else if (snprintf(dest, sizeof dest, "%s/%s", s->cwd, arg) >= (int)sizeof dest) {
        line_push(s, "cd: that path is too long");
        return;
    }
    for (size_t n = strlen(dest); n > 1 && dest[n - 1] == '/'; n--) dest[n - 1] = 0;   /* "docs/" (from Tab) = "docs" */
    if (!is_folder(dest)) {
        char text[TERM_COLS];
        snprintf(text, sizeof text, "cd: %.60s is not a folder", arg);
        line_push(s, text);
        return;
    }
    snprintf(s->cwd, sizeof s->cwd, "%s", dest);
}

/* `edit FILE`: ask the app to open its text editor (see terminal_edit_request). */
static void cmd_edit(Session *s, const char *arg) {
    char name[NZ_PATH];
    snprintf(name, sizeof name, "%s", skip_blanks(arg));
    size_t n = strlen(name);
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) name[--n] = 0;
    if (!name[0]) { line_push(s, "usage: edit FILE   (opens the text editor; new files are created)"); return; }
    if (name[0] == '/') snprintf(s->edit_request, sizeof s->edit_request, "%s", name);
    else if (snprintf(s->edit_request, sizeof s->edit_request, "%s/%s", s->cwd, name) >= (int)sizeof s->edit_request)
        s->edit_request[0] = 0;                   /* path too long: ignore */
}

static void cmd_clear(Session *s, const char *arg) { (void)arg; clear_session(s); }

/* `exit`: stop what is running, and tell the app to close the Terminal */
static void cmd_exit(Session *s, const char *arg) {
    (void)arg;
    process_interrupt(&s->proc);
    s->exit_requested = true;
}

static const struct {
    const char *name;
    void (*run)(Session *s, const char *arg);
} builtins[] = {
    { "cd", cmd_cd }, { "clear", cmd_clear }, { "edit", cmd_edit }, { "exit", cmd_exit },
};

/* A built-in command? Then it is run here and true comes back. (An empty line
 * counts as done too.) The names work in any case: the on-screen keyboard
 * may start with a capital. */
static bool run_builtin(Session *s, const char *line) {
    char name[64];
    int end = 0;
    if (sscanf(line, "%63s%n", name, &end) != 1) return true;
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (!strcasecmp(name, builtins[i].name)) { builtins[i].run(s, line + end); return true; }
    return false;
}

const char *terminal_edit_request(void) {
    static char out[NZ_PATH];
    if (!S || !S->edit_request[0]) return NULL;
    snprintf(out, sizeof out, "%s", S->edit_request);
    S->edit_request[0] = 0;
    return out;
}

/* ---------- typed lines ---------- */
/* Show a line the user typed for a running program. If the program printed
 * a prompt without a newline ("Name: ", ">>> "), the input joins that line
 * like on a real terminal; otherwise it gets a "> " marker. */
static void echo_input_line(Session *s, const char *text) {
    char echo[TERM_COLS];
    if (s->partial_len) {
        s->partial[s->partial_len] = 0;
        snprintf(echo, sizeof echo, "%s%s", s->partial, text);
        s->partial_len = 0;
    } else snprintf(echo, sizeof echo, "> %s", text);
    line_push(s, echo);
}

/* A program is running: the typed line is its input. (It goes in the up/down
 * history, but never onto the card: it may be an answer such as a password.) */
static void send_to_program(Session *s, const char *text) {
    char line[TERM_INPUT + 2];
    int len = snprintf(line, sizeof line, "%s\n", text);
    echo_input_line(s, text);
    hist_add(text, 0);
    if (!process_write_stdin(&s->proc, line, len)) line_push(s, "(the program isn't reading what you type)");
}

void terminal_submit(const char *cmd) {
    Session *s = S;
    if (!s) return;
    s->hist_browse = -1;
    if (process_is_running(&s->proc)) { send_to_program(s, cmd); return; }
    show_command(s, cmd);
    int keep = keep_on_card(cmd);
    if (hist_add(cmd, keep) && keep) hist_save();
    if (run_builtin(s, cmd)) return;
    s->have_reported = false;
    if (!process_start_command(&s->proc, cmd, s->cwd)) {
        line_push(s, "(could not start the shell, /bin/sh)");
        s->have_reported = true;
    }
}

void terminal_interrupt(void) {
    if (S) process_interrupt(&S->proc);
}

/* Ctrl+D: tell the running program there is no more input. */
void terminal_send_eof(void) {
    if (!S || !process_is_running(&S->proc)) return;
    echo_input_line(S, "^D");
    process_close_stdin(&S->proc);
}

/* A program the app started itself (not typed): show it like a command. */
void terminal_note_launched_path(const char *path) {
    if (!S) return;
    S->have_reported = false;
    show_command(S, path);
}

/* Every frame: collect every session's output, and say when a program has ended. */
void terminal_update(void) {
    for (int i = 0; i < session_count; i++) {
        Session *s = sessions[i];
        process_poll(&s->proc, on_output, s);
        int code = process_exit_code(&s->proc);
        if (!s->have_reported && code >= 0) {
            show_result(s, code);
            s->have_reported = true;
        }
    }
}

/* ---------- sessions ---------- */
static Session *session_make(const char *cwd) {
    Session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ring = malloc(TERM_LINES * sizeof *s->ring);
    if (!s->ring) { free(s); return NULL; }
    process_init(&s->proc);
    s->hist_browse = -1;
    s->have_reported = true;   /* nothing has started yet */
    snprintf(s->cwd, sizeof s->cwd, "%s", cwd);
    return s;
}

static void session_free(Session *s) {
    process_shutdown(&s->proc);   /* stops a program that is still running */
    free(s->ring);
    free(s);
}

static void free_all_sessions(void) {
    for (int i = 0; i < session_count; i++) session_free(sessions[i]);
    session_count = 0;
    S = NULL;
}

void terminal_init(void) {
    free_all_sessions();
    hist_load();
    Session *s = session_make(HOME_FOLDER);
    if (!s) return;
    sessions[session_count++] = s;
    S = s;
    line_push(s, "NZEUS Deck terminal - /bin/sh");
    line_push(s, "X or F1 = menu: list of commands, copy + paste, sessions");
    line_push(s, "Built in: cd, clear, exit, edit FILE");
    line_push(s, "While a program runs, what you type goes to it");
}

int terminal_session_new(void) {
    if (session_count >= TERM_SESSIONS) return -1;
    Session *s = session_make(S ? S->cwd : HOME_FOLDER);    /* starts in the same folder */
    if (!s) return -1;
    sessions[session_count++] = s;
    S = s;
    char msg[64];
    snprintf(msg, sizeof msg, "Session %d - /bin/sh", session_count);
    line_push(s, msg);
    return session_count - 1;
}

int terminal_session_close(void) {
    int i = terminal_session_current();
    if (i < 0) return session_count;
    session_free(sessions[i]);
    for (int j = i; j < session_count - 1; j++) sessions[j] = sessions[j + 1];
    session_count--;
    S = session_count ? sessions[i > 0 ? i - 1 : 0] : NULL;
    return session_count;
}

int terminal_session_count(void) { return session_count; }

int terminal_session_current(void) {
    for (int i = 0; i < session_count; i++) if (sessions[i] == S) return i;
    return -1;
}

void terminal_session_switch(int index) {
    if (index >= 0 && index < session_count) S = sessions[index];
}

const char *terminal_session_cwd(int index) {
    return (index >= 0 && index < session_count) ? sessions[index]->cwd : "";
}

bool terminal_session_running(int index) {
    return index >= 0 && index < session_count && process_is_running(&sessions[index]->proc);
}

/* true once after `exit` was typed */
bool terminal_exit_pending(void) {
    bool asked = S && S->exit_requested;
    if (asked) S->exit_requested = false;
    return asked;
}

void terminal_free(void) {
    free_all_sessions();
    hist_clear();
}

void terminal_set_cwd(const char *dir) { if (S && dir && dir[0]) snprintf(S->cwd, sizeof S->cwd, "%s", dir); }
const char *terminal_get_cwd(void) { return S ? S->cwd : ""; }

/* ---------- what the app draws ---------- */
/* The unfinished output line (e.g. a ">>> " prompt) is shown as the last line. */
int terminal_line_count(void) { return S ? S->ring_count + (S->partial_len > 0 ? 1 : 0) : 0; }
const char *terminal_line(int index) {
    if (!S) return "";
    if (index == S->ring_count && S->partial_len > 0) { S->partial[S->partial_len] = 0; return S->partial; }
    if (index < 0 || index >= S->ring_count) return "";
    int slot = (S->ring_head - S->ring_count + index + 2 * TERM_LINES) % TERM_LINES;
    return S->ring[slot];
}

/* The output of the newest "$ command": the lines after it, without the
 * closing "[exit code N]" line. */
int terminal_last_output(int *first) {
    int count = terminal_line_count(), start = -1;
    for (int i = count - 1; i >= 0; i--)
        if (!strncmp(terminal_line(i), "$ ", 2)) { start = i + 1; break; }
    if (start < 0) { *first = 0; return 0; }
    int end = count;
    if (end > start && (!strncmp(terminal_line(end - 1), "[exit code ", 11) ||
                        !strncmp(terminal_line(end - 1), "[stopped by signal ", 19))) end--;
    *first = start;
    return end - start;
}

const char *terminal_prompt_line(void) {
    if (!S) return "";
    /* "$" = next line is a shell command, ">" = it goes to the running program */
    snprintf(prompt_line, sizeof prompt_line, "%s %s", process_is_running(&S->proc) ? ">" : "$", S->input_buf);
    return prompt_line;
}

const char *terminal_input_text(void) { return S ? S->input_buf : ""; }

void terminal_set_input(const char *text) {
    if (!S) return;
    snprintf(S->input_buf, sizeof S->input_buf, "%s", text ? text : "");
    S->hist_browse = -1;
}

/* UP = older (stops at the oldest), DOWN = newer, then back to the line you
 * were typing before you started. */
void terminal_history_move(int delta) {
    if (!S || !hist_count) return;
    Session *s = S;
    if (s->hist_browse < 0) {
        if (delta > 0) return;                   /* already on the live line */
        snprintf(s->saved_input, sizeof s->saved_input, "%s", s->input_buf);
        s->hist_browse = hist_count;             /* "one after the newest" */
    }
    s->hist_browse += delta;
    if (s->hist_browse < 0) s->hist_browse = 0;
    if (s->hist_browse >= hist_count) {
        s->hist_browse = -1;
        snprintf(s->input_buf, sizeof s->input_buf, "%s", s->saved_input);
        return;
    }
    snprintf(s->input_buf, sizeof s->input_buf, "%s", hist[s->hist_browse]);
}

const char *terminal_history_step(const char *typed, int delta) {
    if (!S) return "";
    if (S->hist_browse < 0) snprintf(S->input_buf, sizeof S->input_buf, "%s", typed ? typed : "");
    terminal_history_move(delta);
    return S->input_buf;
}

int terminal_scroll_get(void) { return S ? S->scroll_off : 0; }

/* lines scrolled back: 0 (the newest at the bottom) .. all but one */
void terminal_scroll_set(int lines) {
    if (!S) return;
    int most = S->ring_count > 0 ? S->ring_count - 1 : 0;
    S->scroll_off = lines < 0 ? 0 : lines > most ? most : lines;
}

/* ---------- the typed line, from a USB keyboard ---------- */
void terminal_kbd_char(char c) {
    if (!S || !c) return;
    size_t len = strlen(S->input_buf);
    if (len + 1 >= sizeof S->input_buf) return;   /* full */
    S->input_buf[len] = c;
    S->input_buf[len + 1] = 0;
}

void terminal_kbd_backspace(void) {
    if (S && S->input_buf[0]) S->input_buf[strlen(S->input_buf) - 1] = 0;
}

void terminal_kbd_submit(void) {
    if (!S) return;
    char line[TERM_INPUT];
    snprintf(line, sizeof line, "%s", S->input_buf);
    S->input_buf[0] = 0;
    terminal_submit(line);
    if (S) S->hist_browse = -1;
}

bool terminal_running(void) { return S && process_is_running(&S->proc); }

/* ---------- Tab completion ---------- */
static int insert_at(char *line, size_t size, int *cursor, const char *text) {
    size_t len = strlen(line), add = strlen(text);
    if (len + add + 1 > size) return 0;
    memmove(line + *cursor + add, line + *cursor, len - (size_t)*cursor + 1);
    memcpy(line + *cursor, text, add);
    *cursor += (int)add;
    return 1;
}

int terminal_complete(char *line, size_t size, int *cursor) {
    if (!S || *cursor < 0 || *cursor > (int)strlen(line)) return 0;
    int ws = *cursor;                                      /* the word before the cursor */
    while (ws > 0 && line[ws - 1] != ' ') ws--;
    char word[NZ_PATH], dir[NZ_PATH * 2 + 2];
    snprintf(word, sizeof word, "%.*s", *cursor - ws, line + ws);
    const char *slash = strrchr(word, '/');
    const char *prefix = slash ? slash + 1 : word;
    if (!slash) snprintf(dir, sizeof dir, "%s", S->cwd);   /* look in the current folder */
    else if (word[0] == '/') snprintf(dir, sizeof dir, "%.*s", (int)(slash - word) + 1, word);
    else snprintf(dir, sizeof dir, "%s/%.*s", S->cwd, (int)(slash - word), word);
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t plen = strlen(prefix);
    static char names[40][64];                             /* the first 40 matches, to list them */
    char common[256] = "";
    int matches = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && prefix[0] != '.') continue;     /* hidden files only if asked */
        if (strncmp(e->d_name, prefix, plen)) continue;
        if (matches == 0) snprintf(common, sizeof common, "%s", e->d_name);
        else {
            size_t k = 0;                                  /* shorten to the shared start */
            while (common[k] && common[k] == e->d_name[k]) k++;
            common[k] = 0;
        }
        if (matches < 40) snprintf(names[matches], sizeof names[0], "%.63s", e->d_name);
        matches++;
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches == 1) {                                    /* one match: finish the name */
        char full[NZ_PATH * 3], add[260];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, common);
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        snprintf(add, sizeof add, "%s%s", common + plen, is_dir ? "/" : " ");
        insert_at(line, size, cursor, add);
        return 1;
    }
    if (strlen(common) > plen) { insert_at(line, size, cursor, common + plen); return matches; }
    char row[TERM_COLS] = "";                              /* nothing more to add: show the choices */
    for (int i = 0; i < matches && i < 40; i++) {
        size_t used = strlen(row);
        if (used && used + strlen(names[i]) + 2 > 76) { line_push(S, row); row[0] = 0; used = 0; }
        snprintf(row + used, sizeof row - used, "%s%s", used ? "  " : "", names[i]);
    }
    if (row[0]) line_push(S, row);
    if (matches > 40) line_push(S, "(and more)");
    return matches;
}

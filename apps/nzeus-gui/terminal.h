#ifndef NZ_TERMINAL_H
#define NZ_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>

/* terminal.c - the NZEUS Terminal's engine: everything but the drawing.
 *
 * Up to TERM_SESSIONS sessions. Each one has its own /bin/sh process, folder,
 * scrollback (TERM_LINES lines, made when the session opens) and typed line.
 * The functions below work on the CURRENT session (the one on screen);
 * terminal_update() keeps all of them running.
 *
 * The command history (up/down) is shared by all sessions and saved on the
 * SD card after every command (nzeus/terminal_history, one command per line,
 * the last TERM_HIST, no repeats in a row), so it survives a power cut.
 *
 * Every buffer has a fixed size, so a program that prints without end (`yes`)
 * can't use up the memory. The app draws the text itself, with
 * terminal_line_count() and terminal_line(). */

#define TERM_LINES     400   /* scrollback lines per session (400 x 128 = 50 KB) */
#define TERM_COLS      128
#define TERM_HIST      200   /* commands kept in the history */
#define TERM_INPUT     256
#define TERM_SESSIONS    4

void terminal_init(void);                   /* one session; loads the saved history */
void terminal_free(void);                   /* stops every program, frees everything */
void terminal_note_launched_path(const char *path);
bool terminal_exit_pending(void);           /* true once after `exit` was typed: close the app */

void terminal_update(void);                 /* every frame: every session's program output */
bool terminal_running(void);                /* a program runs in the current session */

void terminal_submit(const char *cmd);      /* a typed line: a command, or input for the running program */
void terminal_interrupt(void);              /* Ctrl+C for the running program */
void terminal_send_eof(void);               /* Ctrl+D: end of input for the running program */
const char *terminal_edit_request(void);    /* path from `edit FILE`, once; NULL if none */
void terminal_set_cwd(const char *cwd);
const char *terminal_get_cwd(void);
void terminal_output(const char *buf, int len);  /* add program output to the current session */
void terminal_clear(void);                  /* empty the scrollback (like `clear`) */

/* What the app draws */
int  terminal_line_count(void);
const char *terminal_line(int index);       /* 0 = oldest .. count-1 = newest */
const char *terminal_prompt_line(void);     /* "$ " or "> " + the typed line */

/* The typed line of the current session (kept when switching sessions) */
void terminal_kbd_char(char c);
void terminal_kbd_backspace(void);
void terminal_kbd_submit(void);
const char *terminal_input_text(void);
void terminal_set_input(const char *s);

/* History: up (delta -1, older) / down (delta +1, newer, then back to what you
 * were typing). terminal_history_step starts from `typed` and returns the line
 * to show. */
void terminal_history_move(int delta);
const char *terminal_history_step(const char *typed, int delta);
int  terminal_history_count(void);
const char *terminal_history_get(int i);   /* 0 = oldest */
int  terminal_scroll_get(void);
void terminal_scroll_set(int lines);

/* Sessions */
int  terminal_session_new(void);            /* the new one's number (0..3), -1 = no room */
int  terminal_session_close(void);          /* close the current one; returns how many are left */
int  terminal_session_count(void);
int  terminal_session_current(void);        /* 0-based */
void terminal_session_switch(int index);
const char *terminal_session_cwd(int index);
bool terminal_session_running(int index);

/* The last command's output in the current session: its first line and how
 * many lines (0 = none). */
int  terminal_last_output(int *first);

/* Tab: complete the file or folder name before the cursor (in the current
 * folder, or the folder written in the word). Returns how many names matched;
 * with several, the shared start is added and the names are listed in the
 * output. */
int  terminal_complete(char *line, size_t size, int *cursor);

#endif

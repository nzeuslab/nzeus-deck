#ifndef NZEUS_TERM_CMDS_H
#define NZEUS_TERM_CMDS_H
#include "lvgl.h"
#include "usbkbd.h"

/* The Terminal's "Commands" list: every command the console really has,
 * grouped by what you want to do, each with a one-line plain explanation.
 * Parts you fill in are in CAPITALS (cd FOLDER). Two more groups: Recent
 * (from the history) and Favourites (starred with Y, saved in
 * nzeus/terminal_favourites).
 *
 * The list opens on top of the Terminal (inside its window in desktop mode):
 *   D-pad up/down  move          LEFT/RIGHT  other group     L1/R1  a page
 *   A              put the command on the typed line (it is NOT run)
 *   Y              star / unstar (Favourites)                B      close
 * USB keyboard: arrows, Enter, Esc, S = star. Mouse: click a row or < >. */

typedef struct { const char *cmd, *what, *example; } TermCmd;

/* called with the chosen command, after the list has closed */
typedef void (*term_cmds_pick_fn)(const char *cmd);

void term_cmds_open(lv_obj_t *parent, term_cmds_pick_fn pick);
void term_cmds_close(void);
void term_cmds_drop(void);                   /* the Terminal is closing (its objects go with it) */
int  term_cmds_is_open(void);
int  term_cmds_key(int key);                 /* a button while the list is open (always 1) */
void term_cmds_kbd(const KbEvent *ev);

/* Where the first CAPITALS part of a command starts (-1 = none); its length in *len. */
int  term_cmds_placeholder(const char *cmd, int *len);

/* Favourites (nzeus/terminal_favourites, one command per line) */
int  term_fav_toggle(const char *cmd);       /* 1 = now starred, 0 = removed */
int  term_fav_has(const char *cmd);

/* for tests */
int  term_cmds_total(void);                  /* commands in the table */
const TermCmd *term_cmds_entry(int i);       /* entry i of the table (group names: cmd NULL), NULL past the end */
int  term_cmds_group(void);                  /* the group shown */
int  term_cmds_group_count(void);
const char *term_cmds_group_name(int g);
const char *term_cmds_row_text(int part);    /* selected row as drawn: 0 = command, 1 = explanation */
int  term_cmds_rows(void);

#endif

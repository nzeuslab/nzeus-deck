/* vtwidget.h - a real terminal emulator as an LVGL widget.
 *
 * The terminal logic (escape sequences, colours, cursor moves, scroll
 * regions, the alternate screen, UTF-8...) is done by libvterm (vterm/,
 * MIT licence, the library vim and neovim use). This widget only:
 *   - feeds it the bytes a program prints,
 *   - draws its screen with our 8 x 16 font (box-drawing and block
 *     characters are drawn as lines and rectangles by our own code),
 *   - turns our keyboard events into the bytes a terminal program expects,
 *   - optionally runs the program itself (through a pseudo-terminal on the
 *     PC, through plain pipes on the console, which has no PTYs).
 *
 * Only the rows libvterm reports as changed are redrawn.
 *
 * Typical use, program run by the widget:
 *     lv_obj_t *vt = vtw_create(lv_scr_act(), 80, 30);  // 640 x 480 pixels
 *     char *argv[] = { "/bin/sh", NULL };
 *     vtw_spawn(vt, argv, 1);
 *     every frame:  vtw_poll(vt);  and for each key: vtw_key(vt, ev.key, ev.ch, 0);
 *
 * Typical use, bytes from somewhere else (an SSH connection):
 *     vtw_feed(vt, bytes_received, n);                       // draw them
 *     n = vtw_take_input(vt, buf, sizeof buf); send(buf, n);  // keys + replies
 */
#ifndef NZEUS_VTWIDGET_H
#define NZEUS_VTWIDGET_H

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#define VTW_CELL_W 8     /* every character cell is 8 x 20 pixels: */
#define VTW_CELL_H 20    /* 16 px letters + 2 px of air above and below */

/* modifier bits for vtw_key (the same values libvterm uses) */
#define VTW_MOD_SHIFT 0x01
#define VTW_MOD_ALT   0x02
#define VTW_MOD_CTRL  0x04

/* Extra key codes for vtw_key, for keys usbkbd.h does not have (yet).
 * They start at 100 so they never clash with the KB_* values. */
#define VTW_KEY_INSERT 100
#define VTW_KEY_F(n)   (100 + (n))      /* VTW_KEY_F(1) .. VTW_KEY_F(12) */

/* Create a terminal of cols x rows cells (the widget is cols*8 x rows*20
 * pixels). Returns NULL if memory ran out. Deleting the object (lv_obj_del)
 * stops a running program and frees everything. */
lv_obj_t *vtw_create(lv_obj_t *parent, int cols, int rows);

/* Output of the program: any bytes, any chunk size (a UTF-8 character or an
 * escape sequence may be split between two calls). */
void vtw_feed(lv_obj_t *vt, const char *bytes, size_t len);

/* Bytes that should go to the program: typed keys and the terminal's
 * answers to questions such as "where is the cursor?" (DSR) or "what are
 * you?" (DA). Returns how many bytes were copied into buf (0 = none).
 * When the program was started with vtw_spawn, vtw_poll sends them itself
 * and this returns 0. */
size_t vtw_take_input(lv_obj_t *vt, char *buf, size_t max);

/* A key press. key: a KB_* value from usbkbd.h (KB_CHAR with the character
 * in ch), or VTW_KEY_INSERT / VTW_KEY_F(1..12). mods: VTW_MOD_* bits
 * (e.g. KB_CHAR 'r' with VTW_MOD_CTRL sends Ctrl+R). */
void vtw_key(lv_obj_t *vt, int key, int ch, int mods);

/* Paste text. Programs that asked for "bracketed paste" (vim, Claude Code)
 * get it wrapped in the paste markers so it is not run as typed commands. */
void vtw_paste(lv_obj_t *vt, const char *text, size_t len);

/* Default text and background colour (0xRRGGBB), e.g. from the UI theme.
 * Programs that pick their own colours still get those. */
void vtw_set_colors(lv_obj_t *vt, uint32_t fg, uint32_t bg);

/* Change one of the 16 basic ANSI colours (index 0..15) to 0xRRGGBB. */
void vtw_set_palette(lv_obj_t *vt, int index, uint32_t rgb);

/* Run a program (argv[0] is searched in PATH) with its input and output
 * connected to this terminal. TERM=xterm-256color and COLORTERM=truecolor
 * are set for it. use_pty=1: through a pseudo-terminal (full-screen
 * programs work); if no PTY is available it falls back to pipes. With pipes
 * (use_pty=0) the widget itself echoes typed text, handles Backspace and
 * sends whole lines, like a simple terminal; full-screen programs cannot
 * work that way. Returns 0 when started, -1 on error (or when a program is
 * already running). */
int vtw_spawn(lv_obj_t *vt, char *const argv[], int use_pty);

/* Call every frame: reads what the program printed (never waits), sends
 * pending input to it, notices when it ended. Does nothing without one. */
void vtw_poll(lv_obj_t *vt);

int  vtw_child_running(lv_obj_t *vt);   /* 1 while the spawned program runs */
int  vtw_child_exit_code(lv_obj_t *vt); /* its exit code once ended (128+N = killed by signal N), -1 before */
int  vtw_uses_pty(lv_obj_t *vt);        /* 1 = pseudo-terminal, 0 = pipes / none */
/* Pipes, but the far end has a real terminal (ssh -t from the console): no
 * local echo or line editing; keys are sent as terminal bytes at once. */
void vtw_set_raw_pipe(lv_obj_t *vt, int on);
void vtw_kill(lv_obj_t *vt, int sig);   /* send a signal (e.g. SIGTERM) to the program */

/* Change the size in cells (also tells the program, through the PTY). */
void vtw_resize(lv_obj_t *vt, int cols, int rows);

/* Cursor position (0-based) and the window title the program set (OSC 0/2). */
void vtw_get_cursor(lv_obj_t *vt, int *row, int *col);
const char *vtw_title(lv_obj_t *vt);

/* One row of the screen as UTF-8 text (trailing blanks removed), for tests. */
const char *vtw_row_text(lv_obj_t *vt, int row, char *buf, size_t n);

/* Scrolling back. Lines that scroll off the top of the normal screen are
 * kept (the newest ~500, at most ~100 KB, packed small; full-screen programs
 * on the alternate screen keep none). vtw_scroll moves the view: lines > 0
 * = back to older lines, < 0 = forward again; it returns how far back the
 * view is now (0 = the live screen). New output from the program, a typed
 * key (vtw_key, vtw_paste) or a resize jump back to the live screen. */
int    vtw_scroll(lv_obj_t *vt, int lines);
int    vtw_scroll_offset(lv_obj_t *vt);   /* lines scrolled back, 0 = live */
int    vtw_kept_lines(lv_obj_t *vt);      /* how many lines are kept */
size_t vtw_kept_bytes(lv_obj_t *vt);      /* their memory in bytes */
/* What row `row` of the view shows right now (a kept line while scrolled
 * back), as UTF-8 text, for tests. */
const char *vtw_view_text(lv_obj_t *vt, int row, char *buf, size_t n);

/* All the text there is, oldest first (the Claude app's chat view reads it):
 * lines 0 .. vtw_kept_lines()-1 are the kept lines, then come the rows of the
 * live screen (wherever the view is scrolled to). Arabic and other letters
 * our font lacks are given back as they were printed. */
int vtw_line_count(lv_obj_t *vt);
const char *vtw_line_text(lv_obj_t *vt, int i, char *buf, size_t n);
/* Goes up every time the program prints something (or the size changes):
 * the same number as last time = the text is still the same. */
unsigned long vtw_changes(lv_obj_t *vt);

#endif

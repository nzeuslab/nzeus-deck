#ifndef NZEUS_SHOT_H
#define NZEUS_SHOT_H
#include <stddef.h>

/* Screenshots, and the command channel: one request per file in /tmp/nzeus-cmd
 * (req.N; the answer comes back as done.N), written over SSH (e.g. by Claude
 * on the PC):
 *   "shot /path.png"       save the screen as a PNG
 *   "press A B START"      press buttons
 *   "type some text"       type on the keyboard
 *   "mouse X Y"  "click X Y"  "rclick X Y"  "drag X1 Y1 X2 Y2"  "wheel N"
 *                  the pointer, the same way a USB mouse moves it (desktop mode)
 *   "open NAME"    open an app by its name (car, terminal, wi-fi...; any case)
 *   "home"         back to the home screen (closes the full-screen app, like B)
 * A command writes /tmp/nzeus-cmd/req.<pid> (one line); NZEUS answers in
 * /tmp/nzeus-cmd/done.<pid> with "ok" or "error: ...". */

int  shot_save_png(const char *path);   /* the screen now -> PNG file; 0 = ok */
/* A screenshot into nzeus/shots/ (Screenshot menu item, SELECT + L1).
 * Puts the file's name (e.g. "shot-20261005-142233.png") in name; 0 = saved. */
int  shot_take(char *name, size_t n);
void shot_poll(void);                   /* every frame: answer waiting requests */
extern const char *shot_cmd_dir;        /* "/tmp/nzeus-cmd" (tests change it) */

#endif

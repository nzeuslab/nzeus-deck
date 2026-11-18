#ifndef NZ_EDITOR_H
#define NZ_EDITOR_H

#include <stdbool.h>

/* Text buffer for the NZEUS text editor (NZEUS LAB).
 *
 * Only the text logic lives here (load, edit, save); frogshell.c draws it and
 * feeds it keys. The whole file is kept in RAM as one string per line, so
 * files are limited to EDITOR_MAX_BYTES. Windows-style CRLF line endings and
 * a missing final newline are kept as they were when saving. */

#define EDITOR_MAX_BYTES (512 * 1024)

bool editor_open(const char *path);    /* false: see editor_error() */
bool editor_save(void);                /* writes a temp file, then renames it over the original */
void editor_close(void);               /* frees the buffer */

const char *editor_path(void);
const char *editor_error(void);        /* reason for the last failure */
bool editor_modified(void);            /* changed since opened/saved */

int  editor_line_count(void);
const char *editor_line(int row);      /* text of a line (no newline) */
int  editor_line_len(int row);
int  editor_cursor_row(void);
int  editor_cursor_col(void);

void editor_set_cursor(int row, int col);   /* clamped to the text */
void editor_move(int drow, int dcol);       /* arrows; left/right wrap across lines */
void editor_home(void);
void editor_end(void);

void editor_insert_char(char c);        /* raw insert: '\n' = new line, '\t' = 4 spaces */
void editor_insert_text(const char *s);
void editor_newline(void);              /* split the line, keep the indent (+ smart rules) */
void editor_backspace(void);            /* delete before the cursor (joins lines at col 0) */
void editor_delete(void);               /* delete at the cursor (joins lines at the end) */

/* Smart editing rules, switched on per file type, like VS Code. */
#define EDIT_SMART_PAIRS 1   /* auto-close () [] {} "" '' and step over the closer */
#define EDIT_PYTHON      2   /* ':' indents; return/pass/... and else:/except: dedent */
#define EDIT_BRACES      4   /* a line starting with '}' dedents */
void editor_set_smart(int flags);       /* after editor_open (which resets it to 0) */
void editor_type_char(char c);          /* a typed character, with the smart rules */
void editor_tab(void);                  /* Tab: spaces up to the next multiple of 4 */
void editor_unindent(void);             /* Shift+Tab: back to the previous indent level */

/* Lowest line changed since the last call (the colouring restarts there).
 * Returns a very large number when nothing changed. */
int  editor_take_dirty_row(void);

#endif

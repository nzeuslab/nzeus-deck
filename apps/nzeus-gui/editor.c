/* editor.c - text buffer for the NZEUS text editor (NZEUS LAB).
 * See editor.h. No drawing or input here, so it can be tested on the PC
 * (tests/test_editor.c). */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "editor.h"

typedef struct { char *s; int len, cap; } Line;   /* one line of text, NUL-terminated */

static Line *lines;
static int   nlines, lines_cap;
static int   cur_row, cur_col;
static int   want_col;        /* column to return to when moving up/down past short lines */
static bool  modified;
static bool  crlf;            /* file used Windows "\r\n" line endings */
static bool  final_newline;   /* file ended with a newline */
static char  path_buf[1024];
static char  err[160];
static int   smart;           /* EDIT_* smart-editing rules for this file type */
static int   dirty_row = INT_MAX;   /* lowest line changed (for re-colouring) */

static void mark_dirty(int row) { if (row < 0) row = 0; if (row < dirty_row) dirty_row = row; }

/* ---- small helpers ---- */

static bool line_reserve(Line *l, int need) {
    if (need + 1 <= l->cap) return true;
    int cap = l->cap ? l->cap : 32;
    while (cap < need + 1) cap *= 2;
    char *p = realloc(l->s, (size_t)cap);
    if (!p) return false;
    l->s = p;
    l->cap = cap;
    return true;
}

/* Insert a new line at index idx, holding a copy of s[0..len). */
static bool line_insert(int idx, const char *s, int len) {
    if (nlines + 1 > lines_cap) {
        int cap = lines_cap ? lines_cap * 2 : 64;
        Line *p = realloc(lines, (size_t)cap * sizeof *p);
        if (!p) return false;
        lines = p;
        lines_cap = cap;
    }
    Line l = { NULL, 0, 0 };
    if (!line_reserve(&l, len)) return false;
    memcpy(l.s, s, (size_t)len);
    l.s[len] = 0;
    l.len = len;
    memmove(lines + idx + 1, lines + idx, (size_t)(nlines - idx) * sizeof *lines);
    lines[idx] = l;
    nlines++;
    return true;
}

static void line_remove(int idx) {
    free(lines[idx].s);
    memmove(lines + idx, lines + idx + 1, (size_t)(nlines - idx - 1) * sizeof *lines);
    nlines--;
}

static void clamp_cursor(void) {
    if (cur_row < 0) cur_row = 0;
    if (cur_row >= nlines) cur_row = nlines - 1;
    if (cur_col < 0) cur_col = 0;
    if (cur_col > lines[cur_row].len) cur_col = lines[cur_row].len;
}

/* ---- open / save / close ---- */

void editor_close(void) {
    for (int i = 0; i < nlines; i++) free(lines[i].s);
    free(lines);
    lines = NULL;
    nlines = lines_cap = 0;
    cur_row = cur_col = want_col = 0;
    modified = false;
}

bool editor_open(const char *path) {
    editor_close();
    err[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, sizeof err, "Cannot open: %s", strerror(errno)); return false; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) { fclose(f); snprintf(err, sizeof err, "Not a file"); return false; }
    if (st.st_size > EDITOR_MAX_BYTES) { fclose(f); snprintf(err, sizeof err, "Too big to edit (max %d KB)", EDITOR_MAX_BYTES / 1024); return false; }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size + 1);
    if (!data) { fclose(f); snprintf(err, sizeof err, "Out of memory"); return false; }
    size_t got = fread(data, 1, size, f);
    fclose(f);
    if (got != size) { free(data); snprintf(err, sizeof err, "Read error"); return false; }
    if (memchr(data, 0, size)) { free(data); snprintf(err, sizeof err, "Not a text file"); return false; }

    final_newline = size == 0 || data[size - 1] == '\n';   /* new/empty files get one */
    crlf = false;
    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            if (i == size && start == size && size > 0) break;   /* text ended with '\n': no extra empty line */
            size_t end = i;
            if (end > start && data[end - 1] == '\r') { end--; if (i < size) crlf = true; }
            if (!line_insert(nlines, data + start, (int)(end - start))) {
                free(data); editor_close(); snprintf(err, sizeof err, "Out of memory"); return false;
            }
            start = i + 1;
        }
    }
    free(data);
    if (nlines == 0 && !line_insert(0, "", 0)) { snprintf(err, sizeof err, "Out of memory"); return false; }
    snprintf(path_buf, sizeof path_buf, "%s", path);
    cur_row = cur_col = want_col = 0;
    modified = false;
    smart = 0;
    dirty_row = 0;                    /* everything needs colouring */
    return true;
}

bool editor_save(void) {
    char tmp[1100];
    err[0] = 0;
    /* Write to a temporary file first: if anything fails half-way, the
     * original file is still intact. Then rename it over the original. */
    snprintf(tmp, sizeof tmp, "%s.saving", path_buf);
    FILE *f = fopen(tmp, "wb");
    if (!f) { snprintf(err, sizeof err, "Save failed: %s", strerror(errno)); return false; }
    const char *eol = crlf ? "\r\n" : "\n";
    bool ok = true;
    for (int i = 0; i < nlines && ok; i++) {
        if (lines[i].len && fwrite(lines[i].s, 1, (size_t)lines[i].len, f) != (size_t)lines[i].len) ok = false;
        bool last = i == nlines - 1;
        if (ok && (!last || final_newline) && fputs(eol, f) == EOF) ok = false;
    }
    if (fflush(f) != 0) ok = false;
    if (ok) fsync(fileno(f));        /* make sure it really reached the SD card */
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path_buf) != 0) {
        snprintf(err, sizeof err, "Save failed: %s", strerror(errno));
        unlink(tmp);
        return false;
    }
    modified = false;
    return true;
}

/* ---- read access ---- */

const char *editor_path(void)  { return path_buf; }
const char *editor_error(void) { return err; }
bool editor_modified(void)     { return modified; }
int  editor_line_count(void)   { return nlines; }
const char *editor_line(int row) { return row >= 0 && row < nlines ? lines[row].s : ""; }
int  editor_line_len(int row)  { return row >= 0 && row < nlines ? lines[row].len : 0; }
int  editor_cursor_row(void)   { return cur_row; }
int  editor_cursor_col(void)   { return cur_col; }

/* ---- cursor movement ---- */

void editor_set_cursor(int row, int col) {
    if (!nlines) return;
    cur_row = row;
    cur_col = col;
    clamp_cursor();
    want_col = cur_col;
}

void editor_move(int drow, int dcol) {
    if (!nlines) return;
    if (drow) {                        /* up/down: try to stay in the same column */
        cur_row += drow;
        if (cur_row < 0) cur_row = 0;
        if (cur_row >= nlines) cur_row = nlines - 1;
        cur_col = want_col < lines[cur_row].len ? want_col : lines[cur_row].len;
        return;
    }
    if (dcol < 0) {                    /* left: at the start, jump to the end of the line above */
        if (cur_col > 0) cur_col--;
        else if (cur_row > 0) { cur_row--; cur_col = lines[cur_row].len; }
    } else if (dcol > 0) {             /* right: at the end, jump to the start of the next line */
        if (cur_col < lines[cur_row].len) cur_col++;
        else if (cur_row < nlines - 1) { cur_row++; cur_col = 0; }
    }
    want_col = cur_col;
}

void editor_home(void) { if (nlines) { cur_col = 0; want_col = 0; } }
void editor_end(void)  { if (nlines) { cur_col = lines[cur_row].len; want_col = cur_col; } }

/* ---- editing ---- */

void editor_insert_char(char c) {
    if (!nlines) return;
    if (c == '\n') { editor_newline(); return; }
    if (c == '\t') { editor_insert_text("    "); return; }   /* Python style: 4 spaces */
    Line *l = &lines[cur_row];
    if (!line_reserve(l, l->len + 1)) return;
    memmove(l->s + cur_col + 1, l->s + cur_col, (size_t)(l->len - cur_col + 1));  /* +1 moves the NUL too */
    l->s[cur_col] = c;
    l->len++;
    cur_col++;
    want_col = cur_col;
    modified = true;
    mark_dirty(cur_row);
}

void editor_insert_text(const char *s) {
    for (; s && *s; s++) editor_insert_char(*s);
}

/* ---- smart-editing helpers ---- */

static const char *py_block_end[] = { "return", "pass", "break", "continue", "raise", NULL };
static const char *py_dedent_kw[] = { "else", "elif", "except", "finally", NULL };

static int is_word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Does s[0..n) start with `word` as a whole word? */
static int starts_with_word(const char *s, int n, const char *word) {
    int m = (int)strlen(word);
    return n >= m && !memcmp(s, word, (size_t)m) && (n == m || !is_word_char(s[m]));
}
static int starts_with_any(const char *s, int n, const char **words) {
    for (; *words; words++) if (starts_with_word(s, n, *words)) return 1;
    return 0;
}

static int line_indent(int row) { int i = 0; while (i < lines[row].len && lines[row].s[i] == ' ') i++; return i; }
static int line_blank(int row) {
    for (int i = 0; i < lines[row].len; i++) if (lines[row].s[i] != ' ' && lines[row].s[i] != '\t') return 0;
    return 1;
}
/* Indent of the nearest non-empty line above `row`. */
static int prev_code_indent(int row) {
    for (int r = row - 1; r >= 0; r--) if (!line_blank(r)) return line_indent(r);
    return 0;
}

/* Give line `row` exactly `new_indent` leading spaces (cursor follows). */
static void reindent_line(int row, int new_indent) {
    Line *l = &lines[row];
    int ind = line_indent(row), diff = new_indent - ind;
    if (!diff) return;
    if (diff > 0 && !line_reserve(l, l->len + diff)) return;
    memmove(l->s + new_indent, l->s + ind, (size_t)(l->len - ind + 1));
    if (diff > 0) memset(l->s, ' ', (size_t)new_indent);
    l->len += diff;
    if (row == cur_row) {
        cur_col += diff;
        if (cur_col < 0) cur_col = 0;
        if (cur_col > l->len) cur_col = l->len;
        want_col = cur_col;
    }
    modified = true;
    mark_dirty(row);
}

static int is_empty_pair(char a, char b) {
    return (a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') ||
           (a == '"' && b == '"') || (a == '\'' && b == '\'');
}
/* Auto-close a bracket/quote only before a "boundary" (end, space, closer...). */
static int boundary(char next) {
    return next == 0 || next == ' ' || next == ')' || next == ']' || next == '}' ||
           next == ',' || next == ':' || next == ';';
}

void editor_newline(void) {
    if (!nlines) return;
    Line *l = &lines[cur_row];
    /* Auto-indent: copy this line's leading spaces, then adjust:
     *  +4 after a Python line ending in ':' (if/for/def/...) or after '(' '[' '{'
     *  -4 after return/pass/break/continue/raise (that block is finished)
     *  Enter between "()" "[]" "{}": the closer goes on its own line below. */
    int indent = 0;
    while (indent < l->len && indent < cur_col && (l->s[indent] == ' ' || l->s[indent] == '\t')) indent++;
    int end = cur_col;
    while (end > 0 && (l->s[end - 1] == ' ' || l->s[end - 1] == '\t')) end--;
    char last = end > 0 ? l->s[end - 1] : 0;                 /* last character before the cursor */
    char next = cur_col < l->len ? l->s[cur_col] : 0;        /* character right after it */
    int keep = indent;                                        /* leading characters copied */
    int deeper = 0;                                           /* extra spaces */
    if ((smart & EDIT_PYTHON) && last == ':') deeper = 4;
    if (smart && (last == '(' || last == '[' || last == '{')) deeper = 4;
    if ((smart & EDIT_PYTHON) && !deeper && end > indent &&
        starts_with_any(l->s + indent, end - indent, py_block_end))
        keep = indent >= 4 ? indent - 4 : 0;
    int split = smart && is_empty_pair(last, next) && last != '"' && last != '\'';

    int tail_len = l->len - cur_col;
    int new_len = keep + deeper + (split ? 0 : tail_len);
    char *buf = malloc((size_t)new_len + 1);
    if (!buf) return;
    memcpy(buf, l->s, (size_t)keep);                          /* indent */
    memset(buf + keep, ' ', (size_t)deeper);
    if (!split) memcpy(buf + keep + deeper, l->s + cur_col, (size_t)tail_len);  /* text right of the cursor */
    if (!line_insert(cur_row + 1, buf, new_len)) { free(buf); return; }
    free(buf);
    if (split) {                                              /* "{|}": the '}' gets its own line */
        l = &lines[cur_row];
        char *closer = malloc((size_t)(indent + tail_len) + 1);
        if (closer) {
            memcpy(closer, l->s, (size_t)indent);
            memcpy(closer + indent, l->s + cur_col, (size_t)tail_len);
            line_insert(cur_row + 2, closer, indent + tail_len);
            free(closer);
        }
    }
    l = &lines[cur_row];                                      /* lines may have moved in memory */
    l->len = cur_col;                                         /* cut this line at the cursor */
    l->s[l->len] = 0;
    mark_dirty(cur_row);
    cur_row++;
    cur_col = keep + deeper;
    want_col = cur_col;
    modified = true;
}

void editor_backspace(void) {
    if (!nlines) return;
    if (cur_col > 0) {
        Line *l = &lines[cur_row];
        int n = 1;
        int only_spaces = 1;
        for (int j = 0; j < cur_col; j++) if (l->s[j] != ' ') { only_spaces = 0; break; }
        if (smart && only_spaces) {                   /* inside the indent: remove one level */
            n = cur_col % 4;
            if (n == 0) n = 4;
        } else if ((smart & EDIT_SMART_PAIRS) && cur_col < l->len && is_empty_pair(l->s[cur_col - 1], l->s[cur_col])) {
            memmove(l->s + cur_col, l->s + cur_col + 1, (size_t)(l->len - cur_col));   /* "(|)": the ')' goes too */
            l->len--;
        }
        memmove(l->s + cur_col - n, l->s + cur_col, (size_t)(l->len - cur_col + 1));
        l->len -= n;
        cur_col -= n;
        mark_dirty(cur_row);
    } else if (cur_row > 0) {                  /* at the line start: join with the line above */
        mark_dirty(cur_row - 1);
        Line *prev = &lines[cur_row - 1], *l = &lines[cur_row];
        int join_at = prev->len;
        if (!line_reserve(prev, prev->len + l->len)) return;
        memcpy(prev->s + prev->len, l->s, (size_t)l->len + 1);
        prev->len += l->len;
        line_remove(cur_row);
        cur_row--;
        cur_col = join_at;
    } else return;                              /* very start of the file: nothing to delete */
    want_col = cur_col;
    modified = true;
}

void editor_delete(void) {
    if (!nlines) return;
    Line *l = &lines[cur_row];
    if (cur_col < l->len) {
        memmove(l->s + cur_col, l->s + cur_col + 1, (size_t)(l->len - cur_col));
        l->len--;
    } else if (cur_row < nlines - 1) {          /* at the line end: pull the next line up */
        Line *next = &lines[cur_row + 1];
        if (!line_reserve(l, l->len + next->len)) return;
        memcpy(l->s + l->len, next->s, (size_t)next->len + 1);
        l->len += next->len;
        line_remove(cur_row + 1);
    } else return;                              /* very end of the file */
    modified = true;
    mark_dirty(cur_row);
}

/* ---- smart typing ---- */

void editor_set_smart(int flags) { smart = flags; }

int editor_take_dirty_row(void) {
    int d = dirty_row;
    dirty_row = INT_MAX;
    return d;
}

/* Python: typing the ':' of "else:" "elif ...:" "except ...:" "finally:"
 * moves that line one level back, out of the block above (like VS Code). */
static void dedent_block_keyword(void) {
    Line *l = &lines[cur_row];
    int ind = line_indent(cur_row);
    if (ind < 4 || !starts_with_any(l->s + ind, l->len - ind, py_dedent_kw)) return;
    if (ind >= prev_code_indent(cur_row)) reindent_line(cur_row, ind - 4);
}

/* C/shell: a '}' typed as the first thing on a line moves one level back. */
static void dedent_closing_brace(void) {
    Line *l = &lines[cur_row];
    int ind = line_indent(cur_row);
    if (ind >= 4 && cur_col == ind + 1 && l->s[ind] == '}') reindent_line(cur_row, ind - 4);
}

void editor_type_char(char c) {
    if (!nlines) return;
    Line *l = &lines[cur_row];
    char next = cur_col < l->len ? l->s[cur_col] : 0;
    char prev = cur_col > 0 ? l->s[cur_col - 1] : 0;
    if (smart & EDIT_SMART_PAIRS) {
        /* typing a closer that is already right there: just step over it */
        if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && next == c) {
            cur_col++;
            want_col = cur_col;
            return;
        }
        char closer = c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : 0;
        if (closer && boundary(next)) {                       /* ( -> (|) */
            editor_insert_char(c); editor_insert_char(closer);
            cur_col--; want_col = cur_col;
            return;
        }
        /* " -> "|" , but not after a letter (so "don't" still works) */
        if ((c == '"' || c == '\'') && !is_word_char(prev) && prev != c && boundary(next)) {
            editor_insert_char(c); editor_insert_char(c);
            cur_col--; want_col = cur_col;
            return;
        }
    }
    editor_insert_char(c);
    if (c == ':' && (smart & EDIT_PYTHON)) dedent_block_keyword();
    if (c == '}' && (smart & EDIT_BRACES)) dedent_closing_brace();
}

void editor_tab(void) {
    if (!nlines) return;
    int n = 4 - cur_col % 4;                    /* to the next multiple of 4 */
    while (n--) editor_insert_char(' ');
}

void editor_unindent(void) {
    if (!nlines) return;
    int ind = line_indent(cur_row);
    if (ind) reindent_line(cur_row, ((ind - 1) / 4) * 4);    /* back to the previous level */
}

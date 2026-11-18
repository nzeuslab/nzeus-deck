/* highlight.c - syntax colouring for the NZEUS text editor. See highlight.h.
 *
 * The state passed from line to line is one small number:
 *   bits 0-1: something still open  (1 = Python ''' / C comment / shell '
 *                                     2 = Python """ / shell ")
 *   bits 2-7: bracket depth (0..63), for the depth-coloured brackets */
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "highlight.h"

#define ST_OPEN(s)     ((s) & 3)
#define ST_DEPTH(s)    (((s) >> 2) & 63)
static int st_make(int open, int depth) { if (depth < 0) depth = 0; if (depth > 63) depth = 63; return open | (depth << 2); }

/* ---- word lists ---- */
static const char *py_control[] = { "if", "elif", "else", "for", "while", "break", "continue", "return",
    "try", "except", "finally", "raise", "with", "as", "import", "from", "pass", "yield", "await",
    "async", "assert", "del", "global", "nonlocal", "match", "case", NULL };
static const char *py_keyword[] = { "def", "class", "lambda", "and", "or", "not", "in", "is",
    "True", "False", "None", "self", NULL };
static const char *py_function[] = { "print", "input", "len", "range", "open", "isinstance", "enumerate",
    "zip", "map", "filter", "sorted", "sum", "min", "max", "abs", "round", "any", "all", "super",
    "repr", "chr", "ord", "hex", "bin", "oct", "iter", "next", "format", "hash", "id", "dir",
    "getattr", "setattr", "hasattr", "callable", "reversed", "exit", "quit", NULL };
static const char *py_type[] = { "int", "str", "float", "list", "dict", "set", "tuple", "bool",
    "bytes", "object", "type", "Exception", NULL };
static const char *sh_control[] = { "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
    "done", "case", "esac", "in", "function", "return", "exit", "break", "continue", NULL };
static const char *sh_command[] = { "echo", "printf", "read", "cd", "export", "local", "set", "unset",
    "shift", "source", "test", "eval", "exec", "trap", "wait", "sleep", "cat", "ls", "grep", "sed",
    "awk", "mkdir", "rm", "cp", "mv", "chmod", "sync", "mount", "kill", "python", NULL };
static const char *c_control[] = { "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "sizeof", NULL };
static const char *c_keyword[] = { "int", "char", "short", "long", "unsigned", "signed", "float",
    "double", "void", "bool", "struct", "union", "enum", "typedef", "static", "const", "extern",
    "volatile", "register", "inline", "true", "false", "NULL", "uint8_t", "uint16_t", "uint32_t",
    "int8_t", "int16_t", "int32_t", "size_t", NULL };

static int in_list(const char *w, int n, const char **list) {
    for (; *list; list++) if ((int)strlen(*list) == n && !memcmp(*list, w, (size_t)n)) return 1;
    return 0;
}
static int ident_start(int c) { return isalpha(c) || c == '_'; }
static int ident_char(int c)  { return isalnum(c) || c == '_'; }
static int ends_with(const char *w, int n, const char *end) {
    int m = (int)strlen(end);
    return n > m && !memcmp(w + n - m, end, (size_t)m);
}

/* Brackets: opening ones take the colour of the current depth, then go one
 * deeper; closing ones come back up first, so a pair always matches. */
static int bracket(char c, int *depth, unsigned char *k) {
    if (c == '(' || c == '[' || c == '{') { *k = (unsigned char)(HL_BRACKET1 + *depth % 3); (*depth)++; return 1; }
    if (c == ')' || c == ']' || c == '}') { if (*depth > 0) (*depth)--; *k = (unsigned char)(HL_BRACKET1 + *depth % 3); return 1; }
    return 0;
}

/* Is the next non-space character after s[i] an opening '('? (a call) */
static int followed_by_paren(const char *s, int len, int i) {
    while (i < len && s[i] == ' ') i++;
    return i < len && s[i] == '(';
}

/* A number: 42, 3.14, 1e-5, 0xFF, 1_000 */
static int number(const char *s, int len, int i, unsigned char *k) {
    int start = i;
    while (i < len && (ident_char((unsigned char)s[i]) || s[i] == '.' ||
           ((s[i] == '+' || s[i] == '-') && i > start && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
        k[i++] = HL_NUMBER;
    return i;
}

/* ---- Python ---- */
static int hl_python(const char *s, int len, int state, unsigned char *k) {
    int open = ST_OPEN(state), depth = ST_DEPTH(state), i = 0;
    int after = 0;                         /* 1 = previous word was "def", 2 = "class" */
    int first = 1;                         /* still at the first token of the line */
    while (i < len) {
        if (open) {                        /* inside a ''' or """ string from an earlier line */
            char q = open == 1 ? '\'' : '"';
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len) { k[i] = k[i + 1] = HL_STRING; i += 2; continue; }
                if (s[i] == q && i + 2 < len && s[i + 1] == q && s[i + 2] == q) {
                    k[i] = k[i + 1] = k[i + 2] = HL_STRING; i += 3; open = 0; break;
                }
                k[i++] = HL_STRING;
            }
            continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == '#') { while (i < len) k[i++] = HL_COMMENT; break; }
        /* strings, with optional prefix letters: r"..." b'...' f"..." rb"..." */
        int p = i;
        while (p < len && p - i < 2 && s[p] && strchr("rRbBfFuU", s[p])) p++;
        if (p < len && (s[p] == '\'' || s[p] == '"')) {
            char q = s[p];
            for (int j = i; j < p; j++) k[j] = HL_STRING;
            if (p + 2 < len && s[p + 1] == q && s[p + 2] == q) {   /* ''' or """: may span lines */
                k[p] = k[p + 1] = k[p + 2] = HL_STRING;
                i = p + 3;
                open = q == '\'' ? 1 : 2;
                after = 0; first = 0;
                continue;
            }
            k[p] = HL_STRING;
            i = p + 1;
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len) { k[i] = k[i + 1] = HL_STRING; i += 2; continue; }
                k[i] = HL_STRING;
                if (s[i++] == q) break;
            }
            after = 0; first = 0;
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1]))) {
            i = number(s, len, i, k); after = 0; first = 0; continue;
        }
        if (ident_start(c)) {
            int st = i;
            while (i < len && ident_char((unsigned char)s[i])) i++;
            int n = i - st;
            HlKind kind = HL_TEXT;
            if (after == 1) kind = HL_FUNCTION;                     /* def NAME */
            else if (after == 2) kind = HL_CLASS;                   /* class NAME */
            else if (in_list(s + st, n, py_control)) kind = HL_CONTROL;
            else if (in_list(s + st, n, py_keyword)) kind = HL_KEYWORD;
            else if (in_list(s + st, n, py_type) || ends_with(s + st, n, "Error") ||
                     ends_with(s + st, n, "Exception") || ends_with(s + st, n, "Warning")) kind = HL_CLASS;
            else if (in_list(s + st, n, py_function) || followed_by_paren(s, len, i)) kind = HL_FUNCTION;
            after = (n == 3 && !memcmp(s + st, "def", 3)) ? 1 : (n == 5 && !memcmp(s + st, "class", 5)) ? 2 : 0;
            for (int j = st; j < i; j++) k[j] = (unsigned char)kind;
            first = 0;
            continue;
        }
        if (c == '@' && first) {                                  /* @decorator */
            k[i++] = HL_FUNCTION;
            while (i < len && (ident_char((unsigned char)s[i]) || s[i] == '.')) k[i++] = HL_FUNCTION;
            first = 0;
            continue;
        }
        if (bracket((char)c, &depth, &k[i])) { i++; after = 0; first = 0; continue; }
        if (c != ' ' && c != '\t') { after = 0; first = 0; }
        k[i++] = HL_TEXT;
    }
    return st_make(open, depth);
}

/* ---- shell scripts ---- */
static int hl_shell(const char *s, int len, int state, unsigned char *k) {
    int open = ST_OPEN(state), depth = ST_DEPTH(state), i = 0;
    int command = 1;                       /* at a spot where a command name can start */
    while (i < len) {
        if (open) {                        /* inside '...' (1) or "..." (2), maybe from an earlier line */
            char q = open == 1 ? '\'' : '"';
            while (i < len) {
                if (q == '"' && s[i] == '\\' && i + 1 < len) { k[i] = k[i + 1] = HL_STRING; i += 2; continue; }
                if (q == '"' && s[i] == '$' && i + 1 < len && (ident_start((unsigned char)s[i + 1]) || s[i + 1] == '{')) {
                    k[i++] = HL_VARIABLE;                         /* "$NAME" inside a string */
                    if (s[i] == '{') { while (i < len && s[i] != '}') k[i++] = HL_VARIABLE; if (i < len) k[i++] = HL_VARIABLE; }
                    else while (i < len && ident_char((unsigned char)s[i])) k[i++] = HL_VARIABLE;
                    continue;
                }
                k[i] = HL_STRING;
                if (s[i++] == q) { open = 0; break; }
            }
            command = 0;
            continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) { while (i < len) k[i++] = HL_COMMENT; break; }
        if (c == '\'' || c == '"') { k[i++] = HL_STRING; open = c == '\'' ? 1 : 2; continue; }
        if (c == '$') {                                           /* $NAME ${NAME} $1 $? */
            k[i++] = HL_VARIABLE;
            if (i < len && s[i] == '{') { while (i < len && s[i] != '}') k[i++] = HL_VARIABLE; if (i < len) k[i++] = HL_VARIABLE; }
            else if (i < len && (isdigit((unsigned char)s[i]) || strchr("?#@*!$", s[i]))) k[i++] = HL_VARIABLE;
            else while (i < len && ident_char((unsigned char)s[i])) k[i++] = HL_VARIABLE;
            command = 0;
            continue;
        }
        if (isdigit(c) && (i == 0 || !ident_char((unsigned char)s[i - 1]))) { i = number(s, len, i, k); command = 0; continue; }
        if (ident_start(c)) {
            int st = i;
            while (i < len && (ident_char((unsigned char)s[i]) || s[i] == '-' || s[i] == '.')) i++;
            int n = i - st;
            HlKind kind = HL_TEXT;
            if (in_list(s + st, n, sh_control)) kind = HL_CONTROL;
            else if (command && (in_list(s + st, n, sh_command) || followed_by_paren(s, len, i))) kind = HL_FUNCTION;
            for (int j = st; j < i; j++) k[j] = (unsigned char)kind;
            command = kind == HL_CONTROL;                         /* "if grep ..." / "then echo" */
            continue;
        }
        if (bracket((char)c, &depth, &k[i])) { i++; continue; }
        if (c == ';' || c == '|' || c == '&') command = 1;        /* a new command starts */
        k[i++] = HL_TEXT;
    }
    return st_make(open, depth);
}

/* ---- C / C++ / Arduino ---- */
static int hl_c(const char *s, int len, int state, unsigned char *k) {
    int open = ST_OPEN(state), depth = ST_DEPTH(state), i = 0;
    int first = 1;
    while (i < len) {
        if (open) {                        /* inside a block comment from an earlier line */
            while (i < len) {
                if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') { k[i] = k[i + 1] = HL_COMMENT; i += 2; open = 0; break; }
                k[i++] = HL_COMMENT;
            }
            continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == '/' && i + 1 < len && s[i + 1] == '/') { while (i < len) k[i++] = HL_COMMENT; break; }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') { k[i] = k[i + 1] = HL_COMMENT; i += 2; open = 1; continue; }
        if (c == '#' && first) {                                  /* #include / #define */
            k[i++] = HL_CONTROL;
            while (i < len && (s[i] == ' ' || isalpha((unsigned char)s[i]))) k[i++] = HL_CONTROL;
            if (i < len && s[i] == '<') { while (i < len && s[i] != '>') k[i++] = HL_STRING; if (i < len) k[i++] = HL_STRING; }
            first = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = (char)c;
            k[i++] = HL_STRING;
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len) { k[i] = k[i + 1] = HL_STRING; i += 2; continue; }
                k[i] = HL_STRING;
                if (s[i++] == q) break;
            }
            first = 0;
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1]))) { i = number(s, len, i, k); first = 0; continue; }
        if (ident_start(c)) {
            int st = i;
            while (i < len && ident_char((unsigned char)s[i])) i++;
            int n = i - st;
            HlKind kind = HL_TEXT;
            if (in_list(s + st, n, c_control)) kind = HL_CONTROL;
            else if (in_list(s + st, n, c_keyword)) kind = HL_KEYWORD;
            else if (followed_by_paren(s, len, i)) kind = HL_FUNCTION;
            for (int j = st; j < i; j++) k[j] = (unsigned char)kind;
            first = 0;
            continue;
        }
        if (bracket((char)c, &depth, &k[i])) { i++; first = 0; continue; }
        if (c != ' ' && c != '\t') first = 0;
        k[i++] = HL_TEXT;
    }
    return st_make(open, depth);
}

/* ---- public ---- */

HlLang hl_language_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/')) return HL_LANG_NONE;
    dot++;
    if (!strcasecmp(dot, "py")) return HL_LANG_PYTHON;
    if (!strcasecmp(dot, "sh") || !strcasecmp(dot, "bash")) return HL_LANG_SHELL;
    if (!strcasecmp(dot, "c") || !strcasecmp(dot, "h") || !strcasecmp(dot, "cpp") ||
        !strcasecmp(dot, "hpp") || !strcasecmp(dot, "ino")) return HL_LANG_C;
    return HL_LANG_NONE;
}

int hl_line(HlLang lang, const char *s, int len, int state, unsigned char *k) {
    switch (lang) {
    case HL_LANG_PYTHON: return hl_python(s, len, state, k);
    case HL_LANG_SHELL:  return hl_shell(s, len, state, k);
    case HL_LANG_C:      return hl_c(s, len, state, k);
    default:
        memset(k, HL_TEXT, (size_t)len);
        return 0;
    }
}

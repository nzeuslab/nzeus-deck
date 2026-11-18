#ifndef NZ_HIGHLIGHT_H
#define NZ_HIGHLIGHT_H

/* Syntax colouring for the NZEUS text editor (NZEUS LAB).
 * Works one line at a time: it says what each character is (keyword,
 * string, comment, ...) and frogshell.c picks the colour - the same colours
 * as VS Code's "Dark+" theme. Pure logic, tested on the PC
 * (tests/test_highlight.c). */

typedef enum { HL_LANG_NONE, HL_LANG_PYTHON, HL_LANG_SHELL, HL_LANG_C } HlLang;

/* What a character is. */
typedef enum {
    HL_TEXT,                       /* plain code: names, operators */
    HL_CONTROL,                    /* if for while return import ... (purple) */
    HL_KEYWORD,                    /* def class True None int static ... (blue) */
    HL_STRING,                     /* "text" 'text' (orange) */
    HL_COMMENT,                    /* # comment, // comment (green) */
    HL_NUMBER,                     /* 42 3.14 0xFF (light green) */
    HL_FUNCTION,                   /* print( my_function( def NAME (yellow) */
    HL_CLASS,                      /* class NAME, ValueError, int (teal) */
    HL_VARIABLE,                   /* shell $VARIABLES (light blue) */
    HL_BRACKET1, HL_BRACKET2, HL_BRACKET3   /* brackets, coloured by nesting depth */
} HlKind;

/* Language from the file name: .py, .sh, .c/.h/.cpp/.ino. Anything else: none. */
HlLang hl_language_for(const char *path);

/* Colour one line of `len` characters into kinds[] (one HlKind each).
 * `state` = what was still open at the end of the previous line (a
 * multi-line string or comment, and the bracket depth): 0 for the first
 * line, then pass the returned value to the next line. */
int hl_line(HlLang lang, const char *s, int len, int state, unsigned char *kinds);

#endif

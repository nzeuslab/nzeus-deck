#ifndef NZ_USBKBD_H
#define NZ_USBKBD_H

#include <stdbool.h>

/* usbkbd.c - USB keyboards plugged into the console's OTG port.
 *
 * Linux already turns every USB keyboard into a /dev/input/eventN file that
 * reports key presses; we read those directly, no helper program needed. Any
 * number of keyboards may be plugged in (keys from all of them count), and at
 * any time: usbkbd_next() looks for new ones every 1.5 s and forgets
 * unplugged ones. The letters follow the US layout. */

typedef enum {
    KB_NONE = 0,
    KB_CHAR,                       /* a printable character, in .ch */
    KB_ENTER, KB_BACKSPACE, KB_DELETE, KB_TAB, KB_BACKTAB /* Shift+Tab */, KB_ESC,
    KB_UP, KB_DOWN, KB_LEFT, KB_RIGHT, KB_HOME, KB_END, KB_PGUP, KB_PGDN,
    KB_CTRL_C,                     /* interrupt the running program */
    KB_CTRL_D,                     /* end of input for the running program */
    KB_CTRL_S,                     /* save (text editor) */
    KB_CTRL_OTHER,                 /* any other Ctrl + letter: the letter in .ch (Claude Code: Ctrl+R, Ctrl+O...) */
    /* desktop shortcuts (desktop.c uses them; in handheld mode they do nothing) */
    KB_SUPER,                      /* the Windows key, pressed and let go on its own: start menu */
    KB_SUPER_UP, KB_SUPER_DOWN,    /* Windows key + an arrow: maximize, restore / minimize, */
    KB_SUPER_LEFT, KB_SUPER_RIGHT, /* snap the window to the left / right half */
    KB_CTRL_ESC,                   /* Ctrl+Esc: start menu */
    KB_ALT_TAB,                    /* Alt+Tab: the next window (.ch = KB_SHIFTED: the one before) */
    KB_ALT_F4,                     /* Alt+F4: close the window in front */
    KB_ALT_ENTER,                  /* Alt+Enter: maximize / restore the window in front */
    KB_CTRL_ALT_T,                 /* Ctrl+Alt+T: open the Terminal */
    KB_F1                          /* F1 .. F12 = KB_F1 + 0 .. 11 (keep it last) */
} KbKey;
#define KB_F(n) ((KbKey)(KB_F1 + (n) - 1))

typedef struct { KbKey key; char ch; } KbEvent;
/* .ch of a special key (an arrow, Home, End, Ctrl+C...) is KB_SHIFTED when
 * Shift was held (the Terminal selects text with Shift + arrows), else 0. */
#define KB_SHIFTED 'S'

void usbkbd_init(void);     /* opens the keyboards plugged in right now */
void usbkbd_close(void);

/* The next key press, oldest first; false when there is none. Holding a key
 * repeats it (except Enter, Esc and Ctrl combinations). Call it every frame,
 * keyboard or not: that is when newly plugged keyboards are found. */
bool usbkbd_next(KbEvent *ev);

bool usbkbd_connected(void);   /* at least one keyboard is open */

#ifdef NZ_TESTING
/* PC tests: pretend the kernel reported this key (KEY_* code; value 1 = down,
 * 2 = held, 0 = let go), then read the result with usbkbd_next() */
void usbkbd_test_event(int code, int value);
#endif

#endif

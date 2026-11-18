/* usbkbd.c - USB keyboards (see usbkbd.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#include "usbkbd.h"

#define MAX_KEYBOARDS   8      /* /dev/input/event0 .. event7 are looked at */
#define QUEUE_SIZE     32      /* key presses waiting to be picked up */
#define LOOK_AGAIN_MS 1500     /* how often to look for a newly plugged keyboard */

/* ---------- which character each key types (US layout) ----------
 * Linux numbers the keys row by row, so each row of the keyboard is a string
 * that starts at the number of its first key. */
typedef struct { int first_key; const char *plain, *shifted; } KeyRow;
static const KeyRow rows[] = {
    { KEY_1,         "1234567890-=",  "!@#$%^&*()_+" },
    { KEY_Q,         "qwertyuiop[]",  "QWERTYUIOP{}" },
    { KEY_A,         "asdfghjkl;'`",  "ASDFGHJKL:\"~" },
    { KEY_BACKSLASH, "\\zxcvbnm,./",  "|ZXCVBNM<>?" },
    { KEY_KPASTERISK, "*",            "*" },
    { KEY_SPACE,     " ",             " " },
};

/* the character a key types, or 0 if it types none */
static char key_char(int code, bool shift)
{
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        int pos = code - rows[r].first_key;
        if (pos >= 0 && pos < (int)strlen(rows[r].plain))
            return shift ? rows[r].shifted[pos] : rows[r].plain[pos];
    }
    return 0;
}

/* keys that do something instead of typing */
static KbKey key_action(int code)
{
    if (code >= KEY_F1 && code <= KEY_F10) return KB_F(code - KEY_F1 + 1);
    switch (code) {
    case KEY_F11:       return KB_F(11);
    case KEY_F12:       return KB_F(12);
    case KEY_ENTER:
    case KEY_KPENTER:   return KB_ENTER;
    case KEY_BACKSPACE: return KB_BACKSPACE;
    case KEY_DELETE:    return KB_DELETE;
    case KEY_TAB:       return KB_TAB;
    case KEY_ESC:       return KB_ESC;
    case KEY_UP:        return KB_UP;
    case KEY_DOWN:      return KB_DOWN;
    case KEY_LEFT:      return KB_LEFT;
    case KEY_RIGHT:     return KB_RIGHT;
    case KEY_HOME:      return KB_HOME;
    case KEY_END:       return KB_END;
    case KEY_PAGEUP:    return KB_PGUP;
    case KEY_PAGEDOWN:  return KB_PGDN;
    }
    return KB_NONE;
}

/* ---------- the waiting key presses (a ring: oldest at `first`) ---------- */
static KbEvent queue[QUEUE_SIZE];
static int first, waiting;

static void queue_add(KbKey key, char ch)
{
    if (waiting == QUEUE_SIZE - 1) return;   /* full: this press is lost */
    KbEvent *e = &queue[(first + waiting) % QUEUE_SIZE];
    e->key = key;
    e->ch = ch;
    waiting++;
}

/* ---------- turning kernel key events into key presses ---------- */
static bool shift, ctrl, alt;   /* held right now (left or right one) */
static bool super;              /* the Windows key is held */
static bool super_used;         /* ... and another key was pressed with it */

/* value: 1 = pressed, 2 = held down (auto-repeat), 0 = let go */
static void key_event(int code, int value)
{
    bool down = value != 0;
    switch (code) {                                       /* the modifier keys only change state */
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift = down; return;
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  ctrl = down;  return;
    case KEY_LEFTALT:   case KEY_RIGHTALT:   alt = down;   return;
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:               /* the Windows key: */
        if (value == 1) { super = true; super_used = false; }
        else if (value == 0) {                            /* let go on its own = start menu */
            if (super && !super_used) queue_add(KB_SUPER, 0);
            super = false;
        }
        return;
    }
    if (!down) return;                                    /* letting go of a key types nothing */
    bool repeat = value == 2;

    if (super) {                                          /* Windows key + a key: window shortcuts */
        super_used = true;
        if (value != 1) return;
        if (code == KEY_UP) queue_add(KB_SUPER_UP, 0);
        else if (code == KEY_DOWN) queue_add(KB_SUPER_DOWN, 0);
        else if (code == KEY_LEFT) queue_add(KB_SUPER_LEFT, 0);
        else if (code == KEY_RIGHT) queue_add(KB_SUPER_RIGHT, 0);
        return;                                           /* (other Windows key combinations: nothing) */
    }

    if (ctrl) {                                           /* Ctrl shortcuts (only on the first press) */
        if (value != 1) return;
        if (alt && code == KEY_T) queue_add(KB_CTRL_ALT_T, 0);             /* Ctrl+Alt+T = Terminal */
        else if (code == KEY_ESC) queue_add(KB_CTRL_ESC, 0);               /* Ctrl+Esc = start menu */
        else if (code == KEY_C) queue_add(KB_CTRL_C, shift ? KB_SHIFTED : 0);   /* Ctrl+Shift+C = copy */
        else if (code == KEY_D) queue_add(KB_CTRL_D, 0);
        else if (code == KEY_S) queue_add(KB_CTRL_S, 0);
        else {
            char letter = key_char(code, false);
            if (letter >= 'a' && letter <= 'z') queue_add(KB_CTRL_OTHER, letter);
        }
        return;
    }
    if (alt) {                                            /* Alt shortcuts (desktop windows) */
        if (value != 1) return;
        if (code == KEY_TAB) queue_add(KB_ALT_TAB, shift ? KB_SHIFTED : 0);   /* Shift: go back */
        else if (code == KEY_F4) queue_add(KB_ALT_F4, 0);
        else if (code == KEY_ENTER || code == KEY_KPENTER) queue_add(KB_ALT_ENTER, 0);
        return;                                           /* (other Alt combinations type nothing) */
    }

    KbKey action = key_action(code);
    if (action == KB_TAB && shift) action = KB_BACKTAB;
    if (action != KB_NONE) {
        if (repeat && (action == KB_ENTER || action == KB_ESC)) return;   /* holding these must not fire them again */
        queue_add(action, shift ? KB_SHIFTED : 0);       /* (Shift + arrows selects text) */
        return;
    }
    char c = key_char(code, shift);
    if (c) queue_add(KB_CHAR, c);
}

/* ---------- the keyboards ---------- */
typedef struct { int fd; int number; } Keyboard;   /* number = N of /dev/input/eventN */
static Keyboard kbd[MAX_KEYBOARDS];
static int kbd_count;
static long long looked_at_ms;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* A device is taken for a keyboard if it has both a Q key and a space bar
 * (a mouse, the console's own buttons or a remote has not). */
static bool has_key(const unsigned char *bits, int key) { return bits[key / 8] & (1 << (key % 8)); }

static bool is_keyboard(int fd)
{
    unsigned char keys[KEY_MAX / 8 + 1] = { 0 };
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return false;
    return has_key(keys, KEY_Q) && has_key(keys, KEY_SPACE);
}

static bool already_open(int number)
{
    for (int i = 0; i < kbd_count; i++) if (kbd[i].number == number) return true;
    return false;
}

/* Open every keyboard that isn't open yet (the kernel makes the eventN file
 * by itself the moment a keyboard is plugged in). */
static void look_for_keyboards(void)
{
    for (int n = 0; n < MAX_KEYBOARDS && kbd_count < MAX_KEYBOARDS; n++) {
        if (already_open(n)) continue;
        char name[32];
        snprintf(name, sizeof name, "/dev/input/event%d", n);
        int fd = open(name, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!is_keyboard(fd)) { close(fd); continue; }
        kbd[kbd_count].fd = fd;
        kbd[kbd_count].number = n;
        kbd_count++;
    }
    looked_at_ms = now_ms();
}

/* That keyboard was unplugged: forget it (the last one moves into its place). */
static void forget(int i)
{
    close(kbd[i].fd);
    kbd[i] = kbd[--kbd_count];
}

/* Collect what every keyboard reported since the last frame. */
static void read_keyboards(void)
{
    for (int i = 0; i < kbd_count; i++) {
        struct input_event ev;
        ssize_t got;
        while ((got = read(kbd[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev)
            if (ev.type == EV_KEY) key_event(ev.code, ev.value);
        /* "nothing yet" is EAGAIN; any other error means the keyboard is gone */
        if (got < 0 && errno != EAGAIN && errno != EINTR) forget(i--);
    }
}

void usbkbd_init(void)
{
    kbd_count = 0;
    look_for_keyboards();
}

void usbkbd_close(void)
{
    while (kbd_count) forget(kbd_count - 1);
}

bool usbkbd_connected(void) { return kbd_count > 0; }

#ifdef NZ_TESTING
void usbkbd_test_event(int code, int value) { key_event(code, value); }
#endif

bool usbkbd_next(KbEvent *ev)
{
    if (!waiting) {
        if (now_ms() - looked_at_ms >= LOOK_AGAIN_MS) look_for_keyboards();
        read_keyboards();
    }
    if (!waiting) {
        ev->key = KB_NONE;
        ev->ch = 0;
        return false;
    }
    *ev = queue[first];
    first = (first + 1) % QUEUE_SIZE;
    waiting--;
    return true;
}

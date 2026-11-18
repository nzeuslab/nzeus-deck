/* usbkbd_compare.c - plays the same made-up keyboard session into a usbkbd.c
 * and prints everything it reports. Built twice (old and new usbkbd.c) by
 * test_usbkbd_compare.sh: the two printouts must be identical.
 *
 * usbkbd.c is compiled with open/read/close/ioctl/clock_gettime renamed to the
 * t_ versions below, so it talks to 8 fake /dev/input/eventN devices and a
 * fake clock instead of real hardware. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "../usbkbd.h"

#define DEVICES 8
#define QMAX    512

typedef struct {
    int present, keyboard;            /* plugged in? a keyboard (or a mouse)? */
    struct input_event q[QMAX];       /* what it will report */
    int head, tail;
} Dev;
static Dev dev[DEVICES];
static long long fake_ms = 100000;

int t_clock_gettime(clockid_t c, struct timespec *t)
{
    (void)c;
    t->tv_sec = fake_ms / 1000;
    t->tv_nsec = (fake_ms % 1000) * 1000000;
    return 0;
}

int t_open(const char *path, int flags, ...)
{
    int n;
    (void)flags;
    if (sscanf(path, "/dev/input/event%d", &n) != 1 || n < 0 || n >= DEVICES || !dev[n].present) { errno = ENOENT; return -1; }
    return 100 + n;
}

int t_close(int fd) { (void)fd; return 0; }

int t_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    unsigned char *bits = va_arg(ap, unsigned char *);
    va_end(ap);
    memset(bits, 0, _IOC_SIZE(req));
    Dev *d = &dev[fd - 100];
    if (!d->present) { errno = ENODEV; return -1; }
    if (d->keyboard) { bits[KEY_Q / 8] |= 1 << (KEY_Q % 8); bits[KEY_SPACE / 8] |= 1 << (KEY_SPACE % 8); }
    else bits[BTN_LEFT / 8] |= 1 << (BTN_LEFT % 8);
    return 0;
}

ssize_t t_read(int fd, void *buf, size_t n)
{
    Dev *d = &dev[fd - 100];
    if (!d->present) { errno = ENODEV; return -1; }       /* unplugged */
    if (d->head == d->tail) { errno = EAGAIN; return -1; }   /* nothing yet */
    if (n < sizeof(struct input_event)) { errno = EINVAL; return -1; }
    memcpy(buf, &d->q[d->tail], sizeof(struct input_event));
    d->tail = (d->tail + 1) % QMAX;
    return sizeof(struct input_event);
}

static unsigned rng = 12345;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static void push(int n, int type, int code, int value)
{
    Dev *d = &dev[n];
    int next = (d->head + 1) % QMAX;
    if (next == d->tail) return;
    d->q[d->head].type = (unsigned short)type;
    d->q[d->head].code = (unsigned short)code;
    d->q[d->head].value = value;
    d->head = next;
}

/* a key code with a bias towards the interesting ones */
static int some_key(void)
{
    static const int often[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT,
        KEY_ENTER, KEY_KPENTER, KEY_ESC, KEY_TAB, KEY_BACKSPACE, KEY_DELETE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_F1, KEY_F5, KEY_F10, KEY_F11, KEY_F12, KEY_C, KEY_D, KEY_S,
        KEY_KPASTERISK, KEY_SPACE, KEY_GRAVE, KEY_BACKSLASH, KEY_102ND, KEY_KP7, KEY_CAPSLOCK };
    switch (rnd(3)) {
    case 0:  return often[rnd(sizeof often / sizeof often[0])];
    case 1:  return (int)rnd(128);
    default: return (int)rnd(KEY_MAX + 1);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) rng = (unsigned)atoi(argv[1]);
    dev[0].present = 1;                       /* a mouse */
    dev[2].present = dev[2].keyboard = 1;     /* a keyboard */
    usbkbd_init();
    printf("start connected=%d\n", usbkbd_connected());
    for (int frame = 0; frame < 3000; frame++) {
        switch (rnd(40)) {                    /* now and then: plug / unplug / restart */
        case 0: { int n = (int)rnd(DEVICES); dev[n].present = !dev[n].present; dev[n].keyboard = (int)rnd(3) != 0;
                  dev[n].head = dev[n].tail = 0; printf("frame %d: device %d %s\n", frame, n, dev[n].present ? "plugged" : "unplugged"); break; }
        case 1: usbkbd_close(); usbkbd_init(); printf("frame %d: restart\n", frame); break;
        }
        int burst = rnd(10) == 0 ? (int)rnd(60) : (int)rnd(6);   /* sometimes more than the queue holds */
        for (int i = 0; i < burst; i++) {
            int n = (int)rnd(DEVICES);
            if (!dev[n].present) continue;
            int r = (int)rnd(10);
            if (r == 0) push(n, EV_SYN, 0, 0);
            else if (r == 1) push(n, EV_MSC, 4, (int)rnd(100000));
            else push(n, EV_KEY, some_key(), (int)rnd(10) < 4 ? 1 : (int)rnd(10) < 6 ? 0 : (int)rnd(10) < 9 ? 2 : (int)rnd(5));
        }
        fake_ms += rnd(4) == 0 ? (long long)rnd(2000) : (long long)rnd(40);
        KbEvent e;
        while (usbkbd_next(&e)) printf("frame %d: key %d ch %d\n", frame, (int)e.key, (int)(unsigned char)e.ch);
        if (e.key != KB_NONE || e.ch != 0) printf("frame %d: the empty answer is not cleared\n", frame);
        if (frame % 50 == 0) printf("frame %d: connected=%d\n", frame, usbkbd_connected());
    }
    usbkbd_close();
    printf("end connected=%d\n", usbkbd_connected());
    return 0;
}

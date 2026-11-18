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

#include "usbmouse.h"

#define MOUSE_MAX_NODES 8
#define MOUSE_RESCAN_MS 1500   /* how often to look for newly plugged mice */

static int fds[MOUSE_MAX_NODES];
static int fd_node[MOUSE_MAX_NODES];   /* N of /dev/input/eventN for each fd */
static int fd_count;
static long long last_scan_ms;
static int screen_w = 640, screen_h = 480;
static int pos_x, pos_y;
static bool left_down, right_down;

static long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool has_bit(const unsigned char *bits, int bit) { return bits[bit / 8] & (1u << (bit % 8)); }

/* Is this evdev node a mouse? It must report relative X/Y movement and have
 * a left button. (Some keyboard receivers are both: then both modules open
 * the node, which evdev allows - each reader gets every event.) */
static bool node_is_mouse(int fd) {
    unsigned char rel[REL_MAX / 8 + 1], key[KEY_MAX / 8 + 1];
    memset(rel, 0, sizeof rel);
    memset(key, 0, sizeof key);
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel) < 0) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0) return false;
    return has_bit(rel, REL_X) && has_bit(rel, REL_Y) && has_bit(key, BTN_LEFT);
}

static bool node_already_open(int node) {
    for (int i = 0; i < fd_count; i++) if (fd_node[i] == node) return true;
    return false;
}

static void scan_nodes(void) {
    char path[32];
    for (int i = 0; i < MOUSE_MAX_NODES && fd_count < MOUSE_MAX_NODES; i++) {
        if (node_already_open(i)) continue;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (node_is_mouse(fd)) { fd_node[fd_count] = i; fds[fd_count++] = fd; }
        else close(fd);
    }
    last_scan_ms = mono_ms();
}

/* A mouse was unplugged: forget it (the last slot fills the gap). */
static void drop_fd(int i) {
    close(fds[i]);
    fd_count--;
    fds[i] = fds[fd_count];
    fd_node[i] = fd_node[fd_count];
}

void usbmouse_init(int w, int h) {
    screen_w = w > 0 ? w : 640;
    screen_h = h > 0 ? h : 480;
    pos_x = screen_w / 2;          /* start in the middle of the screen */
    pos_y = screen_h / 2;
    left_down = right_down = false;
    fd_count = 0;
    scan_nodes();
}

void usbmouse_close(void) {
    for (int i = 0; i < fd_count; i++) close(fds[i]);
    fd_count = 0;
}

bool usbmouse_poll(UsbMouse *m) {
    bool changed = false;
    int before = fd_count;
    memset(m, 0, sizeof *m);
    /* Hot-plug: look for mice plugged in after the program started. */
    if (mono_ms() - last_scan_ms >= MOUSE_RESCAN_MS) scan_nodes();
    struct input_event ev;
    for (int i = 0; i < fd_count; i++) {
        ssize_t got;
        while ((got = read(fds[i], &ev, sizeof ev)) == (ssize_t)sizeof ev) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X)          { pos_x += ev.value; m->moved = true; }
                else if (ev.code == REL_Y)     { pos_y += ev.value; m->moved = true; }
                else if (ev.code == REL_WHEEL) { m->wheel += ev.value; }
                changed = true;
            } else if (ev.type == EV_KEY) {
                /* value: 1 = pressed, 0 = released (2 = autorepeat, unused) */
                if (ev.code == BTN_LEFT) {
                    if (ev.value == 1 && !left_down) m->left_click = true;
                    left_down = ev.value != 0;
                    changed = true;
                } else if (ev.code == BTN_RIGHT) {
                    if (ev.value == 1 && !right_down) m->right_click = true;
                    right_down = ev.value != 0;
                    changed = true;
                }
            }
        }
        /* ENODEV/EIO instead of EAGAIN: the mouse was unplugged. */
        if (got < 0 && errno != EAGAIN && errno != EINTR) { drop_fd(i); i--; }
    }
    /* keep the pointer on the screen */
    if (pos_x < 0) pos_x = 0;
    if (pos_y < 0) pos_y = 0;
    if (pos_x >= screen_w) pos_x = screen_w - 1;
    if (pos_y >= screen_h) pos_y = screen_h - 1;
    m->x = pos_x;
    m->y = pos_y;
    m->left_down = left_down;
    m->connected = fd_count > 0;
    if (fd_count != before) changed = true;   /* plugged in or out */
    return changed;
}

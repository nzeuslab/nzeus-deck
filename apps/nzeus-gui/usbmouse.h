#ifndef NZEUS_USBMOUSE_H
#define NZEUS_USBMOUSE_H
/* (First written for our FrogShell add-ons; NZEUS adds left_down for dragging.) */

#include <stdbool.h>

/* Physical USB mouse support via OTG (/dev/input/event*, raw evdev), the
 * same way usbkbd.c handles keyboards. One pointer position is kept for the
 * whole screen; any connected mouse moves it. Mice can be plugged in or out
 * at any time: usbmouse_poll rescans /dev/input every 1.5 s. */

typedef struct {
    int  x, y;          /* pointer position in screen pixels */
    int  wheel;         /* wheel steps since the last poll: + = up, - = down */
    bool moved;         /* the pointer moved since the last poll */
    bool left_click;    /* left button went down since the last poll */
    bool right_click;   /* right button went down since the last poll */
    bool left_down;     /* left button is being held right now (for dragging) */
    bool connected;     /* at least one mouse is plugged in */
} UsbMouse;

void usbmouse_init(int screen_w, int screen_h);
void usbmouse_close(void);

/* Read all pending mouse events into *m. Returns true when anything changed
 * (movement, click, wheel, plug/unplug). Call it every frame. */
bool usbmouse_poll(UsbMouse *m);

#endif

#ifndef NZEUS_USBMODE_H
#define NZEUS_USBMODE_H

/* The USB-C port's mode (Settings > USB). The work is done by
 * nzeus/bin/nzeus-usbmode.sh (linux-port/deck-modern), run in the background:
 *   "Devices"        the console is the host (keyboard, mouse, Wi-Fi / Bluetooth dongle)
 *   "PC connection"  the console is a USB device for a PC: a network card (SSH) + a serial port
 * Call usbmode_poll() often (every 250 ms) while the page is open. */
enum { USBM_CHECKING, USBM_MISSING, USBM_HOST, USBM_PC, USBM_SWITCHING };

void usbmode_poll(void);              /* collects answers; asks "status" every 3 s */
void usbmode_refresh(void);           /* ask "status" now */
int  usbmode_state(void);             /* USBM_* */
int  usbmode_switch(int to_pc);       /* starts the switch in the background; 0 = started */
const char *usbmode_line(void);       /* what is going on, in plain words */
const char *usbmode_note(void);       /* the last message of a switch (or its error), "" = none */

#endif

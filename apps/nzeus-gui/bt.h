#ifndef NZEUS_BT_H
#define NZEUS_BT_H

/* Bluetooth keyboards and mice (the Settings app's Bluetooth section).
 *
 * The real work is done by two programs on the SD card (core/deck-modern/bt/README.md):
 *   nzeus/bin/nzeus-bt-up.sh   switches Bluetooth on (add-ons, firmware, service)
 *   nzeus/bin/nzeus-bt         status / list / scan / pair / connect / ...
 * Everything here runs them as background programs (bgjob.c), so the screen
 * never freezes. Call bt_poll() often (every 250 ms) while the page is open:
 * it collects their answers. */

enum { BT_CHECKING,     /* no answer yet */
       BT_MISSING,      /* the Bluetooth add-on is not on the SD card */
       BT_OFF,
       BT_NO_DONGLE,    /* switched on, but no dongle plugged in */
       BT_STARTING,     /* nzeus-bt-up.sh is running */
       BT_STOPPING,     /* nzeus-bt off is running */
       BT_ON };

typedef struct {
    char addr[18];        /* "AA:BB:CC:DD:EE:FF" */
    char name[40];        /* the device's name, or its address if it has none */
    char kind[16];        /* keyboard, mouse, keyboard+mouse, phone, ... */
    char type[8];         /* classic, le, dual */
    int  rssi;            /* signal in dBm (-40 strong ... -90 weak; 0 = unknown) */
    int  supported;       /* scan: 1 = a keyboard/mouse NZEUS can use */
    int  connected;       /* my devices: 1 = connected now */
} BtDev;

/* what bt_pair_next() found in nzeus-bt pair's output */
enum { BT_EV_NONE,        /* nothing new */
       BT_EV_PASSKEY,     /* text = the digits to type on the keyboard */
       BT_EV_TYPED,       /* text = how many digits the keyboard has typed */
       BT_EV_ENTER,       /* the DEVICE shows a number: ask for it, then bt_pair_answer() */
       BT_EV_INFO,        /* text = something to show small */
       BT_EV_PAIRED,
       BT_EV_DONE,        /* connected (or connecting, for LE devices): finished */
       BT_EV_ERROR };     /* text = what went wrong, in plain words */

void bt_poll(void);                     /* collect finished background jobs; ask status every 3 s */
void bt_refresh(void);                  /* ask status + list again now */
int  bt_state(void);                    /* BT_* */
const char *bt_problem(void);           /* the last problem in plain words, or "" */
const char *bt_start_line(void);        /* the last line nzeus-bt-up.sh printed */

int  bt_turn_on(void);                  /* runs nzeus-bt-up.sh in the background; 0 = started */
int  bt_turn_off(void);                 /* runs nzeus-bt off in the background */

int  bt_device_count(void);             /* my (paired) devices */
const BtDev *bt_device(int i);
int  bt_list_version(void);             /* changes whenever the list changes */

int  bt_scan_start(int seconds);        /* 0 = started (or already searching) */
int  bt_scanning(void);
int  bt_scan_elapsed_ms(void);
int  bt_found_count(void);              /* devices found by the last search (paired ones left out) */
const BtDev *bt_found(int i);
int  bt_scan_version(void);             /* changes when a search finishes */

/* connect / disconnect / remove (= forget): one at a time, in the background */
int  bt_command(const char *verb, const char *addr);   /* 0 = started */
int  bt_command_busy(void);
int  bt_command_result(char *msg, int n);   /* 1 once when it finished: msg = what to show */

int  bt_pair(const char *addr);         /* starts nzeus-bt pair ADDR; 0 = started */
int  bt_pair_next(char *text, int n);   /* the next BT_EV_* (call until BT_EV_NONE) */
int  bt_pair_answer(long number);       /* the number for BT_EV_ENTER -> the program's input */
void bt_pair_cancel(void);              /* stops the pairing program */
int  bt_pairing(void);

/* for tests: read one line of `nzeus-bt scan` or `nzeus-bt list` */
int  bt_parse_line(const char *line, BtDev *d, int from_scan);
int  bt_valid_addr(const char *s);

#endif

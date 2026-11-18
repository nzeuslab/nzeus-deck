#ifndef NZEUS_SERIAL_H
#define NZEUS_SERIAL_H

/* USB-serial ports (Arduino, STM32 boards, the car's OBD adapter...).
 *
 * The kernel has the adapter drivers built in (cdc-acm, ch341, cp210x,
 * ftdi_sio, pl2303), and Linux makes the port the moment an adapter is
 * plugged in (Settings > USB on Devices): /dev/ttyUSB0, /dev/ttyACM0... */

#define SERIAL_MAX_PORTS 8

int  serial_list(char names[][48], int max);    /* the ports there now */
int  serial_open(const char *dev, int baud);     /* fd, or -1. Raw 8N1, never blocks */
int  serial_set_baud(int fd, int baud);
int  serial_read(int fd, char *buf, int n);      /* what has arrived (0 = nothing yet) */
int  serial_write(int fd, const char *s, int n);
void serial_close(int fd);

extern const char *serial_extra_port;            /* tests: a fake port to list too (a pty) */
extern const int serial_bauds[];                 /* 9600 ... 115200 */
extern const int serial_baud_count;

#endif

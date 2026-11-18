/* serial.c - USB-serial ports (see serial.h). */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "serial.h"
#include "settings.h"

const char *serial_extra_port;
const int serial_bauds[] = { 9600, 19200, 38400, 57600, 115200 };
const int serial_baud_count = 5;

#ifdef NZ_TESTING
/* Keep the device present while simulating one failed read. */
static int fail_next_read;
void serial_test_fail_next_read(void) { fail_next_read = 1; }
#endif

int serial_list(char names[][48], int max)
{
    int n = 0;
    static const char *const pats[] = { "/dev/ttyUSB*", "/dev/ttyACM*" };
    for (int p = 0; p < 2; p++) {
        glob_t g;
        if (glob(pats[p], 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc && n < max; i++) snprintf(names[n++], 48, "%s", g.gl_pathv[i]);
        globfree(&g);
    }
    if (serial_extra_port && n < max) snprintf(names[n++], 48, "%s", serial_extra_port);
    return n;
}

static speed_t speed_of(int baud)
{
    switch (baud) {
    case 9600: return B9600;     case 19200: return B19200;   case 38400: return B38400;
    case 57600: return B57600;   case 115200: return B115200; default: return B9600;
    }
}

int serial_set_baud(int fd, int baud)
{
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return -1;
    cfsetispeed(&t, speed_of(baud));
    cfsetospeed(&t, speed_of(baud));
    return tcsetattr(fd, TCSANOW, &t);
}

int serial_open(const char *dev, int baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) != 0) { close(fd); return -1; }
    cfmakeraw(&t);                         /* bytes in, bytes out: no echo, no line editing */
    t.c_cflag |= CLOCAL | CREAD;           /* ignore modem lines, allow reading */
    t.c_cflag &= ~(CSTOPB | PARENB);       /* 8 data bits, no parity, 1 stop bit */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, speed_of(baud));
    cfsetospeed(&t, speed_of(baud));
    if (tcsetattr(fd, TCSANOW, &t) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int serial_read(int fd, char *buf, int n)
{
    if (fd < 0) return -1;
#ifdef NZ_TESTING
    if (fail_next_read) { fail_next_read = 0; errno = EIO; return -1; }
#endif
    int r = (int)read(fd, buf, (size_t)n);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;   /* -1: unplugged */
    return r;
}

int serial_write(int fd, const char *s, int n)
{
    if (fd < 0) return -1;
    return (int)write(fd, s, (size_t)n);
}

void serial_close(int fd) { if (fd >= 0) close(fd); }

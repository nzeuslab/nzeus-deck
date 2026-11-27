/* SPDX-License-Identifier: GPL-2.0 */
/*
 * box-demo: first interactive program on modern Linux. A box on the native
 * framebuffer that the D-pad (or left stick) moves. A/B/X/Y change its colour,
 * L1/R1 make it smaller/bigger, Start ends the demo early. A big MM:SS clock
 * and a blinking dot show it is alive, so it doubles as a long display test
 * with ONE owner of /dev/fb0 (photos show the elapsed time).
 *
 * Same framebuffer checks and exit restore as fb-pattern. Input devices are
 * only read. Waits in poll() between events and clock ticks (no busy loop).
 * Logs ALIVE every minute and every key press to stdout.
 * Usage: box-demo SECONDS   (1..3600)
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W 640
#define H 480
#define BACKGROUND 0xff202830u
#define BORDER 0xffffffffu
#define CLOCK_COLOR 0xffe0e0e0u

static uint32_t *fb;
static volatile sig_atomic_t stop;
static void interrupted(int sig) { (void)sig; stop = 1; }

static void fill(int x, int y, int w, int h, uint32_t color)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < H; ++j)
		for (int i = x < 0 ? 0 : x; i < x + w && i < W; ++i)
			fb[j * W + i] = color;
}

/* 3x5 digits, bit 2 = left column, rows top to bottom. */
static const uint8_t digit[11][5] = {
	{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
	{5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
	{7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {0, 2, 0, 2, 0} /* ':' */,
};

static void draw_clock(unsigned seconds)
{
	const int scale = 8, top = 24, left = W / 2 - (5 * 4 * scale) / 2;
	int glyphs[5] = {(seconds / 600) % 10, (seconds / 60) % 10, 10,
			 (seconds % 60) / 10, seconds % 10};
	fill(left, top, 5 * 4 * scale, 5 * scale, BACKGROUND);
	for (int g = 0; g < 5; ++g)
		for (int row = 0; row < 5; ++row)
			for (int col = 0; col < 3; ++col)
				if (digit[glyphs[g]][row] >> (2 - col) & 1)
					fill(left + g * 4 * scale + col * scale, top + row * scale,
					     scale, scale, CLOCK_COLOR);
}

int main(int argc, char **argv)
{
	static const uint32_t colors[] = {0xff00c000u, 0xffe00000u, 0xff2060ffu, 0xffffd000u};
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	struct pollfd fds[8];
	struct timespec start, now;
	int count = 0, x = W / 2 - 30, y = H / 2 - 30, size = 60, color = 0;
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 0, shown = ~0u, presses = 0;
	int fd, ret;

	if (seconds < 1 || seconds > 3600) {
		fputs("Usage: box-demo SECONDS (1..3600)\n", stderr);
		return 1;
	}
	for (int i = 0; i < 8; ++i) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int in = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (in < 0)
			continue;
		fds[count].fd = in;
		fds[count].events = POLLIN;
		++count;
	}
	if (!count) { fputs("No input devices\n", stderr); return 1; }
	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("open framebuffer"); return 1; }
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
		perror("framebuffer info"); return 1;
	}
	if (strncmp(fix.id, "hc16xx-gma-argb", sizeof(fix.id)) ||
	    var.xres != W || var.yres != H || var.bits_per_pixel != 32 ||
	    var.red.offset != 16 || var.green.offset != 8 || var.blue.offset != 0 ||
	    var.transp.offset != 24 || fix.line_length != W * 4 || fix.smem_len != W * H * 4) {
		fputs("Refusing unexpected framebuffer\n", stderr); return 1;
	}
	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { perror("mmap framebuffer"); return 1; }
	signal(SIGINT, interrupted); signal(SIGTERM, interrupted); signal(SIGHUP, interrupted);

	fill(0, 0, W, H, BORDER);
	fill(4, 4, W - 8, H - 8, BACKGROUND);
	fill(x, y, size, size, colors[color]);
	if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK)) { perror("activate GMA"); return 1; }
	printf("BOX_DEMO_ACTIVE %u s, %d input devices\n", seconds, count);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &start);

	while (!stop) {
		long ms;
		unsigned elapsed;
		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
		elapsed = (unsigned)(ms / 1000);
		if (elapsed >= seconds)
			break;
		if (elapsed != shown) {
			shown = elapsed;
			draw_clock(elapsed);
			fill(W - 40, 24, 16, 16, elapsed & 1 ? 0xff00ff00u : BACKGROUND);
			if (elapsed && elapsed % 60 == 0) {
				printf("ALIVE t=%u presses=%u box=%d,%d\n", elapsed, presses, x, y);
				fflush(stdout);
			}
		}
		if (poll(fds, count, (int)(1000 - ms % 1000)) <= 0)
			continue;
		for (int i = 0; i < count; ++i) {
			struct input_event ev;
			if (!(fds[i].revents & POLLIN))
				continue;
			while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				int nx = x, ny = y, nsize = size, ncolor = color;
				if (ev.type != EV_KEY || ev.value == 0)
					continue;
				switch (ev.code) {     /* value 1 = press, 2 = auto-repeat */
				case KEY_UP: ny -= 8; break;
				case KEY_DOWN: ny += 8; break;
				case KEY_LEFT: nx -= 8; break;
				case KEY_RIGHT: nx += 8; break;
				case KEY_LEFTCTRL: ncolor = 0; break;   /* A */
				case KEY_LEFTALT: ncolor = 1; break;    /* B */
				case KEY_SPACE: ncolor = 2; break;      /* X */
				case KEY_LEFTSHIFT: ncolor = 3; break;  /* Y */
				case KEY_TAB: if (size > 20) nsize -= 10; break;        /* L1 */
				case KEY_BACKSPACE: if (size < 200) nsize += 10; break; /* R1 */
				case KEY_ENTER: stop = 1; break;        /* Start */
				}
				if (ev.value == 1) {
					++presses;
					printf("KEY code=%u t=%ld.%03ld\n", ev.code, ms / 1000, ms % 1000);
				}
				/* keep the box inside the border and below the clock */
				if (nx < 8) nx = 8;
				if (ny < 72) ny = 72;
				if (nx > W - 8 - nsize) nx = W - 8 - nsize;
				if (ny > H - 8 - nsize) ny = H - 8 - nsize;
				if (nx != x || ny != y || nsize != size || ncolor != color) {
					fill(x, y, size, size, BACKGROUND);
					x = nx; y = ny; size = nsize; color = ncolor;
					fill(x, y, size, size, colors[color]);
				}
			}
		}
		fflush(stdout);
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	printf("BOX_DEMO_END t=%ld presses=%u by_start=%d\n",
	       (long)(now.tv_sec - start.tv_sec), presses, (int)stop);
	ret = ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN);
	if (ret) perror("restore firmware display");
	else puts("FIRMWARE_LAYER_RESTORE_REQUESTED");
	munmap(fb, fix.smem_len);
	close(fd);
	return ret ? 1 : 0;
}

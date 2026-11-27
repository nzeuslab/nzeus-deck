/* SPDX-License-Identifier: GPL-2.0 */
/*
 * key-events: print what the kernel's input devices report, read-only.
 * Opens every /dev/input/event0..7 O_RDONLY, prints each device's name, then
 * waits (poll, no busy loop) for SECONDS and prints every key press/release
 * with its time and key code name. Usage: key-events SECONDS (1..120)
 */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const char *key_name(unsigned code)
{
	switch (code) {
	case KEY_UP: return "UP";            case KEY_DOWN: return "DOWN";
	case KEY_LEFT: return "LEFT";        case KEY_RIGHT: return "RIGHT";
	case KEY_LEFTCTRL: return "A";       case KEY_LEFTALT: return "B";
	case KEY_SPACE: return "X";          case KEY_LEFTSHIFT: return "Y";
	case KEY_TAB: return "L1";           case KEY_BACKSPACE: return "R1";
	case KEY_PAGEUP: return "L2";        case KEY_PAGEDOWN: return "R2";
	case KEY_ESC: return "SELECT";       case KEY_ENTER: return "START";
	case KEY_VOLUMEUP: return "VOL+";    case KEY_MENU: return "FN";
	case KEY_POWER: return "POWER";
	default: return "?";
	}
}

int main(int argc, char **argv)
{
	struct pollfd fds[8];
	int count = 0, seconds = argc > 1 ? atoi(argv[1]) : 0;
	unsigned presses = 0;
	struct timespec start, now;

	if (seconds < 1 || seconds > 120) {
		fputs("Usage: key-events SECONDS (1..120)\n", stderr);
		return 1;
	}
	for (int i = 0; i < 8; ++i) {
		char path[32], name[64] = "?";
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		printf("DEVICE %s \"%s\"\n", path, name);
		fds[count].fd = fd;
		fds[count].events = POLLIN;
		++count;
	}
	printf("DEVICES=%d\n", count);
	if (!count)
		return 1;
	printf("EVENTS_START %d s - press the buttons now\n", seconds);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		long left;
		clock_gettime(CLOCK_MONOTONIC, &now);
		left = seconds * 1000L - ((now.tv_sec - start.tv_sec) * 1000L +
					  (now.tv_nsec - start.tv_nsec) / 1000000L);
		if (left <= 0)
			break;
		if (poll(fds, count, (int)left) <= 0)
			continue;
		for (int i = 0; i < count; ++i) {
			struct input_event ev;
			if (!(fds[i].revents & POLLIN))
				continue;
			while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value == 2)   /* skip autorepeat */
					continue;
				clock_gettime(CLOCK_MONOTONIC, &now);
				printf("t=%7.3f event%d code=%u %s %s\n",
				       (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9,
				       i, ev.code, key_name(ev.code), ev.value ? "DOWN" : "up");
				presses += ev.value == 1;
				fflush(stdout);
			}
		}
	}
	printf("EVENTS_END presses=%u\n", presses);
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pin-drive: power-off EXPERIMENT (2026-10-10). Drives ONE of two GPIO L pins
 * low from user space, in a fully running Linux, the way the stock powergpio
 * tool drives L14 (disassembled: OUT 0x18800054 bit cleared, then DIR
 * 0x18800058 bit set), and prints what the pin reads back every 0.5 s.
 *   - L14 = the power BUTTON line. Run 1 (15 s): the pin read low the whole
 *     time and the console stayed on -> L14 only senses the button; driving
 *     it is not a "press" for the power chip (its own long press is ~7 s).
 *   - L01 = the stock device tree's "power-pin" (panel node, next to
 *     power-button = 14). Question: does driving it low switch the power off?
 *
 * Safety:
 *   - touches ONLY the chosen bit (1 or 14) of OUT and DIR (read-modify-write);
 *   - refuses unless that pin is an input reading high;
 *   - lets go (DIR bit cleared, OUT bit restored) at the end and on SIGTERM/SIGINT.
 * Usage: pin-drive SECONDS [PIN]   (1..20 s; PIN 1 or 14, default 14)
 * Build: mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32 -Wall -Wextra
 *        -o pin-drive-mipsel pin-drive.c
 */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define L_IN   (0x050 / 4)
#define L_OUT  (0x054 / 4)
#define L_DIR  (0x058 / 4)

static uint32_t pin_mask;		/* the one bit this run may touch */
static volatile uint32_t *regs;
static uint32_t old_out;
static volatile sig_atomic_t stop;

#ifdef SIMULATE
/* PC test: a fake register page; the pin reads low while it is driven low. */
static uint32_t sim[4096 / 4];
static void sim_update(void)
{
	if ((sim[L_DIR] & pin_mask) && !(sim[L_OUT] & pin_mask))
		sim[L_IN] &= ~pin_mask;
	else
		sim[L_IN] |= pin_mask;
}
#endif

static uint32_t rd(int reg)
{
#ifdef SIMULATE
	sim_update();
#endif
	return regs[reg];
}

static void set_pin(int reg, int on)
{
	uint32_t v = regs[reg];
	regs[reg] = on ? (v | pin_mask) : (v & ~pin_mask);
#ifdef SIMULATE
	sim_update();
#endif
}

/* Let go: back to input first, then put the OUT bit back as it was. */
static void release(void)
{
	set_pin(L_DIR, 0);
	set_pin(L_OUT, (old_out & pin_mask) != 0);
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void show(const char *what, double t)
{
	uint32_t in = rd(L_IN), out = rd(L_OUT), dir = rd(L_DIR);
	printf("%s t=%5.1f pin_in=%u pin_out=%u pin_dir=%u  (IN=%08x OUT=%08x DIR=%08x)\n",
	       what, t, !!(in & pin_mask), !!(out & pin_mask), !!(dir & pin_mask),
	       (unsigned)in, (unsigned)out, (unsigned)dir);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 0;
	int pin = argc > 2 ? atoi(argv[2]) : 14;

	if (seconds < 1 || seconds > 20 || (pin != 1 && pin != 14)) {
		fprintf(stderr, "usage: pin-drive SECONDS (1..20) [PIN: 1 or 14]\n");
		return 2;
	}
	pin_mask = 1u << pin;
#ifdef SIMULATE
	regs = sim;
	sim[L_IN] = 0x001e6ffe;	/* values seen on the console on 2026-10-10 */
	sim[L_OUT] = 0x140;
	sim[L_DIR] = 0x140;
#else
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("pin-drive: open /dev/mem");
		return 2;
	}
	void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x18800000);
	close(fd);
	if (page == MAP_FAILED) {
		perror("pin-drive: mmap");
		return 2;
	}
	regs = page;	/* offsets above are from 0x18800000 */
#endif
	show("BEFORE", 0);
	if (rd(L_DIR) & pin_mask) {
		printf("REFUSED_PIN_ALREADY_OUTPUT\n");
		return 3;
	}
	if (!(rd(L_IN) & pin_mask)) {
		printf("REFUSED_PIN_READS_LOW\n");
		return 3;
	}
	old_out = rd(L_OUT);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	/* Same order as the stock powergpio tool: output value low, then output. */
	printf("PIN_DRIVE_LOW L%02d for %d s (the console may switch off during this)\n", pin, seconds);
	fflush(stdout);
	double t0 = now();
	set_pin(L_OUT, 0);
	set_pin(L_DIR, 1);
	while (!stop && now() - t0 < seconds) {
		show("HOLD", now() - t0);
		usleep(500000);
	}
	release();
	show(stop ? "RELEASED_EARLY" : "RELEASED", now() - t0);
	printf("PIN_DRIVE_END L%02d still_on=1 held=%.1f s\n", pin, now() - t0);
	return 0;
}

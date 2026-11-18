/* SPDX-License-Identifier: GPL-2.0 */
/*
 * key-watch: READ-ONLY look at the E3100 GPIO pins while buttons are pressed.
 *
 * Maps the one 4 KiB register page at 0x18800000 from /dev/mem with PROT_READ
 * only (opened O_RDONLY), so it cannot change anything. Reads the four GPIO
 * banks (L 0x044, B 0x0c4, R 0x0e4, T 0x344: INT_EN, EDGE_RISE, EDGE_FALL, IN,
 * OUT, DIR; the INT_ST status word is never read) and the pin-mux bytes of the
 * L bank (0x4a0 + pin). Then:
 *   1. a short burst of back-to-back reads of the four IN words, counting how
 *      often each bit flips (shows pins that something else, e.g. the AVP, is
 *      clocking fast - the button serializer uses L08/L09);
 *   2. samples every 5 ms (sleeping in between) for SECONDS and prints every
 *      change of an IN bit with its time, e.g. "t=3.215 L11 1->0".
 * Usage: key-watch SECONDS   (1..120)
 * GPIO number = bank*32 + bit (L=0, B=1, R=2, T=3).
 */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MEM_PATH
#define MEM_PATH "/dev/mem"
#endif
#ifndef REG_PAGE
#define REG_PAGE 0x18800000u
#endif
#define MAX_CHANGE_LINES 400
#define BURST_READS 200000

static const char bank_name[4] = {'L', 'B', 'R', 'T'};
static const unsigned bank_offset[4] = {0x044, 0x0c4, 0x0e4, 0x344};
static const char *const reg_name[6] = {"INT_EN", "RISE", "FALL", "IN", "OUT", "DIR"};

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	volatile const uint32_t *page;
	uint32_t last[4], cur[4];
	unsigned flips[4][32] = {{0}}, burst[4][32] = {{0}};
	unsigned lines = 0, dropped = 0;
	int seconds, fd;
	double start;

	seconds = argc > 1 ? atoi(argv[1]) : 0;
	if (seconds < 1 || seconds > 120) {
		fputs("Usage: key-watch SECONDS (1..120)\n", stderr);
		return 1;
	}
	fd = open(MEM_PATH, O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open " MEM_PATH " read-only"); return 1; }
	page = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, REG_PAGE);
	if (page == MAP_FAILED) { perror("read-only mmap"); return 1; }

	for (int b = 0; b < 4; ++b) {
		printf("BANK_%c", bank_name[b]);
		for (int r = 0; r < 6; ++r)
			printf(" %s=%08x", reg_name[r], page[(bank_offset[b] + 4 * r) / 4]);
		printf("\n");
	}
	printf("PINMUX_L00_L31=");
	for (int w = 0; w < 8; ++w)
		printf("%08x%s", page[(0x4a0 + 4 * w) / 4], w < 7 ? " " : "\n");

	/* 1. fast burst */
	for (int b = 0; b < 4; ++b)
		last[b] = page[(bank_offset[b] + 0x0c) / 4];
	start = now();
	for (int n = 0; n < BURST_READS; ++n) {
		for (int b = 0; b < 4; ++b) {
			uint32_t v = page[(bank_offset[b] + 0x0c) / 4], d = v ^ last[b];
			for (int bit = 0; d; ++bit, d >>= 1)
				if (d & 1)
					++burst[b][bit];
			last[b] = v;
		}
	}
	printf("BURST %d reads of each IN word in %.3f s; bits that flipped:", BURST_READS, now() - start);
	for (int b = 0; b < 4; ++b)
		for (int bit = 0; bit < 32; ++bit)
			if (burst[b][bit])
				printf(" %c%02d=%u", bank_name[b], bit, burst[b][bit]);
	printf("\n");

	/* 2. slow watch */
	for (int b = 0; b < 4; ++b) {
		last[b] = page[(bank_offset[b] + 0x0c) / 4];
		printf("START_IN_%c=%08x\n", bank_name[b], last[b]);
	}
	printf("WATCH_START %d s - press the buttons now\n", seconds);
	fflush(stdout);
	start = now();
	while (now() - start < seconds) {
		struct timespec pause = {0, 5 * 1000 * 1000};
		nanosleep(&pause, NULL);
		for (int b = 0; b < 4; ++b) {
			uint32_t d;
			cur[b] = page[(bank_offset[b] + 0x0c) / 4];
			d = cur[b] ^ last[b];
			for (int bit = 0; bit < 32; ++bit) {
				if (!(d >> bit & 1))
					continue;
				++flips[b][bit];
				if (lines < MAX_CHANGE_LINES) {
					printf("t=%7.3f %c%02d %u->%u\n", now() - start, bank_name[b], bit,
					       last[b] >> bit & 1, cur[b] >> bit & 1);
					++lines;
				} else {
					++dropped;
				}
			}
			last[b] = cur[b];
		}
		fflush(stdout);
	}
	printf("WATCH_END lines=%u not_printed=%u\n", lines, dropped);
	printf("CHANGES_PER_PIN:");
	for (int b = 0; b < 4; ++b)
		for (int bit = 0; bit < 32; ++bit)
			if (flips[b][bit])
				printf(" %c%02d=%u", bank_name[b], bit, flips[b][bit]);
	printf("\n");
	for (int b = 0; b < 4; ++b)
		printf("END_IN_%c=%08x\n", bank_name[b], last[b]);
	return 0;
}

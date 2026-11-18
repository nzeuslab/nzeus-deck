/* SPDX-License-Identifier: GPL-2.0 */
/*
 * key-scan: read the R36S button serializer (D-pad, A/B/X/Y, L1/R1,
 * Select/Start) from user space, to learn its bit order before any kernel
 * change. Same 2-wire protocol as Siarhei's gpio-serializer-keys driver:
 * clock = L08 (idle high), latch+data = L09 (open drain: driven low to latch,
 * released to read). A pressed key is expected to read as 0.
 *
 * This WRITES two GPIO registers (L bank OUT 0x18800054, DIR 0x18800058), but
 * only ever bits 8 and 9:
 *   - refuses unless L08 and L09 are already outputs driven high (the state
 *     the firmware leaves them in, seen by key-watch on 2026-10-10);
 *   - every write is read-modify-write of bits 8/9 only; after each scan it
 *     checks that no other OUT/DIR bit changed (e.g. L06 LCD reset, L14 power)
 *     and stops if one did;
 *   - between scans the data line is released (input), as in the driver;
 *   - restores L08/L09 to output-high at the end and on SIGTERM/SIGINT.
 * Usage: key-scan SECONDS [CLOCKS]   (1..120 s, 8..32 clocks, default 24)
 * Prints the raw bits of each scan when they change: bit i = value after
 * i clocks (bit 0 = right after the latch), '1' = high, '0' = low.
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
#define CLK    (1u << 8)
#define DAT    (1u << 9)
#define OURS   (CLK | DAT)
#define MAX_CHANGE_LINES 300

static volatile uint32_t *regs;
static uint32_t base_out, base_dir;
static volatile sig_atomic_t stop;

#ifdef SIMULATE
/* PC test: a 74HC165-like 2-wire chip in software. Keys "pressed" depend on
 * time so the change printing can be checked. */
static uint32_t sim[4096 / 4];
static uint32_t shift, pressed_mask;
static int sim_bit;
static void sim_update(void)
{
	int data_driven_low = (sim[L_DIR] & DAT) && !(sim[L_OUT] & DAT);
	static int was_low, clk_was_high = 1;
	int clk_high = (sim[L_OUT] & CLK) != 0;
	if (data_driven_low)
		was_low = 1;
	else if (was_low) {             /* latch released: load keys */
		was_low = 0;
		shift = ~pressed_mask;  /* pressed = 0 */
		sim_bit = 0;
	}
	if (clk_was_high && !clk_high)  /* falling clock: next bit */
		++sim_bit;
	clk_was_high = clk_high;
	sim[L_IN] = (sim[L_IN] & ~DAT) |
		    ((sim[L_DIR] & DAT) ? (sim[L_OUT] & DAT) :
		     ((sim_bit < 12 ? shift >> sim_bit & 1 : 1) ? DAT : 0));
}
#define REG_WRITE(i, v) do { regs[i] = (v); sim_update(); } while (0)
#else
#define REG_WRITE(i, v) (regs[i] = (v))
#endif

static void wait_us(unsigned us)
{
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	do clock_gettime(CLOCK_MONOTONIC, &b);
	while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000 < (long)us);
}

static void set_bits(int index, uint32_t mask, uint32_t value)
{
	uint32_t v = regs[index];
	REG_WRITE(index, (v & ~mask) | (value & mask));
}

static void restore(void)
{
	set_bits(L_OUT, OURS, OURS);   /* value high first, then drive: no low glitch */
	set_bits(L_DIR, OURS, OURS);
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* One 2-wire scan. Returns raw bits, bit i = level after i clocks. */
static uint32_t scan(int clocks)
{
	uint32_t bits = 0;

	set_bits(L_OUT, DAT, 0);        /* latch: output value low first, */
	set_bits(L_DIR, DAT, DAT);      /* then drive it */
	wait_us(12);
	set_bits(L_DIR, DAT, 0);        /* release data: input, pulled up */
	wait_us(3);
	bits |= (regs[L_IN] & DAT) ? 1 : 0;
	for (int i = 1; i < clocks; ++i) {
		set_bits(L_OUT, CLK, 0);
		wait_us(3);
		bits |= ((regs[L_IN] & DAT) ? 1u : 0u) << i;
		set_bits(L_OUT, CLK, CLK);
		wait_us(3);
	}
	set_bits(L_OUT, CLK, 0);        /* extra clock: back to latch-detect mode */
	wait_us(3);
	set_bits(L_OUT, CLK, CLK);
	set_bits(L_OUT, DAT, DAT);      /* between scans data stays released */
	return bits;
}

static void print_bits(uint32_t bits, int clocks)
{
	for (int i = 0; i < clocks; ++i)
		putchar(bits >> i & 1 ? '1' : '0');
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 0;
	int clocks = argc > 2 ? atoi(argv[2]) : 24;
	unsigned lines = 0, scans = 0, others = 0;
	uint32_t last, bits;
	struct timespec start, t;

	if (seconds < 1 || seconds > 120 || clocks < 8 || clocks > 32) {
		fputs("Usage: key-scan SECONDS(1..120) [CLOCKS(8..32)]\n", stderr);
		return 1;
	}
#ifdef SIMULATE
	regs = sim;
	sim[L_OUT] = 0x340; sim[L_DIR] = 0x340; sim[L_IN] = 0x00186fc6;
#else
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }
	void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x18800000);
	if (page == MAP_FAILED) { perror("mmap"); return 1; }
	regs = page;   /* offsets above are from 0x18800000 */
#endif
	base_out = regs[L_OUT];
	base_dir = regs[L_DIR];
	printf("BEFORE L_OUT=%08x L_DIR=%08x L_IN=%08x\n", base_out, base_dir, regs[L_IN]);
	if ((base_out & OURS) != OURS || (base_dir & OURS) != OURS) {
		printf("REFUSED_UNEXPECTED_IDLE_STATE (need L08/L09 output-high)\n");
		return 1;
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	last = scan(clocks);
	printf("FIRST_SCAN ");
	print_bits(last, clocks);
	printf("\nSCAN_START %d s, %d clocks, one scan every 10 ms - press the buttons now\n", seconds, clocks);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct timespec pause = {0, 10 * 1000 * 1000};
		double elapsed;
#ifdef SIMULATE
		clock_gettime(CLOCK_MONOTONIC, &t);
		elapsed = (t.tv_sec - start.tv_sec) + (t.tv_nsec - start.tv_nsec) / 1e9;
		pressed_mask = elapsed > 0.5 && elapsed < 0.8 ? 1u << 3 :
			       elapsed > 1.2 && elapsed < 1.5 ? 1u << 10 : 0;
#endif
		nanosleep(&pause, NULL);
		clock_gettime(CLOCK_MONOTONIC, &t);
		elapsed = (t.tv_sec - start.tv_sec) + (t.tv_nsec - start.tv_nsec) / 1e9;
		if (stop || elapsed >= seconds)
			break;
		bits = scan(clocks);
		++scans;
		if (((regs[L_OUT] ^ base_out) | (regs[L_DIR] ^ base_dir)) & ~OURS) {
			/* someone else (the AVP?) changed another pin: hands off */
			++others;
			printf("OTHER_BITS_CHANGED L_OUT=%08x L_DIR=%08x - STOPPING\n", regs[L_OUT], regs[L_DIR]);
			break;
		}
		if (bits != last) {
			if (lines < MAX_CHANGE_LINES) {
				printf("t=%7.3f ", elapsed);
				print_bits(bits, clocks);
				printf("  changed:");
				for (int i = 0; i < clocks; ++i)
					if ((bits ^ last) >> i & 1)
						printf(" %d%s", i, bits >> i & 1 ? "^" : "v");
				printf("\n");
				fflush(stdout);
			}
			++lines;
			last = bits;
		}
	}
	restore();
	printf("SCAN_END scans=%u change_lines=%u stopped_by_signal=%d other_bit_stop=%u\n",
	       scans, lines, (int)stop, others);
	printf("AFTER L_OUT=%08x L_DIR=%08x L_IN=%08x\n", regs[L_OUT], regs[L_DIR], regs[L_IN]);
	printf("RESTORED_OK=%d\n", ((regs[L_OUT] ^ base_out) | (regs[L_DIR] ^ base_dir)) == 0);
	return 0;
}

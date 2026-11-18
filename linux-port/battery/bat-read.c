/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bat-read: READ-ONLY look at the SAR ADC that measures the battery.
 *
 * Decoded from the factory bootloader's own "saradc" driver
 * (boot-analysis/bootloader-disasm.txt lines 42621-42968):
 *   - ADC block 0x18818400; the latest sample of channel ch is ONE BYTE at
 *     0x188184D0 + idx, idx = ch < 4 ? ch : ch + 2 (ch1 -> D1, ch5 -> D7);
 *   - the stock value is min(255, 241 * raw[ch] / raw[ch0]) because the stock
 *     tree sets adjust_channel = 0 (channel 0 = reference);
 *   - ch1 = battery level (about 128 empty .. 175 full; bootloader: <= 127 is
 *     "low"), ch5 = charger (>= 64 = plugged in);
 *   - clock gate: 0x18800060 bit 12, CLEAR = running.
 * The bootloader configures channels 0, 1 and 5 before Linux starts; reading
 * needs no start command (it reads the latest value). This tool only maps the
 * two register pages PROT_READ from /dev/mem opened O_RDONLY and reads only
 * registers the bootloader itself writes, plus the data bytes.
 * Usage: bat-read SECONDS   (1..120, one sample per second)
 */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static unsigned level(unsigned raw, unsigned ref)
{
	unsigned v;
	if (!ref)
		return 0;
	v = 241 * raw / ref;
	return v > 255 ? 255 : v;
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 0, fd;
	volatile const uint8_t *adc, *sys;
	volatile const uint32_t *adc32;
	void *p;

	if (seconds < 1 || seconds > 120) {
		fputs("Usage: bat-read SECONDS (1..120)\n", stderr);
		return 1;
	}
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open /dev/mem read-only"); return 1; }
	p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x18818000);
	if (p == MAP_FAILED) { perror("read-only mmap ADC"); return 1; }
	adc = (const uint8_t *)p + 0x400;
	adc32 = (volatile const uint32_t *)adc;
	p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x18800000);
	if (p == MAP_FAILED) { perror("read-only mmap sys"); return 1; }
	sys = p;

	printf("CLOCK_GATE0=%08x SAR_ADC_CLOCK=%s\n", *(volatile const uint32_t *)(sys + 0x60),
	       (*(volatile const uint32_t *)(sys + 0x60) >> 12 & 1) ? "GATED_OFF" : "RUNNING");
	printf("ADC_0C=%08x ADC_70=%08x ADC_74=%08x ADC_88=%08x ADC_8C=%08x\n",
	       adc32[0x0c / 4], adc32[0x70 / 4], adc32[0x74 / 4], adc32[0x88 / 4], adc32[0x8c / 4]);
	printf("ADC_90_B3=");
	for (int o = 0x90; o < 0xb4; o += 4)
		printf("%08x%s", adc32[o / 4], o < 0xb0 ? " " : "\n");
	printf("SYS_160_173=%08x %08x %08x %08x %08x\n",
	       *(volatile const uint32_t *)(sys + 0x160), *(volatile const uint32_t *)(sys + 0x164),
	       *(volatile const uint32_t *)(sys + 0x168), *(volatile const uint32_t *)(sys + 0x16c),
	       *(volatile const uint32_t *)(sys + 0x170));
	printf("DATA_D0_D7=");
	for (int i = 0; i < 8; ++i)
		printf("%02x%s", adc[0xd0 + i], i < 7 ? " " : "\n");
	printf("SAMPLES (t raw_ref raw_bat raw_chg -> battery_level charger_level)\n");
	fflush(stdout);
	for (int t = 0; t < seconds; ++t) {
		unsigned ref = adc[0xd0], bat = adc[0xd1], chg = adc[0xd7];
		unsigned lb = level(bat, ref), lc = level(chg, ref);
		int pct = lb <= 128 ? 0 : lb >= 175 ? 100 : (int)((lb - 128) * 100 / 47);
		printf("t=%3d ref=%3u bat=%3u chg=%3u -> BATTERY=%3u (about %3d%%%s) CHARGER=%3u (%s)\n",
		       t, ref, bat, chg, lb, pct, lb <= 127 ? ", LOW" : "", lc,
		       lc >= 64 ? "plugged in" : "not plugged in");
		fflush(stdout);
		sleep(1);
	}
	return 0;
}

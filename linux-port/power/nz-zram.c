/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nz-zram: switch on compressed swap in RAM (zram, gma-native-power and
 * later). Sets /sys/block/zram0/disksize, writes a version-1 swap header to
 * /dev/zram0 (a RAM device; nothing touches the SD card) and calls swapon with
 * a high priority. The RAM BusyBox has no mkswap/swapon, hence this tool.
 * Usage: nz-zram SIZE_MB   (4..64)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	long mb = argc > 1 ? atol(argv[1]) : 0, page = sysconf(_SC_PAGESIZE);
	unsigned long long bytes;
	char text[32], *header;
	FILE *f;
	int fd, prio = 100;

	if (mb < 4 || mb > 64 || page < 4096 || page > 65536) {
		fputs("Usage: nz-zram SIZE_MB (4..64)\n", stderr);
		return 1;
	}
	bytes = (unsigned long long)mb << 20;
	f = fopen("/sys/block/zram0/disksize", "w");
	if (!f) { perror("/sys/block/zram0/disksize"); return 1; }
	fprintf(f, "%llu\n", bytes);
	if (fclose(f)) { perror("set zram disksize"); return 1; }

	header = calloc(1, page);
	if (!header) return 1;
	/* union swap_header, version 1 */
	*(uint32_t *)(header + 1024) = 1;                          /* version */
	*(uint32_t *)(header + 1028) = (uint32_t)(bytes / page - 1); /* last_page */
	memcpy(header + page - 10, "SWAPSPACE2", 10);
	fd = open("/dev/zram0", O_WRONLY | O_CLOEXEC);
	if (fd < 0) { perror("/dev/zram0"); return 1; }
	if (write(fd, header, page) != page || fsync(fd)) { perror("write swap header"); return 1; }
	close(fd);
	if (swapon("/dev/zram0", SWAP_FLAG_PREFER | (prio << SWAP_FLAG_PRIO_SHIFT))) {
		perror("swapon /dev/zram0");
		return 1;
	}
	snprintf(text, sizeof(text), "%ld", mb);
	printf("ZRAM_SWAP_ON size=%sMiB page=%ld\n", text, page);
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb-regs: READ-ONLY snapshot of the R36S USB controllers (MUSB) and their
 * PHYs, for linux-port/usbhost (Phase 0). Runs on the stock 4.4 kernel and on
 * the modern NZEUS kernels (needs /dev/mem, opened O_RDONLY, mapped PROT_READ:
 * this program cannot write any register).
 *
 * What it reads (USB-HOST.md "Phase 0"), each at the width the drivers use:
 *   clock gates   GATE1 0x18800064 (bit24 usb0, bit25 usb1; SET = clock OFF),
 *                 SEL_REG1 0x1880007c (bit12/13 usb0/usb1 clock select)
 *   resets        0x18800080 bit28 (usb0), 0x18800084 bit29 (usb1); SET = in reset
 *   MUSB common   FADDR 00, POWER 01, INTRTXE 06, INTRRXE 08, INTRUSBE 0b,
 *                 FRAME 0c, INDEX 0e, TESTMODE 0f, DEVCTL 60, BABBLE/MISC 61,
 *                 HWVERS 6c, EPINFO..LS_EOF1 78-7e
 *   vendor UTMI   one 32-bit read at +0x380 (bytes 0x380 UTMI, 0x381 VBUS, ...),
 *                 exactly what the musb glue itself reads
 *   PHY           usb0phy 0x18845000..0x18845023, usb1phy 0x18845100..0x18845123,
 *                 byte reads (the PHY driver uses readb) plus UPHY20 as 32 bits
 *
 * What it NEVER reads, on purpose (reads with side effects):
 *   +0x02 INTRTX, +0x04 INTRRX, +0x0a INTRUSB: interrupt status, CLEARED BY
 *       READING - a read here would steal interrupts from the running driver
 *       and could hang a transfer or a connect/disconnect event.
 *   +0x20..+0x5c endpoint FIFOs: a read POPS data out of the FIFO (lost packet).
 *   +0x10..+0x1f indexed endpoint CSRs: harmless to read, but they belong to
 *       whatever endpoint the driver selected in INDEX; not useful, skipped.
 *   +0x62..+0x67 FIFO size/address (also INDEX-dependent), +0x70..+0x77 ULPI
 *       (this core is UTMI), +0x80.. bus-control, +0x100.. flat CSRs,
 *       +0x200.. DMA (its interrupt register also clears on read).
 * A controller whose clock is gated off or which is held in reset is NOT read,
 * and neither is its PHY (on some SoCs that stalls the bus). usb1 and its PHY
 * are only read with --usb1.
 *
 * Usage: usb-regs [--usb1]
 * Output: one "name=value" item per line, stable order, for diffing.
 */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SYS_BASE  0x18800000u
#define USB0_BASE 0x18844000u
#define PHY_BASE  0x18845000u
#define USB1_BASE 0x18850000u

static volatile const uint8_t *map_ro(int fd, uint32_t base)
{
	void *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, base);

	return p == MAP_FAILED ? NULL : (volatile const uint8_t *)p;
}

static uint8_t rb(volatile const uint8_t *b, unsigned off) { return b[off]; }
static uint16_t rw(volatile const uint8_t *b, unsigned off) { return *(volatile const uint16_t *)(b + off); }
static uint32_t rl(volatile const uint8_t *b, unsigned off) { return *(volatile const uint32_t *)(b + off); }

/* The registers that are safe to read (no read side effects), with widths. */
static const struct { const char *name; unsigned off; unsigned width; } musb_regs[] = {
	{ "FADDR", 0x00, 1 }, { "POWER", 0x01, 1 },
	{ "INTRTXE", 0x06, 2 }, { "INTRRXE", 0x08, 2 }, { "INTRUSBE", 0x0b, 1 },
	{ "FRAME", 0x0c, 2 }, { "INDEX", 0x0e, 1 }, { "TESTMODE", 0x0f, 1 },
	{ "DEVCTL", 0x60, 1 }, { "MISC61", 0x61, 1 }, { "HWVERS", 0x6c, 2 },
	{ "EPINFO", 0x78, 1 }, { "RAMINFO", 0x79, 1 }, { "LINKINFO", 0x7a, 1 },
	{ "VPLEN", 0x7b, 1 }, { "HS_EOF1", 0x7c, 1 }, { "FS_EOF1", 0x7d, 1 },
	{ "LS_EOF1", 0x7e, 1 },
};

/* Offsets that must never be read (checked at start: a table edit cannot add one). */
static int forbidden(unsigned off, unsigned width)
{
	for (unsigned a = off; a < off + width; ++a) {
		if ((a >= 0x02 && a <= 0x05) || a == 0x0a ||	/* INTRTX, INTRRX, INTRUSB */
		    (a >= 0x10 && a <= 0x5f) ||			/* indexed CSRs + FIFOs */
		    (a >= 0x62 && a <= 0x67) || (a >= 0x70 && a <= 0x77) ||
		    (a >= 0x80 && a < 0x380) || a >= 0x384)
			return 1;
	}
	return 0;
}

static void dump_musb(const char *tag, volatile const uint8_t *m)
{
	uint8_t devctl = rb(m, 0x60), power = rb(m, 0x01);
	uint32_t utmi;

	for (unsigned i = 0; i < sizeof(musb_regs) / sizeof(musb_regs[0]); ++i) {
		unsigned off = musb_regs[i].off, w = musb_regs[i].width;

		if (w == 1)
			printf("%s.%s(+%02x)=%02x\n", tag, musb_regs[i].name, off, rb(m, off));
		else
			printf("%s.%s(+%02x)=%04x\n", tag, musb_regs[i].name, off, rw(m, off));
	}
	utmi = rl(m, 0x380);
	printf("%s.VENDOR380(+380,32bit)=%08x\n", tag, utmi);
	printf("%s.UTMI(+380)=%02x disable_phy=%u suspend_phy=%u otg_bits(3:2)=%u peripheral=%u disable_otg=%u\n",
	       tag, utmi & 0xff, utmi & 1, (utmi >> 1) & 1, (utmi >> 2) & 3, (utmi >> 4) & 1, (utmi >> 7) & 1);
	printf("%s.VBUS(+381)=%02x\n", tag, (utmi >> 8) & 0xff);
	printf("%s.DEVCTL_DECODE session=%u host_req=%u host_mode=%u vbus_level=%u lsdev=%u fsdev=%u b_device=%u\n",
	       tag, devctl & 1, (devctl >> 1) & 1, (devctl >> 2) & 1, (devctl >> 3) & 3,
	       (devctl >> 5) & 1, (devctl >> 6) & 1, (devctl >> 7) & 1);
	printf("%s.POWER_DECODE softconn=%u hs_enable=%u hs_mode=%u reset=%u resume=%u suspend=%u\n",
	       tag, (power >> 6) & 1, (power >> 5) & 1, (power >> 4) & 1, (power >> 3) & 1,
	       (power >> 2) & 1, (power >> 1) & 1);
}

static void dump_phy(const char *tag, volatile const uint8_t *p, unsigned off)
{
	printf("%s.bytes(+00..+23)=", tag);
	for (unsigned i = 0; i < 0x24; ++i)
		printf("%02x%s", rb(p, off + i), i == 0x23 ? "\n" : " ");
	printf("%s.UPHY20(32bit)=%08x force_vbus_valid(bit15)=%u\n", tag, rl(p, off + 0x20),
	       (rl(p, off + 0x20) >> 15) & 1);
}

int main(int argc, char **argv)
{
	volatile const uint8_t *sys, *usb0, *usb1 = NULL, *phy;
	int want_usb1 = argc > 1 && !strcmp(argv[1], "--usb1");
	uint32_t gate1, sel, rst1, rst2;
	int fd;

	if (argc > 2 || (argc == 2 && !want_usb1)) {
		fprintf(stderr, "usage: usb-regs [--usb1]\n");
		return 2;
	}
	for (unsigned i = 0; i < sizeof(musb_regs) / sizeof(musb_regs[0]); ++i)
		if (forbidden(musb_regs[i].off, musb_regs[i].width)) {
			fprintf(stderr, "internal error: %s is a read-side-effect register\n", musb_regs[i].name);
			return 3;
		}
	/* NZ_USB_MEM: a plain file standing in for /dev/mem, PC tests only */
	fd = open(getenv("NZ_USB_MEM") ? getenv("NZ_USB_MEM") : "/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open /dev/mem read-only"); return 1; }
	sys = map_ro(fd, SYS_BASE);
	usb0 = map_ro(fd, USB0_BASE);
	phy = map_ro(fd, PHY_BASE);
	if (!sys || !usb0 || !phy) { perror("read-only mmap"); return 1; }

	gate1 = rl(sys, 0x64);
	sel = rl(sys, 0x7c);
	rst1 = rl(sys, 0x80);
	rst2 = rl(sys, 0x84);
	printf("USB_REGS_V1 read-only\n");
	printf("SYS.GATE1(18800064)=%08x usb0_clock_off=%u usb1_clock_off=%u\n", gate1, (gate1 >> 24) & 1, (gate1 >> 25) & 1);
	printf("SYS.SEL_REG1(1880007c)=%08x usb0_sel=%u usb1_sel=%u\n", sel, (sel >> 12) & 1, (sel >> 13) & 1);
	printf("SYS.RESET1(18800080)=%08x usb0_in_reset=%u\n", rst1, (rst1 >> 28) & 1);
	printf("SYS.RESET2(18800084)=%08x usb1_in_reset=%u\n", rst2, (rst2 >> 29) & 1);

	if ((gate1 >> 24) & 1 || (rst1 >> 28) & 1) {
		printf("USB0 SKIPPED: clock off or in reset (USB0 and PHY0 not read)\n");
	} else {
		dump_musb("USB0", usb0);
		dump_phy("PHY0", phy, 0x000);
	}

	if (!want_usb1) {
		printf("USB1 not read (give --usb1)\n");
	} else if ((gate1 >> 25) & 1 || (rst2 >> 29) & 1) {
		printf("USB1 SKIPPED: clock off or in reset (not read)\n");
	} else {
		usb1 = map_ro(fd, USB1_BASE);
		if (!usb1) { perror("read-only mmap usb1"); return 1; }
		dump_musb("USB1", usb1);
		dump_phy("PHY1", phy, 0x100);
	}
	printf("USB_REGS_DONE\n");
	return 0;
}

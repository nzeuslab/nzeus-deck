/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nz-cdeject: switch the LB-Link BL-WN650BT (Realtek RTL8821CU) Wi-Fi + Bluetooth
 * dongle out of its "driver CD" mode. In that mode it shows up as a USB disk
 * 0bda:1a2b; mainline usb-storage ignores it on purpose (US_FL_IGNORE_DEVICE),
 * so nothing else will ever eject it. After the eject the dongle re-appears
 * as 0bda:c820 (rtw88_8821cu + btusb).
 *
 * How: like usb_modeswitch "StandardEject", through usbdevfs only
 * (/dev/bus/usb/BBB/DDD, no SCSI/sg needed):
 *   1. find the ONE device whose sysfs idVendor:idProduct is exactly 0bda:1a2b
 *   2. open its usbdevfs node and check the device descriptor read from it is
 *      0bda:1a2b too (refuse anything else - other devices are never touched)
 *   3. find its mass-storage interface (class 08, subclass 06, protocol 50 =
 *      SCSI bulk-only) and the bulk IN/OUT endpoints
 *   4. claim that interface (refused if any kernel driver owns it; we never
 *      detach drivers)
 *   5. bulk-only CBW "PREVENT ALLOW MEDIUM REMOVAL (allow)" + its CSW, then
 *      CBW "START STOP UNIT" with LoEj=1 Start=0 (= eject) + its CSW. The
 *      dongle often disconnects before the last CSW: that counts as success.
 *   6. release the interface, close.
 *
 * Usage: nz-cdeject [--check]     --check: only find and print, send nothing
 * Exit: 0 ejected (or --check found it), 1 error, 2 no CD-mode dongle present.
 * Test hooks (PC only): NZ_USBDEV (default /sys/bus/usb/devices),
 * NZ_DEVBUS (default /dev/bus/usb).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <unistd.h>

#define CD_VID 0x0bda
#define CD_PID 0x1a2b

static const char *env_or(const char *name, const char *fallback)
{
	const char *v = getenv(name);

	return v && *v ? v : fallback;
}

static int read_hex_file(const char *dir, const char *name, unsigned *out)
{
	char path[512], buf[32];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return sscanf(buf, "%x", out) == 1 ? 0 : -1;
}

static int read_dec_file(const char *dir, const char *name, unsigned *out)
{
	char path[512], buf[32];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return sscanf(buf, "%u", out) == 1 ? 0 : -1;
}

/* Find exactly one 0bda:1a2b device; returns 1 found, 0 none, -1 more than one. */
static int find_device(unsigned *bus, unsigned *dev)
{
	const char *sys = env_or("NZ_USBDEV", "/sys/bus/usb/devices");
	DIR *d = opendir(sys);
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char path[512];
		unsigned vid, pid, b, n;

		if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
			continue;	/* interfaces look like 1-1:1.0 */
		snprintf(path, sizeof(path), "%s/%s", sys, e->d_name);
		if (read_hex_file(path, "idVendor", &vid) || read_hex_file(path, "idProduct", &pid))
			continue;
		if (vid != CD_VID || pid != CD_PID)
			continue;
		if (read_dec_file(path, "busnum", &b) || read_dec_file(path, "devnum", &n))
			continue;
		printf("found CD-mode dongle 0bda:1a2b at %s (bus %03u device %03u)\n", e->d_name, b, n);
		*bus = b;
		*dev = n;
		found++;
	}
	closedir(d);
	return found > 1 ? -1 : found;
}

struct target {
	int ifnum;
	unsigned char ep_in, ep_out;
};

/* Parse what read() returns on a usbdevfs node: device + config descriptors. */
static int parse_descriptors(const unsigned char *p, size_t len, struct target *t)
{
	size_t pos;
	int cur_if = -1, cur_ok = 0;

	if (len < 18 || p[0] != 18 || p[1] != 1) {
		fprintf(stderr, "no device descriptor\n");
		return -1;
	}
	if ((p[8] | p[9] << 8) != CD_VID || (p[10] | p[11] << 8) != CD_PID) {
		fprintf(stderr, "REFUSED: the device node is %04x:%04x, not 0bda:1a2b\n",
			p[8] | p[9] << 8, p[10] | p[11] << 8);
		return -1;
	}
	t->ifnum = -1;
	t->ep_in = t->ep_out = 0;
	/* the first configuration only */
	pos = 18;
	if (pos + 9 > len || p[pos + 1] != 2) {
		fprintf(stderr, "no configuration descriptor\n");
		return -1;
	}
	{
		size_t total = p[pos + 2] | p[pos + 3] << 8, end = pos + total;

		if (end > len)
			end = len;
		pos += p[pos];
		while (pos + 2 <= end && p[pos] >= 2) {
			unsigned char blen = p[pos], type = p[pos + 1];

			if (pos + blen > end)
				break;
			if (type == 4 && blen >= 9) {		/* interface */
				cur_if = p[pos + 2];
				cur_ok = p[pos + 3] == 0 && p[pos + 5] == 0x08 &&
					 p[pos + 6] == 0x06 && p[pos + 7] == 0x50;
				if (cur_ok && t->ifnum < 0)
					t->ifnum = cur_if;
			} else if (type == 5 && blen >= 7 && cur_ok && cur_if == t->ifnum) {
				unsigned char addr = p[pos + 2], attr = p[pos + 3] & 3;

				if (attr == 2 && (addr & 0x80) && !t->ep_in)
					t->ep_in = addr;
				else if (attr == 2 && !(addr & 0x80) && !t->ep_out)
					t->ep_out = addr;
			}
			pos += blen;
		}
	}
	if (t->ifnum < 0 || !t->ep_in || !t->ep_out) {
		fprintf(stderr, "no bulk-only mass-storage interface with bulk IN+OUT\n");
		return -1;
	}
	return 0;
}

#ifndef NZ_TEST_BULK
static int bulk(int fd, unsigned char ep, void *data, unsigned len)
{
	struct usbdevfs_bulktransfer b = { .ep = ep, .len = len, .timeout = 3000, .data = data };

	return ioctl(fd, USBDEVFS_BULK, &b);
}
#define claim_interface(fd, n)		ioctl(fd, USBDEVFS_CLAIMINTERFACE, n)
#define release_interface(fd, n)	ioctl(fd, USBDEVFS_RELEASEINTERFACE, n)
#else
/*
 * PC TEST BUILD ONLY (gcc -DNZ_TEST_BULK, test-usbhost-pc.sh): OUT transfers
 * are appended to the file $NZ_BULK_OUT as "endpoint byte + 31 CBW bytes";
 * IN transfers answer with a good CSW, except the eject one when
 * NZ_BULK_GONE=1 (the dongle already left). The console build never has this.
 */
static uint32_t test_tag;
static int bulk(int fd, unsigned char ep, void *data, unsigned len)
{
	unsigned char *d = data;
	FILE *f;

	(void)fd;
	if (!(ep & 0x80)) {
		f = fopen(getenv("NZ_BULK_OUT"), "ab");
		if (!f)
			return -1;
		fputc(ep, f);
		fwrite(data, 1, len, f);
		fclose(f);
		test_tag = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
		return len;
	}
	if (getenv("NZ_BULK_GONE") && test_tag == 0x4e5a0002) {
		errno = ENODEV;
		return -1;
	}
	memset(d, 0, len);
	memcpy(d, "USBS", 4);
	d[4] = test_tag; d[5] = test_tag >> 8; d[6] = test_tag >> 16; d[7] = test_tag >> 24;
	return 13;
}
#define claim_interface(fd, n)		((void)(fd), (void)(n), 0)
#define release_interface(fd, n)	do { (void)(fd); (void)(n); } while (0)
#endif

/* One SCSI command without data: CBW out, CSW in. Returns CSW status or -1. */
static int scsi_nodata(int fd, const struct target *t, uint32_t tag,
		       const unsigned char *cdb, unsigned cdblen, int csw_optional)
{
	unsigned char cbw[31], csw[13];
	int r;

	memset(cbw, 0, sizeof(cbw));
	cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';	/* 0x43425355 */
	cbw[4] = tag; cbw[5] = tag >> 8; cbw[6] = tag >> 16; cbw[7] = tag >> 24;
	/* dCBWDataTransferLength 0, bmCBWFlags 0, bCBWLUN 0 */
	cbw[14] = cdblen;
	memcpy(cbw + 15, cdb, cdblen);
	r = bulk(fd, t->ep_out, cbw, sizeof(cbw));
	if (r != (int)sizeof(cbw)) {
		fprintf(stderr, "CBW %02x not sent (%d, %s)\n", cdb[0], r, strerror(errno));
		return -1;
	}
	r = bulk(fd, t->ep_in, csw, sizeof(csw));
	if (r != (int)sizeof(csw)) {
		if (csw_optional) {
			printf("CSW %02x not received (%s): the dongle probably left CD mode already\n",
			       cdb[0], strerror(errno));
			return 0;
		}
		fprintf(stderr, "CSW %02x not received (%d, %s)\n", cdb[0], r, strerror(errno));
		return -1;
	}
	if (memcmp(csw, "USBS", 4)) {
		fprintf(stderr, "CSW %02x has no USBS signature\n", cdb[0]);
		return -1;
	}
	printf("SCSI %02x: CSW status %u\n", cdb[0], csw[12]);
	return csw[12];
}

int main(int argc, char **argv)
{
	static const unsigned char allow[6] = { 0x1e, 0, 0, 0, 0, 0 };		/* allow removal */
	static const unsigned char eject[6] = { 0x1b, 0, 0, 0, 0x02, 0 };	/* LoEj=1 Start=0 */
	int check = argc == 2 && !strcmp(argv[1], "--check");
	unsigned char desc[4096];
	unsigned bus = 0, dev = 0;
	struct target t;
	char node[512];
	ssize_t len;
	int fd, found, r;

	if (argc > 2 || (argc == 2 && !check)) {
		fprintf(stderr, "usage: nz-cdeject [--check]\n");
		return 1;
	}
	found = find_device(&bus, &dev);
	if (found < 0) {
		fprintf(stderr, "REFUSED: more than one 0bda:1a2b device; unplug all but one\n");
		return 1;
	}
	if (!found) {
		printf("no CD-mode dongle (0bda:1a2b) present\n");
		return 2;
	}
	snprintf(node, sizeof(node), "%s/%03u/%03u", env_or("NZ_DEVBUS", "/dev/bus/usb"), bus, dev);
	fd = open(node, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", node, strerror(errno));
		return 1;
	}
	len = read(fd, desc, sizeof(desc));
	if (len < 0 || parse_descriptors(desc, (size_t)len, &t)) {
		close(fd);
		return 1;
	}
	printf("interface %d, bulk IN %02x, bulk OUT %02x (%s)\n", t.ifnum, t.ep_in, t.ep_out, node);
	if (check) {
		close(fd);
		printf("CHECK_ONLY: nothing sent\n");
		return 0;
	}
	if (claim_interface(fd, &t.ifnum) < 0) {
		fprintf(stderr, "REFUSED: cannot claim interface %d (%s); a driver may own it\n",
			t.ifnum, strerror(errno));
		close(fd);
		return 1;
	}
	r = scsi_nodata(fd, &t, 0x4e5a0001, allow, sizeof(allow), 0);
	if (r < 0)
		printf("allow-removal step failed; trying the eject anyway\n");
	r = scsi_nodata(fd, &t, 0x4e5a0002, eject, sizeof(eject), 1);
	release_interface(fd, &t.ifnum);	/* fails if it already left: fine */
	close(fd);
	if (r < 0) {
		fprintf(stderr, "EJECT_FAILED\n");
		return 1;
	}
	printf("EJECT_SENT: the dongle should come back as 0bda:c820 in a few seconds\n");
	return 0;
}

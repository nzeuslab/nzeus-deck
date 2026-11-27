/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nz-sdboot: decide whether the initramfs may switch into the SD-card Linux
 * system (ports/r36s-hichip/sdroot/README.md, ports/r36s-hichip/rootfs/PLAN.md section 6).
 *
 * It ONLY READS. Every file is opened O_RDONLY; the card is never written.
 * The only thing it may create is a missing /dev/mmcblk0pN node in the RAM
 * /dev (mknod, from the numbers the kernel shows in sysfs).
 *
 *   nz-sdboot [--log FILE]
 *       Prints one line per check and ends with exactly one of
 *         NZ_SDBOOT_OK                      (exit 0: switch to the SD root)
 *         NZ_SDBOOT_STAY_IN_RAM: <reason>   (exit 1 refused, 3 no answer,
 *                                            4 FN held / nzeus.ramonly)
 *       Checks, in this order:
 *         1. kernel command line has no "nzeus.ramonly"
 *         2. FN is not held (input device "r36s-keys", KEY_MENU)
 *         3. mmcblk0p1..p5 appear within 10 s (and no p6)
 *         4. both NZEUS headers (p2 and p5, sector 0): magic, CRC-32,
 *            version, identical; the 5 parts are exactly the planned ones
 *            (same format as ports/r36s-hichip/rootfs/region-tool/nz_region.py)
 *         5. card CID (sysfs) == header CID; card size == header card size
 *         6. each partition: sysfs start/size/name == header; p1, p2, p5
 *            read-only in the kernel (ro=1); /dev node numbers == sysfs
 *         7. p3 holds an ext4 file system whose UUID == header nzroot UUID
 *   nz-sdboot --check-init DIR [--log FILE]
 *       In a child process: chroot(DIR) and check that /sbin/init is a
 *       program this kernel can start (MIPS32 little-endian ELF, o32,
 *       legacy NaN, no FP64; its loader or #! interpreter exists).
 *       Ends with NZ_SDBOOT_INIT_OK or NZ_SDBOOT_STAY_IN_RAM: <reason>.
 *
 * All the work runs in a child process; the parent gives up after a fixed
 * time (exit 3), so a card that never answers cannot block the RAM rescue.
 *
 * TEST ONLY (PC, qemu-mipsel): --fake-root DIR reads every /proc, /sys and
 * /dev path below DIR instead. It is refused unless DIR contains the file
 * NZ-SDBOOT-FAKE-TREE, it never writes /dev/kmsg and never calls mknod, and
 * /init never passes it. Fake trees may use regular files for /dev nodes;
 * a fake evdev node is a text file "0" (FN up) or "1" (FN held). Only in
 * this mode may --wait SEC / --timeout SEC shorten the waits.
 *
 * Build: mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32
 *        -Wall -Wextra -Werror -o nz-sdboot nz-sdboot.c
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAKE_MARKER "NZ-SDBOOT-FAKE-TREE"
#define KEYS_NAME "r36s-keys"
#define FN_CODE KEY_MENU          /* 139, r36s-gma-avp-keys.dts key-fn */
#define HDR_MAGIC "NZEUS-LINUX-AREA"
#define HDR_VERSION 1
#define HDR_PARTS 5
#define EXIT_OK 0
#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_NO_ANSWER 3
#define EXIT_ESCAPE 4

/* The five parts every header must list, in this order (nz_region.py
 * compute_layout): name, kind (KIND table), read-only flag. */
static const struct { const char *name; uint32_t kind; uint32_t ro; } want[HDR_PARTS] = {
	{ "nzstock", 0, 1 }, { "nzhdr", 1, 1 }, { "nzroot", 2, 0 },
	{ "nzdata", 3, 0 }, { "nzhdr2", 1, 1 },
};

struct part { char name[17]; uint64_t start, bytes; uint32_t kind, flags; uint8_t uuid[16]; };
struct header {
	uint64_t card_bytes, fat_end, region_start, region_end;
	uint8_t cid[16];
	uint32_t count;
	struct part p[HDR_PARTS];
};

static const char *root = "";     /* "" on the console; fake tree in tests */
static int fake;
static int log_fd = -1, kmsg_fd = -1;
static int wait_s = 10, timeout_s = 30;

static void say(const char *fmt, ...)
{
	char line[512], kline[540];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n >= (int)sizeof(line))
		n = sizeof(line) - 1;
	printf("nz-sdboot: %s\n", line);
	fflush(stdout);
	if (log_fd >= 0)
		dprintf(log_fd, "%s\n", line);
	if (kmsg_fd >= 0) {
		n = snprintf(kline, sizeof(kline), "<5>nz-sdboot: %s\n", line);
		if (n > 0)
			(void)!write(kmsg_fd, kline, n < (int)sizeof(kline) ? n : (int)sizeof(kline) - 1);
	}
}

/* Path below the fake root (or the real one). */
static const char *P(const char *path)
{
	static char buf[4][512];
	static int i;
	char *b = buf[i++ & 3];

	snprintf(b, sizeof(buf[0]), "%s%s", root, path);
	return b;
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&t, &t) < 0 && errno == EINTR)
		;
}

/* Read a small sysfs/proc text file, without the trailing newline. */
static int read_text(const char *path, char *out, size_t size)
{
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, out, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	out[n] = 0;
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = 0;
	return 0;
}

static int read_u64(const char *path, uint64_t *value)
{
	char text[64], *end;

	if (read_text(path, text, sizeof(text)) < 0 || !text[0])
		return -1;
	errno = 0;
	*value = strtoull(text, &end, 10);
	return (errno || *end) ? -1 : 0;
}

static uint32_t crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static void hex(const uint8_t *b, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", b[i]);
}

/* ------------------------------------------------------------- 1. cmdline */
static int ram_only_requested(void)
{
	char line[4096], *tok, *save = NULL;

	if (read_text(P("/proc/cmdline"), line, sizeof(line)) < 0)
		return 0;
	for (tok = strtok_r(line, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save))
		if (!strcmp(tok, "nzeus.ramonly"))
			return 1;
	return 0;
}

/* -------------------------------------------------------------- 2. FN key */
/* 1 = FN held, 0 = not held, -1 = cannot tell (reason printed). */
static int fn_held(void)
{
	char path[256], name[64], dev[128];
	struct dirent *e;
	DIR *d;
	int found = -1, attempts;

	/* The keys driver probes before /init runs; allow 3 s anyway. */
	for (attempts = 0; attempts < 30 && found < 0; attempts++) {
		if (attempts)
			sleep_ms(100);
		d = opendir(P("/sys/class/input"));
		if (!d)
			continue;
		while ((e = readdir(d))) {
			if (strncmp(e->d_name, "event", 5))
				continue;
			snprintf(path, sizeof(path), "/sys/class/input/%.32s/device/name", e->d_name);
			if (read_text(P(path), name, sizeof(name)) == 0 && !strcmp(name, KEYS_NAME)) {
				snprintf(dev, sizeof(dev), "/dev/input/%.32s", e->d_name);
				found = 1;
				break;
			}
		}
		closedir(d);
	}
	if (found < 0) {
		say("no input device named %s: cannot read FN", KEYS_NAME);
		return -1;
	}
	struct stat st;
	int fd = open(P(dev), O_RDONLY | O_CLOEXEC | O_NONBLOCK);

	if (fd < 0 || fstat(fd, &st) < 0) {
		say("cannot open %s (%s): cannot read FN", dev, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	if (fake && S_ISREG(st.st_mode)) {
		char c = '0';

		(void)!read(fd, &c, 1);
		close(fd);
		say("FN %s (%s, fake)", c == '1' ? "HELD" : "not held", dev);
		return c == '1';
	}
	if (!S_ISCHR(st.st_mode)) {
		close(fd);
		say("%s is not a character device: cannot read FN", dev);
		return -1;
	}
	/*
	 * r36s-keys is gpio-keys-polled: the poller runs only while the device
	 * is open, and its FIRST poll reports every key as pressed (last_state
	 * starts at -1) until the real GPIO level is read one poll (20 ms)
	 * later. So read the key state twice, 150 ms and 300 ms after opening;
	 * FN counts as held if either reading says so (a false "held" only
	 * keeps the console in the RAM rescue system, which is always safe).
	 */
	unsigned char keys[(KEY_MAX + 7) / 8];
	int held = 0;

	for (int round = 0; round < 2; round++) {
		sleep_ms(150);
		memset(keys, 0, sizeof(keys));
		if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
			say("EVIOCGKEY on %s failed (%s): cannot read FN", dev, strerror(errno));
			close(fd);
			return -1;
		}
		if (keys[FN_CODE / 8] & (1 << (FN_CODE % 8)))
			held = 1;
	}
	close(fd);
	say("FN %s (%s)", held ? "HELD" : "not held", dev);
	return held;
}

/* ------------------------------------------------------ 3. wait for card */
static int sysfs_dev(int n, unsigned *maj, unsigned *min)
{
	char path[96], text[32];

	snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d/dev", n);
	if (read_text(P(path), text, sizeof(text)) < 0)
		return -1;
	return sscanf(text, "%u:%u", maj, min) == 2 ? 0 : -1;
}

/* /dev/mmcblk0pN exists and is the device sysfs names (or a fake file). */
static int dev_node_ok(int n, int create)
{
	char dev[48];
	unsigned maj, min;
	struct stat st;

	snprintf(dev, sizeof(dev), "/dev/mmcblk0p%d", n);
	if (sysfs_dev(n, &maj, &min) < 0)
		return 0;
	if (stat(P(dev), &st) < 0) {
		if (!create || fake || errno != ENOENT)
			return 0;
		/* devtmpfs normally makes it; if not, make the node in RAM /dev. */
		if (mknod(dev, S_IFBLK | 0600, makedev(maj, min)) < 0 || stat(dev, &st) < 0)
			return 0;
		say("created %s (%u:%u) from sysfs", dev, maj, min);
	}
	if (fake && S_ISREG(st.st_mode))
		return 1;
	return S_ISBLK(st.st_mode) && major(st.st_rdev) == maj && minor(st.st_rdev) == min;
}

static int wait_for_card(void)
{
	int tenths = wait_s * 10;

	for (int t = 0; t <= tenths; t++) {
		int all = 1;

		for (int n = 1; n <= HDR_PARTS; n++)
			if (!dev_node_ok(n, 1))
				all = 0;
		if (all) {
			say("mmcblk0p1..p5 present after %d.%d s", t / 10, t % 10);
			return 0;
		}
		if (t < tenths)
			sleep_ms(100);
	}
	for (int n = 1; n <= HDR_PARTS; n++)
		if (!dev_node_ok(n, 0)) {
			say("mmcblk0p%d did not appear within %d s (no card, or no blkdevparts= partitions)",
			    n, wait_s);
			return -1;
		}
	return 0;   /* all appeared just after the last look */
}

/* ------------------------------------------------------------ 4. headers */
static int read_sector(const char *dev, off_t offset, uint8_t *buf, size_t len)
{
	int fd = open(P(dev), O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0) {
		say("cannot open %s read-only: %s", dev, strerror(errno));
		return -1;
	}
	n = pread(fd, buf, len, offset);
	close(fd);
	if (n != (ssize_t)len) {
		say("short read from %s at %lld", dev, (long long)offset);
		return -1;
	}
	return 0;
}

/* Same layout as nz_region.py make_header / parse_header. */
static int parse_header(const char *dev, const uint8_t *raw, struct header *h)
{
	if (memcmp(raw, HDR_MAGIC, 16)) {
		say("%s: no NZEUS header magic", dev);
		return -1;
	}
	if (le32(raw + 508) != crc32(raw, 508)) {
		say("%s: header CRC mismatch (stored %08x, computed %08x)", dev, le32(raw + 508), crc32(raw, 508));
		return -1;
	}
	if (le32(raw + 16) != HDR_VERSION || le32(raw + 20) != 512) {
		say("%s: unknown header version %u / size %u", dev, le32(raw + 16), le32(raw + 20));
		return -1;
	}
	h->card_bytes = le64(raw + 24);
	h->fat_end = le64(raw + 32);
	h->region_start = le64(raw + 40);
	h->region_end = le64(raw + 48);
	memcpy(h->cid, raw + 56, 16);
	h->count = le32(raw + 104);
	if (h->count != HDR_PARTS) {
		say("%s: header lists %u parts, expected %d", dev, h->count, HDR_PARTS);
		return -1;
	}
	for (int i = 0; i < HDR_PARTS; i++) {
		const uint8_t *q = raw + 112 + 64 * i;
		struct part *p = &h->p[i];

		memcpy(p->name, q, 16);
		p->name[16] = 0;
		p->start = le64(q + 16);
		p->bytes = le64(q + 24);
		p->kind = le32(q + 32);
		p->flags = le32(q + 36);
		memcpy(p->uuid, q + 40, 16);
		if (strcmp(p->name, want[i].name) || p->kind != want[i].kind || (p->flags & 1) != want[i].ro) {
			say("%s: part %d is %s (kind %u, ro %u), expected %s (kind %u, ro %u)", dev, i + 1,
			    p->name, p->kind, p->flags & 1, want[i].name, want[i].kind, want[i].ro);
			return -1;
		}
		if (p->start % 512 || p->bytes % 512 || !p->bytes) {
			say("%s: part %s is not whole sectors", dev, p->name);
			return -1;
		}
	}
	if (h->region_start != h->p[1].start || h->region_end != h->p[4].start + h->p[4].bytes) {
		say("%s: area start/end do not match nzhdr/nzhdr2", dev);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------- 7. ext4 root */
static int check_root_fs(const struct header *h)
{
	uint8_t sb[1024];
	char want_uuid[33], got[33];

	if (read_sector("/dev/mmcblk0p3", 1024, sb, sizeof(sb)) < 0)
		return -1;
	if (le16(sb + 0x38) != 0xef53) {
		say("mmcblk0p3 (nzroot) holds no ext2/3/4 file system");
		return -1;
	}
	hex(sb + 0x68, 16, got);
	hex(h->p[2].uuid, 16, want_uuid);
	if (memcmp(sb + 0x68, h->p[2].uuid, 16)) {
		say("mmcblk0p3 file system UUID %s is not the header's nzroot UUID %s", got, want_uuid);
		return -1;
	}
	say("mmcblk0p3: ext4 superblock, UUID %s = header", got);
	return 0;
}

static int check_card(void)
{
	uint8_t raw[2][512];
	struct header h[2];
	char cid_text[64], cid_hdr[33], path[96], text[512];
	uint64_t card_sectors = 0, value = 0;

	if (wait_for_card() < 0)
		return EXIT_REFUSED;
	if (dev_node_ok(6, 0) || access(P("/sys/class/block/mmcblk0p6"), F_OK) == 0) {
		say("mmcblk0p6 exists: the partitions are not the planned five");
		return EXIT_REFUSED;
	}
	if (read_sector("/dev/mmcblk0p2", 0, raw[0], 512) < 0 ||
	    read_sector("/dev/mmcblk0p5", 0, raw[1], 512) < 0)
		return EXIT_REFUSED;
	if (parse_header("mmcblk0p2 (nzhdr)", raw[0], &h[0]) < 0 ||
	    parse_header("mmcblk0p5 (nzhdr2)", raw[1], &h[1]) < 0)
		return EXIT_REFUSED;
	if (memcmp(raw[0], raw[1], 512)) {
		say("the two headers (p2, p5) are both valid but differ");
		return EXIT_REFUSED;
	}
	say("both headers valid (CRC %08x) and identical", le32(raw[0] + 508));

	/* 5. this exact card */
	if (read_text(P("/sys/block/mmcblk0/device/cid"), cid_text, sizeof(cid_text)) < 0) {
		say("cannot read the card CID from sysfs");
		return EXIT_REFUSED;
	}
	hex(h[0].cid, 16, cid_hdr);
	if (strcasecmp(cid_text, cid_hdr)) {
		say("card CID %s is not the header CID %s (wrong card)", cid_text, cid_hdr);
		return EXIT_REFUSED;
	}
	say("card CID %s = header", cid_hdr);
	if (read_u64(P("/sys/block/mmcblk0/size"), &card_sectors) < 0 || card_sectors * 512 != h[0].card_bytes) {
		say("card size %llu sectors is not the header card size %llu bytes",
		    (unsigned long long)card_sectors, (unsigned long long)h[0].card_bytes);
		return EXIT_REFUSED;
	}

	/* 6. every partition exactly as the header says */
	for (int n = 1; n <= HDR_PARTS; n++) {
		const struct part *p = &h[0].p[n - 1];
		char *name;

		snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d/start", n);
		if (read_u64(P(path), &value) < 0 || value * 512 != p->start) {
			say("mmcblk0p%d starts at sector %llu, header %s says %llu", n,
			    (unsigned long long)value, p->name, (unsigned long long)(p->start / 512));
			return EXIT_REFUSED;
		}
		snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d/size", n);
		if (read_u64(P(path), &value) < 0 || value * 512 != p->bytes) {
			say("mmcblk0p%d has %llu sectors, header %s says %llu", n,
			    (unsigned long long)value, p->name, (unsigned long long)(p->bytes / 512));
			return EXIT_REFUSED;
		}
		snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d/uevent", n);
		if (read_text(P(path), text, sizeof(text)) < 0 || !(name = strstr(text, "PARTNAME=")) ||
		    strncmp(name + 9, p->name, strlen(p->name)) ||
		    (name[9 + strlen(p->name)] != '\n' && name[9 + strlen(p->name)] != 0)) {
			say("mmcblk0p%d is not named %s (blkdevparts= line differs from the header)", n, p->name);
			return EXIT_REFUSED;
		}
		snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d/ro", n);
		if (read_u64(P(path), &value) < 0) {
			say("cannot read mmcblk0p%d/ro", n);
			return EXIT_REFUSED;
		}
		if (want[n - 1].ro && value != 1) {
			say("mmcblk0p%d (%s) is NOT read-only in the kernel", n, p->name);
			return EXIT_REFUSED;
		}
		say("mmcblk0p%d %-7s start %llu size %llu ro=%llu: matches", n, p->name,
		    (unsigned long long)(p->start / 512), (unsigned long long)(p->bytes / 512),
		    (unsigned long long)value);
	}
	if (check_root_fs(&h[0]) < 0)
		return EXIT_REFUSED;
	return EXIT_OK;
}

static int decide(void)
{
	int fn;

	if (ram_only_requested()) {
		say("kernel command line has nzeus.ramonly");
		return EXIT_ESCAPE;
	}
	fn = fn_held();
	if (fn < 0)
		return EXIT_REFUSED;
	if (fn) {
		say("FN is held: staying in the RAM rescue system");
		return EXIT_ESCAPE;
	}
	return check_card();
}

/* ----------------------------------------------------- --check-init DIR */
static int check_elf(const char *path, int depth);

static int check_interp(const char *path, const char *interp, int depth)
{
	if (interp[0] != '/') {
		say("%s: interpreter '%s' is not an absolute path", path, interp);
		return -1;
	}
	if (depth > 2) {
		say("%s: interpreter chain too long", path);
		return -1;
	}
	return check_elf(interp, depth + 1);
}

static int check_elf(const char *path, int depth)
{
	uint8_t eh[52], ph[32];
	char interp[256];
	struct stat st;
	int fd;

	if (stat(path, &st) < 0) {
		say("%s is missing in the SD root (%s)", path, strerror(errno));
		return -1;
	}
	if (!S_ISREG(st.st_mode) || access(path, X_OK) < 0) {
		say("%s is not an executable file", path);
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, eh, sizeof(eh)) < 2) {
		say("cannot read %s", path);
		if (fd >= 0)
			close(fd);
		return -1;
	}
	if (eh[0] == '#' && eh[1] == '!') {
		ssize_t n = pread(fd, interp, sizeof(interp) - 1, 2);
		char *s, *e;

		close(fd);
		interp[n > 0 ? n : 0] = 0;
		for (s = interp; *s == ' ' || *s == '\t'; s++)
			;
		for (e = s; *e && *e != ' ' && *e != '\t' && *e != '\n'; e++)
			;
		*e = 0;
		say("%s is a script for %s", path, s);
		return check_interp(path, s, depth);
	}
	if (memcmp(eh, "\177ELF", 4) || eh[4] != 1 || eh[5] != 1) {
		close(fd);
		say("%s is not a 32-bit little-endian ELF program", path);
		return -1;
	}
	uint16_t type = le16(eh + 16), machine = le16(eh + 18), phentsize = le16(eh + 42), phnum = le16(eh + 44);
	uint32_t flags = le32(eh + 36), phoff = le32(eh + 28);

	if (machine != 8 || (type != 2 && type != 3)) {
		close(fd);
		say("%s is not a MIPS program (machine %u, type %u)", path, machine, type);
		return -1;
	}
	if (flags & 0x20) {             /* EF_MIPS_ABI2 = n32 */
		close(fd);
		say("%s uses the n32 ABI; this kernel runs o32 programs", path);
		return -1;
	}
	if ((flags & 0xf0000000u) == 0x90000000u || (flags & 0xf0000000u) == 0xa0000000u) {
		close(fd);
		say("%s is built for MIPS32/64 release 6; the 74Kc is release 2", path);
		return -1;
	}
	if (flags & 0x400) {            /* EF_MIPS_NAN2008 */
		close(fd);
		say("%s uses NaN2008; the 74Kc FPU and this kernel use legacy NaN", path);
		return -1;
	}
	if (flags & 0x200) {            /* EF_MIPS_FP64 */
		close(fd);
		say("%s needs 64-bit FPU registers (FP64); kernel has no MIPS_O32_FP64_SUPPORT", path);
		return -1;
	}
	interp[0] = 0;
	for (unsigned i = 0; i < phnum && phentsize >= 32; i++) {
		if (pread(fd, ph, sizeof(ph), phoff + (off_t)i * phentsize) != (ssize_t)sizeof(ph))
			break;
		if (le32(ph) == 3) {     /* PT_INTERP */
			uint32_t off = le32(ph + 4), size = le32(ph + 16);
			ssize_t n;

			if (size == 0 || size >= sizeof(interp)) {
				close(fd);
				say("%s: bad PT_INTERP", path);
				return -1;
			}
			n = pread(fd, interp, size, off);
			interp[n > 0 ? n : 0] = 0;
		}
	}
	close(fd);
	if (!interp[0]) {
		say("%s: static MIPS32 little-endian program (flags %08x)", path, flags);
		return 0;
	}
	say("%s: MIPS32 little-endian program, loader %s", path, interp);
	return check_interp(path, interp, depth);
}

static int check_init(const char *dir)
{
	if (chroot(dir) < 0 || chdir("/") < 0) {
		say("cannot chroot into %s: %s", dir, strerror(errno));
		return EXIT_REFUSED;
	}
	return check_elf("/sbin/init", 0) < 0 ? EXIT_REFUSED : EXIT_OK;
}

/* --------------------------------------------------------------- main */
static int usage(void)
{
	fputs("usage: nz-sdboot [--log FILE]\n"
	      "       nz-sdboot --check-init DIR [--log FILE]\n"
	      "test only: --fake-root DIR [--wait SEC] [--timeout SEC]\n", stderr);
	return EXIT_USAGE;
}

int main(int argc, char **argv)
{
	const char *log = NULL, *init_dir = NULL;
	char marker[600];
	int pipefd[2], wait_set = 0, timeout_set = 0;
	struct pollfd pfd;
	unsigned char verdict;
	pid_t child;

	for (int i = 1; i < argc; i++) {
		if (i + 1 >= argc)
			return usage();
		if (!strcmp(argv[i], "--log"))
			log = argv[++i];
		else if (!strcmp(argv[i], "--check-init"))
			init_dir = argv[++i];
		else if (!strcmp(argv[i], "--fake-root"))
			root = argv[++i], fake = 1;
		else if (!strcmp(argv[i], "--wait"))
			wait_s = atoi(argv[++i]), wait_set = 1;
		else if (!strcmp(argv[i], "--timeout"))
			timeout_s = atoi(argv[++i]), timeout_set = 1;
		else
			return usage();
	}
	if (fake) {
		struct stat st;

		snprintf(marker, sizeof(marker), "%s/" FAKE_MARKER, root);
		if (!root[0] || !strcmp(root, "/") || stat(marker, &st) < 0 || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "nz-sdboot: --fake-root needs a test tree containing %s\n", FAKE_MARKER);
			return EXIT_USAGE;
		}
	} else if (wait_set || timeout_set) {
		fputs("nz-sdboot: --wait/--timeout are only for --fake-root tests\n", stderr);
		return EXIT_USAGE;
	}
	if (wait_s < 0 || wait_s > 30 || timeout_s < 1 || timeout_s > 60)
		return usage();
	if (log)
		log_fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (!fake)
		kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	/* The child does the work; the parent waits at most timeout_s. */
	if (pipe(pipefd) < 0)
		return EXIT_NO_ANSWER;
	fflush(stdout);
	child = fork();
	if (child < 0) {
		say("fork failed: %s", strerror(errno));
		puts("NZ_SDBOOT_STAY_IN_RAM: internal error");
		return EXIT_NO_ANSWER;
	}
	if (child == 0) {
		close(pipefd[0]);
		verdict = (unsigned char)(init_dir ? check_init(init_dir) : decide());
		(void)!write(pipefd[1], &verdict, 1);
		_exit(0);
	}
	close(pipefd[1]);
	pfd.fd = pipefd[0];
	pfd.events = POLLIN;
	int r;

	do
		r = poll(&pfd, 1, timeout_s * 1000);
	while (r < 0 && errno == EINTR);
	if (r <= 0 || read(pipefd[0], &verdict, 1) != 1) {
		/* A child stuck in the kernel (card not answering) is left alone. */
		say(r == 0 ? "no answer within %d s (card or driver not responding)"
			   : "the check process failed (%d)", r == 0 ? timeout_s : r);
		puts("NZ_SDBOOT_STAY_IN_RAM: no answer");
		if (log_fd >= 0)
			dprintf(log_fd, "NZ_SDBOOT_STAY_IN_RAM: no answer\n");
		return EXIT_NO_ANSWER;
	}
	waitpid(child, NULL, 0);
	const char *what = verdict == EXIT_OK ? (init_dir ? "NZ_SDBOOT_INIT_OK" : "NZ_SDBOOT_OK")
			 : verdict == EXIT_ESCAPE ? "NZ_SDBOOT_STAY_IN_RAM: escape requested"
			 : "NZ_SDBOOT_STAY_IN_RAM: a check failed (see the line above)";

	puts(what);
	if (log_fd >= 0)
		dprintf(log_fd, "%s\n", what);
	if (kmsg_fd >= 0)
		dprintf(kmsg_fd, "<5>nz-sdboot: %s\n", what);
	return verdict;
}

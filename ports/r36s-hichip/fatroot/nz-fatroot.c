/*
 * nz-fatroot: start NZEUS Deck from files on the card's normal FAT32
 * partition (the "copy-and-go" install). Runs in the boot RAM disk.
 *
 *   nz-fatroot find DEV
 *       Prints FAT_OFFSET=<bytes> and FAT_SIZE=<bytes> of the card's FAT32.
 *   nz-fatroot attach DEV NEWROOT [--dry-run]
 *       FAT32 (through a loop device) -> /fat, read-write, noatime, flush.
 *       /fat/nzeus/nzroot.img -> NEWROOT (ext4, read-only; Debian remounts it).
 *       /fat/nzeus/nzdata.img -> /data0 (ext4, the user's data).
 *       e2fsck -p checks both images first. --dry-run prints the steps only.
 *   nz-fatroot detach NEWROOT
 *       Unmounts /data0, NEWROOT and /fat (loop devices free themselves).
 *   nz-fatroot show TEXT
 *       Draws TEXT on the screen (/dev/fb0): the kernel has no text console there.
 *
 * Stock cards keep sector 0 scrambled (every 4 bytes XOR EF BE AD DE), so the
 * kernel's own partition reader sees nothing; this program reads it itself,
 * with the same rules as tools/r36s_card.py and the Windows installer.
 *
 * Exit codes: 0 ok, 3 no R36S FAT32 / card did not answer, 4 a system file is
 * missing or the wrong size, 5 a system file is damaged, 6 a mount failed,
 * 1 usage or other error. Every failure prints ONE line starting "fatroot:".
 *
 * PC tests only: NZ_ROOT (prefix for /fat, /data0 and NEWROOT), NZ_E2FSCK
 * (e2fsck path), NZ_ROOT_IMG_SIZE (expected nzroot.img size), NZ_FB + NZ_FB_GEOMETRY
 * (a plain file as the screen, e.g. 640x480x16).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SECTOR 512
#define ROOT_IMG_SIZE 3758096384ULL      /* 3.5 GiB: what the release ships */
#define DATA_IMG_MIN (16ULL << 20)       /* anything smaller can't be real */

static int dry_run;
static char path_buf[3][512];

/* NZ_ROOT + path (only for tests; on the console NZ_ROOT is unset) */
static const char *P(int slot, const char *path)
{
	const char *root = getenv("NZ_ROOT");
	snprintf(path_buf[slot], sizeof path_buf[slot], "%s%s", root ? root : "", path);
	return path_buf[slot];
}

static int fail(int code, const char *what)
{
	printf("fatroot: %s\n", what);
	fflush(stdout);
	return code;
}

static void card_timeout(int sig)
{
	(void)sig;
	static const char msg[] = "fatroot: the SD card did not answer within 10 seconds\n";
	if (write(1, msg, sizeof msg - 1) < 0) { /* nothing more we can do */ }
	_exit(3);
}

static uint16_t u16(const uint8_t *b, int o) { return (uint16_t)(b[o] | b[o + 1] << 8); }
static uint32_t u32(const uint8_t *b, int o)
{
	return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24;
}

static int read_at(int fd, uint64_t off, void *buf, size_t n)
{
	return pread(fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

/* Is this (plain) sector 0 an R36S card's table? Fills start/count; returns NULL or why not. */
static const char *check_table(int fd, const uint8_t *mbr, uint64_t size, uint32_t *start, uint32_t *count)
{
	static uint8_t boot[SECTOR], root[128 * SECTOR];
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) return "no partition-table mark";
	/* (bytes 0-439 may hold start-up code: the 16 GB stock card has some, the 64 GB one doesn't) */
	uint8_t type = mbr[446 + 4];
	*start = u32(mbr, 446 + 8);
	*count = u32(mbr, 446 + 12);
	if (type != 0x0B && type != 0x0C) return "the first partition is not FAT32";
	for (int i = 446 + 16; i < 510; i++) if (mbr[i]) return "more than one partition";
	if (*start < 1 || *count < 1) return "an empty partition";
	if (size && ((uint64_t)*start + *count) * SECTOR > size) return "the partition is bigger than the card";
	if (read_at(fd, (uint64_t)*start * SECTOR, boot, SECTOR)) return "could not read the FAT32 start";
	if (boot[510] != 0x55 || boot[511] != 0xAA || memcmp(boot + 0x52, "FAT32   ", 8))
		return "no FAT32 file system where the partition starts";
	if (u32(boot, 0x1C) != *start || u32(boot, 0x20) != *count)
		return "the file system doesn't match the partition table";
	/* the console's CUBEGM folder must be in the top folder */
	unsigned bps = u16(boot, 11), spc = boot[13], reserved = u16(boot, 14), fats = boot[16];
	uint32_t fat_size = u32(boot, 36), root_cluster = u32(boot, 44);
	if (bps != SECTOR || spc < 1 || spc > 128 || root_cluster < 2) return "an unusual FAT32 layout";
	uint64_t root_at = ((uint64_t)*start + reserved + (uint64_t)fats * fat_size +
			    (uint64_t)(root_cluster - 2) * spc) * SECTOR;
	if (read_at(fd, root_at, root, spc * SECTOR)) return "could not read the top folder";
	for (unsigned e = 0; e < spc * SECTOR; e += 32) {
		if (!root[e]) break;
		if (!memcmp(root + e, "CUBEGM     ", 11) && (root[e + 11] & 0x10)) return NULL;
	}
	return "no CUBEGM folder (not an R36S card)";
}

/* Finds the FAT32 on DEV. Returns 0 and fills offset/size, or an exit code. */
static int find_fat(const char *dev, uint64_t *offset, uint64_t *size)
{
	uint8_t s0[SECTOR], plain[SECTOR];
	static const uint8_t key[4] = { 0xEF, 0xBE, 0xAD, 0xDE };
	uint32_t start = 0, count = 0;
	signal(SIGALRM, card_timeout);
	alarm(10);                                  /* a dead card must not stop the boot */
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) { alarm(0); return fail(3, "cannot open the SD card"); }
	off_t end = lseek(fd, 0, SEEK_END);
	uint64_t devsize = end > 0 ? (uint64_t)end : 0;
	if (read_at(fd, 0, s0, SECTOR)) { close(fd); alarm(0); return fail(3, "cannot read the SD card"); }
	const char *why = check_table(fd, s0, devsize, &start, &count);
	if (why) {                                  /* not plain: try the factory scramble */
		for (int i = 0; i < SECTOR; i++) plain[i] = s0[i] ^ key[i % 4];
		const char *why2 = check_table(fd, plain, devsize, &start, &count);
		if (why2) {
			char msg[256];
			snprintf(msg, sizeof msg, "this SD card is not an R36S card (%s)", why2);
			close(fd); alarm(0);
			return fail(3, msg);
		}
	}
	close(fd);
	alarm(0);
	*offset = (uint64_t)start * SECTOR;
	*size = (uint64_t)count * SECTOR;
	return 0;
}

/* ---- loop devices: kept open until the mount holds them (AUTOCLEAR frees them later) ----
 * Direct I/O is asked for: then a loop device's data comes from the card by DMA. Without it
 * the loop driver COPIES the data with the CPU and, on this MIPS 74K (data cache with
 * aliases), does not flush the cache after that copy - files read fine, but programs run
 * from nzroot.img got stale bytes and Debian's /sbin/init died at once (BLACK, 2026-11-18).
 * Prints "direct-io LABEL: on|OFF" (the kernel quietly refuses when it can't). */
static int loop_attach(const char *file, uint64_t offset, uint64_t size, char *name, size_t name_len,
		       const char *label)
{
	int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0) return -1;
	int n = ioctl(ctl, LOOP_CTL_GET_FREE);
	close(ctl);
	if (n < 0) return -1;
	snprintf(name, name_len, "/dev/loop%d", n);
	int lfd = -1;
	for (int i = 0; i < 50 && lfd < 0; i++) {     /* devtmpfs makes the node a moment later */
		lfd = open(name, O_RDWR | O_CLOEXEC);
		if (lfd < 0) usleep(20000);
	}
	int bfd = open(file, O_RDWR | O_CLOEXEC);
	if (lfd < 0 || bfd < 0) { if (lfd >= 0) close(lfd); if (bfd >= 0) close(bfd); return -1; }
	struct loop_config cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.fd = (uint32_t)bfd;
	cfg.info.lo_offset = offset;
	cfg.info.lo_sizelimit = size;
	cfg.info.lo_flags = LO_FLAGS_AUTOCLEAR | LO_FLAGS_DIRECT_IO;
	int rc = ioctl(lfd, LOOP_CONFIGURE, &cfg);
	if (rc < 0 && errno == EINVAL) {                /* an older kernel: without direct I/O */
		cfg.info.lo_flags = LO_FLAGS_AUTOCLEAR;
		rc = ioctl(lfd, LOOP_CONFIGURE, &cfg);
	}
	close(bfd);
	if (rc < 0) { close(lfd); return -1; }
	struct loop_info64 st;                          /* what the kernel really did */
	memset(&st, 0, sizeof st);
	int dio = ioctl(lfd, LOOP_GET_STATUS64, &st) == 0 && (st.lo_flags & LO_FLAGS_DIRECT_IO);
	printf("direct-io %s: %s\n", label, dio ? "on" : "OFF (the kernel refused it)");
	fflush(stdout);
	return lfd;
}

/* Runs e2fsck MODE ("-p" = only safe repairs, "-y" = repair everything) on loopdev.
 * Returns e2fsck's exit code: 0 clean, 1/2 repaired, 4 and up = problems left. */
static int run_e2fsck(const char *mode, const char *loopdev)
{
	const char *e2fsck = getenv("NZ_E2FSCK") ? getenv("NZ_E2FSCK") : "/bin/e2fsck";
	pid_t pid = fork();
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
		execl(e2fsck, "e2fsck", mode, loopdev, (char *)NULL);
		_exit(127);
	}
	int status = 0;
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return 127;
	return WEXITSTATUS(status);
}

/* e2fsck -p: 0 clean, 1/2 repaired, else damaged. Returns 0 or exit code 5. */
static int fsck(const char *loopdev, const char *label, const char *file)
{
	int code = run_e2fsck("-p", loopdev);
	if (code == 0) { printf("fsck %s: clean\n", label); return 0; }
	if (code == 1 || code == 2) { printf("fsck %s: repaired\n", label); return 0; }
	char msg[240];
	snprintf(msg, sizeof msg, "%s is damaged (e2fsck exit %d). Copy NZEUS to the card again with install.bat",
		 file, code);
	return fail(5, msg);
}

/* The user's data image is damaged: one full repair try (e2fsck -y). 0 = usable now. */
static int fsck_data_or_repair(const char *loopdev)
{
	int code = run_e2fsck("-p", loopdev);
	if (code == 0 || code == 1 || code == 2) { printf("fsck data: %s\n", code ? "repaired" : "clean"); return 0; }
	code = run_e2fsck("-y", loopdev);
	if (code == 0 || code == 1 || code == 2) { printf("fsck data: repaired (full repair)\n"); return 0; }
	return -1;
}

static void detach_all(const char *newroot)
{
	umount2(P(0, "/data0"), MNT_DETACH);
	umount2(P(0, newroot), MNT_DETACH);
	umount2(P(0, "/fat"), MNT_DETACH);
}

static int check_image(const char *name, uint64_t want, uint64_t min)
{
	char path[256], msg[300];
	struct stat st;
	snprintf(path, sizeof path, "/fat/nzeus/%s", name);
	if (stat(P(1, path), &st) != 0) {
		snprintf(msg, sizeof msg, "%s is missing on the SD card (run install.bat again)", name);
		return fail(4, msg);
	}
	if ((want && (uint64_t)st.st_size != want) || (uint64_t)st.st_size < min) {
		snprintf(msg, sizeof msg, "%s has the wrong size %lld (copy it again with install.bat)",
			 name, (long long)st.st_size);
		return fail(4, msg);
	}
	return 0;
}

static int attach(const char *dev, const char *newroot)
{
	uint64_t off, size;
	int rc = find_fat(dev, &off, &size);
	if (rc) return rc;
	uint64_t root_size = getenv("NZ_ROOT_IMG_SIZE") ? strtoull(getenv("NZ_ROOT_IMG_SIZE"), NULL, 10)
							: ROOT_IMG_SIZE;
	char fat_loop[32], root_loop[32], data_loop[32];
	int fat_fd = -1, root_fd = -1, data_fd = -1;

	printf("loop %s offset=%llu size=%llu -> fat\n", dev, (unsigned long long)off, (unsigned long long)size);
	if (!dry_run) {
		mkdir(P(0, "/fat"), 0755);
		fat_fd = loop_attach(dev, off, size, fat_loop, sizeof fat_loop, "fat");
		if (fat_fd < 0) return fail(6, "could not make a loop device for the FAT32");
	}
	printf("mount vfat /fat noatime,flush\n");
	if (!dry_run && mount(fat_loop, P(0, "/fat"), "vfat", MS_NOATIME, "flush") != 0) {
		close(fat_fd);
		return fail(6, "could not mount the SD card's FAT32");
	}
	if ((rc = check_image("nzroot.img", root_size, 0)) || (rc = check_image("nzdata.img", 0, DATA_IMG_MIN)))
		goto undo;

	printf("loop /fat/nzeus/nzroot.img -> root\n");
	if (!dry_run && (root_fd = loop_attach(P(1, "/fat/nzeus/nzroot.img"), 0, 0, root_loop, sizeof root_loop, "root")) < 0) {
		rc = fail(6, "could not make a loop device for nzroot.img"); goto undo;
	}
	if (dry_run) printf("fsck root\n");
	else if ((rc = fsck(root_loop, "root", "nzroot.img"))) goto undo;

	printf("loop /fat/nzeus/nzdata.img -> data\n");
	if (!dry_run && (data_fd = loop_attach(P(1, "/fat/nzeus/nzdata.img"), 0, 0, data_loop, sizeof data_loop, "data")) < 0) {
		rc = fail(6, "could not make a loop device for nzdata.img"); goto undo;
	}
	int data_ok = 1;
	if (dry_run) printf("fsck data\n");
	else if (fsck_data_or_repair(data_loop) != 0) {
		/* Windows can't repair ext4, so stopping here would leave the user stuck:
		 * start anyway, with an empty temporary /data. The file on the card is kept. */
		printf("fatroot-warning: nzdata.img is damaged and could not be repaired; NZEUS starts "
		       "with an empty temporary /data (the file on the card is kept)\n");
		close(data_fd);
		data_fd = -1;                             /* AUTOCLEAR frees its loop device */
		data_ok = 0;
	}

	printf("mount ext4 %s ro\n", newroot);
	if (!dry_run) {
		mkdir(P(0, newroot), 0755);
		if (mount(root_loop, P(0, newroot), "ext4", MS_RDONLY | MS_NOATIME, "") != 0) {
			rc = fail(6, "could not mount nzroot.img"); goto undo;
		}
	}
	printf("mount ext4 /data0 noatime\n");
	if (!dry_run) {
		mkdir(P(0, "/data0"), 0755);
		if (data_ok) {
			if (mount(data_loop, P(0, "/data0"), "ext4", MS_NOATIME | MS_NODEV | MS_NOSUID, "") != 0) {
				rc = fail(6, "could not mount nzdata.img"); goto undo;
			}
		} else if (mount("tmpfs", P(0, "/data0"), "tmpfs", MS_NODEV | MS_NOSUID, "size=16m,mode=0755") != 0) {
			rc = fail(6, "could not make a temporary /data"); goto undo;
		}
	}
	rc = 0;
	goto out;
undo:
	if (!dry_run) detach_all(newroot);
out:
	/* the mounts now hold the loop devices; AUTOCLEAR frees each one at its unmount */
	if (data_fd >= 0) close(data_fd);
	if (root_fd >= 0) close(root_fd);
	if (fat_fd >= 0) close(fat_fd);
	fflush(stdout);
	return rc;
}

/* ---- show: a message on the console's screen (there is no text console there) ---- */
#include <linux/fb.h>
#include "font8x16.h"

#define SCALE 2                                   /* 16x32-pixel letters: readable on 3.5" */

static int show(const char *text)
{
	const char *fbpath = getenv("NZ_FB") ? getenv("NZ_FB") : "/dev/fb0";
	unsigned w = 0, h = 0, bpp = 0, line = 0;
	int fd = open(fbpath, O_RDWR | O_CLOEXEC);
	if (fd < 0) return fail(1, "no screen to show the message on");
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0) {
		w = var.xres; h = var.yres; bpp = var.bits_per_pixel; line = fix.line_length;
	} else if (getenv("NZ_FB_GEOMETRY")) {        /* PC tests: a plain file stands in */
		sscanf(getenv("NZ_FB_GEOMETRY"), "%ux%ux%u", &w, &h, &bpp);
		line = w * bpp / 8;
	}
	if (!w || !h || (bpp != 16 && bpp != 32) || line < w * bpp / 8) {
		close(fd);
		return fail(1, "the screen has an unknown format");
	}
	unsigned bytes = bpp / 8;
	uint32_t bg = bpp == 16 ? 0x2104 : 0x202020, fg = bpp == 16 ? 0xFFFF : 0xFFFFFF;
	static uint8_t row[4096 * 4];
	if (line > sizeof row) { close(fd); return fail(1, "the screen is too wide"); }

	/* word-wrap the text into lines that fit the screen */
	unsigned cols = w / (8 * SCALE) - 4, nlines = 0;   /* 2 letters of margin each side */
	char lines[16][128];
	const char *p = text;
	while (*p && nlines < 16) {
		unsigned n = (unsigned)strlen(p);
		if (n > cols) {                           /* break at the last space that fits */
			n = cols;
			while (n > 0 && p[n] != ' ') n--;
			if (n == 0) n = cols;
		}
		if (n > 127) n = 127;
		memcpy(lines[nlines], p, n);
		lines[nlines++][n] = 0;
		p += n;
		while (*p == ' ') p++;
	}
	unsigned top = h > nlines * 16 * SCALE ? (h - nlines * 16 * SCALE) / 2 : 0;

	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {        /* background */
			uint32_t c = bg;
			memcpy(row + x * bytes, &c, bytes);
		}
		if (y >= top && y < top + nlines * 16 * SCALE) {
			unsigned li = (y - top) / (16 * SCALE), gy = ((y - top) % (16 * SCALE)) / SCALE;
			unsigned len = (unsigned)strlen(lines[li]);
			unsigned left = (w - len * 8 * SCALE) / 2;   /* centred */
			for (unsigned i = 0; i < len; i++) {
				unsigned char ch = (unsigned char)lines[li][i];
				if (ch < 32 || ch > 126) ch = '?';
				uint8_t bits = font8x16[ch - 32][gy];
				for (unsigned gx = 0; gx < 8; gx++)
					if (bits & (0x80 >> gx))
						for (unsigned s = 0; s < SCALE; s++)
							memcpy(row + (left + (i * 8 + gx) * SCALE + s) * bytes, &fg, bytes);
			}
		}
		if (pwrite(fd, row, w * bytes, (off_t)y * line) != (ssize_t)(w * bytes)) {
			close(fd);
			return fail(1, "could not draw on the screen");
		}
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 3 && !strcmp(argv[1], "find")) {
		uint64_t off, size;
		int rc = find_fat(argv[2], &off, &size);
		if (rc) return rc;
		printf("FAT_OFFSET=%llu\nFAT_SIZE=%llu\n", (unsigned long long)off, (unsigned long long)size);
		return 0;
	}
	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "attach")) {
		dry_run = argc == 5 && !strcmp(argv[4], "--dry-run");
		if (argc == 5 && !dry_run) return fail(1, "unknown option");
		return attach(argv[2], argv[3]);
	}
	if (argc == 3 && !strcmp(argv[1], "detach")) {
		detach_all(argv[2]);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "show"))
		return show(argv[2]);
	fprintf(stderr, "usage: nz-fatroot find DEV | attach DEV NEWROOT [--dry-run] | detach NEWROOT | show TEXT\n");
	return 1;
}

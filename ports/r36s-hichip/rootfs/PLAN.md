# A real Linux system on the SD card (modern kernel) — PLAN

Written 2026-10-09. It is the "separate, explicit plan" that
my lab rules ask for before the modern kernel may ever write the boot card.
**I decided on 2026-10-10 that modern Linux may write the card, in the
Linux area only (section 12).** The PC-side card actions (survey, install,
restore, reset, verify of the area) exist since then in
`card-test/card_io.py` + `card-test/region_io.py`, with a guided session
`purelinux/session/sdroot-install.ps1`; they are tested on a fake card only.
Each step that touches the card still needs my OK.

## 0. In one paragraph

The 64 GB test card only uses its first ~16 GB (the stock FAT32 partition).
The other ~44 GiB have never been touched by any of our tools and belong to no
partition. We put a **"Linux area"** there, starting at exactly **16 GiB**:
a small NZEUS header, a 4 GiB **ext4 root file system** (`nzroot`, built with
Buildroot), a ~39.5 GiB **ext4 data area** (`nzdata`, mounted at `/data`) and a
backup header. The stock FAT32 partition and every byte before 16 GiB stay
exactly as they are — the stock system never even sees the area. The kernel
learns where the area is from one line on its command line (`blkdevparts=`).
The kernel still starts from its built-in RAM system (initramfs) with the USB
rescue shell; only if every check passes does it switch into the SD system. If
anything is wrong it simply stays in the RAM rescue shell, exactly like today.

## 1. What the card looks like today (facts, with sources)

| Fact | Value | Source |
|---|---|---|
| Card size | 63,864,569,856 B = 124,735,488 sectors = **exactly 60,906 MiB** | `card-test/card_io.py` `CARD_SIZE`, Windows `Get-Disk` |
| Sector 0 | partition table XOR `DEADBEEF` (Windows sees "empty") | `release/r36s_card.ps1` |
| Partitions | exactly **one**, FAT32 (type 0x0B/0x0C); table count must equal the FAT boot sector's total sectors | `r36s_card.ps1` `Test-R36sTable` ("more than one partition" is rejected) |
| FAT32 start | LBA 2048 = **1 MiB** (boot sector "MSDOS5.0", "NO NAME", "FAT32") | `display/STATUS.md` "SD READ-ONLY TEST PASSED", `tools/fix_fsinfo.py` |
| Bytes our tools ever read/wrote | first **16,357,785,600 B = 15,600 MiB** (LBA 31,948,800), called "the established FAT extent" | `card_io.py` `IMAGE_SIZE`, `diagnostics/uart-stock-card/RESULT.md`, `read_test_card.py` ("only the part we wrote") |
| Linux `df` of the FAT32 | 15,262,720 1K-blocks | `display/STATUS.md` "SD READ-ONLY MOUNT PASSED" |

So the FAT32 partition is **LBA 2048 up to at most LBA 31,948,800**, i.e. at most
31,946,752 sectors = **15,599 MiB = 16,356,737,024 B**, and the **first byte that
is guaranteed not to belong to it is 16,357,785,600** (15,600 MiB).

Honest gaps:
- The exact `TotSec32` value from the boot sector is not stored in the repo (the
  hexdump is only in `usb-command-20261009T133058.txt` on the PC). The region
  tool therefore never trusts a number from this document: it reads the real
  partition table and boot sector from the fresh snapshot and refuses if the
  FAT32 ends later than 15,600 MiB or closer than 64 MiB to the Linux area.
- `df`'s 15,262,720 KiB is the FAT's *data area as Linux counts clusters*, not
  the partition size. It is 694 MiB smaller than 15,599 MiB, more than FAT
  overhead explains (maybe the FAT tables cover fewer clusters than the
  partition). It does not matter for this plan (we never use `df` for
  offsets), but it is noted as unexplained.
- "Unused" means: outside the only partition, and never written by our tools
  (every install asserts `offset + count <= IMAGE_SIZE`). Whether the factory
  left zeros there is **unknown** — the survey step (section 4) reads it before
  anything is planned, and non-zero content there is a reason to stop and ask.

Free space after the FAT32: 63,864,569,856 − 16,357,785,600 =
**47,506,784,256 B (44.2 GiB)**.

## 2. Layout of the Linux area

All boundaries are multiples of 4 MiB (SD erase blocks are typically 4 MiB).

| # | Name | Start (byte) | Start | Size | Kernel | What |
|---|---|---|---|---|---|---|
| p1 | `nzstock` | 1,048,576 | 1 MiB | 15,599 MiB (= FAT32) | **read-only** | stock FAT32, unchanged |
| – | gap | 16,357,785,600 | 15,600 MiB | 784 MiB | none | never touched (guard) |
| p2 | `nzhdr` | 17,179,869,184 | **16 GiB** (LBA 33,554,432) | 4 MiB | **read-only** | NZEUS header (sector 0), rest zero |
| p3 | `nzroot` | 17,184,063,488 | 16,388 MiB | 4 GiB | rw after checks | ext4 root file system |
| p4 | `nzdata` | 21,479,030,784 | 20,484 MiB | 40,416 MiB (39.5 GiB) | rw after checks | ext4, mounted at `/data` |
| p5 | `nzhdr2` | 63,858,278,400 | 60,900 MiB | 4 MiB | **read-only** | backup header |
| – | card end | 63,862,472,704 | 60,904 MiB | 2 MiB | none | never touched (card end is not 4 MiB aligned) |

Why these numbers:
- **16 GiB start**: easy to recognise, 4 MiB aligned, and leaves a 784 MiB gap
  after the largest possible FAT32 end. The tool refuses if the gap would be
  below 64 MiB.
- **Header** (512 bytes, little-endian, CRC-32), identical copy at the end so
  a damaged first copy is detectable: magic `NZEUS-LINUX-AREA`, version, card
  size, FAT end, area start/end, the card **CID**, the first-MiB SHA-256 at
  creation, and the five parts (name, start, size, kind, read-only flag, file
  system UUID). Exact format: `region-tool/nz_region.py` (`make_header`).
  Tools and the boot check use the header to refuse to touch anything that
  is not exactly this area on exactly this card.
- **4 GiB root** is plenty for a Buildroot system (< 64 MB) plus packages
  added later; user files go to `/data` so the root can be rebuilt from the PC
  without losing them. (A second root slot for A/B updates would fit later by
  shrinking `nzdata`; not planned now.)
- **No swap partition**: the kernel already has zram; swapping to the card
  wears it and is slow at the current 19.8 MHz SD clock.

## 3. File system: ext4 (not f2fs)

- **ext4 is already in our kernel** (`CONFIG_EXT4_FS=y`, `JBD2=y`); f2fs is not
  (`# CONFIG_F2FS_FS is not set`), nor are MD/device-mapper.
- **Journaling** protects against the most likely accident on a handheld: the
  battery dying or a hard power-off mid-write. ext4's journal (default
  `data=ordered`) recovers on the next mount; `e2fsck` is mature and in
  Buildroot. f2fs is designed for raw flash, but SD cards have their own
  controller (FTL) that already does that job; f2fs's checker is younger and
  power-loss recovery has a weaker track record. On SD cards ext4 is the
  common, boring, safe choice.
- Options: `noatime` (fewer writes), no online `discard` (some cards are slow or
  buggy with it; `fstrim` by hand later if needed), default 5 s commit (small
  loss window on sudden power-off).
- Created on the PC by `mke2fs 1.47.0` (WSL Ubuntu 24.04) with its ext4
  defaults: `has_journal ext_attr resize_inode dir_index filetype extent 64bit
  flex_bg sparse_super large_file huge_file dir_nlink extra_isize
  metadata_csum`, 4 KiB blocks, `lazy_itable_init=0,lazy_journal_init=0,nodiscard`
  (no background zeroing on the console later; all metadata exists from the start).
  The console's e2fsprogs is 1.47.2, which knows all of these.
- `nzdata`: `-m 1` (1 % reserved) and `-i 65536` (fewer inodes, more room for big files).

## 4. Creating the area from the PC (region tool + card_io.py)

Planning tool: `region-tool/nz_region.py` (Python stdlib + e2fsprogs; WSL). It
has **no install action** and refuses device paths; it only plans. The card is
read and written only by `card-test/card_io.py` (`region-*` actions, profile
`linux-area`), whose rules are all in `card-test/region_io.py`. The self-test
(`python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py selftest`) runs that same
module on copies of a fake sparse 64 GB card; results in section 12 and
`region-tool/README.md`.

Card workflow (lab rule 2), mapped to the tools:

1. **Identify** (Windows): `card_io.py` checks disk 5, `Size` 63,864,569,856,
   USB, not system/boot (unchanged); the session script also checks the reader
   serial (my reader's) (as `card-install.ps1`). A USB card reader cannot show
   the CID, so the CID is checked on the console instead (section 6) and is
   stored in the header; on the PC the identity is size + reader + the
   first-MiB hash and FAT boot sector, compared at every step.
2. **Fresh snapshot** of the FAT side: the existing `card_io.py snapshot`
   (first 16,357,785,600 B → `before.img` + `snapshot.json`), profile
   `linux-area` → `rootfs/card-region` (junction to `<work folder>\card-region`).
3. **Fresh survey of the area**: `card_io.py region-survey` reads
   `[16 GiB, 60,904 MiB)` = 46,682,603,520 B (~20–40 min over USB), read-only.
   It keeps only the **non-zero 64 KiB blocks** (`region-before.blocks`, back
   to back) plus `region-survey.json` (their offsets and SHA-256, a SHA-256 of
   the whole area, first-MiB and FAT-boot-sector hashes). Not a sparse image:
   NTFS files are not sparse by default and WSL cannot see holes on `/mnt/d`.
   It refuses a folder that already has a survey or plan (surveys are always
   fresh) and stops above 1 GiB of non-zero data; any non-zero data at all
   makes the session script **stop and ask**.
4. **Candidate** (WSL, `region-tool/wsl-region.sh plan`): copies the root image
   (default the Debian build `~/nzeus-rootfs-debian/out/rootfs.ext4`) to
   `~/nzeus-region/<UTC time>/` and runs `nz_region.py build` there →
   `region-candidate.img` (sparse, the exact intended content of the whole
   area; `build` refuses folders under `/mnt/`), `build.json` (incl. the root
   image SHA-256), `nzroot-initramfs.conf`. The root image is grown (or shrunk)
   to exactly 4 GiB with `resize2fs`; `nzdata` is made with `-m 1 -i 65536`,
   label `nzdata`. Both file systems must pass `e2fsck -fn`; each header block
   must contain only the header.
5. **Sector plan**: `nz_region.py plan ... --out <card folder>` → `delta.json`
   in the `prepare_card.py` style (absolute offsets, 64 KiB blocks,
   `before`/`after` SHA-256, plus `first_mib_sha256`, `fat_boot_sector_sha256`,
   `snapshot_sha256`, `survey_json_sha256`, the layout and the `blkdevparts`
   line) and `region-after.blocks` (new content of each block). It **refuses**
   if: the snapshot does not match `snapshot.json`; sector 0 is not the
   scrambled single-FAT32 table; the survey is from another card state; the
   FAT32 reaches into the guard gap; any block lies outside
   `[16 GiB, 60,904 MiB)` or below 16,357,785,600; blocks overlap; the plan is
   bigger than **2 GiB** (raised from 512 MiB for a Debian root, reasons in
   `region-tool/README.md`); the candidate was built for another layout.
   `nz_region.py check <folder>` and, on Windows, `region_io.py check <folder>`
   re-check it.
6. **Install**: `card_io.py region-install`: volume locks as today; plan rules
   again (card_io trusts no number in the plan); survey and after-data files
   belong to the plan; first MiB + FAT boot sector unchanged; **preflight every
   block** against its `before` hash (stale survey or changed card → refused,
   nothing written); then write + fsync + read back each block; afterwards
   the identity again and **the whole stock part re-read and compared with the
   snapshot hash**.
7. **Way back**: the stock system never reads the area, so the stock card keeps
   working with or without it. To remove the area: `region-restore` (only the
   blocks the install touched, back to the surveyed content; refuses if Linux
   has written to them) or, after Linux has used the area, `region-reset`
   (whole area back to the survey, writing only blocks that differ). Section 12.

## 5. How Linux finds the area

Sector 0 is scrambled, so the kernel sees no partitions (`mmcblk0` only).

**Recommended: kernel command-line partitions.** Add `CONFIG_CMDLINE_PARTITION=y`
(today `# CONFIG_CMDLINE_PARTITION is not set`; everything else needed is
already `=y`: `EXT4_FS`, `JBD2`, `BLK_DEV_LOOP`, `MSDOS_PARTITION`,
`MMC_BLOCK` with `MMC_BLOCK_MINORS=8` ≥ 5 partitions). Then add to `bootargs`
in the board DTS (same place as `console=ttyS0,19200 ... rdinit=/init`):

```
blkdevparts=mmcblk0:15599M@1M(nzstock)ro,4M@16G(nzhdr)ro,4G@16388M(nzroot),40416M@20484M(nzdata),4M@60900M(nzhdr2)ro
```

(computed by `nz_region.py layout`; if the real FAT32 is shorter than 15,599 MiB
the tool prints the exact p1 size — a p1 slightly longer than the FAT is
harmless because it is read-only). The kernel parser (checked in the pinned
7.2 source, `block/partitions/cmdline.c`) runs before the MBR parser, marks
`ro` partitions read-only in the kernel (`BD_READ_ONLY`), and gives
`mmcblk0p1`..`p5` in this order. Caveat: it silently shortens or drops
partitions past the end of a smaller card, so the boot check compares every
partition's sysfs `start`/`size` with the header before use.

**Without a kernel change (for read-only tests now):** a loop device with
offset **and size limit** (`LOOP_CONFIGURE` with `lo_offset` = part start and
`lo_sizelimit` = part size, read-only flag), i.e. `sd/sd-loop-ro.c` plus a
`sizelimit` argument. Good for a first "mount `nzroot` read-only and look"
test with the current `gma-native-*` kernels; less good for booting (the
whole card would have to be writable underneath the loop).

dm-linear is not possible (`# CONFIG_MD is not set`).

## 6. Boot flow

Kernel and initramfs stay the RAM system we have; the rescue shell never
depends on the card.

```
kernel -> /init (initramfs, PID 1)
  mount devtmpfs/proc/sys/tmp/run; start /usb-rescue (USB shell on ttyGS0)   [as today]
  if FN held at power-on (EVIOCGKEY on r36s-keys) or no mmcblk0 within 10 s -> stay in RAM
  nz-region-check (new static helper, O_RDONLY only):
      header in p2 sector 0 and p5 sector 0: magic, CRC, identical
      /sys/block/mmcblk0/device/cid == header CID == NZ_CID (000000000000000000000000000000b7)
      /sys/block/mmcblk0/size == header card size
      p1..p5 sysfs start/size == header == nzroot-initramfs.conf
      p1, p2, p5 report ro=1 (kernel read-only)
    any mismatch -> stay in RAM, print why (UART, screen, USB)
  mount -t ext4 -o ro,noatime /dev/mmcblk0p3 /newroot
  /newroot/etc/nzeus-rootfs-release has NZEUS_ROOTFS_LAYOUT=1 and /newroot/sbin/init exists
    else umount, stay in RAM
  stop the usb-rescue shell loop (the USB gadget stays bound; ttyGS0 stays)
  mount --move /dev /proc /sys /run into /newroot
  exec switch_root /newroot /sbin/init
SD root (BusyBox init, files in buildroot/board/nzeus-r36s/overlay):
  S00nzmount  - missing kernel file systems, hostname
  S01nzroot   - root is still READ-ONLY: e2fsck -p on "/" (allowed while mounted ro),
                then remount rw,noatime only if e2fsck returned 0 or 1;
                otherwise stay read-only and say so (never forces a repair,
                never reboots by itself); refuses rw if p1/p2/p5 are not ro
  S03nzdata   - label must be "nzdata"; e2fsck -p; mount /data noatime,nodev,nosuid
  ttyGS0      - getty -n -l nz-autologin -> root shell on the USB cable
                (same USB identity, so diagnostics/rescue-command.py keeps working)
  shutdown    - rcK, swapoff, sync, umount -a -r (then nz-poweroff)
```

Why the file system check runs inside the SD root and not in the initramfs:
`e2fsck` refuses a mounted file system unless it is the read-only `/`, and
putting `e2fsck` (≈1 MB with libraries) into the RAM image would grow the
kernel file. The classic Unix order (mount `/` read-only → check → remount
read-write) gives the same protection.

New pieces needed for that build (not written yet, separate step):
`CONFIG_CMDLINE_PARTITION=y` + the `bootargs` line; initramfs BusyBox gets
`switch_root`, `kill`, `mountpoint`, `blockdev` (today it has 62 options and
none of these); a small static `nz-region-check` in C (same style as
`sd/sd-loop-ro.c`); `/init` gets the block above. The `/usb-rescue` kernel-name
guard must admit the new build name (the 2026-10-09 lesson).

## 7. Recovery

| Problem | What happens | Fix |
|---|---|---|
| SD root damaged / e2fsck unhappy | SD root stays read-only, login still works | log in, read the message; repair from RAM rescue |
| SD root will not mount, header wrong, wrong card | initramfs stays in the RAM rescue shell (today's behaviour) | nothing breaks; fix from the PC |
| Want to repair `nzroot` by hand | hold **FN** at power-on → RAM rescue; upload a static `e2fsck` with `rescue-upload.py`; `e2fsck -f /dev/mmcblk0p3` | or rebuild `nzroot` from the PC with the region tool (`/data` is separate) |
| New kernel does not boot | existing recovery profiles (unchanged) | e.g. `gma-native-keys-recovery` |
| Card back to daily stock use | stock never sees the area | rule 4 as today (original kernel, hash, stock reboot over ADB); optionally remove the area with the reversed plan |
| Sudden power loss | ext4 journal replays on next mount; e2fsck at next boot | nothing |

The RAM rescue shell never depends on the SD root: it is started before any
card access, and every failure path ends in it.

## 8. The Buildroot root file system

External tree: `buildroot/` (`external.desc` name `NZEUS_ROOTFS`,
`configs/nzeus_r36s_defconfig`, `board/nzeus-r36s/`). Build in WSL:
`wsl -d Ubuntu -- bash ports/r36s-hichip/rootfs/buildroot/build.sh` (clones Buildroot
2025.02.x pinned at `393f0196…`, checks that every defconfig line survived,
builds, copies only the manifest back). Buildroot builds **userspace only**
(`BR2_LINUX_KERNEL` off; we build the kernel ourselves).

Choices:
- **CPU**: `mipsel`, MIPS32r2, **hard float** (`-mfpxx`, legacy NaN — matches
  the Ubuntu cross compiler our kernel and tools use), `-mtune=74kc`
  (Buildroot has no 74Kc entry), `-Os`.
- **musl, not glibc**: the machine has 128 MiB addressable and Linux currently
  sees ~38.7 MiB (`MemTotal 39588 kB`). musl's C library is a few hundred KB
  versus several MB of glibc libraries and locale data, its programs use less
  RAM, and static linking (handy for rescue tools) works well. Everything in
  the package list builds with musl in Buildroot.
- **Kernel headers = our kernel** (7.2.0-rc5, `SiarheiVolkau/linux-hichip` at
  `da5f1a22…` as a GitHub archive tarball), declared as "6.12 or later", which
  is how Buildroot accepts headers newer than it knows.
- **Packages** (33 target packages incl. libraries, see
  `buildroot/manifest/PLANNED-packages.txt`): BusyBox 1.37 (Buildroot config +
  `busybox.fragment`: timeout, blockdev, nc, ntpd, pstree, taskset, …),
  e2fsprogs (e2fsck/tune2fs/resize2fs), dropbear (key login only), nano,
  htop, evtest, fbset, i2c-tools, wpa_supplicant (nl80211, WPA3 → OpenSSL,
  cli) + iw for later Wi-Fi, ca-certificates, **python3** (`.pyc` only, with
  ssl/zlib/readline). Python 3 instead of MicroPython because the goal is "full
  Linux tools"; MicroPython stays available as the static binary already on
  the FAT32 (`nzeus/bin`). vim is not included (BusyBox `vi` + nano); add
  `BR2_PACKAGE_VIM` if wanted (~3 MB).
- **System**: hostname `nzeus`, BusyBox init + mdev, root login without
  password **only** via the USB cable (getty autologin on `ttyGS0`), `/tmp` is
  an 8 MB tmpfs (lab rule: no big files in RAM), `/data` for files.
- **Images**: `rootfs.tar` and a 160 MB `rootfs.ext4` (label `nzroot`); the
  region tool grows the ext4 to 4 GiB. Target size goal < 64 MB; the real size
  comes from `nzeus-rootfs-manifest.txt` after the first WSL build.

**Cloud build result (2026-10-09): NOT built.** Buildroot was cloned and the
defconfig resolves completely (every line present in `.config`, package list
computed), but the cloud's egress policy blocked the source downloads
(`sources.buildroot.net`, `distfiles.ariadne.space`, `matt.ucc.asn.au`,
`kernel.org`, GNU mirrors, `musl.libc.org`, `python.org`, `busybox.net`, …;
even the GitHub archive URL for the kernel headers returned 403). So there is
no image size and no rootfs.tar hash yet. Expect roughly 1–2 h for the first
WSL build.

## 9. Testing order on the real console (each step needs my OK)

1. PC only: Buildroot build in WSL; `nz_region.py selftest --rootfs-ext4
   <real rootfs.ext4>`; boot the rootfs in QEMU (malta, mipsel) if wanted.
2. Card, read-only on the console first: survey → plan → install the area with
   the PC (FAT32 untouched, proven by snapshot hash). Boot the current kernel,
   attach `nzroot` with a read-only loop (offset + sizelimit), `e2fsck -n`,
   compare file hashes with `rootfs.tar`. **No card writes from Linux.**
3. First writes from Linux — the SD write path of `dw_mmc` has **never been
   exercised** on this SoC: write a test file in `/data` only, sync, unmount,
   power off, then verify from the PC that the FAT side and the headers are
   byte-identical and the file is right. Strongly suggested: do steps 3–4 on a
   **spare card** first (the plan's "one SD card" is the only good image).
4. `gma-native-sdroot` kernel (cmdline partitions + new `/init`): boot into the
   SD root while I watch; check FN escape and every failure path
   (unplugged/wrong header → RAM rescue).

## 10. Lab rule changes this plan would need (proposed wording, NOT applied)

Replace, in the modern-port status:
> Never mount the boot card read-write from modern Linux until a separate, explicit plan exists.

with:
> **SD writes from modern Linux are allowed ONLY inside the NZEUS Linux area**
> (`ports/r36s-hichip/rootfs/PLAN.md`): `mmcblk0p3` (`nzroot`) and `mmcblk0p4`
> (`nzdata`) as defined by the `blkdevparts=` line, and only after the boot
> check passed on that boot (header, CID, partition sizes). The stock FAT32
> (`p1`) and both header blocks (`p2`, `p5`) must stay read-only in the kernel:
> never `blockdev --setrw` them, never write `/dev/mmcblk0` itself, never mount
> `p1` read-write.

Add to cardinal rule 2:
> The Linux area is created or rewritten from the PC only with
> `ports/r36s-hichip/rootfs/region-tool/nz_region.py` plans (every block inside
> `[16 GiB, 60,904 MiB)`, checked by `nz_region.py check`), after a fresh
> snapshot of the FAT side **and** a fresh survey of the area, installed with
> preflight of every preimage and readback of every block.

Change cardinal rule 15 to:
> Over SSH/ADB/USB on a running console: never write raw devices
> (`/dev/mmcblk0`, `/dev/mmcblk0p1/p2/p5`, `/dev/mtd*`, `/dev/mem`) — no
> dd/mkfs/fdisk on them. `e2fsck` on `mmcblk0p3`/`p4` and normal file writes on
> the mounted `nzroot`/`/data` are allowed. ...(rest unchanged)

Add to cardinal rule 4:
> A broken SD root must never block the RAM rescue shell: every failure path
> of the SD-root boot ends in the initramfs rescue shell, and holding FN at
> power-on skips the SD root.

## 11. Open risks and untested parts

- **No hardware evidence for any of this.** Everything was tested on the PC
  only: the region tool's self-test (fake sparse card, simulated install),
  overlay scripts syntax-checked with BusyBox ash under qemu-mipsel and dash.
- **Buildroot image not built** (cloud network policy). Package sizes, Python
  start-up RAM and whether dropbear runs on the modern kernel (the old static
  `dropbearmulti` segfaulted) are unknown.
- **Linux SD writes are untested** on this SoC (only reads were verified).
- **Exact FAT32 size** not in the repo; the tool reads it at plan time. The
  `df` discrepancy (section 1) is unexplained.
- **Contents of the area unknown** until the survey; the card's erased state
  could be `0xFF` instead of zeros, which would make the plan write the whole
  area — the survey's 1 GiB cap and the 2 GiB plan limit refuse that, and we
  would stop and ask.
- **CID cannot be checked on the PC** (USB reader); the console-side boot
  check covers it, and the PC side relies on size, reader serial and hashes.
- The card-reader survey/install/restore/reset/verify actions exist
  (2026-10-10) but have run only against fake card files (and `card_io.py`'s
  glue only with Windows calls faked); `nz-region-check`, the new `/init`
  block and the kernel config change belong to the `gma-native-sdroot` work.
- `blkdevparts` silently adapts to other cards; only the boot check protects
  a different card (e.g. the BLACK console's) — it must never be skipped.

## 12. My decision 2026-10-10: modern Linux may write the card (Linux area only; FAT32 stays read-only by default)

I decided on 2026-10-10 that modern Linux may use the SD card read-write.
The goal is a real, writable Linux system in the unused ~44 GiB after the stock
FAT32 of the 64 GB test card (layout: section 2). What that means:

- **From the PC** the card is written only by `card-test/card_io.py`
  `region-install` / `region-restore` / `region-reset` (profile `linux-area`),
  and only inside `[16 GiB, 60,904 MiB)`. The bounds are fixed constants in
  `card-test/region_io.py` (card_io never trusts a number from the plan), and
  the whole stock part is re-read and hash-compared after every write action.
  Rule 2 applies as always: card identity, fresh backup (`snapshot`), fresh
  survey of the area, candidate + sector plan, readback of every block, way
  back ready (`region-restore` / `region-reset`).
- **From Linux** only `mmcblk0p3` (`nzroot`) and `mmcblk0p4` (`nzdata`) may be
  written, after the boot check (section 6). `p1` (stock FAT32), `p2` and `p5`
  (headers) stay read-only in the kernel (`ro` in `blkdevparts`).
  **The FAT32 stays read-only by default**: writing it from modern Linux would
  need its own plan.
- The wording in section 10 is the intended rule text; it is not
  applied by this tool change (I update the lab rules myself).
- Still true: start-up changes only while I watch (rule 6), no success
  claims without hardware evidence (rule 7). The `dw_mmc` write path has never
  run on this SoC; section 9 step 3 (first writes in `/data` only, then a PC
  check) is the first hardware test of it. A spare card first is still the
  safer choice.

### Install steps (console OFF, GREEN console's card in the USB reader)

Prerequisites: the Debian root image is built in WSL
(`~/nzeus-rootfs-debian/out/rootfs.ext4`, or pass `-RootfsExt4 <WSL path>`),
WSL has `e2fsprogs` and `python3`, D: has 20 GB free. Then:

```
powershell -ExecutionPolicy Bypass -File "ports\r36s-hichip\purelinux\session\sdroot-install.ps1"
```

| Step | What happens | Writes? |
|---|---|---|
| 0 | WSL: root image and tools exist; card = disk 5, reader serial (my reader's), 63,864,569,856 B, USB, not system/boot; I type YES | no |
| 1 | `card_io.py snapshot --profile linux-area` → `rootfs/card-region` (junction to `<work folder>\card-region`): `before.img` + `snapshot.json` (10–20 min, UAC Yes) | no |
| 2 | `card_io.py region-survey`: whole area read once → `region-survey.json` + `region-before.blocks` (20–40 min, UAC Yes). Any non-zero data → stop and ask Claude (I must type YES to go on) | no |
| 3 | WSL `region-tool/wsl-region.sh plan`: root image copied to `~/nzeus-region/<UTC time>/`, `nz_region.py build` (candidate on the WSL disk), `nz_region.py plan` → `delta.json` + `region-after.blocks` in the card folder, `nz_region.py check` | no |
| 4 | Windows: `region_io.py check` (the rules card_io uses), plan size shown, refuses > 2 GiB; I type YES | no |
| 5 | `card_io.py region-install` (UAC Yes): preflight of every `before` block, write + read back each block, identity again, whole stock part re-read = snapshot hash. Evidence: `rootfs/card-region/region-install.log` (`ALL_PREIMAGES_VERIFIED`, `VERIFIED_BLOCK` lines, `STOCK_EXTENT_UNCHANGED`, `INSTALL_COMPLETE`) | Linux area only |

Then, each only while I watch:
1. Kernel: `powershell -ExecutionPolicy Bypass -File "ports\r36s-hichip\purelinux\session\card-install.ps1" -CardProfile gma-native-sdroot`
   (installs over `gma-native-net`; needs `display/package-sdroot`; the usual
   snapshot/plan/readback of the FAT side).
2. Console ON: SD root, or the RAM rescue shell (COM9) if any check fails; FN
   held at power-on skips the SD root. Keep the UART log and photos.
3. After Linux has written to `/data`: console OFF, `sdroot-install.ps1 -Step
   verify` (read-only) must say `changed` for the area, and the stock part must
   be a recorded state: `SAME_AS_SNAPSHOT` (as at the area install) or, after
   the kernel install, "the card state right after the install of
   card-sdroot" (the script compares the stock hash with every kernel plan's
   `after_sha256`). That is the PC-side proof that Linux wrote only its area.

Kernel order on the card: `keys` (now) → `gma-native-pure` → (`avp-stub`
optional, a firmware file, not a kernel) → `gma-native-net` → `gma-native-sdroot`.
New profiles in `card_io.py`, `prepare_card.py` and `card-install.ps1`:
`gma-native-net` (over pure, package `display/package-net`) +
`gma-native-net-recovery` (back to pure), `gma-native-sdroot` (over net,
package `display/package-sdroot`) + `gma-native-sdroot-recovery` (back to net).
Both packages are made by the kernel work (`diagnostics/package-uart-boot.py`
still needs `--display-net` / `--display-sdroot` options for that).

### Recovery steps

| Situation | Do this (console OFF, card in the reader) |
|---|---|
| Install stopped half-way, or right after the install (Linux has not written yet) | `sdroot-install.ps1 -Step restore`: only the blocks the install touched get their surveyed content back (each must hold the installed or the surveyed content, else it refuses and writes nothing) |
| Linux has used the area and it should go | `sdroot-install.ps1 -Step reset`: reads the whole area, writes the surveyed content only to blocks that differ. **Erases the Linux system and `/data`.** Allowed only if the header blocks hold this plan's header or the surveyed content |
| Just look | `sdroot-install.ps1 -Step verify` (read-only): `surveyed` / `installed` / `changed`, plus the stock-part comparison |
| sdroot kernel does not boot | `card-install.ps1 -CardProfile gma-native-sdroot-recovery` (→ net), then if needed `gma-native-net-recovery` (→ pure), `gma-native-pure-recovery` (→ keys) |
| Card back to daily stock use | rule 4 as always (original kernel, hash, stock reboot over ADB). The area may stay: the stock system never reads it |
| Any log says `STOCK EXTENT ...` changed | stop and tell Claude; `rootfs/card-region/before.img` is the backup of the stock part |

Why `restore` touches only the install's blocks: every other block of the
area was not written by the install, so it still holds what the survey saw.
Writing zeros over all 44 GiB would take hours and wear the card for nothing.
Once Linux has written, ext4 may have used any block of `p3`/`p4`, so `reset`
reads everything and rewrites only what differs from the survey (with
readback).

### Test log (PC only, no hardware evidence)

2026-10-10, cloud, Python 3.13, e2fsprogs 1.47.0:
- `nz_region.py selftest` with the tiny test root: **70/70 PASS** (plan 147
  blocks, 9.2 MiB; ~10 min, of which most is reading the fake 44 GiB area).
- `nz_region.py selftest --rootfs-ext4 <5 GiB ext4 with ~400 MB of random data
  and 3,000 files>` (Debian-sized stand-in, bigger than `nzroot`, so it is
  shrunk to 4 GiB): **70/70 PASS**, plan **6,754 blocks, 422.1 MiB**.
- `card_io.py` itself (argument checks, profile folder, log, `region-*` glue)
  run on Linux with the Windows calls faked and `\\.\PhysicalDrive5` a link to
  a fake card file: **10/10** (survey, `region_io.py check` output, install,
  verify = `installed` + `SAME_AS_SNAPSHOT`, second install refused as stale,
  restore, reset = 0 blocks, three wrong profile/action combinations refused).
  This harness is not in the repo (it fakes Windows); it proves the glue only.
- `prepare_card.py` profiles `gma-native-net`, `-net-recovery`,
  `gma-native-sdroot`, `-sdroot-recovery` on fake 16 GB cards with fake
  packages: **5/5** (each plan writes the expected kernel over the expected
  one; sdroot over pure is refused).
- Not done: anything on the real card or the real Windows PC; the PowerShell
  scripts were not parse-checked in this session (the sandbox refused to start
  `pwsh`); the real Debian `rootfs.ext4` has not been through `build` yet.

## 13. Update 2026-10-10: Debian is now the main root file system

I decided today that the modern Linux may use the SD card read-write, with
the goal "a full true Linux PC". The root file system for `nzroot` is now
**Debian 12 bookworm (mipsel), minimal, sysvinit**: `debian/README.md`,
built in WSL by `debian/build-debian.sh` (not built yet: the cloud cannot reach
Debian's servers). Buildroot (section 8) stays as the small fallback and now
uses the same NZEUS scripts (`debian/apply-overlay.sh ... buildroot`).
The start-up behaviour in section 6 is unchanged: root mounted read-only by
the RAM system, `e2fsck -p`, read-write only on rc 0/1, `/data` checked the
same way, the same USB rescue identity and shell on ttyGS0.

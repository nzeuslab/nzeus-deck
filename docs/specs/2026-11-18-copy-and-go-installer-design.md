# Copy-and-go installer for NZEUS Deck: design

*2026-11-18, NZEUS LAB. Status: approved 2026-11-18.*

## Why

NZEUS Deck (Linux 7.2 + Debian 12) runs on my own R36S clones, but
installing it is an expert job: 16 GB card snapshots, WSL builds, admin tools
writing raw blocks into the card's hidden area, and settings tied to my
64 GB card (`nz_region.py` CARD_BYTES, my card's CID, a `blkdevparts=` line
for my layout). The goal is for **another owner of the same console to
install it in a few minutes, and undo it just as easily**, with no special
tools or skills. This is also public evidence of the lab's work
(see `docs/WRITEUP.md`).

## Who it's for

Owners of the **green R36S V2.7 clone, HiChip E3100, board HC16E3100V20**
with its stock SD card. Not Rockchip R36S units: they already have ArkOS and
similar systems.

## Goals
- Install = download a zip, run one installer on Windows (or follow 3 copy
  steps on Mac/Linux), put the card back, power on, and the NZEUS Deck home
  screen appears.
- Uninstall = run one uninstaller (or copy 2 files back) and the console boots
  its stock system again.
- Works on any card size and layout the stock card might have, not just
  my 64 GB card.
- Every change is a file change on the card's normal FAT32 partition, plus the
  factory's sector-0 scramble, which the installer undoes and redoes itself.

## Non-goals
- Keeping the stock game menu usable alongside NZEUS (in my words: "we don't care
  about games"). A user can still go back via uninstall.
- A one-click installer for Mac/Linux in v1 (manual steps only).
- Rockchip R36S, other HiChip consoles.
- Using the upper 128 MiB of RAM.
- Changing my own console setup (hidden Linux area, my kernel). It stays.

## Anti-goals (a failure even if it works)
- Anything that writes the NOR flash or can brick a console.
- Any change that can't be undone from a PC by copying files.
- Shipping the console maker's files (stock kernel, AVP firmware, device
  trees, games) or the maker's driver `driver_r36sx*.so`.
- Shipping my personal data: SSH keys, Wi-Fi, CID, settings, WhatsApp
  keys. The image must pass `privacy-scan-image.sh`.
- Publishing the kernel wrapper key (see Decisions).

## Constraints (facts from the R36S work)
- The factory bootloader (in NOR, never touched) loads `cubegm/advapi32.dll`
  from the FAT32 partition: a ChaCha20-wrapped legacy uImage + CRC32, no
  signature. Key = derived from the factory bootloader
  (`linux-port/boot-analysis/image_codec.py`). The same modern card booted on
  both of my consoles, so the key is very likely shared across units.
- It also reads `cubegm/Bubbles.scr` (SD device tree, only selected properties)
  and `cubegm/ApplicationFrame.dll` (AVP co-processor firmware). Rule: never
  replace `Bubbles.scr`. `ApplicationFrame.dll` may be swapped for the NZEUS
  stand-in made by `linux-port/purelinux/prepare_avpstub.py`; that stand-in is
  what frees the RAM (about 117 MiB usable instead of about 39).
- Sector 0 of stock cards is XOR-scrambled with `EF BE AD DE`
  (`nz_region.py unscramble`), so Linux's normal partition parser sees nothing.
- Kernel has VFAT, FAT, NLS 437/ISO8859-1, loop, ext4, devtmpfs
  (`linux-port/artifacts/kernel.config`).
- FAT32 files are limited to 4 GiB − 1 byte.
- Boot today: RAM initramfs (`linux-port/sdroot/init` + `nz-sdboot`) checks a
  header and the card CID, then mounts `mmcblk0p3` read-only and switch_roots;
  on ANY failure it stays in the RAM rescue system with a USB root shell.
- A generic Debian root already exists: built with `NZ_NO_AUTHORIZED_KEYS=1`,
  222 packages, privacy-clean, 4 GiB ext4 (`rootfs/debian/FRESH_CARD_2026-10-14.md`).

## Approaches considered
1. **Copy-and-go (chosen):** Linux's root file system is one image file on the
   FAT32 partition; install = copy files. Easiest, fully undoable from any
   PC. Trade-off: Linux writes to the FAT32 partition, so pulling the power at
   a bad moment could damage FAT32 files. I accepted that for public users.
2. **Windows installer writing the hidden area** (today's method,
   generalised): keeps FAT32 read-only, but needs admin rights, raw disk
   writes, card snapshots and per-card layout maths. Harder for users and the
   most work. Rejected.
3. **Full SD-card image for Etcher/Raspberry Pi Imager:** would have to
   contain the maker's boot files and device trees. Rejected (can't
   redistribute them, and it would wipe the card).

## Design

### What goes on the card (all on the FAT32 partition)
| Path | What |
|---|---|
| `cubegm/advapi32.dll` | NZEUS kernel, wrapped for the factory bootloader. The original is saved first. |
| `cubegm/ApplicationFrame.dll` | NZEUS AVP stand-in (frees RAM). The original is saved first. |
| `nzeus/nzroot.img` | Debian 12 root, ext4, sized to fit FAT32 (≤ 4 GiB − 1; target 3.5 GiB). Replaced by updates. |
| `nzeus/nzdata.img` | the user's data, ext4, mounted at `/data` like my p4 today (Deck settings, Wi-Fi, clock, logs). Made on first install, never replaced by updates. *My call* (builder Q2). |
| `nzeus/backup/` | the user's original `advapi32.dll` and `ApplicationFrame.dll` + `SHA256SUMS` |
| `nzeus/VERSION`, `nzeus/install-log.txt` | release version, what the installer did |

### New boot path in the initramfs ("fatroot")
1. Read sector 0 of `/dev/mmcblk0`. If it isn't a valid MBR, unscramble it
   (XOR `EF BE AD DE`) and check again.
2. Find the FAT32 partition. Make a loop device over it (offset + size limit
   on `/dev/mmcblk0`), mount vfat read-write, `noatime`.
3. Check that `/nzeus/nzroot.img` exists and is a sensible size. Loop-mount
   it as ext4 (journal replay is fine), then switch_root. The FAT32 mount
   moves into the new system at `/sdcard`, so users can reach their files.
4. Any failure: the existing behaviour (RAM rescue system + USB root shell),
   plus a plain message on screen saying what failed.
- **Two builds** (*my call*, builder Q3): the public kernel boots fatroot only and
  carries no `blkdevparts=` line; my private kernel and hidden-area path
  stay exactly as they are. The initramfs scripts are shared where possible.

### Installer (Windows): `install.bat` → `install.ps1` (built into Windows, no downloads)
1. Ask Windows for admin rights once. Find removable disks whose sector 0
   unscrambles to one FAT32 partition; if several, ask which one. Open the card
   (unscramble sector 0) and check it holds `cubegm/advapi32.dll` and
   `cubegm/Bubbles.scr`.
2. Refuse unless: FAT32; enough free space for `nzroot.img` + 100 MB (if not:
   "delete some games and run me again"); this isn't already an NZEUS card
   (if it is: offer update or uninstall).
3. Save the two original `cubegm` files to `nzeus/backup/` with SHA-256s, and
   read them back to check.
4. Copy the NZEUS files and verify every one by SHA-256 read-back.
5. Close the card (scramble sector 0 again, read back), then tell the user: eject, put the card in, power on; "if the screen stays black
   for 2 minutes, run uninstall.bat".
- `uninstall.bat` → `uninstall.ps1`: put both originals back (verified), delete
  `nzroot.img`, and ask whether to delete `nzdata.img` (default: keep, it holds
  their files).
- Update = install over an NZEUS card: replace kernel, AVP stand-in and
  `nzroot.img`; keep `nzdata.img` and `nzeus/backup/`.
- Mac/Linux v1: `INSTALL.md` with the same steps by hand.

### Release builder (my PC, WSL): `make-release.sh`
Builds the public kernel and initramfs, wraps the kernel with the key read
from **the builder's own NOR backup**, builds the generic Debian root
(`NZ_NO_AUTHORIZED_KEYS=1`) at release size, makes the AVP stand-in, runs the
privacy scan, and writes `NZEUS-Deck-<version>.zip` + `SHA256SUMS`. The zip is
uploaded by me as a GitHub Release, never committed to git.

## Decisions (and whose)
- Copy-and-go, FAT32 written by Linux for public users: **mine**.
- Windows one-click first, Mac/Linux manual: *my call* (most owners use Windows;
  PowerShell needs nothing installed).
- **The key isn't published, and the installer doesn't check it.** If a
  console's key were different, the bootloader's CRC check fails, the console
  doesn't boot, and uninstall fixes it. Nothing can be bricked because NOR is
  never written. *My call*: avoids publishing a key taken from the maker's
  firmware and needs no crypto in PowerShell.
- First release keeps the existing Deck apps. Claude/WhatsApp need the user's
  own PC "brain" and stay off until configured: *my call*.

## Tests
**PC (automated):**
- fatroot init: a test harness in the style of `sdroot/test-sdroot-init.py`
  with fake card images (scrambled and plain MBR, FAT32 from `mkfs.vfat`, a
  real small ext4 image). Cases: success; no FAT32; image missing; image
  corrupt/truncated; data image missing (made fresh) ; card read error/timeout.
  My private build: its existing tests still pass unchanged.
- installer/uninstaller: run against a fake drive folder. Cases: fresh install;
  second run = update; uninstall restores byte-identical originals; not an
  R36S card; not enough space; a copy interrupted half-way and run again.
- release: zip contents list, SHA256SUMS, privacy scan, no maker files, no
  NOR dump, no key.
- existing suites still pass (Deck 709, region 70, WhatsApp 14).

**Hardware (I watch, start-up changes only while I watch):**
- Make a "stranger's card": I write a backup image of a stock card to a spare
  SD card (identify by CID first). Check it boots stock on BLACK.
- From the release zip only, run `install.bat` as a stranger would. Boot on
  BLACK: home screen, sound, Wi-Fi with the dongle, power-off, restart, files
  in `/sdcard`. Pull the power once mid-use, boot again.
- `uninstall.bat`, and stock boots again. Then repeat the install on GREEN.

## Left to the builder
How the public kernel's command line is set, the exact
image size, loop device numbering, the on-screen message mechanism,
PowerShell details, and folder layout of the release scripts.

## Windows can't see a stock card (builder Q1, settled)
Stock cards keep sector 0 XOR-scrambled, so Windows shows an empty, unformatted
disk (`rootfs/PLAN.md` §1; NZEUS 0.9's `r36s_card.ps1` "Open"/"Close").
Decision (*my call*): the installer and uninstaller each run **open → copy →
close in one go**, after a single Windows admin prompt. The card always goes
back in the console scrambled, exactly as the factory expects, so it doesn't
matter whether the bootloader would accept an unscrambled card (untested).
The open/close code reuses `r36s_card.ps1`'s checks: exactly one FAT32
partition, its size matches the FAT boot sector, the card is removable, and
sector 0 is read back after every write. If anything fails half-way, the
installer closes the card again before stopping. Mac/Linux: a small Python
`r36s_card.py open|close` run with sudo, then the same manual copy steps.
This changes the goal "any PC can undo it" to "any PC can undo it with the
uninstaller (or the open/close script)".

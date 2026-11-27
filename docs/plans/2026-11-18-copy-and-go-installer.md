# Copy-and-go installer Implementation Plan

**Goal:** A stranger with a green R36S V2.7 (HiChip E3100) downloads one zip, double-clicks `install.bat`, and their console boots NZEUS Deck; `uninstall.bat` puts it back.

**Architecture:** A public kernel whose initramfs runs a new "fatroot" path. It finds the FAT32 partition behind the scrambled sector 0, loop-mounts `nzeus/nzroot.img` as `/` and `nzeus/nzdata.img` as `/data`, and exposes the FAT32 at `/sdcard`. A Windows PowerShell installer unscrambles sector 0, copies 4 files with read-back checks, and scrambles it again. `make-release.sh` (WSL, my PC) builds every piece and the zip.

**Tech Stack:** C (static mipsel, plus x86 for tests), BusyBox ash, Python 3 (tests, packaging), Windows PowerShell 5.1, WSL Ubuntu (mipsel-linux-gnu toolchain, qemu-mipsel, e2fsprogs, dosfstools, mtools).

**Spec:** `docs/specs/2026-11-18-copy-and-go-installer-design.md` (approved 2026-11-18)

## Global Constraints
- Repo: this repository (nzeus-public). The private repo `R36S` is **read-only** for this work: inputs may be read from it, nothing is written there.
- Never write NOR. Never write a real card from any test. Build scripts never flash, upload or reboot.
- Sector-0 scramble: every 4 bytes XOR `EF BE AD DE` (the same operation both ways).
- Card layout on FAT32: `cubegm/advapi32.dll`, `cubegm/ApplicationFrame.dll`, `nzeus/nzroot.img` (3,758,096,384 B = 3.5 GiB), `nzeus/nzdata.img` (1,073,741,824 B = 1 GiB), `nzeus/backup/{advapi32.dll,ApplicationFrame.dll,SHA256SUMS}`, `nzeus/VERSION`, `nzeus/install-log.txt`.
- Never replace `cubegm/Bubbles.scr`.
- Nothing from the maker in the zip or in git: stock kernel, factory AVP firmware, device trees from the card, NOR dumps, `driver_r36sx*.so`, the wrapper key.
- Private inputs come only through env vars: `NZ_NOR_BACKUP` (my NOR backup, private), `NZ_KERNEL_TREE`, `NZ_PORT_WORK`, `NZ_STOCK_CUBEGM`.
- Generic image: `NZ_NO_AUTHORIZED_KEYS=1`, and `privacy-scan-image.sh` must pass.
- Free space needed on the card: nzroot + nzdata + 100 MiB = 4,937,744,384 B on a fresh install.
- Commits are local only. I push and publish myself (R36S rule 9).
- Comments are short and plain (I'm learning).

## Review Focus
1. **Card pulled out or PC sleeps half-way through install:** the card must never stay unscrambled with half-copied files. A rerun must finish or roll back. Test: `test_install_resumes_after_interrupt` (Task 6).
2. **Two removable disks look like R36S cards, or the user's USB stick is picked by mistake:** the installer must only offer disks whose sector 0 unscrambles to exactly one FAT32 partition matching its FAT boot sector, and must show each disk's size and name. Test: `test_finds_only_r36s_cards` (Task 6).
3. **Card already full of games:** refuse with the exact space needed and leave the card unchanged (scrambled). Test: `test_refuses_low_space_and_recloses` (Task 6).
4. **Power pulled while NZEUS is running:** next boot must still reach the home screen. That means FAT32 is mounted `noatime` with no file creation at boot; `e2fsck -p` runs on both images before mounting. Test: `fatroot_dirty_ext4_is_repaired` (Task 3), plus the hardware power-pull (Task 9).
5. **Card already unscrambled** (opened earlier by NZEUS 0.9's tool, or a crashed install): `find` must accept a plain MBR, and the installer must detect the "open" state and close it at the end. Tests: `find_plain_mbr` (Task 2), `test_open_card_is_closed_at_end` (Task 6).

---

### Task 0: Make nzeus-public a local git repo
**Files:** Create: `.gitattributes` (`*.sh *.py *.c init text eol=lf`, `*.ps1 *.bat text eol=crlf`)
- [ ] Step 1: `git init -b main` in nzeus-public. Check `.gitignore` already excludes `reports/gui/`, `apps/dropbear/`, `*.img`, `*.bin`.
- [ ] Step 2: `git add -A && git status --short | grep -E '\.(img|bin|ko|so)$'`. Expected: empty.
- [ ] Step 3: Commit: "Public NZEUS Deck source (modern Linux), before installer work".

### Task 1: `tools/r36s_card.py`: scramble, inspect, open and close
**Files:** Create `tools/r36s_card.py`, `tools/tests/test_r36s_card.py`
**Interfaces (Produces):**
- `scramble(sector: bytes) -> bytes` (512 B in, 512 B out; involution)
- `inspect(sector0: bytes, fat_boot: Callable[[int], bytes]) -> dict`: returns `{'state': 'closed'|'open', 'start_lba': int, 'sectors': int, 'type': int}`. Raises `NotR36sCard(reason: str)` unless exactly one partition of type 0x0B/0x0C and its count equals the FAT boot sector's total sectors (BPB offset 0x20) with `FAT32` at 0x52.
- CLI: `r36s_card.py status|open|close DEVICE_OR_IMAGE`. open/close write only sector 0, then read back and compare. Exit 0 = done or already in that state; 2 = not an R36S card.
- [ ] Step 1: Tests with a fixture builder `make_fake_card(path, size_mib=64, scrambled=True)` (`mkfs.vfat -F 32` on a partition at LBA 2048 via `mformat`/`dd`): `test_scramble_roundtrip`, `test_inspect_closed`, `test_inspect_open`, `test_rejects_two_partitions`, `test_rejects_size_mismatch`, `test_open_then_close_is_byte_identical` (full-image SHA-256 equal), `test_open_twice_is_noop`.
- [ ] Step 2: Run `python3 tools/tests/test_r36s_card.py` in WSL. Expected: fails (module missing).
- [ ] Step 3: Implement. Port the rules from `R36S/backups/old-release-names-2026-10-07/NZEUS-0.9/r36s_card.ps1` (`Switch-Scramble`, `Test-R36sTable`) and `nz_region.unscramble`.
- [ ] Step 4: Run. Expected: 7/7 PASS.
- [ ] Step 5: Commit "r36s_card.py: open/close a stock card's scrambled sector 0".

### Task 2: `nz-fatroot` C helper
**Files:** Create `ports/r36s-hichip/fatroot/nz-fatroot.c`, `ports/r36s-hichip/fatroot/test-nz-fatroot.py`
**Interfaces (Produces):**
- `nz-fatroot find DEV`: prints `FAT_OFFSET=<bytes>` and `FAT_SIZE=<bytes>` (shell-sourceable), exit 0. Exit 3 = no R36S FAT32 (same rules as Task 1 `inspect`). Opens DEV read-only.
- `nz-fatroot attach DEV NEWROOT`: (1) loop over the FAT32 (offset + sizelimit, `LOOP_CONFIGURE`) → mount vfat rw `noatime,flush` at `/fat`; (2) check `nzeus/nzroot.img` is 3,758,096,384 B and `nzeus/nzdata.img` exists; (3) run `/bin/e2fsck -p` on each image through its loop; (4) mount root ext4 `ro` at NEWROOT and data ext4 `rw,noatime` at `/data0`. Exit codes: 0 ok, 3 no FAT, 4 image missing/wrong size, 5 fsck failed, 6 mount failed. Each failure prints one plain line starting `fatroot:`. On failure it undoes what it did.
- `NZ_ROOT` env prefix for test trees (like `nz-sdboot`'s `P()`), and `--dry-run` to print the syscalls instead of making them.
- [ ] Step 1: Tests (qemu-mipsel on the MIPS build, native on the x86 build): `find_scrambled`, `find_plain_mbr`, `find_rejects_usb_stick` (a FAT32 with 2 partitions), `find_unreadable_dev` (FIFO, must time out ≤ 10 s like nz-sdboot), `attach_dry_run_order` (exact syscall list), `attach_missing_root_image` (exit 4), `attach_wrong_root_size` (exit 4). With `sudo -n true` available: `attach_real_loops` and `fatroot_dirty_ext4_is_repaired` (x86 build, real loop devices, an ext4 with a forced dirty flag). Otherwise print `SKIP (no sudo)`.
- [ ] Step 2: Run. Expected: fail (no binary).
- [ ] Step 3: Implement. Static mipsel build flags exactly as `sdroot/build-initramfs.sh` uses for `nz-sdboot`.
- [ ] Step 4: Run. Expected: all PASS (sudo cases PASS or SKIP).
- [ ] Step 5: Commit.

### Task 3: fatroot `/init` and initramfs
**Files:** Create `ports/r36s-hichip/fatroot/init`, `ports/r36s-hichip/fatroot/build-initramfs.sh`, `ports/r36s-hichip/fatroot/initramfs.list.in`, `ports/r36s-hichip/fatroot/test-fatroot-init.py`. Modify `ports/r36s-hichip/sdroot/busybox.config` + `busybox-applets.txt` only if `e2fsck` must come from outside BusyBox (it does: ship a static `e2fsck` from e2fsprogs built for mipsel).
**Interfaces:** Consumes `nz-fatroot` (Task 2). Produces `fatroot/build-initramfs.sh WORK_DIR`, which writes `WORK_DIR/initramfs.list` for `CONFIG_INITRAMFS_SOURCE`.
- init flow: mount devtmpfs/proc/sys/tmpfs → wait up to 10 s for `/dev/mmcblk0` → `nz-fatroot attach /dev/mmcblk0 /newroot` → `mount --move /fat /newroot/sdcard`, `mount --move /data0 /newroot/data`, move /dev /proc /sys /run → `exec switch_root /newroot /sbin/init`. On any failure: print the `fatroot:` line to the console **and to `/dev/tty1`** (fbcon), then run the unchanged RAM rescue section copied from `sdroot/init`.
- [ ] Step 1: Tests in the style of `sdroot/test-sdroot-init.py` (stubs for mount/switch_root): `init_success_moves_mounts` (order: /fat→/newroot/sdcard, /data0→/newroot/data, then dev/proc/sys/run, then switch_root), `init_no_card_rescue`, `init_attach_fails_rescue_and_message` (message on tty1 stub), `init_rescue_section_identical` (the rescue block is byte-identical to sdroot/init's).
- [ ] Step 2: Run. Expected: fail.
- [ ] Step 3: Implement init + build script (static e2fsck: e2fsprogs 1.47.x source with sha256 pinned in `upstream/`).
- [ ] Step 4: Run. Expected: PASS. Also `bash ports/r36s-hichip/sdroot/test-all.sh` still passes (sdroot untouched).
- [ ] Step 5: Commit.

### Task 4: Public Debian image variant
**Files:** Modify `ports/r36s-hichip/rootfs/debian/build-debian.sh`, `make-image.sh`, `test-debian-pc.sh`. Create `ports/r36s-hichip/rootfs/debian/overlay-public/` (files that differ from `overlay/` only).
**Interfaces (Produces):** `build-debian.sh --public-fatroot --reuse` → `out/nzroot.img` (3,758,096,384 B, ext4 label `nzroot`) and `out/nzdata.img` (1,073,741,824 B, ext4 label `nzdata`, empty except `/nzeus/` with the Deck's default layout).
- overlay-public: `etc/fstab` without the p4 line; `/sdcard` mount point; `nzeus-datamount` and `nzeus-rootcheck` accept `/data` already mounted by the initramfs, and accept a writable FAT32 at `/sdcard` (this overrides `NZ_NEVER_WRITE` for the public build only); no `blkdevparts` or CID anywhere.
- [ ] Step 1: Add to `test-debian-pc.sh`: `public_image_sizes`, `public_fstab_has_no_p4`, `public_no_cid_no_blkdevparts` (grep the image for `blkdevparts=` and 32-hex CIDs → none), `public_privacy_scan_clean`, `public_no_authorized_keys`, `public_claude_whatsapp_unconfigured` (no PC address, no keys in the Deck settings).
- [ ] Step 2: Run. Expected: the new cases fail.
- [ ] Step 3: Implement.
- [ ] Step 4: Run. Expected: all PASS, and the existing 278 Debian checks unchanged.
- [ ] Step 5: Commit.

### Task 5: Public kernel + AVP stand-in, packaged
**Files:** Create `ports/r36s-hichip/kernel-public/build.sh`, `board.dts`, `package.py`, `test-package.py`. Copy (no edits) `R36S/linux-port/diagnostics/apt/fpudiag/{0001-nzeus-fpudiag.patch,package-clk900/entry.S,entry.ld}` and `R36S/linux-port/purelinux/make-avp-stub.py`.
**Interfaces:**
- Consumes: `fatroot/build-initramfs.sh` (Task 3); env `NZ_KERNEL_TREE`, `NZ_PORT_WORK`, `NZ_NOR_BACKUP`.
- Produces: `package.py --nor $NZ_NOR_BACKUP --out DIR` → `DIR/advapi32.dll`, `DIR/ApplicationFrame.dll`, `DIR/verification.json`.
- `board.dts` = `package-clk900/board.dts` with `blkdevparts=…` removed from `/chosen/bootargs`. Everything else is identical (asserted by the test).
- Kernel config = the daily kernel's `fpudiag/build/kernel.config`. The only allowed differences are `CONFIG_INITRAMFS_SOURCE` and `CONFIG_LOCALVERSION="-nzeus-public"`.
- [ ] Step 1: `test-package.py`: `dts_differs_only_by_blkdevparts`, `config_differs_only_by_initramfs_and_localversion`, `kernel_wrapper_decodes_and_crc_ok`, `avp_stub_wrapper_decodes_and_crc_ok`, `no_key_bytes_in_outputs` (the 32-byte key never appears in any output file or log), `bootargs_has_no_blkdevparts`.
- [ ] Step 2: Run. Expected: fail.
- [ ] Step 3: Implement.
- [ ] Step 4: Run. Expected: PASS.
- [ ] Step 5: Commit.

### Task 6: Windows installer and uninstaller
**Files:** Create `installer/NzeusCard.psm1`, `installer/install.ps1`, `installer/uninstall.ps1`, `install.bat`, `uninstall.bat`, `installer/tests/test-installer.ps1`.
**Interfaces (Produces, in `NzeusCard.psm1`):**
- `Get-R36sCards` → objects `{Number, FriendlyName, SizeBytes, State}` (removable disks only; same rules as Task 1).
- `Open-R36sCard -Number N` / `Close-R36sCard -Number N`: raw sector-0 write through `\\.\PhysicalDriveN`, read back, then `Update-Disk`. Open also makes sure the volume has a drive letter and returns it. Close flushes (`Write-VolumeCache`) and removes the letter first. Both are no-ops if the card is already in that state.
- `Install-NzeusFiles -Root <drive or folder> -Release <folder>`: backup, copy, verify, write `nzeus/install-log.txt`. Copies go to `*.part` first and are renamed after the SHA-256 check, so an interrupted run never leaves a half file under the real name.
- `Uninstall-NzeusFiles -Root -KeepData:$true`
- Test seam: every function takes `-DiskImage <path>` (sector 0 from an image file) and `-Root <folder>` (files in a plain folder), so tests need no admin and no card.
- `install.bat`: re-launches `install.ps1` elevated (`Start-Process powershell -Verb RunAs -ArgumentList '-ExecutionPolicy Bypass -File …'`), then pauses so the user can read the result.
- User-facing text (exact): success = "Done. Eject the card, put it in the console and switch it on. NZEUS starts in about 30 seconds. If the screen stays black for 2 minutes, run uninstall.bat." Low space = "Not enough free space: NZEUS needs {X} GB, the card has {Y} GB free. Delete some games from the card and run install.bat again."
- [ ] Step 1: Tests (plain PowerShell asserts, no Pester): `test_finds_only_r36s_cards`, `test_fresh_install_layout` (exact file list + SHA-256s), `test_backup_holds_originals`, `test_second_run_updates_keeps_data` (nzdata untouched, nzroot replaced), `test_uninstall_restores_byte_identical`, `test_uninstall_keeps_data_by_default`, `test_not_r36s_card_refused`, `test_refuses_low_space_and_recloses`, `test_install_resumes_after_interrupt` (kill after the 2nd file; rerun completes; no `*.part` left), `test_open_card_is_closed_at_end`, `test_bubbles_never_touched`.
- [ ] Step 2: Run `powershell -ExecutionPolicy Bypass -File installer\tests\test-installer.ps1`. Expected: fail.
- [ ] Step 3: Implement.
- [ ] Step 4: Run. Expected: 11/11 PASS.
- [ ] Step 5: Commit.

### Task 7: Release maker and release check
**Files:** Create `make-release.sh`, `tools/check-release.py`, `INSTALL.md`, `release-files/README.txt`, `release-files/SOURCE.txt` (GPL: repo URL, commit, linux-hichip commit, BusyBox/e2fsprogs/Dropbear versions + download URLs and sha256).
**Interfaces:** Consumes Tasks 1–6. Produces `dist/NZEUS-Deck-<version>.zip` + `dist/SHA256SUMS`. Version = `git describe --always --dirty`; refuses to build when dirty.
- Zip layout: `install.bat`, `uninstall.bat`, `installer/*`, `r36s_card.py`, `INSTALL.md`, `README.txt`, `SOURCE.txt`, `LICENSE`, `card/cubegm/{advapi32.dll,ApplicationFrame.dll}`, `card/nzeus/{nzroot.img,nzdata.img,VERSION}`.
- [ ] Step 1: `check-release.py ZIP` checks: `exact_file_list`, `sha256sums_match`, `images_privacy_scan_clean` (runs `privacy-scan-image.sh` on both images), `no_maker_files` (no zip member's SHA-256 equals any file in `$NZ_STOCK_CUBEGM`, a folder holding the stock card's original `cubegm/` files from my backup), `no_nor_dump_or_key`, `zip_size_under_250MB`.
- [ ] Step 2: Run on a deliberately bad zip (`--self-test` builds one with a fake key and an extra file). Expected: both caught.
- [ ] Step 3: Implement `make-release.sh` (calls Tasks 3–5 builds, copies the installer, zips with `-9`). Write `INSTALL.md` (Windows: 4 steps; Mac/Linux: `sudo python3 r36s_card.py open /dev/diskN`, copy `card/*`, `close`).
- [ ] Step 4: Run `bash make-release.sh && python3 tools/check-release.py dist/NZEUS-Deck-*.zip`. Expected: all PASS.
- [ ] Step 5: Commit (scripts and docs only; `dist/` is gitignored).

### Task 8: Full PC test run
- [ ] Step 1: Run all suites. Expected: Task 1 7/7, Task 2 all, Task 3 all, Task 4 Debian incl. new, Task 5 6/6, Task 6 11/11, Task 7 all, Deck 709/0, region selftest 70/70, WhatsApp 14 OK, sdroot `test-all.sh` PASS.
- [ ] Step 2: End-to-end on a fake card: `make_fake_card` 16 GiB sparse, scrambled, with fake `cubegm/{advapi32.dll,Bubbles.scr,ApplicationFrame.dll}` → run `install.ps1 -DiskImage` (via the folder seam for files) → run x86 `nz-fatroot attach` on the result with sudo → `/newroot/etc/os-release` says Debian 12 → uninstall → whole image SHA-256 equals the start.
- [ ] Step 3: Write `docs/TEST-REPORT-2026-11.md` with every count. Commit.

### Task 9: Hardware test (I'm present, every boot watched)
**Files:** the checklist, filled in as it runs (kept as a private lab log; the public summary is `docs/TEST-REPORT-2026-11.md`).
- [ ] Step 1: I insert a spare microSD (≥ 16 GB). Identify it by CID and size (never by disk number alone). Write a backup image of a stock card to it, read it back, SHA-256 compare.
- [ ] Step 2: The card in BLACK → stock system boots (photo).
- [ ] Step 3: Extract the release zip into a new folder → double-click `install.bat` → follow only its text.
- [ ] Step 4: Card in BLACK, power on → home screen within 2 minutes (photo); speaker plays; Wi-Fi dongle joins; a file copied to `/sdcard` shows on the PC later; power-off; restart.
- [ ] Step 5: Pull the power while the Deck is running → boots to the home screen again.
- [ ] Step 6: `uninstall.bat` → stock boots again (photo).
- [ ] Step 7: Repeat 3–4 on GREEN.
- [ ] Step 8: Record the results in the lab log and summarise them in TEST-REPORT-2026-11.md. Commit.

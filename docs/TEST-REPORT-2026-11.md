# Test report: NZEUS Deck copy-and-go release (November 2026)

## On real consoles

- **R36S V2.7 (HiChip E3100):** a stock SD card was set up with `install.bat`
  and the console starts to the NZEUS home screen.
- Sound, Wi-Fi, Bluetooth and power-off are the same Debian system and NZEUS
  Deck app as the earlier SD-card build, where they were tested on these consoles.
- Install, update, uninstall and recovery after a power cut (both images are
  checked with `e2fsck` at every start; a damaged data image still starts) are
  covered by the PC tests below.

## Problems found on real hardware, and fixed

1. **Stock cards can have start-up code in sector 0** and the FAT32 at sector 32.
   The card check now accepts that.
2. **Stock cards come almost full of games**, and Windows can't show a closed
   card, so "delete some games" was impossible for a user. `install.bat` now
   offers to delete the games (it asks first, and saves the console's two
   start-up files before anything is deleted).
3. **Debian froze right after the start-up RAM disk handed over.** The loop
   driver copies file data with the CPU and doesn't flush the cache afterwards;
   the E3100's MIPS 74K has an aliasing data cache, so programs run from
   `nzroot.img` got stale bytes (files still read fine). All three loop devices
   now use direct I/O (the data comes by DMA). Before handing over, the start-up
   RAM disk runs a program from `nzroot.img`; if that fails, the console stays
   in the rescue system with a message on screen instead of freezing.
4. **Windows sometimes refuses the write to sector 0** right after the card's
   layout changed. Each write is retried, and the card is read back to decide.

Also new: `nzeus/boot-log.txt` on the card (the start-up log and the first
3 minutes), so a start that goes wrong can be looked at on a PC.

## PC test suites

All run on a PC (WSL Ubuntu + Windows PowerShell 5.1) for this release.

| Suite | Command | Result |
|---|---|---|
| Card open/close tool | `python3 tools/tests/test_r36s_card.py` | 10/10 |
| Boot helper (MIPS under qemu + real loop devices) | `python3 ports/r36s-hichip/fatroot/test-nz-fatroot.py` | 16/16 |
| Boot script (MIPS BusyBox under qemu) | `python3 ports/r36s-hichip/fatroot/test-fatroot-init.py --busybox …` | 13/13 |
| Public Debian images | `sudo bash ports/r36s-hichip/rootfs/debian/test-public-image.sh …` | 9/9 |
| Public kernel + AVP stand-in | `python3 ports/r36s-hichip/kernel-public/test-package.py …` | 7/7 |
| Release zip check (self-test + real zip) | `python3 tools/check-release.py …` | RELEASE OK (6/6) |
| Windows installer (fake card images + folders) | `installer/tests/test-installer.ps1` | 34/34 |
| Windows installer, full size (real 4.5 GB release files) | `installer/tests/test-installer-fullsize.ps1 ZIP` | 4/4 |
| End-to-end: real release on a 6 GiB fake card, install → boot helper mounts it → uninstall → card back exactly | `python3 tools/tests/test_e2e.py ZIP` | 4/4 |
| NZEUS Deck (PC build) | `bash build_nzeus_gui.sh pc` | all passed |
| SD region tool self-test | `nz_region.py selftest` | 70/70 |
| WhatsApp transport | `test_whatsapp_transport.py` | 14/14 |

## Known gaps

- `ports/r36s-hichip/sdroot/test-all.sh` and 44 checks of `test-debian-pc.sh` need
  files that are not in this repository (boot BusyBox, rescue tools).
- No emoji pictures in the release yet.
- 5 helper programs come pre-built (their sha256 is pinned in the repo).
- A power cut while the console updates an image file's date rewrites that
  file's FAT32 directory entry (review item, still open).

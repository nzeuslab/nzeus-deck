# Installing NZEUS Deck

**For the R36S V2.7 clone with the HiChip E3100 chip only** (board
HC16E3100V20, any colour). Most R36S consoles use a Rockchip chip: this is not for them.
Tested on real consoles.

What happens: NZEUS is copied onto your console's SD card next to your games.
The console's own system isn't deleted. Three of its start-up files (including
the power-on picture) are saved first, and `uninstall.bat` puts them back. Nothing inside the console itself
(its flash chip) is ever changed.

You need: the console's SD card, a card reader, a Windows PC, and about
5 GB free on the card. Stock cards usually come full: if yours is, `install.bat`
offers to delete all the games to make room (it asks first; deleted games don't come back).

## Windows

1. Unzip the download into a folder.
2. Take the SD card out of the console and put it in the card reader.
3. Double-click **install.bat**. Windows asks once for administrator rights
   (the card's first sector has to be written). Follow the text: type Y.
   Copying takes a few minutes.
4. Put the card back in the console and switch it on. NZEUS starts in about
   30 seconds.

### On a new, empty SD card

Want to keep the console's own card exactly as it is? Put NZEUS on a second card
instead (8 GB or bigger; everything on it is erased):

1. Double-click **install-new-card.bat**.
2. Step 1: put the console's **original** card in the card reader. One small file
   is copied off it (`cubegm/Bubbles.scr`, the console's own board file, which
   NZEUS can't include). That card is only read, never changed.
3. Step 2: put the **new** card in, type ERASE. The card is set up the way the
   console's start-up expects, then NZEUS is copied and checked.
4. Put the new card in the console and switch it on.

Keep the original card: it still has the console's own system. Later releases
update the new card with install.bat as usual; uninstall.bat has nothing to put
back on it.

If the screen shows a message starting "NZEUS could not start", do what it
says. If the screen stays black for 2 minutes, run **uninstall.bat**: your
console goes back to its own system.

**Reporting a problem:** card in the PC, double-click **collect-logs.bat**. It copies
NZEUS's logs off the card into a new `nzeus-logs-...` folder next to it (nothing on the
card changes). Attach those files to your bug report.

**Going back:** card in the PC, double-click **uninstall.bat**. Your NZEUS files
(`nzeus/nzdata.img`) are kept unless you type DELETE.

**A newer NZEUS:** run the new release's install.bat on the same card. The
system is replaced; your NZEUS files are kept.

## Mac / Linux (by hand)

The card's first sector is scrambled, so the PC sees an empty disk. Unscramble
it, copy, then scramble it again. `r36s_card.py` checks that the disk really is
an R36S card before it writes anything.

```bash
diskutil list                                  # Mac: find the card, e.g. /dev/disk4 (Linux: lsblk -> /dev/sdX)
sudo python3 more/r36s_card.py open /dev/disk4      # Mac may need: diskutil unmountDisk /dev/disk4 first
# the card now mounts as a normal drive (e.g. /Volumes/NO NAME). Then:
C="/Volumes/NO NAME"
# FIRST INSTALL ONLY: save the console's own three files. If nzeus/backup already
# exists, skip these 3 lines - never overwrite the backup, it can't be replaced.
mkdir "$C/nzeus" "$C/nzeus/backup"
cp -n "$C/cubegm/advapi32.dll" "$C/cubegm/ApplicationFrame.dll" "$C/cubegm/xgame-logo.bmp" "$C/nzeus/backup/"
(cd "$C/nzeus/backup" && shasum -a 256 advapi32.dll ApplicationFrame.dll xgame-logo.bmp > SHA256SUMS)   # Linux: sha256sum
cp card/cubegm/* "$C/cubegm/"
cp card/nzeus/nzroot.img card/nzeus/VERSION "$C/nzeus/"
cp -n card/nzeus/nzdata.img "$C/nzeus/"          # -n: an update keeps your files
diskutil unmountDisk /dev/disk4                    # NOT eject: the card must stay attached. Linux: umount
sudo python3 more/r36s_card.py close /dev/disk4
```

Then check before you close (optional): `shasum -a 256 -c SHA256SUMS` in the
release folder compares your downloaded files.

To go back: `open`, copy the files from `nzeus/backup/` back into `cubegm/`, `close`.

## What's on the card afterwards

| File | What |
|---|---|
| `cubegm/advapi32.dll` | the NZEUS kernel (the stock one is in `nzeus/backup/`) |
| `cubegm/ApplicationFrame.dll` | a tiny stand-in for the console's second processor, so Linux gets the memory |
| `cubegm/xgame-logo.bmp` | the NZEUS picture the console shows at power-on (the stock one is in `nzeus/backup/`) |
| `nzeus/nzroot.img` | the NZEUS system (Debian 12) |
| `nzeus/nzdata.img` | your NZEUS files and settings |
| `nzeus/install-log.txt` | what the installer did |

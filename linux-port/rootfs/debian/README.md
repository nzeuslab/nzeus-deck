# Debian on the console's SD card (the "nzroot" root file system)

For a generic fresh card, set `NZ_NO_AUTHORIZED_KEYS=1` when building. This
removes any owner authorized key inherited from a reused build root and leaves
SSH disabled until that card's owner configures their public key. The overlay
also removes the exact obsolete `nzeus-claude-access.sh` stub on reuse. It never
clears a live console's settings or `/data`. Test this behavior with
`python3 linux-port/rootfs/debian/tests/test_image_reuse.py` on Linux/WSL.

**Current status (2026-10-11): [modern Linux status](../../STATUS.md).** Debian
12 has since been built and booted on hardware. The following October 9 status
describes the original preparation stage; package-manager crashes remain open.

Continuation PC checks: **268 passed, 0 failed**, including helper modes/hashes
and refusal of a missing required helper. The fake-card region recovery suite
also passed **70/70**. This run used the boot BusyBox; the optional full-BusyBox
behaviour round and non-WSL refusal were skipped. The builder now records the
source commit, tracked diff and helper manifest, and refuses a tracked-source
change during a build. A complete new Debian image has not yet been built with
these changes. See [continuation evidence](../../../reports/continuation/README.md).

Status 2026-10-09: **scripts written and PC-tested with fakes only. Debian has
NOT been built yet** (the cloud machine cannot reach the Debian servers) and
**nothing has run on the console.** The first real build happens on my PC.

## What this is, in plain words

I decided that the modern Linux may use the SD card read-write, to make
the console "a full true Linux PC". The plan (`../PLAN.md`) puts a Linux area
after the stock FAT32: `p3 nzroot` (4 GiB, the system) and `p4 nzdata` (`/data`,
your files). The kernel starts its small RAM system first; if all checks pass it
switches into `nzroot`.

This folder makes what goes into `nzroot`: **Debian 12 "bookworm"** for the
console's processor (`mipsel` = MIPS, little-endian; bookworm is the last Debian
release with mipsel, supported until about mid-2028), kept small:

- **sysvinit, not systemd** - systemd plus udev and dbus would use several MB of
  the ~39 MB of RAM. These three are blocked from ever being installed.
- the tools from `packages.txt`: bash, apt, nano, less, htop, ps/top, ip, ping,
  curl, Python 3, e2fsprogs, Dropbear SSH, wpa_supplicant + iw (Wi-Fi later),
  evtest (buttons), fbset (screen), ca-certificates, BusyBox (for `udhcpc` and
  `ntpd` later), and for the USB Wi-Fi/Bluetooth dongle `firmware-realtek`
  (Debian's `non-free-firmware` part), `wireless-regdb`, `usbutils`. No manuals/docs (licences are kept), no recommended extras.
- the NZEUS pieces from `overlay/` (next section).

The result is one file, `rootfs.ext4`: an ext4 file system of exactly 4 GiB
(label `nzroot`, 4 KiB blocks), as a *sparse* file (it only takes the space of
what is really in it). `nz_region.py build --rootfs-ext4` takes exactly this
file (`../region-tool/README.md`).

## What NZEUS adds (overlay/)

| What | File(s) | How it behaves |
|---|---|---|
| USB serial rescue port | `usr/local/sbin/nzeus-usb-gadget`, `etc/init.d/nzeus-usb` | The **same** USB device as the RAM rescue (`NZEUS-RAM-RESCUE-01`, VID 1d6b PID 0104, one serial function), so the PC still sees COM9. Normally the RAM system already made it and nothing changes. Started first, before any disk check. |
| Root shell on the USB cable | `usr/local/sbin/nzeus-usb-shell`, `etc/inittab` | No password (the cable to your PC is the way in, like the rescue shell). bash with line editing; `linux-port/diagnostics/rescue-command.py` and `rescue-upload.py` work **unchanged** (tested). It is there while start-up continues. |
| Root file system check | `usr/local/sbin/nzeus-rootcheck`, `etc/init.d/nzeus-rootfs` | The root arrives **read-only**. `e2fsck -p` checks it; only on result 0 or 1 it becomes read-write. Otherwise it stays read-only and says why - **never a forced repair, never an automatic restart**. This is the only check: Debian's own `checkroot.sh` is kept out (fstab: `ro`, pass 0). It also refuses read-write unless p1, p2, p5 are read-only in the kernel. |
| `/data` | `usr/local/sbin/nzeus-datamount`, `etc/init.d/nzeus-data` | label must be `nzdata`, `e2fsck -p`, then mounted `noatime,nodev,nosuid`. |
| Boot files (p1, FAT32) | `etc/fstab` | mounted **read-only** at `/mnt/stock` (the kernel also marks it read-only): the kernel and boot files, hash-checked through it. Nothing else uses it. |
| `/tmp` | `etc/fstab` | RAM, 8 MB only. Big files belong in `/data`. |
| Clock | `usr/local/sbin/nzeus-clock`, `etc/init.d/nzeus-clock` | No clock chip: at start the time comes from `/data/nzeus/clock.txt` (seconds + UTC date); saved every 5 min and at power-off, to `/data` only (tmp file + rename). |
| Swap | `usr/local/sbin/nzeus-zram`, `etc/init.d/nzeus-zram` | zram = compressed RAM, half the RAM. Nothing swaps to the card. |
| SSH | `etc/init.d/nzeus-ssh`, `etc/default/nzeus-ssh` | Debian's Dropbear, **public keys only**, starts only if `/root/.ssh/authorized_keys` exists. Host keys are made on the console at the first start. The static Dropbear from `../../usbnet/dropbear/` is also installed (`/usr/local/sbin/dropbearmulti-modern`) as a fallback. |
| USB network | `/usr/local/bin/usbnet-up`, `usbnet-down`, `usbnet-confirm` | the unchanged `../../usbnet/` scripts (console 192.168.77.1, PC .2). |
| NZEUS Deck | `/usr/local/bin/nzeus-modern`, `nzeus-deck`, `etc/init.d/nzeus-modern` | not started at power-on unless `/data/nzeus/autostart-modern` exists. By hand: `nzeus-deck start` / `stop`. Its files go to `/data/nzeus/deck`. |
| Deck helpers, examples, emoji | `/usr/local/lib/nzeus/deck-bin`, `python-examples`, `emoji` | from `../../deck-modern/` (`README.md` there): helpers + `deck-bin/SHA256SUMS`, the checked `nzeus-bt` build, the Python app's examples; the emoji pack only if `tools/make_emoji.py` made it (else a warning). `nzeus-deck` links `NZ_DIR/bin` and `NZ_DIR/emoji` there. |
| Status | `/usr/local/bin/nzeus-status` | kernel, memory, battery, partitions, read-only/read-write, USB, network, SSH, clock. |
| Power off | `etc/default/halt` | `poweroff` (or holding the power button 3 s) shuts down cleanly and the kernel's power driver switches the console off. |
| Help | `etc/motd` | plain-words help shown at SSH login (`cat /etc/motd` on the USB shell). |

`apply-overlay.sh` copies all this into a root folder (it sets the file modes,
refuses Windows line ends and private keys). The Buildroot fallback
(`../buildroot/`) uses the same scripts (see "Buildroot" below).

## Building it (my PC, WSL Ubuntu)

**Time:** about 30-60 minutes the first time (most of it runs the console's
MIPS programs through an emulator). **Space:** about 3 GB free on the WSL disk.
**Needs:** internet, and your WSL password (sudo).

1. Once, install the tools (in the Ubuntu window, or `wsl -d Ubuntu --` in front):
   ```
   sudo apt update
   sudo apt install debootstrap qemu-user-static binfmt-support debian-archive-keyring e2fsprogs wget
   ```
2. Optional, for SSH: put your **public** key (the `.pub` file, never the private
   one) into WSL, e.g. from PowerShell:
   ```
   wsl -d Ubuntu -- mkdir -p ~/r36s/keys
   wsl -d Ubuntu -- cp /mnt/c/Users/<you>/.ssh/id_ed25519.pub ~/r36s/keys/authorized_keys
   ```
   (`build-debian.sh` uses `~/r36s/keys/authorized_keys` if it exists, or the file
   in `NZ_AUTHORIZED_KEYS=...`. It refuses a file containing a private key. The
   keys are only copied into the image, never into the repo.)
3. Optional: build the real NZEUS Deck first (`apps/frontend-modern/README.md`);
   otherwise the cloud test build `nzeus-modern-cloud` is used (with a warning).
4. Build, from the project folder in PowerShell:
   ```
   wsl -d Ubuntu -- bash linux-port/rootfs/debian/build-debian.sh
   ```
   It checks that it runs in WSL and that the work folder is on the WSL disk
   (not `/mnt/c`), asks for sudo, then: debootstrap (Debian's own installer) ->
   packages -> overlay -> clean-up -> image -> manifest. At the end it prints
   `BUILD_COMPLETE`.
5. After editing only files in `overlay/`: rebuild in minutes with
   ```
   wsl -d Ubuntu -- bash linux-port/rootfs/debian/build-debian.sh --reuse
   ```

### What it produces and where

| Where | What | In git? |
|---|---|---|
| `~/nzeus-rootfs-debian/out/rootfs.ext4` (WSL disk) | the image, 4 GiB sparse | **never** (`*.ext4` is in `.gitignore`) |
| `~/nzeus-rootfs-debian/out/rootfs.ext4.info` | sha256, used bytes, features | no |
| `~/nzeus-rootfs-debian/rootfs/` | the root folder (for `--reuse`) | no |
| `~/nzeus-rootfs-debian/build.log` | everything printed | no |
| `linux-port/rootfs/debian/manifest/debian-rootfs-manifest.txt` | package list with versions, image sha256, sizes | **yes - commit this** |

From Windows the WSL folder is `\\wsl$\Ubuntu\home\<you>\nzeus-rootfs-debian`.
The previous image is kept once as `rootfs.ext4.prev`.

### Check it (PC only, nothing touches the card)

```
wsl -d Ubuntu -- python3 linux-port/rootfs/region-tool/nz_region.py selftest --workdir ~/nzeus-region-test --rootfs-ext4 ~/nzeus-rootfs-debian/out/rootfs.ext4
```
This builds the whole Linux area on a fake 64 GB card with your image and
proves the stock part stays byte-identical (needs `dosfstools mtools`).

Putting it on the real card is a separate step (I approve it) with the region
tool and the card workflow (`../PLAN.md` sections 4 and 9). This folder never
writes a card.

## Adding programs later

- **On the console** (needs a network; today that is the USB cable network,
  `usbnet-up`, plus internet sharing from the PC, which is not set up yet):
  `apt update` then `apt install NAME`. Estimate (not measured): with ~39 MB
  of RAM `apt update` is slow and may run out of memory (zram helps); the
  AVP-free "pure" mode (~118 MB) should be much easier.
- **On the PC** (easiest, fast; then put the new image on the card with the
  region tool as usual): add the package name to `packages.txt`, then
  ```
  wsl -d Ubuntu -- bash linux-port/rootfs/debian/build-debian.sh --reuse --packages
  ```
  It installs what is new (plus Debian's latest security updates) into the
  kept root folder through the emulator and makes a new image. Files in
  `/data` are not part of the image, so they survive a new root.
- `/etc/apt/preferences.d/90nzeus-no-systemd` makes apt refuse systemd, udev
  and dbus. A package that really needs one of them will not install.

## Tests on the PC (no Debian, no card)

```
sudo bash linux-port/rootfs/debian/test-debian-pc.sh [WORK_DIR [FULL_BUSYBOX]]
```
Cloud runs 2026-10-09: **264/264 PASS** with a fuller MIPS BusyBox as the
second argument (2026-10-10 with the USB host additions, `../../usbhost/`: **302/302**), 195/195 with only the console's boot BusyBox (then the
BusyBox-ash behaviour round is skipped). 6/6 deliberate mistakes were caught
(rw on e2fsck rc 2, "ro" removed from fstab, wrong USB serial, clock saved
without /data, p1/p2/p5 check skipped, bracketed paste on). One earlier run
had make-image.sh end silently with no output; it did not happen again in 5
later runs (cause unknown - probably the shared cloud machine).
Debian's real start-up order was checked once by hand with insserv 1.14 and
Debian's initscripts 3.08 (Ubuntu archive): nzeus-usb S02, nzeus-zram S03,
checkroot S04, nzeus-rootfs S05, checkfs S06, checkroot-bootclean S07,
mountall S08, nzeus-data S09, nzeus-clock S10; at power-off nzeus-clock K01
before umountfs K03. build-debian.sh checks this order again with the real
bookworm insserv. It checks: every script's syntax with dash, bash and the
console's boot BusyBox (qemu-mipsel); fstab/inittab/init headers/packages/apt
pins; the scripts' behaviour on fake folders with stand-ins for
blockdev/e2label/e2fsck/mount/mkswap/swapon (root check for e2fsck results
0-8, wrong device/label, p1/p2/p5 not read-only; `/data`; clock restore/save
(only `/data/nzeus/clock.txt`); zram size;
nzeus-status), with dash (as Debian) and BusyBox ash (as Buildroot); the USB
gadget compared value-by-value with `linux-port/usb-rescue`; the **unchanged**
`rescue-command.py`/`rescue-upload.py` against `nzeus-usb-shell` over a
pseudo-terminal (bash and dash; 166 KB binary upload with every byte value
read back exactly; a control run shows bash without our `inputrc` would send
escape codes); `apply-overlay.sh` refusals (Windows line ends, private key,
non-key line); a fake root folder through the same `make-image.sh`
(4 GiB sparse, e2fsck clean, features, owners, modes, device nodes) and then
`nz_region.py selftest --rootfs-ext4` with it (31/31); `build-debian.sh`
refusing a non-WSL PC, `/mnt/c` and a non-Linux file system.

What these tests do NOT prove: that Debian builds (not run), that it boots on
the console, that the kernel's USB gadget/ext4/SD writes behave, that Debian's
programs run on this CPU and kernel (the old stock Dropbear crashed on the
modern kernel, see `../../usbnet/dropbear/README.md`), RAM use, or speed.

## Things to know / open points

- **Kernel name:** `rescue-command.py`/`rescue-upload.py` only talk to kernels
  named `7.2.0-rc5-nzeus-experimental*`, `-gma-native*` or `-screenlog*`, and
  `usbnet-up.sh` only to `-gma-native*`. The SD-root kernel must keep such a
  name (e.g. `...-nzeus-gma-native-sdroot`). Our own scripts accept any
  `*-nzeus*` kernel.
- **What the RAM system (initramfs) must do before `switch_root`** (another
  part of the work): mount `nzroot` **read-only**, check
  `/etc/nzeus-rootfs-release` has `NZEUS_ROOTFS_LAYOUT=1` and `/sbin/init`
  exists, stop its own USB shell loop but leave the gadget plugged in, move
  `/dev /proc /sys /run`, then `exec switch_root /newroot /sbin/init`.
- `/sbin/init` is a link into `/usr/sbin` (Debian's "merged /usr"); test with
  `test -e`, not by reading the link.
- The kernel config has no `CONFIG_SYSVIPC` (a few programs need it; none in
  our list) and zram only in the power/pure kernels (`CONFIG_ZRAM`); without
  it there is simply no swap.
- Debian start scripts switched off by the builder: `hwclock.sh` (no clock
  chip), `brightness` (would write a saved level to the AVP backlight at each
  start), `kmod`, and Debian's own `dropbear`/`ssh` start (ours is `nzeus-ssh`).
- No getty on the screen (no keyboard yet) and none on the UART (it is only
  listened to). No syslog daemon: messages go to `dmesg` and `/run/*.log`.
- Root has no password at all (`*`): password logins are impossible, the USB
  shell needs none, SSH needs a key.

## Buildroot (the small fallback)

`../buildroot/` still builds a tiny musl system. It now uses the same scripts:
its `post-build.sh` runs `apply-overlay.sh TARGET buildroot` (shared
`/usr/local/sbin/nzeus-*`, `nzeus-status`, `nzeus-deck`, usbnet, Dropbear
static, nzeus-modern, motd, e2fsck.conf), its `S01nzroot`/`S03nzdata` call
`nzeus-rootcheck`/`nzeus-datamount`, new `S00nzusb`, `S02nzzram`, `S04nzclock`,
and its inittab runs `nzeus-usb-shell` on ttyGS0. Differences that remain:
BusyBox init instead of sysvinit, its own fstab (no `/mnt/stock`), no apt,
`BR2_TARGET_GENERIC_REMOUNT_ROOTFS_RW` is now off
(the root becomes writable only after the check). Not built either.

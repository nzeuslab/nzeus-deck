# gma-native-sdroot: start the Linux system that lives on the SD card

Written 2026-10-09/10. **PC/QEMU-tested only. Nothing here has
run on the console yet, and nothing here writes the card.**

## What it does, in plain words

Until now the modern kernel only ever ran its small built-in RAM system (the
"rescue shell" on COM9). This kernel, **`gma-native-sdroot`**, can instead start
the real Linux system that is stored on the SD card (the "Linux area" from
`../rootfs/PLAN.md`: `nzroot` = the system, `nzdata` = your files at `/data`).

It is careful about it. At every start it checks the card first and only
switches to the SD system when **everything** is exactly right. If anything is
wrong - or you hold **FN** - it does exactly what the RAM rescue system always
did: USB rescue shell on COM9 (`NZEUS-RAM-RESCUE-01`) plus the serial shell.

```
power on -> kernel -> /init (RAM system)
  nz-sdboot checks (reads only):
    1. "nzeus.ramonly" on the kernel command line?      -> stay in RAM
    2. FN held?                                           -> stay in RAM
    3. card partitions p1..p5 appear within 10 s?        no -> stay in RAM
    4. both NZEUS headers (p2 and p5): magic, CRC, same?  no -> stay in RAM
    5. card ID (CID) and card size = header?              no -> stay in RAM
    6. every partition start/size/name = header,
       p1 (stock FAT32), p2, p5 read-only in the kernel?  no -> stay in RAM
    7. p3 holds ext4 with the header's UUID?              no -> stay in RAM
  mount p3 (nzroot) READ-ONLY at /newroot (gives up after 20 s)
  /newroot/sbin/init is a program this kernel can run?    no -> unmount, stay in RAM
  blockdev says p1, p2, p5 are read-only?                 no -> unmount, stay in RAM
  move /dev /proc /sys /run into /newroot, exec switch_root /newroot /sbin/init
```

The SD system's own start-up then checks its file system (`e2fsck`) while it is
still read-only and only then makes it writable (rootfs agent's part, see
"Contract" below). The stock FAT32 (p1) and both header blocks (p2, p5) are
read-only **inside the kernel**, so even a mistake in the SD system cannot
write them.

## The pieces (this folder + `../display`)

| File | What |
|---|---|
| `../display/build-sdroot.sh` | builds everything in WSL (kernel, initramfs, board tree) and runs the verifier |
| `../display/r36s-gma-avp-sdroot.dts` | = pure board tree + the `blkdevparts=` line in `bootargs` |
| `../display/verify-avprpc.py --variant sdroot` | checks the build against `gma-native-net` (see "Exact differences") |
| `build-initramfs.sh` | builds the RAM system: BusyBox, `nz-sdboot`, `/init`, list for the kernel |
| `busybox.config`, `busybox-applets.txt` | the boot BusyBox config (= old one + 5 options) and its exact applet list |
| `nz-sdboot.c` | the checker (static MIPS program, ~680 KB, opens everything read-only) |
| `init` | the new `/init` (the rescue part at the bottom is the old `../init`, unchanged) |
| `initramfs-contents.txt` | every entry the RAM system must contain - nothing more, nothing less |
| `list-initramfs.py` | prints a built `initramfs.cpio.gz` in that format |
| `make-test-header.py` | makes a REAL header with `../rootfs/region-tool/nz_region.py` (for tests) |
| `test-nz-sdboot.py`, `test-sdroot-init.py`, `test-usb-admission.py`, `test-all.sh` | PC tests (below) |
| `test-results/` | results of the cloud run |

## Build on the PC

Needs `build-pure.sh` and then `build-net.sh` done first (sdroot sits on net).

```
wsl -d Ubuntu -- bash linux-port/display/build-sdroot.sh
wsl -d Ubuntu -- bash linux-port/sdroot/test-all.sh
wsl -d Ubuntu -- python3 linux-port/diagnostics/package-uart-boot.py --display-sdroot
wsl -d Ubuntu -- python3 linux-port/diagnostics/emulate-entry.py --display-sdroot --through-fdt
```

`build-sdroot.sh` ends with `GMA_NATIVE_SDROOT_PC_BUILD_COMPLETE`. It only makes
files (`display/build-sdroot/`, work folder
`/home/nzeus/r36s-linux-port-work-20261007/sdroot`); it never touches a card,
the console or USB. `test-all.sh` must end with `SDROOT_PC_TESTS_PASSED`.

Putting the kernel on the card is a normal card job (lab rule 2) and is
**not prepared here**: the card tool needs two new profiles,
`gma-native-sdroot` (this kernel over pure/net) and `gma-native-sdroot-recovery`
(back to the pure or net kernel). Before this kernel can actually start the SD
system, the Linux area itself must be on the card (`../rootfs/PLAN.md` steps)
- without it, this kernel simply behaves like the net kernel (RAM rescue).

## What you will see

- **Screen:** the same as with the pure/net kernels (this kernel prints no boot
  text on the screen). What the SD system shows later is up to the SD system.
- **Serial log (Uno, 19200):** lines starting `nz-sdboot:` - one per check,
  e.g. `card CID ... = header`, `mmcblk0p3 nzroot start ... matches` - then
  either `NZ_SDBOOT_OK` and `NZEUS SD root: all checks passed: exec switch_root`
  or `NZ_SDBOOT_STAY_IN_RAM: ...` with the reason on the line before it.
  The same lines go to the kernel log (`dmesg | grep nz-sdboot`) and to
  `/run/nzeus-sdboot.log` (also visible from the SD system).
- **USB:** in both cases COM9 = `USB\VID_1D6B&PID_0104\NZEUS-RAM-RESCUE-01`.
  In RAM mode it appears as today. In SD mode the SD system makes it (a few
  seconds later), so `diagnostics/rescue-command.py` works the same way.
  `uname -r` says `7.2.0-rc5-nzeus-gma-native-sdroot+` (admitted by
  `/usb-rescue` and by `rescue-command.py`; `test-usb-admission.py` proves it).

## Escape: FN, nzeus.ramonly

- **Hold FN while switching on** and keep holding until COM9 appears: the
  console stays in the RAM rescue system (`FN is held` in the log). Nothing on
  the card is mounted. (Technically: `nz-sdboot` opens the `r36s-keys` input
  device - its keys are only polled while it is open - waits 150 ms, reads the
  keys, waits 150 ms more, reads again; FN = `KEY_MENU`, GPIO L02.)
- For tests: add `nzeus.ramonly` to the kernel command line (a separate board
  tree) - same result without holding a key.
- If the `r36s-keys` device is missing, FN cannot be read, so it also stays in
  RAM (an escape that might not work is not accepted).

## If something goes wrong (recovery)

| What | What happens | What to do |
|---|---|---|
| any check fails | RAM rescue, reason in the serial log/`dmesg` | read the reason; fix the card from the PC |
| SD system starts but is broken | its own problem; RAM rescue not running | power off, hold FN at the next start -> RAM rescue |
| `/sbin/init` of the SD system crashes right after the switch | kernel panic (screen frozen, no COM9); `panic=0` = no reboot | switch off by hand (the 3 s power-button hold may not work after a panic - untested); next start hold FN |
| card reader / card hangs | after 30 s `nz-sdboot` gives up (`no answer`), RAM rescue | power off; check the card on the PC |
| this kernel does not boot at all | as with every new kernel | card profile back to the **pure or net** kernel (`gma-native-sdroot-recovery`) |

Things `nz-sdboot` checks before the switch so that a kernel panic is unlikely:
`/sbin/init` exists in the SD root (symlinks resolved inside the SD root, so
Debian's `/sbin -> usr/sbin` works), is executable, is a 32-bit little-endian
MIPS program, o32, not NaN2008, not FP64, not MIPS release 6, and its loader
(e.g. `/lib/ld.so.1`) or `#!` interpreter exists. It cannot prove the program
actually works.

## Contract: what the SD root (rootfs agent) must do

When `/sbin/init` of the SD root starts:

- It is PID 1, started as `/sbin/init` with no arguments. `/` is
  `/dev/mmcblk0p3`, **ext4, mounted read-only, noatime**. Mounted already
  (moved from the RAM system, do not mount again): `/dev` (devtmpfs, with
  `/dev/pts` devpts inside), `/proc`, `/sys`, `/run` (tmpfs, holds
  `/run/nzeus-sdboot.log`). Not mounted: `/tmp`, configfs, `/data`.
  So the root must contain the folders `/dev /proc /sys /run` (checked) and a
  working `/sbin/init` (checked as above).
- **USB shell:** no USB gadget exists. The root must create it exactly like
  `../usb-rescue` does: configfs at `/sys/kernel/config`, gadget folder
  `usb_gadget/nzeus_rescue`, idVendor `0x1d6b`, idProduct `0x0104`, bcdUSB
  `0x0200`, bcdDevice `0x0100`, bDeviceClass `0x02`, serialnumber
  `NZEUS-RAM-RESCUE-01`, manufacturer `NZEUS LAB`, product
  `NZEUS Experimental Linux Rescue`, config `c.1` (MaxPower 250) with function
  `acm.usb0`, bound to the first controller in `/sys/class/udc`. Then a **root
  shell on `/dev/ttyGS0` without a login prompt** (e.g. `getty -n -l
  <autologin> ttyGS0` or a respawning `setsid -c sh` like `usb-rescue`), with a
  POSIX `sh`, `stty`, `uname`, here-documents and a writable `/tmp` - that is
  all `diagnostics/rescue-command.py` needs. Keeping the gadget name
  `nzeus_rescue` lets `../usbnet/usbnet-up.sh` work unchanged.
- Start the USB shell **before** anything that may hang (file system check,
  network), so COM9 is there even if the rest of the start-up fails.
- **File systems:** check `/` with `e2fsck -p` while it is still read-only
  (allowed for `/`), remount read-write only if `e2fsck` returned 0 or 1;
  otherwise stay read-only and say so (no forced repair, no automatic reboot).
  `/data` = `/dev/mmcblk0p4` (label `nzdata`), `e2fsck -p` before mounting.
  Note: mounting p3 read-only can still replay the ext4 journal after a power
  loss - that is a normal ext4 write inside p3 (allowed).
- **Never** write `/dev/mmcblk0`, `/dev/mmcblk0p1`, `p2` or `p5`, never
  `blockdev --setrw` them, never mount `p1` read-write (see
  `../rootfs/PLAN.md` section 10).
- Shutdown: stop services, `swapoff -a`, `sync`, remount `/` read-only /
  unmount `/data`, then power off (`nz-poweroff` / `poweroff`).
- Kernel facts the root can rely on: no modules (everything built in), no
  cgroups (sysvinit only), `devtmpfs`, `inotify`, `signalfd`, `epoll`,
  `timerfd`, `eventfd`, `fhandle` (udev works), System V IPC, POSIX shared
  memory via tmpfs, IPv4 + IPv6, swap (zram), normal o32 hard-float programs
  (FPXX/FP32, legacy NaN - Debian mipsel bookworm). RAM: ~39 MB with the
  factory AVP running, ~118 MB after the pure-Linux RAM release.

## Exact differences from gma-native-net

Kernel config (checked by `verify-avprpc.py --variant sdroot`, nothing else may differ):

```
CONFIG_LOCALVERSION       "-nzeus-gma-native-net" -> "-nzeus-gma-native-sdroot"
CONFIG_INITRAMFS_SOURCE   ".../display-usbfix/initramfs.list" -> ".../sdroot/initramfs.list"
CONFIG_CMDLINE_PARTITION  (off) -> y     the blkdevparts= partitions
CONFIG_SYSVIPC            (off) -> y     System V IPC: Debian userland (fakeroot, X MIT-SHM, ipcs...)
CONFIG_SYSVIPC_SYSCTL     (off) -> y     follows SYSVIPC
(CONFIG_IPC_NS is explicitly kept off: no extra namespace)
```

Everything else a Debian sysvinit system needs was already in net: DEVTMPFS,
TMPFS, UNIX, INET/IPV6, INOTIFY_USER, EPOLL, SIGNALFD, TIMERFD, EVENTFD,
FILE_LOCKING, FHANDLE, EXT4 + JBD2, BINFMT_ELF/SCRIPT, FUTEX, AIO,
MIPS_FP_SUPPORT, SWAP, UNIX98_PTYS, MMC_BLOCK (8 minors >= 5 partitions).
Not added on purpose: POSIX_MQUEUE (rarely used), cgroups (systemd only),
ACLs/xattrs security labels, f2fs, modules.

Board tree: only `/chosen/bootargs` gains
`blkdevparts=mmcblk0:15599M@1M(nzstock)ro,4M@16G(nzhdr)ro,4G@16388M(nzroot),40416M@20484M(nzdata),4M@60900M(nzhdr2)ro`
(= `nz_region.py layout` for the 64 GB test card; the verifier checks both).
If the region tool prints a different line for the real card (a shorter
FAT32), update the DTS and `BLKDEVPARTS` in `verify-avprpc.py` together -
`nz-sdboot` would otherwise refuse because p1 differs from the header.

RAM system (initramfs): the old one (`display/bootlog/build-usbfix`) plus
`/newroot`, `/bin/nz-sdboot` and the applets `switch_root kill mountpoint
blockdev`; `/init` replaced by `init` here; `/usb-rescue` unchanged; BusyBox
rebuilt from the same official 1.37.0 tarball with `SWITCH_ROOT KILL
MOUNTPOINT BLOCKDEV FEATURE_MOUNT_FLAGS` added (the last one gives `mount
--move` and `noatime`). The kernel file grows by about 0.3 MB (mostly
`nz-sdboot`); the RAM system is freed after the switch.

## Tests (cloud run 2026-10-09, see `test-results/`)

- `test-nz-sdboot.py`: **42/42** - fake `/proc`, `/sys`, `/dev` trees under
  qemu-mipsel with a real `nz_region.py` header and a real `mke2fs`
  superblock: pass; escape (FN, `nzeus.ramonly`); refusals: no keys device,
  bad CRC in p2 / in p5, no magic, empty header, two valid but different
  headers, header says p1 not read-only, CID mismatch, card size, wrong start
  p3 / p1, wrong size p4, wrong partition name, p1 / p2 / p5 not read-only,
  extra p6, no ext4, wrong ext4 UUID, missing p2, missing `/dev` node, no card;
  "no answer" (a read that never returns) -> exit 3 after the time limit;
  argument safety; `--check-init`: static, dynamic with/without loader,
  usrmerge symlinks, absolute symlink, missing, PC program, NaN2008, not
  executable, script with/without interpreter.
- `test-sdroot-init.py`: **22/22** - the real `/init` (only its marked path
  block replaced) with the new and the current boot BusyBox under qemu-mipsel,
  dangerous commands stubbed: syntax with both BusyBoxes, no `[`, success path
  (ro mount, checks, moves in order, `switch_root`), and every failure path ends
  in the unchanged RAM rescue (sdboot exit 1/3/4, mount fails, mount hangs,
  not ro, bad init, p1/p2/p5 not ro, no `/run`, a move failing half way with
  roll-back, a BusyBox without `switch_root`), plus two end-to-end runs with the
  REAL `nz-sdboot` (pass -> switch, wrong card -> RAM).
- `test-usb-admission.py`: **10/10** - `/usb-rescue`'s name guard admits
  `7.2.0-rc5-nzeus-gma-native-sdroot+` (PC sh and MIPS BusyBox with QEMU_UNAME).
- Kernel: built in the cloud at the pinned commit (copy of the net build
  folder + the config above); `verify-avprpc.py --variant sdroot` passes on
  stand-in folders; regression `keys power pure net` still pass; **28 mutants
  caught** (wrong blkdevparts, missing `ro` on nzstock/nzhdr/nzhdr2, extra `ro`,
  no line, other bootargs, extra property/node change, extra config options
  POSIX_MQUEUE / IPC_NS, missing CMDLINE_PARTITION / SYSVIPC, wrong initramfs
  list / version, kernel without the parser, changed `/init` / `/usb-rescue`,
  BusyBox copy differs, applet list, missing `/newroot` / `switch_root`, extra
  file, old `/init`, dynamic `nz-sdboot`, wrong permissions) and an unchanged
  re-pack is accepted. `build-sdroot.sh` itself ran end to end on a scratch copy
  (only its three WSL paths changed). `package-uart-boot.py --display-sdroot`
  packaged it (scratch copy; the old direct-image hash file is not in the repo).
- Not run in the cloud: `emulate-entry.py --display-sdroot` (no
  qemu-system-mipsel / gdb-multiarch there). Nothing on hardware.

## Not known yet (needs the console)

- Whether the kernel really creates `mmcblk0p1..p5` from `blkdevparts=`
  (source read: `block/partitions/cmdline.c` runs first, sets the `ro`
  flag and `PARTNAME`), and their sysfs values.
- Whether `r36s-keys` reads FN reliably 150-300 ms after opening.
- SD **writes** from Linux on this SoC have never been tried (the SD root's
  e2fsck/remount and an ext4 journal replay are the first writes).
- Whether the Debian root and its USB shell start (rootfs agent).

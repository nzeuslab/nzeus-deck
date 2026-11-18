# USB host mode: Wi-Fi + Bluetooth dongle, USB keyboard, USB stick

Written 2026-10-10. **PC/QEMU-tested only. Nothing here has run
on the console. Nothing here writes the SD card, flashes, uploads or reboots.**
Plan and background: `../purelinux/USB-HOST.md`, `../purelinux/BT-HDMI.md`.

## What this is, in plain words

The console's USB-C **data** port (the one the PC cable uses) can work two ways:

- **device mode** - the console is a "gadget" for a PC. This is how the USB
  rescue serial port (COM9, `NZEUS-RAM-RESCUE-01`) works. **Our lifeline.**
- **host mode** - the console is the "computer" and you plug things into it
  (with a USB-C OTG adapter): a USB stick, a keyboard, the Wi-Fi + Bluetooth
  dongle. That turns the console into a small "full Linux PC" with Wi-Fi.

Until now the modern kernel could only do device mode. This folder prepares:

1. **Phase 0 (read-only):** a tiny program that only *reads* the USB registers,
   so we can compare the stock system (where host mode works) with ours.
2. **A new kernel `gma-native-host`** = `gma-native-sdroot` + a switchable USB
   port + drivers for USB sticks, Wi-Fi (RTL8821CU) and Bluetooth. **It still
   starts in device mode with the same rescue port as before.**
3. **A trial switch with an automatic way back:** `usb-host-trial.sh` switches
   to host mode for a few minutes and then *by itself* back to device mode with
   the rescue port - unless you confirm.
4. **Wi-Fi bring-up** with your network settings stored only on the console,
   and SSH over Wi-Fi as the second lifeline while the USB port is a host.

## What you need

- A **USB-C OTG adapter** (USB-C plug -> USB-A socket, "OTG"). It goes into the
  console's data port (the same one as the PC cable; the other USB-C is
  charge-only).
- The **LB-Link BL-WN650BT** dongle (Realtek RTL8821CU, Wi-Fi + Bluetooth).
- For the first tests: a **USB stick** (FAT32) and a simple **USB keyboard**.
- For Phase 0: a cheap **USB-C power meter / tester** (shows volts) if you have one.
- The console must be on the **SD Debian root** (`../rootfs/`, `../sdroot/`) for
  the trial and Wi-Fi scripts. Phase 0 works on any kernel.

## Safety (read this first)

- **UNPLUG THE PC CABLE before host mode. Never use host mode with a PC
  attached.** Nobody has measured yet what the console does with the 5 V line
  (VBUS) in host mode. `usb-host-trial.sh` waits until the USB controller says
  "not attached" (no PC) and refuses to switch otherwise.
- **There is no USB rescue port while the console is a host.** The ways back:
  1. **wait**: after the trial time (default 300 s = 5 min) it switches back by
     itself. Then unplug the adapter, plug the PC cable in: COM9 is back.
     If the USB network is on for boot (`/etc/nzeus/usbnet-at-boot`, the
     default since 2026-10-10), the way back also starts `nzeus-usbnet-boot`,
     exactly as at power-on: plug the PC in within 3 minutes and it becomes
     serial + network again (`ssh nzeus-deck`); otherwise COM9 stays.
     (Before host mode the trial stops the USB network mode, so its own
     180 s way back can never touch the port while it is a host.)
  2. `usb-host-off.sh` (on the console's keyboard/terminal, or over SSH on Wi-Fi).
  3. **power off**: hold the power button 3 s. **Every start is device mode
     with the rescue port again** - host mode is never remembered.
- The kernel itself refuses a switch it cannot do and falls back to device mode.
- Everything here lives in RAM (`/run`) except two log files on `/data`.
  Nothing writes the stock FAT32, the boot files, NOR flash or the car.
- Your Wi-Fi password is only in `/data/nzeus/wifi.conf` on the console
  (owner root, `chmod 600`). `wifi.conf` is in `.gitignore`; the scripts never
  print or log it.

## Order of the tests (do them in this order, stop at the first surprise)

### Step 1 - Phase 0, read-only, on the kernels you already have

Nothing changes on the console. `usb-regs` reads registers through `/dev/mem`
opened **read-only**; it never reads registers that change when read (see
"What usb-regs never reads" below).

a) **Stock system with the dongle working** (stock has the port in host mode;
   this is the most useful reading - it shows how the stock driver set up host
   mode). First, with the PC cable, copy the two files over ADB:
   ```
   adb push linux-port/usbhost/usb-regs-mipsel /tmp/nzeus-tools/usb-regs
   adb push linux-port/usbhost/usb-phase0.sh /tmp/nzeus-tools/usb-phase0.sh
   adb shell chmod 755 /tmp/nzeus-tools/usb-regs
   ```
   Then unplug the PC, plug the dongle in on the OTG adapter, wait until stock
   Wi-Fi/Bluetooth works, and in the stock NZEUS Deck **Terminal** app run
   `sh /tmp/nzeus-tools/usb-phase0.sh dongle > /tmp/nzeus-tools/phase0-dongle.txt`.
   Plug the PC back in and fetch it: `adb pull /tmp/nzeus-tools/phase0-dongle.txt`.
   (Read-only. If `/dev/mem` is missing on stock, note that and skip.)
b) **Modern kernel (whatever is on the card now), PC cable = rescue port:**
   ```
   python linux-port\diagnostics\rescue-upload.py linux-port\usbhost\usb-regs-mipsel --name usb-regs
   python linux-port\diagnostics\rescue-command.py linux-port\usbhost\usb-phase0.sh --seconds 20
   ```
   (`usb-phase0.sh` is run with the label `unlabelled` that way; that is fine.)
c) With the USB-C tester between the console and the OTG adapter: write down
   the voltage with nothing plugged in, with the stick, with the dongle - on
   stock. (Tells us if the console powers the port by itself.)

Keep the outputs (`diagnostics/uart-usb-fix/usb-command-*.txt`). The lines to
compare: `USB0.UTMI(+380)`, `USB0.VBUS(+381)`, `USB0.DEVCTL`, `PHY0.UPHY20`.
If stock shows `force_vbus_valid(bit15)=1` and our host trial sees no device,
use `usb-host-trial.sh 300 --force-vbus` (see Step 4).

### Step 2 - build the host kernel (PC, WSL)

Needs `build-sdroot.sh` done first.
```
wsl -d Ubuntu -- bash linux-port/display/build-host.sh
wsl -d Ubuntu -- python3 linux-port/diagnostics/package-uart-boot.py --display-host
wsl -d Ubuntu -- python3 linux-port/diagnostics/emulate-entry.py --display-host --through-fdt
wsl -d Ubuntu -- bash linux-port/usbhost/test-usbhost-pc.sh
```
`build-host.sh` ends with `GMA_NATIVE_HOST_PC_BUILD_COMPLETE`; the test with
`USBHOST_PC_TESTS_PASSED`. Also rebuild the Debian root (Step 3 needs the new
packages and scripts): `wsl -d Ubuntu -- bash linux-port/rootfs/debian/build-debian.sh --reuse --packages`.

### Step 3 - the host kernel on the card, still in DEVICE mode

Putting the kernel on the card is a normal card job (lab rule 2) and is
**not prepared here**: the card tool needs two new profiles, `gma-native-host`
(this kernel over sdroot) and `gma-native-host-recovery` (back to sdroot).
Then, watching the restart:
- COM9 must come back exactly as before (`NZEUS-RAM-RESCUE-01`). **This is the
  most important check**: the kernel starts the USB port in a new way.
  (With `nzeus-usbnet` on, the Debian boot then moves on to serial + network
  as usual: `ssh nzeus-deck` must work like with the sdroot kernel.)
- `uname -r` = `7.2.0-rc5-nzeus-gma-native-host+`
- `usb-host-status` shows `role: device   (role switch 18844000.usb-role-switch ...)`
- `dmesg | grep -i -E 'musb|role'` shows `MUSB core started as peripheral` and
  `dr_mode otg: started as peripheral`.

### Step 4 - first host trial: a USB stick

On the rescue shell (COM9):
```
usb-host-trial 300
```
It prints a big warning and `USBHOST_TRIAL_STARTED`. Now **unplug the PC cable**
(within 60 s). Watch the UART log (Uno): `usbhost: no PC attached` ->
`HOST MODE ON`. Plug in the OTG adapter with the stick. If you have a USB
keyboard you can't use it yet (only one port) - so just wait. After 5 minutes:
`no confirmation` -> `BACK TO DEVICE MODE`. Unplug the adapter, plug the PC in,
then read the log:
```
cat /data/nzeus/usbhost.log
dmesg | grep -i -E 'usb|sd[a-z]|musb' | tail -40
```
Success = the stick appeared (`sda`) during host mode. If nothing appeared, run
the trial again with `--force-vbus` (only after looking at Phase 0).

### Step 5 - keyboard, then the dongle

- **Keyboard:** same trial with a USB keyboard; type `usb-host-status` and
  `usb-host-confirm` on the console's own terminal (NZEUS Deck terminal or the
  text console). Then `usb-host-off` to go back.
- **Dongle:** same trial with the dongle. If it shows up as `0bda:1a2b` ("driver
  CD" mode), `wifi-up` ejects it with `nz-cdeject` (it then comes back as
  `0bda:c820` and the Wi-Fi driver loads its firmware from `/lib/firmware`).

### Step 6 - Wi-Fi and SSH (the second lifeline)

Once, on the console (rescue shell):
```
cp /usr/local/lib/nzeus/usbhost/wifi.conf.example /data/nzeus/wifi.conf
nano /data/nzeus/wifi.conf          # your network name and password
chmod 600 /data/nzeus/wifi.conf
```
SSH needs your public key in `/root/.ssh/authorized_keys` (the Debian builder
puts it there, `../rootfs/debian/README.md`). The dongle takes the only USB port,
so no keyboard: let the trial start Wi-Fi by itself:
```
usb-host-trial 600 --wifi      # on COM9; then unplug the PC, plug in adapter + dongle
```
After host mode is on, `wifi-up` runs by itself (it waits up to 120 s for the
dongle), joins, gets an address by DHCP and starts SSH. Find the address in your
router's list (name `nzeus`) or on the UART log (`wifi: address ...`). Then from
the PC:
```
ssh root@ADDRESS usb-host-confirm      # only once SSH really works
```
(A small USB hub on the OTG adapter would allow dongle + keyboard together -
untested with this USB controller.)
From then on, `ssh root@ADDRESS` is the way in; `usb-host-off` (over SSH) brings
back the USB rescue port (the SSH session ends then - that is expected).

Bluetooth (keyboards/gamepads) comes after Wi-Fi: the kernel parts are in
(`btusb`, `btrtl`, HIDP, UHID); `apps/bluetooth` `nzeus-bt` must be rebuilt
for the modern kernel first (BT-HDMI.md).

## The pieces

| File | What |
|---|---|
| `usb-regs.c`, `usb-regs-mipsel` | Phase 0 register reader (static MIPS, `/dev/mem` read-only, `PROT_READ`) |
| `usb-phase0.sh LABEL` | Phase 0 script: reader + roles/devices/interrupts/charger from `/sys`, read-only |
| `kernel/hc16xx.c`, `kernel/phy-hichip-usb.c`, `kernel/install-source.py` | the musb glue and PHY changes; the installer checks the pinned commit and the original bytes (`--diff` prints the change) |
| `../display/build-host.sh`, `../display/r36s-gma-avp-host.dts` | kernel build (sdroot + the above + config) and board tree |
| `../display/verify-avprpc.py --variant host` | exact differences against sdroot (config, DT, initramfs, symbols) |
| `usb-host-trial.sh [SECONDS] [--force-vbus] [--wifi]` | the trial with the automatic way back (`--wifi`: start `wifi-up.sh` by itself once host mode is on) |
| `usb-host-confirm.sh`, `usb-host-off.sh`, `usb-host-status.sh` | keep host mode / leave it / what is plugged in (read-only, like `lsusb -t`) |
| `nz-cdeject.c`, `nz-cdeject-mipsel` | ejects the dongle's "driver CD" mode; touches **only** a device that is exactly `0bda:1a2b` |
| `wifi-up.sh`, `wifi-down.sh`, `udhcpc.script`, `wifi.conf.example` | Wi-Fi with Debian's `wpa_supplicant` + BusyBox `udhcpc`, SSH as second lifeline |
| `SHA256SUMS` | the two MIPS programs (the Debian builder checks them) |
| `test-usbhost-pc.sh [FULL_BUSYBOX]` | PC tests (below) |

The Debian builder installs the scripts into `/usr/local/lib/nzeus/usbhost/`,
the programs as `/usr/local/sbin/nz-cdeject` and `/usr/local/sbin/usb-regs`,
and short commands `usb-host-trial`, `usb-host-confirm`, `usb-host-off`,
`usb-host-status`, `wifi-up`, `wifi-down` in `/usr/local/bin`. New packages:
`firmware-realtek` (from Debian's `non-free-firmware` part: Realtek's
redistributable licence, kept in `/usr/share/doc/firmware-realtek/copyright`),
`wireless-regdb`, `usbutils`.

## How the kernel part works

Siarhei's glue always made the USB core a gadget (`.mode = MUSB_PERIPHERAL /* TODO */`,
one shared settings struct for both controllers). Now (`kernel/hc16xx.c`):

- each controller gets its **own** settings; the mode comes from the board tree
  `dr_mode`: `peripheral` (as before), `host` (host only, like stock), or `otg`
  = **starts as peripheral** and offers a **role switch** you can write:
  `/sys/class/usb_role/18844000.usb-role-switch/role` (`host` / `device`).
  The board tree changes only usb0 to `otg`; usb1 stays off (never showed a
  device - probably not wired).
- a role change **re-creates the USB core** in the other mode. Host mode is then
  the USB core's normal "host only" path (the session bit is set by
  `musb_start()` when the root hub powers its port - like the stock kernel,
  whose device tree also says `dr_mode = "host"`), and device mode is exactly the
  boot path, so the rescue gadget is created the same way as today. If host
  mode fails, the glue goes back to peripheral by itself.
- the old "legacy" `mode` file of the USB core called a function that can sleep
  while interrupts were off (a kernel bug). That function now never sleeps and
  never changes the mode; only the role switch does.
- `nzeus_host_force_vbus` (in `/sys/class/usb_role/18844000.usb-role-switch/device/`,
  default 0): if 1, the PHY reports "VBUS valid" in host mode (PHY register
  UPHY20 bit 15, "configure phy vbus/avalid/bvalid = 1" in Siarhei's PHY
  driver). Off by default; with 0 the PHY driver touches no new register.

### Research: what the stock driver did

- The stock GPL tree we have (`hichip-linux-drivers`: amprpc, avp-proxy, fbdev,
  lvds, mmz, virtuart, watchdog, hwspinlock) **contains no USB/musb code** -
  checked both copies; the SDK subset in the cloud has none either. So there is
  no stock `hc16xx_musb_set_vbus` / `set_mode` source to port line by line.
- What we know instead:
  - SDK `board/hichip/hc16xx/common/dts/hc16xx-common.dtsi:496-514`: both
    controllers `dr_mode = "host"`, `index_num`, `ctrl_base = <0x18800000>`
    (the system-control block; what the stock glue does there is unknown).
  - SDK bootloader `apps-bootloader/source/main.c:450-457` (`reset_usb_mode`):
    after USB-device upgrades it calls `hcusb_set_mode(port, MUSB_HOST)` - host
    is the normal state on stock.
  - Stock boot log (`../card-test/restored-dmesg.txt:93-111`): `---> usb#0 reset`
    twice and `---> usb#0 mode : 0x1 (0x1:host, ...)` - reset, then host mode
    via the musb core's host-only path ("MUSB HDRC host driver").
  - Siarhei's glue `hc16xx_musb_set_mode()` has the UTMI register sequence for
    host (OTG detection off = bits 7,3,2 set, PERIPHERAL bit 4 clear) and the
    PHY power cycle; it is kept byte-for-byte (`hc16xx_utmi_set_mode`).
- **Phase 0 (a)** is how we fill the gap: the same register reader on stock
  shows the exact UTMI/VBUS/PHY values the stock driver leaves in host mode.

### Kernel config: exactly what changes against gma-native-sdroot

(checked by `verify-avprpc.py --variant host`; nothing else may differ)

```
LOCALVERSION "-nzeus-gma-native-sdroot" -> "-nzeus-gma-native-host"
asked for:   SCSI BLK_DEV_SD USB_STORAGE CFG80211 MAC80211 RTW88 RTW88_8821CU
             BT BT_BREDR BT_LE BT_HIDP BT_HCIBTUSB BT_HCIBTUSB_RTL UHID
came along:  SCSI_COMMON SCSI_DMA BLK_DEV_BSG BLK_DEV_BSG_COMMON SG_POOL
             CFG80211_CRDA_SUPPORT CFG80211_REQUIRE_SIGNED_REGDB CFG80211_USE_KERNEL_REGDB_KEYS
             MAC80211_HAS_RC MAC80211_RC_MINSTREL MAC80211_RC_DEFAULT_MINSTREL
             MAC80211_RC_DEFAULT="minstrel_ht" RTL_CARDS RTW88_CORE RTW88_USB RTW88_8821C
             DEV_COREDUMP WANT_DEV_COREDUMP BT_LE_L2CAP_ECRED BT_RTL BT_INTEL REGMAP
             CRYPTO + AEAD AES CCM CTR ECB ECC ECDH GCM HASH KPP MANAGER RSA SHA256 SIG
             SKCIPHER (and their *2 helpers), CRYPTO_HW, CRYPTO_LIB_AES
             CRYPTO_LIB_AES_CBC_MACS CRYPTO_LIB_ARC4 CRYPTO_LIB_GF128HASH CRYPTO_LIB_UTILS
             CRYPTO_HASH_INFO ASN1 ASSOCIATIVE_ARRAY ASYMMETRIC_KEY_TYPE
             ASYMMETRIC_PUBLIC_KEY_SUBTYPE CLZ_TAB KEYS MPILIB OID_REGISTRY
             PKCS7_MESSAGE_PARSER SYSTEM_DATA_VERIFICATION SYSTEM_TRUSTED_KEYRING
             X509_CERTIFICATE_PARSER SYSTEM_TRUSTED_KEYS=""
kept off:    MODULES, FW_LOADER_USER_HELPER, BT_RFCOMM, BT_BNEP, BT_LEDS, BT_DEBUGFS,
             CFG80211_WEXT, MAC80211_MESH/LEDS/DEBUGFS, SCSI_PROC_FS, SCSI_LOWLEVEL,
             BT_HCIBTUSB_BCM/MTK, RFKILL (not needed)
already on:  USB_MUSB_DUAL_ROLE USB_ROLE_SWITCH USB_HID HID_GENERIC FW_LOADER PACKET VFAT
```
The RSA/X.509/PKCS#7 group is there because the kernel checks the signature of
Debian's `regulatory.db` (kernel default; turning it off needs `EXPERT`). It
could be dropped later to save roughly 0.2 MB (guess).

**Firmware:** everything is built in, but the firmware is NOT in the kernel.
`rtw88` asks for `rtw88/rtw8821c_fw.bin` when the dongle is plugged in
(`request_firmware_nowait` at probe), `btrtl` for `rtl_bt/rtl8821c_fw.bin`
(config file optional for 8821C) when Bluetooth starts. The port is only a host
after the SD root is mounted, so the kernel finds them directly in
`/lib/firmware` (no user-space helper needed).

### RAM cost (measured on the cloud build, file sizes)

| | gma-native-sdroot | gma-native-host | more |
|---|---|---|---|
| kernel in RAM (ELF memory size, incl. bss) | 11,218,752 B | 13,514,128 B | **+2.19 MiB** |
| text+data+bss (`size`) | 10,146,796 B | 12,405,044 B | +2.15 MiB |
| loaded image | 9,943,104 B | 12,203,424 B | +2.16 MiB |
| gzip -9 (card file size, about) | 5.06 MB | 6.13 MB | +1.07 MB |

By part (new objects): mac80211 545 KB, Bluetooth core 470 KB, cfg80211 353 KB,
rtw88 212 KB, SCSI 143 KB, crypto 183 KB, keys/X.509 48 KB, btusb+btrtl 83 KB,
usb-storage 41 KB. With the factory AVP running (~39 MB RAM) that is ~5 % less
free memory, all the time. **Guess (not measured):** with the dongle working,
another ~0.5-1 MB for Wi-Fi buffers (rtw88 keeps 4 x 32 KB receive buffers +
8 skbs) and wpa_supplicant (~1-2 MB user space).

## What usb-regs never reads (and why)

- `+0x02 INTRTX`, `+0x04 INTRRX`, `+0x0a INTRUSB`: interrupt status registers
  that are **cleared by reading** - a read would steal an interrupt from the
  running driver (a lost connect/disconnect or a stuck transfer).
- `+0x20..+0x5c` endpoint **FIFOs**: a read **takes data out** of the FIFO.
- `+0x10..+0x1f`, `+0x62..+0x67` (endpoint registers selected by INDEX - not
  useful), `+0x70..+0x77` (ULPI, not used), `+0x80..` (bus control, flat
  endpoint registers, DMA - whose interrupt register also clears on read).
- a controller (and its PHY) whose clock is off or which is held in reset
  (on some chips such a read hangs the bus). usb1 only with `--usb1`.
The program checks its own table against this list and refuses to run if it
ever contains one of them (the PC test proves that with two broken builds).

## Tests on the PC (cloud run 2026-10-10)

Rebased onto main 2026-10-10 evening (branch `claude/usbhost-rebased`, home PC,
WSL as root): `test-usbhost-pc.sh` **179/179** with the boot BusyBox (no fuller
BusyBox with `od` on this PC, so the BusyBox-ash Wi-Fi round was skipped), incl.
the new USB-network checks (stopped before host mode, not restarted while host,
rescue gadget first then `nzeus-usbnet-boot` once, refused while usbnet-up is
switching, not started without `usbnet-at-boot`); `test-debian-pc.sh`
**246/246**; `verify-avprpc.py --variant power/pure/net/sdroot` still pass on
copies of the existing builds. The kernel was NOT rebuilt after the rebase
(`hc16xx.c` changed: deferred core start-up now returns `-EPROBE_DEFER`), so
`--variant host` has not run on this version.

- `test-usbhost-pc.sh` with the boot BusyBox and the fuller BusyBox:
  **197/197** - syntax (dash, boot BusyBox ash, full BusyBox ash, no `[`);
  `usb-regs` on a fake `/dev/mem` (decoding, clock/reset guards, usb1 only on
  request, clear-on-read registers never printed, two broken tables refused,
  reproducible build); `nz-cdeject` (MIPS under QEMU and a PC test build: only
  `0bda:1a2b`, refuses a node that is not, two dongles, no storage interface,
  exact CBW bytes `1E` then `1B ... 02`, dongle leaving early = success); the
  trial with dash and the boot BusyBox ash on fake sysfs/configfs with a small
  "kernel simulator": refusals, PC never unplugged (nothing changes), unplug ->
  host -> automatic way back with the REAL `nzeus-usb-gadget`, confirm keeps
  host past the timer, `usb-host-off` during and after, cancel before the
  unplug, kernel refusing host, a failing way back saying "power off",
  `--force-vbus`, `--wifi` starting `wifi-up --wait 120`; status/phase 0 (read-only: nothing in the fake `/sys`
  changes); Wi-Fi (dash + full BusyBox): refusals (device mode, missing file,
  file readable by others, short password, no ssid, bad security), CD-mode
  eject, config root-only with hex SSID and exact quoted password (UTF-8,
  quotes, `$`, backslash), 64-hex key, Windows line ends, WPA3 = SAE + PMF,
  password never in output/logs, DHCP, SSH started, join timeout, option
  checks; `wifi-down`;
  `udhcpc.script` (dash + boot BusyBox).
- Kernel: built in the cloud at the pinned commit (copy of the sdroot build
  folder + `install-source.py` + the config above), `W=1` clean on both changed
  files (the PHY driver's old unused variable removed too); `build-host.sh` ran end to end on a scratch copy (only its three WSL
  paths changed) and its config step gives the identical `.config`;
  `verify-avprpc.py --variant host` passes, regression `keys power pure net
  sdroot` still pass, **29 mutants caught** (usb0 dr_mode host/peripheral,
  usb1 otg/enabled, extra usb0 property, usb0 disabled, lost blkdevparts, other
  node changed, extra BT_RFCOMM/MODULES/FW_LOADER_USER_HELPER, 11 missing
  options, version, initramfs list, sdroot ELF, a kernel with the OLD glue,
  changed `/usb-rescue`, BusyBox and SHA256SUMS copies) and an unchanged
  re-pack is accepted. `package-uart-boot.py --display-host` packaged it
  (scratch copy; the old direct-image hash file is not in the repo).
- Debian root: `sudo bash test-debian-pc.sh WORK FULL_BUSYBOX` **302/302**
  (was 264): + the three packages, `non-free-firmware` on every apt line, the
  usbhost copies/programs/wrappers installed unchanged, no `wifi.conf` in the image.
- Not run in the cloud: `emulate-entry.py --display-host` (no
  qemu-system-mipsel / gdb-multiarch there). **Nothing on hardware.**

## Not known yet (needs the console) - guesses are marked

- **VBUS:** does the console power the port in host mode by itself (charger/boost
  chip, no GPIO found)? Stock powers the dongle, so the hardware can. Whether it
  depends on anything software does: unknown. What happens with a PC attached
  in host mode: unknown - hence the rule.
- **Is the UTMI sequence enough for host mode?** It is Siarhei's (from the
  vendor), never run in host mode on this board. The stock driver may also use
  `ctrl_base` (0x18800000) registers we don't know (guess: Phase 0 (a) compared
  with (b) will show differences in UTMI/VBUS/PHY bytes; system-control bytes
  outside the clock/reset words are not read).
- **VBUS sensing:** whether the core sees "VBUS valid" in host mode without
  `--force-vbus` (guess: yes, if the boost chip really puts 5 V on the port).
- **Re-creating the core** at run time: the MUSB core's remove path is meant
  for module unload; it has never been exercised on this SoC. The kernel falls
  back to peripheral on failure, but a hang would need a power-off.
- PIO-only USB (no DMA): speed estimate storage 5-15 MB/s, Wi-Fi 15-50 Mbit/s
  (guess). Full-speed/low-speed devices (keyboards) through the MUSB root port
  without a hub: expected to work (guess).
- `firmware-realtek` in bookworm contains `rtw88/rtw8821c_fw.bin` and
  `rtl_bt/rtl8821c_fw.bin` (expected from the package's file list; the image
  was not built in the cloud). Installed size about 6 MB on the 4 GB root (guess).
- WPA3 (SAE) with rtw88 through mac80211: expected to work, untested.
- usb1: probably not wired (nothing ever appeared on it); left off.

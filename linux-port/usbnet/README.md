# USB network for the modern-Linux console (design + how to test)

Goal: real TCP/IP between the console (Linux 7.2 port) and the Windows PC over
the USB cable that is already plugged in, so SSH/HTTP work natively - **without
losing the USB serial rescue shell** (COM9, `NZEUS-RAM-RESCUE-01`), which is our
only way in.

Status 2026-10-09: **PC-tested only. Nothing here has run on the console yet.**

## The short version

- The boot gadget made by `/usb-rescue` (serial only) is **not changed at all**.
  The initramfs stays byte-identical (checked by `verify-avprpc.py --variant net`).
- `usbnet-up.sh`, sent with `rescue-command.py` when you want it, builds a
  **second** gadget = the same kind of serial port + a network card, and swaps
  the USB controller over to it. The console gets `192.168.77.1/24`, the PC
  `192.168.77.2/24` (no gateway, so the PC's internet is not touched).
- **Automatic way back:** unless `usbnet-confirm.sh` reaches the console through
  the NEW serial port within 180 s, the console swaps back to the untouched
  rescue gadget by itself. A power cycle always gives the plain rescue gadget
  back (everything is in RAM; no card or flash writes anywhere).
- **Two network kinds:**
  - **NCM** (recommended, standard USB class, Windows 11 built-in driver) - needs
    the new kernel `gma-native-net` (= pure + `CONFIG_USB_CONFIGFS_NCM`).
  - **RNDIS** (Microsoft's own protocol, Windows 10 and 11 built-in driver) -
    **already in every gma-native kernel, including the one on the card now.**
    So the first hardware test needs **no new kernel and no card write**.
  - `usbnet-up.sh` with no argument (`auto`) uses NCM if the kernel has it, else RNDIS.

## Why this design (and not "just add NCM to /usb-rescue")

| | A: composite gadget at boot (edit `/usb-rescue`) | **B: separate gadget, switched at run time (chosen)** |
|---|---|---|
| Boot path | changed: new initramfs, new kernel install | unchanged, byte-identical |
| If Windows/driver/musb misbehave | lifeline broken **at every boot** until the card is rolled back | only after you run it; auto-revert after 180 s; power cycle = clean |
| First test | needs a card write | none: RNDIS works on the current kernel |
| Cost | always on | a few commands each boot |

A only becomes reasonable after B has worked on hardware for a while.

## How Windows identifies the rescue port - and why the PC tools changed

`rescue-command.py` / `rescue-upload.py` used to accept exactly one serial port
whose `Win32_SerialPort.PNPDeviceID` **equals** `USB\VID_1D6B&PID_0104\NZEUS-RAM-RESCUE-01`.
That string is the Windows *device instance path*: `USB\VID_xxxx&PID_yyyy\` plus the
USB serial number. It works because the rescue gadget is a single-function device
(device class 0x02): Windows binds `usbser.sys` to the whole device.

A gadget with two functions must say "composite" (device class EF/02/01, one
IAD per function). Windows then loads `usbccgp.sys` for the device and makes one
child per function, named after its first interface:
`USB\VID_1D6B&PID_0104&MI_00\6&<hash>&0&0000`. **The serial number is not in the
child's name**, so the old exact check would (safely) refuse the new port.

Minimal safe change (done): new `diagnostics/rescue_port.py`, used by both tools.
It accepts
1. exactly the old plain identity (unchanged behaviour), or
2. a serial port named `USB\VID_1D6B&PID_0104&MI_..` whose **parent device**
   (`DEVPKEY_Device_Parent`) is exactly `USB\VID_1D6B&PID_0104\NZEUS-RAM-NCM-01`
   or `...\NZEUS-RAM-RNDIS-01`,

and refuses if zero or more than one port match. Never by VID/PID or COM number
alone. If the parent lookup fails, case 1 still works exactly as before.
`test_rescue_port.py` checks the rules with made-up device lists (10/10).
(`display/capture-next-boot.py`, `display/save-pixel-backup.py` and
`card-test/observe_usb.py` still accept only the plain identity - fine, they are
used in plain mode.)

Why the network gadget has its **own serial number and bcdDevice**
(`NZEUS-RAM-NCM-01`/0x0101, `NZEUS-RAM-RNDIS-01`/0x0102): Windows remembers
drivers per device instance (VID+PID+serial). Re-using `NZEUS-RAM-RESCUE-01` with a
different set of functions would make Windows keep its old `usbser` choice for a
device that now looks completely different (commonly needs "Uninstall device" to
fix). Windows also caches "does this device have Microsoft OS descriptors" per
VID+PID+bcdDevice (registry `usbflags`), which RNDIS needs. Separate identities
mean the plain rescue port's Windows setup is never touched. Expect a new COM
number (e.g. COM10/11) in network mode - the tools find it by identity anyway.

## Windows drivers

- **NCM**: `UsbNcm.sys`, built into **Windows 11**; binds automatically to the NCM
  class (02/0D). Some Windows 10 builds also contain `UsbNcm.sys`, but automatic
  binding there is not something we can rely on - on Windows 10 use RNDIS unless
  `pc-setup.ps1` shows the NCM card working. `pc-setup.ps1` prints whether the
  file exists.
- **RNDIS**: Windows 10 and 11 built-in ("Remote NDIS Compatible Device"). We send
  Microsoft OS descriptors (compatible id `RNDIS`, sub id `5162001`) so Windows picks
  it without an .inf - the same values as the stock NZEUS Deck "PC connection".
  RNDIS is put FIRST in the gadget (Windows' long-standing expectation), the serial
  port second (so its name ends in `MI_02`). RNDIS is a Microsoft-only protocol
  that Linux developers consider insecure by design; acceptable on a private cable
  to our own PC, but NCM is the long-term choice.
- **ECM / EEM**: Windows has no built-in driver -> not useful here (both work on
  Linux/macOS; ECM is enabled in the kernel anyway).

## Addresses

Console `192.168.77.1/24` on `usb0`, PC `192.168.77.2/24`, no gateway, no DHCP (the
BusyBox on the console has no `udhcpd`). Fixed MACs (`02:4e:5a:01:00:01/02` NCM,
`02:4e:5a:02:00:01/02` RNDIS) so Windows sees the same card every time and keeps
the address after the first setup. If your home network already uses
192.168.77.x, change `CONSOLE_ADDR`/`PC_ADDR` in `usbnet-up.sh`, `ADDR` in
`dropbear/dropbear-start.sh` and the `pc-setup.ps1` parameters.

## Files

| File | What |
|---|---|
| `usbnet-up.sh` | console: build + switch to the serial+network gadget, IP, auto-revert; `--confirm`, `--status`, `--down` |
| `usbnet-confirm.sh` | console, sent via `rescue-command.py` on the NEW port: "lifeline works, stay" |
| `usbnet-down.sh` | console: back to the plain rescue gadget |
| `pc-setup.ps1` | PC: find the console's card (by parent identity), show + (after YES, as admin) set 192.168.77.2/24, ping, check port 22 |
| `usbnet.config` | the kernel config fragment for `gma-native-net` |
| `test-usbnet-pc.sh` | PC test of the console scripts with the exact boot BusyBox under qemu-mipsel |
| `dropbear/` | SSH server built for the modern kernel + start script (see `dropbear/README.md`) |
| `../display/build-net.sh` | builds kernel `gma-native-net` (pure + NCM), verified by `verify-avprpc.py --variant net` |
| `../diagnostics/rescue_port.py` (+ `test_rescue_port.py`) | exact port finder used by `rescue-command.py` / `rescue-upload.py` |

## Hardware test plan (not done yet; I watch; one step at a time)

Only RAM changes; no card writes in steps 1-3.

0. PC: `bash linux-port/usbnet/test-usbnet-pc.sh` (45/45) and
   `python linux-port/diagnostics/test_rescue_port.py` (10/10).
1. Console booted as usual, COM9 works. Note Device Manager state. Then:
   ```
   python linux-port\diagnostics\rescue-command.py linux-port\usbnet\usbnet-up.sh --seconds 20
   ```
   Expect `USBNET_STARTED ... mode=auto`. 3 s later COM9 disappears; the console
   comes back as `NZEUS-RAM-RNDIS-01` (current kernel = RNDIS). The UART log/screen
   show `usbnet:` lines. Wait until Device Manager shows a new COM port and a
   network card (first time: Windows installs drivers, up to ~1 minute).
2. Within 180 s:
   ```
   python linux-port\diagnostics\rescue-command.py linux-port\usbnet\usbnet-confirm.sh
   ```
   Expect `USBNET_CONFIRMED`. **If this fails, do nothing: the console goes back to
   COM9 by itself after 180 s.** (Evidence for the log: what Device Manager showed.)
3. PowerShell **as administrator**:
   ```
   powershell -ExecutionPolicy Bypass -File linux-port\usbnet\pc-setup.ps1
   ```
   It shows what it would change, asks for YES, then pings `192.168.77.1`.
4. Back to plain: `python linux-port\diagnostics\rescue-command.py linux-port\usbnet\usbnet-down.sh`
   -> COM9 (`NZEUS-RAM-RESCUE-01`) returns. Run `rescue-command.py` once more to prove it.
5. SSH: see `dropbear/README.md`.
6. Later, only after `gma-native-pure` is proven on hardware: `build-net.sh`, then
   the normal card workflow for profile `gma-native-net` (recovery -> pure) and
   repeat 1-4 - `auto` then picks NCM (Windows 11).

## What is NOT known yet (risks)

- **musb unbind/re-bind on this SoC** has never been done. It is what happens on
  a cable unplug/replug, but the configfs `UDC` write path is untested here. If
  the controller does not come back, the auto-revert cannot help (it uses the same
  path): **power-cycle** (safe: RAM-only kernel; the SD card is not mounted rw).
  The Uno UART log still shows what happened (`usbnet:` lines go to /dev/console).
- Endpoints: the musb has 5 IN + 5 OUT endpoints (512-byte FIFOs); serial + NCM
  or RNDIS needs 4 IN + 2 OUT - should fit, not yet seen on hardware.
- Windows: first-time driver install time, the RNDIS-first order, UsbNcm on the
  exact Windows build - only the console+PC test will tell.
- The old rescue shell loop keeps re-opening `/dev/ttyGS0` while its gadget is
  unplugged (it just waits, as after a cable unplug).
- `gma-native-net` sits on `gma-native-pure`, which is itself not hardware-tested.
  The RNDIS path does not need it.
- Not done: card profile/packaging for `gma-native-net` (follow the pure/power
  profiles), Windows Internet Connection Sharing (it forces 192.168.137.x; would
  also need a default route + DNS on the console), DHCP server, IPv6 setup
  (link-local fe80:: comes up by itself but is untested).

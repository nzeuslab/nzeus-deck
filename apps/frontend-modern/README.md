# nzeus-modern: NZEUS Deck on the new Linux 7.2 kernel

## What it is

NZEUS Deck on the console's Linux 7.2 (the SD Debian) is **one static
program**, `nzeus-modern`: the NZEUS core (`apps/nzeus-gui`) linked straight
into a small frontend (`nzeus-modern.c`). No shared libraries, no dlopen, no
maker code. (The old "NZEUS Deck 0.9" for the factory 4.4 kernel - picoarch,
the stock devices, nzeus-netd - was removed from the core on 2026-10-13.)
It only uses standard Linux interfaces:

| What | How |
|---|---|
| Screen | `/dev/fb0` "hc16xx-gma-argb", 640x480, same checks as `ports/r36s-hichip/input/box-demo.c`; switched on with FBIOBLANK UNBLANK, handed back to the firmware (POWERDOWN) at the end, on Ctrl+C / SIGTERM and on a crash |
| Buttons | `/dev/input/event*` "r36s-keys" + "serializer-keys" (and "r36s-power-button", logged only) |
| Battery, charger | `/sys/class/power_supply/battery/capacity`, `.../charger/online`, raw levels in `/sys/bus/platform/devices/188184d0.power/stock_levels` |
| Brightness | `/sys/class/backlight/r36s-avp/brightness` (0..255, NZEUS's own percent curve); without it, any other `/sys/class/backlight/*` device (e.g. r36s-pwm), scaled to its `max_brightness` |
| Sound | the ALSA card "R36S": raw ALSA ioctls on `/dev/snd/pcmC0D0p`, volume = "Master Playback Volume"; speaker switch = the sound driver's sysfs `headphones` |
| Power off / restart | Debian's `poweroff` / `reboot` (restart only with the watchdog-restart kernel); without Debian `sync()` + `reboot(RB_POWER_OFF)` |
| Network | the kernel's own TCP/IP; "online" = a default route in `/proc/net/route` |
| Web | NetSurf (`apt install netsurf-fb`) full screen through `nzeus/bin/nzeus-browser` (`hw_fullscreen_run`) |
| Files | `/tmp/nzeus-data` (`nzeus-deck` links it to `/data/nzeus/deck`) |

The hardware code is `apps/nzeus-gui/hw_modern.c` (devices) and `hw.c`
(restart/power-off rules, the clock keeper). The PC tests
(`build_nzeus_gui.sh pc`) build the same core with fake files for all of
these.

## Buttons

| Console | Linux key | NZEUS |
|---|---|---|
| D-pad / left stick | KEY_UP/DOWN/LEFT/RIGHT | move |
| A / B / X / Y | KEY_LEFTCTRL / KEY_LEFTALT / KEY_SPACE / KEY_LEFTSHIFT | A / B / X / Y, as on the stock system |
| L1 / R1 | KEY_TAB / KEY_BACKSPACE | L1 / R1 |
| Select / Start | KEY_ESC / KEY_ENTER | Select / Start (Select + L1 = screenshot) |
| L2 / R2 | KEY_PAGEUP / KEY_PAGEDOWN | passed on (NZEUS doesn't use them) |
| Vol+ / FN | KEY_VOLUMEUP / KEY_MENU | logged only |
| Power | KEY_POWER | logged; **held 3 s the kernel switches off** (power driver kernel) |

## What works and what doesn't (be honest)

Tested **on the PC only** (2026-10-09): built with Ubuntu's MIPS cross
compiler, run under `qemu-mipsel` with `--selftest`: the home screen draws
(`reports/gui/modern-home.png`), Right/Left through the same button path move
the selection, every pixel has alpha 0xff, and an idle screen copies under one
row a picture. The battery/charger/backlight file reading is tested on the PC
with fake files. **Nothing has run on the console yet.**

Works (expected, once tried on the console):
- the home screen, desktop mode, the built-in apps that need no network or
  sound: Terminal (BusyBox `sh`), Files, Editor, Notes, Calculator, Clock,
  Images, Task manager, Settings...
- battery % and charging sign, brightness and power-off **only on a kernel
  with the power driver** (`gma-native-power`, not yet proven). On today's
  kernel (`gma-native-keys`) the battery shows nothing, the brightness slider
  says "no backlight device here" and Power off says "not possible here" -
  nothing breaks.

Doesn't work yet:
- **sound** (no sound driver on the new kernel; music plays silently, volume
  says "no sound device here")
- **Wi-Fi, Bluetooth, internet**: no drivers yet. Web and Claude say
  "No network: the new Linux has no Wi-Fi driver yet". Wi-Fi/Bluetooth/USB
  sections in Settings find nothing.
- the game menu (TreeFrogUI / stock menu) and "Reboot" (not tried on the new
  kernel: it might just stop with the screen on, so it is switched off)
- Python, Arduino, Car and other apps that need tools from the card's
  `nzeus/bin`: that folder is not there (the card is never used read-write)
- everything NZEUS saves (settings, notes, screenshots, clock.txt) is in RAM
  and **gone after a power-off**. The Files app still calls the top folder
  "SD card" - it is `/tmp/nzeus-data`.

## Files and the SD card

- `NZ_DATA=/some/folder`: keep NZEUS's files there instead (a link
  `/tmp/nzeus-data` -> that folder). Refused on a FAT file system (that would
  be the card) or a folder that can't be written.
- The factory FAT partition only holds boot files and is not shown inside
  NZEUS (an old `sdcard-ro` link from earlier versions is removed at start).

## Build (my PC, WSL)

```
wsl -d Ubuntu -- bash build_nzeus_modern.sh
```
-> `apps/frontend-modern/mips/nzeus-modern` (static, stripped; uses the
buildroot toolchain `~/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot` and
LVGL 8.3.1 in `~/r36s/src/lvgl`, like `build_nzeus_gui.sh`). Flags:
`-O2 -march=mips32r2 -mtune=74kc -mhard-float -EL -static`.
The script checks that the file is static and under 4 MiB (the most
`rescue-upload.py` takes).

Other compilers: `CC=mipsel-linux-gnu-gcc bash build_nzeus_modern.sh` (Ubuntu's
cross compiler) -> `mips/nzeus-modern-cloud`. Add `test` at the end to also
run the PC tests (hw_modern.c with fake files, and the qemu self-test that
saves `reports/gui/modern-home.ppm`/`.png`).

`mips/nzeus-modern-cloud` in git was built in the cloud with the Ubuntu cross
compiler (gcc 12, glibc 2.39) **for testing only**. For the console, build
`nzeus-modern` with the buildroot toolchain on the PC.

## Run on the console

Only on a `7.2.0-rc5-nzeus-gma-native-*` kernel (the scripts refuse
anything else), and with nothing else using the screen (box-demo,
fb-pattern...). Everything is in RAM: no card, flash or raw-device writes.

1. Upload (once per boot; the upload tool refuses a name that already exists):
   ```
   python ports/r36s-hichip/diagnostics/rescue-upload.py apps/frontend-modern/mips/nzeus-modern --name nzeus-modern
   ```
   -> `/tmp/nzeus-tools/nzeus-modern`. `/tmp` is RAM: the file takes about
   3 MB of the ~39 MB, plus what NZEUS uses while running (about 3.5 MB of
   code and fixed data, plus LVGL's and the apps' memory - not measured on the
   console yet: `check-modern.sh` shows `MemAvailable`).
2. Start (in the background, 600 s by default - edit `LIMIT` in the script
   for longer, at most 3600):
   ```
   python ports/r36s-hichip/diagnostics/rescue-command.py apps/frontend-modern/run-modern.sh --seconds 20
   ```
   The log is `/tmp/nzeus-modern.log`: `DATA ...`, `BUTTONS ...` (should list
   r36s-keys and serializer-keys), `NZEUS_MODERN_ACTIVE`, then `KEY ...` for
   each press and `ALIVE` every minute.
3. While it runs: `check-modern.sh` (read-only: running?, log, memory, AVP
   blank count). Stop early: `stop-modern.sh` (SIGTERM; NZEUS closes and the
   screen goes back to the firmware: `FIRMWARE_LAYER_RESTORE_REQUESTED`).
   At the time limit it stops the same way by itself.

Without a screen: `/tmp/nzeus-tools/nzeus-modern --selftest /tmp/m.ppm` runs
NZEUS in memory only (no fb, no buttons) and prints PASS/FAIL lines.

Things to look at on the first console run: the home screen in the right
colours (photo), every button (the log's `KEY` lines), the clock ticking,
that the picture survives the AVP blanks (`check-modern.sh`), the Terminal,
memory (`MemAvailable`), and that `stop-modern.sh` gives the screen back.

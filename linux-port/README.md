# NZEUS: migration to standalone Linux

**Current status (2026-10-11): [STATUS.md](STATUS.md).** Debian 12 now boots
from SD on Linux 7.2.0-rc5 with about 117 MiB usable RAM, native networking
and speaker audio. The dated milestones below are historical.

Updated 2026-10-08. My goal: more memory for applications, native
TCP/IP, full Linux tools, standard drivers and terminals, and eventual SD-card boot
without a PC. This is a kernel/board port, not another stock-system optimization.

**Latest verified milestone:** Linux 7.2.0-rc5 boots from SD into BusyBox userspace
on this E3100 console, with UART logs captured through the Uno at 19200 baud.
The USB rescue script's missing-`[` bug is fixed: its `test` replacement now boots
to an interactive USB shell. Basic PTY allocation, native IPv4 loopback and timer
checks passed. MemTotal39588KiB still uses the conservative memory map. No NOR
flashing, extra RAM, display, sound or Wi-Fi have been enabled.
Evidence: diagnostics/uart-usb-fix/RESULT.md and kernel-dmesg.txt.
Display work (2026-10-08): a separate firmware-timed 640x480 RGB565 framebuffer
driver and ten-second color-bar diagnostic now compile for the modern kernel.
PC checks preserve the working USB rescue files and board settings. Display
activation is manual; no new image has been installed and no screen success is
claimed. See display/README.md and display/build/verification.json.
After reassembly, USB reconnected successfully. Live framebuffer discovery and
temporary firmware-buffer color-bar tests passed exact memory restoration;
user photo confirms visible bars with incorrect colors. A color-conversion bypass
comparison passed restoration, with visual response pending. Native bidirectional PTY and loopback TCP
tests also passed with fuller RAM-only tools. See display/evidence/RESULT.md.
The older experiment chronology below records the earlier unconfirmed stages.

**New SD-path finding (2026-10-07):** the factory bootloader's ChaCha20 image wrapper
and CRC checks are now decoded. Its RAM-mirror mode selects `advapi32.dll` as the
kernel, and it merges selected SD DTB properties into the NOR base. A modified
stock kernel successfully booted from SD: live `/proc/version` contained
`NZEUS7@hichip01`, with a new boot ID and the NZEUS home screen confirmed on the console.
After that stock-marker test, the original SD kernel was restored and hash-verified.
No NOR writes were performed.
See [live test evidence](boot-analysis/sd-probe/live-test-20261007/RESULT.md).
The modern rescue image was written and readback-verified on the test SD card.
Its first test produced a logo followed by a blue screen, with no rescue USB
terminal during the bounded observation. Modern boot remains unconfirmed;
the original card blocks are now restored and readback-verified. ADB confirms the
original `4.4.186-release` kernel, original SD kernel hash, and running NZEUS app.
See [test result](card-test/RESULT.md)
and `card-test/deployment.json`. The original card image is preserved.

**Diagnostics update (2026-10-08):** the rescue shell runs on the actual CPU under
stock Linux. PC CPU emulation verifies decompression, kernel entry and selection
of the appended DTB. A direct-load variant of the same kernel is prepared to
isolate the second decompression stage. A fresh backup is complete and this
direct-load image was installed, with all 134 changed blocks readback-verified.
It stayed at the startup logo without a rescue USB device. All 134 original card
blocks have now been restored and readback-verified. The separate stock-kernel
handoff capture successfully executed custom entry code at 0x81000000 and reported
CPU startup values through ADB. Its original SD kernel is restored and hash-verified;
the subsequent original boot is verified. A larger wrapper containing identical
decompressed code also booted successfully, ruling out a simple 4 MiB file limit.
The modern-kernel Config0-bit-17 entry comparison timed out without rescue USB.
The screen progressed from logo to blue. The original kernel has since been
restored with all 70 repair blocks readback-verified; stock reboot verification passed.
Recovery instructions: diagnostics/config-bit17/READER-RECOVERY.md.
See [diagnostics and limits](diagnostics/REPORT.md) and the separate
[direct-load test plan](diagnostics/direct-card-test/TEST-PLAN.md).

**Current status (2026-10-08): modern Linux booted from SD into its rescue shell.**
The Uno received the entry markers, Linux 7.2.0-rc5-nzeus-experimental+, /init
banner and BusyBox ash prompt. No NOR changes were needed. UART output works at
19200; USB rescue hit a missing-`[` shell-command bug now being repaired. This
does not yet provide a working display, sound, Wi-Fi or reclaimed RAM.
See diagnostics/uart-boot/RESULT.md and its saved console-boot.txt.

Prior status: the stock interrupt-state diagnostic booted and reported zero
external interrupt-enable banks, so that proposed cause is unsupported. The
original SD kernel file is restored and hash-verified; the current session still
runs the stock diagnostic until reboot. See diagnostics/irq-probe/RESULT.md.
Next priority is early modern boot logging; an available development board may
provide serial access, depending on its model. Modern boot is still unconfirmed.

## Built on the PC

- A pinned source snapshot from Siarhei's `linux-hichip`, branch `hc16xx-7.2`, commit
  `da5f1a22ef2ff5c46aa3f750fa10825813970b09` (upstream commit date 2026-08-31).
  The small `upstream/` reference snapshot includes board source, required headers,
  baseline config and README. `upstream-manifest.json` records their SHA-256 hashes.
  The full pinned kernel checkout lives in WSL at
  `/home/nzeus/r36s-linux-port-20261007`; upstream licenses remain in their files.
- `r36s-e3100-bringup.dts`: a conservative, serial-first experimental board description.
- Linux **7.2.0-rc5** compiled for little-endian MIPS32r2, with native IPv4,
  UNIX98 terminals and a built-in rescue filesystem. `bringup.config` was merged
  and resolved; `artifacts/kernel.config` is the actual build configuration.
- A static BusyBox 1.37.0 rescue shell, built from the official checksum-verified
  source. Its shell runs under QEMU MIPS user emulation on the PC. This checks
  executable compatibility, not booting the kernel or emulating this board.
- `validate-board.sh`: compiles both the upstream board and our draft and checks the
  final RAM range, boot arguments, USB role and SD clock. Logs remain in `build/`.
- `evidence/`: hardware readings and the running stock tree pulled through ADB.

**The UART-instrumented experimental image has booted into a rescue shell.
This is not a complete desktop distribution or a ready-to-use SD image.
The stock marker passed; the first modern USB-only test did not expose its rescue terminal. Never replace stock `cubegm/dtb.bin` or
`Bubbles.scr` with this DTB.**

PC-only commands from the project folder (the build uses the pinned WSL checkout):

```powershell
wsl -d Ubuntu -- bash linux-port/validate-board.sh
wsl -d Ubuntu -- bash linux-port/build-experimental.sh
wsl -d Ubuntu -- python3 linux-port/verify-artifacts.py
```

Build work files are in `/home/nzeus/r36s-linux-port-work-20261007`. The build script
contains no upload, flashing or reboot command. `--export-only` exports an existing
build and reruns its artifact checks.

### Output and validation

| Artifact | Purpose |
|---|---|
| `artifacts/nzeus-r36s-experimental-with-dtb.bin` | Experimental compressed kernel plus board tree; current size is in the verification log |
| `artifacts/nzeus-compressed.elf` | Linked decompressor layout used to verify the board-tree position |
| `artifacts/nzeus-kernel.elf` | Uncompressed kernel and load layout |
| `artifacts/initramfs.cpio.gz` | Built-in rescue filesystem, 50 entries; also exported for inspection |
| `artifacts/busybox-mipsel` | Static rescue utility executable |
| `artifacts/SHA256SUMS` | SHA-256 hashes of build artifacts and resolved configurations |
| `build/artifact-verification.txt` | Latest successful PC verification results |

The verifier checks artifact hashes, exact raw-image contents against the linked
ELF, DTB placement at the linked `.appended_dtb` offset, entry address `0x81000000`, and load regions
within the conservative RAM window. It checks native IPv4/PTYS configuration and
the exact rescue script and executable inside the initramfs, then runs the MIPS
shell and checks startup-script syntax under QEMU.

The rescue script starts a UART shell and attempts to expose an ACM terminal through
the existing USB cable (`NZEUS Experimental Linux Rescue`, serial
`NZEUS-RAM-RESCUE-01`). This could provide a root shell without a UART adapter if
the kernel reaches USB startup. The first hardware test did not expose this USB
terminal. Early failures before USB startup are invisible over that cable.
It does not mount the SD card or flash, configure a network link, or start SSH/ADB.
USB networking is compiled in but still needs board testing and userspace setup.

The build completed with upstream warnings: 13 inherited device-tree naming
warnings, two unused variables in the USB PHY driver, and a compiler overread
warning involving the decompressor's linker-defined `__image_end` boundary.
Logs are retained in `build/`; a successful compile does not settle runtime behavior.

## Stock-console evidence captured on 2026-10-07

ADB serial `NZEUS-R36S`. The SD tools check the card's CID (serial); pass your own card's with `--cid`.

| Item | Evidence on 2026-10-07 |
|---|---|
| Kernel | `4.4.186-release`, build #23, 2026-01-27 |
| CPU | MIPS 74Kc, little-endian project/toolchain, MIPS32r2 with FPU |
| Linux memory | `MemTotal: 43160 kB`; physical range `0x00000000..0x02f91e4f` |
| Swap | 16 MB zram at priority 100 plus approximately 128 MB SD swap |
| USB cable | Active gadget bound to `musb-hdrc.0.auto` |
| Original boot tree restored | Live tree and `Bubbles.scr`: SHA-256 `69b97c429782cf739629a8cb919446483b8b35f96964ad79cc0caedcd600bfb6` |
| Alternate factory tree | `dtb.bin`: SHA-256 `1fe3c74f776ec2991cde0c13b041ab92cf04f744b33eb1fad2de9f32f66bf543` |

The board identification, DDR marking and pin map are recorded in
`../share_lis82/r36s-e3100/README.md` and the saved factory device trees.

## Correct expectations

- **Memory:** newer project notes report a live mirror at +128 MB. This session did
  not repeat that physical-memory experiment. Treat 128 MB as the provisional total
  addressable budget, not as memory already available to Linux. Kernel, DMA, display,
  and any remaining second-processor reservations reduce what applications can use.
  Do not promise 192 MB or a specific two/three-times gain on this board.
- **Networking:** the stock kernel has packet/UNIX sockets, but lacks IPv4/TCP.
  `nzeus-netd` supplies TCP/IP in user space today. A new kernel can provide normal
  sockets; a functioning network device and normal userspace are still required.
- **Programs:** Python 3, curl, SSH, git, vim and tmux are reasonable MIPS build targets.
  Package availability and the CPU ABI must be checked for the selected root filesystem.
  Native compiling will be memory-limited. Current Node.js does not list 32-bit MIPS
  as supported; it is not an acceptance requirement or a promised package.
- **Drivers:** an upstream class driver alone does not make the HiChip controller
  work. USB host is a prerequisite for this Wi-Fi/BT dongle, USB audio, Ethernet,
  webcams and MIDI. WPA3 also needs firmware/driver and userspace support. Bluetooth
  headphones additionally require the appropriate audio software and profiles.
- **Boot:** ADB runs after stock Linux starts. It is not the Boot ROM/SPL loader and
  does not supply recovery after an early boot failure. Reading an ext4 SD root
  filesystem after USB boot is not the same thing as booting independently from SD.

## Current migration path and boot boundary

The target remains modern Linux and a fully usable handheld. See the updated
[compatibility findings and milestone plan](compatibility/MIGRATION.md).
The stock 4.4 SDK is a reference and possible diagnostic tool; rebuilding it is not
an unconditional prerequisite or a guarantee of compatibility with newer AVP firmware.

The no-flash SD route is proven for a modified stock kernel. A flash programmer is
not required for PC preparation or that reversible SD-file test. Modern-kernel
handoff is still unverified, and a failed modern boot may require removing the SD
and restoring the original file with the PC reader. UART would help diagnose early
failures, but I don't have the adapter. No NOR modification is planned.

The first modern target is a USB rescue terminal with the current RAM range and
stock DTB/AVP files preserved. `clk_ignore_unused` temporarily prevents generic
clock cleanup from disrupting firmware-owned hardware. Buttons, backlight, panel,
audio, battery and power-hold are not implemented in the current board profile.
After rescue boot: native networking/PTYS/storage, handheld drivers, USB host and
wireless, measured RAM reclamation, ordinary Linux userspace, and daily-use tests.

`package-modern-sd.py` now creates a PC-only encrypted gzip uImage containing the
modern self-decompressing image and appended board tree. Source hashes, load/entry,
CRC checks, cipher roundtrip, exact decompression and factory bootmem non-overlap
pass. The candidate is under `artifacts/sd-candidate/`; its report explicitly states
that hardware handoff is unverified. It has now been installed on the test SD, with every changed block verified;
modern boot success is still pending.

The factory Linux entry arguments were traced in the actual binary: UHI `(-2,
DTB, 0, 0)`. Modern source selects its appended board tree first. The actual factory cache-writeback and interrupt-masking calls have been traced.
The minimal rescue profile uses CPU-local timers and leaves shared timers, SD/MMC,
GPIO/input and PWM disabled to reduce AVP coexistence risks. These measures permit
a controlled test; they do not prove runtime hardware compatibility. A packaging pass alone
is not sufficient. No modern boot, extra RAM or modern handheld drivers are proven.

The separate `ram-handoff/` diagnostic module tested one allocated page on stock
Linux and exited. It is not a kernel loader. The newly proven SD path is now the
primary boot investigation; in-memory relocation work is not required for it.

## Upstream sources checked

- [HiChip kernel and driver status](https://github.com/SiarheiVolkau/linux-hichip/tree/da5f1a22ef2ff5c46aa3f750fa10825813970b09)
  — partial SD and USB gadget support; no USB host; display/DSI/audio unfinished.
- [SF3000 boot research](https://github.com/SiarheiVolkau/sf3k-hacking)
  — USB loader development and early UART boot path; not R36S hardware validation.
- [Upstream board](https://github.com/SiarheiVolkau/linux-hichip/blob/da5f1a22ef2ff5c46aa3f750fa10825813970b09/arch/mips/boot/dts/hichip/sf3000.dts)
  — contains a 256 MB description and `mem=192M`; neither is copied into our effective map.
- [Node.js platform support](https://github.com/nodejs/node/blob/main/BUILDING.md#platform-list)
  — no supported 32-bit MIPS target in the current list.

Older `ROADMAP.md`/`MORNING_REPORT.md` describe earlier milestones; they are not a
current completion checklist. Newer source and live evidence already show native
NZEUS apps, network workaround, remote access, Fast SD, Bluetooth code and zram.

# Getting Linux 7.2 onto a $40 handheld: what I found

*NZEUS LAB, October 2026. Device: R36S V2.7 clone (green), board HC16E3100V20.*

The R36S is a cheap retro-gaming handheld. Most R36S units use a Rockchip chip
and run community Linux images. My unit is a **clone**: a HiChip E3100 (MIPS
74Kc) running the maker's own stock Linux 4.4. None of the usual images work on
it. This is how I found out what was inside and got a modern Linux running on
it, without ever writing the console's flash chip.

## 1. Looking inside without breaking anything

Ground rules I kept the whole way:

- **Never write the NOR flash** (the 512 KB chip holding the bootloader). If
  that breaks, the console is a brick without a hardware programmer.
- **Back up the SD card before every change.** Write changes block by block and
  read every block back to check it. Keep a tested way back for each change.
- **No success claims without hardware evidence**: a serial log, a USB
  identity, a photo, or a reading from the device.

Tools: an Arduino Uno as a **receive-only** serial (UART) logger at 19200 baud
on the board's debug pads, a USB cable, a multimeter, and a PC.

## 2. What the hardware really is

| | Finding |
|---|---|
| CPU | MIPS 74Kc little-endian, MIPS32r2 with hardware FPU |
| RAM | 256 MB DDR3 chip; the stock Linux only sees about 42 MB |
| Flash | 512 KB NOR (bootloader); everything else lives on the SD card |
| Screen | 640x480 MIPI-DSI, plus a mini-HDMI port |
| Sound | Owned by a **second processor** (the "AVP"), not by Linux |
| Stock kernel | Linux 4.4, no TCP/IP networking, no temperature sensor |

## 3. How the console boots, and why I could replace the kernel

Reading a backup of the NOR flash showed how the factory bootloader starts the
system:

1. It reads the kernel from the SD card's first partition, as a file named
   `cubegm/advapi32.dll` (a Windows-looking name on a Linux device).
2. The kernel is a normal Linux `uImage`, **wrapped with ChaCha20 encryption**
   plus a CRC32 checksum.
3. The key comes from the bootloader itself.
4. I found **no signature check**. Any kernel wrapped the right way will start.

From a security point of view this means there's no secure boot. Anyone with
the SD card can run their own kernel. On a $40 game console that's what lets
an owner run their own software, so I don't count it as a vulnerability. But it
does mean the encryption only hides the kernel; it doesn't protect it.

Another trick: the stock SD card's first sector is **XOR-scrambled with
`DEADBEEF`**, so Windows can't read its partition table. Formatting the card
from Windows would wipe the console's system. My card tools unscramble that sector to read
the real partition table (`linux-port/rootfs/region-tool/`).

## 4. Bringing up a modern kernel

Siarhei Volkau's **linux-hichip** project already had Linux running on related
HiChip chips. Starting from his `hc16xx-7.2` branch, the work for this board was:

- **A RAM-only rescue shell first.** The first kernels ran entirely from RAM
  and gave a root shell over USB. A bad kernel couldn't damage the card.
- **USB that actually appears.** An early "no USB" result turned out to be my
  own startup script refusing kernels with new names. It wasn't a driver
  failure. Lesson: check your own tools before blaming the hardware.
- **Screen colours.** The picture was correct for about two minutes and then
  turned green and purple. The stock firmware runs a display timeout that
  changes the output. Our own display driver fixed it.
- **Sound.** The speaker belongs to the AVP co-processor. The stock audio
  device turned out to be a dead end. Working sound needed the AVP's own
  packet format, then a native Linux sound card driver (`linux-port/audio/`).
- **More memory.** Stopping the AVP's memory reservation and handing it to
  Linux raised usable RAM from about 39 MiB to about 117 MiB, proven at boot
  on the real console.
- **A floating-point fault.** At the factory clock speed (1188 MHz), the
  hardware FPU sometimes corrupted memory writes. At 900 MHz the same tests
  passed every time, so the daily kernel runs at 900 MHz with the FPU on.

## 5. The upper 128 MiB of RAM: still open

The chip is 256 MB, but reads above 128 MiB land back in the lower half (an
"alias"). The bootloader's memory-mapping bit for the larger layout is
**already set**, so that's not the fix. I haven't tried changing the memory
controller settings on the live system, because a wrong setting could stop it
booting. That needs a safe, recoverable experiment first, and is the next step.

## 6. Where it stands

The console now boots Linux 7.2 and Debian 12 from its SD card and runs NZEUS
Deck daily: a desktop with a terminal, editor, Python, web browser, Wi-Fi, SSH,
media player and car-diagnostics tools. Its flash chip has never been written.
The source is in this repository.

## Responsible disclosure

If I ever find an issue that puts **other people's** devices or data at risk,
I'll report it privately to the maker before publishing anything.

# Modern Linux: buttons (read-only first)

What the repo knows (stock): no input devices; the vendor daemon `cubevol`
reads the buttons through `/dev/mem`. The vendor `/panel` DT node names the pins:
serializer clock L08, data L09 (D-pad, A/B/X/Y, L1/R1, Select/Start, order on
this board unproven); direct pins L2 = L11, R2 = L07, Vol- = L12 (also the UART
TX the Uno listens to), Vol+ = L13, FN = L02, power button = L14 (also the pin
nzeus-poweroff drives low), power-pin = L01. Our kernel already has
gpio-hc16xx, gpio-keys and gpio-serializer-keys built in, but the GPIO
controller is disabled in our board tree.

## Step 1: watch the pins (read-only)

`key-watch.c` (static MIPS `key-watch-mipsel`, built with
`mipsel-linux-gnu-gcc -Os -static -s -march=mips32r2 -mabi=32 -Wall -Wextra
-Werror`) maps the 4 KiB page at 0x18800000 PROT_READ from `/dev/mem` opened
O_RDONLY. It prints the four GPIO banks' INT_EN/RISE/FALL/IN/OUT/DIR (never the
interrupt status word) and the L-bank pin-mux bytes, does a fast burst of reads
to find pins something else is toggling, then samples every 5 ms for N seconds
and prints each input change with its time. PC-tested under qemu-mipsel against
a fake register file. `key-watch.sh` runs it for 60 s.

Press order for the test (hold each about 1 s, wait about 2 s between):
L2, R2, Vol+, Vol-, FN, D-pad Up, A, Start, left stick left, right stick up.
Do not press Power or RST.

### Result 2026-10-10 (usb-command-20261009T134950.txt)

Pressed L2, R2, Vol+, Vol-, FN, Up, A, Start, left stick, right stick.
- L2 = **L07**, R2 = **L11** (the SF3000 wiring; the vendor `/panel` names
  `key-shoulder-left = 11` - our bring-up override has them swapped), Vol+ =
  **L13**, FN = **L02**. All idle high, pressed low (active low).
- Vol- showed nothing: L12 is muxed as the UART (pin-mux byte 0x11) and reads 0.
  Do not use Vol- while the UART console owns L12.
- Up, A, Start and the sticks changed no pin: they are behind the serializer,
  which nothing is clocking (the burst saw no flips). L08/L09 are outputs driven
  high (L_OUT=L_DIR=0x340: L06, L08, L09). L14 (power) is an input reading 1.
- Pin-mux: L02, L07-L11, L13, L14 are function 0 (GPIO).

## Step 2: read the serializer (writes L08/L09 only)

`key-scan.c` (static `key-scan-mipsel`, same flags) clocks the serializer from
user space with the 2-wire protocol of `gpio_serializer_keys.c`: latch by
driving L09 low 12 us, release it, read, then clock L08 low/high (3 us each)
and read after each falling edge; one extra clock at the end. It refuses unless
L08/L09 are output-high, only read-modify-writes bits 8/9 of L_OUT/L_DIR, keeps
L09 released between scans, stops if any other OUT/DIR bit changes, restores
output-high on exit/SIGTERM. Prints the raw 24 bits whenever they change. Logic
tested on the PC against a simulated 2-wire chip under qemu-mipsel.

Press order (hold about 1 s, about 2 s apart): Up, Down, Left, Right, A, B, X,
Y, L1, R1, Select, Start, left stick left, right stick up.

### Result 2026-10-10 (usb-command-20261009T135450.txt)

The chip answered: 12 keys, bits 12-23 always 1, pressed = 0, RESTORED_OK=1,
no other pin changed in 5821 scans. Order = exactly the SF3000 order:

| bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| key | L1 | X | Y | R1 | B | A | Select | Start | Up | Down | Left | Right |

(X was pressed together with R1 once by accident: a 0.15 s blip on bit 3,
which the separate R1 press confirms.) Left stick left = bit 10 (Left): the
left stick is wired to the D-pad. Right stick up = bit 1 (X) with a short Y
blip: the right stick is wired to the face buttons. No analog input.

## Step 3: kernel button drivers (`gma-native-keys`)

`../display/r36s-gma-avp-keys.dts` = the sdro board tree plus: GPIO controller
on, a `gpio-keys-polled` node (L2 L07, R2 L11, Vol+ L13, FN L02, active low,
20 ms) and the upstream serializer-keys node enabled unchanged. The old
gpio-keys node (L2/R2 swapped) stays disabled. Polled on purpose: no GPIO pin
interrupt is ever enabled. Config = sdro + `CONFIG_KEYBOARD_GPIO_POLLED`.
`verify-avprpc.py --variant keys` checks every changed property, the measured
key order and that no other node uses the GPIO controller (L06 LCD reset, L14
power stay untouched); mutation-tested. Compiled in the cloud at the pinned
commit (symbols gpio_keys_polled_probe, gpio_serializer_keys_probe,
hc16xx_gpio_probe present). Test: `key-events.sh` (read-only, prints presses).

### Result 2026-10-10: ALL BUTTONS WORK (usb-command-20261009T141848.txt)

`gma-native-keys` booted (`7.2.0-rc5-nzeus-gma-native-keys+`): `hc16xx-gpio
registered 128 GPIOs`, `input0 r36s-keys` (event0), `input1 serializer-keys`
(event1). `key-events` saw every button with the right name: Up Down Left
Right A B X Y L1 R1 Select Start (event1), L2 R2 Vol+ FN (event0); 17 presses
(my first press was Down by mistake - also reported correctly). The GPIO
controller's interrupt count stayed 0 before and after (no pin interrupt, no
storm); `ERR: 7` was the same before and after.

## Step 4: first interactive program (`box-demo`)

`box-demo.c` (static `box-demo-mipsel`): a box on the native framebuffer.
D-pad / left stick move it, A/B/X/Y change colour (green/red/blue/yellow),
L1/R1 smaller/bigger, Start ends early. Big MM:SS clock and a blinking dot
(alive). Same framebuffer checks and exit restore as fb-pattern; input only
read; waits in poll(). `box-start.sh` runs it 25 min in the background (also
the long display test with ONE fb owner); `box-check.sh` reads its log and
counts AVP blank recoveries.

### Result 2026-10-10: 25-MINUTE RUN PASSED (usb-command-20261009T144839.txt)

box-demo ran the full 1500 s as the only fb owner (`BOX_DEMO_END t=1500
presses=80 by_start=0`, then the firmware layer restore). 80 key presses
handled; ALIVE every minute. The AVP blanked 13 times (about every 120 s) and
the GMA watchdog recovered every one (`reapply 0, layer 0`, out back to
...112). Photo at 02:09: correct colours.

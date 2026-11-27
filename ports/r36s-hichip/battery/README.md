# Modern Linux: battery and charger (read-only first)

No HiChip ADC driver exists for the modern kernel. The factory bootloader,
however, carries its own complete "saradc" driver, decoded from
`../boot-analysis/bootloader-disasm.txt` (lines 42621-42968):

- ADC block 0x18818400 (shares its 0x100 window with the PWM at 0x18818410);
  per-channel analog settings at 0x18800160..0x18800173; clock gate
  0x18800060 bit 12 (clear = running; `clk_ignore_unused` keeps it on).
- Latest sample of channel ch = one byte at 0x188184D0 + idx,
  idx = ch < 4 ? ch : ch + 2. No start command; the driver just reads it.
- Stock value = min(255, 241 * raw[ch] / raw[ch0]) (stock DT
  `adjust_channel = <0>`). ch1 = battery level (about 128 empty .. 175 full,
  `apps/nzeus-gui/hw.h`; bootloader: <= 127 = low), ch5 = charger (>= 64 =
  plugged in, `apps/nzeus-gui/hw.c`).
- The bootloader configures channels 0, 1 and 5 before Linux starts.

## Step 1: read it (read-only)

`bat-read.c` (static `bat-read-mipsel`, usual flags) maps both pages PROT_READ
from `/dev/mem` opened O_RDONLY, prints the clock gate, the configuration
registers the bootloader writes and the data bytes, then one line per second:
raw values, the stock-equivalent battery level (with a rough %) and the
charger level. `battery-read.sh` runs it for 60 s.

If the values move sensibly (charger level jumps when the cable is plugged
in), the next step is a small kernel driver exposing them as a standard Linux
battery (`/sys/class/power_supply`).

### Result 2026-10-10: BATTERY AND CHARGER READ (usb-command-20261009T143304.txt)

On `gma-native-keys`, uptime 863 s: `SAR_ADC_CLOCK=RUNNING` (gate 0x0e020f8c,
bit 12 clear). The bootloader's configuration is still in place and the ADC
free-runs: ref (ch0) 245-249, battery (ch1) 177-178, so the stock battery
level is 171-174 (about 93-97 %), steady over 60 s. Charger (ch5): 0 unplugged,
raw 141-142 -> level 137-139 plugged in; it followed my plug / unplug /
plug at t=45, 53, 55 s within one sample. DATA_D0_D7 = f7 b1 00 00 f7 b1 00 00
(idx 4 mirrors idx 0 and idx 5 mirrors idx 1 at that moment).
Registers: 0C=08041000 70=40543210 74=81000f01 88=20630040 8C=00238000.
Next: a small read-only kernel driver -> /sys/class/power_supply.

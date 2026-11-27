# Port: R36S V2.7 clone (HiChip E3100)

| | |
|---|---|
| Console | R36S V2.7 clone, board marked **HC16E3100V20**, any colour |
| Chip | HiChip E3100 (HC16xx family), MIPS |
| Not this port | R36S consoles with a Rockchip chip (those run ArkOS and similar) |
| Kernel | Linux 7.2, from [linux-hichip](https://github.com/SiarheiVolkau/linux-hichip) branch `hc16xx-7.2` (pinned in `upstream-manifest.json`) |
| System | Debian 12 (mipsel) on the SD card |
| Status | Released: v1.0 beta |

## What works

- Starts Linux from the SD card without a PC; the console's flash chip is never changed
- Screen (correct colours), buttons, brightness, battery and charger readings,
  power-off and restart
- Speaker and headphones (switch by hand: FN + Vol+, or Settings > Sound)
- USB networking and SSH to a PC; USB host mode: Wi-Fi dongle (RTL8821CU),
  Bluetooth, keyboard, USB stick

## Not done yet

- Only about 115-117 MiB of RAM is usable; the upper memory mirrors the lower 128 MiB
- Headphone plug detection is unreliable, so the switch is manual
- Battery calibration and long-run tests (cold boots, heavy use)

Full details and the evidence for each point: [STATUS.md](STATUS.md).
How the port was built, step by step: [README.md](README.md).

## Hardware-specific parts in the shared folders

These live outside this folder but talk to this console's hardware directly. A new
port will need its own version or a setting:

- `../../apps/frontend-modern/`: the console program (screen device, buttons, sound, battery)
- `../../installer/` and `../../tools/r36s_card.py`: recognise this console's SD card
  and its scrambled boot sector

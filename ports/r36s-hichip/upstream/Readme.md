
# Linux for HiChip HC16XX

This repository contains a work-in-progress port of the Linux kernel for **HiChip HC16XX** family SoCs, with the primary target being the **Data Frog SF3000** handheld game console.

The goal of this project is to provide a clean, upstream-oriented Linux port for HC16XX-based devices without relying on vendor SDKs or proprietary kernel trees.

> **Status:** Early development. The kernel boots on real hardware and basic platform bring-up is in progress.

---

## Supported devices

| Device | SoC | Status | Notes |
|--------|-----|--------|-------|
| Data Frog SF3000 | HiChip HC16XX | Early bring-up | Kernel boots to userspace initialization, serial console operational, no storage support yet |


## Current HC16xx support

| Subsystem | Status | Notes |
|-----------|--------|-------|
| MIPS74Kf CPU | Done | Including MMU, FPU, DSPr2 |
| MIPS32r1 CPU |  | Not planned |
| Early console | Done | 8250/16550 compatible |
| Serial console | Done | Runtime console operational |
| IRQ controller | Done | |
| Clock framework | Partial | Partially implemented will be extended by developing next drivers |
| Timers | Done | |
| RTC |  |  |
| Watchdog |  |  |
| GPIO | Done |  |
| Pin controller | Done |  |
| DMA | TODO |  |
| SFSPI | TODO |  |
| SPI master | TODO |  |
| ADC |  |  |
| PWM/Backlight | Done |  |
| MMC / SD | Partial | No high-speed modes yet |
| USB | Partial | No DMA, no OTG, no Host mode |
| I2C |  |  |
| I2S | TODO |  |
| Display Controller / DE | WiP |  |
| MIPI-DSI | TODO |  |
| HDMI |  |  |
| Multimedia IPs |  | H264,JPEG decoders etc. |

## Current SF3000 support
| Subsystem | Status | Notes |
|-----------|--------|-------|
| Charging | Done | Fully hardware solution |
| Charging and battery status | TODO |  |
| RGB LED | N/A | Can't be controlled in software |
| D-Pad, Buttons | Done | Obscure serializer in use for 12 keys, 4 keys are GPIO |
| Joysticks | Done | Wired in parallel to corresponding buttons |
| Display | TODO | MIPI-DSI display of unknown model |
| Backlight | Done | PWM backlight driver. minimally tested. Need to pick proper PWM points for linearity. |
| Audio | TODO | External DAC, amplifier, integrated mono speaker, headphone jack |
| TF card slot | Done |  |
---

## Development goals

Current priority is to make SF3000 functional for it's use case - retro games emulation.

---

## Building

The kernel can be built using any modern GCC MIPS little-endian cross compiler:

```bash
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- sf3000_defconfig
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu-
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- vmlinuz.bin
cat arch/mips/boot/vmlinuz.bin arch/mips/boot/dts/hichip/sf3000.dtb > vmlinuz.bin
```

---

## Disclaimer

This repository is **not** affiliated with HiChip or Data Frog.

The implementation is based on publicly available information and reverse engineering of commercially available devices.

The project is intended for research, preservation and Linux platform support.

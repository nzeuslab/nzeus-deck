# Ports: one folder per console

NZEUS Deck is split in two:

- **Shared parts** work the same on every console: the Deck's apps
  (`../apps/`), its helper programs (`../core/deck-modern/`), the installer
  (`../installer/`) and the release tools (`../tools/`).
- **A port** is everything that belongs to one console's hardware: its kernel
  settings and device tree (the map of its chip, screen, buttons and pins), its
  drivers, how it boots from the SD card and how its system image is built.

Each console gets its own folder here:

| Port | Console | Chip | Status |
|---|---|---|---|
| [`r36s-hichip/`](r36s-hichip/PORT.md) | R36S V2.7 clone (board HC16E3100V20) | HiChip E3100 (HC16xx, MIPS) | Released (v1.0 beta) |

## What a port folder holds

| Part | What it is |
|---|---|
| `PORT.md` | One page: the console, the chip, what works and what doesn't yet |
| `kernel-public/` | Builds and packages the kernel for this console |
| `*.dts` | The device tree: tells Linux what hardware is on the board |
| `display/`, `audio/`, `power/`, `input/`, `battery/` | Drivers and notes for each part of the hardware |
| `usbnet/`, `usbhost/` | USB networking and USB host mode |
| `fatroot/`, `sdroot/` | How the console starts Linux from the SD card |
| `rootfs/` | Builds the Debian system image; copies in `../core/deck-modern/` |
| `upstream/` | The pinned upstream kernel work this port builds on |

Not every console needs every part. A console that already boots Linux the
normal way won't need `fatroot/`, for example.

## Adding a new console

1. **Investigate first, privately.** Photos of the board, the chip markings and a
   full backup of the console's original SD card (and flash, if it has one). Never
   commit the maker's firmware, keys or card images to this repo.
2. **Copy the closest port:** `cp -r ports/r36s-hichip ports/<new-console>`, then
   rewrite `PORT.md` and delete the parts that don't apply.
3. **Change the hardware parts:** device tree, kernel settings, drivers, boot method.
4. **Keep shared code shared.** If something in `core/` or `apps/` needs to behave
   differently on the new console, add a setting for it instead of copying the file
   into the port.
5. **Add the port to the table above** and to the main [README](../README.md).

Some files in `r36s-hichip/rootfs/debian/` are not really specific to this console
(the Debian package list, the overlay). They stay in the port until a second console
needs them; then they move into `../core/`.

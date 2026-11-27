# Third-party software and credits

NZEUS Deck builds on other people's work. Thank you to all of them.

## Research this builds on
- **Siarhei Volkau**: sf3k-hacking and linux-hichip (Linux for HiChip HC16xx).
  The kernel used here is his `hc16xx-7.2` branch. The files in
  `ports/r36s-hichip/upstream/` and `ports/r36s-hichip/usbhost/kernel/` carry his copyright (GPL-2.0).
- **ozkaoz**: r36sx-hclinux research on HiChip consoles.

## Included in this repository
| Part | Where | Licence |
|---|---|---|
| libmad (MP3 decoding) | `apps/nzeus-gui/libmad/` | GPL-2.0-or-later, `licenses/GPL-2.0.txt` |
| libvterm (terminal) | `apps/nzeus-gui/vterm/` | MIT, `licenses/libvterm-MIT.txt` |
| libretro.h | `apps/nzeus-gui/libretro.h` | MIT, `licenses/libretro-h-MIT.txt` |
| IBM Plex Sans, Sans Arabic, Mono (car report PDF) | `apps/nzeus-gui/font_report.c` | SIL OFL 1.1, `licenses/IBM-Plex-OFL-1.1.txt` |
| unscii pixel font (via LVGL) | `apps/nzeus-gui/font_mono*.c` | public domain |
| Twemoji graphics (test pictures) | `apps/nzeus-gui/tests/emoji/` | CC BY 4.0, `licenses/Twemoji-CC-BY-4.0.txt` |

## Downloaded at build time (not included)
| Part | Licence |
|---|---|
| LVGL 8.3 and its Montserrat font (`setup_lvgl.sh`) | MIT, `licenses/LVGL-MIT.txt`; font: OFL 1.1, `licenses/Montserrat-OFL-1.1.txt` |
| Linux kernel (linux-hichip) | GPL-2.0 |
| BusyBox 1.37.0 (rescue shell; checksum in `ports/r36s-hichip/upstream/`) | GPL-2.0 |
| Dropbear SSH | MIT-style, `licenses/Dropbear-MIT.txt` |
| Debian 12 packages (`ports/r36s-hichip/rootfs/debian/packages.txt`) | each package's own licence |
| Twemoji release (`tools/make_emoji.py`) | CC BY 4.0 |

The Deck's own icons, themes and boot logo are its own work.

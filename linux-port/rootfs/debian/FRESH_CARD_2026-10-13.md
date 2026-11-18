# Fresh-card Debian root image — 13 October 2026

Superseded by `FRESH_CARD_2026-10-14.md`. The compressed artifact below retains
this exact historical build; the main WSL image path now contains the October 14
refresh, while this build is retained as `rootfs.ext4.prev`.

Built and PC-verified. This is the **Debian root filesystem component**, not a
complete SD-card disk image or universal installer. It has not been boot-tested
on a fresh physical card. It does not replace the working console's `/data`.

## Local artifacts

- Compressed: `backups/modern-cleanup-20261013/fresh-card/rootfs.ext4.xz`
- Historical uncompressed in WSL Ubuntu: `/home/nzeus/nzeus-rootfs-debian/out/rootfs.ext4.prev`
- SHA256SUMS, manifest, provenance and privacy/smoke logs accompany the compressed
  copy. Large artifacts stay out of Git; `manifest/` holds the committed records.

| Property | Value |
| --- | --- |
| System | Debian 12 bookworm, mipsel, SysV init |
| Filesystem | ext4, label nzroot, 4,294,967,296 logical bytes |
| Packages | 222 |
| Managed image paths | 1,533; zero missing, different or unmapped |
| Compressed size | 55,003,016 bytes |
| Uncompressed SHA-256 | `7066e6baa7bb1890212476eb2373161404dfe2766ffceb057df201e6ec913f23` |
| Compressed SHA-256 | `d5d50f17782720b752cff4d0d828df543e87ee0a68d41b472f5db390315602d7` |
| Source commit | `7172b778351467c44ff6b48c58c7ecd9e1c0974b` (after history rewrite) |
| Deck executable SHA-256 | `c3e16147768eff7ce57cc8ba2aa8f44c5e36fd2f29084e46f6d2c3388834d6e1` |

Includes the modern-only Deck/Health fix, framebuffer NetSurf/fonts, Python 3
and seven examples, 1,427 emoji pictures with licence, PDF rendering, ALSA tools,
Wi-Fi/Bluetooth firmware and current helpers. No personal authorized SSH key,
host private key, Wi-Fi configuration or `/data` files are shipped. SSH stays
disabled until the new card's owner provisions their public key through initial
setup; USB rescue remains available. My personal root access is not shipped.

## Verification

Ext4 check clean. Provenance reads the actual image, matches managed bytes/modes
to sources and cross-checks the build root. Privacy scan clean. QEMU runs Python,
OpenSSL, Poppler and an apt install simulation. NetSurf reaches display
initialization but cannot initialize the PC's absent framebuffer; fresh-card
browser hardware operation is untested. Region-tool self-test passes **70/70**
using this real image on a fake card: install, readback, restore/reset and
refusals. No physical card was written.

Built with `NZ_NO_AUTHORIZED_KEYS=1` and `build-debian.sh --reuse --packages`.
The builder reports tracked changes because Windows snapshot junctions differ
in WSL; provenance confirms **no image input changed**. The previous image is
retained as `rootfs.ext4.prev`. This refresh does not claim a bit-for-bit
reproducible Debian bootstrap.

## Before installing on a card

Follow `../region-tool/README.md`: identify the target, take fresh stock and
Linux-area snapshots, prepare a candidate/delta, check every preimage, install
with readback and retain recovery. Existing Windows profiles pin the current
test card's size/identity; do not use them on another card without deliberately
preparing that card's supported identity/layout. Never Explorer-format the
working card or replace its whole Linux area over existing `/data`.

Kernel and seven boot files are separate. Use the supported HiChip E3100/V2.7
boot path and current 900 MHz hardware-FPU daily kernel. Do not reintroduce
1188 MHz hardware-FPU operation or the stock 0.9 startup path. Hardware status:
`../../STATUS.md`.

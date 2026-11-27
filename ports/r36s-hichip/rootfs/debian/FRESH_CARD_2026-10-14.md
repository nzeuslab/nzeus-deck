# Fresh-card Debian root image — 14 October 2026

This refresh includes clearer WhatsApp connection errors and the builder fix
that preserves generic-image privacy through sudo. The existing modern GUI
executable and all 222 packages remain unchanged. This is the **Debian root
filesystem component**, not a complete SD-card disk image or universal
installer. It has not been boot-tested on a fresh physical card.

## Artifacts

- Compressed: `backups/console-progress-20261014/fresh-card/rootfs.ext4.xz`
- Uncompressed in WSL Ubuntu: `/home/nzeus/nzeus-rootfs-debian/out/rootfs.ext4`
- SHA256SUMS, manifest, provenance and verification logs accompany the private
  compressed copy. Large artifacts remain outside Git.
- The October 13 compressed artifact remains in
  `backups/modern-cleanup-20261013/fresh-card/`; its uncompressed copy is now
  `/home/nzeus/nzeus-rootfs-debian/out/rootfs.ext4.prev`.

| Property | Value |
| --- | --- |
| System | Debian 12.15 bookworm, mipsel, SysV init |
| Filesystem | ext4, label nzroot, 4,294,967,296 logical bytes |
| Packages | 222 |
| Managed paths | 1,533; zero missing, different or unmapped |
| Compressed size | 48,683,840 bytes |
| Uncompressed SHA-256 | `9824ce927b78906d60b90663a14dea0ad32e79eef782379ca3ceb57a07050182` |
| Compressed SHA-256 | `a7c9d2e23bf03d6ced2b25fa4861049737770c8f738956cd5bd8536545baa836` |
| Source commit | `06a58f877c08d518991857568fc5d87b5b9fbfa0` |
| Deck executable SHA-256 | `c3e16147768eff7ce57cc8ba2aa8f44c5e36fd2f29084e46f6d2c3388834d6e1` |

Built with `NZ_NO_AUTHORIZED_KEYS=1` and `build-debian.sh --reuse`, without
package installation or upgrades. Image inputs match the committed sources;
the reported tracked differences are unrelated WSL snapshot junctions and
generated records. This does not claim bit-for-bit reproducible bootstrap.

## Verification

The ext4 check is clean. Provenance reads the image itself, matches managed
bytes/modes to sources and finds no differences against the build root.
Privacy scan and explicit assertions confirm no owner authorized key, SSH host
private keys or `/data` files. QEMU runs Python 3.11.2, OpenSSL and Poppler,
checks WhatsApp helper shell syntax and completes an apt install simulation
without installing packages. Its temporary emulator was removed afterward.

The compressed archive passes `xz -t` and a complete decompression SHA-256
comparison against the original 4 GiB image. All **70/70 recovery checks** pass
using this real image on a fake card, including install/readback, interrupted
install recovery, restore/reset and refusal checks; no physical device is used.

## Installation boundary

This image was not installed on the working console or its card. A separate
home/away WhatsApp address correction changed two settings on the live console
later in this session; it did not install this image or change boot files. The
October 13 physical-card snapshot remains historical recovery evidence, not a
fresh preimage for the now-running card. Do not overwrite an existing card's
Linux area and `/data` with this generic component.

Follow `../region-tool/README.md`: identify the target card, take fresh stock
and Linux-area snapshots, prepare a candidate/delta, check every preimage,
install with readback and retain recovery. Existing Windows profiles pin the
current test card's identity and must not be reused for an unrelated card.
Never Explorer-format the working card.

Kernel and seven boot files are separate. Use the supported HiChip E3100/V2.7
boot path and current 900 MHz hardware-FPU daily kernel. SSH requires the new
owner to provision their public key through initial setup; no personal access
key is shipped. Fresh-card hardware boot and the full installer remain to be
validated. Current hardware status: `../../STATUS.md`.

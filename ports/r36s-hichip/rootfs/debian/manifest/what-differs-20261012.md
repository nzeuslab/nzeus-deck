# What differs: new image vs the live card (docs)

DRY RUN ONLY. Nothing here was executed. The console was not contacted.

Image: `rootfs.ext4` sha256 `f9790acc46e129b92d90e2750fe8edeeeade33228014b08def141d3e797655ce`, built 2026-10-12T16:53:23Z from commit `ac6300c7b2b8e430688773778feae6f79c77ce9a`.

Live state is taken from the repo docs (live-card-known-20261012.json), not read from the console. Live root image: `a41d2a0c33026e2107a731b25fc1aa8f3fec5c1a69cbac10a41152d3a1d17c08`.

Added on the live card by hand after that image (docs):

- nzeus-ram overlay + init script (RAM release at boot) (lab notes 2026-10-10 21:14 "installed live on the card's Debian")
- nzeus-usbnet + /etc/nzeus/usbnet-at-boot (rndis) (lab notes 2026-10-10 21:33)
- nzeus-voldown (lab notes 2026-10-10 23:00)
- time zone: my local zone (lab notes 2026-10-10 21:25 ("card set live"))
- /root/.ssh/authorized_keys (my public key) (lab notes 2026-10-10 21:25 (image a41d2a0c had "no SSH key yet"))
- Realtek Wi-Fi/BT firmware + upstream regulatory.db copied live (not via dpkg) (lab notes 2026-10-11 Wi-Fi entry)
- alsa-utils installed with dpkg from the official mirror (lab notes 2026-10-11 ALSA entry)
- Deck helpers in /usr/local/lib/nzeus/deck-bin + nzeus-deck start/autostart (lab notes 2026-10-11 Deck Settings audit; signed updates 2026-10-12)
- poppler-utils + 27 dependencies (apt, nofpu kernel) (reports/continuation/pdf-reader-20261012.md)

## Managed files (97): link 3, same 24, unknown-live-hash 70

| path | image sha256 | live | state | sync action |
|---|---|---|---|---|
| `/etc/apt/apt.conf.d/90nzeus` | 2eccaec95d69 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/apt/preferences.d/90nzeus-no-systemd` | be0228c68157 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/default/halt` | 72a12b526da5 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/default/locale` | 89dd29db91ea | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/default/nzeus-ssh` | 7c35589d46d1 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/default/nzeus-usb-shell` | f1aa4a36057f | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/dpkg/dpkg.cfg.d/90nzeus-nodoc` | 4b3f690ad4c1 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/e2fsck.conf` | 7f0d0fb98f05 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/fstab` | 17a1914b8d6f | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/hostname` | 34f101a1c46f | - | unknown-live-hash (source unchanged since the live image) | NEVER (the card's setting wins) |
| `/etc/hosts` | ee42512d48ed | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-battlog` | d1c83fbab0c3 | - | unknown-live-hash (source changed 2026-10-10T21:41, after the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-clock` | eda5b1fec369 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-data` | 173d938515b6 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-modern` | 18b9eda56494 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-ram` | c8d24ad33026 | - | unknown-live-hash (source changed 2026-10-10T21:15, after the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-rootfs` | c3299b10962a | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-ssh` | dc48e42d35c5 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-usb` | ff41c7243a95 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-usbnet` | 59ba9f334681 | - | unknown-live-hash (source changed 2026-10-10T21:32, after the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-voldown` | c26734932a45 | - | unknown-live-hash (source changed 2026-10-10T22:05, after the live image) | read live hash first (collect) |
| `/etc/init.d/nzeus-zram` | 06033ab529f5 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/inittab` | a334fa86afbb | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/localtime` | - | - | link (checked by target: /usr/share/zoneinfo/<my local zone>) | NEVER (the card's setting wins) |
| `/etc/motd` | 7ab87826a8fb | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/etc/nzeus-rootfs-release` | 67fb8d071107 | - | unknown-live-hash | NEVER (build stamp of the image; the card's own stays) |
| `/etc/nzeus/autostart-modern` | 517ca56973c4 | - | unknown-live-hash (source changed 2026-10-10T21:40, after the live image) | NEVER (my switch (Deck autostart)) |
| `/etc/nzeus/inputrc` | 608a495a0400 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/nzeus/sdroot.conf` | 2eb021ae0c8c | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/nzeus/usbnet-at-boot` | 4d6beb937c70 | - | unknown-live-hash (source changed 2026-10-10T21:32, after the live image) | NEVER (my switch (deleting it turns the USB network off)) |
| `/etc/profile.d/nzeus.sh` | 866b6cfa0afc | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/sysctl.d/90-nzeus.conf` | 2de1f1d3d130 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/etc/timezone` | 2f96218f000b | - | unknown-live-hash | NEVER (the card's setting wins) |
| `/root/.ssh/authorized_keys` | 5d922aaa8524 | - | unknown-live-hash | NEVER (my SSH keys on the card win) |
| `/usr/bin/scp` | - | - | link (checked by target: ../local/sbin/dropbearmulti-modern) | nothing |
| `/usr/local/bin/nzeus-deck` | ff76694208ac | - | unknown-live-hash (source changed 2026-10-11T14:37, after the live image) | read live hash first (collect) |
| `/usr/local/bin/nzeus-modern` | 3bf416e9b542 | 3bf416e9 (prefix) | same | nothing |
| `/usr/local/bin/nzeus-status` | e34f58c3ac68 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/bin/python` | - | - | link (checked by target: /usr/bin/python3) | nothing |
| `/usr/local/bin/usb-host-confirm` | ffc73e2785d4 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/bin/usb-host-off` | 368cb60dc4c1 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/bin/usb-host-status` | aecc52aecc74 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/bin/usb-host-trial` | 6c23d9c6ef3f | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/bin/usbnet-confirm` | 07cf741cf3a6 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/bin/usbnet-down` | 11a82983b63c | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/bin/usbnet-up` | 131c0d6b20d4 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/bin/wifi-down` | fbdfadec7ac6 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/bin/wifi-up` | a964d096d8be | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/deck-bin/SHA256SUMS` | 2b9c994b692c | - | unknown-live-hash | read live hash first (collect) |
| `/usr/local/lib/nzeus/deck-bin/dropbearmulti` | fed0e192f1de | fed0e192f1de | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/flag-now.sh` | d59d3a2e6da7 | d59d3a2e6da7 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-bt` | e732e67d68d0 | e732e67d68d0 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-bt-up.sh` | 2a6572cd9dd9 | 2a6572cd9dd9 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-bt.bin` | dd7d52504d7f | dd7d52504d7f | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-car-support` | 8ae2028a4922 | 8ae2028a4922 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-claude-access.sh` | 9122a7d26701 | 9122a7d26701 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-claude-connect.sh` | ad4ce76d9f40 | ad4ce76d9f40 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-common.sh` | 0b6f6bf77982 | 0b6f6bf77982 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-fetch` | 68e6a1166ea0 | 68e6a1166ea0 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-net-up.sh` | b67f3c474811 | b67f3c474811 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-netctl` | 740d39aeac9d | 740d39aeac9d | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-update.sh` | cdba2ac72d5a | cdba2ac72d5a | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-upload.sh` | 0abf0084d357 | 0abf0084d357 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-usbmode.sh` | 079cb6e5cc8e | 079cb6e5cc8e | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-usbserial.sh` | d040744358bd | d040744358bd | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/nzeus-wa.sh` | 34e657716bc4 | 34e657716bc4 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/python` | dc809aac56cc | dc809aac56cc | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/wifi-auto.sh` | 3b2bb127589f | 3b2bb127589f | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/wifi-join-core.sh` | 5413cefc16a0 | 5413cefc16a0 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/wifi-join.sh` | dbf6d06ccac2 | dbf6d06ccac2 | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/wifi-on.sh` | f44dcb03925a | f44dcb03925a | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/wifi-scan.sh` | a51229d5ea48 | a51229d5ea48 | same | nothing |
| `/usr/local/lib/nzeus/usbhost/fn-hold.py` | bf74fed867d8 | - | unknown-live-hash (source changed 2026-10-10T23:12, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/udhcpc.script` | 44761c086224 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/usb-host-confirm.sh` | 184a40b7df0f | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/usb-host-off.sh` | 18fd7e73ed7d | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/usb-host-status.sh` | 578246397dd6 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/usb-host-trial.sh` | 1b1c32001a3f | - | unknown-live-hash (source changed 2026-10-11T15:58, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/wifi-down.sh` | cbfd8e73bafa | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/wifi-up.sh` | 334499d3b665 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbhost/wifi.conf.example` | a6a665503db9 | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbnet/usbnet-confirm.sh` | e5f5ecd5b489 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbnet/usbnet-down.sh` | a4697140fc58 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/lib/nzeus/usbnet/usbnet-up.sh` | 96b5e4a476af | - | unknown-live-hash (source changed 2026-10-11T11:53, after the live image) | read live hash first (collect) |
| `/usr/local/sbin/dropbearmulti-modern` | 2cb1600373b9 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/nz-cdeject` | f2b12e9137ea | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-battlog` | a365af57710c | - | unknown-live-hash (source changed 2026-10-10T21:41, after the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-clock` | b4f9129e6e27 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-datamount` | 6bfcb30f8f90 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-ram` | c2bd492b9613 | - | unknown-live-hash (source changed 2026-10-10T21:15, after the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-rootcheck` | d77e1b68a621 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-usb-gadget` | 0d891d5695a3 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-usb-shell` | 47db2f0896f7 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-usbnet-boot` | 48b7a3aed855 | - | unknown-live-hash (source changed 2026-10-10T21:32, after the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-voldown` | 3a2ebf17014b | - | unknown-live-hash (source changed 2026-10-10T22:29, after the live image) | read live hash first (collect) |
| `/usr/local/sbin/nzeus-zram` | ec92e3d9cb36 | - | unknown-live-hash (source unchanged since the live image) | read live hash first (collect) |
| `/usr/local/sbin/usb-regs` | 0cca5293ef5a | - | unknown-live-hash (source changed 2026-10-10T22:39, after the live image) | read live hash first (collect) |

## Packages (not part of a file sync; need apt on the nofpu kernel)

Image 197, live (docs) 193.

- In the image, not recorded on the live card (4): firmware-realtek, libusb-1.0-0, usbutils, wireless-regdb
- Recorded on the live card, not in the image (0): none
- Different versions (0): none

## How a real sync would run (NOT done here)

1. I connect the console; fresh read-only `collect` output replaces the docs-based column.
2. Re-run `plan`; only `differs`/`missing-on-live` rows with a copy action are candidates.
3. I approve the list. Old copies are saved to `/data/nzeus/sync-backup-DATE` first;
   every copied file is read back by hash; nothing in `/data` is changed otherwise.
4. Start-up files only while I watch a restart (lab rule 6); the app
   via the signed updater with its 60-second watcher. Never a raw device write.

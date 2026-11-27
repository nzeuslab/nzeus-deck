# Managed-file sync plan (live hashes)

DRY RUN ONLY. Nothing here was executed. The console was not contacted.

Image: `rootfs.ext4` sha256 `f9790acc46e129b92d90e2750fe8edeeeade33228014b08def141d3e797655ce`, built 2026-10-12T16:53:23Z from commit `ac6300c7b2b8e430688773778feae6f79c77ce9a`.

## Managed files (97): differs 5, link 3, missing-on-live 2, same 87

| path | image sha256 | live | state | sync action |
|---|---|---|---|---|
| `/etc/apt/apt.conf.d/90nzeus` | 2eccaec95d69 | 2eccaec95d69 | same | nothing |
| `/etc/apt/preferences.d/90nzeus-no-systemd` | be0228c68157 | be0228c68157 | same | nothing |
| `/etc/default/halt` | 72a12b526da5 | 72a12b526da5 | same | nothing |
| `/etc/default/locale` | 89dd29db91ea | 89dd29db91ea | same | nothing |
| `/etc/default/nzeus-ssh` | 7c35589d46d1 | 7c35589d46d1 | same | nothing |
| `/etc/default/nzeus-usb-shell` | f1aa4a36057f | f1aa4a36057f | same | nothing |
| `/etc/dpkg/dpkg.cfg.d/90nzeus-nodoc` | 4b3f690ad4c1 | 4b3f690ad4c1 | same | nothing |
| `/etc/e2fsck.conf` | 7f0d0fb98f05 | 7f0d0fb98f05 | same | nothing |
| `/etc/fstab` | 17a1914b8d6f | 17a1914b8d6f | same | nothing |
| `/etc/hostname` | 34f101a1c46f | 34f101a1c46f | same | NEVER (the card's setting wins) |
| `/etc/hosts` | ee42512d48ed | ee42512d48ed | same | nothing |
| `/etc/init.d/nzeus-battlog` | d1c83fbab0c3 | d1c83fbab0c3 | same | nothing |
| `/etc/init.d/nzeus-clock` | eda5b1fec369 | eda5b1fec369 | same | nothing |
| `/etc/init.d/nzeus-data` | 173d938515b6 | 173d938515b6 | same | nothing |
| `/etc/init.d/nzeus-modern` | 18b9eda56494 | 18b9eda56494 | same | nothing |
| `/etc/init.d/nzeus-ram` | c8d24ad33026 | 4cc5125cd2b1 | differs | copy (backup old -> /data/nzeus/sync-backup-DATE, readback); START-UP FILE: only with me watching a restart |
| `/etc/init.d/nzeus-rootfs` | c3299b10962a | c3299b10962a | same | nothing |
| `/etc/init.d/nzeus-ssh` | dc48e42d35c5 | dc48e42d35c5 | same | nothing |
| `/etc/init.d/nzeus-usb` | ff41c7243a95 | ff41c7243a95 | same | nothing |
| `/etc/init.d/nzeus-usbnet` | 59ba9f334681 | 59ba9f334681 | same | nothing |
| `/etc/init.d/nzeus-voldown` | c26734932a45 | c26734932a45 | same | nothing |
| `/etc/init.d/nzeus-zram` | 06033ab529f5 | 06033ab529f5 | same | nothing |
| `/etc/inittab` | a334fa86afbb | a334fa86afbb | same | nothing |
| `/etc/localtime` | - | - | link (checked by target: /usr/share/zoneinfo/<my local zone>) | NEVER (the card's setting wins) |
| `/etc/motd` | 7ab87826a8fb | 126b5f387f3b | differs | copy (backup old -> /data/nzeus/sync-backup-DATE, readback) |
| `/etc/nzeus-rootfs-release` | 67fb8d071107 | 6548d88d60ca | differs | NEVER (build stamp of the image; the card's own stays) |
| `/etc/nzeus/autostart-modern` | 517ca56973c4 | - | missing-on-live | NEVER (my switch (Deck autostart)) |
| `/etc/nzeus/inputrc` | 608a495a0400 | 608a495a0400 | same | nothing |
| `/etc/nzeus/sdroot.conf` | 2eb021ae0c8c | 2eb021ae0c8c | same | nothing |
| `/etc/nzeus/usbnet-at-boot` | 4d6beb937c70 | 4d6beb937c70 | same | NEVER (my switch (deleting it turns the USB network off)) |
| `/etc/profile.d/nzeus.sh` | 866b6cfa0afc | 866b6cfa0afc | same | nothing |
| `/etc/sysctl.d/90-nzeus.conf` | 2de1f1d3d130 | 2de1f1d3d130 | same | nothing |
| `/etc/timezone` | 2f96218f000b | 2f96218f000b | same | NEVER (the card's setting wins) |
| `/root/.ssh/authorized_keys` | 5d922aaa8524 | 5d922aaa8524 | same | NEVER (my SSH keys on the card win) |
| `/usr/bin/scp` | - | - | link (checked by target: ../local/sbin/dropbearmulti-modern) | nothing |
| `/usr/local/bin/nzeus-deck` | ff76694208ac | ff76694208ac | same | nothing |
| `/usr/local/bin/nzeus-modern` | 3bf416e9b542 | d9653e7eca3b | differs | copy (backup old -> /data/nzeus/sync-backup-DATE, readback); app: use the signed updater + 60-s watcher |
| `/usr/local/bin/nzeus-status` | e34f58c3ac68 | e34f58c3ac68 | same | nothing |
| `/usr/local/bin/python` | - | - | link (checked by target: /usr/bin/python3) | nothing |
| `/usr/local/bin/usb-host-confirm` | ffc73e2785d4 | ffc73e2785d4 | same | nothing |
| `/usr/local/bin/usb-host-off` | 368cb60dc4c1 | 368cb60dc4c1 | same | nothing |
| `/usr/local/bin/usb-host-status` | aecc52aecc74 | aecc52aecc74 | same | nothing |
| `/usr/local/bin/usb-host-trial` | 6c23d9c6ef3f | 6c23d9c6ef3f | same | nothing |
| `/usr/local/bin/usbnet-confirm` | 07cf741cf3a6 | 07cf741cf3a6 | same | nothing |
| `/usr/local/bin/usbnet-down` | 11a82983b63c | 11a82983b63c | same | nothing |
| `/usr/local/bin/usbnet-up` | 131c0d6b20d4 | 131c0d6b20d4 | same | nothing |
| `/usr/local/bin/wifi-down` | fbdfadec7ac6 | fbdfadec7ac6 | same | nothing |
| `/usr/local/bin/wifi-up` | a964d096d8be | a964d096d8be | same | nothing |
| `/usr/local/lib/nzeus/deck-bin/SHA256SUMS` | 2b9c994b692c | - | missing-on-live | copy (backup old -> /data/nzeus/sync-backup-DATE, readback) |
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
| `/usr/local/lib/nzeus/usbhost/fn-hold.py` | bf74fed867d8 | bf74fed867d8 | same | nothing |
| `/usr/local/lib/nzeus/usbhost/udhcpc.script` | 44761c086224 | 44761c086224 | same | nothing |
| `/usr/local/lib/nzeus/usbhost/usb-host-confirm.sh` | 184a40b7df0f | 184a40b7df0f | same | nothing |
| `/usr/local/lib/nzeus/usbhost/usb-host-off.sh` | 18fd7e73ed7d | 18fd7e73ed7d | same | nothing |
| `/usr/local/lib/nzeus/usbhost/usb-host-status.sh` | 578246397dd6 | 578246397dd6 | same | nothing |
| `/usr/local/lib/nzeus/usbhost/usb-host-trial.sh` | 1b1c32001a3f | 1b1c32001a3f | same | nothing |
| `/usr/local/lib/nzeus/usbhost/wifi-down.sh` | cbfd8e73bafa | cbfd8e73bafa | same | nothing |
| `/usr/local/lib/nzeus/usbhost/wifi-up.sh` | 334499d3b665 | 334499d3b665 | same | nothing |
| `/usr/local/lib/nzeus/usbhost/wifi.conf.example` | a6a665503db9 | a6a665503db9 | same | nothing |
| `/usr/local/lib/nzeus/usbnet/usbnet-confirm.sh` | e5f5ecd5b489 | e5f5ecd5b489 | same | nothing |
| `/usr/local/lib/nzeus/usbnet/usbnet-down.sh` | a4697140fc58 | a4697140fc58 | same | nothing |
| `/usr/local/lib/nzeus/usbnet/usbnet-up.sh` | 96b5e4a476af | 96b5e4a476af | same | nothing |
| `/usr/local/sbin/dropbearmulti-modern` | 2cb1600373b9 | 2cb1600373b9 | same | nothing |
| `/usr/local/sbin/nz-cdeject` | f2b12e9137ea | f2b12e9137ea | same | nothing |
| `/usr/local/sbin/nzeus-battlog` | a365af57710c | a365af57710c | same | nothing |
| `/usr/local/sbin/nzeus-clock` | b4f9129e6e27 | b4f9129e6e27 | same | nothing |
| `/usr/local/sbin/nzeus-datamount` | 6bfcb30f8f90 | 6bfcb30f8f90 | same | nothing |
| `/usr/local/sbin/nzeus-ram` | c2bd492b9613 | 4c9c4f4082b7 | differs | copy (backup old -> /data/nzeus/sync-backup-DATE, readback); START-UP FILE: only with me watching a restart |
| `/usr/local/sbin/nzeus-rootcheck` | d77e1b68a621 | d77e1b68a621 | same | nothing |
| `/usr/local/sbin/nzeus-usb-gadget` | 0d891d5695a3 | 0d891d5695a3 | same | nothing |
| `/usr/local/sbin/nzeus-usb-shell` | 47db2f0896f7 | 47db2f0896f7 | same | nothing |
| `/usr/local/sbin/nzeus-usbnet-boot` | 48b7a3aed855 | 48b7a3aed855 | same | nothing |
| `/usr/local/sbin/nzeus-voldown` | 3a2ebf17014b | 3a2ebf17014b | same | nothing |
| `/usr/local/sbin/nzeus-zram` | ec92e3d9cb36 | ec92e3d9cb36 | same | nothing |
| `/usr/local/sbin/usb-regs` | 0cca5293ef5a | 0cca5293ef5a | same | nothing |

## How a real sync would run (NOT done here)

1. I connect the console; fresh read-only `collect` output replaces the docs-based column.
2. Re-run `plan`; only `differs`/`missing-on-live` rows with a copy action are candidates.
3. I approve the list. Old copies are saved to `/data/nzeus/sync-backup-DATE` first;
   every copied file is read back by hash; nothing in `/data` is changed otherwise.
4. Start-up files only while I watch a restart (lab rule 6); the app
   via the signed updater with its 60-second watcher. Never a raw device write.

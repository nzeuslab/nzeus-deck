#!/bin/sh
# Buildroot post-build: runs on the PC with $1 = target directory.
# Never touches a card or a console.
set -e
TARGET=$1
# The NZEUS pieces shared with the Debian root (linux-port/rootfs/debian/overlay):
# /usr/local/sbin/nzeus-* (root check, /data, USB port + shell, zram, clock),
# nzeus-status, nzeus-deck, usbnet helpers, static dropbear, nzeus-modern,
# motd, e2fsck.conf, /etc/nzeus/sdroot.conf. Debian-only files are skipped.
sh "$(dirname "$0")/../../../debian/apply-overlay.sh" "$TARGET" buildroot
chmod 0755 "$TARGET/usr/sbin/nz-autologin" "$TARGET"/etc/init.d/S0*nz*
mkdir -p "$TARGET/data" "$TARGET/root/.ssh"
chmod 0700 "$TARGET/root/.ssh"
# Identity file the initramfs checks before switch_root (PLAN.md, "Boot flow").
cat > "$TARGET/etc/nzeus-rootfs-release" <<EOF
NZEUS_ROOTFS=1
NZEUS_ROOTFS_LAYOUT=1
NZEUS_ROOTFS_FLAVOUR=buildroot
BUILDROOT=$BR2_VERSION
EOF

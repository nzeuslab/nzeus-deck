#!/bin/bash
# build-debian.sh [WORK_DIR] [--reuse] - build the NZEUS SD Linux root file system:
# Debian 12 "bookworm" for mipsel (the console's CPU), minimal, sysvinit, plus
# the NZEUS pieces (overlay/), then rootfs.ext4 for the "nzroot" partition.
#
# Runs in WSL Ubuntu on my PC, from the project folder:
#   wsl -d Ubuntu -- bash ports/r36s-hichip/rootfs/debian/build-debian.sh
# It asks for your Linux password (sudo): debootstrap, chroot and correct file
# owners need root. Takes ~30-60 min the first time, needs ~3 GB free on the
# WSL disk. Full explanation: README.md next to this file.
#
# WORK_DIR (default ~/nzeus-rootfs-debian) must be on the WSL Linux disk, NOT
# under /mnt/c or /mnt/d (Windows folders lose Linux owners/permissions).
#   WORK_DIR/rootfs/             the root folder (kept for --reuse)
#   WORK_DIR/out/rootfs.ext4     the image (4 GiB sparse file) + rootfs.ext4.info
#   WORK_DIR/build.log           everything that was printed
# Only the small text manifest is written into the repo (manifest/). Images
# and keys are never written into the repo.
#
#   --reuse   keep WORK_DIR/rootfs from an earlier run (skip debootstrap + apt),
#             only apply overlay/ again and make a new image (minutes, not an hour)
#   --reuse --packages   the same, but first install what is new in packages.txt
#             (and Debian's latest updates) into the kept root folder
#   --public-fatroot     the PUBLIC copy-and-go image instead: overlay-public/etc on
#             top of overlay/, then WORK_DIR/out/nzroot.img (3.5 GiB, fits FAT32)
#             + an empty WORK_DIR/out/nzdata.img (1 GiB). Use its own WORK_DIR and
#             NZ_NO_AUTHORIZED_KEYS=1 (docs/specs/2026-11-18-copy-and-go-installer-design.md)
# Environment:
#   NZ_AUTHORIZED_KEYS=FILE  your SSH PUBLIC key(s) (default ~/r36s/keys/authorized_keys
#                            if it exists). Never a private key; never committed.
#   NZ_MODERN_BIN=FILE       nzeus-modern program (default: apps/frontend-modern/mips/)
#   NZ_NO_AUTHORIZED_KEYS=1  generic fresh-card image: remove inherited owner key
#   NZ_ALLOW_NON_WSL=1       allow a normal Linux PC instead of WSL
#
# Safety: builds files on the PC only. It never writes an SD card, never
# talks to the console, never flashes or reboots anything (lab rule 8).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SUITE=bookworm
ARCH=mipsel
MAIN_MIRROR=http://deb.debian.org/debian
ARCHIVE_MIRROR=http://archive.debian.org/debian
KEYRING=/usr/share/keyrings/debian-archive-keyring.gpg
QEMU=/usr/bin/qemu-mipsel-static
HOST_PACKAGES="debootstrap qemu-user-static binfmt-support debian-archive-keyring e2fsprogs wget"
# main + non-free-firmware (firmware-realtek for the USB Wi-Fi/Bluetooth dongle)
COMPONENTS="main non-free-firmware"

die() { echo "build-debian: ERROR: $*" >&2; exit 1; }
say() { echo "build-debian: $*"; }

# ---------------------------------------------------------------- arguments
WORK=
REUSE=0
PACKAGES=0
PUBLIC=0
for a in "$@"; do
    case "$a" in
        --reuse) REUSE=1 ;;
        --packages) PACKAGES=1 ;;
        --public-fatroot) PUBLIC=1 ;;
        -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
        -*) die "unknown option $a" ;;
        *) test -z "$WORK" || die "only one WORK_DIR"; WORK=$a ;;
    esac
done
if test -n "${SUDO_USER:-}" && test "$SUDO_USER" != root; then
    USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
else
    USER_HOME=$HOME
fi
WORK=${WORK:-$USER_HOME/nzeus-rootfs-debian}
case "$WORK" in /*) ;; *) WORK=$PWD/$WORK ;; esac
if test "${NZ_NO_AUTHORIZED_KEYS:-0}" != 1 && test -z "${NZ_AUTHORIZED_KEYS:-}" && test -f "$USER_HOME/r36s/keys/authorized_keys"; then
    NZ_AUTHORIZED_KEYS=$USER_HOME/r36s/keys/authorized_keys
fi

# ---------------------------------------------------------------- where are we?
if ! grep -qiE 'microsoft|wsl' /proc/sys/kernel/osrelease 2>/dev/null; then
    test "${NZ_ALLOW_NON_WSL:-0}" = 1 || die "this is not WSL. Run it in WSL Ubuntu (or set NZ_ALLOW_NON_WSL=1 on a normal Linux PC)"
fi
case "$WORK" in
    /mnt/*) die "WORK_DIR $WORK is a Windows folder. Use a folder on the WSL disk, e.g. ~/nzeus-rootfs-debian" ;;
esac
p=$WORK
while ! test -e "$p"; do p=$(dirname "$p"); done
fstype=$(stat -f -c %T "$p")
case "$fstype" in
    ext2/ext3|ext4|btrfs|xfs|f2fs) ;;
    *) die "WORK_DIR is on a '$fstype' file system; it must be the Linux disk (ext4)" ;;
esac

# ---------------------------------------------------------------- become root
if test "$(id -u)" != 0; then
    say "needs root for debootstrap/chroot/file owners: asking sudo (your WSL password)"
    exec sudo env NZ_AUTHORIZED_KEYS="${NZ_AUTHORIZED_KEYS:-}" NZ_MODERN_BIN="${NZ_MODERN_BIN:-}" \
        NZ_NO_AUTHORIZED_KEYS="${NZ_NO_AUTHORIZED_KEYS:-0}" \
        NZ_ALLOW_NON_WSL="${NZ_ALLOW_NON_WSL:-0}" bash "$0" "$WORK" $(test $REUSE = 1 && echo --reuse) $(test $PACKAGES = 1 && echo --packages) $(test $PUBLIC = 1 && echo --public-fatroot)
fi

ROOT=$WORK/rootfs
OUT=$WORK/out
mkdir -p "$WORK" "$OUT"
exec > >(tee -a "$WORK/build.log") 2>&1
say "start $(date -u +%Y-%m-%dT%H:%M:%SZ), work folder $WORK"

# sudo can reject the owner's checkout as dubious ownership. Trust only this
# resolved project path for these read-only calls, and pin the source state now.
REPO_COMMIT=$(git -c safe.directory="$REPO" -C "$REPO" rev-parse HEAD) || die "cannot record source commit"
REPO_DIRTY=no
git -c safe.directory="$REPO" -C "$REPO" diff --quiet HEAD || REPO_DIRTY=yes
REPO_DIFF_SHA=$(git -c safe.directory="$REPO" -C "$REPO" diff --binary HEAD | sha256sum | cut -d' ' -f1)

# ---------------------------------------------------------------- host tools
missing=
for t in debootstrap chroot mke2fs e2fsck tune2fs dumpe2fs wget sha256sum awk; do
    command -v "$t" > /dev/null || missing="$missing $t"
done
test -x "$QEMU" || missing="$missing qemu-mipsel-static"
test -r "$KEYRING" || missing="$missing debian-archive-keyring"
test -z "$missing" || die "missing:$missing. Install once with: sudo apt install $HOST_PACKAGES"
avail=$(df -P -k "$WORK" | awk 'NR == 2 { print $4 }')
test "$avail" -ge $((3 * 1024 * 1024)) || die "only $((avail / 1024)) MiB free on the WSL disk; need ~3 GB"

# qemu-user via binfmt_misc: lets the PC run the console's MIPS programs inside the chroot
ensure_binfmt() {
    local b=/proc/sys/fs/binfmt_misc
    test -e "$b/register" || mount -t binfmt_misc binfmt_misc "$b" 2>/dev/null || true
    if ! test -e "$b/qemu-mipsel"; then
        command -v update-binfmts > /dev/null && update-binfmts --enable qemu-mipsel 2>/dev/null || true
    fi
    if ! test -e "$b/qemu-mipsel" && test -r /usr/lib/binfmt.d/qemu-mipsel.conf; then
        cat /usr/lib/binfmt.d/qemu-mipsel.conf > "$b/register" 2>/dev/null || true
    fi
    test -e "$b/qemu-mipsel" || die "binfmt for qemu-mipsel is not registered. Try: sudo apt install --reinstall qemu-user-static binfmt-support; sudo update-binfmts --enable qemu-mipsel (or restart WSL: wsl --shutdown)"
    test "$(head -n 1 "$b/qemu-mipsel")" = enabled || echo 1 > "$b/qemu-mipsel"
    BINFMT_INTERP=$(sed -n 's/^interpreter //p' "$b/qemu-mipsel")
    say "binfmt qemu-mipsel: enabled, interpreter $BINFMT_INTERP"
}
ensure_binfmt

# ---------------------------------------------------------------- mirror
ARCHIVED=0
if wget -q -T 30 -O /dev/null "$MAIN_MIRROR/dists/$SUITE/Release"; then
    MIRROR=$MAIN_MIRROR
    SOURCES="deb $MAIN_MIRROR $SUITE $COMPONENTS
deb $MAIN_MIRROR $SUITE-updates $COMPONENTS
deb http://security.debian.org/debian-security $SUITE-security $COMPONENTS"
elif wget -q -T 30 -O /dev/null "$ARCHIVE_MIRROR/dists/$SUITE/Release"; then
    MIRROR=$ARCHIVE_MIRROR
    SOURCES="deb $ARCHIVE_MIRROR $SUITE $COMPONENTS
deb http://archive.debian.org/debian-security $SUITE-security $COMPONENTS"
    ARCHIVED=1
    say "NOTE: $SUITE is on archive.debian.org now (no more updates there)"
else
    die "neither $MAIN_MIRROR nor $ARCHIVE_MIRROR has $SUITE (network down?)"
fi
say "mirror $MIRROR"

# ---------------------------------------------------------------- chroot helpers
MOUNTED=()
chroot_mount() {
    mountpoint -q "$ROOT/proc" || { mount -t proc proc "$ROOT/proc"; MOUNTED+=("$ROOT/proc"); }
    mountpoint -q "$ROOT/sys" || { mount -t sysfs sysfs "$ROOT/sys"; MOUNTED+=("$ROOT/sys"); }
    mountpoint -q "$ROOT/dev" || { mount --bind /dev "$ROOT/dev"; MOUNTED+=("$ROOT/dev"); }
    mkdir -p "$ROOT/dev/pts"
    mountpoint -q "$ROOT/dev/pts" || { mount --bind /dev/pts "$ROOT/dev/pts"; MOUNTED+=("$ROOT/dev/pts"); }
}
chroot_umount() {
    local i
    for ((i = ${#MOUNTED[@]} - 1; i >= 0; i--)); do
        umount "${MOUNTED[$i]}" 2>/dev/null || umount -l "${MOUNTED[$i]}" 2>/dev/null || true
    done
    MOUNTED=()
}
trap chroot_umount EXIT
in_chroot() {
    chroot "$ROOT" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
        DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true "$@"
}
mounted_below() { awk -v r="$1" 'index($2, r "/") == 1 || $2 == r { f = 1 } END { exit !f }' /proc/mounts; }
safe_remove_root() {
    mounted_below "$ROOT" && die "something is still mounted below $ROOT - not deleting (reboot WSL: wsl --shutdown)"
    rm -rf --one-file-system "$ROOT"
}
# The qemu program inside the chroot (needed when binfmt has no "F" flag);
# removed again before the image is made.
put_qemu() {
    install -D -m 755 "$QEMU" "$ROOT$QEMU"
    if test -n "$BINFMT_INTERP" && test "$BINFMT_INTERP" != "$QEMU"; then
        install -D -m 755 "$BINFMT_INTERP" "$ROOT$BINFMT_INTERP"
    fi
}
APT_OPTS=(-y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold)

# ---------------------------------------------------------------- 3. packages (function)
install_packages() {
    say "3/6 packages (packages.txt)"
    put_qemu
    chroot_mount
    rm -f "$ROOT/etc/resolv.conf"
    cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
    printf '#!/bin/sh\n# build time only: do not start services in the chroot\nexit 101\n' > "$ROOT/usr/sbin/policy-rc.d"
    chmod 755 "$ROOT/usr/sbin/policy-rc.d"
    # apt/dpkg settings first, so no docs are unpacked and systemd/udev/dbus are blocked
    for d in etc/apt/apt.conf.d etc/apt/preferences.d etc/dpkg/dpkg.cfg.d; do
        for f in "$HERE/overlay/$d"/*; do install -D -m 644 "$f" "$ROOT/$d/$(basename "$f")"; done
    done
    echo "$SOURCES" > "$ROOT/etc/apt/sources.list"
    if test "$ARCHIVED" = 1; then
        # archive.debian.org keeps the last Release files, whose "valid until" date has passed
        echo 'Acquire::Check-Valid-Until "false";' > "$ROOT/etc/apt/apt.conf.d/91nzeus-archive"
    fi
    in_chroot apt-get update
    in_chroot apt-get "${APT_OPTS[@]}" dist-upgrade
    pkgs=$(sed 's/#.*//' "$HERE/packages.txt" | xargs)
    in_chroot apt-get "${APT_OPTS[@]}" --no-install-recommends install $pkgs
    grep -h '^Date:' "$ROOT"/var/lib/apt/lists/*_dists_${SUITE}_InRelease 2>/dev/null | head -n 1 > "$WORK/release-date.txt" || true
    for bad in systemd systemd-sysv udev dbus dbus-daemon; do
        if in_chroot dpkg-query -W -f='${Status}' "$bad" 2>/dev/null | grep -q 'install ok installed'; then
            die "$bad got installed (RAM!) - find which package pulled it in"
        fi
    done
    in_chroot dpkg-query -W -f='${Status}' sysvinit-core | grep -q 'install ok installed' || die "sysvinit-core is not installed"
    touch "$WORK/.stage-packages"
}

# ---------------------------------------------------------------- 1.+2. base system
if test $REUSE = 1 && test -e "$WORK/.stage-packages"; then
    say "--reuse: keeping $ROOT (base + packages from an earlier run)"
    if test $PACKAGES = 1; then
        install_packages          # new lines in packages.txt (and Debian updates)
    fi
else
    test $REUSE = 1 && say "--reuse asked, but no finished earlier run: building fresh"
    rm -f "$WORK/.stage-base" "$WORK/.stage-packages"
    if test -e "$ROOT"; then say "removing the old root folder $ROOT"; safe_remove_root; fi
    say "1/6 debootstrap first stage ($SUITE $ARCH minbase)"
    debootstrap --foreign --arch="$ARCH" --variant=minbase --keyring="$KEYRING" "$SUITE" "$ROOT" "$MIRROR"
    put_qemu
    say "2/6 debootstrap second stage (MIPS programs under qemu: slow, 10-30 min)"
    chroot "$ROOT" /debootstrap/debootstrap --second-stage
    chroot "$ROOT" /bin/true || die "cannot run MIPS programs in the chroot (binfmt/qemu)"
    touch "$WORK/.stage-base"
    install_packages
fi

# ---------------------------------------------------------------- 4. NZEUS overlay + settings
say "4/6 NZEUS overlay and settings"
put_qemu
chroot_mount
NZ_AUTHORIZED_KEYS=${NZ_AUTHORIZED_KEYS:-} NZ_MODERN_BIN=${NZ_MODERN_BIN:-} sh "$HERE/apply-overlay.sh" "$ROOT" debian
if test "$PUBLIC" = 1; then
    # copy-and-go image: the few files that differ (only etc/, never the README)
    test "${NZ_NO_AUTHORIZED_KEYS:-0}" = 1 || die "--public-fatroot needs NZ_NO_AUTHORIZED_KEYS=1 (no owner key in a public image)"
    for f in fstab motd nzeus/sdroot.conf; do
        install -o 0 -g 0 -m 0644 "$HERE/overlay-public/etc/$f" "$ROOT/etc/$f"
    done
    install -d -o 0 -g 0 -m 0755 "$ROOT/sdcard" "$ROOT/data"
    # boot log on the card (nzeus/boot-log.txt) for the first 3 minutes, the very first step
    install -o 0 -g 0 -m 0755 "$HERE/overlay-public/etc/init.d/nzeus-bootlog" "$ROOT/etc/init.d/nzeus-bootlog"
    say "public copy-and-go files applied (fstab, motd, sdroot.conf, /sdcard, nzeus-bootlog)"
fi
# Enable in dependency order (insserv refuses a service whose Required-Start
# service is not enabled yet: nzeus-clock needs nzeus-data needs nzeus-rootfs).
enabled=" "
for n in nzeus-usb nzeus-ram nzeus-zram nzeus-rootfs nzeus-data nzeus-clock nzeus-ssh nzeus-usbnet nzeus-battlog nzeus-voldown nzeus-modern; do
    test -e "$ROOT/etc/init.d/$n" || die "overlay has no init script $n"
    in_chroot update-rc.d "$n" defaults || die "update-rc.d $n failed"
    enabled="$enabled$n "
done
for s in "$HERE"/overlay/etc/init.d/*; do
    case "$enabled" in *" $(basename "$s") "*) ;; *) die "init script $(basename "$s") is not in the enable list" ;; esac
done
if test "$PUBLIC" = 1; then
    in_chroot update-rc.d nzeus-bootlog defaults || die "update-rc.d nzeus-bootlog failed"
fi
# things that must not run here: no clock chip (hwclock.sh), no modules (kmod),
# no udev/dbus, our own SSH start, and "brightness" (it would write a saved
# level to the AVP backlight at every start; NZEUS Deck sets brightness itself)
for s in hwclock.sh kmod udev dbus rsyslog cron networking ssh dropbear brightness; do
    if test -e "$ROOT/etc/init.d/$s"; then in_chroot update-rc.d -f "$s" remove; fi
done
# root: no password at all ('*' = password login impossible, but NOT "locked",
# which Dropbear would treat as "refuse even keys"). USB shell needs none.
in_chroot usermod -p '*' root
# NZEUS Deck (Python app, Files .py, Terminal) runs the command "python"; Debian has python3.
if test -x "$ROOT/usr/bin/python3"; then ln -sf /usr/bin/python3 "$ROOT/usr/local/bin/python"; fi
# Time zone: UTC, or the zone named by NZ_TIMEZONE (e.g. NZ_TIMEZONE=Europe/London);
# the Deck lets everyone pick their own later. UTC if that zone's data is missing.
TZONE=${NZ_TIMEZONE:-Etc/UTC}
if test -e "$ROOT/usr/share/zoneinfo/$TZONE"; then
    echo "$TZONE" > "$ROOT/etc/timezone"; ln -sf "/usr/share/zoneinfo/$TZONE" "$ROOT/etc/localtime"
else
    echo Etc/UTC > "$ROOT/etc/timezone"
    if test -e "$ROOT/usr/share/zoneinfo/Etc/UTC"; then ln -sf /usr/share/zoneinfo/Etc/UTC "$ROOT/etc/localtime"; fi
fi
# Host keys must be made on the console (first start of nzeus-ssh), not shared by every image.
rm -f "$ROOT"/etc/dropbear/dropbear_*_host_key

# start-up order as insserv made it (the real Debian tools, not a guess)
order=$(ls "$ROOT/etc/rcS.d" | sed -n 's/^S[0-9][0-9]//p' | tr '\n' ' ')
say "rcS order: $order"
pos() { echo " $order " | awk -v n="$1" '{ for (i = 1; i <= NF; i++) if ($i == n) { print i; exit } print 0 }'; }
before() { local a b; a=$(pos "$1"); b=$(pos "$2"); test "$a" -gt 0 && test "$b" -gt 0 && test "$a" -lt "$b" || die "start-up order: $1 must come before $2 ($order)"; }
before nzeus-usb checkroot.sh
if test "$PUBLIC" = 1; then
    before nzeus-bootlog nzeus-ram
    before nzeus-bootlog nzeus-usb
fi
before checkroot.sh nzeus-rootfs
before nzeus-rootfs checkroot-bootclean.sh
before nzeus-rootfs mountall.sh
before mountall.sh nzeus-data
before nzeus-data nzeus-clock
# the clock must also be saved when switching off / restarting
for d in rc0.d rc6.d; do
    ls "$ROOT/etc/$d" | grep -q '^K[0-9][0-9]nzeus-clock$' || die "nzeus-clock has no stop link in $d (clock would not be saved at power-off)"
done
grep -q '^GS0:.*nzeus-usb-shell' "$ROOT/etc/inittab" || die "inittab has no USB shell"
test -x "$ROOT/sbin/init" || die "no /sbin/init"

# ---------------------------------------------------------------- 5. clean up
say "5/6 clean up (apt caches, docs, logs, build-only files)"
in_chroot apt-get clean
find "$ROOT/var/lib/apt/lists" -mindepth 1 -maxdepth 1 -type f -delete
rm -f "$ROOT"/var/cache/apt/*.bin
find "$ROOT/usr/share/doc" -mindepth 1 ! -type d ! -name copyright -delete 2>/dev/null || true
rm -rf "$ROOT"/usr/share/man/* "$ROOT"/usr/share/info/*
find "$ROOT/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name locale.alias -exec rm -rf {} + 2>/dev/null || true
find "$ROOT/var/log" -type f -exec truncate -s 0 {} +
rm -f "$ROOT/usr/sbin/policy-rc.d" "$ROOT/root/.bash_history"
rm -rf "$ROOT"/tmp/* "$ROOT"/var/tmp/*
if test -e "$ROOT/etc/machine-id"; then : > "$ROOT/etc/machine-id"; fi
rm -f "$ROOT/etc/resolv.conf"
printf '# No DNS server set yet (the USB network has no internet).\n# Wi-Fi/DHCP or you (nameserver 1.1.1.1) fill this in.\n' > "$ROOT/etc/resolv.conf"
chroot_umount
rm -f "$ROOT$QEMU"
if test -n "$BINFMT_INTERP"; then rm -f "$ROOT$BINFMT_INTERP"; fi
# privacy: no private keys anywhere we put things
if grep -rlI 'PRIVATE KEY' "$ROOT/root" "$ROOT/etc/nzeus" "$ROOT/usr/local" 2>/dev/null | grep -q .; then
    die "a PRIVATE key ended up in the root folder - not making an image"
fi

# ---------------------------------------------------------------- 6. image + manifest
say "6/6 image"
# Refuse to label an image with a commit/diff that changed during this build.
test "$(git -c safe.directory="$REPO" -C "$REPO" rev-parse HEAD)" = "$REPO_COMMIT" || die "source commit changed during build"
test "$(git -c safe.directory="$REPO" -C "$REPO" diff --binary HEAD | sha256sum | cut -d' ' -f1)" = "$REPO_DIFF_SHA" || die "tracked sources changed during build"
if test "$PUBLIC" = 1; then
    # nzroot.img must fit a FAT32 file (< 4 GiB); nzdata.img starts empty except the Deck's folder.
    # An earlier pair becomes *.prev (make-image.sh never overwrites an image).
    for f in nzroot.img nzdata.img; do
        if test -e "$OUT/$f"; then mv -f "$OUT/$f" "$OUT/$f.prev" && say "the previous $f is now $OUT/$f.prev"; fi
    done
    bash "$HERE/make-image.sh" "$ROOT" "$OUT/nzroot.img" 3584M
    rm -f "$OUT/nzdata.img" && D=$(mktemp -d) && install -d -m 0755 "$D/nzeus"
    truncate -s 1G "$OUT/nzdata.img"
    mkfs.ext4 -q -F -L nzdata -b 4096 -E root_owner=0:0 -d "$D" "$OUT/nzdata.img"
    rmdir "$D/nzeus" "$D"
    test "$(stat -c %s "$OUT/nzroot.img")" = 3758096384 || die "nzroot.img is not 3.5 GiB"
    test "$(stat -c %s "$OUT/nzdata.img")" = 1073741824 || die "nzdata.img is not 1 GiB"
    if test -n "${SUDO_UID:-}"; then chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT/nzroot.img" "$OUT/nzdata.img"; fi
    say "public images: $OUT/nzroot.img + $OUT/nzdata.img (NOT in the repo)"
    say "BUILD_COMPLETE $(date -u +%Y-%m-%dT%H:%M:%SZ) - nothing was written to any card"
    exit 0
fi
if test -e "$OUT/rootfs.ext4"; then
    mv -f "$OUT/rootfs.ext4" "$OUT/rootfs.ext4.prev"
    mv -f "$OUT/rootfs.ext4.info" "$OUT/rootfs.ext4.prev.info" 2>/dev/null || true
    say "the previous image is now $OUT/rootfs.ext4.prev"
fi
bash "$HERE/make-image.sh" "$ROOT" "$OUT/rootfs.ext4"

M=$HERE/manifest
mkdir -p "$M"
{
    echo "NZEUS SD Linux root file system - Debian manifest (written by build-debian.sh)"
    echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "suite=$SUITE arch=$ARCH variant=minbase mirror=$MIRROR"
    echo "debian_release_$(cat "$WORK/release-date.txt" 2>/dev/null || echo 'Date: unknown')"
    echo "repo_commit=$REPO_COMMIT"
    echo "repo_tracked_changes=$REPO_DIRTY"
    echo "repo_tracked_diff_sha256=$REPO_DIFF_SHA"
    echo "deck_helpers_manifest_sha256=$(sha256sum "$ROOT/usr/local/lib/nzeus/deck-bin/SHA256SUMS" | cut -d' ' -f1)"
    echo "tree_bytes=$(du -s -B1 --apparent-size "$ROOT" | cut -f1)"
    echo "tree_files=$(find "$ROOT" -xdev | wc -l)"
    echo "authorized_keys=$(test -s "$ROOT/root/.ssh/authorized_keys" && echo yes || echo no)"
    echo "nzeus_modern_sha256=$(sha256sum "$ROOT/usr/local/bin/nzeus-modern" | cut -d' ' -f1)"
    cat "$OUT/rootfs.ext4.info"
    echo
    echo "rcS order: $order"
    echo
    echo "installed packages (name version arch installed-size-KiB):"
    awk '/^Package: /{p=$2} /^Version: /{v=$2} /^Architecture: /{a=$2} /^Installed-Size: /{s=$2}
         /^Status: /{st=$0} /^$/{ if (st ~ /install ok installed/) printf "  %s %s %s %s\n", p, v, a, s; p=v=a=s=st="" }
         END { if (st ~ /install ok installed/) printf "  %s %s %s %s\n", p, v, a, s }' \
        "$ROOT/var/lib/dpkg/status" | sort
} > "$M/debian-rootfs-manifest.txt"
if test -n "${SUDO_UID:-}"; then chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$M/debian-rootfs-manifest.txt" 2>/dev/null || true; fi
say "manifest: $M/debian-rootfs-manifest.txt (commit only this text file)"
say "image:    $OUT/rootfs.ext4 (NOT in the repo; do not commit images)"
say "next (PC only): python3 ports/r36s-hichip/rootfs/region-tool/nz_region.py selftest --workdir ~/nzeus-region-test --rootfs-ext4 $OUT/rootfs.ext4"
say "BUILD_COMPLETE $(date -u +%Y-%m-%dT%H:%M:%SZ) - nothing was written to any card"

#!/bin/sh
# apply-overlay.sh ROOT_DIR [debian|buildroot]
# Put the NZEUS pieces into a root folder on the PC (never a card, never the
# console). Used by build-debian.sh (debian), by the Buildroot post-build
# script (buildroot) and by test-debian-pc.sh on a fake root folder.
#
#   overlay/                         -> ROOT (modes set here, owner = whoever runs it;
#                                       build-debian.sh runs as root, Buildroot's
#                                       image step makes everything root-owned)
#   ../../usbnet/usbnet-*.sh         -> /usr/local/lib/nzeus/usbnet/ (unchanged copies)
#   ../../usbhost/ scripts            -> /usr/local/lib/nzeus/usbhost/ (unchanged copies)
#   ../../usbhost/nz-cdeject-mipsel, usb-regs-mipsel
#                                    -> /usr/local/sbin/nz-cdeject, usb-regs (sha256 checked)
#   ../../usbnet/dropbear/dropbearmulti-modern-mipsel
#                                    -> /usr/local/sbin/dropbearmulti-modern (sha256 checked)
#   ../../../../core/deck-modern/ (required-files.txt)
#                                    -> /usr/local/lib/nzeus/deck-bin/ (+ SHA256SUMS)
#   ../../../../core/deck-modern/bt/nzeus-bt-mipsel
#                                    -> /usr/local/lib/nzeus/deck-bin/nzeus-bt.bin (sha256 checked)
#   ../../../../core/deck-modern/python-examples/*.py
#                                    -> /usr/local/lib/nzeus/python-examples/ (the Python app's examples)
#   emoji pack (tools/make_emoji.py) -> /usr/local/lib/nzeus/emoji/ (skipped with a warning if
#                                       not made; nzeus-deck links NZ_DIR/emoji to it)
#   nzeus-modern (static)            -> /usr/local/bin/nzeus-modern
#   /etc/nzeus-rootfs-release        (the RAM system checks NZEUS_ROOTFS_LAYOUT=1)
#   NZ_AUTHORIZED_KEYS=FILE          -> /root/.ssh/authorized_keys (public keys only;
#                                       a file with a private key is refused)
#
# "buildroot" skips the Debian-only files (inittab, fstab, init.d, apt, dpkg,
# halt, locale): Buildroot keeps its own BusyBox init files, which call the
# same /usr/local/sbin scripts.
#
# Environment: NZ_MODERN_BIN (default apps/frontend-modern/mips/nzeus-modern, else
# the cloud test build nzeus-modern-cloud with a warning), NZ_AUTHORIZED_KEYS,
# NZ_EMOJI_DIR (default core/deck-modern/build/emoji, made by tools/make_emoji.py).
# POSIX sh (dash). Exit 1 on any problem.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
OVL=$HERE/overlay
ROOT=${1:?usage: apply-overlay.sh ROOT_DIR [debian|buildroot]}
FLAVOUR=${2:-debian}
CR=$(printf '\r')

die() { echo "apply-overlay: $*" >&2; exit 1; }
case "$FLAVOUR" in debian|buildroot) ;; *) die "flavour must be debian or buildroot" ;; esac
test -d "$ROOT" || die "$ROOT is not a folder"
case "$(cd "$ROOT" && pwd)" in /) die "refusing to write into / of this PC" ;; esac

# put SRC DEST MODE: copy one file, refusing Windows line ends in text files
put() {
    test -f "$1" || die "missing $1"
    if ! is_binary "$1" && grep -q "$CR" "$1"; then
        die "$1 has Windows (CRLF) line ends - scripts would not run"
    fi
    mkdir -p "$(dirname "$2")"
    rm -f "$2"                      # replace links/old files, never write through a link
    cp "$1" "$2"
    chmod "$3" "$2"
}
is_binary() { test "$(head -c 4 "$1" | tail -c 3)" = ELF; }

count=0
for f in $(cd "$OVL" && find . -type f | sed 's|^\./||' | sort); do
    case "$FLAVOUR:$f" in
        buildroot:etc/inittab|buildroot:etc/fstab|buildroot:etc/init.d/*|buildroot:etc/apt/*|\
        buildroot:etc/dpkg/*|buildroot:etc/default/halt|buildroot:etc/default/locale|\
        buildroot:etc/profile.d/*|buildroot:etc/hostname|buildroot:etc/hosts|buildroot:etc/sysctl.d/*)
            continue ;;
    esac
    case "$f" in
        etc/init.d/*|usr/local/sbin/*|usr/local/bin/*) mode=755 ;;
        *) mode=644 ;;
    esac
    put "$OVL/$f" "$ROOT/$f" "$mode"
    count=$((count + 1))
done
echo "apply-overlay: $count overlay files ($FLAVOUR)"

# USB network helpers, unchanged (see ports/r36s-hichip/usbnet/README.md)
for s in usbnet-up.sh usbnet-down.sh usbnet-confirm.sh; do
    put "$REPO/ports/r36s-hichip/usbnet/$s" "$ROOT/usr/local/lib/nzeus/usbnet/$s" 755
done

# USB host mode + Wi-Fi helpers, unchanged (see ports/r36s-hichip/usbhost/README.md)
UHDIR=$REPO/ports/r36s-hichip/usbhost
for s in usb-host-trial.sh usb-host-confirm.sh usb-host-off.sh usb-host-status.sh \
         wifi-up.sh wifi-down.sh udhcpc.script; do
    put "$UHDIR/$s" "$ROOT/usr/local/lib/nzeus/usbhost/$s" 755
done
put "$UHDIR/wifi.conf.example" "$ROOT/usr/local/lib/nzeus/usbhost/wifi.conf.example" 644
# FN-hold watcher: the owner's "PC cable is out" for the trial (the port cannot sense it)
put "$UHDIR/fn-hold.py" "$ROOT/usr/local/lib/nzeus/usbhost/fn-hold.py" 644
# its two static programs (CD-mode eject, read-only USB register reader), checked
for b in nz-cdeject usb-regs; do
    want=$(awk -v f="$b-mipsel" '$2 == f { print $1 }' "$UHDIR/SHA256SUMS")
    got=$(sha256sum "$UHDIR/$b-mipsel" | cut -d' ' -f1)
    test -n "$want" && test "$want" = "$got" || die "$b-mipsel does not match ports/r36s-hichip/usbhost/SHA256SUMS"
    put "$UHDIR/$b-mipsel" "$ROOT/usr/local/sbin/$b" 755
done

# NZEUS Deck's helper programs for the SD Debian (core/deck-modern/README.md):
# the Deck runs NZ_DIR/bin/<helper>; nzeus-deck links that folder to here.
DMDIR=$REPO/core/deck-modern
DB=$ROOT/usr/local/lib/nzeus/deck-bin
# Reused build roots may still contain the removed stock-era access stub.
# This exact obsolete path is owned by NZEUS; leave unrelated local tools alone.
rm -f "$DB/nzeus-claude-access.sh"
# Explicit inventory: missing helpers must not silently produce a partial image.
test -s "$DMDIR/required-files.txt" || die "missing Deck helper inventory"
while read -r f; do
    case "$f" in ''|.*|*/*|*[!A-Za-z0-9._-]*) die "bad Deck helper name: $f" ;; esac
    put "$DMDIR/$f" "$DB/$f" 755
done < "$DMDIR/required-files.txt"
(
    cd "$DB"
    while read -r f; do sha256sum "$f"; done < "$DMDIR/required-files.txt"
) > "$DB/SHA256SUMS"
# nzeus-bt, the Bluetooth program (static MIPS, core/deck-modern/bt/build.sh);
# the deck-bin/nzeus-bt wrapper runs it. Checked against its SHA256SUMS.
BTDIR=$DMDIR/bt
want=$(awk '$2 == "nzeus-bt-mipsel" { print $1 }' "$BTDIR/SHA256SUMS")
got=$(sha256sum "$BTDIR/nzeus-bt-mipsel" | cut -d' ' -f1)
test -n "$want" && test "$want" = "$got" || die "nzeus-bt-mipsel does not match core/deck-modern/bt/SHA256SUMS"
put "$BTDIR/nzeus-bt-mipsel" "$DB/nzeus-bt.bin" 755

# The Python app's examples (app_python.c: /usr/local/lib/nzeus/python-examples)
n=0
for f in "$DMDIR"/python-examples/*.py; do
    test -f "$f" || continue
    put "$f" "$ROOT/usr/local/lib/nzeus/python-examples/${f##*/}" 644
    n=$((n + 1))
done
test "$n" -gt 0 || die "no Python examples in $DMDIR/python-examples"

# Emoji pictures for text (WhatsApp...): made on the PC by tools/make_emoji.py
# (Twemoji, CC-BY 4.0 - LICENSE.txt goes with them). Binary pictures: copied
# as they are (no line-end check). nzeus-deck links NZ_DIR/emoji here.
EM=${NZ_EMOJI_DIR:-$DMDIR/build/emoji}
if test -s "$EM/index.txt" && test -f "$EM/LICENSE.txt"; then
    ED=$ROOT/usr/local/lib/nzeus/emoji
    rm -rf "$ED" && mkdir -p "$ED"
    for f in "$EM"/*; do
        case "${f##*/}" in
            *.bin|index.txt|LICENSE.txt) cp "$f" "$ED/" && chmod 644 "$ED/${f##*/}" || die "cannot copy $f" ;;
        esac
    done
    echo "apply-overlay: emoji: $(ls "$ED" | grep -c '\.bin$') pictures"
else
    echo "apply-overlay: WARNING no emoji pack in $EM (python tools/make_emoji.py): text shows no emoji pictures"
fi

# The static Dropbear built for the modern kernel (fallback SSH server; Debian's
# own dropbear-bin is used when present). Checked against its SHA256SUMS.
DBDIR=$REPO/ports/r36s-hichip/usbnet/dropbear
want=$(awk '$2 ~ /dropbearmulti-modern-mipsel$/ { print $1 }' "$DBDIR/SHA256SUMS")
got=$(sha256sum "$DBDIR/dropbearmulti-modern-mipsel" | cut -d' ' -f1)
test -n "$want" && test "$want" = "$got" || die "dropbearmulti-modern-mipsel does not match its SHA256SUMS"
put "$DBDIR/dropbearmulti-modern-mipsel" "$ROOT/usr/local/sbin/dropbearmulti-modern" 755
# Server side of 'scp -O' from the PC: Dropbear starts root logins with
# PATH=/usr/sbin:/usr/bin:/sbin:/bin (no /usr/local), and Debian's dropbear-bin
# has no scp - so /usr/bin/scp = the multi-call program (seen missing on the
# console's RAM system 2026-10-10). A real scp from a package is left alone.
if ! test -e "$ROOT/usr/bin/scp"; then
    mkdir -p "$ROOT/usr/bin"
    ln -s ../local/sbin/dropbearmulti-modern "$ROOT/usr/bin/scp"
fi

# NZEUS Deck for the modern kernel (static). The PC build is preferred.
MB=${NZ_MODERN_BIN:-}
if test -z "$MB"; then
    MB=$REPO/apps/frontend-modern/mips/nzeus-modern
    if ! test -f "$MB"; then
        MB=$REPO/apps/frontend-modern/mips/nzeus-modern-cloud
        echo "apply-overlay: WARNING: apps/frontend-modern/mips/nzeus-modern not built;"
        echo "               using the cloud TEST build nzeus-modern-cloud (see apps/frontend-modern/README.md)"
    fi
fi
test -f "$MB" || die "no nzeus-modern binary ($MB)"
is_binary "$MB" || die "$MB is not a program (ELF)"
put "$MB" "$ROOT/usr/local/bin/nzeus-modern" 755

# folders the scripts expect
mkdir -p "$ROOT/data" "$ROOT/mnt/stock" "$ROOT/root" "$ROOT/etc/nzeus"
chmod 755 "$ROOT/data" "$ROOT/mnt/stock"
chmod 700 "$ROOT/root"

# SSH public keys from a file on the PC (never committed). Public keys only.
if test "${NZ_NO_AUTHORIZED_KEYS:-0}" = 1; then
    # A generic fresh-card image must not inherit the builder's root access.
    rm -f "$ROOT/root/.ssh/authorized_keys"
    echo "apply-overlay: generic image: no owner SSH key (SSH stays off)"
elif test -n "${NZ_AUTHORIZED_KEYS:-}"; then
    k=$NZ_AUTHORIZED_KEYS
    test -f "$k" || die "NZ_AUTHORIZED_KEYS=$k is not a file"
    grep -q 'PRIVATE KEY' "$k" && die "$k contains a PRIVATE key - give the .pub file instead"
    if grep -v '^[[:space:]]*#' "$k" | grep -v '^[[:space:]]*$' |
        grep -q -v -E '^(ssh-ed25519|ecdsa-sha2-nistp(256|384|521)|ssh-rsa) [A-Za-z0-9+/=]+( .*)?$'; then
        die "$k has lines that are not ssh-ed25519 / ecdsa / ssh-rsa public keys"
    fi
    grep -q -E '^(ssh-ed25519|ecdsa-sha2-|ssh-rsa) ' "$k" || die "$k has no public key line"
    mkdir -p "$ROOT/root/.ssh"
    chmod 700 "$ROOT/root/.ssh"
    put "$k" "$ROOT/root/.ssh/authorized_keys" 600
    echo "apply-overlay: $(grep -c -E '^(ssh-|ecdsa-)' "$k") SSH public key(s) installed for root"
else
    echo "apply-overlay: no NZ_AUTHORIZED_KEYS: SSH stays off until /root/.ssh/authorized_keys exists"
fi

# identity file checked by the RAM system before switch_root (PLAN.md "Boot flow")
{
    echo "NZEUS_ROOTFS=1"
    echo "NZEUS_ROOTFS_LAYOUT=1"
    echo "NZEUS_ROOTFS_FLAVOUR=$FLAVOUR"
    echo "NZEUS_ROOTFS_BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$ROOT/etc/nzeus-rootfs-release"
chmod 644 "$ROOT/etc/nzeus-rootfs-release"
echo "apply-overlay: done ($ROOT)"

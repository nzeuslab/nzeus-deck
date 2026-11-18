#!/bin/bash
# privacy-scan-image.sh ROOT_DIR - privacy scan of a built nzroot root folder
# (the folder rootfs.ext4 is made from; provenance.py checks they match).
# Lab rule 10: "run the privacy scan on every build".
#
# Checks (prints file NAMES only, never the matched words):
#   1. no private key body anywhere in the tree
#   2. no file with my private words (release/private_words.txt) in
#      the NZEUS-managed places (/usr/local, /etc, /root, /data, /home)
#   3. same with the country word list, NZEUS places only (Debian's own time
#      zone data legitimately names countries)
#   4. no Wi-Fi configuration (only the shipped wifi.conf.example, no psk= line)
#   5. no picture files in the NZEUS places (Microsoft art - Bliss, XP icons,
#      Windows logo - lives only on my console's /data)
#   6. /data in the image is empty (user files never come from the PC)
#   7. no file identical to the private word lists themselves
# Read-only. Exit 0 = clean, 1 = something to look at.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
ROOT=${1:?usage: privacy-scan-image.sh ROOT_DIR}
WORDS=${NZ_PRIVATE_WORDS_DIR:-$REPO/release}    # my lists live in the private repo
P=$WORDS/private_words.txt
PC=$WORDS/private_country_words.txt
test -f "$P" && test -f "$PC" || { echo "privacy: missing the private word lists"; exit 1; }
NZ_PLACES=("$ROOT/usr/local" "$ROOT/etc" "$ROOT/root" "$ROOT/data" "$ROOT/home")
bad=0
check() {  # NAME FILES...
    local name=$1; shift
    if test -n "$*"; then echo "FAIL $name:"; printf '   %s\n' "$@"; bad=1; else echo "PASS $name"; fi
}

# Files that came from Debian packages (dpkg's own file lists) are Debian's, not
# mine: e.g. GnuTLS carries public self-test EC keys, /etc/services has
# service names that contain a listed word. They are listed, not failed.
DEB=$(mktemp)
cat "$ROOT"/var/lib/dpkg/info/*.list 2>/dev/null | sed "s|^|$ROOT|" | sort -u > "$DEB"
notdeb() { grep -v -x -F -f "$DEB"; }
mapfile -t r < <(grep -rlaP -e '-----BEGIN (OPENSSH|RSA|EC|DSA)? ?PRIVATE KEY-----\s*[A-Za-z0-9+/]{20}' "$ROOT" 2>/dev/null | notdeb)
check "no private key bodies in the whole tree (outside Debian's own package files)" "${r[@]}"
mapfile -t r < <(grep -rlaP -e '-----BEGIN (OPENSSH|RSA|EC|DSA)? ?PRIVATE KEY-----\s*[A-Za-z0-9+/]{20}' "$ROOT" 2>/dev/null | grep -x -F -f "$DEB")
test -n "${r[*]}" && { echo "NOTE Debian package files with built-in (public test) key text:"; printf '   %s\n' "${r[@]}"; }
# My PUBLIC SSH key is meant to be in the image; its comment field may
# name me, so the image itself stays private (never published).
mapfile -t r < <(grep -rlai -F -f "$P" "${NZ_PLACES[@]}" 2>/dev/null | notdeb | grep -v -x -F "$ROOT/root/.ssh/authorized_keys")
check "no private words in NZEUS places (outside Debian's package files)" "${r[@]}"
if grep -qai -F -f "$P" "$ROOT/root/.ssh/authorized_keys" 2>/dev/null; then
    echo "WARN the SSH public key's comment names a person: keep this image private (it is never published)"
fi
mapfile -t r < <(grep -rlai -F -f "$PC" "${NZ_PLACES[@]}" 2>/dev/null | grep -v -e '/etc/timezone$')
check "no private country words in NZEUS places (except /etc/timezone, the chosen zone)" "${r[@]}"
mapfile -t r < <(find "$ROOT" -xdev \( -iname 'wifi.conf' -o -iname 'wpa_supplicant*.conf' -o -iname '*.nmconnection' \) 2>/dev/null | notdeb)
check "no Wi-Fi configuration files" "${r[@]}"
# a real password line: psk= followed by a value that is not a variable or the
# shipped placeholder (wifi-up.sh writes psk=$psk; the example says YOUR-WIFI-PASSWORD)
mapfile -t r < <(grep -rlaiE '^[[:space:]]*psk="?[^"$[:space:]]' "$ROOT/usr/local" "$ROOT/etc" "$ROOT/root" "$ROOT/data" 2>/dev/null |
    while read -r f; do grep -aiE '^[[:space:]]*psk="?[^"$[:space:]]' "$f" | grep -qv 'YOUR-WIFI-PASSWORD' && echo "$f"; done)
check "no Wi-Fi password lines" "${r[@]}"
mapfile -t r < <(find "${NZ_PLACES[@]}" -xdev -type f \( -iname '*.bmp' -o -iname '*.png' -o -iname '*.jpg' \
    -o -iname '*.jpeg' -o -iname '*.ico' -o -iname '*.gif' -o -iname '*bliss*' -o -iname '*xp*.bmp' \) 2>/dev/null)
check "no picture files in NZEUS places" "${r[@]}"
mapfile -t r < <(find "$ROOT/data" -mindepth 1 2>/dev/null)
check "/data is empty" "${r[@]}"
ps=$(sha256sum "$P" "$PC" | cut -d' ' -f1 | sort -u)
mapfile -t r < <(find "$ROOT" -xdev -type f -size "-$(( $(stat -c %s "$P" "$PC" | sort -n | tail -n 1) + 1 ))c" \
    -size "+$(( $(stat -c %s "$P" "$PC" | sort -n | head -n 1) - 1 ))c" -print0 2>/dev/null |
    xargs -0 -r sha256sum | grep -F -f <(echo "$ps") | cut -d' ' -f3-)
check "no copy of the private word lists" "${r[@]}"
if test $bad = 0; then echo "privacy scan (image): clean"; else echo "privacy scan (image): LOOK AT THE FAILS ABOVE"; fi
rm -f "$DEB"
exit $bad

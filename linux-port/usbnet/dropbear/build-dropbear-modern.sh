#!/bin/bash
# Builds Dropbear SSH for the MODERN Linux port (7.2.0-rc5 on the R36S):
# one static file, "dropbearmulti-modern-mipsel" (server + client + scp + key tool).
#
# Plain Dropbear (the modern kernel has real TCP/IP and PTYs), built with
# Ubuntu's normal MIPS cross compiler + glibc 2.39. (The older HiChip-SDK build
# segfaults on this kernel - see README.md, "The crash".)
#
# Usage (WSL Ubuntu, from the R36S folder):
#   sudo apt install gcc-mipsel-linux-gnu qemu-user      (once)
#   bash linux-port/usbnet/dropbear/build-dropbear-modern.sh [SOURCE]
# SOURCE = the Dropbear 2026.94 release tarball OR a folder with the 2026.94
#          source. Default: ~/r36s/downloads/dropbear-2026.94.tar.bz2, downloaded
#          from the Dropbear site the first time. A tarball is always checked
#          against the pinned SHA-256 below before it is unpacked.
# Uses localoptions.h (features: keys only, no passwords) and nss_fallback.c
# (users from /etc/passwd without glibc's NSS plug-ins), both next to this file.
# Output: linux-port/usbnet/dropbear/dropbearmulti-modern-mipsel (+ SHA256SUMS,
#         build-info.txt). PC build only: nothing is uploaded, nothing touches a device.
set -euo pipefail
VER=2026.94
SHA=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
URL=https://matt.ucc.asn.au/dropbear/releases/dropbear-$VER.tar.bz2
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HOME/r36s/downloads/dropbear-$VER.tar.bz2}
B=${BUILD_DIR:-$HOME/r36s/build/dropbear-modern}       # all building happens here
CROSS=mipsel-linux-gnu-
# Same CPU as our other console programs: MIPS32 release 2, tuned for the 74Kc.
ARCH="-march=mips32r2 -mtune=74kc"
PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp"
OUT=$HERE/dropbearmulti-modern-mipsel

# the release tarball: downloaded once (only the default path), then always sha256-checked
if test -z "${1:-}" && ! test -e "$SRC"; then
    echo "== 0. download $URL"
    mkdir -p "$(dirname "$SRC")"
    curl -fsSL --proto '=https' --tlsv1.2 -o "$SRC.part" "$URL"
    echo "$SHA  $SRC.part" | sha256sum -c - || { rm -f "$SRC.part"; echo "download has the wrong sha256"; exit 1; }
    mv -f "$SRC.part" "$SRC"
fi

echo "== 1. fresh source copy in $B/src"
rm -rf "$B" && mkdir -p "$B/src"
if test -d "$SRC"; then
    grep -q "DROPBEAR_VERSION \"$VER\"" "$SRC/src/sysoptions.h" || { echo "not Dropbear $VER: $SRC"; exit 1; }
    # copy without any version-control folder
    tar -C "$SRC" --exclude=.git -cf - . | tar -C "$B/src" -xf -
else
    echo "$SHA  $SRC" | sha256sum -c -
    tar xjf "$SRC" -C "$B/src" --strip-components=1
fi
cp "$HERE/localoptions.h" "$B/src/localoptions.h"    # keys only, no passwords

echo "== 2. static MIPS build with ${CROSS}gcc"
cd "$B/src"
# nss_fallback.c: the static program finds users in /etc/passwd without glibc's
# NSS plug-ins (the modern RAM system has no /lib at all).
${CROSS}gcc $ARCH -O2 -c "$HERE/nss_fallback.c" -o "$B/nss_fallback.o"
WRAP="-Wl,--wrap=getpwnam,--wrap=getpwuid,--wrap=initgroups $B/nss_fallback.o"
# Ubuntu's MIPS linker marks the stack executable by default (old kernels needed
# that for floating-point emulation; Linux 4.8+ does not). Ask for a normal stack.
NOEXEC="-Wl,-z,noexecstack"
CC=${CROSS}gcc AR=${CROSS}ar RANLIB=${CROSS}ranlib STRIP=${CROSS}strip \
CFLAGS="$ARCH -O2" LDFLAGS="$ARCH $NOEXEC $WRAP" \
    ./configure --host=mipsel-linux-gnu --enable-static --disable-shadow \
        --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx \
        --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline \
        --disable-loginfunc --enable-bundled-libtom > "$B/configure.log" 2>&1
make -j"$(nproc)" PROGRAMS="$PROGRAMS" MULTI=1 > "$B/make.log" 2>&1 || { tail -30 "$B/make.log"; exit 1; }
grep -i "warning: Using" "$B/make.log" | sed 's/.*Using/   note: Using/; s/ in statically.*//' | sort -u || true
${CROSS}nm dropbearmulti | grep -q __wrap_getpwnam && echo "   nss_fallback linked in: yes"
${CROSS}strip -o "$OUT" dropbearmulti
cp LICENSE "$HERE/LICENSE"

echo "== 3. checks"
# Static (no interpreter), MIPS32r2 little-endian, and code/data in SEPARATE load
# segments: code read+execute only, data read+write only, stack not executable.
${CROSS}readelf -h "$OUT" | grep -E "Class|Data|Machine|Flags"
${CROSS}readelf -lW "$OUT" > "$B/segments.txt"
cat "$B/segments.txt"
! grep -q INTERP "$B/segments.txt"
test "$(grep -c '^  LOAD' "$B/segments.txt")" = 2
grep '^  LOAD' "$B/segments.txt" | head -1 | grep -q ' R E '
grep '^  LOAD' "$B/segments.txt" | tail -1 | grep -q ' RW  '
grep '^  GNU_STACK' "$B/segments.txt" | grep -q ' RW  '
size=$(stat -c %s "$OUT")
test "$size" -lt 2097152 || { echo "too big: $size bytes"; exit 1; }
if command -v qemu-mipsel > /dev/null; then
    qemu-mipsel "$OUT" dropbear -V 2>&1 | head -1
    qemu-mipsel "$OUT" dropbearkey -t ed25519 -f "$B/test_host_key" > /dev/null 2>&1
    qemu-mipsel "$OUT" dropbearkey -y -f "$B/test_host_key" | grep -q '^ssh-ed25519 ' && echo "   ed25519 host key made + read back under qemu-mipsel: yes"
fi

echo "== 4. record"
(cd "$HERE" && sha256sum dropbearmulti-modern-mipsel > SHA256SUMS)
{
    echo "dropbear $VER, static, stripped, $size bytes"
    echo "compiler: $(${CROSS}gcc --version | head -1)"
    echo "flags: $ARCH -O2 -Wl,-z,noexecstack, configure --enable-static (hardening on: stack protector, FORTIFY)"
    echo "options: linux-port/usbnet/dropbear/localoptions.h (keys only, no passwords); plain Dropbear"
    echo "sha256: $(cut -d' ' -f1 "$HERE/SHA256SUMS")"
} > "$HERE/build-info.txt"
cat "$HERE/build-info.txt"
echo 'DROPBEAR_MODERN_PC_BUILD_COMPLETE: not yet run on the console.'

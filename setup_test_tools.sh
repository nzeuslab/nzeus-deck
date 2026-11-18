#!/bin/bash
# Builds a PC (x86-64) copy of Dropbear for the Deck's PC tests.
# The Claude-connection tests make the console's SSH key with Dropbear;
# on the PC this build stands in for the console's own (mipsel) Dropbear.
# Output: apps/dropbear/x86/dropbearmulti (not in git).
# Same Dropbear release and sha256 as linux-port/usbnet/dropbear/build-dropbear-modern.sh.
set -euo pipefail
VER=2026.94
SHA=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
URL=https://matt.ucc.asn.au/dropbear/releases/dropbear-$VER.tar.bz2
HERE=$(cd "$(dirname "$0")" && pwd)
TAR=$HOME/r36s/downloads/dropbear-$VER.tar.bz2
B=$HOME/r36s/build/dropbear-x86
OUT=$HERE/apps/dropbear/x86/dropbearmulti

# 1) the release tarball: downloaded once, then always checked against its sha256
mkdir -p "$(dirname "$TAR")"
if [ ! -f "$TAR" ]; then
    curl -fsSL --proto '=https' --tlsv1.2 -o "$TAR.part" "$URL"
    echo "$SHA  $TAR.part" | sha256sum -c - || { rm -f "$TAR.part"; echo "download has the wrong sha256"; exit 1; }
    mv "$TAR.part" "$TAR"
fi
echo "$SHA  $TAR" | sha256sum -c -

# 2) build it for this PC (one multi-call program, like on the console)
rm -rf "$B" && mkdir -p "$B"
tar -xjf "$TAR" -C "$B" --strip-components=1
cd "$B"
./configure --enable-bundled-libtom --disable-zlib > configure.log 2>&1
make -j"$(nproc)" PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp" MULTI=1 > make.log 2>&1 \
    || { tail -30 make.log; exit 1; }

mkdir -p "$(dirname "$OUT")"
cp dropbearmulti "$OUT"
echo "built $OUT ($(stat -c %s "$OUT") bytes)"

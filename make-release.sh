#!/bin/bash
# make-release.sh [WORK_DIR] - builds the NZEUS Deck release zip people download.
# Runs in WSL on the maintainer's PC. Builds files only: never touches a card,
# the console, GitHub or anything online except the pinned source downloads.
#
#   NZ_BOOTLOADER=.../factory-bootloader.bin bash make-release.sh
#
# Needs (env):
#   NZ_BOOTLOADER          your console's factory bootloader (from your own NOR backup):
#                          the factory wrapper key is computed from it, never stored
#   NZ_PRIVATE_WORDS_DIR   your private word lists (the image privacy scan) - optional but
#                          the release check refuses to call it clean without them
#   NZ_STOCK_CUBEGM        folder with a stock card's cubegm/ files (proves none is shipped)
#   NZ_ROOTFS_WORK         the Debian work folder (default /home/nzeus/nzeus-rootfs-public)
#   NZ_KERNEL_TREE         linux-hichip clone holding the pinned commit (see kernel-public/build.sh)
# Result: dist/NZEUS-Deck-<version>.zip + dist/SHA256SUMS (not in git).
set -euo pipefail
R=$(cd "$(dirname "$0")" && pwd)
: "${NZ_BOOTLOADER:?set NZ_BOOTLOADER to your factory bootloader file}"
WORK=${1:-/home/nzeus/nzeus-release}
ROOTFS=${NZ_ROOTFS_WORK:-/home/nzeus/nzeus-rootfs-public}
git -C "$R" diff --quiet HEAD || { echo "STOP: the repo has uncommitted changes - a release must match a commit"; exit 1; }
VER=$(git -C "$R" describe --always --tags)
NAME=NZEUS-Deck-$VER
mkdir -p "$WORK" "$R/dist"
echo "== NZEUS Deck release $VER ($(date +%T))"

echo "1/5 kernel"
bash "$R/ports/r36s-hichip/kernel-public/build.sh" "$WORK/kernel" | tail -2
echo "2/5 kernel + AVP stand-in packages"
python3 "$R/ports/r36s-hichip/kernel-public/package.py" --elf "$WORK/kernel/nzeus-kernel.elf" \
    --bootloader "$NZ_BOOTLOADER" --out "$WORK/package" > "$WORK/package.log"
echo "3/5 NZEUS Deck program"
(cd "$R" && bash build_nzeus_modern.sh > "$WORK/deck.log" 2>&1) || { tail -20 "$WORK/deck.log"; exit 1; }
echo "4/5 Debian images (public, no owner key)"
REUSE=$(test -d "$ROOTFS/rootfs" && echo --reuse || true)
NZ_NO_AUTHORIZED_KEYS=1 NZ_MODERN_BIN="$R/apps/frontend-modern/mips/nzeus-modern" \
    bash "$R/ports/r36s-hichip/rootfs/debian/build-debian.sh" "$ROOTFS" $REUSE --public-fatroot > "$WORK/debian.log" 2>&1 \
    || { tail -20 "$WORK/debian.log"; exit 1; }
: "${NZ_PRIVATE_WORDS_DIR:?set NZ_PRIVATE_WORDS_DIR: a release is never made without the privacy scan}"
if true; then
    sudo env NZ_PRIVATE_WORDS_DIR="$NZ_PRIVATE_WORDS_DIR" bash "$R/ports/r36s-hichip/rootfs/debian/test-public-image.sh" "$ROOTFS" | tail -1
fi

echo "5/5 zip ($(date +%T))"
python3 - "$R" "$WORK" "$ROOTFS" "$NAME" "$VER" <<'PY'
import hashlib, sys, zipfile
from pathlib import Path
repo, work, rootfs, name, ver = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], sys.argv[5]
files = {   # in the zip            <- from
    # what people click sits on top; everything else is in more/
    'READ ME FIRST.txt': repo / 'release-files/README.txt',
    'install.bat': repo / 'install.bat', 'install-new-card.bat': repo / 'install-new-card.bat',
    'uninstall.bat': repo / 'uninstall.bat', 'collect-logs.bat': repo / 'collect-logs.bat',
    'more/installer/NzeusCard.psm1': repo / 'installer/NzeusCard.psm1',
    'more/installer/install.ps1': repo / 'installer/install.ps1', 'more/installer/uninstall.ps1': repo / 'installer/uninstall.ps1',
    'more/installer/collect-logs.ps1': repo / 'installer/collect-logs.ps1', 'more/installer/new-card.ps1': repo / 'installer/new-card.ps1',
    'more/r36s_card.py': repo / 'tools/r36s_card.py', 'more/INSTALL.md': repo / 'INSTALL.md',
    'more/SOURCE.txt': repo / 'release-files/SOURCE.txt', 'more/LICENSE': repo / 'LICENSE',
    'card/cubegm/advapi32.dll': work / 'package/advapi32.dll',
    'card/cubegm/ApplicationFrame.dll': work / 'package/ApplicationFrame.dll',
    'card/cubegm/xgame-logo.bmp': repo / 'apps/bootlogo/xgame-logo.bmp',    # the NZEUS power-on picture
    'card/nzeus/nzroot.img': rootfs / 'out/nzroot.img', 'card/nzeus/nzdata.img': rootfs / 'out/nzdata.img',
}
def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()
(work / 'VERSION').write_text(ver + '\n')
files['card/nzeus/VERSION'] = work / 'VERSION'
sums = '\n'.join(f'{sha(src)}  {arc}' for arc, src in sorted(files.items())) + '\n'
out = repo / 'dist' / f'{name}.zip'
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for arc, src in sorted(files.items()):
        z.write(src, f'{name}/{arc}')
    z.writestr(f'{name}/more/SHA256SUMS', sums)
(repo / 'dist' / 'SHA256SUMS').write_text(f'{sha(out)}  {out.name}\n')
print(f'{out} ({out.stat().st_size / 2**20:.0f} MB)')
PY
echo "next: python3 tools/check-release.py dist/$NAME.zip   (with NZ_BOOTLOADER, NZ_STOCK_CUBEGM, NZ_PRIVATE_WORDS_DIR)"

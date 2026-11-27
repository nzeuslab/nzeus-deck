# Makes the NZEUS emoji pack: one small picture per emoji, for NZEUS Deck's
# WhatsApp app (and every label that uses artext.c). On the console the Deck
# reads them from NZ_DIR/emoji (/data/nzeus/deck/nzeus/emoji).
#
# Where the pictures come from: Twemoji (the emoji set Twitter/X made), from
# its maintained official repo github.com/jdecked/twemoji. The graphics are
# CC-BY 4.0, so an attribution file goes next to them (LICENSE.txt).
# The release archive is downloaded ONCE into tools/emoji_cache/ and checked
# against a fixed sha256 (a changed or broken download is refused).
#
# Which emoji: only those that are ONE Unicode code point (file names without
# "-"). The PC bridge turns every emoji into one code point before it sends.
#
# The output (default: core/deck-modern/build/emoji/, not in git):
#   1f600.bin ...  LVGL 8 binary images (what lv_img loads from a file):
#                  4-byte header + LV_IMG_CF_TRUE_COLOR_ALPHA pixels, i.e. for
#                  16-bit colour (lv_conf.h: LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0)
#                  3 bytes per pixel: RGB565 low byte, high byte, then alpha
#   index.txt      the code points there are pictures for, sorted, one hex per line
#   LICENSE.txt    the attribution (CC-BY 4.0)
#
# How the pack gets to the console:
#   - the system image: ports/r36s-hichip/rootfs/debian/apply-overlay.sh copies it to
#     /usr/local/lib/nzeus/emoji, and nzeus-deck links NZ_DIR/emoji there
#   - an existing console: python tools/publish_update.py --emoji (signed
#     update; the console puts the pictures in NZ_DIR/emoji)
#
# Usage (Windows Python with Pillow):
#   python tools/make_emoji.py                     the whole pack (default folder above)
#   python tools/make_emoji.py --out DIR           somewhere else
#   python tools/make_emoji.py --out DIR --only 1f600,1f602   just a few (the tests)
import argparse
import hashlib
import io
import os
import struct
import sys
import tarfile
import urllib.request

from PIL import Image

TAG = "v17.0.3"                                        # the pinned Twemoji release
URL = "https://github.com/jdecked/twemoji/archive/refs/tags/%s.tar.gz" % TAG
SHA256 = "a0855654b633045ae2337537e77f1bb4361162f7fcd910e613eaab1d6d9c5fca"
SIZE = 16                                              # pixels: matches the 16 px text font

HERE = os.path.dirname(os.path.abspath(__file__))      # R36S/tools
R36S = os.path.dirname(HERE)
CACHE = os.path.join(HERE, "emoji_cache")
ARCHIVE = os.path.join(CACHE, "twemoji-%s.tar.gz" % TAG.lstrip("v"))
DEFAULT_OUT = os.path.join(R36S, "core", "deck-modern", "build", "emoji")

LV_IMG_CF_TRUE_COLOR_ALPHA = 5


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def get_archive():
    """The release archive from the cache, downloaded the first time."""
    os.makedirs(CACHE, exist_ok=True)
    if not os.path.exists(ARCHIVE):
        print("downloading", URL)
        tmp = ARCHIVE + ".part"
        urllib.request.urlretrieve(URL, tmp)
        os.replace(tmp, ARCHIVE)
    got = sha256_of(ARCHIVE)
    if got != SHA256:
        sys.exit("make_emoji: %s has the wrong sha256 (%s) - delete it and try again" % (ARCHIVE, got))
    return ARCHIVE


def lvgl_bin(png_bytes):
    """A 72x72 Twemoji PNG -> an LVGL 8 .bin image of SIZE x SIZE pixels."""
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    img = img.resize((SIZE, SIZE), Image.LANCZOS)      # (Pillow handles the see-through edges right)
    # the 4-byte header: colour format (5 bits), 5 unused bits, width (11 bits), height (11 bits)
    out = bytearray(struct.pack("<I", LV_IMG_CF_TRUE_COLOR_ALPHA | (SIZE << 10) | (SIZE << 21)))
    px = img.tobytes()                                  # R, G, B, A for each pixel, row by row
    for i in range(0, len(px), 4):
        r, g, b, a = px[i], px[i + 1], px[i + 2], px[i + 3]
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)   # RGB565
        if a == 0:
            c = 0                                       # (fully see-through: colour unused)
        out += struct.pack("<HB", c, a)                 # low byte, high byte, alpha
    return bytes(out)


LICENSE = """NZEUS emoji pictures
====================

These pictures are Twemoji, made by Twitter, Inc. and other contributors,
from the maintained Twemoji project: https://github.com/jdecked/twemoji
(release %s). Copyright 2019 Twitter, Inc and other contributors.

The graphics are licensed under CC-BY 4.0:
https://creativecommons.org/licenses/by/4.0/

Changes made for NZEUS: each picture was shrunk to %d x %d pixels and
converted to LVGL's binary image format (.bin); only emoji that are one
Unicode code point are included.
"""


def main():
    ap = argparse.ArgumentParser(description="Make the NZEUS emoji pack (Twemoji -> LVGL .bin)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output folder")
    ap.add_argument("--only", default="", help="comma-separated hex code points (a small pack for tests)")
    args = ap.parse_args()
    only = {h.strip().lower() for h in args.only.split(",") if h.strip()}

    os.makedirs(args.out, exist_ok=True)
    done = []
    total = 0
    with tarfile.open(get_archive(), "r:gz") as tar:
        for m in tar:
            parts = m.name.split("/")
            if len(parts) != 4 or parts[1:3] != ["assets", "72x72"] or not parts[3].endswith(".png"):
                continue
            hexname = parts[3][:-4].lower()
            if "-" in hexname:                          # a sequence (several code points): skipped
                continue
            try:
                cp = int(hexname, 16)
            except ValueError:
                continue
            if only and hexname not in only:
                continue
            data = lvgl_bin(tar.extractfile(m).read())
            name = "%x.bin" % cp                        # e.g. 1f600.bin
            with open(os.path.join(args.out, name), "wb") as f:
                f.write(data)
            total += len(data)
            done.append(cp)
    done.sort()
    with open(os.path.join(args.out, "index.txt"), "w", newline="\n") as f:
        for cp in done:
            f.write("%x\n" % cp)
    with open(os.path.join(args.out, "LICENSE.txt"), "w", newline="\n") as f:
        f.write(LICENSE % (TAG, SIZE, SIZE))
    print("emoji: %d pictures, %d x %d px, %.1f KB of pictures in %s"
          % (len(done), SIZE, SIZE, total / 1024.0, args.out))


if __name__ == "__main__":
    main()

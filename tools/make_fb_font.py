#!/usr/bin/env python3
"""Makes ports/r36s-hichip/fatroot/font8x16.h: the 8x16 letters `nz-fatroot show` draws.

Source: Lat15-Fixed16.psf.gz from Debian/Ubuntu's console-setup-linux package
("All console fonts are public domain by nature", its copyright file).
Only the printable ASCII letters (32..126) are kept: 95 x 16 bytes.

  python3 tools/make_fb_font.py /usr/share/consolefonts/Lat15-Fixed16.psf.gz
"""
import gzip
import struct
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / 'ports' / 'r36s-hichip' / 'fatroot' / 'font8x16.h'


def glyphs(data):
    """Returns (height, list of glyph bitmaps) from a PSF1 or PSF2 font."""
    if data[:2] == b'\x36\x04':                       # PSF1
        height = data[3]
        return height, [data[4 + i * height:4 + (i + 1) * height] for i in range(256)]
    magic, _, hdr, _, count, size, height, width = struct.unpack_from('<8I', data)
    assert magic == 0x864AB572 and width == 8, 'need an 8-pixel-wide PSF font'
    return height, [data[hdr + i * size:hdr + (i + 1) * size] for i in range(count)]


def main(path):
    height, g = glyphs(gzip.open(path).read())
    assert height == 16, f'need a 16-pixel-high font, got {height}'
    lines = ['/* 8x16 letters 32..126 for nz-fatroot show. Made by tools/make_fb_font.py from',
             ' * Lat15-Fixed16.psf.gz (console-setup: "All console fonts are public domain"). */',
             'static const unsigned char font8x16[95][16] = {']
    for c in range(32, 127):
        lines.append('\t{' + ', '.join('0x%02x' % b for b in g[c]) + '},  /* %r */' % chr(c))
    lines.append('};')
    OUT.write_text('\n'.join(lines) + '\n')
    print(f'wrote {OUT}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '/usr/share/consolefonts/Lat15-Fixed16.psf.gz')

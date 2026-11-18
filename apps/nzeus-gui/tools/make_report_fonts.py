# Makes the fonts for the car health report PDF (font_report.c).
#
# The report is drawn by LVGL at 150 dots per inch, so it needs its own,
# bigger fonts than the screen: IBM Plex Sans (English), IBM Plex Sans Arabic
# (Arabic) and IBM Plex Mono (codes, VIN, numbers), at the sizes the report
# uses. Each glyph (letter picture) is stored with 16 grey levels (4 bits).
#
# Arabic: the console has no "shaping engine" (the program that picks the
# right joined shape of each letter). carreport_text.c does that itself by
# swapping each letter for its "presentation form" (the ready-made start /
# middle / end / alone shape, Unicode FE80-FEFC). So the Arabic part of these
# fonts holds exactly those shapes, plus Arabic punctuation, digits and the
# small vowel marks. Every Arabic letter of normal Arabic text is covered,
# not only the letters of the sample report.
#
# Usage (Windows Python with freetype-py, from PyPI):
#   python make_report_fonts.py  <folder with the 3 unzipped IBM Plex releases>  <output .c>
# Font sources (SIL Open Font License 1.1, official IBM GitHub releases):
#   @ibm/plex-sans@1.1.0         ibm-plex-sans.zip         (IBM Plex Sans 3.005)
#   @ibm/plex-sans-arabic@1.1.0  ibm-plex-sans-arabic.zip  (IBM Plex Sans Arabic 1.005)
#   @ibm/plex-mono@1.1.0         ibm-plex-mono.zip         (IBM Plex Mono 2.004)
import math
import os
import sys

import freetype

SRC, OUT = sys.argv[1], sys.argv[2]
SANS = SRC + '/ibm-plex-sans/ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-%s.ttf'
ARAB = SRC + '/ibm-plex-sans-arabic/ibm-plex-sans-arabic/fonts/complete/ttf/IBMPlexSansArabic-%s.ttf'
MONO = SRC + '/ibm-plex-mono/ibm-plex-mono/fonts/complete/ttf/IBMPlexMono-%s.ttf'

PX = 150 / 72          # 1 point = 2.083 pixels at 150 dpi

# Latin: plain ASCII plus the few extra signs the report may use
LATIN = list(range(0x20, 0x7F)) + [0xA0, 0xB0, 0xB1, 0xB5, 0xB7, 0xD7,
                                   0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D,
                                   0x2022, 0x2026, 0x2212]
# Arabic: punctuation, tatweel, vowel marks, Arabic-Indic digits, and all
# letter shapes of "Arabic Presentation Forms-B" (incl. the lam-alef joins)
ARABIC = [0x060C, 0x061B, 0x061F, 0x0640] + list(range(0x064B, 0x0653)) + \
         list(range(0x0660, 0x066D)) + [0x0670] + list(range(0xFE80, 0xFEFD))
# "glue" copies of . , : for numbers like 764.0, 1,250 and 14:18: LVGL may
# break a line after a real . , or : - these private codes look the same but
# never break (carreport.c swaps them in between two digits)
ALIASES = {0xE000: 0x2E, 0xE001: 0x2C, 0xE002: 0x3A}
MARKS = set(range(0x064B, 0x0653)) | {0x0670}     # drawn over the letter before them

# name, point size, weight, Arabic too?, fallback font (used for any missing letter)
FONTS = [
    ('rp_font_reg19',    9.0,   'Regular',  True,  None),             # small labels, footer
    ('rp_font_med19',    9.0,   'Medium',   True,  None),             # table headers, pills
    ('rp_font_semi20',   9.75,  'SemiBold', True,  None),             # table group rows
    ('rp_font_reg22',    10.5,  'Regular',  True,  None),             # table cells, module names
    ('rp_font_reg23',    11.25, 'Regular',  True,  None),             # normal text
    ('rp_font_med25',    12.0,  'Medium',   True,  None),             # info grid values
    ('rp_font_semi27',   12.75, 'SemiBold', True,  None),             # summary headline
    ('rp_font_bold31',   15.0,  'Bold',     True,  None),             # section titles
    ('rp_font_bold44',   21.0,  'Bold',     True,  None),             # report title
    ('rp_font_mono19',   9.0,   'Regular',  False, 'rp_font_reg19'),  # top caps line
    ('rp_font_mono22',   10.5,  'Regular',  False, 'rp_font_reg22'),  # fault codes, module codes
    ('rp_font_monomed25', 12.0, 'Medium',   False, 'rp_font_med25'),  # VIN, odometer, battery
]


def render(face, cp, px, floor_adv):
    """one glyph -> (advance in 1/16 px, box w, box h, ofs x, ofs y, 4-bit pixels)"""
    face.set_char_size(int(round(px * 64)))
    # light hinting: sharpens rows (vertical only), keeps letter widths exact
    face.load_char(chr(ALIASES.get(cp, cp)), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
    g = face.glyph
    adv = g.linearHoriAdvance / 65536.0                  # exact width, unhinted
    # joined Arabic shapes: round the width DOWN so that neighbours overlap a
    # little instead of leaving a hairline gap in the joining stroke
    adv16 = int(math.floor(adv)) * 16 if floor_adv else int(round(adv * 16))
    if cp in MARKS:
        adv16 = 0                                        # vowel marks take no room
    bm = g.bitmap
    w, h = bm.width, bm.rows
    pixels = []
    for y in range(h):
        row = bm.buffer[y * bm.pitch: y * bm.pitch + w]
        pixels += [(v * 15 + 127) // 255 for v in row]   # 256 greys -> 16 greys
    # trim empty columns/rows so no space is wasted
    if w and h:
        while h and not any(pixels[(h - 1) * w:h * w]):
            pixels = pixels[:(h - 1) * w]; h -= 1
        top_cut = 0
        while h and not any(pixels[:w]):
            pixels = pixels[w:]; h -= 1; top_cut += 1
        left = 0
        while w and not any(pixels[y * w] for y in range(h)):
            pixels = [p for i, p in enumerate(pixels) if i % w]; w -= 1; left += 1
        while w and not any(pixels[y * w + w - 1] for y in range(h)):
            pixels = [p for i, p in enumerate(pixels) if i % w != w - 1]; w -= 1
    else:
        top_cut = left = 0
    if not w or not h:
        return adv16, 0, 0, 0, 0, []
    ofs_x = g.bitmap_left + left
    ofs_y = (g.bitmap_top - top_cut) - h                 # bottom of the box, up from the baseline
    return adv16, w, h, ofs_x, ofs_y, pixels


def pack4(pixels):
    """2 pixels per byte, first pixel in the high half (LVGL's 4-bit format)"""
    out = []
    for i in range(0, len(pixels), 2):
        a = pixels[i]
        b = pixels[i + 1] if i + 1 < len(pixels) else 0
        out.append((a << 4) | b)
    return out


def make_font(name, pt, weight, arabic, fallback):
    px = pt * PX
    latin_face = freetype.Face((MONO if fallback else SANS) % weight)
    arab_face = freetype.Face(ARAB % weight) if arabic else None
    chars = []
    for cp in LATIN + sorted(ALIASES):
        if latin_face.get_char_index(ALIASES.get(cp, cp)):
            chars.append((cp, latin_face, False))
    if arab_face:
        for cp in ARABIC:
            if arab_face.get_char_index(cp):
                # letters that join are 0xFE8B and up (except a few alone/end-only
                # shapes); rounding every Arabic shape down is harmless
                chars.append((cp, arab_face, cp >= 0xFE80))
    bitmap = []
    dsc = ['{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id 0: none */']
    codes = []
    for cp, face, floor_adv in chars:
        adv16, w, h, ox, oy, pixels = render(face, cp, px, floor_adv)
        assert w < 256 and h < 256 and -128 <= ox < 128 and -128 <= oy < 128, (name, hex(cp))
        dsc.append('{.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}'
                   % (len(bitmap), adv16, w, h, ox, oy))
        bitmap += pack4(pixels)
        codes.append(cp)
    # character -> glyph number tables: one "range" per run of consecutive codes
    ranges = []
    gid = 1
    i = 0
    while i < len(codes):
        j = i
        while j + 1 < len(codes) and codes[j + 1] == codes[j] + 1:
            j += 1
        ranges.append((codes[i], j - i + 1, gid))
        gid += j - i + 1
        i = j + 1
    line_h = int(round(1.3 * px))        # IBM Plex: 1.025 above + 0.275 below the baseline
    base = int(round(0.275 * px))
    src = []
    src.append('/* %s: IBM Plex %s%s %s, %.2f pt at 150 dpi (%.2f px), %d glyphs, 4 bits per pixel */'
               % (name, 'Mono' if fallback else 'Sans', ' + Sans Arabic' if arabic else '', weight, pt, px, len(codes)))
    src.append('static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_bitmap[] = {' % name)
    for k in range(0, len(bitmap), 24):
        src.append('    ' + ','.join('0x%02x' % b for b in bitmap[k:k + 24]) + ',')
    src.append('};')
    src.append('static const lv_font_fmt_txt_glyph_dsc_t %s_dsc[] = {' % name)
    src.append(',\n'.join('    ' + d for d in dsc))
    src.append('};')
    src.append('static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {' % name)
    for start, length, g0 in ranges:
        src.append('    {.range_start = 0x%04X, .range_length = %d, .glyph_id_start = %d, .unicode_list = NULL, '
                   '.glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},'
                   % (start, length, g0))
    src.append('};')
    src.append('static lv_font_fmt_txt_glyph_cache_t %s_cache;' % name)
    src.append('static const lv_font_fmt_txt_dsc_t %s_fdsc = {' % name)
    src.append('    .glyph_bitmap = %s_bitmap, .glyph_dsc = %s_dsc, .cmaps = %s_cmaps,' % (name, name, name))
    src.append('    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = %d, .bpp = 4, .kern_classes = 0,' % len(ranges))
    src.append('    .bitmap_format = 0, .cache = &%s_cache' % name)
    src.append('};')
    src.append('const lv_font_t %s = {' % name)
    src.append('    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt, .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,')
    src.append('    .line_height = %d, .base_line = %d, .subpx = LV_FONT_SUBPX_NONE,' % (line_h, base))
    src.append('    .underline_position = 0, .underline_thickness = 0, .dsc = &%s_fdsc,' % name)
    src.append('    .fallback = %s' % ('&' + fallback if fallback else 'NULL'))
    src.append('};')
    print('%-18s %3d glyphs, %6d bitmap bytes, %d ranges' % (name, len(codes), len(bitmap), len(ranges)))
    return '\n'.join(src), len(bitmap) + 8 * len(dsc)


parts = ['/* font_report.c - the fonts of the car health report PDF (carreport.c).',
         ' * Made by tools/make_report_fonts.py from IBM Plex Sans, IBM Plex Sans Arabic and',
         ' * IBM Plex Mono (SIL Open Font License 1.1, Copyright IBM Corp.). Do not edit by hand. */',
         '#include "lvgl.h"', '',
         '/* the mono fonts borrow letters they lack (Arabic) from these */']
for f in FONTS:
    parts.append('extern const lv_font_t %s;' % f[0])
total = 0
for f in FONTS:
    text, size = make_font(*f)
    parts.append('')
    parts.append(text)
    total += size
with open(OUT, 'w', newline='\n') as fp:
    fp.write('\n'.join(parts) + '\n')
print('total about %d KB of font data -> %s' % (total // 1024, OUT))

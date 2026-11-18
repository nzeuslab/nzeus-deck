# Makes NZEUS's terminal font (font_mono16.c): 8 x 16 pixels per character.
#
# LVGL's built-in "unscii 16" is 16 pixels WIDE per character, so only 40
# characters fit on the 640-pixel screen. A terminal needs about 80. So we take
# LVGL's 8 x 8 unscii font (public domain glyphs, shipped inside LVGL) and
# double every row: same 8-pixel width, twice the height -> 80 columns of
# readable, chunky "retro terminal" text.
#
# The same trick also makes bigger "pixel" fonts for the CRT themes: every
# pixel becomes an SX x SY block (3 x 3 -> 24 x 24 characters).
#
# Usage: python3 make_mono_font.py  path/to/lv_font_unscii_8.c  output.c  [SX SY NAME LINE_H FALLBACK]
#   default:  1 2 lv_font_nzeus_mono_16 18 lv_font_montserrat_16
# FALLBACK: the font LVGL uses for symbols this font doesn't have (Wi-Fi, battery...)
import re
import sys

src_path, out_path = sys.argv[1], sys.argv[2]
SX, SY = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (1, 2)
NAME = sys.argv[5] if len(sys.argv) > 5 else "lv_font_nzeus_mono_16"
LINE_H = int(sys.argv[6]) if len(sys.argv) > 6 else 18
FALLBACK = sys.argv[7] if len(sys.argv) > 7 else "lv_font_montserrat_16"
src = open(src_path).read()

# 1) the packed glyph pictures (1 bit per pixel, rows run together)
bm_block = re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", src, re.S).group(1)
bm_block = re.sub(r"/\*.*?\*/", "", bm_block, flags=re.S)           # drop the comments
bitmap = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", bm_block)]

# 2) per-glyph info
dsc = []
for m in re.finditer(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
                     r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src):
    dsc.append(tuple(int(v) for v in m.groups()))

def read_bits(start_byte, count):
    """count bits starting at byte start_byte, most significant bit first"""
    out = []
    for i in range(count):
        byte = bitmap[start_byte + i // 8]
        out.append((byte >> (7 - i % 8)) & 1)
    return out

new_bitmap, new_dsc = [], []
for (index, adv_w, box_w, box_h, ofs_x, ofs_y) in dsc:
    if box_w == 0 or box_h == 0:
        new_dsc.append((len(new_bitmap), adv_w * SX, box_w, box_h, ofs_x, ofs_y))
        continue
    bits = read_bits(index, box_w * box_h)
    rows = [bits[r * box_w:(r + 1) * box_w] for r in range(box_h)]
    wide = [[px for px in row for _ in range(SX)] for row in rows]   # every pixel SX times
    doubled = [px for row in wide for _ in range(SY) for px in row]  # every row SY times
    packed = []
    for i in range(0, len(doubled), 8):
        chunk = doubled[i:i + 8] + [0] * (8 - len(doubled[i:i + 8]))
        packed.append(int("".join(map(str, chunk)), 2))
    new_dsc.append((len(new_bitmap), adv_w * SX, box_w * SX, box_h * SY, ofs_x * SX, ofs_y * SY))
    new_bitmap += packed

bitmap_c = ",\n    ".join(", ".join(f"0x{b:02x}" for b in new_bitmap[i:i + 16])
                          for i in range(0, len(new_bitmap), 16))
dsc_c = ",\n    ".join(f"{{.bitmap_index = {d[0]}, .adv_w = {d[1]}, .box_w = {d[2]}, .box_h = {d[3]}, "
                       f".ofs_x = {d[4]}, .ofs_y = {d[5]}}}" for d in new_dsc)

out = f"""/* {NAME} - NZEUS pixel font, {8 * SX} x {8 * SY} pixels per character.
 * Made by tools/make_mono_font.py from LVGL's built-in unscii 8 x 8 font
 * (public domain glyphs), each pixel made {SX} x {SY}. Do not edit by hand. */
#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
    {bitmap_c}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
    {dsc_c}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{
        .range_start = 32, .range_length = 96, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }}
}};

static lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
    .cache = &cache
}};

const lv_font_t {NAME} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {LINE_H},
    .base_line = 0,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0,
    .underline_thickness = 0,
    .dsc = &font_dsc,
    .fallback = &{FALLBACK}
}};
"""
open(out_path, "w").write(out)
print(f"{len(dsc)} glyphs, {len(new_bitmap)} bitmap bytes -> {out_path}")

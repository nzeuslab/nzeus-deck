#!/usr/bin/env python3
"""Turns art/retro-icons.png (NZEUS's own 24 app icons for the Retro Blue theme:
6 x 4 cells, transparent background) into retroicons_data.c: each icon at
32 x 32 (desktop) and 20 x 20 (menus) as RGB565 pixels, see-through = pure
green 0x07E0 (LVGL's chroma key), made smaller smoothly here on the PC.

Run after changing the picture:  python3 apps/nzeus-gui/art/make_icon_data.py
(needs Pillow). The PC tests check that retroicons_data.c matches retro-icons.png."""
import hashlib
import tempfile
from pathlib import Path
from PIL import Image

here = Path(__file__).resolve().parent
SHEET = here / "retro-icons.png"
# the sheet's cells, row by row = the icon file names (as nzeus/icons/NAME.bmp)
NAMES = ["files", "terminal", "web", "car", "settings", "media",
         "editor", "system", "python", "electronics", "arduino", "claude",
         "wifi", "taskmanager", "calculator", "notes", "clock", "images",
         "whatsapp", "probe", "devtools", "health", "bluetooth", "update"]
KEY = 0x07E0
EDGE = (150, 170, 200)      # half-see-through edge pixels are mixed with this (looks right on light and blue)

def rgb565(r, g, b):
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return 0x07C0 if v == KEY else v        # (never the see-through colour by accident)

def icon(cell: Image.Image, size: int):
    box = cell.getchannel("A").point(lambda a: 255 if a > 24 else 0).getbbox() or (0, 0, cell.width, cell.height)
    part = cell.crop(box)
    side = max(part.width, part.height)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(part, ((side - part.width) // 2, (side - part.height) // 2))
    # premultiplied resize: no dark halo from the invisible pixels' colours
    small = square.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")
    out = []
    for r, g, b, a in list(small.convert("RGBA").get_flattened_data() if hasattr(small, "get_flattened_data") else small.getdata()):
        if a < 100:
            out.append(KEY)
        else:
            k = a / 255
            out.append(rgb565(int(r * k + EDGE[0] * (1 - k)), int(g * k + EDGE[1] * (1 - k)), int(b * k + EDGE[2] * (1 - k))))
    return out

sheet = Image.open(SHEET).convert("RGBA")
cw, ch = sheet.width // 6, sheet.height // 4
big, small = [], []
for i, name in enumerate(NAMES):
    cell = sheet.crop(((i % 6) * cw, (i // 6) * ch, (i % 6 + 1) * cw, (i // 6 + 1) * ch))
    big.append(icon(cell, 32))
    small.append(icon(cell, 20))

def table(rows, per):
    lines = []
    for px in rows:
        lines.append("    { " + ", ".join(f"0x{v:04x}" for v in px) + " }")
    return ",\n".join(lines)

sha = hashlib.sha256(SHEET.read_bytes()).hexdigest()
out = here.parent / "retroicons_data.c"
out.write_text(
    "/* retroicons_data.c - the Retro Blue theme's built-in app icons (art/retro-icons.png).\n"
    " * Made by art/make_icon_data.py: do not edit by hand.\n"
    f" * sha256 {sha} */\n"
    "#include <stdint.h>\n"
    f"const int nz_retro_icon_count = {len(NAMES)};\n"
    "const char *const nz_retro_icon_names[] = { " + ", ".join(f'"{n}"' for n in NAMES) + " };\n"
    f"const uint16_t nz_retro_icons32[][32 * 32] = {{\n{table(big, 32)}\n}};\n"
    f"const uint16_t nz_retro_icons20[][20 * 20] = {{\n{table(small, 20)}\n}};\n",
    newline="\n",
)
# a preview to look at: the icons on the wallpaper's blue and on a light menu
prev = Image.new("RGB", (len(NAMES) * 40, 76), (60, 110, 200))
for i in range(len(NAMES)):
    for px, size, y, bg in ((big[i], 32, 4, None), (small[i], 20, 44, (236, 233, 216))):
        if bg:
            prev.paste(bg, (i * 40, 40, i * 40 + 40, 76))
        for p, v in enumerate(px):
            if v == KEY:
                continue
            r, g, b = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
            prev.putpixel((i * 40 + (40 - size) // 2 + p % size, y + p // size), (r, g, b))
prev.resize((prev.width * 2, prev.height * 2), Image.NEAREST).save(Path(tempfile.gettempdir()) / "retro-icons-preview.png")
print(f"{out.name}: {len(NAMES)} icons, sheet sha256 {sha}; preview in {tempfile.gettempdir()}")

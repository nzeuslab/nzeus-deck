#!/usr/bin/env python3
"""Turns art/hill.jpg (the built-in "Hills" wallpaper, 640 x 446, baseline JPG)
into wallpaper_hill_data.c, so the picture is part of the program.

Run after changing the picture:  python3 apps/nzeus-gui/art/make_hill_data.py
The PC tests check that wallpaper_hill_data.c still matches hill.jpg."""
import hashlib
from pathlib import Path

here = Path(__file__).resolve().parent
data = (here / "hill.jpg").read_bytes()
sha = hashlib.sha256(data).hexdigest()
rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
out = here.parent / "wallpaper_hill_data.c"
out.write_text(
    "/* wallpaper_hill_data.c - the built-in \"Hills\" wallpaper picture (art/hill.jpg).\n"
    " * Made by art/make_hill_data.py: do not edit by hand.\n"
    f" * sha256 {sha} */\n"
    "const unsigned char nz_hill_jpg[] = {\n    " + ",\n    ".join(rows) + "\n};\n"
    f"const unsigned int nz_hill_jpg_len = {len(data)};\n",
    newline="\n",
)
print(f"{out.name}: {len(data)} bytes, sha256 {sha}")

# Makes the NZEUS power-on picture (cubegm/xgame-logo.bmp on the SD card).
#
# The console shows this file at power-on. To be sure it accepts ours, we copy
# the stock file's exact format:
#   - the same 54-byte BMP header, byte for byte (640x480, 32-bit, top row first)
#   - pixels stored as blue, green, red, alpha, with alpha always 255
#   - the same total size (1,228,854 bytes)
from PIL import Image, ImageDraw, ImageFont

STOCK = "stock/xgame-logo.bmp"     # the original, kept as a backup
OUT = "xgame-logo.bmp"             # ours, same name as on the card
W, H = 640, 480

# --- draw the picture (same look as the NZEUS splash) ---
img = Image.new("RGB", (W, H), (14, 16, 22))
d = ImageDraw.Draw(img)

def font(size, bold=True):
    return ImageFont.truetype("C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf", size)

def center(text, y, f, fill):
    d.text(((W - d.textlength(text, font=f)) / 2, y), text, font=f, fill=fill)

d.rounded_rectangle((32, 32, W - 32, H - 32), radius=18, outline=(60, 200, 120), width=3)
for i, c in enumerate([(230, 80, 80), (230, 190, 70), (70, 200, 110)]):
    d.ellipse((56 + i * 30, 52, 74 + i * 30, 70), fill=c)
center("NZEUS", 120, font(128), (235, 240, 245))
center("Linux", 262, font(80), (60, 200, 120))
center("starting...", 380, font(28, False), (150, 160, 170))

# --- write it in the stock file's exact format ---
header = open(STOCK, "rb").read(54)          # copy the original header unchanged
pixels = bytearray()
for r, g, b in img.getdata():                # top row first, like the stock file
    pixels += bytes((b, g, r, 255))
data = header + pixels
assert len(data) == len(open(STOCK, "rb").read()), "size differs from the stock file"
open(OUT, "wb").write(data)
img.save("xgame-logo_preview.png")
print(f"saved {OUT}: {len(data)} bytes, header identical to the stock file")

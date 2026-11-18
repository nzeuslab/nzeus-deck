# Makes NZEUS OS's icon set: icons.c + icons.h (LVGL 8.3 images) and a preview
# contact sheet (reports/gui/icons_preview.png).
#
# Every icon is a clean outline ("line") icon drawn from simple geometry:
# lines, arcs, circles, rounded rectangles and a few filled dots/polygons.
#   - Icons are designed on a 24 x 24 unit grid (like most line-icon sets).
#   - They are drawn 8x bigger than needed, then shrunk (box filter), which
#     gives smooth anti-aliased edges ("supersampling").
#   - Straight lines are "snapped" to the pixel grid so they stay crisp.
#   - The same stroke weight is used everywhere: 3 px at 48, 2 px at 32,
#     1.5 px at 20.
#
# The images are ALPHA-ONLY (LV_IMG_CF_ALPHA_8BIT): one byte per pixel saying
# how much "ink" covers it (0 = transparent, 255 = full). LVGL colours them at
# draw time with the theme colour (style img_recolor), so one icon set works on
# every theme.
#
# Usage:  python make_icons.py          (Windows Python with Pillow is enough)
import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.dirname(HERE)                               # apps/nzeus-gui
R36S_DIR = os.path.dirname(os.path.dirname(APP_DIR))          # R36S
OUT_C = os.path.join(APP_DIR, "icons.c")
OUT_H = os.path.join(APP_DIR, "icons.h")
OUT_PNG = os.path.join(R36S_DIR, "reports", "gui", "icons_preview.png")

SIZES = (48, 32, 20)
SS = 8                                   # supersampling factor
STROKE = {48: 3.0, 32: 2.0, 20: 1.5}    # line width in pixels for each size
# Where line centres go so the line edges land on pixel edges:
# 0.5 = on pixel centres (odd-ish widths), 0.0 = on pixel borders (even widths)
PHASE = {48: 0.5, 32: 0.0, 20: 0.5}


# ---------------------------------------------------------------------------
# Small geometry helpers (all in pixels)
# ---------------------------------------------------------------------------
def _round_corners(pts, radius, closed):
    """Replace sharp corners of a polyline with small curves.
    radius: one number (pixels) or a list with one radius per point."""
    n = len(pts)
    radii = list(radius) if isinstance(radius, (list, tuple)) else [radius] * n
    out = []
    for i, (px, py) in enumerate(pts):
        r = radii[i]
        if r <= 0 or (not closed and (i == 0 or i == n - 1)):
            out.append((px, py))
            continue
        ax, ay = pts[i - 1]
        bx, by = pts[(i + 1) % n]
        la, lb = math.hypot(ax - px, ay - py), math.hypot(bx - px, by - py)
        if la < 1e-9 or lb < 1e-9:
            out.append((px, py))
            continue
        ux, uy = (ax - px) / la, (ay - py) / la          # towards previous point
        vx, vy = (bx - px) / lb, (by - py) / lb          # towards next point
        cos_t = max(-1.0, min(1.0, ux * vx + uy * vy))
        theta = math.acos(cos_t)                          # angle at the corner
        if theta < 1e-3 or theta > math.pi - 1e-3:        # (almost) straight: nothing to round
            out.append((px, py))
            continue
        t = min(r / math.tan(theta / 2), la * 0.5, lb * 0.5)   # how far back the curve starts
        rr = t * math.tan(theta / 2)                       # the radius we can really use
        bis = math.hypot(ux + vx, uy + vy)
        cx = px + (ux + vx) / bis * rr / math.sin(theta / 2)  # centre of the corner circle
        cy = py + (uy + vy) / bis * rr / math.sin(theta / 2)
        p1 = (px + ux * t, py + uy * t)
        p2 = (px + vx * t, py + vy * t)
        a1 = math.atan2(p1[1] - cy, p1[0] - cx)
        a2 = math.atan2(p2[1] - cy, p2[0] - cx)
        da = (a2 - a1 + math.pi) % (2 * math.pi) - math.pi   # the short way round
        steps = max(4, int(abs(da) * rr * SS / 2))
        for k in range(steps + 1):
            a = a1 + da * k / steps
            out.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return out


def _arc_pts(cx, cy, rx, ry, a0, a1):
    """Points on an arc (angles in degrees, 0 = right, 90 = down)."""
    steps = max(8, int(abs(a1 - a0) / 360 * 2 * math.pi * max(rx, ry) * SS / 2))
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * k / steps)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * k / steps)))
            for k in range(steps + 1)]


# ---------------------------------------------------------------------------
# The pen: draws one icon at one size. Coordinates are in 24-unit grid units.
# ---------------------------------------------------------------------------
class Pen:
    def __init__(self, size, offset=(0.0, 0.0), snap=True, centre_lines=""):
        self.size = size
        self.small = size <= 20          # icons may drop details at 20 px
        self.k = size / 24.0             # pixels per grid unit
        self.w = STROKE[size]            # stroke width in pixels
        self.phase = PHASE[size]
        self.ox, self.oy = offset        # shift (units) that centres the icon
        self.do_snap = snap
        # Icon middle in pixels. An icon with a line straight through its middle
        # (clock hand, power stem...) would get that line blurred across two
        # pixels at sizes whose lines sit on pixel centres; for those icons the
        # middle moves half a pixel onto a pixel centre so every line is crisp.
        half = 0.5 if (snap and self.phase == 0.5) else 0.0
        self.cx = size / 2.0 + (half if "x" in centre_lines else 0.0)
        self.cy = size / 2.0 + (half if "y" in centre_lines else 0.0)
        self.img = Image.new("L", (size * SS, size * SS), 0)
        self.draw = ImageDraw.Draw(self.img)

    # -- units -> pixels --------------------------------------------------
    def _snap(self, v, centre, phase, bias=0.0):
        """Move pixel coordinate v to the nearest 'crisp' position (whole number
        + phase). Chosen symmetrically around the centre, so symmetric icons stay
        symmetric. bias > 0 prefers positions nearer the centre."""
        if not self.do_snap:
            return v
        d = v - centre
        if abs(d) < 1e-6:
            return v                     # a point at the exact middle stays put
        f = (phase - centre) % 1.0       # grid offset, measured from the centre
        n = math.floor(abs(d) - f + 0.5 - bias)
        return centre + math.copysign(max(n, 0) + f, d)

    def sx(self, v, phase=None, bias=0.0):
        return self._snap(v, self.cx, self.phase if phase is None else phase, bias)

    def sy(self, v, phase=None, bias=0.0):
        return self._snap(v, self.cy, self.phase if phase is None else phase, bias)

    def raw(self, x, y):
        """Grid units -> pixels, without snapping."""
        return ((x + self.ox) * self.k + self.cx - self.size / 2.0,
                (y + self.oy) * self.k + self.cy - self.size / 2.0)

    def px(self, x, y, snap=True):
        X, Y = self.raw(x, y)
        if snap:
            X, Y = self.sx(X), self.sy(Y)
        return X, Y

    # -- raw drawing at 8x (pixels in, supersampled out) -------------------
    def _to_ss(self, pts):
        return [(x * SS, y * SS) for x, y in pts]

    def _stroke_px(self, pts, closed, ink=255, width=None):
        hw = (width or self.w) * SS / 2
        # Pillow fills polygons "inclusively" (one extra 1/8-pixel row), so the
        # band is drawn half a sub-pixel narrower on each side to stay exact.
        band = hw - 0.5
        P = self._to_ss(pts)
        segs = list(zip(P, P[1:]))
        if closed:
            segs.append((P[-1], P[0]))
        for (x0, y0), (x1, y1) in segs:
            L = math.hypot(x1 - x0, y1 - y0)
            if L < 1e-6:
                continue
            nx, ny = -(y1 - y0) / L * band, (x1 - x0) / L * band
            self.draw.polygon([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny),
                               (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], fill=ink)
        for x, y in P:                   # round caps and round joins
            self.draw.ellipse((x - hw, y - hw, x + hw - 1, y + hw - 1), fill=ink)

    def _fill_px(self, pts, ink=255):
        self.draw.polygon(self._to_ss(pts), fill=ink)

    # -- the drawing commands used by the icons ---------------------------
    def line(self, *pts, closed=False, r=0, snap=True, ink=255, width=None):
        """Polyline (or closed outline) with round caps/joins; r rounds corners (units)."""
        P = [self.px(x, y, snap) for x, y in pts]
        radius = [v * self.k for v in r] if isinstance(r, (list, tuple)) else r * self.k
        self._stroke_px(_round_corners(P, radius, closed), closed, ink, width)

    def rect(self, x0, y0, x1, y1, r=0, ink=255):
        self.line((x0, y0), (x1, y0), (x1, y1), (x0, y1), closed=True, r=r, ink=ink)

    def rect_fill(self, x0, y0, x1, y1, r=0, ink=255):
        self.poly_fill((x0, y0), (x1, y0), (x1, y1), (x0, y1), r=r, ink=ink)

    def poly_fill(self, *pts, r=0, snap=True, ink=255):
        P = [self.px(x, y, snap) for x, y in pts]
        radius = [v * self.k for v in r] if isinstance(r, (list, tuple)) else r * self.k
        self._fill_px(_round_corners(P, radius, True), ink)

    def _radius_px(self, cx_px, r_units, snap_fn=None):
        """Radius in pixels such that the circle's left/right edges land on crisp
        positions. Rounds slightly inwards so circles never crowd the box edge."""
        r = r_units * self.k
        if not self.do_snap:
            return r
        left = (snap_fn or self.sx)(cx_px - r, bias=0.15)
        return max(cx_px - left, 0.5)

    def circle(self, cx, cy, r, ink=255):
        X, Y = self.px(cx, cy)
        R = self._radius_px(X, r)
        self._stroke_px(_arc_pts(X, Y, R, R, 0, 360)[:-1], True, ink)

    def ellipse(self, cx, cy, rx, ry, ink=255):
        X, Y = self.px(cx, cy)
        self._stroke_px(_arc_pts(X, Y, self._radius_px(X, rx), self._radius_px(Y, ry, self.sy),
                                 0, 360)[:-1], True, ink)

    def arc(self, cx, cy, r, a0, a1, ink=255):
        """Arc from angle a0 to a1 (degrees, 0 = right, 90 = down, growing clockwise)."""
        X, Y = self.px(cx, cy)
        R = self._radius_px(X, r)
        self._stroke_px(_arc_pts(X, Y, R, R, a0, a1), False, ink)

    def disc(self, cx, cy, r, a0=0, a1=360, ink=255, snap=True):
        """Filled circle (or pie slice) of radius r units."""
        X, Y = self.px(cx, cy, snap)
        R = r * self.k
        pts = _arc_pts(X, Y, R, R, a0, a1)
        if a1 - a0 < 360:
            pts.append((X, Y))
        self._fill_px(pts, ink)

    def dot(self, cx, cy, scale=1.0, ink=255):
        """A round dot whose size follows the stroke weight (keys, pins, lights).
        It is centred so it covers whole pixels: an even-width dot sits on a
        pixel corner, an odd-width dot on a pixel centre."""
        R = self.w * 0.9 * scale
        n = max(1, round(2 * R))                 # dot width in whole pixels
        ph = 0.5 if n % 2 else 0.0
        X, Y = self.raw(cx, cy)
        X, Y = self.sx(X, ph), self.sy(Y, ph)
        if self.do_snap:
            R = n / 2 * 1.08                     # a touch over, to fill the corner pixels
        self._fill_px(_arc_pts(X, Y, R, R, 0, 360), ink)

    def result(self):
        return self.img.resize((self.size, self.size), Image.BOX)


# ---------------------------------------------------------------------------
# The icons (24 x 24 grid; most shapes span about 2.5 .. 21.5)
# ---------------------------------------------------------------------------
def page(p):
    """A sheet of paper with a folded top-right corner (the 'file' base shape)."""
    p.line((4.5, 2.5), (14, 2.5), (19.5, 8), (19.5, 21.5), (4.5, 21.5), closed=True,
           r=[2.5, 0.6, 0.6, 2.5, 2.5])
    p.line((14, 2.5), (14, 8), (19.5, 8), r=[0, 1.5, 0])


def i_terminal(p):
    p.rect(2.5, 4, 21.5, 20, r=2.5)                         # window
    p.line((6.5, 9), (9.5, 12), (6.5, 15))                  # >
    p.line((12, 15), (17.5, 15))                            # _


def i_files(p):
    # an open folder: the back of the folder ...
    p.line((19.5, 10.5), (19.5, 7.5), (11.5, 7.5), (9.5, 5), (2.5, 5), (2.5, 19),
           r=[0, 2, 1, 1, 2, 0])
    # ... and the front flap, tipped forward
    p.line((2.5, 19), (6.5, 10.5), (21.5, 10.5), (18, 19), (2.5, 19), r=[0, 1.5, 1.5, 1.5, 0])


def i_editor(p):
    p.line((11, 3), (3, 3), (3, 21), (21, 21), (21, 13), r=2.5)   # page, open at the top right
    # a pencil, tip at the bottom left
    tip, back, hw, tip_len = (9, 15), (20.4, 3.6), 2.0, 3.0
    ux, uy = (back[0] - tip[0]), (back[1] - tip[1])
    L = math.hypot(ux, uy)
    ux, uy = ux / L, uy / L
    nx, ny = -uy * hw, ux * hw
    ax, ay = tip[0] + ux * tip_len, tip[1] + uy * tip_len
    p.line(tip, (ax + nx, ay + ny), (back[0] + nx, back[1] + ny),
           (back[0] - nx, back[1] - ny), (ax - nx, ay - ny), closed=True,
           r=[0.3, 0.3, 1.6, 1.6, 0.3], snap=False)


def i_python(p):
    # a ">>>" interactive prompt
    for i in range(3):
        x = 3.5 + i * 6
        p.line((x, 7), (x + 5, 12), (x, 17))


def i_car(p):
    p.line((5, 10.5), (7, 5), (17, 5), (19, 10.5), r=[0, 1.5, 1.5, 0])   # roof + windscreen
    p.rect(2.5, 10.5, 21.5, 17, r=2.5)                                 # body
    p.line((6, 17), (6, 19.5))                                         # wheels
    p.line((18, 17), (18, 19.5))
    p.dot(7, 13.75)                                                    # headlights
    p.dot(17, 13.75)


def i_web(p):
    p.circle(12, 12, 9.75)
    p.ellipse(12, 12, 4.25, 9.75)
    p.line((2.25, 12), (21.75, 12))


def _rotate(pts, degrees, cx=12, cy=12):
    """Turn points around (cx, cy)."""
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return [(cx + x * c - y * s, cy + x * s + y * c) for x, y in pts]


def i_electronics(p):
    # a resistor component lying diagonally: body with colour bands + two wire leads
    ang, lead, a, h = -45, 13.2, 6.0, 3.4
    p.line(*_rotate([(-lead, 0), (-a, 0)], ang), snap=False)
    p.line(*_rotate([(a, 0), (lead, 0)], ang), snap=False)
    body = _rotate([(-a, -h), (a, -h), (a, h), (-a, h)], ang)
    if p.small:
        # at 20 px: a slightly fatter body with a single band
        a, h = 6.5, 4.0
        p.line(*_rotate([(-a, -h), (a, -h), (a, h), (-a, h)], ang), closed=True, r=2.6, snap=False)
        p.line(*_rotate([(0, -h), (0, h)], ang), snap=False)
    else:
        p.line(*body, closed=True, r=2.2, snap=False)
        for bx in (-1.8, 1.8):
            p.line(*_rotate([(bx, -h), (bx, h)], ang), snap=False)


def i_arduino(p):
    # a microcontroller board: USB socket and power jack on the left edge,
    # a row of header pins along the top and bottom edges
    p.rect(5.5, 4.5, 21.5, 19.5, r=2)
    p.rect_fill(2.5, 7.5, 9, 11.5, r=0.5)                   # USB socket
    p.rect_fill(2.5, 14, 7.5, 17, r=0.5)                    # power jack
    for x in (12.5, 15.5, 18.5):
        p.dot(x, 7.75, 0.8)
        p.dot(x, 16.25, 0.8)


def sparkle(p, cx, cy, R, waist=0.2):
    """A four-pointed twinkle (filled), made of four curved points.
    waist: how fat the star is between its points (0 = needle thin)."""
    pts = []
    for k in range(4):
        a0 = math.radians(-90 + 90 * k)
        a1 = math.radians(-90 + 90 * (k + 1))
        p0 = (cx + R * math.cos(a0), cy + R * math.sin(a0))
        p2 = (cx + R * math.cos(a1), cy + R * math.sin(a1))
        am = (a0 + a1) / 2
        ctrl = (cx + R * waist * math.cos(am), cy + R * waist * math.sin(am))
        for t in range(12):
            t /= 12
            x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * ctrl[0] + t * t * p2[0]
            y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * ctrl[1] + t * t * p2[1]
            pts.append((x, y))
    p.poly_fill(*pts, snap=False)


def i_claude(p):
    # a chat bubble with a small sparkle: a generic "AI assistant" mark
    p.line((2.5, 3.5), (21.5, 3.5), (21.5, 16.5), (11.5, 16.5), (6.5, 20.5), (6.5, 16.5),
           (2.5, 16.5), closed=True, r=[3, 3, 3, 1, 0.6, 0.6, 3])
    if p.small:
        sparkle(p, 12, 10, 5.2, 0.3)
    else:
        sparkle(p, 12, 10, 4.8, 0.2)


def i_chat(p):
    # a round speech bubble with three dots: messages (the WhatsApp app)
    p.line((2.5, 4), (21.5, 4), (21.5, 17), (11, 17), (5.5, 21), (6.5, 17),
           (2.5, 17), closed=True, r=[5, 5, 5, 1, 0.6, 0.6, 5])
    for x in (7.5, 12, 16.5):
        p.dot(x, 10.5, 0.8 if p.small else 1.0)


def i_media(p):
    p.circle(12, 12, 9.75)
    tri = [(9.75, 7.75), (16.5, 12), (9.75, 16.25)]
    if p.small:
        p.poly_fill(*tri, r=0.5, snap=False)
    else:
        p.line(*tri, closed=True, r=1.0, snap=False)


def i_wifi(p):
    cy = 18.5
    for r in (4.5, 9, 13.5):
        p.arc(12, cy, r, 225, 315)
    p.dot(12, cy, 1.1)


def i_settings(p):
    # a gear: n teeth around a ring, with a hole in the middle
    n = 6 if p.small else 8
    cx = cy = 12
    r_tip, r_root = 10.0, 7.4
    pts, radii = [], []
    period = 2 * math.pi / n
    for j in range(n):
        t = -math.pi / 2 + j * period
        for ang, rad, rr in ((t - period * 0.25, r_root, 1.0), (t - period * 0.15, r_tip, 0.5),
                             (t + period * 0.15, r_tip, 0.5), (t + period * 0.25, r_root, 1.0),
                             (t + period * 0.5, r_root, 0)):
            pts.append((cx + rad * math.cos(ang), cy + rad * math.sin(ang)))
            radii.append(rr)
    p.line(*pts, closed=True, r=radii, snap=False)
    p.circle(cx, cy, 3.25)


def i_taskmgr(p):
    p.rect(2.5, 2.5, 21.5, 21.5, r=2.5)
    p.line((6.5, 15.5), (10, 11.5), (13, 14.5), (17.5, 8.5))


def i_calculator(p):
    p.rect(4.5, 2.5, 19.5, 21.5, r=2.5)
    if p.small:
        p.rect_fill(7.75, 5.5, 16.25, 9.5, r=0.4)               # display
    else:
        p.rect(8, 6, 16, 9.5, r=0.6)
    for y in (13.5, 17.5):                                      # keys
        for x in (8.25, 12, 15.75):
            p.dot(x, y, 0.85)


def i_notes(p):
    # a sticky note with its bottom-right corner peeled up
    f = 13.5 if p.small else 14.5                            # where the fold starts
    p.line((2.5, 2.5), (21.5, 2.5), (21.5, f), (f, 21.5), (2.5, 21.5), closed=True,
           r=[2.5, 2.5, 0.6, 0.6, 2.5])
    p.line((f, 21.5), (f, f), (21.5, f), r=[0, 1.5, 0])
    p.line((7, 8), (17, 8))
    if not p.small:
        p.line((7, 12), (12, 12))


def i_clock(p):
    p.circle(12, 12, 9.75)
    p.line((12, 6.5), (12, 12), (15.5, 14))


def i_screenshot(p):
    # the four corners of a capture frame, with a lens in the middle
    L = 5.5
    for x, y, dx, dy in ((2.5, 2.5, 1, 1), (21.5, 2.5, -1, 1), (2.5, 21.5, 1, -1), (21.5, 21.5, -1, -1)):
        p.line((x, y + dy * L), (x, y), (x + dx * L, y), r=[0, 2.5, 0])
    p.circle(12, 12, 3.75)


def i_system(p):
    # a processor chip with its pins
    p.rect(5.5, 5.5, 18.5, 18.5, r=2)
    pins = (9.5, 14.5) if p.small else (9, 12, 15)
    a, b = (2.75, 21.25) if p.small else (2.25, 21.75)       # pin ends
    for v in pins:
        p.line((v, a), (v, 5.5))
        p.line((v, 18.5), (v, b))
        p.line((a, v), (5.5, v))
        p.line((18.5, v), (b, v))
    if p.small:
        p.rect_fill(9.5, 9.5, 14.5, 14.5, r=0.3)
    else:
        p.rect(9.5, 9.5, 14.5, 14.5, r=0.6)


def i_about(p):
    p.circle(12, 12, 9.75)
    p.dot(12, 7.75, 0.95)
    p.line((12, 11), (12, 16.75))


def i_handheld(p):
    # a pocket game console (screen on top, d-pad and buttons below)
    p.rect(4.5, 2.5, 19.5, 21.5, r=2.5)
    p.rect(7.75, 5.5, 16.25, 11.5, r=0.8)
    if p.small:
        p.line((7.5, 16.5), (11, 16.5))
        p.line((9.25, 14.75), (9.25, 18.25))
        p.dot(15.25, 16.5, 0.9)
    else:
        p.line((7.25, 16.5), (11.25, 16.5))
        p.line((9.25, 14.5), (9.25, 18.5))
        p.dot(16, 15.5, 0.8)
        p.dot(14, 17.75, 0.8)


def i_desktop(p):
    p.rect(2.5, 3.5, 21.5, 16.5, r=2.5)
    p.line((12, 16.5), (12, 20.5))
    p.line((8, 20.5), (16, 20.5))


def i_folder(p):
    p.line((2.5, 5), (9.5, 5), (11.5, 7.5), (21.5, 7.5), (21.5, 19), (2.5, 19), closed=True,
           r=[2, 1, 1, 2, 2, 2])


def i_file(p):
    page(p)


def i_file_text(p):
    page(p)
    if not p.small:
        p.line((8, 9), (10, 9))
    p.line((8, 13), (16, 13))
    p.line((8, 17), (16, 17))


def i_file_code(p):
    page(p)
    if p.small:
        p.line((10.5, 11), (7.5, 14.5), (10.5, 18))
        p.line((13.5, 11), (16.5, 14.5), (13.5, 18))
    else:
        p.line((10.25, 11.5), (7.75, 14.5), (10.25, 17.5))
        p.line((13.75, 11.5), (16.25, 14.5), (13.75, 17.5))


def i_file_image(p):
    page(p)
    if p.small:                                              # sun + hill(s)
        p.dot(9, 11, 0.8)
        p.line((7.5, 18.5), (12.5, 13.5), (16.5, 17.5), r=0.5)
    else:
        p.dot(14, 12, 0.8)
        p.line((7.5, 18.5), (10.5, 15), (13, 17.5), (14.5, 16), (16.5, 18), r=0.5)


def i_file_audio(p):
    page(p)
    p.disc(10, 17.25, 1.9)
    p.line((11.5, 17.25), (11.5, 10.5), (15, 12), r=[0, 0.5, 0])


def i_usb(p):
    # the USB "trident"
    p.line((12, 5.5), (12, 18.5))
    p.poly_fill((12, 2), (15, 6.25), (9, 6.25), r=0.4)        # arrow head
    p.disc(12, 19.5, 2.1)                                       # base
    p.line((12, 16), (6.5, 12.5), (6.5, 10.5), r=1)            # left branch -> circle
    p.disc(6.5, 9.25, 1.7)
    p.line((12, 13.5), (17.5, 10), (17.5, 8.5), r=1)           # right branch -> square
    p.rect_fill(15.75, 5.25, 19.25, 8.75, r=0.3)


def i_image(p):
    p.rect(2.5, 3.5, 21.5, 20.5, r=2.5)
    if p.small:                                             # sun + two hills
        p.dot(16, 8.5, 0.9)
    else:
        p.circle(16, 8.75, 2)
    p.line((2.5, 17.5), (8, 12), (12.5, 16.5), (15.5, 13.5), (21.5, 19), r=0.6)


def i_music(p):
    p.line((8.5, 17.5), (8.5, 5), (20, 3), (20, 15.5), r=[0, 0.8, 0.8, 0])
    if p.small:
        p.disc(5.75, 17.5, 3.2)
        p.disc(17.25, 15.5, 3.2)
    else:
        p.circle(5.75, 17.5, 2.75)
        p.circle(17.25, 15.5, 2.75)


def i_theme(p):
    # a ring with a half-filled disc inside: light / dark
    p.circle(12, 12, 9.75)
    p.disc(12, 12, 6.0, -90, 90)


def i_power(p):
    p.arc(12, 13, 8.75, -50, 230)
    p.line((12, 2.5), (12, 11.5))


def i_keyboard(p):
    p.rect(2.5, 5, 21.5, 19, r=2.5)
    cols = (7, 12, 17) if p.small else (6.5, 10.17, 13.83, 17.5)
    for y in (8.75, 12):
        for x in cols:
            p.dot(x, y, 0.75)
    p.line((8, 15.25), (16, 15.25))


def i_serial(p):
    # a D-sub serial plug (trapezoid shell with two rows of pins)
    p.line((2.5, 6.5), (21.5, 6.5), (19, 17.5), (5, 17.5), closed=True, r=2.5)
    if p.small:
        top, bottom = (8, 12, 16), (10, 14)
    else:
        top, bottom = (7.5, 9.75, 12, 14.25, 16.5), (8.625, 10.875, 13.125, 15.375)
    for x in top:
        p.dot(x, 10, 0.7)
    for x in bottom:
        p.dot(x, 13.75, 0.7)


def i_gauge(p):
    cy = 14
    p.arc(12, cy, 10, 150, 390)
    p.line((12, cy), (16.5, 9.5))
    p.dot(12, cy, 1.1)
    if not p.small:                                      # scale ticks
        for a in (210, 270, 330):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            p.line((12 + 7 * ca, cy + 7 * sa), (12 + 6.4 * ca, cy + 6.4 * sa), snap=False)


def i_lock(p):
    p.rect(4.5, 10.5, 19.5, 21, r=2)                      # body
    p.line((8, 10.5), (8, 7))                             # shackle
    p.line((16, 10.5), (16, 7))
    p.arc(12, 7, 4, 180, 360)
    p.dot(12, 15.75, 1.0)                                 # keyhole


# Order matters: it is the NzIcon enum order in icons.h.
ICONS = [
    ("TERMINAL", i_terminal), ("FILES", i_files), ("EDITOR", i_editor), ("PYTHON", i_python),
    ("CAR", i_car), ("WEB", i_web), ("ELECTRONICS", i_electronics), ("ARDUINO", i_arduino),
    ("CLAUDE", i_claude), ("MEDIA", i_media), ("WIFI", i_wifi), ("SETTINGS", i_settings),
    ("TASKMGR", i_taskmgr), ("CALCULATOR", i_calculator), ("NOTES", i_notes), ("CLOCK", i_clock),
    ("SCREENSHOT", i_screenshot), ("SYSTEM", i_system), ("ABOUT", i_about),
    ("HANDHELD", i_handheld), ("DESKTOP", i_desktop),
    ("FOLDER", i_folder), ("FILE", i_file), ("FILE_TEXT", i_file_text), ("FILE_CODE", i_file_code),
    ("FILE_IMAGE", i_file_image), ("FILE_AUDIO", i_file_audio), ("USB", i_usb), ("IMAGE", i_image),
    ("MUSIC", i_music), ("THEME", i_theme), ("POWER", i_power), ("KEYBOARD", i_keyboard),
    ("SERIAL", i_serial), ("GAUGE", i_gauge), ("LOCK", i_lock), ("CHAT", i_chat),
]


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def centring_offset(fn):
    """Draw the icon once without snapping and work out the shift (in grid
    units) that puts its ink exactly in the middle of the box."""
    pen = Pen(48, snap=False)
    fn(pen)
    box = pen.img.point(lambda v: 255 if v > 0 else 0).getbbox()
    if not box:
        return 0.0, 0.0
    unit = 48 * SS / 24.0
    cx = (box[0] + box[2]) / 2 / unit
    cy = (box[1] + box[3]) / 2 / unit
    # round to a quarter unit so tiny differences don't move lines off the pixel grid
    return round((12 - cx) * 4) / 4, round((12 - cy) * 4) / 4


# Icons with a line or dot straight through their middle (see Pen.__init__):
# "x" = a vertical line on the centre, "y" = a horizontal one.
CENTRE_LINES = {"WEB": "xy", "WIFI": "x", "CLOCK": "x", "ABOUT": "x", "DESKTOP": "x",
                "USB": "x", "POWER": "x", "GAUGE": "x", "LOCK": "x"}


def recentre(img):
    """Pixel snapping can leave an icon a whole pixel off-centre; move it back
    by whole pixels (which keeps every line crisp)."""
    box = img.point(lambda v: 255 if v > 40 else 0).getbbox()
    if not box:
        return img
    size = img.size[0]
    def whole(off):                      # 0.5 px stays (on purpose, see Pen), 1 px moves
        return int(math.copysign(math.floor(abs(off) + 0.25), off))
    dx = whole(size / 2 - (box[0] + box[2]) / 2)
    dy = whole(size / 2 - (box[1] + box[3]) / 2)
    if dx == 0 and dy == 0:
        return img
    moved = Image.new("L", img.size, 0)
    moved.paste(img, (dx, dy))
    return moved


def render_all():
    """{(name, size): PIL 'L' image}"""
    out = {}
    for name, fn in ICONS:
        off = centring_offset(fn)
        for size in SIZES:
            pen = Pen(size, off, centre_lines=CENTRE_LINES.get(name, ""))
            fn(pen)
            out[(name, size)] = recentre(pen.result())
    return out


# ---------------------------------------------------------------------------
# C output
# ---------------------------------------------------------------------------
def write_c(images):
    lines = [
        "/* icons.c - the NZEUS OS icon set. MADE BY tools/make_icons.py - do not edit by hand.",
        " *",
        f" * {len(ICONS)} line icons, each in {len(SIZES)} sizes ({', '.join(str(s) for s in SIZES)} px).",
        " * Format LV_IMG_CF_ALPHA_8BIT: one byte per pixel, row by row, saying how much",
        " * ink covers that pixel (0 = transparent, 255 = solid). The colour comes from the",
        " * style when the icon is drawn (lv_obj_set_style_img_recolor), so every theme",
        " * can paint the same icons in its own colour. */",
        '#include "icons.h"',
        "",
    ]
    total = 0
    for name, _ in ICONS:
        for size in SIZES:
            data = list(images[(name, size)].tobytes())
            total += len(data)
            lines.append(f"static LV_ATTRIBUTE_LARGE_CONST const uint8_t {name.lower()}_{size}[{len(data)}] = {{")
            for row in range(size):
                vals = data[row * size:(row + 1) * size]
                lines.append("    " + ",".join(str(v) for v in vals) + ",")
            lines.append("};")
        lines.append("")
    for size in SIZES:
        lines.append(f"static const lv_img_dsc_t icons_{size}[ICON_COUNT] = {{")
        for name, _ in ICONS:
            lines.append(f"    [ICON_{name}] = {{ .header.cf = LV_IMG_CF_ALPHA_8BIT, .header.always_zero = 0, "
                         f".header.reserved = 0, .header.w = {size}, .header.h = {size}, "
                         f".data_size = {size * size}, .data = {name.lower()}_{size} }},")
        lines.append("};")
        lines.append("")
    lines += [
        "/* The icon picture for `id` at the size nearest to `size` (48, 32 or 20 px). */",
        "const lv_img_dsc_t *icon_get(NzIcon id, int size)",
        "{",
        "    if((int)id < 0 || (int)id >= ICON_COUNT) return NULL;",
        "    if(size >= 40) return &icons_48[id];      /* 40 is half-way between 32 and 48 */",
        "    if(size >= 26) return &icons_32[id];      /* 26 is half-way between 20 and 32 */",
        "    return &icons_20[id];",
        "}",
        "",
    ]
    with open(OUT_C, "w", newline="\n") as f:
        f.write("\n".join(lines))
    return total


def write_h():
    names = [f"ICON_{n}" for n, _ in ICONS]
    enum_lines = []
    for i in range(0, len(names), 4):
        enum_lines.append("    " + ", ".join(names[i:i + 4]) + ",")
    h = f"""/* icons.h - the NZEUS OS icon set. MADE BY tools/make_icons.py - do not edit by hand.
 *
 * Clean outline icons in three sizes: 48 px (home tiles), 32 px (desktop icons)
 * and 20 px (start menu, lists, file rows). They are alpha-only pictures
 * (LV_IMG_CF_ALPHA_8BIT), so they take the colour of the style that draws them:
 *
 *     lv_obj_t *img = lv_img_create(parent);
 *     lv_img_set_src(img, icon_get(ICON_WIFI, 20));
 *     lv_obj_set_style_img_recolor(img, accent_colour, 0);
 *     lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
 */
#ifndef NZEUS_ICONS_H
#define NZEUS_ICONS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

typedef enum {{
    /* the 12 home-screen apps, in screen order */
{chr(10).join(enum_lines[:3])}
    /* everything else */
{chr(10).join(enum_lines[3:])}
    ICON_COUNT
}} NzIcon;

/* The icon `id` at `size` px: 48, 32 or 20 (any other size gives the nearest one).
 * Returns NULL for an unknown id. */
const lv_img_dsc_t *icon_get(NzIcon id, int size);

#ifdef __cplusplus
}}
#endif

#endif
"""
    with open(OUT_H, "w", newline="\n") as f:
        f.write(h)


# ---------------------------------------------------------------------------
# Preview contact sheet
# ---------------------------------------------------------------------------
def _font(size):
    for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def _paint(mask, fg):
    """Colour an alpha mask: returns an RGBA image of colour fg."""
    img = Image.new("RGBA", mask.size, fg + (0,))
    img.putalpha(mask)
    return img


def write_preview(images):
    themes = [("dark theme  -  #3CC878 on #0E1016", (0x0E, 0x10, 0x16), (0x3C, 0xC8, 0x78), (150, 155, 170)),
              ("light theme  -  #1F9D57 on #F3F5F8", (0xF3, 0xF5, 0xF8), (0x1F, 0x9D, 0x57), (90, 95, 110))]
    cols, cell_w, cell_h, margin = 8, 150, 86, 16
    rows = (len(ICONS) + cols - 1) // cols
    grid_w = cols * cell_w
    zoom_cols = 19
    zoom_rows = (len(ICONS) + zoom_cols - 1) // zoom_cols
    zoom_cell = 60
    title_h = 30
    sec_h = title_h + rows * cell_h + 8
    zoom_h = title_h + zoom_rows * (zoom_cell + 14) + 8
    W = grid_w + 2 * margin
    H = 40 + 2 * sec_h + 2 * zoom_h + margin
    sheet = Image.new("RGB", (W, H), (40, 42, 48))
    d = ImageDraw.Draw(sheet)
    f_title, f_label, f_small = _font(15), _font(11), _font(9)
    d.text((margin, 10), f"NZEUS OS icons - {len(ICONS)} icons x {len(SIZES)} sizes (48 / 32 / 20 px)",
           font=_font(17), fill=(235, 235, 240))
    y = 40
    for title, bg, fg, label_col in themes:          # main grids
        d.rectangle((margin, y, margin + grid_w - 1, y + sec_h - 1), fill=bg)
        d.text((margin + 10, y + 7), title, font=f_title, fill=label_col)
        for i, (name, _) in enumerate(ICONS):
            cx0 = margin + (i % cols) * cell_w
            cy0 = y + title_h + (i // cols) * cell_h
            x = cx0 + 12
            for size in SIZES:
                top = cy0 + 4 + (48 - size) // 2
                sheet.paste(_paint(images[(name, size)], fg), (x, top), _paint(images[(name, size)], fg))
                x += size + 10
            d.text((cx0 + 12, cy0 + 58), f"{i:2d} {name.lower()}", font=f_label, fill=label_col)
        y += sec_h
    for title, bg, fg, label_col in themes:          # 20 px icons at 2x (nearest-neighbour)
        d.rectangle((margin, y, margin + grid_w - 1, y + zoom_h - 1), fill=bg)
        d.text((margin + 10, y + 7), "20 px icons at 2x zoom  -  " + title.split("  -  ")[0], font=f_title,
               fill=label_col)
        for i, (name, _) in enumerate(ICONS):
            cx0 = margin + 10 + (i % zoom_cols) * zoom_cell
            cy0 = y + title_h + (i // zoom_cols) * (zoom_cell + 14)
            big = images[(name, 20)].resize((40, 40), Image.NEAREST)
            sheet.paste(_paint(big, fg), (cx0 + 8, cy0), _paint(big, fg))
            d.text((cx0 + 2, cy0 + 44), name.lower()[:10], font=f_small, fill=label_col)
        y += zoom_h
    os.makedirs(os.path.dirname(OUT_PNG), exist_ok=True)
    sheet.save(OUT_PNG)


def report(images):
    """Print anything that looks off: ink not centred, or too close to the edge."""
    for (name, size), img in sorted(images.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        box = img.point(lambda v: 255 if v > 40 else 0).getbbox()
        if not box:
            print(f"  EMPTY: {name} {size}")
            continue
        dx = (box[0] + box[2]) / 2 - size / 2
        dy = (box[1] + box[3]) / 2 - size / 2
        if abs(dx) > 0.5 or abs(dy) > 0.5 or min(box[0], box[1], size - box[2], size - box[3]) < 1:
            print(f"  check {name:12s} {size:2d}px: ink box {box}, off-centre by ({dx:+.1f}, {dy:+.1f})")


def main():
    images = render_all()
    total = write_c(images)
    write_h()
    write_preview(images)
    report(images)
    print(f"{len(ICONS)} icons x {len(SIZES)} sizes, {total} bytes of image data ({total / 1024:.1f} KiB)")
    print(f"-> {OUT_C}\n-> {OUT_H}\n-> {OUT_PNG}")


if __name__ == "__main__":
    main()

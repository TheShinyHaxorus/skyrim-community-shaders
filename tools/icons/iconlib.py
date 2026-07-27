"""Minimal vector-ish drawing toolkit for the Community Shaders icon set.

All coordinates are in icon space (0..SIZE). Rendering happens on a
supersampled 'L' ink mask and is downsampled at the end, which gives clean
antialiasing without needing an SVG rasteriser.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

SIZE = 128
SS = 6  # supersample factor
DEV = SIZE * SS

STROKE = 10.0  # default stroke width, icon px
ACCENT = (76, 165, 255)  # #4CA5FF
DANGER = (255, 109, 109)  # #FF6D6D
WHITE = (255, 255, 255)


def _d(v):
    return v * SS


class Mask:
    """A single-channel ink layer drawn at supersampled resolution."""

    def __init__(self):
        self.img = Image.new("L", (DEV, DEV), 0)
        self.dr = ImageDraw.Draw(self.img)

    # --- primitives -----------------------------------------------------
    def polyline(self, pts, w=STROKE, closed=False):
        """Stroke a polyline with round caps and round joins."""
        if len(pts) < 2:
            if pts:
                self.dot(pts[0][0], pts[0][1], w / 2)
            return
        p = list(pts) + ([pts[0]] if closed else [])
        r = _d(w) / 2.0
        for (x0, y0), (x1, y1) in zip(p, p[1:]):
            self.dr.line([_d(x0), _d(y0), _d(x1), _d(y1)], fill=255, width=int(round(_d(w))))
        for x, y in p:  # round joins + caps
            self.dr.ellipse([_d(x) - r, _d(y) - r, _d(x) + r, _d(y) + r], fill=255)

    def polygon(self, pts):
        self.dr.polygon([(_d(x), _d(y)) for x, y in pts], fill=255)

    def dot(self, cx, cy, r):
        self.dr.ellipse([_d(cx - r), _d(cy - r), _d(cx + r), _d(cy + r)], fill=255)

    def circle(self, cx, cy, r, w=STROKE):
        self.polyline(arc_pts(cx, cy, r, 0, 360), w, closed=True)

    def disc(self, cx, cy, r):
        self.dot(cx, cy, r)

    def arc(self, cx, cy, r, a0, a1, w=STROKE):
        self.polyline(arc_pts(cx, cy, r, a0, a1), w)

    def ellipse_ring(self, cx, cy, rx, ry, w=STROKE):
        self.polyline(ellipse_pts(cx, cy, rx, ry, 0, 360), w, closed=True)

    def rrect(self, x0, y0, x1, y1, rad, w=STROKE, fill=False):
        pts = rrect_pts(x0, y0, x1, y1, rad)
        if fill:
            self.polygon(pts)
        else:
            self.polyline(pts, w, closed=True)

    def bar(self, x0, y0, x1, y1, w):
        """A round-capped thick bar from (x0,y0) to (x1,y1)."""
        self.polyline([(x0, y0), (x1, y1)], w)

    def arrow_head(self, tip, ang_deg, size=22, w=STROKE, spread=42):
        """V-shaped head at `tip`, opening away from heading `ang_deg`."""
        tx, ty = tip
        for s in (+1, -1):
            a = math.radians(ang_deg + 180 + s * spread)
            self.polyline([(tx, ty), (tx + size * math.cos(a), ty + size * math.sin(a))], w)

    # --- combination ----------------------------------------------------
    def to_np(self):
        return np.asarray(self.img, dtype=np.float32) / 255.0

    def dilated(self, gap):
        """Grow the ink by `gap` icon px (used to knock a halo out of a layer)."""
        a = self.to_np()
        rad = int(round(_d(gap)))
        if rad <= 0:
            return a
        out = a.copy()
        # disc dilation via shifted maxima over the offsets of a filled circle
        for dy in range(-rad, rad + 1):
            span = int(math.sqrt(max(rad * rad - dy * dy, 0)))
            if span == 0 and dy != 0:
                continue
            shifted = np.roll(a, dy, axis=0)
            if dy > 0:
                shifted[:dy, :] = 0
            elif dy < 0:
                shifted[dy:, :] = 0
            # horizontal max over [-span, span] via cumulative sliding window
            acc = shifted
            step = 1
            while step <= span:
                acc = np.maximum(acc, np.roll(acc, step, axis=1))
                acc = np.maximum(acc, np.roll(acc, -step, axis=1))
                step *= 2
            out = np.maximum(out, acc)
        return out


    def add_np(self, arr):
        cur = self.to_np()
        merged = np.maximum(cur, arr)
        self.img = Image.fromarray((merged * 255).astype(np.uint8), "L")
        self.dr = ImageDraw.Draw(self.img)

    def circular_arrow(self, cx, cy, r, a0, a1, w=STROKE, head=22):
        """Arc from a0 to a1 (degrees) with an arrowhead at the a1 end."""
        self.arc(cx, cy, r, a0, a1, w)
        end = math.radians(a1)
        tip = (cx + r * math.cos(end), cy + r * math.sin(end))
        sign = 1.0 if a1 >= a0 else -1.0
        heading = math.degrees(math.atan2(sign * math.cos(end), -sign * math.sin(end)))
        self.arrow_head(tip, heading, head, w)


def silhouette_ring(shapes, w=STROKE):
    """Outline of a union of convex shapes: outer silhouette minus an inset copy.

    shapes: ('disc', cx, cy, r) or ('rect', x0, y0, x1, y1)
    Produces a clean cloud/blob outline that overlapping strokes cannot.
    """
    outer, inner = Mask(), Mask()
    for s in shapes:
        if s[0] == "disc":
            _, cx, cy, r = s
            outer.disc(cx, cy, r)
            if r - w > 0:
                inner.disc(cx, cy, r - w)
        else:
            _, x0, y0, x1, y1 = s
            outer.polygon([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
            if x1 - x0 > 2 * w and y1 - y0 > 2 * w:
                inner.polygon([(x0 + w, y0 + w), (x1 - w, y0 + w), (x1 - w, y1 - w), (x0 + w, y1 - w)])
    m = Mask()
    m.add_np(np.clip(outer.to_np() - inner.to_np(), 0, 1))
    return m


def arc_pts(cx, cy, r, a0, a1, seg=None):
    """Sample an arc. Angles in degrees, 0 = +x, increasing clockwise on screen."""
    sweep = abs(a1 - a0)
    n = seg or max(8, int(sweep / 4) + 2)
    return [
        (
            cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
            cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)),
        )
        for i in range(n + 1)
    ]


def ellipse_pts(cx, cy, rx, ry, a0, a1, seg=None):
    sweep = abs(a1 - a0)
    n = seg or max(10, int(sweep / 4) + 2)
    return [
        (
            cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
            cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n)),
        )
        for i in range(n + 1)
    ]


def rrect_pts(x0, y0, x1, y1, rad):
    rad = min(rad, (x1 - x0) / 2, (y1 - y0) / 2)
    p = []
    p += arc_pts(x1 - rad, y0 + rad, rad, -90, 0)
    p += arc_pts(x1 - rad, y1 - rad, rad, 0, 90)
    p += arc_pts(x0 + rad, y1 - rad, rad, 90, 180)
    p += arc_pts(x0 + rad, y0 + rad, rad, 180, 270)
    return p


def bezier(p0, p1, p2, n=28):
    """Quadratic bezier sample points."""
    out = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        out.append(
            (
                u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1],
            )
        )
    return out


def compose(base, accent=None, accent_color=ACCENT, mono=False, gap=3.5):
    """Combine ink layers into a finished RGBA icon.

    In mono mode the accent is drawn white but a `gap`-wide halo is knocked out
    of the base first, so the two shapes stay readable without colour.
    """
    b = base.to_np()
    if accent is not None and mono:
        b = np.clip(b - accent.dilated(gap), 0, 1)

    # base layer
    rgb = np.dstack([b, b, b]).astype(np.float32)  # white ink
    alpha = b

    # accent composited over the base
    if accent is not None:
        a = accent.to_np()
        col = np.array(WHITE if mono else accent_color, dtype=np.float32) / 255.0
        rgb = rgb * (1.0 - a[..., None]) + col * a[..., None]
        alpha = np.maximum(alpha, a)

    np.clip(rgb, 0, 1, out=rgb)
    out = np.dstack([rgb * 255.0, alpha * 255.0]).astype(np.uint8)
    img = Image.fromarray(out, "RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    return img

"""Rasterise a single-path SVG icon into the Community Shaders 128x128 RGBA icon format.

No SVG rasteriser (cairosvg/pycairo/resvg) is available in this environment, so this module
implements just enough of the SVG path grammar to render the flat, single-colour icons used by
the menu: the full path command set (M/L/H/V/C/S/Q/T/A/Z, absolute and relative), nonzero-winding
fill, and box-filtered supersampling for antialiasing.

Usage:
    python tools/icons/svg_to_icon.py <source.svg> <dest.png> [--size 128] [--safe 104]
"""

from __future__ import annotations

import argparse
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

SUPERSAMPLE = 8
CURVE_SEGMENTS = 24
ARC_SEGMENTS_PER_TURN = 96

_TOKEN = re.compile(r"[MmZzLlHhVvCcSsQqTtAa]|-?\d*\.?\d+(?:[eE][-+]?\d+)?")


def _tokenize(d: str) -> list[str]:
    return _TOKEN.findall(d)


def _arc_to_cubics(x0, y0, rx, ry, phi_deg, large_arc, sweep, x1, y1):
    """Endpoint -> centre parameterisation (SVG spec F.6.5), emitted as polyline points."""
    if rx == 0 or ry == 0 or (x0 == x1 and y0 == y1):
        return [(x1, y1)]

    rx, ry = abs(rx), abs(ry)
    phi = math.radians(phi_deg)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)

    dx2 = (x0 - x1) / 2.0
    dy2 = (y0 - y1) / 2.0
    x1p = cos_phi * dx2 + sin_phi * dy2
    y1p = -sin_phi * dx2 + cos_phi * dy2

    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:
        scale = math.sqrt(lam)
        rx *= scale
        ry *= scale

    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large_arc == sweep:
        coef = -coef
    cxp = coef * rx * y1p / ry
    cyp = -coef * ry * x1p / rx

    cx = cos_phi * cxp - sin_phi * cyp + (x0 + x1) / 2.0
    cy = sin_phi * cxp + cos_phi * cyp + (y0 + y1) / 2.0

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        det = ux * vy - uy * vx
        return math.atan2(det, dot)

    theta1 = angle(1.0, 0.0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dtheta > 0:
        dtheta -= 2 * math.pi
    elif sweep and dtheta < 0:
        dtheta += 2 * math.pi

    steps = max(2, int(ARC_SEGMENTS_PER_TURN * abs(dtheta) / (2 * math.pi)) + 1)
    points = []
    for i in range(1, steps + 1):
        theta = theta1 + dtheta * i / steps
        px = cos_phi * rx * math.cos(theta) - sin_phi * ry * math.sin(theta) + cx
        py = sin_phi * rx * math.cos(theta) + cos_phi * ry * math.sin(theta) + cy
        points.append((px, py))
    return points


def parse_path(d: str) -> list[list[tuple[float, float]]]:
    """Flatten an SVG path into a list of closed subpaths of points."""
    tokens = _tokenize(d)
    i = 0
    subpaths: list[list[tuple[float, float]]] = []
    current: list[tuple[float, float]] = []
    cx = cy = 0.0
    start_x = start_y = 0.0
    prev_cubic_ctrl = None
    prev_quad_ctrl = None
    command = None

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    def flush():
        nonlocal current
        if len(current) >= 3:
            subpaths.append(current)
        current = []

    while i < len(tokens):
        token = tokens[i]
        if token.isalpha():
            command = token
            i += 1
        elif command in (None, "Z", "z"):
            raise ValueError(f"unexpected numeric token at {i} in path")

        rel = command.islower()
        op = command.upper()

        if op == "M":
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            flush()
            cx, cy = x, y
            start_x, start_y = x, y
            current = [(cx, cy)]
            command = "l" if rel else "L"
            prev_cubic_ctrl = prev_quad_ctrl = None
        elif op == "L":
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            cx, cy = x, y
            current.append((cx, cy))
            prev_cubic_ctrl = prev_quad_ctrl = None
        elif op == "H":
            x = num()
            cx = cx + x if rel else x
            current.append((cx, cy))
            prev_cubic_ctrl = prev_quad_ctrl = None
        elif op == "V":
            y = num()
            cy = cy + y if rel else y
            current.append((cx, cy))
            prev_cubic_ctrl = prev_quad_ctrl = None
        elif op in ("C", "S"):
            if op == "C":
                x1, y1 = num(), num()
                if rel:
                    x1, y1 = cx + x1, cy + y1
            else:
                x1, y1 = (2 * cx - prev_cubic_ctrl[0], 2 * cy - prev_cubic_ctrl[1]) if prev_cubic_ctrl else (cx, cy)
            x2, y2 = num(), num()
            x, y = num(), num()
            if rel:
                x2, y2 = cx + x2, cy + y2
                x, y = cx + x, cy + y
            for s in range(1, CURVE_SEGMENTS + 1):
                t = s / CURVE_SEGMENTS
                mt = 1 - t
                px = mt**3 * cx + 3 * mt**2 * t * x1 + 3 * mt * t**2 * x2 + t**3 * x
                py = mt**3 * cy + 3 * mt**2 * t * y1 + 3 * mt * t**2 * y2 + t**3 * y
                current.append((px, py))
            prev_cubic_ctrl = (x2, y2)
            prev_quad_ctrl = None
            cx, cy = x, y
        elif op in ("Q", "T"):
            if op == "Q":
                x1, y1 = num(), num()
                if rel:
                    x1, y1 = cx + x1, cy + y1
            else:
                x1, y1 = (2 * cx - prev_quad_ctrl[0], 2 * cy - prev_quad_ctrl[1]) if prev_quad_ctrl else (cx, cy)
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            for s in range(1, CURVE_SEGMENTS + 1):
                t = s / CURVE_SEGMENTS
                mt = 1 - t
                px = mt**2 * cx + 2 * mt * t * x1 + t**2 * x
                py = mt**2 * cy + 2 * mt * t * y1 + t**2 * y
                current.append((px, py))
            prev_quad_ctrl = (x1, y1)
            prev_cubic_ctrl = None
            cx, cy = x, y
        elif op == "A":
            rx, ry, rot = num(), num(), num()
            large_arc, sweep = int(num()), int(num())
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            current.extend(_arc_to_cubics(cx, cy, rx, ry, rot, large_arc, sweep, x, y))
            cx, cy = x, y
            prev_cubic_ctrl = prev_quad_ctrl = None
        elif op == "Z":
            flush()
            cx, cy = start_x, start_y
            current = [(cx, cy)]
            prev_cubic_ctrl = prev_quad_ctrl = None
        else:
            raise ValueError(f"unsupported path command {command!r}")

    flush()
    return subpaths


def rasterise(subpaths, width, height):
    """Nonzero-winding scanline fill at SUPERSAMPLE resolution, box-filtered down."""
    hi_w, hi_h = width * SUPERSAMPLE, height * SUPERSAMPLE
    coverage = np.zeros((hi_h, hi_w), dtype=np.uint8)

    edges = []
    for points in subpaths:
        scaled = [(x * SUPERSAMPLE, y * SUPERSAMPLE) for x, y in points]
        closed = scaled + [scaled[0]]
        for (x0, y0), (x1, y1) in zip(closed, closed[1:]):
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    if not edges:
        return np.zeros((height, width), dtype=np.uint8)

    ex0 = np.array([e[0] for e in edges])
    ey0 = np.array([e[1] for e in edges])
    ex1 = np.array([e[2] for e in edges])
    ey1 = np.array([e[3] for e in edges])
    direction = np.where(ey1 > ey0, 1, -1)
    ytop = np.minimum(ey0, ey1)
    ybot = np.maximum(ey0, ey1)
    slope = (ex1 - ex0) / (ey1 - ey0)

    xs = np.arange(hi_w)
    for row in range(hi_h):
        sample_y = row + 0.5
        hit = (ytop <= sample_y) & (ybot > sample_y)
        if not hit.any():
            continue
        crossings = ex0[hit] + (sample_y - ey0[hit]) * slope[hit]
        winding = direction[hit]
        order = np.argsort(crossings, kind="stable")
        crossings = crossings[order]
        accumulated = np.cumsum(winding[order])
        inside = accumulated != 0
        for idx in np.nonzero(inside)[0]:
            if idx + 1 >= len(crossings):
                continue
            left = int(math.ceil(crossings[idx] - 0.5))
            right = int(math.ceil(crossings[idx + 1] - 0.5))
            if right > left:
                coverage[row, max(0, left):min(hi_w, right)] = 255

    block = coverage.reshape(height, SUPERSAMPLE, width, SUPERSAMPLE)
    return block.mean(axis=(1, 3)).round().astype(np.uint8)


def load_svg_paths(path: Path):
    root = ET.parse(path).getroot()
    view_box = root.get("viewBox")
    if view_box:
        vx, vy, vw, vh = (float(v) for v in view_box.replace(",", " ").split())
    else:
        vx, vy = 0.0, 0.0
        vw = float(root.get("width", 24))
        vh = float(root.get("height", 24))

    subpaths = []
    for element in root.iter():
        if element.tag.split("}")[-1] != "path":
            continue
        d = element.get("d")
        if d:
            subpaths.extend(parse_path(d))
    return subpaths, (vx, vy, vw, vh)


def render(svg_path: Path, size: int, safe: int, color: tuple[int, int, int], fit_ink: bool = True) -> Image.Image:
    subpaths, (vx, vy, vw, vh) = load_svg_paths(svg_path)

    if fit_ink:
        xs = [p[0] for sub in subpaths for p in sub]
        ys = [p[1] for sub in subpaths for p in sub]
        vx, vy = min(xs), min(ys)
        vw, vh = max(xs) - vx, max(ys) - vy

    scale = safe / max(vw, vh)
    offset_x = (size - vw * scale) / 2.0
    offset_y = (size - vh * scale) / 2.0

    placed = [[((x - vx) * scale + offset_x, (y - vy) * scale + offset_y) for x, y in sub] for sub in subpaths]
    alpha = rasterise(placed, size, size)

    rgba = np.zeros((size, size, 4), dtype=np.uint8)
    rgba[..., 0] = color[0]
    rgba[..., 1] = color[1]
    rgba[..., 2] = color[2]
    rgba[..., 3] = alpha
    return Image.fromarray(rgba, "RGBA")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("dest")
    parser.add_argument("--size", type=int, default=128)
    parser.add_argument("--safe", type=int, default=104)
    parser.add_argument("--color", default="255,255,255")
    parser.add_argument("--no-fit-ink", action="store_true")
    args = parser.parse_args()

    color = tuple(int(c) for c in args.color.split(","))
    image = render(Path(args.source), args.size, args.safe, color, not args.no_fit_ink)
    dest = Path(args.dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    image.save(dest)
    print(f"wrote {dest}")


if __name__ == "__main__":
    main()

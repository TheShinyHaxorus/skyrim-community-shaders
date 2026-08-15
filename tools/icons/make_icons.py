"""Generates the Community Shaders icon set.

Design system
-------------
canvas 128x128, glyph inside a 104x104 safe box (12px margin)
stroke  10px, round caps + round joins  (~1.9px at the 24px UI render size)
colour  category icons are pure white (the UI tints them with the theme text
        colour); action icons get one accent -- ACCENT for normal actions,
        DANGER for destructive ones -- and ship a Monochrome twin where the
        accent is knocked out of the base with a halo instead of coloured.
"""

import sys
from pathlib import Path

from iconlib import (ACCENT, DANGER, STROKE, Mask, arc_pts, bezier, compose,
                     ellipse_pts, silhouette_ring)

W = STROKE
CATEGORY = {}
ACTION = {}


def category(fn):
    CATEGORY[fn.__name__.replace("_", "-")] = fn
    return fn


def action(fn):
    ACTION[fn.__name__.replace("_", "-")] = fn
    return fn


# ---------------------------------------------------------------- categories

@category
def characters():
    m = Mask()
    m.circle(64, 42, 20, W)                      # head
    m.polyline(arc_pts(64, 116, 38, 190, 350), W)  # shoulders
    return m, None


@category
def display():
    m = Mask()
    m.rrect(18, 26, 110, 88, 12, W)  # screen
    m.bar(64, 88, 64, 100, W)        # stand
    m.bar(44, 104, 84, 104, W)       # foot
    return m, None


@category
def grass():
    m = Mask()
    # control points must sit well off the chord, otherwise the quadratic
    # degenerates to a straight line and the tuft reads as a bundle of sticks
    m.polyline(bezier((50, 112), (46, 80), (18, 58)), W)
    m.polyline(bezier((58, 112), (54, 70), (38, 34)), W)
    m.polyline(bezier((67, 112), (70, 68), (72, 28)), W)
    m.polyline(bezier((76, 112), (82, 72), (104, 42)), W)
    return m, None


@category
def lighting():
    m = Mask()
    m.circle(64, 50, 25, W)     # bulb
    m.bar(52, 84, 76, 84, W)    # screw base
    m.bar(56, 98, 72, 98, W)
    return m, None


@category
def sky():
    m = Mask()
    for a in range(-195, 0, 45):  # rays only in the open upper-left arc
        m.polyline(arc_pts(34, 32, 20, a, a) + arc_pts(34, 32, 28, a, a), 8)
    m.circle(34, 32, 12, 8)
    m.add_np(
        silhouette_ring(
            [
                ("disc", 70, 72, 30),
                ("disc", 38, 86, 20),
                ("disc", 100, 86, 18),
                ("rect", 38, 78, 100, 106),
            ],
            W,
        ).to_np()
    )
    return m, None


@category
def landscape():
    m = Mask()
    m.polyline([(16, 98), (46, 50), (63, 76), (82, 42), (112, 98)], W)
    m.bar(16, 98, 112, 98, W)
    return m, None


@category
def water():
    m = Mask()
    p = [(64, 16)]
    p += bezier((64, 16), (96, 56), (96, 74))
    p += arc_pts(64, 74, 32, 0, 180)
    p += bezier((32, 74), (32, 56), (64, 16))
    m.polyline(p, W, closed=True)
    return m, None


@category
def debug():
    m = Mask()
    m.polyline(arc_pts(42, 42, 21, -55, 215), 11)  # open wrench jaw
    m.bar(55, 55, 100, 100, 13)                    # handle
    return m, None


@category
def materials():
    m = Mask()
    m.rrect(20, 20, 108, 108, 12, W)  # swatch
    m.rrect(34, 34, 60, 60, 4, fill=True)
    m.rrect(68, 68, 94, 94, 4, fill=True)
    return m, None


@category
def post_processing():
    m = Mask()
    m.circle(64, 64, 40, W)
    # left half filled -> the classic contrast/grading mark
    m.polygon(arc_pts(64, 64, 35, 90, 270))
    return m, None


# ------------------------------------------------------------------ actions

@action
def save_settings():
    # Floppy: body outline, knocked-out shutter at top, label plate at bottom.
    base, acc = Mask(), Mask()
    base.rrect(20, 20, 108, 108, 10, W)
    base.rrect(42, 74, 86, 108, 4, W)         # label plate, shares the body's bottom edge
    acc.rrect(50, 34, 78, 60, 3, fill=True)   # shutter, inset so its halo misses the body
    return base, acc


@action
def load_settings():
    # Open folder: back panel with tab, accent front flap.
    base, acc = Mask(), Mask()
    base.polyline([(18, 100), (18, 36), (48, 36), (58, 50), (102, 50), (102, 68)], W)
    acc.polyline([(22, 102), (38, 64), (112, 64), (96, 102)], W, closed=True)
    return base, acc


@action
def clear_cache():
    # Cache as a data cylinder with a clear-badge; the badge sits clear of the
    # body so it stays separable at 20px instead of filling it.
    base, acc = Mask(), Mask()
    base.ellipse_ring(54, 30, 30, 11, W)                       # top disc
    base.bar(24, 30, 24, 72, W)                                # left wall
    base.bar(84, 30, 84, 62, W)                                # right wall
    base.polyline(ellipse_pts(54, 72, 30, 11, 0, 180), W)      # bottom rim
    acc.bar(80, 112, 112, 80, W)                               # clear badge
    acc.bar(80, 80, 112, 112, W)
    return base, acc


@action
def delete():
    base, acc = Mask(), Mask()
    base.polyline([(34, 36), (41, 106), (87, 106), (94, 36)], W)  # can
    base.bar(58, 56, 58, 90, 7)
    base.bar(70, 56, 70, 90, 7)
    acc.bar(24, 34, 104, 34, W)                                   # lid
    acc.polyline([(52, 34), (52, 20), (76, 20), (76, 34)], 9)     # handle
    return base, acc, DANGER


@action
def restore_settings():
    base, acc = Mask(), Mask()
    acc.circular_arrow(64, 64, 34, 60, -210, W)  # counter-clockwise revert loop
    return base, acc


@action
def apply_to_game():
    base, acc = Mask(), Mask()
    base.polyline([(22, 74), (22, 106), (106, 106), (106, 74)], W)  # tray
    acc.bar(64, 20, 64, 78, W)                                      # arrow in
    acc.arrow_head((64, 80), 90, 24, W)
    return base, acc


@action
def pause():
    base, acc = Mask(), Mask()
    base.circle(64, 64, 40, W)
    acc.bar(53, 46, 53, 82, 13)
    acc.bar(75, 46, 75, 82, 13)
    return base, acc


@action
def undo():
    base, acc = Mask(), Mask()
    # straight shaft with a hooked tail -- deliberately unlike the full
    # circular loop used by restore-settings, so the two never read alike
    acc.polyline([(38, 54), (70, 54)] + bezier((70, 54), (100, 58), (98, 98)), W)
    acc.arrow_head((36, 54), 180, 22, W)
    return base, acc


@action
def free_camera():
    base, acc = Mask(), Mask()
    base.polyline([(42, 38), (49, 24), (75, 24), (82, 38)], 9)  # viewfinder
    base.rrect(16, 38, 112, 104, 12, W)                          # body
    acc.circle(64, 71, 18, 9)                                    # lens
    return base, acc


@action
def play_mode():
    base, acc = Mask(), Mask()
    acc.polyline([(46, 26), (46, 102), (104, 64)], W, closed=True)
    return base, acc


# --------------------------------------------------------------------- main

def unpack(res):
    if len(res) == 3:
        return res[0], res[1], res[2]
    return res[0], res[1], ACCENT


def main(out_root):
    out_root = Path(out_root)
    cat_dir = out_root / "Categories"
    act_dir = out_root / "Action Icons"
    mono_dir = act_dir / "Monochrome"
    for d in (cat_dir, act_dir, mono_dir):
        d.mkdir(parents=True, exist_ok=True)

    n = 0
    for name, fn in CATEGORY.items():
        base, acc = fn()
        compose(base, acc).save(cat_dir / f"{name}.png")
        n += 1

    for name, fn in ACTION.items():
        base, acc, col = unpack(fn())
        compose(base, acc, col, mono=False).save(act_dir / f"{name}.png")
        compose(base, acc, col, mono=True).save(mono_dir / f"{name}.png")
        n += 2

    print(f"wrote {n} icons to {out_root}")


if __name__ == "__main__":
    main(sys.argv[1])

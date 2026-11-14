#!/usr/bin/env python3
"""Pixel level check of the flixel batch entry point (lime_vulkan_batch_quads_d plus the
camera canvas view matrix).

Why it has its own test: when these numbers are wrong the image still looks
normal, only the position, the scale or the sampling is slightly off, which is
invisible in game.

The geometry is hard coded in the demo's --scene=quads, the view matrix is
(2,0,0,2, 64,32):

  Q0  source (0,0,128,128)  identity           -> screen ( 64, 32)-(320,288)  four quadrants
  Q1  source (0,0,64,64)    translate (128,0)  -> screen (320, 32)-(448,160)  solid red
  Q2  source (64,0,64,64)   translate (400,200)-> screen (864,432)-( 992,560) solid green

So every assertion checks several things at once: the quad matrix is applied
first and the canvas matrix after it, the uv is normalised by the texture size,
and the y axis is not flipped.

    python tests/test_quad_batches.py
"""

from __future__ import annotations

import sys

from test_shader_pixels import capture

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)

# (label, client coordinates, expected colour, tolerance)
SAMPLES = [
    # Q0: the whole four quadrant texture, which checks that the identity quad
    # matrix plus the view matrix only translate and scale
    ("q0-red", (128, 96), RED, 4),
    ("q0-green", (256, 96), GREEN, 4),
    ("q0-blue", (128, 224), BLUE, 4),
    ("q0-white", (256, 224), WHITE, 4),
    # Everything outside Q0 must be clear black: the view matrix drew nothing outside the canvas
    ("outside-corner", (16, 16), BLACK, 3),
    ("outside-middle", (600, 600), BLACK, 3),
    # Q1: the quad matrix translates again and the uv takes only the top left quarter
    ("q1-red", (384, 96), RED, 4),
    ("q1-near-left", (330, 96), RED, 6),
    ("q1-beyond-right", (470, 96), BLACK, 3),
    # Q2: the per-vertex alpha and colour multiplier array branch, uv takes the
    # top right quarter (green). The world rectangle is 64x64, doubled by the view
    # to 128x128 on screen at (864,432)-(992,560). The earlier sample points
    # assumed (992,432)-(1120,560), a full rectangle width off.
    ("q2-green", (928, 496), GREEN, 4),
    # Right and bottom must be outside the rectangle: these two assert that
    # nothing was drawn too large, do not delete them
    ("q2-beyond-right", (1056, 496), BLACK, 3),
    ("q2-beyond-bottom", (928, 620), BLACK, 3),
]


def main() -> int:
    from PIL import Image

    shot, info = capture("quads", "--scene=quads")

    image = Image.open(shot).convert("RGB")

    left = int(info["clientX"])
    top = int(info["clientY"])
    width = int(info["clientWidth"])
    height = int(info["clientHeight"])

    client = image.crop((left, top, left + width, top + height))

    print(f"\n--- quads: lime_vulkan_batch_quads_d + canvas view matrix (client area {width}x{height})")

    failures = 0

    for label, point, expected, tolerance in SAMPLES:
        x = min(width - 1, max(0, point[0]))
        y = min(height - 1, max(0, point[1]))
        actual = client.getpixel((x, y))

        delta = max(abs(actual[i] - expected[i]) for i in range(3))
        ok = delta <= tolerance

        if not ok:
            failures += 1

        print(f"  {'ok  ' if ok else 'FAIL'} {label:18s} at ({x:4d},{y:4d})"
              f" expected {expected} actual {actual} (delta {delta})")

    if failures:
        print(f"\n{failures} of {len(SAMPLES)} quad batch check(s) failed")
        return 1

    print(f"\nall {len(SAMPLES)} quad batch checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

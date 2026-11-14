#!/usr/bin/env python3
"""Pixel level check of the 2D batch.

Why 'looks the same' is not enough: a wrong colour, a wrong projection, a wrong
blend mode and a scissor that does nothing all still look plausible in a
screenshot. So the scene draws colours with known values and the test asserts
numbers at known coordinates.

Flow:
    1. tools/capture-demo.ps1 -DemoArguments '--scene=test' takes a screenshot
       (including the client area geometry)
    2. this script crops the client area, samples the four bands and compares
       them against the expected values

Usage:
    python tests/test_batch_pixels.py logs/screenshots/test.png
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# The expected values are exactly what the shader writes:
#   vertex colour = texture(white) * tint, NORMAL is src*a + dst*(1-a), over black.
EXPECTED = [
    # (name,        x fraction, y fraction, expected RGB, tolerance)
    ("normal-opaque", 0.125, 0.125, (255, 0, 0), 4),
    ("normal-alpha", 0.125, 0.375, (0, 128, 0), 6),
    ("scissor-out", 0.125, 0.625, (0, 0, 0), 4),
    ("scissor-in", 0.500, 0.625, (0, 0, 255), 4),
    ("additive", 0.125, 0.875, (64, 64, 64), 8),
]


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: test_batch_pixels.py <screenshot.png>", file=sys.stderr)
        return 2

    from PIL import Image

    path = Path(argv[0])
    if not path.exists():
        print(f"missing {path} (run tools/capture-demo.ps1 first)", file=sys.stderr)
        return 2

    sidecar = path.with_suffix(".json")
    if not sidecar.exists():
        print(f"missing {sidecar}: the capture script must write client-area info", file=sys.stderr)
        return 2

    info = json.loads(sidecar.read_text(encoding="utf-8-sig"))
    image = Image.open(path).convert("RGB")

    left = int(info["clientX"])
    top = int(info["clientY"])
    width = int(info["clientWidth"])
    height = int(info["clientHeight"])

    client = image.crop((left, top, left + width, top + height))

    failures = 0
    print(f"client area {width}x{height} (from {path.name})")

    for name, fx, fy, expected, tolerance in EXPECTED:
        x = min(width - 1, max(0, int(width * fx)))
        y = min(height - 1, max(0, int(height * fy)))
        actual = client.getpixel((x, y))

        delta = max(abs(actual[i] - expected[i]) for i in range(3))
        ok = delta <= tolerance

        if not ok:
            failures += 1

        print(f"  {'ok  ' if ok else 'FAIL'} {name:16s} at ({x:4d},{y:4d}) expected {expected} actual {actual} (delta {delta})")

    if failures:
        print(f"\n{failures} case(s) failed")
        return 1

    print("\nall pixel checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

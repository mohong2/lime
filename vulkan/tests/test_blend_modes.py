#!/usr/bin/env python3
"""Pixel level check of the blend modes the fixed function unit can express.

Why this exists: subtract / darken / lighten are done purely by VkBlendOp and
blend factors, so a wrong op or a swapped source/destination still renders
"something" and looks plausible. Each band therefore paints a colour whose
result over a mid grey clear is a single predictable number.

Flow:
    pwsh -File tools/capture-demo.ps1 -DemoArguments --scene=blend -Out <png>
    python tests/test_blend_modes.py <png>
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# Clear colour is 0.5 grey, which the 8 bit target rounds to 127.
GREY = 127

# (name, band index, expected RGB, tolerance)
EXPECTED = [
    # darken red over grey: min(255, GREY), min(0, GREY)
    ("darken-red", 0, (GREY, 0, 0), 4),
    # lighten red over grey: max(255, GREY), max(0, GREY)
    ("lighten-red", 1, (255, GREY, GREY), 4),
    # subtract red from grey: dst - src, clamped at 0
    ("subtract-red", 2, (0, GREY, GREY), 4),
    # darken with black: min(0, GREY) everywhere
    ("darken-black", 3, (0, 0, 0), 4),
    # a white colour offset on a fully transparent texture must leave the clear
    # colour alone: the offset is additive, and adding it to a premultiplied
    # texel that has no alpha turned every tinted sprite into a solid block
    ("offset-transparent", 4, (GREY, GREY, GREY), 4),
    # the same offset on an opaque white texel still has to subtract fully
    ("offset-black", 5, (0, 0, 0), 4),
]


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: test_blend_modes.py <screenshot.png>", file=sys.stderr)
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
    band = height // 6

    failures = 0
    print(f"client area {width}x{height} (from {path.name})")

    for name, index, expected, tolerance in EXPECTED:
        x = width // 2
        y = min(height - 1, band * index + band // 2)
        actual = client.getpixel((x, y))

        delta = max(abs(actual[i] - expected[i]) for i in range(3))
        ok = delta <= tolerance

        if not ok:
            failures += 1

        print(f"  {'ok  ' if ok else 'FAIL'} {name:16s} at ({x:4d},{y:4d}) expected {expected} actual {actual} (delta {delta})")

    if failures:
        print(f"\n{failures} case(s) failed")
        return 1

    print("\nall blend mode checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

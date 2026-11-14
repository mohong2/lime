#!/usr/bin/env python3
"""Pixel level check of the shader pipeline.

Why looking right is not enough: the shader path has plenty of silent failure
modes, a wrong uniform offset after translation, a missing std140 alignment
step, a varying location that does not line up, vertex colour treated as
premultiplied or as straight, a blend mode that picks the wrong table. All of
them look fine in a screenshot.

So every scene uses a mathematically predictable shader on a four quadrant
texture (red top left, green top right, blue bottom left, white bottom right)
and asserts concrete numbers at concrete coordinates.

The flow (screenshots included, one command):
    python tests/test_shader_pixels.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEMO = ROOT / "bin" / "lime-vulkan-demo.exe"
CAPTURE = ROOT / "tools" / "capture-demo.ps1"
OUT_DIR = ROOT / "build" / "pixels"
CACHE = ROOT / "build" / "shadercache"

# Where the quad sits in the client area (matches --quad=), the centres of the four quadrants.
QUAD = (64, 64, 192, 192)
RED = (112, 112)
GREEN = (208, 112)
BLUE = (112, 208)
WHITE = (208, 208)
OUTSIDE = (24, 24)

BLACK = (0, 0, 0)
MAGENTA = (255, 0, 255)


def shader(name: str) -> str:
    return f"tests/shaders/{name}"


SCENARIOS: list[dict] = [
    {
        "name": "sprite",
        "why": "flixel default fragment: texture * vertex colour, which with a white colour and alpha 1 must pass through",
        "args": f"--scene=shader --vert={shader('sprite.vert')} --frag={shader('sprite.frag')}"
                " --quad=64,64,192,192",
        "samples": [
            ("red-quadrant", RED, (255, 0, 0), 4),
            ("green-quadrant", GREEN, (0, 255, 0), 4),
            ("blue-quadrant", BLUE, (0, 0, 255), 4),
            ("white-quadrant", WHITE, (255, 255, 255), 4),
            ("outside", OUTSIDE, BLACK, 3),
        ],
    },
    {
        "name": "sprite-tint",
        "why": "vertex colour (1,0,0) with alpha 128: the colour transform path has to work",
        "args": f"--scene=shader --vert={shader('sprite.vert')} --frag={shader('sprite.frag')}"
                " --quad=64,64,192,192 --tint=0x80FF0000",
        "samples": [
            ("red-tinted", RED, (128, 0, 0), 6),
            ("green-killed", GREEN, (0, 0, 0), 4),
            ("blue-killed", BLUE, (0, 0, 0), 4),
            ("white-tinted", WHITE, (128, 0, 0), 6),
        ],
    },
    {
        "name": "swap",
        "why": "ColorSwap: only pixels close to uColor1 are replaced by uColor2",
        "args": f"--scene=shader --vert={shader('sprite.vert')} --frag={shader('swap.frag')}"
                " --quad=64,64,192,192 --uniform=uColor1=1,0,0"
                " --uniform=uColor2=1,0,1 --uniform=uThreshold=0.1",
        "samples": [
            ("red-swapped", RED, MAGENTA, 4),
            ("green-kept", GREEN, (0, 255, 0), 4),
            ("blue-kept", BLUE, (0, 0, 255), 4),
            ("white-kept", WHITE, (255, 255, 255), 4),
        ],
    },
    {
        "name": "palette",
        "why": "RGBPalette: three mask channels tint separately, and white (all three channels) must come out black",
        "args": f"--scene=shader --vert={shader('sprite.vert')} --frag={shader('palette.frag')}"
                " --quad=64,64,192,192 --uniform=r=1,0,0 --uniform=g=0,1,0"
                " --uniform=b=0,0,1 --uniform=mult=1",
        "samples": [
            ("red-mask", RED, (255, 0, 0), 4),
            ("green-mask", GREEN, (0, 255, 0), 4),
            ("blue-mask", BLUE, (0, 0, 255), 4),
            ("white-all-masks", WHITE, BLACK, 4),
        ],
    },
    {
        "name": "wiggle",
        "why": "WiggleEffect with zero displacement must look identical to no effect (the vertex path is not skewed)",
        "args": f"--scene=shader --vert={shader('wiggle.vert')} --frag={shader('sprite.frag')}"
                " --quad=64,64,192,192 --uniform=uOffset=0,0"
                " --uniform=uAmplitude=0 --uniform=uFrequency=0.05",
        "samples": [
            ("red-quadrant", RED, (255, 0, 0), 4),
            ("blue-quadrant", BLUE, (0, 0, 255), 4),
            ("outside", OUTSIDE, BLACK, 3),
        ],
    },
    {
        "name": "wiggle-shift",
        "why": "vertex displacement of -16 pixels: the quad moves left and everything past its right edge must be black",
        "args": f"--scene=shader --vert={shader('wiggle.vert')} --frag={shader('sprite.frag')}"
                " --quad=64,64,192,192 --uniform=uOffset=-16,0"
                " --uniform=uAmplitude=0 --uniform=uFrequency=0.05",
        "samples": [
            ("shifted-red", (96, 112), (255, 0, 0), 4),
            ("newly-covered", (56, 112), (255, 0, 0), 4),
            ("now-empty-right", (248, 112), BLACK, 4),
        ],
    },
    {
        "name": "warp",
        "why": "fbm shader: loops and array uniforms, the pipeline has to build and really produce noise",
        "args": f"--scene=shader --vert={shader('sprite.vert')} --frag={shader('warp.frag')}"
                " --quad=64,64,192,192 --uniform=uArray=0.6,0.4,0.3,0.2",
        "nonBlack": 0.05,
    },
]


def capture(name: str, arguments: str) -> tuple[Path, dict]:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shot = OUT_DIR / f"{name}.png"

    # Automated runs must never show a modal dialog: it parks the process and keeps the exe locked for the next link.
    command = [
        "pwsh", "-NoProfile", "-File", str(CAPTURE),
        "-DemoArguments", arguments + " --no-dialog",
        "-Out", str(shot),
        "-DelayMs", "1200",
    ]

    result = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True)

    if result.returncode != 0:
        raise RuntimeError(f"capture failed for {name}:\n{result.stdout}\n{result.stderr}")

    sidecar = shot.with_suffix(".json")

    if not sidecar.exists():
        raise RuntimeError(f"capture did not write {sidecar}")

    return shot, json.loads(sidecar.read_text(encoding="utf-8-sig"))


def main(argv: list[str]) -> int:
    from PIL import Image

    only = argv[0] if argv else None

    failures = 0
    total = 0

    for scenario in SCENARIOS:
        name = scenario["name"]

        if only and only != name:
            continue

        print(f"\n--- {name}: {scenario['why']}")

        try:
            shot, info = capture(name, scenario["args"])
        except RuntimeError as error:
            print(f"  FAIL {error}")
            failures += 1
            continue

        image = Image.open(shot).convert("RGB")

        left = int(info["clientX"])
        top = int(info["clientY"])
        width = int(info["clientWidth"])
        height = int(info["clientHeight"])

        client = image.crop((left, top, left + width, top + height))

        for label, point, expected, tolerance in scenario.get("samples", []):
            total += 1

            x = min(width - 1, max(0, point[0]))
            y = min(height - 1, max(0, point[1]))
            actual = client.getpixel((x, y))

            delta = max(abs(actual[i] - expected[i]) for i in range(3))
            ok = delta <= tolerance

            if not ok:
                failures += 1

            print(f"  {'ok  ' if ok else 'FAIL'} {label:18s} at ({x:4d},{y:4d})"
                  f" expected {expected} actual {actual} (delta {delta})")

        if "nonBlack" in scenario:
            total += 1

            quad = client.crop((QUAD[0], QUAD[1], QUAD[0] + QUAD[2], QUAD[1] + QUAD[3]))
            pixels = list(quad.getdata())
            lit = sum(1 for pixel in pixels if max(pixel) > 8)
            ratio = lit / max(1, len(pixels))

            ok = ratio >= scenario["nonBlack"]

            if not ok:
                failures += 1

            print(f"  {'ok  ' if ok else 'FAIL'} {'lit-pixels':18s} {ratio * 100:5.1f}%"
                  f" of the quad is not black (need >= {scenario['nonBlack'] * 100:.1f}%)")

    if total == 0:
        print("no scenario matched", file=sys.stderr)
        return 2

    if failures:
        print(f"\n{failures} of {total} check(s) failed")
        return 1

    print(f"\nall {total} shader pixel checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

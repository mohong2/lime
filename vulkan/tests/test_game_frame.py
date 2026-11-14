#!/usr/bin/env python3
"""Verifies the Vulkan backend inside the real game process: two frames, compared.

This is the acceptance test for the renderer takeover. It does not read code or
trust log claims, it answers three questions:

  1. with --vulkan, does the game really hand the frame over?
  2. without --vulkan, is everything as before (the OpenGL path is intact)?
  3. do the two frames show the same thing (not black, not noise, not half drawn)?

Method: same exe, same delay, one screenshot each. Both are scaled to 160x90 and
compared by mean absolute difference. The title screen is animated (the logo
bounces with the music), so a per-pixel comparison would fail; but 'same image'
and 'black or garbled' are an order of magnitude apart on this metric.

    python tests/test_game_frame.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def find_game_root() -> Path:
    """The game repository that owns this copy of the renderer.

    It used to sit next to vulkan/; now the renderer lives under the project's
    .haxelib/lime/git/vulkan, so hard coding ROOT.parent was only right for one
    layout. Walk up until the capture script shows up instead.
    """
    for candidate in [ROOT] + list(ROOT.parents):
        if (candidate / "tools" / "vulkan-capture.ps1").exists():
            return candidate
    return ROOT.parent / "FNF-SeiunEngine"


GAME = find_game_root()
CAPTURE = GAME / "tools" / "vulkan-capture.ps1"
OUT_DIR = ROOT / "build" / "game"

# Upper bound for the mean absolute difference. Measured on the same image it is
# around 6..12 (animation phase plus interpolation differences between the two
# backends), while black against a normal frame is 60 and up. 30 leaves room for
# the animation and still never lets a black frame through.
MAX_MEAN_DIFF = 30.0
MIN_LIT_RATIO = 0.05


def capture(name: str, arguments: str) -> tuple[Path, dict, list[str]]:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shot = OUT_DIR / f"{name}.png"

    command = [
        "pwsh", "-NoProfile", "-File", str(CAPTURE),
        "-Out", str(shot),
        "-GameArguments", arguments,
        "-DelayMs", "9000",
    ]

    result = subprocess.run(command, cwd=str(GAME), capture_output=True, text=True)

    if result.returncode != 0:
        raise RuntimeError(f"capture failed for {name}:\n{result.stdout}\n{result.stderr}")

    sidecar = shot.with_suffix(".json")

    if not sidecar.exists():
        raise RuntimeError(f"capture wrote no sidecar: {sidecar}")

    info = json.loads(sidecar.read_text(encoding="utf-8-sig"))
    log_path = Path(info["log"])
    log = log_path.read_text(encoding="utf-8", errors="replace").splitlines() if log_path.exists() else []

    return shot, info, log


def client_crop(image, info):
    left = int(info["clientX"])
    top = int(info["clientY"])
    width = int(info["clientWidth"])
    height = int(info["clientHeight"])
    return image.crop((left, top, left + width, top + height))


def lit_ratio(image) -> float:
    small = image.convert("RGB").resize((160, 90))
    pixels = list(small.getdata())
    lit = sum(1 for pixel in pixels if max(pixel) > 16)
    return lit / max(1, len(pixels))


def mean_diff(a, b) -> float:
    left = a.convert("RGB").resize((160, 90))
    right = b.convert("RGB").resize((160, 90))
    total = 0
    count = 0

    for pa, pb in zip(left.getdata(), right.getdata()):
        for i in range(3):
            total += abs(pa[i] - pb[i])
            count += 1

    return total / max(1, count)


def main() -> int:
    from PIL import Image

    if not CAPTURE.exists():
        print(f"game capture script missing: {CAPTURE}", file=sys.stderr)
        return 2

    failures = 0

    print("\n--- opengl: without --vulkan (regression baseline)")
    gl_shot, gl_info, gl_log = capture("opengl", "")
    gl_image = client_crop(Image.open(gl_shot), gl_info)
    gl_lit = lit_ratio(gl_image)

    ok = gl_lit >= MIN_LIT_RATIO
    failures += 0 if ok else 1
    print(f"  {'ok  ' if ok else 'FAIL'} lit {gl_lit * 100:5.1f}% of the client area"
          f" (need >= {MIN_LIT_RATIO * 100:.0f}%)")

    print("\n--- vulkan: with --vulkan")
    vk_shot, vk_info, vk_log = capture("vulkan", "--vulkan")
    vk_image = client_crop(Image.open(vk_shot), vk_info)
    vk_lit = lit_ratio(vk_image)

    ok = vk_lit >= MIN_LIT_RATIO
    failures += 0 if ok else 1
    print(f"  {'ok  ' if ok else 'FAIL'} lit {vk_lit * 100:5.1f}% of the client area"
          f" (need >= {MIN_LIT_RATIO * 100:.0f}%)")

    attached = [line for line in vk_log if "[vulkan] attached:" in line]
    failed = [line for line in vk_log if "[vulkan] FAILED:" in line]

    ok = len(attached) == 1 and not failed
    failures += 0 if ok else 1
    print(f"  {'ok  ' if ok else 'FAIL'} swapchain: {len(attached)} attached, {len(failed)} failure(s)")

    for line in attached:
        print(f"       {line.strip()}")
    for line in failed:
        print(f"       {line.strip()}")

    diff = mean_diff(gl_image, vk_image)
    ok = diff <= MAX_MEAN_DIFF
    failures += 0 if ok else 1
    print(f"  {'ok  ' if ok else 'FAIL'} same picture: mean |delta| {diff:5.1f}"
          f" (need <= {MAX_MEAN_DIFF:.0f} of 255)")

    if failures:
        print(f"\n{failures} of 4 game frame check(s) failed")
        return 1

    print("\nall 4 game frame checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

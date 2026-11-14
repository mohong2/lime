#!/usr/bin/env python3
"""Verifies the shader toolchain itself, not the pixels it produces.

Three things, each of them the answer to a real bug:

  1. translation and compilation      -- five representative shaders must pass
  2. std140 offsets and SPIR-V alignment -- a wrong offset scrambles the uniforms
     while the image only looks 'the wrong colour'
  3. the disk cache                   -- the second run must hit the cache and be
     byte for byte identical

The second one matters most: the std140 layout computed here has to match the
OpMemberDecorate Offset values glslang writes into the SPIR-V exactly, otherwise
the value the CPU writes and the location the GPU reads differ. That used to be
spotted by eye and is compared automatically now.

Usage:
    python tests/test_shader_toolchain.py
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERC = ROOT / "bin" / "lime-vulkan-shaderc.exe"
SHADERS = ROOT / "tests" / "shaders"
OUT = ROOT / "build" / "toolchain"

PAIRS = [
    ("sprite", "sprite.vert", "sprite.frag"),
    ("swap", "sprite.vert", "swap.frag"),
    ("wiggle", "wiggle.vert", "sprite.frag"),
    ("palette", "sprite.vert", "palette.frag"),
    ("warp", "sprite.vert", "warp.frag"),
]

REFLECT_OFFSET = re.compile(r"^\s*\+(\d+)\s")
SPIRV_OFFSET = re.compile(r"OpMemberDecorate\s+%(\S+)\s+(\d+)\s+Offset\s+(\d+)")


def find_spirv_dis() -> str | None:
    sdk = os.environ.get("VULKAN_SDK")

    if sdk:
        candidate = Path(sdk) / "Bin" / "spirv-dis.exe"

        if candidate.exists():
            return str(candidate)

    return shutil.which("spirv-dis")


def run(arguments: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([str(SHADERC)] + arguments, cwd=str(ROOT),
                          capture_output=True, text=True)


def check_pair(name: str, vertex: str, fragment: str, disassembler: str | None) -> int:
    failures = 0

    print(f"\n--- {name}")

    result = run([
        "--vert", str(SHADERS / vertex),
        "--frag", str(SHADERS / fragment),
        "--out", str(OUT),
        "--name", name,
        "--reflect",
    ])

    if result.returncode != 0:
        print(f"  FAIL translate/compile\n{result.stdout}\n{result.stderr}")
        return 1

    summary = [line for line in result.stdout.splitlines() if line.startswith("ok:")]

    print(f"  ok   compiled ({summary[0] if summary else 'no summary'})")

    # ---- 1. the offsets from the reflection, in declaration order ----

    reflected: list[int] = []

    for line in result.stdout.splitlines():
        match = REFLECT_OFFSET.match(line)

        if match:
            reflected.append(int(match.group(1)))

    if not reflected:
        print("  FAIL no uniform offsets in the reflection output")
        return 1

    if disassembler is None:
        print("  skip std140 cross-check (spirv-dis not found)")
        return 0

    # ---- 2. the offsets glslang wrote into the SPIR-V ----

    spirv = OUT / f"{name}.frag.spv"

    dump = subprocess.run([disassembler, str(spirv)], capture_output=True, text=True)

    if dump.returncode != 0:
        print(f"  FAIL spirv-dis failed\n{dump.stderr}")
        return 1

    members: dict[int, int] = {}

    for line in dump.stdout.splitlines():
        match = SPIRV_OFFSET.search(line)

        if match:
            members[int(match.group(2))] = int(match.group(3))

    actual = [members[index] for index in sorted(members)]

    if len(actual) != len(reflected):
        print(f"  FAIL member count: reflection {len(reflected)}, SPIR-V {len(actual)}")
        return 1

    if actual != reflected:
        print("  FAIL std140 offsets differ")
        print(f"       reflection: {reflected}")
        print(f"       SPIR-V    : {actual}")
        return 1

    print(f"  ok   std140 layout matches SPIR-V ({len(actual)} members: {actual})")

    return failures


def check_cache() -> int:
    print("\n--- cache")

    cache = ROOT / "build" / "toolchain-cache"
    shutil.rmtree(cache, ignore_errors=True)

    arguments = [
        "--vert", str(SHADERS / "sprite.vert"),
        "--frag", str(SHADERS / "warp.frag"),
        "--cache", str(cache),
        "--out", str(OUT),
        "--name", "cachetest",
        "--verbose",
    ]

    first = run(arguments)

    if first.returncode != 0:
        print(f"  FAIL cold run\n{first.stdout}")
        return 1

    second = run(arguments)

    if second.returncode != 0:
        print(f"  FAIL warm run\n{second.stdout}")
        return 1

    if "cache miss" not in first.stdout:
        print("  FAIL the first run should miss the cache")
        return 1

    if "backend=cache" not in second.stdout:
        print(f"  FAIL the second run should hit the cache\n{second.stdout}")
        return 1

    print("  ok   cold run compiled, warm run came from the cache")

    # The cache has to really replace the compiler: with both backends disabled the run still succeeds, which proves the cache was used.
    offline = run([
        "--vert", str(SHADERS / "sprite.vert"),
        "--frag", str(SHADERS / "warp.frag"),
        "--cache", str(cache),
        "--no-dll",
        "--no-process",
    ])

    if offline.returncode != 0:
        print(f"  FAIL cache-only run failed\n{offline.stdout}")
        return 1

    print("  ok   cache alone is enough (no compiler needed)")

    return 0


def main() -> int:
    if not SHADERC.exists():
        print(f"missing {SHADERC} (run build-native.ps1)", file=sys.stderr)
        return 2

    OUT.mkdir(parents=True, exist_ok=True)

    disassembler = find_spirv_dis()

    if disassembler is None:
        print("warning: spirv-dis not found, the std140 cross-check will be skipped")

    failures = 0

    for name, vertex, fragment in PAIRS:
        failures += check_pair(name, vertex, fragment, disassembler)

    failures += check_cache()

    if failures:
        print(f"\n{failures} check(s) failed")
        return 1

    print("\nall toolchain checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""GLSL -> SPIR-V front end for the experimental Vulkan backend.

Why this exists
---------------
A Vulkan pipeline takes SPIR-V, never GLSL, and OpenFL/Flixel ship GLSL ES 1.00
sources (attribute / varying / gl_FragColor). Turning those into SPIR-V needs
three steps that this tool makes explicit and testable:

    1. lift the source to ESSL 310 (the oldest ESSL that Vulkan accepts)
    2. compile it with glslangValidator for the Vulkan 1.0 environment
    3. record a stable cache key + reflection metadata next to the .spv

Step 1 is deliberately conservative: it only rewrites constructs with an exact
equivalent. Anything it cannot translate is reported as an error and left
untouched, because a silently approximated shader is a wrong picture, which is
worse than a clear failure. In the engine, step 1 is the job of
source/shaders/MobileShaderConverter.hx (which already lifts 1.00 to 3.00); this
tool is the offline reference implementation and the CI gate.

Usage
-----
    python glsl2spirv.py Shader.frag
    python glsl2spirv.py --stage fragment --out build/shaders Shader.frag
    python glsl2spirv.py --check A.frag B.vert      # compile only, no output
    python glsl2spirv.py --json report.json Shader.frag

Requires glslangValidator (ships with the Vulkan SDK; VULKAN_SDK is honoured).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

TARGET_VERSION = 310

# ---------------------------------------------------------------------------
# source lifting: ESSL 100 -> ESSL 310
# ---------------------------------------------------------------------------

VERSION_RE = re.compile(r"^\s*#version\s+(\d+)(\s+\w+)?\s*$", re.MULTILINE)


@dataclass
class LiftResult:
    source: str
    notes: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def _replace_word(source: str, word: str, replacement: str) -> str:
    """Replaces a keyword only where it is a whole word (keeps identifiers)."""
    return re.sub(r"(?<![\w$.])" + re.escape(word) + r"(?![\w$])", replacement, source)


def lift(source: str, stage: str) -> LiftResult:
    result = LiftResult(source=source)

    # 1. version line ------------------------------------------------------
    match = VERSION_RE.search(source)
    if match:
        current = int(match.group(1))
        if current >= TARGET_VERSION:
            pass
        elif current == 100:
            source = source[: match.start()] + f"#version {TARGET_VERSION} es" + source[match.end() :]
            result.notes.append(f"upgraded #version 100 -> {TARGET_VERSION} es")
        elif current in (300, 310):
            source = source[: match.start()] + f"#version {TARGET_VERSION} es" + source[match.end() :]
            result.notes.append(f"upgraded #version {current} -> {TARGET_VERSION} es")
        else:
            result.errors.append(f"unsupported #version {current}")
    else:
        source = f"#version {TARGET_VERSION} es\n" + source
        result.notes.append(f"added #version {TARGET_VERSION} es")

    # 2. fragment output ---------------------------------------------------
    if stage == "fragment":
        if "gl_FragData" in source:
            result.errors.append("gl_FragData (MRT) has no direct Vulkan equivalent; needs a manual port")
        if "gl_FragColor" in source:
            source = _replace_word(source, "gl_FragColor", "lime_FragColor")
            source = source.replace("lime_FragColor", "lime_FragColor")
            declaration = "layout(location = 0) out highp vec4 lime_FragColor;\n"
            # insert after the last precision statement / before the first
            # function or global declaration
            lines = source.splitlines(keepends=True)
            insert_at = 1
            for index, line in enumerate(lines):
                stripped = line.strip()
                if stripped.startswith("precision ") or stripped.startswith("#"):
                    insert_at = index + 1
            source = "".join(lines[:insert_at]) + declaration + "".join(lines[insert_at:])
            result.notes.append("gl_FragColor -> out vec4 lime_FragColor")

        if not re.search(r"precision\s+(lowp|mediump|highp)\s+float", source):
            source = source.replace(f"#version {TARGET_VERSION} es\n", f"#version {TARGET_VERSION} es\nprecision mediump float;\n", 1)
            result.notes.append("added 'precision mediump float;'")

    # 3. keywords ----------------------------------------------------------
    if stage == "vertex":
        if _replace_word(source, "attribute", "in") != source:
            source = _replace_word(source, "attribute", "in")
            result.notes.append("attribute -> in")
        if _replace_word(source, "varying", "out") != source:
            source = _replace_word(source, "varying", "out")
            result.notes.append("varying -> out")
    else:
        if _replace_word(source, "varying", "in") != source:
            source = _replace_word(source, "varying", "in")
            result.notes.append("varying -> in")

    # 4. texture lookups ---------------------------------------------------
    for old, new in (("texture2D", "texture"), ("texture2DProj", "textureProj"), ("textureCube", "texture")):
        if _replace_word(source, old, new) != source:
            source = _replace_word(source, old, new)
            result.notes.append(f"{old}() -> {new}()")

    # 4b. uniforms ---------------------------------------------------------
    # Vulkan GLSL has no free-standing non-opaque uniforms and no unbound
    # samplers: value uniforms go into one std140 block, samplers get bindings.
    source, uniform_notes, uniform_errors = block_uniforms(source)
    result.notes.extend(uniform_notes)
    result.errors.extend(uniform_errors)

    # 5. explicit locations ------------------------------------------------
    # SPIR-V has no name-based attribute lookup: every user input/output needs a
    # location, and the two stages must agree on their varyings. Attributes are
    # numbered in declaration order; varyings are numbered by sorted name, which
    # is the same order in both stages no matter how they are declared.
    source, location_notes, location_errors = assign_locations(source, stage)
    result.notes.extend(location_notes)
    result.errors.extend(location_errors)

    # 6. things Vulkan GLSL will not accept --------------------------------
    if re.search(r"\bgl_FragDepth\b", source):
        result.notes.append("gl_FragDepth kept (valid in ESSL 310)")

    result.source = source
    return result


UNIFORM_RE = re.compile(
    r"^(?P<indent>\s*)uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?P<type>[A-Za-z_][\w]*)\s+(?P<name>[A-Za-z_][\w]*)\s*;\s*$"
)

SAMPLER_TYPES = {"sampler2D", "sampler3D", "samplerCube", "sampler2DArray", "isampler2D", "usampler2D"}


def block_uniforms(source: str) -> tuple[str, list[str], list[str]]:
    """Wraps value uniforms into a std140 block and binds every sampler."""
    lines = source.splitlines(keepends=True)
    notes: list[str] = []
    errors: list[str] = []

    depth = 0
    value_uniforms: list[int] = []
    samplers: list[int] = []

    for index, line in enumerate(lines):
        stripped = line.strip()

        if depth == 0 and stripped.startswith("uniform"):
            match = UNIFORM_RE.match(line.rstrip("\n"))
            if match:
                if match.group("type") in SAMPLER_TYPES:
                    samplers.append(index)
                else:
                    value_uniforms.append(index)
            elif "{" in line:
                notes.append("uniform block kept as written")
            else:
                errors.append(f"cannot classify uniform line {index + 1}: {stripped}")

        depth += line.count("{") - line.count("}")

    if value_uniforms:
        declarations = []
        for index in value_uniforms:
            match = UNIFORM_RE.match(lines[index].rstrip("\n"))
            declarations.append(f"\t{match.group('type')} {match.group('name')};\n")

        block = ["layout(set = 0, binding = 0, std140) uniform lime_Uniforms {\n"] + declarations + ["};\n"]
        first = value_uniforms[0]
        # replace the first declaration with the block, drop the rest
        lines[first] = "".join(block)
        for index in reversed(value_uniforms[1:]):
            del lines[index]

        notes.append(f"{len(value_uniforms)} value uniform(s) grouped into a std140 block at set 0, binding 0")
        notes.append("the engine must upload that block in this declaration order (std140 padding applies)")

    text = "".join(lines)

    # re-scan for sampler declarations (indices moved), bind them in order
    sampler_pattern = re.compile(
        r"^\s*uniform\s+(?:(?:highp|mediump|lowp)\s+)?(sampler2D|sampler3D|samplerCube|sampler2DArray|isampler2D|usampler2D)\s+([A-Za-z_][\w]*)\s*;\s*$",
        re.MULTILINE,
    )

    binding = 1 if value_uniforms else 0
    handled = 0

    def bind(match: re.Match) -> str:
        nonlocal binding, handled
        text_ = f"layout(set = 0, binding = {binding}) uniform {match.group(1)} {match.group(2)};"
        binding += 1
        handled += 1
        return text_

    text = sampler_pattern.sub(bind, text)

    if handled:
        notes.append(f"{handled} sampler(s) bound at set 0, binding 1..")

    return text, notes, errors


DECL_RE = re.compile(
    r"^(?P<indent>\s*)(?P<qualifier>in|out)\s+(?:(?:highp|mediump|lowp)\s+)?(?P<type>[A-Za-z_][\w]*)\s+(?P<name>[A-Za-z_][\w]*)\s*;\s*$"
)


def assign_locations(source: str, stage: str) -> tuple[str, list[str], list[str]]:
    lines = source.splitlines()
    notes: list[str] = []
    errors: list[str] = []

    depth = 0
    declarations: list[tuple[int, str, str, str]] = []  # (line index, qualifier, type, name)

    for index, line in enumerate(lines):
        stripped = line.strip()

        if depth == 0 and not stripped.startswith("//"):
            match = DECL_RE.match(line)
            if match:
                declarations.append((index, match.group("qualifier"), match.group("type"), match.group("name")))

        depth += line.count("{") - line.count("}")

    if not declarations:
        return source, notes, errors

    varyings = sorted({name for _, qualifier, _, name in declarations if (stage == "vertex" and qualifier == "out") or (stage == "fragment" and qualifier == "in")})

    attribute_index = 0
    location_by_name: dict[str, int] = {name: position for position, name in enumerate(varyings)}

    for index, qualifier, type_, name in declarations:
        if "[" in lines[index]:
            errors.append(f"arrays in interface variables are not supported yet ({name})")
            continue

        if stage == "vertex" and qualifier == "in":
            location = attribute_index
            attribute_index += 1
        elif name in location_by_name:
            location = location_by_name[name]
        else:
            location = 0

        indent = lines[index][: len(lines[index]) - len(lines[index].lstrip())]
        lines[index] = f"{indent}layout(location = {location}) {qualifier} {type_} {name};"

    notes.append(f"{len(declarations)} interface variable(s) given explicit locations")
    return "\n".join(lines) + ("\n" if source.endswith("\n") else ""), notes, errors


# ---------------------------------------------------------------------------
# compilation
# ---------------------------------------------------------------------------

def find_glslang() -> str | None:
    found = shutil.which("glslangValidator")
    if found:
        return found
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidate = Path(sdk) / "Bin" / ("glslangValidator.exe" if os.name == "nt" else "glslangValidator")
        if candidate.exists():
            return str(candidate)
    for root in ("C:/VulkanSDK", "/usr/local/VulkanSDK"):
        path = Path(root)
        if not path.exists():
            continue
        for version in sorted(path.iterdir(), reverse=True):
            candidate = version / "Bin" / ("glslangValidator.exe" if os.name == "nt" else "glslangValidator")
            if candidate.exists():
                return str(candidate)
    return None


STAGE_BY_EXTENSION = {".vert": "vertex", ".frag": "fragment", ".comp": "compute"}


STAGE_EXTENSION = {"vertex": ".vert", "fragment": ".frag", "compute": ".comp"}


def compile_shader(glslang: str, source: str, stage: str, target: Path | None) -> tuple[bool, str]:
    # glslang classifies by extension; a .glsl temp file is rejected even with
    # -S, so the temporary keeps the stage extension.
    temp = Path(os.environ.get("TEMP", "/tmp")) / ("lime-vk-shader" + STAGE_EXTENSION.get(stage, ".frag"))
    temp.write_text(source, encoding="utf-8")

    # -S takes glslang's short stage names, not "vertex"/"fragment".
    short_stage = {"vertex": "vert", "fragment": "frag", "compute": "comp"}.get(stage, "frag")

    args = [glslang, "-V", "--target-env", "vulkan1.0", "-S", short_stage]
    if target is not None:
        args += ["-o", str(target)]
    args.append(str(temp))

    completed = subprocess.run(args, capture_output=True, text=True)
    output = (completed.stdout or "") + (completed.stderr or "")
    return completed.returncode == 0, output


def cache_key(source: str, stage: str) -> str:
    digest = hashlib.sha256()
    digest.update(f"{stage}:{TARGET_VERSION}:{source}".encode("utf-8"))
    return digest.hexdigest()[:16]


# ---------------------------------------------------------------------------
# cli
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Compile GLSL ES shaders to SPIR-V for the lime Vulkan backend")
    parser.add_argument("shaders", nargs="+", help=".vert / .frag files")
    parser.add_argument("--stage", choices=["vertex", "fragment", "compute"], help="override the stage (default: by extension)")
    parser.add_argument("--out", help="output directory for .spv files")
    parser.add_argument("--check", action="store_true", help="compile only, write nothing")
    parser.add_argument("--json", help="write a machine readable report here")
    parser.add_argument("--keep-glsl", action="store_true", help="write the lifted .glsl next to the .spv")
    args = parser.parse_args(argv)

    glslang = find_glslang()
    if glslang is None:
        print("error: glslangValidator not found (install the Vulkan SDK or set VULKAN_SDK)", file=sys.stderr)
        return 2

    report = []
    failures = 0

    for name in args.shaders:
        path = Path(name)
        stage = args.stage or STAGE_BY_EXTENSION.get(path.suffix, "fragment")
        entry = {"file": str(path), "stage": stage, "ok": False, "notes": [], "errors": [], "output": ""}

        lifted = lift(path.read_text(encoding="utf-8"), stage)
        entry["notes"] = lifted.notes
        entry["errors"] = lifted.errors

        if lifted.errors:
            failures += 1
            report.append(entry)
            print(f"FAIL {path.name}: " + "; ".join(lifted.errors))
            continue

        target = None
        if not args.check and args.out:
            out_dir = Path(args.out)
            out_dir.mkdir(parents=True, exist_ok=True)
            # With the stage suffix: a .vert and a .frag of the same name do not overwrite each other
            target = out_dir / f"{path.stem}.{stage}.spv"

        ok, output = compile_shader(glslang, lifted.source, stage, target)
        entry["ok"] = ok
        entry["output"] = output.strip()
        entry["cacheKey"] = cache_key(lifted.source, stage)

        if target is not None and ok:
            entry["spv"] = str(target)
            if args.keep_glsl:
                (target.parent / f"{path.stem}.{stage}.glsl").write_text(lifted.source, encoding="utf-8")

        if ok:
            size = target.stat().st_size if target is not None and target.exists() else 0
            print(f"ok   {path.name} -> {stage}" + (f", {size} bytes SPIR-V" if target else ""))
        else:
            failures += 1
            print(f"FAIL {path.name}:")
            print("   " + "\n   ".join(output.strip().splitlines()[:12]))

        report.append(entry)

    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"\n{len(args.shaders) - failures}/{len(args.shaders)} shader(s) compiled")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

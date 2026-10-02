# libjpeg-turbo 3.0.4 SIMD kernels (`simd/`)

This directory holds the upstream `simd/` subtree of libjpeg-turbo 3.0.4, plus two
pre-assembled static libraries that the lime build links into `lime.ndll`.

## Why the kernels are pre-assembled

The lime native toolkit is compiled by hxcpp, which has **no assembler support**: it neither
compiles `.asm` sources nor accepts a prebuilt `.obj` as a `<file>` entry. The x86 SIMD kernels
are NASM assembly, so they cannot be built as part of the normal hxcpp run.

They are therefore assembled ahead of time with NASM into a static library, committed here, and
linked in by `project/Build.xml`:

    <lib name="${NATIVE_TOOLKIT_PATH}/jpeg/simd/jsimd_x86_64.lib" if="LIME_JPEG HXCPP_M64" />
    <lib name="${NATIVE_TOOLKIT_PATH}/jpeg/simd/jsimd_i386.lib"   if="LIME_JPEG HXCPP_M32" />

`jconfigint.h` defines `WITH_SIMD 1` only for `_WIN32 && (_M_X64 || _M_IX86)`, and
`files.xml` compiles the matching arch dispatcher (`simd/x86_64/jsimd.c` / `simd/i386/jsimd.c`)
only for `windows`. Every other target keeps the scalar C build unchanged.

## Contents

    jsimd.h                 upstream (declarations used by the arch dispatchers)
    CMakeLists.txt          upstream, kept for provenance / to review the source list
    nasm/                   upstream .inc files, incl. the pre-generated jsimdcfg.inc
    x86_64/                 upstream 35 .asm + jsimd.c dispatcher
    i386/                   upstream 56 .asm + jsimd.c dispatcher
    jsimd_x86_64.lib        27 objects  (committed build input)
    jsimd_i386.lib          44 objects  (committed build input)
    build-simd-libs.ps1     rebuilds the two .lib files

The `.lib` files are **checked in on purpose**: a fresh checkout needs them to link, and must
not need NASM to build. Only re-run `build-simd-libs.ps1` if the kernel sources change.

## Regenerating

    pwsh -File build-simd-libs.ps1 -Nasm C:\path\to\nasm.exe          # NASM 2.16.03 was used
    pwsh -File build-simd-libs.ps1 -Nasm C:\path\to\nasm.exe -Arch x86_64

The source list and flags inside the script are a verbatim copy of upstream
`simd/CMakeLists.txt` (`SIMD_SOURCES` lines 129-138 / 140-155; NASM flags lines 61-72 and 115):

    x86_64:  -f win64 -DWIN64 -D__x86_64__  -I<nasm/> -I<x86_64/>
    i386:    -f win32 -DWIN32                -I<nasm/> -I<i386/>

## Runtime dispatch

Kernel selection happens at **run time** via `jpeg_simd_cpu_support()` (`jsimdcpu.asm`), so one
`lime.ndll` uses SSE2, AVX2 or nothing depending on the CPU it lands on. Omitting the `.lib`
does not degrade gracefully: the link fails with unresolved `jsimd_*` externals.

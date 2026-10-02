# build-simd-libs.ps1
#
# Assemble the libjpeg-turbo 3.0.4 NASM SIMD kernels that live in this directory
# into the static libraries the lime build links:
#
#   jsimd_x86_64.lib   (27 objects, consumed by the x64 MSVC build)
#   jsimd_i386.lib     (44 objects, consumed by the x86 MSVC build)
#
# WHY THIS EXISTS
#   hxcpp, which compiles the whole lime native toolkit, has no assembler support
#   (no .asm handling, and it will not accept a prebuilt .obj as a <file> entry), so
#   the kernels cannot be assembled as part of the normal build.  They are therefore
#   assembled ahead of time into a static library, committed, and linked in via
#   <lib> in project/Build.xml.  jconfigint.h turns WITH_SIMD on only for MSVC
#   x86/x64, so no other target needs these.
#
#   The .lib is a build INPUT, not an artefact: a plain checkout needs it and needs
#   no NASM.  Only re-run this script if the kernel sources change.
#
# SOURCE LIST AND FLAGS
#   Copied verbatim from upstream simd/CMakeLists.txt (SIMD_SOURCES at lines 129-138
#   for x86_64 and 140-155 for i386; NASM flags at lines 61-72 and 115).  Upstream
#   builds exactly these files; simd/x86_64/ and simd/i386/ also contain the
#   jccolext/jcgryext/jdcolext/jdmrgext helpers, which are %include'd by those
#   sources and must NOT be assembled separately.
#
# USAGE
#   pwsh -File build-simd-libs.ps1 -Nasm C:\path\to\nasm.exe
#   pwsh -File build-simd-libs.ps1 -Nasm C:\path\to\nasm.exe -Arch x86_64
#
#   Nasm must be 2.13 or newer (upstream requirement); NASM 2.16.03 is what the
#   committed libraries were produced with.

param(
  [Parameter(Mandatory = $true)][string]$Nasm,
  [ValidateSet("x86_64", "i386", "both")][string]$Arch = "both"
)

$ErrorActionPreference = "Stop"

$Simd = $PSScriptRoot
if (!(Test-Path $Nasm)) { throw "NASM not found at $Nasm" }

# Intermediate objects go to a temp dir; only the final .lib is written back here.
$WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("jpeg-simd-build-" + [System.Guid]::NewGuid().ToString("N"))

# ---- exact upstream SIMD_SOURCES (simd/CMakeLists.txt) --------------------------
$SRC_X86_64 = @(
  "jsimdcpu.asm",
  "jfdctflt-sse.asm",
  "jccolor-sse2.asm", "jcgray-sse2.asm", "jchuff-sse2.asm", "jcphuff-sse2.asm",
  "jcsample-sse2.asm", "jdcolor-sse2.asm", "jdmerge-sse2.asm", "jdsample-sse2.asm",
  "jfdctfst-sse2.asm", "jfdctint-sse2.asm", "jidctflt-sse2.asm", "jidctfst-sse2.asm",
  "jidctint-sse2.asm", "jidctred-sse2.asm", "jquantf-sse2.asm", "jquanti-sse2.asm",
  "jccolor-avx2.asm", "jcgray-avx2.asm", "jcsample-avx2.asm", "jdcolor-avx2.asm",
  "jdmerge-avx2.asm", "jdsample-avx2.asm", "jfdctint-avx2.asm", "jidctint-avx2.asm",
  "jquanti-avx2.asm"
)

$SRC_I386 = @(
  "jsimdcpu.asm", "jfdctflt-3dn.asm", "jidctflt-3dn.asm", "jquant-3dn.asm",
  "jccolor-mmx.asm", "jcgray-mmx.asm", "jcsample-mmx.asm", "jdcolor-mmx.asm",
  "jdmerge-mmx.asm", "jdsample-mmx.asm", "jfdctfst-mmx.asm", "jfdctint-mmx.asm",
  "jidctfst-mmx.asm", "jidctint-mmx.asm", "jidctred-mmx.asm", "jquant-mmx.asm",
  "jfdctflt-sse.asm", "jidctflt-sse.asm", "jquant-sse.asm",
  "jccolor-sse2.asm", "jcgray-sse2.asm", "jchuff-sse2.asm", "jcphuff-sse2.asm",
  "jcsample-sse2.asm", "jdcolor-sse2.asm", "jdmerge-sse2.asm", "jdsample-sse2.asm",
  "jfdctfst-sse2.asm", "jfdctint-sse2.asm", "jidctflt-sse2.asm", "jidctfst-sse2.asm",
  "jidctint-sse2.asm", "jidctred-sse2.asm", "jquantf-sse2.asm", "jquanti-sse2.asm",
  "jccolor-avx2.asm", "jcgray-avx2.asm", "jcsample-avx2.asm", "jdcolor-avx2.asm",
  "jdmerge-avx2.asm", "jdsample-avx2.asm", "jfdctint-avx2.asm", "jidctint-avx2.asm",
  "jquanti-avx2.asm"
)

function Find-LibExe([string]$HostBin) {
  $roots = @(
    "${env:ProgramFiles}\Microsoft Visual Studio",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
  )
  foreach ($root in $roots) {
    if (!(Test-Path $root)) { continue }
    $hit = Get-ChildItem -Path $root -Filter "lib.exe" -Recurse -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -like "*\bin\$HostBin\lib.exe" } |
           Sort-Object FullName -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }
  }
  return $null
}

function Build-Arch([string]$A) {
  if ($A -eq "x86_64") {
    $sources = $SRC_X86_64
    $format  = "win64"
    $defines = @("-DWIN64", "-D__x86_64__")
    $hostBin = "Hostx64\x64"
  } else {
    $sources = $SRC_I386
    $format  = "win32"
    $defines = @("-DWIN32")
    $hostBin = "Hostx86\x86"
  }

  $srcDir  = Join-Path $Simd $A
  $nasmInc = Join-Path $Simd "nasm"
  $objDir  = Join-Path $WorkRoot ($A + "\obj")
  $libOut  = Join-Path $Simd ("jsimd_" + $A + ".lib")
  New-Item -ItemType Directory -Path $objDir -Force | Out-Null

  Write-Host ("[" + $A + "] assembling " + $sources.Count + " files ...")
  $failed = 0
  foreach ($s in $sources) {
    $src = Join-Path $srcDir $s
    if (!(Test-Path $src)) { Write-Host "  MISSING $s" -ForegroundColor Red; $failed++; continue }
    $obj = Join-Path $objDir ([System.IO.Path]::GetFileNameWithoutExtension($s) + ".obj")
    $nargs = @("-f$format") + $defines + @("-I$nasmInc\", "-I$srcDir\", "-o", $obj, $src)
    & $Nasm @nargs
    if ($LASTEXITCODE -ne 0) { Write-Host "  FAILED $s" -ForegroundColor Red; $failed++ }
  }
  if ($failed -ne 0) { throw ("[" + $A + "] " + $failed + " file(s) failed to assemble") }

  $objs = Get-ChildItem $objDir -Filter *.obj
  if ($objs.Count -ne $sources.Count) {
    throw ("[" + $A + "] expected " + $sources.Count + " objects, got " + $objs.Count)
  }

  $libExe = Find-LibExe $hostBin
  if (!$libExe) { throw "lib.exe not found for $hostBin - install the MSVC build tools" }
  if (Test-Path $libOut) { Remove-Item $libOut -Force }
  & $libExe /nologo /OUT:$libOut @($objs.FullName)
  if ($LASTEXITCODE -ne 0) { throw ("[" + $A + "] lib.exe failed (" + $LASTEXITCODE + ")") }

  $li = Get-Item $libOut
  Write-Host ("[" + $A + "] OK  " + $li.Name + "  " + $li.Length + " bytes  (" + $objs.Count + " objects)") -ForegroundColor Green
}

try {
  if ($Arch -eq "both") { Build-Arch "x86_64"; Build-Arch "i386" }
  else { Build-Arch $Arch }
} finally {
  if (Test-Path $WorkRoot) { Remove-Item $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue }
}

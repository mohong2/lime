# Builds the Vulkan backend as a standalone dll, for the pixel tests, the demo
# and the window attach tool. The game build does not use this: inside Lime the
# same sources are linked into Lime.ndll (see project/Build.xml).
#
#   bin/lime-vulkan.dll            the library (exports lime_vulkan_*)
#   bin/lime-vulkan.lib            import library
#   bin/lime-vulkan-demo.exe       standalone demo, plain Win32 window
#   bin/lime-vulkan-attach.exe     submits Vulkan frames into another process's window
#   bin/lime-vulkan-shaderc.exe    offline GLSL to SPIR-V front end
#
# Needs MSVC (2019 or newer) and python. The Vulkan headers are vendored under
# third_party/, so no Vulkan SDK is required.
#
# Usage: pwsh -File build-native.ps1 [-Targets dll|all]
param(
    # all = dll + demo + attach + shaderc, for development
    # dll = lime-vulkan.dll only, which is what the tests need
    [ValidateSet('all', 'dll')]
    [string]$Targets = 'all'
)

$ErrorActionPreference = 'Stop'

$root   = $PSScriptRoot
$src    = Join-Path $root 'native/src'
$inc    = Join-Path $root 'native/include'
$third  = Join-Path $root 'third_party'
$demo   = Join-Path $root 'native/demo'
$attach = Join-Path $root 'native/attach'
$out    = Join-Path $root 'bin'
$obj    = Join-Path $root 'build/obj'
$objDemo = Join-Path $obj 'demo'
$objAttach = Join-Path $obj 'attach'
$objShaderc = Join-Path $obj 'shaderc'

function Enter-Msvc {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $candidates = @(
        'C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Auxiliary/Build/vcvars64.bat',
        'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat',
        'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Auxiliary/Build/vcvars64.bat',
        'C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat',
        'C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Auxiliary/Build/vcvars64.bat'
    )

    $vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $vcvars) { throw 'vcvars64.bat not found: run this from an x64 Native Tools prompt' }

    Write-Host "[build] loading MSVC environment from $vcvars"
    $commandLine = '"' + $vcvars + '" >nul 2>&1 && set'
    cmd /c $commandLine | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ('Env:' + $matches[1]) -Value $matches[2] }
    }
}

Enter-Msvc

New-Item -ItemType Directory -Force -Path $out, $obj, $objDemo, $objAttach, $objShaderc, (Join-Path $root 'native/shaders/out') | Out-Null

Write-Host '[build] compiling shaders'
& python (Join-Path $root 'tools/glsl2spirv.py') --out (Join-Path $root 'native/shaders/out') (Join-Path $root 'native/shaders/triangle.vert') (Join-Path $root 'native/shaders/triangle.frag') (Join-Path $root 'native/shaders/quad.vert') (Join-Path $root 'native/shaders/quad.frag')
if ($LASTEXITCODE -ne 0) { throw 'glsl2spirv failed' }

$spirv = Join-Path $root 'native/shaders/out'

& python (Join-Path $root 'tools/spv2header.py') --out (Join-Path $src 'triangle_spirv.h') ('kTriangleVertexSpirv=' + (Join-Path $spirv 'triangle.vertex.spv')) ('kTriangleFragmentSpirv=' + (Join-Path $spirv 'triangle.fragment.spv'))
if ($LASTEXITCODE -ne 0) { throw 'spv2header failed (triangle)' }

& python (Join-Path $root 'tools/spv2header.py') --out (Join-Path $src 'quad_spirv.h') ('kQuadVertexSpirv=' + (Join-Path $spirv 'quad.vertex.spv')) ('kQuadFragmentSpirv=' + (Join-Path $spirv 'quad.fragment.spv'))
if ($LASTEXITCODE -ne 0) { throw 'spv2header failed (quad)' }

$common   = @('/nologo', '/c', '/EHsc', '/W3', '/MT', '/O2', '/DNDEBUG', '/D_CRT_SECURE_NO_WARNINGS')
$includes = @("/I$inc", "/I$src", "/I$third")

# haxe_api.cpp is not listed on purpose: it binds to the hxcpp runtime, which
# only exists inside Lime.ndll.
$sources = @('loader.cpp', 'device.cpp', 'surface_win32.cpp', 'window_win32.cpp', 'image.cpp', 'batch.cpp', 'glsl.cpp', 'compile.cpp', 'program.cpp', 'perf.cpp', 'api.cpp') | ForEach-Object { Join-Path $src $_ }

Remove-Item -Path (Join-Path $obj '*.obj') -Force -ErrorAction SilentlyContinue

Write-Host '[build] compiling native sources'
& cl @common @includes "/Fo$obj/" @sources
if ($LASTEXITCODE -ne 0) { throw 'native compile failed' }

$objects = Get-ChildItem -LiteralPath $obj -Filter '*.obj' | ForEach-Object { $_.FullName }

Write-Host '[build] linking lime-vulkan.dll'
& cl /nologo /LD /MT "/Fe$(Join-Path $out 'lime-vulkan.dll')" @objects user32.lib gdi32.lib
if ($LASTEXITCODE -ne 0) { throw 'dll link failed' }

if ($Targets -eq 'all') {
    Write-Host '[build] building demo'
    & cl @common @includes "/Fo$objDemo/" (Join-Path $demo 'main.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'demo compile failed' }

    & cl /nologo /MT "/Fe$(Join-Path $out 'lime-vulkan-demo.exe')" (Join-Path $objDemo 'main.obj') @objects user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'demo link failed' }

    Write-Host '[build] building attach tool'
    & cl @common @includes "/Fo$objAttach/" (Join-Path $attach 'main.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'attach compile failed' }

    & cl /nologo /MT "/Fe$(Join-Path $out 'lime-vulkan-attach.exe')" (Join-Path $objAttach 'main.obj') @objects user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'attach link failed' }

    Write-Host ''
    Write-Host '[build] building shader compiler cli'
    & cl @common @includes "/Fo$objShaderc/" (Join-Path $root 'native/shaderc/main.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'shaderc compile failed' }

    & cl /nologo /MT "/Fe$(Join-Path $out 'lime-vulkan-shaderc.exe')" (Join-Path $objShaderc 'main.obj') (Join-Path $obj 'glsl.obj') (Join-Path $obj 'compile.obj')
    if ($LASTEXITCODE -ne 0) { throw 'shaderc link failed' }
}

Write-Host '[build] ok:'
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
